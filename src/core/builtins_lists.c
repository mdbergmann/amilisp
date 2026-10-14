#include "builtins.h"
#include "symbol.h"
#include "package.h"
#include "mem.h"
#include "error.h"
#include "printer.h"
#include "vm.h"
#include "string_utils.h"
#include "../platform/platform.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

/* --- List utility helpers --- */

/* Extract a non-negative integer count argument (BUTLAST/NBUTLAST/LAST style).
 * Declared in builtins.h — NTH and %SETF-NTH in the neighbouring files walk a
 * list by the same rules.
 * - Fixnum non-negative: return its value, clamped to INT32_MAX.
 * - Positive bignum: return INT32_MAX (caller treats as "more than length").
 * - Negative bignum / negative fixnum: signal TYPE-ERROR.
 * - Non-integer: signal TYPE-ERROR. */
int32_t cl_extract_count_or_clamp(CL_Obj n_arg, const char *fn_name)
{
    if (CL_FIXNUM_P(n_arg)) {
        int32_t v = CL_FIXNUM_VAL(n_arg);
        if (v < 0)
            cl_signal_type_error(n_arg, "UNSIGNED-BYTE", fn_name);
        return v;
    }
    if (CL_BIGNUM_P(n_arg)) {
        CL_Bignum *bn = (CL_Bignum *)CL_OBJ_TO_PTR(n_arg);
        if (bn->sign)
            cl_signal_type_error(n_arg, "UNSIGNED-BYTE", fn_name);
        return 0x7FFFFFFF;
    }
    cl_signal_type_error(n_arg, "UNSIGNED-BYTE", fn_name);
    return 0; /* unreachable */
}

/* Call a 2-arg test function (default eql) */
static CL_Obj call_test(CL_Obj test_fn, CL_Obj a, CL_Obj b)
{
    CL_Obj targs[2];
    targs[0] = a;
    targs[1] = b;
    if (CL_FUNCTION_OBJ_P(test_fn))
        /* cl_vm_apply GC-roots targs across the call. */
        return cl_vm_apply(test_fn, targs, 2);
    cl_error(CL_ERR_TYPE, "not a function (test)");
    return CL_NIL;
}

/* Extract :test keyword argument from args[start..n-1], return default (eql) if absent */
static CL_Obj extract_test_arg(CL_Obj *args, int n, int start)
{
    int i;
    CL_Obj kw_test = cl_intern_in("TEST", 4, cl_package_keyword);
    for (i = start; i < n - 1; i += 2) {
        if (args[i] == kw_test)
            return cl_coerce_funcdesig(args[i + 1], ":TEST");
    }
    /* Default: eql */
    {
        CL_Obj eql_sym = cl_intern_in("EQL", 3, cl_package_cl);
        CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(eql_sym);
        return s->function;
    }
}

/* --- List utilities --- */

static CL_Obj bi_nthcdr(CL_Obj *args, int n)
{
    int32_t idx = cl_extract_count_or_clamp(args[0], "NTHCDR");
    CL_Obj list = args[1];
    CL_UNUSED(n);
    while (idx > 0 && !CL_NULL_P(list)) {
        if (!CL_CONS_P(list))
            cl_signal_type_error(list, "LIST", "NTHCDR");
        list = cl_cdr(list);
        idx--;
    }
    return list;
}

static CL_Obj bi_last(CL_Obj *args, int n)
{
    CL_Obj list = args[0];
    int32_t count = 1;
    int32_t len = 0;
    CL_Obj p;

    if (n >= 2)
        count = cl_extract_count_or_clamp(args[1], "LAST");
    if (CL_NULL_P(list)) return CL_NIL;
    if (!CL_CONS_P(list))
        cl_signal_type_error(list, "LIST", "LAST");

    /* Count length */
    for (p = list; CL_CONS_P(p); p = cl_cdr(p))
        len++;

    /* Skip (len - count) conses */
    {
        int32_t skip = (count >= len) ? 0 : len - count;
        while (skip > 0) {
            list = cl_cdr(list);
            skip--;
        }
    }
    return list;
}

static CL_Obj bi_acons(CL_Obj *args, int n)
{
    CL_Obj pair;
    CL_UNUSED(n);
    /* args[] slots are already-rooted VM-stack slots — no CL_GC_PROTECT
     * (double registration would double-forward them on compaction). */
    pair = cl_cons(args[0], args[1]);
    return cl_cons(pair, args[2]);
}

static CL_Obj bi_copy_list(CL_Obj *args, int n)
{
    CL_Obj list = args[0];
    CL_Obj result = CL_NIL, tail = CL_NIL;
    CL_UNUSED(n);

    CL_GC_PROTECT(result);
    CL_GC_PROTECT(tail);
    /* GC SAFETY: cl_cons can compact — the input cursor is re-read after
     * every allocation and must be a forwarded root. */
    CL_GC_PROTECT(list);

    while (CL_CONS_P(list)) {
        CL_Obj cell = cl_cons(cl_car(list), CL_NIL);
        if (CL_NULL_P(result)) {
            result = cell;
        } else {
            ((CL_Cons *)CL_OBJ_TO_PTR(tail))->cdr = cell;
        }
        tail = cell;
        list = cl_cdr(list);
    }
    /* Handle dotted list */
    if (!CL_NULL_P(list) && !CL_NULL_P(tail)) {
        ((CL_Cons *)CL_OBJ_TO_PTR(tail))->cdr = list;
    }

    CL_GC_UNPROTECT(3);
    return result;
}

static CL_Obj bi_pairlis(CL_Obj *args, int n)
{
    CL_Obj keys = args[0], data = args[1];
    CL_Obj result = (n >= 3) ? args[2] : CL_NIL;
    CL_Obj pairs = CL_NIL, ptail = CL_NIL;

    CL_GC_PROTECT(result);
    CL_GC_PROTECT(pairs);
    CL_GC_PROTECT(ptail);
    /* GC SAFETY: both input cursors are re-read after the conses below. */
    CL_GC_PROTECT(keys);
    CL_GC_PROTECT(data);

    while (!CL_NULL_P(keys) && !CL_NULL_P(data)) {
        CL_Obj pair = cl_cons(cl_car(keys), cl_car(data));
        CL_Obj cell = cl_cons(pair, CL_NIL);
        if (CL_NULL_P(pairs)) {
            pairs = cell;
        } else {
            ((CL_Cons *)CL_OBJ_TO_PTR(ptail))->cdr = cell;
        }
        ptail = cell;
        keys = cl_cdr(keys);
        data = cl_cdr(data);
    }

    /* Append existing alist */
    if (!CL_NULL_P(ptail)) {
        ((CL_Cons *)CL_OBJ_TO_PTR(ptail))->cdr = result;
        result = pairs;
    }

    CL_GC_UNPROTECT(5);
    return result;
}

/* --- Functions with :test keyword --- */

static CL_Obj bi_assoc(CL_Obj *args, int n)
{
    CL_Obj key, alist, test_fn;
    CL_Obj pair = CL_NIL;

    /* GC SAFETY: extract_test_arg interns and a Lisp :test can allocate —
     * either can compact.  Read the args AFTER the intern (args[] is a
     * rooted VM-stack slice) and keep the C-local key/cursor/test/pair
     * forwarded across every call_test. */
    test_fn = extract_test_arg(args, n, 2);
    key = args[0];
    alist = args[1];

    CL_GC_PROTECT(key);
    CL_GC_PROTECT(alist);
    CL_GC_PROTECT(test_fn);
    CL_GC_PROTECT(pair);
    while (!CL_NULL_P(alist)) {
        pair = cl_car(alist);
        if (CL_CONS_P(pair)) {
            if (!CL_NULL_P(call_test(test_fn, key, cl_car(pair)))) {
                CL_GC_UNPROTECT(4);
                return pair;
            }
        }
        alist = cl_cdr(alist);
    }
    CL_GC_UNPROTECT(4);
    return CL_NIL;
}

static CL_Obj bi_rassoc(CL_Obj *args, int n)
{
    CL_Obj item, alist, test_fn;
    CL_Obj pair = CL_NIL;

    /* GC SAFETY: same shape as bi_assoc — see the comment there. */
    test_fn = extract_test_arg(args, n, 2);
    item = args[0];
    alist = args[1];

    CL_GC_PROTECT(item);
    CL_GC_PROTECT(alist);
    CL_GC_PROTECT(test_fn);
    CL_GC_PROTECT(pair);
    while (!CL_NULL_P(alist)) {
        pair = cl_car(alist);
        if (CL_CONS_P(pair)) {
            if (!CL_NULL_P(call_test(test_fn, item, cl_cdr(pair)))) {
                CL_GC_UNPROTECT(4);
                return pair;
            }
        }
        alist = cl_cdr(alist);
    }
    CL_GC_UNPROTECT(4);
    return CL_NIL;
}

static CL_Obj bi_getf(CL_Obj *args, int n)
{
    CL_Obj plist = args[0], indicator = args[1];
    CL_Obj def = (n >= 3) ? args[2] : CL_NIL;

    while (!CL_NULL_P(plist)) {
        CL_Obj key = cl_car(plist);
        plist = cl_cdr(plist);
        if (CL_NULL_P(plist)) break;
        if (key == indicator)
            return cl_car(plist);
        plist = cl_cdr(plist);
    }
    return def;
}

/* %SETF-GETF: (setf (getf plist indicator) value)
 * Returns the possibly-modified plist.  If INDICATOR is present,
 * destructively updates its value cell (preserving EQ on PLIST);
 * if absent, conses INDICATOR and VALUE onto the front and returns
 * the new list.  CLHS §5.1.2.4 requires the place (PLIST) to be
 * reassigned — the caller (compile_setf / the getf setf-expander)
 * wraps this call in a SETF back onto the original place so the
 * binding reflects the prepend branch. */
static CL_Obj bi_setf_getf(CL_Obj *args, int n)
{
    CL_Obj plist = args[0], indicator = args[1], value = args[2];
    CL_Obj p = plist;
    CL_UNUSED(n);
    while (!CL_NULL_P(p)) {
        CL_Obj key = cl_car(p);
        CL_Obj val_cell = cl_cdr(p);
        if (CL_NULL_P(val_cell)) break;
        if (key == indicator) {
            /* Found — destructively update the value cell */
            ((CL_Cons *)CL_OBJ_TO_PTR(val_cell))->car = value;
            return plist;
        }
        p = cl_cdr(val_cell);
    }
    /* Not found — prepend (indicator value) to the plist and return
     * the new list head.  GC protect during the two cl_cons calls. */
    CL_GC_PROTECT(plist);
    CL_GC_PROTECT(indicator);
    {
        CL_Obj rest = cl_cons(value, plist);
        CL_Obj head;
        CL_GC_PROTECT(rest);
        head = cl_cons(indicator, rest);
        CL_GC_UNPROTECT(3);
        return head;
    }
}

/* GET-PROPERTIES: (get-properties plist indicator-list)
 * Returns 3 values: indicator, value, tail.
 * Searches plist for any indicator in indicator-list (using EQ).
 * If found, returns the indicator, its value, and the tail of the
 * plist starting at that indicator. If not found, returns NIL NIL NIL. */
static CL_Obj bi_get_properties(CL_Obj *args, int n)
{
    CL_Obj plist = args[0], indicators = args[1];
    CL_UNUSED(n);

    while (!CL_NULL_P(plist)) {
        CL_Obj key = cl_car(plist);
        CL_Obj val_cell = cl_cdr(plist);
        if (CL_NULL_P(val_cell)) break;
        /* Check if key is in indicator-list */
        CL_Obj ind = indicators;
        while (!CL_NULL_P(ind)) {
            if (cl_car(ind) == key) {
                /* Found — return indicator, value, tail */
                cl_mv_values[0] = key;
                cl_mv_values[1] = cl_car(val_cell);
                cl_mv_values[2] = plist;
                cl_mv_count = 3;
                return key;
            }
            ind = cl_cdr(ind);
        }
        plist = cl_cdr(val_cell);
    }
    /* Not found */
    cl_mv_values[0] = CL_NIL;
    cl_mv_values[1] = CL_NIL;
    cl_mv_values[2] = CL_NIL;
    cl_mv_count = 3;
    return CL_NIL;
}

/* For subst, default to EQUAL so that list patterns can be matched.
 * CLHS says EQL, but real-world code (cl-ppcre) relies on matching list
 * patterns via subst, and other implementations coalesce equal constants
 * at compile time making eql work like equal. Using equal as default
 * makes our subst compatible with such code. */
static CL_Obj extract_test_arg_subst(CL_Obj *args, int n, int start)
{
    int i;
    CL_Obj kw_test = cl_intern_in("TEST", 4, cl_package_keyword);
    for (i = start; i < n - 1; i += 2) {
        if (args[i] == kw_test)
            return cl_coerce_funcdesig(args[i + 1], ":TEST");
    }
    /* Default: equal (for compatibility with constant coalescing) */
    {
        CL_Obj equal_sym = cl_intern_in("EQUAL", 5, cl_package_cl);
        CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(equal_sym);
        return s->function;
    }
}

/* Recursive core of SUBST.  Takes the already-resolved test function so
 * the recursion never re-reads an unrooted C sub_args array after an
 * allocating call.  GC SAFETY: every parameter is a by-value C local of
 * THIS frame — the caller's protections don't cover them — so protect
 * them all before call_test/the recursion can compact. */
static CL_Obj subst_rec(CL_Obj new_obj, CL_Obj old_obj, CL_Obj tree,
                        CL_Obj test_fn)
{
    CL_Obj car_r, cdr_r;

    CL_GC_PROTECT(new_obj);
    CL_GC_PROTECT(old_obj);
    CL_GC_PROTECT(tree);
    CL_GC_PROTECT(test_fn);

    if (!CL_NULL_P(call_test(test_fn, old_obj, tree))) {
        CL_GC_UNPROTECT(4);
        return new_obj;
    }
    if (!CL_CONS_P(tree)) {
        CL_GC_UNPROTECT(4);
        return tree;
    }

    car_r = subst_rec(new_obj, old_obj, cl_car(tree), test_fn);
    CL_GC_PROTECT(car_r);
    cdr_r = subst_rec(new_obj, old_obj, cl_cdr(tree), test_fn);
    CL_GC_UNPROTECT(5);

    if (car_r == cl_car(tree) && cdr_r == cl_cdr(tree))
        return tree;
    return cl_cons(car_r, cdr_r);
}

static CL_Obj bi_subst(CL_Obj *args, int n)
{
    /* args[] is a rooted VM-stack slice, so re-reading it after the
     * (potentially allocating) keyword interning in extract_test_arg_subst
     * is safe. */
    CL_Obj test_fn = extract_test_arg_subst(args, n, 3);
    return subst_rec(args[0], args[1], args[2], test_fn);
}

/* Recursive core of SUBLIS — same shape and GC-safety rules as subst_rec. */
static CL_Obj sublis_rec(CL_Obj alist, CL_Obj tree, CL_Obj test_fn)
{
    CL_Obj al, car_r, cdr_r;
    CL_Obj pair = CL_NIL;

    CL_GC_PROTECT(alist);
    CL_GC_PROTECT(tree);
    CL_GC_PROTECT(test_fn);
    CL_GC_PROTECT(pair);
    al = alist;
    CL_GC_PROTECT(al);

    /* Check each alist entry against tree */
    while (!CL_NULL_P(al)) {
        pair = cl_car(al);
        if (CL_CONS_P(pair)) {
            if (!CL_NULL_P(call_test(test_fn, cl_car(pair), tree))) {
                CL_Obj hit = cl_cdr(pair);
                CL_GC_UNPROTECT(5);
                return hit;
            }
        }
        al = cl_cdr(al);
    }

    if (!CL_CONS_P(tree)) {
        CL_GC_UNPROTECT(5);
        return tree;
    }

    car_r = sublis_rec(alist, cl_car(tree), test_fn);
    CL_GC_PROTECT(car_r);
    cdr_r = sublis_rec(alist, cl_cdr(tree), test_fn);
    CL_GC_UNPROTECT(6);

    if (car_r == cl_car(tree) && cdr_r == cl_cdr(tree))
        return tree;
    return cl_cons(car_r, cdr_r);
}

static CL_Obj bi_sublis(CL_Obj *args, int n)
{
    CL_Obj test_fn = extract_test_arg(args, n, 2);
    return sublis_rec(args[0], args[1], test_fn);
}

static CL_Obj bi_adjoin(CL_Obj *args, int n)
{
    CL_Obj item, list, test_fn, l;

    /* GC SAFETY: same shape as bi_assoc — protect the C locals across
     * every (potentially compacting) call_test. */
    test_fn = extract_test_arg(args, n, 2);
    item = args[0];
    list = args[1];

    CL_GC_PROTECT(item);
    CL_GC_PROTECT(list);
    CL_GC_PROTECT(test_fn);
    l = list;
    CL_GC_PROTECT(l);
    for (; !CL_NULL_P(l); l = cl_cdr(l)) {
        if (!CL_NULL_P(call_test(test_fn, item, cl_car(l)))) {
            CL_GC_UNPROTECT(4);
            return list;
        }
    }
    CL_GC_UNPROTECT(4);
    return cl_cons(item, list);
}

/* --- Destructive operations --- */

static CL_Obj bi_nconc(CL_Obj *args, int n)
{
    CL_Obj result = CL_NIL;
    CL_Obj tail = CL_NIL;
    int i;

    for (i = 0; i < n; i++) {
        CL_Obj list = args[i];
        CL_Obj this_tail;
        if (CL_NULL_P(list)) continue;

        /* Find this list's tail BEFORE splicing.  Walking after the splice
         * would loop forever on the (nconc x x) idiom: the splice itself
         * makes the previous tail point back into x, so walking x again
         * traverses an already-circular list.  Each input list is required
         * by HyperSpec to be a proper list (or nil) prior to nconc, so this
         * pre-splice walk is guaranteed to terminate. */
        this_tail = list;
        while (CL_CONS_P(this_tail) && CL_CONS_P(cl_cdr(this_tail)))
            this_tail = cl_cdr(this_tail);

        if (CL_NULL_P(result)) {
            result = list;
        } else {
            ((CL_Cons *)CL_OBJ_TO_PTR(tail))->cdr = list;
        }
        tail = this_tail;
    }
    return result;
}

static CL_Obj bi_nreverse(CL_Obj *args, int n)
{
    CL_Obj seq = args[0];
    CL_UNUSED(n);

    if (CL_NULL_P(seq)) return CL_NIL;

    if (CL_CONS_P(seq)) {
        CL_Obj prev = CL_NIL;
        while (CL_CONS_P(seq)) {
            CL_Obj next = cl_cdr(seq);
            ((CL_Cons *)CL_OBJ_TO_PTR(seq))->cdr = prev;
            prev = seq;
            seq = next;
        }
        return prev;
    }
    if (CL_ANY_STRING_P(seq)) {
        uint32_t slen = cl_string_length(seq);
        uint32_t i, half = slen / 2;
        for (i = 0; i < half; i++) {
            int a = cl_string_char_at(seq, i);
            int b = cl_string_char_at(seq, slen - 1 - i);
            cl_string_set_char_at(seq, i, b);
            cl_string_set_char_at(seq, slen - 1 - i, a);
        }
        return seq;
    }
    if (CL_VECTOR_P(seq)) {
        CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(seq);
        uint32_t alen = cl_vector_active_length(v);
        uint32_t i, half = alen / 2;
        CL_Obj *data = cl_vector_data(v);
        for (i = 0; i < half; i++) {
            CL_Obj tmp = data[i];
            data[i] = data[alen - 1 - i];
            data[alen - 1 - i] = tmp;
        }
        return seq;
    }
    if (CL_BYTE_VECTOR_P(seq)) {
        CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(seq);
        uint32_t blen = cl_bytevec_active_length(bv);  /* honour fill pointer */
        uint32_t i, half = blen / 2;
        for (i = 0; i < half; i++) {
            int32_t tmp = cl_bytevec_get(bv, i);
            cl_bytevec_set(bv, i, cl_bytevec_get(bv, blen - 1 - i));
            cl_bytevec_set(bv, blen - 1 - i, tmp);
        }
        return seq;
    }
    if (CL_BIT_VECTOR_P(seq)) {
        CL_BitVector *bv = (CL_BitVector *)CL_OBJ_TO_PTR(seq);
        uint32_t blen = cl_bv_active_length(bv);   /* honour fill pointer */
        uint32_t i, half = blen / 2;
        for (i = 0; i < half; i++) {
            uint32_t a = cl_bv_get_bit(bv, i);
            uint32_t b = cl_bv_get_bit(bv, blen - 1 - i);
            cl_bv_set_bit(bv, i, b);
            cl_bv_set_bit(bv, blen - 1 - i, a);
        }
        return seq;
    }
    cl_signal_type_error(seq, "SEQUENCE", "NREVERSE");
    return CL_NIL;
}

/* DELETE / DELETE-IF / DELETE-IF-NOT share REMOVE's full behaviour (keyword
 * handling, :count, :start, :end, :from-end, :key, :test/:test-not and
 * argument validation).  CLHS permits the destructive operations to "but need
 * not" modify the argument, so delegating to the non-destructive REMOVE family
 * is conforming and keeps a single source of truth for the semantics. */
static CL_Obj bi_delete(CL_Obj *args, int n)
{
    extern CL_Obj bi_delete_export(CL_Obj *args, int n);
    return bi_delete_export(args, n);
}

static CL_Obj bi_delete_if(CL_Obj *args, int n)
{
    extern CL_Obj bi_delete_if_export(CL_Obj *args, int n);
    return bi_delete_if_export(args, n);
}

/* Recursive core of NSUBST — same shape and GC-safety rules as subst_rec:
 * every parameter is a by-value C local of THIS frame, so each level roots
 * its own copies before call_test/the recursion can compact.  The destructive
 * car/cdr stores re-derive the cons pointer from the rooted `tree` AFTER the
 * recursion (the pre-recursion pointer may be a stale offset). */
static CL_Obj nsubst_rec(CL_Obj new_obj, CL_Obj old_obj, CL_Obj tree,
                         CL_Obj test_fn)
{
    CL_Obj car_r, cdr_r;

    CL_GC_PROTECT(new_obj);
    CL_GC_PROTECT(old_obj);
    CL_GC_PROTECT(tree);
    CL_GC_PROTECT(test_fn);

    if (!CL_NULL_P(call_test(test_fn, old_obj, tree))) {
        CL_GC_UNPROTECT(4);
        return new_obj;
    }
    if (!CL_CONS_P(tree)) {
        CL_GC_UNPROTECT(4);
        return tree;
    }

    car_r = nsubst_rec(new_obj, old_obj, cl_car(tree), test_fn);
    ((CL_Cons *)CL_OBJ_TO_PTR(tree))->car = car_r;
    cdr_r = nsubst_rec(new_obj, old_obj, cl_cdr(tree), test_fn);
    ((CL_Cons *)CL_OBJ_TO_PTR(tree))->cdr = cdr_r;
    CL_GC_UNPROTECT(4);
    return tree;
}

static CL_Obj bi_nsubst(CL_Obj *args, int n)
{
    /* args[] is a rooted VM-stack slice, so re-reading it after the
     * (potentially allocating) keyword handling in extract_test_arg is
     * safe (mirrors bi_subst). */
    CL_Obj test_fn = extract_test_arg(args, n, 3);
    return nsubst_rec(args[0], args[1], args[2], test_fn);
}

/* --- Copy/construction --- */

static CL_Obj bi_butlast(CL_Obj *args, int n)
{
    CL_Obj list = args[0];
    int32_t count = 1;
    int32_t len = 0;
    CL_Obj p, result = CL_NIL, tail = CL_NIL;

    if (n >= 2)
        count = cl_extract_count_or_clamp(args[1], "BUTLAST");
    if (CL_NULL_P(list)) return CL_NIL;
    if (!CL_CONS_P(list))
        cl_signal_type_error(list, "LIST", "BUTLAST");

    for (p = list; CL_CONS_P(p); p = cl_cdr(p))
        len++;

    {
        int32_t keep = len - count;
        if (keep <= 0) return CL_NIL;

        CL_GC_PROTECT(result);
        CL_GC_PROTECT(tail);
        /* GC SAFETY: p is re-read after each cl_cons compaction. */
        CL_GC_PROTECT(p);

        p = list;
        while (keep > 0) {
            CL_Obj cell = cl_cons(cl_car(p), CL_NIL);
            if (CL_NULL_P(result)) {
                result = cell;
            } else {
                ((CL_Cons *)CL_OBJ_TO_PTR(tail))->cdr = cell;
            }
            tail = cell;
            p = cl_cdr(p);
            keep--;
        }

        CL_GC_UNPROTECT(3);
    }
    return result;
}

/* Destructive butlast: mutate the input list so that the last `count` conses
 * are removed, and return the (possibly empty) prefix.  Per CLHS, returns
 * the original list (eq) when at least one cons is kept; nil otherwise. */
static CL_Obj bi_nbutlast(CL_Obj *args, int n)
{
    CL_Obj list = args[0];
    int32_t count = 1;
    int32_t len = 0;
    int32_t keep;
    CL_Obj p;
    int32_t i;

    if (n >= 2)
        count = cl_extract_count_or_clamp(args[1], "NBUTLAST");
    if (CL_NULL_P(list)) return CL_NIL;
    if (!CL_CONS_P(list))
        cl_signal_type_error(list, "LIST", "NBUTLAST");

    for (p = list; CL_CONS_P(p); p = cl_cdr(p))
        len++;

    keep = (count >= len) ? 0 : len - count;
    if (keep <= 0) return CL_NIL;

    /* Walk to the (keep-1)-th cons and set its cdr to nil. */
    p = list;
    for (i = 0; i < keep - 1; i++)
        p = cl_cdr(p);
    ((CL_Cons *)CL_OBJ_TO_PTR(p))->cdr = CL_NIL;
    return list;
}

static CL_Obj bi_copy_tree(CL_Obj *args, int n)
{
    CL_Obj tree = args[0];
    CL_Obj head = CL_NIL;
    CL_Obj tail = CL_NIL;
    CL_UNUSED(n);

    if (!CL_CONS_P(tree))
        return tree;

    /* Walk down the cdr spine iteratively, recursing only for the car.
     * A naive recurse-on-both-axes implementation pushes 2 GC roots per
     * level on the cdr chain — copy-tree on a 500+ element flat list
     * (e.g. ansi-test's *universe*) overflows the 1024-slot root stack
     * and aborts the VM.  CONS_P is checked at the top of the loop so a
     * dotted-tail (improper list) is preserved. */
    CL_GC_PROTECT(tree);
    CL_GC_PROTECT(head);
    CL_GC_PROTECT(tail);
    while (CL_CONS_P(tree)) {
        CL_Obj car_r;
        CL_Obj new_cons;
        {
            CL_Obj sub[1];
            sub[0] = cl_car(tree);
            car_r = bi_copy_tree(sub, 1);
        }
        CL_GC_PROTECT(car_r);
        new_cons = cl_cons(car_r, CL_NIL);
        CL_GC_UNPROTECT(1);
        if (CL_NULL_P(head)) {
            head = new_cons;
        } else {
            ((CL_Cons *)CL_OBJ_TO_PTR(tail))->cdr = new_cons;
        }
        tail = new_cons;
        tree = cl_cdr(tree);
    }
    /* Preserve a non-nil terminator (e.g. dotted pair); for proper
     * lists this is CL_NIL and the loop already stored CL_NIL in
     * tail->cdr via the new_cons constructor. */
    if (!CL_NULL_P(tree))
        ((CL_Cons *)CL_OBJ_TO_PTR(tail))->cdr = tree;
    CL_GC_UNPROTECT(3);
    return head;
}

/* --- Mapping variants --- */

/* Helper: call a function on args (same as mapcar dispatch) */
static CL_Obj call_func(CL_Obj func, CL_Obj *call_args, int nargs)
{
    if (CL_FUNCTION_P(func)) {
        CL_Function *f = (CL_Function *)CL_OBJ_TO_PTR(func);
        if (nargs < f->min_args)
            cl_error(CL_ERR_ARGS, "too few arguments (got %d, min %d)",
                     nargs, f->min_args);
        if (f->max_args >= 0 && nargs > f->max_args)
            cl_error(CL_ERR_ARGS, "too many arguments (got %d, max %d)",
                     nargs, f->max_args);
        /* via cl_vm_apply: it GC-roots call_args across the call (the mapped
         * function may compact while reading its own args). */
        return cl_vm_apply(func, call_args, nargs);
    }
    if (CL_BYTECODE_P(func) || CL_CLOSURE_P(func))
        return cl_vm_apply(func, call_args, nargs);
    cl_error(CL_ERR_TYPE, "not a function");
    return CL_NIL;
}

static CL_Obj bi_mapc(CL_Obj *args, int n)
{
    CL_Obj func = cl_coerce_funcdesig(args[0], "MAPC");
    CL_Obj first_list = args[1];
    int nlists = n - 1;
    CL_Obj lists[16];
    CL_Obj call_args[16];
    int i;

    if (nlists > 16)
        cl_error(CL_ERR_ARGS,
                 "MAPC: too many list arguments (%d; max 16)", nlists);
    for (i = 0; i < nlists; i++)
        lists[i] = args[i + 1];

    /* GC-protect the list cursors across call_func (it runs the mapped
     * function, which may allocate and compact, relocating the cursors). */
    CL_GC_PROTECT(func);
    CL_GC_PROTECT(first_list);
    for (i = 0; i < nlists; i++)
        CL_GC_PROTECT(lists[i]);

    for (;;) {
        for (i = 0; i < nlists; i++) {
            if (CL_NULL_P(lists[i])) goto mapc_done;
        }
        for (i = 0; i < nlists; i++) {
            call_args[i] = cl_car(lists[i]);
            lists[i] = cl_cdr(lists[i]);
        }
        call_func(func, call_args, nlists);
    }

mapc_done:
    CL_GC_UNPROTECT(2 + nlists);
    return first_list;
}

static CL_Obj bi_mapcan(CL_Obj *args, int n)
{
    CL_Obj func = cl_coerce_funcdesig(args[0], "MAPCAN");
    int nlists = n - 1;
    CL_Obj lists[16];
    CL_Obj call_args[16];
    CL_Obj result = CL_NIL, tail = CL_NIL;
    int i;

    if (nlists > 16)
        cl_error(CL_ERR_ARGS,
                 "MAPCAN: too many list arguments (%d; max 16)", nlists);
    for (i = 0; i < nlists; i++)
        lists[i] = args[i + 1];

    CL_GC_PROTECT(func);
    CL_GC_PROTECT(result);
    CL_GC_PROTECT(tail);
    /* Protect cursors across call_func (mapped fn may allocate/compact). */
    for (i = 0; i < nlists; i++)
        CL_GC_PROTECT(lists[i]);

    for (;;) {
        CL_Obj val;

        for (i = 0; i < nlists; i++) {
            if (CL_NULL_P(lists[i])) goto mapcan_done;
        }
        for (i = 0; i < nlists; i++) {
            call_args[i] = cl_car(lists[i]);
            lists[i] = cl_cdr(lists[i]);
        }

        val = call_func(func, call_args, nlists);

        /* nconc val onto result */
        if (!CL_NULL_P(val)) {
            if (CL_NULL_P(result)) {
                result = val;
            } else {
                ((CL_Cons *)CL_OBJ_TO_PTR(tail))->cdr = val;
            }
            /* Walk to end of val */
            while (CL_CONS_P(val) && CL_CONS_P(cl_cdr(val)))
                val = cl_cdr(val);
            tail = val;
        }
    }

mapcan_done:
    CL_GC_UNPROTECT(3 + nlists);
    return result;
}

static CL_Obj bi_maplist(CL_Obj *args, int n)
{
    CL_Obj func = cl_coerce_funcdesig(args[0], "MAPLIST");
    int nlists = n - 1;
    CL_Obj lists[16];
    CL_Obj call_args[16];
    CL_Obj result = CL_NIL, tail = CL_NIL;
    int i;

    if (nlists > 16)
        cl_error(CL_ERR_ARGS,
                 "MAPLIST: too many list arguments (%d; max 16)", nlists);
    for (i = 0; i < nlists; i++)
        lists[i] = args[i + 1];

    CL_GC_PROTECT(func);
    CL_GC_PROTECT(result);
    CL_GC_PROTECT(tail);
    /* Protect cursors across call_func (mapped fn may allocate/compact). */
    for (i = 0; i < nlists; i++)
        CL_GC_PROTECT(lists[i]);

    for (;;) {
        CL_Obj val;

        for (i = 0; i < nlists; i++) {
            if (CL_NULL_P(lists[i])) goto maplist_done;
        }

        /* Pass the cdrs (sublists), not cars */
        for (i = 0; i < nlists; i++)
            call_args[i] = lists[i];

        val = call_func(func, call_args, nlists);

        {
            CL_Obj cell = cl_cons(val, CL_NIL);
            if (CL_NULL_P(result)) {
                result = cell;
            } else {
                ((CL_Cons *)CL_OBJ_TO_PTR(tail))->cdr = cell;
            }
            tail = cell;
        }

        for (i = 0; i < nlists; i++)
            lists[i] = cl_cdr(lists[i]);
    }

maplist_done:
    CL_GC_UNPROTECT(3 + nlists);
    return result;
}

static CL_Obj bi_mapl(CL_Obj *args, int n)
{
    /* coerce: MAPL accepts a function designator (symbol) like its siblings */
    CL_Obj func = cl_coerce_funcdesig(args[0], "MAPL");
    CL_Obj first_list = args[1];
    int nlists = n - 1;
    CL_Obj lists[16];
    CL_Obj call_args[16];
    int i;

    if (nlists > 16)
        cl_error(CL_ERR_ARGS,
                 "MAPL: too many list arguments (%d; max 16)", nlists);
    for (i = 0; i < nlists; i++)
        lists[i] = args[i + 1];

    /* Protect cursors across call_func (mapped fn may allocate/compact). */
    CL_GC_PROTECT(func);
    CL_GC_PROTECT(first_list);
    for (i = 0; i < nlists; i++)
        CL_GC_PROTECT(lists[i]);

    for (;;) {
        for (i = 0; i < nlists; i++) {
            if (CL_NULL_P(lists[i])) goto mapl_done;
        }
        for (i = 0; i < nlists; i++)
            call_args[i] = lists[i];

        call_func(func, call_args, nlists);

        for (i = 0; i < nlists; i++)
            lists[i] = cl_cdr(lists[i]);
    }

mapl_done:
    CL_GC_UNPROTECT(2 + nlists);
    return first_list;
}

static CL_Obj bi_mapcon(CL_Obj *args, int n)
{
    /* coerce: MAPCON accepts a function designator (symbol) like its siblings */
    CL_Obj func = cl_coerce_funcdesig(args[0], "MAPCON");
    int nlists = n - 1;
    CL_Obj lists[16];
    CL_Obj call_args[16];
    CL_Obj result = CL_NIL, tail = CL_NIL;
    int i;

    if (nlists > 16)
        cl_error(CL_ERR_ARGS,
                 "MAPCON: too many list arguments (%d; max 16)", nlists);
    for (i = 0; i < nlists; i++)
        lists[i] = args[i + 1];

    CL_GC_PROTECT(func);
    CL_GC_PROTECT(result);
    CL_GC_PROTECT(tail);
    /* Protect cursors across call_func (mapped fn may allocate/compact). */
    for (i = 0; i < nlists; i++)
        CL_GC_PROTECT(lists[i]);

    for (;;) {
        CL_Obj val;

        for (i = 0; i < nlists; i++) {
            if (CL_NULL_P(lists[i])) goto mapcon_done;
        }

        for (i = 0; i < nlists; i++)
            call_args[i] = lists[i];

        val = call_func(func, call_args, nlists);

        /* nconc val onto result */
        if (!CL_NULL_P(val)) {
            if (CL_NULL_P(result)) {
                result = val;
            } else {
                ((CL_Cons *)CL_OBJ_TO_PTR(tail))->cdr = val;
            }
            while (CL_CONS_P(val) && CL_CONS_P(cl_cdr(val)))
                val = cl_cdr(val);
            tail = val;
        }

        for (i = 0; i < nlists; i++)
            lists[i] = cl_cdr(lists[i]);
    }

mapcon_done:
    CL_GC_UNPROTECT(3 + nlists);
    return result;
}

/* --- list*, make-list, remf (Phase 8 Step 1) --- */

static CL_Obj bi_list_star(CL_Obj *args, int n)
{
    CL_Obj result;
    int i;
    if (n == 0)
        cl_error(CL_ERR_ARGS, "LIST*: at least 1 argument required");
    if (n == 1)
        return args[0];
    /* Last arg becomes the final cdr */
    result = args[n - 1];
    CL_GC_PROTECT(result);
    for (i = n - 2; i >= 0; i--)
        result = cl_cons(args[i], result);
    CL_GC_UNPROTECT(1);
    return result;
}

static CL_Obj bi_make_list(CL_Obj *args, int n)
{
    int32_t count;
    CL_Obj init_elem = CL_NIL;
    CL_Obj result = CL_NIL;
    int i;
    CL_Obj kw_init;

    if (!CL_FIXNUM_P(args[0]) || CL_FIXNUM_VAL(args[0]) < 0)
        cl_signal_type_error(args[0], "UNSIGNED-BYTE", "MAKE-LIST");
    count = CL_FIXNUM_VAL(args[0]);

    /* Detect odd count first (e.g. trailing :initial-element with no value). */
    if (n > 1 && ((n - 1) & 1))
        cl_error(CL_ERR_ARGS, "MAKE-LIST: odd number of keyword arguments");

    /* Parse keyword args: scan for :allow-other-keys first (leftmost wins) */
    {
        int allow_other = 0;
        int aok_found = 0;
        for (i = 1; i < n - 1; i += 2) {
            if (args[i] == KW_ALLOW_OTHER_KEYS && !aok_found) {
                allow_other = !CL_NULL_P(args[i + 1]);
                aok_found = 1;
            }
        }

        /* Now scan for :initial-element (leftmost wins) and validate
         * unknown keywords if allow-other-keys is not set. */
        kw_init = cl_intern_in("INITIAL-ELEMENT", 15, cl_package_keyword);
        {
            int found = 0;
            for (i = 1; i < n - 1; i += 2) {
                if (args[i] == kw_init) {
                    if (!found) {
                        init_elem = args[i + 1];
                        found = 1;
                    }
                } else if (args[i] == KW_ALLOW_OTHER_KEYS) {
                    /* already consumed */
                } else if (!allow_other) {
                    cl_error(CL_ERR_ARGS,
                             "MAKE-LIST: unknown keyword %s",
                             CL_SYMBOL_P(args[i]) ? cl_symbol_name(args[i])
                                                  : "?");
                }
            }
        }
    }

    /* init_elem is re-read on every iteration after the previous cl_cons may
     * have compacted — root it alongside the growing result. */
    CL_GC_PROTECT(result);
    CL_GC_PROTECT(init_elem);
    for (i = 0; i < count; i++)
        result = cl_cons(init_elem, result);
    CL_GC_UNPROTECT(2);
    return result;
}

static CL_Obj bi_remf(CL_Obj *args, int n)
{
    /* (remf place indicator) — destructive plist removal
       Returns the modified plist. Second value: T if found. */
    CL_Obj plist = args[0], indicator = args[1];
    CL_Obj prev = CL_NIL, curr = plist;
    CL_UNUSED(n);

    while (!CL_NULL_P(curr)) {
        CL_Obj key = cl_car(curr);
        CL_Obj next = cl_cdr(curr);
        if (CL_NULL_P(next)) break;
        if (key == indicator) {
            CL_Obj rest = cl_cdr(next);
            if (CL_NULL_P(prev)) {
                /* Removing from head */
                cl_mv_count = 2;
                cl_mv_values[0] = rest;
                cl_mv_values[1] = SYM_T;
                return rest;
            } else {
                ((CL_Cons *)CL_OBJ_TO_PTR(prev))->cdr = rest;
                cl_mv_count = 2;
                cl_mv_values[0] = plist;
                cl_mv_values[1] = SYM_T;
                return plist;
            }
        }
        prev = next;
        curr = cl_cdr(next);
    }
    cl_mv_count = 2;
    cl_mv_values[0] = plist;
    cl_mv_values[1] = CL_NIL;
    return plist;
}

/* --- Registration --- */

void cl_builtins_lists_init(void)
{
    /* List utilities */
    defun("NTHCDR", bi_nthcdr, 2, 2);
    defun("LAST", bi_last, 1, 2);
    defun("ACONS", bi_acons, 3, 3);
    defun("COPY-LIST", bi_copy_list, 1, 1);
    defun("PAIRLIS", bi_pairlis, 2, 3);
    defun("ASSOC", bi_assoc, 2, -1);
    defun("RASSOC", bi_rassoc, 2, -1);
    defun("GETF", bi_getf, 2, 3);
    defun("GET-PROPERTIES", bi_get_properties, 2, 2);
    cl_register_builtin("%SETF-GETF", bi_setf_getf, 3, 3, cl_package_amilisp);
    defun("SUBST", bi_subst, 3, -1);
    defun("SUBLIS", bi_sublis, 2, -1);
    defun("ADJOIN", bi_adjoin, 2, -1);

    /* Destructive list ops */
    defun("NCONC", bi_nconc, 0, -1);
    defun("NREVERSE", bi_nreverse, 1, 1);
    defun("DELETE", bi_delete, 2, -1);
    defun("DELETE-IF", bi_delete_if, 2, -1);
    defun("NSUBST", bi_nsubst, 3, -1);

    /* Copy/construction */
    defun("BUTLAST", bi_butlast, 1, 2);
    defun("NBUTLAST", bi_nbutlast, 1, 2);
    defun("COPY-TREE", bi_copy_tree, 1, 1);

    /* Phase 8 Step 1 */
    defun("LIST*", bi_list_star, 1, -1);
    defun("MAKE-LIST", bi_make_list, 1, -1);
    cl_register_builtin("%REMF", bi_remf, 2, 2, cl_package_amilisp);

    /* Mapping variants */
    defun("MAPC", bi_mapc, 2, -1);
    defun("MAPCAN", bi_mapcan, 2, -1);
    defun("MAPLIST", bi_maplist, 2, -1);
    defun("MAPL", bi_mapl, 2, -1);
    defun("MAPCON", bi_mapcon, 2, -1);
}
