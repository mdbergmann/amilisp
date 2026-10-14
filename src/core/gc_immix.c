/* gc_immix.c — the Immix-style mark-region backend behind gc_backend.h
 * (specs/gc-backend-immix.md, Part 2).
 *
 * The arena is viewed as LINES (CL_GC_LINE_SIZE bytes, 128 by default)
 * with one side byte per line, im_line_map: a line whose byte equals the
 * current epoch im_epoch is UNAVAILABLE — touched by a live object in the
 * last trace, or claimed by an allocation cursor since — and any other
 * value means available.  A cycle is
 *
 *   1. mark   — the shared tracer (gc_trace.c); GCB_ON_MARK stamps every
 *               line of every marked object with the cycle's epoch (exact:
 *               the size is in the header) and counts the live bytes;
 *   2. finalize — the finalizable list (objects with external resources,
 *               listed at allocation) is walked: dead entries are
 *               finalized and their headers neutralised to CL_TYPE_DEAD;
 *   3. line sweep — one pass over the line map: lines the trace did not
 *               stamp are zeroed (available), the rest counted;
 *   4. the mark sense flips (gc_hdr_xor) so every survivor reads unmarked
 *               for the next trace without a clearing pass.
 *
 * Nothing in a cycle is proportional to dead space, and allocation is
 * always a bump: gcb_alloc_slow claims the next HOLE (a run of available
 * lines) as the thread's chunk cursor, formatted as one dead block, and
 * cl_alloc cuts objects from it lock-free (gcb_cursor_cut, gc_backend.h).
 * The shared sliding compactor (gc_compact.c) is the rare full
 * defragmentation — when no hole fits a request after a cycle, when the
 * line-level free space has fragmented badly, before an image save, and
 * under DEBUG_GC_STRESS — and it leaves the arena dense with the marks
 * normalised; the backend re-derives its tables from that state.
 *
 * Two invariants every claim maintains, because every other pass relies
 * on them:
 *
 *  - LINEAR WALKABILITY (mem.h, GcWalk): the header chain from CL_ALIGN to
 *    cl_heap.bump is exact at all times.  A hole's lines hold only dead
 *    blocks, but a dead object may START in the live line before the hole
 *    and extend into it (its header still claims the full size), and one
 *    may start inside the hole and extend past its end into the live line
 *    after it.  The claim TRUNCATES the first (its header stays, its size
 *    now ends at the hole start) and SPLITS the second (a dead header is
 *    written at the hole's end for its tail), then formats the hole as one
 *    dead block.  A live object never touches an available line (its lines
 *    were all stamped), so both operations only ever rewrite dead space.
 *    A hole above the bump front is preceded by a dead filler block so the
 *    chain stays contiguous.  There is therefore no clipping rule in the
 *    iterator: the chain is the truth, and the spec's "walkability under
 *    holes" is implemented here, at the claim, instead of in every walk.
 *
 *  - THE BLOCK-START INDEX (gc_jitscan.c): every entry of a page below the
 *    bump front names a header on the chain at or below the page's first
 *    byte.  The classic sweep rebuilds the index every cycle; this backend
 *    never walks, so the index persists across cycles and a claim must not
 *    leave an entry naming a header it erased.  Pages whose first byte lies
 *    in the hole are re-noted to the hole's header; pages after the hole
 *    whose entry names a header INSIDE it can only come from an earlier
 *    chunk claim that started inside this hole (the only noted blocks that
 *    get cut into un-noted pieces), so they lie within one chunk length of
 *    the hole's end and gc_hdr_repoint re-points exactly that range.  The
 *    DEBUG builds audit the whole index after every claim (DEBUG_GC) and
 *    every collection (DEBUG_GC / DEBUG_GC_STRESS).
 *
 * Dead blocks carry CL_TYPE_DEAD (types.h), never the raw-size free-block
 * header of the classic backend: a conservative JIT-stack candidate or an
 * overflow-rescan child that lands on one is refused outright, and
 * gc_finalize_dead ignores it, so a corpse is finalized exactly once.
 *
 * Multi-threading: a claim takes alloc_mutex, a cut does not; every cursor
 * is dropped at the top of every collection (gc_cursor_drop_all, mem.c).
 * Single-threaded, the same chunk cursor is the main thread's and no lock
 * is taken anywhere.
 */
#include "gc_internal.h"

#ifndef CL_GC_IMMIX
#error "gc_immix.c is the CL_GC_IMMIX backend; build it only with GC=immix"
#endif

/* ---- State (declared in gc_backend.h where the inlines need it) ------ */

uint32_t gc_hdr_xor = 0;            /* mark sense: 0 or CL_HDR_MARK_BIT */
uint8_t *im_line_map = NULL;        /* one byte per line */
uint8_t  im_epoch = 1;              /* current epoch, 1..255 */
uint32_t im_marked_bytes = 0;       /* live bytes found by the last trace */

static uint32_t im_nlines = 0;      /* whole lines in the arena */
static uint32_t im_scan = 1;        /* the chunk cursor's scan position */
static uint32_t im_chunk_lines = 0; /* lines per chunk claim, at most */
static int      im_normalised = 1;  /* marks exact: after compact / adopt */
static int      im_want_compact = 0;/* policy: compact before the next refill */

/* Finalizable objects (bytecode, streams, foreign pointers, thread
 * wrappers): offsets, listed at allocation, dropped when finalized. */
static uint32_t *im_fin = NULL;
static uint32_t  im_fin_count = 0, im_fin_cap = 0;
static int       im_fin_incomplete = 0;   /* a note failed: compact to catch up */

/* Diagnostics (gcb_stats_report). */
static uint32_t im_live_lines = 0, im_free_lines = 0, im_largest_run = 0;
static uint32_t im_cycles = 0, im_policy_compactions = 0;
static uint32_t im_holes_claimed = 0;

#define IM_LINE_SHIFT CL_GC_LINE_SHIFT
#define IM_LINE       CL_GC_LINE_SIZE
#define IM_PAGE_SHIFT GC_HDR_PAGE_SHIFT

/* A chunk claim takes at most this many lines (32 KB with 128-byte lines);
 * scaled down on small arenas so N threads' chunks cannot eat a test-sized
 * heap.  Requests above a chunk are placed directly in a run of their own. */
#ifndef CL_IMMIX_CHUNK_LINES
#define CL_IMMIX_CHUNK_LINES 256u
#endif
#define IM_CHUNK_MIN_LINES 4u

/* The largest run a request may need: one dead block must describe it. */
#define IM_MAX_RUN_LINES (CL_HDR_SIZE_MASK >> IM_LINE_SHIFT)

static void im_next_epoch(void)
{
    im_epoch = (uint8_t)(im_epoch == 255 ? 1 : im_epoch + 1);
}

/* No generational mode here (mem.h keeps the call sites unconditional). */
int cl_gengc_enabled(void) { return 0; }

int im_type_finalizable(uint8_t type)
{
    return type == TYPE_BYTECODE || type == TYPE_STREAM ||
           type == TYPE_FOREIGN_POINTER || type == TYPE_THREAD;
}

/* ================================================================
 * The finalizable list
 * ================================================================ */

static int im_fin_append(uint32_t off)
{
    if (im_fin_count == im_fin_cap) {
        uint32_t ncap = im_fin_cap ? im_fin_cap * 2 : 1024;
        uint32_t *nf = (uint32_t *)platform_alloc(ncap * sizeof(uint32_t));
        if (!nf)
            return 0;
        if (im_fin) {
            memcpy(nf, im_fin, im_fin_count * sizeof(uint32_t));
            platform_free(im_fin);
        }
        im_fin = nf;
        im_fin_cap = ncap;
    }
    im_fin[im_fin_count++] = off;
    return 1;
}

/* cl_alloc just allocated a finalizable object at `off` (no lock held;
 * these are rare types, so taking alloc_mutex here costs nothing).  A
 * failed note degrades gracefully: the object's resources are released by
 * the next compaction instead, which the policy schedules. */
void im_fin_note(uint32_t off, int need_lock)
{
    if (need_lock) platform_mutex_lock(alloc_mutex);
    if (!im_fin_append(off))
        im_fin_incomplete = 1;
    if (need_lock) platform_mutex_unlock(alloc_mutex);
}

/* After the trace: finalize every listed object the trace did not mark
 * and neutralise its header, keep the rest.  World stopped. */
static void im_finalize_dead(void)
{
    uint32_t i, kept = 0;
    for (i = 0; i < im_fin_count; i++) {
        uint32_t off = im_fin[i];
        uint8_t *ptr = cl_heap.arena + off;
        if (CL_HDR_TYPE(ptr) == CL_TYPE_DEAD)
            continue;                       /* neutralised by a claim */
        if (GCB_HDR_MARKED(ptr)) {
            im_fin[kept++] = off;
            continue;
        }
        gc_finalize_dead(ptr);
        CL_HDR(ptr) = (CL_TYPE_DEAD << CL_HDR_TYPE_SHIFT) | CL_HDR_SIZE(ptr);
    }
    im_fin_count = kept;
}

/* Rebuild the list from a dense arena (after a slide or an image
 * adoption, when every listed offset is stale).  One chain walk; the
 * caller is already walking the arena for other reasons. */
static void im_fin_rebuild(void)
{
    GcWalk w;
    uint32_t off, size;
    uint8_t type;
    im_fin_count = 0;
    im_fin_incomplete = 0;
    gc_walk_begin(&w);
    while (gc_walk_next(&w, &off, &size, &type)) {
        if (type != CL_TYPE_DEAD && im_type_finalizable(type) &&
            !im_fin_append(off))
            im_fin_incomplete = 1;
    }
}

/* ================================================================
 * The line map
 * ================================================================ */

/* Stamp lines [l, l+n) unavailable. */
static void im_stamp(uint32_t l, uint32_t n)
{
    memset(im_line_map + l, im_epoch, n);
}

/* After a slide or an adoption: every line below the bump front is in
 * use, every line above is free, and the cursor scans from the front. */
static void im_map_from_dense(void)
{
    uint32_t used = (cl_heap.bump + IM_LINE - 1) >> IM_LINE_SHIFT;
    if (used > im_nlines) used = im_nlines;
    im_stamp(0, used);
    memset(im_line_map + used, 0, im_nlines - used);
    im_scan = used;
    im_live_lines = used;
    im_free_lines = im_nlines - used;
    im_largest_run = im_free_lines;
}

/* The line sweep: zero every line the trace did not stamp, count the
 * rest, and find the largest run of available lines.  O(lines). */
static void im_sweep_lines(void)
{
    uint32_t l, live = 0, run = 0, largest = 0;
    uint8_t e = im_epoch;
    im_line_map[0] = e;                 /* NIL's line is never available */
    for (l = 1; l < im_nlines; l++) {
        if (im_line_map[l] == e) {
            live++;
            if (run > largest) largest = run;
            run = 0;
        } else {
            im_line_map[l] = 0;
            run++;
        }
    }
    if (run > largest) largest = run;
    im_live_lines = live + 1;
    im_free_lines = im_nlines - im_live_lines;
    im_largest_run = largest;
    im_scan = 1;
}

/* First available line at or after `from`, or 0 (line 0 is never
 * available).  *len receives the run's length, counted up to `cap`. */
static uint32_t im_first_run(uint32_t from, uint32_t cap, uint32_t *len)
{
    uint8_t e = im_epoch;
    uint32_t l = from, n;
    while (l < im_nlines && im_line_map[l] == e)
        l++;
    if (l >= im_nlines)
        return 0;
    for (n = 1; n < cap && l + n < im_nlines && im_line_map[l + n] != e; n++)
        ;
    *len = n;
    return l;
}

/* First run of at least `need` available lines at or after `from`, its
 * length counted up to `cap` (>= need).  0 when there is none. */
static uint32_t im_find_run(uint32_t from, uint32_t need, uint32_t cap,
                            uint32_t *len)
{
    for (;;) {
        uint32_t l = im_first_run(from, cap, len);
        if (!l)
            return 0;
        if (*len >= need)
            return l;
        from = l + *len;
    }
}

/* ================================================================
 * Claiming a hole — the chain and the index stay exact
 * ================================================================ */

/* The block covering arena offset `x` (< cl_heap.bump), and its end.
 * Bounded by the block-start index to about a page of headers; the
 * chain is exact, so the walk cannot miss. */
static uint32_t im_block_covering(uint32_t x, uint32_t *end)
{
    uint32_t b = gc_hdr_walk_start_at(x);
    for (;;) {
        uint32_t size = CL_HDR_SIZE(cl_heap.arena + b);
        if (size == 0) {
            /* A broken chain: nothing below this is trustworthy.  The
             * debug builds say so; a release build leaves the chain as it
             * is (the next walk truncates there as it always did). */
            gc_walk_broken(b, CL_HDR(cl_heap.arena + b), cl_heap.bump);
            *end = x;
            return x;
        }
        if (b + size > x) {
            *end = b + size;
            return b;
        }
        b += size;
    }
}

#if defined(DEBUG_GC) || defined(DEBUG_GC_STRESS)
/* The last claims, for a [GC-BADMARK] that names a dead block: which
 * claim formatted it, in which collection epoch, and what header the
 * claim found at its start (the object a stale reference still names). */
#define IM_DBG_CLAIMS 64
static struct { uint32_t start, end, old_hdr, gc; } im_dbg_claims[IM_DBG_CLAIMS];
static uint32_t im_dbg_nclaims = 0;

static void im_dbg_note_claim(uint32_t start, uint32_t end)
{
    uint32_t i = im_dbg_nclaims++ % IM_DBG_CLAIMS;
    im_dbg_claims[i].start = start;
    im_dbg_claims[i].end = end;
    im_dbg_claims[i].old_hdr = start < cl_heap.bump ? CL_HDR(cl_heap.arena + start) : 0;
    im_dbg_claims[i].gc = cl_heap.gc_count;
}

#ifdef DEBUG_GC_STRESS
static long im_stress_k = -2, im_stress_n = 0;
#endif

void im_debug_badmark(uint32_t off)
{
    uint32_t n = im_dbg_nclaims < IM_DBG_CLAIMS ? im_dbg_nclaims : IM_DBG_CLAIMS;
    uint32_t k;
    fprintf(stderr, "  immix: epoch %u, gc #%lu, line %lu byte %u, %lu claims so far\n",
            (unsigned)im_epoch, (unsigned long)cl_heap.gc_count,
            (unsigned long)(off >> IM_LINE_SHIFT),
            (unsigned)im_line_map[off >> IM_LINE_SHIFT],
            (unsigned long)im_dbg_nclaims);
#ifdef DEBUG_GC_STRESS
    if (im_stress_k > 1)
        fprintf(stderr, "  immix: stressed allocation #%ld (every %ldth compacts; "
                "CLAMIGA_GC_STRESS_AT=<index> names the code there)\n",
                im_stress_n, im_stress_k);
#endif
    for (k = 0; k < n; k++) {
        uint32_t i = (im_dbg_nclaims - 1 - k) % IM_DBG_CLAIMS;
        if (off >= im_dbg_claims[i].start && off < im_dbg_claims[i].end)
            fprintf(stderr, "  claim #%lu (gc #%lu): [0x%08lx, 0x%08lx) found header "
                    "0x%08lx (type %u size %lu) at its start\n",
                    (unsigned long)(im_dbg_nclaims - k),
                    (unsigned long)im_dbg_claims[i].gc,
                    (unsigned long)im_dbg_claims[i].start,
                    (unsigned long)im_dbg_claims[i].end,
                    (unsigned long)im_dbg_claims[i].old_hdr,
                    (unsigned)((im_dbg_claims[i].old_hdr >> CL_HDR_TYPE_SHIFT) & 0xFF),
                    (unsigned long)(im_dbg_claims[i].old_hdr & CL_HDR_SIZE_MASK));
    }
}

static void im_check_available(uint32_t l, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (im_line_map[l + i] == im_epoch) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "FATAL: Immix claim of lines %lu+%lu: line %lu is not "
                     "available (epoch %u)\n", (unsigned long)l,
                     (unsigned long)n, (unsigned long)(l + i),
                     (unsigned)im_epoch);
            platform_write_string(buf);
            platform_flush_output();
            abort();
        }
    }
}
#endif

/* Claim lines [l, l+n) as one dead block: make the chain contiguous
 * around it (truncation, split), format it, note the index, stamp the
 * lines, account the bytes.  Returns the block's offset — the first
 * line's start, or the old bump front when the lines lie above it (the
 * block then reaches from the front to the last line's end).  Caller
 * holds alloc_mutex when multi-threaded. */
static uint32_t im_claim(uint32_t l, uint32_t n)
{
    uint32_t start = l << IM_LINE_SHIFT;
    uint32_t end = (l + n) << IM_LINE_SHIFT;
    uint32_t bump = cl_heap.bump;

#if defined(DEBUG_GC) || defined(DEBUG_GC_STRESS)
    im_check_available(l, n);
#endif

    if (start >= bump) {
        /* Virgin space: the claim begins at the chain's end itself rather
         * than at the line boundary above it, so the first object of the
         * claim sits where the classic bump allocator would have put it —
         * no dead filler, and nothing for the next compaction to slide it
         * over (the bytes before `start` lie in a line that is already
         * stamped, as the chain's last object lives there). */
        start = bump;
    } else {
        uint32_t b, b_end;
        /* The split first, while the chain through the hole is intact: a
         * dead block starting inside [start, end) — or the spanner itself —
         * that extends past `end` gets a header for its tail there. */
        if (end < bump) {
            b = im_block_covering(end, &b_end);
            if (b < end) {
                gcb_format_dead(end, b_end - end);
                gc_hdr_note(end, b_end - end);
            }
        }
        /* The spanner: the block covering the byte before the hole, cut
         * short at the hole.  It is dead (it touches an available line),
         * so its header is the only thing to rewrite. */
        b = im_block_covering(start - 1, &b_end);
        if (b_end > start)
            CL_HDR(cl_heap.arena + b) =
                (CL_TYPE_DEAD << CL_HDR_TYPE_SHIFT) | (start - b);
    }

#if defined(DEBUG_GC) || defined(DEBUG_GC_STRESS)
    im_dbg_note_claim(start, end);
#endif
    gcb_format_dead(start, end - start);
    gc_hdr_note(start, end - start);
    /* Entries of the pages after the hole that still name a header the
     * format above erased (see the header comment): within one chunk
     * length of `end`, re-pointed at the header there — or at nothing when
     * the chain ends at `end`. */
    gc_hdr_repoint(end >> IM_PAGE_SHIFT,
                   (end + (im_chunk_lines << IM_LINE_SHIFT)) >> IM_PAGE_SHIFT,
                   start, end, end < bump ? end : GC_HDR_NONE);
    if (end > bump) {
        cl_heap.bump = end;
        gc_hdr_bump_reset();
    }
    im_stamp(l, n);
    cl_heap.total_allocated += end - start;
    im_holes_claimed++;
    cl_heap.tlab_refills++;

#ifdef DEBUG_GC
    /* Every claim rewrites the chain and the index; audit both at once
     * (O(arena) per claim — DEBUG_GC is the slow build by definition). */
    if (gc_hdr_page) {
        int v = cl_gc_audit_hdr_index();
        if (v != 0) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "FATAL: Immix claim of 0x%08lx+%lu left %d bad "
                     "block-start index page(s)\n", (unsigned long)start,
                     (unsigned long)(end - start), v);
            platform_write_string(buf);
            platform_flush_output();
            abort();
        }
    }
#endif
    return start;
}

/* ================================================================
 * Allocation: the refill behind cl_alloc's cursor cut
 * ================================================================ */

/* Serve `*sizep` bytes: refill t's chunk from the next hole and cut, or
 * place a request larger than a chunk in a run of its own.  NULL when no
 * run of available lines fits (the caller collects).  Caller holds
 * alloc_mutex when multi-threaded. */
static void *im_refill(CL_Thread *t, uint32_t *sizep, int multi)
{
    uint32_t size = *sizep;
    uint32_t need = (size + IM_LINE - 1) >> IM_LINE_SHIFT;
    uint32_t l, len, start, end;

    if (need > im_chunk_lines) {
        /* Large: its own run, exactly as many lines as it needs; the tail
         * of the last line stays a dead block (under a line — not worth a
         * cursor, and keeping it out of the cursor bounds the index
         * re-pointing above to one chunk length). */
        uint32_t rem;
        l = im_find_run(im_scan, need, need, &len);
        if (!l)
            return NULL;
        start = im_claim(l, need);
        end = (l + need) << IM_LINE_SHIFT;
        rem = end - start - size;
        if (rem < CL_MIN_ALLOC_SIZE) {
            size += rem;                    /* absorb the tail */
            *sizep = size;
        } else {
            gcb_format_dead(start + size, rem);
        }
        CL_HDR(cl_heap.arena + start) = (CL_TYPE_DEAD << CL_HDR_TYPE_SHIFT) | size;
        cl_heap.total_consed += size;
        return cl_heap.arena + start;
    }

    /* A chunk: the next hole in scan order, if the request fits it — a
     * hole too small for this request stays available for the next
     * (smaller) one, and the request takes the first run ahead that fits. */
    l = im_first_run(im_scan, im_chunk_lines, &len);
    if (!l)
        return NULL;
    if (len >= need) {
        im_scan = l + len;
    } else {
        l = im_find_run(l + len, need, im_chunk_lines, &len);
        if (!l)
            return NULL;
    }
    gc_cursor_drop(t);                      /* flush the old chunk's accounting */
    start = im_claim(l, len);
    end = (l + len) << IM_LINE_SHIFT;
    t->alloc_cur = start;
    t->alloc_lim = end;
    return gcb_cursor_cut(t, sizep, end - start, multi);
}

static int im_collect(uint32_t seen_gc_count);

/* Compaction gate shared by the policy and the last resort: at most once
 * per collection epoch. */
static int im_compact_now(void)
{
    if (gc_last_compact_cycle == cl_heap.gc_count)
        return 0;
    gc_last_compact_cycle = cl_heap.gc_count;
    cl_gc_compact();
    return 1;
}

CL_NOINLINE void *gcb_alloc_slow(CL_Thread *t, uint32_t *sizep, int multi)
{
    void *ptr;
    int stage = 0;

    if (((*sizep + IM_LINE - 1) >> IM_LINE_SHIFT) > IM_MAX_RUN_LINES)
        cl_storage_error("Allocation too large for the arena: %u bytes",
                         (unsigned)*sizep);

    if (multi) platform_mutex_lock(alloc_mutex);
    for (;;) {
        ptr = im_refill(t, sizep, multi);
        if (ptr)
            break;
        if (stage == 0) {
            /* No hole fits: a cycle (deduped against a peer's, like the
             * classic cl_gc_if_stale — a skipped cycle is retried against
             * the peer's result and, on a second miss, run for real),
             * then the compaction the cycle's policy may have asked for. */
            uint32_t seen = cl_heap.gc_count;
            if (multi) platform_mutex_unlock(alloc_mutex);
            if (!im_collect(seen)) {
                if (multi) platform_mutex_lock(alloc_mutex);
                ptr = im_refill(t, sizep, multi);
                if (ptr) break;
                if (multi) platform_mutex_unlock(alloc_mutex);
                im_collect(cl_heap.gc_count);
            }
            if (im_want_compact && !(gc_fwd_fail_bump &&
                                     cl_heap.bump >= gc_fwd_fail_bump)) {
                im_want_compact = 0;
                if (im_compact_now())
                    im_policy_compactions++;
            }
            if (multi) platform_mutex_lock(alloc_mutex);
            stage = 1;
            continue;
        }
        if (stage == 1) {
            /* Still no run: fragmentation, or line-level waste — the
             * compactor is the last resort, tried once per cycle. */
            int ran;
            if (multi) platform_mutex_unlock(alloc_mutex);
            ran = im_compact_now();
            if (multi) platform_mutex_lock(alloc_mutex);
            stage = 2;
            if (ran)
                continue;
        }
        if (multi) platform_mutex_unlock(alloc_mutex);
        cl_storage_error("Heap exhausted (requested %u bytes)",
                         (unsigned)*sizep);
    }
    if (multi) platform_mutex_unlock(alloc_mutex);
    return ptr;
}

/* ================================================================
 * A cycle
 * ================================================================ */

/* The policy, after a cycle.  Compaction is worthwhile only when the
 * object-level live set leaves room (under 7/8 of the arena, the classic
 * rule); it is wanted when the holes hold less than half of the true free
 * space (line-level fragmentation: dead objects sharing lines with live
 * ones), when the holes are under a sixteenth of the arena (a cycle per
 * few allocations otherwise), or when a finalizable note was lost. */
static void im_policy_after_cycle(void)
{
    uint32_t arena = cl_heap.arena_size;
    uint32_t free_b = im_free_lines << IM_LINE_SHIFT;
    uint32_t true_free = arena > im_marked_bytes ? arena - im_marked_bytes : 0;
    int worthwhile = im_marked_bytes < arena - (arena >> 3);
    im_want_compact = 0;
    if (im_fin_incomplete) {
        im_want_compact = 1;
        return;
    }
    if (worthwhile && (free_b < true_free / 2 || free_b < arena >> 4))
        im_want_compact = 1;
}

/* The collection, with the world stopped.  seen_gc_count carries the
 * allocation-path dedup: multi-threaded, a cycle that a peer already ran
 * since the caller observed exhaustion is skipped (returns 0). */
static int im_collect(uint32_t seen_gc_count)
{
    int multithread = (cl_thread_count > 1);
    uint64_t t0 = gc_stop_world_timed(multithread), t1;
    uint64_t diag_t0 = t0;

    if (multithread && cl_heap.gc_count != seen_gc_count) {
        gc_epoch_skips++;
        cl_gc_resume_the_world();
        return 0;
    }

    gc_cursor_drop_all();

#ifdef DEBUG_GC
    {
        char buf[96];
        snprintf(buf, sizeof(buf), "GC #%lu (immix): marking...\n",
                 (unsigned long)(cl_heap.gc_count + 1));
        platform_write_string(buf);
    }
#endif
    im_next_epoch();
    im_marked_bytes = 0;
    cl_stream_outbuf_gc_mark_begin();
    gc_mark();
    cl_stream_outbuf_gc_reclaim();
    gc_srcloc_invalidate_dead();
    t1 = platform_time_us();
    gc_time_mark_us += t1 - t0;
    t0 = t1;
#ifdef DEBUG_GC
    gc_verify_marked();                 /* a no-op unless the marks are exact */
    if (gc_verify_errors > 0)
        gc_dump_roots_dbg();
#endif

    im_finalize_dead();
    im_sweep_lines();
    gc_hdr_xor ^= CL_HDR_MARK_BIT;      /* survivors read unmarked from here */
    im_normalised = 0;
    cl_heap.total_allocated = im_marked_bytes;
    cl_heap.gc_count++;
    im_cycles++;
    im_policy_after_cycle();
    gc_time_sweep_us += platform_time_us() - t0;
#if defined(DEBUG_GC) || defined(DEBUG_GC_STRESS)
    gc_hdr_index_selfcheck();
#endif
    if (gc_diag_on())
        gc_diag_report("immix", platform_time_us() - diag_t0);
#ifdef DEBUG_GC
    {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "GC #%lu done: %lu live bytes, %lu/%lu lines free\n",
                 (unsigned long)cl_heap.gc_count,
                 (unsigned long)im_marked_bytes,
                 (unsigned long)im_free_lines, (unsigned long)im_nlines);
        platform_write_string(buf);
    }
#endif

    if (multithread)
        cl_gc_resume_the_world();
    return 1;
}

void gcb_collect(int why)
{
    (void)why;                          /* every reason is a cycle here */
    im_collect(cl_heap.gc_count);
}

/* ================================================================
 * The backend interface (gc_backend.h)
 * ================================================================ */

uint8_t *gcb_arena_alloc(uint32_t size)
{
    return (uint8_t *)platform_alloc(size);
}

void gcb_arena_free(uint8_t *arena, uint32_t size)
{
    (void)size;
    platform_free(arena);
}

static void im_release_tables(void)
{
    if (im_line_map) { platform_free(im_line_map); im_line_map = NULL; }
    if (im_fin) { platform_free(im_fin); im_fin = NULL; }
    im_fin_cap = im_fin_count = 0;
    im_nlines = 0;
}

void gcb_init(void)
{
    im_release_tables();                /* a previous heap's (re-init) */

    im_nlines = cl_heap.arena_size >> IM_LINE_SHIFT;
    if (im_nlines < 2) im_nlines = 2;
    im_line_map = (uint8_t *)platform_alloc(im_nlines);
    if (!im_line_map) {
        char buf[96];
        snprintf(buf, sizeof(buf),
                 "FATAL: cannot allocate the %lu-byte GC line map\n",
                 (unsigned long)im_nlines);
        platform_write_string(buf);
        platform_flush_output();
        exit(1);
    }
    im_epoch = 1;
    gc_hdr_xor = 0;
    im_marked_bytes = 0;
    im_normalised = 1;
    im_want_compact = 0;
    im_fin_incomplete = 0;
    im_cycles = im_policy_compactions = im_holes_claimed = 0;
    {
        uint32_t chunk = im_nlines / 64u;
        if (chunk > CL_IMMIX_CHUNK_LINES) chunk = CL_IMMIX_CHUNK_LINES;
        if (chunk < IM_CHUNK_MIN_LINES) chunk = IM_CHUNK_MIN_LINES;
        im_chunk_lines = chunk;
    }
    /* The block-start index bounds every claim's chain walk (and the
     * conservative scan's validation, as under classic).  Without it a
     * claim walks the chain from the start — correct, and slow. */
    gc_hdr_index_init();
    if (!gc_hdr_page) {
        static int warned = 0;
        if (!warned) {
            warned = 1;
            platform_write_string("clamiga: Immix GC without a block-start "
                                  "index - every hole claim walks the arena\n");
        }
    }
    memset(im_line_map, 0, im_nlines);
    im_map_from_dense();                /* bump == CL_ALIGN: line 0 in use */
}

void gcb_shutdown(void)
{
    im_release_tables();
    gc_hdr_index_release();
}

/* After a slide or an adoption: the arena is dense below the bump front,
 * every header's mark bit is clear, the free list may hold the gaps the
 * compactor left under JIT pins.  Re-derive everything from that. */
static void im_from_dense(void)
{
    uint32_t off;
    gc_hdr_xor = 0;
    im_normalised = 1;
    /* Pin gaps: dead blocks from here on, off the (unused) free list. */
    for (off = cl_heap.free_list; off; ) {
        CL_FreeBlock *fb = (CL_FreeBlock *)(cl_heap.arena + off);
        uint32_t next = fb->next_offset;
        CL_HDR(fb) = (CL_TYPE_DEAD << CL_HDR_TYPE_SHIFT) | (CL_HDR(fb) & CL_HDR_SIZE_MASK);
        off = next;
    }
    cl_heap.free_list = 0;
    im_map_from_dense();
    im_fin_rebuild();
    im_want_compact = 0;
}

void gcb_adopt_image_begin(void)
{
    im_epoch = 1;
    gc_hdr_xor = 0;
    im_marked_bytes = cl_heap.total_allocated;
    im_fin_count = 0;
    im_fin_incomplete = 0;
    if (gc_hdr_page)
        gc_hdr_index_clear();
    gc_hdr_bump_reset();
}

void gcb_adopt_image_finish(void)
{
    GcWalk w;
    uint32_t off, size;
    uint8_t type;
    /* The saving process left the marks clear (its writer compacted),
     * but normalise anyway — a classic saver's generational mode leaves
     * sticky marks set — and rebuild the block-start index. */
    gc_walk_begin(&w);
    while (gc_walk_next(&w, &off, &size, &type)) {
        CL_HDR(cl_heap.arena + off) &= ~CL_HDR_MARK_BIT;
        if (GC_HDR_CROSSES(off, size))
            gc_hdr_note(off, size);
    }
    im_from_dense();
}

void gcb_before_image_save(void)
{
    /* One compaction: dense [CL_ALIGN, bump), free_list == 0, marks
     * normalised and clear — what the writer and the loader expect. */
    cl_gc_compact();
}

void gcb_compact_begin(void)
{
    /* The compactor's trace must see the true live set: corpses the
     * line-sweep cycles left behind read marked by parity, so clear every
     * bit on the chain and reset the sense.  Skipped while the marks are
     * still exact (right after a compaction or an adoption — the
     * DEBUG_GC_STRESS regime, one compaction per allocation). */
    im_next_epoch();
    im_marked_bytes = 0;
    if (!im_normalised) {
        GcWalk w;
        uint32_t off, size;
        uint8_t type;
        gc_walk_begin(&w);
        while (gc_walk_next(&w, &off, &size, &type))
            CL_HDR(cl_heap.arena + off) &= ~CL_HDR_MARK_BIT;
        gc_hdr_xor = 0;
        im_normalised = 1;
    }
}

void gcb_compact_fallback(int outcome)
{
    /* The trace is complete and nothing moved: finish it as a plain cycle
     * (the marks were normalised in gcb_compact_begin, so the sense flip
     * leaves the survivors unmarked exactly as a cycle does). */
    (void)outcome;
    im_finalize_dead();
    im_sweep_lines();
    gc_hdr_xor ^= CL_HDR_MARK_BIT;
    im_normalised = 0;
    cl_heap.total_allocated = im_marked_bytes;
    im_cycles++;
    im_policy_after_cycle();
    im_want_compact = 0;                /* the thing that just failed */
}

void gcb_compact_done(void)
{
    im_from_dense();
}

void gcb_verify_collect(void)
{
    cl_gc_compact();
}

int gcb_marks_exact(void)
{
    return im_normalised;
}

#ifdef DEBUG_GC_STRESS
/* The second stress knob (CLAMIGA_GC_STRESS_IMMIX=K, with CLAMIGA_GC_STRESS
 * set): of every K stressed allocations, one runs the compaction and the
 * other K-1 run a line-sweep cycle, so the holes, the claims (truncation,
 * split, index re-pointing), the finalizable list and the sense flip get
 * the same per-allocation treatment the compactor gets.  Unset or 1: the
 * plain regime, a compaction before every allocation. */
/* (declared with the claim log above) */
void im_stress_collect(void)
{
    if (im_stress_k == -2) {
        const char *e = getenv("CLAMIGA_GC_STRESS_IMMIX");
        im_stress_k = e ? atol(e) : 0;
    }
    if (im_stress_k > 1 && (++im_stress_n % im_stress_k) != 0)
        im_collect(cl_heap.gc_count);
    else
        cl_gc_compact();
}
#endif

void gcb_stats_report(void (*out)(const char *))
{
    char buf[256];
    sprintf(buf, "Immix: %lu lines of %u bytes; after the last cycle %lu live, "
            "%lu free (largest hole %lu lines)\n",
            (unsigned long)im_nlines, (unsigned)IM_LINE,
            (unsigned long)im_live_lines, (unsigned long)im_free_lines,
            (unsigned long)im_largest_run);
    out(buf);
    sprintf(buf, "Immix: %lu holes claimed (chunks of up to %lu lines), %lu cycles, "
            "%lu policy compactions, %lu finalizable objects listed\n",
            (unsigned long)im_holes_claimed, (unsigned long)im_chunk_lines,
            (unsigned long)im_cycles, (unsigned long)im_policy_compactions,
            (unsigned long)im_fin_count);
    out(buf);
}
