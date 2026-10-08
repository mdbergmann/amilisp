/* mem.c — heap core: arena, allocation fast path, constructors,
 * root stacks and the registered-root table, init/shutdown, stats.
 *
 * The collector itself lives in gc_trace.c (mark), gc_classic.c
 * (sweep, free list, cl_gc, the generational mode), gc_compact.c
 * (the sliding compactor), gc_jitscan.c (conservative JIT-stack
 * scan and its block-start index) and gc_verify.c (verifiers);
 * state they share is declared in gc_internal.h.
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
static void *alloc_from_bump(uint32_t size);
static void *alloc_from_free_list(uint32_t *sizep, uint32_t max_steps);

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

/* Fast-path bound on the first-fit free-list walk.  alloc_from_free_list is
 * O(list length); under a fragmented heap (many small blocks interspersed with
 * live objects) an allocation-heavy workload would degrade to O(n) per
 * allocation — O(n^2) overall — which manifests as a 100% CPU hang (e.g. the
 * bignum-heavy power-of-ten table built at FASL load time by jzon's
 * eisel-lemire).  cl_alloc probes only this many blocks on the fast path; if no
 * fit turns up it compacts (which resets the bump pointer and clears
 * fragmentation, restoring the O(1) bump path) rather than walking the whole
 * list.  A full unbounded walk is still used as the last resort before
 * declaring the heap exhausted, so a fit deep in the list is never missed.
 * See tests/test_alloc_freelist_perf.c for the regression test. */
#ifndef CL_FREELIST_PROBE_LIMIT
#define CL_FREELIST_PROBE_LIMIT 32
#endif

/* Track last GC cycle at which compaction ran — prevents infinite loops
 * when the heap is genuinely full (no fragmentation to reclaim). */
uint32_t gc_last_compact_cycle = 0xFFFFFFFF;

/* Number of sweep-only GCs (cl_gc) run since the last compaction.  A sweep
 * never resets the bump pointer, so once the bump is exhausted a run of
 * sweeps makes no lasting progress: each just re-coalesces the free list and
 * the next allocation sweeps again.  When this counter crosses
 * GC_SWEEPS_BEFORE_COMPACT (and the heap is not genuinely near-full) cl_alloc
 * forces a compaction, which resets the bump pointer and defragments — escaping
 * the "sweep-forever" regime.  Reset to 0 by cl_gc_compact. */
uint32_t gc_sweeps_since_compact = 0;


#ifdef DEBUG_GC_STRESS
/* GC-stress debugging: when ready, force a compaction before every single-
 * threaded allocation (see cl_alloc).  Enabled at runtime after boot so the
 * FASL/boot load isn't slowed to a crawl. */
int cl_gc_stress_ready = 0;
static int cl_gc_stress_active = 0;
#endif

/* Pending compaction flag — set when cl_alloc detects fragmentation,
 * cleared when compaction runs at a safe point.
 * Non-static: accessed by VM dispatch loop for safe-point checks. */
int gc_compact_pending = 0;

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
 * pessimization, not an improvement.  SIZE is CL_ALIGN-aligned here. */
static inline void alloc_zero(void *ptr, uint32_t size)
{
    if (size == CL_MIN_ALLOC_SIZE) {
        uint32_t *p = (uint32_t *)ptr;
        uint32_t n = CL_MIN_ALLOC_SIZE / 4;
        while (n--) *p++ = 0;
    } else {
        memset(ptr, 0, size);
    }
}

#ifdef CL_TLAB
/* ================================================================
 * TLAB — per-thread allocation buffers (multi-threaded fast path)
 *
 * With >1 thread registered, every cl_alloc used to serialize on
 * alloc_mutex — at actor-workload allocation rates the mutex (and its
 * cache-line ping-pong) dominates the per-message cost.  Instead, each
 * thread carves a private CHUNK from the shared bump front (or, when the
 * bump is exhausted, the free list) once per ~chunk of allocation, and
 * cuts objects from it with NO locking.
 *
 * Invariants (all load-bearing — see gc_sweep/gc_compute_forwarding):
 *  - The uncut remainder [tlab_cur, tlab_end) is formatted as a
 *    CL_FreeBlock at ALL times (size = remainder, not linked into the
 *    free list), so every linear arena walk parses the chunk.  A cut
 *    rewrites the remainder header BEFORE the object header/memset
 *    touches disjoint bytes below it.
 *  - Cuts never leave a remainder smaller than CL_MIN_ALLOC_SIZE: an
 *    undersized tail is absorbed into the object (same precedent as
 *    alloc_from_free_list's use-entire-block path).
 *  - Every GC cycle clears ALL threads' TLABs during stop-the-world
 *    (gc_tlab_reset_all).  Mandatory, not an optimization: gc_sweep
 *    rebuilds the free list from unmarked regions, so an active tail
 *    would be handed to another thread while the owner keeps cutting.
 *    Threads refill on their next allocation.
 *  - There is no safepoint between a cut and its header write, so a
 *    stop-the-world initiator can never walk a headerless cut (STW
 *    waits for this thread to park; it parks only at safepoints).
 *
 * Single-threaded execution (including the whole DEBUG_GC_STRESS suite)
 * never takes this path: cl_alloc gates it on `multi`.
 * ================================================================ */

/* Chunk size: configured once per heap in cl_mem_init (0 = disabled).
 * CLAMIGA_TLAB_CHUNK=<bytes> overrides; scaled down on small heaps so
 * N threads' chunks can't consume a test-sized arena. */
#ifndef CL_TLAB_CHUNK
#define CL_TLAB_CHUNK (32u * 1024u)
#endif
#define CL_TLAB_MIN_CHUNK 4096u
static uint32_t tlab_chunk_size = 0;
static uint32_t tlab_max_obj    = 0;   /* larger objects go the shared path */

/* Format [off, off+len) as a dead, walkable region (len >= 8, aligned).
 * Not linked into the free list — the next sweep reclaims/coalesces it. */
static void tlab_format_hole(uint32_t off, uint32_t len)
{
    CL_FreeBlock *fb = (CL_FreeBlock *)(cl_heap.arena + off);
    fb->size = len;
    fb->next_offset = 0;
}

/* Owner-only, lock-free: cut `*sizep` bytes from t's chunk.  May grow
 * *sizep by up to CL_MIN_ALLOC_SIZE-CL_ALIGN (absorbed tail).  Returns
 * NULL when no chunk or it doesn't fit (caller refills). */
static void *tlab_cut(CL_Thread *t, uint32_t *sizep)
{
    uint32_t size = *sizep;
    uint32_t rem;
    void *ptr;

    if (!t->tlab_end)
        return NULL;
    rem = t->tlab_end - t->tlab_cur;
    if (size > rem)
        return NULL;
    if (rem - size < CL_MIN_ALLOC_SIZE) {
        size = rem;                 /* absorb undersized tail */
        *sizep = size;
    }
    ptr = cl_heap.arena + t->tlab_cur;
    t->tlab_cur += size;
    if (t->tlab_cur == t->tlab_end)
        t->tlab_cur = t->tlab_end = 0;
    else
        tlab_format_hole(t->tlab_cur, t->tlab_end - t->tlab_cur);
    t->tlab_consed += size;
    return ptr;
}

/* Carve a fresh chunk for t and cut the first object from it.  Caller
 * MUST hold alloc_mutex.  Any abandoned remainder of the previous chunk
 * stays behind as a formatted hole (< tlab_max_obj bytes; the next sweep
 * reclaims it).  Falls back to the bounded free-list probe when the bump
 * front is exhausted, so TLABs stay effective in the sweep-only regime
 * between compactions.  Returns NULL when neither source can supply a
 * chunk — the caller degrades to the shared slow path. */
static void *tlab_refill_cut(CL_Thread *t, uint32_t *sizep)
{
    void    *chunk;
    uint32_t chunk_size = tlab_chunk_size;
    uint32_t chunk_off;

    if (!chunk_size)
        return NULL;
    chunk = alloc_from_bump(chunk_size);
    if (!chunk) {
        /* May return a larger block (use-entire-block path) — honor it. */
        chunk = alloc_from_free_list(&chunk_size, CL_FREELIST_PROBE_LIMIT);
    }
    if (!chunk)
        return NULL;
    chunk_off = (uint32_t)((uint8_t *)chunk - cl_heap.arena);

    cl_heap.total_consed += t->tlab_consed;
    t->tlab_consed = 0;
    /* Chunk-granular: counts the whole chunk as allocated up front; the
     * next sweep recomputes the exact figure.  Errs toward "heap fuller
     * than it is", which only makes the compaction heuristics fire
     * earlier, never later. */
    cl_heap.total_allocated += chunk_size;
    cl_heap.tlab_refills++;

    tlab_format_hole(chunk_off, chunk_size);
    t->tlab_cur = chunk_off;
    t->tlab_end = chunk_off + chunk_size;
    return tlab_cut(t, sizep);
}

/* Clear every registered thread's TLAB.  Called with the world stopped
 * (or single-threaded) at the top of each GC cycle, and on heap re-init.
 * Remainders are already formatted holes — the sweep/compaction that
 * follows reclaims them; owners refill on their next allocation. */
void gc_tlab_reset_all(void)
{
    CL_Thread *t;
    for (t = cl_thread_list; t; t = t->next) {
        t->tlab_cur = t->tlab_end = 0;
        cl_heap.total_consed += t->tlab_consed;
        t->tlab_consed = 0;
    }
}

/* Thread-exit retirement (see mem.h).  Runs on a live heap, so the
 * counter flush needs alloc_mutex; the remainder hole needs nothing. */
void cl_tlab_retire(CL_Thread *t)
{
    int multi = (cl_thread_count > 1);
    if (!t->tlab_end && !t->tlab_consed)
        return;
    if (multi && alloc_mutex) platform_mutex_lock(alloc_mutex);
    t->tlab_cur = t->tlab_end = 0;
    cl_heap.total_consed += t->tlab_consed;
    t->tlab_consed = 0;
    if (multi && alloc_mutex) platform_mutex_unlock(alloc_mutex);
}
#else /* !CL_TLAB */
/* Feature compiled out (Amiga binary-size gate — see mem.h).  Keep the
 * GC-entry hook as a no-op so call sites stay unconditional. */
void gc_tlab_reset_all(void) {}
#endif /* CL_TLAB */

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

#ifdef CL_GENGC
    /* Reset any previous heap's generational state FIRST: protection must
     * be dropped and the fault handler detached before the old arena is
     * freed below (a handler pointing at freed watch state would turn the
     * next unrelated crash into chaos).  Documented stale-static bug
     * class: every gen_* variable is re-derived for the new arena. */
    gen_reset();

    /* Page-aligned arena so old space can be mprotect-ed.  Fall back to
     * the classic allocator (and classic GC) if mmap is unavailable. */
    {
        const char *env = getenv("CLAMIGA_GENGC");
        int want_gen = !(env && env[0] == '0' && env[1] == '\0');
        cl_heap.arena = NULL;
        if (want_gen) {
            /* Round the watch region up to whole pages; the arena itself
             * keeps the requested byte size. */
            gen_page = platform_page_size();
            gen_npages = (heap_size + gen_page - 1) / gen_page;
            cl_heap.arena =
                (uint8_t *)platform_alloc_pages(gen_npages * gen_page);
            if (cl_heap.arena) {
                gen_dirty = (volatile uint8_t *)
                    platform_alloc((gen_npages + 7) / 8);
                gen_cross = (uint32_t *)
                    platform_alloc(gen_npages * sizeof(uint32_t));
                if (gen_dirty && gen_cross &&
                    platform_write_watch_install(cl_heap.arena,
                                                 gen_npages * gen_page,
                                                 gen_dirty) == 0) {
                    uint32_t i;
                    for (i = 0; i < gen_npages; i++)
                        gen_cross[i] = GEN_CROSS_NONE;
                    gen_arena_pages = 1;
                    gen_enabled = 1;
                } else {
                    /* Partial setup failed — release and fall back. */
                    if (gen_dirty) { platform_free((void *)gen_dirty); gen_dirty = NULL; }
                    if (gen_cross) { platform_free(gen_cross); gen_cross = NULL; }
                    platform_free_pages(cl_heap.arena, gen_npages * gen_page);
                    cl_heap.arena = NULL;
                    platform_write_string(
                        "clamiga: generational GC unavailable "
                        "(mmap/write-watch setup failed) - classic GC\n");
                }
            }
        }
        if (!cl_heap.arena) {
            gen_arena_pages = 0;
            cl_heap.arena = (uint8_t *)platform_alloc(heap_size);
        }
    }
#else
    cl_heap.arena = (uint8_t *)platform_alloc(heap_size);
#endif
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
    gc_sweeps_since_compact = 0;

    /* Block-start index for the JIT native-stack scan — classic collector
     * only (see the definition above).  Re-derived for every new arena. */
#ifdef CL_GENGC
    if (gen_enabled)
        gc_hdr_index_release();
    else
#endif
        gc_hdr_index_init();

#ifdef CL_TLAB
    /* TLAB chunk size for this heap.  CLAMIGA_TLAB_CHUNK=<bytes> overrides
     * (0 disables); otherwise CL_TLAB_CHUNK, scaled down so per-thread
     * chunks can't dominate a small (test-sized) arena, and disabled
     * entirely when even the minimum chunk would be outsized. */
    {
        uint32_t chunk = CL_TLAB_CHUNK;
        const char *e = getenv("CLAMIGA_TLAB_CHUNK");
        if (e)
            chunk = (uint32_t)atol(e);
        if (chunk) {
            uint32_t cap = heap_size / 64u;
            if (chunk > cap) chunk = cap;
            chunk &= ~(uint32_t)(CL_ALIGN - 1);
            if (chunk < CL_TLAB_MIN_CHUNK)
                chunk = 0;
        }
        tlab_chunk_size = chunk;
        tlab_max_obj = chunk / 8u;
    }

    /* Clear stale TLABs on every registered thread: their offsets point
     * into the PREVIOUS arena (unit tests re-init with fresh heaps). */
    {
        CL_Thread *t;
        for (t = cl_thread_list; t; t = t->next) {
            t->tlab_cur = t->tlab_end = 0;
            t->tlab_consed = 0;
        }
    }
#endif /* CL_TLAB */

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
    cl_heap.bump = bump;
    cl_heap.free_list = 0;
    /* The restored payload is dense (the writer compacts before dumping):
     * everything in [CL_ALIGN, bump) is live allocation. */
    cl_heap.total_allocated = bump - CL_ALIGN;
    cl_heap.total_consed = bump - CL_ALIGN;
    gc_reset_transient_state();
    gc_last_compact_cycle = 0xFFFFFFFF;
    gc_sweeps_since_compact = 0;
    /* Block-start index: the payload replaces whatever the arena held;
     * cl_mem_adopt_image_finish's classic walk re-notes it. */
    if (gc_hdr_page)
        gc_hdr_index_clear();
    gc_hdr_bump_reset();
}

void cl_mem_adopt_image_finish(void)
{
    uint8_t *ptr = cl_heap.arena + CL_ALIGN;
    uint8_t *end = cl_heap.arena + cl_heap.bump;

#ifdef CL_GENGC
    if (gen_enabled) {
        /* The whole payload becomes OLD space: sticky marks set (a minor
         * trace terminates at marked old objects), crossing map rebuilt
         * (page -> first object start), watermark moved to bump, dirty
         * bits cleared, old-space protection re-armed.  The survivor/
         * finalizable/rehash side lists are empty by construction (fresh
         * init, no nursery allocation has happened yet). */
        while (ptr < end) {
            uint32_t size = CL_HDR_SIZE(ptr);
            if (size == 0) break;
            CL_HDR_SET_MARK(ptr);
            gen_cross_note((uint32_t)(ptr - cl_heap.arena));
            ptr += size;
        }
        gen_old_top = cl_heap.bump;
        gen_live_count = 0;
        gen_fin_count = 0;
        gen_ht_count = 0;
        if (gen_dirty)
            memset((void *)gen_dirty, 0, (gen_npages + 7) / 8);
        gen_protect_old();
        return;
    }
#endif
    /* Classic collector: the saving process may have run generational
     * (sticky marks SET on every old object) — normalize to the all-clear
     * state gc_mark assumes at cycle start.  Also covers a classic-mode
     * save, where the pre-dump compaction already left marks clear.
     * The same walk rebuilds the block-start index for the payload. */
    while (ptr < end) {
        uint32_t size = CL_HDR_SIZE(ptr);
        if (size == 0) break;
        CL_HDR_CLR_MARK(ptr);
        if (GC_HDR_CROSSES((uint32_t)(ptr - cl_heap.arena), size))
            gc_hdr_note((uint32_t)(ptr - cl_heap.arena), size);
        ptr += size;
    }
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
 * The walk mirrors gc_sweep's: step object by object from the first slot to
 * the bump pointer using the header size.  Free-list blocks are walked over
 * safely — a free block's header word is its raw size, which reads back as
 * an unmarked TYPE_CONS, so it never matches TYPE_BYTECODE and its size
 * still advances the cursor correctly.
 *
 * Only memory is released here.  OS handles (stream fds, lock mutexes,
 * thread handles) are deliberately left to the dedicated shutdown paths that
 * already ran — cl_stream_shutdown and cl_thread_shutdown — so this walk
 * cannot double-close a handle or print a spurious "dropped without CLOSE"
 * warning for a stream the caller never had to close. */
static uint32_t gc_release_all_offheap(void)
{
    uint8_t *ptr = cl_heap.arena + CL_ALIGN;  /* offset 0 is reserved for NIL */
    uint8_t *end = cl_heap.arena + cl_heap.bump;
    uint32_t freed = 0;

    while (ptr < end) {
        uint32_t size = CL_HDR_SIZE(ptr);
        if (size == 0) break;             /* corrupt header — stop, don't guess */
        if (CL_HDR_TYPE(ptr) == TYPE_BYTECODE)
            freed += bytecode_release_offheap((CL_Bytecode *)ptr);
        ptr += size;
    }
    return freed;
}

void cl_mem_shutdown(void)
{
#ifdef CL_GENGC
    int was_pages = gen_arena_pages;
    uint32_t watch_bytes = gen_npages * gen_page;
    gen_reset();   /* unprotect + detach handler before we touch any page */
#endif

    /* Before the arena goes away, hand back what hangs off it.  Must run
     * after gen_reset() above (the nursery pages are read-protected until
     * then) and before either free below. */
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

#ifdef CL_GENGC
    if (cl_heap.arena && was_pages) {
        platform_free_pages(cl_heap.arena, watch_bytes);
        cl_heap.arena = NULL;
        gen_arena_pages = 0;
    }
#endif
    if (cl_heap.arena) {
        platform_free(cl_heap.arena);
        cl_heap.arena = NULL;
    }
    gc_mark_stack_release();
    gc_hdr_index_release();
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

static void *alloc_from_bump(uint32_t size)
{
    if (cl_bump_fits(cl_heap.bump, size, cl_heap.arena_size)) {
        uint32_t old = cl_heap.bump;
        void *ptr = cl_heap.arena + old;
        cl_heap.bump = old + size;
        /* Block-start index: one compare per bump allocation; the slow
         * path runs once per 4 KB of bump-allocated data. */
        if (cl_heap.bump > gc_hdr_bump_watch)
            gc_hdr_note_bump(old, size);
        return ptr;
    }
    return NULL;
}

/* First-fit allocation from the free list.  Walks at most max_steps blocks
 * (0 == unbounded); returns NULL if no fit is found within the bound. */
static void *alloc_from_free_list(uint32_t *sizep, uint32_t max_steps)
{
    uint32_t size = *sizep;
    uint32_t *prev_off = &cl_heap.free_list;
    uint32_t cur_off = cl_heap.free_list;
    uint32_t steps = 0;

    while (cur_off) {
        CL_FreeBlock *block = (CL_FreeBlock *)(cl_heap.arena + cur_off);
        cl_heap.freelist_steps++;
        if (max_steps && ++steps > max_steps)
            return NULL;
        if (block->size >= size) {
            uint32_t remainder = block->size - size;
            if (remainder >= CL_MIN_ALLOC_SIZE) {
                /* Split block */
                uint32_t new_off = cur_off + size;
                CL_FreeBlock *new_free = (CL_FreeBlock *)(cl_heap.arena + new_off);
                new_free->size = remainder;
                new_free->next_offset = block->next_offset;
                *prev_off = new_off;
                /* Block-start index: the front piece keeps the block's
                 * header, so the index stays valid without this; noting
                 * both pieces keeps the scan's walk short after a large
                 * free block has been carved into many objects. */
                if (GC_HDR_CROSSES(cur_off, size))
                    gc_hdr_note(cur_off, size);
                if (GC_HDR_CROSSES(new_off, remainder))
                    gc_hdr_note(new_off, remainder);
            } else {
                /* Use entire block — report actual size so header matches */
                size = block->size;
                *sizep = size;
                *prev_off = block->next_offset;
            }
            memset(block, 0, size);
            return block;
        }
        prev_off = &block->next_offset;
        cur_off = block->next_offset;
    }
    return NULL;
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
     * NOTE: the `!multi` gate means the TLAB fast path below (CL_TLAB,
     * multi-threaded only) never runs under gc-stress — the two are
     * mutually exclusive by construction.  TLAB coverage instead comes
     * from tlab_mixed_sizes_vs_concurrent_compaction in
     * tests/test_gc_threaded.c, which hammers concurrent compaction
     * against live TLAB cutting without the per-alloc stress hook. */
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
                cl_gc_compact();
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

#ifdef CL_TLAB
    /* TLAB fast path (multi-threaded only): cut from this thread's private
     * chunk without touching alloc_mutex; refill under the mutex when the
     * chunk is spent.  Objects above tlab_max_obj take the shared path so
     * a large vector doesn't waste most of a chunk.  When even a refill
     * fails (bump front AND bounded free-list probe exhausted), fall
     * through to the shared slow path below, which GCs/compacts. */
    if (multi && size <= tlab_max_obj) {
        CL_Thread *t = cl_get_current_thread();
        ptr = tlab_cut(t, &size);
        if (!ptr) {
            platform_mutex_lock(alloc_mutex);
            ptr = tlab_refill_cut(t, &size);
            platform_mutex_unlock(alloc_mutex);
        }
        if (ptr) {
            alloc_zero(ptr, size);
            ((CL_Header *)ptr)->header = CL_MAKE_HDR(type, size);
#ifdef CL_GENGC
            /* Young finalizable objects are tracked so a minor GC can
             * release their external resources without walking dead
             * nursery space.  Rare types — the note takes alloc_mutex
             * itself (we are on the lock-free path here). */
            if (gen_enabled && gen_type_finalizable(type))
                gen_fin_note((uint32_t)((uint8_t *)ptr - cl_heap.arena), 1);
#endif
            return ptr;
        }
    }
#endif /* CL_TLAB */

    if (multi) platform_mutex_lock(alloc_mutex);

    /* Fast path: bump allocator, then a *bounded* first-fit free-list probe.
     * The bound keeps a single allocation cheap (O(CL_FREELIST_PROBE_LIMIT))
     * even when the free list is long; if no fit turns up quickly we fall
     * through to GC/compaction below rather than walking the whole list. */
    ptr = alloc_from_bump(size);
    if (!ptr) {
        /* Try free list (may update size if using entire oversized block) */
        ptr = alloc_from_free_list(&size, CL_FREELIST_PROBE_LIMIT);
    }
#ifdef CL_GENGC
    if (!ptr && gen_enabled) {
        /* Nursery exhausted: minor collection first (traces only the live
         * young set + dirty pages, slides survivors onto the watermark and
         * resets the bump), escalating to a full compaction when the minor
         * is refused, is deduped away without freeing enough, or old space
         * has grown to the point that nursery headroom is gone (< 1/8 of
         * the arena — reclaim the floating old garbage instead of minor-
         * thrashing a sliver of nursery). */
        uint32_t seen_gc_count = cl_heap.gc_count;
        int minor_ok;
        if (multi) platform_mutex_unlock(alloc_mutex);
        minor_ok = cl_gc_minor(seen_gc_count);
        if (multi) platform_mutex_lock(alloc_mutex);
        if (minor_ok &&
            cl_heap.arena_size - gen_old_top >= cl_heap.arena_size / 8)
            ptr = alloc_from_bump(size);
        if (!ptr) {
            if (multi) platform_mutex_unlock(alloc_mutex);
            cl_gc_compact();
            if (multi) platform_mutex_lock(alloc_mutex);
            ptr = alloc_from_bump(size);
            if (!ptr && !gen_enabled) {
                /* The compaction degraded to a classic sweep (fallback
                 * paths disable gen mode) — the space is on the free
                 * list now. */
                ptr = alloc_from_free_list(&size, 0 /* unbounded */);
            }
        }
        if (!ptr) {
            if (multi) platform_mutex_unlock(alloc_mutex);
            cl_storage_error("Heap exhausted (requested %u bytes)",
                             (unsigned)size);
        }
    }
#endif /* CL_GENGC */
    if (!ptr) {
        /* Bump exhausted and the bounded free-list probe missed.  Sweep first:
         * it is cheaper than compaction, may itself free a fit, AND recomputes
         * the live-set size (cl_heap.total_allocated) so the compaction decision
         * below uses a FRESH measurement — not a stale high-water that an
         * earlier transient peak could pin near the arena size.  Release
         * alloc_mutex first so other threads can reach safepoints during STW.
         *
         * cl_gc_if_stale dedups the redundant-GC burst: if a peer thread
         * collected while we raced for stop-the-world initiator-ship, it
         * skips (returns 0) and we first retry the allocation against the
         * peer's fresh sweep; only if that retry ALSO misses do we run our
         * own full collection. */
        uint32_t seen_gc_count = cl_heap.gc_count;
        int collected;
        if (multi) platform_mutex_unlock(alloc_mutex);
        collected = cl_gc_if_stale(seen_gc_count);
        if (multi) platform_mutex_lock(alloc_mutex);
        if (!collected) {
            ptr = alloc_from_bump(size);
            if (!ptr)
                ptr = alloc_from_free_list(&size, CL_FREELIST_PROBE_LIMIT);
            if (!ptr) {
                /* The peer's cycle left no fit for us — collect for real. */
                if (multi) platform_mutex_unlock(alloc_mutex);
                cl_gc();
                if (multi) platform_mutex_lock(alloc_mutex);
                collected = 1;
            }
        }
        if (collected) {
        gc_sweeps_since_compact++;
        ptr = alloc_from_bump(size);
        if (!ptr) {
            /* Guard: skip the free-list probe when Trigger 2 would compact
             * immediately afterward.  alloc_from_free_list zeroes the block it
             * extracts (memset in the return path); a zeroed block has a zero
             * size field, which stops every compaction pass's arena scan at that
             * slot, leaving live objects above it without forwarding addresses —
             * the next bump allocation then silently overwrites them.  Skipping
             * the probe keeps the arena intact so cl_gc_compact() can walk it
             * cleanly; ptr stays NULL and the compaction block below fires via
             * Trigger 1 (!ptr), satisfying this allocation after compaction. */
            uint32_t probe_live_hwm = cl_heap.arena_size - (cl_heap.arena_size >> 3);
            int will_compact_t2 = (gc_last_compact_cycle != cl_heap.gc_count) &&
                                  (cl_heap.total_allocated < probe_live_hwm) &&
                                  (gc_sweeps_since_compact >= GC_SWEEPS_BEFORE_COMPACT) &&
                                  !(gc_fwd_fail_bump &&
                                    cl_heap.bump >= gc_fwd_fail_bump);
            if (!will_compact_t2)
                ptr = alloc_from_free_list(&size, CL_FREELIST_PROBE_LIMIT);
        }

        /* Now decide whether to ALSO compact (which resets the bump pointer and
         * defragments).  Gated to once per GC cycle.  Two triggers:
         *
         *   1. The sweep + bounded probe STILL found no fit — compaction is the
         *      only remaining way to satisfy this allocation (the long-standing
         *      last-resort path).
         *
         *   2. We have swept GC_SWEEPS_BEFORE_COMPACT times since the last
         *      compaction AND the heap is not near-full.  A sweep-only cl_gc()
         *      never resets the bump pointer, so when a fragmented free list
         *      defeats the bounded probe we can keep "succeeding" via a buried
         *      free block while running a full mark-sweep on nearly every
         *      allocation — the "sweep-forever" pathology (acutely heap-size
         *      sensitive: hundreds to thousands of GCs cold-compiling a large
         *      system, while a nearby heap size needs ~15).  Compaction frees a
         *      large contiguous run of bump space, escaping the loop.
         *      "Not near-full" uses the FRESH live-set size from the sweep just
         *      done, so a heap that is mostly reclaimable garbage (compaction
         *      frees >= 1/8 of the arena — worthwhile) is distinguished from one
         *      genuinely packed with live data (compaction would only thrash, so
         *      we stay on the sweep + free-list-reuse path).
         *
         * NOTE: compaction is a moving GC.  All CL_Obj C locals live across an
         * allocating call MUST be GC-protected, and raw pointers derived from
         * CL_Obj re-derived afterwards.  (This site already compacted before —
         * no new requirement.)  Safe while JIT'd code is on the m68k stack:
         * gc_scan_jit_native_stack offset-validates each conservative root. */
        if (gc_last_compact_cycle != cl_heap.gc_count) {
            uint32_t live_hwm =
                cl_heap.arena_size - (cl_heap.arena_size >> 3);   /* 7/8 arena */
            int compaction_worthwhile = (cl_heap.total_allocated < live_hwm);
            /* Trigger 2 is additionally gated on the forwarding-table OOM
             * latch (gc_fwd_fail_bump): re-attempting at the same bump
             * level would fail the same platform_alloc again after a full
             * (wasted) mark pass.  Trigger 1 (!ptr) is exempt — when
             * compaction is the ONLY way to satisfy the allocation it
             * must be attempted regardless. */
            int fwd_blocked = (gc_fwd_fail_bump != 0 &&
                               cl_heap.bump >= gc_fwd_fail_bump);
            /* Trigger 1 while the latch is set would run the whole mark
             * phase only to fail the same allocation again — first walk
             * the entire free list, which the bounded probe above may
             * have cut short.  A hit means no compaction (so the zeroed
             * block cannot break an arena walk, see the probe guard
             * above); a miss leaves the arena untouched for the attempt. */
            if (!ptr && fwd_blocked)
                ptr = alloc_from_free_list(&size, 0 /* unbounded */);
            if (!ptr ||
                (compaction_worthwhile && !fwd_blocked &&
                 gc_sweeps_since_compact >= GC_SWEEPS_BEFORE_COMPACT)) {
                gc_compact_pending = 1;
                gc_last_compact_cycle = cl_heap.gc_count;
                if (multi) platform_mutex_unlock(alloc_mutex);
                cl_gc_compact();   /* resets bump + gc_sweeps_since_compact */
                gc_compact_pending = 0;
                if (multi) platform_mutex_lock(alloc_mutex);
                ptr = alloc_from_bump(size);
            }
        }
        }  /* if (collected) — skipped when a peer's GC already satisfied us */
    }
    if (!ptr) {
        /* Last resort before declaring the heap exhausted: the fit may be
         * deeper in the free list than the bounded probe reached.  Walk the
         * whole list once so a genuine fit is never missed. */
        ptr = alloc_from_free_list(&size, 0 /* unbounded */);
    }
    if (!ptr) {
        if (multi) platform_mutex_unlock(alloc_mutex);
        cl_storage_error("Heap exhausted (requested %u bytes)", (unsigned)size);
    }

    /* (The header-size guard runs at function entry, BEFORE any bump
     * advance — see above.  alloc_from_free_list can only ever shrink-fit
     * or return the exact block, never grow size past the mask.) */

    /* Initialize: zero entire block, then set header.
     * Zeroing prevents stale data in padding/trailing bytes from being
     * misinterpreted by GC (e.g. closure padding read as upvalue slots)
     * or FASL serializer (traverses object graph by following CL_Obj fields). */
    alloc_zero(ptr, size);
    ((CL_Header *)ptr)->header = CL_MAKE_HDR(type, size);
    cl_heap.total_allocated += size;
    cl_heap.total_consed += size;

#ifdef CL_GENGC
    /* See the TLAB-path twin above; here alloc_mutex is already held
     * (or the process is single-threaded) — no extra locking. */
    if (gen_enabled && gen_type_finalizable(type))
        gen_fin_note((uint32_t)((uint8_t *)ptr - cl_heap.arena), 0);
#endif

    if (multi) platform_mutex_unlock(alloc_mutex);

    return ptr;
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
#ifdef CL_TLAB
    sprintf(buf, "TLAB: %lu-byte chunks, %lu refills\n",
            (unsigned long)tlab_chunk_size,
            (unsigned long)cl_heap.tlab_refills);
    platform_write_string(buf);
#endif
}
