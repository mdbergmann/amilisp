/*
 * test_gc_threaded.c — Tests for Phase 2 GC coordination.
 *
 * Tests: concurrent allocation stress test, thread registry,
 * allocation mutex correctness, safepoint/STW GC under contention.
 */

#include "test.h"
#include "core/types.h"
#include "core/mem.h"
#include "core/error.h"
#include "core/package.h"
#include "core/symbol.h"
#include "core/thread.h"
#include "core/stream.h"
#include "platform/platform.h"
#include "platform/platform_thread.h"

#include <string.h>
#include <stdlib.h>   /* abort() — watchdog escalation on deadlock */
#include <stdio.h>    /* fgets/stdin redirection for the read_line regression */
#include <unistd.h>   /* pipe/dup/dup2 — redirect stdin in the read_line test */

/* Undef the gc_root_count macro so we can access the struct field directly
 * on CL_Thread instances that aren't the current thread. */
#undef gc_root_count

static void setup(void)
{
    platform_init();
    cl_thread_init();
    cl_error_init();
    cl_mem_init(4 * 1024 * 1024);  /* 4MB heap */
    cl_package_init();
    cl_symbol_init();
}

static void teardown(void)
{
    cl_mem_shutdown();
    cl_thread_shutdown();
    platform_shutdown();
}

/* A mid-process heap re-init frees the arena the packages and the boot
 * symbols live in, but the SYM_* / KW_* globals keep their old offsets.
 * The next code path that stores one of them into a live object — every
 * cl_error does, through cl_create_condition_from_error's type symbol —
 * plants a stale offset the next mark walks ([GC-BADMARK] in
 * error_unwind_restores_gc_roots_nested under DEBUG_GC, once the new
 * arena's bump front had grown past the old SYM_SIMPLE_ERROR offset).
 * So a re-init here always re-runs the package + symbol init, the same
 * contract main() and tests/test_gc_immix.c follow. */
static void reinit_heap(uint32_t size)
{
    cl_mem_shutdown();
    cl_mem_init(size);
    cl_package_init();
    cl_symbol_init();
}

/* ================================================================
 * Thread registry tests
 * ================================================================ */

TEST(thread_registry_main_registered)
{
    /* Main thread should be registered during cl_thread_init */
    ASSERT(cl_thread_list != NULL);
    ASSERT_EQ_INT((int)cl_thread_count, 1);
    ASSERT(cl_thread_list == cl_main_thread_ptr);
}

TEST(thread_registry_add_remove)
{
    CL_Thread extra;
    memset(&extra, 0, sizeof(CL_Thread));
    extra.id = 99;

    cl_thread_register(&extra);
    ASSERT_EQ_INT((int)cl_thread_count, 2);

    cl_thread_unregister(&extra);
    ASSERT_EQ_INT((int)cl_thread_count, 1);
}

/* ================================================================
 * Single-thread allocation under mutex (regression: must not deadlock)
 * ================================================================ */

TEST(alloc_single_thread_under_mutex)
{
    /* Allocating many objects should work fine with the alloc_mutex */
    int i;
    CL_Obj list = CL_NIL;
    CL_GC_PROTECT(list);

    for (i = 0; i < 1000; i++) {
        list = cl_cons(CL_MAKE_FIXNUM(i), list);
    }

    ASSERT(!CL_NULL_P(list));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(list)), 999);

    CL_GC_UNPROTECT(1);
}

/* ================================================================
 * Single-thread GC under new STW path (regression)
 * ================================================================ */

TEST(gc_single_thread_stw)
{
    /* Force GC multiple times — should work with STW path (no-op for 1 thread) */
    int i;
    CL_Obj list = CL_NIL;
    CL_GC_PROTECT(list);

    for (i = 0; i < 500; i++) {
        list = cl_cons(CL_MAKE_FIXNUM(i), list);
    }

    cl_gc();  /* Explicit GC */
    cl_gc();  /* Again */

    /* List should survive GC */
    ASSERT(!CL_NULL_P(list));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(list)), 499);

    CL_GC_UNPROTECT(1);
}

/* ================================================================
 * Concurrent allocation stress test
 *
 * Multiple threads allocate cons cells concurrently.  This exercises:
 * - alloc_mutex correctness (no heap corruption)
 * - Safepoint in cl_alloc()
 * - STW GC with multiple registered threads
 *
 * Each thread has its own CL_Thread with its own gc_roots, so
 * GC root protection works per-thread.
 * ================================================================ */

/* Per-thread work context */
typedef struct {
    CL_Thread    thread;
    int          alloc_count;  /* how many conses to allocate */
    int          result_ok;    /* 1 if validation passed */
} WorkerCtx;

static void *concurrent_allocator(void *arg)
{
    WorkerCtx *ctx = (WorkerCtx *)arg;
    CL_Thread *t = &ctx->thread;
    int i;
    CL_Obj list = CL_NIL;
    CL_Obj *root_ptr;

    /* Set up this thread's TLS and register */
    platform_tls_set(t);
    cl_thread_register(t);

    /* GC-protect our list root */
    root_ptr = &list;
    t->gc_roots[0] = root_ptr;
    t->gc_root_count = 1;

    /* Allocate cons cells */
    for (i = 0; i < ctx->alloc_count; i++) {
        list = cl_cons(CL_MAKE_FIXNUM(i), list);
    }

    /* Validate the list */
    ctx->result_ok = 1;
    {
        CL_Obj walk = list;
        int expected = ctx->alloc_count - 1;
        while (!CL_NULL_P(walk)) {
            if (!CL_CONS_P(walk) || CL_FIXNUM_VAL(cl_car(walk)) != expected) {
                ctx->result_ok = 0;
                break;
            }
            walk = cl_cdr(walk);
            expected--;
        }
        if (expected != -1)
            ctx->result_ok = 0;
    }

    t->gc_root_count = 0;
    cl_thread_unregister(t);
    return NULL;
}

TEST(concurrent_alloc_2_threads)
{
    void *handles[2];
    WorkerCtx workers[2];
    int i;

    for (i = 0; i < 2; i++) {
        memset(&workers[i], 0, sizeof(WorkerCtx));
        workers[i].thread.id = (uint32_t)(i + 1);
        workers[i].thread.status = 1;
        workers[i].thread.mv_count = 1;
        workers[i].alloc_count = 200;
        workers[i].result_ok = 0;
    }

    for (i = 0; i < 2; i++)
        platform_thread_create(&handles[i], concurrent_allocator, &workers[i], 0);

    for (i = 0; i < 2; i++)
        platform_thread_join(handles[i], NULL);

    for (i = 0; i < 2; i++)
        ASSERT_EQ_INT(workers[i].result_ok, 1);
}

TEST(concurrent_alloc_4_threads)
{
    void *handles[4];
    WorkerCtx workers[4];
    int i;

    for (i = 0; i < 4; i++) {
        memset(&workers[i], 0, sizeof(WorkerCtx));
        workers[i].thread.id = (uint32_t)(i + 1);
        workers[i].thread.status = 1;
        workers[i].thread.mv_count = 1;
        workers[i].alloc_count = 100;
        workers[i].result_ok = 0;
    }

    for (i = 0; i < 4; i++)
        platform_thread_create(&handles[i], concurrent_allocator, &workers[i], 0);

    for (i = 0; i < 4; i++)
        platform_thread_join(handles[i], NULL);

    for (i = 0; i < 4; i++)
        ASSERT_EQ_INT(workers[i].result_ok, 1);
}

/* ================================================================
 * Concurrent allocation with forced GC
 *
 * Use a small heap so GC is triggered frequently, exercising the
 * STW path under contention.
 * ================================================================ */

TEST(concurrent_alloc_with_gc)
{
    void *handles[3];
    WorkerCtx workers[3];
    int i;

    /* Shrink heap to force frequent GC */
    reinit_heap(256 * 1024);  /* 256KB — forces GC during concurrent alloc */

    for (i = 0; i < 3; i++) {
        memset(&workers[i], 0, sizeof(WorkerCtx));
        workers[i].thread.id = (uint32_t)(i + 1);
        workers[i].thread.status = 1;
        workers[i].thread.mv_count = 1;
        workers[i].alloc_count = 50;  /* modest count to avoid exhaustion */
        workers[i].result_ok = 0;
    }

    for (i = 0; i < 3; i++)
        platform_thread_create(&handles[i], concurrent_allocator, &workers[i], 0);

    for (i = 0; i < 3; i++)
        platform_thread_join(handles[i], NULL);

    for (i = 0; i < 3; i++)
        ASSERT_EQ_INT(workers[i].result_ok, 1);

    /* Restore normal heap */
    reinit_heap(4 * 1024 * 1024);
}

/* ================================================================
 * GC-epoch dedup (cl_gc_if_stale)
 *
 * When N threads exhaust the bump front near-simultaneously, each loser
 * of the stop-the-world initiator race used to run its own full
 * mark-sweep on the heap a peer had JUST swept.  cl_gc_if_stale skips
 * the collection when the GC epoch advanced before this thread became
 * initiator.  The skip requires a multi-threaded world (single-threaded
 * there is no race, so it must always collect).
 *
 * These tests (and the phase-timer test below) pin the CLASSIC collector
 * semantics: under the generational collector cl_gc()/cl_gc_if_stale
 * route to compaction and the allocation path dedups via cl_gc_minor
 * instead.  reinit_classic()/reinit_default() swap collectors by
 * re-initializing the heap around them.
 * ================================================================ */

static void reinit_classic(void)
{
    setenv("CLAMIGA_GENGC", "0", 1);
    reinit_heap(4 * 1024 * 1024);
}

static void reinit_default(void)
{
    unsetenv("CLAMIGA_GENGC");
    reinit_heap(4 * 1024 * 1024);
}

#ifdef CL_GC_CLASSIC   /* cl_gc_if_stale is the classic backend's entry point */
TEST(gc_if_stale_single_thread_always_collects)
{
    uint32_t before = cl_heap.gc_count;

    /* Current epoch: collects */
    ASSERT_EQ_INT(cl_gc_if_stale(before), 1);
    ASSERT_EQ_INT((int)(cl_heap.gc_count - before), 1);

    /* Stale epoch, but single-threaded — no peer can have collected for
     * us, so it must STILL collect (the dedup is MT-only by design). */
    ASSERT_EQ_INT(cl_gc_if_stale(before), 1);
    ASSERT_EQ_INT((int)(cl_heap.gc_count - before), 2);
}
#endif /* CL_GC_CLASSIC */

/* Peer that parks inside a GC safe region until released, making the
 * process multi-threaded (cl_thread_count > 1) without ever touching the
 * heap — the main thread can then run STW GCs deterministically. */
typedef struct {
    CL_Thread    thread;
    volatile int ready;
    volatile int hold;
} SafeParkCtx;

static void *safe_region_parker(void *arg)
{
    SafeParkCtx *ctx = (SafeParkCtx *)arg;
    platform_tls_set(&ctx->thread);
    cl_thread_register(&ctx->thread);
    cl_gc_enter_safe_region();
    ctx->ready = 1;
    while (ctx->hold)
        platform_thread_yield();
    cl_gc_leave_safe_region();
    cl_thread_unregister(&ctx->thread);
    return NULL;
}

#ifdef CL_GC_CLASSIC
TEST(gc_if_stale_skips_when_epoch_advanced)
{
    SafeParkCtx ctx;
    void *handle;
    uint32_t seen, after_peer_gc, skips_before, skips_after;

    memset(&ctx, 0, sizeof(ctx));
    ctx.thread.id = 91;
    ctx.thread.status = 1;
    ctx.thread.mv_count = 1;
    ctx.hold = 1;
    platform_thread_create(&handle, safe_region_parker, &ctx, 0);
    while (!ctx.ready)
        platform_thread_yield();

    /* Record the epoch a hypothetical allocator saw, then let "a peer"
     * (here: this thread, standing in for one) collect — the epoch
     * advances past what the caller observed. */
    seen = cl_heap.gc_count;
    cl_gc();
    after_peer_gc = cl_heap.gc_count;
    ASSERT_EQ_INT((int)(after_peer_gc - seen), 1);

    /* Stale epoch + multi-threaded → the redundant collection is skipped
     * and counted. */
    cl_gc_stw_stats(NULL, NULL, &skips_before);
    ASSERT_EQ_INT(cl_gc_if_stale(seen), 0);
    ASSERT_EQ_INT((int)(cl_heap.gc_count - after_peer_gc), 0);
    cl_gc_stw_stats(NULL, NULL, &skips_after);
    ASSERT_EQ_INT((int)(skips_after - skips_before), 1);

    /* Fresh epoch → collects normally. */
    ASSERT_EQ_INT(cl_gc_if_stale(cl_heap.gc_count), 1);
    ASSERT_EQ_INT((int)(cl_heap.gc_count - after_peer_gc), 1);

    ctx.hold = 0;
    platform_thread_join(handle, NULL);
}

/* ================================================================
 * GC phase timers (cl_gc_time_stats / cl_gc_stw_stats) — the stats
 * exposed by (ext:%gc-time-stats).  Verifies the counters/timers move in
 * the expected phase when a sweep GC, a compaction, and a stop-the-world
 * event each run, and stay put otherwise (e.g. compact_us must not move
 * on a plain sweep GC).
 * ================================================================ */
#endif /* CL_GC_CLASSIC */

TEST(gc_time_stats_and_stw_stats_accumulate)
{
    SafeParkCtx ctx;
    void *handle;
    CL_Obj garbage;
    uint64_t stw0, mark0, sweep0, compact0;
    uint64_t stw1, mark1, sweep1, compact1;
    uint32_t stops0, skips0, stops1, skips1;
    uint64_t stw_max0, stw_max1;
    uint32_t gc_before, compact_before;
    int i;

    /* Sweep GC: mark_us/sweep_us accumulate, compact_us/STW stats do not.
     * Cons real garbage first so mark/sweep have measurable work to do. */
    garbage = CL_NIL;
    CL_GC_PROTECT(garbage);
    for (i = 0; i < 2000; i++)
        garbage = cl_cons(CL_MAKE_FIXNUM(i), garbage);
    garbage = CL_NIL;  /* drop it — next GCs reclaim it as real garbage */

    cl_gc_time_stats(&stw0, &mark0, &sweep0, &compact0);
    cl_gc_stw_stats(&stops0, &stw_max0, &skips0);
    gc_before = cl_heap.gc_count;

    for (i = 0; i < 20; i++)
        cl_gc();
    ASSERT_EQ_INT((int)(cl_heap.gc_count - gc_before), 20);

    cl_gc_time_stats(&stw1, &mark1, &sweep1, &compact1);
    ASSERT(mark1 + sweep1 > mark0 + sweep0);
    ASSERT_EQ(compact1, compact0);

    cl_gc_stw_stats(&stops1, &stw_max1, &skips1);
    ASSERT_EQ_INT((int)(stops1 - stops0), 0);  /* single-threaded: no STW event */
    CL_GC_UNPROTECT(1);

    /* Compaction: compact_us and compact_count move; mark/sweep untouched
     * (cl_gc_compact runs its own internal mark/sweep, counted separately). */
    compact_before = cl_heap.compact_count;
    cl_gc_time_stats(&stw0, &mark0, &sweep0, &compact0);
    cl_gc_compact();
    ASSERT_EQ_INT((int)(cl_heap.compact_count - compact_before), 1);
    cl_gc_time_stats(&stw1, &mark1, &sweep1, &compact1);
    ASSERT(compact1 > compact0);
    ASSERT_EQ(mark1, mark0);
    ASSERT_EQ(sweep1, sweep0);

    /* Stop-the-world: a parked peer thread makes cl_gc() take the real STW
     * path — stops/stw_us move. */
    memset(&ctx, 0, sizeof(ctx));
    ctx.thread.id = 92;
    ctx.thread.status = 1;
    ctx.thread.mv_count = 1;
    ctx.hold = 1;
    platform_thread_create(&handle, safe_region_parker, &ctx, 0);
    while (!ctx.ready)
        platform_thread_yield();

    cl_gc_stw_stats(&stops0, &stw_max0, &skips0);
    cl_gc_time_stats(&stw0, NULL, NULL, NULL);
    cl_gc();
    cl_gc_stw_stats(&stops1, &stw_max1, &skips1);
    cl_gc_time_stats(&stw1, NULL, NULL, NULL);
    ASSERT_EQ_INT((int)(stops1 - stops0), 1);
    /* >=: with the peer already parked in its safe region the stop can
     * complete inside one microsecond tick, so the timer may not move. */
    ASSERT(stw1 >= stw0);

    ctx.hold = 0;
    platform_thread_join(handle, NULL);
}

/* ================================================================
 * Safepoint smoke test — verify CL_SAFEPOINT() doesn't crash
 * when gc_requested is 0
 * ================================================================ */

TEST(safepoint_no_gc)
{
    CL_SAFEPOINT();
    CL_SAFEPOINT();
    CL_SAFEPOINT();
    ASSERT(1);  /* If we get here, safepoints are no-ops when no GC requested */
}

/* ================================================================
 * Regression: stop-the-world GC must not deadlock behind a thread
 * parked in a blocking socket syscall.
 *
 * A thread blocked in accept()/read()/write() cannot reach a GC
 * safepoint, so the platform socket layer brackets each blocking
 * syscall with cl_gc_enter_safe_region()/cl_gc_leave_safe_region().
 * Without that bracketing, a full GC from any other thread waits
 * forever for the parked thread to stop (the SLY :spawn deadlock).
 *
 * The worker parks in accept(); the main thread then runs a full
 * STW cl_gc().  A watchdog thread aborts with a diagnostic if the
 * GC does not return within a few seconds, turning a regression
 * into a loud test failure instead of an indefinite hang.
 * ================================================================ */

typedef struct {
    CL_Thread       thread;
    PlatformSocket  listener;
    volatile int    parked;     /* set just before entering accept() */
} AcceptCtx;

static volatile int g_stw_gc_done = 0;

static void *deadlock_watchdog(void *arg)
{
    int waited_ms = 0;
    (void)arg;
    while (!g_stw_gc_done && waited_ms < 5000) {
        platform_sleep_ms(50);
        waited_ms += 50;
    }
    if (!g_stw_gc_done) {
        fprintf(stderr,
            "\nFATAL: stop-the-world GC deadlocked behind a thread blocked in "
            "socket accept().\n  The GC safe-region bracketing around blocking "
            "socket syscalls has regressed\n  (see platform_socket_accept/read/"
            "write in platform_posix.c / platform_amiga.c).\n");
        abort();
    }
    return NULL;
}

static void *accept_blocker(void *arg)
{
    AcceptCtx *ctx = (AcceptCtx *)arg;
    CL_Thread *t = &ctx->thread;
    PlatformSocket conn;

    platform_tls_set(t);
    cl_thread_register(t);

    ctx->parked = 1;
    /* Blocks until the main thread connects.  With the fix this runs inside a
     * GC safe region, so a concurrent STW GC counts this thread as stopped. */
    conn = platform_socket_accept(ctx->listener);
    if (conn != PLATFORM_SOCKET_INVALID)
        platform_socket_close(conn);

    cl_thread_unregister(t);
    return NULL;
}

TEST(stw_gc_with_thread_blocked_in_accept)
{
    void *whandle, *wdog;
    AcceptCtx ctx;
    int port = 0;
    PlatformSocket listener, client;

    listener = platform_socket_listen(0, 1, &port);  /* port 0 = ephemeral, loopback */
    ASSERT(listener != PLATFORM_SOCKET_INVALID);
    ASSERT(port > 0);

    memset(&ctx, 0, sizeof(ctx));
    ctx.thread.id = 1;
    ctx.thread.status = 1;
    ctx.thread.mv_count = 1;
    ctx.listener = listener;

    g_stw_gc_done = 0;
    platform_thread_create(&wdog, deadlock_watchdog, NULL, 0);
    platform_thread_create(&whandle, accept_blocker, &ctx, 0);

    /* Wait until the worker is about to block in accept(), then give it a beat
     * to actually park inside the syscall. */
    while (!ctx.parked)
        platform_sleep_ms(5);
    platform_sleep_ms(100);

    /* The operation that used to hang forever. */
    cl_gc();
    g_stw_gc_done = 1;  /* tell the watchdog we survived */

    /* Unblock the worker so it can finish, then join everything. */
    client = platform_socket_connect("127.0.0.1", port, 0);
    platform_thread_join(whandle, NULL);
    platform_thread_join(wdog, NULL);

    if (client != PLATFORM_SOCKET_INVALID)
        platform_socket_close(client);
    platform_socket_close(listener);

    ASSERT(1);  /* reaching here means GC returned — no deadlock */
}

/* ================================================================
 * Regression: stop-the-world GC must not deadlock behind a thread
 * parked in the blocking stdin read (platform_read_line).
 *
 * This is the SLY :spawn REPL hang.  The main thread parks in
 * cl_repl()'s fgets (under a `tail -f /dev/null | amilisp` launcher it
 * never gets a line), while a spawned worker printing a backtrace into
 * SLDB allocates and fires the first stop-the-world GC.  Unless
 * platform_read_line brackets its fgets with the GC safe region, the
 * main thread is neither stopped nor in a safe region, so STW waits
 * forever for a safepoint it can never reach.
 *
 * The worker parks in platform_read_line() reading an empty pipe wired
 * onto stdin; the main thread then runs a full STW cl_gc().  A watchdog
 * aborts with a diagnostic if the GC does not return within a few
 * seconds, turning a regression into a loud failure instead of a hang.
 * ================================================================ */

typedef struct {
    CL_Thread    thread;
    volatile int parked;
} ReadLineCtx;

static volatile int g_readline_gc_done = 0;

static void *readline_deadlock_watchdog(void *arg)
{
    int waited_ms = 0;
    (void)arg;
    while (!g_readline_gc_done && waited_ms < 5000) {
        platform_sleep_ms(50);
        waited_ms += 50;
    }
    if (!g_readline_gc_done) {
        fprintf(stderr,
            "\nFATAL: stop-the-world GC deadlocked behind a thread blocked in "
            "platform_read_line()\n  (the blocking stdin fgets/FGets).  The GC "
            "safe-region bracketing has regressed\n  (see platform_read_line in "
            "platform_posix.c / platform_amiga.c).  This is the SLY :spawn REPL "
            "hang.\n");
        abort();
    }
    return NULL;
}

static void *readline_blocker(void *arg)
{
    ReadLineCtx *ctx = (ReadLineCtx *)arg;
    CL_Thread *t = &ctx->thread;
    char buf[64];

    platform_tls_set(t);
    cl_thread_register(t);

    ctx->parked = 1;
    /* Blocks reading stdin (redirected to an empty pipe).  With the fix this
     * runs inside a GC safe region, so a concurrent STW GC counts this thread
     * as stopped. */
    platform_read_line(buf, sizeof(buf));

    cl_thread_unregister(t);
    return NULL;
}

TEST(stw_gc_with_thread_blocked_in_read_line)
{
    void *whandle, *wdog;
    ReadLineCtx ctx;
    int pipefd[2];
    int saved_stdin;

    /* Redirect stdin to the read end of an empty pipe so the worker's
     * platform_read_line() blocks until we choose to unblock it. */
    ASSERT_EQ_INT(pipe(pipefd), 0);
    saved_stdin = dup(STDIN_FILENO);
    ASSERT(saved_stdin >= 0);
    ASSERT(dup2(pipefd[0], STDIN_FILENO) >= 0);
    clearerr(stdin);

    memset(&ctx, 0, sizeof(ctx));
    ctx.thread.id = 1;
    ctx.thread.status = 1;
    ctx.thread.mv_count = 1;

    g_readline_gc_done = 0;
    platform_thread_create(&wdog, readline_deadlock_watchdog, NULL, 0);
    platform_thread_create(&whandle, readline_blocker, &ctx, 0);

    /* Wait until the worker is about to block, then give it a beat to actually
     * park inside fgets. */
    while (!ctx.parked)
        platform_sleep_ms(5);
    platform_sleep_ms(100);

    /* The operation that used to hang forever. */
    cl_gc();
    g_readline_gc_done = 1;  /* tell the watchdog we survived */

    /* Unblock the worker: closing the write end makes fgets return EOF. */
    close(pipefd[1]);
    platform_thread_join(whandle, NULL);
    platform_thread_join(wdog, NULL);

    /* Restore the real stdin. */
    dup2(saved_stdin, STDIN_FILENO);
    close(saved_stdin);
    close(pipefd[0]);
    clearerr(stdin);

    ASSERT(1);  /* reaching here means GC returned — no deadlock */
}

/* ================================================================
 * Regression: stop-the-world GC must not deadlock behind a thread
 * BLOCKED ON an internal I/O lock that is held across a GC safe region.
 *
 * The hazard (the rare extreme-concurrency STW deadlock): thread A
 * holds a stream I/O lock (e.g. a per-socket write lock) and enters a
 * GC safe region for the blocking write() syscall — so STW counts A as
 * stopped.  Thread B wants the SAME lock and blocks in
 * platform_mutex_lock(), which is neither a safepoint nor a safe region,
 * so a peer's STW waits for B forever; meanwhile A returns from the
 * syscall and parks in cl_gc_leave_safe_region() *still holding the
 * lock*.  Nobody makes progress.
 *
 * Fix: internal stream locks are acquired via cl_gc_safe_mutex_lock(),
 * which brackets the contended blocking wait in a safe region so STW
 * counts the waiter as stopped.  This test drives that path directly: A
 * takes the lock and enters a safe region (modelling the blocking
 * write); B contends via cl_gc_safe_mutex_lock(); the main thread then
 * runs a full STW cl_gc(), which must return.  A watchdog aborts on hang
 * so a regression is a loud failure, not an indefinite stall.
 * ================================================================ */

typedef struct {
    CL_Thread    thread;
    void        *lock;
    volatile int holding;     /* A: acquired lock + entered safe region */
    volatile int contending;  /* B: about to block on the held lock */
    volatile int release;     /* main -> A: leave safe region + unlock */
    volatile int done;
} IoLockCtx;

static volatile int g_iolock_gc_done = 0;

static void *iolock_deadlock_watchdog(void *arg)
{
    int waited_ms = 0;
    (void)arg;
    while (!g_iolock_gc_done && waited_ms < 5000) {
        platform_sleep_ms(50);
        waited_ms += 50;
    }
    if (!g_iolock_gc_done) {
        fprintf(stderr,
            "\nFATAL: stop-the-world GC deadlocked behind a thread blocked on an "
            "internal I/O lock\n  held across a GC safe region.  This is the rare "
            "extreme-concurrency STW deadlock:\n  cl_gc_safe_mutex_lock() has "
            "regressed (see thread.c), or a stream lock site is\n  back to a plain "
            "platform_mutex_lock() (see stream.c).\n");
        abort();
    }
    return NULL;
}

/* A: hold the lock and sit in a safe region, as a blocking socket write would. */
static void *iolock_holder(void *arg)
{
    IoLockCtx *ctx = (IoLockCtx *)arg;
    CL_Thread *t = &ctx->thread;
    platform_tls_set(t);
    cl_thread_register(t);

    cl_gc_safe_mutex_lock(ctx->lock);   /* uncontended: fast path, no safe region */
    cl_gc_enter_safe_region();          /* model the blocking write() syscall */
    ctx->holding = 1;
    while (!ctx->release)
        platform_sleep_ms(5);
    cl_gc_leave_safe_region();          /* "syscall" returns */
    platform_mutex_unlock(ctx->lock);

    cl_thread_unregister(t);
    ctx->done = 1;
    return NULL;
}

/* B: contend for the SAME lock.  cl_gc_safe_mutex_lock must put it into a safe
 * region while blocked so a concurrent STW counts it as stopped. */
static void *iolock_contender(void *arg)
{
    IoLockCtx *ctx = (IoLockCtx *)arg;
    CL_Thread *t = &ctx->thread;
    platform_tls_set(t);
    cl_thread_register(t);

    ctx->contending = 1;
    cl_gc_safe_mutex_lock(ctx->lock);   /* contended: blocks GC-cooperatively */
    platform_mutex_unlock(ctx->lock);

    cl_thread_unregister(t);
    ctx->done = 1;
    return NULL;
}

TEST(stw_gc_with_thread_blocked_on_io_lock)
{
    void *ah, *bh, *wdog;
    void *lock = NULL;
    IoLockCtx a, b;

    platform_mutex_init(&lock);
    ASSERT(lock != NULL);

    memset(&a, 0, sizeof(a));
    a.thread.id = 1; a.thread.status = 1; a.thread.mv_count = 1; a.lock = lock;
    memset(&b, 0, sizeof(b));
    b.thread.id = 2; b.thread.status = 1; b.thread.mv_count = 1; b.lock = lock;

    g_iolock_gc_done = 0;
    platform_thread_create(&wdog, iolock_deadlock_watchdog, NULL, 0);

    /* Start A and wait until it holds the lock inside a safe region. */
    platform_thread_create(&ah, iolock_holder, &a, 0);
    while (!a.holding)
        platform_sleep_ms(5);

    /* Start B and give it a beat to actually block on the held lock. */
    platform_thread_create(&bh, iolock_contender, &b, 0);
    while (!b.contending)
        platform_sleep_ms(5);
    platform_sleep_ms(100);

    /* The operation that used to hang forever: STW must count BOTH the
     * safe-region holder and the safe-region-blocked contender as stopped. */
    cl_gc();
    g_iolock_gc_done = 1;   /* tell the watchdog we survived */

    /* Let A release; B then acquires and releases in turn. */
    a.release = 1;
    platform_thread_join(ah, NULL);
    platform_thread_join(bh, NULL);
    platform_thread_join(wdog, NULL);

    platform_mutex_destroy(lock);

    ASSERT(a.done && b.done);
}

/* ================================================================
 * Regression: data reachable through a stream object relocated by a
 * compacting GC must stay correct across cl_gc_safe_mutex_lock's
 * contended path.
 *
 * The test above only proves stop-the-world GC doesn't deadlock behind
 * a thread blocked on a bare platform mutex.  It says nothing about
 * memory safety: cl_stream_read_char/write_char/write_byte/write_string/
 * close all derive `st` from the CL_Obj `stream` BEFORE calling
 * cl_gc_safe_mutex_lock(iolock), and that call's contended path parks
 * in a GC safe region.  Unless `stream` is GC-protected across the call
 * and `st` re-derived afterward, a peer's compaction mid-wait relocates
 * the stream object and every subsequent st-> access on the far side of
 * the lock reads/writes through a stale offset.
 *
 * This drives that exact race through the real cl_stream_read_char API
 * (not a bare mutex): A holds the shared console-read lock, parked in a
 * real blocking read (platform_getchar on a redirected, empty stdin
 * pipe).  B contends for the SAME lock, which blocks it inside
 * cl_gc_safe_mutex_lock's contended path.  While both are parked, the
 * main thread builds dead garbage ahead of the two stream objects and
 * forces a real compacting GC, so the compaction actually relocates
 * both.  A is then unblocked and B's contended lock acquisition follows;
 * both must read back the exact bytes fed into the pipe afterward, and
 * the stream objects must remain intact (TYPE_STREAM, correct
 * flags/unread_char) through the relocation.
 * ================================================================ */

typedef struct {
    CL_Thread    thread;
    CL_Obj       stream;
    volatile int about_to_block;  /* set right before the blocking call */
    volatile int done;
    int          ch;              /* result of cl_stream_read_char */
} StreamRaceCtx;

static volatile int g_stream_race_done = 0;

static void *stream_race_watchdog(void *arg)
{
    int waited_ms = 0;
    (void)arg;
    while (!g_stream_race_done && waited_ms < 5000) {
        platform_sleep_ms(50);
        waited_ms += 50;
    }
    if (!g_stream_race_done) {
        fprintf(stderr,
            "\nFATAL: a stream read blocked on a contended I/O lock did not "
            "complete correctly after a concurrent compacting GC.  The "
            "CL_GC_PROTECT around cl_gc_safe_mutex_lock in cl_stream_read_char "
            "has regressed (see stream.c).\n");
        abort();
    }
    return NULL;
}

/* A: hold the shared console-read lock and block in a real blocking read,
 * exactly like the production hazard this regression covers. */
static void *stream_race_holder(void *arg)
{
    StreamRaceCtx *ctx = (StreamRaceCtx *)arg;
    CL_Thread *t = &ctx->thread;
    platform_tls_set(t);
    cl_thread_register(t);

    ctx->about_to_block = 1;
    ctx->ch = cl_stream_read_char(ctx->stream);   /* blocks in platform_getchar() */

    cl_thread_unregister(t);
    ctx->done = 1;
    return NULL;
}

/* B: contend for the SAME lock via the real stream API.  With the fix,
 * cl_stream_read_char GC-protects its `stream` local across the contended
 * cl_gc_safe_mutex_lock() call, so a concurrent compaction relocating the
 * stream object doesn't leave `st` dangling once the lock is acquired. */
static void *stream_race_contender(void *arg)
{
    StreamRaceCtx *ctx = (StreamRaceCtx *)arg;
    CL_Thread *t = &ctx->thread;
    platform_tls_set(t);
    cl_thread_register(t);

    ctx->about_to_block = 1;
    ctx->ch = cl_stream_read_char(ctx->stream);

    cl_thread_unregister(t);
    ctx->done = 1;
    return NULL;
}

TEST(stream_read_survives_iolock_contention_and_compaction)
{
    void *ah, *bh, *wdog;
    StreamRaceCtx a, b;
    CL_Obj stream_a, stream_b;
    CL_Obj junk;
    int i;
    int pipefd[2];
    int saved_stdin;

    cl_stream_init();

    /* Redirect stdin to the read end of an empty pipe so both readers block
     * until we choose to feed bytes in. */
    ASSERT_EQ_INT(pipe(pipefd), 0);
    saved_stdin = dup(STDIN_FILENO);
    ASSERT(saved_stdin >= 0);
    ASSERT(dup2(pipefd[0], STDIN_FILENO) >= 0);
    clearerr(stdin);

    /* Dead garbage ahead of the two stream objects, so a forced compaction
     * actually slides them to a new offset instead of being a no-op. */
    junk = CL_NIL;
    for (i = 0; i < 512; i++)
        junk = cl_cons(CL_MAKE_FIXNUM(i), CL_NIL);
    (void)junk;   /* unreachable after the loop — pure compaction fodder */

    stream_a = cl_make_stream(CL_STREAM_INPUT, CL_STREAM_CONSOLE);
    CL_GC_PROTECT(stream_a);
    stream_b = cl_make_stream(CL_STREAM_INPUT, CL_STREAM_CONSOLE);
    CL_GC_PROTECT(stream_b);

    memset(&a, 0, sizeof(a));
    a.thread.id = 1; a.thread.status = 1; a.thread.mv_count = 1; a.stream = stream_a;
    memset(&b, 0, sizeof(b));
    b.thread.id = 2; b.thread.status = 1; b.thread.mv_count = 1; b.stream = stream_b;

    g_stream_race_done = 0;
    platform_thread_create(&wdog, stream_race_watchdog, NULL, 0);

    /* Start A and wait until it's about to block in the real getchar(). */
    platform_thread_create(&ah, stream_race_holder, &a, 0);
    while (!a.about_to_block)
        platform_sleep_ms(5);
    platform_sleep_ms(50);   /* let it actually reach platform_getchar() */

    /* Start B and give it a beat to actually block contending for the SAME
     * console-read lock A now holds. */
    platform_thread_create(&bh, stream_race_contender, &b, 0);
    while (!b.about_to_block)
        platform_sleep_ms(5);
    platform_sleep_ms(100);

    /* Force a real moving compaction while both are parked: A inside the
     * blocking read's safe region, B inside cl_gc_safe_mutex_lock's
     * contended-wait safe region.  This relocates both CL_Stream objects. */
    cl_gc_compact();

    /* Overwrite the space vacated by the slide — exactly where the two
     * streams used to sit — with different heap data.  Without this, a
     * stale (unprotected) `st` might coincidentally still read back
     * correct-looking bytes, since compaction only moves live data down and
     * doesn't clear what's left behind; a stale read only reliably shows up
     * once that space is reused for something else. */
    {
        CL_Obj poison = CL_NIL;
        for (i = 0; i < 4096; i++)
            poison = cl_cons(CL_MAKE_FIXNUM(0xBEEF), CL_NIL);
        (void)poison;
    }

    /* Feed both bytes at once: A's getchar() consumes the first as soon as
     * it wakes and releases the lock; B's getchar() (once it has acquired
     * the now-free lock) finds the second already buffered, so neither
     * blocks again. */
    ASSERT_EQ_INT((int)write(pipefd[1], "AB", 2), 2);

    platform_thread_join(ah, NULL);
    platform_thread_join(bh, NULL);
    g_stream_race_done = 1;   /* tell the watchdog we survived */
    platform_thread_join(wdog, NULL);

    close(pipefd[1]);
    dup2(saved_stdin, STDIN_FILENO);
    close(saved_stdin);
    close(pipefd[0]);
    clearerr(stdin);

    CL_GC_UNPROTECT(2);

    ASSERT(a.done && b.done);
    ASSERT_EQ_INT(a.ch, (int)'A');
    ASSERT_EQ_INT(b.ch, (int)'B');

    /* The (relocated) stream objects must still be intact — a stale `st`
     * after the contended lock would read/write through whatever now
     * occupies the old offset instead of the real stream. */
    ASSERT(CL_STREAM_P(stream_a));
    ASSERT(CL_STREAM_P(stream_b));
    {
        CL_Stream *sa = (CL_Stream *)CL_OBJ_TO_PTR(stream_a);
        CL_Stream *sb = (CL_Stream *)CL_OBJ_TO_PTR(stream_b);
        ASSERT(sa->flags & CL_STREAM_FLAG_OPEN);
        ASSERT(sb->flags & CL_STREAM_FLAG_OPEN);
        ASSERT_EQ_INT(sa->unread_char, -1);
        ASSERT_EQ_INT(sb->unread_char, -1);
    }
}

/* ================================================================
 * Regression: error unwind through a GC-protected C frame must
 * restore gc_root_count (the "sento gc_mark SEGV").
 *
 * The original bug: a C builtin such as bi_map does CL_GC_PROTECT on
 * its result/tail accumulators before a loop, then a nested call
 * signals an error mid-loop.  cl_error_unwind longjmps back to the
 * enclosing CL_CATCH but, on the *nested* error-frame path, did NOT
 * restore gc_root_count.  The unwound builtin's stack-local CL_Obj
 * slots stayed registered in this thread's gc_roots[].  That stack
 * memory was then reused by later calls, so a subsequent gc_mark
 * (often from ANOTHER thread — gc_mark_thread_roots walks ALL threads'
 * roots) dereferenced a stale slot whose bytes now decoded to a bogus
 * heap header with a multi-megabyte length, and walked off the arena.
 *
 * Fix (commit b2ad54d): CL_ErrorFrame.saved_gc_roots, captured in
 * cl_error_frame_push() and restored in cl_error_unwind() before the
 * longjmp.  These two tests assert that invariant directly and under
 * concurrent GC.
 *
 * NOTE: the leak lives ONLY on the nested error-frame path
 * (cl_error_frame_top > 1); the outermost frame calls
 * cl_gc_reset_roots() which zeroes the count regardless.  So both
 * tests wrap the erroring inner CL_CATCH in an OUTER CL_CATCH —
 * exactly the sento/fiveam topology (handler-case per actor message
 * around the bi_map call).
 * ================================================================ */

/* Mimics bi_map: protect accumulators, then a nested call errors
 * mid-loop.  Never returns — cl_error longjmps to the nearest CL_CATCH. */
static CL_NORETURN void map_like_builtin_that_errors(void)
{
    CL_Obj func   = CL_NIL;
    CL_Obj result = CL_NIL;
    CL_Obj tail   = CL_NIL;

    CL_GC_PROTECT(func);
    CL_GC_PROTECT(result);
    CL_GC_PROTECT(tail);

    /* Build a partial accumulator, then "the mapped function signals"
     * mid-loop — just like a Lisp error raised inside (map ... func ...). */
    result = cl_cons(CL_MAKE_FIXNUM(0), result);
    result = cl_cons(CL_MAKE_FIXNUM(1), result);
    tail   = result;
    cl_error(CL_ERR_GENERAL, "simulated error from mapped function");

    /* No CL_GC_UNPROTECT: cl_error above is noreturn (longjmps to the
     * nearest CL_CATCH).  Leaving the 3 roots pushed is the whole point —
     * the unwind must reclaim them; that is what this test verifies. */
}

TEST(error_unwind_restores_gc_roots_nested)
{
    int outer_err;
    int base = CT->gc_root_count;
    CL_Obj keep = CL_NIL;

    CL_CATCH(outer_err);                 /* frame 0 */
    if (outer_err == CL_ERR_NONE) {
        int inner_err, mid;

        /* A legitimately-protected root held across the inner unwind.  Its
         * slot must survive; the leaked builtin slots must not. */
        keep = cl_cons(CL_MAKE_FIXNUM(42), CL_NIL);
        CL_GC_PROTECT(keep);             /* gc_root_count = base + 1 */
        mid = CT->gc_root_count;

        CL_CATCH(inner_err);             /* nested frame; saved_gc_roots = mid */
        if (inner_err == CL_ERR_NONE) {
            map_like_builtin_that_errors();   /* +3 roots, then longjmp */
            ASSERT(0);                   /* must not fall through */
        }
        CL_UNCATCH();                    /* pop the inner frame */

        /* THE REGRESSION CHECK: the 3 roots pushed by the unwound builtin
         * must have been dropped — gc_root_count is back to the inner
         * catch-site snapshot, not mid + 3 (the pre-b2ad54d leak). */
        ASSERT_EQ_INT(CT->gc_root_count, mid);

        /* And the legitimately-protected root must still be live & intact
         * across a GC — which now must not walk any stale slot. */
        cl_gc();
        ASSERT(CL_CONS_P(keep));
        ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(keep)), 42);

        CL_GC_UNPROTECT(1);              /* pop keep */
    }
    CL_UNCATCH();                        /* pop the outer frame */

    ASSERT_EQ_INT(CT->gc_root_count, base);
}

/* ----------------------------------------------------------------
 * Concurrent variant: a worker repeatedly maps-and-errors (each error
 * unwinding through a GC-protected frame) while the main thread hammers
 * stop-the-world GC.  This is the actual sento topology — a worker doing
 * bi_map+error while another thread's gc_mark_thread_roots walks every
 * thread's roots.
 *
 * Two failure signals, either of which flags a regression:
 *   1. Drift: with the outer frame held open for the whole loop, a
 *      leaked unwind adds +3 unreclaimed roots each iteration, so
 *      gc_root_count climbs off its baseline.  The worker checks this
 *      every iteration and bails.
 *   2. Crash: a leaked stale root, walked by a concurrent compacting GC,
 *      dereferences the poison written into the reused stack frame and
 *      SEGVs — turning a silent leak into a loud process abort.
 * ---------------------------------------------------------------- */

typedef struct {
    CL_Thread     thread;
    volatile int  ready;
    volatile int  done;
    int           iters;
    int           result_ok;
} MapErrCtx;

/* Reuse the just-unwound builtin's stack region with a poison CL_Obj: a
 * heap-tagged value at a ~4GB offset.  If a stale root still points here,
 * a concurrent gc_mark dereferences far past the arena and crashes.
 * Harmless when roots were correctly dropped. */
static void poison_reused_frame(void)
{
    volatile CL_Obj junk[24];
    int i;
    for (i = 0; i < 24; i++)
        junk[i] = (CL_Obj)0xFFFFFFF8u;   /* (x & 3)==0 => looks heap, huge offset */
    if (junk[0] == CL_NIL) junk[0] = 0;  /* defeat dead-store elimination */
}

static void *map_error_victim(void *arg)
{
    MapErrCtx *ctx = (MapErrCtx *)arg;
    CL_Thread *t = &ctx->thread;
    int outer_err;

    platform_tls_set(t);
    cl_thread_register(t);
    ctx->ready = 1;

    CL_CATCH(outer_err);                 /* held open for the whole loop */
    if (outer_err == CL_ERR_NONE) {
        CL_Obj keep = CL_NIL;
        int baseline, i;

        keep = cl_cons(CL_MAKE_FIXNUM(7), CL_NIL);
        CL_GC_PROTECT(keep);
        baseline = CT->gc_root_count;    /* must hold flat across all iters */

        ctx->result_ok = 1;
        for (i = 0; i < ctx->iters; i++) {
            int inner_err;

            CL_CATCH(inner_err);         /* nested frame each iteration */
            if (inner_err == CL_ERR_NONE)
                map_like_builtin_that_errors();   /* +3 roots, then longjmp */
            CL_UNCATCH();

            /* Signal 1 — drift. */
            if (CT->gc_root_count != baseline) {
                ctx->result_ok = 0;
                break;
            }

            poison_reused_frame();

            /* Allocate so the concurrent STW collector reaches us at a
             * safepoint and the moving GC actually compacts/relocates. */
            keep = cl_cons(CL_MAKE_FIXNUM(i), keep);
        }

        /* The legitimately-protected list must be intact after the storm. */
        if (!CL_CONS_P(keep) || CL_FIXNUM_VAL(cl_car(keep)) != ctx->iters - 1)
            ctx->result_ok = 0;

        CL_GC_UNPROTECT(1);
    } else {
        ctx->result_ok = 0;              /* the outer frame must never catch */
    }
    CL_UNCATCH();

    cl_thread_unregister(t);
    ctx->done = 1;
    return NULL;
}

TEST(concurrent_gc_vs_map_error_unwind)
{
    void *vhandle;
    MapErrCtx ctx;
    int spins;

    /* Small heap so the concurrent GC compacts (moves objects) often —
     * relocation is when a stale root does the most damage. */
    reinit_heap(512 * 1024);

    memset(&ctx, 0, sizeof(ctx));
    ctx.thread.id = 1;
    ctx.thread.status = 1;
    ctx.thread.mv_count = 1;
    ctx.iters = 400;
    ctx.result_ok = 0;

    platform_thread_create(&vhandle, map_error_victim, &ctx, 0);

    while (!ctx.ready)
        platform_sleep_ms(1);

    /* Collector: hammer STW GC concurrently with the victim's
     * protect/error/unwind loop.  If a leaked root survives an unwind,
     * one of these gc_mark passes walks poison and SEGVs.  The spin cap
     * is a safety net against a coordination hang. */
    spins = 0;
    while (!ctx.done && spins < 1000000) {
        cl_gc();
        spins++;
    }

    platform_thread_join(vhandle, NULL);

    ASSERT_EQ_INT(ctx.result_ok, 1);
    ASSERT_EQ_INT(ctx.done, 1);

    /* Restore normal heap. */
    reinit_heap(4 * 1024 * 1024);
}

/* ================================================================
 * Regression: the UNWIND-PROTECT value save stack (Tier-4 phase 2,
 * OP_MV_SAVE) is a per-thread root table that gc_mark_thread_roots and
 * gc_update_thread_roots walk up to mv_save_top, so
 * cl_thread_reset_lisp_state — the heap re-init and image-restore reset
 * of every registered thread — must clear mv_save_top like the other
 * root tops.  A record left in flight (a cleanup was running when the
 * heap went away) would otherwise be walked as stale offsets into the
 * freed arena.  Caught by the commit's review; this pins the invariant.
 * ================================================================ */

TEST(mv_save_top_reset_on_heap_reinit)
{
    CL_Thread *t = cl_current_thread;

    /* Park one record by hand: two values, then the fixnum-tagged count. */
    t->mv_save_buf[0] = CL_MAKE_FIXNUM(1);
    t->mv_save_buf[1] = CL_MAKE_FIXNUM(2);
    t->mv_save_buf[2] = CL_MAKE_FIXNUM(2);
    t->mv_save_top = 3;
    cl_thread_reset_lisp_state(t);
    ASSERT_EQ_INT(t->mv_save_top, 0);

    /* The re-init path resets every registered thread through it. */
    t->mv_save_top = 3;
    reinit_heap(512 * 1024);
    ASSERT_EQ_INT(cl_current_thread->mv_save_top, 0);

    reinit_heap(4 * 1024 * 1024);
}

/* ================================================================
 * Regression: shared C-global Lisp tables must be reset by cl_mem_init.
 *
 * gc_mark marks a set of shared globals DIRECTLY (package registry,
 * compiler/CLOS/condition tables, readtable closures) — not through the
 * resettable registered-root table.  After a mid-process heap
 * shutdown/re-init (every C unit test does this; embedded restarts
 * would too), those statics still held offsets into the FREED arena.
 * As soon as the new heap's bump front grew past such a stale offset,
 * the offset passed gc_mark's plausibility validation and a mark bit
 * was stamped into the middle of whatever live object now spans it —
 * observed as 0x800000 (CL_HDR_MARK_BIT) ORed into a cons cdr, i.e.
 * silent corruption of unrelated data.  The old tests never allocated
 * far enough past the stale offsets to trip it; the TLAB tests below
 * do, which is how it surfaced.
 * ================================================================ */

TEST(shared_tables_reset_on_heap_reinit)
{
    extern CL_Obj macro_table, setf_table, setf_fn_table,
        setf_expander_table, type_table, compiler_macro_table;
    extern CL_Obj cl_clos_class_table, struct_table, condition_hierarchy,
        condition_slot_table, condition_default_initargs,
        condition_slot_initforms;
    CL_Obj keep = CL_NIL;
    int cycle, i;

    /* setup() populated the package registry et al. in the original 4MB
     * heap, at low offsets.  Re-init with a fresh small heap — the raw
     * cl_mem_init here, since the invariant below is about what IT
     * clears; the package/symbol init is re-run once that is checked. */
    cl_mem_shutdown();
    cl_mem_init(512 * 1024);

    /* The crisp invariant: every unconditionally-marked shared global
     * must have been cleared by cl_mem_init. */
    ASSERT(CL_NULL_P(cl_package_registry));
    ASSERT(CL_NULL_P(macro_table));
    ASSERT(CL_NULL_P(setf_table));
    ASSERT(CL_NULL_P(setf_fn_table));
    ASSERT(CL_NULL_P(setf_expander_table));
    ASSERT(CL_NULL_P(type_table));
    ASSERT(CL_NULL_P(compiler_macro_table));
    ASSERT(CL_NULL_P(cl_clos_class_table));
    ASSERT(CL_NULL_P(struct_table));
    ASSERT(CL_NULL_P(condition_hierarchy));
    ASSERT(CL_NULL_P(condition_slot_table));
    ASSERT(CL_NULL_P(condition_default_initargs));
    ASSERT(CL_NULL_P(condition_slot_initforms));

    cl_package_init();
    cl_symbol_init();

    /* The behavioral leg: push the bump front well past every old
     * table offset, then GC repeatedly.  Pre-fix, one of these marks
     * walked a stale offset and corrupted the protected list. */
    CL_GC_PROTECT(keep);
    for (cycle = 0; cycle < 6; cycle++) {
        keep = CL_NIL;
        for (i = 0; i < 2000; i++)
            keep = cl_cons(CL_MAKE_FIXNUM(i), keep);
        cl_gc();
        {
            CL_Obj walk = keep;
            int expected = 1999;
            while (!CL_NULL_P(walk)) {
                ASSERT(CL_CONS_P(walk));
                ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(walk)), expected);
                walk = cl_cdr(walk);
                expected--;
            }
            ASSERT_EQ_INT(expected, -1);
        }
        cl_gc_compact();
    }
    CL_GC_UNPROTECT(1);

    /* Restore the standard test heap (packages + symbols included —
     * see reinit_heap). */
    reinit_heap(4 * 1024 * 1024);
}

/* ================================================================
 * TLAB — per-thread allocation buffers (multi-threaded fast path)
 *
 * These tests force the chunk kind of the allocation cursor (mem.c)
 * through its full lifecycle: refills (chunk exhaustion), STW GC dropping
 * all cursors mid-allocation (gc_cursor_drop_all), compaction sliding
 * objects allocated from chunks, mixed chunk/direct-path sizes, and the
 * multi→single-thread transition leaving formatted holes for the next
 * sweep to reclaim.  The single-threaded (front) kind and the regime
 * transitions are covered by tests/test_gc_cursor.c.
 * Heaps are sized small so chunks are small (heap/64) and every worker
 * refills many times through real GC cycles.
 * ================================================================ */

#ifdef CL_TLAB

/* Worker: build a list of (i . "i-as-string") pairs — conses AND
 * variable-length strings, both cut from the TLAB — then validate every
 * element, so a hole-formatting or double-hand-out bug shows up as a
 * corrupted car/string rather than only as a crash. */
typedef struct {
    CL_Thread    thread;
    int          alloc_count;
    int          result_ok;
} TlabWorkerCtx;

static void *tlab_list_builder(void *arg)
{
    TlabWorkerCtx *ctx = (TlabWorkerCtx *)arg;
    CL_Thread *t = &ctx->thread;
    int i;
    CL_Obj list = CL_NIL;

    platform_tls_set(t);
    cl_thread_register(t);

    t->gc_roots[0] = &list;
    t->gc_root_count = 1;

    for (i = 0; i < ctx->alloc_count; i++) {
        char nb[32];
        CL_Obj str, pair;
        snprintf(nb, sizeof(nb), "v%d", i);
        str = cl_make_string(nb, (uint32_t)strlen(nb));
        pair = cl_cons(CL_MAKE_FIXNUM(i), str);
        list = cl_cons(pair, list);
    }

    ctx->result_ok = 1;
    {
        CL_Obj walk = list;
        int expected = ctx->alloc_count - 1;
        while (!CL_NULL_P(walk)) {
            CL_Obj pair = cl_car(walk);
            char nb[32];
            CL_String *s;
            if (!CL_CONS_P(pair) ||
                CL_FIXNUM_VAL(cl_car(pair)) != expected ||
                !CL_STRING_P(cl_cdr(pair))) {
                ctx->result_ok = 0;
                break;
            }
            snprintf(nb, sizeof(nb), "v%d", expected);
            s = (CL_String *)CL_OBJ_TO_PTR(cl_cdr(pair));
            if (strcmp((char *)s->data, nb) != 0) {
                ctx->result_ok = 0;
                break;
            }
            walk = cl_cdr(walk);
            expected--;
        }
        if (expected != -1)
            ctx->result_ok = 0;
    }

    t->gc_root_count = 0;
    cl_thread_unregister(t);
    return NULL;
}

TEST(tlab_concurrent_refills_and_gc)
{
    void *handles[4];
    TlabWorkerCtx workers[4];
    int i;
    uint32_t refills_before;

    /* 512K heap → 8K chunks; 4 workers × 2000 pairs (cons+string ≈ 48+
     * bytes each) → each worker refills its TLAB many times and the
     * combined churn forces several STW GC cycles mid-allocation. */
    reinit_heap(512 * 1024);
    refills_before = cl_heap.tlab_refills;

    for (i = 0; i < 4; i++) {
        memset(&workers[i], 0, sizeof(TlabWorkerCtx));
        workers[i].thread.id = (uint32_t)(i + 1);
        workers[i].thread.status = 1;
        workers[i].thread.mv_count = 1;
        workers[i].alloc_count = 2000;
    }

    for (i = 0; i < 4; i++)
        platform_thread_create(&handles[i], tlab_list_builder, &workers[i], 0);
    for (i = 0; i < 4; i++)
        platform_thread_join(handles[i], NULL);

    for (i = 0; i < 4; i++)
        ASSERT_EQ_INT(workers[i].result_ok, 1);

    /* The fast path must actually have engaged — otherwise this test is
     * vacuously green while the TLAB never ran. */
    ASSERT(cl_heap.tlab_refills > refills_before);

    reinit_heap(4 * 1024 * 1024);
}

/* Worker: alternate TLAB-sized allocations with vectors ABOVE
 * tlab_max_obj (shared mutex path) so both allocators interleave in one
 * thread, while the main thread hammers full compactions.  Vector
 * contents are validated element-by-element after the storm — a TLAB
 * hole mis-parsed by the compactor's linear walk corrupts the slide and
 * shows up here. */
typedef struct {
    CL_Thread     thread;
    volatile int  ready;
    volatile int  done;
    int           iters;
    int           result_ok;
} TlabMixCtx;

static void *tlab_mixed_size_worker(void *arg)
{
    TlabMixCtx *ctx = (TlabMixCtx *)arg;
    CL_Thread *t = &ctx->thread;
    int i, j;
    CL_Obj keep = CL_NIL;   /* list of (fixnum . vector) */

    platform_tls_set(t);
    cl_thread_register(t);
    t->gc_roots[0] = &keep;
    t->gc_root_count = 1;
    ctx->ready = 1;

    ctx->result_ok = 1;
    for (i = 0; i < ctx->iters; i++) {
        /* 600 CL_Obj slots ≈ 2.4KB payload > tlab_max_obj (1K at an 8K
         * chunk) → shared path; the pair/backbone conses → TLAB path. */
        CL_Obj vec = cl_make_vector(600);
        CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(vec);
        for (j = 0; j < 600; j++)
            v->data[j] = CL_MAKE_FIXNUM(i * 600 + j);
        keep = cl_cons(cl_cons(CL_MAKE_FIXNUM(i), vec), keep);
        /* Keep only the last 4 vectors live so the heap doesn't fill:
         * older ones become compaction fodder. */
        if (i >= 4) {
            CL_Obj walk = keep;
            int k;
            for (k = 0; k < 3; k++) walk = cl_cdr(walk);
            ((CL_Cons *)CL_OBJ_TO_PTR(walk))->cdr = CL_NIL;
        }
    }

    /* Validate the survivors. */
    {
        CL_Obj walk = keep;
        int idx = ctx->iters - 1;
        while (!CL_NULL_P(walk) && ctx->result_ok) {
            CL_Obj pair = cl_car(walk);
            CL_Vector *v;
            if (!CL_CONS_P(pair) ||
                CL_FIXNUM_VAL(cl_car(pair)) != idx ||
                !CL_VECTOR_P(cl_cdr(pair))) {
                ctx->result_ok = 0;
                break;
            }
            v = (CL_Vector *)CL_OBJ_TO_PTR(cl_cdr(pair));
            for (j = 0; j < 600; j++) {
                if (v->data[j] != CL_MAKE_FIXNUM(idx * 600 + j)) {
                    ctx->result_ok = 0;
                    break;
                }
            }
            walk = cl_cdr(walk);
            idx--;
        }
    }

    t->gc_root_count = 0;
    cl_thread_unregister(t);
    ctx->done = 1;
    return NULL;
}

TEST(tlab_mixed_sizes_vs_concurrent_compaction)
{
    void *handles[2];
    TlabMixCtx ctxs[2];
    int i, spins;

    reinit_heap(512 * 1024);

    for (i = 0; i < 2; i++) {
        memset(&ctxs[i], 0, sizeof(TlabMixCtx));
        ctxs[i].thread.id = (uint32_t)(i + 1);
        ctxs[i].thread.status = 1;
        ctxs[i].thread.mv_count = 1;
        ctxs[i].iters = 150;
    }

    for (i = 0; i < 2; i++)
        platform_thread_create(&handles[i], tlab_mixed_size_worker, &ctxs[i], 0);
    for (i = 0; i < 2; i++)
        while (!ctxs[i].ready)
            platform_sleep_ms(1);

    /* Hammer moving compactions while both workers cut from TLABs; every
     * cycle drops their chunks (gc_cursor_drop_all) and slides survivors
     * across the formatted holes.  Spin cap = hang safety net. */
    spins = 0;
    while ((!ctxs[0].done || !ctxs[1].done) && spins < 1000000) {
        cl_gc_compact();
        spins++;
    }

    for (i = 0; i < 2; i++)
        platform_thread_join(handles[i], NULL);

    for (i = 0; i < 2; i++) {
        ASSERT_EQ_INT(ctxs[i].done, 1);
        ASSERT_EQ_INT(ctxs[i].result_ok, 1);
    }

    reinit_heap(4 * 1024 * 1024);
}

/* Regression (Immix, 2026-10-13): a chunk claim walks the header chain
 * on the LIVE heap — under alloc_mutex, but the neighbouring region can be
 * another thread's chunk, whose owner rewrites its headers lock-free on
 * every cut.  cl_alloc used to zero the whole block, header word
 * included, before storing the object's header, so a claim walking
 * through a peer's chunk could read a zero header (walk ends) or — on a
 * weakly-ordered host — the new object header followed by the stale
 * bytes where the remainder header had not yet landed, follow a garbage
 * size, and truncate a header inside the peer's live objects.  Seen as
 * crashes and "CDR: argument is not of type LIST" in
 * test_mt_dispatch_addmethod_race under GC=immix, never under classic.
 * The fix keeps a covering header in the slot at all times (alloc_zero
 * skips the header word) and orders remainder-header-before-object-header
 * with a barrier when threads are live (gcb_cursor_cut, gc_backend.h).
 *
 * The scenario: four workers cut conses and strings from their chunks
 * (claims every few KB on a 1 MB heap, so claim walks constantly cross
 * peers' chunks) and every so often take a vector larger than a chunk
 * (the direct claim path, whose placeholder header cl_alloc overwrites
 * after the lock is dropped).  Each worker validates a rotating window of
 * its own objects; the main thread verifies the whole heap afterwards.
 * Runs on both backends (classic never walks the live chain, so there it
 * is a plain MT allocation hammer). */
typedef struct {
    CL_Thread     thread;
    int           iters;
    int           result_ok;
    uint32_t      bad_at;
} ClaimCutCtx;

#define CLAIMCUT_WINDOW 8

static void *claim_cut_worker(void *arg)
{
    ClaimCutCtx *ctx = (ClaimCutCtx *)arg;
    CL_Thread *t = &ctx->thread;
    CL_Obj window[CLAIMCUT_WINDOW];
    CL_Obj strtmp = CL_NIL;     /* rooted below the window slots */
    int i, j, w;

    platform_tls_set(t);
    cl_thread_register(t);
    for (w = 0; w < CLAIMCUT_WINDOW; w++) {
        window[w] = CL_NIL;
        t->gc_roots[w] = &window[w];
    }
    t->gc_roots[CLAIMCUT_WINDOW] = &strtmp;
    t->gc_root_count = CLAIMCUT_WINDOW + 1;
    ctx->result_ok = 1;

    for (i = 0; i < ctx->iters && ctx->result_ok; i++) {
        /* A small object graph: a list of 8 conses whose car is the
         * iteration tagged with the slot, plus a short string; every 700th
         * iteration a vector larger than any chunk (9000 slots = 36 KB). */
        /* Build straight into the rooted slots: a plain C local would be
         * invisible to (and stale after) a collection among the workers. */
        w = i % CLAIMCUT_WINDOW;
        window[w] = CL_NIL;
        if (i % 700 == 0) {
            CL_Vector *v;
            window[w] = cl_make_vector(9000);
            v = (CL_Vector *)CL_OBJ_TO_PTR(window[w]);
            for (j = 0; j < 9000; j++)
                v->data[j] = CL_MAKE_FIXNUM(i + j);
        } else {
            for (j = 0; j < 8; j++)
                window[w] = cl_cons(CL_MAKE_FIXNUM(i * 8 + j), window[w]);
            strtmp = cl_make_string("claim-cut", 9);
            window[w] = cl_cons(strtmp, window[w]);
        }

        /* Validate the whole window (the slots filled in the last 8
         * iterations) against what was stored. */
        for (j = 0; j < CLAIMCUT_WINDOW && i >= j; j++) {
            int k = i - j;
            CL_Obj o = window[k % CLAIMCUT_WINDOW];
            if (k % 700 == 0) {
                CL_Vector *v;
                int m;
                if (!CL_VECTOR_P(o)) { ctx->result_ok = 0; ctx->bad_at = (uint32_t)k; break; }
                v = (CL_Vector *)CL_OBJ_TO_PTR(o);
                for (m = 0; m < 9000; m += 997)
                    if (v->data[m] != CL_MAKE_FIXNUM(k + m)) {
                        ctx->result_ok = 0; ctx->bad_at = (uint32_t)k; break;
                    }
            } else {
                int m;
                if (!CL_CONS_P(o) || !CL_STRING_P(cl_car(o))) {
                    ctx->result_ok = 0; ctx->bad_at = (uint32_t)k; break;
                }
                o = cl_cdr(o);
                for (m = 7; m >= 0; m--) {
                    if (!CL_CONS_P(o) ||
                        cl_car(o) != CL_MAKE_FIXNUM(k * 8 + m)) {
                        ctx->result_ok = 0; ctx->bad_at = (uint32_t)k; break;
                    }
                    o = cl_cdr(o);
                }
                if (ctx->result_ok && !CL_NULL_P(o)) {
                    ctx->result_ok = 0; ctx->bad_at = (uint32_t)k;
                }
            }
        }
    }

    t->gc_root_count = 0;
    cl_thread_unregister(t);
    return NULL;
}

TEST(claims_vs_concurrent_cuts)
{
    void *handles[4];
    ClaimCutCtx ctxs[4];
    int i;

    reinit_heap(1024 * 1024);

    for (i = 0; i < 4; i++) {
        memset(&ctxs[i], 0, sizeof(ClaimCutCtx));
        ctxs[i].thread.id = (uint32_t)(i + 1);
        ctxs[i].thread.status = 1;
        ctxs[i].thread.mv_count = 1;
        ctxs[i].iters = 60000;
    }
    for (i = 0; i < 4; i++)
        platform_thread_create(&handles[i], claim_cut_worker, &ctxs[i], 0);
    /* The workers collect among themselves; a main thread parked in
     * pthread_join outside a safe region would be the straggler every one
     * of their stop-the-world requests waits for. */
    cl_gc_enter_safe_region();
    for (i = 0; i < 4; i++)
        platform_thread_join(handles[i], NULL);
    cl_gc_leave_safe_region();

    for (i = 0; i < 4; i++) {
        if (!ctxs[i].result_ok)
            printf("    worker %d: object from iteration %lu corrupted\n",
                   i, (unsigned long)ctxs[i].bad_at);
        ASSERT_EQ_INT(ctxs[i].result_ok, 1);
    }
    /* The chain and the block-start index must still be exact: a claim
     * that truncated a header inside a peer's chunk leaves a block whose
     * size no longer reaches the next header. */
    {
        int faults = cl_gc_verify_heap();
        if (faults > 0)
            printf("    heap verify: %s\n", cl_gc_verify_heap_report());
        ASSERT_EQ_INT(faults, 0);
    }
    ASSERT(cl_gc_audit_hdr_index() <= 0);

    reinit_heap(4 * 1024 * 1024);
}

/* Multi→single transition: workers leave abandoned (retired) chunks
 * behind as formatted holes; after they exit, single-threaded sweep AND
 * compaction must parse those holes, reclaim them, and keep main-thread
 * data intact.  Exercises cl_cursor_retire via cl_thread_unregister. */
TEST(tlab_leftover_holes_reclaimed_after_threads_exit)
{
    void *handles[3];
    TlabWorkerCtx workers[3];
    int i;
    CL_Obj keep = CL_NIL;
    uint32_t live_before, live_after;

    reinit_heap(512 * 1024);

    keep = CL_NIL;
    CL_GC_PROTECT(keep);
    for (i = 0; i < 100; i++)
        keep = cl_cons(CL_MAKE_FIXNUM(i), keep);

    for (i = 0; i < 3; i++) {
        memset(&workers[i], 0, sizeof(TlabWorkerCtx));
        workers[i].thread.id = (uint32_t)(i + 1);
        workers[i].thread.status = 1;
        workers[i].thread.mv_count = 1;
        workers[i].alloc_count = 500;
    }
    for (i = 0; i < 3; i++)
        platform_thread_create(&handles[i], tlab_list_builder, &workers[i], 0);
    for (i = 0; i < 3; i++)
        platform_thread_join(handles[i], NULL);
    for (i = 0; i < 3; i++)
        ASSERT_EQ_INT(workers[i].result_ok, 1);

    /* Single-threaded again.  Sweep must reclaim the workers' dead data
     * AND their abandoned chunk holes (they coalesce into free blocks)... */
    ASSERT_EQ_INT((int)cl_thread_count, 1);
    cl_gc();
    live_before = cl_heap.total_allocated;

    /* ...and a moving compaction must walk the swept arena cleanly. */
    cl_gc_compact();
    live_after = cl_heap.total_allocated;
    ASSERT(live_after <= live_before);

    /* Main-thread data survived both. */
    {
        CL_Obj walk = keep;
        int expected = 99;
        while (!CL_NULL_P(walk)) {
            ASSERT(CL_CONS_P(walk));
            ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(walk)), expected);
            walk = cl_cdr(walk);
            expected--;
        }
        ASSERT_EQ_INT(expected, -1);
    }

    CL_GC_UNPROTECT(1);
    reinit_heap(4 * 1024 * 1024);
}

#endif /* CL_TLAB */

/* ================================================================
 * Regression: cl_lock_alloc_obj must GC-protect `name` across its
 * allocation (see builtins_thread.c).  Under the generational collector
 * the allocation is a MOVING collection point, so an unprotected `name`
 * local would end up stale (pre-move offset) once stored into the
 * returned lock's `name` slot.  Locks are plain heap words now (no side
 * table, no cap): make many of them so the allocation itself has to
 * collect, then check that a named lock's name survived intact.
 * ================================================================ */

TEST(lock_alloc_obj_protects_name_across_gc)
{
    CL_Obj held = CL_NIL, name, lock;
    CL_Lock *lk;
    CL_String *s;
    int i;

    CL_GC_PROTECT(held);
    for (i = 0; i < 20000; i++) {
        CL_Obj filler = cl_lock_alloc_obj(0, CL_NIL, "TEST");
        held = cl_cons(filler, held);
    }
    held = CL_NIL;   /* every filler lock is now unreachable garbage */
    CL_GC_UNPROTECT(1);

    name = cl_make_string("named-lock-regression", 22);
    cl_gc();
    lock = cl_lock_alloc_obj(0, name, "TEST");

    ASSERT(CL_LOCK_P(lock));
    lk = (CL_Lock *)CL_OBJ_TO_PTR(lock);
    ASSERT_EQ_INT((int)lk->state, 0);
    ASSERT_EQ_INT((int)lk->depth, 0);
    ASSERT(CL_STRING_P(lk->name));
    s = (CL_String *)CL_OBJ_TO_PTR(lk->name);
    ASSERT_EQ_INT((int)s->length, 22);
    ASSERT(memcmp(s->data, "named-lock-regression", 22) == 0);
}

/* ================================================================
 * main
 * ================================================================ */

int main(void)
{
    test_init();
    setup();

    /* Registry */
    RUN(thread_registry_main_registered);
    RUN(thread_registry_add_remove);

    /* Single-thread regression */
    RUN(alloc_single_thread_under_mutex);
    RUN(gc_single_thread_stw);
    RUN(safepoint_no_gc);

    /* Multi-thread allocation */
    RUN(concurrent_alloc_2_threads);
    RUN(concurrent_alloc_4_threads);
    RUN(concurrent_alloc_with_gc);

    /* GC-epoch dedup of redundant allocation-triggered collections and
     * the phase timers — classic-collector semantics (see reinit_classic) */
    reinit_classic();
#ifdef CL_GC_CLASSIC
    RUN(gc_if_stale_single_thread_always_collects);
    RUN(gc_if_stale_skips_when_epoch_advanced);
#endif
    RUN(gc_time_stats_and_stw_stats_accumulate);
    reinit_default();

    /* Regression: STW GC vs. thread blocked in a socket syscall */
    RUN(stw_gc_with_thread_blocked_in_accept);

    /* Regression: STW GC vs. thread blocked in stdin read_line (SLY :spawn hang) */
    RUN(stw_gc_with_thread_blocked_in_read_line);

    /* Regression: STW GC vs. thread blocked ON an internal I/O lock held
     * across a GC safe region (the rare extreme-concurrency STW deadlock) */
    RUN(stw_gc_with_thread_blocked_on_io_lock);

    /* Regression: a compacting GC racing cl_gc_safe_mutex_lock's contended
     * path must not corrupt the relocated stream object it was fired on */
    RUN(stream_read_survives_iolock_contention_and_compaction);

    /* Regression: error unwind through a GC-protected frame restores
     * gc_root_count (the sento gc_mark SEGV) — deterministic + concurrent */
    RUN(error_unwind_restores_gc_roots_nested);
    RUN(concurrent_gc_vs_map_error_unwind);

    /* Regression: unconditionally-marked shared Lisp globals (package
     * registry, compiler tables, ...) must be reset on heap re-init */
    RUN(shared_tables_reset_on_heap_reinit);
    RUN(mv_save_top_reset_on_heap_reinit);

    /* Regression: cl_lock_alloc_obj's `name` must survive the moving
     * cl_gc_reclaim_young()/cl_gc() retries on lock-table exhaustion */
    RUN(lock_alloc_obj_protects_name_across_gc);

#ifdef CL_TLAB
    /* TLAB: per-thread allocation buffers (refills, GC reset, compaction,
     * mixed sizes, multi→single leftovers) */
    RUN(tlab_concurrent_refills_and_gc);
    RUN(tlab_mixed_sizes_vs_concurrent_compaction);
    RUN(claims_vs_concurrent_cuts);
    RUN(tlab_leftover_holes_reclaimed_after_threads_exit);
#endif

    teardown();

    REPORT();
}
