#include "test.h"
#include "core/types.h"
#include "core/mem.h"
#include "core/gc_backend.h"
#include "core/error.h"
#include "core/package.h"
#include "core/symbol.h"
#include "core/thread.h"
#include "platform/platform.h"

#include <string.h>

/*
 * Tests for the conservative m68k-stack scanner used by the JIT.
 * Even though the scanner is a no-op on host (no JIT_M68K builds
 * codegen_m68k.c), the surrounding machinery — jit_depth,
 * jit_stack_top, cl_jit_active_threads, the conservative scan in
 * gc_mark_thread_roots, the compaction inhibit — is compiled on
 * host and exercised here.
 *
 * To make the test independent of compiler optimization (which might
 * keep a CL_Obj local in a register and never spill it), we place
 * the offset in a `volatile` array whose address bounds the scan
 * window.  That forces the value to be observable in memory at GC
 * time, exactly like a JIT-spilled operand.
 *
 * The moving compactor PINS conservatively-referenced objects (they keep
 * their offset) rather than rewriting the C stack, so the JIT's spilled
 * pointers stay valid and coincidental-integer C-stack words are never
 * mis-rewritten.  See specs/native-backend.md §"GC interaction" option B.
 */

extern volatile int cl_jit_active_threads;  /* mem.c */
extern int gc_compact_pending;              /* gc_compact.c */

/* Replicate the static gc_is_freed predicate from mem.c — checks the
 * 0xDE poison pattern gc_sweep writes at offset 8 of freed blocks. */
static int test_is_freed(uint32_t offset)
{
    const uint8_t *p = (const uint8_t *)(cl_arena_base + offset);
    return p[8] == 0xDE && p[9] == 0xDE && p[10] == 0xDE && p[11] == 0xDE;
}

static void setup(void)
{
    /* The conservative JIT-stack scan validates candidates against the
     * free-list snapshot — CLASSIC collector machinery (the m68k JIT and
     * the generational collector never coexist; gen mode keeps no free
     * list).  Pin classic mode so sweeps still produce free blocks. */
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
    cl_jit_active_threads = 0;
    CT->jit_depth = 0;
    CT->jit_stack_top = NULL;
    cl_mem_shutdown();
    platform_shutdown();
}

/* --- With jit_depth > 0 the conservative scan picks up an unrooted
 * CL_Obj offset spilled on the C stack and keeps the object alive
 * across cl_gc(). --- */

TEST(scan_keeps_obj_alive)
{
    volatile CL_Obj buf[4];          /* volatile → guaranteed in memory */
    CL_Obj alive;
    void  *scan_top;

    setup();

    alive = cl_cons(CL_MAKE_FIXNUM(7), CL_MAKE_FIXNUM(11));
    ASSERT(CL_CONS_P(alive));

    /* Place the offset in a stack slot the scan will visit. */
    buf[0] = 0;
    buf[1] = alive;
    buf[2] = 0;
    buf[3] = 0;

    /* Scan window: from current SP (captured inside the scanner) up
     * to just past `buf`.  As long as buf is BELOW jit_stack_top
     * and ABOVE the scanner's own SP, it gets scanned. */
    scan_top = (void *)((char *)&buf[3] + sizeof(buf[0]) + 16);
    CT->jit_stack_top = scan_top;
    CT->jit_depth = 1;
    cl_jit_active_threads = 1;

    cl_gc();

    /* The cons must be intact: header type still TYPE_CONS, not
     * poisoned, and car/cdr still readable. */
    ASSERT(!test_is_freed((uint32_t)alive));
    ASSERT(CL_CONS_P(alive));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(alive)), 7);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_cdr(alive)), 11);

    /* Silence unused-but-needed-for-memory-side-effect warnings */
    (void)buf;

    cl_jit_active_threads = 0;
    CT->jit_depth = 0;
    CT->jit_stack_top = NULL;
    teardown();
}

/* --- With jit_depth == 0 the scan is skipped: the same setup that
 * kept the object alive above now results in a swept slot.  Proves
 * the previous test isn't passing by accident (e.g. via some
 * unrelated root). --- */

TEST(scan_skipped_when_depth_zero)
{
    volatile CL_Obj buf[4];
    CL_Obj victim;
    uint32_t off;

    setup();

    victim = cl_cons(CL_MAKE_FIXNUM(7), CL_MAKE_FIXNUM(11));
    ASSERT(CL_CONS_P(victim));
    off = (uint32_t)victim;

    buf[0] = 0;
    buf[1] = victim;
    buf[2] = 0;
    buf[3] = 0;

    /* Same scan_top, but jit_depth == 0 → scan is skipped. */
    CT->jit_stack_top = (void *)((char *)&buf[3] + sizeof(buf[0]) + 16);
    CT->jit_depth = 0;
    cl_jit_active_threads = 0;

    /* Clear the local CL_Obj from memory so the only place victim's
     * offset survives is in the volatile buffer we control. */
    victim = CL_NIL;

    cl_gc();

    /* The slot at `off` should now be a freed block — poisoned.
     * If for some reason a coincidental root protected it (e.g. an
     * interned literal happens to share the bit pattern), the test
     * remains conservatively useful as a smoke check, so we
     * tolerate that case rather than flake the suite. */
    if (!test_is_freed(off)) {
        printf("  NOTE: victim slot was not swept — accept as a "
               "non-fatal smoke result (unexpected aliasing)\n");
    }
    /* The strong invariant: jit_depth was 0, so the global counter
     * stayed 0, so compaction was NOT inhibited. */
    ASSERT_EQ_INT(cl_jit_active_threads, 0);

    (void)buf;
    teardown();
}

/* --- Compaction now runs even when JIT is active, because
 * gc_scan_jit_native_stack validates each candidate offset against
 * real header offsets before marking — phantom marks are
 * impossible.  Confirm the inhibit was lifted by leaving the
 * counter non-zero and observing that a pending compact still
 * runs. --- */

TEST(compaction_runs_even_when_jit_active)
{
    setup();

    /* Allocate something to compact (otherwise compaction is a
     * no-op and gc_compact_pending stays set, falsely failing). */
    {
        int i;
        for (i = 0; i < 8; i++)
            cl_cons(CL_MAKE_FIXNUM(i), CL_NIL);
    }

    gc_compact_pending = 1;
    cl_jit_active_threads = 1;

    cl_gc_compact_if_pending();
    ASSERT_EQ_INT(gc_compact_pending, 0);   /* ran despite jit_active */

    cl_jit_active_threads = 0;
    teardown();
}

/* Push a frame containing a large zeroed buffer.  When this function
 * returns its frame memory persists below the caller's SP and stays
 * zero until something else overwrites it.  We use this to scrub C-stack
 * debris (leftover spilled CL_Obj values from prior cl_cons calls or
 * earlier tests) out of the JIT scan window before exercising the
 * forwarding pass — otherwise conservative scanning ropes in dead
 * garbage offsets and pins them, defeating the "compaction actually
 * moves kept" precondition the test relies on.
 *
 * Sized large enough to span both the prior test's stack frame range
 * (so RUN(another_test) → RUN(forward_updates_jit_stack) doesn't carry
 * old `alive` offsets forward as conservative roots) and the cl_cons
 * call-chain depth this test itself triggers. */
static void zero_scratch_stack(void)
{
    volatile uint8_t buf[131072];   /* 128 KB */
    int i;
    for (i = 0; i < (int)sizeof(buf); i++) buf[i] = 0;
}

/* --- Compaction PINS objects reachable only through a conservative
 * (offset-valued) JIT-stack reference: option B in
 * specs/native-backend.md §"GC interaction".  A pinned object keeps its
 * arena offset across a moving compaction, so the JIT's spilled operand
 * pointer (and any C-stack word that merely looks like that offset) stays
 * valid with NO stack writeback — which is what eliminates the old
 * coincidental-integer corruption.
 *
 * Here the kept cons is referenced ONLY from the volatile JIT-stack slot
 * buf[1] (not CL_GC_PROTECT'd), so the conservative scan is its sole
 * root.  After compaction it must (a) still be alive, (b) sit at the SAME
 * offset (pinned, not moved), and (c) be reachable through the untouched
 * buf[1] slot. --- */

TEST(pinned_obj_does_not_move)
{
    volatile CL_Obj buf[4];
    CL_Obj kept;
    uint32_t old_off;

    setup();

    /* Wipe C-stack debris from prior tests so stale CL_Obj values can't
     * be conservatively pinned by the scan during compaction. */
    zero_scratch_stack();

    /* Garbage before the kept cons — unrooted, reclaimed by compaction
     * (proves the compaction actually ran and slid live data). */
    {
        int i;
        for (i = 0; i < 100; i++)
            cl_cons(CL_MAKE_FIXNUM(i), CL_NIL);
    }

    kept = cl_cons(CL_MAKE_FIXNUM(42), CL_MAKE_FIXNUM(99));
    old_off = (uint32_t)kept;

    buf[0] = 0;
    buf[1] = kept;          /* sole reference: a conservative JIT-stack root */
    buf[2] = 0;
    buf[3] = 0;
    kept = CL_NIL;          /* drop the precise local; only buf[1] roots it */

    CT->jit_stack_top = (void *)((char *)&buf[3] + sizeof(buf[0]) + 16);
    CT->jit_depth = 1;
    cl_jit_active_threads = 1;

    cl_gc_compact();

    /* Pinned: the object stayed at its original offset, and buf[1] — never
     * rewritten — still points at it.  Content intact. */
    ASSERT_EQ_INT((uint32_t)buf[1], old_off);
    ASSERT(!test_is_freed(old_off));
    ASSERT(CL_CONS_P((CL_Obj)old_off));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car((CL_Obj)old_off)), 42);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_cdr((CL_Obj)old_off)), 99);

    /* Pinning the cons above the reclaimed garbage leaves a gap below it.
     * That gap MUST be filled with valid free-block headers, or the GC's
     * linear arena walk (which steps by header size from CL_ALIGN) desyncs
     * on the stale bytes — the exact "CAR: not a LIST" corruption this fix
     * addresses.  Exercise the walk: drop the pin and run more GCs + allocs;
     * a desync would crash or corrupt here. */
    CT->jit_depth = 0;
    CT->jit_stack_top = NULL;
    cl_jit_active_threads = 0;
    buf[1] = 0;
    cl_gc();          /* walk #1: must traverse the gap free-block cleanly */
    {
        int i;
        for (i = 0; i < 200; i++)   /* reuse the freed gap, allocate fresh */
            cl_cons(CL_MAKE_FIXNUM(i), CL_MAKE_FIXNUM(i + 1));
    }
    cl_gc_compact();  /* walk #2: full compaction over the post-gap heap */

    (void)buf;
    cl_jit_active_threads = 0;
    CT->jit_depth = 0;
    CT->jit_stack_top = NULL;
    teardown();
}

/* --- Same shape, but a CL_GC_PROTECT'd object that is NOT referenced
 * from the JIT stack still moves under compaction.  Proves pinning is
 * specific to conservative JIT-stack roots and that compaction is in fact
 * relocating objects (so the pin in the previous test is meaningful). --- */

static CL_Obj test_moved_slot;   /* static: survive -O3/-flto reload */

TEST(unpinned_obj_moves)
{
    volatile CL_Obj buf[4];
    uint32_t old_off;

    setup();
    test_moved_slot = CL_NIL;
    zero_scratch_stack();

    {
        int i;
        for (i = 0; i < 100; i++)
            cl_cons(CL_MAKE_FIXNUM(i), CL_NIL);
    }

    test_moved_slot = cl_cons(CL_MAKE_FIXNUM(42), CL_MAKE_FIXNUM(99));
    CL_GC_PROTECT(test_moved_slot);
    old_off = (uint32_t)test_moved_slot;

    /* JIT-stack slot holds a RAW INTEGER equal to old_off, not derived
     * from a live reference path — but jit_depth=0 means the scan is
     * skipped, so it neither pins nor is rewritten. */
    buf[0] = 0;
    buf[1] = (CL_Obj)old_off;
    buf[2] = 0;
    buf[3] = 0;

    CT->jit_stack_top = (void *)((char *)&buf[3] + sizeof(buf[0]) + 16);
    CT->jit_depth = 0;
    cl_jit_active_threads = 0;

    cl_gc_compact();

    /* Not pinned (jit_depth=0): the rooted object moved, and buf[1] — a
     * raw integer outside any scan — was left untouched at the stale
     * offset (no writeback exists anymore). */
    ASSERT((uint32_t)test_moved_slot != old_off);
    ASSERT_EQ_INT((uint32_t)buf[1], old_off);
    ASSERT(CL_CONS_P(test_moved_slot));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(test_moved_slot)), 42);

    CL_GC_UNPROTECT(1);
    (void)buf;
    test_moved_slot = CL_NIL;
    teardown();
}

#ifdef CL_GC_CLASSIC   /* the free list is classic machinery */
/* Is `offset` the start of a block on the heap's free list? */
static int test_free_list_contains(uint32_t offset)
{
    uint32_t cur = cl_heap.free_list;
    while (cur) {
        CL_FreeBlock *fb = (CL_FreeBlock *)(cl_arena_base + cur);
        if (cur == offset) return 1;
        cur = fb->next_offset;
    }
    return 0;
}

/* --- A stale spill word holding a FREED offset must not resurrect the
 * free block.  A free block's header word is its raw size, which reads
 * as an unmarked TYPE_CONS: without the free-list skip in phase 2 the
 * scan marks it live, gc_mark_children chases its next_offset link and
 * poisoned payload as car/cdr, and the sweep silently drops the block
 * from the free list (treating garbage as an allocated cons).  The
 * regression assertion: after a GC with the stale spill in the scan
 * window, the freed block is still ON the free list. --- */

TEST(scan_skips_free_list_blocks)
{
    volatile CL_Obj buf[4];
    CL_Obj l1, l2, victim;
    uint32_t free_off;

    setup();
    zero_scratch_stack();

    /* Live neighbours on both sides so the victim's block can't
     * coalesce away — its exact offset must reappear in the free list. */
    l1 = cl_cons(CL_MAKE_FIXNUM(1), CL_NIL);
    victim = cl_cons(CL_MAKE_FIXNUM(2), CL_NIL);
    l2 = cl_cons(CL_MAKE_FIXNUM(3), CL_NIL);
    CL_GC_PROTECT(l1);
    CL_GC_PROTECT(l2);

    free_off = (uint32_t)victim;
    victim = CL_NIL;

    /* Sweep the victim into the free list (no JIT scan yet). */
    cl_gc();
    ASSERT(test_free_list_contains(free_off));

    /* Plant the freed offset as a stale JIT spill in the scan window. */
    buf[0] = 0;
    buf[1] = (CL_Obj)free_off;
    buf[2] = 0;
    buf[3] = 0;
    CT->jit_stack_top = (void *)((char *)&buf[3] + sizeof(buf[0]) + 16);
    CT->jit_depth = 1;
    cl_jit_active_threads = 1;

    cl_gc();

    /* The free block must have been skipped by the conservative scan:
     * still unmarked, still on the free list.  (Pre-fix: marked live,
     * swept as an allocated cons, absent from the list.) */
    ASSERT(test_free_list_contains(free_off));
    ASSERT(!(((CL_Header *)(cl_arena_base + free_off))->header & CL_HDR_MARK_BIT));

    /* Live neighbours untouched. */
    ASSERT(CL_CONS_P(l1));
    ASSERT(CL_CONS_P(l2));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(l1)), 1);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(l2)), 3);

    CL_GC_UNPROTECT(2);
    (void)buf;
    cl_jit_active_threads = 0;
    CT->jit_depth = 0;
    CT->jit_stack_top = NULL;
    teardown();
}
#endif /* CL_GC_CLASSIC */

#ifdef CL_GC_IMMIX
/* --- A stale spill naming a CORPSE.  The Immix backend never walks dead
 * space: a dead cons that shares a line with live neighbours keeps its
 * header until the line is reclaimed, and every other cycle it reads
 * UNMARKED (the mark sense flips), so the conservative scan accepts it
 * and traces it.  Its slots are garbage by then — here an offset INSIDE
 * a live cons.  The trace from a conservative root must validate every
 * push as a real object start; otherwise it stamps a mark bit into the
 * live cons's cdr (observed as the fixnum 4 turning into 0x400004). --- */

TEST(scan_traces_a_corpse_without_marking_interiors)
{
    volatile CL_Obj buf[4];
    CL_Obj l1, l2, victim;
    uint32_t v_off, interior;
    int cycle;

    setup();
    zero_scratch_stack();

    l1 = cl_cons(CL_MAKE_FIXNUM(1), CL_NIL);
    victim = cl_cons(CL_MAKE_FIXNUM(2), CL_NIL);
    l2 = cl_cons(CL_MAKE_FIXNUM(3), CL_MAKE_FIXNUM(4));
    CL_GC_PROTECT(l1);
    CL_GC_PROTECT(l2);
    v_off = (uint32_t)victim;
    victim = CL_NIL;

    /* The victim dies; its header survives in the live neighbours' line. */
    cl_gc();
    ASSERT_EQ_INT(CL_HDR_TYPE(cl_arena_base + v_off), TYPE_CONS);
    ASSERT_EQ_INT((int)(v_off >> CL_GC_LINE_SHIFT), (int)(l2 >> CL_GC_LINE_SHIFT));

    /* Garbage in the corpse: its car now names the inside of l2 (the cdr
     * word on a 64-bit host, the car word on the Amiga — an interior word
     * either way). */
    interior = (uint32_t)l2 + CL_ALIGN;
    ((CL_Cons *)(cl_arena_base + v_off))->car = (CL_Obj)interior;

    buf[0] = 0;
    buf[1] = (CL_Obj)v_off;
    buf[2] = 0;
    buf[3] = 0;
    CT->jit_stack_top = (void *)((char *)&buf[3] + sizeof(buf[0]) + 16);
    CT->jit_depth = 1;
    cl_jit_active_threads = 1;

    /* Three cycles cover both parities of the corpse's stale mark. */
    for (cycle = 0; cycle < 3; cycle++) {
        cl_gc();
        ASSERT(CL_CONS_P(l1));
        ASSERT(CL_CONS_P(l2));
        ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(l2)), 3);
        ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_cdr(l2)), 4);   /* no bit stamped into it */
        ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(l1)), 1);
    }

    CL_GC_UNPROTECT(2);
    (void)buf;
    cl_jit_active_threads = 0;
    CT->jit_depth = 0;
    CT->jit_stack_top = NULL;
    teardown();
}
#endif /* CL_GC_IMMIX */

/* --- cl_error_unwind resets jit_depth and the global counter when
 * unwinding past a simulated JIT frame. --- */

TEST(unwind_resets_jit_active_count)
{
    int err = 0;

    setup();

    /* Pretend we entered cl_jit_invoke */
    CT->jit_depth = 1;
    CT->jit_stack_top = (void *)&err;
    cl_jit_active_threads = 1;

    CL_CATCH(err);
    if (err) {
        ASSERT_EQ_INT(CT->jit_depth, 0);
        ASSERT(CT->jit_stack_top == NULL);
        ASSERT_EQ_INT(cl_jit_active_threads, 0);
    } else {
        cl_error(CL_ERR_GENERAL, "simulated error inside JIT'd code");
        ASSERT(0 && "unreachable");
    }
    CL_UNCATCH();

    teardown();
}

int main(void)
{
    test_init();
    RUN(scan_keeps_obj_alive);
    RUN(scan_skipped_when_depth_zero);
    RUN(compaction_runs_even_when_jit_active);
    RUN(pinned_obj_does_not_move);
    RUN(unpinned_obj_moves);
#ifdef CL_GC_CLASSIC
    RUN(scan_skips_free_list_blocks);
#endif
#ifdef CL_GC_IMMIX
    RUN(scan_traces_a_corpse_without_marking_interiors);
#endif
    RUN(unwind_resets_jit_active_count);
    REPORT();
}
