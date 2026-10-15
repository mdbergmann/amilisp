/*
 * test_ffi_stub.c — specification of the FFI stub object (TYPE_FFI_STUB,
 * types.h CL_FfiStub): the binding descriptor AMIGA.FFI:DEFCFUN and
 * FFI:DEFCSTRUCT install in a function cell instead of a wrapper closure
 * (specs/raw-bindings-footprint.md, Phase 1).
 *
 * Pinned here, at the C level:
 *   - construction (cl_make_ffi_stub) and the kind/arity table
 *   - it is a function for every predicate: FUNCTIONP, COMPILED-FUNCTION-P,
 *     TYPEP FUNCTION, TYPE-OF, CLASS-OF, CL_FUNCTION_OBJ_P
 *   - calling through cl_vm_apply, OP_CALL (direct + FUNCALL), OP_APPLY,
 *     MAPCAR; the arity error; every field kind's read/write conversions
 *   - survival of a compacting GC with name/aux forwarded
 *   - FASL round trip (FASL_TAG_FFI_STUB) and rejection of a corrupt tag
 *   - the printer, DESCRIBE, DISASSEMBLE, EXT:FUNCTION-ARGLIST,
 *     FUNCTION-LAMBDA-EXPRESSION, ffi::%ffi-stub-info
 *   - the constructors' argument validation
 *
 * The DEFCFUN / DEFCSTRUCT macro surface is tests/test_amiga_ffi.c; calls
 * that reach the OS are tests/amiga/test-raw-bindings.lisp.
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
#include "core/fasl.h"
#include "core/repl.h"
#include "core/thread.h"
#include "platform/platform.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

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

static CL_Obj user_sym(const char *name)
{
    return cl_intern_in(name, (uint32_t)strlen(name), cl_package_cl_user);
}

/* ================================================================
 * Object level
 * ================================================================ */

TEST(make_stub_sets_fields_and_is_a_function)
{
    CL_Obj name = user_sym("STUB-T1");
    CL_Obj base = user_sym("*STUB-BASE*");
    CL_Obj stub;
    CL_FfiStub *fs;
    CL_GC_PROTECT(name);
    CL_GC_PROTECT(base);
    stub = cl_make_ffi_stub(CL_STUB_LIBCALL, name, base,
                            CL_AMIGA_MAKE_REGSPEC(0x109u, CL_AMIGA_RES_I16),
                            -204, 3);
    CL_GC_UNPROTECT(2);
    ASSERT(CL_FFI_STUB_P(stub));
    ASSERT(CL_FUNCTION_OBJ_P(stub));
    ASSERT(!CL_FUNCTION_P(stub));
    ASSERT(!CL_CLOSURE_P(stub));
    /* 20 bytes of payload; the allocator rounds the block to CL_ALIGN */
    ASSERT_EQ_INT((int)sizeof(CL_FfiStub), 20);
    ASSERT((int)CL_HDR_SIZE(CL_OBJ_TO_PTR(stub)) >= 20 &&
           (int)CL_HDR_SIZE(CL_OBJ_TO_PTR(stub)) <= 24);
    fs = (CL_FfiStub *)CL_OBJ_TO_PTR(stub);
    ASSERT(fs->name == name);
    ASSERT(fs->aux == base);
    ASSERT_EQ_INT((int)fs->a, (int)0x60000109u);
    ASSERT_EQ_INT((int)fs->b, -204);
    ASSERT_EQ_INT((int)fs->kind, CL_STUB_LIBCALL);
    ASSERT_EQ_INT((int)fs->ctype, 3);
    ASSERT_STR_EQ(cl_type_name(stub), "FUNCTION");
    ASSERT_EQ_INT(cl_ffi_stub_arity(stub), 3);
}

TEST(arity_per_kind)
{
    CL_Obj name = user_sym("STUB-T2");
    CL_Obj s;
    s = cl_make_ffi_stub(CL_STUB_PEEK, name, CL_NIL, 4, 0, CL_STUB_CT_U8);
    ASSERT_EQ_INT(cl_ffi_stub_arity(s), 1);
    s = cl_make_ffi_stub(CL_STUB_POKE, name, CL_NIL, 4, 0, CL_STUB_CT_U8);
    ASSERT_EQ_INT(cl_ffi_stub_arity(s), 2);
    s = cl_make_ffi_stub(CL_STUB_PEEK_IDX, name, CL_NIL, 4, 2, CL_STUB_CT_U16);
    ASSERT_EQ_INT(cl_ffi_stub_arity(s), 2);
    s = cl_make_ffi_stub(CL_STUB_POKE_IDX, name, CL_NIL, 4, 2, CL_STUB_CT_U16);
    ASSERT_EQ_INT(cl_ffi_stub_arity(s), 3);
    s = cl_make_ffi_stub(CL_STUB_FIELD_PTR, name, CL_NIL, 4, 0, 0);
    ASSERT_EQ_INT(cl_ffi_stub_arity(s), 1);
    s = cl_make_ffi_stub(CL_STUB_LIBCALL, name, name, 0, -30, 0);
    ASSERT_EQ_INT(cl_ffi_stub_arity(s), 0);
    s = cl_make_ffi_stub(CL_STUB_LIBCALL, name, name, 0, -30, 7);
    ASSERT_EQ_INT(cl_ffi_stub_arity(s), 7);
}

/* A compacting collection moves the stub and its symbols; a rooted
 * reference must come back with name/aux forwarded, not stale. */
TEST(stub_survives_compaction_with_children_forwarded)
{
    CL_Obj name = user_sym("STUB-GC-NAME");
    CL_Obj base = user_sym("*STUB-GC-BASE*");
    CL_Obj stub;
    int i;
    CL_GC_PROTECT(name);
    CL_GC_PROTECT(base);
    stub = cl_make_ffi_stub(CL_STUB_LIBCALL, name, base, 0x20000008u, -36, 1);
    CL_GC_PROTECT(stub);
    /* churn: garbage before and after, several full collections */
    for (i = 0; i < 3; i++) {
        int j;
        for (j = 0; j < 2000; j++)
            (void)cl_make_string("garbage-between-collections", 27);
        cl_gc();
        ASSERT(CL_FFI_STUB_P(stub));
        {
            CL_FfiStub *fs = (CL_FfiStub *)CL_OBJ_TO_PTR(stub);
            ASSERT(fs->name == name);
            ASSERT(fs->aux == base);
            ASSERT(CL_SYMBOL_P(fs->name));
            ASSERT_STR_EQ(cl_symbol_name(fs->name), "STUB-GC-NAME");
            ASSERT_STR_EQ(cl_symbol_name(fs->aux), "*STUB-GC-BASE*");
            ASSERT_EQ_INT((int)fs->a, (int)0x20000008u);
            ASSERT_EQ_INT((int)fs->b, -36);
        }
    }
    CL_GC_UNPROTECT(3);
}

/* ================================================================
 * Calling
 * ================================================================ */

/* Install a C-made PEEK stub in a symbol and call it every way the VM can. */
TEST(peek_stub_called_via_apply_opcall_funcall_apply_mapcar)
{
    CL_Obj name = user_sym("STUB-PK");
    CL_Obj stub;
    CL_GC_PROTECT(name);
    stub = cl_make_ffi_stub(CL_STUB_PEEK, name, CL_NIL, 2, 0, CL_STUB_CT_U16);
    ((CL_Symbol *)CL_OBJ_TO_PTR(name))->function = stub;
    CL_GC_UNPROTECT(1);

    ASSERT_STR_EQ(eval_print("(defvar cl-user::*stub-mem* (ffi:alloc-foreign 16))"), "*STUB-MEM*");
    ASSERT_STR_EQ(eval_print("(ffi:poke-u16 cl-user::*stub-mem* 513 2)"), "513");
    /* OP_CALL direct (compiled call to a stub-bound symbol) */
    ASSERT_STR_EQ(eval_print("(cl-user::stub-pk cl-user::*stub-mem*)"), "513");
    /* OP_CALL via FUNCALL of the object, and of the symbol */
    ASSERT_STR_EQ(eval_print("(funcall #'cl-user::stub-pk cl-user::*stub-mem*)"), "513");
    ASSERT_STR_EQ(eval_print("(funcall 'cl-user::stub-pk cl-user::*stub-mem*)"), "513");
    /* OP_APPLY */
    ASSERT_STR_EQ(eval_print("(apply #'cl-user::stub-pk (list cl-user::*stub-mem*))"), "513");
    /* cl_vm_apply from a builtin (MAPCAR) */
    ASSERT_STR_EQ(eval_print("(mapcar #'cl-user::stub-pk (list cl-user::*stub-mem* cl-user::*stub-mem*))"), "(513 513)");
    /* multiple values: exactly one */
    ASSERT_STR_EQ(eval_print("(multiple-value-list (cl-user::stub-pk cl-user::*stub-mem*))"), "(513)");
    /* from C too */
    {
        CL_Obj args[1];
        CL_Obj r;
        args[0] = cl_symbol_value(user_sym("*STUB-MEM*"));
        r = cl_vm_apply(stub, args, 1);
        ASSERT(CL_FIXNUM_P(r));
        ASSERT_EQ_INT(CL_FIXNUM_VAL(r), 513);
    }
}

TEST(stub_arity_errors_name_the_binding)
{
    const char *r;
    r = eval_print("(cl-user::stub-pk)");
    ASSERT(contains(r, "ERROR:Too few arguments to STUB-PK: expected 1, got 0"));
    r = eval_print("(funcall #'cl-user::stub-pk 1 2)");
    ASSERT(contains(r, "Too many arguments to STUB-PK: expected 1, got 2"));
    r = eval_print("(apply #'cl-user::stub-pk '(1 2 3))");
    ASSERT(contains(r, "Too many arguments to STUB-PK: expected 1, got 3"));
    /* from C */
    {
        CL_Obj stub = ((CL_Symbol *)CL_OBJ_TO_PTR(user_sym("STUB-PK")))->function;
        CL_Obj args[2];
        int err;
        args[0] = CL_MAKE_FIXNUM(1);
        args[1] = CL_MAKE_FIXNUM(2);
        CL_CATCH(err);
        if (err == CL_ERR_NONE) {
            cl_vm_apply(stub, args, 2);
            CL_UNCATCH();
            ASSERT(0 && "expected an arity error");
        } else {
            CL_UNCATCH();
            ASSERT_EQ_INT(err, CL_ERR_ARGS);
            ASSERT(contains(cl_error_msg, "expected 1, got 2"));
        }
    }
}

/* Every field kind: round trip through a foreign buffer.  The reader and
 * writer conversions must match the PEEK-x / POKE-x builtins the old
 * DEFCSTRUCT expansion called (signed extension, :fptr NIL for NULL,
 * :pointer as a raw integer, floats, bignum for a u32 above the fixnum
 * range). */
TEST(field_kinds_round_trip)
{
    eval_print("(require \"ffi\")");
    ASSERT_STR_EQ(eval_print(
        "(ffi:defcstruct (cl-user::fk :size 48) "
        "  (cl-user::u8 :u8 0) (cl-user::i8 :i8 1) (cl-user::u16 :u16 2) (cl-user::i16 :i16 4) "
        "  (cl-user::u32 :u32 8) (cl-user::i32 :i32 12) (cl-user::ptr :pointer 16) "
        "  (cl-user::fp :fptr 20) (cl-user::sg :single 24) (cl-user::db :double 32) "
        "  (cl-user::sub (:struct 4) 40) (cl-user::arr (:array :i16 2) 44))"), "FK");
    ASSERT_STR_EQ(eval_print("cl-user::*fk-size*"), "48");
    ASSERT_STR_EQ(eval_print(
        "(let ((m (ffi:alloc-foreign 48))) "
        "  (setf (cl-user::fk-u8 m) 255 (cl-user::fk-i8 m) -1 (cl-user::fk-u16 m) 65535 "
        "        (cl-user::fk-i16 m) -2 (cl-user::fk-u32 m) #xFFFFFFFF (cl-user::fk-i32 m) -3 "
        "        (cl-user::fk-ptr m) #x1234 (cl-user::fk-fp m) nil "
        "        (cl-user::fk-sg m) 1.5 (cl-user::fk-db m) 2.25d0 "
        "        (cl-user::fk-arr m 1) -7) "
        "  (prog1 (list (cl-user::fk-u8 m) (cl-user::fk-i8 m) (cl-user::fk-u16 m) (cl-user::fk-i16 m) "
        "               (cl-user::fk-u32 m) (cl-user::fk-i32 m) (cl-user::fk-ptr m) (cl-user::fk-fp m) "
        "               (cl-user::fk-sg m) (cl-user::fk-db m) (cl-user::fk-arr m 1) "
        "               (- (ffi:foreign-pointer-address (cl-user::fk-sub m)) (ffi:foreign-pointer-address m))) "
        "    (ffi:free-foreign m)))"),
        "(255 -1 65535 -2 4294967295 -3 4660 NIL 1.5 2.25d0 -7 40)");
    /* :fptr writer accepts a pointer / an integer / NIL; reader gives NIL for NULL */
    ASSERT_STR_EQ(eval_print(
        "(let ((m (ffi:alloc-foreign 48))) "
        "  (setf (cl-user::fk-fp m) 99) "
        "  (prog1 (list (ffi:foreign-pointer-address (cl-user::fk-fp m)) "
        "               (progn (setf (cl-user::fk-fp m) (ffi:make-foreign-pointer 77)) "
        "                      (ffi:foreign-pointer-address (cl-user::fk-fp m))) "
        "               (progn (setf (cl-user::fk-fp m) nil) (cl-user::fk-fp m))) "
        "    (ffi:free-foreign m)))"),
        "(99 77 NIL)");
    /* writer type errors name the accessor */
    ASSERT(contains(eval_print(
        "(let ((m (ffi:alloc-foreign 48))) (unwind-protect (setf (cl-user::fk-u8 m) \"x\") (ffi:free-foreign m)))"),
        "%SET-FK-U8: value must be an integer"));
    ASSERT(contains(eval_print(
        "(let ((m (ffi:alloc-foreign 48))) (unwind-protect (setf (cl-user::fk-fp m) 1.5) (ffi:free-foreign m)))"),
        "%SET-FK-FP: pointer field value must be"));
    /* a NULL foreign pointer is refused, not dereferenced */
    ASSERT(contains(eval_print("(cl-user::fk-u8 (ffi:make-foreign-pointer 0))"),
                    "invalid/null foreign pointer"));
}

/* A DEFCSTRUCT and a SETF of one of its accessors compiled in the SAME
 * form (a PROGN at the REPL, a test body, a DEFUN body): the updater must
 * be known while the SETF is compiled, before the installer has run —
 * the compile-time DEFSETF registration the per-field forms used to
 * provide, now done by compile_call for the bulk installer.  Regression:
 * the Amiga suite's ffi-defcstruct check failed with "Undefined function:
 * %SETF-...::TEST-POINT-X" without it. */
TEST(defcstruct_setf_compiled_in_same_form)
{
    eval_print("(require \"ffi\")");
    ASSERT_STR_EQ(eval_print(
        "(progn (ffi:defcstruct cl-user::sf1 (cl-user::a :u16 0) (cl-user::b :u16 2)) "
        "  (let ((m (ffi:alloc-foreign 4))) "
        "    (setf (cl-user::sf1-a m) 100 (cl-user::sf1-b m) 200) "
        "    (prog1 (list (cl-user::sf1-a m) (cl-user::sf1-b m)) (ffi:free-foreign m))))"),
        "(100 200)");
    /* inside a function body: compiled long before it runs */
    ASSERT_STR_EQ(eval_print(
        "(defun cl-user::sf2-fn () "
        "  (ffi:defcstruct cl-user::sf2 (cl-user::x :i8 0) (cl-user::arr (:array :u8 2) 1)) "
        "  (let ((m (ffi:alloc-foreign 4))) "
        "    (setf (cl-user::sf2-x m) -3 (cl-user::sf2-arr m 1) 9) "
        "    (prog1 (list (cl-user::sf2-x m) (cl-user::sf2-arr m 1)) (ffi:free-foreign m))))"),
        "SF2-FN");
    ASSERT_STR_EQ(eval_print("(fboundp 'cl-user::sf2-x)"), "NIL");   /* not installed yet ... */
    ASSERT_STR_EQ(eval_print("(cl-user::sf2-fn)"), "(-3 9)");       /* ... but SETF resolved */
    ASSERT_STR_EQ(eval_print("(fboundp 'cl-user::%set-sf2-arr)"), "T");
}

/* ================================================================
 * FASL
 * ================================================================ */

TEST(fasl_tag_and_version_pinned)
{
    ASSERT_EQ_INT(FASL_TAG_FFI_STUB, 0x21);
    ASSERT(CL_FASL_VERSION >= 30);
}

TEST(fasl_round_trip_preserves_every_field)
{
    static uint8_t buf[512];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_Obj name = user_sym("STUB-FASL");
    CL_Obj base = user_sym("*STUB-FASL-BASE*");
    CL_Obj stub, back;
    CL_FfiStub *fs;
    CL_GC_PROTECT(name);
    CL_GC_PROTECT(base);
    stub = cl_make_ffi_stub(CL_STUB_LIBCALL, name, base,
                            CL_AMIGA_MAKE_REGSPEC(0x0089u, CL_AMIGA_RES_U8), -390, 2);
    CL_GC_PROTECT(stub);

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, stub);
    ASSERT_EQ_INT(w.error, FASL_OK);
    ASSERT_EQ_INT(buf[0], FASL_TAG_FFI_STUB);
    ASSERT_EQ_INT(buf[1], CL_STUB_LIBCALL);   /* kind */
    ASSERT_EQ_INT(buf[2], 2);                 /* ctype = nparams */
    /* b = -390 big-endian i16, a big-endian u32 */
    ASSERT_EQ_INT(((buf[3] << 8) | buf[4]), (int)(uint16_t)-390);
    ASSERT_EQ_INT(buf[5], 0x70);
    cl_fasl_writer_release(&w);

    cl_fasl_reader_init(&r, buf, w.pos);
    back = cl_fasl_deserialize_obj(&r);
    ASSERT_EQ_INT(r.error, FASL_OK);
    ASSERT(CL_FFI_STUB_P(back));
    ASSERT(back != stub);   /* a fresh descriptor */
    fs = (CL_FfiStub *)CL_OBJ_TO_PTR(back);
    ASSERT(fs->name == name);   /* symbols resolve to the same objects */
    ASSERT(fs->aux == base);
    ASSERT_EQ_INT((int)fs->a, (int)0x70000089u);
    ASSERT_EQ_INT((int)fs->b, -390);
    ASSERT_EQ_INT((int)fs->kind, CL_STUB_LIBCALL);
    ASSERT_EQ_INT((int)fs->ctype, 2);

    /* a field stub: aux NIL, offset|count packing intact */
    stub = cl_make_ffi_stub(CL_STUB_POKE_IDX, name, CL_NIL, 12u | (4u << 16), 2, CL_STUB_CT_I16);
    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, stub);
    ASSERT_EQ_INT(w.error, FASL_OK);
    cl_fasl_writer_release(&w);
    cl_fasl_reader_init(&r, buf, w.pos);
    back = cl_fasl_deserialize_obj(&r);
    ASSERT_EQ_INT(r.error, FASL_OK);
    fs = (CL_FfiStub *)CL_OBJ_TO_PTR(back);
    ASSERT_EQ_INT((int)fs->kind, CL_STUB_POKE_IDX);
    ASSERT_EQ_INT((int)fs->ctype, CL_STUB_CT_I16);
    ASSERT_EQ_INT((int)fs->a, (int)(12u | (4u << 16)));
    ASSERT_EQ_INT((int)fs->b, 2);
    ASSERT(CL_NULL_P(fs->aux));
    CL_GC_UNPROTECT(3);
}

TEST(fasl_rejects_corrupt_stub_descriptor)
{
    static uint8_t buf[64];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_Obj name = user_sym("STUB-BAD");
    CL_Obj stub = cl_make_ffi_stub(CL_STUB_PEEK, name, CL_NIL, 0, 0, CL_STUB_CT_U8);
    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, stub);
    ASSERT_EQ_INT(w.error, FASL_OK);
    cl_fasl_writer_release(&w);
    buf[1] = CL_STUB_KIND_MAX + 1;            /* impossible kind */
    cl_fasl_reader_init(&r, buf, w.pos);
    (void)cl_fasl_deserialize_obj(&r);
    ASSERT_EQ_INT(r.error, FASL_ERR_BAD_TAG);
    buf[1] = CL_STUB_PEEK;
    buf[2] = CL_STUB_CT_MAX + 1;              /* impossible field type */
    cl_fasl_reader_init(&r, buf, w.pos);
    (void)cl_fasl_deserialize_obj(&r);
    ASSERT_EQ_INT(r.error, FASL_ERR_BAD_TAG);
}

/* compile-file -> load: stubs installed by the loaded FASL are callable
 * and carry their fields; the inline hook fires for the loaded callers. */
TEST(compile_file_load_round_trip)
{
    const char *dir = getenv("TMPDIR");
    char src[512], fasl[512], form[1400];
    char *p;
    FILE *f;
    if (!dir || !*dir) dir = "/tmp";
    snprintf(src, sizeof(src), "%s/amilisp_stub_rt_%d.lisp", dir, (int)getpid());
    snprintf(fasl, sizeof(fasl), "%s/amilisp_stub_rt_%d.fasl", dir, (int)getpid());
    /* The paths go into Lisp string literals below: a Windows TMPDIR spelled
     * with backslashes ("C:\a\_temp\...") would have them eaten by the reader
     * as escapes ("\t" -> tab).  Windows takes '/' everywhere, so spell the
     * paths that way for the C runtime and the reader alike. */
    for (p = src; *p; p++) if (*p == '\\') *p = '/';
    for (p = fasl; *p; p++) if (*p == '\\') *p = '/';
    f = fopen(src, "w");
    ASSERT(f != NULL);
    fprintf(f,
        "(eval-when (:compile-toplevel :load-toplevel :execute) (require \"amiga/ffi\"))\n"
        "(defvar cl-user::*rt-base* nil)\n"
        "(amiga.ffi:defcfun cl-user::rt-fn cl-user::*rt-base* -48 (:a0 a :d0 b) :result :i16)\n"
        "(ffi:defcstruct (cl-user::rt-pt :size 8) (cl-user::x :i16 0) (cl-user::arr (:array :u8 4) 4))\n"
        "(defun cl-user::rt-caller (a) (cl-user::rt-fn a 1))\n"
        "(defun cl-user::rt-field (m) (setf (cl-user::rt-pt-arr m 2) 9) (list (cl-user::rt-pt-x m) (cl-user::rt-pt-arr m 2)))\n");
    fclose(f);
    snprintf(form, sizeof(form),
             "(progn (compile-file \"%s\" :output-file \"%s\") t)", src, fasl);
    ASSERT_STR_EQ(eval_print(form), "T");
    /* the compile-time EVAL-WHEN installed the stub in this image too */
    ASSERT_STR_EQ(eval_print("(fboundp 'cl-user::rt-fn)"), "T");
    /* wipe and reload from the FASL */
    eval_print("(fmakunbound 'cl-user::rt-fn) (fmakunbound 'cl-user::rt-pt-x)");
    snprintf(form, sizeof(form), "(progn (load \"%s\") t)", fasl);
    ASSERT_STR_EQ(eval_print(form), "T");
    ASSERT_STR_EQ(eval_print("(ffi::%ffi-stub-info 'cl-user::rt-fn)"),
        "(:KIND :LIBCALL :NAME RT-FN :BASE *RT-BASE* :LVO -48 :REGSPEC 8 :RESULT :I16 :NPARAMS 2)");
    ASSERT_STR_EQ(eval_print("(ffi::%ffi-stub-info 'cl-user::rt-pt-x)"),
        "(:KIND :PEEK :NAME RT-PT-X :CTYPE :I16 :OFFSET 0)");
    ASSERT(contains(eval_print("(cl-user::rt-caller 1)"), "not open"));
    ASSERT(contains(eval_print(
        "(with-output-to-string (*standard-output*) (disassemble 'cl-user::rt-caller))"),
        "AMIGA_CALL"));
    ASSERT_STR_EQ(eval_print(
        "(let ((m (ffi:alloc-foreign 8))) (setf (cl-user::rt-pt-x m) -5) "
        "  (prog1 (cl-user::rt-field m) (ffi:free-foreign m)))"), "(-5 9)");
    remove(src);
    remove(fasl);
}

/* ================================================================
 * Printing, introspection, constructors
 * ================================================================ */

TEST(printer_describe_disassemble_arglist)
{
    CL_Obj name = user_sym("STUB-PR");
    CL_Obj stub;
    char buf[256];
    CL_GC_PROTECT(name);
    stub = cl_make_ffi_stub(CL_STUB_LIBCALL, name, name, 0x30000089u, -390, 2);
    CL_GC_PROTECT(stub);
    cl_prin1_to_string(stub, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "#<FFI-STUB STUB-PR LVO -390 (2 args)>");
    stub = cl_make_ffi_stub(CL_STUB_PEEK_IDX, name, CL_NIL, 12u | (4u << 16), 2, CL_STUB_CT_I16);
    cl_prin1_to_string(stub, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "#<FFI-STUB STUB-PR PEEK-IDX :I16 @12 [4 x 2]>");
    stub = cl_make_ffi_stub(CL_STUB_FIELD_PTR, name, CL_NIL, 8, 0, 0);
    cl_prin1_to_string(stub, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "#<FFI-STUB STUB-PR FIELD-PTR @8>");
    stub = cl_make_ffi_stub(CL_STUB_POKE, CL_NIL, CL_NIL, 8, 0, CL_STUB_CT_DOUBLE);
    cl_prin1_to_string(stub, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "#<FFI-STUB anonymous POKE :DOUBLE @8>");
    CL_GC_UNPROTECT(2);

    /* DESCRIBE / DISASSEMBLE / arglist / lambda-expression on a defcstruct stub */
    {
        const char *r;
        r = eval_print("(with-output-to-string (*standard-output*) (describe #'cl-user::fk-arr))");
        ASSERT(contains(r, "FFI stub"));
        ASSERT(contains(r, "array field reader, :I16 at offset 44, 2-byte elements, count 2"));
        r = eval_print("(with-output-to-string (*standard-output*) (describe #'cl-user::stub-pk))");
        ASSERT(contains(r, "struct field reader, :U16 at offset 2"));
        r = eval_print("(with-output-to-string (*standard-output*) (disassemble 'cl-user::fk-u8))");
        ASSERT(contains(r, "FFI stub: #<FFI-STUB FK-U8 PEEK :U8 @0>"));
        ASSERT_STR_EQ(eval_print("(ext:function-arglist #'cl-user::fk-arr)"), "(#:ARG0 #:ARG1)");
        ASSERT_STR_EQ(eval_print("(ext:function-arglist #'cl-user::%set-fk-arr)"), "(#:ARG0 #:ARG1 #:ARG2)");
        ASSERT_STR_EQ(eval_print("(ext:function-arglist #'cl-user::fk-sub)"), "(#:ARG0)");
        ASSERT_STR_EQ(eval_print("(multiple-value-list (function-lambda-expression #'cl-user::fk-u8))"),
                      "(NIL T FK-U8)");
        ASSERT_STR_EQ(eval_print("(ffi::%ffi-stub-info #'car)"), "NIL");
        ASSERT_STR_EQ(eval_print("(ffi::%ffi-stub-info 'no-such-function-zzz)"), "NIL");
    }
}

TEST(constructors_validate_arguments)
{
    /* libcall */
    ASSERT(contains(eval_print("(amiga::%make-libcall-stub 'cl-user::v '*b* -30 8 0 1)"), "#<FFI-STUB V LVO -30 (1 args)>"));
    ASSERT(contains(eval_print("(amiga::%make-libcall-stub \"v\" '*b* -30 8 0 1)"), "NAME must be a symbol"));
    ASSERT(contains(eval_print("(amiga::%make-libcall-stub 'cl-user::v 42 -30 8 0 1)"), "library base must be a symbol"));
    ASSERT(contains(eval_print("(amiga::%make-libcall-stub 'cl-user::v '*b* 30 8 0 1)"), "LVO offset must be a negative integer"));
    ASSERT(contains(eval_print("(amiga::%make-libcall-stub 'cl-user::v '*b* -40000 8 0 1)"), "LVO offset must be"));
    ASSERT(contains(eval_print("(amiga::%make-libcall-stub 'cl-user::v '*b* -30 8 9 1)"), "result kind must be 0..8"));
    ASSERT(contains(eval_print("(amiga::%make-libcall-stub 'cl-user::v '*b* -30 8 0 8)"), "argument count must be 0..7"));
    /* register index 13 (A5) for an in-use argument is refused */
    ASSERT(contains(eval_print("(amiga::%make-libcall-stub 'cl-user::v '*b* -30 #xD 0 1)"), "not one of D0-D7/A0-A4"));
    /* ... but harmless in an unused nibble */
    ASSERT(contains(eval_print("(amiga::%make-libcall-stub 'cl-user::v '*b* -30 #xD0 0 1)"), "#<FFI-STUB"));
    /* %DEFCFUN installs and returns the name */
    ASSERT_STR_EQ(eval_print("(amiga::%defcfun 'cl-user::v2 '*b* -30 8 2 1)"), "V2");
    ASSERT(contains(eval_print("(ffi::%ffi-stub-info 'cl-user::v2)"), ":RESULT :POINTER"));

    /* field stubs */
    ASSERT(contains(eval_print("(ffi::%make-field-stub 'cl-user::w :peek :u8 3)"), "#<FFI-STUB W PEEK :U8 @3>"));
    ASSERT(contains(eval_print("(ffi::%make-field-stub 'cl-user::w :libcall :u8 3)"), "kind must be :PEEK"));
    ASSERT(contains(eval_print("(ffi::%make-field-stub 'cl-user::w :peek :u128 3)"), "unknown field type U128"));
    ASSERT(contains(eval_print("(ffi::%make-field-stub 'cl-user::w :peek :u8 -1)"), "offset must be a non-negative fixnum"));
    ASSERT(contains(eval_print("(ffi::%make-field-stub 'cl-user::w :peek-idx :u8 3 3)"), "element size must be 1, 2, 4 or 8"));
    ASSERT(contains(eval_print("(ffi::%make-field-stub 'cl-user::w :peek-idx :u8 3 1 70000)"), "element count must be 0..65535"));
    ASSERT(contains(eval_print("(ffi::%make-field-stub 'cl-user::w :peek-idx :u8 70000 1 4)"), "array field offset must be <= 65535"));
    ASSERT(contains(eval_print("(ffi::%make-field-stub 'cl-user::w :peek-idx :u8 3 1 4)"), "#<FFI-STUB W PEEK-IDX :U8 @3 [4 x 1]>"));
    ASSERT(contains(eval_print("(ffi::%make-field-stub 'cl-user::w :field-ptr nil 16)"), "#<FFI-STUB W FIELD-PTR @16>"));
    /* bulk installer */
    ASSERT(contains(eval_print("(ffi::%define-cstruct-accessors '((cl-user::q-a :u8)))"), "malformed field entry"));
    ASSERT(contains(eval_print("(ffi::%define-cstruct-accessors '((cl-user::q-a :u128 0)))"), "unknown field type"));
    ASSERT(contains(eval_print("(ffi::%define-cstruct-accessors '((cl-user::q-a (:array :u8 0) 0)))"), "positive element count"));
    ASSERT_STR_EQ(eval_print("(ffi::%define-cstruct-accessors '((cl-user::q-a :u8 0) (cl-user::q-s (:struct 4) 4)))"), "T");
    ASSERT_STR_EQ(eval_print("(list (fboundp 'cl-user::q-a) (fboundp 'cl-user::%set-q-a) (fboundp 'cl-user::q-s) (fboundp 'cl-user::%set-q-s))"),
                  "(T T T NIL)");
}

int main(void)
{
    setup();

    printf("--- test_ffi_stub ---\n");

    RUN(make_stub_sets_fields_and_is_a_function);
    RUN(arity_per_kind);
    RUN(stub_survives_compaction_with_children_forwarded);

    RUN(peek_stub_called_via_apply_opcall_funcall_apply_mapcar);
    RUN(stub_arity_errors_name_the_binding);
    RUN(field_kinds_round_trip);
    RUN(defcstruct_setf_compiled_in_same_form);

    RUN(fasl_tag_and_version_pinned);
    RUN(fasl_round_trip_preserves_every_field);
    RUN(fasl_rejects_corrupt_stub_descriptor);
    RUN(compile_file_load_round_trip);

    RUN(printer_describe_disassemble_arglist);
    RUN(constructors_validate_arguments);

    REPORT();
    teardown();
    return test_fail > 0 ? 1 : 0;
}
