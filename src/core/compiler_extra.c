/*
 * compiler_extra.c — Conditionals, case, quasiquote, MV, defs
 *
 * Split from compiler.c. Contains: and, or, cond, quasiquote,
 * case, typecase, multiple-value-*, eval-when, defsetf,
 * defvar, defparameter, defun, defmacro.
 */

#include "compiler_internal.h"
#include "../platform/platform_thread.h"
#include <stdio.h>

/* Forward-jump fixups in AND/OR/COND/CASE/TYPECASE use pending-jump
 * chains threaded through the operand placeholders (cl_emit_jump_chain /
 * cl_patch_jump_chain) — one int of state per nesting level.  These
 * compiles recurse once per level of nested conditionals (macro towers
 * expand into deep COND/CASE chains), so per-frame patch arrays would
 * multiply into tens of KB on small fixed Amiga shell stacks. */

/* --- And / Or --- */

void compile_and(CL_Compiler *c, CL_Obj form)
{
    CL_Obj args = cl_cdr(form);
    int saved_tail = c->in_tail;
    int nil_chain = CL_JUMP_CHAIN_END;

    if (CL_NULL_P(args)) {
        /* (and) => T */
        cl_emit(c, OP_T);
        return;
    }

    if (CL_NULL_P(cl_cdr(args))) {
        /* (and x) => just compile x */
        compile_expr(c, cl_car(args));
        return;
    }

    /* Multiple args: short-circuit chain */
    CL_GC_PROTECT(args);
    while (!CL_NULL_P(args)) {
        int is_last = CL_NULL_P(cl_cdr(args));
        if (is_last) {
            c->in_tail = saved_tail;
            compile_expr(c, cl_car(args));
        } else {
            c->in_tail = 0;
            compile_expr(c, cl_car(args));
            /* A non-last AND arm contributes exactly one value: the NIL it
             * short-circuits with (CLHS 5.1).  Reset BEFORE the OP_DUP so the
             * short-circuit exit carries a clean MV state — resetting after
             * the jump (as this did) only cleaned the fall-through path, and
             * (multiple-value-list (and (values nil 2) 3)) saw the leak. */
            cl_emit(c, OP_MV_RESET);
            c->mv_state = CL_MV_ONE;
            cl_emit(c, OP_DUP);
            nil_chain = cl_emit_jump_chain(c, OP_JNIL, nil_chain);
            cl_emit(c, OP_POP);
        }
        args = cl_cdr(args);
    }
    CL_GC_UNPROTECT(1);

    /* Patch all nil-jumps to here (done label) */
    cl_patch_jump_chain(c, nil_chain);
}

void compile_or(CL_Compiler *c, CL_Obj form)
{
    CL_Obj args = cl_cdr(form);
    int saved_tail = c->in_tail;
    int true_chain = CL_JUMP_CHAIN_END;

    if (CL_NULL_P(args)) {
        /* (or) => NIL */
        cl_emit(c, OP_NIL);
        return;
    }

    if (CL_NULL_P(cl_cdr(args))) {
        /* (or x) => just compile x */
        compile_expr(c, cl_car(args));
        return;
    }

    /* Multiple args: short-circuit chain */
    CL_GC_PROTECT(args);
    while (!CL_NULL_P(args)) {
        int is_last = CL_NULL_P(cl_cdr(args));
        if (is_last) {
            c->in_tail = saved_tail;
            compile_expr(c, cl_car(args));
        } else {
            c->in_tail = 0;
            compile_expr(c, cl_car(args));
            /* "If the first form yields non-nil, OR returns that value [only]"
             * — CLHS 5.1.  Reset before the OP_DUP so the short-circuit exit
             * carries one value: (multiple-value-list (or (values 1 2) 3))
             * used to be (1 2). */
            cl_emit(c, OP_MV_RESET);
            c->mv_state = CL_MV_ONE;
            cl_emit(c, OP_DUP);
            true_chain = cl_emit_jump_chain(c, OP_JTRUE, true_chain);
            cl_emit(c, OP_POP);
        }
        args = cl_cdr(args);
    }
    CL_GC_UNPROTECT(1);

    /* Patch all true-jumps to here (done label) */
    cl_patch_jump_chain(c, true_chain);
}

/* --- Cond --- */

void compile_cond(CL_Compiler *c, CL_Obj form)
{
    CL_Obj clauses = cl_cdr(form);
    int saved_tail = c->in_tail;
    /* Two exit chains so a stale arm can be given its own OP_MV_RESET when
     * the arms disagree — see cl_mv_close_join.  join_mv is their merge
     * (CL_MV_ONE = identity), any_stale records that one of them needs it. */
    int done_chain = CL_JUMP_CHAIN_END;
    int stale_chain = CL_JUMP_CHAIN_END;
    int join_mv = CL_MV_ONE;
    int any_stale = 0;

    if (CL_NULL_P(clauses)) {
        cl_emit(c, OP_NIL);
        return;
    }

    CL_GC_PROTECT(form);
    CL_GC_PROTECT(clauses);
    while (!CL_NULL_P(clauses)) {
        CL_Obj clause = cl_car(clauses);
        CL_Obj test = cl_car(clause);
        CL_Obj body = cl_cdr(clause);
        int is_last = CL_NULL_P(cl_cdr(clauses));

        if (test == SYM_T && is_last) {
            /* (t body...) — default clause, no test needed */
            c->in_tail = saved_tail;
            if (CL_NULL_P(body))
                cl_emit(c, OP_T);
            else
                compile_progn(c, body);
            join_mv = cl_mv_merge(join_mv, c->mv_state);
            if (c->mv_state == CL_MV_STALE) any_stale = 1;
        } else {
            int jnil_pos;
            /* Compile test */
            c->in_tail = 0;
            compile_expr(c, test);

            /* GC SAFETY: re-derive body from the protected cursor AFTER
             * compile_expr(test) — it allocates and can compact, leaving the
             * pre-read body a stale offset (same shape as compile_if). */
            clause = cl_car(clauses);
            body = cl_cdr(clause);

            if (CL_NULL_P(body)) {
                /* (cond (test)) with no body — return the test value itself.
                 * DUP before JNIL so value stays on stack if test is true.
                 * CLHS 5.1: only the test's PRIMARY value is returned, so
                 * clear the MV state before the value is duplicated out. */
                cl_emit(c, OP_MV_RESET);
                c->mv_state = CL_MV_ONE;
                cl_emit(c, OP_DUP);
                jnil_pos = cl_emit_jump(c, OP_JNIL);
            } else {
                jnil_pos = cl_emit_jump(c, OP_JNIL);

                /* Compile body */
                c->in_tail = saved_tail;
                compile_progn(c, body);
            }
            join_mv = cl_mv_merge(join_mv, c->mv_state);

            /* Always JMP past NIL fallthrough (even last non-t clause) —
             * onto the stale chain when this arm needs normalizing. */
            if (c->mv_state == CL_MV_STALE) {
                any_stale = 1;
                stale_chain = cl_emit_jump_chain(c, OP_JMP, stale_chain);
            } else {
                done_chain = cl_emit_jump_chain(c, OP_JMP, done_chain);
            }

            cl_patch_jump(c, jnil_pos);
            if (CL_NULL_P(body)) {
                /* JNIL popped the DUP'd value (was NIL), pop the original too */
                cl_emit(c, OP_POP);
            }
        }

        clauses = cl_cdr(clauses);
    }
    CL_GC_UNPROTECT(2);  /* clauses, form */

    /* If the last clause wasn't a t-default, we need a NIL fallthrough.
     * form was protected across the compiles above, so this re-walk reads
     * the forwarded list. */
    {
        CL_Obj last_clause;
        CL_Obj c2 = cl_cdr(form);
        /* Walk to last clause */
        while (!CL_NULL_P(cl_cdr(c2))) c2 = cl_cdr(c2);
        last_clause = cl_car(c2);
        if (cl_car(last_clause) != SYM_T)
            cl_emit(c, OP_NIL);   /* an arm of its own — exactly one value */
    }

    /* Patch all exit jumps, normalizing the stale arms if they disagree
     * with a multi-valued one. */
    cl_mv_close_join(c, done_chain, stale_chain, any_stale, join_mv);
}

/* --- Quasiquote --- */

/* Expand a quasiquote template into LIST/CONS/APPEND forms.
 * Uses depth tracking for correct nested backquote handling.
 *
 * At depth 0: UNQUOTE evaluates, UNQUOTE-SPLICING splices.
 * At depth > 0: all QQ markers are reconstructed as data.
 * QUASIQUOTE increments depth, UNQUOTE/UNQUOTE-SPLICING decrement depth.
 *
 * GC safety: every helper that calls cl_intern_in (an allocating call)
 * protects all CL_Obj locals it needs after the call.  Nested cl_cons
 * patterns are split into statements so no outer CL_Obj arg is read into
 * an unspecified-order temporary before an inner allocating call can run
 * a compacting GC and relocate it.
 */

static CL_Obj qq_expand(CL_Obj x, int depth);
static CL_Obj qq_expand_list(CL_Obj x, int depth);

/* Build (QUOTE x) — protect both locals: cl_cons can trigger compaction,
 * staling x (CAR) or tail (CDR of the outer cons) if unprotected. */
static CL_Obj qq_quote(CL_Obj x)
{
    CL_Obj tail = CL_NIL;
    CL_GC_PROTECT(x);
    CL_GC_PROTECT(tail);
    tail = cl_cons(x, CL_NIL);
    {
        CL_Obj result = cl_cons(SYM_QUOTE, tail);
        CL_GC_UNPROTECT(2);
        return result;
    }
}

/* Max args we put in a single generated APPEND/LIST call.  A quasiquote
 * template with N top-level elements expands to one APPEND with N segment
 * args; OP_CALL/OP_TAILCALL encode the arg count in a single byte, so a
 * call with > 255 args silently truncates the count and corrupts the VM
 * stack.  ironclad's threefish.lisp has setf templates of ~289 elements
 * (the 1024-bit ARX round macro), which is exactly this case.  Chunk the
 * APPEND into nested groups so no single call exceeds this bound; APPEND is
 * associative and every segment is freshly consed, so nesting is
 * value-preserving.  Kept comfortably below 255 for headroom. */
#define CL_QQ_MAX_CALL_ARGS 128

static int qq_proper_length(CL_Obj l)
{
    int n = 0;
    while (CL_CONS_P(l)) { n++; l = cl_cdr(l); }
    return n;
}

/* Simplify (APPEND ...) forms, chunking when the arg list is too long to
 * encode in a single OP_CALL byte (see CL_QQ_MAX_CALL_ARGS). */
static CL_Obj qq_append(CL_Obj args)
{
    CL_Obj sym_append;
    /* Protect args: cl_intern_in allocates and may compact the heap. */
    CL_GC_PROTECT(args);
    sym_append = cl_intern_in("APPEND", 6, cl_package_cl);
    CL_GC_PROTECT(sym_append);
    if (CL_NULL_P(args)) {
        CL_GC_UNPROTECT(2);
        return CL_NIL;
    }
    if (CL_NULL_P(cl_cdr(args))) {
        CL_Obj val = cl_car(args);
        CL_GC_UNPROTECT(2);
        return val;
    }
    if (qq_proper_length(args) <= CL_QQ_MAX_CALL_ARGS) {
        CL_Obj val = cl_cons(sym_append, args);
        CL_GC_UNPROTECT(2);
        return val;
    }
    /* Too many segments: partition into groups of CL_QQ_MAX_CALL_ARGS, wrap
     * each group in its own (APPEND group...) and recurse on the list of
     * group forms (so the outer call also stays within the bound). */
    {
        CL_Obj groups = CL_NIL, groups_tail = CL_NIL;
        CL_Obj cur = args;
        CL_Obj val;
        CL_GC_PROTECT(groups);
        CL_GC_PROTECT(groups_tail);
        CL_GC_PROTECT(cur);
        while (!CL_NULL_P(cur)) {
            CL_Obj group = CL_NIL, group_tail = CL_NIL;
            int k = 0;
            CL_GC_PROTECT(group);
            CL_GC_PROTECT(group_tail);
            while (!CL_NULL_P(cur) && k < CL_QQ_MAX_CALL_ARGS) {
                CL_Obj cell = cl_cons(cl_car(cur), CL_NIL);
                if (CL_NULL_P(group)) group = cell;
                else ((CL_Cons *)CL_OBJ_TO_PTR(group_tail))->cdr = cell;
                group_tail = cell;
                cur = cl_cdr(cur);
                k++;
            }
            {
                CL_Obj group_form = cl_cons(sym_append, group);
                CL_Obj gcell;
                CL_GC_PROTECT(group_form);
                gcell = cl_cons(group_form, CL_NIL);
                if (CL_NULL_P(groups)) groups = gcell;
                else ((CL_Cons *)CL_OBJ_TO_PTR(groups_tail))->cdr = gcell;
                groups_tail = gcell;
                CL_GC_UNPROTECT(1); /* group_form */
            }
            CL_GC_UNPROTECT(2); /* group, group_tail */
        }
        val = qq_append(groups);
        CL_GC_UNPROTECT(5); /* groups, groups_tail, cur, args, sym_append */
        return val;
    }
}

/* Build (LIST a b c ...) */
static CL_Obj qq_list(CL_Obj items)
{
    CL_Obj sym_list;
    /* Protect items: cl_intern_in allocates and may compact the heap. */
    CL_GC_PROTECT(items);
    sym_list = cl_intern_in("LIST", 4, cl_package_cl);
    {
        CL_Obj val = cl_cons(sym_list, items);
        CL_GC_UNPROTECT(1);
        return val;
    }
}

/* Expand quasiquote template at the given nesting depth.
 * Returns a form that, when evaluated, produces the desired structure. */
static CL_Obj qq_expand(CL_Obj x, int depth)
{
    /* Atom: quote it (symbols need quoting, self-evaluating atoms don't) */
    if (!CL_CONS_P(x)) {
        if (CL_NULL_P(x))
            return CL_NIL;
        if (CL_SYMBOL_P(x))
            return qq_quote(x);
        /* Vector template: convert elements to a list, expand quasiquote
         * on the list, then wrap so it builds a vector at runtime.
         * Form returned: (COERCE <list-form> 'SIMPLE-VECTOR).  Splices
         * inside the vector work for free since the list expansion
         * already handles them. */
        if (CL_VECTOR_P(x)) {
            CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(x);
            uint32_t i, n = cl_vector_active_length(v);
            CL_Obj elems = CL_NIL, list_form = CL_NIL;
            CL_Obj sym_coerce = CL_NIL, tail = CL_NIL;
            CL_GC_PROTECT(elems);
            CL_GC_PROTECT(list_form);
            CL_GC_PROTECT(sym_coerce);
            CL_GC_PROTECT(tail);
            CL_GC_PROTECT(x);
            for (i = n; i > 0; i--)
                elems = cl_cons(cl_vector_data((CL_Vector *)CL_OBJ_TO_PTR(x))[i - 1], elems);
            list_form = qq_expand(elems, depth);
            sym_coerce = cl_intern_in("COERCE", 6, cl_package_cl);
            {
                /* sym_sv and quoted_sv must both be protected: cl_intern_in
                 * and qq_quote both allocate, and cl_cons(quoted_sv, ...) can
                 * compact the heap, staling an unprotected C local. */
                CL_Obj sym_sv = CL_NIL;
                CL_Obj quoted_sv = CL_NIL;
                CL_GC_PROTECT(sym_sv);
                CL_GC_PROTECT(quoted_sv);
                sym_sv = cl_intern_in("SIMPLE-VECTOR", 13, cl_package_cl);
                quoted_sv = qq_quote(sym_sv);
                tail = cl_cons(quoted_sv, CL_NIL);
                CL_GC_UNPROTECT(2);
            }
            tail = cl_cons(list_form, tail);
            {
                CL_Obj val = cl_cons(sym_coerce, tail);
                CL_GC_UNPROTECT(5);
                return val;
            }
        }
        /* Self-evaluating: fixnum, char, string, etc. */
        return x;
    }

    {
        CL_Obj head = cl_car(x);

        /* (UNQUOTE expr) */
        if (head == SYM_UNQUOTE) {
            if (depth == 0)
                return cl_car(cl_cdr(x));  /* Evaluate expr */
            /* depth > 0: reconstruct as (LIST 'UNQUOTE (qq-expand expr depth-1)) */
            {
                CL_Obj inner = qq_expand(cl_car(cl_cdr(x)), depth - 1);
                CL_Obj tail;
                CL_GC_PROTECT(inner);
                CL_GC_PROTECT(tail);
                tail = cl_cons(inner, CL_NIL);
                tail = cl_cons(qq_quote(SYM_UNQUOTE), tail);
                CL_GC_UNPROTECT(2);
                return qq_list(tail);
            }
        }

        /* (UNQUOTE-SPLICING expr) */
        if (head == SYM_UNQUOTE_SPLICING) {
            if (depth == 0) {
                /* At top level, this means "evaluate expr for splicing".
                 * Return the expr — caller is responsible for splicing context. */
                return cl_car(cl_cdr(x));
            }
            /* depth > 0: reconstruct */
            {
                CL_Obj inner = qq_expand(cl_car(cl_cdr(x)), depth - 1);
                CL_Obj tail;
                CL_GC_PROTECT(inner);
                CL_GC_PROTECT(tail);
                tail = cl_cons(inner, CL_NIL);
                tail = cl_cons(qq_quote(SYM_UNQUOTE_SPLICING), tail);
                CL_GC_UNPROTECT(2);
                return qq_list(tail);
            }
        }

        /* (QUASIQUOTE expr) — nested backquote */
        if (head == SYM_QUASIQUOTE) {
            CL_Obj inner = qq_expand(cl_car(cl_cdr(x)), depth + 1);
            CL_Obj tail;
            CL_GC_PROTECT(inner);
            CL_GC_PROTECT(tail);
            tail = cl_cons(inner, CL_NIL);
            tail = cl_cons(qq_quote(SYM_QUASIQUOTE), tail);
            CL_GC_UNPROTECT(2);
            return qq_list(tail);
        }
    }

    /* Regular list: expand each element, combine with APPEND */
    {
        CL_Obj result = CL_NIL;
        CL_Obj cursor = x;

        CL_GC_PROTECT(result);
        /* Protect cursor so its value is updated if the GC compacts during
         * qq_expand_list or cl_cons inside the loop body. */
        CL_GC_PROTECT(cursor);

        while (CL_CONS_P(cursor)) {
            CL_Obj elem = cl_car(cursor);

            /* Detect dotted unquote: (... UNQUOTE expr) where UNQUOTE is bare symbol
             * This comes from reader: `(a b . ,x) → (a b UNQUOTE x)
             * Add tail_expr as the last APPEND segment and break out —
             * the normal APPEND path handles splices correctly.
             * Re-derive rest from the protected cursor each iteration. */
            {
                CL_Obj rest = cl_cdr(cursor);
                if (depth == 0 && elem == SYM_UNQUOTE &&
                    CL_CONS_P(rest) && CL_NULL_P(cl_cdr(rest))) {
                    CL_Obj tail_expr = cl_car(rest);
                    result = cl_cons(tail_expr, result);
                    cursor = CL_NIL;  /* tail already handled */
                    break;
                }
            }

            {
                /* expanded is a fresh heap value; protect it so cl_cons
                 * can compact without staling the unprotected C local. */
                CL_Obj expanded = qq_expand_list(elem, depth);
                CL_GC_PROTECT(expanded);
                result = cl_cons(expanded, result);
                CL_GC_UNPROTECT(1);
            }
            /* Advance from the GC-updated cursor, not from a pre-saved rest. */
            cursor = cl_cdr(cursor);
        }

        /* Handle dotted tail (non-unquote atom as CDR) — shouldn't normally happen
         * since reader produces proper lists, but handle for safety */
        if (!CL_NULL_P(cursor)) {
            CL_Obj tail_exp = qq_expand(cursor, depth);
            result = cl_cons(tail_exp, result);
        }

        /* Reverse */
        {
            CL_Obj rev = CL_NIL;
            CL_GC_PROTECT(rev);
            while (!CL_NULL_P(result)) {
                rev = cl_cons(cl_car(result), rev);
                result = cl_cdr(result);
            }
            result = rev;
            CL_GC_UNPROTECT(1);
        }

        {
            CL_Obj val = qq_append(result);
            CL_GC_UNPROTECT(2);  /* result, cursor */
            return val;
        }
    }
}

/* Expand a single list element for use in APPEND.
 * Returns a form that evaluates to a LIST (for non-splicing elements)
 * or directly to a list value (for splicing elements).
 *
 * GC safety: x and sym_list are both protected across all allocating calls.
 * Every return path unprotects exactly the number of roots pushed above it. */
static CL_Obj qq_expand_list(CL_Obj x, int depth)
{
    CL_Obj sym_list;
    /* Protect x before the first allocating call (cl_intern_in). */
    CL_GC_PROTECT(x);
    sym_list = cl_intern_in("LIST", 4, cl_package_cl);
    /* Protect sym_list across all subsequent allocating calls. */
    CL_GC_PROTECT(sym_list);
    /* From here on: protect depth = 2 (x, sym_list).
     * Each return path must CL_GC_UNPROTECT by its own total depth. */

    /* Atom: (LIST 'x) */
    if (!CL_CONS_P(x)) {
        if (CL_NULL_P(x)) {
            CL_Obj tail = cl_cons(CL_NIL, CL_NIL);
            CL_Obj val  = cl_cons(sym_list, tail);
            CL_GC_UNPROTECT(2);
            return val;
        }
        if (CL_SYMBOL_P(x)) {
            CL_Obj tail = CL_NIL;
            CL_GC_PROTECT(tail);            /* depth = 3 */
            tail = cl_cons(qq_quote(x), CL_NIL);
            {
                CL_Obj val = cl_cons(sym_list, tail);
                CL_GC_UNPROTECT(3);
                return val;
            }
        }
        /* Self-evaluating atom (fixnum, char, string, …) */
        {
            CL_Obj tail = cl_cons(x, CL_NIL);
            CL_Obj val  = cl_cons(sym_list, tail);
            CL_GC_UNPROTECT(2);
            return val;
        }
    }

    {
        CL_Obj head = cl_car(x);

        /* (UNQUOTE expr) at depth 0: (LIST expr) */
        if (head == SYM_UNQUOTE && depth == 0) {
            CL_Obj tail = cl_cons(cl_car(cl_cdr(x)), CL_NIL);
            CL_Obj val  = cl_cons(sym_list, tail);
            CL_GC_UNPROTECT(2);
            return val;
        }

        /* (UNQUOTE-SPLICING expr) at depth 0: expr (spliced into APPEND) */
        if (head == SYM_UNQUOTE_SPLICING && depth == 0) {
            CL_Obj val = cl_car(cl_cdr(x));
            CL_GC_UNPROTECT(2);
            return val;
        }

        /* (UNQUOTE inner) at depth > 0 */
        if (head == SYM_UNQUOTE && depth > 0) {
            CL_Obj inner_form = cl_car(cl_cdr(x));

            /* Special case: (UNQUOTE (UNQUOTE-SPLICING expr)) at depth 1
             * This is ,,@expr — evaluate expr and wrap each element in UNQUOTE.
             * Produces: (MAPCAR (LAMBDA (#:V) (LIST 'UNQUOTE #:V)) expr)
             * which is spliced into the APPEND call. */
            if (depth == 1 && CL_CONS_P(inner_form) &&
                cl_car(inner_form) == SYM_UNQUOTE_SPLICING) {
                CL_Obj expr        = cl_car(cl_cdr(inner_form));
                CL_Obj gv          = CL_NIL;
                CL_Obj sym_mapcar  = CL_NIL;
                CL_Obj sym_lambda  = CL_NIL;
                CL_Obj lambda_form = CL_NIL;
                CL_Obj tail        = CL_NIL;
                CL_Obj name_str;
                /* depth = 2 + 6 extra = 8 at return */
                CL_GC_PROTECT(expr);
                CL_GC_PROTECT(gv);
                CL_GC_PROTECT(sym_mapcar);
                CL_GC_PROTECT(sym_lambda);
                CL_GC_PROTECT(lambda_form);
                CL_GC_PROTECT(tail);

                sym_mapcar = cl_intern_in("MAPCAR", 6, cl_package_cl);
                sym_lambda = cl_intern_in("LAMBDA", 6, cl_package_cl);
                name_str   = cl_make_string("%QQV", 4);
                gv         = cl_make_symbol(name_str);

                /* Build (LIST 'UNQUOTE #:V) step by step into tail */
                tail = cl_cons(gv, CL_NIL);
                tail = cl_cons(qq_quote(SYM_UNQUOTE), tail);
                tail = qq_list(tail);           /* (LIST 'UNQUOTE #:V) */

                /* Build ((LIST 'UNQUOTE #:V)) — the LAMBDA body list */
                tail = cl_cons(tail, CL_NIL);

                /* Build (LAMBDA (#:V) (LIST 'UNQUOTE #:V)) */
                {
                    CL_Obj params = cl_cons(gv, CL_NIL);  /* (#:V) */
                    lambda_form   = cl_cons(params, tail); /* (#:V (LIST ...)) */
                }
                lambda_form = cl_cons(sym_lambda, lambda_form);

                /* Build (MAPCAR lambda expr) */
                tail = cl_cons(expr, CL_NIL);
                tail = cl_cons(lambda_form, tail);
                {
                    CL_Obj val = cl_cons(sym_mapcar, tail);
                    CL_GC_UNPROTECT(8);
                    return val;
                }
            }

            /* General case: reconstruct as (LIST (LIST 'UNQUOTE inner')) */
            {
                CL_Obj inner = qq_expand(inner_form, depth - 1);
                CL_Obj tail = CL_NIL;
                CL_GC_PROTECT(inner);           /* depth = 3 */
                CL_GC_PROTECT(tail);            /* depth = 4 */
                tail = cl_cons(inner, CL_NIL);
                tail = cl_cons(qq_quote(SYM_UNQUOTE), tail);
                tail = qq_list(tail);           /* (LIST 'UNQUOTE inner) */
                tail = cl_cons(tail, CL_NIL);
                {
                    CL_Obj val = cl_cons(sym_list, tail);
                    CL_GC_UNPROTECT(4);
                    return val;
                }
            }
        }

        /* (UNQUOTE-SPLICING inner) at depth > 0 */
        if (head == SYM_UNQUOTE_SPLICING && depth > 0) {
            CL_Obj inner_form = cl_car(cl_cdr(x));
            /* Special case: (UNQUOTE-SPLICING (UNQUOTE-SPLICING expr)) at depth 1
             * This is ,@,@expr — very rare; fall through to the general case. */
            {
                CL_Obj inner = qq_expand(inner_form, depth - 1);
                CL_Obj tail = CL_NIL;
                CL_GC_PROTECT(inner);           /* depth = 3 */
                CL_GC_PROTECT(tail);            /* depth = 4 */
                tail = cl_cons(inner, CL_NIL);
                tail = cl_cons(qq_quote(SYM_UNQUOTE_SPLICING), tail);
                tail = qq_list(tail);           /* (LIST 'UNQUOTE-SPLICING inner) */
                tail = cl_cons(tail, CL_NIL);
                {
                    CL_Obj val = cl_cons(sym_list, tail);
                    CL_GC_UNPROTECT(4);
                    return val;
                }
            }
        }

        /* (QUASIQUOTE expr): increase depth, wrap in LIST */
        if (head == SYM_QUASIQUOTE) {
            CL_Obj inner = qq_expand(cl_car(cl_cdr(x)), depth + 1);
            CL_Obj tail = CL_NIL;
            CL_GC_PROTECT(inner);               /* depth = 3 */
            CL_GC_PROTECT(tail);                /* depth = 4 */
            tail = cl_cons(inner, CL_NIL);
            tail = cl_cons(qq_quote(SYM_QUASIQUOTE), tail);
            tail = qq_list(tail);               /* (LIST 'QUASIQUOTE inner) */
            tail = cl_cons(tail, CL_NIL);
            {
                CL_Obj val = cl_cons(sym_list, tail);
                CL_GC_UNPROTECT(4);
                return val;
            }
        }
    }

    /* Nested list: (LIST (qq-expand x depth)) */
    {
        CL_Obj expanded = qq_expand(x, depth);
        CL_Obj tail = CL_NIL;
        CL_GC_PROTECT(expanded);               /* depth = 3 */
        CL_GC_PROTECT(tail);                   /* depth = 4 */
        tail = cl_cons(expanded, CL_NIL);
        {
            CL_Obj val = cl_cons(sym_list, tail);
            CL_GC_UNPROTECT(4);
            return val;
        }
    }
}

/* Compile a quasiquote form by expanding to LIST/APPEND forms,
 * then compiling the result as a normal expression. */
void compile_quasiquote(CL_Compiler *c, CL_Obj form)
{
    CL_Obj tmpl = cl_car(cl_cdr(form));
    CL_Obj expanded;
    /* Protect tmpl so the template tree stays reachable during the
     * (potentially deep, allocating) recursive qq_expand traversal. */
    CL_GC_PROTECT(tmpl);
    expanded = qq_expand(tmpl, 0);
    CL_GC_UNPROTECT(1);
    compile_expr(c, expanded);
}

/* --- Case forms --- */

void compile_case(CL_Compiler *c, CL_Obj form, int error_if_no_match)
{
    /* (case keyform (key-or-keys body...)... [(t|otherwise body...)]) */
    CL_Obj keyform = cl_car(cl_cdr(form));
    CL_Obj clauses = cl_cdr(cl_cdr(form));
    CL_CompEnv *env = c->env;
    int saved_local_count = env->local_count;
    int saved_tail = c->in_tail;
    int temp_slot;
    /* Two exit chains so a stale arm can be given its own OP_MV_RESET when
     * the arms disagree — see cl_mv_close_join. */
    int done_chain = CL_JUMP_CHAIN_END;
    int stale_chain = CL_JUMP_CHAIN_END;
    int join_mv = CL_MV_ONE;
    int any_stale = 0;
    int had_default = 0;
    CL_Obj ecase_expected = CL_NIL;

    /* Allocate temp slot for keyform value */
    temp_slot = env->local_count;
    env->locals[temp_slot] = CL_NIL;  /* Clear stale binding */
    env->local_count++;
    if (env->local_count > env->max_locals)
        env->max_locals = env->local_count;

    /* GC SAFETY: protect `clauses` BEFORE compile_expr(keyform) — the keyform
     * compile macroexpands/allocates and can compact, and `form` is a by-value
     * parameter copy the caller's protection does not forward.  Protecting
     * here also covers the ecase pre-scan and clause-walk cl_cons calls. */
    CL_GC_PROTECT(clauses);

    /* Compile and store keyform */
    c->in_tail = 0;
    compile_expr(c, keyform);
    cl_emit(c, OP_STORE);
    cl_emit(c, (uint8_t)temp_slot);
    cl_emit(c, OP_POP);

    /* For ECASE, pre-scan the clause keys to build the expected-type
     * specifier (member k1 k2 ...) used in the fall-through TYPE-ERROR
     * (CLHS 5.3: ECASE signals an error of type TYPE-ERROR).  Built here,
     * while the clause list is fresh, and GC-protected across the main walk
     * (compile_progn below allocates and may move objects). */
    if (error_if_no_match) {
        CL_Obj keylist = CL_NIL;
        CL_Obj cl2 = clauses;
        CL_Obj ks;
        CL_GC_PROTECT(keylist);
        CL_GC_PROTECT(cl2);
        while (!CL_NULL_P(cl2)) {
            ks = cl_car(cl_car(cl2));
            if (ks != SYM_T && ks != SYM_OTHERWISE) {
                if (CL_CONS_P(ks)) {
                    CL_Obj k = ks;
                    CL_GC_PROTECT(k);
                    while (!CL_NULL_P(k)) {
                        keylist = cl_cons(cl_car(k), keylist);
                        k = cl_cdr(k);
                    }
                    CL_GC_UNPROTECT(1);
                } else {
                    keylist = cl_cons(ks, keylist);
                }
            }
            cl2 = cl_cdr(cl2);
        }
        /* keylist is read after the intern (mem.h, cl_list2) */
        ecase_expected = cl_intern_in("MEMBER", 6, cl_package_cl);
        ecase_expected = cl_cons(ecase_expected, keylist);
        CL_GC_UNPROTECT(2); /* cl2, keylist */
        CL_GC_PROTECT(ecase_expected);
    }

    /* Process clauses */
    while (!CL_NULL_P(clauses)) {
        CL_Obj clause = cl_car(clauses);
        CL_Obj keys = cl_car(clause);
        CL_Obj body = cl_cdr(clause);

        /* Protect keys and body across compile_progn — compile_progn is an
         * allocating call and a moving GC compaction invalidates bare locals. */
        CL_GC_PROTECT(keys);
        CL_GC_PROTECT(body);

        /* Check for default clause: T or OTHERWISE */
        if (keys == SYM_T || keys == SYM_OTHERWISE) {
            had_default = 1;
            c->in_tail = saved_tail;
            if (CL_NULL_P(body))
                cl_emit(c, OP_NIL);
            else
                compile_progn(c, body);
            join_mv = cl_mv_merge(join_mv, c->mv_state);
            if (c->mv_state == CL_MV_STALE) {
                any_stale = 1;
                stale_chain = cl_emit_jump_chain(c, OP_JMP, stale_chain);
            } else {
                done_chain = cl_emit_jump_chain(c, OP_JMP, done_chain);
            }
        } else {
            int body_chain = CL_JUMP_CHAIN_END;
            int next_clause_pos = -1;

            /* Emit EQ tests for each key.  Every key but the last jumps to
             * the body when it matches; the LAST key falls into the body
             * and jumps to the next clause when it does not — `EQ; JNIL
             * next` instead of `EQ; JTRUE body; JMP next`, one dispatch
             * fewer on the miss path (the peephole fuses it to EQ_JNIL) and
             * the shape every single-key clause takes. */
            if (CL_CONS_P(keys)) {
                /* Multiple keys: ((k1 k2 k3) body...) */
                CL_Obj k = keys;
                while (!CL_NULL_P(k)) {
                    int last = CL_NULL_P(cl_cdr(k));
                    cl_emit(c, OP_LOAD);
                    cl_emit(c, (uint8_t)temp_slot);
                    cl_emit_const(c, cl_car(k));
                    cl_emit(c, OP_EQ);
                    if (last)
                        next_clause_pos = cl_emit_jump(c, OP_JNIL);
                    else
                        body_chain = cl_emit_jump_chain(c, OP_JTRUE, body_chain);
                    k = cl_cdr(k);
                }
            } else {
                /* Single key: (key body...) */
                cl_emit(c, OP_LOAD);
                cl_emit(c, (uint8_t)temp_slot);
                cl_emit_const(c, keys);
                cl_emit(c, OP_EQ);
                next_clause_pos = cl_emit_jump(c, OP_JNIL);
            }

            /* body: patch all key-match jumps here */
            cl_patch_jump_chain(c, body_chain);

            /* Compile body */
            c->in_tail = saved_tail;
            if (CL_NULL_P(body))
                cl_emit(c, OP_NIL);
            else
                compile_progn(c, body);
            join_mv = cl_mv_merge(join_mv, c->mv_state);
            if (c->mv_state == CL_MV_STALE) {
                any_stale = 1;
                stale_chain = cl_emit_jump_chain(c, OP_JMP, stale_chain);
            } else {
                done_chain = cl_emit_jump_chain(c, OP_JMP, done_chain);
            }

            /* next_clause: */
            if (next_clause_pos >= 0) cl_patch_jump(c, next_clause_pos);
        }

        CL_GC_UNPROTECT(2); /* body, keys */
        clauses = cl_cdr(clauses);
    }
    /* Stack now: [clauses] (case) or [clauses, ecase_expected] (ecase) */

    /* No default matched */
    if (!had_default) {
        if (error_if_no_match) {
            /* ecase: signal (error 'type-error :datum <val>
             *                       :expected-type '(member k1 k2 ...))
             * per CLHS 5.3 (must be of type TYPE-ERROR, not SIMPLE-ERROR). */
            CL_Obj sym_error = cl_intern_in("ERROR", 5, cl_package_cl);
            int idx = cl_add_constant(c, sym_error);
            cl_emit(c, OP_FLOAD);
            cl_emit_u16(c, (uint16_t)idx);
            cl_emit_const(c, SYM_TYPE_ERROR);
            cl_emit_const(c, cl_intern_keyword("DATUM", 5));
            cl_emit(c, OP_LOAD);
            cl_emit(c, (uint8_t)temp_slot);
            cl_emit_const(c, cl_intern_keyword("EXPECTED-TYPE", 13));
            cl_emit_const(c, ecase_expected);
            cl_emit_call(c, OP_CALL, 5);
        } else {
            cl_emit(c, OP_NIL);
        }
    }
    if (error_if_no_match)
        CL_GC_UNPROTECT(1); /* ecase_expected */
    CL_GC_UNPROTECT(1); /* clauses */

    /* The no-match fall-through is an arm too; patch every exit, giving the
     * stale arms their own reset if they disagree with a multi-valued one. */
    if (c->mv_state == CL_MV_STALE) any_stale = 1;
    cl_mv_close_join(c, done_chain, stale_chain, any_stale,
                     cl_mv_merge(join_mv, c->mv_state));

    cl_env_clear_boxed(env, saved_local_count);

    /* Restore scope */
    env->local_count = saved_local_count;
}

void compile_typecase(CL_Compiler *c, CL_Obj form, int error_if_no_match)
{
    /* (typecase keyform (type-spec body...)... [(t|otherwise body...)])
     * Uses TYPEP for each clause — supports compound type specifiers. */
    CL_Obj keyform = cl_car(cl_cdr(form));
    CL_Obj clauses = cl_cdr(cl_cdr(form));
    CL_CompEnv *env = c->env;
    int saved_local_count = env->local_count;
    int saved_tail = c->in_tail;
    int temp_slot;
    /* Two exit chains so a stale arm can be given its own OP_MV_RESET when
     * the arms disagree — see cl_mv_close_join. */
    int done_chain = CL_JUMP_CHAIN_END;
    int stale_chain = CL_JUMP_CHAIN_END;
    int join_mv = CL_MV_ONE;
    int any_stale = 0;
    int had_default = 0;
    CL_Obj sym_typep;
    int typep_idx;
    CL_Obj etypecase_expected = CL_NIL;

    /* Allocate temp slot for keyform value */
    temp_slot = env->local_count;
    env->locals[temp_slot] = CL_NIL;  /* Clear stale binding */
    env->local_count++;
    if (env->local_count > env->max_locals)
        env->max_locals = env->local_count;

    /* GC SAFETY: protect `clauses` BEFORE compile_expr(keyform) — the keyform
     * compile macroexpands/allocates and can compact, and `form` is a by-value
     * parameter copy the caller's protection does not forward.  Protecting
     * here also covers the intern/pre-scan/clause-walk allocations below. */
    CL_GC_PROTECT(clauses);

    /* Compile and store keyform */
    c->in_tail = 0;
    compile_expr(c, keyform);
    cl_emit(c, OP_STORE);
    cl_emit(c, (uint8_t)temp_slot);
    cl_emit(c, OP_POP);

    /* Pre-load TYPEP symbol index — must use CL package explicitly,
     * not cl_current_package (which may be KEYWORD during #+#. eval) */
    sym_typep = cl_intern_in("TYPEP", 5, cl_package_cl);
    typep_idx = cl_add_constant(c, sym_typep);

    /* For ETYPECASE, pre-scan the clause type-specs to build the
     * expected-type specifier (or t1 t2 ...) used in the fall-through
     * TYPE-ERROR (CLHS 5.3: ETYPECASE signals an error of type TYPE-ERROR).
     * GC-protected across the main walk below. */
    if (error_if_no_match) {
        CL_Obj typelist = CL_NIL;
        CL_Obj cl2 = clauses;
        CL_Obj ty;
        CL_GC_PROTECT(typelist);
        CL_GC_PROTECT(cl2);
        while (!CL_NULL_P(cl2)) {
            ty = cl_car(cl_car(cl2));
            if (ty != SYM_T && ty != SYM_OTHERWISE)
                typelist = cl_cons(ty, typelist);
            cl2 = cl_cdr(cl2);
        }
        etypecase_expected = cl_cons(SYM_OR, typelist);
        CL_GC_UNPROTECT(2); /* cl2, typelist */
        CL_GC_PROTECT(etypecase_expected);
    }

    /* Process clauses */
    while (!CL_NULL_P(clauses)) {
        CL_Obj clause = cl_car(clauses);
        CL_Obj type_spec = cl_car(clause);
        CL_Obj body = cl_cdr(clause);

        /* Protect type_spec and body across compile_progn — compile_progn is an
         * allocating call and a moving GC compaction invalidates bare locals. */
        CL_GC_PROTECT(type_spec);
        CL_GC_PROTECT(body);

        if (type_spec == SYM_T || type_spec == SYM_OTHERWISE) {
            had_default = 1;
            c->in_tail = saved_tail;
            if (CL_NULL_P(body))
                cl_emit(c, OP_NIL);
            else
                compile_progn(c, body);
            join_mv = cl_mv_merge(join_mv, c->mv_state);
            if (c->mv_state == CL_MV_STALE) {
                any_stale = 1;
                stale_chain = cl_emit_jump_chain(c, OP_JMP, stale_chain);
            } else {
                done_chain = cl_emit_jump_chain(c, OP_JMP, done_chain);
            }
        } else {
            int jnil_pos;

            /* Emit: (typep keyform 'type-spec) */
            cl_emit(c, OP_FLOAD);
            cl_emit_u16(c, (uint16_t)typep_idx);
            cl_emit(c, OP_LOAD);
            cl_emit(c, (uint8_t)temp_slot);
            cl_emit_const(c, type_spec);
            cl_emit_call(c, OP_CALL, 2);
            jnil_pos = cl_emit_jump(c, OP_JNIL);

            /* Compile body */
            c->in_tail = saved_tail;
            if (CL_NULL_P(body))
                cl_emit(c, OP_NIL);
            else
                compile_progn(c, body);
            join_mv = cl_mv_merge(join_mv, c->mv_state);
            if (c->mv_state == CL_MV_STALE) {
                any_stale = 1;
                stale_chain = cl_emit_jump_chain(c, OP_JMP, stale_chain);
            } else {
                done_chain = cl_emit_jump_chain(c, OP_JMP, done_chain);
            }

            cl_patch_jump(c, jnil_pos);
        }

        CL_GC_UNPROTECT(2); /* body, type_spec */
        clauses = cl_cdr(clauses);
    }
    /* Stack now: [clauses] (typecase) or [clauses, etypecase_expected] (etypecase) */

    /* No default */
    if (!had_default) {
        if (error_if_no_match) {
            /* etypecase: signal (error 'type-error :datum <val>
             *                          :expected-type '(or t1 t2 ...))
             * per CLHS 5.3 (must be of type TYPE-ERROR). */
            CL_Obj sym_error = cl_intern_in("ERROR", 5, cl_package_cl);
            int idx = cl_add_constant(c, sym_error);
            cl_emit(c, OP_FLOAD);
            cl_emit_u16(c, (uint16_t)idx);
            cl_emit_const(c, SYM_TYPE_ERROR);
            cl_emit_const(c, cl_intern_keyword("DATUM", 5));
            cl_emit(c, OP_LOAD);
            cl_emit(c, (uint8_t)temp_slot);
            cl_emit_const(c, cl_intern_keyword("EXPECTED-TYPE", 13));
            cl_emit_const(c, etypecase_expected);
            cl_emit_call(c, OP_CALL, 5);
        } else {
            cl_emit(c, OP_NIL);
        }
    }
    if (error_if_no_match)
        CL_GC_UNPROTECT(1); /* etypecase_expected */
    CL_GC_UNPROTECT(1); /* clauses */

    /* The no-match fall-through is an arm too; patch every exit, giving the
     * stale arms their own reset if they disagree with a multi-valued one. */
    if (c->mv_state == CL_MV_STALE) any_stale = 1;
    cl_mv_close_join(c, done_chain, stale_chain, any_stale,
                     cl_mv_merge(join_mv, c->mv_state));

    cl_env_clear_boxed(env, saved_local_count);

    env->local_count = saved_local_count;
}

/* --- Multiple Values --- */

CL_Obj compile_multiple_value_bind(CL_Compiler *c, CL_Obj form)
{
    /* (multiple-value-bind (vars...) values-form body...)
     *
     * Trampoline-aware: prelude binds the locals and resets MV state,
     * body's tail form returns to the driver loop, postlude restores
     * env->local_count + in_tail.  Without this, deeply nested MVB
     * (common in optimized macro output) grows the C stack one frame
     * per level. */
    CL_Obj vars_list = cl_car(cl_cdr(form));
    CL_Obj values_form = cl_car(cl_cdr(cl_cdr(form)));
    CL_Obj body = cl_cdr(cl_cdr(cl_cdr(form)));
    CL_CompEnv *env = c->env;
    int saved_local_count = env->local_count;
    int saved_tail = c->in_tail;
    CL_Obj vl;
    int var_index;
    CL_TailFrame *tf;

    /* GC-protect vars_list and body — compile_expr can compact, making them stale */
    CL_GC_PROTECT(vars_list);
    CL_GC_PROTECT(body);

    /* Compile values form — primary on stack, MV buffer set.  The OP_MV_LOADs
     * below read that buffer, so it must describe VALUES-FORM and not some
     * earlier form (cl_mv_normalize); OP_STORE/OP_POP in between are
     * deliberately MV-transparent. */
    c->in_tail = 0;
    compile_expr(c, values_form);
    cl_mv_normalize(c);

    /* Load MV values into local slots.
     * Var 0 uses the primary from the stack (reliable).
     * Vars 1+ use OP_MV_LOAD (reads from MV buffer, set by bi_values). */
    var_index = 0;
    vl = vars_list;
    while (!CL_NULL_P(vl)) {
        CL_Obj var = cl_car(vl);  /* re-read each iter from protected cursor */
        int slot = cl_env_add_local(env, var);
        if (var_index == 0) {
            /* Primary value is on stack top */
            cl_emit(c, OP_STORE);
            cl_emit(c, (uint8_t)slot);
            cl_emit(c, OP_POP);
        } else {
            cl_emit(c, OP_MV_LOAD);
            cl_emit(c, (uint8_t)var_index);
            cl_emit(c, OP_STORE);
            cl_emit(c, (uint8_t)slot);
            cl_emit(c, OP_POP);
        }
        var_index++;
        vl = cl_cdr(vl);
    }
    CL_GC_UNPROTECT(2);  /* vars_list, body (cl_tail_push uses platform_alloc, not GC) */

    /* If no vars, still need to pop the primary */
    if (var_index == 0)
        cl_emit(c, OP_POP);

    if (env->local_count > env->max_locals)
        env->max_locals = env->local_count;

    /* Reset MV state so stale values don't leak through the body */
    cl_emit(c, OP_MV_RESET);

    c->in_tail = saved_tail;

    tf = cl_tail_push(c);
    tf->kind = CL_TAIL_MULTIPLE_VALUE_BIND;
    tf->saved_local_count = saved_local_count;
    tf->saved_tail = saved_tail;
    tf->saved_notinline_count = c->notinline_count;
    return compile_body_tail(c, body);
}

void compile_multiple_value_list(CL_Compiler *c, CL_Obj form)
{
    /* (multiple-value-list form) */
    CL_Obj values_form = cl_car(cl_cdr(form));
    int saved_tail = c->in_tail;

    c->in_tail = 0;
    compile_expr(c, values_form);
    cl_mv_normalize(c);          /* the list must be VALUES-FORM's own values */
    cl_emit(c, OP_MV_TO_LIST);   /* pops primary, builds list from MV state */

    c->in_tail = saved_tail;
}

void compile_nth_value(CL_Compiler *c, CL_Obj form)
{
    /* (nth-value n form) */
    CL_Obj n_form = cl_car(cl_cdr(form));
    CL_Obj values_form = cl_car(cl_cdr(cl_cdr(form)));
    int saved_tail = c->in_tail;

    c->in_tail = 0;

    if (CL_FIXNUM_P(n_form)) {
        int idx = CL_FIXNUM_VAL(n_form);
        compile_expr(c, values_form);
        if (idx == 0) {
            /* Primary value is already on stack — nothing to do */
        } else {
            cl_mv_normalize(c);  /* OP_MV_LOAD must read VALUES-FORM's buffer */
            cl_emit(c, OP_POP);  /* discard primary */
            cl_emit(c, OP_MV_LOAD);
            cl_emit(c, (uint8_t)(idx < 0 ? 255 : idx > 255 ? 255 : idx));
        }
        /* nth-value returns a single value per CL spec — clear stale MV state
         * so callers using multiple-value-list see only this one value */
        cl_emit(c, OP_MV_RESET);
        c->mv_state = CL_MV_ONE;
    } else {
        /* Dynamic index: stack will be [index] [primary].
         * GC SAFETY: compile_expr(n_form) allocates/compacts and `form` is a
         * by-value parameter copy — protect values_form across it. */
        CL_GC_PROTECT(values_form);
        compile_expr(c, n_form);
        compile_expr(c, values_form);
        CL_GC_UNPROTECT(1);
        cl_mv_normalize(c);       /* OP_NTH_VALUE reads the MV buffer */
        cl_emit(c, OP_NTH_VALUE); /* pops primary, pops index, pushes result */
    }

    c->in_tail = saved_tail;
}

void compile_multiple_value_prog1(CL_Compiler *c, CL_Obj form)
{
    /* (multiple-value-prog1 first-form forms...)
     * Evaluate first-form, save all its values,
     * evaluate remaining forms for effect, restore saved values. */
    CL_Obj first_form = cl_car(cl_cdr(form));
    CL_Obj rest_forms = cl_cdr(cl_cdr(form));
    CL_CompEnv *env = c->env;
    int saved_local_count = env->local_count;
    int saved_tail = c->in_tail;
    int list_slot;

    c->in_tail = 0;

    /* GC SAFETY: protect rest_forms BEFORE compile_expr(first_form) — that
     * compile allocates/compacts, and the inner cursor block below would
     * otherwise start from a stale offset (`form` is a by-value parameter
     * copy the caller's protection does not forward). */
    CL_GC_PROTECT(rest_forms);

    /* Compile first form — primary on stack, MV buffer set */
    compile_expr(c, first_form);
    cl_mv_normalize(c);          /* save FIRST-FORM's values, not an older set */
    cl_emit(c, OP_MV_TO_LIST);   /* pops primary, saves all values as a list */

    /* Allocate slot for saved list */
    list_slot = env->local_count;
    env->locals[list_slot] = CL_NIL;  /* Clear stale binding */
    env->local_count++;
    if (env->local_count > env->max_locals)
        env->max_locals = env->local_count;
    cl_emit(c, OP_STORE);
    cl_emit(c, (uint8_t)list_slot);
    cl_emit(c, OP_POP);

    /* Evaluate remaining forms for effect — rest_forms is protected above
     * and doubles as the walk cursor (forwarded on compaction). */
    while (!CL_NULL_P(rest_forms)) {
        compile_expr(c, cl_car(rest_forms));
        cl_emit(c, OP_POP);
        rest_forms = cl_cdr(rest_forms);
    }
    CL_GC_UNPROTECT(1); /* rest_forms */

    /* Restore MV state by calling VALUES-LIST on saved list */
    {
        CL_Obj vl_sym = cl_intern_in("VALUES-LIST", 11, cl_package_cl);
        int sym_idx = cl_add_constant(c, vl_sym);
        cl_emit(c, OP_FLOAD);
        cl_emit_u16(c, (uint16_t)sym_idx);
        cl_emit(c, OP_LOAD);
        cl_emit(c, (uint8_t)list_slot);
        cl_emit_call(c, OP_CALL, 1);
    }

    c->in_tail = saved_tail;

    cl_env_clear_boxed(env, saved_local_count);

    env->local_count = saved_local_count;
}

void compile_multiple_value_call(CL_Compiler *c, CL_Obj form)
{
    /* (multiple-value-call fn form1 form2 ...)
     * => (apply fn (append (multiple-value-list form1) ...))
     * Build the expansion at compile time and compile it. */
    CL_Obj fn_form = cl_car(cl_cdr(form));
    CL_Obj value_forms = cl_cdr(cl_cdr(form));
    CL_Obj append_args = CL_NIL;
    CL_Obj result, f;
    CL_Obj sym_apply, sym_append, sym_mvl;

    sym_apply  = cl_intern_in("APPLY", 5, cl_package_cl);
    sym_append = cl_intern_in("APPEND", 6, cl_package_cl);
    sym_mvl    = cl_intern_in("MULTIPLE-VALUE-LIST", 19, cl_package_cl);

    /* GC SAFETY: the conses below can compact — protect the cursor and the
     * interned symbols (symbols move under compaction too), and build each
     * chain one cl_cons at a time. */
    CL_GC_PROTECT(fn_form);
    CL_GC_PROTECT(append_args);
    CL_GC_PROTECT(sym_apply);
    CL_GC_PROTECT(sym_append);
    CL_GC_PROTECT(sym_mvl);

    /* Build list of (multiple-value-list formN) in reverse */
    f = value_forms;
    CL_GC_PROTECT(f);
    for (; !CL_NULL_P(f); f = cl_cdr(f)) {
        CL_Obj mvl_call = cl_cons(cl_car(f), CL_NIL);
        mvl_call = cl_cons(sym_mvl, mvl_call);
        append_args = cl_cons(mvl_call, append_args);
    }
    CL_GC_UNPROTECT(1);  /* f */

    /* Reverse to restore order */
    {
        CL_Obj rev = CL_NIL;
        CL_GC_PROTECT(rev);
        while (!CL_NULL_P(append_args)) {
            rev = cl_cons(cl_car(append_args), rev);
            append_args = cl_cdr(append_args);
        }
        append_args = rev;
        CL_GC_UNPROTECT(1);
    }

    /* Build (apply fn (append mvl1 mvl2 ...)) — sequenced conses */
    result = cl_cons(sym_append, append_args);
    result = cl_cons(result, CL_NIL);
    result = cl_cons(fn_form, result);
    result = cl_cons(sym_apply, result);
    CL_GC_UNPROTECT(5);  /* fn_form, append_args, sym_apply, sym_append, sym_mvl */

    compile_expr(c, result);
}

/* --- Eval-when / Defsetf --- */

CL_Obj compile_eval_when(CL_Compiler *c, CL_Obj form)
{
    /* (eval-when (situations...) body...)
     * In single-pass compile-and-eval, always execute body.
     * At top level with multiple forms: if any is defmacro/deftype,
     * compile and eval each individually so macros are available for
     * later forms.
     *
     * Trampoline-aware: the body's tail form returns to the driver loop
     * via a CL_TAIL_EVAL_WHEN frame (no postlude needed — pure body
     * wrapper).  The two-pass branch (top-level with definitions) still
     * recurses into compile_progn for the second pass; that path runs
     * at toplevel so it doesn't compound deep nesting. */
    CL_Obj body = cl_cdr(cl_cdr(form));
    CL_TailFrame *tf;

    if (c->env->parent == NULL &&
        !CL_NULL_P(body) && !CL_NULL_P(cl_cdr(body))) {
        /* Check if any body form is a definition that later forms might depend on */
        int has_def = 0;
        CL_Obj rest;
        for (rest = body; !CL_NULL_P(rest); rest = cl_cdr(rest)) {
            CL_Obj sub = cl_car(rest);
            if (CL_CONS_P(sub)) {
                CL_Obj head = cl_car(sub);
                if (head == SYM_DEFMACRO || head == SYM_DEFTYPE) {
                    has_def = 1;
                    break;
                }
            }
        }
        if (has_def) {
            /* First pass: compile and eval each form individually so
               macros/types are available for later forms in this body.
               GC SAFETY: protect body for the whole pass — it is handed to
               compile_progn_tail below and cl_compile/cl_vm_eval compact. */
            CL_GC_PROTECT(body);
            for (rest = body; !CL_NULL_P(rest); rest = cl_cdr(rest)) {
                CL_Obj sub = cl_car(rest);
                CL_Obj bc;
                CL_GC_PROTECT(rest);
                bc = cl_compile(sub);
                if (!CL_NULL_P(bc)) {
                    cl_vm_eval(bc);
                }
                CL_GC_UNPROTECT(1);
            }
            CL_GC_UNPROTECT(1);  /* body */
            /* Second pass falls through to the trampoline body below so
             * bytecodes are preserved for FASL serialization (compile-file).
             * Macros are now available; definitions may eval twice — harmless. */
        }
    }

    tf = cl_tail_push(c);
    tf->kind = CL_TAIL_EVAL_WHEN;
    return compile_progn_tail(c, body);
}

void compile_load_time_value(CL_Compiler *c, CL_Obj form)
{
    /* (load-time-value form &optional read-only-p)
     *
     * COMPILE-FILE (cl_compiling_to_file==1): defer evaluation to load time
     * by synthesizing a memoized LET form.  The value-form is compiled into
     * the function body so it only runs when the FASL is loaded and the
     * function is first called — by which point any forward-referenced
     * functions are defined.  A fresh (NIL . NIL) cell per LTV occurrence
     * acts as the memo slot; its CAR is T once initialized, CDR holds the value.
     *
     * REPL/LOAD (cl_compiling_to_file==0): compile-time IS load-time.
     * Evaluate now in the null lexical environment, emit result as a constant. */
    CL_Obj value_form = cl_car(cl_cdr(form));
    CL_Obj bytecode, result;

    if (cl_compiling_to_file) {
        /* CLHS 3.2.4.4: value-form is evaluated in a null lexical environment
         * at load time (not compile time, not first-call time).
         *
         * Implementation:
         *  1. Compile value-form via cl_compile() (null lexical env) → thunk.
         *  2. Build an init bytecode: (progn (rplacd cell (funcall thunk))
         *                                    (rplaca cell t))
         *     Append this to cl_ltv_init_buf so cf_process_toplevel_form emits
         *     it as a FASL unit BEFORE the containing defun — the FASL loader
         *     runs it at load time, satisfying CLHS.
         *  3. Inline a safety fallback in the function body:
         *       (let ((G (quote cell)))
         *         (if (car G) (cdr G)
         *             (progn (rplacd G (funcall (quote thunk)))
         *                    (rplaca G t) (cdr G))))
         *     The IF guard handles the degenerate case where the init thunk
         *     didn't run (e.g. function called before FASL loader completed).
         *
         * Both the init thunk and the fallback use (funcall thunk) so that
         * value-form is always evaluated in the null lexical environment that
         * cl_compile() provides, never in the enclosing function's env. */
        CL_Obj cell, gensym, thunk, quoted_cell, quoted_thunk, funcall_thunk;
        CL_Obj car_g, cdr_g1, cdr_g2;
        CL_Obj rplacd_call, rplaca_call, progn_form;
        CL_Obj if_form, let_binding, let_bindings, let_form;
        CL_Obj tmp;

        CL_GC_PROTECT(value_form);         /*  1 */

        /* Build (lambda () value_form), compile in null lexical env, then
         * evaluate ONCE to get a callable closure.  cl_compile uses null env
         * (CLHS 3.2.4.4, Issue [1]).  Evaluating immediately yields a closure
         * rather than a raw bytecode program, so (funcall (quote thunk)) in
         * both the inline init code and the safety fallback works correctly. */
        {
            CL_Obj ltv_bc;
            tmp = cl_cons(value_form, CL_NIL);    /* (value_form) */
            tmp = cl_cons(CL_NIL, tmp);            /* (() value_form) */
            tmp = cl_cons(SYM_LAMBDA, tmp);        /* (lambda () value_form) */
            CL_GC_PROTECT(tmp);
            ltv_bc = cl_compile(tmp);
            CL_GC_UNPROTECT(1);
            CL_GC_PROTECT(ltv_bc);
            thunk = cl_vm_eval(ltv_bc);
            CL_GC_UNPROTECT(1);
        }
        CL_GC_PROTECT(thunk);              /*  2 */

        cell = cl_cons(CL_NIL, CL_NIL);
        CL_GC_PROTECT(cell);               /*  3 */
        gensym = cl_gensym_with_name("LTV");
        CL_GC_PROTECT(gensym);             /*  4 */

        /* (QUOTE <cell>) */
        tmp = cl_cons(cell, CL_NIL);
        quoted_cell = cl_cons(SYM_QUOTE, tmp);
        CL_GC_PROTECT(quoted_cell);        /*  5 */

        /* (QUOTE thunk) */
        tmp = cl_cons(thunk, CL_NIL);
        quoted_thunk = cl_cons(SYM_QUOTE, tmp);
        CL_GC_PROTECT(quoted_thunk);       /*  6 */

        /* (FUNCALL (QUOTE thunk)) — shared by init form and safety fallback */
        tmp = cl_cons(quoted_thunk, CL_NIL);
        funcall_thunk = cl_cons(SYM_FUNCALL, tmp);
        CL_GC_PROTECT(funcall_thunk);      /*  7 */

        /* Register (cell, thunk) for compile_defun to emit inline init code.
         * compile_defun calls compile_expr(c, init_form) in the OUTER compiler
         * so the cell constant is shared with the lambda body within one FASL
         * unit — FASL deduplication then makes them the same object at load
         * time (CLHS 3.2.4.4 load-time evaluation). */
        if (cl_ltv_init_count < CL_LTV_INIT_MAX) {
            CT->ltv_init_cells[cl_ltv_init_count]  = cell;
            CT->ltv_init_thunks[cl_ltv_init_count] = thunk;
            cl_ltv_init_count++;
        }
        /* Protection count: 7 (value_form thunk cell gensym quoted_cell
         * quoted_thunk funcall_thunk) */

        /* --- Build function body with safety fallback ---
           (LET ((G (QUOTE cell)))
             (IF (CAR G) (CDR G)
                 (PROGN (RPLACD G (FUNCALL (QUOTE thunk)))
                        (RPLACA G T) (CDR G)))) */

        /* Each operator is interned in the statement before the list that
         * uses it, never inside that call's argument list (mem.h, cl_list2):
         * `tmp` holds it for no longer than that. */

        /* (CAR G) */
        tmp = cl_intern_in("CAR", 3, cl_package_cl);
        car_g = cl_list2(tmp, gensym);
        CL_GC_PROTECT(car_g);              /*  8 */

        /* (CDR G) — then-branch */
        tmp = cl_intern_in("CDR", 3, cl_package_cl);
        cdr_g1 = cl_list2(tmp, gensym);
        CL_GC_PROTECT(cdr_g1);             /*  9 */

        /* (CDR G) — fallback tail */
        tmp = cl_intern_in("CDR", 3, cl_package_cl);
        cdr_g2 = cl_list2(tmp, gensym);
        CL_GC_PROTECT(cdr_g2);             /* 10 */

        /* (RPLACD G (FUNCALL (QUOTE thunk))) */
        tmp = cl_intern_in("RPLACD", 6, cl_package_cl);
        rplacd_call = cl_list3(tmp, gensym, funcall_thunk);
        CL_GC_PROTECT(rplacd_call);        /* 11 */

        /* (RPLACA G T) */
        tmp = cl_intern_in("RPLACA", 6, cl_package_cl);
        rplaca_call = cl_list3(tmp, gensym, SYM_T);
        CL_GC_PROTECT(rplaca_call);        /* 12 */

        /* (PROGN (RPLACD G ...) (RPLACA G T) (CDR G)) */
        tmp = cl_intern_in("PROGN", 5, cl_package_cl);
        progn_form = cl_list4(tmp, rplacd_call, rplaca_call, cdr_g2);
        CL_GC_PROTECT(progn_form);         /* 13 */

        /* (IF (CAR G) (CDR G) (PROGN ...)) */
        tmp = cl_intern_in("IF", 2, cl_package_cl);
        if_form = cl_list4(tmp, car_g, cdr_g1, progn_form);
        CL_GC_PROTECT(if_form);            /* 14 */

        /* LET binding: (G (QUOTE <cell>)) */
        tmp = cl_cons(quoted_cell, CL_NIL);
        let_binding = cl_cons(gensym, tmp);
        CL_GC_PROTECT(let_binding);        /* 15 */

        /* LET bindings list: ((G (QUOTE <cell>))) */
        let_bindings = cl_cons(let_binding, CL_NIL);
        CL_GC_PROTECT(let_bindings);       /* 16 */

        /* (LET ((G (QUOTE <cell>))) (IF ...)) */
        tmp = cl_intern_in("LET", 3, cl_package_cl);
        let_form = cl_list3(tmp, let_bindings, if_form);
        CL_GC_PROTECT(let_form);           /* 17 */

        compile_expr(c, let_form);
        CL_GC_UNPROTECT(17);
        return;
    }

    /* REPL/LOAD path: keep existing eager behavior unchanged. */
    CL_GC_PROTECT(form);

    bytecode = cl_compile(value_form);
    if (CL_NULL_P(bytecode)) {
        cl_emit(c, OP_NIL);
        CL_GC_UNPROTECT(1);
        return;
    }

    CL_GC_PROTECT(bytecode);
    result = cl_vm_eval(bytecode);
    CL_GC_UNPROTECT(1); /* bytecode */

    if (CL_NULL_P(result))
        cl_emit(c, OP_NIL);
    else if (result == SYM_T)
        cl_emit(c, OP_T);
    else
        cl_emit_const(c, result);

    CL_GC_UNPROTECT(1); /* form */
}

void compile_defsetf(CL_Compiler *c, CL_Obj form)
{
    /* Short form: (defsetf accessor updater)
     * Registers that (setf (accessor args...) val) → (updater args... val) */
    CL_Obj accessor = cl_car(cl_cdr(form));
    CL_Obj third    = cl_car(cl_cdr(cl_cdr(form)));
    CL_Obj updater;
    int acc_idx, upd_idx;

    /* Long form (CLHS 5.5.5):
     *   (defsetf access-fn defsetf-lambda-list (store-var*) [decl|doc]* form*)
     * The 3rd element is the access lambda list (a cons), or an empty
     * lambda list (NIL) followed by a store-variable list.  The short
     * form's updater is always a (non-NIL) symbol, so a cons in the 3rd
     * slot — or NIL with trailing forms — unambiguously marks the long
     * form.  Delegate to the Lisp helper amilisp::%defsetf-long, which
     * expands into a define-setf-expander so subforms are evaluated once
     * and &optional/&key defaults in the lambda list are honored. */
    if (CL_CONS_P(third) ||
        (CL_NULL_P(third) && !CL_NULL_P(cl_cdr(cl_cdr(cl_cdr(form)))))) {
        CL_Obj sym, new_form;
        /* GC SAFETY: cl_intern_in can allocate — protect form so the
         * cl_cdr(form) read below sees the forwarded list. */
        CL_GC_PROTECT(form);
        sym = cl_intern_in("%DEFSETF-LONG", 13, cl_package_amilisp);
        CL_GC_PROTECT(sym);
        new_form = cl_cons(sym, cl_cdr(form));
        CL_GC_UNPROTECT(2);  /* sym, form */
        CL_GC_PROTECT(new_form);
        compile_expr(c, new_form);
        CL_GC_UNPROTECT(1);
        return;
    }

    updater = third;

    /* GC SAFETY: the table conses below can compact — keep accessor/updater
     * forwarded for the cl_add_constant calls after (a stale symbol in the
     * constant pool would make OP_DEFSETF register a non-canonical name). */
    CL_GC_PROTECT(accessor);
    CL_GC_PROTECT(updater);

    /* Store mapping in setf_table at compile time (immediate side effect).
     * The registrar conses outside the write lock (STW-vs-rwlock
     * deadlock — see cl_alist_index_prepend). */
    cl_register_setf_updater(accessor, updater);

    /* Emit OP_DEFSETF so the mapping is registered at load time (FASL) */
    acc_idx = cl_add_constant(c, accessor);
    upd_idx = cl_add_constant(c, updater);
    CL_GC_UNPROTECT(2);  /* accessor, updater */
    cl_emit(c, OP_DEFSETF);
    cl_emit(c, (uint8_t)(acc_idx >> 8));
    cl_emit(c, (uint8_t)(acc_idx));
    cl_emit(c, (uint8_t)(upd_idx >> 8));
    cl_emit(c, (uint8_t)(upd_idx));
}

/* --- Deftype --- */

/* Docstring capture, defined with the DEFVAR family below. */
static CL_Obj body_docstring(CL_Obj body);
static void emit_doc_call(CL_Compiler *c, CL_Obj name,
                          const char *doc_type, int doc_type_len, CL_Obj doc);

void compile_deftype(CL_Compiler *c, CL_Obj form)
{
    /* (deftype name lambda-list body...)
     * Compile the expander as a lambda, then emit OP_DEFTYPE */
    CL_Obj name = cl_car(cl_cdr(form));
    CL_Obj lambda_list = cl_car(cl_cdr(cl_cdr(form)));
    CL_Obj body = cl_cdr(cl_cdr(cl_cdr(form)));
    CL_Obj doc = body_docstring(body);
    CL_Obj lambda_form;
    int idx;

    /* GC SAFETY: `name` is a raw C local read out of `form`.  cl_compile_env
     * does NOT re-protect the `expr` it forwards, so `form` (and thus a bare
     * `name` read from it) is rooted only in the bi_load/compile-file caller's
     * frame — a separate C local that the compactor forwards independently.
     * Every allocating call below (the cl_cons pair that builds lambda_form,
     * then compile_expr compiling the whole expander lambda) can compact and
     * relocate the symbol; an unprotected `name` then holds a stale offset (an
     * orphan ~1 object off from the relocated canonical symbol).  cl_add_constant
     * would bake that stale offset into the constant pool, so OP_DEFTYPE
     * registers a NON-canonical name in type_table and cl_get_type_expander's
     * `cl_car(pair) == name` identity check never matches the interned symbol —
     * subtypep/upgraded-array-element-type then silently fail for the deftype.
     * Protect BEFORE the first allocation, not after.  `doc` likewise. */
    CL_GC_PROTECT(name);
    CL_GC_PROTECT(doc);

    lambda_form = cl_list_star3(SYM_LAMBDA, lambda_list, body);
    CL_GC_PROTECT(lambda_form);

    compile_expr(c, lambda_form);

    CL_GC_UNPROTECT(1);            /* lambda_form — name, doc stay protected */

    /* OP_DEFTYPE pops the closure, registers type, pushes name */
    idx = cl_add_constant(c, name);
    cl_emit(c, OP_DEFTYPE);
    cl_emit_u16(c, (uint16_t)idx);
    emit_doc_call(c, name, "TYPE", 4, doc);
    CL_GC_UNPROTECT(2);            /* doc, name */
}

/* --- Defvar / Defparameter --- */

/* Emit a runtime call (BUILTIN 'NAME), leaving its value on the stack;
 * emit_mark_call discards it.
 *
 * Used by defconstant / defvar to mark a symbol constant / special at LOAD
 * time.  The compile-time side effect (setting the symbol flag while
 * compiling) does not survive when the form is later loaded from a
 * precompiled FASL in a fresh session, so the marking must also happen via
 * emitted bytecode.  BUILTIN is interned in the AMILISP package.
 *
 * GC SAFETY: building (BUILTIN (QUOTE NAME)) conses three cells via allocating
 * calls; every intermediate is protected across the next allocation so a
 * compacting GC (deterministically under gc-stress) cannot leave a stale
 * C-local offset.  NAME must already be GC-protected by the caller. */
static void emit_quoted_call(CL_Compiler *c, const char *builtin,
                             int builtin_len, CL_Obj name)
{
    CL_Obj inner, quoted, b_sym, b_arg, mark_form;
    inner = cl_cons(name, CL_NIL);
    CL_GC_PROTECT(inner);
    quoted = cl_cons(SYM_QUOTE, inner);
    CL_GC_UNPROTECT(1);            /* inner (now reachable from quoted) */
    CL_GC_PROTECT(quoted);
    b_sym = cl_intern_in(builtin, builtin_len, cl_package_amilisp);
    CL_GC_PROTECT(b_sym);
    b_arg = cl_cons(quoted, CL_NIL);
    CL_GC_PROTECT(b_arg);
    mark_form = cl_cons(b_sym, b_arg);
    CL_GC_UNPROTECT(2);            /* b_sym, b_arg (reachable from mark_form) */
    CL_GC_PROTECT(mark_form);
    compile_expr(c, mark_form);
    CL_GC_UNPROTECT(2);            /* quoted, mark_form */
}

static void emit_mark_call(CL_Compiler *c, const char *builtin, int builtin_len,
                           CL_Obj name)
{
    emit_quoted_call(c, builtin, builtin_len, name);
    cl_emit(c, OP_POP);
}

/* --- Documentation strings --- */

/* The docstring of a DEFUN / DEFMACRO / DEFTYPE body, or CL_NIL.
 *
 * CLHS 3.4.11: a string among the leading declarations is documentation
 * only when at least one form follows it -- `(defun f () "x")` RETURNS
 * "x".  Mirrors the skip in process_body_declarations, which is where the
 * string used to disappear (specs/documentation-introspection.md). */
static CL_Obj body_docstring(CL_Obj body)
{
    CL_Obj forms = body;
    while (CL_CONS_P(forms)) {
        CL_Obj form = cl_car(forms);
        if (CL_ANY_STRING_P(form))
            return CL_NULL_P(cl_cdr(forms)) ? CL_NIL : form;
        if (!(CL_CONS_P(form) && cl_car(form) == SYM_DECLARE))
            return CL_NIL;
        forms = cl_cdr(forms);
    }
    return CL_NIL;
}

/* Emit (AMILISP::%SET-DOCUMENTATION 'NAME 'DOC-TYPE DOC) and discard its
 * value; a no-op when DOC is not a string or EXT:*CAPTURE-DOCUMENTATION*
 * is NIL.
 *
 * A load-time call, for the same reason emit_mark_call is one: recording
 * the string while compiling would be lost the moment the definition is
 * loaded from a FASL, and a lean image wants to be able to drop the
 * strings altogether (the switch).  DOC_TYPE is the name of a CL symbol --
 * FUNCTION, VARIABLE, TYPE.  %SET-DOCUMENTATION is the boot.lisp writer
 * behind (SETF DOCUMENTATION); it is defined before the first docstring
 * in boot.lisp so a source boot can run these calls too.
 *
 * GC SAFETY: every intermediate is protected across the next allocation;
 * NAME must already be protected by the caller. */
static void emit_doc_call(CL_Compiler *c, CL_Obj name,
                          const char *doc_type, int doc_type_len, CL_Obj doc)
{
    CL_Obj tmp, q_name, q_type, args, form, sym;

    if (!CL_ANY_STRING_P(doc) || !cl_capture_documentation_p())
        return;

    CL_GC_PROTECT(doc);                             /* 1 */
    tmp = cl_cons(name, CL_NIL);
    CL_GC_PROTECT(tmp);
    q_name = cl_cons(SYM_QUOTE, tmp);
    CL_GC_UNPROTECT(1);                             /* tmp */
    CL_GC_PROTECT(q_name);                          /* 2 */
    sym = cl_intern_in(doc_type, (uint32_t)doc_type_len, cl_package_cl);
    CL_GC_PROTECT(sym);
    tmp = cl_cons(sym, CL_NIL);
    CL_GC_UNPROTECT(1);                             /* sym */
    CL_GC_PROTECT(tmp);
    q_type = cl_cons(SYM_QUOTE, tmp);
    CL_GC_UNPROTECT(1);                             /* tmp */
    CL_GC_PROTECT(q_type);                          /* 3 */
    args = cl_cons(doc, CL_NIL);
    CL_GC_PROTECT(args);                            /* 4 */
    args = cl_cons(q_type, args);
    args = cl_cons(q_name, args);
    sym = cl_intern_in("%SET-DOCUMENTATION", 18, cl_package_amilisp);
    CL_GC_PROTECT(sym);
    form = cl_cons(sym, args);
    CL_GC_UNPROTECT(1);                             /* sym */
    CL_GC_PROTECT(form);                            /* 5 */
    compile_expr(c, form);
    cl_emit(c, OP_POP);
    CL_GC_UNPROTECT(5);            /* form, args, q_type, q_name, doc */
}

/* The third argument of (DEFVAR / DEFPARAMETER / DEFCONSTANT name init doc),
 * or CL_NIL.  REST is the tail after the name. */
static CL_Obj defvar_docstring(CL_Obj rest)
{
    if (CL_CONS_P(rest) && CL_CONS_P(cl_cdr(rest)))
        return cl_car(cl_cdr(rest));
    return CL_NIL;
}

void compile_defvar(CL_Compiler *c, CL_Obj form)
{
    /* (defvar name) or (defvar name init-form) */
    CL_Obj name = cl_car(cl_cdr(form));
    CL_Obj rest = cl_cdr(cl_cdr(form));
    CL_Obj doc = defvar_docstring(rest);
    CL_Symbol *sym = (CL_Symbol *)CL_OBJ_TO_PTR(name);
    int saved_tail = c->in_tail;

    sym->flags |= CL_SYM_SPECIAL;

    /* GC SAFETY: compile_expr(init-form) can compact under GC stress and
     * relocate the `name` symbol.  cl_compile_env does not re-protect the form,
     * so a bare `name` would go stale and cl_add_constant/cl_emit_const would
     * bake a stale offset into OP_DEFVAR / the return value — marking the WRONG
     * symbol special at FASL load.  Same class as compile_deftype.  `doc` is
     * read from `rest` for the same reason, before the init form compiles;
     * `rest` itself is protected because the init form is read from it only
     * after the BOUNDP test below has compiled (and allocated). */
    CL_GC_PROTECT(name);
    CL_GC_PROTECT(doc);
    CL_GC_PROTECT(rest);
    /* Nothing below is in tail position -- the form returns NAME.  An init
     * form compiled with in_tail set became a tail call, which made the
     * OP_DEFVAR after it dead code: (defun f () (defvar *x* (g))) never
     * bound *X*. */
    c->in_tail = 0;
    if (!CL_NULL_P(rest)) {
        int idx, bound_pos, store_pos;
        /* CLHS DEFVAR: the initial-value form "is evaluated only if name is
         * not already bound".  OP_DEFVAR alone checked boundness only when
         * STORING, so the form's side effects ran on every reload.
         *
         *     (%GLOBALLY-BOUND-P 'name)  JTRUE bound
         *     <init-form>                JMP  store
         *   bound:  NIL
         *   store:  OP_DEFVAR name
         *
         * The bound branch still runs OP_DEFVAR: it proclaims the variable
         * special (a symbol bound by a plain SETQ must become special) and,
         * asking the same global-value question as the test, never stores
         * the placeholder NIL. */
        emit_quoted_call(c, "%GLOBALLY-BOUND-P", 17, name);
        bound_pos = cl_emit_jump(c, OP_JTRUE);
        compile_expr(c, cl_car(rest));
        store_pos = cl_emit_jump(c, OP_JMP);
        cl_patch_jump(c, bound_pos);
        cl_emit(c, OP_NIL);
        cl_patch_jump(c, store_pos);
        idx = cl_add_constant(c, name);
        cl_emit(c, OP_DEFVAR);
        cl_emit_u16(c, (uint16_t)idx);
    } else {
        /* (defvar name) with no init-form: proclaim the variable special but
         * leave it UNBOUND (CLHS).  OP_DEFVAR can't be reused here — it would
         * store a value when the symbol is unbound, binding it to NIL.  Emit a
         * runtime (%MARK-SPECIAL 'name) so the special proclamation also takes
         * effect at LOAD time; the compile-time flag set above is lost when
         * this form is loaded from a precompiled FASL, which left bare
         * (defvar *x*) variables non-special and broke dynamic LET binding of
         * them (e.g. serapeum's DEFPLACE / *PLACE*). */
        emit_mark_call(c, "%MARK-SPECIAL", 13, name);
    }
    cl_emit_const(c, name);
    emit_doc_call(c, name, "VARIABLE", 8, doc);
    c->in_tail = saved_tail;
    CL_GC_UNPROTECT(3);  /* rest, doc, name */
}

void compile_defparameter(CL_Compiler *c, CL_Obj form)
{
    /* (defparameter name init-form) — always sets value, marks special */
    CL_Obj name = cl_car(cl_cdr(form));
    CL_Obj rest = cl_cdr(cl_cdr(form));
    CL_Obj doc = defvar_docstring(rest);
    CL_Symbol *sym = (CL_Symbol *)CL_OBJ_TO_PTR(name);
    int idx;
    int saved_tail = c->in_tail;

    sym->flags |= CL_SYM_SPECIAL;

    /* GC SAFETY: see compile_defvar — protect `name` across compile_expr's
     * compaction so OP_GSTORE/OP_DEFVAR get the live symbol, not a stale one. */
    CL_GC_PROTECT(name);
    CL_GC_PROTECT(doc);
    c->in_tail = 0;            /* see compile_defvar: the store must not be dead */
    if (!CL_NULL_P(rest)) {
        compile_expr(c, cl_car(rest));
        idx = cl_add_constant(c, name);
        /* OP_GSTORE always sets the value (defparameter semantics).
         * Then OP_DEFVAR with NIL marks the symbol special at load time
         * (critical for FASL — compile-time side effect is lost). */
        cl_emit(c, OP_GSTORE);
        cl_emit_u16(c, (uint16_t)idx);
        cl_emit(c, OP_POP);
        cl_emit_const(c, CL_NIL);
        cl_emit(c, OP_DEFVAR);
        cl_emit_u16(c, (uint16_t)idx);
    }
    cl_emit_const(c, name);
    emit_doc_call(c, name, "VARIABLE", 8, doc);
    c->in_tail = saved_tail;
    CL_GC_UNPROTECT(2);  /* doc, name */
}

void compile_defconstant(CL_Compiler *c, CL_Obj form)
{
    /* (defconstant name value) — always sets value, marks constant */
    CL_Obj name = cl_car(cl_cdr(form));
    CL_Obj rest = cl_cdr(cl_cdr(form));
    CL_Obj doc = defvar_docstring(rest);
    CL_Symbol *sym = (CL_Symbol *)CL_OBJ_TO_PTR(name);
    int idx;
    int saved_tail = c->in_tail;

    /* If already constant with a different value, error */
    if ((sym->flags & CL_SYM_CONSTANT) && sym->value != CL_UNBOUND) {
        if (!CL_NULL_P(rest)) {
            /* Compile the value to check, but we'll compare at compile time */
            /* CL spec: redefining with same value is allowed, different value is undefined */
        }
    }

    sym->flags |= CL_SYM_CONSTANT;

    /* GC SAFETY: see compile_defvar — protect `name` across compile_expr. */
    CL_GC_PROTECT(name);
    CL_GC_PROTECT(doc);
    c->in_tail = 0;            /* see compile_defvar: the store must not be dead */
    if (!CL_NULL_P(rest)) {
        compile_expr(c, cl_car(rest));
        idx = cl_add_constant(c, name);
        cl_emit(c, OP_GSTORE);
        cl_emit_u16(c, (uint16_t)idx);
        cl_emit(c, OP_POP);
    }
    /* Runtime: mark the symbol constant so CONSTANTP works even when this
     * defconstant is loaded from a precompiled FASL (where the compile-time
     * flag set above does not survive into the loading session). */
    emit_mark_call(c, "%MARK-CONSTANT", 14, name);
    cl_emit_const(c, name);
    emit_doc_call(c, name, "VARIABLE", 8, doc);
    c->in_tail = saved_tail;
    CL_GC_UNPROTECT(2);  /* doc, name */
}

/* --- named-lambda --- */

void compile_named_lambda(CL_Compiler *c, CL_Obj form)
{
    /* (named-lambda name lambda-list &body body) → named closure */
    CL_Obj rest = cl_cdr(cl_cdr(form));
    CL_Obj lambda_form, name;
    /* GC SAFETY: the cl_cons can compact — derive name from the protected
     * form only afterwards, else pending_lambda_name gets a stale symbol. */
    CL_GC_PROTECT(form);
    lambda_form = cl_cons(SYM_LAMBDA, rest);
    name = cl_car(cl_cdr(form));
    CL_GC_UNPROTECT(1);
    CL_GC_PROTECT(lambda_form);
    /* Only a symbol becomes a name: bc->name is read back as a CL_Symbol by
     * the printer's closure-printing cases, so a name of any other shape
     * (a raw (setf foo) cons, a fixnum, a string) would type-confuse that
     * read.  Anything else compiles anonymous, the same fallback
     * compile_flet / compile_labels use for their local function names. */
    pending_lambda_name = CL_SYMBOL_P(name) ? name : CL_NIL;
    compile_expr(c, lambda_form);
    CL_GC_UNPROTECT(1);
}

/* --- Defun / Defmacro --- */

/* Emit inline load-time init code for a LOAD-TIME-VALUE memo cell.
 * Called from compile_defun after the lambda is compiled, using the OUTER
 * compiler c.  Produces:
 *   (progn (rplacd (quote cell) (funcall (quote thunk)))
 *          (rplaca (quote cell) t))
 * followed by OP_POP to discard the init result.
 *
 * Because this compiles with the outer compiler c (not a fresh inner
 * compiler), cell and thunk become constants of the defun's FASL unit.
 * FASL deduplication then ensures the cell in the init code and the cell
 * in the lambda body (another constant of the same unit) are the same
 * deserialized object — satisfying CLHS load-time evaluation. */
static void compile_defun_emit_ltv_init(CL_Compiler *c, CL_Obj cell, CL_Obj thunk)
{
    CL_Obj quoted_cell, quoted_thunk, funcall_thunk;
    CL_Obj rplacd_init, rplaca_init, init_form;
    CL_Obj tmp;

    CL_GC_PROTECT(cell);                   /*  1 */
    CL_GC_PROTECT(thunk);                  /*  2 */

    /* (QUOTE cell) */
    tmp = cl_cons(cell, CL_NIL);
    quoted_cell = cl_cons(SYM_QUOTE, tmp);
    CL_GC_PROTECT(quoted_cell);            /*  3 */

    /* (QUOTE thunk) */
    tmp = cl_cons(thunk, CL_NIL);
    quoted_thunk = cl_cons(SYM_QUOTE, tmp);
    CL_GC_PROTECT(quoted_thunk);           /*  4 */

    /* (FUNCALL (QUOTE thunk)) */
    tmp = cl_cons(quoted_thunk, CL_NIL);
    funcall_thunk = cl_cons(SYM_FUNCALL, tmp);
    CL_GC_PROTECT(funcall_thunk);          /*  5 */

    /* (RPLACD (QUOTE cell) (FUNCALL (QUOTE thunk))) -- the operator is
     * interned before, not inside, the list call (mem.h, cl_list2) */
    tmp = cl_intern_in("RPLACD", 6, cl_package_cl);
    rplacd_init = cl_list3(tmp, quoted_cell, funcall_thunk);
    CL_GC_PROTECT(rplacd_init);            /*  6 */

    /* (RPLACA (QUOTE cell) T) */
    tmp = cl_intern_in("RPLACA", 6, cl_package_cl);
    rplaca_init = cl_list3(tmp, quoted_cell, SYM_T);
    CL_GC_PROTECT(rplaca_init);            /*  7 */

    /* (PROGN (RPLACD ...) (RPLACA ...)) */
    tmp = cl_intern_in("PROGN", 5, cl_package_cl);
    init_form = cl_list3(tmp, rplacd_init, rplaca_init);
    CL_GC_PROTECT(init_form);              /*  8 */

    compile_expr(c, init_form);
    cl_emit(c, OP_POP);  /* discard (rplaca cell t) result */

    CL_GC_UNPROTECT(8);
}

void compile_defun(CL_Compiler *c, CL_Obj form)
{
    CL_Obj name = cl_car(cl_cdr(form));
    CL_Obj lambda_list = cl_car(cl_cdr(cl_cdr(form)));
    CL_Obj body = cl_cdr(cl_cdr(cl_cdr(form)));
    CL_Obj block_body, lambda_form;
    CL_Obj real_name = name;       /* symbol for block/return */
    CL_Obj store_sym = name;       /* symbol for OP_FSTORE */
    int is_setf_fn = 0;
    int ltv_base, ltv_i;
    /* Read before the first allocation and protected underneath everything
     * else: `form` is a by-value copy the compactor does not forward, so
     * neither it nor `body` can be re-read once compile_expr has run. */
    CL_Obj doc_name = name;
    CL_Obj doc = body_docstring(body);

    CL_GC_PROTECT(doc_name);
    CL_GC_PROTECT(doc);
    CL_GC_PROTECT(name);    /* protect name across all allocating calls below */
    /* lambda_list and body are sub-structures of `form`; they are read here
     * but consed into lambda_form/block_body only AFTER the cl_cons calls
     * below.  Those conses allocate and (under a compacting GC) relocate the
     * arena-resident param/body lists out from under these unprotected C
     * locals — leaving lambda_form with a stale offset for its params, so
     * parse_lambda_list later miscounts (e.g. `(n)` read as 2 required).
     * Protect them so the compactor forwards the locals. */
    CL_GC_PROTECT(lambda_list);
    CL_GC_PROTECT(body);

    /* Handle (defun (setf name) ...) — setf function */
    if (CL_CONS_P(name) && cl_car(name) == SYM_SETF && CL_CONS_P(cl_cdr(name))) {
        CL_Obj accessor = cl_car(cl_cdr(name));
        is_setf_fn = 1;
        /* Protect accessor across the allocating cl_setf_store_symbol / cl_cons
         * below — those can compact and otherwise leave this bare local (and
         * real_name, which aliases it) holding a stale offset. */
        CL_GC_PROTECT(accessor);

        /* Hidden symbol %SETF-<package>::<name> for storing the function.
         * Interned in AMILISP, package-qualified so same-named accessors in
         * different packages don't collide; matches compile_setf_place's
         * late-binding path (both go through cl_setf_store_symbol). */
        store_sym = cl_setf_store_symbol(accessor);

        /* Register in setf_fn_table: (accessor . store_sym).
         * Protect store_sym so the compactor forwards this caller-level local;
         * cl_register_setf_function internally forwards its own parameter
         * copies but cannot update the variable here. */
        CL_GC_PROTECT(store_sym);
        cl_register_setf_function(accessor, store_sym);
        CL_GC_UNPROTECT(1);  /* store_sym — re-protected unconditionally below */
        real_name = accessor;  /* block name = accessor name */
        CL_GC_UNPROTECT(1);    /* accessor */
    }

    /* Protect real_name and store_sym after the setf block has set their
     * final values; the setf branch's cl_intern_in/cl_cons calls above may
     * have triggered compaction, making their pre-block values stale. */
    CL_GC_PROTECT(real_name);
    CL_GC_PROTECT(store_sym);

    /* CL spec: defun wraps body in (block name ...) */
    block_body = cl_list_star3(SYM_BLOCK, real_name, body);
    CL_GC_PROTECT(block_body);

    /* Build (lambda (params) (block name body...)).  Not as nested cl_cons
     * calls: SYM_LAMBDA and lambda_list would be read before the inner
     * conses allocate, and a collection there left this form with a stale
     * LAMBDA -- the whole DEFUN then compiled as a function call, its
     * parameters as global variables (mem.h, cl_list2). */
    lambda_form = cl_list3(SYM_LAMBDA, lambda_list, block_body);
    CL_GC_PROTECT(lambda_form);

    ltv_base = cl_ltv_init_count;
    pending_lambda_name = real_name;
    compile_expr(c, lambda_form);

    CL_GC_UNPROTECT(7); /* lambda_form, block_body, store_sym, real_name, body, lambda_list, name */

    /* Emit inline load-time init code for any LOAD-TIME-VALUE forms
     * accumulated while compiling the lambda body.  Using the outer
     * compiler c keeps cell/thunk in the same FASL unit as the lambda
     * so FASL dedup makes them the same deserialized objects (CLHS 3.2.4.4). */
    if (cl_compiling_to_file) {
        for (ltv_i = ltv_base; ltv_i < cl_ltv_init_count; ltv_i++)
            compile_defun_emit_ltv_init(c,
                                        CT->ltv_init_cells[ltv_i],
                                        CT->ltv_init_thunks[ltv_i]);
        cl_ltv_init_count = ltv_base;
    }

    /* Store as function binding */
    {
        int idx = cl_add_constant(c, store_sym);
        /* OP_FSTORE peeks top without popping — stores to function slot */
        cl_emit(c, OP_FSTORE);
        cl_emit_u16(c, (uint16_t)idx);
    }

    /* Return the name (replace closure on stack) */
    cl_emit(c, OP_POP);
    cl_emit_const(c, is_setf_fn ? name : real_name);

    /* Docstring, keyed on the name as written -- (SETF FOO) included, so
     * (DOCUMENTATION '(SETF FOO) 'FUNCTION) finds it. */
    emit_doc_call(c, doc_name, "FUNCTION", 8, doc);
    CL_GC_UNPROTECT(2);  /* doc, doc_name */
}

/* Generate a unique uninterned symbol for defmacro destructuring */
CL_Obj defmacro_gensym(void)
{
    static volatile uint32_t counter = 0;
    char buf[32];
    CL_Obj name_str;
    snprintf(buf, sizeof(buf), "%%DMAC%lu",
             (unsigned long)(platform_atomic_inc(&counter) - 1));
    name_str = cl_make_string(buf, (uint32_t)strlen(buf));
    return cl_make_symbol(name_str);
}

/* Check if param is a lambda list keyword (&optional, &key, etc.) */
int defmacro_is_ll_keyword(CL_Obj param)
{
    return CL_SYMBOL_P(param) &&
           (param == SYM_AMP_OPTIONAL || param == SYM_AMP_KEY ||
            param == SYM_AMP_REST || param == SYM_AMP_BODY ||
            param == SYM_AMP_ALLOW_OTHER_KEYS ||
            param == SYM_AMP_ENVIRONMENT || param == SYM_AMP_WHOLE);
}

/* Check if a lambda list needs destructuring transformation.
 * Returns 1 if any required parameter is a list (not a symbol),
 * or if &body/&rest is followed by a list pattern.
 * List params after &optional/&key are (name default) specs, not destructuring. */
int defmacro_needs_destructuring(CL_Obj ll)
{
    int in_optional_or_key = 0;

    while (!CL_NULL_P(ll)) {
        CL_Obj param = cl_car(ll);

        if (CL_SYMBOL_P(param) &&
            (param == SYM_AMP_OPTIONAL || param == SYM_AMP_KEY)) {
            in_optional_or_key = 1;
            ll = cl_cdr(ll);
            continue;
        }
        if (CL_SYMBOL_P(param) &&
            (param == SYM_AMP_BODY || param == SYM_AMP_REST)) {
            in_optional_or_key = 0; /* back to positional mode */
            /* Check if next param is a list pattern */
            {
                CL_Obj next = cl_cdr(ll);
                if (!CL_NULL_P(next) && CL_CONS_P(cl_car(next)))
                    return 1;
            }
            ll = cl_cdr(ll);
            continue;
        }
        if (defmacro_is_ll_keyword(param)) {
            ll = cl_cdr(ll);
            continue;
        }

        /* Required parameter (before any keyword) */
        if (!in_optional_or_key && CL_CONS_P(param))
            return 1;

        ll = cl_cdr(ll);
    }
    return 0;
}

void compile_defmacro(CL_Compiler *c, CL_Obj form)
{
    /* Compile the expander as a lambda, then emit OP_DEFMACRO */
    CL_Obj name = cl_car(cl_cdr(form));
    CL_Obj lambda_list = cl_car(cl_cdr(cl_cdr(form)));
    CL_Obj body = cl_cdr(cl_cdr(cl_cdr(form)));
    int idx;
    CL_Obj lambda_form;
    /* Kept underneath the working set (see the UNPROTECT counts below):
     * the docstring and name for the load-time DOCUMENTATION record, and
     * the lambda list as the user wrote it, for EXT:FUNCTION-ARGLIST --
     * assigned once &environment has been stripped, before the rewrites
     * that turn it into the expander's internal parameter list. */
    CL_Obj doc = body_docstring(body);
    CL_Obj doc_name = name;
    CL_Obj display_ll = CL_NIL;

    CL_GC_PROTECT(doc);
    CL_GC_PROTECT(doc_name);
    CL_GC_PROTECT(display_ll);
    CL_GC_PROTECT(name);
    CL_GC_PROTECT(lambda_list);
    CL_GC_PROTECT(body);

    /* Strip &environment var from lambda list and bind it to the current
     * lexical env (captured by the caller, read via %MACROEXPAND-ENV)
     * at the head of the expander body.  Macro expanders are called
     * with (whole-form arg1 arg2 ...) — the env is threaded through a
     * thread-local rather than the arg list, so the user's &environment
     * parameter is realized here as a plain LET binding. */
    {
        CL_Obj cur = lambda_list;
        CL_Obj prev = CL_NIL;
        while (!CL_NULL_P(cur)) {
            CL_Obj param = cl_car(cur);
            if (param == SYM_AMP_ENVIRONMENT) {
                CL_Obj next = cl_cdr(cur);
                CL_Obj env_var = CL_NULL_P(next) ? CL_NIL : cl_car(next);
                CL_Obj after = CL_NULL_P(next) ? CL_NIL : cl_cdr(next);
                /* Remove &environment and its var from the lambda list */
                if (CL_NULL_P(prev))
                    lambda_list = after;
                else
                    ((CL_Cons *)CL_OBJ_TO_PTR(prev))->cdr = after;
                /* Wrap body: (let ((env-var (AMILISP::%MACROEXPAND-ENV))) ...body)
                 * GC SAFETY: sequenced conses with env_var protected — the
                 * intern/cons calls can compact and env_var is re-read across
                 * them (body is protected at function entry). */
                if (CL_SYMBOL_P(env_var)) {
                    CL_Obj capture_sym, capture_call, binding, bindings, let_form;
                    CL_GC_PROTECT(env_var);
                    capture_sym = cl_intern_in("%MACROEXPAND-ENV", 16, cl_package_amilisp);
                    capture_call = cl_cons(capture_sym, CL_NIL);
                    binding = cl_cons(capture_call, CL_NIL);
                    binding = cl_cons(env_var, binding);
                    bindings = cl_cons(binding, CL_NIL);
                    let_form = cl_cons(bindings, body);
                    let_form = cl_cons(SYM_LET, let_form);
                    body = cl_cons(let_form, CL_NIL);
                    CL_GC_UNPROTECT(1);
                }
                break;
            }
            prev = cur;
            cur = cl_cdr(cur);
        }
    }
    display_ll = lambda_list;

    /* Handle &whole: strip it from the lambda list and use its variable
     * as the first parameter (receives the whole form from expander).
     * If no &whole, add a hidden gensym to receive (and ignore) the form.
     * Macro expanders are always called with (whole-form arg1 arg2 ...). */
    {
        CL_Obj whole_var = CL_NIL;
        if (!CL_NULL_P(lambda_list) && cl_car(lambda_list) == SYM_AMP_WHOLE) {
            /* (&whole var ...) — strip &whole, use var */
            CL_Obj rest = cl_cdr(lambda_list);
            if (!CL_NULL_P(rest)) {
                whole_var = cl_car(rest);
                lambda_list = cl_cdr(rest); /* skip &whole and var */
            }
        }
        if (CL_NULL_P(whole_var)) {
            /* No &whole — generate hidden gensym to receive the form arg */
            whole_var = defmacro_gensym();
        }
        /* Prepend whole_var as the first parameter */
        lambda_list = cl_cons(whole_var, lambda_list);
    }

    /* Transform destructuring lambda lists:
     * Replace list parameters with gensyms and wrap body in
     * destructuring-bind forms. */
    if (defmacro_needs_destructuring(lambda_list)) {
        CL_Obj new_ll = CL_NIL, new_ll_tail = CL_NIL;
        CL_Obj cur = lambda_list;
        int in_opt_key = 0; /* inside &optional or &key section */

        CL_GC_PROTECT(new_ll);
        CL_GC_PROTECT(new_ll_tail);
        /* GC SAFETY: the loop allocates (gensyms + conses) and re-reads the
         * cursor each iteration. */
        CL_GC_PROTECT(cur);

        while (!CL_NULL_P(cur)) {
            CL_Obj param = cl_car(cur);
            CL_Obj next_param;
            CL_Obj cell;

            /* Track lambda list keyword sections */
            if (CL_SYMBOL_P(param) &&
                (param == SYM_AMP_OPTIONAL || param == SYM_AMP_KEY)) {
                in_opt_key = 1;
                cell = cl_cons(param, CL_NIL);
                if (CL_NULL_P(new_ll)) { new_ll = cell; }
                else { ((CL_Cons *)CL_OBJ_TO_PTR(new_ll_tail))->cdr = cell; }
                new_ll_tail = cell;
                cur = cl_cdr(cur);
                continue;
            }

            if (!in_opt_key && CL_CONS_P(param)) {
                /* Required list parameter — replace with gensym + destructuring-bind */
                CL_Obj gs = defmacro_gensym();
                CL_Obj db_form;
                CL_GC_PROTECT(gs);

                /* Wrap body: (destructuring-bind pattern gensym ...body).
                 * GC SAFETY: sequenced conses; param re-derived from the
                 * protected cursor after the gensym/cons allocations. */
                db_form = cl_cons(gs, body);
                param = cl_car(cur);
                db_form = cl_cons(param, db_form);
                db_form = cl_cons(SYM_DESTRUCTURING_BIND, db_form);
                body = cl_cons(db_form, CL_NIL);

                /* Add gensym to new lambda list */
                cell = cl_cons(gs, CL_NIL);
                if (CL_NULL_P(new_ll)) { new_ll = cell; }
                else { ((CL_Cons *)CL_OBJ_TO_PTR(new_ll_tail))->cdr = cell; }
                new_ll_tail = cell;
                CL_GC_UNPROTECT(1); /* gs */
            } else if (CL_SYMBOL_P(param) &&
                       (param == SYM_AMP_BODY || param == SYM_AMP_REST)) {
                in_opt_key = 0; /* back to positional */
                /* &body/&rest — check if next is a list pattern */
                next_param = CL_NULL_P(cl_cdr(cur)) ? CL_NIL : cl_car(cl_cdr(cur));
                if (CL_CONS_P(next_param)) {
                    /* Replace with &body/&rest gensym + destructuring-bind */
                    CL_Obj gs = defmacro_gensym();
                    CL_Obj db_form;
                    CL_GC_PROTECT(gs);

                    /* GC SAFETY: sequenced conses; next_param and param
                     * re-derived from the protected cursor after the
                     * gensym/cons allocations. */
                    db_form = cl_cons(gs, body);
                    next_param = cl_car(cl_cdr(cur));
                    db_form = cl_cons(next_param, db_form);
                    db_form = cl_cons(SYM_DESTRUCTURING_BIND, db_form);
                    body = cl_cons(db_form, CL_NIL);

                    /* Add &body/&rest gensym to new lambda list */
                    param = cl_car(cur);
                    cell = cl_cons(param, CL_NIL);
                    if (CL_NULL_P(new_ll)) { new_ll = cell; }
                    else { ((CL_Cons *)CL_OBJ_TO_PTR(new_ll_tail))->cdr = cell; }
                    new_ll_tail = cell;

                    cell = cl_cons(gs, CL_NIL);
                    ((CL_Cons *)CL_OBJ_TO_PTR(new_ll_tail))->cdr = cell;
                    new_ll_tail = cell;

                    cur = cl_cdr(cur); /* skip the list pattern */
                    CL_GC_UNPROTECT(1); /* gs */
                } else {
                    /* Normal &body/&rest with symbol — just copy */
                    cell = cl_cons(param, CL_NIL);
                    if (CL_NULL_P(new_ll)) { new_ll = cell; }
                    else { ((CL_Cons *)CL_OBJ_TO_PTR(new_ll_tail))->cdr = cell; }
                    new_ll_tail = cell;
                }
            } else {
                /* Normal symbol parameter or &optional/&key spec — just copy */
                cell = cl_cons(param, CL_NIL);
                if (CL_NULL_P(new_ll)) { new_ll = cell; }
                else { ((CL_Cons *)CL_OBJ_TO_PTR(new_ll_tail))->cdr = cell; }
                new_ll_tail = cell;
            }
            cur = cl_cdr(cur);
        }

        lambda_list = new_ll;
        CL_GC_UNPROTECT(3); /* new_ll, new_ll_tail, cur */
    }

    /* CL spec: a macro's body is enclosed in an implicit BLOCK named
       after the macro.  Build (lambda (ll) (block name body...)).
       Without this, (return-from <macro-name> ...) inside the body
       errors with "no block named <macro-name>". */
    {
        CL_Obj block_body = cl_list_star3(SYM_BLOCK, name, body);
        CL_GC_PROTECT(block_body);
        lambda_form = cl_list3(SYM_LAMBDA, lambda_list, block_body);
        CL_GC_UNPROTECT(1);
    }
    CL_GC_PROTECT(lambda_form);

    /* Wrap the inner expander so (macro-function 'foo) returns a
     * function with the CLHS-mandated 2-arg (form environment) shape.
     * Build:
     *   (let ((#:inner <lambda_form>))
     *     (lambda (#:form #:env)
     *       (amilisp::%call-macro-expander #:inner #:form #:env)))
     * The wrapper's 2-required-arg signature makes 0/1/3+ arg calls
     * to (macro-function ...) signal PROGRAM-ERROR (via CL_ERR_ARGS),
     * matching CLHS for PUSH.ERROR.1 / PUSHNEW.ERROR.4 / REMF.ERROR.1. */
    {
        CL_Obj inner_gs = defmacro_gensym();
        CL_Obj form_gs, env_gs, cme_sym;
        CL_Obj inner_call, outer_ll, outer_lambda;
        CL_Obj let_binding, let_bindings, wrap_form;
        CL_GC_PROTECT(inner_gs);
        form_gs = defmacro_gensym();
        CL_GC_PROTECT(form_gs);
        env_gs = defmacro_gensym();
        CL_GC_PROTECT(env_gs);
        cme_sym = cl_intern_in("%CALL-MACRO-EXPANDER", 20, cl_package_amilisp);
        /* (amilisp::%call-macro-expander #:inner #:form #:env) */
        inner_call = cl_cons(env_gs, CL_NIL);
        inner_call = cl_cons(form_gs, inner_call);
        inner_call = cl_cons(inner_gs, inner_call);
        inner_call = cl_cons(cme_sym, inner_call);
        CL_GC_PROTECT(inner_call);
        /* (lambda (#:form #:env) <inner_call>) */
        outer_ll = cl_cons(env_gs, CL_NIL);
        outer_ll = cl_cons(form_gs, outer_ll);
        CL_GC_PROTECT(outer_ll);
        outer_lambda = cl_cons(inner_call, CL_NIL);
        outer_lambda = cl_cons(outer_ll, outer_lambda);
        outer_lambda = cl_cons(SYM_LAMBDA, outer_lambda);
        CL_GC_PROTECT(outer_lambda);
        /* (let ((#:inner <lambda_form>)) <outer_lambda>) */
        let_binding = cl_cons(lambda_form, CL_NIL);
        let_binding = cl_cons(inner_gs, let_binding);
        CL_GC_PROTECT(let_binding);
        let_bindings = cl_cons(let_binding, CL_NIL);
        CL_GC_PROTECT(let_bindings);
        wrap_form = cl_cons(outer_lambda, CL_NIL);
        wrap_form = cl_cons(let_bindings, wrap_form);
        wrap_form = cl_cons(SYM_LET, wrap_form);
        CL_GC_PROTECT(wrap_form);

        compile_expr(c, wrap_form);

        /* The wrapper is what (MACRO-FUNCTION 'name) returns, so it is the
         * object EXT:FUNCTION-ARGLIST (and DESCRIBE, and an editor's
         * arglist display) asks about -- and its own lambda list is the
         * meaningless (#:form #:env).  Its template is the constant
         * compile_lambda added last to `c`; hand it the lambda list as
         * written instead.  Identity-checked rather than assumed: the
         * template's captured list must start with our form gensym.
         * bc is heap-resident; there is no source-level write barrier
         * (specs/generational-gc.md, dirty-page tracking). */
        if (c->const_count > 0) {
            CL_Obj tmpl = c->constants[c->const_count - 1];
            if (CL_BYTECODE_P(tmpl)) {
                CL_Bytecode *bc = (CL_Bytecode *)CL_OBJ_TO_PTR(tmpl);
                if (CL_CONS_P(bc->source_lambda_list) &&
                    cl_car(bc->source_lambda_list) == form_gs)
                    bc->source_lambda_list = display_ll;
            }
        }

        CL_GC_UNPROTECT(9); /* wrap_form, let_bindings, let_binding,
                               outer_lambda, outer_ll, inner_call,
                               env_gs, form_gs, inner_gs */
    }

    CL_GC_UNPROTECT(4); /* lambda_form, body, lambda_list, name */

    /* OP_DEFMACRO pops the wrapper closure, registers macro, pushes name */
    idx = cl_add_constant(c, name);
    cl_emit(c, OP_DEFMACRO);
    cl_emit_u16(c, (uint16_t)idx);

    /* CLHS: a macro's docstring lives under the FUNCTION doc-type. */
    emit_doc_call(c, doc_name, "FUNCTION", 8, doc);
    CL_GC_UNPROTECT(3); /* display_ll, doc_name, doc */
}

/* --- Declaration processing --- */

/* Process a single declaration specifier, applying global side effects.
 * Used by declaim, proclaim, and declare (for special).
 *
 * `proclaimed` (see compiler.h): for (optimize ...), 1 also writes the
 * global baseline cl_optimize_global (under cl_tables_wrlock — it is
 * shared across threads); 0 (body declare) writes only C's effective
 * optimize_settings, which the enclosing body's postlude restores when the
 * body's scope ends.  C's optimize_settings is written unlocked either
 * way: it is exclusively owned by the thread running this compile. */
void cl_process_declaration_specifier(CL_Compiler *c, CL_Obj spec, int proclaimed)
{
    CL_Obj head;

    if (!CL_CONS_P(spec)) return;
    head = cl_car(spec);

    if (head == SYM_SPECIAL_DECL) {
        /* (special var...) — mark each symbol as globally special */
        CL_Obj vars = cl_cdr(spec);
        while (!CL_NULL_P(vars)) {
            CL_Obj var = cl_car(vars);
            if (CL_SYMBOL_P(var)) {
                CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(var);
                s->flags |= CL_SYM_SPECIAL;
            }
            vars = cl_cdr(vars);
        }
    } else if (head == SYM_INLINE_DECL) {
        /* (inline fn...) — set inline flag */
        CL_Obj fns = cl_cdr(spec);
        while (!CL_NULL_P(fns)) {
            CL_Obj fn = cl_car(fns);
            if (CL_SYMBOL_P(fn)) {
                CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(fn);
                s->flags |= CL_SYM_INLINE;
            }
            fns = cl_cdr(fns);
        }
    } else if (head == SYM_NOTINLINE_DECL) {
        /* (notinline fn...) — clear inline flag */
        CL_Obj fns = cl_cdr(spec);
        while (!CL_NULL_P(fns)) {
            CL_Obj fn = cl_car(fns);
            if (CL_SYMBOL_P(fn)) {
                CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(fn);
                s->flags &= ~CL_SYM_INLINE;
            }
            fns = cl_cdr(fns);
        }
    } else if (head == SYM_OPTIMIZE_DECL) {
        /* (optimize (speed 3) (safety 1) ...) or (optimize speed) */
        CL_Obj quals = cl_cdr(spec);
        while (!CL_NULL_P(quals)) {
            CL_Obj q = cl_car(quals);
            CL_Obj name;
            int val = 3; /* bare name means 3 per CL spec */
            if (CL_CONS_P(q)) {
                name = cl_car(q);
                if (!CL_NULL_P(cl_cdr(q)) && CL_FIXNUM_P(cl_car(cl_cdr(q))))
                    val = CL_FIXNUM_VAL(cl_car(cl_cdr(q)));
            } else {
                name = q;
            }
            if (val < 0) val = 0;
            if (val > 3) val = 3;
            /* C's optimize_settings is exclusively owned by this thread's
             * compile chain — no lock needed (see the function comment). */
            if (c) {
                if (name == SYM_SPEED) {
                    extern int cl_optimize_force_speed;
                    if (cl_optimize_force_speed >= 0)
                        val = cl_optimize_force_speed;
                    c->optimize_settings.speed = (uint8_t)val;
                    /* High-water mark for the peephole post-pass: body
                     * declares are scope-restored before finalization, so
                     * the pass gates on the maximum ever in effect. */
                    if ((uint8_t)val > c->peep_speed_max)
                        c->peep_speed_max = (uint8_t)val;
                }
                else if (name == SYM_SAFETY) c->optimize_settings.safety = (uint8_t)val;
                else if (name == SYM_DEBUG) c->optimize_settings.debug = (uint8_t)val;
                else if (name == SYM_SPACE) c->optimize_settings.space = (uint8_t)val;
            }
            if (proclaimed) {
                /* cl_optimize_global is process-wide: lock it. */
                cl_tables_wrlock();
                if (name == SYM_SPEED) cl_optimize_global.speed = (uint8_t)val;
                else if (name == SYM_SAFETY) cl_optimize_global.safety = (uint8_t)val;
                else if (name == SYM_DEBUG) cl_optimize_global.debug = (uint8_t)val;
                else if (name == SYM_SPACE) cl_optimize_global.space = (uint8_t)val;
                cl_tables_rwunlock();
            }
            quals = cl_cdr(quals);
        }
    }
    /* type, ftype, ignore, ignorable, dynamic-extent — no-op for now */
}

/* Scan body for (declare (special ...)) forms and return a list of
 * locally-special variable symbols. Does NOT consume the forms. */
CL_Obj scan_local_specials(CL_Obj body)
{
    CL_Obj result = CL_NIL;
    CL_Obj forms = body;

    CL_GC_PROTECT(result);
    while (!CL_NULL_P(forms)) {
        CL_Obj form = cl_car(forms);

        /* Stop at first non-declare form */
        if (!CL_CONS_P(form) || cl_car(form) != SYM_DECLARE)
            break;

        /* Walk each specifier in (declare spec1 spec2 ...) */
        {
            CL_Obj specs = cl_cdr(form);
            while (!CL_NULL_P(specs)) {
                CL_Obj spec = cl_car(specs);
                if (CL_CONS_P(spec) && cl_car(spec) == SYM_SPECIAL_DECL) {
                    CL_Obj vars = cl_cdr(spec);
                    while (!CL_NULL_P(vars)) {
                        result = cl_cons(cl_car(vars), result);
                        vars = cl_cdr(vars);
                    }
                }
                specs = cl_cdr(specs);
            }
        }
        forms = cl_cdr(forms);
    }
    CL_GC_UNPROTECT(1);
    return result;
}

/* Check if a variable is in the locally-special list */
int is_locally_special(CL_Obj var, CL_Obj local_specials)
{
    CL_Obj list = local_specials;
    while (!CL_NULL_P(list)) {
        if (cl_car(list) == var) return 1;
        list = cl_cdr(list);
    }
    return 0;
}

/* Strip leading (declare ...) forms from body, processing their specifiers.
 * Also skips docstrings (string literals among declarations per CL spec).
 * Returns the remaining body forms (first non-declare form onward). */
CL_Obj process_body_declarations(CL_Compiler *c, CL_Obj body)
{
    CL_Obj forms = body;

    while (!CL_NULL_P(forms)) {
        CL_Obj form = cl_car(forms);

        /* Skip docstrings among declarations (string followed by more forms) */
        if (CL_ANY_STRING_P(form) && !CL_NULL_P(cl_cdr(forms))) {
            forms = cl_cdr(forms);
            continue;
        }

        /* Stop at first non-declare form */
        if (!CL_CONS_P(form) || cl_car(form) != SYM_DECLARE)
            break;

        /* Process each specifier.
         * Skip (special ...) — those are scoped declarations handled by
         * scan_local_specials() in compile_let/compile_lambda.
         * Setting the global CL_SYM_SPECIAL flag here would permanently
         * pollute the symbol, making ALL future bindings dynamic even in
         * scopes that didn't declare it special. */
        {
            CL_Obj specs = cl_cdr(form);
            while (!CL_NULL_P(specs)) {
                CL_Obj spec = cl_car(specs);
                if (CL_CONS_P(spec) && cl_car(spec) == SYM_NOTINLINE_DECL) {
                    /* Record lexically-scoped notinline so compile_call can
                     * suppress compiler-macro expansion for these operators
                     * (CLHS 3.2.2.1.3).  Still call the global processor below
                     * for its existing inline-flag side effect. */
                    CL_Obj fns = cl_cdr(spec);
                    while (!CL_NULL_P(fns)) {
                        cl_compiler_push_notinline(c, cl_car(fns));
                        fns = cl_cdr(fns);
                    }
                }
                if (!(CL_CONS_P(spec) && cl_car(spec) == SYM_SPECIAL_DECL))
                    cl_process_declaration_specifier(c, spec, 0);
                specs = cl_cdr(specs);
            }
        }
        forms = cl_cdr(forms);
    }
    return forms;
}

/* --- Declaim --- */

void compile_declaim(CL_Compiler *c, CL_Obj form)
{
    /* (declaim decl-spec ...) — CLHS: behaves like (eval-when
     * (:compile-toplevel :load-toplevel :execute) (proclaim 'spec) ...).
     * Two legs:
     *   1. :compile-toplevel — process each spec globally NOW, so it is in
     *      force for the rest of this compilation unit (and, as permitted
     *      by "it is unspecified whether the compile-time side effects
     *      persist", for the rest of the process).
     *   2. :load-toplevel/:execute — emit a runtime (PROCLAIM 'spec) call
     *      per spec.  Without this leg a declaim compiled into a FASL
     *      evaporated: loading the fasl left the proclaimed baseline
     *      untouched, so a library's (declaim (optimize ...)) or (declaim
     *      (special ...)) silently stopped applying to later compiles
     *      whenever its fasl was cache-warm.
     * PROCLAIM tolerates every spec kind (unknown heads are no-ops in
     * cl_process_declaration_specifier), and re-running the compile-time
     * leg when the form is evaluated directly is idempotent. */
    CL_Obj sym_proclaim;
    int fidx;
    CL_Obj specs;
    CL_GC_PROTECT(form);
    /* PROCLAIM is interned by cl_builtins_init long before any compile, so
     * this is a lookup — but protect FORM first anyway in case it ever
     * allocates. */
    sym_proclaim = cl_intern_in("PROCLAIM", 8, cl_package_cl);
    fidx = cl_add_constant(c, sym_proclaim);
    specs = cl_cdr(form);
    CL_GC_PROTECT(specs);
    while (!CL_NULL_P(specs)) {
        CL_Obj spec = cl_car(specs);
        /* Stash SPEC into the (GC-rooted, forwarded-in-place) constant pool
         * and emit the call BEFORE processing it — processing may allocate,
         * which would leave the raw SPEC local holding a stale offset. */
        cl_emit(c, OP_FLOAD);
        cl_emit_u16(c, (uint16_t)fidx);
        cl_emit_const(c, spec);
        cl_emit_call(c, OP_CALL, 1);
        cl_emit(c, OP_POP);
        cl_process_declaration_specifier(c, spec, 1);
        specs = cl_cdr(specs);
    }
    CL_GC_UNPROTECT(2);  /* form, specs */
    cl_emit(c, OP_NIL);
}

/* --- Locally --- */

CL_Obj compile_locally(CL_Compiler *c, CL_Obj form)
{
    /* (locally (declare ...) body...) — pure body wrapper, no postlude.
     * Push a CL_TAIL_LOCALLY placeholder so the trampoline shape stays
     * consistent (compile_expr's drain treats it as a no-op). */
    CL_Obj body = cl_cdr(form);
    CL_TailFrame *tf = cl_tail_push(c);
    tf->kind = CL_TAIL_LOCALLY;
    /* Save before process_body_declarations (in compile_body_tail) pushes any
     * notinline names, so the LOCALLY postlude can pop them back off. */
    tf->saved_notinline_count = c->notinline_count;
    return compile_body_tail(c, body);
}

/* --- The --- */

/* CLHS 4.2.3: compound FUNCTION and VALUES type specifiers may only be
 * used for declaration, never for discrimination — TYPEP must signal an
 * error on them.  THE therefore must not emit a runtime OP_ASSERT_TYPE
 * for such a spec, nor for a compound spec that contains one (e.g.
 * (or null (function () t))).  Returns 1 if the spec is safe to hand to
 * TYPEP at runtime. */
static int the_spec_checkable_p(CL_Obj spec)
{
    CL_Obj head;
    if (!CL_CONS_P(spec))
        return 1;
    head = cl_car(spec);
    if (head == SYM_FUNCTION ||
        head == cl_intern_in("VALUES", 6, cl_package_cl))
        return 0;
    if (head == SYM_AND || head == SYM_OR ||
        head == cl_intern_in("NOT", 3, cl_package_cl)) {
        CL_Obj rest = cl_cdr(spec);
        while (CL_CONS_P(rest)) {
            if (!the_spec_checkable_p(cl_car(rest)))
                return 0;
            rest = cl_cdr(rest);
        }
    }
    return 1;
}

void compile_the(CL_Compiler *c, CL_Obj form)
{
    /* (the type-spec value-form) */
    CL_Obj args = cl_cdr(form);
    CL_Obj type_spec, value_form;

    if (CL_NULL_P(args) || CL_NULL_P(cl_cdr(args)))
        cl_error(CL_ERR_ARGS, "THE: requires type and value form");

    type_spec = cl_car(args);
    value_form = cl_car(cl_cdr(args));

    /* Declaration-only specs (compound FUNCTION/VALUES, CLHS 4.2.3): skip
     * the assertion entirely, matching the behavior of major
     * implementations.  Per CLHS 5.3.2 "the", checking is permitted but
     * not required, and TYPEP would signal on these specs at runtime. */
    if (!the_spec_checkable_p(type_spec)) {
        compile_expr(c, value_form);
        return;
    }

    if (c->optimize_settings.safety >= 1) {
        /* Disable tail position: ASSERT_TYPE must run after value form */
        int saved_tail = c->in_tail;
        int idx;
        c->in_tail = 0;
        /* GC SAFETY: compile_expr can compact — protect args and re-derive
         * type_spec after, else a stale offset lands in the constant pool. */
        CL_GC_PROTECT(args);
        compile_expr(c, value_form);
        type_spec = cl_car(args);
        CL_GC_UNPROTECT(1);
        c->in_tail = saved_tail;

        idx = cl_add_constant(c, type_spec);
        cl_emit(c, OP_ASSERT_TYPE);
        cl_emit_u16(c, (uint16_t)idx);
    } else {
        /* safety 0: no check, preserve tail position */
        compile_expr(c, value_form);
    }
}

/* --- Trace / Untrace --- */

void compile_trace(CL_Compiler *c, CL_Obj form)
{
    /* (trace) — return list of currently traced functions
     * (trace name1 name2 ...) — trace functions, return list of names */
    CL_Obj args = cl_cdr(form);
    int trace_fn_idx;
    int n = 0;
    CL_Obj a;

    if (CL_NULL_P(args)) {
        /* (trace) with no args: call %TRACED-FUNCTIONS */
        int idx = cl_add_constant(c, cl_intern_in("%TRACED-FUNCTIONS", 17, cl_package_amilisp));
        cl_emit(c, OP_FLOAD);
        cl_emit_u16(c, (uint16_t)idx);
        cl_emit_call(c, OP_CALL, 0);
        return;
    }

    trace_fn_idx = cl_add_constant(c, cl_intern_in("%TRACE-FUNCTION", 15, cl_package_amilisp));
    a = args;
    while (!CL_NULL_P(a)) {
        CL_Obj name = cl_car(a);
        int name_idx = cl_add_constant(c, name);
        cl_emit(c, OP_FLOAD);
        cl_emit_u16(c, (uint16_t)trace_fn_idx);
        cl_emit(c, OP_CONST);
        cl_emit_u16(c, (uint16_t)name_idx);
        cl_emit_call(c, OP_CALL, 1);
        n++;
        a = cl_cdr(a);
    }
    cl_emit(c, OP_LIST);
    cl_emit(c, (uint8_t)n);
}

void compile_untrace(CL_Compiler *c, CL_Obj form)
{
    /* (untrace) — untrace all, return NIL
     * (untrace name1 name2 ...) — untrace named functions, return list */
    CL_Obj args = cl_cdr(form);
    int untrace_fn_idx;
    int n = 0;
    CL_Obj a;

    if (CL_NULL_P(args)) {
        /* (untrace) with no args: call %UNTRACE-ALL */
        int idx = cl_add_constant(c, cl_intern_in("%UNTRACE-ALL", 12, cl_package_amilisp));
        cl_emit(c, OP_FLOAD);
        cl_emit_u16(c, (uint16_t)idx);
        cl_emit_call(c, OP_CALL, 0);
        return;
    }

    untrace_fn_idx = cl_add_constant(c, cl_intern_in("%UNTRACE-FUNCTION", 17, cl_package_amilisp));
    a = args;
    while (!CL_NULL_P(a)) {
        CL_Obj name = cl_car(a);
        int name_idx = cl_add_constant(c, name);
        cl_emit(c, OP_FLOAD);
        cl_emit_u16(c, (uint16_t)untrace_fn_idx);
        cl_emit(c, OP_CONST);
        cl_emit_u16(c, (uint16_t)name_idx);
        cl_emit_call(c, OP_CALL, 1);
        n++;
        a = cl_cdr(a);
    }
    cl_emit(c, OP_LIST);
    cl_emit(c, (uint8_t)n);
}

/* --- Time --- */

void compile_time(CL_Compiler *c, CL_Obj form)
{
    /* (time expr) — evaluate expr, print timing/allocation stats, return result */
    CL_Obj body = cl_cdr(form);
    int saved_tail = c->in_tail;
    int start_time_slot, start_run_slot, start_consed_slot, start_gc_slot, result_slot;
    int get_time_idx, get_run_idx, get_consed_idx, get_gc_idx, report_idx;

    if (CL_NULL_P(body))
        cl_error(CL_ERR_ARGS, "TIME: missing expression");

    c->in_tail = 0;
    start_time_slot = alloc_temp_slot(c->env);
    start_run_slot = alloc_temp_slot(c->env);
    start_consed_slot = alloc_temp_slot(c->env);
    start_gc_slot = alloc_temp_slot(c->env);
    result_slot = alloc_temp_slot(c->env);

    get_time_idx = cl_add_constant(c, cl_intern_in("%GET-INTERNAL-TIME", 18, cl_package_amilisp));
    get_run_idx = cl_add_constant(c, cl_intern_in("%GET-RUN-TIME", 13, cl_package_amilisp));
    get_consed_idx = cl_add_constant(c, cl_intern_in("%GET-BYTES-CONSED", 17, cl_package_amilisp));
    get_gc_idx = cl_add_constant(c, cl_intern_in("%GET-GC-COUNT", 13, cl_package_amilisp));
    report_idx = cl_add_constant(c, cl_intern_in("%TIME-REPORT", 12, cl_package_amilisp));

    /* Capture start time */
    cl_emit(c, OP_FLOAD);
    cl_emit_u16(c, (uint16_t)get_time_idx);
    cl_emit_call(c, OP_CALL, 0);
    cl_emit(c, OP_STORE);
    cl_emit(c, (uint8_t)start_time_slot);
    cl_emit(c, OP_POP);

    /* Capture start run (CPU) time */
    cl_emit(c, OP_FLOAD);
    cl_emit_u16(c, (uint16_t)get_run_idx);
    cl_emit_call(c, OP_CALL, 0);
    cl_emit(c, OP_STORE);
    cl_emit(c, (uint8_t)start_run_slot);
    cl_emit(c, OP_POP);

    /* Capture start bytes consed */
    cl_emit(c, OP_FLOAD);
    cl_emit_u16(c, (uint16_t)get_consed_idx);
    cl_emit_call(c, OP_CALL, 0);
    cl_emit(c, OP_STORE);
    cl_emit(c, (uint8_t)start_consed_slot);
    cl_emit(c, OP_POP);

    /* Capture start GC count */
    cl_emit(c, OP_FLOAD);
    cl_emit_u16(c, (uint16_t)get_gc_idx);
    cl_emit_call(c, OP_CALL, 0);
    cl_emit(c, OP_STORE);
    cl_emit(c, (uint8_t)start_gc_slot);
    cl_emit(c, OP_POP);

    /* Compile body expression (result on stack) */
    compile_expr(c, cl_car(body));

    /* Save body result in temp slot, pop from stack */
    cl_emit(c, OP_STORE);
    cl_emit(c, (uint8_t)result_slot);
    cl_emit(c, OP_POP);

    /* Print timing: FLOAD %TIME-REPORT, LOAD start_time, start_consed,
     * start_gc, start_run, CALL 4 */
    cl_emit(c, OP_FLOAD);
    cl_emit_u16(c, (uint16_t)report_idx);
    cl_emit(c, OP_LOAD);
    cl_emit(c, (uint8_t)start_time_slot);
    cl_emit(c, OP_LOAD);
    cl_emit(c, (uint8_t)start_consed_slot);
    cl_emit(c, OP_LOAD);
    cl_emit(c, (uint8_t)start_gc_slot);
    cl_emit(c, OP_LOAD);
    cl_emit(c, (uint8_t)start_run_slot);
    cl_emit_call(c, OP_CALL, 4);
    cl_emit(c, OP_POP);  /* Discard %TIME-REPORT result (NIL) */

    /* Restore body result */
    cl_emit(c, OP_LOAD);
    cl_emit(c, (uint8_t)result_slot);
    c->in_tail = saved_tail;
}

/* --- in-package --- */

void compile_in_package(CL_Compiler *c, CL_Obj form)
{
    /* (in-package name) where name is a symbol or string */
    CL_Obj name_form = cl_car(cl_cdr(form));
    CL_Obj pkg;
    const char *name_str;
    uint32_t name_len;
    int idx;

    /* Resolve package name at compile time */
    if (CL_SYMBOL_P(name_form)) {
        CL_Symbol *sym = (CL_Symbol *)CL_OBJ_TO_PTR(name_form);
        CL_String *sname = (CL_String *)CL_OBJ_TO_PTR(sym->name);
        name_str = sname->data;
        name_len = sname->length;
    } else if (CL_STRING_P(name_form)) {
        CL_String *s = (CL_String *)CL_OBJ_TO_PTR(name_form);
        name_str = s->data;
        name_len = s->length;
    } else {
        cl_error(CL_ERR_TYPE, "IN-PACKAGE: expected symbol or string");
        return;
    }

    pkg = cl_find_package(name_str, name_len);
    if (!CL_NULL_P(pkg)) {
        /* Fast path: package known at compile time.  Set cl_current_package
         * so subsequent forms in the same compilation unit (e.g. inside a
         * compile-file pass) intern symbols in the new package, then emit
         * a constant store into *PACKAGE*. */
        cl_current_package = pkg;
        cl_emit_const(c, pkg);
        idx = cl_add_constant(c, SYM_STAR_PACKAGE);
        cl_emit(c, OP_GSTORE);
        cl_emit_u16(c, (uint16_t)idx);
        return;
    }

    /* Fallback: package not known at compile time.  This happens when a
     * top-level form bundles `(load <file>)` and `(in-package :PKG)` so that
     * PKG is created by the load only at run time.  Emit a runtime call to
     * amilisp::%set-current-package so the lookup-and-error happens at the
     * point this form is actually executed. */
    {
        CL_Obj name_str_obj = cl_make_string(name_str, name_len);
        CL_Obj setter_sym = cl_intern_in("%SET-CURRENT-PACKAGE", 20,
                                         cl_package_amilisp);
        int setter_idx = cl_add_constant(c, setter_sym);
        int name_idx = cl_add_constant(c, name_str_obj);
        cl_emit(c, OP_FLOAD);
        cl_emit_u16(c, (uint16_t)setter_idx);
        cl_emit(c, OP_CONST);
        cl_emit_u16(c, (uint16_t)name_idx);
        cl_emit_call(c, OP_CALL, 1);
    }
}
