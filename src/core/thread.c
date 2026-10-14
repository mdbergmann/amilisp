/*
 * thread.c — Thread system initialization and registry.
 *
 * Allocates and initializes the main thread's CL_Thread struct.
 * cl_current_thread is TLS-backed via platform_tls_get/set.
 * Thread registry: linked list of all CL_Thread instances.
 * GC safepoint: threads pause here during stop-the-world GC.
 */

#define CL_THREAD_NO_MACROS  /* access struct members directly */
#include "thread.h"
#include "symbol.h"
#include "package.h"
#include "error.h"
#include "vm.h"
#include "../platform/platform.h"
#include "../platform/platform_thread.h"
#include <string.h>
#include <stdio.h>   /* CLAMIGA_STW_DIAG straggler reports (and race selftest) */
#include <stdlib.h>  /* atoi for the env threshold */

static CL_Thread cl_main_thread;
CL_Thread *cl_main_thread_ptr = NULL;

/* Whether cl_main_thread is currently linked into cl_thread_list — set once,
 * in cl_thread_init, cleared as the very first thing cl_thread_shutdown does
 * (before cl_thread_unregister and the nlx_stack/saved_pending_stack frees
 * that follow it).  cl_main_thread_ptr itself stays non-NULL forever (the
 * crash handler needs it), so pointer identity alone cannot tell a live main
 * thread from a torn-down one; cl_thread_current_is_registered's fast path
 * needs this flag to avoid answering "registered" for a callback that lands
 * on the main task/thread after shutdown has freed its NLX/pending stacks. */
static volatile int cl_main_thread_registered = 0;

/* The main task's TLS slot value (tc_UserData on AmigaOS/MorphOS) as it was
 * BEFORE cl_thread_init overwrote it — restored at exit so the C runtime's
 * post-main teardown finds its own context, not our CL_Thread*. */
static void *cl_saved_main_tls = NULL;

/* ---- Thread registry ---- */
CL_Thread  *cl_thread_list      = NULL;
void       *cl_thread_list_lock = NULL;
uint32_t    cl_thread_count     = 0;

/* ---- GC coordination ---- */
static void *gc_mutex   = NULL;  /* protects GC state transitions */
static void *gc_condvar = NULL;  /* threads wait here during STW; initiator waits for all stopped */

#ifdef DEBUG_THREAD_RACE_HOOKS
/* Set immediately before the STW initiator (cl_gc_stop_the_world) parks in
 * platform_condvar_wait, i.e. the instant it has found a straggler and is
 * about to sleep for a wakeup.  cl_debug_thread_exit_race_selftest()'s
 * worker spins on this so its unregister always lands inside the exact
 * window cl_thread_unregister's gc_condvar broadcast fixes, instead of
 * relying on scheduler timing to hit it (see selftest below). */
static volatile int dbg_race_stw_parked = 0;
#endif

/* Register a thread in the global thread list.
 * `live` = 1 marks it as participating in stop-the-world immediately; use this
 * when the calling context IS the thread's own running OS thread (it can reach
 * safepoints).  `live` = 0 registers a NEWBORN: linked so GC marks its roots,
 * but skipped by the STW wait loop until its OS thread reaches
 * cl_gc_thread_online — see gc_live in thread.h. */
static void cl_thread_register_ex(CL_Thread *t, uint8_t live)
{
    platform_mutex_lock(cl_thread_list_lock);
    t->gc_live = live;
    t->next = cl_thread_list;
    cl_thread_list = t;
    cl_thread_count++;
    platform_mutex_unlock(cl_thread_list_lock);
}

/* Register an already-running thread (self-registration): live immediately. */
void cl_thread_register(CL_Thread *t)
{
    cl_thread_register_ex(t, 1);
}

/* Register a thread whose OS thread does not exist yet (the make-thread child):
 * a newborn that becomes live only when it reaches cl_gc_thread_online. */
void cl_thread_register_newborn(CL_Thread *t)
{
    cl_thread_register_ex(t, 0);
}

/* Unregister a thread from the global thread list */
void cl_thread_unregister(CL_Thread *t)
{
    CL_Thread **pp;
    /* Drop the exiting thread's allocation cursor before it leaves the
     * registry: once off the list, gc_cursor_drop_all can no longer flush
     * it.  A chunk's uncut remainder is already a formatted hole; the next
     * sweep reclaims it.  Must run BEFORE taking the list lock (retire
     * takes alloc_mutex). */
    cl_cursor_retire(t);
    /* Null-guard: a worker that exits during process teardown can arrive
     * here after cl_thread_shutdown tore the registry lock down (only
     * possible in the count<=1 shutdown path; with workers left the lock
     * is deliberately leaked, see cl_thread_shutdown). */
    if (!cl_thread_list_lock) return;
    platform_mutex_lock(cl_thread_list_lock);
    for (pp = &cl_thread_list; *pp; pp = &(*pp)->next) {
        if (*pp == t) {
            *pp = t->next;
            t->next = NULL;
            cl_thread_count--;
            break;
        }
    }
    platform_mutex_unlock(cl_thread_list_lock);

    /* Wake a possible STW initiator.  cl_gc_stop_the_world may be parked
     * in platform_condvar_wait(gc_condvar) waiting for exactly this
     * thread to reach a safepoint; a thread past its last safepoint that
     * exits instead never stops, so without this broadcast the initiator
     * re-scans only on other threads' safepoint broadcasts — with none
     * left, GC (and the whole process) hangs forever.  Must be done
     * AFTER releasing the list lock: the initiator acquires gc_mutex
     * then the list lock, so taking gc_mutex while holding the list lock
     * here would be an ABBA deadlock.  Holding gc_mutex around the
     * broadcast closes the lost-wakeup window (the initiator either
     * holds gc_mutex scanning — and will see the shrunken list — or is
     * inside condvar_wait and receives the broadcast). */
    if (gc_mutex) {
        platform_mutex_lock(gc_mutex);
        platform_condvar_broadcast(gc_condvar);
        platform_mutex_unlock(gc_mutex);
    }
}

/* ---- GC safepoint (slow path) ---- */

void cl_gc_safepoint(void)
{
    CL_Thread *self = (CL_Thread *)platform_tls_get();
    if (!self || !self->gc_requested)
        return;
    /* Shutdown teardown window: cl_thread_shutdown nulls gc_mutex; a
     * straggling worker must not lock a destroyed mutex
     * (ObtainSemaphore(NULL) on Amiga) — same guard as enter/leave_safe_region. */
    if (!gc_mutex)
        return;

    /* Record where we froze so a peer's compaction can conservatively scan
     * THIS thread's JIT native stack ([jit_park_sp .. jit_stack_top)) — the
     * peer cannot use its own SP as the lower bound.  Captured before we mark
     * ourselves stopped so it is valid the instant the initiator observes us. */
    self->jit_park_sp = CL_CAPTURE_SP();

    platform_mutex_lock(gc_mutex);
    self->gc_stopped = 1;
    /* Wake GC initiator who may be waiting for all threads to stop */
    platform_condvar_broadcast(gc_condvar);
    /* Wait until GC is complete (gc_requested cleared by initiator) */
    while (self->gc_requested)
        platform_condvar_wait(gc_condvar, gc_mutex);
    self->gc_stopped = 0;
    platform_mutex_unlock(gc_mutex);
}

/* ---- Safe regions (blocking syscalls outside the heap) ---- */

void cl_gc_enter_safe_region(void)
{
    CL_Thread *self = (CL_Thread *)platform_tls_get();
    if (!self) return;
    /* No GC coordination (e.g. torn down at shutdown) -> nothing to coordinate
     * with, so a "safe region" is a no-op.  Guards against locking a destroyed
     * gc_mutex (ObtainSemaphore(NULL) on Amiga) during exit teardown. */
    if (!gc_mutex) return;
    /* Nested region: the platform layer brackets blocking syscalls too
     * (sleep/file/system), and those may run inside builtin-level
     * brackets.  Only the OUTERMOST pair performs the real transition —
     * an inner leave clearing in_safe_region would hand a "stopped"
     * thread back to heap-touching code mid-GC.  Depth is owner-only.
     * jit_park_sp keeps the OUTER capture: it is higher on the stack, so
     * it covers all JIT frames the inner region would cover and more. */
    if (self->safe_region_depth++ > 0)
        return;
    /* Freeze point for JIT-stack scanning: the blocking syscall we are about to
     * enter runs on stack BELOW here, while any JIT-spilled operands live ABOVE
     * — so [jit_park_sp .. jit_stack_top) covers exactly this thread's JIT
     * frames for a peer's compaction (see jit_park_sp in thread.h). */
    self->jit_park_sp = CL_CAPTURE_SP();
    platform_mutex_lock(gc_mutex);
    self->in_safe_region = 1;
    /* Wake any STW initiator counting safe-region threads as stopped */
    platform_condvar_broadcast(gc_condvar);
    platform_mutex_unlock(gc_mutex);
}

void cl_gc_leave_safe_region(void)
{
    CL_Thread *self = (CL_Thread *)platform_tls_get();
    if (!self) return;
    if (!gc_mutex) return;  /* see cl_gc_enter_safe_region */
    /* Inner leave of a nested region: nothing to do (see enter).  The
     * depth-0 clamp tolerates an unmatched leave (defensive). */
    if (self->safe_region_depth > 0 && --self->safe_region_depth > 0)
        return;
    platform_mutex_lock(gc_mutex);
    /* Re-capture the freeze point AFTER acquiring gc_mutex: the syscall has
     * returned and we are about to park at this frame (below our still-
     * intact JIT frames) until GC finishes.  Captured before the lock, a
     * peer's compaction that starts while we block ON the lock would scan
     * from a stamp above the mutex frames — capturing under the lock makes
     * the stamp cover this exact park frame (tier-4 audit T7). */
    self->jit_park_sp = CL_CAPTURE_SP();
    /* If a GC is currently running, park here until it completes — we
     * cannot return to the heap-touching caller while the world is
     * stopped, otherwise we'd race the mark/sweep. */
    while (self->gc_requested) {
        self->gc_stopped = 1;
        platform_condvar_broadcast(gc_condvar);
        platform_condvar_wait(gc_condvar, gc_mutex);
    }
    self->gc_stopped = 0;
    self->in_safe_region = 0;
    platform_mutex_unlock(gc_mutex);
}

/* Acquire an internal C mutex GC-cooperatively.  See thread.h for the deadlock
 * this avoids.  Fast path: an uncontended trylock — no gc_mutex traffic, so the
 * hot I/O paths pay only one atomic.  Slow path (contended): bracket the
 * blocking wait in a safe region so a peer's stop-the-world GC counts this
 * thread as stopped and proceeds; cl_gc_leave_safe_region() on the far side
 * parks us if a GC is now running.  We hold the lock while parked, which is safe
 * because every other contender waits the same GC-cooperative way — none stalls
 * the world — and GC touches no data these locks protect.
 *
 * Use this ONLY for a lock that can be held across a blocking syscall / GC safe
 * region (the per-stream I/O locks).  Do NOT use it for a lock whose critical
 * section a GC touches (e.g. the stream side-table mutex): the safe region turns
 * the acquisition into a peer-compaction safepoint, and such short, non-blocking
 * critical sections don't need it anyway — a waiter is only briefly delayed. */
void cl_gc_safe_mutex_lock(void *mutex)
{
    if (!mutex) return;
    if (!CL_MT()) { platform_mutex_lock(mutex); return; }
    if (platform_mutex_trylock(mutex) == 0)
        return;                       /* uncontended — no GC coordination */
    cl_gc_enter_safe_region();
    platform_mutex_lock(mutex);
    cl_gc_leave_safe_region();
}

/* ---- Newborn → live transition (thread startup barrier) ---- */

/* Called once at the top of a worker's entry function, BEFORE it touches the
 * heap.  Atomically (under gc_mutex) transitions the thread from newborn
 * (gc_live == 0) to live (gc_live == 1) so it starts participating in
 * stop-the-world GC — but only after making sure it does not come online in
 * the middle of an STW that has already stopped the world.
 *
 * Race analysis (the whole point of this function):
 *   The STW initiator sets gc_requested on all other threads and then, in its
 *   wait loop, waits ONLY for `gc_live` threads (a newborn has no OS thread to
 *   reach a safepoint, so waiting for it would hang the world).  Both the
 *   request/wait and this barrier serialize on gc_mutex, so relative to the
 *   initiator holding gc_mutex this thread is either fully before or fully
 *   after:
 *     - Before the initiator's request: we see gc_requested == 0, become live,
 *       and unlock.  The initiator then requests us (we're live now) and waits
 *       for us; we reach a safepoint at our first allocation and park.  Safe.
 *     - After (initiator already requested us and is in its wait loop, which
 *       releases gc_mutex only inside condvar_wait): we see gc_requested == 1
 *       and park here WITHOUT becoming live or touching the heap.  The wait
 *       loop skips us (gc_live still 0), so the world stops without us; resume
 *       clears gc_requested and wakes us, and we then become live.  Safe.
 */
void cl_gc_thread_online(CL_Thread *self)
{
    if (!self) return;
    if (!gc_mutex) { self->gc_live = 1; return; }
    platform_mutex_lock(gc_mutex);
    /* Park if a stop-the-world is in progress and requested us: we must not
     * enter the heap-touching caller while the world is stopped.  We stay a
     * newborn (gc_live == 0) so the initiator's wait loop does not wait on us. */
    while (self->gc_requested) {
        self->gc_stopped = 1;
        platform_condvar_broadcast(gc_condvar);
        platform_condvar_wait(gc_condvar, gc_mutex);
    }
    self->gc_stopped = 0;
    self->gc_live = 1;   /* now participate in stop-the-world */
    platform_mutex_unlock(gc_mutex);
}

/* ---- Stop-the-world GC coordination ---- */

/* ---- STW straggler diagnostic (CLAMIGA_STW_DIAG) ----
 *
 * When the CLAMIGA_STW_DIAG environment variable is set, the STW initiator's
 * wait loop uses a TIMED condvar wait; every time the timeout elapses without
 * the world having stopped it reports the straggler thread (the one the scan
 * found neither stopped nor in a safe region) and dumps every thread's wait
 * state — so a hung stop-the-world names its culprit in amilisp's own output
 * instead of requiring an external debugger.  The env value is the report
 * cadence in milliseconds; "1" or a non-numeric value selects 5000ms.
 *
 * The timed wait also RESCANS after each timeout, so if the hang was a lost
 * condvar wakeup (the straggler actually stopped but the initiator missed the
 * broadcast) the rescan unsticks the world — the report says so, because that
 * outcome is itself the diagnosis.  Runtime diagnostic, not DEBUG-flag
 * instrumentation: always compiled, zero cost when the env var is unset
 * (same pattern as CLAMIGA_LOCK_DIAG in builtins_thread.c). */

/* Pure parsing rule, factored out of stw_diag_threshold_ms so the
 * DEBUG_THREAD_RACE_HOOKS self-test below can exercise every branch
 * (unset/"1"/non-numeric/>=2) directly without touching the cache below. */
static int32_t stw_diag_parse_ms(const char *s)
{
    if (!s || !*s)
        return -1;
    {
        int v = atoi(s);
        return (v >= 2) ? v : 5000;
    }
}

/* -2 = env not read yet; -1 = disabled; >=2 = cadence in ms.  Benign
 * lazy-init race: every thread computes the same value. */
static int32_t stw_diag_cached_ms = -2;

static int32_t stw_diag_threshold_ms(void)
{
    if (stw_diag_cached_ms == -2) {
        char envbuf[32];
        const char *s = platform_getenv("CLAMIGA_STW_DIAG", envbuf,
                                        (int)sizeof(envbuf));
        stw_diag_cached_ms = stw_diag_parse_ms(s);
    }
    return stw_diag_cached_ms;
}

/* Called by the GC initiator (from cl_gc) to stop all other threads.
 * The caller must NOT hold alloc_mutex when calling this.
 *
 * Reentrancy: if another thread is already inside STW we cannot blocking-lock
 * gc_mutex — that other thread has already requested *us* to stop and is
 * waiting for us to reach a safepoint, but a blocked mutex_lock never yields
 * to one.  We use trylock and either (a) win the race and become initiator,
 * (b) lose to a peer that hasn't requested us yet (yield + retry), or (c)
 * acquire gc_mutex but find we've been requested to stop in the interim
 * (drop the mutex so the requester can continue, park at a safepoint, and
 * try again afterwards). */
void cl_gc_stop_the_world(void)
{
    CL_Thread *self = (CL_Thread *)platform_tls_get();
    CL_Thread *t;

    for (;;) {
        if (platform_mutex_trylock(gc_mutex) == 0) {
            if (!self || !self->gc_requested) break;  /* initiator */
            /* We won the race for gc_mutex but in the meantime another
             * thread (that hasn't released yet through resume_the_world)
             * already marked us for stop.  Release the mutex so they can
             * see all_stopped, then park ourselves. */
            platform_mutex_unlock(gc_mutex);
        }
        cl_gc_safepoint();           /* park if we've been requested */
        platform_thread_yield();
    }

    /* Request all other threads to stop */
    platform_mutex_lock(cl_thread_list_lock);
    for (t = cl_thread_list; t; t = t->next) {
        if (t != self)
            t->gc_requested = 1;
    }
    platform_mutex_unlock(cl_thread_list_lock);
    /* After the flags: a peer looping on direct JIT calls misses its next
     * call site and polls gc_requested (specs/jit-direct-calls.md §5). */
    cl_call_gen_bump("stw request");

    /* Wait until all other threads have reached a safepoint or are inside
     * a safe region (blocking syscall not touching the heap). */
    if (self) self->wait_kind = 4;  /* GC-STW-WAIT (diagnostic) */
    {
    int32_t  diag_ms   = stw_diag_threshold_ms();
    uint32_t waited_ms = 0;   /* cumulative timed-out wait (diag only) */
    for (;;) {
        int all_stopped = 1;
        platform_mutex_lock(cl_thread_list_lock);
        for (t = cl_thread_list; t; t = t->next) {
            /* Skip newborns (gc_live == 0): they are registered so GC marks
             * their roots, but their OS thread has not reached the online
             * barrier and can never hit a safepoint — waiting for one would
             * hang the world.  cl_gc_thread_online parks any newborn that
             * comes online while this STW is in progress, so skipping them
             * here cannot let a thread touch the stopped heap. */
            /* A thread whose registration landed AFTER the request phase
             * above has gc_requested==0 and would be treated as stopped
             * while its OS thread runs.  Today that is unreachable only
             * because bi_make_thread's wrapper alloc is a mandatory
             * safepoint between registering and creating the OS thread —
             * an incidental invariant.  Make it structural: (re)request
             * any live, unrequested peer during each rescan (idempotent,
             * under the list lock); it then parks at its next safepoint
             * before all_stopped can become true. */
            if (t != self && t->gc_live && !t->gc_requested) {
                t->gc_requested = 1;
                cl_call_gen_bump("stw request");
            }
            if (t != self && t->gc_live && t->gc_requested
                && !t->gc_stopped && !t->in_safe_region) {
                all_stopped = 0;
                /* Record which thread is holding up the world so a watchdog
                 * dump can name the straggler that never reached a safepoint
                 * (e.g. a thread blocked in a syscall outside a safe region). */
                if (self) self->wait_straggler_tid = (int)t->id;
                break;
            }
        }
        platform_mutex_unlock(cl_thread_list_lock);

        if (all_stopped) {
            if (waited_ms > 0)
                fprintf(stderr, "GC-STW diag: world stopped after %u ms of "
                        "timed-out waits - a timeout RESCAN unstuck it, which "
                        "points at a lost gc_condvar wakeup, or at a straggler "
                        "that stopped without a broadcast reaching us\n",
                        (unsigned)waited_ms);
            break;
        }
#ifdef DEBUG_THREAD_RACE_HOOKS
        dbg_race_stw_parked = 1;
#endif
        if (diag_ms < 0) {
            platform_condvar_wait(gc_condvar, gc_mutex);
        } else if (platform_condvar_wait_timeout(gc_condvar, gc_mutex,
                                                 (uint32_t)diag_ms) != 0) {
            waited_ms += (uint32_t)diag_ms;
            fprintf(stderr, "GC-STW diag: world NOT stopped after %u ms - "
                    "straggler tid=%d (running, neither at a safepoint nor in "
                    "a safe region); all-thread dump follows\n",
                    (unsigned)waited_ms, self ? self->wait_straggler_tid : -1);
            cl_dump_thread_waits();
        }
    }
    }
    if (self) self->wait_kind = 0;
    /* gc_mutex remains held — caller runs GC then calls resume */
}

/* Called after GC completes to wake all stopped threads */
void cl_gc_resume_the_world(void)
{
    CL_Thread *t;

    /* Clear gc_requested on all threads */
    platform_mutex_lock(cl_thread_list_lock);
    for (t = cl_thread_list; t; t = t->next) {
        t->gc_requested = 0;
        /* gc_stopped will be cleared by the thread itself in cl_gc_safepoint */
    }
    platform_mutex_unlock(cl_thread_list_lock);

    /* Wake all waiting threads */
    platform_condvar_broadcast(gc_condvar);
    platform_mutex_unlock(gc_mutex);
}

#ifdef DEBUG_THREAD_RACE_HOOKS
/* ---- Deterministic repro/regression check for the STW-hang-on-exit fix ----
 *
 * The bug cl_thread_unregister's gc_condvar broadcast fixes needs a worker
 * to exit (unregister) at the exact instant the STW initiator has found it
 * as the sole straggler and is parking in platform_condvar_wait — too
 * narrow a window to land reliably via ordinary thread-churn timing on a
 * multi-core host.  This self-test forces the window directly: the worker
 * never reaches a safepoint (simulating one that already crossed its last
 * one) and only unregisters once dbg_race_stw_parked confirms the
 * initiator is about to sleep.  If cl_thread_unregister's wakeup were
 * missing or broken, cl_gc_stop_the_world below never returns and the
 * process hangs — the shell test wraps this binary in `timeout` to turn
 * that hang into a deterministic FAIL. */

static void *dbg_race_worker_fn(void *arg)
{
    CL_Thread *t = (CL_Thread *)arg;
    cl_thread_register(t);            /* live=1: the STW below will target it */
    while (!dbg_race_stw_parked)      /* wait for the initiator to park */
        platform_thread_yield();
    cl_thread_unregister(t);          /* exits WITHOUT ever hitting a safepoint */
    return NULL;
}

/* ---- STW straggler-diagnostic self-test (CLAMIGA_STW_DIAG) ----
 *
 * Exercises stw_diag_parse_ms's branches directly (a pure function, no
 * process state to race on) and then drives one real timed-wait/rescan
 * cycle: a worker registers live and sleeps well past the configured
 * threshold without ever reaching a safepoint, forcing
 * cl_gc_stop_the_world's timed wait to expire at least once, print a
 * straggler report, and rescan once the worker unregisters. A hang here
 * means the timed-wait/rescan path regressed; the shell test's `timeout`
 * wrapper turns that into a deterministic FAIL like the race test below.
 *
 * Must run before dbg_race_selftest below touches cl_gc_stop_the_world:
 * stw_diag_cached_ms is a process-wide lazy cache, and this is the only
 * chance to have it observe CLAMIGA_STW_DIAG being set. */
static void *dbg_stw_diag_worker_fn(void *arg)
{
    CL_Thread *t = (CL_Thread *)arg;
    cl_thread_register(t);     /* live=1: the STW below will target it */
    platform_sleep_ms(60);     /* outlasts the 20ms threshold set below */
    cl_thread_unregister(t);   /* exits WITHOUT ever hitting a safepoint */
    return NULL;
}

static int dbg_stw_diag_selftest(void)
{
    CL_Thread *worker;
    void *handle;
    int spins;

    if (stw_diag_parse_ms(NULL)  != -1)   return 0;
    if (stw_diag_parse_ms("")    != -1)   return 0;
    if (stw_diag_parse_ms("1")   != 5000) return 0;
    if (stw_diag_parse_ms("abc") != 5000) return 0;
    if (stw_diag_parse_ms("20")  != 20)   return 0;

    setenv("CLAMIGA_STW_DIAG", "20", 1);

    worker = cl_thread_alloc_worker();
    if (!worker) return 0;
    if (platform_thread_create(&handle, dbg_stw_diag_worker_fn, worker,
                               CL_WORKER_C_STACK_SIZE) != 0) {
        cl_thread_free_worker(worker);
        return 0;
    }

    for (spins = 0; cl_thread_count < 2 && spins < 1000000; spins++)
        platform_thread_yield();
    if (cl_thread_count < 2) return 0;

    cl_gc_stop_the_world();   /* hangs here if the diag path regresses */
    cl_gc_resume_the_world();

    printf("STW-DIAG-SELFTEST-OK\n");
    return 1;
}

/* Runs the race deterministically and reports the outcome on stdout/exit
 * code; invoked automatically at process start when this build is compiled
 * with -DDEBUG_THREAD_RACE_HOOKS (see tests/test_mt_thread_exit_gc.sh and
 * the `test-mt-thread-exit-race` Makefile target). */
static void dbg_race_selftest_and_exit(void)
{
    CL_Thread *worker;
    void *handle;
    int spins;

    cl_thread_init();

    if (!dbg_stw_diag_selftest()) {
        fprintf(stderr, "RACE-SELFTEST: STW-diag self-test failed\n");
        exit(2);
    }

    worker = cl_thread_alloc_worker();
    if (!worker) {
        fprintf(stderr, "RACE-SELFTEST: worker alloc failed\n");
        exit(2);
    }

    if (platform_thread_create(&handle, dbg_race_worker_fn, worker,
                               CL_WORKER_C_STACK_SIZE) != 0) {
        fprintf(stderr, "RACE-SELFTEST: thread_create failed\n");
        cl_thread_free_worker(worker);
        exit(2);
    }

    /* Wait for the worker to register (bounded — this is not the race
     * window under test, just startup). */
    for (spins = 0; cl_thread_count < 2 && spins < 1000000; spins++)
        platform_thread_yield();
    if (cl_thread_count < 2) {
        fprintf(stderr, "RACE-SELFTEST: worker never registered\n");
        exit(2);
    }

    cl_gc_stop_the_world();   /* hangs here if the fix regresses */
    cl_gc_resume_the_world();

    printf("RACE-SELFTEST-OK\n");
    exit(0);
}

__attribute__((constructor))
static void dbg_race_selftest_ctor(void)
{
    /* CLAMIGA_RACE_SELFTEST=0 skips the self-test so the same binary can
     * run a Lisp program under the other race hooks it carries
     * (CLAMIGA_RACE_LOCK_MARK_DELAY_MS, tests/test_mt_lock_barge_race.sh). */
    const char *skip = getenv("CLAMIGA_RACE_SELFTEST");
    if (skip && skip[0] == '0' && skip[1] == '\0')
        return;
    dbg_race_selftest_and_exit();  /* never returns */
}
#endif /* DEBUG_THREAD_RACE_HOOKS */

/* ---- Thread interrupt handling (slow path) ---- */

void cl_thread_handle_interrupt(CL_Thread *t)
{
    CL_Obj func;

    if (!t->interrupt_pending)
        return;

    /* Defer while holding an internal rwlock reader: an interrupt closure
     * that signals — or a destroy — unwinds via longjmp, and nothing
     * restores rdlock_tables_held on that path.  The leaked reader count
     * then blocks the next writer FOREVER (process-wide hang).  Leaving
     * interrupt_pending set retries at the next safepoint outside the
     * locked section.
     *
     * NOTE: rdlock_package_held is declared (thread.h) but nothing
     * currently increments it — cl_package_rwlock is taken directly via
     * platform_rwlock_rdlock() in package.c/symbol.c, not through a
     * counted wrapper like cl_tables_rdlock_at().  Until that wrapper
     * exists, checking it here would always read 0, so it is
     * intentionally omitted rather than included as dead, misleading
     * coverage. */
    if (t->rdlock_tables_held > 0)
        return;

    /* Pair with the publisher's barrier (bi_interrupt_thread /
     * bi_destroy_thread): having observed interrupt_pending == 1, order
     * the payload reads (destroy_requested / interrupt_func) after it —
     * on ARM64 the loads can otherwise be satisfied in the reverse
     * order and consume the flag with a stale NIL payload. */
    platform_memory_barrier();

    /* Consume under cl_thread_list_lock — the same lock the publishers
     * hold.  The old unlocked read-then-clear raced a concurrent second
     * publish: this thread's NIL/0 stores clobbered the fresh payload
     * (interrupt silently dropped), or cleared interrupt_pending while a
     * concurrent destroy_requested stayed set — the destroy was then
     * never seen again (mp:destroy-thread of a busy worker never took
     * effect). */
    if (cl_thread_list_lock) platform_mutex_lock(cl_thread_list_lock);
    if (!t->interrupt_pending) {
        if (cl_thread_list_lock) platform_mutex_unlock(cl_thread_list_lock);
        return;
    }

    /* destroy-thread: abort the thread by raising an error */
    if (t->destroy_requested) {
        t->destroy_requested = 0;
        t->interrupt_pending = 0;
        t->interrupt_func = CL_NIL;
        if (cl_thread_list_lock) platform_mutex_unlock(cl_thread_list_lock);
        /* Quiet abort: runs handler-case / unwind-protect cleanups but does
         * not enter the debugger, so a destroyed worker unwinds silently
         * instead of dropping into SLDB / fgets(stdin). */
        cl_abort_current_thread("Thread destroyed");
        return;  /* not reached — longjmps */
    }

    /* interrupt-thread: call the pending function.  Protect the C local:
     * t->interrupt_func was just cleared (the old root), and cl_vm_apply
     * copies its func argument only after some allocating setup on the
     * closure path — an unrooted `func` across that window is the usual
     * stale-local hazard (tier-4 audit F7). */
    func = t->interrupt_func;
    t->interrupt_func = CL_NIL;
    t->interrupt_pending = 0;
    if (cl_thread_list_lock) platform_mutex_unlock(cl_thread_list_lock);

    if (!CL_NULL_P(func)) {
        CL_GC_PROTECT(func);
        cl_vm_apply(func, NULL, 0);
        CL_GC_UNPROTECT(1);
    }
}

/* ---- TLV table operations ---- */

CL_Obj cl_tlv_get(CL_Thread *t, CL_Obj sym)
{
    uint32_t idx = (sym >> 2) & (CL_TLV_TABLE_SIZE - 1);
    uint32_t i;
    for (i = 0; i < CL_TLV_TABLE_SIZE; i++) {
        uint32_t slot = (idx + i) & (CL_TLV_TABLE_SIZE - 1);
        CL_Obj k = t->tlv_table[slot].symbol;
        if (k == CL_NIL) return CL_TLV_ABSENT;
        if (k == sym)     return t->tlv_table[slot].value;
    }
    return CL_TLV_ABSENT;
}

void cl_tlv_set(CL_Thread *t, CL_Obj sym, CL_Obj val)
{
    uint32_t idx = (sym >> 2) & (CL_TLV_TABLE_SIZE - 1);
    uint32_t first_tombstone = CL_TLV_TABLE_SIZE;
    uint32_t i;
    for (i = 0; i < CL_TLV_TABLE_SIZE; i++) {
        uint32_t slot = (idx + i) & (CL_TLV_TABLE_SIZE - 1);
        CL_Obj k = t->tlv_table[slot].symbol;
        if (k == sym) {
            t->tlv_table[slot].value = val;
            return;
        }
        if (k == CL_UNBOUND && first_tombstone == CL_TLV_TABLE_SIZE)
            first_tombstone = slot;
        if (k == CL_NIL) {
            uint32_t target = (first_tombstone < CL_TLV_TABLE_SIZE)
                              ? first_tombstone : slot;
            t->tlv_table[target].symbol = sym;
            t->tlv_table[target].value  = val;
            t->tlv_entry_count++;
            return;
        }
    }
    if (first_tombstone < CL_TLV_TABLE_SIZE) {
        t->tlv_table[first_tombstone].symbol = sym;
        t->tlv_table[first_tombstone].value  = val;
        t->tlv_entry_count++;
    }
}

void cl_tlv_remove(CL_Thread *t, CL_Obj sym)
{
    uint32_t idx = (sym >> 2) & (CL_TLV_TABLE_SIZE - 1);
    uint32_t i;
    for (i = 0; i < CL_TLV_TABLE_SIZE; i++) {
        uint32_t slot = (idx + i) & (CL_TLV_TABLE_SIZE - 1);
        CL_Obj k = t->tlv_table[slot].symbol;
        if (k == CL_NIL) return;
        if (k == sym) {
            t->tlv_table[slot].symbol = CL_UNBOUND;
            t->tlv_table[slot].value  = CL_NIL;
            if (t->tlv_entry_count > 0)
                t->tlv_entry_count--;
            return;
        }
    }
}

/* Rebuild the TLV table after a compacting GC has relocated symbols.
 * Entries are keyed by symbol arena-offset (sym >> 2); when the GC moves a
 * symbol it updates the stored offset in place but the entry stays in its old
 * probe slot, so cl_tlv_get would recompute a different slot and miss — the
 * dynamic binding silently reverts to the symbol's global value. Re-insert
 * every live entry by its new hash. Tombstones are dropped: a clean rebuild
 * reconstructs all probe chains and needs no deleted-markers. */
void cl_tlv_rehash(CL_Thread *t)
{
    CL_TLVEntry live[CL_TLV_TABLE_SIZE];
    uint32_t n = 0, i;

    if (!t || t->tlv_entry_count == 0)
        return;

    /* Snapshot live entries (skip empties and tombstones) */
    for (i = 0; i < CL_TLV_TABLE_SIZE; i++) {
        CL_Obj sym = t->tlv_table[i].symbol;
        if (sym != CL_NIL && sym != CL_UNBOUND) {
            live[n].symbol = sym;
            live[n].value  = t->tlv_table[i].value;
            n++;
        }
    }

    /* Clear the table, then re-insert by the new (post-move) hashes */
    for (i = 0; i < CL_TLV_TABLE_SIZE; i++) {
        t->tlv_table[i].symbol = CL_NIL;
        t->tlv_table[i].value  = CL_NIL;
    }
    t->tlv_entry_count = 0;
    for (i = 0; i < n; i++)
        cl_tlv_set(t, live[i].symbol, live[i].value);
}

/* High-level TLV-aware accessors.
 * Fast path: when tlv_entry_count == 0, no dynamic bindings are active
 * on this thread, so skip the TLV hash probe entirely.  This turns a
 * 30-50 cycle probe into a single compare + direct field read. */

/* Per-thread slot behind the cl_current_package macro (package.h).
 * Mirrors cl_symbol_value's CT resolution below.  The pre-thread-init
 * fallback slot exists only for code paths that touch package machinery
 * before cl_thread_init has installed cl_main_thread_ptr; the real
 * binary (and every test harness) runs cl_thread_init first, so it is
 * never live while the heap is — it needs no GC rooting. */
CL_Obj *cl_current_package_ref(void)
{
    static CL_Obj pre_thread_init_slot = CL_NIL;
    CL_Thread *t = (cl_thread_count <= 1)
                   ? cl_main_thread_ptr
                   : (CL_Thread *)platform_tls_get();
    if (!t) return &pre_thread_init_slot;
    return &t->current_package;
}

/* Thread-deriving wrapper over cl_symbol_value_on (thread.h), which is what
 * the VM calls with the thread it already holds.  CL_NIL's value cell lives
 * on the SYM_NIL storage shadow; the inline does that fixup. */
CL_Obj cl_symbol_value(CL_Obj sym)
{
    CL_Thread *t = (cl_thread_count <= 1)
                   ? cl_main_thread_ptr
                   : (CL_Thread *)platform_tls_get();
    return cl_symbol_value_on(t, sym);
}

/* C-level dynamic binding, mirroring OP_DYNBIND: installs a THREAD-LOCAL
 * override restored by cl_dynbind_restore_to(mark).  For C builtins that
 * temporarily override specials (the *PRINT-* controls in WRITE /
 * WRITE-TO-STRING / PPRINT / FORMAT's ~D renderer): writing the GLOBAL cell
 * and restoring it later races every peer thread's reader between the set
 * and the restore (tier-4 FS16).  Restoring to a CL_TLV_ABSENT old value
 * REMOVES the TLV entry, so a transient bind leaves no per-thread residue. */
void cl_dynbind_c(CL_Obj sym, CL_Obj val)
{
    CL_Thread *t = CT;
    CL_Obj old = cl_tlv_get(t, sym);
    if (t->dyn_top >= CL_MAX_DYN_BINDINGS)
        cl_error(CL_ERR_OVERFLOW, "Dynamic binding stack overflow");
    t->dyn_stack[t->dyn_top].symbol = sym;
    t->dyn_stack[t->dyn_top].old_value = old;
    t->dyn_top++;
    cl_tlv_set(t, sym, val);
    if (sym == SYM_STAR_PACKAGE)
        cl_sync_current_package_from_dynamic();
}

void cl_set_symbol_value(CL_Obj sym, CL_Obj val)
{
    CL_Thread *t = (cl_thread_count <= 1)
                   ? cl_main_thread_ptr
                   : (CL_Thread *)platform_tls_get();
    if (CL_NULL_P(sym)) sym = SYM_NIL;
    if (t->tlv_entry_count > 0) {
        CL_Obj v = cl_tlv_get(t, sym);
        if (v != CL_TLV_ABSENT) {
            cl_tlv_set(t, sym, val);
            return;
        }
    }
    ((CL_Symbol *)CL_OBJ_TO_PTR(sym))->value = val;
}

/* Compare-and-swap on one CL_Obj cell — the primitive behind
 * MP:COMPARE-AND-SWAP.  Returns the value the cell held when the CAS was
 * decided: OLD when the swap happened, the other content when it did not
 * (the SBCL CAS convention; callers test success with an EQ against OLD).
 *
 * platform_atomic_cas is a native lock-cmpxchg / LL-SC on the host and a
 * Forbid()/Permit() window on the single-core AmigaOS and MorphOS targets,
 * where disabling task switching is a complete atomicity guarantee.  A
 * failed hardware CAS must not simply return a fresh read of the cell: a
 * racing thread could store OLD back between the failure and that read,
 * and the caller would then take a swap that never happened for success.
 * So re-decide from the re-read value until the cell either differs from
 * OLD or the swap succeeds. */
CL_Obj cl_cas_cell(volatile CL_Obj *cell, CL_Obj old, CL_Obj new_val)
{
    for (;;) {
        CL_Obj seen = *cell;
        if (seen != old) return seen;
        if (platform_atomic_cas(cell, old, new_val)) return old;
    }
}

/* CAS on a symbol's value with the same binding resolution as
 * CL_SYMBOL_VALUE / CL_SET_SYMBOL_VALUE: a thread-local (dynamic) binding of
 * SYM in the calling thread is the target if one exists, else the global
 * cell.  Returns the previous value — CL_UNBOUND if the target binding was
 * unbound (the caller decides how to report that).  A TLV entry is owned
 * by its thread alone, so a plain compare-then-store is atomic there. */
CL_Obj cl_symbol_value_cas(CL_Obj sym, CL_Obj old, CL_Obj new_val)
{
    CL_Thread *t = (cl_thread_count <= 1)
                   ? cl_main_thread_ptr
                   : (CL_Thread *)platform_tls_get();
    if (CL_NULL_P(sym)) sym = SYM_NIL;
    if (t->tlv_entry_count > 0) {
        CL_Obj v = cl_tlv_get(t, sym);
        if (v != CL_TLV_ABSENT) {
            if (v == old) cl_tlv_set(t, sym, new_val);
            return v;
        }
    }
    return cl_cas_cell(&((CL_Symbol *)CL_OBJ_TO_PTR(sym))->value, old, new_val);
}

int cl_symbol_boundp(CL_Obj sym)
{
    CL_Thread *t = (cl_thread_count <= 1)
                   ? cl_main_thread_ptr
                   : (CL_Thread *)platform_tls_get();
    if (CL_NULL_P(sym)) sym = SYM_NIL;
    if (t->tlv_entry_count > 0) {
        CL_Obj v = cl_tlv_get(t, sym);
        if (v != CL_TLV_ABSENT) return v != CL_UNBOUND;
    }
    return ((CL_Symbol *)CL_OBJ_TO_PTR(sym))->value != CL_UNBOUND;
}

/* ---- Side tables and worker thread allocation ---- */

CL_Thread *cl_thread_table[CL_MAX_THREADS];
uint32_t cl_thread_table_gen[CL_MAX_THREADS];

/* Process-unique thread serials (see CL_Thread.serial).  Main is 1; the
 * counter only ever grows, so a serial baked into a lock's state word
 * can never come to mean a different thread. */
static volatile uint32_t cl_thread_serial_counter = 1;

uint32_t cl_thread_next_serial(void)
{
    return platform_atomic_inc(&cl_thread_serial_counter);
}

int cl_thread_table_alloc(CL_Thread *t)
{
    int i, result = -1;
    platform_mutex_lock(cl_thread_list_lock);
    for (i = 0; i < CL_MAX_THREADS; i++) {
        if (!cl_thread_table[i]) {
            cl_thread_table[i] = t;
            /* New occupancy: invalidate every wrapper still pointing at
             * this slot from a previous occupant (see cl_thread_table_gen
             * in thread.h). */
            cl_thread_table_gen[i]++;
            result = i;
            break;
        }
    }
    platform_mutex_unlock(cl_thread_list_lock);
    return result;
}

void cl_thread_table_free(int id)
{
    platform_mutex_lock(cl_thread_list_lock);
    if (id >= 0 && id < CL_MAX_THREADS)
        cl_thread_table[id] = NULL;
    platform_mutex_unlock(cl_thread_list_lock);
}

CL_Thread *cl_thread_alloc_worker(void)
{
    return cl_thread_alloc_worker_sized(0, 0, 0);
}

CL_Thread *cl_thread_alloc_worker_sized(uint32_t vm_stack_size,
                                        uint32_t vm_frames,
                                        uint32_t nlx_frames)
{
    CL_Thread *t;

    /* Each requested size is a MINIMUM (see thread.h): raise anything below
     * the platform default to the default, so workers only ever grow. */
    if (vm_stack_size < CL_WORKER_VM_STACK_SIZE)
        vm_stack_size = CL_WORKER_VM_STACK_SIZE;
    if (vm_frames < CL_WORKER_VM_FRAME_SIZE)
        vm_frames = CL_WORKER_VM_FRAME_SIZE;
    if (nlx_frames < CL_WORKER_NLX_FRAMES)
        nlx_frames = CL_WORKER_NLX_FRAMES;

    t = (CL_Thread *)platform_alloc(sizeof(CL_Thread));
    if (!t) return NULL;
    memset(t, 0, sizeof(CL_Thread));
    /* Before the allocations below, so the buffer is valid for the whole
     * lifetime — including the failure paths, which free t without ever
     * reaching the end of this function. */
    cl_thread_backtrace_init(t);

    /* Allocate VM stack and frames.  On host the CL_WORKER_ sizes equal the
     * main thread's, so a worker can run the same call/NLX depth main can (a
     * smaller budget silently kills the worker — see thread.h).  On AmigaOS they
     * stay at the historical compact sizes; see the CL_WORKER_* rationale. */
    t->vm.stack = (CL_Obj *)platform_alloc(
        vm_stack_size * sizeof(CL_Obj));
    if (!t->vm.stack) { platform_free(t); return NULL; }
    t->vm.stack_size = vm_stack_size;
    t->vm.frames = (CL_Frame *)platform_alloc(
        vm_frames * sizeof(CL_Frame));
    if (!t->vm.frames) {
        platform_free(t->vm.stack);
        platform_free(t);
        return NULL;
    }
    t->vm.frame_size = (int)vm_frames;

    /* Allocate NLX stack */
    t->nlx_stack = (CL_NLXFrame *)platform_alloc(
        nlx_frames * sizeof(CL_NLXFrame));
    if (!t->nlx_stack) {
        platform_free(t->vm.frames);
        platform_free(t->vm.stack);
        platform_free(t);
        return NULL;
    }
    t->nlx_max = (int)nlx_frames;

    /* Allocate saved pending-throw stack */
    t->saved_pending_stack = (CL_SavedPending *)platform_alloc(
        CL_WORKER_SAVED_PENDING * sizeof(CL_SavedPending));
    if (!t->saved_pending_stack) {
        platform_free(t->nlx_stack);
        platform_free(t->vm.frames);
        platform_free(t->vm.stack);
        platform_free(t);
        return NULL;
    }
    t->saved_pending_max = CL_WORKER_SAVED_PENDING;

    /* Default: single-value mode */
    t->mv_count = 1;
    t->status = 0; /* created */
    t->serial = cl_thread_next_serial();
    /* park stays NULL until the worker's own OS thread creates it
     * (thread_entry) — on AmigaOS the handle is a signal bit of the task
     * that parks, so the parent cannot allocate it. */

    return t;
}

void cl_thread_backtrace_init(CL_Thread *t)
{
    t->backtrace_buf = t->backtrace_inline;
    t->backtrace_cap = CL_BACKTRACE_BUF_SIZE;
    t->backtrace_inline[0] = '\0';
}

void cl_thread_backtrace_release(CL_Thread *t)
{
    char *heap = t->backtrace_buf;
    size_t n;

    if (heap == NULL) {              /* never initialized */
        cl_thread_backtrace_init(t);
        return;
    }
    if (heap == t->backtrace_inline)  /* never grown: nothing to release */
        return;

    /* Keep as much of the text as the inline block holds rather than clearing
     * it: cl_thread_shutdown releases the main thread's buffer but deliberately
     * leaves the thread readable, because the crash handler still prints this
     * backtrace. */
    n = strlen(heap);
    if (n > (size_t)CL_BACKTRACE_BUF_SIZE - 1)
        n = (size_t)CL_BACKTRACE_BUF_SIZE - 1;
    memcpy(t->backtrace_inline, heap, n);
    t->backtrace_inline[n] = '\0';

    platform_free(heap);
    t->backtrace_buf = t->backtrace_inline;
    t->backtrace_cap = CL_BACKTRACE_BUF_SIZE;
}

/* Reset every Lisp-heap reference gc_visit_thread_roots walks for this
 * thread.  Called by cl_mem_init for each REGISTERED thread on heap
 * re-initialization: the arena those CL_Obj values pointed into was just
 * freed, so each one is a stale offset that — once the fresh heap's bump
 * front grows past it — passes gc_mark's plausibility validation and
 * stamps a mark bit into the middle of an unrelated live object (the
 * "stale static tables on re-init" corruption class; see the shared-
 * globals reset in cl_mem_init and the regression tests in
 * tests/test_gc_threaded.c).  Keep the field list in sync with
 * gc_visit_thread_roots in gc_trace.c. */
void cl_thread_reset_lisp_state(CL_Thread *t)
{
    int i;
    t->gc_root_count = 0;
    t->dyn_top = 0;
    t->nlx_top = 0;
    t->handler_top = 0;
    t->handler_floor = 0;
    t->handler_active_mask = 0;
    t->restart_top = 0;
    t->restart_floor = 0;
    t->nlx_floor = 0;
    t->callback_depth = 0;
    t->callback_error_code = 0;
    t->callback_error = CL_NIL;
    t->last_condition = CL_NIL;
    t->vm.sp = 0;
    t->vm.fp = 0;
    for (i = 0; i < CL_MAX_MV; i++)
        t->mv_values[i] = CL_NIL;   /* marked unconditionally, all slots */
    t->pending_throw = 0;
    t->pending_tag = CL_NIL;
    t->pending_value = CL_NIL;
    t->pending_mv_count = 0;
    t->saved_pending_top = 0;
    t->mv_save_top = 0;   /* UNWIND-PROTECT value records (mv_save_buf) */
    t->pending_lambda_name = CL_NIL;
    t->name = CL_NIL;
    t->result = CL_NIL;
    t->interrupt_func = CL_NIL;
    t->thread_obj = CL_NIL;
    t->current_lex_env = CL_NIL;
    t->current_package = CL_NIL;   /* re-set by cl_package_init on re-init */
    t->rd_stream = CL_NIL;
    t->rd_uninterned = CL_NIL;
    t->rd_labels = CL_NIL;
    t->pr_stream = CL_NIL;
    t->pr_inprog_top = 0;
    t->pr_circle_active = 0;
    for (i = 0; i < CL_CIRCLE_HT_SIZE; i++)
        t->pr_circle_keys[i] = CL_NIL;
    t->ltv_init_count = 0;
    t->active_compiler = NULL;   /* GC walks the compiler chain too */
    for (i = 0; i < CL_TLV_TABLE_SIZE; i++) {
        t->tlv_table[i].symbol = CL_NIL;
        t->tlv_table[i].value = CL_NIL;
    }
    t->tlv_entry_count = 0;
    t->vm_extra_count = 0;   /* cl_vm_gc_visit_extra_thread walks this too */
    t->wait_kind = 0;        /* MP wait registration (wait_obj/wait_lock are
                              * GC roots — see gc_visit_thread_roots) */
    t->wait_obj = CL_NIL;
    t->wait_lock = CL_NIL;
}

int cl_thread_current_is_registered(void)
{
    void *me = platform_tls_get();
    CL_Thread *t;
    int found = 0;
    int attempts;
    if (!me) return 0;
    /* Fast path for the common case — a GUI program's hooks and dispatchers
     * all run on the main thread — answered without touching the lock at
     * all.  Pointer identity alone is not enough: cl_thread_shutdown leaves
     * cl_main_thread_ptr valid forever (the crash handler needs it) but
     * unregisters cl_main_thread and frees its nlx_stack/saved_pending_stack,
     * so a callback landing on the main task/thread after shutdown must fall
     * through to "not registered" rather than resolve to torn-down state via
     * this identity check. cl_main_thread_registered tracks exactly whether
     * cl_main_thread is currently linked into cl_thread_list. */
    if (me == (void *)cl_main_thread_ptr) return cl_main_thread_registered;
    /* A foreign task must not BLOCK on Lisp's list mutex, but it must also
     * not walk cl_thread_list lock-free: cl_thread_unregister unlinks a node
     * from the middle of the list under this same lock, and once unlinked a
     * concurrent free (cl_thread_free_worker, from the zombie reaper or a
     * GC-sweep finalize) can reclaim it at any time — a lock-free reader
     * that already holds a pointer read before the unlink would then
     * dereference `t->next` on freed memory. platform_mutex_trylock never
     * blocks (AttemptSemaphore on AmigaOS, pthread_mutex_trylock on POSIX).
     * The lock is only ever held for a few instructions (a link, an unlink,
     * a scan), so a bounded retry with a yield between attempts makes a
     * false "not registered" for a genuine Lisp worker — whose hook would
     * then silently return 0 — practically impossible, while a foreign task
     * still never waits indefinitely. */
    if (!cl_thread_list_lock) return 0;
    for (attempts = 0; attempts < 1000; attempts++) {
        if (platform_mutex_trylock(cl_thread_list_lock) == 0) {
            for (t = cl_thread_list; t; t = t->next)
                if ((void *)t == me) { found = 1; break; }
            platform_mutex_unlock(cl_thread_list_lock);
            return found;
        }
        platform_thread_yield();
    }
    return 0;
}

void cl_thread_free_worker(CL_Thread *t)
{
    if (!t) return;
    cl_thread_backtrace_release(t);
    if (t->saved_pending_stack) platform_free(t->saved_pending_stack);
    if (t->nlx_stack)  platform_free(t->nlx_stack);
    if (t->vm.frames)  platform_free(t->vm.frames);
    if (t->vm.stack)   platform_free(t->vm.stack);
    platform_free(t);
}

/* ---- Init / Shutdown ---- */

void cl_thread_init(void)
{
    memset(&cl_main_thread, 0, sizeof(CL_Thread));
    cl_thread_backtrace_init(&cl_main_thread);

    /* Initialize TLS and set main thread as current.
     *
     * Save whatever the OS/C-runtime had stored in this task's TLS slot BEFORE
     * we overwrite it.  On AmigaOS/MorphOS platform_tls_* is the task's
     * tc_UserData, which the -noixemul C runtime may use for its own per-task
     * context.  We never read our value back on the main thread (CT uses the
     * cl_main_thread_ptr global while single-threaded), but the runtime re-reads
     * tc_UserData during crt0's post-main teardown — leaving our CL_Thread* there
     * makes that teardown dereference garbage and freeze the machine.  Restore
     * the saved value at exit via cl_thread_restore_main_tls(). */
    platform_tls_init();
    cl_saved_main_tls = platform_tls_get();
    platform_tls_set(&cl_main_thread);
    cl_main_thread_ptr = &cl_main_thread;

    /* Initialize thread registry */
    platform_mutex_init(&cl_thread_list_lock);
    cl_thread_list = NULL;
    cl_thread_count = 0;

    /* Initialize GC coordination */
    platform_mutex_init(&gc_mutex);
    platform_condvar_init(&gc_condvar);

    /* The main thread's MP park handle and serial (workers get theirs in
     * thread_entry / cl_thread_alloc_worker_sized).  A failed init leaves
     * park NULL: the lock and condvar slow paths then sleep-poll. */
    cl_thread_serial_counter = 1;
    cl_main_thread.serial = 1;
    cl_main_thread.park = NULL;
    if (platform_park_init(&cl_main_thread.park) != 0)
        cl_main_thread.park = NULL;

    /* Allocate NLX stack */
    cl_main_thread.nlx_stack = (CL_NLXFrame *)platform_alloc(
        CL_MAX_NLX_FRAMES * sizeof(CL_NLXFrame));
    cl_main_thread.nlx_max = CL_MAX_NLX_FRAMES;
    cl_main_thread.nlx_top = 0;

    /* Allocate saved pending-throw stack */
    cl_main_thread.saved_pending_stack = (CL_SavedPending *)platform_alloc(
        CL_MAX_SAVED_PENDING * sizeof(CL_SavedPending));
    cl_main_thread.saved_pending_max = CL_MAX_SAVED_PENDING;
    cl_main_thread.saved_pending_top = 0;

    /* Default: single-value mode */
    cl_main_thread.mv_count = 1;

    /* Mark as running */
    cl_main_thread.id = 0;
    cl_main_thread.status = 1; /* running */
    cl_main_thread.gc_live = 1; /* main thread always participates in STW */

    /* Initialize side tables */
    memset(cl_thread_table, 0, sizeof(cl_thread_table));

    /* Register main thread in both registry and side table */
    cl_thread_register(&cl_main_thread);
    cl_thread_table[0] = &cl_main_thread;
    cl_main_thread_registered = 1;
}

/* Restore the main task's TLS slot (tc_UserData) to the value it held before
 * cl_thread_init overwrote it.  Must run before the process exits so the C
 * runtime's post-main teardown reads its own per-task context rather than our
 * CL_Thread* (see cl_thread_init).  Idempotent and safe on every platform —
 * EXCEPT that platform_tls_set() always writes the *calling* task/thread's
 * slot, so this must only be called from the main thread: calling it from a
 * worker (e.g. a fatal-exit path reached on a worker thread while others are
 * still registered) would stomp that worker's own TLS slot with the main
 * thread's saved value instead, corrupting cl_get_current_thread()'s TLS
 * fallback for it (used whenever cl_thread_count > 1) rather than restoring
 * anything.  Guard against that misuse here so every call site (including
 * fatal-exit paths that can run on any thread) is safe unconditionally. */
void cl_thread_restore_main_tls(void)
{
    if (CT != cl_main_thread_ptr) return;
    platform_tls_set(cl_saved_main_tls);
}

void cl_thread_reap_zombies(void)
{
    CL_Thread *zombie;
    int i;

    if (!cl_thread_list_lock)
        return;
    platform_mutex_lock(cl_thread_list_lock);
    for (i = 1; i < CL_MAX_THREADS; i++) {
        zombie = cl_thread_table[i];
        if (!zombie || zombie->status < 2) continue;
        /* A claimed join owns this worker's cleanup: the joiner is (or
         * will be) parked in platform_thread_join on the claimed handle
         * and frees the worker itself.  Freeing it here would be a
         * use-after-free / double free. */
        if (zombie->join_in_progress) continue;
        cl_thread_table[i] = NULL;
        if (zombie->platform_handle) {
            platform_thread_detach(zombie->platform_handle);
            zombie->platform_handle = NULL;
        }
        cl_thread_free_worker(zombie);
    }
    platform_mutex_unlock(cl_thread_list_lock);
}

void cl_thread_shutdown(void)
{
    /* Clear BEFORE unregistering/freeing anything: a callback that lands on
     * the main task/thread from this point on must see "not registered"
     * from cl_thread_current_is_registered's fast path, not a stale "yes"
     * from cl_main_thread_ptr identity alone (which stays valid forever). */
    cl_main_thread_registered = 0;

    /* Unregister main thread */
    cl_thread_unregister(&cl_main_thread);

    /* Let workers that already left the registry finish exiting.  Such a
     * worker is still running its exit tail — it takes the registry lock
     * this function destroys below, and on AmigaOS it executes code that
     * is unloaded once main returns (nothing kills a straggling task
     * there; it crashes in freed memory).  cl_thread_count now counts only
     * the workers still registered, which are not waited for.  The tail
     * takes microseconds; the bound only guards against a wedged one.
     *
     * Pass cl_thread_count BY ADDRESS, not a value snapshotted here: a
     * still-registered worker counted into that snapshot can itself
     * unregister (dropping cl_thread_count) while this poll is still
     * waiting out an earlier straggler.  A frozen snapshot would then treat
     * that worker's own still-running exit tail as "drained" the instant
     * the earlier straggler's tail ends, instead of waiting for it too —
     * platform_thread_drain re-reads *keep on every poll so the wait
     * target tracks the live registered count. */
    {
        uint32_t left = platform_thread_drain(&cl_thread_count, 2000);
        if (left)
            fprintf(stderr, "[MP] shutdown: %u exiting worker thread(s) did "
                    "not finish within 2 s\n", (unsigned)left);
    }

    /* Workers that finished but were never joined -- a thread the program
     * only polled with THREAD-ALIVE-P, whose wrapper something still
     * reaches -- keep their VM stack, frames, NLX and saved-pending stacks
     * until MAKE-THREAD finds the table full or the wrapper's finalizer
     * runs, neither of which happens at exit.  Hand them back now, while
     * the registry lock still exists; every one is off-heap memory that
     * AmigaOS would never reclaim (the Clamacs editor exited with four
     * such threads, 4.8 MB, before this reap: its port's listener and
     * connection threads, the self transport's worker, the REPL thread). */
    cl_thread_reap_zombies();

    /* Main's MP park handle: unregistered above, so no release scan or
     * notify can find it, and interrupt delivery only unparks a thread
     * with a live wait registration (main has none here).  Clear the
     * pointer under the list lock so a peer that read it under the lock
     * cannot see a torn state, then free it — on AmigaOS the signal bit
     * must be freed by the main task itself, which this is. */
    if (cl_main_thread.park) {
        void *p = cl_main_thread.park;
        if (cl_thread_list_lock) platform_mutex_lock(cl_thread_list_lock);
        cl_main_thread.park = NULL;
        if (cl_thread_list_lock) platform_mutex_unlock(cl_thread_list_lock);
        platform_park_destroy(p);
    }

    if (cl_main_thread.saved_pending_stack) {
        platform_free(cl_main_thread.saved_pending_stack);
        cl_main_thread.saved_pending_stack = NULL;
    }
    if (cl_main_thread.nlx_stack) {
        platform_free(cl_main_thread.nlx_stack);
        cl_main_thread.nlx_stack = NULL;
    }
    /* Release any grown backtrace block, but leave the buffer pointing at the
     * inline one — the crash handler below still prints cl_backtrace_buf. */
    cl_thread_backtrace_release(&cl_main_thread);
    /* Keep cl_main_thread_ptr valid — the crash handler accesses
     * thread state (cl_vm.sp, etc.) via the CT macro.  Setting it
     * to NULL would cause a SIGSEGV in the handler itself. */

    /* With worker threads still registered, destroying the GC/registry
     * primitives yanks them out from under live code: the next worker
     * safepoint locks a destroyed gc_mutex (UB on pthreads, a NULL
     * ObtainSemaphore on Amiga), and its exit path locks a destroyed
     * cl_thread_list_lock.  Deliberately LEAK them instead: the process is
     * exiting, and a handful of orphaned OS primitives is a far better
     * outcome than a crash in teardown.  (On AmigaOS the leak is real — the
     * system does not reclaim a process's memory — which is why this path
     * warns and why main() reports it separately in the CLAMIGA_MEM_DIAG
     * line rather than counting it as a clean exit.) */
    if (cl_thread_count > 0) {
        static int warned = 0;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "[MP] shutdown with %u worker thread(s) still "
                    "running - leaking GC/thread primitives (safe; process "
                    "is exiting)\n", (unsigned)cl_thread_count);
        }
        return;
    }

    /* Destroy GC coordination */
    if (gc_condvar) {
        platform_condvar_destroy(gc_condvar);
        gc_condvar = NULL;
    }
    if (gc_mutex) {
        platform_mutex_destroy(gc_mutex);
        gc_mutex = NULL;
    }

    /* Destroy thread registry lock */
    if (cl_thread_list_lock) {
        platform_mutex_destroy(cl_thread_list_lock);
        cl_thread_list_lock = NULL;
    }

    /* MP locks and condition variables own no OS primitive (they are plain
     * heap words, specs/mp-locks-heap-words.md), so there is nothing of
     * theirs to hand back here. */
}
