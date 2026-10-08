/* gc_trace.c — the mark phase: mark stack, gc_mark_obj and the
 * per-type children walk (GC_WALK_OBJ_CHILDREN, gc_internal.h),
 * root enumeration in value form, the overflow re-scan, and the
 * dead-object finalizers (off-heap bytecode payload, streams, ...).
 * Shared by every collector.  State: gc_internal.h.
 */
#include "gc_internal.h"

/* GC mark stack (iterative marking).
 *
 * Starts on a small static buffer (BSS — always available, cannot fail) and
 * GROWS geometrically on demand up to a heap-proportional cap; see
 * gc_mark_stack_grow.  Growth matters: an object with more unmarked children
 * than the stack has free slots (a >4K-element vector, a large hash table)
 * overflows it, and the overflow fallback below is QUADRATIC — each full-arena
 * re-scan pass recovers at most ~one-stack-full of dropped children, so a
 * large live heap needs live/capacity passes, each pass re-visiting every
 * marked object's children.  Measured on a 210MB live heap: 49 s per GC with
 * a fixed 4096-entry stack vs 35 ms with a large-enough stack.  The re-scan
 * loop in gc_mark is kept as the correctness fallback for when growth fails
 * (platform_alloc OOM or cap reached) — it is slow but never wrong.
 * Growth happens with the world stopped (mark runs under STW), so no other
 * thread can observe the buffer swap. */
static CL_Obj   gc_mark_stack_initial[CL_GC_MARK_STACK_SIZE];
CL_Obj  *gc_mark_stack = gc_mark_stack_initial;
uint32_t gc_mark_stack_cap = CL_GC_MARK_STACK_SIZE;
uint32_t gc_mark_top = 0;
int gc_mark_overflow = 0;    /* Set when mark stack overflows */
int gc_mark_grow_failed = 0; /* Per-cycle latch: stop re-attempting a
                                     * failed grow on every subsequent push
                                     * this cycle.  Reset by gc_mark. */
/* Monotonic diagnostics (exposed via ext:%gc-mark-stats and cl_mem_stats):
 * grows = successful capacity doublings; rescan passes = full-arena overflow
 * recovery passes — nonzero means the quadratic fallback ran (a growth
 * failure), which is worth knowing about long before it becomes a 49s GC. */
uint32_t gc_mark_stack_grows = 0;
uint32_t gc_mark_rescan_passes = 0;

/* Off-heap bytecode payload handed back to the allocator since heap init:
 * total bytes and the number of bytecode objects finalized.  Exposed as
 * (ext:%bytecode-offheap-stats).  A CL_Bytecode owns up to eight
 * platform_alloc'd buffers that live outside the arena, so "how much of that
 * has actually been returned" is not visible in any arena statistic — on
 * AmigaOS, where an unreturned block is lost until reboot, it is the number
 * that decides whether a long REPL session or a repeated launch bleeds Fast
 * RAM.  Monotonic; wraps at 4GB (diagnostic only).  Reset by cl_mem_init. */
uint32_t gc_bc_offheap_bytes = 0;
uint32_t gc_bc_offheap_count = 0;
/* Test hook: when nonzero, caps growth at this many entries so the overflow
 * re-scan fallback can be exercised deterministically (see
 * tests/test_gc_markstack.c).  0 = normal heap-proportional cap. */
uint32_t gc_mark_stack_test_limit = 0;

/* --- Mark Phase --- */

#if defined(DEBUG_GC) || defined(DEBUG_GC_STRESS)
/* Highest valid type tag — CL_TYPE_MAX is maintained next to enum CL_ObjType
 * in types.h.  A local copy here drifted once (TYPE_BYTE_VECTOR was added
 * after TYPE_RESTART without bumping it), making the plausibility guards
 * below abort on every byte-vector mark in non-wide stress builds; the
 * compile-time check keeps at least that known-latest tag covered. */
#define GC_DBG_MAX_TYPE CL_TYPE_MAX
typedef char gc_dbg_max_type_covers_ffi_stub[
    (TYPE_FFI_STUB <= GC_DBG_MAX_TYPE) ? 1 : -1];
/* Object whose children are currently being pushed — provenance for the
 * gc_mark_push plausibility guard.  NULL while pushing from the root set. */
static void *gc_dbg_mark_parent;
/* Which root category / index the mark walk is currently processing —
 * provenance for badmarks that come from the root set itself. */
static const char *gc_dbg_root_src = "?";
static int gc_dbg_root_idx = -1;
#define GC_DBG_SRC(tag, idx) \
    (gc_dbg_mark_parent = NULL, gc_dbg_root_src = (tag), gc_dbg_root_idx = (idx))
#else
#define GC_DBG_SRC(tag, idx) ((void)0)
#endif

/* Double the mark stack's capacity (up to a heap-proportional cap).  Runs
 * with the world stopped and the stack contents are plain arena offsets, so
 * an alloc+copy+free swap is safe at any push.  Returns 1 on success, 0 when
 * growth is not possible (cap reached or platform_alloc OOM) — the caller
 * then falls back to the overflow re-scan protocol.  A failure latches for
 * the rest of the cycle so a heap under memory pressure doesn't re-attempt
 * the same failing allocation on every subsequent push. */
static int gc_mark_stack_grow(void)
{
    uint32_t max_cap, new_cap;
    CL_Obj *new_buf;

    if (gc_mark_grow_failed)
        return 0;

    /* Cap the stack's C-heap footprint at ~1/32 of the arena size (entries
     * are 4 bytes, so arena_size/128 entries).  Worst-case marking frontier
     * is bounded by the live object count, but that bound is far larger than
     * any real workload's frontier — the cap exists so a pathological graph
     * degrades to the (correct) re-scan fallback instead of doubling RAM
     * usage.  On the 68020/8MB target a 4MB heap caps at 32K entries (128KB),
     * covering e.g. a full 32K-element vector of fresh children. */
    max_cap = cl_heap.arena_size / 128u;
    if (max_cap < CL_GC_MARK_STACK_SIZE)
        max_cap = CL_GC_MARK_STACK_SIZE;
    if (gc_mark_stack_test_limit && max_cap > gc_mark_stack_test_limit)
        max_cap = gc_mark_stack_test_limit;

    if (gc_mark_stack_cap >= max_cap) {
        gc_mark_grow_failed = 1;
        return 0;
    }
    new_cap = gc_mark_stack_cap * 2u;
    if (new_cap > max_cap)
        new_cap = max_cap;

    new_buf = (CL_Obj *)platform_alloc((unsigned long)new_cap * sizeof(CL_Obj));
    if (!new_buf) {
        gc_mark_grow_failed = 1;
        return 0;
    }
    memcpy(new_buf, gc_mark_stack, (size_t)gc_mark_top * sizeof(CL_Obj));
    if (gc_mark_stack != gc_mark_stack_initial)
        platform_free(gc_mark_stack);
    gc_mark_stack = new_buf;
    gc_mark_stack_cap = new_cap;
    gc_mark_stack_grows++;
    return 1;
}

/* Release a grown mark stack back to the static initial buffer.  Called on
 * heap re-init/shutdown; NOT called between GC cycles — a workload that grew
 * the stack once will grow it again, so keeping the buffer (bounded by the
 * cap above) avoids per-GC alloc churn. */
void gc_mark_stack_release(void)
{
    if (gc_mark_stack != gc_mark_stack_initial) {
        platform_free(gc_mark_stack);
        gc_mark_stack = gc_mark_stack_initial;
    }
    gc_mark_stack_cap = CL_GC_MARK_STACK_SIZE;
    gc_mark_top = 0;
}

static void gc_mark_push(CL_Obj obj)
{
    /* Skip immediates and out-of-bounds */
    if (CL_NULL_P(obj) || CL_FIXNUM_P(obj) || CL_CHAR_P(obj))
        return;
    if (obj >= cl_heap.arena_size)
        return;

    /* Skip already-marked objects — avoids duplicate pushes and makes
     * overflow re-scan efficient (only pushes truly unmarked children) */
    if (CL_HDR_MARKED(CL_OBJ_TO_PTR(obj)))
        return;

#if defined(DEBUG_GC) || defined(DEBUG_GC_STRESS)
    /* Plausibility guard with provenance — see gc_mark_obj's twin.  Checking
     * at PUSH time (not pop) means gc_dbg_mark_parent still names the object
     * whose slot held the bad reference. */
    {
        void *cptr = CL_OBJ_TO_PTR(obj);
        if ((obj & (CL_ALIGN - 1)) != 0 || CL_HDR_SIZE(cptr) == 0 ||
            obj + CL_HDR_SIZE(cptr) > cl_heap.bump ||
            CL_HDR_TYPE(cptr) > GC_DBG_MAX_TYPE) {
            fprintf(stderr,
                    "[GC-BADMARK] gc_mark_push(0x%08x): implausible object "
                    "start (type=%u size=%u bump=0x%08x)\n",
                    (unsigned)obj, (unsigned)CL_HDR_TYPE(cptr),
                    (unsigned)CL_HDR_SIZE(cptr), (unsigned)cl_heap.bump);
            if (gc_dbg_mark_parent) {
                fprintf(stderr,
                        "  parent @0x%08x type=%u size=%u\n",
                        (unsigned)((uint8_t *)gc_dbg_mark_parent - cl_heap.arena),
                        (unsigned)CL_HDR_TYPE(gc_dbg_mark_parent),
                        (unsigned)CL_HDR_SIZE(gc_dbg_mark_parent));
            } else {
                fprintf(stderr, "  parent: (root set - no parent object)\n");
            }
            cl_capture_backtrace();
            fprintf(stderr, "%s", cl_backtrace_buf);
            fflush(stderr);
            abort();
        }
    }
#endif

    if (gc_mark_top >= gc_mark_stack_cap && !gc_mark_stack_grow()) {
        /* Growth impossible (cap/OOM): drop the child and let gc_mark's
         * full-arena re-scan loop recover it.  Slow (quadratic) but correct.
         * Warn once per process — a silent fallback here is how a 50s GC
         * pause hides from every normal build. */
        static int overflow_warned = 0;
        if (!overflow_warned) {
            overflow_warned = 1;
            platform_write_string("GC: mark stack cannot grow "
                                  "(OOM or cap); falling back to heap "
                                  "re-scan - expect slow GC cycles\n");
        }
        gc_mark_overflow = 1;
        return;
    }
    gc_mark_stack[gc_mark_top++] = obj;
}

void gc_mark_children(void *ptr, uint8_t type)
{
#if defined(DEBUG_GC) || defined(DEBUG_GC_STRESS)
    gc_dbg_mark_parent = ptr;
#endif
#define GC_VISIT(slot) gc_mark_push(slot)
#define GC_BYTECODE_TAIL(bc) ((void)(bc))
    /* Pin this live output stream's outbuf slot so the post-mark reclaim
     * (cl_stream_outbuf_gc_reclaim) doesn't free a buffer still in use. */
#define GC_STREAM_TAIL(st)                                                    \
    do {                                                                      \
        if (((st)->direction & CL_STREAM_OUTPUT) && (st)->out_buf_handle != 0)\
            cl_stream_outbuf_gc_mark_use((st)->out_buf_handle);               \
    } while (0)
    GC_WALK_OBJ_CHILDREN(ptr, type);
#undef GC_VISIT
#undef GC_BYTECODE_TAIL
#undef GC_STREAM_TAIL
}

void gc_mark_obj(CL_Obj obj)
{
    void *ptr;
    if (CL_NULL_P(obj) || CL_FIXNUM_P(obj) || CL_CHAR_P(obj))
        return;

    /* Check if offset is within arena bounds */
    if (obj >= cl_heap.arena_size)
        return;

    ptr = CL_OBJ_TO_PTR(obj);

    if (CL_HDR_MARKED(ptr)) return;
#if defined(DEBUG_GC) || defined(DEBUG_GC_STRESS)
    /* Plausibility guard: marking an offset that is not a real object start
     * ORs CL_HDR_MARK_BIT into arbitrary object interiors (observed: a
     * hashtable's flags word and bucket slot), which the compactor then
     * treats as live headers — silent corruption far from the culprit.
     * A stale/interior/double-forwarded reference is the only way to get
     * here; abort loudly with the offending offset so the referrer can be
     * hunted while it is still on the stack. */
    if ((obj & (CL_ALIGN - 1)) != 0 || CL_HDR_SIZE(ptr) == 0 ||
        obj + CL_HDR_SIZE(ptr) > cl_heap.bump ||
        CL_HDR_TYPE(ptr) > GC_DBG_MAX_TYPE) {
        fprintf(stderr,
                "[GC-BADMARK] gc_mark_obj(0x%08x): implausible object start "
                "(type=%u size=%u bump=0x%08x) - interior or stale offset\n"
                "  source: %s[%d]%s\n",
                (unsigned)obj, (unsigned)CL_HDR_TYPE(ptr),
                (unsigned)CL_HDR_SIZE(ptr), (unsigned)cl_heap.bump,
                gc_dbg_root_src, gc_dbg_root_idx,
                gc_dbg_mark_parent ? " (via object children)" : "");
#ifdef DEBUG_GC
        if (gc_dbg_root_idx >= 0 && strcmp(gc_dbg_root_src, "gc_roots") == 0 &&
            CT->gc_root_files[gc_dbg_root_idx])
            fprintf(stderr, "  root pushed at %s:%d\n",
                    CT->gc_root_files[gc_dbg_root_idx],
                    CT->gc_root_lines[gc_dbg_root_idx]);
#endif
        cl_capture_backtrace();
        fprintf(stderr, "%s", cl_backtrace_buf);
        fflush(stderr);
        abort();
    }
#endif
    CL_HDR_SET_MARK(ptr);
#ifdef CL_GENGC
    /* Minor cycles: record every live nursery object as it is discovered —
     * the survivor list is what lets the rest of the minor never touch
     * dead nursery space.  gc_mark_obj's marked-check above guarantees
     * exactly one note per object. */
    if (gen_minor_active && obj >= gen_old_top)
        gen_live_note(obj);
#endif
    gc_mark_children(ptr, CL_HDR_TYPE(ptr));
}

/* --- Root enumeration, slot-visitor form ---------------------------------
 *
 * The root set is enumerated ONCE, here, as the set of CL_Obj *slots* that
 * live outside the arena, and every pass that needs the roots instantiates
 * the walk with its own visitor: the mark phase with "mark the value"
 * (gc_mark_slot below), the compactor with "forward the slot in place"
 * (gc_update_slot, gc_compact.c), the registered-root dedup with an
 * address probe.  Mark and forward can therefore never disagree on what a
 * root is: a slot the walk skips is neither kept alive nor rewritten, and
 * a slot it visits is both.  The three parts:
 *
 *   gc_visit_thread_roots(t)    everything hanging off one CL_Thread except
 *                               its gc_roots[] stack
 *   gc_visit_registered_roots   global_roots[] + every thread's gc_roots[]
 *                               (the CL_GC_PROTECT / cl_gc_register_root
 *                               slots), plain order, no dedup — the
 *                               compactor has its own instantiation that
 *                               forwards each distinct address once
 *   gc_visit_shared_roots       the process-wide tables and registries
 *
 * Deliberately NOT visited (stale words that are not object starts, so
 * marking them would set mark bits inside objects and forwarding them
 * would rewrite garbage into garbage):
 *   nlx_stack[i].mv_values      written together with mv_count only in the
 *                               zero-allocation window between a throw's
 *                               stash and its longjmp landing; outside it
 *                               mv_values[0] is whatever the frame push
 *                               left there.  Values that live across an
 *                               ALLOCATING unwind-protect cleanup are parked
 *                               in pending_mv_values / saved_pending_stack.
 *   pre_call_mv_values          call_builtin (vm.c) snapshots mv_values into
 *                               it before every builtin; the only consumers
 *                               (THROW's NLX capture, builtins_io.c) read it
 *                               back before any allocating call — verified
 *                               zero-alloc.  REGRESSION HAZARD: if an NLX
 *                               builtin ever allocates before consuming it,
 *                               these slots must be visited under a validity
 *                               flag, like pending_mv_values.
 *   pending_mv_values beyond pending_mv_count, or at all while no throw is
 *                               pending — stale offsets from completed throws.
 *   the JIT native stack        gc_scan_jit_native_stack (mark only): a
 *                               conservative scan that PINS what it finds,
 *                               so the compactor never rewrites C-stack
 *                               words.
 *
 * Called with the world stopped — no locking, every thread is parked.
 *
 * We must #undef gc_root_count here because thread.h defines it as
 * (CT->gc_root_count), which collides with t->gc_root_count member access. */
#undef gc_root_count

void gc_visit_thread_roots(CL_Thread *t, gc_slot_fn visit)
{
    int i;

    /* Dynamic binding stack (saved old values) */
    for (i = 0; i < t->dyn_top; i++) {
        GC_DBG_SRC("dyn_stack", i);
        visit(&t->dyn_stack[i].symbol);
        visit(&t->dyn_stack[i].old_value);
    }

    /* NLX stack (catch tags, results, and saved bytecodes; mv_values
     * deliberately excluded — see the header comment). */
    for (i = 0; i < t->nlx_top; i++) {
        /* Every frame below nlx_top must have all three initialized — a
         * C-pushed frame too (cl_nlx_frame_init_c): the slot's previous
         * occupant leaves stale offsets behind, and marking one corrupts
         * the arena walk.  The per-field source names the culprit. */
        GC_DBG_SRC("nlx_stack.tag", i);
        visit(&t->nlx_stack[i].tag);
        GC_DBG_SRC("nlx_stack.result", i);
        visit(&t->nlx_stack[i].result);
        GC_DBG_SRC("nlx_stack.bytecode", i);
        visit(&t->nlx_stack[i].bytecode);
    }

    /* Handler stack */
    for (i = 0; i < t->handler_top; i++) {
        GC_DBG_SRC("handler_stack", i);
        visit(&t->handler_stack[i].type_name);
        visit(&t->handler_stack[i].handler);
    }

    /* Restart stack */
    for (i = 0; i < t->restart_top; i++) {
        GC_DBG_SRC("restart_stack", i);
        visit(&t->restart_stack[i].name);
        visit(&t->restart_stack[i].handler);
        visit(&t->restart_stack[i].tag);
        visit(&t->restart_stack[i].restart);
    }

    /* VM execution stack */
    if (t->vm.stack) {
        for (i = 0; i < t->vm.sp; i++) {
            GC_DBG_SRC("vm.stack", i);
            visit(&t->vm.stack[i]);
        }
    }

    /* Bytecode objects referenced by active VM frames */
    for (i = 0; i < t->vm.fp; i++) {
        GC_DBG_SRC("vm.frames.bytecode", i);
        visit(&t->vm.frames[i].bytecode);
    }

    /* Multiple values and pending throw state */
    for (i = 0; i < CL_MAX_MV; i++) {
        GC_DBG_SRC("mv_values", i);
        visit(&t->mv_values[i]);
    }
    /* (pre_call_mv_values deliberately excluded — see the header comment.) */
    GC_DBG_SRC("pending_tag/value", 0);
    visit(&t->pending_tag);
    visit(&t->pending_value);
    /* UNWIND-PROTECT value records (OP_MV_SAVE): every used word is a
     * CL_Obj — the values, and their count as a fixnum — live across the
     * allocating cleanup forms. */
    for (i = 0; i < t->mv_save_top && i < CL_MV_SAVE_SIZE; i++) {
        GC_DBG_SRC("mv_save_buf", i);
        visit(&t->mv_save_buf[i]);
    }
    /* Secondary values of an in-flight THROW.  These are live while an
     * unwind-protect cleanup runs (arbitrary allocating Lisp) between
     * the throw and the catch landing; without marking, a sweep during
     * the cleanup collects them and a compaction leaves stale offsets.
     * Bound STRICTLY by pending_mv_count while a throw is in flight:
     * unlike mv_values (continuously maintained), slots beyond the
     * count — or the whole array when no throw is pending — hold stale
     * offsets from completed throws; marking those would set mark bits
     * at non-object-start offsets and corrupt the arena walk. */
    if (t->pending_throw) {
        for (i = 0; i < t->pending_mv_count && i < CL_MAX_MV; i++)
            visit(&t->pending_mv_values[i]);
    }
    /* Saved pending-throw snapshots: one per armed unwind-protect whose
     * cleanup is running (nested UWPs park the OUTER throw's tag/values
     * here — see saved_pending_stack in thread.h). */
    for (i = 0; i < t->saved_pending_top; i++) {
        int m;
        /* A snapshot taken while no throw was in flight copies whatever
         * stale tag/value a COMPLETED throw left behind — only entries
         * with an armed pending_throw hold live references. */
        if (!t->saved_pending_stack[i].pending_throw)
            continue;
        visit(&t->saved_pending_stack[i].pending_tag);
        visit(&t->saved_pending_stack[i].pending_value);
        for (m = 0; m < t->saved_pending_stack[i].pending_mv_count &&
                    m < CL_MAX_MV; m++)
            visit(&t->saved_pending_stack[i].pending_mv_values[m]);
    }
    /* Compiler hand-off: holds a CL_Obj symbol across compile_expr's allocations
     * (set in compile_named_lambda/compile_defun, consumed in compile_lambda).
     * Must be marked AND updated or a compaction mid-lambda-compile leaves
     * bc->name a stale offset under GC stress. */
    GC_DBG_SRC("pending_lambda_name", 0);
    visit(&t->pending_lambda_name);
    /* Foreign-callback boundary: the condition parked by a callback until
     * the foreign call returns, and the one an unwind is carrying. */
    GC_DBG_SRC("callback_error", 0);
    visit(&t->callback_error);
    visit(&t->last_condition);

    /* Thread metadata */
    GC_DBG_SRC("thread-metadata", 0);
    visit(&t->name);
    visit(&t->result);
    visit(&t->interrupt_func);
    visit(&t->thread_obj);
    /* MP wait registration: the lock / condvar this thread is parked on
     * (and the lock a condition-wait re-acquires).  A parked thread keeps
     * its object alive, and the UPDATE counterpart forwards the record so
     * the releaser's / notifier's identity compare sees the moved object. */
    GC_DBG_SRC("mp-wait", 0);
    visit(&t->wait_obj);
    visit(&t->wait_lock);

    /* Current lexical env installed for a macro expander — keeps the
     * &environment alist alive while the expander runs. */
    GC_DBG_SRC("current_lex_env", 0);
    visit(&t->current_lex_env);

    /* Per-thread current package (the cl_current_package slot).  Packages
     * are also reachable via the package registry, so this mark is mostly
     * redundant — but the slot must be in the thread walk so the UPDATE
     * counterpart forwards it after a compaction (a stale offset here
     * makes this thread's reader intern into garbage). */
    GC_DBG_SRC("current_package", 0);
    visit(&t->current_package);

    /* Reader state — in-flight reader stream plus per-read uninterned
     * symbol alist (so #:foo identity survives a GC during a long READ). */
    GC_DBG_SRC("reader-state", 0);
    visit(&t->rd_stream);
    visit(&t->rd_uninterned);
    visit(&t->rd_labels);

    /* Printer in-progress object stack (re-entrancy detection in
     * print-object-hook).  Pinned across hook apply so a GC inside the
     * Lisp print-object method doesn't sweep the object whose recursion
     * we're guarding. */
    for (i = 0; i < t->pr_inprog_top; i++) {
        GC_DBG_SRC("pr_inprog", i);
        visit(&t->pr_inprog[i]);
    }

    /* Current printer output target (format/print/write).  Held in a thread
     * field for the whole print, across allocating sub-prints (print-object
     * hooks, string-stream outbuf growth).  A peer thread's compaction while
     * this thread is parked mid-print relocates the stream object; without
     * marking+forwarding this slot, out_char/out_str dereference a stale
     * offset and scribble into freed arena — the multi-thread `(format nil …)`
     * corruption.  Single-writer (owning thread), stopped during STW GC. */
    GC_DBG_SRC("pr_stream", 0);
    visit(&t->pr_stream);

    /* *print-circle* detection table keys — live CL_Obj references while a
     * circular/shared print is in progress (pr_circle_active).  Same hazard
     * as pr_stream: relocated out from under the parked printer thread. */
    if (t->pr_circle_active) {
        int ci;
        for (ci = 0; ci < CL_CIRCLE_HT_SIZE; ci++) {
            if (!CL_NULL_P(t->pr_circle_keys[ci])) {
                GC_DBG_SRC("pr_circle_keys", ci);
                visit(&t->pr_circle_keys[ci]);
            }
        }
    }

    /* Pending LOAD-TIME-VALUE (cell, thunk) pairs (compile-file only) */
    for (i = 0; i < t->ltv_init_count; i++) {
        GC_DBG_SRC("ltv_init", i);
        visit(&t->ltv_init_cells[i]);
        visit(&t->ltv_init_thunks[i]);
    }

    /* Compiler constants (active compilers may hold CL_Obj values
     * in platform_alloc'd memory not reachable from the GC arena) */
    GC_DBG_SRC("compiler-constants", 0);
    cl_compiler_gc_visit_thread(t, visit);

    /* VM-internal buffers (e.g. vm_extra_args during &rest processing) */
    GC_DBG_SRC("vm_extra_args", 0);
    cl_vm_gc_visit_extra_thread(t, visit);

    /* Thread-Local Value (TLV) table — both symbol and value of every
     * non-empty, non-tombstone entry */
    {
        int ti;
        for (ti = 0; ti < CL_TLV_TABLE_SIZE; ti++) {
            CL_Obj sym = t->tlv_table[ti].symbol;
            if (sym != CL_NIL && sym != CL_UNBOUND) {
                GC_DBG_SRC("tlv_table", ti);
                visit(&t->tlv_table[ti].symbol);
                visit(&t->tlv_table[ti].value);
            }
        }
    }
}

/* The dynamically registered slots: cl_gc_register_root'ed globals and the
 * CL_GC_PROTECT stacks of every thread.  Each entry is the ADDRESS of a C
 * variable, so the same slot can legitimately appear twice (a double
 * protect, a protect of a slot the thread walk already covers); this walk
 * reports every registration as-is and leaves de-duplication to the
 * visitor that needs it (gc_update_registered_roots). */
void gc_visit_registered_roots(gc_slot_fn visit)
{
    CL_Thread *t;
    int i;

    for (i = 0; i < n_global_roots; i++) {
        GC_DBG_SRC("global_roots", i);
        visit(global_roots[i]);
    }
    for (t = cl_thread_list; t; t = t->next) {
        for (i = 0; i < t->gc_root_count; i++) {
            GC_DBG_SRC("gc_roots", i);
            visit(t->gc_roots[i]);
        }
    }
}
/* Restore gc_root_count macro for the rest of the file */
#define gc_root_count (CT->gc_root_count)

/* Process-wide roots: the registries and tables that are not reachable
 * through any thread.  Slots that may hold NIL are visited unconditionally
 * — both gc_mark_obj and gc_forward are no-ops on immediates. */
void gc_visit_shared_roots(gc_slot_fn visit)
{
    GC_DBG_SRC("shared-globals", 0);

    /* Package registry — transitively reaches all packages, all symbols,
     * and all their values/functions/plists */
    visit(&cl_package_registry);

    /* Compiler tables (alists not reachable through packages) */
    visit(&macro_table);
    visit(&setf_table);
    visit(&setf_fn_table);
    visit(&setf_expander_table);
    visit(&type_table);
    visit(&compiler_macro_table);
    visit(&cl_clos_class_table);
    visit(&struct_table);
    visit(&condition_hierarchy);
    visit(&condition_slot_table);
    visit(&condition_default_initargs);
    visit(&condition_slot_initforms);

    /* Thread system: main thread's Lisp object */
    {
        extern CL_Obj *cl_main_thread_lisp_obj_ptr(void);
        CL_Obj *slot = cl_main_thread_lisp_obj_ptr();
        if (slot) visit(slot);
    }

    /* Readtable user macro closures */
    {
        int rt, ch;
        for (rt = 0; rt < CL_RT_POOL_SIZE; rt++) {
            if (!(cl_readtable_alloc_mask & (1u << rt)))
                continue;
            for (ch = 0; ch < CL_RT_CHARS; ch++) {
                visit(&cl_readtable_pool[rt].macro_fn[ch]);
                visit(&cl_readtable_pool[rt].dispatch_fn[ch]);
            }
        }
    }

    /* Active FASL readers — their gensym_objs[]/shared_objs[] dedup tables
     * hold CL_Obj references that forward GENSYM_REF/OBJ_REF resolve through
     * but which may not yet be reachable from the graph under construction. */
    cl_fasl_gc_visit_readers(visit);

    /* Active FASL WRITERS — bi_load's auto-cache writer holds gensym dedup
     * entries across the interleaved compile/eval of later forms. */
    cl_fasl_gc_visit_writers(visit);

    /* Active MAKE-LOAD-FORM writer pre-pass — its walk/result arrays hold
     * CL_Obj offsets that a compaction during a user MAKE-LOAD-FORM method
     * must keep live and forward. */
    cl_fasl_gc_visit_mlf(visit);
}

/* Every root slot, once per registration. */
void gc_visit_roots(gc_slot_fn visit)
{
    CL_Thread *t;

    for (t = cl_thread_list; t; t = t->next)
        gc_visit_thread_roots(t, visit);
    gc_visit_registered_roots(visit);
    gc_visit_shared_roots(visit);
}

/* The mark phase's visitor: mark the slot's value.  Roots are marked
 * directly (not via gc_mark_push): gc_mark_obj sets the mark bit at once
 * and only then pushes children, so if the mark stack overflows the root
 * itself is still marked and the heap re-scan recovers its children —
 * a root dropped from the stack would never be marked at all. */
static void gc_mark_slot(CL_Obj *slot)
{
    gc_mark_obj(*slot);
}

void gc_mark(void)
{
    CL_Thread *t;

    gc_mark_overflow = 0;
    /* Re-arm mark-stack growth: a previous cycle's OOM may have been
     * transient (the failing platform_alloc can succeed now), and the
     * heap-proportional cap re-derives from the current arena anyway. */
    gc_mark_grow_failed = 0;
    /* Reset the pinned-object set; the conservative JIT-stack scan below
     * rebuilds it this cycle.  Stays empty unless a JIT frame is live. */
    jit_pinned_count = 0;
    gc_jit_pin_oom = 0;
    /* Invalidate the free-list snapshot — the free list has changed since
     * the last cycle; the first JIT-stack scan this cycle re-collects it. */
    jit_scan_free_valid = 0;

    /* Per-thread roots for ALL registered threads, plus the conservative
     * scan of a thread's JIT native stack (no-op unless it is parked inside
     * JIT'd code).  During STW GC all other threads are stopped, so the
     * thread list is stable — no lock needed for iteration. */
    for (t = cl_thread_list; t; t = t->next) {
        gc_visit_thread_roots(t, gc_mark_slot);
        gc_scan_jit_native_stack(t);
    }
    gc_visit_registered_roots(gc_mark_slot);
    gc_visit_shared_roots(gc_mark_slot);

    /* Drain mark stack iteratively (children pushed by gc_mark_obj above).
     * Do NOT clear gc_mark_overflow here — it may have been set during
     * root marking above, and the re-scan loop below must handle it. */
    while (gc_mark_top > 0) {
        CL_Obj obj = gc_mark_stack[--gc_mark_top];
        gc_mark_obj(obj);
    }

    /* Handle mark stack overflow: re-scan heap for marked objects whose
     * children may not have been pushed.  Repeat until no overflow.
     * Only reachable when gc_mark_stack_grow failed (cap/OOM) — each pass
     * is O(arena) and recovers at most ~one-stack-full of dropped children,
     * so this is the quadratic last resort, not the normal path.  The pass
     * counter feeds ext:%gc-mark-stats so the fallback is observable. */
    while (gc_mark_overflow) {
        uint8_t *ptr = cl_heap.arena + CL_ALIGN;
        uint8_t *end = cl_heap.arena + cl_heap.bump;

        gc_mark_overflow = 0;
        gc_mark_rescan_passes++;
        while (ptr < end) {
            uint32_t size = CL_HDR_SIZE(ptr);
            if (size == 0) break;
            if (CL_HDR_MARKED(ptr)) {
                /* Re-push children — gc_mark_obj will skip already-marked ones */
                gc_mark_children(ptr, CL_HDR_TYPE(ptr));
            }
            ptr += size;
        }
        /* Drain anything newly pushed */
        while (gc_mark_top > 0) {
            CL_Obj obj = gc_mark_stack[--gc_mark_top];
            gc_mark_obj(obj);
        }
    }
}

/* --- Sweep Phase --- */

/* Release external resources owned by a dead heap object.
 * Called from gc_sweep with the world stopped, so the per-table mutex used
 * by the stream / thread side tables is not needed: no other
 * thread can mutate the tables here. */
/* R-srcloc: the reader's cons→source-line table is keyed by arena OFFSETS.
 * Two staleness modes corrupt its diagnostics: (a) a key whose cons died —
 * a later object allocated at the same offset false-matches and reports a
 * WRONG file/line; (b) after compaction, a surviving cons moved and its
 * (correct) entry becomes unreachable while its old offset may false-match.
 * Fix: clear dead keys right after marking (mark bits still set), and
 * forward surviving keys while the forwarding table is alive — then move
 * each entry to the slot its NEW key hashes to, since lookup only ever
 * probes that one slot. */
void gc_srcloc_invalidate_dead(void)
{
    uint32_t i;
    for (i = 0; i < CL_SRCLOC_SIZE; i++) {
        CL_Obj k = cl_srcloc_table[i].cons_obj;
        if (CL_NULL_P(k) || CL_FIXNUM_P(k) || CL_CHAR_P(k)) continue;
        if (k >= cl_heap.bump || !CL_HDR_MARKED(CL_OBJ_TO_PTR(k)))
            cl_srcloc_table[i].cons_obj = CL_NIL;
    }
}

void gc_srcloc_forward(void)
{
    uint32_t i;
    for (i = 0; i < CL_SRCLOC_SIZE; i++) {
        CL_Obj k = cl_srcloc_table[i].cons_obj;
        if (CL_NULL_P(k) || CL_FIXNUM_P(k) || CL_CHAR_P(k)) continue;
        cl_srcloc_table[i].cons_obj = gc_forward(k);
    }
    /* Re-home in place, no scratch table (16 KB matters on the Amiga).
     * Carry a misplaced entry to its home slot: an empty home takes it; a
     * home holding a misplaced entry swaps, and that entry is carried on;
     * a home holding its own entry is a collision, and the table is lossy
     * like srcloc_record's overwrite: the higher offset keeps the slot.
     * A slide keeps allocation order and a minor promotes above the old
     * space, so that is the more recently read form, the one the compiler
     * may still look up; the loser is typically a long-compiled form kept
     * alive by a quoted constant or a lambda list.  Each step settles or
     * drops one misplaced entry, so the pass is linear, and slots below i
     * only ever hold settled entries. */
    for (i = 0; i < CL_SRCLOC_SIZE; i++) {
        CL_SrcLoc carry;
        uint32_t home;
        if (CL_NULL_P(cl_srcloc_table[i].cons_obj)) continue;
        home = CL_SRCLOC_INDEX(cl_srcloc_table[i].cons_obj);
        if (home == i) continue;
        carry = cl_srcloc_table[i];
        cl_srcloc_table[i].cons_obj = CL_NIL;
        for (;;) {
            CL_SrcLoc *slot = &cl_srcloc_table[home];
            CL_SrcLoc next;
            if (CL_NULL_P(slot->cons_obj)) { *slot = carry; break; }
            if (CL_SRCLOC_INDEX(slot->cons_obj) == home) {
                if (carry.cons_obj > slot->cons_obj) *slot = carry;
                break;
            }
            next = *slot;
            *slot = carry;
            carry = next;
            home = CL_SRCLOC_INDEX(carry.cons_obj);
        }
    }
}

/* Release every off-heap buffer owned by a CL_Bytecode and return the
 * number of bytes handed back.
 *
 * A CL_Bytecode is a small arena object with EIGHT platform_alloc'd
 * satellites hanging off it — the bytecode body, the constants pool, the
 * three &key arrays, the pc→line map, and the JIT's native code plus its
 * reloc table.  Each is allocated fresh for exactly one bytecode (see
 * compiler.c cl_compile_lambda / cl_compile, fasl.c read_bytecode and
 * image.c restore — none of them ever aliases a buffer into a second
 * object), so ownership is exclusive and freeing here is a clean handback.
 *
 * `source_file` is deliberately NOT freed: it points into the interned
 * source-file pool (cl_intern_source_file), which is shared by every
 * bytecode compiled from the same file and owned by that pool.
 *
 * Every field is NULLed and its length zeroed so a second call is a
 * no-op — the sweep can reach the same corpse twice (once directly, once
 * via the coalesce loop), and the shutdown walk runs over objects the
 * sweep may already have finalized. */
uint32_t bytecode_release_offheap(CL_Bytecode *bc)
{
    uint32_t freed = 0;
    int had_any = (bc->code || bc->constants || bc->key_syms ||
                   bc->key_slots || bc->key_suppliedp_slots ||
                   bc->line_map || bc->native_code || bc->native_relocs);

    if (bc->code) {
        freed += bc->code_len;
        platform_free(bc->code);
        bc->code = NULL;
    }
    bc->code_len = 0;

    if (bc->constants) {
        freed += (uint32_t)bc->n_constants * (uint32_t)sizeof(CL_Obj);
        platform_free(bc->constants);
        bc->constants = NULL;
    }
    bc->n_constants = 0;

    if (bc->key_syms) {
        freed += (uint32_t)bc->n_keys * (uint32_t)sizeof(CL_Obj);
        platform_free(bc->key_syms);
        bc->key_syms = NULL;
    }
    if (bc->key_slots) {
        freed += bc->n_keys;
        platform_free(bc->key_slots);
        bc->key_slots = NULL;
    }
    if (bc->key_suppliedp_slots) {
        freed += bc->n_keys;
        platform_free(bc->key_suppliedp_slots);
        bc->key_suppliedp_slots = NULL;
    }
    bc->n_keys = 0;

    if (bc->line_map) {
        freed += (uint32_t)bc->line_map_count * (uint32_t)sizeof(CL_LineEntry);
        platform_free(bc->line_map);
        bc->line_map = NULL;
    }
    bc->line_map_count = 0;

    if (bc->native_code) {
        freed += bc->native_len;
        cl_jit_free_native(bc->native_code);   /* backend's own allocator */
        bc->native_code = NULL;
    }
    bc->native_len = 0;

    if (bc->native_relocs) {
        freed += (uint32_t)bc->native_reloc_count * (uint32_t)sizeof(uint32_t);
        platform_free(bc->native_relocs);
        bc->native_relocs = NULL;
    }
    bc->native_reloc_count = 0;

    /* Not ours to free — the interned source-file pool owns the string (see
     * cl_intern_source_file).  Dropped rather than kept so no bytecode is
     * left pointing into that pool after cl_compiler_shutdown releases it. */
    bc->source_file = NULL;

    if (had_any) {
        gc_bc_offheap_bytes += freed;
        gc_bc_offheap_count++;
    }
    return freed;
}

void gc_finalize_dead(uint8_t *ptr)
{
    switch (CL_HDR_TYPE(ptr)) {
    case TYPE_BYTECODE: {
        /* Free every off-heap buffer owned by a dead bytecode: the body, the
         * constants pool, the &key arrays, the line map and the JIT artifacts
         * are all platform_alloc'd and were otherwise leaked for good when
         * the object is swept.  Safe here: the world is stopped and a DEAD
         * (unmarked) bytecode cannot be executing — a running one is
         * reachable from a VM frame (gc_visit_thread_roots marks both
         * vm.frames[i].bytecode and nlx_stack[i].bytecode, which is what
         * keeps the raw frame->code / frame->constants pointers valid) or
         * from the conservative JIT stack scan, and would have been marked. */
        bytecode_release_offheap((CL_Bytecode *)ptr);
        break;
    }
    case TYPE_STREAM: {
        /* Output-stream outbuf slots are NOT freed here.  Freeing a dead
         * stream's st->out_buf_handle is unsafe: the slot's handle may already
         * have been recycled by a live string-output-stream (handles are
         * reused as soon as a slot is freed), so re-finalizing a stale corpse
         * would free the LIVE stream's buffer.  Outbufs are reclaimed instead
         * by the mark-driven cl_stream_outbuf_gc_reclaim() after marking.
         *
         * OS handles, however, ARE closed here (ST8): a dead file/socket
         * stream that was never CLOSEd would otherwise leak its fd forever —
         * long-running servers exhaust the descriptor table.  The close uses
         * GC-context variants (platform_file_close is already safe-region-
         * free on both platforms; platform_socket_close_gc skips the
         * write-buffer flush and its safe-region bracket — buffered output
         * on an UNREACHABLE stream is forfeit, and entering a safe region
         * while this thread orchestrates the collection is forbidden).  The
         * OPEN flag gates it: a properly closed stream cleared it (under the
         * iolock/CAS discipline in cl_stream_close), and clearing it here
         * makes a coalesce-path re-finalize a no-op.  Runs under STW — no
         * concurrent closer can race the flag. */
        CL_Stream *st = (CL_Stream *)ptr;
        if ((st->flags & CL_STREAM_FLAG_OPEN) && st->handle_id != 0 &&
            (st->stream_type == CL_STREAM_FILE ||
             st->stream_type == CL_STREAM_SOCKET)) {
            st->flags &= ~CL_STREAM_FLAG_OPEN;
            fprintf(stderr, "[GC] warning: closing a %s stream that was "
                    "dropped without CLOSE (handle %u)\n",
                    st->stream_type == CL_STREAM_FILE ? "file" : "socket",
                    (unsigned)st->handle_id);
            if (st->stream_type == CL_STREAM_FILE)
                platform_file_close((PlatformFile)st->handle_id);
            else
                platform_socket_close_gc((PlatformSocket)st->handle_id);
        }
        /* A C-buffer stream its owner was unwound past (LOAD left by a
         * non-local exit): slot and file buffer come back here. */
        cl_stream_cbuf_gc_release(st);
        break;
    }
    /* TYPE_LOCK / TYPE_CONDVAR: plain heap words, nothing external to
     * release — a dead lock is ordinary garbage whether or not some thread
     * still "holds" it (specs/mp-locks-heap-words.md).  A thread parked on
     * a lock or condvar keeps it alive through its wait_obj root, so a
     * waiter can never find its object gone. */
    case TYPE_LOCK:
    case TYPE_CONDVAR:
        break;
    case TYPE_FOREIGN_POINTER: {
        /* Reclaim the side-table slot for transient (unowned) foreign
         * pointers — dlsym results, values returned from foreign calls, and
         * results of pointer arithmetic (CFFI inc-pointer / make-pointer).
         * Without this the POSIX side table leaks one slot per such pointer.
         *
         * OWNED allocations are deliberately left alone: per CFFI semantics
         * FFI:ALLOC-FOREIGN memory lives until an explicit FFI:FREE-FOREIGN,
         * and callers routinely keep only the integer address (via
         * pointer-address / make-pointer) while the wrapper object dies — so
         * freeing here would be a use-after-free.  Each register/alloc hands
         * out a unique handle, so this finalize is 1:1 with the slot. */
        CL_ForeignPtr *fp = (CL_ForeignPtr *)ptr;
        if (fp->address != 0 && !(fp->flags & CL_FPTR_FLAG_OWNED))
            platform_ffi_release(fp->address);
        break;
    }
    case TYPE_THREAD: {
        /* Wrapper for an MP thread becoming dead means the OS thread must
         * already be finished: while the worker is registered, gc_mark
         * marks t->thread_obj from gc_visit_thread_roots, so the wrapper
         * is reachable.  thread_entry publishes the terminal status (2
         * finished / 3 aborted) only AFTER it has called
         * cl_thread_unregister(t) — so a wrapper that reaches sweep is
         * guaranteed to have both `t` out of cl_thread_list and
         * status >= 2.
         *
         * Three guards:
         *  - status >= 2: defense-in-depth; never free a still-running
         *    worker that some other path failed to keep the wrapper
         *    reachable for.
         *  - t->thread_obj points back to THIS wrapper: required to
         *    avoid a slot-reuse double-free.  After bi_join_thread or
         *    the bi_make_thread zombie reaper releases the slot, the
         *    next make-thread can reuse it for an unrelated worker.
         *    The old wrapper still holds the now-stale thread_id;
         *    without this check we'd free the new worker on the old
         *    wrapper's finalize. */
        CL_ThreadObj *to = (CL_ThreadObj *)ptr;
        if (to->thread_id != 0 && to->thread_id < CL_MAX_THREADS) {
            CL_Thread *t = cl_thread_table[to->thread_id];
            /* table_gen compare: exact slot-reuse guard — the wrapper's
             * generation must match the slot's current occupancy.  This
             * replaces the old `t->thread_obj == CL_PTR_TO_OBJ(ptr)`
             * back-compare: t->thread_obj stops being forwarded once the
             * worker unregisters, so after any post-unregister compaction
             * it held a stale offset, the compare failed, and the worker
             * leaked until the table-full reaper ran.  The generation pair
             * is immune to relocation (plain integers) and is unique per
             * occupancy, so it is both safe against slot reuse AND exact.
             * Safe to read without the list lock: sweep runs during STW,
             * no peer can reclaim the slot concurrently. */
            if (t && t != cl_main_thread_ptr && t->status >= 2 &&
                cl_thread_table_gen[to->thread_id] == to->table_gen) {
                cl_thread_table[to->thread_id] = NULL;
                if (t->platform_handle) {
                    platform_thread_detach(t->platform_handle);
                    t->platform_handle = NULL;
                }
                cl_thread_free_worker(t);
            }
        }
        break;
    }
    default:
        break;
    }
}
