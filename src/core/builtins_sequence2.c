#include "builtins.h"
#include "symbol.h"
#include "package.h"
#include "mem.h"
#include "error.h"
#include "vm.h"
#include "string_utils.h"
#include "../platform/platform.h"
#include <string.h>

/* From compiler.c — expand a (def)type name/specifier; CL_NIL if none. */
extern CL_Obj cl_get_type_expander(CL_Obj name);

/* --- Pre-interned keyword symbols --- */

static CL_Obj KW_TEST = CL_NIL;
static CL_Obj KW_TEST_NOT = CL_NIL;
static CL_Obj KW_KEY = CL_NIL;
static CL_Obj KW_START1 = CL_NIL;
static CL_Obj KW_END1 = CL_NIL;
static CL_Obj KW_START2 = CL_NIL;
static CL_Obj KW_END2 = CL_NIL;
static CL_Obj KW_FROM_END = CL_NIL;
static CL_Obj SYM_EQL_FN = CL_NIL;
static CL_Obj SYM_LIST = CL_NIL;

/* --- Shared helpers (same as builtins_sequence.c, static per file) --- */

static CL_Obj call_func(CL_Obj func, CL_Obj *call_args, int nargs)
{
    CL_Obj designator = func;
    /* Resolve symbol function designator */
    if (CL_SYMBOL_P(func)) {
        CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(func);
        func = s->function;
    }
    if (CL_FUNCTION_OBJ_P(func))
        /* cl_vm_apply GC-roots call_args across the call (the function may
         * compact while reading its own args). */
        return cl_vm_apply(func, call_args, nargs);
    {
        /* Which builtin (call_builtin records it per thread), what it was
         * handed, and -- when a symbol was handed over -- what its function
         * cell held: the two tell a bad argument from a clobbered cell. */
        char val[96], des[96], nm[96];
        CL_Obj b = CT->last_builtin;
        const char *bname = "?";
        if (CL_HEAP_P(b) && b <= cl_heap.arena_size - sizeof(CL_Function) &&
            CL_HDR_TYPE(CL_OBJ_TO_PTR(b)) == TYPE_FUNCTION &&
            CL_SYMBOL_P(((CL_Function *)CL_OBJ_TO_PTR(b))->name))
            bname = cl_symbol_name(((CL_Function *)CL_OBJ_TO_PTR(b))->name);
        if (designator != func)
            cl_error(CL_ERR_TYPE, "%s: not a function: %s (the function cell of %s), "
                     "called with (%s)", bname,
                     cl_obj_brief(func, val, sizeof(val)),
                     cl_obj_brief(designator, des, sizeof(des)),
                     cl_args_brief(call_args, nargs, nm, sizeof(nm)));
        else
            cl_error(CL_ERR_TYPE, "%s: not a function: %s, called with (%s)", bname,
                     cl_obj_brief(func, val, sizeof(val)),
                     cl_args_brief(call_args, nargs, nm, sizeof(nm)));
    }
    return CL_NIL;
}

static CL_Obj call_test(CL_Obj test_fn, CL_Obj a, CL_Obj b)
{
    CL_Obj targs[2];
    targs[0] = a;
    targs[1] = b;
    return call_func(test_fn, targs, 2);
}

static CL_Obj call_1(CL_Obj fn, CL_Obj arg)
{
    CL_Obj pargs[1];
    pargs[0] = arg;
    return call_func(fn, pargs, 1);
}

static CL_Obj apply_key(CL_Obj key_fn, CL_Obj elem)
{
    if (CL_NULL_P(key_fn)) return elem;
    return call_1(key_fn, elem);
}

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

/* ======================================================= */
/* EVERY / SOME / NOTANY / NOTEVERY                        */
/* ======================================================= */

/* Helper: get sequence length for any sequence type */
static int32_t every_seq_len(CL_Obj seq)
{
    if (CL_NULL_P(seq)) return 0;
    if (CL_ANY_STRING_P(seq))
        return (int32_t)cl_string_length(seq);
    if (CL_VECTOR_P(seq)) {
        CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(seq);
        return (int32_t)cl_vector_active_length(v);
    }
    if (CL_BIT_VECTOR_P(seq)) {
        CL_BitVector *bv = (CL_BitVector *)CL_OBJ_TO_PTR(seq);
        return (int32_t)cl_bv_active_length(bv);
    }
    if (CL_BYTE_VECTOR_P(seq)) {
        CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(seq);
        return (int32_t)cl_bytevec_active_length(bv);
    }
    return -1; /* list — length unknown */
}

/* Helper: get element at index for non-list sequences */
static CL_Obj every_seq_elt(CL_Obj seq, int32_t idx)
{
    if (CL_ANY_STRING_P(seq)) {
        return CL_MAKE_CHAR(cl_string_char_at(seq, (uint32_t)idx));
    }
    if (CL_VECTOR_P(seq)) {
        CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(seq);
        return cl_vector_data(v)[idx];
    }
    if (CL_BIT_VECTOR_P(seq)) {
        CL_BitVector *bv = (CL_BitVector *)CL_OBJ_TO_PTR(seq);
        return CL_MAKE_FIXNUM(cl_bv_get_bit(bv, (uint32_t)idx));
    }
    if (CL_BYTE_VECTOR_P(seq)) {
        CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(seq);
        return CL_MAKE_FIXNUM(cl_bytevec_get(bv, (uint32_t)idx));
    }
    return CL_NIL;
}

static CL_Obj bi_every(CL_Obj *args, int n)
{
    CL_Obj pred = cl_coerce_funcdesig(args[0], "EVERY");
    int nseqs = n - 1;
    CL_Obj seqs[16];    /* current list tails (for list seqs) or original seq */
    int32_t lens[16];   /* -1 for list, >= 0 for vector/string */
    CL_Obj call_args[16];
    int i;
    int32_t idx = 0;
    CL_Obj last_result = SYM_T;

    if (nseqs > 16)
        cl_error(CL_ERR_ARGS,
                 "EVERY: too many sequence arguments (%d; max 16)", nseqs);
    for (i = 0; i < nseqs; i++) {
        seqs[i] = args[i + 1];
        lens[i] = every_seq_len(seqs[i]);
    }

    /* GC-protect the list cursors across call_func: the predicate may allocate
     * and compact, relocating the list being walked (see the map-family note). */
    CL_GC_PROTECT(pred);
    CL_GC_PROTECT(last_result);
    for (i = 0; i < nseqs; i++)
        CL_GC_PROTECT(seqs[i]);

    for (;;) {
        /* Check exhaustion */
        for (i = 0; i < nseqs; i++) {
            if (lens[i] >= 0) {
                if (idx >= lens[i]) goto every_done;
            } else {
                if (CL_NULL_P(seqs[i])) goto every_done;
            }
        }
        /* Collect elements */
        for (i = 0; i < nseqs; i++) {
            if (lens[i] >= 0) {
                call_args[i] = every_seq_elt(args[i + 1], idx);
            } else {
                call_args[i] = cl_car(seqs[i]);
                seqs[i] = cl_cdr(seqs[i]);
            }
        }
        idx++;
        last_result = call_func(pred, call_args, nseqs);
        if (CL_NULL_P(last_result)) { last_result = CL_NIL; goto every_done; }
    }

every_done:
    CL_GC_UNPROTECT(2 + nseqs);
    /* EVERY returns a boolean (CLHS): T when every call succeeded, NIL when a
     * call returned false — never the raw (possibly non-T truthy) predicate
     * value, which would break callers that EQ/EQUAL the result against T. */
    return CL_NULL_P(last_result) ? CL_NIL : SYM_T;
}

static CL_Obj bi_some(CL_Obj *args, int n)
{
    CL_Obj pred = cl_coerce_funcdesig(args[0], "SOME");
    int nseqs = n - 1;
    CL_Obj seqs[16];
    int32_t lens[16];
    CL_Obj call_args[16];
    int i;
    int32_t idx = 0;
    CL_Obj result = CL_NIL;

    if (nseqs > 16)
        cl_error(CL_ERR_ARGS,
                 "SOME: too many sequence arguments (%d; max 16)", nseqs);
    for (i = 0; i < nseqs; i++) {
        seqs[i] = args[i + 1];
        lens[i] = every_seq_len(seqs[i]);
    }

    /* GC-protect the list cursors across call_func (see bi_every / map note). */
    CL_GC_PROTECT(pred);
    for (i = 0; i < nseqs; i++)
        CL_GC_PROTECT(seqs[i]);

    for (;;) {
        for (i = 0; i < nseqs; i++) {
            if (lens[i] >= 0) {
                if (idx >= lens[i]) { result = CL_NIL; goto some_done; }
            } else {
                if (CL_NULL_P(seqs[i])) { result = CL_NIL; goto some_done; }
            }
        }
        for (i = 0; i < nseqs; i++) {
            if (lens[i] >= 0) {
                call_args[i] = every_seq_elt(args[i + 1], idx);
            } else {
                call_args[i] = cl_car(seqs[i]);
                seqs[i] = cl_cdr(seqs[i]);
            }
        }
        idx++;
        result = call_func(pred, call_args, nseqs);
        if (!CL_NULL_P(result)) goto some_done;
    }

some_done:
    CL_GC_UNPROTECT(1 + nseqs);
    return result;
}

static CL_Obj bi_notany(CL_Obj *args, int n)
{
    /* (notany pred seqs...) = (not (some pred seqs...)) */
    CL_Obj result = bi_some(args, n);
    return CL_NULL_P(result) ? SYM_T : CL_NIL;
}

static CL_Obj bi_notevery(CL_Obj *args, int n)
{
    /* (notevery pred seqs...) = (not (every pred seqs...)) */
    CL_Obj result = bi_every(args, n);
    return CL_NULL_P(result) ? SYM_T : CL_NIL;
}

/* ======================================================= */
/* MAP                                                     */
/* ======================================================= */

/* Helper: check if any sequence is exhausted */
static int map_exhausted(CL_Obj *seqs, int32_t *lens, int nseqs, int32_t idx)
{
    int i;
    for (i = 0; i < nseqs; i++) {
        if (lens[i] >= 0) {
            if (idx >= lens[i]) return 1;
        } else {
            if (CL_NULL_P(seqs[i])) return 1;
        }
    }
    return 0;
}

/* Helper: collect elements from sequences at current index */
static void map_collect_args(CL_Obj *seqs, int32_t *lens, CL_Obj *orig_seqs,
                             CL_Obj *call_args, int nseqs, int32_t idx)
{
    int i;
    for (i = 0; i < nseqs; i++) {
        if (lens[i] >= 0) {
            call_args[i] = every_seq_elt(orig_seqs[i], idx);
        } else {
            call_args[i] = cl_car(seqs[i]);
            seqs[i] = cl_cdr(seqs[i]);
        }
    }
}

/* Helper: match result-type symbol name */
/* True if TYPE denotes a character subtype (CHARACTER/BASE-CHAR/... or a
 * deftype that expands to one).  Used to decide whether a (vector ...)
 * result-type should yield a string. DEPTH bounds deftype recursion. */
static int seq_elt_type_is_char(CL_Obj type, int depth)
{
    if (depth <= 0) return 0;
    if (CL_SYMBOL_P(type)) {
        const char *nm = cl_symbol_name(type);
        if (strcmp(nm, "CHARACTER") == 0 || strcmp(nm, "BASE-CHAR") == 0 ||
            strcmp(nm, "STANDARD-CHAR") == 0 || strcmp(nm, "EXTENDED-CHAR") == 0)
            return 1;
        if (strcmp(nm, "*") == 0) return 0;
        {
            CL_Obj ex = cl_get_type_expander(type);
            if (!CL_NULL_P(ex))
                return seq_elt_type_is_char(cl_vm_apply(ex, NULL, 0), depth - 1);
        }
    }
    return 0;
}

/* True if TYPE denotes the BIT type (or a deftype expanding to it). */
static int seq_elt_type_is_bit(CL_Obj type, int depth)
{
    if (depth <= 0) return 0;
    if (CL_SYMBOL_P(type)) {
        const char *nm = cl_symbol_name(type);
        if (strcmp(nm, "BIT") == 0) return 1;
        if (strcmp(nm, "*") == 0) return 0;
        {
            CL_Obj ex = cl_get_type_expander(type);
            if (!CL_NULL_P(ex))
                return seq_elt_type_is_bit(cl_vm_apply(ex, NULL, 0), depth - 1);
        }
    }
    return 0;
}

/* Classify a sequence constructor result-type (for MERGE / MAKE-SEQUENCE).
 *   0 = nil/null, 1 = list, 2 = string, 3 = (general) vector,
 *   4 = bit-vector, -1 = unknown.
 * *len_out is set to the declared length, or -1 when unspecified / `*`.
 * DEPTH bounds deftype-expansion recursion. */
static int seq_ctor_type_class(CL_Obj rt, int depth, int32_t *len_out)
{
    *len_out = -1;
    if (depth <= 0) return -1;
    if (CL_NULL_P(rt)) return 0;
    if (CL_SYMBOL_P(rt)) {
        const char *nm = cl_symbol_name(rt);
        if (strcmp(nm, "NULL") == 0) return 0;
        if (strcmp(nm, "LIST") == 0 || strcmp(nm, "CONS") == 0) return 1;
        if (strcmp(nm, "STRING") == 0 || strcmp(nm, "SIMPLE-STRING") == 0 ||
            strcmp(nm, "BASE-STRING") == 0 || strcmp(nm, "SIMPLE-BASE-STRING") == 0)
            return 2;
        if (strcmp(nm, "BIT-VECTOR") == 0 || strcmp(nm, "SIMPLE-BIT-VECTOR") == 0)
            return 4;
        if (strcmp(nm, "VECTOR") == 0 || strcmp(nm, "SIMPLE-VECTOR") == 0 ||
            strcmp(nm, "ARRAY") == 0 || strcmp(nm, "SIMPLE-ARRAY") == 0)
            return 3;
        {
            CL_Obj ex = cl_get_type_expander(rt);
            if (!CL_NULL_P(ex))
                return seq_ctor_type_class(cl_vm_apply(ex, NULL, 0), depth - 1, len_out);
        }
        return -1;
    }
    if (CL_CONS_P(rt)) {
        CL_Obj head = cl_car(rt);
        if (CL_SYMBOL_P(head)) {
            const char *nm = cl_symbol_name(head);
            CL_Obj rest = cl_cdr(rt);
            int cls = -1;
            CL_Obj lenarg = CL_NIL;
            if (strcmp(nm, "LIST") == 0 || strcmp(nm, "CONS") == 0) return 1;
            if (strcmp(nm, "STRING") == 0 || strcmp(nm, "SIMPLE-STRING") == 0 ||
                strcmp(nm, "BASE-STRING") == 0 || strcmp(nm, "SIMPLE-BASE-STRING") == 0) {
                cls = 2;
                lenarg = CL_NULL_P(rest) ? CL_NIL : cl_car(rest);
            } else if (strcmp(nm, "BIT-VECTOR") == 0 ||
                       strcmp(nm, "SIMPLE-BIT-VECTOR") == 0) {
                cls = 4;
                lenarg = CL_NULL_P(rest) ? CL_NIL : cl_car(rest);
            } else if (strcmp(nm, "VECTOR") == 0 || strcmp(nm, "SIMPLE-VECTOR") == 0) {
                /* (vector [elt-type [length]]) */
                CL_Obj eltarg = CL_NULL_P(rest) ? CL_NIL : cl_car(rest);
                lenarg = (CL_NULL_P(rest) || CL_NULL_P(cl_cdr(rest)))
                             ? CL_NIL : cl_car(cl_cdr(rest));
                if (!CL_NULL_P(eltarg) && seq_elt_type_is_char(eltarg, 8)) cls = 2;
                else if (!CL_NULL_P(eltarg) && seq_elt_type_is_bit(eltarg, 8)) cls = 4;
                else cls = 3;
            } else if (strcmp(nm, "ARRAY") == 0 || strcmp(nm, "SIMPLE-ARRAY") == 0) {
                CL_Obj eltarg = CL_NULL_P(rest) ? CL_NIL : cl_car(rest);
                if (!CL_NULL_P(eltarg) && seq_elt_type_is_char(eltarg, 8)) cls = 2;
                else if (!CL_NULL_P(eltarg) && seq_elt_type_is_bit(eltarg, 8)) cls = 4;
                else cls = 3;
            } else {
                CL_Obj ex = cl_get_type_expander(head);
                if (!CL_NULL_P(ex)) {
                    CL_Obj arg_array[16];
                    int na = 0;
                    CL_Obj r = rest;
                    while (!CL_NULL_P(r) && na < 16) {
                        arg_array[na++] = cl_car(r);
                        r = cl_cdr(r);
                    }
                    return seq_ctor_type_class(cl_vm_apply(ex, arg_array, na),
                                               depth - 1, len_out);
                }
                return -1;
            }
            if (CL_FIXNUM_P(lenarg)) *len_out = CL_FIXNUM_VAL(lenarg);
            return cls;
        }
    }
    return -1;
}

/* Classify a MAP/MERGE result-type specifier into:
 *   0 = nil/null, 1 = list, 2 = string, 3 = (general) vector, -1 = unknown.
 * Expands deftypes and understands compound (vector ...) / (array ...) /
 * (string ...) specifiers, choosing string when the element-type is a
 * character subtype (so e.g. babel's (vector unicode-char *) maps to a
 * string).  DEPTH bounds deftype-expansion recursion. */
static int seq_result_type_class(CL_Obj rt, int depth)
{
    if (depth <= 0) return -1;
    if (CL_NULL_P(rt)) return 0;
    if (CL_SYMBOL_P(rt)) {
        const char *nm = cl_symbol_name(rt);
        if (strcmp(nm, "NULL") == 0) return 0;
        if (strcmp(nm, "LIST") == 0 || strcmp(nm, "CONS") == 0) return 1;
        if (strcmp(nm, "STRING") == 0 || strcmp(nm, "SIMPLE-STRING") == 0 ||
            strcmp(nm, "BASE-STRING") == 0 || strcmp(nm, "SIMPLE-BASE-STRING") == 0)
            return 2;
        if (strcmp(nm, "VECTOR") == 0 || strcmp(nm, "SIMPLE-VECTOR") == 0 ||
            strcmp(nm, "ARRAY") == 0 || strcmp(nm, "SIMPLE-ARRAY") == 0 ||
            strcmp(nm, "BIT-VECTOR") == 0 || strcmp(nm, "SIMPLE-BIT-VECTOR") == 0)
            return 3;
        {
            CL_Obj ex = cl_get_type_expander(rt);
            if (!CL_NULL_P(ex))
                return seq_result_type_class(cl_vm_apply(ex, NULL, 0), depth - 1);
        }
        return -1;
    }
    if (CL_CONS_P(rt)) {
        CL_Obj head = cl_car(rt);
        if (CL_SYMBOL_P(head)) {
            const char *nm = cl_symbol_name(head);
            if (strcmp(nm, "LIST") == 0 || strcmp(nm, "CONS") == 0) return 1;
            if (strcmp(nm, "STRING") == 0 || strcmp(nm, "SIMPLE-STRING") == 0 ||
                strcmp(nm, "BASE-STRING") == 0 ||
                strcmp(nm, "SIMPLE-BASE-STRING") == 0)
                return 2;
            if (strcmp(nm, "VECTOR") == 0 || strcmp(nm, "SIMPLE-VECTOR") == 0 ||
                strcmp(nm, "ARRAY") == 0 || strcmp(nm, "SIMPLE-ARRAY") == 0) {
                CL_Obj eargs = cl_cdr(rt);
                if (!CL_NULL_P(eargs) && seq_elt_type_is_char(cl_car(eargs), 8))
                    return 2;
                return 3;
            }
            if (strcmp(nm, "BIT-VECTOR") == 0 ||
                strcmp(nm, "SIMPLE-BIT-VECTOR") == 0)
                return 3;
            {
                CL_Obj ex = cl_get_type_expander(head);
                if (!CL_NULL_P(ex)) {
                    CL_Obj arg_array[16];
                    int nargs = 0;
                    CL_Obj r = cl_cdr(rt);
                    while (!CL_NULL_P(r) && nargs < 16) {
                        arg_array[nargs++] = cl_car(r);
                        r = cl_cdr(r);
                    }
                    return seq_result_type_class(cl_vm_apply(ex, arg_array, nargs),
                                                 depth - 1);
                }
            }
        }
    }
    return -1;
}

static CL_Obj bi_map(CL_Obj *args, int n)
{
    /* (map result-type function &rest sequences) */
    CL_Obj result_type = args[0];
    CL_Obj func;
    int nseqs = n - 2;
    CL_Obj seqs[16];
    CL_Obj orig_seqs[16];
    int32_t lens[16];
    CL_Obj call_args[16];
    CL_Obj result = CL_NIL, tail = CL_NIL;
    int i;
    int32_t idx = 0;
    int rt; /* 0=nil, 1=list, 2=string, 3=vector */
    int32_t decl_len = -1;

    /* Classify the result-type FIRST: seq_result_type_class may expand a
     * deftype via cl_vm_apply, which can allocate/compact.  Only args[]
     * (GC-rooted) is live at this point, so no offsets are stale across it. */
    rt = seq_result_type_class(result_type, 16);
    /* Re-read from the rooted args[0]: the classify may have run a deftype
     * expander (cl_vm_apply) and compacted, staling the local copy that the
     * (or ...) walk / re-classification below dereference. */
    result_type = args[0];
    if (rt < 0) {
        /* An (or t1 t2 ...) of sequence subtypes is a valid MAP result-type
         * (MAP.48 / MAP.ERROR.11): the result must be a member of one of the
         * branches whose declared length matches the produced length.  Resolve
         * the branch against the produced length here. */
        if (CL_CONS_P(result_type) && CL_SYMBOL_P(cl_car(result_type)) &&
            strcmp(cl_symbol_name(cl_car(result_type)), "OR") == 0) {
            int32_t produced = 0x7FFFFFFF;
            CL_Obj branch;
            int found_cls = -1;   /* raw class of the (single) matching branch */
            int ambiguous = 0;
            int nsq = (nseqs > 16) ? 16 : nseqs;
            for (i = 0; i < nsq; i++) {
                int32_t l = every_seq_len(args[i + 2]);
                if (l < 0) l = seq_length(args[i + 2]);
                if (l < produced) produced = l;
            }
            /* A branch matches when it is a sequence subtype whose declared
             * length agrees with the produced length.  CLHS leaves the element
             * type ambiguous when several branches of different element types
             * match (MAP.ERROR.10: (or (vector bit) (vector t))) — signal an
             * error in that case.  Exactly one matching class → produce it;
             * none → type-error (MAP.ERROR.11). */
            CL_GC_PROTECT(branch);
            for (branch = cl_cdr(result_type); !CL_NULL_P(branch);
                 branch = cl_cdr(branch)) {
                int32_t blen = -1;
                int bcls = seq_ctor_type_class(cl_car(branch), 16, &blen);
                if (bcls >= 0 && (blen < 0 || blen == produced)) {
                    if (found_cls < 0) found_cls = bcls;
                    else if (found_cls != bcls) ambiguous = 1;
                }
            }
            CL_GC_UNPROTECT(1);
            if (found_cls < 0 || ambiguous) {
                /* Produce a vector of the mapped length and signal a type-error
                 * whose DATUM is that produced sequence and EXPECTED-TYPE the
                 * result-type object — the ansi `signals-error ... type-error`
                 * check requires (typep datum expected-type) to be NIL
                 * (MAP.ERROR.11); MAP.ERROR.10 only needs some error.  Force a
                 * length mismatch so the post-production check below fires
                 * cl_signal_type_error_obj(result, args[0], "MAP"). */
                rt = 3;
                decl_len = (produced == 0x7FFFFFFF ? 0 : produced) + 1;
            } else {
                rt = (found_cls == 4) ? 3 : found_cls;  /* bit-vector handled as vector */
                /* Branch already validated against the produced length. */
                decl_len = -1;
            }
        } else {
            /* A result-type that is not a recognized subtype of SEQUENCE
             * (e.g. SYMBOL) is a type-error per CLHS MAP, not a program-error. */
            cl_signal_type_error(result_type, "SEQUENCE", "MAP");
            return CL_NIL;
        }
    } else {
        /* Capture a declared length constraint, e.g. (vector * 8) or (string 5),
         * so we can reject a mismatched produced length with a type-error. */
        (void)seq_ctor_type_class(result_type, 16, &decl_len);
    }

    func = cl_coerce_funcdesig(args[1], "MAP");
    if (nseqs > 16)
        cl_error(CL_ERR_ARGS,
                 "MAP: too many sequence arguments (%d; max 16)", nseqs);
    for (i = 0; i < nseqs; i++) {
        seqs[i] = args[i + 2];
        orig_seqs[i] = seqs[i];
        lens[i] = every_seq_len(seqs[i]);
    }

    if (rt == 0) {
        /* nil/null result-type: iterate for side-effects.
         * GC-protect the seq cursors (and orig_seqs, used for index access on
         * vector/string args): call_func runs the mapped function, which may
         * allocate and compact, relocating list cursors and vector/string
         * objects.  Without this the next cl_car(seqs[i]) / every_seq_elt walks
         * stale offsets ("CAR: argument is not of type LIST"). */
        CL_GC_PROTECT(func);
        for (i = 0; i < nseqs; i++) { CL_GC_PROTECT(seqs[i]); CL_GC_PROTECT(orig_seqs[i]); }
        for (idx = 0; ; idx++) {
            if (map_exhausted(seqs, lens, nseqs, idx)) {
                CL_GC_UNPROTECT(1 + 2 * nseqs);
                return CL_NIL;
            }
            map_collect_args(seqs, lens, orig_seqs, call_args, nseqs, idx);
            call_func(func, call_args, nseqs);
        }
    }

    if (rt == 2 || rt == 3) {
        /* STRING or VECTOR result: need to know length first */
        int32_t min_len = 0x7FFFFFFF;
        for (i = 0; i < nseqs; i++) {
            int32_t l = (lens[i] >= 0) ? lens[i] : seq_length(seqs[i]);
            if (l < min_len) min_len = l;
        }

        if (rt == 2) {
            /* STRING result: allocate the (possibly wide) string up front
             * and fill it char-by-char.  This avoids any fixed buffer cap
             * and preserves characters > 255 (cl_string_set_char_at handles
             * the wide-string case). */
            CL_GC_PROTECT(func);
            result = cl_make_string(NULL, (uint32_t)min_len);
            CL_GC_PROTECT(result);
            /* Re-read the cursors from args[] (rooted VM-stack slots) — the
             * cl_make_string above can compact, and protecting the stale
             * copies below would root pre-compaction offsets. */
            for (i = 0; i < nseqs; i++) seqs[i] = orig_seqs[i] = args[i + 2];
            /* Protect seq cursors across call_func (see rt==0 note). */
            for (i = 0; i < nseqs; i++) { CL_GC_PROTECT(seqs[i]); CL_GC_PROTECT(orig_seqs[i]); }
            for (idx = 0; idx < min_len; idx++) {
                CL_Obj val;
                map_collect_args(seqs, lens, orig_seqs, call_args, nseqs, idx);
                val = call_func(func, call_args, nseqs);
                cl_string_set_char_at(result, (uint32_t)idx,
                                      CL_CHAR_P(val) ? CL_CHAR_VAL(val) : 0);
            }
            CL_GC_UNPROTECT(2 * nseqs);
            /* CLHS: a result-type length that disagrees with the produced
             * length is a type-error (datum = produced sequence). */
            if (decl_len >= 0 && decl_len != min_len)
                cl_signal_type_error_obj(result, args[0], "MAP"); /* args[0] GC-rooted; result_type may be stale */
            CL_GC_UNPROTECT(2);
            return result;
        } else {
            /* VECTOR */
            CL_GC_PROTECT(func);
            result = cl_make_vector((uint32_t)min_len);
            CL_GC_PROTECT(result);
            /* Re-read the cursors from args[] after the allocation (see the
             * string branch above). */
            for (i = 0; i < nseqs; i++) seqs[i] = orig_seqs[i] = args[i + 2];
            /* Protect seq cursors across call_func (see rt==0 note). */
            for (i = 0; i < nseqs; i++) { CL_GC_PROTECT(seqs[i]); CL_GC_PROTECT(orig_seqs[i]); }
            for (idx = 0; idx < min_len; idx++) {
                CL_Obj val;
                map_collect_args(seqs, lens, orig_seqs, call_args, nseqs, idx);
                val = call_func(func, call_args, nseqs);
                cl_vector_data((CL_Vector *)CL_OBJ_TO_PTR(result))[idx] = val;
            }
            CL_GC_UNPROTECT(2 * nseqs);
            if (decl_len >= 0 && decl_len != min_len)
                cl_signal_type_error_obj(result, args[0], "MAP"); /* args[0] GC-rooted; result_type may be stale */
            CL_GC_UNPROTECT(2);
            return result;
        }
    }

    /* LIST result type */
    CL_GC_PROTECT(func);
    CL_GC_PROTECT(result);
    CL_GC_PROTECT(tail);
    /* Protect seq cursors across call_func (see rt==0 note). */
    for (i = 0; i < nseqs; i++) { CL_GC_PROTECT(seqs[i]); CL_GC_PROTECT(orig_seqs[i]); }

    for (idx = 0; ; idx++) {
        CL_Obj val, cell;

        if (map_exhausted(seqs, lens, nseqs, idx)) break;
        map_collect_args(seqs, lens, orig_seqs, call_args, nseqs, idx);

        val = call_func(func, call_args, nseqs);
        cell = cl_cons(val, CL_NIL);
        if (CL_NULL_P(result)) result = cell;
        else ((CL_Cons *)CL_OBJ_TO_PTR(tail))->cdr = cell;
        tail = cell;
    }

    CL_GC_UNPROTECT(3 + 2 * nseqs);
    return result;
}

/* ======================================================= */
/* MISMATCH                                                */
/* ======================================================= */

static CL_Obj bi_mismatch(CL_Obj *args, int n)
{
    CL_Obj seq1 = args[0], seq2 = args[1];
    CL_Obj test_fn = SYM_EQL_FN;
    CL_Obj test_not_fn = CL_NIL;
    CL_Obj key_fn = CL_NIL;
    int32_t start1 = 0, end1, start2 = 0, end2;
    int32_t len1, len2;
    int32_t i, j;
    int ki;
    int from_end = 0;

    cl_check_seq_keywords(args, n, 2,
        SK_TEST | SK_TEST_NOT | SK_KEY | SK_START1 | SK_END1 |
        SK_START2 | SK_END2 | SK_FROM_END);

    /* Parse keywords.  CLHS 3.4.1.4: on a repeated keyword the leftmost
     * pair wins.  Walk the plist back-to-front so the leftmost occurrence
     * is applied last and overwrites later duplicates. */
    for (ki = ((n - 2) & ~1); ki >= 2; ki -= 2) {
        if (ki + 1 >= n) continue;
        if (args[ki] == KW_TEST) test_fn = cl_coerce_funcdesig(args[ki + 1], ":TEST");
        else if (args[ki] == KW_TEST_NOT) test_not_fn = cl_coerce_funcdesig(args[ki + 1], ":TEST-NOT");
        else if (args[ki] == KW_KEY) key_fn = CL_NULL_P(args[ki + 1]) ? CL_NIL : cl_coerce_funcdesig(args[ki + 1], ":KEY");
        else if (args[ki] == KW_START1 && CL_FIXNUM_P(args[ki + 1])) start1 = CL_FIXNUM_VAL(args[ki + 1]);
        else if (args[ki] == KW_START2 && CL_FIXNUM_P(args[ki + 1])) start2 = CL_FIXNUM_VAL(args[ki + 1]);
        else if (args[ki] == KW_FROM_END) from_end = !CL_NULL_P(args[ki + 1]);
    }

    len1 = seq_length(seq1);
    len2 = seq_length(seq2);

    /* Set defaults for end if not provided (leftmost wins) */
    end1 = len1;
    end2 = len2;
    for (ki = ((n - 2) & ~1); ki >= 2; ki -= 2) {
        if (ki + 1 >= n) continue;
        if (args[ki] == KW_END1 && !CL_NULL_P(args[ki + 1]) && CL_FIXNUM_P(args[ki + 1]))
            end1 = CL_FIXNUM_VAL(args[ki + 1]);
        if (args[ki] == KW_END2 && !CL_NULL_P(args[ki + 1]) && CL_FIXNUM_P(args[ki + 1]))
            end2 = CL_FIXNUM_VAL(args[ki + 1]);
    }

    /* GC SAFETY: apply_key/call_test run user code that can compact — the
     * seq/test/key locals are re-read every iteration, and e1 is held across
     * e2's apply_key.  Protect them all (returned values are immediates). */
    CL_GC_PROTECT(seq1);
    CL_GC_PROTECT(seq2);
    CL_GC_PROTECT(test_fn);
    CL_GC_PROTECT(test_not_fn);
    CL_GC_PROTECT(key_fn);

    if (from_end) {
        /* Search from the end; return position in seq1 of rightmost mismatch + 1,
         * or NIL if subsequences are identical. Per CL spec, the returned index
         * is the leftmost position such that elements at or after it don't all match. */
        int32_t sublen1 = end1 - start1;
        int32_t sublen2 = end2 - start2;
        int32_t mismatch_pos = -1;
        i = end1 - 1;
        j = end2 - 1;
        while (i >= start1 && j >= start2) {
            CL_Obj e1 = apply_key(key_fn, seq_elt(seq1, i));
            CL_Obj e2;
            int match;
            CL_GC_PROTECT(e1);
            e2 = apply_key(key_fn, seq_elt(seq2, j));
            match = !CL_NULL_P(test_not_fn)
                ? CL_NULL_P(call_test(test_not_fn, e1, e2))
                : !CL_NULL_P(call_test(test_fn, e1, e2));
            CL_GC_UNPROTECT(1);
            if (!match) {
                mismatch_pos = i + 1;
                break;
            }
            i--;
            j--;
        }
        CL_GC_UNPROTECT(5);
        if (mismatch_pos >= 0) return CL_MAKE_FIXNUM(mismatch_pos);
        /* All compared elements matched; check if lengths differ */
        if (sublen1 != sublen2) {
            /* Shorter sequence ran out first */
            if (sublen1 < sublen2) return CL_MAKE_FIXNUM(start1);
            else return CL_MAKE_FIXNUM(start1 + (sublen1 - sublen2));
        }
        return CL_NIL;
    }

    i = start1;
    j = start2;
    while (i < end1 && j < end2) {
        CL_Obj e1 = apply_key(key_fn, seq_elt(seq1, i));
        CL_Obj e2;
        int match;
        CL_GC_PROTECT(e1);
        e2 = apply_key(key_fn, seq_elt(seq2, j));
        match = !CL_NULL_P(test_not_fn)
            ? CL_NULL_P(call_test(test_not_fn, e1, e2))
            : !CL_NULL_P(call_test(test_fn, e1, e2));
        CL_GC_UNPROTECT(1);
        if (!match) {
            CL_GC_UNPROTECT(5);
            return CL_MAKE_FIXNUM(i);
        }
        i++;
        j++;
    }

    CL_GC_UNPROTECT(5);
    /* If both subsequences exhausted, no mismatch */
    if (i == end1 && j == end2) return CL_NIL;
    /* Otherwise mismatch at position where one ended */
    return CL_MAKE_FIXNUM(i);
}

/* ======================================================= */
/* SEARCH                                                  */
/* ======================================================= */

static CL_Obj bi_search(CL_Obj *args, int n)
{
    CL_Obj seq1 = args[0], seq2 = args[1];
    CL_Obj test_fn = SYM_EQL_FN;
    CL_Obj test_not_fn = CL_NIL;
    CL_Obj key_fn = CL_NIL;
    int32_t start1 = 0, end1, start2 = 0, end2;
    int32_t len1, len2, sublen;
    int32_t i, j;
    int ki, from_end = 0;

    cl_check_seq_keywords(args, n, 2,
        SK_TEST | SK_TEST_NOT | SK_KEY | SK_START1 | SK_END1 |
        SK_START2 | SK_END2 | SK_FROM_END);

    /* Parse keywords.  CLHS 3.4.1.4: leftmost duplicate wins — walk the
     * plist back-to-front so the leftmost pair is applied last. */
    for (ki = ((n - 2) & ~1); ki >= 2; ki -= 2) {
        if (ki + 1 >= n) continue;
        if (args[ki] == KW_TEST) test_fn = cl_coerce_funcdesig(args[ki + 1], ":TEST");
        else if (args[ki] == KW_TEST_NOT) test_not_fn = cl_coerce_funcdesig(args[ki + 1], ":TEST-NOT");
        else if (args[ki] == KW_KEY) key_fn = CL_NULL_P(args[ki + 1]) ? CL_NIL : cl_coerce_funcdesig(args[ki + 1], ":KEY");
        else if (args[ki] == KW_START1 && CL_FIXNUM_P(args[ki + 1])) start1 = CL_FIXNUM_VAL(args[ki + 1]);
        else if (args[ki] == KW_START2 && CL_FIXNUM_P(args[ki + 1])) start2 = CL_FIXNUM_VAL(args[ki + 1]);
        else if (args[ki] == KW_FROM_END) from_end = !CL_NULL_P(args[ki + 1]);
    }

    len1 = seq_length(seq1);
    len2 = seq_length(seq2);
    end1 = len1;
    end2 = len2;

    for (ki = ((n - 2) & ~1); ki >= 2; ki -= 2) {
        if (ki + 1 >= n) continue;
        if (args[ki] == KW_END1 && !CL_NULL_P(args[ki + 1]) && CL_FIXNUM_P(args[ki + 1]))
            end1 = CL_FIXNUM_VAL(args[ki + 1]);
        if (args[ki] == KW_END2 && !CL_NULL_P(args[ki + 1]) && CL_FIXNUM_P(args[ki + 1]))
            end2 = CL_FIXNUM_VAL(args[ki + 1]);
    }

    sublen = end1 - start1;
    /* Empty needle matches at start2 (or end2 searching from the end). */
    if (sublen <= 0)
        return CL_MAKE_FIXNUM(from_end ? end2 : start2);

    /* GC SAFETY: same as bi_mismatch — protect the seq/test/key locals for
     * the whole scan and e1 across e2's apply_key. */
    CL_GC_PROTECT(seq1);
    CL_GC_PROTECT(seq2);
    CL_GC_PROTECT(test_fn);
    CL_GC_PROTECT(test_not_fn);
    CL_GC_PROTECT(key_fn);

    /* Brute-force search.  With :from-end, scan right-to-left and return the
     * leftmost index of the rightmost match. */
    if (from_end) {
        for (i = end2 - sublen; i >= start2; i--) {
            int match = 1;
            for (j = 0; j < sublen; j++) {
                CL_Obj e1 = apply_key(key_fn, seq_elt(seq1, start1 + j));
                CL_Obj e2;
                int m;
                CL_GC_PROTECT(e1);
                e2 = apply_key(key_fn, seq_elt(seq2, i + j));
                m = !CL_NULL_P(test_not_fn)
                    ? CL_NULL_P(call_test(test_not_fn, e1, e2))
                    : !CL_NULL_P(call_test(test_fn, e1, e2));
                CL_GC_UNPROTECT(1);
                if (!m) { match = 0; break; }
            }
            if (match) {
                CL_GC_UNPROTECT(5);
                return CL_MAKE_FIXNUM(i);
            }
        }
        CL_GC_UNPROTECT(5);
        return CL_NIL;
    }

    for (i = start2; i + sublen <= end2; i++) {
        int match = 1;
        for (j = 0; j < sublen; j++) {
            CL_Obj e1 = apply_key(key_fn, seq_elt(seq1, start1 + j));
            CL_Obj e2;
            int m;
            CL_GC_PROTECT(e1);
            e2 = apply_key(key_fn, seq_elt(seq2, i + j));
            m = !CL_NULL_P(test_not_fn)
                ? CL_NULL_P(call_test(test_not_fn, e1, e2))
                : !CL_NULL_P(call_test(test_fn, e1, e2));
            CL_GC_UNPROTECT(1);
            if (!m) { match = 0; break; }
        }
        if (match) {
            CL_GC_UNPROTECT(5);
            return CL_MAKE_FIXNUM(i);
        }
    }

    CL_GC_UNPROTECT(5);
    return CL_NIL;
}

/* ======================================================= */
/* SORT / STABLE-SORT                                      */
/* ======================================================= */

/* Merge sort for lists — O(n log n) time, O(1) extra space */

static CL_Obj list_merge(CL_Obj a, CL_Obj b, CL_Obj pred, CL_Obj key_fn)
{
    CL_Obj result = CL_NIL, tail = CL_NIL;

    CL_GC_PROTECT(result);
    CL_GC_PROTECT(tail);
    CL_GC_PROTECT(a);
    CL_GC_PROTECT(b);
    /* GC SAFETY: pred/key_fn are re-read each iteration across the user
     * calls; ka is held across kb's apply_key. */
    CL_GC_PROTECT(pred);
    CL_GC_PROTECT(key_fn);

    while (!CL_NULL_P(a) && !CL_NULL_P(b)) {
        CL_Obj ka = CL_NIL, kb;
        CL_Obj pick;
        CL_GC_PROTECT(ka);
        ka = apply_key(key_fn, cl_car(a));
        kb = apply_key(key_fn, cl_car(b));
        CL_GC_UNPROTECT(1);
        /* Stable merge: pick `b` only when pred(kb, ka) is true, i.e.
           b is strictly less than a.  Otherwise (a strictly less, or
           equal, or incomparable) pick `a` — this preserves the
           original order of elements the predicate considers equal. */
        if (!CL_NULL_P(call_test(pred, kb, ka))) {
            pick = b;
            b = cl_cdr(b);
        } else {
            pick = a;
            a = cl_cdr(a);
        }
        if (CL_NULL_P(result)) {
            result = pick;
        } else {
            ((CL_Cons *)CL_OBJ_TO_PTR(tail))->cdr = pick;
        }
        tail = pick;
    }

    /* Append remainder */
    if (!CL_NULL_P(a)) {
        if (CL_NULL_P(result)) result = a;
        else ((CL_Cons *)CL_OBJ_TO_PTR(tail))->cdr = a;
    } else if (!CL_NULL_P(b)) {
        if (CL_NULL_P(result)) result = b;
        else ((CL_Cons *)CL_OBJ_TO_PTR(tail))->cdr = b;
    }

    CL_GC_UNPROTECT(6);
    return result;
}

static CL_Obj list_merge_sort(CL_Obj list, CL_Obj pred, CL_Obj key_fn)
{
    CL_Obj slow, fast, mid, result;

    if (CL_NULL_P(list) || CL_NULL_P(cl_cdr(list)))
        return list;

    /* Split into two halves with fast/slow pointer */
    slow = list;
    fast = cl_cdr(list);
    while (!CL_NULL_P(fast) && !CL_NULL_P(cl_cdr(fast))) {
        slow = cl_cdr(slow);
        fast = cl_cdr(cl_cdr(fast));
    }
    mid = cl_cdr(slow);
    ((CL_Cons *)CL_OBJ_TO_PTR(slow))->cdr = CL_NIL;

    /* GC-protect list and mid across recursive calls:
       each recursive sort triggers merges that call key/pred functions,
       which can allocate and trigger GC.  pred/key_fn are by-value C locals
       of THIS frame too — without their own roots the recursive sorts leave
       them stale before they're passed on to list_merge. */
    CL_GC_PROTECT(list);
    CL_GC_PROTECT(mid);
    CL_GC_PROTECT(pred);
    CL_GC_PROTECT(key_fn);

    list = list_merge_sort(list, pred, key_fn);
    mid = list_merge_sort(mid, pred, key_fn);

    result = list_merge(list, mid, pred, key_fn);
    CL_GC_UNPROTECT(4);
    return result;
}

/* ======================================================= */
/* MERGE                                                   */
/* ======================================================= */

/* Helper: collect sequence elements into a GC-managed vector.  A GC vector
 * (not a platform_alloc C array) is required: the merge loop below runs
 * user :key/predicate code that can compact, and only a heap vector's
 * elements are traced and forwarded across that.  Caller must GC-protect
 * the returned handle.  seq must be GC-protected by the caller (the
 * cl_make_vector here can itself compact). */
static CL_Obj seq_to_gc_vector(CL_Obj seq, int32_t *out_len)
{
    int32_t len, i;
    CL_Obj vec;
    CL_Obj cur;

    if (CL_NULL_P(seq)) { *out_len = 0; return CL_NIL; }

    if (CL_CONS_P(seq)) {
        len = 0;
        cur = seq;
        while (!CL_NULL_P(cur)) { len++; cur = cl_cdr(cur); }
        vec = cl_make_vector((uint32_t)len);
        /* Filling allocates nothing; re-read the (caller-protected) seq
         * after the allocation above. */
        cur = seq;
        for (i = 0; i < len; i++) {
            cl_vector_data((CL_Vector *)CL_OBJ_TO_PTR(vec))[i] = cl_car(cur);
            cur = cl_cdr(cur);
        }
    } else if (CL_VECTOR_P(seq)) {
        len = (int32_t)cl_vector_active_length((CL_Vector *)CL_OBJ_TO_PTR(seq));
        vec = cl_make_vector((uint32_t)len);
        for (i = 0; i < len; i++)
            cl_vector_data((CL_Vector *)CL_OBJ_TO_PTR(vec))[i] =
                cl_vector_data((CL_Vector *)CL_OBJ_TO_PTR(seq))[i];
    } else if (CL_ANY_STRING_P(seq)) {
        len = (int32_t)cl_string_length(seq);
        vec = cl_make_vector((uint32_t)len);
        for (i = 0; i < len; i++)
            cl_vector_data((CL_Vector *)CL_OBJ_TO_PTR(vec))[i] =
                CL_MAKE_CHAR(cl_string_char_at(seq, (uint32_t)i));
    } else if (CL_BIT_VECTOR_P(seq)) {
        len = (int32_t)cl_bv_active_length((CL_BitVector *)CL_OBJ_TO_PTR(seq));
        vec = cl_make_vector((uint32_t)len);
        for (i = 0; i < len; i++)
            cl_vector_data((CL_Vector *)CL_OBJ_TO_PTR(vec))[i] =
                CL_MAKE_FIXNUM(cl_bv_get_bit((CL_BitVector *)CL_OBJ_TO_PTR(seq),
                                             (uint32_t)i));
    } else if (CL_BYTE_VECTOR_P(seq)) {
        len = (int32_t)cl_bytevec_active_length((CL_ByteVector *)CL_OBJ_TO_PTR(seq));
        vec = cl_make_vector((uint32_t)len);
        for (i = 0; i < len; i++)
            cl_vector_data((CL_Vector *)CL_OBJ_TO_PTR(vec))[i] =
                CL_MAKE_FIXNUM(cl_bytevec_get(
                    (CL_ByteVector *)CL_OBJ_TO_PTR(seq), (uint32_t)i));
    } else {
        *out_len = 0;
        cl_error(CL_ERR_TYPE, "MERGE: not a sequence");
        return CL_NIL;
    }
    *out_len = len;
    return vec;
}
#define MERGE_ELT(vec, i) (cl_vector_data((CL_Vector *)CL_OBJ_TO_PTR(vec))[(i)])

/* (merge result-type sequence1 sequence2 predicate &key key)
 * Merge two sorted sequences into one sorted sequence of result-type. */
static CL_Obj bi_merge(CL_Obj *args, int n)
{
    CL_Obj seq1, seq2, pred;
    CL_Obj key_fn = CL_NIL;
    int i, rt;
    int32_t len_constraint = -1;

    /* Classify the result-type FIRST: seq_ctor_type_class can run a deftype
     * expander (cl_vm_apply) that allocates/compacts.  Classification:
     * 0=null 1=list 2=string 3=vector 4=bit-vector; len_constraint is the
     * declared length, or -1 when unspecified / `*`.  Every other operand is
     * read from args[] (GC-rooted VM-stack slots) afterwards, so nothing is
     * live across the expander. */
    rt = seq_ctor_type_class(args[0], 16, &len_constraint);
    if (rt < 0)
        cl_error(CL_ERR_ARGS, "MERGE: unsupported result-type");

    cl_check_seq_keywords(args, n, 4, SK_KEY);
    pred = cl_coerce_funcdesig(args[3], "MERGE");
    CL_GC_PROTECT(pred);   /* the :key coerce below may allocate/compact */
    for (i = 4; i + 1 < n; i += 2) {
        if (args[i] == KW_KEY)
            key_fn = CL_NULL_P(args[i + 1]) ? CL_NIL : cl_coerce_funcdesig(args[i + 1], ":KEY");
    }
    CL_GC_PROTECT(key_fn);
    seq1 = args[1];
    seq2 = args[2];
    CL_GC_PROTECT(seq1);
    CL_GC_PROTECT(seq2);

    /* Fast path: list merge when both inputs are lists */
    if (rt == 1 && (CL_NULL_P(seq1) || CL_CONS_P(seq1)) &&
                   (CL_NULL_P(seq2) || CL_CONS_P(seq2))) {
        CL_Obj r = list_merge(seq1, seq2, pred, key_fn);
        CL_GC_UNPROTECT(4);
        return r;
    }

    /* General path: collect into GC vectors, merge, build result.
     * GC SAFETY: platform_alloc arrays are invisible to the GC — after any
     * compacting :key/pred call their remaining CL_Obj entries are stale.
     * GC vectors are traced and forwarded; only the handles need roots, and
     * the data pointer is re-derived (MERGE_ELT) after every allocation. */
    {
        CL_Obj a1 = CL_NIL, a2 = CL_NIL, out = CL_NIL;
        int32_t n1 = 0, n2 = 0, ntotal, ia, ib, io;
        CL_Obj result = CL_NIL, tail = CL_NIL;

        /* seq1/seq2/pred/key_fn are already rooted above (4 outer roots);
         * the UNPROTECT(7) sites below pop these 3 plus those 4. */
        CL_GC_PROTECT(a1);
        CL_GC_PROTECT(a2);
        CL_GC_PROTECT(out);

        a1 = seq_to_gc_vector(seq1, &n1);
        a2 = seq_to_gc_vector(seq2, &n2);
        ntotal = n1 + n2;
        if (ntotal > 0)
            out = cl_make_vector((uint32_t)ntotal);

        /* Stable merge into out.  Elements are read fresh from the protected
         * vectors on every access; ea/ka are additionally held across the
         * second apply_key / call_test. */
        ia = 0; ib = 0; io = 0;
        while (ia < n1 && ib < n2) {
            CL_Obj ka = CL_NIL, kb;
            CL_GC_PROTECT(ka);  /* rooted while the second apply_key runs */
            ka = apply_key(key_fn, MERGE_ELT(a1, ia));
            kb = apply_key(key_fn, MERGE_ELT(a2, ib));
            /* Take from b only if pred(kb, ka) is true */
            if (!CL_NULL_P(call_test(pred, kb, ka)))
                { MERGE_ELT(out, io) = MERGE_ELT(a2, ib); io++; ib++; }
            else
                { MERGE_ELT(out, io) = MERGE_ELT(a1, ia); io++; ia++; }
            CL_GC_UNPROTECT(1); /* ka */
        }
        while (ia < n1) { MERGE_ELT(out, io) = MERGE_ELT(a1, ia); io++; ia++; }
        while (ib < n2) { MERGE_ELT(out, io) = MERGE_ELT(a2, ib); io++; ib++; }

        /* A declared length must match the actual result length. */
        if (len_constraint >= 0 && len_constraint != ntotal) {
            CL_GC_UNPROTECT(7);
            cl_error(CL_ERR_TYPE, "MERGE: result length does not match result-type");
        }

        if (rt == 0) {
            /* result-type NULL: only the empty merge is well typed. */
            CL_GC_UNPROTECT(7);
            if (ntotal != 0)
                cl_error(CL_ERR_TYPE, "MERGE: non-empty result for result-type NULL");
            return CL_NIL;
        } else if (rt == 1) {
            /* Build list result — out is a traced vector, so its elements
             * stay valid across the cl_cons allocations. */
            CL_GC_PROTECT(result);
            CL_GC_PROTECT(tail);
            for (i = 0; i < ntotal; i++) {
                CL_Obj cell = cl_cons(MERGE_ELT(out, i), CL_NIL);
                if (CL_NULL_P(result)) result = cell;
                else ((CL_Cons *)CL_OBJ_TO_PTR(tail))->cdr = cell;
                tail = cell;
            }
            CL_GC_UNPROTECT(2);
        } else if (rt == 2) {
            /* String result (characters are immediates, but read through the
             * protected vector anyway — it is re-derived after the alloc). */
            result = cl_make_string(NULL, (uint32_t)ntotal);
            for (i = 0; i < ntotal; i++)
                cl_string_set_char_at(result, (uint32_t)i,
                                      CL_CHAR_P(MERGE_ELT(out, i))
                                      ? CL_CHAR_VAL(MERGE_ELT(out, i)) : 0);
        } else if (rt == 4) {
            /* Bit-vector result. */
            result = cl_make_bit_vector((uint32_t)ntotal);
            {
                CL_BitVector *bv = (CL_BitVector *)CL_OBJ_TO_PTR(result);
                for (i = 0; i < ntotal; i++)
                    cl_bv_set_bit(bv, (uint32_t)i,
                                  (CL_FIXNUM_P(MERGE_ELT(out, i)) &&
                                   CL_FIXNUM_VAL(MERGE_ELT(out, i)) != 0));
            }
        } else {
            /* General vector result — re-derive both data pointers AFTER the
             * allocation (the old code read out[] through a pre-alloc pointer). */
            result = cl_make_vector((uint32_t)ntotal);
            for (i = 0; i < ntotal; i++)
                cl_vector_data((CL_Vector *)CL_OBJ_TO_PTR(result))[i] =
                    MERGE_ELT(out, i);
        }

        CL_GC_UNPROTECT(7);
        return result;
    }
}

/* Stable bottom-up merge sort of the first LEN elements of the general
 * vector VEC, in place — O(n log n) predicate calls, one scratch vector of
 * LEN elements.  (Until 0.13 vectors went through an O(n^2) insertion
 * sort: 20,000 elements took 1 s as a vector and 0.1 s as a list.)
 *
 * Takes the vector CL_Obj, NOT a raw data pointer: the :key/predicate
 * calls run arbitrary Lisp whose allocations can compact the heap and
 * relocate both vectors, so every element access re-derives the data
 * pointer from a rooted handle (MERGE_ELT).  The two buffers alternate
 * roles between passes; `in_tmp` says which one currently holds the data,
 * because a plain `src`/`dst` copy of a handle would go stale across a
 * compaction.  The scratch is a GC vector rather than platform_alloc so a
 * user :key/predicate longjmp (handler-case) cannot leak it. */
static void gc_vector_merge_sort(CL_Obj vec, int32_t len, CL_Obj pred, CL_Obj key_fn)
{
    CL_Obj tmp = CL_NIL;
    int32_t width, lo, i;
    int in_tmp = 0;

    if (len <= 1) return;
    CL_GC_PROTECT(vec);
    CL_GC_PROTECT(pred);
    CL_GC_PROTECT(key_fn);
    CL_GC_PROTECT(tmp);
    tmp = cl_make_vector((uint32_t)len);

#define MS_SRC (in_tmp ? tmp : vec)
#define MS_DST (in_tmp ? vec : tmp)
    for (width = 1; width < len; width *= 2) {
        for (lo = 0; lo < len; lo += 2 * width) {
            int32_t mid = lo + width;
            int32_t hi = (len - lo < 2 * width) ? len : lo + 2 * width;
            int32_t ia = lo, ib = mid, io = lo;
            if (mid > len) mid = len;
            /* Stable merge: take from the right run only when its head is
             * strictly less than the left head (pred(kb, ka)); on equal or
             * incomparable keys the left element goes first, which keeps the
             * input order of elements the predicate does not distinguish.
             * ka is rooted across kb's apply_key; kb goes straight into the
             * rooted call_test args. */
            while (ia < mid && ib < hi) {
                CL_Obj ka = CL_NIL, kb;
                CL_GC_PROTECT(ka);
                ka = apply_key(key_fn, MERGE_ELT(MS_SRC, ia));
                kb = apply_key(key_fn, MERGE_ELT(MS_SRC, ib));
                if (!CL_NULL_P(call_test(pred, kb, ka))) {
                    MERGE_ELT(MS_DST, io) = MERGE_ELT(MS_SRC, ib);
                    io++; ib++;
                } else {
                    MERGE_ELT(MS_DST, io) = MERGE_ELT(MS_SRC, ia);
                    io++; ia++;
                }
                CL_GC_UNPROTECT(1); /* ka */
            }
            while (ia < mid) { MERGE_ELT(MS_DST, io) = MERGE_ELT(MS_SRC, ia); io++; ia++; }
            while (ib < hi)  { MERGE_ELT(MS_DST, io) = MERGE_ELT(MS_SRC, ib); io++; ib++; }
        }
        in_tmp = !in_tmp;
    }
    /* The sorted data must end up in VEC: copy back if the last pass left it
     * in the scratch (an odd number of passes). */
    if (in_tmp)
        for (i = 0; i < len; i++) MERGE_ELT(vec, i) = MERGE_ELT(tmp, i);
#undef MS_SRC
#undef MS_DST
    CL_GC_UNPROTECT(4);
}

/* Set element I of a string / bit-vector / vector sequence. */
static void array_seq_set(CL_Obj seq, int32_t i, CL_Obj v)
{
    if (CL_ANY_STRING_P(seq)) {
        if (CL_CHAR_P(v)) cl_string_set_char_at(seq, (uint32_t)i, CL_CHAR_VAL(v));
    } else if (CL_BIT_VECTOR_P(seq)) {
        uint32_t p = (uint32_t)i;  /* cl_bv_set_bit evaluates its index twice */
        cl_bv_set_bit((CL_BitVector *)CL_OBJ_TO_PTR(seq), p,
                      (CL_FIXNUM_P(v) && CL_FIXNUM_VAL(v) != 0));
    } else if (CL_BYTE_VECTOR_P(seq)) {
        /* Used by SORT/NREVERSE-style permutations, where v originates from
         * the same sequence and is always in range; silently skip non-fixnums
         * to mirror the string/bit branches above. */
        CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(seq);
        if (CL_FIXNUM_P(v)) cl_bytevec_set(bv, i, CL_FIXNUM_VAL(v));
    } else if (CL_VECTOR_P(seq)) {
        cl_vector_data((CL_Vector *)CL_OBJ_TO_PTR(seq))[i] = v;
    }
}

/* Stable sort for string / bit-vector / byte-vector sequences: the elements
 * (immediates — characters / bits / bytes) are snapshotted into a GC vector,
 * merge-sorted there, and written back.  A GC vector rather than
 * platform_alloc so a user :key/predicate longjmp (handler-case) cannot leak
 * the snapshot permanently; MERGE_ELT re-derives its data pointer after the
 * sort's own allocations and user calls. */
static void array_seq_sort(CL_Obj seq, int32_t len, CL_Obj pred, CL_Obj key_fn)
{
    int32_t i;
    CL_Obj tmpv = CL_NIL;
    if (len <= 1) return;
    CL_GC_PROTECT(seq);    /* the snapshot alloc + user calls may compact */
    CL_GC_PROTECT(pred);
    CL_GC_PROTECT(key_fn);
    CL_GC_PROTECT(tmpv);
    tmpv = cl_make_vector((uint32_t)len);
    for (i = 0; i < len; i++) MERGE_ELT(tmpv, i) = seq_elt(seq, i);
    gc_vector_merge_sort(tmpv, len, pred, key_fn);
    for (i = 0; i < len; i++) array_seq_set(seq, i, MERGE_ELT(tmpv, i));
    CL_GC_UNPROTECT(4);
}

static CL_Obj bi_sort(CL_Obj *args, int n)
{
    CL_Obj seq = args[0], pred = cl_coerce_funcdesig(args[1], "SORT");
    CL_Obj key_fn = CL_NIL;
    int i;

    cl_check_seq_keywords(args, n, 2, SK_KEY);
    /* Parse :key */
    for (i = 2; i + 1 < n; i += 2) {
        if (args[i] == KW_KEY)
            key_fn = CL_NULL_P(args[i + 1]) ? CL_NIL : cl_coerce_funcdesig(args[i + 1], ":KEY");
    }

    if (CL_NULL_P(seq)) return CL_NIL;

    if (CL_CONS_P(seq)) {
        return list_merge_sort(seq, pred, key_fn);
    }

    if (CL_VECTOR_P(seq)) {
        CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(seq);
        gc_vector_merge_sort(seq, (int32_t)cl_vector_active_length(v), pred, key_fn);
        return args[0];
    }

    if (CL_ANY_STRING_P(seq) || CL_BIT_VECTOR_P(seq) || CL_BYTE_VECTOR_P(seq)) {
        array_seq_sort(seq, seq_length(seq), pred, key_fn);
        return args[0];
    }

    cl_signal_type_error(seq, "SEQUENCE", "SORT");
    return CL_NIL;
}

static CL_Obj bi_stable_sort(CL_Obj *args, int n)
{
    /* Both the list and the vector merge sort are stable */
    return bi_sort(args, n);
}

/* ======================================================= */
/* Registration                                            */
/* ======================================================= */

void cl_builtins_sequence2_init(void)
{
    /* Pre-intern keyword symbols */
    KW_TEST   = cl_intern_keyword("TEST", 4);
    KW_TEST_NOT = cl_intern_keyword("TEST-NOT", 8);
    KW_KEY    = cl_intern_keyword("KEY", 3);
    KW_START1 = cl_intern_keyword("START1", 6);
    KW_END1   = cl_intern_keyword("END1", 4);
    KW_START2 = cl_intern_keyword("START2", 6);
    KW_END2   = cl_intern_keyword("END2", 4);
    KW_FROM_END = cl_intern_keyword("FROM-END", 8);

    /* Cache eql function */
    {
        CL_Obj eql_sym = cl_intern_in("EQL", 3, cl_package_cl);
        CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(eql_sym);
        SYM_EQL_FN = s->function;
    }

    /* Cache 'list symbol */
    SYM_LIST = cl_intern_in("LIST", 4, cl_package_cl);

    /* Quantifiers */
    defun("EVERY", bi_every, 2, -1);
    defun("SOME", bi_some, 2, -1);
    defun("NOTANY", bi_notany, 2, -1);
    defun("NOTEVERY", bi_notevery, 2, -1);

    /* Map (generic) */
    defun("MAP", bi_map, 3, -1);

    /* Comparison/search */
    defun("MISMATCH", bi_mismatch, 2, -1);
    defun("SEARCH", bi_search, 2, -1);

    /* Merge */
    defun("MERGE", bi_merge, 4, -1);

    /* Sorting */
    defun("SORT", bi_sort, 2, -1);
    defun("STABLE-SORT", bi_stable_sort, 2, -1);

    /* Register cached symbols for GC compaction forwarding */
    cl_gc_register_root(&KW_TEST);
    cl_gc_register_root(&KW_TEST_NOT);
    cl_gc_register_root(&KW_KEY);
    cl_gc_register_root(&KW_START1);
    cl_gc_register_root(&KW_END1);
    cl_gc_register_root(&KW_START2);
    cl_gc_register_root(&KW_END2);
    cl_gc_register_root(&KW_FROM_END);
    cl_gc_register_root(&SYM_EQL_FN);
    cl_gc_register_root(&SYM_LIST);
}
