/* test_type_expander.c — AMILISP::%TYPE-EXPANDER builtin.
 *
 * %TYPE-EXPANDER exposes the internal deftype expander table to Lisp so a
 * portable TYPEXPAND can be implemented on top of it (the AmiLisp
 * introspect-environment fork's #+cl-amiga typexpand).  serapeum's
 * EXPLODE-TYPE relies on TYPEXPAND to resolve a user `deftype' alias to its
 * underlying disjunction — before this builtin existed it returned the alias
 * unexpanded and the EXPLODE-TYPE test failed.
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

int main(void)
{
    test_init();
    setup();

    RUN(type_expander_builtin_is_nil);
    RUN(type_expander_non_symbol_is_nil);
    RUN(type_expander_atom_alias_expands);
    RUN(type_expander_parameterized_alias_expands);
    RUN(type_expander_drives_portable_typexpand);

    teardown();
    REPORT();
}
