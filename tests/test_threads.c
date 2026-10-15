/*
 * test_threads.c — Tests for Phase 4: CL-level threading API (MP package).
 *
 * Tests: thread creation/join, thread-alive-p, current-thread, all-threads,
 * lock acquire/release, condvar wait/notify, dynamic binding isolation,
 * error handling in threads, GC under threaded execution.
 */

#include "test.h"
#include "core/types.h"
#include "core/mem.h"
#include "core/error.h"
#include "core/package.h"
#include "core/symbol.h"
#include "core/reader.h"
#include "core/printer.h"
#include "core/compiler.h"
#include "core/vm.h"
#include "core/builtins.h"
#include "core/repl.h"
#include "core/thread.h"
#include "platform/platform.h"
#include "platform/platform_thread.h"

#include <string.h>
#include <stdio.h>

/* ================================================================
 * Setup / teardown
 * ================================================================ */

static void setup(void)
{
    platform_init();
    cl_thread_init();
    cl_error_init();
    cl_mem_init(4 * 1024 * 1024);  /* 4MB heap */
    cl_package_init();
    cl_symbol_init();
    cl_reader_init();
    cl_printer_init();
    cl_compiler_init();
    cl_vm_init(0, 0);
    cl_builtins_init();
    cl_repl_init();
}

static void teardown(void)
{
    cl_vm_shutdown();
    cl_mem_shutdown();
    cl_thread_shutdown();
    platform_shutdown();
}

/* Helper: eval a Lisp string, return printed result */
static const char *eval_print(const char *str)
{
    static char buf[4096];
    int err;

    CL_CATCH(err);
    if (err == CL_ERR_NONE) {
        CL_Obj result = cl_eval_string(str);
        cl_prin1_to_string(result, buf, sizeof(buf));
        CL_UNCATCH();
        return buf;
    } else {
        CL_UNCATCH();
        cl_vm.sp = 0;
        cl_vm.fp = 0;
        snprintf(buf, sizeof(buf), "ERROR:%d", err);
        return buf;
    }
}

/* ================================================================
 * Thread creation and join
 * ================================================================ */

TEST(thread_make_and_join)
{
    const char *r = eval_print(
        "(mp:join-thread (mp:make-thread (lambda () 42)))");
    ASSERT_STR_EQ(r, "42");
}

TEST(thread_make_with_name)
{
    const char *r = eval_print(
        "(mp:thread-name (mp:make-thread (lambda () nil) :name \"worker\"))");
    ASSERT_STR_EQ(r, "\"worker\"");
}

TEST(thread_join_returns_result)
{
    const char *r = eval_print(
        "(mp:join-thread (mp:make-thread (lambda () (+ 10 20 30))))");
    ASSERT_STR_EQ(r, "60");
}

TEST(thread_join_cons_result)
{
    const char *r = eval_print(
        "(mp:join-thread (mp:make-thread (lambda () (list 1 2 3))))");
    ASSERT_STR_EQ(r, "(1 2 3)");
}

/* Regression: a worker thread must tolerate the same call-nesting depth as the
 * main thread.  Worker VM sizes used to be 4x smaller (256 frames vs 1024), so a
 * recursion ~300-deep overflowed the worker's VM frame stack where the main
 * thread coped fine.  That overflow could not be caught (running a Lisp handler
 * itself needs frames) so the worker died silently, its result lost — which is
 * why (asdf:load-system ...) hung under Sly (slynk evaluates on a channel worker
 * thread) yet loaded fine headless (main thread).  Depth 500 is > the old worker
 * limit (256) and < the main limit (1024): pre-fix the worker dies and
 * join-thread returns NIL; post-fix it returns 500 exactly like the main thread. */
TEST(worker_deep_recursion_survives_like_main)
{
    /* Sanity: the main thread computes it (well under 1024 frames). */
    const char *m = eval_print(
        "(labels ((r (n) (if (<= n 0) 0 (1+ (funcall #'r (1- n)))))) (r 500))");
    ASSERT_STR_EQ(m, "500");

    /* The worker must produce the identical result — not NIL from a dead thread. */
    const char *w = eval_print(
        "(mp:join-thread"
        "  (mp:make-thread"
        "    (lambda ()"
        "      (labels ((r (n) (if (<= n 0) 0 (1+ (funcall #'r (1- n))))))"
        "        (r 500)))))");
    ASSERT_STR_EQ(w, "500");
}

/* Regression: a worker must tolerate deep NLX (CATCH/BLOCK/UNWIND-PROTECT)
 * nesting up to its own nlx_stack capacity, and the overflow guards must bound
 * against the PER-THREAD capacity (cl_nlx_max) — not the global CL_MAX_NLX_FRAMES
 * constant.  Historically the worker nlx_stack was allocated with only 256 slots
 * while every push guard compared against CL_MAX_NLX_FRAMES (2048), so a worker
 * nesting 257..2048 NLX frames wrote past the end of its allocation (silent heap
 * corruption).  Nesting 400 CATCH frames (> the old 256 allocation) must now
 * simply work: the throw unwinds to the innermost catch and 42 propagates out. */
TEST(worker_deep_nlx_nesting_no_corruption)
{
    const char *w = eval_print(
        "(mp:join-thread"
        "  (mp:make-thread"
        "    (lambda ()"
        "      (labels ((c (n) (if (<= n 0) (throw 'done 42) (catch 'done (c (1- n))))))"
        "        (catch 'done (c 400))))))");
    ASSERT_STR_EQ(w, "42");
}

/* Regression: an error raised deep enough to be near (but under) the frame
 * limit must still be catchable on a worker.  With the old compact worker sizes,
 * a handler-case wrapping deep work could be bypassed entirely when the frame
 * stack filled.  Now a worker matches the main thread: the handler runs and its
 * value is returned. */
TEST(worker_handler_case_runs_after_deep_work)
{
    const char *w = eval_print(
        "(mp:join-thread"
        "  (mp:make-thread"
        "    (lambda ()"
        "      (labels ((r (n) (if (<= n 0) (error \"boom\") (1+ (funcall #'r (1- n))))))"
        "        (handler-case (r 400) (error (e) (declare (ignore e)) :caught))))))");
    ASSERT_STR_EQ(w, ":CAUGHT");
}

/* MP:MAKE-THREAD per-thread size keywords (:stack-size :vm-stack-size
 * :vm-frames :nlx-frames).  Each is a MINIMUM the worker's budget is raised
 * to.  Motivated by the AmigaOS worker walls: the compile-time CL_WORKER_*
 * defaults there must stay at their historical values (layout-fragile
 * moving-GC bug, see thread.h), so growing a worker is a per-thread runtime
 * opt-in that touches only that worker's malloc'd arrays and OS stack. */
TEST(make_thread_size_keywords_grow_worker)
{
    /* Depth 1500 exceeds even the host default (1024 frames): without the
     * keyword the worker dies silently and join returns NIL... */
    const char *dflt = eval_print(
        "(mp:join-thread"
        "  (mp:make-thread"
        "    (lambda ()"
        "      (labels ((r (n) (if (<= n 0) 0 (1+ (funcall #'r (1- n))))))"
        "        (r 1500)))))");
    ASSERT_STR_EQ(dflt, "NIL");

    /* ...with :vm-frames raised past it, the same work completes. */
    const char *big = eval_print(
        "(mp:join-thread"
        "  (mp:make-thread"
        "    (lambda ()"
        "      (labels ((r (n) (if (<= n 0) 0 (1+ (funcall #'r (1- n))))))"
        "        (r 1500)))"
        "    :vm-frames 4096))");
    ASSERT_STR_EQ(big, "1500");

    /* All four keywords together are accepted alongside :name. */
    const char *all = eval_print(
        "(mp:join-thread"
        "  (mp:make-thread (lambda () 42) :name \"sized\""
        "    :stack-size 1000000 :vm-stack-size 32768"
        "    :vm-frames 2048 :nlx-frames 4096))");
    ASSERT_STR_EQ(all, "42");
}

TEST(make_thread_size_keywords_are_minimums)
{
    /* Tiny requests are floored at the platform defaults — a worker can only
     * be grown, so depth 500 (needs > 256 frames) must still succeed. */
    const char *w = eval_print(
        "(mp:join-thread"
        "  (mp:make-thread"
        "    (lambda ()"
        "      (labels ((r (n) (if (<= n 0) 0 (1+ (funcall #'r (1- n))))))"
        "        (r 500)))"
        "    :stack-size 4096 :vm-stack-size 16 :vm-frames 16 :nlx-frames 16))");
    ASSERT_STR_EQ(w, "500");

    /* C-level: the sized allocator itself applies the floors. */
    {
        CL_Thread *t = cl_thread_alloc_worker_sized(16, 16, 16);
        ASSERT(t != NULL);
        ASSERT_EQ(t->vm.stack_size, (uint32_t)CL_WORKER_VM_STACK_SIZE);
        ASSERT_EQ(t->vm.frame_size, CL_WORKER_VM_FRAME_SIZE);
        ASSERT_EQ(t->nlx_max, CL_WORKER_NLX_FRAMES);
        cl_thread_free_worker(t);

        t = cl_thread_alloc_worker_sized(CL_WORKER_VM_STACK_SIZE * 2,
                                         CL_WORKER_VM_FRAME_SIZE * 2,
                                         CL_WORKER_NLX_FRAMES * 2);
        ASSERT(t != NULL);
        ASSERT_EQ(t->vm.stack_size, (uint32_t)(CL_WORKER_VM_STACK_SIZE * 2));
        ASSERT_EQ(t->vm.frame_size, CL_WORKER_VM_FRAME_SIZE * 2);
        ASSERT_EQ(t->nlx_max, CL_WORKER_NLX_FRAMES * 2);
        cl_thread_free_worker(t);
    }
}

TEST(make_thread_size_keyword_rejects_bad_values)
{
    /* Non-fixnum, non-positive, and absurd values all signal a clean
     * type error instead of creating a misconfigured worker. */
    const char *r = eval_print(
        "(list"
        " (handler-case (progn (mp:make-thread (lambda () 1) :vm-frames :foo)"
        "                      :no-error)"
        "   (error () :caught))"
        " (handler-case (progn (mp:make-thread (lambda () 1) :stack-size 0)"
        "                      :no-error)"
        "   (error () :caught))"
        " (handler-case (progn (mp:make-thread (lambda () 1) :nlx-frames -5)"
        "                      :no-error)"
        "   (error () :caught))"
        " (handler-case (progn (mp:make-thread (lambda () 1)"
        "                                      :vm-stack-size 999999999)"
        "                      :no-error)"
        "   (error () :caught)))");
    ASSERT_STR_EQ(r, "(:CAUGHT :CAUGHT :CAUGHT :CAUGHT)");
}

/* ================================================================
 * Thread predicates
 * ================================================================ */

TEST(thread_alive_p_before_join)
{
    /* A newly created thread may or may not have finished by the time we check */
    const char *r = eval_print(
        "(let ((thr (mp:make-thread (lambda () (dotimes (i 1000) i)))))"
        "  (prog1 (mp:thread-alive-p thr) (mp:join-thread thr)))");
    ASSERT(strcmp(r, "T") == 0 || strcmp(r, "NIL") == 0);
}

TEST(thread_alive_p_after_join)
{
    const char *r = eval_print(
        "(let ((thr (mp:make-thread (lambda () 42))))"
        "  (mp:join-thread thr)"
        "  (mp:thread-alive-p thr))");
    ASSERT_STR_EQ(r, "NIL");
}

/* ================================================================
 * Current thread and all-threads
 * ================================================================ */

TEST(current_thread_returns_thread)
{
    const char *r = eval_print("(mp:current-thread)");
    ASSERT(strstr(r, "#<THREAD") != NULL);
}

TEST(all_threads_includes_main)
{
    const char *r = eval_print("(>= (length (mp:all-threads)) 1)");
    ASSERT_STR_EQ(r, "T");
}

/* Regression: the main/initial thread must carry a default name ("main
 * thread"), matching bordeaux-threads / SBCL / CCL, so ALL-THREADS prints
 * #<THREAD "main thread"> rather than the old #<THREAD NIL>.  See
 * cl_builtins_thread_init in src/core/builtins_thread.c. */
TEST(main_thread_has_default_name)
{
    const char *r = eval_print("(mp:thread-name (mp:current-thread))");
    ASSERT_STR_EQ(r, "\"main thread\"");
}

/* The named main thread must still be EQ to the wrapper ALL-THREADS reports,
 * and that name must be visible through the ALL-THREADS handle too. */
TEST(main_thread_name_via_all_threads)
{
    const char *r;
    r = eval_print("(eq (mp:current-thread) (first (mp:all-threads)))");
    ASSERT_STR_EQ(r, "T");
    r = eval_print("(mp:thread-name (first (mp:all-threads)))");
    ASSERT_STR_EQ(r, "\"main thread\"");
}

/* ================================================================
 * Thread yield
 * ================================================================ */

TEST(thread_yield_no_crash)
{
    const char *r = eval_print("(mp:thread-yield)");
    ASSERT_STR_EQ(r, "NIL");
}

/* ================================================================
 * Lock operations
 * ================================================================ */

TEST(lock_make)
{
    const char *r = eval_print("(mp:make-lock \"test-lock\")");
    ASSERT(strstr(r, "#<LOCK") != NULL);
}

TEST(lock_acquire_release)
{
    const char *r = eval_print(
        "(let ((lk (mp:make-lock)))"
        "  (mp:acquire-lock lk)"
        "  (mp:release-lock lk)"
        "  t)");
    ASSERT_STR_EQ(r, "T");
}

TEST(lock_trylock)
{
    const char *r = eval_print(
        "(let ((lk (mp:make-lock)))"
        "  (mp:acquire-lock lk nil))");
    ASSERT_STR_EQ(r, "T");
}

TEST(lock_with_lock_held)
{
    const char *r = eval_print(
        "(let ((lk (mp:make-lock)))"
        "  (mp:with-lock-held (lk)"
        "    42))");
    ASSERT_STR_EQ(r, "42");
}

TEST(lock_contention_two_threads)
{
    /* Two threads increment a shared counter protected by a lock */
    const char *r = eval_print(
        "(let ((counter (list 0))"
        "      (lk (mp:make-lock)))"
        "  (let ((t1 (mp:make-thread"
        "              (lambda ()"
        "                (dotimes (i 100)"
        "                  (mp:with-lock-held (lk)"
        "                    (setf (car counter) (+ (car counter) 1)))))))"
        "        (t2 (mp:make-thread"
        "              (lambda ()"
        "                (dotimes (i 100)"
        "                  (mp:with-lock-held (lk)"
        "                    (setf (car counter) (+ (car counter) 1))))))))"
        "    (mp:join-thread t1)"
        "    (mp:join-thread t2)"
        "    (car counter)))");
    ASSERT_STR_EQ(r, "200");
}

/* ================================================================
 * Condition variables
 * ================================================================ */

TEST(condvar_make)
{
    const char *r = eval_print("(mp:make-condition-variable)");
    ASSERT(strstr(r, "#<CONDITION-VARIABLE>") != NULL);
}

TEST(condvar_notify_wait)
{
    /* Producer-consumer: one thread waits, another notifies */
    const char *r = eval_print(
        "(let ((lk (mp:make-lock))"
        "      (cv (mp:make-condition-variable))"
        "      (ready (list nil)))"
        "  (let ((consumer (mp:make-thread"
        "                    (lambda ()"
        "                      (mp:acquire-lock lk)"
        "                      (loop until (car ready)"
        "                            do (mp:condition-wait cv lk))"
        "                      (mp:release-lock lk)"
        "                      (car ready)))))"
        "    (mp:thread-yield)"
        "    (mp:acquire-lock lk)"
        "    (setf (car ready) t)"
        "    (mp:condition-notify cv)"
        "    (mp:release-lock lk)"
        "    (mp:join-thread consumer)))");
    ASSERT_STR_EQ(r, "T");
}

/* ================================================================
 * Dynamic binding isolation (TLV inheritance)
 * ================================================================ */

TEST(thread_sees_global_package_value)
{
    /* Child thread sees global value of *package* (no TLV inheritance) */
    const char *r = eval_print(
        "(mp:join-thread"
        "  (mp:make-thread"
        "    (lambda () (package-name *package*))))");
    ASSERT_STR_EQ(r, "\"COMMON-LISP-USER\"");
}

TEST(thread_let_binding_isolation)
{
    /* With LET, child's dynamic binding is truly isolated */
    const char *r = eval_print(
        "(progn"
        "  (defvar *test-let-var* :parent)"
        "  (let ((child (mp:make-thread"
        "                 (lambda ()"
        "                   (let ((*test-let-var* :child))"
        "                     *test-let-var*)))))"
        "    (let ((child-result (mp:join-thread child)))"
        "      (list *test-let-var* child-result))))");
    ASSERT_STR_EQ(r, "(:PARENT :CHILD)");
}

TEST(thread_fresh_dynamic_env)
{
    /* Regression: a thread spawned INSIDE an active LET binding of a special
     * must NOT inherit that binding — it sees only the GLOBAL value.
     * (Previously the parent's TLV table was snapshotted into the child,
     * leaking the dynamic binding; SLYNK's with-connection relies on fresh
     * per-thread specials, so the leak cascaded into a fatal debugger entry.) */
    const char *r = eval_print(
        "(progn"
        "  (defvar *fresh-dyn-var* :global)"
        "  (let ((*fresh-dyn-var* :bound-in-parent))"
        "    (mp:join-thread"
        "      (mp:make-thread"
        "        (lambda () *fresh-dyn-var*)))))");
    ASSERT_STR_EQ(r, ":GLOBAL");
}

/* ================================================================
 * Error handling in threads
 * ================================================================ */

TEST(thread_error_sets_aborted)
{
    /* Thread that errors should not crash the parent */
    const char *r = eval_print(
        "(let ((thr (mp:make-thread"
        "             (lambda () (error \"boom\")))))"
        "  (mp:join-thread thr)"
        "  :survived)");
    ASSERT_STR_EQ(r, ":SURVIVED");
}

/* ================================================================
 * Multiple threads
 * ================================================================ */

TEST(thread_multiple_concurrent)
{
    const char *r = eval_print(
        "(let ((threads (list"
        "  (mp:make-thread (lambda () (* 1 10)))"
        "  (mp:make-thread (lambda () (* 2 10)))"
        "  (mp:make-thread (lambda () (* 3 10)))"
        "  (mp:make-thread (lambda () (* 4 10))))))"
        "  (mapcar #'mp:join-thread threads))");
    ASSERT_STR_EQ(r, "(10 20 30 40)");
}

/* ================================================================
 * Named condition variables
 * ================================================================ */

TEST(condvar_make_with_name)
{
    const char *r = eval_print(
        "(mp:make-condition-variable \"my-cv\")");
    ASSERT(strstr(r, "#<CONDITION-VARIABLE") != NULL);
    ASSERT(strstr(r, "my-cv") != NULL);
}

TEST(condvar_name_accessor)
{
    const char *r = eval_print(
        "(mp:condition-name (mp:make-condition-variable \"test-cv\"))");
    ASSERT_STR_EQ(r, "\"test-cv\"");
}

TEST(condvar_name_nil_when_unnamed)
{
    const char *r = eval_print(
        "(mp:condition-name (mp:make-condition-variable))");
    ASSERT_STR_EQ(r, "NIL");
}

/* ================================================================
 * Lock name accessor
 * ================================================================ */

TEST(lock_name_accessor)
{
    const char *r = eval_print(
        "(mp:lock-name (mp:make-lock \"my-lock\"))");
    ASSERT_STR_EQ(r, "\"my-lock\"");
}

TEST(lock_name_nil_when_unnamed)
{
    const char *r = eval_print(
        "(mp:lock-name (mp:make-lock))");
    ASSERT_STR_EQ(r, "NIL");
}

/* ================================================================
 * Type predicates
 * ================================================================ */

TEST(threadp_true)
{
    const char *r = eval_print("(mp:threadp (mp:current-thread))");
    ASSERT_STR_EQ(r, "T");
}

TEST(threadp_false)
{
    const char *r = eval_print("(mp:threadp 42)");
    ASSERT_STR_EQ(r, "NIL");
}

TEST(lockp_true)
{
    const char *r = eval_print("(mp:lockp (mp:make-lock))");
    ASSERT_STR_EQ(r, "T");
}

TEST(lockp_false)
{
    const char *r = eval_print("(mp:lockp \"not-a-lock\")");
    ASSERT_STR_EQ(r, "NIL");
}

TEST(condition_variable_p_true)
{
    const char *r = eval_print(
        "(mp:condition-variable-p (mp:make-condition-variable))");
    ASSERT_STR_EQ(r, "T");
}

TEST(condition_variable_p_false)
{
    const char *r = eval_print("(mp:condition-variable-p nil)");
    ASSERT_STR_EQ(r, "NIL");
}

/* ================================================================
 * interrupt-thread
 * ================================================================ */

TEST(interrupt_thread_basic)
{
    /* Interrupt a running thread to set a shared flag */
    const char *r = eval_print(
        "(let ((flag (list nil))"
        "      (lk (mp:make-lock))"
        "      (cv (mp:make-condition-variable)))"
        "  (let ((thr (mp:make-thread"
        "               (lambda ()"
        "                 (mp:acquire-lock lk)"
        "                 (loop until (car flag)"
        "                       do (mp:condition-wait cv lk))"
        "                 (mp:release-lock lk)"
        "                 (car flag)))))"
        "    (mp:thread-yield)"
        "    (mp:interrupt-thread thr"
        "      (lambda () (setf (car flag) :interrupted)))"
        "    (mp:acquire-lock lk)"
        "    (mp:condition-notify cv)"
        "    (mp:release-lock lk)"
        "    (mp:join-thread thr)))");
    ASSERT_STR_EQ(r, ":INTERRUPTED");
}

TEST(interrupt_thread_self)
{
    /* Interrupting current thread calls function immediately */
    const char *r = eval_print(
        "(let ((x 0))"
        "  (mp:interrupt-thread (mp:current-thread)"
        "    (lambda () (setf x 42)))"
        "  x)");
    ASSERT_STR_EQ(r, "42");
}

/* ================================================================
 * destroy-thread
 * ================================================================ */

TEST(destroy_thread_basic)
{
    /* Destroyed thread should be joinable and report aborted status */
    const char *r = eval_print(
        "(let ((thr (mp:make-thread"
        "             (lambda ()"
        "               (loop (mp:thread-yield))))))"
        "  (mp:thread-yield)"
        "  (mp:destroy-thread thr)"
        "  (mp:join-thread thr)"
        "  :destroyed)");
    ASSERT_STR_EQ(r, ":DESTROYED");
}

TEST(destroy_thread_not_alive_after)
{
    const char *r = eval_print(
        "(let ((thr (mp:make-thread"
        "             (lambda ()"
        "               (loop (mp:thread-yield))))))"
        "  (mp:thread-yield)"
        "  (mp:destroy-thread thr)"
        "  (mp:join-thread thr)"
        "  (mp:thread-alive-p thr))");
    ASSERT_STR_EQ(r, "NIL");
}

TEST(destroy_thread_interrupts_sleep)
{
    /* SLEEP must surface a pending MP:DESTROY-THREAD before the full
     * nominal duration elapses; otherwise sento's
     * stop--with-wait--destroy-thread test (and any caller of
     * bt2:destroy-thread on a thread blocked in (sleep …)) waits the
     * full sleep and the test's wall-clock budget blows up.
     *
     * Spawn a thread that sleeps 30s, destroy it, then time the join.
     * Should join in well under one second on a healthy build. */
    const char *r = eval_print(
        "(let* ((thr (mp:make-thread (lambda () (sleep 30))))"
        "       (start (get-internal-real-time)))"
        "  (sleep 0.05)"
        "  (mp:destroy-thread thr)"
        "  (mp:join-thread thr)"
        "  (let ((elapsed-ms (* 1000.0 (/ (- (get-internal-real-time) start)"
        "                                 internal-time-units-per-second))))"
        "    (if (< elapsed-ms 1000) :ok elapsed-ms)))");
    ASSERT_STR_EQ(r, ":OK");
}

/* ================================================================
 * GC stress under threads
 * ================================================================ */

TEST(thread_gc_stress)
{
    const char *r = eval_print(
        "(let ((t1 (mp:make-thread"
        "            (lambda () (length (make-list 200)))))"
        "      (t2 (mp:make-thread"
        "            (lambda () (length (make-list 200))))))"
        "  (+ (mp:join-thread t1) (mp:join-thread t2)))");
    ASSERT_STR_EQ(r, "400");
}

/* Recursive lock: same thread acquires twice and releases twice without
 * deadlocking. Required by log4cl/serapeum which use bt:make-recursive-lock
 * and bt:with-recursive-lock-held with potential nesting. */
TEST(recursive_lock_self_reacquire)
{
    const char *r = eval_print(
        "(let ((lk (mp:make-recursive-lock \"rec\")))"
        "  (mp:acquire-lock lk t)"
        "  (mp:acquire-lock lk t)"
        "  (mp:release-lock lk)"
        "  (mp:release-lock lk)"
        "  :ok)");
    ASSERT_STR_EQ(r, ":OK");
}

TEST(recursive_lock_with_nested_macro)
{
    const char *r = eval_print(
        "(let ((lk (mp:make-recursive-lock)))"
        "  (mp:with-recursive-lock-held (lk)"
        "    (mp:with-recursive-lock-held (lk)"
        "      :inner)))");
    ASSERT_STR_EQ(r, ":INNER");
}

/* (mp:current-thread) must return the same eq object across calls so it
 * can be used as a hash-table key (required by bordeaux-threads-2's
 * .known-threads. registry). */
TEST(current_thread_identity_main)
{
    const char *r = eval_print(
        "(eq (mp:current-thread) (mp:current-thread))");
    ASSERT_STR_EQ(r, "T");
}

/* Identity must also hold inside a worker thread, AND between the value
 * returned by mp:make-thread and (mp:current-thread) called from inside. */
TEST(current_thread_identity_in_worker)
{
    const char *r = eval_print(
        "(let* ((slot nil)"
        "       (th (mp:make-thread"
        "             (lambda () (setq slot (mp:current-thread)) t))))"
        "  (mp:join-thread th)"
        "  (eq slot th))");
    ASSERT_STR_EQ(r, "T");
}

/* Locks are plain heap words: there is no side table and no cap, so
 * making thousands of them (sento's `ask-s` allocates one per call) is
 * just allocation.  Kept as the smoke test for that path. */
TEST(lock_table_slot_reclaimed_by_gc)
{
    const char *r = eval_print(
        "(progn (dotimes (i 4000) (mp:make-lock)) :ok)");
    ASSERT_STR_EQ(r, ":OK");
}

/* Same for condition-variables. */
TEST(condvar_table_slot_reclaimed_by_gc)
{
    const char *r = eval_print(
        "(progn (dotimes (i 4000) (mp:make-condition-variable)) :ok)");
    ASSERT_STR_EQ(r, ":OK");
}

/* CL_MAX_THREADS = 256, but sento's actor-system pattern creates many
 * worker threads per test fixture and shuts them down without explicitly
 * calling mp:join-thread.  thread_entry sets status=2 and unregisters
 * the worker from cl_thread_list, but until 2026-04-28 the side-table
 * slot was never reclaimed.  After ~50 sento tests the cascade
 * "MP:MAKE-THREAD: thread table full (max 256)" failed every fixture in
 * AGENT.ARRAY-TESTS and beyond.
 *
 * Fix: gc_finalize_dead reclaims the slot when the wrapping CL_ThreadObj
 * is unreachable (which implies the worker is unregistered, since while
 * registered gc_mark_thread_roots keeps the wrapper alive via
 * t->thread_obj).  bi_make_thread runs cl_gc and retries on table-full,
 * mirroring the lock-allocator pattern.
 *
 * Test creates >> 256 unjoined threads.  Polls thread-alive-p so each
 * worker is guaranteed to have set status=2 + unregistered before we
 * drop the wrapper, eliminating scheduling flakiness. */
TEST(thread_table_slot_reclaimed_by_gc)
{
    const char *r = eval_print(
        "(progn"
        "  (dotimes (i 400)"
        "    (let ((th (mp:make-thread (lambda () nil))))"
        "      (do () ((not (mp:thread-alive-p th)))"
        "        (mp:thread-yield))))"
        "  :ok)");
    ASSERT_STR_EQ(r, ":OK");
}

/* Slot-reuse race: bi_join_thread frees the side-table slot, but the
 * Lisp-visible CL_ThreadObj wrapper keeps the now-stale thread_id.  If
 * MAKE-THREAD reuses that slot for a new worker, and the OLD wrapper
 * later becomes unreachable, gc_finalize_dead would key off the stale
 * id and free the unrelated new worker — corrupting whatever code was
 * still holding the new thread.  Fixed by setting tobj->thread_id to
 * (uint32_t)-1 in bi_join_thread after the slot is released. */
TEST(thread_slot_reuse_after_join_no_double_free)
{
    const char *r = eval_print(
        "(let ((th1 (mp:make-thread (lambda () 42))))"
        "  (mp:join-thread th1)"
        "  (let ((th2 (mp:make-thread (lambda () 99))))"
        "    (setq th1 nil)"
        "    (gc)"
        "    (mp:join-thread th2)))");
    ASSERT_STR_EQ(r, "99");
}

/* Regression: a worker's function object must stay GC-protected for the
 * entire duration of cl_vm_apply.  thread_entry used to null t->result right
 * after grabbing func into a C local, which dropped the ONLY GC root the
 * parent installed (gc_roots[0] = &t->result, see bi_make_thread).  A
 * stop-the-world mark-and-sweep during the worker's abort-restart setup or
 * its apply would then sweep the (otherwise unreferenced) closure; the
 * worker would apply freed memory, error out, and join-thread would return
 * NIL instead of the real result.
 *
 * The worker body allocates nothing — 42 is a fixnum constant — so a non-42
 * result can ONLY mean the closure object itself was collected mid-apply.
 * Hammering (gc) on the parent each iteration deterministically lands a
 * sweep inside the vulnerable window: before the fix this counted ~4 bad
 * out of 1500 (reproducible every run); after the fix it is exactly 0. */
TEST(worker_function_gc_protected_across_apply)
{
    const char *r = eval_print(
        "(let ((bad 0))"
        "  (dotimes (i 1500)"
        "    (let ((th (mp:make-thread (lambda () 42))))"
        "      (gc) (gc)"
        "      (unless (eql (mp:join-thread th) 42)"
        "        (setq bad (1+ bad)))))"
        "  bad)");
    ASSERT_STR_EQ(r, "0");
}

/* Zombie reaper: when GC can't reclaim slots because user code still
 * holds wrappers (e.g. bordeaux-threads' .known-threads. registry, which
 * is non-weak in AmiLisp), bi_make_thread falls back to scanning the
 * table for status>=2 entries and reaps them, invalidating the
 * wrapper's thread_id so it becomes a zombie (alive-p => NIL).
 *
 * Test: pin every spawned wrapper into a hash table so GC can't free
 * them, then create > CL_MAX_THREADS workers.  Without the reaper,
 * make-thread would error after 256.  After the reaper kicks in, the
 * pinned wrappers should report thread-alive-p => NIL (zombie), and
 * make-thread should keep working. */
TEST(thread_zombie_reaper_under_pinned_wrappers)
{
    const char *r = eval_print(
        "(let ((registry (make-hash-table)))"
        "  (dotimes (i 400)"
        "    (let ((th (mp:make-thread (lambda () nil))))"
        "      (do () ((not (mp:thread-alive-p th)))"
        "        (mp:thread-yield))"
        "      (setf (gethash i registry) th)))"
        "  (let ((alive 0) (zombie 0))"
        "    (maphash (lambda (k v) (declare (ignore k))"
        "               (if (mp:thread-alive-p v)"
        "                   (incf alive)"
        "                   (incf zombie)))"
        "             registry)"
        "    (list :total (hash-table-count registry)"
        "          :alive alive :zombie zombie)))");
    /* All 400 wrappers retained, all should be zombie now. */
    ASSERT_STR_EQ(r, "(:TOTAL 400 :ALIVE 0 :ZOMBIE 400)");
}

/* MP:ALL-THREADS used to allocate fresh CL_ThreadObj wrappers for each
 * non-main slot, producing aliases that pointed to the same thread_id
 * as the canonical wrapper.  Now it reuses the canonical wrapper from
 * CL_Thread.thread_obj so that EQ identity matches across (current-
 * thread), (make-thread) and (all-threads). */
TEST(all_threads_returns_canonical_wrapper)
{
    const char *r = eval_print(
        "(let* ((th  (mp:make-thread (lambda () (mp:thread-yield) :done)))"
        "       (lst (mp:all-threads)))"
        "  (prog1 (eq th (find th lst))"
        "    (mp:join-thread th)))");
    ASSERT_STR_EQ(r, "T");
}

/* MP:ALL-THREADS must report only currently-running threads (per the
 * bordeaux-threads / SBCL / CCL contract), even if a finished worker's
 * wrapper is still held by user code (and therefore its slot in
 * cl_thread_table is still allocated, since gc_finalize_dead only
 * reclaims it once the wrapper is unreachable).
 *
 * Repro: spawn a no-op thread, wait until thread-alive-p flips to NIL,
 * then assert the wrapper is no longer in (mp:all-threads).  We avoid
 * (mp:join-thread) because join eagerly frees the slot and would mask
 * the bug — the production scenario is sento's actor-system shutdown
 * where workers exit naturally and wrappers linger in REPL history. */
TEST(all_threads_excludes_finished_workers)
{
    const char *r = eval_print(
        "(let ((th (mp:make-thread (lambda () :done))))"
        "  (loop while (mp:thread-alive-p th)"
        "        do (mp:thread-yield))"
        "  (let ((found (find th (mp:all-threads))))"
        "    (mp:join-thread th)"
        "    (if found :leaked :ok)))");
    ASSERT_STR_EQ(r, ":OK");
}

/* (abort) inside a worker thread must unwind cleanly to a top-level
 * ABORT restart established by thread_entry — matching SBCL/CCL's
 * with-simple-restart abort wrapper around every thread.  Without it,
 * frameworks (sento, log4cl) that deliberately invoke ABORT from a
 * handler-bind to kill a misbehaving worker would crash with
 * "Restart ABORT not found" instead of the worker dying cleanly. */
TEST(thread_top_level_abort_restart_kills_worker_cleanly)
{
    const char *r = eval_print(
        "(let ((th (mp:make-thread"
        "            (lambda ()"
        "              (handler-bind ((serious-condition #'abort))"
        "                (error \"die!\"))"
        "              :should-not-reach))))"
        "  (loop while (mp:thread-alive-p th) do (mp:thread-yield))"
        "  (mp:join-thread th))");
    /* After (abort) unwinds to thread top, the thread exits with NIL
     * result (not :SHOULD-NOT-REACH).  status=2 (finished cleanly). */
    ASSERT_STR_EQ(r, "NIL");
}

TEST(thread_direct_abort_call_unwinds_cleanly)
{
    /* Direct (abort) — no enclosing handler — also unwinds via the
     * thread-top restart, not the "no restart found" error path. */
    const char *r = eval_print(
        "(let ((th (mp:make-thread"
        "            (lambda () (abort) :unreachable))))"
        "  (mp:join-thread th))");
    ASSERT_STR_EQ(r, "NIL");
}

/* The thread-top ABORT restart now carries a :report ("Return to top
 * level"), so PRINC / ~A of it yields that description rather than the
 * escaped #<RESTART ABORT> fallback.  slynk derives its (:abort ...)
 * reason from this restart's report, so the report is the root-cause
 * fix for throw-to-toplevel reporting "NIL".  (eval_print prin1s the
 * returned string, hence the surrounding quotes.) */
TEST(thread_top_abort_restart_princ_shows_report)
{
    const char *r = eval_print(
        "(mp:join-thread (mp:make-thread"
        "  (lambda () (princ-to-string (find-restart 'abort)))))");
    ASSERT_STR_EQ(r, "\"Return to top level\"");
}

/* PRIN1 / ~S must be unchanged: the printer's escaped fallback form is
 * still used for the readable representation, so report-less restarts
 * (tests/test_condition.c lisp_restart_no_report_princ) are unaffected. */
TEST(thread_top_abort_restart_prin1_unchanged)
{
    const char *r = eval_print(
        "(mp:join-thread (mp:make-thread"
        "  (lambda () (prin1-to-string (find-restart 'abort)))))");
    ASSERT_STR_EQ(r, "\"#<RESTART ABORT>\"");
}

/* Two threads racing to allocate locks past the table limit must NOT
 * deadlock.  Used to: thread A grabbed gc_mutex inside cl_gc_stop_the_world
 * and waited for B to reach a safepoint; B was blocked on gc_mutex inside
 * its own cl_gc call and never yielded.  Fixed by trylock+safepoint loop
 * in cl_gc_stop_the_world. */
TEST(make_lock_concurrent_no_deadlock)
{
    const char *r = eval_print(
        "(let ((t1 (mp:make-thread"
        "            (lambda () (dotimes (i 2000) (mp:make-lock)) :a)))"
        "      (t2 (mp:make-thread"
        "            (lambda () (dotimes (i 2000) (mp:make-lock)) :b))))"
        "  (list (mp:join-thread t1) (mp:join-thread t2)))");
    ASSERT_STR_EQ(r, "(:A :B)");
}

/* ================================================================
 * MP:DUMP-THREAD-WAITS
 * ================================================================ */

/* (mp:dump-thread-waits) must return NIL without signaling an error,
 * even when called single-threaded (only the main thread is in the
 * table). */
TEST(dump_thread_waits_returns_nil)
{
    const char *r = eval_print("(mp:dump-thread-waits)");
    ASSERT_STR_EQ(r, "NIL");
}

/* With a live worker present, dump-thread-waits must still return NIL
 * and must not crash — the per-thread printing path is exercised. */
TEST(dump_thread_waits_with_live_worker)
{
    const char *r = eval_print(
        "(let* ((lk (mp:make-lock))"
        "       (cv (mp:make-condition-variable))"
        "       (ready (list nil))"
        "       (thr (mp:make-thread"
        "              (lambda ()"
        "                (mp:acquire-lock lk)"
        "                (setf (car ready) t)"
        "                (mp:condition-wait cv lk)"
        "                (mp:release-lock lk)))))"
        "  (mp:acquire-lock lk)"
        "  (loop until (car ready) do (mp:release-lock lk) (mp:thread-yield) (mp:acquire-lock lk))"
        "  (let ((result (mp:dump-thread-waits)))"
        "    (mp:condition-notify cv)"
        "    (mp:release-lock lk)"
        "    (mp:join-thread thr)"
        "    result))");
    ASSERT_STR_EQ(r, "NIL");
}

/* ================================================================
 * main
 * ================================================================ */

int main(void)
{
    test_init();
    setup();

    /* Thread creation and join */
    RUN(thread_make_and_join);
    RUN(thread_make_with_name);
    RUN(thread_join_returns_result);
    RUN(thread_join_cons_result);

    /* Worker VM sizes must match main (asdf:load-system under Sly hang) */
    RUN(worker_deep_recursion_survives_like_main);
    RUN(worker_deep_nlx_nesting_no_corruption);
    RUN(worker_handler_case_runs_after_deep_work);

    /* Per-thread size keywords */
    RUN(make_thread_size_keywords_grow_worker);
    RUN(make_thread_size_keywords_are_minimums);
    RUN(make_thread_size_keyword_rejects_bad_values);

    /* Thread predicates */
    RUN(thread_alive_p_before_join);
    RUN(thread_alive_p_after_join);

    /* Current thread */
    RUN(current_thread_returns_thread);
    RUN(all_threads_includes_main);
    RUN(main_thread_has_default_name);
    RUN(main_thread_name_via_all_threads);

    /* Yield */
    RUN(thread_yield_no_crash);

    /* Locks */
    RUN(lock_make);
    RUN(lock_acquire_release);
    RUN(lock_trylock);
    RUN(lock_with_lock_held);
    RUN(lock_contention_two_threads);

    /* Condition variables */
    RUN(condvar_make);
    RUN(condvar_notify_wait);
    RUN(condvar_make_with_name);
    RUN(condvar_name_accessor);
    RUN(condvar_name_nil_when_unnamed);

    /* Lock name */
    RUN(lock_name_accessor);
    RUN(lock_name_nil_when_unnamed);

    /* Type predicates */
    RUN(threadp_true);
    RUN(threadp_false);
    RUN(lockp_true);
    RUN(lockp_false);
    RUN(condition_variable_p_true);
    RUN(condition_variable_p_false);

    /* Interrupt thread */
    RUN(interrupt_thread_basic);
    RUN(interrupt_thread_self);

    /* Destroy thread */
    RUN(destroy_thread_basic);
    RUN(destroy_thread_not_alive_after);
    RUN(destroy_thread_interrupts_sleep);

    /* Dynamic bindings */
    RUN(thread_sees_global_package_value);
    RUN(thread_let_binding_isolation);
    RUN(thread_fresh_dynamic_env);

    /* Error handling */
    RUN(thread_error_sets_aborted);

    /* Multiple threads */
    RUN(thread_multiple_concurrent);

    /* GC stress */
    RUN(thread_gc_stress);

    /* Recursive locks (required by log4cl/serapeum) */
    RUN(recursive_lock_self_reacquire);
    RUN(recursive_lock_with_nested_macro);

    /* Thread object identity (required by bordeaux-threads-2 hash registry) */
    RUN(current_thread_identity_main);
    RUN(current_thread_identity_in_worker);

    /* GC reclaims MP table slots */
    RUN(lock_table_slot_reclaimed_by_gc);
    RUN(condvar_table_slot_reclaimed_by_gc);
    RUN(thread_table_slot_reclaimed_by_gc);
    RUN(thread_slot_reuse_after_join_no_double_free);
    RUN(worker_function_gc_protected_across_apply);
    RUN(thread_zombie_reaper_under_pinned_wrappers);
    RUN(all_threads_returns_canonical_wrapper);
    RUN(all_threads_excludes_finished_workers);
    RUN(make_lock_concurrent_no_deadlock);
    RUN(thread_top_level_abort_restart_kills_worker_cleanly);
    RUN(thread_direct_abort_call_unwinds_cleanly);
    RUN(thread_top_abort_restart_princ_shows_report);
    RUN(thread_top_abort_restart_prin1_unchanged);

    /* MP:DUMP-THREAD-WAITS */
    RUN(dump_thread_waits_returns_nil);
    RUN(dump_thread_waits_with_live_worker);

    teardown();

    REPORT();
}
