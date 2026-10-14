#include "test.h"
#include "core/types.h"
#include "core/mem.h"
#include "core/error.h"
#include "core/package.h"
#include "core/symbol.h"
#include "core/thread.h"
#include "platform/platform.h"

#include <stdlib.h>
#include <string.h>

/* The classic backend's cursor shapes (the live bump front single-
 * threaded, the chunk multi-threaded): trivial pass under another
 * backend, whose cursor has its own suite. */
#ifdef CL_GC_CLASSIC

/*
 * Tests for the allocation cursor (gc_backend.h, "The classic allocation
 * cursor"; the shared drop/retire paths in mem.c): the
 * region cl_alloc's fast path cuts objects from without locking, and the
 * backend's refill of it (gcb_alloc_slow).
 *
 * The contract these tests pin down:
 *
 *  - single-threaded, the cursor is the live bump front: every cut
 *    advances cl_heap.bump by exactly the object's size and accounts it
 *    there and then, so the front, the walk bound and the statistics are
 *    exact after every allocation — nothing is cached in the thread and
 *    nothing has to be flushed;
 *  - a cut never leaves a remainder smaller than CL_MIN_ALLOC_SIZE: at
 *    the arena's end the tail is absorbed into the object and the front
 *    lands exactly on arena_size;
 *  - collections, retirement and image adoption leave the front
 *    consistent and the next allocation lands exactly on it;
 *  - multi-threaded (CL_TLAB), the cursor is a private chunk in the
 *    thread's fields; once the thread is alone again the chunk is never
 *    cut (the front is used — cutting it would move the front backwards),
 *    its remainder stays a walkable hole, and its deferred accounting is
 *    flushed by the next collection or slow-path entry.
 *
 * The chunk kind's own lifecycle under real concurrency (refills,
 * concurrent GC, retirement) is tests/test_gc_threaded.c's TLAB section.
 */

static void setup(void)
{
    /* Classic collector: the front's behaviour across a sweep (the bump
     * does not move) and a compaction (it does) are what is checked. */
    setenv("AMILISP_GENGC", "0", 1);
    platform_init();
    cl_thread_init();
    cl_error_init();
    cl_mem_init(CL_DEFAULT_HEAP_SIZE);
    cl_package_init();
    cl_symbol_init();
}

static void teardown(void)
{
    cl_mem_shutdown();
    platform_shutdown();
}

/* Walk the whole arena with the iterator and check the chain contract:
 * contiguous blocks from CL_ALIGN to the bump front.  Fails the calling
 * test on a violation; *total gets the summed block sizes. */
#define CHECKED_WALK(total) do {                                            \
    GcWalk _w;                                                              \
    uint32_t _off, _size, _expect = CL_ALIGN;                               \
    uint8_t _type;                                                          \
    (total) = 0;                                                            \
    gc_walk_begin(&_w);                                                     \
    while (gc_walk_next(&_w, &_off, &_size, &_type)) {                      \
        ASSERT_EQ_INT((int)_off, (int)_expect);                             \
        ASSERT(_size >= CL_ALIGN && (_size & (CL_ALIGN - 1)) == 0);         \
        _expect = _off + _size;                                             \
        (total) += _size;                                                   \
    }                                                                       \
    ASSERT(gc_walk_complete(&_w));                                          \
    ASSERT_EQ_INT((int)_expect, (int)cl_heap.bump);                         \
} while (0)

/* Is `off` a block start of the walk, and with what size/type? */
static int walk_find(uint32_t off, uint32_t *size, uint8_t *type)
{
    GcWalk w;
    uint32_t o, s;
    uint8_t t;
    gc_walk_begin(&w);
    while (gc_walk_next(&w, &o, &s, &t)) {
        if (o == off) { *size = s; *type = t; return 1; }
        if (o > off) return 0;
    }
    return 0;
}

#define CHUNK_EMPTY(t) ((t)->alloc_cur == 0 && (t)->alloc_lim == 0)

/* --- Single-threaded: the cursor is the live bump front. --- */

TEST(front_cursor_is_the_bump_front)
{
    CL_Thread *t = cl_get_current_thread();
    uint32_t bump0, allocated0, consed0;
    CL_Obj c;
    int i;

    cl_gc();
    ASSERT(CHUNK_EMPTY(t));

    bump0 = cl_heap.bump;
    allocated0 = cl_heap.total_allocated;
    consed0 = cl_heap.total_consed;

    c = cl_cons(CL_MAKE_FIXNUM(1), CL_NIL);
    CL_GC_PROTECT(c);

    /* The object sits at the old front, the front moved by its size, the
     * accounting is per cut and nothing was cached in the thread. */
    ASSERT_EQ_INT((int)c, (int)bump0);
    ASSERT_EQ_INT((int)cl_heap.bump, (int)(bump0 + CL_HDR_SIZE(CL_OBJ_TO_PTR(c))));
    ASSERT_EQ_INT((int)(cl_heap.total_allocated - allocated0),
                  (int)CL_HDR_SIZE(CL_OBJ_TO_PTR(c)));
    ASSERT_EQ_INT((int)(cl_heap.total_consed - consed0),
                  (int)CL_HDR_SIZE(CL_OBJ_TO_PTR(c)));
    ASSERT(CHUNK_EMPTY(t));
    ASSERT_EQ_INT((int)t->alloc_consed, 0);

    /* Every further cut: each object exactly where the front was, the
     * walk contiguous up to it at every step. */
    for (i = 0; i < 1000; i++) {
        uint32_t before = cl_heap.bump, total;
        CL_Obj o = cl_make_vector((uint32_t)(i % 7) + 1);
        ASSERT_EQ_INT((int)o, (int)before);
        ASSERT_EQ_INT((int)cl_heap.bump,
                      (int)(before + CL_HDR_SIZE(CL_OBJ_TO_PTR(o))));
        ASSERT(CHUNK_EMPTY(t));
        if (i % 100 == 0) {
            CHECKED_WALK(total);
            ASSERT_EQ_INT((int)total, (int)(cl_heap.bump - CL_ALIGN));
        }
    }
    ASSERT_EQ_INT((int)(cl_heap.total_allocated - allocated0),
                  (int)(cl_heap.bump - bump0));
    ASSERT_EQ_INT((int)(cl_heap.total_consed - consed0),
                  (int)(cl_heap.bump - bump0));
    CL_GC_UNPROTECT(1);
}

/* --- Collections and retirement leave the front consistent. --- */

TEST(collections_leave_the_front_consistent)
{
    CL_Thread *t = cl_get_current_thread();
    CL_Obj keep = CL_NIL;
    uint32_t bump_after, total;
    int i;

    CL_GC_PROTECT(keep);
    for (i = 0; i < 500; i++)
        keep = cl_cons(CL_MAKE_FIXNUM(i), keep);

    /* Sweep: the front does not move; the next object lands on it. */
    cl_gc();
    bump_after = cl_heap.bump;
    keep = cl_cons(CL_MAKE_FIXNUM(-1), keep);
    ASSERT_EQ_INT((int)keep, (int)bump_after);
    ASSERT_EQ_INT((int)cl_heap.bump,
                  (int)(bump_after + CL_HDR_SIZE(CL_OBJ_TO_PTR(keep))));
    CHECKED_WALK(total);

    /* Compaction: the front moves down; the next object lands on the NEW
     * front, never at a pre-compaction offset. */
    for (i = 0; i < 500; i++)
        cl_cons(CL_MAKE_FIXNUM(i), CL_NIL);          /* garbage */
    cl_gc_compact();
    bump_after = cl_heap.bump;
    keep = cl_cons(CL_MAKE_FIXNUM(-2), keep);
    ASSERT_EQ_INT((int)keep, (int)bump_after);
    CHECKED_WALK(total);

    /* Retirement (what cl_thread_unregister does) is a no-op for a thread
     * without a chunk: the front is untouched. */
    bump_after = cl_heap.bump;
    cl_cursor_retire(t);
    ASSERT(CHUNK_EMPTY(t));
    ASSERT_EQ_INT((int)cl_heap.bump, (int)bump_after);
    keep = cl_cons(CL_MAKE_FIXNUM(-3), keep);
    ASSERT_EQ_INT((int)keep, (int)bump_after);

    /* The kept list survived all of it. */
    {
        CL_Obj p = keep;
        int n = 0;
        for (; !CL_NULL_P(p); p = cl_cdr(p)) n++;
        ASSERT_EQ_INT(n, 503);
    }
    CL_GC_UNPROTECT(1);
}

/* --- The arena's tail is absorbed, never left as an unwalkable sliver. --- */

TEST(front_cut_absorbs_the_arena_tail)
{
    uint32_t rem, total;
    void *tail;
    CL_Obj probe;

    /* A small heap so filling it is cheap. */
    cl_mem_shutdown();
    cl_mem_init(256 * 1024);

    /* Fill to within a few blocks of the end with garbage... */
    while (cl_heap.arena_size - cl_heap.bump > 4096)
        cl_make_vector(200);
    /* ...then ask for a block that would leave CL_ALIGN bytes behind.  The
     * cut grows the request by that sliver: the block ends exactly at
     * arena_size and the front lands there with it. */
    rem = cl_heap.arena_size - cl_heap.bump;
    ASSERT(rem > CL_MIN_ALLOC_SIZE + CL_ALIGN);
    tail = cl_alloc(TYPE_BIT_VECTOR, rem - CL_ALIGN);
    ASSERT(tail != NULL);
    ASSERT_EQ_INT((int)CL_HDR_SIZE(tail), (int)rem);
    ASSERT_EQ_INT((int)cl_heap.bump, (int)cl_heap.arena_size);
    CHECKED_WALK(total);
    ASSERT_EQ_INT((int)total, (int)(cl_heap.arena_size - CL_ALIGN));

    /* The front is spent: the next allocation goes through the backend —
     * a collection, then a free block or the compacted front.  Either way
     * it is served and the arena walks. */
    probe = cl_cons(CL_MAKE_FIXNUM(7), CL_NIL);
    CL_GC_PROTECT(probe);
    ASSERT(cl_heap.gc_count > 0);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(probe)), 7);
    CHECKED_WALK(total);
    CL_GC_UNPROTECT(1);

    cl_mem_shutdown();
    cl_mem_init(CL_DEFAULT_HEAP_SIZE);
}

/* --- Regime changes: the chunk is used only while multi-threaded. --- */

TEST(chunk_is_never_cut_once_the_thread_is_alone_again)
{
    CL_Thread *main_t = cl_get_current_thread();
    CL_Thread *fake;
    CL_Obj keep = CL_NIL, o;
    uint32_t front_before, total;
    uint32_t hole_off = 0, hole_len = 0;
    int i;

    CL_GC_PROTECT(keep);
    for (i = 0; i < 100; i++)
        keep = cl_cons(CL_MAKE_FIXNUM(i), keep);
    ASSERT(CHUNK_EMPTY(main_t));
    front_before = cl_heap.bump;

    /* A newborn is registered (cl_thread_count -> 2) but never parks: a
     * stop-the-world initiator does not wait for it, so collections here
     * cannot deadlock. */
    fake = cl_thread_alloc_worker();
    ASSERT(fake != NULL);
    fake->status = 1;
    cl_thread_register_newborn(fake);
    ASSERT_EQ_INT((int)cl_thread_count, 2);

    o = cl_cons(CL_MAKE_FIXNUM(1000), CL_NIL);
    CL_GC_PROTECT(o);
#ifdef CL_TLAB
    /* Multi-threaded: a chunk carved from the front — the object lies
     * inside it, its remainder is a formatted hole, the front advanced
     * past the whole chunk, the accounting is deferred. */
    ASSERT(!CHUNK_EMPTY(main_t));
    ASSERT(o >= front_before && o < main_t->alloc_lim);
    ASSERT(main_t->alloc_cur > o);
    ASSERT_EQ_INT((int)cl_heap.bump, (int)main_t->alloc_lim);
    ASSERT(cl_heap.bump - front_before >= 4096);
    hole_off = main_t->alloc_cur;
    hole_len = main_t->alloc_lim - main_t->alloc_cur;
    ASSERT(hole_len >= CL_MIN_ALLOC_SIZE);
    ASSERT_EQ_INT((int)((CL_FreeBlock *)(cl_heap.arena + hole_off))->size,
                  (int)hole_len);
    ASSERT(main_t->alloc_consed > 0);
#else
    /* No chunks in this build: allocated directly at the front, under
     * the lock. */
    ASSERT(CHUNK_EMPTY(main_t));
    ASSERT_EQ_INT((int)o, (int)front_before);
#endif
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(o)), 1000);
    CHECKED_WALK(total);

    /* Back to one thread: the chunk (if any) is never cut again — cutting
     * it would move the front backwards.  The next object lands on the
     * front, the chunk's remainder stays a hole. */
    cl_thread_unregister(fake);
    cl_thread_free_worker(fake);
    ASSERT_EQ_INT((int)cl_thread_count, 1);
    front_before = cl_heap.bump;
    {
        CL_Obj p = cl_cons(CL_MAKE_FIXNUM(2000), CL_NIL);
        CL_GC_PROTECT(p);
        ASSERT_EQ_INT((int)p, (int)front_before);
        ASSERT_EQ_INT((int)cl_heap.bump,
                      (int)(front_before + CL_HDR_SIZE(CL_OBJ_TO_PTR(p))));
        CL_GC_UNPROTECT(1);
    }
    if (hole_len) {
        uint32_t size;
        uint8_t type;
        ASSERT_EQ_INT((int)main_t->alloc_cur, (int)hole_off);   /* untouched */
        ASSERT(walk_find(hole_off, &size, &type));
        ASSERT_EQ_INT((int)size, (int)hole_len);
        ASSERT_EQ_INT(type, 0);
    }
    CHECKED_WALK(total);

    /* A sweep drops the leftover chunk (flushing its deferred accounting)
     * and reclaims the hole into the free list; a compaction removes it;
     * the kept data survives both. */
    {
        uint32_t consed_before = cl_heap.total_consed;
        uint32_t deferred = main_t->alloc_consed;
        cl_gc();
        ASSERT(CHUNK_EMPTY(main_t));
        ASSERT_EQ_INT((int)main_t->alloc_consed, 0);
        ASSERT(cl_heap.total_consed - consed_before >= deferred);
    }
    CHECKED_WALK(total);
    cl_gc_compact();
    CHECKED_WALK(total);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(o)), 1000);
    {
        CL_Obj p = keep;
        int n = 0;
        for (; !CL_NULL_P(p); p = cl_cdr(p)) n++;
        ASSERT_EQ_INT(n, 100);
    }
    CL_GC_UNPROTECT(2);
}

#ifdef CL_TLAB
/* A leftover chunk is also dropped by the single-threaded SLOW path (the
 * front spent -> the backend), not only by a collection: its accounting
 * is flushed before the direct allocation is accounted. */
TEST(leftover_chunk_flushed_by_the_slow_path)
{
    CL_Thread *main_t = cl_get_current_thread();
    CL_Thread *fake;
    CL_Obj keep = CL_NIL;
    uint32_t deferred, consed_before, total;
    int i;

    cl_mem_shutdown();
    cl_mem_init(256 * 1024);
    main_t = cl_get_current_thread();
    CL_GC_PROTECT(keep);

    fake = cl_thread_alloc_worker();
    ASSERT(fake != NULL);
    fake->status = 1;
    cl_thread_register_newborn(fake);
    for (i = 0; i < 50; i++)
        keep = cl_cons(CL_MAKE_FIXNUM(i), keep);
    ASSERT(!CHUNK_EMPTY(main_t));
    deferred = main_t->alloc_consed;
    ASSERT(deferred >= 50 * CL_MIN_ALLOC_SIZE);
    cl_thread_unregister(fake);
    cl_thread_free_worker(fake);

    /* Spend the front with garbage (the chunk is never touched), then
     * allocate once more: the slow path drops the chunk, collects, and
     * serves the object. */
    consed_before = cl_heap.total_consed;
    while (cl_heap.arena_size - cl_heap.bump > 4096)
        cl_make_vector(200);
    ASSERT(!CHUNK_EMPTY(main_t));
    {
        uint32_t gc_before = cl_heap.gc_count;
        CL_Obj v = cl_make_vector(2000);
        CL_GC_PROTECT(v);
        ASSERT(cl_heap.gc_count > gc_before);
        ASSERT(CHUNK_EMPTY(main_t));
        ASSERT_EQ_INT((int)main_t->alloc_consed, 0);
        ASSERT(cl_heap.total_consed - consed_before >= deferred);
        CL_GC_UNPROTECT(1);
    }
    CHECKED_WALK(total);
    {
        CL_Obj p = keep;
        int n = 0;
        for (; !CL_NULL_P(p); p = cl_cdr(p)) n++;
        ASSERT_EQ_INT(n, 50);
    }
    CL_GC_UNPROTECT(1);

    cl_mem_shutdown();
    cl_mem_init(CL_DEFAULT_HEAP_SIZE);
}

/* With a second thread registered, main cuts from chunks; a collection
 * drops them (flushing the deferred accounting) and the chunk's remainder
 * parses as a hole for the collector. */
TEST(chunk_cursor_dropped_by_collection_with_accounting_flushed)
{
    CL_Thread *main_t = cl_get_current_thread();
    CL_Thread *fake;
    CL_Obj keep = CL_NIL;
    uint32_t consed_before, deferred, total;
    int i;

    CL_GC_PROTECT(keep);
    fake = cl_thread_alloc_worker();
    ASSERT(fake != NULL);
    fake->status = 1;
    cl_thread_register_newborn(fake);

    for (i = 0; i < 200; i++)
        keep = cl_cons(CL_MAKE_FIXNUM(i), keep);
    ASSERT(!CHUNK_EMPTY(main_t));
    deferred = main_t->alloc_consed;
    ASSERT(deferred >= 200 * CL_MIN_ALLOC_SIZE);
    consed_before = cl_heap.total_consed;

    cl_gc();
    ASSERT(CHUNK_EMPTY(main_t));
    ASSERT_EQ_INT((int)main_t->alloc_consed, 0);
    ASSERT(cl_heap.total_consed - consed_before >= deferred);
    CHECKED_WALK(total);

    cl_gc_compact();
    ASSERT(CHUNK_EMPTY(main_t));
    CHECKED_WALK(total);
    {
        CL_Obj p = keep;
        int n = 0;
        for (; !CL_NULL_P(p); p = cl_cdr(p)) n++;
        ASSERT_EQ_INT(n, 200);
    }

    cl_thread_unregister(fake);
    cl_thread_free_worker(fake);
    CL_GC_UNPROTECT(1);
}
#endif /* CL_TLAB */

/* --- Image adoption replaces the arena; the front follows the payload. --- */

TEST(image_adoption_resets_the_front)
{
    CL_Thread *t = cl_get_current_thread();
    CL_Obj keep = CL_NIL, p;
    uint32_t dense_bump, total;
    int i;

    CL_GC_PROTECT(keep);
    for (i = 0; i < 300; i++)
        keep = cl_cons(CL_MAKE_FIXNUM(i), keep);
    cl_gc_compact();                 /* dense [CL_ALIGN, bump): a payload */
    dense_bump = cl_heap.bump;

    /* Allocate past it, then adopt a payload that ends at dense_bump. */
    cl_cons(CL_MAKE_FIXNUM(-1), CL_NIL);
    ASSERT(cl_heap.bump > dense_bump);
    cl_mem_adopt_image_begin(dense_bump);
    ASSERT(CHUNK_EMPTY(t));
    ASSERT_EQ_INT((int)cl_heap.bump, (int)dense_bump);
    cl_mem_adopt_image_finish();

    p = cl_cons(CL_MAKE_FIXNUM(-2), keep);
    CL_GC_PROTECT(p);
    ASSERT_EQ_INT((int)p, (int)dense_bump);          /* at the adopted front */
    CHECKED_WALK(total);
    cl_gc_compact();
    CHECKED_WALK(total);
    {
        CL_Obj q = p;
        int n = 0;
        for (; !CL_NULL_P(q); q = cl_cdr(q)) n++;
        ASSERT_EQ_INT(n, 301);
    }
    CL_GC_UNPROTECT(2);
}

int main(void)
{
    setup();

    RUN(front_cursor_is_the_bump_front);
    RUN(collections_leave_the_front_consistent);
    RUN(front_cut_absorbs_the_arena_tail);
    RUN(chunk_is_never_cut_once_the_thread_is_alone_again);
#ifdef CL_TLAB
    RUN(leftover_chunk_flushed_by_the_slow_path);
    RUN(chunk_cursor_dropped_by_collection_with_accounting_flushed);
#endif
    RUN(image_adoption_resets_the_front);

    teardown();
    REPORT();
}

#else /* !CL_GC_CLASSIC */

TEST(classic_backend_not_built) { ASSERT(1); }
int main(void) { test_init(); RUN(classic_backend_not_built); REPORT(); }

#endif /* CL_GC_CLASSIC */
