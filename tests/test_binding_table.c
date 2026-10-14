/*
 * test_binding_table.c — specification of the demand-interned binding
 * tables (src/core/bindtab.c, CL_Package.bindings; specs/
 * raw-bindings-footprint.md Phase 2): the packed table a generated OS
 * binding module ships instead of ~2000 definitions, and the package hook
 * that builds a name the first time anything looks it up.
 *
 * Pinned here:
 *   - the packer: every row kind, value encodings (u32 / i32 / wide
 *     bignum), sorting, the validation errors, the decoder round trip
 *   - materialisation through FIND-SYMBOL / INTERN / the reader / a FASL
 *     reference: status, value, CONSTANTP / special, FFI stubs, arity
 *   - the %SET- writer probe and the DEFSETF pair
 *   - platform / version guards (present, exported, unbound when failed;
 *     MorphOS and AmigaOS variants of one name)
 *   - inheritance through :USE; present (shadowing) symbols filled at
 *     registration; a reload overwrites; INTERN of a non-table name
 *   - the eager flip on DO-SYMBOLS / UNINTERN / %PACKAGE-SYMBOLS: same
 *     symbols (EQ), table dropped, nothing resurrects
 *   - GC: materialised symbols and their stubs survive compaction
 *   - COMPILE-FILE of a DEFINE-BINDING-TABLE module: the FASL carries the
 *     blob, a fresh load registers it, names resolve lazily
 *   - SHEDDING (SAVE-IMAGE :SHAKE-BINDINGS, Phase 3): the blob goes, the
 *     marker stays, touched names keep working, untouched ones stop
 *     existing (with a reader error that says why), the flip becomes a
 *     no-op, and re-registration undoes it
 *   - the DEFMACRO trampoline with > 254 arguments (the bug a 1000-row
 *     table form exposed: it used to drop the tail silently)
 *
 * The generator's use of it is tests/test_amiga_bindgen.sh; the Amiga-side
 * calls are tests/amiga/test-raw-bindings.lisp.
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
#include "core/bindtab.h"
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
    static char buf[4096];
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

/* The shared fixture: a package with one shadowed name and a table that
 * exercises every row kind.  *BTT-VERSION* starts NIL (version guards
 * fail, like on the host), *BTT-BASE* NIL (library calls error out at the
 * base check — the stub's own path). */
static void define_fixture(const char *pkg)
{
    char form[2048];
    snprintf(form, sizeof(form),
        "(progn"
        " (defpackage \"%s\" (:use \"CL\") (:shadow \"OPEN\") (:export \"OPEN\"))"
        " (defvar *btt-base* nil)"
        " (defvar *btt-version* nil)"
        " (amilisp::%%register-binding-table \"%s\""
        "  (amilisp::%%make-binding-table"
        "   '((:const \"+ONE+\" 1) (:const \"+U32+\" #xFFFFFFFF) (:const \"+NEG+\" -5)"
        "     (:const \"+BIG+\" #x746578742E6461746174797065) (:const \"+NEGBIG+\" -123456789012345678901234567890)"
        "     (:const \"+CLASS+\" \"Window.mui\")"
        "     (:var \"*SZ*\" 12)"
        "     (:fn \"OPEN\" -30 (:d1 :d2) :unsigned)"
        "     (:fn \"FOO\" -36 (:a0) :pointer)"
        "     (:fn \"NEWER\" -42 (:a0) :bool 45)"
        "     (:fn \"MOS-ONLY\" -48 () :void :morphos)"
        "     (:fn \"VARIANT\" -54 (:a0) :signed :not-morphos)"
        "     (:fn \"VARIANT\" -60 (:a0) :signed :morphos)"
        "     (:field \"PT-X\" :i16 0)"
        "     (:field \"PT-ARR\" (:array :u8 4) 2)"
        "     (:struct \"WIN\" 20 (\"LEFT\" :i16 6) (\"NODE\" (:struct 8) 8) (\"RP\" :fptr 16))"
        "     (:name \"BLT\")))"
        "  '*btt-base* '*btt-version*)"
        " t)", pkg, pkg);
    eval_print(form);
}

/* ================================================================
 * Packer / decoder
 * ================================================================ */

TEST(pack_decode_round_trip_every_row_kind)
{
    const char *r = eval_print(
        "(amilisp::%binding-table-entries (amilisp::%make-binding-table"
        " '((:const \"+B+\" #xFFFFFFFF) (:const \"+A+\" 1) (:const \"+N+\" -5)"
        "   (:const \"+W+\" #x746578742E6461746174797065) (:const \"+NW+\" -123456789012345678901234567890)"
        "   (:const \"+S31+\" #x80000000) (:const \"+M31+\" -2147483648) (:const \"+M32+\" -2147483649)"
        "   (:const \"+STR+\" \"Window.mui\") (:const \"+EMPTY+\" \"\") (:var \"*VS*\" \"vs\")"
        "   (:var \"*V*\" 7)"
        "   (:fn \"F\" -30 (:a0 :d0) :pointer) (:fn \"G\" -36 () :void :morphos 40)"
        "   (:fn \"G\" -42 () :void :not-morphos)"
        "   (:field \"P-X\" :i16 0) (:field \"P-A\" (:array :u16 3) 4) (:field \"P-S\" (:struct 8) 10)"
        "   (:struct \"Q\" 8 (\"X\" :u8 0) (\"Y\" (:array :i8 2) 1))"
        "   (:name \"BIG\"))))");
    /* sorted by name, struct expanded into *Q-SIZE* + accessors, variants kept
     * in order; string values (a C header's MUIC_Window "Window.mui") come
     * back as strings, the empty string included */
    ASSERT_STR_EQ(r,
        "((:VAR \"*Q-SIZE*\" 8) (:VAR \"*V*\" 7) (:VAR \"*VS*\" \"vs\") (:CONST \"+A+\" 1) (:CONST \"+B+\" 4294967295)"
        " (:CONST \"+EMPTY+\" \"\")"
        " (:CONST \"+M31+\" -2147483648) (:CONST \"+M32+\" -2147483649) (:CONST \"+N+\" -5)"
        " (:CONST \"+NW+\" -123456789012345678901234567890) (:CONST \"+S31+\" 2147483648)"
        " (:CONST \"+STR+\" \"Window.mui\")"
        " (:CONST \"+W+\" 9221870457395268199001198522469) (:NAME \"BIG\")"
        " (:FN \"F\" -30 (:A0 :D0) :POINTER) (:FN \"G\" -36 NIL :VOID :MORPHOS 40)"
        " (:FN \"G\" -42 NIL :VOID :NOT-MORPHOS) (:FIELD \"P-A\" (:ARRAY :U16 3) 4)"
        " (:FIELD \"P-S\" :STRUCT 10) (:FIELD \"P-X\" :I16 0) (:FIELD \"Q-X\" :U8 0)"
        " (:FIELD \"Q-Y\" (:ARRAY :I8 2) 1))");
}

TEST(pack_blob_layout_and_size)
{
    /* header 16 + 3 entries x 16 + names "+A+" "+B+" "F" = 7 bytes */
    ASSERT_STR_EQ(eval_print(
        "(length (amilisp::%make-binding-table '((:const \"+A+\" 1) (:const \"+B+\" 2) (:fn \"F\" -30 () :void))))"),
        "71");
    /* magic "CLBT", version 1, the has-libcalls flag */
    ASSERT_STR_EQ(eval_print(
        "(let ((b (amilisp::%make-binding-table '((:fn \"F\" -30 () :void)))))"
        " (list (aref b 0) (aref b 1) (aref b 2) (aref b 3) (aref b 4) (aref b 5)))"),
        "(67 76 66 84 1 1)");
    ASSERT_STR_EQ(eval_print(
        "(aref (amilisp::%make-binding-table '((:const \"+A+\" 1))) 5)"), "0");
    ASSERT_STR_EQ(eval_print("(amilisp::%binding-table-entries (amilisp::%make-binding-table '()))"), "NIL");
    /* a string value: u8 length + the bytes in the arena after the names —
     * header 16 + 1 entry x 16 + "+A+" 3 + (1 + "xy" 2) = 38; the entry's
     * payload is the arena offset of the length byte (3), flags = STRING */
    ASSERT_STR_EQ(eval_print(
        "(let ((b (amilisp::%make-binding-table '((:const \"+A+\" \"xy\")))))"
        " (list (length b) (aref b 23) (aref b 27) (aref b 35) (aref b 36) (aref b 37)))"),
        "(38 16 3 2 120 121)");
}

TEST(packer_rejects_malformed_rows)
{
    ASSERT(contains(eval_print("(amilisp::%make-binding-table '((:const \"+A+\" 1.5)))"),
                    "row 1 (+A+): value must be an integer or a string, got SINGLE-FLOAT"));
    ASSERT(contains(eval_print("(amilisp::%make-binding-table '((:const \"+A+\" :kw)))"),
                    "row 1 (+A+): value must be an integer or a string"));
    /* string values: ASCII, at most 255 characters (the arena length byte) */
    ASSERT(contains(eval_print("(amilisp::%make-binding-table (list (list :const \"+A+\" (format nil \"W~Andow\" (code-char 233)))))"),
                    "row 1 (+A+): string value must be ASCII (character 1 is code 233)"));
    ASSERT(contains(eval_print("(amilisp::%make-binding-table (list (list :const \"+A+\" (make-string 256 :initial-element #\\x))))"),
                    "row 1 (+A+): string value must be at most 255 characters (got 256)"));
    ASSERT_STR_EQ(eval_print("(length (third (first (amilisp::%binding-table-entries"
                             " (amilisp::%make-binding-table (list (list :const \"+A+\" (make-string 255 :initial-element #\\x))))))))"),
                  "255");
    ASSERT(contains(eval_print("(amilisp::%make-binding-table '((:const \"\" 1)))"),
                    "1..255 characters"));
    ASSERT(contains(eval_print("(amilisp::%make-binding-table '((:const \"\\\"\" 1)))"),
                    "")); /* one-char non-ASCII check is below */
    ASSERT(contains(eval_print("(amilisp::%make-binding-table (list (list :const (string (code-char 233)) 1)))"),
                    "must be ASCII"));
    ASSERT(contains(eval_print("(amilisp::%make-binding-table '((:fn \"F\" 30 () :void)))"),
                    "LVO must be a negative integer"));
    ASSERT(contains(eval_print("(amilisp::%make-binding-table '((:fn \"F\" -30 (:a5) :void)))"),
                    "register 0 is not one of :D0-:D7/:A0-:A4"));
    ASSERT(contains(eval_print("(amilisp::%make-binding-table '((:fn \"F\" -30 (:a0 :a1 :a2 :a3 :a4 :d0 :d1 :d2) :void)))"),
                    "0..7"));
    ASSERT(contains(eval_print("(amilisp::%make-binding-table '((:fn \"F\" -30 () :bogus)))"),
                    "result kind must be one of"));
    ASSERT(contains(eval_print("(amilisp::%make-binding-table '((:fn \"F\" -30 () :void :morphos :not-morphos)))"),
                    "exclude each other"));
    ASSERT(contains(eval_print("(amilisp::%make-binding-table '((:fn \"F\" -30 () :void :later)))"),
                    "unknown option LATER"));
    ASSERT(contains(eval_print("(amilisp::%make-binding-table '((:field \"X\" :i64 0)))"),
                    "unknown field type"));
    ASSERT(contains(eval_print("(amilisp::%make-binding-table '((:field \"X\" (:array :u8 0) 0)))"),
                    "element count 1..65535"));
    ASSERT(contains(eval_print("(amilisp::%make-binding-table '((:field \"X\" :u8 -1)))"),
                    "offset must be a non-negative fixnum"));
    ASSERT(contains(eval_print("(amilisp::%make-binding-table '((:struct \"S\" 4 (\"X\" :u8))))"),
                    "struct field must be"));
    ASSERT(contains(eval_print("(amilisp::%make-binding-table '((:zap \"X\")))"),
                    "unknown row kind ZAP"));
    ASSERT(contains(eval_print("(amilisp::%make-binding-table '((:const \"+A+\" 1) (:const \"+A+\" 2)))"),
                    "duplicate name +A+"));
    ASSERT(contains(eval_print("(amilisp::%make-binding-table '((:const \"+A+\" 1) (:fn \"+A+\" -30 () :void)))"),
                    "duplicate name +A+"));
    ASSERT(contains(eval_print("(amilisp::%make-binding-table '(42))"),
                    "row 1: expected"));
    ASSERT(contains(eval_print("(amilisp::%make-binding-table 'x)"), "proper list"));
}

TEST(register_rejects_corrupt_blobs_and_bad_args)
{
    eval_print("(defpackage \"BTT-R\" (:use \"CL\"))");
    ASSERT(contains(eval_print("(amilisp::%register-binding-table \"BTT-R\" \"nope\" nil nil)"),
                    "malformed binding table (not a byte vector)"));
    ASSERT(contains(eval_print(
        "(let ((b (amilisp::%make-binding-table '((:const \"+A+\" 1))))) (setf (aref b 0) 88)"
        " (amilisp::%register-binding-table \"BTT-R\" b nil nil))"),
        "bad magic"));
    ASSERT(contains(eval_print(
        "(let ((b (amilisp::%make-binding-table '((:const \"+A+\" 1))))) (setf (aref b 22) 99)"
        " (amilisp::%register-binding-table \"BTT-R\" b nil nil))"),
        "unknown entry kind"));
    /* a string value whose length byte (arena offset 3 = blob byte 35 in
     * the one-row layout) points past the arena, and an unknown flag bit */
    ASSERT(contains(eval_print(
        "(let ((b (amilisp::%make-binding-table '((:const \"+A+\" \"xy\"))))) (setf (aref b 35) 200)"
        " (amilisp::%register-binding-table \"BTT-R\" b nil nil))"),
        "string value outside the arena"));
    ASSERT(contains(eval_print(
        "(let ((b (amilisp::%make-binding-table '((:const \"+A+\" \"xy\"))))) (setf (aref b 36) 200)"
        " (amilisp::%register-binding-table \"BTT-R\" b nil nil))"),
        "non-ASCII string value"));
    ASSERT(contains(eval_print(
        "(let ((b (amilisp::%make-binding-table '((:const \"+A+\" 1))))) (setf (aref b 23) 128)"
        " (amilisp::%register-binding-table \"BTT-R\" b nil nil))"),
        "unknown entry flag"));
    ASSERT(contains(eval_print(
        "(amilisp::%register-binding-table \"BTT-R\""
        " (amilisp::%make-binding-table '((:fn \"F\" -30 () :void))) nil nil)"),
        "no :BASE variable"));
    ASSERT(contains(eval_print(
        "(amilisp::%register-binding-table \"NO-SUCH-PKG\""
        " (amilisp::%make-binding-table '((:const \"+A+\" 1))) nil nil)"),
        "not found"));
    /* a rejected table leaves the package ordinary */
    ASSERT_STR_EQ(eval_print("(amilisp::%binding-table-info \"BTT-R\")"), "NIL");
}

/* ================================================================
 * Materialisation
 * ================================================================ */

TEST(find_symbol_materialises_constants_and_variables)
{
    define_fixture("BTT-A");
    /* nothing built at registration beyond the shadow + eager names */
    ASSERT(contains(eval_print("(getf (amilisp::%binding-table-info \"BTT-A\") :symbols)"), "1"));
    ASSERT_STR_EQ(eval_print("(getf (amilisp::%binding-table-info \"BTT-A\") :entries)"), "20");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (find-symbol \"+ONE+\" \"BTT-A\"))"),
                  "(BTT-A:+ONE+ :EXTERNAL)");
    ASSERT_STR_EQ(eval_print("(list btt-a:+one+ btt-a:+u32+ btt-a:+neg+ btt-a:+big+ btt-a:+negbig+ btt-a:*sz*)"),
                  "(1 4294967295 -5 9221870457395268199001198522469 -123456789012345678901234567890 12)");
    ASSERT_STR_EQ(eval_print("(list (constantp 'btt-a:+one+) (constantp 'btt-a:*sz*) (boundp 'btt-a:*sz*))"),
                  "(T NIL T)");
    /* a string constant: a fresh simple string, DEFCONSTANT semantics, the
     * same object on every read */
    ASSERT_STR_EQ(eval_print("(multiple-value-list (find-symbol \"+CLASS+\" \"BTT-A\"))"),
                  "(BTT-A:+CLASS+ :EXTERNAL)");
    ASSERT_STR_EQ(eval_print("(list btt-a:+class+ (constantp 'btt-a:+class+) (simple-string-p btt-a:+class+)"
                             " (eq btt-a:+class+ (symbol-value 'btt-a:+class+)))"),
                  "(\"Window.mui\" T T T)");
    ASSERT(contains(eval_print("(setq btt-a:+class+ \"x\")"), "constant"));
    /* a special variable, like DEFCSTRUCT's defvar: dynamically rebindable */
    ASSERT_STR_EQ(eval_print("(let ((btt-a:*sz* 99)) (list btt-a:*sz* (symbol-value 'btt-a:*sz*)))"), "(99 99)");
    ASSERT(contains(eval_print("(setq btt-a:+one+ 2)"), "constant"));
    /* second lookup: the same symbol, nothing rebuilt */
    ASSERT_STR_EQ(eval_print("(eq (find-symbol \"+ONE+\" \"BTT-A\") 'btt-a:+one+)"), "T");
    ASSERT_STR_EQ(eval_print("(symbol-package 'btt-a:+one+)"), "#<PACKAGE BTT-A>");
    /* a name the table does not have is simply absent */
    ASSERT_STR_EQ(eval_print("(multiple-value-list (find-symbol \"NOPE\" \"BTT-A\"))"), "(NIL NIL)");
}

TEST(function_entries_become_ffi_stubs)
{
    define_fixture("BTT-B");
    ASSERT_STR_EQ(eval_print("(ffi::%ffi-stub-info #'btt-b:foo)"),
                  "(:KIND :LIBCALL :NAME BTT-B:FOO :BASE *BTT-BASE* :LVO -36 :REGSPEC 8 :RESULT :POINTER :NPARAMS 1)");
    ASSERT_STR_EQ(eval_print("(list (functionp #'btt-b:foo) (fboundp 'btt-b:foo) (length (ext:function-arglist #'btt-b:foo)))"),
                  "(T T 1)");
    /* the stub's own arity error, and the base-variable check */
    ASSERT(contains(eval_print("(btt-b:foo)"), "expected 1, got 0"));
    ASSERT(contains(eval_print("(btt-b:foo 1)"), "not open"));
    ASSERT(contains(eval_print("(funcall #'btt-b:foo 1)"), "not open"));
    /* the :shadow name got the definition at registration and is external */
    ASSERT_STR_EQ(eval_print("(list (eq 'btt-b:open 'cl:open) (getf (ffi::%ffi-stub-info #'btt-b:open) :lvo)"
                             " (nth-value 1 (find-symbol \"OPEN\" \"BTT-B\")))"),
                  "(NIL -30 :EXTERNAL)");
    /* a (:name) row: exported, no definition */
    ASSERT_STR_EQ(eval_print("(multiple-value-list (find-symbol \"BLT\" \"BTT-B\"))"), "(BTT-B:BLT :EXTERNAL)");
    ASSERT_STR_EQ(eval_print("(fboundp 'btt-b:blt)"), "NIL");
    ASSERT_STR_EQ(eval_print("(progn (defun btt-b:blt (x) (* x 2)) (btt-b:blt 21))"), "42");
}

TEST(field_entries_reader_writer_and_setf)
{
    define_fixture("BTT-C");
    ASSERT_STR_EQ(eval_print("(ffi::%ffi-stub-info #'btt-c:pt-x)"),
                  "(:KIND :PEEK :NAME BTT-C:PT-X :CTYPE :I16 :OFFSET 0)");
    /* the writer is built with the reader, internal, DEFSETF-linked */
    ASSERT_STR_EQ(eval_print("(multiple-value-list (find-symbol \"%SET-PT-X\" \"BTT-C\"))"),
                  "(BTT-C::%SET-PT-X :INTERNAL)");
    ASSERT_STR_EQ(eval_print("(getf (ffi::%ffi-stub-info 'btt-c::%set-pt-x) :kind)"), ":POKE");
    ASSERT_STR_EQ(eval_print(
        "(let ((m (ffi:alloc-foreign 32)))"
        " (unwind-protect"
        "  (progn (setf (btt-c:pt-x m) -7 (btt-c:pt-arr m 3) 200 (btt-c:win-left m) 9 (btt-c:win-rp m) m)"
        "   (list (btt-c:pt-x m) (btt-c:pt-arr m 3) (btt-c:win-left m)"
        "         (ffi:foreign-pointer-p (btt-c:win-node m)) (ffi:foreign-pointer-p (btt-c:win-rp m))"
        "         btt-c:*win-size*))"
        "  (ffi:free-foreign m)))"),
        "(-7 200 9 T T 20)");
    /* probing the writer FIRST builds the pair too */
    ASSERT_STR_EQ(eval_print("(multiple-value-list (find-symbol \"%SET-WIN-LEFT\" \"BTT-C\"))"),
                  "(BTT-C::%SET-WIN-LEFT :INTERNAL)");
    /* an embedded struct has no writer */
    ASSERT_STR_EQ(eval_print("(multiple-value-list (find-symbol \"%SET-WIN-NODE\" \"BTT-C\"))"), "(NIL NIL)");
    ASSERT(contains(eval_print("(btt-c:pt-arr (ffi:make-foreign-pointer 16) 7)"), "out of range"));
}

TEST(guards_present_exported_unbound_and_variants)
{
    define_fixture("BTT-D");
    /* version guard with *BTT-VERSION* NIL: exists, exported, unbound */
    ASSERT_STR_EQ(eval_print("(multiple-value-list (find-symbol \"NEWER\" \"BTT-D\"))"), "(BTT-D:NEWER :EXTERNAL)");
    ASSERT_STR_EQ(eval_print("(fboundp 'btt-d:newer)"), "NIL");
    ASSERT_STR_EQ(eval_print("(fboundp 'btt-d:mos-only)"), "NIL");
    ASSERT_STR_EQ(eval_print("(getf (ffi::%ffi-stub-info #'btt-d:variant) :lvo)"), "-54");
    /* a second package, same table, :MORPHOS on and the version high enough */
    eval_print("(push :morphos *features*)");
    eval_print("(setq *btt-version* 46)");
    define_fixture("BTT-D2");
    ASSERT_STR_EQ(eval_print("(list (fboundp 'btt-d2:newer) (fboundp 'btt-d2:mos-only)"
                             " (getf (ffi::%ffi-stub-info #'btt-d2:variant) :lvo))"),
                  "(T T -60)");
    eval_print("(setq *features* (remove :morphos *features*))");
    eval_print("(setq *btt-version* 44)");
    define_fixture("BTT-D3");
    ASSERT_STR_EQ(eval_print("(fboundp 'btt-d3:newer)"), "NIL");
    eval_print("(setq *btt-version* nil)");
}

TEST(intern_and_reader_go_through_the_table)
{
    define_fixture("BTT-E");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (intern \"+ONE+\" \"BTT-E\"))"), "(BTT-E:+ONE+ :EXTERNAL)");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (intern \"FRESH\" \"BTT-E\"))"), "(BTT-E::FRESH NIL)");
    /* pkg::name reads through cl_intern_in */
    ASSERT_STR_EQ(eval_print("(list btt-e::+neg+ (nth-value 1 (find-symbol \"+NEG+\" \"BTT-E\")))"), "(-5 :EXTERNAL)");
    /* the reader in the package itself */
    ASSERT_STR_EQ(eval_print("(let ((*package* (find-package \"BTT-E\"))) (eval (read-from-string \"(list +u32+ *sz*)\")))"),
                  "(4294967295 12)");
    /* find-all-symbols materialises in every table that has the name */
    define_fixture("BTT-E2");
    ASSERT_STR_EQ(eval_print("(length (remove-if-not (lambda (s) (member (package-name (symbol-package s)) '(\"BTT-E\" \"BTT-E2\") :test #'string=)) (find-all-symbols \"+BIG+\")))"), "2");
}

TEST(inheritance_through_use_and_shadowing)
{
    define_fixture("BTT-F");
    eval_print("(defpackage \"BTT-F-USER\" (:use \"CL\" \"BTT-F\"))");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (find-symbol \"FOO\" \"BTT-F-USER\"))"), "(BTT-F:FOO :INHERITED)");
    ASSERT_STR_EQ(eval_print("(let ((*package* (find-package \"BTT-F-USER\"))) (eval (read-from-string \"+one+\")))"), "1");
    /* internal setters are NOT inherited */
    ASSERT_STR_EQ(eval_print("(multiple-value-list (find-symbol \"%SET-PT-X\" \"BTT-F-USER\"))"), "(NIL NIL)");
    /* INTERN in the user yields the inherited (lazy) symbol, as CLHS says */
    ASSERT_STR_EQ(eval_print("(multiple-value-list (intern \"+NEG+\" \"BTT-F-USER\"))"),
                  "(BTT-F:+NEG+ :INHERITED)");
    /* a present (shadowing) symbol in the user wins over the lazy one */
    eval_print("(shadow \"+U32+\" \"BTT-F-USER\")");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (find-symbol \"+U32+\" \"BTT-F-USER\"))"),
                  "(BTT-F-USER::+U32+ :INTERNAL)");
    /* SHADOW of a table name in the lazy package itself builds it (it IS present) */
    eval_print("(shadow \"+U32+\" \"BTT-F\")");
    ASSERT_STR_EQ(eval_print("(list btt-f:+u32+ (nth-value 1 (find-symbol \"+U32+\" \"BTT-F\")))"), "(4294967295 :EXTERNAL)");
}

TEST(enumeration_flips_eager_and_nothing_resurrects)
{
    define_fixture("BTT-G");
    eval_print("(defvar *btt-g-one* 'btt-g:+one+)");
    ASSERT_STR_EQ(eval_print("(not (null (amilisp::%binding-table-info \"BTT-G\")))"), "T");
    ASSERT_STR_EQ(eval_print("(let ((n 0)) (do-symbols (s \"BTT-G\") (when (eq (symbol-package s) (find-package \"BTT-G\")) (incf n))) (> n 18))"), "T");
    /* table gone, symbols the same, everything defined */
    ASSERT_STR_EQ(eval_print("(amilisp::%binding-table-info \"BTT-G\")"), "NIL");
    ASSERT_STR_EQ(eval_print("(list (eq *btt-g-one* 'btt-g:+one+) btt-g:+big+ (fboundp 'btt-g:foo) (fboundp 'btt-g:pt-x)"
                             " (fboundp 'btt-g::%set-pt-x) (nth-value 1 (find-symbol \"NEWER\" \"BTT-G\")))"),
                  "(T 9221870457395268199001198522469 T T T :EXTERNAL)");
    /* unintern after the flip: gone for good */
    eval_print("(unintern 'btt-g:+one+ \"BTT-G\")");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (find-symbol \"+ONE+\" \"BTT-G\"))"), "(NIL NIL)");
    /* unintern on a still-lazy package flips first */
    define_fixture("BTT-G2");
    eval_print("(unintern 'btt-g2:foo \"BTT-G2\")");
    ASSERT_STR_EQ(eval_print("(list (amilisp::%binding-table-info \"BTT-G2\") (find-symbol \"FOO\" \"BTT-G2\") (fboundp 'btt-g2:pt-x))"),
                  "(NIL NIL T)");
    /* apropos and do-external-symbols see the whole set */
    define_fixture("BTT-G3");
    ASSERT_STR_EQ(eval_print("(let ((n 0)) (do-external-symbols (s \"BTT-G3\") (incf n)) (>= n 18))"), "T");
    /* %PACKAGE-SYMBOLS (what LOOP ... BEING THE SYMBOLS and APROPOS use) */
    define_fixture("BTT-G4");
    ASSERT_STR_EQ(eval_print("(and (member 'btt-g4:+negbig+ (amilisp::%package-symbols \"BTT-G4\")) t)"), "T");
    ASSERT_STR_EQ(eval_print("(amilisp::%binding-table-info \"BTT-G4\")"), "NIL");
    /* explicit flip hook */
    define_fixture("BTT-G5");
    ASSERT_STR_EQ(eval_print("(progn (amilisp::%binding-table-materialize-all \"BTT-G5\")"
                             " (list (amilisp::%binding-table-info \"BTT-G5\") (fboundp 'btt-g5:foo)))"), "(NIL T)");
}

TEST(reload_overwrites_present_definitions)
{
    define_fixture("BTT-H");
    ASSERT_STR_EQ(eval_print("btt-h:+one+"), "1");
    ASSERT_STR_EQ(eval_print("(getf (ffi::%ffi-stub-info #'btt-h:foo) :lvo)"), "-36");
    /* The reload rewrites the present FOO's function cell, so a JIT call
     * site caching the old stub must be invalidated (cl_call_gen moves). */
    ASSERT_STR_EQ(eval_print("(progn (defparameter *btt-gen* (amilisp::%call-gen)) t)"), "T");
    eval_print("(amilisp::%register-binding-table \"BTT-H\""
               " (amilisp::%make-binding-table '((:const \"+ONE+\" 111) (:fn \"FOO\" -300 (:a0) :pointer)))"
               " '*btt-base* nil)");
    ASSERT_STR_EQ(eval_print("(list btt-h:+one+ (getf (ffi::%ffi-stub-info #'btt-h:foo) :lvo))"), "(111 -300)");
    ASSERT_STR_EQ(eval_print("(/= *btt-gen* (amilisp::%call-gen))"), "T");
    /* names of the old table that the new one lacks keep their old definition */
    ASSERT_STR_EQ(eval_print("(and (fboundp 'btt-h:open) (getf (amilisp::%binding-table-info \"BTT-H\") :entries))"), "2");
}

TEST(export_import_on_lazy_package_flip_first)
{
    /* Root cause the lazy-package cases below first tripped over, in a
     * PLAIN package: a MAKE-SYMBOL symbol carried no name hash, so IMPORT /
     * EXPORT / SHADOWING-IMPORT linked it into bucket 0 and FIND-SYMBOL
     * never saw it again (CLHS 11.1.1.2.4 — importing an uninterned symbol
     * is ordinary).  MAKE-SYMBOL now fills the hash. */
    eval_print("(defpackage \"BTT-PLAIN\" (:use \"CL\"))");
    ASSERT_STR_EQ(eval_print(
        "(let ((a (make-symbol \"IMPORTED\")) (b (make-symbol \"EXPORTED\")) (c (make-symbol \"SHADOWED\")))"
        " (import a \"BTT-PLAIN\") (export b \"BTT-PLAIN\") (shadowing-import c \"BTT-PLAIN\")"
        " (list (eq (find-symbol \"IMPORTED\" \"BTT-PLAIN\") a) (nth-value 1 (find-symbol \"IMPORTED\" \"BTT-PLAIN\"))"
        "       (eq (find-symbol \"EXPORTED\" \"BTT-PLAIN\") b) (nth-value 1 (find-symbol \"EXPORTED\" \"BTT-PLAIN\"))"
        "       (eq (find-symbol \"SHADOWED\" \"BTT-PLAIN\") c)))"),
        "(T :INTERNAL T :EXTERNAL T)");
    define_fixture("BTT-I");
    eval_print("(defvar *btt-i-foreign* (make-symbol \"FOREIGN\"))");
    eval_print("(import *btt-i-foreign* \"BTT-I\")");
    ASSERT_STR_EQ(eval_print("(list (amilisp::%binding-table-info \"BTT-I\") (fboundp 'btt-i:foo))"), "(NIL T)");
    /* EXPORT of a present symbol does not flip */
    define_fixture("BTT-I2");
    eval_print("(export (intern \"+ONE+\" \"BTT-I2\") \"BTT-I2\")");
    ASSERT_STR_EQ(eval_print("(not (null (amilisp::%binding-table-info \"BTT-I2\")))"), "T");
    eval_print("(export (intern \"NEWSYM\" \"BTT-I2\") \"BTT-I2\")");
    ASSERT_STR_EQ(eval_print("(not (null (amilisp::%binding-table-info \"BTT-I2\")))"), "T");
    /* EXPORT of a genuinely foreign symbol (not in the table at all) on a
     * still-lazy package flips first, same as IMPORT above */
    define_fixture("BTT-I3");
    eval_print("(defvar *btt-i3-foreign* (make-symbol \"FOREIGN\"))");
    eval_print("(export *btt-i3-foreign* \"BTT-I3\")");
    ASSERT_STR_EQ(eval_print("(list (amilisp::%binding-table-info \"BTT-I3\")"
                             " (eq (find-symbol \"FOREIGN\" \"BTT-I3\") *btt-i3-foreign*) (fboundp 'btt-i3:foo))"),
                  "(NIL T T)");
}

/* UNEXPORT and SHADOWING-IMPORT are the two remaining export-set mutators
 * that flip a still-lazy binding-table package eager before touching it
 * (see UNINTERN in enumeration_flips_eager_and_nothing_resurrects and
 * IMPORT/EXPORT above) — pinned separately since neither was covered. */
TEST(unexport_and_shadowing_import_on_lazy_package_flip_first)
{
    define_fixture("BTT-K");
    ASSERT_STR_EQ(eval_print("(not (null (amilisp::%binding-table-info \"BTT-K\")))"), "T");
    eval_print("(unexport 'btt-k:foo \"BTT-K\")");
    ASSERT_STR_EQ(eval_print("(list (amilisp::%binding-table-info \"BTT-K\")"
                             " (nth-value 1 (find-symbol \"FOO\" \"BTT-K\")) (fboundp 'btt-k::foo))"),
                  "(NIL :INTERNAL T)");

    /* a foreign symbol with the SAME NAME as an existing table entry:
     * shadowing-import must replace the just-flipped FOO, not conflict */
    define_fixture("BTT-K2");
    eval_print("(defvar *btt-k2-foreign* (make-symbol \"FOO\"))");
    ASSERT_STR_EQ(eval_print("(not (null (amilisp::%binding-table-info \"BTT-K2\")))"), "T");
    eval_print("(shadowing-import *btt-k2-foreign* \"BTT-K2\")");
    ASSERT_STR_EQ(eval_print("(list (amilisp::%binding-table-info \"BTT-K2\")"
                             " (eq (find-symbol \"FOO\" \"BTT-K2\") *btt-k2-foreign*)"
                             " (and (member *btt-k2-foreign* (package-shadowing-symbols \"BTT-K2\")) t))"),
                  "(NIL T T)");
}

TEST(materialised_symbols_survive_compaction)
{
    CL_Obj sym, before;
    int i;
    define_fixture("BTT-J");
    sym = cl_find_symbol("+BIG+", 5, cl_find_package("BTT-J", 5));
    ASSERT(CL_SYMBOL_P(sym));
    before = sym;
    CL_GC_PROTECT(sym);
    for (i = 0; i < 3; i++) {
        int j;
        for (j = 0; j < 3000; j++)
            (void)cl_make_string("garbage-between-collections", 27);
        cl_gc();
        ASSERT(cl_find_symbol("+BIG+", 5, cl_find_package("BTT-J", 5)) == sym);
        ASSERT_STR_EQ(cl_symbol_name(sym), "+BIG+");
    }
    CL_GC_UNPROTECT(1);
    (void)before;
    ASSERT_STR_EQ(eval_print("btt-j:+big+"), "9221870457395268199001198522469");
    ASSERT_STR_EQ(eval_print("(getf (ffi::%ffi-stub-info #'btt-j:foo) :lvo)"), "-36");
    /* materialise under churn: names built while the heap moves */
    ASSERT_STR_EQ(eval_print(
        "(let ((acc nil)) (dolist (n '(\"+NEG+\" \"PT-X\" \"WIN-RP\" \"*SZ*\" \"+U32+\" \"+CLASS+\")) "
        "  (dotimes (i 2000) (make-string 40)) (ext:gc) (push (symbol-name (find-symbol n \"BTT-J\")) acc))"
        " (dotimes (i 2000) (make-string 40)) (ext:gc)"
        " (list (length acc) (fboundp 'btt-j:pt-x) btt-j:+neg+ btt-j:*sz* btt-j:+class+))"),
        "(6 T -5 12 \"Window.mui\")");
}

/* ================================================================
 * Shedding — SAVE-IMAGE :SHAKE-BINDINGS (spec Phase 3)
 * ================================================================ */

TEST(shed_closes_the_package_at_the_names_already_referenced)
{
    define_fixture("BTT-S");
    /* touch three names of different kinds before the shed */
    eval_print("(defvar *btt-s-one* 'btt-s:+one+)");
    eval_print("(defvar *btt-s-foo* #'btt-s:foo)");
    eval_print("(defvar *btt-s-ptx* (fboundp 'btt-s:pt-x))");

    ASSERT_STR_EQ(eval_print("(getf (amilisp::%binding-table-info \"BTT-S\") :shed)"), "NIL");
    /* sheds exactly the blob's bytes, and only once */
    ASSERT_STR_EQ(eval_print("(let ((b (getf (amilisp::%binding-table-info \"BTT-S\") :bytes)))"
                             "  (list (= b (amilisp::%shed-binding-tables \"BTT-S\"))"
                             "        (amilisp::%shed-binding-tables \"BTT-S\")))"), "(T 0)");

    /* the marker stays: this package WAS lazy, and says so */
    ASSERT_STR_EQ(eval_print("(list (getf (amilisp::%binding-table-info \"BTT-S\") :shed)"
                             "      (getf (amilisp::%binding-table-info \"BTT-S\") :entries)"
                             "      (getf (amilisp::%binding-table-info \"BTT-S\") :bytes)"
                             "      (amilisp::%binding-table-entries \"BTT-S\"))"),
                  "(T 0 0 NIL)");

    /* touched names keep value, stub and identity */
    ASSERT_STR_EQ(eval_print("(list (eq *btt-s-one* 'btt-s:+one+) btt-s:+one+"
                             " (eq *btt-s-foo* #'btt-s:foo)"
                             " (getf (ffi::%ffi-stub-info #'btt-s:foo) :lvo)"
                             " (fboundp 'btt-s:pt-x))"),
                  "(T 1 T -36 T)");

    /* a field's %SET- writer is built WITH its reader, so touching PT-X
     * carried the setter across too — the pair is never half-present */
    ASSERT_STR_EQ(eval_print("(nth-value 1 (find-symbol \"%SET-PT-X\" \"BTT-S\"))"), ":INTERNAL");

    /* untouched names are GONE — a clean CLHS miss */
    ASSERT_STR_EQ(eval_print("(multiple-value-list (find-symbol \"+U32+\" \"BTT-S\"))"), "(NIL NIL)");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (find-symbol \"WIN-LEFT\" \"BTT-S\"))"), "(NIL NIL)");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (find-symbol \"%SET-WIN-LEFT\" \"BTT-S\"))"), "(NIL NIL)");
    /* the reader says WHY, instead of looking like a typo */
    ASSERT(contains(eval_print("(read-from-string \"btt-s:+u32+\")"), "shed by SAVE-IMAGE"));

    /* the eager flip is a no-op on a shed package: nothing to materialise,
     * and the marker survives (without the NIL-blob guard this reads a NIL
     * byte vector) */
    ASSERT_STR_EQ(eval_print("(progn (amilisp::%binding-table-materialize-all \"BTT-S\")"
                             " (list (getf (amilisp::%binding-table-info \"BTT-S\") :shed)"
                             "       (multiple-value-list (find-symbol \"+U32+\" \"BTT-S\"))))"),
                  "(T (NIL NIL))");

    /* INTERN still makes an ordinary fresh symbol (CLHS 11.1.1.2) — unbound,
     * internal, and NOT the shed binding.  Done last: it adds the name. */
    ASSERT_STR_EQ(eval_print("(let ((s (intern \"+NEG+\" \"BTT-S\")))"
                             "  (list (symbolp s) (boundp s) (nth-value 1 (find-symbol \"+NEG+\" \"BTT-S\"))))"),
                  "(T NIL :INTERNAL)");
    /* enumeration sees only what is present, and does not resurrect */
    ASSERT_STR_EQ(eval_print("(let ((n 0)) (do-symbols (s \"BTT-S\")"
                             "   (when (eq (symbol-package s) (find-package \"BTT-S\")) (incf n)))"
                             " (list (< n 10) (getf (amilisp::%binding-table-info \"BTT-S\") :shed)))"),
                  "(T T)");

    /* re-registering a table undoes it (loading the module's FASL again),
     * and the symbols materialised before stay EQ */
    eval_print("(amilisp::%register-binding-table \"BTT-S\""
               " (amilisp::%make-binding-table '((:const \"+ONE+\" 1) (:const \"+U32+\" #xFFFFFFFF)"
               "                                 (:fn \"FOO\" -36 (:a0) :pointer)))"
               " '*btt-base* nil)");
    ASSERT_STR_EQ(eval_print("(list (getf (amilisp::%binding-table-info \"BTT-S\") :shed)"
                             " (eq *btt-s-one* 'btt-s:+one+) btt-s:+u32+)"),
                  "(NIL T 4294967295)");

    /* an ordinary package has nothing to shed */
    eval_print("(defpackage \"BTT-S-PLAIN\" (:use \"CL\"))");
    ASSERT_STR_EQ(eval_print("(list (amilisp::%shed-binding-tables \"BTT-S-PLAIN\")"
                             " (amilisp::%binding-table-info \"BTT-S-PLAIN\"))"), "(0 NIL)");
    /* nor has a package already flipped eager */
    define_fixture("BTT-S2");
    eval_print("(amilisp::%binding-table-materialize-all \"BTT-S2\")");
    ASSERT_STR_EQ(eval_print("(amilisp::%shed-binding-tables \"BTT-S2\")"), "0");
}

TEST(shed_all_reclaims_the_blobs)
{
    CL_Obj pkg, pkg2;
    uint32_t bytes = 0, count, before, after;

    define_fixture("BTT-S3");
    define_fixture("BTT-S4");
    pkg = cl_find_package("BTT-S3", 6);
    pkg2 = cl_find_package("BTT-S4", 6);
    ASSERT(CL_PACKAGE_P(pkg) && CL_PACKAGE_P(pkg2));
    ASSERT(cl_bindtab_shed_p(pkg) == 0);

    /* a dense heap measurement: compact first, so `bump` is live bytes */
    cl_gc();
    cl_gc_compact();
    before = cl_heap.bump;

    /* every lazy package in the registry — the fixtures of the tests above
     * are still lazy too, so the count is a lower bound, not an equality */
    count = cl_bindtab_shed_all(&bytes);
    ASSERT(count >= 2);
    ASSERT(bytes > 200);
    ASSERT(cl_bindtab_shed_p(pkg) == 1);
    ASSERT(cl_bindtab_shed_p(pkg2) == 1);

    /* the blobs were reachable ONLY from the packages: they are garbage now */
    cl_gc();
    cl_gc_compact();
    after = cl_heap.bump;
    ASSERT(before > after);
    ASSERT(before - after >= bytes);

    /* shedding again finds nothing, and reports it */
    ASSERT(cl_bindtab_shed(pkg) == 0);
    ASSERT(cl_bindtab_shed_all(NULL) == 0);
    /* a non-package argument is simply not a table */
    ASSERT(cl_bindtab_shed(CL_NIL) == 0);
    ASSERT(cl_bindtab_shed_p(CL_NIL) == 0);
}

/* ================================================================
 * The macro, COMPILE-FILE and the > 254-argument expander
 * ================================================================ */

TEST(define_binding_table_macro_and_compile_file_round_trip)
{
    FILE *f = fopen("/tmp/btt-mod.lisp", "w");
    ASSERT(f != NULL);
    fputs("(eval-when (:compile-toplevel :load-toplevel :execute) (require \"amiga/ffi\"))\n"
          "(defpackage \"BTT-MOD\" (:use \"CL\") (:export \"*M-BASE*\"))\n"
          "(in-package \"BTT-MOD\")\n"
          "(defvar *m-base* nil)\n"
          "(amiga.ffi:define-binding-table \"BTT-MOD\" (:base *m-base*)\n"
          "  (:const \"+K+\" #x80000064)\n"
          "  (:const \"+S+\" \"Notify.mui\")\n"
          "  (:struct \"NODE\" 14 (\"SUCC\" :fptr 0) (\"TYPE\" :u8 8))\n"
          "  (:fn \"CALL-ME\" -30 (:a0 :d0) :u16)\n"
          "  (:name \"LATE\"))\n"
          "(defun late (x) (list :late x))\n"
          "(defun user-of-k () +k+)\n", f);
    fclose(f);
    ASSERT(contains(eval_print("(compile-file \"/tmp/btt-mod.lisp\" :output-file \"/tmp/btt-mod.fasl\")"), "btt-mod.fasl"));
    /* compile time registered the table in THIS image already (eval-when) */
    ASSERT_STR_EQ(eval_print("(not (null (amilisp::%binding-table-info \"BTT-MOD\")))"), "T");
    /* wipe and reload from the FASL: the blob comes back, names resolve lazily */
    eval_print("(delete-package \"BTT-MOD\")");
    eval_print("(progn (defpackage \"BTT-MOD\" (:use \"CL\") (:export \"*M-BASE*\")) (load \"/tmp/btt-mod.fasl\"))");
    ASSERT_STR_EQ(eval_print("(getf (amilisp::%binding-table-info \"BTT-MOD\") :entries)"), "7");
    ASSERT_STR_EQ(eval_print("(list btt-mod:+k+ btt-mod:*node-size* (fboundp 'btt-mod:node-succ)"
                             " (getf (ffi::%ffi-stub-info #'btt-mod:call-me) :result) (btt-mod:late 1) (btt-mod::user-of-k)"
                             " btt-mod:+s+ (constantp 'btt-mod:+s+))"),
                  "(2147483748 14 T :U16 (:LATE 1) 2147483748 \"Notify.mui\" T)");
    unlink("/tmp/btt-mod.lisp");
    unlink("/tmp/btt-mod.fasl");
}

TEST(macro_trampoline_carries_more_than_254_arguments)
{
    /* the table forms of the generated modules have ~1000 rows; the
     * %CALL-MACRO-EXPANDER trampoline used to truncate at 254 silently */
    ASSERT_STR_EQ(eval_print(
        "(progn (defmacro btt-count (a &body rows) (declare (ignore a)) (length rows))"
        " (list (eval (list* 'btt-count 'x (loop for i below 254 collect i)))"
        "       (eval (list* 'btt-count 'x (loop for i below 255 collect i)))"
        "       (eval (list* 'btt-count 'x (loop for i below 1005 collect i)))"
        "       (eval (list* 'btt-count 'x (loop for i below 4000 collect i)))))"),
        "(254 255 1005 4000)");
    /* and a real table of 1000 rows through the macro */
    ASSERT_STR_EQ(eval_print(
        "(progn (defpackage \"BTT-BIG\" (:use \"CL\"))"
        " (eval (list* 'amiga.ffi:define-binding-table \"BTT-BIG\" '()"
        "              (loop for i below 1000 collect (list :const (format nil \"+C~D+\" i) i))))"
        " (list (getf (amilisp::%binding-table-info \"BTT-BIG\") :entries)"
        "       (symbol-value (find-symbol \"+C999+\" \"BTT-BIG\"))))"),
        "(1000 999)");
}

int main(void)
{
    setup();
    eval_print("(require \"amiga/ffi\")");

    RUN(pack_decode_round_trip_every_row_kind);
    RUN(pack_blob_layout_and_size);
    RUN(packer_rejects_malformed_rows);
    RUN(register_rejects_corrupt_blobs_and_bad_args);

    RUN(find_symbol_materialises_constants_and_variables);
    RUN(function_entries_become_ffi_stubs);
    RUN(field_entries_reader_writer_and_setf);
    RUN(guards_present_exported_unbound_and_variants);
    RUN(intern_and_reader_go_through_the_table);
    RUN(inheritance_through_use_and_shadowing);
    RUN(enumeration_flips_eager_and_nothing_resurrects);
    RUN(reload_overwrites_present_definitions);
    RUN(export_import_on_lazy_package_flip_first);
    RUN(unexport_and_shadowing_import_on_lazy_package_flip_first);
    RUN(materialised_symbols_survive_compaction);
    RUN(shed_closes_the_package_at_the_names_already_referenced);
    RUN(shed_all_reclaims_the_blobs);

    RUN(define_binding_table_macro_and_compile_file_round_trip);
    RUN(macro_trampoline_carries_more_than_254_arguments);

    teardown();
    REPORT();
}
