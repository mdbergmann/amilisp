/* gc_jitscan.c — conservative scan of a thread's JIT native stack
 * (pins the objects it references for the compactor) and the
 * block-start index that bounds its "is this word an object
 * start?" validation.  State: gc_internal.h.
 */
#include "gc_internal.h"

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
uint32_t *gc_hdr_page   = NULL;
static uint32_t  gc_hdr_npages = 0;
#if defined(DEBUG_GC) || defined(DEBUG_GC_STRESS)
static void gc_jit_scan_verify_index(const uint32_t *candidates, int n_cand);
#endif
/* Next page boundary at or above cl_heap.bump: a bump allocation that ends
 * beyond it covers a page's first byte and must be noted.  0xFFFFFFFF
 * (never crossed) while the index is absent, so alloc_from_bump pays one
 * compare either way. */
uint32_t  gc_hdr_bump_watch = 0xFFFFFFFFu;


/* Record `off` as the block covering the first byte of every page whose
 * first byte lies in [off, off+size).  Assignment, not min/max: the caller
 * knows the block is the one covering those bytes right now. */
void gc_hdr_note(uint32_t off, uint32_t size)
{
    uint32_t first = (off + GC_HDR_PAGE - 1) >> GC_HDR_PAGE_SHIFT;
    uint32_t last  = (off + size - 1) >> GC_HDR_PAGE_SHIFT;
    if (!gc_hdr_page) return;
    if (last >= gc_hdr_npages) last = gc_hdr_npages - 1;
    while (first <= last)
        gc_hdr_page[first++] = off;
}

void gc_hdr_bump_reset(void)
{
    if (gc_hdr_page)
        gc_hdr_bump_watch = (cl_heap.bump + GC_HDR_PAGE - 1)
                            & ~(uint32_t)(GC_HDR_PAGE - 1);
    else
        gc_hdr_bump_watch = 0xFFFFFFFFu;
}

/* Slow path of alloc_from_bump: the block [old, old+size) just placed at
 * the bump front crossed gc_hdr_bump_watch. */
void gc_hdr_note_bump(uint32_t old, uint32_t size)
{
    gc_hdr_note(old, size);
    gc_hdr_bump_reset();
}

void gc_hdr_index_clear(void)
{
    uint32_t i;
    for (i = 0; i < gc_hdr_npages; i++)
        gc_hdr_page[i] = GC_HDR_NONE;
}

void gc_hdr_index_release(void)
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
void gc_hdr_index_init(void)
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

/* Persistent JIT-stack scan candidate buffer — defined here so
 * cl_mem_shutdown can free it; helpers and full doc live with
 * gc_scan_jit_native_stack below. */
uint32_t        *jit_scan_cand_buf = NULL;
int              jit_scan_cand_cap = 0;

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
uint32_t        *jit_pinned       = NULL;
int              jit_pinned_count = 0;
int              jit_pinned_cap   = 0;

/* Set when jit_pin_record could not grow jit_pinned[] (platform_alloc
 * OOM): the pinned set is incomplete, so cl_gc_compact MUST NOT move
 * anything this cycle — a live JIT frame's spilled C-stack word would
 * keep the old offset of a relocated object.  Reset by gc_mark. */
int              gc_jit_pin_oom   = 0;

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
uint32_t        *jit_scan_free_snap  = NULL;
int              jit_scan_free_count = 0;
int              jit_scan_free_cap   = 0;
int              jit_scan_free_valid = 0;


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

int cand_cmp(const void *a, const void *b)
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

void gc_scan_jit_native_stack(CL_Thread *t)
{
    uint32_t *candidates;
    int n_cand = 0;
    int upper;
    int cand_cap;
    char *scan_lo;
    char *scan_hi;
    uintptr_t addr;
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
            GcWalk w;
            uint32_t offset, size;
            uint8_t type;
            gc_walk_begin(&w);
            while (gc_walk_next(&w, &offset, &size, &type)) {
                if (cand_bsearch(candidates, n_cand, offset) &&
                    !gc_offset_is_free_block(offset)) {
                    gc_mark_obj((CL_Obj)offset);
                    jit_pin_record(offset);
                }
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
void gc_hdr_index_selfcheck(void)
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
        GcWalk w;
        uint32_t o, size;
        uint8_t type;
        if (i > 0 && offset == candidates[i - 1]) continue;
        by_index = gc_hdr_is_block_start(offset, &cur);
        gc_walk_begin(&w);
        while (gc_walk_next(&w, &o, &size, &type)) {
            if (o == offset) { by_walk = 1; break; }
            if (o > offset) break;
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
