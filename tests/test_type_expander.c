/* test_type_expander.c — AMILISP::%TYPE-EXPANDER builtin and the
 * AMILISP:TYPEXPAND-1 / AMILISP:TYPEXPAND functions built on it (lib/boot.lisp).
 *
 * %TYPE-EXPANDER exposes the internal deftype expander table to Lisp;
 * TYPEXPAND-1 / TYPEXPAND are the implementation's own type expanders in the
 * shape of sb-ext:typexpand, which the introspect-environment library's
 * #+amilisp backend delegates to with a one-liner.  serapeum's EXPLODE-TYPE
 * relies on TYPEXPAND to resolve a user `deftype' alias to its underlying
 * disjunction — before this existed it returned the alias unexpanded and the
 * EXPLODE-TYPE test failed.
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

/* A built-in / primitive type name has no deftype expander → NIL. */
TEST(type_expander_builtin_is_nil)
{
    ASSERT_STR_EQ(eval_print("(amilisp::%type-expander 'list)"), "NIL");
    ASSERT_STR_EQ(eval_print("(amilisp::%type-expander 'integer)"), "NIL");
}

/* A non-symbol argument never names a deftype → NIL (not an error). */
TEST(type_expander_non_symbol_is_nil)
{
    ASSERT_STR_EQ(eval_print("(amilisp::%type-expander 42)"), "NIL");
    ASSERT_STR_EQ(eval_print("(amilisp::%type-expander '(a b))"), "NIL");
}

/* An atom deftype's expander takes zero arguments and yields its body. */
TEST(type_expander_atom_alias_expands)
{
    ASSERT_STR_EQ(eval_print(
        "(progn (deftype my-disj () '(or x (or y z) (member :x :y)))"
        "       (funcall (amilisp::%type-expander 'my-disj)))"),
        "(OR X (OR Y Z) (MEMBER :X :Y))");
}

/* A parameterized deftype's expander is applied to the compound type args. */
TEST(type_expander_parameterized_alias_expands)
{
    ASSERT_STR_EQ(eval_print(
        "(progn (deftype my-int (lo hi) `(integer ,lo ,hi))"
        "       (funcall (amilisp::%type-expander 'my-int) 0 9))"),
        "(INTEGER 0 9)");
}

/* End-to-end: a TYPEXPAND built on %TYPE-EXPANDER (mirroring the
 * introspect-environment shim) fully resolves a user deftype alias while
 * leaving built-in type names untouched. */
TEST(type_expander_drives_portable_typexpand)
{
    const char *defs =
        "(progn"
        "(defun tx1 (type)"
        "  (multiple-value-bind (head args)"
        "      (if (consp type) (values (car type) (cdr type)) (values type nil))"
        "    (if (symbolp head)"
        "        (let ((ex (amilisp::%type-expander head)))"
        "          (if ex (values (apply ex args) t) (values type nil)))"
        "        (values type nil))))"
        "(defun tx (type)"
        "  (loop (multiple-value-bind (new exp) (tx1 type)"
        "          (if exp (setf type new) (return type))))))";
    (void)eval_print(defs);
    (void)eval_print("(deftype color () '(member :red :green :blue))");
    /* a deftype alias is resolved to its body */
    ASSERT_STR_EQ(eval_print("(tx 'color)"), "(MEMBER :RED :GREEN :BLUE)");
    /* a built-in type name is left unchanged */
    ASSERT_STR_EQ(eval_print("(tx 'string)"), "STRING");
    /* a non-alias compound type is left unchanged */
    ASSERT_STR_EQ(eval_print("(tx '(integer 0 9))"), "(INTEGER 0 9)");
}

/* ---- AMILISP:TYPEXPAND-1 / AMILISP:TYPEXPAND (the native entry points) ---- */

/* An atom alias expands one step; the second value says so. */
TEST(typexpand_1_atom_alias)
{
    (void)eval_print("(deftype tx1-color () '(member :red :green :blue))");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (amilisp:typexpand-1 'tx1-color))"),
                  "((MEMBER :RED :GREEN :BLUE) T)");
}

/* A parameterized alias is applied to the compound specifier's arguments. */
TEST(typexpand_1_parameterized_alias)
{
    (void)eval_print("(deftype tx1-range (lo hi) `(integer ,lo ,hi))");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (amilisp:typexpand-1 '(tx1-range 0 9)))"),
                  "((INTEGER 0 9) T)");
}

/* CLHS deftype: "If the lambda-list has &optional or &key parameters that
 * have no default supplied, the default is *" -- not NIL as in an ordinary
 * lambda.  Bare VAR, (VAR) and ((:KEY VAR)) specs all get the * default; a
 * written default (even NIL) is kept; &aux keeps its NIL default; and the
 * expansion is the same whether the deftype is typed in or loaded from a
 * FASL (the rewrite happens at compile time). */
TEST(deftype_unsupplied_optional_and_key_default_to_star)
{
    (void)eval_print("(deftype tx-opt (&optional lo) `(integer ,lo))");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (amilisp:typexpand-1 '(tx-opt)))"),
                  "((INTEGER *) T)");
    ASSERT_STR_EQ(eval_print("(amilisp:typexpand-1 'tx-opt)"), "(INTEGER *)");
    ASSERT_STR_EQ(eval_print("(amilisp:typexpand-1 '(tx-opt 3))"), "(INTEGER 3)");
    /* (VAR) spec and two optionals */
    (void)eval_print("(deftype tx-opt2 (&optional (lo) hi) `(integer ,lo ,hi))");
    ASSERT_STR_EQ(eval_print("(amilisp:typexpand '(tx-opt2))"), "(INTEGER * *)");
    ASSERT_STR_EQ(eval_print("(amilisp:typexpand '(tx-opt2 1))"), "(INTEGER 1 *)");
    /* &key: bare and ((:key var)) spec; a written default is kept */
    (void)eval_print("(deftype tx-key (&key lo ((:top hi)) (mid nil))"
                     "  `(list ,lo ,hi ,mid))");
    ASSERT_STR_EQ(eval_print("(amilisp:typexpand '(tx-key))"), "(LIST * * NIL)");
    ASSERT_STR_EQ(eval_print("(amilisp:typexpand '(tx-key :top 7))"), "(LIST * 7 NIL)");
    /* &rest and &aux are untouched: rest is the (empty) list, aux is NIL */
    (void)eval_print("(deftype tx-rest (&rest r &aux a) `(list ,r ,a))");
    ASSERT_STR_EQ(eval_print("(amilisp:typexpand '(tx-rest))"), "(LIST NIL NIL)");
    /* typep sees the rewritten default too: (INTEGER *) is every integer */
    ASSERT_STR_EQ(eval_print("(list (typep -5 '(tx-opt)) (typep 5 '(tx-opt 3))"
                             "      (typep 1 '(tx-opt 3)))"),
                  "(T T NIL)");
    /* a required parameter and a supplied-p variable are left alone */
    (void)eval_print("(deftype tx-sup (a &optional (b 'x b-p)) `(list ,a ,b ,b-p))");
    ASSERT_STR_EQ(eval_print("(amilisp:typexpand '(tx-sup 1))"), "(LIST 1 X NIL)");
}

/* TYPEXPAND-1 takes exactly one step: an alias of an alias yields the
 * intermediate alias, not the final type. */
TEST(typexpand_1_single_step)
{
    (void)eval_print("(deftype tx1-base () '(or string number))");
    (void)eval_print("(deftype tx1-mid () 'tx1-base)");
    (void)eval_print("(deftype tx1-top () 'tx1-mid)");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (amilisp:typexpand-1 'tx1-top))"),
                  "(TX1-MID T)");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (amilisp:typexpand 'tx1-top))"),
                  "((OR STRING NUMBER) T)");
}

/* Nothing to expand: standard type names, compound standard types, and a
 * compound whose head is not a symbol come back EQ with NIL. */
TEST(typexpand_unchanged_returns_nil)
{
    ASSERT_STR_EQ(eval_print("(multiple-value-list (amilisp:typexpand-1 'string))"),
                  "(STRING NIL)");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (amilisp:typexpand 'string))"),
                  "(STRING NIL)");
    ASSERT_STR_EQ(eval_print("(let ((ty '(integer 0 9)))"
                             "  (multiple-value-bind (new exp) (amilisp:typexpand ty)"
                             "    (list (eq new ty) exp)))"),
                  "(T NIL)");
    ASSERT_STR_EQ(eval_print("(multiple-value-list (amilisp:typexpand '((a) b)))"),
                  "(((A) B) NIL)");
}

/* A class object is a type specifier (CLHS 4.2.3) and is never expanded. */
TEST(typexpand_class_object_unchanged)
{
    ASSERT_STR_EQ(eval_print("(multiple-value-bind (new exp)"
                             "    (amilisp:typexpand (find-class 'integer))"
                             "  (list (eq new (find-class 'integer)) exp))"),
                  "(T NIL)");
}

/* The standard signature takes an optional environment, which is ignored. */
TEST(typexpand_accepts_env)
{
    (void)eval_print("(deftype tx-env-alias () 'fixnum)");
    ASSERT_STR_EQ(eval_print("(amilisp:typexpand 'tx-env-alias nil)"), "FIXNUM");
    ASSERT_STR_EQ(eval_print("(amilisp:typexpand-1 'tx-env-alias nil)"), "FIXNUM");
    /* a real &environment object, passed as a quoted constant */
    ASSERT_STR_EQ(eval_print("(macrolet ((m (&environment e)"
                             "             `(amilisp:typexpand 'tx-env-alias ',e)))"
                             "  (m))"),
                  "FIXNUM");
}

/* Anything that is not a type specifier is a TYPE-ERROR, not a silent NIL. */
TEST(typexpand_non_type_specifier_errors)
{
    ASSERT_STR_EQ(eval_print("(handler-case (amilisp:typexpand 42)"
                             "  (type-error (e) (type-error-datum e)))"),
                  "42");
    ASSERT_STR_EQ(eval_print("(handler-case (amilisp:typexpand-1 \"string\")"
                             "  (type-error (e) (type-error-datum e)))"),
                  "\"string\"");
}

/* Redefining a deftype takes effect immediately: the expander table holds
 * the latest expander. */
TEST(typexpand_sees_redefinition)
{
    (void)eval_print("(deftype tx-redef () 'string)");
    ASSERT_STR_EQ(eval_print("(amilisp:typexpand 'tx-redef)"), "STRING");
    (void)eval_print("(deftype tx-redef () 'integer)");
    ASSERT_STR_EQ(eval_print("(amilisp:typexpand 'tx-redef)"), "INTEGER");
}

/* The names are exported from AMILISP (and so visible from CL-USER), as the
 * library's `amilisp:typexpand' reference requires. */
TEST(typexpand_exported_from_amilisp)
{
    /* (CL-USER inherits AMILISP, so the printer shows no prefix: check the
     * home package by name.) */
    ASSERT_STR_EQ(eval_print("(multiple-value-bind (sym status)"
                             "    (find-symbol \"TYPEXPAND\" \"AMILISP\")"
                             "  (list (package-name (symbol-package sym)) status))"),
                  "(\"AMILISP\" :EXTERNAL)");
    ASSERT_STR_EQ(eval_print("(nth-value 1 (find-symbol \"TYPEXPAND-1\" \"AMILISP\"))"),
                  ":EXTERNAL");
}

int main(void)
{
    test_init();
    setup();

    RUN(type_expander_builtin_is_nil);
    RUN(type_expander_non_symbol_is_nil);
    RUN(type_expander_atom_alias_expands);
    RUN(type_expander_parameterized_alias_expands);
    RUN(type_expander_drives_portable_typexpand);
    RUN(typexpand_1_atom_alias);
    RUN(typexpand_1_parameterized_alias);
    RUN(deftype_unsupplied_optional_and_key_default_to_star);
    RUN(typexpand_1_single_step);
    RUN(typexpand_unchanged_returns_nil);
    RUN(typexpand_class_object_unchanged);
    RUN(typexpand_accepts_env);
    RUN(typexpand_non_type_specifier_errors);
    RUN(typexpand_sees_redefinition);
    RUN(typexpand_exported_from_amilisp);

    teardown();
    REPORT();
}
