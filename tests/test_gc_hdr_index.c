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

/* The index is shared (gc_jitscan.c), but these tests drive it through
 * the classic backend's free-list splits and sweeps: trivial pass under
 * another backend. */
#ifdef CL_GC_CLASSIC

/*
 * Tests for the block-start index behind the conservative JIT
 * native-stack scan (gc_jitscan.c, gc_hdr_page[] / gc_hdr_is_block_start).
 *
 * The scan must decide, for every word on the m68k stack that looks
 * like an arena offset, whether it is the START of a block.  Before the
 * index it walked the whole arena bump-front header by header on every
 * collection taken inside JIT'd code — the marker's JIT cost the Clamacs
 * spike measured (mark 80 ms vs 40 ms without the JIT on a 68040-class
 * machine, first collection 361 vs 129 ms).  The index answers the same
 * question from a per-page entry plus at most a page of header walking.
 *
 * Its invariant — every page below the bump front maps to a real header
 * at or below the page's first byte — is maintained by the sweep, the
 * slide, the bump allocator, the free-list splitter and the image
 * adopter, and checked here after each of those with
 * cl_gc_audit_hdr_index() (the same audit ext:%gc-audit-hdr-index
 * exposes to the Amiga suite, where the JIT actually runs).  The scan
 * itself is exercised with the fake-JIT-window technique of
 * test_jit_gc_scan.c: a volatile array below jit_stack_top carries the
 * "spilled" candidates.  Index on and off (CLAMIGA_HDR_INDEX=0, the
 * fallback full walk) must agree.
 */

extern volatile int cl_jit_active_threads;  /* mem.c */

static void setup_with(const char *hdr_index)
{
    /* Classic collector: the index and the free-list snapshot the scan
     * validates against are classic-mode machinery (gen mode keeps no
     * free list and never runs the JIT). */
    setenv("CLAMIGA_GENGC", "0", 1);
    if (hdr_index)
        setenv("CLAMIGA_HDR_INDEX", hdr_index, 1);
    else
        unsetenv("CLAMIGA_HDR_INDEX");
    platform_init();
    cl_thread_init();
    cl_error_init();
    cl_mem_init(CL_DEFAULT_HEAP_SIZE);
    cl_package_init();
    cl_symbol_init();
}

static void teardown(void)
{
    cl_jit_active_threads = 0;
    CT->jit_depth = 0;
    CT->jit_stack_top = NULL;
    cl_mem_shutdown();
    platform_shutdown();
    unsetenv("CLAMIGA_HDR_INDEX");
}

/* Push `o` onto the rooted list *keep (both survive any GC inside). */
static void keep_push(CL_Obj *keep, CL_Obj o)
{
    CL_GC_PROTECT(o);
    *keep = cl_cons_rooted(&o, keep);
    CL_GC_UNPROTECT(1);
}

/* Allocate a mix of block sizes: conses, strings of varying length and a
 * few page-spanning vectors.  Every `keep_every`-th object is retained. */
static void churn(CL_Obj *keep, int n, int keep_every, int salt)
{
    static const char pad[64] =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    int i;
    for (i = 0; i < n; i++) {
        CL_Obj o;
        int k = i + salt;
        if (k % 97 == 0)
            o = cl_make_vector(2000u + (uint32_t)(k % 5) * 700u); /* 8-19 KB */
        else if (k % 7 == 0)
            o = cl_make_string(pad, (uint32_t)(k % 60) + 1);
        else
            o = cl_cons(CL_MAKE_FIXNUM(k), CL_NIL);
        if (i % keep_every == 0)
            keep_push(keep, o);
    }
}

static int free_list_contains(uint32_t off)
{
    uint32_t cur;
    for (cur = cl_heap.free_list; cur;
         cur = ((CL_FreeBlock *)(cl_arena_base + cur))->next_offset)
        if (cur == off) return 1;
    return 0;
}

/* --- The invariant holds through every pass that writes headers. --- */

TEST(index_clean_through_alloc_sweep_split_compact)
{
    CL_Obj keep = CL_NIL;

    setup_with(NULL);
    CL_GC_PROTECT(keep);

    ASSERT_EQ_INT(cl_gc_audit_hdr_index(), 0);      /* fresh arena */

    churn(&keep, 6000, 3, 0);                        /* bump allocation */
    ASSERT_EQ_INT(cl_gc_audit_hdr_index(), 0);

    cl_gc();                                         /* sweep coalesces */
    ASSERT_EQ_INT(cl_gc_audit_hdr_index(), 0);

    churn(&keep, 4000, 2, 5);                        /* free-list splits */
    ASSERT_EQ_INT(cl_gc_audit_hdr_index(), 0);

    cl_gc_compact();                                 /* slide, bump reset */
    ASSERT_EQ_INT(cl_gc_audit_hdr_index(), 0);

    churn(&keep, 3000, 4, 11);                       /* bump again */
    ASSERT_EQ_INT(cl_gc_audit_hdr_index(), 0);

    cl_gc();
    ASSERT_EQ_INT(cl_gc_audit_hdr_index(), 0);

    CL_GC_UNPROTECT(1);
    teardown();
}

/* --- CLAMIGA_HDR_INDEX=0: no index, the scan's full-walk fallback still
 * keeps a JIT-only-referenced object alive. --- */

TEST(index_absent_when_disabled_scan_falls_back)
{
    volatile CL_Obj buf[4];
    CL_Obj alive;

    setup_with("0");
    ASSERT_EQ_INT(cl_gc_audit_hdr_index(), -1);

    alive = cl_cons(CL_MAKE_FIXNUM(7), CL_MAKE_FIXNUM(11));
    buf[0] = 0;
    buf[1] = alive;
    buf[2] = 0;
    buf[3] = 0;
    CT->jit_stack_top = (void *)((char *)&buf[3] + sizeof(buf[0]) + 16);
    CT->jit_depth = 1;
    cl_jit_active_threads = 1;

    cl_gc();

    ASSERT(CL_CONS_P(alive));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(alive)), 7);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_cdr(alive)), 11);
    ASSERT_EQ_INT(cl_gc_audit_hdr_index(), -1);

    (void)buf;
    teardown();
}

/* --- With the index, on a fragmented heap: a JIT-only reference marks
 * and pins its object across a compaction; interior offsets (a page's
 * first byte inside a big string, a slot inside a cons) and a free
 * block's offset are rejected — no phantom mark lands in their data. --- */

TEST(scan_with_index_marks_pins_rejects_interior_and_free)
{
    volatile CL_Obj buf[8];
    CL_Obj keep = CL_NIL;
    CL_Obj big, l1, l2, victim, alive;
    uint32_t alive_off, free_off, page_inside_big;
    CL_String *s;
    uint32_t i;

    setup_with(NULL);
    CL_GC_PROTECT(keep);

    churn(&keep, 3000, 2, 0);                        /* fragmentation */

    /* A string spanning several pages, filled with 'x'. */
    {
        char *fill = (char *)malloc(12000);
        memset(fill, 'x', 12000);
        big = cl_make_string(fill, 12000);
        free(fill);
    }
    CL_GC_PROTECT(big);
    /* The first byte of the next page inside the string: the index entry
     * for that page is big's start, and this offset is not a block. */
    page_inside_big = (((uint32_t)big + 4096u) & ~4095u);
    ASSERT(page_inside_big > (uint32_t)big + 16);
    ASSERT(page_inside_big < (uint32_t)big + 12000);

    /* Live neighbours around a victim so its exact offset reappears on
     * the free list after the sweep. */
    l1 = cl_cons(CL_MAKE_FIXNUM(1), CL_NIL);
    CL_GC_PROTECT(l1);
    victim = cl_cons(CL_MAKE_FIXNUM(2), CL_NIL);
    l2 = cl_cons(CL_MAKE_FIXNUM(3), CL_NIL);
    CL_GC_PROTECT(l2);
    free_off = (uint32_t)victim;
    victim = CL_NIL;

    cl_gc();                                          /* victim -> free list */
    ASSERT(free_list_contains(free_off));
    ASSERT_EQ_INT(cl_gc_audit_hdr_index(), 0);

    /* The object only the "JIT frame" references. */
    alive = cl_cons(CL_MAKE_FIXNUM(7), CL_MAKE_FIXNUM(11));
    alive_off = (uint32_t)alive;

    buf[0] = 0;
    buf[1] = alive;                                   /* real block start */
    buf[2] = (CL_Obj)page_inside_big;                 /* interior, page-aligned */
    buf[3] = (CL_Obj)((uint32_t)alive + 8);           /* interior: cdr slot */
    buf[4] = (CL_Obj)free_off;                        /* free block */
    buf[5] = (CL_Obj)((uint32_t)big + 24);            /* interior, same page */
    buf[6] = 0;
    buf[7] = 0;
    CT->jit_stack_top = (void *)((char *)&buf[7] + sizeof(buf[0]) + 16);
    CT->jit_depth = 1;
    cl_jit_active_threads = 1;

    /* Sweep first: the free block must stay free, alive must survive. */
    cl_gc();
    ASSERT(free_list_contains(free_off));
    ASSERT(!(((CL_Header *)(cl_arena_base + free_off))->header & CL_HDR_MARK_BIT));
    ASSERT(CL_CONS_P(alive));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(alive)), 7);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_cdr(alive)), 11);   /* no phantom mark in the cdr */
    s = (CL_String *)CL_OBJ_TO_PTR(big);
    ASSERT_EQ_INT((int)s->length, 12000);
    for (i = 0; i < 12000; i++)
        if (s->data[i] != 'x') break;
    ASSERT_EQ_INT((int)i, 12000);                      /* no phantom mark in the string */
    ASSERT_EQ_INT(cl_gc_audit_hdr_index(), 0);

    /* Then compact: alive is pinned (same offset), the string moves
     * intact, the pin's gap is a valid free block and the index is clean
     * over the slid heap. */
    cl_gc_compact();
    ASSERT(CL_CONS_P(alive));
    ASSERT_EQ_INT((int)(uint32_t)alive, (int)alive_off);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(alive)), 7);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_cdr(alive)), 11);
    ASSERT(CL_STRING_P(big));
    s = (CL_String *)CL_OBJ_TO_PTR(big);
    ASSERT_EQ_INT((int)s->length, 12000);
    for (i = 0; i < 12000; i++)
        if (s->data[i] != 'x') break;
    ASSERT_EQ_INT((int)i, 12000);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(l1)), 1);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(l2)), 3);
    ASSERT_EQ_INT(cl_gc_audit_hdr_index(), 0);

    /* Frame gone: a plain sweep now reclaims alive, index still clean. */
    CT->jit_depth = 0;
    CT->jit_stack_top = NULL;
    cl_jit_active_threads = 0;
    cl_gc();
    ASSERT(free_list_contains(alive_off));
    ASSERT_EQ_INT(cl_gc_audit_hdr_index(), 0);

    (void)buf;
    CL_GC_UNPROTECT(4);
    teardown();
}

/* --- A large free block carved from the front into thousands of small
 * objects (the split-behind case): the index entry for a page deep in
 * the former block may still point at the block's original start, and
 * the scan must still validate a cons at the far end by walking headers
 * from there — and keep it alive. --- */

TEST(scan_with_index_finds_object_deep_in_carved_block)
{
    volatile CL_Obj buf[4];
    CL_Obj keep = CL_NIL;
    CL_Obj big, last = CL_NIL, guard, plug;
    uint32_t big_off, last_off;
    int i;

    setup_with(NULL);
    CL_GC_PROTECT(keep);
    CL_GC_PROTECT(last);

    /* A guard object after the block keeps it from coalescing into the
     * bump front; 200 KB then dies and becomes one free block. */
    {
        char *fill = (char *)malloc(200000);
        memset(fill, 'y', 200000);
        big = cl_make_string(fill, 200000);
        free(fill);
    }
    CL_GC_PROTECT(big);
    big_off = (uint32_t)big;
    guard = cl_cons(CL_MAKE_FIXNUM(1), CL_NIL);
    CL_GC_PROTECT(guard);
    big = CL_NIL;
    cl_gc();
    ASSERT(free_list_contains(big_off));
    ASSERT_EQ_INT(cl_gc_audit_hdr_index(), 0);

    /* The allocator tries the bump front first: plug it with one vector
     * that takes all but a sliver of the remaining bump space, so the
     * conses below can only come from the free list. */
    {
        uint32_t room = cl_heap.arena_size - cl_heap.bump;
        ASSERT(room > 8192);
        plug = cl_make_vector((room - 4096) / sizeof(CL_Obj));
        CL_GC_PROTECT(plug);
        ASSERT(cl_heap.arena_size - cl_heap.bump < 4096 + 64);
    }

    /* Carve ~8000 conses (first-fit: the big block's front, page after
     * page); `last` is the deepest one that landed inside it. */
    for (i = 0; i < 8000; i++) {
        CL_Obj o = cl_cons(CL_MAKE_FIXNUM(i), CL_NIL);
        if (i % 2 == 0) keep_push(&keep, o);
        if ((uint32_t)o > big_off + 4096 && (uint32_t)o < big_off + 200000)
            last = o;
    }
    ASSERT(!CL_NULL_P(last));
    last_off = (uint32_t)last;
    ASSERT_EQ_INT(cl_gc_audit_hdr_index(), 0);

    buf[0] = 0;
    buf[1] = last;
    buf[2] = (CL_Obj)(last_off + 4);                   /* interior: car slot */
    buf[3] = 0;
    CT->jit_stack_top = (void *)((char *)&buf[3] + sizeof(buf[0]) + 16);
    CT->jit_depth = 1;
    cl_jit_active_threads = 1;

    {
        int32_t val = CL_FIXNUM_VAL(cl_car(last));
        cl_gc();
        ASSERT(CL_CONS_P(last));
        ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(last)), val);
        ASSERT_EQ_INT(cl_gc_audit_hdr_index(), 0);

        cl_gc_compact();
        ASSERT(CL_CONS_P(last));
        ASSERT_EQ_INT((int)(uint32_t)last, (int)last_off);   /* pinned */
        ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(last)), val);
        ASSERT_EQ_INT(cl_gc_audit_hdr_index(), 0);
    }

    (void)buf;
    CL_GC_UNPROTECT(5);
    teardown();
}

/* --- Heap re-initialization re-derives the index for the new arena
 * (stale-static bug class): a second cl_mem_init after use must start
 * clean and stay clean. --- */

TEST(index_rederived_on_heap_reinit)
{
    CL_Obj keep = CL_NIL;

    setup_with(NULL);
    CL_GC_PROTECT(keep);
    churn(&keep, 3000, 3, 0);
    cl_gc();
    ASSERT_EQ_INT(cl_gc_audit_hdr_index(), 0);
    CL_GC_UNPROTECT(1);

    cl_mem_shutdown();
    cl_mem_init(CL_DEFAULT_HEAP_SIZE / 2);            /* smaller arena */
    cl_package_init();
    cl_symbol_init();
    ASSERT_EQ_INT(cl_gc_audit_hdr_index(), 0);
    keep = CL_NIL;
    CL_GC_PROTECT(keep);
    churn(&keep, 3000, 3, 7);
    ASSERT_EQ_INT(cl_gc_audit_hdr_index(), 0);
    cl_gc_compact();
    ASSERT_EQ_INT(cl_gc_audit_hdr_index(), 0);
    CL_GC_UNPROTECT(1);
    teardown();
}

int main(void)
{
    test_init();
    RUN(index_clean_through_alloc_sweep_split_compact);
    RUN(index_absent_when_disabled_scan_falls_back);
    RUN(scan_with_index_marks_pins_rejects_interior_and_free);
    RUN(scan_with_index_finds_object_deep_in_carved_block);
    RUN(index_rederived_on_heap_reinit);
    REPORT();
}

#else /* !CL_GC_CLASSIC */

TEST(classic_backend_not_built) { ASSERT(1); }
int main(void) { test_init(); RUN(classic_backend_not_built); REPORT(); }

#endif /* CL_GC_CLASSIC */
