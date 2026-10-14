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

/* Backend lines of cl_mem_stats and ROOM, one "\n"-terminated line per
 * call of `out` (platform_write_string for the raw console report,
 * cl_write_cstring_to_stdout for ROOM, whose output a test may capture
 * with WITH-OUTPUT-TO-STRING). */
void gcb_stats_report(void (*out)(const char *));

/* The collection cl_gc_verify_heap runs before it checks the arena: one
 * after which every block that is not free is live and the marks are in
 * the state the verifier expects (classic: a sweep; Immix: a compaction,
 * which normalises the mark sense and drops every corpse). */
void gcb_verify_collect(void);

/* Are the header mark bits exact right now — marked == live after the
 * last trace, unmarked == dead, for EVERY block on the chain?  Classic:
 * always (the sweep clears what it finds).  Immix: only after a
 * compaction or an image adoption, until the next line-sweep cycle leaves
 * stale-marked corpses behind (gc_backend.h, "Marks without a clearing
 * pass").  The DEBUG_GC walk-based verifiers run only when this is 1. */
int gcb_marks_exact(void);

/* ---- Hot hooks, per backend ------------------------------------------ */

#ifdef CL_GC_CLASSIC

/* Mark bits: the classic backend reads the header bit as is (the Immix
 * backend flips the bit's sense every cycle, see below).  Every mark-bit
 * access outside the backend file goes through these three — the raw
 * CL_HDR_MARKED / CL_HDR_SET_MARK / CL_HDR_CLR_MARK / CL_HDR_MARK_BIT
 * forms are reserved for types.h, this header and src/core/gc_*.c
 * (tests/test_gc_mark_macros.sh is the lint). */
#define GCB_HDR_MARKED(p)    CL_HDR_MARKED(p)
#define GCB_HDR_SET_MARK(p)  CL_HDR_SET_MARK(p)
#define GCB_HDR_CLR_MARK(p)  CL_HDR_CLR_MARK(p)

/* The header word of a freshly allocated object (cl_alloc): born
 * unmarked, which for the classic bit is the plain header. */
#define GCB_MAKE_HDR(type, size)  CL_MAKE_HDR(type, size)

/* The conservative JIT-stack scan validated `off` as a block start that
 * is not on the free list.  A backend may still refuse it (Immix: a dead
 * block carries CL_TYPE_DEAD); classic accepts every block start. */
#define GCB_SCAN_REJECT(off)  0

/* gc_mark_push is about to push `obj` (an in-arena, non-immediate word).
 * Immix validates it as an object start while the trace runs from the
 * conservative JIT roots or the overflow rescan — those can start at a
 * stale-marked corpse whose slots are garbage; classic never needs to. */
#define GCB_PUSH_REJECT(obj)  0
#define GCB_MARK_VALIDATE(on) ((void)0)

/* The trace's per-walk cache (GcMarkCtx, gc_internal.h): the words the
 * inline visit reads on every reference, loaded once per drain or
 * children walk so the hot path reads registers, not globals.  Classic
 * caches nothing of its own; its mark bit is absolute. */
#define GCB_MARK_CTX_FIELDS
#define GCB_MARK_CTX_LOAD(c)        ((void)(c))
#define GCB_MARK_CTX_STORE(c)       ((void)(c))
#define GCB_HDR_MARKED_C(c, p)      GCB_HDR_MARKED(p)
#define GCB_HDR_SET_MARK_C(c, p)    GCB_HDR_SET_MARK(p)
#define GCB_ON_MARK_C(c, off, ptr)  GCB_ON_MARK(off, ptr)
#define GCB_PUSH_REJECT_C(c, obj)   0

/* The collection DEBUG_GC_STRESS forces before an allocation (cl_alloc):
 * the moving compaction, which is what makes an unprotected CL_Obj local
 * deterministic (Immix adds a second knob that mixes in its own cycles). */
#define GCB_STRESS_COLLECT()  cl_gc_compact()
#define GCB_BADMARK_REPORT(off)  ((void)0)

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

/* ---- The Immix-style mark-region backend (gc_immix.c) ----------------
 *
 * The arena is viewed as LINES of 1 << CL_GC_LINE_SHIFT bytes (128 by
 * default); the side table im_line_map holds one byte per line.  A line
 * is UNAVAILABLE when its byte equals the current epoch im_epoch — it was
 * touched by a live object in the last trace, or claimed by an allocation
 * cursor since — and available otherwise.  The trace stamps every line of
 * every marked object (GCB_ON_MARK, exact, the size is in the header),
 * the line sweep zeroes what the trace did not stamp, and the allocator
 * claims runs of available lines (holes) as chunk cursors.  Nothing in a
 * cycle walks the arena; see specs/gc-backend-immix.md, Part 2, and the
 * header comment of gc_immix.c for the chain and index invariants.
 *
 * Marks without a clearing pass: the meaning of the header mark bit flips
 * every cycle.  marked(p) == ((CL_HDR(p) ^ gc_hdr_xor) & CL_HDR_MARK_BIT);
 * gc_hdr_xor is 0 or CL_HDR_MARK_BIT, and a completed cycle toggles it,
 * so every survivor reads unmarked for the next trace at zero cost.  The
 * consequences (a corpse not yet overwritten reads marked or unmarked by
 * the parity of the cycle it died in) are confined to code that finds
 * objects by WALKING the chain: the compactor normalises every bit before
 * its trace, the conservative JIT scan and the overflow rescan validate
 * what they push (GCB_PUSH_REJECT), finalized corpses and holes carry
 * CL_TYPE_DEAD and are refused outright (GCB_SCAN_REJECT), and the
 * DEBUG_GC verifiers run only while the marks are exact. */

#ifndef CL_GC_LINE_SHIFT
#define CL_GC_LINE_SHIFT 7
#endif
#define CL_GC_LINE_SIZE  (1u << CL_GC_LINE_SHIFT)

extern uint32_t gc_hdr_xor;        /* 0 or CL_HDR_MARK_BIT: the mark sense */
extern uint8_t *im_line_map;       /* one byte per line, see above */
extern uint8_t  im_epoch;          /* 1..255, never 0 (0 = available) */
extern uint32_t im_marked_bytes;   /* live bytes found by the last trace */
extern int      gc_mark_validating;/* gc_mark_push validates object starts */

#define GCB_HDR_MARKED(p) \
    ((CL_HDR(p) ^ gc_hdr_xor) & CL_HDR_MARK_BIT)
#define GCB_HDR_SET_MARK(p) \
    (CL_HDR(p) = (CL_HDR(p) & ~CL_HDR_MARK_BIT) | (CL_HDR_MARK_BIT ^ gc_hdr_xor))
#define GCB_HDR_CLR_MARK(p) \
    (CL_HDR(p) = (CL_HDR(p) & ~CL_HDR_MARK_BIT) | gc_hdr_xor)
#define GCB_MAKE_HDR(type, size)  (CL_MAKE_HDR(type, size) | gc_hdr_xor)

/* One object has just been marked: stamp its lines with the epoch.
 * Nearly every object sits inside one line (a cons is 12 bytes, a line
 * 128), so that case is one store with no loop — on the 68020 the
 * general loop below costs more than the rest of the visit.  MAP and E
 * are the caller's cached im_line_map / im_epoch (both constant for the
 * whole trace). */
CL_ALWAYS_INLINE void im_stamp_lines(uint8_t *map, uint8_t e,
                                     uint32_t off, uint32_t size)
{
    uint32_t l = off >> CL_GC_LINE_SHIFT;
    uint32_t last = (off + size - 1) >> CL_GC_LINE_SHIFT;
    uint8_t *m = map + l;
    *m = e;
    if (last != l) {
        uint32_t n = last - l;
        do {
            *++m = e;
        } while (--n);
    }
}
#define GCB_ON_MARK(off, ptr) \
    do { uint32_t sz_ = CL_HDR_SIZE(ptr); \
         im_stamp_lines(im_line_map, im_epoch, (off), sz_); \
         im_marked_bytes += sz_; } while (0)

/* The trace's per-walk cache (GcMarkCtx, gc_internal.h).  The mark
 * sense, the epoch and the line map are constant for the whole trace
 * (the sense flips and the epoch advances between cycles); the live
 * byte count accumulates here and is added back at the store; the
 * push-validation flag is read once per walk (it changes only between
 * the trace's phases, never inside a drain). */
#define GCB_MARK_CTX_FIELDS \
    uint32_t xor_; uint8_t *map; uint32_t marked; int validate; uint8_t epoch;
#define GCB_MARK_CTX_LOAD(c) \
    ((c)->xor_ = gc_hdr_xor, (c)->map = im_line_map, (c)->epoch = im_epoch, \
     (c)->marked = 0, (c)->validate = gc_mark_validating)
#define GCB_MARK_CTX_STORE(c) \
    (im_marked_bytes += (c)->marked, (c)->marked = 0)
#define GCB_HDR_MARKED_C(c, p) \
    ((CL_HDR(p) ^ (c)->xor_) & CL_HDR_MARK_BIT)
#define GCB_HDR_SET_MARK_C(c, p) \
    (CL_HDR(p) = (CL_HDR(p) & ~CL_HDR_MARK_BIT) | (CL_HDR_MARK_BIT ^ (c)->xor_))
#define GCB_ON_MARK_C(c, off, ptr) \
    do { uint32_t sz_ = CL_HDR_SIZE(ptr); \
         im_stamp_lines((c)->map, (c)->epoch, (off), sz_); \
         (c)->marked += sz_; } while (0)
#define GCB_PUSH_REJECT_C(c, obj) \
    ((c)->validate && !gc_hdr_object_start_p(obj))

/* Finalizable objects are listed at allocation (one site), so a cycle
 * finalizes the dead ones without walking dead space. */
int  im_type_finalizable(uint8_t type);
void im_fin_note(uint32_t off, int need_lock);
#define GCB_ON_ALLOC_FINALIZABLE(off, type, multi) \
    do { if (im_type_finalizable(type)) im_fin_note((off), (multi)); } while (0)

#define GCB_SCAN_REJECT(off) \
    (CL_HDR_TYPE(cl_heap.arena + (off)) == CL_TYPE_DEAD)

int  gc_hdr_object_start_p(uint32_t off);     /* gc_jitscan.c */
void gc_hdr_object_start_begin(void);         /* reset its walk cursor */
#define GCB_PUSH_REJECT(obj) \
    (gc_mark_validating && !gc_hdr_object_start_p(obj))
#define GCB_MARK_VALIDATE(on) \
    (gc_mark_validating = (on), gc_hdr_object_start_begin())

#ifdef DEBUG_GC_STRESS
void im_stress_collect(void);
#define GCB_STRESS_COLLECT()  im_stress_collect()
#else
#define GCB_STRESS_COLLECT()  cl_gc_compact()
#endif

/* A [GC-BADMARK] report names `off`: the backend adds what it knows (the
 * claims that formatted that address, the header they found there). */
#if defined(DEBUG_GC) || defined(DEBUG_GC_STRESS)
void im_debug_badmark(uint32_t off);
#define GCB_BADMARK_REPORT(off)  im_debug_badmark(off)
#endif

/* ---- The Immix allocation cursor (cl_alloc's fast path) ----
 *
 * One cursor shape in both regimes: the thread's chunk [alloc_cur,
 * alloc_lim) (thread.h), a hole the backend claimed for it in
 * gcb_alloc_slow.  Single-threaded the chunk is the main thread's, found
 * through cl_main_thread_ptr (no TLS load), and no lock is taken anywhere;
 * multi-threaded the claim takes alloc_mutex and the cut does not.  The
 * invariants of the classic chunk hold unchanged (the remainder is always
 * a formatted block, never under CL_MIN_ALLOC_SIZE, every cursor dropped
 * under stop-the-world), with one difference: a hole or remainder is
 * formatted with CL_TYPE_DEAD, not as a raw-size free block, so no walk
 * and no conservative scan can mistake it for a cons. */

/* Format [off, off+len) as one dead block (len >= CL_ALIGN, aligned,
 * <= CL_HDR_SIZE_MASK).  Its mark bit is irrelevant: nothing marks or
 * traces a dead block. */
CL_ALWAYS_INLINE void gcb_format_dead(uint32_t off, uint32_t len)
{
    CL_HDR(cl_heap.arena + off) = (CL_TYPE_DEAD << CL_HDR_TYPE_SHIFT) | len;
}

/* Cut *sizep bytes from t's chunk (rem = alloc_lim - alloc_cur >= size);
 * may grow *sizep by an absorbed undersized tail.  Owner-only, lock-free.
 *
 * Unlike the classic chunk, this one is READ by other threads while it is
 * being cut: a claim (im_claim) walks the chain on the live heap, under
 * alloc_mutex only, to find the block covering the byte before its hole,
 * the one covering its end, and the headers it re-points the index at —
 * and a neighbouring region can be another thread's chunk.  The walk
 * stays consistent as long as every header it can reach covers what
 * follows: the remainder header is stored BEFORE cl_alloc stores the
 * object header over the old one (which, until then, still covers both),
 * the object header's slot is never zeroed (alloc_zero, mem.c), and
 * `multi` orders the two stores on a weakly-ordered host, where the
 * walker could otherwise see the object header and stale bytes behind it
 * (a compiler barrier on the single-core m68k). */
CL_ALWAYS_INLINE void *gcb_cursor_cut(CL_Thread *t, uint32_t *sizep,
                                      uint32_t rem, int multi)
{
    uint32_t size = *sizep;
    uint32_t cur = t->alloc_cur;
    void *ptr = cl_heap.arena + cur;

    if (rem - size < CL_MIN_ALLOC_SIZE) {
        size = rem;
        *sizep = size;
    }
    cur += size;
    if (cur == t->alloc_lim)
        t->alloc_cur = t->alloc_lim = 0;
    else {
        t->alloc_cur = cur;
        gcb_format_dead(cur, t->alloc_lim - cur);
        if (multi) platform_memory_barrier();
    }
    t->alloc_consed += size;
    return ptr;
}

CL_ALWAYS_INLINE void *gcb_alloc_fast(uint32_t *sizep, int multi,
                                      CL_Thread **tp)
{
    CL_Thread *t = multi ? (CL_Thread *)platform_tls_get() : cl_main_thread_ptr;
    uint32_t rem = t->alloc_lim - t->alloc_cur;
    *tp = t;
    if (*sizep <= rem)
        return gcb_cursor_cut(t, sizep, rem, multi);
    return NULL;
}

#endif /* CL_GC_IMMIX */

#endif /* CL_GC_BACKEND_H */
