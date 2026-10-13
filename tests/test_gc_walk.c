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

/*
 * Tests for the arena iterator (mem.h: GcWalk, gc_walk_begin / gc_walk_next
 * / gc_walk_seek / gc_walk_complete).
 *
 * Every pass that steps the arena's header chain — sweep, forwarding,
 * slide, rehash, the image writer and reader, the shutdown release walk —
 * goes through this one iterator, so what the tests pin down is the
 * chain contract they all rely on:
 *
 *  - the walk starts at CL_ALIGN (offset 0 is NIL's reserved slot), yields
 *    every block in address order, each block starting exactly where the
 *    previous one ended, and ends exactly at the bump front;
 *  - a block is whatever a header describes: a live object, a free-list
 *    block (type 0, the raw size), or a bare size-only gap header too
 *    small to carry a free-list link (gc_make_free_gap's degenerate case);
 *  - a header the walk cannot step over (size 0) ends it there, and
 *    gc_walk_complete says so — a release build truncates the pass as the
 *    hand-written loops used to, the debug builds abort loudly;
 *  - gc_walk_seek re-synchronises the cursor after a body rewrote the
 *    chain (the sweep coalesces dead neighbours into one block).
 */

static void setup(void)
{
    /* Classic collector: the free list and the sweep's coalescing are
     * classic-mode machinery; the iterator itself does not care. */
    setenv("CLAMIGA_GENGC", "0", 1);
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

/* Walk the whole arena, checking the chain contract as it goes.  Returns
 * the number of blocks; *total is the sum of their sizes.  Fails the
 * calling test on any contract violation. */
#define CHECKED_WALK(nblocks, total) do {                                   \
    GcWalk _w;                                                              \
    uint32_t _off, _size, _expect = CL_ALIGN;                               \
    uint8_t _type;                                                          \
    (nblocks) = 0; (total) = 0;                                             \
    gc_walk_begin(&_w);                                                     \
    while (gc_walk_next(&_w, &_off, &_size, &_type)) {                      \
        ASSERT_EQ_INT((int)_off, (int)_expect);         /* contiguous */    \
        ASSERT(_size >= CL_ALIGN && (_size & (CL_ALIGN - 1)) == 0);         \
        ASSERT_EQ_INT((int)_size, (int)CL_HDR_SIZE(cl_heap.arena + _off));  \
        ASSERT_EQ_INT((int)_type, (int)CL_HDR_TYPE(cl_heap.arena + _off));  \
        _expect = _off + _size;                                             \
        (nblocks)++; (total) += _size;                                      \
    }                                                                       \
    ASSERT(gc_walk_complete(&_w));                                          \
    ASSERT_EQ_INT((int)_expect, (int)cl_heap.bump);     /* ends at bump */  \
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

static uint32_t free_list_bytes(uint32_t *blocks)
{
    uint32_t off, bytes = 0;
    *blocks = 0;
    for (off = cl_heap.free_list; off; ) {
        CL_FreeBlock *fb = (CL_FreeBlock *)(cl_heap.arena + off);
        bytes += fb->size;
        (*blocks)++;
        off = fb->next_offset;
    }
    return bytes;
}

/* --- Fresh allocations: every object a block, in order, up to bump. --- */

TEST(walk_covers_fresh_allocations)
{
    CL_Obj c, s, v;
    uint32_t n, total, size;
    uint8_t type;

    c = cl_cons(CL_MAKE_FIXNUM(1), CL_NIL);
    CL_GC_PROTECT(c);
    s = cl_make_string("arena iterator", 14);
    CL_GC_PROTECT(s);
    v = cl_make_vector(100);
    CL_GC_PROTECT(v);

    CHECKED_WALK(n, total);
    ASSERT(n >= 3);
    ASSERT_EQ_INT((int)total, (int)(cl_heap.bump - CL_ALIGN));

    /* Each object is yielded at its own offset with its own header. */
    ASSERT(walk_find(c, &size, &type));
    ASSERT_EQ_INT(type, TYPE_CONS);
    ASSERT_EQ_INT((int)size, (int)CL_HDR_SIZE(CL_OBJ_TO_PTR(c)));
    ASSERT(walk_find(s, &size, &type));
    ASSERT_EQ_INT(type, CL_HDR_TYPE(CL_OBJ_TO_PTR(s)));
    ASSERT(walk_find(v, &size, &type));
    ASSERT_EQ_INT(type, TYPE_VECTOR);
    ASSERT(size >= 100 * sizeof(CL_Obj));

    /* An interior offset is not a block start. */
    ASSERT(!walk_find(v + CL_ALIGN, &size, &type));

    CL_GC_UNPROTECT(3);
}

/* --- After a sweep: free blocks are blocks too, and the accounting
 *     (live + free list) is exactly what the walk sees. --- */

#ifdef CL_GC_CLASSIC   /* the free list and the sweep's coalescing are classic */
TEST(walk_sees_free_blocks_after_sweep)
{
    CL_Obj keep = CL_NIL;
    uint32_t n, total, free_blocks, free_bytes, off, size;
    uint8_t type;
    int i;

    CL_GC_PROTECT(keep);
    for (i = 0; i < 2000; i++) {
        CL_Obj o = cl_make_vector((uint32_t)(i % 13) + 1);
        if (i % 5 == 0) {
            CL_GC_PROTECT(o);
            keep = cl_cons_rooted(&o, &keep);
            CL_GC_UNPROTECT(1);
        }
    }
    cl_gc();                                 /* classic: sweep + coalesce */

    CHECKED_WALK(n, total);
    ASSERT_EQ_INT((int)total, (int)(cl_heap.bump - CL_ALIGN));

    free_bytes = free_list_bytes(&free_blocks);
    ASSERT(free_blocks > 0);
    ASSERT_EQ_INT((int)(free_bytes + cl_heap.total_allocated), (int)total);

    /* Every free-list block is a block start of the walk, with the free
     * block's own (coalesced) size and the raw-size header's type 0. */
    for (off = cl_heap.free_list; off;
         off = ((CL_FreeBlock *)(cl_heap.arena + off))->next_offset) {
        ASSERT(walk_find(off, &size, &type));
        ASSERT_EQ_INT((int)size, (int)((CL_FreeBlock *)(cl_heap.arena + off))->size);
        ASSERT_EQ_INT(type, 0);
        ASSERT(!CL_HDR_MARKED(cl_heap.arena + off));
    }

    /* The kept objects are still blocks (and still unmarked after the sweep). */
    {
        CL_Obj p;
        int kept = 0;
        for (p = keep; !CL_NULL_P(p); p = cl_cdr(p)) {
            ASSERT(walk_find(cl_car(p), &size, &type));
            ASSERT_EQ_INT(type, TYPE_VECTOR);
            ASSERT(!CL_HDR_MARKED(cl_heap.arena + cl_car(p)));
            kept++;
        }
        ASSERT_EQ_INT(kept, 400);
    }
    CL_GC_UNPROTECT(1);
}

/* --- A bare size-only gap header (no room for a free-list link) is a
 *     block the walk steps over, and the next sweep absorbs it. --- */

TEST(walk_steps_over_a_bare_gap_header)
{
    CL_Obj keep = CL_NIL;
    uint32_t n, total, off, fb_size, gap, size, next_off;
    uint8_t type;
    int i;

    CL_GC_PROTECT(keep);
    for (i = 0; i < 500; i++) {
        CL_Obj o = cl_make_vector(8);
        if (i % 2 == 0) {
            CL_GC_PROTECT(o);
            keep = cl_cons_rooted(&o, &keep);
            CL_GC_UNPROTECT(1);
        }
    }
    cl_gc();

    /* A free block big enough to lose CL_ALIGN bytes off its tail. */
    for (off = cl_heap.free_list; off;
         off = ((CL_FreeBlock *)(cl_heap.arena + off))->next_offset)
        if (((CL_FreeBlock *)(cl_heap.arena + off))->size >= 4 * CL_MIN_ALLOC_SIZE)
            break;
    ASSERT(off != 0);
    fb_size = ((CL_FreeBlock *)(cl_heap.arena + off))->size;
    ASSERT(walk_find(off + fb_size, &size, &type) || off + fb_size == cl_heap.bump);
    next_off = off + fb_size;

    /* Shrink the free block and write a bare header over its last
     * CL_ALIGN bytes: type 0, size CL_ALIGN, no link — exactly what
     * gc_make_free_gap writes for a sub-header remainder. */
    gap = off + fb_size - CL_ALIGN;
    ((CL_FreeBlock *)(cl_heap.arena + off))->size = fb_size - CL_ALIGN;
    ((CL_Header *)(cl_heap.arena + gap))->header = CL_ALIGN;

    CHECKED_WALK(n, total);
    ASSERT_EQ_INT((int)total, (int)(cl_heap.bump - CL_ALIGN));
    ASSERT(walk_find(off, &size, &type));
    ASSERT_EQ_INT((int)size, (int)(fb_size - CL_ALIGN));
    ASSERT(walk_find(gap, &size, &type));
    ASSERT_EQ_INT((int)size, CL_ALIGN);
    ASSERT_EQ_INT(type, 0);
    if (next_off < cl_heap.bump)
        ASSERT(walk_find(next_off, &size, &type));

    /* The sweep (which walks through the same iterator and re-seeks after
     * coalescing) absorbs the gap back into the free block. */
    cl_gc();
    CHECKED_WALK(n, total);
    ASSERT(walk_find(off, &size, &type));
    ASSERT(size >= fb_size);
    ASSERT(!walk_find(gap, &size, &type));

    CL_GC_UNPROTECT(1);
}

/* --- After a compaction the chain is dense: no free blocks, every
 *     block live, and the walk's total is the live total. --- */

#endif /* CL_GC_CLASSIC */

TEST(walk_after_compaction_is_dense)
{
    CL_Obj keep = CL_NIL;
    uint32_t n, total, free_blocks;
    int i;

    CL_GC_PROTECT(keep);
    for (i = 0; i < 1000; i++) {
        CL_Obj o = cl_cons(CL_MAKE_FIXNUM(i), CL_NIL);
        if (i % 3 == 0) {
            CL_GC_PROTECT(o);
            keep = cl_cons_rooted(&o, &keep);
            CL_GC_UNPROTECT(1);
        }
    }
    cl_gc_compact();

    CHECKED_WALK(n, total);
    ASSERT_EQ_INT((int)total, (int)(cl_heap.bump - CL_ALIGN));
    ASSERT_EQ_INT((int)total, (int)cl_heap.total_allocated);
    ASSERT_EQ_INT((int)free_list_bytes(&free_blocks), 0);
    ASSERT_EQ_INT((int)free_blocks, 0);
    CL_GC_UNPROTECT(1);
}

/* --- gc_walk_seek: the cursor continues at a block the body chose. --- */

TEST(walk_seek_continues_at_the_given_block)
{
    GcWalk w;
    uint32_t off, size, first_off, first_size, third_off;
    uint8_t type;
    CL_Obj a, b, c;

    a = cl_cons(CL_NIL, CL_NIL);
    CL_GC_PROTECT(a);
    b = cl_cons(CL_NIL, CL_NIL);
    CL_GC_PROTECT(b);
    c = cl_cons(CL_NIL, CL_NIL);
    CL_GC_PROTECT(c);
    ASSERT_EQ_INT((int)b, (int)(a + CL_HDR_SIZE(CL_OBJ_TO_PTR(a))));
    ASSERT_EQ_INT((int)c, (int)(b + CL_HDR_SIZE(CL_OBJ_TO_PTR(b))));

    gc_walk_begin(&w);
    ASSERT(gc_walk_next(&w, &first_off, &first_size, &type));
    ASSERT_EQ_INT((int)first_off, CL_ALIGN);
    ASSERT(!gc_walk_complete(&w));

    /* Skip ahead: the next block yielded is the one we seek to, as if the
     * body had merged everything in between into one block. */
    gc_walk_seek(&w, c);
    ASSERT(gc_walk_next(&w, &off, &size, &type));
    ASSERT_EQ_INT((int)off, (int)c);
    ASSERT_EQ_INT(type, TYPE_CONS);

    /* ...and back: a seek is just a cursor move. */
    gc_walk_seek(&w, a);
    ASSERT(gc_walk_next(&w, &off, &size, &type));
    ASSERT_EQ_INT((int)off, (int)a);
    ASSERT(gc_walk_next(&w, &off, &size, &type));
    ASSERT_EQ_INT((int)off, (int)b);
    ASSERT(gc_walk_next(&w, &third_off, &size, &type));
    ASSERT_EQ_INT((int)third_off, (int)c);

    /* Seeking to the end completes the walk. */
    gc_walk_seek(&w, cl_heap.bump);
    ASSERT(!gc_walk_next(&w, &off, &size, &type));
    ASSERT(gc_walk_complete(&w));

    CL_GC_UNPROTECT(3);
}

#if !defined(DEBUG_GC) && !defined(DEBUG_GC_STRESS)
/* --- A size-0 header ends the walk there, and gc_walk_complete says the
 *     walk did NOT reach the bump front.  (The debug builds report the
 *     header and abort instead, so this case is release-only.) --- */

TEST(walk_stops_at_a_broken_header)
{
    GcWalk w;
    uint32_t off, size, n_before, n_broken, last;
    uint8_t type;
    CL_Obj a, b;
    uint32_t saved;

    a = cl_make_vector(16);
    CL_GC_PROTECT(a);
    b = cl_make_vector(16);
    CL_GC_PROTECT(b);

    n_before = 0;
    gc_walk_begin(&w);
    while (gc_walk_next(&w, &off, &size, &type)) n_before++;
    ASSERT(gc_walk_complete(&w));

    saved = CL_HDR(CL_OBJ_TO_PTR(b));
    CL_HDR(CL_OBJ_TO_PTR(b)) = 0;             /* size 0: cannot be stepped */

    n_broken = 0;
    last = 0;
    gc_walk_begin(&w);
    while (gc_walk_next(&w, &off, &size, &type)) { n_broken++; last = off; }
    ASSERT(!gc_walk_complete(&w));
    ASSERT_EQ_INT((int)w.off, (int)b);         /* stopped AT the bad header */
    ASSERT_EQ_INT((int)last, (int)a);          /* the block before it was the last yielded */
    ASSERT(n_broken < n_before);

    CL_HDR(CL_OBJ_TO_PTR(b)) = saved;
    gc_walk_begin(&w);
    while (gc_walk_next(&w, &off, &size, &type)) ;
    ASSERT(gc_walk_complete(&w));
    CL_GC_UNPROTECT(2);
}
#endif

int main(void)
{
    setup();

    RUN(walk_covers_fresh_allocations);
#ifdef CL_GC_CLASSIC
    RUN(walk_sees_free_blocks_after_sweep);
    RUN(walk_steps_over_a_bare_gap_header);
#endif
    RUN(walk_after_compaction_is_dense);
    RUN(walk_seek_continues_at_the_given_block);
#if !defined(DEBUG_GC) && !defined(DEBUG_GC_STRESS)
    RUN(walk_stops_at_a_broken_header);
#endif

    teardown();
    REPORT();
}
