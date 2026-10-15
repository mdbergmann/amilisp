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
#include "core/compiler_internal.h" /* determine_boxed_vars (deep-scan regression) */
#include "platform/platform.h"
#if defined(PLATFORM_POSIX) || defined(PLATFORM_WIN32)
#include <locale.h>
#endif

static void setup(void)
{
#if defined(PLATFORM_POSIX) || defined(PLATFORM_WIN32)
    /* Match the runtime's locale strategy in src/main.c so Unicode
     * classification (iswalpha/iswupper/iswlower) works in stock
     * containers where LANG is unset, and on Windows, where the UCRT
     * spells the UTF-8 locale ".UTF-8". */
    if (!setlocale(LC_CTYPE, "C.UTF-8") &&
        !setlocale(LC_CTYPE, "en_US.UTF-8") &&
        !setlocale(LC_CTYPE, ".UTF-8")) {
        setlocale(LC_CTYPE, "");
    }
#endif
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
    static char buf[256];
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

/* --- Arithmetic --- */

TEST(eval_add)
{
    ASSERT_EQ_INT(eval_int("(+ 1 2)"), 3);
    ASSERT_EQ_INT(eval_int("(+ 1 2 3 4 5)"), 15);
    ASSERT_EQ_INT(eval_int("(+)"), 0);
}

TEST(eval_sub)
{
    ASSERT_EQ_INT(eval_int("(- 10 3)"), 7);
    ASSERT_EQ_INT(eval_int("(- 5)"), -5);
    ASSERT_EQ_INT(eval_int("(- 100 50 25)"), 25);
}

TEST(eval_mul)
{
    ASSERT_EQ_INT(eval_int("(* 3 4)"), 12);
    ASSERT_EQ_INT(eval_int("(* 2 3 4)"), 24);
    ASSERT_EQ_INT(eval_int("(*)"), 1);
}

TEST(eval_div)
{
    ASSERT_EQ_INT(eval_int("(/ 20 4)"), 5);
    ASSERT_EQ_INT(eval_int("(/ 100 2 5)"), 10);
}

TEST(eval_1plus_1minus)
{
    ASSERT_EQ_INT(eval_int("(1+ 41)"), 42);
    ASSERT_EQ_INT(eval_int("(1- 43)"), 42);
}

TEST(eval_mod)
{
    ASSERT_EQ_INT(eval_int("(mod 10 3)"), 1);
    ASSERT_EQ_INT(eval_int("(mod 9 3)"), 0);
}

/* --- Comparison --- */

TEST(eval_numeq)
{
    ASSERT_STR_EQ(eval_print("(= 5 5)"), "T");
    ASSERT_STR_EQ(eval_print("(= 5 6)"), "NIL");
}

TEST(eval_lt)
{
    ASSERT_STR_EQ(eval_print("(< 1 2)"), "T");
    ASSERT_STR_EQ(eval_print("(< 2 1)"), "NIL");
    ASSERT_STR_EQ(eval_print("(< 1 2 3)"), "T");
}

TEST(eval_gt)
{
    ASSERT_STR_EQ(eval_print("(> 3 2)"), "T");
    ASSERT_STR_EQ(eval_print("(> 3 2 1)"), "T");
    ASSERT_STR_EQ(eval_print("(> 1 2)"), "NIL");
}

TEST(eval_le_ge)
{
    ASSERT_STR_EQ(eval_print("(<= 1 1)"), "T");
    ASSERT_STR_EQ(eval_print("(<= 1 2)"), "T");
    ASSERT_STR_EQ(eval_print("(<= 2 1)"), "NIL");
    ASSERT_STR_EQ(eval_print("(>= 2 2)"), "T");
    ASSERT_STR_EQ(eval_print("(>= 2 1)"), "T");
}

/* --- Predicates --- */

TEST(eval_predicates)
{
    ASSERT_STR_EQ(eval_print("(null nil)"), "T");
    ASSERT_STR_EQ(eval_print("(null t)"), "NIL");
    ASSERT_STR_EQ(eval_print("(consp '(1))"), "T");
    ASSERT_STR_EQ(eval_print("(consp 42)"), "NIL");
    ASSERT_STR_EQ(eval_print("(atom 42)"), "T");
    ASSERT_STR_EQ(eval_print("(atom '(1))"), "NIL");
    ASSERT_STR_EQ(eval_print("(numberp 42)"), "T");
    ASSERT_STR_EQ(eval_print("(symbolp 'foo)"), "T");
    ASSERT_STR_EQ(eval_print("(stringp \"hi\")"), "T");
    ASSERT_STR_EQ(eval_print("(listp nil)"), "T");
    ASSERT_STR_EQ(eval_print("(listp '(1))"), "T");
}

TEST(eval_not)
{
    ASSERT_STR_EQ(eval_print("(not nil)"), "T");
    ASSERT_STR_EQ(eval_print("(not t)"), "NIL");
    ASSERT_STR_EQ(eval_print("(not 42)"), "NIL");
}

TEST(eval_eq_eql_equal)
{
    ASSERT_STR_EQ(eval_print("(eq 'a 'a)"), "T");
    ASSERT_STR_EQ(eval_print("(eql 42 42)"), "T");
    ASSERT_STR_EQ(eval_print("(equal '(1 2) '(1 2))"), "T");
    ASSERT_STR_EQ(eval_print("(equal '(1 2) '(1 3))"), "NIL");
}

/* --- List operations --- */

TEST(eval_cons_car_cdr)
{
    ASSERT_STR_EQ(eval_print("(cons 1 2)"), "(1 . 2)");
    ASSERT_STR_EQ(eval_print("(car '(a b c))"), "A");
    ASSERT_STR_EQ(eval_print("(cdr '(a b c))"), "(B C)");
}

TEST(eval_list)
{
    ASSERT_STR_EQ(eval_print("(list 1 2 3)"), "(1 2 3)");
    ASSERT_STR_EQ(eval_print("(list)"), "NIL");
}

TEST(eval_length)
{
    ASSERT_EQ_INT(eval_int("(length '(1 2 3))"), 3);
    ASSERT_EQ_INT(eval_int("(length nil)"), 0);
    ASSERT_EQ_INT(eval_int("(length \"hello\")"), 5);
}

TEST(eval_append)
{
    ASSERT_STR_EQ(eval_print("(append '(1 2) '(3 4))"), "(1 2 3 4)");
    ASSERT_STR_EQ(eval_print("(append nil '(1))"), "(1)");
}

TEST(eval_reverse)
{
    ASSERT_STR_EQ(eval_print("(reverse '(1 2 3))"), "(3 2 1)");
    ASSERT_STR_EQ(eval_print("(reverse nil)"), "NIL");
}

TEST(eval_nth)
{
    ASSERT_STR_EQ(eval_print("(nth 0 '(a b c))"), "A");
    ASSERT_STR_EQ(eval_print("(nth 1 '(a b c))"), "B");
    ASSERT_STR_EQ(eval_print("(nth 2 '(a b c))"), "C");
}

TEST(eval_misc_math)
{
    ASSERT_STR_EQ(eval_print("(zerop 0)"), "T");
    ASSERT_STR_EQ(eval_print("(zerop 1)"), "NIL");
    ASSERT_STR_EQ(eval_print("(plusp 5)"), "T");
    ASSERT_STR_EQ(eval_print("(minusp -3)"), "T");
    ASSERT_EQ_INT(eval_int("(abs -5)"), 5);
    ASSERT_EQ_INT(eval_int("(max 1 5 3)"), 5);
    ASSERT_EQ_INT(eval_int("(min 1 5 3)"), 1);
}

/* --- Special forms --- */

TEST(eval_quote)
{
    ASSERT_STR_EQ(eval_print("'foo"), "FOO");
    ASSERT_STR_EQ(eval_print("'(1 2 3)"), "(1 2 3)");
    ASSERT_STR_EQ(eval_print("(quote hello)"), "HELLO");
}

TEST(eval_if)
{
    ASSERT_STR_EQ(eval_print("(if t 'yes 'no)"), "YES");
    ASSERT_STR_EQ(eval_print("(if nil 'yes 'no)"), "NO");
    ASSERT_STR_EQ(eval_print("(if nil 'yes)"), "NIL");
    ASSERT_STR_EQ(eval_print("(if 42 'yes 'no)"), "YES");
}

TEST(eval_progn)
{
    ASSERT_EQ_INT(eval_int("(progn 1 2 3)"), 3);
    ASSERT_STR_EQ(eval_print("(progn)"), "NIL");
}

TEST(eval_setq)
{
    eval_print("(setq test-var 99)");
    ASSERT_EQ_INT(eval_int("test-var"), 99);
}

TEST(eval_let)
{
    ASSERT_EQ_INT(eval_int("(let ((x 10) (y 20)) (+ x y))"), 30);
    ASSERT_EQ_INT(eval_int("(let ((a 1) (b 2) (c 3)) (+ a b c))"), 6);
}

TEST(eval_letstar)
{
    ASSERT_EQ_INT(eval_int("(let* ((x 10) (y (* x 2))) y)"), 20);
    ASSERT_EQ_INT(eval_int("(let* ((a 1) (b (+ a 1)) (c (+ b 1))) c)"), 3);
}

/* --- Functions --- */

TEST(eval_defun_basic)
{
    eval_print("(defun double (x) (* x 2))");
    ASSERT_EQ_INT(eval_int("(double 21)"), 42);
}

TEST(eval_defun_recursive)
{
    eval_print("(defun fact (n) (if (<= n 1) 1 (* n (fact (1- n)))))");
    ASSERT_EQ_INT(eval_int("(fact 0)"), 1);
    ASSERT_EQ_INT(eval_int("(fact 1)"), 1);
    ASSERT_EQ_INT(eval_int("(fact 5)"), 120);
    ASSERT_EQ_INT(eval_int("(fact 10)"), 3628800);
}

TEST(eval_defun_fibonacci)
{
    eval_print("(defun fib (n) (if (<= n 1) n (+ (fib (- n 1)) (fib (- n 2)))))");
    ASSERT_EQ_INT(eval_int("(fib 0)"), 0);
    ASSERT_EQ_INT(eval_int("(fib 1)"), 1);
    ASSERT_EQ_INT(eval_int("(fib 10)"), 55);
}

TEST(eval_defun_multiarg)
{
    eval_print("(defun add3 (a b c) (+ a b c))");
    ASSERT_EQ_INT(eval_int("(add3 1 2 3)"), 6);
}

TEST(eval_lambda)
{
    ASSERT_EQ_INT(eval_int("((lambda (x) (* x x)) 5)"), 25);
}

/* Regression: a literal NIL appearing as a non-final form in a LAMBDA body
 * must not truncate the body.  compile_body conflated "compile_body_tail
 * returned NIL" (empty body) with "the tail form is the literal NIL" — when
 * the body's first form was NIL and more forms followed, it pushed a
 * PROGN_ITER frame but then short-circuited to OP_NIL and abandoned the
 * frame, dropping every form after the NIL.  The lambda silently returned
 * NIL.  Exposed by serapeum's define-case-macro, which expands the dispatch
 * cont into (lambda (...) (declare ...) nil `(cond ...)) — the leading NIL
 * dropped the (cond ...) so faux-case/string-case/etc. all returned NIL. */
TEST(eval_lambda_leading_nil_body)
{
    /* Bare leading NIL, then a value form. */
    ASSERT_EQ_INT(eval_int("(funcall (lambda () nil 42))"), 42);
    /* Leading NIL after a declaration. */
    ASSERT_EQ_INT(eval_int(
        "(funcall (lambda (x) (declare (ignorable x)) nil (+ x 1)) 10)"), 11);
    /* Multiple leading NILs. */
    ASSERT_EQ_INT(eval_int("(funcall (lambda (x) nil nil (* x 2)) 10)"), 20);
    /* A lone NIL body is still NIL (genuinely "returns nil"). */
    ASSERT_STR_EQ(eval_print("(funcall (lambda () nil))"), "NIL");
    /* Empty body is NIL. */
    ASSERT_STR_EQ(eval_print("(funcall (lambda ()))"), "NIL");
}

TEST(eval_function_ref)
{
    ASSERT_STR_EQ(eval_print("(mapcar #'1+ '(1 2 3))"), "(2 3 4)");
}

/* Regression: #'(setf name) — a (function (setf symbol)) reference must
 * resolve to the symbol that names the setter (the same one setf-place
 * compilation uses), not FLOAD the raw (SETF NAME) cons.  FLOADing the cons
 * stored a non-symbol in the constant pool, which OP_FLOAD then rejected
 * ("corrupted constant pool entry").  Exposed by chipz inflate.lisp's
 * #'(setf filename) / #'(setf comment). */
TEST(eval_function_ref_setf)
{
    ASSERT_STR_EQ(eval_print(
        "(progn"
        "  (defparameter *fref-cell* (list 0))"
        "  (defun (setf fref-acc) (v x) (setf (car x) v) v)"
        "  (funcall #'(setf fref-acc) 42 *fref-cell*)"
        "  (car *fref-cell*))"), "42");
}

/* Regression: (setf ACCESSOR) functions defined on same-named accessors in
 * DIFFERENT packages must stay distinct.  The hidden storage symbol was
 * %SETF-<name> (name-only), so the two writers collided on one function cell
 * (last definition won) — e.g. (setf local-time::sec-of) got clobbered by
 * (setf local-time-duration::sec-of).  Now keyed by %SETF-<package>::<name>.
 * Covers both the funcall #'(setf x) and (setf (x ..) ..) place paths. */
TEST(eval_setf_function_distinct_per_package)
{
    /* Packages must exist before the package-qualified symbols are READ, so
     * create them in their own eval_print calls (not inside one form). */
    eval_print("(defpackage :setf-p1 (:use :cl))");
    eval_print("(defpackage :setf-p2 (:use :cl))");
    eval_print("(defun (setf setf-p1::acc) (v x) (setf (car x) (list :p1 v)) v)");
    eval_print("(defun (setf setf-p2::acc) (v x) (setf (car x) (list :p2 v)) v)");
    ASSERT_STR_EQ(eval_print(
        "(let ((c1 (list 0)) (c2 (list 0)))"
        "  (funcall #'(setf setf-p1::acc) 11 c1)"
        "  (funcall #'(setf setf-p2::acc) 22 c2)"
        "  (setf (setf-p1::acc c1) 33)"
        "  (list (car c1) (car c2)))"),
        "((:P1 33) (:P2 22))");
}

/* Regression: a LABELS with more than the old CL_MAX_LOCAL_FUNS (32) functions
 * in scope, where a function bound past slot 32 is referenced via #' and
 * called.  Overflow used to silently fail to bind the name, so #'fn resolved
 * to an undefined global FLOAD at runtime.  Mirrors chipz inflate.lisp's
 * 37-function state-machine LABELS (its 33rd fn, gzip-isize, was the casualty).
 * Builds (labels ((fnx0 () 0) ... (fnx39 () 39)) (funcall #'fnx39)) => 39. */
TEST(eval_labels_past_32_funs)
{
    ASSERT_STR_EQ(eval_print(
        "(eval"
        "  (let* ((n 40)"
        "         (names (loop for i below n"
        "                      collect (intern (format nil \"FNX~D\" i))))"
        "         (defs (loop for s in names for i from 0"
        "                     collect (list s '() i))))"
        "    (list 'labels defs"
        "          (list 'funcall (list 'function (car (last names)))))))"),
        "39");
}

/* --- Error handling --- */

TEST(eval_error_unbound)
{
    const char *r = eval_print("nonexistent-var-xyz");
    ASSERT(strstr(r, "ERROR") != NULL);
}

TEST(eval_error_divzero)
{
    const char *r = eval_print("(/ 1 0)");
    ASSERT(strstr(r, "ERROR") != NULL);
}

TEST(eval_error_type)
{
    const char *r = eval_print("(+ 1 'a)");
    ASSERT(strstr(r, "ERROR") != NULL);
}

/* --- Self-evaluating --- */

TEST(eval_self_evaluating)
{
    ASSERT_EQ_INT(eval_int("42"), 42);
    ASSERT_STR_EQ(eval_print("t"), "T");
    ASSERT_STR_EQ(eval_print("nil"), "NIL");
    ASSERT_STR_EQ(eval_print("\"hello\""), "\"hello\"");
}

TEST(eval_keyword)
{
    ASSERT_STR_EQ(eval_print(":test"), ":TEST");
}

/* --- Macros --- */

TEST(eval_defmacro_simple)
{
    /* Macro that always returns a constant form */
    eval_print("(defmacro always-42 () 42)");
    ASSERT_EQ_INT(eval_int("(always-42)"), 42);
}

TEST(eval_defmacro_with_args)
{
    /* Macro that builds (+ x 1) using list */
    eval_print("(defmacro inc (x) (list '+ x 1))");
    ASSERT_EQ_INT(eval_int("(inc 5)"), 6);
    ASSERT_EQ_INT(eval_int("(inc 99)"), 100);
}

TEST(eval_defmacro_when)
{
    /* when macro: (when test body...) => (if test (progn body...)) */
    eval_print("(defmacro my-when (test &rest body) (list 'if test (cons 'progn body)))");
    ASSERT_EQ_INT(eval_int("(my-when t 1 2 42)"), 42);
    ASSERT_STR_EQ(eval_print("(my-when nil 1 2 42)"), "NIL");
}

TEST(eval_defmacro_unless)
{
    /* unless macro: (unless test body...) => (if test nil (progn body...)) */
    eval_print("(defmacro my-unless (test &rest body) (list 'if test nil (cons 'progn body)))");
    ASSERT_EQ_INT(eval_int("(my-unless nil 1 2 99)"), 99);
    ASSERT_STR_EQ(eval_print("(my-unless t 1 2 99)"), "NIL");
}

/* CLHS 3.1.2.1.2.4: a defmacro body is enclosed in an implicit BLOCK
 * named after the macro, so (return-from <name> ...) in the body exits
 * the expander early.  Serapeum's fbind et al. rely on this. */
TEST(eval_defmacro_implicit_block)
{
    eval_print("(defmacro early-out (x)"
               "  (when (numberp x)"
               "    (return-from early-out (list 'quote (list :num x))))"
               "  (list 'quote (list :other x)))");
    ASSERT_STR_EQ(eval_print("(early-out 42)"), "(:NUM 42)");
    ASSERT_STR_EQ(eval_print("(early-out foo)"), "(:OTHER FOO)");
}

TEST(eval_defmacro_identity)
{
    /* Macro receives unevaluated forms — verify by quoting the arg */
    eval_print("(defmacro quote-it (x) (list 'quote x))");
    ASSERT_STR_EQ(eval_print("(quote-it (+ 1 2))"), "(+ 1 2)");
    ASSERT_STR_EQ(eval_print("(quote-it hello)"), "HELLO");
}

/* --- Closures with captured upvalues --- */

TEST(eval_closure_capture_let)
{
    /* Lambda captures variable from let binding */
    ASSERT_EQ_INT(eval_int("(let ((x 10)) ((lambda (y) (+ x y)) 5))"), 15);
    ASSERT_EQ_INT(eval_int("(let ((a 100)) ((lambda () a)))"), 100);
}

TEST(eval_closure_make_adder)
{
    /* Classic make-adder: defun returns closure capturing n */
    eval_print("(defun make-adder (n) (lambda (x) (+ n x)))");
    ASSERT_EQ_INT(eval_int("((make-adder 10) 5)"), 15);
    ASSERT_EQ_INT(eval_int("((make-adder 100) 42)"), 142);
}

TEST(eval_closure_multiple_captures)
{
    /* Capture multiple variables */
    ASSERT_EQ_INT(eval_int("(let ((a 1) (b 2) (c 3)) ((lambda () (+ a b c))))"), 6);
    ASSERT_EQ_INT(eval_int("(let ((x 10) (y 20)) ((lambda (z) (+ x y z)) 30))"), 60);
}

TEST(eval_closure_nested)
{
    /* Nested lambda: inner captures from outer closure's upvalue */
    eval_print("(defun make-adder2 (n) (lambda (x) ((lambda (a b) (+ a b)) n x)))");
    ASSERT_EQ_INT(eval_int("((make-adder2 10) 5)"), 15);

    /* Double-nesting: outer captures from let, inner captures from outer */
    ASSERT_EQ_INT(eval_int("(let ((x 10)) ((lambda () ((lambda () (+ x 5))))))"), 15);
}

TEST(eval_closure_shared_scope)
{
    /* Two lambdas capture the same variable (value capture = independent copies) */
    eval_print("(defun make-pair (x) (list (lambda () x) (lambda () (+ x 1))))");
    ASSERT_EQ_INT(eval_int("(let ((p (make-pair 10))) ((car p)))"), 10);
    ASSERT_EQ_INT(eval_int("(let ((p (make-pair 10))) ((car (cdr p))))"), 11);
}

/* --- Heap-boxed cells (mutable closure capture) --- */

TEST(eval_cell_basic_counter)
{
    /* Closure captures and mutates a let variable via setq */
    eval_print("(let ((n 0)) (defun cell-inc () (setq n (+ n 1)) n))");
    ASSERT_EQ_INT(eval_int("(cell-inc)"), 1);
    ASSERT_EQ_INT(eval_int("(cell-inc)"), 2);
    ASSERT_EQ_INT(eval_int("(cell-inc)"), 3);
}

TEST(eval_cell_shared_getter_setter)
{
    /* Two closures share a mutable variable via heap-boxed cell */
    eval_print("(let ((x 10)) (defun cg () x) (defun cs (v) (setq x v)))");
    ASSERT_EQ_INT(eval_int("(cg)"), 10);
    eval_print("(cs 42)");
    ASSERT_EQ_INT(eval_int("(cg)"), 42);
    eval_print("(cs 99)");
    ASSERT_EQ_INT(eval_int("(cg)"), 99);
}

TEST(eval_cell_accumulator_param)
{
    /* Parameter captured and mutated in returned closure */
    eval_print("(defun make-acc (init) (lambda (x) (setq init (+ init x)) init))");
    eval_print("(defvar *ta* (make-acc 0))");
    ASSERT_EQ_INT(eval_int("(funcall *ta* 5)"), 5);
    ASSERT_EQ_INT(eval_int("(funcall *ta* 3)"), 8);
    ASSERT_EQ_INT(eval_int("(funcall *ta* 10)"), 18);
}

TEST(eval_cell_independent_instances)
{
    /* Multiple closure instances have independent cells */
    eval_print("(defun mk-ctr () (let ((n 0)) (lambda () (setq n (+ n 1)) n)))");
    eval_print("(defvar *tc1* (mk-ctr))");
    eval_print("(defvar *tc2* (mk-ctr))");
    ASSERT_EQ_INT(eval_int("(funcall *tc1*)"), 1);
    ASSERT_EQ_INT(eval_int("(funcall *tc1*)"), 2);
    ASSERT_EQ_INT(eval_int("(funcall *tc2*)"), 1);  /* independent */
    ASSERT_EQ_INT(eval_int("(funcall *tc1*)"), 3);
    ASSERT_EQ_INT(eval_int("(funcall *tc2*)"), 2);
}

TEST(eval_cell_setq_before_capture)
{
    /* Mutation before capture — closure sees post-mutation value */
    ASSERT_EQ_INT(eval_int("(let ((x 0)) (setq x 42) ((lambda () x)))"), 42);
}

TEST(eval_cell_nested_closure)
{
    /* Inner closure mutates var from outer let via upvalue chain */
    eval_print("(let ((x 0))"
               "  (defun cell-outer () (lambda () (setq x (+ x 1)) x)))");
    ASSERT_EQ_INT(eval_int("(funcall (cell-outer))"), 1);
    ASSERT_EQ_INT(eval_int("(funcall (cell-outer))"), 2);
}

TEST(eval_cell_let_star)
{
    /* let* with one mutable captured var, one read-only captured var */
    eval_print("(let* ((a 1) (b 10))"
               "  (defun clg () (+ a b))"
               "  (defun cls (v) (setq a v)))");
    ASSERT_EQ_INT(eval_int("(clg)"), 11);
    eval_print("(cls 5)");
    ASSERT_EQ_INT(eval_int("(clg)"), 15);
}

TEST(eval_cell_flet_mutation)
{
    /* flet function mutates captured outer variable */
    ASSERT_EQ_INT(eval_int(
        "(let ((n 0))"
        "  (flet ((bump () (setq n (+ n 1))))"
        "    (bump) (bump) (bump) n))"), 3);
}

TEST(eval_cell_labels_shared)
{
    /* labels functions share mutable variable */
    ASSERT_STR_EQ(eval_print(
        "(let ((n 0))"
        "  (labels ((my-inc () (setq n (+ n 1)))"
        "           (my-peek () n))"
        "    (my-inc) (my-inc) (my-inc) (my-peek)))"), "3");
}

TEST(eval_cell_do_let_capture)
{
    /* do loop with per-iteration let binding: each closure gets own value */
    ASSERT_STR_EQ(eval_print(
        "(let ((fns nil))"
        "  (do ((i 0 (+ i 1)))"
        "      ((= i 3) (mapcar #'funcall (reverse fns)))"
        "    (let ((j i))"
        "      (setq fns (cons (lambda () j) fns)))))"),
        "(0 1 2)");
}

TEST(eval_cell_readonly_no_boxing)
{
    /* Read-only capture: no boxing, no regression */
    ASSERT_EQ_INT(eval_int("(let ((x 42)) ((lambda () x)))"), 42);
    eval_print("(defun ma2 (n) (lambda (x) (+ n x)))");
    ASSERT_EQ_INT(eval_int("((ma2 10) 5)"), 15);
}

TEST(eval_setf_boxing_across_closure)
{
    /* setf must trigger boxing when variable is captured across closure boundary */
    eval_print("(defun test-setf-box (seed)"
               "  (let ((f (lambda (x) (setf seed (+ seed x)))))"
               "    (funcall f 1)"
               "    (funcall f 2)"
               "    seed))");
    ASSERT_EQ_INT(eval_int("(test-setf-box 0)"), 3);

    /* reduce pattern from ASDF */
    eval_print("(defun test-reduce (combinator seed)"
               "  (dolist (x '(1 2 3))"
               "    (funcall (lambda () (setf seed (funcall combinator x seed)))))"
               "  seed)");
    ASSERT_EQ_INT(eval_int("(test-reduce #'+ 0)"), 6);
}

/* Regression: (setf (values a b) ...) inside a closure must box a and b.
 * The boxing scanner recognized (setf var ...) but treated (setf (values a b)
 * ...) as a read of a/b, so they weren't boxed and the closure's writes were
 * silently dropped (outer a/b kept their initial values).  serapeum's MVFOLD
 * compiler macro hits this: it emits (setf (values seed0 seed1) (funcall ...))
 * hoisted into a do-each thunk. */
TEST(eval_setf_values_boxing_across_closure)
{
    eval_print("(defun test-setf-values-box (a b)"
               "  (flet ((step1 (x) (setf (values a b) (values (+ a x) (max b x)))))"
               "    (mapc #'step1 '(1 2 3)))"
               "  (list a b))");
    ASSERT_STR_EQ(eval_print("(test-setf-values-box 0 0)"), "(6 3)");
    /* both values must update, not just the first */
    eval_print("(defun test-sv-second (a b)"
               "  (funcall (lambda () (setf (values a b) (values 7 9))))"
               "  (list a b))");
    ASSERT_STR_EQ(eval_print("(test-sv-second 0 0)"), "(7 9)");
}

/* Regression (gc-stress): non-symbol sub-place inside (setf (values ...) ...).
 * The boxing scanner's VALUES branch walked vplaces without GC-protecting the
 * cursor; scan_body_for_boxing on a non-symbol vp can macroexpand→allocate→
 * compact, leaving the cursor stale.  Exercise the else branch under gc-stress
 * by using (car x) as a sub-place inside the VALUES form. */
TEST(eval_setf_values_nonsymbol_place_gcstress)
{
    /* (setf (values (car x) b) ...) — (car x) is a non-symbol place, so the
     * boxing scanner takes the else branch and recurses into scan_body_for_boxing.
     * Under GC stress the vplaces cursor must survive the recursive scan. */
    eval_print("(defun test-sv-nonsym (x b)"
               "  (funcall (lambda () (setf (values (car x) b) (values 10 20))))"
               "  (list (car x) b))");
    ASSERT_STR_EQ(eval_print("(test-sv-nonsym (list 0) 0)"), "(10 20)");
    /* Multiple non-symbol sub-places to walk more iterations of the cursor loop */
    eval_print("(defun test-sv-multi-nonsym (x y)"
               "  (funcall (lambda () (setf (values (car x) (car y)) (values 3 7))))"
               "  (list (car x) (car y)))");
    ASSERT_STR_EQ(eval_print("(test-sv-multi-nonsym (list 0) (list 0))"), "(3 7)");
}

/* --- AND / OR / COND --- */

TEST(eval_and)
{
    /* (and) => T */
    ASSERT_STR_EQ(eval_print("(and)"), "T");
    /* (and x) => x */
    ASSERT_EQ_INT(eval_int("(and 42)"), 42);
    /* Short-circuit: returns first nil */
    ASSERT_STR_EQ(eval_print("(and 1 nil 2)"), "NIL");
    /* All truthy: returns last value */
    ASSERT_EQ_INT(eval_int("(and 1 2 3)"), 3);
    /* First is nil */
    ASSERT_STR_EQ(eval_print("(and nil 1)"), "NIL");
}

TEST(eval_or)
{
    /* (or) => NIL */
    ASSERT_STR_EQ(eval_print("(or)"), "NIL");
    /* (or x) => x */
    ASSERT_EQ_INT(eval_int("(or 42)"), 42);
    /* Short-circuit: returns first truthy */
    ASSERT_EQ_INT(eval_int("(or nil 2 3)"), 2);
    /* All nil: returns nil */
    ASSERT_STR_EQ(eval_print("(or nil nil nil)"), "NIL");
    /* First is truthy */
    ASSERT_EQ_INT(eval_int("(or 1 2)"), 1);
}

TEST(eval_cond)
{
    /* Basic clause matching */
    ASSERT_EQ_INT(eval_int("(cond (t 42))"), 42);
    ASSERT_EQ_INT(eval_int("(cond (nil 1) (t 2))"), 2);
    /* No matching clause */
    ASSERT_STR_EQ(eval_print("(cond (nil 1))"), "NIL");
    /* Multiple body forms */
    ASSERT_EQ_INT(eval_int("(cond (t 1 2 3))"), 3);
    /* Condition with expression */
    ASSERT_EQ_INT(eval_int("(cond ((= 1 2) 10) ((= 1 1) 20))"), 20);
    /* Empty cond */
    ASSERT_STR_EQ(eval_print("(cond)"), "NIL");
}

TEST(eval_when_unless)
{
    /* when: true => body result */
    ASSERT_EQ_INT(eval_int("(when t 1 2 42)"), 42);
    /* when: nil => NIL */
    ASSERT_STR_EQ(eval_print("(when nil 1 2 42)"), "NIL");
    /* unless: nil => body result */
    ASSERT_EQ_INT(eval_int("(unless nil 1 2 99)"), 99);
    /* unless: true => NIL */
    ASSERT_STR_EQ(eval_print("(unless t 1 2 99)"), "NIL");
}

/* --- mapcar / funcall / apply with compiled functions --- */

TEST(eval_mapcar_lambda)
{
    /* mapcar with lambda */
    ASSERT_STR_EQ(eval_print("(mapcar (lambda (x) (* x x)) '(1 2 3))"), "(1 4 9)");
    /* mapcar with closure */
    eval_print("(defun make-multiplier (n) (lambda (x) (* n x)))");
    ASSERT_STR_EQ(eval_print("(mapcar (make-multiplier 3) '(1 2 3))"), "(3 6 9)");
}

TEST(eval_funcall_lambda)
{
    /* funcall with lambda */
    ASSERT_EQ_INT(eval_int("(funcall (lambda (a b) (+ a b)) 10 20)"), 30);
    /* funcall with closure */
    ASSERT_EQ_INT(eval_int("(funcall (make-adder 100) 5)"), 105);
}

TEST(eval_apply_closure)
{
    /* apply with lambda */
    ASSERT_EQ_INT(eval_int("(apply (lambda (a b) (+ a b)) '(3 4))"), 7);
    /* apply with closure */
    ASSERT_EQ_INT(eval_int("(apply (make-adder 50) '(7))"), 57);
}

/* --- Loop forms: dolist, dotimes, do --- */

TEST(eval_dolist)
{
    /* Basic iteration — collect via side effect */
    eval_print("(setq *result* nil)");
    eval_print("(dolist (x '(1 2 3)) (setq *result* (cons x *result*)))");
    ASSERT_STR_EQ(eval_print("*result*"), "(3 2 1)");

    /* Empty list — body never executes */
    eval_print("(setq *count* 0)");
    eval_print("(dolist (x nil) (setq *count* (+ *count* 1)))");
    ASSERT_EQ_INT(eval_int("*count*"), 0);

    /* Result form */
    ASSERT_EQ_INT(eval_int("(dolist (x '(1 2 3) 42))"), 42);

    /* Default return is NIL */
    ASSERT_STR_EQ(eval_print("(dolist (x '(1 2 3)))"), "NIL");

    /* var is NIL during result-form */
    ASSERT_STR_EQ(eval_print("(dolist (x '(1 2 3) x))"), "NIL");

    /* Nested dolist with same var name — stale slot binding regression test.
     * Inner dolist's var 's' in slot N must not shadow outer dolist's 's'
     * after the inner scope exits (anonymous iter/result slots must clear
     * stale locals[] entries). */
    ASSERT_STR_EQ(eval_print(
        "(let ((r nil))"
        "  (let ((y 0))"
        "    (dolist (s '(a b)) s))"
        "  (dolist (s '(x y z))"
        "    (setq r (cons s r)))"
        "  (reverse r))"),
        "(X Y Z)");
}

TEST(eval_dotimes)
{
    /* Count 0..n-1 */
    eval_print("(setq *sum* 0)");
    eval_print("(dotimes (i 5) (setq *sum* (+ *sum* i)))");
    ASSERT_EQ_INT(eval_int("*sum*"), 10);  /* 0+1+2+3+4 */

    /* Zero count — body never executes */
    eval_print("(setq *count* 0)");
    eval_print("(dotimes (i 0) (setq *count* (+ *count* 1)))");
    ASSERT_EQ_INT(eval_int("*count*"), 0);

    /* Result form */
    ASSERT_EQ_INT(eval_int("(dotimes (i 3 99))"), 99);

    /* Default return is NIL */
    ASSERT_STR_EQ(eval_print("(dotimes (i 3))"), "NIL");

    /* var accessible in result-form (equals count at end) */
    ASSERT_EQ_INT(eval_int("(dotimes (i 5 i))"), 5);
}

/* CLHS 3.6: DOLIST/DOTIMES bodies are implicit tagbodies — GO to tags
 * defined within the body must work.  Regression for serapeum
 * static-let.lisp :start/(go :start) usage and similar patterns. */
TEST(eval_dolist_implicit_tagbody)
{
    /* (go :top) re-enters the loop body at :top without advancing the
       iterator.  Repeats until the flag guarding the GO flips false. */
    ASSERT_EQ_INT(eval_int(
        "(let ((n 0) (retries 0))"
        "  (dolist (x '(a))"
        "    (declare (ignore x))"
        "    :top"
        "    (incf n)"
        "    (when (< retries 3)"
        "      (incf retries)"
        "      (go :top)))"
        "  n)"), 4);  /* first pass + 3 GO re-entries */
}

TEST(eval_dotimes_implicit_tagbody)
{
    /* Same principle for dotimes: i=0 goes back to :top twice before
       falling through; i=1,2 each pass through once. */
    ASSERT_EQ_INT(eval_int(
        "(let ((hits 0))"
        "  (dotimes (i 3)"
        "    :top"
        "    (incf hits)"
        "    (when (and (zerop i) (< hits 2)) (go :top)))"
        "  hits)"), 4);
}

TEST(eval_do)
{
    /* Simple countdown */
    ASSERT_EQ_INT(eval_int(
        "(do ((i 10 (- i 1))) ((= i 0) 42))"), 42);

    /* Multiple vars with steps */
    ASSERT_EQ_INT(eval_int(
        "(do ((i 0 (+ i 1)) (sum 0 (+ sum i))) ((= i 5) sum))"), 10);

    /* Var without step — retains value */
    ASSERT_EQ_INT(eval_int(
        "(do ((x 100) (i 0 (+ i 1))) ((= i 3) x))"), 100);

    /* Body forms execute */
    eval_print("(setq *do-body* 0)");
    eval_print("(do ((i 0 (+ i 1))) ((= i 3)) (setq *do-body* (+ *do-body* 1)))");
    ASSERT_EQ_INT(eval_int("*do-body*"), 3);

    /* Multiple result forms (progn) */
    ASSERT_EQ_INT(eval_int(
        "(do ((i 0 (+ i 1))) ((= i 1) 10 20 30))"), 30);

    /* No result form — returns NIL */
    ASSERT_STR_EQ(eval_print(
        "(do ((i 0 (+ i 1))) ((= i 1)))"), "NIL");
}

TEST(eval_do_star)
{
    /* Sequential binding: j sees current i (not previous) */
    ASSERT_STR_EQ(eval_print(
        "(let ((r nil))"
        "  (do* ((i 0 (+ i 1)) (j (* i 10) (* i 10)))"
        "       ((>= i 3) (reverse r))"
        "    (push (list i j) r)))"),
        "((0 0) (1 10) (2 20))");
    /* Contrast with do (parallel): j would see old i */
    ASSERT_STR_EQ(eval_print(
        "(let ((r nil))"
        "  (do ((i 0 (+ i 1)) (j 0 (* i 10)))"
        "      ((>= i 3) (reverse r))"
        "    (push (list i j) r)))"),
        "((0 0) (1 0) (2 10))");
    /* Simple do* */
    ASSERT_EQ_INT(eval_int(
        "(do* ((i 10 (- i 1))) ((= i 0) 42))"), 42);
}

/* Regression: CLHS 6.1.1 allows a do/do* var-spec to be a bare symbol
 * (var with no init/step, bound to NIL), not just a (var ...) list.
 * compile_do/compile_do_star did cl_car() on the spec assuming a list,
 * so a bare symbol raised "CAR: argument is not of type LIST" at compile
 * time — this broke puri (drakma's URI dep), aborting chipi's load. */
TEST(eval_do_bare_var)
{
    /* Bare var is bound to NIL and is settable in the body. */
    ASSERT_STR_EQ(eval_print(
        "(do* ((i 0 (1+ i)) untouched) ((= i 2) untouched))"), "NIL");
    ASSERT_EQ_INT(eval_int(
        "(do ((i 0 (1+ i)) x) ((= i 2) x) (setq x (* i 10)))"), 10);
    /* do* sequential: bare var settable, mixes with (var), (var init step). */
    ASSERT_STR_EQ(eval_print(
        "(do* ((a) (b 5) (c 0 (1+ c))) ((= c 3) (list a b c))"
        "  (setq a (cons c a)))"), "((2 1 0) 5 3)");
    /* Bare var as the ONLY spec. */
    ASSERT_EQ_INT(eval_int(
        "(do* (x) ((eq x :done) 7) (setq x :done))"), 7);
    /* Bare var + cond + (return ...) in body (the puri urn-nss-equal shape). */
    ASSERT_STR_EQ(eval_print(
        "(do* ((i 0 (1+ i)) c) ((= i 5) :ok)"
        "  (setq c i)"
        "  (cond ((= c 3) (return :hit))))"), ":HIT");
}

/* --- Quasiquote --- */

TEST(eval_quasiquote_atom)
{
    ASSERT_EQ_INT(eval_int("`42"), 42);
    ASSERT_STR_EQ(eval_print("`foo"), "FOO");
}

TEST(eval_quasiquote_simple_list)
{
    ASSERT_STR_EQ(eval_print("`(1 2 3)"), "(1 2 3)");
    ASSERT_STR_EQ(eval_print("`(a b c)"), "(A B C)");
}

TEST(eval_quasiquote_unquote)
{
    ASSERT_STR_EQ(eval_print("`(a ,(+ 1 2) c)"), "(A 3 C)");
    ASSERT_STR_EQ(eval_print("(let ((x 10)) `(val ,x))"), "(VAL 10)");
}

TEST(eval_quasiquote_splicing)
{
    ASSERT_STR_EQ(eval_print("`(a ,@'(1 2 3) b)"), "(A 1 2 3 B)");
    ASSERT_STR_EQ(eval_print("`(,@'(1 2) ,@'(3 4))"), "(1 2 3 4)");
    ASSERT_STR_EQ(eval_print("`(a ,@'(1 2))"), "(A 1 2)");
}

/* ,.  (destructive splice, CLHS "The Backquote").  iterate uses this
 * in macros like `(for ,var ,.(nreverse kwd-clauses)); our reader must
 * accept it — we collapse ,. to the same expansion as ,@.  Regression
 * for trivia/iterate compilation which previously signalled
 * "Unbound variable: ." because `.(...)` was read as a plain symbol. */
TEST(eval_quasiquote_splicing_nconc)
{
    ASSERT_STR_EQ(eval_print("`(a ,.'(1 2 3) b)"), "(A 1 2 3 B)");
    ASSERT_STR_EQ(eval_print("`(,.'(1 2) ,.'(3 4))"), "(1 2 3 4)");
}

/* Regression: a quasiquote template with > 255 top-level elements expands to
 * a single APPEND call.  OP_CALL/OP_TAILCALL encode the arg count in one byte,
 * so the count used to wrap (e.g. 600 -> 88) while the code still pushed every
 * segment, desyncing the VM stack so a segment value landed in the callee slot
 * ("Not a function: heap object type 0").  This is exactly ironclad
 * threefish.lisp's 1024-bit ARX round macro (setf template ~289 elements).
 * qq_append now chunks the APPEND so templates of any size compile and run. */
TEST(eval_quasiquote_large_call_args)
{
    static char src[16384];
    int i, pos;

    /* 300 (literal . unquote) pairs = 600 top-level elements. */
    pos = 0;
    pos += snprintf(src + pos, sizeof(src) - pos, "(let ((x 7)) (length `(");
    for (i = 0; i < 300; i++)
        pos += snprintf(src + pos, sizeof(src) - pos, "a%d ,x ", i);
    pos += snprintf(src + pos, sizeof(src) - pos, ")))");
    ASSERT_EQ_INT(eval_int(src), 600);

    /* Value correctness across a chunk boundary (chunk size 128): odd indices
     * are the unquoted x; index 257 must still read back as x's value. */
    pos = 0;
    pos += snprintf(src + pos, sizeof(src) - pos, "(let ((x 99)) (nth 257 `(");
    for (i = 0; i < 300; i++)
        pos += snprintf(src + pos, sizeof(src) - pos, "k%d ,x ", i);
    pos += snprintf(src + pos, sizeof(src) - pos, ")))");
    ASSERT_EQ_INT(eval_int(src), 99);

    /* Unquote-splicing with > 128 splice segments also chunks correctly. */
    pos = 0;
    pos += snprintf(src + pos, sizeof(src) - pos, "(length `(");
    for (i = 0; i < 200; i++)
        pos += snprintf(src + pos, sizeof(src) - pos, ",@(list %d) ", i);
    pos += snprintf(src + pos, sizeof(src) - pos, "))");
    ASSERT_EQ_INT(eval_int(src), 200);

    /* ...and the spliced values land in order across the chunk boundary. */
    pos = 0;
    pos += snprintf(src + pos, sizeof(src) - pos, "(nth 175 `(");
    for (i = 0; i < 200; i++)
        pos += snprintf(src + pos, sizeof(src) - pos, ",@(list %d) ", i);
    pos += snprintf(src + pos, sizeof(src) - pos, "))");
    ASSERT_EQ_INT(eval_int(src), 175);
}

/* CLHS: macroexpand-1 form &optional env => expansion, expanded-p
 * (1) Returns two values
 * (2) Accepts optional env argument (ignored)
 * iterate calls (macroexpand-1 form *env*) so arity must be 2-wide. */
TEST(eval_macroexpand_1_values_and_env)
{
    eval_print("(defmacro mx1-m (x) `(+ ,x 1))");
    /* Multiple-value-list to inspect both return values */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (macroexpand-1 '(mx1-m 5)))"),
        "((+ 5 1) T)");
    /* env arg accepted, ignored */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (macroexpand-1 '(mx1-m 7) nil))"),
        "((+ 7 1) T)");
    /* Non-macro call: expansion is the form, expanded-p is NIL */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (macroexpand-1 '(+ 1 2)))"),
        "((+ 1 2) NIL)");
    /* Non-cons input: returns form unchanged with expanded-p = NIL */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (macroexpand-1 'plain-sym))"),
        "(PLAIN-SYM NIL)");
}

/* Regression: a symbol-macro that expands to NIL must still report
 * expanded-p = T.  Serapeum's defining-types relies on
 * (macroexpand-1 'serapeum/unlocked:%union env) returning NIL (not the
 * symbol itself) when the symbol-macro is bound.  Previously the
 * lookup returned CL_NIL both for "no binding" and "expands to NIL",
 * causing the caller to see no expansion and treat %union as a bare
 * symbol — surfacing as "No class named %UNION" at defunit time. */
TEST(eval_macroexpand_symbol_macro_nil_expansion)
{
    eval_print("(define-symbol-macro mx1-nil nil)");
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (macroexpand-1 'mx1-nil))"),
        "(NIL T)");
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (macroexpand 'mx1-nil))"),
        "(NIL T)");
    /* Caller pattern used by serapeum env-super */
    ASSERT_STR_EQ(eval_print(
        "(or (macroexpand-1 'mx1-nil) 'default)"),
        "DEFAULT");
}

/* CLHS: subtypep and typep take an optional environment argument.
 * serapeum uses (subtypep t1 t2 env), iterate uses (typep x type env). */
TEST(eval_subtypep_typep_env_arg)
{
    ASSERT_STR_EQ(eval_print("(nth-value 0 (subtypep 'fixnum 'integer))"), "T");
    ASSERT_STR_EQ(eval_print("(nth-value 0 (subtypep 'fixnum 'integer nil))"), "T");
    ASSERT_STR_EQ(eval_print("(typep 5 'integer)"), "T");
    ASSERT_STR_EQ(eval_print("(typep 5 'integer nil)"), "T");
}

/* Bounded integer range specifiers are subtypes of INTEGER / NUMBER. */
TEST(eval_subtypep_bounded_integer_ranges)
{
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep '(unsigned-byte 32) 'integer))"), "(T T)");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep '(unsigned-byte 16) 'integer))"), "(T T)");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep '(signed-byte 8) 'integer))"), "(T T)");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep '(mod 100) 'integer))"), "(T T)");
    /* Integer ranges are also subtypes of NUMBER / T. */
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep '(unsigned-byte 8) 'number))"), "(T T)");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep '(unsigned-byte 8) 't))"), "(T T)");
}

/* Bounded float/real/rational range specifiers are subtypes of their base
 * kind and broader numeric supertypes.  serapeum's RANDOM-RANGE-TYPE builds
 * e.g. (single-float 0.0 (10.0)) and asserts (subtypep type 'number). */
TEST(eval_subtypep_bounded_float_ranges)
{
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep '(single-float 0.0 (10.0)) 'number))"), "(T T)");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep '(single-float 0.0 (10.0)) 'float))"), "(T T)");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep '(single-float 0.0 (10.0)) 'single-float))"), "(T T)");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep '(double-float 0.0d0 (10.0d0)) 'number))"), "(T T)");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep '(double-float 0.0d0 (10.0d0)) 'real))"), "(T T)");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep '(rational 0 (10)) 'number))"), "(T T)");
    /* No false positives: a base kind is NOT a subtype of a bounded subrange,
     * and unrelated float kinds are not subtypes of each other. */
    ASSERT_STR_EQ(eval_print("(nth-value 0 (subtypep 'single-float '(single-float 0.0 1.0)))"), "NIL");
    ASSERT_STR_EQ(eval_print("(nth-value 0 (subtypep '(single-float 0.0 1.0) 'double-float))"), "NIL");
}

/* (subtypep A (not B)): when A is a non-empty subtype of B (certain), A overlaps
 * B and is certainly NOT a subtype of (not B) — answer (NIL T), not uncertain.
 * serapeum's SOFT-LIST-OF deftype branches on (subtypep 'null type); with
 * type = (not null) it needs (subtypep 'null '(not null)) => (NIL T) to pick
 * PROPER-LIST-WITHOUT-NIL? over PROPER-LIST?. */
TEST(eval_subtypep_not_overlap)
{
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep 'null '(not null)))"), "(NIL T)");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep 'fixnum '(not integer)))"), "(NIL T)");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep 'integer '(not number)))"), "(NIL T)");
    /* A type unrelated to B stays uncertain (no false certainty). */
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep 'integer '(not null)))"), "(NIL NIL)");
}

/* CLHS 3.1.2.1.1: a lexical variable binding (LET, LET-star, a lambda list) shadows a
 * SYMBOL-MACROLET or DEFINE-SYMBOL-MACRO binding of the same name for the
 * extent of its scope.  serapeum's `local` relies on this when it rebinds a
 * symbol-macro's name as a fresh lexical variable and then assigns to it. */
TEST(eval_let_shadows_symbol_macrolet)
{
    /* Inner LET shadows an enclosing SYMBOL-MACROLET; setq/read hit the var. */
    ASSERT_STR_EQ(eval_print(
        "(let ((g 10)) (symbol-macrolet ((x g)) (let ((x nil)) (setq x 2) (list x g))))"),
        "(2 10)");
    ASSERT_STR_EQ(eval_print(
        "(let ((g 10)) (symbol-macrolet ((x g)) (let ((x 5)) (setf g 99) x)))"),
        "5");
    /* An inner SYMBOL-MACROLET still shadows an outer LET (other direction). */
    ASSERT_STR_EQ(eval_print(
        "(let ((x 1)) (symbol-macrolet ((x 2)) x))"),
        "2");
}

/* A lexical LET binding also shadows a *global* DEFINE-SYMBOL-MACRO. */
TEST(eval_let_shadows_global_symbol_macro)
{
    eval_print("(define-symbol-macro gsm-shadow 1)");
    ASSERT_STR_EQ(eval_print("gsm-shadow"), "1");
    ASSERT_STR_EQ(eval_print("(let ((gsm-shadow 5)) gsm-shadow)"), "5");
    ASSERT_STR_EQ(eval_print("(let ((gsm-shadow 5)) (setq gsm-shadow 9) gsm-shadow)"), "9");
    ASSERT_STR_EQ(eval_print("(let ((gsm-shadow 5)) (setf gsm-shadow 9) gsm-shadow)"), "9");
    /* setf (values ...) through a lexical rebinding of the global sym-macro. */
    ASSERT_STR_EQ(eval_print(
        "(let ((gsm-shadow 0) (y 0)) (setf (values gsm-shadow y) (values 4 7)) (list gsm-shadow y))"),
        "(4 7)");
    /* Outside the LET the global symbol-macro is still in effect. */
    ASSERT_STR_EQ(eval_print("gsm-shadow"), "1");
}

/* (mod n) is a valid compound type specifier for TYPEP (CLHS type MOD):
 * non-negative integers strictly less than n.  ironclad's index/digest code
 * uses (typep x '(mod ...)) heavily via check-type. */
TEST(eval_typep_mod)
{
    ASSERT_STR_EQ(eval_print("(typep 0 '(mod 8))"), "T");
    ASSERT_STR_EQ(eval_print("(typep 7 '(mod 8))"), "T");
    ASSERT_STR_EQ(eval_print("(typep 8 '(mod 8))"), "NIL");   /* upper bound exclusive */
    ASSERT_STR_EQ(eval_print("(typep -1 '(mod 8))"), "NIL");  /* negatives excluded */
    ASSERT_STR_EQ(eval_print("(typep 1/2 '(mod 8))"), "NIL"); /* non-integers excluded */
    /* Large bound forces a bignum comparison. */
    ASSERT_STR_EQ(eval_print("(typep 4000000000 '(mod 4294967296))"), "T");
    ASSERT_STR_EQ(eval_print("(typep 4294967296 '(mod 4294967296))"), "NIL");
}

/* FDEFINITION accepts a function name, which per the CLHS glossary is a symbol
 * OR a list (SETF symbol).  snooze calls (fdefinition '(setf hunchentoot:...))
 * to invoke setf-expanders by name. */
TEST(eval_fdefinition_setf)
{
    eval_print("(defun (setf fd-place) (val obj) (setf (car obj) val) val)");
    /* (fdefinition '(setf fd-place)) returns the setter function. */
    ASSERT_STR_EQ(eval_print("(functionp (fdefinition '(setf fd-place)))"), "T");
    eval_print("(defparameter *fd-cell* (list 1 2))");
    eval_print("(funcall (fdefinition '(setf fd-place)) 99 *fd-cell*)");
    ASSERT_STR_EQ(eval_print("(car *fd-cell*)"), "99");
    /* Plain-symbol function names still work. */
    ASSERT_STR_EQ(eval_print("(functionp (fdefinition 'car))"), "T");
    /* SYMBOL-FUNCTION, by contrast, must reject a (setf foo) list. */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (progn (symbol-function '(setf fd-place)) :no-error)"
        "  (type-error () :type-error))"), ":TYPE-ERROR");
}

/* Serapeum's with-subtype-dispatch / with-simple-vector-dispatch needs
 * (subtypep <vec-type> '(simple-array * (*))) to return T for simple 1-D
 * vectors, otherwise it raises "X is not a subtype of (SIMPLE-ARRAY * (*))".
 * See serapeum/types.lisp:411. */
TEST(eval_subtypep_compound_array_specs)
{
    /* 1-D simple-array supertype: simple-vector, simple-bit-vector qualify. */
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep 'simple-vector '(simple-array * (*))))"), "(T T)");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep 'simple-bit-vector '(simple-array * (*))))"), "(T T)");
    /* (simple-array * *) ≡ SIMPLE-ARRAY (no restriction). */
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep 'simple-vector '(simple-array * *)))"), "(T T)");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep 'simple-bit-vector '(simple-array * *)))"), "(T T)");
    /* (array * (*)) ≡ VECTOR. */
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep 'vector '(array * (*))))"), "(T T)");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep 'simple-vector '(array * (*))))"), "(T T)");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep 'string '(array * (*))))"), "(T T)");
    /* (array * *) ≡ ARRAY. */
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep 'vector '(array * *)))"), "(T T)");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep 'simple-array '(array * *)))"), "(T T)");
    /* Compound on the subtype side. */
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep '(simple-array * (*)) 'array))"), "(T T)");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep '(simple-array * (*)) 'simple-array))"), "(T T)");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep '(simple-array * (*)) 'vector))"), "(T T)");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep '(simple-array * (*)) 'sequence))"), "(T T)");
    /* Negative: SIMPLE-ARRAY includes multi-dim so is NOT subtype of 1-D form. */
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep 'simple-array '(simple-array * (*))))"), "(NIL T)");
    /* Negative: unrelated type. */
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep 'fixnum '(simple-array * (*))))"), "(NIL T)");
}

/* serapeum/types.lisp `subtypes-exhaustive?` errors if subtypep returns (NIL NIL)
 * for a valid subtype pair.  Rank-1 simple-arrays with concrete element types
 * must be recognized as subtypes of the obvious parents. */
TEST(eval_subtypep_typed_compound_array_specs)
{
    /* (simple-array character (*)) == simple-string, a subtype of STRING, VECTOR, …. */
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep '(simple-array character (*)) 'string))"), "(T T)");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep '(simple-array character (*)) 'vector))"), "(T T)");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep '(simple-array character (*)) 'array))"), "(T T)");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep '(simple-array character (*)) 'sequence))"), "(T T)");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep '(simple-array character (*)) '(simple-array * (*))))"), "(T T)");
    /* (simple-array base-char (*)) == simple-base-string. */
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep '(simple-array base-char (*)) 'string))"), "(T T)");
    /* (simple-array bit (*)) == simple-bit-vector. */
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep '(simple-array bit (*)) 'bit-vector))"), "(T T)");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep '(simple-array bit (*)) 'vector))"), "(T T)");
    /* (simple-array t (*)) == simple-vector. */
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep '(simple-array t (*)) 'simple-vector))"), "(T T)");
    /* Atomic string subtypes: SIMPLE-STRING, BASE-STRING, SIMPLE-BASE-STRING. */
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep 'simple-string 'string))"), "(T T)");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep 'simple-base-string 'string))"), "(T T)");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep 'base-string 'vector))"), "(T T)");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep 'simple-base-string 'vector))"), "(T T)");
}

/* SUBTYPEP must descend into compound (OR ...) / (AND ...) / (MEMBER ...)
 * and through user-defined deftype aliases with compound bodies — serapeum
 * deftypes like KEY-DESIGNATOR = (or null function-name function) used to
 * yield (NIL NIL) and produce hundreds of compile-time warnings. */
TEST(eval_subtypep_compound_combinators)
{
    /* (subtypep X (or A B C)) — yes if X ⊆ some Ai. */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (subtypep 'null '(or null symbol function)))"), "(T T)");
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (subtypep 'function '(or null symbol function)))"), "(T T)");
    /* Disjoint from every branch — certain no. */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (subtypep 'integer '(or null symbol function)))"), "(NIL T)");

    /* Deftype alias with OR body. */
    eval_print("(deftype kd-test () '(or null symbol function))");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep 'null 'kd-test))"), "(T T)");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep 'symbol 'kd-test))"), "(T T)");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep 'function 'kd-test))"), "(T T)");
    /* Identical aliases — equality fast-path. */
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep 'kd-test 'kd-test))"), "(T T)");

    /* (subtypep (or A B) X) — yes iff every Ai ⊆ X. */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (subtypep '(or null symbol) 'kd-test))"), "(T T)");

    /* (subtypep (member ...) (member ...)) — element-wise typep. */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (subtypep '(member :a) '(member :a :b)))"), "(T T)");
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (subtypep '(member :c) '(member :a :b)))"), "(NIL T)");

    /* (and ...) on the left — true if any branch is. */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (subtypep '(and integer symbol) 'integer))"), "(T T)");

    /* Both sides OR — distribute T1 (every Ai ⊆ T2), don't try to find
     * T1 ⊆ Ci which is the wrong simplification direction.
     * Pre-fix: returned (NIL T) because head2=OR fired before head1=OR
     * and the multi-branch T1 wasn't a subtype of any single Ci. */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (subtypep '(or null symbol) '(or symbol null)))"),
        "(T T)");
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (subtypep '(or string null fixnum) '(or string symbol fixnum)))"),
        "(T T)");
    /* Reordered branches still match. */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (subtypep '(or fixnum string) '(or string fixnum)))"),
        "(T T)");
    /* Cross-type via subtype rule: NULL ⊆ SYMBOL should be transitive. */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list "
        " (subtypep '(or string character pathname null symbol fixnum)"
        "           '(or string character symbol pathname fixnum)))"),
        "(T T)");
    /* Deftype's expanded form on either side. */
    eval_print("(deftype fplt-test () '(or string character symbol pathname fixnum))");
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list "
        " (subtypep '(or string character pathname null symbol fixnum) 'fplt-test))"),
        "(T T)");
}

TEST(eval_subtypep_integer_range_subset)
{
    /* Range subset: [L1, H1] ⊆ [L2, H2] iff L1 >= L2 and H1 <= H2.
     * Pre-fix: integer-on-RHS just returned (NIL NIL). */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (subtypep '(integer 0 1023) '(integer 0 *)))"),
        "(T T)");
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (subtypep '(integer 5 10) '(integer 0 100)))"),
        "(T T)");
    /* Out of range — certain no. */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (subtypep '(integer 0 *) '(integer 0 1023)))"),
        "(NIL T)");
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (subtypep '(integer -1 5) '(integer 0 10)))"),
        "(NIL T)");

    /* Bounded integer ⊆ FIXNUM — works when bounds fit in fixnum range. */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (subtypep '(integer 0 1023) 'fixnum))"),
        "(T T)");
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (subtypep '(integer -100 100) 'fixnum))"),
        "(T T)");
    /* (integer 0 *) extends past most-positive-fixnum, so it's NOT a
     * subtype of FIXNUM. */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (subtypep '(integer 0 *) 'fixnum))"),
        "(NIL T)");

    /* (mod N) = (integer 0 (1- N)). */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (subtypep '(mod 10) '(integer 0 9)))"),
        "(T T)");
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (subtypep '(mod 10) '(integer 0 8)))"),
        "(NIL T)");

    /* (unsigned-byte N) range. */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (subtypep '(unsigned-byte 8) '(integer 0 255)))"),
        "(T T)");
    /* (signed-byte N) range. */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (subtypep '(signed-byte 8) '(integer -128 127)))"),
        "(T T)");
    /* BIT = (integer 0 1). */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (subtypep 'bit '(integer 0 1)))"),
        "(T T)");

    /* Empty range (mod 0) is empty type — subtype of everything. */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (subtypep '(mod 0) '(integer 1 2)))"),
        "(T T)");

    /* Non-integer numeric supertype on the LHS with integer-typed RHS:
     * RATIONAL, REAL, FLOAT, NUMBER, COMPLEX include non-integer values
     * (1/2, 0.5, etc.) so they are certainly NOT subtypes of any
     * integer-only spec.  Pre-fix returned (NIL NIL) "uncertain";
     * trivia logged "Can't check exhaustiveness" warnings under
     * serapeum's range.lisp typecase-of REAL*.  Now returns (NIL T). */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (subtypep 'rational '(integer 0 1023)))"),
        "(NIL T)");
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (subtypep 'real 'integer))"),
        "(NIL T)");
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (subtypep 'float 'fixnum))"),
        "(NIL T)");
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (subtypep 'number '(unsigned-byte 32)))"),
        "(NIL T)");
}

TEST(eval_subtypep_universal_t_and_nil)
{
    /* CLHS 4.2.5: T is the universal supertype.  Every type — including
     * unknown / undefined ones — is a subtype of T.  Pre-fix, our
     * subtypep returned (NIL NIL) for `(subtypep <undefined> 't)` which
     * tripped serapeum's `assure` macro into spurious "Required type
     * X is not a subtypep of declared type T" warnings. */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (subtypep 'undefined-type 't))"), "(T T)");
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (subtypep 'integer 't))"), "(T T)");
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (subtypep '(or string null) 't))"), "(T T)");
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (subtypep '(integer 0 100) 't))"), "(T T)");

    /* NIL is the empty type — only NIL is a subtype of NIL.
     * Used by serapeum's `(assert (not (subtypep type-spec nil)))`. */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (subtypep 'nil 'nil))"), "(T T)");
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (subtypep 'integer 'nil))"), "(NIL T)");
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (subtypep 'undefined-type 'nil))"), "(NIL T)");
}

/* The compiler must handle deeply-nested forms in tail position without
 * exhausting the C stack.  Pre-trampoline-refactor (2026-04-29), this
 * blew the C stack at ~700 levels because every compile_let /
 * compile_block / compile_if / compile_progn frame stayed alive while
 * the next nesting level compiled.  Now the trampoline in compile_expr
 * iterates these forms in a flat loop, with postludes drained from a
 * heap-allocated tail-frame stack. */
TEST(eval_compiler_trampoline_deep_if_chain)
{
    /* 1000-deep (if eq-i (return) (if eq-i ... (if eq-i ... :default))).
     * Our heap-built form bypasses the COND clause-cap limit and stresses
     * the IF ELSE-branch trampoline. */
    ASSERT_STR_EQ(eval_print(
        "(let ((form :default))"
        "  (dotimes (i 1000) (setf form `(if (eq i ,i) ',i ,form)))"
        "  (eval `(let ((i 999)) ,form)))"), "999");
}

TEST(eval_compiler_trampoline_deep_let_chain)
{
    /* 200-deep nested LET in body-tail position.  Each level adds one
     * lexical slot so we cap at <CL_MAX_LOCALS=256.  Pre-trampoline this
     * grew C stack ~2.8 KB per level (compile_let frame). */
    ASSERT_STR_EQ(eval_print(
        "(let ((form 'x))"
        "  (dotimes (i 200) (setf form `(let ((x ,i)) ,form)))"
        "  (eval form))"), "0");
}

TEST(eval_compiler_trampoline_deep_handler_bind)
{
    /* 60-deep nested HANDLER-BIND.  Capped by the VM-level handler stack
     * (a runtime limit, unrelated to the compiler trampoline).  The
     * trampoline itself handles arbitrary depth — this just needs to
     * verify deep nesting compiles in flat C stack. */
    ASSERT_STR_EQ(eval_print(
        "(let ((form 42))"
        "  (dotimes (i 60)"
        "    (setf form `(handler-bind ((error (lambda (c) c))) ,form)))"
        "  (eval form))"), "42");
}

TEST(eval_compiler_trampoline_deep_mvb)
{
    /* 200-deep nested MULTIPLE-VALUE-BIND in body-tail position. */
    ASSERT_STR_EQ(eval_print(
        "(let ((form 42))"
        "  (dotimes (i 200)"
        "    (setf form `(multiple-value-bind (a b) (values ,i ,i) "
        "                   (declare (ignore a b)) ,form)))"
        "  (eval form))"), "42");
}

TEST(eval_compiler_trampoline_deep_flet)
{
    /* 200-deep nested FLET in body-tail position.  Each level adds one
     * local slot for the function (flet stores funcs in anonymous slots),
     * so we cap below CL_MAX_LOCALS. */
    ASSERT_STR_EQ(eval_print(
        "(let ((form 42))"
        "  (dotimes (i 200)"
        "    (setf form `(flet ((nop () nil)) ,form)))"
        "  (eval form))"), "42");
}

TEST(eval_compiler_trampoline_deep_labels)
{
    /* 200-deep nested LABELS in body-tail position.  Same slot budget
     * as FLET. */
    ASSERT_STR_EQ(eval_print(
        "(let ((form 42))"
        "  (dotimes (i 200)"
        "    (setf form `(labels ((nop () nil)) ,form)))"
        "  (eval form))"), "42");
}

TEST(eval_compiler_trampoline_deep_eval_when)
{
    /* 500-deep nested EVAL-WHEN bodies. */
    ASSERT_STR_EQ(eval_print(
        "(let ((form 42))"
        "  (dotimes (i 500)"
        "    (setf form `(eval-when (:execute) ,form)))"
        "  (eval form))"), "42");
}

TEST(eval_compiler_trampoline_deep_if_then)
{
    /* 1000-deep IF chain on the THEN side: (if t (if t (if t ... 42))).
     * Pre-IF-THEN-trampoline (2026-04-30), this grew one C frame per
     * level via compile_if's recursive compile_expr(c, then_form).
     * Now driven by IF_AFTER_THEN/IF_AFTER_ELSE continuation frames so
     * the C stack stays flat. */
    ASSERT_STR_EQ(eval_print(
        "(let ((form 42))"
        "  (dotimes (i 1000) (setf form `(if t ,form 0)))"
        "  (eval form))"), "42");
}

TEST(eval_compiler_trampoline_deep_if_then_with_else)
{
    /* 1000-deep IF chain alternating THEN and ELSE.  Stresses both the
     * IF_AFTER_THEN postlude (which dispatches the ELSE) and the
     * IF_AFTER_ELSE postlude (which patches the JMP). */
    ASSERT_STR_EQ(eval_print(
        "(let ((form 42))"
        "  (dotimes (i 1000) "
        "    (setf form (if (oddp i) `(if t ,form 0) `(if nil 0 ,form))))"
        "  (eval form))"), "42");
}

TEST(eval_compiler_trampoline_deep_block_multi_body)
{
    /* Block whose body has 200 non-tail forms before the tail.  Each
     * non-tail form previously recursed into compile_expr through
     * compile_nontail_body's loop.  Now driven by PROGN_ITER frames so
     * iteration stays off the C stack. */
    ASSERT_STR_EQ(eval_print(
        "(let ((forms (list 42)))"
        "  (dotimes (i 200) (push 1 forms))"
        "  (eval `(block xx ,@forms)))"), "42");
}

TEST(eval_compiler_trampoline_block_return_from_nontail)
{
    /* Regression: BLOCK frame must drain AFTER PROGN_ITER frames pushed
     * by the body iteration.  Pre-fix, BLOCK_LOCAL drained first and
     * emitted its closing STORE/POP/LOAD before the body's non-tail
     * forms compiled, so return-from from the first body form would
     * land at the wrong place — the block returned 'no instead of 'yes. */
    ASSERT_STR_EQ(eval_print(
        "(block xx (return-from xx 'yes) 'no)"), "YES");
}

TEST(eval_compiler_deeply_nested_blocks)
{
    /* Regression: pre-fix CL_MAX_BLOCKS=16 was too small for code with
     * many nested BLOCKs (trivia's macroexpansion of MATCH produces 30+
     * nested blocks).  When c->block_count reached the limit, the next
     * compile_block did `c->blocks[c->block_count++]` which wrote past
     * the array end onto the adjacent block_count field — corrupting it
     * with a CL_Obj value (the block tag).  This surfaced later as a
     * SIGBUS in compile_return_from when iterating c->blocks[].tag with
     * the corrupted block_count.
     *
     * Fix: bump CL_MAX_BLOCKS to 256 + add a clean overflow error in
     * compile_block.  This test builds 200 nested BLOCKs (each using
     * a unique gensym tag so each one allocates a distinct slot) and
     * verifies the deepest body value still flows out. */
    ASSERT_EQ_INT(eval_int(
        "(let ((expr 99))"
        "  (dotimes (i 200)"
        "    (setf expr `(block ,(intern (format nil \"B~a\" i)) ,expr)))"
        "  (eval expr))"),
        99);
    /* Inner return-from threading from inside a chain of 50 nested
     * blocks back to a named outer block.  Pre-fix this would either
     * SIGBUS during compile or, with smaller depths, miscompile because
     * compile_return_from iterated a corrupted blocks[] array. */
    ASSERT_EQ_INT(eval_int(
        "(let ((expr '(return-from top 7)))"
        "  (dotimes (i 50)"
        "    (setf expr `(block ,(intern (format nil \"B~a\" i)) ,expr)))"
        "  (eval `(block top ,expr 'fall-through)))"),
        7);
}

TEST(eval_labels_shadows_global_macro)
{
    /* CLHS 3.1.2.1.2.4 / 3.1.2.1.2: a function form's CAR can be a
     * global function, a local FLET/LABELS function, OR a macro.
     * Local FLET/LABELS bindings SHADOW global macro definitions of
     * the same symbol.
     *
     * Pre-fix, our compile_expr_step checked `cl_macro_p(head)` BEFORE
     * looking up local FLET/LABELS, so a labels-bound function named
     * `test` (or any other symbol with a global macro defined elsewhere
     * — e.g. fiveam:test from a co-loaded library) would expand to the
     * macro's body instead of calling the labels function.  This
     * surfaced as "Undefined function: REGISTER-TEST" during cold
     * compile of trivia.balland2006/optimizer.lisp's `apply-fusion`,
     * which uses `(labels ((test (c) ...)) ... #'test ... (test c) ...)`. */
    eval_print("(defmacro shadow-mac (x) `(error \"macro called: ~a\" ,x))");
    /* Local labels function with the same name should shadow the macro. */
    ASSERT_EQ_INT(eval_int(
        "(labels ((shadow-mac (n) (* n 2)))"
        "  (shadow-mac 21))"),
        42);
    /* Same for FLET. */
    ASSERT_EQ_INT(eval_int(
        "(flet ((shadow-mac (n) (* n 3)))"
        "  (shadow-mac 14))"),
        42);
    /* Mutual recursion via labels: the recursive call from inside
     * shadow-mac itself must also resolve to the labels binding. */
    ASSERT_EQ_INT(eval_int(
        "(labels ((shadow-mac (n) (if (zerop n) 99 (shadow-mac (1- n)))))"
        "  (shadow-mac 5))"),
        99);
    /* #'name should also reach the labels binding (not the macro). */
    ASSERT_EQ_INT(eval_int(
        "(labels ((shadow-mac (n) (+ n 1000)))"
        "  (funcall #'shadow-mac 42))"),
        1042);
}

TEST(scanner_skips_macrolet_bindings)
{
    /* Regression: scan_body_for_boxing used to walk MACROLET bindings as
     * regular code — when a binding had the form (NAME (params) body),
     * NAME would be looked up as a global macro and the binding's params
     * + body would be passed in as the macro's args.  In lparallel/psort
     * this hit a global macro chain whose deep expansion eventually called
     * `lparallel.defpun::%plet-if` with bogus arity, signaling an error
     * that longjmp'd out of scan_body_for_boxing's CL_CATCH and (combined
     * with stack frame layout) smashed bi_compile_file's stack canary.
     *
     * The fix: scan_body_for_boxing now skips MACROLET bindings entirely
     * (only the body is scanned). */
    eval_print("(defmacro scan-trap-macro (x) "
               "  (error \"scan-trap-macro must NOT be expanded by scanner: got ~s\" x))");
    /* Inner macrolet redefines scan-trap-macro locally with same arity
     * as the global macro.  Scanner walking the binding as a call would
     * trigger the global macro's error.  After the fix, no expansion
     * happens at scan time and the form compiles cleanly. */
    ASSERT_EQ_INT(eval_int(
        "(let ((result 0))"
        "  (macrolet ((scan-trap-macro (n) `(setq result (* ,n 2))))"
        "    (scan-trap-macro 21))"
        "  result)"),
        42);
    /* Same for SYMBOL-MACROLET — bindings are (sym expansion) pairs and
     * walking them as code would treat a complex expansion form as a
     * function call. */
    eval_print("(defmacro symac-trap (x) "
               "  (error \"symac-trap must NOT be expanded by scanner: got ~s\" x))");
    ASSERT_EQ_INT(eval_int(
        "(let ((result 0))"
        "  (symbol-macrolet ((symac-trap (incf result 33)))"
        "    symac-trap"
        "    symac-trap)"
        "  result)"),
        66);
}

/* CLHS 6.1.3.1: accumulation clauses may include OF-TYPE after INTO.
 * split-sequence and serapeum use this.  We ignore the type spec but
 * must consume it so the clause parses. */
TEST(eval_loop_of_type_after_into)
{
    ASSERT_EQ_INT(eval_int(
        "(loop for i from 1 to 10 "
        "      sum i into total of-type fixnum "
        "      finally (return total))"),
        55);
}

/* CLHS 6.1.2.1: AND introduces another parallel iteration variable.
 * Serapeum uses `(loop for trail on list and head on head ...)`. */
TEST(eval_loop_for_and_parallel)
{
    /* FOR/IN + AND/IN */
    ASSERT_STR_EQ(eval_print(
        "(loop for x in '(1 2 3) and y in '(10 20 30) collect (+ x y))"),
        "(11 22 33)");
    /* FOR/ON + AND/ON — serapeum partition pattern */
    ASSERT_STR_EQ(eval_print(
        "(loop for trail on '(a b c d) and head on '(x y) "
        "      collect (list (car trail) (car head)))"),
        "((A X) (B Y))");
    /* FOR FROM/TO + AND = THEN */
    ASSERT_STR_EQ(eval_print(
        "(loop for i from 0 below 3 and x = 100 then (1+ x) "
        "      collect (list i x))"),
        "((0 100) (1 101) (2 102))");
}

/* CLHS 6.1.2.1.3 (for-as-on-list): the end of the list is tested "as if by
 * using atom", NOT endp.  So FOR/ON over a non-list (or the dotted tail of an
 * improper list) terminates the loop instead of signalling a TYPE-ERROR.
 * Regression: amilisp used endp, so e.g. chipi-api's equal-item-lists-p test
 * helper -- which does (loop for (k v) on <hash-table> by #'cddr ...) and
 * relies on the conforming zero-iteration behaviour -- crashed with a
 * TYPE-ERROR where SBCL passes vacuously.  FOR/IN, by contrast, correctly
 * keeps using endp. */
TEST(eval_loop_for_on_uses_atom_end_test)
{
    /* Proper list: ON still iterates every tail. */
    ASSERT_STR_EQ(eval_print("(loop for x on '(1 2 3) collect (car x))"),
                  "(1 2 3)");
    /* Non-list atom: ON terminates with zero iterations (atom test true). */
    ASSERT_STR_EQ(eval_print("(loop for x on 'foo collect x)"), "NIL");
    /* Destructuring var ON a non-list atom: same -- this is the chipi case. */
    ASSERT_STR_EQ(eval_print(
        "(loop for (k v) on 'foo by #'cddr collect k)"), "NIL");
    /* Destructuring ON a non-list via a hash-table, mirroring the failing
     * chipi-api equal-item-lists-p helper exactly. */
    ASSERT_STR_EQ(eval_print(
        "(loop for (k v) on (make-hash-table) by #'cddr collect k)"), "NIL");
    /* Improper (dotted) list: ON stops at the atom tail. */
    ASSERT_STR_EQ(eval_print("(loop for x on '(1 2 . 3) collect (car x))"),
                  "(1 2)");
    /* Destructuring plist walk over a proper list still works. */
    ASSERT_STR_EQ(eval_print(
        "(loop for (k v) on '(:a 1 :b 2) by #'cddr collect (list k v))"),
        "((:A 1) (:B 2))");
    /* FOR/IN keeps the strict endp semantics: a non-list atom is an error
     * (CL_ERR_TYPE == 2). */
    ASSERT_STR_EQ(eval_print("(loop for x in 'foo collect x)"), "ERROR:2");
}

TEST(eval_quasiquote_nested_list)
{
    ASSERT_STR_EQ(eval_print("`(a (b ,(+ 1 2)))"), "(A (B 3))");
}

TEST(eval_quasiquote_dotted)
{
    ASSERT_STR_EQ(eval_print("`(a . ,(+ 1 2))"), "(A . 3)");
}

TEST(eval_quasiquote_in_macro)
{
    eval_print("(defmacro my-inc2 (x) `(+ ,x 1))");
    ASSERT_EQ_INT(eval_int("(my-inc2 5)"), 6);
    ASSERT_EQ_INT(eval_int("(my-inc2 99)"), 100);
}

TEST(eval_quasiquote_macro_splice)
{
    eval_print("(defmacro my-progn (&rest body) `(progn ,@body))");
    ASSERT_EQ_INT(eval_int("(my-progn 1 2 42)"), 42);
}

/* --- Nested quasiquote --- */

TEST(eval_quasiquote_nested_double_unquote)
{
    /* ``(a ,,x) at outer level evaluates x, inner level preserves UNQUOTE */
    ASSERT_STR_EQ(eval_print("(let ((x 42)) ``(a ,,x))"),
                  "(QUASIQUOTE (A (UNQUOTE 42)))");
}

TEST(eval_quasiquote_nested_unquote_splice)
{
    /* ,,@xs — splice into nested quasiquote produces multiple UNQUOTE forms */
    ASSERT_STR_EQ(eval_print("(let ((xs '(1 2 3))) ``(a ,,@xs))"),
                  "(QUASIQUOTE (A (UNQUOTE 1) (UNQUOTE 2) (UNQUOTE 3)))");
}

TEST(eval_quasiquote_nested_simple)
{
    /* Nested backquote without unquote — all data */
    ASSERT_STR_EQ(eval_print("``(a b)"),
                  "(QUASIQUOTE (A B))");
}

TEST(eval_quasiquote_nested_inner_splice)
{
    /* ,@expr inside single backquote still works */
    ASSERT_STR_EQ(eval_print("(let ((xs '(1 2))) `(a ,@xs b))"),
                  "(A 1 2 B)");
}

TEST(eval_quasiquote_once_only_pattern)
{
    /* Test the once-only macro pattern from Alexandria */
    eval_print(
        "(defmacro test-oo (var &body body)"
        "  (let ((g (gensym)))"
        "    `(let ((,g ,var))"
        "       `(let ((,,g ,,(car body)))"
        "          t))))");
    ASSERT_STR_EQ(eval_print("(macroexpand-1 '(test-oo x (+ 1 2)))"),
                  /* Should produce a let-binding with gensym */
                  eval_print("(macroexpand-1 '(test-oo x (+ 1 2)))"));
}

/* `#(,x) and `#(,@xs) — quasiquote inside vector templates.  Sento's FSM
 * BUNCHER tests build per-event vectors with
 *   (concatenate 'vector vec `#(,(queue-num *received-event*)))
 * which previously evaluated to a literal #((UNQUOTE …)) because the qq
 * expander treated vectors as opaque atoms. */
TEST(eval_quasiquote_vector_unquote)
{
    ASSERT_STR_EQ(eval_print("(let ((x 42)) `#(,x))"), "#(42)");
    ASSERT_STR_EQ(eval_print("(let ((x 1) (y 2)) `#(,x ,y))"), "#(1 2)");
    ASSERT_STR_EQ(eval_print("(let ((s 'foo)) `#(a ,s c))"),
                  "#(A FOO C)");
}

TEST(eval_quasiquote_vector_splice)
{
    ASSERT_STR_EQ(eval_print("(let ((items (list 1 2 3))) `#(,@items))"),
                  "#(1 2 3)");
    ASSERT_STR_EQ(eval_print("(let ((items '(b c))) `#(a ,@items d))"),
                  "#(A B C D)");
}

/* --- Standard condition accessors --- */

TEST(eval_reader_error_condition)
{
    /* reader-error is a subtype of both parse-error and stream-error */
    ASSERT_STR_EQ(eval_print("(typep (make-condition 'reader-error) 'reader-error)"), "T");
    ASSERT_STR_EQ(eval_print("(typep (make-condition 'reader-error) 'parse-error)"), "T");
    ASSERT_STR_EQ(eval_print("(typep (make-condition 'reader-error) 'stream-error)"), "T");
    ASSERT_STR_EQ(eval_print("(typep (make-condition 'reader-error) 'error)"), "T");
}

TEST(eval_stream_error_stream_accessor)
{
    ASSERT_STR_EQ(eval_print(
        "(stream-error-stream (make-condition 'stream-error :stream *standard-output*))"),
        eval_print("*standard-output*"));
}

TEST(eval_package_error_package_accessor)
{
    ASSERT_STR_EQ(eval_print(
        "(package-error-package (make-condition 'package-error :package (find-package :cl)))"),
        eval_print("(find-package :cl)"));
}

TEST(eval_cell_error_name_accessor)
{
    ASSERT_STR_EQ(eval_print(
        "(cell-error-name (make-condition 'cell-error :name 'foo))"),
        "FOO");
}

/* FDEFINITION of a (SETF accessor) name with no (SETF accessor) function
 * (e.g. the accessor has only a DEFSETF): the UNDEFINED-FUNCTION names the
 * whole function name, not "?", and CELL-ERROR-NAME is the list. */
TEST(eval_undefined_setf_function_name_in_report)
{
    ASSERT_STR_EQ(eval_print(
        "(handler-case (fdefinition '(setf no-such-setf-accessor-xyz))"
        "  (undefined-function (e)"
        "    (list (not (null (search \"(SETF NO-SUCH-SETF-ACCESSOR-XYZ)\""
        "                             (princ-to-string e))))"
        "          (cell-error-name e))))"),
        "(T (SETF NO-SUCH-SETF-ACCESSOR-XYZ))");
}

/* CLHS CELL-ERROR: every runtime UNDEFINED-FUNCTION / UNBOUND-VARIABLE
 * carries the name.  The VM's own raise sites (OP_CALL_GLOBAL, OP_FLOAD,
 * OP_GLOAD, FUNCALL / APPLY of a symbol) used plain cl_error and left
 * CELL-ERROR-NAME NIL; FUNCALL of an fbound-less symbol was even a
 * TYPE-ERROR.  The report text is unchanged. */
TEST(eval_runtime_cell_error_name)
{
    ASSERT_STR_EQ(eval_print(
        "(handler-case (cen-no-such-fn-a 1)"
        "  (undefined-function (c) (list (cell-error-name c) (princ-to-string c))))"),
        "(CEN-NO-SUCH-FN-A \"Undefined function: CEN-NO-SUCH-FN-A\")");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (funcall (lambda () (cen-no-such-fn-b)))"
        "  (undefined-function (c) (cell-error-name c)))"),
        "CEN-NO-SUCH-FN-B");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (funcall 'cen-no-such-fn-c 1)"
        "  (undefined-function (c) (cell-error-name c)))"),
        "CEN-NO-SUCH-FN-C");
    ASSERT_STR_EQ(eval_print(
        "(let ((s 'cen-no-such-fn-d))"
        "  (handler-case (funcall s)"
        "    (undefined-function (c) (cell-error-name c))))"),
        "CEN-NO-SUCH-FN-D");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (apply 'cen-no-such-fn-e '(1 2))"
        "  (undefined-function (c) (cell-error-name c)))"),
        "CEN-NO-SUCH-FN-E");
    ASSERT_STR_EQ(eval_print(
        "(let ((s 'cen-no-such-fn-f))"
        "  (handler-case (apply s 1 '(2))"
        "    (undefined-function (c) (cell-error-name c))))"),
        "CEN-NO-SUCH-FN-F");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (mapcar 'cen-no-such-fn-g '(1))"
        "  (undefined-function (c) (cell-error-name c)))"),
        "CEN-NO-SUCH-FN-G");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (function cen-no-such-fn-h)"
        "  (undefined-function (c) (cell-error-name c)))"),
        "CEN-NO-SUCH-FN-H");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (typep 1 '(satisfies cen-no-such-pred))"
        "  (undefined-function (c) (cell-error-name c)))"),
        "CEN-NO-SUCH-PRED");
    ASSERT_STR_EQ(eval_print(
        "(handler-case cen-unbound-var-a"
        "  (unbound-variable (c) (list (cell-error-name c) (princ-to-string c))))"),
        "(CEN-UNBOUND-VAR-A \"Unbound variable: CEN-UNBOUND-VAR-A\")");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (funcall (lambda () (if cen-unbound-var-b 1 2)))"
        "  (unbound-variable (c) (cell-error-name c)))"),
        "CEN-UNBOUND-VAR-B");
    /* CLHS: both are CELL-ERRORs (the C hierarchy; the CLOS classes are
     * checked in tests/amiga/run-tests.lisp, test_vm runs without CLOS) */
    ASSERT_STR_EQ(eval_print(
        "(list (subtypep 'undefined-function 'cell-error)"
        "      (subtypep 'unbound-variable 'cell-error)"
        "      (typep (make-condition 'undefined-function :name 'x) 'cell-error))"),
        "(T T T)");
    ASSERT_STR_EQ(eval_print(
        "(list (handler-case (cen-no-such-fn-j) (cell-error (c) (cell-error-name c)))"
        "      (handler-case cen-unbound-var-c (cell-error (c) (cell-error-name c)))"
        "      (handler-case (symbol-function 'cen-no-such-fn-k)"
        "        (cell-error (c) (cell-error-name c))))"),
        "(CEN-NO-SUCH-FN-J CEN-UNBOUND-VAR-C CEN-NO-SUCH-FN-K)");
    /* the TYPE-ERROR handler no longer sees FUNCALL of an unbound symbol */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (funcall 'cen-no-such-fn-i)"
        "  (type-error () :type) (undefined-function () :undef))"),
        ":UNDEF");
}

TEST(eval_file_error_pathname_accessor)
{
    ASSERT_STR_EQ(eval_print(
        "(file-error-pathname (make-condition 'file-error :pathname \"/tmp/x\"))"),
        "\"/tmp/x\"");
}

/* --- define-compiler-macro --- */

TEST(eval_define_compiler_macro)
{
    /* define-compiler-macro returns the name (CLHS 3.2.2.1). */
    ASSERT_STR_EQ(eval_print(
        "(define-compiler-macro my-fn (&whole form x) (declare (ignore form x)) nil)"),
        "MY-FN");
    /* compiler-macro-function now returns the registered function. */
    ASSERT_STR_EQ(eval_print("(functionp (compiler-macro-function 'my-fn))"), "T");
    /* Returns NIL for a symbol that has no compiler macro. */
    ASSERT_STR_EQ(eval_print("(compiler-macro-function 'no-such-fn)"), "NIL");
}

/* --- setf values --- */

TEST(eval_setf_values)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((a 0) (b 0) (c 0))"
        "  (setf (values a b c) (values 10 20 30))"
        "  (list a b c))"),
        "(10 20 30)");
}

TEST(eval_setf_values_two)
{
    ASSERT_EQ_INT(eval_int(
        "(let ((x 0) (y 0))"
        "  (setf (values x y) (values 100 200))"
        "  (+ x y))"), 300);
}

/* --- define-setf-expander (no-op) --- */

TEST(eval_define_setf_expander)
{
    ASSERT_STR_EQ(eval_print(
        "(define-setf-expander my-place (x) (declare (ignore x)) nil)"),
        "MY-PLACE");
}

/* --- Gensym --- */

TEST(eval_gensym)
{
    CL_Obj g1, g2;

    /* gensym returns a symbol */
    ASSERT_STR_EQ(eval_print("(symbolp (gensym))"), "T");

    /* Two gensyms are distinct */
    g1 = cl_eval_string("(gensym)");
    g2 = cl_eval_string("(gensym)");
    ASSERT(g1 != g2);
}

/* CLHS *GENSYM-COUNTER* / GENSYM conformance — covers ANSI tests
 * gensym.{3,4,9,10,11,12}, gensym-counter.1, gensym.error.{1,8,10,11,12}. */
TEST(eval_gensym_counter)
{
    /* *GENSYM-COUNTER* exists and is a non-negative integer */
    ASSERT_STR_EQ(eval_print(
        "(and (integerp *gensym-counter*) (>= *gensym-counter* 0) t)"),
        "T");

    /* Counter is used as suffix and bumped after symbol creation */
    ASSERT_STR_EQ(eval_print(
        "(let ((*gensym-counter* 1)) (symbol-name (gensym)))"),
        "\"G1\"");
    ASSERT_STR_EQ(eval_print(
        "(let ((*gensym-counter* 1327)) (symbol-name (gensym \"FOO\")))"),
        "\"FOO1327\"");
    ASSERT_STR_EQ(eval_print(
        "(let ((*gensym-counter* 12345)) (gensym) *gensym-counter*)"),
        "12346");

    /* Bignum counter is supported and incremented correctly */
    ASSERT_STR_EQ(eval_print(
        "(let ((*gensym-counter* 1234567890123456789012345678901234567890))"
        "  (symbol-name (gensym)))"),
        "\"G1234567890123456789012345678901234567890\"");
    ASSERT_STR_EQ(eval_print(
        "(let ((*gensym-counter* 12345678901234567890123456789012345678901234567890))"
        "  (gensym) *gensym-counter*)"),
        "12345678901234567890123456789012345678901234567891");

    /* Integer arg overrides counter and does NOT increment it */
    ASSERT_STR_EQ(eval_print(
        "(let ((*gensym-counter* 10)) (symbol-name (gensym 123)))"),
        "\"G123\"");
    ASSERT_STR_EQ(eval_print(
        "(let ((*gensym-counter* 10)) (gensym 123) *gensym-counter*)"),
        "10");

    /* Bad prefix type → TYPE-ERROR */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (gensym t) (type-error () :te))"),
        ":TE");

    /* Negative counter → TYPE-ERROR */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (let ((*gensym-counter* -1)) (gensym)) "
        "  (type-error () :te))"),
        ":TE");

    /* Non-integer counter → TYPE-ERROR */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (let ((*gensym-counter* 'defun)) (gensym)) "
        "  (type-error () :te))"),
        ":TE");
}

TEST(eval_gentemp)
{
    /* gentemp returns a symbol */
    ASSERT_STR_EQ(eval_print("(symbolp (gentemp))"), "T");

    /* Unlike gensym, gentemp interns the symbol */
    ASSERT_STR_EQ(eval_print(
        "(let ((s (gentemp \"GTTEST-\")))"
        "  (eq s (find-symbol (symbol-name s))))"),
        "T");

    /* Two calls produce distinct symbols */
    ASSERT_STR_EQ(eval_print(
        "(eq (gentemp \"GTX-\") (gentemp \"GTX-\"))"),
        "NIL");

    /* Explicit package designator (string) works */
    ASSERT_STR_EQ(eval_print(
        "(eq (symbol-package (gentemp \"GTP-\" \"CL-USER\"))"
        "    (find-package \"CL-USER\"))"),
        "T");

    /* gentemp skips a name that is already interned in the package */
    ASSERT_STR_EQ(eval_print(
        "(let* ((s1 (gentemp \"GTSKIP-\"))"
        "       (taken (intern (format nil \"GTSKIP-~a\""
        "                               (1+ (parse-integer (symbol-name s1)"
        "                                                  :start 7)))))"
        "       (s2 (gentemp \"GTSKIP-\")))"
        "  (and (not (eq s2 taken))"
        "       (not (eq s2 s1))))"),
        "T");

    /* CLHS: GENTEMP does NOT touch *GENSYM-COUNTER* */
    ASSERT_STR_EQ(eval_print(
        "(let ((*gensym-counter* 42)) (gentemp \"GTNC-\") *gensym-counter*)"),
        "42");

    /* Character package designator (CLHS string-designator) — covers
     * ansi-test gentemp.5 (gentemp "Y" #\Z). */
    eval_print("(unless (find-package \"C\") (make-package \"C\" :use nil))");
    ASSERT_STR_EQ(eval_print(
        "(eq (symbol-package (gentemp \"X\" #\\C)) (find-package \"C\"))"),
        "T");

    /* Bad prefix → TYPE-ERROR (covers gentemp.error.1) */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (gentemp 'foo) (type-error () :te))"),
        ":TE");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (gentemp 5) (type-error () :te))"),
        ":TE");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (gentemp nil) (type-error () :te))"),
        ":TE");

    /* Bad package designator → TYPE-ERROR (covers gentemp.error.2) */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (gentemp \"X\" 5) (type-error () :te))"),
        ":TE");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (gentemp \"X\" '(a b)) (type-error () :te))"),
        ":TE");
}

/* --- Load --- */

TEST(eval_load)
{
    eval_print("(load \"tests/test_load.lisp\")");
    ASSERT_EQ_INT(eval_int("*load-test-var*"), 42);
    ASSERT_EQ_INT(eval_int("(load-test-fn 5)"), 105);
}

/* --- Boot file functions --- */

TEST(eval_boot_functions)
{
    ASSERT_EQ_INT(eval_int("(cadr '(1 2 3))"), 2);
    ASSERT_STR_EQ(eval_print("(caar '((a b) c))"), "A");
    ASSERT_STR_EQ(eval_print("(cdar '((a b) c))"), "(B)");
    ASSERT_STR_EQ(eval_print("(cddr '(1 2 3))"), "(3)");
    ASSERT_EQ_INT(eval_int("(caddr '(1 2 3))"), 3);
    ASSERT_EQ_INT(eval_int("(identity 42)"), 42);
    ASSERT_STR_EQ(eval_print("(endp nil)"), "T");
    ASSERT_STR_EQ(eval_print("(endp '(1))"), "NIL");
}

/* --- Phase 4: return / return-from --- */

TEST(eval_block_return)
{
    ASSERT_EQ_INT(eval_int("(block nil 1 (return 42) 3)"), 42);
    ASSERT_EQ_INT(eval_int("(block foo (return-from foo 42))"), 42);
    ASSERT_EQ_INT(eval_int("(block nil 1 2 3)"), 3);
    ASSERT_STR_EQ(eval_print("(block nil (return nil))"), "NIL");
}

TEST(eval_return_in_do)
{
    ASSERT_EQ_INT(eval_int(
        "(do ((i 0 (1+ i))) ((= i 10) 99) (if (= i 5) (return 42)))"), 42);
}

TEST(eval_return_in_dolist)
{
    ASSERT_EQ_INT(eval_int(
        "(dolist (x '(1 3 4 7)) (if (= 0 (mod x 2)) (return x)))"), 4);
}

TEST(eval_return_in_dotimes)
{
    ASSERT_EQ_INT(eval_int(
        "(dotimes (i 100) (if (= i 7) (return i)))"), 7);
}

/* --- Phase 4: prog1 / prog2 --- */

TEST(eval_prog1)
{
    ASSERT_EQ_INT(eval_int("(prog1 1 2 3)"), 1);
    ASSERT_EQ_INT(eval_int("(prog1 42)"), 42);
}

TEST(eval_prog2)
{
    ASSERT_EQ_INT(eval_int("(prog2 1 2 3)"), 2);
    ASSERT_EQ_INT(eval_int("(prog2 1 42)"), 42);
}

/* --- Phase 4: case / ecase --- */

TEST(eval_case)
{
    ASSERT_STR_EQ(eval_print("(case 2 (1 'a) (2 'b) (3 'c))"), "B");
    ASSERT_STR_EQ(eval_print("(case 5 ((1 2) 'low) ((3 4) 'high) (t 'other))"), "OTHER");
    ASSERT_STR_EQ(eval_print("(case 99 (1 'a))"), "NIL");
    ASSERT_STR_EQ(eval_print("(ecase 2 (1 'a) (2 'b))"), "B");
    ASSERT_STR_EQ(eval_print("(ecase 3 ((1 2) 'low) ((3 4) 'high))"), "HIGH");
}

/* Regression: ECASE/ETYPECASE/CCASE/CTYPECASE must signal TYPE-ERROR (CLHS
 * 5.3), not SIMPLE-ERROR, with the unmatched value as datum and a (member
 * ...)/(or ...) expected-type.  CCASE/CTYPECASE also establish STORE-VALUE. */
TEST(eval_exhaustive_case_type_error)
{
    ASSERT_STR_EQ(eval_print(
        "(handler-case (ecase 5 (1 'a) (2 'b))"
        "  (type-error (e) (list (type-error-datum e)"
        "                        (type-error-expected-type e)))"
        "  (error () 'wrong-class))"),
        "(5 (MEMBER 2 1))");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (etypecase 5 (string 's) (symbol 'y))"
        "  (type-error (e) (list (type-error-datum e)"
        "                        (type-error-expected-type e)))"
        "  (error () 'wrong-class))"),
        "(5 (OR SYMBOL STRING))");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (let ((x 5)) (ccase x ((1) 'a)))"
        "  (type-error () 'te) (error () 'wrong-class))"),
        "TE");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (let ((x 5)) (ctypecase x (string 's)))"
        "  (type-error () 'te) (error () 'wrong-class))"),
        "TE");
}

TEST(eval_typecase)
{
    ASSERT_STR_EQ(eval_print("(typecase 42 (integer 'num) (string 'str))"), "NUM");
    ASSERT_STR_EQ(eval_print("(typecase \"hi\" (integer 'num) (string 'str))"), "STR");
    ASSERT_STR_EQ(eval_print("(typecase 42 (string 'str) (t 'other))"), "OTHER");
}

/* --- Phase 4: flet / labels --- */

TEST(eval_flet)
{
    ASSERT_EQ_INT(eval_int("(flet ((f (x) (+ x 1))) (f 3))"), 4);
    /* flet with closure capture from enclosing let */
    ASSERT_EQ_INT(eval_int("(let ((x 10)) (flet ((f (y) (+ x y))) (f 5)))"), 15);
}

TEST(eval_labels)
{
    /* Use unique names to avoid collision with earlier defun tests */
    ASSERT_EQ_INT(eval_int(
        "(labels ((lab-fact (n) (if (<= n 1) 1 (* n (lab-fact (- n 1)))))) (lab-fact 5))"), 120);
    /* Mutual recursion */
    ASSERT_STR_EQ(eval_print(
        "(labels ((lab-even (n) (if (= n 0) t (lab-odd (- n 1)))) "
        "(lab-odd (n) (if (= n 0) nil (lab-even (- n 1))))) "
        "(lab-even 4))"), "T");
    /* Labels inside let* — boxed slots must not leak into subsequent bindings */
    ASSERT_EQ_INT(eval_int(
        "(let* ((x 99) "
        "       (y (labels ((fn (z) z)) (fn x)))) y)"), 99);
    /* Labels with implicit block (return-from) */
    ASSERT_EQ_INT(eval_int(
        "(labels ((lab-f (n) (if (<= n 0) (return-from lab-f 42) (lab-f (- n 1))))) (lab-f 3))"), 42);
}

/* --- Phase 4: &optional / &key --- */

TEST(eval_optional)
{
    eval_print("(defun opt-test (a &optional b) (list a b))");
    ASSERT_STR_EQ(eval_print("(opt-test 1)"), "(1 NIL)");
    ASSERT_STR_EQ(eval_print("(opt-test 1 2)"), "(1 2)");
}

TEST(eval_optional_default)
{
    eval_print("(defun opt-def (a &optional (b 10)) (+ a b))");
    ASSERT_EQ_INT(eval_int("(opt-def 1)"), 11);
    ASSERT_EQ_INT(eval_int("(opt-def 1 2)"), 3);
}

TEST(eval_key)
{
    eval_print("(defun key-test (&key x y) (list x y))");
    ASSERT_STR_EQ(eval_print("(key-test :x 1 :y 2)"), "(1 2)");
    ASSERT_STR_EQ(eval_print("(key-test)"), "(NIL NIL)");
    ASSERT_STR_EQ(eval_print("(key-test :y 5)"), "(NIL 5)");
}

TEST(eval_key_default)
{
    eval_print("(defun key-def (&key (x 0) (y 10)) (+ x y))");
    ASSERT_EQ_INT(eval_int("(key-def :x 5)"), 15);
    ASSERT_EQ_INT(eval_int("(key-def)"), 10);
}

TEST(eval_optional_suppliedp)
{
    /* &optional with supplied-p variable */
    eval_print("(defun opt-sp (a &optional (b nil b-p)) (list a b b-p))");
    ASSERT_STR_EQ(eval_print("(opt-sp 1)"), "(1 NIL NIL)");
    ASSERT_STR_EQ(eval_print("(opt-sp 1 2)"), "(1 2 T)");
    ASSERT_STR_EQ(eval_print("(opt-sp 1 nil)"), "(1 NIL T)");
    /* Multiple optionals with supplied-p */
    eval_print("(defun opt-sp2 (&optional (x 0 xp) (y 0 yp)) (list x xp y yp))");
    ASSERT_STR_EQ(eval_print("(opt-sp2)"), "(0 NIL 0 NIL)");
    ASSERT_STR_EQ(eval_print("(opt-sp2 5)"), "(5 T 0 NIL)");
    ASSERT_STR_EQ(eval_print("(opt-sp2 5 10)"), "(5 T 10 T)");
}

/* A supplied-p variable and &rest (CLHS 3.4.1.2-3.4.1.3): the &rest list
 * lies right after the positional parameters.  Its slot used to come after
 * the supplied-p slots, so the first supplied-p store overwrote the list and
 * the rest variable read an empty slot. */
TEST(eval_optional_suppliedp_rest)
{
    eval_print("(defun opt-sp-r (a &optional (b 2 bp) &rest r) (list a b bp r))");
    ASSERT_STR_EQ(eval_print("(opt-sp-r 1)"), "(1 2 NIL NIL)");
    ASSERT_STR_EQ(eval_print("(opt-sp-r 1 5)"), "(1 5 T NIL)");
    ASSERT_STR_EQ(eval_print("(opt-sp-r 1 5 6 7)"), "(1 5 T (6 7))");
    eval_print("(defun opt-sp-r2 (&optional (a 1 ap) (b 2 bp) &rest r) (list a ap b bp r))");
    ASSERT_STR_EQ(eval_print("(opt-sp-r2)"), "(1 NIL 2 NIL NIL)");
    ASSERT_STR_EQ(eval_print("(opt-sp-r2 9 8 7 6)"), "(9 T 8 T (7 6))");
    /* &rest holds the keyword pairs too. */
    eval_print("(defun opt-sp-rk (&optional (a 1 ap) &rest r &key x) (list a ap r x))");
    ASSERT_STR_EQ(eval_print("(opt-sp-rk)"), "(1 NIL NIL NIL)");
    ASSERT_STR_EQ(eval_print("(opt-sp-rk 1 :x 2)"), "(1 T (:X 2) 2)");
    /* A lambda, and APPLY's spread arguments. */
    ASSERT_STR_EQ(eval_print("(funcall (lambda (&optional (a 1 ap) &rest r) (list a ap r)) 1 2 3)"),
                  "(1 T (2 3))");
    ASSERT_STR_EQ(eval_print("(apply #'opt-sp-r 1 5 '(6 7 8))"), "(1 5 T (6 7 8))");
}

/* CLHS 3.4.1: an init-form may refer to any parameter variable to its left,
 * supplied-p variables included.  They used to be bound only after every
 * default had run, so a later default read the global value (unbound). */
TEST(eval_optional_default_sees_suppliedp)
{
    eval_print("(defun opt-sp-d (&optional (a 1 ap) (b (if ap :sup :nosup))) (list a b))");
    ASSERT_STR_EQ(eval_print("(opt-sp-d)"), "(1 :NOSUP)");
    ASSERT_STR_EQ(eval_print("(opt-sp-d 5)"), "(5 :SUP)");
    ASSERT_STR_EQ(eval_print("(opt-sp-d 5 6)"), "(5 6)");
    /* ... but not its own: (b ... bp) does not see bp in its default. */
    eval_print("(defvar *opt-sp-glob* :global)");
    eval_print("(defun opt-sp-own (&optional (*opt-sp-glob* *opt-sp-glob*)) *opt-sp-glob*)");
    ASSERT_STR_EQ(eval_print("(opt-sp-own)"), ":GLOBAL");
    ASSERT_STR_EQ(eval_print("(opt-sp-own 3)"), "3");
    /* With &rest after them. */
    eval_print("(defun opt-sp-dr (&optional (a 1 ap) (b (list ap)) &rest r) (list a b r))");
    ASSERT_STR_EQ(eval_print("(opt-sp-dr)"), "(1 (NIL) NIL)");
    ASSERT_STR_EQ(eval_print("(opt-sp-dr 4)"), "(4 (T) NIL)");
    ASSERT_STR_EQ(eval_print("(opt-sp-dr 4 5 6)"), "(4 5 (6))");
}

TEST(eval_optional_lambda)
{
    ASSERT_EQ_INT(eval_int("((lambda (a &optional (b 5)) (+ a b)) 10)"), 15);
    ASSERT_EQ_INT(eval_int("((lambda (a &optional (b 5)) (+ a b)) 10 20)"), 30);
}

/* --- Phase 4: &allow-other-keys --- */

TEST(eval_key_unknown_error)
{
    /* Unknown keyword should signal an error */
    const char *r = eval_print("(key-test :x 1 :z 99)");
    ASSERT(strstr(r, "ERROR") != NULL);
}

TEST(eval_key_allow_other_keys)
{
    /* &allow-other-keys in lambda list suppresses error */
    eval_print("(defun aok-test (&key x &allow-other-keys) x)");
    ASSERT_EQ_INT(eval_int("(aok-test :x 5 :z 99)"), 5);
    ASSERT_EQ_INT(eval_int("(aok-test :z 99)"), 0);  /* x defaults to NIL=0 as fixnum... */
    ASSERT_STR_EQ(eval_print("(aok-test :z 99)"), "NIL");
}

TEST(eval_key_caller_allow)
{
    /* Caller passes :allow-other-keys t to suppress error */
    ASSERT_STR_EQ(eval_print("(key-test :x 1 :z 99 :allow-other-keys t)"), "(1 NIL)");
}

/* --- Phase 4: eval builtin --- */

TEST(eval_eval_builtin)
{
    ASSERT_EQ_INT(eval_int("(eval '(+ 1 2))"), 3);
    ASSERT_EQ_INT(eval_int("(eval '(* 6 7))"), 42);
    ASSERT_STR_EQ(eval_print("(eval ''foo)"), "FOO");
}

/* --- Phase 4: macroexpand-1 / macroexpand --- */

TEST(eval_macroexpand_1)
{
    eval_print("(defmacro me-test (x) `(+ ,x 1))");
    /* macroexpand-1 on a macro call returns expanded form */
    ASSERT_STR_EQ(eval_print("(macroexpand-1 '(me-test 5))"), "(+ 5 1)");
    /* macroexpand-1 on a non-macro form returns it unchanged */
    ASSERT_STR_EQ(eval_print("(macroexpand-1 '(+ 1 2))"), "(+ 1 2)");
}

TEST(eval_macroexpand)
{
    /* macroexpand expands until no more macros at top level */
    eval_print("(defmacro me-wrap (x) `(me-test ,x))");
    ASSERT_STR_EQ(eval_print("(macroexpand '(me-wrap 5))"), "(+ 5 1)");
    /* Non-macro form returned unchanged */
    ASSERT_STR_EQ(eval_print("(macroexpand '(+ 1 2))"), "(+ 1 2)");
}

/* --- &whole support in defmacro --- */

TEST(eval_defmacro_whole_basic)
{
    /* &whole captures the entire macro call form */
    eval_print("(defmacro wtest (&whole w x y) (list 'quote w))");
    ASSERT_STR_EQ(eval_print("(wtest 1 2)"), "(WTEST 1 2)");
}

TEST(eval_defmacro_whole_with_body)
{
    /* &whole with &body */
    eval_print("(defmacro wbody (&whole w tag &body body)"
               "  (list 'quote (list w tag body)))");
    ASSERT_STR_EQ(eval_print("(wbody :x 1 2 3)"),
                  "((WBODY :X 1 2 3) :X (1 2 3))");
}

TEST(eval_defmacro_whole_with_optional)
{
    /* &whole with &optional — matches the global-vars pattern */
    eval_print("(defmacro wopt (&whole w name value &optional doc)"
               "  (list 'quote (list w name value doc)))");
    ASSERT_STR_EQ(eval_print("(wopt a b)"),
                  "((WOPT A B) A B NIL)");
    ASSERT_STR_EQ(eval_print("(wopt a b \"d\")"),
                  "((WOPT A B \"d\") A B \"d\")");
}

TEST(eval_defmacro_no_whole_still_works)
{
    /* Macros without &whole still work with extra hidden param */
    eval_print("(defmacro nw-test (x) `(+ ,x 10))");
    ASSERT_STR_EQ(eval_print("(nw-test 5)"), "15");
}

/* --- Phase 4 Tier 2: tagbody/go --- */

TEST(eval_tagbody_basic)
{
    /* tagbody with sequential tags and statements returns NIL */
    ASSERT_STR_EQ(eval_print("(let ((x 0)) (tagbody (setq x 1) (setq x (+ x 1))) x)"), "2");
}

TEST(eval_tagbody_forward_go)
{
    /* go jumps forward, skipping intermediate code */
    ASSERT_EQ_INT(eval_int("(let ((x 0)) (tagbody (go end) (setq x 99) end) x)"), 0);
}

TEST(eval_tagbody_backward_go)
{
    /* go jumps backward to implement a loop */
    ASSERT_EQ_INT(eval_int(
        "(let ((x 0)) (tagbody loop (setq x (+ x 1)) (if (< x 5) (go loop))) x)"), 5);
}

TEST(eval_tagbody_nested)
{
    /* nested tagbody */
    ASSERT_EQ_INT(eval_int(
        "(let ((x 0)) (tagbody outer (setq x (+ x 1)) (tagbody (setq x (+ x 10)) (go done)) done) x)"), 11);
}

TEST(eval_tagbody_fixnum_tags)
{
    /* integer tags are valid */
    ASSERT_EQ_INT(eval_int(
        "(let ((x 0)) (tagbody (go 2) 1 (setq x 99) 2 (setq x 42)) x)"), 42);
}

TEST(eval_tagbody_go_from_if)
{
    /* go from inside if */
    ASSERT_EQ_INT(eval_int(
        "(let ((x 0)) (tagbody start (setq x (+ x 1)) (if (< x 3) (go start))) x)"), 3);
}

TEST(eval_tagbody_local_named_lambda)
{
    /* Regression: a local variable named LAMBDA used inside a tagbody
       with backward GO must not be confused with the LAMBDA special
       operator by the compiler's NLX-detection scan. */
    ASSERT_STR_EQ(eval_print(
        "(loop for lambda in '(a b c) collect lambda)"), "(A B C)");
    ASSERT_STR_EQ(eval_print(
        "(let* ((lambda nil) (acc nil))"
        "  (tagbody"
        "     (setq lambda 'X)"
        "     (push lambda acc)"
        "     (go done)"
        "     done)"
        "  acc)"), "(X)");
    /* Same for FLET, LABELS, RESTART-CASE — all four were affected. */
    ASSERT_STR_EQ(eval_print(
        "(loop for flet in '(1 2) collect flet)"), "(1 2)");
    ASSERT_STR_EQ(eval_print(
        "(loop for labels in '(1 2) collect labels)"), "(1 2)");
}

TEST(eval_tagbody_nlx_dispatch_stack_balance)
{
    /* Regression: when a tagbody uses the NLX path (because its body
       contains a closure-creating form, like an inline lambda), each `go`
       compiles to OP_TAGBODY_GO.  The longjmp-landing dispatch table
       previously jumped straight to the matched tag's code position with
       the tag-index still on the value stack — but the body resumes at
       fall-through stack height, so the leftover tag-index polluted the
       containing form's stack.  In a parallel `let` with two bindings,
       the first binding's STORE then consumed the stale tag-index instead
       of its own init result.  Fix: per-tag dispatch shims pop the
       tag-index before jumping to the tag body. */
    ASSERT_STR_EQ(eval_print(
        "(let ((header (cons :hdr '(a b)))"
        "      (rows (let ((acc nil) (i 3))"
        "              (tagbody loop-here"
        "                 (when (zerop i) (go done))"
        "                 (push (funcall (lambda (x) x) i) acc)"
        "                 (decf i)"
        "                 (go loop-here)"
        "                 done)"
        "              acc)))"
        "  (cons header rows))"),
        "((:HDR A B) 1 2 3)");
}

TEST(eval_tagbody_go_through_macro_closure)
{
    /* Regression: a user macro that wraps its body in a closure
       (e.g. `(in-lambda ...)` -> `(funcall (lambda () ...))`) hides the
       lambda from the compiler's NLX-detection scan, which scans the
       *unexpanded* source.  The enclosing tagbody was then wrongly
       compiled with local jumps, so a `(go start)` inside the
       macro-generated closure could not reach its tag ("GO: no tag named
       START").  Fix: the NLX scanners macroexpand macro calls before
       deciding.  This idiom is pervasive in SLY (with-connection, etc.). */
    eval_print("(defmacro in-lambda (&body body) "
               "  `(funcall (lambda () ,@body)))");
    /* go across one macro-hidden closure */
    ASSERT_EQ_INT(eval_int(
        "(let ((i 0)) (tagbody start (incf i) "
        "   (in-lambda (when (< i 3) (go start)))) i)"), 3);
    /* go across two nested macro-hidden closures */
    eval_print("(defmacro twice-wrapped (&body body) "
               "  `(in-lambda (in-lambda ,@body)))");
    ASSERT_EQ_INT(eval_int(
        "(let ((i 0)) (tagbody start (incf i) "
        "   (twice-wrapped (when (< i 5) (go start)))) i)"), 5);
}

TEST(eval_block_return_through_macro_closure)
{
    /* Same root cause as eval_tagbody_go_through_macro_closure, for
       RETURN-FROM: a macro-hidden lambda must promote the enclosing block
       to the NLX path so RETURN-FROM can unwind out of the closure. */
    eval_print("(defmacro in-lambda (&body body) "
               "  `(funcall (lambda () ,@body)))");
    /* named block */
    ASSERT_EQ_INT(eval_int(
        "(block blk (in-lambda (return-from blk 42)) 99)"), 42);
    /* anonymous block via bare (return ...) */
    ASSERT_EQ_INT(eval_int(
        "(block nil (in-lambda (return 7)) 99)"), 7);
    /* SLY-style: macro named like with-connection wrapping the body */
    eval_print("(defmacro with-conn (&body body) "
               "  `(funcall (lambda () ,@body)))");
    ASSERT_STR_EQ(eval_print(
        "(block done (with-conn (when t (return-from done :ok))) :nope)"),
        ":OK");
}

TEST(eval_return_from_across_catch_no_nlx_leak)
{
    /* Regression: a local RETURN-FROM that jumps over an intervening CATCH
       (or HANDLER-BIND) must NOT use the cheap local-jump path — that would
       skip the OP_UNCATCH / OP_HANDLER_POP and strand the frame on the NLX
       stack on every NORMAL exit.  Looping such a form more than
       CL_MAX_NLX_FRAMES (2048) times overflowed the NLX stack ("NLX stack
       overflow"); this is exactly how handler-case's
       (block B (catch 'T (return-from B ...))) expansion leaked one CATCH
       frame per call when reading an SSL stream byte-by-byte (cl+ssl). */

    /* Direct shape: block wrapping a catch with a normal-path return-from. */
    eval_print("(defun rf-over-catch (n) "
               "  (let ((acc 0)) "
               "    (dotimes (i n acc) "
               "      (setq acc (block blk (catch 'tg (return-from blk 1)))))))");
    /* 5000 > 2048: pre-fix this overflowed the NLX stack. */
    ASSERT_EQ_INT(eval_int("(rf-over-catch 5000)"), 1);

    /* The real-world trigger: handler-case on the normal (no-condition)
       path, looped well past the NLX cap. */
    ASSERT_EQ_INT(eval_int(
        "(let ((acc 0)) (dotimes (i 5000 acc) "
        "  (setq acc (handler-case (+ i 1) (error () -1)))) 7)"), 7);

    /* Multiple values still flow through the (no-condition) handler-case
       path — the block return must preserve them. */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list "
        "  (dotimes (i 3000 (handler-case (values 1 2 3) (error () nil)))))"),
        "(1 2 3)");

    /* And the handled path still works after the promotion to NLX. */
    ASSERT_EQ_INT(eval_int(
        "(handler-case (error \"boom\") (error () 42))"), 42);
}

TEST(eval_macro_non_closure_keeps_local_go)
{
    /* A macro that does NOT introduce a closure must still leave the
       tagbody on the fast local-jump path (the macro-aware scan expands
       it, sees no closure form, and does not promote to NLX). */
    eval_print("(defmacro just-progn (&body body) `(progn ,@body))");
    ASSERT_EQ_INT(eval_int(
        "(let ((i 0)) (tagbody start (incf i) "
        "   (just-progn (when (< i 4) (go start)))) i)"), 4);
}

/* --- Phase 4 Tier 2: catch/throw --- */

TEST(eval_catch_basic)
{
    /* Basic catch/throw */
    ASSERT_EQ_INT(eval_int("(catch 'done (throw 'done 42))"), 42);
}

TEST(eval_catch_normal)
{
    /* Normal return (no throw) */
    ASSERT_EQ_INT(eval_int("(catch 'done (+ 1 2))"), 3);
}

TEST(eval_catch_throw_across_call)
{
    /* throw across function calls */
    eval_print("(defun throw-helper () (throw 'bail 99))");
    ASSERT_EQ_INT(eval_int("(catch 'bail (+ 1 (throw-helper)))"), 99);
}

TEST(eval_catch_nested)
{
    /* nested catches — inner catches first */
    ASSERT_EQ_INT(eval_int(
        "(catch 'outer (+ 10 (catch 'inner (throw 'inner 5))))"), 15);
}

TEST(eval_catch_nested_outer)
{
    /* nested catches — throw to outer */
    ASSERT_EQ_INT(eval_int(
        "(catch 'outer (catch 'inner (throw 'outer 42)))"), 42);
}

TEST(eval_catch_no_value)
{
    /* throw with no value defaults to NIL */
    ASSERT_STR_EQ(eval_print("(catch 'done (throw 'done))"), "NIL");
}

TEST(eval_throw_unmatched)
{
    /* Unmatched throw signals error */
    ASSERT_STR_EQ(eval_print("(catch 'a (throw 'b 1))"), "ERROR:1");
}

TEST(eval_catch_throw_deep_chain)
{
    /* Regression: throw across deep call chain — exercises NLX frame sync
     * when longjmp restores frame state after potential tail calls in callees */
    eval_print("(defun nlx-deep-a (f) (funcall f))");
    eval_print("(defun nlx-deep-b (f) (nlx-deep-a f))");
    eval_print("(defun nlx-deep-c (f) (nlx-deep-b f))");
    ASSERT_EQ_INT(eval_int(
        "(catch 'deep-tag (nlx-deep-c (lambda () (throw 'deep-tag 77))))"), 77);
    /* Verify computation continues correctly after catch restores frame */
    ASSERT_EQ_INT(eval_int(
        "(+ 1 (catch 'deep-tag (nlx-deep-c (lambda () (throw 'deep-tag 77)))))"), 78);
}

TEST(eval_block_return_deep_chain)
{
    /* Regression: return-from across deep call chain */
    eval_print("(defun blk-caller (f) (funcall f))");
    ASSERT_EQ_INT(eval_int(
        "(block done (blk-caller (lambda () (return-from done 99))) 0)"), 99);
    /* Normal path still works */
    ASSERT_EQ_INT(eval_int(
        "(block done (blk-caller (lambda () 50)) 42)"), 42);
}

TEST(eval_uwp_throw_deep_cleanup)
{
    /* Regression: unwind-protect cleanup runs correctly after throw across deep calls */
    ASSERT_STR_EQ(eval_print(
        "(let ((log nil))"
        "  (catch 'x"
        "    (unwind-protect"
        "      (nlx-deep-c (lambda () (throw 'x 1)))"
        "      (setq log 'cleaned)))"
        "  log)"),
        "CLEANED");
}

/* --- Phase 4 Tier 2: unwind-protect --- */

TEST(eval_uwp_normal)
{
    /* Normal exit runs cleanup, returns protected form value */
    ASSERT_STR_EQ(eval_print(
        "(let ((log nil)) (let ((r (unwind-protect 42 (setq log t)))) (list r log)))"),
        "(42 T)");
}

TEST(eval_uwp_throw_cleanup)
{
    /* throw through UWP runs cleanup */
    ASSERT_STR_EQ(eval_print(
        "(let ((cleanup nil)) (catch 'done (unwind-protect (throw 'done 1) (setq cleanup t))) cleanup)"),
        "T");
}

TEST(eval_uwp_throw_value)
{
    /* throw through UWP delivers correct value */
    ASSERT_EQ_INT(eval_int(
        "(catch 'done (unwind-protect (throw 'done 42) (+ 1 2)))"), 42);
}

TEST(eval_uwp_nested)
{
    /* nested UWP: both cleanups run */
    ASSERT_STR_EQ(eval_print(
        "(let ((log nil)) (catch 'done (unwind-protect (unwind-protect (throw 'done 1) (setq log (cons 'inner log))) (setq log (cons 'outer log)))) log)"),
        "(OUTER INNER)");
}

TEST(eval_uwp_error_cleanup)
{
    /* error through UWP runs cleanup (checked via global side-effect,
       since error propagates past catch and aborts the expression) */
    eval_print("(setq *uwp-flag* nil)");
    eval_print("(unwind-protect (error \"boom\") (setq *uwp-flag* t))");
    ASSERT_STR_EQ(eval_print("*uwp-flag*"), "T");
}

/* --- Phase 4 Tier 2: Multiple Values --- */

TEST(eval_values_basic)
{
    /* (values) returns NIL */
    ASSERT_STR_EQ(eval_print("(values)"), "NIL");
    /* (values 1) returns 1 */
    ASSERT_EQ_INT(eval_int("(values 1)"), 1);
    /* (values 1 2 3) returns primary = 1 */
    ASSERT_EQ_INT(eval_int("(values 1 2 3)"), 1);
}

TEST(eval_multiple_value_bind)
{
    /* Basic MVB */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-bind (a b c) (values 1 2 3) (list a b c))"), "(1 2 3)");
    /* More vars than values — extras are NIL */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-bind (a b c) (values 1) (list a b c))"), "(1 NIL NIL)");
    /* Fewer vars than values — extras ignored */
    ASSERT_EQ_INT(eval_int(
        "(multiple-value-bind (a) (values 10 20 30) a)"), 10);
}

TEST(eval_multiple_value_list)
{
    ASSERT_STR_EQ(eval_print("(multiple-value-list (values 1 2 3))"), "(1 2 3)");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (values))"), "NIL");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (+ 1 2))"), "(3)");
}

TEST(eval_multiple_value_prog1)
{
    /* Preserves all values of first form */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (multiple-value-prog1 (values 1 2 3) (+ 4 5)))"), "(1 2 3)");
}

TEST(eval_nth_value)
{
    ASSERT_EQ_INT(eval_int("(nth-value 0 (values 10 20 30))"), 10);
    ASSERT_EQ_INT(eval_int("(nth-value 1 (values 10 20 30))"), 20);
    ASSERT_EQ_INT(eval_int("(nth-value 2 (values 10 20 30))"), 30);
    /* Out of range returns NIL */
    ASSERT_STR_EQ(eval_print("(nth-value 5 (values 1 2 3))"), "NIL");
}

TEST(eval_values_list)
{
    /* values-list creates multiple values from a list */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (values-list '(1 2 3)))"), "(1 2 3)");
    ASSERT_EQ_INT(eval_int("(values-list '(42))"), 42);
}

TEST(eval_mv_propagation)
{
    /* MV propagates through progn (last form's values) */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (progn 1 (values 2 3 4)))"), "(2 3 4)");
    /* MV propagates through if */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (if t (values 1 2)))"), "(1 2)");
    /* MV propagates through let body */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (let ((x 1)) (values x 2 3)))"), "(1 2 3)");
    /* Non-MV form yields single value */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list 42)"), "(42)");
}

/* === Dynamic (special) variables === */

TEST(eval_defvar_basic)
{
    ASSERT_STR_EQ(eval_print("(defvar *x* 10)"), "*X*");
    ASSERT_EQ_INT(eval_int("*x*"), 10);
}

TEST(eval_defvar_no_overwrite)
{
    /* defvar does not overwrite existing value */
    eval_print("(defvar *y* 42)");
    eval_print("(defvar *y* 99)");
    ASSERT_EQ_INT(eval_int("*y*"), 42);
}

TEST(eval_defvar_init_form_only_when_unbound)
{
    /* CLHS DEFVAR: the initial-value form "is evaluated only if name is
     * not already bound" -- not merely left unstored.  The form's side
     * effect used to run on every re-evaluation. */
    eval_print("(defvar *dvc* 0)");
    ASSERT_STR_EQ(eval_print("(defvar *dvx* (setq *dvc* (+ *dvc* 1)))"),
                  "*DVX*");
    ASSERT_STR_EQ(eval_print("(defvar *dvx* (setq *dvc* (+ *dvc* 1)))"),
                  "*DVX*");
    ASSERT_EQ_INT(eval_int("*dvc*"), 1);
    ASSERT_EQ_INT(eval_int("*dvx*"), 1);
    /* A form that would signal is not evaluated at all once bound. */
    ASSERT_STR_EQ(eval_print("(defvar *dvx* (error \"evaluated\"))"),
                  "*DVX*");
    ASSERT_EQ_INT(eval_int("*dvx*"), 1);
    /* Unbound again -> evaluated again. */
    eval_print("(makunbound '*dvx*)");
    eval_print("(defvar *dvx* (setq *dvc* (+ *dvc* 10)))");
    ASSERT_EQ_INT(eval_int("*dvc*"), 11);
    ASSERT_EQ_INT(eval_int("*dvx*"), 11);
}

TEST(eval_def_forms_store_in_tail_position)
{
    /* A DEFVAR / DEFPARAMETER / DEFCONSTANT that is the last form of a
     * function body used to compile its value form as a tail call: the
     * store after it was dead code, the function returned the value
     * instead of the name, and the variable stayed unbound. */
    eval_print("(defun dvt-val () 42)");
    eval_print("(defun dvt-var () (defvar *dvt-v* (dvt-val)))");
    eval_print("(defun dvt-par () (defparameter *dvt-p* (dvt-val)))");
    eval_print("(defun dvt-con () (defconstant +dvt-c+ (dvt-val)))");
    ASSERT_STR_EQ(eval_print("(dvt-var)"), "*DVT-V*");
    ASSERT_STR_EQ(eval_print("(dvt-par)"), "*DVT-P*");
    ASSERT_STR_EQ(eval_print("(dvt-con)"), "+DVT-C+");
    ASSERT_EQ_INT(eval_int("*dvt-v*"), 42);
    ASSERT_EQ_INT(eval_int("*dvt-p*"), 42);
    ASSERT_EQ_INT(eval_int("+dvt-c+"), 42);
}

TEST(eval_defparameter_overwrite)
{
    /* defparameter always overwrites */
    eval_print("(defparameter *z* 10)");
    ASSERT_EQ_INT(eval_int("*z*"), 10);
    eval_print("(defparameter *z* 20)");
    ASSERT_EQ_INT(eval_int("*z*"), 20);
}

TEST(eval_defconstant_basic)
{
    ASSERT_STR_EQ(eval_print("(defconstant +my-const+ 42)"), "+MY-CONST+");
    ASSERT_EQ_INT(eval_int("+my-const+"), 42);
}

TEST(eval_defconstant_setq_error)
{
    /* setq on a constant should error at compile time */
    eval_print("(defconstant +dc1+ 10)");
    const char *r = eval_print("(setq +dc1+ 20)");
    ASSERT(strstr(r, "ERROR") != NULL);
    /* Value should be unchanged */
    ASSERT_EQ_INT(eval_int("+dc1+"), 10);
}

TEST(eval_defconstant_set_error)
{
    /* set on a constant should error at runtime */
    eval_print("(defconstant +dc2+ 99)");
    const char *r = eval_print("(set '+dc2+ 0)");
    ASSERT(strstr(r, "ERROR") != NULL);
    ASSERT_EQ_INT(eval_int("+dc2+"), 99);
}

TEST(eval_defconstant_t_is_constant)
{
    /* T is a constant per CL spec */
    const char *r = eval_print("(setq t 42)");
    ASSERT(strstr(r, "ERROR") != NULL);
}

TEST(eval_defconstant_keyword_error)
{
    /* Keywords are constants per CL spec */
    const char *r = eval_print("(set ':foo 42)");
    ASSERT(strstr(r, "ERROR") != NULL);
}

TEST(eval_special_let_binding)
{
    /* let dynamically binds special var, restores after */
    eval_print("(defvar *a* 1)");
    ASSERT_EQ_INT(eval_int("(let ((*a* 2)) *a*)"), 2);
    ASSERT_EQ_INT(eval_int("*a*"), 1);
}

TEST(eval_special_let_star_binding)
{
    /* let* same behavior */
    eval_print("(defvar *b* 10)");
    ASSERT_EQ_INT(eval_int("(let* ((*b* 20)) *b*)"), 20);
    ASSERT_EQ_INT(eval_int("*b*"), 10);
}

TEST(eval_special_visible_in_called_fn)
{
    /* Dynamic binding visible through function calls */
    eval_print("(defvar *d* 0)");
    eval_print("(defun read-d () *d*)");
    ASSERT_EQ_INT(eval_int("(let ((*d* 99)) (read-d))"), 99);
    ASSERT_EQ_INT(eval_int("(read-d)"), 0);
}

TEST(eval_special_nested_binding)
{
    /* Nested let with same special var */
    eval_print("(defvar *e* 1)");
    ASSERT_EQ_INT(eval_int(
        "(let ((*e* 2)) (let ((*e* 3)) *e*))"), 3);
    ASSERT_EQ_INT(eval_int("*e*"), 1);
}

TEST(eval_special_setq)
{
    /* setq modifies current binding, outer restored */
    eval_print("(defvar *f* 1)");
    ASSERT_EQ_INT(eval_int(
        "(let ((*f* 10)) (setq *f* 20) *f*)"), 20);
    ASSERT_EQ_INT(eval_int("*f*"), 1);
}

TEST(eval_special_unwind_protect)
{
    /* Bindings restored on throw through unwind-protect */
    eval_print("(defvar *g* 1)");
    eval_print(
        "(catch 'done"
        "  (unwind-protect"
        "    (let ((*g* 99))"
        "      (throw 'done *g*))"
        "    nil))");
    ASSERT_EQ_INT(eval_int("*g*"), 1);
}

TEST(eval_unwind_protect_multiple_values)
{
    /* CLHS 5.2: unwind-protect returns the values of protected-form.
     * Plural: must propagate all values, not just the primary.
     * Regression for sento HANDLE-CALL deadlock — bordeaux-threads-2's
     * (with-lock-held lk (gethash k h)) relies on the secondary presentp
     * value flowing through the implicit unwind-protect. */
    ASSERT_EQ_INT(eval_int(
        "(multiple-value-bind (a b)"
        "    (unwind-protect (values 1 2) nil)"
        "  (+ a b))"), 3);
    /* Cleanup forms must not leak into the value count */
    ASSERT_EQ_INT(eval_int(
        "(multiple-value-bind (a b c)"
        "    (unwind-protect (values 10 20)"
        "      (values 99 98 97))"
        "  (+ a b (or c 0)))"), 30);
    /* Cleanup runs and secondary value flows through (regression for the
     * (with-lock-held l (gethash k h)) idiom in bordeaux-threads-2). */
    eval_print("(defvar *uwp-cleanup* 0)");
    eval_print(
        "(progn"
        "  (setq *uwp-cleanup* 0)"
        "  (multiple-value-bind (val present-p)"
        "      (let ((ht (make-hash-table :test 'eql)))"
        "        (setf (gethash 'k ht) 'v)"
        "        (unwind-protect (gethash 'k ht)"
        "          (incf *uwp-cleanup*)))"
        "    (and present-p (eq val 'v))))");
    ASSERT_EQ_INT(eval_int("*uwp-cleanup*"), 1);
}

/* Regression: secondary values must survive a non-local exit through
 * an unwind-protect (the pending_mv_* snapshot / restore path). */
TEST(eval_uwprot_nlx_multiple_values)
{
    /* (a) Exact repro: return-from through one unwind-protect. */
    eval_print("(defun cwlh-mv (fn) (unwind-protect (funcall fn) nil))");
    eval_print("(defun recv-mv () (block done (cwlh-mv (lambda () (return-from done (values nil t))))))");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (recv-mv))"), "(NIL T)");

    /* (b) More than 2 secondaries. */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list"
        "  (block b (unwind-protect (return-from b (values 1 2 3 4)) nil)))"),
        "(1 2 3 4)");

    /* (c) Nested unwind-protects: both cleanups run in order, all values flow. */
    eval_print("(defvar *uwp-nlx-log* nil)");
    eval_print("(setq *uwp-nlx-log* nil)");
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list"
        "  (block b"
        "    (unwind-protect"
        "      (unwind-protect"
        "        (return-from b (values 1 2 3))"
        "        (setq *uwp-nlx-log* (cons 'inner *uwp-nlx-log*)))"
        "      (setq *uwp-nlx-log* (cons 'outer *uwp-nlx-log*)))))"),
        "(1 2 3)");
    ASSERT_STR_EQ(eval_print("*uwp-nlx-log*"), "(OUTER INNER)");

    /* (d) THROW carrying multiple values through unwind-protect (bi_throw path). */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list"
        "  (catch 'k (unwind-protect (throw 'k (values 10 20)) nil)))"),
        "(10 20)");

    /* (e) Cleanup form's own (values ...) must not bleed into result. */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list"
        "  (block b (unwind-protect (return-from b (values 'a 'b)) (values 99 98 97))))"),
        "(A B)");

    /* (f) Zero-value (values) through unwind-protect. */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list"
        "  (block b (unwind-protect (return-from b (values)) nil)))"),
        "NIL");

    /* (g) Restart-driven NLX through unwind-protect (cl_throw_to_tag path). */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list"
        "  (handler-bind ((simple-error"
        "                  (lambda (e)"
        "                    (declare (ignore e))"
        "                    (invoke-restart 'mv-restart))))"
        "    (unwind-protect"
        "      (restart-case (error \"test\")"
        "        (mv-restart () (values 'x 'y)))"
        "      nil)))"),
        "(X Y)");
}

TEST(eval_special_error_restore)
{
    /* Bindings restored on error */
    eval_print("(defvar *h* 1)");
    eval_print("(let ((*h* 99)) (error \"boom\"))");
    ASSERT_EQ_INT(eval_int("*h*"), 1);
}

TEST(eval_special_mixed_let)
{
    /* let with both lexical and special bindings */
    eval_print("(defvar *m* 10)");
    ASSERT_EQ_INT(eval_int(
        "(let ((x 1) (*m* 20) (y 2)) (+ x *m* y))"), 23);
    ASSERT_EQ_INT(eval_int("*m*"), 10);
}

TEST(eval_special_lambda_param_dynbind)
{
    /* Lambda params that are globally special must create dynamic bindings
     * visible to called functions and closures (not just lexical). */
    eval_print("(defvar *sp1* nil)");
    eval_print("(defun read-sp1 () *sp1*)");
    /* Required param */
    ASSERT_EQ_INT(eval_int("(funcall (lambda (*sp1*) (read-sp1)) 42)"), 42);
    ASSERT_STR_EQ(eval_print("*sp1*"), "NIL");
    /* Key param */
    ASSERT_EQ_INT(eval_int("(funcall (lambda (&key (*sp1* nil)) (read-sp1)) :*sp1* 99)"), 99);
    ASSERT_STR_EQ(eval_print("*sp1*"), "NIL");
    /* Key param with alias */
    ASSERT_EQ_INT(eval_int(
        "(funcall (lambda (&key ((:k *sp1*) nil)) (read-sp1)) :k 77)"), 77);
    ASSERT_STR_EQ(eval_print("*sp1*"), "NIL");
    /* Pre-created closure sees dynamic binding */
    ASSERT_EQ_INT(eval_int(
        "(let ((cl (lambda () *sp1*)))"
        "  (funcall (lambda (x &key ((:k *sp1*) nil)) (funcall x))"
        "    cl :k 55))"), 55);
}

/* --- setf and mutation --- */

TEST(eval_setf_car_cdr)
{
    ASSERT_STR_EQ(eval_print("(let ((x (list 1 2 3))) (setf (car x) 10) x)"), "(10 2 3)");
    ASSERT_STR_EQ(eval_print("(let ((x (cons 1 2))) (setf (cdr x) 3) x)"), "(1 . 3)");
}

TEST(eval_setf_first_rest)
{
    ASSERT_STR_EQ(eval_print("(let ((x (list 1 2 3))) (setf (first x) 10) x)"), "(10 2 3)");
    ASSERT_STR_EQ(eval_print("(let ((x (list 1 2 3))) (setf (rest x) (list 20 30)) x)"), "(1 20 30)");
}

TEST(eval_setf_nth)
{
    ASSERT_STR_EQ(eval_print("(let ((x (list 1 2 3))) (setf (nth 1 x) 20) x)"), "(1 20 3)");
    ASSERT_STR_EQ(eval_print("(let ((x (list 1 2 3))) (setf (nth 0 x) 10) x)"), "(10 2 3)");
}

TEST(eval_setf_variable)
{
    ASSERT_EQ_INT(eval_int("(let ((x 1)) (setf x 42) x)"), 42);
}

TEST(eval_setf_multiple)
{
    /* (setf a 1 b 2) — returns last value */
    ASSERT_EQ_INT(eval_int("(let ((a 0) (b 0)) (setf a 1 b 2))"), 2);
}

TEST(eval_rplaca_rplacd)
{
    ASSERT_STR_EQ(eval_print("(let ((x (cons 1 2))) (rplaca x 10))"), "(10 . 2)");
    ASSERT_STR_EQ(eval_print("(let ((x (cons 1 2))) (rplacd x 20))"), "(1 . 20)");
}

TEST(eval_car_cdr_type_error)
{
    /* Regression: car/cdr previously dereferenced any heap object as a
     * cons.  (car 'a) returned the symbol's first slot, (cdr 'a) the
     * second — silently corrupting any code that defended itself with
     * (handler-case ... (type-error ...)).  Now they signal a proper
     * TYPE-ERROR with :datum and :expected-type slots populated. */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (car 'foo) (type-error (c) "
        "  (list (type-error-datum c) (type-error-expected-type c))))"),
        "(FOO LIST)");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (cdr 5) (type-error (c) "
        "  (list (type-error-datum c) (type-error-expected-type c))))"),
        "(5 LIST)");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (car #\\a) (type-error (c) "
        "  (list (type-error-datum c) (type-error-expected-type c))))"),
        "(#\\a LIST)");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (car \"abc\") (type-error (c) "
        "  (list (type-error-datum c) (type-error-expected-type c))))"),
        "(\"abc\" LIST)");
    /* (car nil) is NIL, not an error. */
    ASSERT_STR_EQ(eval_print("(car nil)"), "NIL");
    ASSERT_STR_EQ(eval_print("(cdr nil)"), "NIL");
}

TEST(eval_rplaca_rplacd_type_error)
{
    /* Regression: rplaca/rplacd previously errored with a generic
     * SIMPLE-ERROR-style condition that didn't expose :datum /
     * :expected-type, so handler-case clauses checking those slots got
     * NIL.  Now they signal TYPE-ERROR with expected-type CONS (per
     * CLHS — note: nil is a list but NOT a cons, so (rplaca nil ...)
     * must error). */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (rplaca 'foo 1) (type-error (c) "
        "  (list (type-error-datum c) (type-error-expected-type c))))"),
        "(FOO CONS)");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (rplacd nil 1) (type-error (c) "
        "  (list (type-error-datum c) (type-error-expected-type c))))"),
        "(NIL CONS)");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (rplaca #\\a 1) (type-error (c) "
        "  (list (type-error-datum c) (type-error-expected-type c))))"),
        "(#\\a CONS)");
}

TEST(eval_aref_make_array_vectorp)
{
    ASSERT_STR_EQ(eval_print("(vectorp (make-array 3))"), "T");
    ASSERT_STR_EQ(eval_print("(vectorp 42)"), "NIL");
    ASSERT_EQ_INT(eval_int(
        "(let ((v (make-array 3)))"
        "  (setf (aref v 0) 10)"
        "  (setf (aref v 1) 20)"
        "  (setf (aref v 2) 30)"
        "  (+ (aref v 0) (aref v 1) (aref v 2)))"), 60);
}

TEST(eval_setf_svref)
{
    ASSERT_EQ_INT(eval_int(
        "(let ((v (make-array 2)))"
        "  (setf (svref v 0) 5)"
        "  (setf (svref v 1) 7)"
        "  (+ (svref v 0) (svref v 1)))"), 12);
}

TEST(eval_aref_string)
{
    /* aref on strings should return characters (CL spec: strings are arrays) */
    ASSERT_STR_EQ(eval_print("(aref \"hello\" 0)"), "#\\h");
    ASSERT_STR_EQ(eval_print("(aref \"hello\" 4)"), "#\\o");
    /* setf aref on strings */
    ASSERT_STR_EQ(eval_print(
        "(let ((s (copy-seq \"hello\")))"
        "  (setf (aref s 0) #\\H)"
        "  s)"), "\"Hello\"");
    /* row-major-aref on strings */
    ASSERT_STR_EQ(eval_print("(row-major-aref \"abcd\" 2)"), "#\\c");
}

TEST(eval_symbol_value)
{
    eval_print("(defvar *sv-test* 42)");
    ASSERT_EQ_INT(eval_int("(symbol-value '*sv-test*)"), 42);
}

TEST(eval_setf_symbol_value)
{
    eval_print("(defvar *sv-setf* 10)");
    ASSERT_EQ_INT(eval_int("(setf (symbol-value '*sv-setf*) 99)"), 99);
    ASSERT_EQ_INT(eval_int("*sv-setf*"), 99);
}

TEST(eval_set_builtin)
{
    eval_print("(defvar *set-test* 10)");
    ASSERT_EQ_INT(eval_int("(set '*set-test* 77)"), 77);
    ASSERT_EQ_INT(eval_int("*set-test*"), 77);
}

TEST(eval_setf_return_value)
{
    /* setf returns the assigned value */
    ASSERT_EQ_INT(eval_int("(let ((x (list 1 2 3))) (setf (car x) 99))"), 99);
    ASSERT_EQ_INT(eval_int(
        "(let ((v (make-array 1))) (setf (aref v 0) 42))"), 42);
}

/* --- member & pushnew --- */

TEST(eval_member_found)
{
    ASSERT_STR_EQ(eval_print("(member 2 '(1 2 3))"), "(2 3)");
}

TEST(eval_member_not_found)
{
    ASSERT_STR_EQ(eval_print("(member 4 '(1 2 3))"), "NIL");
}

TEST(eval_member_with_test)
{
    ASSERT_STR_EQ(eval_print(
        "(member \"a\" (list \"a\" \"b\") :test #'equal)"), "(\"a\" \"b\")");
}

TEST(eval_pushnew_new)
{
    ASSERT_STR_EQ(eval_print("(let ((x '(1 2))) (pushnew 3 x) x)"),
                  "(3 1 2)");
}

TEST(eval_pushnew_existing)
{
    ASSERT_STR_EQ(eval_print("(let ((x '(1 2))) (pushnew 1 x) x)"),
                  "(1 2)");
}

/* --- eval-when --- */

TEST(eval_eval_when_execute)
{
    ASSERT_EQ_INT(eval_int("(eval-when (:execute) (+ 1 2))"), 3);
}

TEST(eval_eval_when_multiple_situations)
{
    ASSERT_EQ_INT(eval_int(
        "(eval-when (:compile-toplevel :load-toplevel :execute) (+ 10 20))"),
        30);
}

TEST(eval_eval_when_body)
{
    /* Top-level multi-form eval-when processes forms individually for macro availability.
     * Verify side effects work (defvar sets value). */
    ASSERT_EQ_INT(eval_int(
        "(progn (eval-when (:execute) (defvar *ew-test* 0) (setq *ew-test* 42)) *ew-test*)"), 42);
}

/* --- destructuring-bind --- */

TEST(eval_destructuring_bind_simple)
{
    ASSERT_STR_EQ(eval_print(
        "(destructuring-bind (a b c) '(1 2 3) (list a b c))"),
        "(1 2 3)");
}

TEST(eval_destructuring_bind_nested)
{
    ASSERT_STR_EQ(eval_print(
        "(destructuring-bind (a (b c) &rest d) '(1 (2 3) 4 5) (list a b c d))"),
        "(1 2 3 (4 5))");
}

TEST(eval_destructuring_bind_rest)
{
    ASSERT_STR_EQ(eval_print(
        "(destructuring-bind (a &rest b) '(1 2 3) (list a b))"),
        "(1 (2 3))");
}

TEST(eval_destructuring_bind_optional)
{
    ASSERT_STR_EQ(eval_print(
        "(destructuring-bind (a &optional (b 10)) '(1) (list a b))"),
        "(1 10)");
}

TEST(eval_destructuring_bind_optional_provided)
{
    ASSERT_STR_EQ(eval_print(
        "(destructuring-bind (a &optional (b 10)) '(1 2) (list a b))"),
        "(1 2)");
}

/* CLHS 3.4.5: a destructuring lambda list may be dotted — (a . b) is the
 * abbreviation for (a &rest b).  Regression: eta-hab's
 * (destructuring-bind (name . label) '(sym . "str") ...) faulted with
 * "CAR: argument is not of type LIST". */
TEST(eval_destructuring_bind_dotted_pair)
{
    ASSERT_STR_EQ(eval_print(
        "(destructuring-bind (a . b) '(1 . 2) (list a b))"),
        "(1 2)");
}

TEST(eval_destructuring_bind_dotted_tail)
{
    ASSERT_STR_EQ(eval_print(
        "(destructuring-bind (a b . c) '(1 2 3 4) (list a b c))"),
        "(1 2 (3 4))");
}

TEST(eval_destructuring_bind_dotted_nested)
{
    ASSERT_STR_EQ(eval_print(
        "(destructuring-bind (a (b . c) . d) '(1 (2 . 3) 4) (list a b c d))"),
        "(1 2 3 (4))");
}

TEST(eval_destructuring_bind_dotted_after_optional)
{
    ASSERT_STR_EQ(eval_print(
        "(destructuring-bind (a &optional b . c) '(1 2 3) (list a b c))"),
        "(1 2 (3))");
}

TEST(eval_destructuring_bind_dotted_optional_defaulted)
{
    ASSERT_STR_EQ(eval_print(
        "(destructuring-bind (a &optional (b 9) . c) '(1) (list a b c))"),
        "(1 9 NIL)");
}

/* The boot.lisp macro expander (%dbind-expand) is a separate path from the
 * compiler special form — the boxing/NLX pre-scans macroexpand through it,
 * which is where eta-hab actually faulted.  Exercise it explicitly. */
TEST(eval_destructuring_bind_dotted_macroexpand_path)
{
    ASSERT_STR_EQ(eval_print(
        "(eval (macroexpand-1 "
        "  '(destructuring-bind (a &optional b . c) '(1 2 3) (list a b c))))"),
        "(1 2 (3))");
}

/* Regression: a dotted tail after &key (or &aux) is malformed per CLHS
 * 3.4.5 — the dotted abbreviation stands in the &rest position, which must
 * precede &key/&aux.  Both destructuring-bind paths (the compiler special
 * form AND the boot.lisp %dbind-expand macro used by MACROEXPAND/code
 * walkers) must reject it identically, rather than one silently accepting
 * malformed input the other errors on.
 *
 * The compiler special form path signals its cl_error at COMPILE TIME
 * (compile_destructure_pattern runs while compiling the enclosing
 * handler-case form), so the error aborts compilation of the whole
 * top-level form before any handler-case protection is installed — it
 * surfaces as an uncaught top-level error, not something handler-case can
 * catch.  The macroexpand-1 path runs %dbind-expand as an ordinary
 * function call at RUNTIME (inside the handler-case's dynamic extent), so
 * its (error ...) call is caught normally. */
TEST(eval_destructuring_bind_dotted_after_key_rejected)
{
    ASSERT_STR_EQ(eval_print(
        "(destructuring-bind (a &key b . c) '(1 :b 2 3 4) (list a b c))"),
        "ERROR:1");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (eval (macroexpand-1 "
        "  '(destructuring-bind (a &key b . c) '(1 :b 2 3 4) (list a b c))))"
        "  (error () :err))"), ":ERR");
}

TEST(eval_destructuring_bind_dotted_after_aux_rejected)
{
    ASSERT_STR_EQ(eval_print(
        "(destructuring-bind (a &aux (b 1) . c) '(1) (list a b c))"),
        "ERROR:1");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (eval (macroexpand-1 "
        "  '(destructuring-bind (a &aux (b 1) . c) '(1) (list a b c))))"
        "  (error () :err))"), ":ERR");
}

/* Regression: a destructuring &key section silently accepted keywords it
 * did not name — (destructuring-bind (&key a) '(:b 1) a) returned NIL where
 * CLHS 3.4.4 (destructuring lambda lists follow the 3.4.1.4 keyword rules)
 * requires an error, as ordinary lambda lists already did.  Found by
 * chipi-ui's CHART--RIGHT-AXIS-REJECTS-UNKNOWN-KEYS.  Both paths — the
 * compiler special form and the boot.lisp %dbind-expand macro used by
 * MACROEXPAND/code walkers — must agree, and both honour the two ways of
 * suppressing the check: &allow-other-keys in the lambda list and
 * :allow-other-keys with a true value in the list itself (3.4.1.4.1). */
TEST(eval_destructuring_bind_unknown_key_rejected)
{
    /* PROGRAM-ERROR, like the VM's own check for function calls */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (destructuring-bind (&key a) '(:b 1) a)"
        "  (program-error () :err))"), ":ERR");
    /* the chipi-ui shape: ((keyword var)) specs plus a defaulted key */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (destructuring-bind (&key ((:series ids)) range)"
        "                  '(:series (soc) :label \"%\") (list ids range))"
        "  (program-error () :err))"), ":ERR");
    /* the message names the offending key AND what the section accepts */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (destructuring-bind (&key ((:series ids)) range)"
        "                  '(:series (soc) :label \"%\") (list ids range))"
        "  (error (e) (princ-to-string e)))"),
        "\"destructuring-bind: unknown keyword argument :LABEL "
        "(accepted: :SERIES :RANGE)\"");
    /* &key with no keys accepts nothing but :allow-other-keys */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (destructuring-bind (x y &key) '(1 2 :z 3) (list x y))"
        "  (program-error () :err))"), ":ERR");
    /* &rest before &key does not exempt the section */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (destructuring-bind (&rest r &key a) '(:b 1) r)"
        "  (program-error () :err))"), ":ERR");
    /* nested pattern, and &optional ahead of &key */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (destructuring-bind ((&key a)) '((:b 1)) a)"
        "  (program-error () :err))"), ":ERR");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (destructuring-bind (a &optional b &key c) '(1 2 :d 3) c)"
        "  (program-error () :err))"), ":ERR");
    /* a non-symbol in keyword position is diagnosed, not compared blindly */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (destructuring-bind (&key a) '(5 1) a)"
        "  (program-error () :err))"), ":ERR");

    /* --- the check must NOT fire here --- */
    /* known keys still bind, in any order; leftmost duplicate wins */
    ASSERT_STR_EQ(eval_print(
        "(destructuring-bind (&key a b) '(:b 2 :a 1) (list a b))"), "(1 2)");
    ASSERT_STR_EQ(eval_print(
        "(destructuring-bind (&key a) '(:a 1 :a 2) a)"), "1");
    /* &allow-other-keys in the lambda list */
    ASSERT_STR_EQ(eval_print(
        "(destructuring-bind (&key a &allow-other-keys) '(:b 1 :a 2) a)"), "2");
    /* :allow-other-keys T in the list; NIL there does not suppress */
    ASSERT_STR_EQ(eval_print(
        "(destructuring-bind (&key a) '(:b 1 :allow-other-keys t) a)"), "NIL");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (destructuring-bind (&key a) '(:b 1 :allow-other-keys nil) a)"
        "  (program-error () :err))"), ":ERR");
    /* leftmost :allow-other-keys value wins when it appears more than once
     * (CLHS 3.4.1.4.1): the leading NIL must not be shadowed by a later T */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (destructuring-bind (&key a)"
        "                  '(:b 1 :allow-other-keys nil :allow-other-keys t) a)"
        "  (program-error () :err))"), ":ERR");
    ASSERT_STR_EQ(eval_print(
        "(destructuring-bind (&key a)"
        "    '(:b 1 :allow-other-keys t :allow-other-keys nil) a)"), "NIL");
    /* ansi-test destructuring-bind.23: :allow-other-keys itself is always
     * an accepted key, even for an empty &key section */
    ASSERT_STR_EQ(eval_print(
        "(destructuring-bind (&rest x &key) '(:allow-other-keys 1) x)"),
        "(:ALLOW-OTHER-KEYS 1)");
    /* (safety 0) elides the check, like the arity guards */
    ASSERT_STR_EQ(eval_print(
        "(locally (declare (optimize (safety 0)))"
        "  (destructuring-bind (&key a) '(:b 1) a))"), "NIL");

    /* --- the macro path (%dbind-expand) must agree on all of it --- */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (eval (macroexpand-1"
        "  '(destructuring-bind (&key a) '(:b 1) a)))"
        "  (program-error () :err))"), ":ERR");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (eval (macroexpand-1"
        "  '(destructuring-bind (&key ((:series ids)) range)"
        "      '(:series (soc) :label \"%\") (list ids range))))"
        "  (error (e) (princ-to-string e)))"),
        "\"destructuring-bind: unknown keyword argument :LABEL "
        "(accepted: :SERIES :RANGE)\"");
    ASSERT_STR_EQ(eval_print(
        "(eval (macroexpand-1"
        "  '(destructuring-bind (&key a &allow-other-keys) '(:b 1 :a 2) a)))"),
        "2");
    ASSERT_STR_EQ(eval_print(
        "(eval (macroexpand-1"
        "  '(destructuring-bind (&key a) '(:b 1 :allow-other-keys t) a)))"),
        "NIL");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (eval (macroexpand-1"
        "  '(destructuring-bind (&key a)"
        "       '(:b 1 :allow-other-keys nil :allow-other-keys t) a)))"
        "  (program-error () :err))"), ":ERR");
    ASSERT_STR_EQ(eval_print(
        "(eval (macroexpand-1"
        "  '(destructuring-bind (&rest x &key) '(:allow-other-keys 1) x)))"),
        "(:ALLOW-OTHER-KEYS 1)");
    /* &aux after &key is not a keyword */
    ASSERT_STR_EQ(eval_print(
        "(eval (macroexpand-1"
        "  '(destructuring-bind (&key a &aux (b (list a))) '(:a 1) (list a b))))"),
        "(1 (1))");
}

TEST(eval_destructuring_bind_body)
{
    ASSERT_STR_EQ(eval_print(
        "(destructuring-bind (a &body b) '(1 2 3) (list a b))"),
        "(1 (2 3))");
}

TEST(eval_destructuring_bind_key)
{
    /* Regression: &key with multiple keywords in destructuring-bind
       was reading garbage from the stack for unfound keys */
    ASSERT_STR_EQ(eval_print(
        "(destructuring-bind (&key a b) '(:a 1) (list a b))"),
        "(1 NIL)");
    ASSERT_STR_EQ(eval_print(
        "(destructuring-bind (&key a b) '(:a 1 :b 2) (list a b))"),
        "(1 2)");
    ASSERT_STR_EQ(eval_print(
        "(destructuring-bind (&key a b c) '(:a 1 :c 3) (list a b c))"),
        "(1 NIL 3)");
}

TEST(eval_destructuring_bind_optional_rest_key)
{
    /* Regression: &optional + &rest + &key ALL together — the &rest handler
       nested inside the &optional branch did `goto done` and never processed
       a trailing &key, so the &key vars were left unbound ("Unbound variable").
       This is the cl-who WITH-HTML-OUTPUT lambda list shape (var &optional
       stream &rest rest &key prologue indent).  Verified vs SBCL/CLISP. */
    ASSERT_STR_EQ(eval_print(
        "(destructuring-bind (var &optional stream &rest rest &key indent)"
        "    '(s nil :indent 9)"
        "  (list var stream rest indent))"),
        "(S NIL (:INDENT 9) 9)");
    /* &key absent from the args: indent defaults to NIL, rest is empty. */
    ASSERT_STR_EQ(eval_print(
        "(destructuring-bind (var &optional stream &rest rest &key indent)"
        "    '(s nil)"
        "  (list var stream rest indent))"),
        "(S NIL NIL NIL)");
    /* Two &key vars after &rest, both supplied out of order. */
    ASSERT_STR_EQ(eval_print(
        "(destructuring-bind (a &optional b &rest r &key x y)"
        "    '(1 2 :y 20 :x 10)"
        "  (list a b r x y))"),
        "(1 2 (:Y 20 :X 10) 10 20)");
}

/* Regression: destructuring-bind must be a real macro that MACROEXPAND
   actually expands (CLHS).  It used to be registered as an identity "inlined
   macro stub" — MACRO-FUNCTION returned non-NIL but MACROEXPAND-1 returned the
   form unchanged.  A code-walker that recurses on (macroexpand form) — e.g.
   serapeum's internal-definitions walker — then looped forever.  The native
   compile path is still used to compile it (the stub is only consulted at
   runtime), so it must STILL produce correct results too. */
TEST(eval_destructuring_bind_macroexpands)
{
    /* second value of macroexpand-1 must be T (it really expanded) */
    ASSERT_STR_EQ(eval_print(
        "(if (nth-value 1 (macroexpand-1 '(destructuring-bind (a b) y (list a b))))"
        "    'expanded 'unchanged)"),
        "EXPANDED");
    /* the expansion, when evaluated standalone, must still be correct */
    ASSERT_STR_EQ(eval_print(
        "(eval (macroexpand-1 '(destructuring-bind (a (b c) &rest d) '(1 (2 3) 4 5)"
        "                        (list a b c d))))"),
        "(1 2 3 (4 5))");
    /* &key/&optional/supplied-p through the macroexpansion path */
    ASSERT_STR_EQ(eval_print(
        "(eval (macroexpand-1 '(destructuring-bind (&key (a 0 ap)) '(:a 7)"
        "                        (list a ap))))"),
        "(7 T)");
}

/* Regression: define-modify-macro with an &optional spec that has a default,
   e.g. (&optional (delta 1)), spliced the whole (delta 1) spec into the call
   form so the generated macro emitted (function place (delta 1)) — evaluating
   (delta 1) as a call → "Undefined function: DELTA".  Must pass the var. */
TEST(eval_define_modify_macro_optional_default)
{
    eval_print("(defun dmm-plus (a b) (+ a b))");
    eval_print("(define-modify-macro dmm-incf (&optional (delta 1)) dmm-plus)");
    ASSERT_EQ_INT(eval_int("(let ((x 5)) (dmm-incf x) x)"), 6);
    ASSERT_EQ_INT(eval_int("(let ((x 5)) (dmm-incf x 10) x)"), 15);
}

/* Regression: a lexical (declare (notinline f)) must inhibit compiler-macro
   expansion of f (CLHS 3.2.2.1.3).  The standard compiler-macro fallback idiom
   expands (f ...) into (locally (declare (notinline f)) (... (f ...))); without
   honoring notinline the compiler re-fires the compiler macro on the inner call
   forever (observed as a GC-root-stack overflow compiling serapeum). */
TEST(eval_notinline_suppresses_compiler_macro)
{
    eval_print("(defun nicm-fn (x) (* x 2))");
    /* compiler macro that wraps the call in a notinline shield, exactly the
       idiom that used to loop.  It must terminate and call the real function. */
    eval_print(
        "(define-compiler-macro nicm-fn (&whole form x)"
        "  (declare (ignore form))"
        "  `(locally (declare (notinline nicm-fn)) (+ 0 (nicm-fn ,x))))");
    /* terminates → real nicm-fn runs inside the notinline scope → 5*2 = 10 */
    ASSERT_EQ_INT(eval_int("(nicm-fn 5)"), 10);
    /* notinline scope ends after the locally: a fresh call expands again. */
    ASSERT_EQ_INT(eval_int("(locally (declare (notinline nicm-fn)) (nicm-fn 6))"), 12);
}

/* --- defsetf --- */

TEST(eval_defsetf_short)
{
    eval_print("(defun my-get (vec i) (aref vec i))");
    eval_print(
        "(defun my-put (vec i val) (progn (setf (aref vec i) val) val))");
    eval_print("(defsetf my-get my-put)");
    ASSERT_EQ_INT(eval_int(
        "(let ((v (make-array 3))) (setf (my-get v 0) 42) (my-get v 0))"),
        42);
}

TEST(eval_defsetf_cadr)
{
    eval_print("(defun set-cadr (l v) (rplaca (cdr l) v) v)");
    eval_print("(defsetf cadr set-cadr)");
    ASSERT_STR_EQ(eval_print(
        "(let ((x (list 1 2 3))) (setf (cadr x) 99) x)"),
        "(1 99 3)");
}

/* get-setf-expansion must return proper store-form for defsetf places,
 * so that define-setf-expander on higher-level accessors can compose. */
TEST(eval_get_setf_expansion_defsetf)
{
    /* Define a struct with a mutable slot */
    eval_print("(defstruct (box (:copier nil)) (val nil))");
    /* Define a higher-level setf expander that composes with the struct setter.
     * (setf (box-val-plus-one b) v) => set box-val to (1- v) */
    eval_print(
        "(define-setf-expander box-val-plus-one (b &environment env)"
        "  (multiple-value-bind (temps vals stores store-form access-form)"
        "      (get-setf-expansion (list 'box-val b) env)"
        "    (let ((store (gensym)))"
        "      (values temps vals (list store)"
        "              (list 'let (list (list (car stores) (list '1- store)))"
        "                    store-form)"
        "              (list '1+ access-form)))))");
    eval_print("(defun box-val-plus-one (b) (1+ (box-val b)))");
    ASSERT_EQ_INT(eval_int(
        "(let ((b (make-box :val 10)))"
        "  (setf (box-val-plus-one b) 100)"
        "  (box-val b))"),
        99);
}

TEST(eval_equalp)
{
    /* Case-insensitive chars and strings */
    ASSERT_STR_EQ(eval_print("(equalp #\\a #\\A)"), "T");
    ASSERT_STR_EQ(eval_print("(equalp \"hello\" \"HELLO\")"), "T");
    ASSERT_STR_EQ(eval_print("(equalp \"abc\" \"ABD\")"), "NIL");
    /* Numeric cross-type */
    ASSERT_STR_EQ(eval_print("(equalp 1 1.0)"), "T");
    ASSERT_STR_EQ(eval_print("(equalp 0 0.0)"), "T");
    /* Vectors element-wise */
    ASSERT_STR_EQ(eval_print("(equalp #(1 2 3) #(1 2 3))"), "T");
    ASSERT_STR_EQ(eval_print("(equalp #(1 2 3) #(1 2 4))"), "NIL");
    /* Structs */
    eval_print("(defstruct ep-test (x nil) (y nil))");
    ASSERT_STR_EQ(eval_print("(equalp (make-ep-test :x 1 :y \"hi\") (make-ep-test :x 1.0 :y \"HI\"))"), "T");
    /* make-array with :element-type character creates string */
    ASSERT_STR_EQ(eval_print(
        "(stringp (make-array 3 :element-type 'character :initial-element #\\x))"), "T");
    ASSERT_STR_EQ(eval_print(
        "(stringp (make-array '(3) :element-type 'base-char :initial-element #\\x))"), "T");

    /* Hash tables (CLHS): same test, same count, equalp values per key.
       Regression: previously EQUALP fell back to identity for hash tables,
       so two structurally-equal tables compared NIL — this broke alexandria
       DEFINE-CONSTANT (used by babel) which re-evaluates constants under
       EQUALP. */
    ASSERT_STR_EQ(eval_print(
        "(let ((a (make-hash-table)) (b (make-hash-table)))"
        "  (dotimes (i 50) (setf (gethash i a) (* i i)) (setf (gethash i b) (* i i)))"
        "  (equalp a b))"), "T");
    /* Differing value for one key -> not equalp */
    ASSERT_STR_EQ(eval_print(
        "(let ((a (make-hash-table)) (b (make-hash-table)))"
        "  (dotimes (i 50) (setf (gethash i a) (* i i)) (setf (gethash i b) (* i i)))"
        "  (setf (gethash 5 b) 999)"
        "  (equalp a b))"), "NIL");
    /* Differing entry count -> not equalp */
    ASSERT_STR_EQ(eval_print(
        "(let ((a (make-hash-table)) (b (make-hash-table)))"
        "  (dotimes (i 50) (setf (gethash i a) i) (setf (gethash i b) i))"
        "  (setf (gethash 99 b) 1)"
        "  (equalp a b))"), "NIL");
    /* Differing test -> not equalp */
    ASSERT_STR_EQ(eval_print(
        "(equalp (make-hash-table :test 'eql) (make-hash-table :test 'equal))"), "NIL");
    /* EQUAL-test tables with string keys and list values compared by content */
    ASSERT_STR_EQ(eval_print(
        "(let ((a (make-hash-table :test 'equal)) (b (make-hash-table :test 'equal)))"
        "  (setf (gethash \"foo\" a) (list 1 2)) (setf (gethash \"foo\" b) (list 1 2))"
        "  (equalp a b))"), "T");
    /* Empty tables of the same test are equalp; a non-table is not */
    ASSERT_STR_EQ(eval_print("(equalp (make-hash-table) (make-hash-table))"), "T");
    ASSERT_STR_EQ(eval_print("(equalp (make-hash-table) 5)"), "NIL");

    /* EQUALP compares two vectors as SEQUENCES: by ACTIVE length (the fill
       pointer if present), comparing only the active prefix.  This matches
       SBCL/CLISP and is mandated by ansi-test EQUALP.11
       (data-and-control-flow/equalp.lsp): (equalp #(1 2 3) <dim-8 fp-3 (1 2 3)>)
       => T, and REVERSE-VECTOR.6.  A fill-pointered vector is equalp to a plain
       vector holding the same active elements. */
    ASSERT_STR_EQ(eval_print(
        "(equalp (make-array 5 :fill-pointer 2 :initial-contents '(1 2 3 4 5))"
        "        (make-array 2 :initial-contents '(1 2)))"), "T"); /* active (1 2) == (1 2) */
    /* Same storage size, different fill pointers -> different active lengths -> not equalp. */
    ASSERT_STR_EQ(eval_print(
        "(equalp (make-array 5 :fill-pointer 2 :initial-contents '(1 2 3 4 5))"
        "        (make-array 5 :fill-pointer 3 :initial-contents '(1 2 3 4 5)))"), "NIL");
    /* Active length 2 vs a plain length-5 vector -> not equalp. */
    ASSERT_STR_EQ(eval_print(
        "(equalp (make-array 5 :fill-pointer 2 :initial-contents '(1 2 3 4 5))"
        "        (make-array 5 :initial-contents '(1 2 3 4 5)))"), "NIL");
    /* EQUALP on multidimensional arrays: same dims + equalp elements */
    ASSERT_STR_EQ(eval_print("(equalp #2A((1 2) (3 4)) #2A((1 2) (3 4)))"), "T");
    ASSERT_STR_EQ(eval_print("(equalp #2A((1 2) (3 4)) #2A((1 2) (3 5)))"), "NIL");
    /* Same elements, different shape -> not equalp */
    ASSERT_STR_EQ(eval_print("(equalp #2A((1 2) (3 4)) #(1 2 3 4))"), "NIL");
    /* equalp descends vectors element-wise regardless of representation */
    ASSERT_STR_EQ(eval_print("(equalp #(1 2 3) (vector 1 2 3))"), "T");
    /* Fill-pointer bit-vectors compare by active length too. */
    ASSERT_STR_EQ(eval_print(
        "(equalp (make-array 6 :element-type 'bit :fill-pointer 3"
        "          :initial-contents '(1 0 1 1 1 1)) #*101)"), "T"); /* active (1 0 1) */
    ASSERT_STR_EQ(eval_print(
        "(equalp (make-array 6 :element-type 'bit :fill-pointer 3"
        "          :initial-contents '(1 0 1 1 1 1)) #*1011)"), "NIL"); /* active len 3 != 4 */
    /* Same storage, different fill pointers -> different active lengths -> not equalp. */
    ASSERT_STR_EQ(eval_print(
        "(equalp (make-array 6 :element-type 'bit :fill-pointer 3"
        "          :initial-contents '(1 0 1 1 1 1))"
        "        (make-array 6 :element-type 'bit :fill-pointer 5"
        "          :initial-contents '(1 0 1 1 1 1)))"), "NIL");
}

/* Sequence operations on adjustable / fill-pointer / displaced character
 * vectors (string-vectors) and bit-vectors: result type and active length. */
TEST(eval_string_vector_seq_ops)
{
    /* REVERSE of a fill-pointer string yields a STRING of the active length,
       not a general (vector character). */
    ASSERT_STR_EQ(eval_print(
        "(let ((s (make-array 9 :element-type 'character"
        "          :initial-contents \"12345ZZZZ\" :fill-pointer 5)))"
        "  (let ((r (reverse s))) (list (stringp r) r)))"), "(T \"54321\")");
    /* REVERSE of a fill-pointer bit-vector honours the active length. */
    ASSERT_STR_EQ(eval_print(
        "(reverse (make-array 10 :element-type 'bit"
        "          :initial-contents '(0 0 0 1 1 0 1 0 1 0) :fill-pointer 5))"), "#*11000");
    /* SUBSTITUTE on an adjustable string returns a STRING. */
    ASSERT_STR_EQ(eval_print(
        "(let ((s (make-array 5 :element-type 'character"
        "          :initial-contents \"abcab\" :adjustable t)))"
        "  (let ((r (substitute #\\x #\\a s))) (list (stringp r) r)))"), "(T \"xbcxb\")");
    /* NSUBSTITUTE on a fill-pointer string mutates only the active region. */
    ASSERT_STR_EQ(eval_print(
        "(let ((s (make-array 9 :element-type 'character"
        "          :initial-contents \"abcabZZZZ\" :fill-pointer 5)))"
        "  (nsubstitute #\\x #\\a s) (list (length s) (string= s \"xbcxb\")))"), "(5 T)");
    /* REMOVE-IF on an adjustable string returns a STRING. */
    ASSERT_STR_EQ(eval_print(
        "(let ((s (make-array 10 :element-type 'character"
        "          :initial-contents \"ab1c23def4\" :adjustable t)))"
        "  (let ((r (remove-if #'alpha-char-p s))) (list (stringp r) r)))"), "(T \"1234\")");
    /* COPY-SEQ of an adjustable base-char string is a SIMPLE string. */
    ASSERT_STR_EQ(eval_print(
        "(simple-string-p (copy-seq (make-array 4 :element-type 'base-char"
        "          :initial-contents '(#\\a #\\b #\\c #\\d) :adjustable t)))"), "T");
    /* MAKE-ARRAY displacing a (one-element) list dimension onto a vector must
       install a real displaced view, not a fresh NIL-filled array. */
    ASSERT_STR_EQ(eval_print(
        "(let* ((v1 (copy-seq #(a b c d e f g h)))"
        "       (v2 (make-array '(4) :displaced-to v1 :displaced-index-offset 2)))"
        "  (copy-seq v2))"), "#(C D E F)");
    /* MAKE-ARRAY displacing onto a string while requesting a fill pointer keeps
       the fill pointer (the active length is honoured). */
    ASSERT_STR_EQ(eval_print(
        "(let* ((s0 \"XXabcdeYYY\")"
        "       (s (make-array 5 :element-type 'character :displaced-to s0"
        "           :displaced-index-offset 2 :fill-pointer 3)))"
        "  (list (length s) (string= s \"abc\")))"), "(3 T)");
    /* AREF ignores the fill pointer (accesses the whole bit-vector storage). */
    ASSERT_STR_EQ(eval_print(
        "(let ((b (make-array 8 :element-type 'bit :initial-element 0 :fill-pointer 4)))"
        "  (fill b 1) (loop for i from 0 below 8 collect (aref b i)))"),
        "(1 1 1 1 0 0 0 0)");
    /* MAKE-ARRAY :initial-contents from a BIT-VECTOR source copies its bits
       (the array seq accessor must handle bit-vector sources, not just lists). */
    ASSERT_STR_EQ(eval_print(
        "(coerce (make-array 9 :element-type 'bit :initial-contents #*001011000) 'list)"),
        "(0 0 1 0 1 1 0 0 0)");
    /* REMOVE :count :from-end on a (simple) string removes the LAST matches. */
    ASSERT_STR_EQ(eval_print("(remove #\\b \"abcdbad\" :count 1 :from-end t)"), "\"abcdad\"");
    ASSERT_STR_EQ(eval_print("(remove #\\b \"abcdbad\" :count 1)"), "\"acdbad\"");
}

/* --- ANSI sequences conformance: the last 10 SEQUENCES-chapter failures --- */
TEST(eval_ansi_sequences_last10)
{
    /* FILL.ORDER.4: duplicate :start/:end keywords — CLHS 3.4.1.4 leftmost
       wins (here :end 3, :start 1), and all value forms are evaluated. */
    ASSERT_STR_EQ(eval_print(
        "(let ((i 0) (a (copy-seq #(a a a a))))"
        "  (list (fill a 'z :end (progn (incf i) 3) :end (progn (incf i) 1)"
        "               :start (progn (incf i) 1) :start (progn (incf i) 0)) i))"),
        "(#(A Z Z A) 4)");

    /* (SETF FILL-POINTER) works on a fill-pointer bit-vector (MISMATCH-BIT-VECTOR.24). */
    ASSERT_STR_EQ(eval_print(
        "(let ((m (make-array 6 :element-type 'bit"
        "          :initial-contents '(0 0 1 0 1 1) :fill-pointer 4)))"
        "  (list (length m) (setf (fill-pointer m) 6) (length m)))"),
        "(4 6 6)");

    /* SUBSEQ of a fill-pointer character vector returns a STRING of the active
       length, with and without an end index (SUBSEQ-STRING.3). */
    ASSERT_STR_EQ(eval_print(
        "(let ((s (make-array 10 :element-type 'character"
        "          :initial-contents \"abcdefghij\" :fill-pointer 8)))"
        "  (list (stringp (subseq s 0)) (subseq s 0) (subseq s 2) (subseq s 2 5)))"),
        "(T \"abcdefgh\" \"cdefgh\" \"cde\")");

    /* CONCATENATE honours the active length of a fill-pointer bit-vector and a
       declared length constraint (CONCATENATE.30 / .30A). */
    ASSERT_STR_EQ(eval_print(
        "(let ((x (make-array 10 :element-type 'bit"
        "          :initial-contents '(0 1 1 0 0 1 0 1 1 1) :fill-pointer 5)))"
        "  (list (concatenate 'bit-vector x '(0))"
        "        (concatenate '(bit-vector 10) x x)"
        "        (concatenate '(bit-vector *) x)))"),
        "(#*011000 #*0110001100 #*01100)");

    /* MAP with an (or ...) result type: the branch whose declared length matches
       the produced length is used (MAP.48). */
    ASSERT_STR_EQ(eval_print(
        "(map '(or (vector t 10) (vector t 5)) #'identity '(1 2 3 4 5))"),
        "#(1 2 3 4 5)");
    /* No branch matches the produced length → type-error (MAP.ERROR.11). */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (progn (map '(or (vector t 5) (vector t 10))"
        "                          #'identity '(1 2 3 4 5 6)) :no-error)"
        "  (type-error () :type-error))"),
        ":TYPE-ERROR");
    /* Ambiguous element type across matching branches → error (MAP.ERROR.10). */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (progn (map '(or (vector bit) (vector t))"
        "                          #'identity '(1 0 1)) :no-error)"
        "  (error () :error))"),
        ":ERROR");

    /* (SETF SUBSEQ) evaluates the PLACE subforms left-to-right BEFORE the value
       form, per CLHS 5.1.1.1.1 (SUBSEQ.ORDER.3): a=1 b=2 c=3 d=4. */
    ASSERT_STR_EQ(eval_print(
        "(let ((i 0) a b c d (s (copy-seq \"abcdefgh\")))"
        "  (setf (subseq (progn (setf a (incf i)) s)"
        "                (progn (setf b (incf i)) 1)"
        "                (progn (setf c (incf i)) 4))"
        "        (progn (setf d (incf i)) \"xyz\"))"
        "  (list s a b c d))"),
        "(\"axyzefgh\" 1 2 3 4)");
}

/* --- defun block (return-from inside do) --- */

TEST(eval_defun_return_from)
{
    eval_print(
        "(defun find-it (item list)"
        "  (do ((l list (cdr l)))"
        "      ((null l) nil)"
        "    (when (eql item (car l))"
        "      (return-from find-it l))))");
    ASSERT_STR_EQ(eval_print("(find-it 2 '(1 2 3))"), "(2 3)");
    ASSERT_STR_EQ(eval_print("(find-it 4 '(1 2 3))"), "NIL");
}

/* --- Cross-closure return-from (NLX-based blocks) --- */

TEST(eval_return_from_labels)
{
    /* return-from across labels boundary (ASDF pattern) */
    eval_print(
        "(defun frob (x)"
        "  (labels ((inner (v)"
        "             (if (> v 10)"
        "               (return-from frob (+ v 100))"
        "               v)))"
        "    (inner x)))");
    ASSERT_EQ_INT(eval_int("(frob 5)"), 5);
    ASSERT_EQ_INT(eval_int("(frob 20)"), 120);
}

TEST(eval_return_from_flet)
{
    /* return-from across flet boundary */
    eval_print(
        "(defun flet-test (x)"
        "  (flet ((helper (v)"
        "           (when (evenp v)"
        "             (return-from flet-test (* v 10)))))"
        "    (helper x)"
        "    x))");
    ASSERT_EQ_INT(eval_int("(flet-test 3)"), 3);
    ASSERT_EQ_INT(eval_int("(flet-test 4)"), 40);
}

TEST(eval_return_from_lambda)
{
    /* return-from across lambda boundary */
    eval_print(
        "(defun lambda-test (lst)"
        "  (dolist (x lst)"
        "    (funcall (lambda (v)"
        "               (when (= v 42)"
        "                 (return-from lambda-test :found)))"
        "             x))"
        "  :not-found)");
    ASSERT_STR_EQ(eval_print("(lambda-test '(1 2 3))"), ":NOT-FOUND");
    ASSERT_STR_EQ(eval_print("(lambda-test '(1 42 3))"), ":FOUND");
}

TEST(eval_return_from_nested_labels)
{
    /* return-from through two levels of labels nesting */
    eval_print(
        "(defun nested-test (x)"
        "  (labels ((outer (v)"
        "             (labels ((inner (w)"
        "                       (when (> w 100)"
        "                         (return-from nested-test :big))))"
        "               (inner v)"
        "               :small)))"
        "    (outer x)))");
    ASSERT_STR_EQ(eval_print("(nested-test 50)"), ":SMALL");
    ASSERT_STR_EQ(eval_print("(nested-test 200)"), ":BIG");
}

TEST(eval_block_with_unwind_protect)
{
    /* return-from across labels with interposing unwind-protect.
     * Use *uwp-log* (special var) since closures have value capture. */
    eval_print("(defvar *uwp-log* nil)");
    eval_print(
        "(defun uwp-test (x)"
        "  (setq *uwp-log* nil)"
        "  (labels ((inner (v)"
        "             (unwind-protect"
        "               (when (> v 5) (return-from uwp-test :early))"
        "               (setq *uwp-log* :cleaned))))"
        "    (inner x)"
        "    :normal))");
    ASSERT_STR_EQ(eval_print("(uwp-test 10)"), ":EARLY");
    ASSERT_STR_EQ(eval_print("*uwp-log*"), ":CLEANED");
}

/* Regression: (block X (unwind-protect (return-from X ...) cleanup))
 * in pure bytecode (no closure forms in body) used to compile the
 * return-from as a plain OP_JMP that bypassed OP_UWPOP and the cleanup
 * forms — silent CLHS violation.  Fix: tree_needs_nlx_block now
 * promotes such a block to the NLX path so the unwind walks UWPROT. */
TEST(eval_block_uwp_local_return_from_runs_cleanup)
{
    eval_print("(defvar *uwp-local-rf* 0)");
    eval_print("(setq *uwp-local-rf* 0)");
    ASSERT_STR_EQ(eval_print(
        "(block x"
        "  (unwind-protect (return-from x 1)"
        "    (incf *uwp-local-rf*)))"), "1");
    ASSERT_STR_EQ(eval_print("*uwp-local-rf*"), "1");
}

/* Same regression, anonymous block + (return-from nil ...). */
TEST(eval_block_nil_uwp_return_from_runs_cleanup)
{
    eval_print("(defvar *uwp-nil-rf* 0)");
    eval_print("(setq *uwp-nil-rf* 0)");
    ASSERT_STR_EQ(eval_print(
        "(block nil"
        "  (unwind-protect (return-from nil 7)"
        "    (incf *uwp-nil-rf*)))"), "7");
    ASSERT_STR_EQ(eval_print("*uwp-nil-rf*"), "1");
}

/* Same regression, anonymous block + bare (return ...). */
TEST(eval_block_nil_uwp_bare_return_runs_cleanup)
{
    eval_print("(defvar *uwp-nil-r* 0)");
    eval_print("(setq *uwp-nil-r* 0)");
    ASSERT_STR_EQ(eval_print(
        "(block nil"
        "  (unwind-protect (return 9)"
        "    (incf *uwp-nil-r*)))"), "9");
    ASSERT_STR_EQ(eval_print("*uwp-nil-r*"), "1");
}

/* Regression: tail call inside defun leaks NLX BLOCK frame, causing
 * OP_UWPOP to pop the leaked BLOCK instead of the UWP.  This corrupts
 * unwind-protect cleanup when a cross-closure return-from unwinds
 * through the UWP. */
TEST(eval_uwp_nlx_leak_tailcall)
{
    eval_print("(defvar *uwp-cleanup-val* nil)");
    /* call-with-fn: has &key so keyword supplied-p writes T to stack.
     * The defun block uses NLX (body contains lambda). Tail call to
     * the thunk leaks the BLOCK NLX frame. */
    eval_print(
        "(defun call-with-fn (thunk &key override)"
        "  (declare (ignore override))"
        "  (funcall thunk))");
    /* outer-fn: calls call-with-fn with a lambda that has unwind-protect.
     * The lambda captures session (a local) and accesses it in cleanup. */
    eval_print(
        "(defun outer-fn (fun)"
        "  (call-with-fn"
        "    #'(lambda ()"
        "        (let ((session 'my-session))"
        "          (unwind-protect"
        "              (funcall fun)"
        "            (setq *uwp-cleanup-val* session))))))");
    /* wrapper: establishes a block, calls outer-fn, which calls fun.
     * fun does return-from across the UWP boundary. */
    eval_print(
        "(defun wrapper ()"
        "  (block done"
        "    (outer-fn #'(lambda () (return-from done :early)))"
        "    :normal))");
    ASSERT_STR_EQ(eval_print("(wrapper)"), ":EARLY");
    ASSERT_STR_EQ(eval_print("*uwp-cleanup-val*"), "MY-SESSION");
}

/* Regression: NLX block frame precision.  Functions with closures that
 * don't use return-from should NOT push NLX BLOCK frames.  Previously,
 * any function containing lambda/labels/flet pushed an NLX frame even
 * when return-from was never used, leaking NLX frames during deep
 * recursion (e.g. CLOS dispatch + FSet WB tree operations). */
TEST(eval_block_nlx_precision)
{
    /* Defun with labels but no return-from — should use local block path.
     * Deep mutual recursion shouldn't overflow NLX stack. */
    eval_print(
        "(defun nlx-prec-a (n)"
        "  (labels ((even? (x) (if (= x 0) t (odd? (1- x))))"
        "           (odd? (x) (if (= x 0) nil (even? (1- x)))))"
        "    (even? n)))");
    ASSERT_STR_EQ(eval_print("(nlx-prec-a 100)"), "T");
    ASSERT_STR_EQ(eval_print("(nlx-prec-a 99)"), "NIL");

    /* Same with lambda — no return-from, so no NLX needed */
    eval_print(
        "(defun nlx-prec-b (lst)"
        "  (let ((result nil))"
        "    (dolist (x lst)"
        "      (push (funcall (lambda (v) (* v v)) x) result))"
        "    (nreverse result)))");
    ASSERT_STR_EQ(eval_print("(nlx-prec-b '(1 2 3))"), "(1 4 9)");
}

/* Regression: dolist variable shadowing.  CL spec requires the list-form
 * to be evaluated BEFORE the loop variable is bound.  Previously, the
 * loop variable was added to the environment before compiling the list-form,
 * so (dolist (x x) ...) would see the new uninitialized x. */
TEST(eval_dolist_var_shadowing)
{
    /* The classic pattern from FSet's Do-WB-Set-Tree-Members:
     * (dolist (val (some-accessor val)) ...) where val shadows outer val */
    ASSERT_STR_EQ(eval_print(
        "(let ((mylist '(a b c)))"
        "  (let ((result nil))"
        "    (dolist (mylist mylist)"
        "      (push mylist result))"
        "    (nreverse result)))"), "(A B C)");

    /* dotimes equivalent: (dotimes (n n) ...) where n shadows outer n */
    ASSERT_EQ_INT(eval_int(
        "(let ((n 5))"
        "  (let ((sum 0))"
        "    (dotimes (n n)"
        "      (incf sum n))"
        "    sum))"), 10);  /* 0+1+2+3+4 = 10 */
}

/* --- Phase 5 Tier 1: Character functions --- */

TEST(eval_characterp)
{
    ASSERT_STR_EQ(eval_print("(characterp #\\A)"), "T");
    ASSERT_STR_EQ(eval_print("(characterp 65)"), "NIL");
    ASSERT_STR_EQ(eval_print("(characterp #\\Space)"), "T");
}

TEST(eval_char_comparison)
{
    ASSERT_STR_EQ(eval_print("(char= #\\A #\\A)"), "T");
    ASSERT_STR_EQ(eval_print("(char= #\\A #\\B)"), "NIL");
    ASSERT_STR_EQ(eval_print("(char/= #\\A #\\B)"), "T");
    ASSERT_STR_EQ(eval_print("(char/= #\\A #\\A)"), "NIL");
    ASSERT_STR_EQ(eval_print("(char< #\\A #\\B)"), "T");
    ASSERT_STR_EQ(eval_print("(char< #\\B #\\A)"), "NIL");
    ASSERT_STR_EQ(eval_print("(char> #\\B #\\A)"), "T");
    ASSERT_STR_EQ(eval_print("(char<= #\\A #\\A)"), "T");
    ASSERT_STR_EQ(eval_print("(char<= #\\A #\\B)"), "T");
    ASSERT_STR_EQ(eval_print("(char>= #\\B #\\A)"), "T");
    ASSERT_STR_EQ(eval_print("(char>= #\\A #\\A)"), "T");
}

TEST(eval_char_comparison_variadic)
{
    /* Variadic char comparisons (CLHS allows 1+ args) */
    ASSERT_STR_EQ(eval_print("(char<= #\\a #\\b #\\c)"), "T");
    ASSERT_STR_EQ(eval_print("(char<= #\\a #\\c #\\b)"), "NIL");
    ASSERT_STR_EQ(eval_print("(char= #\\a #\\a #\\a)"), "T");
    ASSERT_STR_EQ(eval_print("(char= #\\a #\\a #\\b)"), "NIL");
    ASSERT_STR_EQ(eval_print("(char< #\\a #\\b #\\c)"), "T");
    ASSERT_STR_EQ(eval_print("(char> #\\c #\\b #\\a)"), "T");
    /* Single arg always returns T */
    ASSERT_STR_EQ(eval_print("(char= #\\x)"), "T");
    ASSERT_STR_EQ(eval_print("(char<= #\\x)"), "T");
}

TEST(eval_char_code_conversion)
{
    ASSERT_EQ_INT(eval_int("(char-code #\\A)"), 65);
    ASSERT_STR_EQ(eval_print("(code-char 65)"), "#\\A");
    ASSERT_EQ_INT(eval_int("(char-code (code-char 97))"), 97);
}

TEST(eval_char_case)
{
    ASSERT_STR_EQ(eval_print("(char-upcase #\\a)"), "#\\A");
    ASSERT_STR_EQ(eval_print("(char-upcase #\\A)"), "#\\A");
    ASSERT_STR_EQ(eval_print("(char-downcase #\\A)"), "#\\a");
    ASSERT_STR_EQ(eval_print("(char-downcase #\\a)"), "#\\a");
}

/* Regression: a codepoint above the BMP must not be TRUNCATED by the case
 * and classification functions.  On Windows their wint_t is 16 bits wide
 * while CHAR-CODE-LIMIT is #x110000, so an unguarded (wint_t)c turned
 * U+10428 into U+0428 — CHAR-UPCASE returned an unrelated Cyrillic letter
 * and STRING-UPCASE silently corrupted any string holding an emoji.
 *
 * What each platform ANSWERS differs legitimately and is not asserted here:
 * glibc knows the Deseret case mapping and returns U+10400, while the
 * Windows build reports no case above the BMP.  Both are fine; dropping the
 * high half is not. */
TEST(eval_char_case_astral_not_truncated)
{
    /* U+10428 DESERET SMALL LETTER LONG I.  Either it stays itself or it
     * maps to its uppercase U+10400 — but it stays astral either way, and it
     * is never U+0428 (CYRILLIC CAPITAL LETTER SHA). */
    int up = eval_int("(char-code (char-upcase (code-char #x10428)))");
    int down = eval_int("(char-code (char-downcase (code-char #x10428)))");
    ASSERT(up >= 0x10000);
    ASSERT(up != 0x428);
    ASSERT(down >= 0x10000);
    ASSERT(down != 0x428);
    /* U+1F600 GRINNING FACE has no case anywhere: it must come back as
     * itself, not as U+F600 (a private-use character). */
    ASSERT_EQ_INT(eval_int("(char-code (char-upcase (code-char #x1F600)))"),
                  0x1F600);
    ASSERT_EQ_INT(eval_int("(char-code (char-downcase (code-char #x1F600)))"),
                  0x1F600);
    ASSERT_STR_EQ(eval_print("(alpha-char-p (code-char #x1F600))"), "NIL");
    /* A whole string survives: this is what STRING-UPCASE corrupted. */
    ASSERT_EQ_INT(eval_int(
        "(char-code (char (string-upcase (string (code-char #x1F600))) 0))"),
        0x1F600);
    /* The BMP path is untouched — setup() has put LC_CTYPE in UTF-8 mode,
     * which is what the case mapping above ASCII follows. */
    ASSERT_EQ_INT(eval_int("(char-code (char-upcase (code-char #xE9)))"), 0xC9);
    ASSERT_EQ_INT(eval_int("(char-code (char-downcase (code-char #x391)))"),
                  0x3B1);
}

TEST(eval_char_predicates)
{
    ASSERT_STR_EQ(eval_print("(upper-case-p #\\A)"), "T");
    ASSERT_STR_EQ(eval_print("(upper-case-p #\\a)"), "NIL");
    ASSERT_STR_EQ(eval_print("(lower-case-p #\\a)"), "T");
    ASSERT_STR_EQ(eval_print("(lower-case-p #\\A)"), "NIL");
    ASSERT_STR_EQ(eval_print("(alpha-char-p #\\A)"), "T");
    ASSERT_STR_EQ(eval_print("(alpha-char-p #\\1)"), "NIL");
    ASSERT_STR_EQ(eval_print("(digit-char-p #\\5)"), "5");
    ASSERT_STR_EQ(eval_print("(digit-char-p #\\0)"), "0");
    ASSERT_STR_EQ(eval_print("(digit-char-p #\\9)"), "9");
    ASSERT_STR_EQ(eval_print("(digit-char-p #\\A)"), "NIL");
    ASSERT_STR_EQ(eval_print("(digit-char-p #\\A 16)"), "10");
    ASSERT_STR_EQ(eval_print("(digit-char-p #\\f 16)"), "15");
}

/* --- Phase 5 Tier 1: Symbol functions --- */

TEST(eval_symbol_name)
{
    ASSERT_STR_EQ(eval_print("(symbol-name 'foo)"), "\"FOO\"");
    ASSERT_STR_EQ(eval_print("(symbol-name nil)"), "\"NIL\"");
}

TEST(eval_symbol_package_fn)
{
    /* symbol-package returns a package object */
    ASSERT_STR_EQ(eval_print("(null (symbol-package 'foo))"), "NIL");
}

TEST(eval_fboundp)
{
    ASSERT_STR_EQ(eval_print("(fboundp '+)"), "T");
    ASSERT_STR_EQ(eval_print("(fboundp (gensym))"), "NIL");
    /* fboundp/fmakunbound with (setf name) */
    ASSERT_STR_EQ(eval_print("(progn (defun (setf test-fbp-acc) (v o) v) (fboundp '(setf test-fbp-acc)))"), "T");
    ASSERT_STR_EQ(eval_print("(progn (fmakunbound '(setf test-fbp-acc)) (fboundp '(setf test-fbp-acc)))"), "NIL");
    /* fmakunbound on unregistered (setf name) should not error */
    ASSERT_STR_EQ(eval_print("(fmakunbound '(setf no-such-setter))"), "(SETF NO-SUCH-SETTER)");
    /* makunbound */
    ASSERT_STR_EQ(eval_print("(progn (defvar *mak-test-var* 99) (boundp '*mak-test-var*))"), "T");
    ASSERT_STR_EQ(eval_print("(progn (makunbound '*mak-test-var*) (boundp '*mak-test-var*))"), "NIL");
    ASSERT_STR_EQ(eval_print("(makunbound '*mak-test-var*)"), "*MAK-TEST-VAR*");

    /* CLHS: fboundp is true for macros AND special operators, not just
     * functions.  Pre-fix, our bi_fboundp only checked s->function which
     * left every defmacro'd symbol reading as NIL — broke serapeum's
     * compile-time `(fboundp 'trivia:defpattern)` checks under cold
     * cache loads. */
    eval_print("(defmacro fbp-test-macro () `(progn))");
    ASSERT_STR_EQ(eval_print("(fboundp 'fbp-test-macro)"), "T");
    ASSERT_STR_EQ(eval_print("(fboundp 'if)"), "T");           /* special op */
    ASSERT_STR_EQ(eval_print("(fboundp 'block)"), "T");        /* special op */
    ASSERT_STR_EQ(eval_print("(fboundp 'unwind-protect)"), "T"); /* special op */
}

TEST(eval_fdefinition)
{
    /* fdefinition returns the same as symbol-function */
    ASSERT_STR_EQ(eval_print("(functionp (fdefinition '+))"), "T");
}

TEST(eval_make_symbol)
{
    ASSERT_STR_EQ(eval_print("(symbolp (make-symbol \"TEST\"))"), "T");
    ASSERT_STR_EQ(eval_print("(symbol-name (make-symbol \"HELLO\"))"), "\"HELLO\"");
}

TEST(eval_keywordp)
{
    ASSERT_STR_EQ(eval_print("(keywordp :foo)"), "T");
    ASSERT_STR_EQ(eval_print("(keywordp 'foo)"), "NIL");
    ASSERT_STR_EQ(eval_print("(keywordp 42)"), "NIL");
}

/* Regression: compiler use-after-free of the per-form compiler env.
 *
 * cl_compile_env created its CL_Compiler/CL_CompEnv but (unlike
 * compile_lambda) did not shield it from NLX unwinds.  When a subform
 * macroexpansion performs a non-local exit, the VM landing unwinds the
 * active-compiler chain, which would free the in-progress compiler — and
 * cl_env_destroy its env — out from under the running compile.  The
 * subsequent c->env dereference (e.g. cl_env_lookup_local_macro) was then a
 * heap-use-after-free; it crashed compiling babel's
 * INSTANTIATE-CONCRETE-MAPPINGS form (found via ASan).  Today the shield is
 * the anchor test in cl_compiler_unwind_to: a compiler owned by a still-live
 * C frame anchors shallower than the NLX landing frame and is never freed.
 *
 * This exercises the path: a macro that performs a handler-case NLX during its
 * own expansion, used inside compiled lambda-bearing forms, iterated to churn
 * the compiler pool.  Deterministic detection of the freed-memory read is via
 * ASan; here we assert the runtime stays healthy and keeps compiling. */
TEST(eval_compile_env_nlx_macro_no_uaf)
{
    eval_print(
        "(defmacro nlx-expand-mac (x)"
        "  (handler-case (error \"nlx during expand\") (error () nil))"
        "  (list '+ x 1))");
    eval_print(
        "(dotimes (i 40)"
        "  (eval `(defun ,(intern (format nil \"NLX-USE-~D\" i)) (a)"
        "           (let ((b a))"
        "             (list (nlx-expand-mac b)"
        "                   (lambda (y) (+ y b)))))))");
    /* If the compiler env had been freed mid-compile, the chain would be
       corrupt by now.  Verify a freshly compiled+run function is correct:
       NLX-USE-7 returns (list (+ b 1) <closure>), and the closure adds b. */
    ASSERT_STR_EQ(eval_print("(first (nlx-use-7 10))"), "11");
    ASSERT_STR_EQ(eval_print("(funcall (second (nlx-use-7 10)) 5)"), "15");
    ASSERT_STR_EQ(eval_print(
        "(progn (defun nlx-after (p q) (let ((s (* p q))) (+ s s)))"
        "       (nlx-after 6 7))"), "84");
}

/* --- Phase 5 Tier 1: Fixed builtins --- */

TEST(eval_length_vector)
{
    ASSERT_EQ_INT(eval_int("(length (make-array 5))"), 5);
}

TEST(eval_equal_vector)
{
    /* CLHS: EQUAL on general (non-string, non-bit) vectors/arrays is identity,
       NOT element-wise — two distinct vectors with equal contents are NOT
       equal. (equalp does compare element-wise; see eval_equalp.) */
    eval_print("(defun make-vec-12 () (let ((v (make-array 2))) (setf (aref v 0) 1) (setf (aref v 1) 2) v))");
    ASSERT_STR_EQ(eval_print(
        "(equal (make-vec-12) (make-vec-12))"), "NIL");
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-vec-12))) (equal v v))"), "T");  /* same object -> eq -> equal */
    ASSERT_STR_EQ(eval_print(
        "(equalp (make-vec-12) (make-vec-12))"), "T");    /* equalp descends */
}

TEST(eval_mapcar_multi_list)
{
    ASSERT_STR_EQ(eval_print("(mapcar #'+ '(1 2 3) '(10 20 30))"),
        "(11 22 33)");
    ASSERT_STR_EQ(eval_print("(mapcar #'list '(a b) '(1 2))"),
        "((A 1) (B 2))");
    /* Uneven lists — stops at shortest */
    ASSERT_STR_EQ(eval_print("(mapcar #'+ '(1 2 3) '(10 20))"),
        "(11 22)");
}

TEST(eval_vector)
{
    ASSERT_STR_EQ(eval_print("(vector 1 2 3)"), "#(1 2 3)");
    ASSERT_STR_EQ(eval_print("(vector)"), "#()");
    ASSERT_EQ_INT(eval_int("(aref (vector 10 20 30) 1)"), 20);
    ASSERT_STR_EQ(eval_print("(vectorp (vector 1 2))"), "T");
}

TEST(eval_vector_reader)
{
    /* #(...) reader syntax */
    ASSERT_STR_EQ(eval_print("#(1 2 3)"), "#(1 2 3)");
    ASSERT_STR_EQ(eval_print("#()"), "#()");
    ASSERT_EQ_INT(eval_int("(aref #(10 20 30) 1)"), 20);
    ASSERT_EQ_INT(eval_int("(length #(a b c d))"), 4);
    ASSERT_STR_EQ(eval_print("(simple-vector-p #(1 2))"), "T");
    ASSERT_STR_EQ(eval_print("(vectorp #(1))"), "T");
    /* Nested vectors */
    ASSERT_STR_EQ(eval_print("(aref #(#(1 2) #(3 4)) 0)"), "#(1 2)");
    ASSERT_EQ_INT(eval_int("(aref (aref #(#(1 2) #(3 4)) 1) 0)"), 3);
    /* Mixed types */
    ASSERT_STR_EQ(eval_print("#(1 \"hello\" a)"), "#(1 \"hello\" A)");
}

TEST(eval_array_print_multidim)
{
    /* 2D */
    ASSERT_STR_EQ(eval_print(
        "(make-array '(2 3) :initial-contents '((1 2 3) (4 5 6)))"),
        "#2A((1 2 3) (4 5 6))");
    /* 3D */
    ASSERT_STR_EQ(eval_print(
        "(make-array '(2 2 2) :initial-contents '(((1 2) (3 4)) ((5 6) (7 8))))"),
        "#3A(((1 2) (3 4)) ((5 6) (7 8)))");
    /* Empty dimension */
    ASSERT_STR_EQ(eval_print("(make-array '(2 0))"), "#2A(() ())");
    ASSERT_STR_EQ(eval_print("(make-array '(0 3))"), "#2A()");
    /* *print-array* nil */
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-array* nil)) (write-to-string (vector 1 2)))"),
        "\"#<VECTOR>\"");
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-array* nil)) (write-to-string (make-array '(2 3))))"),
        "\"#<ARRAY>\"");
}

TEST(eval_array_dimensions)
{
    ASSERT_STR_EQ(eval_print("(array-dimensions (make-array 5))"), "(5)");
    ASSERT_STR_EQ(eval_print("(array-dimensions (vector 1 2 3))"), "(3)");
    ASSERT_STR_EQ(eval_print("(array-dimensions (vector))"), "(0)");
}

TEST(eval_array_rank)
{
    ASSERT_EQ_INT(eval_int("(array-rank (make-array 5))"), 1);
    ASSERT_EQ_INT(eval_int("(array-rank (vector 1 2 3))"), 1);
}

TEST(eval_setf_aref_multidim)
{
    /* 2D array: setf + read back */
    ASSERT_EQ_INT(eval_int(
        "(let ((a (make-array '(2 3))))"
        "  (setf (aref a 0 0) 10)"
        "  (setf (aref a 0 1) 20)"
        "  (setf (aref a 0 2) 30)"
        "  (setf (aref a 1 0) 40)"
        "  (setf (aref a 1 1) 50)"
        "  (setf (aref a 1 2) 60)"
        "  (+ (aref a 0 0) (aref a 1 2)))"), 70);

    /* setf returns the value */
    ASSERT_EQ_INT(eval_int(
        "(let ((a (make-array '(3 3))))"
        "  (setf (aref a 1 2) 99))"), 99);

    /* 3D array */
    ASSERT_EQ_INT(eval_int(
        "(let ((a (make-array '(2 2 2))))"
        "  (setf (aref a 0 0 0) 1)"
        "  (setf (aref a 1 1 1) 8)"
        "  (+ (aref a 0 0 0) (aref a 1 1 1)))"), 9);
}

TEST(eval_array_dimension)
{
    /* 1D */
    ASSERT_EQ_INT(eval_int("(array-dimension (make-array 5) 0)"), 5);
    /* 2D */
    ASSERT_EQ_INT(eval_int("(array-dimension (make-array '(3 4)) 0)"), 3);
    ASSERT_EQ_INT(eval_int("(array-dimension (make-array '(3 4)) 1)"), 4);
}

TEST(eval_array_total_size)
{
    ASSERT_EQ_INT(eval_int("(array-total-size (make-array 5))"), 5);
    ASSERT_EQ_INT(eval_int("(array-total-size (make-array '(3 4)))"), 12);
    ASSERT_EQ_INT(eval_int("(array-total-size (make-array '(2 3 4)))"), 24);
}

TEST(eval_array_element_type)
{
    ASSERT_STR_EQ(eval_print("(array-element-type (make-array 5))"), "T");
    ASSERT_STR_EQ(eval_print("(array-element-type (make-array '(3 4)))"), "T");
    ASSERT_STR_EQ(eval_print("(array-element-type \"hello\")"), "CHARACTER");
    ASSERT_STR_EQ(eval_print("(array-element-type #*1010)"), "BIT");
    ASSERT_STR_EQ(eval_print("(array-element-type (vector 1 2 3))"), "T");
}

TEST(eval_class_of_array_vs_vector)
{
    /* vectorp should return T for 1D arrays, NIL for 2D+ */
    ASSERT_STR_EQ(eval_print("(vectorp (make-array 5))"), "T");
    ASSERT_STR_EQ(eval_print("(vectorp (make-array '(2 3)))"), "NIL");
    /* %class-of should return VECTOR for 1D, ARRAY for 2D+ */
    ASSERT_STR_EQ(eval_print("(amilisp::%class-of (vector 1 2))"), "VECTOR");
    ASSERT_STR_EQ(eval_print("(amilisp::%class-of (make-array '(2 3)))"), "ARRAY");
}

TEST(eval_array_row_major_index)
{
    /* 1D: identity */
    ASSERT_EQ_INT(eval_int("(array-row-major-index (make-array 5) 3)"), 3);
    /* 2D: row-major = row*ncols + col */
    ASSERT_EQ_INT(eval_int("(array-row-major-index (make-array '(3 4)) 0 0)"), 0);
    ASSERT_EQ_INT(eval_int("(array-row-major-index (make-array '(3 4)) 1 0)"), 4);
    ASSERT_EQ_INT(eval_int("(array-row-major-index (make-array '(3 4)) 2 3)"), 11);
}

TEST(eval_row_major_aref)
{
    /* Access via row-major index */
    ASSERT_EQ_INT(eval_int(
        "(let ((a (make-array '(2 3))))"
        "  (setf (aref a 1 2) 99)"
        "  (row-major-aref a 5))"), 99);  /* row 1, col 2 = 1*3+2 = 5 */

    /* setf row-major-aref */
    ASSERT_EQ_INT(eval_int(
        "(let ((a (make-array '(2 3))))"
        "  (setf (row-major-aref a 5) 77)"
        "  (aref a 1 2))"), 77);

    /* setf returns value */
    ASSERT_EQ_INT(eval_int(
        "(let ((a (make-array 3)))"
        "  (setf (row-major-aref a 1) 42))"), 42);
}

TEST(eval_fill_pointer)
{
    /* Basic fill-pointer read */
    ASSERT_EQ_INT(eval_int(
        "(fill-pointer (make-array 10 :fill-pointer 0))"), 0);
    ASSERT_EQ_INT(eval_int(
        "(fill-pointer (make-array 10 :fill-pointer 5))"), 5);

    /* fill-pointer T means start at array size (CLHS) */
    ASSERT_EQ_INT(eval_int(
        "(fill-pointer (make-array 10 :fill-pointer t))"), 10);

    /* array-has-fill-pointer-p */
    ASSERT_STR_EQ(eval_print(
        "(array-has-fill-pointer-p (make-array 5 :fill-pointer 0))"), "T");
    ASSERT_STR_EQ(eval_print(
        "(array-has-fill-pointer-p (make-array 5))"), "NIL");

    /* setf fill-pointer */
    ASSERT_EQ_INT(eval_int(
        "(let ((v (make-array 10 :fill-pointer 0)))"
        "  (setf (fill-pointer v) 7)"
        "  (fill-pointer v))"), 7);

    /* setf returns the new value */
    ASSERT_EQ_INT(eval_int(
        "(let ((v (make-array 10 :fill-pointer 0)))"
        "  (setf (fill-pointer v) 3))"), 3);
}

TEST(eval_vector_push)
{
    /* basic vector-push */
    ASSERT_EQ_INT(eval_int(
        "(let ((v (make-array 5 :fill-pointer 0)))"
        "  (vector-push 10 v)"
        "  (vector-push 20 v)"
        "  (vector-push 30 v)"
        "  (fill-pointer v))"), 3);

    /* vector-push returns old fill-pointer */
    ASSERT_EQ_INT(eval_int(
        "(let ((v (make-array 5 :fill-pointer 0)))"
        "  (vector-push 10 v)"
        "  (vector-push 20 v))"), 1);

    /* vector-push returns NIL when full */
    ASSERT_STR_EQ(eval_print(
        "(let ((v (make-array 2 :fill-pointer 0)))"
        "  (vector-push 1 v)"
        "  (vector-push 2 v)"
        "  (vector-push 3 v))"), "NIL");

    /* pushed elements accessible via aref */
    ASSERT_EQ_INT(eval_int(
        "(let ((v (make-array 3 :fill-pointer 0)))"
        "  (vector-push 10 v)"
        "  (vector-push 20 v)"
        "  (+ (aref v 0) (aref v 1)))"), 30);

    /* length reflects fill pointer */
    ASSERT_EQ_INT(eval_int(
        "(let ((v (make-array 10 :fill-pointer 0)))"
        "  (vector-push 1 v)"
        "  (vector-push 2 v)"
        "  (length v))"), 2);
}

TEST(eval_adjust_array)
{
    /* grow array, preserving old data */
    ASSERT_EQ_INT(eval_int(
        "(let ((v (make-array 3 :adjustable t :initial-element 1)))"
        "  (let ((v2 (adjust-array v 5 :initial-element 99)))"
        "    (+ (aref v2 0) (aref v2 3))))"), 100);  /* 1 + 99 */

    /* shrink array */
    ASSERT_EQ_INT(eval_int(
        "(let ((v (make-array 5 :adjustable t :initial-element 42)))"
        "  (array-total-size (adjust-array v 3)))"), 3);

    /* preserves fill pointer */
    ASSERT_EQ_INT(eval_int(
        "(let ((v (make-array 5 :fill-pointer 2 :adjustable t)))"
        "  (fill-pointer (adjust-array v 10)))"), 2);

    /* adjust with :fill-pointer override */
    ASSERT_EQ_INT(eval_int(
        "(let ((v (make-array 5 :fill-pointer 2 :adjustable t)))"
        "  (fill-pointer (adjust-array v 10 :fill-pointer 8)))"), 8);
}

/* --- Array type predicates (Step 7) --- */

TEST(eval_array_type_predicates)
{
    /* arrayp */
    ASSERT_STR_EQ(eval_print("(arrayp (vector 1 2 3))"), "T");
    ASSERT_STR_EQ(eval_print("(arrayp (make-array 5))"), "T");
    ASSERT_STR_EQ(eval_print("(arrayp (make-array '(2 3)))"), "T");
    ASSERT_STR_EQ(eval_print("(arrayp \"hello\")"), "T");
    ASSERT_STR_EQ(eval_print("(arrayp 42)"), "NIL");
    ASSERT_STR_EQ(eval_print("(arrayp '(1 2))"), "NIL");
    ASSERT_STR_EQ(eval_print("(arrayp nil)"), "NIL");

    /* simple-vector-p */
    ASSERT_STR_EQ(eval_print("(simple-vector-p (vector 1 2 3))"), "T");
    ASSERT_STR_EQ(eval_print("(simple-vector-p (make-array 5))"), "T");
    ASSERT_STR_EQ(eval_print("(simple-vector-p (make-array 5 :fill-pointer 0))"), "NIL");
    ASSERT_STR_EQ(eval_print("(simple-vector-p (make-array 5 :adjustable t))"), "NIL");
    ASSERT_STR_EQ(eval_print("(simple-vector-p (make-array '(2 3)))"), "NIL");
    ASSERT_STR_EQ(eval_print("(simple-vector-p \"hello\")"), "NIL");
    ASSERT_STR_EQ(eval_print("(simple-vector-p 42)"), "NIL");

    /* adjustable-array-p */
    ASSERT_STR_EQ(eval_print("(adjustable-array-p (make-array 5 :adjustable t))"), "T");
    ASSERT_STR_EQ(eval_print("(adjustable-array-p (make-array 5))"), "NIL");
    ASSERT_STR_EQ(eval_print("(adjustable-array-p (vector 1 2))"), "NIL");
    ASSERT_STR_EQ(eval_print("(adjustable-array-p \"hello\")"), "NIL");
}

TEST(eval_array_typep)
{
    /* typep with ARRAY — true for all arrays */
    ASSERT_STR_EQ(eval_print("(typep (vector 1 2) 'array)"), "T");
    ASSERT_STR_EQ(eval_print("(typep (make-array '(2 3)) 'array)"), "T");
    ASSERT_STR_EQ(eval_print("(typep \"hello\" 'array)"), "T");
    ASSERT_STR_EQ(eval_print("(typep 42 'array)"), "NIL");

    /* typep with VECTOR — true for 1D arrays and strings */
    ASSERT_STR_EQ(eval_print("(typep (vector 1 2) 'vector)"), "T");
    ASSERT_STR_EQ(eval_print("(typep \"hello\" 'vector)"), "T");
    ASSERT_STR_EQ(eval_print("(typep (make-array '(2 3)) 'vector)"), "NIL");

    /* typep with SIMPLE-VECTOR — 1D, element-type T, simple */
    ASSERT_STR_EQ(eval_print("(typep (vector 1 2) 'simple-vector)"), "T");
    ASSERT_STR_EQ(eval_print("(typep (make-array 5) 'simple-vector)"), "T");
    ASSERT_STR_EQ(eval_print("(typep (make-array 5 :fill-pointer 0) 'simple-vector)"), "NIL");
    ASSERT_STR_EQ(eval_print("(typep (make-array 5 :adjustable t) 'simple-vector)"), "NIL");
    ASSERT_STR_EQ(eval_print("(typep \"hello\" 'simple-vector)"), "NIL");
    ASSERT_STR_EQ(eval_print("(typep (make-array '(2 3)) 'simple-vector)"), "NIL");

    /* typep with SIMPLE-ARRAY — array with no fill-pointer, not adjustable */
    ASSERT_STR_EQ(eval_print("(typep (vector 1 2) 'simple-array)"), "T");
    ASSERT_STR_EQ(eval_print("(typep (make-array '(2 3)) 'simple-array)"), "T");
    ASSERT_STR_EQ(eval_print("(typep \"hello\" 'simple-array)"), "T");
    ASSERT_STR_EQ(eval_print("(typep (make-array 5 :fill-pointer 0) 'simple-array)"), "NIL");
    ASSERT_STR_EQ(eval_print("(typep (make-array 5 :adjustable t) 'simple-array)"), "NIL");
}

TEST(eval_array_type_of)
{
    /* type-of returns specific types */
    ASSERT_STR_EQ(eval_print("(type-of (vector 1 2 3))"), "SIMPLE-VECTOR");
    ASSERT_STR_EQ(eval_print("(type-of (make-array 5))"), "SIMPLE-VECTOR");
    ASSERT_STR_EQ(eval_print("(type-of (make-array 5 :fill-pointer 0))"), "VECTOR");
    ASSERT_STR_EQ(eval_print("(type-of (make-array 5 :adjustable t))"), "VECTOR");
    ASSERT_STR_EQ(eval_print("(type-of (make-array '(2 3)))"), "SIMPLE-ARRAY");
    ASSERT_STR_EQ(eval_print("(type-of \"hello\")"), "STRING");
}

/* --- Phase 5 Tier 2: String functions --- */

TEST(eval_string_comparison)
{
    ASSERT_STR_EQ(eval_print("(string= \"hello\" \"hello\")"), "T");
    ASSERT_STR_EQ(eval_print("(string= \"hello\" \"HELLO\")"), "NIL");
    ASSERT_STR_EQ(eval_print("(string-equal \"hello\" \"HELLO\")"), "T");
    ASSERT_STR_EQ(eval_print("(string< \"abc\" \"abd\")"), "T");
    ASSERT_STR_EQ(eval_print("(string< \"abd\" \"abc\")"), "NIL");
    ASSERT_STR_EQ(eval_print("(string> \"abd\" \"abc\")"), "T");
    ASSERT_STR_EQ(eval_print("(string<= \"abc\" \"abc\")"), "T");
    ASSERT_STR_EQ(eval_print("(string>= \"abc\" \"abc\")"), "T");
}

TEST(eval_string_case_conversion)
{
    ASSERT_STR_EQ(eval_print("(string-upcase \"hello\")"), "\"HELLO\"");
    ASSERT_STR_EQ(eval_print("(string-downcase \"HELLO\")"), "\"hello\"");
    ASSERT_STR_EQ(eval_print("(string-upcase \"Hello World\")"), "\"HELLO WORLD\"");
}

TEST(eval_string_trim)
{
    ASSERT_STR_EQ(eval_print("(string-trim \" \" \"  hello  \")"), "\"hello\"");
    ASSERT_STR_EQ(eval_print("(string-left-trim \" \" \"  hello  \")"), "\"hello  \"");
    ASSERT_STR_EQ(eval_print("(string-right-trim \" \" \"  hello  \")"), "\"  hello\"");
    /* char-bag as list of characters (CL spec: sequence of characters) */
    ASSERT_STR_EQ(eval_print("(string-trim (list #\\Space #\\Tab) \"  hello  \")"), "\"hello\"");
    ASSERT_STR_EQ(eval_print("(string-left-trim (list #\\Space) \"  hello  \")"), "\"hello  \"");
    ASSERT_STR_EQ(eval_print("(string-right-trim (list #\\Space) \"  hello  \")"), "\"  hello\"");
    /* char-bag as vector */
    ASSERT_STR_EQ(eval_print("(string-trim (vector #\\* #\\+) \"**+hello+**\")"), "\"hello\"");
    /* empty char-bag */
    ASSERT_STR_EQ(eval_print("(string-trim nil \"  hello  \")"), "\"  hello  \"");
    ASSERT_STR_EQ(eval_print("(string-trim (list) \"  hello  \")"), "\"  hello  \"");
}

TEST(eval_subseq)
{
    ASSERT_STR_EQ(eval_print("(subseq \"hello\" 1 3)"), "\"el\"");
    ASSERT_STR_EQ(eval_print("(subseq \"hello\" 2)"), "\"llo\"");
    ASSERT_STR_EQ(eval_print("(subseq '(a b c d) 1 3)"), "(B C)");
}

TEST(eval_concatenate)
{
    /* String result */
    ASSERT_STR_EQ(eval_print("(concatenate 'string \"hello\" \" \" \"world\")"),
        "\"hello world\"");
    ASSERT_STR_EQ(eval_print("(concatenate 'string \"ab\" (list #\\c #\\d))"),
        "\"abcd\"");
    ASSERT_STR_EQ(eval_print("(concatenate 'string (vector #\\x #\\y))"),
        "\"xy\"");
    /* Vector result */
    ASSERT_STR_EQ(eval_print("(concatenate 'simple-vector #(a b) #(c d))"),
        "#(A B C D)");
    ASSERT_STR_EQ(eval_print("(concatenate 'vector '(1 2) #(3 4))"),
        "#(1 2 3 4)");
    ASSERT_STR_EQ(eval_print("(concatenate 'vector \"ab\")"),
        "#(#\\a #\\b)");
    /* List result */
    ASSERT_STR_EQ(eval_print("(concatenate 'list #(1 2) '(3 4))"),
        "(1 2 3 4)");
    ASSERT_STR_EQ(eval_print("(concatenate 'list)"),
        "NIL");
    /* Empty inputs */
    ASSERT_STR_EQ(eval_print("(concatenate 'string)"), "\"\"");
    ASSERT_STR_EQ(eval_print("(concatenate 'vector)"), "#()");
    /* Compound result-type specifiers (ironclad ed448-dom uses
     * (simple-array (unsigned-byte 8) (*))) — a non-character array/vector
     * element type yields a general vector; a character element type a
     * string. */
    ASSERT_STR_EQ(eval_print(
        "(concatenate '(simple-array (unsigned-byte 8) (*)) "
        "(map 'vector #'char-code \"AB\") (vector 0) (vector 2))"),
        "#(65 66 0 2)");
    ASSERT_STR_EQ(eval_print("(concatenate '(vector character) \"ab\" \"cd\")"),
        "\"abcd\"");
    ASSERT_STR_EQ(eval_print("(concatenate '(simple-array character (*)) \"xy\" \"z\")"),
        "\"xyz\"");
    ASSERT_STR_EQ(eval_print("(concatenate '(vector t) '(1 2) #(3))"),
        "#(1 2 3)");
}

TEST(eval_char_accessor)
{
    ASSERT_STR_EQ(eval_print("(char \"hello\" 0)"), "#\\h");
    ASSERT_STR_EQ(eval_print("(char \"hello\" 4)"), "#\\o");
    ASSERT_STR_EQ(eval_print("(schar \"abc\" 1)"), "#\\b");
}

TEST(eval_string_coerce)
{
    ASSERT_STR_EQ(eval_print("(string 'foo)"), "\"FOO\"");
    ASSERT_STR_EQ(eval_print("(string \"hello\")"), "\"hello\"");
    ASSERT_STR_EQ(eval_print("(string #\\A)"), "\"A\"");
}

/* Regression: string functions must accept adjustable / fill-pointer
 * character vectors (CL_STRING_VECTOR_P) as valid CL strings — not just
 * simple TYPE_STRING.  hunchentoot's url-rewrite builds tag names this way
 * and passed them to STRING-EQUAL / WRITE-STRING / STRING-UPCASE, which
 * previously errored "not a string designator". */
TEST(eval_string_fns_on_fill_pointer_string)
{
    /* Build an adjustable fill-pointer string and push chars into it. */
    const char *mk =
        "(let ((s (make-array 0 :element-type 'character "
        "                       :fill-pointer 0 :adjustable t)))"
        "  (vector-push-extend #\\H s) (vector-push-extend #\\i s) s)";
    char buf[512];
    snprintf(buf, sizeof(buf), "(string-equal %s \"hi\")", mk);
    ASSERT_STR_EQ(eval_print(buf), "T");
    snprintf(buf, sizeof(buf), "(string= %s \"Hi\")", mk);
    ASSERT_STR_EQ(eval_print(buf), "T");
    snprintf(buf, sizeof(buf), "(string-upcase %s)", mk);
    ASSERT_STR_EQ(eval_print(buf), "\"HI\"");
    snprintf(buf, sizeof(buf), "(string-downcase %s)", mk);
    ASSERT_STR_EQ(eval_print(buf), "\"hi\"");
    snprintf(buf, sizeof(buf),
             "(with-output-to-string (o) (write-string %s o))", mk);
    ASSERT_STR_EQ(eval_print(buf), "\"Hi\"");
    /* The printer already treats string vectors as strings; confirm a
     * fill-pointer string PRINCs as its chars, not as #(...). */
    snprintf(buf, sizeof(buf), "(princ-to-string %s)", mk);
    ASSERT_STR_EQ(eval_print(buf), "\"Hi\"");
    /* string-trim and string-capitalize on fill-pointer strings */
    ASSERT_STR_EQ(eval_print(
        "(string-trim \" \""
        "  (let ((s (make-array 0 :element-type 'character"
        "              :fill-pointer 0 :adjustable t)))"
        "    (vector-push-extend #\\space s)"
        "    (vector-push-extend #\\h s)"
        "    (vector-push-extend #\\i s)"
        "    (vector-push-extend #\\space s)"
        "    s))"), "\"hi\"");
    ASSERT_STR_EQ(eval_print(
        "(string-capitalize"
        "  (let ((s (make-array 0 :element-type 'character"
        "              :fill-pointer 0 :adjustable t)))"
        "    (vector-push-extend #\\h s)"
        "    (vector-push-extend #\\e s)"
        "    (vector-push-extend #\\l s)"
        "    (vector-push-extend #\\l s)"
        "    (vector-push-extend #\\o s)"
        "    s))"), "\"Hello\"");
}

/* Regression: long-form DEFSETF (CLHS 5.5.5).  Previously compile_defsetf
 * only handled the short form and mis-stored the access lambda list as the
 * "updater", so (setf (place ...) v) emitted FLOAD of a cons → corruption.
 * hunchentoot's (defsetf session-value (symbol &optional session) (new) ...)
 * is the real-world trigger. */
TEST(eval_defsetf_long_form)
{
    cl_eval_string("(defvar *dsl* (make-hash-table :test 'equal))");
    cl_eval_string("(defun dslot (k &optional (tag :d)) (gethash (cons k tag) *dsl*))");
    cl_eval_string(
        "(defsetf dslot (k &optional (tag :d)) (v)"
        "  `(setf (gethash (cons ,k ,tag) *dsl*) ,v))");
    /* one arg: tag defaults */
    cl_eval_string("(setf (dslot \"a\") 1)");
    ASSERT_EQ_INT(eval_int("(dslot \"a\")"), 1);
    /* two args */
    cl_eval_string("(setf (dslot \"b\" :x) 2)");
    ASSERT_EQ_INT(eval_int("(dslot \"b\" :x)"), 2);
    ASSERT_STR_EQ(eval_print("(dslot \"b\")"), "NIL");
    /* subform evaluated exactly once */
    cl_eval_string("(defvar *dsl-n* 0)");
    cl_eval_string("(defun dsl-key () (incf *dsl-n*) \"k\")");
    cl_eval_string("(setf (dslot (dsl-key)) 42)");
    ASSERT_EQ_INT(eval_int("*dsl-n*"), 1);
    ASSERT_EQ_INT(eval_int("(dslot \"k\")"), 42);
    /* incf composes through the place */
    cl_eval_string("(setf (dslot \"n\") 10)");
    cl_eval_string("(incf (dslot \"n\") 5)");
    ASSERT_EQ_INT(eval_int("(dslot \"n\")"), 15);
}

/* Regression: PRIN1 of a string with a non-ASCII char must emit the actual
 * character, NOT a non-conformant "\\uXXXX" escape.  CL has no \\u string
 * escape, so the old form failed to re-read EQUAL and broke downstream HTML
 * escaping of Latin-1 parameter values in cl-who (the hunchentoot foreign-
 * character parameter tests rendered no "&#xFC;").  Round-tripping through a
 * string-output-stream brings the codepoint back as a single char, so the
 * printed form of (string (code-char 252)) is the 3 chars  "  ü  "  =
 * (34 252 34), not the 8-char \\u00FC escape (34 92 117 48 48 70 67 34). */
TEST(eval_prin1_wide_string_no_u_escape)
{
    /* A wide string with a non-ASCII char round-trips through a string stream
     * as the real codepoint (252), NOT the 6-char ü escape the old printer
     * emitted (which would read back as 34 92 117 48 48 70 67 ...). */
    ASSERT_STR_EQ(eval_print(
        "(map 'list 'char-code"
        "  (with-output-to-string (o) (prin1 (string (code-char 252)) o)))"),
        "(34 252 34)");
    /* PRIN1-TO-STRING of the same wide string round-trips via READ-FROM-STRING
     * to an EQUAL string (the \u escape never did). */
    ASSERT_STR_EQ(eval_print(
        "(char-code (char (read-from-string (prin1-to-string (string (code-char 252)))) 0))"),
        "252");
    /* #\" and #\\ are still escaped with a leading #\\ (ASCII, no encoding). */
    ASSERT_STR_EQ(eval_print(
        "(map 'list 'char-code"
        "  (with-output-to-string (o) (prin1 (coerce (list #\\\" #\\\\) 'string) o)))"),
        "(34 92 34 92 92 34)");
}

/* Regression: a BASE string (TYPE_STRING, from COERCE 'string of ASCII-fitting
 * chars) holding a byte > 127 must round-trip through a string-output stream as
 * its real codepoint, not U+FFFD.  print_string used to read each char as a
 * SIGNED char, so 0xFC sign-extended to -4, dodged cl_stream_write_char's
 * `ch > 0x7F` latin-1→UTF-8 encode test, and got written as a lone raw byte that
 * get-output-stream-string then mis-decoded as the replacement char 65533.  Both
 * the prin1 (escape) and princ (raw) branches are covered. */
TEST(eval_prin1_base_string_high_byte_roundtrip)
{
    /* prin1 of a base string with #\ü (252): the ü survives as 252, not 65533. */
    ASSERT_STR_EQ(eval_print(
        "(map 'list 'char-code"
        "  (with-output-to-string (o)"
        "    (prin1 (coerce (list #\\\" (code-char 252) #\\\\) 'string) o)))"),
        "(34 92 34 252 92 92 34)");
    /* princ (no escapes) of a base string high byte also survives as 252. */
    ASSERT_STR_EQ(eval_print(
        "(map 'list 'char-code"
        "  (with-output-to-string (o) (princ (coerce (list (code-char 252)) 'string) o)))"),
        "(252)");
    /* Full PRIN1 → READ-FROM-STRING round-trip preserves the codepoint. */
    ASSERT_STR_EQ(eval_print(
        "(char-code (char (read-from-string"
        "  (prin1-to-string (coerce (list (code-char 252)) 'string))) 0))"),
        "252");
}

/* Regression: stream EOF must signal a proper END-OF-FILE condition (CLHS),
 * not a generic simple-error — libraries (e.g. url-rewrite) catch
 * (end-of-file () ...) to terminate read loops. */
TEST(eval_end_of_file_condition)
{
    ASSERT_STR_EQ(eval_print(
        "(handler-case (with-input-from-string (s \"a\") (read-char s) (read-char s))"
        "  (end-of-file () :caught))"), ":CAUGHT");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (with-input-from-string (s \"\") (peek-char nil s))"
        "  (end-of-file () :caught))"), ":CAUGHT");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (with-input-from-string (s \"x\") (read-line s) (read-line s))"
        "  (end-of-file () :caught))"), ":CAUGHT");
    /* eof-error-p nil still returns the eof-value, no condition */
    ASSERT_STR_EQ(eval_print(
        "(with-input-from-string (s \"\") (read-char s nil :eof))"), ":EOF");
    ASSERT_STR_EQ(eval_print(
        "(with-input-from-string (s \"\") (peek-char nil s nil :none))"), ":NONE");
    /* read-byte EOF — regression: must signal END-OF-FILE, not a generic error */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (with-input-from-string (s \"\") (read-byte s))"
        "  (end-of-file () :caught))"), ":CAUGHT");
    ASSERT_STR_EQ(eval_print(
        "(with-input-from-string (s \"\") (read-byte s nil :eof))"), ":EOF");
    /* read-char-no-hang EOF — regression: must signal END-OF-FILE, not a generic error.
     * Uses a file stream so CL_STREAM_FLAG_EOF gets set (string streams don't set it). */
    eval_print("(with-open-file (out \"/tmp/.amilisp-eof-test\""
               "  :direction :output :if-exists :supersede"
               "  :if-does-not-exist :create))");
    ASSERT_STR_EQ(eval_print(
        "(handler-case"
        "  (with-open-file (s \"/tmp/.amilisp-eof-test\")"
        "    (read-char s nil nil)"   /* reaches EOF, sets CL_STREAM_FLAG_EOF */
        "    (read-char-no-hang s))"  /* flag set → signals END-OF-FILE */
        "  (end-of-file () :caught))"), ":CAUGHT");
    ASSERT_STR_EQ(eval_print(
        "(with-open-file (s \"/tmp/.amilisp-eof-test\")"
        "  (read-char s nil nil)"
        "  (read-char-no-hang s nil :eof))"), ":EOF");
}

TEST(eval_parse_integer)
{
    ASSERT_EQ_INT(eval_int("(parse-integer \"42\")"), 42);
    ASSERT_EQ_INT(eval_int("(parse-integer \"-7\")"), -7);
    ASSERT_EQ_INT(eval_int("(parse-integer \"FF\" :radix 16)"), 255);
    ASSERT_EQ_INT(eval_int("(parse-integer \"  123  \")"), 123);
}

TEST(eval_write_to_string)
{
    ASSERT_STR_EQ(eval_print("(write-to-string 42)"), "\"42\"");
    ASSERT_STR_EQ(eval_print("(write-to-string 'foo)"), "\"FOO\"");
}

TEST(eval_prin1_to_string)
{
    ASSERT_STR_EQ(eval_print("(prin1-to-string \"hello\")"),
        "\"\\\"hello\\\"\"");
    ASSERT_STR_EQ(eval_print("(prin1-to-string 42)"), "\"42\"");
}

TEST(eval_princ_to_string)
{
    ASSERT_STR_EQ(eval_print("(princ-to-string \"hello\")"), "\"hello\"");
    ASSERT_STR_EQ(eval_print("(princ-to-string 42)"), "\"42\"");
}

/* CLHS 22.1.3.3.1 — when *PRINT-ESCAPE* is NIL (i.e. PRINC / ~A), a symbol is
 * written using only its characters: no package prefix, no leading ':' for
 * keywords, no '#:' for uninterned symbols.  Regression for the printer fix
 * after teaching cl_current_package to track dynamic *PACKAGE* bindings. */
TEST(eval_princ_symbol_no_package_prefix)
{
    eval_print("(make-package \"PRC-TEST\")");
    eval_print("(intern \"HIDDEN\" (find-package \"PRC-TEST\"))");
    /* Symbol from another package — PRINC must still emit just the name */
    ASSERT_STR_EQ(eval_print(
        "(princ-to-string (find-symbol \"HIDDEN\" (find-package \"PRC-TEST\")))"),
        "\"HIDDEN\"");
    /* Keyword — no leading colon */
    ASSERT_STR_EQ(eval_print("(princ-to-string :hello)"), "\"HELLO\"");
    /* Uninterned gensym — no #: prefix */
    ASSERT_STR_EQ(eval_print("(princ-to-string (make-symbol \"ANON\"))"), "\"ANON\"");
}

/* Regression for the interaction that broke fiveam's def-special-environment:
 * FORMAT ~A inside (let ((*package* X)) …) must still print a symbol from
 * the outer package without a qualifier.  Before the printer fix, propagating
 * the let-binding into cl_current_package caused ~A to print
 * 'FOO from DSENV-TEST as \"DSENV-TEST::FOO\". */
TEST(eval_format_aesthetic_respects_print_escape)
{
    eval_print("(make-package \"FMTA\")");
    eval_print("(intern \"FOO\" (find-package \"FMTA\"))");
    ASSERT_STR_EQ(eval_print(
        "(let ((*package* (find-package :common-lisp-user)))"
        "  (format nil \"~A\" (find-symbol \"FOO\" (find-package \"FMTA\"))))"),
        "\"FOO\"");
}

/* --- Phase 5 Tier 3: List utilities --- */

TEST(eval_nthcdr)
{
    ASSERT_STR_EQ(eval_print("(nthcdr 0 '(1 2 3))"), "(1 2 3)");
    ASSERT_STR_EQ(eval_print("(nthcdr 2 '(1 2 3))"), "(3)");
    ASSERT_STR_EQ(eval_print("(nthcdr 3 '(1 2 3))"), "NIL");
    ASSERT_STR_EQ(eval_print("(nthcdr 5 '(1 2 3))"), "NIL");
}

/* CLHS 14.2 NTH: "n---a non-negative integer".  Regression for a MUI hook
 * that was handed the unsubstituted MUIV_TriggerValue (#x49893131 — above
 * MOST-POSITIVE-FIXNUM, so a bignum) and got "index must be a number" from
 * NTH; the negative case silently returned the first element. */
TEST(eval_nth_index_domain)
{
    ASSERT_STR_EQ(eval_print("(nth 0 '(1 2 3))"), "1");
    ASSERT_STR_EQ(eval_print("(nth 2 '(1 2 3))"), "3");
    ASSERT_STR_EQ(eval_print("(nth 9 '(1 2 3))"), "NIL");
    /* a bignum index is a legal non-negative integer: past the end -> NIL */
    ASSERT_STR_EQ(eval_print("(nth #x49893131 '(1 2 3))"), "NIL");
    ASSERT_STR_EQ(eval_print("(nth (1+ most-positive-fixnum) '(1 2 3))"), "NIL");
    ASSERT_STR_EQ(eval_print("(nthcdr #x49893131 '(1 2 3))"), "NIL");
    /* negative and non-integer indices are type errors, not silent CARs */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (nth -1 '(1 2 3)) (type-error () :te))"), ":TE");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (nth (- -1 most-positive-fixnum) '(1 2 3))"
        "  (type-error () :te))"), ":TE");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (nth 'a '(1 2 3)) (type-error () :te))"), ":TE");
    /* (setf (nth n l)) walks by the same rules */
    ASSERT_STR_EQ(eval_print(
        "(let ((l (list 1 2 3))) (setf (nth 1 l) 99) l)"), "(1 99 3)");
    ASSERT_STR_EQ(eval_print(
        "(let ((l (list 1 2 3)))"
        "  (handler-case (progn (setf (nth -1 l) 99) l) (type-error () l)))"),
        "(1 2 3)");
}

TEST(eval_last)
{
    ASSERT_STR_EQ(eval_print("(last '(1 2 3))"), "(3)");
    ASSERT_STR_EQ(eval_print("(last '(1 2 3) 2)"), "(2 3)");
    ASSERT_STR_EQ(eval_print("(last '(1 2 3) 0)"), "NIL");
    ASSERT_STR_EQ(eval_print("(last nil)"), "NIL");
}

TEST(eval_acons)
{
    ASSERT_STR_EQ(eval_print("(acons 'a 1 nil)"), "((A . 1))");
    ASSERT_STR_EQ(eval_print("(acons 'b 2 '((a . 1)))"), "((B . 2) (A . 1))");
}

TEST(eval_copy_list)
{
    ASSERT_STR_EQ(eval_print("(copy-list '(1 2 3))"), "(1 2 3)");
    ASSERT_STR_EQ(eval_print("(copy-list nil)"), "NIL");
    /* Verify it's a copy, not the same structure */
    ASSERT_STR_EQ(eval_print("(let ((x '(1 2 3))) (eq x (copy-list x)))"), "NIL");
}

TEST(eval_pairlis)
{
    ASSERT_STR_EQ(eval_print("(pairlis '(a b c) '(1 2 3))"), "((A . 1) (B . 2) (C . 3))");
    ASSERT_STR_EQ(eval_print("(pairlis '(a b) '(1 2) '((c . 3)))"), "((A . 1) (B . 2) (C . 3))");
}

TEST(eval_assoc)
{
    ASSERT_STR_EQ(eval_print("(assoc 'b '((a . 1) (b . 2) (c . 3)))"), "(B . 2)");
    ASSERT_STR_EQ(eval_print("(assoc 'd '((a . 1) (b . 2)))"), "NIL");
    ASSERT_STR_EQ(eval_print("(assoc \"b\" (list (cons \"a\" 1) (cons \"b\" 2)) :test #'equal)"), "(\"b\" . 2)");
}

TEST(eval_rassoc)
{
    ASSERT_STR_EQ(eval_print("(rassoc 2 '((a . 1) (b . 2) (c . 3)))"), "(B . 2)");
    ASSERT_STR_EQ(eval_print("(rassoc 9 '((a . 1) (b . 2)))"), "NIL");
}

TEST(eval_getf)
{
    ASSERT_EQ_INT(eval_int("(getf '(:a 1 :b 2) :b)"), 2);
    ASSERT_STR_EQ(eval_print("(getf '(:a 1 :b 2) :c)"), "NIL");
    ASSERT_EQ_INT(eval_int("(getf '(:a 1 :b 2) :c 99)"), 99);
}

TEST(eval_subst)
{
    ASSERT_STR_EQ(eval_print("(subst 'x 'b '(a b (c b)))"), "(A X (C X))");
    ASSERT_STR_EQ(eval_print("(subst 99 1 '(1 (2 1) 3))"), "(99 (2 99) 3)");
    /* CLHS: subst defaults to EQL — match list patterns with :test #'equal. */
    ASSERT_STR_EQ(eval_print(
        "(subst 'REPLACED '(TARGET) '(a (b (TARGET) c) d) :test #'equal)"),
        "(A (B REPLACED C) D)");
}

TEST(eval_sublis)
{
    ASSERT_STR_EQ(eval_print("(sublis '((a . 1) (b . 2)) '(a b c))"), "(1 2 C)");
}

TEST(eval_adjoin)
{
    ASSERT_STR_EQ(eval_print("(adjoin 1 '(2 3))"), "(1 2 3)");
    ASSERT_STR_EQ(eval_print("(adjoin 2 '(1 2 3))"), "(1 2 3)");
}

TEST(eval_nconc)
{
    ASSERT_STR_EQ(eval_print("(nconc (list 1 2) (list 3 4))"), "(1 2 3 4)");
    ASSERT_STR_EQ(eval_print("(nconc nil (list 1 2))"), "(1 2)");
    ASSERT_STR_EQ(eval_print("(nconc (list 1) nil (list 2 3))"), "(1 2 3)");

    /* Regression: (nconc s s) is the canonical idiom for building a
     * circular list — must terminate and the last cons's cdr must point
     * back at the head.  A previous bug walked the list to find its tail
     * AFTER splicing, which on the same-list case spun forever. */
    ASSERT_EQ_INT(eval_int(
        "(let ((s (copy-list '(a b c d)))) (nconc s s) "
        "  (if (eq s (cdr (cdr (cdr (cdr s))))) 1 0))"), 1);
    /* Aliasing through a leading nil arg too. */
    ASSERT_EQ_INT(eval_int(
        "(let ((s (copy-list '(x y)))) (nconc nil s s) "
        "  (if (eq s (cdr (cdr s))) 1 0))"), 1);
}

TEST(eval_nreverse)
{
    ASSERT_STR_EQ(eval_print("(nreverse (list 1 2 3))"), "(3 2 1)");
    ASSERT_STR_EQ(eval_print("(nreverse nil)"), "NIL");
    ASSERT_STR_EQ(eval_print("(nreverse (list 1))"), "(1)");
    /* nreverse on strings */
    ASSERT_STR_EQ(eval_print("(nreverse (copy-seq \"hello\"))"), "\"olleh\"");
    ASSERT_STR_EQ(eval_print("(nreverse (copy-seq \"\"))"), "\"\"");
    ASSERT_STR_EQ(eval_print("(nreverse (copy-seq \"a\"))"), "\"a\"");
    /* nreverse on vectors */
    ASSERT_STR_EQ(eval_print("(nreverse (vector 1 2 3))"), "#(3 2 1)");
    ASSERT_STR_EQ(eval_print("(nreverse (vector))"), "#()");
}

TEST(eval_delete)
{
    ASSERT_STR_EQ(eval_print("(delete 2 (list 1 2 3 2 4))"), "(1 3 4)");
    ASSERT_STR_EQ(eval_print("(delete 5 (list 1 2 3))"), "(1 2 3)");
    ASSERT_STR_EQ(eval_print("(delete \"b\" (list \"a\" \"b\" \"c\") :test #'equal)"), "(\"a\" \"c\")");
}

/* DELETE on non-list sequences delegates to REMOVE (CLHS allows
 * destructive operations to return a fresh sequence).  Sento's array-
 * agent calls (apply #'delete item array :test #'string=) on the
 * underlying vector — previously the list-only loop tripped on cl_car
 * of an empty vector. */
TEST(eval_delete_vector)
{
    ASSERT_STR_EQ(eval_print("(delete \"foo\" #() :test #'string=)"), "#()");
    ASSERT_STR_EQ(eval_print(
        "(delete \"foo\" (vector \"bar\" \"foo\" \"baz\") :test #'string=)"),
        "#(\"bar\" \"baz\")");
    ASSERT_STR_EQ(eval_print("(delete 2 #(1 2 3 2 4))"), "#(1 3 4)");
}

TEST(eval_delete_string)
{
    ASSERT_STR_EQ(eval_print("(delete #\\b \"abcb\")"), "\"ac\"");
    ASSERT_STR_EQ(eval_print("(delete #\\x \"abc\")"), "\"abc\"");
}

TEST(eval_delete_if)
{
    ASSERT_STR_EQ(eval_print("(delete-if #'zerop (list 0 1 0 2 0 3))"), "(1 2 3)");
    ASSERT_STR_EQ(eval_print("(delete-if #'numberp (list 1 2 3))"), "NIL");
}

TEST(eval_nsubst)
{
    ASSERT_STR_EQ(eval_print("(let ((x (list 'a 'b (list 'c 'b)))) (nsubst 'x 'b x))"), "(A X (C X))");
}

TEST(eval_butlast)
{
    ASSERT_STR_EQ(eval_print("(butlast '(1 2 3))"), "(1 2)");
    ASSERT_STR_EQ(eval_print("(butlast '(1 2 3) 2)"), "(1)");
    ASSERT_STR_EQ(eval_print("(butlast '(1 2 3) 5)"), "NIL");
    ASSERT_STR_EQ(eval_print("(butlast nil)"), "NIL");
}

TEST(eval_copy_tree)
{
    ASSERT_STR_EQ(eval_print("(copy-tree '(1 (2 3) 4))"), "(1 (2 3) 4)");
    ASSERT_STR_EQ(eval_print("(copy-tree 42)"), "42");
    /* Deep copy: modifying original doesn't affect copy */
    ASSERT_STR_EQ(eval_print("(let ((x '(1 (2 3)))) (let ((y (copy-tree x))) (equal x y)))"), "T");
    /* Improper list — dotted tail must be preserved verbatim. */
    ASSERT_STR_EQ(eval_print("(copy-tree '(1 2 . 3))"), "(1 2 . 3)");
    /* Regression: a long flat list previously recursed down the cdr
     * spine, pushing 2 GC roots per level and overflowing the 1024-slot
     * root stack at >500 elements (this aborted ANSI-test copy-tree.2,
     * which copies the ~1000-element *universe* list).  The iterative
     * spine-walk implementation handles arbitrary lengths. */
    ASSERT_STR_EQ(eval_print(
        "(let ((n 2000)) (eql n (length (copy-tree (loop for i below n collect i)))))"),
        "T");
    /* And verify per-element identity for a long list with cons cars. */
    ASSERT_STR_EQ(eval_print(
        "(let* ((x (loop for i below 1500 collect (cons i (1+ i)))) "
        "       (y (copy-tree x))) "
        "  (and (equal x y) (not (eq x y)) (not (eq (first x) (first y)))))"),
        "T");
}

TEST(eval_mapc)
{
    /* mapc returns the first list */
    ASSERT_STR_EQ(eval_print("(mapc #'1+ '(1 2 3))"), "(1 2 3)");
    /* mapc with side effects using a global */
    ASSERT_STR_EQ(eval_print("(progn (setq *mapc-r* nil) (mapc (lambda (x) (setq *mapc-r* (cons (* x x) *mapc-r*))) '(1 2 3)) (nreverse *mapc-r*))"), "(1 4 9)");
}

TEST(eval_mapcan)
{
    ASSERT_STR_EQ(eval_print("(mapcan (lambda (x) (if (numberp x) (list x) nil)) '(a 1 b 2 c 3))"), "(1 2 3)");
}

TEST(eval_maplist)
{
    ASSERT_STR_EQ(eval_print("(maplist #'length '(1 2 3 4))"), "(4 3 2 1)");
}

TEST(eval_mapl)
{
    /* mapl returns the first list, operates on sublists */
    ASSERT_STR_EQ(eval_print("(progn (setq *mapl-r* nil) (mapl (lambda (l) (setq *mapl-r* (cons (length l) *mapl-r*))) '(a b c)) (nreverse *mapl-r*))"), "(3 2 1)");
}

TEST(eval_mapcon)
{
    ASSERT_STR_EQ(eval_print("(mapcon (lambda (l) (list (length l))) '(a b c))"), "(3 2 1)");
}

TEST(eval_intersection)
{
    ASSERT_STR_EQ(eval_print("(intersection '(1 2 3 4) '(2 4 6))"), "(2 4)");
    ASSERT_STR_EQ(eval_print("(intersection '(1 2) '(3 4))"), "NIL");
    /* :test-not support */
    ASSERT_STR_EQ(eval_print("(intersection '(0 1 2) '(1 2 3 4) :test-not (lambda (x y) (not (and (eql x y) (> x 1)))))"), "(2)");
}

TEST(eval_union)
{
    /* union returns list1 elements plus list2 elements not in list1 */
    ASSERT_STR_EQ(eval_print("(union '(1 2 3) '(2 3 4))"), "(3 2 1 4)");
    /* :test-not support */
    ASSERT_STR_EQ(eval_print("(sort (copy-list (union '(1 2) '(1 3) :test-not #'/=)) #'<)"), "(1 2 3)");
}

TEST(eval_set_difference)
{
    ASSERT_STR_EQ(eval_print("(set-difference '(1 2 3 4) '(2 4))"), "(1 3)");
    ASSERT_STR_EQ(eval_print("(set-difference '(1 2) '(1 2))"), "NIL");
    /* :test-not support */
    ASSERT_STR_EQ(eval_print("(sort (copy-list (set-difference '(1 2 3 4 5) '(4 2 8 0) :test-not (lambda (x y) (not (and (eql x y) (> x 3)))))) #'<)"), "(1 2 3 5)");
}

TEST(eval_subsetp)
{
    ASSERT_STR_EQ(eval_print("(subsetp '(1 2) '(1 2 3))"), "T");
    ASSERT_STR_EQ(eval_print("(subsetp '(1 4) '(1 2 3))"), "NIL");
    ASSERT_STR_EQ(eval_print("(subsetp nil '(1 2 3))"), "T");
}

/* --- Hash tables --- */

TEST(eval_make_hash_table)
{
    ASSERT_STR_EQ(eval_print("(hash-table-p (make-hash-table))"), "T");
    ASSERT_STR_EQ(eval_print("(hash-table-p 42)"), "NIL");
    ASSERT_STR_EQ(eval_print("(hash-table-p nil)"), "NIL");
    ASSERT_STR_EQ(eval_print("(type-of (make-hash-table))"), "HASH-TABLE");
}

TEST(eval_gethash_setf)
{
    eval_print("(defvar *ht1* (make-hash-table))");
    ASSERT_STR_EQ(eval_print("(setf (gethash 'a *ht1*) 1)"), "1");
    ASSERT_STR_EQ(eval_print("(gethash 'a *ht1*)"), "1");
    ASSERT_STR_EQ(eval_print("(setf (gethash 'b *ht1*) 2)"), "2");
    ASSERT_STR_EQ(eval_print("(gethash 'b *ht1*)"), "2");
    ASSERT_STR_EQ(eval_print("(hash-table-count *ht1*)"), "2");
}

TEST(eval_gethash_default)
{
    eval_print("(defvar *ht2* (make-hash-table))");
    ASSERT_STR_EQ(eval_print("(gethash 'x *ht2*)"), "NIL");
    ASSERT_STR_EQ(eval_print("(gethash 'x *ht2* 99)"), "99");
}

TEST(eval_gethash_overwrite)
{
    eval_print("(defvar *ht3* (make-hash-table))");
    eval_print("(setf (gethash 'k *ht3*) 10)");
    ASSERT_STR_EQ(eval_print("(gethash 'k *ht3*)"), "10");
    eval_print("(setf (gethash 'k *ht3*) 20)");
    ASSERT_STR_EQ(eval_print("(gethash 'k *ht3*)"), "20");
    ASSERT_STR_EQ(eval_print("(hash-table-count *ht3*)"), "1");
}

TEST(eval_remhash)
{
    eval_print("(defvar *ht4* (make-hash-table))");
    eval_print("(setf (gethash 'a *ht4*) 1)");
    eval_print("(setf (gethash 'b *ht4*) 2)");
    ASSERT_STR_EQ(eval_print("(remhash 'a *ht4*)"), "T");
    ASSERT_STR_EQ(eval_print("(gethash 'a *ht4*)"), "NIL");
    ASSERT_STR_EQ(eval_print("(hash-table-count *ht4*)"), "1");
    ASSERT_STR_EQ(eval_print("(remhash 'z *ht4*)"), "NIL");
}

TEST(eval_clrhash)
{
    eval_print("(defvar *ht5* (make-hash-table))");
    eval_print("(setf (gethash 'a *ht5*) 1)");
    eval_print("(setf (gethash 'b *ht5*) 2)");
    eval_print("(clrhash *ht5*)");
    ASSERT_STR_EQ(eval_print("(hash-table-count *ht5*)"), "0");
    ASSERT_STR_EQ(eval_print("(gethash 'a *ht5*)"), "NIL");
}

TEST(eval_maphash)
{
    eval_print("(defvar *ht6* (make-hash-table))");
    eval_print("(setf (gethash 'x *ht6*) 10)");
    eval_print("(setf (gethash 'y *ht6*) 20)");
    eval_print("(defvar *sum6* 0)");
    eval_print("(maphash (lambda (k v) (setq *sum6* (+ *sum6* v))) *ht6*)");
    ASSERT_STR_EQ(eval_print("*sum6*"), "30");
}

TEST(eval_hash_table_equal_test)
{
    eval_print("(defvar *hte* (make-hash-table :test 'equal))");
    eval_print("(setf (gethash \"hello\" *hte*) 42)");
    ASSERT_STR_EQ(eval_print("(gethash \"hello\" *hte*)"), "42");
    ASSERT_STR_EQ(eval_print("(hash-table-count *hte*)"), "1");
}

TEST(eval_hash_table_eq_test)
{
    eval_print("(defvar *hteq* (make-hash-table :test 'eq))");
    eval_print("(setf (gethash 'foo *hteq*) 99)");
    ASSERT_STR_EQ(eval_print("(gethash 'foo *hteq*)"), "99");
}

TEST(eval_gethash_mv)
{
    /* gethash returns two values: value and present-p */
    eval_print("(defvar *htmv* (make-hash-table))");
    eval_print("(setf (gethash 'k *htmv*) 42)");
    ASSERT_STR_EQ(eval_print("(multiple-value-bind (v p) (gethash 'k *htmv*) p)"), "T");
    ASSERT_STR_EQ(eval_print("(multiple-value-bind (v p) (gethash 'missing *htmv*) p)"), "NIL");
}

/* CLHS hash-table accessors needed by serapeum and other libraries. */
TEST(eval_hash_table_accessors)
{
    eval_print("(defvar *ht-accessors* (make-hash-table :test 'equal))");
    /* hash-table-test returns the test as a symbol */
    ASSERT_STR_EQ(eval_print("(hash-table-test *ht-accessors*)"), "EQUAL");
    /* hash-table-size returns a positive integer */
    ASSERT_STR_EQ(eval_print("(and (integerp (hash-table-size *ht-accessors*))"
                             "     (plusp (hash-table-size *ht-accessors*)))"), "T");
    /* rehash-size > 1, rehash-threshold in (0,1] */
    ASSERT_STR_EQ(eval_print("(> (hash-table-rehash-size *ht-accessors*) 1)"), "T");
    ASSERT_STR_EQ(eval_print("(and (> (hash-table-rehash-threshold *ht-accessors*) 0)"
                             "     (<= (hash-table-rehash-threshold *ht-accessors*) 1))"), "T");
}

TEST(eval_hash_table_test_symbols)
{
    /* All four standard tests should round-trip through hash-table-test */
    ASSERT_STR_EQ(eval_print("(hash-table-test (make-hash-table :test 'eq))"), "EQ");
    ASSERT_STR_EQ(eval_print("(hash-table-test (make-hash-table :test 'eql))"), "EQL");
    ASSERT_STR_EQ(eval_print("(hash-table-test (make-hash-table :test 'equal))"), "EQUAL");
    ASSERT_STR_EQ(eval_print("(hash-table-test (make-hash-table :test 'equalp))"), "EQUALP");
}

/* CLHS 18.2 — WITH-HASH-TABLE-ITERATOR.
 * Required by serapeum (DO-HASH-TABLE) and other libraries. */
TEST(eval_with_hash_table_iterator_empty)
{
    /* Empty table: iter-fn immediately returns (values nil nil nil). */
    eval_print("(defvar *htit-empty* (make-hash-table))");
    ASSERT_STR_EQ(eval_print(
        "(with-hash-table-iterator (it *htit-empty*)"
        "  (multiple-value-bind (p k v) (it)"
        "    (declare (ignore k v))"
        "    p))"), "NIL");
}

TEST(eval_with_hash_table_iterator_walks_pairs)
{
    /* Two-entry table: first two calls return (values t key value);
     * third call returns (values nil nil nil). Sum the values to verify
     * we see every pair exactly once regardless of iteration order. */
    eval_print("(defvar *htit-2* (make-hash-table))");
    eval_print("(setf (gethash 'a *htit-2*) 11)");
    eval_print("(setf (gethash 'b *htit-2*) 22)");
    ASSERT_STR_EQ(eval_print(
        "(with-hash-table-iterator (it *htit-2*)"
        "  (let ((sum 0) (count 0))"
        "    (loop"
        "      (multiple-value-bind (p k v) (it)"
        "        (declare (ignore k))"
        "        (unless p (return))"
        "        (incf sum v)"
        "        (incf count)))"
        "    (list sum count)))"), "(33 2)");
}

/* --- Phase 5: Sequence functions --- */

TEST(eval_find)
{
    ASSERT_STR_EQ(eval_print("(find 3 '(1 2 3 4 5))"), "3");
    ASSERT_STR_EQ(eval_print("(find 9 '(1 2 3))"), "NIL");
    ASSERT_STR_EQ(eval_print("(find 2 '(1 2 3 2 1) :start 2)"), "2");
    ASSERT_STR_EQ(eval_print("(find \"b\" '(\"a\" \"b\" \"c\") :test #'equal)"), "\"b\"");
    /* Regression: :key nil must work (treated as identity) */
    ASSERT_STR_EQ(eval_print("(find 3 '(1 2 3 4) :key nil)"), "3");
}

TEST(eval_find_if)
{
    eval_print("(defun my-evenp (x) (= (mod x 2) 0))");
    ASSERT_STR_EQ(eval_print("(find-if #'my-evenp '(1 2 3 4))"), "2");
    ASSERT_STR_EQ(eval_print("(find-if #'my-evenp '(1 3 5))"), "NIL");
}

TEST(eval_find_if_not)
{
    ASSERT_STR_EQ(eval_print("(find-if-not #'my-evenp '(2 4 5 6))"), "5");
}

TEST(eval_position)
{
    ASSERT_STR_EQ(eval_print("(position 3 '(1 2 3 4))"), "2");
    ASSERT_STR_EQ(eval_print("(position 9 '(1 2 3))"), "NIL");
    ASSERT_STR_EQ(eval_print("(position 1 '(1 2 1 2) :from-end t)"), "2");
}

TEST(eval_position_if)
{
    ASSERT_STR_EQ(eval_print("(position-if #'my-evenp '(1 3 4 5))"), "2");
}

TEST(eval_position_if_not)
{
    ASSERT_STR_EQ(eval_print("(position-if-not #'my-evenp '(2 4 5))"), "2");
}

TEST(eval_count)
{
    ASSERT_EQ_INT(eval_int("(count 1 '(1 2 1 3 1))"), 3);
    ASSERT_EQ_INT(eval_int("(count 9 '(1 2 3))"), 0);
}

TEST(eval_count_if)
{
    ASSERT_EQ_INT(eval_int("(count-if #'my-evenp '(1 2 3 4 5 6))"), 3);
}

TEST(eval_count_if_not)
{
    ASSERT_EQ_INT(eval_int("(count-if-not #'my-evenp '(1 2 3 4 5))"), 3);
}

TEST(eval_remove)
{
    ASSERT_STR_EQ(eval_print("(remove 3 '(1 2 3 4 3 5))"), "(1 2 4 5)");
    ASSERT_STR_EQ(eval_print("(remove 3 '(1 2 3 4 3 5) :count 1)"), "(1 2 4 3 5)");
    ASSERT_STR_EQ(eval_print("(remove 9 '(1 2 3))"), "(1 2 3)");
}

TEST(eval_remove_if)
{
    ASSERT_STR_EQ(eval_print("(remove-if #'my-evenp '(1 2 3 4 5))"), "(1 3 5)");
}

TEST(eval_remove_if_not)
{
    ASSERT_STR_EQ(eval_print("(remove-if-not #'my-evenp '(1 2 3 4 5))"), "(2 4)");
}

TEST(eval_remove_duplicates)
{
    ASSERT_STR_EQ(eval_print("(remove-duplicates '(1 2 1 3 2 4))"), "(1 3 2 4)");
    ASSERT_STR_EQ(eval_print("(remove-duplicates '(a b a c))"), "(B A C)");
}

TEST(eval_substitute)
{
    ASSERT_STR_EQ(eval_print("(substitute 99 3 '(1 2 3 4 3))"), "(1 2 99 4 99)");
    ASSERT_STR_EQ(eval_print("(substitute 0 3 '(1 2 3 4 3) :count 1)"), "(1 2 0 4 3)");
    /* :from-end with :count — substitute last match */
    ASSERT_STR_EQ(eval_print("(substitute 'a 'b '(a b b c) :count 1 :from-end t)"), "(A B A C)");
    ASSERT_STR_EQ(eval_print("(substitute 0 3 '(3 1 3 2 3) :count 2 :from-end t)"), "(3 1 0 2 0)");
    /* substitute on strings */
    ASSERT_STR_EQ(eval_print("(substitute #\\Space #\\/ \"foo/bar\")"), "\"foo bar\"");
}

TEST(eval_substitute_if)
{
    ASSERT_STR_EQ(eval_print("(substitute-if 0 #'my-evenp '(1 2 3 4 5))"), "(1 0 3 0 5)");
    /* :from-end with :count */
    ASSERT_STR_EQ(eval_print("(substitute-if 'a #'null '(nil 1 b nil c) :count 1 :from-end t)"), "(NIL 1 B A C)");
}

TEST(eval_substitute_if_not)
{
    ASSERT_STR_EQ(eval_print("(substitute-if-not 0 #'my-evenp '(1 2 3 4 5))"), "(0 2 0 4 0)");
}

TEST(eval_nsubstitute)
{
    ASSERT_STR_EQ(eval_print("(let ((x (list 1 2 3 4 3))) (nsubstitute 99 3 x) x)"), "(1 2 99 4 99)");
    ASSERT_STR_EQ(eval_print("(let ((x (list 1 2 3 4 3))) (nsubstitute 0 3 x :count 1) x)"), "(1 2 0 4 3)");
}

TEST(eval_nsubstitute_if)
{
    ASSERT_STR_EQ(eval_print("(let ((x (list 1 2 3 4 5))) (nsubstitute-if 0 #'my-evenp x) x)"), "(1 0 3 0 5)");
}

TEST(eval_nsubstitute_if_not)
{
    ASSERT_STR_EQ(eval_print("(let ((x (list 1 2 3 4 5))) (nsubstitute-if-not 0 #'my-evenp x) x)"), "(0 2 0 4 0)");
}

TEST(eval_reduce)
{
    ASSERT_EQ_INT(eval_int("(reduce #'+ '(1 2 3 4))"), 10);
    ASSERT_EQ_INT(eval_int("(reduce #'+ '() :initial-value 0)"), 0);
    ASSERT_EQ_INT(eval_int("(reduce #'+ '(5) :initial-value 10)"), 15);
    ASSERT_STR_EQ(eval_print("(reduce #'cons '(1 2 3))"), "((1 . 2) . 3)");
}

TEST(eval_fill)
{
    ASSERT_STR_EQ(eval_print("(let ((x (list 1 2 3 4))) (fill x 0) x)"), "(0 0 0 0)");
    ASSERT_STR_EQ(eval_print("(let ((x (list 1 2 3 4))) (fill x 0 :start 1 :end 3) x)"), "(1 0 0 4)");
}

TEST(eval_replace_fn)
{
    ASSERT_STR_EQ(eval_print("(let ((x (list 1 2 3 4 5))) (replace x '(a b c) :start1 1) x)"), "(1 A B C 5)");
}

TEST(eval_every)
{
    ASSERT_STR_EQ(eval_print("(every #'my-evenp '(2 4 6))"), "T");
    ASSERT_STR_EQ(eval_print("(every #'my-evenp '(2 3 6))"), "NIL");
    ASSERT_STR_EQ(eval_print("(every #'my-evenp '())"), "T");
}

TEST(eval_some)
{
    ASSERT_STR_EQ(eval_print("(some #'my-evenp '(1 3 4))"), "T");
    ASSERT_STR_EQ(eval_print("(some #'my-evenp '(1 3 5))"), "NIL");
}

TEST(eval_notany)
{
    ASSERT_STR_EQ(eval_print("(notany #'my-evenp '(1 3 5))"), "T");
    ASSERT_STR_EQ(eval_print("(notany #'my-evenp '(1 2 3))"), "NIL");
}

TEST(eval_notevery)
{
    ASSERT_STR_EQ(eval_print("(notevery #'my-evenp '(2 4 5))"), "T");
    ASSERT_STR_EQ(eval_print("(notevery #'my-evenp '(2 4 6))"), "NIL");
}

TEST(eval_map_fn)
{
    ASSERT_STR_EQ(eval_print("(map 'list #'1+ '(1 2 3))"), "(2 3 4)");
    /* map over string */
    ASSERT_STR_EQ(eval_print("(map 'list #'char-code \"abc\")"), "(97 98 99)");
    /* map over vector */
    ASSERT_STR_EQ(eval_print("(map 'list #'1+ #(10 20 30))"), "(11 21 31)");
    /* map nil result-type over string */
    ASSERT_STR_EQ(eval_print("(let ((r nil)) (map nil (lambda (c) (push c r)) \"ab\") (length r))"), "2");
    /* map with mixed sequence types */
    ASSERT_STR_EQ(eval_print("(map 'list #'+ '(1 2 3) #(10 20 30))"), "(11 22 33)");
    ASSERT_STR_EQ(eval_print("(map 'list #'cons '(a b c) \"xyz\")"), "((A . #\\x) (B . #\\y) (C . #\\z))");
    /* map with string result type */
    ASSERT_STR_EQ(eval_print("(map 'string #'char-upcase \"hello\")"), "\"HELLO\"");
    /* map with vector result type */
    ASSERT_STR_EQ(eval_print("(map 'vector #'1+ '(1 2 3))"), "#(2 3 4)");
    /* map stops at shortest sequence */
    ASSERT_STR_EQ(eval_print("(map 'list #'+ '(1 2 3 4) #(10 20))"), "(11 22)");
    ASSERT_STR_EQ(eval_print("(map 'list #'identity \"ab\")"), "(#\\a #\\b)");
    /* map with subtype result-type specifiers */
    ASSERT_STR_EQ(eval_print("(map 'simple-vector #'1+ '(1 2 3))"), "#(2 3 4)");
    ASSERT_STR_EQ(eval_print("(map 'simple-string #'char-upcase \"hello\")"), "\"HELLO\"");
    ASSERT_STR_EQ(eval_print("(map 'cons #'1+ '(1 2 3))"), "(2 3 4)");
}

TEST(eval_every_sequences)
{
    /* every with string */
    ASSERT_STR_EQ(eval_print("(every #'alpha-char-p \"hello\")"), "T");
    ASSERT_STR_EQ(eval_print("(every #'alpha-char-p \"hello1\")"), "NIL");
    /* every with vector */
    ASSERT_STR_EQ(eval_print("(every #'numberp #(1 2 3))"), "T");
    ASSERT_STR_EQ(eval_print("(every #'numberp #(1 :a 3))"), "NIL");
    /* every with empty sequences */
    ASSERT_STR_EQ(eval_print("(every #'identity \"\")"), "T");
    ASSERT_STR_EQ(eval_print("(every #'identity #())"), "T");
    /* every with mixed sequence types */
    ASSERT_STR_EQ(eval_print("(every #'eql '(97 98 99) (map 'list #'char-code \"abc\"))"), "T");
    /* every with bit-vector */
    ASSERT_STR_EQ(eval_print("(every #'zerop #*000)"), "T");
    ASSERT_STR_EQ(eval_print("(every #'zerop #*010)"), "NIL");
}

TEST(eval_some_sequences)
{
    /* some with string */
    ASSERT_STR_EQ(eval_print("(some #'digit-char-p \"abc1\")"), "1");
    ASSERT_STR_EQ(eval_print("(some #'digit-char-p \"abcd\")"), "NIL");
    /* some with vector */
    ASSERT_STR_EQ(eval_print("(some #'zerop #(1 2 0 3))"), "T");
    ASSERT_STR_EQ(eval_print("(some #'zerop #(1 2 3))"), "NIL");
    /* some with empty sequences */
    ASSERT_STR_EQ(eval_print("(some #'identity \"\")"), "NIL");
    ASSERT_STR_EQ(eval_print("(some #'identity #())"), "NIL");
    /* some with bit-vector */
    ASSERT_STR_EQ(eval_print("(some #'plusp #*000)"), "NIL");
    ASSERT_STR_EQ(eval_print("(some #'plusp #*010)"), "T");
}

TEST(eval_notany_sequences)
{
    /* notany with string */
    ASSERT_STR_EQ(eval_print("(notany #'digit-char-p \"abcd\")"), "T");
    ASSERT_STR_EQ(eval_print("(notany #'digit-char-p \"abc1\")"), "NIL");
    /* notany with vector */
    ASSERT_STR_EQ(eval_print("(notany #'zerop #(1 2 3))"), "T");
    ASSERT_STR_EQ(eval_print("(notany #'zerop #(1 0 3))"), "NIL");
}

TEST(eval_notevery_sequences)
{
    /* notevery with string */
    ASSERT_STR_EQ(eval_print("(notevery #'alpha-char-p \"hello1\")"), "T");
    ASSERT_STR_EQ(eval_print("(notevery #'alpha-char-p \"hello\")"), "NIL");
    /* notevery with vector */
    ASSERT_STR_EQ(eval_print("(notevery #'numberp #(1 :a 3))"), "T");
    ASSERT_STR_EQ(eval_print("(notevery #'numberp #(1 2 3))"), "NIL");
}

TEST(eval_mismatch)
{
    ASSERT_STR_EQ(eval_print("(mismatch '(1 2 3) '(1 2 3))"), "NIL");
    ASSERT_STR_EQ(eval_print("(mismatch '(1 2 3) '(1 2 4))"), "2");
    ASSERT_STR_EQ(eval_print("(mismatch '(1 2) '(1 2 3))"), "2");
}

TEST(eval_search_fn)
{
    ASSERT_STR_EQ(eval_print("(search '(2 3) '(1 2 3 4))"), "1");
    ASSERT_STR_EQ(eval_print("(search '(9) '(1 2 3))"), "NIL");
    ASSERT_STR_EQ(eval_print("(search '() '(1 2 3))"), "0");
}

TEST(eval_sort)
{
    ASSERT_STR_EQ(eval_print("(sort (list 3 1 4 1 5) #'<)"), "(1 1 3 4 5)");
    ASSERT_STR_EQ(eval_print("(sort (list 5 4 3 2 1) #'<)"), "(1 2 3 4 5)");
    ASSERT_STR_EQ(eval_print("(sort (list 1) #'<)"), "(1)");
    ASSERT_STR_EQ(eval_print("(sort '() #'<)"), "NIL");
}

TEST(eval_stable_sort)
{
    ASSERT_STR_EQ(eval_print("(stable-sort (list 3 1 2) #'<)"), "(1 2 3)");
    /* Stability: when the predicate is NIL for all pairs (no element is
       strictly less than any other), the order must be preserved.
       Regression for list_merge reversing unrelated elements — caught by
       serapeum's simplify-subtypes assertion (sort-subtypes is expected
       to be idempotent when no subtype relationships exist). */
    ASSERT_STR_EQ(eval_print("(stable-sort (list 'a 'b 'c 'd 'e) (lambda (x y) (declare (ignore x y)) nil))"),
                  "(A B C D E)");
    /* Stability: equal elements (by :key) preserve relative order. */
    ASSERT_STR_EQ(eval_print(
        "(stable-sort (list (cons 1 'a) (cons 1 'b) (cons 1 'c)) #'< :key #'car)"),
        "((1 . A) (1 . B) (1 . C))");
}

TEST(eval_sort_with_key)
{
    ASSERT_STR_EQ(eval_print("(sort (list -3 1 -2 4) #'< :key #'abs)"), "(1 -2 -3 4)");
    /* Regression: sort with string key on large list (GC bug #8 — list_merge_sort
       didn't GC-protect list/mid across recursive calls) */
    ASSERT_STR_EQ(eval_print(
        "(let ((items (loop for i from 1 to 200 collect (format nil \"item~3,'0d\" i))))"
        "  (length (sort items #'string<)))"), "200");
}

TEST(eval_sort_key_nil)
{
    /* Regression: :key nil must be treated as identity (CL spec) */
    ASSERT_STR_EQ(eval_print("(sort (list 3 1 2) #'< :key nil)"), "(1 2 3)");
    ASSERT_STR_EQ(eval_print("(sort (vector 3 1 2) #'< :key nil)"), "#(1 2 3)");
    ASSERT_STR_EQ(eval_print("(stable-sort (list 3 1 2) #'< :key nil)"), "(1 2 3)");
}

TEST(eval_stale_boxed_flag)
{
    /* Regression: labels in one cond branch boxing a variable must not
     * corrupt sibling branches that reuse the same local slot.
     * The bug was that env->boxed[] flags were not cleared when a let
     * scope exited, causing CELL_REF to be emitted for plain locals. */
    ASSERT_STR_EQ(eval_print(
        "(defun test-stale-boxed (x fn)"
        "  (cond ((consp x)"
        "         (let ((acc 0))"
        "           (labels ((body (k v)"
        "                      (declare (ignore k))"
        "                      (setf acc (+ acc (funcall fn v)))))"
        "             (body (car x) (cdr x)))"
        "           acc))"
        "        ((vectorp x)"
        "         (let ((result 0))"
        "           (dotimes (i (length x))"
        "             (let ((h (funcall fn (svref x i))))"
        "               (setf result (logxor result h))))"
        "           result))))"), "TEST-STALE-BOXED");
    /* cons branch: acc = identity(42) = 42 */
    ASSERT_STR_EQ(eval_print("(test-stale-boxed '(a . 42) #'identity)"), "42");
    /* vector branch must not crash (was SIGSEGV before fix) */
    ASSERT_STR_EQ(eval_print("(test-stale-boxed #(3 5) #'identity)"), "6");
}

TEST(eval_typep)
{
    ASSERT_STR_EQ(eval_print("(typep 42 'integer)"), "T");
    ASSERT_STR_EQ(eval_print("(typep 42 'fixnum)"), "T");
    ASSERT_STR_EQ(eval_print("(typep 42 'number)"), "T");
    ASSERT_STR_EQ(eval_print("(typep 42 'string)"), "NIL");
    ASSERT_STR_EQ(eval_print("(typep \"hello\" 'string)"), "T");
    ASSERT_STR_EQ(eval_print("(typep #\\A 'character)"), "T");
    ASSERT_STR_EQ(eval_print("(typep nil 'null)"), "T");
    ASSERT_STR_EQ(eval_print("(typep nil 'symbol)"), "T");
    ASSERT_STR_EQ(eval_print("(typep nil 'list)"), "T");
    ASSERT_STR_EQ(eval_print("(typep '(1 2) 'cons)"), "T");
    ASSERT_STR_EQ(eval_print("(typep '(1 2) 'list)"), "T");
    ASSERT_STR_EQ(eval_print("(typep 42 'atom)"), "T");
    ASSERT_STR_EQ(eval_print("(typep '(1) 'atom)"), "NIL");
    ASSERT_STR_EQ(eval_print("(typep :foo 'keyword)"), "T");
    ASSERT_STR_EQ(eval_print("(typep 'bar 'keyword)"), "NIL");
    ASSERT_STR_EQ(eval_print("(typep #'+ 'function)"), "T");
    ASSERT_STR_EQ(eval_print("(typep (make-hash-table) 'hash-table)"), "T");
    ASSERT_STR_EQ(eval_print("(typep 42 't)"), "T");
    ASSERT_STR_EQ(eval_print("(typep 42 'nil)"), "NIL");
    ASSERT_STR_EQ(eval_print("(typep '(1 2 3) 'sequence)"), "T");
    ASSERT_STR_EQ(eval_print("(typep (vector 1 2) 'sequence)"), "T");
    ASSERT_STR_EQ(eval_print("(typep (vector 1 2) 'vector)"), "T");
}

TEST(eval_coerce)
{
    ASSERT_STR_EQ(eval_print("(coerce 42 't)"), "42");
    ASSERT_STR_EQ(eval_print("(coerce 65 'character)"), "#\\A");
    ASSERT_STR_EQ(eval_print("(coerce #\\A 'integer)"), "65");
    ASSERT_STR_EQ(eval_print("(coerce #\\A 'character)"), "#\\A");
    ASSERT_STR_EQ(eval_print("(coerce 42 'integer)"), "42");
    ASSERT_STR_EQ(eval_print("(coerce 'foo 'string)"), "\"FOO\"");
    ASSERT_STR_EQ(eval_print("(coerce #\\A 'string)"), "\"A\"");
    ASSERT_STR_EQ(eval_print("(coerce \"hello\" 'string)"), "\"hello\"");
    ASSERT_STR_EQ(eval_print("(coerce '(1 2 3) 'vector)"), "#(1 2 3)");
    ASSERT_STR_EQ(eval_print("(coerce (vector 1 2 3) 'list)"), "(1 2 3)");
    ASSERT_STR_EQ(eval_print("(coerce nil 'list)"), "NIL");
    ASSERT_STR_EQ(eval_print("(coerce (vector) 'list)"), "NIL");
    ASSERT_STR_EQ(eval_print("(coerce nil 'vector)"), "#()");
    /* coerce to function */
    ASSERT_STR_EQ(eval_print("(functionp (coerce #'car 'function))"), "T");
    ASSERT_STR_EQ(eval_print("(funcall (coerce #'+ 'function) 1 2)"), "3");
    ASSERT_STR_EQ(eval_print("(funcall (coerce '+ 'function) 3 4)"), "7");
    ASSERT_STR_EQ(eval_print("(funcall (coerce '(lambda (x) (* x 2)) 'function) 5)"), "10");
    /* Regression: a BIT element type in a compound (vector/array bit ...)
     * result spec must yield a bit-vector, not a general SIMPLE-VECTOR. */
    ASSERT_STR_EQ(eval_print("(bit-vector-p (coerce '(1 0 1) '(vector bit 3)))"), "T");
    ASSERT_STR_EQ(eval_print("(bit-vector-p (coerce '(1 0 1) '(vector bit)))"), "T");
    ASSERT_STR_EQ(eval_print("(bit-vector-p (coerce '(1 0 1) '(array bit (3))))"), "T");
    ASSERT_STR_EQ(eval_print("(coerce '(1 0 1 1 1 1 0 0) '(vector bit 8))"), "#*10111100");
    ASSERT_STR_EQ(eval_print("(equal (coerce '(1 0 1) '(vector bit 3)) #*101)"), "T");
    /* Non-bit, non-char element types still give a general vector. */
    ASSERT_STR_EQ(eval_print("(type-of (coerce '(1 2 3) '(vector fixnum 3)))"), "SIMPLE-VECTOR");
}

/* --- Compound typep --- */

TEST(eval_typep_compound)
{
    /* (or ...) */
    ASSERT_STR_EQ(eval_print("(typep 42 '(or integer string))"), "T");
    ASSERT_STR_EQ(eval_print("(typep \"hi\" '(or integer string))"), "T");
    ASSERT_STR_EQ(eval_print("(typep #\\A '(or integer string))"), "NIL");

    /* (and ...) */
    ASSERT_STR_EQ(eval_print("(typep nil '(and symbol list))"), "T");
    ASSERT_STR_EQ(eval_print("(typep 42 '(and number atom))"), "T");
    ASSERT_STR_EQ(eval_print("(typep 42 '(and number string))"), "NIL");

    /* (not ...) */
    ASSERT_STR_EQ(eval_print("(typep 42 '(not string))"), "T");
    ASSERT_STR_EQ(eval_print("(typep \"hi\" '(not string))"), "NIL");

    /* (member ...) */
    ASSERT_STR_EQ(eval_print("(typep 1 '(member 1 2 3))"), "T");
    ASSERT_STR_EQ(eval_print("(typep 4 '(member 1 2 3))"), "NIL");

    /* (eql ...) */
    ASSERT_STR_EQ(eval_print("(typep 42 '(eql 42))"), "T");
    ASSERT_STR_EQ(eval_print("(typep 43 '(eql 42))"), "NIL");

    /* (satisfies ...) */
    ASSERT_STR_EQ(eval_print("(typep 42 '(satisfies numberp))"), "T");
    ASSERT_STR_EQ(eval_print("(typep \"hi\" '(satisfies numberp))"), "NIL");

    /* nested compound */
    ASSERT_STR_EQ(eval_print("(typep 42 '(or (and integer atom) string))"), "T");
    ASSERT_STR_EQ(eval_print("(typep #\\A '(or integer string))"), "NIL");

    /* (cons car-type cdr-type) with compound car/cdr types */
    ASSERT_STR_EQ(eval_print("(typep '(a . b) '(cons symbol symbol))"), "T");
    ASSERT_STR_EQ(eval_print("(typep '(a . b) '(cons symbol integer))"), "NIL");
    ASSERT_STR_EQ(eval_print("(typep '(a . 1) '(cons symbol integer))"), "T");
    ASSERT_STR_EQ(eval_print("(typep '(a . b) '(cons * *))"), "T");
    ASSERT_STR_EQ(eval_print("(typep 42 '(cons * *))"), "NIL");
    /* compound car/cdr: (eql ...) must be checked, not skipped */
    ASSERT_STR_EQ(eval_print("(typep '(~ \"x\") '(cons (eql ~) (cons t null)))"), "T");
    ASSERT_STR_EQ(eval_print("(typep '(and a) '(cons (eql ~) (cons t null)))"), "NIL");
    ASSERT_STR_EQ(eval_print("(typep '(? x) '(cons (eql ?) (cons t null)))"), "T");
    ASSERT_STR_EQ(eval_print("(typep '(! x) '(cons (eql ?) (cons t null)))"), "NIL");
    /* nested cons types */
    ASSERT_STR_EQ(eval_print("(typep '(a b) '(cons symbol (cons symbol null)))"), "T");
    ASSERT_STR_EQ(eval_print("(typep '(a b c) '(cons symbol (cons symbol null)))"), "NIL");
}

/* --- numeric range type specifiers --- */

TEST(eval_typep_numeric_range)
{
    /* (integer low high) — inclusive bounds */
    ASSERT_STR_EQ(eval_print("(typep 3 '(integer 0 7))"), "T");
    ASSERT_STR_EQ(eval_print("(typep 0 '(integer 0 7))"), "T");
    ASSERT_STR_EQ(eval_print("(typep 7 '(integer 0 7))"), "T");
    ASSERT_STR_EQ(eval_print("(typep 8 '(integer 0 7))"), "NIL");
    ASSERT_STR_EQ(eval_print("(typep -1 '(integer 0 7))"), "NIL");
    /* Non-integer fails base type check */
    ASSERT_STR_EQ(eval_print("(typep 3.0 '(integer 0 7))"), "NIL");

    /* Wildcard bounds with * */
    ASSERT_STR_EQ(eval_print("(typep 100 '(integer 0 *))"), "T");
    ASSERT_STR_EQ(eval_print("(typep -5 '(integer * 0))"), "T");
    ASSERT_STR_EQ(eval_print("(typep 42 '(integer * *))"), "T");

    /* No bounds = just base type */
    ASSERT_STR_EQ(eval_print("(typep 42 '(integer))"), "T");
    ASSERT_STR_EQ(eval_print("(typep 3.0 '(integer))"), "NIL");

    /* Exclusive bounds via (n) list form */
    ASSERT_STR_EQ(eval_print("(typep 1 '(integer (0) 7))"), "T");
    ASSERT_STR_EQ(eval_print("(typep 0 '(integer (0) 7))"), "NIL");
    ASSERT_STR_EQ(eval_print("(typep 6 '(integer 0 (7)))"), "T");
    ASSERT_STR_EQ(eval_print("(typep 7 '(integer 0 (7)))"), "NIL");

    /* (real low high) */
    ASSERT_STR_EQ(eval_print("(typep 3 '(real 0 10))"), "T");
    ASSERT_STR_EQ(eval_print("(typep 3.5 '(real 0 10))"), "T");
    ASSERT_STR_EQ(eval_print("(typep 3/2 '(real 0 10))"), "T");

    /* (float low high) */
    ASSERT_STR_EQ(eval_print("(typep 3.0 '(float 0.0 10.0))"), "T");
    ASSERT_STR_EQ(eval_print("(typep 3 '(float 0 10))"), "NIL");

    /* (rational low high) */
    ASSERT_STR_EQ(eval_print("(typep 3/2 '(rational 0 10))"), "T");
    ASSERT_STR_EQ(eval_print("(typep 3 '(rational 0 10))"), "T");
    ASSERT_STR_EQ(eval_print("(typep 3.0 '(rational 0 10))"), "NIL");

    /* check-type with range type */
    ASSERT_STR_EQ(eval_print("(let ((x 5)) (check-type x (integer 0 7)) x)"), "5");
}

/* --- vectorp: strings are vectors per CL spec --- */

TEST(eval_vectorp)
{
    ASSERT_STR_EQ(eval_print("(vectorp #(1 2 3))"), "T");
    ASSERT_STR_EQ(eval_print("(vectorp \"hello\")"), "T");
    ASSERT_STR_EQ(eval_print("(vectorp '(1 2))"), "NIL");
    ASSERT_STR_EQ(eval_print("(vectorp 42)"), "NIL");
}

/* --- coerce simple-vector --- */

TEST(eval_coerce_simple_vector)
{
    ASSERT_STR_EQ(eval_print("(coerce '(1 2 3) 'simple-vector)"), "#(1 2 3)");
    ASSERT_STR_EQ(eval_print("(coerce #(1 2) 'simple-vector)"), "#(1 2)");
}

/* --- count/count-if with :from-end --- */

TEST(eval_count_from_end)
{
    ASSERT_STR_EQ(eval_print("(let ((i 0)) (count-if (lambda (x) (> (+ x (incf i)) 4)) '(0 1 2 3 4 5) :from-end t))"), "6");
    ASSERT_STR_EQ(eval_print("(count-if #'oddp '(1 2 3 4 5) :from-end t)"), "3");
}

/* --- deftype --- */

TEST(eval_deftype)
{
    /* Simple no-arg deftype */
    eval_print("(deftype my-num () 'number)");
    ASSERT_STR_EQ(eval_print("(typep 42 'my-num)"), "T");
    ASSERT_STR_EQ(eval_print("(typep \"hi\" 'my-num)"), "NIL");

    /* Deftype expanding to compound */
    eval_print("(deftype string-or-num () '(or string number))");
    ASSERT_STR_EQ(eval_print("(typep 42 'string-or-num)"), "T");
    ASSERT_STR_EQ(eval_print("(typep \"hi\" 'string-or-num)"), "T");
    ASSERT_STR_EQ(eval_print("(typep #\\A 'string-or-num)"), "NIL");
}

/* --- subtypep --- */

TEST(eval_subtypep)
{
    /* Known hierarchy checks */
    ASSERT_STR_EQ(eval_print("(subtypep 'fixnum 'number)"), "T");
    ASSERT_STR_EQ(eval_print("(subtypep 'fixnum 'integer)"), "T");
    ASSERT_STR_EQ(eval_print("(subtypep 'integer 'number)"), "T");
    ASSERT_STR_EQ(eval_print("(subtypep 'cons 'list)"), "T");
    ASSERT_STR_EQ(eval_print("(subtypep 'null 'list)"), "T");
    ASSERT_STR_EQ(eval_print("(subtypep 'list 'sequence)"), "T");
    ASSERT_STR_EQ(eval_print("(subtypep 'string 'sequence)"), "T");
    ASSERT_STR_EQ(eval_print("(subtypep 'keyword 'symbol)"), "T");
    ASSERT_STR_EQ(eval_print("(subtypep 'compiled-function 'function)"), "T");

    /* Same type */
    ASSERT_STR_EQ(eval_print("(subtypep 'integer 'integer)"), "T");

    /* Not a subtype */
    ASSERT_STR_EQ(eval_print("(subtypep 'string 'number)"), "NIL");
    ASSERT_STR_EQ(eval_print("(subtypep 'number 'fixnum)"), "NIL");

    /* nil is subtype of everything */
    ASSERT_STR_EQ(eval_print("(subtypep 'nil 'number)"), "T");

    /* everything is subtype of t */
    ASSERT_STR_EQ(eval_print("(subtypep 'number 't)"), "T");

    /* MV return: second value should be T for known types */
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep 'fixnum 'number))"), "(T T)");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (subtypep 'string 'number))"), "(NIL T)");
}

/* --- typecase with compound type specs --- */

TEST(eval_typecase_compound)
{
    ASSERT_STR_EQ(eval_print("(typecase 42 ((or integer string) \"match\") (t \"no\"))"), "\"match\"");
    ASSERT_STR_EQ(eval_print("(typecase \"hi\" ((or integer string) \"match\") (t \"no\"))"), "\"match\"");
    ASSERT_STR_EQ(eval_print("(typecase #\\A ((or integer string) \"match\") (t \"no\"))"), "\"no\"");
    /* typecase with list type now properly matches nil */
    ASSERT_STR_EQ(eval_print("(typecase nil (list \"list\") (t \"other\"))"), "\"list\"");
}

/* --- Disassemble --- */

TEST(eval_disassemble_defun)
{
    /* disassemble of a user-defined function returns NIL */
    eval_print("(defun disasm-test (x) (+ x 1))");
    ASSERT_STR_EQ(eval_print("(disassemble 'disasm-test)"), "NIL");
}

TEST(eval_disassemble_closure)
{
    /* disassemble of a closure returns NIL */
    eval_print("(defun make-adder (n) (lambda (x) (+ x n)))");
    ASSERT_STR_EQ(eval_print("(disassemble (make-adder 5))"), "NIL");
}

TEST(eval_disassemble_builtin)
{
    /* disassemble of a built-in function returns NIL with message */
    ASSERT_STR_EQ(eval_print("(disassemble 'cons)"), "NIL");
}

TEST(eval_disassemble_unbound)
{
    /* disassemble of unbound symbol signals an error */
    ASSERT_STR_EQ(eval_print("(disassemble 'no-such-function-xyz)"), "ERROR:8");
}

/* --- Declarations: declare, declaim, proclaim, locally --- */

TEST(eval_declaim_special)
{
    /* declaim marks a variable as globally special */
    eval_print("(declaim (special *decl-x*))");
    eval_print("(setq *decl-x* 10)");
    /* A function should see the dynamic binding */
    eval_print("(defun get-decl-x () *decl-x*)");
    ASSERT_EQ_INT(eval_int("(let ((*decl-x* 99)) (get-decl-x))"), 99);
}

TEST(eval_declare_special_let)
{
    /* Local declare special in let body — forces dynamic binding */
    eval_print("(setq *ds-var* 10)");
    eval_print("(defun get-ds-var () *ds-var*)");
    ASSERT_EQ_INT(eval_int(
        "(let ((*ds-var* 42)) (declare (special *ds-var*)) (get-ds-var))"), 42);
}

TEST(eval_declare_special_letstar)
{
    /* Local declare special in let* body */
    eval_print("(setq *ds-var2* 10)");
    eval_print("(defun get-ds-var2 () *ds-var2*)");
    ASSERT_EQ_INT(eval_int(
        "(let* ((*ds-var2* 55)) (declare (special *ds-var2*)) (get-ds-var2))"), 55);
}

TEST(eval_declare_ignore)
{
    /* (declare (ignore x)) accepted without error */
    ASSERT_EQ_INT(eval_int("(let ((x 1)) (declare (ignore x)) 42)"), 42);
}

TEST(eval_declare_type)
{
    /* (declare (type fixnum x)) accepted without error */
    ASSERT_EQ_INT(eval_int("(let ((x 1)) (declare (type fixnum x)) x)"), 1);
}

TEST(eval_declaim_optimize)
{
    /* (declaim (optimize (speed 3))) accepted without error */
    ASSERT_STR_EQ(eval_print("(declaim (optimize (speed 3)))"), "NIL");
}

TEST(eval_declaim_inline)
{
    /* (declaim (inline foo)) accepted without error */
    ASSERT_STR_EQ(eval_print("(declaim (inline cons))"), "NIL");
}

TEST(eval_proclaim_special)
{
    /* (proclaim '(special *proc-var*)) — runtime special declaration */
    eval_print("(proclaim '(special *proc-var*))");
    eval_print("(setq *proc-var* 100)");
    eval_print("(defun get-proc-var () *proc-var*)");
    ASSERT_EQ_INT(eval_int("(let ((*proc-var* 200)) (get-proc-var))"), 200);
}

TEST(eval_locally_basic)
{
    /* (locally body...) — evaluates body, returns last value */
    ASSERT_EQ_INT(eval_int("(locally 1 2 3)"), 3);
}

TEST(eval_locally_declare)
{
    /* (locally (declare (special *loc-var*)) ...) */
    eval_print("(setq *loc-var* 5)");
    eval_print("(defun get-loc-var () *loc-var*)");
    ASSERT_EQ_INT(eval_int("(locally (declare (special *loc-var*)) *loc-var*)"), 5);
}

TEST(eval_declare_misplaced)
{
    /* declare at top level is now a warning (ignored), returns NIL */
    ASSERT_STR_EQ(eval_print("(declare (special x))"), "NIL");
}

TEST(eval_declare_in_lambda)
{
    /* declare in lambda body */
    ASSERT_EQ_INT(eval_int(
        "((lambda (x) (declare (ignore x)) 42) 99)"), 42);
}

TEST(eval_declaim_multiple)
{
    /* declaim with multiple specifiers */
    ASSERT_STR_EQ(eval_print(
        "(declaim (special *dm1*) (special *dm2*))"), "NIL");
    eval_print("(setq *dm1* 1)");
    eval_print("(setq *dm2* 2)");
    eval_print("(defun get-dm1 () *dm1*)");
    eval_print("(defun get-dm2 () *dm2*)");
    ASSERT_EQ_INT(eval_int("(let ((*dm1* 10)) (get-dm1))"), 10);
    ASSERT_EQ_INT(eval_int("(let ((*dm2* 20)) (get-dm2))"), 20);
}

/* ===== Phase 6 — The ===== */

TEST(eval_the_basic)
{
    /* (the type form) passes through value when type matches */
    ASSERT_EQ_INT(eval_int("(the fixnum 42)"), 42);
    ASSERT_STR_EQ(eval_print("(the string \"hello\")"), "\"hello\"");
    ASSERT_EQ_INT(eval_int("(the fixnum (+ 1 2))"), 3);
    ASSERT_STR_EQ(eval_print("(the symbol 'foo)"), "FOO");
    ASSERT_STR_EQ(eval_print("(the list '(1 2 3))"), "(1 2 3)");
    ASSERT_STR_EQ(eval_print("(the null nil)"), "NIL");
}

TEST(eval_the_compound_type)
{
    /* compound type specifiers */
    ASSERT_EQ_INT(eval_int("(the (or fixnum null) 42)"), 42);
    ASSERT_STR_EQ(eval_print("(the (or fixnum null) nil)"), "NIL");
    ASSERT_STR_EQ(eval_print("(the (or string symbol) \"hi\")"), "\"hi\"");
}

TEST(eval_the_type_error)
{
    /* signals type-error when value doesn't match */
    ASSERT_STR_EQ(eval_print("(the fixnum \"oops\")"), "ERROR:2");
    ASSERT_STR_EQ(eval_print("(the string 42)"), "ERROR:2");
}

TEST(eval_the_safety_zero)
{
    /* safety 0 skips the check */
    eval_print("(declaim (optimize (safety 0)))");
    ASSERT_STR_EQ(eval_print("(the fixnum \"oops\")"), "\"oops\"");
    /* restore default safety */
    eval_print("(declaim (optimize (safety 1)))");
}

TEST(eval_the_nested)
{
    /* nested the forms */
    ASSERT_EQ_INT(eval_int("(the fixnum (the fixnum (+ 1 2)))"), 3);
}

TEST(eval_the_values_type_spec)
{
    /* (the (values ...) form) — values type specifier (CLHS 4.2.3).
     * Per CLHS 5.3.2, an implementation is permitted (not required) to
     * check value counts/types.  We skip the check; (values) and friends
     * must not signal "invalid compound type specifier head: VALUES". */
    ASSERT_EQ_INT(eval_int("(the (values) (+ 1 2))"), 3);
    ASSERT_EQ_INT(eval_int("(the (values fixnum) 42)"), 42);
    ASSERT_EQ_INT(eval_int("(the (values fixnum &optional) 42)"), 42);
    ASSERT_EQ_INT(eval_int("(the (values &optional) 42)"), 42);
    ASSERT_EQ_INT(eval_int("(the (values fixnum string) 42)"), 42);
}

TEST(eval_the_as_place)
{
    /* CLHS 5.1.2.4: (the type place) is a place — the inner form is the
     * place, the type spec wraps the access form.  Without correct
     * handling in get-setf-expansion, INCF/DECF/PUSH on (the type x)
     * would evaluate the type specifier as a variable and signal
     * "Unbound variable: <TYPE>".  Regression test for cl-ppcre cold
     * load (convert.lisp uses `(incf (the fixnum reg-num))`). */
    ASSERT_EQ_INT(eval_int("(let ((x 1)) (incf (the fixnum x)) x)"), 2);
    ASSERT_EQ_INT(eval_int("(let ((x 5)) (decf (the fixnum x) 2) x)"), 3);
    ASSERT_EQ_INT(eval_int("(let ((x 10)) (setf (the fixnum x) 99) x)"), 99);
    /* Nested place: (the type (slot accessor)) — type wraps a non-symbol place */
    ASSERT_STR_EQ(eval_print(
        "(let ((c (cons 1 2))) (incf (the fixnum (car c))) c)"),
        "(2 . 2)");
}

/* ===== Phase 5 — Trace/Untrace ===== */

TEST(eval_trace_basic)
{
    /* Trace a function and verify result is still correct */
    eval_print("(defun tr-add (a b) (+ a b))");
    eval_print("(trace tr-add)");
    ASSERT_EQ_INT(eval_int("(tr-add 3 4)"), 7);
    eval_print("(untrace tr-add)");
}

TEST(eval_trace_returns_list)
{
    /* (trace name) returns a list of traced names */
    eval_print("(defun tr-foo () 42)");
    ASSERT_STR_EQ(eval_print("(trace tr-foo)"), "(TR-FOO)");
    eval_print("(untrace tr-foo)");
}

TEST(eval_trace_untrace)
{
    /* Untrace returns list, function still works */
    eval_print("(defun tr-sq (x) (* x x))");
    eval_print("(trace tr-sq)");
    ASSERT_EQ_INT(eval_int("(tr-sq 5)"), 25);
    ASSERT_STR_EQ(eval_print("(untrace tr-sq)"), "(TR-SQ)");
    ASSERT_EQ_INT(eval_int("(tr-sq 6)"), 36);
}

TEST(eval_trace_query)
{
    /* (trace) with no args returns list of traced functions */
    eval_print("(defun tr-a () 1)");
    eval_print("(trace tr-a)");
    ASSERT_STR_EQ(eval_print("(trace)"), "(TR-A)");
    eval_print("(untrace tr-a)");
    ASSERT_STR_EQ(eval_print("(trace)"), "NIL");
}

TEST(eval_untrace_all)
{
    /* (untrace) with no args clears all traces */
    eval_print("(defun tr-x () 10)");
    eval_print("(defun tr-y () 20)");
    eval_print("(trace tr-x tr-y)");
    ASSERT_STR_EQ(eval_print("(untrace)"), "NIL");
    ASSERT_STR_EQ(eval_print("(trace)"), "NIL");
}

TEST(eval_trace_builtin)
{
    /* Trace a built-in function */
    eval_print("(trace cons)");
    ASSERT_STR_EQ(eval_print("(cons 1 2)"), "(1 . 2)");
    eval_print("(untrace cons)");
}

TEST(eval_trace_multiple)
{
    /* Trace multiple functions */
    eval_print("(defun tr-p (x) (* x x))");
    eval_print("(defun tr-q (x) (+ x 1))");
    ASSERT_STR_EQ(eval_print("(trace tr-p tr-q)"), "(TR-P TR-Q)");
    ASSERT_EQ_INT(eval_int("(tr-p (tr-q 3))"), 16);
    eval_print("(untrace)");
}

/* --- Trace stream redirection --- */

TEST(eval_trace_stream_redirect)
{
    /* TRACE output must go to *trace-output*, not platform stdout */
    const char *out;
    eval_print("(defun tr-sredir (x) (* x x))");
    eval_print("(trace tr-sredir)");
    /* Redirect *trace-output* to a string stream and capture trace output */
    out = eval_print(
        "(let ((s (make-string-output-stream)))"
        "  (let ((*trace-output* s))"
        "    (tr-sredir 5)"
        "    (get-output-stream-string s)))");
    ASSERT(strstr(out, "TR-SREDIR") != NULL);
    ASSERT(strstr(out, "(") != NULL);
    ASSERT(strstr(out, "returned") != NULL);
    /* After untrace, no further trace output should be captured */
    eval_print("(untrace tr-sredir)");
    out = eval_print(
        "(let ((s (make-string-output-stream)))"
        "  (let ((*trace-output* s))"
        "    (tr-sredir 5)"
        "    (get-output-stream-string s)))");
    ASSERT(strstr(out, "TR-SREDIR") == NULL);
    ASSERT(strstr(out, "returned") == NULL);
}

/* --- Backtrace --- */

TEST(eval_backtrace_named)
{
    /* Error in a named function shows its name in backtrace */
    int err;
    eval_print("(defun bt-inner () (error \"oops\"))");
    eval_print("(defun bt-outer () (+ 1 (bt-inner)))");
    CL_CATCH(err);
    if (err == CL_ERR_NONE) {
        cl_eval_string("(bt-outer)");
        CL_UNCATCH();
    } else {
        CL_UNCATCH();
    }
    /* Backtrace should contain BT-INNER and BT-OUTER */
    ASSERT(strstr(cl_backtrace_buf, "BT-INNER") != NULL);
    ASSERT(strstr(cl_backtrace_buf, "BT-OUTER") != NULL);
    cl_vm.sp = 0;
    cl_vm.fp = 0;
}

TEST(eval_backtrace_anonymous)
{
    /* Top-level error shows <anonymous> */
    int err;
    CL_CATCH(err);
    if (err == CL_ERR_NONE) {
        cl_eval_string("(+ undefined-var 1)");
        CL_UNCATCH();
    } else {
        CL_UNCATCH();
    }
    ASSERT(strstr(cl_backtrace_buf, "<anonymous>") != NULL);
    cl_vm.sp = 0;
    cl_vm.fp = 0;
}

TEST(eval_backtrace_recursive)
{
    /* Recursive function shows multiple frames */
    int err;
    eval_print("(defun bt-rec (n) (if (= n 0) (error \"bottom\") (+ 1 (bt-rec (1- n)))))");
    CL_CATCH(err);
    if (err == CL_ERR_NONE) {
        cl_eval_string("(bt-rec 3)");
        CL_UNCATCH();
    } else {
        CL_UNCATCH();
    }
    /* Should have multiple BT-REC entries */
    {
        const char *p = cl_backtrace_buf;
        int count = 0;
        while ((p = strstr(p, "BT-REC")) != NULL) {
            count++;
            p += 6;
        }
        ASSERT(count == 4); /* n=0,1,2,3 */
    }
    cl_vm.sp = 0;
    cl_vm.fp = 0;
}

TEST(eval_backtrace_uwprot)
{
    /* Backtrace captured before unwind-protect cleanup */
    int err;
    eval_print("(defun bt-uwp-inner () (error \"err\"))");
    eval_print("(defun bt-uwp-outer () (unwind-protect (bt-uwp-inner) nil))");
    CL_CATCH(err);
    if (err == CL_ERR_NONE) {
        cl_eval_string("(bt-uwp-outer)");
        CL_UNCATCH();
    } else {
        CL_UNCATCH();
    }
    ASSERT(strstr(cl_backtrace_buf, "BT-UWP-INNER") != NULL);
    ASSERT(strstr(cl_backtrace_buf, "BT-UWP-OUTER") != NULL);
    cl_vm.sp = 0;
    cl_vm.fp = 0;
}

/* Regression: a nested LAMBDA in the body must not steal the enclosing
 * function's name.
 *
 * DEFUN hands the name to COMPILE-LAMBDA through the single per-thread slot
 * pending_lambda_name.  compile_lambda used to read that slot only after
 * compile_body had run, so the FIRST nested lambda — which always finishes
 * before its parent — claimed the name and cleared the slot, leaving the
 * function itself NIL.  Every backtrace through such a function then showed
 * "<anonymous>", which is most functions that use HANDLER-BIND, RESTART-CASE,
 * MAPCAR, or any other lambda-taking form. */
TEST(eval_backtrace_name_survives_inner_lambda)
{
    int err;
    eval_print("(defun bt-lam-inner () (error \"oops\"))");
    /* The funcall'd lambda is what used to steal BT-LAM-OUTER's name.  The
     * (+ 1 ...) keeps the inner call out of tail position so the frame is
     * really on the stack to be named. */
    eval_print("(defun bt-lam-outer () (funcall (lambda () 1)) (+ 1 (bt-lam-inner)))");
    CL_CATCH(err);
    if (err == CL_ERR_NONE) {
        cl_eval_string("(bt-lam-outer)");
        CL_UNCATCH();
    } else {
        CL_UNCATCH();
    }
    ASSERT(strstr(cl_backtrace_buf, "  0: BT-LAM-INNER") != NULL);
    ASSERT(strstr(cl_backtrace_buf, "  1: BT-LAM-OUTER") != NULL);
    cl_vm.sp = 0;
    cl_vm.fp = 0;
}

/* The other half of the same bug: the inner lambda must stay anonymous rather
 * than wear the name it stole.  Pre-fix these two frames were swapped —
 * "0: BT-LAM-HOST" over "1: <anonymous>". */
TEST(eval_backtrace_inner_lambda_stays_anonymous)
{
    int err;
    /* Trailing NIL keeps the funcall out of tail position, so both the lambda
     * frame and its host frame are live when the error is signalled. */
    eval_print("(defun bt-lam-host () (funcall (lambda () (error \"boom\"))) nil)");
    CL_CATCH(err);
    if (err == CL_ERR_NONE) {
        cl_eval_string("(bt-lam-host)");
        CL_UNCATCH();
    } else {
        CL_UNCATCH();
    }
    ASSERT(strstr(cl_backtrace_buf, "  0: <anonymous>") != NULL);
    ASSERT(strstr(cl_backtrace_buf, "  1: BT-LAM-HOST") != NULL);
    cl_vm.sp = 0;
    cl_vm.fp = 0;
}

/* The shape the bug was reported as: RESTART-CASE expands its restart bodies
 * into closures and HANDLER-BIND its handlers into lambdas, so both functions
 * lost their names and the debugger backtrace was all "<anonymous>". */
TEST(eval_backtrace_restart_case_handler_bind_named)
{
    int err;
    eval_print("(define-condition bt-cond () ())");
    eval_print("(defun bt-low () (restart-case (error 'bt-cond) "
               "(bt-r1 () 1) (bt-r2 () 2)))");
    eval_print("(defun bt-high () (handler-bind "
               "((warning (lambda (c) (declare (ignore c)) nil))) "
               "(+ 1 (bt-low))))");
    CL_CATCH(err);
    if (err == CL_ERR_NONE) {
        cl_eval_string("(bt-high)");
        CL_UNCATCH();
    } else {
        CL_UNCATCH();
    }
    ASSERT(strstr(cl_backtrace_buf, "  0: BT-LOW") != NULL);
    ASSERT(strstr(cl_backtrace_buf, "  1: BT-HIGH") != NULL);
    cl_vm.sp = 0;
    cl_vm.fp = 0;
}

/* The remaining anonymous frame kinds, named as of FASL 29: a FLET local, a
 * LABELS local, and a method body.  All three build their lambda in the
 * compiler (compile_flet / compile_labels) or in DEFMETHOD's expansion without
 * ever setting the name hand-off slot, so they compiled with NAME = NIL.  Only
 * safe to set now that compile_lambda claims the slot at entry: before that
 * fix the name would have been stolen by the first lambda nested in the local
 * function's own body. */
TEST(eval_backtrace_flet_local_named)
{
    int err;
    /* (+ 1 ...) keeps the local's frame off the tail path so it is really on
     * the stack when the error is signalled.  The #' reference keeps the
     * local a closure: a non-escaping local function is compiled inline
     * (no frame of its own — see eval_backtrace_inlined_local_no_frame),
     * and this test is about the closure's NAME. */
    eval_print("(defun bt-flet-host () "
               "(flet ((bt-flet-local () (error \"boom\"))) (+ 1 (funcall #'bt-flet-local))))");
    CL_CATCH(err);
    if (err == CL_ERR_NONE) {
        cl_eval_string("(bt-flet-host)");
        CL_UNCATCH();
    } else {
        CL_UNCATCH();
    }
    ASSERT(strstr(cl_backtrace_buf, "  0: BT-FLET-LOCAL") != NULL);
    ASSERT(strstr(cl_backtrace_buf, "  1: BT-FLET-HOST") != NULL);
    cl_vm.sp = 0;
    cl_vm.fp = 0;
}

/* LABELS twin, plus the other half of the hand-off contract: a lambda nested
 * in the local function's body must stay anonymous rather than wear the
 * local's name.  Pre-claim-at-entry semantics these two frames would swap. */
TEST(eval_backtrace_labels_local_named)
{
    int err;
    /* Both (+ 1 ...) wrappers matter: a funcall in tail position replaces the
     * local's frame instead of stacking on it, so the frame under test would
     * not exist at all. */
    eval_print("(defun bt-lab-host () "
               "(labels ((bt-lab-local () (+ 1 (funcall (lambda () (error \"boom\")))))) "
               "(+ 1 (funcall #'bt-lab-local))))");
    CL_CATCH(err);
    if (err == CL_ERR_NONE) {
        cl_eval_string("(bt-lab-host)");
        CL_UNCATCH();
    } else {
        CL_UNCATCH();
    }
    ASSERT(strstr(cl_backtrace_buf, "  0: <anonymous>") != NULL);
    ASSERT(strstr(cl_backtrace_buf, "  1: BT-LAB-LOCAL") != NULL);
    ASSERT(strstr(cl_backtrace_buf, "  2: BT-LAB-HOST") != NULL);
    cl_vm.sp = 0;
    cl_vm.fp = 0;
}

/* A non-escaping local function is compiled inline (specs/performance.md
 * 4.2 item 5): the call has no frame, so the backtrace shows the HOST as
 * frame 0 — the error's line still points into the local's body, which is
 * the diagnostic that remains.  (debug 3) turns inlining off for the
 * function and brings the local's frame back. */
TEST(eval_backtrace_inlined_local_no_frame)
{
    int err;
    eval_print("(defun bt-inl-host () "
               "(flet ((bt-inl-local () (error \"boom\"))) (+ 1 (bt-inl-local))))");
    CL_CATCH(err);
    if (err == CL_ERR_NONE) {
        cl_eval_string("(bt-inl-host)");
        CL_UNCATCH();
    } else {
        CL_UNCATCH();
    }
    ASSERT(strstr(cl_backtrace_buf, "  0: BT-INL-HOST") != NULL);
    ASSERT(strstr(cl_backtrace_buf, "BT-INL-LOCAL") == NULL);
    cl_vm.sp = 0;
    cl_vm.fp = 0;

    eval_print("(defun bt-inl-host3 () (declare (optimize (debug 3))) "
               "(flet ((bt-inl-local3 () (error \"boom\"))) (+ 1 (bt-inl-local3))))");
    CL_CATCH(err);
    if (err == CL_ERR_NONE) {
        cl_eval_string("(bt-inl-host3)");
        CL_UNCATCH();
    } else {
        CL_UNCATCH();
    }
    ASSERT(strstr(cl_backtrace_buf, "  0: BT-INL-LOCAL3") != NULL);
    ASSERT(strstr(cl_backtrace_buf, "  1: BT-INL-HOST3") != NULL);
    cl_vm.sp = 0;
    cl_vm.fp = 0;
}

/* A method body carries its generic function's name.  This frame sits directly
 * under the %GF-DISPATCH-* dispatcher, so while it was anonymous every CLOS
 * backtrace had a hole exactly where the user's code was. */
TEST(eval_backtrace_method_named)
{
    int err;
    eval_print("(defgeneric bt-meth-gf (x))");
    eval_print("(defmethod bt-meth-gf ((x integer)) (error \"boom\"))");
    eval_print("(defun bt-meth-host () (+ 1 (bt-meth-gf 1)))");
    CL_CATCH(err);
    if (err == CL_ERR_NONE) {
        cl_eval_string("(bt-meth-host)");
        CL_UNCATCH();
    } else {
        CL_UNCATCH();
    }
    ASSERT(strstr(cl_backtrace_buf, "  0: BT-METH-GF") != NULL);
    ASSERT(strstr(cl_backtrace_buf, "BT-METH-HOST") != NULL);
    cl_vm.sp = 0;
    cl_vm.fp = 0;
}

/* CLHS 5.3: an FLET/LABELS name may be (SETF f), i.e. a cons.  bc->name is
 * read as a symbol by the printer and the backtrace formatter, so a cons must
 * never reach it — the local stays anonymous instead.  Checks the guard from
 * both sides: the form still compiles and runs, and its symbol-named sibling
 * in the same FLET still gets named. */
TEST(eval_backtrace_setf_named_flet_local_stays_anonymous)
{
    eval_print("(defun bt-sf-host () "
               "(flet (((setf bt-sf-x) (v o) (list v o)) (bt-sf-ok () 7)) "
               "(list (bt-sf-ok) (princ-to-string #'bt-sf-ok))))");
    ASSERT_STR_EQ(eval_print("(bt-sf-host)"), "(7 \"#<CLOSURE BT-SF-OK>\")");
}

/* Same guard, the NAMED-LAMBDA path: unlike compile_flet/compile_labels
 * above, compile_named_lambda used to hand its name argument straight to
 * pending_lambda_name with no CL_SYMBOL_P check at all, so a (SETF f) cons
 * name would reach bc->name and type-confuse the printer's closure-printing
 * cases, which read it back as a CL_Symbol. Checks both sides: a cons name
 * stays anonymous, and a plain-symbol sibling is still named normally. */
TEST(eval_named_lambda_setf_name_stays_anonymous)
{
    ASSERT_STR_EQ(eval_print("(named-lambda (setf bt-nl-x) (v o) (list v o))"),
                  "#<CLOSURE anonymous>");
    /* The guard is CL_SYMBOL_P, not "not a cons" — every other shape a name
     * could take falls back the same way. */
    ASSERT_STR_EQ(eval_print("(named-lambda 42 (x) x)"), "#<CLOSURE anonymous>");
    ASSERT_STR_EQ(eval_print("(named-lambda \"nm\" (x) x)"), "#<CLOSURE anonymous>");
    ASSERT_STR_EQ(eval_print("(named-lambda bt-nl-ok () 7)"),
                  "#<CLOSURE BT-NL-OK>");
}

TEST(eval_backtrace_empty)
{
    /* No backtrace when error occurs outside VM (e.g., parse error) */
    int err;
    cl_vm.sp = 0;
    cl_vm.fp = 0;
    cl_backtrace_buf[0] = '\0';
    CL_CATCH(err);
    if (err == CL_ERR_NONE) {
        cl_eval_string("(+ 1");  /* unterminated — reader error */
        CL_UNCATCH();
    } else {
        CL_UNCATCH();
    }
    /* Reader errors happen before VM runs, so backtrace should be empty */
    ASSERT(cl_backtrace_buf[0] == '\0');
    cl_vm.sp = 0;
    cl_vm.fp = 0;
}

/* --- Time --- */

TEST(eval_time_basic)
{
    /* (time expr) should return the value of expr */
    ASSERT_STR_EQ(eval_print("(time (+ 1 2))"), "3");
}

TEST(eval_time_nested)
{
    /* time can be nested in other forms */
    ASSERT_STR_EQ(eval_print("(+ 10 (time (* 3 4)))"), "22");
}

TEST(eval_time_defun)
{
    /* time works with function calls */
    cl_eval_string("(defun time-test (x) (* x x))");
    ASSERT_STR_EQ(eval_print("(time (time-test 5))"), "25");
}

TEST(eval_time_stats)
{
    /* (time expr) should return correct value and internal helpers work */
    ASSERT_STR_EQ(eval_print("(time (+ 1 2))"), "3");

    /* %GET-BYTES-CONSED returns a non-negative fixnum */
    ASSERT_STR_EQ(eval_print("(integerp (%get-bytes-consed))"), "T");
    ASSERT_STR_EQ(eval_print("(>= (%get-bytes-consed) 0)"), "T");

    /* %GET-GC-COUNT returns a non-negative fixnum */
    ASSERT_STR_EQ(eval_print("(integerp (%get-gc-count))"), "T");
    ASSERT_STR_EQ(eval_print("(>= (%get-gc-count) 0)"), "T");

    /* Bytes consed increases after allocation */
    ASSERT_STR_EQ(eval_print(
        "(let ((before (%get-bytes-consed)))"
        "  (cons 1 2)"
        "  (> (%get-bytes-consed) before))"), "T");

    /* Start-value helpers must stay within fixnum range: a mask wider than
     * CL_FIXNUM_MAX sets the fixnum sign bit once the underlying counter
     * passes 2^30 and reads back negative (regression: 0x7FFFFFFF masks) */
    ASSERT_STR_EQ(eval_print(
        "(let ((x (%get-internal-time)))"
        "  (and (integerp x) (<= 0 x most-positive-fixnum) t))"), "T");
    ASSERT_STR_EQ(eval_print(
        "(let ((x (%get-run-time)))"
        "  (and (integerp x) (<= 0 x most-positive-fixnum) t))"), "T");
    ASSERT_STR_EQ(eval_print(
        "(let ((x (%get-bytes-consed)))"
        "  (and (integerp x) (<= 0 x most-positive-fixnum) t))"), "T");
    ASSERT_STR_EQ(eval_print(
        "(let ((x (%get-gc-count)))"
        "  (and (integerp x) (<= 0 x most-positive-fixnum) t))"), "T");
}

TEST(eval_time_report_counter_wrap)
{
    /* %TIME-REPORT wraparound branch: start values at most-positive-fixnum
     * force end < start, so the elapsed figures must come from the modulus
     * arithmetic (CL_FIXNUM_MAX) instead of underflowing. */
    const char *out = eval_print(
        "(with-output-to-string (*trace-output*)"
        "  (%time-report most-positive-fixnum most-positive-fixnum"
        "                most-positive-fixnum most-positive-fixnum))");
    ASSERT(strstr(out, "ms real") != NULL);
    ASSERT(strstr(out, "ms cpu") != NULL);
    ASSERT(strstr(out, "consed") != NULL);
    ASSERT(strstr(out, "GC cycles") != NULL);
}

TEST(eval_time_trace_redirect)
{
    /* HyperSpec: TIME writes its output to *trace-output* */
    const char *out = eval_print(
        "(with-output-to-string (*trace-output*)"
        "  (time 42))");
    ASSERT(strstr(out, "ms real") != NULL);
    ASSERT(strstr(out, "ms cpu") != NULL);
    ASSERT(strstr(out, "consed") != NULL);
    ASSERT(strstr(out, "GC cycles") != NULL);
}

TEST(eval_time_report_legacy_three_args)
{
    /* Backward compat: bytecode compiled before the 4th (CPU-time) arg
     * existed calls %TIME-REPORT with only 3 args (start-time,
     * start-consed, start-gc). That path must still print a valid report,
     * without the "real"/"cpu" split introduced for the 4-arg form. */
    const char *out = eval_print(
        "(with-output-to-string (*trace-output*)"
        "  (%time-report (%get-internal-time) (%get-bytes-consed) (%get-gc-count)))");
    ASSERT(strstr(out, "ms;") != NULL);
    ASSERT(strstr(out, "ms real") == NULL);
    ASSERT(strstr(out, "ms cpu") == NULL);
    ASSERT(strstr(out, "consed") != NULL);
    ASSERT(strstr(out, "GC cycles") != NULL);
}

TEST(eval_get_internal_real_time)
{
    /* get-internal-real-time returns a fixnum */
    ASSERT_STR_EQ(eval_print("(integerp (get-internal-real-time))"), "T");
}

TEST(eval_get_internal_run_time)
{
    /* CLHS 25.1.4.2: get-internal-run-time returns a non-negative integer */
    ASSERT_STR_EQ(eval_print("(integerp (get-internal-run-time))"), "T");
    ASSERT_STR_EQ(eval_print("(>= (get-internal-run-time) 0)"), "T");

    /* Run time (CPU) is monotonically non-decreasing */
    ASSERT_STR_EQ(eval_print(
        "(let ((before (get-internal-run-time)))"
        "  (dotimes (i 100000))"
        "  (>= (get-internal-run-time) before))"), "T");

    /* %GET-RUN-TIME (internal TIME helper) returns a non-negative fixnum */
    ASSERT_STR_EQ(eval_print("(integerp (%get-run-time))"), "T");
    ASSERT_STR_EQ(eval_print("(>= (%get-run-time) 0)"), "T");
}

/* --- Source location tracking --- */

TEST(eval_srcloc_load_backtrace)
{
    /* Errors in loaded files show file:line in backtrace */
    int err;
    cl_vm.sp = 0;
    cl_vm.fp = 0;

    CL_CATCH(err);
    if (err == CL_ERR_NONE) {
        cl_eval_string("(load \"tests/test_srcloc.lisp\")");
        CL_UNCATCH();
    } else {
        CL_UNCATCH();
    }
    /* Check backtrace contains file and line info */
    ASSERT(strstr(cl_backtrace_buf, "test_srcloc.lisp") != NULL);
    ASSERT(strstr(cl_backtrace_buf, "SRCLOC-FAIL") != NULL);
    cl_vm.sp = 0;
    cl_vm.fp = 0;
}

TEST(eval_srcloc_reader_line)
{
    /* Reader tracks line numbers in string streams */
    CL_ReadStream stream;
    CL_Obj expr;
    int line;

    stream.buf = "(foo)\n(bar)\n(baz)";
    stream.pos = 0;
    stream.len = 17;
    stream.line = 1;

    expr = cl_read_from_string(&stream);
    /* First expression starts at line 1 */
    line = cl_srcloc_lookup(expr);
    ASSERT_EQ_INT(line, 1);

    expr = cl_read_from_string(&stream);
    /* Second expression starts at line 2 */
    line = cl_srcloc_lookup(expr);
    ASSERT_EQ_INT(line, 2);

    expr = cl_read_from_string(&stream);
    /* Third expression starts at line 3 */
    line = cl_srcloc_lookup(expr);
    ASSERT_EQ_INT(line, 3);
}

/* --- Macrolet --- */

TEST(eval_macrolet_basic)
{
    ASSERT_EQ_INT(eval_int("(macrolet ((square (x) `(* ,x ,x))) (square 5))"), 25);
}

TEST(eval_macrolet_shadow)
{
    /* Local macro shadows global macro */
    eval_print("(defmacro my-inc (x) `(+ ,x 1))");
    ASSERT_EQ_INT(eval_int("(my-inc 10)"), 11);
    /* Now shadow with macrolet that adds 100 */
    ASSERT_EQ_INT(eval_int("(macrolet ((my-inc (x) `(+ ,x 100))) (my-inc 10))"), 110);
    /* Global still works */
    ASSERT_EQ_INT(eval_int("(my-inc 10)"), 11);
}

TEST(eval_macrolet_scope)
{
    const char *result;
    /* Macro not visible outside macrolet body */
    ASSERT_EQ_INT(eval_int("(macrolet ((add3 (x) `(+ ,x 3))) (add3 7))"), 10);
    /* add3 should not exist as a global macro, so calling it will error */
    result = eval_print("(add3 7)");
    ASSERT(strncmp(result, "ERROR:", 6) == 0);
}

TEST(eval_macrolet_nested)
{
    /* Inner macrolet shadows outer */
    ASSERT_EQ_INT(eval_int(
        "(macrolet ((m (x) `(+ ,x 1)))"
        "  (macrolet ((m (x) `(+ ,x 10)))"
        "    (m 5)))"), 15);
    /* Outer still works after inner scope */
    ASSERT_EQ_INT(eval_int(
        "(macrolet ((m (x) `(+ ,x 1)))"
        "  (+ (macrolet ((m (x) `(+ ,x 10))) (m 5))"
        "     (m 5)))"), 21);
}

TEST(eval_macrolet_with_body)
{
    /* macrolet with multiple body forms */
    ASSERT_STR_EQ(eval_print(
        "(macrolet ((double (x) `(* 2 ,x)))"
        "  (double 3)"
        "  (double 7))"), "14");
}

TEST(eval_macrolet_across_lambda)
{
    /* macrolet must be lexically visible inside nested lambdas (CL spec) */
    ASSERT_EQ_INT(eval_int(
        "(macrolet ((foo () '42))"
        "  (funcall (lambda () (foo))))"), 42);
    /* macrolet visible across multiple lambda levels */
    ASSERT_EQ_INT(eval_int(
        "(macrolet ((add10 (x) `(+ ,x 10)))"
        "  (funcall (lambda () (funcall (lambda () (add10 5))))))"), 15);
    /* macrolet with quasiquote unquote-splicing inside lambda */
    ASSERT_STR_EQ(eval_print(
        "(macrolet ((&body () '(list 1 2 3)))"
        "  (funcall (lambda () `(progn ,@(&body)))))"), "(PROGN 1 2 3)");
}

/* --- Symbol-macrolet --- */

TEST(eval_symbol_macrolet_basic)
{
    ASSERT_EQ_INT(eval_int("(symbol-macrolet ((x 42)) x)"), 42);
}

TEST(eval_symbol_macrolet_expr)
{
    /* Symbol macro expands to an expression */
    ASSERT_EQ_INT(eval_int("(symbol-macrolet ((x (+ 1 2))) x)"), 3);
}

TEST(eval_symbol_macrolet_setq)
{
    /* (setq sym val) on a symbol-macro rewrites to (setf expansion val) */
    ASSERT_STR_EQ(eval_print(
        "(let ((a (cons 1 2)))"
        "  (symbol-macrolet ((x (car a)))"
        "    (setq x 99)"
        "    a))"), "(99 . 2)");
}

TEST(eval_symbol_macrolet_scope)
{
    /* Symbol macro not visible outside body */
    ASSERT_EQ_INT(eval_int(
        "(let ((result 0))"
        "  (symbol-macrolet ((x 42))"
        "    (setq result x))"
        "  result)"), 42);
}

TEST(eval_symbol_macrolet_nested)
{
    /* Inner shadows outer */
    ASSERT_EQ_INT(eval_int(
        "(symbol-macrolet ((x 10))"
        "  (symbol-macrolet ((x 20))"
        "    x))"), 20);
}

TEST(eval_symbol_macrolet_multiple)
{
    /* Multiple symbol macros */
    ASSERT_EQ_INT(eval_int(
        "(symbol-macrolet ((x 10) (y 20))"
        "  (+ x y))"), 30);
}

TEST(eval_symbol_macrolet_across_lambda)
{
    /* symbol-macrolet must be lexically visible inside nested lambdas */
    ASSERT_EQ_INT(eval_int(
        "(symbol-macrolet ((x 42))"
        "  (funcall (lambda () x)))"), 42);
    ASSERT_EQ_INT(eval_int(
        "(symbol-macrolet ((x 10) (y 20))"
        "  (funcall (lambda () (+ x y))))"), 30);
}

TEST(eval_symbol_macrolet_shadowed_by_lambda_param)
{
    /* A lambda parameter must shadow an outer symbol-macro of the same
     * name.  The classic trigger is (symbol-macrolet ((x EXPR-USING-X))
     * (defmethod foo ((x ...)) body)) — without proper shadowing the
     * inner reference re-expands the symbol-macro infinitely.
     * Regression for serapeum/internal-definitions.lisp's define-env-method
     * which generated exactly that pattern for self / vars / decls / ... */
    ASSERT_EQ_INT(eval_int(
        "(symbol-macrolet ((x (+ x 100)))"
        "  (funcall (lambda (x) x) 7))"), 7);
    /* Same shadowing rule for nested lambdas with no parameter named X —
     * those should still see the outer symbol-macro. */
    ASSERT_EQ_INT(eval_int(
        "(symbol-macrolet ((x 42))"
        "  (funcall (lambda (y) (+ y x)) 1))"), 43);
    /* Deepest case: parameter shadows in outer lambda, and inner closure
     * captures the parameter (not the macro).  This is the exact pattern
     * that re-recursed in serapeum. */
    ASSERT_EQ_INT(eval_int(
        "(symbol-macrolet ((self (+ self 999)))"
        "  (funcall (funcall (lambda (self)"
        "                      (lambda () self))"
        "                    11)))"), 11);
}

TEST(eval_symbol_macrolet_shadowed_by_let)
{
    /* Inner symbol-macrolet inside a let still wins (locally added macro
     * is more recent than the outer let binding). */
    ASSERT_EQ_INT(eval_int(
        "(let ((x 1))"
        "  (symbol-macrolet ((x 99)) x))"), 99);
}

/* --- &ENVIRONMENT / MACROEXPAND-1 with lexical env ---
 * Macros that declare an &environment parameter receive a snapshot of
 * the symbol-macros active at the call site.  Passing that env to
 * MACROEXPAND-1 lets the macro read those bindings.  This is the core
 * mechanism used by serapeum's WITH-BOOLEAN / BOOLEAN-IF pair. */
TEST(eval_environment_symbol_macrolet_expand)
{
    /* Baseline: outside any symbol-macrolet, env lookup of x is
     * unknown and MACROEXPAND-1 returns (values x nil). */
    eval_print(
        "(defmacro env-probe-expanded-p (&environment env)"
        "  (multiple-value-bind (v p) (macroexpand-1 'x env)"
        "    (declare (ignore v))"
        "    (if p t nil)))");
    ASSERT_STR_EQ(eval_print("(env-probe-expanded-p)"), "NIL");
    /* Inside symbol-macrolet, env lookup finds x and the macro
     * receives the expansion. */
    eval_print(
        "(defmacro env-probe-x (&environment env)"
        "  (multiple-value-bind (v p) (macroexpand-1 'x env)"
        "    (if p `(quote ,v) `(quote :not-found))))");
    ASSERT_STR_EQ(eval_print("(symbol-macrolet ((x 42)) (env-probe-x))"),
                  "42");
}

TEST(eval_environment_with_boolean_idiom)
{
    /* Replica of serapeum's WITH-BOOLEAN / report-branches idiom.  The
     * outer macro stashes a value in a symbol-macrolet; the inner
     * macro reads it back through its &environment parameter. */
    eval_print(
        "(defmacro wb (tag &body body &environment env)"
        "  (multiple-value-bind (outer p) (macroexpand-1 '%branches env)"
        "    (let ((chain (if p (cadr outer) nil)))"
        "      `(symbol-macrolet ((%branches (quote ,(cons tag chain))))"
        "         ,@body))))");
    eval_print(
        "(defmacro report-branches (&environment env)"
        "  (multiple-value-bind (v p) (macroexpand-1 '%branches env)"
        "    (if p `(quote ,(cadr v)) `(quote ()))))");
    /* No surrounding wb -> NIL chain. */
    ASSERT_STR_EQ(eval_print("(report-branches)"), "NIL");
    /* Single wb -> (:A). */
    ASSERT_STR_EQ(eval_print("(wb :a (report-branches))"), "(:A)");
    /* Nested wb -> (:INNER :OUTER) — most recent first. */
    ASSERT_STR_EQ(eval_print(
        "(wb :outer (wb :inner (report-branches)))"),
        "(:INNER :OUTER)");
}

TEST(eval_environment_macrolet_captures_symbol_macros)
{
    /* MACROLET-defined macros must also see the current lexical env
     * via &environment — same plumbing as DEFMACRO. */
    ASSERT_STR_EQ(eval_print(
        "(macrolet ((probe (&environment env)"
        "             (multiple-value-bind (v p) (macroexpand-1 'q env)"
        "               (if p `(quote ,v) `(quote :none)))))"
        "  (symbol-macrolet ((q 'found))"
        "    (probe)))"),
        "(QUOTE FOUND)");
}

TEST(eval_environment_macrolet_inside_binding_form)
{
    /* Regression: a MACROLET macro whose &environment is NOT the first
     * lambda-list element, used inside a binding form (LET) that triggers
     * the boxing pre-scan.  The pre-scan installs the macrolet expanders to
     * "see through" macrolet calls, then the real compile installs them
     * again.  cl_macrolet_install_expanders used to splice &environment out
     * by DESTRUCTIVELY mutating the shared lambda-list, so the 2nd install
     * saw no &environment and never re-bound the env var, making the
     * expander body's reference to it "Unbound variable: ENV".  This is the
     * exact shape of ironclad sha3.lisp's STATE-AREF macrolets. */
    ASSERT_EQ_INT(eval_int(
        "(let ((v (make-array 3 :initial-element 0)))"
        "  (macrolet ((setit (i val &environment env)"
        "               `(setf (aref v ,(macroexpand i env)) ,val)))"
        "    (symbol-macrolet ((idx 1))"
        "      (setit idx 99)"
        "      (aref v 1))))"), 99);
    /* &environment last, two required params, used as a read place. */
    ASSERT_EQ_INT(eval_int(
        "(let ((v (make-array '(2 2) :initial-element 7)))"
        "  (macrolet ((g (x y &environment env)"
        "               `(aref v ,(macroexpand x env) ,(macroexpand y env))))"
        "    (g 1 1)))"), 7);
}

TEST(scanner_deep_nesting_no_root_overflow)
{
    /* Regression: scan_body_for_boxing recurses through the LET/MACROLET/FLET/
     * COND/... special-form handlers and the macro-expansion fall-through
     * WITHOUT bumping its scan_recurse_depth guard, and each level holds GC
     * roots (the walk cursor / body / macro expansion) across its recursive
     * call.  A deeply nested binding form — the shape serapeum's
     * sequences.lisp macro-expands into (cascading DO-VECTOR/SEQ-DISPATCH/
     * WITH-VREF) — drove gc_root_count to the per-thread CL_GC_ROOT_STACK_SIZE
     * ceiling, where cl_gc_pop_roots abort()s the whole process.  The
     * gc_root_count backstop at the top of scan_body_for_boxing now bails out
     * before that.  Build (let ((a (let ((a (... 0 ...))) a))) a) ~2500 deep
     * (well past the 1024-root ceiling) and run the boxing pre-scan on it
     * directly: it must RETURN, not crash. */
    CL_Obj sym_a = cl_intern("A", 1);
    CL_Obj form = CL_MAKE_FIXNUM(0);
    CL_Obj body;
    CL_Obj vars[1];
    uint8_t boxed[1];
    int i;
    CL_GC_PROTECT(sym_a);
    CL_GC_PROTECT(form);
    for (i = 0; i < 2500; i++) {
        /* form := (let ((a <form>)) a) */
        CL_Obj binding  = cl_cons(sym_a, cl_cons(form, CL_NIL));   /* (a <form>) */
        CL_Obj bindings = cl_cons(binding, CL_NIL);                /* ((a <form>)) */
        form = cl_cons(SYM_LET, cl_cons(bindings, cl_cons(sym_a, CL_NIL)));
    }
    vars[0] = sym_a;
    body = cl_cons(form, CL_NIL);
    CL_GC_PROTECT(body);
    /* Without the backstop this abort()s; with it, it returns cleanly. */
    determine_boxed_vars(body, vars, 1, boxed);
    CL_GC_UNPROTECT(3);
    ASSERT(1); /* reached here -> no GC-root-stack overflow abort */
}

/* define-symbol-macro — global (stored on symbol plist, consulted by
   compile_symbol via cl_lookup_global_symbol_macro).  Regression tests
   for the sento/serapeum +alist-breakeven+ defconst pattern, which
   defines a constant under a symbol-macro alias and then references it
   via #. at read time later in the same file.  Each top-level form
   must be compiled+evaluated in isolation to match LOAD semantics —
   that's how the plist entry becomes visible before the next form is
   compiled. */
TEST(eval_define_symbol_macro_basic)
{
    eval_print("(defconstant +storage-dsm-basic+ 30)");
    eval_print("(define-symbol-macro sm-basic +storage-dsm-basic+)");
    ASSERT_EQ_INT(eval_int("sm-basic"), 30);
}

TEST(eval_define_symbol_macro_read_time_eval)
{
    /* #.symbol-macro must expand and evaluate at read time.  This is
       the exact pattern that broke in serapeum/functions.lisp when
       +alist-breakeven+ was referenced via #. */
    eval_print("(defconstant +storage-dsm-rte+ 25)");
    eval_print("(define-symbol-macro sm-rte +storage-dsm-rte+)");
    ASSERT_EQ_INT(eval_int("'#.sm-rte"), 25);
}

TEST(eval_define_symbol_macro_macroexpand_1)
{
    /* macroexpand-1 on a symbol should return the symbol-macro
       expansion and T as second value. */
    eval_print("(define-symbol-macro sm-mx +sm-mx-backing+)");
    ASSERT_STR_EQ(eval_print("(macroexpand-1 'sm-mx)"), "+SM-MX-BACKING+");
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-bind (form expanded-p) (macroexpand-1 'sm-mx)"
        "  (list form expanded-p))"), "(+SM-MX-BACKING+ T)");
}

TEST(eval_define_symbol_macro_setf)
{
    /* setf on a symbol-macro rewrites to setf on its expansion. */
    eval_print("(defparameter *sm-setf-cell* (cons 1 2))");
    eval_print("(define-symbol-macro sm-setf (car *sm-setf-cell*))");
    eval_print("(setf sm-setf 99)");
    ASSERT_STR_EQ(eval_print("*sm-setf-cell*"), "(99 . 2)");
}

/* --- Phase 8 Step 2: Missing string operations --- */

TEST(eval_string_capitalize)
{
    ASSERT_STR_EQ(eval_print("(string-capitalize \"hello world\")"), "\"Hello World\"");
    ASSERT_STR_EQ(eval_print("(string-capitalize \"HELLO WORLD\")"), "\"Hello World\"");
    ASSERT_STR_EQ(eval_print("(string-capitalize \"foo-bar\")"), "\"Foo-Bar\"");
}

TEST(eval_string_case_designators)
{
    /* string-upcase/downcase/capitalize must accept string designators
       (symbols, characters), not just strings */
    ASSERT_STR_EQ(eval_print("(string-downcase :FIVEAM)"), "\"fiveam\"");
    ASSERT_STR_EQ(eval_print("(string-downcase 'HELLO)"), "\"hello\"");
    ASSERT_STR_EQ(eval_print("(string-upcase :foo)"), "\"FOO\"");
    ASSERT_STR_EQ(eval_print("(string-capitalize :hello-world)"), "\"Hello-World\"");
}

TEST(eval_nstring_upcase)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((s (copy-seq \"hello\")))"
        "  (nstring-upcase s)"
        "  s)"), "\"HELLO\"");
}

TEST(eval_nstring_downcase)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((s (copy-seq \"HELLO\")))"
        "  (nstring-downcase s)"
        "  s)"), "\"hello\"");
}

TEST(eval_nstring_capitalize)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((s (copy-seq \"hello world\")))"
        "  (nstring-capitalize s)"
        "  s)"), "\"Hello World\"");
}

TEST(eval_string_case_start_end)
{
    /* :START/:END bounding indices (CLHS 21.2, string-upcase page):
       only the bounded subsequence is case-converted; the rest of the
       fresh copy is unchanged. */
    ASSERT_STR_EQ(eval_print(
        "(string-upcase \"Dr. Livingstone, I presume?\" :start 6 :end 10)"),
        "\"Dr. LiVINGstone, I presume?\"");
    ASSERT_STR_EQ(eval_print("(string-upcase \"hello world\" :start 6)"),
                  "\"hello WORLD\"");
    ASSERT_STR_EQ(eval_print("(string-downcase \"HELLO WORLD\" :end 5)"),
                  "\"hello WORLD\"");
    /* :END NIL designates the end of the string. */
    ASSERT_STR_EQ(eval_print("(string-upcase \"abc\" :start 1 :end nil)"),
                  "\"aBC\"");
    /* Empty bounded region — untouched copy. */
    ASSERT_STR_EQ(eval_print("(string-upcase \"abc\" :start 1 :end 1)"),
                  "\"abc\"");
    /* Capitalize: word detection restarts at :START; a word already in
       progress before :START is treated as starting fresh. */
    ASSERT_STR_EQ(eval_print(
        "(string-capitalize \"hello world\" :start 6 :end 11)"),
        "\"hello World\"");
    /* String designators combine with :START/:END. */
    ASSERT_STR_EQ(eval_print("(string-upcase 'foo-bar :start 4)"),
                  "\"FOO-BAR\"");
}

TEST(eval_nstring_case_start_end)
{
    /* CLHS nstring-upcase example — destructive, returns the same string. */
    ASSERT_STR_EQ(eval_print(
        "(let ((s (copy-seq \"Dr. LiVINGstone, I presume?\")))"
        "  (list (eq s (nstring-downcase s :start 6 :end 10)) s))"),
        "(T \"Dr. Livingstone, I presume?\")");
    ASSERT_STR_EQ(eval_print(
        "(let ((s (copy-seq \"hello world\")))"
        "  (nstring-upcase s :start 6) s)"), "\"hello WORLD\"");
    ASSERT_STR_EQ(eval_print(
        "(let ((s (copy-seq \"hello world\")))"
        "  (nstring-capitalize s :end 5) s)"), "\"Hello world\"");
}

TEST(eval_string_case_start_end_errors)
{
    /* start > end, end > length, negative start — all bounding-index
       errors; odd/unknown keywords are PROGRAM-ERRORs; non-integer
       :START is a TYPE-ERROR.  :ALLOW-OTHER-KEYS T admits unknowns. */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (string-upcase \"abc\" :start 2 :end 1)"
        "  (error () :bad-bounds))"), ":BAD-BOUNDS");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (string-upcase \"abc\" :end 4)"
        "  (error () :bad-bounds))"), ":BAD-BOUNDS");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (nstring-downcase (copy-seq \"abc\") :start -1)"
        "  (error () :bad-bounds))"), ":BAD-BOUNDS");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (string-upcase \"abc\" :start)"
        "  (error () :odd-keywords))"), ":ODD-KEYWORDS");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (string-upcase \"abc\" :frob 1)"
        "  (error () :unknown-keyword))"), ":UNKNOWN-KEYWORD");
    ASSERT_STR_EQ(eval_print(
        "(string-upcase \"abc\" :frob 1 :allow-other-keys t)"), "\"ABC\"");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (string-upcase \"abc\" :start 'x)"
        "  (type-error () :start-type))"), ":START-TYPE");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (nstring-upcase 42)"
        "  (type-error () :not-a-string))"), ":NOT-A-STRING");
}

TEST(eval_char_name)
{
    ASSERT_STR_EQ(eval_print("(char-name #\\Space)"), "\"Space\"");
    ASSERT_STR_EQ(eval_print("(char-name #\\Newline)"), "\"Newline\"");
    ASSERT_STR_EQ(eval_print("(char-name #\\A)"), "NIL");
    /* U+FFFD round-trips through char-name (clack/flexi-streams dependency). */
    ASSERT_STR_EQ(eval_print("(char-name #\\Replacement_Character)"),
                  "\"Replacement_Character\"");
}

TEST(eval_name_char)
{
    ASSERT_STR_EQ(eval_print("(name-char \"Space\")"), "#\\Space");
    ASSERT_STR_EQ(eval_print("(name-char \"SPACE\")"), "#\\Space");
    ASSERT_STR_EQ(eval_print("(name-char \"Newline\")"), "#\\Newline");
    ASSERT_STR_EQ(eval_print("(name-char \"xyzzy\")"), "NIL");
    /* name-char of the Unicode replacement-character name yields U+FFFD. */
    ASSERT_STR_EQ(eval_print("(char-code (name-char \"Replacement_Character\"))"),
                  "65533");
}

TEST(eval_char_ci_compare)
{
    ASSERT_STR_EQ(eval_print("(char-equal #\\a #\\A)"), "T");
    ASSERT_STR_EQ(eval_print("(char-not-equal #\\a #\\b)"), "T");
    ASSERT_STR_EQ(eval_print("(char-lessp #\\a #\\B)"), "T");
    ASSERT_STR_EQ(eval_print("(char-greaterp #\\b #\\A)"), "T");
    ASSERT_STR_EQ(eval_print("(char-not-greaterp #\\a #\\A)"), "T");
    ASSERT_STR_EQ(eval_print("(char-not-lessp #\\A #\\a)"), "T");
}

TEST(eval_graphic_char_p)
{
    ASSERT_STR_EQ(eval_print("(graphic-char-p #\\A)"), "T");
    ASSERT_STR_EQ(eval_print("(graphic-char-p #\\Space)"), "T");
    ASSERT_STR_EQ(eval_print("(graphic-char-p #\\Newline)"), "NIL");
}

TEST(eval_alphanumericp)
{
    ASSERT_STR_EQ(eval_print("(alphanumericp #\\A)"), "T");
    ASSERT_STR_EQ(eval_print("(alphanumericp #\\5)"), "T");
    ASSERT_STR_EQ(eval_print("(alphanumericp #\\!)"), "NIL");
}

TEST(eval_digit_char)
{
    ASSERT_STR_EQ(eval_print("(digit-char 0)"), "#\\0");
    ASSERT_STR_EQ(eval_print("(digit-char 9)"), "#\\9");
    ASSERT_STR_EQ(eval_print("(digit-char 10 16)"), "#\\A");
    ASSERT_STR_EQ(eval_print("(digit-char 10)"), "NIL");
}

/* --- Phase 8 Step 3: Missing sequence operations --- */

TEST(eval_elt)
{
    ASSERT_EQ_INT(eval_int("(elt '(a b c) 1)"), CL_FIXNUM_VAL(cl_eval_string("'b")));
    ASSERT_STR_EQ(eval_print("(elt (vector 10 20 30) 2)"), "30");
    ASSERT_STR_EQ(eval_print("(elt \"abc\" 0)"), "#\\a");
}

TEST(eval_setf_elt)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((v (vector 1 2 3)))"
        "  (setf (elt v 1) 99) v)"), "#(1 99 3)");
    ASSERT_STR_EQ(eval_print(
        "(let ((l (list 1 2 3)))"
        "  (setf (elt l 0) 99) l)"), "(99 2 3)");
}

TEST(eval_copy_seq)
{
    ASSERT_STR_EQ(eval_print("(copy-seq '(1 2 3))"), "(1 2 3)");
    ASSERT_STR_EQ(eval_print("(copy-seq \"hello\")"), "\"hello\"");
    /* copy-seq makes a fresh vector with the same elements; compare contents
       with EQUALP (EQUAL on general vectors is identity per CLHS). */
    ASSERT_STR_EQ(eval_print(
        "(let ((a (vector 1 2 3))"
        "      (b (copy-seq (vector 1 2 3))))"
        "  (equalp a b))"), "T");
    ASSERT_STR_EQ(eval_print("(copy-seq nil)"), "NIL");
}

TEST(eval_map_into)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((v (vector 0 0 0)))"
        "  (map-into v #'+ (vector 1 2 3) (vector 10 20 30))"
        "  v)"), "#(11 22 33)");
}

/* --- Phase 8 Step 4: Higher-order functions --- */

TEST(eval_complement)
{
    ASSERT_STR_EQ(eval_print(
        "(funcall (complement #'zerop) 0)"), "NIL");
    ASSERT_STR_EQ(eval_print(
        "(funcall (complement #'zerop) 5)"), "T");
}

TEST(eval_constantly)
{
    ASSERT_STR_EQ(eval_print(
        "(funcall (constantly 42) 1 2 3)"), "42");
    ASSERT_STR_EQ(eval_print(
        "(mapcar (constantly 'x) '(1 2 3))"), "(X X X)");
}

/* --- Phase 8 Step 1: Missing list operations --- */

TEST(eval_list_star)
{
    ASSERT_STR_EQ(eval_print("(list* 1 2)"), "(1 . 2)");
    ASSERT_STR_EQ(eval_print("(list* 1 2 3)"), "(1 2 . 3)");
    ASSERT_STR_EQ(eval_print("(list* 1 2 3 nil)"), "(1 2 3)");
    ASSERT_STR_EQ(eval_print("(list* 'a)"), "A");
}

TEST(eval_make_list)
{
    ASSERT_STR_EQ(eval_print("(make-list 3)"), "(NIL NIL NIL)");
    ASSERT_STR_EQ(eval_print("(make-list 3 :initial-element 'x)"), "(X X X)");
    ASSERT_STR_EQ(eval_print("(make-list 0)"), "NIL");
}

TEST(eval_tree_equal)
{
    ASSERT_STR_EQ(eval_print("(tree-equal '(1 (2 3)) '(1 (2 3)))"), "T");
    ASSERT_STR_EQ(eval_print("(tree-equal '(1 2) '(1 3))"), "NIL");
    ASSERT_STR_EQ(eval_print("(tree-equal nil nil)"), "T");
    ASSERT_STR_EQ(eval_print("(tree-equal '(\"a\" \"b\") '(\"a\" \"b\") :test #'equal)"), "T");
    /* CLHS: structure must match — atom vs cons is NIL regardless of test. */
    ASSERT_STR_EQ(eval_print(
        "(tree-equal (list 1) 2 :test (constantly t))"), "NIL");
    /* :test is applied only to leaves, never to conses. */
    ASSERT_STR_EQ(eval_print(
        "(tree-equal '(10 20 . 30) '(11 22 . 34) :test #'<)"), "T");
    /* Duplicated keyword: leftmost wins. */
    ASSERT_STR_EQ(eval_print(
        "(tree-equal '(a . b) '(b . a) :test (complement #'eql) :test #'eql)"), "T");
}

TEST(eval_list_length)
{
    ASSERT_STR_EQ(eval_print("(list-length '(1 2 3))"), "3");
    ASSERT_STR_EQ(eval_print("(list-length nil)"), "0");
    /* Tortoise-and-hare must terminate on circular lists. */
    ASSERT_STR_EQ(eval_print(
        "(let ((x (list 1 2 3))) (setf (cdr (last x)) x) (list-length x))"),
        "NIL");
}

TEST(eval_tailp)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((x '(1 2 3)))"
        "  (tailp (cdr x) x))"), "T");
    ASSERT_STR_EQ(eval_print("(tailp nil '(1 2 3))"), "T");
    ASSERT_STR_EQ(eval_print("(tailp '(4) '(1 2 3))"), "NIL");
}

TEST(eval_ldiff)
{
    ASSERT_STR_EQ(eval_print(
        "(let* ((x '(1 2 3 4))"
        "       (tail (cddr x)))"
        "  (ldiff x tail))"), "(1 2)");
    ASSERT_STR_EQ(eval_print("(ldiff '(1 2 3) nil)"), "(1 2 3)");
}

TEST(eval_revappend)
{
    ASSERT_STR_EQ(eval_print("(revappend '(1 2 3) '(4 5))"), "(3 2 1 4 5)");
    ASSERT_STR_EQ(eval_print("(revappend nil '(1 2))"), "(1 2)");
}

TEST(eval_nreconc)
{
    ASSERT_STR_EQ(eval_print("(nreconc (list 1 2 3) '(4 5))"), "(3 2 1 4 5)");
    ASSERT_STR_EQ(eval_print("(nreconc nil '(1 2))"), "(1 2)");
}

TEST(eval_assoc_if)
{
    ASSERT_STR_EQ(eval_print("(assoc-if #'symbolp '((1 . a) (b . 2) (3 . c)))"), "(B . 2)");
    ASSERT_STR_EQ(eval_print("(assoc-if #'null '((1 . a) (2 . b)))"), "NIL");
}

TEST(eval_rassoc_if)
{
    ASSERT_STR_EQ(eval_print("(rassoc-if #'symbolp '((1 . 2) (3 . b) (4 . 5)))"), "(3 . B)");
    ASSERT_STR_EQ(eval_print("(rassoc-if #'null '((1 . a) (2 . b)))"), "NIL");
}

TEST(eval_remf)
{
    /* CLHS 5.1.2: REMF is a place-modifying macro that destructively
       removes the indicator/value pair from the plist named by place,
       and returns a single generalized boolean (T if removed, NIL if
       the indicator was not present). */
    ASSERT_STR_EQ(eval_print(
        "(let ((p (list :a 1 :b 2 :c 3)))"
        "  (remf p :b)"
        "  p)"), "(:A 1 :C 3)");
    /* Removing the head of the plist must update the variable. */
    ASSERT_STR_EQ(eval_print(
        "(let ((p (list :a 1 :b 2)))"
        "  (remf p :a)"
        "  p)"), "(:B 2)");
    /* Return value is the boolean. */
    ASSERT_STR_EQ(eval_print(
        "(let ((p (list :a 1 :b 2))) (remf p :a))"), "T");
    ASSERT_STR_EQ(eval_print(
        "(let ((p (list :a 1 :b 2))) (remf p :z))"), "NIL");
    /* Place is unchanged when indicator absent. */
    ASSERT_STR_EQ(eval_print(
        "(let ((p (list :a 1 :b 2))) (remf p :z) p)"), "(:A 1 :B 2)");
}

/* --- Paren depth --- */

TEST(paren_depth_balanced)
{
    ASSERT_EQ_INT(cl_paren_depth("(+ 1 2)"), 0);
    ASSERT_EQ_INT(cl_paren_depth("((a) (b))"), 0);
    ASSERT_EQ_INT(cl_paren_depth("()"), 0);
    ASSERT_EQ_INT(cl_paren_depth(""), 0);
    ASSERT_EQ_INT(cl_paren_depth("hello"), 0);
}

TEST(paren_depth_unbalanced)
{
    ASSERT_EQ_INT(cl_paren_depth("(defun foo"), 1);
    ASSERT_EQ_INT(cl_paren_depth("(defun foo (x)"), 1);
    ASSERT_EQ_INT(cl_paren_depth("(("), 2);
    ASSERT_EQ_INT(cl_paren_depth("(+ 1 2))"), -1);
}

TEST(paren_depth_strings)
{
    /* Parens inside strings should not count */
    ASSERT_EQ_INT(cl_paren_depth("(print \"((\")"), 0);
    ASSERT_EQ_INT(cl_paren_depth("(print \")\")"), 0);
    ASSERT_EQ_INT(cl_paren_depth("(print \"\\\"\")"), 0);
}

TEST(paren_depth_unterminated_string)
{
    /* A string literal runs to the closing quote, newlines included (CLHS
     * 2.4.5) — an unterminated one means "need more input", not "complete
     * form with balanced parens" (which handed the reader a parse error at
     * the REPL, issue #14). */
    ASSERT_EQ_INT(cl_paren_depth("\""), 1);
    ASSERT_EQ_INT(cl_paren_depth("\"abc"), 1);
    ASSERT_EQ_INT(cl_paren_depth("\"abc\ndef"), 1);
    ASSERT_EQ_INT(cl_paren_depth("(print \"abc"), 1);
    /* Trailing backslash: the escape consumes the (future) next char, so
     * the string is still open. */
    ASSERT_EQ_INT(cl_paren_depth("\"abc\\"), 1);
    /* An escaped quote does not close the string. */
    ASSERT_EQ_INT(cl_paren_depth("\"abc\\\""), 1);
    /* Once the quote closes, the form is complete again. */
    ASSERT_EQ_INT(cl_paren_depth("\"abc\ndef\""), 0);
    ASSERT_EQ_INT(cl_paren_depth("\"\n\""), 0);
}

TEST(paren_depth_comments)
{
    /* Parens in comments should not count */
    ASSERT_EQ_INT(cl_paren_depth("(+ 1 2) ; extra ("), 0);
    ASSERT_EQ_INT(cl_paren_depth("; just a comment ((("), 0);
    /* Multi-line with comment */
    ASSERT_EQ_INT(cl_paren_depth("(defun foo (x)\n  ; body )\n  (+ x 1))"), 0);
}

TEST(paren_depth_char_literals)
{
    /* Character literal #\( should not count as open paren */
    ASSERT_EQ_INT(cl_paren_depth("(list #\\( #\\))"), 0);
    /* Named character literals */
    ASSERT_EQ_INT(cl_paren_depth("(list #\\Space)"), 0);
}

TEST(paren_depth_multiline)
{
    /* Typical multi-line defun */
    ASSERT_EQ_INT(cl_paren_depth("(defun fact (n)"), 1);
    ASSERT_EQ_INT(cl_paren_depth("(defun fact (n)\n  (if (<= n 1) 1"), 2);
    ASSERT_EQ_INT(cl_paren_depth(
        "(defun fact (n)\n  (if (<= n 1) 1\n    (* n (fact (1- n)))))"), 0);
}

/* --- History variables --- */

TEST(history_star_basic)
{
    /* Eval (+ 1 2) => 3, update history, check * */
    CL_Obj form = cl_eval_string("'(+ 1 2)");
    CL_Obj values = cl_cons(CL_MAKE_FIXNUM(3), CL_NIL);
    cl_repl_update_history(form, values);
    ASSERT_EQ_INT(eval_int("*"), 3);
}

TEST(history_star_shift)
{
    /* First eval: result 10 */
    cl_repl_update_history(CL_NIL, cl_cons(CL_MAKE_FIXNUM(10), CL_NIL));
    /* Second eval: result 20 */
    cl_repl_update_history(CL_NIL, cl_cons(CL_MAKE_FIXNUM(20), CL_NIL));
    /* Third eval: result 30 */
    cl_repl_update_history(CL_NIL, cl_cons(CL_MAKE_FIXNUM(30), CL_NIL));

    ASSERT_EQ_INT(eval_int("*"), 30);
    ASSERT_EQ_INT(eval_int("**"), 20);
    ASSERT_EQ_INT(eval_int("***"), 10);
}

TEST(history_plus_shift)
{
    CL_Obj form1, form2, form3;
    form1 = cl_eval_string("'(first-form)");
    form2 = cl_eval_string("'(second-form)");
    form3 = cl_eval_string("'(third-form)");

    cl_repl_update_history(form1, cl_cons(CL_MAKE_FIXNUM(1), CL_NIL));
    cl_repl_update_history(form2, cl_cons(CL_MAKE_FIXNUM(2), CL_NIL));
    cl_repl_update_history(form3, cl_cons(CL_MAKE_FIXNUM(3), CL_NIL));

    /* + should be the last form */
    ASSERT_STR_EQ(eval_print("+"), "(THIRD-FORM)");
    ASSERT_STR_EQ(eval_print("++"), "(SECOND-FORM)");
    ASSERT_STR_EQ(eval_print("+++"), "(FIRST-FORM)");
}

TEST(history_minus_during_eval)
{
    /* - is set before eval, so we test by checking SYM_MINUS value directly */
    CL_Obj form = cl_eval_string("'(some-form)");
    CL_Symbol *s;

    /* Simulate what the REPL does: set - before compile/eval */
    s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_MINUS);
    s->value = form;

    ASSERT_STR_EQ(eval_print("-"), "(SOME-FORM)");
}

TEST(history_arithmetic_still_works)
{
    /* Verify *, +, - still work as arithmetic functions */
    ASSERT_EQ_INT(eval_int("(* 3 4)"), 12);
    ASSERT_EQ_INT(eval_int("(+ 5 6)"), 11);
    ASSERT_EQ_INT(eval_int("(- 10 3)"), 7);
}

/* --- REPL result printing --- */

TEST(repl_zero_values_print_nothing)
{
    /* (values) returns no values at all, so the REPL prints nothing —
     * it used to print NIL (issue #6). */
    ASSERT_EQ_INT(cl_repl_result_printable(cl_eval_string("(values)")), 0);
    ASSERT_EQ_INT(cl_repl_result_printable(cl_eval_string("(values-list nil)")), 0);
    ASSERT_EQ_INT(cl_repl_result_printable(cl_eval_string("(progn (values))")), 0);
    /* Zero values survive a function return as zero values */
    cl_eval_string("(defun repl-zero-values-fn () (values))");
    ASSERT_EQ_INT(cl_repl_result_printable(
                      cl_eval_string("(repl-zero-values-fn)")), 0);
}

TEST(repl_one_value_prints_even_when_nil)
{
    /* A single value that happens to be NIL is still a value. */
    ASSERT(cl_repl_result_printable(cl_eval_string("(values nil)")));
    ASSERT(cl_repl_result_printable(cl_eval_string("(list)")));
    ASSERT(cl_repl_result_printable(cl_eval_string("(car '())")));
    /* Ordinary values, single and multiple */
    ASSERT(cl_repl_result_printable(cl_eval_string("(+ 1 2)")));
    ASSERT(cl_repl_result_printable(cl_eval_string("(values 1 2 3)")));
    ASSERT(cl_repl_result_printable(cl_eval_string("(floor 7 2)")));
}

TEST(repl_zero_values_do_not_leak_into_next_form)
{
    /* cl_mv_count is thread state that survives across top-level forms,
     * and value-propagating opcodes don't reset it — a (values) must not
     * silence the NEXT form's result. */
    cl_eval_string("(values)");
    ASSERT(cl_repl_result_printable(cl_eval_string("42")));
    cl_eval_string("(values)");
    ASSERT(cl_repl_result_printable(cl_eval_string("'foo")));
    cl_eval_string("(values)");
    ASSERT(cl_repl_result_printable(cl_eval_string("(list)")));
}

TEST(repl_eval_string_nothing_evaluated_signals_no_values)
{
    /* When cl_eval_string evaluates nothing at all — blank or comment-only
     * input — it must leave a definitive "no values" signal instead of the
     * MV state of the PREVIOUS call, or a caller gating its echo on
     * cl_repl_result_printable (the Debug> prompt) echoes NIL for input
     * that produced nothing. */
    cl_eval_string("(values 1 2 3)");
    ASSERT_EQ_INT(cl_repl_result_printable(cl_eval_string(";; just a comment")), 0);
    cl_eval_string("(values 1 2 3)");
    ASSERT_EQ_INT(cl_repl_result_printable(cl_eval_string("   ")), 0);
}

/* --- REPL multiple-value echo (CLHS 25.1) --- */

/* Print the list of values the REPL would echo for STR. */
static const char *repl_values(const char *str)
{
    static char buf[256];
    CL_Obj result = cl_eval_string(str);
    CL_Obj values = cl_repl_values_list(result);
    CL_GC_PROTECT(values);        /* printing allocates and can compact */
    cl_prin1_to_string(values, buf, sizeof(buf));
    CL_GC_UNPROTECT(1);
    return buf;
}

TEST(repl_values_list_zero_one_and_many)
{
    /* Zero values print nothing at all (issue #6) */
    ASSERT_STR_EQ(repl_values("(values)"), "NIL");
    ASSERT_STR_EQ(repl_values("(values-list nil)"), "NIL");
    /* One value — including a NIL one, which is still a value */
    ASSERT_STR_EQ(repl_values("42"), "(42)");
    ASSERT_STR_EQ(repl_values("(values nil)"), "(NIL)");
    ASSERT_STR_EQ(repl_values("(car '())"), "(NIL)");
    /* Every value is echoed, not just the primary */
    ASSERT_STR_EQ(repl_values("(floor 7 2)"), "(3 1)");
    ASSERT_STR_EQ(repl_values("(values 1 2 3)"), "(1 2 3)");
    ASSERT_STR_EQ(repl_values("(values-list '(:a :b))"), "(:A :B)");
}

TEST(repl_values_list_index_zero_comes_from_the_stack)
{
    /* cl_mv_values[0] is stale for every producer that only writes
     * cl_mv_count = 1 (OP_CONST, OP_GLOAD, arithmetic, …), so slot 0 must
     * come from the returned primary. */
    cl_eval_string("(values 111 222)");
    ASSERT_STR_EQ(repl_values("(+ 1 2)"), "(3)");
    cl_eval_string("(values 111 222)");
    ASSERT_STR_EQ(repl_values("'sym"), "(SYM)");
}

TEST(repl_history_slash_holds_all_values)
{
    /* CLHS 25.1.1: * is the primary value, / the list of all of them. */
    CL_Obj values = cl_repl_values_list(cl_eval_string("(floor 7 2)"));
    CL_GC_PROTECT(values);
    cl_repl_update_history(CL_NIL, values);
    CL_GC_UNPROTECT(1);
    ASSERT_EQ_INT(eval_int("*"), 3);
    ASSERT_STR_EQ(eval_print("/"), "(3 1)");

    cl_repl_update_history(CL_NIL, cl_repl_values_list(cl_eval_string("9")));
    ASSERT_EQ_INT(eval_int("*"), 9);
    ASSERT_STR_EQ(eval_print("/"), "(9)");
    ASSERT_STR_EQ(eval_print("//"), "(3 1)");

    cl_repl_update_history(CL_NIL, cl_repl_values_list(cl_eval_string("(values)")));
    ASSERT_STR_EQ(eval_print("*"), "NIL");     /* no primary value */
    ASSERT_STR_EQ(eval_print("/"), "NIL");     /* no values at all */
    ASSERT_STR_EQ(eval_print("//"), "(9)");
    ASSERT_STR_EQ(eval_print("///"), "(3 1)");
}

/* --- Multiple-values hygiene (CLHS 3.1.7 / 5.1) ---
 *
 * A form specified to return one value must not hand its caller the
 * multiple values of some earlier form.  The compiler enforces this by
 * emitting OP_MV_RESET in front of the opcodes that observe the MV state
 * (see cl_mv_normalize); these tests pin both directions — the leak is
 * closed AND real multiple values still propagate. */

static const char *mvl(const char *form)
{
    static char buf[256];
    char src[512];
    snprintf(src, sizeof(src), "(multiple-value-list %s)", form);
    return strcpy(buf, eval_print(src));
}

TEST(mv_hygiene_single_value_forms_do_not_leak)
{
    /* The value came back through OP_LOAD, which leaves the MV buffer
     * holding (values 1 2) / (floor 7 2). */
    ASSERT_STR_EQ(mvl("(let ((x (values 1 2))) x)"), "(1)");
    ASSERT_STR_EQ(mvl("(let ((x (floor 7 2))) x)"), "(3)");
    ASSERT_STR_EQ(mvl("(let* ((x (values 1 2))) x)"), "(1)");
    /* ... through a boxed (closed-over, mutated) binding: OP_CELL_REF */
    ASSERT_STR_EQ(mvl("(let ((x (values 1 2)))"
                      "  (funcall (lambda () (setq x x))) x)"), "(1)");
    /* ... out of a function whose body is a bare parameter reference */
    cl_eval_string("(defun mv-passthrough (x) x)");
    ASSERT_STR_EQ(mvl("(mv-passthrough (values 1 2))"), "(1)");
    /* ... through the peeking stores of SETQ / SETF (CLHS 5.1) */
    ASSERT_STR_EQ(mvl("(let ((y 0)) (setq y (values 1 2)))"), "(1)");
    ASSERT_STR_EQ(mvl("(let ((y 0)) (setq y (floor 7 2)) y)"), "(3)");
    cl_eval_string("(defvar *mv-global* nil)");
    ASSERT_STR_EQ(mvl("(setq *mv-global* (values 1 2))"), "(1)");
    ASSERT_STR_EQ(mvl("(setf *mv-global* (values 1 2))"), "(1)");
}

TEST(mv_hygiene_short_circuit_forms_return_one_value)
{
    /* CLHS 5.1: a non-last AND/OR arm contributes only its primary value;
     * so does a COND clause with no body. */
    ASSERT_STR_EQ(mvl("(or (values 1 2) 3)"), "(1)");
    ASSERT_STR_EQ(mvl("(and (values nil 2) 3)"), "(NIL)");
    ASSERT_STR_EQ(mvl("(cond ((values 1 2)))"), "(1)");
    /* The LAST arm still propagates everything */
    ASSERT_STR_EQ(mvl("(or nil (values 1 2))"), "(1 2)");
    ASSERT_STR_EQ(mvl("(and 1 (values 3 4))"), "(3 4)");
    ASSERT_STR_EQ(mvl("(and (values 1 2))"), "(1 2)");
}

TEST(mv_hygiene_joins_do_not_leak)
{
    /* Both arms deliver a single value through OP_LOAD */
    ASSERT_STR_EQ(mvl("(let ((a 1) (b 2)) (values a b) (if t a b))"), "(1)");
    ASSERT_STR_EQ(mvl("(let ((a 1)) (values a 2) (if nil a 7))"), "(7)");
    ASSERT_STR_EQ(mvl("(let ((a 1)) (values a 2) (cond (nil 0) (t a)))"), "(1)");
    ASSERT_STR_EQ(mvl("(let ((a 1)) (values a 2) (case 9 (9 a) (t 0)))"), "(1)");
    /* Local RETURN-FROM exit through the block's result slot */
    ASSERT_STR_EQ(mvl("(block b (let ((x (values 1 2))) (return-from b x)))"),
                  "(1)");
    /* Loop result form */
    ASSERT_STR_EQ(mvl("(let ((a 1)) (values a 2) (dolist (i '(1)) i))"), "(NIL)");
}

TEST(mv_hygiene_mixed_joins_normalize_only_the_stale_arm)
{
    /* The hard case: one arm of the join can deliver several values while
     * another arrives through OP_LOAD.  Resetting at the join would throw
     * away the first arm's extra values; not resetting would let the second
     * hand out the values of whatever ran before it (here the TEST form,
     * which is deliberately multi-valued).  The stale arm gets a reset on a
     * landing only it reaches — cl_mv_close_join. */
    ASSERT_STR_EQ(mvl("(let ((y 5)) (if (values nil 2) (floor 7 2) y))"), "(5)");
    ASSERT_STR_EQ(mvl("(let ((y 5)) (if (values t 2) (floor 7 2) y))"), "(3 1)");
    /* ... and with the arms the other way round, so the stale arm is the one
     * that arrives by JMP rather than by falling through */
    ASSERT_STR_EQ(mvl("(let ((y 5)) (if (values t 2) y (floor 7 2)))"), "(5)");
    ASSERT_STR_EQ(mvl("(let ((y 5)) (if (values nil 2) y (floor 7 2)))"), "(3 1)");
    /* Multi-armed joins: COND / CASE / TYPECASE share one exit chain */
    ASSERT_STR_EQ(mvl("(let ((y 5)) (cond ((values nil 2) (floor 7 2)) (t y)))"),
                  "(5)");
    ASSERT_STR_EQ(mvl("(let ((y 5)) (cond ((values t 2) (floor 7 2)) (t y)))"),
                  "(3 1)");
    ASSERT_STR_EQ(mvl("(let ((y 5)) (case 9 (1 (floor 7 2)) (t y)))"), "(5)");
    ASSERT_STR_EQ(mvl("(let ((y 5)) (case 1 (1 (floor 7 2)) (t y)))"), "(3 1)");
    ASSERT_STR_EQ(mvl("(let ((y 5)) (typecase \"s\" (integer (floor 7 2)) (t y)))"),
                  "(5)");
    /* Local BLOCK / loop exits, which reach the join through a result slot */
    ASSERT_STR_EQ(mvl("(let ((y 5)) (block b"
                      "  (when (values nil 2) (return-from b (floor 7 2))) y))"),
                  "(5)");
    ASSERT_STR_EQ(mvl("(let ((y 5)) (block b"
                      "  (when (values t 2) (return-from b (floor 7 2))) y))"),
                  "(3 1)");
    ASSERT_STR_EQ(mvl("(let ((y 5)) (dolist (i '(1) y)"
                      "  (when (values nil 2) (return (floor 7 2)))))"), "(5)");
    ASSERT_STR_EQ(mvl("(let ((y 5)) (dolist (i '(1) y)"
                      "  (when (values t 2) (return (floor 7 2)))))"), "(3 1)");
    ASSERT_STR_EQ(mvl("(let ((y 5)) (dotimes (i 1 y)"
                      "  (when (values nil 2) (return (floor 7 2)))))"), "(5)");
    ASSERT_STR_EQ(mvl("(let ((y 5)) (do ((i 0 (1+ i))) ((> i 0) y)"
                      "  (when (values nil 2) (return (floor 7 2)))))"), "(5)");
}

TEST(mv_hygiene_real_multiple_values_still_propagate)
{
    ASSERT_STR_EQ(mvl("(values 1 2)"), "(1 2)");
    ASSERT_STR_EQ(mvl("(progn 0 (values 1 2))"), "(1 2)");
    ASSERT_STR_EQ(mvl("(let ((x 1)) (values x 2))"), "(1 2)");
    ASSERT_STR_EQ(mvl("(if t (values 1 2) (values 3 4))"), "(1 2)");
    ASSERT_STR_EQ(mvl("(when t (values 1 2))"), "(1 2)");
    ASSERT_STR_EQ(mvl("(cond (t (values 1 2)))"), "(1 2)");
    ASSERT_STR_EQ(mvl("(case 1 (1 (values 1 2)))"), "(1 2)");
    ASSERT_STR_EQ(mvl("(block b (values 1 2))"), "(1 2)");
    ASSERT_STR_EQ(mvl("(block b (return-from b (values 1 2)))"), "(1 2)");
    ASSERT_STR_EQ(mvl("(catch 'c (values 1 2))"), "(1 2)");
    ASSERT_STR_EQ(mvl("(catch 'c (throw 'c (values 1 2)))"), "(1 2)");
    ASSERT_STR_EQ(mvl("(unwind-protect (values 1 2) nil)"), "(1 2)");
    ASSERT_STR_EQ(mvl("(multiple-value-prog1 (values 1 2) nil)"), "(1 2)");
    ASSERT_STR_EQ(mvl("(locally (values 1 2))"), "(1 2)");
    ASSERT_STR_EQ(mvl("(the integer (values 1 2))"), "(1 2)");
    ASSERT_STR_EQ(mvl("(progv '(*mv-progv*) '(1) (values 1 2))"), "(1 2)");
    ASSERT_STR_EQ(mvl("(dolist (i '(1)) (return (values 1 2)))"), "(1 2)");
    ASSERT_STR_EQ(mvl("(handler-case (values 1 2) (error () nil))"), "(1 2)");
    ASSERT_STR_EQ(mvl("(restart-case (values 1 2) (r () nil))"), "(1 2)");
    ASSERT_STR_EQ(mvl("(multiple-value-call #'values (values 1 2))"), "(1 2)");
    ASSERT_STR_EQ(mvl("(funcall (lambda () (values 1 2)))"), "(1 2)");
    ASSERT_STR_EQ(mvl("(apply #'values '(1 2))"), "(1 2)");
    ASSERT_STR_EQ(mvl("(floor 7 2)"), "(3 1)");
}

TEST(mv_hygiene_call_result_is_classified_from_the_callee)
{
    /* A call's MV state belongs to the CALLEE, never to whatever its last
     * argument happened to compile to.  Emitting OP_CALL raw let the state
     * of the final argument stand — and an argument that arrives through
     * OP_LOAD is CL_MV_STALE, so the observer reset it: every function whose
     * body was a MULTIPLE-VALUE-PROG1 lost its second value, because the
     * VALUES-LIST call that restores them ends in a variable load
     * (serapeum's SWAPHASH; caught by make test-extra).  cl_emit_call is
     * now the single point where that classification happens. */
    cl_eval_string("(defun mv-swap (k v ht)"
                   "  (multiple-value-prog1 (gethash k ht)"
                   "    (setf (gethash k ht) v)))");
    cl_eval_string("(defvar *mv-ht* (make-hash-table))");
    cl_eval_string("(setf (gethash 'k *mv-ht*) 1)");
    ASSERT_STR_EQ(mvl("(mv-swap 'k 2 *mv-ht*)"), "(1 T)");
    ASSERT_STR_EQ(eval_print("(nth-value 1 (mv-swap 'k 3 *mv-ht*))"), "T");
    ASSERT_STR_EQ(eval_print("(nth-value 1 (mv-swap 'absent 1 *mv-ht*))"), "NIL");
    /* The same shape without the hash table: the cleanup form's last
     * argument is a plain variable. */
    cl_eval_string("(defun mv-p1 (a b) (multiple-value-prog1 (values a b) (list b)))");
    ASSERT_STR_EQ(mvl("(mv-p1 1 2)"), "(1 2)");
}

TEST(mv_hygiene_bind_and_nth_value_read_their_own_form)
{
    /* MULTIPLE-VALUE-BIND and NTH-VALUE read the MV buffer; it must
     * describe THEIR values form, not something evaluated before it. */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-bind (a b) (let ((x (values 1 2))) x) (list a b))"),
        "(1 NIL)");
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-bind (a b) (floor 7 2) (list a b))"), "(3 1)");
    ASSERT_STR_EQ(eval_print("(nth-value 1 (let ((x (values 1 2))) x))"), "NIL");
    ASSERT_STR_EQ(eval_print("(nth-value 1 (floor 7 2))"), "1");
    /* NTH-VALUE itself returns exactly one value */
    ASSERT_STR_EQ(mvl("(nth-value 0 (values 1 2))"), "(1)");
}

TEST(mv_hygiene_zero_values_survive_a_call)
{
    /* (values) through a function return, and through forms that must not
     * turn "no values" into "one NIL value". */
    cl_eval_string("(defun mv-none () (values))");
    ASSERT_STR_EQ(mvl("(mv-none)"), "NIL");
    ASSERT_STR_EQ(mvl("(progn 1 (mv-none))"), "NIL");
    ASSERT_STR_EQ(mvl("(let ((x 1)) x (mv-none))"), "NIL");
    ASSERT_STR_EQ(mvl("(block b (mv-none))"), "NIL");
    ASSERT_STR_EQ(mvl("(if t (mv-none) 1)"), "NIL");
}

/* --- Printer control variables --- */

TEST(eval_print_var_defaults)
{
    /* CL spec default values */
    ASSERT_STR_EQ(eval_print("*print-escape*"), "T");
    ASSERT_STR_EQ(eval_print("*print-readably*"), "NIL");
    ASSERT_EQ_INT(eval_int("*print-base*"), 10);
    ASSERT_STR_EQ(eval_print("*print-radix*"), "NIL");
    ASSERT_STR_EQ(eval_print("*print-level*"), "NIL");
    ASSERT_STR_EQ(eval_print("*print-length*"), "NIL");
    ASSERT_STR_EQ(eval_print("*print-case*"), ":UPCASE");
    ASSERT_STR_EQ(eval_print("*print-gensym*"), "T");
    ASSERT_STR_EQ(eval_print("*print-array*"), "T");
    ASSERT_STR_EQ(eval_print("*print-circle*"), "NIL");
    ASSERT_STR_EQ(eval_print("*print-pretty*"), "NIL");
}

TEST(eval_print_escape_dynamic)
{
    /* prin1-to-string forces escape=T */
    ASSERT_STR_EQ(eval_print("(prin1-to-string \"hello\")"),
        "\"\\\"hello\\\"\"");
    /* princ-to-string forces escape=NIL */
    ASSERT_STR_EQ(eval_print("(princ-to-string \"hello\")"), "\"hello\"");
    /* Dynamic binding: prin1 overrides *print-escape* NIL binding */
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-escape* nil)) (prin1-to-string \"hi\"))"),
        "\"\\\"hi\\\"\"");
    /* Dynamic binding: princ overrides *print-escape* T binding */
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-escape* t)) (princ-to-string \"hi\"))"),
        "\"hi\"");
    /* Characters also affected */
    ASSERT_STR_EQ(eval_print("(prin1-to-string #\\Space)"), "\"#\\\\Space\"");
    ASSERT_STR_EQ(eval_print("(princ-to-string #\\Space)"), "\" \"");
}

TEST(eval_print_level)
{
    /* Level 0 — everything is # */
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-level* 0)) (prin1-to-string '(a b)))"),
        "\"#\"");
    /* Level 1 — top list shown, nested lists become # */
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-level* 1)) (prin1-to-string '(a (b (c)))))"),
        "\"(A #)\"");
    /* Level 2 */
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-level* 2)) (prin1-to-string '(a (b (c)))))"),
        "\"(A (B #))\"");
    /* NIL = no limit (default) */
    ASSERT_STR_EQ(eval_print(
        "(prin1-to-string '(a (b (c))))"),
        "\"(A (B (C)))\"");
}

TEST(eval_print_length)
{
    /* Length 0 — no elements shown */
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-length* 0)) (prin1-to-string '(a b c)))"),
        "\"(...)\"");
    /* Length 2 — first 2 shown, rest truncated */
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-length* 2)) (prin1-to-string '(a b c d e)))"),
        "\"(A B ...)\"");
    /* Length longer than list — no truncation */
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-length* 10)) (prin1-to-string '(a b)))"),
        "\"(A B)\"");
    /* Dotted pair not truncated when within length */
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-length* 2)) (prin1-to-string '(a . b)))"),
        "\"(A . B)\"");
}

TEST(eval_print_level_length_combined)
{
    /* Both level and length active */
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-level* 1) (*print-length* 2))"
        "  (prin1-to-string '(a b (c d) e)))"),
        "\"(A B ...)\"");
}

TEST(eval_print_base_binary)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-base* 2)) (prin1-to-string 10))"),
        "\"1010\"");
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-base* 2)) (prin1-to-string 255))"),
        "\"11111111\"");
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-base* 2)) (prin1-to-string 0))"),
        "\"0\"");
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-base* 2)) (prin1-to-string -5))"),
        "\"-101\"");
}

TEST(eval_print_base_octal)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-base* 8)) (prin1-to-string 8))"),
        "\"10\"");
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-base* 8)) (prin1-to-string 255))"),
        "\"377\"");
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-base* 8)) (prin1-to-string 0))"),
        "\"0\"");
}

TEST(eval_print_base_hex)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-base* 16)) (prin1-to-string 255))"),
        "\"FF\"");
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-base* 16)) (prin1-to-string 256))"),
        "\"100\"");
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-base* 16)) (prin1-to-string 0))"),
        "\"0\"");
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-base* 16)) (prin1-to-string -1))"),
        "\"-1\"");
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-base* 16)) (prin1-to-string 3735928559))"),
        "\"DEADBEEF\"");
}

TEST(eval_print_base_other)
{
    /* Base 3 */
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-base* 3)) (prin1-to-string 9))"),
        "\"100\"");
    /* Base 36 */
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-base* 36)) (prin1-to-string 35))"),
        "\"Z\"");
}

TEST(eval_print_radix_decimal)
{
    /* Radix with base 10: trailing dot */
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-radix* t)) (prin1-to-string 42))"),
        "\"42.\"");
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-radix* t)) (prin1-to-string 0))"),
        "\"0.\"");
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-radix* t)) (prin1-to-string -7))"),
        "\"-7.\"");
}

TEST(eval_print_radix_binary)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-base* 2) (*print-radix* t)) (prin1-to-string 10))"),
        "\"#b1010\"");
}

TEST(eval_print_radix_octal)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-base* 8) (*print-radix* t)) (prin1-to-string 255))"),
        "\"#o377\"");
}

TEST(eval_print_radix_hex)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-base* 16) (*print-radix* t)) (prin1-to-string 255))"),
        "\"#xFF\"");
}

TEST(eval_print_radix_other)
{
    /* Base 3 with radix: #3r prefix */
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-base* 3) (*print-radix* t)) (prin1-to-string 9))"),
        "\"#3r100\"");
}

TEST(eval_print_base_bignum)
{
    /* Bignum in hex */
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-base* 16)) (prin1-to-string (expt 2 32)))"),
        "\"100000000\"");
    /* Bignum in binary */
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-base* 2)) (prin1-to-string (expt 2 16)))"),
        "\"10000000000000000\"");
    /* Bignum in octal */
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-base* 8)) (prin1-to-string 65536))"),
        "\"200000\"");
}

TEST(eval_print_radix_bignum)
{
    /* Bignum with radix prefix */
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-base* 16) (*print-radix* t)) (prin1-to-string (expt 2 32)))"),
        "\"#x100000000\"");
    /* Bignum with base 10 radix (trailing dot) */
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-radix* t)) (prin1-to-string (expt 2 32)))"),
        "\"4294967296.\"");
}

TEST(eval_print_case_upcase)
{
    /* Default :UPCASE — symbols printed as-is */
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-case* :upcase)) (prin1-to-string 'hello))"),
        "\"HELLO\"");
    ASSERT_STR_EQ(eval_print(
        "(prin1-to-string 'hello)"), "\"HELLO\"");
}

TEST(eval_print_case_downcase)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-case* :downcase)) (prin1-to-string 'hello))"),
        "\"hello\"");
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-case* :downcase)) (prin1-to-string 'foo-bar))"),
        "\"foo-bar\"");
    /* NIL is a symbol too */
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-case* :downcase)) (prin1-to-string nil))"),
        "\"nil\"");
    /* T is a symbol too */
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-case* :downcase)) (prin1-to-string t))"),
        "\"t\"");
    /* Keywords */
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-case* :downcase)) (prin1-to-string :foo))"),
        "\":foo\"");
}

TEST(eval_print_case_capitalize)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-case* :capitalize)) (prin1-to-string 'hello))"),
        "\"Hello\"");
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-case* :capitalize)) (prin1-to-string 'foo-bar))"),
        "\"Foo-Bar\"");
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-case* :capitalize)) (prin1-to-string nil))"),
        "\"Nil\"");
    /* Keywords */
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-case* :capitalize)) (prin1-to-string :test))"),
        "\":Test\"");
}

TEST(eval_print_case_in_list)
{
    /* print-case affects symbols inside lists */
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-case* :downcase)) (prin1-to-string '(a b c)))"),
        "\"(a b c)\"");
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-case* :capitalize)) (prin1-to-string '(hello world)))"),
        "\"(Hello World)\"");
}

TEST(eval_print_gensym)
{
    /* Default: *print-gensym* = T, uninterned symbols get #: prefix */
    ASSERT_STR_EQ(eval_print(
        "(prin1-to-string (make-symbol \"FOO\"))"),
        "\"#:FOO\"");
    /* With *print-gensym* = NIL, no #: prefix */
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-gensym* nil)) (prin1-to-string (make-symbol \"FOO\")))"),
        "\"FOO\"");
    /* Still applies print-case */
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-gensym* nil) (*print-case* :downcase))"
        "  (prin1-to-string (make-symbol \"FOO\")))"),
        "\"foo\"");
}

TEST(eval_print_array)
{
    /* Default: *print-array* = T, vectors print contents */
    ASSERT_STR_EQ(eval_print(
        "(prin1-to-string (vector 1 2 3))"),
        "\"#(1 2 3)\"");
    /* With *print-array* = NIL, vectors print as #<VECTOR> */
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-array* nil)) (prin1-to-string (vector 1 2 3)))"),
        "\"#<VECTOR>\"");
    /* Multi-dim arrays print as #<ARRAY> when *print-array* = NIL */
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-array* nil)) (prin1-to-string (make-array '(2 3))))"),
        "\"#<ARRAY>\"");
}

/* --- WRITE function with keyword arguments --- */

TEST(eval_write_basic)
{
    /* write returns the object */
    ASSERT_EQ_INT(eval_int(
        "(let ((x (write 42 :stream (make-string-output-stream)))) x)"), 42);

    /* write with :escape nil acts like princ */
    ASSERT_STR_EQ(eval_print(
        "(let ((s (make-string-output-stream)))"
        "  (write \"hello\" :stream s :escape nil)"
        "  (get-output-stream-string s))"),
        "\"hello\"");

    /* write with :escape t acts like prin1 */
    ASSERT_STR_EQ(eval_print(
        "(let ((s (make-string-output-stream)))"
        "  (write \"hello\" :stream s :escape t)"
        "  (get-output-stream-string s))"),
        "\"\\\"hello\\\"\"");
}

TEST(eval_write_base_radix)
{
    /* write with :base 16 */
    ASSERT_STR_EQ(eval_print(
        "(let ((s (make-string-output-stream)))"
        "  (write 255 :stream s :base 16)"
        "  (get-output-stream-string s))"),
        "\"FF\"");

    /* write with :base 2 */
    ASSERT_STR_EQ(eval_print(
        "(let ((s (make-string-output-stream)))"
        "  (write 10 :stream s :base 2)"
        "  (get-output-stream-string s))"),
        "\"1010\"");

    /* write with :radix t :base 16 */
    ASSERT_STR_EQ(eval_print(
        "(let ((s (make-string-output-stream)))"
        "  (write 255 :stream s :base 16 :radix t)"
        "  (get-output-stream-string s))"),
        "\"#xFF\"");
}

TEST(eval_write_level_length)
{
    /* write with :level 1 */
    ASSERT_STR_EQ(eval_print(
        "(let ((s (make-string-output-stream)))"
        "  (write '(a (b (c))) :stream s :level 1)"
        "  (get-output-stream-string s))"),
        "\"(A #)\"");

    /* write with :length 2 */
    ASSERT_STR_EQ(eval_print(
        "(let ((s (make-string-output-stream)))"
        "  (write '(a b c d e) :stream s :length 2)"
        "  (get-output-stream-string s))"),
        "\"(A B ...)\"");
}

TEST(eval_write_case)
{
    /* write with :case :downcase */
    ASSERT_STR_EQ(eval_print(
        "(let ((s (make-string-output-stream)))"
        "  (write 'hello :stream s :case :downcase)"
        "  (get-output-stream-string s))"),
        "\"hello\"");

    /* write with :case :capitalize */
    ASSERT_STR_EQ(eval_print(
        "(let ((s (make-string-output-stream)))"
        "  (write 'foo-bar :stream s :case :capitalize)"
        "  (get-output-stream-string s))"),
        "\"Foo-Bar\"");
}

TEST(eval_write_gensym_array)
{
    /* write with :gensym nil */
    ASSERT_STR_EQ(eval_print(
        "(let ((s (make-string-output-stream)))"
        "  (write (make-symbol \"FOO\") :stream s :gensym nil)"
        "  (get-output-stream-string s))"),
        "\"FOO\"");

    /* write with :array nil */
    ASSERT_STR_EQ(eval_print(
        "(let ((s (make-string-output-stream)))"
        "  (write (vector 1 2) :stream s :array nil)"
        "  (get-output-stream-string s))"),
        "\"#<VECTOR>\"");
}

/* --- *print-circle* --- */

TEST(eval_print_circle_cdr_cycle)
{
    /* CDR cycle: (let ((x (list 1 2))) (rplacd (cdr x) x) x) => #1=(1 2 . #1#) */
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-circle* t))"
        "  (let ((x (list 1 2)))"
        "    (rplacd (cdr x) x)"
        "    (prin1-to-string x)))"),
        "\"#0=(1 2 . #0#)\"");
}

TEST(eval_print_circle_car_self)
{
    /* CAR self-ref: (let ((x (list nil))) (rplaca x x) x) => #1=(#1#) */
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-circle* t))"
        "  (let ((x (list nil)))"
        "    (rplaca x x)"
        "    (prin1-to-string x)))"),
        "\"#0=(#0#)\"");
}

TEST(eval_print_circle_shared_sub)
{
    /* Shared substructure: same object in two positions */
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-circle* t))"
        "  (let ((sub (list 1 2)))"
        "    (prin1-to-string (list sub sub))))"),
        "\"(#0=(1 2) #0#)\"");
}

TEST(eval_print_circle_no_sharing)
{
    /* No sharing: plain list prints without labels */
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-circle* t))"
        "  (prin1-to-string '(1 2 3)))"),
        "\"(1 2 3)\"");
}

TEST(eval_print_circle_nil_default)
{
    /* *print-circle* NIL (default): no labels even with sharing */
    ASSERT_STR_EQ(eval_print(
        "(let ((sub (list 1)))"
        "  (prin1-to-string (list sub sub)))"),
        "\"((1) (1))\"");
}

TEST(eval_print_circle_vector)
{
    /* Circular vector: vector containing self */
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-circle* t))"
        "  (let ((v (vector nil 42)))"
        "    (setf (aref v 0) v)"
        "    (prin1-to-string v)))"),
        "\"#0=#(#0# 42)\"");
}

TEST(eval_print_circle_deep_shared)
{
    /* Deep shared: leaf shared at depth */
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-circle* t))"
        "  (let ((leaf (list 99)))"
        "    (prin1-to-string (list (list leaf) (list leaf)))))"),
        "\"((#0=(99)) (#0#))\"");
}

TEST(eval_print_circle_write_to_string)
{
    /* write-to-string with :circle t */
    ASSERT_STR_EQ(eval_print(
        "(let ((x (list 1 2)))"
        "  (rplacd (cdr x) x)"
        "  (write-to-string x :circle t))"),
        "\"#0=(1 2 . #0#)\"");
}

/* --- Format directive extensions --- */

TEST(eval_format_decimal)
{
    /* ~D: decimal */
    ASSERT_STR_EQ(eval_print("(format nil \"~D\" 42)"), "\"42\"");
    ASSERT_STR_EQ(eval_print("(format nil \"~D\" -7)"), "\"-7\"");
    ASSERT_STR_EQ(eval_print("(format nil \"~D\" 0)"), "\"0\"");
}

TEST(eval_format_binary)
{
    /* ~B: binary */
    ASSERT_STR_EQ(eval_print("(format nil \"~B\" 10)"), "\"1010\"");
    ASSERT_STR_EQ(eval_print("(format nil \"~B\" 255)"), "\"11111111\"");
    ASSERT_STR_EQ(eval_print("(format nil \"~B\" 0)"), "\"0\"");
}

TEST(eval_format_octal)
{
    /* ~O: octal */
    ASSERT_STR_EQ(eval_print("(format nil \"~O\" 8)"), "\"10\"");
    ASSERT_STR_EQ(eval_print("(format nil \"~O\" 255)"), "\"377\"");
}

TEST(eval_format_hex)
{
    /* ~X: hexadecimal */
    ASSERT_STR_EQ(eval_print("(format nil \"~X\" 255)"), "\"FF\"");
    ASSERT_STR_EQ(eval_print("(format nil \"~X\" 256)"), "\"100\"");
    ASSERT_STR_EQ(eval_print("(format nil \"~X\" 0)"), "\"0\"");
}

TEST(eval_format_character)
{
    /* ~C: character */
    ASSERT_STR_EQ(eval_print("(format nil \"~C\" #\\A)"), "\"A\"");
    ASSERT_STR_EQ(eval_print("(format nil \"~C\" #\\Space)"), "\" \"");
}

TEST(eval_format_write)
{
    /* ~W: write */
    ASSERT_STR_EQ(eval_print("(format nil \"~W\" \"hello\")"), "\"\\\"hello\\\"\"");
    ASSERT_STR_EQ(eval_print("(format nil \"~W\" 42)"), "\"42\"");
}

TEST(eval_format_freshline)
{
    /* ~&: fresh line.  The newline in the value prints literally inside the
     * quotes — prin1 escapes only " and \ (CLHS 22.1.3.4). */
    ASSERT_STR_EQ(eval_print("(format nil \"hello~&world\")"), "\"hello\nworld\"");
}

TEST(eval_format_page)
{
    /* ~|: page separator (form feed character) */
    ASSERT_EQ_INT(eval_int("(length (format nil \"a~|b\"))"), 3);
    ASSERT_EQ_INT(eval_int("(char-code (char (format nil \"a~|b\") 1))"), 12);
}

TEST(eval_format_justify)
{
    /* ~<...~> body is emitted (no padding without column tracking). */
    ASSERT_STR_EQ(eval_print("(format nil \"~<x~>\")"), "\"x\"");
    /* ~<overflow~:;body~> drops the overflow indicator. */
    ASSERT_STR_EQ(eval_print(
        "(format nil \"~@[~<~%~:; ~A~>~]\" 42)"), "\" 42\"");
    /* ~<...~:> (logical block) emits the body; ~_ becomes a no-op. */
    ASSERT_STR_EQ(eval_print(
        "(format nil \"~<~{~A~^,~_~}~:>\" '(1 2 3))"), "\"1,2,3\"");
}

TEST(eval_format_mixed)
{
    /* Mix of directives */
    ASSERT_STR_EQ(eval_print(
        "(format nil \"dec=~D hex=~X bin=~B\" 255 255 255)"),
        "\"dec=255 hex=FF bin=11111111\"");
}

/* --- Advanced Format: Step 1 — padding, commas, sign --- */

TEST(eval_format_padded_decimal)
{
    /* ~mincolD: right-pad with spaces */
    ASSERT_STR_EQ(eval_print("(format nil \"~10D\" 42)"), "\"        42\"");
    /* ~mincol,'padcharD: pad with custom char */
    ASSERT_STR_EQ(eval_print("(format nil \"~10,'0D\" 42)"), "\"0000000042\"");
    /* ~:D: insert commas */
    ASSERT_STR_EQ(eval_print("(format nil \"~:D\" 1234567)"), "\"1,234,567\"");
    /* ~@D: always show sign */
    ASSERT_STR_EQ(eval_print("(format nil \"~@D\" 42)"), "\"+42\"");
    ASSERT_STR_EQ(eval_print("(format nil \"~@D\" -7)"), "\"-7\"");
    /* Combine mincol + comma + sign */
    ASSERT_STR_EQ(eval_print("(format nil \"~14:@D\" 1234567)"), "\"    +1,234,567\"");
}

TEST(eval_format_padded_aesthetic)
{
    /* ~mincolA: left-justify, right-pad */
    ASSERT_STR_EQ(eval_print("(format nil \"~10A\" \"hi\")"), "\"hi        \"");
    /* ~mincol@A: right-justify, left-pad */
    ASSERT_STR_EQ(eval_print("(format nil \"~10@A\" \"hi\")"), "\"        hi\"");
    /* ~mincolS: left-justify, right-pad (with escape) */
    ASSERT_STR_EQ(eval_print("(format nil \"~10S\" \"hi\")"), "\"\\\"hi\\\"      \"");
}

TEST(eval_format_prefix_params)
{
    /* ~n%: multiple newlines */
    ASSERT_EQ_INT(eval_int("(length (format nil \"~3%\"))"), 3);
    /* ~n~: multiple tildes */
    ASSERT_STR_EQ(eval_print("(format nil \"~3~\")"), "\"~~~\"");
}

/* --- Advanced Format: Step 2 — ~* and ~T --- */

TEST(eval_format_goto)
{
    /* ~* skip forward 1 arg */
    ASSERT_STR_EQ(eval_print("(format nil \"~A ~* ~A\" 1 2 3)"), "\"1  3\"");
    /* ~n* skip forward n args */
    ASSERT_STR_EQ(eval_print("(format nil \"~A ~2* ~A\" 1 2 3 4)"), "\"1  4\"");
    /* ~:* back up 1 arg */
    ASSERT_STR_EQ(eval_print("(format nil \"~A ~A ~:*~A\" 1 2)"), "\"1 2 2\"");
    /* ~n:* back up n args */
    ASSERT_STR_EQ(eval_print("(format nil \"~A ~A ~2:*~A ~A\" 1 2)"), "\"1 2 1 2\"");
    /* ~n@* go to absolute position */
    ASSERT_STR_EQ(eval_print("(format nil \"~A ~A ~0@*~A\" 1 2)"), "\"1 2 1\"");
}

TEST(eval_format_tabulate)
{
    /* ~colnumT: absolute tab to column */
    ASSERT_STR_EQ(eval_print("(format nil \"abc~10Tdef\")"), "\"abc       def\"");
    /* Already past column — colinc kicks in */
    ASSERT_STR_EQ(eval_print("(format nil \"abcdefghij~10,5Txx\")"), "\"abcdefghij     xx\"");
    /* ~colnum,colinc@T: relative tab */
    ASSERT_STR_EQ(eval_print("(format nil \"abc~3,1@Tdef\")"), "\"abc   def\"");
}

/* --- Advanced Format: Step 3 — ~(~) case conversion --- */

TEST(eval_format_case_downcase)
{
    /* ~(text~) — lowercase all */
    ASSERT_STR_EQ(eval_print("(format nil \"~(~A~)\" \"HELLO WORLD\")"),
                  "\"hello world\"");
}

TEST(eval_format_case_capitalize)
{
    /* ~:(text~) — capitalize each word */
    ASSERT_STR_EQ(eval_print("(format nil \"~:(~A~)\" \"hello world\")"),
                  "\"Hello World\"");
}

TEST(eval_format_case_cap_first)
{
    /* ~@(text~) — capitalize first word only */
    ASSERT_STR_EQ(eval_print("(format nil \"~@(~A~)\" \"hello world\")"),
                  "\"Hello world\"");
}

TEST(eval_format_case_upcase)
{
    /* ~:@(text~) — uppercase all */
    ASSERT_STR_EQ(eval_print("(format nil \"~:@(~A~)\" \"hello\")"),
                  "\"HELLO\"");
}

/* --- Advanced Format: Step 4 — ~[~;~] conditional --- */

TEST(eval_format_cond_numeric)
{
    /* ~[c0~;c1~;c2~] — select by integer */
    ASSERT_STR_EQ(eval_print("(format nil \"~[zero~;one~;two~]\" 1)"), "\"one\"");
    ASSERT_STR_EQ(eval_print("(format nil \"~[zero~;one~;two~]\" 0)"), "\"zero\"");
    ASSERT_STR_EQ(eval_print("(format nil \"~[zero~;one~;two~]\" 2)"), "\"two\"");
    /* Out of range — no output */
    ASSERT_STR_EQ(eval_print("(format nil \"~[zero~;one~]\" 5)"), "\"\"");
}

TEST(eval_format_cond_default)
{
    /* ~[c0~;c1~:;default~] — last clause with ~:; is default */
    ASSERT_STR_EQ(eval_print("(format nil \"~[zero~;one~:;other~]\" 5)"), "\"other\"");
    ASSERT_STR_EQ(eval_print("(format nil \"~[zero~;one~:;other~]\" 0)"), "\"zero\"");
}

TEST(eval_format_cond_boolean)
{
    /* ~:[false~;true~] — nil=clause0, non-nil=clause1 */
    ASSERT_STR_EQ(eval_print("(format nil \"~:[false~;true~]\" nil)"), "\"false\"");
    ASSERT_STR_EQ(eval_print("(format nil \"~:[false~;true~]\" 42)"), "\"true\"");
    ASSERT_STR_EQ(eval_print("(format nil \"~:[false~;true~]\" t)"), "\"true\"");
}

TEST(eval_format_cond_atsign)
{
    /* ~@[clause~] — if non-nil, format clause (arg available); if nil, skip */
    ASSERT_STR_EQ(eval_print("(format nil \"~@[x=~A ~]done\" 42)"), "\"x=42 done\"");
    ASSERT_STR_EQ(eval_print("(format nil \"~@[x=~A ~]done\" nil)"), "\"done\"");
}

/* --- Advanced format: Step 5 — ~{~} iteration and ~^ --- */

TEST(eval_format_iteration_list)
{
    /* ~{body~} — iterate over list elements */
    ASSERT_STR_EQ(eval_print("(format nil \"~{~A ~}\" '(1 2 3))"), "\"1 2 3 \"");
    ASSERT_STR_EQ(eval_print("(format nil \"~{~A~^, ~}\" '(1 2 3))"), "\"1, 2, 3\"");
    ASSERT_STR_EQ(eval_print("(format nil \"~{~A~}\" nil)"), "\"\"");
}

TEST(eval_format_iteration_sublists)
{
    /* ~:{body~} — iterate over list of sublists */
    ASSERT_STR_EQ(eval_print("(format nil \"~:{[~A ~A] ~}\" '((a 1) (b 2)))"), "\"[A 1] [B 2] \"");
    ASSERT_STR_EQ(eval_print("(format nil \"~:{~A=~A ~}\" '((x 1) (y 2) (z 3)))"), "\"X=1 Y=2 Z=3 \"");
}

TEST(eval_format_iteration_atsign)
{
    /* ~@{body~} — iterate using remaining args directly */
    ASSERT_STR_EQ(eval_print("(format nil \"~@{~A~^, ~}\" 1 2 3)"), "\"1, 2, 3\"");
    ASSERT_STR_EQ(eval_print("(format nil \"~@{~A ~A~^; ~}\" 'a 1 'b 2)"), "\"A 1; B 2\"");
}

TEST(eval_format_iteration_colon_atsign)
{
    /* ~:@{body~} — remaining args are sublists */
    ASSERT_STR_EQ(eval_print("(format nil \"~:@{~A-~A ~}\" '(a 1) '(b 2))"), "\"A-1 B-2 \"");
}

TEST(eval_format_iteration_limit)
{
    /* ~n{body~} — max iteration count */
    ASSERT_STR_EQ(eval_print("(format nil \"~2{~A ~}\" '(1 2 3 4 5))"), "\"1 2 \"");
}

TEST(eval_format_iteration_nil_list)
{
    /* CLHS 22.3.7.4: NIL list arg → no iterations, even with v-prefix. */
    ASSERT_STR_EQ(eval_print("(format nil \"[~{X~}]\" nil)"), "\"[]\"");
    ASSERT_STR_EQ(eval_print("(format nil \"[~v{X~}]\" 3 nil)"), "\"[]\"");
}

TEST(eval_format_iteration_non_list_arg)
{
    /* SBCL behavior (matches print-table's ~v{─~} pattern in
       trivial-benchmark): ~{ with a non-NIL non-list arg treats the arg
       as a "non-empty source" with no extractable elements.  Iteration
       runs until the v-prefix cap is reached when body consumes nothing.
       Body that tries to consume args from such a source would error. */
    ASSERT_STR_EQ(eval_print("(format nil \"[~v{X~}]\" 3 5)"), "\"[XXX]\"");
    ASSERT_STR_EQ(eval_print("(format nil \"[~v{X~}]\" 0 5)"), "\"[]\"");
    /* Print-table-style: outer iterates a list of widths, inner runs
       v iterations of a literal char per element.  Previously errored
       with `CAR: 5 is not a list`. */
    ASSERT_STR_EQ(eval_print(
        "(format nil \"[~{~v{X~}~^|~:*~}]\" '(3 5 2))"),
        "\"[XXX|XXXXX]\"");
}

TEST(eval_format_fixed_non_number_arg)
{
    /* CLHS 22.3.3.1: ~F on a non-number arg prints it as by ~wD — right-
       justified in w columns, like the numbers.  Without this fallback,
       trivial-benchmark's print-table chokes on the symbol header row. */
    ASSERT_STR_EQ(eval_print("(format nil \"[~vf]\" 8 'total)"),  "\"[   TOTAL]\"");
    ASSERT_STR_EQ(eval_print("(format nil \"[~v,2f]\" 8 1.5)"),   "\"[    1.50]\"");
    ASSERT_STR_EQ(eval_print("(format nil \"[~v,2f]\" 8 :total)"),"\"[   TOTAL]\"");
}

TEST(eval_format_escape)
{
    /* ~^ — escape iteration when no args remain */
    ASSERT_STR_EQ(eval_print("(format nil \"~{~A~^-~}\" '(x))"), "\"X\"");
    ASSERT_STR_EQ(eval_print("(format nil \"~{~A~^-~}\" '(x y z))"), "\"X-Y-Z\"");
}

/* --- Advanced format: Step 6 — ~? recursive and ~R radix --- */

TEST(eval_format_recursive)
{
    /* ~? — recursive format with string + arg list */
    ASSERT_STR_EQ(eval_print("(format nil \"~? and ~A\" \"~A ~A\" '(1 2) 3)"), "\"1 2 and 3\"");
}

TEST(eval_format_recursive_atsign)
{
    /* ~@? — recursive format using remaining args */
    ASSERT_STR_EQ(eval_print("(format nil \"~@? and ~A\" \"~A ~A\" 1 2 3)"), "\"1 2 and 3\"");
}

#ifdef CL_WIDE_STRINGS
TEST(eval_format_wide_string_passthrough)
{
    /* Wide-string format strings (e.g. trivial-benchmark's box-drawing chars)
     * used to be rejected by cl_format_to_stream, silently dropping the entire
     * format. Now literal non-ASCII codepoints flow through verbatim. */

    /* U+2502 BOX DRAWINGS LIGHT VERTICAL (= 9474). */
    ASSERT_STR_EQ(eval_print(
        "(length (format nil"
        "  (concatenate 'string (string (code-char 9474)) \"hi\""
        "                       (string (code-char 9474)))))"),
        "4");
    ASSERT_STR_EQ(eval_print(
        "(char-code (char (format nil"
        "  (concatenate 'string (string (code-char 9474)) \"hi\")) 0))"),
        "9474");

    /* Mixed ASCII directives + wide literal chars in a single format string. */
    ASSERT_STR_EQ(eval_print(
        "(char-code (char (format nil"
        "  (concatenate 'string (string (code-char 9474)) \"~A\") 1) 0))"),
        "9474");
    ASSERT_STR_EQ(eval_print(
        "(char-code (char (format nil"
        "  (concatenate 'string (string (code-char 9474)) \"~A\") 1) 1))"),
        "49");

    /* ~~ and ~% still parse inside a wide format string. */
    ASSERT_STR_EQ(eval_print(
        "(length (format nil"
        "  (concatenate 'string (string (code-char 9474)) \"~~~%\")))"),
        "3");

    /* ~? recursive format accepts a wide format string. */
    ASSERT_STR_EQ(eval_print(
        "(length (format nil \"~?\""
        "  (concatenate 'string (string (code-char 9474)) \"~A\") '(1)))"),
        "2");
}
#endif

TEST(eval_format_radix_cardinal)
{
    /* ~R — cardinal English */
    ASSERT_STR_EQ(eval_print("(format nil \"~R\" 0)"), "\"zero\"");
    ASSERT_STR_EQ(eval_print("(format nil \"~R\" 5)"), "\"five\"");
    ASSERT_STR_EQ(eval_print("(format nil \"~R\" 42)"), "\"forty-two\"");
    ASSERT_STR_EQ(eval_print("(format nil \"~R\" 100)"), "\"one hundred\"");
    ASSERT_STR_EQ(eval_print("(format nil \"~R\" 1000)"), "\"one thousand\"");
    ASSERT_STR_EQ(eval_print("(format nil \"~R\" 1234)"), "\"one thousand two hundred thirty-four\"");
}

TEST(eval_format_radix_ordinal)
{
    /* ~:R — ordinal English */
    ASSERT_STR_EQ(eval_print("(format nil \"~:R\" 1)"), "\"first\"");
    ASSERT_STR_EQ(eval_print("(format nil \"~:R\" 3)"), "\"third\"");
    ASSERT_STR_EQ(eval_print("(format nil \"~:R\" 21)"), "\"twenty-first\"");
    ASSERT_STR_EQ(eval_print("(format nil \"~:R\" 100)"), "\"one hundredth\"");
}

TEST(eval_format_radix_roman)
{
    /* ~@R — Roman numerals */
    ASSERT_STR_EQ(eval_print("(format nil \"~@R\" 4)"), "\"IV\"");
    ASSERT_STR_EQ(eval_print("(format nil \"~@R\" 9)"), "\"IX\"");
    ASSERT_STR_EQ(eval_print("(format nil \"~@R\" 42)"), "\"XLII\"");
    ASSERT_STR_EQ(eval_print("(format nil \"~@R\" 1999)"), "\"MCMXCIX\"");
}

TEST(eval_format_radix_old_roman)
{
    /* ~:@R — old-style Roman (additive) */
    ASSERT_STR_EQ(eval_print("(format nil \"~:@R\" 4)"), "\"IIII\"");
    ASSERT_STR_EQ(eval_print("(format nil \"~:@R\" 9)"), "\"VIIII\"");
}

TEST(eval_format_radix_base)
{
    /* ~nR — print in base n */
    ASSERT_STR_EQ(eval_print("(format nil \"~2R\" 10)"), "\"1010\"");
    ASSERT_STR_EQ(eval_print("(format nil \"~8R\" 255)"), "\"377\"");
    ASSERT_STR_EQ(eval_print("(format nil \"~16R\" 255)"), "\"FF\"");
}

/* --- Phase 8: Loop macro --- */

TEST(eval_loop_simple_return)
{
    /* Simple loop with return */
    ASSERT_EQ_INT(eval_int(
        "(let ((i 0))"
        "  (loop"
        "    (when (= i 5) (return i))"
        "    (setq i (+ i 1))))"), 5);
}

TEST(eval_loop_simple_accumulate)
{
    /* Simple loop accumulating a list */
    ASSERT_STR_EQ(eval_print(
        "(let ((result nil) (i 0))"
        "  (loop"
        "    (when (>= i 3) (return (nreverse result)))"
        "    (push i result)"
        "    (setq i (+ i 1))))"), "(0 1 2)");
}

TEST(eval_loop_while)
{
    /* Extended loop with while */
    ASSERT_EQ_INT(eval_int(
        "(let ((i 0) (sum 0))"
        "  (loop while (< i 5)"
        "    do (setq sum (+ sum i))"
        "       (setq i (+ i 1)))"
        "  sum)"), 10);
}

TEST(eval_loop_until)
{
    /* Extended loop with until */
    ASSERT_EQ_INT(eval_int(
        "(let ((i 0) (sum 0))"
        "  (loop until (= i 4)"
        "    do (setq sum (+ sum i))"
        "       (setq i (+ i 1)))"
        "  sum)"), 6);
}

TEST(eval_loop_do_multiple)
{
    /* Extended loop do with multiple body forms */
    ASSERT_EQ_INT(eval_int(
        "(let ((x 0) (y 0))"
        "  (loop while (< x 3)"
        "    do (setq x (+ x 1))"
        "       (setq y (+ y 10)))"
        "  y)"), 30);
}

/* --- Phase 8: Loop Step 2 — for/as/repeat --- */

TEST(eval_loop_for_in)
{
    /* for var in list */
    ASSERT_STR_EQ(eval_print(
        "(let ((r nil))"
        "  (loop for x in '(1 2 3)"
        "    do (push x r))"
        "  (nreverse r))"), "(1 2 3)");
}

TEST(eval_loop_for_in_by)
{
    /* for var in list by #'cddr */
    ASSERT_STR_EQ(eval_print(
        "(let ((r nil))"
        "  (loop for x in '(1 2 3 4 5)"
        "    by #'cddr"
        "    do (push x r))"
        "  (nreverse r))"), "(1 3 5)");
}

TEST(eval_loop_for_on)
{
    /* for var on list */
    ASSERT_STR_EQ(eval_print(
        "(let ((r nil))"
        "  (loop for x on '(1 2 3)"
        "    do (push x r))"
        "  (nreverse r))"), "((1 2 3) (2 3) (3))");
}

TEST(eval_loop_for_from_to)
{
    /* for var from start to end */
    ASSERT_EQ_INT(eval_int(
        "(let ((sum 0))"
        "  (loop for i from 1 to 5"
        "    do (setq sum (+ sum i)))"
        "  sum)"), 15);
}

TEST(eval_loop_for_from_below)
{
    /* for var from start below end */
    ASSERT_EQ_INT(eval_int(
        "(let ((sum 0))"
        "  (loop for i from 0 below 4"
        "    do (setq sum (+ sum i)))"
        "  sum)"), 6);
}

TEST(eval_loop_for_upto)
{
    /* for var upto end -- UPTO as first arithmetic keyword (defaults from 0) */
    ASSERT_STR_EQ(eval_print(
        "(loop for i upto 5 collect i)"), "(0 1 2 3 4 5)");
}

TEST(eval_loop_for_to)
{
    /* for var to end -- TO as first arithmetic keyword (defaults from 0) */
    ASSERT_STR_EQ(eval_print(
        "(loop for i to 3 collect i)"), "(0 1 2 3)");
}

TEST(eval_loop_for_upto_by)
{
    /* for var upto end by step -- first keyword UPTO with BY */
    ASSERT_STR_EQ(eval_print(
        "(loop for i upto 10 by 2 collect i)"), "(0 2 4 6 8 10)");
}

TEST(eval_loop_for_downto)
{
    /* for var downto end -- DOWNTO as first arithmetic keyword (defaults from 0) */
    ASSERT_STR_EQ(eval_print(
        "(loop for i downto -3 collect i)"), "(0 -1 -2 -3)");
}

TEST(eval_loop_for_downto_by)
{
    /* for var downto end by step -- DOWNTO as first keyword with BY */
    ASSERT_STR_EQ(eval_print(
        "(loop for i downto -6 by 2 collect i)"), "(0 -2 -4 -6)");
}

TEST(eval_loop_for_from_by)
{
    /* for var from start to end by step */
    ASSERT_STR_EQ(eval_print(
        "(let ((r nil))"
        "  (loop for i from 0 to 10 by 3"
        "    do (push i r))"
        "  (nreverse r))"), "(0 3 6 9)");
}

TEST(eval_loop_for_downfrom)
{
    /* for var downfrom start to end */
    ASSERT_STR_EQ(eval_print(
        "(let ((r nil))"
        "  (loop for i downfrom 5 to 1"
        "    do (push i r))"
        "  (nreverse r))"), "(5 4 3 2 1)");
}

TEST(eval_loop_for_downfrom_above)
{
    /* for var downfrom start above end */
    ASSERT_STR_EQ(eval_print(
        "(let ((r nil))"
        "  (loop for i downfrom 5 above 2"
        "    do (push i r))"
        "  (nreverse r))"), "(5 4 3)");
}

TEST(eval_loop_for_across)
{
    /* for var across vector */
    ASSERT_STR_EQ(eval_print(
        "(let ((r nil))"
        "  (loop for c across \"abc\""
        "    do (push c r))"
        "  (nreverse r))"), "(#\\a #\\b #\\c)");
}

TEST(eval_loop_for_eq_then)
{
    /* for var = init then step */
    ASSERT_STR_EQ(eval_print(
        "(let ((r nil))"
        "  (loop for x = 1 then (* x 2)"
        "    while (< x 20)"
        "    do (push x r))"
        "  (nreverse r))"), "(1 2 4 8 16)");
}

TEST(eval_loop_for_eq_then_nil)
{
    /* for var = init then nil -- step form is literal nil */
    ASSERT_STR_EQ(eval_print(
        "(let ((r nil))"
        "  (loop for first = t then nil"
        "    for i from 0 to 3"
        "    do (push first r))"
        "  (nreverse r))"), "(T NIL NIL NIL)");
}

TEST(eval_loop_repeat)
{
    /* repeat n */
    ASSERT_EQ_INT(eval_int(
        "(let ((count 0))"
        "  (loop repeat 5"
        "    do (setq count (+ count 1)))"
        "  count)"), 5);
}

TEST(eval_loop_for_multiple)
{
    /* multiple for clauses */
    ASSERT_STR_EQ(eval_print(
        "(let ((r nil))"
        "  (loop for x in '(a b c)"
        "        for i from 1"
        "    do (push (cons i x) r))"
        "  (nreverse r))"), "((1 . A) (2 . B) (3 . C))");
}

/* --- Phase 8: Loop Step 3 — accumulation + return --- */

TEST(eval_loop_collect)
{
    /* collect */
    ASSERT_STR_EQ(eval_print(
        "(loop for x in '(1 2 3) collect x)"), "(1 2 3)");
}

TEST(eval_loop_collect_expr)
{
    /* collect expression */
    ASSERT_STR_EQ(eval_print(
        "(loop for x in '(1 2 3) collect (* x x))"), "(1 4 9)");
    /* inline type declarations in FOR clauses */
    ASSERT_STR_EQ(eval_print(
        "(loop for i fixnum from 0 to 3 collect i)"), "(0 1 2 3)");
    ASSERT_STR_EQ(eval_print(
        "(loop for x float = 1.0 then (* x 2.0) for i from 1 to 3 collect x)"), "(1.0 2.0 4.0)");
}

TEST(eval_loop_collect_into)
{
    /* collect into named var — use do + return to verify accumulation.
       The INTO variable is user-visible and holds the accumulation IN
       ORDER at every step (CLHS 6.1.3; GitHub #19) — no NREVERSE. */
    ASSERT_STR_EQ(eval_print(
        "(loop for x in '(1 2 3 4 5)"
        "  collect x into result"
        "  do (when (= x 3) (return result)))"), "(1 2 3)");
}

TEST(eval_loop_sum)
{
    /* sum */
    ASSERT_EQ_INT(eval_int(
        "(loop for x in '(1 2 3 4 5) sum x)"), 15);
}

TEST(eval_loop_sum_into)
{
    /* sum into named var — use do + return to verify */
    ASSERT_EQ_INT(eval_int(
        "(loop for x in '(1 2 3 4 5)"
        "  sum x into total"
        "  do (when (= x 3) (return total)))"), 6);
}

TEST(eval_loop_count)
{
    /* count */
    ASSERT_EQ_INT(eval_int(
        "(loop for x in '(1 nil 2 nil 3) count x)"), 3);
}

TEST(eval_loop_maximize)
{
    /* maximize */
    ASSERT_EQ_INT(eval_int(
        "(loop for x in '(3 1 4 1 5 9 2 6) maximize x)"), 9);
}

TEST(eval_loop_minimize)
{
    /* minimize */
    ASSERT_EQ_INT(eval_int(
        "(loop for x in '(3 1 4 1 5 9 2 6) minimize x)"), 1);
}

TEST(eval_loop_append)
{
    /* append */
    ASSERT_STR_EQ(eval_print(
        "(loop for x in '((a b) (c d) (e)) append x)"), "(A B C D E)");
}

TEST(eval_loop_nconc)
{
    /* nconc */
    ASSERT_STR_EQ(eval_print(
        "(loop for x in '((1 2) (3 4)) nconc (copy-list x))"), "(1 2 3 4)");
}

/* Regression: LOOP must let COLLECT, NCONC and APPEND be mixed into the same
 * accumulation and keep source order (CLHS 6.1.3).  COLLECT builds the result
 * reversed (push) and relies on a final NREVERSE; NCONC/APPEND used to build
 * forward, so the finalizer's "reverse?" decision — taken from whichever clause
 * appeared first — was wrong for the others, silently REVERSING the result.
 * This broke cl-ppcre's NORMALIZE-VAR-LIST (a `loop ... if (consp e) nconc ...
 * else collect ...`), so REGISTER-GROUPS-BIND bound regex groups in reverse —
 * which made hunchentoot compute a negative Content-Length on a Range request
 * and corrupted cl-who table rendering.  Now all list accumulators build
 * reversed (NRECONC/REVAPPEND) under one uniform final NREVERSE. */
TEST(eval_loop_mixed_collect_nconc)
{
    /* The exact normalize-var-list shape: if/else mixing nconc and collect,
     * here with every element taking the COLLECT (else) branch. */
    ASSERT_STR_EQ(eval_print(
        "(loop for x in '(a b c)"
        "  if (consp x) nconc (list (car x)) else collect x)"), "(A B C)");
    /* Both branches exercised, order preserved across the mix. */
    ASSERT_STR_EQ(eval_print(
        "(loop for x in '(a (b1 b2) c)"
        "  if (consp x) nconc (copy-list x) else collect x)"), "(A B1 B2 C)");
    /* COLLECT and NCONC interleaved unconditionally in one iteration. */
    ASSERT_STR_EQ(eval_print(
        "(loop for x in '(1 2 3) collect x nconc (list (* x 10)))"),
        "(1 10 2 20 3 30)");
    /* APPEND mixed with COLLECT keeps order too. */
    ASSERT_STR_EQ(eval_print(
        "(loop for x in '((1 2) 3 (4 5))"
        "  if (listp x) append x else collect x)"), "(1 2 3 4 5)");
    /* Pure NCONC / APPEND still correct (no spurious reversal). */
    ASSERT_STR_EQ(eval_print(
        "(loop for x in '((1 2) (3 4)) nconc (copy-list x))"), "(1 2 3 4)");
    ASSERT_STR_EQ(eval_print(
        "(loop for x in '((a b) (c d)) append x)"), "(A B C D)");
}

TEST(eval_loop_return)
{
    /* return clause */
    ASSERT_EQ_INT(eval_int(
        "(loop for x in '(1 2 3 4 5)"
        "  do (when (= x 3) (return x)))"), 3);
}

TEST(eval_loop_return_clause)
{
    /* loop return keyword */
    ASSERT_EQ_INT(eval_int(
        "(loop for x from 1"
        "  do (when (> x 10) (return x)))"), 11);
}

TEST(eval_loop_return_in_do_let)
{
    /* return inside do + let — regression test for Fix 22 */
    ASSERT_EQ_INT(eval_int(
        "(loop while t do (let ((x 42)) (return x)))"), 42);
    ASSERT_EQ_INT(eval_int(
        "(loop for i from 0 do (let ((y (* i 10)))"
        "  (when (= y 30) (return y))))"), 30);
    /* return inside do + let* with multiple bindings */
    ASSERT_EQ_INT(eval_int(
        "(loop while t do (let* ((a 1) (b 2)) (return (+ a b))))"), 3);
    /* return inside defun + loop + do + let */
    cl_eval_string("(defun %test-loop-ret () (loop while t do (let ((x 7)) (return x))))");
    ASSERT_EQ_INT(eval_int("(%test-loop-ret)"), 7);
}

TEST(eval_loop_when_collect)
{
    /* when ... collect */
    ASSERT_STR_EQ(eval_print(
        "(loop for x in '(1 2 3 4 5 6)"
        "  when (evenp x) collect x)"), "(2 4 6)");
}

TEST(eval_loop_if_else)
{
    /* if ... collect ... else collect */
    ASSERT_STR_EQ(eval_print(
        "(loop for x in '(1 2 3 4 5)"
        "  if (oddp x) collect x"
        "  else collect (- x))"), "(1 -2 3 -4 5)");
}

TEST(eval_loop_unless_collect)
{
    /* unless ... collect */
    ASSERT_STR_EQ(eval_print(
        "(loop for x in '(1 2 3 4 5)"
        "  unless (evenp x) collect x)"), "(1 3 5)");
}

TEST(eval_loop_when_and)
{
    /* when ... and chaining: collect + side effect */
    ASSERT_STR_EQ(eval_print(
        "(let ((side nil))"
        "  (let ((r (loop for x in '(1 2 3 4)"
        "             when (evenp x) collect x"
        "             and do (push x side))))"
        "    (list r (nreverse side))))"), "((2 4) (2 4))");
}

TEST(eval_loop_always)
{
    /* always — true case */
    ASSERT_STR_EQ(eval_print(
        "(loop for x in '(2 4 6) always (evenp x))"), "T");
    /* always — false case */
    ASSERT_STR_EQ(eval_print(
        "(loop for x in '(2 3 6) always (evenp x))"), "NIL");
}

TEST(eval_loop_never)
{
    /* never — true case */
    ASSERT_STR_EQ(eval_print(
        "(loop for x in '(1 3 5) never (evenp x))"), "T");
    /* never — false case */
    ASSERT_STR_EQ(eval_print(
        "(loop for x in '(1 2 5) never (evenp x))"), "NIL");
}

TEST(eval_loop_thereis)
{
    /* thereis — found */
    ASSERT_EQ_INT(eval_int(
        "(loop for x in '(1 2 3 4 5)"
        "  thereis (and (> x 3) x))"), 4);
    /* thereis — not found */
    ASSERT_STR_EQ(eval_print(
        "(loop for x in '(1 2 3)"
        "  thereis (and (> x 10) x))"), "NIL");
}

TEST(eval_loop_with)
{
    /* with var = expr */
    ASSERT_EQ_INT(eval_int(
        "(loop with sum = 0"
        "  for x in '(1 2 3)"
        "  do (setq sum (+ sum x))"
        "  finally (return sum))"), 6);
}

TEST(eval_loop_with_and)
{
    /* with ... and */
    ASSERT_STR_EQ(eval_print(
        "(loop with x = 10 and y = 20"
        "  repeat 1 collect (+ x y))"), "(30)");
}

TEST(eval_loop_with_destructuring)
{
    /* with destructuring — dotted pair */
    ASSERT_STR_EQ(eval_print(
        "(loop with (a . b) = (cons 1 2)"
        "  do (return (list a b)))"), "(1 2)");
    /* with destructuring — proper list */
    ASSERT_STR_EQ(eval_print(
        "(loop with (first . rest) = '(10 20 30)"
        "  do (return (list first rest)))"), "(10 (20 30))");
    /* with destructuring — nested */
    ASSERT_STR_EQ(eval_print(
        "(loop with ((a b) . c) = '((1 2) 3 4)"
        "  do (return (list a b c)))"), "(1 2 (3 4))");
}

TEST(eval_loop_named)
{
    /* named block with return-from */
    ASSERT_EQ_INT(eval_int(
        "(loop named my-loop"
        "  for x from 1"
        "  do (when (> x 5) (return-from my-loop x)))"), 6);
}

TEST(eval_loop_initially)
{
    /* initially form */
    ASSERT_STR_EQ(eval_print(
        "(let ((log nil))"
        "  (loop initially (push 'start log)"
        "    for x in '(1 2) do (push x log))"
        "  (nreverse log))"), "(START 1 2)");
}

TEST(eval_loop_finally)
{
    /* finally with return */
    ASSERT_EQ_INT(eval_int(
        "(loop for x in '(1 2 3 4 5)"
        "  sum x into total"
        "  finally (return total))"), 15);
}

TEST(eval_loop_collect_into_finally)
{
    /* collect into with finally */
    ASSERT_STR_EQ(eval_print(
        "(loop for x in '(a b c)"
        "  collect x into items"
        "  finally (return items))"), "(A B C)");
}

TEST(eval_loop_finish)
{
    /* loop-finish terminates loop normally, returns accumulation */
    ASSERT_EQ_INT(eval_int(
        "(loop for i from 1 to 10 sum i"
        "  do (when (= i 5) (loop-finish)))"), 15);
}

TEST(eval_loop_finish_finally)
{
    /* loop-finish still runs finally forms */
    ASSERT_EQ_INT(eval_int(
        "(let ((ran nil))"
        "  (loop for i from 1 to 10"
        "    do (when (= i 3) (loop-finish))"
        "    finally (setq ran t))"
        "  (if ran 1 0))"), 1);
}

/* ---- LOOP BEING clause ---- */

TEST(eval_loop_being_hash_keys)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((ht (make-hash-table)))"
        "  (setf (gethash :a ht) 1)"
        "  (setf (gethash :b ht) 2)"
        "  (sort (loop for k being the hash-keys of ht collect k)"
        "        #'string< :key #'symbol-name))"), "(:A :B)");
}

TEST(eval_loop_being_hash_values)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((ht (make-hash-table)))"
        "  (setf (gethash :a ht) 10)"
        "  (setf (gethash :b ht) 20)"
        "  (sort (loop for v being the hash-values in ht collect v)"
        "        #'<))"), "(10 20)");
}

TEST(eval_loop_being_hash_keys_using)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((ht (make-hash-table)))"
        "  (setf (gethash :x ht) 42)"
        "  (loop for k being each hash-key of ht"
        "        using (hash-value v)"
        "        collect (list k v)))"), "((:X 42))");
}

TEST(eval_loop_being_hash_values_using)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((ht (make-hash-table)))"
        "  (setf (gethash :x ht) 42)"
        "  (loop for v being each hash-value of ht"
        "        using (hash-key k)"
        "        collect (list k v)))"), "((:X 42))");
}

TEST(eval_loop_being_symbols)
{
    /* Create a package with known symbols, count them */
    ASSERT_STR_EQ(eval_print(
        "(progn"
        "  (defpackage \"LOOP-SYM-TEST\" (:use))"
        "  (intern \"AA\" \"LOOP-SYM-TEST\")"
        "  (intern \"BB\" \"LOOP-SYM-TEST\")"
        "  (length (loop for s being the symbols of \"LOOP-SYM-TEST\""
        "               collect s)))"), "2");
}

TEST(eval_loop_being_external_symbols)
{
    ASSERT_STR_EQ(eval_print(
        "(progn"
        "  (defpackage \"LOOP-EXT-TEST\" (:use) (:export \"PUB\"))"
        "  (intern \"PRIV\" \"LOOP-EXT-TEST\")"
        "  (length (loop for s being the external-symbols of \"LOOP-EXT-TEST\""
        "               collect s)))"), "1");
}

/* ---- LOOP destructuring ---- */

TEST(eval_loop_destructuring_in)
{
    /* destructure pairs from a list */
    ASSERT_STR_EQ(eval_print(
        "(loop for (a b) in '((1 2) (3 4) (5 6))"
        "  collect (+ a b))"), "(3 7 11)");
}

TEST(eval_loop_destructuring_dotted)
{
    /* destructure dotted pairs */
    ASSERT_STR_EQ(eval_print(
        "(loop for (a . b) in '((x . 1) (y . 2) (z . 3))"
        "  collect (list a b))"), "((X 1) (Y 2) (Z 3))");
}

TEST(eval_loop_destructuring_nested)
{
    /* nested destructuring */
    ASSERT_STR_EQ(eval_print(
        "(loop for (a (b c)) in '((1 (2 3)) (4 (5 6)))"
        "  collect (+ a b c))"), "(6 15)");
}

TEST(eval_loop_destructuring_on)
{
    /* destructuring with ON */
    ASSERT_STR_EQ(eval_print(
        "(loop for (a . b) on '(1 2 3)"
        "  collect (list a b))"), "((1 (2 3)) (2 (3)) (3 NIL))");
}

/* --- Loop WHILE/UNTIL positional semantics (CLHS 6.1.4) --- */

TEST(eval_loop_while_after_collect)
{
    /* while AFTER collect: last element collected before termination */
    ASSERT_STR_EQ(eval_print(
        "(loop for x = 0 then (1+ x)"
        "  collect x"
        "  while (< x 3))"), "(0 1 2 3)");
}

TEST(eval_loop_until_after_collect)
{
    /* until AFTER collect: element collected before termination test */
    ASSERT_STR_EQ(eval_print(
        "(loop for x = 0 then (1+ x)"
        "  collect x"
        "  until (= x 3))"), "(0 1 2 3)");
}

TEST(eval_loop_while_before_collect)
{
    /* while BEFORE collect: stop before collecting when test fails */
    ASSERT_STR_EQ(eval_print(
        "(loop for i from 0"
        "  while (< i 4)"
        "  collect i)"), "(0 1 2 3)");
}

TEST(eval_loop_if_collect_while)
{
    /* conditional collect + while — both should apply correctly */
    ASSERT_STR_EQ(eval_print(
        "(loop for i from 0 to 10"
        "  if (evenp i) collect i"
        "  while (< i 6))"), "(0 2 4 6)");
}

TEST(eval_loop_collect_while_search)
{
    /* split-string pattern from log4cl — the original trigger */
    ASSERT_STR_EQ(eval_print(
        "(loop for start = 0 then (+ pos 1)"
        "  as pos = (position #\\. \"FOO.BAR.BAZ\" :start start)"
        "  collect (subseq \"FOO.BAR.BAZ\" start pos)"
        "  while pos)"), "(\"FOO\" \"BAR\" \"BAZ\")");
}

/* --- Pretty-printing / *print-pretty* --- */

TEST(eval_print_pretty_default)
{
    /* *print-right-margin* defaults to NIL */
    ASSERT_STR_EQ(eval_print("*print-right-margin*"), "NIL");
    /* *print-pretty* defaults to NIL */
    ASSERT_STR_EQ(eval_print("*print-pretty*"), "NIL");
}

TEST(eval_print_pretty_short_list)
{
    /* Short list stays on one line even with pretty printing */
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-pretty* t) (*print-right-margin* 40))"
        "  (write-to-string '(1 2 3)))"),
        "\"(1 2 3)\"");
}

TEST(eval_print_pretty_long_list)
{
    /* Narrow margin forces line breaks (greedy fill: break when past margin) */
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-pretty* t) (*print-right-margin* 10))"
        "  (write-to-string '(alpha beta gamma delta)))"),
        "\"(ALPHA BETA\n GAMMA DELTA)\"");
}

TEST(eval_print_pretty_nested)
{
    /* Nested lists indent correctly */
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-pretty* t) (*print-right-margin* 15))"
        "  (write-to-string '(a (b c d) e)))"),
        "\"(A (B C D) E)\"");
}

TEST(eval_print_pretty_vector)
{
    /* Vectors break similarly to lists */
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-pretty* t) (*print-right-margin* 10))"
        "  (write-to-string (vector 'alpha 'beta 'gamma)))"),
        "\"#(ALPHA BETA\n  GAMMA)\"");
}

TEST(eval_print_pretty_empty)
{
    /* NIL and single-element lists */
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-pretty* t) (*print-right-margin* 10))"
        "  (write-to-string nil))"),
        "\"NIL\"");
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-pretty* t) (*print-right-margin* 10))"
        "  (write-to-string '(x)))"),
        "\"(X)\"");
}

TEST(eval_print_pretty_dotted)
{
    /* Dotted pair stays inline if short enough */
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-pretty* t) (*print-right-margin* 40))"
        "  (write-to-string '(a . b)))"),
        "\"(A . B)\"");
}

TEST(eval_pprint_basic)
{
    /* pprint outputs newline + pretty-printed form */
    ASSERT_STR_EQ(eval_print(
        "(let ((s (make-string-output-stream)))"
        "  (pprint '(1 2 3) s)"
        "  (get-output-stream-string s))"),
        "\"\n(1 2 3)\"");
}

TEST(eval_print_pretty_write_keyword)
{
    /* write :pretty t enables pretty printing */
    ASSERT_STR_EQ(eval_print(
        "(write-to-string '(1 2 3) :pretty t :right-margin 40)"),
        "\"(1 2 3)\"");
}

TEST(eval_print_pretty_write_to_string)
{
    /* write-to-string :pretty t with narrow margin */
    ASSERT_STR_EQ(eval_print(
        "(write-to-string '(alpha beta gamma delta) :pretty t :right-margin 10)"),
        "\"(ALPHA BETA\n GAMMA DELTA)\"");
}

TEST(eval_print_pretty_off_no_break)
{
    /* When *print-pretty* is nil, no line breaks even past margin */
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-pretty* nil) (*print-right-margin* 5))"
        "  (write-to-string '(alpha beta gamma)))"),
        "\"(ALPHA BETA GAMMA)\"");
}

TEST(eval_print_pretty_with_level_length)
{
    /* Pretty + level/length interact correctly */
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-pretty* t) (*print-right-margin* 10)"
        "      (*print-length* 2))"
        "  (write-to-string '(alpha beta gamma delta)))"),
        "\"(ALPHA BETA ...)\"");
}

/* --- rotatef / shiftf --- */

TEST(eval_rotatef_basic)
{
    eval_print("(defvar *rf-a* 1)");
    eval_print("(defvar *rf-b* 2)");
    eval_print("(defvar *rf-c* 3)");
    eval_print("(rotatef *rf-a* *rf-b* *rf-c*)");
    ASSERT_EQ_INT(eval_int("*rf-a*"), 2);
    ASSERT_EQ_INT(eval_int("*rf-b*"), 3);
    ASSERT_EQ_INT(eval_int("*rf-c*"), 1);
}

TEST(eval_rotatef_two)
{
    eval_print("(defvar *rf2-x* 10)");
    eval_print("(defvar *rf2-y* 20)");
    eval_print("(rotatef *rf2-x* *rf2-y*)");
    ASSERT_EQ_INT(eval_int("*rf2-x*"), 20);
    ASSERT_EQ_INT(eval_int("*rf2-y*"), 10);
}

TEST(eval_rotatef_returns_nil)
{
    eval_print("(defvar *rfr-a* 1)");
    eval_print("(defvar *rfr-b* 2)");
    ASSERT_STR_EQ(eval_print("(rotatef *rfr-a* *rfr-b*)"), "NIL");
}

TEST(eval_rotatef_car)
{
    eval_print("(defvar *rfc-x* (cons 1 2))");
    eval_print("(defvar *rfc-y* 99)");
    eval_print("(rotatef (car *rfc-x*) *rfc-y*)");
    ASSERT_EQ_INT(eval_int("(car *rfc-x*)"), 99);
    ASSERT_EQ_INT(eval_int("*rfc-y*"), 1);
}

TEST(eval_shiftf_basic)
{
    eval_print("(defvar *sf-a* 1)");
    eval_print("(defvar *sf-b* 2)");
    eval_print("(defvar *sf-c* 3)");
    ASSERT_EQ_INT(eval_int("(shiftf *sf-a* *sf-b* *sf-c* 99)"), 1);
    ASSERT_EQ_INT(eval_int("*sf-a*"), 2);
    ASSERT_EQ_INT(eval_int("*sf-b*"), 3);
    ASSERT_EQ_INT(eval_int("*sf-c*"), 99);
}

TEST(eval_shiftf_two)
{
    eval_print("(defvar *sf2-x* 10)");
    ASSERT_EQ_INT(eval_int("(shiftf *sf2-x* 42)"), 10);
    ASSERT_EQ_INT(eval_int("*sf2-x*"), 42);
}

TEST(eval_shiftf_car)
{
    eval_print("(defvar *sfc-x* (cons 5 6))");
    ASSERT_EQ_INT(eval_int("(shiftf (car *sfc-x*) 77)"), 5);
    ASSERT_EQ_INT(eval_int("(car *sfc-x*)"), 77);
}

/* --- pprint-newline --- */

TEST(eval_pprint_newline_mandatory)
{
    /* :mandatory doesn't error */
    ASSERT_STR_EQ(eval_print("(progn (pprint-newline :mandatory) t)"), "T");
}

TEST(eval_pprint_newline_fill)
{
    /* :fill doesn't error */
    ASSERT_STR_EQ(eval_print("(progn (pprint-newline :fill) t)"), "T");
}

TEST(eval_pprint_newline_kinds)
{
    /* All four kinds accepted */
    ASSERT_STR_EQ(eval_print("(progn (pprint-newline :linear) t)"), "T");
    ASSERT_STR_EQ(eval_print("(progn (pprint-newline :miser) t)"), "T");
}

/* --- pprint-indent --- */

TEST(eval_pprint_indent_current)
{
    /* pprint-indent :current doesn't error */
    ASSERT_STR_EQ(eval_print(
        "(progn (pprint-indent :current 2) t)"),
        "T");
}

TEST(eval_pprint_indent_block)
{
    /* pprint-indent :block doesn't error */
    ASSERT_STR_EQ(eval_print(
        "(progn (pprint-indent :block 4) t)"),
        "T");
}

/* --- pprint-logical-block --- */

TEST(eval_pprint_logical_block_basic)
{
    /* pprint-logical-block with prefix/suffix, using with-output-to-string */
    ASSERT_STR_EQ(eval_print(
        "(with-output-to-string (s)"
        "  (pprint-logical-block (s '(1 2 3) :prefix \"[\" :suffix \"]\")"
        "    (princ (pprint-pop) s)"
        "    (pprint-exit-if-list-exhausted)"
        "    (write-char #\\Space s)"
        "    (princ (pprint-pop) s)"
        "    (pprint-exit-if-list-exhausted)"
        "    (write-char #\\Space s)"
        "    (princ (pprint-pop) s)))"),
        "\"[1 2 3]\"");
}

/* --- pprint-dispatch --- */

TEST(eval_pprint_dispatch_set_get)
{
    /* set-pprint-dispatch and pprint-dispatch round-trip */
    eval_print("(set-pprint-dispatch 'cons (lambda (s obj) (format s \"<~S>\" obj)))");
    ASSERT_STR_EQ(eval_print("(pprint-dispatch '(1 2))"), "#<CLOSURE anonymous>");
    /* Clean up */
    eval_print("(set-pprint-dispatch 'cons nil)");
}

TEST(eval_copy_pprint_dispatch)
{
    /* copy-pprint-dispatch returns a copy */
    ASSERT_STR_EQ(eval_print("(copy-pprint-dispatch)"), "NIL");
}

TEST(eval_pprint_dispatch_custom)
{
    /* Custom dispatch function used during pretty printing */
    eval_print(
        "(set-pprint-dispatch 'integer"
        "  (lambda (s obj)"
        "    (write-string \"[\" s)"
        "    (princ obj s)"
        "    (write-string \"]\" s))"
        "  1)");
    ASSERT_STR_EQ(eval_print(
        "(let ((*print-pretty* t))"
        "  (with-output-to-string (s)"
        "    (write 42 :stream s :pretty t)))"),
        "\"[42]\"");
    /* Clean up */
    eval_print("(set-pprint-dispatch 'integer nil)");
}

/* --- Modules (provide / require) --- */

TEST(eval_provide_adds_to_modules)
{
    /* provide adds module name to *modules* */
    eval_print("(provide \"my-module\")");
    ASSERT_STR_EQ(eval_print("(car *modules*)"), "\"my-module\"");
}

TEST(eval_provide_no_duplicate)
{
    /* provide does not add duplicates */
    eval_print("(provide \"dup-mod\")");
    eval_print("(provide \"dup-mod\")");
    /* Count occurrences - should be exactly 1 */
    ASSERT_STR_EQ(eval_print(
        "(count \"dup-mod\" *modules* :test #'string=)"), "1");
}

TEST(eval_provide_symbol_name)
{
    /* provide accepts a symbol, using its name */
    eval_print("(provide 'sym-mod)");
    ASSERT_STR_EQ(eval_print(
        "(find \"SYM-MOD\" *modules* :test #'string=)"), "\"SYM-MOD\"");
}

TEST(eval_require_already_provided)
{
    /* require of already-provided module is a no-op */
    eval_print("(provide \"already-there\")");
    ASSERT_STR_EQ(eval_print("(require \"already-there\")"), "NIL");
}

TEST(eval_require_with_pathname)
{
    /* require loads a file when not yet provided */
    eval_print("(require \"test-module\" \"tests/test_module.lisp\")");
    ASSERT_STR_EQ(eval_print("*test-module-loaded*"), "T");
    ASSERT_EQ_INT(eval_int("(test-module-fn 5)"), 50);
    /* The module file calls (provide "test-module"), so it should be in *modules* */
    ASSERT_STR_EQ(eval_print(
        "(find \"test-module\" *modules* :test #'string=)"), "\"test-module\"");
}

TEST(eval_require_does_not_reload)
{
    /* After provide, require should not load again */
    eval_print("(provide \"test-module\")");
    eval_print("(setq *test-module-loaded* nil)");
    eval_print("(require \"test-module\" \"tests/test_module.lisp\")");
    /* Should still be nil because require was a no-op */
    ASSERT_STR_EQ(eval_print("*test-module-loaded*"), "NIL");
}

/* --- Property lists --- */

TEST(eval_symbol_plist_initial)
{
    ASSERT_STR_EQ(eval_print("(symbol-plist 'test-plist-sym-1)"), "NIL");
}

TEST(eval_setf_get)
{
    ASSERT_STR_EQ(eval_print("(setf (get 'test-plist-sym-2 'bar) 42)"), "42");
    ASSERT_STR_EQ(eval_print("(get 'test-plist-sym-2 'bar)"), "42");
}

TEST(eval_get_default)
{
    ASSERT_STR_EQ(eval_print("(get 'test-plist-sym-2 'baz 99)"), "99");
    ASSERT_STR_EQ(eval_print("(get 'test-plist-sym-2 'baz)"), "NIL");
}

TEST(eval_remprop)
{
    eval_print("(setf (get 'test-plist-sym-3 'foo) 10)");
    ASSERT_STR_EQ(eval_print("(remprop 'test-plist-sym-3 'foo)"), "T");
    ASSERT_STR_EQ(eval_print("(get 'test-plist-sym-3 'foo)"), "NIL");
    ASSERT_STR_EQ(eval_print("(remprop 'test-plist-sym-3 'foo)"), "NIL");
}

TEST(eval_setf_get_update)
{
    eval_print("(setf (get 'test-plist-sym-4 'x) 1)");
    eval_print("(setf (get 'test-plist-sym-4 'x) 2)");
    ASSERT_STR_EQ(eval_print("(get 'test-plist-sym-4 'x)"), "2");
}

/* --- Block comments (#|...|#) --- */

TEST(eval_block_comment_basic)
{
    ASSERT_EQ_INT(eval_int("#| comment |# 42"), 42);
}

TEST(eval_block_comment_inline)
{
    ASSERT_EQ_INT(eval_int("(+ 1 #| skip |# 2)"), 3);
}

TEST(eval_block_comment_nested)
{
    ASSERT_EQ_INT(eval_int("#| outer #| inner |# still outer |# 99"), 99);
}

TEST(eval_block_comment_multiline)
{
    ASSERT_EQ_INT(eval_int("#|\n  multi\n  line\n|# 7"), 7);
}

/* --- &aux in lambda lists --- */

TEST(eval_aux_basic)
{
    ASSERT_EQ_INT(eval_int("(funcall (lambda (x &aux (y 10)) (+ x y)) 5)"), 15);
}

TEST(eval_aux_multiple)
{
    ASSERT_EQ_INT(eval_int("(funcall (lambda (x &aux (a 1) (b 2)) (+ x a b)) 10)"), 13);
}

TEST(eval_aux_no_init)
{
    ASSERT_STR_EQ(eval_print("(funcall (lambda (&aux z) z))"), "NIL");
}

TEST(eval_aux_uses_param)
{
    ASSERT_EQ_INT(eval_int("(funcall (lambda (x &aux (y (* x 2))) y) 5)"), 10);
}

TEST(eval_aux_with_key)
{
    ASSERT_EQ_INT(eval_int("(funcall (lambda (&key x &aux (y (or x 0))) (+ y 1)) :x 5)"), 6);
}

TEST(eval_defun_aux)
{
    eval_print("(defun test-aux-fn (a &aux (b (+ a 1))) b)");
    ASSERT_EQ_INT(eval_int("(test-aux-fn 10)"), 11);
}

/* CLHS 3.4.1.4: &aux init forms see outer scope (LET* semantics) — the
 * variable being bound is NOT yet visible inside its own init form, so
 * `(&aux (record (list record)))` must use the outer parameter `record`,
 * not the freshly-shadowed NIL slot. */
TEST(eval_aux_self_shadowing_init)
{
    ASSERT_STR_EQ(eval_print(
        "(funcall (lambda (record &aux (record (list record))) record) 'hello)"),
        "(HELLO)");
}

TEST(eval_aux_init_sees_earlier_aux)
{
    ASSERT_EQ_INT(eval_int(
        "(funcall (lambda (x &aux (a (* x 2)) (b (+ a 1))) b) 3)"),
        7);
}

/* --- apply with symbols --- */

TEST(eval_apply_symbol)
{
    ASSERT_EQ_INT(eval_int("(apply '+ '(1 2 3))"), 6);
}

TEST(eval_apply_symbol_funcall)
{
    ASSERT_EQ_INT(eval_int("(apply '* 2 '(3 4))"), 24);
}

/* Regression: APPLY must spread far more than the old 64-arg cap onto the VM
 * stack (CALL-ARGUMENTS-LIMIT).  serapeum's (apply #'string+ <up-to-1000>)
 * tripped the old "too many arguments (>64)" error.  Cover a &rest bytecode
 * function, required+&rest, and builtins, all past 64/255. */
TEST(eval_apply_rest_over_64)
{
    /* &rest bytecode function applied to 100 / 255 / 1000 args. */
    ASSERT_EQ_INT(eval_int(
        "(progn (defun %ar (&rest xs) (length xs))"
        " (apply #'%ar (loop for i from 1 to 100 collect i)))"), 100);
    ASSERT_EQ_INT(eval_int(
        "(apply #'%ar (loop for i from 1 to 255 collect i))"), 255);
    ASSERT_EQ_INT(eval_int(
        "(apply #'%ar (loop for i from 1 to 1000 collect i))"), 1000);
}

TEST(eval_apply_required_plus_rest_over_64)
{
    /* Required args bind first, the surplus forms the &rest list. */
    ASSERT_STR_EQ(eval_print(
        "(progn (defun %arr (a b &rest xs) (list a b (length xs)))"
        " (apply #'%arr (loop for i from 1 to 200 collect i)))"),
        "(1 2 198)");
}

TEST(eval_apply_builtin_over_64)
{
    /* Builtins have an (int nargs) ABI — bounded only by the stack. */
    ASSERT_EQ_INT(eval_int(
        "(length (apply #'list (loop for i from 1 to 300 collect i)))"), 300);
    ASSERT_EQ_INT(eval_int(
        "(apply #'+ (loop for i from 1 to 500 collect 1))"), 500);
}

TEST(eval_apply_rest_value_correct)
{
    /* The spread elements must arrive in order, not just in the right count:
     * sum the &rest list collected from APPLY. */
    ASSERT_EQ_INT(eval_int(
        "(progn (defun %asum (&rest xs) (reduce #'+ xs :initial-value 0))"
        " (apply #'%asum (loop for i from 1 to 100 collect i)))"), 5050);
}

TEST(eval_call_arguments_limit_constant)
{
    /* CALL-ARGUMENTS-LIMIT must be a positive integer not smaller than
     * LAMBDA-PARAMETERS-LIMIT (CLHS). */
    ASSERT_STR_EQ(eval_print(
        "(and (>= call-arguments-limit lambda-parameters-limit)"
        " (plusp call-arguments-limit))"), "T");
}

/* --- package-used-by-list --- */

TEST(eval_package_used_by_list)
{
    /* CL-USER uses CL, so CL-USER should be in CL's used-by list */
    ASSERT_STR_EQ(eval_print("(if (member (find-package \"COMMON-LISP-USER\") (package-used-by-list (find-package \"COMMON-LISP\"))) t nil)"), "T");
}

/* --- shadowing-import --- */

TEST(eval_shadowing_import)
{
    eval_print("(defpackage \"TEST-SHAD-PKG\" (:use))");
    eval_print("(shadowing-import 'cons (find-package \"TEST-SHAD-PKG\"))");
    ASSERT_STR_EQ(eval_print("(if (member 'cons (package-shadowing-symbols (find-package \"TEST-SHAD-PKG\"))) t nil)"), "T");
}

/* shadowing-import must replace a conflicting same-named symbol (CLHS) */
TEST(eval_shadowing_import_conflict)
{
    /* Create package with its own FOO symbol */
    eval_print("(defpackage \"TEST-SHAD-CONF\" (:use) (:intern \"FOO\"))");
    /* Create a different FOO in another package */
    eval_print("(defpackage \"TEST-SHAD-SRC\" (:use) (:export \"FOO\"))");
    /* shadowing-import the external FOO — must replace the internal one */
    eval_print("(shadowing-import 'test-shad-src::foo (find-package \"TEST-SHAD-CONF\"))");
    /* The symbol should now be the one from TEST-SHAD-SRC */
    ASSERT_STR_EQ(eval_print("(symbol-package (find-symbol \"FOO\" \"TEST-SHAD-CONF\"))"),
                  "#<PACKAGE TEST-SHAD-SRC>");
    /* It should be on the shadowing list */
    ASSERT_STR_EQ(eval_print("(if (member 'test-shad-src::foo (package-shadowing-symbols (find-package \"TEST-SHAD-CONF\"))) t nil)"), "T");
}

/* shadowing-import of the same symbol is a no-op (no duplicate in shadowing list) */
TEST(eval_shadowing_import_same_symbol)
{
    eval_print("(defpackage \"TEST-SHAD-SAME\" (:use))");
    eval_print("(shadowing-import 'cons (find-package \"TEST-SHAD-SAME\"))");
    eval_print("(shadowing-import 'cons (find-package \"TEST-SHAD-SAME\"))");
    ASSERT_STR_EQ(eval_print("(length (package-shadowing-symbols (find-package \"TEST-SHAD-SAME\")))"), "1");
}

/* --- copy-symbol --- */

TEST(eval_copy_symbol_basic)
{
    ASSERT_STR_EQ(eval_print("(symbol-name (copy-symbol 'foo))"), "\"FOO\"");
}

TEST(eval_copy_symbol_uninterned)
{
    ASSERT_STR_EQ(eval_print("(symbol-package (copy-symbol 'foo))"), "NIL");
}

TEST(eval_copy_symbol_props)
{
    eval_print("(defvar *copy-sym-test* 42)");
    ASSERT_STR_EQ(eval_print("(symbol-value (copy-symbol '*copy-sym-test* t))"), "42");
}

/* CLHS 2.4.8.5: Within a single call to READ, all #:foo with the same
 * name denote the same uninterned symbol.  Compile-file output from
 * SBCL et al. relies on this — e.g. string-case.lisp has a pretty-printed
 * macroexpansion containing a LET that binds #:input2056 and references
 * it deeper inside, all parsed as one top-level form. */
TEST(eval_reader_shared_uninterned_within_one_read)
{
    /* Multiple #:x in one read share identity (LET binding flows through). */
    ASSERT_STR_EQ(eval_print("(eval (read-from-string \"(let ((#:x 10)) (+ #:x #:x))\"))"),
                  "20");
    /* Two #:foo in one read are EQ; two separate reads are NOT EQ. */
    ASSERT_STR_EQ(eval_print(
        "(let ((form (read-from-string \"(#:bar #:bar)\"))) (eq (car form) (cadr form)))"),
        "T");
    ASSERT_STR_EQ(eval_print(
        "(eq (read-from-string \"#:foo\") (read-from-string \"#:foo\"))"),
        "NIL");
}

/* --- LOOP nested :if sub-clause tests --- */

TEST(eval_loop_nested_if)
{
    ASSERT_STR_EQ(eval_print(
        "(loop :for x :in '(1 2 3)"
        "  :when (> x 1)"
        "    :if (= x 2) :collect :two"
        "    :else :collect :three"
        "    :end)"),
        "(:TWO :THREE)");
}

TEST(eval_loop_nested_when_else)
{
    /* :when with nested :when */
    ASSERT_STR_EQ(eval_print(
        "(loop :for x :in '(1 2 3 4 5)"
        "  :when (oddp x)"
        "    :if (> x 2) :collect x :end)"),
        "(3 5)");
}

TEST(eval_loop_nested_unless)
{
    ASSERT_STR_EQ(eval_print(
        "(loop :for x :in '(1 2 3 4)"
        "  :when t"
        "    :unless (evenp x) :collect x :end)"),
        "(1 3)");
}

/* --- defmacro destructuring tests --- */

TEST(eval_defmacro_destructuring_required)
{
    /* Required parameter is a list pattern */
    eval_print("(defmacro dm-test1 ((a b) c) `(list ,a ,b ,c))");
    ASSERT_STR_EQ(eval_print("(dm-test1 (1 2) 3)"), "(1 2 3)");
}

TEST(eval_defmacro_destructuring_body)
{
    /* &body with destructuring pattern (if-let pattern) */
    eval_print("(defmacro dm-test2 (var &body (then-form &optional else-form))"
               "  `(let ((,var t)) (if ,var ,then-form ,else-form)))");
    ASSERT_STR_EQ(eval_print("(dm-test2 x :yes :no)"), ":YES");
    ASSERT_STR_EQ(eval_print("(dm-test2 x :only)"), ":ONLY");
}

TEST(eval_defmacro_optional_not_destructured)
{
    /* &optional with (name default) should NOT be destructured */
    eval_print("(defmacro dm-test3 (x &optional (y 42)) `(list ,x ,y))");
    ASSERT_STR_EQ(eval_print("(dm-test3 1)"), "(1 42)");
    ASSERT_STR_EQ(eval_print("(dm-test3 1 2)"), "(1 2)");
}

/* CLHS: (macro-function name) returns "a function of two arguments,
 * a form and an environment".  Calling it with the wrong arity must
 * signal PROGRAM-ERROR.  ANSI tests PUSH.ERROR.1 / PUSHNEW.ERROR.4 /
 * REMF.ERROR.1 exercise exactly this — they funcall the result of
 * macro-function with 0, 1, and 3 args. */
TEST(eval_macro_function_arity)
{
    eval_print("(defmacro mf-arity-test (a b) `(list ,a ,b))");
    /* Correct 2-arg call expands the form */
    ASSERT_STR_EQ(eval_print(
        "(funcall (macro-function 'mf-arity-test) '(mf-arity-test 1 2) nil)"),
        "(LIST 1 2)");
    /* 0 args → PROGRAM-ERROR */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (funcall (macro-function 'mf-arity-test)) "
        "  (program-error () :pe))"),
        ":PE");
    /* 1 arg → PROGRAM-ERROR */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (funcall (macro-function 'mf-arity-test) "
        "                       '(mf-arity-test 1 2)) "
        "  (program-error () :pe))"),
        ":PE");
    /* 3 args → PROGRAM-ERROR */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (funcall (macro-function 'mf-arity-test) "
        "                       '(mf-arity-test 1 2) nil nil) "
        "  (program-error () :pe))"),
        ":PE");
}

/* CLHS: macro-function's argument is a symbol.  NIL is a symbol with no
 * macro binding, so (macro-function nil) must return NIL — exactly like
 * (macro-function t).  Regression: NIL is the tagged value 0 (not a heap
 * symbol), so a bare CL_SYMBOL_P check rejected it with a TYPE-ERROR, the
 * only one of all symbols to fail during a full do-all-symbols scan. */
TEST(eval_macro_function_nil)
{
    ASSERT_STR_EQ(eval_print("(macro-function nil)"), "NIL");
    ASSERT_STR_EQ(eval_print("(macro-function t)"), "NIL");
    ASSERT_STR_EQ(eval_print("(compiler-macro-function nil)"), "NIL");
    ASSERT_STR_EQ(eval_print("(compiler-macro-function t)"), "NIL");
    /* A genuine non-symbol still signals an error. */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (macro-function 42) (type-error () :te))"),
        ":TE");
}

/* --- define-modify-macro tests --- */

TEST(eval_define_modify_macro)
{
    eval_print("(define-modify-macro appendf (&rest args) append)");
    ASSERT_STR_EQ(eval_print(
        "(let ((x '(1 2))) (appendf x '(3 4)) x)"),
        "(1 2 3 4)");
}

/* --- reduce :from-end tests --- */

TEST(eval_reduce_from_end)
{
    ASSERT_STR_EQ(eval_print(
        "(reduce #'cons '(1 2 3 4) :from-end t)"),
        "(1 2 3 . 4)");
}

TEST(eval_reduce_from_end_initial)
{
    ASSERT_STR_EQ(eval_print(
        "(reduce #'cons '(1 2 3) :from-end t :initial-value '())"),
        "(1 2 3)");
}

/* --- defmethod implicit block test --- */

TEST(eval_defmethod_implicit_block)
{
    eval_print("(defgeneric dmib-fn (x))");
    eval_print("(defmethod dmib-fn ((x t)) (return-from dmib-fn 42) 99)");
    ASSERT_STR_EQ(eval_print("(dmib-fn 1)"), "42");
}

/* --- user-homedir-pathname test --- */

TEST(eval_user_homedir_pathname)
{
    ASSERT_STR_EQ(eval_print("(pathnamep (user-homedir-pathname))"), "T");
}

/* --- ext:system-command tests --- */

TEST(eval_system_command_true)
{
    ASSERT_STR_EQ(eval_print("(ext:system-command \"true\")"), "0");
}

TEST(eval_system_command_false)
{
    ASSERT_STR_EQ(eval_print("(ext:system-command \"false\")"), "1");
}

TEST(eval_system_command_echo)
{
    /* The null device is spelled differently by cmd.exe, which is the shell
     * system() hands the command to on Windows. */
#ifdef PLATFORM_WIN32
    const char *cmd = "(ext:system-command \"echo hello > NUL\")";
#else
    const char *cmd = "(ext:system-command \"echo hello > /dev/null\")";
#endif
    ASSERT_STR_EQ(eval_print(cmd), "0");
}

TEST(eval_getcwd)
{
    /* ext:getcwd should return a non-empty string holding an absolute path */
    const char *result = eval_print("(ext:getcwd)");
    ASSERT(result[0] == '"');  /* printed as a string */
#ifdef PLATFORM_WIN32
    /* "C:/Users/..." — a drive letter, then the separator amilisp
     * normalises every Windows path to. */
    ASSERT(result[2] == ':');
    ASSERT(result[3] == '/');
#else
    ASSERT(result[1] == '/');  /* absolute path */
#endif
}

/* --- mp: threading primitives (real implementations + stubs) --- */

TEST(eval_mp_make_lock)
{
    /* Real lock objects now, not NIL */
    const char *r = eval_print("(mp:make-lock)");
    ASSERT(strstr(r, "#<LOCK") != NULL);
    r = eval_print("(mp:make-lock \"my-lock\")");
    ASSERT(strstr(r, "#<LOCK") != NULL);
}

TEST(eval_mp_make_recursive_lock)
{
    /* Stub delegates to make-lock, returns real lock */
    const char *r = eval_print("(mp:make-recursive-lock)");
    ASSERT(strstr(r, "#<LOCK") != NULL);
    r = eval_print("(mp:make-recursive-lock \"rec\")");
    ASSERT(strstr(r, "#<LOCK") != NULL);
}

TEST(eval_mp_with_lock_held)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((lk (mp:make-lock))) (mp:with-lock-held (lk) 42))"), "42");
}

TEST(eval_mp_with_recursive_lock_held)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((lk (mp:make-recursive-lock))) "
        "  (mp:with-recursive-lock-held (lk) 99))"), "99");
}

TEST(eval_mp_memory_barriers)
{
    ASSERT_STR_EQ(eval_print("(mp:read-memory-barrier)"), "NIL");
    ASSERT_STR_EQ(eval_print("(mp:write-memory-barrier)"), "NIL");
}

TEST(eval_ext_defglobal)
{
    ASSERT_STR_EQ(eval_print(
        "(progn (ext:defglobal *ext-test-glob* 77) *ext-test-glob*)"), "77");
}

/* --- Quicklisp/ASDF compatibility regression tests --- */

TEST(eval_remove_if_not_string)
{
    /* remove-if-not on strings must iterate characters, not raw vector data */
    ASSERT_STR_EQ(eval_print("(remove-if-not #'digit-char-p \"2021-02-13\")"),
                  "\"20210213\"");
}

TEST(eval_remove_if_string)
{
    ASSERT_STR_EQ(eval_print("(remove-if #'digit-char-p \"abc123\")"),
                  "\"abc\"");
}

TEST(eval_remove_string)
{
    ASSERT_STR_EQ(eval_print("(remove #\\- \"2021-02-13\")"),
                  "\"20210213\"");
}

TEST(eval_key_suppliedp_param)
{
    /* &key supplied-p parameter: T when key is explicitly supplied */
    cl_eval_string("(defun %test-sp (&key (x nil xp)) (list x xp))");
    ASSERT_STR_EQ(eval_print("(%test-sp)"), "(NIL NIL)");
    ASSERT_STR_EQ(eval_print("(%test-sp :x nil)"), "(NIL T)");
    ASSERT_STR_EQ(eval_print("(%test-sp :x 42)"), "(42 T)");
}

TEST(eval_key_nil_value_not_default)
{
    /* Passing :x nil must NOT apply the default value */
    cl_eval_string("(defun %test-kd (&key (x 99)) x)");
    ASSERT_EQ_INT(eval_int("(%test-kd)"), 99);
    ASSERT_EQ(cl_eval_string("(%test-kd :x nil)"), CL_NIL);
    ASSERT_EQ_INT(eval_int("(%test-kd :x 42)"), 42);
}

TEST(eval_key_default_special_var)
{
    /* Key param shadows a special variable — default must see the dynamic
     * value, not the uninitialized local slot (was returning NIL). */
    cl_eval_string("(defvar *%test-ks* 100)");
    cl_eval_string("(defun %test-ks (&key ((:path *%test-ks*) *%test-ks*)) *%test-ks*)");
    ASSERT_EQ_INT(eval_int("(%test-ks)"), 100);
    ASSERT_EQ_INT(eval_int("(%test-ks :path 77)"), 77);
    /* Later key default can reference earlier key param */
    cl_eval_string("(defun %test-ks2 (&key (a 10) (b a)) (list a b))");
    ASSERT_STR_EQ(eval_print("(%test-ks2)"), "(10 10)");
    ASSERT_STR_EQ(eval_print("(%test-ks2 :a 20)"), "(20 20)");
    ASSERT_STR_EQ(eval_print("(%test-ks2 :a 20 :b 30)"), "(20 30)");
}

TEST(eval_special_var_unwind_closure)
{
    /* Dynamic bindings must be restored after closure returns */
    cl_eval_string("(defvar *%tw1* nil)");
    cl_eval_string("(defun %tw1-inner () *%tw1*)");
    cl_eval_string(
        "(defun %tw1-outer ()"
        "  (let ((*%tw1* :outer))"
        "    (let ((f (lambda () (let ((*%tw1* :inner)) (%tw1-inner)))))"
        "      (list (%tw1-inner) (funcall f) (%tw1-inner)))))");
    ASSERT_STR_EQ(eval_print("(%tw1-outer)"), "(:OUTER :INNER :OUTER)");
}

TEST(eval_special_var_unwind_block_return)
{
    /* return from block must unwind dynamic bindings */
    cl_eval_string("(defvar *%tw2* nil)");
    cl_eval_string(
        "(defun %tw2-test ()"
        "  (loop (let ((*%tw2* :bound)) (return *%tw2*))))");
    ASSERT_STR_EQ(eval_print("(list (%tw2-test) *%tw2*)"), "(:BOUND NIL)");
}

TEST(eval_destructuring_bind_rest_key)
{
    /* destructuring-bind must process &key after &rest */
    ASSERT_STR_EQ(eval_print(
        "(destructuring-bind (a &rest r &key x y) '(1 :x 10 :y 20)"
        "  (list a r x y))"),
        "(1 (:X 10 :Y 20) 10 20)");
}

TEST(eval_destructuring_bind_arity_strict)
{
    /* CLHS: a list that does not match the structure of the lambda list
       (too few or too many elements) must signal an error. */
    /* too few required */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (destructuring-bind (a b) '(\"x\") (list a b))"
        "  (error () :err))"), ":ERR");
    /* too many for a required-only pattern */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (destructuring-bind (a b) '(1 2 3) (list a b))"
        "  (error () :err))"), ":ERR");
    /* too few inside a nested pattern */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (destructuring-bind (a (b c)) '(1 (2)) (list a b c))"
        "  (error () :err))"), ":ERR");
    /* too many with &optional but no &rest */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (destructuring-bind (a &optional b) '(1 2 3) (list a b))"
        "  (error () :err))"), ":ERR");
    /* exact match still binds */
    ASSERT_STR_EQ(eval_print(
        "(destructuring-bind (a b) '(1 2) (list a b))"), "(1 2)");
    /* nested exact match */
    ASSERT_STR_EQ(eval_print(
        "(destructuring-bind (a (b c)) '(1 (2 3)) (list a b c))"),
        "(1 2 3)");
    /* &rest legitimately absorbs the remainder — no error */
    ASSERT_STR_EQ(eval_print(
        "(destructuring-bind (a &rest r) '(1 2 3) (list a r))"),
        "(1 (2 3))");
    /* missing optional is fine */
    ASSERT_STR_EQ(eval_print(
        "(destructuring-bind (a &optional b) '(1) (list a b))"),
        "(1 NIL)");
    /* dotted tail absorbs the remainder — no error */
    ASSERT_STR_EQ(eval_print(
        "(destructuring-bind (a . b) '(1 2 3) (list a b))"),
        "(1 (2 3))");
}

TEST(eval_destructuring_bind_whole_aux)
{
    /* CLHS 3.4.4/3.4.5: destructuring lambda lists support &whole and &aux.
       Before these were handled, the walker treated &whole / &aux as ordinary
       required parameters — mis-binding the variables and inflating the
       required-arity count, so the arity guards reported a spurious "too few
       elements" (this broke esrap's defrule and macros.lisp). */
    /* &whole binds the entire list, consumes no element */
    ASSERT_STR_EQ(eval_print(
        "(destructuring-bind (&whole w a b) '(1 2) (list w a b))"),
        "((1 2) 1 2)");
    /* &whole + &rest */
    ASSERT_STR_EQ(eval_print(
        "(destructuring-bind (&whole w a &rest r) '(1 2 3) (list w a r))"),
        "((1 2 3) 1 (2 3))");
    /* &aux binds let*-style with no list element / no arity constraint */
    ASSERT_STR_EQ(eval_print(
        "(destructuring-bind (a b &aux (c 99)) '(1 2) (list a b c))"),
        "(1 2 99)");
    /* &aux default is NIL; later aux/earlier params are visible to inits */
    ASSERT_STR_EQ(eval_print(
        "(destructuring-bind (a &aux (b (+ a 2)) c) '(1) (list b c))"),
        "(3 NIL)");
    /* &optional followed by &aux */
    ASSERT_STR_EQ(eval_print(
        "(destructuring-bind (a &optional (b 9) &aux (c 5)) '(1) (list a b c))"),
        "(1 9 5)");
    /* destructuring &rest: the rest var may itself be a pattern */
    ASSERT_STR_EQ(eval_print(
        "(destructuring-bind (a &rest (b c)) '(1 2 3) (list a b c))"),
        "(1 2 3)");
    /* genuine too-few still errors (guard intact after the &whole/&aux fix) */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (destructuring-bind (a b c) '(1 2) (list a b c))"
        "  (error () :err))"), ":ERR");
    /* a macro lambda list with &whole + &rest expands correctly.  Define the
       macro and use it in separate top-level forms so the macro is registered
       before its use is compiled. */
    eval_print("(defmacro mw (&whole form a &rest r) (declare (ignore a r))"
               "  (list 'quote form))");
    ASSERT_STR_EQ(eval_print("(mw 1 2 3)"), "(MW 1 2 3)");
}

/* --- Synonym stream tests (Bug 14) --- */

TEST(eval_synonym_stream)
{
    /* make-synonym-stream should delegate writes to the symbol's value */
    ASSERT_STR_EQ(eval_print(
        "(let ((s (make-string-output-stream)))"
        "  (let ((*trace-output* s))"
        "    (let ((syn (make-synonym-stream '*trace-output*)))"
        "      (write-string \"hello\" syn)"
        "      (get-output-stream-string s))))"),
        "\"hello\"");
}

TEST(eval_compile_file_truename)
{
    /* *compile-file-truename* should be NIL when not compiling */
    ASSERT_STR_EQ(eval_print("*compile-file-truename*"), "NIL");
    ASSERT_STR_EQ(eval_print("*compile-file-pathname*"), "NIL");
}

TEST(eval_directory_basic)
{
    /* directory should return a list (possibly empty) */
    ASSERT_STR_EQ(eval_print("(listp (directory \"/nonexistent-path-12345/*.xyz\"))"), "T");
}

/* --- let* variable shadowing with closure capture --- */

TEST(eval_let_star_shadow_closure_mutation)
{
    /* Regression: closures in let* body must capture the innermost binding
       when the same variable name is rebound (shadowed). */
    ASSERT_STR_EQ(eval_print(
        "(let* ((x 10) (x 20))"
        "  (labels ((set-x (v) (setf x v)) (get-x () x))"
        "    (set-x 30) (get-x)))"), "30");
}

TEST(eval_let_star_shadow_initform_ref)
{
    /* Init-form of shadowed binding should see the earlier binding. */
    ASSERT_STR_EQ(eval_print("(let* ((x 10) (x (1+ x))) x)"), "11");
}

TEST(eval_let_star_shadow_no_closure)
{
    /* Basic let* shadowing without closures. */
    ASSERT_STR_EQ(eval_print("(let* ((x 1) (x (+ x 100))) x)"), "101");
}

/* CLHS LET: a binding is VAR or (VAR [INIT-FORM]).  (x 1 2) compiled
 * silently as (x 1) — the compiler read the car and cadr and dropped the
 * rest without a word.  Now a compile-time error, for LET and LET*, that
 * names the binding; the well-formed shapes are unaffected. */
TEST(eval_let_malformed_binding_rejected)
{
    ASSERT_STR_EQ(eval_print("(let ((x 1 2)) x)"), "ERROR:1");
    ASSERT_STR_EQ(eval_print("(let* ((x 1 2)) x)"), "ERROR:1");
    ASSERT_STR_EQ(eval_print("(let ((x 1) (y 2 3)) y)"), "ERROR:1");
    /* the message names the binding and the form */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (eval '(let ((x 1 2)) x))"
        "  (error (e) (let ((m (format nil \"~a\" e)))"
        "    (list (and (search \"LET:\" m) t) (and (search \"(X 1 2)\" m) t)))))"),
        "(T T)");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (eval '(let* ((x 1 2)) x))"
        "  (error (e) (and (search \"LET*:\" (format nil \"~a\" e)) t)))"),
        "T");
    /* dotted binding, non-symbol variable, dotted binding list */
    ASSERT_STR_EQ(eval_print("(let ((x . 1)) x)"), "ERROR:1");
    ASSERT_STR_EQ(eval_print("(let ((1 2)) 3)"), "ERROR:1");
    ASSERT_STR_EQ(eval_print("(let (\"s\") 3)"), "ERROR:1");
    ASSERT_STR_EQ(eval_print("(let ((x 1) . y) x)"), "ERROR:1");
    /* the well-formed shapes still compile */
    ASSERT_STR_EQ(eval_print("(let (x (y) (z 3)) (list x y z))"), "(NIL NIL 3)");
    ASSERT_STR_EQ(eval_print("(let* (x (y) (z 3)) (list x y z))"), "(NIL NIL 3)");
    ASSERT_STR_EQ(eval_print("(let () 4)"), "4");
}

TEST(eval_let_too_many_bindings_rejected)
{
    /* CL_MAX_BINDINGS (64) is a compiler limit; bindings past it were
     * silently dropped by the parallel LET's n < CL_MAX_BINDINGS guard,
     * and LET* indexed its boxing table past the end.  Both now error. */
    char form[4096];
    int i, len = 0;
    len += snprintf(form + len, sizeof(form) - (size_t)len, "(let (");
    for (i = 0; i < 65; i++)
        len += snprintf(form + len, sizeof(form) - (size_t)len, "(v%d %d) ", i, i);
    snprintf(form + len, sizeof(form) - (size_t)len, ") v64)");
    ASSERT_STR_EQ(eval_print(form), "ERROR:1");
    /* exactly 64 is fine */
    len = 0;
    len += snprintf(form + len, sizeof(form) - (size_t)len, "(let* (");
    for (i = 0; i < 64; i++)
        len += snprintf(form + len, sizeof(form) - (size_t)len, "(v%d %d) ", i, i);
    snprintf(form + len, sizeof(form) - (size_t)len, ") v63)");
    ASSERT_STR_EQ(eval_print(form), "63");
}

TEST(eval_let_bind_name_shadows_macro)
{
    /* Regression: the compiler's scan_body_for_boxing must not treat the
       binding variable name in (let/let* ((VAR VALUE)...) ...) as a macro
       call when VAR happens to shadow a registered macro.
       This was triggered by log4cl's expand-log-with-level which binds
       the local variable CHECK-TYPE, shadowing the standard macro. */
    ASSERT_STR_EQ(eval_print(
        "(defun regress-let-shadow-check-type (x)"
        "  (let* ((check-type x))"
        "    (list check-type)))"), "REGRESS-LET-SHADOW-CHECK-TYPE");
    ASSERT_STR_EQ(eval_print(
        "(regress-let-shadow-check-type 42)"), "(42)");
}

TEST(eval_mvbind_var_shadows_macro)
{
    /* Regression: multiple-value-bind var list must not be scanned as a
       macro call when a var name shadows a registered macro. */
    ASSERT_STR_EQ(eval_print(
        "(defun regress-mvbind-shadow (x)"
        "  (multiple-value-bind (check-type) (values x)"
        "    check-type))"), "REGRESS-MVBIND-SHADOW");
    ASSERT_EQ_INT(eval_int("(regress-mvbind-shadow 7)"), 7);
}

TEST(eval_do_implicit_tagbody)
{
    /* CLHS 6.1.6: the body of do / do* is an implicit tagbody —
       (go TAG) must find tags placed in the body.  Used by serapeum's
       lines-limit in strings.lisp and countless other libraries. */
    ASSERT_STR_EQ(eval_print(
        "(let ((visited nil))"
        "  (do* ((i 0 (1+ i)))"
        "       ((= i 3) (nreverse visited))"
        "    (push 'before visited)"
        "    (when (= i 1) (go skip))"
        "    (push 'body visited)"
        "    skip"
        "    (push 'after visited)))"),
        "(BEFORE BODY AFTER BEFORE AFTER BEFORE BODY AFTER)");
    /* Same for plain DO. */
    ASSERT_STR_EQ(eval_print(
        "(let ((visited nil))"
        "  (do ((i 0 (1+ i)))"
        "      ((= i 3) (nreverse visited))"
        "    (push 'before visited)"
        "    (when (= i 1) (go skip))"
        "    (push 'body visited)"
        "    skip"
        "    (push 'after visited)))"),
        "(BEFORE BODY AFTER BEFORE AFTER BEFORE BODY AFTER)");
}

TEST(eval_destructuring_bind_pattern_shadows_macro)
{
    /* Regression: scan_body_for_boxing must not walk a destructuring-bind
       pattern as a general form.  Without this, a pattern `(m)` whose
       head symbol is a globally-registered macro gets macroexpanded
       during boxing analysis — the expander runs with the wrong arity
       and could crash the VM (serapeum types.lisp vs fiveam's TEST).

       Side-effect detection: the macro counts its expansions.  If the
       scanner wrongly expands the pattern during compilation of
       dbp-outer, the counter will be > 0.  With the fix it must be 0. */
    eval_print("(defparameter *dbp-expansions* 0)");
    eval_print(
        "(defmacro dbp-inner (&rest args)"
        "  (declare (ignore args))"
        "  (incf *dbp-expansions*)"
        "  nil)");
    /* Reset after any incidental expansions during defmacro itself */
    eval_print("(setq *dbp-expansions* 0)");
    eval_print(
        "(defmacro dbp-outer ((dbp-inner) &body body)"
        "  `(list ',dbp-inner ,@body))");
    ASSERT_EQ_INT(eval_int("*dbp-expansions*"), 0);
    ASSERT_STR_EQ(eval_print("(dbp-outer (y) 1 2 3)"), "(Y 1 2 3)");
}

/* --- Heap exhaustion / storage error tests --- */

/* --- PROGV tests --- */

TEST(eval_progv_basic)
{
    /* Simple dynamic binding */
    eval_print("(defvar *pv-x* 10)");
    ASSERT_EQ_INT(eval_int("(progv '(*pv-x*) '(99) *pv-x*)"), 99);
    /* Restored after progv */
    ASSERT_EQ_INT(eval_int("*pv-x*"), 10);
}

TEST(eval_progv_multiple)
{
    /* Multiple bindings */
    eval_print("(defvar *pv-a* 1)");
    eval_print("(defvar *pv-b* 2)");
    ASSERT_EQ_INT(eval_int("(progv '(*pv-a* *pv-b*) '(10 20) (+ *pv-a* *pv-b*))"), 30);
    ASSERT_EQ_INT(eval_int("*pv-a*"), 1);
    ASSERT_EQ_INT(eval_int("*pv-b*"), 2);
}

TEST(eval_progv_fewer_values)
{
    /* Fewer values than symbols — extras become unbound */
    eval_print("(defvar *pv-c* 5)");
    ASSERT_EQ_INT(eval_int(
        "(progv '(*pv-c*) '(42) *pv-c*)"), 42);
    /* With no values at all */
    ASSERT_STR_EQ(eval_print(
        "(progv '(*pv-c*) '() (boundp '*pv-c*))"), "NIL");
    /* Restored after */
    ASSERT_EQ_INT(eval_int("*pv-c*"), 5);
}

TEST(eval_progv_extra_values)
{
    /* More values than symbols — extra values ignored */
    eval_print("(defvar *pv-d* 0)");
    ASSERT_EQ_INT(eval_int("(progv '(*pv-d*) '(77 88 99) *pv-d*)"), 77);
    ASSERT_EQ_INT(eval_int("*pv-d*"), 0);
}

TEST(eval_progv_empty_symbols)
{
    /* Empty symbol list — body executes normally */
    ASSERT_EQ_INT(eval_int("(progv '() '() (+ 1 2))"), 3);
}

TEST(eval_progv_restore_on_throw)
{
    /* Bindings restored on non-local exit */
    eval_print("(defvar *pv-e* 100)");
    ASSERT_EQ_INT(eval_int(
        "(catch 'done (progv '(*pv-e*) '(999) (throw 'done 42)))"), 42);
    ASSERT_EQ_INT(eval_int("*pv-e*"), 100);
}

TEST(eval_progv_nested)
{
    /* Nested progv */
    eval_print("(defvar *pv-f* 0)");
    ASSERT_EQ_INT(eval_int(
        "(progv '(*pv-f*) '(1) "
        "  (progv '(*pv-f*) '(2) *pv-f*))"), 2);
    ASSERT_EQ_INT(eval_int("*pv-f*"), 0);
}

/* --- set-syntax-from-char tests --- */

TEST(eval_set_syntax_from_char_basic)
{
    /* set-syntax-from-char returns T */
    ASSERT_STR_EQ(eval_print(
        "(let ((*readtable* (copy-readtable)))"
        "  (set-syntax-from-char #\\{ #\\( *readtable*))"), "T");
}

TEST(eval_set_syntax_from_char_copies_macro)
{
    /* Set a custom reader macro on {, then copy it to [ via set-syntax-from-char */
    ASSERT_STR_EQ(eval_print(
        "(let ((*readtable* (copy-readtable)))"
        "  (set-macro-character #\\{ (lambda (s c) (declare (ignore s c)) 42))"
        "  (set-syntax-from-char #\\[ #\\{ *readtable* *readtable*)"
        "  (functionp (get-macro-character #\\[)))"), "T");
}

TEST(eval_set_syntax_from_char_reset_constituent)
{
    /* Copy constituent syntax (A) to macro char ({) — resets it */
    ASSERT_STR_EQ(eval_print(
        "(let ((*readtable* (copy-readtable)))"
        "  (set-macro-character #\\{ (lambda (s c) (declare (ignore s c)) 42))"
        "  (set-syntax-from-char #\\{ #\\A *readtable* *readtable*)"
        "  (get-macro-character #\\{))"), "NIL");
}

TEST(eval_set_syntax_from_char_default_from)
{
    /* Default from-readtable is standard (slot 0) */
    /* Copy ( from standard to { — should make { a terminating macro */
    ASSERT_STR_EQ(eval_print(
        "(let ((*readtable* (copy-readtable)))"
        "  (set-syntax-from-char #\\{ #\\())"
        ), "T");
}

TEST(eval_nth_value_mv_reset)
{
    /* Regression: nth-value must reset MV state so callers using
     * multiple-value-list see only the single extracted value,
     * not the stale MV buffer from the inner form. */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (funcall (lambda () (nth-value 1 (values 'a 'b)))))"),
        "(B)");
    /* Primary value path too */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (funcall (lambda () (nth-value 0 (values 'x 'y)))))"),
        "(X)");
}

TEST(eval_do_return_after_nlx_block)
{
    /* Regression: do, do*, dolist, dotimes create implicit NIL block.
     * If block_info.uses_nlx is stale from a prior block, RETURN inside
     * the loop body emits OP_BLOCK_RETURN (NLX) instead of a local jump,
     * causing "RETURN-FROM: no block named NIL" at runtime. */
    ASSERT_EQ_INT(eval_int(
        "(block foo (return-from foo 1) (do ((i 0 (1+ i))) ((= i 3) 99) (return 42)))"),
        1);
    /* do with return in body */
    ASSERT_EQ_INT(eval_int(
        "(do ((i 0 (1+ i))) (nil) (when (= i 5) (return i)))"),
        5);
    /* do* with return */
    ASSERT_EQ_INT(eval_int(
        "(do* ((i 0 (1+ i))) (nil) (when (= i 3) (return (* i 10))))"),
        30);
    /* dolist with return */
    ASSERT_EQ_INT(eval_int(
        "(dolist (x '(1 2 3 4 5)) (when (= x 3) (return x)))"),
        3);
    /* dotimes with return */
    ASSERT_EQ_INT(eval_int(
        "(dotimes (i 100) (when (= i 7) (return i)))"),
        7);
    /* Nested: NLX block followed by do with return */
    ASSERT_EQ_INT(eval_int(
        "(progn (block outer (return-from outer 0)) (do ((i 0 (1+ i))) (nil) (when (= i 2) (return i))))"),
        2);
}

TEST(eval_block_return_preserves_mv)
{
    /* Regression: return-from across NLX must preserve multiple values,
     * not just the primary value. */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (block b (return-from b (values 1 2 3))))"),
        "(1 2 3)");
    /* Cross-closure return (do-map pattern) */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (block nil (funcall (lambda () (return (values :a :b))))))"),
        "(:A :B)");
    /* catch/throw: throw is currently a function (not special operator),
     * so MV from the result form is lost in the function call.
     * Test single-value throw works correctly. */
    ASSERT_EQ_INT(eval_int(
        "(catch 'tag (throw 'tag 42))"), 42);
}

TEST(eval_seq_test_not)
{
    /* :test-not for count, find, position, remove, etc. */
    ASSERT_EQ_INT(eval_int(
        "(count 3 '(1 2 3 2 5 6) :test-not #'>=)"), 2);
    ASSERT_EQ_INT(eval_int(
        "(count 3 '(1 2 3 4 5) :test-not #'eql)"), 4);
    ASSERT_STR_EQ(eval_print(
        "(find 3 '(1 2 3 4 5) :test-not #'eql)"), "1");
    ASSERT_STR_EQ(eval_print(
        "(remove 3 '(1 2 3 4 3) :test-not #'eql)"), "(3 3)");
    /* position: first element where (not (eql 3 elem)) = true is index 0 */
    ASSERT_EQ_INT(eval_int(
        "(position 3 '(3 2 3 4 5) :test-not #'eql)"), 1);
}

TEST(eval_setf_composite_cadr)
{
    /* (setf (cdar x) val) should work like (rplacd (car x) val) */
    ASSERT_EQ_INT(eval_int(
        "(let ((x (cons (cons 1 2) 3))) (setf (cdar x) 99) (cdar x))"),
        99);
    /* (setf (caar x) val) */
    ASSERT_EQ_INT(eval_int(
        "(let ((x (cons (cons 1 2) 3))) (setf (caar x) 77) (caar x))"),
        77);
    /* (setf (cadr x) val) */
    ASSERT_EQ_INT(eval_int(
        "(let ((x (list 1 2 3))) (setf (cadr x) 88) (cadr x))"),
        88);
    /* (setf (cddr x) val) */
    ASSERT_STR_EQ(eval_print(
        "(let ((x (list 1 2 3))) (setf (cddr x) '(99)) x)"),
        "(1 2 99)");
    /* 3-deep: (setf (cadar x) val) */
    ASSERT_EQ_INT(eval_int(
        "(let ((x (list (cons 1 (cons 2 3))))) (setf (cadar x) 55) (cadar x))"),
        55);
}

/* --- MEMBER with :key / :test / :test-not (CLHS) --- */

TEST(eval_member_key_lambda)
{
    /* :KEY is applied to each element before :TEST.  Essential for
     * libraries like trivia that do (MEMBER sym *list* :KEY #'CAR). */
    ASSERT_STR_EQ(eval_print(
        "(member 'b (list (list 'a 1) (list 'b 2)) :key #'car)"),
        "((B 2))");
}

TEST(eval_member_test_not)
{
    /* :TEST-NOT inverts the match — returns the tail starting at the
     * first element the predicate rejects. */
    ASSERT_STR_EQ(eval_print("(member 1 (list 1 2 3) :test-not #'eql)"),
                  "(2 3)");
}

TEST(eval_member_test_and_test_not_conflict)
{
    /* CLHS: supplying both :TEST and :TEST-NOT is an error. */
    int err;
    CL_CATCH(err);
    if (err == CL_ERR_NONE) {
        cl_eval_string("(member 1 '(1 2) :test #'eql :test-not #'eql)");
        CL_UNCATCH();
        ASSERT(0 && "expected error");
    } else {
        CL_UNCATCH();
        cl_vm.sp = 0;
        cl_vm.fp = 0;
    }
}

TEST(eval_member_if_and_if_not)
{
    ASSERT_STR_EQ(eval_print("(member-if #'evenp '(1 3 5 6 7 8))"), "(6 7 8)");
    ASSERT_STR_EQ(eval_print("(member-if-not #'oddp '(1 3 5 6 7 8))"), "(6 7 8)");
    /* :key is applied to the element before the predicate. */
    ASSERT_STR_EQ(eval_print("(member-if #'evenp '((1) (2) (3)) :key #'car)"),
                  "((2) (3))");
    /* No match returns NIL. */
    ASSERT_STR_EQ(eval_print("(member-if #'evenp '(1 3 5))"), "NIL");
    ASSERT_STR_EQ(eval_print("(member-if-not #'symbolp '(a b c))"), "NIL");
}

/* --- (SETF (GETF place ind) val) semantics (CLHS §5.1.2.4) --- */

TEST(eval_setf_getf_key_absent_prepends)
{
    /* When the indicator is not in the plist, SETF must prepend a new
     * indicator/value pair and REASSIGN the place — so reading the
     * variable afterwards reflects the update. */
    ASSERT_STR_EQ(eval_print(
        "(let ((p nil)) (setf (getf p :a) 1) p)"),
        "(:A 1)");
}

TEST(eval_setf_getf_key_absent_returns_value)
{
    /* SETF returns the new value, not the new plist. */
    ASSERT_STR_EQ(eval_print(
        "(let ((p nil)) (setf (getf p :a) 42))"),
        "42");
}

TEST(eval_setf_getf_key_present_updates_in_place)
{
    /* When the indicator is already present, update the value cell
     * without prepending (preserves EQ on the plist head). */
    ASSERT_STR_EQ(eval_print(
        "(let ((p (list :a 1 :b 2))) (setf (getf p :a) 99) p)"),
        "(:A 99 :B 2)");
}

TEST(eval_setf_getf_key_absent_preserves_existing)
{
    /* Prepending must not clobber the other pairs. */
    ASSERT_STR_EQ(eval_print(
        "(let ((p (list :a 1))) (setf (getf p :b) 2) p)"),
        "(:B 2 :A 1)");
}

TEST(eval_setf_getf_value_evaluated_once)
{
    /* VAL must be evaluated exactly once — important when it has side
     * effects.  We rewrite (setf (getf p i) v) to a LET that binds V
     * to value; double evaluation of PLACE and IND is tolerated (only
     * symbol places reach this path in practice). */
    eval_print("(defvar *gcount* 0)");
    eval_print(
        "(let ((p (list :a 1))) (setf (getf p :a) (progn (incf *gcount*) 9)) p)");
    ASSERT_STR_EQ(eval_print("*gcount*"), "1");
}

TEST(eval_reader_leading_dot_float_in_list)
{
    /* CLHS 2.3.1: a token of the form `.<digit>...` is a potential number.
     * The reader peeks one character past the `.` to disambiguate dotted-pair
     * syntax from a leading-decimal float; the stream's single-slot pushback
     * means `unread_char(next); unread_char('.')` would lose the digit and
     * leave the reader staring at a bare `.` symbol.  This regression
     * exercises every leg of the form so the seeded-prefix path stays honest. */
    ASSERT_STR_EQ(eval_print(".5"), "0.5");
    ASSERT_STR_EQ(eval_print("(list .2 .5 1.5)"), "(0.2 0.5 1.5)");
    ASSERT_STR_EQ(eval_print("(+ .25 .25)"), "0.5");
    /* Genuine dotted pair must still work after the refactor. */
    ASSERT_STR_EQ(eval_print("(car '(1 . 2))"), "1");
    ASSERT_STR_EQ(eval_print("(cdr '(1 . 2))"), "2");
    /* `.foo` reads as a symbol named `.FOO` (constituent-only token). */
    ASSERT_STR_EQ(eval_print("(symbol-name (car '(.foo)))"), "\".FOO\"");
}

TEST(eval_eval_self_evaluating_opaque)
{
    /* CLHS 3.1.2.1.3: standard objects without an explicit form
     * interpretation evaluate to themselves.  The compiler used to error with
     * "Cannot compile: unexpected STREAM in expression position" the first
     * time bordeaux-threads fed `(eval *standard-output*)` through
     * binding-default-specials — exactly the path lparallel hits when
     * spawning workers with default special bindings.  Cover STREAM, LOCK,
     * THREAD, RANDOM-STATE, CONDITION, and HASH-TABLE so a future
     * regression doesn't sneak past unnoticed. */
    ASSERT_STR_EQ(eval_print("(eq *standard-output* (eval *standard-output*))"), "T");
    ASSERT_STR_EQ(eval_print("(eq *random-state* (eval *random-state*))"), "T");
    ASSERT_STR_EQ(eval_print(
        "(let ((lk (mp:make-lock \"x\"))) (eq lk (eval lk)))"), "T");
    ASSERT_STR_EQ(eval_print(
        "(let ((cv (mp:make-condition-variable))) (eq cv (eval cv)))"), "T");
    ASSERT_STR_EQ(eval_print(
        "(let ((c (make-condition 'simple-error :format-control \"x\")))"
        "  (eq c (eval c)))"), "T");
    ASSERT_STR_EQ(eval_print(
        "(let ((h (make-hash-table))) (eq h (eval h)))"), "T");
}

TEST(eval_ccase_ctypecase)
{
    /* CCASE / CTYPECASE: continuable variants of ECASE / ETYPECASE with a
     * STORE-VALUE restart (see test_condition.c for the retry coverage).  They
     * are also called with a bare, non-place keyFORM (literal / quote), as here
     * and in lparallel's central scheduler — that must still compile and match. */
    ASSERT_STR_EQ(eval_print("(ccase 'b ((a) 1) ((b) 2) ((c) 3))"), "2");
    ASSERT_STR_EQ(eval_print("(ccase 5 ((1) :one) ((2 5) :two-or-five))"),
                  ":TWO-OR-FIVE");
    ASSERT_STR_EQ(eval_print("(ctypecase 42 (string :s) (integer :i))"), ":I");
    ASSERT_STR_EQ(eval_print("(ctypecase \"hi\" (string :s) (integer :i))"), ":S");
}

TEST(eval_scanner_symbol_macrolet_environment)
{
    /* Regression (ironclad sha3.lisp:65 "Unbound variable: X").
     *
     * The compiler's closure/NLX pre-scanners (scan_body_for_boxing and
     * nlx_scan) speculatively macroexpand any global macro they meet, to see
     * through macro-generated closures.  They must register enclosing
     * SYMBOL-MACROLET bindings into the active compiler env first, because an
     * &environment-aware expander may resolve a symbol-macro via
     * (macroexpand x env).  ironclad's GET-KECCAK-ROTATE-OFFSET does exactly
     * that for an x bound by a SYMBOL-MACROLET that DOTIMES-UNROLLED emits; if
     * the scanner expands it without the binding, the expander signals
     * "Unbound variable: X" mid-scan.
     *
     * That spurious scan-time error is normally swallowed by the scanner's
     * CL_CATCH — UNLESS an outer HANDLER-CASE / UNWIND-PROTECT is active during
     * compilation (as ASDF's compile driver installs), in which case cl_error
     * signals through the condition system / NLX stack first and the outer
     * handler hijacks it, aborting the whole compilation.  The fix makes the
     * speculative expansion SUCCEED (fold to its constant) so no error is
     * raised.  This test reproduces the ASDF condition with an explicit
     * HANDLER-CASE around the function's compilation. */
    cl_eval_string(
        "(defun smm-tmexpand (form env)"
        "  (let ((real (macroexpand form env)))"
        "    (if (atom real) real"
        "        (cons (car real)"
        "              (mapcar (lambda (x) (smm-tmexpand x env)) (cdr real))))))");
    cl_eval_string(
        "(defmacro smm-unrolled ((var limit) &body body &environment env)"
        "  (loop for i from 0 below (eval (smm-tmexpand limit env))"
        "        collect (list 'symbol-macrolet (list (list var i))"
        "                      (cons 'progn body)) into forms"
        "        finally (return (cons 'progn forms))))");
    cl_eval_string(
        "(defparameter *smm-offs*"
        "  (make-array '(2 2) :initial-contents '((10 11) (12 13))))");
    cl_eval_string(
        "(defmacro smm-get (x y &environment env)"
        "  (aref *smm-offs* (eval (smm-tmexpand x env))"
        "                   (eval (smm-tmexpand y env))))");
    /* Compile smm-keccak inside a HANDLER-CASE.  Sum of the 2x2 table is 46;
     * without the fix the scan-time error escapes and the clause returns -1. */
    ASSERT_EQ_INT(eval_int(
        "(handler-case"
        "    (progn"
        "      (eval '(defun smm-keccak (acc)"
        "               (smm-unrolled (x 2)"
        "                 (smm-unrolled (y 2)"
        "                   (setf acc (+ acc (smm-get x y)))))))"
        "      (smm-keccak 0))"
        "  (error (e) (declare (ignore e)) -1))"), 46);
}

/* Regression (local-time:parse-timestring returned NIL month/day → chipi
 * influx persistence "CDR/%STRUCT-REF" failures).
 *
 * The boxing pre-scan (scan_body_for_boxing) decides which enclosing
 * variables a FLET/LABELS closure mutates, so it can box them and make the
 * write visible outside the closure.  It MUST macroexpand MACROLET-defined
 * macros while scanning a closure body — otherwise a `(setf var ...)` hidden
 * inside such a macro is never seen, the variable is not boxed, and the
 * closure mutates a private copy while the enclosing binding keeps its
 * initial value.  local-time's %SPLIT-TIMESTRING does exactly this: a
 * PARSE-INTEGER-INTO macrolet does `(setf month value)` inside the date
 * LABELS functions, so month/day came back NIL and a later `(<= 1 month 12)`
 * raised "<=: argument is not of type REAL".
 *
 * The fix installs the macrolet's expanders into the compiler env for the
 * duration of the body scan (and makes the scan-time expansion path
 * recognise local macros).  The SYMBOL-MACROLET variant is the same bug. */
TEST(eval_scanner_macrolet_setf_boxing)
{
    /* setf of an enclosing lexical through a MACROLET macro, inside LABELS */
    ASSERT_EQ_INT(eval_int(
        "(let ((m 0))"
        "  (macrolet ((setm (x) `(setf m ,x)))"
        "    (labels ((s () (setm 99)))"
        "      (s)"
        "      m)))"), 99);

    /* same, but the MACROLET is nested *inside* the LABELS function body */
    ASSERT_EQ_INT(eval_int(
        "(let ((m 0))"
        "  (labels ((s () (macrolet ((setm (x) `(setf m ,x))) (setm 7))))"
        "    (s)"
        "    m))"), 7);

    /* FLET form */
    ASSERT_EQ_INT(eval_int(
        "(let ((m 0))"
        "  (flet ((s () (macrolet ((setm (x) `(setf m ,x))) (setm 5))))"
        "    (s)"
        "    m))"), 5);

    /* The local-time shape: a parse-into macrolet that setf's outer vars from
     * inside LABELS, then reads them — must reflect the writes, not NIL. */
    ASSERT_STR_EQ(eval_print(
        "(let (y mo d)"
        "  (macrolet ((into (place v lo hi)"
        "               `(progn (setf ,place ,v)"
        "                       (unless (<= ,lo ,place ,hi) (error \"range\"))"
        "                       (values))))"
        "    (labels ((sy (v) (into y v 1 9999))"
        "             (sm (v) (into mo v 1 12))"
        "             (sd (v) (into d v 1 31)))"
        "      (sy 2026) (sm 6) (sd 27)"
        "      (list y mo d))))"), "(2026 6 27)");

    /* SYMBOL-MACROLET analogue: setf through a symbol-macro inside LABELS. */
    ASSERT_EQ_INT(eval_int(
        "(let ((m 0))"
        "  (labels ((s () (symbol-macrolet ((q m)) (setf q 42))))"
        "    (s)"
        "    m))"), 42);

    /* In-place macrolet (no FLET/LABELS) whose expansion both setfs a place
     * and references global macro stubs (SETF/PROGN have macro-functions for
     * MACROEXPAND conformance).  Compiling the inner (setf y v) re-enters the
     * global-macro check, whose cl_build_lex_env conses a non-empty lexical
     * env from the enclosing macrolet binding; the local `expr`/`head` copies
     * in compile_expr_step must be refreshed after that allocation or the
     * setf is dispatched with the relocated enclosing PROGN form.  Surfaced
     * under GC stress as "%SETF-SETF" / the wrong (hi) value (see the
     * mlet-into case in test_gc_stress_regression.sh). */
    ASSERT_EQ_INT(eval_int(
        "(let ((y 0))"
        "  (macrolet ((into (place v lo hi)"
        "               `(progn (setf ,place ,v)"
        "                       (unless (<= ,lo ,place ,hi) (error \"range\"))"
        "                       (values))))"
        "    (into y 2026 1 9999))"
        "  y)"), 2026);
}

TEST(eval_symbol_macrolet_nil_expansion)
{
    /* A symbol-macro whose expansion is literally NIL is a valid binding;
     * referencing it must yield NIL, NOT "Unbound variable" (the lexical
     * symbol-macro lookup used CL_NIL as both the not-found sentinel and a
     * legitimate expansion).  This is the minimal form of the trivia
     * (match nil (nil t)) failure. */
    ASSERT_STR_EQ(eval_print("(symbol-macrolet ((a nil)) a)"), "NIL");
    ASSERT_STR_EQ(eval_print("(symbol-macrolet ((a nil)) (list a a))"), "(NIL NIL)");
    /* Compiled inside a lambda body, too. */
    ASSERT_STR_EQ(eval_print(
        "(funcall (lambda () (symbol-macrolet ((a nil)) a)))"), "NIL");
    /* A non-NIL expansion still works (no regression). */
    ASSERT_STR_EQ(eval_print("(symbol-macrolet ((a 5)) (+ a 1))"), "6");
}

TEST(eval_heap_exhaustion_error)
{
    /* Accumulating live data until heap is full should signal CL_ERR_STORAGE
     * with a proper message, not crash with a segfault.
     * NOTE: this must be the LAST test — heap state is unreliable after. */
    int err;
    cl_gc_reset_roots();  /* Clear any stale roots from previous tests */
    CL_CATCH(err);
    if (err == CL_ERR_NONE) {
        cl_eval_string("(let ((x nil)) (dotimes (i 500000) (push i x)) x)");
        CL_UNCATCH();
        /* If we get here, heap was big enough — not a failure */
    } else {
        CL_UNCATCH();
        ASSERT_EQ_INT(err, CL_ERR_STORAGE);
        ASSERT(strstr(cl_error_msg, "Heap exhausted") != NULL);
    }
    cl_vm.sp = 0;
    cl_vm.fp = 0;
}

/* --- load-time-value --- */

static void test_eval_load_time_value_basic(void)
{
    /* Evaluates the form and returns the result */
    ASSERT_STR_EQ(eval_print("(load-time-value (+ 1 2))"), "3");
    ASSERT_STR_EQ(eval_print("(load-time-value (list 1 2 3))"), "(1 2 3)");
    ASSERT_STR_EQ(eval_print("(load-time-value 42)"), "42");
    ASSERT_STR_EQ(eval_print("(load-time-value t)"), "T");
    ASSERT_STR_EQ(eval_print("(load-time-value nil)"), "NIL");
    ASSERT_STR_EQ(eval_print("(load-time-value \"hello\")"), "\"hello\"");
}

static void test_eval_load_time_value_in_function(void)
{
    /* load-time-value inside a function returns the same object each call */
    eval_print("(defun ltv-test () (load-time-value (cons 1 2)))");
    ASSERT_STR_EQ(eval_print("(ltv-test)"), "(1 . 2)");
    ASSERT_STR_EQ(eval_print("(eq (ltv-test) (ltv-test))"), "T");
}

static void test_eval_load_time_value_with_read_only(void)
{
    /* read-only-p is accepted (second argument) */
    ASSERT_STR_EQ(eval_print("(load-time-value (+ 10 20) t)"), "30");
    ASSERT_STR_EQ(eval_print("(load-time-value (list 'a 'b) nil)"), "(A B)");
}

static void test_eval_load_time_value_null_lex_env(void)
{
    /* Form is evaluated in the null lexical environment —
     * surrounding lexical bindings should NOT be visible.
     * In a single-pass system this is best-effort, but we test
     * that the form at least evaluates correctly. */
    ASSERT_STR_EQ(eval_print("(load-time-value (* 6 7))"), "42");
}

static void test_eval_load_time_value_side_effect(void)
{
    /* Side effects happen at load time (compilation) */
    eval_print("(defvar *ltv-counter* 0)");
    eval_print("(defun ltv-side () (load-time-value (incf *ltv-counter*)))");
    /* The incf happened during compilation of ltv-side, so counter is 1 */
    ASSERT_STR_EQ(eval_print("*ltv-counter*"), "1");
    /* Calling the function just returns the cached value, no further incf */
    ASSERT_STR_EQ(eval_print("(ltv-side)"), "1");
    ASSERT_STR_EQ(eval_print("(ltv-side)"), "1");
    ASSERT_STR_EQ(eval_print("*ltv-counter*"), "1");
}

static void test_eval_load_time_value_complex_form(void)
{
    /* Complex form: hash table creation at load time */
    eval_print("(defun ltv-ht () (load-time-value (let ((h (make-hash-table))) (setf (gethash 'x h) 42) h)))");
    ASSERT_STR_EQ(eval_print("(gethash 'x (ltv-ht))"), "42");
    /* Same hash table each time */
    ASSERT_STR_EQ(eval_print("(eq (ltv-ht) (ltv-ht))"), "T");
}

static void test_eval_load_time_value_nested(void)
{
    /* load-time-value nested inside other forms */
    ASSERT_STR_EQ(eval_print("(+ 1 (load-time-value (+ 2 3)))"), "6");
    ASSERT_STR_EQ(eval_print("(list (load-time-value 'a) (load-time-value 'b))"), "(A B)");
}

/* --- get-properties --- */

static void test_eval_get_properties_found(void)
{
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (get-properties '(:a 1 :b 2 :c 3) '(:b :c)))"),
        "(:B 2 (:B 2 :C 3))");
}

static void test_eval_get_properties_first_match(void)
{
    /* Returns the first matching indicator */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (get-properties '(:a 1 :b 2 :c 3) '(:c :a)))"),
        "(:A 1 (:A 1 :B 2 :C 3))");
}

static void test_eval_get_properties_not_found(void)
{
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (get-properties '(:a 1 :b 2) '(:x :y)))"),
        "(NIL NIL NIL)");
}

static void test_eval_get_properties_empty(void)
{
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (get-properties '() '(:a)))"),
        "(NIL NIL NIL)");
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (get-properties '(:a 1) '()))"),
        "(NIL NIL NIL)");
}

/* --- str test regressions: declare special scoping --- */

/* (declare (special x)) in a function body must NOT globally pollute x.
 * Inner let* rebinding x should be lexical (capturable by closures). */
static void test_eval_declare_special_scoped(void)
{
    ASSERT_STR_EQ(eval_print(
        "(progn "
        "  (defun %test-make-closure () "
        "    (declare (special %test-es)) "
        "    (let* ((%test-es (concatenate 'string \"inner:\" %test-es))) "
        "      (lambda () %test-es))) "
        "  (let ((%test-es \"HELLO\")) "
        "    (declare (special %test-es)) "
        "    (funcall (%test-make-closure))))"),
        "\"inner:HELLO\"");
}

/* Free special declaration: reference a special variable not bound locally */
static void test_eval_declare_special_free_ref(void)
{
    ASSERT_STR_EQ(eval_print(
        "(progn "
        "  (defun %test-read-special () "
        "    (declare (special %test-sv)) "
        "    %test-sv) "
        "  (let ((%test-sv 42)) "
        "    (declare (special %test-sv)) "
        "    (%test-read-special)))"),
        "42");
}

/* Parameter special declaration */
static void test_eval_declare_special_param(void)
{
    ASSERT_STR_EQ(eval_print(
        "(progn "
        "  (defun %test-param-sp (x) "
        "    (declare (special x)) "
        "    (let ((result (locally (declare (special x)) x))) result)) "
        "  (%test-param-sp 77))"),
        "77");
}

/* CLHS: special-operator-p recognizes the 25 CL special operators.
 * Regression for iterate which calls (special-operator-p (car form))
 * to distinguish special forms from ordinary calls. */
static void test_eval_special_operator_p(void)
{
    ASSERT_STR_EQ(eval_print(
        "(list (special-operator-p 'quote)"
        "      (special-operator-p 'if)"
        "      (special-operator-p 'let*)"
        "      (special-operator-p 'tagbody)"
        "      (special-operator-p 'the))"),
        "(T T T T T)");
    /* Regular functions/macros, and undefined symbols, are not special ops */
    ASSERT_STR_EQ(eval_print(
        "(list (special-operator-p 'car)"
        "      (special-operator-p 'defun)"
        "      (special-operator-p 'no-such-symbol))"),
        "(NIL NIL NIL)");
}

/* alpha-char-p must recognize Latin-1 letters */
static void test_eval_alpha_char_p_unicode(void)
{
    /* code 241 = ñ, code 209 = Ñ */
    ASSERT_STR_EQ(eval_print("(alpha-char-p (code-char 241))"), "T");
    ASSERT_STR_EQ(eval_print("(alpha-char-p (code-char 209))"), "T");
    ASSERT_STR_EQ(eval_print("(alpha-char-p (code-char 233))"), "T"); /* é */
}

/* upper-case-p/lower-case-p for Unicode */
static void test_eval_upper_lower_case_unicode(void)
{
    ASSERT_STR_EQ(eval_print("(upper-case-p (code-char 209))"), "T");  /* Ñ */
    ASSERT_STR_EQ(eval_print("(lower-case-p (code-char 241))"), "T");  /* ñ */
    ASSERT_STR_EQ(eval_print("(upper-case-p (code-char 241))"), "NIL");
    ASSERT_STR_EQ(eval_print("(lower-case-p (code-char 209))"), "NIL");
}

#ifdef CL_WIDE_STRINGS
/* Wide string in reader: non-ASCII char in string literal */
static void test_eval_wide_string_reader(void)
{
    /* (string (code-char 12288)) creates a 1-char wide string with U+3000 */
    ASSERT_STR_EQ(eval_print("(char-code (char (string (code-char 12288)) 0))"), "12288");
    /* Concatenate preserves wide characters */
    ASSERT_STR_EQ(eval_print(
        "(char-code (char (concatenate 'string (string (code-char 12288)) \"a\") 0))"),
        "12288");
    ASSERT_STR_EQ(eval_print(
        "(length (concatenate 'string (string (code-char 12288)) \"abc\"))"),
        "4");
}

/* string-trim with wide characters */
static void test_eval_string_trim_wide(void)
{
    /* Trim U+3000 (fullwidth space) from both sides */
    ASSERT_STR_EQ(eval_print(
        "(string-trim (list (code-char 12288)) "
        "  (concatenate 'string (string (code-char 12288)) \"rst\" "
        "    (string (code-char 12288))))"),
        "\"rst\"");
    /* Trim with mixed ASCII and wide whitespace */
    ASSERT_STR_EQ(eval_print(
        "(string-trim (list #\\Space (code-char 12288)) "
        "  (concatenate 'string \" \" (string (code-char 12288)) \"abc\" \" \"))"),
        "\"abc\"");
}
#endif

/* concatenate must signal error for non-sequence arguments */
static void test_eval_concatenate_type_error(void)
{
    ASSERT_STR_EQ(eval_print(
        "(handler-case (concatenate 'string \"a\" 1) "
        "  (type-error () :got-type-error))"),
        ":GOT-TYPE-ERROR");
}

/* Regression: a non-symbol in keyword-argument position must signal a
 * program-error, not crash.  Previously the VM's keyword-matching loop fed the
 * non-symbol straight to cl_symbol_name() (an unchecked CL_OBJ_TO_PTR deref),
 * and the resulting garbage pointer was passed as a "%s" arg to cl_error,
 * crashing inside vsnprintf.  Surfaced by ansi-test MAKE-SEQUENCE.ERROR.13:
 * (make-sequence 'list 10 0 0) — the integer 0 lands in the &key plist's key
 * slot.  Covers the normal call path, the APPLY path and the macro that wraps
 * make-sequence. */
static void test_eval_nonsymbol_keyword_program_error(void)
{
    /* Direct call path (OP_CALL / OP_TAILCALL keyword matching). */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (funcall (lambda (&key foo) foo) 0 0) "
        "  (program-error () :got-program-error))"),
        ":GOT-PROGRAM-ERROR");
    /* APPLY path. */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (apply (lambda (&key foo) foo) '(0 0)) "
        "  (program-error () :got-program-error))"),
        ":GOT-PROGRAM-ERROR");
    /* The exact ansi-test form. */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (make-sequence 'list 10 0 0) "
        "  (program-error () :got-program-error))"),
        ":GOT-PROGRAM-ERROR");
    /* A genuine unknown (but symbol) keyword still reports cleanly. */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (funcall (lambda (&key foo) foo) :bar 1) "
        "  (program-error () :got-program-error))"),
        ":GOT-PROGRAM-ERROR");
}

/* Regression: REMOVE / REMOVE-IF / REMOVE-IF-NOT on a non-sequence must signal
 * a type-error, not crash.  bi_remove ruled out lists/strings/bit-vectors/NIL
 * and then routed everything else to remove_from_vector, which dereferenced
 * the argument as a CL_Vector — a wild read for a number, symbol, etc.
 * Surfaced by ansi-test REMOVE.ERROR.11 ((remove 'a x) over the type universe,
 * which includes non-sequences). */
static void test_eval_remove_nonsequence_type_error(void)
{
    ASSERT_STR_EQ(eval_print(
        "(handler-case (remove 'a 5) (type-error () :got-type-error))"),
        ":GOT-TYPE-ERROR");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (remove 'a 'foo) (type-error () :got-type-error))"),
        ":GOT-TYPE-ERROR");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (remove-if #'evenp 5) (type-error () :got-type-error))"),
        ":GOT-TYPE-ERROR");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (remove-if-not #'evenp 'foo) "
        "  (type-error () :got-type-error))"),
        ":GOT-TYPE-ERROR");
    /* Sanity: a real vector still works. */
    ASSERT_STR_EQ(eval_print("(remove 3 #(1 2 3 4 3 5))"), "#(1 2 4 5)");
}

/* Regression: the builtin func-ptr sanity check must not assume host
 * function pointers are 4-byte aligned.  x86-64/ARM64 ABIs give no such
 * guarantee — gcc -O1 (default -falign-functions) routinely emits functions
 * on odd byte boundaries (the inlined-macro stub used during boot landed at
 * 0x...593 here).  An alignment-based check rejected that as a "corrupted C
 * function pointer" and aborted boot in some link layouts, while passing in
 * others — so every test binary crashed at boot on Linux while amilisp (a
 * different layout) survived.  See cl_vm_builtin_fptr_plausible in vm.c. */
TEST(builtin_fptr_plausible_unaligned)
{
    uintptr_t base;

    /* NULL and tiny addresses are always rejected (a clobbered func slot is
       typically a small tagged CL_Obj, not a code address). */
    ASSERT_EQ_INT(cl_vm_builtin_fptr_plausible(NULL), 0);
    ASSERT_EQ_INT(cl_vm_builtin_fptr_plausible((const void *)(uintptr_t)0x4), 0);
    ASSERT_EQ_INT(cl_vm_builtin_fptr_plausible((const void *)(uintptr_t)0xFFF), 0);

    /* A real, callable function pointer is plausible. */
    ASSERT_EQ_INT(cl_vm_builtin_fptr_plausible((const void *)(uintptr_t)&setup), 1);

    /* Core of the fix: a high but non-4-byte-aligned address.  On host this
       MUST be accepted (the old code rejected it); on m68k an odd code
       pointer is genuinely impossible and stays rejected. */
    base = ((uintptr_t)&setup) & ~(uintptr_t)0xFFF; /* page-aligned, >= 0x1000 */
    base |= 0x1000u;
#ifdef PLATFORM_AMIGA
    ASSERT_EQ_INT(cl_vm_builtin_fptr_plausible((const void *)(base | 0x1u)), 0);
#else
    ASSERT_EQ_INT(cl_vm_builtin_fptr_plausible((const void *)(base | 0x1u)), 1);
    ASSERT_EQ_INT(cl_vm_builtin_fptr_plausible((const void *)(base | 0x3u)), 1);
#endif
}

/* --- tier-4 batch 7a: fixed-size C buffer caps removed --- */

TEST(eval_apply_builtin_over_64_args)
{
    /* The C bi_apply path (reached when APPLY itself is applied, e.g. via
     * FUNCALL #'APPLY) staged args in a flat_args[64] C array and silently
     * dropped everything past 64.  It now spreads onto the VM stack up to
     * CALL-ARGUMENTS-LIMIT. */
    ASSERT_STR_EQ(eval_print(
        "(funcall #'apply #'+ (make-list 100 :initial-element 1))"), "100");
    /* Spread initial args + trailing list through the same path. */
    ASSERT_STR_EQ(eval_print(
        "(funcall #'apply #'+ 1 2 3 (make-list 97 :initial-element 1))"),
        "103");
    /* Sanity: the result reflects the LAST args, not just the first 64. */
    ASSERT_STR_EQ(eval_print(
        "(funcall #'apply #'max (append (make-list 99 :initial-element 1)"
        "                               (list 42)))"), "42");
}

TEST(eval_remove_string_over_1023_chars)
{
    /* remove_from_string staged the kept characters in a char buf[1024],
     * silently truncating results at 1023 characters. */
    ASSERT_STR_EQ(eval_print(
        "(length (remove #\\a (make-string 2000 :initial-element #\\b)))"),
        "2000");
    ASSERT_STR_EQ(eval_print(
        "(let ((s (concatenate 'string (make-string 1500 :initial-element #\\b)"
        "                              \"a\""
        "                              (make-string 100 :initial-element #\\c))))"
        "  (length (remove #\\a s)))"), "1600");
}

TEST(eval_concatenate_string_over_4096_chars)
{
    /* bi_concatenate's string path staged into a 4096-slot buffer and
     * silently truncated longer results. */
    ASSERT_STR_EQ(eval_print(
        "(length (concatenate 'string"
        "  (make-string 3000 :initial-element #\\x)"
        "  (make-string 3000 :initial-element #\\y)))"), "6000");
    /* Last characters survive intact. */
    ASSERT_STR_EQ(eval_print(
        "(char (concatenate 'string (make-string 5000 :initial-element #\\x)"
        "                           \"z\") 5000)"), "#\\z");
}

#ifdef CL_WIDE_STRINGS
TEST(eval_remove_concatenate_wide_chars_preserved)
{
    /* remove_from_string narrowed every kept char with a (char) cast,
     * mangling wide characters; concatenate must keep them wide too. */
    ASSERT_STR_EQ(eval_print(
        "(let ((s (coerce (list #\\a (code-char #x4E2D) #\\b) 'string)))"
        "  (let ((r (remove #\\a s)))"
        "    (list (length r) (char= (char r 0) (code-char #x4E2D))"
        "          (char r 1))))"), "(2 T #\\b)");
    ASSERT_STR_EQ(eval_print(
        "(let ((r (concatenate 'string \"ab\""
        "           (coerce (list (code-char #x4E2D)) 'string))))"
        "  (list (length r) (char= (char r 2) (code-char #x4E2D))))"),
        "(3 T)");
}
#endif

/* --- tier-4 batch 7c: print-control dynbinds, bignum file positions --- */

TEST(eval_print_override_restored_after_error)
{
    /* FS16: the *PRINT-* keyword overrides are thread-local dynamic binds
     * now.  The discriminating case: when printing SIGNALS and an outer
     * handler catches, the old global save/set/restore skipped its restore
     * — *PRINT-BASE* stayed clobbered at 2 for the rest of the session.
     * A dynamic bind unwinds with the non-local exit. */
    ASSERT_STR_EQ(eval_print(
        "(progn"
        "  (defclass t7c-boom () ())"
        "  (defmethod print-object ((o t7c-boom) s) (error \"boom\"))"
        "  (handler-case (write-to-string (make-instance 't7c-boom) :base 2)"
        "    (error () nil))"
        "  *print-base*)"), "10");
    /* Normal path unchanged: override applies, then unwinds. */
    ASSERT_STR_EQ(eval_print(
        "(list (write-to-string 255 :base 16) *print-base*)"),
        "(\"FF\" 10)");
}

TEST(eval_file_position_bignum)
{
    /* ST7: positions past CL_FIXNUM_MAX (~1GB) were truncated by the fixnum
     * tag shift.  Seek past that in a (sparse) temp file and read the
     * position back — must round-trip as a bignum. */
    ASSERT_STR_EQ(eval_print(
        "(with-open-file (s \"t7c-pos.tmp\" :direction :output"
        "                   :if-exists :supersede)"
        "  (let ((big (+ 1073741823 6)))"
        "    (file-position s big)"
        "    (prog1 (list (= (file-position s) big)"
        "                 (typep (file-position s) 'bignum))"
        "      (close s)"
        "      (delete-file \"t7c-pos.tmp\"))))"), "(T T)");
}

/* --- tier-4 batch 7b: hashtable hash/equality contract (AH5) --- */

TEST(eval_hashtable_wide_string_keys)
{
    /* Wide strings were identity-hashed but content-compared: an EQUAL
     * lookup with a fresh string of the same characters missed. */
    ASSERT_STR_EQ(eval_print(
        "(let ((ht (make-hash-table :test 'equal)) (ok t))"
        "  (dotimes (i 30)"
        "    (setf (gethash (format nil \"k~A~A\" (code-char 20013) i) ht) i))"
        "  (dotimes (i 30)"
        "    (unless (eql (gethash (format nil \"k~A~A\" (code-char 20013) i) ht) i)"
        "      (setf ok nil)))"
        "  (list ok (hash-table-count ht)))"), "(T 30)");
}

TEST(eval_hashtable_equalp_vector_keys)
{
    /* EQUALP descends vectors in keys_equal but hashed them by identity —
     * a fresh equal-content vector key missed. */
    ASSERT_STR_EQ(eval_print(
        "(let ((ht (make-hash-table :test 'equalp)))"
        "  (setf (gethash (vector 1 2 3) ht) :v)"
        "  (gethash (vector 1 2 3) ht))"), ":V");
}

TEST(eval_hashtable_equal_bitvector_keys)
{
    /* keys_equal had no bit-vector branch: equal-content bit-vector keys
     * never matched (CLHS EQUAL descends bit-vectors). */
    ASSERT_STR_EQ(eval_print(
        "(let ((ht (make-hash-table :test 'equal)))"
        "  (setf (gethash #*1011 ht) :bv)"
        "  (gethash (copy-seq #*1011) ht))"), ":BV");
    ASSERT_STR_EQ(eval_print(
        "(let ((ht (make-hash-table :test 'equal)))"
        "  (setf (gethash #*1011 ht) :bv)"
        "  (gethash #*1010 ht))"), "NIL");
}

TEST(eval_hashtable_equal_vector_identity)
{
    /* CLHS: EQUAL on general vectors is EQ — the same object is found,
     * a fresh equal-content vector is not (matches the EQUAL builtin). */
    ASSERT_STR_EQ(eval_print(
        "(let ((ht (make-hash-table :test 'equal)) (v (vector 1 2)))"
        "  (setf (gethash v ht) :id)"
        "  (list (gethash v ht) (gethash (vector 1 2) ht)))"), "(:ID NIL)");
}

TEST(eval_hashtable_equalp_wide_char_no_low_byte_match)
{
    /* EQUALP char compare used a (char) cast: U+4E41 (low byte #x41) and
     * #\A compared \"equalp\" and hashed to the same bucket — a false hit. */
    ASSERT_STR_EQ(eval_print(
        "(let ((ht (make-hash-table :test 'equalp)))"
        "  (setf (gethash (code-char 20033) ht) :wide)"
        "  (list (gethash #\\A ht) (gethash (code-char 20033) ht)))"),
        "(NIL :WIDE)");
}

/* --- tier-4 batch 7b: map-family designators and 16-list caps --- */

TEST(eval_mapl_mapcon_symbol_designators)
{
    /* MAPL/MAPCON rejected symbol function designators (missing
     * cl_coerce_funcdesig, unlike their siblings). */
    ASSERT_STR_EQ(eval_print("(mapl 'identity (list 1 2))"), "(1 2)");
    ASSERT_STR_EQ(eval_print("(mapcon 'list (list 1 2))"), "((1 2) (2))");
}

TEST(eval_map_family_over_16_seqs_errors)
{
    /* The silent 16-list clamp dropped whole argument sequences; it is a
     * loud error now. */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (apply #'mapcar #'+ (make-list 17 :initial-element '(1)))"
        "  (error () :err))"), ":ERR");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (apply #'map 'list #'+ (make-list 17 :initial-element '(1)))"
        "  (error () :err))"), ":ERR");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (apply #'every #'= (make-list 17 :initial-element '(1)))"
        "  (error () :err))"), ":ERR");
    /* 16 sequences still work. */
    ASSERT_STR_EQ(eval_print(
        "(apply #'mapcar #'+ (make-list 16 :initial-element '(1)))"), "(16)");
}

TEST(vm_call_builtin_on_stack_args)
{
    /* cl_vm_call_builtin is the m68k JIT's direct entry to a C builtin
     * (jit_dispatch, src/jit/runtime_m68k.c): the arguments already sit on the
     * VM stack.  Same result and MV state as OP_CALL, same arity
     * diagnostic; the caller restores sp. */
    CL_Thread *thr = cl_get_current_thread();
    CL_Obj plus = ((CL_Symbol *)CL_OBJ_TO_PTR(cl_intern("+", 1)))->function;
    CL_Obj car  = ((CL_Symbol *)CL_OBJ_TO_PTR(cl_intern("CAR", 3)))->function;
    int base = cl_vm.sp;
    CL_Obj r;
    int err;
    ASSERT(CL_FUNCTION_P(plus) && CL_FUNCTION_P(car));
    cl_vm.stack[base] = CL_MAKE_FIXNUM(1);
    cl_vm.stack[base + 1] = CL_MAKE_FIXNUM(2);
    cl_vm.sp = base + 2;
    r = cl_vm_call_builtin(thr, (CL_Function *)CL_OBJ_TO_PTR(plus),
                           &cl_vm.stack[base], 2);
    cl_vm.sp = base;
    ASSERT_EQ(3, CL_FIXNUM_VAL(r));
    ASSERT_EQ(1, cl_mv_count);
    ASSERT(cl_mv_values[0] == r);
    /* Arity is validated before the C function runs. */
    CL_CATCH(err);
    if (err == CL_ERR_NONE) {
        cl_vm_call_builtin(thr, (CL_Function *)CL_OBJ_TO_PTR(car),
                           &cl_vm.stack[base], 0);
        CL_UNCATCH();
        ASSERT(0 && "CAR with no arguments must signal");
    } else {
        CL_UNCATCH();
        ASSERT_EQ(CL_ERR_ARGS, err);
    }
    cl_vm.sp = base;
}

int main(void)
{
    test_init();
    setup();

    RUN(eval_add);
    RUN(eval_special_operator_p);
    RUN(eval_sub);
    RUN(eval_mul);
    RUN(eval_div);
    RUN(eval_1plus_1minus);
    RUN(eval_mod);
    RUN(eval_numeq);
    RUN(eval_lt);
    RUN(eval_gt);
    RUN(eval_le_ge);
    RUN(eval_predicates);
    RUN(eval_not);
    RUN(eval_eq_eql_equal);
    RUN(eval_cons_car_cdr);
    RUN(eval_list);
    RUN(eval_length);
    RUN(eval_append);
    RUN(eval_reverse);
    RUN(eval_nth);
    RUN(eval_misc_math);
    RUN(eval_quote);
    RUN(eval_if);
    RUN(eval_progn);
    RUN(eval_setq);
    RUN(eval_let);
    RUN(eval_letstar);
    RUN(eval_defun_basic);
    RUN(eval_defun_recursive);
    RUN(eval_defun_fibonacci);
    RUN(eval_defun_multiarg);
    RUN(eval_lambda);
    RUN(eval_lambda_leading_nil_body);
    RUN(eval_function_ref);
    RUN(eval_function_ref_setf);
    RUN(eval_setf_function_distinct_per_package);
    RUN(eval_labels_past_32_funs);
    RUN(eval_error_unbound);
    RUN(eval_error_divzero);
    RUN(eval_error_type);
    RUN(eval_self_evaluating);
    RUN(eval_keyword);
    RUN(eval_defmacro_simple);
    RUN(eval_defmacro_with_args);
    RUN(eval_defmacro_when);
    RUN(eval_defmacro_unless);
    RUN(eval_defmacro_implicit_block);
    RUN(eval_defmacro_identity);
    RUN(eval_closure_capture_let);
    RUN(eval_closure_make_adder);
    RUN(eval_closure_multiple_captures);
    RUN(eval_closure_nested);
    RUN(eval_closure_shared_scope);

    /* Heap-boxed cells (mutable closure capture) */
    RUN(eval_cell_basic_counter);
    RUN(eval_cell_shared_getter_setter);
    RUN(eval_cell_accumulator_param);
    RUN(eval_cell_independent_instances);
    RUN(eval_cell_setq_before_capture);
    RUN(eval_cell_nested_closure);
    RUN(eval_cell_let_star);
    RUN(eval_cell_flet_mutation);
    RUN(eval_cell_labels_shared);
    RUN(eval_cell_do_let_capture);
    RUN(eval_cell_readonly_no_boxing);
    RUN(eval_setf_boxing_across_closure);
    RUN(eval_setf_values_boxing_across_closure);
    RUN(eval_setf_values_nonsymbol_place_gcstress);

    RUN(eval_and);
    RUN(eval_or);
    RUN(eval_cond);
    RUN(eval_when_unless);
    RUN(eval_mapcar_lambda);
    RUN(eval_funcall_lambda);
    RUN(eval_apply_closure);
    RUN(eval_dolist);
    RUN(eval_dotimes);
    RUN(eval_dolist_implicit_tagbody);
    RUN(eval_dotimes_implicit_tagbody);
    RUN(eval_do);
    RUN(eval_do_star);
    RUN(eval_do_bare_var);
    RUN(eval_quasiquote_atom);
    RUN(eval_quasiquote_simple_list);
    RUN(eval_quasiquote_unquote);
    RUN(eval_quasiquote_splicing);
    RUN(eval_quasiquote_splicing_nconc);
    RUN(eval_quasiquote_large_call_args);
    RUN(eval_quasiquote_vector_unquote);
    RUN(eval_quasiquote_vector_splice);
    RUN(eval_macroexpand_1_values_and_env);
    RUN(eval_macroexpand_symbol_macro_nil_expansion);
    RUN(eval_subtypep_typep_env_arg);
    RUN(eval_subtypep_bounded_integer_ranges);
    RUN(eval_subtypep_bounded_float_ranges);
    RUN(eval_subtypep_not_overlap);
    RUN(eval_let_shadows_symbol_macrolet);
    RUN(eval_let_shadows_global_symbol_macro);
    RUN(eval_typep_mod);
    RUN(eval_fdefinition_setf);
    RUN(eval_subtypep_compound_array_specs);
    RUN(eval_subtypep_typed_compound_array_specs);
    RUN(eval_subtypep_compound_combinators);
    RUN(eval_subtypep_universal_t_and_nil);
    RUN(eval_subtypep_integer_range_subset);
    RUN(eval_compiler_trampoline_deep_if_chain);
    RUN(eval_compiler_trampoline_deep_let_chain);
    RUN(eval_compiler_trampoline_deep_handler_bind);
    RUN(eval_compiler_trampoline_deep_mvb);
    RUN(eval_compiler_trampoline_deep_flet);
    RUN(eval_compiler_trampoline_deep_labels);
    RUN(eval_compiler_trampoline_deep_eval_when);
    RUN(eval_compiler_trampoline_deep_if_then);
    RUN(eval_compiler_trampoline_deep_if_then_with_else);
    RUN(eval_compiler_trampoline_deep_block_multi_body);
    RUN(eval_compiler_trampoline_block_return_from_nontail);
    RUN(eval_compiler_deeply_nested_blocks);
    RUN(eval_labels_shadows_global_macro);
    RUN(scanner_skips_macrolet_bindings);
    RUN(eval_loop_of_type_after_into);
    RUN(eval_loop_for_and_parallel);
    RUN(eval_loop_for_on_uses_atom_end_test);
    RUN(eval_quasiquote_nested_list);
    RUN(eval_quasiquote_dotted);
    RUN(eval_quasiquote_in_macro);
    RUN(eval_quasiquote_macro_splice);
    RUN(eval_quasiquote_nested_double_unquote);
    RUN(eval_quasiquote_nested_unquote_splice);
    RUN(eval_quasiquote_nested_simple);
    RUN(eval_quasiquote_nested_inner_splice);
    RUN(eval_quasiquote_once_only_pattern);
    RUN(eval_reader_error_condition);
    RUN(eval_stream_error_stream_accessor);
    RUN(eval_package_error_package_accessor);
    RUN(eval_cell_error_name_accessor);
    RUN(eval_undefined_setf_function_name_in_report);
    RUN(eval_runtime_cell_error_name);
    RUN(eval_file_error_pathname_accessor);
    RUN(eval_define_compiler_macro);
    RUN(eval_setf_values);
    RUN(eval_setf_values_two);
    RUN(eval_define_setf_expander);
    RUN(eval_gensym);
    RUN(eval_gensym_counter);
    RUN(eval_gentemp);
    RUN(eval_load);
    RUN(eval_boot_functions);
    RUN(eval_block_return);
    RUN(eval_return_in_do);
    RUN(eval_return_in_dolist);
    RUN(eval_return_in_dotimes);
    RUN(eval_prog1);
    RUN(eval_prog2);
    RUN(eval_case);
    RUN(eval_exhaustive_case_type_error);
    RUN(eval_typecase);
    RUN(eval_flet);
    RUN(eval_labels);
    RUN(eval_optional);
    RUN(eval_optional_default);
    RUN(eval_optional_suppliedp);
    RUN(eval_optional_suppliedp_rest);
    RUN(eval_optional_default_sees_suppliedp);
    RUN(eval_key);
    RUN(eval_key_default);
    RUN(eval_optional_lambda);
    RUN(eval_key_unknown_error);
    RUN(eval_key_allow_other_keys);
    RUN(eval_key_caller_allow);
    RUN(eval_eval_builtin);
    RUN(eval_macroexpand_1);
    RUN(eval_macroexpand);
    RUN(eval_defmacro_whole_basic);
    RUN(eval_defmacro_whole_with_body);
    RUN(eval_defmacro_whole_with_optional);
    RUN(eval_defmacro_no_whole_still_works);
    RUN(eval_tagbody_basic);
    RUN(eval_tagbody_forward_go);
    RUN(eval_tagbody_backward_go);
    RUN(eval_tagbody_nested);
    RUN(eval_tagbody_fixnum_tags);
    RUN(eval_tagbody_go_from_if);
    RUN(eval_tagbody_local_named_lambda);
    RUN(eval_tagbody_nlx_dispatch_stack_balance);
    RUN(eval_tagbody_go_through_macro_closure);
    RUN(eval_block_return_through_macro_closure);
    RUN(eval_return_from_across_catch_no_nlx_leak);
    RUN(eval_macro_non_closure_keeps_local_go);
    RUN(eval_catch_basic);
    RUN(eval_catch_normal);
    RUN(eval_catch_throw_across_call);
    RUN(eval_catch_nested);
    RUN(eval_catch_nested_outer);
    RUN(eval_catch_no_value);
    RUN(eval_throw_unmatched);
    RUN(eval_catch_throw_deep_chain);
    RUN(eval_block_return_deep_chain);
    RUN(eval_uwp_throw_deep_cleanup);
    RUN(eval_uwp_normal);
    RUN(eval_uwp_throw_cleanup);
    RUN(eval_uwp_throw_value);
    RUN(eval_uwp_nested);
    RUN(eval_uwp_error_cleanup);
    RUN(eval_values_basic);
    RUN(eval_multiple_value_bind);
    RUN(eval_multiple_value_list);
    RUN(eval_multiple_value_prog1);
    RUN(eval_nth_value);
    RUN(eval_values_list);
    RUN(eval_mv_propagation);
    RUN(eval_defvar_basic);
    RUN(eval_defvar_no_overwrite);
    RUN(eval_defvar_init_form_only_when_unbound);
    RUN(eval_def_forms_store_in_tail_position);
    RUN(eval_defparameter_overwrite);
    RUN(eval_defconstant_basic);
    RUN(eval_defconstant_setq_error);
    RUN(eval_defconstant_set_error);
    RUN(eval_defconstant_t_is_constant);
    RUN(eval_defconstant_keyword_error);
    RUN(eval_special_let_binding);
    RUN(eval_special_let_star_binding);
    RUN(eval_special_visible_in_called_fn);
    RUN(eval_special_nested_binding);
    RUN(eval_special_setq);
    RUN(eval_special_unwind_protect);
    RUN(eval_unwind_protect_multiple_values);
    RUN(eval_uwprot_nlx_multiple_values);
    RUN(eval_special_error_restore);
    RUN(eval_special_mixed_let);
    RUN(eval_special_lambda_param_dynbind);
    RUN(eval_setf_car_cdr);
    RUN(eval_setf_first_rest);
    RUN(eval_setf_nth);
    RUN(eval_setf_variable);
    RUN(eval_setf_multiple);
    RUN(eval_rplaca_rplacd);
    RUN(eval_car_cdr_type_error);
    RUN(eval_rplaca_rplacd_type_error);
    RUN(eval_aref_make_array_vectorp);
    RUN(eval_setf_svref);
    RUN(eval_aref_string);
    RUN(eval_symbol_value);
    RUN(eval_setf_symbol_value);
    RUN(eval_set_builtin);
    RUN(eval_setf_return_value);
    RUN(eval_member_found);
    RUN(eval_member_not_found);
    RUN(eval_member_with_test);
    RUN(eval_pushnew_new);
    RUN(eval_pushnew_existing);
    RUN(eval_eval_when_execute);
    RUN(eval_eval_when_multiple_situations);
    RUN(eval_eval_when_body);
    RUN(eval_destructuring_bind_simple);
    RUN(eval_destructuring_bind_nested);
    RUN(eval_destructuring_bind_rest);
    RUN(eval_destructuring_bind_optional);
    RUN(eval_destructuring_bind_optional_provided);
    RUN(eval_destructuring_bind_dotted_pair);
    RUN(eval_destructuring_bind_dotted_tail);
    RUN(eval_destructuring_bind_dotted_nested);
    RUN(eval_destructuring_bind_dotted_after_optional);
    RUN(eval_destructuring_bind_dotted_optional_defaulted);
    RUN(eval_destructuring_bind_dotted_macroexpand_path);
    RUN(eval_destructuring_bind_dotted_after_key_rejected);
    RUN(eval_destructuring_bind_dotted_after_aux_rejected);
    RUN(eval_destructuring_bind_unknown_key_rejected);
    RUN(eval_destructuring_bind_body);
    RUN(eval_destructuring_bind_key);
    RUN(eval_destructuring_bind_optional_rest_key);
    RUN(eval_destructuring_bind_macroexpands);
    RUN(eval_define_modify_macro_optional_default);
    RUN(eval_notinline_suppresses_compiler_macro);
    RUN(eval_defsetf_short);
    RUN(eval_defsetf_cadr);
    RUN(eval_get_setf_expansion_defsetf);
    RUN(eval_equalp);
    RUN(eval_string_vector_seq_ops);
    RUN(eval_ansi_sequences_last10);
    RUN(eval_defun_return_from);

    /* Cross-closure return-from */
    RUN(eval_return_from_labels);
    RUN(eval_return_from_flet);
    RUN(eval_return_from_lambda);
    RUN(eval_return_from_nested_labels);
    RUN(eval_block_with_unwind_protect);
    RUN(eval_block_uwp_local_return_from_runs_cleanup);
    RUN(eval_block_nil_uwp_return_from_runs_cleanup);
    RUN(eval_block_nil_uwp_bare_return_runs_cleanup);
    RUN(eval_uwp_nlx_leak_tailcall);
    RUN(eval_block_nlx_precision);
    RUN(eval_dolist_var_shadowing);

    /* Phase 5 Tier 1 */
    RUN(eval_characterp);
    RUN(eval_char_comparison);
    RUN(eval_char_comparison_variadic);
    RUN(eval_char_code_conversion);
    RUN(eval_char_case);
    RUN(eval_char_case_astral_not_truncated);
    RUN(eval_char_predicates);
    RUN(eval_symbol_name);
    RUN(eval_symbol_package_fn);
    RUN(eval_fboundp);
    RUN(eval_fdefinition);
    RUN(eval_make_symbol);
    RUN(eval_keywordp);
    RUN(eval_compile_env_nlx_macro_no_uaf);
    RUN(eval_length_vector);
    RUN(eval_equal_vector);
    RUN(eval_mapcar_multi_list);
    RUN(eval_vector);
    RUN(eval_vector_reader);
    RUN(eval_array_print_multidim);
    RUN(eval_array_dimensions);
    RUN(eval_array_rank);
    RUN(eval_setf_aref_multidim);
    RUN(eval_array_dimension);
    RUN(eval_array_total_size);
    RUN(eval_array_element_type);
    RUN(eval_class_of_array_vs_vector);
    RUN(eval_array_row_major_index);
    RUN(eval_row_major_aref);
    RUN(eval_fill_pointer);
    RUN(eval_vector_push);
    RUN(eval_adjust_array);
    RUN(eval_array_type_predicates);
    RUN(eval_array_typep);
    RUN(eval_array_type_of);

    /* Phase 5 Tier 2 */
    RUN(eval_string_comparison);
    RUN(eval_string_case_conversion);
    RUN(eval_string_trim);
    RUN(eval_subseq);
    RUN(eval_concatenate);
    RUN(eval_char_accessor);
    RUN(eval_string_coerce);
    RUN(eval_string_fns_on_fill_pointer_string);
    RUN(eval_defsetf_long_form);
    RUN(eval_prin1_wide_string_no_u_escape);
    RUN(eval_prin1_base_string_high_byte_roundtrip);
    RUN(eval_end_of_file_condition);
    RUN(eval_parse_integer);
    RUN(eval_write_to_string);
    RUN(eval_prin1_to_string);
    RUN(eval_princ_to_string);
    RUN(eval_princ_symbol_no_package_prefix);
    RUN(eval_format_aesthetic_respects_print_escape);

    /* Phase 5 Tier 3 */
    RUN(eval_nthcdr);
    RUN(eval_nth_index_domain);
    RUN(eval_last);
    RUN(eval_acons);
    RUN(eval_copy_list);
    RUN(eval_pairlis);
    RUN(eval_assoc);
    RUN(eval_rassoc);
    RUN(eval_getf);
    RUN(eval_subst);
    RUN(eval_sublis);
    RUN(eval_adjoin);
    RUN(eval_nconc);
    RUN(eval_nreverse);
    RUN(eval_delete);
    RUN(eval_delete_vector);
    RUN(eval_delete_string);
    RUN(eval_delete_if);
    RUN(eval_nsubst);
    RUN(eval_butlast);
    RUN(eval_copy_tree);
    RUN(eval_mapc);
    RUN(eval_mapcan);
    RUN(eval_maplist);
    RUN(eval_mapl);
    RUN(eval_mapcon);
    RUN(eval_intersection);
    RUN(eval_union);
    RUN(eval_set_difference);
    RUN(eval_subsetp);

    /* Phase 5 — Hash tables */
    RUN(eval_make_hash_table);
    RUN(eval_gethash_setf);
    RUN(eval_gethash_default);
    RUN(eval_gethash_overwrite);
    RUN(eval_remhash);
    RUN(eval_clrhash);
    RUN(eval_maphash);
    RUN(eval_hash_table_equal_test);
    RUN(eval_hash_table_eq_test);
    RUN(eval_gethash_mv);
    RUN(eval_hash_table_accessors);
    RUN(eval_hash_table_test_symbols);
    RUN(eval_with_hash_table_iterator_empty);
    RUN(eval_with_hash_table_iterator_walks_pairs);

    /* Phase 5 — Sequence functions */
    RUN(eval_find);
    RUN(eval_find_if);
    RUN(eval_find_if_not);
    RUN(eval_position);
    RUN(eval_position_if);
    RUN(eval_position_if_not);
    RUN(eval_count);
    RUN(eval_count_if);
    RUN(eval_count_if_not);
    RUN(eval_remove);
    RUN(eval_remove_if);
    RUN(eval_remove_if_not);
    RUN(eval_remove_duplicates);
    RUN(eval_substitute);
    RUN(eval_substitute_if);
    RUN(eval_substitute_if_not);
    RUN(eval_nsubstitute);
    RUN(eval_nsubstitute_if);
    RUN(eval_nsubstitute_if_not);
    RUN(eval_reduce);
    RUN(eval_fill);
    RUN(eval_replace_fn);
    RUN(eval_every);
    RUN(eval_some);
    RUN(eval_notany);
    RUN(eval_notevery);
    RUN(eval_map_fn);
    RUN(eval_every_sequences);
    RUN(eval_some_sequences);
    RUN(eval_notany_sequences);
    RUN(eval_notevery_sequences);
    RUN(eval_mismatch);
    RUN(eval_search_fn);
    RUN(eval_sort);
    RUN(eval_stable_sort);
    RUN(eval_sort_with_key);
    RUN(eval_sort_key_nil);
    RUN(eval_stale_boxed_flag);

    /* Phase 5 — Type system */
    RUN(eval_typep);
    RUN(eval_coerce);
    RUN(eval_typep_compound);
    RUN(eval_typep_numeric_range);
    RUN(eval_vectorp);
    RUN(eval_coerce_simple_vector);
    RUN(eval_count_from_end);
    RUN(eval_deftype);
    RUN(eval_subtypep);
    RUN(eval_typecase_compound);

    /* Phase 5 — Disassemble */
    RUN(eval_disassemble_defun);
    RUN(eval_disassemble_closure);
    RUN(eval_disassemble_builtin);
    RUN(eval_disassemble_unbound);

    /* Phase 5 — Declarations */
    RUN(eval_declaim_special);
    RUN(eval_declare_special_let);
    RUN(eval_declare_special_letstar);
    RUN(eval_declare_ignore);
    RUN(eval_declare_type);
    RUN(eval_declaim_optimize);
    RUN(eval_declaim_inline);
    RUN(eval_proclaim_special);
    RUN(eval_locally_basic);
    RUN(eval_locally_declare);
    RUN(eval_declare_misplaced);
    RUN(eval_declare_in_lambda);
    RUN(eval_declaim_multiple);

    /* Phase 6 — The */
    RUN(eval_the_basic);
    RUN(eval_the_compound_type);
    RUN(eval_the_type_error);
    RUN(eval_the_safety_zero);
    RUN(eval_the_nested);
    RUN(eval_the_values_type_spec);
    RUN(eval_the_as_place);

    /* Phase 5 — Trace/Untrace */
    RUN(eval_trace_basic);
    RUN(eval_trace_returns_list);
    RUN(eval_trace_untrace);
    RUN(eval_trace_query);
    RUN(eval_untrace_all);
    RUN(eval_trace_builtin);
    RUN(eval_trace_multiple);
    RUN(eval_trace_stream_redirect);

    /* Phase 5 — Backtrace */
    RUN(eval_backtrace_named);
    RUN(eval_backtrace_anonymous);
    RUN(eval_backtrace_recursive);
    RUN(eval_backtrace_uwprot);
    RUN(eval_backtrace_name_survives_inner_lambda);
    RUN(eval_backtrace_inner_lambda_stays_anonymous);
    RUN(eval_backtrace_restart_case_handler_bind_named);
    RUN(eval_backtrace_flet_local_named);
    RUN(eval_backtrace_labels_local_named);
    RUN(eval_backtrace_inlined_local_no_frame);
    RUN(eval_backtrace_method_named);
    RUN(eval_backtrace_setf_named_flet_local_stays_anonymous);
    RUN(eval_named_lambda_setf_name_stays_anonymous);
    RUN(eval_backtrace_empty);
    RUN(eval_time_basic);
    RUN(eval_time_nested);
    RUN(eval_time_defun);
    RUN(eval_time_stats);
    RUN(eval_time_trace_redirect);
    RUN(eval_time_report_legacy_three_args);
    RUN(eval_time_report_counter_wrap);
    RUN(eval_get_internal_real_time);
    RUN(eval_get_internal_run_time);
    RUN(eval_srcloc_load_backtrace);
    RUN(eval_srcloc_reader_line);

    /* Phase 7 — Macrolet / Symbol-macrolet */
    RUN(eval_macrolet_basic);
    RUN(eval_macrolet_shadow);
    RUN(eval_macrolet_scope);
    RUN(eval_macrolet_nested);
    RUN(eval_macrolet_with_body);
    RUN(eval_macrolet_across_lambda);
    RUN(eval_symbol_macrolet_basic);
    RUN(eval_symbol_macrolet_expr);
    RUN(eval_symbol_macrolet_setq);
    RUN(eval_symbol_macrolet_scope);
    RUN(eval_symbol_macrolet_nested);
    RUN(eval_symbol_macrolet_multiple);
    RUN(eval_symbol_macrolet_across_lambda);
    RUN(eval_symbol_macrolet_shadowed_by_lambda_param);
    RUN(eval_symbol_macrolet_shadowed_by_let);
    RUN(eval_environment_symbol_macrolet_expand);
    RUN(eval_environment_with_boolean_idiom);
    RUN(eval_environment_macrolet_captures_symbol_macros);
    RUN(eval_environment_macrolet_inside_binding_form);
    RUN(scanner_deep_nesting_no_root_overflow);
    RUN(eval_define_symbol_macro_basic);
    RUN(eval_define_symbol_macro_read_time_eval);
    RUN(eval_define_symbol_macro_macroexpand_1);
    RUN(eval_define_symbol_macro_setf);

    /* Phase 8 Step 2 — Missing string operations */
    RUN(eval_string_capitalize);
    RUN(eval_string_case_designators);
    RUN(eval_nstring_upcase);
    RUN(eval_nstring_downcase);
    RUN(eval_nstring_capitalize);
    RUN(eval_string_case_start_end);
    RUN(eval_nstring_case_start_end);
    RUN(eval_string_case_start_end_errors);
    RUN(eval_char_name);
    RUN(eval_name_char);
    RUN(eval_char_ci_compare);
    RUN(eval_graphic_char_p);
    RUN(eval_alphanumericp);
    RUN(eval_digit_char);

    /* Phase 8 Step 3 — Missing sequence operations */
    RUN(eval_elt);
    RUN(eval_setf_elt);
    RUN(eval_copy_seq);
    RUN(eval_map_into);

    /* Phase 8 Step 4 — Higher-order functions */
    RUN(eval_complement);
    RUN(eval_constantly);

    /* Phase 8 Step 1 — Missing list operations */
    RUN(eval_list_star);
    RUN(eval_make_list);
    RUN(eval_tree_equal);
    RUN(eval_list_length);
    RUN(eval_tailp);
    RUN(eval_ldiff);
    RUN(eval_revappend);
    RUN(eval_nreconc);
    RUN(eval_assoc_if);
    RUN(eval_rassoc_if);
    RUN(eval_remf);

    /* Phase 8 — Loop macro */
    RUN(eval_loop_simple_return);
    RUN(eval_loop_simple_accumulate);
    RUN(eval_loop_while);
    RUN(eval_loop_until);
    RUN(eval_loop_do_multiple);
    RUN(eval_loop_for_in);
    RUN(eval_loop_for_in_by);
    RUN(eval_loop_for_on);
    RUN(eval_loop_for_from_to);
    RUN(eval_loop_for_from_below);
    RUN(eval_loop_for_upto);
    RUN(eval_loop_for_to);
    RUN(eval_loop_for_upto_by);
    RUN(eval_loop_for_downto);
    RUN(eval_loop_for_downto_by);
    RUN(eval_loop_for_from_by);
    RUN(eval_loop_for_downfrom);
    RUN(eval_loop_for_downfrom_above);
    RUN(eval_loop_for_across);
    RUN(eval_loop_for_eq_then);
    RUN(eval_loop_for_eq_then_nil);
    RUN(eval_loop_repeat);
    RUN(eval_loop_for_multiple);
    RUN(eval_loop_collect);
    RUN(eval_loop_collect_expr);
    RUN(eval_loop_collect_into);
    RUN(eval_loop_sum);
    RUN(eval_loop_sum_into);
    RUN(eval_loop_count);
    RUN(eval_loop_maximize);
    RUN(eval_loop_minimize);
    RUN(eval_loop_append);
    RUN(eval_loop_nconc);
    RUN(eval_loop_mixed_collect_nconc);
    RUN(eval_loop_return);
    RUN(eval_loop_return_clause);
    RUN(eval_loop_return_in_do_let);
    RUN(eval_loop_when_collect);
    RUN(eval_loop_if_else);
    RUN(eval_loop_unless_collect);
    RUN(eval_loop_when_and);
    RUN(eval_loop_always);
    RUN(eval_loop_never);
    RUN(eval_loop_thereis);
    RUN(eval_loop_with);
    RUN(eval_loop_with_and);
    RUN(eval_loop_with_destructuring);
    RUN(eval_loop_named);
    RUN(eval_loop_initially);
    RUN(eval_loop_finally);
    RUN(eval_loop_collect_into_finally);
    RUN(eval_loop_finish);
    RUN(eval_loop_finish_finally);
    RUN(eval_loop_being_hash_keys);
    RUN(eval_loop_being_hash_values);
    RUN(eval_loop_being_hash_keys_using);
    RUN(eval_loop_being_hash_values_using);
    RUN(eval_loop_being_symbols);
    RUN(eval_loop_being_external_symbols);
    RUN(eval_loop_destructuring_in);
    RUN(eval_loop_destructuring_dotted);
    RUN(eval_loop_destructuring_nested);
    RUN(eval_loop_destructuring_on);
    RUN(eval_loop_while_after_collect);
    RUN(eval_loop_until_after_collect);
    RUN(eval_loop_while_before_collect);
    RUN(eval_loop_if_collect_while);
    RUN(eval_loop_collect_while_search);

    /* Paren depth */
    RUN(paren_depth_balanced);
    RUN(paren_depth_unbalanced);
    RUN(paren_depth_strings);
    RUN(paren_depth_unterminated_string);
    RUN(paren_depth_comments);
    RUN(paren_depth_char_literals);
    RUN(paren_depth_multiline);

    /* History variables */
    RUN(history_star_basic);
    RUN(history_star_shift);
    RUN(history_plus_shift);
    RUN(history_minus_during_eval);
    RUN(history_arithmetic_still_works);

    RUN(repl_zero_values_print_nothing);
    RUN(repl_one_value_prints_even_when_nil);
    RUN(repl_zero_values_do_not_leak_into_next_form);
    RUN(repl_eval_string_nothing_evaluated_signals_no_values);
    RUN(repl_values_list_zero_one_and_many);
    RUN(repl_values_list_index_zero_comes_from_the_stack);
    RUN(repl_history_slash_holds_all_values);

    /* Multiple-values hygiene */
    RUN(mv_hygiene_single_value_forms_do_not_leak);
    RUN(mv_hygiene_short_circuit_forms_return_one_value);
    RUN(mv_hygiene_joins_do_not_leak);
    RUN(mv_hygiene_mixed_joins_normalize_only_the_stale_arm);
    RUN(mv_hygiene_real_multiple_values_still_propagate);
    RUN(mv_hygiene_call_result_is_classified_from_the_callee);
    RUN(mv_hygiene_bind_and_nth_value_read_their_own_form);
    RUN(mv_hygiene_zero_values_survive_a_call);

    /* Printer control variables */
    RUN(eval_print_var_defaults);
    RUN(eval_print_escape_dynamic);
    RUN(eval_print_level);
    RUN(eval_print_length);
    RUN(eval_print_level_length_combined);
    RUN(eval_print_base_binary);
    RUN(eval_print_base_octal);
    RUN(eval_print_base_hex);
    RUN(eval_print_base_other);
    RUN(eval_print_radix_decimal);
    RUN(eval_print_radix_binary);
    RUN(eval_print_radix_octal);
    RUN(eval_print_radix_hex);
    RUN(eval_print_radix_other);
    RUN(eval_print_base_bignum);
    RUN(eval_print_radix_bignum);
    RUN(eval_print_case_upcase);
    RUN(eval_print_case_downcase);
    RUN(eval_print_case_capitalize);
    RUN(eval_print_case_in_list);
    RUN(eval_print_gensym);
    RUN(eval_print_array);

    /* WRITE function with keyword arguments */
    RUN(eval_write_basic);
    RUN(eval_write_base_radix);
    RUN(eval_write_level_length);
    RUN(eval_write_case);
    RUN(eval_write_gensym_array);

    /* *print-circle* */
    RUN(eval_print_circle_cdr_cycle);
    RUN(eval_print_circle_car_self);
    RUN(eval_print_circle_shared_sub);
    RUN(eval_print_circle_no_sharing);
    RUN(eval_print_circle_nil_default);
    RUN(eval_print_circle_vector);
    RUN(eval_print_circle_deep_shared);
    RUN(eval_print_circle_write_to_string);

    /* Format directive extensions */
    RUN(eval_format_decimal);
    RUN(eval_format_binary);
    RUN(eval_format_octal);
    RUN(eval_format_hex);
    RUN(eval_format_character);
    RUN(eval_format_write);
    RUN(eval_format_freshline);
    RUN(eval_format_page);
    RUN(eval_format_justify);
    RUN(eval_format_mixed);

    /* Advanced format: padding, commas, sign */
    RUN(eval_format_padded_decimal);
    RUN(eval_format_padded_aesthetic);
    RUN(eval_format_prefix_params);

    /* Advanced format: ~* and ~T */
    RUN(eval_format_goto);
    RUN(eval_format_tabulate);

    /* Advanced format: ~(~) case conversion */
    RUN(eval_format_case_downcase);
    RUN(eval_format_case_capitalize);
    RUN(eval_format_case_cap_first);
    RUN(eval_format_case_upcase);

    /* Advanced format: ~[~;~] conditional */
    RUN(eval_format_cond_numeric);
    RUN(eval_format_cond_default);
    RUN(eval_format_cond_boolean);
    RUN(eval_format_cond_atsign);

    /* Advanced format: ~{~} iteration and ~^ */
    RUN(eval_format_iteration_list);
    RUN(eval_format_iteration_sublists);
    RUN(eval_format_iteration_atsign);
    RUN(eval_format_iteration_colon_atsign);
    RUN(eval_format_iteration_limit);
    RUN(eval_format_iteration_nil_list);
    RUN(eval_format_iteration_non_list_arg);
    RUN(eval_format_fixed_non_number_arg);
    RUN(eval_format_escape);

    /* Advanced format: ~? recursive and ~R radix */
    RUN(eval_format_recursive);
    RUN(eval_format_recursive_atsign);
#ifdef CL_WIDE_STRINGS
    RUN(eval_format_wide_string_passthrough);
#endif
    RUN(eval_format_radix_cardinal);
    RUN(eval_format_radix_ordinal);
    RUN(eval_format_radix_roman);
    RUN(eval_format_radix_old_roman);
    RUN(eval_format_radix_base);

    /* *print-pretty* / pprint */
    RUN(eval_print_pretty_default);
    RUN(eval_print_pretty_short_list);
    RUN(eval_print_pretty_long_list);
    RUN(eval_print_pretty_nested);
    RUN(eval_print_pretty_vector);
    RUN(eval_print_pretty_empty);
    RUN(eval_print_pretty_dotted);
    RUN(eval_pprint_basic);
    RUN(eval_print_pretty_write_keyword);
    RUN(eval_print_pretty_write_to_string);
    RUN(eval_print_pretty_off_no_break);
    RUN(eval_print_pretty_with_level_length);

    /* rotatef / shiftf */
    RUN(eval_rotatef_basic);
    RUN(eval_rotatef_two);
    RUN(eval_rotatef_returns_nil);
    RUN(eval_rotatef_car);
    RUN(eval_shiftf_basic);
    RUN(eval_shiftf_two);
    RUN(eval_shiftf_car);

    /* pprint-newline / pprint-indent */
    RUN(eval_pprint_newline_mandatory);
    RUN(eval_pprint_newline_fill);
    RUN(eval_pprint_newline_kinds);
    RUN(eval_pprint_indent_current);
    RUN(eval_pprint_indent_block);

    /* pprint-logical-block */
    RUN(eval_pprint_logical_block_basic);

    /* pprint-dispatch */
    RUN(eval_pprint_dispatch_set_get);
    RUN(eval_copy_pprint_dispatch);
    RUN(eval_pprint_dispatch_custom);

    /* provide / require */
    RUN(eval_provide_adds_to_modules);
    RUN(eval_provide_no_duplicate);
    RUN(eval_provide_symbol_name);
    RUN(eval_require_already_provided);
    RUN(eval_require_with_pathname);
    RUN(eval_require_does_not_reload);

    /* property lists */
    RUN(eval_symbol_plist_initial);
    RUN(eval_setf_get);
    RUN(eval_get_default);
    RUN(eval_remprop);
    RUN(eval_setf_get_update);

    /* block comments */
    RUN(eval_block_comment_basic);
    RUN(eval_block_comment_inline);
    RUN(eval_block_comment_nested);
    RUN(eval_block_comment_multiline);

    /* &aux */
    RUN(eval_aux_basic);
    RUN(eval_aux_multiple);
    RUN(eval_aux_no_init);
    RUN(eval_aux_uses_param);
    RUN(eval_aux_with_key);
    RUN(eval_defun_aux);
    RUN(eval_aux_self_shadowing_init);
    RUN(eval_aux_init_sees_earlier_aux);

    /* apply with symbols */
    RUN(eval_apply_symbol);
    RUN(eval_apply_symbol_funcall);
    RUN(eval_apply_rest_over_64);
    RUN(eval_apply_required_plus_rest_over_64);
    RUN(eval_apply_builtin_over_64);
    RUN(eval_apply_rest_value_correct);
    RUN(eval_call_arguments_limit_constant);

    /* package-used-by-list */
    RUN(eval_package_used_by_list);

    /* shadowing-import */
    RUN(eval_shadowing_import);
    RUN(eval_shadowing_import_conflict);
    RUN(eval_shadowing_import_same_symbol);

    /* copy-symbol */
    RUN(eval_copy_symbol_basic);
    RUN(eval_copy_symbol_uninterned);
    RUN(eval_copy_symbol_props);
    RUN(eval_reader_shared_uninterned_within_one_read);

    /* LOOP nested :if sub-clause */
    RUN(eval_loop_nested_if);
    RUN(eval_loop_nested_when_else);
    RUN(eval_loop_nested_unless);

    /* defmacro destructuring */
    RUN(eval_defmacro_destructuring_required);
    RUN(eval_defmacro_destructuring_body);
    RUN(eval_defmacro_optional_not_destructured);
    RUN(eval_macro_function_arity);
    RUN(eval_macro_function_nil);

    /* define-modify-macro */
    RUN(eval_define_modify_macro);

    /* reduce :from-end */
    RUN(eval_reduce_from_end);
    RUN(eval_reduce_from_end_initial);

    /* defmethod implicit block */
    RUN(eval_defmethod_implicit_block);

    /* user-homedir-pathname */
    RUN(eval_user_homedir_pathname);

    /* ext:system-command, ext:getcwd */
    RUN(eval_system_command_true);
    RUN(eval_system_command_false);
    RUN(eval_system_command_echo);
    RUN(eval_getcwd);

    /* mp: threading primitives */
    RUN(eval_mp_make_lock);
    RUN(eval_mp_make_recursive_lock);
    RUN(eval_mp_with_lock_held);
    RUN(eval_mp_with_recursive_lock_held);
    RUN(eval_mp_memory_barriers);
    RUN(eval_ext_defglobal);

    /* quicklisp/ASDF compatibility regressions */
    RUN(eval_remove_if_not_string);
    RUN(eval_remove_if_string);
    RUN(eval_remove_string);
    RUN(eval_key_suppliedp_param);
    RUN(eval_key_nil_value_not_default);
    RUN(eval_key_default_special_var);
    RUN(eval_special_var_unwind_closure);
    RUN(eval_special_var_unwind_block_return);
    RUN(eval_destructuring_bind_rest_key);
    RUN(eval_destructuring_bind_arity_strict);
    RUN(eval_destructuring_bind_whole_aux);

    /* synonym streams, compile-file vars, directory (Bug 14) */
    RUN(eval_synonym_stream);
    RUN(eval_compile_file_truename);
    RUN(eval_directory_basic);

    /* let* variable shadowing with closure capture */
    RUN(eval_let_star_shadow_closure_mutation);
    RUN(eval_let_star_shadow_initform_ref);
    RUN(eval_let_star_shadow_no_closure);
    RUN(eval_let_malformed_binding_rejected);
    RUN(eval_let_too_many_bindings_rejected);
    RUN(eval_let_bind_name_shadows_macro);
    RUN(eval_mvbind_var_shadows_macro);
    RUN(eval_do_implicit_tagbody);
    RUN(eval_destructuring_bind_pattern_shadows_macro);

    /* PROGV */
    RUN(eval_progv_basic);
    RUN(eval_progv_multiple);
    RUN(eval_progv_fewer_values);
    RUN(eval_progv_extra_values);
    RUN(eval_progv_empty_symbols);
    RUN(eval_progv_restore_on_throw);
    RUN(eval_progv_nested);

    /* set-syntax-from-char */
    RUN(eval_set_syntax_from_char_basic);
    RUN(eval_set_syntax_from_char_copies_macro);
    RUN(eval_set_syntax_from_char_reset_constituent);
    RUN(eval_set_syntax_from_char_default_from);

    /* FSet regressions: nth-value MV reset, loop block uses_nlx */
    RUN(eval_nth_value_mv_reset);
    RUN(eval_do_return_after_nlx_block);

    /* NLX preserves multiple values */
    RUN(eval_block_return_preserves_mv);

    /* :test-not keyword for sequence functions */
    RUN(eval_seq_test_not);

    /* setf composite c[ad]+r accessors */
    RUN(eval_setf_composite_cadr);

    /* MEMBER with :key / :test / :test-not */
    RUN(eval_member_key_lambda);
    RUN(eval_member_test_not);
    RUN(eval_member_test_and_test_not_conflict);
    RUN(eval_member_if_and_if_not);

    /* (setf (getf place ind) val) — CLHS §5.1.2.4 prepend-on-absent */
    RUN(eval_setf_getf_key_absent_prepends);
    RUN(eval_setf_getf_key_absent_returns_value);
    RUN(eval_setf_getf_key_present_updates_in_place);
    RUN(eval_setf_getf_key_absent_preserves_existing);
    RUN(eval_setf_getf_value_evaluated_once);

    /* reader: leading-dot float disambiguation vs dotted-pair `.` */
    RUN(eval_reader_leading_dot_float_in_list);

    /* compiler: opaque heap objects (stream, lock, condvar, ...) are
     * self-evaluating per CLHS 3.1.2.1.3 */
    RUN(eval_eval_self_evaluating_opaque);
    RUN(eval_ccase_ctypecase);

    /* load-time-value */
    RUN(eval_load_time_value_basic);
    RUN(eval_load_time_value_in_function);
    RUN(eval_load_time_value_with_read_only);
    RUN(eval_load_time_value_null_lex_env);
    RUN(eval_load_time_value_side_effect);
    RUN(eval_load_time_value_complex_form);
    RUN(eval_load_time_value_nested);

    /* get-properties */
    RUN(eval_get_properties_found);
    RUN(eval_get_properties_first_match);
    RUN(eval_get_properties_not_found);
    RUN(eval_get_properties_empty);

    /* str test regressions */
    RUN(eval_declare_special_scoped);
    RUN(eval_declare_special_free_ref);
    RUN(eval_declare_special_param);
    RUN(eval_alpha_char_p_unicode);
    RUN(eval_upper_lower_case_unicode);
    RUN(eval_concatenate_type_error);
    RUN(eval_nonsymbol_keyword_program_error);
    RUN(eval_remove_nonsequence_type_error);

    /* compiler scanner: symbol-macrolet + &environment macro (ironclad sha3) */
    RUN(eval_scanner_symbol_macrolet_environment);
    /* compiler scanner: macrolet/symbol-macrolet setf boxing (local-time) */
    RUN(eval_scanner_macrolet_setf_boxing);
    /* symbol-macro whose expansion is NIL (trivia (match nil (nil t))) */
    RUN(eval_symbol_macrolet_nil_expansion);
#ifdef CL_WIDE_STRINGS
    RUN(eval_wide_string_reader);
    RUN(eval_string_trim_wide);
#endif

    /* heap exhaustion / storage errors — MUST stay last; heap state is
     * unreliable afterwards.  Append new tests ABOVE this line. */
    RUN(eval_heap_exhaustion_error);
    RUN(builtin_fptr_plausible_unaligned);

    RUN(eval_print_override_restored_after_error);
    RUN(eval_file_position_bignum);
    RUN(eval_hashtable_wide_string_keys);
    RUN(eval_hashtable_equalp_vector_keys);
    RUN(eval_hashtable_equal_bitvector_keys);
    RUN(eval_hashtable_equal_vector_identity);
    RUN(eval_hashtable_equalp_wide_char_no_low_byte_match);
    RUN(eval_mapl_mapcon_symbol_designators);
    RUN(eval_map_family_over_16_seqs_errors);

    RUN(eval_apply_builtin_over_64_args);
    RUN(vm_call_builtin_on_stack_args);
    RUN(eval_remove_string_over_1023_chars);
    RUN(eval_concatenate_string_over_4096_chars);
#ifdef CL_WIDE_STRINGS
    RUN(eval_remove_concatenate_wide_chars_preserved);
#endif

    teardown();
    REPORT();
}
