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
    static char buf[512];
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
        return buf;
    }
}

/* Helper: eval and get fixnum value */
static int eval_int(const char *str)
{
    CL_Obj result = cl_eval_string(str);
    return CL_FIXNUM_VAL(result);
}

/* Helper: eval and expect an error, return 1 if error occurred */
static int eval_errors(const char *str)
{
    int err; CL_CATCH(err);
    if (err == CL_ERR_NONE) {
        cl_eval_string(str);
        CL_UNCATCH();
        return 0;
    } else {
        CL_UNCATCH();
        return 1;
    }
}

/* ============================================================ */
/* make-array: 1D construction                                  */
/* ============================================================ */

TEST(make_array_simple)
{
    /* Basic 1D array */
    ASSERT_EQ_INT(eval_int("(length (make-array 5))"), 5);
    ASSERT_EQ_INT(eval_int("(array-rank (make-array 5))"), 1);
    /* Default elements are NIL */
    ASSERT_STR_EQ(eval_print("(aref (make-array 3) 0)"), "NIL");
}

/* Regression: a negative or absurdly large dimension must signal a CATCHABLE
 * error (subtype of ERROR), not fatally abort the VM.  Previously a negative
 * fixnum dimension cast to a near-2^32 uint32_t and tripped cl_alloc's hard
 * header-size guard via cl_storage_error, which escapes ordinary
 * (handler-case ... (error () ...)).  A bad HTTP Range size in hunchentoot
 * triggered exactly this. */
TEST(make_array_bad_dimension_is_catchable)
{
    /* negative dimension -> caught */
    ASSERT_EQ_INT(eval_errors("(make-array -1)"), 1);
    /* over-limit (but still fixnum) dimension -> caught */
    ASSERT_EQ_INT(eval_errors("(make-array 20000000)"), 1);
    /* and it is a proper ERROR (handler-case (error) catches it) */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (make-array -1048552 :element-type '(unsigned-byte 8))"
        "  (error () :caught))"), ":CAUGHT");
    /* a legitimate large-but-valid array still works */
    ASSERT_EQ_INT(eval_int("(length (make-array 100000))"), 100000);
}

TEST(make_array_initial_element)
{
    ASSERT_EQ_INT(eval_int("(aref (make-array 3 :initial-element 42) 0)"), 42);
    ASSERT_EQ_INT(eval_int("(aref (make-array 3 :initial-element 42) 2)"), 42);
}

TEST(make_array_initial_contents)
{
    ASSERT_STR_EQ(eval_print("(make-array 3 :initial-contents '(10 20 30))"),
        "#(10 20 30)");
}

TEST(make_array_fill_pointer)
{
    /* :fill-pointer T starts at array size (CLHS) */
    ASSERT_EQ_INT(eval_int("(fill-pointer (make-array 10 :fill-pointer t))"), 10);
    /* :fill-pointer integer */
    ASSERT_EQ_INT(eval_int("(fill-pointer (make-array 10 :fill-pointer 5))"), 5);
    /* length respects fill pointer */
    ASSERT_EQ_INT(eval_int(
        "(length (make-array 10 :fill-pointer 3))"), 3);
}

TEST(make_array_fill_pointer_t_string)
{
    /* :fill-pointer T for character arrays should init to array size */
    ASSERT_EQ_INT(eval_int(
        "(fill-pointer (make-array 3 :element-type 'character :fill-pointer t :adjustable t))"), 3);
    /* Verify content is accessible */
    ASSERT_STR_EQ(eval_print(
        "(let ((s (make-array 2 :element-type 'character :fill-pointer t :adjustable t)))"
        "  (setf (aref s 0) #\\a) (setf (aref s 1) #\\b) s)"), "\"ab\"");
    /* vector-push-extend works after fill-pointer T */
    ASSERT_STR_EQ(eval_print(
        "(let ((s (make-array 2 :element-type 'character :fill-pointer t :adjustable t)))"
        "  (setf (aref s 0) #\\x) (setf (aref s 1) #\\y)"
        "  (vector-push-extend #\\z s) s)"), "\"xyz\"");
}

/* A fill-pointer/adjustable character vector (string-vector) is a valid CL
 * STRING: stringp / typep 'string report it as one, so CHAR and (SETF CHAR)
 * must accept it too.  Regression: CHAR used to reject string-vectors with
 * "CHAR: not a string", which broke FSet's COMPARE-STRINGS-LEXICOGRAPHICALLY
 * (it falls back to CHAR when SIMPLE-STRING-P is false). */
TEST(char_accessor_on_string_vector)
{
    ASSERT_STR_EQ(eval_print(
        "(stringp (make-array 3 :element-type 'character :fill-pointer 3 :adjustable t))"), "T");
    ASSERT_STR_EQ(eval_print(
        "(simple-string-p (make-array 3 :element-type 'character :fill-pointer 3 :adjustable t))"),
        "NIL");
    /* CHAR reads a string-vector */
    ASSERT_STR_EQ(eval_print(
        "(let ((s (make-array 3 :element-type 'character :fill-pointer 3 :adjustable t)))"
        "  (setf (char s 0) #\\a) (setf (char s 1) #\\b) (setf (char s 2) #\\c)"
        "  (char s 1))"), "#\\b");
    /* CHAR honors the active (fill-pointer) length */
    ASSERT_STR_EQ(eval_print(
        "(let ((s (make-array 4 :element-type 'character :fill-pointer 2 :adjustable t)))"
        "  (setf (char s 0) #\\x) (setf (char s 1) #\\y)"
        "  (list (char s 0) (char s 1)))"), "(#\\x #\\y)");
}

/* SCHAR requires a simple-string (CLHS §17.1); it must signal a type-error
 * when given a non-simple string (fill-pointer/adjustable character vector). */
TEST(schar_requires_simple_string)
{
    /* SCHAR works on a simple string */
    ASSERT_STR_EQ(eval_print("(schar \"hello\" 1)"), "#\\e");
    /* SCHAR on a non-simple string (fill-pointer vector) must error */
    ASSERT(eval_errors(
        "(let ((s (make-array 3 :element-type 'character :fill-pointer 3 :adjustable t)))"
        "  (setf (char s 0) #\\a) (setf (char s 1) #\\b) (setf (char s 2) #\\c)"
        "  (schar s 0))"));
}

/* Regression: storing into a typed sequence (string / bit-vector) via the
 * internal arr_seq_set path — used by REPLACE, MAP-INTO, SUBSTITUTE, etc. —
 * must enforce the element type, signalling a type-error for an invalid
 * element instead of silently dropping it.  serapeum's PAD-START relied on
 * (REPLACE <char-string> <list-of-non-chars>) raising a type-error. */
TEST(replace_into_string_type_checks)
{
    /* a char goes in fine */
    ASSERT_STR_EQ(eval_print(
        "(replace (copy-seq \"___\") \"ab\")"), "\"ab_\"");
    /* a non-character element must signal a type-error, not be dropped */
    ASSERT(eval_errors(
        "(replace (make-array 3 :element-type 'character :initial-element #\\Space)"
        "         '(foo bar))"));
    /* the partial store must not have happened silently: still all spaces */
    ASSERT_STR_EQ(eval_print(
        "(let ((s (make-array 3 :element-type 'character :initial-element #\\Space)))"
        "  (ignore-errors (replace s '(foo bar))) s)"),
        "\"   \"");
    /* bit-vector: a non-bit element is a type-error */
    ASSERT(eval_errors(
        "(replace (make-array 3 :element-type 'bit :initial-element 0) '(foo))"));
    ASSERT(eval_errors(
        "(replace (make-array 3 :element-type 'bit :initial-element 0) '(2))"));
    /* a valid bit still works */
    ASSERT_STR_EQ(eval_print(
        "(replace (make-array 3 :element-type 'bit :initial-element 0) '(1 1))"),
        "#*110");
}

TEST(make_array_initial_contents_string)
{
    /* :initial-contents with a string source */
    ASSERT_STR_EQ(eval_print(
        "(coerce (make-array 3 :element-type 'character :fill-pointer t :adjustable t"
        "                    :initial-contents \"abc\") 'string)"), "\"abc\"");
    /* :initial-contents with a vector source */
    ASSERT_STR_EQ(eval_print(
        "(make-array 3 :element-type 'character :initial-contents '(#\\x #\\y #\\z))"), "\"xyz\"");
}

/* Element-type drives the string width like MAKE-STRING: BASE-CHAR /
 * STANDARD-CHAR build a narrow BASE-STRING, CHARACTER builds a wide string.
 * Keeping these consistent with MAKE-STRING is required so (TYPEP s
 * 'BASE-STRING) reflects how the array was requested — FSet's seq leaves
 * (built via MAKE-ARRAY :element-type 'base-char) depend on it. */
TEST(make_array_base_char_is_base_string)
{
    ASSERT_STR_EQ(eval_print(
        "(typep (make-array 3 :element-type 'base-char :initial-element #\\a) 'base-string)"),
        "T");
    ASSERT_STR_EQ(eval_print(
        "(typep (make-array '(3) :element-type 'standard-char :initial-element #\\a) 'base-string)"),
        "T");
}

TEST(make_array_character_is_wide_string)
{
    /* A general CHARACTER element-type is NOT a base-string, and must hold
     * characters beyond the base range without truncation. */
    ASSERT_STR_EQ(eval_print(
        "(typep (make-array 2 :element-type 'character :initial-element #\\a) 'base-string)"),
        "NIL");
    ASSERT_STR_EQ(eval_print(
        "(let ((s (make-array 1 :element-type 'character :initial-element #\\a)))"
        "  (setf (char s 0) (code-char 1000)) (char-code (char s 0)))"),
        "1000");
}

TEST(make_array_base_char_wide_content_widens)
{
    /* Defensive: narrow declared element-type but contents need wide
     * storage — widen rather than silently truncate. */
    ASSERT_STR_EQ(eval_print(
        "(char-code (char (make-array 1 :element-type 'base-char "
        ":initial-element (code-char 5000)) 0))"),
        "5000");
}

/* CLHS COERCE: for a string result type the object must be a SEQUENCE, and
 * NIL is the empty one -- so (coerce nil 'string) is "", the same as
 * (coerce '() 'vector) is #().  It used to answer "NIL", reading NIL as the
 * symbol the way STRING does; the Lisp Clamacs's (coerce (nreverse chars)
 * 'string) then spelled an empty quoted word "NIL". */
TEST(coerce_nil_to_string_is_empty)
{
    ASSERT_STR_EQ(eval_print("(coerce nil 'string)"), "\"\"");
    ASSERT_STR_EQ(eval_print("(coerce '() 'simple-string)"), "\"\"");
    ASSERT_STR_EQ(eval_print("(length (coerce (nreverse (list)) 'string))"), "0");
    /* STRING itself keeps its designator semantics. */
    ASSERT_STR_EQ(eval_print("(string nil)"), "\"NIL\"");
    ASSERT_STR_EQ(eval_print("(coerce 'nil-ish 'string)"), "\"NIL-ISH\"");
}

/* class-of a MAKE-ARRAY string is STRING, so STRING-specialized CLOS
 * methods (and any %class-of consumer) see it correctly. */
TEST(make_array_string_class_of_is_string)
{
    ASSERT_STR_EQ(eval_print(
        "(%class-of (make-array 3 :element-type 'character :initial-element #\\a))"),
        "STRING");
}

/* Regression: a DEFTYPE alias for CHARACTER must upgrade to a STRING, not a
 * general (vector t).  flexi-streams' (deftype char* () 'character) hit this:
 * OCTETS-TO-STRING built (make-array n :element-type 'char*) and got a
 * SIMPLE-VECTOR of characters, so STRINGP was NIL and hunchentoot's
 * x-www-form POST parsing — (split "&" <that>) — failed. */
TEST(make_array_deftype_character_is_string)
{
    ASSERT_STR_EQ(eval_print(
        "(progn (deftype my-char () 'character)"
        "       (stringp (make-array 3 :element-type 'my-char :initial-element #\\a)))"),
        "T");
    /* nested deftype alias resolves too */
    ASSERT_STR_EQ(eval_print(
        "(progn (deftype my-char2 () 'my-char)"
        "       (stringp (make-array 2 :element-type 'my-char2 :initial-element #\\x)))"),
        "T");
    /* a deftype alias for BIT upgrades to a bit-vector */
    ASSERT_STR_EQ(eval_print(
        "(progn (deftype my-bit () 'bit)"
        "       (bit-vector-p (make-array 4 :element-type 'my-bit :initial-element 0)))"),
        "T");
}

/* Regression: UPGRADED-ARRAY-ELEMENT-TYPE must expand deftypes (CLHS — it
 * accepts any type specifier).  A CHARACTER alias upgrades to CHARACTER, a
 * BIT alias to BIT, everything else to T. */
TEST(upgraded_array_element_type_expands_deftype)
{
    ASSERT_STR_EQ(eval_print(
        "(progn (deftype my-char3 () 'character) (upgraded-array-element-type 'my-char3))"),
        "CHARACTER");
    ASSERT_STR_EQ(eval_print(
        "(progn (deftype my-bit2 () 'bit) (upgraded-array-element-type 'my-bit2))"),
        "BIT");
    ASSERT_STR_EQ(eval_print("(upgraded-array-element-type 'character)"), "CHARACTER");
    /* FIXNUM / SINGLE-FLOAT / DOUBLE-FLOAT are specialized element types kept
     * distinct from the general T so a (VECTOR FIXNUM) is not a (VECTOR T)
     * (serapeum's VECT-TYPE). */
    ASSERT_STR_EQ(eval_print("(upgraded-array-element-type 'fixnum)"), "FIXNUM");
    ASSERT_STR_EQ(eval_print("(upgraded-array-element-type 'single-float)"), "SINGLE-FLOAT");
    ASSERT_STR_EQ(eval_print("(upgraded-array-element-type 'double-float)"), "DOUBLE-FLOAT");
    /* still T: integer subranges and other types we don't specialize */
    ASSERT_STR_EQ(eval_print("(upgraded-array-element-type 'integer)"), "T");
    ASSERT_STR_EQ(eval_print("(upgraded-array-element-type 'number)"), "T");
}

/* Regression: integer subtypes whose value set is contained in BIT = (integer
 * 0 1) must upgrade to BIT, so make-array builds a real bit-vector.  serapeum's
 * RANGE produces bit ranges with bounds like (integer 0 (1)) and then dispatches
 * on the result type; without the upgrade it was a general T vector and SBIT
 * blew up with "not a bit vector". */
TEST(upgraded_array_element_type_bit_subtypes)
{
    ASSERT_STR_EQ(eval_print("(upgraded-array-element-type '(integer 0 1))"), "BIT");
    ASSERT_STR_EQ(eval_print("(upgraded-array-element-type '(integer 0 (1)))"), "BIT");
    ASSERT_STR_EQ(eval_print("(upgraded-array-element-type '(unsigned-byte 1))"), "BIT");
    ASSERT_STR_EQ(eval_print("(upgraded-array-element-type '(mod 2))"), "BIT");
    /* not subtypes of BIT — but within the byte range they upgrade to the
     * packed byte-vector element types (specialization order bit > u8 > s8) */
    ASSERT_STR_EQ(eval_print("(upgraded-array-element-type '(integer 0 2))"),
                  "(UNSIGNED-BYTE 8)");
    ASSERT_STR_EQ(eval_print("(upgraded-array-element-type '(unsigned-byte 8))"),
                  "(UNSIGNED-BYTE 8)");
    ASSERT_STR_EQ(eval_print("(upgraded-array-element-type '(integer -1 1))"),
                  "(SIGNED-BYTE 8)");
    /* 9-16 bit ranges → the packed 16-bit kinds; wider → general T */
    ASSERT_STR_EQ(eval_print("(upgraded-array-element-type '(unsigned-byte 16))"),
                  "(UNSIGNED-BYTE 16)");
    ASSERT_STR_EQ(eval_print("(upgraded-array-element-type '(integer -1 200))"),
                  "(SIGNED-BYTE 16)");
    ASSERT_STR_EQ(eval_print("(upgraded-array-element-type '(unsigned-byte 17))"), "T");
    /* make-array builds a true bit-vector that SBIT accepts */
    ASSERT_STR_EQ(eval_print(
        "(let ((b (make-array 3 :element-type '(integer 0 (1)))))"
        "  (setf (sbit b 0) 1) (list (bit-vector-p b) b))"),
        "(T #*100)");
}

/* Specialized numeric element types (FIXNUM / SINGLE-FLOAT / DOUBLE-FLOAT)
 * are kept distinct from the general T so a (VECTOR FIXNUM) is not a
 * (VECTOR T).  Regression for serapeum's VECT-TYPE; see specs/mop.md note on
 * the array element-type representation. */
TEST(make_array_specialized_element_type)
{
    /* array-element-type reports the specialized type */
    ASSERT_STR_EQ(eval_print(
        "(array-element-type (make-array 3 :element-type 'fixnum))"), "FIXNUM");
    ASSERT_STR_EQ(eval_print(
        "(array-element-type (make-array 3 :element-type 'single-float))"),
        "SINGLE-FLOAT");
    /* unspecialized arrays stay T */
    ASSERT_STR_EQ(eval_print("(array-element-type (make-array 3))"), "T");
    ASSERT_STR_EQ(eval_print("(array-element-type (vector 1 2 3))"), "T");
    ASSERT_STR_EQ(eval_print(
        "(array-element-type (make-array 3 :element-type 'integer))"), "T");
    /* (vector t) excludes a specialized fixnum array */
    ASSERT_STR_EQ(eval_print(
        "(typep (make-array 3 :element-type 'fixnum :adjustable t :fill-pointer 0)"
        "       '(vector t))"), "NIL");
    /* but a general vector is still (vector t) */
    ASSERT_STR_EQ(eval_print("(typep (vector 1 2 3) '(vector t))"), "T");
    ASSERT_STR_EQ(eval_print("(typep #() '(vector t))"), "T");
    /* (vector fixnum) matches only the specialized array */
    ASSERT_STR_EQ(eval_print(
        "(typep (make-array 3 :element-type 'fixnum) '(vector fixnum))"), "T");
    ASSERT_STR_EQ(eval_print("(typep #(1 2 3) '(vector fixnum))"), "NIL");
    /* (array t) / (array fixnum) likewise discriminate */
    ASSERT_STR_EQ(eval_print(
        "(typep (make-array 3 :element-type 'fixnum) '(array t))"), "NIL");
    ASSERT_STR_EQ(eval_print(
        "(typep (make-array 3 :element-type 'fixnum) '(array fixnum))"), "T");
    /* the array still holds general tagged values regardless of code */
    ASSERT_STR_EQ(eval_print(
        "(let ((a (make-array 2 :element-type 'fixnum)))"
        "  (setf (aref a 0) 5) (aref a 0))"), "5");
}

/* Regression: the non-destructive sequence functions must preserve a
 * specialized numeric element type, so a COPY-SEQ / SUBSEQ of a (VECTOR
 * SINGLE-FLOAT) still reports SINGLE-FLOAT, and MAKE-SEQUENCE on a typed
 * vector type produces a vector of that type.  ansi COPY-SEQ.23 /
 * SUBSEQ.SPECIALIZED-VECTOR.2 / MAKE-SEQUENCE.29; alexandria COPY-SEQUENCE.1. */
TEST(seq_funcs_preserve_specialized_element_type)
{
    /* COPY-SEQ keeps the element type and the values */
    ASSERT_STR_EQ(eval_print(
        "(array-element-type"
        "  (copy-seq (make-array 3 :element-type 'single-float"
        "                          :initial-contents '(1.0 2.0 3.0))))"),
        "SINGLE-FLOAT");
    ASSERT_STR_EQ(eval_print(
        "(array-element-type"
        "  (copy-seq (make-array 3 :element-type 'double-float"
        "                          :initial-contents '(1.0d0 2.0d0 3.0d0))))"),
        "DOUBLE-FLOAT");
    ASSERT_STR_EQ(eval_print(
        "(equalp (make-array 2 :element-type 'single-float :initial-contents '(1.0 2.0))"
        "        (copy-seq (make-array 2 :element-type 'single-float"
        "                              :initial-contents '(1.0 2.0))))"), "T");
    /* a general vector copy stays T */
    ASSERT_STR_EQ(eval_print("(array-element-type (copy-seq (vector 1 2 3)))"), "T");
    /* SUBSEQ keeps the element type */
    ASSERT_STR_EQ(eval_print(
        "(array-element-type"
        "  (subseq (make-array 5 :element-type 'single-float"
        "                        :initial-contents '(1.0 2.0 3.0 4.0 5.0)) 1 4))"),
        "SINGLE-FLOAT");
    ASSERT_STR_EQ(eval_print(
        "(subseq (make-array 5 :element-type 'single-float"
        "                      :initial-contents '(1.0 2.0 3.0 4.0 5.0)) 1 4)"),
        "#(2.0 3.0 4.0)");
    /* MAKE-SEQUENCE honors a specialized (vector ...) type */
    ASSERT_STR_EQ(eval_print(
        "(array-element-type (make-sequence '(vector single-float) 4"
        "                                   :initial-element 1.0))"),
        "SINGLE-FLOAT");
    ASSERT_STR_EQ(eval_print(
        "(typep (make-sequence '(vector single-float) 4 :initial-element 1.0)"
        "       '(vector single-float))"), "T");
    /* an unspecialized vector type stays T */
    ASSERT_STR_EQ(eval_print(
        "(array-element-type (make-sequence 'vector 3))"), "T");
    ASSERT_STR_EQ(eval_print(
        "(array-element-type (make-sequence '(vector t) 3))"), "T");
    /* COERCE to a specialized vector type honors the element type (this is the
     * path alexandria's COPY-SEQUENCE uses for a non-matching target type) */
    ASSERT_STR_EQ(eval_print(
        "(array-element-type (coerce '(1 2 3) '(vector fixnum)))"), "FIXNUM");
    ASSERT_STR_EQ(eval_print(
        "(array-element-type (coerce '(1.0 2.0) '(vector single-float)))"),
        "SINGLE-FLOAT");
    ASSERT_STR_EQ(eval_print(
        "(typep (coerce '(1 2 3) '(vector fixnum)) '(vector fixnum))"), "T");
    /* COERCE to a plain vector stays T */
    ASSERT_STR_EQ(eval_print(
        "(array-element-type (coerce '(1 2 3) 'vector))"), "T");
}

TEST(make_array_adjustable)
{
    ASSERT_STR_EQ(eval_print(
        "(adjustable-array-p (make-array 5 :adjustable t))"), "T");
    ASSERT_STR_EQ(eval_print(
        "(adjustable-array-p (make-array 5))"), "NIL");
}

/* ============================================================ */
/* make-array: multi-dimensional                                */
/* ============================================================ */

TEST(make_array_2d)
{
    ASSERT_STR_EQ(eval_print("(array-dimensions (make-array '(2 3)))"), "(2 3)");
    ASSERT_EQ_INT(eval_int("(array-rank (make-array '(2 3)))"), 2);
    ASSERT_EQ_INT(eval_int("(array-total-size (make-array '(2 3)))"), 6);
}

TEST(make_array_3d)
{
    ASSERT_EQ_INT(eval_int("(array-rank (make-array '(2 3 4)))"), 3);
    ASSERT_EQ_INT(eval_int("(array-total-size (make-array '(2 3 4)))"), 24);
    ASSERT_STR_EQ(eval_print("(array-dimensions (make-array '(2 3 4)))"), "(2 3 4)");
}

TEST(make_array_2d_initial_contents)
{
    ASSERT_STR_EQ(eval_print(
        "(make-array '(2 3) :initial-contents '((1 2 3) (4 5 6)))"),
        "#2A((1 2 3) (4 5 6))");
}

TEST(make_array_3d_initial_contents)
{
    ASSERT_STR_EQ(eval_print(
        "(make-array '(2 2 2) :initial-contents '(((1 2) (3 4)) ((5 6) (7 8))))"),
        "#3A(((1 2) (3 4)) ((5 6) (7 8)))");
}

TEST(make_array_2d_initial_element)
{
    ASSERT_EQ_INT(eval_int(
        "(aref (make-array '(2 3) :initial-element 99) 1 2)"), 99);
}

TEST(make_array_empty_dims)
{
    /* Empty rows */
    ASSERT_STR_EQ(eval_print("(make-array '(2 0))"), "#2A(() ())");
    ASSERT_STR_EQ(eval_print("(make-array '(0 3))"), "#2A()");
    ASSERT_EQ_INT(eval_int("(array-total-size (make-array '(0 3)))"), 0);
}

/* ============================================================ */
/* vector function                                              */
/* ============================================================ */

TEST(vector_constructor)
{
    ASSERT_STR_EQ(eval_print("(vector 1 2 3)"), "#(1 2 3)");
    ASSERT_STR_EQ(eval_print("(vector)"), "#()");
    ASSERT_EQ_INT(eval_int("(length (vector 'a 'b 'c 'd))"), 4);
}

/* ============================================================ */
/* aref / svref                                                 */
/* ============================================================ */

TEST(aref_1d)
{
    ASSERT_EQ_INT(eval_int("(aref (vector 10 20 30) 0)"), 10);
    ASSERT_EQ_INT(eval_int("(aref (vector 10 20 30) 2)"), 30);
}

TEST(aref_2d)
{
    ASSERT_EQ_INT(eval_int(
        "(let ((a (make-array '(2 3) :initial-contents '((1 2 3) (4 5 6)))))"
        "  (aref a 1 2))"), 6);
    ASSERT_EQ_INT(eval_int(
        "(let ((a (make-array '(2 3) :initial-contents '((1 2 3) (4 5 6)))))"
        "  (aref a 0 0))"), 1);
}

TEST(aref_3d)
{
    ASSERT_EQ_INT(eval_int(
        "(let ((a (make-array '(2 2 2) :initial-contents '(((1 2) (3 4)) ((5 6) (7 8))))))"
        "  (aref a 1 1 1))"), 8);
}

TEST(aref_bounds_error)
{
    ASSERT(eval_errors("(aref (vector 1 2 3) 5)"));
    ASSERT(eval_errors("(aref (vector 1 2 3) -1)"));
}

TEST(svref_basic)
{
    ASSERT_EQ_INT(eval_int("(svref (vector 10 20 30) 1)"), 20);
}

TEST(svref_rejects_nonsimple)
{
    ASSERT(eval_errors("(svref (make-array 3 :fill-pointer 0) 0)"));
}

/* ============================================================ */
/* setf aref                                                    */
/* ============================================================ */

TEST(setf_aref_1d)
{
    ASSERT_EQ_INT(eval_int(
        "(let ((v (make-array 3)))"
        "  (setf (aref v 0) 42) (aref v 0))"), 42);
    /* setf returns value */
    ASSERT_EQ_INT(eval_int(
        "(let ((v (make-array 1))) (setf (aref v 0) 99))"), 99);
}

TEST(setf_aref_2d)
{
    ASSERT_EQ_INT(eval_int(
        "(let ((a (make-array '(2 3))))"
        "  (setf (aref a 1 2) 77) (aref a 1 2))"), 77);
}

TEST(setf_aref_3d)
{
    ASSERT_EQ_INT(eval_int(
        "(let ((a (make-array '(2 2 2))))"
        "  (setf (aref a 1 0 1) 55) (aref a 1 0 1))"), 55);
}

/* ============================================================ */
/* array-dimension / array-total-size / array-row-major-index   */
/* ============================================================ */

TEST(array_dimension)
{
    ASSERT_EQ_INT(eval_int("(array-dimension (make-array 5) 0)"), 5);
    ASSERT_EQ_INT(eval_int("(array-dimension (make-array '(3 4)) 0)"), 3);
    ASSERT_EQ_INT(eval_int("(array-dimension (make-array '(3 4)) 1)"), 4);
}

TEST(array_total_size)
{
    ASSERT_EQ_INT(eval_int("(array-total-size (make-array 5))"), 5);
    ASSERT_EQ_INT(eval_int("(array-total-size (make-array '(3 4)))"), 12);
    ASSERT_EQ_INT(eval_int("(array-total-size (make-array '(2 3 4)))"), 24);
}

TEST(array_row_major_index)
{
    ASSERT_EQ_INT(eval_int("(array-row-major-index (make-array 5) 3)"), 3);
    ASSERT_EQ_INT(eval_int("(array-row-major-index (make-array '(3 4)) 0 0)"), 0);
    ASSERT_EQ_INT(eval_int("(array-row-major-index (make-array '(3 4)) 1 0)"), 4);
    ASSERT_EQ_INT(eval_int("(array-row-major-index (make-array '(3 4)) 2 3)"), 11);
}

/* ANSI-test cons/sublis.3 regression: equalp-with-case in rt.lsp calls
 * ARRAY-RANK on a leaf if (typep x 'array) is true. Strings ARE arrays in
 * CL, so ARRAY-RANK / ARRAY-DIMENSIONS / ARRAY-DIMENSION / ARRAY-TOTAL-SIZE
 * / ARRAY-ROW-MAJOR-INDEX must accept strings. */
TEST(array_ops_on_string)
{
    ASSERT_EQ_INT(eval_int("(array-rank \"foo\")"), 1);
    ASSERT_STR_EQ(eval_print("(array-dimensions \"foo\")"), "(3)");
    ASSERT_EQ_INT(eval_int("(array-dimension \"foo\" 0)"), 3);
    ASSERT_EQ_INT(eval_int("(array-total-size \"hello\")"), 5);
    ASSERT_EQ_INT(eval_int("(array-row-major-index \"abc\" 1)"), 1);
    ASSERT_STR_EQ(eval_print("(array-has-fill-pointer-p \"abc\")"), "NIL");
    ASSERT_STR_EQ(eval_print("(adjustable-array-p \"abc\")"), "NIL");
}

/* ============================================================ */
/* row-major-aref / (setf row-major-aref)                       */
/* ============================================================ */

TEST(row_major_aref)
{
    ASSERT_EQ_INT(eval_int(
        "(let ((a (make-array '(2 3) :initial-contents '((1 2 3) (4 5 6)))))"
        "  (row-major-aref a 5))"), 6);
    ASSERT_EQ_INT(eval_int(
        "(let ((a (make-array '(2 3) :initial-contents '((1 2 3) (4 5 6)))))"
        "  (row-major-aref a 0))"), 1);
}

TEST(setf_row_major_aref)
{
    ASSERT_EQ_INT(eval_int(
        "(let ((a (make-array '(2 3))))"
        "  (setf (row-major-aref a 5) 77)"
        "  (aref a 1 2))"), 77);
}

/* ============================================================ */
/* fill-pointer / (setf fill-pointer) / array-has-fill-pointer-p*/
/* ============================================================ */

TEST(fill_pointer_ops)
{
    ASSERT_EQ_INT(eval_int(
        "(fill-pointer (make-array 10 :fill-pointer 0))"), 0);
    ASSERT_EQ_INT(eval_int(
        "(fill-pointer (make-array 10 :fill-pointer 5))"), 5);
    ASSERT_STR_EQ(eval_print(
        "(array-has-fill-pointer-p (make-array 5 :fill-pointer 0))"), "T");
    ASSERT_STR_EQ(eval_print(
        "(array-has-fill-pointer-p (make-array 5))"), "NIL");
}

TEST(setf_fill_pointer)
{
    ASSERT_EQ_INT(eval_int(
        "(let ((v (make-array 10 :fill-pointer 0)))"
        "  (setf (fill-pointer v) 7)"
        "  (fill-pointer v))"), 7);
    /* returns new value */
    ASSERT_EQ_INT(eval_int(
        "(let ((v (make-array 10 :fill-pointer 0)))"
        "  (setf (fill-pointer v) 3))"), 3);
}

TEST(fill_pointer_bounds)
{
    /* Cannot set fill-pointer beyond length */
    ASSERT(eval_errors(
        "(let ((v (make-array 5 :fill-pointer 0)))"
        "  (setf (fill-pointer v) 10))"));
    /* Cannot get fill-pointer on array without one */
    ASSERT(eval_errors("(fill-pointer (make-array 5))"));
}

/* ============================================================ */
/* vector-push / vector-push-extend                             */
/* ============================================================ */

TEST(vector_push)
{
    /* Returns index of pushed element */
    ASSERT_EQ_INT(eval_int(
        "(let ((v (make-array 5 :fill-pointer 0)))"
        "  (vector-push 10 v))"), 0);
    ASSERT_EQ_INT(eval_int(
        "(let ((v (make-array 5 :fill-pointer 0)))"
        "  (vector-push 10 v)"
        "  (vector-push 20 v))"), 1);
    /* Fill pointer advances */
    ASSERT_EQ_INT(eval_int(
        "(let ((v (make-array 5 :fill-pointer 0)))"
        "  (vector-push 10 v)"
        "  (vector-push 20 v)"
        "  (vector-push 30 v)"
        "  (fill-pointer v))"), 3);
    /* Returns NIL when full */
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 2 :fill-pointer 0)))"
        "  (vector-push 1 v)"
        "  (vector-push 2 v)"
        "  (vector-push 3 v))"), "NIL");
    /* Data is stored correctly */
    ASSERT_EQ_INT(eval_int(
        "(let ((v (make-array 3 :fill-pointer 0)))"
        "  (vector-push 10 v)"
        "  (vector-push 20 v)"
        "  (+ (aref v 0) (aref v 1)))"), 30);
}

/* Bit vectors are a heap type of their own: the three fill-pointer operators
 * must take one exactly like any other vector (CLHS VECTOR-PUSH, VECTOR-POP;
 * they answered "not a vector" before). */
TEST(vector_push_pop_bit_vector)
{
    /* index returned, fill pointer advances, bits land where pushed */
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 5 :element-type 'bit :fill-pointer 0)))"
        "  (list (vector-push 1 v) (vector-push 0 v) (vector-push 1 v)"
        "        (fill-pointer v) (length v) (bit-vector-p v) v))"),
        "(0 1 2 3 3 T #*101)");
    /* a push clears a stale 1 above the fill pointer as well as setting one */
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 3 :element-type 'bit :initial-element 1"
        "                       :fill-pointer 0)))"
        "  (vector-push 0 v) (vector-push 1 v) (vector-push 0 v) v)"),
        "#*010");
    /* across the 32-bit word boundary of the packed storage */
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 70 :element-type 'bit :fill-pointer 0)))"
        "  (dotimes (i 70) (vector-push (if (zerop (mod i 3)) 1 0) v))"
        "  (list (fill-pointer v) (count 1 v) (bit v 30) (bit v 33) (bit v 63)"
        "        (bit v 64) (bit v 69)))"),
        "(70 24 1 1 1 0 1)");
    /* full: VECTOR-PUSH returns NIL and leaves the vector alone */
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 2 :element-type 'bit :fill-pointer 0)))"
        "  (list (vector-push 1 v) (vector-push 1 v) (vector-push 0 v)"
        "        (fill-pointer v) v))"),
        "(0 1 NIL 2 #*11)");
    /* VECTOR-POP hands the bits back in reverse */
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 4 :element-type 'bit :fill-pointer 0)))"
        "  (vector-push 1 v) (vector-push 0 v) (vector-push 1 v)"
        "  (list (vector-pop v) (vector-pop v) (fill-pointer v) (length v)"
        "        (vector-pop v) (fill-pointer v)))"),
        "(1 0 1 1 1 0)");
    /* VECTOR-POP at fill pointer zero is an error */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (vector-pop (make-array 2 :element-type 'bit"
        "                                      :fill-pointer 0))"
        "  (error () :caught))"), ":CAUGHT");
    /* no fill pointer: an error for all three, not a silent store */
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 2 :element-type 'bit :initial-element 0)))"
        "  (list (handler-case (vector-push 1 v) (error () :caught))"
        "        (handler-case (vector-push-extend 1 v) (error () :caught))"
        "        (handler-case (vector-pop v) (error () :caught)) v))"),
        "(:CAUGHT :CAUGHT :CAUGHT #*00)");
    /* only 0 and 1 are bits: a type-error, fill pointer untouched */
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 4 :element-type 'bit :fill-pointer 0)))"
        "  (list (handler-case (vector-push 2 v) (type-error () :caught))"
        "        (handler-case (vector-push 'a v) (type-error () :caught))"
        "        (handler-case (vector-push-extend -1 v) (type-error () :caught))"
        "        (fill-pointer v)))"),
        "(:CAUGHT :CAUGHT :CAUGHT 0)");
}

TEST(vector_push_extend_bit_vector)
{
    /* with room it is VECTOR-PUSH */
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 3 :element-type 'bit :fill-pointer 0)))"
        "  (list (vector-push-extend 1 v) (vector-push-extend 0 v)"
        "        (vector-push-extend 1 v) (fill-pointer v) v))"),
        "(0 1 2 3 #*101)");
    /* full and NOT adjustable: a clean error (CLHS: VECTOR-PUSH-EXTEND
     * needs an actually adjustable vector to extend), never a silent store */
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 2 :element-type 'bit :fill-pointer 2"
        "                       :initial-element 1)))"
        "  (list (handler-case (vector-push-extend 0 v) (error () :caught))"
        "        (fill-pointer v) v))"),
        "(:CAUGHT 2 #*11)");
}

/* An adjustable bit vector grows in place and keeps its identity: the bits
 * move to a backing bit vector the object then refers to (types.h). */
TEST(vector_push_extend_bit_vector_grows)
{
    /* from capacity zero, across several growths and word boundaries */
    ASSERT_STR_EQ(eval_print(
        "(let* ((v (make-array 0 :element-type 'bit :adjustable t"
        "                        :fill-pointer 0))"
        "       (w v) (idx nil))"
        "  (dotimes (i 200)"
        "    (push (vector-push-extend (if (zerop (mod i 3)) 1 0) v) idx))"
        "  (list (eq v w) (equal (reverse idx) (loop for i below 200 collect i))"
        "        (fill-pointer v) (length v) (count 1 v)"
        "        (bit v 0) (bit v 1) (bit v 63) (bit v 64) (bit v 66) (bit v 198)"
        "        (>= (array-total-size v) 200)))"),
        "(T T 200 200 67 1 0 1 0 1 1 T)");
    /* still a bit vector in every respect */
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 2 :element-type 'bit :adjustable t"
        "                       :fill-pointer 0)))"
        "  (dotimes (i 70) (vector-push-extend (mod i 2) v))"
        "  (list (bit-vector-p v) (simple-bit-vector-p v) (adjustable-array-p v)"
        "        (array-element-type v) (typep v 'bit-vector) (subseq v 0 6)"
        "        (length (copy-seq v)) (reduce #'+ v) (position 1 v)"
        "        (vector-pop v) (fill-pointer v)))"),
        "(T NIL T BIT T #*010101 70 35 1 1 69)");
    /* the optional extension is a minimum for the growth */
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 2 :element-type 'bit :adjustable t"
        "                       :fill-pointer 2 :initial-element 1)))"
        "  (list (vector-push-extend 0 v 1000) (>= (array-total-size v) 1002)"
        "        (fill-pointer v) v))"),
        "(2 T 3 #*110)");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (vector-push-extend 1 (make-array 0 :element-type 'bit"
        "                 :adjustable t :fill-pointer 0) -1)"
        "  (type-error () :caught))"), ":CAUGHT");
    /* growth keeps the bits ABOVE the fill pointer too (they are elements
     * of the array, CLHS 15.1.1.2), and a write after growth lands in the
     * new backing */
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 4 :element-type 'bit :adjustable t"
        "                       :fill-pointer 4 :initial-contents '(1 0 1 1))))"
        "  (vector-push-extend 1 v)"
        "  (setf (bit v 1) 1 (fill-pointer v) 2)"
        "  (list v (aref v 3) (aref v 4)))"),
        "(#*11 1 1)");
    /* printer, EQUAL/EQUALP and EQUAL hashing see the active elements */
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 1 :element-type 'bit :adjustable t"
        "                       :fill-pointer 0))"
        "      (h (make-hash-table :test 'equal)))"
        "  (dolist (b '(1 0 1 1 0)) (vector-push-extend b v))"
        "  (setf (gethash #*10110 h) :hit)"
        "  (list (format nil \"~A\" v) (equal v #*10110) (equalp v #*10110)"
        "        (equal v #*10111) (gethash v h)))"),
        "(\"#*10110\" T T NIL :HIT)");
}

/* EQUAL on bit vectors compares the active elements: bits above a fill
 * pointer are not part of the sequence (it compared total lengths and whole
 * words before, so a fill-pointered bit vector never equalled its contents). */
TEST(equal_bit_vector_fill_pointer)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 8 :element-type 'bit :fill-pointer 3"
        "                       :initial-contents '(1 0 1 1 1 1 1 1))))"
        "  (list (equal v #*101) (equal #*101 v) (equal v #*10111111)"
        "        (equal v #*100) (equal v (copy-seq v))))"),
        "(T T NIL NIL T)");
}

TEST(adjust_array_bit_vector)
{
    /* adjustable: adjusted in place, bits kept, growth filled */
    ASSERT_STR_EQ(eval_print(
        "(let* ((v (make-array 3 :element-type 'bit :adjustable t"
        "                        :initial-contents '(1 0 1)))"
        "       (w (adjust-array v 10 :initial-element 1)))"
        "  (list (eq v w) v (length v) (adjustable-array-p v)))"),
        "(T #*1011111111 10 T)");
    /* growth without :initial-element: zeros here */
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 3 :element-type 'bit :adjustable t"
        "                       :initial-element 1)))"
        "  (adjust-array v 40) (list (count 1 v) (length v) (bit v 39)))"),
        "(3 40 0)");
    /* shrink then grow: the cut-off bits do not come back, and the
     * whole-word operators see clean storage */
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 40 :element-type 'bit :adjustable t"
        "                       :initial-element 1)))"
        "  (adjust-array v 3) (adjust-array v 6)"
        "  (list (copy-seq v) (equal v #*111000) (bit-ior v #*000000) (bit-not v)"
        "        (progn (adjust-array v 0) (length v))"
        "        (progn (adjust-array v 2 :initial-element 1) v)))"),
        "(#*111000 T #*111000 #*000111 0 #*11)");
    /* not adjustable: a fresh bit vector, the original untouched */
    ASSERT_STR_EQ(eval_print(
        "(let* ((v (make-array 3 :element-type 'bit"
        "                        :initial-contents '(1 0 1)))"
        "       (w (adjust-array v 5 :initial-element 1)))"
        "  (list (eq v w) v w (bit-vector-p w) (adjustable-array-p w)"
        "        (adjust-array v 2)))"),
        "(NIL #*101 #*10111 T NIL #*10)");
    /* fill pointer: carried, replaced by :fill-pointer, T = the new size */
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 4 :element-type 'bit :adjustable t :fill-pointer 2"
        "                       :initial-contents '(1 1 0 1))))"
        "  (list (fill-pointer (adjust-array v 8)) (copy-seq v)"
        "        (fill-pointer (adjust-array v 8 :fill-pointer 4)) (copy-seq v)"
        "        (fill-pointer (adjust-array v 6 :fill-pointer t))"
        "        (fill-pointer (adjust-array (make-array 4 :element-type 'bit"
        "                                      :fill-pointer 1) 9))))"),
        "(2 #*11 4 #*1101 6 1)");
    /* errors: fill pointer past the new size, a non-bit :initial-element,
     * a negative dimension */
    ASSERT_STR_EQ(eval_print(
        "(flet ((bv () (make-array 4 :element-type 'bit :adjustable t"
        "                            :fill-pointer 4)))"
        "  (list (handler-case (adjust-array (bv) 2) (error () :caught))"
        "        (handler-case (adjust-array (bv) 8 :fill-pointer 9)"
        "          (error () :caught))"
        "        (handler-case (adjust-array (bv) 8 :initial-element 2)"
        "          (type-error () :caught))"
        "        (handler-case (adjust-array (bv) -1) (type-error () :caught))))"),
        "(:CAUGHT :CAUGHT :CAUGHT :CAUGHT)");
}

/* A grown bit vector refers to its backing from inside the object, so the
 * collector must trace and forward that ref: across full collections, and
 * (generational host collector) from a tenured bit vector to a backing
 * allocated after it. */
TEST(grown_bit_vector_survives_gc)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 0 :element-type 'bit :adjustable t"
        "                       :fill-pointer 0))"
        "      (ok t))"
        "  (ext:gc) (ext:gc)"
        "  (dotimes (round 6)"
        "    (dotimes (i 300) (vector-push-extend (mod (+ i round) 2) v))"
        "    (dotimes (i 4000) (make-list 8))"
        "    (when (evenp round) (ext:gc))"
        "    (unless (and (= (length v) (* 300 (1+ round)))"
        "                 (= (count 1 v) (* 150 (1+ round)))"
        "                 (= (bit v (1- (length v))) (mod (+ 299 round) 2)))"
        "      (setf ok nil)))"
        /* ADJUST-ARRAY keeps every element of the old array -- the zeros  */
        /* between fill pointer and capacity included -- and fills beyond. */
        "  (let ((cap (array-total-size v)))"
        "    (adjust-array v 4000 :initial-element 1 :fill-pointer t)"
        "    (dotimes (i 4000) (make-list 8))"
        "    (ext:gc)"
        "    (list ok (length v) (count 1 v :end 1800)"
        "          (= (count 1 v) (+ 900 (- 4000 cap))))))"),
        "(T 4000 900 T)");
}

TEST(vector_push_extend_basic)
{
    /* Extends when full, returns old fill-pointer index */
    ASSERT_EQ_INT(eval_int(
        "(let ((v (make-array 2 :fill-pointer 0 :adjustable t)))"
        "  (vector-push-extend 10 v)"
        "  (vector-push-extend 20 v)"
        "  (vector-push-extend 30 v))"), 2);
    /* Fill pointer advances past original capacity */
    ASSERT_EQ_INT(eval_int(
        "(let ((v (make-array 2 :fill-pointer 0 :adjustable t)))"
        "  (vector-push-extend 10 v)"
        "  (vector-push-extend 20 v)"
        "  (vector-push-extend 30 v)"
        "  (fill-pointer v))"), 3);
    /* Data is accessible after extension */
    ASSERT_EQ_INT(eval_int(
        "(let ((v (make-array 2 :fill-pointer 0 :adjustable t)))"
        "  (vector-push-extend 10 v)"
        "  (vector-push-extend 20 v)"
        "  (vector-push-extend 30 v)"
        "  (+ (aref v 0) (aref v 1) (aref v 2)))"), 60);
}

TEST(vector_push_extend_multiple)
{
    /* Multiple extensions work correctly */
    ASSERT_EQ_INT(eval_int(
        "(let ((v (make-array 1 :fill-pointer 0 :adjustable t)))"
        "  (dotimes (i 20) (vector-push-extend i v))"
        "  (fill-pointer v))"), 20);
    /* Verify data integrity after many extensions */
    ASSERT_EQ_INT(eval_int(
        "(let ((v (make-array 1 :fill-pointer 0 :adjustable t)))"
        "  (dotimes (i 10) (vector-push-extend (* i 10) v))"
        "  (+ (aref v 0) (aref v 5) (aref v 9)))"), 140);  /* 0 + 50 + 90 */
}

TEST(vector_push_extend_identity)
{
    /* Vector identity preserved (EQ) after extension */
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 1 :fill-pointer 0 :adjustable t)))"
        "  (let ((v2 v))"
        "    (vector-push-extend 42 v)"
        "    (vector-push-extend 99 v)"
        "    (eq v v2)))"), "T");
}

TEST(vector_push_extend_zero_capacity)
{
    /* Extending from zero-length vector */
    ASSERT_EQ_INT(eval_int(
        "(let ((v (make-array 0 :fill-pointer 0 :adjustable t)))"
        "  (vector-push-extend 42 v)"
        "  (aref v 0))"), 42);
}

TEST(vector_push_extend_nonadjustable_basic)
{
    /* Non-adjustable fill-pointer vector can be extended (CLHS: only fill-pointer required) */
    ASSERT_EQ_INT(eval_int(
        "(let ((v (make-array 2 :fill-pointer 0)))"
        "  (vector-push-extend 10 v)"
        "  (vector-push-extend 20 v)"
        "  (vector-push-extend 30 v))"), 2);
    ASSERT_EQ_INT(eval_int(
        "(let ((v (make-array 2 :fill-pointer 0)))"
        "  (vector-push-extend 10 v)"
        "  (vector-push-extend 20 v)"
        "  (vector-push-extend 30 v)"
        "  (fill-pointer v))"), 3);
}

TEST(vector_push_extend_nonadjustable_data)
{
    /* Data is accessible after extending a non-adjustable fill-pointer vector */
    ASSERT_EQ_INT(eval_int(
        "(let ((v (make-array 2 :fill-pointer 0)))"
        "  (vector-push-extend 10 v)"
        "  (vector-push-extend 20 v)"
        "  (vector-push-extend 30 v)"
        "  (+ (aref v 0) (aref v 1) (aref v 2)))"), 60);
}

TEST(vector_push_extend_nonadjustable_many)
{
    /* Multiple extensions from length-1 non-adjustable fill-pointer vector */
    ASSERT_EQ_INT(eval_int(
        "(let ((v (make-array 1 :fill-pointer 0)))"
        "  (dotimes (i 20) (vector-push-extend i v))"
        "  (fill-pointer v))"), 20);
    /* Zero-length start */
    ASSERT_EQ_INT(eval_int(
        "(let ((v (make-array 0 :fill-pointer 0)))"
        "  (vector-push-extend 42 v)"
        "  (aref v 0))"), 42);
}

TEST(vector_push_extend_nonadjustable_identity)
{
    /* Object identity is preserved after extension */
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 1 :fill-pointer 0)))"
        "  (let ((v2 v))"
        "    (vector-push-extend 42 v)"
        "    (vector-push-extend 99 v)"
        "    (eq v v2)))"), "T");
}

TEST(vector_push_extend_nonadjustable_still_nonadjustable)
{
    /* adjustable-array-p must still return NIL after extension */
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 2 :fill-pointer 0)))"
        "  (vector-push-extend 10 v)"
        "  (vector-push-extend 20 v)"
        "  (vector-push-extend 30 v)"
        "  (adjustable-array-p v))"), "NIL");
}

TEST(vector_push_extend_nonadjustable_character)
{
    /* Character (string) fill-pointer vector without :adjustable t */
    ASSERT_STR_EQ(eval_print(
        "(let ((s (make-array 2 :element-type 'character :fill-pointer 0)))"
        "  (vector-push-extend #\\x s)"
        "  (vector-push-extend #\\y s)"
        "  (vector-push-extend #\\z s)"
        "  s)"), "\"xyz\"");
}

TEST(vector_push_extend_nonadjustable_character_equal)
{
    /* Regression: EQUAL between a literal string (TYPE_STRING) and a
       character vector (TYPE_VECTOR + STRING flag, here grown by
       vector-push-extend) must compare element-wise, not return NIL. */
    ASSERT_STR_EQ(eval_print(
        "(equal \"xyz\""
        "  (let ((s (make-array 2 :element-type 'character :fill-pointer 0)))"
        "    (vector-push-extend #\\x s)"
        "    (vector-push-extend #\\y s)"
        "    (vector-push-extend #\\z s)"
        "    s))"), "T");
}

TEST(equal_string_vs_char_vector)
{
    /* Both directions, plus a true negative. */
    ASSERT_STR_EQ(eval_print(
        "(equal (make-array 3 :element-type 'character"
        "                     :initial-contents '(#\\a #\\b #\\c))"
        "       \"abc\")"), "T");
    ASSERT_STR_EQ(eval_print(
        "(equal \"abc\""
        "       (make-array 3 :element-type 'character"
        "                     :initial-contents '(#\\a #\\b #\\c)))"), "T");
    ASSERT_STR_EQ(eval_print(
        "(equal \"abc\""
        "       (make-array 3 :element-type 'character"
        "                     :initial-contents '(#\\a #\\b #\\d)))"), "NIL");
}

TEST(fill_pointer_string_as_equal_hash_key)
{
    /* Regression: a fill-pointer character vector is a STRING, and EQUAL
       says T against a literal of the same content (above) -- so SXHASH must
       agree (CLHS: EQUAL implies equal SXHASH) and an EQUAL / EQUALP hash
       table must find the key from either side.  It hashed by identity
       (EQUAL) or via the type-blind vector path (EQUALP), so a key built with
       VECTOR-PUSH-EXTEND (a JSON parser's object key) was never found by a
       literal lookup. */
    ASSERT_STR_EQ(eval_print(
        "(let ((k (make-array 2 :element-type 'character"
        "                       :adjustable t :fill-pointer 0)))"
        "  (vector-push-extend #\\a k)"
        "  (vector-push-extend #\\b k)"
        "  (list (= (sxhash k) (sxhash \"ab\"))"
        "        (let ((h (make-hash-table :test 'equal)))"
        "          (setf (gethash k h) 1)"
        "          (gethash \"ab\" h))"
        "        (let ((h (make-hash-table :test 'equal)))"
        "          (setf (gethash \"ab\" h) 2)"
        "          (gethash k h))"
        "        (let ((h (make-hash-table :test 'equalp)))"
        "          (setf (gethash k h) 3)"
        "          (gethash \"AB\" h))))"), "(T 1 2 3)");
}

/* ============================================================ */
/* adjust-array                                                 */
/* ============================================================ */

TEST(adjust_array_grow)
{
    ASSERT_EQ_INT(eval_int(
        "(let ((v (make-array 3 :adjustable t :initial-element 1)))"
        "  (let ((v2 (adjust-array v 5 :initial-element 99)))"
        "    (+ (aref v2 0) (aref v2 3))))"), 100);  /* 1 + 99 */
}

TEST(adjust_array_shrink)
{
    ASSERT_EQ_INT(eval_int(
        "(let ((v (make-array 5 :adjustable t :initial-element 42)))"
        "  (array-total-size (adjust-array v 3)))"), 3);
}

TEST(adjust_array_preserves_fp)
{
    ASSERT_EQ_INT(eval_int(
        "(let ((v (make-array 5 :fill-pointer 2 :adjustable t)))"
        "  (fill-pointer (adjust-array v 10)))"), 2);
}

TEST(adjust_array_override_fp)
{
    ASSERT_EQ_INT(eval_int(
        "(let ((v (make-array 5 :fill-pointer 2 :adjustable t)))"
        "  (fill-pointer (adjust-array v 10 :fill-pointer 8)))"), 8);
}

TEST(adjust_array_identity)
{
    /* Adjustable arrays: adjust-array returns same object (EQ) */
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 3 :adjustable t)))"
        "  (eq v (adjust-array v 10)))"), "T");
    /* Data accessible through original reference after adjust */
    ASSERT_EQ_INT(eval_int(
        "(let ((v (make-array 3 :adjustable t :initial-element 5)))"
        "  (adjust-array v 6 :initial-element 99)"
        "  (+ (aref v 0) (aref v 4)))"), 104);  /* 5 + 99 */
}

TEST(adjust_array_repeated_regrow)
{
    /* Regression: ADJUST-ARRAY of an array that is ITSELF the result of a
       previous adjust (hence internally DISPLACED to a backing store) must
       allocate a fresh, NON-displaced backing.  Carrying the DISPLACED flag
       into the new backing produced a vector flagged displaced but with no
       backing pointer, so the next cl_vector_data() followed garbage and
       crashed.  Repeated growth of an (unsigned-byte 8) fill-pointer buffer
       is exactly what drakma/chunga does while reading an HTTP response. */
    ASSERT_EQ_INT(eval_int(
        "(let ((v (make-array 4 :element-type '(unsigned-byte 8)"
        "                       :adjustable t :fill-pointer 0)))"
        "  (dotimes (i 3) (vector-push-extend i v))"
        "  (setq v (adjust-array v 8))"
        "  (dotimes (i 5) (vector-push-extend (+ 10 i) v))"
        "  (setq v (adjust-array v 20))"      /* 2nd adjust: input already displaced */
        "  (dotimes (i 5) (vector-push-extend (+ 100 i) v))"
        "  (+ (length v) (aref v 0) (aref v 7) (aref v 12)))"),
        /* len 13 + v[0]=0 + v[7]=14 + v[12]=104 */ 131);
}

TEST(adjust_array_negative_dimension)
{
    /* CLHS: a negative dimension must signal a catchable error — before the
       tier-4 fix the (uint32_t) cast wrapped it to a near-2^32 length whose
       alloc_size computation wrapped again inside cl_make_array: a silent
       heap smash, not an error. */
    ASSERT_STR_EQ(eval_print(
        "(handler-case"
        "  (adjust-array (make-array 3 :adjustable t) -1)"
        "  (error () :caught))"), ":CAUGHT");
    /* List-dims spelling takes the same wrap path */
    ASSERT_STR_EQ(eval_print(
        "(handler-case"
        "  (adjust-array (make-array 3 :adjustable t) '(-7))"
        "  (error () :caught))"), ":CAUGHT");
}

TEST(adjust_array_dimension_limit)
{
    /* Dimensions above ARRAY-DIMENSION-LIMIT are a catchable error too. */
    ASSERT_STR_EQ(eval_print(
        "(handler-case"
        "  (adjust-array (make-array 3 :adjustable t)"
        "                (1+ array-dimension-limit))"
        "  (error () :caught))"), ":CAUGHT");
}

TEST(vector_push_extend_bad_extension)
{
    /* A negative extension arg used to wrap old_len + ext (uint32) and
       allocate a tiny backing store whose new_data[fp] write landed out of
       bounds — must be a catchable error instead. */
    ASSERT_STR_EQ(eval_print(
        "(handler-case"
        "  (let ((v (make-array 2 :fill-pointer 2 :adjustable t)))"
        "    (vector-push-extend 9 v -5))"
        "  (error () :caught))"), ":CAUGHT");
    /* An extension that would exceed ARRAY-DIMENSION-LIMIT errors (the
       extension is a minimum, so clamping would silently violate it). */
    ASSERT_STR_EQ(eval_print(
        "(handler-case"
        "  (let ((v (make-array 2 :fill-pointer 2 :adjustable t)))"
        "    (vector-push-extend 9 v (+ array-dimension-limit 1)))"
        "  (error () :caught))"), ":CAUGHT");
    /* A legal explicit extension still works and honors the minimum. */
    ASSERT_EQ_INT(eval_int(
        "(let ((v (make-array 2 :fill-pointer 2 :adjustable t)))"
        "  (vector-push-extend 9 v 7)"
        "  (aref v 2))"), 9);
}

/* ============================================================ */
/* Displaced arrays                                             */
/* ============================================================ */

TEST(displaced_vector_basic)
{
    /* Displaced vector shares data with target */
    ASSERT_EQ_INT(eval_int(
        "(let* ((a (vector 10 20 30 40 50))"
        "       (d (make-array 3 :displaced-to a :displaced-index-offset 1)))"
        "  (aref d 0))"), 20);
    ASSERT_EQ_INT(eval_int(
        "(let* ((a (vector 10 20 30 40 50))"
        "       (d (make-array 3 :displaced-to a :displaced-index-offset 1)))"
        "  (aref d 2))"), 40);
}

TEST(displaced_vector_offset_zero)
{
    /* Default offset = 0 */
    ASSERT_EQ_INT(eval_int(
        "(let* ((a (vector 10 20 30))"
        "       (d (make-array 2 :displaced-to a)))"
        "  (aref d 0))"), 10);
    ASSERT_EQ_INT(eval_int(
        "(let* ((a (vector 10 20 30))"
        "       (d (make-array 2 :displaced-to a)))"
        "  (aref d 1))"), 20);
}

TEST(displaced_vector_write_through)
{
    /* Writes through displaced array modify the backing array */
    ASSERT_EQ_INT(eval_int(
        "(let* ((a (vector 1 2 3 4 5))"
        "       (d (make-array 3 :displaced-to a :displaced-index-offset 2)))"
        "  (setf (aref d 0) 99)"
        "  (aref a 2))"), 99);
}

TEST(displaced_vector_length)
{
    /* Length of displaced vector is the specified length */
    ASSERT_EQ_INT(eval_int(
        "(let* ((a (vector 1 2 3 4 5))"
        "       (d (make-array 3 :displaced-to a :displaced-index-offset 1)))"
        "  (length d))"), 3);
}

TEST(displaced_string_copy)
{
    /* Displaced to string: creates a copy of the substring */
    ASSERT_STR_EQ(eval_print(
        "(make-array 4 :element-type 'character"
        "  :displaced-to \"hello world\" :displaced-index-offset 6)"),
        "\"worl\"");
}

TEST(displaced_string_equalp)
{
    /* The log4cl kw= pattern: displaced string compared with equalp */
    ASSERT_STR_EQ(eval_print(
        "(let* ((s \":downcase\")"
        "       (sub (make-array (1- (length s))"
        "              :element-type (array-element-type s)"
        "              :displaced-to s :displaced-index-offset 1)))"
        "  (equalp sub \"downcase\"))"), "T");
}

TEST(displaced_char_vector)
{
    /* Displaced to a character vector (fill-pointer string) */
    ASSERT_STR_EQ(eval_print(
        "(let* ((a (make-array 5 :element-type 'character"
        "           :fill-pointer t :initial-element #\\x))"
        "       (d (make-array 3 :displaced-to a :displaced-index-offset 1)))"
        "  d)"), "\"xxx\"");
}

TEST(array_displacement_query)
{
    /* array-displacement returns backing array and offset */
    ASSERT_STR_EQ(eval_print(
        "(let* ((a (vector 1 2 3))"
        "       (d (make-array 2 :displaced-to a :displaced-index-offset 1)))"
        "  (multiple-value-bind (arr off) (array-displacement d)"
        "    (list (eq arr a) off)))"), "(T 1)");
}

TEST(array_displacement_not_displaced)
{
    /* Non-displaced array returns NIL, 0 */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-bind (arr off) (array-displacement (vector 1 2 3))"
        "  (list arr off))"), "(NIL 0)");
}

TEST(displaced_vector_error_bounds)
{
    /* Error when displaced bounds exceed target */
    ASSERT(eval_errors(
        "(make-array 5 :displaced-to (vector 1 2 3) :displaced-index-offset 0)"));
}

TEST(displaced_with_fill_pointer)
{
    /* Displaced vector with fill pointer */
    ASSERT_EQ_INT(eval_int(
        "(let* ((a (vector 10 20 30 40 50))"
        "       (d (make-array 4 :displaced-to a :displaced-index-offset 1"
        "           :fill-pointer 2)))"
        "  (length d))"), 2);
    ASSERT_EQ_INT(eval_int(
        "(let* ((a (vector 10 20 30 40 50))"
        "       (d (make-array 4 :displaced-to a :displaced-index-offset 1"
        "           :fill-pointer 2)))"
        "  (aref d 1))"), 30);
}

TEST(displaced_multidim_read)
{
    /* A multi-dimensional array displaced onto a vector must read its
       elements through the backing (regression: the multi-dim make-array
       path used to ignore :displaced-to and return a fresh NIL array). */
    ASSERT_EQ_INT(eval_int(
        "(let* ((a (make-array 12))"
        "       (d (make-array '(2 3) :displaced-to a :displaced-index-offset 0)))"
        "  (dotimes (i 12) (setf (aref a i) (1+ i)))"
        "  (aref d 1 2))"), 6);   /* row-major 5 -> a[5]=6 */
    /* row-major-aref chases the displacement */
    ASSERT_EQ_INT(eval_int(
        "(let* ((a (make-array 12))"
        "       (d (make-array '(2 3) :displaced-to a :displaced-index-offset 2)))"
        "  (dotimes (i 12) (setf (aref a i) (1+ i)))"
        "  (row-major-aref d 5))"), 8);   /* a[2+5]=a[7]=8 */
}

TEST(displaced_multidim_write_through)
{
    /* Writes through a displaced multi-dim array reach the backing. */
    ASSERT_EQ_INT(eval_int(
        "(let* ((a (make-array 12 :initial-element 0))"
        "       (d (make-array '(2 3) :displaced-to a :displaced-index-offset 2)))"
        "  (setf (aref d 1 2) 99)"   /* row-major 5 -> a[7] */
        "  (aref a 7))"), 99);
}

TEST(displaced_multidim_dims)
{
    /* Shape/rank/total-size of a displaced multi-dim array are its own. */
    ASSERT_STR_EQ(eval_print(
        "(let* ((a (make-array 48))"
        "       (d (make-array '(2 3 3 2) :displaced-to a)))"
        "  (list (array-rank d) (array-dimensions d) (array-total-size d)))"),
        "(4 (2 3 3 2) 36)");
}

TEST(displaced_multidim_displacement_query)
{
    /* array-displacement reports backing + offset for a multi-dim view. */
    ASSERT_STR_EQ(eval_print(
        "(let* ((a (make-array 12))"
        "       (d (make-array '(2 3) :displaced-to a :displaced-index-offset 4)))"
        "  (multiple-value-bind (arr off) (array-displacement d)"
        "    (list (eq arr a) off)))"), "(T 4)");
}

TEST(displaced_multidim_nested)
{
    /* serapeum RESHAPE pattern: a 1-D array displaced onto a multi-dim array
       that is itself displaced onto a vector — the chase must accumulate
       offsets across both levels. */
    ASSERT_EQ_INT(eval_int(
        "(let* ((a (make-array 48))"
        "       (m (make-array '(2 3 3 2) :displaced-to a))"
        "       (r (make-array 36 :displaced-to m)))"
        "  (dotimes (i 48) (setf (aref a i) (1+ i)))"
        "  (aref r 35))"), 36);
}

TEST(displaced_multidim_error_bounds)
{
    /* Offset + total size beyond the backing must error. */
    ASSERT(eval_errors(
        "(make-array '(2 3) :displaced-to (make-array 5) :displaced-index-offset 0)"));
}

/* ============================================================ */
/* Type predicates: arrayp, vectorp, simple-vector-p,           */
/*                  adjustable-array-p                          */
/* ============================================================ */

TEST(arrayp)
{
    ASSERT_STR_EQ(eval_print("(arrayp (vector 1 2 3))"), "T");
    ASSERT_STR_EQ(eval_print("(arrayp (make-array 5))"), "T");
    ASSERT_STR_EQ(eval_print("(arrayp (make-array '(2 3)))"), "T");
    ASSERT_STR_EQ(eval_print("(arrayp \"hello\")"), "T");
    ASSERT_STR_EQ(eval_print("(arrayp 42)"), "NIL");
    ASSERT_STR_EQ(eval_print("(arrayp '(1 2))"), "NIL");
    ASSERT_STR_EQ(eval_print("(arrayp nil)"), "NIL");
}

TEST(vectorp)
{
    ASSERT_STR_EQ(eval_print("(vectorp (vector 1 2))"), "T");
    ASSERT_STR_EQ(eval_print("(vectorp (make-array 3))"), "T");
    ASSERT_STR_EQ(eval_print("(vectorp 42)"), "NIL");
    /* Multi-dim is not a vector */
    ASSERT_STR_EQ(eval_print("(vectorp (make-array '(2 3)))"), "NIL");
}

TEST(simple_vector_p)
{
    ASSERT_STR_EQ(eval_print("(simple-vector-p (vector 1 2 3))"), "T");
    ASSERT_STR_EQ(eval_print("(simple-vector-p (make-array 5))"), "T");
    ASSERT_STR_EQ(eval_print("(simple-vector-p (make-array 5 :fill-pointer 0))"), "NIL");
    ASSERT_STR_EQ(eval_print("(simple-vector-p (make-array 5 :adjustable t))"), "NIL");
    ASSERT_STR_EQ(eval_print("(simple-vector-p (make-array '(2 3)))"), "NIL");
    ASSERT_STR_EQ(eval_print("(simple-vector-p \"hello\")"), "NIL");
    ASSERT_STR_EQ(eval_print("(simple-vector-p 42)"), "NIL");
}

TEST(adjustable_array_p)
{
    ASSERT_STR_EQ(eval_print("(adjustable-array-p (make-array 5 :adjustable t))"), "T");
    ASSERT_STR_EQ(eval_print("(adjustable-array-p (make-array 5))"), "NIL");
    ASSERT_STR_EQ(eval_print("(adjustable-array-p (vector 1 2))"), "NIL");
    ASSERT_STR_EQ(eval_print("(adjustable-array-p \"hello\")"), "NIL");
}

/* ============================================================ */
/* typep with array type specifiers                             */
/* ============================================================ */

TEST(typep_array)
{
    ASSERT_STR_EQ(eval_print("(typep (vector 1 2) 'array)"), "T");
    ASSERT_STR_EQ(eval_print("(typep (make-array '(2 3)) 'array)"), "T");
    ASSERT_STR_EQ(eval_print("(typep \"hello\" 'array)"), "T");
    ASSERT_STR_EQ(eval_print("(typep 42 'array)"), "NIL");
}

TEST(typep_vector)
{
    ASSERT_STR_EQ(eval_print("(typep (vector 1 2) 'vector)"), "T");
    ASSERT_STR_EQ(eval_print("(typep \"hello\" 'vector)"), "T");
    ASSERT_STR_EQ(eval_print("(typep (make-array '(2 3)) 'vector)"), "NIL");
    ASSERT_STR_EQ(eval_print("(typep 42 'vector)"), "NIL");
}

TEST(typep_simple_vector)
{
    ASSERT_STR_EQ(eval_print("(typep (vector 1 2) 'simple-vector)"), "T");
    ASSERT_STR_EQ(eval_print("(typep (make-array 5) 'simple-vector)"), "T");
    ASSERT_STR_EQ(eval_print("(typep (make-array 5 :fill-pointer 0) 'simple-vector)"), "NIL");
    ASSERT_STR_EQ(eval_print("(typep (make-array 5 :adjustable t) 'simple-vector)"), "NIL");
    ASSERT_STR_EQ(eval_print("(typep \"hello\" 'simple-vector)"), "NIL");
    ASSERT_STR_EQ(eval_print("(typep (make-array '(2 3)) 'simple-vector)"), "NIL");
}

TEST(typep_simple_array)
{
    ASSERT_STR_EQ(eval_print("(typep (vector 1 2) 'simple-array)"), "T");
    ASSERT_STR_EQ(eval_print("(typep (make-array '(2 3)) 'simple-array)"), "T");
    ASSERT_STR_EQ(eval_print("(typep \"hello\" 'simple-array)"), "T");
    ASSERT_STR_EQ(eval_print("(typep (make-array 5 :fill-pointer 0) 'simple-array)"), "NIL");
    ASSERT_STR_EQ(eval_print("(typep (make-array 5 :adjustable t) 'simple-array)"), "NIL");
}

/* Regression: (simple-array ET (*)) must honor the element type — a general T
 * vector is NOT a (simple-array bit (*)).  This was the second half of the
 * serapeum RANGE/SBIT bug: WITH-TYPE-DISPATCH lists (simple-array bit (*))
 * first, so a T vector that wrongly matched it dispatched to SBIT. */
TEST(typep_simple_array_element_type)
{
    /* general T vector */
    ASSERT_STR_EQ(eval_print("(typep (vector 1 2) '(simple-array t (*)))"), "T");
    ASSERT_STR_EQ(eval_print("(typep (vector 1 2) '(simple-array bit (*)))"), "NIL");
    ASSERT_STR_EQ(eval_print("(typep (vector 1 2) '(simple-array character (*)))"), "NIL");
    ASSERT_STR_EQ(eval_print("(typep (vector 1 2) '(simple-array * (*)))"), "T");
    /* bit-vector */
    ASSERT_STR_EQ(eval_print("(typep #*101 '(simple-array bit (*)))"), "T");
    ASSERT_STR_EQ(eval_print("(typep #*101 '(simple-array t (*)))"), "NIL");
    /* string */
    ASSERT_STR_EQ(eval_print("(typep \"ab\" '(simple-array character (*)))"), "T");
    ASSERT_STR_EQ(eval_print("(typep \"ab\" '(simple-array t (*)))"), "NIL");
    ASSERT_STR_EQ(eval_print("(typep \"ab\" '(simple-array bit (*)))"), "NIL");
    /* integer subtype of BIT names the bit-vector type too */
    ASSERT_STR_EQ(eval_print("(typep #*101 '(simple-array (integer 0 1) (*)))"), "T");
    ASSERT_STR_EQ(eval_print("(typep (vector 1 2) '(simple-array (integer 0 1) (*)))"), "NIL");
}

/* ============================================================ */
/* type-of for arrays                                           */
/* ============================================================ */

TEST(type_of_array)
{
    ASSERT_STR_EQ(eval_print("(type-of (vector 1 2 3))"), "SIMPLE-VECTOR");
    ASSERT_STR_EQ(eval_print("(type-of (make-array 5))"), "SIMPLE-VECTOR");
    ASSERT_STR_EQ(eval_print("(type-of (make-array 5 :fill-pointer 0))"), "VECTOR");
    ASSERT_STR_EQ(eval_print("(type-of (make-array 5 :adjustable t))"), "VECTOR");
    ASSERT_STR_EQ(eval_print("(type-of (make-array '(2 3)))"), "SIMPLE-ARRAY");
    ASSERT_STR_EQ(eval_print("(type-of \"hello\")"), "STRING");
}

/* ============================================================ */
/* Reader #(...) syntax                                         */
/* ============================================================ */

TEST(reader_vector_literal)
{
    ASSERT_STR_EQ(eval_print("#(1 2 3)"), "#(1 2 3)");
    ASSERT_STR_EQ(eval_print("#()"), "#()");
    ASSERT_EQ_INT(eval_int("(aref #(10 20 30) 1)"), 20);
    ASSERT_EQ_INT(eval_int("(length #(a b c d))"), 4);
    ASSERT_STR_EQ(eval_print("(simple-vector-p #(1 2))"), "T");
    ASSERT_STR_EQ(eval_print("(vectorp #(1))"), "T");
}

TEST(reader_vector_nested)
{
    ASSERT_STR_EQ(eval_print("(aref #(#(1 2) #(3 4)) 0)"), "#(1 2)");
    ASSERT_EQ_INT(eval_int("(aref (aref #(#(1 2) #(3 4)) 1) 0)"), 3);
}

TEST(reader_vector_mixed_types)
{
    ASSERT_STR_EQ(eval_print("#(1 \"hello\" a)"), "#(1 \"hello\" A)");
}

/* ============================================================ */
/* Printer: 1D vectors                                          */
/* ============================================================ */

TEST(print_1d_vector)
{
    ASSERT_STR_EQ(eval_print("(vector 1 2 3)"), "#(1 2 3)");
    ASSERT_STR_EQ(eval_print("(vector)"), "#()");
    /* Fill pointer: only active elements printed */
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 5 :fill-pointer 0)))"
        "  (vector-push 10 v)"
        "  (vector-push 20 v)"
        "  v)"), "#(10 20)");
}

TEST(print_1d_with_print_length)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-length* 2)) (write-to-string (vector 1 2 3 4)))"),
        "\"#(1 2...)\"");
}

/* ============================================================ */
/* Printer: multi-dimensional                                   */
/* ============================================================ */

TEST(print_2d)
{
    ASSERT_STR_EQ(eval_print(
        "(make-array '(2 3) :initial-contents '((1 2 3) (4 5 6)))"),
        "#2A((1 2 3) (4 5 6))");
}

TEST(print_3d)
{
    ASSERT_STR_EQ(eval_print(
        "(make-array '(2 2 2) :initial-contents '(((1 2) (3 4)) ((5 6) (7 8))))"),
        "#3A(((1 2) (3 4)) ((5 6) (7 8)))");
}

TEST(make_array_multidim_size_overflow)
{
    /* Regression: the multi-dim total-size product had no negative /
     * dimension-limit / uint32-overflow checks (the 1-D path has all
     * three).  '(65536 65537) wrapped total to 65536, allocating a
     * small vector whose stored dims claimed the full extent — every
     * per-dimension-checked AREF write then landed far outside the
     * allocation (in-arena OOB writes = silent heap corruption). */
    ASSERT(eval_errors("(make-array '(65536 65537))"));
    ASSERT(eval_errors("(make-array '(16777216 16777216))"));
    ASSERT(eval_errors("(make-array '(3 -1))"));
    ASSERT(eval_errors("(make-array '(1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17))"));
    /* Must be a catchable error, not a VM abort */
    ASSERT_STR_EQ(":CAUGHT",
        eval_print("(handler-case (make-array '(65536 65537)) (error () :caught))"));
    /* Sane multi-dim arrays still work */
    ASSERT_STR_EQ("7",
        eval_print("(let ((a (make-array '(2 3) :initial-element 7))) (aref a 1 2))"));
}

TEST(make_hash_table_size_overflow)
{
    /* Regression: a huge or negative :size wrapped the bucket-count
     * computation (requested*4+2 and bucket_count*sizeof(CL_Obj)),
     * allocating a ~24-byte table whose bucket_count field claimed 2^31
     * buckets — every gethash/puthash then indexed wildly outside the
     * object. */
    ASSERT(eval_errors("(make-hash-table :size -1)"));
    /* Huge :size is a hint (CLHS): clamped to CL_HT_MAX_BUCKETS.  In a
     * small test heap the (legitimate, clamped) 4MB bucket vector may
     * still exceed free space — that must be a clean catchable error.
     * Pre-fix this wrapped to a ~24-byte allocation claiming 2^30
     * buckets and gethash wild-wrote far outside it (crash). */
    {
        const char *r = eval_print(
            "(handler-case"
            " (let ((ht (make-hash-table :size most-positive-fixnum)))"
            "  (setf (gethash 'a ht) 42) (gethash 'a ht))"
            " (error () :clean-error))");
        ASSERT(strcmp(r, "42") == 0 || strcmp(r, ":CLEAN-ERROR") == 0 ||
               strncmp(r, "ERROR:", 6) == 0 /* storage-error via CL_CATCH */);
    }
    /* A large-but-allocatable :size must produce a working table */
    ASSERT_STR_EQ("42",
        eval_print("(let ((ht (make-hash-table :size 5000)))"
                   " (setf (gethash 'a ht) 42) (gethash 'a ht))"));
}

TEST(print_2d_empty)
{
    ASSERT_STR_EQ(eval_print("(make-array '(2 0))"), "#2A(() ())");
    ASSERT_STR_EQ(eval_print("(make-array '(0 3))"), "#2A()");
}

TEST(print_array_nil)
{
    /* *print-array* nil: unreadable format */
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-array* nil)) (write-to-string (vector 1 2)))"),
        "\"#<VECTOR>\"");
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-array* nil)) (write-to-string (make-array '(2 3))))"),
        "\"#<ARRAY>\"");
}

/* ============================================================ */
/* subtypep for array types                                     */
/* ============================================================ */

TEST(subtypep_array)
{
    ASSERT_STR_EQ(eval_print("(subtypep 'simple-vector 'vector)"), "T");
    ASSERT_STR_EQ(eval_print("(subtypep 'simple-vector 'array)"), "T");
    ASSERT_STR_EQ(eval_print("(subtypep 'simple-vector 'sequence)"), "T");
    ASSERT_STR_EQ(eval_print("(subtypep 'vector 'array)"), "T");
    ASSERT_STR_EQ(eval_print("(subtypep 'vector 'sequence)"), "T");
    ASSERT_STR_EQ(eval_print("(subtypep 'string 'vector)"), "T");
    ASSERT_STR_EQ(eval_print("(subtypep 'string 'array)"), "T");
    ASSERT_STR_EQ(eval_print("(subtypep 'simple-array 'array)"), "T");
    /* Not subtypes */
    ASSERT_STR_EQ(eval_print("(subtypep 'array 'vector)"), "NIL");
    ASSERT_STR_EQ(eval_print("(subtypep 'vector 'simple-vector)"), "NIL");
}

/* ============================================================ */
/* main                                                         */
/* ============================================================ */

int main(void)
{
    test_init();
    setup();

    /* Construction */
    RUN(make_array_simple);
    RUN(make_array_bad_dimension_is_catchable);
    RUN(make_array_initial_element);
    RUN(make_array_initial_contents);
    RUN(make_array_fill_pointer);
    RUN(make_array_fill_pointer_t_string);
    RUN(coerce_nil_to_string_is_empty);
    RUN(char_accessor_on_string_vector);
    RUN(schar_requires_simple_string);
    RUN(replace_into_string_type_checks);
    RUN(make_array_initial_contents_string);
    RUN(make_array_base_char_is_base_string);
    RUN(make_array_character_is_wide_string);
    RUN(make_array_base_char_wide_content_widens);
    RUN(make_array_string_class_of_is_string);
    RUN(make_array_specialized_element_type);
    RUN(seq_funcs_preserve_specialized_element_type);
    RUN(make_array_adjustable);
    RUN(make_array_2d);
    RUN(make_array_3d);
    RUN(make_array_2d_initial_contents);
    RUN(make_array_3d_initial_contents);
    RUN(make_array_2d_initial_element);
    RUN(make_array_empty_dims);

    /* vector function */
    RUN(vector_constructor);

    /* Access */
    RUN(aref_1d);
    RUN(aref_2d);
    RUN(aref_3d);
    RUN(aref_bounds_error);
    RUN(svref_basic);
    RUN(svref_rejects_nonsimple);
    RUN(setf_aref_1d);
    RUN(setf_aref_2d);
    RUN(setf_aref_3d);

    /* Query */
    RUN(array_dimension);
    RUN(array_total_size);
    RUN(array_row_major_index);
    RUN(array_ops_on_string);
    RUN(row_major_aref);
    RUN(setf_row_major_aref);

    /* Fill pointer */
    RUN(fill_pointer_ops);
    RUN(setf_fill_pointer);
    RUN(fill_pointer_bounds);
    RUN(vector_push);
    RUN(vector_push_pop_bit_vector);
    RUN(vector_push_extend_bit_vector);
    RUN(vector_push_extend_bit_vector_grows);
    RUN(equal_bit_vector_fill_pointer);
    RUN(adjust_array_bit_vector);
    RUN(grown_bit_vector_survives_gc);
    RUN(vector_push_extend_basic);
    RUN(vector_push_extend_multiple);
    RUN(vector_push_extend_identity);
    RUN(vector_push_extend_zero_capacity);
    RUN(vector_push_extend_nonadjustable_basic);
    RUN(vector_push_extend_nonadjustable_data);
    RUN(vector_push_extend_nonadjustable_many);
    RUN(vector_push_extend_nonadjustable_identity);
    RUN(vector_push_extend_nonadjustable_still_nonadjustable);
    RUN(vector_push_extend_nonadjustable_character);
    RUN(vector_push_extend_nonadjustable_character_equal);
    RUN(equal_string_vs_char_vector);
    RUN(fill_pointer_string_as_equal_hash_key);

    /* Adjust */
    RUN(adjust_array_grow);
    RUN(adjust_array_shrink);
    RUN(adjust_array_preserves_fp);
    RUN(adjust_array_override_fp);
    RUN(adjust_array_repeated_regrow);
    RUN(adjust_array_identity);
    RUN(adjust_array_negative_dimension);
    RUN(adjust_array_dimension_limit);
    RUN(vector_push_extend_bad_extension);

    /* Displaced arrays */
    RUN(displaced_vector_basic);
    RUN(displaced_vector_offset_zero);
    RUN(displaced_vector_write_through);
    RUN(displaced_vector_length);
    RUN(displaced_string_copy);
    RUN(displaced_string_equalp);
    RUN(displaced_char_vector);
    RUN(array_displacement_query);
    RUN(array_displacement_not_displaced);
    RUN(displaced_vector_error_bounds);
    RUN(displaced_with_fill_pointer);
    RUN(displaced_multidim_read);
    RUN(displaced_multidim_write_through);
    RUN(displaced_multidim_dims);
    RUN(displaced_multidim_displacement_query);
    RUN(displaced_multidim_nested);
    RUN(displaced_multidim_error_bounds);

    /* Type predicates */
    RUN(arrayp);
    RUN(vectorp);
    RUN(simple_vector_p);
    RUN(adjustable_array_p);

    /* typep */
    RUN(typep_array);
    RUN(typep_vector);
    RUN(typep_simple_vector);
    RUN(typep_simple_array);
    RUN(typep_simple_array_element_type);

    /* type-of */
    RUN(type_of_array);

    /* Reader */
    RUN(reader_vector_literal);
    RUN(reader_vector_nested);
    RUN(reader_vector_mixed_types);

    /* Printer */
    RUN(print_1d_vector);
    RUN(print_1d_with_print_length);
    RUN(print_2d);
    RUN(print_3d);
    RUN(print_2d_empty);
    RUN(make_array_multidim_size_overflow);
    RUN(make_hash_table_size_overflow);
    RUN(print_array_nil);

    /* subtypep */
    RUN(subtypep_array);

    /* deftype element-type expansion (flexi-streams octets-to-string regression) */
    RUN(make_array_deftype_character_is_string);
    RUN(upgraded_array_element_type_expands_deftype);
    RUN(upgraded_array_element_type_bit_subtypes);

    teardown();
    REPORT();
}
