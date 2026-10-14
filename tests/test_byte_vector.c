/*
 * Tests for packed byte vectors — TYPE_BYTE_VECTOR, the true packed storage
 * behind (unsigned-byte 8/16) / (signed-byte 8/16) arrays: 1 byte per
 * element for the 8-bit kinds, 2 bytes for the 16-bit kinds (elt_shift).
 *
 * Specified behavior follows the HyperSpec for make-array / aref / elt /
 * fill / subseq / array-element-type / upgraded-array-element-type / typep
 * on specialized arrays; where amilisp deliberately restricts a feature
 * (vector-push-extend / adjust-array on packed storage), the tests pin the
 * clean-error behavior so it never degrades into silent corruption.
 */
#include "test.h"
#include "core/types.h"
#include "core/mem.h"
#include "core/error.h"
#include "core/package.h"
#include "core/symbol.h"
#include "core/reader.h"
#include "core/printer.h"
#include "core/compiler.h"
#include "core/vm.h"
#include "core/builtins.h"
#include "core/repl.h"
#include "core/fasl.h"
#include "platform/platform.h"

static void setup(void)
{
    platform_init();
    cl_thread_init();
    cl_error_init();
    cl_mem_init(CL_DEFAULT_HEAP_SIZE);
    cl_package_init();
    cl_symbol_init();
    cl_reader_init();
    cl_printer_init();
    cl_compiler_init();
    cl_vm_init(0, 0);
    cl_builtins_init();
    cl_repl_init();
}

static void teardown(void)
{
    cl_mem_shutdown();
    platform_shutdown();
}

/* Helper: eval a string, return printed result */
static const char *eval_print(const char *str)
{
    static char buf[1024];
    int err;

    CL_CATCH(err);
    if (err == CL_ERR_NONE) {
        CL_Obj result = cl_eval_string(str);
        cl_prin1_to_string(result, buf, sizeof(buf));
        CL_UNCATCH();
        return buf;
    } else {
        CL_UNCATCH();
        snprintf(buf, sizeof(buf), "ERROR:%d", err);
        cl_vm.sp = 0;
        cl_vm.fp = 0;
        return buf;
    }
}

/* ========================================================
 * Step 1: Allocation, layout, predicate, GC
 * ======================================================== */

TEST(alloc_zero)
{
    CL_Obj bv = cl_make_byte_vector(0, 0, 0);
    ASSERT(CL_BYTE_VECTOR_P(bv));
    ASSERT_EQ_INT(0, (int)((CL_ByteVector *)CL_OBJ_TO_PTR(bv))->length);
}

TEST(alloc_len_and_zero_fill)
{
    CL_Obj bv = cl_make_byte_vector(100, 0, 0);
    CL_ByteVector *b = (CL_ByteVector *)CL_OBJ_TO_PTR(bv);
    uint32_t i;
    ASSERT_EQ_INT(100, (int)b->length);
    for (i = 0; i < 100; i++)
        ASSERT_EQ_INT(0, (int)b->data[i]);
}

TEST(packed_size_is_one_byte_per_element)
{
    /* The whole point: header + N bytes, not header + 4N.  Guard against a
     * regression that silently reverts to CL_Obj storage. */
    CL_Obj bv = cl_make_byte_vector(64, 0, 0);
    CL_ByteVector *b = (CL_ByteVector *)CL_OBJ_TO_PTR(bv);
    ASSERT(CL_HDR_SIZE(b) < sizeof(CL_ByteVector) + 64 + CL_ALIGN);
}

TEST(signedness_flag)
{
    CL_Obj u = cl_make_byte_vector(4, 0, 0);
    CL_Obj s = cl_make_byte_vector(4, 1, 0);
    ASSERT_EQ_INT(0, (int)((CL_ByteVector *)CL_OBJ_TO_PTR(u))->is_signed);
    ASSERT_EQ_INT(1, (int)((CL_ByteVector *)CL_OBJ_TO_PTR(s))->is_signed);
}

TEST(signed_get_sign_extends)
{
    CL_Obj s = cl_make_byte_vector(2, 1, 0);
    CL_ByteVector *b = (CL_ByteVector *)CL_OBJ_TO_PTR(s);
    b->data[0] = 0xFF;   /* -1 as (signed-byte 8) */
    b->data[1] = 0x80;   /* -128 */
    ASSERT_EQ_INT(-1, (int)cl_bytevec_get(b, 0));
    ASSERT_EQ_INT(-128, (int)cl_bytevec_get(b, 1));
    /* Same raw bytes read unsigned */
    b = (CL_ByteVector *)CL_OBJ_TO_PTR(cl_make_byte_vector(1, 0, 0));
    b->data[0] = 0xFF;
    ASSERT_EQ_INT(255, (int)cl_bytevec_get(b, 0));
}

TEST(predicate_negative)
{
    ASSERT(!CL_BYTE_VECTOR_P(CL_NIL));
    ASSERT(!CL_BYTE_VECTOR_P(CL_MAKE_FIXNUM(42)));
    ASSERT(!CL_BYTE_VECTOR_P(cl_make_vector(3)));
    ASSERT(!CL_BYTE_VECTOR_P(cl_make_bit_vector(3)));
    ASSERT(!CL_VECTOR_P(cl_make_byte_vector(3, 0, 0)));
    ASSERT(!CL_BIT_VECTOR_P(cl_make_byte_vector(3, 0, 0)));
}

TEST(gc_survives_with_contents)
{
    CL_Obj bv = cl_make_byte_vector(64, 0, 0);
    CL_ByteVector *b = (CL_ByteVector *)CL_OBJ_TO_PTR(bv);
    b->data[0] = 255;
    b->data[63] = 7;

    CL_GC_PROTECT(bv);
    cl_gc();
    CL_GC_UNPROTECT(1);

    ASSERT(CL_BYTE_VECTOR_P(bv));
    b = (CL_ByteVector *)CL_OBJ_TO_PTR(bv);
    ASSERT_EQ_INT(255, (int)b->data[0]);
    ASSERT_EQ_INT(7, (int)b->data[63]);
}

/* ========================================================
 * Step 2: MAKE-ARRAY routing + init options
 * ======================================================== */

TEST(make_array_u8_is_packed)
{
    CL_Obj v = cl_eval_string("(make-array 8 :element-type '(unsigned-byte 8))");
    ASSERT(CL_BYTE_VECTOR_P(v));
    ASSERT_EQ_INT(0, (int)((CL_ByteVector *)CL_OBJ_TO_PTR(v))->is_signed);
}

TEST(make_array_s8_is_packed)
{
    CL_Obj v = cl_eval_string("(make-array 8 :element-type '(signed-byte 8))");
    ASSERT(CL_BYTE_VECTOR_P(v));
    ASSERT_EQ_INT(1, (int)((CL_ByteVector *)CL_OBJ_TO_PTR(v))->is_signed);
}

TEST(make_array_upgraded_specs)
{
    /* (mod 256), (integer 0 255) upgrade to u8; (integer -5 5) to s8;
     * (unsigned-byte 1) stays a bit-vector (more specialized). */
    ASSERT(CL_BYTE_VECTOR_P(cl_eval_string(
        "(make-array 4 :element-type '(mod 256))")));
    ASSERT(CL_BYTE_VECTOR_P(cl_eval_string(
        "(make-array 4 :element-type '(integer 0 255))")));
    ASSERT(CL_BYTE_VECTOR_P(cl_eval_string(
        "(make-array 4 :element-type '(integer -5 5))")));
    ASSERT(CL_BIT_VECTOR_P(cl_eval_string(
        "(make-array 4 :element-type '(unsigned-byte 1))")));
    /* (unsigned-byte 16) packs at 2 bytes/element since slice 3;
     * (unsigned-byte 17) does NOT fit any packed kind — general vector */
    ASSERT(CL_BYTE_VECTOR_P(cl_eval_string(
        "(make-array 4 :element-type '(unsigned-byte 16))")));
    ASSERT(CL_VECTOR_P(cl_eval_string(
        "(make-array 4 :element-type '(unsigned-byte 17))")));
}

TEST(make_array_initial_element)
{
    ASSERT_STR_EQ(eval_print(
        "(make-array 3 :element-type '(unsigned-byte 8) :initial-element 9)"),
        "#(9 9 9)");
}

TEST(make_array_initial_contents)
{
    ASSERT_STR_EQ(eval_print(
        "(make-array 4 :element-type '(unsigned-byte 8) "
        ":initial-contents '(0 1 128 255))"),
        "#(0 1 128 255)");
    ASSERT_STR_EQ(eval_print(
        "(make-array 3 :element-type '(signed-byte 8) "
        ":initial-contents '(-128 0 127))"),
        "#(-128 0 127)");
}

TEST(make_array_initial_element_range_error)
{
    ASSERT_STR_EQ(eval_print(
        "(handler-case (make-array 2 :element-type '(unsigned-byte 8) "
        ":initial-element 256) (type-error () :caught))"),
        ":CAUGHT");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (make-array 2 :element-type '(unsigned-byte 8) "
        ":initial-element -1) (type-error () :caught))"),
        ":CAUGHT");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (make-array 2 :element-type '(signed-byte 8) "
        ":initial-element 128) (type-error () :caught))"),
        ":CAUGHT");
}

TEST(make_array_initial_contents_range_error)
{
    ASSERT_STR_EQ(eval_print(
        "(handler-case (make-array 2 :element-type '(unsigned-byte 8) "
        ":initial-contents '(1 300)) (type-error () :caught))"),
        ":CAUGHT");
}

TEST(make_array_dims_list_spelling)
{
    /* '(n) one-element dimension list must behave like n */
    ASSERT(CL_BYTE_VECTOR_P(cl_eval_string(
        "(make-array '(6) :element-type '(unsigned-byte 8))")));
}

/* ========================================================
 * Step 3: AREF / (SETF AREF) / ELT / bounds / range
 * ======================================================== */

TEST(aref_setf_aref)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 3 :element-type '(unsigned-byte 8))))"
        "  (setf (aref v 0) 255 (aref v 2) 1)"
        "  (list (aref v 0) (aref v 1) (aref v 2)))"),
        "(255 0 1)");
}

TEST(aref_signed_roundtrip)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 2 :element-type '(signed-byte 8))))"
        "  (setf (aref v 0) -128 (aref v 1) 127)"
        "  (list (aref v 0) (aref v 1)))"),
        "(-128 127)");
}

TEST(aref_bounds_error)
{
    ASSERT_STR_EQ(eval_print(
        "(handler-case (aref (make-array 2 :element-type '(unsigned-byte 8)) 2)"
        " (error () :caught))"),
        ":CAUGHT");
}

TEST(setf_aref_range_error)
{
    /* Both the builtin path and the OP_ASET fast path (constant index in
     * compiled code) must reject out-of-range stores. */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (let ((v (make-array 2 :element-type '(unsigned-byte 8))))"
        "  (setf (aref v 0) 300)) (error () :caught))"),
        ":CAUGHT");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (let ((v (make-array 2 :element-type '(signed-byte 8))))"
        "  (setf (aref v 0) 200)) (error () :caught))"),
        ":CAUGHT");
}

TEST(elt_and_setf_elt)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 3 :element-type '(unsigned-byte 8)"
        " :initial-contents '(7 8 9))))"
        "  (setf (elt v 1) 42)"
        "  (list (elt v 0) (elt v 1) (elt v 2)))"),
        "(7 42 9)");
}

TEST(row_major_aref)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 3 :element-type '(unsigned-byte 8))))"
        "  (setf (row-major-aref v 1) 5)"
        "  (row-major-aref v 1))"),
        "5");
}

/* ========================================================
 * Step 4: array queries / predicates / types
 * ======================================================== */

TEST(array_element_type)
{
    ASSERT_STR_EQ(eval_print(
        "(array-element-type (make-array 2 :element-type '(unsigned-byte 8)))"),
        "(UNSIGNED-BYTE 8)");
    ASSERT_STR_EQ(eval_print(
        "(array-element-type (make-array 2 :element-type '(signed-byte 8)))"),
        "(SIGNED-BYTE 8)");
}

TEST(upgraded_array_element_type)
{
    ASSERT_STR_EQ(eval_print("(upgraded-array-element-type '(unsigned-byte 8))"),
                  "(UNSIGNED-BYTE 8)");
    ASSERT_STR_EQ(eval_print("(upgraded-array-element-type '(mod 256))"),
                  "(UNSIGNED-BYTE 8)");
    ASSERT_STR_EQ(eval_print("(upgraded-array-element-type '(signed-byte 8))"),
                  "(SIGNED-BYTE 8)");
    ASSERT_STR_EQ(eval_print("(upgraded-array-element-type '(integer 0 100))"),
                  "(UNSIGNED-BYTE 8)");
}

TEST(predicates_and_queries)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 5 :element-type '(unsigned-byte 8))))"
        "  (list (vectorp v) (arrayp v) (simple-vector-p v)"
        "        (array-rank v) (array-dimensions v) (array-total-size v)"
        "        (array-dimension v 0) (array-in-bounds-p v 4)"
        "        (array-in-bounds-p v 5) (length v)))"),
        "(T T NIL 1 (5) 5 5 T NIL 5)");
}

TEST(typep_discrimination)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 2 :element-type '(unsigned-byte 8))))"
        "  (list (typep v 'vector) (typep v 'array) (typep v 'sequence)"
        "        (typep v '(vector (unsigned-byte 8)))"
        "        (typep v '(vector (signed-byte 8)))"
        "        (typep v '(vector t))"
        "        (typep v 'simple-vector)"
        "        (typep v 'bit-vector) (typep v 'string)))"),
        "(T T T T NIL NIL NIL NIL NIL)");
    /* general T vector must not satisfy (vector (unsigned-byte 8)) */
    ASSERT_STR_EQ(eval_print(
        "(typep (make-array 2) '(vector (unsigned-byte 8)))"), "NIL");
}

TEST(type_of_roundtrips_through_typep)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 3 :element-type '(unsigned-byte 8))))"
        "  (typep v (type-of v)))"), "T");
    ASSERT_STR_EQ(eval_print(
        "(type-of (make-array 3 :element-type '(unsigned-byte 8)))"),
        "(VECTOR (UNSIGNED-BYTE 8) 3)");
}

/* ========================================================
 * Step 5: fill pointer / vector-push / vector-pop
 * ======================================================== */

TEST(fill_pointer_push_pop)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 3 :element-type '(unsigned-byte 8) :fill-pointer 0)))"
        "  (list (vector-push 10 v) (vector-push 20 v) (fill-pointer v)"
        "        (length v) (vector-pop v) (fill-pointer v)))"),
        "(0 1 2 2 20 1)");
}

TEST(vector_push_full_returns_nil)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 1 :element-type '(unsigned-byte 8) :fill-pointer 1)))"
        "  (vector-push 1 v))"),
        "NIL");
}

TEST(vector_push_extend_full_errors_cleanly)
{
    /* NON-adjustable packed storage cannot be extended in place — must be a
     * clean error, never a silently different object.  (Adjustable byte
     * arrays use the annotated general-vector representation and extend
     * fine — see adjustable_byte_arrays_grow.) */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (let ((v (make-array 1 :element-type '(unsigned-byte 8)"
        " :fill-pointer 1))) (vector-push-extend 2 v))"
        " (error () :caught))"),
        ":CAUGHT");
}

TEST(adjust_array_fresh_copy)
{
    /* ADJUST-ARRAY on a packed (non-adjustable) byte vector returns a FRESH
     * byte vector per CLHS — contents copied, growth from :initial-element,
     * fill pointer carried over. */
    ASSERT_STR_EQ(eval_print(
        "(let* ((v (make-array 3 :element-type '(unsigned-byte 8)"
        "             :initial-contents '(1 2 3)))"
        "       (w (adjust-array v 5 :initial-element 9)))"
        "  (list (eq v w) w (array-element-type w)))"),
        "(NIL #(1 2 3 9 9) (UNSIGNED-BYTE 8))");
    ASSERT_STR_EQ(eval_print(
        "(adjust-array (make-array 4 :element-type '(unsigned-byte 8)"
        " :initial-contents '(1 2 3 4)) 2)"),
        "#(1 2)");
}

TEST(adjustable_byte_arrays_grow)
{
    /* :adjustable t byte arrays use the annotated general-vector
     * representation so ADJUST-ARRAY / VECTOR-PUSH-EXTEND grow them IN
     * PLACE (identity preserved) — the drakma/chunga HTTP-buffer idiom.
     * They still report the byte element type and satisfy the same typep. */
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 4 :element-type '(unsigned-byte 8)"
        "            :adjustable t :fill-pointer 0)))"
        "  (dotimes (i 3) (vector-push-extend i v))"
        "  (let ((w (adjust-array v 8)))"
        "    (dotimes (i 5) (vector-push-extend (+ 10 i) v))"
        "    (list (eq v w) (adjustable-array-p v)"
        "          (array-element-type v)"
        "          (typep v '(vector (unsigned-byte 8)))"
        "          (typep v '(vector t))"
        "          (length v) (aref v 0) (aref v 7))))"),
        "(T T (UNSIGNED-BYTE 8) T NIL 8 0 14)");
    /* the adjustable representation is NOT packed */
    CL_Obj v = cl_eval_string(
        "(make-array 4 :element-type '(unsigned-byte 8) :adjustable t)");
    ASSERT(!CL_BYTE_VECTOR_P(v));
    ASSERT(CL_VECTOR_P(v));
}

/* ========================================================
 * Step 6: sequence functions
 * ======================================================== */

TEST(seq_fill)
{
    ASSERT_STR_EQ(eval_print(
        "(fill (make-array 4 :element-type '(unsigned-byte 8)) 7)"),
        "#(7 7 7 7)");
    ASSERT_STR_EQ(eval_print(
        "(fill (make-array 4 :element-type '(unsigned-byte 8)"
        " :initial-element 1) 9 :start 1 :end 3)"),
        "#(1 9 9 1)");
}

TEST(seq_subseq)
{
    ASSERT_STR_EQ(eval_print(
        "(subseq (make-array 5 :element-type '(unsigned-byte 8)"
        " :initial-contents '(3 1 4 1 5)) 1 4)"),
        "#(1 4 1)");
    ASSERT_STR_EQ(eval_print(
        "(array-element-type (subseq (make-array 2 :element-type"
        " '(unsigned-byte 8)) 0))"),
        "(UNSIGNED-BYTE 8)");
}

TEST(seq_setf_subseq)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 5 :element-type '(unsigned-byte 8))))"
        "  (setf (subseq v 1 4) (make-array 3 :element-type '(unsigned-byte 8)"
        "                          :initial-contents '(9 8 7)))"
        "  v)"),
        "#(0 9 8 7 0)");
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 3 :element-type '(unsigned-byte 8))))"
        "  (setf (subseq v 0) '(1 2 3)) v)"),
        "#(1 2 3)");
}

TEST(seq_copy_reverse_nreverse)
{
    ASSERT_STR_EQ(eval_print(
        "(let* ((v (make-array 3 :element-type '(unsigned-byte 8)"
        "            :initial-contents '(1 2 3)))"
        "       (c (copy-seq v)))"
        "  (list (reverse v) v (eq c v) (array-element-type c)))"),
        "(#(3 2 1) #(1 2 3) NIL (UNSIGNED-BYTE 8))");
    ASSERT_STR_EQ(eval_print(
        "(nreverse (make-array 3 :element-type '(unsigned-byte 8)"
        " :initial-contents '(1 2 3)))"),
        "#(3 2 1)");
}

TEST(seq_scans)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 5 :element-type '(unsigned-byte 8)"
        " :initial-contents '(3 1 4 1 5))))"
        "  (list (position 4 v) (count 1 v) (find 5 v)"
        "        (some #'evenp v) (every #'integerp v) (reduce #'+ v)))"),
        "(2 2 5 T T 14)");
}

TEST(seq_sort_replace_concatenate)
{
    ASSERT_STR_EQ(eval_print(
        "(sort (make-array 5 :element-type '(unsigned-byte 8)"
        " :initial-contents '(3 1 4 1 5)) #'<)"),
        "#(1 1 3 4 5)");
    ASSERT_STR_EQ(eval_print(
        "(replace (make-array 3 :element-type '(unsigned-byte 8))"
        " (make-array 3 :element-type '(unsigned-byte 8)"
        " :initial-contents '(1 2 3)))"),
        "#(1 2 3)");
    ASSERT_STR_EQ(eval_print(
        "(concatenate 'list (make-array 2 :element-type '(unsigned-byte 8)"
        " :initial-contents '(1 2)) '(3))"),
        "(1 2 3)");
}

TEST(seq_map_and_map_into)
{
    ASSERT_STR_EQ(eval_print(
        "(map 'list #'1+ (make-array 3 :element-type '(unsigned-byte 8)"
        " :initial-contents '(1 2 3)))"),
        "(2 3 4)");
    ASSERT_STR_EQ(eval_print(
        "(map-into (make-array 3 :element-type '(unsigned-byte 8))"
        " #'+ #(1 2 3) #(10 20 30))"),
        "#(11 22 33)");
}

TEST(coerce_roundtrip)
{
    ASSERT_STR_EQ(eval_print(
        "(coerce (make-array 3 :element-type '(unsigned-byte 8)"
        " :initial-contents '(1 2 3)) 'list)"),
        "(1 2 3)");
    ASSERT_STR_EQ(eval_print(
        "(array-element-type (coerce '(1 2 3) '(vector (unsigned-byte 8))))"),
        "(UNSIGNED-BYTE 8)");
    ASSERT_STR_EQ(eval_print(
        "(coerce '(-1 127) '(vector (signed-byte 8)))"),
        "#(-1 127)");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (coerce '(1 300) '(vector (unsigned-byte 8)))"
        " (type-error () :caught))"),
        ":CAUGHT");
}

/* Regression: CONCATENATE/COERCE with a byte element-type in the result-type
 * spec must build the packed byte-vector kind — and must expand deftype
 * aliases (e.g. OCTET) exactly like MAKE-ARRAY/TYPEP do.  Previously
 * CONCATENATE always built a general vector (and COERCE missed aliases), so
 * (typep (concatenate '(vector octet) ...) '(vector octet)) was NIL — the
 * knx-conn ack--to-bytes failure. */
TEST(concatenate_coerce_packed_result_types)
{
    ASSERT_STR_EQ(eval_print(
        "(array-element-type (concatenate '(vector (unsigned-byte 8))"
        " '(1 2) (make-array 2 :element-type '(unsigned-byte 8)"
        " :initial-contents '(3 4)) (vector 5)))"),
        "(UNSIGNED-BYTE 8)");
    ASSERT_STR_EQ(eval_print(
        "(concatenate '(vector (unsigned-byte 8)) '(1 2) #(3 4))"),
        "#(1 2 3 4)");
    /* Deftype alias element type — the knx-conn shape. */
    ASSERT_STR_EQ(eval_print(
        "(progn (deftype cc-octet () '(unsigned-byte 8))"
        " (typep (concatenate '(vector cc-octet) #(6 16 4 33) '(0 10))"
        "        '(vector cc-octet)))"),
        "T");
    ASSERT_STR_EQ(eval_print(
        "(array-element-type (coerce #(6 16 4 33) '(vector cc-octet)))"),
        "(UNSIGNED-BYTE 8)");
    /* (simple-array (unsigned-byte 8) (*)) spelling (ironclad-style). */
    ASSERT_STR_EQ(eval_print(
        "(array-element-type (concatenate"
        " '(simple-array (unsigned-byte 8) (*)) '(1 2 3)))"),
        "(UNSIGNED-BYTE 8)");
    /* Signed and 16-bit result types. */
    ASSERT_STR_EQ(eval_print(
        "(let ((v (concatenate '(vector (signed-byte 8)) '(-128 -1) '(127))))"
        "  (list (array-element-type v) v))"),
        "((SIGNED-BYTE 8) #(-128 -1 127))");
    ASSERT_STR_EQ(eval_print(
        "(array-element-type (concatenate '(vector (unsigned-byte 16))"
        " '(1000 65535)))"),
        "(UNSIGNED-BYTE 16)");
    /* Elements outside the packed range signal a catchable TYPE-ERROR. */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (concatenate '(vector (unsigned-byte 8)) '(1 300))"
        " (type-error () :caught))"),
        ":CAUGHT");
    /* A declared length constraint is still enforced on the packed path. */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (concatenate '(vector (unsigned-byte 8) 5) '(1 2))"
        " (error () :caught))"),
        ":CAUGHT");
}

/* ========================================================
 * Step 7: equality + hashing
 * ======================================================== */

TEST(equalp_content)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((a (make-array 3 :element-type '(unsigned-byte 8)"
        "           :initial-contents '(1 2 3)))"
        "      (b (make-array 3 :element-type '(unsigned-byte 8)"
        "           :initial-contents '(1 2 3))))"
        "  (list (equalp a b) (equal a b) (eq a b)))"),
        "(T NIL NIL)");
    /* EQUALP compares element values across specializations */
    ASSERT_STR_EQ(eval_print(
        "(equalp (make-array 2 :element-type '(unsigned-byte 8)"
        " :initial-contents '(1 2)) (vector 1 2))"),
        "T");
}

TEST(equalp_hash_table_keys)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((h (make-hash-table :test 'equalp)))"
        "  (setf (gethash (make-array 2 :element-type '(unsigned-byte 8)"
        "                   :initial-contents '(1 2)) h) :hit)"
        "  (gethash (make-array 2 :element-type '(unsigned-byte 8)"
        "             :initial-contents '(1 2)) h))"),
        ":HIT");
    /* Cross-type: a byte-vector key must also be findable via an
     * EQUALP-equal general vector or bit-vector, and vice versa (CLHS
     * EQUALP is defined over content, not concrete array type). */
    ASSERT_STR_EQ(eval_print(
        "(let ((h (make-hash-table :test 'equalp)))"
        "  (setf (gethash (make-array 2 :element-type '(unsigned-byte 8)"
        "                   :initial-contents '(1 2)) h) :from-bv)"
        "  (gethash (vector 1 2) h))"),
        ":FROM-BV");
    ASSERT_STR_EQ(eval_print(
        "(let ((h (make-hash-table :test 'equalp)))"
        "  (setf (gethash (vector 1 0) h) :from-vec)"
        "  (gethash (make-array 2 :element-type '(unsigned-byte 8)"
        "             :initial-contents '(1 0)) h))"),
        ":FROM-VEC");
    ASSERT_STR_EQ(eval_print(
        "(let ((h (make-hash-table :test 'equalp)))"
        "  (setf (gethash (make-array 2 :element-type '(unsigned-byte 8)"
        "                   :initial-contents '(1 0)) h) :from-bv-vs-bit)"
        "  (gethash #*10 h))"),
        ":FROM-BV-VS-BIT");
}

/* ========================================================
 * Step 8: printer
 * ======================================================== */

TEST(printer_forms)
{
    ASSERT_STR_EQ(eval_print(
        "(make-array 3 :element-type '(unsigned-byte 8)"
        " :initial-contents '(0 128 255))"),
        "#(0 128 255)");
    ASSERT_STR_EQ(eval_print(
        "(make-array 2 :element-type '(signed-byte 8)"
        " :initial-contents '(-1 -128))"),
        "#(-1 -128)");
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-array* nil))"
        "  (prin1-to-string (make-array 2 :element-type '(unsigned-byte 8))))"),
        "\"#<BYTE-VECTOR>\"");
}

/* ========================================================
 * Step 10: remove/delete family (slice 2)
 * ======================================================== */

TEST(remove_delete_family)
{
    /* REMOVE returns a fresh PACKED vector and leaves the source intact
     * (CLHS 17.3: result element type = source element type). */
    ASSERT_STR_EQ(eval_print(
        "(let* ((v (make-array 5 :element-type '(unsigned-byte 8)"
        "            :initial-contents '(3 1 4 1 5)))"
        "       (r (remove 1 v)))"
        "  (list r (array-element-type r) (equalp v #(3 1 4 1 5))))"),
        "(#(3 4 5) (UNSIGNED-BYTE 8) T)");
    /* Signed variant */
    ASSERT_STR_EQ(eval_print(
        "(let ((r (remove -1 (make-array 4 :element-type '(signed-byte 8)"
        "                      :initial-contents '(-1 2 -1 -128)))))"
        "  (list r (array-element-type r)))"),
        "(#(2 -128) (SIGNED-BYTE 8))");
    /* Keyword arguments: :count, :count+:from-end, :start, :end */
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 5 :element-type '(unsigned-byte 8)"
        "           :initial-contents '(3 1 4 1 5))))"
        "  (list (remove 1 v :count 1)"
        "        (remove 1 v :count 1 :from-end t)"
        "        (remove 1 v :start 2)"
        "        (remove 1 v :end 2)))"),
        "(#(3 4 1 5) #(3 1 4 5) #(3 1 4 5) #(3 4 1 5))");
    /* :key and :test run user code against fixnum elements */
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 5 :element-type '(unsigned-byte 8)"
        "           :initial-contents '(3 1 4 1 5))))"
        "  (list (remove 2 v :key #'1+)"
        "        (remove 1 v :test #'<)"
        "        (remove 1 v :test-not #'eql)))"),
        "(#(3 4 5) #(1 1) #(1 1))");
    /* remove-if / remove-if-not */
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 5 :element-type '(unsigned-byte 8)"
        "           :initial-contents '(3 1 4 1 5))))"
        "  (list (remove-if #'oddp v) (remove-if-not #'oddp v)))"),
        "(#(4) #(3 1 1 5))");
    /* DELETE family (may destroy the source; result is what matters) */
    ASSERT_STR_EQ(eval_print(
        "(let ((r (delete 1 (make-array 5 :element-type '(unsigned-byte 8)"
        "                     :initial-contents '(3 1 4 1 5)))))"
        "  (list r (array-element-type r)))"),
        "(#(3 4 5) (UNSIGNED-BYTE 8))");
    ASSERT_STR_EQ(eval_print(
        "(list (delete-if #'oddp (make-array 3 :element-type '(unsigned-byte 8)"
        "                          :initial-contents '(1 2 3)))"
        "      (delete-if-not #'oddp (make-array 3 :element-type '(unsigned-byte 8)"
        "                              :initial-contents '(1 2 3))))"),
        "(#(2) #(1 3))");
    /* No match → all elements survive; all match → empty packed vector */
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 2 :element-type '(unsigned-byte 8)"
        "           :initial-contents '(7 7))))"
        "  (list (remove 200 v) (remove 7 v)"
        "        (array-element-type (remove 7 v))))"),
        "(#(7 7) #() (UNSIGNED-BYTE 8))");
    /* A non-sequence still signals a clean type-error */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (remove 1 (make-hash-table))"
        "  (type-error () :caught))"),
        ":CAUGHT");
}

/* ========================================================
 * Step 11: deftype aliases as element types (typep / the)
 *
 * flexi-streams' (deftype octet () '(unsigned-byte 8)) regression: TYPEP
 * must expand user deftype aliases when matching array element types, or
 * (the (array octet *) v) inside string-to-octets signals a spurious
 * type-error and every hunchentoot response dies.
 * ======================================================== */

TEST(typep_deftype_alias_element_type)
{
    eval_print("(deftype tbv-octet () '(unsigned-byte 8))");
    eval_print("(deftype tbv-s8 () '(integer -128 127))");
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 2 :element-type '(unsigned-byte 8))))"
        "  (list (typep v '(vector tbv-octet))"
        "        (typep v '(array tbv-octet *))"
        "        (typep v '(array tbv-octet))"
        "        (typep v '(simple-array tbv-octet (*)))"
        "        (typep v '(vector tbv-s8))))"),
        "(T T T T NIL)");
    /* Signed byte vectors match the signed alias, not the unsigned one */
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 2 :element-type '(signed-byte 8))))"
        "  (list (typep v '(vector tbv-s8)) (typep v '(vector tbv-octet))))"),
        "(T NIL)");
    /* Non-byte arrays never match the aliases */
    ASSERT_STR_EQ(eval_print(
        "(list (typep (make-array 2) '(vector tbv-octet))"
        "      (typep #*10 '(vector tbv-octet))"
        "      (typep \"ab\" '(vector tbv-octet)))"),
        "(NIL NIL NIL)");
    /* The exact flexi-streams string-to-octets pattern: THE + setf aref
     * at default safety must not signal */
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 3 :element-type 'tbv-octet)))"
        "  (setf (aref (the (array tbv-octet *) v) 0) 65)"
        "  (aref v 0))"),
        "65");
}

/* ========================================================
 * Step 11b: multi-dim :displaced-to onto a packed byte vector
 *
 * serapeum's RESHAPE displaces a multi-dim array onto RANGE vectors, which
 * pack to byte vectors since slice 1.  The packed heap type cannot back a
 * live CL_Obj view, so make-array copies the window (values + byte element
 * annotation) — same documented limitation as the string/bit-vector
 * displaced paths: mutations of the target do not propagate.
 * ======================================================== */

TEST(displaced_multidim_onto_byte_vector)
{
    ASSERT_STR_EQ(eval_print(
        "(let* ((v (make-array 6 :element-type '(unsigned-byte 8)"
        "            :initial-contents '(1 2 3 4 5 6)))"
        "       (a (make-array '(2 3) :displaced-to v)))"
        "  (list (array-dimensions a) (aref a 0 0) (aref a 1 2)"
        "        (array-element-type a)))"),
        "((2 3) 1 6 (UNSIGNED-BYTE 8))");
    /* :displaced-index-offset and signed sources */
    ASSERT_STR_EQ(eval_print(
        "(let* ((v (make-array 6 :element-type '(signed-byte 8)"
        "            :initial-contents '(-1 -2 -3 -4 -5 -6)))"
        "       (a (make-array '(2 2) :displaced-to v"
        "                      :displaced-index-offset 2)))"
        "  (list (aref a 0 0) (aref a 1 1) (array-element-type a)))"),
        "(-3 -6 (SIGNED-BYTE 8))");
    /* Out-of-bounds window still signals cleanly */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (make-array '(2 4) :displaced-to"
        "                (make-array 6 :element-type '(unsigned-byte 8)))"
        "  (error () :caught))"),
        ":CAUGHT");
}

/* ========================================================
 * Step 12: subtypep must respect upgraded element types
 *
 * CLHS 15: an array type is a subtype of another only when the UPGRADED
 * element types agree.  The TID hierarchy used to drop element types,
 * making ALL specialized array types mutual subtypes — serapeum's
 * WITH-TYPE-DISPATCH dedups branches via subtypep, so the contradiction
 * with TYPEP collapsed its dispatch and (serapeum:range -5 0) signalled
 * a type-error.
 * ======================================================== */

TEST(subtypep_array_element_types)
{
    /* Distinct element classes are certainly NOT subtypes, both ways */
    ASSERT_STR_EQ(eval_print(
        "(list (multiple-value-list (subtypep '(simple-array (signed-byte 8) (*))"
        "                                     '(simple-array integer (*))))"
        "      (multiple-value-list (subtypep '(simple-array integer (*))"
        "                                     '(simple-array (signed-byte 8) (*))))"
        "      (multiple-value-list (subtypep '(simple-array (unsigned-byte 8) (*))"
        "                                     '(simple-array (signed-byte 8) (*))))"
        "      (multiple-value-list (subtypep '(simple-array bit (*))"
        "                                     '(simple-array integer (*)))))"),
        "((NIL T) (NIL T) (NIL T) (NIL T))");
    /* Same element class remains a subtype */
    ASSERT_STR_EQ(eval_print(
        "(list (multiple-value-list (subtypep '(simple-array (unsigned-byte 8) (*))"
        "                                     '(simple-array (unsigned-byte 8) (*))))"
        "      (multiple-value-list (subtypep '(vector (unsigned-byte 8))"
        "                                     '(array (unsigned-byte 8))))"
        "      (multiple-value-list (subtypep '(vector tbv-octet)"
        "                                     '(vector (unsigned-byte 8)))))"),
        "((T T) (T T) (T T))");
    /* Wildcard rules: constrained ⊆ wild, wild ⊄ constrained */
    ASSERT_STR_EQ(eval_print(
        "(list (multiple-value-list (subtypep '(vector (unsigned-byte 8)) 'vector))"
        "      (multiple-value-list (subtypep '(vector (unsigned-byte 8)) 'sequence))"
        "      (multiple-value-list (subtypep 'vector '(vector (unsigned-byte 8))))"
        "      (multiple-value-list (subtypep 'vector '(vector t))))"),
        "((T T) (T T) (NIL T) (NIL T))");
    /* (vector character) ≡ string, (vector bit) ≡ bit-vector in the
     * hierarchy */
    ASSERT_STR_EQ(eval_print(
        "(list (multiple-value-list (subtypep '(vector character) 'string))"
        "      (multiple-value-list (subtypep '(vector bit) 'bit-vector))"
        "      (multiple-value-list (subtypep 'string '(vector character)))"
        "      (multiple-value-list (subtypep 'bit-vector '(array bit))))"),
        "((T T) (T T) (T T) (T T))");
    /* TYPEP and SUBTYPEP must agree: a byte vector is a member of every
     * type it is a subtype of and of no type subtypep certainly excludes
     * (the serapeum with-type-dispatch contract) */
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 2 :element-type '(signed-byte 8)))"
        "      (this '(simple-array (signed-byte 8) (*))))"
        "  (list (typep v this)"
        "        (multiple-value-list (subtypep this '(simple-array integer (*))))"
        "        (typep v '(simple-array integer (*)))))"),
        "(T (NIL T) NIL)");
}

/* ========================================================
 * Step 9: FASL round trip
 * ======================================================== */

TEST(fasl_roundtrip)
{
    /* Serialize a byte vector through the FASL constant writer/reader. */
    extern void cl_fasl_writer_init(CL_FaslWriter *w, uint8_t *buf, uint32_t capacity);
    static uint8_t buf[256];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_Obj bv = cl_make_byte_vector(4, 1, 0);
    CL_ByteVector *b = (CL_ByteVector *)CL_OBJ_TO_PTR(bv);
    CL_Obj back;
    b->data[0] = 0x00; b->data[1] = 0x7F; b->data[2] = 0x80; b->data[3] = 0xFF;

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, bv);
    ASSERT_EQ_INT(FASL_OK, w.error);
    cl_fasl_writer_release(&w);

    cl_fasl_reader_init(&r, buf, w.pos);
    back = cl_fasl_deserialize_obj(&r);
    ASSERT_EQ_INT(FASL_OK, r.error);
    ASSERT(CL_BYTE_VECTOR_P(back));
    b = (CL_ByteVector *)CL_OBJ_TO_PTR(back);
    ASSERT_EQ_INT(4, (int)b->length);
    ASSERT_EQ_INT(1, (int)b->is_signed);
    ASSERT_EQ_INT(0x00, (int)b->data[0]);
    ASSERT_EQ_INT(0x7F, (int)b->data[1]);
    ASSERT_EQ_INT(0x80, (int)b->data[2]);
    ASSERT_EQ_INT(0xFF, (int)b->data[3]);
}

/* ========================================================
 * Step 13 (slice 3): packed 16-bit kinds —
 * (unsigned-byte 16) / (signed-byte 16), 2 bytes per element
 * ======================================================== */

TEST(w16_alloc_layout_and_accessors)
{
    /* 2 bytes per element, elt_shift = 1, zero-filled, get/set round-trip
     * over the full range including the sign boundary. */
    CL_Obj u = cl_make_byte_vector(32, 0, 1);
    CL_ByteVector *b = (CL_ByteVector *)CL_OBJ_TO_PTR(u);
    uint32_t i;
    ASSERT(CL_BYTE_VECTOR_P(u));
    ASSERT_EQ_INT(32, (int)b->length);
    ASSERT_EQ_INT(1, (int)b->elt_shift);
    ASSERT(CL_HDR_SIZE(b) < sizeof(CL_ByteVector) + 2 * 32 + CL_ALIGN);
    for (i = 0; i < 32; i++)
        ASSERT_EQ_INT(0, (int)cl_bytevec_get(b, i));
    cl_bytevec_set(b, 0, 65535);
    cl_bytevec_set(b, 1, 0x1234);
    ASSERT_EQ_INT(65535, (int)cl_bytevec_get(b, 0));
    ASSERT_EQ_INT(0x1234, (int)cl_bytevec_get(b, 1));
    /* signed: same raw bits sign-extend */
    b = (CL_ByteVector *)CL_OBJ_TO_PTR(cl_make_byte_vector(2, 1, 1));
    cl_bytevec_set(b, 0, -32768);
    cl_bytevec_set(b, 1, -1);
    ASSERT_EQ_INT(-32768, (int)cl_bytevec_get(b, 0));
    ASSERT_EQ_INT(-1, (int)cl_bytevec_get(b, 1));
}

TEST(w16_make_array_routing)
{
    CL_Obj v = cl_eval_string("(make-array 4 :element-type '(unsigned-byte 16))");
    ASSERT(CL_BYTE_VECTOR_P(v));
    ASSERT_EQ_INT(0, (int)((CL_ByteVector *)CL_OBJ_TO_PTR(v))->is_signed);
    ASSERT_EQ_INT(1, (int)((CL_ByteVector *)CL_OBJ_TO_PTR(v))->elt_shift);
    v = cl_eval_string("(make-array 4 :element-type '(signed-byte 16))");
    ASSERT(CL_BYTE_VECTOR_P(v));
    ASSERT_EQ_INT(1, (int)((CL_ByteVector *)CL_OBJ_TO_PTR(v))->is_signed);
    ASSERT_EQ_INT(1, (int)((CL_ByteVector *)CL_OBJ_TO_PTR(v))->elt_shift);
    /* Ranges that need 9-16 bits upgrade to the 16-bit kinds; 8-bit ranges
     * keep the tighter 1-byte representation. */
    v = cl_eval_string("(make-array 4 :element-type '(mod 65536))");
    ASSERT(CL_BYTE_VECTOR_P(v) &&
           ((CL_ByteVector *)CL_OBJ_TO_PTR(v))->elt_shift == 1);
    v = cl_eval_string("(make-array 4 :element-type '(integer 0 1000))");
    ASSERT(CL_BYTE_VECTOR_P(v) &&
           ((CL_ByteVector *)CL_OBJ_TO_PTR(v))->elt_shift == 1 &&
           ((CL_ByteVector *)CL_OBJ_TO_PTR(v))->is_signed == 0);
    v = cl_eval_string("(make-array 4 :element-type '(integer -5 1000))");
    ASSERT(CL_BYTE_VECTOR_P(v) &&
           ((CL_ByteVector *)CL_OBJ_TO_PTR(v))->elt_shift == 1 &&
           ((CL_ByteVector *)CL_OBJ_TO_PTR(v))->is_signed == 1);
    v = cl_eval_string("(make-array 4 :element-type '(integer 0 255))");
    ASSERT(CL_BYTE_VECTOR_P(v) &&
           ((CL_ByteVector *)CL_OBJ_TO_PTR(v))->elt_shift == 0);
}

TEST(w16_init_and_boundaries)
{
    ASSERT_STR_EQ(eval_print(
        "(make-array 3 :element-type '(unsigned-byte 16) :initial-element 40000)"),
        "#(40000 40000 40000)");
    ASSERT_STR_EQ(eval_print(
        "(make-array 4 :element-type '(unsigned-byte 16)"
        " :initial-contents '(0 255 256 65535))"),
        "#(0 255 256 65535)");
    ASSERT_STR_EQ(eval_print(
        "(make-array 3 :element-type '(signed-byte 16)"
        " :initial-contents '(-32768 0 32767))"),
        "#(-32768 0 32767)");
    /* aref/setf at the exact boundaries */
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 2 :element-type '(signed-byte 16))))"
        "  (setf (aref v 0) -32768 (aref v 1) 32767)"
        "  (list (aref v 0) (aref v 1)))"),
        "(-32768 32767)");
}

TEST(w16_range_errors)
{
    /* One-past-the-boundary stores must signal a catchable TYPE-ERROR on
     * every store path: setf aref (builtin + OP_ASET), initial-element,
     * initial-contents, fill, coerce. */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (setf (aref (make-array 1 :element-type"
        " '(unsigned-byte 16)) 0) 65536) (type-error () :caught))"),
        ":CAUGHT");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (setf (aref (make-array 1 :element-type"
        " '(unsigned-byte 16)) 0) -1) (type-error () :caught))"),
        ":CAUGHT");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (setf (aref (make-array 1 :element-type"
        " '(signed-byte 16)) 0) 32768) (type-error () :caught))"),
        ":CAUGHT");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (setf (aref (make-array 1 :element-type"
        " '(signed-byte 16)) 0) -32769) (type-error () :caught))"),
        ":CAUGHT");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (make-array 2 :element-type '(unsigned-byte 16)"
        " :initial-element 100000) (type-error () :caught))"),
        ":CAUGHT");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (fill (make-array 2 :element-type '(unsigned-byte 16))"
        " 65536) (type-error () :caught))"),
        ":CAUGHT");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (coerce '(1 65536) '(vector (unsigned-byte 16)))"
        " (type-error () :caught))"),
        ":CAUGHT");
}

TEST(w16_types_and_upgrading)
{
    ASSERT_STR_EQ(eval_print(
        "(array-element-type (make-array 2 :element-type '(unsigned-byte 16)))"),
        "(UNSIGNED-BYTE 16)");
    ASSERT_STR_EQ(eval_print(
        "(array-element-type (make-array 2 :element-type '(signed-byte 16)))"),
        "(SIGNED-BYTE 16)");
    ASSERT_STR_EQ(eval_print("(upgraded-array-element-type '(unsigned-byte 16))"),
                  "(UNSIGNED-BYTE 16)");
    ASSERT_STR_EQ(eval_print("(upgraded-array-element-type '(unsigned-byte 9))"),
                  "(UNSIGNED-BYTE 16)");
    ASSERT_STR_EQ(eval_print("(upgraded-array-element-type '(integer -5 1000))"),
                  "(SIGNED-BYTE 16)");
    ASSERT_STR_EQ(eval_print("(upgraded-array-element-type '(unsigned-byte 17))"),
                  "T");
    ASSERT_STR_EQ(eval_print(
        "(type-of (make-array 3 :element-type '(unsigned-byte 16)))"),
        "(VECTOR (UNSIGNED-BYTE 16) 3)");
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 3 :element-type '(signed-byte 16)))) "
        "(typep v (type-of v)))"), "T");
    /* typep discriminates all three packed element classes */
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 2 :element-type '(unsigned-byte 16))))"
        "  (list (typep v '(vector (unsigned-byte 16)))"
        "        (typep v '(vector (unsigned-byte 8)))"
        "        (typep v '(vector (signed-byte 16)))"
        "        (typep v '(vector t)) (typep v 'vector)))"),
        "(T NIL NIL NIL T)");
    /* subtypep: distinct upgraded element classes are certainly disjoint */
    ASSERT_STR_EQ(eval_print(
        "(list (multiple-value-list (subtypep '(vector (unsigned-byte 16))"
        "                                     '(vector (unsigned-byte 8))))"
        "      (multiple-value-list (subtypep '(vector (unsigned-byte 8))"
        "                                     '(vector (unsigned-byte 16))))"
        "      (multiple-value-list (subtypep '(vector (unsigned-byte 16))"
        "                                     '(vector (unsigned-byte 16)))))"),
        "((NIL T) (NIL T) (T T))");
    /* deftype aliases expand for the 16-bit kinds too */
    eval_print("(deftype tbv-u16 () '(unsigned-byte 16))");
    ASSERT_STR_EQ(eval_print(
        "(typep (make-array 2 :element-type '(unsigned-byte 16))"
        " '(vector tbv-u16))"), "T");
}

TEST(w16_sequences)
{
    ASSERT_STR_EQ(eval_print(
        "(fill (make-array 3 :element-type '(unsigned-byte 16)) 12345)"),
        "#(12345 12345 12345)");
    ASSERT_STR_EQ(eval_print(
        "(fill (make-array 4 :element-type '(unsigned-byte 16)"
        " :initial-element 1) 999 :start 1 :end 3)"),
        "#(1 999 999 1)");
    ASSERT_STR_EQ(eval_print(
        "(subseq (make-array 5 :element-type '(unsigned-byte 16)"
        " :initial-contents '(300 1000 40000 1000 500)) 1 4)"),
        "#(1000 40000 1000)");
    ASSERT_STR_EQ(eval_print(
        "(array-element-type (subseq (make-array 2 :element-type"
        " '(signed-byte 16)) 0))"),
        "(SIGNED-BYTE 16)");
    ASSERT_STR_EQ(eval_print(
        "(let* ((v (make-array 3 :element-type '(unsigned-byte 16)"
        "            :initial-contents '(256 512 65535)))"
        "       (c (copy-seq v)))"
        "  (list (reverse v) (eq c v) (array-element-type c)))"),
        "(#(65535 512 256) NIL (UNSIGNED-BYTE 16))");
    ASSERT_STR_EQ(eval_print(
        "(nreverse (make-array 3 :element-type '(signed-byte 16)"
        " :initial-contents '(-1 0 -32768)))"),
        "#(-32768 0 -1)");
    ASSERT_STR_EQ(eval_print(
        "(sort (make-array 5 :element-type '(unsigned-byte 16)"
        " :initial-contents '(3000 100 40000 100 500)) #'<)"),
        "#(100 100 500 3000 40000)");
    ASSERT_STR_EQ(eval_print(
        "(replace (make-array 3 :element-type '(unsigned-byte 16))"
        " (make-array 3 :element-type '(unsigned-byte 16)"
        " :initial-contents '(1000 2000 3000)))"),
        "#(1000 2000 3000)");
    /* (setf subseq) same-width fast path AND cross-width element-wise path */
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 4 :element-type '(unsigned-byte 16))))"
        "  (setf (subseq v 1 3) (make-array 2 :element-type '(unsigned-byte 16)"
        "                          :initial-contents '(40000 50000)))"
        "  v)"),
        "#(0 40000 50000 0)");
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 3 :element-type '(unsigned-byte 16))))"
        "  (setf (subseq v 0) (make-array 3 :element-type '(unsigned-byte 8)"
        "                        :initial-contents '(1 2 255)))"
        "  v)"),
        "#(1 2 255)");
    /* cross-width store still range-checks: u16 values into a u8 target */
    ASSERT_STR_EQ(eval_print(
        "(handler-case"
        "  (let ((v (make-array 2 :element-type '(unsigned-byte 8))))"
        "    (setf (subseq v 0) (make-array 2 :element-type '(unsigned-byte 16)"
        "                          :initial-contents '(1 300))))"
        "  (type-error () :caught))"),
        ":CAUGHT");
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 5 :element-type '(unsigned-byte 16)"
        " :initial-contents '(300 100 4000 100 50000))))"
        "  (list (position 4000 v) (count 100 v) (find 50000 v)"
        "        (reduce #'+ v)))"),
        "(2 2 50000 54500)");
    /* remove keeps the packed 16-bit result type */
    ASSERT_STR_EQ(eval_print(
        "(let ((r (remove 1000 (make-array 4 :element-type '(unsigned-byte 16)"
        "                        :initial-contents '(1000 40000 1000 2)))))"
        "  (list r (array-element-type r)))"),
        "(#(40000 2) (UNSIGNED-BYTE 16))");
}

TEST(w16_fill_pointer_push_pop_adjust)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 3 :element-type '(unsigned-byte 16) :fill-pointer 0)))"
        "  (list (vector-push 40000 v) (vector-push 65535 v) (fill-pointer v)"
        "        (vector-pop v) (fill-pointer v)))"),
        "(0 1 2 65535 1)");
    ASSERT_STR_EQ(eval_print(
        "(let* ((v (make-array 2 :element-type '(unsigned-byte 16)"
        "             :initial-contents '(40000 50000)))"
        "       (w (adjust-array v 4 :initial-element 60000)))"
        "  (list (eq v w) w (array-element-type w)))"),
        "(NIL #(40000 50000 60000 60000) (UNSIGNED-BYTE 16))");
    /* adjustable 16-bit arrays use the annotated general-vector fallback
     * and still report their element type */
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 2 :element-type '(unsigned-byte 16)"
        "            :adjustable t :fill-pointer 0)))"
        "  (dotimes (i 4) (vector-push-extend (* i 10000) v))"
        "  (list (adjustable-array-p v) (array-element-type v)"
        "        (typep v '(vector (unsigned-byte 16))) (aref v 3)))"),
        "(T (UNSIGNED-BYTE 16) T 30000)");
    {
        CL_Obj v = cl_eval_string(
            "(make-array 4 :element-type '(unsigned-byte 16) :adjustable t)");
        ASSERT(!CL_BYTE_VECTOR_P(v));
        ASSERT(CL_VECTOR_P(v));
    }
}

TEST(w16_coerce_and_equalp)
{
    ASSERT_STR_EQ(eval_print(
        "(array-element-type (coerce '(1000 65535) '(vector (unsigned-byte 16))))"),
        "(UNSIGNED-BYTE 16)");
    ASSERT_STR_EQ(eval_print(
        "(coerce (make-array 3 :element-type '(unsigned-byte 16)"
        " :initial-contents '(1 40000 65535)) 'list)"),
        "(1 40000 65535)");
    /* coercing a u8 vector to u16 widens element-wise */
    ASSERT_STR_EQ(eval_print(
        "(let ((w (coerce (make-array 2 :element-type '(unsigned-byte 8)"
        "                   :initial-contents '(1 255))"
        "                 '(vector (unsigned-byte 16)))))"
        "  (list w (array-element-type w)))"),
        "(#(1 255) (UNSIGNED-BYTE 16))");
    /* EQUALP is content-based across element widths and concrete types */
    ASSERT_STR_EQ(eval_print(
        "(list (equalp (make-array 2 :element-type '(unsigned-byte 16)"
        "                :initial-contents '(1 2))"
        "              (make-array 2 :element-type '(unsigned-byte 8)"
        "                :initial-contents '(1 2)))"
        "      (equalp (make-array 2 :element-type '(unsigned-byte 16)"
        "                :initial-contents '(1 2)) (vector 1 2))"
        "      (equalp (make-array 2 :element-type '(unsigned-byte 16)"
        "                :initial-contents '(1 2))"
        "              (make-array 2 :element-type '(unsigned-byte 16)"
        "                :initial-contents '(1 3))))"),
        "(T T NIL)");
    /* equalp hash tables: 16-bit keys findable via equal-content u8 keys */
    ASSERT_STR_EQ(eval_print(
        "(let ((h (make-hash-table :test 'equalp)))"
        "  (setf (gethash (make-array 2 :element-type '(unsigned-byte 16)"
        "                   :initial-contents '(1 2)) h) :hit16)"
        "  (gethash (make-array 2 :element-type '(unsigned-byte 8)"
        "             :initial-contents '(1 2)) h))"),
        ":HIT16");
}

TEST(w16_displaced_multidim)
{
    ASSERT_STR_EQ(eval_print(
        "(let* ((v (make-array 6 :element-type '(unsigned-byte 16)"
        "            :initial-contents '(1000 2000 3000 4000 5000 60000)))"
        "       (a (make-array '(2 3) :displaced-to v)))"
        "  (list (aref a 0 0) (aref a 1 2) (array-element-type a)))"),
        "(1000 60000 (UNSIGNED-BYTE 16))");
    /* 1-D displaced window copy honours the element width */
    ASSERT_STR_EQ(eval_print(
        "(make-array 2 :element-type '(unsigned-byte 16) :displaced-to"
        " (make-array 4 :element-type '(unsigned-byte 16)"
        "   :initial-contents '(10 20 30000 40000))"
        " :displaced-index-offset 2)"),
        "#(30000 40000)");
}

TEST(w16_fasl_roundtrip_big_endian_wire)
{
    /* 16-bit payload must round-trip AND sit big-endian on the wire so a
     * FASL written on a little-endian host loads unchanged on m68k. */
    static uint8_t buf[256];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_Obj bv = cl_make_byte_vector(3, 0, 1);
    CL_ByteVector *b = (CL_ByteVector *)CL_OBJ_TO_PTR(bv);
    CL_Obj back;
    cl_bytevec_set(b, 0, 0x1234);
    cl_bytevec_set(b, 1, 0x0001);
    cl_bytevec_set(b, 2, 0xFFFF);

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, bv);
    ASSERT_EQ_INT(FASL_OK, w.error);
    cl_fasl_writer_release(&w);

    /* Wire layout: OBJ_DEF + u16 id (shared-object framing, v36), then tag,
     * u32 length (BE), u8 is_signed, u8 elt_shift, then per-element u16
     * big-endian. */
    ASSERT_EQ_INT(FASL_TAG_OBJ_DEF, (int)buf[0]);
    ASSERT_EQ_INT(FASL_TAG_BYTE_VECTOR, (int)buf[3]);
    ASSERT_EQ_INT(3, (int)buf[7]);       /* length LSB of big-endian u32 */
    ASSERT_EQ_INT(0, (int)buf[8]);       /* is_signed */
    ASSERT_EQ_INT(1, (int)buf[9]);       /* elt_shift */
    ASSERT_EQ_INT(0x12, (int)buf[10]);   /* element 0 high byte first */
    ASSERT_EQ_INT(0x34, (int)buf[11]);
    ASSERT_EQ_INT(0x00, (int)buf[12]);
    ASSERT_EQ_INT(0x01, (int)buf[13]);

    cl_fasl_reader_init(&r, buf, w.pos);
    back = cl_fasl_deserialize_obj(&r);
    ASSERT_EQ_INT(FASL_OK, r.error);
    ASSERT(CL_BYTE_VECTOR_P(back));
    b = (CL_ByteVector *)CL_OBJ_TO_PTR(back);
    ASSERT_EQ_INT(3, (int)b->length);
    ASSERT_EQ_INT(0, (int)b->is_signed);
    ASSERT_EQ_INT(1, (int)b->elt_shift);
    ASSERT_EQ_INT(0x1234, (int)cl_bytevec_get(b, 0));
    ASSERT_EQ_INT(0x0001, (int)cl_bytevec_get(b, 1));
    ASSERT_EQ_INT(0xFFFF, (int)cl_bytevec_get(b, 2));

    /* Signed variant keeps sign across the round trip */
    bv = cl_make_byte_vector(2, 1, 1);
    b = (CL_ByteVector *)CL_OBJ_TO_PTR(bv);
    cl_bytevec_set(b, 0, -32768);
    cl_bytevec_set(b, 1, 32767);
    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, bv);
    ASSERT_EQ_INT(FASL_OK, w.error);
    cl_fasl_writer_release(&w);
    cl_fasl_reader_init(&r, buf, w.pos);
    back = cl_fasl_deserialize_obj(&r);
    ASSERT_EQ_INT(FASL_OK, r.error);
    b = (CL_ByteVector *)CL_OBJ_TO_PTR(back);
    ASSERT_EQ_INT(-32768, (int)cl_bytevec_get(b, 0));
    ASSERT_EQ_INT(32767, (int)cl_bytevec_get(b, 1));
}

TEST(w16_io_ffi_reject_non_octet)
{
    /* Byte-copy interfaces (FFI poke-bytes, UDP buffers) must reject 16-bit
     * vectors loudly instead of silently reinterpreting 2-byte elements. */
    ASSERT_STR_EQ(eval_print(
        "(let ((p (ffi:alloc-foreign 8)))"
        "  (unwind-protect"
        "      (handler-case"
        "          (ffi:poke-bytes p (make-array 2 :element-type"
        "                              '(unsigned-byte 16)))"
        "        (error () :caught))"
        "    (ffi:free-foreign p)))"),
        ":CAUGHT");
}

/* ========================================================
 * Bulk fast paths: REPLACE memmove, MAP-INTO bitwise folds,
 * EXT:UNPACK-BYTERUN1 (PackBits RLE decode)
 * ======================================================== */

TEST(replace_u8_memmove_fast_path)
{
    /* Same element type: one memmove.  Verify exact CLHS :start/:end
     * semantics survive the fast path. */
    ASSERT_STR_EQ(eval_print(
        "(let ((a (make-array 8 :element-type '(unsigned-byte 8)"
        "                       :initial-element 7))"
        "      (b (make-array 8 :element-type '(unsigned-byte 8)"
        "                       :initial-contents '(1 2 3 4 5 6 7 8))))"
        "  (replace a b :start1 2 :start2 4 :end2 7))"),
        "#(7 7 5 6 7 7 7 7)");
}

TEST(replace_u8_overlap_same_object)
{
    /* CLHS: overlapping REPLACE on the same sequence behaves as if the
     * source were copied out first — memmove gives exactly that. */
    ASSERT_STR_EQ(eval_print(
        "(let ((a (make-array 5 :element-type '(unsigned-byte 8)"
        "                       :initial-contents '(1 2 3 4 5))))"
        "  (replace a a :start1 1 :start2 0 :end2 4))"),
        "#(1 1 2 3 4)");
    ASSERT_STR_EQ(eval_print(
        "(let ((a (make-array 5 :element-type '(unsigned-byte 8)"
        "                       :initial-contents '(1 2 3 4 5))))"
        "  (replace a a :start1 0 :start2 1))"),
        "#(2 3 4 5 5)");
}

TEST(replace_u16_memmove_fast_path)
{
    /* 16-bit elements: offsets scale by elt_shift; values > 255 survive. */
    ASSERT_STR_EQ(eval_print(
        "(let ((a (make-array 4 :element-type '(unsigned-byte 16)"
        "                       :initial-element 0))"
        "      (b (make-array 4 :element-type '(unsigned-byte 16)"
        "                       :initial-contents '(1000 2000 3000 4000))))"
        "  (replace a b :start1 1 :end1 3 :start2 2))"),
        "#(0 3000 4000 0)");
}

TEST(replace_mixed_signedness_keeps_type_check)
{
    /* u8 200 into an s8 vector must stay a type-error (the general path),
     * NOT a silent bit-pattern reinterpretation to -56. */
    ASSERT_STR_EQ(eval_print(
        "(handler-case"
        "    (replace (make-array 2 :element-type '(signed-byte 8))"
        "             (make-array 2 :element-type '(unsigned-byte 8)"
        "                           :initial-element 200))"
        "  (error () :caught))"),
        ":CAUGHT");
    /* In-range values still copy correctly through the general path. */
    ASSERT_STR_EQ(eval_print(
        "(replace (make-array 2 :element-type '(signed-byte 8))"
        "         (make-array 2 :element-type '(unsigned-byte 8)"
        "                       :initial-element 5))"),
        "#(5 5)");
}

TEST(replace_string_memmove_fast_path)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((a (copy-seq \"abcdef\")))"
        "  (replace a \"XYZ\" :start1 2 :end2 2))"),
        "\"abXYef\"");
}

TEST(map_into_logior_fast_path)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((a (make-array 4 :element-type '(unsigned-byte 8)"
        "                       :initial-contents '(1 2 4 8)))"
        "      (b (make-array 4 :element-type '(unsigned-byte 8)"
        "                       :initial-contents '(16 32 64 128))))"
        "  (map-into a #'logior a b))"),
        "#(17 34 68 136)");
    ASSERT_STR_EQ(eval_print(
        "(let ((a (make-array 3 :element-type '(unsigned-byte 8)"
        "                       :initial-contents '(15 240 255)))"
        "      (b (make-array 3 :element-type '(unsigned-byte 8)"
        "                       :initial-contents '(85 85 85))))"
        "  (list (map-into (make-array 3 :element-type '(unsigned-byte 8))"
        "                  #'logand a b)"
        "        (map-into (make-array 3 :element-type '(unsigned-byte 8))"
        "                  #'logxor a b)))"),
        "(#(5 80 85) #(90 165 170))");
}

TEST(map_into_fast_path_min_length_and_fill_pointer)
{
    /* Shorter source bounds the fold; a fill-pointered result gets its
     * fill pointer set to the count stored (CLHS MAP-INTO). */
    ASSERT_STR_EQ(eval_print(
        "(let ((a (make-array 5 :element-type '(unsigned-byte 8)"
        "                       :fill-pointer 1))"
        "      (b (make-array 3 :element-type '(unsigned-byte 8)"
        "                       :initial-contents '(1 2 3))))"
        "  (map-into a #'logior b b)"
        "  (list (fill-pointer a) (aref a 0) (aref a 2)))"),
        "(3 1 3)");
}

TEST(map_into_general_fn_still_works_on_byte_vectors)
{
    /* A non-builtin function must keep taking the general VM-apply path. */
    ASSERT_STR_EQ(eval_print(
        "(let ((a (make-array 3 :element-type '(unsigned-byte 8)"
        "                       :initial-contents '(1 2 3)))"
        "      (b (make-array 3 :element-type '(unsigned-byte 8)"
        "                       :initial-contents '(10 20 30))))"
        "  (map-into a (lambda (x y) (+ x y)) a b))"),
        "#(11 22 33)");
    /* Signed vectors skip the fast path but stay correct via the VM. */
    ASSERT_STR_EQ(eval_print(
        "(let ((a (make-array 2 :element-type '(signed-byte 8)"
        "                       :initial-contents '(-1 3))))"
        "  (map-into a #'logand a a))"),
        "#(-1 3)");
}

TEST(unpack_byterun1_literal_repeat_noop)
{
    /* Literal run (code 2 => 3 bytes), repeat run (253 => 4x), and the
     * code-128 no-op, decoded mid-stream with a dst offset. */
    ASSERT_STR_EQ(eval_print(
        "(let ((src (make-array 7 :element-type '(unsigned-byte 8)"
        "                         :initial-contents '(128 2 9 8 7 253 5)))"
        "      (dst (make-array 9 :element-type '(unsigned-byte 8)"
        "                         :initial-element 99)))"
        "  (list (ext:unpack-byterun1 src 0 7 dst 7 1) dst))"),
        "(7 #(99 9 8 7 5 5 5 5 99))");
}

TEST(unpack_byterun1_stops_at_dst_len)
{
    /* Decoding stops exactly at dst-len; trailing source bytes are left
     * unconsumed and the returned position points at them. */
    ASSERT_STR_EQ(eval_print(
        "(let ((src (make-array 4 :element-type '(unsigned-byte 8)"
        "                         :initial-contents '(255 1 0 42)))"
        "      (dst (make-array 2 :element-type '(unsigned-byte 8))))"
        "  (list (ext:unpack-byterun1 src 0 4 dst 2) dst))"),
        "(2 #(1 1))");
}

TEST(unpack_byterun1_error_cases)
{
    /* Truncated input: output still short when the source runs out. */
    ASSERT_STR_EQ(eval_print(
        "(handler-case"
        "    (ext:unpack-byterun1"
        "     (make-array 1 :element-type '(unsigned-byte 8)"
        "                   :initial-element 2)"
        "     0 1 (make-array 3 :element-type '(unsigned-byte 8)) 3)"
        "  (error () :caught))"),
        ":CAUGHT");
    /* Repeat run overflowing the requested output length. */
    ASSERT_STR_EQ(eval_print(
        "(handler-case"
        "    (ext:unpack-byterun1"
        "     (make-array 2 :element-type '(unsigned-byte 8)"
        "                   :initial-contents '(253 7))"
        "     0 2 (make-array 2 :element-type '(unsigned-byte 8)) 2)"
        "  (error () :caught))"),
        ":CAUGHT");
    /* Bad source range and non-byte-vector arguments. */
    ASSERT_STR_EQ(eval_print(
        "(handler-case"
        "    (ext:unpack-byterun1"
        "     (make-array 2 :element-type '(unsigned-byte 8)) 0 5"
        "     (make-array 2 :element-type '(unsigned-byte 8)) 2)"
        "  (error () :caught))"),
        ":CAUGHT");
    ASSERT_STR_EQ(eval_print(
        "(handler-case"
        "    (ext:unpack-byterun1 (vector 1 2) 0 2"
        "     (make-array 2 :element-type '(unsigned-byte 8)) 2)"
        "  (error () :caught))"),
        ":CAUGHT");
}

TEST(count_byte_vector_fast_path)
{
    /* Plain count, :start/:end, :from-end — all pure-EQL fast-path shapes;
     * results must match the CLHS general semantics exactly. */
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 8 :element-type '(unsigned-byte 8)"
        "            :initial-contents '(1 0 2 0 0 3 0 1))))"
        "  (list (count 0 v)"
        "        (count 0 v :start 2)"
        "        (count 0 v :start 2 :end 5)"
        "        (count 0 v :from-end t)"
        "        (count 9 v)"
        "        (count 300 v)"        /* outside u8 range: 0, no error */
        "        (count -1 v)))"),
        "(4 3 2 4 0 0 0)");
    /* Signed and 16-bit element types go through the same packed loop. */
    ASSERT_STR_EQ(eval_print(
        "(list (count -3 (make-array 4 :element-type '(signed-byte 8)"
        "                  :initial-contents '(-3 3 -3 0)))"
        "      (count 700 (make-array 3 :element-type '(unsigned-byte 16)"
        "                   :initial-contents '(700 7 700)))"
        "      (count -700 (make-array 2 :element-type '(signed-byte 16)"
        "                    :initial-contents '(-700 700))))"),
        "(2 2 1)");
    /* :key and a non-EQL :test leave the fast path — still correct. */
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 4 :element-type '(unsigned-byte 8)"
        "            :initial-contents '(1 2 3 4))))"
        "  (list (count 3 v :key #'1+)"
        "        (count 2 v :test #'<)))"),
        "(1 2)");
    /* Fill pointer bounds the active length. */
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 6 :element-type '(unsigned-byte 8)"
        "            :fill-pointer 3 :initial-contents '(7 7 0 7 7 7))))"
        "  (count 7 v))"),
        "2");
}

TEST(copy_rows_deinterleave)
{
    /* Gather every other pair out of an interleaved vector — the ILBM
     * plane-extraction shape (chunk 2, src stride 4, dst stride 2). */
    ASSERT_STR_EQ(eval_print(
        "(let ((src (make-array 12 :element-type '(unsigned-byte 8)"
        "              :initial-contents '(1 2 9 9 3 4 9 9 5 6 9 9)))"
        "      (dst (make-array 6 :element-type '(unsigned-byte 8)"
        "              :initial-element 0)))"
        "  (ext:copy-rows dst src 3 2 0 2 0 4))"),
        "#(1 2 3 4 5 6)");
    /* Scatter back out (dst stride wider than chunk): untouched gap
     * bytes keep their old value. */
    ASSERT_STR_EQ(eval_print(
        "(let ((src (make-array 4 :element-type '(unsigned-byte 8)"
        "              :initial-contents '(1 2 3 4)))"
        "      (dst (make-array 6 :element-type '(unsigned-byte 8)"
        "              :initial-element 9)))"
        "  (ext:copy-rows dst src 2 2 0 3 0 2))"),
        "#(1 2 9 3 4 9)");
}

TEST(copy_rows_zero_and_offsets)
{
    /* count 0 and chunk 0 are no-ops even with wild strides. */
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 2 :element-type '(unsigned-byte 8)"
        "            :initial-contents '(7 8))))"
        "  (ext:copy-rows v v 0 5 0 1000 0 1000)"
        "  (ext:copy-rows v v 3 0 0 1000 0 1000)"
        "  v)"),
        "#(7 8)");
    /* Non-zero starts on both sides. */
    ASSERT_STR_EQ(eval_print(
        "(let ((src (make-array 6 :element-type '(unsigned-byte 8)"
        "              :initial-contents '(0 0 1 0 2 3)))"
        "      (dst (make-array 5 :element-type '(unsigned-byte 8)"
        "              :initial-element 9)))"
        "  (ext:copy-rows dst src 2 1 1 2 2 2))"),
        "#(9 1 9 2 9)");
}

TEST(copy_rows_error_cases)
{
    /* Last row runs past the source. */
    ASSERT_STR_EQ(eval_print(
        "(handler-case"
        "    (ext:copy-rows (make-array 8 :element-type '(unsigned-byte 8))"
        "                   (make-array 8 :element-type '(unsigned-byte 8))"
        "                   3 2 0 2 0 4)"
        "  (error () :caught))"),
        ":CAUGHT");
    /* Last row runs past the destination. */
    ASSERT_STR_EQ(eval_print(
        "(handler-case"
        "    (ext:copy-rows (make-array 5 :element-type '(unsigned-byte 8))"
        "                   (make-array 8 :element-type '(unsigned-byte 8))"
        "                   3 2 0 2 0 2)"
        "  (error () :caught))"),
        ":CAUGHT");
    /* Negative stride and non-byte-vector arguments. */
    ASSERT_STR_EQ(eval_print(
        "(handler-case"
        "    (ext:copy-rows (make-array 4 :element-type '(unsigned-byte 8))"
        "                   (make-array 4 :element-type '(unsigned-byte 8))"
        "                   2 1 0 1 3 -1)"
        "  (error () :caught))"),
        ":CAUGHT");
    ASSERT_STR_EQ(eval_print(
        "(handler-case"
        "    (ext:copy-rows (vector 1 2)"
        "                   (make-array 2 :element-type '(unsigned-byte 8))"
        "                   1 1 0 1 0 1)"
        "  (error () :caught))"),
        ":CAUGHT");
}

int main(void)
{
    setup();

    RUN(alloc_zero);
    RUN(alloc_len_and_zero_fill);
    RUN(packed_size_is_one_byte_per_element);
    RUN(signedness_flag);
    RUN(signed_get_sign_extends);
    RUN(predicate_negative);
    RUN(gc_survives_with_contents);

    RUN(make_array_u8_is_packed);
    RUN(make_array_s8_is_packed);
    RUN(make_array_upgraded_specs);
    RUN(make_array_initial_element);
    RUN(make_array_initial_contents);
    RUN(make_array_initial_element_range_error);
    RUN(make_array_initial_contents_range_error);
    RUN(make_array_dims_list_spelling);

    RUN(aref_setf_aref);
    RUN(aref_signed_roundtrip);
    RUN(aref_bounds_error);
    RUN(setf_aref_range_error);
    RUN(elt_and_setf_elt);
    RUN(row_major_aref);

    RUN(array_element_type);
    RUN(upgraded_array_element_type);
    RUN(predicates_and_queries);
    RUN(typep_discrimination);
    RUN(type_of_roundtrips_through_typep);

    RUN(fill_pointer_push_pop);
    RUN(vector_push_full_returns_nil);
    RUN(vector_push_extend_full_errors_cleanly);
    RUN(adjust_array_fresh_copy);
    RUN(adjustable_byte_arrays_grow);

    RUN(seq_fill);
    RUN(seq_subseq);
    RUN(seq_setf_subseq);
    RUN(seq_copy_reverse_nreverse);
    RUN(seq_scans);
    RUN(seq_sort_replace_concatenate);
    RUN(seq_map_and_map_into);
    RUN(coerce_roundtrip);
    RUN(concatenate_coerce_packed_result_types);

    RUN(equalp_content);
    RUN(equalp_hash_table_keys);

    RUN(printer_forms);

    RUN(remove_delete_family);
    RUN(typep_deftype_alias_element_type);
    RUN(displaced_multidim_onto_byte_vector);
    RUN(subtypep_array_element_types);

    RUN(fasl_roundtrip);

    RUN(w16_alloc_layout_and_accessors);
    RUN(w16_make_array_routing);
    RUN(w16_init_and_boundaries);
    RUN(w16_range_errors);
    RUN(w16_types_and_upgrading);
    RUN(w16_sequences);
    RUN(w16_fill_pointer_push_pop_adjust);
    RUN(w16_coerce_and_equalp);
    RUN(w16_displaced_multidim);
    RUN(w16_fasl_roundtrip_big_endian_wire);
    RUN(w16_io_ffi_reject_non_octet);

    RUN(replace_u8_memmove_fast_path);
    RUN(replace_u8_overlap_same_object);
    RUN(replace_u16_memmove_fast_path);
    RUN(replace_mixed_signedness_keeps_type_check);
    RUN(replace_string_memmove_fast_path);
    RUN(map_into_logior_fast_path);
    RUN(map_into_fast_path_min_length_and_fill_pointer);
    RUN(map_into_general_fn_still_works_on_byte_vectors);
    RUN(count_byte_vector_fast_path);
    RUN(unpack_byterun1_literal_repeat_noop);
    RUN(unpack_byterun1_stops_at_dst_len);
    RUN(unpack_byterun1_error_cases);
    RUN(copy_rows_deinterleave);
    RUN(copy_rows_zero_and_offsets);
    RUN(copy_rows_error_cases);

    teardown();
    REPORT();
}
