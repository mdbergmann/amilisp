/* Regression tests for the LOOP macro's stepping semantics.
 *
 * Primary bug (surfaced by cl-spark's FIB tests): variables introduced by a
 * single FOR ... AND ... AND ... clause must be stepped in PARALLEL
 * (CLHS 6.1.2.1.4).  The previous expansion stepped them sequentially, so
 *
 *   (loop for f1 = 0 then f2 and f2 = 1 then (+ f1 f2) repeat n
 *         finally (return f1))
 *
 * computed powers of two (0 1 2 4 8 ...) instead of Fibonacci, because the
 * step of f2 saw the freshly-stepped f1.  These tests pin the parallel
 * behaviour while keeping ordinary sequential FOR clauses unchanged.
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
        cl_vm.sp = 0;
        cl_vm.fp = 0;
        snprintf(buf, sizeof(buf), "ERROR:%d", err);
        return buf;
    }
}

/* ================================================================
 * Parallel stepping of AND-connected FOR clauses
 * ================================================================ */

TEST(for_and_then_parallel_fib)
{
    /* The cl-spark FIB definition: must yield Fibonacci, not powers of 2. */
    ASSERT_STR_EQ(eval_print(
        "(loop for i from 0 to 8 collect"
        " (loop for f1 = 0 then f2 and f2 = 1 then (+ f1 f2)"
        "       repeat i finally (return f1)))"),
                  "(0 1 1 2 3 5 8 13 21)");
}

TEST(for_and_swap)
{
    /* Classic parallel-step swap: a and b exchange each iteration. */
    ASSERT_STR_EQ(eval_print(
        "(loop for a = 1 then b and b = 2 then a"
        "      repeat 3 finally (return (list a b)))"),
                  "(2 1)");
}

TEST(for_and_numeric_parallel)
{
    /* Two numeric steppers connected by AND advance from their own old
     * values, not each other's stepped values. */
    ASSERT_STR_EQ(eval_print(
        "(loop for i from 0 and j from 10"
        "      repeat 4 finally (return (list i j)))"),
                  "(4 14)");
}

TEST(for_then_first_iteration_uses_init)
{
    /* FOR var = init THEN step: the first body iteration sees INIT. */
    ASSERT_STR_EQ(eval_print(
        "(loop for x = 7 then (* x 2) repeat 4 collect x)"),
                  "(7 14 28 56)");
}

TEST(sequential_for_still_sequential)
{
    /* Separate FOR clauses (not joined by AND) step sequentially: j sees the
     * already-stepped i. */
    ASSERT_STR_EQ(eval_print(
        "(loop for i from 1 to 3 for j = (* i 10) collect j)"),
                  "(10 20 30)");
}

TEST(for_then_repeat_zero)
{
    /* REPEAT 0 runs the body zero times; FINALLY sees the initial values. */
    ASSERT_STR_EQ(eval_print(
        "(loop for f1 = 0 then f2 and f2 = 1 then (+ f1 f2)"
        "      repeat 0 finally (return f1))"),
                  "0");
}

TEST(for_destructuring_then)
{
    /* A destructured = THEN variable: the parts must follow the stepper,
     * not lag an iteration behind it (the first one used to see NIL). */
    ASSERT_STR_EQ(eval_print(
        "(loop for (side . other) = (cons :b :a) then (cons other side)"
        "      repeat 4 collect (list side other))"),
                  "((:B :A) (:A :B) (:B :A) (:A :B))");
    ASSERT_STR_EQ(eval_print(
        "(loop for (a b) = (list 1 2) then (list b (+ b 1))"
        "      while (< a 4) collect (list a b))"),
                  "((1 2) (2 3) (3 4))");
}

TEST(for_destructuring_then_and_from)
{
    /* Mixed AND group: the FROM stepper runs at the end of the iteration,
     * the destructuring in the next preamble sees the stepped pair. */
    ASSERT_STR_EQ(eval_print(
        "(loop for i from 0 and (p . q) = (cons :x :y) then (cons q p)"
        "      repeat 3 collect (list i p q))"),
                  "((0 :X :Y) (1 :Y :X) (2 :X :Y))");
}

TEST(sequential_then_chain)
{
    /* Separate (non-AND) FOR ... = ... THEN clauses must step in SOURCE
     * ORDER, each seeing the sibling's value from the correct point.  This
     * is cl-ppcre's create-matcher-aux idiom: curr takes the previous
     * iteration's `next`, and `next` is recomputed from curr.  Regression:
     * the THEN stepper must keep its position relative to the no-THEN
     * clause's preamble assignment (it must run before it here). */
    ASSERT_STR_EQ(eval_print(
        "(loop for e in '(a b c)"
        "      for curr = 'nf then next"
        "      for next = (list e curr)"
        "      finally (return next))"),
                  "(C (B (A NF)))");
}

TEST(then_step_uses_stepped_sibling)
{
    /* Sequential FOR clauses step in source order: i is stepped first, then
     * prev = i reads the freshly-stepped i (so prev tracks the current i,
     * not the previous one).  This pins the THEN stepper's position in the
     * preamble relative to the earlier clause's stepping. */
    ASSERT_STR_EQ(eval_print(
        "(loop for i from 1 to 4"
        "      for prev = 0 then i"
        "      collect (list i prev))"),
                  "((1 0) (2 2) (3 3) (4 4))");
}

TEST(for_and_mixed_then_from_parallel)
{
    /* CLHS 6.1.2.1.4: mixed AND group (= THEN + FROM) must step in parallel.
     * a's step (+ a j) must use the pre-step j, not j already incremented.
     * Correct: (0 1 3);  wrong (sequential): (0 2 5). */
    ASSERT_STR_EQ(eval_print(
        "(loop for a = 0 then (+ a j) and j from 1"
        "      repeat 3 collect a)"),
                  "(0 1 3)");
}

TEST(for_and_mixed_from_then_parallel)
{
    /* Same semantics with the clauses in reverse declaration order. */
    ASSERT_STR_EQ(eval_print(
        "(loop for j from 1 and a = 0 then (+ a j)"
        "      repeat 3 collect a)"),
                  "(0 1 3)");
}

/* ================================================================
 * GitHub #19: INTO accumulators are user-visible and must be in order
 *
 * An INTO variable is "like a variable established by WITH" (CLHS 6.1.3)
 * and can be read in the body and in FINALLY.  The list accumulators used
 * to build it reversed (push / NRECONC / REVAPPEND) and NREVERSE it only in
 * the loop epilogue, so the body saw (3 :K 2 :K 1 :K) and a (setf (getf
 * acc :k) v) on it corrupted the result.  INTO variables now carry a
 * hidden tail pointer and are spliced in order at every step; the
 * anonymous accumulator keeps the cheaper reverse-and-NREVERSE strategy.
 * ================================================================ */

TEST(into_collect_in_order_each_step)
{
    ASSERT_STR_EQ(eval_print(
        "(let (seen)"
        "  (loop for x in '(1 2 3) collect x into r do (push (copy-list r) seen))"
        "  (nreverse seen))"),
                  "((1) (1 2) (1 2 3))");
}

TEST(into_nconc_in_order_each_step)
{
    ASSERT_STR_EQ(eval_print(
        "(let (seen)"
        "  (loop for x in '(1 2) nconc (list :k x) into r do (push (copy-list r) seen))"
        "  (nreverse seen))"),
                  "((:K 1) (:K 1 :K 2))");
}

TEST(into_append_in_order_each_step)
{
    ASSERT_STR_EQ(eval_print(
        "(let (seen)"
        "  (loop for x in '(1 2) for y in '(a b)"
        "        append (list x y) into r do (push (copy-list r) seen))"
        "  (nreverse seen))"),
                  "((1 A) (1 A 2 B))");
}

TEST(into_nconc_plist_setf_getf_issue_19)
{
    /* The reporter's exact form. */
    ASSERT_STR_EQ(eval_print(
        "(loop :for e :in (list 1 2 3)"
        "      :nconc (list :elem e) :into acc"
        "      :when (= e 3) :do (setf (getf acc :elem) 10)"
        "      :finally (return acc))"),
                  "(:ELEM 10 :ELEM 2 :ELEM 3)");
}

TEST(into_return_from_body_in_order)
{
    /* No NREVERSE needed (or allowed) by the caller. */
    ASSERT_STR_EQ(eval_print(
        "(loop for x in '(1 2 3 4 5) collect x into r"
        "      do (when (= x 3) (return r)))"),
                  "(1 2 3)");
}

TEST(into_append_copies_argument)
{
    /* APPEND must not share structure with its argument (CLHS 6.1.3). */
    ASSERT_STR_EQ(eval_print(
        "(let ((src (list 1 2)))"
        "  (list (eq src (loop for i below 1 append src into acc"
        "                      finally (return acc)))"
        "        src))"),
                  "(NIL (1 2))");
}

TEST(into_nconc_and_append_skip_nil_chunks)
{
    /* A NIL chunk must not disturb the tail pointer — before or after the
       first real element. */
    ASSERT_STR_EQ(eval_print(
        "(loop for x in '(nil 1 nil 2 3 nil)"
        "      nconc (and x (list x)) into r finally (return r))"),
                  "(1 2 3)");
    ASSERT_STR_EQ(eval_print(
        "(loop for x in '(nil 1 nil 2 3 nil)"
        "      append (and x (list x)) into r finally (return r))"),
                  "(1 2 3)");
}

TEST(into_mixed_kinds_same_var)
{
    /* COLLECT, NCONC and APPEND into ONE variable share its tail pointer. */
    ASSERT_STR_EQ(eval_print(
        "(loop for x in '(1 2 3)"
        "      collect x into r"
        "      nconc (list (* x 10)) into r"
        "      append (list (* x 100)) into r"
        "      finally (return r))"),
                  "(1 10 100 2 20 200 3 30 300)");
}

TEST(into_shared_by_toplevel_and_conditional_clause)
{
    /* Both clause parsers (top-level and IF/WHEN sub-clause) must agree on
       the same hidden tail variable for the same INTO name. */
    ASSERT_STR_EQ(eval_print(
        "(loop for x in '(1 2 3) collect x into r"
        "      when (oddp x) collect 'x into r"
        "      finally (return r))"),
                  "(1 X 2 3 X)");
    ASSERT_STR_EQ(eval_print(
        "(loop for x in '(1 2) for y in '(a b)"
        "      if (oddp x) collect y into r and collect x into r"
        "      else nconc (list y x) into r"
        "      finally (return r))"),
                  "(A 1 B 2)");
}

TEST(into_after_loop_finish_and_empty)
{
    ASSERT_STR_EQ(eval_print(
        "(loop for x in '(1 2 3) collect x into r"
        "      do (when (= x 2) (loop-finish)) finally (return r))"),
                  "(1 2)");
    ASSERT_STR_EQ(eval_print(
        "(loop for x in nil collect x into r finally (return r))"),
                  "NIL");
}

/* CLHS 6.1.1.7: NIL as a loop variable ignores the value.  LOOP bound it
 * as (LET* ((NIL NIL)) ...), which LET rightly rejects -- serapeum's
 * LET-OVER-LAMBDA (loop for nil in preds collect (gensym)) did not compile. */
TEST(nil_variable_ignores_the_value)
{
    ASSERT_STR_EQ(eval_print("(loop for nil in '(1 2 3) collect 7)"), "(7 7 7)");
    ASSERT_STR_EQ(eval_print(
        "(loop for nil on '(1 2) for i from 0 collect i)"), "(0 1)");
    ASSERT_STR_EQ(eval_print("(loop for nil across #(1 2) count t)"), "2");
    ASSERT_STR_EQ(eval_print(
        "(loop for nil in '(a b) and i from 0 collect i)"), "(0 1)");
    /* NIL inside a destructuring pattern was already fine */
    ASSERT_STR_EQ(eval_print(
        "(loop for (nil . x) in '((1 . 2) (3 . 4)) collect x)"), "(2 4)");
}

TEST(anonymous_accumulator_unchanged)
{
    /* The default accumulator keeps its reverse-and-NREVERSE strategy and
       still mixes the three kinds in source order. */
    ASSERT_STR_EQ(eval_print(
        "(loop for x in '(1 2) collect x nconc (list (* x 10)) append (list 'a))"),
                  "(1 10 A 2 20 A)");
    ASSERT_STR_EQ(eval_print(
        "(let ((src (list 1 2))) (eq src (loop for i below 1 append src)))"),
                  "NIL");
}

int main(void)
{
    setup();

    RUN(for_and_then_parallel_fib);
    RUN(for_and_swap);
    RUN(for_and_numeric_parallel);
    RUN(for_then_first_iteration_uses_init);
    RUN(sequential_for_still_sequential);
    RUN(for_then_repeat_zero);
    RUN(for_destructuring_then);
    RUN(for_destructuring_then_and_from);
    RUN(sequential_then_chain);
    RUN(then_step_uses_stepped_sibling);
    RUN(for_and_mixed_then_from_parallel);
    RUN(for_and_mixed_from_then_parallel);

    RUN(into_collect_in_order_each_step);
    RUN(into_nconc_in_order_each_step);
    RUN(into_append_in_order_each_step);
    RUN(into_nconc_plist_setf_getf_issue_19);
    RUN(into_return_from_body_in_order);
    RUN(into_append_copies_argument);
    RUN(into_nconc_and_append_skip_nil_chunks);
    RUN(into_mixed_kinds_same_var);
    RUN(into_shared_by_toplevel_and_conditional_clause);
    RUN(into_after_loop_finish_and_empty);
    RUN(nil_variable_ignores_the_value);
    RUN(anonymous_accumulator_unchanged);

    teardown();
    REPORT();
}
