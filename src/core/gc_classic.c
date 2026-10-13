/* gc_classic.c — the classic collector: gc_sweep (free-list
 * rebuild), cl_gc and its stop-the-world wrappers, and the
 * generational mode (CL_GENGC, host only: nursery, dirty pages,
 * minor collections).  State: gc_internal.h.
 */
#include "gc_internal.h"


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
#endif /* CL_GENGC */

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

/* Stop the world (multi-threaded), accounting the wait into the STW phase
 * timers, and return the timestamp at which the world was stopped. */
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
