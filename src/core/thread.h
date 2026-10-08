#ifndef CL_THREAD_H
#define CL_THREAD_H

/*
 * CL_Thread: per-thread execution state.
 *
 * Phase 0: single global instance, compatibility macros.
 * Phase 1: TLS-backed cl_current_thread.
 * Phase 2: thread registry, GC coordination (safepoints, STW).
 */

#include "vm.h"
#include "error.h"
#include "mem.h"
#include "../platform/platform_thread.h"  /* platform_tls_get (inline on POSIX) */

/* Forward declarations */
struct CL_Compiler_s;

/* Constants for thread-local array sizes */
#define CL_PP_INDENT_MAX     32
#define CL_CIRCLE_HT_SIZE    256
#define CL_VM_TRACE_SIZE     64

/* ---- Per-thread slot-resolution cache (builtins_struct.c) ----
 *
 * SLOT-VALUE / (SETF SLOT-VALUE) / SLOT-BOUNDP resolve a (type, slot-name)
 * pair to a slot index on EVERY access.  The shared pair index answers that
 * in O(1), but reaching it costs a tables rdlock and unlock — a real
 * pthread_rwlock pair, ~2% of the sento profile on its own — plus the hash
 * probe.  This cache answers the repeat case with three compares and no
 * lock at all.
 *
 * Per-thread on purpose: a shared table would need either a lock (the cost
 * being removed) or atomic multi-word publication, since a torn read of
 * (type, slot, index) across threads would return an index belonging to a
 * different type.  Nothing here is shared, so there is nothing to tear.
 *
 * Entries are validated against cl_struct_layout_gen, which is bumped by
 * every event that can invalidate them: a type (re)registration, and a
 * compaction that moves the symbols the keys are made of.  A stale entry
 * therefore fails the generation compare before its (possibly recycled)
 * key offsets are ever compared.  gen 0 never matches, so a zeroed thread
 * starts with an empty cache. */
#define CL_SLOT_IC_SIZE      64   /* power of two */

typedef struct {
    CL_Obj   type;   /* instance type_desc symbol */
    CL_Obj   slot;   /* slot-name symbol */
    int32_t  idx;    /* resolved position, or -1 for "no such slot" */
    uint32_t gen;    /* cl_struct_layout_gen this entry was resolved under */
} CL_SlotICEntry;

/* Saved pending-throw stack depth (pushed once per active UWP arming, i.e.
 * the nesting depth of live UNWIND-PROTECT forms; special bindings use
 * OP_DYNBIND and do not count).  An entry is ~600 bytes (it carries a copy of
 * the pending error message).  68k AmigaOS gets 128 -- half the 256-entry
 * stack's ~150 KB of Fast RAM per process, twice its workers' 64 because the
 * main thread is where ASDF's nested WITH-* forms run during a system load.
 * Overflow is a clean, catchable CL_ERR_OVERFLOW ("saved-pending stack
 * overflow", OP_UWPROT in vm.c); the Amiga suite tests it.  See
 * CL_MAX_NLX_FRAMES in vm.h for the rationale.  MorphOS and the host keep
 * 256. */
#if defined(PLATFORM_AMIGA) && !defined(PLATFORM_MORPHOS)
#define CL_MAX_SAVED_PENDING 128
#else
#define CL_MAX_SAVED_PENDING 256
#endif

/* ---- Thread-Local Value (TLV) table ---- */
#define CL_TLV_TABLE_SIZE  256
#define CL_TLV_ABSENT      ((CL_Obj)0xFFFFFFFE)  /* "no TLV entry" sentinel */

typedef struct {
    CL_Obj symbol;  /* CL_NIL = empty slot, CL_UNBOUND = tombstone */
    CL_Obj value;
} CL_TLVEntry;

/* Snapshot of all pending-throw fields, saved/restored around UWP cleanups
 * so a nested unwind-protect inside a cleanup cannot clobber the outer
 * non-local transfer. */
typedef struct {
    int    pending_throw;
    CL_Obj pending_tag;
    CL_Obj pending_value;
    int    pending_mv_count;
    CL_Obj pending_mv_values[CL_MAX_MV];
    int    pending_error_code;
    char   pending_error_msg[512];
    int    entered_via_longjmp; /* 1 if our UWPROT longjmp was triggered */
} CL_SavedPending;

typedef struct CL_Thread_s {
    /* ---- Thread metadata ---- */
    uint32_t id;
    CL_Obj   name;           /* CL string or NIL */
    uint8_t  status;         /* 0=created, 1=running, 2=finished, 3=aborted */
    CL_Obj   result;

    /* ---- VM instance ---- */
    CL_VM    vm;

    /* ---- Dynamic binding stack ---- */
    int               dyn_top;

    /* ---- NLX stack (heap-allocated) ---- */
    CL_NLXFrame      *nlx_stack;
    int               nlx_max;
    int               nlx_top;

    /* ---- Saved pending-throw stack (heap-allocated) ----
     * Pushed once per active OP_UWPROT arming so nested UWPs inside a
     * cleanup cannot clobber the outer non-local transfer state. */
    CL_SavedPending  *saved_pending_stack;
    int               saved_pending_top;
    int               saved_pending_max;

    /* ---- Handler stack ---- */
    int               handler_top;
    int               handler_floor;
    /* Per-handler "active" bit (bit j = handler_stack[j] enabled).  This is the
     * authoritative replacement for the per-frame `active` flag: it is snapshot
     * into every NLX / error frame at establishment and restored verbatim on
     * unwind, so the CLHS 9.1.4 "disabled band" set by a running handler is
     * automatically restored when the handler exits via a NON-LOCAL transfer
     * (e.g. a handler that INVOKE-RESTARTs a restart in its own dynamic extent
     * — fiveam's process-failure pattern).  Before this, the band-restore lived
     * only in C locals in cl_signal_condition and was skipped by the longjmp,
     * leaving the handler permanently disabled so the next signal escaped.
     * CL_MAX_HANDLER_BINDINGS == 64 fits exactly in a uint64_t. */
    uint64_t          handler_active_mask;

    /* ---- Restart stack ---- */
    int               restart_top;
    int               restart_floor;

    /* ---- Foreign-callback boundary (builtins_ffi.c, ffi_callback_handler) ----
     * While Lisp runs inside a C callback — a struct Hook entry MUI or
     * ReAction invoked, a qsort comparator — no non-local exit may cross the
     * C frames of the foreign caller (held semaphores, dispatcher state,
     * Forbid() nesting).  nlx_floor hides the NLX frames established
     * outside the callback from THROW / RETURN-FROM / GO: every tag-search
     * loop stops at it, so the miss becomes an ordinary "no catch" error
     * caught at the boundary — the same way handler_floor / restart_floor
     * hide the outer handlers and restarts.  callback_depth > 0 keeps the
     * interactive debugger (and *DEBUGGER-HOOK*) out, see
     * cl_invoke_debugger.  A condition that reaches the boundary is parked
     * in callback_error / callback_error_code (its message in
     * callback_error_msg) and re-signaled by cl_ffi_deferred_error_check
     * once the foreign call that invoked the callback has returned.
     * last_condition is the condition an unwind is currently carrying
     * (written by cl_error / cl_error_from_condition immediately before
     * they longjmp), which is how the boundary gets hold of the object the
     * catch site would otherwise never see. */
    int    nlx_floor;
    int    callback_depth;
    int    callback_error_code;     /* 0 = nothing pending */
    CL_Obj callback_error;          /* the parked condition, NIL for CL_ERR_EXIT */
    CL_Obj last_condition;

    /* ---- Pending throw state ---- */
    int    pending_throw;      /* 0=none, 1=throw, 2=error */
    CL_Obj pending_tag;
    CL_Obj pending_value;
    int    pending_error_code;
    int    pending_mv_count;
    CL_Obj pending_mv_values[CL_MAX_MV];

    /* ---- Error frames ---- */
    int           error_frame_top;
    int           error_code;
    int           exit_code;

    /* ---- Multiple values ---- */
    CL_Obj mv_values[CL_MAX_MV];
    int    mv_count;
    /* Ctrl-C break poll counter (vm.c VM_POLL_BREAK): bumped on every
     * OP_CALL/OP_TAILCALL and backward OP_JMP, so it MUST be per-thread —
     * as a process-wide static every thread wrote the same cache line on
     * every call, which cost the 8-thread sento matrix 10–27% (see
     * docs/sento-bench-results-0.8.md). */
    uint32_t break_poll_ctr;
    /* Crash diagnostics: the CL_Function object of the builtin this thread
     * entered last, stored by call_builtin and printed by main.c's fatal
     * handler.  Per-thread for the same reason as break_poll_ctr — a
     * process-wide static written on every builtin call makes all threads
     * bounce one cache line.  Deliberately NOT a GC root: the fatal
     * handler validates it before dereferencing, and rooting it would keep
     * a dead builtin alive to no purpose. */
    CL_Obj last_builtin;
    /* mv state captured by call_builtin before its per-call reset; lets
     * NLX builtins (THROW) see the multiple values of their last argument. */
    int    pre_call_mv_count;
    CL_Obj pre_call_mv_values[CL_MAX_MV];

    /* ---- Lisp-visible thread object (CL_ThreadObj wrapping this thread) ----
     * Set at thread creation, returned by (mp:current-thread). Must remain
     * eq across calls so callers can use it as a hash-table key (e.g.
     * bordeaux-threads-2's .known-threads. which keys by native-thread). */
    CL_Obj thread_obj;

    /* ---- GC root stack (per-thread) ---- */
    int     gc_root_count;

    /* ---- Reader state ---- */
    CL_Obj      rd_stream;
    int         rd_eof;
    int         rd_suppress;   /* counter for nested #+/#- */
    int         rd_line;
    int         rd_last_eof;   /* EOF flag of the most recently completed
                                * cl_read_from_stream — survives across
                                * nested reader invocations, so bi_read
                                * still sees whether its own read hit EOF. */
    CL_Obj      rd_uninterned; /* Alist (name . symbol) of #:foo tokens
                                * read so far in the current READ call.
                                * Per CLHS 2.4.8.5: within one READ, all
                                * #:foo with the same name denote the
                                * same uninterned symbol. */
    CL_Obj      rd_labels;     /* Alist (label-fixnum . object) for the
                                * #n= / #n# shared/circular-structure reader
                                * macros (CLHS 2.4.8.15-16).  Scoped to one
                                * top-level READ call. */
    int         rd_label_backrefs; /* count of #n# that resolved to a still-
                                * incomplete label (i.e. a circular forward
                                * reference); when >0, #n= must fix up the
                                * placeholder after reading its object. */
    const char *current_source_file;
    uint16_t    current_file_id;

    /* ---- Printer state ---- */
    int32_t pr_depth;
    int32_t pr_column;
    int32_t pr_indent_top;
    int     pr_circle_count;
    int     pr_circle_next_label;
    int     pr_circle_active;
    CL_Obj  pr_stream;
    int     pr_to_buffer;
    char   *pr_out_buf;
    int     pr_out_pos;
    int     pr_out_size;
    int     pr_pprint_dispatch_active;

    /* In-progress object stack for print-object-hook re-entrancy detection.
     * Pushed before cl_vm_apply on a hook, popped after.  If the hook would
     * be fired for an object already on the stack, we emit "#<...>" and
     * skip the apply — terminates Lisp-side circular print-object recursion
     * (e.g. sento's actor-cell ↔ message-box ↔ queue ↔ message-item cycle). */
#define CL_PR_INPROG_MAX 32
    int     pr_inprog_top;

    /* ---- Compiler chain ---- */
    struct CL_Compiler_s *active_compiler;
    CL_Obj pending_lambda_name;

    /* ---- Macroexpansion lexical environment ----
     * Set by cl_macroexpand_1_env before cl_vm_apply of the expander,
     * restored afterwards.  Exposed to user macros as the value of
     * their &environment parameter via the CLAMIGA::%MACROEXPAND-ENV
     * builtin.  Represented as an alist of (SYMBOL . EXPANSION) pairs
     * capturing the symbol-macros active at the macro call site.
     * CL_NIL = empty env. */
    CL_Obj current_lex_env;

    /* ---- Per-thread current package ----
     * The C-side mirror of *PACKAGE* consumed by the reader, printer,
     * INTERN and the package-function defaults (via the cl_current_package
     * macro in package.h).  One slot PER THREAD: a *PACKAGE* bind/unbind/
     * setq on one thread must never redirect another thread's reader.
     * When this was a single shared global, Sly's control thread — which
     * binds *PACKAGE* to SLYNK-IO-PACKAGE around every wire message —
     * clobbered it while a worker thread ran COMPILE-FILE, so the worker's
     * reader interned random tokens into SLYNK-IO-PACKAGE: the lambda-list
     * and body occurrences of a defmacro parameter became two distinct
     * symbols, the body reference compiled to a global lookup, and the
     * poisoned FASL failed forever after with "Unbound variable" at
     * macroexpansion time.  Kept in sync with THIS thread's dynamic
     * *PACKAGE* view by cl_sync_current_package_from_dynamic; initialized
     * for workers from the GLOBAL value of *PACKAGE* in thread_entry
     * (fresh dynamic environment, like every other special).  Marked and
     * forwarded in gc_visit_thread_roots. */
    CL_Obj current_package;

    /* ---- Trace & debug ---- */
    int    trace_depth;
    int    trace_count;
    /* Formatted backtrace text.  backtrace_buf points at backtrace_inline
     * until a deep backtrace outgrows it, then at a platform_alloc'd block of
     * backtrace_cap bytes (see bt_reserve in vm.c).  cl_thread_backtrace_init
     * runs for every thread, so backtrace_buf is never NULL — the ~50
     * `fprintf(stderr, "%s", cl_backtrace_buf)` diagnostics can print it
     * unconditionally.  backtrace_inline stays immediately before
     * c_stack_base: that adjacency is what the overflow regression test in
     * tests/test_backtrace.c detects a runaway append with. */
    char  *backtrace_buf;
    int    backtrace_cap;
    int    debug_base_fp;   /* VM frame depth snapshot at error time, so the
                             * debugger-hook (SLDB) sees the error-time
                             * backtrace rather than its own pushed frames. */

    /* ---- JIT native-stack GC scan window ----
     * jit_depth: number of nested cl_jit_invoke frames currently on the
     *   C stack for this thread.  0 = thread is not inside JIT'd code.
     * jit_stack_top: captured SP of the OUTERMOST cl_jit_invoke frame
     *   (higher address on a stack-grows-down architecture).  The GC
     *   conservatively scans [current SP .. jit_stack_top) for words
     *   that look like heap offsets.  Only meaningful when jit_depth>0.
     *
     * See specs/native-backend.md §"GC interaction" — option A. */
    int    jit_depth;
    void  *jit_stack_top;
    /* Lowest SP native code may run at before a call must take the C
     * helper path (which signals "C stack nearly exhausted"): the outermost
     * cl_jit_invoke sets it to SP - platform_stack_headroom() + 16K, the
     * margin cl_check_c_stack keeps.  NULL = headroom unknown, the compare
     * never fires.  Read by native code through A3 (= this CL_Thread, set
     * by cl_jit_enter), so it costs no FindTask per call.  Like
     * jit_stack_top it is only meaningful while jit_depth > 0.  See
     * specs/jit-direct-calls.md §4. */
    char  *jit_c_floor;
    /* Countdown of native loop iterations to the next poll.  Every loop
     * header in native code (the target of a backward branch, and the
     * entry of a function that may tail-call itself) decrements it through
     * A3; when it wraps, cl_jit_runtime_loop_poll runs the interpreter's
     * OP_JMP checks -- GC safepoint, pending interrupt, Ctrl-C -- and
     * resets it to CL_JIT_LOOP_POLL_EVERY - 1.  Zero-initialised like the
     * rest of the struct, so the first header polls at once. */
    uint16_t jit_loop_ctr;
    /* Captured C stack pointer at the moment this thread PARKED for a
     * stop-the-world GC (safepoint or safe-region entry).  A peer thread that
     * initiates a compaction cannot use its own SP as the lower bound when
     * conservatively scanning THIS thread's JIT native stack — it must use the
     * point below which this thread's JIT-spilled operands live, i.e. where it
     * froze.  The GC scans [jit_park_sp .. jit_stack_top) for such peers.  Only
     * meaningful while the thread is stopped with jit_depth > 0. */
    void  *jit_park_sp;

#ifdef JIT_A64
    /* cl_jit_invoke entries on this thread (%JIT-INVOKE-COUNT reads the
     * calling thread's).  Per-thread because it is written on every native
     * entry: a process-wide counter there is the shared cache line the VM
     * hot-path rule in CLAUDE.md forbids.  The single-CPU m68k backend keeps
     * its global (jit_common.c). */
    uint32_t jit_invoke_count;
    /* A native non-self tail call to native code (cl_jit_vmstack_tail)
     * leaves its callee's arguments at the frame base, the callee above
     * them, and this set to nargs + 1; cl_jit_invoke then enters the callee
     * from the same frame base instead of nesting a C call, so mutual tail
     * recursion runs in constant space as in the interpreter.  0 = none. */
    uint32_t jit_tail_pending;
    /* The direct-call counters (CL_JIT_DS_*, jit.h; %JIT-DIRECT-CALL-STATS
     * reads the calling thread's).  Written on the miss path, which every
     * call to a builtin takes -- per-thread for the same reason as
     * jit_invoke_count. */
    uint32_t jit_ds[6];
#endif

    /* ---- VM extras ---- */
    int    vm_extra_count;


    /* ---- VM eval depth tracking ---- */
    int    vm_eval_depth_val;
    int    vm_max_eval_depth;
    long   c_stack_max_val;

    /* ---- VM trace buffer (crash diagnostics) ---- */
    int vm_trace_pos;

    /* ---- Crash diagnostics ---- */
    volatile uint8_t  dbg_last_op;
    volatile uint32_t dbg_last_ip;
    volatile int      dbg_last_fp;
    volatile uint8_t *dbg_last_code;

    /* ---- Debugger state ---- */
    int in_debugger;
    /* Nesting depth of cl_invoke_debugger on this thread.  Bounded by
     * CL_DEBUGGER_MAX_DEPTH to stop a re-signalling debugger-hook / restart
     * from recursing the C stack into a SIGSEGV.  Snapshotted/restored by the
     * error-frame machinery (CL_ErrorFrame.saved_debugger_depth). */
    int debugger_depth;

    /* ---- Thread-Local Value (TLV) table ---- */
    uint32_t    tlv_entry_count;  /* number of active TLV entries — 0 = skip probes */

#ifdef CL_TLAB
    /* ---- Per-thread allocation buffer (TLAB) ----
     * Arena OFFSETS bounding this thread's private bump chunk; 0/0 = no
     * chunk.  Only meaningful while cl_thread_count > 1 (cl_alloc's fast
     * path); carved from the shared bump front / free list under
     * alloc_mutex, then cut without any locking by the owning thread.
     * The uncut remainder [tlab_cur, tlab_end) is kept formatted as a
     * CL_FreeBlock hole at ALL times so every linear arena walk (sweep,
     * forwarding, slide, conservative JIT scan) parses it.  Every GC
     * cycle clears all threads' TLABs during stop-the-world (mandatory:
     * gc_sweep rebuilds the free list and would otherwise hand an active
     * tail to another thread).  Owner-written; a peer touches these only
     * during STW (gc_tlab_reset_all) or after the thread finished
     * (cl_tlab_retire from unregister). */
    uint32_t tlab_cur;
    uint32_t tlab_end;
    uint32_t tlab_consed;   /* object bytes cut since the last flush to
                             * cl_heap.total_consed (refill/GC/retire) */
#endif /* CL_TLAB */

    /* ---- GC coordination ---- */
    volatile uint8_t gc_requested;
    volatile uint8_t gc_stopped;
    /* 0 = newborn: registered in cl_thread_list (so GC marks/forwards its
     * roots) but its OS thread has not yet reached the online barrier, so it
     * cannot cooperate with stop-the-world.  1 = live: participates in STW.
     * A stop-the-world initiator waits only for `gc_live` threads; a newborn
     * would never reach a safepoint and would hang the world.  The
     * newborn→live transition happens under gc_mutex in cl_gc_thread_online,
     * serialized against the STW request so a thread that comes online during
     * an in-progress STW parks instead of touching the stopped heap.
     * See cl_gc_thread_online / cl_gc_stop_the_world. */
    volatile uint8_t gc_live;
    /* Non-zero while the thread is in a blocking syscall that does not touch
     * the heap (e.g. pthread_join).  STW treats such a thread as already
     * stopped — it cannot reach a Lisp-level safepoint, but it also cannot
     * race the GC. */
    volatile uint8_t in_safe_region;
    /* Nesting depth for cl_gc_enter/leave_safe_region.  Owner-only.  Safe
     * regions nest (e.g. the platform layer brackets a blocking syscall
     * that a builtin already bracketed): only the OUTERMOST enter/leave
     * pair touches gc_mutex / in_safe_region — an inner leave clearing the
     * flag early would let this thread return to heap-touching code while
     * a peer's STW GC still counts it as stopped. */
    int safe_region_depth;

    /* ---- Thread interruption ---- */
    volatile uint8_t interrupt_pending;   /* 1 = check interrupt_func or destroy */
    volatile uint8_t destroy_requested;   /* 1 = abort at next safepoint */
    uint8_t destroyed;                    /* 1 = MP:DESTROY-THREAD is ending this thread
                                           * (own thread only): its death is wanted,
                                           * MP:*THREAD-DEATH-HOOKS* stay quiet */
    CL_Obj interrupt_func;               /* function for interrupt-thread */

    /* ---- Thread registry ---- */
    /* Set (under cl_thread_list_lock) by the JOIN-THREAD that claimed this
     * worker's platform handle.  While set, the zombie reaper and any
     * concurrent JOIN must not free the worker — the claimant owns the
     * cleanup; late joiners wait for the table slot to clear and read the
     * result from the GC-managed wrapper. */
    volatile uint8_t join_in_progress;
    struct CL_Thread_s *next;
    void *platform_handle;

    /* ---- rwlock leak diagnostics ----
     * Per-thread reader-hold count for cl_tables_rwlock and cl_package_rwlock.
     * Bumped/decremented by cl_tables_rdlock / cl_tables_rwunlock (and the
     * package equivalents) so a writer that gets stuck can identify which
     * thread leaked a reader lock, and bi_condition_wait can refuse to sleep
     * while holding one.  rdlock_tables_sites stores the call-site of each
     * outstanding rdlock so the diagnostic can name the leaking caller. */
#define CL_RDLOCK_SITES_MAX 32
    int         rdlock_tables_held;
    int         rdlock_tables_sites_top;
    int         rdlock_package_held;

    /* ---- Wait-state diagnostics (deadlock triage) ----
     * What this thread is currently blocked on.  Set just before a thread
     * parks in MP:CONDITION-WAIT / a blocking MP:ACQUIRE-LOCK / a stop-the-world
     * GC barrier and cleared on wake.  Read by another thread via
     * (MP:DUMP-THREAD-WAITS) to tell a lost-wakeup (a worker still parked on its
     * queue condvar after a notify) apart from a lock-ordering deadlock (a thread
     * blocked acquiring a held lock) or a stalled stop-the-world GC.  Single-writer
     * (the owning thread) so the racy cross-thread read is fine for diagnostics. */
    volatile int wait_kind;     /* 0=running,1=condwait,2=condwait/timeout,3=lock-acquire,4=GC-STW-wait */
    volatile int wait_straggler_tid;  /* GC-STW-wait: the thread holding up the world */
    /* The lock or condvar the thread is parked on lives in wait_obj /
     * wait_lock at the END of the struct (GC roots, see there). */

    /* ---- COMPILE-FILE per-thread state ---- */
    /* Set to 1 while COMPILE-FILE is active (thread-local so concurrent
     * compile-file calls on different threads don't race on this flag). */
    int compiling_to_file;
    /* Pending LOAD-TIME-VALUE (cell, thunk) pairs registered during lambda
     * compilation.  compile_defun reads these after compile_lambda returns
     * and emits inline init code into the TOP-LEVEL compiler so the init
     * runs at FASL load time within the same FASL unit.  FASL deduplication
     * then ensures the cell in the init code and in the function body are
     * the same deserialized object (CLHS 3.2.4.4). */
#define CL_LTV_INIT_MAX 32
    int    ltv_init_count;

    /* Top of the UNWIND-PROTECT value save stack (mv_save_buf below).  A
     * scalar every NLX push reads: it must stay in the scalar section — the
     * first build had it after the 8 KB buffer, which put it back into the
     * per-dispatch base-materialization the note below describes. */
    int32_t mv_save_top;

    /* ---- Large per-thread tables ----
     *
     * Every array bigger than a cache line lives HERE, after all the scalar
     * fields, and new ones go here too.  The reason is the code the C
     * compiler emits for cl_vm_run: it merges every opcode handler's
     * VM_DISPATCH into ONE shared indirect branch, and any per-thread field
     * whose offset from `thr` exceeds the immediate range of a load
     * (aarch64: 16KB for 4-byte fields, 32KB for 8-byte; 68020: 32KB) has
     * its base address materialized in that shared block — one stack reload
     * plus an add on EVERY dispatch, for every such field the handlers
     * touch.  With the 32KB dyn_stack sitting near the top of the struct,
     * everything after it (the NLX/handler/restart tops, mv_count, the
     * GC-root count, the printer state) paid that, and adding two more such
     * fields in the Tier-4 phase-2 NLX work cost a uniform ~0.5ns per
     * dispatch (+17% on an empty loop) until this reorder.  Same class of
     * sensitivity as the slot_ic note below and the vm.o/LTO note in
     * CLAUDE.md.  Element access into these tables is by index and does
     * not care where they sit. */
    CL_DynBinding     dyn_stack[CL_MAX_DYN_BINDINGS];
    CL_HandlerBinding handler_stack[CL_MAX_HANDLER_BINDINGS];
    CL_RestartBinding restart_stack[CL_MAX_RESTART_BINDINGS];
    char   callback_error_msg[512];
    char   pending_error_msg[512];
    CL_ErrorFrame error_frames[CL_MAX_ERROR_FRAMES];
    int           error_frame_overflowing; /* see cl_error_frame_push */
    char          error_msg[512];
    CL_Obj *gc_roots[CL_GC_ROOT_STACK_SIZE];
#ifdef DEBUG_GC
    const char *gc_root_files[CL_GC_ROOT_STACK_SIZE];
    int         gc_root_lines[CL_GC_ROOT_STACK_SIZE];
#endif
    int32_t pr_indent_stack[CL_PP_INDENT_MAX];
    int32_t pr_block_start[CL_PP_INDENT_MAX];
    CL_Obj  pr_circle_keys[CL_CIRCLE_HT_SIZE];
    int32_t pr_circle_vals[CL_CIRCLE_HT_SIZE];
    CL_Obj  pr_inprog[CL_PR_INPROG_MAX];
    /* backtrace_inline must stay immediately before c_stack_base — see the
     * backtrace_buf comment above and tests/test_backtrace.c. */
    char   backtrace_inline[CL_BACKTRACE_BUF_SIZE];
    char  *c_stack_base;
    struct {
        uint8_t  op;
        uint32_t ip;
        int      fp;
        int      sp;
        uint8_t *code;
    } vm_trace_buf[CL_VM_TRACE_SIZE];
    CL_Obj vm_extra_args_buf[256];
    CL_TLVEntry tlv_table[CL_TLV_TABLE_SIZE];
    const char *rdlock_tables_sites[CL_RDLOCK_SITES_MAX];
    CL_Obj ltv_init_cells[CL_LTV_INIT_MAX];
    CL_Obj ltv_init_thunks[CL_LTV_INIT_MAX];

    /* Slot-index cache for SLOT-VALUE and friends (see CL_SlotICEntry).
     * Holds CL_Obj keys that are NOT GC roots: the generation stamp makes
     * every entry unusable after a compaction, so a stale offset here is
     * never compared against a live one, let alone dereferenced.
     *
     * Deliberately LAST in the struct.  It is a kilobyte of cold-ish table,
     * and putting it anywhere earlier pushes the VM's own hot fields (the
     * stack pointers, mv state, the poll counter) onto different cache
     * lines — measured as a uniform ~3ns on every call row of
     * trunk/bench-prims.lisp, which is the layout sensitivity CLAUDE.md
     * warns about.  New per-thread tables belong here too. */
    CL_SlotICEntry slot_ic[CL_SLOT_IC_SIZE];

    /* UNWIND-PROTECT value save stack (vm.h CL_MV_SAVE_SIZE): records of
     * [values..., fixnum count] pushed by OP_MV_SAVE while a cleanup runs.
     * Every used word is a CL_Obj — marked and forwarded by the GC up to
     * mv_save_top (a scalar, so it lives with the other scalars above the
     * tables).  At the end for the same layout reason as slot_ic. */
    CL_Obj  mv_save_buf[CL_MV_SAVE_SIZE];

    /* ---- MP lock / condition-variable parking (specs/mp-locks-heap-words.md) ----
     * park:     this thread's platform park handle (token semantics), created
     *           by the thread itself at start-up and destroyed by it at exit —
     *           on AmigaOS it is a signal bit of the owning task.  Read by
     *           peers only under cl_thread_list_lock (release scan, notify,
     *           interrupt delivery); NULL means "cannot park" (degrade to a
     *           sleep poll).
     * wait_obj: the lock (wait_kind 3) or condvar (wait_kind 1/2) this thread
     *           is registered as waiting on, NIL otherwise.  Written under
     *           cl_thread_list_lock; a releaser / notifier scans the thread
     *           list for it.  GC ROOT: marked and forwarded with the thread
     *           metadata, so a parked thread keeps its lock alive and the
     *           record holds the forwarded object after a compaction.
     * wait_lock: the lock a condition-wait will re-acquire (diagnostics; GC
     *           root for the same reason).
     * serial:   process-unique thread number (never reused, unlike the table
     *           slot `id`), the owner identity baked into CL_Lock.state.  A
     *           lock left held by an exited thread therefore can never be
     *           mistaken for one owned by a later thread that got its slot.
     * Cold fields, so at the end (the layout note above). */
    void    *park;
    CL_Obj   wait_obj;
    CL_Obj   wait_lock;
    uint32_t serial;
} CL_Thread;

/* Current thread pointer — TLS-backed.
 * platform_tls_get() returns the CL_Thread* for the calling OS thread.
 * A fast-path global is kept for single-threaded hot paths (Phase 0 compat). */
extern CL_Thread *cl_main_thread_ptr;   /* fast access to main thread */
extern uint32_t    cl_thread_count;      /* number of registered threads */

/* Inline fast path: when single-threaded (the common case), return the
 * cached main thread pointer directly, avoiding the TLS lookup. */
static inline CL_Thread *cl_get_current_thread(void)
{
    if (cl_thread_count <= 1)
        return cl_main_thread_ptr;
    return (CL_Thread *)platform_tls_get();
}

#define cl_current_thread cl_get_current_thread()
#define CT cl_current_thread

/* Is the calling OS thread / task a registered Lisp thread?  The TLS slot
 * of a foreign task (input.device calling a gadget method, a non-Lisp
 * pthread) is NULL or junk — this compares it against cl_thread_list by
 * identity and never dereferences it.  Used by the FFI callback entry
 * before it touches any Lisp state (thread.c). */
int cl_thread_current_is_registered(void);

/* Initialize/shutdown thread system */
void cl_thread_init(void);
void cl_thread_shutdown(void);

/* Ask every other registered thread to unwind and exit (the DESTROY-THREAD
 * path: UNWIND-PROTECT cleanups run) and wait up to TIMEOUT_MS for them to
 * leave the registry.  Returns how many are still registered.  Process exit
 * on AmigaOS calls it before tearing anything down: nothing ends a worker
 * task there, and one still running when main returns executes unloaded
 * code.  A worker that never reaches a safepoint (stuck in foreign code)
 * is not stopped by this. */
uint32_t cl_thread_stop_workers(uint32_t timeout_ms);
/* Restore the main task's TLS slot (tc_UserData) to its pre-init value.
 * Call once just before the process exits — see cl_thread_init. */
void cl_thread_restore_main_tls(void);

/* ---- Thread creation API ---- */

/* Worker thread VM/NLX/C-stack sizes.
 *
 * HOST: match the main thread's budgets.  Worker threads used to get a 4x-16x
 * smaller VM stack / call-frame stack / NLX stack / OS stack than the main
 * thread, which was a silent-death bug: a worker overflowed its VM frame stack
 * at ~256 nested calls where the main thread tolerates ~1024, and the overflow
 * is UNRECOVERABLE (running the Lisp handler that would report it itself needs
 * frames, and there are none), so the worker just dies -- status 3, no debugger,
 * no result.  That is what made (asdf:load-system ...) hang under Sly: slynk
 * evaluates on a channel worker thread, and a big load's ASDF + CLOS-dispatch +
 * macroexpansion call chains exceed 256 frames but fit under 1024, so it loads
 * fine headless (main thread) yet silently kills the slynk worker -- Emacs never
 * gets a :return (REPL hangs forever) and a later request answers "Thread not
 * found".  See tests/test_threads.c:worker_deep_recursion_survives_like_main.
 *
 * AMIGA: keep the historical compact sizes -- byte-for-byte what shipped before
 * this fix.  Two reasons: (1) RAM is scarce on the 8MB target, and (2) the
 * decisive one -- bumping these sizes shifts heap/binary layout enough to tip a
 * PRE-EXISTING, layout-fragile moving-GC bug into firing (a Guru partway through
 * the Amiga test suite; the parent commit passes the whole suite).  The bug is
 * NOT in this code: the crash lands on the main thread before any worker exists,
 * and moves to a different, unrelated test for a different layout (the
 * heisenbug signature).  Until that GC bug is fixed separately, the worker-size
 * fix stays host-only -- which is where it is needed: Sly connects to the host
 * binary, so the hang is a host problem, and Amiga worker code keeps the
 * pre-existing 256-frame budget it has always run with. */
#if defined(PLATFORM_AMIGA)
#define CL_WORKER_VM_STACK_SIZE  4096   /* 4K entries = 16KB (pre-fix value) */
#define CL_WORKER_VM_FRAME_SIZE  256    /* 256 frames        (pre-fix value) */
#define CL_WORKER_NLX_FRAMES     256    /* 256 NLX frames    (pre-fix value) */
#define CL_WORKER_SAVED_PENDING   64    /* 64 saved-pending  (pre-fix value) */
#ifdef PLATFORM_MORPHOS
/* MorphOS: 64K is a 68k-era stack budget.  PPC frames are several times
 * larger (register-save areas, minimum-frame overhead), the MAIN thread
 * already needs a 1MB __stack on MorphOS, and worker C-stack overflow is
 * UNGUARDED outside the reader/compiler recursion guards -- it corrupts
 * memory or kills the task (a Frozen CL-Thread under the Grim Reaper)
 * instead of signalling.  RAM is plentiful on MorphOS machines, so give
 * workers a real stack.  CL_PROC_STACK_TAGS sizes both the 68k and PPC
 * stacks from this one value. */
#define CL_WORKER_C_STACK_SIZE   (256u * 1024u)
#else
#define CL_WORKER_C_STACK_SIZE     0    /* OS default (~64KB, pre-fix value) */
#endif
#else
#define CL_WORKER_VM_STACK_SIZE  CL_VM_STACK_SIZE      /* 16K entries = 64KB */
#define CL_WORKER_VM_FRAME_SIZE  CL_VM_FRAME_SIZE      /* 1024 frames */
#define CL_WORKER_NLX_FRAMES     CL_MAX_NLX_FRAMES     /* 2048 NLX frames */
#define CL_WORKER_SAVED_PENDING  CL_MAX_SAVED_PENDING  /* 256 saved-pending */
#define CL_WORKER_C_STACK_SIZE   (8u * 1024u * 1024u)  /* match main (8MB) */
#endif

/* Thread side table: maps thread_id -> CL_Thread* */
#define CL_MAX_THREADS 256
extern CL_Thread *cl_thread_table[CL_MAX_THREADS];
/* Per-slot generation counter, bumped every time a slot is (re)claimed by
 * cl_thread_table_alloc.  A CL_ThreadObj wrapper snapshots the value at
 * creation (table_gen); a mismatch later means the slot was reused for an
 * unrelated worker and the wrapper's thread has already exited.  Read/compared
 * under cl_thread_list_lock (or during STW GC, when no peer can run). */
extern uint32_t cl_thread_table_gen[CL_MAX_THREADS];

/* MP locks and condition variables are plain heap objects (CL_Lock /
 * CL_CondVar in types.h) with no OS primitive and no side table behind
 * them; blocking goes through each thread's own park handle.  The only
 * process-wide state is the thread serial counter below. */

/* Next process-unique thread serial (1 = the main thread).  Assigned once
 * per CL_Thread and never reused; the value that identifies a lock's owner
 * in CL_Lock.state. */
uint32_t cl_thread_next_serial(void);

/* Reset every Lisp-heap reference gc_visit_thread_roots walks (stale after
 * a heap re-initialization — see cl_mem_init).  Keep in sync with
 * gc_visit_thread_roots in gc_trace.c. */
void cl_thread_reset_lisp_state(CL_Thread *t);

/* Allocate and initialize a new CL_Thread for a worker */
CL_Thread *cl_thread_alloc_worker(void);

/* Sized variant backing MP:MAKE-THREAD's :vm-stack-size/:vm-frames/:nlx-frames
 * keywords.  Each size is a MINIMUM: 0 or anything below the platform's
 * CL_WORKER_* default is raised to that default, so a worker can only be
 * grown, never shrunk below the historically safe floors.  The enlarged
 * arrays come from platform_alloc (malloc/AllocVec), NOT the GC arena, so an
 * opt-in size changes nothing about compile-time heap layout — the Amiga
 * defaults stay byte-for-byte historical (see the CL_WORKER_* rationale). */
CL_Thread *cl_thread_alloc_worker_sized(uint32_t vm_stack_size,
                                        uint32_t vm_frames,
                                        uint32_t nlx_frames);

/* Free a worker CL_Thread's resources */
void cl_thread_free_worker(CL_Thread *t);

/* Free every worker that finished (status >= 2) and that no JOIN-THREAD
 * has claimed: its table slot is cleared, its OS handle detached, its
 * stacks handed back.  The wrapper object stays (EQ identity, the name);
 * its finalizer's table_gen compare keeps it from freeing a later
 * occupant of the slot.  Called when MAKE-THREAD finds the table full,
 * and by cl_thread_shutdown, because a thread the program only polled
 * with THREAD-ALIVE-P is otherwise freed by nothing before exit -- on
 * AmigaOS its stacks would be lost until reboot. */
void cl_thread_reap_zombies(void);

/* Point T's backtrace buffer at its inline block.  Must run for every
 * CL_Thread before anything reads cl_backtrace_buf. */
void cl_thread_backtrace_init(CL_Thread *t);

/* Drop any heap block bt_reserve grew for T and fall back to the inline
 * block, leaving the buffer valid (and empty) rather than dangling — the
 * crash handler still prints it after cl_thread_shutdown. */
void cl_thread_backtrace_release(CL_Thread *t);

/* Allocate a side table slot for a thread, returns id or -1 */
int cl_thread_table_alloc(CL_Thread *t);
void cl_thread_table_free(int id);

/* Allocate a fresh, free CL_Lock heap object.  recursive != 0 makes it
 * re-entrant for its owner.  Errors out via cl_error() on failure;
 * err_prefix tags the message for the caller (e.g. "MP:MAKE-LOCK", "FASL"). */
CL_Obj cl_lock_alloc_obj(int recursive, CL_Obj name, const char *err_prefix);

/* ---- Thread registry ---- */
extern CL_Thread  *cl_thread_list;      /* linked list of all threads */
extern void       *cl_thread_list_lock; /* mutex protecting the list */
extern uint32_t    cl_thread_count;     /* number of registered threads */

void cl_thread_register(CL_Thread *t);
/* Register a make-thread child whose OS thread does not exist yet: a newborn
 * skipped by the STW wait loop until it reaches cl_gc_thread_online. */
void cl_thread_register_newborn(CL_Thread *t);
void cl_thread_unregister(CL_Thread *t);

/* ---- GC coordination ---- */

/* ---- Multi-thread check ---- */
/* True when more than one thread is registered — used to skip locking
 * in the common single-threaded case for zero overhead on 68020. */
#define CL_MT() (cl_thread_count > 1)

/* Capture the frame address of the calling function.
 * On every stack-grows-down ABI we target (m68k SysV, host x86-64),
 * the actual SP at the time of the call is at a lower address than
 * this value, so it serves as a valid upper bound for the JIT
 * native-stack scan window.  Implemented as a macro so the captured
 * address is in the CALLER's frame, not a helper's.
 *
 * __builtin_frame_address(0) is supported by gcc on m68k and host. */
#define CL_CAPTURE_SP() ((void *)__builtin_frame_address(0))

/* Safepoint check — insert at function calls and backward jumps.
 * The volatile reads are cheap; the slow paths handle GC and interrupts.
 * GC is handled first (critical), then pending interrupts. */
#define CL_SAFEPOINT() \
    do { \
        CL_Thread *_sp = cl_get_current_thread(); \
        if (_sp->gc_requested) cl_gc_safepoint(); \
        if (_sp->interrupt_pending) cl_thread_handle_interrupt(_sp); \
    } while (0)

void cl_gc_safepoint(void);           /* slow path: stop until GC completes */
void cl_thread_handle_interrupt(CL_Thread *t);  /* slow path: handle pending interrupt */

/* Hang-triage dump (builtins_thread.c): per-thread wait states to stderr.
 * The C entry point behind MP:DUMP-THREAD-WAITS; also called by the
 * CLAMIGA_STW_DIAG straggler watchdog in cl_gc_stop_the_world. */
void cl_dump_thread_waits(void);

/* Bring a freshly-created worker online: the first thing its OS thread must do
 * (before touching the heap) is transition from newborn to `gc_live` under
 * gc_mutex.  If a stop-the-world GC is in progress and already requested this
 * thread, the barrier parks here until it completes instead of racing the
 * stopped/compacting heap.  This closes the newborn-vs-STW hang: the parent
 * registers the child before creating its OS thread (so GC roots are marked),
 * but until the child reaches this barrier it is invisible to the STW wait
 * loop, which only waits for `gc_live` threads. */
void cl_gc_thread_online(CL_Thread *self);

/* Mark the current thread as entering / leaving a blocking syscall that does
 * not touch the heap.  Bracket calls like platform_thread_join with
 * cl_gc_enter_safe_region() / cl_gc_leave_safe_region() so STW does not
 * deadlock waiting for a thread that cannot reach a safepoint. */
void cl_gc_enter_safe_region(void);
void cl_gc_leave_safe_region(void);

/* Acquire an internal C mutex that may be held across a GC safepoint or a
 * blocking-syscall safe region without stalling a concurrent stop-the-world GC.
 * A plain platform_mutex_lock() on such a contended lock is neither a safepoint
 * nor a safe region, so a peer running STW waits for the blocked thread forever
 * while the current holder parks in leave_safe_region still holding the lock.
 * Fast (uncontended) path is a single trylock; pair with platform_mutex_unlock. */
void cl_gc_safe_mutex_lock(void *mutex);

/* ---- TLV functions ---- */

/* TLV table operations */
CL_Obj cl_tlv_get(CL_Thread *t, CL_Obj sym);
/* C-level dynamic bind (thread-local, like OP_DYNBIND); pair with
 * cl_dynbind_restore_to(mark) from vm.h. */
void   cl_dynbind_c(CL_Obj sym, CL_Obj val);
void   cl_tlv_set(CL_Thread *t, CL_Obj sym, CL_Obj val);
void   cl_tlv_remove(CL_Thread *t, CL_Obj sym);
/* Rebuild the TLV table after a compacting GC relocated symbols (the table is
 * keyed by symbol arena-offset, which changes when the object moves). */
void   cl_tlv_rehash(CL_Thread *t);

/* High-level TLV-aware accessors */
CL_Obj cl_symbol_value(CL_Obj sym);
void   cl_set_symbol_value(CL_Obj sym, CL_Obj val);
int    cl_symbol_boundp(CL_Obj sym);

/* Symbol-value read on a thread the caller already holds.
 *
 * cl_symbol_value re-derives the thread on every call (a TLS lookup once
 * cl_thread_count > 1), which the VM does not need: cl_vm_run caches the
 * CL_Thread * for the whole dispatch loop.  OP_GLOAD is 5% of all opcodes
 * in the sento profile — 72 per message — so the lookup is pure overhead
 * there.  With no dynamic binding active on this thread the TLV probe is
 * skipped too and the read is one arena load.
 *
 * Declared here (not in a .c) so the VM's GLOAD/GSTORE handlers inline it.
 * SYM_NIL: CL_NIL is the tag 0, whose value cell lives on the heap-allocated
 * NIL storage shadow — the same fixup cl_symbol_value does. */
extern CL_Obj SYM_NIL;   /* symbol.h; declared here to keep this inline */

static inline CL_Obj cl_symbol_value_on(CL_Thread *t, CL_Obj sym)
{
    if (CL_NULL_P(sym)) sym = SYM_NIL;
    if (t->tlv_entry_count > 0) {
        CL_Obj v = cl_tlv_get(t, sym);
        if (v != CL_TLV_ABSENT) return v;
    }
    return ((CL_Symbol *)CL_OBJ_TO_PTR(sym))->value;
}

/* Compare-and-swap primitives behind MP:COMPARE-AND-SWAP.  Both return the
 * value the cell held when the CAS was decided (== OLD iff it swapped).
 * cl_symbol_value_cas returns CL_UNBOUND for an unbound target binding. */
CL_Obj cl_cas_cell(volatile CL_Obj *cell, CL_Obj old, CL_Obj new_val);
CL_Obj cl_symbol_value_cas(CL_Obj sym, CL_Obj old, CL_Obj new_val);

/* ================================================================
 * Compatibility macros
 *
 * Map legacy global names to CL_Thread fields so that existing code
 * compiles unchanged.  Guarded by CL_THREAD_NO_MACROS so that
 * thread.c (which accesses the struct directly) can suppress them.
 * ================================================================ */

#ifndef CL_THREAD_NO_MACROS

/* VM */
#define cl_vm               (CT->vm)

/* Dynamic bindings */
#define cl_dyn_stack        (CT->dyn_stack)
#define cl_dyn_top          (CT->dyn_top)

/* NLX stack */
#define cl_nlx_stack        (CT->nlx_stack)
#define cl_nlx_top          (CT->nlx_top)
/* Per-thread NLX capacity: the number of CL_NLXFrame slots actually allocated
 * for THIS thread's nlx_stack (CL_MAX_NLX_FRAMES for the main thread,
 * CL_WORKER_NLX_FRAMES for workers — which is smaller on AmigaOS).  Every NLX
 * push guard and diagnostic loop MUST bound against this, NOT the global
 * CL_MAX_NLX_FRAMES constant: a worker with a smaller allocation would
 * otherwise be allowed to write past the end of its nlx_stack (heap
 * corruption).  This must hold on every platform, including AmigaOS — a
 * memory-safety guard is not allowed to be loosened to dodge an unrelated,
 * pre-existing layout-fragile moving-GC bug; that bug needs a real fix (see
 * memory note sly_load_hang_worker_vm_frame_budget), not a wider NLX bound. */
#define cl_nlx_max          (CT->nlx_max)

/* Saved pending-throw stack */
#define cl_saved_pending_stack  (CT->saved_pending_stack)
#define cl_saved_pending_top    (CT->saved_pending_top)
#define cl_saved_pending_max    (CT->saved_pending_max)

/* Handler stack */
#define cl_handler_stack    (CT->handler_stack)
#define cl_handler_top      (CT->handler_top)
#define cl_handler_floor    (CT->handler_floor)
#define cl_handler_active_mask (CT->handler_active_mask)

/* Restart stack */
#define cl_restart_stack    (CT->restart_stack)
#define cl_restart_top      (CT->restart_top)
#define cl_restart_floor    (CT->restart_floor)

/* Foreign-callback boundary: the lowest NLX frame a THROW / RETURN-FROM /
 * GO may target (0 outside any callback) */
#define cl_nlx_floor        (CT->nlx_floor)

/* Pending throw */
#define cl_pending_throw      (CT->pending_throw)
#define cl_pending_tag        (CT->pending_tag)
#define cl_pending_value      (CT->pending_value)
#define cl_pending_error_code (CT->pending_error_code)
#define cl_pending_error_msg  (CT->pending_error_msg)
#define cl_pending_mv_count   (CT->pending_mv_count)
#define cl_pending_mv_values  (CT->pending_mv_values)

/* Error frames */
#define cl_error_frames     (CT->error_frames)
#define cl_error_frame_top  (CT->error_frame_top)
#define cl_error_code       (CT->error_code)
#define cl_error_msg        (CT->error_msg)
#define cl_exit_code        (CT->exit_code)

/* Multiple values */
#define cl_mv_values           (CT->mv_values)
#define cl_mv_count            (CT->mv_count)
#define cl_pre_call_mv_count   (CT->pre_call_mv_count)
#define cl_pre_call_mv_values  (CT->pre_call_mv_values)

/* GC roots */
#define gc_root_count       (CT->gc_root_count)

/* Trace & debug */
#define cl_trace_depth      (CT->trace_depth)
#define cl_trace_count      (CT->trace_count)
#define cl_backtrace_buf    (CT->backtrace_buf)
#define cl_backtrace_cap    (CT->backtrace_cap)
#define cl_c_stack_base     (CT->c_stack_base)
#define cl_debug_base_fp    (CT->debug_base_fp)

/* Compiler chain */
#define cl_active_compiler  (CT->active_compiler)
#define pending_lambda_name (CT->pending_lambda_name)

/* Macroexpansion lexical environment (current &environment value) */
#define cl_current_lex_env  (CT->current_lex_env)

/* Source location tracking */
#define cl_current_source_file (CT->current_source_file)
#define cl_current_file_id     (CT->current_file_id)

/* Debugger */
#define cl_in_debugger      (CT->in_debugger)
#define cl_debugger_depth   (CT->debugger_depth)

/* COMPILE-FILE state */
#define cl_compiling_to_file (CT->compiling_to_file)
#define cl_ltv_init_count    (CT->ltv_init_count)

/* VM debug (used under #ifdef DEBUG_VM, but always present in struct) */
#define vm_eval_depth       (CT->vm_eval_depth_val)
#define c_stack_max_seen    (CT->c_stack_max_val)

/* Crash diagnostics */
#define dbg_last_op         (CT->dbg_last_op)
#define dbg_last_ip         (CT->dbg_last_ip)
#define dbg_last_fp         (CT->dbg_last_fp)
#define dbg_last_code       (CT->dbg_last_code)

#endif /* CL_THREAD_NO_MACROS */

#endif /* CL_THREAD_H */
