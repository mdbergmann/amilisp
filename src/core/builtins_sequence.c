#include "builtins.h"
#include "symbol.h"
#include "package.h"
#include "mem.h"
#include "error.h"
#include "vm.h"
#include "string_utils.h"
#include "stream.h"
#include "../platform/platform.h"
#include <string.h>
#include <stdio.h>

/* --- Pre-interned keyword symbols --- */

static CL_Obj KW_TEST = CL_NIL;
static CL_Obj KW_TEST_NOT = CL_NIL;
static CL_Obj KW_KEY = CL_NIL;
static CL_Obj KW_START = CL_NIL;
static CL_Obj KW_END = CL_NIL;
static CL_Obj KW_COUNT = CL_NIL;
static CL_Obj KW_FROM_END = CL_NIL;
static CL_Obj KW_INITIAL_VALUE = CL_NIL;
static CL_Obj KW_START1 = CL_NIL;
static CL_Obj KW_END1 = CL_NIL;
static CL_Obj KW_START2 = CL_NIL;
static CL_Obj KW_END2 = CL_NIL;
static CL_Obj SEQ_KW_AOK = CL_NIL;
static CL_Obj SYM_EQL_FN = CL_NIL;
/* Builtin function objects for the MAP-INTO byte-vector fast path (bitwise
 * folds over packed u8 vectors run as C loops instead of one VM apply per
 * element — the ILBM mask-fold case).  Identity comparison: a redefined
 * LOGIOR no longer matches the cached original, so the fast path only fires
 * for the true builtins. */
static CL_Obj SYM_LOGIOR_FN = CL_NIL;
static CL_Obj SYM_LOGAND_FN = CL_NIL;
static CL_Obj SYM_LOGXOR_FN = CL_NIL;

/* --- Shared infrastructure --- */

/* Parsed keyword arguments for sequence functions */
typedef struct {
    CL_Obj test_fn;     /* :test function (default eql) */
    CL_Obj test_not_fn; /* :test-not function (default NIL) */
    CL_Obj key_fn;      /* :key function (default NIL = identity) */
    int32_t start;      /* :start (default 0) */
    int32_t end;        /* :end (default -1 = length) */
    int32_t count;      /* :count (default -1 = all) */
    int from_end;       /* :from-end (default 0) */
} SeqArgs;

static void parse_seq_args(CL_Obj *args, int n, int kw_start, SeqArgs *sa)
{
    int i;
    sa->test_fn = SYM_EQL_FN;
    sa->test_not_fn = CL_NIL;
    sa->key_fn = CL_NIL;
    sa->start = 0;
    sa->end = -1;
    sa->count = -1;
    sa->from_end = 0;

    /* Walk the keyword plist from the last pair down to the first so that, when
     * the same keyword appears more than once, the leftmost (first) value wins
     * — as required by CLHS 3.4.1.4. */
    for (i = kw_start + ((n - kw_start) / 2) * 2 - 2; i >= kw_start; i -= 2) {
        CL_Obj kw = args[i];
        CL_Obj val = args[i + 1];
        if (kw == KW_TEST) {
            sa->test_fn = cl_coerce_funcdesig(val, ":TEST");
        } else if (kw == KW_TEST_NOT) {
            sa->test_not_fn = cl_coerce_funcdesig(val, ":TEST-NOT");
        } else if (kw == KW_KEY) {
            sa->key_fn = CL_NULL_P(val) ? CL_NIL : cl_coerce_funcdesig(val, ":KEY");
        } else if (kw == KW_START) {
            if (CL_FIXNUM_P(val)) sa->start = CL_FIXNUM_VAL(val);
        } else if (kw == KW_END) {
            if (!CL_NULL_P(val) && CL_FIXNUM_P(val)) sa->end = CL_FIXNUM_VAL(val);
        } else if (kw == KW_COUNT) {
            /* nil => unlimited (leave sentinel -1).  A supplied integer limits
             * the count; per CLHS a negative count is treated as zero. */
            if (!CL_NULL_P(val) && CL_FIXNUM_P(val)) {
                sa->count = CL_FIXNUM_VAL(val);
                if (sa->count < 0) sa->count = 0;
            } else if (CL_BIGNUM_P(val)) {
                /* A bignum :count can't bound any real sequence's match
                 * total.  A negative bignum means "remove none" (count 0);
                 * a positive one means effectively unlimited (sentinel -1). */
                if (((CL_Bignum *)CL_OBJ_TO_PTR(val))->sign != 0)
                    sa->count = 0;
            }
        } else if (kw == KW_FROM_END) {
            sa->from_end = !CL_NULL_P(val);
        }
    }
}

/* Map a keyword symbol to its SK_* flag, or 0 if it is not a sequence keyword. */
static unsigned seq_keyword_flag(CL_Obj kw)
{
    if (kw == KW_TEST)      return SK_TEST;
    if (kw == KW_TEST_NOT)  return SK_TEST_NOT;
    if (kw == KW_KEY)       return SK_KEY;
    if (kw == KW_START)     return SK_START;
    if (kw == KW_END)       return SK_END;
    if (kw == KW_COUNT)     return SK_COUNT;
    if (kw == KW_FROM_END)  return SK_FROM_END;
    if (kw == KW_INITIAL_VALUE) return SK_INITIAL_VALUE;
    if (kw == KW_START1)    return SK_START1;
    if (kw == KW_END1)      return SK_END1;
    if (kw == KW_START2)    return SK_START2;
    if (kw == KW_END2)      return SK_END2;
    return 0;
}

/* The builtin whose arguments are being checked: call_builtin records it
 * per thread before entering the C function (thread.h last_builtin).  A
 * name that would have to be read from a corrupt object is answered as
 * "?" -- these messages are read on a corrupted heap. */
static const char *seq_builtin_name(void)
{
    CL_Obj f = CT->last_builtin;
    if (CL_HEAP_P(f) && f <= cl_heap.arena_size - sizeof(CL_Function) &&
        CL_HDR_TYPE(CL_OBJ_TO_PTR(f)) == TYPE_FUNCTION) {
        CL_Obj name = ((CL_Function *)CL_OBJ_TO_PTR(f))->name;
        if (CL_SYMBOL_P(name)) return cl_symbol_name(name);
    }
    return "?";
}

/* A keyword-argument error names the builtin, the offending value and its
 * position, and lists every argument: a non-symbol in a key position is
 * how a shifted or corrupted argument list first shows itself, and which
 * value sits where tells a missing leading argument (everything shifted
 * left by one) from a single bad value. */
static void seq_keyword_error(CL_Obj *args, int n, int i, const char *what)
{
    char val[96], all[256];
    cl_error(CL_ERR_ARGS, "%s: %s: %s (argument %d of %d; the arguments: %s)",
             seq_builtin_name(), what, cl_obj_brief(args[i], val, sizeof(val)),
             i + 1, n, cl_args_brief(args, n, all, sizeof(all)));
}

void cl_check_seq_keywords(CL_Obj *args, int n, int kw_start, unsigned allowed)
{
    int i, aok = 0;

    /* An odd number of cells in the keyword portion is a program-error. */
    if (((n - kw_start) & 1) != 0) {
        char all[256];
        cl_error(CL_ERR_ARGS, "%s: odd number of keyword arguments "
                 "(%d after the %d positional; the arguments: %s)",
                 seq_builtin_name(), n - kw_start, kw_start,
                 cl_args_brief(args, n, all, sizeof(all)));
    }

    /* Leftmost :allow-other-keys wins. */
    for (i = kw_start; i + 1 < n; i += 2) {
        if (args[i] == SEQ_KW_AOK) { aok = !CL_NULL_P(args[i + 1]); break; }
    }
    if (aok) return;

    for (i = kw_start; i + 1 < n; i += 2) {
        CL_Obj kw = args[i];
        if (kw == SEQ_KW_AOK) continue;
        if (!CL_SYMBOL_P(kw))
            seq_keyword_error(args, n, i, "keyword-argument key is not a symbol");
        if (!(seq_keyword_flag(kw) & allowed))
            seq_keyword_error(args, n, i, "unrecognized keyword argument");
    }
}

/* "not a function" for a :test / :key / mapped function designator: what
 * the value is, which builtin got it. */
static void seq_not_a_function(CL_Obj fn, const char *role)
{
    char val[96];
    cl_error(CL_ERR_TYPE, "%s: not a function%s: %s", seq_builtin_name(), role,
             cl_obj_brief(fn, val, sizeof(val)));
}

/* Call a 2-arg test function */
static CL_Obj call_test(CL_Obj test_fn, CL_Obj a, CL_Obj b)
{
    CL_Obj targs[2];
    targs[0] = a;
    targs[1] = b;
    /* Resolve symbol function designator */
    if (CL_SYMBOL_P(test_fn)) {
        CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(test_fn);
        test_fn = s->function;
    }
    /* Unwrap funcallable instances (e.g. a generic function) — the
     * discriminating-function slot is a closure, handled below. */
    if (cl_funcallable_instance_p(test_fn))
        test_fn = cl_unwrap_funcallable(test_fn);
    if (CL_FUNCTION_OBJ_P(test_fn))
        /* cl_vm_apply GC-roots targs across the call (a :test/:key may compact
         * while reading its args). */
        return cl_vm_apply(test_fn, targs, 2);
    seq_not_a_function(test_fn, " (:test)");
    return CL_NIL;
}

/* Call a 1-arg predicate/key function */
static CL_Obj call_1(CL_Obj fn, CL_Obj arg)
{
    CL_Obj pargs[1];
    pargs[0] = arg;
    /* Resolve symbol function designator */
    if (CL_SYMBOL_P(fn)) {
        CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(fn);
        fn = s->function;
    }
    if (cl_funcallable_instance_p(fn)) {
        /* Reader-GF fast path: a promoted reader used as :key/:test-arg or
         * mapped function ((mapcar #'reader xs), (remove-if-not #'reader xs))
         * answers straight from the inline cache — no unwrap, no VM entry.
         * Probe before the unwrap discards the GF identity. */
        CL_Obj v = cl_gf_reader_ic_probe(fn, arg);
        if (v != CL_UNBOUND) return v;
        fn = cl_unwrap_funcallable(fn);
    }
    if (CL_FUNCTION_OBJ_P(fn))
        /* cl_vm_apply GC-roots pargs across the call. */
        return cl_vm_apply(fn, pargs, 1);
    seq_not_a_function(fn, "");
    return CL_NIL;
}

/* Call a function designator with no arguments (used by REDUCE on an empty
 * subsequence with no :initial-value — CLHS specifies (funcall fn)). */
static CL_Obj call_0(CL_Obj fn)
{
    if (CL_SYMBOL_P(fn)) {
        CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(fn);
        fn = s->function;
    }
    if (cl_funcallable_instance_p(fn))
        fn = cl_unwrap_funcallable(fn);
    if (CL_FUNCTION_OBJ_P(fn))
        return cl_vm_apply(fn, NULL, 0);
    seq_not_a_function(fn, "");
    return CL_NIL;
}

/* Apply :key if present, otherwise return element unchanged */
static CL_Obj apply_key(CL_Obj key_fn, CL_Obj elem)
{
    if (CL_NULL_P(key_fn)) return elem;
    return call_1(key_fn, elem);
}

/* Test item against element (applying :key to element).
 * GC SAFETY: apply_key runs user code that can compact — item and the
 * SeqArgs function fields (a C struct invisible to the GC) are protected
 * across it so the test call reads forwarded values, not stale offsets. */
static int seq_test_match(SeqArgs *sa, CL_Obj item, CL_Obj elem)
{
    CL_Obj keyed;
    int r;
    CL_GC_PROTECT(item);
    CL_GC_PROTECT(sa->test_fn);
    CL_GC_PROTECT(sa->test_not_fn);
    CL_GC_PROTECT(sa->key_fn);  /* keep forwarded for the NEXT match call too */
    keyed = apply_key(sa->key_fn, elem);
    if (!CL_NULL_P(sa->test_not_fn))
        r = CL_NULL_P(call_test(sa->test_not_fn, item, keyed));
    else
        r = !CL_NULL_P(call_test(sa->test_fn, item, keyed));
    CL_GC_UNPROTECT(4);
    return r;
}

/* Test predicate against element (applying :key to element).
 * GC SAFETY: pred and key_fn (by-value copies) are held across apply_key so
 * the call_1 below reads forwarded values.  NOTE: this only protects the
 * copies — a caller that re-reads its own pred/key locals on the next
 * iteration must protect those itself (see remove_from_list) or use the
 * SeqArgs variant below. */
static int seq_pred_match(CL_Obj pred, CL_Obj key_fn, CL_Obj elem)
{
    CL_Obj keyed;
    int r;
    CL_GC_PROTECT(pred);
    CL_GC_PROTECT(key_fn);
    keyed = apply_key(key_fn, elem);
    r = !CL_NULL_P(call_1(pred, keyed));
    CL_GC_UNPROTECT(2);
    return r;
}

/* SeqArgs variant: additionally roots sa->key_fn through the pointer, so the
 * CALLER's SeqArgs field is forwarded across the user call and stays valid
 * for the next iteration (the struct is invisible to the GC otherwise). */
static int seq_pred_match_sa(CL_Obj pred, SeqArgs *sa, CL_Obj elem)
{
    CL_Obj keyed;
    int r;
    CL_GC_PROTECT(pred);
    CL_GC_PROTECT(sa->key_fn);
    keyed = apply_key(sa->key_fn, elem);
    r = !CL_NULL_P(call_1(pred, keyed));
    CL_GC_UNPROTECT(2);
    return r;
}

/* --- Sequence element access helpers --- */

static int32_t seq_length(CL_Obj seq)
{
    if (CL_NULL_P(seq)) return 0;
    if (CL_CONS_P(seq)) {
        int32_t len = 0;
        while (!CL_NULL_P(seq)) { len++; seq = cl_cdr(seq); }
        return len;
    }
    if (CL_VECTOR_P(seq)) {
        CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(seq);
        return (int32_t)cl_vector_active_length(v);
    }
    if (CL_ANY_STRING_P(seq)) {
        return (int32_t)cl_string_length(seq);
    }
    if (CL_BIT_VECTOR_P(seq)) {
        CL_BitVector *bv = (CL_BitVector *)CL_OBJ_TO_PTR(seq);
        return (int32_t)cl_bv_active_length(bv);
    }
    if (CL_BYTE_VECTOR_P(seq)) {
        CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(seq);
        return (int32_t)cl_bytevec_active_length(bv);
    }
    /* Carry :datum/:expected-type so handler code (and the ANSI
     * check-type-error tests) can recover the offending object. */
    cl_signal_type_error(seq, "SEQUENCE", "sequence operation");
    return 0;
}

static CL_Obj seq_elt(CL_Obj seq, int32_t idx)
{
    if (CL_CONS_P(seq) || CL_NULL_P(seq)) {
        while (idx > 0 && !CL_NULL_P(seq)) { seq = cl_cdr(seq); idx--; }
        return CL_NULL_P(seq) ? CL_NIL : cl_car(seq);
    }
    if (CL_VECTOR_P(seq)) {
        CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(seq);
        if ((uint32_t)idx >= cl_vector_active_length(v)) cl_error(CL_ERR_ARGS, "index out of bounds");
        return cl_vector_data(v)[idx];
    }
    if (CL_ANY_STRING_P(seq)) {
        if ((uint32_t)idx >= cl_string_length(seq)) cl_error(CL_ERR_ARGS, "index out of bounds");
        return CL_MAKE_CHAR(cl_string_char_at(seq, (uint32_t)idx));
    }
    if (CL_BIT_VECTOR_P(seq)) {
        CL_BitVector *bv = (CL_BitVector *)CL_OBJ_TO_PTR(seq);
        if ((uint32_t)idx >= cl_bv_active_length(bv)) cl_error(CL_ERR_ARGS, "index out of bounds");
        return CL_MAKE_FIXNUM(cl_bv_get_bit(bv, (uint32_t)idx));
    }
    if (CL_BYTE_VECTOR_P(seq)) {
        CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(seq);
        if ((uint32_t)idx >= cl_bytevec_active_length(bv)) cl_error(CL_ERR_ARGS, "index out of bounds");
        return CL_MAKE_FIXNUM(cl_bytevec_get(bv, (uint32_t)idx));
    }
    cl_signal_type_error(seq, "SEQUENCE", "sequence operation");
    return CL_NIL;
}

/* --- Generic array-sequence (vector / string / bit-vector) helpers --- */
/* These re-derive the heap pointer on every call so they remain valid even
 * across allocating :test/:key calls (the compacting GC may relocate seq).
 * The caller must keep `seq` GC-rooted (args[] and CL_GC_PROTECTed locals are).*/

static int seq_is_array(CL_Obj seq)
{
    return CL_VECTOR_P(seq) || CL_ANY_STRING_P(seq) || CL_BIT_VECTOR_P(seq) ||
           CL_BYTE_VECTOR_P(seq);
}

static CL_Obj arr_seq_get(CL_Obj seq, int32_t i)
{
    if (CL_VECTOR_P(seq))
        return cl_vector_data((CL_Vector *)CL_OBJ_TO_PTR(seq))[i];
    if (CL_ANY_STRING_P(seq))
        return CL_MAKE_CHAR(cl_string_char_at(seq, (uint32_t)i));
    if (CL_BYTE_VECTOR_P(seq))
        return CL_MAKE_FIXNUM(
            cl_bytevec_get((CL_ByteVector *)CL_OBJ_TO_PTR(seq), (uint32_t)i));
    /* bit vector */
    return CL_MAKE_FIXNUM(cl_bv_get_bit((CL_BitVector *)CL_OBJ_TO_PTR(seq), (uint32_t)i));
}

static void arr_seq_set(CL_Obj seq, int32_t i, CL_Obj val)
{
    if (CL_VECTOR_P(seq)) {
        cl_vector_data((CL_Vector *)CL_OBJ_TO_PTR(seq))[i] = val;
        return;
    }
    if (CL_ANY_STRING_P(seq)) {
        /* A string's element type is CHARACTER; storing anything else is a
         * type-error (CLHS).  Previously this silently dropped non-character
         * values, which let e.g. REPLACE into a string from a list of
         * non-characters succeed quietly instead of signalling. */
        if (!CL_CHAR_P(val))
            cl_signal_type_error(val, "CHARACTER", "storing into a string");
        cl_string_set_char_at(seq, (uint32_t)i, CL_CHAR_VAL(val));
        return;
    }
    if (CL_BYTE_VECTOR_P(seq)) {
        /* Byte vector: element type is (UNSIGNED/SIGNED-BYTE 8/16). */
        CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(seq);
        cl_bytevec_set(bv, i,
                       cl_bytevec_check_value(val, bv->is_signed, bv->elt_shift,
                                              "storing into a byte vector"));
        return;
    }
    /* bit vector: element type is BIT (0 or 1). */
    {
        int32_t v;
        if (!CL_FIXNUM_P(val))
            cl_signal_type_error(val, "BIT", "storing into a bit vector");
        v = CL_FIXNUM_VAL(val);
        if (v != 0 && v != 1)
            cl_signal_type_error(val, "BIT", "storing into a bit vector");
        cl_bv_set_bit((CL_BitVector *)CL_OBJ_TO_PTR(seq), (uint32_t)i, v != 0);
    }
}

/* String-like = a simple string (TYPE_STRING / TYPE_WIDE_STRING) OR an
 * adjustable / fill-pointer character vector (CL_VEC_FLAG_STRING).  Both are
 * valid CL STRINGs and the sequence functions must produce STRING results for
 * either, not a general (vector character). */
static int seq_is_string_like(CL_Obj seq)
{
    return CL_ANY_STRING_P(seq) || CL_STRING_VECTOR_P(seq);
}

/* True if a string-like sequence holds any character code > 255 (needs wide
 * storage to round-trip without truncation). */
static int seq_string_needs_wide(CL_Obj seq)
{
#ifdef CL_WIDE_STRINGS
    int32_t i, len = (int32_t)cl_string_length(seq);
    if (CL_WIDE_STRING_P(seq)) return 1;
    for (i = 0; i < len; i++)
        if (cl_string_char_at(seq, (uint32_t)i) > 255) return 1;
#else
    (void)seq;
#endif
    return 0;
}

/* Make a fresh simple result sequence of the same broad class as SEQ
 * (string / bit-vector / general vector) holding LENGTH elements.  Used so the
 * non-destructive sequence functions return a result of the correct type. */
static CL_Obj make_seq_result_like(CL_Obj seq, uint32_t length)
{
    if (seq_is_string_like(seq)) {
#ifdef CL_WIDE_STRINGS
        if (seq_string_needs_wide(seq)) return cl_make_wide_string(NULL, length);
#endif
        return cl_make_string(NULL, length);
    }
    if (CL_BIT_VECTOR_P(seq)) return cl_make_bit_vector(length);
    if (CL_BYTE_VECTOR_P(seq)) {
        CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(seq);
        return cl_make_byte_vector(length, bv->is_signed, bv->elt_shift);
    }
    return cl_make_vector(length);
}

/* Fresh copy of an array sequence preserving element values and active length.
 * GC-protects `seq` so the source pointer re-derived after the (compacting)
 * allocation is forwarded to the relocated object. */
static CL_Obj copy_array_seq(CL_Obj seq)
{
    CL_Obj result;
    CL_GC_PROTECT(seq);
    if (seq_is_string_like(seq)) {
        /* Includes adjustable/fill-pointer character vectors: produce a simple
         * STRING of the active length, copying characters individually. */
        uint32_t alen = cl_string_length(seq);
        uint32_t i;
        result = make_seq_result_like(seq, alen);
        for (i = 0; i < alen; i++)
            cl_string_set_char_at(result, i, cl_string_char_at(seq, i));
    } else if (CL_VECTOR_P(seq)) {
        CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(seq);
        uint32_t alen = cl_vector_active_length(v);
        CL_Vector *rv;
        result = cl_make_vector(alen);
        rv = (CL_Vector *)CL_OBJ_TO_PTR(result);
        v = (CL_Vector *)CL_OBJ_TO_PTR(seq); /* refresh after alloc */
        memcpy(cl_vector_data(rv), cl_vector_data(v), alen * sizeof(CL_Obj));
        /* Preserve the declared element type (serapeum VECT-TYPE / ansi
         * COPY-SEQ.23): a copy of a specialized numeric vector must report the
         * same ARRAY-ELEMENT-TYPE. */
        rv->elt_type = v->elt_type;
    } else if (CL_BYTE_VECTOR_P(seq)) {
        CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(seq);
        uint32_t blen = cl_bytevec_active_length(bv);
        CL_ByteVector *rv;
        result = cl_make_byte_vector(blen, bv->is_signed, bv->elt_shift);
        rv = (CL_ByteVector *)CL_OBJ_TO_PTR(result);
        bv = (CL_ByteVector *)CL_OBJ_TO_PTR(seq); /* refresh after alloc */
        memcpy(rv->data, bv->data, (size_t)blen << bv->elt_shift);
    } else {
        /* bit vector */
        CL_BitVector *bv = (CL_BitVector *)CL_OBJ_TO_PTR(seq);
        uint32_t blen = cl_bv_active_length(bv);
        CL_BitVector *rv;
        result = cl_make_bit_vector(blen);
        rv = (CL_BitVector *)CL_OBJ_TO_PTR(result);
        bv = (CL_BitVector *)CL_OBJ_TO_PTR(seq); /* refresh after alloc */
        memcpy(rv->data, bv->data, CL_BV_WORDS(blen) * sizeof(uint32_t));
    }
    CL_GC_UNPROTECT(1);
    return result;
}

/* ======================================================= */
/* FIND / FIND-IF / FIND-IF-NOT                            */
/* ======================================================= */

static CL_Obj bi_find(CL_Obj *args, int n)
{
    CL_Obj seq = args[1];
    SeqArgs sa;
    int32_t i, len, end;

    cl_check_seq_keywords(args, n, 2, SK_FIND_KEYS);
    parse_seq_args(args, n, 2, &sa);
    len = seq_length(seq);
    end = (sa.end < 0) ? len : sa.end;

    /* GC SAFETY (this and every scan loop below): the :test/:key call can
     * compact — list cursors are protected roots, elements are re-read from
     * the protected cursor / rooted args[1] slot after a match, and the
     * :from-end `found` slot is protected across subsequent iterations. */
    if (!sa.from_end) {
        /* Forward scan */
        if (CL_CONS_P(seq) || CL_NULL_P(seq)) {
            CL_Obj cur = seq;
            CL_GC_PROTECT(cur);
            for (i = 0; i < end && !CL_NULL_P(cur); i++, cur = cl_cdr(cur)) {
                if (i >= sa.start && seq_test_match(&sa, args[0], cl_car(cur))) {
                    CL_Obj r = cl_car(cur);
                    CL_GC_UNPROTECT(1);
                    return r;
                }
            }
            CL_GC_UNPROTECT(1);
        } else {
            for (i = sa.start; i < end; i++) {
                if (seq_test_match(&sa, args[0], seq_elt(args[1], i)))
                    return seq_elt(args[1], i);
            }
        }
    } else {
        /* :from-end — forward scan tracking last match */
        CL_Obj found = CL_NIL;
        int found_p = 0;
        CL_GC_PROTECT(found);
        if (CL_CONS_P(seq) || CL_NULL_P(seq)) {
            CL_Obj cur = seq;
            CL_GC_PROTECT(cur);
            for (i = 0; i < end && !CL_NULL_P(cur); i++, cur = cl_cdr(cur)) {
                if (i >= sa.start && seq_test_match(&sa, args[0], cl_car(cur))) {
                    found = cl_car(cur);
                    found_p = 1;
                }
            }
            CL_GC_UNPROTECT(1);
        } else {
            for (i = sa.start; i < end; i++) {
                if (seq_test_match(&sa, args[0], seq_elt(args[1], i))) {
                    found = seq_elt(args[1], i);
                    found_p = 1;
                }
            }
        }
        CL_GC_UNPROTECT(1);  /* found */
        if (found_p) return found;
    }
    return CL_NIL;
}

static CL_Obj bi_find_if(CL_Obj *args, int n)
{
    CL_Obj seq = args[1];
    SeqArgs sa;
    int32_t i, len, end;

    cl_check_seq_keywords(args, n, 2, SK_FIND_IF_KEYS);
    parse_seq_args(args, n, 2, &sa);
    len = seq_length(seq);
    end = (sa.end < 0) ? len : sa.end;

    if (!sa.from_end) {
        if (CL_CONS_P(seq) || CL_NULL_P(seq)) {
            CL_Obj cur = seq;
            CL_GC_PROTECT(cur);
            for (i = 0; i < end && !CL_NULL_P(cur); i++, cur = cl_cdr(cur)) {
                if (i >= sa.start && seq_pred_match_sa(args[0], &sa, cl_car(cur))) {
                    CL_Obj r = cl_car(cur);
                    CL_GC_UNPROTECT(1);
                    return r;
                }
            }
            CL_GC_UNPROTECT(1);
        } else {
            for (i = sa.start; i < end; i++) {
                if (seq_pred_match_sa(args[0], &sa, seq_elt(args[1], i)))
                    return seq_elt(args[1], i);
            }
        }
    } else {
        CL_Obj found = CL_NIL;
        int found_p = 0;
        CL_GC_PROTECT(found);
        if (CL_CONS_P(seq) || CL_NULL_P(seq)) {
            CL_Obj cur = seq;
            CL_GC_PROTECT(cur);
            for (i = 0; i < end && !CL_NULL_P(cur); i++, cur = cl_cdr(cur)) {
                if (i >= sa.start && seq_pred_match_sa(args[0], &sa, cl_car(cur))) {
                    found = cl_car(cur);
                    found_p = 1;
                }
            }
            CL_GC_UNPROTECT(1);
        } else {
            for (i = sa.start; i < end; i++) {
                if (seq_pred_match_sa(args[0], &sa, seq_elt(args[1], i))) {
                    found = seq_elt(args[1], i);
                    found_p = 1;
                }
            }
        }
        CL_GC_UNPROTECT(1);  /* found */
        if (found_p) return found;
    }
    return CL_NIL;
}

static CL_Obj bi_find_if_not(CL_Obj *args, int n)
{
    CL_Obj seq = args[1];
    SeqArgs sa;
    int32_t i, len, end;

    cl_check_seq_keywords(args, n, 2, SK_FIND_IF_KEYS);
    parse_seq_args(args, n, 2, &sa);
    len = seq_length(seq);
    end = (sa.end < 0) ? len : sa.end;

    if (!sa.from_end) {
        if (CL_CONS_P(seq) || CL_NULL_P(seq)) {
            CL_Obj cur = seq;
            CL_GC_PROTECT(cur);
            for (i = 0; i < end && !CL_NULL_P(cur); i++, cur = cl_cdr(cur)) {
                if (i >= sa.start && !seq_pred_match_sa(args[0], &sa, cl_car(cur))) {
                    CL_Obj r = cl_car(cur);
                    CL_GC_UNPROTECT(1);
                    return r;
                }
            }
            CL_GC_UNPROTECT(1);
        } else {
            for (i = sa.start; i < end; i++) {
                if (!seq_pred_match_sa(args[0], &sa, seq_elt(args[1], i)))
                    return seq_elt(args[1], i);
            }
        }
    } else {
        CL_Obj found = CL_NIL;
        int found_p = 0;
        CL_GC_PROTECT(found);
        if (CL_CONS_P(seq) || CL_NULL_P(seq)) {
            CL_Obj cur = seq;
            CL_GC_PROTECT(cur);
            for (i = 0; i < end && !CL_NULL_P(cur); i++, cur = cl_cdr(cur)) {
                if (i >= sa.start && !seq_pred_match_sa(args[0], &sa, cl_car(cur))) {
                    found = cl_car(cur);
                    found_p = 1;
                }
            }
            CL_GC_UNPROTECT(1);
        } else {
            for (i = sa.start; i < end; i++) {
                if (!seq_pred_match_sa(args[0], &sa, seq_elt(args[1], i))) {
                    found = seq_elt(args[1], i);
                    found_p = 1;
                }
            }
        }
        CL_GC_UNPROTECT(1);  /* found */
        if (found_p) return found;
    }
    return CL_NIL;
}

/* ======================================================= */
/* POSITION / POSITION-IF / POSITION-IF-NOT                */
/* ======================================================= */

static CL_Obj bi_position(CL_Obj *args, int n)
{
    CL_Obj seq = args[1];
    SeqArgs sa;
    int32_t i, len, end;

    cl_check_seq_keywords(args, n, 2, SK_FIND_KEYS);
    parse_seq_args(args, n, 2, &sa);
    len = seq_length(seq);
    end = (sa.end < 0) ? len : sa.end;

    if (!sa.from_end) {
        if (CL_CONS_P(seq) || CL_NULL_P(seq)) {
            CL_Obj cur = seq;
            CL_GC_PROTECT(cur);
            for (i = 0; i < end && !CL_NULL_P(cur); i++, cur = cl_cdr(cur)) {
                if (i >= sa.start && seq_test_match(&sa, args[0], cl_car(cur))) {
                    CL_GC_UNPROTECT(1);
                    return CL_MAKE_FIXNUM(i);
                }
            }
            CL_GC_UNPROTECT(1);
        } else {
            for (i = sa.start; i < end; i++) {
                if (seq_test_match(&sa, args[0], seq_elt(args[1], i)))
                    return CL_MAKE_FIXNUM(i);
            }
        }
    } else {
        int32_t found = -1;
        if (CL_CONS_P(seq) || CL_NULL_P(seq)) {
            CL_Obj cur = seq;
            CL_GC_PROTECT(cur);
            for (i = 0; i < end && !CL_NULL_P(cur); i++, cur = cl_cdr(cur)) {
                if (i >= sa.start && seq_test_match(&sa, args[0], cl_car(cur)))
                    found = i;
            }
            CL_GC_UNPROTECT(1);
        } else {
            for (i = sa.start; i < end; i++) {
                if (seq_test_match(&sa, args[0], seq_elt(args[1], i)))
                    found = i;
            }
        }
        if (found >= 0) return CL_MAKE_FIXNUM(found);
    }
    return CL_NIL;
}

static CL_Obj bi_position_if(CL_Obj *args, int n)
{
    CL_Obj seq = args[1];
    SeqArgs sa;
    int32_t i, len, end;

    cl_check_seq_keywords(args, n, 2, SK_FIND_IF_KEYS);
    parse_seq_args(args, n, 2, &sa);
    len = seq_length(seq);
    end = (sa.end < 0) ? len : sa.end;

    if (!sa.from_end) {
        if (CL_CONS_P(seq) || CL_NULL_P(seq)) {
            CL_Obj cur = seq;
            CL_GC_PROTECT(cur);
            for (i = 0; i < end && !CL_NULL_P(cur); i++, cur = cl_cdr(cur)) {
                if (i >= sa.start && seq_pred_match_sa(args[0], &sa, cl_car(cur))) {
                    CL_GC_UNPROTECT(1);
                    return CL_MAKE_FIXNUM(i);
                }
            }
            CL_GC_UNPROTECT(1);
        } else {
            for (i = sa.start; i < end; i++) {
                if (seq_pred_match_sa(args[0], &sa, seq_elt(args[1], i)))
                    return CL_MAKE_FIXNUM(i);
            }
        }
    } else {
        int32_t found = -1;
        if (CL_CONS_P(seq) || CL_NULL_P(seq)) {
            CL_Obj cur = seq;
            CL_GC_PROTECT(cur);
            for (i = 0; i < end && !CL_NULL_P(cur); i++, cur = cl_cdr(cur)) {
                if (i >= sa.start && seq_pred_match_sa(args[0], &sa, cl_car(cur)))
                    found = i;
            }
            CL_GC_UNPROTECT(1);
        } else {
            for (i = sa.start; i < end; i++) {
                if (seq_pred_match_sa(args[0], &sa, seq_elt(args[1], i)))
                    found = i;
            }
        }
        if (found >= 0) return CL_MAKE_FIXNUM(found);
    }
    return CL_NIL;
}

static CL_Obj bi_position_if_not(CL_Obj *args, int n)
{
    CL_Obj seq = args[1];
    SeqArgs sa;
    int32_t i, len, end;

    cl_check_seq_keywords(args, n, 2, SK_FIND_IF_KEYS);
    parse_seq_args(args, n, 2, &sa);
    len = seq_length(seq);
    end = (sa.end < 0) ? len : sa.end;

    if (!sa.from_end) {
        if (CL_CONS_P(seq) || CL_NULL_P(seq)) {
            CL_Obj cur = seq;
            CL_GC_PROTECT(cur);
            for (i = 0; i < end && !CL_NULL_P(cur); i++, cur = cl_cdr(cur)) {
                if (i >= sa.start && !seq_pred_match_sa(args[0], &sa, cl_car(cur))) {
                    CL_GC_UNPROTECT(1);
                    return CL_MAKE_FIXNUM(i);
                }
            }
            CL_GC_UNPROTECT(1);
        } else {
            for (i = sa.start; i < end; i++) {
                if (!seq_pred_match_sa(args[0], &sa, seq_elt(args[1], i)))
                    return CL_MAKE_FIXNUM(i);
            }
        }
    } else {
        int32_t found = -1;
        if (CL_CONS_P(seq) || CL_NULL_P(seq)) {
            CL_Obj cur = seq;
            CL_GC_PROTECT(cur);
            for (i = 0; i < end && !CL_NULL_P(cur); i++, cur = cl_cdr(cur)) {
                if (i >= sa.start && !seq_pred_match_sa(args[0], &sa, cl_car(cur)))
                    found = i;
            }
            CL_GC_UNPROTECT(1);
        } else {
            for (i = sa.start; i < end; i++) {
                if (!seq_pred_match_sa(args[0], &sa, seq_elt(args[1], i)))
                    found = i;
            }
        }
        if (found >= 0) return CL_MAKE_FIXNUM(found);
    }
    return CL_NIL;
}

/* ======================================================= */
/* COUNT / COUNT-IF / COUNT-IF-NOT                         */
/* ======================================================= */

static CL_Obj bi_count(CL_Obj *args, int n)
{
    CL_Obj seq = args[1];
    SeqArgs sa;
    int32_t i, len, end, cnt = 0;

    cl_check_seq_keywords(args, n, 2, SK_FIND_KEYS);
    parse_seq_args(args, n, 2, &sa);
    len = seq_length(seq);
    end = (sa.end < 0) ? len : sa.end;

    /* Packed byte-vector fast path: counting a fixnum under the default
     * EQL test (no :key, no :test-not) is a pure C loop over the packed
     * elements.  The general path below costs one VM test call per
     * element, which made the map cache's wall-code validation (three
     * COUNTs over a 30x30 city's 3600 wall bytes) take ~4.7 s on a 14MHz
     * 68020.  :from-end changes nothing for a pure EQL count, so it is
     * included; out-of-range bounds fall through to the general path's
     * error behavior. */
    if (CL_BYTE_VECTOR_P(seq) && CL_FIXNUM_P(args[0]) &&
        CL_NULL_P(sa.key_fn) && CL_NULL_P(sa.test_not_fn) &&
        sa.test_fn == SYM_EQL_FN &&
        sa.start >= 0 && sa.start <= end && end <= len) {
        CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(seq);
        int32_t item = CL_FIXNUM_VAL(args[0]);
        for (i = sa.start; i < end; i++)
            if (cl_bytevec_get(bv, i) == item)
                cnt++;
        return CL_MAKE_FIXNUM(cnt);
    }

    if (sa.from_end) {
        if (CL_CONS_P(seq) || CL_NULL_P(seq)) {
            int32_t range = end - sa.start;
            if (range > 0) {
                /* Snapshot into a GC vector, not a platform_alloc array: the
                 * :test/:key calls compact, and the GC neither traces nor
                 * forwards a raw C array's CL_Obj entries (same fix as
                 * REDUCE :from-end).  Re-deref the vector each access — it
                 * moves too. */
                CL_Obj vec = cl_make_vector((uint32_t)range);
                CL_Obj cur;
                int32_t j = 0;
                CL_GC_PROTECT(vec);
                cur = args[1];  /* fresh read: cl_make_vector may have compacted */
                for (i = 0; i < end && !CL_NULL_P(cur); i++, cur = cl_cdr(cur)) {
                    if (i >= sa.start)
                        cl_vector_data((CL_Vector *)CL_OBJ_TO_PTR(vec))[j++] = cl_car(cur);
                }
                for (i = j - 1; i >= 0; i--) {
                    if (seq_test_match(&sa, args[0],
                            cl_vector_data((CL_Vector *)CL_OBJ_TO_PTR(vec))[i]))
                        cnt++;
                }
                CL_GC_UNPROTECT(1);
            }
        } else {
            for (i = end - 1; i >= sa.start; i--) {
                if (seq_test_match(&sa, args[0], seq_elt(args[1], i)))
                    cnt++;
            }
        }
    } else {
        if (CL_CONS_P(seq) || CL_NULL_P(seq)) {
            CL_Obj cur = seq;
            CL_GC_PROTECT(cur);
            for (i = 0; i < end && !CL_NULL_P(cur); i++, cur = cl_cdr(cur)) {
                if (i >= sa.start && seq_test_match(&sa, args[0], cl_car(cur)))
                    cnt++;
            }
            CL_GC_UNPROTECT(1);
        } else {
            for (i = sa.start; i < end; i++) {
                if (seq_test_match(&sa, args[0], seq_elt(args[1], i)))
                    cnt++;
            }
        }
    }
    return CL_MAKE_FIXNUM(cnt);
}

static CL_Obj bi_count_if(CL_Obj *args, int n)
{
    CL_Obj seq = args[1];
    SeqArgs sa;
    int32_t i, len, end, cnt = 0;

    cl_check_seq_keywords(args, n, 2, SK_FIND_IF_KEYS);
    parse_seq_args(args, n, 2, &sa);
    len = seq_length(seq);
    end = (sa.end < 0) ? len : sa.end;

    if (sa.from_end) {
        /* :from-end — iterate in reverse order (matters for stateful predicates) */
        if (CL_CONS_P(seq) || CL_NULL_P(seq)) {
            /* GC-vector snapshot — see bi_count for why not platform_alloc */
            int32_t range = end - sa.start;
            if (range > 0) {
                CL_Obj vec = cl_make_vector((uint32_t)range);
                CL_Obj cur;
                int32_t j = 0;
                CL_GC_PROTECT(vec);
                cur = args[1];
                for (i = 0; i < end && !CL_NULL_P(cur); i++, cur = cl_cdr(cur)) {
                    if (i >= sa.start)
                        cl_vector_data((CL_Vector *)CL_OBJ_TO_PTR(vec))[j++] = cl_car(cur);
                }
                for (i = j - 1; i >= 0; i--) {
                    if (seq_pred_match_sa(args[0], &sa,
                            cl_vector_data((CL_Vector *)CL_OBJ_TO_PTR(vec))[i]))
                        cnt++;
                }
                CL_GC_UNPROTECT(1);
            }
        } else {
            for (i = end - 1; i >= sa.start; i--) {
                if (seq_pred_match_sa(args[0], &sa, seq_elt(args[1], i)))
                    cnt++;
            }
        }
    } else {
        if (CL_CONS_P(seq) || CL_NULL_P(seq)) {
            CL_Obj cur = seq;
            CL_GC_PROTECT(cur);
            for (i = 0; i < end && !CL_NULL_P(cur); i++, cur = cl_cdr(cur)) {
                if (i >= sa.start && seq_pred_match_sa(args[0], &sa, cl_car(cur)))
                    cnt++;
            }
            CL_GC_UNPROTECT(1);
        } else {
            for (i = sa.start; i < end; i++) {
                if (seq_pred_match_sa(args[0], &sa, seq_elt(args[1], i)))
                    cnt++;
            }
        }
    }
    return CL_MAKE_FIXNUM(cnt);
}

static CL_Obj bi_count_if_not(CL_Obj *args, int n)
{
    CL_Obj seq = args[1];
    SeqArgs sa;
    int32_t i, len, end, cnt = 0;

    cl_check_seq_keywords(args, n, 2, SK_FIND_IF_KEYS);
    parse_seq_args(args, n, 2, &sa);
    len = seq_length(seq);
    end = (sa.end < 0) ? len : sa.end;

    if (sa.from_end) {
        if (CL_CONS_P(seq) || CL_NULL_P(seq)) {
            /* GC-vector snapshot — see bi_count for why not platform_alloc */
            int32_t range = end - sa.start;
            if (range > 0) {
                CL_Obj vec = cl_make_vector((uint32_t)range);
                CL_Obj cur;
                int32_t j = 0;
                CL_GC_PROTECT(vec);
                cur = args[1];
                for (i = 0; i < end && !CL_NULL_P(cur); i++, cur = cl_cdr(cur)) {
                    if (i >= sa.start)
                        cl_vector_data((CL_Vector *)CL_OBJ_TO_PTR(vec))[j++] = cl_car(cur);
                }
                for (i = j - 1; i >= 0; i--) {
                    if (!seq_pred_match_sa(args[0], &sa,
                            cl_vector_data((CL_Vector *)CL_OBJ_TO_PTR(vec))[i]))
                        cnt++;
                }
                CL_GC_UNPROTECT(1);
            }
        } else {
            for (i = end - 1; i >= sa.start; i--) {
                if (!seq_pred_match_sa(args[0], &sa, seq_elt(args[1], i)))
                    cnt++;
            }
        }
    } else {
        if (CL_CONS_P(seq) || CL_NULL_P(seq)) {
            CL_Obj cur = seq;
            CL_GC_PROTECT(cur);
            for (i = 0; i < end && !CL_NULL_P(cur); i++, cur = cl_cdr(cur)) {
                if (i >= sa.start && !seq_pred_match_sa(args[0], &sa, cl_car(cur)))
                    cnt++;
            }
            CL_GC_UNPROTECT(1);
        } else {
            for (i = sa.start; i < end; i++) {
                if (!seq_pred_match_sa(args[0], &sa, seq_elt(args[1], i)))
                    cnt++;
            }
        }
    }
    return CL_MAKE_FIXNUM(cnt);
}

/* ======================================================= */
/* REMOVE / REMOVE-IF / REMOVE-IF-NOT / REMOVE-DUPLICATES */
/* ======================================================= */

/* Helper: build a result list from elements, skipping marked positions.
   Works for list sequences by collecting non-removed elements. */
static CL_Obj remove_from_list(CL_Obj seq, int32_t start, int32_t end,
                               int32_t count, int from_end,
                               CL_Obj item, CL_Obj test_fn, CL_Obj key_fn,
                               int mode) /* 0=test-item, 1=pred, 2=pred-not, 3=test-not-item */
{
    CL_Obj result = CL_NIL, tail = CL_NIL;
    CL_Obj cur = CL_NIL;
    int32_t i, removed = 0;

    CL_GC_PROTECT(result);
    CL_GC_PROTECT(tail);
    /* The input cursor `cur` walks `seq` while the loop body conses the
     * kept elements via cl_cons — an allocating call that, under a
     * compacting GC, relocates the list out from under `cur`, leaving it a
     * stale offset (the next cl_cdr(cur) then walks garbage).  Likewise
     * `item`/`test_fn`/`key_fn` are heap objects held by value across those
     * allocations (e.g. REMOVE'ing a class struct from a class's long
     * direct-subclasses list during DEFCLASS redefinition).  Protect them
     * all so the compactor forwards these locals. */
    CL_GC_PROTECT(cur);
    CL_GC_PROTECT(item);
    CL_GC_PROTECT(test_fn);
    CL_GC_PROTECT(key_fn);
    /* `seq` is re-read for the second from-end pass after the counting pass
     * has already run allocating apply_key/call_test calls — protect it so
     * that re-read doesn't start from a stale offset. */
    CL_GC_PROTECT(seq);

    if (from_end && count >= 0) {
        /* Two-pass: count matches from end */
        int32_t total_matches = 0;
        int32_t skip_count;
        cur = seq;
        for (i = 0; !CL_NULL_P(cur); i++, cur = cl_cdr(cur)) {
            if (i >= start && (end < 0 || i < end)) {
                CL_Obj elem = cl_car(cur);
                int match = 0;
                if (mode == 0) {
                    CL_Obj keyed = apply_key(key_fn, elem);
                    match = !CL_NULL_P(call_test(test_fn, item, keyed));
                } else if (mode == 3) {
                    CL_Obj keyed = apply_key(key_fn, elem);
                    match = CL_NULL_P(call_test(test_fn, item, keyed));
                } else if (mode == 1) {
                    match = seq_pred_match(item, key_fn, elem);
                } else {
                    match = !seq_pred_match(item, key_fn, elem);
                }
                if (match) total_matches++;
            }
        }
        /* Skip first (total_matches - count) matches from the front */
        skip_count = total_matches - count;
        if (skip_count < 0) skip_count = 0;

        cur = seq;
        for (i = 0; !CL_NULL_P(cur); i++, cur = cl_cdr(cur)) {
            CL_Obj elem = cl_car(cur);
            int match = 0;
            if (i >= start && (end < 0 || i < end)) {
                if (mode == 0) {
                    CL_Obj keyed = apply_key(key_fn, elem);
                    match = !CL_NULL_P(call_test(test_fn, item, keyed));
                } else if (mode == 3) {
                    CL_Obj keyed = apply_key(key_fn, elem);
                    match = CL_NULL_P(call_test(test_fn, item, keyed));
                } else if (mode == 1) {
                    match = seq_pred_match(item, key_fn, elem);
                } else {
                    match = !seq_pred_match(item, key_fn, elem);
                }
            }
            if (match) {
                if (skip_count > 0) {
                    skip_count--;
                    match = 0; /* keep this one */
                }
            }
            if (!match) {
                CL_Obj cell;
                /* Re-read elem: the match test above ran allocating
                 * apply_key/call_test — `cur` is a forwarded root, the
                 * pre-test elem copy may be a stale offset. */
                elem = cl_car(cur);
                cell = cl_cons(elem, CL_NIL);
                if (CL_NULL_P(result)) result = cell;
                else ((CL_Cons *)CL_OBJ_TO_PTR(tail))->cdr = cell;
                tail = cell;
            }
        }
    } else {
        /* Forward removal */
        cur = seq;
        for (i = 0; !CL_NULL_P(cur); i++, cur = cl_cdr(cur)) {
            CL_Obj elem = cl_car(cur);
            int should_remove = 0;

            if (i >= start && (end < 0 || i < end) && (count < 0 || removed < count)) {
                if (mode == 0) {
                    CL_Obj keyed = apply_key(key_fn, elem);
                    should_remove = !CL_NULL_P(call_test(test_fn, item, keyed));
                } else if (mode == 3) {
                    CL_Obj keyed = apply_key(key_fn, elem);
                    should_remove = CL_NULL_P(call_test(test_fn, item, keyed));
                } else if (mode == 1) {
                    should_remove = seq_pred_match(item, key_fn, elem);
                } else {
                    should_remove = !seq_pred_match(item, key_fn, elem);
                }
            }

            if (should_remove) {
                removed++;
            } else {
                CL_Obj cell;
                /* Re-read elem after the allocating match test (see the
                 * from-end pass above). */
                elem = cl_car(cur);
                cell = cl_cons(elem, CL_NIL);
                if (CL_NULL_P(result)) result = cell;
                else ((CL_Cons *)CL_OBJ_TO_PTR(tail))->cdr = cell;
                tail = cell;
            }
        }
    }

    CL_GC_UNPROTECT(7); /* seq, key_fn, test_fn, item, cur, tail, result */
    return result;
}

/* Decide whether the element at one list position matches, for the destructive
 * list path.  Mirrors the per-element test in remove_from_list. */
static int del_elem_match(int mode, CL_Obj item, CL_Obj test_fn,
                          CL_Obj key_fn, CL_Obj elem)
{
    if (mode == 0) {
        CL_Obj keyed = apply_key(key_fn, elem);
        return !CL_NULL_P(call_test(test_fn, item, keyed));
    } else if (mode == 3) {
        CL_Obj keyed = apply_key(key_fn, elem);
        return CL_NULL_P(call_test(test_fn, item, keyed));
    } else if (mode == 1) {
        return seq_pred_match(item, key_fn, elem);
    }
    return !seq_pred_match(item, key_fn, elem);
}

/* Destructive list delete (CLHS DELETE/DELETE-IF/DELETE-IF-NOT for lists):
 * splice matching conses out of `seq` in place by relinking cdr pointers and
 * return the (possibly new) head.  Same argument shape and mode codes as
 * remove_from_list.  Callers must treat the input as consumed and use the
 * returned value (delete "might destroy" its argument — and here it does, so
 * code that holds an alias to an interior cons sees the splice, as ANSI and
 * other implementations allow). */
static CL_Obj delete_from_list(CL_Obj seq, int32_t start, int32_t end,
                               int32_t count, int from_end,
                               CL_Obj item, CL_Obj test_fn, CL_Obj key_fn,
                               int mode)
{
    CL_Obj head = seq, prev = CL_NIL, cur = CL_NIL;
    int32_t i, removed = 0, skip_count = 0;

    /* No allocation happens here, but call_test/apply_key may run user code
     * that allocates and triggers compaction; protect every live offset. */
    CL_GC_PROTECT(head);
    CL_GC_PROTECT(prev);
    CL_GC_PROTECT(cur);
    CL_GC_PROTECT(item);
    CL_GC_PROTECT(test_fn);
    CL_GC_PROTECT(key_fn);

    if (from_end && count >= 0) {
        /* First pass: count total matches in range, so we keep all but the
         * last `count` of them (skip the leading skip_count matches). */
        int32_t total_matches = 0;
        cur = seq;
        for (i = 0; !CL_NULL_P(cur); i++, cur = cl_cdr(cur)) {
            if (i >= start && (end < 0 || i < end) &&
                del_elem_match(mode, item, test_fn, key_fn, cl_car(cur)))
                total_matches++;
        }
        skip_count = total_matches - count;
        if (skip_count < 0) skip_count = 0;
    }

    cur = seq;
    i = 0;
    while (!CL_NULL_P(cur)) {
        int should_remove = 0;

        if (i >= start && (end < 0 || i < end) &&
            (from_end || count < 0 || removed < count)) {
            if (del_elem_match(mode, item, test_fn, key_fn, cl_car(cur))) {
                if (from_end && count >= 0) {
                    if (skip_count > 0) skip_count--;   /* keep this one */
                    else should_remove = 1;
                } else {
                    should_remove = 1;
                }
            }
        }

        /* Read next AFTER del_elem_match: apply_key/call_test may compact,
         * making a pre-read next stale. cur is GC-protected so it's valid. */
        {
            CL_Obj next = cl_cdr(cur);
            if (should_remove) {
                removed++;
                if (CL_NULL_P(prev))
                    head = next;
                else
                    ((CL_Cons *)CL_OBJ_TO_PTR(prev))->cdr = next;
                /* prev stays put; cur advances */
            } else {
                prev = cur;
            }
            cur = next;
        }
        i++;
    }

    CL_GC_UNPROTECT(6); /* key_fn, test_fn, item, cur, prev, head */
    return head;
}

CL_Obj bi_remove_export(CL_Obj *args, int n);
CL_Obj bi_remove_if_export(CL_Obj *args, int n);
CL_Obj bi_remove_if_not_export(CL_Obj *args, int n);
CL_Obj bi_delete_export(CL_Obj *args, int n);
CL_Obj bi_delete_if_export(CL_Obj *args, int n);
CL_Obj bi_delete_if_not_export(CL_Obj *args, int n);

/* DELETE: destructive on lists, otherwise same as REMOVE. */
CL_Obj bi_delete_export(CL_Obj *args, int n)
{
    CL_Obj item = args[0], seq = args[1];
    SeqArgs sa;
    cl_check_seq_keywords(args, n, 2, SK_ALL);
    parse_seq_args(args, n, 2, &sa);

    if (CL_NULL_P(seq)) return CL_NIL;
    if (CL_CONS_P(seq)) {
        int32_t len = seq_length(seq);
        int32_t end = (sa.end < 0) ? len : sa.end;
        if (!CL_NULL_P(sa.test_not_fn))
            return delete_from_list(seq, sa.start, end, sa.count, sa.from_end,
                                    item, sa.test_not_fn, sa.key_fn, 3);
        return delete_from_list(seq, sa.start, end, sa.count, sa.from_end,
                                item, sa.test_fn, sa.key_fn, 0);
    }
    /* Non-list sequences: REMOVE's allocating paths are already the
     * standard-permitted behaviour. */
    return bi_remove_export(args, n);
}

CL_Obj bi_delete_if_export(CL_Obj *args, int n)
{
    CL_Obj pred = args[0], seq = args[1];
    SeqArgs sa;
    cl_check_seq_keywords(args, n, 2, SK_ALL);
    parse_seq_args(args, n, 2, &sa);

    if (CL_NULL_P(seq)) return CL_NIL;
    if (CL_CONS_P(seq)) {
        int32_t len = seq_length(seq);
        int32_t end = (sa.end < 0) ? len : sa.end;
        return delete_from_list(seq, sa.start, end, sa.count, sa.from_end,
                                pred, CL_NIL, sa.key_fn, 1);
    }
    return bi_remove_if_export(args, n);
}

CL_Obj bi_delete_if_not_export(CL_Obj *args, int n)
{
    CL_Obj pred = args[0], seq = args[1];
    SeqArgs sa;
    cl_check_seq_keywords(args, n, 2, SK_ALL);
    parse_seq_args(args, n, 2, &sa);

    if (CL_NULL_P(seq)) return CL_NIL;
    if (CL_CONS_P(seq)) {
        int32_t len = seq_length(seq);
        int32_t end = (sa.end < 0) ? len : sa.end;
        return delete_from_list(seq, sa.start, end, sa.count, sa.from_end,
                                pred, CL_NIL, sa.key_fn, 2);
    }
    return bi_remove_if_not_export(args, n);
}

/* remove_from_string: shared string path for remove/remove-if/remove-if-not.
   mode: 0=remove (item+test), 1=remove-if (pred), 2=remove-if-not (pred) */
static int remove_str_match(int mode, CL_Obj test_fn, CL_Obj key_fn,
                            CL_Obj item_or_pred, CL_Obj elem)
{
    /* Pred modes go through seq_pred_match (which applies :key itself and
     * GC-protects its copies) — the old code applied :key here, DISCARDED
     * the result, and passed a then-stale elem to the predicate. */
    if (mode == 1) return seq_pred_match(item_or_pred, key_fn, elem);
    if (mode == 2) return !seq_pred_match(item_or_pred, key_fn, elem);
    {
        /* mode 0: item + :test.  GC SAFETY: apply_key can compact — hold
         * test_fn/item across it. */
        CL_Obj keyed;
        int r;
        CL_GC_PROTECT(test_fn);
        CL_GC_PROTECT(item_or_pred);
        keyed = apply_key(key_fn, elem);
        r = !CL_NULL_P(call_test(test_fn, item_or_pred, keyed));
        CL_GC_UNPROTECT(2);
        return r;
    }
}

static CL_Obj remove_from_string(CL_Obj seq, CL_Obj item_or_pred,
                                  CL_Obj test_fn, CL_Obj key_fn,
                                  int32_t start, int32_t end,
                                  int32_t count, int from_end, int mode)
{
    int32_t slen = (int32_t)cl_string_length(seq);
    int32_t out = 0, i, removed = 0;
    int32_t skip_matches = 0;   /* leading matches to KEEP for :from-end + :count */
    CL_Obj keep_vec;
    CL_Obj result;

    if (end < 0) end = slen;

    /* GC SAFETY: the :test/:key/pred calls can compact — seq (read via
     * cl_string_char_at each iteration) and the fn/item parameter copies
     * would otherwise go stale after the first user call. */
    CL_GC_PROTECT(seq);
    CL_GC_PROTECT(item_or_pred);
    CL_GC_PROTECT(test_fn);
    CL_GC_PROTECT(key_fn);

    /* Kept characters are staged in a GC vector, not a fixed C buffer: a
     * 1024-byte buffer silently truncated results past 1023 chars, and its
     * (char) narrowing mangled wide characters.  Character objects are
     * immediates, so storing them never allocates; the vector is reclaimed
     * by GC even if a user :test/:key function longjmps (handler-case). */
    keep_vec = cl_make_vector((uint32_t)slen);
    CL_GC_PROTECT(keep_vec);

    /* :from-end with a :count removes the LAST `count` matches.  Count all
     * matches in [start,end) first, then keep the leading (total - count). */
    if (from_end && count >= 0) {
        int32_t total = 0;
        for (i = start; i < end && i < slen; i++) {
            CL_Obj elem = CL_MAKE_CHAR(cl_string_char_at(seq, (uint32_t)i));
            if (remove_str_match(mode, test_fn, key_fn, item_or_pred, elem))
                total++;
        }
        skip_matches = total - count;
        if (skip_matches < 0) skip_matches = 0;
    }

    for (i = 0; i < slen; i++) {
        CL_Obj elem = CL_MAKE_CHAR(cl_string_char_at(seq, (uint32_t)i));
        int should_remove = 0;
        int gate = from_end ? 1 : (count < 0 || removed < count);
        if (i >= start && i < end && gate &&
            remove_str_match(mode, test_fn, key_fn, item_or_pred, elem)) {
            if (from_end && count >= 0) {
                if (skip_matches > 0) skip_matches--;  /* keep this leading match */
                else should_remove = 1;
            } else {
                should_remove = 1;
            }
        }
        if (should_remove) {
            removed++;
        } else {
            /* Re-derive the data pointer per store: the match call above
             * can compact and move keep_vec. */
            CL_Vector *kv = (CL_Vector *)CL_OBJ_TO_PTR(keep_vec);
            cl_vector_data(kv)[out++] = elem;
        }
    }

    /* Build the result from the kept characters, preserving width. */
    {
        CL_Vector *kv = (CL_Vector *)CL_OBJ_TO_PTR(keep_vec);
        CL_Obj *kd = cl_vector_data(kv);
#ifdef CL_WIDE_STRINGS
        int wide = 0;
        for (i = 0; i < out; i++) {
            if (CL_CHAR_VAL(kd[i]) > 0x7F) { wide = 1; break; }
        }
        result = wide ? cl_make_wide_string(NULL, (uint32_t)out)
                      : cl_make_string(NULL, (uint32_t)out);
#else
        result = cl_make_string(NULL, (uint32_t)out);
#endif
        /* Re-derive after the result allocation, then fill (no allocation
         * in this loop, so kd stays valid). */
        kv = (CL_Vector *)CL_OBJ_TO_PTR(keep_vec);
        kd = cl_vector_data(kv);
        for (i = 0; i < out; i++)
            cl_string_set_char_at(result, (uint32_t)i, CL_CHAR_VAL(kd[i]));
    }
    CL_GC_UNPROTECT(5);  /* seq, item_or_pred, test_fn, key_fn, keep_vec */
    return result;
}

/* Keep-flag arrays for the remove/remove-duplicates family are staged in a
 * GC bit-vector, not platform_alloc memory: the flag array is held across
 * user :test/:key calls, and if one of those longjmps (handler-case) a
 * platform_alloc buffer leaks permanently — a GC object is simply collected.
 * The macros re-derive the object pointer on every access because those same
 * user calls can compact and move the bit-vector. */
#define KEEP_BV_SET(obj, i, b) \
    cl_bv_set_bit((CL_BitVector *)CL_OBJ_TO_PTR(obj), (uint32_t)(i), (b))
#define KEEP_BV_GET(obj, i) \
    cl_bv_get_bit((CL_BitVector *)CL_OBJ_TO_PTR(obj), (uint32_t)(i))

/* remove_from_bitvector: shared bit-vector path for remove/remove-if/remove-if-not.
   mode: 0=test-item, 1=pred, 2=pred-not, 3=test-not-item */
static CL_Obj remove_from_bitvector(CL_Obj seq, int32_t start, int32_t end,
                                     int32_t count, int from_end,
                                     CL_Obj item, CL_Obj test_fn, CL_Obj key_fn,
                                     int mode)
{
    CL_BitVector *bv = (CL_BitVector *)CL_OBJ_TO_PTR(seq);
    int32_t bvlen = (int32_t)cl_bv_active_length(bv);
    int32_t i, out = 0, removed = 0;
    CL_Obj keep;
    CL_Obj result;

    if (end < 0) end = bvlen;
    if (bvlen == 0) return cl_make_bit_vector(0);

    /* GC SAFETY: same as remove_from_vector — the user :test/:key calls can
     * compact, and seq/item/test_fn/key_fn are re-read across them. */
    CL_GC_PROTECT(seq);
    CL_GC_PROTECT(item);
    CL_GC_PROTECT(test_fn);
    CL_GC_PROTECT(key_fn);

    keep = cl_make_bit_vector((uint32_t)bvlen);
    CL_GC_PROTECT(keep);
    for (i = 0; i < bvlen; i++)
        KEEP_BV_SET(keep, i, 1);

    if (from_end && count >= 0) {
        /* Two-pass: count total matches, then skip first (total-count) from front */
        int32_t total_matches = 0, skip_count;
        for (i = 0; i < bvlen; i++) {
            if (i >= start && i < end) {
                CL_Obj elem, keyed;
                int match;
                bv = (CL_BitVector *)CL_OBJ_TO_PTR(seq);
                elem = CL_MAKE_FIXNUM(cl_bv_get_bit(bv, (uint32_t)i));
                keyed = apply_key(key_fn, elem);
                if (mode == 0)
                    match = !CL_NULL_P(call_test(test_fn, item, keyed));
                else if (mode == 3)
                    match = CL_NULL_P(call_test(test_fn, item, keyed));
                else if (mode == 1)
                    match = !CL_NULL_P(call_1(item, keyed));
                else
                    match = CL_NULL_P(call_1(item, keyed));
                if (match) total_matches++;
            }
        }
        skip_count = total_matches - count;
        if (skip_count < 0) skip_count = 0;
        for (i = 0; i < bvlen; i++) {
            if (i >= start && i < end) {
                CL_Obj elem, keyed;
                int match;
                bv = (CL_BitVector *)CL_OBJ_TO_PTR(seq);
                elem = CL_MAKE_FIXNUM(cl_bv_get_bit(bv, (uint32_t)i));
                keyed = apply_key(key_fn, elem);
                if (mode == 0)
                    match = !CL_NULL_P(call_test(test_fn, item, keyed));
                else if (mode == 3)
                    match = CL_NULL_P(call_test(test_fn, item, keyed));
                else if (mode == 1)
                    match = !CL_NULL_P(call_1(item, keyed));
                else
                    match = CL_NULL_P(call_1(item, keyed));
                if (match) {
                    if (skip_count > 0) {
                        skip_count--;
                    } else {
                        KEEP_BV_SET(keep, i, 0);
                    }
                }
            }
        }
    } else {
        /* Forward removal */
        for (i = 0; i < bvlen; i++) {
            int should_remove = 0;
            if (i >= start && i < end && (count < 0 || removed < count)) {
                CL_Obj elem, keyed;
                bv = (CL_BitVector *)CL_OBJ_TO_PTR(seq);
                elem = CL_MAKE_FIXNUM(cl_bv_get_bit(bv, (uint32_t)i));
                keyed = apply_key(key_fn, elem);
                if (mode == 0)
                    should_remove = !CL_NULL_P(call_test(test_fn, item, keyed));
                else if (mode == 3)
                    should_remove = CL_NULL_P(call_test(test_fn, item, keyed));
                else if (mode == 1)
                    should_remove = !CL_NULL_P(call_1(item, keyed));
                else
                    should_remove = CL_NULL_P(call_1(item, keyed));
            }
            if (should_remove) {
                KEEP_BV_SET(keep, i, 0);
                removed++;
            }
        }
    }

    /* Count surviving bits */
    for (i = 0; i < bvlen; i++)
        if (KEEP_BV_GET(keep, i)) out++;

    result = cl_make_bit_vector((uint32_t)out);
    bv = (CL_BitVector *)CL_OBJ_TO_PTR(seq);
    {
        CL_BitVector *rbv = (CL_BitVector *)CL_OBJ_TO_PTR(result);
        int32_t j = 0;
        for (i = 0; i < bvlen; i++) {
            if (KEEP_BV_GET(keep, i)) {
                if (cl_bv_get_bit(bv, (uint32_t)i))
                    cl_bv_set_bit(rbv, (uint32_t)j, 1);
                j++;
            }
        }
    }

    CL_GC_UNPROTECT(5);  /* seq, item, test_fn, key_fn, keep */
    return result;
}

/* remove_from_vector: shared vector path for remove/remove-if/remove-if-not.
   mode: 0=test-item, 1=pred, 2=pred-not, 3=test-not-item */
static CL_Obj remove_from_vector(CL_Obj seq, int32_t start, int32_t end,
                                  int32_t count, int from_end,
                                  CL_Obj item, CL_Obj test_fn, CL_Obj key_fn,
                                  int mode)
{
    int32_t vlen;
    /* The caller has already ruled out lists, strings, bit-vectors and NIL,
     * then routed everything else here.  Packed byte vectors are handled via
     * the generic arr_seq_get/arr_seq_set accessors.  Anything else (number,
     * multidim array, hash-table, structure, ...) is not a sequence:
     * dereferencing it as a vector would read a wild offset and crash.
     * Signal a type-error per CLHS instead (REMOVE et al. require a sequence
     * designator). */
    if (!CL_VECTOR_P(seq) && !CL_BYTE_VECTOR_P(seq))
        cl_signal_type_error(seq, "SEQUENCE", "REMOVE");
    vlen = CL_BYTE_VECTOR_P(seq)
        ? (int32_t)cl_bytevec_active_length((CL_ByteVector *)CL_OBJ_TO_PTR(seq))
        : (int32_t)cl_vector_active_length((CL_Vector *)CL_OBJ_TO_PTR(seq));
    int32_t i, out = 0, removed = 0;
    CL_Obj keep;
    CL_Obj result;

    if (end < 0) end = vlen;
    if (vlen == 0) return make_seq_result_like(seq, 0);

    /* GC SAFETY: apply_key/call_test/call_1 run user code that can compact.
     * seq is re-dereferenced every iteration and item/test_fn/key_fn are
     * re-read — all four are parameter copies invisible to the GC without
     * these roots (a stale seq makes the v re-derivation read garbage). */
    CL_GC_PROTECT(seq);
    CL_GC_PROTECT(item);
    CL_GC_PROTECT(test_fn);
    CL_GC_PROTECT(key_fn);

    keep = cl_make_bit_vector((uint32_t)vlen);
    CL_GC_PROTECT(keep);
    for (i = 0; i < vlen; i++)
        KEEP_BV_SET(keep, i, 1);

    if (from_end && count >= 0) {
        int32_t total_matches = 0, skip_count;
        for (i = 0; i < vlen; i++) {
            if (i >= start && i < end) {
                CL_Obj elem, keyed;
                int match;
                elem = arr_seq_get(seq, i);
                keyed = apply_key(key_fn, elem);
                if (mode == 0)
                    match = !CL_NULL_P(call_test(test_fn, item, keyed));
                else if (mode == 3)
                    match = CL_NULL_P(call_test(test_fn, item, keyed));
                else if (mode == 1)
                    match = !CL_NULL_P(call_1(item, keyed));
                else
                    match = CL_NULL_P(call_1(item, keyed));
                if (match) total_matches++;
            }
        }
        skip_count = total_matches - count;
        if (skip_count < 0) skip_count = 0;
        for (i = 0; i < vlen; i++) {
            if (i >= start && i < end) {
                CL_Obj elem, keyed;
                int match;
                elem = arr_seq_get(seq, i);
                keyed = apply_key(key_fn, elem);
                if (mode == 0)
                    match = !CL_NULL_P(call_test(test_fn, item, keyed));
                else if (mode == 3)
                    match = CL_NULL_P(call_test(test_fn, item, keyed));
                else if (mode == 1)
                    match = !CL_NULL_P(call_1(item, keyed));
                else
                    match = CL_NULL_P(call_1(item, keyed));
                if (match) {
                    if (skip_count > 0) skip_count--;
                    else KEEP_BV_SET(keep, i, 0);
                }
            }
        }
    } else {
        for (i = 0; i < vlen; i++) {
            int should_remove = 0;
            if (i >= start && i < end && (count < 0 || removed < count)) {
                CL_Obj elem, keyed;
                elem = arr_seq_get(seq, i);
                keyed = apply_key(key_fn, elem);
                if (mode == 0)
                    should_remove = !CL_NULL_P(call_test(test_fn, item, keyed));
                else if (mode == 3)
                    should_remove = CL_NULL_P(call_test(test_fn, item, keyed));
                else if (mode == 1)
                    should_remove = !CL_NULL_P(call_1(item, keyed));
                else
                    should_remove = CL_NULL_P(call_1(item, keyed));
            }
            if (should_remove) {
                KEEP_BV_SET(keep, i, 0);
                removed++;
            }
        }
    }

    for (i = 0; i < vlen; i++)
        if (KEEP_BV_GET(keep, i)) out++;

    /* A string-vector (adjustable/fill-pointer character array) must yield a
     * STRING, not a general (vector character), and a packed byte vector a
     * packed byte vector; make_seq_result_like picks the class from SEQ.
     * arr_seq_get/arr_seq_set perform no allocation and re-derive the heap
     * pointers on every call; seq is still protected (function-wide root) so
     * compaction forwards it. */
    result = make_seq_result_like(seq, (uint32_t)out);
    {
        int32_t j = 0;
        for (i = 0; i < vlen; i++) {
            if (KEEP_BV_GET(keep, i))
                arr_seq_set(result, j++, arr_seq_get(seq, i));
        }
    }

    CL_GC_UNPROTECT(5);  /* seq, item, test_fn, key_fn, keep */
    return result;
}

CL_Obj bi_remove_export(CL_Obj *args, int n);
CL_Obj bi_remove_if_export(CL_Obj *args, int n);
CL_Obj bi_remove_if_not_export(CL_Obj *args, int n);
static CL_Obj bi_remove(CL_Obj *args, int n);
static CL_Obj bi_remove_if(CL_Obj *args, int n);
static CL_Obj bi_remove_if_not(CL_Obj *args, int n);

CL_Obj bi_remove_export(CL_Obj *args, int n) { return bi_remove(args, n); }
CL_Obj bi_remove_if_export(CL_Obj *args, int n) { return bi_remove_if(args, n); }
CL_Obj bi_remove_if_not_export(CL_Obj *args, int n) { return bi_remove_if_not(args, n); }

static CL_Obj bi_remove(CL_Obj *args, int n)
{
    CL_Obj item = args[0], seq = args[1];
    SeqArgs sa;
    cl_check_seq_keywords(args, n, 2, SK_ALL);
    parse_seq_args(args, n, 2, &sa);

    if (CL_NULL_P(seq)) return CL_NIL;
    if (CL_CONS_P(seq)) {
        int32_t len = seq_length(seq);
        int32_t end = (sa.end < 0) ? len : sa.end;
        if (!CL_NULL_P(sa.test_not_fn))
            return remove_from_list(seq, sa.start, end, sa.count, sa.from_end,
                                    item, sa.test_not_fn, sa.key_fn, 3);
        return remove_from_list(seq, sa.start, end, sa.count, sa.from_end,
                                item, sa.test_fn, sa.key_fn, 0);
    }
    if (CL_ANY_STRING_P(seq)) {
        return remove_from_string(seq, item, sa.test_fn, sa.key_fn,
                                  sa.start, sa.end, sa.count, sa.from_end, 0);
    }
    if (CL_BIT_VECTOR_P(seq)) {
        if (!CL_NULL_P(sa.test_not_fn))
            return remove_from_bitvector(seq, sa.start, sa.end, sa.count, sa.from_end,
                                         item, sa.test_not_fn, sa.key_fn, 3);
        return remove_from_bitvector(seq, sa.start, sa.end, sa.count, sa.from_end,
                                     item, sa.test_fn, sa.key_fn, 0);
    }
    /* Vector path */
    if (!CL_NULL_P(sa.test_not_fn))
        return remove_from_vector(seq, sa.start, sa.end, sa.count, sa.from_end,
                                  item, sa.test_not_fn, sa.key_fn, 3);
    return remove_from_vector(seq, sa.start, sa.end, sa.count, sa.from_end,
                              item, sa.test_fn, sa.key_fn, 0);
}

static CL_Obj bi_remove_if(CL_Obj *args, int n)
{
    CL_Obj pred = args[0], seq = args[1];
    SeqArgs sa;
    cl_check_seq_keywords(args, n, 2, SK_ALL);
    parse_seq_args(args, n, 2, &sa);

    if (CL_NULL_P(seq)) return CL_NIL;
    if (CL_CONS_P(seq)) {
        int32_t len = seq_length(seq);
        int32_t end = (sa.end < 0) ? len : sa.end;
        return remove_from_list(seq, sa.start, end, sa.count, sa.from_end,
                                pred, CL_NIL, sa.key_fn, 1);
    }
    if (CL_ANY_STRING_P(seq)) {
        return remove_from_string(seq, pred, CL_NIL, sa.key_fn,
                                  sa.start, sa.end, sa.count, sa.from_end, 1);
    }
    if (CL_BIT_VECTOR_P(seq)) {
        return remove_from_bitvector(seq, sa.start, sa.end, sa.count, sa.from_end,
                                     pred, CL_NIL, sa.key_fn, 1);
    }
    /* Vector path */
    return remove_from_vector(seq, sa.start, sa.end, sa.count, sa.from_end,
                              pred, CL_NIL, sa.key_fn, 1);
}

static CL_Obj bi_remove_if_not(CL_Obj *args, int n)
{
    CL_Obj pred = args[0], seq = args[1];
    SeqArgs sa;
    cl_check_seq_keywords(args, n, 2, SK_ALL);
    parse_seq_args(args, n, 2, &sa);

    if (CL_NULL_P(seq)) return CL_NIL;
    if (CL_CONS_P(seq)) {
        int32_t len = seq_length(seq);
        int32_t end = (sa.end < 0) ? len : sa.end;
        return remove_from_list(seq, sa.start, end, sa.count, sa.from_end,
                                pred, CL_NIL, sa.key_fn, 2);
    }
    if (CL_ANY_STRING_P(seq)) {
        return remove_from_string(seq, pred, CL_NIL, sa.key_fn,
                                  sa.start, sa.end, sa.count, sa.from_end, 2);
    }
    if (CL_BIT_VECTOR_P(seq)) {
        return remove_from_bitvector(seq, sa.start, sa.end, sa.count, sa.from_end,
                                     pred, CL_NIL, sa.key_fn, 2);
    }
    /* Vector path */
    return remove_from_vector(seq, sa.start, sa.end, sa.count, sa.from_end,
                              pred, CL_NIL, sa.key_fn, 2);
}

/* Compare two raw elements for the duplicate test (applying :key to both). */
static int rd_match(SeqArgs *sa, CL_Obj a, CL_Obj b)
{
    CL_Obj ka, kb;
    int m;
    /* GC SAFETY: root b and the SeqArgs fn fields (via the caller's struct)
     * across the user :key/:test calls — see seq_test_match. */
    CL_GC_PROTECT(b);
    CL_GC_PROTECT(sa->test_fn);
    CL_GC_PROTECT(sa->test_not_fn);
    CL_GC_PROTECT(sa->key_fn);
    ka = apply_key(sa->key_fn, a);
    CL_GC_PROTECT(ka);
    kb = apply_key(sa->key_fn, b);
    if (!CL_NULL_P(sa->test_not_fn))
        m = CL_NULL_P(call_test(sa->test_not_fn, ka, kb));
    else
        m = !CL_NULL_P(call_test(sa->test_fn, ka, kb));
    CL_GC_UNPROTECT(5);
    return m;
}

static CL_Obj bi_remove_duplicates(CL_Obj *args, int n)
{
    CL_Obj seq = args[0];
    SeqArgs sa;
    CL_Obj result = CL_NIL, tail = CL_NIL, tmp = CL_NIL;
    int32_t i, j, len, end, kept = 0;
    int is_list;

    cl_check_seq_keywords(args, n, 1, SK_FIND_KEYS);
    parse_seq_args(args, n, 1, &sa);
    if (sa.start < 0) sa.start = 0;
    len = seq_length(seq);
    end = (sa.end < 0 || sa.end > len) ? len : sa.end;
    is_list = (CL_CONS_P(seq) || CL_NULL_P(seq));
    if (!is_list && !seq_is_array(seq))
        cl_signal_type_error(seq, "SEQUENCE", "REMOVE-DUPLICATES");

    CL_GC_PROTECT(seq);   /* local copy of args[0]; protect across allocations */
    CL_GC_PROTECT(result);
    CL_GC_PROTECT(tail);
    CL_GC_PROTECT(tmp);

    /* Snapshot elements into a GC-rooted vector so the :test/:key calls cannot
     * leave us reading stale arena offsets. */
    tmp = cl_make_vector((uint32_t)(len > 0 ? len : 0));
    if (is_list) {
        CL_Obj cur = seq;
        for (i = 0; i < len && !CL_NULL_P(cur); i++, cur = cl_cdr(cur))
            cl_vector_data((CL_Vector *)CL_OBJ_TO_PTR(tmp))[i] = cl_car(cur);
    } else {
        for (i = 0; i < len; i++)
            cl_vector_data((CL_Vector *)CL_OBJ_TO_PTR(tmp))[i] = arr_seq_get(seq, i);
    }

    /* Decide which indices to keep.  Within [start,end), with :from-end NIL the
     * last of a set of duplicates is kept (an element is dropped if it recurs
     * later); with :from-end T the first is kept (dropped if it recurs earlier).
     * Indices outside [start,end) are always kept. */
    {
        /* GC bit-vector, not platform_alloc: the flags live across rd_match
         * user code, and a handler-case longjmp out of it would leak a C
         * allocation permanently (see KEEP_BV_SET). */
        CL_Obj keep = cl_make_bit_vector((uint32_t)(len > 0 ? len : 1));
        CL_GC_PROTECT(keep);
        for (i = 0; i < len; i++) KEEP_BV_SET(keep, i, 1);
        for (i = sa.start; i < end; i++) {
            /* Read both elements fresh from the protected snapshot on every
             * comparison — rd_match runs user code that can compact, which
             * would leave a pre-read `ei` stale for the next j iteration. */
            if (!sa.from_end) {
                for (j = i + 1; j < end; j++) {
                    if (KEEP_BV_GET(keep, j) && rd_match(&sa, arr_seq_get(tmp, i),
                                            arr_seq_get(tmp, j))) { KEEP_BV_SET(keep, i, 0); break; }
                }
            } else {
                for (j = sa.start; j < i; j++) {
                    if (KEEP_BV_GET(keep, j) && rd_match(&sa, arr_seq_get(tmp, i),
                                            arr_seq_get(tmp, j))) { KEEP_BV_SET(keep, i, 0); break; }
                }
            }
        }
        for (i = 0; i < len; i++) if (KEEP_BV_GET(keep, i)) kept++;

        if (is_list) {
            CL_Obj elem = CL_NIL;
            CL_GC_PROTECT(elem);
            for (i = 0; i < len; i++) {
                if (KEEP_BV_GET(keep, i)) {
                    CL_Obj cell;
                    elem = arr_seq_get(tmp, i);
                    cell = cl_cons(elem, CL_NIL);
                    if (CL_NULL_P(result)) result = cell;
                    else ((CL_Cons *)CL_OBJ_TO_PTR(tail))->cdr = cell;
                    tail = cell;
                }
            }
            CL_GC_UNPROTECT(1);
        } else {
            int32_t k = 0;
            result = make_seq_result_like(seq, (uint32_t)kept);
            for (i = 0; i < len; i++)
                if (KEEP_BV_GET(keep, i)) arr_seq_set(result, k++, arr_seq_get(tmp, i));
        }
        CL_GC_UNPROTECT(1);  /* keep */
    }

    CL_GC_UNPROTECT(4);
    return result;
}

/* ======================================================= */
/* SUBSTITUTE / SUBSTITUTE-IF / SUBSTITUTE-IF-NOT          */
/* ======================================================= */

/* Unified match test for the substitute family.
 * mode: 0 = SUBSTITUTE (eql/:test against olditem),
 *       1 = SUBSTITUTE-IF (predicate), 2 = SUBSTITUTE-IF-NOT (negated). */
static int subst_match(int mode, SeqArgs *sa, CL_Obj olditem, CL_Obj predfn, CL_Obj elem)
{
    if (mode == 0) return seq_test_match(sa, olditem, elem);
    if (mode == 1) return seq_pred_match_sa(predfn, sa, elem);
    return !seq_pred_match_sa(predfn, sa, elem);
}

/* Core for SUBSTITUTE / SUBSTITUTE-IF / SUBSTITUTE-IF-NOT and their
 * destructive N* counterparts, over lists, vectors, strings and bit vectors.
 *
 * Elements are snapshotted into a heap vector first so the :test/:key calls
 * (which may allocate and trigger a compacting GC) only ever read through the
 * GC-rooted snapshot.  :from-end is honoured by scanning the snapshot in
 * reverse, which also gives the reverse test-call order the spec requires. */
static CL_Obj do_subst(int mode, int destructive, CL_Obj *args, int n)
{
    CL_Obj newitem = args[0];
    CL_Obj olditem = (mode == 0) ? args[1] : CL_NIL;
    CL_Obj predfn  = (mode == 0) ? CL_NIL : args[1];
    CL_Obj seq = args[2];
    SeqArgs sa;
    CL_Obj tmp = CL_NIL, result = CL_NIL, tail = CL_NIL, cur;
    int32_t i, len, end, replaced = 0;
    int is_list;

    cl_check_seq_keywords(args, n, 3,
        SK_KEY | SK_START | SK_END | SK_FROM_END | SK_COUNT |
        (mode == 0 ? (SK_TEST | SK_TEST_NOT) : 0));
    parse_seq_args(args, n, 3, &sa);
    if (sa.start < 0) sa.start = 0;
    len = seq_length(seq);
    end = (sa.end < 0 || sa.end > len) ? len : sa.end;
    is_list = (CL_CONS_P(seq) || CL_NULL_P(seq));
    if (!is_list && !seq_is_array(seq))
        cl_signal_type_error(seq, "SEQUENCE", "SUBSTITUTE");

    CL_GC_PROTECT(seq);   /* local copy of args[2]; protect across allocations */
    CL_GC_PROTECT(newitem);
    CL_GC_PROTECT(olditem);
    CL_GC_PROTECT(predfn);
    CL_GC_PROTECT(tmp);
    CL_GC_PROTECT(result);
    CL_GC_PROTECT(tail);

    /* Snapshot element values into a heap vector. */
    tmp = cl_make_vector((uint32_t)(len > 0 ? len : 0));
    if (is_list) {
        cur = seq;
        for (i = 0; i < len && !CL_NULL_P(cur); i++, cur = cl_cdr(cur))
            cl_vector_data((CL_Vector *)CL_OBJ_TO_PTR(tmp))[i] = cl_car(cur);
    } else {
        for (i = 0; i < len; i++)
            cl_vector_data((CL_Vector *)CL_OBJ_TO_PTR(tmp))[i] = arr_seq_get(seq, i);
    }

    /* Decide replacements, mutating the snapshot in place. */
    if (sa.from_end) {
        for (i = end - 1; i >= sa.start; i--) {
            if (sa.count >= 0 && replaced >= sa.count) break;
            if (subst_match(mode, &sa, olditem, predfn, arr_seq_get(tmp, i))) {
                arr_seq_set(tmp, i, newitem);
                replaced++;
            }
        }
    } else {
        for (i = sa.start; i < end; i++) {
            if (sa.count >= 0 && replaced >= sa.count) break;
            if (subst_match(mode, &sa, olditem, predfn, arr_seq_get(tmp, i))) {
                arr_seq_set(tmp, i, newitem);
                replaced++;
            }
        }
    }

    /* Produce the result. */
    if (destructive) {
        if (is_list) {
            cur = seq;
            for (i = 0; i < len && !CL_NULL_P(cur); i++, cur = cl_cdr(cur))
                ((CL_Cons *)CL_OBJ_TO_PTR(cur))->car = arr_seq_get(tmp, i);
        } else {
            for (i = sa.start; i < end; i++)
                arr_seq_set(seq, i, arr_seq_get(tmp, i));
        }
        result = seq;
    } else if (is_list) {
        CL_Obj elem = CL_NIL;
        CL_GC_PROTECT(elem);
        for (i = 0; i < len; i++) {
            CL_Obj cell;
            elem = arr_seq_get(tmp, i);
            cell = cl_cons(elem, CL_NIL);
            if (CL_NULL_P(result)) result = cell;
            else ((CL_Cons *)CL_OBJ_TO_PTR(tail))->cdr = cell;
            tail = cell;
        }
        CL_GC_UNPROTECT(1);
    } else {
        result = copy_array_seq(seq);
        for (i = sa.start; i < end; i++)
            arr_seq_set(result, i, arr_seq_get(tmp, i));
    }

    CL_GC_UNPROTECT(7);
    return result;
}

static CL_Obj bi_substitute(CL_Obj *args, int n)        { return do_subst(0, 0, args, n); }
static CL_Obj bi_substitute_if(CL_Obj *args, int n)     { return do_subst(1, 0, args, n); }
static CL_Obj bi_substitute_if_not(CL_Obj *args, int n) { return do_subst(2, 0, args, n); }

/* ======================================================= */
/* NSUBSTITUTE (destructive)                               */
/* ======================================================= */

static CL_Obj bi_nsubstitute(CL_Obj *args, int n)        { return do_subst(0, 1, args, n); }
static CL_Obj bi_nsubstitute_if(CL_Obj *args, int n)     { return do_subst(1, 1, args, n); }
static CL_Obj bi_nsubstitute_if_not(CL_Obj *args, int n) { return do_subst(2, 1, args, n); }

/* ======================================================= */
/* REDUCE                                                  */
/* ======================================================= */

static CL_Obj bi_reduce(CL_Obj *args, int n)
{
    CL_Obj func = args[0], seq = args[1];
    CL_Obj initial = CL_NIL;
    int has_initial = 0, from_end = 0;
    int32_t start = 0, end_val, len;
    CL_Obj key_fn = CL_NIL;
    CL_Obj accum;
    int i;

    cl_check_seq_keywords(args, n, 2,
        SK_KEY | SK_FROM_END | SK_START | SK_END | SK_INITIAL_VALUE);

    /* Parse keyword args manually.  CLHS 3.4.1.4: when a keyword is
     * supplied more than once, the leftmost pair is the one that takes
     * effect — so only record the first occurrence of each keyword. */
    {
        int seen_key = 0, seen_start = 0, seen_from_end = 0;
        for (i = 2; i + 1 < n; i += 2) {
            if (args[i] == KW_INITIAL_VALUE) {
                if (!has_initial) { initial = args[i + 1]; has_initial = 1; }
            } else if (args[i] == KW_KEY) {
                if (!seen_key) { key_fn = args[i + 1]; seen_key = 1; }
            } else if (args[i] == KW_START) {
                if (!seen_start) {
                    if (CL_FIXNUM_P(args[i + 1])) start = CL_FIXNUM_VAL(args[i + 1]);
                    seen_start = 1;
                }
            } else if (args[i] == KW_END) {
                /* handled below */
            } else if (args[i] == KW_FROM_END) {
                if (!seen_from_end) {
                    if (!CL_NULL_P(args[i + 1])) from_end = 1;
                    seen_from_end = 1;
                }
            }
        }
    }

    len = seq_length(seq);
    /* Re-scan for :end (leftmost wins) */
    end_val = len;
    for (i = 2; i + 1 < n; i += 2) {
        if (args[i] == KW_END) {
            if (!CL_NULL_P(args[i + 1]) && CL_FIXNUM_P(args[i + 1]))
                end_val = CL_FIXNUM_VAL(args[i + 1]);
            break;
        }
    }

    if (start >= end_val) {
        /* Empty subsequence: with an :initial-value return it unchanged,
         * otherwise call the reducing function with no arguments (CLHS). */
        if (has_initial) return initial;
        return call_0(func);
    }

    if (from_end) {
        /* :from-end T — process from right to left.
         * For right-associative reduction: f(e1, f(e2, f(e3, init)))
         * Arguments to f are: (element, accumulator) */
        int32_t sub_len = end_val - start;

        if (has_initial) {
            accum = initial;
        } else {
            accum = apply_key(key_fn, seq_elt(seq, end_val - 1));
            end_val--;
            sub_len--;
        }

        CL_GC_PROTECT(accum);
        CL_GC_PROTECT(func);
        CL_GC_PROTECT(key_fn);  /* stale otherwise once apply_key/call_test compacts */

        /* Test args[1], not the local seq: when there is no :initial-value the
         * apply_key above may already have compacted, staling the local (and a
         * stale offset can misclassify, sending a list down the random-access
         * path or vice versa). */
        if (!(CL_CONS_P(args[1]) || CL_NULL_P(args[1]))) {
            /* Any array sequence (vector / string / bit-vector): random access.
             * Read through args[1] each iteration — the VM-stack slot is a
             * forwarded root, unlike the local `seq` copy which goes stale
             * after the first compacting apply_key/call_test. */
            int32_t idx;
            for (idx = end_val - 1; idx >= start; idx--) {
                CL_Obj elem = apply_key(key_fn, seq_elt(args[1], idx));
                accum = call_test(func, elem, accum);
            }
        } else {
            /* List: collect elements into a GC-managed vector, then iterate
             * backwards.  A vector is a single heap object traced by the GC, so
             * its elements are forwarded across the compactions that
             * apply_key/call_test trigger — unlike a platform_alloc C array,
             * whose CL_Obj entries (arena offsets) would go stale. */
            CL_Obj vec = cl_make_vector((uint32_t)(sub_len < 0 ? 0 : sub_len));
            /* Seed the cursor from args[1] (a forwarded VM-stack slot), not the
             * local seq: both the cl_make_vector above and the no-initial-value
             * apply_key earlier can compact, leaving seq a stale offset. */
            CL_Obj cur = args[1];
            int32_t idx = 0, j = 0;
            CL_GC_PROTECT(vec);
            /* Collection does not allocate (cl_car/cl_cdr only), so `cur` and the
             * vector store are safe without further protection. */
            while (idx < start && !CL_NULL_P(cur)) { cur = cl_cdr(cur); idx++; }
            while (idx < end_val && !CL_NULL_P(cur)) {
                cl_vector_data((CL_Vector *)CL_OBJ_TO_PTR(vec))[j++] = cl_car(cur);
                cur = cl_cdr(cur);
                idx++;
            }
            for (idx = j - 1; idx >= 0; idx--) {
                /* Re-deref vec each iteration: a compaction in the prior
                 * call_test may have relocated the vector object. */
                CL_Obj elem = apply_key(key_fn,
                    cl_vector_data((CL_Vector *)CL_OBJ_TO_PTR(vec))[idx]);
                accum = call_test(func, elem, accum);
            }
            CL_GC_UNPROTECT(1); /* vec */
        }

        CL_GC_UNPROTECT(3); /* key_fn, func, accum */
        return accum;
    }

    /* Forward (default) reduction */
    if (has_initial) {
        accum = initial;
    } else {
        accum = apply_key(key_fn, seq_elt(seq, start));
        start++;
    }

    CL_GC_PROTECT(accum);
    CL_GC_PROTECT(func);
    CL_GC_PROTECT(key_fn);

    /* args[1], not the stale local seq: the no-initial-value apply_key above
     * may have compacted (same window as the :from-end path). */
    if (CL_CONS_P(args[1]) || CL_NULL_P(args[1])) {
        CL_Obj cur = args[1];
        int32_t idx = 0;
        /* GC-protect the list cursor: call_test (the reducing fn) and apply_key
         * may allocate and compact, relocating the list (see the map note). */
        CL_GC_PROTECT(cur);
        /* Skip to start */
        while (idx < start && !CL_NULL_P(cur)) { cur = cl_cdr(cur); idx++; }
        while (idx < end_val && !CL_NULL_P(cur)) {
            CL_Obj elem = apply_key(key_fn, cl_car(cur));
            accum = call_test(func, accum, elem);
            cur = cl_cdr(cur);
            idx++;
        }
        CL_GC_UNPROTECT(1);
    } else {
        int32_t idx;
        for (idx = start; idx < end_val; idx++) {
            /* args[1], not the stale local seq — see the :from-end note */
            CL_Obj elem = apply_key(key_fn, seq_elt(args[1], idx));
            accum = call_test(func, accum, elem);
        }
    }

    CL_GC_UNPROTECT(3); /* key_fn, func, accum */
    return accum;
}

/* ======================================================= */
/* FILL                                                    */
/* ======================================================= */

static CL_Obj bi_fill(CL_Obj *args, int n)
{
    CL_Obj seq = args[0], item = args[1];
    int32_t start = 0, end_val, len;
    int32_t i;

    cl_check_seq_keywords(args, n, 2, SK_START | SK_END);
    len = seq_length(seq);   /* signals type-error if seq is not a sequence */
    end_val = len;

    /* Parse and validate :start / :end.  Both must be valid bounding indices
     * (CLHS): :start a non-negative integer <= length, :end nil or an integer
     * in [start, length].  Anything else is a type-error.
     * CLHS 3.4.1.4: leftmost duplicate keyword wins — walk back-to-front so
     * the leftmost occurrence is applied last (FILL.ORDER.4). */
    for (i = ((n - 2) & ~1); i >= 2; i -= 2) {
        CL_Obj v;
        if (i + 1 >= n) continue;
        v = args[i + 1];
        if (args[i] == KW_START) {
            if (!CL_FIXNUM_P(v) || CL_FIXNUM_VAL(v) < 0 || CL_FIXNUM_VAL(v) > len)
                cl_error(CL_ERR_TYPE, "FILL: :start is not a valid bounding index");
            start = CL_FIXNUM_VAL(v);
        } else if (args[i] == KW_END) {
            if (!CL_NULL_P(v)) {
                if (!CL_FIXNUM_P(v) || CL_FIXNUM_VAL(v) < 0 || CL_FIXNUM_VAL(v) > len)
                    cl_error(CL_ERR_TYPE, "FILL: :end is not a valid bounding index");
                end_val = CL_FIXNUM_VAL(v);
            } else {
                end_val = len;
            }
        }
    }
    if (start > end_val)
        cl_error(CL_ERR_TYPE, "FILL: :start is greater than :end");

    if (CL_CONS_P(seq) || CL_NULL_P(seq)) {
        CL_Obj cur = seq;
        for (i = 0; i < end_val && !CL_NULL_P(cur); i++, cur = cl_cdr(cur)) {
            if (i >= start)
                ((CL_Cons *)CL_OBJ_TO_PTR(cur))->car = item;
        }
    } else if (CL_VECTOR_P(seq)) {
        CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(seq);
        CL_Obj *elts = cl_vector_data(v);
        int32_t vlen = (int32_t)cl_vector_active_length(v);
        for (i = start; i < end_val && i < vlen; i++)
            elts[i] = item;
    } else if (CL_ANY_STRING_P(seq)) {
        int32_t slen = (int32_t)cl_string_length(seq);
        if (!CL_CHAR_P(item))
            cl_error(CL_ERR_TYPE, "FILL: string requires a character");
        for (i = start; i < end_val && i < slen; i++)
            cl_string_set_char_at(seq, (uint32_t)i, CL_CHAR_VAL(item));
    } else if (CL_BIT_VECTOR_P(seq)) {
        CL_BitVector *bv = (CL_BitVector *)CL_OBJ_TO_PTR(seq);
        int32_t bvlen = (int32_t)cl_bv_active_length(bv);
        int32_t val;
        if (!CL_FIXNUM_P(item))
            cl_error(CL_ERR_TYPE, "FILL: bit vector requires 0 or 1");
        val = CL_FIXNUM_VAL(item);
        if (val != 0 && val != 1)
            cl_error(CL_ERR_TYPE, "FILL: bit vector requires 0 or 1");
        for (i = start; i < end_val && i < bvlen; i++)
            cl_bv_set_bit(bv, (uint32_t)i, val);
    } else if (CL_BYTE_VECTOR_P(seq)) {
        /* Packed elements: FILL is a memset (8-bit) or a tight store loop
         * (16-bit) over the range. */
        CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(seq);
        int32_t bvlen = (int32_t)cl_bytevec_active_length(bv);
        int32_t hi = (end_val < bvlen) ? end_val : bvlen;
        int32_t v = cl_bytevec_check_value(item, bv->is_signed, bv->elt_shift,
                                           "FILL on a byte vector");
        if (hi > start) {
            if (bv->elt_shift) {
                for (i = start; i < hi; i++)
                    cl_bytevec_set(bv, (uint32_t)i, v);
            } else {
                memset(bv->data + start, (uint8_t)v, (size_t)(hi - start));
            }
        }
    }

    return seq;
}

/* ======================================================= */
/* REPLACE                                                 */
/* ======================================================= */

static CL_Obj bi_replace(CL_Obj *args, int n)
{
    CL_Obj seq1 = args[0], seq2 = args[1];
    int32_t start1 = 0, end1, start2 = 0, end2;
    int32_t len1, len2;
    int32_t i, j, count;

    cl_check_seq_keywords(args, n, 2, SK_START1 | SK_END1 | SK_START2 | SK_END2);
    /* Parse keyword args */
    len1 = seq_length(seq1);
    len2 = seq_length(seq2);
    end1 = len1;
    end2 = len2;

    /* CLHS 3.4.1.4: leftmost duplicate keyword wins — walk back-to-front. */
    for (i = ((n - 2) & ~1); i >= 2; i -= 2) {
        if (i + 1 >= n) continue;
        if (args[i] == KW_START1 && CL_FIXNUM_P(args[i + 1]))
            start1 = CL_FIXNUM_VAL(args[i + 1]);
        else if (args[i] == KW_END1 && !CL_NULL_P(args[i + 1]) && CL_FIXNUM_P(args[i + 1]))
            end1 = CL_FIXNUM_VAL(args[i + 1]);
        else if (args[i] == KW_START2 && CL_FIXNUM_P(args[i + 1]))
            start2 = CL_FIXNUM_VAL(args[i + 1]);
        else if (args[i] == KW_END2 && !CL_NULL_P(args[i + 1]) && CL_FIXNUM_P(args[i + 1]))
            end2 = CL_FIXNUM_VAL(args[i + 1]);
    }

    count = end1 - start1;
    if (end2 - start2 < count) count = end2 - start2;
    if (count <= 0) return seq1;

    /* Fast path 1: both packed byte vectors of the same element type — the
     * copy is one memmove over the payloads (overlap-safe, same object
     * included) instead of a per-element loop that boxes every byte through
     * a snapshot vector.  The element type must match exactly: the same
     * width so offsets agree, the same signedness so bit patterns keep
     * their values (a u8 200 memmoved into an s8 vector would silently
     * become -56 where the general path signals a type-error). */
    if (CL_BYTE_VECTOR_P(seq1) && CL_BYTE_VECTOR_P(seq2) &&
        start1 >= 0 && start2 >= 0 && end1 <= len1 && end2 <= len2) {
        CL_ByteVector *b1 = (CL_ByteVector *)CL_OBJ_TO_PTR(seq1);
        CL_ByteVector *b2 = (CL_ByteVector *)CL_OBJ_TO_PTR(seq2);
        if (b1->elt_shift == b2->elt_shift && b1->is_signed == b2->is_signed) {
            /* volatile: m68k-amigaos-gcc miscompiles flexible-array
             * `->data + offset` folded into the copy (the pointer comes
             * out off by one) — force the base through memory first, as
             * in cl_stream_write_lisp_string. */
            uint8_t *volatile d1 = b1->data;
            const uint8_t *volatile d2 = b2->data;
            memmove((uint8_t *)d1 + ((uint32_t)start1 << b1->elt_shift),
                    (const uint8_t *)d2 + ((uint32_t)start2 << b2->elt_shift),
                    (size_t)((uint32_t)count << b1->elt_shift));
            return seq1;
        }
    }

    /* Fast path 2: both simple base strings — same 1-byte element layout,
     * one overlap-safe memmove. */
    if (CL_STRING_P(seq1) && CL_STRING_P(seq2) &&
        start1 >= 0 && start2 >= 0 && end1 <= len1 && end2 <= len2) {
        CL_String *s1 = (CL_String *)CL_OBJ_TO_PTR(seq1);
        CL_String *s2 = (CL_String *)CL_OBJ_TO_PTR(seq2);
        /* volatile: see the byte-vector path above (m68k workaround). */
        char *volatile c1 = s1->data;
        const char *volatile c2 = s2->data;
        memmove((char *)c1 + start1, (const char *)c2 + start2, (size_t)count);
        return seq1;
    }

    /* Snapshot the source region first — it makes the copy correct when seq1
     * and seq2 are the same object with overlapping regions (CLHS REPLACE).
     * A GC vector, not platform_alloc: seq_elt/arr_seq_set can signal a
     * type-error mid-copy (improper list, typed-array element mismatch), and
     * the longjmp would leak a C allocation; a GC object is just collected.
     * The snapshot allocation itself may compact, so it happens before any
     * raw pointer derivation; the fill/write-back loops below perform no
     * allocation, so the one data-pointer derivation stays valid. */
    {
        CL_Obj buf;
        CL_Obj *bd;
        CL_GC_PROTECT(seq1);
        CL_GC_PROTECT(seq2);
        buf = cl_make_vector((uint32_t)count);
        CL_GC_PROTECT(buf);
        bd = cl_vector_data((CL_Vector *)CL_OBJ_TO_PTR(buf));
        for (i = 0; i < count; i++)
            bd[i] = seq_elt(seq2, start2 + i);

        if (CL_CONS_P(seq1)) {
            CL_Obj cur1 = seq1;
            for (i = 0; i < start1 && !CL_NULL_P(cur1); i++) cur1 = cl_cdr(cur1);
            for (j = 0; j < count && !CL_NULL_P(cur1); j++, cur1 = cl_cdr(cur1))
                ((CL_Cons *)CL_OBJ_TO_PTR(cur1))->car = bd[j];
        } else {
            for (i = 0; i < count; i++)
                arr_seq_set(seq1, start1 + i, bd[i]);
        }
        CL_GC_UNPROTECT(3);
    }

    return seq1;
}

/* --- READ-SEQUENCE / WRITE-SEQUENCE (CLHS 21.2) ---------------------------
 *
 * C builtins replacing the boot.lisp per-element Lisp loops: on a 68020 a
 * VM round-trip per byte made reading a 20KB file cost seconds.  Element
 * kind follows the sequence (matching the previous boot.lisp behavior):
 * string-like sequences read/write characters, everything else bytes.
 *
 * Fast path: an (unsigned-byte 8) byte vector against a file stream moves
 * whole chunks through cl_stream_read_bytes/cl_stream_write_bytes (one
 * platform read/write per 4KB instead of one per byte).  Every other
 * combination uses a per-element C loop over the existing read/write
 * primitives, preserving their semantics (UTF-8 decode, socket timeouts,
 * string streams, EOF). */

#define CL_SEQ_IO_CHUNK 4096

/* Parse the :start/:end keyword tail (leftmost duplicate wins, CLHS 3.4.1.4).
 * *endp stays -1 when :end is missing or NIL. */
static void seq_io_parse_start_end(CL_Obj *args, int n, int32_t *start,
                                   int32_t *endp, const char *who)
{
    int i;
    *start = 0;
    *endp = -1;
    cl_check_seq_keywords(args, n, 2, SK_START | SK_END);
    for (i = ((n - 2) & ~1); i >= 2; i -= 2) {
        if (i + 1 >= n) continue;
        if (args[i] == KW_START) {
            if (!CL_FIXNUM_P(args[i + 1]))
                cl_error(CL_ERR_TYPE, "%s: :start is not a valid bounding index", who);
            *start = CL_FIXNUM_VAL(args[i + 1]);
        } else if (args[i] == KW_END) {
            if (!CL_NULL_P(args[i + 1])) {
                if (!CL_FIXNUM_P(args[i + 1]))
                    cl_error(CL_ERR_TYPE, "%s: :end is not a valid bounding index", who);
                *endp = CL_FIXNUM_VAL(args[i + 1]);
            }
        }
    }
}

static void seq_io_check_bounds(int32_t start, int32_t end, int32_t len,
                                const char *who)
{
    if (start < 0 || end > len || start > end)
        cl_error(CL_ERR_ARGS,
                 "%s: bounding indices %d and %d are bad for a sequence of length %d",
                 who, (int)start, (int)end, (int)len);
}

/* (read-sequence sequence stream &key start end) => position (CLHS).
 * Destructively fills sequence[start,end) with elements read from stream;
 * returns the index of the first element not updated (EOF before end). */
static CL_Obj bi_read_sequence(CL_Obj *args, int n)
{
    CL_Obj seq = args[0];
    CL_Obj stream = args[1];
    int32_t start, end, len, i;
    int use_char;
    int seq_is_list;

    if (!CL_STREAM_P(stream))
        cl_error(CL_ERR_TYPE, "READ-SEQUENCE: second argument is not a stream");
    {
        CL_Stream *st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
        if (st->direction != CL_STREAM_INPUT && st->direction != CL_STREAM_IO)
            cl_error(CL_ERR_TYPE, "READ-SEQUENCE: stream is not an input stream");
    }

    len = seq_length(seq);              /* signals if not a sequence */
    seq_io_parse_start_end(args, n, &start, &end, "READ-SEQUENCE");
    if (end < 0) end = len;
    seq_io_check_bounds(start, end, len, "READ-SEQUENCE");

    use_char = seq_is_string_like(seq);
    seq_is_list = (CL_CONS_P(seq) || CL_NULL_P(seq));
    i = start;

    /* Bulk path: unsigned 8-bit byte vector filled from a file stream.
     * Chunk through a C stack buffer: the platform read may park in a GC
     * safe region where a peer compaction moves the vector, so the payload
     * pointer is re-derived from rooted args[0] after every chunk. */
    if (!seq_is_list && CL_BYTE_VECTOR_P(seq)) {
        CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(seq);
        if (bv->elt_shift == 0 && !bv->is_signed) {
            char chunk[CL_SEQ_IO_CHUNK];
            while (i < end) {
                uint32_t want = (uint32_t)(end - i);
                int32_t got;
                if (want > sizeof(chunk)) want = (uint32_t)sizeof(chunk);
                got = cl_stream_read_bytes(args[1], chunk, want);
                if (got == -2) break;   /* no bulk path — per-element below */
                if (got > 0) {
                    /* volatile: m68k flexible-array `->data + offset`
                     * miscompile workaround (see bi_replace). */
                    uint8_t *volatile d;
                    bv = (CL_ByteVector *)CL_OBJ_TO_PTR(args[0]);
                    d = bv->data;
                    memcpy((uint8_t *)d + i, chunk, (size_t)got);
                    i += got;
                }
                if (got < (int32_t)want)
                    return CL_MAKE_FIXNUM(i);   /* EOF */
            }
            if (i >= end)
                return CL_MAKE_FIXNUM(i);
        }
    }

    /* Per-element loop.  A blocking read can park in a GC safe region where
     * a peer compacts, so the list cursor must stay rooted; array stores go
     * through rooted args[0] each iteration. */
    {
        CL_Obj cur = CL_NIL;
        if (seq_is_list) {
            int32_t k;
            cur = seq;
            for (k = 0; k < i && CL_CONS_P(cur); k++)
                cur = cl_cdr(cur);
        }
        CL_GC_PROTECT(cur);
        for (; i < end; i++) {
            int el = use_char ? cl_stream_read_char(args[1])
                              : cl_stream_read_byte(args[1]);
            if (el == -1) break;
            if (seq_is_list) {
                if (!CL_CONS_P(cur)) break;
                ((CL_Cons *)CL_OBJ_TO_PTR(cur))->car =
                    use_char ? CL_MAKE_CHAR(el) : CL_MAKE_FIXNUM(el);
                cur = cl_cdr(cur);
            } else {
                arr_seq_set(args[0], i,
                            use_char ? CL_MAKE_CHAR(el) : CL_MAKE_FIXNUM(el));
            }
        }
        CL_GC_UNPROTECT(1);
    }
    return CL_MAKE_FIXNUM(i);
}

/* (write-sequence sequence stream &key start end) => sequence (CLHS).
 * Characters go through the char path (UTF-8/latin-1 encode), integers
 * through the byte path; a mixed sequence is written element-wise. */
static CL_Obj bi_write_sequence(CL_Obj *args, int n)
{
    CL_Obj seq = args[0];
    CL_Obj stream = args[1];
    int32_t start, end, len, i;
    int seq_is_list;

    if (!CL_STREAM_P(stream))
        cl_error(CL_ERR_TYPE, "WRITE-SEQUENCE: second argument is not a stream");
    {
        CL_Stream *st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
        if (st->direction != CL_STREAM_OUTPUT && st->direction != CL_STREAM_IO)
            cl_error(CL_ERR_TYPE,
                     "WRITE-SEQUENCE: stream is not an output stream");
    }

    len = seq_length(seq);
    seq_io_parse_start_end(args, n, &start, &end, "WRITE-SEQUENCE");
    if (end < 0) end = len;
    seq_io_check_bounds(start, end, len, "WRITE-SEQUENCE");
    if (start == end) return args[0];
    seq_is_list = (CL_CONS_P(seq) || CL_NULL_P(seq));

    /* Bulk path 1: a simple base string — the stream layer already has an
     * arena-safe chunked writer for exactly this. */
    if (CL_STRING_P(seq)) {
        cl_stream_write_lisp_string(stream, seq, (uint32_t)start, (uint32_t)end);
        return args[0];
    }

    /* Bulk path 2: unsigned 8-bit byte vector to a file/socket stream.
     * Chunked through a C stack buffer (re-derive the payload after each
     * write — it may park in a safe region where a peer compacts). */
    if (!seq_is_list && CL_BYTE_VECTOR_P(seq)) {
        CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(seq);
        if (bv->elt_shift == 0 && !bv->is_signed) {
            char chunk[CL_SEQ_IO_CHUNK];
            int bulk_ok = 1;
            i = start;
            while (i < end && bulk_ok) {
                uint32_t nb = (uint32_t)(end - i);
                int32_t wr;
                /* volatile: m68k flexible-array `->data + offset`
                 * miscompile workaround (see bi_replace). */
                const uint8_t *volatile d;
                if (nb > sizeof(chunk)) nb = (uint32_t)sizeof(chunk);
                bv = (CL_ByteVector *)CL_OBJ_TO_PTR(args[0]);
                d = bv->data;
                memcpy(chunk, (const uint8_t *)d + i, (size_t)nb);
                wr = cl_stream_write_bytes(args[1], chunk, nb);
                if (wr == -2) { bulk_ok = 0; break; }
                i += (int32_t)nb;
            }
            if (bulk_ok)
                return args[0];
            start = i;   /* fall through: write the rest per element */
        }
    }

    /* Per-element loop (lists, general vectors, wide strings, non-file
     * streams).  Root the list cursor across blocking writes. */
    {
        CL_Obj cur = CL_NIL;
        if (seq_is_list) {
            int32_t k;
            cur = seq;
            for (k = 0; k < start && CL_CONS_P(cur); k++)
                cur = cl_cdr(cur);
        }
        CL_GC_PROTECT(cur);
        for (i = start; i < end; i++) {
            CL_Obj el;
            if (seq_is_list) {
                if (!CL_CONS_P(cur)) break;
                el = cl_car(cur);
                cur = cl_cdr(cur);
            } else {
                el = arr_seq_get(args[0], i);
            }
            if (CL_CHAR_P(el)) {
                cl_stream_write_char(args[1], CL_CHAR_VAL(el));
            } else if (CL_FIXNUM_P(el)) {
                int32_t b = CL_FIXNUM_VAL(el);
                if (b < 0 || b > 255) {
                    CL_GC_UNPROTECT(1);
                    cl_error(CL_ERR_GENERAL,
                             "WRITE-SEQUENCE: byte value %d out of range 0-255",
                             (int)b);
                }
                cl_stream_write_byte(args[1], b);
            } else {
                CL_GC_UNPROTECT(1);
                cl_signal_type_error(el, "(OR CHARACTER (UNSIGNED-BYTE 8))",
                                     "WRITE-SEQUENCE element");
            }
        }
        CL_GC_UNPROTECT(1);
    }
    return args[0];
}

/* --- Phase 8 Step 3: elt, (setf elt), copy-seq, map-into --- */

static CL_Obj bi_elt(CL_Obj *args, int n)
{
    int32_t idx, len;
    CL_UNUSED(n);
    /* The index must be a valid index for the sequence; otherwise a type-error
     * is signalled (CLHS ELT). */
    if (!CL_FIXNUM_P(args[1]))
        cl_error(CL_ERR_TYPE, "ELT: index is not a valid sequence index");
    idx = CL_FIXNUM_VAL(args[1]);
    len = seq_length(args[0]);   /* signals type-error if not a sequence */
    if (idx < 0 || idx >= len)
        cl_error(CL_ERR_TYPE, "ELT: index out of bounds for sequence");
    return seq_elt(args[0], idx);
}

static CL_Obj bi_setf_elt(CL_Obj *args, int n)
{
    /* args: sequence index new-value */
    CL_Obj seq = args[0];
    int32_t idx;
    CL_Obj val = args[2];
    CL_UNUSED(n);
    if (!CL_FIXNUM_P(args[1]))
        cl_error(CL_ERR_TYPE, "(SETF ELT): index is not a valid sequence index");
    idx = CL_FIXNUM_VAL(args[1]);
    if (idx < 0)
        cl_error(CL_ERR_TYPE, "(SETF ELT): index out of bounds for sequence");

    if (CL_CONS_P(seq) || CL_NULL_P(seq)) {
        while (idx > 0 && !CL_NULL_P(seq)) { seq = cl_cdr(seq); idx--; }
        if (CL_NULL_P(seq))
            cl_error(CL_ERR_TYPE, "(SETF ELT): index out of bounds for sequence");
        ((CL_Cons *)CL_OBJ_TO_PTR(seq))->car = val;
        return val;
    }
    if (CL_VECTOR_P(seq)) {
        CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(seq);
        if ((uint32_t)idx >= cl_vector_active_length(v))
            cl_error(CL_ERR_TYPE, "(SETF ELT): index out of bounds for sequence");
        cl_vector_data(v)[idx] = val;
        return val;
    }
    if (CL_ANY_STRING_P(seq)) {
        if ((uint32_t)idx >= cl_string_length(seq))
            cl_error(CL_ERR_TYPE, "(SETF ELT): index out of bounds for sequence");
        if (!CL_CHAR_P(val))
            cl_error(CL_ERR_TYPE, "(SETF ELT): value must be a character for string");
        cl_string_set_char_at(seq, (uint32_t)idx, CL_CHAR_VAL(val));
        return val;
    }
    if (CL_BIT_VECTOR_P(seq)) {
        CL_BitVector *bv = (CL_BitVector *)CL_OBJ_TO_PTR(seq);
        int32_t v;
        if ((uint32_t)idx >= cl_bv_active_length(bv))
            cl_error(CL_ERR_TYPE, "(SETF ELT): index out of bounds for sequence");
        if (!CL_FIXNUM_P(val))
            cl_error(CL_ERR_TYPE, "(SETF ELT): value must be 0 or 1 for bit vector");
        v = CL_FIXNUM_VAL(val);
        if (v != 0 && v != 1)
            cl_error(CL_ERR_TYPE, "(SETF ELT): value must be 0 or 1 for bit vector");
        cl_bv_set_bit(bv, (uint32_t)idx, v);
        return val;
    }
    if (CL_BYTE_VECTOR_P(seq)) {
        CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(seq);
        if ((uint32_t)idx >= cl_bytevec_active_length(bv))
            cl_error(CL_ERR_TYPE, "(SETF ELT): index out of bounds for sequence");
        arr_seq_set(seq, idx, val);  /* range-checks against signedness */
        return val;
    }
    cl_error(CL_ERR_TYPE, "(SETF ELT): not a sequence");
    return CL_NIL;
}

static CL_Obj bi_copy_seq(CL_Obj *args, int n)
{
    CL_Obj seq = args[0];
    CL_UNUSED(n);

    if (CL_NULL_P(seq)) return CL_NIL;

    /* GC-protect the source: the local `seq` is a copy of args[0], so the
     * compactor would not otherwise forward it across the allocations below,
     * leaving a stale offset and a corrupted copy under heap pressure. */
    CL_GC_PROTECT(seq);

    if (CL_CONS_P(seq)) {
        /* Copy list */
        CL_Obj result = CL_NIL, tail = CL_NIL;
        CL_GC_PROTECT(result);
        CL_GC_PROTECT(tail);
        while (CL_CONS_P(seq)) {
            CL_Obj cell = cl_cons(cl_car(seq), CL_NIL);
            if (CL_NULL_P(result))
                result = cell;
            else
                ((CL_Cons *)CL_OBJ_TO_PTR(tail))->cdr = cell;
            tail = cell;
            seq = cl_cdr(seq);
        }
        CL_GC_UNPROTECT(3);
        return result;
    }
    /* Arrays (general vector, string, adjustable/fill-pointer character vector,
     * bit-vector): copy_array_seq produces a fresh SIMPLE sequence of the same
     * broad class and the active length — including a STRING (not a general
     * vector) for a string-vector, and the active length for fill-pointered
     * vectors / bit-vectors. */
    if (seq_is_array(seq)) {
        CL_Obj result = copy_array_seq(seq);
        CL_GC_UNPROTECT(1);
        return result;
    }
    CL_GC_UNPROTECT(1);
    cl_signal_type_error(seq, "SEQUENCE", "COPY-SEQ");
    return CL_NIL;
}

static CL_Obj bi_map_into(CL_Obj *args, int n)
{
    CL_Obj result_seq = args[0];
    CL_Obj func = cl_coerce_funcdesig(args[1], "MAP-INTO");
    int n_seqs = n - 2;
    CL_Obj seqs[16];
    CL_Obj call_args[16];
    CL_Obj res_cur;
    int32_t idx = 0, result_cap;
    int j, result_is_list, result_is_array;

    if (n_seqs > 16)
        cl_error(CL_ERR_ARGS,
                 "MAP-INTO: too many sequence arguments (%d; max 16)", n_seqs);
    for (j = 0; j < n_seqs; j++)
        seqs[j] = args[j + 2];

    result_is_list = (CL_CONS_P(result_seq) || CL_NULL_P(result_seq));
    result_is_array = (CL_VECTOR_P(result_seq) || CL_ANY_STRING_P(result_seq) ||
                       CL_BIT_VECTOR_P(result_seq) || CL_BYTE_VECTOR_P(result_seq));
    if (!result_is_list && !result_is_array)
        cl_signal_type_error(result_seq, "SEQUENCE", "MAP-INTO");

    /* CLHS MAP-INTO: a fill pointer on the result is *ignored* when deciding
     * how many elements to store — the storage size bounds it instead — and
     * is afterwards set to the number of elements stored.  So cap on the full
     * allocated length, not the active (fill-pointer) length. */
    if (CL_VECTOR_P(result_seq))
        result_cap = (int32_t)((CL_Vector *)CL_OBJ_TO_PTR(result_seq))->length;
    else if (CL_BIT_VECTOR_P(result_seq))
        result_cap = (int32_t)((CL_BitVector *)CL_OBJ_TO_PTR(result_seq))->length;
    else if (CL_BYTE_VECTOR_P(result_seq))
        result_cap = (int32_t)((CL_ByteVector *)CL_OBJ_TO_PTR(result_seq))->length;
    else
        result_cap = seq_length(result_seq);

    /* Fast path: a bitwise fold (#'logior/#'logand/#'logxor, the true
     * builtins) over unsigned 8-bit byte vectors into an unsigned 8-bit
     * byte vector runs as a plain C loop — no per-element VM apply, no
     * boxing.  Overlap (result == a source, the in-place fold idiom) is
     * fine: each element is read before it is written.  No allocation
     * happens, so raw payload pointers stay valid throughout. */
    if ((func == SYM_LOGIOR_FN || func == SYM_LOGAND_FN ||
         func == SYM_LOGXOR_FN) &&
        !CL_NULL_P(func) && n_seqs >= 1 && CL_BYTE_VECTOR_P(result_seq)) {
        CL_ByteVector *rbv = (CL_ByteVector *)CL_OBJ_TO_PTR(result_seq);
        int all_u8 = (rbv->elt_shift == 0 && !rbv->is_signed);
        int32_t m = result_cap;
        for (j = 0; all_u8 && j < n_seqs; j++) {
            if (CL_BYTE_VECTOR_P(seqs[j])) {
                CL_ByteVector *sb = (CL_ByteVector *)CL_OBJ_TO_PTR(seqs[j]);
                int32_t sl = (int32_t)cl_bytevec_active_length(sb);
                if (sb->elt_shift != 0 || sb->is_signed) all_u8 = 0;
                else if (sl < m) m = sl;
            } else {
                all_u8 = 0;
            }
        }
        if (all_u8) {
            /* No allocation below — raw payload pointers may be hoisted.
             * volatile: m68k flexible-array base-pointer miscompile
             * workaround (see bi_replace) — route each ->data through a
             * volatile local before indexing off it. */
            uint8_t *volatile dstv = rbv->data;
            uint8_t *dst = (uint8_t *)dstv;
            uint8_t *srcs[16];
            int32_t k;
            int op = (func == SYM_LOGIOR_FN) ? 0
                   : (func == SYM_LOGAND_FN) ? 1 : 2;
            for (j = 0; j < n_seqs; j++) {
                uint8_t *volatile sv =
                    ((CL_ByteVector *)CL_OBJ_TO_PTR(seqs[j]))->data;
                srcs[j] = (uint8_t *)sv;
            }
            for (k = 0; k < m; k++) {
                uint8_t acc = srcs[0][k];
                for (j = 1; j < n_seqs; j++) {
                    uint8_t v = srcs[j][k];
                    acc = (op == 0) ? (uint8_t)(acc | v)
                        : (op == 1) ? (uint8_t)(acc & v)
                                    : (uint8_t)(acc ^ v);
                }
                dst[k] = acc;
            }
            if (rbv->fill_pointer != CL_NO_FILL_POINTER)
                rbv->fill_pointer = (uint32_t)m;
            return result_seq;
        }
    }

    res_cur = result_seq;       /* list write cursor */
    CL_GC_PROTECT(result_seq);  /* protect against compaction in cl_vm_apply loop */
    CL_GC_PROTECT(res_cur);
    CL_GC_PROTECT(func);
    /* Protect the source seq cursors too: cl_vm_apply runs the mapped function,
     * which may allocate and compact, relocating the list cursors in seqs[]
     * (and the vector/string objects read via seq_elt).  Without rooting them
     * the next cl_car(seqs[j]) walks stale memory under GC stress. */
    for (j = 0; j < n_seqs; j++)
        CL_GC_PROTECT(seqs[j]);

    /* Precompute source sequence lengths for non-list types */
    {
        int32_t src_lens[16];
        for (j = 0; j < n_seqs; j++) {
            if (CL_CONS_P(seqs[j]) || CL_NULL_P(seqs[j]))
                src_lens[j] = -1; /* list — track via CL_NULL_P */
            else
                src_lens[j] = seq_length(seqs[j]);
        }

        for (idx = 0; idx < result_cap; idx++) {
            CL_Obj val;
            if (result_is_list && CL_NULL_P(res_cur)) break;
            /* Check if any source sequence is exhausted */
            for (j = 0; j < n_seqs; j++) {
                if (src_lens[j] >= 0) {
                    if (idx >= src_lens[j]) goto map_into_done;
                } else {
                    if (CL_NULL_P(seqs[j])) goto map_into_done;
                }
            }
            /* Gather arguments */
            for (j = 0; j < n_seqs; j++) {
                if (src_lens[j] < 0) {
                    call_args[j] = cl_car(seqs[j]);
                    seqs[j] = cl_cdr(seqs[j]);
                } else {
                    call_args[j] = seq_elt(seqs[j], idx);
                }
            }
            /* Call the mapping function exactly once per stored element.
             * cl_vm_apply GC-roots call_args; res_cur is CL_GC_PROTECTed so
             * it survives compaction triggered inside the call. */
            val = cl_vm_apply(func, call_args, n_seqs);
            if (result_is_list) {
                ((CL_Cons *)CL_OBJ_TO_PTR(res_cur))->car = val;
                res_cur = cl_cdr(res_cur);
            } else {
                arr_seq_set(result_seq, idx, val);
            }
        }
    } /* end src_lens block */

map_into_done:
    CL_GC_UNPROTECT(3 + n_seqs); /* pops seqs[], func, res_cur, result_seq */
    /* Set the fill pointer of a fill-pointered result to the count stored. */
    if (CL_VECTOR_P(result_seq)) {
        CL_Vector *rv = (CL_Vector *)CL_OBJ_TO_PTR(result_seq);
        if (rv->fill_pointer != CL_NO_FILL_POINTER)
            rv->fill_pointer = (uint32_t)idx;
    } else if (CL_BIT_VECTOR_P(result_seq)) {
        CL_BitVector *rbv = (CL_BitVector *)CL_OBJ_TO_PTR(result_seq);
        if (rbv->fill_pointer != CL_NO_FILL_POINTER)
            rbv->fill_pointer = (uint32_t)idx;
    } else if (CL_BYTE_VECTOR_P(result_seq)) {
        CL_ByteVector *rbv = (CL_ByteVector *)CL_OBJ_TO_PTR(result_seq);
        if (rbv->fill_pointer != CL_NO_FILL_POINTER)
            rbv->fill_pointer = (uint32_t)idx;
    }

    return result_seq;
}

/* ======================================================= */
/* Registration                                            */
/* ======================================================= */

void cl_builtins_sequence_init(void)
{
    /* Pre-intern keyword symbols */
    KW_TEST          = cl_intern_keyword("TEST", 4);
    KW_TEST_NOT      = cl_intern_keyword("TEST-NOT", 8);
    KW_KEY           = cl_intern_keyword("KEY", 3);
    KW_START         = cl_intern_keyword("START", 5);
    KW_END           = cl_intern_keyword("END", 3);
    KW_COUNT         = cl_intern_keyword("COUNT", 5);
    KW_FROM_END      = cl_intern_keyword("FROM-END", 8);
    KW_INITIAL_VALUE = cl_intern_keyword("INITIAL-VALUE", 13);
    KW_START1        = cl_intern_keyword("START1", 6);
    KW_END1          = cl_intern_keyword("END1", 4);
    KW_START2        = cl_intern_keyword("START2", 6);
    KW_END2          = cl_intern_keyword("END2", 4);
    SEQ_KW_AOK = cl_intern_keyword("ALLOW-OTHER-KEYS", 16);

    /* Cache eql function */
    {
        CL_Obj eql_sym = cl_intern_in("EQL", 3, cl_package_cl);
        CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(eql_sym);
        SYM_EQL_FN = s->function;
    }

    /* Cache the bitwise-fold builtins for the MAP-INTO fast path.  The
     * arithmetic builtins register before the sequence builtins (see
     * cl_builtins_init), so the function cells are populated here. */
    {
        CL_Obj sym = cl_intern_in("LOGIOR", 6, cl_package_cl);
        SYM_LOGIOR_FN = ((CL_Symbol *)CL_OBJ_TO_PTR(sym))->function;
        sym = cl_intern_in("LOGAND", 6, cl_package_cl);
        SYM_LOGAND_FN = ((CL_Symbol *)CL_OBJ_TO_PTR(sym))->function;
        sym = cl_intern_in("LOGXOR", 6, cl_package_cl);
        SYM_LOGXOR_FN = ((CL_Symbol *)CL_OBJ_TO_PTR(sym))->function;
    }

    /* Find family */
    defun("FIND", bi_find, 2, -1);
    defun("FIND-IF", bi_find_if, 2, -1);
    defun("FIND-IF-NOT", bi_find_if_not, 2, -1);

    /* Position family */
    defun("POSITION", bi_position, 2, -1);
    defun("POSITION-IF", bi_position_if, 2, -1);
    defun("POSITION-IF-NOT", bi_position_if_not, 2, -1);

    /* Count family */
    defun("COUNT", bi_count, 2, -1);
    defun("COUNT-IF", bi_count_if, 2, -1);
    defun("COUNT-IF-NOT", bi_count_if_not, 2, -1);

    /* Remove family */
    defun("REMOVE", bi_remove, 2, -1);
    defun("REMOVE-IF", bi_remove_if, 2, -1);
    defun("REMOVE-IF-NOT", bi_remove_if_not, 2, -1);
    defun("REMOVE-DUPLICATES", bi_remove_duplicates, 1, -1);
    defun("DELETE-DUPLICATES", bi_remove_duplicates, 1, -1);
    /* DELETE-IF-NOT: destructive on lists (splices in place), like DELETE. */
    defun("DELETE-IF-NOT", bi_delete_if_not_export, 2, -1);

    /* Substitute family */
    defun("SUBSTITUTE", bi_substitute, 3, -1);
    defun("SUBSTITUTE-IF", bi_substitute_if, 3, -1);
    defun("SUBSTITUTE-IF-NOT", bi_substitute_if_not, 3, -1);
    defun("NSUBSTITUTE", bi_nsubstitute, 3, -1);
    defun("NSUBSTITUTE-IF", bi_nsubstitute_if, 3, -1);
    defun("NSUBSTITUTE-IF-NOT", bi_nsubstitute_if_not, 3, -1);

    /* Reduce */
    defun("REDUCE", bi_reduce, 2, -1);

    /* Fill, Replace */
    defun("FILL", bi_fill, 2, -1);
    defun("REPLACE", bi_replace, 2, -1);

    /* Stream <-> sequence bulk I/O (CLHS 21.2) */
    defun("READ-SEQUENCE", bi_read_sequence, 2, 6);
    defun("WRITE-SEQUENCE", bi_write_sequence, 2, 6);

    /* Phase 8 Step 3 */
    defun("ELT", bi_elt, 2, 2);
    cl_register_builtin("%SETF-ELT", bi_setf_elt, 3, 3, cl_package_amilisp);
    defun("COPY-SEQ", bi_copy_seq, 1, 1);
    defun("MAP-INTO", bi_map_into, 2, -1);

    /* Register cached symbols for GC compaction forwarding */
    cl_gc_register_root(&KW_TEST);
    cl_gc_register_root(&KW_TEST_NOT);
    cl_gc_register_root(&KW_KEY);
    cl_gc_register_root(&KW_START);
    cl_gc_register_root(&KW_END);
    cl_gc_register_root(&KW_COUNT);
    cl_gc_register_root(&KW_FROM_END);
    cl_gc_register_root(&KW_INITIAL_VALUE);
    cl_gc_register_root(&KW_START1);
    cl_gc_register_root(&KW_END1);
    cl_gc_register_root(&KW_START2);
    cl_gc_register_root(&KW_END2);
    cl_gc_register_root(&SEQ_KW_AOK);
    cl_gc_register_root(&SYM_EQL_FN);
    cl_gc_register_root(&SYM_LOGIOR_FN);
    cl_gc_register_root(&SYM_LOGAND_FN);
    cl_gc_register_root(&SYM_LOGXOR_FN);
}
