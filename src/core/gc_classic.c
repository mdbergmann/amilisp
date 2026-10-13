/* gc_classic.c — the classic backend behind gc_backend.h: bump +
 * free-list allocation (gcb_alloc_slow and the chunk refill), gc_sweep
 * (free-list rebuild), the sweep-then-compact policy, the collection
 * entry points, image adoption, and the generational mode (CL_GENGC,
 * host only: nursery, dirty pages, minor collections).  Shared state:
 * gc_internal.h; the cursor cuts it refills: gc_backend.h.
 */
#include "gc_internal.h"

#ifndef CL_GC_CLASSIC
#error "gc_classic.c is the CL_GC_CLASSIC backend; build it only with GC=classic"
#endif

static void classic_gc(void);


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


int      gen_enabled = 0;   /* runtime switch (mmap+watch OK, env) */
uint32_t gen_old_top = CL_ALIGN;  /* old/nursery watermark */
volatile uint8_t *gen_dirty = NULL;   /* 1 bit per arena page */
uint32_t *gen_cross = NULL; /* per page: first object start offset */
uint32_t gen_page = 0;      /* platform page size */
uint32_t gen_npages = 0;    /* arena pages */
int      gen_protected = 0; /* old space currently read-only */
int      gen_arena_pages = 0; /* arena from platform_alloc_pages */

/* Diagnostics (ext:%gengc-stats) */
uint32_t gen_minor_count = 0;
uint64_t gc_time_minor_us = 0;
uint64_t gen_promoted_bytes = 0;
uint32_t gen_dirty_last = 0;

/* Rehash work list: post-slide offsets of hash tables encountered on
 * dirty pages / among survivors during a minor cycle.  Grown on demand;
 * survives across cycles (capacity only). */
uint32_t *gen_ht_list = NULL;
uint32_t gen_ht_count = 0, gen_ht_cap = 0;

/* Survivor list: offsets of live nursery objects, recorded by gc_mark_obj
 * while gen_minor_active — the whole point is that a minor cycle touches
 * only LIVE young objects, never walking the (dominant) dead nursery
 * space.  gen_live_new[] holds each survivor's assigned post-slide offset
 * (parallel array, filled after the list is sorted); gc_forward binary-
 * searches the pair during a minor instead of using a nursery-sized
 * forwarding table (which would cost an ~O(nursery) allocate+zero per
 * cycle).  Capacity persists across cycles. */
uint32_t *gen_live = NULL;
uint32_t *gen_live_new = NULL;
uint32_t gen_live_count = 0, gen_live_cap = 0;
int gen_minor_active = 0;   /* mark phase records survivors */
int gen_minor_fwd = 0;      /* gc_forward uses the survivor list */
int gen_live_oom = 0;       /* list growth failed — escalate */

/* Young finalizable objects (streams/locks/condvars/threads/bytecode/
 * foreign pointers), recorded at allocation time.  A minor never walks
 * dead nursery space, so without this list a dead young lock's platform
 * handle would leak until the next major.  Entries are dropped every
 * minor: dead ones are finalized, survivors are old afterwards (the next
 * major's full walk owns them). */
uint32_t *gen_fin = NULL;
uint32_t gen_fin_count = 0, gen_fin_cap = 0;

/* Types gc_finalize_dead releases external resources for. */
int gen_type_finalizable(uint8_t type)
{
    return type == TYPE_BYTECODE || type == TYPE_STREAM ||
           type == TYPE_FOREIGN_POINTER || type == TYPE_THREAD;
}

void gen_live_note(uint32_t off)
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
void gen_fin_note(uint32_t off, int need_lock)
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

/* Bytes of protectable pages behind cl_heap.arena while gen_arena_pages:
 * gen_reset() zeroes gen_npages/gen_page, so the release needs its own
 * record of what was mapped. */
static uint32_t gen_arena_bytes = 0;

/* Protect [0, gen_old_top) read-only (page-truncated).  The straddling
 * page (old tail + nursery head) stays writable — mark it dirty so the
 * next minor scans its old objects unconditionally. */
void gen_protect_old(void)
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
void gen_unprotect_all(void)
{
    if (gen_protected) {
        platform_page_protect(cl_heap.arena, gen_npages * gen_page, 0);
        gen_protected = 0;
    }
}

/* Record obj_offset as a (possibly) first object start in its page. */
void gen_cross_note(uint32_t obj_offset)
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
void gen_reset(void)
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
#else
int cl_gengc_enabled(void) { return 0; }
#endif /* CL_GENGC */

/* ================================================================
 * Allocation: the classic refill behind cl_alloc's cursor cut
 * (gcb_alloc_fast, gc_backend.h) — the bump front, then a bounded
 * first-fit probe of the free list, then collection and compaction
 * as the policy below dictates.
 * ================================================================ */

/* Fast-path bound on the first-fit free-list walk.  alloc_from_free_list is
 * O(list length); under a fragmented heap (many small blocks interspersed with
 * live objects) an allocation-heavy workload would degrade to O(n) per
 * allocation — O(n^2) overall — which manifests as a 100% CPU hang (e.g. the
 * bignum-heavy power-of-ten table built at FASL load time by jzon's
 * eisel-lemire).  The slow path probes only this many blocks; if no fit
 * turns up it compacts (which resets the bump pointer and clears
 * fragmentation, restoring the O(1) bump path) rather than walking the whole
 * list.  A full unbounded walk is still used as the last resort before
 * declaring the heap exhausted, so a fit deep in the list is never missed.
 * See tests/test_alloc_freelist_perf.c for the regression test. */
#ifndef CL_FREELIST_PROBE_LIMIT
#define CL_FREELIST_PROBE_LIMIT 32
#endif

/* Number of sweep-only GCs run since the last compaction.  A sweep never
 * resets the bump pointer, so once the bump is exhausted a run of sweeps
 * makes no lasting progress: each just re-coalesces the free list and the
 * next allocation sweeps again.  When this counter crosses
 * GC_SWEEPS_BEFORE_COMPACT (and the heap is not genuinely near-full) the
 * slow path forces a compaction, which resets the bump pointer and
 * defragments — escaping the "sweep-forever" regime.  Reset to 0 by the
 * compactor (gcb_compact_done / gcb_compact_fallback). */
uint32_t gc_sweeps_since_compact = 0;

#ifdef CL_TLAB
/* Chunk size: configured once per heap in gcb_init (0 = disabled).
 * CLAMIGA_TLAB_CHUNK=<bytes> overrides; scaled down on small heaps so
 * N threads' chunks can't consume a test-sized arena. */
#ifndef CL_TLAB_CHUNK
#define CL_TLAB_CHUNK (32u * 1024u)
#endif
#define CL_TLAB_MIN_CHUNK 4096u
static uint32_t tlab_chunk_size = 0;
static uint32_t tlab_max_obj    = 0;   /* larger objects never REFILL a chunk */
#endif

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

#ifdef CL_TLAB
/* Carve a fresh chunk for t and cut the first object from it.  Caller
 * MUST hold alloc_mutex.  Any abandoned remainder of the previous chunk
 * stays behind as a formatted hole (< tlab_max_obj bytes; the next sweep
 * reclaims it).  Falls back to the bounded free-list probe when the bump
 * front is exhausted, so chunks stay effective in the sweep-only regime
 * between compactions.  Returns NULL when neither source can supply a
 * chunk — the caller degrades to the shared slow path. */
static void *cursor_refill_chunk(CL_Thread *t, uint32_t *sizep)
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

    gc_cursor_drop(t);   /* flush the previous chunk's accounting */
    /* Chunk-granular: counts the whole chunk as allocated up front; the
     * next sweep recomputes the exact figure.  Errs toward "heap fuller
     * than it is", which only makes the compaction heuristics fire
     * earlier, never later. */
    cl_heap.total_allocated += chunk_size;
    cl_heap.tlab_refills++;

    gcb_cursor_format_hole(chunk_off, chunk_size);
    t->alloc_cur = chunk_off;
    t->alloc_lim = chunk_off + chunk_size;
    return gcb_cursor_cut_chunk(t, sizep, chunk_size);
}
#endif /* CL_TLAB */

/* The slow path (contract: gc_backend.h).  Multi-threaded, refill the
 * chunk (CL_TLAB) and cut; otherwise allocate directly — the front, then
 * the free list, collecting and compacting as the policy below dictates.
 * Accounts the block itself (cl_heap.total_allocated / total_consed,
 * under alloc_mutex when multi-threaded).  Out of line so cl_alloc's fast
 * path keeps its registers. */
CL_NOINLINE void *gcb_alloc_slow(CL_Thread *t, uint32_t *sizep, int multi)
{
    uint32_t size = *sizep;
    void *ptr = NULL;

    if (multi) platform_mutex_lock(alloc_mutex);

    if (!multi) {
        /* A chunk left over from the multi-threaded regime is never cut
         * again: flush its accounting now (its remainder stays a hole). */
        if (t->alloc_lim || t->alloc_consed)
            gc_cursor_drop(t);
    }
#ifdef CL_TLAB
    else if (size <= tlab_max_obj) {
        /* Objects above tlab_max_obj never refill: a large vector would
         * waste most of a chunk.  They are still CUT from a chunk with
         * room (the fast path), and otherwise allocated directly. */
        ptr = cursor_refill_chunk(t, sizep);
        if (ptr) {
            platform_mutex_unlock(alloc_mutex);
            return ptr;
        }
    }
#endif

    /* Direct allocation: the bump front (single-threaded it is exhausted,
     * the fast path just missed it; multi-threaded it is still worth a
     * try after a chunk refill found no room for a whole chunk), then a
     * *bounded* first-fit free-list probe.  The bound keeps a single
     * allocation cheap (O(CL_FREELIST_PROBE_LIMIT)) even when the free
     * list is long; if no fit turns up quickly we fall through to
     * GC/compaction below rather than walking the whole list. */
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
         * cl_gc_if_stale dedups the redundant-GC burst: if a peer
         * thread collected while we raced for stop-the-world initiator-ship,
         * it skips (returns 0) and we first retry the allocation against the
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
                classic_gc();
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

    /* (The header-size guard runs at cl_alloc's entry, BEFORE any bump
     * advance.  alloc_from_free_list can only ever shrink-fit or return
     * the exact block, never grow size past the mask.) */
    *sizep = size;
    cl_heap.total_allocated += size;
    cl_heap.total_consed += size;

    if (multi) platform_mutex_unlock(alloc_mutex);

    return ptr;
}

void gc_sweep(void)
{
    GcWalk w;
    uint32_t off, size;
    uint8_t type;
    uint8_t *end = cl_heap.arena + cl_heap.bump;

    cl_heap.free_list = 0;
    cl_heap.total_allocated = 0;

    gc_walk_begin(&w);
    while (gc_walk_next(&w, &off, &size, &type)) {
        uint8_t *ptr = cl_heap.arena + off;

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
            cl_heap.free_list = off;
#ifdef DEBUG_GC
            /* Poison free block data after the 8-byte header (size +
             * next_offset) to detect use-after-free. */
            if (total > sizeof(CL_FreeBlock))
                memset((uint8_t *)fb + sizeof(CL_FreeBlock), 0xDE,
                       total - sizeof(CL_FreeBlock));
#endif
            size = total;  /* the coalesced region is one block now */
            gc_walk_seek(&w, off + total);
        }
        /* Block-start index: coalescing just erased the headers inside a
         * free block, so every page this block covers is re-pointed at
         * it (live objects re-note themselves, a no-op in effect). */
        if (GC_HDR_CROSSES(off, size))
            gc_hdr_note(off, size);
    }
    gc_hdr_bump_reset();
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

    gc_cursor_drop_all();

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

/* Core of a sweep GC — runs with the world already stopped (or
 * single-threaded).  Split out so cl_gc_if_stale can decide AFTER winning
 * the stop-the-world race whether a collection is still needed. */
static void cl_gc_stopped(uint64_t t0)
{
    uint64_t t1;
    uint64_t diag_t_begin = t0;   /* CLAMIGA_GC_DIAG: pause = whole collection */

    /* Drop every thread's allocation cursor before any pass runs: gc_sweep
     * rebuilds the free list from unmarked regions, so an active chunk tail
     * (already a formatted hole) would otherwise be handed out twice — once
     * by the free list, once by the owning thread's next cut.  Runs even
     * when single-threaded: the front cursor must refill at the post-sweep
     * bump too, and chunk leftovers after a thread-count drop get flushed. */
    gc_cursor_drop_all();

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

/* The full (sweep) collection: what cl_gc() is under the classic backend. */
static void classic_gc(void)
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

/* Allocation-triggered GC with redundant-cycle dedup (contract: mem.h). */
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
     * allocation against the peer's sweep (it falls back to a real
     * collection if the retry still finds no fit). */
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

/* ================================================================
 * The backend interface (gc_backend.h)
 * ================================================================ */

uint8_t *gcb_arena_alloc(uint32_t size)
{
#ifdef CL_GENGC
    uint8_t *arena;
    const char *env;
    int want_gen;

    /* Reset any previous heap's generational state FIRST: protection must
     * be dropped and the fault handler detached before anything else
     * happens (a handler pointing at freed watch state would turn the
     * next unrelated crash into chaos).  Documented stale-static bug
     * class: every gen_* variable is re-derived for the new arena. */
    gen_reset();

    /* Page-aligned arena so old space can be mprotect-ed.  Fall back to
     * the plain allocator (and classic GC) if mmap is unavailable. */
    env = getenv("CLAMIGA_GENGC");
    want_gen = !(env && env[0] == '0' && env[1] == '\0');
    arena = NULL;
    if (want_gen) {
        /* Round the watch region up to whole pages; the arena itself
         * keeps the requested byte size. */
        gen_page = platform_page_size();
        gen_npages = (size + gen_page - 1) / gen_page;
        arena = (uint8_t *)platform_alloc_pages(gen_npages * gen_page);
        if (arena) {
            gen_dirty = (volatile uint8_t *)
                platform_alloc((gen_npages + 7) / 8);
            gen_cross = (uint32_t *)
                platform_alloc(gen_npages * sizeof(uint32_t));
            if (gen_dirty && gen_cross &&
                platform_write_watch_install(arena, gen_npages * gen_page,
                                             gen_dirty) == 0) {
                uint32_t i;
                for (i = 0; i < gen_npages; i++)
                    gen_cross[i] = GEN_CROSS_NONE;
                gen_arena_pages = 1;
                gen_arena_bytes = gen_npages * gen_page;
                gen_enabled = 1;
            } else {
                /* Partial setup failed — release and fall back. */
                if (gen_dirty) { platform_free((void *)gen_dirty); gen_dirty = NULL; }
                if (gen_cross) { platform_free(gen_cross); gen_cross = NULL; }
                platform_free_pages(arena, gen_npages * gen_page);
                arena = NULL;
                platform_write_string(
                    "clamiga: generational GC unavailable "
                    "(mmap/write-watch setup failed) - classic GC\n");
            }
        }
    }
    if (!arena) {
        gen_arena_pages = 0;
        gen_arena_bytes = 0;
        arena = (uint8_t *)platform_alloc(size);
    }
    return arena;
#else
    return (uint8_t *)platform_alloc(size);
#endif
}

void gcb_arena_free(uint8_t *arena, uint32_t size)
{
#ifdef CL_GENGC
    if (gen_arena_pages) {
        platform_free_pages(arena, gen_arena_bytes);
        gen_arena_pages = 0;
        gen_arena_bytes = 0;
        return;
    }
#endif
    (void)size;
    platform_free(arena);
}

void gcb_init(void)
{
    gc_sweeps_since_compact = 0;

    /* Block-start index for the JIT native-stack scan — the generational
     * mode keeps none (old space is dense, the crossing map serves).
     * Re-derived for every new arena. */
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
            uint32_t cap = cl_heap.arena_size / 64u;
            if (chunk > cap) chunk = cap;
            chunk &= ~(uint32_t)(CL_ALIGN - 1);
            if (chunk < CL_TLAB_MIN_CHUNK)
                chunk = 0;
        }
        tlab_chunk_size = chunk;
        tlab_max_obj = chunk / 8u;
    }
#endif /* CL_TLAB */
}

void gcb_shutdown(void)
{
#ifdef CL_GENGC
    gen_reset();   /* unprotect + detach handler before anyone touches a page */
#endif
    gc_hdr_index_release();
}

void gcb_collect(int why)
{
#ifdef CL_GENGC
    if (why == GCB_WHY_RECLAIM_YOUNG && gen_enabled) {
        /* A refused minor is fine here — the caller escalates to a full
         * cl_gc() when its retry still finds no free slot. */
        (void)cl_gc_minor(cl_heap.gc_count);
        return;
    }
#endif
    (void)why;
    classic_gc();
}

void gcb_adopt_image_begin(void)
{
    gc_sweeps_since_compact = 0;
    /* Block-start index: the payload replaces whatever the arena held;
     * gcb_adopt_image_finish's classic walk re-notes it. */
    if (gc_hdr_page)
        gc_hdr_index_clear();
    gc_hdr_bump_reset();
}

void gcb_adopt_image_finish(void)
{
    GcWalk w;
    uint32_t off, size;
    uint8_t type;

    gc_walk_begin(&w);
#ifdef CL_GENGC
    if (gen_enabled) {
        /* The whole payload becomes OLD space: sticky marks set (a minor
         * trace terminates at marked old objects), crossing map rebuilt
         * (page -> first object start), watermark moved to bump, dirty
         * bits cleared, old-space protection re-armed.  The survivor/
         * finalizable/rehash side lists are empty by construction (fresh
         * init, no nursery allocation has happened yet). */
        while (gc_walk_next(&w, &off, &size, &type)) {
            CL_HDR_SET_MARK(cl_heap.arena + off);
            gen_cross_note(off);
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
    while (gc_walk_next(&w, &off, &size, &type)) {
        CL_HDR_CLR_MARK(cl_heap.arena + off);
        if (GC_HDR_CROSSES(off, size))
            gc_hdr_note(off, size);
    }
}

void gcb_before_image_save(void)
{
    /* Full GC, then compaction: guarantees free_list == 0 and a dense
     * [CL_ALIGN, bump) payload (under gen mode both are compactions). */
    classic_gc();
    cl_gc_compact();
}

void gcb_compact_begin(void)
{
#ifdef CL_GENGC
    if (gen_enabled) {
        GcWalk w;
        uint32_t off, size;
        uint8_t type;
        /* The compactor's passes write mark bits and slide objects
         * everywhere — drop old-space protection for the duration. */
        gen_unprotect_all();
        /* Sticky marks would prune the FULL trace at the roots (gc_mark_obj
         * skips marked objects) — clear every mark bit first so this
         * collection sees the true live set.  One linear walk. */
        gc_walk_begin(&w);
        while (gc_walk_next(&w, &off, &size, &type))
            CL_HDR_CLR_MARK(cl_heap.arena + off);
    }
#endif
}

void gcb_compact_fallback(int outcome)
{
    /* The mark phase is complete and nothing has moved: a sweep rebuilds
     * the free list from it, exactly the non-moving cycle. */
    gc_sweep();
#ifdef CL_GENGC
    /* The sweep just rebuilt a free list and cleared survivor marks —
     * exactly the classic collector's state, and incompatible with the
     * generational invariants (dense old space, sticky marks).  Drop to
     * classic mode for the rest of the session. */
    if (gen_enabled) {
        gen_enabled = 0;
        platform_write_string(outcome == GCB_COMPACT_FALLBACK_FWD
                              ? "clamiga: generational GC disabled "
                                "(forwarding-table OOM) - classic GC\n"
                              : "clamiga: generational GC disabled "
                                "(compaction fallback) - classic GC\n");
    }
#else
    (void)outcome;
#endif
    /* Don't let the sweep-forever escape immediately re-attempt a doomed
     * compaction on every following allocation (the forwarding-OOM latch
     * in gc_compact.c gates the retries by bump level). */
    gc_sweeps_since_compact = 0;
}

void gcb_compact_done(void)
{
    /* The bump pointer was just reset to the end of the surviving objects,
     * so the sweep-forever escape counter starts fresh. */
    gc_sweeps_since_compact = 0;

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
}

void gcb_verify_collect(void)
{
    cl_gc();
}

int gcb_marks_exact(void)
{
    return 1;
}

void gcb_stats_report(void)
{
#ifdef CL_TLAB
    char buf[128];
    sprintf(buf, "TLAB: %lu-byte chunks, %lu refills\n",
            (unsigned long)tlab_chunk_size,
            (unsigned long)cl_heap.tlab_refills);
    platform_write_string(buf);
#endif
}
