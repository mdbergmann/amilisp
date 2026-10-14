/*
 * builtins_thread.c — MP (multiprocessing) package builtins.
 *
 * Phase 4: CL-level threading API.
 * Threads, locks (mutexes), condition variables.
 */

#define CL_THREAD_NO_MACROS  /* access CL_Thread fields directly */
#include "thread.h"
#include "builtins.h"
#include "symbol.h"
#include "package.h"
#include "mem.h"
#include "error.h"
#include "vm.h"
#include "stream.h"
#include "float.h"
#include "compiler.h"
#include "string_utils.h"
#include "../platform/platform.h"
#include "../platform/platform_thread.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ================================================================
 * Pre-interned keyword symbols
 * ================================================================ */

static CL_Obj KW_NAME_THR = 0;  /* :NAME keyword for make-thread */
/* Per-thread size keywords for make-thread.  Each is a MINIMUM the worker's
 * budget is raised to (never lowered below the platform defaults) — see
 * cl_thread_alloc_worker_sized.  Motivated by the AmigaOS worker walls: the
 * compile-time CL_WORKER_* defaults must stay at their historical values
 * (layout-fragile moving-GC bug, see thread.h), but a runtime opt-in only
 * changes the requesting worker's malloc'd arrays and OS stack. */
static CL_Obj KW_STACK_SIZE_THR    = 0;  /* :STACK-SIZE (C stack, bytes) */
static CL_Obj KW_VM_STACK_SIZE_THR = 0;  /* :VM-STACK-SIZE (entries) */
static CL_Obj KW_VM_FRAMES_THR     = 0;  /* :VM-FRAMES (call frames) */
static CL_Obj KW_NLX_FRAMES_THR    = 0;  /* :NLX-FRAMES (NLX frames) */

/* ================================================================
 * Main thread's Lisp-visible thread object
 * ================================================================ */

static CL_Obj main_thread_obj = CL_NIL;

/* Cached `(lambda (&rest _) nil)` used as the thread-top ABORT
 * restart handler.  bi_invoke_restart calls the handler before
 * throwing to the catch tag, so we need a callable that swallows
 * any args and returns NIL. */
static CL_Obj thread_abort_handler = CL_NIL;

static CL_Obj get_thread_abort_handler(void)
{
    /* Lazy init must be serialized: two racing first-worker creations
     * would otherwise BOTH see NIL and BOTH cl_gc_register_root the
     * same address.  gc_forward is not idempotent, so a double-
     * registered root is forwarded twice on compaction and ends up
     * pointing at an unrelated object (and the unlocked
     * n_global_roots++ can additionally lose a concurrent
     * registration).  The roots themselves are registered once in
     * cl_builtins_thread_init (registering a NIL-holding global is
     * free); only the cached value is built lazily.
     *
     * cl_thread_list_lock must never be held across the allocating
     * cl_eval_string call below: it is the exact lock cl_gc_stop_the_world
     * takes to enumerate threads, and it is explicitly on the "do NOT use
     * cl_gc_safe_mutex_lock" list in thread.c (its critical section is one
     * GC itself touches).  Holding a raw platform_mutex_lock on it across
     * an allocating call risks the lock's holder self-deadlocking as STW
     * initiator, or a peer blocked on the lock (not at a safepoint) hanging
     * STW's wait loop.  So build the value with no lock held at all, and
     * only take the lock — briefly, non-allocating — to publish it
     * first-writer-wins; a losing racer's redundant value is simply
     * unreferenced and collected normally. */
    if (CL_NULL_P(thread_abort_handler)) {
        extern CL_Obj cl_eval_string(const char *str);
        CL_Obj built = cl_eval_string(
            "(lambda (&rest args) (declare (ignore args)) nil)");
        platform_mutex_lock(cl_thread_list_lock);
        if (CL_NULL_P(thread_abort_handler))
            thread_abort_handler = built;
        platform_mutex_unlock(cl_thread_list_lock);
    }
    return thread_abort_handler;
}

/* Cached :report string for the thread-top ABORT restart.  Shared
 * read-only by every worker thread's restart — safe because strings
 * are immutable here and the printer only reads it.  Wording matches
 * amilisp's interactive debugger (src/core/debugger.c). */
static CL_Obj thread_abort_report = CL_NIL;

static CL_Obj get_thread_abort_report(void)
{
    /* See get_thread_abort_handler for why this is locked, why the lock is
     * never held across the allocating call, and why the root registration
     * lives in cl_builtins_thread_init. */
    if (CL_NULL_P(thread_abort_report)) {
        CL_Obj built = cl_make_string("Return to top level", 19);
        platform_mutex_lock(cl_thread_list_lock);
        if (CL_NULL_P(thread_abort_report))
            thread_abort_report = built;
        platform_mutex_unlock(cl_thread_list_lock);
    }
    return thread_abort_report;
}

/* Accessor for GC root marking (called from mem.c) */
CL_Obj cl_main_thread_lisp_obj(void)
{
    return main_thread_obj;
}

/* Pointer accessor for GC compaction (called from mem.c) */
CL_Obj *cl_main_thread_lisp_obj_ptr(void)
{
    return &main_thread_obj;
}

/* ================================================================
 * Thread entry wrapper
 *
 * The OS thread runs this function.  It:
 * 1. Sets up TLS so CT points to this thread's CL_Thread
 * 2. Inherits dynamic bindings (TLV snapshot done by parent)
 * 3. Sets up error handler to catch all errors
 * 4. Calls the user function
 * 5. Stores result and sets status
 * 6. Cleans up
 * ================================================================ */

static void *thread_entry(void *arg)
{
    CL_Thread *t = (CL_Thread *)arg;
    CL_Obj func;
    int err;

    /* 1. Set up TLS for this OS thread */
    platform_tls_set(t);

    /* 1b. This thread's MP park handle (specs/mp-locks-heap-words.md).
     *     Created by the thread itself: on AmigaOS it is a signal bit of
     *     the task that will Wait() on it.  A failure leaves it NULL and
     *     the lock/condvar slow paths sleep-poll instead of parking. */
    if (platform_park_init(&t->park) != 0)
        t->park = NULL;

    /* 1a. Come online for stop-the-world GC.  Until now the child was a
     *     newborn: registered in cl_thread_list (so GC marks/forwards its
     *     roots — notably t->result below) but invisible to the STW wait loop.
     *     This barrier transitions it to `gc_live` under gc_mutex, parking here
     *     if a peer STW is already in progress so we never touch the stopped
     *     heap.  MUST run before the `func = t->result` read: if a compaction
     *     ran during the parent's create window, t->result was forwarded and
     *     we must read the post-barrier (up-to-date) value. */
    cl_gc_thread_online(t);

    /* 2. TLV snapshot was done by parent before thread_create */

    /* 3. Mark as running */
    t->status = 1;

    /* 3a. Initialize this thread's current-package slot from the GLOBAL
     *     value of *PACKAGE* — a new thread starts with a fresh dynamic
     *     environment (see the make-thread note in bi_make_thread), so it
     *     sees the global value, never the parent's dynamic binding.
     *     Reading AFTER the online barrier (step 1a) is deliberate: any
     *     compaction during the parent's create window has already
     *     forwarded the symbol's value slot.  Without this the slot is
     *     CL_NIL and the thread's reader would intern package-less. */
    {
        CL_Obj gpkg = CL_NIL;
        if (CL_SYMBOL_P(SYM_STAR_PACKAGE))
            gpkg = ((CL_Symbol *)CL_OBJ_TO_PTR(SYM_STAR_PACKAGE))->value;
        t->current_package = CL_PACKAGE_P(gpkg) ? gpkg : cl_package_cl_user;
    }

    /* 4. Retrieve stashed function (stored in result field by parent).
     *
     *    DO NOT clear t->result here.  t->result is a GC root: gc_visit_thread_roots
     *    marks and forwards it as thread metadata, so it is the ONLY thing
     *    keeping `func` (a freshly-allocated
     *    closure object) alive while we apply it below.  Nulling it would leave
     *    `func` reachable only through this unrooted C local — a concurrent
     *    stop-the-world mark-and-sweep (e.g. another thread calling (gc))
     *    would then sweep the closure mid-apply, and cl_vm_apply would run
     *    on freed memory, erroring out so t->result stays NIL and
     *    join-thread returns NIL instead of the real result.  We overwrite
     *    t->result with the computed result only AFTER cl_vm_apply returns.
     *
     *    Reading t->result AFTER cl_gc_thread_online (step 1a) is deliberate:
     *    any compaction during the parent's create window has already forwarded
     *    this slot, so we read the live (relocated) closure offset. */
    func = t->result;

    /* func stays GC-safe via t->result (see the note above at step 4) —
     * no gc_roots[] entry is added here; do not reintroduce one. */

    /* 5. Call user function inside error handler.
     *    CL_CATCH/CL_UNCATCH macros use compatibility names that are
     *    suppressed by CL_THREAD_NO_MACROS, so expand them inline.
     *
     *    Mirror cl_error_frame_push(): snapshot gc_root_count into
     *    saved_gc_roots so cl_error_unwind can drop CL_GC_PROTECT entries
     *    that belong to C frames it is unwinding out of.  Without this,
     *    a worker's stale gc_roots[] survives the longjmp back here and
     *    a subsequent gc_visit_thread_roots walks dangling stack pointers
     *    — manifesting as gc_mark SEGV under sento workloads. */
    if (t->error_frame_top < CL_MAX_ERROR_FRAMES) {
        t->error_frames[t->error_frame_top].active = 1;
        t->error_frames[t->error_frame_top].saved_gc_roots = t->gc_root_count;
        err = CL_SETJMP(t->error_frames[t->error_frame_top++].buf);
    } else {
        err = CL_ERR_OVERFLOW;
    }

    /* Status to publish at the very end — see ordering note at end of
     * function.  2 = finished cleanly (or via ABORT), 3 = errored. */
    int final_status = 3;

    if (err == 0) {
        CL_Obj result = CL_NIL;
        CL_Obj abort_handler;
        CL_Obj abort_report;
        CL_Obj abort_tag;
        int my_nlx_idx = -1;
        int my_restart_idx = -1;
        int aborted = 0;

#ifdef DEBUG_THREAD
        fprintf(stderr, "[THR] tid=%u CT=%p func=0x%08x type=%d starting\n",
                t->id, (void *)t, func,
                CL_HEAP_P(func) ? (int)CL_HDR_TYPE(CL_OBJ_TO_PTR(func)) : -1);
        fflush(stderr);
#endif

        /* Establish a top-level ABORT restart for the thread.
         * `(abort)` anywhere in the thread body unwinds here and
         * the thread exits cleanly — matching SBCL/CCL semantics
         * (with-simple-restart abort at the top of every thread).
         * Without this, `(abort)` in a worker raises
         * "Restart ABORT not found" which is not what frameworks
         * (bordeaux-threads, sento) expect. */
        /* Pre-compute report before abort_tag is allocated: on first thread
         * creation get_thread_abort_report() calls cl_make_string (allocating),
         * which can trigger compaction.  If called as an argument expression
         * inside cl_make_restart() after abort_tag is assigned, C's
         * unspecified evaluation order may read abort_tag after relocation. */
        abort_report  = get_thread_abort_report();
        /* GC-protect abort_report across the remaining allocations: on a worker
         * thread this setup runs concurrently with other threads, so any of the
         * allocating calls below (get_thread_abort_handler, cl_cons,
         * cl_make_restart) can be interrupted by a PEER thread's stop-the-world
         * compaction that relocates these objects.  Without protection, the
         * unprotected C locals hold stale offsets and cl_make_restart bakes them
         * into the abort restart — permanently corrupting restart_stack[0], so
         * every later GC marks a garbage offset (ORing the mark bit into a live
         * data word).  Single-thread never hit this: the worker's own allocs
         * only compact under this thread, at points the convention already
         * covers, and the main thread's abort setup runs before any peer exists. */
        CL_GC_PROTECT(abort_report);
        abort_handler = get_thread_abort_handler();
        CL_GC_PROTECT(abort_handler);
        abort_tag = cl_cons(SYM_ABORT, CL_NIL);
        CL_GC_PROTECT(abort_tag);

        if (t->nlx_top < t->nlx_max) {
            CL_NLXFrame *frame = &t->nlx_stack[t->nlx_top];
            frame->type = CL_NLX_CATCH;
            frame->tag = abort_tag;
            frame->vm_sp = t->vm.sp;
            frame->vm_fp = t->vm.fp;
            frame->result = CL_NIL;
            /* Marked by every GC (see bi_warn's frame push): a C frame has
             * no bytecode, and whatever the slot held before must not be
             * mistaken for one. */
            frame->bytecode = CL_NIL;
            frame->dyn_mark = t->dyn_top;
            frame->handler_mark = t->handler_top;
            /* Snapshot the disabled-handler band like the vm.c/jit NLX setup
             * sites, so an unwind to this thread-abort frame restores it
             * consistently (harmless today — mask is 0 at thread entry — but
             * required once a handler is established before this frame). */
            frame->handler_active_mask = t->handler_active_mask;
            frame->restart_mark = t->restart_top;
            frame->gc_root_mark = t->gc_root_count;
            frame->mv_count = 1;
            frame->mv_save_mark = t->mv_save_top;
            frame->landing = NULL;   /* C-owned frame: cl_nlx_jump uses buf */
            my_nlx_idx = t->nlx_top;
            t->nlx_top++;

            if (t->restart_top < CL_MAX_RESTART_BINDINGS) {
                CL_Obj abort_restart =
                    cl_make_restart(SYM_ABORT, abort_handler,
                                    abort_report,
                                    CL_NIL, CL_NIL, abort_tag);
                {
                    CL_Restart *rp = (CL_Restart *)CL_OBJ_TO_PTR(abort_restart);
                    t->restart_stack[t->restart_top].name    = rp->name;
                    t->restart_stack[t->restart_top].handler = rp->function;
                    t->restart_stack[t->restart_top].tag     = rp->tag;
                    t->restart_stack[t->restart_top].restart = abort_restart;
                }
                my_restart_idx = t->restart_top;
                t->restart_top++;
            }

            /* abort_tag/report/handler are now rooted via the nlx frame and the
             * abort restart entry (or discarded); release the startup pins.
             * Done before CL_SETJMP so the normal path balances; the abort
             * longjmp path restores gc_root_count from frame->gc_root_mark. */
            CL_GC_UNPROTECT(3);

            if (CL_SETJMP(frame->buf) != 0)
                aborted = 1;
        }

        if (!aborted) {
            /* Re-read func from t->result: the abort-restart setup above
             * (cl_cons / cl_make_restart, and get_thread_abort_* on the first
             * thread) allocates, and any of those allocations — or a peer
             * thread's stop-the-world compaction during them — can relocate the
             * closure.  t->result is a GC root (marked+forwarded as thread
             * metadata), so it holds the live offset; the `func` C-local read
             * back at thread entry does NOT get forwarded and is now stale.
             * Applying the stale local was the multi-thread "Not a function:
             * heap object type N" corruption. */
            func = t->result;
            result = cl_vm_apply(func, NULL, 0);
            t->result = result;
        } else {
            /* (abort) was invoked — thread exits with NIL result */
            t->result = CL_NIL;
        }
        final_status = 2; /* finished (cleanly or via abort) — published below */

#ifdef DEBUG_THREAD
        fprintf(stderr, "[THR] tid=%u CT=%p finished result=0x%08x aborted=%d\n",
                t->id, (void *)t, t->result, aborted);
        fflush(stderr);
#endif

        /* Pop NLX/restart frames on normal completion.
         * Aborted path: cl_throw_to_tag truncated t->nlx_top to my_nlx_idx
         * already; replicate the OP_CATCH restore for handler/restart/dyn. */
        if (!aborted) {
            if (my_nlx_idx >= 0) t->nlx_top = my_nlx_idx;
            if (my_restart_idx >= 0) t->restart_top = my_restart_idx;
        } else {
            t->restart_top = t->nlx_stack[my_nlx_idx].restart_mark;
            t->handler_top = t->nlx_stack[my_nlx_idx].handler_mark;
            t->handler_active_mask = t->nlx_stack[my_nlx_idx].handler_active_mask;
            cl_dynbind_restore_to(t->nlx_stack[my_nlx_idx].dyn_mark);
        }
    } else {
        /* Thread errored — final_status stays at 3 (aborted by error).
         * Clear the stashed function out of t->result so JOIN-THREAD on an
         * errored worker returns NIL (its prior behaviour) rather than the
         * leftover closure object we deliberately kept there for GC during
         * the apply above. */
        t->result = CL_NIL;
        /* Diagnostic: a worker that dies via an uncaught error otherwise
         * vanishes silently (status 3), which is very hard to debug — e.g. a
         * Hunchentoot request thread aborting mid-handler just closes the
         * connection.  Surface the error code+message when CLAMIGA_THREAD_ERRORS
         * is set in the environment. */
        {
            char envbuf[8];
            if (platform_getenv("CLAMIGA_THREAD_ERRORS", envbuf, sizeof(envbuf))) {
                fprintf(stderr,
                        "[THREAD-ERROR] worker tid=%u died: err=%d msg=\"%s\"\n",
                        t->id, err,
                        t->pending_error_msg[0] ? t->pending_error_msg :
                        t->error_msg[0] ? t->error_msg : "(none)");
                fflush(stderr);
            }
        }
        /* Tell whoever asked to hear of it (MP:*THREAD-DEATH-HOOKS*), while
         * this thread's own error frame still catches what a hook cannot
         * and the thread is still registered, so a collection the hook
         * triggers sees its roots.  Not for a death MP:DESTROY-THREAD asked
         * for, and not for a (QUIT) called on a worker. */
        if (!t->destroyed && err != CL_ERR_EXIT)
            cl_run_thread_death_hooks(t->error_msg);
    }

    /* CL_UNCATCH inline */
    if (t->error_frame_top > 0) {
        t->error_frame_top--;
        t->error_frames[t->error_frame_top].active = 0;
    }

    /* 6. Clear gc_roots — we're done using func */
    t->gc_root_count = 0;

    /* 6a. Publish the result into the GC-managed wrapper BEFORE unregistering.
     *   After step 7 unregisters this worker, gc_visit_thread_roots no longer
     *   marks or forwards t->result, so a peer thread's compaction that runs
     *   while JOIN-THREAD is parked (in cl_gc_leave_safe_region around
     *   pthread_join) would sweep or relocate the result object out from under
     *   the reader — JOIN-THREAD then returns a stale offset (garbage / an
     *   unrelated live object).  The wrapper (CL_ThreadObj) IS a GC-managed
     *   heap object, kept reachable by the user's thread reference and
     *   marked+forwarded via gc_{mark,update} TYPE_THREAD, so storing the
     *   result there keeps it live and current until JOIN reads it.  We are
     *   still registered here, so t->thread_obj is a valid, forwarded offset. */
    if (CL_THREAD_P(t->thread_obj)) {
        CL_ThreadObj *wrap = (CL_ThreadObj *)CL_OBJ_TO_PTR(t->thread_obj);
        wrap->result = t->result;
    }

    /* 7. Unregister from thread list BEFORE publishing the terminal status.
     *
     *   Both bi_make_thread's zombie reaper and gc_finalize_dead(TYPE_THREAD)
     *   treat `status >= 2` as the signal that `t` is safe to free.  If we
     *   set status first, there is a window where this worker is still
     *   linked in `cl_thread_list` AND status >= 2 — the reaper would free
     *   `t` (along with `t->vm.stack`, `t->vm.frames`, `t->nlx_stack`),
     *   leaving a dangling pointer in `cl_thread_list`.  The next
     *   stop-the-world `gc_mark` walks that list and SEGVs in
     *   `gc_visit_thread_roots` while reading freed memory.
     *
     *   By unregistering FIRST, status >= 2 implies "no longer in
     *   cl_thread_list", and gc_mark cannot reach a freed worker via the
     *   list walk.
     */
    cl_thread_unregister(t);

    /* 7a. Free the park handle.  Off the list, no release scan or notify
     *     can reach it, and interrupt delivery unparks only a thread with a
     *     live wait registration (we have none).  Cleared under the list
     *     lock so a peer that reads `park` under it never sees a freed
     *     handle; freed by this task itself (AmigaOS: FreeSignal must be
     *     called by the owning task).  The lock may already be torn down
     *     during process exit (see cl_thread_unregister). */
    if (t->park) {
        void *p = t->park;
        if (cl_thread_list_lock) platform_mutex_lock(cl_thread_list_lock);
        t->park = NULL;
        if (cl_thread_list_lock) platform_mutex_unlock(cl_thread_list_lock);
        platform_park_destroy(p);
    }

    /* 8. Publish terminal status LAST.  After this write, observers
     *    (mp:thread-alive-p, the reaper, gc_finalize_dead) may free `t` at
     *    any moment.  The OS thread must not touch `t` from here on. */
    t->status = final_status;

    return NULL;
}

/* ================================================================
 * Thread builtins
 * ================================================================ */

/* (mp:make-thread function &key name) -> thread */
/* Parse one of make-thread's size keywords: a positive fixnum <= `cap`.
 * The caps are typo-catchers (e.g. entries-vs-bytes confusion), far above
 * any realistic budget; the platform-default FLOORS are applied later by
 * cl_thread_alloc_worker_sized / the platform thread layer. */
static uint32_t mt_size_arg(CL_Obj val, const char *kw, uint32_t cap)
{
    int32_t v;
    if (!CL_FIXNUM_P(val) || (v = CL_FIXNUM_VAL(val)) <= 0 ||
        (uint32_t)v > cap)
        cl_error(CL_ERR_TYPE,
                 "MP:MAKE-THREAD: %s must be a positive fixnum <= %u",
                 kw, (unsigned)cap);
    return (uint32_t)v;
}

static CL_Obj bi_make_thread(CL_Obj *args, int n)
{
    CL_Obj func = args[0];
    CL_Obj name = CL_NIL;
    CL_Thread *child;
    int thread_id;
    CL_ThreadObj *tobj;
    CL_Obj thread_obj;
    uint32_t c_stack = 0, vm_stack = 0, vm_frames = 0, nlx_frames = 0;
    int i;

    /* Parse &key name stack-size vm-stack-size vm-frames nlx-frames.
     * Unknown keywords are ignored (bordeaux-threads passes extras).
     * The size keywords are MINIMUMS — values below the platform defaults
     * are raised to them, so a worker can only be grown. */
    for (i = 1; i + 1 < n; i += 2) {
        if (args[i] == KW_NAME_THR)
            name = args[i + 1];
        else if (args[i] == KW_STACK_SIZE_THR)
            c_stack = mt_size_arg(args[i + 1], ":STACK-SIZE",
                                  64u * 1024u * 1024u);
        else if (args[i] == KW_VM_STACK_SIZE_THR)
            vm_stack = mt_size_arg(args[i + 1], ":VM-STACK-SIZE",
                                   1u << 20);
        else if (args[i] == KW_VM_FRAMES_THR)
            vm_frames = mt_size_arg(args[i + 1], ":VM-FRAMES", 65536u);
        else if (args[i] == KW_NLX_FRAMES_THR)
            nlx_frames = mt_size_arg(args[i + 1], ":NLX-FRAMES", 65536u);
    }

    /* Validate function argument */
    func = cl_coerce_funcdesig(func, "MP:MAKE-THREAD");

    /* Allocate worker CL_Thread (0 = platform default for each size) */
    child = cl_thread_alloc_worker_sized(vm_stack, vm_frames, nlx_frames);
    if (!child)
        cl_error(CL_ERR_STORAGE, "MP:MAKE-THREAD: cannot allocate thread");

    /* Allocate side table slot.  The table is bounded but slots are
     * reclaimed at GC sweep when the wrapping CL_ThreadObj becomes
     * unreachable (see gc_finalize_dead, TYPE_THREAD).  Run GC once
     * and retry — same pattern as bi_make_lock.
     *
     * func/name are plain CL_Obj C locals (copies of the still-rooted
     * args[] slots), not GC roots themselves: both cl_gc_reclaim_young()
     * and cl_gc() below are moving collections under the generational
     * collector, so they must stay GC-protected across this entire span,
     * not just around the final cl_alloc(). */
    CL_GC_PROTECT(func);
    CL_GC_PROTECT(name);
    thread_id = cl_thread_table_alloc(child);
    if (thread_id < 0) {
        /* Dead thread objects are mostly RECENT — a minor cycle usually
         * frees their slots; a full collection is the second resort. */
        cl_gc_reclaim_young();
        thread_id = cl_thread_table_alloc(child);
    }
    if (thread_id < 0) {
        cl_gc();
        thread_id = cl_thread_table_alloc(child);
    }
    if (thread_id < 0) {
        /* GC didn't free any slot: every occupant has a still-reachable
         * wrapper.  In real workloads this happens because external
         * registries (e.g. bordeaux-threads' .known-threads. weak hash
         * — which is non-weak under cl-amiga today) hold wrappers for
         * workers that have long since finished.  Reap any slot whose
         * worker reached status >= 2 (finished/aborted): NULL the slot,
         * detach the OS handle, free the worker.  The wrapper stays
         * alive (so EQ identity survives, name accessor still works),
         * and gc_finalize_dead's table_gen compare (see mem.c) prevents
         * the wrapper's eventual finalize from double-freeing an
         * unrelated worker if the slot is later reused: reaping here
         * does not bump cl_thread_table_gen[slot], so once a new worker
         * later reoccupies the slot the wrapper's stale table_gen no
         * longer matches and its finalize leaves the new occupant alone.
         * The same walk runs at shutdown (cl_thread_reap_zombies). */
        cl_thread_reap_zombies();
        thread_id = cl_thread_table_alloc(child);
    }
    if (thread_id < 0) {
        CL_GC_UNPROTECT(2);
        cl_thread_free_worker(child);
        cl_error(CL_ERR_GENERAL, "MP:MAKE-THREAD: thread table full (max %d)",
                 CL_MAX_THREADS);
    }

    child->id = (uint32_t)thread_id;
    child->name = name;

    /* A new thread starts with a FRESH dynamic environment: per CL /
     * bordeaux-threads semantics it sees only the GLOBAL values of special
     * variables, never the dynamic (LET/PROGV) bindings active in the parent
     * at spawn time.  Global values live in the symbol's value slot; the
     * worker's TLV table holds only that thread's own active bindings.  Since
     * cl_thread_alloc_worker() zeroes the worker, its TLV table is already
     * empty (tlv_entry_count == 0) and reads fall through to globals — so we
     * deliberately do NOT copy the parent's TLV table here. */

    /* Stash func in child->result for the entry wrapper to retrieve.
     *
     * child->result (and child->name above) are GC roots automatically: once
     * the child is registered, gc_visit_thread_roots marks t->result / t->name
     * as thread metadata and forwards those exact slots after a compaction
     * (gc_trace.c).  So the closure stays live and its offset is kept current
     * across any peer thread's stop-the-world GC.
     *
     * Do NOT also register &child->result / &child->name in child->gc_roots[].
     * Historically that forwarded each slot TWICE — once via the gc_roots[]
     * entry and once via the direct t->result / t->name update — and
     * gc_forward() is not idempotent: re-forwarding an already-relocated offset
     * re-maps it through whatever live object now occupies that arena slot,
     * leaving child->result pointing at the wrong object.  That was the
     * multi-thread "func has wrong type at thread entry → TYPE-ERROR" corruption
     * exposed once concurrent make-thread stopped hanging.  (The compactor's
     * registered-root pass now skips a registration the thread walk already
     * covers, so the double registration would merely be dead weight — still
     * don't.)  gc_root_count stays
     * 0; the worker uses gc_roots[] for its own CL_GC_PROTECT calls. */
    child->result = func;

    /* Register child in thread list BEFORE creating OS thread, so GC can mark
     * its roots even though the child hasn't started yet.  Register it as a
     * NEWBORN (gc_live == 0): it has no OS thread and cannot reach a safepoint,
     * so a concurrent stop-the-world must NOT wait for it — otherwise the world
     * hangs forever waiting for a thread that will never stop.  The child flips
     * itself to live via cl_gc_thread_online once its OS thread starts. */
    cl_thread_register_newborn(child);

    /* Create the Lisp-visible thread object.
     * ORDERING NOTE: this cl_alloc is also a mandatory SAFEPOINT between
     * registering the newborn and creating its OS thread — historically
     * the only thing closing the register-after-STW-request race.  The
     * STW wait loop now re-requests unrequested live threads on every
     * rescan (thread.c), so the invariant is structural, but keep the
     * register→alloc→create order anyway.
     *
     * func/name are still the protect pair pushed before the table-alloc
     * retries above; unprotect here once this cl_alloc (their last
     * allocation-adjacent use) is done. */
    tobj = (CL_ThreadObj *)cl_alloc(TYPE_THREAD, sizeof(CL_ThreadObj));
    CL_GC_UNPROTECT(2);
    if (!tobj) {
        cl_thread_unregister(child);
        cl_thread_table_free(thread_id);
        cl_thread_free_worker(child);
        cl_error(CL_ERR_STORAGE, "MP:MAKE-THREAD: cannot allocate thread object");
    }
    tobj->thread_id = (uint32_t)thread_id;
    /* Safe to read unlocked: the slot's gen only changes when the slot is
     * re-claimed, which cannot happen while our worker occupies it. */
    tobj->table_gen = cl_thread_table_gen[thread_id];
    tobj->name = name;
    thread_obj = CL_PTR_TO_OBJ(tobj);

    /* Cache it on the worker so (mp:current-thread) returns the same CL_Obj.
     * Required for bordeaux-threads-2 .known-threads. eql lookups. */
    child->thread_obj = thread_obj;

    /* Create OS thread.  Pass an explicit C stack size (CL_WORKER_C_STACK_SIZE)
     * rather than 0/OS-default: the default is far smaller than the main
     * thread's stack (512KB vs 8MB on macOS, 64KB on AmigaOS), which would make
     * a worker crash/corrupt at a call depth main handles.  See the define.
     * :STACK-SIZE can only raise the platform value, never lower it; the
     * platform layer additionally floors nonzero requests at its own default
     * (65536 on AmigaOS, where CL_WORKER_C_STACK_SIZE is 0 = OS default). */
    {
        uint32_t stack_floor = CL_WORKER_C_STACK_SIZE;
        if (c_stack < stack_floor)
            c_stack = stack_floor;
    }
    if (platform_thread_create(&child->platform_handle,
                               thread_entry, child,
                               c_stack) != 0) {
        cl_thread_unregister(child);
        cl_thread_table_free(thread_id);
        cl_thread_free_worker(child);
        cl_error(CL_ERR_GENERAL, "MP:MAKE-THREAD: failed to create OS thread");
    }

    return thread_obj;
}

/* (mp:join-thread thread) -> result */
static CL_Obj bi_join_thread(CL_Obj *args, int n)
{
    CL_ThreadObj *tobj;
    CL_Thread *t;
    CL_Obj result;
    void *handle;
    uint32_t id;
    CL_UNUSED(n);

    if (!CL_THREAD_P(args[0]))
        cl_error(CL_ERR_TYPE, "MP:JOIN-THREAD: argument must be a thread");

    tobj = (CL_ThreadObj *)CL_OBJ_TO_PTR(args[0]);
    id = tobj->thread_id;
    if (id >= CL_MAX_THREADS)
        cl_error(CL_ERR_GENERAL, "MP:JOIN-THREAD: invalid thread id");

    /* Claim the join under the table lock.  Without mutual exclusion two
     * joiners both called platform_thread_join on the same handle (double
     * pthread_join — UB and a double free of the handle struct) and both
     * freed the worker; the zombie reaper could also free a finished
     * worker while a joiner was still parked inside the join. */
    platform_mutex_lock(cl_thread_list_lock);
    t = cl_thread_table[id];
    /* Slot-identity check: table slots are reused after a join or a
     * zombie-reap.  Without the generation compare, a wrapper for a
     * long-exited thread would join (and then FREE) whatever unrelated
     * worker currently occupies its old slot — a double free of that
     * worker when its own joiner finishes.  tobj was derived before the
     * lock; no allocation happened since, so it is not stale (a peer STW
     * compaction cannot complete while this thread runs unparked). */
    if (!t || cl_thread_table_gen[id] != tobj->table_gen) {
        platform_mutex_unlock(cl_thread_list_lock);
        cl_error(CL_ERR_GENERAL, "MP:JOIN-THREAD: thread has already exited");
    }
    if (!t->platform_handle && !t->join_in_progress) {
        platform_mutex_unlock(cl_thread_list_lock);
        cl_error(CL_ERR_GENERAL, "MP:JOIN-THREAD: cannot join main thread");
    }
    if (t->join_in_progress) {
        /* Another joiner owns the OS-level join and the cleanup.  Wait
         * for the owner to STAMP the wrapper (thread_id = -1, its final
         * step), then read the result from the wrapper like the owner
         * does.  Spinning on `cl_thread_table[id] == t` was ABA-prone:
         * the freed CL_Thread could be reallocated at the same address
         * for a NEW worker that reuses the slot, making the compare
         * "equal" again and parking this waiter forever.  The wrapper
         * read must happen OUTSIDE the safe region (args[0] is a rooted,
         * forwarded VM slot, but a heap read from inside a safe region
         * races the compaction itself); the enter/leave pair per round
         * parks us whenever a GC is running. */
        platform_mutex_unlock(cl_thread_list_lock);
        for (;;) {
            tobj = (CL_ThreadObj *)CL_OBJ_TO_PTR(args[0]);
            if (tobj->thread_id == (uint32_t)-1)
                break;
            cl_gc_enter_safe_region();
            platform_thread_yield();
            cl_gc_leave_safe_region();
        }
        /* The owner's wrapper stamp is a plain store, not paired with a
         * lock this waiter takes, so the read above is not guaranteed to
         * observe the owner's prior write of tobj->result on weakly-
         * ordered hardware.  Pair it with an explicit barrier, matching
         * the rigor applied to interrupt_pending/destroy_requested. */
        platform_memory_barrier();
        tobj = (CL_ThreadObj *)CL_OBJ_TO_PTR(args[0]);  /* re-derive (moved?) */
        return tobj->result;
    }
    t->join_in_progress = 1;
    handle = t->platform_handle;
    t->platform_handle = NULL;   /* claimed: reaper must not detach it */
    platform_mutex_unlock(cl_thread_list_lock);

    /* Wait for thread to finish.  pthread_join blocks the caller outside
     * the VM dispatch loop — bracket it with the safe-region marker so a
     * concurrent stop-the-world GC counts us as already stopped. */
    cl_gc_enter_safe_region();
    platform_thread_join(handle, NULL);
    cl_gc_leave_safe_region();

    /* Re-derive tobj: cl_gc_leave_safe_region() can PARK this thread while a
     * peer thread runs a stop-the-world compaction, which relocates the wrapper
     * object.  The `tobj` C-pointer computed before the safe region is then
     * stale — it points at the wrapper's pre-move address.  Using it below would
     * read a garbage thread_id (so cl_thread_table_free clears the wrong slot,
     * leaving cl_thread_table[id] pointing at the worker we free here) and write
     * `-1` to the stale address (so the real wrapper keeps thread_id=id).  The
     * result: gc_finalize_dead later frees this same worker a SECOND time —
     * the multi-thread "pointer being freed was not allocated" crash.  args[0]
     * is a VM-stack root, forwarded by the compaction, so re-read through it. */
    tobj = (CL_ThreadObj *)CL_OBJ_TO_PTR(args[0]);

    /* Read the result from the GC-managed wrapper, not the worker's t->result:
     * the worker published it there before unregistering (see thread_entry step
     * 6a), and the wrapper's slot is marked+forwarded across any compaction that
     * ran while we were parked in the safe region above.  t->result is no longer
     * a GC root once the worker unregistered, so it may be stale here. */
    result = tobj->result;

    /* Clean up worker thread resources, then invalidate this wrapper's
     * slot index.  Without the invalidation, if MAKE-THREAD reuses the
     * slot for a new worker, this wrapper still points at thread_id=N
     * which now refers to a different CL_Thread; when the wrapper later
     * dies, gc_finalize_dead would free the unrelated worker.  Setting
     * thread_id out of range makes finalize skip this wrapper. */
    cl_thread_table_free((int)tobj->thread_id);
    cl_thread_free_worker(t);
    tobj->thread_id = (uint32_t)-1;
    /* platform_handle was free()d inside platform_thread_join; clear so
     * any subsequent code that might pick up `t` doesn't dereference. */
    /* (t itself is gone now — nothing more to do) */

    return result;
}

/* (mp:thread-alive-p thread) -> bool */
static CL_Obj bi_thread_alive_p(CL_Obj *args, int n)
{
    CL_ThreadObj *tobj;
    CL_Thread *t;
    CL_UNUSED(n);

    if (!CL_THREAD_P(args[0]))
        cl_error(CL_ERR_TYPE, "MP:THREAD-ALIVE-P: argument must be a thread");

    tobj = (CL_ThreadObj *)CL_OBJ_TO_PTR(args[0]);
    if (tobj->thread_id >= CL_MAX_THREADS)
        return CL_NIL;

    /* Read table slot and status under the lock: the zombie reaper and
     * a completing JOIN free workers under the same lock, so an
     * unlocked read here could dereference a just-freed CL_Thread. */
    {
        int alive;
        platform_mutex_lock(cl_thread_list_lock);
        t = cl_thread_table[tobj->thread_id];
        /* status: 0=created, 1=running, 2=finished, 3=aborted.
         * Generation mismatch = slot reused by an unrelated worker; this
         * wrapper's thread exited long ago (see bi_join_thread). */
        alive = (t && t->status <= 1 &&
                 cl_thread_table_gen[tobj->thread_id] == tobj->table_gen);
        platform_mutex_unlock(cl_thread_list_lock);
        return alive ? CL_T : CL_NIL;
    }
}

/* (mp:current-thread) -> thread */
static CL_Obj bi_current_thread(CL_Obj *args, int n)
{
    CL_Thread *self = (CL_Thread *)platform_tls_get();
    CL_UNUSED(args);
    CL_UNUSED(n);

    /* Main thread returns the pre-allocated object */
    if (self->id == 0)
        return main_thread_obj;

    /* Worker threads return the cached thread object set at make-thread time.
     * Identity matters: bordeaux-threads-2 keys .known-threads. by the value
     * returned here, then looks up via (mp:current-thread) inside the new
     * thread; the two must be eql. */
    if (!CL_NULL_P(self->thread_obj))
        return self->thread_obj;

    /* Fallback (should not normally happen): allocate a fresh wrapper and
     * cache it so subsequent calls return the same object. */
    {
        CL_ThreadObj *tobj = (CL_ThreadObj *)cl_alloc(TYPE_THREAD,
                                                       sizeof(CL_ThreadObj));
        if (!tobj) return CL_NIL;
        tobj->thread_id = self->id;
        tobj->table_gen = cl_thread_table_gen[self->id];
        tobj->name = self->name;
        self->thread_obj = CL_PTR_TO_OBJ(tobj);
        return self->thread_obj;
    }
}

/* (mp:all-threads) -> list */
static CL_Obj bi_all_threads(CL_Obj *args, int n)
{
    CL_Obj result = CL_NIL;
    CL_Obj snap;
    int i, count = 0;
    CL_UNUSED(args);
    CL_UNUSED(n);

    /* Two phases.  The table walk must hold cl_thread_list_lock (the
     * zombie reaper / a completing JOIN free workers under it — an
     * unlocked t->status read is a use-after-free), but we must NOT
     * allocate while holding it: an allocation can trigger stop-the-
     * world GC while an exiting peer blocks on this lock and can never
     * park.  So: allocate a snapshot vector first, fill it under the
     * lock (plain stores), and cons the result after releasing.  The
     * vector roots the wrappers across the consing. */
    snap = cl_make_vector(CL_MAX_THREADS);
    CL_GC_PROTECT(snap);
    {
        CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(snap);
        platform_mutex_lock(cl_thread_list_lock);
        for (i = 0; i < CL_MAX_THREADS; i++) {
            CL_Thread *t = cl_thread_table[i];
            if (!t) continue;
            if (i == 0) {
                v->data[count++] = main_thread_obj;
            } else {
                /* Skip finished (status=2) / aborted (status=3) workers.
                 * The slot+wrapper survive until the wrapper is GC'd so
                 * accessors like THREAD-NAME still work on a held handle,
                 * but ALL-THREADS must report only live threads
                 * (bordeaux-threads / SBCL / CCL contract). */
                if (t->status >= 2) continue;
                /* Reuse the canonical wrapper that was created at
                 * make-thread time (stashed in CL_Thread->thread_obj).
                 * Allocating a fresh wrapper would create an alias with
                 * the same thread_id; when one wrapper later dies,
                 * gc_finalize_dead would race over which one "owns" the
                 * slot. */
                if (CL_NULL_P(t->thread_obj)) continue;
                v->data[count++] = t->thread_obj;
            }
        }
        platform_mutex_unlock(cl_thread_list_lock);
    }

    CL_GC_PROTECT(result);
    for (i = count - 1; i >= 0; i--) {
        /* Re-derive the vector pointer each iteration: cl_cons can
         * compact and move the snapshot vector. */
        CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(snap);
        result = cl_cons(v->data[i], result);
    }
    CL_GC_UNPROTECT(2);
    return result;
}

/* Print a CL string name to stderr (NIL -> "(unnamed)"), char by char so we
 * need no transient C buffer allocation. */
static void dump_print_name(CL_Obj name)
{
    if (CL_NULL_P(name) || !CL_STRING_P(name)) {
        fprintf(stderr, "(unnamed)");
        return;
    }
    {
        uint32_t len = cl_string_length(name), i;
        for (i = 0; i < len; i++) {
            int c = cl_string_char_at(name, i);
            fputc((c >= 32 && c < 127) ? c : '?', stderr);
        }
    }
}

static void dump_print_wait_obj_name(CL_Obj o);   /* lock section below */

/* Human-readable form of CL_Thread.wait_kind (see thread.h).  Shared by
 * MP:DUMP-THREAD-WAITS and the slow-lock-wait diagnostic. */
static const char *wait_kind_name(int wk)
{
    switch (wk) {
    case 0: return "RUN";
    case 1: return "CONDWAIT";
    case 2: return "CONDWAIT/timeout";
    case 3: return "LOCK-ACQUIRE(block)";
    case 4: return "GC-STW-WAIT";
    default: return "?";
    }
}

/* Diagnostic: print, for every live thread, what synchronization primitive it
 * is currently blocked on.  Distinguishes a lost-wakeup (a worker still parked
 * in condwait on its queue condvar after the producer already notified) from a
 * lock-ordering deadlock (a thread blocked acquiring a held lock).  Callable
 * from C (the GC-STW straggler diagnostic in thread.c) as well as from Lisp
 * via MP:DUMP-THREAD-WAITS. */
void cl_dump_thread_waits(void)
{
    int i;
    fprintf(stderr, "==== MP:DUMP-THREAD-WAITS (%u live threads) ====\n",
            cl_thread_count);
    /* Walk the table under the list lock: the zombie reaper / a completing
     * JOIN / gc_finalize_dead free workers concurrently, and this dump is
     * a hang-triage entry point — called exactly when thread churn is
     * pathological.  fprintf under the lock is acceptable here (diagnostic
     * path, stderr). */
    if (cl_thread_list_lock) platform_mutex_lock(cl_thread_list_lock);
    for (i = 0; i < CL_MAX_THREADS; i++) {
        CL_Thread *t = cl_thread_table[i];
        const char *st, *wk;
        if (!t) continue;
        switch (t->status) {
        case 0: st = "created";  break;
        case 1: st = "running";  break;
        case 2: st = "finished"; break;
        case 3: st = "aborted";  break;
        default: st = "?";       break;
        }
        wk = wait_kind_name(t->wait_kind);
        fprintf(stderr, "  tid=%-3u status=%-8s name=\"", t->id, st);
        dump_print_name(t->name);
        fprintf(stderr, "\" %s", wk);
        if (t->wait_kind == 1 || t->wait_kind == 2) {
            fprintf(stderr, " cv=\"");
            dump_print_wait_obj_name(t->wait_obj);
            fprintf(stderr, "\" lock=\"");
            dump_print_wait_obj_name(t->wait_lock);
            fputc('"', stderr);
        } else if (t->wait_kind == 3) {
            fprintf(stderr, " lock=\"");
            dump_print_wait_obj_name(t->wait_obj);
            fputc('"', stderr);
        } else if (t->wait_kind == 4) {
            fprintf(stderr, " waiting-for-tid=%d", t->wait_straggler_tid);
        }
        /* GC coordination flags — a thread with gc_req=1 but stopped=0 and
         * safe=0 is the straggler holding up a stop-the-world GC. */
        fprintf(stderr, " [gc_req=%d stopped=%d safe=%d]",
                (int)t->gc_requested, (int)t->gc_stopped, (int)t->in_safe_region);
        fprintf(stderr, "\n");
    }
    if (cl_thread_list_lock) platform_mutex_unlock(cl_thread_list_lock);
    fprintf(stderr, "==== end ====\n");
    fflush(stderr);
}

/* (mp:dump-thread-waits) -> nil */
static CL_Obj bi_dump_thread_waits(CL_Obj *args, int n)
{
    CL_UNUSED(args);
    CL_UNUSED(n);
    cl_dump_thread_waits();
    return CL_NIL;
}

/* (mp:thread-name thread) -> string/nil */
static CL_Obj bi_thread_name(CL_Obj *args, int n)
{
    CL_ThreadObj *tobj;
    CL_UNUSED(n);

    if (!CL_THREAD_P(args[0]))
        cl_error(CL_ERR_TYPE, "MP:THREAD-NAME: argument must be a thread");

    tobj = (CL_ThreadObj *)CL_OBJ_TO_PTR(args[0]);
    return tobj->name;
}

/* (mp:thread-yield) -> nil */
static CL_Obj bi_thread_yield(CL_Obj *args, int n)
{
    CL_UNUSED(args);
    CL_UNUSED(n);
    platform_thread_yield();
    return CL_NIL;
}

/* ================================================================
 * Lock builtins
 *
 * A lock is a heap word (CL_Lock in types.h): `state` is 0 when free, or
 * the owner's thread serial shifted up one bit, with bit 0
 * (CL_LOCK_CONTENDED) set once some thread has had to park on it.  The
 * runtime allocates no OS mutex per lock.  Blocking goes through each
 * thread's own park handle (platform_park, token semantics): a waiter
 * marks the lock CONTENDED, registers itself (wait_kind / wait_obj, under
 * cl_thread_list_lock), re-checks the lock, and parks; a releaser that
 * sees CONTENDED stores 0 and scans the thread list for ONE thread
 * registered on this lock and unparks it.  The uncontended paths are a
 * single CAS each and never take a lock or scan anything.  The shape is
 * Drepper's "mutex 2" ("Futexes Are Tricky") with a registration list in
 * place of the futex — see specs/mp-locks-heap-words.md for the
 * invariants, and the "Lost wakeups" risk there for why every abandon
 * path (timeout, interrupt) hands the lock on before leaving.
 *
 * GC discipline: a CL_Lock* / CL_CondVar* is re-derived from its GC-rooted
 * args[] slot after anything that can run a compaction — a park (safe
 * region), cl_thread_handle_interrupt (runs Lisp), any allocation.  The
 * objects move; the rooted slot is forwarded.  The registration fields
 * (wait_obj / wait_lock) are thread roots for the same reason.
 * ================================================================ */

#ifdef DEBUG_THREAD_RACE_HOOKS
/* Deterministic race window for tests/test_mt_lock_barge_race.sh: the
 * number of milliseconds a contended acquirer sleeps between registering
 * itself as a waiter and marking the current holder's word CONTENDED
 * (CLAMIGA_RACE_LOCK_MARK_DELAY_MS; 0 / unset = no delay). */
static int dbg_race_lock_mark_delay_ms(void)
{
    static int cached = -1;
    if (cached < 0) {
        char envbuf[16];
        const char *s = platform_getenv("CLAMIGA_RACE_LOCK_MARK_DELAY_MS",
                                        envbuf, (int)sizeof(envbuf));
        cached = (s && *s) ? atoi(s) : 0;
        if (cached < 0) cached = 0;
    }
    return cached;
}
#endif

/* The owner identity stored in CL_Lock.state for thread T. */
#define LOCK_OWNER_WORD(t)  ((uint32_t)(t)->serial << 1)
#define LOCK_OWNER_OF(s)    ((s) & ~(uint32_t)CL_LOCK_CONTENDED)

/* Host: a short yield-spin before parking, for the typical MP critical
 * section that is held for less time than a park/unpark round trip.
 * The spin READS the state word and only attempts a CAS when it reads
 * free, so spinners do not bounce the lock's cache line.  AmigaOS /
 * MorphOS: none — a Forbid()/Permit() yield only reschedules when a task
 * switch is already pending, so spinning cannot hand the single CPU to
 * the holder; park at once. */
#ifdef PLATFORM_AMIGA
#define CL_LOCK_SPIN_YIELDS 0
#else
#define CL_LOCK_SPIN_YIELDS 256
#endif

/* (mp:make-lock &optional name) -> lock */
/* Allocate a fresh, free CL_Lock.  recursive != 0 makes it re-entrant
 * for its owner.  Used by MP:MAKE-LOCK, MP:MAKE-RECURSIVE-LOCK, and the
 * FASL reader (so a lock embedded in a struct/closure constant comes back
 * as a usable, fresh-at-load-time lock instead of NIL).  Errors out via
 * cl_error() — err_prefix customizes the message so callers see e.g.
 * "MP:MAKE-LOCK: ..." vs. "FASL: ...". */
CL_Obj cl_lock_alloc_obj(int recursive, CL_Obj name, const char *err_prefix)
{
    CL_Lock *lk;

    /* `name` is a plain CL_Obj C local (the caller's copy): the allocation
     * below is a moving collection point under the generational collector. */
    CL_GC_PROTECT(name);
    lk = (CL_Lock *)cl_alloc(TYPE_LOCK, sizeof(CL_Lock));
    CL_GC_UNPROTECT(1);
    if (!lk)
        cl_error(CL_ERR_STORAGE, "%s: cannot allocate lock object",
                 err_prefix);

    lk->state = 0;
    lk->depth = 0;
    lk->flags = recursive ? CL_LOCK_FLAG_RECURSIVE : 0;
    lk->name = name;
    return CL_PTR_TO_OBJ(lk);
}

static CL_Obj bi_make_lock(CL_Obj *args, int n)
{
    CL_Obj name = (n > 0) ? args[0] : CL_NIL;
    return cl_lock_alloc_obj(0, name, "MP:MAKE-LOCK");
}

/* (mp:make-recursive-lock &optional name) -> lock
 * A recursive lock can be acquired multiple times by the same thread; it must
 * be released the same number of times before another thread can acquire it. */
static CL_Obj bi_make_recursive_lock(CL_Obj *args, int n)
{
    CL_Obj name = (n > 0) ? args[0] : CL_NIL;
    return cl_lock_alloc_obj(1, name, "MP:MAKE-RECURSIVE-LOCK");
}

/* Seconds (a non-negative real, or NIL for "none") -> milliseconds.
 * Returns 1 when a timeout is in effect and stores it in *ms_out.
 * Clamped to INT32_MAX ms (~24.85 days): ms_until() below folds the
 * deadline through a signed 32-bit subtraction, so a deadline further out
 * than that wraps and reads as already expired on the very first check
 * instead of blocking for the requested (long but finite) duration. */
static int parse_timeout_ms(CL_Obj arg, const char *who, uint32_t *ms_out)
{
    double secs;
    if (CL_NULL_P(arg))
        return 0;
    secs = cl_to_double(arg);
    if (secs < 0.0)
        cl_error(CL_ERR_TYPE, "%s: timeout must be a non-negative real "
                 "number of seconds", who);
    if (secs * 1000.0 > (double)0x7FFFFFFFu)
        *ms_out = 0x7FFFFFFFu;
    else
        *ms_out = (uint32_t)(secs * 1000.0);
    return 1;
}

/* Milliseconds still to wait before `deadline_ms` (monotonic), 0 when it
 * has passed.  Signed compare so the ms clock may wrap. */
static uint32_t ms_until(uint32_t deadline_ms)
{
    int32_t d = (int32_t)(deadline_ms - platform_time_ms());
    return d > 0 ? (uint32_t)d : 0;
}

/* Clear this thread's wait registration (under cl_thread_list_lock so a
 * concurrent release scan / notify sees either the whole record or none). */
static void wait_deregister(CL_Thread *self)
{
    platform_mutex_lock(cl_thread_list_lock);
    self->wait_kind = 0;
    self->wait_obj = CL_NIL;
    self->wait_lock = CL_NIL;
    platform_mutex_unlock(cl_thread_list_lock);
}

/* Park this thread for up to `ms` milliseconds (0 = until unparked)
 * inside a GC safe region.  A thread without a park handle (init failed
 * at start-up) degrades to a short sleep; its callers re-check their
 * condition in a loop, so the only cost is latency. */
static void wait_park(CL_Thread *self, uint32_t ms)
{
    if (self->park) {
        cl_gc_enter_safe_region();
        platform_park(self->park, ms);
        cl_gc_leave_safe_region();
    } else {
        platform_sleep_ms((ms == 0 || ms > 10) ? 10 : ms);
    }
}

/* Wake one thread registered as a lock-acquire waiter on `lock_obj`
 * (other than `except`).  Called with the lock's state already 0 by a
 * contended release, and by a waiter that abandons a contended wait. */
static void lock_wake_one(CL_Obj lock_obj, CL_Thread *except)
{
    CL_Thread *t;
    platform_mutex_lock(cl_thread_list_lock);
    for (t = cl_thread_list; t; t = t->next) {
        if (t != except && t->wait_kind == 3 && t->wait_obj == lock_obj) {
            if (t->park) platform_unpark(t->park);
            break;
        }
    }
    platform_mutex_unlock(cl_thread_list_lock);
}

/* Release the lock in `lock_obj` that the caller owns at depth 0.  The
 * "mutex 2" release: an uncontended lock is freed by one CAS; a contended
 * one is stored free and ONE parked waiter is unparked.  The store is
 * fenced before the scan so it is ordered against a waiter's
 * register-then-re-check sequence (both under cl_thread_list_lock on the
 * waiter's side): either the waiter's registration is visible to the scan,
 * or the store of 0 is visible to its re-check. */
static void lock_release_core(CL_Obj lock_obj)
{
    CL_Lock *lk = (CL_Lock *)CL_OBJ_TO_PTR(lock_obj);
    uint32_t s = lk->state;
    if (!(s & CL_LOCK_CONTENDED) && platform_atomic_cas(&lk->state, s, 0))
        return;
    /* Contended: a plain store frees the lock, so it needs the RELEASE
     * fence the CAS above has built in.  Without it, on a weakly ordered
     * host (ARM64) the store of 0 can become visible before the critical
     * section's own stores, and the next acquirer — whose CAS is an
     * acquire — reads stale data: the barging soak in
     * tests/test_mp_heap_locks.sh caught exactly one lost `incf` in
     * 320,000 protected increments this way.  The fence after the store
     * orders it against the waiter scan (see above). */
    platform_memory_barrier();
    lk->state = 0;
    platform_memory_barrier();
    lock_wake_one(lock_obj, NULL);
}

/* A waiter that leaves a contended acquire WITHOUT the lock (timeout, or an
 * interrupt about to unwind it) must not strand the other waiters: the
 * release that woke it consumed the CONTENDED bit, so a lock that is free
 * now needs another waiter woken, and a lock that a barging thread took
 * without the bit needs it set again so that thread's release scans. */
static void lock_abandon_wait(CL_Obj lock_obj, CL_Thread *self)
{
    for (;;) {
        CL_Lock *lk = (CL_Lock *)CL_OBJ_TO_PTR(lock_obj);
        uint32_t s = lk->state;
        if (s == 0) {
            lock_wake_one(lock_obj, self);
            return;
        }
        if (s & CL_LOCK_CONTENDED)
            return;
        if (platform_atomic_cas(&lk->state, s, s | CL_LOCK_CONTENDED))
            return;
    }
}

/* ---- Slow-lock-wait diagnostic (CLAMIGA_LOCK_DIAG) ----
 *
 * When the CLAMIGA_LOCK_DIAG environment variable is set, a blocking
 * MP:ACQUIRE-LOCK that has waited at least the threshold reports the
 * contended lock (name), the waiting thread, and the current HOLDER —
 * including what the holder itself is blocked on — to stderr, repeats
 * while still parked, and reports once more when the lock is finally
 * acquired (with the total wait).  The env value is the threshold in
 * milliseconds; "1" or a non-numeric value selects the 1000ms default.
 *
 * Unlike MP:DUMP-THREAD-WAITS (which must be called at exactly the right
 * moment from a watchdog), this triages an intermittent stall from
 * amilisp's own output: the report fires from inside the stalled wait and
 * names the culprit.  Runtime diagnostic, not DEBUG-flag instrumentation:
 * always compiled, zero cost when the env var is unset (one cached-static
 * read per contended park, a path that is already slow — parks are
 * untimed unless the variable is set). */

#define CL_LOCK_DIAG_REPEAT_MS 5000  /* re-report cadence while still parked */

static int32_t lock_diag_threshold_ms(void)
{
    /* -2 = env not read yet; -1 = disabled; >=2 = threshold in ms.  The
     * unsynchronized lazy init is a benign race: every thread computes the
     * same value from the same environment. */
    static int32_t cached = -2;
    if (cached == -2) {
        char envbuf[32];
        const char *s = platform_getenv("CLAMIGA_LOCK_DIAG", envbuf,
                                        (int)sizeof(envbuf));
        if (!s || !*s)
            cached = -1;
        else {
            int v = atoi(s);
            cached = (v >= 2) ? v : 1000;
        }
    }
    return cached;
}

/* The name of the lock / condvar a thread's wait record points at. */
static void dump_print_wait_obj_name(CL_Obj o)
{
    if (CL_LOCK_P(o))
        dump_print_name(((CL_Lock *)CL_OBJ_TO_PTR(o))->name);
    else if (CL_CONDVAR_P(o))
        dump_print_name(((CL_CondVar *)CL_OBJ_TO_PTR(o))->name);
    else
        fprintf(stderr, "(none)");
}

/* Print one slow-wait observation for the lock in `lock_obj`.
 *
 * MUST be called OUTSIDE the GC safe region: it reads heap objects (the
 * lock's and threads' name strings), which a peer's compacting GC could be
 * relocating while the caller is parked.  The caller passes the lock as the
 * re-read GC-rooted args[] value — any CL_Lock* captured before the park
 * is potentially stale after a compaction.
 *
 * `acquired` != 0 prints the final "acquired after N ms" form instead of
 * the still-waiting form. */
static void lock_wait_report(CL_Obj lock_obj, uint32_t waited_ms, int acquired)
{
    CL_Thread *self = cl_get_current_thread();
    CL_Lock *lk = (CL_Lock *)CL_OBJ_TO_PTR(lock_obj);
    uint32_t owner_serial;

    fprintf(stderr, "MP:ACQUIRE-LOCK diag: tid=%u \"", self->id);
    dump_print_name(self->name);
    if (acquired) {
        fprintf(stderr, "\" acquired lock \"");
        dump_print_name(lk->name);
        fprintf(stderr, "\" after %u ms\n", waited_ms);
        fflush(stderr);
        return;
    }
    fprintf(stderr, "\" waiting %u ms for lock \"", waited_ms);
    dump_print_name(lk->name);
    fputc('"', stderr);

    /* Identify the holder by the serial in the state word.  The read is
     * racy by design (diagnostic); the holder is looked up in the thread
     * list under cl_thread_list_lock so the zombie reaper / a completing
     * JOIN cannot free it mid-print.  The list lock is only ever held for
     * short bounded sections (the STW request/scan loops release it before
     * parking on gc_condvar), so blocking on it here cannot deadlock. */
    owner_serial = LOCK_OWNER_OF(lk->state) >> 1;
    platform_mutex_lock(cl_thread_list_lock);
    if (owner_serial == 0) {
        fprintf(stderr, " with no holder (released this instant?)");
    } else {
        CL_Thread *holder;
        for (holder = cl_thread_list; holder; holder = holder->next)
            if (holder->serial == owner_serial) break;
        if (holder) {
            fprintf(stderr, " held by tid=%u \"", holder->id);
            dump_print_name(holder->name);
            fprintf(stderr, "\" depth=%u holder-state=%s",
                    lk->depth, wait_kind_name(holder->wait_kind));
            if (holder->wait_kind == 1 || holder->wait_kind == 2) {
                fprintf(stderr, " cv=\"");
                dump_print_wait_obj_name(holder->wait_obj);
                fprintf(stderr, "\" lock=\"");
                dump_print_wait_obj_name(holder->wait_lock);
                fputc('"', stderr);
            } else if (holder->wait_kind == 3) {
                fprintf(stderr, " lock=\"");
                dump_print_wait_obj_name(holder->wait_obj);
                fputc('"', stderr);
            }
            if (holder->in_safe_region)
                fprintf(stderr, " [in-safe-region: blocking syscall]");
        } else {
            fprintf(stderr, " held by an already-exited thread"
                            " (lock leaked by its holder?)");
        }
    }
    platform_mutex_unlock(cl_thread_list_lock);
    fputc('\n', stderr);
    fflush(stderr);
}

/* Blocking acquire of the lock in the GC-rooted slot *lock_slot, for a
 * caller whose fast-path CAS already failed.  Returns 1 once the lock is
 * held (state = this thread's serial, CONTENDED kept set — the "mutex 2"
 * rule that makes the next release scan for the waiters we could not see),
 * 0 when `timed` and `ms` elapsed first.  The caller sets depth.
 *
 * deliver_interrupts: consume a pending MP:INTERRUPT-THREAD / DESTROY-THREAD
 * while waiting (may longjmp out).  0 defers it to the caller — the
 * condition-wait re-acquire, which must run the interrupt with the lock
 * held so a destroy unwinds through the caller's UNWIND-PROTECT. */
static int lock_acquire_slow(CL_Obj *lock_slot, CL_Thread *self,
                             int timed, uint32_t ms, int deliver_interrupts)
{
    uint32_t me = LOCK_OWNER_WORD(self);
    int32_t  diag_ms = lock_diag_threshold_ms();
    uint32_t start = 0, deadline = 0, next_report = 0;
    int reported = 0;
    int spins = 0;

    if (timed || diag_ms >= 0) {
        start = platform_time_ms();
        deadline = start + ms;
        next_report = start + (uint32_t)(diag_ms >= 0 ? diag_ms : 0);
    }

    for (;;) {
        CL_Lock *lk = (CL_Lock *)CL_OBJ_TO_PTR(*lock_slot);
        uint32_t s = lk->state;
        uint32_t wait_ms = 0;

        if (s == 0) {
            /* Free.  Past the spin phase we take it CONTENDED (see above);
             * in the spin phase we are not a registered waiter and take
             * it plain, like the fast path. */
            uint32_t want = (spins < CL_LOCK_SPIN_YIELDS)
                            ? me : (me | CL_LOCK_CONTENDED);
            if (platform_atomic_cas(&lk->state, 0, want))
                break;
            continue;
        }

        if (spins < CL_LOCK_SPIN_YIELDS) {
            spins++;
            if (deliver_interrupts && self->interrupt_pending)
                cl_thread_handle_interrupt(self);   /* may longjmp */
            platform_thread_yield();
            /* A spinner is not parked in a safe region, so it must reach
             * a safepoint itself or a peer's stop-the-world waits out the
             * whole spin phase; the loop top re-derives the (possibly
             * moved) lock. */
            if (self->gc_requested)
                cl_gc_safepoint();
            continue;
        }

        /* Register FIRST, then make sure the CURRENT holder's word carries
         * CONTENDED, and only then park.  Both orders matter:
         *
         *  - registering before the mark means the holder whose word we
         *    mark cannot release without scanning, and its scan (after
         *    its store of 0) runs after our registration, so it finds us;
         *  - marking AFTER registering means the bit is on the word of
         *    whoever holds the lock NOW.  Marking it before (as the first
         *    version did, then merely re-checking "state == 0" after
         *    registering) lost a wakeup in three steps: the marked holder
         *    released and scanned before we were listed, a spinning
         *    newcomer took the free lock with a plain word, our re-check
         *    saw "held" and parked — and the newcomer's release, seeing no
         *    bit, never scanned.  This is the futex_wait(&val, 2) re-check
         *    of Drepper's mutex 2, done by hand.
         *
         * If the lock is free by the time we look, we do not park at all. */
        platform_mutex_lock(cl_thread_list_lock);
        self->wait_obj = *lock_slot;
        self->wait_lock = CL_NIL;
        self->wait_kind = 3;
        platform_mutex_unlock(cl_thread_list_lock);
        platform_memory_barrier();
#ifdef DEBUG_THREAD_RACE_HOOKS
        /* Widen the registered-but-not-yet-marked window on demand so
         * tests/test_mt_lock_barge_race.sh can force a release plus a
         * barging re-acquire to land inside it (CLAMIGA_RACE_LOCK_MARK_DELAY_MS). */
        if (dbg_race_lock_mark_delay_ms() > 0)
            platform_sleep_ms((uint32_t)dbg_race_lock_mark_delay_ms());
#endif
        for (;;) {
            lk = (CL_Lock *)CL_OBJ_TO_PTR(*lock_slot);
            s = lk->state;
            if (s == 0)
                break;
            if ((s & CL_LOCK_CONTENDED) ||
                platform_atomic_cas(&lk->state, s, s | CL_LOCK_CONTENDED))
                break;
        }
        if (s == 0) {
            wait_deregister(self);
            continue;
        }
        /* An interrupt published before our registration was not
         * delivered by an unpark (the publisher only unparks a registered
         * waiter); consume it here rather than park past it. */
        if (deliver_interrupts && self->interrupt_pending) {
            wait_deregister(self);
            lock_abandon_wait(*lock_slot, self);
            cl_thread_handle_interrupt(self);   /* may longjmp */
            continue;
        }

        if (timed) {
            wait_ms = ms_until(deadline);
            if (wait_ms == 0) {
                wait_deregister(self);
                lock_abandon_wait(*lock_slot, self);
                return 0;
            }
        }
        if (diag_ms >= 0) {
            uint32_t d = ms_until(next_report);
            if (d == 0) d = 1;
            if (wait_ms == 0 || d < wait_ms) wait_ms = d;
        }

        wait_park(self, wait_ms);
        wait_deregister(self);

        if (diag_ms >= 0 && ms_until(next_report) == 0) {
            /* Report OUTSIDE the safe region (we are): the heap is stable
             * and *lock_slot holds the forwarded lock. */
            lock_wait_report(*lock_slot, platform_time_ms() - start, 0);
            next_report = platform_time_ms() + CL_LOCK_DIAG_REPEAT_MS;
            reported = 1;
        }
        if (deliver_interrupts && self->interrupt_pending) {
            lock_abandon_wait(*lock_slot, self);
            cl_thread_handle_interrupt(self);   /* may longjmp */
        }
        /* Loop: re-read the state and try again (a barging newcomer may
         * have taken the lock in between — we re-park, as bordeaux-threads
         * promises nothing about fairness). */
    }

    if (reported) {
        uint32_t total = platform_time_ms() - start;
        lock_wait_report(*lock_slot, total, 1);
    }
    return 1;
}

/* (mp:acquire-lock lock &optional (wait-p t) timeout) -> bool
 * With wait-p NIL the lock is tried once.  timeout (seconds, a non-negative
 * real) bounds a blocking acquire; NIL waits forever, 0 tries once.
 * Returns T when the lock is held on return, NIL otherwise. */
static CL_Obj bi_acquire_lock(CL_Obj *args, int n)
{
    CL_Lock *lk;
    CL_Thread *self;
    uint32_t me, s;
    int wait_p = 1;
    int timed = 0;
    uint32_t ms = 0;

    if (!CL_LOCK_P(args[0]))
        cl_error(CL_ERR_TYPE, "MP:ACQUIRE-LOCK: argument must be a lock");
    if (n > 1 && CL_NULL_P(args[1]))
        wait_p = 0;
    if (n > 2)
        timed = parse_timeout_ms(args[2], "MP:ACQUIRE-LOCK", &ms);

    self = cl_get_current_thread();
    me = LOCK_OWNER_WORD(self);
    lk = (CL_Lock *)CL_OBJ_TO_PTR(args[0]);

    /* Fast path: free -> ours, one CAS. */
    if (platform_atomic_cas(&lk->state, 0, me)) {
        lk->depth = 1;
        return CL_T;
    }
    s = lk->state;
    if (LOCK_OWNER_OF(s) == me) {
        if (lk->flags & CL_LOCK_FLAG_RECURSIVE) {
            lk->depth++;
            return CL_T;
        }
        cl_error(CL_ERR_GENERAL,
                 "MP:ACQUIRE-LOCK: the calling thread already holds this "
                 "lock (a plain lock is not re-entrant; use "
                 "MP:MAKE-RECURSIVE-LOCK for nested acquires)");
    }
    if (!wait_p)
        return CL_NIL;
    if (timed && ms == 0)
        return CL_NIL;    /* one attempt was made above */

    if (!lock_acquire_slow(&args[0], self, timed, ms, 1))
        return CL_NIL;
    lk = (CL_Lock *)CL_OBJ_TO_PTR(args[0]);   /* re-derive: we parked */
    lk->depth = 1;
    return CL_T;
}

/* (mp:release-lock lock) -> nil */
static CL_Obj bi_release_lock(CL_Obj *args, int n)
{
    CL_Lock *lk;
    CL_Thread *self;
    CL_UNUSED(n);

    if (!CL_LOCK_P(args[0]))
        cl_error(CL_ERR_TYPE, "MP:RELEASE-LOCK: argument must be a lock");

    self = cl_get_current_thread();
    lk = (CL_Lock *)CL_OBJ_TO_PTR(args[0]);
    if (LOCK_OWNER_OF(lk->state) != LOCK_OWNER_WORD(self)) {
        if (lk->state == 0)
            cl_error(CL_ERR_GENERAL,
                     "MP:RELEASE-LOCK: the lock is not held");
        cl_error(CL_ERR_GENERAL,
                 "MP:RELEASE-LOCK: the lock is held by another thread "
                 "(only the owner may release it)");
    }
    if (lk->depth > 1) {
        lk->depth--;
        return CL_NIL;
    }
    lk->depth = 0;
    lock_release_core(args[0]);
    return CL_NIL;
}

/* ================================================================
 * Condition variable builtins
 *
 * A condition variable is a heap word too (CL_CondVar): a `waiters`
 * count and a name.  A waiter registers (wait_kind 1/2, wait_obj = cv,
 * wait_lock = lock) and parks on its own handle; notify scans the thread
 * list for one (or every) thread registered on this cv, clears its
 * wait_kind and unparks it.  "Notified" versus "timed out" is decided by
 * the wait_kind field under cl_thread_list_lock — the same lock the
 * notifier writes it under — so a notify landing at the timeout instant
 * counts as a notify, never both or neither.
 * ================================================================ */

/* (mp:make-condition-variable &optional name) -> cv */
static CL_Obj bi_make_condition_variable(CL_Obj *args, int n)
{
    CL_Obj name = (n > 0) ? args[0] : CL_NIL;
    CL_CondVar *cv;

    CL_GC_PROTECT(name);
    cv = (CL_CondVar *)cl_alloc(TYPE_CONDVAR, sizeof(CL_CondVar));
    CL_GC_UNPROTECT(1);
    if (!cv)
        cl_error(CL_ERR_STORAGE,
                 "MP:MAKE-CONDITION-VARIABLE: cannot allocate condvar object");

    cv->waiters = 0;
    cv->name = name;
    return CL_PTR_TO_OBJ(cv);
}

/* (mp:condition-wait cv lock &optional timeout) -> bool
 * timeout is in seconds (real number).  Returns T when notified (or
 * woken spuriously by an interrupt), NIL when the timeout elapsed.
 * The lock must be held by the caller; it is released for the duration
 * of the wait and re-acquired at the same recursion depth before return. */
static CL_Obj bi_condition_wait(CL_Obj *args, int n)
{
    CL_CondVar *cv;
    CL_Lock *lk;
    CL_Thread *self;
    uint32_t me, saved_depth;
    int timed = 0;
    uint32_t ms = 0, deadline = 0;
    CL_Obj result = CL_T;

    if (!CL_CONDVAR_P(args[0]))
        cl_error(CL_ERR_TYPE,
                 "MP:CONDITION-WAIT: first argument must be a condition-variable");
    if (!CL_LOCK_P(args[1]))
        cl_error(CL_ERR_TYPE,
                 "MP:CONDITION-WAIT: second argument must be a lock");
    if (n > 2)
        timed = parse_timeout_ms(args[2], "MP:CONDITION-WAIT", &ms);

    self = cl_get_current_thread();
    me = LOCK_OWNER_WORD(self);
    lk = (CL_Lock *)CL_OBJ_TO_PTR(args[1]);
    if (LOCK_OWNER_OF(lk->state) != me)
        cl_error(CL_ERR_GENERAL,
                 "MP:CONDITION-WAIT: the calling thread does not hold the "
                 "lock (acquire it before waiting)");

    /* If we're about to park while holding an internal rwlock reader
     * (compiler tables), every thread that needs the matching writer lock
     * will block forever.  Treat as a hard bug: dump the holders and abort
     * so we can find the leaky path before we ship a deadlock.
     *
     * NOTE: this cannot yet also guard cl_package_rwlock — thread.h's
     * rdlock_package_held counter has no writer (package.c/symbol.c take
     * cl_package_rwlock directly via platform_rwlock_rdlock(), not through
     * a counted wrapper), so it is always 0.  Checking it here would be
     * dead code implying protection that doesn't exist.  Re-add it once a
     * cl_tables_rdlock_at()-style counted wrapper exists for the package
     * rwlock (and cl_tables_dump_rdlock_holders walks it too). */
    if (CL_MT() && self->rdlock_tables_held > 0) {
        cl_tables_dump_rdlock_holders(
            "[BUG] mp:condition-wait while holding an internal rwlock "
            "reader (tables):");
        cl_capture_backtrace();
        fprintf(stderr, "%s", self->backtrace_buf);
        fflush(stderr);
        abort();
    }

    saved_depth = lk->depth;
    if (timed)
        deadline = platform_time_ms() + ms;

    /* Register on the cv (under the list lock, the notifier's lock too). */
    cv = (CL_CondVar *)CL_OBJ_TO_PTR(args[0]);
    platform_mutex_lock(cl_thread_list_lock);
    self->wait_obj = args[0];
    self->wait_lock = args[1];
    self->wait_kind = timed ? 2 : 1;
    platform_atomic_inc(&cv->waiters);
    platform_mutex_unlock(cl_thread_list_lock);

    /* Pre-park interrupt check.  The publisher sets interrupt_pending
     * under cl_thread_list_lock and unparks a REGISTERED waiter, so an
     * interrupt published before our registration reaches us only here.
     * Consume it HERE, not at "the next safepoint": on the Amiga JIT path a
     * caller loop like (loop (mp:condition-wait cv lk)) contains NO
     * safepoints (JIT'd code polls nothing; builtin calls go through
     * cl_vm_apply without a safepoint), so returning T alone spun the loop
     * forever and a destroy was never delivered — the FS-UAE hang that
     * caught this.  A destroy longjmps out with the lock still held, so the
     * caller's UNWIND-PROTECT releases it. */
    platform_memory_barrier();
    if (self->interrupt_pending) {
        platform_mutex_lock(cl_thread_list_lock);
        if (self->wait_kind != 0) {   /* not already picked by a notifier */
            self->wait_kind = 0;
            cv = (CL_CondVar *)CL_OBJ_TO_PTR(args[0]);
            platform_atomic_dec(&cv->waiters);
        }
        self->wait_obj = CL_NIL;
        self->wait_lock = CL_NIL;
        platform_mutex_unlock(cl_thread_list_lock);
        cl_thread_handle_interrupt(self);  /* longjmps on destroy */
        return CL_T;   /* interrupt ran — spurious wakeup */
    }

    /* Release the lock fully (any recursion depth) and park. */
    lk = (CL_Lock *)CL_OBJ_TO_PTR(args[1]);
    lk->depth = 0;
    lock_release_core(args[1]);

    for (;;) {
        uint32_t wait_ms = 0;
        int done = 0;
        if (timed) {
            wait_ms = ms_until(deadline);
            if (wait_ms > 0)
                wait_park(self, wait_ms);
        } else {
            wait_park(self, 0);
        }
        /* Resolve the wake under the notifier's lock. */
        platform_mutex_lock(cl_thread_list_lock);
        if (self->wait_kind == 0) {
            /* A notifier picked us: it cleared wait_kind, decremented the
             * waiter count and deposited our token. */
            result = CL_T;
            done = 1;
        } else if (self->interrupt_pending ||
                   (timed && ms_until(deadline) == 0)) {
            self->wait_kind = 0;
            cv = (CL_CondVar *)CL_OBJ_TO_PTR(args[0]);
            platform_atomic_dec(&cv->waiters);
            result = self->interrupt_pending ? CL_T : CL_NIL;
            done = 1;
        }
        if (done) {
            self->wait_obj = CL_NIL;
            self->wait_lock = CL_NIL;
        }
        platform_mutex_unlock(cl_thread_list_lock);
        if (done) break;
        /* Spurious return (a stale token from an earlier wake): still
         * registered, deadline not reached — park again. */
    }

    /* Re-acquire the lock at the saved depth.  Interrupts are NOT consumed
     * inside this acquire: a destroy must unwind with the lock held (the
     * caller's UNWIND-PROTECT releases it), exactly as a safepoint delivery
     * would run with the lock re-acquired. */
    lk = (CL_Lock *)CL_OBJ_TO_PTR(args[1]);
    if (!platform_atomic_cas(&lk->state, 0, me))
        lock_acquire_slow(&args[1], self, 0, 0, 0);
    lk = (CL_Lock *)CL_OBJ_TO_PTR(args[1]);
    lk->depth = saved_depth;

    /* Post-wake delivery (same JIT-no-safepoint rationale as above): the
     * publisher's wakeup must lead to consumption HERE — the caller's
     * predicate loop may never reach a safepoint. */
    if (self->interrupt_pending)
        cl_thread_handle_interrupt(self);
    return result;
}

/* Wake one (all == 0) or every thread registered on the condvar in
 * `cv_obj`.  The `waiters` fast path keeps a notify with nobody waiting
 * free of the list lock — most of sento's notifies on the reply path. */
static void condvar_wake(CL_Obj cv_obj, int all)
{
    CL_CondVar *cv = (CL_CondVar *)CL_OBJ_TO_PTR(cv_obj);
    CL_Thread *t;

    if (cv->waiters == 0)
        return;
    platform_mutex_lock(cl_thread_list_lock);
    for (t = cl_thread_list; t; t = t->next) {
        if ((t->wait_kind == 1 || t->wait_kind == 2) && t->wait_obj == cv_obj) {
            t->wait_kind = 0;
            platform_atomic_dec(&cv->waiters);
            if (t->park) platform_unpark(t->park);
            if (!all) break;
        }
    }
    platform_mutex_unlock(cl_thread_list_lock);
}

/* (mp:condition-notify cv) -> nil */
static CL_Obj bi_condition_notify(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    if (!CL_CONDVAR_P(args[0]))
        cl_error(CL_ERR_TYPE,
                 "MP:CONDITION-NOTIFY: argument must be a condition-variable");
    condvar_wake(args[0], 0);
    return CL_NIL;
}

/* (mp:condition-broadcast cv) -> nil */
static CL_Obj bi_condition_broadcast(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    if (!CL_CONDVAR_P(args[0]))
        cl_error(CL_ERR_TYPE,
                 "MP:CONDITION-BROADCAST: argument must be a condition-variable");
    condvar_wake(args[0], 1);
    return CL_NIL;
}

/* ================================================================
 * Thread interruption
 * ================================================================ */

/* Wake a target parked in MP:CONDITION-WAIT or a blocking MP:ACQUIRE-LOCK
 * after publishing an interrupt/destroy to it.  Called with
 * cl_thread_list_lock HELD, right after the interrupt_pending store: the
 * target registers its wait under the same lock and checks the flag
 * AFTER registering, so either we see its registration here and unpark
 * it (a token that its park consumes even if it has not parked yet), or
 * it sees the flag in its own pre-park check and never parks.  No timed
 * backstop is needed.  The target consumes the interrupt inside the
 * waiting builtin itself (for a condition-wait: after re-acquiring its
 * lock). */
static void wake_interrupted_waiter(CL_Thread *target)
{
    if ((target->wait_kind == 1 || target->wait_kind == 2 ||
         target->wait_kind == 3) && target->park)
        platform_unpark(target->park);
}

/* (mp:interrupt-thread thread function) -> t */
static CL_Obj bi_interrupt_thread(CL_Obj *args, int n)
{
    CL_ThreadObj *tobj;
    CL_Thread *self, *target;
    CL_Obj func;
    CL_UNUSED(n);

    if (!CL_THREAD_P(args[0]))
        cl_error(CL_ERR_TYPE,
                 "MP:INTERRUPT-THREAD: first argument must be a thread");

    func = cl_coerce_funcdesig(args[1], "MP:INTERRUPT-THREAD");

    tobj = (CL_ThreadObj *)CL_OBJ_TO_PTR(args[0]);
    if (tobj->thread_id >= CL_MAX_THREADS)
        cl_error(CL_ERR_GENERAL, "MP:INTERRUPT-THREAD: invalid thread id");

    self = (CL_Thread *)platform_tls_get();

    /* Self-interruption: call the function directly */
    if (tobj->thread_id == self->id) {
        cl_vm_apply(func, NULL, 0);
        return CL_T;
    }

    /* Lookup and publish under the table lock so the reaper / a
     * completing JOIN cannot free `target` between the lookup and the
     * stores (use-after-free write). */
    platform_mutex_lock(cl_thread_list_lock);
    target = cl_thread_table[tobj->thread_id];
    /* Generation mismatch = slot reused by an unrelated worker; without
     * this check the interrupt would be delivered to an innocent thread
     * (see bi_join_thread). */
    if (!target ||
        cl_thread_table_gen[tobj->thread_id] != tobj->table_gen) {
        platform_mutex_unlock(cl_thread_list_lock);
        cl_error(CL_ERR_GENERAL,
                 "MP:INTERRUPT-THREAD: thread has already exited");
    }
    /* status: 0=created, 1=running, 2=finished, 3=aborted */
    if (target->status >= 2) {
        platform_mutex_unlock(cl_thread_list_lock);
        cl_error(CL_ERR_GENERAL,
                 "MP:INTERRUPT-THREAD: thread is no longer running");
    }

    /* Store function, then set the pending flag.  The target's consumer
     * (cl_thread_handle_interrupt) now also takes this lock, but its
     * initial interrupt_pending peek is lock-free, so the payload-before-
     * flag order still needs an explicit barrier — on ARM64 the two
     * stores can otherwise become visible flag-first and the target
     * consumes pending with a stale (NIL) func, silently dropping the
     * interrupt. */
    target->interrupt_func = func;
    platform_memory_barrier();
    target->interrupt_pending = 1;
    platform_memory_barrier();
    /* After the flag: JIT call sites miss and poll it (jit-direct-calls §5). */
    cl_call_gen_bump("interrupt");
    wake_interrupted_waiter(target);
    platform_mutex_unlock(cl_thread_list_lock);

    return CL_T;
}

/* (mp:destroy-thread thread) -> t */
static CL_Obj bi_destroy_thread(CL_Obj *args, int n)
{
    CL_ThreadObj *tobj;
    CL_Thread *self, *target;
    CL_UNUSED(n);

    if (!CL_THREAD_P(args[0]))
        cl_error(CL_ERR_TYPE,
                 "MP:DESTROY-THREAD: argument must be a thread");

    tobj = (CL_ThreadObj *)CL_OBJ_TO_PTR(args[0]);
    if (tobj->thread_id >= CL_MAX_THREADS)
        cl_error(CL_ERR_GENERAL, "MP:DESTROY-THREAD: invalid thread id");

    self = (CL_Thread *)platform_tls_get();

    /* Self-destruction: abort immediately (quietly, like the interrupt path) */
    if (tobj->thread_id == self->id)
        cl_abort_current_thread("Thread destroyed");

    /* Same locking + publication discipline as MP:INTERRUPT-THREAD. */
    platform_mutex_lock(cl_thread_list_lock);
    target = cl_thread_table[tobj->thread_id];
    /* Same slot-identity discipline as MP:INTERRUPT-THREAD. */
    if (!target ||
        cl_thread_table_gen[tobj->thread_id] != tobj->table_gen) {
        platform_mutex_unlock(cl_thread_list_lock);
        cl_error(CL_ERR_GENERAL,
                 "MP:DESTROY-THREAD: thread has already exited");
    }
    if (target->status >= 2) {
        platform_mutex_unlock(cl_thread_list_lock);
        cl_error(CL_ERR_GENERAL,
                 "MP:DESTROY-THREAD: thread is no longer running");
    }

    /* Set destroy flag, then the pending flag (payload before flag —
     * see MP:INTERRUPT-THREAD). */
    target->destroy_requested = 1;
    platform_memory_barrier();
    target->interrupt_pending = 1;
    platform_memory_barrier();
    /* After the flag: JIT call sites miss and poll it (jit-direct-calls §5). */
    cl_call_gen_bump("interrupt");
    wake_interrupted_waiter(target);
    platform_mutex_unlock(cl_thread_list_lock);

    return CL_T;
}

uint32_t cl_thread_stop_workers(uint32_t timeout_ms)
{
    CL_Thread *self = (CL_Thread *)platform_tls_get();
    uint32_t i, asked = 0, start, now;

    if (!cl_thread_list_lock) return 0;

    /* Same publication discipline as MP:DESTROY-THREAD, for every worker. */
    platform_mutex_lock(cl_thread_list_lock);
    for (i = 0; i < CL_MAX_THREADS; i++) {
        CL_Thread *t = cl_thread_table[i];
        if (!t || t == self || t->status >= 2) continue;
        t->destroy_requested = 1;
        platform_memory_barrier();
        t->interrupt_pending = 1;
        platform_memory_barrier();
        cl_call_gen_bump("interrupt");
        wake_interrupted_waiter(t);
        asked++;
    }
    platform_mutex_unlock(cl_thread_list_lock);
    if (!asked) return 0;

    /* The unwinding workers may need a GC; we must not hold them off.
     * The bound is measured by the clock, not by summing the naps: a
     * 20 ms Sleep on Windows takes a scheduler tick or two (15.6 ms each),
     * so a counted bound overshoots (platform_thread_drain had the same
     * bug, 3x). */
    cl_gc_enter_safe_region();
    start = platform_time_ms();
    while (cl_thread_count > 1) {
        now = platform_time_ms();
        if (now < start) start = now;          /* clock wrapped (Amiga: midnight) */
        if (now - start >= timeout_ms) break;
        platform_sleep_ms(20);
    }
    cl_gc_leave_safe_region();
    return cl_thread_count > 1 ? cl_thread_count - 1 : 0;
}

/* (mp::%stop-workers timeout-ms) -> number of workers still registered
 * Test hook for the AmigaOS process-exit path (cl_thread_stop_workers). */
static CL_Obj bi_stop_workers(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    if (!CL_FIXNUM_P(args[0]) || CL_FIXNUM_VAL(args[0]) < 0)
        cl_error(CL_ERR_TYPE, "MP::%%STOP-WORKERS: TIMEOUT-MS must be a "
                 "non-negative fixnum");
    return CL_MAKE_FIXNUM((int32_t)cl_thread_stop_workers(
                              (uint32_t)CL_FIXNUM_VAL(args[0])));
}

/* ================================================================
 * Accessors and predicates
 * ================================================================ */

/* (mp:condition-name cv) -> string/nil */
static CL_Obj bi_condition_name(CL_Obj *args, int n)
{
    CL_CondVar *cv;
    CL_UNUSED(n);

    if (!CL_CONDVAR_P(args[0]))
        cl_error(CL_ERR_TYPE,
                 "MP:CONDITION-NAME: argument must be a condition-variable");

    cv = (CL_CondVar *)CL_OBJ_TO_PTR(args[0]);
    return cv->name;
}

/* (mp:lock-name lock) -> string/nil */
static CL_Obj bi_lock_name(CL_Obj *args, int n)
{
    CL_Lock *lk;
    CL_UNUSED(n);

    if (!CL_LOCK_P(args[0]))
        cl_error(CL_ERR_TYPE, "MP:LOCK-NAME: argument must be a lock");

    lk = (CL_Lock *)CL_OBJ_TO_PTR(args[0]);
    return lk->name;
}

/* (mp::%condvar-waiters cv) -> fixnum
 * The number of threads currently registered as waiting on CV.  A test /
 * triage hook: after every waiter has returned (notified, timed out or
 * interrupted) it must read 0 — a non-zero count with nobody waiting is a
 * bookkeeping bug in the wait/notify protocol. */
static CL_Obj bi_condvar_waiters(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    if (!CL_CONDVAR_P(args[0]))
        cl_error(CL_ERR_TYPE,
                 "MP::%%CONDVAR-WAITERS: argument must be a condition-variable");
    return CL_MAKE_FIXNUM((int32_t)((CL_CondVar *)CL_OBJ_TO_PTR(args[0]))->waiters);
}

/* (mp::%os-thread-count) -> fixnum
 * Worker OS threads still executing amilisp code — a thread counts until
 * the very end of its platform entry wrapper, after it has left the MP
 * registry (see platform_thread_live_count).  A test / triage hook. */
static CL_Obj bi_os_thread_count(CL_Obj *args, int n)
{
    CL_UNUSED(args); CL_UNUSED(n);
    return CL_MAKE_FIXNUM((int32_t)platform_thread_live_count());
}

/* (mp::%drain-os-threads keep timeout-ms) -> fixnum
 * Wait until at most KEEP worker OS threads are live, or TIMEOUT-MS passed;
 * returns how many beyond KEEP are still live (0 = drained).  The wait that
 * process exit runs (cl_thread_shutdown), exposed for tests. */
static CL_Obj bi_drain_os_threads(CL_Obj *args, int n)
{
    uint32_t keep, left;
    CL_UNUSED(n);
    if (!CL_FIXNUM_P(args[0]) || CL_FIXNUM_VAL(args[0]) < 0 ||
        !CL_FIXNUM_P(args[1]) || CL_FIXNUM_VAL(args[1]) < 0)
        cl_error(CL_ERR_TYPE, "MP::%%DRAIN-OS-THREADS: KEEP and TIMEOUT-MS "
                 "must be non-negative fixnums");
    keep = (uint32_t)CL_FIXNUM_VAL(args[0]);
    cl_gc_enter_safe_region();
    left = platform_thread_drain(&keep, (uint32_t)CL_FIXNUM_VAL(args[1]));
    cl_gc_leave_safe_region();
    return CL_MAKE_FIXNUM((int32_t)left);
}

/* (mp::%set-thread-exit-delay ms) -> previous ms
 * Test hook: every worker stalls MS after its function returned, before its
 * OS thread counts as gone — holds the exit window open that process exit
 * must wait out (see platform_thread_set_exit_delay). */
static CL_Obj bi_set_thread_exit_delay(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    if (!CL_FIXNUM_P(args[0]) || CL_FIXNUM_VAL(args[0]) < 0)
        cl_error(CL_ERR_TYPE, "MP::%%SET-THREAD-EXIT-DELAY: MS must be a "
                 "non-negative fixnum");
    return CL_MAKE_FIXNUM((int32_t)platform_thread_set_exit_delay(
                              (uint32_t)CL_FIXNUM_VAL(args[0])));
}

/* (mp::%lock-held-p lock) -> bool
 * Whether LOCK is currently held by any thread (racy by nature; a test /
 * triage hook — the ownership check itself lives in RELEASE-LOCK). */
static CL_Obj bi_lock_held_p(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    if (!CL_LOCK_P(args[0]))
        cl_error(CL_ERR_TYPE, "MP::%%LOCK-HELD-P: argument must be a lock");
    return ((CL_Lock *)CL_OBJ_TO_PTR(args[0]))->state != 0 ? CL_T : CL_NIL;
}

/* (mp:threadp obj) -> bool */
static CL_Obj bi_threadp(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    return CL_THREAD_P(args[0]) ? CL_T : CL_NIL;
}

/* (mp:lockp obj) -> bool */
static CL_Obj bi_lockp(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    return CL_LOCK_P(args[0]) ? CL_T : CL_NIL;
}

/* (mp:condition-variable-p obj) -> bool */
static CL_Obj bi_condition_variable_p(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    return CL_CONDVAR_P(args[0]) ? CL_T : CL_NIL;
}

/* ================================================================
 * Compare-and-swap cell builtins — the primitives MP:COMPARE-AND-SWAP,
 * MP:ATOMIC-INCF and MP:ATOMIC-DECF (boot.lisp) expand into, one per
 * place kind.  Every Lisp place is a single 32-bit CL_Obj cell in the
 * arena, so each is one cl_cas_cell on that cell.  All of them return
 * the value the cell held when the CAS was decided: EQ to OLD exactly
 * when the swap happened (SBCL's CAS convention).
 *
 * GC: nothing here allocates between computing the cell address and
 * the CAS, and the operands are rooted in args[], so no protection is
 * needed.  Under the host's generational collector the first store to
 * a read-protected old-space page takes the write-watch fault like any
 * other store: the handler unprotects the page and the CAS instruction
 * re-executes.
 * ================================================================ */

/* (mp::%cas-car cons old new) -> previous car */
static CL_Obj bi_cas_car(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    if (!CL_CONS_P(args[0]))
        cl_signal_type_error(args[0], "CONS", "MP:COMPARE-AND-SWAP (CAR)");
    return cl_cas_cell(&((CL_Cons *)CL_OBJ_TO_PTR(args[0]))->car,
                       args[1], args[2]);
}

/* (mp::%cas-cdr cons old new) -> previous cdr */
static CL_Obj bi_cas_cdr(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    if (!CL_CONS_P(args[0]))
        cl_signal_type_error(args[0], "CONS", "MP:COMPARE-AND-SWAP (CDR)");
    return cl_cas_cell(&((CL_Cons *)CL_OBJ_TO_PTR(args[0]))->cdr,
                       args[1], args[2]);
}

/* (mp::%cas-svref simple-vector index old new) -> previous element.
 * Same admission as SVREF: a rank-1 general vector without fill pointer,
 * displacement or adjustability (flags == 0). */
static CL_Obj bi_cas_svref(CL_Obj *args, int n)
{
    CL_Vector *vec;
    int32_t idx;
    CL_UNUSED(n);
    if (!CL_VECTOR_P(args[0]))
        cl_signal_type_error(args[0], "SIMPLE-VECTOR",
                             "MP:COMPARE-AND-SWAP (SVREF)");
    vec = (CL_Vector *)CL_OBJ_TO_PTR(args[0]);
    if (vec->rank > 1 || vec->flags != 0)
        cl_signal_type_error(args[0], "SIMPLE-VECTOR",
                             "MP:COMPARE-AND-SWAP (SVREF)");
    if (!CL_FIXNUM_P(args[1]))
        cl_signal_type_error(args[1], "FIXNUM",
                             "MP:COMPARE-AND-SWAP (SVREF index)");
    idx = CL_FIXNUM_VAL(args[1]);
    if (idx < 0 || (uint32_t)idx >= vec->length)
        cl_error(CL_ERR_ARGS,
                 "MP:COMPARE-AND-SWAP (SVREF): index %d out of range for a "
                 "vector of length %u", (int)idx, (unsigned)vec->length);
    return cl_cas_cell(&cl_vector_data(vec)[idx], args[2], args[3]);
}

/* (mp::%cas-struct-slot struct index old new) -> previous slot value.
 * STRUCT is a defstruct instance or a CLOS instance (both TYPE_STRUCT);
 * INDEX is the positional slot index, as for CLAMIGA::%STRUCT-REF. */
static CL_Obj bi_cas_struct_slot(CL_Obj *args, int n)
{
    CL_Struct *st;
    int32_t idx;
    CL_UNUSED(n);
    if (!CL_STRUCT_P(args[0]))
        cl_signal_type_error(args[0], "STRUCTURE-OBJECT",
                             "MP:COMPARE-AND-SWAP (structure slot)");
    if (!CL_FIXNUM_P(args[1]))
        cl_signal_type_error(args[1], "FIXNUM",
                             "MP:COMPARE-AND-SWAP (structure slot index)");
    st = (CL_Struct *)CL_OBJ_TO_PTR(args[0]);
    idx = CL_FIXNUM_VAL(args[1]);
    if (idx < 0 || (uint32_t)idx >= st->n_slots)
        cl_error(CL_ERR_ARGS,
                 "MP:COMPARE-AND-SWAP (structure slot): index %d out of range "
                 "(n_slots=%u)", (int)idx, (unsigned)st->n_slots);
    return cl_cas_cell(&st->slots[idx], args[2], args[3]);
}

/* (mp::%cas-symbol-value symbol old new) -> previous value.
 * Targets the calling thread's dynamic binding when one is in effect,
 * else the global value cell — the same resolution SYMBOL-VALUE and SET
 * use, so (let ((*x* ..)) (cas (symbol-value '*x*) ..)) never touches
 * the global cell.  Constants are rejected as by SET; an unbound target
 * signals UNBOUND-VARIABLE as SYMBOL-VALUE would. */
static CL_Obj bi_cas_symbol_value(CL_Obj *args, int n)
{
    CL_Symbol *s;
    CL_Obj prev;
    CL_UNUSED(n);
    if (!CL_SYMBOL_OR_NIL_P(args[0]))
        cl_signal_type_error(args[0], "SYMBOL",
                             "MP:COMPARE-AND-SWAP (SYMBOL-VALUE)");
    s = (CL_Symbol *)CL_OBJ_TO_PTR(CL_NULL_P(args[0]) ? SYM_NIL : args[0]);
    if (s->flags & CL_SYM_CONSTANT)
        cl_error(CL_ERR_GENERAL,
                 "MP:COMPARE-AND-SWAP: cannot assign to constant variable %s",
                 cl_symbol_name(args[0]));
    prev = cl_symbol_value_cas(args[0], args[1], args[2]);
    if (prev == CL_UNBOUND)
        cl_signal_unbound_variable(args[0]);
    return prev;
}

/* (mp::%symbol-specialp symbol) -> generalized boolean.
 * Backs the bare-symbol-place check in MP::%ATOMIC-PLACE (boot.lisp):
 * a plain symbol only has a shared value cell to CAS when it is globally
 * special (DEFVAR/DEFPARAMETER/PROCLAIM SPECIAL) — an ordinary lexical
 * has none, and treating it as one would silently CAS an unrelated
 * global instead of erroring. */
static CL_Obj bi_symbol_specialp(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    if (!CL_SYMBOL_OR_NIL_P(args[0]))
        cl_signal_type_error(args[0], "SYMBOL", "MP::%SYMBOL-SPECIALP");
    return cl_symbol_specialp(args[0]) ? SYM_T : CL_NIL;
}

/* ================================================================
 * Registration
 * ================================================================ */

/* Helper: register a builtin in the MP package and export it.  Must go
 * through cl_register_builtin_exported so the image-relink registry sees
 * the registration (builtins.h). */
static void mp_defun(const char *name, CL_CFunc func, int min, int max)
{
    cl_register_builtin_exported(name, func, min, max, cl_package_mp);
}

/* Internal (unexported) MP helper — reached from the MP macros in boot.lisp
 * and, for a library backend, as mp::%name. */
static void mp_defun_internal(const char *name, CL_CFunc func, int min, int max)
{
    cl_register_builtin(name, func, min, max, cl_package_mp);
}

void cl_builtins_thread_init(void)
{
    CL_ThreadObj *tobj;
    CL_Obj main_name;

    /* Pre-intern keywords */
    KW_NAME_THR          = cl_intern_keyword("NAME", 4);
    KW_STACK_SIZE_THR    = cl_intern_keyword("STACK-SIZE", 10);
    KW_VM_STACK_SIZE_THR = cl_intern_keyword("VM-STACK-SIZE", 13);
    KW_VM_FRAMES_THR     = cl_intern_keyword("VM-FRAMES", 9);
    KW_NLX_FRAMES_THR    = cl_intern_keyword("NLX-FRAMES", 10);

    /* Register the lazily-built abort handler/report caches as GC roots
     * exactly once, here at (single-threaded) boot.  Doing it inside the
     * lazy getters raced: two concurrent first-worker creations could
     * both register the same address, and gc_forward's non-idempotence
     * turns a double-registered root into silent corruption on the next
     * compaction. */
    cl_gc_register_root(&thread_abort_handler);
    cl_gc_register_root(&thread_abort_report);

    /* Create main thread's Lisp-visible thread object.  Give it a default
     * name ("main thread") to match bordeaux-threads / SBCL / CCL, which
     * name the initial thread rather than leaving it NIL — so ALL-THREADS
     * prints #<THREAD main thread> instead of #<THREAD NIL>.
     *
     * Build the name string first and GC-protect it: cl_alloc(TYPE_THREAD)
     * below can trigger compaction, which would relocate an unprotected
     * string. */
    main_name = cl_make_string("main thread", 11);
    CL_GC_PROTECT(main_name);
    tobj = (CL_ThreadObj *)cl_alloc(TYPE_THREAD, sizeof(CL_ThreadObj));
    if (tobj) {
        tobj->thread_id = 0;
        tobj->table_gen = cl_thread_table_gen[0];
        tobj->name = main_name;
        main_thread_obj = CL_PTR_TO_OBJ(tobj);
    }
    CL_GC_UNPROTECT(1);

    /* Keep the C-level main-thread record's name in sync, so the
     * bi_current_thread fallback path (which copies self->name) and any
     * THREAD-NAME on a freshly-built wrapper agree with main_thread_obj. */
    if (cl_main_thread_ptr)
        cl_main_thread_ptr->name = main_name;

    /* MP:*THREAD-DEATH-HOOKS* (cl_run_thread_death_hooks in builtins.c) */
    {
        CL_Obj hooks_sym = cl_intern_in("*THREAD-DEATH-HOOKS*", 20, cl_package_mp);
        CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(hooks_sym);
        s->flags |= CL_SYM_SPECIAL;
        s->value = CL_NIL;
        cl_export_symbol(hooks_sym, cl_package_mp);
    }

    /* Register MP builtins */
    mp_defun("MAKE-THREAD",             bi_make_thread,             1, -1);
    mp_defun("JOIN-THREAD",             bi_join_thread,             1, 1);
    mp_defun("THREAD-ALIVE-P",          bi_thread_alive_p,         1, 1);
    mp_defun("CURRENT-THREAD",          bi_current_thread,          0, 0);
    mp_defun("ALL-THREADS",             bi_all_threads,             0, 0);
    mp_defun("THREAD-NAME",             bi_thread_name,             1, 1);
    mp_defun("THREAD-YIELD",            bi_thread_yield,            0, 0);
    mp_defun("DUMP-THREAD-WAITS",       bi_dump_thread_waits,       0, 0);

    mp_defun("MAKE-LOCK",               bi_make_lock,               0, 1);
    mp_defun("%MAKE-RECURSIVE-LOCK",    bi_make_recursive_lock,     0, 1);
    mp_defun("ACQUIRE-LOCK",            bi_acquire_lock,            1, 3);
    mp_defun("RELEASE-LOCK",            bi_release_lock,            1, 1);

    mp_defun("MAKE-CONDITION-VARIABLE",  bi_make_condition_variable, 0, 1);
    mp_defun("CONDITION-WAIT",           bi_condition_wait,          2, 3);
    mp_defun("CONDITION-NOTIFY",         bi_condition_notify,        1, 1);
    mp_defun("CONDITION-BROADCAST",      bi_condition_broadcast,     1, 1);
    mp_defun("CONDITION-NAME",           bi_condition_name,          1, 1);

    mp_defun("LOCK-NAME",               bi_lock_name,               1, 1);

    mp_defun("INTERRUPT-THREAD",         bi_interrupt_thread,        2, 2);
    mp_defun("DESTROY-THREAD",           bi_destroy_thread,          1, 1);

    mp_defun("THREADP",                  bi_threadp,                 1, 1);
    mp_defun("LOCKP",                    bi_lockp,                   1, 1);
    mp_defun("CONDITION-VARIABLE-P",     bi_condition_variable_p,    1, 1);

    /* Compare-and-swap cell primitives (MP:COMPARE-AND-SWAP expands to these) */
    mp_defun_internal("%CONDVAR-WAITERS",  bi_condvar_waiters,  1, 1);
    mp_defun_internal("%LOCK-HELD-P",      bi_lock_held_p,      1, 1);
    mp_defun_internal("%OS-THREAD-COUNT",  bi_os_thread_count,  0, 0);
    mp_defun_internal("%DRAIN-OS-THREADS", bi_drain_os_threads, 2, 2);
    mp_defun_internal("%SET-THREAD-EXIT-DELAY", bi_set_thread_exit_delay, 1, 1);
    mp_defun_internal("%STOP-WORKERS",     bi_stop_workers,     1, 1);
    mp_defun_internal("%CAS-CAR",          bi_cas_car,          3, 3);
    mp_defun_internal("%CAS-CDR",          bi_cas_cdr,          3, 3);
    mp_defun_internal("%CAS-SVREF",        bi_cas_svref,        4, 4);
    mp_defun_internal("%CAS-STRUCT-SLOT",  bi_cas_struct_slot,  4, 4);
    mp_defun_internal("%CAS-SYMBOL-VALUE", bi_cas_symbol_value, 3, 3);
    mp_defun_internal("%SYMBOL-SPECIALP",  bi_symbol_specialp, 1, 1);

    /* Register cached symbols for GC compaction forwarding */
    cl_gc_register_root(&KW_NAME_THR);
    cl_gc_register_root(&KW_STACK_SIZE_THR);
    cl_gc_register_root(&KW_VM_STACK_SIZE_THR);
    cl_gc_register_root(&KW_VM_FRAMES_THR);
    cl_gc_register_root(&KW_NLX_FRAMES_THR);
}
