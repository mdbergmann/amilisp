/*
 * test_amiga_ffi.c — host-side specification of the AMIGA package surface
 * that the OS-binding modules (lib/amiga/ffi.lisp, lib/amiga/raw/) rely on.
 *
 * What is pinned here:
 *   - cl_amiga_box_result: the d0 -> Lisp boxing per result kind
 *     (unsigned / void / pointer / signed) shared by OP_AMIGA_CALL, the JIT
 *     and CALL-LIBRARY*.  The encoding is platform-neutral, so it is tested
 *     on the host even though no library call can be made here.
 *   - The AMIGA package exists on every platform with the same builtin
 *     names; on the host they signal a clear "only available on
 *     AmigaOS/MorphOS" error instead of being undefined.
 *   - AMIGA.FFI:DEFCFUN's stub: register nibbles, result kind in bits
 *     28-31 (including the BOOL/WORD/BYTE kinds that used to be Lisp
 *     post-processors), arity, the >7-register plist fallback, the :A5
 *     rejection, *DEFCFUN-DOCSTRINGS*.
 *   - FFI:DEFCSTRUCT's :fptr / (:struct N) / (:array T N) / :size, now as
 *     field stubs with bounds-checked array accessors.
 *
 * The stub object itself (type, GC, FASL, printer, VM dispatch) is pinned
 * by tests/test_ffi_stub.c; the Amiga-side half (calls that actually reach
 * the OS) lives in tests/amiga/test-raw-bindings.lisp.
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
#include "core/thread.h"
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

/* Eval STR; return the printed result, or "ERROR:<msg>" on a signalled
 * error (the message text is what the tests below match against). */
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
        snprintf(buf, sizeof(buf), "ERROR:%s", cl_error_msg);
        return buf;
    }
}

static int contains(const char *haystack, const char *needle)
{
    return strstr(haystack, needle) != NULL;
}

/* ================================================================
 * cl_amiga_box_result
 * ================================================================ */

TEST(box_unsigned_small_is_fixnum)
{
    CL_Obj r = cl_amiga_box_result(42, CL_AMIGA_RES_UNSIGNED);
    ASSERT(CL_FIXNUM_P(r));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(r), 42);
}

TEST(box_unsigned_max_fixnum_boundary)
{
    CL_Obj r = cl_amiga_box_result((uint32_t)CL_FIXNUM_MAX, CL_AMIGA_RES_UNSIGNED);
    ASSERT(CL_FIXNUM_P(r));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(r), CL_FIXNUM_MAX);
}

TEST(box_unsigned_large_is_positive_bignum)
{
    /* 0xFFFFFFFF as ULONG must stay 4294967295, never -1 */
    CL_Obj r = cl_amiga_box_result(0xFFFFFFFFu, CL_AMIGA_RES_UNSIGNED);
    CL_Bignum *b;
    ASSERT(CL_BIGNUM_P(r));
    b = (CL_Bignum *)CL_OBJ_TO_PTR(r);
    ASSERT_EQ_INT(b->sign, 0);
    ASSERT_EQ_INT(b->length, 2);
    ASSERT_EQ_INT(b->limbs[0], 0xFFFF);
    ASSERT_EQ_INT(b->limbs[1], 0xFFFF);
}

TEST(box_unsigned_just_above_fixnum)
{
    CL_Obj r = cl_amiga_box_result((uint32_t)CL_FIXNUM_MAX + 1u, CL_AMIGA_RES_UNSIGNED);
    CL_Bignum *b;
    ASSERT(CL_BIGNUM_P(r));
    b = (CL_Bignum *)CL_OBJ_TO_PTR(r);
    ASSERT_EQ_INT(b->sign, 0);
    ASSERT_EQ_INT(b->limbs[0], 0x0000);
    ASSERT_EQ_INT(b->limbs[1], 0x4000);
}

TEST(box_void_is_nil)
{
    ASSERT(CL_NULL_P(cl_amiga_box_result(12345, CL_AMIGA_RES_VOID)));
    ASSERT(CL_NULL_P(cl_amiga_box_result(0, CL_AMIGA_RES_VOID)));
}

TEST(box_pointer_null_is_nil)
{
    ASSERT(CL_NULL_P(cl_amiga_box_result(0, CL_AMIGA_RES_POINTER)));
}

TEST(box_pointer_nonnull_is_foreign_pointer)
{
    CL_Obj r = cl_amiga_box_result(0x07F00010u, CL_AMIGA_RES_POINTER);
    CL_ForeignPtr *p;
    ASSERT(CL_FOREIGN_POINTER_P(r));
    p = (CL_ForeignPtr *)CL_OBJ_TO_PTR(r);
    ASSERT_EQ_INT(p->address, (uint32_t)0x07F00010u);
    ASSERT_EQ_INT(p->size, 0);
    ASSERT_EQ_INT(p->flags, 0);       /* unowned: the OS owns the object */
}

TEST(box_pointer_high_address)
{
    /* Zorro-III / MorphOS addresses above 2^30 must not lose bits */
    CL_Obj r = cl_amiga_box_result(0xC0001234u, CL_AMIGA_RES_POINTER);
    ASSERT(CL_FOREIGN_POINTER_P(r));
    ASSERT_EQ_INT(((CL_ForeignPtr *)CL_OBJ_TO_PTR(r))->address, (uint32_t)0xC0001234u);
}

TEST(box_signed_minus_one)
{
    /* the LONG -1 that used to come back as 4294967295 */
    CL_Obj r = cl_amiga_box_result(0xFFFFFFFFu, CL_AMIGA_RES_SIGNED);
    ASSERT(CL_FIXNUM_P(r));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(r), -1);
}

TEST(box_signed_positive_small)
{
    CL_Obj r = cl_amiga_box_result(1000, CL_AMIGA_RES_SIGNED);
    ASSERT(CL_FIXNUM_P(r));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(r), 1000);
}

TEST(box_signed_fixnum_min_boundary)
{
    CL_Obj r = cl_amiga_box_result((uint32_t)CL_FIXNUM_MIN, CL_AMIGA_RES_SIGNED);
    ASSERT(CL_FIXNUM_P(r));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(r), CL_FIXNUM_MIN);
}

TEST(box_signed_large_negative_is_negative_bignum)
{
    /* -2^30 - 1 = 0xBFFFFFFF */
    CL_Obj r = cl_amiga_box_result(0xBFFFFFFFu, CL_AMIGA_RES_SIGNED);
    CL_Bignum *b;
    ASSERT(CL_BIGNUM_P(r));
    b = (CL_Bignum *)CL_OBJ_TO_PTR(r);
    ASSERT_EQ_INT(b->sign, 1);
    ASSERT_EQ_INT(b->limbs[0], 0x0001);
    ASSERT_EQ_INT(b->limbs[1], 0x4000);   /* magnitude 0x40000001 */
}

TEST(box_signed_int32_min)
{
    CL_Obj r = cl_amiga_box_result(0x80000000u, CL_AMIGA_RES_SIGNED);
    CL_Bignum *b;
    ASSERT(CL_BIGNUM_P(r));
    b = (CL_Bignum *)CL_OBJ_TO_PTR(r);
    ASSERT_EQ_INT(b->sign, 1);
    ASSERT_EQ_INT(b->limbs[0], 0x0000);
    ASSERT_EQ_INT(b->limbs[1], 0x8000);
}

TEST(box_signed_large_positive_is_positive_bignum)
{
    CL_Obj r = cl_amiga_box_result(0x7FFFFFFFu, CL_AMIGA_RES_SIGNED);
    CL_Bignum *b;
    ASSERT(CL_BIGNUM_P(r));
    b = (CL_Bignum *)CL_OBJ_TO_PTR(r);
    ASSERT_EQ_INT(b->sign, 0);
    ASSERT_EQ_INT(b->limbs[0], 0xFFFF);
    ASSERT_EQ_INT(b->limbs[1], 0x7FFF);
}

TEST(res_kind_macro_decodes_bits_28_31)
{
    ASSERT_EQ_INT(CL_AMIGA_RES_KIND(0x00000008u), CL_AMIGA_RES_UNSIGNED);
    ASSERT_EQ_INT(CL_AMIGA_RES_KIND(0x10000008u), CL_AMIGA_RES_VOID);
    ASSERT_EQ_INT(CL_AMIGA_RES_KIND(0x20000008u), CL_AMIGA_RES_POINTER);
    ASSERT_EQ_INT(CL_AMIGA_RES_KIND(0x30000008u), CL_AMIGA_RES_SIGNED);
    ASSERT_EQ_INT(CL_AMIGA_RES_KIND(0x40000008u), CL_AMIGA_RES_BOOL);
    ASSERT_EQ_INT(CL_AMIGA_RES_KIND(0x50000008u), CL_AMIGA_RES_U16);
    ASSERT_EQ_INT(CL_AMIGA_RES_KIND(0x60000008u), CL_AMIGA_RES_I16);
    ASSERT_EQ_INT(CL_AMIGA_RES_KIND(0x70000008u), CL_AMIGA_RES_U8);
    ASSERT_EQ_INT(CL_AMIGA_RES_KIND(0x80000008u), CL_AMIGA_RES_I8);
    /* the Lisp-visible (%ffi-call / call-library-fast) regspec is a fixnum:
     * kinds 0-3 fit, the stub carries the wider kinds in its u32 */
    ASSERT(0x3FFFFFFFu <= (uint32_t)CL_FIXNUM_MAX);
    ASSERT(0x40000000u >  (uint32_t)CL_FIXNUM_MAX);
    ASSERT_EQ_INT((int)CL_AMIGA_MAKE_REGSPEC(0x109u, CL_AMIGA_RES_I16),
                  (int)0x60000109u);
    ASSERT_EQ_INT((int)CL_AMIGA_REGSPEC_NIBBLES(0x60000109u), 0x109);
}

/* The sub-32-bit kinds box only d0.w / d0.b — the upper bits of d0 are
 * undefined for a BOOL/WORD/BYTE C result and must be masked off. */
TEST(box_bool_tests_low_word_only)
{
    ASSERT(CL_NULL_P(cl_amiga_box_result(0x00010000u, CL_AMIGA_RES_BOOL)));
    ASSERT(cl_amiga_box_result(0x00000001u, CL_AMIGA_RES_BOOL) == CL_T);
    ASSERT(cl_amiga_box_result(0xFFFFFFFFu, CL_AMIGA_RES_BOOL) == CL_T);
    ASSERT(cl_amiga_box_result(0x12340000u, CL_AMIGA_RES_BOOL) == CL_NIL);
}

TEST(box_u16_i16_u8_i8_mask_and_sign_extend)
{
    CL_Obj r;
    r = cl_amiga_box_result(0x1234FFFFu, CL_AMIGA_RES_U16);
    ASSERT(CL_FIXNUM_P(r)); ASSERT_EQ_INT(CL_FIXNUM_VAL(r), 65535);
    r = cl_amiga_box_result(0x0000FFFFu, CL_AMIGA_RES_I16);
    ASSERT(CL_FIXNUM_P(r)); ASSERT_EQ_INT(CL_FIXNUM_VAL(r), -1);
    r = cl_amiga_box_result(0x12347FFFu, CL_AMIGA_RES_I16);
    ASSERT(CL_FIXNUM_P(r)); ASSERT_EQ_INT(CL_FIXNUM_VAL(r), 32767);
    r = cl_amiga_box_result(0x000001FFu, CL_AMIGA_RES_U8);
    ASSERT(CL_FIXNUM_P(r)); ASSERT_EQ_INT(CL_FIXNUM_VAL(r), 255);
    r = cl_amiga_box_result(200u, CL_AMIGA_RES_I8);
    ASSERT(CL_FIXNUM_P(r)); ASSERT_EQ_INT(CL_FIXNUM_VAL(r), -56);
    r = cl_amiga_box_result(0xFFFFFF7Fu, CL_AMIGA_RES_I8);
    ASSERT(CL_FIXNUM_P(r)); ASSERT_EQ_INT(CL_FIXNUM_VAL(r), 127);
}

/* ================================================================
 * AMIGA package surface on the host
 * ================================================================ */

/* ================================================================
 * cl_amiga_ffi_arg_to_u32 — what a register argument may be
 * ================================================================ */

TEST(arg_to_u32_integers_pointers_and_booleans)
{
    CL_Obj fp = cl_make_foreign_pointer(0x00DFF000u, 0, 0);

    ASSERT(cl_amiga_ffi_arg_to_u32(CL_MAKE_FIXNUM(42), 1, 0) == 42u);
    ASSERT(cl_amiga_ffi_arg_to_u32(CL_MAKE_FIXNUM(0), 1, 0) == 0u);
    /* a negative LONG is its two's-complement longword */
    ASSERT(cl_amiga_ffi_arg_to_u32(CL_MAKE_FIXNUM(-1), 1, 0) == 0xFFFFFFFFu);
    ASSERT(cl_amiga_ffi_arg_to_u32(CL_MAKE_FIXNUM(-2), 1, 0) == 0xFFFFFFFEu);
    /* bignums: the low 32 bits, two's complement for negatives (the sign
     * used to be dropped: -1073741825 came out as +1073741825) */
    ASSERT(cl_amiga_ffi_arg_to_u32(cl_eval_string("#xFFFFFFFF"), 1, 0) == 0xFFFFFFFFu);
    ASSERT(cl_amiga_ffi_arg_to_u32(cl_eval_string("#x80000000"), 1, 0) == 0x80000000u);
    ASSERT(cl_amiga_ffi_arg_to_u32(cl_eval_string("#x746578"), 1, 0) == 0x746578u);
    ASSERT(cl_amiga_ffi_arg_to_u32(cl_eval_string("-2147483648"), 1, 0) == 0x80000000u);
    ASSERT(cl_amiga_ffi_arg_to_u32(cl_eval_string("-1073741825"), 1, 0) == 0xBFFFFFFFu);
    /* a foreign pointer is its address */
    ASSERT(cl_amiga_ffi_arg_to_u32(fp, 1, 8) == 0x00DFF000u);
    /* NIL is NULL / FALSE, T is TRUE -- the tag-value coercion of
     * AMIGA.BOOPSI, so (rethink-layout group win nil t) is a valid call */
    ASSERT(cl_amiga_ffi_arg_to_u32(CL_NIL, 1, 8) == 0u);
    ASSERT(cl_amiga_ffi_arg_to_u32(CL_T, 4, 0) == 1u);
}

/* Call the converter on VAL for argument ARG_INDEX in register REG_IDX and
 * return the error message it signals, or NULL if it accepted the value. */
static const char *arg_to_u32_error(CL_Obj val, int arg_index, int reg_idx)
{
    static char buf[512];
    int err;
    CL_CATCH(err);
    if (err == CL_ERR_NONE) {
        (void)cl_amiga_ffi_arg_to_u32(val, arg_index, reg_idx);
        CL_UNCATCH();
        return NULL;
    }
    CL_UNCATCH();
    snprintf(buf, sizeof(buf), "%d:%s", err, cl_error_msg);
    return buf;
}

TEST(arg_to_u32_rejects_other_objects_naming_argument_and_register)
{
    const char *m;

    /* a string: the message names the argument position, the register the
     * argument was bound for, what is accepted, and the value itself */
    m = arg_to_u32_error(cl_eval_string("\"nope\""), 2, 8);
    ASSERT(m != NULL);
    ASSERT(contains(m, "register argument 2 (:A0)"));
    ASSERT(contains(m, "must be an integer, a foreign pointer, T or NIL"));
    ASSERT(contains(m, "got \"nope\""));
    {
        char kind[8];
        snprintf(kind, sizeof(kind), "%d:", CL_ERR_TYPE);
        ASSERT(strncmp(m, kind, strlen(kind)) == 0);   /* a TYPE-ERROR */
    }
    /* a float, a symbol, a list, a character: not register images either */
    m = arg_to_u32_error(cl_eval_string("1.5"), 1, 0);
    ASSERT(m != NULL && contains(m, "register argument 1 (:D0)") && contains(m, "got 1.5"));
    m = arg_to_u32_error(cl_eval_string("':foo"), 7, 6);
    ASSERT(m != NULL && contains(m, "register argument 7 (:D6)") && contains(m, "got :FOO"));
    m = arg_to_u32_error(cl_eval_string("'(1 2)"), 3, 12);
    ASSERT(m != NULL && contains(m, "register argument 3 (:A4)") && contains(m, "got (1 2)"));
    m = arg_to_u32_error(cl_eval_string("#\\x"), 1, 1);
    ASSERT(m != NULL && contains(m, "(:D1)"));
    /* the accepted kinds do not signal */
    ASSERT(arg_to_u32_error(CL_T, 1, 0) == NULL);
    ASSERT(arg_to_u32_error(CL_NIL, 1, 0) == NULL);
    ASSERT(arg_to_u32_error(CL_MAKE_FIXNUM(7), 1, 0) == NULL);
}

TEST(host_dispatch_checks_register_args_before_platform_error)
{
    const char *r;
    /* runs after defcfun_installs_stub_with_regspec_and_kind, which asserts
     * on being the FIRST to require amiga/ffi; here it is a no-op */
    eval_print("(require \"amiga/ffi\")");
    /* a defcfun over an OPEN base (any foreign pointer will do on the host):
     * RethinkLayout(gadget, window, requester, refresh) (A0,A1,A2,D0) */
    eval_print("(defvar cl-user::*tb-open* (ffi:make-foreign-pointer 4))");
    ASSERT_STR_EQ(eval_print("(amiga.ffi:defcfun cl-user::rethink cl-user::*tb-open* -48 "
                             "(:a0 gadget :a1 window :a2 requester :d0 refresh) :result :bool)"),
                  "RETHINK");
    /* T and NIL are register images (1 and 0): every argument is accepted
     * and the call gets as far as the platform check -- direct call site
     * (OP_AMIGA_CALL), FUNCALL and APPLY (the stub's own dispatch) alike */
    r = eval_print("(cl-user::rethink (ffi:make-foreign-pointer 8) nil nil t)");
    ASSERT(contains(r, "only available on AmigaOS/MorphOS"));
    r = eval_print("(funcall #'cl-user::rethink nil nil nil t)");
    ASSERT(contains(r, "only available on AmigaOS/MorphOS"));
    r = eval_print("(apply #'cl-user::rethink (list 1 2 3 t))");
    ASSERT(contains(r, "only available on AmigaOS/MorphOS"));
    /* a string in a register slot is the argument error, on every path,
     * naming the argument, its register and the value */
    r = eval_print("(cl-user::rethink nil nil nil \"yes\")");
    ASSERT(contains(r, "ERROR:"));
    ASSERT(contains(r, "register argument 4 (:D0)"));
    ASSERT(contains(r, "got \"yes\""));
    ASSERT(!contains(r, "only available"));
    r = eval_print("(funcall #'cl-user::rethink 1.5 nil nil t)");
    ASSERT(contains(r, "register argument 1 (:A0)"));
    ASSERT(contains(r, "got 1.5"));
    r = eval_print("(apply #'cl-user::rethink (list nil nil :req t))");
    ASSERT(contains(r, "register argument 3 (:A2)"));
    ASSERT(contains(r, "got :REQ"));
}

TEST(amiga_package_exists_on_host)
{
    ASSERT_STR_EQ(eval_print("(package-name (find-package \"AMIGA\"))"), "\"AMIGA\"");
    ASSERT_STR_EQ(eval_print("(and (fboundp 'amiga:open-library) "
                             "(fboundp 'amiga:close-library) "
                             "(fboundp 'amiga:call-library) "
                             "(fboundp 'amiga:call-library-fast) "
                             "(fboundp 'amiga:alloc-chip) "
                             "(fboundp 'amiga:free-chip) "
                             "(fboundp 'amiga:arexx-open) t)"), "T");
    ASSERT_STR_EQ(eval_print("(symbol-package 'amiga:%ffi-call)"), "#<PACKAGE AMIGA>");
}

TEST(amiga_stub_signals_platform_error)
{
    const char *r = eval_print("(amiga:open-library \"intuition.library\" 0)");
    ASSERT(contains(r, "ERROR:"));
    ASSERT(contains(r, "OPEN-LIBRARY"));
    ASSERT(contains(r, "only available on AmigaOS/MorphOS"));
    r = eval_print("(amiga:call-library (ffi:make-foreign-pointer 4) -216 (list :d1 0))");
    ASSERT(contains(r, "CALL-LIBRARY"));
    ASSERT(contains(r, "only available on AmigaOS/MorphOS"));
}

TEST(amiga_call_library_arity_accepts_result_kind)
{
    /* 3..4 args: the 4th is the result kind (the stub still checks arity
     * before erroring, so a 5th argument is an arity error, not the
     * platform error) */
    const char *r = eval_print("(amiga:call-library 1 2 3 4 5)");
    ASSERT(contains(r, "ERROR:"));
    ASSERT(!contains(r, "only available"));
}

/* ================================================================
 * AMIGA.FFI:DEFCFUN encoding
 * ================================================================ */

/* DEFCFUN installs an FFI stub; ffi::%ffi-stub-info exposes its fields.
 * Register nibbles: a0 -> 8, d0 -> 0, d1 -> 1, low nibble = first arg. */
TEST(defcfun_installs_stub_with_regspec_and_kind)
{
    ASSERT_STR_EQ(eval_print("(require \"amiga/ffi\")"), "T");
    ASSERT_STR_EQ(eval_print("(defvar cl-user::*tb* nil)"), "*TB*");
    ASSERT_STR_EQ(eval_print(
        "(amiga.ffi:defcfun cl-user::f1 cl-user::*tb* -30 (:a0 x :d0 y) :void t)"), "F1");
    ASSERT_STR_EQ(eval_print("(ffi::%ffi-stub-info #'cl-user::f1)"),
        "(:KIND :LIBCALL :NAME F1 :BASE *TB* :LVO -30 :REGSPEC 8 :RESULT :VOID :NPARAMS 2)");
    /* :result :pointer, one a0 arg */
    eval_print("(amiga.ffi:defcfun cl-user::f2 cl-user::*tb* -36 (:a0 x) :result :pointer)");
    ASSERT(contains(eval_print("(ffi::%ffi-stub-info 'cl-user::f2)"),
                    ":LVO -36 :REGSPEC 8 :RESULT :POINTER :NPARAMS 1"));
    /* :result :signed, d1 -> nibble 1 */
    eval_print("(amiga.ffi:defcfun cl-user::f3 cl-user::*tb* -42 (:d1 x) :result :signed)");
    ASSERT(contains(eval_print("(ffi::%ffi-stub-info 'cl-user::f3)"),
                    ":LVO -42 :REGSPEC 1 :RESULT :SIGNED :NPARAMS 1"));
    /* :result :unsigned (default); (:d0 a :d1 b :a0 c) -> 0 | 1<<4 | 8<<8 = 2064 */
    eval_print("(amiga.ffi:defcfun cl-user::f4 cl-user::*tb* -48 (:d0 a :d1 b :a0 c))");
    ASSERT(contains(eval_print("(ffi::%ffi-stub-info 'cl-user::f4)"),
                    ":LVO -48 :REGSPEC 2064 :RESULT :UNSIGNED :NPARAMS 3"));
    /* the stub is a function for every CL predicate */
    ASSERT_STR_EQ(eval_print(
        "(list (fboundp 'cl-user::f1) (functionp #'cl-user::f1) "
        "(compiled-function-p #'cl-user::f1) (typep #'cl-user::f1 'function) "
        "(type-of #'cl-user::f1) (class-name (class-of #'cl-user::f1)))"),
        "(T T T T FUNCTION FUNCTION)");
    ASSERT_STR_EQ(eval_print("(ext:function-arglist #'cl-user::f4)"), "(#:ARG0 #:ARG1 #:ARG2)");
    ASSERT_STR_EQ(eval_print("(prin1-to-string #'cl-user::f4)"), "\"#<FFI-STUB F4 LVO -48 (3 args)>\"");
}

/* The sub-32-bit result kinds are carried by the stub (boxed in C by the
 * trampoline, see box_* above) — no Lisp post-processor functions remain. */
TEST(defcfun_sub_word_result_kinds_in_stub)
{
    eval_print("(amiga.ffi:defcfun cl-user::f5 cl-user::*tb* -54 (:d0 x) :result :bool)");
    ASSERT(contains(eval_print("(ffi::%ffi-stub-info 'cl-user::f5)"), ":RESULT :BOOL"));
    eval_print("(amiga.ffi:defcfun cl-user::f5b cl-user::*tb* -54 (:d0 x) :result :i16)");
    ASSERT(contains(eval_print("(ffi::%ffi-stub-info 'cl-user::f5b)"), ":RESULT :I16"));
    eval_print("(amiga.ffi:defcfun cl-user::f5c cl-user::*tb* -54 (:d0 x) :result :u8)");
    ASSERT(contains(eval_print("(ffi::%ffi-stub-info 'cl-user::f5c)"), ":RESULT :U8"));
    ASSERT_STR_EQ(eval_print("(fboundp 'amiga.ffi::%result-bool)"), "NIL");
}

TEST(defcfun_more_than_seven_registers_uses_call_library)
{
    const char *r = eval_print(
        "(macroexpand-1 '(amiga.ffi:defcfun cl-user::f6 cl-user::*tb* -60 "
        "(:a0 a :d0 b :d1 c :a1 d :d2 e :d3 f :d4 g :d5 h) :result :pointer))");
    ASSERT(contains(r, "(CALL-LIBRARY *TB* -60 (LIST :A0 A :D0 B :D1 C :A1 D :D2 E :D3 F :D4 G :D5 H) 2)"));
    /* a real DEFUN, not a stub */
    ASSERT(contains(r, "(DEFUN F6"));
    eval_print("(amiga.ffi:defcfun cl-user::f6 cl-user::*tb* -60 "
               "(:a0 a :d0 b :d1 c :a1 d :d2 e :d3 f :d4 g :d5 h) :result :i8)");
    ASSERT_STR_EQ(eval_print("(ffi::%ffi-stub-info 'cl-user::f6)"), "NIL");
    ASSERT_STR_EQ(eval_print("(length (ext:function-arglist #'cl-user::f6))"), "8");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (function-lambda-expression #'cl-user::f6))"),
                  "(NIL T F6)");
}

TEST(defcfun_rejects_a5)
{
    const char *r = eval_print(
        "(macroexpand-1 '(amiga.ffi:defcfun cl-user::f7 cl-user::*tb* -66 (:a5 x)))");
    ASSERT(contains(r, "ERROR:"));
    ASSERT(contains(r, ":A5"));
}

TEST(defcfun_compiles_and_reports_nil_base_on_host)
{
    ASSERT_STR_EQ(eval_print("(amiga.ffi:defcfun cl-user::f8 cl-user::*tb* -30 (:a0 x) :result :pointer)"), "F8");
    {
        /* direct call: compile_call inlines the stub as OP_AMIGA_CALL */
        const char *r = eval_print("(cl-user::f8 1)");
        ASSERT(contains(r, "ERROR:"));
        ASSERT(contains(r, "*TB*"));
        ASSERT(contains(r, "not open"));
        /* indirect call: the stub's own dispatch, same base check */
        r = eval_print("(funcall #'cl-user::f8 1)");
        ASSERT(contains(r, "not open"));
        r = eval_print("(apply 'cl-user::f8 '(1))");
        ASSERT(contains(r, "not open"));
    }
    /* wrong arity at a direct call site is declined by the inliner and
     * reported by the stub at run time; funcall/apply likewise */
    ASSERT(contains(eval_print("(funcall (compile nil '(lambda () (cl-user::f8 1 2))))"),
                    "Too many arguments to F8: expected 1, got 2"));
    ASSERT(contains(eval_print("(funcall #'cl-user::f8)"),
                    "Too few arguments to F8: expected 1, got 0"));
}

TEST(defcfun_direct_call_inlines_amiga_call)
{
    /* the caller's bytecode holds OP_AMIGA_CALL with the stub's fields;
     * a lexically shadowed or notinline name is left as a plain call */
    const char *r = eval_print(
        "(with-output-to-string (*standard-output*) "
        "  (disassemble (compile nil '(lambda (p) (cl-user::f8 p)))))");
    ASSERT(contains(r, "AMIGA_CALL"));
    ASSERT(contains(r, "off -30 regspec 0x20000008 nargs 1"));
    r = eval_print(
        "(with-output-to-string (*standard-output*) "
        "  (disassemble (compile nil '(lambda (p) (declare (notinline cl-user::f8)) (cl-user::f8 p)))))");
    ASSERT(!contains(r, "AMIGA_CALL"));
    ASSERT(contains(r, "F8"));
    r = eval_print(
        "(with-output-to-string (*standard-output*) "
        "  (disassemble (compile nil '(lambda (p) (flet ((cl-user::f8 (x) x)) (cl-user::f8 p))))))");
    ASSERT(!contains(r, "AMIGA_CALL"));
}

TEST(defcfun_docstrings_switch)
{
    eval_print("(amiga.ffi:defcfun cl-user::f9 cl-user::*tb* -30 (:a0 x) :doc \"APTR F9(APTR x) (A0)\")");
    ASSERT_STR_EQ(eval_print("(documentation 'cl-user::f9 'function)"), "\"APTR F9(APTR x) (A0)\"");
    eval_print("(setf amiga.ffi:*defcfun-docstrings* nil)");
    eval_print("(amiga.ffi:defcfun cl-user::f10 cl-user::*tb* -30 (:a0 x) :doc \"APTR F10(APTR x) (A0)\")");
    ASSERT_STR_EQ(eval_print("(documentation 'cl-user::f10 'function)"), "NIL");
    ASSERT_STR_EQ(eval_print("(fboundp 'cl-user::f10)"), "T");
    eval_print("(setf amiga.ffi:*defcfun-docstrings* t)");
}

TEST(library_version_reads_lib_version_field)
{
    /* struct Library: lib_Version is the UWORD at offset 20 */
    ASSERT_STR_EQ(eval_print(
        "(let ((m (ffi:alloc-foreign 34))) "
        "  (ffi:poke-u16 m 47 20) "
        "  (prog1 (amiga.ffi:library-version m) (ffi:free-foreign m)))"), "47");
}

/* ================================================================
 * FFI:DEFCSTRUCT extensions
 * ================================================================ */

TEST(defcstruct_fptr_struct_array_size)
{
    eval_print("(require \"ffi\")");   /* already provided via amiga/ffi */
    ASSERT_STR_EQ(eval_print(
        "(ffi:defcstruct (cl-user::tst :size 32) "
        "  (cl-user::x :i16 0) (cl-user::y :u16 2) (cl-user::p :fptr 4) "
        "  (cl-user::sub (:struct 8) 8) (cl-user::arr (:array :u8 4) 16) (cl-user::f :single 20))"),
        "TST");
    ASSERT_STR_EQ(eval_print("cl-user::*tst-size*"), "32");   /* explicit, not 24 */
    ASSERT_STR_EQ(eval_print(
        "(let ((m (ffi:alloc-foreign 32))) "
        "  (setf (cl-user::tst-x m) -5 (cl-user::tst-y m) 65535 (cl-user::tst-p m) nil) "
        "  (setf (cl-user::tst-arr m 3) 77) "
        "  (prog1 (list (cl-user::tst-x m) (cl-user::tst-y m) (cl-user::tst-p m) "
        "               (cl-user::tst-arr m 3) (ffi:peek-u8 m 19) "
        "               (ffi:foreign-pointer-p (cl-user::tst-sub m)) "
        "               (- (ffi:foreign-pointer-address (cl-user::tst-sub m)) "
        "                  (ffi:foreign-pointer-address m))) "
        "    (ffi:free-foreign m)))"),
        "(-5 65535 NIL 77 77 T 8)");
    /* :fptr setter takes a foreign pointer, an integer or NIL; getter gives NIL for NULL */
    ASSERT_STR_EQ(eval_print(
        "(let ((m (ffi:alloc-foreign 32))) "
        "  (setf (cl-user::tst-p m) 1234) "
        "  (prog1 (list (ffi:foreign-pointer-address (cl-user::tst-p m)) "
        "               (progn (setf (cl-user::tst-p m) (ffi:make-foreign-pointer 99)) "
        "                      (ffi:foreign-pointer-address (cl-user::tst-p m))) "
        "               (progn (setf (cl-user::tst-p m) nil) (cl-user::tst-p m))) "
        "    (ffi:free-foreign m)))"),
        "(1234 99 NIL)");
}

TEST(defcstruct_derived_size_and_legacy_pointer)
{
    ASSERT_STR_EQ(eval_print(
        "(ffi:defcstruct cl-user::leg (cl-user::a :u8 0) (cl-user::ptr :pointer 4))"), "LEG");
    ASSERT_STR_EQ(eval_print("cl-user::*leg-size*"), "8");
    /* :pointer keeps returning the raw integer address */
    ASSERT_STR_EQ(eval_print(
        "(let ((m (ffi:alloc-foreign 8))) "
        "  (setf (cl-user::leg-ptr m) 4660) "
        "  (prog1 (cl-user::leg-ptr m) (ffi:free-foreign m)))"), "4660");
}

TEST(defcstruct_rejects_unknown_type)
{
    ASSERT(contains(eval_print("(ffi:defcstruct cl-user::bad (cl-user::a :u128 0))"), "ERROR:"));
    ASSERT(contains(eval_print("(ffi:defcstruct cl-user::bad2 (cl-user::a (:array :u8 0) 0))"), "ERROR:"));
}

/* The accessors are field stubs: reader + %SET- writer linked by DEFSETF,
 * one installer call per struct; array accessors are bounds-checked. */
TEST(defcstruct_accessors_are_field_stubs)
{
    ASSERT_STR_EQ(eval_print("(ffi::%ffi-stub-info #'cl-user::tst-x)"),
                  "(:KIND :PEEK :NAME TST-X :CTYPE :I16 :OFFSET 0)");
    ASSERT_STR_EQ(eval_print("(ffi::%ffi-stub-info 'cl-user::%set-tst-x)"),
                  "(:KIND :POKE :NAME %SET-TST-X :CTYPE :I16 :OFFSET 0)");
    ASSERT_STR_EQ(eval_print("(ffi::%ffi-stub-info #'cl-user::tst-arr)"),
                  "(:KIND :PEEK-IDX :NAME TST-ARR :CTYPE :U8 :OFFSET 16 :ELT-SIZE 1 :COUNT 4)");
    ASSERT_STR_EQ(eval_print("(ffi::%ffi-stub-info #'cl-user::tst-sub)"),
                  "(:KIND :FIELD-PTR :NAME TST-SUB :OFFSET 8)");
    /* embedded struct: reader only */
    ASSERT_STR_EQ(eval_print("(fboundp 'cl-user::%set-tst-sub)"), "NIL");
    /* defsetf linkage is what (setf (tst-x m) v) compiles through */
    ASSERT_STR_EQ(eval_print("(amilisp::%get-defsetf-setter 'cl-user::tst-x)"), "%SET-TST-X");
    ASSERT_STR_EQ(eval_print("(prin1-to-string #'cl-user::tst-arr)"),
                  "\"#<FFI-STUB TST-ARR PEEK-IDX :U8 @16 [4 x 1]>\"");
    ASSERT_STR_EQ(eval_print("(ext:function-arglist #'cl-user::%set-tst-arr)"), "(#:ARG0 #:ARG1 #:ARG2)");
    /* the macro expands to ONE installer form per struct (FASL footprint) */
    ASSERT(contains(eval_print("(macroexpand-1 '(ffi:defcstruct cl-user::two (cl-user::a :u8 0) (cl-user::b :u8 1)))"),
                    "(FFI::%DEFINE-CSTRUCT-ACCESSORS (QUOTE ((TWO-A :U8 0) (TWO-B :U8 1))))"));
}

TEST(defcstruct_array_accessor_bounds_and_arity)
{
    ASSERT(contains(eval_print(
        "(let ((m (ffi:alloc-foreign 32))) (unwind-protect (cl-user::tst-arr m 4) (ffi:free-foreign m)))"),
        "TST-ARR: index 4 is out of range for a 4-element array field"));
    ASSERT(contains(eval_print(
        "(let ((m (ffi:alloc-foreign 32))) (unwind-protect (setf (cl-user::tst-arr m -1) 0) (ffi:free-foreign m)))"),
        "index must be a non-negative fixnum"));
    ASSERT(contains(eval_print(
        "(let ((m (ffi:alloc-foreign 32))) (unwind-protect (cl-user::tst-x m 1) (ffi:free-foreign m)))"),
        "Too many arguments to TST-X: expected 1, got 2"));
    ASSERT(contains(eval_print("(cl-user::tst-x 42)"), "must be a foreign pointer"));
    ASSERT(contains(eval_print(
        "(let ((m (ffi:alloc-foreign 32))) (unwind-protect (setf (cl-user::tst-f m) \"x\") (ffi:free-foreign m)))"),
        "must be a real number"));
}

int main(void)
{
    setup();

    printf("--- test_amiga_ffi ---\n");

    RUN(box_unsigned_small_is_fixnum);
    RUN(box_unsigned_max_fixnum_boundary);
    RUN(box_unsigned_large_is_positive_bignum);
    RUN(box_unsigned_just_above_fixnum);
    RUN(box_void_is_nil);
    RUN(box_pointer_null_is_nil);
    RUN(box_pointer_nonnull_is_foreign_pointer);
    RUN(box_pointer_high_address);
    RUN(box_signed_minus_one);
    RUN(box_signed_positive_small);
    RUN(box_signed_fixnum_min_boundary);
    RUN(box_signed_large_negative_is_negative_bignum);
    RUN(box_signed_int32_min);
    RUN(box_signed_large_positive_is_positive_bignum);
    RUN(res_kind_macro_decodes_bits_28_31);
    RUN(box_bool_tests_low_word_only);
    RUN(box_u16_i16_u8_i8_mask_and_sign_extend);

    RUN(arg_to_u32_integers_pointers_and_booleans);
    RUN(arg_to_u32_rejects_other_objects_naming_argument_and_register);

    RUN(amiga_package_exists_on_host);
    RUN(amiga_stub_signals_platform_error);
    RUN(amiga_call_library_arity_accepts_result_kind);

    RUN(defcfun_installs_stub_with_regspec_and_kind);
    RUN(host_dispatch_checks_register_args_before_platform_error);
    RUN(defcfun_sub_word_result_kinds_in_stub);
    RUN(defcfun_more_than_seven_registers_uses_call_library);
    RUN(defcfun_rejects_a5);
    RUN(defcfun_compiles_and_reports_nil_base_on_host);
    RUN(defcfun_direct_call_inlines_amiga_call);
    RUN(defcfun_docstrings_switch);
    RUN(library_version_reads_lib_version_field);

    RUN(defcstruct_fptr_struct_array_size);
    RUN(defcstruct_derived_size_and_legacy_pointer);
    RUN(defcstruct_rejects_unknown_type);
    RUN(defcstruct_accessors_are_field_stubs);
    RUN(defcstruct_array_accessor_bounds_and_arity);

    REPORT();
    teardown();
    return test_fail > 0 ? 1 : 0;
}
