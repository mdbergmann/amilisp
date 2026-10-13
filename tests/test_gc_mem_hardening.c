/*
 * test_gc_mem_hardening.c — regression tests for the tier-4 GC-audit
 * mem.c hardening batch (batch 5):
 *
 * M1  Allocator element-count caps.  The convenience allocators computed
 *     alloc_size = header + count*elt in uint32 arithmetic, which wraps
 *     for counts >= 2^30 (CL_Obj elements) and sails PAST cl_alloc's
 *     23-bit byte-size guard with a tiny wrapped size — the element loops
 *     then scribble far beyond the block.  Normal Lisp entry points are
 *     bounded by ARRAY-DIMENSION-LIMIT checks, but the FASL reader feeds
 *     raw u32 lengths from disk.  Each allocator must now reject the
 *     count BEFORE computing alloc_size (CL_ERR_STORAGE, allocator state
 *     untouched).  cl_make_hashtable additionally had an infinite loop:
 *     its power-of-two round-up (p <<= 1) wraps to 0 for counts > 2^31.
 *
 * M1b FASL reader length sanity.  A corrupted .fasl with a huge vector/
 *     string/struct/bignum/bit-vector count must fail with a clean
 *     FASL_ERR_BAD_LENGTH — pre-fix, FASL_TAG_STRING's
 *     platform_alloc(len+1) wrapped to a tiny buffer and the r->pos+len
 *     truncation check itself wrapped, so cl_fasl_read_bytes memcpy'd
 *     ~4GB over the C heap.
 *
 * M4  Root-dedup skip for all independently-forwarded thread regions
 *     (mv_values / vm_extra_args / saved_pending_stack here; VM stack was
 *     already covered by test_gc_root_dedup.c).  Same deterministic
 *     collision technique: after a normalizing compaction,
 *     [garbage][C][X] equal-size conses make X's forwarded offset equal
 *     C's old offset, so a pre-fix second forward relocated the aliased
 *     root onto C's new home.  saved_pending_stack additionally requires
 *     the entry's pending_throw flag set, mirroring the exact gating
 *     gc_update_thread_roots uses for that region.
 *
 * M8  cl_bump_fits: the naive `bump + size <= arena_size` check wraps in
 *     uint32 near a ~4GB arena's end and hands out a pointer past the
 *     arena.
 *
 * M10 cl_mem_init must clear GC state living in static storage
 *     (gc_compact_pending et al.) so a heap re-initialization doesn't
 *     inherit decisions/offsets from the previous arena.  Kept LAST:
 *     it re-initializes the heap under the other tests' package state.
 *
 * M11 bi_vector_push_extend / bi_adjust_array in-place displacement writes
 *     data[0]/data[1] using the rank<=1 GC-displacement-contract layout
 *     (CL_DISP_BASE_IDX == 0).  A rank>1 array reaching that code would have
 *     its dimension fixnums at data[0.. ] read as the backing pointer by the
 *     GC's mark/relocate pass and silently corrupt the heap.  Unreachable
 *     from Lisp today (MAKE-ARRAY never combines rank>1 with a fill
 *     pointer, and ADJUST-ARRAY rejects rank>1 input before reaching its
 *     displacement write), so both functions carry a loud backstop error
 *     instead.  Exercised here by building a rank>1 vector directly via
 *     cl_make_array (bypassing MAKE-ARRAY's argument validation) and
 *     calling the (now non-static) builtins directly.
 */
#include "test.h"
#include "core/types.h"
#include "core/mem.h"
#include "core/error.h"
#include "core/package.h"
#include "core/symbol.h"
#include "core/fasl.h"
#include "core/thread.h"
#include "platform/platform.h"

/* Non-static mem.c internals under test (not part of the public API). */
extern int cl_bump_fits(uint32_t bump, uint32_t size, uint32_t arena_size);
extern int gc_compact_pending;   /* gc_compact.c */

/* Non-static builtins_array.c internals under test (not part of the
 * public API) — see the M11 comment above. */
extern CL_Obj bi_vector_push_extend(CL_Obj *args, int n);
extern CL_Obj bi_adjust_array(CL_Obj *args, int n);

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

/* --- M1: allocator element-count caps --------------------------------- */

/* Run one over-cap allocation, asserting it signals CL_ERR_STORAGE and
 * leaves the allocator exactly where it was (no wrapped tiny block, no
 * bump advance). */
#define ASSERT_CAPPED(call)                                             \
    do {                                                                \
        uint32_t bump_before = cl_heap.bump;                            \
        CL_Obj r_ = CL_UNBOUND;                                         \
        int err_;                                                       \
        CL_CATCH(err_);                                                 \
        if (err_ == CL_ERR_NONE) {                                      \
            r_ = (call);                                                \
            CL_UNCATCH();                                               \
        } else {                                                        \
            CL_UNCATCH();                                               \
        }                                                               \
        ASSERT_EQ_INT(err_, CL_ERR_STORAGE);                            \
        ASSERT(r_ == CL_UNBOUND);                                       \
        ASSERT_EQ_INT((int)(cl_heap.bump - bump_before), 0);            \
    } while (0)

TEST(make_vector_wrap_length_capped)
{
    /* 2^30 elements: length*4 == 2^32 wraps to 0 → pre-fix allocated a
     * header-only block with length = 2^30. */
    ASSERT_CAPPED(cl_make_vector(0x40000000u));
    /* Just above the cap (no wrap) must also be rejected. */
    ASSERT_CAPPED(cl_make_vector(CL_MAX_VECTOR_ELTS + 1u));
}

TEST(make_array_wrap_total_capped)
{
    uint32_t dims[2];
    dims[0] = 0x10000u; dims[1] = 0x4000u;
    ASSERT_CAPPED(cl_make_array(0x40000000u, 2, dims, 0, CL_NO_FILL_POINTER));
}

TEST(make_string_wrap_length_capped)
{
    /* len UINT32_MAX: `+ len + 1` wraps alloc_size to sizeof(CL_String). */
    ASSERT_CAPPED(cl_make_string(NULL, 0xFFFFFFFFu));
}

#ifdef CL_WIDE_STRINGS
TEST(make_wide_string_wrap_length_capped)
{
    ASSERT_CAPPED(cl_make_wide_string(NULL, 0x40000000u));
}
#endif

TEST(make_struct_wrap_slots_capped)
{
    ASSERT_CAPPED(cl_make_struct(CL_NIL, 0x40000000u));
}

TEST(make_bignum_wrap_limbs_capped)
{
    /* 2^31 limbs: n_limbs*2 wraps to 0. */
    ASSERT_CAPPED(cl_make_bignum(0x80000000u, 0));
}

TEST(make_bit_vector_wrap_bits_capped)
{
    /* nbits near UINT32_MAX: CL_BV_WORDS's `+ 31` wraps to a tiny word
     * count. */
    ASSERT_CAPPED(cl_make_bit_vector(0xFFFFFFF0u));
}

TEST(make_hashtable_huge_bucket_count_capped)
{
    /* THE hang regression: counts > 2^31 made the power-of-two round-up
     * loop (p <<= 1) wrap to 0 and spin forever — pre-fix this test
     * trips the watchdog, post-fix it signals promptly. */
    ASSERT_CAPPED(cl_make_hashtable(0x80000001u, 0));
    ASSERT_CAPPED(cl_make_hashtable(CL_MAX_HT_BUCKETS + 1u, 0));
}

/* --- M1b: FASL reader length sanity ------------------------------------ */

/* Deserialize a hand-crafted tag payload, expecting FASL_ERR_BAD_LENGTH. */
static int fasl_deserialize_error(const uint8_t *bytes, uint32_t n)
{
    CL_FaslReader r;
    cl_fasl_reader_init(&r, bytes, n);
    (void)cl_fasl_deserialize_obj(&r);
    return r.error;
}

TEST(fasl_vector_huge_length_rejected)
{
    /* FASL_TAG_VECTOR + u32 length 0xFFFFFFF0 (big-endian), no elements. */
    static const uint8_t bytes[] = { FASL_TAG_VECTOR, 0xFF, 0xFF, 0xFF, 0xF0 };
    ASSERT_EQ_INT(fasl_deserialize_error(bytes, sizeof(bytes)),
                  FASL_ERR_BAD_LENGTH);
}

TEST(fasl_string_huge_length_rejected)
{
    /* Pre-fix this wrapped platform_alloc(len+1) to a 0-byte buffer AND
     * wrapped read_bytes' r->pos+len truncation check — a ~4GB memcpy
     * over the C heap (segfault at best). */
    static const uint8_t bytes[] = { FASL_TAG_STRING, 0xFF, 0xFF, 0xFF, 0xFF };
    ASSERT_EQ_INT(fasl_deserialize_error(bytes, sizeof(bytes)),
                  FASL_ERR_BAD_LENGTH);
}

TEST(fasl_bignum_huge_limb_count_rejected)
{
    /* FASL_TAG_BIGNUM + u8 sign + u32 n_limbs 0x80000000. */
    static const uint8_t bytes[] = { FASL_TAG_BIGNUM, 0, 0x80, 0x00, 0x00, 0x00 };
    ASSERT_EQ_INT(fasl_deserialize_error(bytes, sizeof(bytes)),
                  FASL_ERR_BAD_LENGTH);
}

TEST(fasl_bit_vector_huge_length_rejected)
{
    static const uint8_t bytes[] = { FASL_TAG_BIT_VECTOR, 0xFF, 0xFF, 0xFF, 0xF0 };
    ASSERT_EQ_INT(fasl_deserialize_error(bytes, sizeof(bytes)),
                  FASL_ERR_BAD_LENGTH);
}

TEST(fasl_struct_huge_slot_count_rejected)
{
    /* FASL_TAG_STRUCT + type_desc (NIL) + u32 n_slots 0x40000000. */
    static const uint8_t bytes[] = { FASL_TAG_STRUCT, FASL_TAG_NIL,
                                     0x40, 0x00, 0x00, 0x00 };
    ASSERT_EQ_INT(fasl_deserialize_error(bytes, sizeof(bytes)),
                  FASL_ERR_BAD_LENGTH);
}

TEST(fasl_plausible_lengths_still_load)
{
    /* Sanity: the new bounds must not reject well-formed payloads. */
    static const uint8_t vec2[] = { FASL_TAG_VECTOR, 0x00, 0x00, 0x00, 0x02,
                                    FASL_TAG_NIL, FASL_TAG_NIL };
    static const uint8_t str3[] = { FASL_TAG_STRING, 0x00, 0x00, 0x00, 0x03,
                                    'a', 'b', 'c' };
    CL_FaslReader r;
    CL_Obj obj;

    cl_fasl_reader_init(&r, vec2, sizeof(vec2));
    obj = cl_fasl_deserialize_obj(&r);
    ASSERT_EQ_INT(r.error, 0);
    ASSERT(CL_VECTOR_P(obj));
    ASSERT_EQ_INT((int)((CL_Vector *)CL_OBJ_TO_PTR(obj))->length, 2);

    cl_fasl_reader_init(&r, str3, sizeof(str3));
    obj = cl_fasl_deserialize_obj(&r);
    ASSERT_EQ_INT(r.error, 0);
    ASSERT(CL_STRING_P(obj));
    ASSERT_EQ_INT((int)((CL_String *)CL_OBJ_TO_PTR(obj))->length, 3);
}

/* --- M4: root aliases of independently-forwarded thread regions ------- */

TEST(protect_of_mv_values_slot_is_harmless)
{
    CL_Obj g1, c;
    CL_Thread *t = cl_get_current_thread();

    /* Normalize so the next three conses are contiguous bump allocations:
     * [garbage][c][x].  After compaction x's new offset == c's old offset,
     * so a pre-fix double forward rewired the alias onto c's new home. */
    cl_gc_compact();

    g1 = cl_cons(CL_MAKE_FIXNUM(111), CL_NIL);
    c  = cl_cons(CL_MAKE_FIXNUM(777), CL_NIL);
    t->mv_values[0] = cl_cons(CL_MAKE_FIXNUM(888), CL_NIL);
    g1 = CL_NIL;
    (void)g1;

    CL_GC_PROTECT(c);
    /* The hazard: mv_values[] is forwarded wholesale by
     * gc_update_thread_roots; protecting a slot of it registers the same
     * address a second time. */
    CL_GC_PROTECT(t->mv_values[0]);

    cl_gc_compact();

    ASSERT(CL_CONS_P(t->mv_values[0]));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(t->mv_values[0])), 888);
    ASSERT(CL_CONS_P(c));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(c)), 777);

    CL_GC_UNPROTECT(2);
    t->mv_values[0] = CL_NIL;
}

TEST(protect_of_vm_extra_args_slot_is_harmless)
{
    CL_Obj g1, c;
    CL_Thread *t = cl_get_current_thread();

    cl_gc_compact();

    g1 = cl_cons(CL_MAKE_FIXNUM(111), CL_NIL);
    c  = cl_cons(CL_MAKE_FIXNUM(777), CL_NIL);
    t->vm_extra_args_buf[0] = cl_cons(CL_MAKE_FIXNUM(888), CL_NIL);
    t->vm_extra_count = 1;
    g1 = CL_NIL;
    (void)g1;

    CL_GC_PROTECT(c);
    CL_GC_PROTECT(t->vm_extra_args_buf[0]);

    cl_gc_compact();

    ASSERT(CL_CONS_P(t->vm_extra_args_buf[0]));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(t->vm_extra_args_buf[0])), 888);
    ASSERT(CL_CONS_P(c));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(c)), 777);

    CL_GC_UNPROTECT(2);
    t->vm_extra_args_buf[0] = CL_NIL;
    t->vm_extra_count = 0;
}

TEST(protect_of_saved_pending_stack_slot_is_harmless)
{
    CL_Obj g1, c;
    CL_Thread *t = cl_get_current_thread();
    int idx;

    cl_gc_compact();

    g1 = cl_cons(CL_MAKE_FIXNUM(111), CL_NIL);
    c  = cl_cons(CL_MAKE_FIXNUM(777), CL_NIL);

    idx = t->saved_pending_top;
    t->saved_pending_stack[idx].pending_throw = 1;
    t->saved_pending_stack[idx].pending_tag = CL_NIL;
    t->saved_pending_stack[idx].pending_value = cl_cons(CL_MAKE_FIXNUM(888), CL_NIL);
    t->saved_pending_stack[idx].pending_mv_count = 0;
    t->saved_pending_top = idx + 1;
    g1 = CL_NIL;
    (void)g1;

    CL_GC_PROTECT(c);
    /* The hazard: an armed (pending_throw) saved_pending_stack[] entry is
     * forwarded wholesale by gc_update_thread_roots; protecting one of its
     * slots registers the same address a second time. */
    CL_GC_PROTECT(t->saved_pending_stack[idx].pending_value);

    cl_gc_compact();

    ASSERT(CL_CONS_P(t->saved_pending_stack[idx].pending_value));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(t->saved_pending_stack[idx].pending_value)), 888);
    ASSERT(CL_CONS_P(c));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(c)), 777);

    CL_GC_UNPROTECT(2);
    t->saved_pending_stack[idx].pending_throw = 0;
    t->saved_pending_stack[idx].pending_value = CL_NIL;
    t->saved_pending_top = idx;
}

TEST(protect_of_dyn_stack_slot_is_harmless)
{
    CL_Obj g1, c;
    CL_Thread *t = cl_get_current_thread();
    int idx;

    cl_gc_compact();

    g1 = cl_cons(CL_MAKE_FIXNUM(111), CL_NIL);
    c  = cl_cons(CL_MAKE_FIXNUM(777), CL_NIL);

    idx = t->dyn_top;
    t->dyn_stack[idx].symbol = CL_NIL;
    t->dyn_stack[idx].old_value = cl_cons(CL_MAKE_FIXNUM(888), CL_NIL);
    t->dyn_top = idx + 1;
    g1 = CL_NIL;
    (void)g1;

    CL_GC_PROTECT(c);
    CL_GC_PROTECT(t->dyn_stack[idx].old_value);

    cl_gc_compact();

    ASSERT(CL_CONS_P(t->dyn_stack[idx].old_value));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(t->dyn_stack[idx].old_value)), 888);
    ASSERT(CL_CONS_P(c));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(c)), 777);

    CL_GC_UNPROTECT(2);
    t->dyn_top = idx;
}

/* An alias BEYOND the live bound must still be forwarded by the
 * registered-root pass (the region loop stops at dyn_top, so nothing
 * else owns it) — guards against over-broad skip ranges. */
TEST(protect_beyond_region_bound_still_forwarded)
{
    CL_Obj g1, c;
    CL_Thread *t = cl_get_current_thread();
    int idx = t->dyn_top;   /* one PAST the live dyn_stack entries */

    cl_gc_compact();

    g1 = cl_cons(CL_MAKE_FIXNUM(111), CL_NIL);
    c  = cl_cons(CL_MAKE_FIXNUM(777), CL_NIL);
    t->dyn_stack[idx].old_value = cl_cons(CL_MAKE_FIXNUM(888), CL_NIL);
    g1 = CL_NIL;
    (void)g1;

    CL_GC_PROTECT(c);
    CL_GC_PROTECT(t->dyn_stack[idx].old_value);  /* sole root for this cons */

    cl_gc_compact();

    ASSERT(CL_CONS_P(t->dyn_stack[idx].old_value));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(t->dyn_stack[idx].old_value)), 888);

    CL_GC_UNPROTECT(2);
    t->dyn_stack[idx].old_value = CL_NIL;
}

/* --- M8: wrap-safe bump-fit check -------------------------------------- */

TEST(bump_fits_rejects_uint32_wrap)
{
    /* Naive bump+size wraps: 0xFFFFFF00 + 0x200 == 0x100 <= arena_size,
     * so the old check handed out a pointer past the arena. */
    ASSERT(!cl_bump_fits(0xFFFFFF00u, 0x200u, 0xFFFFFFFFu));
    /* size alone larger than the arena */
    ASSERT(!cl_bump_fits(0u, 0x300u, 0x200u));
    /* Exact fit and ordinary fits still pass */
    ASSERT(cl_bump_fits(0x100u, 0x100u, 0x200u));
    ASSERT(cl_bump_fits(0u, 0x200u, 0x200u));
    ASSERT(!cl_bump_fits(0x101u, 0x100u, 0x200u));
}

/* --- M11: rank>1 displacement backstop --------------------------------- */

TEST(vector_push_extend_rejects_rank_gt_1)
{
    /* A rank-2 array with a fill pointer at capacity can't be built via
     * MAKE-ARRAY (fill pointers are vector-only), but the internal
     * allocator itself does not cross-validate — construct one directly
     * to reach the backstop guard. */
    uint32_t dims[2];
    CL_Obj vec, args[2];
    int err;

    dims[0] = 1; dims[1] = 2;
    vec = cl_make_array(2, 2, dims,
                        CL_VEC_FLAG_FILL_POINTER | CL_VEC_FLAG_ADJUSTABLE, 2);
    ASSERT(vec != CL_NIL);

    args[0] = CL_MAKE_FIXNUM(42);
    args[1] = vec;

    CL_CATCH(err);
    if (err == CL_ERR_NONE) {
        (void)bi_vector_push_extend(args, 2);
        CL_UNCATCH();
        ASSERT(0); /* must not reach here */
    } else {
        CL_UNCATCH();
    }
    ASSERT_EQ_INT(err, CL_ERR_GENERAL);
}

TEST(adjust_array_rejects_rank_gt_1)
{
    /* ADJUST-ARRAY rejects rank>1 input at entry, before it ever reaches
     * the same displacement-write backstop bi_vector_push_extend hits —
     * this locks in that no rank>1 array can pass through to the write,
     * even if the entry check is ever refactored. */
    uint32_t dims[2];
    CL_Obj vec, args[2];
    int err;

    dims[0] = 1; dims[1] = 2;
    vec = cl_make_array(2, 2, dims, CL_VEC_FLAG_ADJUSTABLE, CL_NO_FILL_POINTER);
    ASSERT(vec != CL_NIL);

    args[0] = vec;
    args[1] = CL_MAKE_FIXNUM(4);

    CL_CATCH(err);
    if (err == CL_ERR_NONE) {
        (void)bi_adjust_array(args, 2);
        CL_UNCATCH();
        ASSERT(0); /* must not reach here */
    } else {
        CL_UNCATCH();
    }
    ASSERT_EQ_INT(err, CL_ERR_GENERAL);
}

/* --- M10: heap re-init clears static GC state -------------------------- */
/* LAST test: it re-initializes the heap, invalidating package/symbol
 * state built in setup(); nothing may allocate Lisp objects after it. */

TEST(mem_reinit_clears_transient_gc_state)
{
    gc_compact_pending = 1;
    cl_mem_shutdown();
    cl_mem_init(CL_DEFAULT_HEAP_SIZE);
    ASSERT_EQ_INT(gc_compact_pending, 0);
}

int main(void)
{
    test_init();
    setup();

    RUN(make_vector_wrap_length_capped);
    RUN(make_array_wrap_total_capped);
    RUN(make_string_wrap_length_capped);
#ifdef CL_WIDE_STRINGS
    RUN(make_wide_string_wrap_length_capped);
#endif
    RUN(make_struct_wrap_slots_capped);
    RUN(make_bignum_wrap_limbs_capped);
    RUN(make_bit_vector_wrap_bits_capped);
    RUN(make_hashtable_huge_bucket_count_capped);

    RUN(fasl_vector_huge_length_rejected);
    RUN(fasl_string_huge_length_rejected);
    RUN(fasl_bignum_huge_limb_count_rejected);
    RUN(fasl_bit_vector_huge_length_rejected);
    RUN(fasl_struct_huge_slot_count_rejected);
    RUN(fasl_plausible_lengths_still_load);

    RUN(protect_of_mv_values_slot_is_harmless);
    RUN(protect_of_vm_extra_args_slot_is_harmless);
    RUN(protect_of_saved_pending_stack_slot_is_harmless);
    RUN(protect_of_dyn_stack_slot_is_harmless);
    RUN(protect_beyond_region_bound_still_forwarded);

    RUN(bump_fits_rejects_uint32_wrap);

    RUN(vector_push_extend_rejects_rank_gt_1);
    RUN(adjust_array_rejects_rank_gt_1);

    RUN(mem_reinit_clears_transient_gc_state);

    teardown();
    REPORT();
}
