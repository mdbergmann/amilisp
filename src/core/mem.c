#include "mem.h"
#include "error.h"
#include "float.h"
#include "vm.h"
#include "readtable.h"
#include "package.h"
#include "stream.h"
#include "reader.h"   /* cl_srcloc_table (GC invalidate/forward hooks) */
#include "../jit/jit.h"    /* cl_jit_free_native */
#include "../platform/platform.h"
#include "../platform/platform_thread.h"
#include <stdarg.h>
#include <stdio.h>    /* CLAMIGA_GC_DIAG per-collection trace (fprintf/stderr) */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef DEBUG_GC_STRESS_CBT
#include <execinfo.h>
#endif

/* External roots needed for GC marking */
extern CL_Obj macro_table, setf_table, setf_fn_table, setf_expander_table, type_table, compiler_macro_table;
extern CL_Obj cl_clos_class_table;
extern CL_Obj struct_table;  /* builtins_struct.c: struct type registry */
extern CL_Obj condition_hierarchy;        /* builtins_condition.c */
extern CL_Obj condition_slot_table;       /* builtins_condition.c */
extern CL_Obj condition_default_initargs; /* builtins_condition.c */
extern CL_Obj condition_slot_initforms;   /* builtins_condition.c */

/* Active compiler chain is accessed via cl_active_compiler macro (thread.h) */
typedef struct CL_Compiler_s CL_Compiler;

/* thread.h defines `pending_lambda_name` as a macro for `(CT->pending_lambda_name)`,
 * but the GC walks every thread via an explicit `t->` pointer (not CT), so we
 * must reference the raw struct member.  Drop the macro here — mem.c only ever
 * touches this field through `t->pending_lambda_name`, never the macro form. */
#undef pending_lambda_name

/* STW GC coordination (defined in thread.c) */
extern void cl_gc_stop_the_world(void);
extern void cl_gc_resume_the_world(void);

/* Allocation mutex — protects bump pointer, free list, and heap metadata */
static void *alloc_mutex = NULL;

CL_Heap cl_heap;
uint8_t *cl_arena_base = NULL;  /* Global arena base for offset↔pointer conversion */

/* GC root stack now lives in CL_Thread.
 * gc_root_count is a macro from thread.h.
 * gc_root_stack is a local macro below. */
#define gc_root_stack (CT->gc_roots)

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
static CL_Obj  *gc_mark_stack = gc_mark_stack_initial;
static uint32_t gc_mark_stack_cap = CL_GC_MARK_STACK_SIZE;
static uint32_t gc_mark_top = 0;
static int gc_mark_overflow = 0;    /* Set when mark stack overflows */
static int gc_mark_grow_failed = 0; /* Per-cycle latch: stop re-attempting a
                                     * failed grow on every subsequent push
                                     * this cycle.  Reset by gc_mark. */
/* Monotonic diagnostics (exposed via ext:%gc-mark-stats and cl_mem_stats):
 * grows = successful capacity doublings; rescan passes = full-arena overflow
 * recovery passes — nonzero means the quadratic fallback ran (a growth
 * failure), which is worth knowing about long before it becomes a 49s GC. */
static uint32_t gc_mark_stack_grows = 0;
static uint32_t gc_mark_rescan_passes = 0;

/* Off-heap bytecode payload handed back to the allocator since heap init:
 * total bytes and the number of bytecode objects finalized.  Exposed as
 * (ext:%bytecode-offheap-stats).  A CL_Bytecode owns up to eight
 * platform_alloc'd buffers that live outside the arena, so "how much of that
 * has actually been returned" is not visible in any arena statistic — on
 * AmigaOS, where an unreturned block is lost until reboot, it is the number
 * that decides whether a long REPL session or a repeated launch bleeds Fast
 * RAM.  Monotonic; wraps at 4GB (diagnostic only).  Reset by cl_mem_init. */
static uint32_t gc_bc_offheap_bytes = 0;
static uint32_t gc_bc_offheap_count = 0;
/* Test hook: when nonzero, caps growth at this many entries so the overflow
 * re-scan fallback can be exercised deterministically (see
 * tests/test_gc_markstack.c).  0 = normal heap-proportional cap. */
static uint32_t gc_mark_stack_test_limit = 0;

/* Cumulative GC phase timers (microseconds, monotonic; exposed via
 * ext:%gc-time-stats and cl_mem_stats).  stw = waiting for other threads to
 * park at the start of a collection; mark/sweep = the sweep-GC phases as
 * timed in cl_gc; compact = whole cl_gc_compact cycles (which run their own
 * mark internally — compaction time is NOT double-counted into mark/sweep).
 * Updated only with the world stopped / single-threaded, so plain u64s. */
static uint64_t gc_time_stw_us = 0;
static uint64_t gc_time_mark_us = 0;
static uint64_t gc_time_sweep_us = 0;
static uint64_t gc_time_compact_us = 0;
static uint64_t gc_time_stw_max_us = 0;  /* worst single stop */
static uint32_t gc_stw_stops = 0;        /* stop-the-world events */
static uint32_t gc_epoch_skips = 0;      /* redundant GCs deduped (see
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

static int gc_diag_on(void)
{
    if (gc_diag_cached == -2) {
        char envbuf[16];
        const char *s = platform_getenv("CLAMIGA_GC_DIAG", envbuf,
                                        (int)sizeof(envbuf));
        gc_diag_cached = (s && *s) ? 1 : 0;
    }
    return gc_diag_cached;
}

static void gc_diag_report(const char *kind, uint64_t pause_us)
{
    fprintf(stderr, "[GC] %lums #%lu %s pause=%luus used=%luK/%luK\n",
            (unsigned long)platform_time_ms(),
            (unsigned long)cl_heap.gc_count, kind,
            (unsigned long)pause_us,
            (unsigned long)(cl_heap.total_allocated >> 10),
            (unsigned long)(cl_heap.arena_size >> 10));
    fflush(stderr);
}

/* Forward declarations */
static void gc_mark(void);
static void gc_sweep(void);
static uint32_t bytecode_release_offheap(CL_Bytecode *bc);
static uint64_t gc_stop_world_timed(int multithread);
static void gc_reset_transient_state(void);
static int root_slot_independently_forwarded(CL_Obj *slot);
static void *alloc_from_bump(uint32_t size);
static void *alloc_from_free_list(uint32_t *sizep, uint32_t max_steps);
void gc_mark_obj(CL_Obj obj);
static void gc_mark_push(CL_Obj obj);
static void gc_mark_stack_release(void);
static void *alloc_from_free_list(uint32_t *sizep, uint32_t max_steps);
static void *alloc_from_bump(uint32_t size);

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
#ifdef DEBUG_GC
static void gc_verify_marked(void);
static void gc_dump_roots_dbg(void);
static int gc_verify_errors;  /* defined/reset inside gc_verify_marked */
#endif

/* --- Block-start index (classic collector) -------------------------------
 *
 * gc_hdr_page[p] is the arena offset of a block header (object, free block
 * or TLAB hole) at or below the first byte of arena page p, from which the
 * linear "walk by header size" is valid.  It lets the conservative JIT
 * native-stack scan (gc_scan_jit_native_stack) decide "is this word the
 * start of a block?" by walking at most one page of headers instead of the
 * whole arena bump-front, which is what made every collection taken inside
 * JIT'd code pay an extra sweep-sized pass in the mark phase (the marker's
 * JIT cost measured by the Clamacs spike: mark 80 ms vs 40 ms without the
 * JIT on a 68040-class machine, first collection 361 vs 129 ms).
 *
 * Invariant (every page p with p*GC_HDR_PAGE < cl_heap.bump): the entry is
 * a real header offset <= p*GC_HDR_PAGE, or GC_HDR_NONE, in which case the
 * nearest lower page's entry (ultimately CL_ALIGN) serves.  A header, once
 * written, stays a header at that offset until the next sweep or slide,
 * because between collections blocks are only ever SPLIT from the front
 * (alloc_from_free_list, tlab_cut) — the front header survives at its
 * offset and the remainder gets a fresh one.  So the index is rebuilt by
 * the two passes that rewrite headers (gc_sweep coalesces, gc_slide moves)
 * and extended by the paths that create a header on virgin bytes
 * (alloc_from_bump, and the split remainder for good measure).  The image
 * loader's classic walk re-notes the adopted payload.
 *
 * Classic collector only: gen mode (host) has no free list, moves young
 * objects in minor collections and never runs the JIT; there the index is
 * absent (NULL) and the scan keeps its full-walk validation.  An allocation
 * failure at init leaves it absent as well — same fallback. */
#define GC_HDR_PAGE_SHIFT 12
#define GC_HDR_PAGE       (1u << GC_HDR_PAGE_SHIFT)
#define GC_HDR_NONE       0xFFFFFFFFu
static uint32_t *gc_hdr_page   = NULL;
static uint32_t  gc_hdr_npages = 0;
#if defined(DEBUG_GC) || defined(DEBUG_GC_STRESS)
static void gc_jit_scan_verify_index(const uint32_t *candidates, int n_cand);
#endif
/* Next page boundary at or above cl_heap.bump: a bump allocation that ends
 * beyond it covers a page's first byte and must be noted.  0xFFFFFFFF
 * (never crossed) while the index is absent, so alloc_from_bump pays one
 * compare either way. */
static uint32_t  gc_hdr_bump_watch = 0xFFFFFFFFu;

/* Does a block [off, off+size) cover the first byte of some page? */
#define GC_HDR_CROSSES(off, size) \
    ((((off) + GC_HDR_PAGE - 1) & ~(uint32_t)(GC_HDR_PAGE - 1)) < (off) + (size))

/* Record `off` as the block covering the first byte of every page whose
 * first byte lies in [off, off+size).  Assignment, not min/max: the caller
 * knows the block is the one covering those bytes right now. */
static void gc_hdr_note(uint32_t off, uint32_t size)
{
    uint32_t first = (off + GC_HDR_PAGE - 1) >> GC_HDR_PAGE_SHIFT;
    uint32_t last  = (off + size - 1) >> GC_HDR_PAGE_SHIFT;
    if (!gc_hdr_page) return;
    if (last >= gc_hdr_npages) last = gc_hdr_npages - 1;
    while (first <= last)
        gc_hdr_page[first++] = off;
}

static void gc_hdr_bump_reset(void)
{
    if (gc_hdr_page)
        gc_hdr_bump_watch = (cl_heap.bump + GC_HDR_PAGE - 1)
                            & ~(uint32_t)(GC_HDR_PAGE - 1);
    else
        gc_hdr_bump_watch = 0xFFFFFFFFu;
}

/* Slow path of alloc_from_bump: the block [old, old+size) just placed at
 * the bump front crossed gc_hdr_bump_watch. */
static void gc_hdr_note_bump(uint32_t old, uint32_t size)
{
    gc_hdr_note(old, size);
    gc_hdr_bump_reset();
}

static void gc_hdr_index_clear(void)
{
    uint32_t i;
    for (i = 0; i < gc_hdr_npages; i++)
        gc_hdr_page[i] = GC_HDR_NONE;
}

static void gc_hdr_index_release(void)
{
    if (gc_hdr_page) {
        platform_free(gc_hdr_page);
        gc_hdr_page = NULL;
    }
    gc_hdr_npages = 0;
    gc_hdr_bump_watch = 0xFFFFFFFFu;
}

/* Size the index for the current arena (called from cl_mem_init after the
 * arena exists and the collector mode is known). */
static void gc_hdr_index_init(void)
{
    char ebuf[8];
    const char *e;
    gc_hdr_index_release();
    /* CLAMIGA_HDR_INDEX=0 leaves the index absent (the scan falls back to
     * the full arena walk) — an A/B switch for measuring the marker's JIT
     * cost, in the spirit of CLAMIGA_GENGC=0. */
    e = platform_getenv("CLAMIGA_HDR_INDEX", ebuf, (int)sizeof(ebuf));
    if (e && e[0] == '0' && e[1] == '\0')
        return;
    gc_hdr_npages = (cl_heap.arena_size + GC_HDR_PAGE - 1) >> GC_HDR_PAGE_SHIFT;
    if (gc_hdr_npages == 0) gc_hdr_npages = 1;
    gc_hdr_page = (uint32_t *)platform_alloc(gc_hdr_npages * sizeof(uint32_t));
    if (!gc_hdr_page) {
        gc_hdr_npages = 0;
        gc_hdr_bump_watch = 0xFFFFFFFFu;
        return;
    }
    gc_hdr_index_clear();
    gc_hdr_bump_reset();
}

/* A header offset from which walking forward reaches every block of page
 * `page` (the page's own entry, else the nearest lower page's, else the
 * first block of the arena). */
static uint32_t gc_hdr_walk_start(uint32_t page)
{
    while (page > 0 && gc_hdr_page[page] == GC_HDR_NONE)
        page--;
    if (page == 0 || gc_hdr_page[page] == GC_HDR_NONE)
        return CL_ALIGN;
    return gc_hdr_page[page];
}

/* Walk cursor for gc_hdr_is_block_start: [blk, nxt) is the block found by
 * the previous call.  Ascending queries within one block or the next few
 * continue from it instead of re-entering through the index. */
typedef struct {
    uint32_t blk;
    uint32_t nxt;
} GC_HdrCursor;

/* Is `offset` the start of a block below the bump front?  Needs the index
 * (caller checks gc_hdr_page).  Bounded: at most one page of headers plus
 * whatever a stale (split-behind) entry adds, never the whole arena. */
static int gc_hdr_is_block_start(uint32_t offset, GC_HdrCursor *cur)
{
    uint32_t b, size;
    if (offset < CL_ALIGN || offset >= cl_heap.bump) return 0;
    if (cur->blk <= offset && offset < cur->nxt)
        return offset == cur->blk;
    b = gc_hdr_walk_start(offset >> GC_HDR_PAGE_SHIFT);
    if (cur->nxt != 0 && cur->nxt <= offset && cur->nxt > b)
        b = cur->nxt;                       /* continue the ascending walk */
    for (;;) {
        size = CL_HDR_SIZE(cl_heap.arena + b);
        if (size == 0) {                    /* malformed header: not a block */
            cur->blk = cur->nxt = 0;
            return 0;
        }
        if (b + size > offset) break;
        b += size;
    }
    cur->blk = b;
    cur->nxt = b + size;
    return b == offset;
}

/* Compaction forwarding structures — one transient platform_alloc block,
 * taken after the mark phase and handed back before cl_gc_compact returns:
 *
 *   gc_fwd_bits  one bit per CL_ALIGN granule of [0, bump), set when the
 *                granule lies inside a marked (live) object;
 *   gc_fwd_page  per GC_FWD_PAGE bytes of arena, the slide cursor at the
 *                page's first byte: CL_ALIGN + the live bytes below it +
 *                the holes pinned objects below it leave behind;
 *   gc_fwd_gap   per entry of the sorted jit_pinned[], the hole the slide
 *                leaves below that pin (pin offset - cursor on arrival).
 *
 *   new(obj) = gc_fwd_page[page(obj)]
 *            + CL_ALIGN * popcount(bits from the page start up to obj)
 *            + sum of gc_fwd_gap over pins in [page start, obj]
 *
 * The bitmap is 1/(8*CL_ALIGN) and the page table 4/GC_FWD_PAGE of the
 * used span — 1/32 + 1/64 on 32-bit, about 390 KB for an 8 MB heap.  The
 * previous design kept one uint32 forwarding address per granule, a second
 * block as large as the heap's used span.  An A1200 with the 8 MB heap in
 * its Zorro block cannot AllocVec another 7-8 MB once the trapdoor Fast
 * RAM holds the compiler pool and the stacks; every bump-exhausted
 * allocation then ran a full mark, failed the table, swept, and repeated
 * on the next allocation — test-amiga-lowend spent ~25 s per allocation in
 * the 'alloc sweep-forever escape' check (2026-09-24).  The size bound is
 * pinned by tests/test_gc_fwd_bitmap.c.
 *
 * A page is two bitmap words (64 granules: 256 bytes on 32-bit, 512 on a
 * 64-bit host), so a lookup pops at most one full word and one partial —
 * the page table costs half the bitmap for that.  The page start falls on
 * a word boundary by construction. */
#if CL_ALIGN == 8
#define GC_FWD_ALIGN_SHIFT 3
#else
#define GC_FWD_ALIGN_SHIFT 2
#endif
#define GC_FWD_WORDS_PER_PAGE 2u
#define GC_FWD_PAGE_SHIFT  (GC_FWD_ALIGN_SHIFT + 5 + 1)
#define GC_FWD_PAGE        (1u << GC_FWD_PAGE_SHIFT)
static uint32_t *gc_fwd_bits  = NULL;
static uint32_t *gc_fwd_page  = NULL;
static uint32_t *gc_fwd_gap   = NULL;
static void     *gc_fwd_block = NULL;   /* the allocation behind the three */

/* Bump level at which the last forwarding-table platform_alloc failed
 * (0 = none).  While the bump front is at or above this level a
 * sweep-triggered compaction attempt (cl_alloc trigger 2) would fail the
 * same way — full mark work for nothing — so trigger 2 is gated on it.
 * Cleared by a successful compaction (which also resets the bump). */
static uint32_t gc_fwd_fail_bump = 0;

/* Fault injection (tests/test_gc_fwd_bitmap.c): while non-zero, a forwarding
 * block of at least this many bytes is refused as if platform_alloc had said
 * no — the only way a host test can drive the OOM fallback and its latch. */
uint32_t cl_gc_fwd_fail_over = 0;

/* Track last GC cycle at which compaction ran — prevents infinite loops
 * when the heap is genuinely full (no fragmentation to reclaim). */
static uint32_t gc_last_compact_cycle = 0xFFFFFFFF;

/* Number of sweep-only GCs (cl_gc) run since the last compaction.  A sweep
 * never resets the bump pointer, so once the bump is exhausted a run of
 * sweeps makes no lasting progress: each just re-coalesces the free list and
 * the next allocation sweeps again.  When this counter crosses
 * GC_SWEEPS_BEFORE_COMPACT (and the heap is not genuinely near-full) cl_alloc
 * forces a compaction, which resets the bump pointer and defragments — escaping
 * the "sweep-forever" regime.  Reset to 0 by cl_gc_compact. */
static uint32_t gc_sweeps_since_compact = 0;


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
static CL_Obj *global_roots[CL_MAX_GLOBAL_ROOTS];
static int n_global_roots = 0;
#ifdef DEBUG_GC
/* Registration sites, recorded by the cl_gc_register_root macro so the
 * image writer can name late-registered roots (mem.h). */
static const char *global_root_files[CL_MAX_GLOBAL_ROOTS];
static int         global_root_lines[CL_MAX_GLOBAL_ROOTS];
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
static void gc_tlab_reset_all(void)
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
static void gc_tlab_reset_all(void) {}
#endif /* CL_TLAB */

#ifdef CL_GENGC
/* ================================================================
 * Generational GC (CL_GENGC) — sliding nursery with promotion-by-
 * watermark and hardware dirty-page tracking.  Design and the full
 * interactions audit: specs/generational-gc.md.
 *
 * Arena layout:   [CL_ALIGN, gen_old_top)   old space  (dense, RO)
 *                 [gen_old_top, arena_size) nursery    (bump, RW)
 *
 * Invariants (all load-bearing):
 *  - ALL allocation bumps inside the nursery; the free list is unused
 *    while gen mode is enabled (majors are always compactions).
 *  - Sticky marks: every live old object keeps its header mark bit SET
 *    between collections, so gc_mark_obj's "skip if marked" check
 *    terminates a minor trace at the old generation with no new code.
 *    Nursery objects are born unmarked.
 *  - Old space is mprotect-ed read-only after every collection; the
 *    write-watch fault handler records first-stores per page in
 *    gen_dirty.  A page that is clean at the next minor GC cannot
 *    reference any object allocated after the previous one.
 *  - Old space is DENSE (no free blocks): built only by compaction and
 *    by sequential promotion, so a linear header walk from any known
 *    object start stays in sync.  gen_cross[] (crossing map) gives each
 *    page's first object start so single dirty pages can be walked
 *    precisely without scanning the whole heap.
 * ================================================================ */

#define GEN_CROSS_NONE 0xFFFFFFFFu

static int      gen_enabled = 0;   /* runtime switch (mmap+watch OK, env) */
static uint32_t gen_old_top = CL_ALIGN;  /* old/nursery watermark */
static volatile uint8_t *gen_dirty = NULL;   /* 1 bit per arena page */
static uint32_t *gen_cross = NULL; /* per page: first object start offset */
static uint32_t gen_page = 0;      /* platform page size */
static uint32_t gen_npages = 0;    /* arena pages */
static int      gen_protected = 0; /* old space currently read-only */
static int      gen_arena_pages = 0; /* arena from platform_alloc_pages */

/* Diagnostics (ext:%gengc-stats) */
static uint32_t gen_minor_count = 0;
static uint64_t gc_time_minor_us = 0;
static uint64_t gen_promoted_bytes = 0;
static uint32_t gen_dirty_last = 0;

/* Rehash work list: post-slide offsets of hash tables encountered on
 * dirty pages / among survivors during a minor cycle.  Grown on demand;
 * survives across cycles (capacity only). */
static uint32_t *gen_ht_list = NULL;
static uint32_t gen_ht_count = 0, gen_ht_cap = 0;

/* Survivor list: offsets of live nursery objects, recorded by gc_mark_obj
 * while gen_minor_active — the whole point is that a minor cycle touches
 * only LIVE young objects, never walking the (dominant) dead nursery
 * space.  gen_live_new[] holds each survivor's assigned post-slide offset
 * (parallel array, filled after the list is sorted); gc_forward binary-
 * searches the pair during a minor instead of using a nursery-sized
 * forwarding table (which would cost an ~O(nursery) allocate+zero per
 * cycle).  Capacity persists across cycles. */
static uint32_t *gen_live = NULL;
static uint32_t *gen_live_new = NULL;
static uint32_t gen_live_count = 0, gen_live_cap = 0;
static int gen_minor_active = 0;   /* mark phase records survivors */
static int gen_minor_fwd = 0;      /* gc_forward uses the survivor list */
static int gen_live_oom = 0;       /* list growth failed — escalate */

/* Young finalizable objects (streams/locks/condvars/threads/bytecode/
 * foreign pointers), recorded at allocation time.  A minor never walks
 * dead nursery space, so without this list a dead young lock's platform
 * handle would leak until the next major.  Entries are dropped every
 * minor: dead ones are finalized, survivors are old afterwards (the next
 * major's full walk owns them). */
static uint32_t *gen_fin = NULL;
static uint32_t gen_fin_count = 0, gen_fin_cap = 0;

/* Types gc_finalize_dead releases external resources for. */
static int gen_type_finalizable(uint8_t type)
{
    return type == TYPE_BYTECODE || type == TYPE_STREAM ||
           type == TYPE_FOREIGN_POINTER || type == TYPE_THREAD;
}

static void gen_live_note(uint32_t off)
{
    if (gen_live_count == gen_live_cap) {
        uint32_t ncap = gen_live_cap ? gen_live_cap * 2 : 4096;
        uint32_t *nl = (uint32_t *)platform_alloc(ncap * sizeof(uint32_t));
        uint32_t *nn = (uint32_t *)platform_alloc(ncap * sizeof(uint32_t));
        if (!nl || !nn) {
            if (nl) platform_free(nl);
            if (nn) platform_free(nn);
            gen_live_oom = 1;
            return;
        }
        if (gen_live) {
            memcpy(nl, gen_live, gen_live_count * sizeof(uint32_t));
            platform_free(gen_live);
            platform_free(gen_live_new);
        }
        gen_live = nl;
        gen_live_new = nn;
        gen_live_cap = ncap;
    }
    gen_live[gen_live_count++] = off;
}

/* Record a freshly allocated finalizable object.  Callers on the lock-free
 * TLAB path pass locked=0 and this takes alloc_mutex itself (finalizable
 * allocations are rare — streams, locks — so the cost is irrelevant). */
static void gen_fin_note(uint32_t off, int need_lock)
{
    if (need_lock) platform_mutex_lock(alloc_mutex);
    if (gen_fin_count == gen_fin_cap) {
        uint32_t ncap = gen_fin_cap ? gen_fin_cap * 2 : 1024;
        uint32_t *nf = (uint32_t *)platform_alloc(ncap * sizeof(uint32_t));
        if (!nf) {
            /* Degraded: the object's external resources are reclaimed at
             * the next major instead. */
            if (need_lock) platform_mutex_unlock(alloc_mutex);
            return;
        }
        if (gen_fin) {
            memcpy(nf, gen_fin, gen_fin_count * sizeof(uint32_t));
            platform_free(gen_fin);
        }
        gen_fin = nf;
        gen_fin_cap = ncap;
    }
    gen_fin[gen_fin_count++] = off;
    if (need_lock) platform_mutex_unlock(alloc_mutex);
}

int cl_gengc_enabled(void) { return gen_enabled; }

/* Protect [0, gen_old_top) read-only (page-truncated).  The straddling
 * page (old tail + nursery head) stays writable — mark it dirty so the
 * next minor scans its old objects unconditionally. */
static void gen_protect_old(void)
{
    uint32_t ro_bytes;
    if (!gen_enabled) return;
    ro_bytes = (gen_old_top / gen_page) * gen_page;
    if (ro_bytes > 0)
        platform_page_protect(cl_heap.arena, ro_bytes, 1);
    if (gen_old_top > ro_bytes) {
        uint32_t p = gen_old_top / gen_page;
        gen_dirty[p >> 3] |= (uint8_t)(1u << (p & 7u));
    }
    gen_protected = 1;
}

/* Drop protection over the whole arena (GC passes write mark bits and
 * slide anywhere; re-init/shutdown must leave pages writable). */
static void gen_unprotect_all(void)
{
    if (gen_protected) {
        platform_page_protect(cl_heap.arena, gen_npages * gen_page, 0);
        gen_protected = 0;
    }
}

/* Record obj_offset as a (possibly) first object start in its page. */
static void gen_cross_note(uint32_t obj_offset)
{
    uint32_t p = obj_offset / gen_page;
    if (gen_cross[p] == GEN_CROSS_NONE || obj_offset < gen_cross[p])
        gen_cross[p] = obj_offset;
}

/* Offset of the object COVERING byte `pos` (pos < gen_old_top): start at
 * the nearest page at/below pos with a recorded first start, then walk
 * forward.  Old space is dense, so the walk stays header-synced. */
static uint32_t gen_covering_object(uint32_t pos)
{
    uint32_t p = pos / gen_page;
    uint32_t off;
    while (gen_cross[p] == GEN_CROSS_NONE || gen_cross[p] > pos) {
        if (p == 0) return CL_ALIGN;   /* degenerate: walk from arena start */
        p--;
    }
    off = gen_cross[p];
    for (;;) {
        uint32_t size = CL_HDR_SIZE(cl_heap.arena + off);
        if (size == 0) return pos;     /* desync guard — caller re-checks */
        if (off + size > pos) return off;
        off += size;
    }
}

void cl_gc_pretouch(const void *ptr, uint32_t len)
{
    uint32_t off, end, p, plast;
    const uint8_t *cp = (const uint8_t *)ptr;
    if (!gen_enabled || !gen_protected || len == 0) return;
    if (cp < cl_heap.arena) return;
    off = (uint32_t)(cp - cl_heap.arena);
    if (off >= gen_old_top) return;            /* nursery/beyond: RW */
    end = off + len;
    if (end > gen_old_top) end = gen_old_top;
    p = off / gen_page;
    plast = (end - 1) / gen_page;
    for (; p <= plast; p++)
        gen_dirty[p >> 3] |= (uint8_t)(1u << (p & 7u));
    platform_page_protect(cl_heap.arena + (off / gen_page) * gen_page,
                          (plast - off / gen_page + 1) * gen_page, 0);
}

void cl_gengc_stats(uint32_t *minors, uint64_t *minor_us,
                    uint64_t *promoted_bytes, uint32_t *old_top,
                    uint32_t *dirty_last)
{
    if (minors)         *minors         = gen_minor_count;
    if (minor_us)       *minor_us       = gc_time_minor_us;
    if (promoted_bytes) *promoted_bytes = gen_promoted_bytes;
    if (old_top)        *old_top        = gen_old_top;
    if (dirty_last)     *dirty_last     = gen_dirty_last;
}

/* Append a (forwarded) hashtable offset to the minor rehash work list. */
static void gen_ht_note(uint32_t off)
{
    if (gen_ht_count == gen_ht_cap) {
        uint32_t ncap = gen_ht_cap ? gen_ht_cap * 2 : 64;
        uint32_t *nl = (uint32_t *)platform_alloc(ncap * sizeof(uint32_t));
        if (!nl) return;  /* degraded: table missed — rehashed on major */
        if (gen_ht_list) {
            memcpy(nl, gen_ht_list, gen_ht_count * sizeof(uint32_t));
            platform_free(gen_ht_list);
        }
        gen_ht_list = nl;
        gen_ht_cap = ncap;
    }
    gen_ht_list[gen_ht_count++] = off;
}

/* Tear down / reset all generational state (heap re-init, shutdown). */
static void gen_reset(void)
{
    gen_unprotect_all();
    platform_write_watch_remove();
    if (gen_dirty) { platform_free((void *)gen_dirty); gen_dirty = NULL; }
    if (gen_cross) { platform_free(gen_cross); gen_cross = NULL; }
    if (gen_ht_list) {
        platform_free(gen_ht_list);
        gen_ht_list = NULL;
        gen_ht_cap = gen_ht_count = 0;
    }
    if (gen_live) {
        platform_free(gen_live);
        platform_free(gen_live_new);
        gen_live = gen_live_new = NULL;
        gen_live_cap = gen_live_count = 0;
    }
    if (gen_fin) {
        platform_free(gen_fin);
        gen_fin = NULL;
        gen_fin_cap = gen_fin_count = 0;
    }
    gen_minor_active = 0;
    gen_minor_fwd = 0;
    gen_live_oom = 0;
    gen_enabled = 0;
    gen_old_top = CL_ALIGN;
    gen_page = gen_npages = 0;
    gen_minor_count = 0;
    gc_time_minor_us = 0;
    gen_promoted_bytes = 0;
    gen_dirty_last = 0;
}
#endif /* CL_GENGC */

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
     * gc_mark_thread_roots walks them unconditionally.  Same bug class as
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

/* Persistent JIT-stack scan candidate buffer — defined here so
 * cl_mem_shutdown can free it; helpers and full doc live with
 * gc_scan_jit_native_stack below. */
static uint32_t        *jit_scan_cand_buf = NULL;
static int              jit_scan_cand_cap = 0;

/* Persistent buffer for the deduplicated registered-root forwarding pass
 * (see gc_update_registered_roots below) — defined here so
 * cl_mem_shutdown can free it. */
static CL_Obj         **gc_root_slot_buf = NULL;
static uint32_t         gc_root_slot_cap = 0;

/* Pinned-object set for the moving compactor (option B).  The mark-phase
 * conservative JIT-stack scan records, in ascending arena-offset order,
 * every *validated* real-object offset referenced from the current
 * thread's JIT m68k stack.  gc_compute_forwarding keeps these objects at
 * their current offset (forwarding = identity) so the JIT's spilled
 * operand pointers and any C-stack words that merely *look* like heap
 * offsets stay valid without rewriting the C stack.  This replaces the
 * old gc_forward_jit_native_stack writeback, which could mis-rewrite a
 * coincidental-integer C-stack word that happened to equal a live
 * object's offset (a layout-roulette corruption — see
 * specs/native-backend.md §"GC interaction").  Rebuilt every mark; only
 * non-empty when a JIT frame is live (jit_depth > 0). */
static uint32_t        *jit_pinned       = NULL;
static int              jit_pinned_count = 0;
static int              jit_pinned_cap   = 0;

/* Set when jit_pin_record could not grow jit_pinned[] (platform_alloc
 * OOM): the pinned set is incomplete, so cl_gc_compact MUST NOT move
 * anything this cycle — a live JIT frame's spilled C-stack word would
 * keep the old offset of a relocated object.  Reset by gc_mark. */
static int              gc_jit_pin_oom   = 0;

/* Free-list snapshot for the conservative JIT-stack scan.  A free block's
 * header word is its raw size (<= CL_HDR_SIZE_MASK), which reads as an
 * UNMARKED TYPE_CONS — so a stale spill word holding a freed offset would
 * pass phase-2 header validation, and gc_mark_obj would mark the free
 * block live while gc_mark_children chases its next_offset link and
 * poisoned payload as car/cdr.  The scan must therefore skip offsets
 * that are on the free list.  Collected (sorted ascending) at most once
 * per GC cycle, lazily on the first scan; the free list cannot change
 * during STW marking.  On snapshot OOM, membership falls back to a
 * linear free-list walk per matched candidate. */
static uint32_t        *jit_scan_free_snap  = NULL;
static int              jit_scan_free_count = 0;
static int              jit_scan_free_cap   = 0;
static int              jit_scan_free_valid = 0;

/* Set during a compaction's reference-update pass when at least one
 * CL_Obj immediate baked into JIT native code was rewritten in place.
 * cl_gc_compact flushes the CPU caches once at the end so 68040/060
 * don't execute stale instruction-cache copies of the patched code. */
static int gc_native_code_patched = 0;

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
static void gc_mark_stack_release(void)
{
    if (gc_mark_stack != gc_mark_stack_initial) {
        platform_free(gc_mark_stack);
        gc_mark_stack = gc_mark_stack_initial;
    }
    gc_mark_stack_cap = CL_GC_MARK_STACK_SIZE;
    gc_mark_top = 0;
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

/* --- Shared object-slot walker (used by mark and compaction-update) ---------
 * gc_mark_children and gc_update_children walk the identical per-type set of
 * CL_Obj slots; they differ only in the operation applied to each slot
 * (mark-push a value vs forward a slot in place) and in two type-specific
 * tails.  The layout is therefore written ONCE here as an X-macro and each
 * function instantiates it by defining GC_VISIT plus the two _TAIL hooks.
 * Because it is macro expansion the generated code is identical to two
 * hand-written switches — zero per-object overhead on this hot GC path.
 *
 * Callers must define, before invoking:
 *   GC_VISIT(slot)          operate on one CL_Obj lvalue slot
 *   GC_BYTECODE_TAIL(bc)    extra work for TYPE_BYTECODE (native reloc forward)
 *   GC_STREAM_TAIL(st)      extra work for TYPE_STREAM   (outbuf pin)
 * and #undef them afterward. */
#define GC_WALK_OBJ_CHILDREN(ptr, type)                                       \
    switch (type) {                                                           \
    case TYPE_CONS: {                                                         \
        CL_Cons *c = (CL_Cons *)(ptr);                                        \
        GC_VISIT(c->car);                                                     \
        GC_VISIT(c->cdr);                                                     \
        break;                                                               \
    }                                                                        \
    case TYPE_SYMBOL: {                                                       \
        CL_Symbol *s = (CL_Symbol *)(ptr);                                    \
        GC_VISIT(s->name);                                                    \
        if (s->value != CL_UNBOUND) GC_VISIT(s->value);                       \
        if (s->function != CL_UNBOUND) GC_VISIT(s->function);                 \
        GC_VISIT(s->plist);                                                   \
        GC_VISIT(s->package);                                                 \
        break;                                                               \
    }                                                                        \
    case TYPE_FUNCTION: {                                                     \
        CL_Function *f = (CL_Function *)(ptr);                                \
        GC_VISIT(f->name);                                                    \
        break;                                                               \
    }                                                                        \
    case TYPE_CLOSURE: {                                                      \
        CL_Closure *cl = (CL_Closure *)(ptr);                                 \
        uint32_t size = CL_HDR_SIZE(ptr);                                     \
        uint32_t n_upvals = (size - sizeof(CL_Closure)) / sizeof(CL_Obj);     \
        uint32_t i;                                                           \
        GC_VISIT(cl->bytecode);                                               \
        for (i = 0; i < n_upvals; i++)                                        \
            GC_VISIT(cl->upvalues[i]);                                        \
        break;                                                               \
    }                                                                        \
    case TYPE_BYTECODE: {                                                     \
        CL_Bytecode *bc = (CL_Bytecode *)(ptr);                               \
        uint16_t i;                                                           \
        GC_VISIT(bc->name);                                                   \
        GC_VISIT(bc->source_lambda_list);                                     \
        for (i = 0; i < bc->n_constants; i++)                                 \
            GC_VISIT(bc->constants[i]);                                       \
        if (bc->key_syms) {                                                   \
            for (i = 0; i < bc->n_keys; i++)                                  \
                GC_VISIT(bc->key_syms[i]);                                    \
        }                                                                     \
        GC_BYTECODE_TAIL(bc);                                                 \
        break;                                                               \
    }                                                                        \
    case TYPE_VECTOR: {                                                       \
        CL_Vector *v = (CL_Vector *)(ptr);                                    \
        uint32_t i;                                                           \
        if (v->flags & CL_VEC_FLAG_DISPLACED) {                              \
            GC_VISIT(v->data[CL_DISP_BASE_IDX(v)]);                           \
        } else {                                                             \
            uint32_t n_entries = (v->rank > 1) ? (uint32_t)v->rank + v->length \
                                               : v->length;                   \
            for (i = 0; i < n_entries; i++)                                   \
                GC_VISIT(v->data[i]);                                         \
        }                                                                    \
        break;                                                               \
    }                                                                        \
    case TYPE_PACKAGE: {                                                      \
        CL_Package *p = (CL_Package *)(ptr);                                  \
        GC_VISIT(p->name);                                                    \
        GC_VISIT(p->symbols);                                                 \
        GC_VISIT(p->use_list);                                                \
        GC_VISIT(p->nicknames);                                               \
        GC_VISIT(p->local_nicknames);                                         \
        GC_VISIT(p->shadowing_symbols);                                       \
        GC_VISIT(p->exported_symbols);                                        \
        GC_VISIT(p->bindings);                                                \
        break;                                                               \
    }                                                                        \
    case TYPE_HASHTABLE: {                                                    \
        CL_Hashtable *ht = (CL_Hashtable *)(ptr);                             \
        uint32_t i;                                                           \
        GC_VISIT(ht->bucket_vec);                                             \
        if (!CL_NULL_P(ht->bucket_vec)) {                                     \
            /* Buckets live in the external vector; walking it covers them */ \
        } else {                                                             \
            for (i = 0; i < ht->bucket_count; i++)                            \
                GC_VISIT(ht->buckets[i]);                                     \
        }                                                                    \
        break;                                                               \
    }                                                                        \
    case TYPE_CONDITION: {                                                    \
        CL_Condition *cond = (CL_Condition *)(ptr);                           \
        GC_VISIT(cond->type_name);                                            \
        GC_VISIT(cond->slots);                                                \
        GC_VISIT(cond->report_string);                                       \
        break;                                                               \
    }                                                                        \
    case TYPE_RESTART: {                                                      \
        CL_Restart *r = (CL_Restart *)(ptr);                                  \
        GC_VISIT(r->name);                                                    \
        GC_VISIT(r->function);                                                \
        GC_VISIT(r->report);                                                  \
        GC_VISIT(r->interactive);                                             \
        GC_VISIT(r->test);                                                    \
        GC_VISIT(r->tag);                                                     \
        break;                                                               \
    }                                                                        \
    case TYPE_FFI_STUB: {                                                     \
        CL_FfiStub *fs = (CL_FfiStub *)(ptr);                                 \
        GC_VISIT(fs->name);                                                   \
        GC_VISIT(fs->aux);                                                    \
        break;                                                               \
    }                                                                        \
    case TYPE_STRUCT: {                                                       \
        CL_Struct *st = (CL_Struct *)(ptr);                                   \
        uint32_t i;                                                           \
        GC_VISIT(st->type_desc);                                              \
        for (i = 0; i < st->n_slots; i++)                                     \
            GC_VISIT(st->slots[i]);                                           \
        break;                                                               \
    }                                                                        \
    case TYPE_STREAM: {                                                       \
        CL_Stream *st = (CL_Stream *)(ptr);                                   \
        GC_VISIT(st->string_buf);                                             \
        GC_VISIT(st->element_type);                                           \
        GC_STREAM_TAIL(st);                                                   \
        break;                                                               \
    }                                                                        \
    case TYPE_RATIO: {                                                        \
        CL_Ratio *r = (CL_Ratio *)(ptr);                                      \
        GC_VISIT(r->numerator);                                               \
        GC_VISIT(r->denominator);                                             \
        break;                                                               \
    }                                                                        \
    case TYPE_COMPLEX: {                                                      \
        CL_Complex *cx = (CL_Complex *)(ptr);                                 \
        GC_VISIT(cx->realpart);                                               \
        GC_VISIT(cx->imagpart);                                              \
        break;                                                               \
    }                                                                        \
    case TYPE_PATHNAME: {                                                     \
        CL_Pathname *pn = (CL_Pathname *)(ptr);                               \
        GC_VISIT(pn->host);                                                   \
        GC_VISIT(pn->device);                                                 \
        GC_VISIT(pn->directory);                                             \
        GC_VISIT(pn->name);                                                   \
        GC_VISIT(pn->type);                                                   \
        GC_VISIT(pn->version);                                                \
        break;                                                               \
    }                                                                        \
    case TYPE_CELL: {                                                         \
        CL_Cell *cell = (CL_Cell *)(ptr);                                     \
        GC_VISIT(cell->value);                                                \
        break;                                                               \
    }                                                                        \
    case TYPE_THREAD: {                                                       \
        CL_ThreadObj *to = (CL_ThreadObj *)(ptr);                            \
        GC_VISIT(to->name);                                                   \
        GC_VISIT(to->result);                                                 \
        break;                                                               \
    }                                                                        \
    case TYPE_LOCK: {                                                         \
        CL_Lock *lk = (CL_Lock *)(ptr);                                       \
        GC_VISIT(lk->name);                                                   \
        break;                                                               \
    }                                                                        \
    case TYPE_CONDVAR: {                                                      \
        CL_CondVar *cv = (CL_CondVar *)(ptr);                                 \
        GC_VISIT(cv->name);                                                   \
        break;                                                               \
    }                                                                        \
    case TYPE_STRING:                                                         \
    case TYPE_BIGNUM:                                                         \
    case TYPE_SINGLE_FLOAT:                                                    \
    case TYPE_DOUBLE_FLOAT:                                                    \
    case TYPE_RANDOM_STATE:                                                    \
    case TYPE_BIT_VECTOR:                                                      \
    case TYPE_BYTE_VECTOR:                                                     \
    case TYPE_FOREIGN_POINTER:                                                 \
    CL_GC_WIDE_STRING_CASE                                                     \
        /* No CL_Obj children — raw numeric/state/byte data */                \
        break;                                                               \
    default:                                                                  \
        break;                                                               \
    }

#ifdef CL_WIDE_STRINGS
#define CL_GC_WIDE_STRING_CASE case TYPE_WIDE_STRING:
#else
#define CL_GC_WIDE_STRING_CASE
#endif

static void gc_mark_children(void *ptr, uint8_t type)
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

/* Conservatively scan a thread's m68k stack for CL_Obj values
 * spilled by JIT'd code.
 *
 * Two-phase to avoid the moving-GC corruption hazard:
 *
 *   Phase 1 — collect: walk 4-byte-aligned words in [scan_lo,
 *   scan_hi).  A word w is a *candidate* if CL_HEAP_P(w) and
 *   w < arena_size.  Collected into a stack-local bounded buffer.
 *
 *   Phase 2 — validate-and-mark: walk the arena bump-front.  At
 *   each real header offset X, binary-search the sorted candidate
 *   buffer; if X is present, call gc_mark_obj(X).  This guarantees
 *   we only call CL_HDR_SET_MARK on offsets that are actual object
 *   starts — phantom marks on mid-object bytes (which would corrupt
 *   neighbouring data and break the moving compactor's relocations)
 *   are impossible.
 *
 * With validation in place the compaction inhibit (compaction
 * suppressed while cl_jit_active_threads > 0) is no longer
 * required for correctness.
 *
 * Capacity policy: the candidate buffer is a persistent, grow-on-demand
 * allocation held in mem.c static storage.  Pre-sized once per call to
 * the scan window's word count (an exact upper bound on the number of
 * candidates), so overflow is impossible by construction.  Across GC
 * cycles the buffer is reused; it only grows when a larger scan window
 * is encountered, so steady state is zero allocations per GC.
 *
 * Only runs when t->jit_depth > 0 and t is the current thread —
 * non-current threads stop at bytecode-VM safepoints where their
 * jit_depth is 0 by construction (JIT'd code does not yet emit its
 * own safepoints).
 *
 * See specs/native-backend.md §"GC interaction" option A → B-lite. */

static int jit_scan_cand_reserve(int need)
{
    int new_cap;
    uint32_t *buf;
    if (jit_scan_cand_cap >= need) return 1;
    new_cap = jit_scan_cand_cap ? jit_scan_cand_cap : 256;
    while (new_cap < need) new_cap *= 2;
    buf = (uint32_t *)platform_alloc((unsigned long)new_cap * sizeof(uint32_t));
    if (!buf) return 0;
    if (jit_scan_cand_buf) platform_free(jit_scan_cand_buf);
    jit_scan_cand_buf = buf;
    jit_scan_cand_cap = new_cap;
    return 1;
}

static int cand_cmp(const void *a, const void *b)
{
    uint32_t aa = *(const uint32_t *)a;
    uint32_t bb = *(const uint32_t *)b;
    return (aa < bb) ? -1 : (aa > bb) ? 1 : 0;
}

/* Append a validated pinned offset.  Called from gc_scan_jit_native_stack's
 * ascending arena walk, so the array stays sorted per scan; skip
 * consecutive duplicates (multiple stack slots can reference the same
 * object).  On OOM the object is still marked live (gc_mark_obj already
 * ran) but it is NOT safe to simply leave it unpinned: a compaction would
 * relocate it while the JIT frame's spilled offset stays stale — silent
 * corruption on resume.  Set gc_jit_pin_oom so cl_gc_compact degrades to
 * a non-moving mark+sweep for this cycle. */
static void jit_pin_record(uint32_t offset)
{
    if (jit_pinned_count > 0 && jit_pinned[jit_pinned_count - 1] == offset)
        return;
    if (jit_pinned_count >= jit_pinned_cap) {
        int new_cap = jit_pinned_cap ? jit_pinned_cap * 2 : 256;
        uint32_t *buf = (uint32_t *)platform_alloc(
            (unsigned long)new_cap * sizeof(uint32_t));
        if (!buf) {
            static int pin_oom_warned = 0;
            gc_jit_pin_oom = 1;
            if (!pin_oom_warned) {
                pin_oom_warned = 1;
                platform_write_string(
                    "GC: JIT pin-table allocation failed - compaction "
                    "suppressed for this cycle (mark+sweep only)\n");
            }
            return;
        }
        if (jit_pinned) {
            memcpy(buf, jit_pinned, (size_t)jit_pinned_count * sizeof(uint32_t));
            platform_free(jit_pinned);
        }
        jit_pinned = buf;
        jit_pinned_cap = new_cap;
    }
    jit_pinned[jit_pinned_count++] = offset;
}

static int cand_bsearch(const uint32_t *arr, int n, uint32_t key)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (arr[mid] == key) return 1;
        if (arr[mid] < key) lo = mid + 1;
        else hi = mid - 1;
    }
    return 0;
}

/* Snapshot the free list into jit_scan_free_snap, sorted ascending.
 * Leaves jit_scan_free_valid = 0 on OOM (callers fall back to a linear
 * free-list walk).  Called at most once per GC cycle — gc_mark resets
 * the valid flag alongside jit_pinned_count. */
static void jit_scan_collect_free_snapshot(void)
{
    uint32_t cur;
    int n = 0;

    for (cur = cl_heap.free_list; cur;
         cur = ((CL_FreeBlock *)(cl_heap.arena + cur))->next_offset)
        n++;

    if (n > jit_scan_free_cap) {
        int new_cap = jit_scan_free_cap ? jit_scan_free_cap : 256;
        uint32_t *buf;
        while (new_cap < n) new_cap *= 2;
        buf = (uint32_t *)platform_alloc((unsigned long)new_cap * sizeof(uint32_t));
        if (!buf) return;
        if (jit_scan_free_snap) platform_free(jit_scan_free_snap);
        jit_scan_free_snap = buf;
        jit_scan_free_cap = new_cap;
    }

    n = 0;
    for (cur = cl_heap.free_list; cur;
         cur = ((CL_FreeBlock *)(cl_heap.arena + cur))->next_offset)
        jit_scan_free_snap[n++] = cur;

    if (n > 1)
        qsort(jit_scan_free_snap, (size_t)n, sizeof(uint32_t), cand_cmp);
    jit_scan_free_count = n;
    jit_scan_free_valid = 1;
}

/* Is `offset` the start of a free-list block?  Uses the per-cycle
 * snapshot when available, else walks the free list directly. */
static int gc_offset_is_free_block(uint32_t offset)
{
    if (jit_scan_free_valid)
        return cand_bsearch(jit_scan_free_snap, jit_scan_free_count, offset);
    {
        uint32_t cur;
        for (cur = cl_heap.free_list; cur;
             cur = ((CL_FreeBlock *)(cl_heap.arena + cur))->next_offset) {
            if (cur == offset) return 1;
        }
    }
    return 0;
}

static void gc_scan_jit_native_stack(CL_Thread *t)
{
    uint32_t *candidates;
    int n_cand = 0;
    int upper;
    int cand_cap;
    char *scan_lo;
    char *scan_hi;
    uintptr_t addr;
    uint8_t *p;
    uint8_t *end;
    static int oom_warned = 0;

    if (t->jit_depth <= 0 || t->jit_stack_top == NULL) return;

    /* Lower bound of the scan window.
     *  - Current (compacting) thread: a local in THIS frame — strictly below
     *    any JIT'd frame on the C call chain leading here.
     *  - A stopped PEER thread (multi-thread STW): its own frozen stack
     *    pointer, captured when it parked at a safepoint / safe region.  We
     *    cannot use our own SP — the peer runs on a different C stack.  Without
     *    this, a peer parked inside JIT'd code had its spilled operands neither
     *    marked nor pinned, so the compactor relocated the referenced objects
     *    and the peer resumed with dangling offsets (the multi-thread JIT-vs-
     *    moving-GC corruption). */
    if (t == cl_get_current_thread()) {
        scan_lo = (char *)&scan_lo;
    } else {
        if (t->jit_park_sp == NULL) return;
        scan_lo = (char *)t->jit_park_sp;
    }
    scan_hi = (char *)t->jit_stack_top;
    if (scan_hi <= scan_lo) return;

    /* Round up to 4-byte alignment (CL_Obj is 32 bits). */
    addr = (uintptr_t)scan_lo;
    addr = (addr + 3u) & ~(uintptr_t)3u;
    if (addr + 4 > (uintptr_t)scan_hi) return;

    /* Exact upper bound: one slot per 4-byte aligned word in the window. */
    upper = (int)(((uintptr_t)scan_hi - addr) / 4);
    if (jit_scan_cand_reserve(upper)) {
        candidates = jit_scan_cand_buf;
        cand_cap = upper;
    } else {
        /* OOM fallback: a SKIPPED scan would leave JIT-only-reachable
         * objects unmarked — the following sweep frees them under the
         * live JIT frame (real corruption, not merely "less precise").
         * Degrade to chunked scanning through a small static buffer
         * instead: correct, needs no memory, costs one extra arena walk
         * per full chunk. */
        static uint32_t emergency_buf[256];
        if (!oom_warned) {
            oom_warned = 1;
            platform_write_string(
                "GC: JIT native-stack scan buffer allocation failed - "
                "falling back to chunked scanning (slower, still safe)\n");
        }
        candidates = emergency_buf;
        cand_cap = (int)(sizeof(emergency_buf) / sizeof(emergency_buf[0]));
    }

    /* Chunked scan: collect up to cand_cap candidate offsets, resolve
     * them against the arena, repeat until the window is exhausted.  The
     * normal path (cand_cap == upper) runs exactly one iteration. */
    while (addr + 4 <= (uintptr_t)scan_hi) {
        /* Phase 1: collect candidate offsets. */
        n_cand = 0;
        for (; addr + 4 <= (uintptr_t)scan_hi && n_cand < cand_cap; addr += 4) {
            CL_Obj w = *(const CL_Obj *)(uintptr_t)addr;
            if (CL_NULL_P(w) || CL_FIXNUM_P(w) || CL_CHAR_P(w)) continue;
            if (w >= cl_heap.arena_size) continue;
            candidates[n_cand++] = (uint32_t)w;
        }

        if (n_cand == 0) break;   /* window exhausted with no candidates */

        /* Sort candidates so the arena walk can binary-search. */
        qsort(candidates, n_cand, sizeof(candidates[0]), cand_cmp);

        /* Snapshot the free list once per GC cycle so the walk below can
         * reject candidates that point at free blocks (their header word is
         * a raw size that masquerades as an unmarked TYPE_CONS). */
        if (!jit_scan_free_valid)
            jit_scan_collect_free_snapshot();

        /* Phase 2: validate each candidate as a block start, then mark
         * and pin the real objects among them.
         *
         * With the block-start index (classic collector) each candidate
         * costs at most a page of header walking — ascending, with the
         * cursor carried between candidates.  Without it (gen mode, or
         * the index could not be allocated) fall back to the original
         * full walk of the arena bump-front from CL_ALIGN (offset 0 is
         * reserved for NIL) by header size, matching gc_sweep's
         * iteration, binary-searching the candidates at every header. */
        if (gc_hdr_page) {
            GC_HdrCursor cur;
            int i;
            cur.blk = cur.nxt = 0;
            for (i = 0; i < n_cand; i++) {
                uint32_t offset = candidates[i];
                if (i > 0 && offset == candidates[i - 1]) continue;
                if (!gc_hdr_is_block_start(offset, &cur)) continue;
                if (gc_offset_is_free_block(offset)) continue;
                gc_mark_obj((CL_Obj)offset);
                /* Pin it: the compactor must not move an object reachable
                 * only through a conservative (offset-valued) C-stack
                 * reference, or the JIT's spilled pointer would dangle.
                 * Ascending within one chunk; gc_compute_forwarding sorts
                 * the aggregate (chunks/threads can interleave). */
                jit_pin_record(offset);
            }
#if defined(DEBUG_GC) || defined(DEBUG_GC_STRESS)
            /* Cross-check the index against the ground truth: the full
             * walk must accept exactly the same candidates.  A mismatch
             * is an index-invariant bug (a header written on virgin bytes
             * without a note, or a note missed by a pass that rewrites
             * headers) — the kind that would otherwise surface as a
             * phantom mark or a missed pin. */
            gc_jit_scan_verify_index(candidates, n_cand);
#endif
        } else {
            p   = cl_heap.arena + CL_ALIGN;
            end = cl_heap.arena + cl_heap.bump;
            while (p < end) {
                uint32_t size = CL_HDR_SIZE(p);
                uint32_t offset;
                if (size == 0) break;       /* defensive: malformed header */
                offset = (uint32_t)(p - cl_heap.arena);
                if (cand_bsearch(candidates, n_cand, offset) &&
                    !gc_offset_is_free_block(offset)) {
                    gc_mark_obj((CL_Obj)offset);
                    jit_pin_record(offset);
                }
                p += size;
            }
        }
    }
}

/* Audit the block-start index against the arena: for every page whose
 * first byte lies below the bump front, the entry (or the nearest lower
 * entry it defers to) must be a header on the linear chain from CL_ALIGN
 * at or below the page's first byte.  Returns the number of pages that
 * violate this, -1 when the index is absent (gen mode / init failure).
 * Diagnostic entry point (ext:%gc-audit-hdr-index and the unit tests);
 * the caller must hold the world stopped or be single-threaded. */
int cl_gc_audit_hdr_index(void)
{
    uint32_t off = CL_ALIGN;
    uint32_t page = 1;      /* page 0's first byte is NIL's reserved slot:
                             * its walk start is CL_ALIGN by definition */
    int violations = 0;

    if (!gc_hdr_page) return -1;

    /* One ascending walk over the real chain; `off` is the block covering
     * the page's first byte when we reach that page. */
    while (page < gc_hdr_npages && (page << GC_HDR_PAGE_SHIFT) < cl_heap.bump) {
        uint32_t page_start = page << GC_HDR_PAGE_SHIFT;
        uint32_t entry;
        for (;;) {
            uint32_t size = CL_HDR_SIZE(cl_heap.arena + off);
            if (size == 0) return violations + 1;   /* chain broken: bail */
            if (off + size > page_start) break;
            off += size;
        }
        /* `off` now covers page_start (off <= page_start < off+size).
         * The entry must be some header <= page_start: either `off`
         * itself or an earlier chain header (a split-behind entry).
         * Verify by walking from the entry to `off`. */
        entry = gc_hdr_walk_start(page);
        if (entry > page_start) {
            violations++;
        } else {
            uint32_t w = entry;
            while (w < off) {
                uint32_t size = CL_HDR_SIZE(cl_heap.arena + w);
                if (size == 0) { w = 0xFFFFFFFFu; break; }
                w += size;
            }
            if (w != off) violations++;
        }
        page++;
    }
    return violations;
}

#if defined(DEBUG_GC) || defined(DEBUG_GC_STRESS)
/* Self-check of the block-start index at the end of a collection.  Under
 * DEBUG_GC every time; under DEBUG_GC_STRESS (a compaction per
 * allocation) every 64th, so the stress suite keeps its pace while the
 * invariant is still checked thousands of times per run.  Fatal on a
 * violation: the scan would turn it into a phantom mark or a missed pin
 * on the Amiga, far from here. */
static void gc_hdr_index_selfcheck(void)
{
    int v;
    if (!gc_hdr_page) return;
#ifndef DEBUG_GC
    if (cl_heap.gc_count % 64 != 0) return;
#endif
    v = cl_gc_audit_hdr_index();
    if (v != 0) {
        char buf[96];
        snprintf(buf, sizeof(buf),
                 "FATAL: GC block-start index: %d page(s) violate the "
                 "invariant after GC #%lu\n", v, (unsigned long)cl_heap.gc_count);
        platform_write_string(buf);
        platform_flush_output();
        abort();
    }
}

/* Recompute the scan's accept set the original way (full arena walk) and
 * compare it with the index-based answers. */
static void gc_jit_scan_verify_index(const uint32_t *candidates, int n_cand)
{
    GC_HdrCursor cur;
    int i;
    int bad = 0;
    cur.blk = cur.nxt = 0;
    for (i = 0; i < n_cand; i++) {
        uint32_t offset = candidates[i];
        int by_index, by_walk = 0;
        uint8_t *p = cl_heap.arena + CL_ALIGN;
        uint8_t *end = cl_heap.arena + cl_heap.bump;
        if (i > 0 && offset == candidates[i - 1]) continue;
        by_index = gc_hdr_is_block_start(offset, &cur);
        while (p < end) {
            uint32_t size = CL_HDR_SIZE(p);
            uint32_t o = (uint32_t)(p - cl_heap.arena);
            if (size == 0) break;
            if (o == offset) { by_walk = 1; break; }
            if (o > offset) break;
            p += size;
        }
        if (by_index != by_walk) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "GC: block-start index disagrees with the arena walk at "
                     "offset 0x%08x (index %d, walk %d)\n",
                     (unsigned)offset, by_index, by_walk);
            platform_write_string(buf);
            bad++;
        }
    }
    if (bad) {
        platform_write_string("FATAL: GC block-start index corrupt\n");
        platform_flush_output();
        abort();
    }
}
#endif

/* Mark all per-thread roots for a single thread.
 * Called during STW GC — no locking needed, thread is stopped.
 *
 * We must #undef gc_root_count here because thread.h defines it as
 * (CT->gc_root_count), which collides with t->gc_root_count member access. */
#undef gc_root_count

/* Audit the GC root registries for double registration.
 *
 * gc_forward is NOT idempotent (the forwarding table is keyed by
 * pre-compaction offsets), so a CL_Obj slot reachable from two root
 * entries is forwarded twice on compaction: the second lookup maps the
 * already-forwarded offset through whatever object's OLD offset it now
 * coincides with, silently rewriting the root to an unrelated object.
 * Every slot must therefore appear at most once across global_roots and
 * all thread root stacks combined.  Returns the number of violations
 * and prints one line per offender so the source can be found.
 *
 * Walks cl_thread_list and every thread's gc_roots[]/gc_root_count, which
 * are otherwise only safe to read under a stop-the-world pause (peer
 * threads mutate their own root stack via CL_GC_PROTECT/UNPROTECT, and
 * thread create/exit mutates the list's `next` links).  Since this is a
 * diagnostic entry point reachable from Lisp at any time (ext:%gc-audit-roots),
 * bracket the walk with cl_gc_stop_the_world()/cl_gc_resume_the_world()
 * ourselves, matching the pattern in cl_gc_compact(). */
int cl_gc_audit_roots(void)
{
    int violations = 0;
    int i, j;
    CL_Thread *t, *t2;
    int multithread = (cl_thread_count > 1);

    if (multithread)
        cl_gc_stop_the_world();

    for (i = 0; i < n_global_roots; i++) {
        for (j = i + 1; j < n_global_roots; j++) {
            if (global_roots[i] == global_roots[j]) {
                fprintf(stderr, "GC root audit: address registered twice "
                        "in global roots\n");
                violations++;
            }
        }
        for (t = cl_thread_list; t; t = t->next) {
            for (j = 0; j < t->gc_root_count; j++) {
                if (t->gc_roots[j] == global_roots[i]) {
                    fprintf(stderr, "GC root audit: global root also on a "
                            "thread root stack (double-forward hazard)\n");
                    violations++;
                }
            }
        }
    }

    /* Duplicate address registered twice within the same thread's own
     * gc_roots[] stack (e.g. an accidental double CL_GC_PROTECT). */
    for (t = cl_thread_list; t; t = t->next) {
        for (i = 0; i < t->gc_root_count; i++) {
            for (j = i + 1; j < t->gc_root_count; j++) {
                if (t->gc_roots[i] == t->gc_roots[j]) {
                    fprintf(stderr, "GC root audit: address registered "
                            "twice on the same thread's root stack\n");
                    violations++;
                }
            }
        }
    }

    /* Same address registered on two different threads' root stacks. */
    for (t = cl_thread_list; t; t = t->next) {
        for (t2 = t->next; t2; t2 = t2->next) {
            for (i = 0; i < t->gc_root_count; i++) {
                for (j = 0; j < t2->gc_root_count; j++) {
                    if (t->gc_roots[i] == t2->gc_roots[j]) {
                        fprintf(stderr, "GC root audit: address registered "
                                "on two different thread root stacks "
                                "(double-forward hazard)\n");
                        violations++;
                    }
                }
            }
        }
    }

    /* Informational: roots aliasing an independently-forwarded thread
     * region (VM stack, mv_values, pending throw values, dyn/handler/
     * restart stacks, vm_extra_args, compiler chain).  Harmless by
     * construction — gc_update_registered_roots skips them — but each
     * marks a redundant CL_GC_PROTECT worth removing at the source.
     * Not counted as violations. */
    for (t = cl_thread_list; t; t = t->next) {
        for (i = 0; i < t->gc_root_count; i++) {
            if (root_slot_independently_forwarded(t->gc_roots[i])) {
                fprintf(stderr, "GC root audit: note - thread root #%d "
                        "aliases an independently-forwarded thread region "
                        "(redundant CL_GC_PROTECT; skipped by the root "
                        "dedup pass)\n", i);
            }
        }
    }

    if (multithread)
        cl_gc_resume_the_world();

    return violations;
}

static void gc_mark_thread_roots(CL_Thread *t)
{
    int i;

    /* GC root stack */
    for (i = 0; i < t->gc_root_count; i++) {
        GC_DBG_SRC("gc_roots", i);
        gc_mark_obj(*t->gc_roots[i]);
    }

    /* Dynamic binding stack (saved old values) */
    for (i = 0; i < t->dyn_top; i++) {
        GC_DBG_SRC("dyn_stack", i);
        gc_mark_obj(t->dyn_stack[i].symbol);
        gc_mark_obj(t->dyn_stack[i].old_value);
    }

    /* NLX stack (catch tags, results, and saved bytecodes).
     * NOTE: nlx_stack[i].mv_values are deliberately NOT marked: they are
     * written together with mv_count only in the zero-allocation window
     * between a throw's stash and its longjmp landing (immediately
     * consumed into cl_mv_values); outside that window mv_count is 1
     * from the frame push while mv_values[0] holds a stale/garbage word
     * — marking it would set mark bits at non-object-start offsets.
     * Values that live across an ALLOCATING unwind-protect cleanup are
     * parked in pending_mv_values / saved_pending_stack, marked below. */
    for (i = 0; i < t->nlx_top; i++) {
        /* Every frame below nlx_top must have all three initialized — a
         * C-pushed frame too (cl_nlx_frame_init_c): the slot's previous
         * occupant leaves stale offsets behind, and marking one corrupts
         * the arena walk.  The per-field source names the culprit. */
        GC_DBG_SRC("nlx_stack.tag", i);
        gc_mark_obj(t->nlx_stack[i].tag);
        GC_DBG_SRC("nlx_stack.result", i);
        gc_mark_obj(t->nlx_stack[i].result);
        GC_DBG_SRC("nlx_stack.bytecode", i);
        gc_mark_obj(t->nlx_stack[i].bytecode);
    }

    /* Handler stack */
    for (i = 0; i < t->handler_top; i++) {
        GC_DBG_SRC("handler_stack", i);
        gc_mark_obj(t->handler_stack[i].type_name);
        gc_mark_obj(t->handler_stack[i].handler);
    }

    /* Restart stack */
    for (i = 0; i < t->restart_top; i++) {
        GC_DBG_SRC("restart_stack", i);
        gc_mark_obj(t->restart_stack[i].name);
        gc_mark_obj(t->restart_stack[i].handler);
        gc_mark_obj(t->restart_stack[i].tag);
        gc_mark_obj(t->restart_stack[i].restart);
    }

    /* VM execution stack */
    if (t->vm.stack) {
        for (i = 0; i < t->vm.sp; i++) {
            GC_DBG_SRC("vm.stack", i);
            gc_mark_obj(t->vm.stack[i]);
        }
    }

    /* JIT native stack — conservative scan when this thread is
     * currently inside JIT'd code.  No-op otherwise. */
    gc_scan_jit_native_stack(t);

    /* Bytecode objects referenced by active VM frames */
    for (i = 0; i < t->vm.fp; i++) {
        GC_DBG_SRC("vm.frames.bytecode", i);
        gc_mark_obj(t->vm.frames[i].bytecode);
    }

    /* Multiple values and pending throw state */
    for (i = 0; i < CL_MAX_MV; i++) {
        GC_DBG_SRC("mv_values", i);
        gc_mark_obj(t->mv_values[i]);
    }
    /* NOTE: pre_call_mv_values is deliberately NOT marked (nor forwarded
     * in gc_update_thread_roots).  call_builtin (vm.c) snapshots mv_values
     * into it immediately before every builtin invocation, and the only
     * consumers (THROW's NLX capture paths in builtins_io.c) read it back
     * BEFORE any allocating call in that window — verified zero-alloc, so
     * no GC can observe a live-but-unmarked value there.  Outside that
     * window the array holds stale offsets from completed calls; marking
     * those would set mark bits at non-object-start offsets and corrupt
     * the arena walk (same hazard as nlx_stack[i].mv_values above).
     * REGRESSION HAZARD: if an NLX builtin ever allocates before
     * consuming pre_call_mv_values, these slots must instead be marked +
     * forwarded under a validity flag, like pending_mv_values below. */
    GC_DBG_SRC("pending_tag/value", 0);
    gc_mark_obj(t->pending_tag);
    gc_mark_obj(t->pending_value);
    /* UNWIND-PROTECT value records (OP_MV_SAVE): every used word is a
     * CL_Obj — the values, and their count as a fixnum — live across the
     * allocating cleanup forms. */
    for (i = 0; i < t->mv_save_top && i < CL_MV_SAVE_SIZE; i++) {
        GC_DBG_SRC("mv_save_buf", i);
        gc_mark_obj(t->mv_save_buf[i]);
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
            gc_mark_obj(t->pending_mv_values[i]);
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
        gc_mark_obj(t->saved_pending_stack[i].pending_tag);
        gc_mark_obj(t->saved_pending_stack[i].pending_value);
        for (m = 0; m < t->saved_pending_stack[i].pending_mv_count &&
                    m < CL_MAX_MV; m++)
            gc_mark_obj(t->saved_pending_stack[i].pending_mv_values[m]);
    }
    /* Compiler hand-off: holds a CL_Obj symbol across compile_expr's allocations
     * (set in compile_named_lambda/compile_defun, consumed in compile_lambda).
     * Must be marked AND updated or a compaction mid-lambda-compile leaves
     * bc->name a stale offset under GC stress. */
    GC_DBG_SRC("pending_lambda_name", 0);
    gc_mark_obj(t->pending_lambda_name);
    /* Foreign-callback boundary: the condition parked by a callback until
     * the foreign call returns, and the one an unwind is carrying. */
    GC_DBG_SRC("callback_error", 0);
    gc_mark_obj(t->callback_error);
    gc_mark_obj(t->last_condition);

    /* Thread metadata */
    GC_DBG_SRC("thread-metadata", 0);
    gc_mark_obj(t->name);
    gc_mark_obj(t->result);
    gc_mark_obj(t->interrupt_func);
    gc_mark_obj(t->thread_obj);
    /* MP wait registration: the lock / condvar this thread is parked on
     * (and the lock a condition-wait re-acquires).  A parked thread keeps
     * its object alive, and the UPDATE counterpart forwards the record so
     * the releaser's / notifier's identity compare sees the moved object. */
    GC_DBG_SRC("mp-wait", 0);
    gc_mark_obj(t->wait_obj);
    gc_mark_obj(t->wait_lock);

    /* Current lexical env installed for a macro expander — keeps the
     * &environment alist alive while the expander runs. */
    GC_DBG_SRC("current_lex_env", 0);
    gc_mark_obj(t->current_lex_env);

    /* Per-thread current package (the cl_current_package slot).  Packages
     * are also reachable via the package registry, so this mark is mostly
     * redundant — but the slot must be in the thread walk so the UPDATE
     * counterpart forwards it after a compaction (a stale offset here
     * makes this thread's reader intern into garbage). */
    GC_DBG_SRC("current_package", 0);
    gc_mark_obj(t->current_package);

    /* Reader state — in-flight reader stream plus per-read uninterned
     * symbol alist (so #:foo identity survives a GC during a long READ). */
    GC_DBG_SRC("reader-state", 0);
    gc_mark_obj(t->rd_stream);
    gc_mark_obj(t->rd_uninterned);
    gc_mark_obj(t->rd_labels);

    /* Printer in-progress object stack (re-entrancy detection in
     * print-object-hook).  Pinned across hook apply so a GC inside the
     * Lisp print-object method doesn't sweep the object whose recursion
     * we're guarding. */
    for (i = 0; i < t->pr_inprog_top; i++) {
        GC_DBG_SRC("pr_inprog", i);
        gc_mark_obj(t->pr_inprog[i]);
    }

    /* Current printer output target (format/print/write).  Held in a thread
     * field for the whole print, across allocating sub-prints (print-object
     * hooks, string-stream outbuf growth).  A peer thread's compaction while
     * this thread is parked mid-print relocates the stream object; without
     * marking+forwarding this slot, out_char/out_str dereference a stale
     * offset and scribble into freed arena — the multi-thread `(format nil …)`
     * corruption.  Single-writer (owning thread), stopped during STW GC. */
    GC_DBG_SRC("pr_stream", 0);
    gc_mark_obj(t->pr_stream);

    /* *print-circle* detection table keys — live CL_Obj references while a
     * circular/shared print is in progress (pr_circle_active).  Same hazard
     * as pr_stream: relocated out from under the parked printer thread. */
    if (t->pr_circle_active) {
        int ci;
        for (ci = 0; ci < CL_CIRCLE_HT_SIZE; ci++) {
            if (!CL_NULL_P(t->pr_circle_keys[ci])) {
                GC_DBG_SRC("pr_circle_keys", ci);
                gc_mark_obj(t->pr_circle_keys[ci]);
            }
        }
    }

    /* Pending LOAD-TIME-VALUE (cell, thunk) pairs (compile-file only) */
    for (i = 0; i < t->ltv_init_count; i++) {
        GC_DBG_SRC("ltv_init", i);
        gc_mark_obj(t->ltv_init_cells[i]);
        gc_mark_obj(t->ltv_init_thunks[i]);
    }

    /* Compiler constants (active compilers may hold CL_Obj values
     * in platform_alloc'd memory not reachable from the GC arena) */
    GC_DBG_SRC("compiler-constants", 0);
    {
        extern void cl_compiler_gc_mark_thread(CL_Thread *t);
        cl_compiler_gc_mark_thread(t);
    }

    /* VM-internal buffers (e.g. vm_extra_args during &rest processing) */
    GC_DBG_SRC("vm_extra_args", 0);
    {
        extern void cl_vm_gc_mark_extra_thread(CL_Thread *t);
        cl_vm_gc_mark_extra_thread(t);
    }

    /* Thread-Local Value (TLV) table — mark both symbol and value
     * for non-empty, non-tombstone entries */
    {
        int ti;
        for (ti = 0; ti < CL_TLV_TABLE_SIZE; ti++) {
            CL_Obj sym = t->tlv_table[ti].symbol;
            if (sym != CL_NIL && sym != CL_UNBOUND) {
                GC_DBG_SRC("tlv_table", ti);
                gc_mark_obj(sym);
                gc_mark_obj(t->tlv_table[ti].value);
            }
        }
    }
    GC_DBG_SRC("shared-globals", 0);
}
/* Restore gc_root_count macro for the rest of mem.c */
#define gc_root_count (CT->gc_root_count)

static void gc_mark(void)
{
    CL_Thread *t;

    gc_mark_overflow = 0;
    /* Re-arm mark-stack growth: a previous cycle's OOM may have been
     * transient (the failing platform_alloc can succeed now), and the
     * heap-proportional cap re-derives from the current arena anyway. */
    gc_mark_grow_failed = 0;
    /* Reset the pinned-object set; the conservative JIT-stack scan
     * (gc_scan_jit_native_stack, reached via gc_mark_thread_roots) rebuilds
     * it this cycle.  Stays empty unless a JIT frame is live. */
    jit_pinned_count = 0;
    gc_jit_pin_oom = 0;
    /* Invalidate the free-list snapshot — the free list has changed since
     * the last cycle; the first JIT-stack scan this cycle re-collects it. */
    jit_scan_free_valid = 0;

    /* Mark all roots directly (not via gc_mark_push).
     * gc_mark_obj immediately sets the mark bit then pushes children.
     * This is critical: if we used gc_mark_push for roots and the mark
     * stack overflowed, dropped roots would never be marked and the
     * heap re-scan couldn't recover them (it only processes children of
     * already-marked objects).  With gc_mark_obj, even if children
     * overflow, the root itself IS marked and recoverable by re-scan. */

    /* Mark per-thread roots for ALL registered threads.
     * During STW GC, all other threads are stopped, so the thread list
     * is stable — no lock needed for iteration. */
    for (t = cl_thread_list; t; t = t->next) {
        gc_mark_thread_roots(t);
    }

    /* Mark shared globals (not per-thread) */

    /* Package registry — transitively marks all packages, all symbols,
     * and all their values/functions/plists */
    gc_mark_obj(cl_package_registry);

    /* Compiler tables (alists not reachable through packages) */
    gc_mark_obj(macro_table);
    gc_mark_obj(setf_table);
    gc_mark_obj(setf_fn_table);
    gc_mark_obj(setf_expander_table);
    gc_mark_obj(type_table);
    gc_mark_obj(compiler_macro_table);
    gc_mark_obj(cl_clos_class_table);
    gc_mark_obj(struct_table);
    gc_mark_obj(condition_hierarchy);
    gc_mark_obj(condition_slot_table);
    gc_mark_obj(condition_default_initargs);
    gc_mark_obj(condition_slot_initforms);

    /* Thread system: main thread's Lisp object */
    {
        extern CL_Obj cl_main_thread_lisp_obj(void);
        CL_Obj mto = cl_main_thread_lisp_obj();
        if (!CL_NULL_P(mto)) gc_mark_obj(mto);
    }

    /* Readtable user macro closures */
    {
        int rt, ch;
        for (rt = 0; rt < CL_RT_POOL_SIZE; rt++) {
            if (!(cl_readtable_alloc_mask & (1u << rt)))
                continue;
            for (ch = 0; ch < CL_RT_CHARS; ch++) {
                if (!CL_NULL_P(cl_readtable_pool[rt].macro_fn[ch]))
                    gc_mark_obj(cl_readtable_pool[rt].macro_fn[ch]);
                if (!CL_NULL_P(cl_readtable_pool[rt].dispatch_fn[ch]))
                    gc_mark_obj(cl_readtable_pool[rt].dispatch_fn[ch]);
            }
        }
    }

    /* Registered global roots (cached keyword/type symbols, etc.) */
    {
        int gi;
        for (gi = 0; gi < n_global_roots; gi++)
            gc_mark_obj(*global_roots[gi]);
    }

    /* Active FASL readers — their gensym_objs[]/shared_objs[] dedup tables
     * hold CL_Obj references that forward GENSYM_REF/OBJ_REF resolve through
     * but which may not yet be reachable from the graph under construction. */
    {
        extern void cl_fasl_gc_mark_readers(void);
        cl_fasl_gc_mark_readers();
    }

    /* Active FASL WRITERS — bi_load's auto-cache writer holds gensym dedup
     * entries across the interleaved compile/eval of later forms. */
    {
        extern void cl_fasl_gc_mark_writers(void);
        cl_fasl_gc_mark_writers();
    }

    /* Active MAKE-LOAD-FORM writer pre-pass — its walk/result arrays hold
     * CL_Obj offsets that a compaction during a user MAKE-LOAD-FORM method
     * must keep live and forward. */
    {
        extern void cl_fasl_gc_mark_mlf(void);
        cl_fasl_gc_mark_mlf();
    }

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
static void gc_srcloc_invalidate_dead(void)
{
    uint32_t i;
    for (i = 0; i < CL_SRCLOC_SIZE; i++) {
        CL_Obj k = cl_srcloc_table[i].cons_obj;
        if (CL_NULL_P(k) || CL_FIXNUM_P(k) || CL_CHAR_P(k)) continue;
        if (k >= cl_heap.bump || !CL_HDR_MARKED(CL_OBJ_TO_PTR(k)))
            cl_srcloc_table[i].cons_obj = CL_NIL;
    }
}

static CL_Obj gc_forward(CL_Obj obj);   /* defined with the compactor below */

static void gc_srcloc_forward(void)
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
static uint32_t bytecode_release_offheap(CL_Bytecode *bc)
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

static void gc_finalize_dead(uint8_t *ptr)
{
    switch (CL_HDR_TYPE(ptr)) {
    case TYPE_BYTECODE: {
        /* Free every off-heap buffer owned by a dead bytecode: the body, the
         * constants pool, the &key arrays, the line map and the JIT artifacts
         * are all platform_alloc'd and were otherwise leaked for good when
         * the object is swept.  Safe here: the world is stopped and a DEAD
         * (unmarked) bytecode cannot be executing — a running one is
         * reachable from a VM frame (gc_mark_thread_roots marks both
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
         * marks t->thread_obj from gc_mark_thread_roots, so the wrapper
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

static void gc_sweep(void)
{
    uint8_t *ptr = cl_heap.arena + CL_ALIGN;  /* Skip offset 0 (reserved for NIL) */
    uint8_t *end = cl_heap.arena + cl_heap.bump;

    cl_heap.free_list = 0;
    cl_heap.total_allocated = 0;

    while (ptr < end) {
        uint32_t size = CL_HDR_SIZE(ptr);

        if (size == 0) break;  /* Safety: shouldn't happen */

        if (CL_HDR_MARKED(ptr)) {
            /* Live object — clear mark for next cycle */
            CL_HDR_CLR_MARK(ptr);
            cl_heap.total_allocated += size;
        } else {
            /* Dead object — finalize, then add to free list and coalesce.
             * Limit coalesced size to CL_HDR_SIZE_MASK (23 bits, ~8MB) because
             * free block's size field occupies the same position as the object
             * header, and the next sweep reads CL_HDR_SIZE() which masks to
             * 23 bits.  Blocks larger than that would be mis-parsed. */
            CL_FreeBlock *fb = (CL_FreeBlock *)ptr;
            uint32_t total = size;

            /* Finalize: release external resources for dead objects */
            gc_finalize_dead(ptr);

            /* Coalesce adjacent free blocks up to max representable size */
            while (ptr + total < end) {
                CL_Header *next = (CL_Header *)(ptr + total);
                uint32_t next_size = next->header & CL_HDR_SIZE_MASK;
                if (next_size == 0) break;
                if (next->header & CL_HDR_MARK_BIT) break;  /* Next is live */
                if (total + next_size > CL_HDR_SIZE_MASK) break;  /* Would overflow 23-bit size */
                /* Finalize the coalesced dead object too */
                gc_finalize_dead((uint8_t *)next);
                total += next_size;
            }

            fb->size = total;
            fb->next_offset = cl_heap.free_list;
            cl_heap.free_list = (uint32_t)(ptr - cl_heap.arena);
#ifdef DEBUG_GC
            /* Poison free block data after the 8-byte header (size +
             * next_offset) to detect use-after-free. */
            if (total > sizeof(CL_FreeBlock))
                memset((uint8_t *)fb + sizeof(CL_FreeBlock), 0xDE,
                       total - sizeof(CL_FreeBlock));
#endif
            size = total;  /* advance past entire coalesced region */
        }
        /* Block-start index: coalescing just erased the headers inside a
         * free block, so every page this block covers is re-pointed at
         * it (live objects re-note themselves, a no-op in effect). */
        if (GC_HDR_CROSSES((uint32_t)(ptr - cl_heap.arena), size))
            gc_hdr_note((uint32_t)(ptr - cl_heap.arena), size);
        ptr += size;
    }
    gc_hdr_bump_reset();
}

/* ================================================================
 * Compacting GC (Lisp-2 style sliding compaction)
 *
 * 4-pass algorithm:
 *   Pass 1: Mark (reuse existing gc_mark)
 *   Pass 2: Compute forwarding addresses
 *   Pass 3: Update all references (roots + heap objects)
 *   Pass 4: Slide live objects to their new positions
 *
 * Triggered when normal GC + free-list can't satisfy an allocation
 * (fragmentation is the bottleneck), or explicitly via cl_gc_compact().
 * ================================================================ */

/* Bytes the forwarding structures need at the current bump front (the
 * bitmap, the page table, one gap per pin).  Exported so a test can pin
 * the bound; a compaction allocates exactly this. */
uint32_t cl_gc_fwd_bytes(void)
{
    uint32_t words = (cl_heap.bump / CL_ALIGN + 31) / 32;
    uint32_t pages = (cl_heap.bump + GC_FWD_PAGE - 1) >> GC_FWD_PAGE_SHIFT;
    return (words + pages + (uint32_t)jit_pinned_count) * (uint32_t)sizeof(uint32_t);
}

/* Allocate / free the forwarding structures.  The whole block is zeroed:
 * the bitmap needs it, and so does the gap array — a pin that pass 2 does
 * not place (a duplicate entry, or a candidate that was never a marked
 * object start) must contribute a zero gap to the lookups, exactly as it
 * contributes nothing to the slide.  Pass 2 writes every page entry. */
static int gc_fwd_alloc(void)
{
    uint32_t words = (cl_heap.bump / CL_ALIGN + 31) / 32;
    uint32_t pages = (cl_heap.bump + GC_FWD_PAGE - 1) >> GC_FWD_PAGE_SHIFT;
    uint32_t bytes = cl_gc_fwd_bytes();
    uint32_t *blk;
    if (cl_gc_fwd_fail_over && bytes >= cl_gc_fwd_fail_over)
        return 0;
    blk = (uint32_t *)platform_alloc(bytes);
    if (!blk) return 0;
    gc_fwd_block = blk;
    gc_fwd_bits  = blk;
    gc_fwd_page  = blk + words;
    gc_fwd_gap   = gc_fwd_page + pages;
    memset(blk, 0, bytes);
    return 1;
}

static void gc_fwd_free(void)
{
    if (gc_fwd_block) {
        platform_free(gc_fwd_block);
        gc_fwd_block = NULL;
        gc_fwd_bits = gc_fwd_page = gc_fwd_gap = NULL;
    }
}

#if defined(__GNUC__) && (defined(__x86_64__) || defined(__aarch64__))
/* One instruction on these hosts.  Not on m68k or PPC, where the builtin
 * becomes a libgcc call: the shift-and-mask form below is inlined there. */
#define gc_popcount32(x) ((uint32_t)__builtin_popcount((unsigned)(x)))
#else
static uint32_t gc_popcount32(uint32_t x)
{
    x = x - ((x >> 1) & 0x55555555u);
    x = (x & 0x33333333u) + ((x >> 2) & 0x33333333u);
    x = (x + (x >> 4)) & 0x0F0F0F0Fu;
    x += x >> 8;
    x += x >> 16;
    return x & 0x3Fu;
}
#endif

/* Set the bitmap for granules [g0, g1) — one live object. */
static void gc_fwd_set_range(uint32_t g0, uint32_t g1)
{
    uint32_t w0 = g0 >> 5, w1 = (g1 - 1) >> 5;
    uint32_t m0 = 0xFFFFFFFFu << (g0 & 31);
    uint32_t m1 = 0xFFFFFFFFu >> (31 - ((g1 - 1) & 31));
    if (w0 == w1) {
        gc_fwd_bits[w0] |= m0 & m1;
        return;
    }
    gc_fwd_bits[w0] |= m0;
    while (++w0 < w1)
        gc_fwd_bits[w0] = 0xFFFFFFFFu;
    gc_fwd_bits[w1] |= m1;
}

/* Sum of the holes left below pins in [lo, hi] (sorted jit_pinned[]). */
static uint32_t gc_fwd_pin_gaps(uint32_t lo, uint32_t hi)
{
    int a = 0, b = jit_pinned_count;
    uint32_t sum = 0;
    while (a < b) {                     /* first pin >= lo */
        int mid = (a + b) / 2;
        if (jit_pinned[mid] < lo) a = mid + 1; else b = mid;
    }
    while (a < jit_pinned_count && jit_pinned[a] <= hi)
        sum += gc_fwd_gap[a++];
    return sum;
}

/* The slide cursor: where the next unpinned live object goes.  Shared by
 * pass 2 (which also records each pin's gap) and pass 4, so the addresses
 * the lookup computes and the addresses the slide uses come from one
 * definition. */
typedef struct {
    uint32_t cur;       /* next free position in the new layout */
    int      pin_i;     /* index into the sorted jit_pinned[] */
} GcPlace;

static uint32_t gc_place(GcPlace *pl, uint32_t off, uint32_t size, int record_gap)
{
    uint32_t at;
    /* Advance past pins below this object (jit_pinned[] is sorted
     * ascending, and objects are visited in ascending offset order). */
    while (pl->pin_i < jit_pinned_count && jit_pinned[pl->pin_i] < off)
        pl->pin_i++;
    if (pl->pin_i < jit_pinned_count && jit_pinned[pl->pin_i] == off) {
        /* Pinned: keep in place.  The cursor is <= off here (all live data
         * below this object compacts into [CL_ALIGN, off)), so leaving
         * [cur, off) as a hole and resuming at the pin's end preserves the
         * monotonic, no-overlap invariant. */
        if (record_gap)
            gc_fwd_gap[pl->pin_i] = off - pl->cur;
        pl->pin_i++;
        /* The same object pinned from two scan chunks or two threads:
         * the duplicates keep their zero gap (gc_fwd_alloc). */
        while (pl->pin_i < jit_pinned_count && jit_pinned[pl->pin_i] == off)
            pl->pin_i++;
        pl->cur = off + size;
        return off;
    }
    at = pl->cur;
    pl->cur += size;
    return at;
}

/* Pass 2: Walk arena linearly, record live granules and the per-page slide
 * cursor from which gc_forward derives every forwarding address. */
static void gc_compute_forwarding(void)
{
    uint8_t *ptr = cl_heap.arena + CL_ALIGN;
    uint8_t *end = cl_heap.arena + cl_heap.bump;
    uint32_t npages = (cl_heap.bump + GC_FWD_PAGE - 1) >> GC_FWD_PAGE_SHIFT;
    uint32_t next_page = 0;
    GcPlace pl;
    pl.cur = CL_ALIGN;               /* skip offset 0 (NIL) */
    pl.pin_i = 0;

    /* jit_pinned[] must be ascending for the single-pass merge in gc_place.
     * A single thread's scan already appends in ascending order (monotonic
     * arena walk), but with multi-thread STW each stopped thread's JIT
     * stack is scanned in turn, so pins from different threads concatenate
     * out of order — sort. */
    if (jit_pinned_count > 1)
        qsort(jit_pinned, (size_t)jit_pinned_count, sizeof(jit_pinned[0]),
              cand_cmp);

    while (ptr < end) {
        uint32_t size = CL_HDR_SIZE(ptr);
        if (size == 0) break;

        if (CL_HDR_MARKED(ptr)) {
            uint32_t off = (uint32_t)(ptr - cl_heap.arena);
            uint32_t new_off;

            /* Pages whose first byte lies in the dead run before this
             * object, or exactly at it, see the cursor as it stands. */
            while (next_page < npages && (next_page << GC_FWD_PAGE_SHIFT) <= off)
                gc_fwd_page[next_page++] = pl.cur;

            new_off = gc_place(&pl, off, size, 1);

            /* Pages starting inside this object: it slides as a unit, so a
             * granule at page start lands at new_off + its distance. */
            while (next_page < npages && (next_page << GC_FWD_PAGE_SHIFT) < off + size) {
                gc_fwd_page[next_page] = new_off + ((next_page << GC_FWD_PAGE_SHIFT) - off);
                next_page++;
            }
            gc_fwd_set_range(off >> GC_FWD_ALIGN_SHIFT, (off + size) >> GC_FWD_ALIGN_SHIFT);
        }
        ptr += size;
    }
    while (next_page < npages)
        gc_fwd_page[next_page++] = pl.cur;
}

/* (The former gc_forward_jit_native_stack writeback pass was removed when
 * the compactor switched to pinning conservatively-referenced objects —
 * option B in specs/native-backend.md §"GC interaction".  Pinned objects
 * never move, so the JIT's spilled operand offsets on the C stack stay
 * valid with no writeback, and a coincidental-integer C-stack word that
 * merely equals a live object's offset is no longer mis-rewritten.) */

/* Translate a CL_Obj through the forwarding table.
 * Returns obj unchanged if it's not a movable heap pointer. */
static CL_Obj gc_forward(CL_Obj obj)
{
    uint32_t g, w, live, fwd;
    if (CL_NULL_P(obj) || CL_FIXNUM_P(obj) || CL_CHAR_P(obj))
        return obj;
    if (obj == CL_UNBOUND)
        return obj;
    if (obj >= cl_heap.bump)
        return obj;
#ifdef CL_GENGC
    if (gen_minor_fwd) {
        /* Minor cycle: forward via binary search over the sorted survivor
         * list instead of a nursery-sized table (whose allocate+zero would
         * cost O(nursery) per cycle).  Old-space offsets never move in a
         * minor; a nursery offset absent from the list references a DEAD
         * object (possible in weak-ish side tables) and is left alone. */
        uint32_t lo, hi;
        if (obj < gen_old_top)
            return obj;
        lo = 0;
        hi = gen_live_count;
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            if (gen_live[mid] < obj)      lo = mid + 1;
            else if (gen_live[mid] > obj) hi = mid;
            else                          return gen_live_new[mid];
        }
        return obj;
    }
#endif
    g = obj >> GC_FWD_ALIGN_SHIFT;
    if (!((gc_fwd_bits[g >> 5] >> (g & 31)) & 1u))
        return obj;      /* dead object or free space: left as it is */
    /* Live granules from the page start up to (not including) obj. */
    w = (obj >> GC_FWD_PAGE_SHIFT) * GC_FWD_WORDS_PER_PAGE;
    live = 0;
    while (w < (g >> 5))
        live += gc_popcount32(gc_fwd_bits[w++]);
    live += gc_popcount32(gc_fwd_bits[w] & ((1u << (g & 31)) - 1u));
    fwd = gc_fwd_page[obj >> GC_FWD_PAGE_SHIFT] + (live << GC_FWD_ALIGN_SHIFT);
    if (jit_pinned_count > 0)
        fwd += gc_fwd_pin_gaps(obj & ~(GC_FWD_PAGE - 1), obj);
    /* An offset inside a live object (never a valid CL_Obj, but a
     * conservative or corrupt slot can hold one) moves with its object:
     * the formula above is exact for interior granules too. */
    return fwd;
}

/* Update a CL_Obj slot in place via forwarding table */
static void gc_update_slot(CL_Obj *slot)
{
    *slot = gc_forward(*slot);
}

/* Pass 3a: Update children of a single heap object.  Shares the per-type slot
 * layout with gc_mark_children via the GC_WALK_OBJ_CHILDREN X-macro above;
 * here each slot is forwarded in place and TYPE_BYTECODE additionally patches
 * CL_Obj immediates baked into JIT'd native code. */
static void gc_update_children(void *ptr, uint8_t type)
{
#define GC_VISIT(slot) gc_update_slot(&(slot))
    /* Forward CL_Obj heap references baked as 32-bit immediates into the JIT'd
     * native code.  native_relocs lists the byte offset of each such 4-byte
     * big-endian field; without this a moving compaction would leave stale
     * arena offsets in executable code (m68k JIT only — native_relocs is NULL
     * on host / non-JIT).  The code buffer is platform_alloc'd (does not move),
     * so the raw pointer stays valid whether we visit the object's old or new
     * copy; only the referenced objects shift.  Stop-the-world GC means no
     * thread is mid-fetch of these bytes. */
#define GC_BYTECODE_TAIL(bc)                                                  \
    do {                                                                      \
        if ((bc)->native_code && (bc)->native_relocs) {                       \
            uint32_t r_;                                                       \
            for (r_ = 0; r_ < (bc)->native_reloc_count; r_++) {               \
                uint8_t *p_ = (bc)->native_code + (bc)->native_relocs[r_];     \
                CL_Obj old = ((CL_Obj)p_[0] << 24) | ((CL_Obj)p_[1] << 16) |   \
                             ((CL_Obj)p_[2] << 8)  |  (CL_Obj)p_[3];           \
                CL_Obj nw = gc_forward(old);                                   \
                if (nw != old) {                                              \
                    p_[0] = (uint8_t)(nw >> 24); p_[1] = (uint8_t)(nw >> 16);  \
                    p_[2] = (uint8_t)(nw >> 8);  p_[3] = (uint8_t)(nw);        \
                    gc_native_code_patched = 1;                               \
                }                                                            \
            }                                                                \
        }                                                                    \
    } while (0)
#define GC_STREAM_TAIL(st) ((void)(st))
    GC_WALK_OBJ_CHILDREN(ptr, type);
#undef GC_VISIT
#undef GC_BYTECODE_TAIL
#undef GC_STREAM_TAIL
}

/* Pass 3b: Update per-thread roots (mirrors gc_mark_thread_roots).
 * Must #undef gc_root_count to avoid macro collision with t->gc_root_count. */
#undef gc_root_count
static void gc_update_thread_roots(CL_Thread *t)
{
    int i;

    /* The gc_roots[] stack (CL_Obj* pointers to C locals) is NOT forwarded
     * here — gc_update_registered_roots forwards all threads' entries from
     * one deduplicated address list, so a slot registered twice (or one
     * aliasing any of the independently-forwarded regions below) is
     * forwarded exactly once. */

    /* Dynamic binding stack */
    for (i = 0; i < t->dyn_top; i++) {
        gc_update_slot(&t->dyn_stack[i].symbol);
        gc_update_slot(&t->dyn_stack[i].old_value);
    }

    /* NLX stack (mv_values deliberately excluded — see the mark phase) */
    for (i = 0; i < t->nlx_top; i++) {
        gc_update_slot(&t->nlx_stack[i].tag);
        gc_update_slot(&t->nlx_stack[i].result);
        gc_update_slot(&t->nlx_stack[i].bytecode);
    }

    /* Handler stack */
    for (i = 0; i < t->handler_top; i++) {
        gc_update_slot(&t->handler_stack[i].type_name);
        gc_update_slot(&t->handler_stack[i].handler);
    }

    /* Restart stack */
    for (i = 0; i < t->restart_top; i++) {
        gc_update_slot(&t->restart_stack[i].name);
        gc_update_slot(&t->restart_stack[i].handler);
        gc_update_slot(&t->restart_stack[i].tag);
        gc_update_slot(&t->restart_stack[i].restart);
    }

    /* VM execution stack */
    if (t->vm.stack) {
        for (i = 0; i < t->vm.sp; i++)
            gc_update_slot(&t->vm.stack[i]);
    }

    /* VM frame bytecodes */
    for (i = 0; i < t->vm.fp; i++)
        gc_update_slot(&t->vm.frames[i].bytecode);

    /* Multiple values and pending throw state.
     * (pre_call_mv_values deliberately excluded — see the mark phase.) */
    for (i = 0; i < CL_MAX_MV; i++)
        gc_update_slot(&t->mv_values[i]);
    gc_update_slot(&t->pending_tag);
    gc_update_slot(&t->pending_value);
    /* UNWIND-PROTECT value records — mirror of the mark phase. */
    for (i = 0; i < t->mv_save_top && i < CL_MV_SAVE_SIZE; i++)
        gc_update_slot(&t->mv_save_buf[i]);
    /* In-flight THROW secondary values + saved pending-throw snapshots —
     * mirror of gc_mark_thread_roots (a snapshot restored after an
     * allocating unwind-protect cleanup must hold FORWARDED offsets, or
     * the restored pending_tag no longer EQ-matches its catch tag:
     * "No catch for tag" / garbage multiple values). */
    if (t->pending_throw) {
        for (i = 0; i < t->pending_mv_count && i < CL_MAX_MV; i++)
            gc_update_slot(&t->pending_mv_values[i]);
    }
    for (i = 0; i < t->saved_pending_top; i++) {
        int m;
        if (!t->saved_pending_stack[i].pending_throw)
            continue;  /* see gc_mark_thread_roots */
        gc_update_slot(&t->saved_pending_stack[i].pending_tag);
        gc_update_slot(&t->saved_pending_stack[i].pending_value);
        for (m = 0; m < t->saved_pending_stack[i].pending_mv_count &&
                    m < CL_MAX_MV; m++)
            gc_update_slot(&t->saved_pending_stack[i].pending_mv_values[m]);
    }
    gc_update_slot(&t->pending_lambda_name);  /* see gc_mark counterpart */
    gc_update_slot(&t->callback_error);
    gc_update_slot(&t->last_condition);

    /* Thread metadata */
    gc_update_slot(&t->name);
    gc_update_slot(&t->result);
    gc_update_slot(&t->interrupt_func);
    gc_update_slot(&t->current_lex_env);
    gc_update_slot(&t->current_package);  /* per-thread *PACKAGE* mirror —
                                           * see gc_mark counterpart */
    gc_update_slot(&t->thread_obj);
    gc_update_slot(&t->wait_obj);         /* MP wait registration — see
                                           * gc_mark counterpart */
    gc_update_slot(&t->wait_lock);

    /* Reader state — must mirror gc_mark_thread_roots.  These were marked
     * but not updated, leaving stale CL_Obj values pointing to old arena
     * offsets after compaction; the next gc_mark would then dereference
     * the stale offset, read garbage as a heap header, and run off the
     * end of the arena while iterating "n_slots"/"n_constants" — manifesting
     * as a SIGSEGV in gc_mark on sento workloads that compact under load. */
    gc_update_slot(&t->rd_stream);
    gc_update_slot(&t->rd_uninterned);
    gc_update_slot(&t->rd_labels);

    /* Printer in-progress object stack — heap pointers can be relocated
     * by compaction during a Lisp print-object hook (which can allocate). */
    for (i = 0; i < t->pr_inprog_top; i++)
        gc_update_slot(&t->pr_inprog[i]);

    /* Current printer output target — forward the relocated stream offset so
     * a parked printer thread resumes writing to the moved object, not the
     * stale one (mirror of the gc_mark_thread_roots counterpart). */
    gc_update_slot(&t->pr_stream);

    /* *print-circle* detection table keys (only meaningful while active). */
    if (t->pr_circle_active) {
        int ci;
        for (ci = 0; ci < CL_CIRCLE_HT_SIZE; ci++) {
            if (!CL_NULL_P(t->pr_circle_keys[ci]))
                gc_update_slot(&t->pr_circle_keys[ci]);
        }
    }

    /* Pending LOAD-TIME-VALUE (cell, thunk) pairs (compile-file only) */
    for (i = 0; i < t->ltv_init_count; i++) {
        gc_update_slot(&t->ltv_init_cells[i]);
        gc_update_slot(&t->ltv_init_thunks[i]);
    }

    /* Compiler constants (platform_alloc'd, hold CL_Obj refs) */
    {
        extern void cl_compiler_gc_update_thread(CL_Thread *t,
                                                  void (*update_fn)(CL_Obj *));
        cl_compiler_gc_update_thread(t, gc_update_slot);
    }

    /* VM extra args buffer */
    {
        extern void cl_vm_gc_update_extra_thread(CL_Thread *t,
                                                  void (*update_fn)(CL_Obj *));
        cl_vm_gc_update_extra_thread(t, gc_update_slot);
    }

    /* TLV table */
    {
        int ti;
        for (ti = 0; ti < CL_TLV_TABLE_SIZE; ti++) {
            CL_Obj sym = t->tlv_table[ti].symbol;
            if (sym != CL_NIL && sym != CL_UNBOUND) {
                gc_update_slot(&t->tlv_table[ti].symbol);
                gc_update_slot(&t->tlv_table[ti].value);
            }
        }
    }

    /* JIT native stack needs no forward-update pass: the mark-phase scan
     * (gc_scan_jit_native_stack) pins every conservatively-referenced
     * object, so they stay at their current offset and the JIT's spilled
     * C-stack offsets remain valid without rewriting the stack. */
}

/* --- Registered-root forwarding with dedup (double-forward hardening) ---
 *
 * gc_forward is NOT idempotent: the forwarding table is keyed by
 * pre-compaction offsets, so forwarding the same slot twice maps the
 * already-forwarded offset through whatever object's OLD offset it now
 * coincides with — silently rewriting the slot to an unrelated object.
 * Historically each root registry was forwarded by its own loop, so any
 * slot reachable from two entries (an accidental double CL_GC_PROTECT,
 * a global both cl_gc_register_root'ed and protected, or a protect of an
 * already-rooted VM-stack slot like CL_GC_PROTECT(args[i])) corrupted
 * with layout-roulette timing.  This pass forwards all dynamically-
 * registered slots (global_roots[] + every thread's gc_roots[]) from one
 * sorted, deduplicated address list, and skips addresses that alias any
 * independently-forwarded thread region (VM stack, mv_values, pending
 * throw values, dyn/handler/restart stacks, vm_extra_args, compiler
 * chain — see root_slot_independently_forwarded) — making duplicate
 * registration harmless by construction.  cl_gc_audit_roots remains the tool for locating the
 * offending registration site.  (gc_root_slot_buf itself is declared with
 * the other persistent GC buffers near the top of the file, so
 * cl_mem_shutdown can free it.) */
static int root_slot_cmp(const void *a, const void *b)
{
    CL_Obj * const *pa = (CL_Obj * const *)a;
    CL_Obj * const *pb = (CL_Obj * const *)b;
    if (*pa < *pb) return -1;
    if (*pa > *pb) return 1;
    return 0;
}

/* Probe callback for testing whether a slot address is owned by an
 * external per-thread forwarding walker (compiler chain).  Reusing the
 * walker itself as the membership test guarantees this can never drift
 * from what gc_update_thread_roots actually forwards. */
static CL_Obj *gc_probe_slot;
static int     gc_probe_hit;
static void gc_probe_check(CL_Obj *slot)
{
    if (slot == gc_probe_slot) gc_probe_hit = 1;
}

/* Does slot alias a per-thread region that gc_update_thread_roots
 * forwards independently?  Those slots must be SKIPPED by the
 * registered-root pass, or they are forwarded twice (gc_forward is not
 * idempotent — double-forwarding rewrites the slot to an unrelated
 * object).  Historically only the VM value stack was exempted; a root
 * registered against mv_values / pending_mv_values / the dyn/handler/
 * restart stacks / vm_extra_args / a compiler-chain slot still
 * double-forwarded with layout-roulette timing.  Every range below is
 * bounded exactly like its forwarding loop in gc_update_thread_roots —
 * an out-of-bounds alias (e.g. beyond dyn_top) is NOT independently
 * forwarded and must stay in the registered-root pass. */
static int root_slot_independently_forwarded(CL_Obj *slot)
{
    CL_Thread *t;
    for (t = cl_thread_list; t; t = t->next) {
        /* VM value stack [stack, stack+sp) */
        if (t->vm.stack && slot >= t->vm.stack &&
            slot < t->vm.stack + t->vm.sp)
            return 1;
        /* Multiple-values buffer (forwarded unconditionally, all slots) */
        if (slot >= t->mv_values && slot < t->mv_values + CL_MAX_MV)
            return 1;
        /* In-flight THROW secondary values — forwarded only while a
         * throw is pending, and only up to pending_mv_count. */
        if (t->pending_throw &&
            slot >= t->pending_mv_values &&
            slot < t->pending_mv_values +
                   (t->pending_mv_count < CL_MAX_MV ? t->pending_mv_count
                                                    : CL_MAX_MV))
            return 1;
        /* Dynamic-binding / handler / restart stacks: every CL_Obj field
         * of an entry below the top is forwarded by its walker loop. */
        if ((char *)slot >= (char *)t->dyn_stack &&
            (char *)slot <  (char *)(t->dyn_stack + t->dyn_top))
            return 1;
        if ((char *)slot >= (char *)t->handler_stack &&
            (char *)slot <  (char *)(t->handler_stack + t->handler_top))
            return 1;
        if ((char *)slot >= (char *)t->restart_stack &&
            (char *)slot <  (char *)(t->restart_stack + t->restart_top))
            return 1;
        /* VM extra-args buffer (&rest processing) */
        if (slot >= t->vm_extra_args_buf &&
            slot < t->vm_extra_args_buf + t->vm_extra_count)
            return 1;
        /* Saved pending-throw snapshots: pending_tag/pending_value/
         * pending_mv_values[] of each armed (pending_throw) entry below
         * saved_pending_top are forwarded unconditionally by
         * gc_update_thread_roots — mirror those exact bounds here. */
        if (t->saved_pending_stack) {
            int spi;
            for (spi = 0; spi < t->saved_pending_top; spi++) {
                CL_SavedPending *sp = &t->saved_pending_stack[spi];
                if (!sp->pending_throw)
                    continue;
                if (slot == &sp->pending_tag || slot == &sp->pending_value)
                    return 1;
                if (slot >= sp->pending_mv_values &&
                    slot < sp->pending_mv_values +
                           (sp->pending_mv_count < CL_MAX_MV
                                ? sp->pending_mv_count : CL_MAX_MV))
                    return 1;
            }
        }
        /* Compiler-chain constants/blocks/tagbodies/env — walked by
         * cl_compiler_gc_update_thread; probe with the walker itself. */
        if (t->active_compiler) {
            extern void cl_compiler_gc_update_thread(CL_Thread *th,
                                                     void (*update_fn)(CL_Obj *));
            gc_probe_slot = slot;
            gc_probe_hit = 0;
            cl_compiler_gc_update_thread(t, gc_probe_check);
            if (gc_probe_hit) return 1;
        }
    }
    return 0;
}

static void gc_update_registered_roots(void)
{
    uint32_t total = (uint32_t)n_global_roots;
    uint32_t n = 0, i;
    CL_Thread *t;
    int j;
#ifdef DEBUG_GC
    uint32_t dups = 0, aliased = 0;
#endif

    for (t = cl_thread_list; t; t = t->next)
        total += (uint32_t)t->gc_root_count;
    if (total == 0)
        return;

    if (total > gc_root_slot_cap) {
        CL_Obj **buf = (CL_Obj **)platform_alloc(total * sizeof(CL_Obj *));
        if (buf) {
            if (gc_root_slot_buf) platform_free(gc_root_slot_buf);
            gc_root_slot_buf = buf;
            gc_root_slot_cap = total;
        }
    }
    if (total > gc_root_slot_cap) {
        /* Buffer allocation failed — fall back to per-registry loops.
         * Exact-duplicate addresses would still double-forward here, but
         * skipping the update entirely would corrupt EVERY slot; the
         * VM-stack alias check needs no memory, so it still applies. */
        for (j = 0; j < n_global_roots; j++)
            gc_update_slot(global_roots[j]);
        for (t = cl_thread_list; t; t = t->next)
            for (j = 0; j < t->gc_root_count; j++)
                if (!root_slot_independently_forwarded(t->gc_roots[j]))
                    gc_update_slot(t->gc_roots[j]);
        return;
    }

    for (j = 0; j < n_global_roots; j++)
        gc_root_slot_buf[n++] = global_roots[j];
    for (t = cl_thread_list; t; t = t->next)
        for (j = 0; j < t->gc_root_count; j++)
            gc_root_slot_buf[n++] = t->gc_roots[j];

    qsort(gc_root_slot_buf, (size_t)n, sizeof(CL_Obj *), root_slot_cmp);

    for (i = 0; i < n; i++) {
        if (i > 0 && gc_root_slot_buf[i] == gc_root_slot_buf[i - 1]) {
#ifdef DEBUG_GC
            dups++;
#endif
            continue;               /* duplicate registration — forward once */
        }
        if (root_slot_independently_forwarded(gc_root_slot_buf[i])) {
#ifdef DEBUG_GC
            aliased++;
#endif
            continue;               /* owned by a thread-region forwarding loop */
        }
        gc_update_slot(gc_root_slot_buf[i]);
    }

#ifdef DEBUG_GC
    if (dups || aliased)
        fprintf(stderr, "GC: root dedup skipped %u duplicate and %u "
                "thread-region-aliased registration(s) this compaction "
                "(harmless, but run ext:%%gc-audit-roots to find the "
                "redundant CL_GC_PROTECT / cl_gc_register_root)\n",
                (unsigned)dups, (unsigned)aliased);
#endif
}
#define gc_root_count (CT->gc_root_count)

/* Pass 3c: Update shared (non-per-thread) roots (mirrors gc_mark shared section) */
static void gc_update_shared_roots(void)
{
    /* Package registry */
    gc_update_slot(&cl_package_registry);

    /* Compiler tables */
    gc_update_slot(&macro_table);
    gc_update_slot(&setf_table);
    gc_update_slot(&setf_fn_table);
    gc_update_slot(&setf_expander_table);
    gc_update_slot(&type_table);
    gc_update_slot(&compiler_macro_table);
    gc_update_slot(&cl_clos_class_table);
    gc_update_slot(&struct_table);
    gc_update_slot(&condition_hierarchy);
    gc_update_slot(&condition_slot_table);
    gc_update_slot(&condition_default_initargs);
    gc_update_slot(&condition_slot_initforms);

    /* Main thread Lisp object */
    {
        extern CL_Obj *cl_main_thread_lisp_obj_ptr(void);
        CL_Obj *ptr = cl_main_thread_lisp_obj_ptr();
        if (ptr) gc_update_slot(ptr);
    }

    /* Readtable user macro closures */
    {
        int rt, ch;
        for (rt = 0; rt < CL_RT_POOL_SIZE; rt++) {
            if (!(cl_readtable_alloc_mask & (1u << rt)))
                continue;
            for (ch = 0; ch < CL_RT_CHARS; ch++) {
                gc_update_slot(&cl_readtable_pool[rt].macro_fn[ch]);
                gc_update_slot(&cl_readtable_pool[rt].dispatch_fn[ch]);
            }
        }
    }

    /* Registered global roots (cached keyword/type symbols, etc.) are
     * forwarded by gc_update_registered_roots — one deduplicated pass
     * shared with the thread gc_roots[] stacks. */

    /* Active FASL readers — forward the relocated offsets in their dedup
     * tables (mirrors cl_fasl_gc_mark_readers in the mark phase). */
    {
        extern void cl_fasl_gc_update_readers(void (*update_fn)(CL_Obj *));
        cl_fasl_gc_update_readers(gc_update_slot);
    }

    /* Active FASL writers — forward the gensym dedup entries (mirrors
     * cl_fasl_gc_mark_writers). */
    {
        extern void cl_fasl_gc_update_writers(void (*update_fn)(CL_Obj *));
        cl_fasl_gc_update_writers(gc_update_slot);
    }

    /* MAKE-LOAD-FORM writer pre-pass — forward the relocated offsets in
     * its walk/result arrays (mirrors cl_fasl_gc_mark_mlf). */
    {
        extern void cl_fasl_gc_update_mlf(void (*update_fn)(CL_Obj *));
        cl_fasl_gc_update_mlf(gc_update_slot);
    }

    /* Struct registry hash index — its keys (name symbols) and cached
     * entry values just moved; mark it stale so it rebuilds lazily
     * before the next lookup (builtins_struct.c).  Ditto the condition
     * hierarchy index and the compiler's deftype / compiler-macro /
     * defsetf table indexes (CL_AlistIndex, compiler.h). */
    {
        extern void cl_struct_index_gc_invalidate(void);
        extern void cl_condition_index_gc_invalidate(void);
        extern void cl_compiler_indexes_gc_invalidate(void);
        cl_struct_index_gc_invalidate();
        cl_condition_index_gc_invalidate();
        cl_compiler_indexes_gc_invalidate();
    }
}

/* Pass 3 (roots half): update every non-heap root slot through the
 * forwarding table.  Shared verbatim between a full compaction and a
 * minor (nursery) collection — gc_forward leaves non-forwarded (old)
 * offsets unchanged, so this is safe and cheap in both. */
static void gc_update_all_roots(void)
{
    CL_Thread *t;

    /* Update per-thread roots */
    for (t = cl_thread_list; t; t = t->next)
        gc_update_thread_roots(t);

    /* Registered root slots (global_roots[] + all thread gc_roots[]),
     * deduplicated so a doubly-registered slot is forwarded exactly once.
     * Disjoint from the passes above/below: entries aliasing an
     * independently-forwarded thread region are skipped (that region's
     * own forwarding loop owns them). */
    gc_update_registered_roots();

    /* Update shared globals */
    gc_update_shared_roots();
}

/* Pass 3: Walk all live heap objects + all roots and update references */
static void gc_update_all_references(void)
{
    uint8_t *ptr, *end;

    gc_update_all_roots();

    /* Walk all live heap objects and update their children */
    ptr = cl_heap.arena + CL_ALIGN;
    end = cl_heap.arena + cl_heap.bump;
    while (ptr < end) {
        uint32_t size = CL_HDR_SIZE(ptr);
        if (size == 0) break;
        if (CL_HDR_MARKED(ptr))
            gc_update_children(ptr, CL_HDR_TYPE(ptr));
        ptr += size;
    }
}

/* Chunk size for splitting a free gap of `total` bytes under alignment
 * `align`: at most CL_HDR_SIZE_MASK, align-multiple, and never leaving a
 * remainder too small to hold a CL_FreeBlock header.  With align=4
 * (Amiga) the naive cap CL_HDR_SIZE_MASK & ~3 can leave a 4-byte
 * remainder; the next iteration's CL_FreeBlock write (8 bytes) would
 * then overrun the gap and smash the following — pinned — object's
 * header.  Non-static so the unit test can exercise the align=4
 * arithmetic on the host (whose CL_ALIGN is 8). */
uint32_t gc_free_gap_chunk(uint32_t total, uint32_t align)
{
    uint32_t chunk = total;
    if (chunk > CL_HDR_SIZE_MASK) {
        chunk = CL_HDR_SIZE_MASK & ~(align - 1u);
        if (total - chunk < (uint32_t)sizeof(CL_FreeBlock))
            chunk -= CL_MIN_ALLOC_SIZE;
    }
    return chunk;
}

/* Write valid free-block header(s) covering [offset, offset+total) and
 * thread them onto the free list, so the arena's linear "walk by header
 * size" stays consistent across the gap.  Mirrors gc_sweep's free-block
 * format (size in the header low bits, type/mark = 0 → an unmarked block
 * the walk skips).  Splits gaps larger than the 23-bit size field into
 * multiple blocks (see gc_free_gap_chunk for the remainder invariant).
 * `total` is always a CL_ALIGN multiple and, when non-zero,
 * >= CL_MIN_ALLOC_SIZE (>= sizeof(CL_FreeBlock)), so every piece can
 * hold the 8-byte header. */
static void gc_make_free_gap(uint32_t offset, uint32_t total)
{
    while (total > 0) {
        uint32_t chunk = gc_free_gap_chunk(total, CL_ALIGN);
        if (chunk < (uint32_t)sizeof(CL_FreeBlock)) {
            /* Degenerate sub-header gap (caller invariant broken — total
             * itself smaller than a CL_FreeBlock).  Write a bare size-only
             * header so the linear arena walk stays in sync, but leave the
             * block off the free list: there is no room for the
             * next_offset link, and writing one would smash the following
             * object.  The space is reclaimed by the next compaction. */
            ((CL_Header *)(cl_heap.arena + offset))->header = chunk;
            if (GC_HDR_CROSSES(offset, chunk))
                gc_hdr_note(offset, chunk);
            return;
        }
        {
            CL_FreeBlock *fb = (CL_FreeBlock *)(cl_heap.arena + offset);
            fb->size = chunk;                       /* type=0, mark=0, size=chunk */
            fb->next_offset = cl_heap.free_list;
            cl_heap.free_list = offset;
#ifdef DEBUG_GC
            if (chunk > sizeof(CL_FreeBlock))
                memset((uint8_t *)fb + sizeof(CL_FreeBlock), 0xDE,
                       chunk - sizeof(CL_FreeBlock));
#endif
        }
        if (GC_HDR_CROSSES(offset, chunk))
            gc_hdr_note(offset, chunk);
        offset += chunk;
        total  -= chunk;
    }
}

/* Pass 4: Slide live objects to their forwarding addresses.
 * Objects only move downward (or stay), so forward copy is safe.
 *
 * Pinned objects (option B) keep their offset, which can leave a gap
 * between the compacted run below them and the pin.  Those gaps are
 * filled with free-block headers (gc_make_free_gap) and threaded onto
 * the free list — both so the linear arena walk stays valid and so the
 * allocator can reclaim the space.  With no pins this degenerates to a
 * gap-free compaction (free_list ends empty). */
static void gc_slide(void)
{
    uint8_t *ptr = cl_heap.arena + CL_ALIGN;
    uint8_t *end = cl_heap.arena + cl_heap.bump;
    uint32_t fill = CL_ALIGN;          /* next contiguous position in new layout */
    uint32_t live_total = 0;
    GcPlace pl;
    pl.cur = CL_ALIGN;
    pl.pin_i = 0;

    cl_heap.free_list = 0;

#ifdef CL_GENGC
    /* Full compaction rebuilds old space from scratch — restart the
     * crossing map; entries are re-noted as objects are placed below. */
    if (gen_enabled) {
        uint32_t i;
        for (i = 0; i < gen_npages; i++)
            gen_cross[i] = GEN_CROSS_NONE;
    }
#endif

    while (ptr < end) {
        uint32_t size = CL_HDR_SIZE(ptr);
        if (size == 0) break;

        if (CL_HDR_MARKED(ptr)) {
            uint32_t old_offset = (uint32_t)(ptr - cl_heap.arena);
            uint32_t new_offset = gc_place(&pl, old_offset, size, 0);
#if defined(DEBUG_GC) || defined(DEBUG_GC_STRESS)
            /* The lookup pass 3 used must agree with the placement the
             * slide performs — for the object start and for a granule
             * inside it. */
            if (gc_forward(old_offset) != new_offset ||
                (size > CL_ALIGN &&
                 gc_forward(old_offset + CL_ALIGN) != new_offset + CL_ALIGN)) {
                platform_write_string("GC: forwarding lookup disagrees with slide\n");
                abort();
            }
#endif

#ifdef CL_GENGC
            /* Sticky-mark invariant: in gen mode every live (old) object
             * keeps its mark bit set between collections, so the copied
             * survivor must stay marked; a major collection clears all
             * marks up front instead (see cl_gc_compact). */
            if (gen_enabled)
                gen_cross_note(new_offset);
            else
#endif
                CL_HDR_CLR_MARK(ptr);

            /* Gap before this object's new position (only at pins) → free. */
            if (new_offset > fill)
                gc_make_free_gap(fill, new_offset - fill);

            if (new_offset != old_offset)
                memmove(cl_heap.arena + new_offset, ptr, size);
            /* Block-start index: placed in ascending order, so the last
             * assignment per page is the covering block. */
            if (GC_HDR_CROSSES(new_offset, size))
                gc_hdr_note(new_offset, size);
            fill = new_offset + size;
            live_total += size;
        } else {
            /* Dead object — release external resources before overwriting */
            gc_finalize_dead(ptr);
        }
        ptr += size;
    }

    cl_heap.bump = fill;
    gc_hdr_bump_reset();
    /* total_allocated counts live data; gap free-blocks below bump are on
     * the free list and excluded (matches gc_sweep's accounting). */
    cl_heap.total_allocated = live_total;
}

/* Is a key's hash address-sensitive — i.e. does it change when the compacting
 * GC relocates objects? Mirrors hash_obj() in builtins_hashtable.c. If a table
 * has any such key it MUST be rehashed after a compaction, or stale buckets
 * make previously-inserted keys un-findable (gethash returns NIL). */
static int gc_key_addr_sensitive(CL_Obj key, uint32_t test)
{
    uint8_t type;
    if (!CL_HEAP_P(key))
        return 0;                 /* fixnum / char / immediate: stable */
    if (test == CL_HT_TEST_EQ)
        return 1;                 /* every heap object hashed by identity */
    type = CL_HDR_TYPE(CL_OBJ_TO_PTR(key));
    switch (type) {
    case TYPE_BIGNUM:
    case TYPE_RATIO:
    case TYPE_SINGLE_FLOAT:
    case TYPE_DOUBLE_FLOAT:
    case TYPE_COMPLEX:
        return 0;                 /* value-based hash under eql/equal/equalp */
    case TYPE_STRING:
#ifdef CL_WIDE_STRINGS
    case TYPE_WIDE_STRING:
#endif
    case TYPE_BIT_VECTOR:
        /* eql hashes these by identity; equal/equalp by contents (wide
         * strings and bit-vectors gained content hashes in tier-4 AH5). */
        return (test == CL_HT_TEST_EQL) ? 1 : 0;
    case TYPE_CONS:
        if (test == CL_HT_TEST_EQL)
            return 1;             /* cons hashed by identity under eql */
        /* equal/equalp: hash_obj() folds in only the car */
        return gc_key_addr_sensitive(((CL_Cons *)CL_OBJ_TO_PTR(key))->car, test);
    default:
        return 1;                 /* symbol, struct, instance, array, … */
    }
}

#if defined(DEBUG_GC) || defined(DEBUG_GC_STRESS)
/* Chain-integrity check: every bucket chain must terminate within ht->count
 * steps.  A longer walk means a chain CYCLE (double-forwarded bucket slot or
 * a put through a stale table pointer) — without this check the next
 * gc_rehash_table spins forever, which is undebuggable in the field. */
static void gc_verify_ht_chains(CL_Hashtable *ht, const char *when)
{
    CL_Obj *bkts;
    uint32_t i, walked = 0;
    if (!CL_NULL_P(ht->bucket_vec))
        bkts = ((CL_Vector *)CL_OBJ_TO_PTR(ht->bucket_vec))->data;
    else
        bkts = ht->buckets;
    for (i = 0; i < ht->bucket_count; i++) {
        CL_Obj chain = bkts[i];
        while (!CL_NULL_P(chain)) {
            if (++walked > ht->count + 8) {
                CL_Cons *e0 = (CL_Cons *)CL_OBJ_TO_PTR(bkts[i]);
                fprintf(stderr,
                        "[GC-HT-BUG] %s: hashtable @0x%08x (test=%u count=%u "
                        "buckets=%u flags=0x%x) chain walk exceeded count at "
                        "bucket %u - cycle/corruption\n"
                        "  chain=0x%08x entry->car(pair)=0x%08x "
                        "entry->cdr=0x%08x\n",
                        when,
                        (unsigned)((uint8_t *)ht - cl_heap.arena),
                        (unsigned)ht->test, (unsigned)ht->count,
                        (unsigned)ht->bucket_count, (unsigned)ht->flags,
                        (unsigned)i,
                        (unsigned)bkts[i], (unsigned)e0->car,
                        (unsigned)e0->cdr);
                if (CL_CONS_P(e0->car)) {
                    CL_Cons *p0 = (CL_Cons *)CL_OBJ_TO_PTR(e0->car);
                    fprintf(stderr, "  pair key=0x%08x val=0x%08x\n",
                            (unsigned)p0->car, (unsigned)p0->cdr);
                }
                /* Re-walk the arena and print the last objects leading up to
                 * this table — a desynced walk (bad header size upstream)
                 * shows up as implausible type/size runs here. */
                {
                    uint8_t *wp = cl_heap.arena + CL_ALIGN;
                    uint8_t *wend = cl_heap.arena + cl_heap.bump;
                    uint32_t woff[8]; uint32_t wtype[8]; uint32_t wsize[8];
                    int wn = 0;
                    while (wp < wend && wp <= (uint8_t *)ht) {
                        uint32_t wsz = CL_HDR_SIZE(wp);
                        if (wsz == 0) break;
                        woff[wn & 7] = (uint32_t)(wp - cl_heap.arena);
                        wtype[wn & 7] = CL_HDR_TYPE(wp);
                        wsize[wn & 7] = wsz;
                        wn++;
                        wp += wsz;
                    }
                    {
                        int k, first = wn > 8 ? wn - 8 : 0;
                        for (k = first; k < wn; k++)
                            fprintf(stderr,
                                    "  walk[%d] @0x%08x type=%u size=%u\n",
                                    k, (unsigned)woff[k & 7],
                                    (unsigned)wtype[k & 7],
                                    (unsigned)wsize[k & 7]);
                        fprintf(stderr, "  walk ended at 0x%08x (ht at 0x%08x, bump 0x%08x)\n",
                                (unsigned)(wp - cl_heap.arena),
                                (unsigned)((uint8_t *)ht - cl_heap.arena),
                                (unsigned)cl_heap.bump);
                    }
                }
                /* Who references this table?  A symbol-value referrer names
                 * the variable holding it. */
                {
                    CL_Obj ht_obj = CL_PTR_TO_OBJ((uint8_t *)ht);
                    uint8_t *wp = cl_heap.arena + CL_ALIGN;
                    uint8_t *wend = cl_heap.arena + cl_heap.bump;
                    int found = 0;
                    while (wp < wend && found < 8) {
                        uint32_t wsz = CL_HDR_SIZE(wp);
                        uint32_t nw, w;
                        if (wsz == 0) break;
                        nw = wsz / 4;
                        for (w = 1; w < nw; w++) {
                            if (((uint32_t *)wp)[w] == ht_obj) {
                                fprintf(stderr,
                                        "  referrer @0x%08x type=%u word=%u",
                                        (unsigned)(wp - cl_heap.arena),
                                        (unsigned)CL_HDR_TYPE(wp), (unsigned)w);
                                if (CL_HDR_TYPE(wp) == TYPE_SYMBOL) {
                                    CL_Symbol *rs = (CL_Symbol *)wp;
                                    if (CL_HEAP_P(rs->name)) {
                                        CL_String *rn = (CL_String *)CL_OBJ_TO_PTR(rs->name);
                                        fprintf(stderr, " symbol=%.*s",
                                                (int)rn->length, rn->data);
                                    }
                                }
                                fprintf(stderr, "\n");
                                found++;
                            }
                        }
                        wp += wsz;
                    }
                    if (!found)
                        fprintf(stderr, "  no arena referrer (C-global/root/TLV?)\n");
                }
                cl_capture_backtrace();
                fprintf(stderr, "%s", cl_backtrace_buf);
                fflush(stderr);
                abort();
            }
            if (!CL_CONS_P(chain)) {
                fprintf(stderr,
                        "[GC-HT-BUG] %s: hashtable %p bucket %u chain entry "
                        "is not a cons (0x%08x)\n",
                        when, (void *)ht, (unsigned)i, (unsigned)chain);
                fflush(stderr);
                abort();
            }
            chain = ((CL_Cons *)CL_OBJ_TO_PTR(chain))->cdr;
        }
    }
}
/* Sweep the arena and verify every populated table (see gc_verify_ht_chains). */
static void gc_verify_all_ht_chains(const char *when)
{
    uint8_t *ptr = cl_heap.arena + CL_ALIGN;
    uint8_t *end = cl_heap.arena + cl_heap.bump;
    while (ptr < end) {
        uint32_t size = CL_HDR_SIZE(ptr);
        if (size == 0) break;
        if (CL_HDR_TYPE(ptr) == TYPE_HASHTABLE) {
            CL_Hashtable *ht = (CL_Hashtable *)ptr;
            if (ht->count > 0)
                gc_verify_ht_chains(ht, when);
        }
        ptr += size;
    }
}
#endif

/* Rehash a single hash table after compaction (no allocation). Recomputes each
 * key's bucket with the table's own test, since relocated objects now hash to
 * different values. Skips tables whose every key is hash-stable. */
static void gc_rehash_table(CL_Hashtable *ht)
{
    CL_Obj *bkts;
    uint32_t bucket_count = ht->bucket_count;
    uint32_t test = ht->test;
    uint32_t i;
    int needs = 0;
    CL_Obj all_entries = CL_NIL;

    /* Get bucket array */
    if (!CL_NULL_P(ht->bucket_vec)) {
        CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(ht->bucket_vec);
        bkts = v->data;
    } else {
        bkts = ht->buckets;
    }

#if defined(DEBUG_GC) || defined(DEBUG_GC_STRESS)
    gc_verify_ht_chains(ht, "pre-rehash");
#endif

    /* Do any keys have address-sensitive hashes? If not, the buckets are still
     * valid after the move and we can leave the table untouched. */
    for (i = 0; i < bucket_count && !needs; i++) {
        CL_Obj chain = bkts[i];
        while (!CL_NULL_P(chain)) {
            CL_Cons *entry = (CL_Cons *)CL_OBJ_TO_PTR(chain);
            CL_Obj pair = entry->car;
            CL_Obj key = ((CL_Cons *)CL_OBJ_TO_PTR(pair))->car;
            if (gc_key_addr_sensitive(key, test)) { needs = 1; break; }
            chain = entry->cdr;
        }
    }
    if (!needs)
        return;

    /* Collect all entries into a single linked list, clear buckets */
    for (i = 0; i < bucket_count; i++) {
        CL_Obj chain = bkts[i];
        while (!CL_NULL_P(chain)) {
            CL_Cons *entry = (CL_Cons *)CL_OBJ_TO_PTR(chain);
            CL_Obj next = entry->cdr;
            entry->cdr = all_entries;
            all_entries = chain;
            chain = next;
        }
        bkts[i] = CL_NIL;
    }

    /* Redistribute using freshly computed hashes (post-move offsets) */
    while (!CL_NULL_P(all_entries)) {
        CL_Cons *entry = (CL_Cons *)CL_OBJ_TO_PTR(all_entries);
        CL_Obj next = entry->cdr;
        CL_Obj pair = entry->car;
        CL_Obj key = ((CL_Cons *)CL_OBJ_TO_PTR(pair))->car;
        uint32_t h = cl_hashtable_hash_key(key, test);
        uint32_t idx = h & (bucket_count - 1);
        entry->cdr = bkts[idx];
        bkts[idx] = all_entries;
        all_entries = next;
    }
}

/* Rehash ALL hash tables in the arena after compaction. Covers eq/eql/equal/
 * equalp: any of them can hash a key by object identity (objects fall through
 * to an identity hash), and identity changes when the GC moves the object. */
static void gc_rehash_tables(void)
{
    uint8_t *ptr = cl_heap.arena + CL_ALIGN;
    uint8_t *end = cl_heap.arena + cl_heap.bump;

    while (ptr < end) {
        uint32_t size = CL_HDR_SIZE(ptr);
        if (size == 0) break;
        if (CL_HDR_TYPE(ptr) == TYPE_HASHTABLE) {
            CL_Hashtable *ht = (CL_Hashtable *)ptr;
            if (ht->count > 0)
                gc_rehash_table(ht);
        }
        ptr += size;
    }
}

/* Rebuild every thread's TLV (dynamic-binding) table after a compaction.
 * Like the user hash tables, the TLV table is keyed by symbol arena-offset and
 * must be re-hashed once symbols have moved, or active dynamic bindings (e.g.
 * a `let` of a special var) silently revert to their global value. */
static void gc_rehash_tlv_tables(void)
{
    CL_Thread *t;
    /* During STW GC all threads are stopped; cl_thread_list is stable. */
    for (t = cl_thread_list; t; t = t->next)
        cl_tlv_rehash(t);
}

/* Run compaction if pending (called from safe points). */
void cl_gc_compact_if_pending(void)
{
    if (gc_compact_pending) {
        gc_compact_pending = 0;
        gc_last_compact_cycle = cl_heap.gc_count;
        cl_gc_compact();
    }
}

/* Main compaction entry point */
void cl_gc_compact(void)
{
    int multithread = (cl_thread_count > 1);
    uint64_t t0 = gc_stop_world_timed(multithread);

    /* Same invariant as cl_gc: no thread may keep cutting from a chunk the
     * passes below reclaim or slide.  Remainders are formatted holes, so
     * forwarding/slide parse them as dead regions. */
    gc_tlab_reset_all();

#ifdef CL_GENGC
    if (gen_enabled) {
        uint8_t *cptr, *cend;
        /* The passes below write mark bits and slide objects everywhere —
         * drop old-space protection for the duration. */
        gen_unprotect_all();
        /* Sticky marks would prune the FULL trace at the roots (gc_mark_obj
         * skips marked objects) — clear every mark bit first so this
         * collection sees the true live set.  One linear walk. */
        cptr = cl_heap.arena + CL_ALIGN;
        cend = cl_heap.arena + cl_heap.bump;
        while (cptr < cend) {
            uint32_t csize = CL_HDR_SIZE(cptr);
            if (csize == 0) break;
            CL_HDR_CLR_MARK(cptr);
            cptr += csize;
        }
    }
#endif

#ifdef DEBUG_GC
    platform_write_string("GC: compaction starting...\n");
#endif

    gc_native_code_patched = 0;

    /* Pass 1: Mark (standard) */
    cl_stream_outbuf_gc_mark_begin();
    gc_mark();
    /* Reclaim outbuf slots of streams that didn't survive marking, BEFORE the
     * slide overwrites their (now-dead) stream objects. */
    cl_stream_outbuf_gc_reclaim();
    gc_srcloc_invalidate_dead();

#ifdef DEBUG_GC
    /* Verify parent→child mark invariant immediately after marking so that
     * 'marked @X.field -> unmarked @Y' diagnostics fire at the exact
     * compaction that would corrupt the heap (not on the next sweep-only GC). */
    gc_verify_marked();
    if (gc_verify_errors > 0)
        gc_dump_roots_dbg();
#endif

    /* JIT pin-table OOM during marking: the pinned set is incomplete, so
     * moving anything would strand a live JIT frame's spilled offsets.
     * Degrade to a non-moving mark+sweep for this cycle (the mark phase
     * above is complete — only relocation is unsafe). */
    if (gc_jit_pin_oom) {
#ifdef DEBUG_GC
        platform_write_string("GC: compact suppressed (JIT pin-table OOM), "
                              "falling back to sweep\n");
#endif
        gc_sweep();
#ifdef CL_GENGC
        /* The sweep just rebuilt a free list and cleared survivor marks —
         * exactly the classic collector's state, and incompatible with the
         * generational invariants (dense old space, sticky marks).  Drop
         * to classic mode for the rest of the session. */
        if (gen_enabled) {
            gen_enabled = 0;
            platform_write_string("clamiga: generational GC disabled "
                                  "(compaction fallback) - classic GC\n");
        }
#endif
        cl_heap.gc_count++;
        /* Don't let the sweep-forever escape immediately re-attempt a
         * doomed compaction on every following allocation. */
        gc_sweeps_since_compact = 0;
        gc_time_compact_us += platform_time_us() - t0;
        if (gc_diag_on())
            gc_diag_report("compact-fallback", platform_time_us() - t0);
        if (multithread) cl_gc_resume_the_world();
        return;
    }

    /* Allocate the forwarding structures */
    if (!gc_fwd_alloc()) {
        if (gc_fwd_fail_bump == 0) {
            /* Say so once per episode (the latch below silences the
             * repeats): a heap that can no longer compact is a degraded
             * mode the user should see in the session log, not something
             * to infer from a sudden GC storm. */
            char msg[128];
            sprintf(msg, "clamiga: GC cannot compact - no %lu bytes of memory "
                         "for the forwarding table, sweeping instead\n",
                    (unsigned long)cl_gc_fwd_bytes());
            platform_write_string(msg);
        }
        gc_sweep();
#ifdef CL_GENGC
        /* See the pin-OOM fallback above: post-sweep heap state is
         * classic, so gen mode must stand down. */
        if (gen_enabled) {
            gen_enabled = 0;
            platform_write_string("clamiga: generational GC disabled "
                                  "(forwarding-table OOM) - classic GC\n");
        }
#endif
        cl_heap.gc_count++;
        /* Reset the sweep-forever escape counter and remember the bump
         * level that failed: without this, gc_sweeps_since_compact stays
         * >= GC_SWEEPS_BEFORE_COMPACT, so EVERY following bump-exhausted
         * allocation re-runs a doomed full mark (+ failed table alloc +
         * sweep) — a permanent 100%-CPU GC-thrash regime.  cl_alloc's
         * trigger 2 skips re-attempts until the bump front is below the
         * recorded failure level (the table size is proportional to it);
         * trigger 1 (compaction as last resort) still always tries. */
        gc_sweeps_since_compact = 0;
        gc_fwd_fail_bump = cl_heap.bump;
        gc_time_compact_us += platform_time_us() - t0;
        if (gc_diag_on())
            gc_diag_report("compact-fallback", platform_time_us() - t0);
        if (multithread) cl_gc_resume_the_world();
        return;
    }

    /* Pass 2: Compute forwarding addresses */
    gc_compute_forwarding();

    /* Pass 3: Update all references */
    gc_update_all_references();
    gc_srcloc_forward();

    /* Pass 4: Slide objects */
    gc_slide();


    /* Clean up forwarding table */
    gc_fwd_free();

    /* If we rewrote any CL_Obj immediate inside JIT'd native code, flush
     * the CPU caches so the patched instruction bytes are re-fetched.
     * REQUIRED on every 68020+ — the 020/030 also have an I-cache (256 B)
     * that can serve stale pre-patch lines; only the 68000/010 lack one.
     * Do NOT "optimize" this to 040/060-only.  No-op on host. */
    if (gc_native_code_patched)
        platform_cache_clear(NULL, 0);

    /* Rehash hash tables whose keys hash by object identity (offsets changed) */
    gc_rehash_tables();
#if defined(DEBUG_GC) || defined(DEBUG_GC_STRESS)
    /* Every object in the arena is a live survivor here — a dirty chain now
     * means THIS compact (or the mutator window before it) corrupted it. */
    gc_verify_all_ht_chains("post-rehash");
#endif

    /* Rehash per-thread TLV tables (keyed by symbol offset, which changed) */
    gc_rehash_tlv_tables();

    cl_heap.gc_count++;
    cl_heap.compact_count++;
#if defined(DEBUG_GC) || defined(DEBUG_GC_STRESS)
    gc_hdr_index_selfcheck();
#endif

    /* The bump pointer was just reset to the end of the surviving objects, so
     * the sweep-forever escape counter starts fresh — see gc_sweeps_since_compact. */
    gc_sweeps_since_compact = 0;
    gc_fwd_fail_bump = 0;   /* table allocation succeeded at this size */

#ifdef CL_GENGC
    if (gen_enabled) {
        /* Everything live is now dense below the bump front and (per the
         * gen-mode gc_slide) still mark-bitted: it becomes the new old
         * space.  Start a fresh dirty era over it.  The young-finalizable
         * list is invalidated wholesale: its offsets predate the slide,
         * dead entries were finalized by the slide, and survivors are old
         * now (the next major's full walk owns them). */
        gen_fin_count = 0;
        gen_old_top = cl_heap.bump;
        memset((void *)gen_dirty, 0, (gen_npages + 7) / 8);
        gen_protect_old();
    }
#endif

#ifdef DEBUG_GC
    /* Post-compaction verification: check all live objects have valid refs */
    {
        uint8_t *vptr = cl_heap.arena + CL_ALIGN;
        uint8_t *vend = cl_heap.arena + cl_heap.bump;
        int vc_errs = 0;
        char vbuf[256];
        while (vptr < vend && vc_errs < 5) {
            uint32_t vsize = CL_HDR_SIZE(vptr);
            uint8_t vtype = CL_HDR_TYPE(vptr);
            uint32_t voff = (uint32_t)(vptr - cl_heap.arena);
            if (vsize == 0) break;
            if (vtype == TYPE_CONS) {
                CL_Cons *c = (CL_Cons *)vptr;
                if (!CL_NULL_P(c->car) && !CL_FIXNUM_P(c->car) && !CL_CHAR_P(c->car)
                    && c->car != CL_UNBOUND && c->car >= cl_heap.bump) {
                    snprintf(vbuf, sizeof(vbuf),
                        "COMPACT-VERIFY: @0x%08x.car -> 0x%08x (OOB, bump=0x%08x)\n",
                        (unsigned)voff, (unsigned)c->car, (unsigned)cl_heap.bump);
                    platform_write_string(vbuf);
                    vc_errs++;
                }
                if (!CL_NULL_P(c->cdr) && !CL_FIXNUM_P(c->cdr) && !CL_CHAR_P(c->cdr)
                    && c->cdr != CL_UNBOUND && c->cdr >= cl_heap.bump) {
                    snprintf(vbuf, sizeof(vbuf),
                        "COMPACT-VERIFY: @0x%08x.cdr -> 0x%08x (OOB, bump=0x%08x)\n",
                        (unsigned)voff, (unsigned)c->cdr, (unsigned)cl_heap.bump);
                    platform_write_string(vbuf);
                    vc_errs++;
                }
            } else if (vtype == TYPE_VECTOR) {
                CL_Vector *v = (CL_Vector *)vptr;
                uint32_t vi;
                uint32_t nelt = (v->flags & CL_VEC_FLAG_DISPLACED) ? CL_DISP_BASE_IDX(v) + 2 :
                                ((v->rank > 1) ? (uint32_t)v->rank + v->length : v->length);
                for (vi = 0; vi < nelt && vc_errs < 5; vi++) {
                    CL_Obj elt = v->data[vi];
                    if (!CL_NULL_P(elt) && !CL_FIXNUM_P(elt) && !CL_CHAR_P(elt)
                        && elt != CL_UNBOUND && (elt & CL_TAG_MASK_LO2) == 0
                        && elt >= cl_heap.bump) {
                        snprintf(vbuf, sizeof(vbuf),
                            "COMPACT-VERIFY: @0x%08x vec[%u] -> 0x%08x (OOB, bump=0x%08x)\n",
                            (unsigned)voff, (unsigned)vi, (unsigned)elt,
                            (unsigned)cl_heap.bump);
                        platform_write_string(vbuf);
                        vc_errs++;
                    }
                }
            }
            vptr += vsize;
        }
    }
#endif

#ifdef DEBUG_GC
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "GC: compact done: %lu/%lu bytes used\n",
                 (unsigned long)cl_heap.total_allocated,
                 (unsigned long)cl_heap.arena_size);
        platform_write_string(buf);
    }
#endif

    gc_time_compact_us += platform_time_us() - t0;
    if (gc_diag_on())
        gc_diag_report("compact", platform_time_us() - t0);

    if (multithread)
        cl_gc_resume_the_world();
}

#ifdef CL_GENGC
/* ================================================================
 * Minor (nursery) collection — see the design block above cl_mem_init
 * and specs/generational-gc.md.
 * ================================================================ */

/* Walk the old-space objects overlapping dirty pages exactly once, in
 * ascending offset order.  mode 0: push children into the mark stack
 * (gc_mark_children skips marked old objects, so only nursery references
 * survive the filter).  mode 1: forward children in place and note
 * hash tables for the post-slide rehash.  Returns 0 on a header-walk
 * desync (caller must escalate to a full compaction), 1 otherwise. */
static int gen_walk_dirty_objects(int mode)
{
    uint32_t last_start = 0;
    uint32_t p;
    uint32_t dirty_pages = 0;
    /* Pages that intersect [CL_ALIGN, gen_old_top) */
    uint32_t old_pages = (gen_old_top + gen_page - 1) / gen_page;

    for (p = 0; p < old_pages; p++) {
        uint32_t pos, page_end, off;
        if (!(gen_dirty[p >> 3] & (uint8_t)(1u << (p & 7u))))
            continue;
        dirty_pages++;
        pos = p * gen_page;
        page_end = pos + gen_page;
        if (pos < CL_ALIGN) pos = CL_ALIGN;
        if (page_end > gen_old_top) page_end = gen_old_top;
        if (pos >= page_end)
            continue;

        off = gen_covering_object(pos);
        while (off < page_end) {
            uint8_t *ptr = cl_heap.arena + off;
            uint32_t size = CL_HDR_SIZE(ptr);
            if (size == 0)
                return 0;              /* desync — refuse the minor */
            if (off > last_start) {    /* spanning objects visit once */
                if (mode == 0) {
                    gc_mark_children(ptr, CL_HDR_TYPE(ptr));
                } else {
                    gc_update_children(ptr, CL_HDR_TYPE(ptr));
                    if (CL_HDR_TYPE(ptr) == TYPE_HASHTABLE &&
                        ((CL_Hashtable *)ptr)->count > 0)
                        gen_ht_note(off);   /* old tables don't move */
                }
                last_start = off;
            }
            off += size;
        }
    }
    if (mode == 0)
        gen_dirty_last = dirty_pages;
    return 1;
}

/* qsort comparator for the survivor list (ascending offsets). */
static int gen_off_cmp(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x < y) ? -1 : (x > y);
}

int cl_gc_minor(uint32_t seen_gc_count)
{
    int multithread = (cl_thread_count > 1);
    uint64_t t0;
    uint32_t new_top, i;

    if (!gen_enabled)
        return 0;

    t0 = gc_stop_world_timed(multithread);

    /* Same redundant-cycle dedup as cl_gc_if_stale: a peer collected while
     * we raced for initiator-ship — let the caller retry allocation. */
    if (multithread && cl_heap.gc_count != seen_gc_count) {
        gc_epoch_skips++;
        cl_gc_resume_the_world();   /* multithread here by construction */
        return 1;
    }

    gc_tlab_reset_all();

    /* GC passes write mark bits / slide within the nursery and read+write
     * old space (rehash) — drop protection for the duration. */
    gen_unprotect_all();

    /* --- Mark live nursery objects, collecting the survivor list ---
     * (a) all roots via the standard enumeration: sticky old marks
     *     terminate the trace at the old generation;
     * (b) precise children of old objects on dirty pages.
     * Every pass below iterates the survivor list — a minor never walks
     * the (dominant) dead nursery space. */
    gen_live_count = 0;
    gen_live_oom = 0;
    gen_minor_active = 1;
    gc_mark();
    if (!gen_walk_dirty_objects(0)) {
        /* desync: escalate — cl_gc_compact re-clears marks */
        gen_minor_active = 0;
        if (multithread) cl_gc_resume_the_world();
        return 0;
    }
    while (gc_mark_top > 0)
        gc_mark_obj(gc_mark_stack[--gc_mark_top]);
    gen_minor_active = 0;
    if (gc_mark_overflow || gen_live_oom ||
        jit_pinned_count > 0 || gc_jit_pin_oom) {
        /* Overflow recovery would need a full-heap rescan (sticky marks
         * make it O(heap)), a failed survivor-list growth leaves the list
         * incomplete, and pinned JIT frames never happen on hosts — all
         * are escalate-to-major events, not errors.  The world stays
         * consistent: compaction clears all marks up front. */
        if (multithread) cl_gc_resume_the_world();
        return 0;
    }
    gc_srcloc_invalidate_dead();

    /* --- Assign survivor addresses: slide down onto the watermark --- */
    qsort(gen_live, (size_t)gen_live_count, sizeof(uint32_t), gen_off_cmp);
    new_top = gen_old_top;
    for (i = 0; i < gen_live_count; i++) {
        gen_live_new[i] = new_top;
        new_top += CL_HDR_SIZE(cl_heap.arena + gen_live[i]);
    }
    gen_minor_fwd = 1;   /* gc_forward now resolves via the list */

    /* --- Update references (old addresses still valid) --- */
    gen_ht_count = 0;
    gc_update_all_roots();
    gc_srcloc_forward();
    if (!gen_walk_dirty_objects(1)) {
        /* Cannot happen: the mode-0 walk of the SAME pages over the SAME
         * crossing map succeeded moments ago under stop-the-world.  If it
         * does, roots are already forwarded and there is no rollback —
         * fail loudly instead of corrupting silently. */
        platform_write_string("FATAL: gengc dirty-page walk desynced "
                              "mid-update - heap invariant broken\n");
        platform_flush_output();
        exit(1);
    }
    /* Survivors' own children + young hash tables (note POST-slide offs) */
    for (i = 0; i < gen_live_count; i++) {
        uint8_t *ptr = cl_heap.arena + gen_live[i];
        gc_update_children(ptr, CL_HDR_TYPE(ptr));
        if (CL_HDR_TYPE(ptr) == TYPE_HASHTABLE &&
            ((CL_Hashtable *)ptr)->count > 0)
            gen_ht_note(gen_live_new[i]);
    }

    /* --- Finalize dead young finalizable objects (recorded at allocation
     * time — the only dead nursery objects a minor must still visit) --- */
    for (i = 0; i < gen_fin_count; i++) {
        uint8_t *ptr = cl_heap.arena + gen_fin[i];
        if (!CL_HDR_MARKED(ptr))
            gc_finalize_dead(ptr);
        /* marked entries are promoted below; the next major's full walk
         * owns their finalization from here on */
    }
    gen_fin_count = 0;

    /* --- Slide survivors down; they keep their mark bit (old now) --- */
    for (i = 0; i < gen_live_count; i++) {
        uint32_t old_off = gen_live[i];
        uint32_t new_off = gen_live_new[i];
        uint32_t size = CL_HDR_SIZE(cl_heap.arena + old_off);
        gen_cross_note(new_off);
        if (new_off != old_off)
            memmove(cl_heap.arena + new_off, cl_heap.arena + old_off, size);
    }
    gen_minor_fwd = 0;

    /* --- Post-move fixups --- */
    for (i = 0; i < gen_ht_count; i++) {
        CL_Hashtable *ht = (CL_Hashtable *)(cl_heap.arena + gen_ht_list[i]);
        gc_rehash_table(ht);
    }
    gen_ht_count = 0;
    gc_rehash_tlv_tables();

    gen_promoted_bytes += new_top - gen_old_top;
    cl_heap.total_allocated = new_top - CL_ALIGN;
    gen_old_top = new_top;
    cl_heap.bump = new_top;

    /* Fresh dirty era: everything below the watermark is protected. */
    memset((void *)gen_dirty, 0, (gen_npages + 7) / 8);
    gen_protect_old();

    cl_heap.gc_count++;
    gen_minor_count++;
    gc_time_minor_us += platform_time_us() - t0;
    if (gc_diag_on())
        gc_diag_report("minor", platform_time_us() - t0);

    if (multithread)
        cl_gc_resume_the_world();
    return 1;
}
#endif /* CL_GENGC */

/* Post-GC verification: check all marked objects have valid children.
 * Must be called AFTER gc_mark() but BEFORE gc_sweep() (marks still set).
 * Reports any marked object that points to an unmarked heap object. */
#ifdef DEBUG_GC

/* Dump the per-thread GC root stack with file:line tags (DEBUG_GC only).
 * Call this after gc_verify_marked() finds errors to name the C frames that
 * hold the suspicious roots — the same technique used to finger cl_cons's
 * arg at mem.c:378 in the PROGN-cursor bug.
 *
 * Must #undef gc_root_count because thread.h defines it as (CT->gc_root_count),
 * which collides with t->gc_root_count member access. */
#undef gc_root_count
static void gc_dump_roots_dbg(void)
{
    /* During STW GC all threads are stopped; cl_thread_list is stable — no lock needed. */
    CL_Thread *t;
    char buf[256];

    for (t = cl_thread_list; t; t = t->next) {
        int i;
        snprintf(buf, sizeof(buf),
                 "GC-ROOTS: thread %u - %d protected roots:\n",
                 (unsigned)t->id, t->gc_root_count);
        platform_write_string(buf);

        for (i = 0; i < t->gc_root_count; i++) {
            CL_Obj val = *t->gc_roots[i];
            int is_heap = (!CL_NULL_P(val) && !CL_FIXNUM_P(val) && !CL_CHAR_P(val)
                           && val != CL_UNBOUND && val < cl_heap.arena_size);
            int marked  = (is_heap && CL_HDR_MARKED(CL_OBJ_TO_PTR(val)));

            snprintf(buf, sizeof(buf),
                     "  root[%d] val=0x%08x %s  @ %s:%d\n",
                     i, (unsigned)val,
                     is_heap ? (marked ? "(heap,marked)" : "(heap,UNMARKED)") : "(imm)",
                     t->gc_root_files[i] ? t->gc_root_files[i] : "?",
                     t->gc_root_lines[i]);
            platform_write_string(buf);
        }
    }
}
#define gc_root_count (CT->gc_root_count)

static void gc_verify_check_ref(CL_Obj parent_offset, const char *field,
                                CL_Obj child)
{
    void *child_ptr;
    if (CL_NULL_P(child) || CL_FIXNUM_P(child) || CL_CHAR_P(child))
        return;
    if (child >= cl_heap.arena_size) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "GC VERIFY: marked @0x%08x.%s -> OUT OF BOUNDS 0x%08x (arena 0x%08x)\n",
                 (unsigned)parent_offset, field,
                 (unsigned)child, (unsigned)cl_heap.arena_size);
        platform_write_string(buf);
        gc_verify_errors++;
        return;
    }
    child_ptr = CL_OBJ_TO_PTR(child);
    if (!CL_HDR_MARKED(child_ptr)) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "GC VERIFY: marked @0x%08x.%s -> unmarked @0x%08x (type %u)\n",
                 (unsigned)parent_offset, field,
                 (unsigned)child, (unsigned)CL_HDR_TYPE(child_ptr));
        platform_write_string(buf);
        gc_verify_errors++;
    }
}

static void gc_verify_marked(void)
{
    uint8_t *ptr = cl_heap.arena + CL_ALIGN;
    uint8_t *end = cl_heap.arena + cl_heap.bump;
    gc_verify_errors = 0;

    while (ptr < end) {
        uint32_t size = CL_HDR_SIZE(ptr);
        if (size == 0) break;

        if (CL_HDR_MARKED(ptr)) {
            CL_Obj parent_off = (CL_Obj)(ptr - cl_heap.arena);
            uint8_t type = CL_HDR_TYPE(ptr);

            switch (type) {
            case TYPE_CONS: {
                CL_Cons *c = (CL_Cons *)ptr;
                gc_verify_check_ref(parent_off, "car", c->car);
                gc_verify_check_ref(parent_off, "cdr", c->cdr);
                break;
            }
            case TYPE_SYMBOL: {
                CL_Symbol *s = (CL_Symbol *)ptr;
                gc_verify_check_ref(parent_off, "name", s->name);
                if (s->value != CL_UNBOUND)
                    gc_verify_check_ref(parent_off, "value", s->value);
                if (s->function != CL_UNBOUND)
                    gc_verify_check_ref(parent_off, "function", s->function);
                gc_verify_check_ref(parent_off, "plist", s->plist);
                gc_verify_check_ref(parent_off, "package", s->package);
                break;
            }
            case TYPE_CLOSURE: {
                CL_Closure *cl = (CL_Closure *)ptr;
                uint32_t n = (size - sizeof(CL_Closure)) / sizeof(CL_Obj);
                uint32_t i;
                gc_verify_check_ref(parent_off, "bytecode", cl->bytecode);
                for (i = 0; i < n; i++)
                    gc_verify_check_ref(parent_off, "upval", cl->upvalues[i]);
                break;
            }
            case TYPE_BYTECODE: {
                CL_Bytecode *bc = (CL_Bytecode *)ptr;
                uint16_t i;
                gc_verify_check_ref(parent_off, "name", bc->name);
                gc_verify_check_ref(parent_off, "lambda-list", bc->source_lambda_list);
                for (i = 0; i < bc->n_constants; i++)
                    gc_verify_check_ref(parent_off, "const", bc->constants[i]);
                break;
            }
            case TYPE_VECTOR: {
                CL_Vector *v = (CL_Vector *)ptr;
                uint32_t i;
                if (v->flags & CL_VEC_FLAG_DISPLACED) {
                    gc_verify_check_ref(parent_off, "displaced",
                                        v->data[CL_DISP_BASE_IDX(v)]);
                } else {
                    uint32_t n = (v->rank > 1) ? (uint32_t)v->rank + v->length : v->length;
                    for (i = 0; i < n; i++)
                        gc_verify_check_ref(parent_off, "elt", v->data[i]);
                }
                break;
            }
            case TYPE_HASHTABLE: {
                CL_Hashtable *ht = (CL_Hashtable *)ptr;
                uint32_t i;
                gc_verify_check_ref(parent_off, "bucket_vec", ht->bucket_vec);
                if (CL_NULL_P(ht->bucket_vec)) {
                    for (i = 0; i < ht->bucket_count; i++)
                        gc_verify_check_ref(parent_off, "bucket", ht->buckets[i]);
                }
                break;
            }
            case TYPE_STRUCT: {
                CL_Struct *st = (CL_Struct *)ptr;
                uint32_t i;
                gc_verify_check_ref(parent_off, "type_desc", st->type_desc);
                for (i = 0; i < st->n_slots; i++)
                    gc_verify_check_ref(parent_off, "slot", st->slots[i]);
                break;
            }
            case TYPE_CONDITION: {
                CL_Condition *cond = (CL_Condition *)ptr;
                gc_verify_check_ref(parent_off, "type_name", cond->type_name);
                gc_verify_check_ref(parent_off, "slots", cond->slots);
                gc_verify_check_ref(parent_off, "report", cond->report_string);
                break;
            }
            case TYPE_RESTART: {
                CL_Restart *r = (CL_Restart *)ptr;
                gc_verify_check_ref(parent_off, "name", r->name);
                gc_verify_check_ref(parent_off, "function", r->function);
                gc_verify_check_ref(parent_off, "report", r->report);
                gc_verify_check_ref(parent_off, "interactive", r->interactive);
                gc_verify_check_ref(parent_off, "test", r->test);
                gc_verify_check_ref(parent_off, "tag", r->tag);
                break;
            }
            case TYPE_FFI_STUB: {
                CL_FfiStub *fs = (CL_FfiStub *)ptr;
                gc_verify_check_ref(parent_off, "name", fs->name);
                gc_verify_check_ref(parent_off, "aux", fs->aux);
                break;
            }
            case TYPE_STREAM: {
                CL_Stream *st = (CL_Stream *)ptr;
                gc_verify_check_ref(parent_off, "string_buf", st->string_buf);
                gc_verify_check_ref(parent_off, "element_type", st->element_type);
                break;
            }
            case TYPE_FUNCTION: {
                CL_Function *f = (CL_Function *)ptr;
                gc_verify_check_ref(parent_off, "name", f->name);
                break;
            }
            case TYPE_RATIO: {
                CL_Ratio *r = (CL_Ratio *)ptr;
                gc_verify_check_ref(parent_off, "num", r->numerator);
                gc_verify_check_ref(parent_off, "den", r->denominator);
                break;
            }
            case TYPE_COMPLEX: {
                CL_Complex *cx = (CL_Complex *)ptr;
                gc_verify_check_ref(parent_off, "real", cx->realpart);
                gc_verify_check_ref(parent_off, "imag", cx->imagpart);
                break;
            }
            case TYPE_PATHNAME: {
                CL_Pathname *pn = (CL_Pathname *)ptr;
                gc_verify_check_ref(parent_off, "host", pn->host);
                gc_verify_check_ref(parent_off, "device", pn->device);
                gc_verify_check_ref(parent_off, "dir", pn->directory);
                gc_verify_check_ref(parent_off, "name", pn->name);
                gc_verify_check_ref(parent_off, "type", pn->type);
                gc_verify_check_ref(parent_off, "version", pn->version);
                break;
            }
            case TYPE_CELL: {
                CL_Cell *cell = (CL_Cell *)ptr;
                gc_verify_check_ref(parent_off, "value", cell->value);
                break;
            }
            case TYPE_PACKAGE: {
                CL_Package *p = (CL_Package *)ptr;
                gc_verify_check_ref(parent_off, "name", p->name);
                gc_verify_check_ref(parent_off, "symbols", p->symbols);
                gc_verify_check_ref(parent_off, "use_list", p->use_list);
                gc_verify_check_ref(parent_off, "nicknames", p->nicknames);
                gc_verify_check_ref(parent_off, "local_nicknames", p->local_nicknames);
                gc_verify_check_ref(parent_off, "shadowing", p->shadowing_symbols);
                gc_verify_check_ref(parent_off, "exported", p->exported_symbols);
                gc_verify_check_ref(parent_off, "bindings", p->bindings);
                break;
            }
            default:
                break;
            }
            if (gc_verify_errors > 20) {
                platform_write_string("GC VERIFY: too many errors, stopping\n");
                return;
            }
        }
        ptr += size;
    }
}

/* Check if an arena offset points to a freed block by testing for poison
 * fill at offset 8 (sizeof(CL_FreeBlock) = 8).  All freed blocks >= 16
 * bytes (CL_MIN_ALLOC_SIZE) have bytes 8-11 poisoned with 0xDE. */
static int gc_is_freed(uint32_t offset)
{
    uint8_t *p = cl_heap.arena + offset;
    /* The poison word lives at bytes 8..11 — it must stay inside the
     * walked arena (callers pass arbitrary CL_Obj values here). */
    if (offset + 12 > cl_heap.bump)
        return 0;
    /* Blocks of exactly sizeof(CL_FreeBlock) — the 8-byte pin-gap chunks
     * gc_make_free_gap can emit — have no room for poison: their bytes
     * 8..11 belong to the NEXT block, so testing them would misreport
     * based on a neighbor's data.  Treat as not-freed (a false negative
     * only; this is a DEBUG_GC diagnostic helper). */
    if (CL_HDR_SIZE(p) <= sizeof(CL_FreeBlock))
        return 0;
    /* Freed blocks have poison at offset 8 (after the 8-byte CL_FreeBlock header) */
    return (p[8] == 0xDE && p[9] == 0xDE && p[10] == 0xDE && p[11] == 0xDE);
}

/* Post-sweep verification: check that no live object contains a reference
 * to a freed block (use-after-free detection).  Uses poison fill pattern
 * to identify freed blocks (can't use type==0 since TYPE_CONS==0). */
static void gc_verify_after_sweep(void)
{
    uint8_t *ptr = cl_heap.arena + CL_ALIGN;
    uint8_t *end = cl_heap.arena + cl_heap.bump;
    int errors = 0;

    while (ptr < end) {
        uint32_t obj_size = CL_HDR_SIZE(ptr);
        CL_Obj parent_off = (CL_Obj)(ptr - cl_heap.arena);

        if (obj_size == 0) break;

        /* Skip free blocks */
        if (gc_is_freed(parent_off)) {
            ptr += obj_size;
            continue;
        }

        /* Live object — check CL_Obj fields for poison or dead refs */
        {
            uint8_t type = CL_HDR_TYPE(ptr);

            #define CHECK_FIELD(val, fname) do { \
                CL_Obj _v = (val); \
                if (_v == 0xDEDEDEDEu) { \
                    char buf[256]; \
                    snprintf(buf, sizeof(buf), \
                             "POST-SWEEP: @0x%08x.%s = POISON 0xDEDEDEDE (type %u)\n", \
                             (unsigned)parent_off, fname, (unsigned)type); \
                    platform_write_string(buf); \
                    errors++; \
                } else if (!CL_NULL_P(_v) && !CL_FIXNUM_P(_v) && !CL_CHAR_P(_v) \
                           && _v < cl_heap.arena_size && gc_is_freed(_v)) { \
                    char buf[256]; \
                    snprintf(buf, sizeof(buf), \
                             "POST-SWEEP: @0x%08x.%s -> freed @0x%08x (type %u)\n", \
                             (unsigned)parent_off, fname, (unsigned)_v, (unsigned)type); \
                    platform_write_string(buf); \
                    errors++; \
                } \
            } while(0)

            switch (type) {
            case TYPE_CONS: {
                CL_Cons *c = (CL_Cons *)ptr;
                CHECK_FIELD(c->car, "car");
                CHECK_FIELD(c->cdr, "cdr");
                break;
            }
            case TYPE_SYMBOL: {
                CL_Symbol *s = (CL_Symbol *)ptr;
                CHECK_FIELD(s->name, "name");
                if (s->value != CL_UNBOUND) CHECK_FIELD(s->value, "value");
                if (s->function != CL_UNBOUND) CHECK_FIELD(s->function, "function");
                CHECK_FIELD(s->plist, "plist");
                CHECK_FIELD(s->package, "package");
                break;
            }
            case TYPE_CLOSURE: {
                CL_Closure *cl = (CL_Closure *)ptr;
                uint32_t n = (obj_size - sizeof(CL_Closure)) / sizeof(CL_Obj);
                uint32_t i;
                CHECK_FIELD(cl->bytecode, "bytecode");
                for (i = 0; i < n; i++)
                    CHECK_FIELD(cl->upvalues[i], "upval");
                break;
            }
            case TYPE_BYTECODE: {
                CL_Bytecode *bc = (CL_Bytecode *)ptr;
                CHECK_FIELD(bc->name, "name");
                CHECK_FIELD(bc->source_lambda_list, "lambda-list");
                break;
            }
            case TYPE_VECTOR: {
                CL_Vector *v = (CL_Vector *)ptr;
                uint32_t i;
                if (v->flags & CL_VEC_FLAG_DISPLACED) {
                    CHECK_FIELD(v->data[CL_DISP_BASE_IDX(v)], "displaced");
                } else {
                    uint32_t n = (v->rank > 1) ? (uint32_t)v->rank + v->length : v->length;
                    for (i = 0; i < n; i++)
                        CHECK_FIELD(v->data[i], "elt");
                }
                break;
            }
            case TYPE_HASHTABLE: {
                CL_Hashtable *ht = (CL_Hashtable *)ptr;
                uint32_t i;
                CHECK_FIELD(ht->bucket_vec, "bucket_vec");
                if (CL_NULL_P(ht->bucket_vec)) {
                    for (i = 0; i < ht->bucket_count; i++)
                        CHECK_FIELD(ht->buckets[i], "bucket");
                }
                break;
            }
            default:
                break;
            }
            #undef CHECK_FIELD

            if (errors > 10) {
                platform_write_string("POST-SWEEP: too many errors, stopping\n");
                return;
            }
        }
        ptr += obj_size;
    }

    if (errors > 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "POST-SWEEP: %d use-after-free errors detected\n", errors);
        platform_write_string(buf);
    }
}
#endif

/* Core of a sweep GC — runs with the world already stopped (or
 * single-threaded).  Split out so cl_gc_if_stale can decide AFTER winning
 * the stop-the-world race whether a collection is still needed. */
static void cl_gc_stopped(uint64_t t0)
{
    uint64_t t1;
    uint64_t diag_t_begin = t0;   /* CLAMIGA_GC_DIAG: pause = whole collection */

    /* Drop every thread's TLAB before any pass runs: gc_sweep rebuilds the
     * free list from unmarked regions, so an active chunk tail (already a
     * formatted hole) would otherwise be handed out twice — once by the
     * free list, once by the owning thread's next cut.  Runs even when
     * single-threaded to reclaim leftovers after thread-count dropped. */
    gc_tlab_reset_all();

#ifdef DEBUG_GC
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "GC #%lu: marking...\n",
                 (unsigned long)(cl_heap.gc_count + 1));
        platform_write_string(buf);
    }
#endif
    cl_stream_outbuf_gc_mark_begin();
    gc_mark();
    /* Reclaim outbuf slots no live stream pinned, before gc_sweep coalesces
     * the dead stream objects into free blocks. */
    cl_stream_outbuf_gc_reclaim();
    gc_srcloc_invalidate_dead();
    t1 = platform_time_us();
    gc_time_mark_us += t1 - t0;
    t0 = t1;
#ifdef DEBUG_GC
    gc_verify_marked();
    if (gc_verify_errors > 0)
        gc_dump_roots_dbg();
    /* After marking, find unmarked objects still referenced from VM stack.
     * Walk each VM stack slot: if it's a heap object that IS marked but is
     * a cons whose car or cdr points to an UNMARKED heap object, that child
     * should have been transitively marked.  This catches cases where a
     * marked cons references an unmarked child — indicating a marking bug. */
    {
        int si;
        char dbuf[256];
        for (si = 0; si < cl_vm.sp; si++) {
            CL_Obj v = cl_vm.stack[si];
            if (CL_NULL_P(v) || CL_FIXNUM_P(v) || CL_CHAR_P(v)) continue;
            if (v >= cl_heap.arena_size) continue;
            if (!CL_HDR_MARKED(CL_OBJ_TO_PTR(v))) {
                snprintf(dbuf, sizeof(dbuf),
                         "GC-DIAG: VM stack[%d]=0x%08x type=%d UNMARKED!\n",
                         si, (unsigned)v, CL_HDR_TYPE(CL_OBJ_TO_PTR(v)));
                platform_write_string(dbuf);
            }
        }
        /* Also check frame bytecodes: if a bytecode's constant references
         * an unmarked object, the constant wasn't transitively marked */
        for (si = 0; si < cl_vm.fp; si++) {
            CL_Obj bc_obj = cl_vm.frames[si].bytecode;
            if (CL_NULL_P(bc_obj) || CL_FIXNUM_P(bc_obj) || CL_CHAR_P(bc_obj)) continue;
            if (bc_obj >= cl_heap.arena_size) continue;
            if (!CL_HDR_MARKED(CL_OBJ_TO_PTR(bc_obj))) {
                snprintf(dbuf, sizeof(dbuf),
                         "GC-DIAG: frame[%d] bytecode=0x%08x UNMARKED!\n",
                         si, (unsigned)bc_obj);
                platform_write_string(dbuf);
            } else {
                void *ptr = CL_OBJ_TO_PTR(bc_obj);
                if (CL_HDR_TYPE(ptr) == TYPE_BYTECODE) {
                    CL_Bytecode *bc = (CL_Bytecode *)ptr;
                    int ci;
                    for (ci = 0; ci < bc->n_constants; ci++) {
                        CL_Obj cval = bc->constants[ci];
                        if (CL_NULL_P(cval) || CL_FIXNUM_P(cval) || CL_CHAR_P(cval)) continue;
                        if (cval >= cl_heap.arena_size) continue;
                        if (!CL_HDR_MARKED(CL_OBJ_TO_PTR(cval))) {
                            snprintf(dbuf, sizeof(dbuf),
                                     "GC-DIAG: frame[%d] bc=0x%08x const[%d]=0x%08x type=%d UNMARKED!\n",
                                     si, (unsigned)bc_obj, ci, (unsigned)cval,
                                     CL_HDR_TYPE(CL_OBJ_TO_PTR(cval)));
                            platform_write_string(dbuf);
                        }
                    }
                }
            }
        }
    }
#endif
    gc_sweep();
    gc_time_sweep_us += platform_time_us() - t0;
#ifdef DEBUG_GC
    gc_verify_after_sweep();
#endif
    cl_heap.gc_count++;
#if defined(DEBUG_GC) || defined(DEBUG_GC_STRESS)
    gc_hdr_index_selfcheck();
#endif
    if (gc_diag_on())
        gc_diag_report("sweep", platform_time_us() - diag_t_begin);
#ifdef DEBUG_GC
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "GC #%lu done: %lu/%lu bytes used\n",
                 (unsigned long)cl_heap.gc_count,
                 (unsigned long)cl_heap.total_allocated,
                 (unsigned long)cl_heap.arena_size);
        platform_write_string(buf);
    }
#endif
}

/* Stop the world (multi-threaded), accounting the wait into the STW phase
 * timers, and return the timestamp at which the world was stopped. */
static uint64_t gc_stop_world_timed(int multithread)
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
    int multithread;
    uint64_t t0;

#ifdef CL_GENGC
    /* Gen mode has no sweep collector: a sweep would rebuild the free
     * list inside old space and break its density (and clear the sticky
     * marks).  An explicit full GC is a compaction there. */
    if (gen_enabled) {
        cl_gc_compact();
        return;
    }
#endif

    multithread = (cl_thread_count > 1);
    t0 = gc_stop_world_timed(multithread);

    cl_gc_stopped(t0);

    /* Resume all stopped threads */
    if (multithread)
        cl_gc_resume_the_world();
}

void cl_gc_reclaim_young(void)
{
#ifdef CL_GENGC
    if (gen_enabled) {
        /* A refused minor is fine here — the caller escalates to a full
         * cl_gc() when its retry still finds no free slot. */
        (void)cl_gc_minor(cl_heap.gc_count);
        return;
    }
#endif
    cl_gc();
}

int cl_gc_if_stale(uint32_t seen_gc_count)
{
    int multithread;
    uint64_t t0;

#ifdef CL_GENGC
    /* Defensive: the gen-mode allocation path uses cl_gc_minor and never
     * reaches this, but any other caller gets full-GC semantics. */
    if (gen_enabled) {
        cl_gc_compact();
        return 1;
    }
#endif

    multithread = (cl_thread_count > 1);
    t0 = gc_stop_world_timed(multithread);

    /* Allocation-triggered GC dedup: when N threads exhaust the bump front
     * near-simultaneously, each loser of the stop-the-world initiator race
     * used to run its own full mark-sweep on the heap a peer had JUST
     * swept — a burst of redundant back-to-back stop-the-world cycles.  If
     * the GC epoch advanced between the caller observing exhaustion (under
     * alloc_mutex) and this thread winning initiator-ship, the heap state
     * that motivated this call is gone: skip, and let the caller retry its
     * allocation against the peer's sweep (it falls back to a real cl_gc
     * if the retry still finds no fit). */
    if (multithread && cl_heap.gc_count != seen_gc_count) {
        gc_epoch_skips++;
        cl_gc_resume_the_world();
        return 0;
    }

    cl_gc_stopped(t0);

    if (multithread)
        cl_gc_resume_the_world();
    return 1;
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

/* ======================================================================
 * Heap verification on demand -- (ext:%heap-verify)
 *
 * A diagnostic for the field, where a corrupted heap first shows itself
 * as an unrelated error ("not a function", a keyword that is no symbol)
 * long after the corruption happened, and where stderr is not seen
 * (AmigaOS, a script's `>log').  It collects (so that every remaining
 * non-free block is a live object whose every reference must be
 * sound), then walks the arena and checks each object's header and
 * each of its child references against a bitmap of real object starts,
 * plus every registered static root and every thread's protect stack.
 * A reference that lands past the bump front, unaligned, inside another
 * object, in a free block, or on a header with an impossible type is
 * reported with its holder.  Answers the fault count; the report text
 * (bounded) is left in a static buffer for cl_gc_verify_heap_report,
 * which the builtin turns into a Lisp string AFTER the walk (the walk
 * itself allocates nothing).
 * ====================================================================== */

#define HV_REPORT_CAP 3072
/* Kept back from the ordinary lines: the "...\n" cut mark (4 bytes) and the
 * closing summary (87 characters at most, plus the NUL). */
#define HV_TAIL_ROOM  96
static char hv_report[HV_REPORT_CAP];
static int  hv_report_len;
static int  hv_cut;                 /* the ordinary lines ran out of room */
static int  hv_faults;
static uint8_t *hv_starts;          /* bit per CL_ALIGN unit: an object starts here */
static uint8_t *hv_free;            /* bit per CL_ALIGN unit: a free block starts here */

/* Append one formatted line if all of it fits below cap; answers whether
 * it did (a line that does not fit is left out whole). */
static int hv_append(int cap, const char *fmt, va_list ap)
{
    int room = cap - hv_report_len;
    int n = vsnprintf(hv_report + hv_report_len, room, fmt, ap);
    if (n < 0 || n >= room) {
        hv_report[hv_report_len] = '\0';
        return 0;
    }
    hv_report_len += n;
    return 1;
}

/* An ordinary line.  They stop HV_TAIL_ROOM short of the buffer's end, with
 * a single "..." to say lines were left out. */
static void hv_say(const char *fmt, ...)
{
    va_list ap;
    int ok;
    if (hv_cut) return;
    va_start(ap, fmt);
    ok = hv_append(HV_REPORT_CAP - HV_TAIL_ROOM, fmt, ap);
    va_end(ap);
    if (!ok) {
        strcpy(hv_report + hv_report_len, "...\n");
        hv_report_len += 4;
        hv_cut = 1;
    }
}

/* The closing line: it is written however full the faults left the buffer,
 * because a heavily corrupted heap is exactly where its count is wanted. */
static void hv_summary(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    hv_append(HV_REPORT_CAP, fmt, ap);
    va_end(ap);
}

#define HV_BIT_SET(map, off)  ((map)[((off) / CL_ALIGN) >> 3] |= (uint8_t)(1u << (((off) / CL_ALIGN) & 7u)))
#define HV_BIT_GET(map, off)  (((map)[((off) / CL_ALIGN) >> 3] >> (((off) / CL_ALIGN) & 7u)) & 1u)

/* What is wrong with a reference, or NULL when nothing is. */
static const char *hv_ref_fault(CL_Obj ref)
{
    if (CL_NULL_P(ref) || CL_FIXNUM_P(ref) || CL_CHAR_P(ref)) return NULL;
    if (ref == CL_UNBOUND) return "the UNBOUND sentinel";
    if (!CL_HEAP_P(ref)) return "not a tagged object";
    /* limit - size, not ref + size: size_t is 32 bits on the Amiga, and a
     * raw word near 0xFFFFFFFF would wrap the sum past the check */
    if (ref > cl_heap.bump - sizeof(CL_Header)) return "past the bump front";
    if ((ref & (CL_ALIGN - 1)) != 0) return "unaligned";
    if (HV_BIT_GET(hv_free, ref)) return "a free block";
    if (!HV_BIT_GET(hv_starts, ref)) return "inside an object (no header there)";
    if (CL_HDR_TYPE(CL_OBJ_TO_PTR(ref)) > CL_TYPE_MAX) return "an impossible type tag";
    return NULL;
}

static void hv_check_ref(const char *holder, CL_Obj holder_obj, int idx, CL_Obj ref)
{
    const char *fault = hv_ref_fault(ref);
    char a[80], b[80];
    if (!fault) return;
    hv_faults++;
    if (holder_obj != CL_NIL)
        hv_say("%s %s slot %d -> 0x%08lx is %s (%s)\n", holder,
               cl_obj_brief(holder_obj, a, sizeof(a)), idx,
               (unsigned long)ref, fault, cl_obj_brief(ref, b, sizeof(b)));
    else
        hv_say("%s slot %d -> 0x%08lx is %s (%s)\n", holder, idx,
               (unsigned long)ref, fault, cl_obj_brief(ref, b, sizeof(b)));
}

int cl_gc_verify_heap(void)
{
    uint32_t off, units, bytes, walk_end, n_objs = 0, n_free = 0;
    int i;
    CL_Thread *t;

    hv_report_len = 0;
    hv_report[0] = '\0';
    hv_cut = 0;
    hv_faults = 0;

    /* Everything that is not free is live after this. */
    cl_gc();

    units = cl_heap.bump / CL_ALIGN + 1;
    bytes = (units + 7) / 8;
    hv_starts = (uint8_t *)platform_alloc(bytes);
    hv_free   = (uint8_t *)platform_alloc(bytes);
    if (!hv_starts || !hv_free) {
        if (hv_starts) platform_free(hv_starts);
        if (hv_free) platform_free(hv_free);
        hv_starts = hv_free = NULL;
        hv_say("heap-verify: no memory for the %lu-byte start bitmaps\n",
               (unsigned long)bytes);
        return -1;
    }
    memset(hv_starts, 0, bytes);
    memset(hv_free, 0, bytes);

    /* Pass 1: every block start; header sanity.  A header that breaks the
     * walk ends it there: walk_end is where the later walks stop too, since
     * past it there is no telling where a block starts. */
    walk_end = cl_heap.bump;
    for (off = CL_ALIGN; off < cl_heap.bump; ) {
        uint8_t *hp = cl_heap.arena + off;
        uint32_t size = CL_HDR_SIZE(hp);
        uint8_t type = CL_HDR_TYPE(hp);
        if (size == 0 || (size & (CL_ALIGN - 1)) != 0 || off + size > cl_heap.bump) {
            hv_faults++;
            hv_say("block @0x%08lx: header size %lu (type %u) breaks the arena walk "
                   "(bump 0x%08lx); walk stopped\n", (unsigned long)off,
                   (unsigned long)size, (unsigned)type, (unsigned long)cl_heap.bump);
            walk_end = off;
            break;
        }
        if (type > CL_TYPE_MAX) {
            hv_faults++;
            hv_say("block @0x%08lx: impossible type tag %u (size %lu)\n",
                   (unsigned long)off, (unsigned)type, (unsigned long)size);
        }
#ifdef CL_GENGC
        if (!gen_enabled && CL_HDR_MARKED(hp)) {   /* old space keeps its mark under GENGC */
#else
        if (CL_HDR_MARKED(hp)) {
#endif
            hv_faults++;
            hv_say("block @0x%08lx: mark bit still set after the collection (type %u)\n",
                   (unsigned long)off, (unsigned)type);
        }
        HV_BIT_SET(hv_starts, off);
        n_objs++;
        off += size;
    }
    /* The free list: those blocks hold no references. */
    for (off = cl_heap.free_list; off != 0; ) {
        CL_FreeBlock *fb;
        if (off >= cl_heap.bump || (off & (CL_ALIGN - 1)) != 0 || !HV_BIT_GET(hv_starts, off)) {
            hv_faults++;
            hv_say("free list: link 0x%08lx is not a block start; list walk stopped\n",
                   (unsigned long)off);
            break;
        }
        HV_BIT_SET(hv_free, off);
        n_free++;
        fb = (CL_FreeBlock *)(cl_heap.arena + off);
        off = fb->next_offset;
        if (n_free > n_objs) {
            hv_faults++;
            hv_say("free list: cycles\n");
            break;
        }
    }
    /* A bare size-only header (too small for a free-list link) holds no
     * object either: nothing live is smaller than a cons. */
    for (off = CL_ALIGN; off < walk_end; ) {
        uint8_t *hp = cl_heap.arena + off;
        uint32_t size = CL_HDR_SIZE(hp);
        if (size == 0 || off + size > walk_end) break;
        if (size < sizeof(CL_Cons) && !HV_BIT_GET(hv_free, off)) {
            HV_BIT_SET(hv_free, off);
            n_free++;
        }
        off += size;
    }

    /* Pass 2: every live object's children. */
    for (off = CL_ALIGN; off < walk_end; ) {
        uint8_t *hp = cl_heap.arena + off;
        uint32_t size = CL_HDR_SIZE(hp);
        uint8_t type = CL_HDR_TYPE(hp);
        CL_Obj self = (CL_Obj)off;
        int slot = 0;
        if (size == 0 || off + size > walk_end) break;
        if (HV_BIT_GET(hv_free, off) || type > CL_TYPE_MAX) { off += size; continue; }
        if (type == TYPE_BYTECODE) {
            CL_Bytecode *bc = (CL_Bytecode *)hp;
            if (!bc->code || bc->code_len == 0) {
                hv_faults++;
                hv_say("bytecode @0x%08lx: code %p len %lu\n", (unsigned long)off,
                       (void *)bc->code, (unsigned long)bc->code_len);
            }
            if (!bc->constants && bc->n_constants > 0) {
                hv_faults++;
                hv_say("bytecode @0x%08lx: %u constants but no pool\n",
                       (unsigned long)off, (unsigned)bc->n_constants);
                off += size; continue;
            }
        }
#define GC_VISIT(s) hv_check_ref(cl_type_name(self), self, slot++, (s))
#define GC_BYTECODE_TAIL(bc) ((void)(bc))
#define GC_STREAM_TAIL(st) ((void)(st))
        GC_WALK_OBJ_CHILDREN(hp, type);
#undef GC_VISIT
#undef GC_BYTECODE_TAIL
#undef GC_STREAM_TAIL
        off += size;
    }

    /* The roots outside the arena: registered statics, thread protect stacks. */
    for (i = 0; i < n_global_roots; i++) {
        const char *fault = hv_ref_fault(*global_roots[i]);
        if (fault) {
            char b[80];
            const char *site = "?";
            int line = 0;
#ifdef DEBUG_GC
            site = global_root_files[i] ? global_root_files[i] : "?";
            line = global_root_lines[i];
#endif
            hv_faults++;
            hv_say("static root #%d (%s:%d) -> 0x%08lx is %s (%s)\n", i, site, line,
                   (unsigned long)*global_roots[i], fault,
                   cl_obj_brief(*global_roots[i], b, sizeof(b)));
        }
    }
#undef gc_root_count
    for (t = cl_thread_list; t; t = t->next) {
        int j;
        for (j = 0; j < t->gc_root_count; j++) {
            const char *fault = hv_ref_fault(*t->gc_roots[j]);
            if (fault) {
                char b[80];
                hv_faults++;
                hv_say("thread %p protect #%d -> 0x%08lx is %s (%s)\n", (void *)t, j,
                       (unsigned long)*t->gc_roots[j], fault,
                       cl_obj_brief(*t->gc_roots[j], b, sizeof(b)));
            }
        }
        for (j = 0; j < t->vm.sp; j++) {
            const char *fault = hv_ref_fault(t->vm.stack[j]);
            if (fault) {
                char b[80];
                hv_faults++;
                hv_say("thread %p VM stack[%d] -> 0x%08lx is %s (%s)\n", (void *)t, j,
                       (unsigned long)t->vm.stack[j], fault,
                       cl_obj_brief(t->vm.stack[j], b, sizeof(b)));
            }
        }
    }

#define gc_root_count (CT->gc_root_count)
    hv_summary("heap-verify: %lu blocks (%lu free), bump 0x%08lx, %d fault(s)\n",
               (unsigned long)n_objs, (unsigned long)n_free,
               (unsigned long)cl_heap.bump, hv_faults);
    platform_free(hv_starts);
    platform_free(hv_free);
    hv_starts = hv_free = NULL;
    return hv_faults;
}

const char *cl_gc_verify_heap_report(void)
{
    return hv_report;
}
