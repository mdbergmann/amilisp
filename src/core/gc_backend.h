#ifndef CL_GC_BACKEND_H
#define CL_GC_BACKEND_H

/*
 * gc_backend.h — the seam between the heap core and the collector
 * (specs/gc-backend-immix.md, Part 1).
 *
 * The heap core (mem.c: the arena, cl_alloc's cursor cut, the
 * constructors, root stacks, the registered-root table, init/shutdown,
 * the public cl_gc* entry points) and the shared facilities (gc_trace.c
 * marking and root enumeration, gc_compact.c the sliding compactor,
 * gc_jitscan.c the conservative JIT scan, gc_verify.c) know the
 * collector only through this header.  Exactly one backend is compiled
 * into a binary — CL_GC_CLASSIC (gc_classic.c) or CL_GC_IMMIX
 * (gc_immix.c, Part 2) — selected in mem.h from the GC= build knob; the
 * hooks on the allocation and mark hot paths are macros / always-inline
 * functions so the classic build pays nothing for the seam.
 *
 * Includable from any runtime file (it needs only mem.h, thread.h and the
 * platform headers); gc_internal.h includes it for the collector's own
 * translation units.
 */

#include "mem.h"
#include "thread.h"
#include "../platform/platform.h"
#include "../platform/platform_thread.h"

/* ---- Function interface, implemented once per backend ----------------- */

/* Why gcb_collect was asked for a cycle.  Allocation-driven collection is
 * the backend's own business inside gcb_alloc_slow, and an explicit
 * compaction calls the shared compactor (cl_gc_compact) directly, so
 * only the two public "collect something" entry points go through here. */
#define GCB_WHY_EXPLICIT       1   /* cl_gc(): (ext:gc), tests, shutdown paths */
#define GCB_WHY_RECLAIM_YOUNG  2   /* cl_gc_reclaim_young(): a cheap cycle that
                                    * frees RECENT handle owners is enough */

/* How the shared compactor ended (gcb_compact_fallback / gcb_compact_done). */
#define GCB_COMPACT_FALLBACK_PIN  1   /* JIT pin-table OOM: moving is unsafe */
#define GCB_COMPACT_FALLBACK_FWD  2   /* forwarding-table OOM: cannot move  */

/* The arena.  The backend allocates it because the classic backend's
 * generational mode needs page-aligned, protectable pages; a backend with
 * no such need returns platform_alloc(size).  NULL = cannot (the heap core
 * reports the failure).  Called once per cl_mem_init; gcb_arena_free is
 * the exact inverse and runs AFTER gcb_shutdown. */
uint8_t *gcb_arena_alloc(uint32_t size);
void     gcb_arena_free(uint8_t *arena, uint32_t size);

/* Side tables and policy state for the arena cl_heap now describes
 * (cl_heap.arena / arena_size / bump are set).  Self-resetting: a unit
 * test re-inits the heap without a shutdown in between, so every static
 * the backend keeps is re-derived here (the documented stale-static bug
 * class).  gcb_shutdown hands every off-heap byte back (make
 * test-memleak) and runs before the arena is released — it must leave
 * the arena readable (drop any page protection) because the heap core
 * still walks it for the off-heap release afterwards. */
void gcb_init(void);
void gcb_shutdown(void);

/* The allocation slow path: the cursor could not serve *sizep bytes.
 * Refills the cursor and cuts, or allocates directly, collecting as the
 * backend's policy dictates.  Accounts the block (cl_heap.total_allocated
 * / total_consed), returns with no lock held, or raises a storage error.
 * *sizep may grow (an absorbed tail, an entire free block).  `multi` is
 * cl_thread_count > 1 as cl_alloc computed it. */
void *gcb_alloc_slow(struct CL_Thread_s *t, uint32_t *sizep, int multi);

/* A collection for one of the GCB_WHY_* reasons above. */
void gcb_collect(int why);

/* Heap-image adoption (image.c; specs/image-save-load.md).  begin() runs
 * right after the heap core installed the payload's bump and accounting
 * (the raw bytes are already in the arena); finish() runs after the
 * relink walk and normalises the backend's view of a dense
 * [CL_ALIGN, bump) in one linear pass.  gcb_before_image_save makes the
 * arena dense and normal for the writer (free_list == 0, marks as the
 * loader expects). */
void gcb_adopt_image_begin(void);
void gcb_adopt_image_finish(void);
void gcb_before_image_save(void);

/* The shared compactor's hooks.  begin() runs with the world stopped and
 * the cursors dropped, before the mark; fallback(outcome) replaces the
 * moving passes with the backend's non-moving collection when the
 * compactor cannot move (the mark phase is complete, nothing has moved);
 * done() runs after the slide, rehash and counters, with the arena dense
 * below the bump front and every survivor's mark bit still SET. */
void gcb_compact_begin(void);
void gcb_compact_fallback(int outcome);
void gcb_compact_done(void);

/* Backend lines of cl_mem_stats (ext:room). */
void gcb_stats_report(void);

/* ---- Hot hooks, per backend ------------------------------------------ */

#ifdef CL_GC_CLASSIC

/* Mark bits: the classic backend reads the header bit as is (the Immix
 * backend flips the bit's sense every cycle, specs/gc-backend-immix.md
 * "Marks without a clearing pass").  Only the collector's own files use
 * these so far; the raw CL_HDR_* forms outside gc_*.c move over with the
 * backend that needs it (Part 2, with a grep lint). */
#define GCB_HDR_MARKED(p)    CL_HDR_MARKED(p)
#define GCB_HDR_SET_MARK(p)  CL_HDR_SET_MARK(p)
#define GCB_HDR_CLR_MARK(p)  CL_HDR_CLR_MARK(p)

/* One object has just been marked (gc_mark_obj, exactly once per object
 * per cycle).  Classic: in a generational minor cycle, record every live
 * nursery object as it is discovered — the survivor list is what lets the
 * rest of the minor never touch dead nursery space.  (Immix: mark its
 * lines.)  Expands only in gc_trace.c, which has gc_internal.h. */
#ifdef CL_GENGC
#define GCB_ON_MARK(off, ptr) \
    do { if (gen_minor_active && (off) >= gen_old_top) gen_live_note(off); } while (0)
#else
#define GCB_ON_MARK(off, ptr) ((void)0)
#endif

/* A finalizable object (stream, lock, thread, bytecode, foreign pointer)
 * was just allocated at `off`.  Classic: young finalizable objects are
 * listed so a minor can release their external resources without walking
 * dead nursery space.  Rare types — the note takes alloc_mutex itself when
 * there is anyone to contend with.  Expands only in cl_alloc (mem.c). */
#ifdef CL_GENGC
#define GCB_ON_ALLOC_FINALIZABLE(off, type, multi) \
    do { if (gen_enabled && gen_type_finalizable(type)) \
             gen_fin_note((off), (multi)); } while (0)
#else
#define GCB_ON_ALLOC_FINALIZABLE(off, type, multi) ((void)0)
#endif

/* ---- The classic allocation cursor (cl_alloc's fast path) ----
 *
 * cl_alloc cuts objects from a cursor — a region [cur, lim) the current
 * thread owns outright — with no locking at all, and the backend refills
 * it in gcb_alloc_slow when a cut does not fit.  The classic backend has
 * two cursors, chosen by the regime:
 *
 *  Single-threaded: the bump front itself, [cl_heap.bump, arena_size).
 *    A cut IS the bump allocation; the front stays exact for every reader
 *    (the arena walks, the block-start index, the statistics, the image
 *    writer) and the accounting is per cut, as the bump allocator always
 *    did.  Deliberately NOT mirrored into the thread: a snapshot of the
 *    front in CL_Thread fields costs the thread-pointer load plus the
 *    dependent cursor loads on every allocation — measured at +8 % on
 *    alloc.cons-churn (bench-opt, host) — and reintroduces the two
 *    thread-count-transition hazards the live front does not have (a
 *    snapshot cut after a second thread registered hands out bytes a
 *    peer's chunk is carving; a stale chunk cut single-threaded would
 *    move the front backwards).  The one regime check is `multi`, which
 *    cl_alloc computes anyway.
 *
 *  Multi-threaded (CL_TLAB): a private CHUNK [alloc_cur, alloc_lim)
 *    (thread.h) carved from the front or the free list under alloc_mutex
 *    — the former TLAB.  With more than one thread registered every
 *    cl_alloc used to serialize on alloc_mutex; at actor-workload
 *    allocation rates the mutex (and its cache-line ping-pong) dominated
 *    the per-message cost.  Accounting is chunk-granular (the whole chunk
 *    counts as allocated when carved) with the consed bytes flushed at
 *    the next refill/drop, so a cut touches no shared counter.  Without
 *    CL_TLAB (Amiga) multi-threaded allocation takes the locked slow
 *    path directly.
 *
 * Invariants (all load-bearing — see gc_sweep/gc_compute_forwarding):
 *  - A chunk's uncut remainder [alloc_cur, alloc_lim) is formatted as a
 *    CL_FreeBlock at ALL times (size = remainder, not linked into the
 *    free list), so every linear arena walk parses the chunk.  A cut
 *    rewrites the remainder header BEFORE the object header/memset
 *    touches disjoint bytes below it.
 *  - Cuts never leave a remainder smaller than CL_MIN_ALLOC_SIZE: an
 *    undersized tail is absorbed into the object (same precedent as
 *    alloc_from_free_list's use-entire-block path) — at the arena's end
 *    too, so the front lands exactly on arena_size.
 *  - A chunk is only ever cut while multi-threaded.  Left over after the
 *    thread count dropped to one it is simply never cut again (the front
 *    is used), and the next collection or slow-path entry drops it; its
 *    remainder is already a hole.  The parent registers the newborn
 *    (builtins_thread.c), so the thread that flips the count to two is
 *    the sole thread; a thread that registers ITSELF while another runs
 *    must not do so concurrently with that thread's allocations — the
 *    rule the unlocked bump allocator always had.
 *  - Every GC cycle drops ALL threads' chunks during stop-the-world
 *    (gc_cursor_drop_all, mem.c).  Mandatory, not an optimization:
 *    gc_sweep rebuilds the free list from unmarked regions, so an active
 *    chunk tail would be handed to another thread while the owner keeps
 *    cutting.  Threads refill on their next allocation.
 *  - There is no safepoint between a cut and its header write, so a
 *    stop-the-world initiator can never walk a headerless cut (STW
 *    waits for this thread to park; it parks only at safepoints).
 *
 * The Immix backend has one cursor shape in both regimes — a hole,
 * private by construction — so for it the single-threaded cut is the
 * chunk cut with the main thread's fields.  The DEBUG_GC_STRESS suite is
 * single-threaded and never cuts a chunk; tests/test_gc_threaded.c's TLAB
 * section covers chunks under concurrent compaction, tests/test_gc_cursor.c
 * the front, the regime transitions and the accounting.
 *
 * Written here, once, because the cuts are instantiated twice: in
 * cl_alloc's fast path (mem.c) and in the chunk refill (gc_classic.c) —
 * and the m68k build is -Os without LTO, where a cross-file call would be
 * a call on every cons (CL_ALWAYS_INLINE, platform.h). */

/* Block-start index behind the conservative JIT scan (gc_jitscan.c): the
 * front cut notes the bump front once per 4 KB it crosses. */
extern uint32_t gc_hdr_bump_watch;
void gc_hdr_note_bump(uint32_t old, uint32_t size);

/* Format [off, off+len) as a dead, walkable region (len >= 8, aligned).
 * Not linked into the free list — the next sweep reclaims/coalesces it. */
CL_ALWAYS_INLINE void gcb_cursor_format_hole(uint32_t off, uint32_t len)
{
    CL_FreeBlock *fb = (CL_FreeBlock *)(cl_heap.arena + off);
    fb->size = len;
    fb->next_offset = 0;
}

/* Cut *sizep bytes from the bump front (the single-threaded cursor).  The
 * caller checked that it fits (`rem` = arena_size - bump >= size).  May
 * grow *sizep by up to CL_MIN_ALLOC_SIZE-CL_ALIGN (absorbed tail).
 * Accounts the object; single-threaded by construction, no lock. */
CL_ALWAYS_INLINE void *gcb_cursor_cut_front(uint32_t *sizep, uint32_t rem)
{
    uint32_t size = *sizep;
    uint32_t cur = cl_heap.bump;
    void *ptr = cl_heap.arena + cur;

    if (rem - size < CL_MIN_ALLOC_SIZE) {
        size = rem;                 /* absorb undersized tail */
        *sizep = size;
    }
    cur += size;
    cl_heap.bump = cur;
    cl_heap.total_allocated += size;
    cl_heap.total_consed += size;
    /* Block-start index: one compare per bump allocation; the slow path
     * runs once per 4 KB of bump-allocated data. */
    if (cur > gc_hdr_bump_watch)
        gc_hdr_note_bump(cur - size, size);
    return ptr;
}

#ifdef CL_TLAB
/* Cut *sizep bytes from t's CHUNK cursor; same contract as the front cut.
 * Owner-only, lock-free: the remainder is re-formatted as a hole, the
 * consed bytes are deferred to the next refill/drop. */
CL_ALWAYS_INLINE void *gcb_cursor_cut_chunk(CL_Thread *t, uint32_t *sizep,
                                            uint32_t rem)
{
    uint32_t size = *sizep;
    uint32_t cur = t->alloc_cur;
    void *ptr = cl_heap.arena + cur;

    if (rem - size < CL_MIN_ALLOC_SIZE) {
        size = rem;                 /* absorb undersized tail */
        *sizep = size;
    }
    cur += size;
    if (cur == t->alloc_lim)
        t->alloc_cur = t->alloc_lim = 0;
    else {
        t->alloc_cur = cur;
        gcb_cursor_format_hole(cur, t->alloc_lim - cur);
    }
    t->alloc_consed += size;
    return ptr;
}
#endif /* CL_TLAB */

/* cl_alloc's fast path: cut *sizep bytes from the regime's cursor — no
 * lock, no call.  Returns the cut, or NULL when the cursor is spent (or
 * there is no chunk), in which case the caller goes to gcb_alloc_slow with
 * the thread this stored in *tp.  (cl_get_current_thread's multi-threaded
 * leg is written out by hand: `multi` is already in a register, and the
 * m68k build at -Os would CALL the inline helper here.) */
CL_ALWAYS_INLINE void *gcb_alloc_fast(uint32_t *sizep, int multi,
                                      CL_Thread **tp)
{
    void *ptr = NULL;
    if (!multi) {
        uint32_t rem = cl_heap.arena_size - cl_heap.bump;
        if (*sizep <= rem)
            ptr = gcb_cursor_cut_front(sizep, rem);
        *tp = cl_main_thread_ptr;
    } else {
        CL_Thread *t = (CL_Thread *)platform_tls_get();
#ifdef CL_TLAB
        {
            uint32_t rem = t->alloc_lim - t->alloc_cur;
            if (*sizep <= rem)
                ptr = gcb_cursor_cut_chunk(t, sizep, rem);
        }
#endif
        *tp = t;
    }
    return ptr;
}

#endif /* CL_GC_CLASSIC */

#ifdef CL_GC_IMMIX
#error "GC=immix: the Immix backend (gc_immix.c) is Part 2 of specs/gc-backend-immix.md and has not landed"
#endif

#endif /* CL_GC_BACKEND_H */
