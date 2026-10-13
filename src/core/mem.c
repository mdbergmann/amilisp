/* mem.c — heap core: arena, allocation fast path, constructors,
 * root stacks and the registered-root table, init/shutdown, stats.
 *
 * Backend-neutral: the collector behind it is whichever file implements
 * gc_backend.h (gc_classic.c: bump + free-list refill, sweep, the
 * generational mode; gc_immix.c: Part 2 of specs/gc-backend-immix.md),
 * and this file asks for it only through the gcb_* interface.  The
 * shared facilities — gc_trace.c (mark, root enumeration), gc_compact.c
 * (the sliding compactor), gc_jitscan.c (conservative JIT-stack scan and
 * its block-start index), gc_verify.c (verifiers) — share state through
 * gc_internal.h.
 */
#include "gc_internal.h"

/* Allocation mutex — protects bump pointer, free list, and heap metadata */
void *alloc_mutex = NULL;

CL_Heap cl_heap;
uint8_t *cl_arena_base = NULL;  /* Global arena base for offset↔pointer conversion */

/* GC root stack now lives in CL_Thread.
 * gc_root_count is a macro from thread.h.
 * gc_root_stack is a local macro below. */
#define gc_root_stack (CT->gc_roots)

/* Forward declarations */
static void gc_reset_transient_state(void);

/* Cumulative GC phase timers (microseconds, monotonic; exposed via
 * ext:%gc-time-stats and cl_mem_stats).  stw = waiting for other threads to
 * park at the start of a collection; mark/sweep = the sweep-GC phases as
 * timed in cl_gc; compact = whole cl_gc_compact cycles (which run their own
 * mark internally — compaction time is NOT double-counted into mark/sweep).
 * Updated only with the world stopped / single-threaded, so plain u64s. */
uint64_t gc_time_stw_us = 0;
uint64_t gc_time_mark_us = 0;
uint64_t gc_time_sweep_us = 0;
uint64_t gc_time_compact_us = 0;
uint64_t gc_time_stw_max_us = 0;  /* worst single stop */
uint32_t gc_stw_stops = 0;        /* stop-the-world events */
uint32_t gc_epoch_skips = 0;      /* redundant GCs deduped (see
                                          * cl_gc_if_stale) */

/* ---- Per-collection trace (CLAMIGA_GC_DIAG) ----
 * One stderr line per collection when the CLAMIGA_GC_DIAG environment
 * variable is set: wall clock, cycle number, collection kind, pause, and
 * heap occupancy.  Lets a hang be classified from clamiga's own output —
 * an endless stream of lines is a GC storm, silence means the time is
 * going somewhere else.  Runtime diagnostic, not DEBUG-flag
 * instrumentation: always compiled, zero cost when the env var is unset
 * (same pattern as CLAMIGA_STW_DIAG / CLAMIGA_SOCK_DIAG). */
static int32_t gc_diag_cached = -2;   /* -2 = env not read; 0 = off; 1 = on */

int gc_diag_on(void)
{
    if (gc_diag_cached == -2) {
        char envbuf[16];
        const char *s = platform_getenv("CLAMIGA_GC_DIAG", envbuf,
                                        (int)sizeof(envbuf));
        gc_diag_cached = (s && *s) ? 1 : 0;
    }
    return gc_diag_cached;
}

void gc_diag_report(const char *kind, uint64_t pause_us)
{
    fprintf(stderr, "[GC] %lums #%lu %s pause=%luus used=%luK/%luK\n",
            (unsigned long)platform_time_ms(),
            (unsigned long)cl_heap.gc_count, kind,
            (unsigned long)pause_us,
            (unsigned long)(cl_heap.total_allocated >> 10),
            (unsigned long)(cl_heap.arena_size >> 10));
    fflush(stderr);
}

#ifdef DEBUG_GC_STRESS
/* GC-stress debugging: when ready, force a compaction before every single-
 * threaded allocation (see cl_alloc).  Enabled at runtime after boot so the
 * FASL/boot load isn't slowed to a crawl. */
int cl_gc_stress_ready = 0;
static int cl_gc_stress_active = 0;
#endif

/* Count of threads currently inside cl_jit_invoke.  Bumped on
 * outermost JIT entry, decremented when the outermost JIT frame
 * returns.  Informational since offset-validated conservative
 * scanning (gc_scan_jit_native_stack) made the compaction inhibit
 * unnecessary; kept for future use (e.g. JIT-aware diagnostics or
 * a faster early-out path).  See specs/native-backend.md
 * §"GC interaction". */
volatile int cl_jit_active_threads = 0;

/* Global root registration table — static CL_Obj variables that must be
 * marked during GC and updated (forwarded) during compaction.
 * Used for cached interned keyword symbols, type symbols, etc. */
CL_Obj *global_roots[CL_MAX_GLOBAL_ROOTS];
int n_global_roots = 0;
#ifdef DEBUG_GC
/* Registration sites, recorded by the cl_gc_register_root macro so the
 * image writer can name late-registered roots (mem.h). */
const char *global_root_files[CL_MAX_GLOBAL_ROOTS];
int         global_root_lines[CL_MAX_GLOBAL_ROOTS];
/* The macro in mem.h redirects call sites to cl_gc_register_root_at;
 * drop it here so the base function below can be defined (and called). */
#undef cl_gc_register_root
#endif

void cl_gc_register_root(CL_Obj *root_ptr)
{
    int i;
    /* Idempotent: registering the same address twice must be a no-op.
     * gc_forward is NOT idempotent (the forwarding table is keyed by
     * pre-compaction offsets), so a slot forwarded twice through two
     * registry entries would be rewritten to an unrelated object.  The
     * init functions that register handles re-run on every heap
     * re-initialization (each C unit test, embedded restarts) and would
     * otherwise duplicate every entry.  Registration is boot-time-only,
     * so the linear scan costs nothing at runtime. */
    for (i = 0; i < n_global_roots; i++) {
        if (global_roots[i] == root_ptr)
            return;
    }
    if (n_global_roots >= CL_MAX_GLOBAL_ROOTS) {
        /* A silently-dropped root is a guaranteed future memory
         * corruption (the slot goes stale on the next compaction and is
         * never marked).  Fail loudly at boot instead. */
        platform_write_string(
            "FATAL: cl_gc_register_root: CL_MAX_GLOBAL_ROOTS exceeded - "
            "raise the limit in mem.h\n");
        platform_flush_output();
        exit(1);
    }
    global_roots[n_global_roots++] = root_ptr;
}

#ifdef DEBUG_GC
void cl_gc_register_root_at(CL_Obj *root_ptr, const char *file, int line)
{
    int before = n_global_roots;
    cl_gc_register_root(root_ptr);
    if (n_global_roots > before) {
        global_root_files[before] = file;
        global_root_lines[before] = line;
    }
}

const char *cl_gc_global_root_file(int i)
{
    if (i < 0 || i >= n_global_roots) return NULL;
    return global_root_files[i];
}

int cl_gc_global_root_line(int i)
{
    if (i < 0 || i >= n_global_roots) return 0;
    return global_root_lines[i];
}
#endif /* DEBUG_GC */

/* --- Heap-image root enumeration (image.c) --- */

int cl_gc_global_root_count(void)
{
    return n_global_roots;
}

CL_Obj cl_gc_global_root_get(int i)
{
    if (i < 0 || i >= n_global_roots) return CL_NIL;
    return *global_roots[i];
}

void cl_gc_global_root_set(int i, CL_Obj value)
{
    if (i < 0 || i >= n_global_roots) return;
    *global_roots[i] = value;
}

/* Align size up to CL_ALIGN boundary */
static uint32_t align_up(uint32_t size)
{
    return (size + CL_ALIGN - 1) & ~(CL_ALIGN - 1);
}

/* Zero a freshly allocated block before its header is written.
 *
 * The zeroing itself is mandatory (see the call sites: stale bytes in
 * padding are read back as CL_Obj fields by the GC and the FASL writer),
 * but for a CONS — CL_MIN_ALLOC_SIZE, and far and away the most allocated
 * object — an out-of-line memset call costs more than the four stores it
 * performs.  The cutoff stays at the minimum block size on purpose: a
 * tuned memset beats a scalar word loop from surprisingly few bytes up
 * (measured on this host at 32 bytes and above), so widening this is a
 * pessimization, not an improvement.  SIZE is CL_ALIGN-aligned here.
 *
 * The header word is NOT zeroed.  It holds the cursor's own header for
 * the bytes being cut (the chunk remainder's, or a hole's placeholder)
 * until cl_alloc stores the object's, so a walk that reaches this block
 * from another thread always finds a header whose size covers it — the
 * Immix claim walks the chain on a live heap (gc_immix.c im_claim, under
 * alloc_mutex only), and a zero header there would end the walk, or lead
 * it into the next cut's stale bytes. */
static inline void alloc_zero(void *ptr, uint32_t size)
{
    uint32_t *p = (uint32_t *)ptr + 1;
    if (size == CL_MIN_ALLOC_SIZE) {
        uint32_t n = CL_MIN_ALLOC_SIZE / 4 - 1;
        while (n--) *p++ = 0;
    } else {
        memset(p, 0, size - 4);
    }
}

/* ================================================================
 * Allocation cursor — the lock-free fast path of cl_alloc
 *
 * cl_alloc cuts objects from a cursor — a region [cur, lim) the current
 * thread owns outright — with no locking at all, and the backend refills
 * it in gcb_alloc_slow when a cut does not fit.  The cursor's SHAPE is the
 * backend's (gcb_alloc_fast in gc_backend.h, where the classic backend's
 * two cursors — the live bump front single-threaded, a private chunk
 * multi-threaded — and their invariants are documented); the per-thread
 * chunk fields (alloc_cur / alloc_lim / alloc_consed, thread.h) and the
 * dropping of cursors under stop-the-world, at thread exit, on heap
 * re-init and on image adoption are shared and live here.
 * ================================================================ */

/* Drop t's chunk and flush its deferred accounting.  The remainder is
 * already a formatted hole, so nothing in the arena changes.  The caller
 * serializes against the owner: stop-the-world, thread retirement, or the
 * owner itself. */
void gc_cursor_drop(CL_Thread *t)
{
    t->alloc_cur = t->alloc_lim = 0;
    cl_heap.total_consed += t->alloc_consed;
    t->alloc_consed = 0;
}

/* Drop every registered thread's chunk.  Called with the world stopped
 * (or single-threaded) at the top of each GC cycle, on heap re-init and
 * on image adoption.  Remainders are already formatted holes — the
 * sweep/compaction that follows reclaims them; owners refill on their
 * next allocation. */
void gc_cursor_drop_all(void)
{
    CL_Thread *t;
    for (t = cl_thread_list; t; t = t->next)
        gc_cursor_drop(t);
}

/* Thread-exit retirement (see mem.h).  Runs on a live heap, so the
 * counter flush needs alloc_mutex; the remainder hole needs nothing. */
void cl_cursor_retire(CL_Thread *t)
{
    int multi = (cl_thread_count > 1);
    if (!t->alloc_lim && !t->alloc_consed)
        return;
    if (multi && alloc_mutex) platform_mutex_lock(alloc_mutex);
    gc_cursor_drop(t);
    if (multi && alloc_mutex) platform_mutex_unlock(alloc_mutex);
}

/* CLAMIGA_MEM_DIAG=1 — report at shutdown how much off-heap object payload
 * was handed back (gc_release_all_offheap).  On AmigaOS, where unreturned
 * memory is lost to the system until reboot, this is the number to watch:
 * pair it with `Avail` before and after a run to account for every byte. */
int cl_mem_diag = 0;

void cl_mem_diag_from_env(void)
{
    char dbuf[8];
    const char *d = platform_getenv("CLAMIGA_MEM_DIAG", dbuf,
                                    (int)sizeof(dbuf));
    cl_mem_diag = (d && d[0] && !(d[0] == '0' && d[1] == '\0'));
}

int cl_mem_try_init(uint32_t heap_size)
{
    cl_mem_diag_from_env();

    if (heap_size == 0)
        heap_size = CL_DEFAULT_HEAP_SIZE;

    /* The backend places the arena (its generational mode needs
     * protectable pages) and resets whatever it kept for a previous heap. */
    cl_heap.arena = gcb_arena_alloc(heap_size);
    if (!cl_heap.arena) {
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "FATAL: Failed to allocate %u-byte heap (try a smaller --heap)\n",
                 (unsigned)heap_size);
        platform_write_string(msg);
        platform_flush_output();
        return -1;
    }
    cl_arena_base = cl_heap.arena;
    cl_heap.arena_size = heap_size;
    cl_heap.bump = CL_ALIGN;  /* Skip offset 0 so it stays NIL */
    cl_heap.free_list = 0;
    cl_heap.total_allocated = 0;
    cl_heap.total_consed = 0;
    cl_heap.gc_count = 0;
    cl_heap.compact_count = 0;
    cl_heap.freelist_steps = 0;
    cl_heap.tlab_refills = 0;
    gc_last_compact_cycle = 0xFFFFFFFF;

    /* Side tables and policy for this arena (block-start index, chunk
     * size, the classic counters) — re-derived for every new heap. */
    gcb_init();

    /* Clear the allocation chunk of every registered thread: the offsets
     * point into the PREVIOUS arena (unit tests re-init with fresh heaps),
     * and a flush of its consed count into the fresh heap's counter would
     * be wrong — zero the counts outright instead of gc_cursor_drop_all. */
    {
        CL_Thread *t;
        for (t = cl_thread_list; t; t = t->next) {
            t->alloc_cur = t->alloc_lim = 0;
            t->alloc_consed = 0;
        }
    }

    /* Reset EVERY registered thread's Lisp-heap references (root stacks,
     * mv_values, pending/handler/restart/reader/printer/TLV state, ...)
     * — all of them are stale offsets into the arena just freed, and
     * gc_visit_thread_roots walks them unconditionally.  Same bug class as
     * the shared-globals reset below; resetting only the CURRENT thread's
     * gc_root_count (the old behavior) left e.g. main's mv_values[] from
     * a pre-re-init cl_error pointing into the fresh arena. */
    {
        CL_Thread *rt;
        for (rt = cl_thread_list; rt; rt = rt->next)
            cl_thread_reset_lisp_state(rt);
    }
    /* Drop any grown mark stack from a previous heap: the growth cap is
     * derived from the (possibly different) new arena size, and unit tests
     * re-init with small heaps.  Also resets gc_mark_top. */
    gc_mark_stack_release();
    gc_mark_stack_grows = 0;
    gc_mark_rescan_passes = 0;
    gc_mark_stack_test_limit = 0;
    gc_time_stw_us = 0;
    gc_time_mark_us = 0;
    gc_time_sweep_us = 0;
    gc_time_compact_us = 0;
    gc_time_stw_max_us = 0;
    gc_stw_stops = 0;
    gc_epoch_skips = 0;
    gc_bc_offheap_bytes = 0;
    gc_bc_offheap_count = 0;

    /* Reset GC state that survives in static storage across heap
     * re-initialization (each C unit test, embedded restarts): pending
     * flags and JIT scan/pin bookkeeping hold offsets and decisions from
     * the PREVIOUS arena. */
    gc_reset_transient_state();

    /* Reset the global root registry: registered slots are static C
     * globals still holding offsets into the PREVIOUS heap after a
     * shutdown/re-init cycle.  Marking them before their init functions
     * re-assign and re-register would set mark bits at arbitrary offsets
     * in the fresh arena. */
    n_global_roots = 0;

    /* Reset the UNCONDITIONALLY-marked shared Lisp globals for the same
     * reason (the documented "stale static tables on re-init" bug class).
     * gc_mark marks these directly — not through the resettable registry
     * above — so after a heap re-init they still hold offsets from the
     * PREVIOUS arena.  Marking such a stale offset in the fresh arena
     * stamps a mark bit into the middle of whatever live object now spans
     * it (the classic 0x800000-in-a-cdr corruption), as soon as the new
     * bump front grows past the old offset so it passes validation.
     * Observed deterministically from the C unit tests' shutdown/re-init
     * pattern; see shared_tables_reset_on_heap_reinit in
     * tests/test_gc_threaded.c.  Every value here is invalid by
     * definition — the arena they pointed into was just freed. */
    macro_table = CL_NIL;
    setf_table = CL_NIL;
    setf_fn_table = CL_NIL;
    setf_expander_table = CL_NIL;
    type_table = CL_NIL;
    compiler_macro_table = CL_NIL;
    cl_clos_class_table = CL_NIL;
    struct_table = CL_NIL;
    condition_hierarchy = CL_NIL;
    condition_slot_table = CL_NIL;
    condition_default_initargs = CL_NIL;
    condition_slot_initforms = CL_NIL;
    cl_package_registry = CL_NIL;
    {
        extern CL_Obj *cl_main_thread_lisp_obj_ptr(void);
        *cl_main_thread_lisp_obj_ptr() = CL_NIL;
    }
    /* Readtable pool user macro closures (marked per allocated entry) */
    {
        int rt, ch;
        for (rt = 0; rt < CL_RT_POOL_SIZE; rt++) {
            if (!(cl_readtable_alloc_mask & (1u << rt)))
                continue;
            for (ch = 0; ch < CL_RT_CHARS; ch++) {
                cl_readtable_pool[rt].macro_fn[ch] = CL_NIL;
                cl_readtable_pool[rt].dispatch_fn[ch] = CL_NIL;
            }
        }
    }

    /* Initialize allocation mutex */
    platform_mutex_init(&alloc_mutex);
    return 0;
}

void cl_mem_init(uint32_t heap_size)
{
    if (cl_mem_try_init(heap_size) != 0)
        exit(1);
}

/* --- Heap-image adoption (image.c; see specs/image-save-load.md) --- */

void cl_mem_adopt_image_begin(uint32_t bump)
{
    /* The payload replaces the arena under every chunk cursor (a thread
     * that allocated multi-threaded before the adoption would otherwise
     * keep cutting inside the restored objects). */
    gc_cursor_drop_all();
    cl_heap.bump = bump;
    cl_heap.free_list = 0;
    /* The restored payload is dense (the writer compacts before dumping):
     * everything in [CL_ALIGN, bump) is live allocation. */
    cl_heap.total_allocated = bump - CL_ALIGN;
    cl_heap.total_consed = bump - CL_ALIGN;
    gc_reset_transient_state();
    gc_last_compact_cycle = 0xFFFFFFFF;
    gcb_adopt_image_begin();
}

#if defined(DEBUG_GC) || defined(DEBUG_GC_STRESS)
/* The arena iterator (mem.h) met a header it cannot step over.  A release
 * build ends the walk quietly — the pass truncates, as the hand-written
 * loops used to; the debug builds name the header and abort, since every
 * block past it is invisible to the collector and the next pass would
 * free or slide garbage. */
void gc_walk_broken(uint32_t off, uint32_t hdr, uint32_t end)
{
    char buf[160];
    snprintf(buf, sizeof(buf),
             "FATAL: GC arena walk broken at 0x%08lx: header 0x%08lx "
             "(size %lu, type %u), walk end 0x%08lx\n",
             (unsigned long)off, (unsigned long)hdr,
             (unsigned long)(hdr & CL_HDR_SIZE_MASK),
             (unsigned)((hdr >> CL_HDR_TYPE_SHIFT) & 0xFF),
             (unsigned long)end);
    platform_write_string(buf);
    platform_flush_output();
    abort();
}
#endif

void cl_mem_adopt_image_finish(void)
{
    gcb_adopt_image_finish();
}

/* Reset static GC bookkeeping on heap re-initialization — see the call
 * in cl_mem_init.  (Defined here, below all the statics it touches.) */
static void gc_reset_transient_state(void)
{
    gc_compact_pending = 0;
    gc_mark_overflow = 0;
    gc_mark_grow_failed = 0;
    gc_fwd_fail_bump = 0;
    gc_jit_pin_oom = 0;
    jit_pinned_count = 0;
    jit_scan_free_count = 0;
    jit_scan_free_valid = 0;
}

/* Hand back the off-heap buffers owned by objects that are STILL LIVE when
 * the runtime shuts down, and return the total bytes released.
 *
 * Freeing the arena releases only the arena.  Every CL_Bytecode in it owns
 * up to eight platform_alloc'd satellites (body, constants, &key arrays,
 * line map, JIT code + relocs) that live OUTSIDE the arena, and the GC only
 * ever reclaims those for objects it has proved dead.  At exit the whole
 * boot image — every function in boot.fasl, clos.fasl and anything the user
 * loaded — is by definition still live, so none of it is ever swept and all
 * of it was leaked.
 *
 * On a host OS the kernel reclaims the process's address space and nobody
 * notices.  On AmigaOS there is no such backstop: AllocVec'd memory not
 * returned before the process exits is gone from the system pool until the
 * next reboot, so every clamiga run permanently ate a few megabytes of Fast
 * RAM.  That is the leak this walk closes.
 *
 * The walk is the arena iterator's: block by block from the first slot to
 * the bump pointer.  Free-list blocks are walked over safely — a free
 * block's header word is its raw size, which reads back as an unmarked
 * TYPE_CONS, so it never matches TYPE_BYTECODE.
 *
 * Only memory is released here.  OS handles (stream fds, lock mutexes,
 * thread handles) are deliberately left to the dedicated shutdown paths that
 * already ran — cl_stream_shutdown and cl_thread_shutdown — so this walk
 * cannot double-close a handle or print a spurious "dropped without CLOSE"
 * warning for a stream the caller never had to close. */
static uint32_t gc_release_all_offheap(void)
{
    GcWalk w;
    uint32_t off, size;
    uint8_t type;
    uint32_t freed = 0;

    gc_walk_begin(&w);
    while (gc_walk_next(&w, &off, &size, &type)) {
        if (type == TYPE_BYTECODE)
            freed += bytecode_release_offheap((CL_Bytecode *)(cl_heap.arena + off));
    }
    return freed;
}

void cl_mem_shutdown(void)
{
    /* The backend first: it drops any page protection and detaches its
     * fault handler before anything below touches the arena, and hands
     * its side tables back. */
    gcb_shutdown();

    /* Before the arena goes away, hand back what hangs off it.  Must run
     * after gcb_shutdown() above (old-space pages are read-protected until
     * then) and before the arena is released below. */
    if (cl_heap.arena) {
        uint32_t reclaimed = gc_bc_offheap_bytes;   /* freed by the GC so far */
        uint32_t offheap = gc_release_all_offheap();
        if (cl_mem_diag) {
            char buf[192];
            snprintf(buf, sizeof(buf),
                     "[mem] off-heap bytecode payload: %lu bytes reclaimed by "
                     "GC during the run, %lu released at shutdown\n"
                     "[mem] arena: %lu bytes\n",
                     (unsigned long)reclaimed, (unsigned long)offheap,
                     (unsigned long)cl_heap.arena_size);
            platform_write_string(buf);
        }
    }

    if (cl_heap.arena) {
        gcb_arena_free(cl_heap.arena, cl_heap.arena_size);
        cl_heap.arena = NULL;
    }
    gc_mark_stack_release();
    if (jit_scan_cand_buf) {
        platform_free(jit_scan_cand_buf);
        jit_scan_cand_buf = NULL;
        jit_scan_cand_cap = 0;
    }
    if (jit_pinned) {
        platform_free(jit_pinned);
        jit_pinned = NULL;
        jit_pinned_cap = 0;
        jit_pinned_count = 0;
    }
    if (jit_scan_free_snap) {
        platform_free(jit_scan_free_snap);
        jit_scan_free_snap = NULL;
        jit_scan_free_cap = 0;
        jit_scan_free_count = 0;
        jit_scan_free_valid = 0;
    }
    if (gc_root_slot_buf) {
        platform_free(gc_root_slot_buf);
        gc_root_slot_buf = NULL;
        gc_root_slot_cap = 0;
    }
    if (alloc_mutex) {
        platform_mutex_destroy(alloc_mutex);
        alloc_mutex = NULL;
    }
}

/* Wrap-safe bump-fit check.  The naive `bump + size <= arena_size` wraps
 * in uint32 when bump sits near the end of a ~4GB arena (size is capped
 * at CL_HDR_SIZE_MASK by cl_alloc, but bump + 8MB can still exceed
 * UINT32_MAX for arena sizes above ~4GB-8MB) — the wrapped sum passes
 * the check and hands out a pointer past the arena.  Non-static so the
 * unit test can exercise the wrap arithmetic without a 4GB heap. */
int cl_bump_fits(uint32_t bump, uint32_t size, uint32_t arena_size)
{
    return size <= arena_size && bump <= arena_size - size;
}

/* Signal a storage error without allocating (safe when heap is exhausted
 * or corrupted).  Uses direct longjmp, bypassing cl_error() which would
 * try to allocate condition objects. */
void cl_storage_error(const char *fmt, ...)
{
    va_list ap;
    cl_error_code = CL_ERR_STORAGE;
    va_start(ap, fmt);
    vsnprintf(cl_error_msg, sizeof(cl_error_msg), fmt, ap);
    va_end(ap);
    /* Skip cl_capture_backtrace() — VM frames may reference corrupted heap */
    cl_backtrace_buf[0] = '\0';
    cl_gc_reset_roots();
    cl_vm.sp = 0;
    cl_vm.fp = 0;
    cl_nlx_top = 0;
    cl_saved_pending_top = 0;
    cl_pending_throw = 0;
    cl_dynbind_restore_to(0);
    cl_handler_top = 0;
    cl_restart_top = 0;
    /* Drop any unwind-protect value-save records the abandoned cleanups
     * held — same reasoning as the resets above, and as the mv_save_top
     * reset error.c's frame unwind now performs (Tier-4 phase 2). */
    CT->mv_save_top = 0;
    if (cl_error_frame_top > 0) {
        /* Don't decrement here — CL_UNCATCH at the catch site pops */
        longjmp(cl_error_frames[cl_error_frame_top - 1].buf, CL_ERR_STORAGE);
    }
    /* No error frame — fatal */
    platform_write_string("FATAL: ");
    platform_write_string(cl_error_msg);
    platform_write_string("\n");
    platform_flush_output();
    exit(1);
}

void *cl_alloc(uint8_t type, uint32_t size)
{
    void *ptr;
    CL_Thread *t;
    int multi = (cl_thread_count > 1);

    /* GC safepoint before allocation — if another thread initiated GC,
     * we must stop here before touching the heap */
    if (multi) CL_SAFEPOINT();

#ifdef DEBUG_GC_STRESS
    /* Force a compacting (moving) GC before every allocation so that any
     * CL_Obj C local held unprotected across an allocating call is reliably
     * corrupted (relocated out from under the stale local).  Single-thread
     * only; gated behind a build flag — compiles to nothing normally.
     *
     * NOTE: the `!multi` gate means the chunk cursor (CL_TLAB, multi-
     * threaded only) never runs under gc-stress — the two are mutually
     * exclusive by construction.  Chunk coverage instead comes from
     * tlab_mixed_sizes_vs_concurrent_compaction in
     * tests/test_gc_threaded.c, which hammers concurrent compaction
     * against live chunk cutting without the per-alloc stress hook.  The
     * front cursor IS covered: every forced compaction drops it, so each
     * stressed allocation refills and cuts it afresh. */
    if (!multi && cl_gc_stress_ready && !cl_gc_stress_active) {
        cl_gc_stress_active = 1;
        {
            /* Bisection controls (read once):
             *   CLAMIGA_GC_STRESS_SKIP=N — only compact on alloc index > N
             *     (suffix mode; binary-search the corrupting alloc).
             *   CLAMIGA_GC_STRESS_AT=N   — compact ONLY at alloc index N and
             *     dump a C backtrace there (pin the exact corrupting site). */
            static long s_skip = -2, s_at = -2;
            static long s_count = 0;
            int do_compact;
            if (s_skip == -2) {
                const char *e1 = getenv("CLAMIGA_GC_STRESS_SKIP");
                const char *e2 = getenv("CLAMIGA_GC_STRESS_AT");
                s_skip = e1 ? atol(e1) : -1;
                s_at   = e2 ? atol(e2) : -1;
            }
            s_count++;
            if (s_at >= 0)        do_compact = (s_count == s_at);
            else if (s_skip >= 0) do_compact = (s_count > s_skip);
            else                  do_compact = 1;
            if (do_compact) {
                if (s_at >= 0) {
                    extern void cl_capture_backtrace(void);
                    fprintf(stderr, "[GCSTRESS-AT] compacting at alloc #%ld\n", s_count);
                    cl_capture_backtrace();
                    fprintf(stderr, "%s", cl_backtrace_buf);
#ifdef DEBUG_GC_STRESS_CBT
                    {
                        void *cbt[32];
                        int ncbt = backtrace(cbt, 32);
                        fprintf(stderr, "[GCSTRESS-AT] C backtrace:\n");
                        backtrace_symbols_fd(cbt, ncbt, 2);
                    }
#endif
                    fflush(stderr);
                }
                GCB_STRESS_COLLECT();
            }
        }
        cl_gc_stress_active = 0;
    }
#endif

    size = align_up(size);
    if (size < CL_MIN_ALLOC_SIZE)
        size = CL_MIN_ALLOC_SIZE;

    /* Guard: size must fit the 23-bit header size field.  This MUST run
     * before alloc_from_bump: the old post-allocation guard fired after
     * the bump pointer had already advanced, longjmp'ing out via
     * cl_storage_error and leaving a headerless region inside the walked
     * bump front — every later arena walk (sweep, mark-overflow rescan,
     * forwarding, slide) desynced there, silently corrupting the heap.
     * Reachable from pure Lisp: (make-array 3000000) passes the element-
     * count check but requests ~12MB of bytes. */
    if (size > CL_HDR_SIZE_MASK)
        cl_storage_error("Allocation too large for header: %u bytes (max %u)",
                         (unsigned)size, (unsigned)CL_HDR_SIZE_MASK);

    /* Fast path: cut from the backend's cursor — no lock, no call
     * (gcb_alloc_fast, gc_backend.h).  A miss (the cursor is spent, or
     * there is no chunk) goes to the backend's slow path, which refills,
     * collects and accounts. */
    ptr = gcb_alloc_fast(&size, multi, &t);
    if (!ptr)
        ptr = gcb_alloc_slow(t, &size, multi);

    /* Initialize: zero entire block, then set header.
     * Zeroing prevents stale data in padding/trailing bytes from being
     * misinterpreted by GC (e.g. closure padding read as upvalue slots)
     * or FASL serializer (traverses object graph by following CL_Obj fields). */
    alloc_zero(ptr, size);
    ((CL_Header *)ptr)->header = GCB_MAKE_HDR(type, size);   /* born unmarked */

    GCB_ON_ALLOC_FINALIZABLE((uint32_t)((uint8_t *)ptr - cl_heap.arena),
                             type, multi);

    return ptr;
}

/* --- Collection entry points (backend-neutral wrappers) --- */

uint64_t gc_stop_world_timed(int multithread)
{
    uint64_t t0, t1;
    /* Every collection enters here, and every one invalidates all JIT call
     * sites: a site's cached callee is neither a root nor relocated, so the
     * bump is what keeps compaction (stale offsets) and sweep-then-reuse
     * (ABA) from ever reaching a hit.  Bumped with the world stopped, so no
     * peer can fill a site from pre-collection offsets after it. */
    if (!multithread) {
        cl_call_gen_bump("gc");
        return platform_time_us();
    }
    t0 = platform_time_us();
    cl_gc_stop_the_world();
    cl_call_gen_bump("gc");
    t1 = platform_time_us();
    gc_time_stw_us += t1 - t0;
    gc_stw_stops++;
    if (t1 - t0 > gc_time_stw_max_us)
        gc_time_stw_max_us = t1 - t0;
    return t1;
}

void cl_gc(void)
{
    gcb_collect(GCB_WHY_EXPLICIT);
}

void cl_gc_reclaim_young(void)
{
    gcb_collect(GCB_WHY_RECLAIM_YOUNG);
}

/* --- Convenience allocators --- */

CL_Obj cl_cons(CL_Obj car, CL_Obj cdr)
{
    CL_Cons *c;

    /* Protect args from GC during allocation */
    CL_GC_PROTECT(car);
    CL_GC_PROTECT(cdr);

    c = (CL_Cons *)cl_alloc(TYPE_CONS, sizeof(CL_Cons));
    CL_GC_UNPROTECT(2);

    if (!c) return CL_NIL;
    c->car = car;
    c->cdr = cdr;
    return CL_PTR_TO_OBJ(c);
}

/* Each list is built back to front, one cl_cons per statement, so every
 * element is read after the previous allocation has finished (and may have
 * moved it); the elements not yet consed stay rooted until then. */
CL_Obj cl_list_star3(CL_Obj a, CL_Obj b, CL_Obj tail)
{
    CL_Obj r;
    CL_GC_PROTECT(a);
    r = cl_cons(b, tail);
    r = cl_cons(a, r);
    CL_GC_UNPROTECT(1);
    return r;
}

CL_Obj cl_list2(CL_Obj a, CL_Obj b)
{
    return cl_list_star3(a, b, CL_NIL);
}

CL_Obj cl_list3(CL_Obj a, CL_Obj b, CL_Obj c)
{
    CL_Obj r;
    CL_GC_PROTECT(a);
    CL_GC_PROTECT(b);
    r = cl_cons(c, CL_NIL);
    r = cl_cons(b, r);
    r = cl_cons(a, r);
    CL_GC_UNPROTECT(2);
    return r;
}

CL_Obj cl_list4(CL_Obj a, CL_Obj b, CL_Obj c, CL_Obj d)
{
    CL_Obj r;
    CL_GC_PROTECT(a);
    CL_GC_PROTECT(b);
    CL_GC_PROTECT(c);
    r = cl_cons(d, CL_NIL);
    r = cl_cons(c, r);
    r = cl_cons(b, r);
    r = cl_cons(a, r);
    CL_GC_UNPROTECT(3);
    return r;
}

/* Cons whose CAR and CDR are ALREADY GC-rooted by the caller — passed by
 * ADDRESS, not by value.
 *
 * cl_cons has to push both arguments onto the GC root stack because its
 * by-value parameters are invisible to a compaction triggered inside
 * cl_alloc.  The VM's own cons sites (OP_CONS, OP_LIST, &rest building)
 * do not need that: their operands live in GC-rooted storage — VM stack
 * slots, which the collector forwards in place, or a local the caller has
 * already CL_GC_PROTECTed.  Taking the addresses lets this read the
 * operands AFTER the allocation, so a relocation during cl_alloc is
 * simply observed rather than needing two more root-stack entries per
 * cons (four stack writes and two bounds checks on the hottest allocation
 * path in the system).
 *
 * The caller must guarantee both locations are rooted and remain valid
 * across the call — the VM stack below sp, or a CL_GC_PROTECTed local.
 * Nothing here allocates after cl_alloc returns, so the raw pointer c is
 * stable for the two stores. */
CL_Obj cl_cons_rooted(const CL_Obj *car_ref, const CL_Obj *cdr_ref)
{
    CL_Cons *c = (CL_Cons *)cl_alloc(TYPE_CONS, sizeof(CL_Cons));
    if (!c) return CL_NIL;
    c->car = *car_ref;
    c->cdr = *cdr_ref;
    return CL_PTR_TO_OBJ(c);
}

CL_Obj cl_make_string(const char *str, uint32_t len)
{
    uint32_t alloc_size;
    CL_String *s;
    char stack_buf[256];
    char *safe_str = NULL;

    /* Cap BEFORE computing alloc_size — len near UINT32_MAX wraps the
     * `+ len + 1` arithmetic past cl_alloc's byte-size guard (see the
     * CL_MAX_* block in mem.h). */
    if (len > CL_MAX_STRING_CHARS)
        cl_storage_error("MAKE-STRING: length %u exceeds the maximum "
                         "heap object size (max %u characters)",
                         (unsigned)len, (unsigned)CL_MAX_STRING_CHARS);
    alloc_size = sizeof(CL_String) + len + 1;

    /* If str points into the arena, copy to a safe buffer first.
     * cl_alloc below may trigger GC compaction which moves arena objects,
     * making the original pointer stale. */
    if (str && (const uint8_t *)str >= cl_heap.arena &&
        (const uint8_t *)str < cl_heap.arena + cl_heap.arena_size) {
        if (len < sizeof(stack_buf)) {
            memcpy(stack_buf, str, len);
            stack_buf[len] = '\0';
            safe_str = stack_buf;
        } else {
            safe_str = (char *)platform_alloc(len + 1);
            if (!safe_str)
                /* MUST NOT fall back to the arena pointer: cl_alloc below
                 * can compact, leaving `str` pointing at moved/freed bytes
                 * — the memcpy would then build a silent garbage string.
                 * Fail loudly instead. */
                cl_storage_error("MAKE-STRING: out of C memory copying a "
                                 "%u-char arena-resident source", (unsigned)len);
            memcpy(safe_str, str, len);
            safe_str[len] = '\0';
        }
        str = safe_str ? safe_str : str;
    }

    s = (CL_String *)cl_alloc(TYPE_STRING, alloc_size);
    if (!s) {
        if (safe_str && safe_str != stack_buf) platform_free(safe_str);
        return CL_NIL;
    }
    s->length = len;
    if (str)
        memcpy(s->data, str, len);
    else
        memset(s->data, 0, len);
    s->data[len] = '\0';

    if (safe_str && safe_str != stack_buf) platform_free(safe_str);
    return CL_PTR_TO_OBJ(s);
}

#ifdef CL_WIDE_STRINGS
CL_Obj cl_make_wide_string(const uint32_t *chars, uint32_t len)
{
    uint32_t alloc_size;
    CL_WideString *s;
    uint32_t stack_buf[64];
    uint32_t *safe_chars = NULL;

    /* Cap BEFORE computing alloc_size — len >= 2^30 wraps the `* 4`
     * arithmetic past cl_alloc's byte-size guard (see mem.h). */
    if (len > CL_MAX_WIDE_STRING_CHARS)
        cl_storage_error("MAKE-WIDE-STRING: length %u exceeds the maximum "
                         "heap object size (max %u wide characters)",
                         (unsigned)len, (unsigned)CL_MAX_WIDE_STRING_CHARS);
    alloc_size = sizeof(CL_WideString) + len * sizeof(uint32_t);

    /* If chars points into the arena, copy to a safe buffer first.
     * cl_alloc below may trigger GC compaction which moves arena objects,
     * making the original pointer stale (same guard as cl_make_string). */
    if (chars && (const uint8_t *)chars >= cl_heap.arena &&
        (const uint8_t *)chars < cl_heap.arena + cl_heap.arena_size) {
        if (len <= sizeof(stack_buf) / sizeof(stack_buf[0])) {
            memcpy(stack_buf, chars, len * sizeof(uint32_t));
            safe_chars = stack_buf;
        } else {
            safe_chars = (uint32_t *)platform_alloc(len * sizeof(uint32_t));
            if (!safe_chars)
                /* Same rule as cl_make_string: never keep the arena
                 * pointer across the allocating call below. */
                cl_storage_error("MAKE-WIDE-STRING: out of C memory copying a "
                                 "%u-char arena-resident wide source",
                                 (unsigned)len);
            memcpy(safe_chars, chars, len * sizeof(uint32_t));
        }
        chars = safe_chars ? safe_chars : chars;
    }

    s = (CL_WideString *)cl_alloc(TYPE_WIDE_STRING, alloc_size);
    if (!s) {
        if (safe_chars && safe_chars != stack_buf) platform_free(safe_chars);
        return CL_NIL;
    }
    s->length = len;
    if (chars)
        memcpy(s->data, chars, len * sizeof(uint32_t));
    else
        memset(s->data, 0, len * sizeof(uint32_t));

    if (safe_chars && safe_chars != stack_buf) platform_free(safe_chars);
    return CL_PTR_TO_OBJ(s);
}
#endif

CL_Obj cl_make_symbol(CL_Obj name)
{
    CL_Symbol *sym;

    CL_GC_PROTECT(name);
    sym = (CL_Symbol *)cl_alloc(TYPE_SYMBOL, sizeof(CL_Symbol));
    CL_GC_UNPROTECT(1);

    if (!sym) return CL_NIL;
    sym->name = name;
    sym->value = CL_UNBOUND;
    sym->function = CL_UNBOUND;   /* symfn-raw: fresh, nothing caches it yet */
    sym->plist = CL_NIL;
    sym->package = CL_NIL;
    sym->hash = 0;
    sym->flags = 0;
    return CL_PTR_TO_OBJ(sym);
}

/* See mem.h.  Starts at 1: 0 is the "never filled" site generation. */
volatile uint32_t cl_call_gen = 1;

void cl_call_gen_bump(const char *why)
{
    if (platform_atomic_inc(&cl_call_gen) == 0)
        platform_atomic_inc(&cl_call_gen);
#ifdef DEBUG_JIT
    {
        char buf[96];
        snprintf(buf, sizeof(buf), "JIT: call-gen -> %lu (%s)\n",
                 (unsigned long)cl_call_gen, why);
        platform_write_string(buf);
    }
#else
    (void)why;
#endif
}

void cl_symbol_set_function(CL_Obj sym, CL_Obj fn)
{
    /* Cell first, bump second: a miss-path fill that read the generation
     * before the bump records the old one and misses again (specs/
     * jit-direct-calls.md §5). */
    ((CL_Symbol *)CL_OBJ_TO_PTR(sym))->function = fn;   /* symfn-raw: the funnel */
    cl_call_gen_bump("function cell");
}

CL_Obj cl_make_function(CL_CFunc func, CL_Obj name, int min_args, int max_args)
{
    CL_Function *f;

    CL_GC_PROTECT(name);
    f = (CL_Function *)cl_alloc(TYPE_FUNCTION, sizeof(CL_Function));
    CL_GC_UNPROTECT(1);

    if (!f) return CL_NIL;
    f->func = func;
    f->name = name;
    f->min_args = min_args;
    f->max_args = max_args;
    return CL_PTR_TO_OBJ(f);
}

CL_Obj cl_make_vector(uint32_t length)
{
    uint32_t alloc_size;
    CL_Vector *v;
    /* Cap BEFORE computing alloc_size — length >= 2^30 wraps the `* 4`
     * arithmetic past cl_alloc's byte-size guard (see mem.h).  Reachable
     * with corrupted FASL vector lengths (raw u32 from disk). */
    if (length > CL_MAX_VECTOR_ELTS)
        cl_storage_error("MAKE-VECTOR: length %u exceeds the maximum "
                         "heap object size (max %u elements)",
                         (unsigned)length, (unsigned)CL_MAX_VECTOR_ELTS);
    alloc_size = sizeof(CL_Vector) + length * sizeof(CL_Obj);
    v = (CL_Vector *)cl_alloc(TYPE_VECTOR, alloc_size);
    if (!v) return CL_NIL;
    v->length = length;
    v->fill_pointer = CL_NO_FILL_POINTER;
    v->flags = 0;
    v->rank = 0;
    v->elt_type = CL_VEC_ELT_T;
    v->_reserved = 0;
    /* data[] already zeroed (= CL_NIL) by cl_alloc */
    return CL_PTR_TO_OBJ(v);
}

CL_Obj cl_make_array(uint32_t total, uint8_t rank, uint32_t *dims,
                     uint8_t flags, uint32_t fill_ptr)
{
    /* For multi-dim (rank>1): store dimensions in data[0..rank-1], elements at data[rank..] */
    uint32_t n_data;
    uint32_t alloc_size;
    CL_Vector *v;
    /* Cap BEFORE computing n_data/alloc_size — see cl_make_vector. */
    if (total > CL_MAX_VECTOR_ELTS ||
        (rank > 1 && (uint32_t)rank > CL_MAX_VECTOR_ELTS - total))
        cl_storage_error("MAKE-ARRAY: total size %u (rank %u) exceeds the "
                         "maximum heap object size (max %u elements)",
                         (unsigned)total, (unsigned)rank,
                         (unsigned)CL_MAX_VECTOR_ELTS);
    n_data = (rank > 1) ? (uint32_t)rank + total : total;
    /* Adjustable vectors need at least 2 data slots for displacement:
       data[0] = backing vector, data[1] = displaced-index-offset */
    if ((flags & (CL_VEC_FLAG_ADJUSTABLE | CL_VEC_FLAG_FILL_POINTER)) && n_data < 2)
        n_data = 2;
    alloc_size = sizeof(CL_Vector) + n_data * sizeof(CL_Obj);
    v = (CL_Vector *)cl_alloc(TYPE_VECTOR, alloc_size);
    if (!v) return CL_NIL;
    v->length = total;
    v->fill_pointer = fill_ptr;
    v->flags = flags;
    v->rank = rank;
    v->elt_type = CL_VEC_ELT_T;
    v->_reserved = 0;
    /* Store dimensions as fixnums for multi-dim */
    if (rank > 1 && dims) {
        uint8_t i;
        for (i = 0; i < rank; i++)
            v->data[i] = CL_MAKE_FIXNUM((int32_t)dims[i]);
    }
    /* Element slots already zeroed (= CL_NIL) by cl_alloc */
    return CL_PTR_TO_OBJ(v);
}

/* Follow displacement chain to get the actual data pointer.
   Accumulates displaced-index-offset stored in data[1] at each level. */
CL_Obj *cl_vector_data_fn(CL_Vector *v)
{
    uint32_t offset = 0;
    while (v->flags & CL_VEC_FLAG_DISPLACED) {
        /* Backing ref at data[base], displaced-index-offset (fixnum) at
         * data[base+1].  base accounts for multi-dim dimension storage. */
        uint32_t base = CL_DISP_BASE_IDX(v);
        if (CL_FIXNUM_P(v->data[base + 1]))
            offset += (uint32_t)CL_FIXNUM_VAL(v->data[base + 1]);
        v = (CL_Vector *)CL_OBJ_TO_PTR(v->data[base]);
    }
    {
        CL_Obj *base = v->rank > 1 ? &v->data[v->rank] : v->data;
        return base + offset;
    }
}

CL_Obj cl_make_hashtable(uint32_t bucket_count, uint32_t test)
{
    uint32_t alloc_size;
    CL_Hashtable *ht;

    /* Cap BEFORE the power-of-two round-up: counts > 2^31 make the
     * round-up loop below spin forever (p <<= 1 wraps to 0), and large
     * counts wrap alloc_size past cl_alloc's byte-size guard (mem.h). */
    if (bucket_count > CL_MAX_HT_BUCKETS)
        cl_storage_error("MAKE-HASH-TABLE: %u buckets exceeds the maximum "
                         "heap object size (max %u)",
                         (unsigned)bucket_count, (unsigned)CL_MAX_HT_BUCKETS);

    /* Round up to power of 2 for fast bitmask indexing (avoids division) */
    if (bucket_count < 2) bucket_count = 2;
    {
        uint32_t p = 1;
        while (p < bucket_count) p <<= 1;
        bucket_count = p;
    }

    alloc_size = sizeof(CL_Hashtable) + bucket_count * sizeof(CL_Obj);
    ht = (CL_Hashtable *)cl_alloc(TYPE_HASHTABLE, alloc_size);
    if (!ht) return CL_NIL;
    ht->test = test;
    ht->count = 0;
    ht->bucket_count = bucket_count;
    ht->flags = 0;
    ht->bucket_vec = CL_NIL;
    /* buckets[] already zeroed (= CL_NIL) by cl_alloc */
    return CL_PTR_TO_OBJ(ht);
}

CL_Obj cl_make_condition(CL_Obj type_name, CL_Obj slots, CL_Obj report_string)
{
    CL_Condition *cond;

    CL_GC_PROTECT(type_name);
    CL_GC_PROTECT(slots);
    CL_GC_PROTECT(report_string);

    cond = (CL_Condition *)cl_alloc(TYPE_CONDITION, sizeof(CL_Condition));
    CL_GC_UNPROTECT(3);

    if (!cond) return CL_NIL;
    cond->type_name = type_name;
    cond->slots = slots;
    cond->report_string = report_string;
    return CL_PTR_TO_OBJ(cond);
}

CL_Obj cl_make_restart(CL_Obj name, CL_Obj function, CL_Obj report,
                       CL_Obj interactive, CL_Obj test, CL_Obj tag)
{
    CL_Restart *r;

    CL_GC_PROTECT(name);
    CL_GC_PROTECT(function);
    CL_GC_PROTECT(report);
    CL_GC_PROTECT(interactive);
    CL_GC_PROTECT(test);
    CL_GC_PROTECT(tag);

    r = (CL_Restart *)cl_alloc(TYPE_RESTART, sizeof(CL_Restart));
    CL_GC_UNPROTECT(6);

    if (!r) return CL_NIL;
    r->name = name;
    r->function = function;   /* symfn-raw: a restart, not a symbol */
    r->report = report;
    r->interactive = interactive;
    r->test = test;
    r->tag = tag;
    return CL_PTR_TO_OBJ(r);
}

CL_Obj cl_make_struct(CL_Obj type_name, uint32_t n_slots)
{
    uint32_t alloc_size;
    CL_Struct *st;

    /* Cap BEFORE computing alloc_size — see cl_make_vector.  Reachable
     * with corrupted FASL struct slot counts (raw u32 from disk). */
    if (n_slots > CL_MAX_STRUCT_SLOTS)
        cl_storage_error("MAKE-STRUCT: %u slots exceeds the maximum "
                         "heap object size (max %u)",
                         (unsigned)n_slots, (unsigned)CL_MAX_STRUCT_SLOTS);
    alloc_size = sizeof(CL_Struct) + n_slots * sizeof(CL_Obj);

    CL_GC_PROTECT(type_name);
    st = (CL_Struct *)cl_alloc(TYPE_STRUCT, alloc_size);
    CL_GC_UNPROTECT(1);

    if (!st) return CL_NIL;
    st->type_desc = type_name;
    st->n_slots = n_slots;
    /* slots[] already zeroed (= CL_NIL) by cl_alloc */
    return CL_PTR_TO_OBJ(st);
}

CL_Obj cl_make_bignum(uint32_t n_limbs, uint32_t sign)
{
    uint32_t alloc_size;
    CL_Bignum *bn;
    /* Cap BEFORE computing alloc_size — see cl_make_vector. */
    if (n_limbs > CL_MAX_BIGNUM_LIMBS)
        cl_storage_error("MAKE-BIGNUM: %u limbs exceeds the maximum "
                         "heap object size (max %u)",
                         (unsigned)n_limbs, (unsigned)CL_MAX_BIGNUM_LIMBS);
    alloc_size = sizeof(CL_Bignum) + n_limbs * sizeof(uint16_t);
    bn = (CL_Bignum *)cl_alloc(TYPE_BIGNUM, alloc_size);
    if (!bn) return CL_NIL;
    bn->length = n_limbs;
    bn->sign = sign;
    /* limbs[] already zeroed by cl_alloc */
    return CL_PTR_TO_OBJ(bn);
}

CL_Obj cl_make_ratio(CL_Obj numerator, CL_Obj denominator)
{
    CL_Ratio *r;

    CL_GC_PROTECT(numerator);
    CL_GC_PROTECT(denominator);

    r = (CL_Ratio *)cl_alloc(TYPE_RATIO, sizeof(CL_Ratio));
    CL_GC_UNPROTECT(2);

    if (!r) return CL_NIL;
    r->numerator = numerator;
    r->denominator = denominator;
    return CL_PTR_TO_OBJ(r);
}

CL_Obj cl_make_complex(CL_Obj realpart, CL_Obj imagpart)
{
    CL_Complex *c;

    CL_GC_PROTECT(realpart);
    CL_GC_PROTECT(imagpart);

    c = (CL_Complex *)cl_alloc(TYPE_COMPLEX, sizeof(CL_Complex));
    CL_GC_UNPROTECT(2);

    if (!c) return CL_NIL;
    c->realpart = realpart;
    c->imagpart = imagpart;
    return CL_PTR_TO_OBJ(c);
}

CL_Obj cl_make_single_float(float value)
{
    CL_SingleFloat *sf = (CL_SingleFloat *)cl_alloc(TYPE_SINGLE_FLOAT,
                                                     sizeof(CL_SingleFloat));
    if (!sf) return CL_NIL;
    sf->value = value;
    return CL_PTR_TO_OBJ(sf);
}

CL_Obj cl_make_double_float(double value)
{
    CL_DoubleFloat *df = (CL_DoubleFloat *)cl_alloc(TYPE_DOUBLE_FLOAT,
                                                      sizeof(CL_DoubleFloat));
    if (!df) return CL_NIL;
    df->value = value;
    return CL_PTR_TO_OBJ(df);
}

CL_Obj cl_make_random_state(uint32_t seed)
{
    CL_RandomState *rs = (CL_RandomState *)cl_alloc(TYPE_RANDOM_STATE,
                                                      sizeof(CL_RandomState));
    if (!rs) return CL_NIL;
    /* Seed all 4 state words from seed using splitmix32-like mixing */
    {
        uint32_t z = seed;
        z = (z ^ (z >> 16)) * 0x45d9f3bU; z ^= z >> 16;
        rs->s[0] = z ? z : 1;
        z = (seed + 0x9e3779b9U);
        z = (z ^ (z >> 16)) * 0x45d9f3bU; z ^= z >> 16;
        rs->s[1] = z ? z : 1;
        z = (seed + 0x9e3779b9U * 2);
        z = (z ^ (z >> 16)) * 0x45d9f3bU; z ^= z >> 16;
        rs->s[2] = z ? z : 1;
        z = (seed + 0x9e3779b9U * 3);
        z = (z ^ (z >> 16)) * 0x45d9f3bU; z ^= z >> 16;
        rs->s[3] = z ? z : 1;
    }
    return CL_PTR_TO_OBJ(rs);
}

CL_Obj cl_make_bit_vector(uint32_t nbits)
{
    uint32_t nwords;
    uint32_t alloc_size;
    CL_BitVector *bv;
    /* Cap BEFORE CL_BV_WORDS — nbits near UINT32_MAX wraps the `+ 31`
     * arithmetic to a tiny word count (see mem.h). */
    if (nbits > CL_MAX_BV_BITS)
        cl_storage_error("MAKE-BIT-VECTOR: length %u exceeds the maximum "
                         "heap object size (max %u bits)",
                         (unsigned)nbits, (unsigned)CL_MAX_BV_BITS);
    nwords = CL_BV_WORDS(nbits);
    alloc_size = sizeof(CL_BitVector) + nwords * sizeof(uint32_t);
    bv = (CL_BitVector *)cl_alloc(TYPE_BIT_VECTOR, alloc_size);
    if (!bv) return CL_NIL;
    bv->length = nbits;
    bv->fill_pointer = CL_NO_FILL_POINTER;
    bv->flags = 0;
    bv->_pad[0] = bv->_pad[1] = bv->_pad[2] = 0;
    /* data[] already zeroed by cl_alloc */
    return CL_PTR_TO_OBJ(bv);
}

CL_Obj cl_make_byte_vector(uint32_t length, int is_signed, int elt_shift)
{
    uint32_t alloc_size;
    CL_ByteVector *bv;
    /* Cap BEFORE computing alloc_size — a length near UINT32_MAX would wrap
     * the `<< shift`/`+ sizeof` arithmetic past cl_alloc's byte-size guard
     * (see mem.h).  Reachable with corrupted FASL byte-vector lengths (raw
     * u32 from disk).  The cap is in BYTES, so a 16-bit vector holds half as
     * many elements. */
    if (length > (CL_MAX_BYTEVEC_BYTES >> elt_shift))
        cl_storage_error("MAKE-BYTE-VECTOR: length %u exceeds the maximum "
                         "heap object size (max %u elements)",
                         (unsigned)length,
                         (unsigned)(CL_MAX_BYTEVEC_BYTES >> elt_shift));
    alloc_size = sizeof(CL_ByteVector) + (length << elt_shift);
    bv = (CL_ByteVector *)cl_alloc(TYPE_BYTE_VECTOR, alloc_size);
    if (!bv) return CL_NIL;
    bv->length = length;
    bv->fill_pointer = CL_NO_FILL_POINTER;
    bv->flags = 0;
    bv->is_signed = (uint8_t)(is_signed ? 1 : 0);
    bv->elt_shift = (uint8_t)elt_shift;
    bv->_pad = 0;
    /* data[] already zeroed by cl_alloc */
    return CL_PTR_TO_OBJ(bv);
}

CL_Obj cl_make_cell(CL_Obj value)
{
    CL_Cell *cell;

    CL_GC_PROTECT(value);
    cell = (CL_Cell *)cl_alloc(TYPE_CELL, sizeof(CL_Cell));
    CL_GC_UNPROTECT(1);

    if (!cell) return CL_NIL;
    cell->value = value;
    return CL_PTR_TO_OBJ(cell);
}

CL_Obj cl_make_foreign_pointer(uint32_t address, uint32_t size, uint8_t flags)
{
    CL_ForeignPtr *fp = (CL_ForeignPtr *)cl_alloc(TYPE_FOREIGN_POINTER,
                                                    sizeof(CL_ForeignPtr));
    if (!fp) return CL_NIL;
    fp->address = address;
    fp->size = size;
    fp->flags = flags;
    fp->_pad[0] = fp->_pad[1] = fp->_pad[2] = 0;
    return CL_PTR_TO_OBJ(fp);
}

CL_Obj cl_make_ffi_stub(uint8_t kind, CL_Obj name, CL_Obj aux,
                        uint32_t a, int16_t b, uint8_t ctype)
{
    CL_FfiStub *st;
    CL_GC_PROTECT(name);
    CL_GC_PROTECT(aux);
    st = (CL_FfiStub *)cl_alloc(TYPE_FFI_STUB, sizeof(CL_FfiStub));
    CL_GC_UNPROTECT(2);
    if (!st) return CL_NIL;
    st->name = name;
    st->aux = aux;
    st->a = a;
    st->b = b;
    st->kind = kind;
    st->ctype = ctype;
    return CL_PTR_TO_OBJ(st);
}

CL_Obj cl_make_pathname(CL_Obj host, CL_Obj device, CL_Obj directory,
                        CL_Obj name, CL_Obj type, CL_Obj version)
{
    CL_Pathname *pn;

    CL_GC_PROTECT(host);
    CL_GC_PROTECT(device);
    CL_GC_PROTECT(directory);
    CL_GC_PROTECT(name);
    CL_GC_PROTECT(type);
    CL_GC_PROTECT(version);

    pn = (CL_Pathname *)cl_alloc(TYPE_PATHNAME, sizeof(CL_Pathname));
    CL_GC_UNPROTECT(6);

    if (!pn) return CL_NIL;
    pn->host = host;
    pn->device = device;
    pn->directory = directory;
    pn->name = name;
    pn->type = type;
    pn->version = version;
    return CL_PTR_TO_OBJ(pn);
}

/* --- GC Root Stack --- */

void cl_gc_push_root(CL_Obj *root)
{
    if (gc_root_count > CL_GC_ROOT_STACK_SIZE || gc_root_count < 0) {
        cl_fatal_diag("[GC-ROOT-BUG] push_root: gc_root_count=%d is CORRUPT (max=%d)\n",
                      gc_root_count, CL_GC_ROOT_STACK_SIZE);
        cl_capture_backtrace();
        cl_fatal_diag("%s", cl_backtrace_buf);
        abort();
    }
    if (gc_root_count < CL_GC_ROOT_STACK_SIZE) {
        gc_root_stack[gc_root_count++] = root;
    } else {
        cl_fatal_diag("FATAL: GC root stack overflow (%d/%d) - increase CL_GC_ROOT_STACK_SIZE\n",
                      gc_root_count, CL_GC_ROOT_STACK_SIZE);
        cl_capture_backtrace();
        cl_fatal_diag("%s", cl_backtrace_buf);
        abort();
    }
}

#ifdef DEBUG_GC
void cl_gc_push_root_dbg(CL_Obj *root, const char *file, int line)
{
    if (gc_root_count > CL_GC_ROOT_STACK_SIZE || gc_root_count < 0) {
        fprintf(stderr, "[GC-ROOT-BUG] push_root_dbg (%s:%d): gc_root_count=%d is CORRUPT (max=%d)\n",
                file, line, gc_root_count, CL_GC_ROOT_STACK_SIZE);
        cl_capture_backtrace();
        fprintf(stderr, "%s", cl_backtrace_buf);
        fflush(stderr);
        abort();
    }
    if (gc_root_count < CL_GC_ROOT_STACK_SIZE) {
        CT->gc_root_files[gc_root_count] = file;
        CT->gc_root_lines[gc_root_count] = line;
        gc_root_stack[gc_root_count++] = root;
    } else {
        fprintf(stderr, "FATAL: GC root stack overflow (%d/%d) at %s:%d - increase CL_GC_ROOT_STACK_SIZE\n",
                gc_root_count, CL_GC_ROOT_STACK_SIZE, file, line);
        /* Dump run-length-compressed pusher sites: a root LEAK (push without
         * pop, usually across a longjmp) shows up as one site repeated
         * hundreds of times.  Only sites recorded via CL_GC_PROTECT carry
         * file/line; plain cl_gc_push_root entries show the previous
         * occupant's stale site — treat those with suspicion. */
        {
            int di, dj;
            for (di = 0; di < CL_GC_ROOT_STACK_SIZE; di = dj) {
                const char *df = CT->gc_root_files[di];
                int dl = CT->gc_root_lines[di];
                for (dj = di + 1; dj < CL_GC_ROOT_STACK_SIZE &&
                         CT->gc_root_files[dj] == df &&
                         CT->gc_root_lines[dj] == dl; dj++)
                    ;
                fprintf(stderr, "  roots[%4d..%4d] %s:%d (x%d)\n",
                        di, dj - 1, df ? df : "?", dl, dj - di);
            }
        }
        cl_capture_backtrace();
        fprintf(stderr, "%s", cl_backtrace_buf);
        fflush(stderr);
        abort();
    }
}
#endif

void cl_gc_pop_roots(int n)
{
    if (gc_root_count > CL_GC_ROOT_STACK_SIZE || gc_root_count < 0) {
        cl_fatal_diag("[GC-ROOT-BUG] pop_roots(%d): gc_root_count=%d is CORRUPT (max=%d)\n",
                      n, gc_root_count, CL_GC_ROOT_STACK_SIZE);
        cl_capture_backtrace();
        cl_fatal_diag("%s", cl_backtrace_buf);
        abort();
    }
    gc_root_count -= n;
    if (gc_root_count < 0) {
        cl_fatal_diag("[GC-ROOT-BUG] pop_roots(%d): gc_root_count went negative -> %d\n",
                      n, gc_root_count);
        cl_capture_backtrace();
        cl_fatal_diag("%s", cl_backtrace_buf);
        abort();
    }
}

void cl_gc_reset_roots(void)
{
    gc_root_count = 0;
}

void cl_gc_mark_stack_stats(uint32_t *cap_entries, uint32_t *grows,
                            uint32_t *rescan_passes)
{
    if (cap_entries)   *cap_entries   = gc_mark_stack_cap;
    if (grows)         *grows         = gc_mark_stack_grows;
    if (rescan_passes) *rescan_passes = gc_mark_rescan_passes;
}

void cl_gc_mark_stack_set_test_limit(uint32_t max_entries)
{
    gc_mark_stack_test_limit = max_entries;
}

void cl_gc_offheap_stats(uint32_t *bytes, uint32_t *count)
{
    if (bytes) *bytes = gc_bc_offheap_bytes;
    if (count) *count = gc_bc_offheap_count;
}

void cl_gc_time_stats(uint64_t *stw_us, uint64_t *mark_us,
                      uint64_t *sweep_us, uint64_t *compact_us)
{
    if (stw_us)     *stw_us     = gc_time_stw_us;
    if (mark_us)    *mark_us    = gc_time_mark_us;
    if (sweep_us)   *sweep_us   = gc_time_sweep_us;
    if (compact_us) *compact_us = gc_time_compact_us;
}

void cl_gc_stw_stats(uint32_t *stops, uint64_t *max_us, uint32_t *epoch_skips)
{
    if (stops)       *stops       = gc_stw_stops;
    if (max_us)      *max_us      = gc_time_stw_max_us;
    if (epoch_skips) *epoch_skips = gc_epoch_skips;
}

void cl_heap_free_list_stats(uint32_t *blocks, uint32_t *bytes,
                             uint32_t *largest)
{
    uint32_t n = 0, total = 0, big = 0, off;
    int multi = (cl_thread_count > 1);
    if (multi && alloc_mutex) platform_mutex_lock(alloc_mutex);
    for (off = cl_heap.free_list; off; ) {
        const CL_FreeBlock *fb = (const CL_FreeBlock *)(cl_heap.arena + off);
        if (fb->size == 0 || off + fb->size > cl_heap.bump) break;  /* corrupt: stop */
        n++;
        total += fb->size;
        if (fb->size > big) big = fb->size;
        off = fb->next_offset;
    }
    if (multi && alloc_mutex) platform_mutex_unlock(alloc_mutex);
    if (blocks)  *blocks  = n;
    if (bytes)   *bytes   = total;
    if (largest) *largest = big;
}

void cl_mem_stats(void)
{
    char buf[256];
    sprintf(buf, "Heap: %lu/%lu bytes used, %lu free, %lu GC cycles\n",
            (unsigned long)cl_heap.total_allocated,
            (unsigned long)cl_heap.arena_size,
            (unsigned long)(cl_heap.arena_size - cl_heap.total_allocated),
            (unsigned long)cl_heap.gc_count);
    platform_write_string(buf);
    sprintf(buf, "GC mark stack: %lu entries (%lu grows, "
            "%lu overflow re-scan passes)\n",
            (unsigned long)gc_mark_stack_cap,
            (unsigned long)gc_mark_stack_grows,
            (unsigned long)gc_mark_rescan_passes);
    platform_write_string(buf);
    gcb_stats_report(platform_write_string);
}
