/*
 * platform_thread_amiga.c — AmigaOS 3+ threading primitives for AmiLisp.
 *
 * Thread:   CreateNewProc() (dos.library)
 * Mutex:    struct SignalSemaphore (exec.library)
 * CondVar:  Signal/Wait + SignalSemaphore + waiter list
 * TLS:      FindTask(NULL)->tc_UserData
 * Atomics:  Forbid()/Permit() pairs (safe on 68020, short critical sections)
 */

#include "platform_thread.h"
#include "platform.h"

#include <proto/exec.h>
#include <proto/dos.h>
#include <exec/semaphores.h>
#include <exec/tasks.h>
#include <dos/dostags.h>
#include <string.h>

#include "platform_amiga_ppc.h"   /* NP_Entry gate for the MorphOS PPC build */

/* ================================================================
 * Thread
 *
 * We use CreateNewProc() which creates a DOS process (superset of
 * Exec Task).  The wrapper struct holds join synchronization state.
 * ================================================================ */

/* Join/exit protocol.  The old protocol had four corruption bugs
 * (GC/threading audit tier 1):
 *
 *  - amiga_thread_entry Signal()ed the parent through `at` AFTER
 *    publishing finished=1, so a joiner that observed the flag via the
 *    semaphore poll freed `at` (and FreeSignal'd the bit) while the
 *    child was still about to read at->parent / at->join_sig — a
 *    use-after-free, plus a stray Signal into whatever task later
 *    reallocated that bit.
 *  - join_sig was allocated in the CREATOR's task at create time.  A
 *    join from any other task Wait()ed on a bit that was never going
 *    to be signalled (hang), and its FreeSignal released a bit number
 *    it never owned in its own task, corrupting that task's signal
 *    allocation while leaking the creator's.
 *  - platform_thread_detach leaked the AmigaThread struct AND the
 *    creator's signal bit.  A task has ~16 free user signal bits, so a
 *    few detached threads exhausted AllocSignal — after which every
 *    platform_condvar_wait hit its sig<0 fallback (see condvar notes).
 *
 * New protocol: no signal bit at create time.  The JOINER allocates
 * its own bit and registers (task, bit) in `at` under Forbid(); the
 * child's exit path extracts them into locals under Forbid() — its
 * very last access to `at` — and Signal()s using only the locals.
 * On the single-core Amiga, Forbid() blocks preemption, so the joiner
 * cannot observe finished=1 (and free `at`) until the child Permit()s.
 * Detach hands `at` to whichever side finishes last. */

typedef struct AmigaThread {
    volatile int            finished;
    volatile int            detached;
    void                   *result;
    void                  *(*func)(void *);
    void                   *arg;
    struct Task            *joiner;    /* registered by platform_thread_join */
    BYTE                    join_sig;  /* signal bit in JOINER's task */
    struct Process         *proc;
    struct AmigaThread *live_next;  /* amiga_live_list link, Forbid()-guarded */
} AmigaThread;

/* Workers still executing amilisp code (see platform_thread.h).  Counted up
 * under the creator's Forbid() in platform_thread_create and down as the
 * very last thing amiga_thread_entry does — inside a Forbid() it never
 * leaves: the task falls off the end still forbidden, and Exec's task
 * removal ends the Forbid().  So once main observes the decrement, the
 * worker can never again run a single instruction of this program's code,
 * and main may return and have the seglist unloaded.  Before this, a worker
 * that had left the MP registry could still be in its exit tail (park
 * destroy, status store, the Forbid/Signal/return below) when main returned;
 * the unloaded code then crashed it — "68k exception at 0x4e" in CL-Thread
 * on MorphOS at the end of every few Clamacs drive runs. */
static volatile uint32_t amiga_live_threads = 0;
static volatile uint32_t amiga_exit_delay_ms = 0;
/* The same workers as a list, for platform_thread_remove_stragglers.  Linked
 * in and out only under Forbid(), exactly where amiga_live_threads moves. */
static AmigaThread *amiga_live_list = NULL;

static void live_list_unlink(AmigaThread *at)   /* caller holds Forbid() */
{
    AmigaThread **pp;
    for (pp = &amiga_live_list; *pp; pp = &(*pp)->live_next) {
        if (*pp == at) { *pp = at->live_next; return; }
    }
}

/* Entry point for new process — runs user function, signals joiner */
static void amiga_thread_entry(void)
{
    struct Task *me = FindTask(NULL);
    AmigaThread *at;
    struct Task *joiner_local;
    BYTE sig_local;
    int free_self;
    LONG spins = 0;

    /* FPU control registers are per-task context — a fresh process starts
     * with the hardware default (extended-precision rounding), not the
     * mode platform_init() set for the main task. */
    platform_fpu_setup();

    /* The parent stores tc_UserData under Forbid() — but CreateNewProc
     * can Wait() internally (DOS packet round-trip), and Wait() BREAKS a
     * Forbid(), so this child may start running before the parent's
     * store.  dos.library zeroes the new process's Task fields, so poll
     * until the parent's non-NULL store becomes visible.  Bounded (~60s
     * of Delay(1) ticks) so a genuinely broken state exits instead of
     * spinning a zombie process forever. */
    while (!(at = (AmigaThread *)me->tc_UserData)) {
        if (++spins > 3000) {
            AmigaThread *e;
            Forbid();              /* exit forbidden, see amiga_live_threads */
            for (e = amiga_live_list; e; e = e->live_next)
                if (e->proc == (struct Process *)me) { live_list_unlink(e); break; }
            amiga_live_threads--;
            return;
        }
        Delay(1);
    }

    at->result = at->func(at->arg);

    if (amiga_exit_delay_ms)
        Delay(amiga_exit_delay_ms / 20 + 1);

    /* Publish completion and take our last look at `at` atomically.  The
     * Forbid() is never Permit()ted: nothing else runs until this task is
     * gone (see amiga_live_threads), so the joiner — which may free `at`
     * the moment it can run and observe finished — cannot race the rest.
     * FreeVec and Signal do not break a Forbid(). */
    Forbid();
    live_list_unlink(at);
    at->finished = 1;
    joiner_local = at->joiner;
    sig_local    = at->join_sig;
    free_self    = at->detached;

    if (free_self) {
        /* Detached: nobody joins; we own the struct now. */
        FreeVec(at);
    } else if (joiner_local && sig_local >= 0) {
        Signal(joiner_local, 1UL << sig_local);
    }
    amiga_live_threads--;
}

/* NP_Entry is entered as m68k code; on MorphOS the PPC entry needs a trap
 * gate or the new process dies in the emulator (see platform_amiga_ppc.h). */
CL_PROC_ENTRY_GATE(amiga_thread_entry_gate, amiga_thread_entry);

int platform_thread_create(void **handle, void *(*func)(void *), void *arg,
                           uint32_t stack_size)
{
    AmigaThread *at;
    struct Process *proc;

    at = (AmigaThread *)AllocVec(sizeof(AmigaThread), MEMF_CLEAR);
    if (!at) return -1;

    at->finished = 0;
    at->detached = 0;
    at->result = NULL;
    at->func = func;
    at->arg = arg;
    at->joiner = NULL;
    at->join_sig = -1;

    /* 65536 is both the 0-means-OS-default value and the FLOOR for explicit
     * requests: MP:MAKE-THREAD's :STACK-SIZE may only grow a worker, never
     * hand it less than the historical budget. */
    if (stack_size < 65536) stack_size = 65536;

    /* Share the creator's console with the worker.  Without NP_Input/
     * NP_Output a CreateNewProc child gets NIL: handles, so a worker's
     * Input()/Output() — which every console path in platform_amiga.c goes
     * through — read nothing and print nowhere (host workers, by contrast,
     * naturally share the process's stdin/stdout).  Passing the parent's
     * handles with NP_CloseInput/NP_CloseOutput FALSE is the standard DOS
     * idiom: the child uses them but never closes them at exit.  Concurrent
     * WRITES from several processes are safe — console output goes through
     * unbuffered Write() packets the console handler serializes.  READS keep
     * the pre-existing single-reader assumption (see platform_amiga.c): the
     * buffered FGets/FGetC console reads must only ever run in one process
     * at a time.  TAG_IGNORE keeps the historical NIL: behavior when the
     * creator itself has no console handle (e.g. Workbench launch). */
    {
        BPTR par_in  = Input();
        BPTR par_out = Output();

        /* Forbid() prevents task switching so the child process won't
         * start running before we set tc_UserData.  This is the standard
         * AmigaOS pattern for passing data to a newly created process. */
        Forbid();

        /* Counted before the child exists, so every exit path of
         * amiga_thread_entry decrements a count that was already added. */
        amiga_live_threads++;
        at->live_next = amiga_live_list;
        amiga_live_list = at;

        proc = CreateNewProcTags(
            NP_Entry,     CL_PROC_ENTRY(amiga_thread_entry_gate, amiga_thread_entry),
            CL_PROC_STACK_TAGS(stack_size),
            NP_Name,      (ULONG)"CL-Thread",
            par_in  ? NP_Input      : TAG_IGNORE, (ULONG)par_in,
            par_in  ? NP_CloseInput : TAG_IGNORE, FALSE,
            par_out ? NP_Output      : TAG_IGNORE, (ULONG)par_out,
            par_out ? NP_CloseOutput : TAG_IGNORE, FALSE,
            TAG_DONE
        );
    }

    if (!proc) {
        live_list_unlink(at);
        amiga_live_threads--;
        Permit();
        FreeVec(at);
        return -1;
    }

    at->proc = proc;
    /* Set UserData so the child can find its AmigaThread struct */
    ((struct Task *)proc)->tc_UserData = at;

    Permit();  /* Now child can start and read tc_UserData safely */

    *handle = at;
    return 0;
}

uint32_t platform_thread_live_count(void)
{
    return amiga_live_threads;   /* one aligned long: a single move */
}

uint32_t platform_thread_drain(const volatile uint32_t *keep, uint32_t timeout_ms)
{
    uint32_t elapsed = 0, live;
    for (;;) {
        live = amiga_live_threads;
        if (live <= *keep) return 0;
        if (elapsed >= timeout_ms) return live - *keep;
        Delay(1);                /* 1/50 s: lets a lower-priority worker run */
        elapsed += 20;
    }
}

/* Tasks on the list have not reached their exit tail's Forbid() (they are
 * unlinked inside it), so none is running amilisp code we could cut short
 * mid-way under our own Forbid(): each sits in Wait() or in the ready
 * queue.  RemTask frees its tc_MemEntry (stack, Process); `at` is left
 * alone — the process is exiting and a joiner may still hold it. */
uint32_t platform_thread_remove_stragglers(void)
{
    AmigaThread *at;
    uint32_t n = 0;

    Forbid();
    for (at = amiga_live_list; at; at = at->live_next) {
        if (at->proc) {
            RemTask((struct Task *)at->proc);
            n++;
        }
    }
    amiga_live_list = NULL;
    amiga_live_threads -= n;
    Permit();
    return n;
}

uint32_t platform_thread_set_exit_delay(uint32_t ms)
{
    uint32_t old = amiga_exit_delay_ms;
    amiga_exit_delay_ms = ms;
    return old;
}

int platform_thread_join(void *handle, void **result)
{
    AmigaThread *at = (AmigaThread *)handle;
    BYTE sig = AllocSignal(-1);
    int need_wait = 0;

    /* Register as the joiner (works from ANY task, not just the
     * creator) and decide atomically whether the child already
     * finished.  If no signal bit is free, degrade to a Delay() poll —
     * correct, just coarser. */
    Forbid();
    if (!at->finished) {
        if (sig < 0) {
            while (!at->finished) {
                Permit();
                Delay(1);      /* give the child CPU time (1/50s tick) */
                Forbid();
            }
        } else {
            at->joiner   = FindTask(NULL);
            at->join_sig = sig;
            need_wait = 1;
        }
    }
    Permit();

    if (need_wait)
        Wait(1UL << sig);   /* child's Signal() uses locals only */
    if (sig >= 0)
        FreeSignal(sig);    /* our own task's bit, consumed or unused */

    if (result) *result = at->result;
    FreeVec(at);
    return 0;
}

/* Detach: hand `at` to whichever side finishes last.  If the child
 * already published finished (its final access to `at`), free the
 * struct here; otherwise mark it detached and the child frees it in
 * its exit path.  Closes the old ~32-bytes + one-signal-bit leak per
 * detached thread (the bit exhaustion starved every later condvar
 * wait into its degraded fallback). */
void platform_thread_detach(void *handle)
{
    AmigaThread *at = (AmigaThread *)handle;
    int free_now;

    Forbid();
    if (at->finished) {
        free_now = 1;
    } else {
        at->detached = 1;
        free_now = 0;
    }
    Permit();

    if (free_now)
        FreeVec(at);
}

void platform_thread_yield(void)
{
    /* Let other tasks of same/higher priority run */
    Forbid();
    Permit();
}

/* ================================================================
 * Mutex (SignalSemaphore)
 * ================================================================ */

int platform_mutex_init(void **handle)
{
    struct SignalSemaphore *sem;
    sem = (struct SignalSemaphore *)AllocVec(sizeof(struct SignalSemaphore), MEMF_CLEAR);
    if (!sem) return -1;
    InitSemaphore(sem);
    *handle = sem;
    return 0;
}

/* SignalSemaphore is naturally recursive: the same task may ObtainSemaphore
 * multiple times and must ReleaseSemaphore the same number of times. So a
 * recursive lock on AmigaOS is just a regular SignalSemaphore. */
int platform_mutex_init_recursive(void **handle)
{
    return platform_mutex_init(handle);
}

void platform_mutex_destroy(void *handle)
{
    FreeVec(handle);
}

void platform_mutex_lock(void *handle)
{
    ObtainSemaphore((struct SignalSemaphore *)handle);
}

void platform_mutex_unlock(void *handle)
{
    ReleaseSemaphore((struct SignalSemaphore *)handle);
}

int platform_mutex_trylock(void *handle)
{
    return AttemptSemaphore((struct SignalSemaphore *)handle) ? 0 : 1;
}

/* ================================================================
 * Read-Write Lock (SignalSemaphore shared/exclusive modes)
 *
 * AmigaOS SignalSemaphore natively supports shared (read) and
 * exclusive (write) access — a perfect fit for rwlock semantics.
 * ================================================================ */

int platform_rwlock_init(void **handle)
{
    struct SignalSemaphore *sem;
    sem = (struct SignalSemaphore *)AllocVec(sizeof(struct SignalSemaphore), MEMF_CLEAR);
    if (!sem) return -1;
    InitSemaphore(sem);
    *handle = sem;
    return 0;
}

void platform_rwlock_destroy(void *handle)
{
    FreeVec(handle);
}

void platform_rwlock_rdlock(void *handle)
{
    ObtainSemaphoreShared((struct SignalSemaphore *)handle);
}

void platform_rwlock_wrlock(void *handle)
{
    ObtainSemaphore((struct SignalSemaphore *)handle);
}

void platform_rwlock_unlock(void *handle)
{
    ReleaseSemaphore((struct SignalSemaphore *)handle);
}

/* ================================================================
 * Condition variable
 *
 * AmigaOS has no native condvar.  We implement one using a
 * SignalSemaphore (protects waiter list) + per-waiter signal bits.
 * ================================================================ */

#define AMIGA_CV_MAX_WAITERS 32

typedef struct {
    struct SignalSemaphore  lock;
    struct Task            *waiters[AMIGA_CV_MAX_WAITERS];
    BYTE                    signals[AMIGA_CV_MAX_WAITERS];
    int                     count;
} AmigaCondVar;

int platform_condvar_init(void **handle)
{
    AmigaCondVar *cv = (AmigaCondVar *)AllocVec(sizeof(AmigaCondVar), MEMF_CLEAR);
    if (!cv) return -1;
    InitSemaphore(&cv->lock);
    cv->count = 0;
    *handle = cv;
    return 0;
}

void platform_condvar_destroy(void *handle)
{
    FreeVec(handle);
}

void platform_condvar_wait(void *handle, void *mutex)
{
    AmigaCondVar *cv = (AmigaCondVar *)handle;
    struct Task *me = FindTask(NULL);
    BYTE sig = AllocSignal(-1);
    int registered = 0;

    /* Degraded paths (no free signal bit / waiter table full) must
     * behave like a SPURIOUS WAKEUP: release the mutex so the
     * signalling side can make progress, yield, re-acquire, return.
     * The old code returned WITHOUT releasing the mutex (sig < 0) or
     * Wait()ed on a signal nobody would ever send (table full) —
     * either way the caller's predicate loop then held the mutex
     * forever and every signaller deadlocked (observed as a gc_mutex
     * deadlock once detached threads had exhausted the signal bits). */
    if (sig >= 0) {
        ObtainSemaphore(&cv->lock);
        if (cv->count < AMIGA_CV_MAX_WAITERS) {
            cv->waiters[cv->count] = me;
            cv->signals[cv->count] = sig;
            cv->count++;
            registered = 1;
        }
        ReleaseSemaphore(&cv->lock);
    }

    if (!registered) {
        if (sig >= 0) FreeSignal(sig);
        platform_mutex_unlock(mutex);
        Delay(1);                      /* spurious wakeup, 1/50s backoff */
        platform_mutex_lock(mutex);
        return;
    }

    /* Release the mutex, wait for signal, re-acquire mutex */
    platform_mutex_unlock(mutex);
    Wait(1UL << sig);
    platform_mutex_lock(mutex);

    FreeSignal(sig);
}

int platform_condvar_wait_timeout(void *handle, void *mutex, uint32_t ms)
{
    AmigaCondVar *cv = (AmigaCondVar *)handle;
    struct Task *me = FindTask(NULL);
    BYTE sig = AllocSignal(-1);
    int registered = 0;
    int got = 0;
    int i;

    if (sig >= 0) {
        ObtainSemaphore(&cv->lock);
        if (cv->count < AMIGA_CV_MAX_WAITERS) {
            cv->waiters[cv->count] = me;
            cv->signals[cv->count] = sig;
            cv->count++;
            registered = 1;
        }
        ReleaseSemaphore(&cv->lock);
    }

    if (!registered) {
        /* Same spurious-wakeup degradation as platform_condvar_wait. */
        if (sig >= 0) FreeSignal(sig);
        platform_mutex_unlock(mutex);
        Delay(ms / 20 + 1);
        platform_mutex_lock(mutex);
        return 1;
    }

    platform_mutex_unlock(mutex);

    /* Timed wait via a Delay() poll — crude but functional.  Track the
     * signal in `got`: the old code re-tested SetSignal AFTER the loop,
     * but the in-loop SetSignal had already CONSUMED the bit, so every
     * wait that was signalled during the poll was misreported as a
     * timeout. */
    {
        uint32_t elapsed = 0;
        uint32_t step = ms < 50 ? ms : 50;  /* 50ms steps */
        for (;;) {
            if (SetSignal(0, 1UL << sig) & (1UL << sig)) {
                got = 1;
                break;
            }
            if (elapsed >= ms) break;
            Delay(step / 20 + 1);  /* Delay() uses 1/50s ticks */
            elapsed += step;
        }
    }

    if (!got) {
        /* Timed out — remove ourselves from the waiter list.  If we are
         * NOT in the list, a signaller already popped us and Signal()ed
         * (inside cv->lock, so the send completed before we got the
         * lock): that wakeup targeted us and must not be dropped —
         * consume it and report "signalled". */
        int found = 0;
        ObtainSemaphore(&cv->lock);
        for (i = 0; i < cv->count; i++) {
            if (cv->waiters[i] == me && cv->signals[i] == sig) {
                cv->count--;
                cv->waiters[i] = cv->waiters[cv->count];
                cv->signals[i] = cv->signals[cv->count];
                found = 1;
                break;
            }
        }
        ReleaseSemaphore(&cv->lock);
        if (!found) {
            Wait(1UL << sig);   /* delivered or in flight — consume it */
            got = 1;
        }
    }

    platform_mutex_lock(mutex);
    FreeSignal(sig);
    return got ? 0 : 1;
}

void platform_condvar_signal(void *handle)
{
    AmigaCondVar *cv = (AmigaCondVar *)handle;

    ObtainSemaphore(&cv->lock);
    if (cv->count > 0) {
        cv->count--;
        Signal(cv->waiters[cv->count], 1UL << cv->signals[cv->count]);
    }
    ReleaseSemaphore(&cv->lock);
}

void platform_condvar_broadcast(void *handle)
{
    AmigaCondVar *cv = (AmigaCondVar *)handle;
    int i;

    ObtainSemaphore(&cv->lock);
    for (i = 0; i < cv->count; i++)
        Signal(cv->waiters[i], 1UL << cv->signals[i]);
    cv->count = 0;
    ReleaseSemaphore(&cv->lock);
}

/* ================================================================
 * Per-thread parking (see platform_thread.h)
 *
 * One signal bit of the OWNING task, allocated once at thread start and
 * freed at thread exit.  Exec signals are tokens already: Signal() sets
 * the bit, Wait() blocks until it is set and clears it, and a Signal()
 * that lands before the Wait() stays set until consumed.  This replaces
 * the per-wait AllocSignal(-1) of the old condvar code, whose exhaustion
 * (a task has ~16 free user bits) produced the degraded spurious-wakeup
 * paths above.
 *
 * A TIMED park has no Wait()-with-timeout in Exec without timer.device;
 * it polls SetSignal() between Delay() steps (20 ms ticks, 50 ms cap),
 * exactly as the condvar timed wait did.  Only condition-wait / acquire-
 * lock with a :timeout take that path; untimed parks — every lock
 * handoff and every untimed condition-wait — block in Wait() and wake on
 * the Signal() itself.
 * ================================================================ */

typedef struct {
    struct Task *task;   /* the owner: the only task that parks */
    BYTE         sig;    /* its signal bit */
} AmigaPark;

int platform_park_init(void **handle)
{
    AmigaPark *p;
    BYTE sig = AllocSignal(-1);
    if (sig < 0) return -1;
    p = (AmigaPark *)AllocVec(sizeof(AmigaPark), MEMF_CLEAR);
    if (!p) {
        FreeSignal(sig);
        return -1;
    }
    p->task = FindTask(NULL);
    p->sig = sig;
    *handle = p;
    return 0;
}

void platform_park_destroy(void *handle)
{
    AmigaPark *p = (AmigaPark *)handle;
    if (!p) return;
    /* A token left behind by a late unpark must not leak into whatever
     * this bit is reused for. */
    SetSignal(0, 1UL << p->sig);
    FreeSignal(p->sig);
    FreeVec(p);
}

int platform_park(void *handle, uint32_t timeout_ms)
{
    AmigaPark *p = (AmigaPark *)handle;
    ULONG mask = 1UL << p->sig;

    if (timeout_ms == 0) {
        Wait(mask);        /* consumes the bit */
        return 0;
    }
    {
        uint32_t elapsed = 0;
        uint32_t step = timeout_ms < 50 ? timeout_ms : 50;
        for (;;) {
            if (SetSignal(0, mask) & mask)   /* test-and-clear = consume */
                return 0;
            if (elapsed >= timeout_ms)
                return 1;
            Delay(step / 20 + 1);   /* 1/50 s ticks, rounded up */
            elapsed += step;
        }
    }
}

void platform_unpark(void *handle)
{
    AmigaPark *p = (AmigaPark *)handle;
    Signal(p->task, 1UL << p->sig);
}

/* ================================================================
 * Atomics (Forbid/Permit — safe on 68020, very short critical sections)
 * ================================================================ */

uint32_t platform_atomic_inc(volatile uint32_t *ptr)
{
    uint32_t val;
    Forbid();
    val = ++(*ptr);
    Permit();
    return val;
}

uint32_t platform_atomic_dec(volatile uint32_t *ptr)
{
    uint32_t val;
    Forbid();
    val = --(*ptr);
    Permit();
    return val;
}

int platform_atomic_cas(volatile uint32_t *ptr, uint32_t expected,
                        uint32_t desired)
{
    int ok;
    Forbid();
    ok = (*ptr == expected);
    if (ok) *ptr = desired;
    Permit();
    return ok;
}

void platform_memory_barrier(void)
{
    /* Single-core 68k: no CPU reordering; only compiler reordering must
     * be prevented.  GCC: an empty asm with a memory clobber.  Other
     * compilers (vbcc): Forbid/Permit — opaque libcalls the compiler
     * cannot reorder stores across. */
#if defined(__GNUC__)
    __asm__ __volatile__("" ::: "memory");
#else
    Forbid();
    Permit();
#endif
}

/* ================================================================
 * TLS (Thread-Local Storage via tc_UserData)
 *
 * Note: tc_UserData is also used by AmigaThread for join sync.
 * Phase 1 uses a simple scheme: the main thread sets tc_UserData
 * directly.  For child threads (Phase 4+), the thread entry wrapper
 * will set tc_UserData to the CL_Thread* after reading the
 * AmigaThread* from it.
 * ================================================================ */

void platform_tls_init(void)
{
    /* Nothing needed — tc_UserData exists in every Task struct */
}

void platform_tls_set(void *value)
{
    FindTask(NULL)->tc_UserData = value;
}

void *platform_tls_get(void)
{
    return FindTask(NULL)->tc_UserData;
}
