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
#include "core/debugger.h"
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
    cl_debugger_init();
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
        /* Reset VM state after error (prevent stale frames) */
        cl_vm.sp = 0;
        cl_vm.fp = 0;
        snprintf(buf, sizeof(buf), "ERROR:%d", err);
        return buf;
    }
}

/* --- C-level tests --- */

TEST(c_make_condition)
{
    CL_Obj cond = cl_make_condition(SYM_TYPE_ERROR, CL_NIL, CL_NIL);
    ASSERT(CL_CONDITION_P(cond));
    ASSERT(!CL_NULL_P(cond));

    {
        CL_Condition *c = (CL_Condition *)CL_OBJ_TO_PTR(cond);
        ASSERT_EQ(c->type_name, SYM_TYPE_ERROR);
        ASSERT_EQ(c->slots, CL_NIL);
        ASSERT_EQ(c->report_string, CL_NIL);
    }
}

TEST(c_condition_p)
{
    CL_Obj cond = cl_make_condition(SYM_SIMPLE_ERROR, CL_NIL, CL_NIL);
    ASSERT(CL_CONDITION_P(cond));

    /* Non-conditions */
    ASSERT(!CL_CONDITION_P(CL_NIL));
    ASSERT(!CL_CONDITION_P(CL_MAKE_FIXNUM(42)));
    ASSERT(!CL_CONDITION_P(SYM_T));
}

TEST(c_condition_with_slots)
{
    CL_Obj slots, pair1, pair2, cond;
    CL_Obj report;

    report = cl_make_string("test error", 10);
    CL_GC_PROTECT(report);

    pair1 = cl_cons(KW_DATUM, CL_MAKE_FIXNUM(42));
    CL_GC_PROTECT(pair1);

    pair2 = cl_cons(KW_EXPECTED_TYPE, SYM_T);
    CL_GC_PROTECT(pair2);

    slots = cl_cons(pair1, cl_cons(pair2, CL_NIL));
    CL_GC_PROTECT(slots);

    cond = cl_make_condition(SYM_TYPE_ERROR, slots, report);
    CL_GC_UNPROTECT(4);

    ASSERT(CL_CONDITION_P(cond));
    {
        CL_Condition *c = (CL_Condition *)CL_OBJ_TO_PTR(cond);
        ASSERT_EQ(c->type_name, SYM_TYPE_ERROR);
        ASSERT(!CL_NULL_P(c->slots));
        ASSERT_EQ(c->report_string, report);
    }
}

/* --- C-level hierarchy test --- */

TEST(c_hierarchy_matches)
{
    extern int cl_condition_type_matches(CL_Obj cond_type, CL_Obj handler_type);

    /* TYPE-ERROR matches itself */
    ASSERT(cl_condition_type_matches(SYM_TYPE_ERROR, SYM_TYPE_ERROR));

    /* TYPE-ERROR matches ERROR, SERIOUS-CONDITION, CONDITION */
    ASSERT(cl_condition_type_matches(SYM_TYPE_ERROR, SYM_ERROR_COND));
    ASSERT(cl_condition_type_matches(SYM_TYPE_ERROR, SYM_SERIOUS_CONDITION));
    ASSERT(cl_condition_type_matches(SYM_TYPE_ERROR, SYM_CONDITION));

    /* TYPE-ERROR does NOT match WARNING */
    ASSERT(!cl_condition_type_matches(SYM_TYPE_ERROR, SYM_WARNING));

    /* SIMPLE-ERROR matches both ERROR and SIMPLE-CONDITION */
    ASSERT(cl_condition_type_matches(SYM_SIMPLE_ERROR, SYM_ERROR_COND));
    ASSERT(cl_condition_type_matches(SYM_SIMPLE_ERROR, SYM_SIMPLE_CONDITION));
    ASSERT(cl_condition_type_matches(SYM_SIMPLE_ERROR, SYM_CONDITION));

    /* SIMPLE-WARNING matches both WARNING and SIMPLE-CONDITION */
    ASSERT(cl_condition_type_matches(SYM_SIMPLE_WARNING, SYM_WARNING));
    ASSERT(cl_condition_type_matches(SYM_SIMPLE_WARNING, SYM_SIMPLE_CONDITION));

    /* DIVISION-BY-ZERO matches ARITHMETIC-ERROR, ERROR, CONDITION */
    ASSERT(cl_condition_type_matches(SYM_DIVISION_BY_ZERO, SYM_ARITHMETIC_ERROR));
    ASSERT(cl_condition_type_matches(SYM_DIVISION_BY_ZERO, SYM_ERROR_COND));
    ASSERT(cl_condition_type_matches(SYM_DIVISION_BY_ZERO, SYM_CONDITION));

    /* WARNING does NOT match ERROR */
    ASSERT(!cl_condition_type_matches(SYM_WARNING, SYM_ERROR_COND));

    /* CONDITION does NOT match ERROR */
    ASSERT(!cl_condition_type_matches(SYM_CONDITION, SYM_ERROR_COND));
}

/* --- C-level signal tests --- */

TEST(c_signal_no_handlers)
{
    /* cl_signal_condition with empty handler stack returns NIL */
    CL_Obj cond = cl_make_condition(SYM_SIMPLE_CONDITION, CL_NIL, CL_NIL);
    CL_Obj result = cl_signal_condition(cond);
    ASSERT_EQ(result, CL_NIL);
}

TEST(c_error_creates_condition)
{
    /* Verify cl_error() still recovers via CL_CATCH (backward compat) */
    int err; CL_CATCH(err);
    if (err == CL_ERR_NONE) {
        cl_error(CL_ERR_GENERAL, "test backward compat");
        CL_UNCATCH();
        ASSERT(0);  /* Should not reach here */
    } else {
        CL_UNCATCH();
        ASSERT_EQ_INT(err, CL_ERR_GENERAL);
    }
}

TEST(c_create_condition_from_error)
{
    CL_Obj cond;
    CL_Condition *c;

    cond = cl_create_condition_from_error(CL_ERR_TYPE, "type mismatch");
    ASSERT(CL_CONDITION_P(cond));
    c = (CL_Condition *)CL_OBJ_TO_PTR(cond);
    ASSERT_EQ(c->type_name, SYM_TYPE_ERROR);
    ASSERT(!CL_NULL_P(c->report_string));

    cond = cl_create_condition_from_error(CL_ERR_DIVZERO, "div by zero");
    c = (CL_Condition *)CL_OBJ_TO_PTR(cond);
    ASSERT_EQ(c->type_name, SYM_DIVISION_BY_ZERO);

    cond = cl_create_condition_from_error(CL_ERR_UNBOUND, "unbound");
    c = (CL_Condition *)CL_OBJ_TO_PTR(cond);
    ASSERT_EQ(c->type_name, SYM_UNBOUND_VARIABLE_COND);

    cond = cl_create_condition_from_error(CL_ERR_UNDEFINED, "undef");
    c = (CL_Condition *)CL_OBJ_TO_PTR(cond);
    ASSERT_EQ(c->type_name, SYM_UNDEFINED_FUNCTION_COND);

    cond = cl_create_condition_from_error(CL_ERR_ARGS, "bad args");
    c = (CL_Condition *)CL_OBJ_TO_PTR(cond);
    ASSERT_EQ(c->type_name, SYM_PROGRAM_ERROR);

    cond = cl_create_condition_from_error(CL_ERR_GENERAL, "generic");
    c = (CL_Condition *)CL_OBJ_TO_PTR(cond);
    ASSERT_EQ(c->type_name, SYM_SIMPLE_ERROR);
}

TEST(c_error_print_goes_to_error_output)
{
    const char *out;

    /* Redirect *error-output* to a fresh string output stream */
    eval_print("(defparameter *test-saved-eo* *error-output*)");
    eval_print("(setf *error-output* (make-string-output-stream))");

    /* Set error state directly and call cl_error_print() from C */
    snprintf(cl_error_msg, sizeof(cl_error_msg), "frobnicate failed");
    cl_backtrace_buf[0] = '\0';
    cl_error_print();

    /* Retrieve captured output and verify it went to *error-output* */
    out = eval_print("(get-output-stream-string *error-output*)");
    ASSERT(strstr(out, "ERROR:") != NULL);
    ASSERT(strstr(out, "frobnicate failed") != NULL);

    /* Restore *error-output* */
    eval_print("(setf *error-output* *test-saved-eo*)");
}

TEST(c_handler_stack_nlx)
{
    /* Push a handler, set up NLX frame, verify handler_top restored after NLX */

    cl_handler_top = 0;
    cl_handler_stack[0].type_name = SYM_CONDITION;
    cl_handler_stack[0].handler = CL_NIL;
    cl_handler_stack[0].handler_mark = 0;
    cl_handler_top = 1;

    {
        int err; CL_CATCH(err);
        if (err == CL_ERR_NONE) {
            /* handler_top should be 1 here */
            ASSERT_EQ_INT(cl_handler_top, 1);
            cl_error(CL_ERR_GENERAL, "test nlx");
            CL_UNCATCH();
        } else {
            CL_UNCATCH();
            /* After error recovery, handler_top should be reset to 0 */
            ASSERT_EQ_INT(cl_handler_top, 0);
        }
    }
}

/* --- Lisp-level tests --- */

TEST(lisp_conditionp)
{
    ASSERT_STR_EQ(eval_print("(conditionp (make-condition 'simple-error :format-control \"test\"))"), "T");
    ASSERT_STR_EQ(eval_print("(conditionp 42)"), "NIL");
    ASSERT_STR_EQ(eval_print("(conditionp nil)"), "NIL");
    ASSERT_STR_EQ(eval_print("(conditionp \"hello\")"), "NIL");
}

TEST(lisp_make_condition)
{
    ASSERT_STR_EQ(eval_print("(conditionp (make-condition 'type-error :datum 42 :expected-type 'string))"), "T");
    ASSERT_STR_EQ(eval_print("(conditionp (make-condition 'warning))"), "T");
    ASSERT_STR_EQ(eval_print("(conditionp (make-condition 'condition))"), "T");
}

TEST(lisp_condition_type_name)
{
    ASSERT_STR_EQ(eval_print("(condition-type-name (make-condition 'type-error))"), "TYPE-ERROR");
    ASSERT_STR_EQ(eval_print("(condition-type-name (make-condition 'simple-error))"), "SIMPLE-ERROR");
    ASSERT_STR_EQ(eval_print("(condition-type-name (make-condition 'warning))"), "WARNING");
}

TEST(lisp_type_error_accessors)
{
    ASSERT_STR_EQ(eval_print("(type-error-datum (make-condition 'type-error :datum 42))"), "42");
    ASSERT_STR_EQ(eval_print("(type-error-expected-type (make-condition 'type-error :expected-type 'string))"), "STRING");
    /* Missing slot returns NIL */
    ASSERT_STR_EQ(eval_print("(type-error-datum (make-condition 'type-error))"), "NIL");
}

TEST(lisp_simple_condition_accessors)
{
    ASSERT_STR_EQ(eval_print("(simple-condition-format-control (make-condition 'simple-error :format-control \"oops\"))"), "\"oops\"");
    ASSERT_STR_EQ(eval_print("(simple-condition-format-arguments (make-condition 'simple-error :format-arguments '(1 2)))"), "(1 2)");
}

TEST(lisp_type_of)
{
    ASSERT_STR_EQ(eval_print("(type-of (make-condition 'type-error))"), "TYPE-ERROR");
    ASSERT_STR_EQ(eval_print("(type-of (make-condition 'simple-error))"), "SIMPLE-ERROR");
    ASSERT_STR_EQ(eval_print("(type-of (make-condition 'warning))"), "WARNING");
}

TEST(lisp_typep_condition)
{
    /* Basic typep */
    ASSERT_STR_EQ(eval_print("(typep (make-condition 'type-error) 'condition)"), "T");
    ASSERT_STR_EQ(eval_print("(typep (make-condition 'type-error) 'error)"), "T");
    ASSERT_STR_EQ(eval_print("(typep (make-condition 'type-error) 'serious-condition)"), "T");
    ASSERT_STR_EQ(eval_print("(typep (make-condition 'type-error) 'type-error)"), "T");

    /* Negative cases */
    ASSERT_STR_EQ(eval_print("(typep (make-condition 'type-error) 'warning)"), "NIL");
    ASSERT_STR_EQ(eval_print("(typep (make-condition 'warning) 'error)"), "NIL");

    /* Warning hierarchy */
    ASSERT_STR_EQ(eval_print("(typep (make-condition 'warning) 'condition)"), "T");
    ASSERT_STR_EQ(eval_print("(typep (make-condition 'simple-warning) 'warning)"), "T");
    ASSERT_STR_EQ(eval_print("(typep (make-condition 'simple-warning) 'simple-condition)"), "T");

    /* Non-condition doesn't match */
    ASSERT_STR_EQ(eval_print("(typep 42 'condition)"), "NIL");
}

/* Regression: a condition whose ERROR superclass is NOT the first parent must
 * still be recognised as an ERROR by typep / subtypep / handler-case.  This
 * mirrors usocket's (define-condition socket-error (socket-condition error));
 * previously only the first parent was registered, so the condition escaped
 * (error ...) handlers and chipi's InfluxDB fetch future never completed. */
TEST(lisp_condition_multiple_inheritance)
{
    eval_print("(define-condition mi-base (condition) ())");
    eval_print("(define-condition mi-err (mi-base error) ())");      /* error 2nd */
    eval_print("(define-condition mi-sub (mi-err) ())");             /* inherits */

    ASSERT_STR_EQ(eval_print("(typep (make-condition 'mi-err) 'error)"), "T");
    ASSERT_STR_EQ(eval_print("(typep (make-condition 'mi-err) 'mi-base)"), "T");
    ASSERT_STR_EQ(eval_print("(typep (make-condition 'mi-sub) 'error)"), "T");
    ASSERT_STR_EQ(eval_print("(typep (make-condition 'mi-sub) 'mi-base)"), "T");

    ASSERT_STR_EQ(eval_print("(subtypep 'mi-err 'error)"), "T");
    ASSERT_STR_EQ(eval_print("(subtypep 'mi-sub 'error)"), "T");
    ASSERT_STR_EQ(eval_print("(subtypep 'mi-err 'mi-base)"), "T");

    /* handler-case must dispatch to the (error ...) clause, not fall through */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (error 'mi-err) (error () :as-error) (condition () :as-cond))"),
        ":AS-ERROR");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (error 'mi-sub) (error () :as-error) (condition () :as-cond))"),
        ":AS-ERROR");

    /* must NOT spuriously widen: a pure non-error condition stays non-error */
    ASSERT_STR_EQ(eval_print("(subtypep 'mi-base 'error)"), "NIL");
    ASSERT_STR_EQ(eval_print("(typep (make-condition 'mi-base) 'error)"), "NIL");
}

TEST(lisp_printer)
{
    /* Condition without report_string */
    ASSERT_STR_EQ(eval_print("(make-condition 'type-error :datum 42)"), "#<CONDITION TYPE-ERROR>");

    /* Condition with report_string */
    ASSERT_STR_EQ(eval_print("(make-condition 'simple-error :format-control \"bad thing\")"),
                  "#<CONDITION SIMPLE-ERROR: \"bad thing\">");
}

TEST(lisp_printer_aesthetic_uses_report)
{
    /* CLHS 9.1.3: PRINC / ~A on a condition prints just the report
     * (the human-readable message), not the #<CONDITION ...> wrapper.
     * Sento's actor-cell-test::error-in-handler relies on
     *   (string= "Foo Error" (format nil "~a" cond))
     * which only holds when ~A drops the wrapper. */
    ASSERT_STR_EQ(eval_print(
        "(format nil \"~a\" (make-condition 'simple-error :format-control \"Foo Error\"))"),
        "\"Foo Error\"");
    /* PRIN1 / ~S keeps the readable wrapper. */
    ASSERT_STR_EQ(eval_print(
        "(format nil \"~s\" (make-condition 'simple-error :format-control \"Foo Error\"))"),
        "\"#<CONDITION SIMPLE-ERROR: \\\"Foo Error\\\">\"");
    /* No report_string, but a standard type ⇒ ~A reports from the slots
     * (the wrapper is only for a type with nothing to say, see
     * lisp_cell_error_family_default_report). */
    ASSERT_STR_EQ(eval_print(
        "(format nil \"~a\" (make-condition 'type-error :datum 42))"),
        "\"The value 42 is not of type NIL\"");
    /* A make-condition'd simple condition fills its ~a directives under
     * ~A (only ERROR used to format them). */
    ASSERT_STR_EQ(eval_print(
        "(format nil \"~a\" (make-condition 'simple-error"
        " :format-control \"Foo ~a\" :format-arguments '(7)))"),
        "\"Foo 7\"");
}

#ifdef CL_WIDE_STRINGS
/* --- Wide-string condition reports ---
 *
 * A report holding any non-ASCII character travels as a
 * TYPE_WIDE_STRING (cl_get_output_stream_string widens on the first
 * byte > 0x7F), and the whole report path once handled only
 * TYPE_STRING: the designator was rejected, the formatted report
 * discarded, the :format-control never captured, and the printer
 * emitted an empty message.  Wide data is built with (code-char N) —
 * never a UTF-8 source literal, which --eval/stdin would
 * double-encode (a separate, pre-existing reader-input bug).  The
 * direct (unformatted) paths use (code-char 8364), a code point no
 * base string can hold, so the string is wide by construction; the
 * formatted paths widen on any non-ASCII output byte either way. */

TEST(lisp_wide_string_error_designator)
{
    /* coerce_to_condition: (error "…€") signaled "Expected condition,
     * symbol, or string" instead of the condition itself. */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (error (concatenate 'string \"kaputt: \""
        "                                  (string (code-char 8364))))"
        "  (error (c) (search \"kaputt: \" (princ-to-string c))))"),
        "0");
}

TEST(lisp_wide_formatted_report)
{
    /* bi_error: the formatted report came back wide and was dropped —
     * the condition reported the raw control \"value: ~A\".  The report
     * must carry the interpolated text AND the actual code point. */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (error \"value: ~A\" (string (code-char 252)))"
        "  (error (c) (let ((r (princ-to-string c)))"
        "               (list (search \"value: \" r)"
        "                     (char-code (char r 7))))))"),
        "(0 252)");
}

TEST(lisp_wide_format_control_captured)
{
    /* make-condition / coerce_to_condition (symbol form): a wide
     * :format-control never reached report_string — the condition
     * printed as #<CONDITION SIMPLE-ERROR> with no message. */
    ASSERT_STR_EQ(eval_print(
        "(search \"gr\" (princ-to-string"
        "  (make-condition 'simple-error :format-control"
        "    (concatenate 'string \"gr\" (string (code-char 8364)) \"sse\"))))"),
        "0");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (error 'simple-error :format-control"
        "                     (concatenate 'string \"weit\" (string (code-char 8364))))"
        "  (error (c) (search \"weit\" (princ-to-string c))))"),
        "0");
}

TEST(lisp_wide_default_initarg_control)
{
    /* merge_default_initargs: a wide :format-control from
     * :default-initargs was skipped the same way.  #. builds the wide
     * literal at read time — the default-initargs table stores values
     * verbatim, not evaluated forms. */
    eval_print(
        "(define-condition wide-di-err (error) ()"
        "  (:default-initargs :format-control"
        "    #.(concatenate 'string \"vorgabe: \" (string (code-char 8364)))))");
    ASSERT_STR_EQ(eval_print(
        "(search \"vorgabe: \" (princ-to-string (make-condition 'wide-di-err)))"),
        "0");
}

TEST(lisp_wide_report_prin1_wrapper)
{
    /* printer.c: out_str_lisp returned silently on a wide report — the
     * ~S wrapper printed #<CONDITION SIMPLE-ERROR: \"\"> with the
     * message gone. */
    ASSERT_STR_EQ(eval_print(
        "(let ((r (format nil \"~s\" (make-condition 'simple-error"
        "            :format-control (concatenate 'string \"bad \""
        "                              (string (code-char 8364)))))))"
        "  (list (and (search \"#<CONDITION SIMPLE-ERROR\" r) t)"
        "        (and (search \"bad \" r) t)))"),
        "(T T)");
}

TEST(lisp_wide_warn_report)
{
    /* bi_warn's no-handler printout degraded a wide report to the bare
     * type name. */
    const char *out;
    eval_print("(defparameter *test-saved-eo* *error-output*)");
    eval_print("(setf *error-output* (make-string-output-stream))");
    eval_print("(warn \"achtung: ~A\" (string (code-char 252)))");
    out = eval_print("(get-output-stream-string *error-output*)");
    ASSERT(strstr(out, "achtung: ") != NULL);
    ASSERT(strstr(out, "SIMPLE-WARNING") == NULL);
    eval_print("(setf *error-output* *test-saved-eo*)");
}

TEST(c_wide_report_reaches_error_msg)
{
    /* error.c cl_error_from_condition: a wide report degraded
     * cl_error_msg to the bare type name (and %s on CL_String->data
     * would read UTF-32 units as bytes).  Signal uncaught with the
     * debugger disabled and inspect the captured message. */
    int err;
    cl_debugger_enabled = 0;
    CL_CATCH(err);
    if (err == CL_ERR_NONE) {
        cl_eval_string(
            "(error \"gross: ~A\" (string (code-char 252)))");
        CL_UNCATCH();
        ASSERT(0 && "error did not unwind");
    } else {
        CL_UNCATCH();
        cl_vm.sp = 0;
        cl_vm.fp = 0;
        ASSERT(strstr(cl_error_msg, "gross: ") != NULL);
        /* the code point arrives UTF-8 encoded, not as a raw byte */
        ASSERT(strstr(cl_error_msg, "\xC3\xBC") != NULL);
    }
}
#endif /* CL_WIDE_STRINGS */

TEST(lisp_define_condition_report_symbol)
{
    /* Regression: define-condition with a :report FUNCTION-NAME SYMBOL (CLHS
       allows a string, a symbol naming a (condition stream) function, or a
       lambda).  The symbol form used to splice bare into funcall's evaluated
       slot -> "Unbound variable: <name>".  This is the cl-who / hunchentoot
       script-engine ASSERTION-FAILED shape: (:report print-assertion). */
    eval_print("(defun my-rep (c s) (declare (ignore c)) (write-string \"hi-from-fn\" s))");
    eval_print(
        "(define-condition my-rep-cond (error) ((x :initarg :x)) (:report my-rep))");
    ASSERT_STR_EQ(eval_print(
        "(format nil \"~a\" (make-condition 'my-rep-cond :x 1))"),
        "\"hi-from-fn\"");
    /* A lambda :report still works (function expression in the evaluated slot). */
    eval_print(
        "(define-condition my-rep-cond2 (error) ((x :initarg :x))"
        "  (:report (lambda (c s) (declare (ignore c)) (write-string \"lam\" s))))");
    ASSERT_STR_EQ(eval_print(
        "(format nil \"~a\" (make-condition 'my-rep-cond2 :x 1))"),
        "\"lam\"");
    /* A string :report is still verbatim. */
    eval_print(
        "(define-condition my-rep-cond3 (error) () (:report \"static msg\"))");
    ASSERT_STR_EQ(eval_print(
        "(format nil \"~a\" (make-condition 'my-rep-cond3))"),
        "\"static msg\"");
}

TEST(lisp_define_condition_bare_symbol_slot)
{
    /* Regression: a slot-specifier in DEFINE-CONDITION may be a bare SYMBOL
       (a slot name with no options) as well as a list (CLHS DEFINE-CONDITION
       / DEFCLASS).  define-condition's slot walk called (car spec) on every
       spec, so a bare symbol -> "CAR: argument is not of type LIST" at
       compile time.  This is the fast-http (clog dependency) shape:
         (define-condition fast-http-error (simple-error) (description) ...) */
    eval_print(
        "(define-condition bss-err (simple-error) (description))");
    /* Defining it no longer errors; it is a real condition type. */
    ASSERT_STR_EQ(eval_print("(conditionp (make-condition 'bss-err))"), "T");
    ASSERT_STR_EQ(eval_print("(typep (make-condition 'bss-err) 'error)"), "T");

    /* A subtype that re-specifies the same slot with options (the fast-http
       subclass pattern: bare slot in the parent, (description ...) in each
       child) works, and the slot is real — readable via a reader. */
    eval_print(
        "(define-condition bss-sub (bss-err)"
        "  ((description :initarg :description :reader bss-desc)))");
    ASSERT_STR_EQ(eval_print("(typep (make-condition 'bss-sub) 'bss-err)"), "T");
    ASSERT_STR_EQ(eval_print(
        "(bss-desc (make-condition 'bss-sub :description \"d\"))"), "\"d\"");

    /* A bare-symbol slot and a list slot mixed in one definition. */
    eval_print(
        "(define-condition bss-err2 (error) (a (b :initarg :b :reader bss-b)))");
    ASSERT_STR_EQ(eval_print("(bss-b (make-condition 'bss-err2 :b 7))"), "7");
}

TEST(lisp_define_condition_default_initargs)
{
    /* Regression: define-condition :default-initargs were silently dropped
       (%set-condition-default-initargs was a no-op stub), so a condition
       signalled without explicit initargs carried NIL where the default
       should be — e.g. (simple-condition-format-control) returned NIL.
       This is the chipi-api auth-controller failure shape:
         (define-condition auth-access-rights-error (auth-error) ()
           (:default-initargs :format-control "Insufficient access rights"))
       caught and checked via simple-condition-format-control. */
    eval_print(
        "(define-condition di-err (simple-condition) ()"
        "  (:default-initargs :format-control \"the default\"))");

    /* Default applies via MAKE-CONDITION when the initarg is omitted. */
    ASSERT_STR_EQ(eval_print(
        "(simple-condition-format-control (make-condition 'di-err))"),
        "\"the default\"");

    /* Default applies via ERROR/SIGNAL (coerce_to_condition path). */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (error 'di-err)"
        "  (di-err (c) (simple-condition-format-control c)))"),
        "\"the default\"");

    /* An explicit initarg OVERRIDES the default (CLHS 7.1.4 precedence). */
    ASSERT_STR_EQ(eval_print(
        "(simple-condition-format-control"
        "  (make-condition 'di-err :format-control \"explicit\"))"),
        "\"explicit\"");

    /* An explicit NIL is preserved — a default must not clobber it. */
    ASSERT_STR_EQ(eval_print(
        "(simple-condition-format-control"
        "  (make-condition 'di-err :format-control nil))"),
        "NIL");

    /* The default :format-control also drives the printed report. */
    ASSERT_STR_EQ(eval_print(
        "(format nil \"~a\" (make-condition 'di-err))"),
        "\"the default\"");

    /* A subclass inherits its OWN default-initargs (single-parent chain). */
    eval_print(
        "(define-condition di-sub (di-err) ()"
        "  (:default-initargs :format-control \"sub default\"))");
    ASSERT_STR_EQ(eval_print(
        "(simple-condition-format-control (make-condition 'di-sub))"),
        "\"sub default\"");

    /* A subclass with NO default-initargs of its own inherits the parent's. */
    eval_print("(define-condition di-sub2 (di-err) ())");
    ASSERT_STR_EQ(eval_print(
        "(simple-condition-format-control (make-condition 'di-sub2))"),
        "\"the default\"");
}

/* --- Lisp-level signal/warn/error tests --- */

TEST(lisp_signal_returns_nil)
{
    ASSERT_STR_EQ(eval_print("(signal (make-condition 'simple-condition :format-control \"test\"))"), "NIL");
}

TEST(lisp_signal_with_string)
{
    ASSERT_STR_EQ(eval_print("(signal \"something happened\")"), "NIL");
}

TEST(lisp_signal_with_symbol)
{
    ASSERT_STR_EQ(eval_print("(signal 'simple-condition)"), "NIL");
}

TEST(lisp_warn_returns_nil)
{
    ASSERT_STR_EQ(eval_print("(warn \"test warning\")"), "NIL");
}

TEST(lisp_warn_with_symbol)
{
    ASSERT_STR_EQ(eval_print("(warn 'simple-warning)"), "NIL");
}

TEST(lisp_warn_goes_to_error_output)
{
    /* Per HyperSpec, WARN writes to *error-output*. Capture it with
       with-output-to-string and verify the output appears there. */
    const char *result = eval_print(
        "(with-output-to-string (*error-output*) (warn \"test warning\"))");
    ASSERT(strstr(result, "WARNING:") != NULL);
    ASSERT(strstr(result, "test warning") != NULL);
}

TEST(lisp_warn_symbol_goes_to_error_output)
{
    /* warn with a condition type also writes to *error-output* */
    const char *result = eval_print(
        "(with-output-to-string (*error-output*) (warn 'simple-warning))");
    ASSERT(strstr(result, "WARNING:") != NULL);
}

TEST(lisp_error_still_caught)
{
    /* (error "test") caught by CL_CATCH in eval_print */
    const char *result = eval_print("(error \"test error\")");
    /* Should be caught as an error, not crash */
    ASSERT_STR_EQ(result, "ERROR:1");
}

TEST(lisp_error_with_symbol)
{
    /* (error 'type-error :datum 42) — condition type maps to CL_ERR_TYPE */
    const char *result = eval_print("(error 'type-error :datum 42)");
    ASSERT_STR_EQ(result, "ERROR:2");
}

/* --- handler-bind tests --- */

TEST(lisp_handler_bind_basic)
{
    /* handler-bind with signal: handler receives condition, throws type name */
    ASSERT_STR_EQ(eval_print(
        "(catch 'test-tag"
        "  (handler-bind ((simple-condition (lambda (c) (throw 'test-tag (condition-type-name c)))))"
        "    (signal (make-condition 'simple-condition :format-control \"test\"))))"),
        "SIMPLE-CONDITION");
}

TEST(lisp_handler_bind_error_type)
{
    /* handler-bind matching on error type via hierarchy */
    ASSERT_STR_EQ(eval_print(
        "(catch 'test-tag"
        "  (handler-bind ((error (lambda (c) (throw 'test-tag 42))))"
        "    (signal (make-condition 'simple-error :format-control \"boom\"))))"),
        "42");
}

TEST(lisp_handler_bind_no_match)
{
    /* handler-bind: non-matching type, handler not called, signal returns nil */
    ASSERT_STR_EQ(eval_print(
        "(catch 'test-tag"
        "  (handler-bind ((type-error (lambda (c) (throw 'test-tag :touched))))"
        "    (signal (make-condition 'simple-warning :format-control \"w\"))"
        "    :untouched))"),
        ":UNTOUCHED");
}

TEST(lisp_handler_bind_multiple_clauses)
{
    /* Multiple clauses: correct one matches */
    ASSERT_STR_EQ(eval_print(
        "(catch 'test-tag"
        "  (handler-bind ((warning (lambda (c) (throw 'test-tag :warn)))"
        "                 (error (lambda (c) (throw 'test-tag :error))))"
        "    (signal (make-condition 'simple-warning :format-control \"w\"))))"),
        ":WARN");
}

/* Regression: a handler that establishes its own HANDLER-BIND / IGNORE-ERRORS
 * while running must NOT clobber its own slot on the handler stack.  The old
 * cl_signal_condition truncated cl_handler_top to the running handler's mark,
 * so an IGNORE-ERRORS inside the handler pushed onto that slot, overwriting the
 * handler's binding with a stale ERROR handler whose handler-case CATCH tag was
 * gone once the handler returned.  A later, unrelated error then invoked the
 * stale binding and threw into the dead tag ("No catch for tag").  This is the
 * shape of ASDF's condition-muffling handler (match-condition-p uses
 * ignore-errors) wrapping a load that signals a warning and then an error —
 * exactly what blocked bringing up serapeum's test suite.
 *
 * With the fix the WARNING handler's slot is preserved (it stays a WARNING
 * handler), so the later (error ...) is not intercepted by it and propagates to
 * the outer HANDLER-CASE, which reports the real condition. */
TEST(lisp_handler_no_slot_clobber_after_inner_ignore_errors)
{
    ASSERT_STR_EQ(eval_print(
        "(handler-case"
        "    (handler-bind ((warning (lambda (c) (declare (ignore c))"
        "                              (ignore-errors (error \"inner probe\"))"
        "                              nil)))"
        "      (warn \"trigger\")"
        "      (error \"real error\"))"
        "  (error (c) (simple-condition-format-control c)))"),
        "\"real error\"");
}

/* The inner IGNORE-ERRORS inside a handler still works (catches its own error)
 * and the handler can still decline so the outer search continues. */
TEST(lisp_handler_inner_ignore_errors_catches)
{
    /* The handler runs IGNORE-ERRORS around a form that errors: ignore-errors
     * returns (values nil condition), so probe ends up NIL — i.e. the inner
     * error was caught by the handler's own ignore-errors, not lost.  The
     * handler then muffles so WARN returns cleanly. */
    ASSERT_STR_EQ(eval_print(
        "(let ((probe :none))"
        "  (handler-bind ((warning (lambda (c) (declare (ignore c))"
        "                            (setq probe (ignore-errors (error \"x\") :no-error))"
        "                            (muffle-warning))))"
        "    (warn \"w\"))"
        "  probe)"),
        "NIL");
}

TEST(lisp_handler_bind_body_value)
{
    /* handler-bind returns body value on normal exit */
    ASSERT_STR_EQ(eval_print(
        "(handler-bind ((error (lambda (c) nil)))"
        "  (+ 1 2))"),
        "3");
}

TEST(lisp_handler_bind_textual_order)
{
    /* CLHS 9.1.4.1: when several handler-bind clauses match the same
     * condition and decline (return normally), they must fire in the order
     * they are written, top to bottom.  Here both CONDITION (general, first)
     * and WARNING (specific, second) match a SIMPLE-WARNING.  The general
     * one is listed first, so it must run first.  Regression: amilisp used
     * to run them newest-first (i.e. reverse textual order), yielding
     * (:B :A) and breaking libraries like snooze that put a broad handler
     * first to do setup the later, more specific handler depends on. */
    ASSERT_STR_EQ(eval_print(
        "(let ((log nil))"
        "  (handler-bind ((condition (lambda (c) (declare (ignore c)) (push :a log)))"
        "                 (warning   (lambda (c) (declare (ignore c)) (push :b log))))"
        "    (signal (make-condition 'simple-warning :format-control \"w\")))"
        "  (nreverse log))"),
        "(:A :B)");
}

TEST(lisp_handler_bind_earlier_handler_sets_state_for_later)
{
    /* The snooze failure mode in miniature: a broad first handler computes
     * state that the later, more specific handler reads.  With the buggy
     * reverse order the specific handler ran first and saw NIL. */
    ASSERT_STR_EQ(eval_print(
        "(let ((ready nil) (result :unset))"
        "  (catch 'tag"
        "    (handler-bind"
        "        ((condition (lambda (c) (declare (ignore c)) (setf ready t)))"
        "         (warning   (lambda (c) (declare (ignore c))"
        "                      (setf result (if ready :ready :not-ready))"
        "                      (throw 'tag result))))"
        "      (signal (make-condition 'simple-warning :format-control \"w\"))))"
        "  result)"),
        ":READY");
}

TEST(lisp_handler_case_catches_error)
{
    /* handler-case catches error and runs clause body */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (error \"boom\")"
        "  (error (c) 42))"),
        "42");
}

TEST(lisp_handler_case_no_error)
{
    /* handler-case with no error returns form value */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (+ 1 2)"
        "  (error (c) 99))"),
        "3");
}

TEST(lisp_handler_case_multiple_values)
{
    /* CLHS: when no condition is signaled, handler-case returns ALL the values
     * of its body.  Regression: an earlier expansion bound the CATCH result to
     * one variable, collapsing (values a b) to just a — which broke callers
     * that (multiple-value-bind (x y) (handler-case (values ...) ...)), e.g.
     * rfc2388/str-based apikey id destructuring in chipi. */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (handler-case (values 1 2 3) (error () :err)))"),
        "(1 2 3)");
    /* ignore-errors is built on handler-case and must also pass them through. */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (ignore-errors (values 7 8 9)))"),
        "(7 8 9)");
    /* A clause body may itself return multiple values. */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (handler-case (error \"x\") (error () (values :a :b))))"),
        "(:A :B)");
}

TEST(lisp_handler_case_type_dispatch)
{
    /* handler-case dispatches on condition type */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (error 'type-error :datum 42 :expected-type 'string)"
        "  (type-error (c) (type-error-datum c))"
        "  (error (c) :generic))"),
        "42");
}

TEST(lisp_handler_case_t_catches_any_condition)
{
    /* T is the universal supertype — handler-case `(t (c) ...)` must catch
     * any condition (CLHS 4.4: T is supertype of everything).  Frameworks
     * (sento, log4cl, fiveam) rely on this as a catch-all clause. */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (error \"boom\")"
        "  (t (c) :caught))"),
        ":CAUGHT");
}

TEST(lisp_handler_bind_t_catches_any_condition)
{
    /* handler-bind with T must invoke the handler for any condition. */
    ASSERT_STR_EQ(eval_print(
        "(let ((seen nil))"
        "  (block done"
        "    (handler-bind ((t (lambda (c)"
        "                       (declare (ignore c))"
        "                       (setf seen :saw)"
        "                       (return-from done :ok))))"
        "      (error \"boom\")))"
        "  seen)"),
        ":SAW");
}

TEST(lisp_handler_case_t_catches_secondary_error_from_invoke_restart)
{
    /* Regression: when handler-bind invokes #'abort and no ABORT restart
     * exists, INVOKE-RESTART signals a secondary error.  The outer
     * handler-case `(t (c) ...)` must catch it (or it would propagate to
     * the debugger, as observed in sento BT-BOX-RESURRECTS-THREAD test). */
    ASSERT_STR_EQ(eval_print(
        "(handler-case"
        "    (handler-bind ((serious-condition #'abort))"
        "      (error \"die!\"))"
        "  (t (c) (declare (ignore c)) :caught))"),
        ":CAUGHT");
}

TEST(lisp_handler_bind_handler_invokes_restart_repeatedly)
{
    /* Regression (fiveam process-failure pattern): a HANDLER-BIND handler that
     * INVOKE-RESTARTs a WITH-SIMPLE-RESTART established in its OWN dynamic
     * extent must keep working for EVERY signal, not just the first.  The
     * handler's non-local exit (invoke-restart longjmp) unwinds past
     * cl_signal_condition, so the CLHS 9.1.4 disabled-band must be restored
     * from the NLX-frame snapshot — otherwise the handler stays disabled and
     * the SECOND signal escapes uncaught.  This blocked fiveam: every test
     * after the first failing check aborted the whole run.
     *
     * COUNT counts how many of the 3 signals reached the (continuing) handler;
     * a correct system records all 3. */
    ASSERT_STR_EQ(eval_print(
        "(let ((count 0))"
        "  (flet ((check (ok)"
        "           (unless ok"
        "             (with-simple-restart (skip \"skip\")"
        "               (error 'simple-error :format-control \"fail\"))"
        "             (incf count))))"
        "    (handler-bind ((simple-error"
        "                     (lambda (e) (declare (ignore e))"
        "                       (invoke-restart (find-restart 'skip)))))"
        "      (check nil) (check t) (check nil) (check nil)))"
        "    count))"),
        "3");
}

TEST(lisp_ignore_errors)
{
    /* ignore-errors catches error, returns nil */
    ASSERT_STR_EQ(eval_print(
        "(ignore-errors (error \"boom\"))"),
        "NIL");
}

TEST(lisp_ignore_errors_no_error)
{
    /* ignore-errors with no error returns form value */
    ASSERT_STR_EQ(eval_print(
        "(ignore-errors (+ 10 20))"),
        "30");
}

/* --- restart-case tests --- */

TEST(lisp_restart_case_basic)
{
    /* Basic restart-case: invoke-restart transfers control */
    ASSERT_STR_EQ(eval_print(
        "(restart-case (invoke-restart 'use-value 42)"
        "  (use-value (v) v))"),
        "42");
}

TEST(lisp_restart_case_normal_exit)
{
    /* restart-case: no restart invoked, returns form value */
    ASSERT_STR_EQ(eval_print(
        "(restart-case (+ 1 2)"
        "  (abort () 99))"),
        "3");
}

TEST(lisp_restart_case_multiple_clauses)
{
    /* Multiple restarts: correct one selected */
    ASSERT_STR_EQ(eval_print(
        "(restart-case (invoke-restart 'continue)"
        "  (abort () :aborted)"
        "  (continue () :continued))"),
        ":CONTINUED");
}

TEST(lisp_restart_case_no_params)
{
    /* Restart with no parameters */
    ASSERT_STR_EQ(eval_print(
        "(restart-case (invoke-restart 'abort)"
        "  (abort () 42))"),
        "42");
}

TEST(lisp_find_restart)
{
    /* find-restart returns restart name when active */
    ASSERT_STR_EQ(eval_print(
        "(restart-case (if (find-restart 'continue) :found :not-found)"
        "  (continue () nil))"),
        ":FOUND");

    /* find-restart result can be passed to invoke-restart (fiveam pattern) */
    ASSERT_STR_EQ(eval_print(
        "(restart-case (invoke-restart (find-restart 'use-value) 99)"
        "  (use-value (v) v))"),
        "99");
}

TEST(lisp_find_restart_missing)
{
    /* find-restart returns NIL when restart is not active */
    ASSERT_STR_EQ(eval_print(
        "(find-restart 'continue)"),
        "NIL");
}

TEST(lisp_compute_restarts)
{
    /* compute-restarts returns list of active restart names */
    ASSERT_STR_EQ(eval_print(
        "(restart-case (length (compute-restarts))"
        "  (abort () nil)"
        "  (continue () nil))"),
        "2");
}

TEST(lisp_restart_is_first_class_object)
{
    /* find-restart returns a first-class restart object (not a name symbol):
     * it is of type RESTART and is not a symbol. */
    ASSERT_STR_EQ(eval_print(
        "(restart-case (typep (find-restart 'foo) 'restart)"
        "  (foo () nil))"),
        "T");
    ASSERT_STR_EQ(eval_print(
        "(restart-case (symbolp (find-restart 'foo))"
        "  (foo () nil))"),
        "NIL");
    ASSERT_STR_EQ(eval_print(
        "(restart-case (type-of (find-restart 'foo))"
        "  (foo () nil))"),
        "RESTART");
}

TEST(lisp_restart_name)
{
    /* restart-name returns the restart's name symbol */
    ASSERT_STR_EQ(eval_print(
        "(restart-case (restart-name (find-restart 'my-restart))"
        "  (my-restart () nil))"),
        "MY-RESTART");
    /* restart-name on a non-restart signals an error */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (restart-name 42)"
        "  (error () :errored))"),
        ":ERRORED");
}

TEST(lisp_compute_restarts_returns_objects)
{
    /* compute-restarts returns restart objects, innermost first; their
     * names are recoverable via restart-name */
    ASSERT_STR_EQ(eval_print(
        "(restart-case"
        "    (restart-case"
        "        (mapcar #'restart-name (compute-restarts))"
        "      (inner () nil))"
        "  (outer () nil))"),
        "(INNER OUTER)");
    /* every element is a RESTART object */
    ASSERT_STR_EQ(eval_print(
        "(restart-case (every (lambda (r) (typep r 'restart)) (compute-restarts))"
        "  (a () nil) (b () nil))"),
        "T");
}

TEST(lisp_invoke_restart_by_object)
{
    /* invoke-restart accepts a restart object (not just a name) and passes
     * arguments through to the restart's handler */
    ASSERT_STR_EQ(eval_print(
        "(restart-case (invoke-restart (find-restart 'doit) 21)"
        "  (doit (x) (* x 2)))"),
        "42");
}

TEST(lisp_store_value_function)
{
    /* (store-value v) invokes the innermost STORE-VALUE restart with v */
    ASSERT_STR_EQ(eval_print(
        "(restart-case (store-value 7)"
        "  (store-value (v) (* v 6)))"),
        "42");
    /* returns NIL when no STORE-VALUE restart is established */
    ASSERT_STR_EQ(eval_print("(store-value 99)"), "NIL");
}

TEST(lisp_use_value_function)
{
    /* (use-value v) invokes the innermost USE-VALUE restart with v */
    ASSERT_STR_EQ(eval_print(
        "(restart-case (use-value 21)"
        "  (use-value (v) (* v 2)))"),
        "42");
    ASSERT_STR_EQ(eval_print("(use-value 1)"), "NIL");
}

TEST(lisp_restart_case_store_value_mutates_outer_var)
{
    /* The STORE-VALUE clause body is compiled as a closure; a (setf x ...)
     * inside it must mutate the ENCLOSING variable (boxing analysis must
     * treat restart-case clause bodies as closures).  Regression for the
     * boxing bug that made continuable CCASE/CTYPECASE loop forever. */
    ASSERT_STR_EQ(eval_print(
        "(let ((x 10))"
        "  (restart-case"
        "      (progn (handler-bind ((error (lambda (c) (declare (ignore c))"
        "                                      (store-value 99))))"
        "               (error \"boom\"))"
        "             x)"
        "    (store-value (nv) (setf x nv) x)))"),
        "99");
}

TEST(lisp_ccase_direct_match)
{
    ASSERT_STR_EQ(eval_print("(let ((x 'b)) (ccase x (a 'A) (b 'B) (c 'C)))"), "B");
    /* grouped keys */
    ASSERT_STR_EQ(eval_print("(let ((x 2)) (ccase x ((1 2 3) 'lo) ((4 5) 'hi)))"), "LO");
    /* empty body returns NIL */
    ASSERT_STR_EQ(eval_print("(let ((x 'a)) (ccase x (a) (b 'B)))"), "NIL");
}

TEST(lisp_ccase_store_value_retry)
{
    /* No clause matches 'zzz; the handler stores a good value, which the
     * loop re-reads and matches — CCASE returns the matching clause body. */
    ASSERT_STR_EQ(eval_print(
        "(let ((x 'zzz))"
        "  (handler-bind ((type-error (lambda (c) (declare (ignore c))"
        "                               (store-value 'b))))"
        "    (ccase x (a 'A) (b 'B))))"),
        "B");
    /* the store actually mutates the place */
    ASSERT_STR_EQ(eval_print(
        "(let ((x 'zzz))"
        "  (handler-bind ((type-error (lambda (c) (declare (ignore c))"
        "                               (store-value 'a))))"
        "    (ccase x (a 'A) (b 'B)))"
        "  x)"),
        "A");
    /* complex place: (car cell) */
    ASSERT_STR_EQ(eval_print(
        "(let ((cell (list 'bad)))"
        "  (handler-bind ((type-error (lambda (c) (declare (ignore c))"
        "                               (store-value 'y))))"
        "    (ccase (car cell) (x 'X) (y 'Y))))"),
        "Y");
}

TEST(lisp_ccase_expected_type)
{
    /* CCASE's type-error reports (member key...) as the expected type */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (let ((x 99)) (ccase x (1 'a) (2 'b)))"
        "  (type-error (c) (type-error-expected-type c)))"),
        "(MEMBER 1 2)");
}

TEST(lisp_ctypecase_direct_and_retry)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((x 5)) (ctypecase x (string 's) (integer 'i)))"), "I");
    /* store-value retry */
    ASSERT_STR_EQ(eval_print(
        "(let ((x 'sym))"
        "  (handler-bind ((type-error (lambda (c) (declare (ignore c))"
        "                               (store-value 42))))"
        "    (ctypecase x (string 's) (integer 'i))))"),
        "I");
    /* expected-type is (or type...) */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (let ((x 'sym)) (ctypecase x (string 's) (integer 'i)))"
        "  (type-error (c) (type-error-expected-type c)))"),
        "(OR STRING INTEGER)");
}

TEST(lisp_restart_report_string)
{
    /* PRINC of a restart prints its :report string; PRIN1 prints #<RESTART name> */
    ASSERT_STR_EQ(eval_print(
        "(restart-case (princ-to-string (find-restart 'foo))"
        "  (foo () :report \"Do the foo thing\" nil))"),
        "\"Do the foo thing\"");
    ASSERT_STR_EQ(eval_print(
        "(restart-case (prin1-to-string (find-restart 'foo))"
        "  (foo () :report \"Do the foo thing\" nil))"),
        "\"#<RESTART FOO>\"");
}

TEST(lisp_restart_report_function)
{
    /* A :report function is called with a stream to produce the report */
    ASSERT_STR_EQ(eval_print(
        "(restart-case"
        "    (princ-to-string (find-restart 'foo))"
        "  (foo () :report (lambda (s) (format s \"computed ~D\" (+ 1 2))) nil))"),
        "\"computed 3\"");
}

TEST(lisp_restart_no_report_princ)
{
    /* With no :report, PRINC falls back to the #<RESTART name> form */
    ASSERT_STR_EQ(eval_print(
        "(restart-case (princ-to-string (find-restart 'bare))"
        "  (bare () nil))"),
        "\"#<RESTART BARE>\"");
}

TEST(lisp_restart_test_filters_find)
{
    /* :test controls applicability for find-restart / compute-restarts */
    ASSERT_STR_EQ(eval_print(
        "(restart-case (if (find-restart 'r) :found :not-found)"
        "  (r () :test (lambda (c) (declare (ignore c)) nil) nil))"),
        ":NOT-FOUND");
    ASSERT_STR_EQ(eval_print(
        "(restart-case (if (find-restart 'r) :found :not-found)"
        "  (r () :test (lambda (c) (declare (ignore c)) t) nil))"),
        ":FOUND");
}

TEST(lisp_invoke_restart_interactively)
{
    /* invoke-restart-interactively uses the :interactive function to build
     * the argument list */
    ASSERT_STR_EQ(eval_print(
        "(restart-case (invoke-restart-interactively (find-restart 'add))"
        "  (add (a b) :interactive (lambda () (list 3 4)) (+ a b)))"),
        "7");
}

TEST(lisp_restart_case_with_handler)
{
    /* handler-case + restart-case interaction:
     * handler invokes a restart established around the signaling form */
    ASSERT_STR_EQ(eval_print(
        "(handler-bind ((error (lambda (c) (invoke-restart 'continue))))"
        "  (restart-case (error \"boom\")"
        "    (continue () 99)))"),
        "99");
}

TEST(lisp_with_simple_restart)
{
    /* with-simple-restart: invoke restart returns (values nil t) */
    ASSERT_STR_EQ(eval_print(
        "(with-simple-restart (abort \"Abort operation\")"
        "  (invoke-restart 'abort))"),
        "NIL");
}

TEST(lisp_with_simple_restart_normal)
{
    /* with-simple-restart: no restart invoked, returns body value */
    ASSERT_STR_EQ(eval_print(
        "(with-simple-restart (abort \"Abort operation\")"
        "  42)"),
        "42");
}

TEST(lisp_cerror_continue)
{
    /* cerror: CONTINUE restart returns NIL */
    ASSERT_STR_EQ(eval_print(
        "(handler-bind ((error (lambda (c) (invoke-restart 'continue))))"
        "  (cerror \"Continue anyway\" \"something bad\")"
        "  :after-cerror)"),
        ":AFTER-CERROR");
}

/* --- define-condition / check-type / assert tests --- */

TEST(lisp_define_condition_basic)
{
    /* define-condition creates a custom type that conditionp recognizes */
    ASSERT_STR_EQ(eval_print(
        "(define-condition my-error (error) ())"),
        "MY-ERROR");
    ASSERT_STR_EQ(eval_print(
        "(conditionp (make-condition 'my-error))"),
        "T");
}

TEST(lisp_define_condition_reader)
{
    /* define-condition with slot and reader accessor */
    eval_print(
        "(define-condition file-error (error)"
        "  ((pathname :initarg :pathname :reader file-error-pathname)))");
    ASSERT_STR_EQ(eval_print(
        "(file-error-pathname (make-condition 'file-error :pathname \"/tmp/foo\"))"),
        "\"/tmp/foo\"");
}

TEST(lisp_define_condition_hierarchy)
{
    /* Custom condition type matches parent via typep */
    eval_print("(define-condition net-error (error) ())");
    ASSERT_STR_EQ(eval_print(
        "(typep (make-condition 'net-error) 'error)"),
        "T");
    ASSERT_STR_EQ(eval_print(
        "(typep (make-condition 'net-error) 'condition)"),
        "T");
    ASSERT_STR_EQ(eval_print(
        "(typep (make-condition 'net-error) 'warning)"),
        "NIL");
}

TEST(lisp_define_condition_handler_case)
{
    /* handler-case catches custom condition type */
    eval_print("(define-condition app-error (error) ())");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (error 'app-error)"
        "  (app-error (c) :caught))"),
        ":CAUGHT");
}

TEST(lisp_define_condition_multi_slots)
{
    /* Multiple slots with readers */
    eval_print(
        "(define-condition db-error (error)"
        "  ((query :initarg :query :reader db-error-query)"
        "   (code :initarg :code :reader db-error-code)))");
    ASSERT_STR_EQ(eval_print(
        "(db-error-query (make-condition 'db-error :query \"SELECT\" :code 42))"),
        "\"SELECT\"");
    ASSERT_STR_EQ(eval_print(
        "(db-error-code (make-condition 'db-error :query \"SELECT\" :code 42))"),
        "42");
}

TEST(lisp_check_type_pass)
{
    /* check-type passes when type matches */
    ASSERT_STR_EQ(eval_print(
        "(let ((x 42)) (check-type x integer) :ok)"),
        ":OK");
}

TEST(lisp_check_type_fail)
{
    /* check-type signals type-error when type doesn't match */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (let ((x \"hello\")) (check-type x integer))"
        "  (type-error (c) (type-error-datum c)))"),
        "\"hello\"");
}

/* A standard condition made without :format-control reports from its
 * slots (CLHS 9.1.3: ~A on a condition prints its report).  Used to print
 * as "#<CONDITION TYPE-ERROR>" — the datum and expected type were in the
 * slots but never shown. */
TEST(lisp_type_error_default_report_princ)
{
    ASSERT_STR_EQ(eval_print(
        "(princ-to-string (make-condition 'type-error"
        " :datum 5 :expected-type 'list))"),
        "\"The value 5 is not of type LIST\"");
}

/* A circular datum: ERROR formats the slot report BEFORE it signals, so
 * without *PRINT-CIRCLE* bound there the print never ended and the handler
 * was never reached (alexandria's LASTCAR.ERROR.2 hung make test-extra).
 * The caller's *PRINT-CIRCLE* is untouched afterwards. */
TEST(lisp_type_error_default_report_circular_datum)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((c (list 1 2 3))) (setf (cdddr c) c)"
        "  (handler-case (error 'type-error :datum c :expected-type 'list)"
        "    (type-error (e) (list (princ-to-string e) *print-circle*))))"),
        "(\"The value #0=(1 2 3 . #0#) is not of type LIST\" NIL)");
    /* circular through the CAR */
    ASSERT_STR_EQ(eval_print(
        "(let ((c (list 1))) (setf (car c) c)"
        "  (handler-case (error 'type-error :datum c :expected-type 'list)"
        "    (type-error (e) (princ-to-string e))))"),
        "\"The value #0=(#0#) is not of type LIST\"");
}

/* Same hang, reached via the :format-control/:format-arguments path
 * (format_condition_report) instead of the slot-derived default report:
 * (error "~a" x) is the more common call form, and a circular format
 * argument used to print forever there too. */
TEST(lisp_simple_error_report_circular_format_arg)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((c (list 1 2 3))) (setf (cdddr c) c)"
        "  (handler-case (error \"~a\" c)"
        "    (error (e) (list (princ-to-string e) *print-circle*))))"),
        "(\"#0=(1 2 3 . #0#)\" NIL)");
}

TEST(lisp_type_error_default_report_error_handler)
{
    /* (error 'type-error ...) — the handler sees the same report */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (error 'type-error :datum \"x\" :expected-type 'integer)"
        "  (error (e) (format nil \"~a\" e)))"),
        "\"The value \\\"x\\\" is not of type INTEGER\"");
    /* CHECK-TYPE and THE go through the same path */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (let ((x 5)) (check-type x string))"
        "  (type-error (e) (format nil \"~a\" e)))"),
        "\"The value 5 is not of type STRING\"");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (the list 7)"
        "  (type-error (e) (format nil \"~a\" e)))"),
        "\"The value 7 is not of type LIST\"");
}

TEST(lisp_type_error_default_report_subtype)
{
    /* A user subtype of TYPE-ERROR without its own :report inherits it;
     * one WITH a :report keeps its own. */
    ASSERT_STR_EQ(eval_print(
        "(progn (define-condition my-te (type-error) ())"
        " (princ-to-string (make-condition 'my-te :datum 1 :expected-type 'symbol)))"),
        "\"The value 1 is not of type SYMBOL\"");
    ASSERT_STR_EQ(eval_print(
        "(progn (define-condition my-te2 (type-error) ()"
        "  (:report (lambda (c s) (format s \"custom ~a\" (type-error-datum c)))))"
        " (princ-to-string (make-condition 'my-te2 :datum 1 :expected-type 'symbol)))"),
        "\"custom 1\"");
}

TEST(lisp_type_error_default_report_prin1_unchanged)
{
    /* ~S / PRIN1 keeps the unreadable #<...> form (print-escape T) */
    ASSERT_STR_EQ(eval_print(
        "(prin1-to-string (make-condition 'type-error"
        " :datum 5 :expected-type 'list))"),
        "\"#<CONDITION TYPE-ERROR>\"");
    /* A :format-control still wins over the slot-derived report */
    ASSERT_STR_EQ(eval_print(
        "(princ-to-string (make-condition 'simple-type-error"
        " :datum 5 :expected-type 'list"
        " :format-control \"bad ~a\" :format-arguments '(5)))"),
        "\"bad 5\"");
}

TEST(lisp_cell_error_family_default_report)
{
    ASSERT_STR_EQ(eval_print(
        "(princ-to-string (make-condition 'unbound-variable :name 'foo))"),
        "\"The variable FOO is unbound\"");
    ASSERT_STR_EQ(eval_print(
        "(princ-to-string (make-condition 'undefined-function :name 'bar))"),
        "\"The function BAR is undefined\"");
    ASSERT_STR_EQ(eval_print(
        "(princ-to-string (make-condition 'division-by-zero"
        " :operation '/ :operands '(1 0)))"),
        "\"DIVISION-BY-ZERO signalled by / with operands (1 0)\"");
    ASSERT_STR_EQ(eval_print(
        "(princ-to-string (make-condition 'file-error :pathname \"x.txt\"))"),
        "\"File error on \\\"x.txt\\\"\"");
    /* A type with no standard slots keeps the #<...> default under ~A */
    ASSERT_STR_EQ(eval_print(
        "(princ-to-string (make-condition 'control-error))"),
        "\"#<CONDITION CONTROL-ERROR>\"");
}

TEST(lisp_assert_pass)
{
    /* assert passes when test is true */
    ASSERT_STR_EQ(eval_print(
        "(progn (assert (= 1 1)) :ok)"),
        ":OK");
}

TEST(lisp_assert_fail)
{
    /* assert signals error when test is false */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (assert (= 1 2))"
        "  (simple-error (c) :assertion-failed))"),
        ":ASSERTION-FAILED");
}

TEST(lisp_restart_stack_nlx)
{
    /* Verify restart stack is properly cleaned up after NLX */

    /* After error recovery, restart_top should be 0 */
    {
        int err; CL_CATCH(err);
        if (err == CL_ERR_NONE) {
            cl_error(CL_ERR_GENERAL, "test nlx");
            CL_UNCATCH();
        } else {
            CL_UNCATCH();
            ASSERT_EQ_INT(cl_restart_top, 0);
        }
    }
}

/* --- UNWIND-PROTECT + restart-case ordering tests --- */

TEST(lisp_restart_case_runs_uwp_cleanup_before_handler)
{
    /* CLHS INVOKE-RESTART: non-local transfer completes (running every
     * UNWIND-PROTECT cleanup) BEFORE the restart handler is called.
     * Correct order: BEFORE → CLEANUP → HANDLER. */
    eval_print("(defvar *restart-uwp-log* nil)");
    ASSERT_STR_EQ(eval_print(
        "(let ((*restart-uwp-log* nil))"
        "  (restart-case"
        "    (unwind-protect"
        "      (progn (push 'before *restart-uwp-log*) (invoke-restart 'abort))"
        "      (push 'cleanup *restart-uwp-log*))"
        "    (abort () (push 'handler *restart-uwp-log*) :done))"
        "  (reverse *restart-uwp-log*))"),
        "(BEFORE CLEANUP HANDLER)");
}

TEST(lisp_restart_case_nested_uwp_cleanup_order)
{
    /* Two interposing unwind-protects: cleanups run innermost-first,
     * both before the handler. */
    ASSERT_STR_EQ(eval_print(
        "(let ((*restart-uwp-log* nil))"
        "  (restart-case"
        "    (unwind-protect"
        "      (unwind-protect"
        "        (progn (push 'before *restart-uwp-log*) (invoke-restart 'abort))"
        "        (push 'inner-cleanup *restart-uwp-log*))"
        "      (push 'outer-cleanup *restart-uwp-log*))"
        "    (abort () (push 'handler *restart-uwp-log*) :done))"
        "  (reverse *restart-uwp-log*))"),
        "(BEFORE INNER-CLEANUP OUTER-CLEANUP HANDLER)");
}

TEST(lisp_restart_case_handler_return_is_restart_case_value)
{
    /* The handler's return value is the value of the restart-case form. */
    ASSERT_STR_EQ(eval_print(
        "(restart-case"
        "  (unwind-protect (invoke-restart 'abort) nil)"
        "  (abort () :handler-result))"),
        ":HANDLER-RESULT");
}

/* --- Nested UWP in cleanup regression tests (bug: nested UWP cleared pending-throw) --- */

TEST(lisp_nested_uwp_in_cleanup_does_not_truncate_transfer)
{
    /* The exact repro from the bug report: a nested unwind-protect inside the
     * cleanup must not truncate the outer invoke-restart transfer.
     * If the abort handler fires, restart-case returns :HANDLER-RAN.
     * If the pending transfer is lost, the UWP returns NIL (list-slot never
     * set in the NLX path) and restart-case returns NIL. */
    ASSERT_STR_EQ(eval_print(
        "(flet ((nested-uwp (thunk)"
        "         (unwind-protect (funcall thunk) nil)))"
        "  (restart-case"
        "    (unwind-protect"
        "      (invoke-restart 'abort)"
        "      (nested-uwp (lambda () nil)))"
        "    (abort () :handler-ran)))"),
        ":HANDLER-RAN");
}

TEST(lisp_two_deep_nested_uwp_in_cleanup)
{
    /* Two levels of nested UWP in cleanup: all cleanups run, handler fires.
     * restart-case returns :HANDLER-RAN if the handler fires correctly. */
    ASSERT_STR_EQ(eval_print(
        "(restart-case"
        "  (unwind-protect"
        "    (invoke-restart 'abort)"
        "    (unwind-protect nil"
        "      (unwind-protect nil nil)))"
        "  (abort () :handler-ran))"),
        ":HANDLER-RAN");
}

TEST(lisp_nested_uwp_cleanup_catches_inner_throw)
{
    /* Cleanup does its own catch/throw; outer handler must still fire.
     * restart-case returns :HANDLER-RAN if the handler fires correctly. */
    ASSERT_STR_EQ(eval_print(
        "(restart-case"
        "  (unwind-protect"
        "    (invoke-restart 'abort)"
        "    (catch 'inner"
        "      (unwind-protect (throw 'inner :x) nil)))"
        "  (abort () :handler-ran))"),
        ":HANDLER-RAN");
}

TEST(lisp_error_in_body_nested_uwp_in_cleanup_propagates)
{
    /* Error path: error in body + nested UWP in cleanup completing normally
     * must still propagate the error. */
    ASSERT_STR_EQ(eval_print(
        "(handler-case"
        "  (unwind-protect"
        "    (error \"test-error\")"
        "    (unwind-protect nil nil))"
        "  (error () :caught))"),
        ":CAUGHT");
}

TEST(lisp_nested_uwp_in_cleanup_runs_code_after_nested_uwp)
{
    /* Code after a nested UWP in an outer cleanup body must run — the inner
     * UWRETHROW must not re-initiate the outer pending NLX prematurely. */
    ASSERT_STR_EQ(eval_print(
        "(let ((log nil))"
        "  (flet ((nested-uwp (thunk)"
        "           (unwind-protect (funcall thunk) nil))"
        "         (mk (x) (push x log) x))"
        "    (restart-case"
        "      (unwind-protect (invoke-restart 'abort)"
        "        (progn (mk :cleanup-start)"
        "               (nested-uwp (lambda () (mk :inside-nested-uwp)))"
        "               (mk :cleanup-after-nested)))"
        "      (abort () (mk :handler)))"
        "    (reverse log)))"),
        "(:CLEANUP-START :INSIDE-NESTED-UWP :CLEANUP-AFTER-NESTED :HANDLER)");
}

TEST(lisp_two_deep_nested_uwp_in_cleanup_runs_code_after_both)
{
    /* Two levels of nested UWP inside the outer cleanup — every marker
     * must fire in order and the handler must run last. */
    ASSERT_STR_EQ(eval_print(
        "(let ((log nil))"
        "  (flet ((nested-uwp (thunk)"
        "           (unwind-protect (funcall thunk) nil))"
        "         (mk (x) (push x log) x))"
        "    (restart-case"
        "      (unwind-protect (invoke-restart 'abort)"
        "        (progn (mk :cleanup-start)"
        "               (nested-uwp (lambda ()"
        "                 (mk :inside-1st)"
        "                 (nested-uwp (lambda () (mk :inside-2nd)))"
        "                 (mk :after-2nd)))"
        "               (mk :after-1st)))"
        "      (abort () (mk :handler)))"
        "    (reverse log)))"),
        "(:CLEANUP-START :INSIDE-1ST :INSIDE-2ND :AFTER-2ND :AFTER-1ST :HANDLER)");
}

TEST(lisp_nested_uwp_in_cleanup_runs_code_after_for_error_propagation)
{
    /* Error path (pending_throw==2): code after the nested UWP in cleanup
     * must still execute, and the error must propagate to handler-case. */
    ASSERT_STR_EQ(eval_print(
        "(let ((log nil))"
        "  (flet ((nested-uwp (thunk)"
        "           (unwind-protect (funcall thunk) nil))"
        "         (mk (x) (push x log) x))"
        "    (handler-case"
        "      (unwind-protect (error \"test-error\")"
        "        (progn (mk :cleanup-start)"
        "               (nested-uwp (lambda () (mk :inside-nested-uwp)))"
        "               (mk :cleanup-after-nested)))"
        "      (error () (mk :caught)))"
        "    (reverse log)))"),
        "(:CLEANUP-START :INSIDE-NESTED-UWP :CLEANUP-AFTER-NESTED :CAUGHT)");
}

/* --- Debugger tests --- */

TEST(c_debugger_disabled_by_default)
{
    /* Debugger should be disabled by default (not in interactive REPL) */
    ASSERT_EQ_INT(cl_debugger_enabled, 0);
}

TEST(c_debugger_recursion_guard)
{
    /* When cl_in_debugger is set, cl_invoke_debugger should return immediately */
    CL_Obj cond = cl_make_condition(SYM_SIMPLE_ERROR, CL_NIL, CL_NIL);
    cl_in_debugger = 1;
    cl_invoke_debugger(cond);  /* Should return immediately, not hang */
    cl_in_debugger = 0;
    ASSERT(1);  /* If we get here, recursion guard worked */
}

TEST(c_debugger_depth_limit_unwinds)
{
    /* Regression: a re-signalling *debugger-hook* / restart can recurse
     * cl_invoke_debugger via cl_error_from_condition with no intervening
     * error frame, which previously ran the C stack into a SIGSEGV.  When
     * the depth counter is already at the limit, cl_invoke_debugger must
     * abandon the nested debugger and unwind to top level (longjmp) instead
     * of recursing — and reset the depth counter to 0. */
    CL_Obj cond = cl_make_condition(SYM_SIMPLE_ERROR, CL_NIL, CL_NIL);
    int err;
    int saved_enabled = cl_debugger_enabled;

    cl_debugger_enabled = 0;
    cl_in_debugger = 0;
    cl_debugger_depth = CL_DEBUGGER_MAX_DEPTH; /* simulate max nesting */

    CL_CATCH(err);
    if (err == CL_ERR_NONE) {
        cl_invoke_debugger(cond);  /* must longjmp to top level, not return */
        CL_UNCATCH();
        ASSERT(0);                 /* unreachable if the guard fired */
    } else {
        CL_UNCATCH();
    }

    /* jump_to_top_level must have reset the nesting state. */
    ASSERT_EQ_INT(cl_debugger_depth, 0);
    ASSERT_EQ_INT(cl_in_debugger, 0);

    cl_debugger_enabled = saved_enabled;
    cl_vm.sp = 0;
    cl_vm.fp = 0;
}

TEST(lisp_debugger_hook_secondary_error_recovers)
{
    /* A *debugger-hook* that itself signals a (secondary) error must unwind
     * cleanly without leaving the debugger nesting state elevated.  Since the
     * hook is bound to NIL while it runs, the secondary error does not re-call
     * the hook; it just unwinds back through the first debugger level. */
    const char *r = eval_print(
        "(let ((*debugger-hook* (lambda (c hook)"
        "                          (declare (ignore c hook))"
        "                          (error \"secondary\"))))"
        "  (error \"primary\"))");
    /* Must come back as an error (not hang / not crash). */
    ASSERT(strncmp(r, "ERROR:", 6) == 0);
    /* And no leaked nesting — the debugger is usable again. */
    ASSERT_EQ_INT(cl_debugger_depth, 0);
    ASSERT_EQ_INT(cl_in_debugger, 0);
}

TEST(lisp_invoke_debugger_exists)
{
    ASSERT_STR_EQ(eval_print("(functionp #'invoke-debugger)"), "T");
}

TEST(lisp_debugger_hook_initially_nil)
{
    ASSERT_STR_EQ(eval_print("*debugger-hook*"), "NIL");
}

TEST(lisp_debugger_hook_is_special)
{
    /* *debugger-hook* should be a special variable (dynamic binding works) */
    ASSERT_STR_EQ(eval_print("(let ((*debugger-hook* 42)) *debugger-hook*)"), "42");
}

TEST(lisp_break_exists)
{
    ASSERT_STR_EQ(eval_print("(functionp #'break)"), "T");
}

TEST(lisp_debugger_hook_called)
{
    /* Set *debugger-hook* to a lambda that marks a box and invokes CONTINUE;
     * trigger error inside restart-case with continue restart */
    ASSERT_STR_EQ(eval_print(
        "(let ((box (cons nil nil)))"
        "  (let ((*debugger-hook* (lambda (c hook)"
        "                           (rplaca box t)"
        "                           (invoke-restart 'continue))))"
        "    (restart-case (error \"test\")"
        "      (continue () nil)))"
        "  (car box))"),
        "T");
}

TEST(lisp_break_with_continue)
{
    /* Set *debugger-hook* to invoke CONTINUE, call break, verify continues */
    ASSERT_STR_EQ(eval_print(
        "(let ((*debugger-hook* (lambda (c hook)"
        "                         (invoke-restart 'continue))))"
        "  (break)"
        "  :after-break)"),
        ":AFTER-BREAK");
}

/* Regression: a WARNING handler that is a Lisp closure calling (muffle-warning)
 * — as opposed to #'muffle-warning installed directly — must muffle cleanly.
 * The muffle restart is thrown from inside a bytecode handler frame; WARN's
 * hand-rolled catch landing previously did not restore cl_vm.sp/fp, corrupting
 * the VM so a spurious "restart MUFFLE-WARNING not found" surfaced afterward. */
TEST(lisp_muffle_warning_from_closure_handler)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((log nil))"
        "  (handler-bind ((warning (lambda (a) (push a log) (muffle-warning))))"
        "    (warn 'simple-warning :format-control \"w\"))"
        "  (length log))"),
        "1");
}

/* Regression: handler-bind in tail position must still pop its handlers.  The
 * body's tail call was emitted as OP_TAILCALL, replacing the frame and skipping
 * OP_HANDLER_POP — leaking one binding per call until the 64-slot handler stack
 * overflowed.  5000 iterations >> 64, so a leak would error here. */
TEST(lisp_handler_bind_tail_position_no_leak)
{
    eval_print("(defun %lk-inner () 42)");
    eval_print("(defun %lk-hb () (handler-bind ((warning #'identity)) (%lk-inner)))");
    ASSERT_STR_EQ(eval_print("(progn (dotimes (i 5000) (%lk-hb)) :done)"), ":DONE");
}

/* Regression: catch in tail position must pop its NLX frame (same class of bug
 * as handler-bind above).  5000 iterations >> 2048-frame stack. */
TEST(lisp_catch_tail_position_no_leak)
{
    eval_print("(defun %lk-inner2 () 7)");
    eval_print("(defun %lk-catch () (catch 'tag (%lk-inner2)))");
    ASSERT_STR_EQ(eval_print("(progn (dotimes (i 5000) (%lk-catch)) :done)"), ":DONE");
}

/* Regression: a define-condition slot :initform must be evaluated per instance
 * when the initarg is not supplied (CLHS 7.1.3). */
TEST(lisp_condition_slot_initform_instance)
{
    eval_print("(define-condition %ci-test (error)"
               "  ((s :initform 42 :accessor %ci-s)))");
    ASSERT_STR_EQ(eval_print("(%ci-s (make-condition '%ci-test))"), "42");
    /* An explicitly supplied initarg overrides the initform. */
    eval_print("(define-condition %ci-test2 (error)"
               "  ((s :initarg :s :initform 42 :accessor %ci-s2)))");
    ASSERT_STR_EQ(eval_print("(%ci-s2 (make-condition '%ci-test2 :s 7))"), "7");
}

/* Regression: a class-allocated slot with no initarg (fiasco's PROGRESS-CHAR /
 * CONTEXT shape) must return its :initform value rather than NIL. */
TEST(lisp_condition_slot_initform_class_allocation)
{
    eval_print("(define-condition %cc-test (warning)"
               "  ((pc :initform #\\X :accessor %cc-pc :allocation :class)))");
    ASSERT_STR_EQ(eval_print("(%cc-pc (make-condition '%cc-test))"), "#\\X");
}

/* Regression: the initform is evaluated at make-condition time (capturing the
 * dynamic environment then), not lazily at read time — fiasco's CONTEXT slot
 * (:initform *context*) depends on this. */
TEST(lisp_condition_slot_initform_evaluated_at_make_time)
{
    eval_print("(defvar *ci-dyn* :outer)");
    eval_print("(define-condition %cd-test (error)"
               "  ((ctx :initform *ci-dyn* :accessor %cd-ctx)))");
    ASSERT_STR_EQ(eval_print(
        "(%cd-ctx (let ((*ci-dyn* :inner)) (make-condition '%cd-test)))"),
        ":INNER");
}

/* Regression: a condition type whose name is ALSO a (&key ...) macro must not
 * trip the compiler's speculative macroexpansion in a handler-case clause head.
 * Serapeum's DISPATCH-CASE-ERROR is both the condition class and a
 * `(&key type datum)` macro; handler-case expands to a let (binding a captured
 * BOX) + handler-bind + typecase, and scan_body_for_boxing walked the
 * handler-bind clause (%hc-dce (lambda ...)) as a form, macroexpanding %hc-dce
 * with one positional arg → "odd number of keyword arguments".  The clause CAR
 * is a type spec, not code; the boxing scan must skip it. */
TEST(lisp_handler_case_type_is_keyword_macro)
{
    eval_print("(defmacro %hc-dce (&key type datum)"
               "  (declare (ignore type datum)) nil)");
    eval_print("(define-condition %hc-dce (error) ())");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (error '%hc-dce)"
        "  (%hc-dce (c) (declare (ignore c)) :caught))"),
        ":CAUGHT");
    /* Non-handled path still returns the body value. */
    ASSERT_STR_EQ(eval_print(
        "(handler-case 7 (%hc-dce (c) (declare (ignore c)) :caught))"),
        "7");
}

/* Regression (NLX-scan twin of the above): the same macro-named condition type
 * in a handler-bind inside a block/return-from.  The block triggers
 * tree_needs_nlx_block → nlx_scan, which likewise walked the handler-bind
 * clause head and macroexpanded it.  This is the exact shape of fiveam's
 * SIGNALS macro that broke serapeum's dispatch-case test registration. */
TEST(lisp_handler_bind_type_macro_in_block)
{
    eval_print("(defmacro %hb-dce (&key type datum)"
               "  (declare (ignore type datum)) nil)");
    eval_print("(define-condition %hb-dce (error) ())");
    ASSERT_STR_EQ(eval_print(
        "(block blk"
        "  (handler-bind ((%hb-dce (lambda (c) (declare (ignore c))"
        "                            (return-from blk :caught))))"
        "    (error '%hb-dce))"
        "  :fell-through)"),
        ":CAUGHT");
}

/* Regression: a handler-case / handler-bind clause type that is a user
 * deftype aliasing a condition class must be expanded so it matches the
 * signaled condition.  This is serapeum/portability's NO-APPLICABLE-METHOD-ERROR
 * pattern: (deftype no-applicable-method-error () 'simple-error) used as a
 * handler-case clause around a generic-function call with no applicable
 * method.  Previously cl_condition_type_matches only walked the condition
 * class hierarchy and never expanded the alias, so the condition escaped. */
TEST(lisp_handler_case_deftype_alias_clause)
{
    eval_print("(deftype %nam-err () 'simple-error)");
    /* handler-case must dispatch through the alias to the real class. */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (error \"boom\")"
        "  (%nam-err () :caught))"),
        ":CAUGHT");
    /* handler-bind likewise. */
    ASSERT_STR_EQ(eval_print(
        "(block b"
        "  (handler-bind ((%nam-err (lambda (c) (declare (ignore c))"
        "                             (return-from b :caught-hb))))"
        "    (error \"boom\"))"
        "  :fell-through)"),
        ":CAUGHT-HB");
    /* A non-matching condition must still decline (alias is for SIMPLE-ERROR,
     * a plain WARNING is not a SIMPLE-ERROR). */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (signal (make-condition 'simple-warning"
        "                                      :format-control \"w\"))"
        "  (%nam-err () :wrongly-caught)"
        "  (warning () :as-warning))"),
        ":AS-WARNING");
}

/* A deftype alias that expands to a compound (or ...) of condition classes
 * must match if the condition is any of them. */
TEST(lisp_handler_case_deftype_alias_or_clause)
{
    eval_print("(deftype %err-or-warn () '(or type-error simple-warning))");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (error 'type-error :datum 1 :expected-type 'string)"
        "  (%err-or-warn () :caught))"),
        ":CAUGHT");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (signal (make-condition 'simple-warning"
        "                                      :format-control \"w\"))"
        "  (%err-or-warn () :caught-warn)"
        "  (warning () :as-warning))"),
        ":CAUGHT-WARN");
    /* program-error is neither branch → declines through to the next clause. */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (error 'program-error)"
        "  (%err-or-warn () :wrongly-caught)"
        "  (error () :as-error))"),
        ":AS-ERROR");
}

/* Regression: pure ERROR (not WARNING) during compile-detect set warnings-p,
 * violating CLHS COMPILE which says warnings-p reflects only WARNING conditions.
 * Directly manipulate the detect state and call cl_signal_condition so we can
 * verify the flag behaviour in isolation. */
TEST(c_compile_detect_error_only)
{
    CL_Obj cond;
    int saved_depth = cl_compile_detect_depth;
    int saved_w     = cl_compile_warnings_p;
    int saved_f     = cl_compile_failure_p;

    cond = cl_make_condition(SYM_SIMPLE_ERROR, CL_NIL, CL_NIL);
    cl_compile_detect_depth = 1;
    cl_compile_warnings_p   = 0;
    cl_compile_failure_p    = 0;
    cl_signal_condition(cond);  /* no handlers → returns NIL, no debugger */
    cl_compile_detect_depth = saved_depth;

    /* CLHS: warnings-p is true for any condition of type ERROR or WARNING, so a
     * pure ERROR sets BOTH warnings-p and failure-p (failure-p excludes only
     * style-warning, which an error is not). */
    ASSERT_EQ_INT(cl_compile_warnings_p, 1);
    ASSERT_EQ_INT(cl_compile_failure_p,  1);

    cl_compile_warnings_p = saved_w;
    cl_compile_failure_p  = saved_f;
}

/* CLHS COMPILE returns warnings-p (value 2) and failure-p (value 3) true when
 * the compiler detects a WARNING/ERROR — e.g. a macro that calls WARN at
 * expansion time (serapeum's EIF/EIF-LET).  Style-warnings set warnings-p but
 * not failure-p; a clean compile sets neither. */
TEST(lisp_compile_reports_warnings_p)
{
    eval_print("(defmacro %eif (test then &optional (else nil else?))"
               "  (unless else? (warn \"missing else\"))"
               "  (list 'if test then else))");
    /* WARN at macroexpansion → warnings-p (and failure-p) true. */
    ASSERT_STR_EQ(eval_print(
        "(let ((*error-output* (make-string-output-stream)))"
        "  (nth-value 1 (compile nil '(lambda (x y) (%eif x y)))))"),
        "T");
    ASSERT_STR_EQ(eval_print(
        "(let ((*error-output* (make-string-output-stream)))"
        "  (nth-value 2 (compile nil '(lambda (x y) (%eif x y)))))"),
        "T");
    /* Complete form → no warning → both NIL. */
    ASSERT_STR_EQ(eval_print(
        "(nth-value 1 (compile nil '(lambda (x y z) (%eif x y z))))"),
        "NIL");
    /* Plain lambda → no warning. */
    ASSERT_STR_EQ(eval_print(
        "(nth-value 1 (compile nil '(lambda (x) x)))"),
        "NIL");
    /* style-warning sets warnings-p but not failure-p. */
    eval_print("(defmacro %sw (x)"
               "  (warn (make-condition 'style-warning))"
               "  x)");
    ASSERT_STR_EQ(eval_print(
        "(let ((*error-output* (make-string-output-stream)))"
        "  (nth-value 1 (compile nil '(lambda () (%sw 1)))))"),
        "T");
    ASSERT_STR_EQ(eval_print(
        "(let ((*error-output* (make-string-output-stream)))"
        "  (nth-value 2 (compile nil '(lambda () (%sw 1)))))"),
        "NIL");
    /* CLHS: a pure ERROR (non-warning) detected at compile time sets BOTH
     * warnings-p and failure-p (warnings-p covers "error or warning").  Use
     * (signal ...) so the error is detected by the compile-detect machinery
     * but not propagated (no handler needed). */
    eval_print("(defmacro %err-only-macro ()"
               "  (signal (make-condition 'simple-error :format-control \"ct-err\"))"
               "  42)");
    ASSERT_STR_EQ(eval_print(
        "(nth-value 1 (compile nil '(lambda () (%err-only-macro))))"),
        "T");
    ASSERT_STR_EQ(eval_print(
        "(nth-value 2 (compile nil '(lambda () (%err-only-macro))))"),
        "T");
}

int main(void)
{
    setup();

    /* C-level tests */
    RUN(c_make_condition);
    RUN(c_condition_p);
    RUN(c_condition_with_slots);
    RUN(c_hierarchy_matches);
    RUN(c_signal_no_handlers);
    RUN(c_error_creates_condition);
    RUN(c_create_condition_from_error);
    RUN(c_compile_detect_error_only);
    RUN(c_error_print_goes_to_error_output);
    RUN(c_handler_stack_nlx);

    /* Lisp-level tests */
    RUN(lisp_conditionp);
    RUN(lisp_make_condition);
    RUN(lisp_condition_type_name);
    RUN(lisp_type_error_accessors);
    RUN(lisp_simple_condition_accessors);
    RUN(lisp_type_of);
    RUN(lisp_typep_condition);
    RUN(lisp_condition_multiple_inheritance);
    RUN(lisp_printer);
    RUN(lisp_printer_aesthetic_uses_report);
    RUN(lisp_define_condition_report_symbol);
    RUN(lisp_define_condition_bare_symbol_slot);
    RUN(lisp_define_condition_default_initargs);
    RUN(lisp_condition_slot_initform_instance);
    RUN(lisp_condition_slot_initform_class_allocation);
    RUN(lisp_condition_slot_initform_evaluated_at_make_time);
    RUN(lisp_handler_case_type_is_keyword_macro);
    RUN(lisp_handler_bind_type_macro_in_block);
    RUN(lisp_handler_case_deftype_alias_clause);
    RUN(lisp_handler_case_deftype_alias_or_clause);
    RUN(lisp_compile_reports_warnings_p);

    /* Signal/warn/error tests */
    RUN(lisp_signal_returns_nil);
    RUN(lisp_signal_with_string);
    RUN(lisp_signal_with_symbol);
    RUN(lisp_warn_returns_nil);
    RUN(lisp_warn_with_symbol);
    RUN(lisp_muffle_warning_from_closure_handler);
    RUN(lisp_warn_goes_to_error_output);
    RUN(lisp_warn_symbol_goes_to_error_output);
    RUN(lisp_error_still_caught);
    RUN(lisp_error_with_symbol);

    /* Wide-string condition reports */
#ifdef CL_WIDE_STRINGS
    RUN(lisp_wide_string_error_designator);
    RUN(lisp_wide_formatted_report);
    RUN(lisp_wide_format_control_captured);
    RUN(lisp_wide_default_initarg_control);
    RUN(lisp_wide_report_prin1_wrapper);
    RUN(lisp_wide_warn_report);
    RUN(c_wide_report_reaches_error_msg);
#endif

    /* handler-bind tests */
    RUN(lisp_handler_bind_basic);
    RUN(lisp_handler_bind_error_type);
    RUN(lisp_handler_bind_no_match);
    RUN(lisp_handler_bind_multiple_clauses);
    RUN(lisp_handler_no_slot_clobber_after_inner_ignore_errors);
    RUN(lisp_handler_inner_ignore_errors_catches);
    RUN(lisp_handler_bind_body_value);
    RUN(lisp_handler_bind_textual_order);
    RUN(lisp_handler_bind_earlier_handler_sets_state_for_later);
    RUN(lisp_handler_bind_tail_position_no_leak);
    RUN(lisp_catch_tail_position_no_leak);

    /* handler-case / ignore-errors tests */
    RUN(lisp_handler_case_catches_error);
    RUN(lisp_handler_case_no_error);
    RUN(lisp_handler_case_multiple_values);
    RUN(lisp_handler_case_type_dispatch);
    RUN(lisp_handler_case_t_catches_any_condition);
    RUN(lisp_handler_bind_t_catches_any_condition);
    RUN(lisp_handler_case_t_catches_secondary_error_from_invoke_restart);
    RUN(lisp_handler_bind_handler_invokes_restart_repeatedly);
    RUN(lisp_ignore_errors);
    RUN(lisp_ignore_errors_no_error);

    /* restart-case tests */
    RUN(lisp_restart_case_basic);
    RUN(lisp_restart_case_normal_exit);
    RUN(lisp_restart_case_multiple_clauses);
    RUN(lisp_restart_case_no_params);
    RUN(lisp_find_restart);
    RUN(lisp_find_restart_missing);
    RUN(lisp_compute_restarts);
    RUN(lisp_restart_is_first_class_object);
    RUN(lisp_restart_name);
    RUN(lisp_compute_restarts_returns_objects);
    RUN(lisp_invoke_restart_by_object);
    RUN(lisp_store_value_function);
    RUN(lisp_use_value_function);
    RUN(lisp_restart_case_store_value_mutates_outer_var);
    RUN(lisp_ccase_direct_match);
    RUN(lisp_ccase_store_value_retry);
    RUN(lisp_ccase_expected_type);
    RUN(lisp_ctypecase_direct_and_retry);
    RUN(lisp_restart_report_string);
    RUN(lisp_restart_report_function);
    RUN(lisp_restart_no_report_princ);
    RUN(lisp_restart_test_filters_find);
    RUN(lisp_invoke_restart_interactively);
    RUN(lisp_restart_case_with_handler);
    RUN(lisp_with_simple_restart);
    RUN(lisp_with_simple_restart_normal);
    RUN(lisp_cerror_continue);
    RUN(lisp_restart_stack_nlx);
    RUN(lisp_restart_case_runs_uwp_cleanup_before_handler);
    RUN(lisp_restart_case_nested_uwp_cleanup_order);
    RUN(lisp_restart_case_handler_return_is_restart_case_value);
    RUN(lisp_nested_uwp_in_cleanup_does_not_truncate_transfer);
    RUN(lisp_two_deep_nested_uwp_in_cleanup);
    RUN(lisp_nested_uwp_cleanup_catches_inner_throw);
    RUN(lisp_error_in_body_nested_uwp_in_cleanup_propagates);
    RUN(lisp_nested_uwp_in_cleanup_runs_code_after_nested_uwp);
    RUN(lisp_two_deep_nested_uwp_in_cleanup_runs_code_after_both);
    RUN(lisp_nested_uwp_in_cleanup_runs_code_after_for_error_propagation);

    /* define-condition / check-type / assert tests */
    RUN(lisp_define_condition_basic);
    RUN(lisp_define_condition_reader);
    RUN(lisp_define_condition_hierarchy);
    RUN(lisp_define_condition_handler_case);
    RUN(lisp_define_condition_multi_slots);
    RUN(lisp_check_type_pass);
    RUN(lisp_check_type_fail);
    RUN(lisp_type_error_default_report_princ);
    RUN(lisp_type_error_default_report_circular_datum);
    RUN(lisp_simple_error_report_circular_format_arg);
    RUN(lisp_type_error_default_report_error_handler);
    RUN(lisp_type_error_default_report_subtype);
    RUN(lisp_type_error_default_report_prin1_unchanged);
    RUN(lisp_cell_error_family_default_report);
    RUN(lisp_assert_pass);
    RUN(lisp_assert_fail);

    /* Debugger tests */
    RUN(c_debugger_disabled_by_default);
    RUN(c_debugger_recursion_guard);
    RUN(c_debugger_depth_limit_unwinds);
    RUN(lisp_debugger_hook_secondary_error_recovers);
    RUN(lisp_invoke_debugger_exists);
    RUN(lisp_debugger_hook_initially_nil);
    RUN(lisp_debugger_hook_is_special);
    RUN(lisp_break_exists);
    RUN(lisp_debugger_hook_called);
    RUN(lisp_break_with_continue);

    teardown();
    REPORT();
}
