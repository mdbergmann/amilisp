#include "test.h"
#include "core/types.h"
#include "core/mem.h"
#include "core/gc_backend.h"
#include "core/error.h"
#include "core/package.h"
#include "core/symbol.h"
#include "core/thread.h"
#include "platform/platform.h"

#include <stdlib.h>
#include <string.h>

/* The Immix-style backend (src/core/gc_immix.c, specs/gc-backend-immix.md
 * Part 2): trivial pass under another backend. */
#ifdef CL_GC_IMMIX

/*
 * What these tests pin down:
 *
 *  - allocation is a cursor cut from a HOLE (a run of available lines)
 *    claimed as the thread's chunk; the first claim of a fresh heap starts
 *    at line 1 (line 0 holds NIL's slot) behind a dead filler, and the
 *    objects cut from a chunk are contiguous;
 *  - a cycle stamps the lines of live objects with the epoch and zeroes
 *    the rest; the next allocations reuse the lowest hole, below the bump
 *    front;
 *  - the header chain stays exact across a claim: a dead object that
 *    starts in the live line before a hole is truncated at the hole, one
 *    that starts inside and ends in the live line after it is split there,
 *    and the block-start index agrees with the chain afterwards;
 *  - a hole too small for a request stays available for the next one; a
 *    request larger than a chunk gets a run of its own and leaves the
 *    cursor alone;
 *  - the mark sense flips every cycle: survivors and newborns read
 *    unmarked under either sense;
 *  - a dead finalizable object is finalized by a plain cycle, exactly
 *    once, and the list survives a compaction and an image adoption;
 *  - fragmentation that defeats every hole makes the allocator compact,
 *    and a genuinely full heap is a storage error, not a hang;
 *  - the heap re-initialises with new tables.
 */

static void setup(void)
{
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

#define LINE_OF(off)  ((uint32_t)(off) >> CL_GC_LINE_SHIFT)
#define LINE          CL_GC_LINE_SIZE
#define LINE_BYTE(l)  (im_line_map[(l)])
#define DEAD_AT(off)  (CL_HDR_TYPE(cl_heap.arena + (off)) == CL_TYPE_DEAD)

/* Walk the whole arena with the iterator and check the chain contract:
 * contiguous blocks from CL_ALIGN to the bump front. */
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

/* The chain and the block-start index agree (0 violations). */
#define CHECKED_INDEX() ASSERT_EQ_INT(cl_gc_audit_hdr_index(), 0)

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

/* Compact, then allocate up to a line boundary with at least 8 KB of the
 * chunk left after it: the returned cons is the first object of a line,
 * everything below it is live (the padding conses hang off its cdr), and
 * the pattern a test builds after it stays inside one chunk.  The tests
 * build their line patterns from there. */
static CL_Obj fresh_line_start(void)
{
    CL_Thread *t = cl_get_current_thread();
    CL_Obj c = CL_NIL;
    int i;
    cl_gc_compact();
    CL_GC_PROTECT(c);
    for (i = 0; i < 4096; i++) {
        c = cl_cons(CL_MAKE_FIXNUM(0), c);
        if (((uint32_t)c & (LINE - 1)) == 0 && t->alloc_lim - (uint32_t)c >= 8192)
            break;
    }
    CL_GC_UNPROTECT(1);
    return c;
}

/* A vector whose block is exactly one line. */
static uint32_t line_vector_elts(void)
{
    uint32_t n;
    for (n = 1; n < 64; n++) {
        CL_Obj v = cl_make_vector(n);
        if (CL_HDR_SIZE(CL_OBJ_TO_PTR(v)) == LINE)
            return n;
    }
    return 0;
}

/* --- Claims and cuts --- */

TEST(fresh_heap_claims_from_the_front)
{
    CL_Thread *t = cl_get_current_thread();
    uint32_t size, total;
    uint8_t type;
    CL_Obj c;

    /* A clean heap of our own: nothing allocated yet. */
    cl_mem_shutdown();
    cl_mem_init(CL_DEFAULT_HEAP_SIZE);
    ASSERT_EQ_INT((int)cl_heap.bump, CL_ALIGN);
    ASSERT_EQ_INT(LINE_BYTE(0), im_epoch);           /* NIL's line: never a hole */
    ASSERT_EQ_INT(LINE_BYTE(1), 0);

    /* The hole is lines 1.., but the claim begins at the chain's end so
     * the first object sits exactly where the bump allocator put it. */
    c = cl_cons(CL_MAKE_FIXNUM(1), CL_NIL);
    ASSERT_EQ_INT((int)c, CL_ALIGN);
    ASSERT(walk_find((uint32_t)c, &size, &type));
    ASSERT_EQ_INT(type, TYPE_CONS);

    /* The chunk: cursor right after the cons, remainder a dead block up
     * to the chunk's end, which is the bump front. */
    ASSERT_EQ_INT((int)t->alloc_cur, (int)(c + size));
    ASSERT_EQ_INT((int)t->alloc_lim, (int)cl_heap.bump);
    ASSERT(DEAD_AT(t->alloc_cur));
    ASSERT_EQ_INT(LINE_BYTE(1), im_epoch);           /* claimed: unavailable */
    ASSERT_EQ_INT(LINE_BYTE(LINE_OF(cl_heap.bump - 1)), im_epoch);
    ASSERT_EQ_INT(LINE_BYTE(LINE_OF(cl_heap.bump)), 0);
    CHECKED_WALK(total);
    CHECKED_INDEX();
    ASSERT_EQ_INT((int)cl_heap.tlab_refills, 1);

    cl_package_init();
    cl_symbol_init();
}

TEST(cuts_within_a_chunk_are_contiguous)
{
    CL_Obj prev, c;
    uint32_t total, refills;
    int i;

    c = fresh_line_start();
    refills = cl_heap.tlab_refills;
    for (i = 0; i < 200; i++) {
        prev = c;
        c = cl_cons(CL_MAKE_FIXNUM(i), CL_NIL);
        ASSERT_EQ_INT((int)c, (int)(prev + CL_HDR_SIZE(CL_OBJ_TO_PTR(prev))));
    }
    ASSERT_EQ_INT((int)cl_heap.tlab_refills, (int)refills);   /* one chunk */
    CHECKED_WALK(total);
    CHECKED_INDEX();
}

/* --- A cycle --- */

TEST(cycle_frees_unstamped_lines_and_the_cursor_reuses_them)
{
    CL_Obj keep = CL_NIL, c;
    uint32_t bump_before, total, l;
    int i;

    CL_GC_PROTECT(keep);
    fresh_line_start();
    /* 4000 conses (64 KB, two chunks); one in the middle survives. */
    for (i = 0; i < 4000; i++) {
        c = cl_cons(CL_MAKE_FIXNUM(i), CL_NIL);
        if (i == 2000) keep = c;
    }
    bump_before = cl_heap.bump;
    cl_gc();

    /* The survivor's line is stamped, the dead ones around it zeroed. */
    ASSERT_EQ_INT(LINE_BYTE(LINE_OF(keep)), im_epoch);
    ASSERT_EQ_INT(LINE_BYTE(LINE_OF(keep) - 2), 0);
    ASSERT_EQ_INT(LINE_BYTE(LINE_OF(keep) + 2), 0);
    ASSERT(CL_CONS_P(keep));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(keep)), 2000);

    /* The next allocation lands in a hole below the front, and its
     * line was available a moment ago. */
    c = cl_cons(CL_MAKE_FIXNUM(-1), CL_NIL);
    ASSERT((uint32_t)c < bump_before);
    ASSERT_EQ_INT((int)cl_heap.bump, (int)bump_before);
    l = LINE_OF(c);
    ASSERT_EQ_INT(LINE_BYTE(l), im_epoch);
    ASSERT_EQ_INT((int)((uint32_t)c & (LINE - 1)), 0);   /* a hole starts on a line */
    CHECKED_WALK(total);
    CHECKED_INDEX();
    CL_GC_UNPROTECT(1);
}

TEST(mark_sense_flips_and_newborns_read_unmarked)
{
    CL_Obj keep, born;
    uint32_t xor0 = gc_hdr_xor;

    keep = cl_cons(CL_MAKE_FIXNUM(1), CL_NIL);
    CL_GC_PROTECT(keep);
    ASSERT(!GCB_HDR_MARKED(CL_OBJ_TO_PTR(keep)));

    cl_gc();
    ASSERT(gc_hdr_xor != xor0);                      /* the sense flipped */
    ASSERT(!GCB_HDR_MARKED(CL_OBJ_TO_PTR(keep)));    /* survivor: unmarked again */
    /* ... though its raw bit is what the trace set under the OLD sense: */
    ASSERT_EQ_INT((int)(CL_HDR(CL_OBJ_TO_PTR(keep)) & CL_HDR_MARK_BIT),
                  (int)(CL_HDR_MARK_BIT ^ xor0));

    born = cl_cons(CL_MAKE_FIXNUM(2), keep);
    CL_GC_PROTECT(born);
    ASSERT(!GCB_HDR_MARKED(CL_OBJ_TO_PTR(born)));    /* born unmarked under this sense */

    cl_gc();
    ASSERT_EQ_INT((int)gc_hdr_xor, (int)xor0);       /* and back */
    ASSERT(!GCB_HDR_MARKED(CL_OBJ_TO_PTR(keep)));
    ASSERT(!GCB_HDR_MARKED(CL_OBJ_TO_PTR(born)));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(born)), 2);
    ASSERT(cl_cdr(born) == keep);

    /* A compaction normalises: raw bits clear, sense 0. */
    cl_gc_compact();
    ASSERT_EQ_INT((int)gc_hdr_xor, 0);
    ASSERT(!(CL_HDR(CL_OBJ_TO_PTR(keep)) & CL_HDR_MARK_BIT));
    ASSERT(gcb_marks_exact());
    cl_gc();
    ASSERT(!gcb_marks_exact());
    CL_GC_UNPROTECT(2);
}

/* --- The chain across a claim --- */

TEST(spanner_is_truncated_at_the_hole_start)
{
    CL_Obj k1, pad, d, n;
    uint32_t d_off, d_size, size, total;
    uint8_t type;

    k1 = fresh_line_start();                 /* line S, at its start */
    CL_GC_PROTECT(k1);
    pad = cl_make_vector(10);                /* still in line S */
    CL_GC_PROTECT(pad);
    d = cl_make_vector(80);                  /* starts in S, spans into S+2 */
    d_off = (uint32_t)d;
    d_size = CL_HDR_SIZE(CL_OBJ_TO_PTR(d));
    ASSERT_EQ_INT((int)LINE_OF(d_off), (int)LINE_OF(k1));
    ASSERT(LINE_OF(d_off + d_size - 1) >= LINE_OF(k1) + 2);
    ASSERT_EQ_INT((int)LINE_OF(pad + CL_HDR_SIZE(CL_OBJ_TO_PTR(pad)) - 1),
                  (int)LINE_OF(k1));
    d = CL_NIL;

    cl_gc();                                 /* d is dead; S live, S+1.. free */
    ASSERT_EQ_INT(LINE_BYTE(LINE_OF(k1)), im_epoch);
    ASSERT_EQ_INT(LINE_BYTE(LINE_OF(k1) + 1), 0);
    ASSERT(walk_find(d_off, &size, &type));
    ASSERT_EQ_INT((int)size, (int)d_size);   /* the corpse is still whole */

    /* The next claim starts at line S+1 and cuts the corpse short. */
    n = cl_cons(CL_MAKE_FIXNUM(1), CL_NIL);
    CL_GC_PROTECT(n);
    ASSERT_EQ_INT((int)n, (int)((LINE_OF(k1) + 1) << CL_GC_LINE_SHIFT));
    ASSERT(walk_find(d_off, &size, &type));
    ASSERT_EQ_INT(type, (int)CL_TYPE_DEAD);
    ASSERT_EQ_INT((int)size, (int)(n - d_off));
    ASSERT(CL_CONS_P(k1));
    ASSERT_EQ_INT((int)((CL_Vector *)CL_OBJ_TO_PTR(pad))->length, 10);
    CHECKED_WALK(total);
    CHECKED_INDEX();
    CL_GC_UNPROTECT(3);
}

TEST(straddler_is_split_at_the_hole_end)
{
    CL_Obj k1, d, k2, n;
    uint32_t d_off, d_end, hole_end, size, total;
    uint8_t type;

    k1 = fresh_line_start();                 /* line S */
    CL_GC_PROTECT(k1);
    d = cl_make_vector(80);                  /* S+16 .. into S+2 */
    d_off = (uint32_t)d;
    d_end = d_off + CL_HDR_SIZE(CL_OBJ_TO_PTR(d));
    ASSERT_EQ_INT((int)LINE_OF(d_end - 1), (int)(LINE_OF(k1) + 2));
    k2 = cl_cons(CL_MAKE_FIXNUM(2), CL_NIL); /* right after d, in S+2 */
    CL_GC_PROTECT(k2);
    ASSERT_EQ_INT((int)k2, (int)d_end);
    d = CL_NIL;

    cl_gc();                                 /* S live, S+1 free, S+2 live */
    ASSERT_EQ_INT(LINE_BYTE(LINE_OF(k1) + 1), 0);
    ASSERT_EQ_INT(LINE_BYTE(LINE_OF(k1) + 2), im_epoch);

    /* The one-line hole at S+1: the corpse is truncated before it and
     * its tail gets a header at the hole's end. */
    n = cl_cons(CL_MAKE_FIXNUM(1), CL_NIL);
    CL_GC_PROTECT(n);
    hole_end = (LINE_OF(k1) + 2) << CL_GC_LINE_SHIFT;
    ASSERT_EQ_INT((int)n, (int)((LINE_OF(k1) + 1) << CL_GC_LINE_SHIFT));
    ASSERT(walk_find(d_off, &size, &type));
    ASSERT_EQ_INT(type, (int)CL_TYPE_DEAD);
    ASSERT_EQ_INT((int)size, (int)(n - d_off));
    ASSERT(walk_find(hole_end, &size, &type));
    ASSERT_EQ_INT(type, (int)CL_TYPE_DEAD);
    ASSERT_EQ_INT((int)size, (int)(d_end - hole_end));
    ASSERT(walk_find((uint32_t)k2, &size, &type));
    ASSERT_EQ_INT(type, TYPE_CONS);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(k2)), 2);
    CHECKED_WALK(total);
    CHECKED_INDEX();

    /* And the hole is a one-line chunk: the cursor ends at the split. */
    ASSERT_EQ_INT((int)cl_get_current_thread()->alloc_lim, (int)hole_end);
    CL_GC_UNPROTECT(3);
}

/* --- Hole selection --- */

TEST(small_hole_is_skipped_by_a_bigger_request_and_kept_for_a_smaller)
{
    CL_Obj k1, k2, v, c;
    uint32_t hole, total;
    int i;

    k1 = fresh_line_start();                 /* line S */
    CL_GC_PROTECT(k1);
    for (i = 0; i < 15; i++)                 /* S+16 .. S+256: line S+1 dead */
        cl_cons(CL_MAKE_FIXNUM(i), CL_NIL);
    k2 = cl_cons(CL_MAKE_FIXNUM(2), CL_NIL); /* at S+256: line S+2 live */
    CL_GC_PROTECT(k2);
    ASSERT_EQ_INT((int)k2, (int)((LINE_OF(k1) + 2) << CL_GC_LINE_SHIFT));
    hole = (LINE_OF(k1) + 1) << CL_GC_LINE_SHIFT;

    cl_gc();
    ASSERT_EQ_INT(LINE_BYTE(LINE_OF(hole)), 0);

    /* Three lines' worth does not fit the one-line hole: placed beyond
     * k2, the hole left available. */
    v = cl_make_vector(80);
    CL_GC_PROTECT(v);
    ASSERT((uint32_t)v > k2);
    ASSERT_EQ_INT(LINE_BYTE(LINE_OF(hole)), 0);

    /* The cursor is the run the vector was cut from; once it is spent,
     * the next refill takes the small hole. */
    c = CL_NIL;
    CL_GC_PROTECT(c);
    for (i = 0; i < 4000 && (uint32_t)c != hole; i++)
        c = cl_cons(CL_MAKE_FIXNUM(3), CL_NIL);
    ASSERT_EQ_INT((int)c, (int)hole);
    ASSERT_EQ_INT(LINE_BYTE(LINE_OF(hole)), im_epoch);
    CHECKED_WALK(total);
    CHECKED_INDEX();
    CL_GC_UNPROTECT(4);
}

TEST(large_request_gets_its_own_run_and_leaves_the_cursor_alone)
{
    CL_Thread *t = cl_get_current_thread();
    CL_Obj c, v, c2;
    uint32_t cur, lim, v_size, lines, l, total, size;
    uint8_t type;

    c = fresh_line_start();
    CL_GC_PROTECT(c);
    cur = t->alloc_cur;
    lim = t->alloc_lim;
    ASSERT(cur != 0);

    v = cl_make_vector(25000);               /* ~100 KB: above any chunk */
    CL_GC_PROTECT(v);
    v_size = CL_HDR_SIZE(CL_OBJ_TO_PTR(v));
    ASSERT((uint32_t)v >= lim);              /* its own run, past the chunk */
    ASSERT_EQ_INT((int)t->alloc_cur, (int)cur);          /* cursor untouched */
    ASSERT_EQ_INT((int)t->alloc_lim, (int)lim);
    lines = (v_size + LINE - 1) >> CL_GC_LINE_SHIFT;
    for (l = LINE_OF(v); l <= LINE_OF(v + v_size - 1); l++)
        ASSERT_EQ_INT(LINE_BYTE(l), im_epoch);
    ASSERT_EQ_INT(LINE_BYTE(LINE_OF(v + v_size - 1) + 1), 0);
    /* The tail of its last line is a dead block (or absorbed). */
    if (v + v_size < ((LINE_OF(v + v_size - 1) + 1) << CL_GC_LINE_SHIFT)) {
        ASSERT(walk_find(v + v_size, &size, &type));
        ASSERT_EQ_INT(type, (int)CL_TYPE_DEAD);
    }
    (void)lines;
    ASSERT(cl_heap.bump >= v + v_size);

    c2 = cl_cons(CL_MAKE_FIXNUM(2), CL_NIL); /* back in the chunk */
    CL_GC_PROTECT(c2);
    ASSERT_EQ_INT((int)c2, (int)cur);
    CHECKED_WALK(total);
    CHECKED_INDEX();
    ((CL_Vector *)CL_OBJ_TO_PTR(v))->data[24999] = CL_MAKE_FIXNUM(7);
    cl_gc();
    ASSERT_EQ_INT(CL_FIXNUM_VAL(((CL_Vector *)CL_OBJ_TO_PTR(v))->data[24999]), 7);
    CL_GC_UNPROTECT(3);
}

/* --- Finalization --- */

static CL_Obj make_bytecode_with_code(void)
{
    CL_Bytecode *bc = (CL_Bytecode *)cl_alloc(TYPE_BYTECODE, sizeof(CL_Bytecode));
    bc->code = (uint8_t *)platform_alloc(32);
    bc->code_len = 32;
    return CL_PTR_TO_OBJ(bc);
}

TEST(dead_finalizable_object_is_finalized_once_by_a_cycle)
{
    CL_Obj bc;
    uint32_t off, count0, count, bytes, size, total;
    uint8_t type;

    bc = make_bytecode_with_code();
    CL_GC_PROTECT(bc);
    off = (uint32_t)bc;
    cl_gc_offheap_stats(&bytes, &count0);

    cl_gc();                                 /* live: nothing released */
    cl_gc_offheap_stats(&bytes, &count);
    ASSERT_EQ_INT((int)count, (int)count0);
    CL_GC_UNPROTECT(1);
    bc = CL_NIL;

    cl_gc();                                 /* dead: released, neutralised */
    cl_gc_offheap_stats(&bytes, &count);
    ASSERT_EQ_INT((int)count, (int)(count0 + 1));
    ASSERT(walk_find(off, &size, &type));
    ASSERT_EQ_INT(type, (int)CL_TYPE_DEAD);
    ASSERT_EQ_INT((int)size, (int)sizeof(CL_Bytecode) + (int)((CL_ALIGN - sizeof(CL_Bytecode) % CL_ALIGN) % CL_ALIGN));

    cl_gc();                                 /* and never again */
    cl_gc_compact();
    cl_gc_offheap_stats(&bytes, &count);
    ASSERT_EQ_INT((int)count, (int)(count0 + 1));
    CHECKED_WALK(total);
}

TEST(finalizable_list_survives_a_compaction)
{
    CL_Obj bc, filler = CL_NIL;
    uint32_t before, count0, count, bytes;
    int i;

    /* Something dead below the bytecode so the compaction moves it. */
    CL_GC_PROTECT(filler);
    for (i = 0; i < 500; i++)
        filler = cl_cons(CL_MAKE_FIXNUM(i), filler);
    bc = make_bytecode_with_code();
    CL_GC_PROTECT(bc);
    filler = CL_NIL;
    before = (uint32_t)bc;
    cl_gc_compact();
    ASSERT((uint32_t)bc < before);           /* moved */
    cl_gc_offheap_stats(&bytes, &count0);

    CL_GC_UNPROTECT(2);
    bc = CL_NIL;
    cl_gc();                                 /* a plain cycle finds it by its new offset */
    cl_gc_offheap_stats(&bytes, &count);
    ASSERT_EQ_INT((int)count, (int)(count0 + 1));
}

/* --- Image adoption and re-init --- */

TEST(image_adoption_rebuilds_the_tables)
{
    CL_Thread *t = cl_get_current_thread();
    CL_Obj keep = CL_NIL, bc, p;
    uint32_t dense_bump, total, count0, count, bytes, l;
    int i;

    CL_GC_PROTECT(keep);
    for (i = 0; i < 300; i++)
        keep = cl_cons(CL_MAKE_FIXNUM(i), keep);
    bc = make_bytecode_with_code();
    CL_GC_PROTECT(bc);
    cl_gc_compact();                         /* dense [CL_ALIGN, bump): a payload */
    dense_bump = cl_heap.bump;

    cl_cons(CL_MAKE_FIXNUM(-1), CL_NIL);     /* past it */
    ASSERT(cl_heap.bump > dense_bump);
    cl_mem_adopt_image_begin(dense_bump);
    ASSERT(t->alloc_cur == 0 && t->alloc_lim == 0);
    cl_mem_adopt_image_finish();

    /* Lines below the payload's front in use, the rest free. */
    for (l = 0; l < LINE_OF(dense_bump - 1); l++)
        ASSERT_EQ_INT(LINE_BYTE(l), im_epoch);
    ASSERT_EQ_INT(LINE_BYTE(LINE_OF(dense_bump - 1) + 1), 0);
    ASSERT(gcb_marks_exact());

    p = cl_cons(CL_MAKE_FIXNUM(-2), keep);
    CL_GC_PROTECT(p);
    ASSERT((uint32_t)p >= dense_bump);       /* above the adopted payload */
    CHECKED_WALK(total);
    CHECKED_INDEX();
    {
        CL_Obj q = p;
        int n = 0;
        for (; !CL_NULL_P(q); q = cl_cdr(q)) n++;
        ASSERT_EQ_INT(n, 301);
    }

    /* The finalizable list was rebuilt from the payload. */
    cl_gc_offheap_stats(&bytes, &count0);
    CL_GC_UNPROTECT(3);
    bc = CL_NIL;
    cl_gc();
    cl_gc_offheap_stats(&bytes, &count);
    ASSERT_EQ_INT((int)count, (int)(count0 + 1));
    CHECKED_WALK(total);
}

TEST(reinit_sizes_the_tables_for_the_new_arena)
{
    CL_Obj keep = CL_NIL;
    uint32_t total;
    int i;

    cl_mem_shutdown();
    cl_mem_init(1024 * 1024);
    cl_package_init();
    cl_symbol_init();
    CL_GC_PROTECT(keep);
    for (i = 0; i < 20000; i++) {
        CL_Obj c = cl_cons(CL_MAKE_FIXNUM(i), keep);
        if (i % 50 == 0) keep = c;
    }
    cl_gc();
    ASSERT_EQ_INT(LINE_BYTE(LINE_OF(keep)), im_epoch);
    CHECKED_WALK(total);
    CHECKED_INDEX();
    CL_GC_UNPROTECT(1);

    cl_mem_shutdown();
    cl_mem_init(CL_DEFAULT_HEAP_SIZE);
    cl_package_init();
    cl_symbol_init();
    cl_gc();
    CHECKED_WALK(total);
}

/* --- Policy --- */

TEST(fragmentation_that_defeats_every_hole_compacts)
{
    CL_Obj kv, v;
    uint32_t n_elts, pairs, i, compactions, total;
    uint32_t kept = 0;

    n_elts = line_vector_elts();
    ASSERT(n_elts > 0);
    fresh_line_start();

    /* Alternate one-line keepers and one-line garbage until the virgin
     * tail is too small for the request below; the keepers live in a
     * vector allocated first, so nothing else is allocated in between. */
    pairs = (cl_heap.arena_size - cl_heap.bump) / (2 * LINE);
    kv = cl_make_vector(pairs);
    CL_GC_PROTECT(kv);
    /* The tail left virgin plus the last chunk's unused remainder must
     * stay under the 64 KB requested below (a chunk is 32 KB at most). */
    pairs = (cl_heap.arena_size - cl_heap.bump - 16 * 1024) / (2 * LINE);
    for (i = 0; i < pairs; i++) {
        v = cl_make_vector(n_elts);
        ((CL_Vector *)CL_OBJ_TO_PTR(kv))->data[i] = v;
        ((CL_Vector *)CL_OBJ_TO_PTR(v))->data[0] = CL_MAKE_FIXNUM((int32_t)i);
        cl_make_vector(n_elts);              /* garbage */
    }
    compactions = cl_heap.compact_count;

    /* No run of 512 lines anywhere: a cycle cannot help, the compaction
     * does. */
    v = cl_make_vector(16000);               /* 64 KB */
    CL_GC_PROTECT(v);
    ASSERT_EQ_INT((int)cl_heap.compact_count, (int)(compactions + 1));
    for (i = 0; i < pairs; i += 97) {
        CL_Obj k = ((CL_Vector *)CL_OBJ_TO_PTR(kv))->data[i];
        ASSERT(CL_HEAP_P(k));
        ASSERT_EQ_INT(CL_HDR_TYPE(CL_OBJ_TO_PTR(k)), TYPE_VECTOR);
        ASSERT_EQ_INT(CL_FIXNUM_VAL(((CL_Vector *)CL_OBJ_TO_PTR(k))->data[0]), (int)i);
        kept++;
    }
    ASSERT(kept > 0);
    CHECKED_WALK(total);
    CHECKED_INDEX();
    CL_GC_UNPROTECT(2);
}

TEST(a_full_heap_is_a_storage_error)
{
    CL_Obj keep = CL_NIL;
    int err = CL_ERR_NONE;
    int i;

    CL_GC_PROTECT(keep);
    CL_CATCH(err);
    if (err == CL_ERR_NONE) {
        for (i = 0; i < 1000000; i++)
            keep = cl_cons(cl_make_vector(64), keep);
    }
    CL_UNCATCH();
    ASSERT_EQ_INT(err, CL_ERR_STORAGE);
    /* cl_storage_error reset the root stack; the heap is still sane. */
    cl_gc_compact();
    cl_gc();
    {
        uint32_t total;
        CHECKED_WALK(total);
    }
}

int main(void)
{
    setup();

    RUN(fresh_heap_claims_from_the_front);
    RUN(cuts_within_a_chunk_are_contiguous);
    RUN(cycle_frees_unstamped_lines_and_the_cursor_reuses_them);
    RUN(mark_sense_flips_and_newborns_read_unmarked);
    RUN(spanner_is_truncated_at_the_hole_start);
    RUN(straddler_is_split_at_the_hole_end);
    RUN(small_hole_is_skipped_by_a_bigger_request_and_kept_for_a_smaller);
    RUN(large_request_gets_its_own_run_and_leaves_the_cursor_alone);
    RUN(dead_finalizable_object_is_finalized_once_by_a_cycle);
    RUN(finalizable_list_survives_a_compaction);
    RUN(image_adoption_rebuilds_the_tables);
    RUN(reinit_sizes_the_tables_for_the_new_arena);
    RUN(fragmentation_that_defeats_every_hole_compacts);
    RUN(a_full_heap_is_a_storage_error);

    teardown();
    REPORT();
}

#else /* !CL_GC_IMMIX */

TEST(immix_backend_not_built) { ASSERT(1); }
int main(void) { test_init(); RUN(immix_backend_not_built); REPORT(); }

#endif /* CL_GC_IMMIX */
