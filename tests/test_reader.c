#include "test.h"
#include "core/types.h"
#include "core/mem.h"
#include "core/error.h"
#include "core/package.h"
#include "core/symbol.h"
#include "core/reader.h"
#include "core/stream.h"
#include "core/string_utils.h"
#include "core/printer.h"
#include "core/compiler.h"
#include "core/vm.h"
#include "core/builtins.h"
#include "core/repl.h"
#include "platform/platform.h"
#include <stdlib.h>
#include <string.h>

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

/* Helper: read from string */
static CL_Obj reads(const char *str)
{
    CL_ReadStream stream;
    stream.buf = str;
    stream.pos = 0;
    stream.len = (int)strlen(str);
    stream.line = 1;
    return cl_read_from_string(&stream);
}

/* Helper: read and print to buffer for comparison */
static const char *read_print(const char *str)
{
    static char buf[256];
    CL_Obj obj = reads(str);
    cl_prin1_to_string(obj, buf, sizeof(buf));
    return buf;
}

/* --- Integer reading --- */

TEST(read_integer)
{
    CL_Obj obj = reads("42");
    ASSERT(CL_FIXNUM_P(obj));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(obj), 42);
}

TEST(read_negative_integer)
{
    CL_Obj obj = reads("-7");
    ASSERT(CL_FIXNUM_P(obj));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(obj), -7);
}

TEST(read_zero)
{
    CL_Obj obj = reads("0");
    ASSERT(CL_FIXNUM_P(obj));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(obj), 0);
}

/* --- Symbol reading --- */

TEST(read_symbol)
{
    CL_Obj obj = reads("foo");
    ASSERT(CL_SYMBOL_P(obj));
    ASSERT_STR_EQ(cl_symbol_name(obj), "FOO");
}

TEST(read_nil)
{
    CL_Obj obj = reads("nil");
    ASSERT(CL_NULL_P(obj));
}

TEST(read_t)
{
    CL_Obj obj = reads("t");
    ASSERT(CL_SYMBOL_P(obj));
    ASSERT_EQ(obj, SYM_T);
}

/* --- String reading --- */

TEST(read_string)
{
    CL_Obj obj = reads("\"hello\"");
    CL_String *s;
    ASSERT(CL_STRING_P(obj));
    s = (CL_String *)CL_OBJ_TO_PTR(obj);
    ASSERT_STR_EQ(s->data, "hello");
}

TEST(read_string_escape)
{
    /* CLHS 2.4.5: backslash is a single escape — the next character is taken
     * exactly as itself.  "a\nb" is the three characters a n b; CL has no
     * C-style \n escape (the old reader had one, which silently corrupted
     * conforming code such as "a\nb" meaning "anb"). */
    CL_Obj obj = reads("\"a\\nb\"");
    CL_String *s;
    ASSERT(CL_STRING_P(obj));
    s = (CL_String *)CL_OBJ_TO_PTR(obj);
    ASSERT_EQ_INT((int)s->length, 3);
    ASSERT_EQ_INT(s->data[1], 'n');

    /* \" and \\ still yield " and \ — that IS the single-escape rule. */
    obj = reads("\"a\\\"b\"");
    s = (CL_String *)CL_OBJ_TO_PTR(obj);
    ASSERT_EQ_INT((int)s->length, 3);
    ASSERT_EQ_INT(s->data[1], '"');

    obj = reads("\"a\\\\b\"");
    s = (CL_String *)CL_OBJ_TO_PTR(obj);
    ASSERT_EQ_INT((int)s->length, 3);
    ASSERT_EQ_INT(s->data[1], '\\');
}

TEST(read_string_literal_newline)
{
    /* A string literal spans lines: everything up to the closing quote is
     * accumulated, newlines included (CLHS 2.4.5). */
    CL_Obj obj = reads("\"a\nb\"");
    CL_String *s;
    ASSERT(CL_STRING_P(obj));
    s = (CL_String *)CL_OBJ_TO_PTR(obj);
    ASSERT_EQ_INT((int)s->length, 3);
    ASSERT_EQ_INT(s->data[1], '\n');
}

/* --- List reading --- */

TEST(read_empty_list)
{
    CL_Obj obj = reads("()");
    ASSERT(CL_NULL_P(obj));
}

TEST(read_list)
{
    CL_Obj obj = reads("(1 2 3)");
    ASSERT(CL_CONS_P(obj));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(obj)), 1);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(cl_cdr(obj))), 2);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(cl_cdr(cl_cdr(obj)))), 3);
    ASSERT(CL_NULL_P(cl_cdr(cl_cdr(cl_cdr(obj)))));
}

TEST(read_nested_list)
{
    CL_Obj obj = reads("(1 (2 3) 4)");
    CL_Obj inner;
    ASSERT(CL_CONS_P(obj));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(obj)), 1);
    inner = cl_car(cl_cdr(obj));
    ASSERT(CL_CONS_P(inner));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(inner)), 2);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(cl_cdr(inner))), 3);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(cl_cdr(cl_cdr(obj)))), 4);
}

TEST(read_dotted_pair)
{
    CL_Obj obj = reads("(1 . 2)");
    ASSERT(CL_CONS_P(obj));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(obj)), 1);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_cdr(obj)), 2);
}

/* --- Quote --- */

TEST(read_quote)
{
    ASSERT_STR_EQ(read_print("'foo"), "(QUOTE FOO)");
}

TEST(read_function_shorthand)
{
    ASSERT_STR_EQ(read_print("#'foo"), "(FUNCTION FOO)");
}

/* --- Character literal --- */

TEST(read_char_literal)
{
    CL_Obj obj = reads("#\\A");
    ASSERT(CL_CHAR_P(obj));
    ASSERT_EQ_INT(CL_CHAR_VAL(obj), 'A');
}

TEST(read_char_space)
{
    CL_Obj obj = reads("#\\Space");
    ASSERT(CL_CHAR_P(obj));
    ASSERT_EQ_INT(CL_CHAR_VAL(obj), ' ');
}

TEST(read_char_newline)
{
    CL_Obj obj = reads("#\\Newline");
    ASSERT(CL_CHAR_P(obj));
    ASSERT_EQ_INT(CL_CHAR_VAL(obj), '\n');
}

TEST(read_char_replacement_character)
{
    /* Regression: #\Replacement_Character (U+FFFD) is the standard Unicode
       character name flexi-streams uses for flex:*substitution-char* (reached
       via clack's hunchentoot handler — a clog dependency).  The reader used
       to signal "Unknown character name: Replacement_Character". */
    CL_Obj obj = reads("#\\Replacement_Character");
    ASSERT(CL_CHAR_P(obj));
    ASSERT_EQ_INT(CL_CHAR_VAL(obj), 0xFFFD);
    /* Case-insensitive, like the other named chars. */
    obj = reads("#\\REPLACEMENT_CHARACTER");
    ASSERT(CL_CHAR_P(obj));
    ASSERT_EQ_INT(CL_CHAR_VAL(obj), 0xFFFD);
}

/* --- Keyword --- */

TEST(read_keyword)
{
    CL_Obj obj = reads(":test");
    CL_Symbol *sym;
    ASSERT(CL_SYMBOL_P(obj));
    sym = (CL_Symbol *)CL_OBJ_TO_PTR(obj);
    ASSERT_STR_EQ(cl_symbol_name(obj), "TEST");
    /* Keywords are self-evaluating */
    ASSERT_EQ(sym->value, obj);
}

/* --- Comments --- */

TEST(read_with_comment)
{
    CL_Obj obj = reads("; this is a comment\n42");
    ASSERT(CL_FIXNUM_P(obj));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(obj), 42);
}

/* --- Round-trip (read-print) --- */

TEST(roundtrip_integer)
{
    ASSERT_STR_EQ(read_print("42"), "42");
    ASSERT_STR_EQ(read_print("-7"), "-7");
    ASSERT_STR_EQ(read_print("0"), "0");
}

TEST(roundtrip_list)
{
    ASSERT_STR_EQ(read_print("(1 2 3)"), "(1 2 3)");
    ASSERT_STR_EQ(read_print("(a b c)"), "(A B C)");
}

TEST(roundtrip_string)
{
    ASSERT_STR_EQ(read_print("\"hello\""), "\"hello\"");
}

TEST(roundtrip_nil)
{
    ASSERT_STR_EQ(read_print("nil"), "NIL");
    ASSERT_STR_EQ(read_print("()"), "NIL");
}

TEST(roundtrip_dotted)
{
    ASSERT_STR_EQ(read_print("(a . b)"), "(A . B)");
}

/* --- Feature conditionals (#+ / #-) --- */

TEST(feature_plus_present)
{
    /* :CL-AMIGA is in *features*, so #+cl-amiga should include the form */
    CL_Obj obj = reads("#+cl-amiga 42");
    ASSERT(CL_FIXNUM_P(obj));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(obj), 42);
}

TEST(feature_names_old_and_new)
{
    /* The implementation's own features since the rename to AmiLisp
     * (0.12): :AMILISP and :AMICL are the new names, :CLAMIGA and
     * :CL-AMIGA the old ones -- all four are present, so a #+ written
     * against either generation includes the form. */
    static const char *const srcs[] = {
        "#+amilisp 1 99", "#+amicl 2 99", "#+clamiga 3 99", "#+cl-amiga 4 99",
        "#+(and amilisp amicl clamiga cl-amiga) 5 99"
    };
    int i;
    for (i = 0; i < 5; i++) {
        CL_Obj obj = reads(srcs[i]);
        ASSERT(CL_FIXNUM_P(obj));
        ASSERT_EQ_INT(CL_FIXNUM_VAL(obj), i + 1);
    }
}

TEST(feature_plus_absent)
{
    /* :NONEXISTENT is not in *features*, so form is skipped, next is read */
    CL_Obj obj = reads("#+nonexistent 42 99");
    ASSERT(CL_FIXNUM_P(obj));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(obj), 99);
}

TEST(feature_minus_present)
{
    /* #-cl-amiga: feature IS present, so form is SKIPPED */
    CL_Obj obj = reads("#-cl-amiga 42 99");
    ASSERT(CL_FIXNUM_P(obj));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(obj), 99);
}

TEST(feature_minus_absent)
{
    /* #-nonexistent: feature is NOT present, so form is INCLUDED */
    CL_Obj obj = reads("#-nonexistent 42");
    ASSERT(CL_FIXNUM_P(obj));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(obj), 42);
}

TEST(feature_plus_posix)
{
    /* On host, :POSIX should be in *features* */
    CL_Obj obj = reads("#+posix :yes");
    ASSERT(CL_SYMBOL_P(obj));
    ASSERT_STR_EQ(cl_symbol_name(obj), "YES");
}

TEST(feature_windows_only_on_windows)
{
    /* :WINDOWS tells the Windows build apart; it is :UNIX like the others */
    CL_Obj obj = reads("#+windows :yes #-windows :no");
    ASSERT(CL_SYMBOL_P(obj));
#ifdef PLATFORM_WIN32
    ASSERT_STR_EQ(cl_symbol_name(obj), "YES");
#else
    ASSERT_STR_EQ(cl_symbol_name(obj), "NO");
#endif
}

TEST(feature_plus_common_lisp)
{
    CL_Obj obj = reads("#+common-lisp :yes");
    ASSERT(CL_SYMBOL_P(obj));
    ASSERT_STR_EQ(cl_symbol_name(obj), "YES");
}

TEST(feature_in_list)
{
    /* Feature conditionals inside a list */
    ASSERT_STR_EQ(read_print("(1 #+cl-amiga 2 3)"), "(1 2 3)");
}

TEST(feature_skip_in_list)
{
    /* Skipped feature conditional inside a list */
    ASSERT_STR_EQ(read_print("(1 #+nonexistent 2 3)"), "(1 3)");
}

TEST(feature_skip_compound_form)
{
    /* Skipping a compound form (list), not just atom */
    ASSERT_STR_EQ(read_print("(1 #+nonexistent (a b c) 3)"), "(1 3)");
}

TEST(feature_and_expr)
{
    /* (:and :cl-amiga :posix) — both present on host */
    CL_Obj obj = reads("#+(and cl-amiga posix) 42 99");
    ASSERT(CL_FIXNUM_P(obj));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(obj), 42);
}

TEST(feature_and_expr_fail)
{
    /* (:and :cl-amiga :nonexistent) — one missing */
    CL_Obj obj = reads("#+(and cl-amiga nonexistent) 42 99");
    ASSERT(CL_FIXNUM_P(obj));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(obj), 99);
}

TEST(feature_or_expr)
{
    /* (:or :nonexistent :cl-amiga) — one present */
    CL_Obj obj = reads("#+(or nonexistent cl-amiga) 42 99");
    ASSERT(CL_FIXNUM_P(obj));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(obj), 42);
}

TEST(feature_or_expr_fail)
{
    /* (:or :nonexistent :also-nonexistent) — none present */
    CL_Obj obj = reads("#+(or nonexistent also-nonexistent) 42 99");
    ASSERT(CL_FIXNUM_P(obj));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(obj), 99);
}

TEST(feature_not_expr)
{
    /* (:not :nonexistent) — not present, so true */
    CL_Obj obj = reads("#+(not nonexistent) 42 99");
    ASSERT(CL_FIXNUM_P(obj));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(obj), 42);
}

TEST(feature_not_expr_fail)
{
    /* (:not :cl-amiga) — present, so false */
    CL_Obj obj = reads("#+(not cl-amiga) 42 99");
    ASSERT(CL_FIXNUM_P(obj));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(obj), 99);
}

TEST(features_is_list)
{
    /* *FEATURES* should be a non-nil list */
    CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_STAR_FEATURES);
    ASSERT(!CL_NULL_P(s->value));
    ASSERT(CL_CONS_P(s->value));
}

/* --- Read-suppress: skipped feature conditionals suppress errors --- */

TEST(feature_suppress_unknown_package)
{
    /* #+nonexistent should suppress "Package not found" error */
    CL_Obj obj = reads("#+nonexistent (unknown-pkg:symbol) 42");
    ASSERT(CL_FIXNUM_P(obj));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(obj), 42);
}

TEST(feature_suppress_internal_symbol)
{
    /* #+nonexistent should suppress pkg::internal access errors */
    CL_Obj obj = reads("#+nonexistent badpkg::internal 42");
    ASSERT(CL_FIXNUM_P(obj));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(obj), 42);
}

TEST(feature_suppress_unknown_char_name)
{
    /* #+nonexistent should suppress unknown character name errors */
    CL_Obj obj = reads("#+nonexistent #\\UnknownCharName 42");
    ASSERT(CL_FIXNUM_P(obj));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(obj), 42);
}

TEST(feature_suppress_nested)
{
    /* Nested `#+a #+b X Y` with BOTH features absent: the inner #+ skips X, and
     * the outer #+ must read PAST the empty inner to skip Y as well -- so BOTH
     * X and Y are dropped.  Verified against SBCL and CLISP: the bare top-level
     * "#+nonexistent #+also-nonexistent foo 42" form consumes foo AND 42 and
     * signals END-OF-FILE.  (The previous expectation of 42 here encoded the
     * old skip_form() under-skip bug.)  Use a list so the outcome is
     * observable: bar (inner) and baz (outer) are both dropped -> (FOO). */
    ASSERT_STR_EQ(read_print("(foo #+nonexistent #+also-nonexistent bar baz)"), "(FOO)");
}

TEST(feature_suppress_unknown_dispatch)
{
    /* #+nonexistent should suppress unknown dispatch macro */
    CL_Obj obj = reads("#+nonexistent #! 42");
    ASSERT(CL_FIXNUM_P(obj));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(obj), 42);
}

TEST(feature_suppress_nested_two_forms)
{
    /* Regression: the `#+a #+a x y` idiom (one #+ per form) must skip BOTH
     * x and y when the feature is absent.  The OUTER #+ has to skip a genuine
     * datum, reading PAST the inner #+ (which yields nothing) to discard `y`.
     * The old skip_form() read only once, stopping at the inner #+'s empty
     * result, so it left `y` behind -> `(A Y B)` instead of `(A B)`. */
    ASSERT_STR_EQ(read_print("(a #+nonexistent #+nonexistent :x y b)"), "(A B)");
}

TEST(feature_suppress_nested_two_forms_clwho)
{
    /* The exact cl-who construct that exposed the bug:
     *   (make-string total #+:lispworks #+:lispworks :element-type 'lw:simple-char)
     * Both the :element-type keyword AND the 'lw:simple-char (whose package LW
     * does not exist) live in the absent-feature branch, so the whole tail is
     * suppressed -> (MAKE-STRING TOTAL), with no "Package LW not found". */
    ASSERT_STR_EQ(
        read_print("(make-string total #+nonexistent #+nonexistent "
                   ":element-type 'lw:simple-char)"),
        "(MAKE-STRING TOTAL)");
}

TEST(feature_suppress_nested_toplevel_extra)
{
    /* Same under-skip at top level: outer #+ must consume past the inner #+
     * and the leftover token so the NEXT real form (99) is returned. */
    CL_Obj obj = reads("#+nonexistent #+nonexistent foo bar 99");
    ASSERT(CL_FIXNUM_P(obj));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(obj), 99);
}

TEST(feature_suppress_in_list)
{
    /* read-suppress inside a list with unknown package */
    ASSERT_STR_EQ(read_print("(1 #+nonexistent unknown-pkg:sym 3)"), "(1 3)");
}

/* --- *READ-SUPPRESS* (CLHS 2.4.8.17, 23.2) ---
 *
 * Regression: the reader tracked only its internal #+/#- skip counter and
 * ignored the *READ-SUPPRESS* variable entirely, so a suppressed read still
 * interned symbols, evaluated #., built #S structures, and signalled parse
 * errors — e.g. (let ((*read-suppress* t)) (read-from-string
 * "(a no-such-pkg:sym b)")) errored with "Package NO-SUCH-PKG not found"
 * instead of returning NIL. */

/* Read STR with *READ-SUPPRESS* bound true; reports the end position in
 * *end_pos when non-NULL (parsing must still consume the whole datum). */
static CL_Obj reads_suppressed(const char *str, int *end_pos)
{
    CL_ReadStream stream;
    CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_READ_SUPPRESS);
    CL_Obj saved = s->value;
    CL_Obj result;

    stream.buf = str;
    stream.pos = 0;
    stream.len = (int)strlen(str);
    stream.line = 1;

    s->value = CL_T;
    result = cl_read_from_string(&stream);
    /* Re-derive: the read may have compacted the heap and moved the symbol. */
    s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_READ_SUPPRESS);
    s->value = saved;

    if (end_pos) *end_pos = stream.pos;
    return result;
}

TEST(read_suppress_unknown_package_returns_nil)
{
    /* The reported bug: an unknown package qualifier must not signal. */
    int end = 0;
    CL_Obj obj = reads_suppressed("(a no-such-pkg:sym b)", &end);
    ASSERT(CL_NULL_P(obj));
    /* "parsing continues": the whole datum is consumed. */
    ASSERT_EQ_INT(end, 21);
}

TEST(read_suppress_ordinary_form_returns_nil)
{
    /* Not just the error paths — a suppressed read yields NIL for any datum. */
    ASSERT(CL_NULL_P(reads_suppressed("(a b c)", NULL)));
    ASSERT(CL_NULL_P(reads_suppressed("42", NULL)));
    ASSERT(CL_NULL_P(reads_suppressed("\"a string\"", NULL)));
    ASSERT(CL_NULL_P(reads_suppressed(":a-keyword", NULL)));
}

TEST(read_suppress_interns_no_symbol)
{
    /* "no symbol will be constructed or interned" — CLHS 2.4.8.17. */
    int status = 0;
    ASSERT(CL_NULL_P(reads_suppressed("(zzz-suppressed-never-interned)", NULL)));
    cl_find_symbol_with_status("ZZZ-SUPPRESSED-NEVER-INTERNED", 29,
                               cl_current_package, &status);
    ASSERT_EQ_INT(status, 0);  /* 0 = not found */
}

TEST(read_suppress_no_read_eval)
{
    /* #. must not evaluate while suppressed — the side effect would be
     * observable even though the value is discarded. */
    int end = 0;
    ASSERT(CL_NULL_P(reads_suppressed("#.(+ 1 2)", &end)));
    ASSERT_EQ_INT(end, 9);
}

TEST(read_suppress_no_struct_construction)
{
    /* #S must not look up MAKE-<type>; an undefined type is not an error. */
    ASSERT(CL_NULL_P(reads_suppressed("#S(no-such-struct :a 1)", NULL)));
}

TEST(read_suppress_restored_after_read)
{
    /* Suppression is scoped to the binding: normal reads still work after. */
    ASSERT(CL_NULL_P(reads_suppressed("(a b c)", NULL)));
    ASSERT_STR_EQ(read_print("(a b c)"), "(A B C)");
}

/* --- Read-time eval (#.) --- */

TEST(read_time_eval_arithmetic)
{
    /* #.(+ 1 2) should read as 3 */
    CL_Obj obj = reads("#.(+ 1 2)");
    ASSERT(CL_FIXNUM_P(obj));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(obj), 3);
}

TEST(read_time_eval_list)
{
    /* #.(list 1 2) should read as (1 2) */
    ASSERT_STR_EQ(read_print("#.(list 1 2)"), "(1 2)");
}

TEST(read_time_eval_in_list)
{
    /* #. inside a list */
    ASSERT_STR_EQ(read_print("(a #.(+ 10 20) b)"), "(A 30 B)");
}

/* --- #S(...) structure-literal reader (CLHS 2.4.8.13) --- */

TEST(read_struct_literal_basic)
{
    /* #S(type :slot value ...) constructs a struct via MAKE-<type>. */
    cl_eval_string("(defstruct sr-point x y)");
    ASSERT_STR_EQ(read_print("#S(SR-POINT :X 1 :Y 2)"), "#S(SR-POINT :X 1 :Y 2)");
}

TEST(read_struct_literal_default_slot)
{
    /* A slot omitted from the literal gets its defstruct default. */
    cl_eval_string("(defstruct sr-def (a 7) b)");
    ASSERT_STR_EQ(read_print("#S(SR-DEF :B 3)"), "#S(SR-DEF :A 7 :B 3)");
}

TEST(read_struct_literal_inherited)
{
    /* Included (inherited) slots are accepted by the literal. */
    cl_eval_string("(defstruct sr-base u v)");
    cl_eval_string("(defstruct (sr-sub (:include sr-base)) w)");
    ASSERT_STR_EQ(read_print("#S(SR-SUB :U 1 :V 2 :W 3)"),
                  "#S(SR-SUB :U 1 :V 2 :W 3)");
}

TEST(read_struct_literal_values_unevaluated)
{
    /* Slot values are read as literal objects, NOT evaluated: a symbol
     * value stays a symbol (this is what lets trivia treat #S slots as
     * sub-patterns). */
    cl_eval_string("(defstruct sr-lit s)");
    ASSERT_STR_EQ(read_print("#S(SR-LIT :S FOO)"), "#S(SR-LIT :S FOO)");
}

TEST(read_struct_literal_bare_symbol_slots)
{
    /* Slot names may be bare symbols (not keywords) per CLHS — coerced. */
    cl_eval_string("(defstruct sr-bare p q)");
    ASSERT_STR_EQ(read_print("#S(SR-BARE P 1 Q 2)"), "#S(SR-BARE :P 1 :Q 2)");
}

/* --- #nA multi-dimensional array reader --- */

TEST(read_2d_array)
{
    /* #2A((1 2) (3 4)) => 2x2 array */
    CL_Obj obj = reads("#2A((1 2) (3 4))");
    ASSERT(CL_VECTOR_P(obj));
    {
        CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(obj);
        ASSERT_EQ_INT(v->rank, 2);
        ASSERT_EQ_INT(v->length, 4);
        /* dims stored in data[0..rank-1] */
        ASSERT_EQ_INT(CL_FIXNUM_VAL(v->data[0]), 2);
        ASSERT_EQ_INT(CL_FIXNUM_VAL(v->data[1]), 2);
        /* elements at data[rank..] */
        ASSERT_EQ_INT(CL_FIXNUM_VAL(v->data[2]), 1);
        ASSERT_EQ_INT(CL_FIXNUM_VAL(v->data[3]), 2);
        ASSERT_EQ_INT(CL_FIXNUM_VAL(v->data[4]), 3);
        ASSERT_EQ_INT(CL_FIXNUM_VAL(v->data[5]), 4);
    }
}

TEST(read_2d_array_print)
{
    /* Verify printed form round-trips */
    ASSERT_STR_EQ(read_print("#2A((1 2) (3 4))"), "#2A((1 2) (3 4))");
}

TEST(read_2d_array_3x2)
{
    /* #2A((1 2) (3 4) (5 6)) => 3x2 array */
    CL_Obj obj = reads("#2A((1 2) (3 4) (5 6))");
    CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(obj);
    ASSERT_EQ_INT(v->rank, 2);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(v->data[0]), 3);  /* dim 0 */
    ASSERT_EQ_INT(CL_FIXNUM_VAL(v->data[1]), 2);  /* dim 1 */
    ASSERT_EQ_INT(CL_FIXNUM_VAL(v->data[2]), 1);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(v->data[7]), 6);
}

TEST(read_1d_array_reader)
{
    /* #1A(1 2 3) => same as #(1 2 3) */
    CL_Obj obj = reads("#1A(1 2 3)");
    ASSERT(CL_VECTOR_P(obj));
    {
        CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(obj);
        ASSERT_EQ_INT(v->length, 3);
    }
}

TEST(read_0d_array)
{
    /* #0A 42 => 0-dimensional array containing 42 */
    CL_Obj obj = reads("#0A 42");
    ASSERT(CL_VECTOR_P(obj));
    {
        CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(obj);
        ASSERT_EQ_INT(v->length, 1);
        ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_vector_data(v)[0]), 42);
    }
}

/* Regression: a dispatch-macro function that invokes a nested read on a
 * different stream (e.g. read-from-string inside the macro body) must not
 * leak the temporary stream back to the outer reader. Before the fix,
 * cl_read_from_stream overwrote reader_stream without saving it, so the
 * outer read saw EOF on the inner string stream and reported
 * "Unterminated list". */
TEST(nested_read_preserves_outer_stream)
{
    const char *setup_src =
        "(progn"
        "  (defun %test-sharpl (stream subchar n-args)"
        "    (declare (ignore subchar n-args))"
        "    (let ((form (read stream t nil t)))"
        /* Nested read on a temp string stream — this is what triggered the
         * bug in iterate's sharpL-reader via read-from-string. */
        "      (read-from-string \"99\")"
        "      (list 'quote form)))"
        "  (setf *readtable* (copy-readtable *readtable*))"
        "  (set-dispatch-macro-character #\\# #\\L #'%test-sharpl))";
    cl_eval_string(setup_src);

    /* This outer list must parse correctly despite the inner read. */
    {
        CL_Obj obj = reads("(a b #L(x y z) c)");
        char buf[128];
        cl_prin1_to_string(obj, buf, sizeof(buf));
        ASSERT_STR_EQ(buf, "(A B (QUOTE (X Y Z)) C)");
    }
}

/* READ-FROM-STRING conformance (CLHS): accepts :START/:END/
 * :PRESERVE-WHITESPACE keywords, returns the index of the first unread
 * character as a second value, and EOF-ERROR-P defaults to T.  uiop/asdf
 * call it with the full keyword form when reading .asd files — the old
 * (string &optional eof-error-p eof-value) signature broke loading
 * cserial-port.asd ("Too many arguments to READ-FROM-STRING"). */
TEST(read_from_string_keywords_and_position)
{
    /* "(a b) c", :start 1 :end 5 reads "A" starting at index 1, terminated
     * by the space at index 2 — CLHS 23.1.2: plain READ (PRESERVE-WHITESPACE
     * NIL, the default) consumes that single terminating whitespace
     * character, so the reported position is 3, not 2. */
    CL_Obj v = cl_eval_string(
        "(multiple-value-list (read-from-string \"(a b) c\" t nil :start 1 :end 5))");
    char buf[64];
    cl_prin1_to_string(v, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(A 3)");
}

TEST(read_from_string_position_default_range)
{
    /* "  foo  " reads FOO (indices 2-4), terminated by the space at index 5,
     * which plain READ consumes — position 6, not 5 (see CLHS 23.1.2). */
    CL_Obj v = cl_eval_string(
        "(multiple-value-list (read-from-string \"  foo  \"))");
    char buf[64];
    cl_prin1_to_string(v, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(FOO 6)");
}

TEST(read_from_string_eof_error_p_defaults_to_t)
{
    CL_Obj v = cl_eval_string(
        "(handler-case (read-from-string \"\") (end-of-file () :eof))");
    char buf[64];
    cl_prin1_to_string(v, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, ":EOF");
}

TEST(read_from_string_preserve_whitespace_accepted)
{
    CL_Obj v = cl_eval_string(
        "(read-from-string \"xy \" t nil :preserve-whitespace t)");
    char buf[64];
    cl_prin1_to_string(v, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "XY");
}

/* CLHS 23.1.2's own read-from-string example: (read-from-string "1 3 5")
 * => 1, 2 — plain READ consumes the single trailing whitespace character
 * that terminated the "1" token, so the position lands past the space. */
TEST(read_from_string_clhs_example_consumes_trailing_whitespace)
{
    CL_Obj v = cl_eval_string(
        "(multiple-value-list (read-from-string \"1 3 5\"))");
    char buf[64];
    cl_prin1_to_string(v, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(1 2)");
}

/* READ-PRESERVING-WHITESPACE must NOT consume the terminating whitespace
 * (CLHS 23.1.2) — contrast with plain READ above. */
TEST(read_preserving_whitespace_does_not_consume_terminator)
{
    CL_Obj v = cl_eval_string(
        "(let ((s (make-string-input-stream \"1 3 5\")))"
        "  (list (read-preserving-whitespace s) (file-position s)))");
    char buf[64];
    cl_prin1_to_string(v, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(1 1)");
}

/* The runtime itself must provide word-size and endianness features (as
 * SBCL/CCL do): CFFI sizes :SIZE/:SSIZE via #+64-bit / #+32-bit and babel
 * selects codecs via endianness.  Leaving these to a patched
 * trivial-features made (defctype :size #+64-bit :uint64 #+32-bit :uint32)
 * read as the malformed (defctype :size) on hosts whose trivial-features
 * predated the word-size patch (eta-hab on Debian arm64). */
TEST(features_word_size_and_endianness)
{
    CL_Obj v = cl_eval_string(
        "(list (if (member :64-bit *features*) 1 0)"
        "      (if (member :32-bit *features*) 1 0)"
        "      (if (member :little-endian *features*) 1 0)"
        "      (if (member :big-endian *features*) 1 0))");
    int f64 = (int)CL_FIXNUM_VAL(cl_car(v));
    int f32 = (int)CL_FIXNUM_VAL(cl_car(cl_cdr(v)));
    int fle = (int)CL_FIXNUM_VAL(cl_car(cl_cdr(cl_cdr(v))));
    int fbe = (int)CL_FIXNUM_VAL(cl_car(cl_cdr(cl_cdr(cl_cdr(v)))));
    /* Exactly one word-size feature and exactly one endianness feature. */
    ASSERT_EQ_INT(f64 + f32, 1);
    ASSERT_EQ_INT(fle + fbe, 1);
}

/* Regression: cl_read_from_stream must not reset the line counter to 1 on
 * every call; the stream's `line` field should persist so loaders see real
 * source-line numbers across top-level forms. */
TEST(stream_line_persists_across_reads)
{
    const char *src = "1\n2\n3\n";
    CL_Obj stream = cl_make_cbuf_input_stream(src, (uint32_t)strlen(src));
    CL_Stream *st;

    (void)cl_read_from_stream(stream);  /* reads "1" */
    (void)cl_read_from_stream(stream);  /* reads "2", consumes \n after "1" */
    st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
    ASSERT(st->line >= 2);

    (void)cl_read_from_stream(stream);  /* reads "3" */
    st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
    ASSERT(st->line >= 3);
}

TEST(read_2d_array_lowercase)
{
    /* #2a also works (lowercase) */
    CL_Obj obj = reads("#2a((10 20) (30 40))");
    ASSERT(CL_VECTOR_P(obj));
    {
        CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(obj);
        ASSERT_EQ_INT(v->rank, 2);
        ASSERT_EQ_INT(CL_FIXNUM_VAL(v->data[2]), 10);
        ASSERT_EQ_INT(CL_FIXNUM_VAL(v->data[5]), 40);
    }
}

/* --- #n= / #n# shared-and-circular-structure reader macros (CLHS 2.4.8.15-16) ---
 * Regression: jzon's jzon.lisp uses (macrolet ((#1=#:|| () ...)) ... (#1# ...))
 * which previously errored "Invalid radix prefix #1=", breaking the whole load. */

/* --- #n<char> user dispatch macro with an infix numeric arg ---
 * Regression: ironclad's readtable registers #@ via SET-DISPATCH-MACRO-CHARACTER
 * and writes s-box literals as #32@(...).  The reader previously parsed the 32
 * as a radix prefix and errored "Invalid radix prefix #32@" because it never
 * consulted the user dispatch table once digits had been seen. */
TEST(read_dispatch_macro_numeric_arg)
{
    cl_eval_string(
        "(set-dispatch-macro-character #\\# #\\@"
        "  (lambda (s c arg) (declare (ignore c)) (list arg (read s nil nil))))");
    /* #32@(...) calls the macro with arg=32 and the following list. */
    ASSERT_STR_EQ(read_print("#32@(1 2 3)"), "(32 (1 2 3))");
    /* No infix arg => arg is NIL (existing non-numeric dispatch path). */
    ASSERT_STR_EQ(read_print("#@(4 5)"), "(NIL (4 5))");
}

/* The C reader resolves the "current" readtable by dereferencing the
 * SYM_STAR_READTABLE global handle (resolve_readtable_idx / cl_readtable_
 * current), and a user dispatch macro installed on a *copied* readtable lives
 * in the readtable pool's dispatch_fn[] (a non-default slot reached only via
 * that handle).  This exercises that whole path across forced compactions:
 *   - the pool's macro_fn[]/dispatch_fn[] CL_Obj entries are GC-rooted and
 *     forwarded (mem.c gc_update_shared_roots) — if they weren't, the relocated
 *     ARRAY-READER closure would be swept or left stale and the read below would
 *     fault or error "Invalid radix prefix";
 *   - SYM_STAR_READTABLE is a registered global root so the handle is forwarded.
 *
 * NB on coverage: the bug that motivated the SYM_STAR_READTABLE registration
 * (ironclad's #n@ reader silently dropped after the *READTABLE* symbol relocated
 * — "Invalid radix prefix #32@", which blocked loading ironclad/chipi-api) only
 * fires when the *READTABLE* symbol itself moves to a new arena offset.  That
 * symbol is interned at boot and the C unit harness has only live boot data
 * below it, so cl_gc_compact() never slides it — identical to the relocation-
 * dependent rehash bugs documented in test_gc_rehash.c, which note the GC-stress
 * harness can't relocate a live boot object either.  The symbol-relocation
 * reproduction is therefore the ironclad/chipi-api integration load (heap-scale
 * fragmentation); this unit test guards the dispatch-resolution + pool-forwarding
 * half of the path. */
TEST(readtable_dispatch_survives_compaction)
{
    int i;

    /* Install a user dispatch macro on a *copy* of the readtable and make that
     * copy the current *READTABLE* — the exact ironclad shape. */
    cl_eval_string(
        "(progn"
        "  (setf *readtable* (copy-readtable *readtable*))"
        "  (set-dispatch-macro-character #\\# #\\@"
        "    (lambda (s c arg) (declare (ignore c)) (list arg (read s nil nil)))))");

    /* Sanity before any compaction. */
    ASSERT_STR_EQ(read_print("#7@(1 2)"), "(7 (1 2))");

    /* Force moving compactions: churn allocates garbage, a normal GC frees it,
     * then two forced compactions relocate the live set (incl. the pool's
     * ARRAY-READER closure). */
    for (i = 0; i < 800; i++)
        cl_cons(CL_MAKE_FIXNUM(i), CL_NIL);
    cl_eval_string("(dotimes (i 20000) (cons i i))");
    cl_gc();
    cl_gc_compact();
    cl_gc_compact();

    /* The dispatch macro must still resolve and its (relocated) closure run. */
    ASSERT_STR_EQ(read_print("#32@(1 2 3)"), "(32 (1 2 3))");
    ASSERT_STR_EQ(read_print("#@(4 5)"), "(NIL (4 5))");
}

TEST(read_label_value)
{
    /* #n=object evaluates to object */
    CL_Obj obj = reads("#1=42");
    ASSERT(CL_FIXNUM_P(obj));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(obj), 42);
}

TEST(read_label_shared)
{
    /* #1=#:foo ... #1# — both positions are the SAME (eq) object */
    CL_Obj obj = reads("(#1=#:foo #1#)");
    CL_Obj a, b;
    ASSERT(CL_CONS_P(obj));
    a = cl_car(obj);
    b = cl_car(cl_cdr(obj));
    ASSERT(CL_SYMBOL_P(a));
    ASSERT(a == b);                       /* shared identity preserved */
}

TEST(read_label_shared_multidigit)
{
    /* Multi-digit labels parse correctly */
    CL_Obj obj = reads("(#10=#:bar #10#)");
    ASSERT(CL_CONS_P(obj));
    ASSERT(cl_car(obj) == cl_car(cl_cdr(obj)));
}

TEST(read_label_circular_list)
{
    /* #1=(1 2 . #1#) — genuinely circular: the tail points back at the head */
    CL_Obj obj = reads("#1=(1 2 . #1#)");
    ASSERT(CL_CONS_P(obj));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(obj)), 1);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(cl_cdr(obj))), 2);
    /* (cddr obj) is spliced back to obj itself, not a leftover placeholder */
    ASSERT(cl_cdr(cl_cdr(obj)) == obj);
}

TEST(read_label_independent_per_read)
{
    /* Labels are scoped to one READ — reusing #1= in a second read is fine */
    CL_Obj a = reads("#1=(7)");
    CL_Obj b = reads("#1=(8)");
    ASSERT(CL_CONS_P(a) && CL_CONS_P(b));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(a)), 7);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(b)), 8);
}

TEST(read_label_nil_dotpair)
{
    /* #1=(nil . nil) must not be misidentified as the forward-ref placeholder.
     * Regression for the CL_READER_SKIP sentinel fix: the old code checked
     * car==NIL && cdr==NIL, which matched a real (nil . nil) value and
     * incorrectly triggered reader_patch_labels on a non-circular structure. */
    CL_Obj obj = reads("(#1=(nil . nil) #1#)");
    CL_Obj a, b;
    ASSERT(CL_CONS_P(obj));
    a = cl_car(obj);          /* the labeled (nil . nil) cons */
    b = cl_car(cl_cdr(obj));  /* #1# — same cons, shared identity */
    ASSERT(CL_CONS_P(a));
    ASSERT(CL_NULL_P(cl_car(a)));
    ASSERT(CL_NULL_P(cl_cdr(a)));
    ASSERT(a == b);           /* shared, not patched incorrectly */
}

/* --- tier-4 batch 7b: token caps signal reader errors, not silent splits --- */

static int read_signals_error(const char *str)
{
    int err;
    CL_CATCH(err);
    if (err == CL_ERR_NONE) {
        reads(str);
        CL_UNCATCH();
        return 0;
    }
    CL_UNCATCH();
    return 1;
}

TEST(long_symbol_token_errors)
{
    /* A >255-char token used to stop consuming at 255 and silently split
     * into TWO tokens.  Now a reader error. */
    char buf[512];
    memset(buf, 'A', 300);
    buf[300] = '\0';
    ASSERT_EQ_INT(read_signals_error(buf), 1);
    /* Exactly 255 chars still reads fine. */
    memset(buf, 'B', 255);
    buf[255] = '\0';
    ASSERT_EQ_INT(read_signals_error(buf), 0);
}

TEST(long_number_token_errors)
{
    /* The infamous case: a 300-digit integer parsed as TWO bignums. */
    char buf[512];
    memset(buf, '5', 300);
    buf[300] = '\0';
    ASSERT_EQ_INT(read_signals_error(buf), 1);
    /* #x-radix numbers share the cap. */
    buf[0] = '#'; buf[1] = 'x';
    memset(buf + 2, '1', 300);
    buf[302] = '\0';
    ASSERT_EQ_INT(read_signals_error(buf), 1);
}

TEST(long_string_literal_grows)
{
    /* >4095-byte string literals used to silently truncate at 4095 (and an
     * intermediate fix errored, which broke loading log4cl — its
     * pattern-layout docstring is >4KB and CLHS puts no limit on string
     * literals).  The reader buffer grows: the full literal round-trips. */
    char *buf = (char *)malloc(20050);
    CL_Obj str;
    buf[0] = '"';
    memset(buf + 1, 'x', 20000);
    buf[20001] = '"';
    buf[20002] = '\0';
    str = reads(buf);
    free(buf);
    ASSERT(CL_ANY_STRING_P(str));
    ASSERT_EQ_INT((int)cl_string_length(str), 20000);
    ASSERT_EQ_INT(cl_string_char_at(str, 19999), 'x');
}

TEST(string_literal_growth_survives_socket_read_timeout)
{
    /* read_char() can longjmp out from underneath read_string() when the
     * underlying stream is a socket whose read deadline elapses mid-literal
     * (stream_raise_timeout) -- bypassing every explicit free in the
     * heap-growth path above and leaking the >4096-byte buffer.  Drive a
     * real socket timeout while a literal is heap-grown, then prove the
     * reader still works correctly afterward: the next read_string call on
     * this thread must reclaim the orphaned buffer instead of leaking it
     * (or corrupting state) on every subsequent aborted read. */
    int bound_port = 0;
    CL_Obj listener = cl_make_listen_stream(0, 1, &bound_port);
    CL_Obj client, conn;
    char chunk[5000];
    int err;
    char *buf2;
    CL_Obj str;

    ASSERT(!CL_NULL_P(listener));
    ASSERT(bound_port > 0);

    client = cl_make_socket_stream("127.0.0.1", bound_port, 0);
    ASSERT(!CL_NULL_P(client));
    conn = cl_socket_stream_accept(listener);
    ASSERT(!CL_NULL_P(conn));

    /* Opening quote + >4096 bytes forces read_string's heap-growth path;
     * the closing quote is never sent, so the client's next read blocks
     * until the timeout below fires. */
    memset(chunk, 'x', sizeof(chunk));
    cl_stream_write_char(conn, '"');
    cl_stream_write_string(conn, chunk, (uint32_t)sizeof(chunk));

    cl_socket_stream_set_timeout(client, 0, 100); /* 100ms read timeout */

    CL_CATCH(err);
    if (err == CL_ERR_NONE) {
        cl_read_from_stream(client);
        CL_UNCATCH();
        ASSERT(0); /* expected a socket read timeout */
    } else {
        CL_UNCATCH();
        ASSERT_EQ_INT(err, CL_ERR_TIMEOUT);
    }

    cl_stream_close(conn);
    cl_stream_close(client);
    cl_stream_close(listener);

    /* Same thread, fresh literal: must round-trip cleanly, proving the
     * per-thread orphan slot reclaimed the leaked buffer rather than
     * leaving the reader (or the heap) corrupted. */
    buf2 = (char *)malloc(5010);
    buf2[0] = '"';
    memset(buf2 + 1, 'y', 5000);
    buf2[5001] = '"';
    buf2[5002] = '\0';
    str = reads(buf2);
    free(buf2);
    ASSERT(CL_ANY_STRING_P(str));
    ASSERT_EQ_INT((int)cl_string_length(str), 5000);
    ASSERT_EQ_INT(cl_string_char_at(str, 4999), 'y');
}

TEST(long_bitvector_literal_errors)
{
    /* #* bits past 4095 used to be silently dropped. */
    char *buf = (char *)malloc(5210);
    int r;
    buf[0] = '#'; buf[1] = '*';
    memset(buf + 2, '1', 5000);
    buf[5002] = '\0';
    r = read_signals_error(buf);
    free(buf);
    ASSERT_EQ_INT(r, 1);
}

TEST(long_char_name_errors)
{
    /* #\LongName past 31 chars used to split into a char + a symbol. */
    char buf[64];
    buf[0] = '#'; buf[1] = '\\';
    memset(buf + 2, 'q', 40);
    buf[42] = '\0';
    ASSERT_EQ_INT(read_signals_error(buf), 1);
    ASSERT_EQ_INT(read_signals_error("#\\Newline"), 0);
}

int main(void)
{
    test_init();
    setup();

    RUN(read_integer);
    RUN(read_negative_integer);
    RUN(read_zero);
    RUN(read_symbol);
    RUN(read_nil);
    RUN(read_t);
    RUN(read_string);
    RUN(read_string_escape);
    RUN(read_string_literal_newline);
    RUN(read_empty_list);
    RUN(read_list);
    RUN(read_nested_list);
    RUN(read_dotted_pair);
    RUN(read_quote);
    RUN(read_function_shorthand);
    RUN(read_char_literal);
    RUN(read_char_space);
    RUN(read_char_newline);
    RUN(read_char_replacement_character);
    RUN(read_keyword);
    RUN(read_with_comment);
    RUN(roundtrip_integer);
    RUN(roundtrip_list);
    RUN(roundtrip_string);
    RUN(roundtrip_nil);
    RUN(roundtrip_dotted);

    /* Feature conditionals */
    RUN(feature_plus_present);
    RUN(feature_names_old_and_new);
    RUN(feature_plus_absent);
    RUN(feature_minus_present);
    RUN(feature_minus_absent);
    RUN(feature_plus_posix);
    RUN(feature_windows_only_on_windows);
    RUN(feature_plus_common_lisp);
    RUN(feature_in_list);
    RUN(feature_skip_in_list);
    RUN(feature_skip_compound_form);
    RUN(feature_and_expr);
    RUN(feature_and_expr_fail);
    RUN(feature_or_expr);
    RUN(feature_or_expr_fail);
    RUN(feature_not_expr);
    RUN(feature_not_expr_fail);
    RUN(features_is_list);

    /* Read-suppress tests */
    RUN(feature_suppress_unknown_package);
    RUN(feature_suppress_internal_symbol);
    RUN(feature_suppress_unknown_char_name);
    RUN(feature_suppress_nested);
    RUN(feature_suppress_nested_two_forms);
    RUN(feature_suppress_nested_two_forms_clwho);
    RUN(feature_suppress_nested_toplevel_extra);
    RUN(feature_suppress_unknown_dispatch);
    RUN(feature_suppress_in_list);
    RUN(read_suppress_unknown_package_returns_nil);
    RUN(read_suppress_ordinary_form_returns_nil);
    RUN(read_suppress_interns_no_symbol);
    RUN(read_suppress_no_read_eval);
    RUN(read_suppress_no_struct_construction);
    RUN(read_suppress_restored_after_read);

    /* Read-time eval */
    RUN(read_time_eval_arithmetic);
    RUN(read_time_eval_list);
    RUN(read_time_eval_in_list);

    /* #S(...) structure-literal reader */
    RUN(read_struct_literal_basic);
    RUN(read_struct_literal_default_slot);
    RUN(read_struct_literal_inherited);
    RUN(read_struct_literal_values_unevaluated);
    RUN(read_struct_literal_bare_symbol_slots);

    /* #nA multi-dimensional array reader */
    RUN(read_2d_array);
    RUN(read_2d_array_print);
    RUN(read_2d_array_3x2);
    RUN(read_1d_array_reader);
    RUN(read_0d_array);
    RUN(read_2d_array_lowercase);

    /* #n= / #n# shared-and-circular-structure labels */
    RUN(read_dispatch_macro_numeric_arg);
    RUN(readtable_dispatch_survives_compaction);
    RUN(read_label_value);
    RUN(read_label_shared);
    RUN(read_label_shared_multidigit);
    RUN(read_label_circular_list);
    RUN(read_label_independent_per_read);
    RUN(read_label_nil_dotpair);

    RUN(long_symbol_token_errors);
    RUN(long_number_token_errors);
    RUN(long_string_literal_grows);
    RUN(string_literal_growth_survives_socket_read_timeout);
    RUN(long_bitvector_literal_errors);
    RUN(long_char_name_errors);

    /* READ-FROM-STRING conformance + runtime-provided features */
    RUN(read_from_string_keywords_and_position);
    RUN(read_from_string_position_default_range);
    RUN(read_from_string_eof_error_p_defaults_to_t);
    RUN(read_from_string_preserve_whitespace_accepted);
    RUN(read_from_string_clhs_example_consumes_trailing_whitespace);
    RUN(read_preserving_whitespace_does_not_consume_terminator);
    RUN(features_word_size_and_endianness);

    /* Regression: nested reader invocation must not clobber outer stream */
    RUN(nested_read_preserves_outer_stream);

    /* Regression: per-stream line counter persists across reads */
    RUN(stream_line_persists_across_reads);

    teardown();
    REPORT();
}
