#include "builtins.h"
#include "symbol.h"
#include "package.h"
#include "mem.h"
#include "error.h"
#include "compiler.h"
#include "../platform/platform.h"
#include "../platform/platform_thread.h"
#include <string.h>

/* --- Mutation --- */

static CL_Obj bi_rplaca(CL_Obj *args, int n)
{
    CL_Cons *cell;
    CL_UNUSED(n);
    if (!CL_CONS_P(args[0]))
        cl_signal_type_error(args[0], "CONS", "RPLACA");
    cell = (CL_Cons *)CL_OBJ_TO_PTR(args[0]);
    cell->car = args[1];
    return args[0];  /* CL spec: returns the cons */
}

static CL_Obj bi_rplacd(CL_Obj *args, int n)
{
    CL_Cons *cell;
    CL_UNUSED(n);
    if (!CL_CONS_P(args[0]))
        cl_signal_type_error(args[0], "CONS", "RPLACD");
    cell = (CL_Cons *)CL_OBJ_TO_PTR(args[0]);
    cell->cdr = args[1];
    return args[0];  /* CL spec: returns the cons */
}

/* --- Symbol accessors --- */

static CL_Obj bi_symbol_value(CL_Obj *args, int n)
{
    CL_Obj val;
    CL_UNUSED(n);
    if (!CL_SYMBOL_OR_NIL_P(args[0]))
        cl_signal_type_error(args[0], "SYMBOL", "SYMBOL-VALUE");
    val = cl_symbol_value(args[0]);
    if (val == CL_UNBOUND)
        cl_signal_unbound_variable(args[0]);
    return val;
}

static CL_Obj bi_symbol_function(CL_Obj *args, int n)
{
    CL_Symbol *s;
    CL_Obj sym = args[0];
    CL_UNUSED(n);
    if (!CL_SYMBOL_OR_NIL_P(sym))
        cl_signal_type_error(sym, "SYMBOL", "SYMBOL-FUNCTION");
    /* CL_NIL is the constant tag (0) — its function/value/plist live on
     * the heap-allocated SYM_NIL storage shadow. */
    if (CL_NULL_P(sym)) sym = SYM_NIL;
    s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
    if (s->function == CL_UNBOUND)
        cl_signal_undefined_function(args[0]);
    return s->function;
}

/* FDEFINITION accepts a function name, which per the CLHS glossary is either a
 * symbol or a list (SETF symbol).  SYMBOL-FUNCTION only accepts a symbol, so
 * this is a separate entry point that additionally resolves the (SETF foo)
 * form to the symbol that holds the setter function. */
static CL_Obj bi_fdefinition(CL_Obj *args, int n)
{
    CL_Obj name = args[0];
    CL_UNUSED(n);
    if (CL_CONS_P(name)) {
        /* (setf accessor) — resolve to the setter symbol, then its function. */
        CL_Obj head = cl_car(name);
        CL_Obj rest = cl_cdr(name);
        if (CL_SYMBOL_P(head) &&
            strcmp(cl_symbol_name(head), "SETF") == 0 &&
            CL_CONS_P(rest) && CL_SYMBOL_P(cl_car(rest)) &&
            CL_NULL_P(cl_cdr(rest))) {
            CL_Obj setf_sym = cl_setf_function_symbol(cl_car(rest));
            CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(setf_sym);
            if (s->function == CL_UNBOUND)
                /* re-read: the %SETF-* intern above may have compacted,
                 * and the signaled condition would carry a stale name */
                cl_signal_undefined_function(args[0]);
            return s->function;
        }
        cl_signal_type_error(name, "FUNCTION-NAME", "FDEFINITION");
    }
    return bi_symbol_function(args, n);
}

/* --- Setf helpers --- */

static CL_Obj bi_setf_nth(CL_Obj *args, int n)
{
    /* (%setf-nth n list val) — walk list to nth cons, rplaca, return val */
    int32_t idx;
    CL_Obj list;
    CL_Cons *cell;
    CL_UNUSED(n);
    /* Same index rules as NTH: non-negative only — a negative index used to
     * walk zero conses and clobber the first element. */
    idx = cl_extract_count_or_clamp(args[0], "%SETF-NTH");
    list = args[1];
    while (idx > 0 && !CL_NULL_P(list)) {
        list = cl_cdr(list);
        idx--;
    }
    if (CL_NULL_P(list) || !CL_CONS_P(list))
        cl_error(CL_ERR_ARGS, "%SETF-NTH: index out of range");
    cell = (CL_Cons *)CL_OBJ_TO_PTR(list);
    cell->car = args[2];
    return args[2];
}

static CL_Obj bi_set_symbol_value(CL_Obj *args, int n)
{
    CL_Symbol *s;
    CL_UNUSED(n);
    if (!CL_SYMBOL_OR_NIL_P(args[0]))
        cl_signal_type_error(args[0], "SYMBOL", "SET");
    s = (CL_Symbol *)CL_OBJ_TO_PTR(args[0]);
    if (s->flags & CL_SYM_CONSTANT)
        cl_error(CL_ERR_GENERAL, "Cannot assign to constant variable: %s",
                 cl_symbol_name(args[0]));
    cl_set_symbol_value(args[0], args[1]);
    return args[1];
}

static CL_Obj bi_set_symbol_function(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    if (!CL_SYMBOL_OR_NIL_P(args[0]))
        cl_signal_type_error(args[0], "SYMBOL", "%SET-SYMBOL-FUNCTION");
    cl_symbol_set_function(args[0], args[1]);
    return args[1];
}

/* --- Boundp --- */

static CL_Obj bi_boundp(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    if (!CL_SYMBOL_OR_NIL_P(args[0]))
        cl_signal_type_error(args[0], "SYMBOL", "BOUNDP");
    return cl_symbol_boundp(args[0]) ? SYM_T : CL_NIL;
}

static CL_Obj bi_symbol_constant_p(CL_Obj *args, int n)
{
    CL_Symbol *s;
    CL_UNUSED(n);
    if (!CL_SYMBOL_P(args[0]))
        return CL_NIL;
    s = (CL_Symbol *)CL_OBJ_TO_PTR(args[0]);
    return (s->flags & CL_SYM_CONSTANT) ? SYM_T : CL_NIL;
}

/* %MARK-CONSTANT: mark a symbol as a constant variable at runtime.
   defconstant emits a call to this so that CONSTANTP works even when the
   form is loaded from a precompiled FASL (the compile-time flag set in
   compile_defconstant does not survive into a fresh load). */
static CL_Obj bi_mark_constant(CL_Obj *args, int n)
{
    CL_Symbol *s;
    CL_UNUSED(n);
    if (!CL_SYMBOL_P(args[0]))
        cl_signal_type_error(args[0], "SYMBOL", "%MARK-CONSTANT");
    s = (CL_Symbol *)CL_OBJ_TO_PTR(args[0]);
    s->flags |= CL_SYM_CONSTANT;
    return args[0];
}

/* %MARK-SPECIAL: proclaim a symbol special at runtime, WITHOUT binding it.
   (defvar name) with no init-form emits a call to this so the special
   proclamation works even when loaded from a precompiled FASL — the
   compile-time flag set in compile_defvar does not survive into a fresh load.
   Unlike OP_DEFVAR this never touches the symbol's value, so the variable
   stays unbound (CLHS defvar with no init form). */
static CL_Obj bi_mark_special(CL_Obj *args, int n)
{
    CL_Symbol *s;
    CL_UNUSED(n);
    if (!CL_SYMBOL_P(args[0]))
        cl_signal_type_error(args[0], "SYMBOL", "%MARK-SPECIAL");
    s = (CL_Symbol *)CL_OBJ_TO_PTR(args[0]);
    s->flags |= CL_SYM_SPECIAL;
    return args[0];
}

/* %GLOBALLY-BOUND-P: T when the symbol's GLOBAL value cell is bound,
   ignoring any dynamic binding on this thread.  DEFVAR emits a call to it
   to skip its initial-value form (CLHS: "evaluated only if name is not
   already bound"); it must ask the question OP_DEFVAR's store asks, or a
   DEFVAR inside a LET of the same variable would evaluate the form and
   then not store it -- or skip it and leave the global unbound. */
static CL_Obj bi_globally_boundp(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    if (!CL_SYMBOL_P(args[0]))
        cl_signal_type_error(args[0], "SYMBOL", "%GLOBALLY-BOUND-P");
    return ((CL_Symbol *)CL_OBJ_TO_PTR(args[0]))->value != CL_UNBOUND
           ? SYM_T : CL_NIL;
}

extern CL_Obj setf_fn_table;

/* Helper: if name is (setf sym), look up the setf-fn symbol in setf_fn_table.
   Returns CL_NIL if not found. */
static CL_Obj lookup_setf_fn_sym(CL_Obj name)
{
    if (CL_CONS_P(name) && cl_car(name) == SYM_SETF) {
        CL_Obj accessor = cl_car(cl_cdr(name));
        CL_Obj entry;
        if (!CL_SYMBOL_P(accessor))
            return CL_NIL;
        cl_tables_rdlock();
        entry = setf_fn_table;
        while (!CL_NULL_P(entry)) {
            CL_Obj pair = cl_car(entry);
            if (cl_car(pair) == accessor) {
                CL_Obj result = cl_cdr(pair);
                cl_tables_rwunlock();
                return result;
            }
            entry = cl_cdr(entry);
        }
        cl_tables_rwunlock();
        return CL_NIL;
    }
    return CL_NIL;
}

static CL_Obj bi_fboundp(CL_Obj *args, int n)
{
    extern int cl_macro_p(CL_Obj name);
    extern CL_Obj bi_special_operator_p(CL_Obj *args, int n);
    CL_Symbol *s;
    CL_UNUSED(n);
    if (CL_CONS_P(args[0])) {
        /* (fboundp '(setf name)) — only a function binding is meaningful here. */
        CL_Obj setf_sym = lookup_setf_fn_sym(args[0]);
        if (CL_NULL_P(setf_sym)) return CL_NIL;
        s = (CL_Symbol *)CL_OBJ_TO_PTR(setf_sym);
        return (s->function != CL_UNBOUND && !CL_NULL_P(s->function))
            ? SYM_T : CL_NIL;
    }
    if (!CL_SYMBOL_OR_NIL_P(args[0]))
        cl_signal_type_error(args[0], "SYMBOL", "FBOUNDP");
    {
        CL_Obj sym = CL_NULL_P(args[0]) ? SYM_NIL : args[0];
        s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
    }
    /* CLHS: fboundp is true for functions, macros, AND special operators. */
    if (s->function != CL_UNBOUND && !CL_NULL_P(s->function))
        return SYM_T;
    if (cl_macro_p(args[0]))
        return SYM_T;
    if (bi_special_operator_p(args, 1) == SYM_T)
        return SYM_T;
    return CL_NIL;
}

static CL_Obj bi_fmakunbound(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    if (CL_CONS_P(args[0])) {
        /* (fmakunbound '(setf name)) */
        CL_Obj setf_sym = lookup_setf_fn_sym(args[0]);
        if (!CL_NULL_P(setf_sym))
            cl_symbol_set_function(setf_sym, CL_UNBOUND);
        return args[0];
    }
    if (!CL_SYMBOL_OR_NIL_P(args[0]))
        cl_signal_type_error(args[0], "SYMBOL", "FMAKUNBOUND");
    cl_symbol_set_function(args[0], CL_UNBOUND);
    return args[0];
}

static CL_Obj bi_makunbound(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    if (!CL_SYMBOL_OR_NIL_P(args[0]))
        cl_signal_type_error(args[0], "SYMBOL", "MAKUNBOUND");
    cl_set_symbol_value(args[0], CL_UNBOUND);
    return args[0];
}

/* CLHS special-operator-p: true if SYMBOL names a CL special operator.
 * The set matches compile_expr() in compiler.c — any symbol dispatched
 * there as a special form is reported here.  Keep the two lists in sync
 * when adding or removing special-form compilation branches. */
CL_Obj bi_special_operator_p(CL_Obj *args, int n)
{
    CL_Obj sym;
    CL_UNUSED(n);
    if (!CL_SYMBOL_OR_NIL_P(args[0]))
        cl_signal_type_error(args[0], "SYMBOL", "SPECIAL-OPERATOR-P");
    sym = args[0];
    /* The 25 ANSI special operators (CLHS 3.1.2.1.2.1).  LAMBDA is NOT
     * one of them — it expands via a macro to FUNCTION (3.1.2.1.2.4) —
     * so it must not be reported here.  Any addition to this set must
     * be matched by the dispatch in compile_expr(). */
    if (sym == SYM_QUOTE || sym == SYM_IF || sym == SYM_PROGN
        || sym == SYM_LET || sym == SYM_LETSTAR
        || sym == SYM_SETQ || sym == SYM_FUNCTION
        || sym == SYM_BLOCK || sym == SYM_RETURN_FROM
        || sym == SYM_FLET || sym == SYM_LABELS
        || sym == SYM_TAGBODY || sym == SYM_GO
        || sym == SYM_CATCH || sym == SYM_THROW
        || sym == SYM_UNWIND_PROTECT
        || sym == SYM_MULTIPLE_VALUE_CALL || sym == SYM_MULTIPLE_VALUE_PROG1
        || sym == SYM_EVAL_WHEN || sym == SYM_LOAD_TIME_VALUE
        || sym == SYM_LOCALLY || sym == SYM_PROGV
        || sym == SYM_MACROLET || sym == SYM_SYMBOL_MACROLET
        || sym == SYM_THE)
        return SYM_T;
    return CL_NIL;
}

static CL_Obj bi_register_setf_function(CL_Obj *args, int n)
{
    /* (%register-setf-function accessor-sym setf-fn-sym) */
    CL_UNUSED(n);
    cl_register_setf_function(args[0], args[1]);
    return args[0];
}

/* (%setf-store-symbol accessor-sym) — the package-qualified hidden symbol
 * that stores accessor's (setf accessor) function.  CLOS :accessor writers
 * use this so they agree with the compiler's setf-place resolution. */
static CL_Obj bi_setf_store_symbol(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    if (!CL_SYMBOL_P(args[0]))
        cl_error(CL_ERR_TYPE, "%%SETF-STORE-SYMBOL: argument must be a symbol");
    return cl_setf_store_symbol(args[0]);
}

extern CL_Obj setf_expander_table;

/* (%get-defsetf-setter accessor-sym) — look up defsetf setter for accessor
 * (hash-indexed table, see cl_get_setf_updater) */
static CL_Obj bi_get_defsetf_setter(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    return cl_get_setf_updater(args[0]);
}

static CL_Obj bi_register_setf_expander(CL_Obj *args, int n)
{
    /* (%register-setf-expander accessor-sym expander-fn)
     * Cons outside the write lock (STW-vs-rwlock deadlock — see
     * cl_table_prepend_locked). */
    CL_UNUSED(n);
    cl_table_prepend_locked(&setf_expander_table, cl_cons(args[0], args[1]));
    return args[0];
}

/* --- Registration --- */

void cl_builtins_mutation_init(void)
{
    /* Mutation */
    defun("RPLACA", bi_rplaca, 2, 2);
    defun("RPLACD", bi_rplacd, 2, 2);

    /* Symbol accessors */
    defun("SYMBOL-VALUE", bi_symbol_value, 1, 1);
    defun("SYMBOL-FUNCTION", bi_symbol_function, 1, 1);
    defun("FDEFINITION", bi_fdefinition, 1, 1);

    /* Setf helpers */
    cl_register_builtin("%SETF-NTH", bi_setf_nth, 3, 3, cl_package_amilisp);
    cl_register_builtin("%SET-SYMBOL-VALUE", bi_set_symbol_value, 2, 2, cl_package_amilisp);
    cl_register_builtin("%SET-SYMBOL-FUNCTION", bi_set_symbol_function, 2, 2, cl_package_amilisp);
    defun("SET", bi_set_symbol_value, 2, 2);

    /* Boundp / Fboundp */
    defun("BOUNDP", bi_boundp, 1, 1);
    cl_register_builtin("%SYMBOL-CONSTANT-P", bi_symbol_constant_p, 1, 1, cl_package_amilisp);
    cl_register_builtin("%MARK-CONSTANT", bi_mark_constant, 1, 1, cl_package_amilisp);
    cl_register_builtin("%MARK-SPECIAL", bi_mark_special, 1, 1, cl_package_amilisp);
    cl_register_builtin("%GLOBALLY-BOUND-P", bi_globally_boundp, 1, 1, cl_package_amilisp);
    defun("FBOUNDP", bi_fboundp, 1, 1);
    defun("FMAKUNBOUND", bi_fmakunbound, 1, 1);
    defun("MAKUNBOUND", bi_makunbound, 1, 1);
    defun("SPECIAL-OPERATOR-P", bi_special_operator_p, 1, 1);
    cl_register_builtin("%REGISTER-SETF-FUNCTION", bi_register_setf_function, 2, 2, cl_package_amilisp);
    cl_register_builtin("%SETF-STORE-SYMBOL", bi_setf_store_symbol, 1, 1, cl_package_amilisp);
    cl_register_builtin("%REGISTER-SETF-EXPANDER", bi_register_setf_expander, 2, 2, cl_package_amilisp);
    cl_register_builtin("%GET-DEFSETF-SETTER", bi_get_defsetf_setter, 1, 1, cl_package_amilisp);
}
