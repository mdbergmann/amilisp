/* Tests for spec 1.3: (declaim (optimize ...)) support —
 * emit-time speed-gated optimization.
 *
 *  - constant folding of pure builtin calls   (speed >= 1, the default)
 *  - dead-branch elimination for constant IF tests
 *  - safety-gated check elision (THE assert, destructuring-bind guards)
 *  - lexical scoping of body (declare (optimize ...)) vs global DECLAIM
 *
 * Behavior is validated against the HyperSpec: folding may never change
 * the value a form yields (CLHS 3.2.2.3 semantic constraints), local
 * declarations are scoped to their body (CLHS 3.3.4), and destructuring
 * mismatch errors are "should signal" — safe code only (CLHS 1.4.2.3). */

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
#include "core/stream.h"
#include "core/repl.h"
#include "core/thread.h"
#include "platform/platform.h"
#include "platform/platform_thread.h"
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
    cl_stream_init();
    cl_repl_init();
}

static void teardown(void)
{
    cl_stream_shutdown();
    cl_mem_shutdown();
    cl_thread_shutdown();
    platform_shutdown();
}

static const char *eval_str(const char *expr)
{
    static char buf[4096];
    int err;

    CL_CATCH(err);
    if (err == CL_ERR_NONE) {
        CL_Obj result = cl_eval_string(expr);
        cl_prin1_to_string(result, buf, sizeof(buf));
        CL_UNCATCH();
        return buf;
    } else {
        CL_UNCATCH();
        snprintf(buf, sizeof(buf), "ERROR:%d", err);
        return buf;
    }
}

/* Disassemble a zero-arg lambda wrapping EXPR and return the text, so
 * tests can assert on the emitted bytecode shape. */
static const char *disasm(const char *expr)
{
    static char src[1024];
    snprintf(src, sizeof(src),
             "(with-output-to-string (*standard-output*)"
             "  (disassemble (lambda () %s)))", expr);
    return eval_str(src);
}

/* --- Constant folding: values (must match runtime semantics) --- */

TEST(fold_arith_values)
{
    ASSERT_STR_EQ("3", eval_str("(+ 1 2)"));
    ASSERT_STR_EQ("13", eval_str("(+ 1 (* 2 3) (- 10 4))"));
    ASSERT_STR_EQ("-5", eval_str("(- 5)"));
    ASSERT_STR_EQ("42", eval_str("(- 50 5 3)"));
    ASSERT_STR_EQ("7", eval_str("(1+ (1- 7))"));
    ASSERT_STR_EQ("0", eval_str("(+)"));
    ASSERT_STR_EQ("1", eval_str("(*)"));
}

TEST(fold_logical_values)
{
    ASSERT_STR_EQ("8", eval_str("(logand 12 10)"));
    ASSERT_STR_EQ("14", eval_str("(logior 12 10)"));
    ASSERT_STR_EQ("6", eval_str("(logxor 12 10)"));
    ASSERT_STR_EQ("-1", eval_str("(logand)"));
    ASSERT_STR_EQ("0", eval_str("(logior)"));
    ASSERT_STR_EQ("1024", eval_str("(ash 1 10)"));
    /* ASH right shift is floor division (CLHS): (ash -17 -2) = -5, not -4 */
    ASSERT_STR_EQ("-5", eval_str("(ash -17 -2)"));
    ASSERT_STR_EQ("-1", eval_str("(ash -1 -40)"));
    ASSERT_STR_EQ("0", eval_str("(ash 1 -40)"));
}

TEST(fold_comparison_values)
{
    ASSERT_STR_EQ("T", eval_str("(< 1 2 3)"));
    ASSERT_STR_EQ("NIL", eval_str("(< 1 3 2)"));
    ASSERT_STR_EQ("T", eval_str("(<= 1 1 2)"));
    ASSERT_STR_EQ("NIL", eval_str("(> 1 1)"));
    ASSERT_STR_EQ("T", eval_str("(>= 3 3 2)"));
    ASSERT_STR_EQ("T", eval_str("(= 4 4 4)"));
    ASSERT_STR_EQ("NIL", eval_str("(= 4 4 5)"));
}

TEST(fold_not_null_values)
{
    ASSERT_STR_EQ("T", eval_str("(not nil)"));
    ASSERT_STR_EQ("NIL", eval_str("(not 5)"));
    ASSERT_STR_EQ("NIL", eval_str("(not '(1))"));
    ASSERT_STR_EQ("T", eval_str("(null '())"));
    ASSERT_STR_EQ("NIL", eval_str("(null 'x)"));
}

/* --- Constant folding: overflow / non-fixnum operands must decline --- */

TEST(fold_overflow_declines_to_runtime)
{
    /* Products/sums beyond fixnum range must reach the runtime bignum
     * path, not wrap at compile time. */
    ASSERT_STR_EQ("1000000000000", eval_str("(* 1000000 1000000)"));
    ASSERT_STR_EQ("1073741824", eval_str("(+ 1073741823 1)"));
    ASSERT_STR_EQ("-1073741825", eval_str("(- -1073741824 1)"));
    ASSERT_STR_EQ("1073741824", eval_str("(- -1073741824)"));
    ASSERT_STR_EQ("2147483648", eval_str("(ash 1 31)"));
    ASSERT_STR_EQ("4.0", eval_str("(+ 1.5 2.5)"));    /* floats not folded */
    ASSERT_STR_EQ("3/2", eval_str("(+ 1/2 1)"));      /* ratios not folded */
}

/* --- Constant folding: bytecode shape --- */

TEST(fold_emits_single_constant)
{
    const char *out = disasm("(+ 1 2 3)");
    ASSERT(strstr(out, "; 6") != NULL);       /* folded literal 6 */
    ASSERT(strstr(out, "FLOAD") == NULL);     /* no call to + ... */
    ASSERT(strstr(out, "CALL") == NULL);      /* ... in either call shape */
    ASSERT(strstr(out, "ADD") == NULL);       /* no inline add opcode */
}

TEST(fold_disabled_at_speed_0)
{
    const char *out = disasm(
        "(locally (declare (optimize (speed 0))) (+ 1 2 3))");
    /* Not folded: three-arg + compiles as a real call (a global call is
     * one fused CALL_GLOBAL since Tier-4 phase 2, not FLOAD + CALL) */
    ASSERT(strstr(out, "CALL_GLOBAL") != NULL);
    /* value still correct */
    ASSERT_STR_EQ("3",
        eval_str("(locally (declare (optimize (speed 0))) (+ 1 2))"));
}

TEST(fold_respects_side_effects)
{
    /* A non-constant argument (here: PRINC) must keep the whole call
     * un-folded and evaluated left-to-right at runtime. */
    ASSERT_STR_EQ("\"27\"", eval_str(
        "(with-output-to-string (*standard-output*)"
        "  (princ (+ 1 (princ 2) 4)))"));
}

TEST(fold_respects_shadowing_and_notinline)
{
    /* FLET shadow: + is the local function, not the builtin */
    ASSERT_STR_EQ("(1 2)", eval_str("(flet ((+ (a b) (list a b))) (+ 1 2))"));
    /* notinline: value unchanged, but folding is suppressed */
    ASSERT_STR_EQ("3", eval_str(
        "(locally (declare (notinline +)) (+ 1 2))"));
    {
        const char *out = disasm(
            "(locally (declare (notinline +)) (+ 1 2))");
        ASSERT(strstr(out, "CALL_GLOBAL") != NULL);   /* a real call */
    }
}

/* --- Dead-branch elimination --- */

TEST(dead_branch_values)
{
    ASSERT_STR_EQ(":LIVE", eval_str("(if (> 2 1) :live :dead)"));
    ASSERT_STR_EQ(":ELSE", eval_str("(if (= 1 2) (error \"dead\") :else)"));
    ASSERT_STR_EQ("NIL", eval_str("(if nil :dead)"));
    ASSERT_STR_EQ("NIL", eval_str("(when nil (error \"must not run\"))"));
    ASSERT_STR_EQ(":RAN", eval_str("(unless nil :ran)"));
}

TEST(dead_branch_not_emitted)
{
    const char *out = disasm("(if t :live (some-undefined-fn))");
    ASSERT(strstr(out, "JNIL") == NULL);      /* no runtime test */
    ASSERT(strstr(out, "SOME-UNDEFINED-FN") == NULL);  /* dead code gone */
    ASSERT(strstr(out, ":LIVE") != NULL);
}

TEST(dead_branch_disabled_at_speed_0)
{
    const char *out = disasm(
        "(locally (declare (optimize (speed 0))) (if t :live :dead))");
    ASSERT(strstr(out, "JNIL") != NULL);      /* real test emitted */
}

TEST(dead_branch_keeps_tail_position)
{
    /* The live branch of a folded IF stays in tail position: deep
     * self-recursion through it must not overflow the VM stack. */
    ASSERT_STR_EQ("DONE", eval_str(
        "(progn"
        "  (defun opt-tail-probe (n)"
        "    (if t"
        "        (if (= n 0) 'done (opt-tail-probe (- n 1)))"
        "        :dead))"
        "  (opt-tail-probe 100000))"));
}

/* --- Scoping of (declare (optimize ...)) --- */

TEST(local_declare_scopes_to_body)
{
    /* Inside the LOCALLY: safety 0 elides the THE check */
    ASSERT_STR_EQ("\"x\"", eval_str(
        "(locally (declare (optimize (safety 0))) (the fixnum \"x\"))"));
    /* After it: safety is back to 1, THE signals */
    ASSERT_STR_EQ(":SIGNALED", eval_str(
        "(handler-case (eval '(the fixnum \"y\")) (error () :signaled))"));
}

TEST(defun_declare_does_not_leak)
{
    eval_str("(defun opt-scope-probe () (declare (optimize (safety 0))) 42)");
    ASSERT_STR_EQ(":SIGNALED", eval_str(
        "(handler-case (eval '(the fixnum \"z\")) (error () :signaled))"));
}

TEST(let_declare_scopes_to_body)
{
    ASSERT_STR_EQ("\"v\"", eval_str(
        "(let ((x 1)) (declare (optimize (safety 0)) (ignorable x))"
        "  (the fixnum \"v\"))"));
    ASSERT_STR_EQ(":SIGNALED", eval_str(
        "(handler-case (eval '(the fixnum \"w\")) (error () :signaled))"));
}

TEST(declaim_is_global_and_persists)
{
    ASSERT_STR_EQ("NIL", eval_str("(declaim (optimize (safety 0)))"));
    ASSERT_STR_EQ("\"g\"", eval_str("(the fixnum \"g\")"));
    /* restore the default for the remaining tests */
    ASSERT_STR_EQ("NIL", eval_str("(declaim (optimize (safety 1)))"));
    ASSERT_STR_EQ(":SIGNALED", eval_str(
        "(handler-case (eval '(the fixnum \"h\")) (error () :signaled))"));
}

/* CLHS DECLAIM behaves like (eval-when (:compile-toplevel :load-toplevel
 * :execute) (proclaim 'spec) ...): the proclamations are made at compile
 * time ("also made at compile-time" for top-level forms in a file) AND
 * whenever the form executes — which includes LOAD of the compiled file.
 * The load-time leg must survive the FASL round trip: loading a fasl whose
 * source declaimed something must re-establish the proclamation in a
 * process where it is not currently in force.  (Regression: compile_declaim
 * used to emit only OP_NIL, so declaims evaporated from compiled files —
 * with a warm fasl cache a library's declaims silently stopped applying.) */
TEST(declaim_proclamations_survive_fasl_round_trip)
{
    char src[512], fasl[512], form[1200], tmpdir[400];
    const char *tmp = getenv("TMPDIR");
    FILE *f;
    if (!tmp || !tmp[0]) tmp = "/tmp";
    snprintf(tmpdir, sizeof(tmpdir), "%s", tmp);
    {
        /* These paths are embedded in the COMPILE-FILE form below, and
         * the Lisp reader eats a backslash as an escape — a Windows
         * $TMPDIR would arrive as "C:UsersyouTemp".  amilisp accepts
         * '/' on Windows, so normalise rather than escape. */
        char *p;
        for (p = tmpdir; *p; p++)
            if (*p == '\\') *p = '/';
    }
    snprintf(src, sizeof(src), "%s/cl_declaim_rt.lisp", tmpdir);
    snprintf(fasl, sizeof(fasl), "%s/cl_declaim_rt.fasl", tmpdir);
    f = fopen(src, "w");
    ASSERT(f != NULL);
    if (!f) return;
    fputs("(declaim (optimize (speed 3)) (special *dc-rt-var*))\n", f);
    fclose(f);

    snprintf(form, sizeof(form),
             "(compile-file \"%s\" :output-file \"%s\")", src, fasl);
    cl_eval_string(form);

    /* The compile-time leg (same process) already set speed=3 and marked
     * the var special — undo BOTH, so the assertions below can only pass
     * via the load-time leg executing out of the fasl. */
    cl_eval_string("(proclaim '(optimize (speed 1)))");
    {
        CL_Obj sym = cl_eval_string("'*dc-rt-var*");
        CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
        s->flags &= (uint32_t)~CL_SYM_SPECIAL;
    }
    ASSERT_EQ_INT(cl_optimize_global.speed, 1);

    snprintf(form, sizeof(form), "(load \"%s\")", fasl);
    cl_eval_string(form);

    ASSERT_EQ_INT(cl_optimize_global.speed, 3);
    {
        CL_Obj sym = cl_eval_string("'*dc-rt-var*");
        CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
        ASSERT((s->flags & CL_SYM_SPECIAL) != 0);
    }

    /* restore the default baseline for the remaining tests */
    cl_eval_string("(proclaim '(optimize (speed 1) (safety 1)))");
    remove(src);
    remove(fasl);
}

TEST(nested_scopes_restore_in_order)
{
    /* speed 0 outer disables folding; inner speed 1 re-enables; after the
     * inner body the outer speed 0 is back in force. */
    const char *out = disasm(
        "(locally (declare (optimize (speed 0)))"
        "  (list (locally (declare (optimize (speed 1))) (+ 1 2))"
        "        (+ 3 4)))");
    ASSERT(strstr(out, "; 3") != NULL);   /* inner folded */
    ASSERT(strstr(out, "; 7") == NULL);   /* outer not folded */
    ASSERT_STR_EQ("(3 7)", eval_str(
        "(locally (declare (optimize (speed 0)))"
        "  (list (locally (declare (optimize (speed 1))) (+ 1 2))"
        "        (+ 3 4)))"));
}

/* --- Safety-gated check elision: destructuring-bind guards --- */

TEST(dbind_guards_signal_at_default_safety)
{
    ASSERT_STR_EQ(":SIGNALED", eval_str(
        "(handler-case (eval '(destructuring-bind (a b) '(1) (list a b)))"
        "  (error () :signaled))"));
    ASSERT_STR_EQ(":SIGNALED", eval_str(
        "(handler-case (eval '(destructuring-bind (a) '(1 2) a))"
        "  (error () :signaled))"));
}

TEST(dbind_guards_elided_at_safety_0)
{
    /* CLHS 1.4.2.3 "should signal": unsafe code may skip the check.
     * Missing elements bind NIL; extra elements are ignored. */
    ASSERT_STR_EQ("(1 NIL)", eval_str(
        "(locally (declare (optimize (safety 0)))"
        "  (destructuring-bind (a b) '(1) (list a b)))"));
    ASSERT_STR_EQ("1", eval_str(
        "(locally (declare (optimize (safety 0)))"
        "  (destructuring-bind (a) '(1 2) a))"));
}

TEST(dbind_own_body_declare_elides_own_guards)
{
    /* Regression: a (declare (optimize (safety 0))) written in a
     * destructuring-bind's OWN body must elide THAT destructuring-bind's
     * own arity guards (CLHS 3.3.4 — the declaration's scope is the whole
     * body, guards included), not just an enclosing scope's declaration. */
    ASSERT_STR_EQ("(1 NIL)", eval_str(
        "(destructuring-bind (a b) '(1)"
        "  (declare (optimize (safety 0)))"
        "  (list a b))"));
    ASSERT_STR_EQ("1", eval_str(
        "(destructuring-bind (a) '(1 2)"
        "  (declare (optimize (safety 0)))"
        "  a)"));
    /* Scoping still ends with the body: a later, unrelated destructuring-bind
     * at default safety must still signal. */
    ASSERT_STR_EQ(":SIGNALED", eval_str(
        "(handler-case (eval '(destructuring-bind (a b) '(1) (list a b)))"
        "  (error () :signaled))"));
}

TEST(the_declaration_only_specs_skip_assert)
{
    /* Regression (found by jzon via chipi): compound FUNCTION and VALUES
     * type specifiers are declaration-only (CLHS 4.2.3) — TYPEP signals on
     * them, so THE at safety >= 1 must not emit a runtime assert for them.
     * Before this fix, (the (function ...) x) compiled at default safety
     * raised "TYPEP: invalid compound type specifier head: FUNCTION". */
    ASSERT_STR_EQ("42", eval_str(
        "(funcall (the (function () t) (lambda () 42)))"));
    ASSERT_STR_EQ("7", eval_str("(the (values t &optional) 7)"));
    /* ...including when nested inside OR / AND / NOT compounds. */
    ASSERT_STR_EQ("NIL", eval_str(
        "(the (or null (function () t)) nil)"));
    ASSERT_STR_EQ("5", eval_str(
        "(the (and t (function (t) t)) 5)"));
    /* Plain FUNCTION (non-compound) stays checkable and still signals. */
    ASSERT_STR_EQ(":SIGNALED", eval_str(
        "(handler-case (eval '(the function 3)) (error () :signaled))"));
    /* Ordinary compound specs still assert at default safety. */
    ASSERT_STR_EQ(":SIGNALED", eval_str(
        "(handler-case (eval '(the (or fixnum symbol) \"s\"))"
        "  (error () :signaled))"));
}

/* --- Folding through QUOTE and constants of other types --- */

TEST(fold_quote_and_t)
{
    ASSERT_STR_EQ("3", eval_str("(+ '1 '2)"));
    ASSERT_STR_EQ("NIL", eval_str("(not t)"));
    ASSERT_STR_EQ("NIL", eval_str("(not 'sym)"));
    ASSERT_STR_EQ(":THEN", eval_str("(if 'sym :then :else)"));
}

/* --- Compiler-macro expansion must run before constant folding ---
 *
 * NOTE: this permanently installs a compiler-macro on LOGAND for the rest
 * of the process, so it must run after every other test that folds LOGAND
 * calls (see fold_logical_values above) — kept last in RUN() order. */

TEST(compiler_macro_fires_despite_constant_args)
{
    /* CLHS 3.2.2.1: compiler-macro expansion is attempted before any other
     * compile-time transformation.  LOGAND is in the constant-folding
     * table (spec 1.3); a compiler macro defined on it must still see
     * every call site, even one where all arguments are compile-time
     * constants — folding must not intercept the call first. */
    eval_str("(defvar *cm-probe* nil)");
    eval_str(
        "(define-compiler-macro logand (&whole form &rest args)"
        "  (declare (ignore args))"
        "  (setq *cm-probe* t)"
        "  form)");  /* returns form unchanged: CLHS "eq to form" = decline */
    ASSERT_STR_EQ("8", eval_str("(logand 12 10)"));
    ASSERT_STR_EQ("T", eval_str("*cm-probe*"));
}

/* --- Cross-thread isolation of scoped (declare (optimize ...)) --- */

TEST(cross_thread_optimize_isolation)
{
    /* Regression: the effective (declare (optimize ...)) settings used to
     * live in a single process-wide global, mutated (unlocked) by every
     * body-scoped declaration's prelude/postlude.  Two threads compiling
     * concurrently could observe each other's scoped override — e.g. one
     * thread's (safety 0) window silently disabling another thread's THE
     * check.  Now each compile owns a private CL_Compiler chain, so no
     * amount of interleaving can leak an override across threads: thread A
     * spins in a (safety 0) LOCALLY (which must always elide the check)
     * while thread B repeatedly compiles a bare (the fixnum ...) at default
     * safety (which must always signal).  Any leak shows up as B failing to
     * signal at least once. */
    ASSERT_STR_EQ("0", eval_str(
        "(let ((ta (mp:make-thread"
        "            (lambda ()"
        "              (dotimes (i 3000)"
        "                (eval '(locally (declare (optimize (safety 0)))"
        "                         (the fixnum \"x\")))))))"
        "      (tb (mp:make-thread"
        "            (lambda ()"
        "              (let ((bad 0))"
        "                (dotimes (i 3000)"
        "                  (handler-case"
        "                      (progn (eval '(the fixnum \"y\")) (incf bad))"
        "                    (error () nil)))"
        "                bad)))))"
        "  (mp:join-thread ta)"
        "  (mp:join-thread tb))"));
}

int main(void)
{
    test_init();
    setup();

    RUN(fold_arith_values);
    RUN(fold_logical_values);
    RUN(fold_comparison_values);
    RUN(fold_not_null_values);
    RUN(fold_overflow_declines_to_runtime);
    RUN(fold_emits_single_constant);
    RUN(fold_disabled_at_speed_0);
    RUN(fold_respects_side_effects);
    RUN(fold_respects_shadowing_and_notinline);
    RUN(dead_branch_values);
    RUN(dead_branch_not_emitted);
    RUN(dead_branch_disabled_at_speed_0);
    RUN(dead_branch_keeps_tail_position);
    RUN(local_declare_scopes_to_body);
    RUN(defun_declare_does_not_leak);
    RUN(let_declare_scopes_to_body);
    RUN(declaim_is_global_and_persists);
    RUN(declaim_proclamations_survive_fasl_round_trip);
    RUN(nested_scopes_restore_in_order);
    RUN(dbind_guards_elided_at_safety_0);
    RUN(dbind_guards_signal_at_default_safety);
    RUN(dbind_own_body_declare_elides_own_guards);
    RUN(the_declaration_only_specs_skip_assert);
    RUN(fold_quote_and_t);

    /* Cross-thread isolation: no ordering constraint on other tests, but
     * kept early among the "must run last" pair below. */
    RUN(cross_thread_optimize_isolation);

    /* Must run last: permanently installs a compiler-macro on LOGAND. */
    RUN(compiler_macro_fires_despite_constant_args);

    teardown();
    REPORT();
}
