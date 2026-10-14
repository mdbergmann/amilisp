/*
 * builtins_bitvector.c — Bit vector predicates, element access, and bitwise operations
 */

#include "builtins.h"
#include "symbol.h"
#include "package.h"
#include "mem.h"
#include "error.h"
#include "../platform/platform.h"
#include <string.h>

/* ======================================================= */
/* PREDICATES                                              */
/* ======================================================= */

DEFINE_TYPE_PREDICATE(bi_bit_vector_p, CL_BIT_VECTOR_P)

static CL_Obj bi_simple_bit_vector_p(CL_Obj *args, int n)
{
    CL_BitVector *bv;
    CL_UNUSED(n);
    if (!CL_BIT_VECTOR_P(args[0])) return CL_NIL;
    bv = (CL_BitVector *)CL_OBJ_TO_PTR(args[0]);
    return (bv->flags == 0) ? SYM_T : CL_NIL;
}

/* ======================================================= */
/* BIT / SBIT element access                               */
/* ======================================================= */

static CL_Obj bi_bit(CL_Obj *args, int n)
{
    CL_BitVector *bv;
    int32_t idx;
    CL_UNUSED(n);
    if (!CL_BIT_VECTOR_P(args[0]))
        cl_error(CL_ERR_TYPE, "BIT: not a bit vector");
    if (!CL_FIXNUM_P(args[1]))
        cl_error(CL_ERR_TYPE, "BIT: index must be a fixnum");
    bv = (CL_BitVector *)CL_OBJ_TO_PTR(args[0]);
    idx = CL_FIXNUM_VAL(args[1]);
    if (idx < 0 || (uint32_t)idx >= cl_bv_active_length(bv))
        cl_error(CL_ERR_ARGS, "BIT: index %d out of range (length %d)",
                 (int)idx, (int)cl_bv_active_length(bv));
    return CL_MAKE_FIXNUM(cl_bv_get_bit(bv, (uint32_t)idx));
}

static CL_Obj bi_sbit(CL_Obj *args, int n)
{
    CL_BitVector *bv;
    int32_t idx;
    CL_UNUSED(n);
    if (!CL_BIT_VECTOR_P(args[0]))
        cl_error(CL_ERR_TYPE, "SBIT: not a bit vector");
    bv = (CL_BitVector *)CL_OBJ_TO_PTR(args[0]);
    if (bv->flags != 0)
        cl_error(CL_ERR_TYPE, "SBIT: not a simple bit vector");
    if (!CL_FIXNUM_P(args[1]))
        cl_error(CL_ERR_TYPE, "SBIT: index must be a fixnum");
    idx = CL_FIXNUM_VAL(args[1]);
    if (idx < 0 || (uint32_t)idx >= bv->length)
        cl_error(CL_ERR_ARGS, "SBIT: index %d out of range", (int)idx);
    return CL_MAKE_FIXNUM(cl_bv_get_bit(bv, (uint32_t)idx));
}

/* (%SETF-BIT bv idx val) -> val */
static CL_Obj bi_setf_bit(CL_Obj *args, int n)
{
    CL_BitVector *bv;
    int32_t idx, val;
    CL_UNUSED(n);
    if (!CL_BIT_VECTOR_P(args[0]))
        cl_error(CL_ERR_TYPE, "(SETF BIT): not a bit vector");
    if (!CL_FIXNUM_P(args[1]))
        cl_error(CL_ERR_TYPE, "(SETF BIT): index must be a fixnum");
    if (!CL_FIXNUM_P(args[2]))
        cl_error(CL_ERR_TYPE, "(SETF BIT): value must be 0 or 1");
    bv = (CL_BitVector *)CL_OBJ_TO_PTR(args[0]);
    idx = CL_FIXNUM_VAL(args[1]);
    val = CL_FIXNUM_VAL(args[2]);
    if (idx < 0 || (uint32_t)idx >= cl_bv_active_length(bv))
        cl_error(CL_ERR_ARGS, "(SETF BIT): index %d out of range", (int)idx);
    if (val != 0 && val != 1)
        cl_error(CL_ERR_TYPE, "(SETF BIT): value must be 0 or 1, got %d", (int)val);
    cl_bv_set_bit(bv, (uint32_t)idx, val);
    return args[2];
}

/* (%SETF-SBIT bv idx val) -> val */
static CL_Obj bi_setf_sbit(CL_Obj *args, int n)
{
    CL_BitVector *bv;
    int32_t idx, val;
    CL_UNUSED(n);
    if (!CL_BIT_VECTOR_P(args[0]))
        cl_error(CL_ERR_TYPE, "(SETF SBIT): not a bit vector");
    bv = (CL_BitVector *)CL_OBJ_TO_PTR(args[0]);
    if (bv->flags != 0)
        cl_error(CL_ERR_TYPE, "(SETF SBIT): not a simple bit vector");
    if (!CL_FIXNUM_P(args[1]))
        cl_error(CL_ERR_TYPE, "(SETF SBIT): index must be a fixnum");
    if (!CL_FIXNUM_P(args[2]))
        cl_error(CL_ERR_TYPE, "(SETF SBIT): value must be 0 or 1");
    idx = CL_FIXNUM_VAL(args[1]);
    val = CL_FIXNUM_VAL(args[2]);
    if (idx < 0 || (uint32_t)idx >= bv->length)
        cl_error(CL_ERR_ARGS, "(SETF SBIT): index %d out of range", (int)idx);
    if (val != 0 && val != 1)
        cl_error(CL_ERR_TYPE, "(SETF SBIT): value must be 0 or 1, got %d", (int)val);
    cl_bv_set_bit(bv, (uint32_t)idx, val);
    return args[2];
}

/* ======================================================= */
/* BITWISE ARRAY OPERATIONS                                */
/* ======================================================= */

/* Zero trailing bits beyond length in last word */
static void bv_mask_trailing(CL_BitVector *bv)
{
    uint32_t tail = bv->length % 32;
    if (tail != 0 && bv->length > 0) {
        uint32_t nwords = CL_BV_WORDS(bv->length);
        bv->data[nwords - 1] &= (1u << tail) - 1;
    }
}

/* Resolve the optional result-bit-array argument per CL spec:
 *   NIL  -> allocate new
 *   T    -> modify first arg in place
 *   bv   -> use that bit vector
 */
static CL_BitVector *resolve_result(CL_Obj opt_result, CL_BitVector *bv1, uint32_t len)
{
    if (CL_NULL_P(opt_result)) {
        CL_Obj r = cl_make_bit_vector(len);
        return (CL_BitVector *)CL_OBJ_TO_PTR(r);
    }
    if (opt_result == SYM_T)
        return bv1;
    if (CL_BIT_VECTOR_P(opt_result)) {
        CL_BitVector *rbv = (CL_BitVector *)CL_OBJ_TO_PTR(opt_result);
        if (rbv->length != len)
            cl_error(CL_ERR_ARGS, "BIT-*: result bit vector length mismatch");
        return rbv;
    }
    cl_error(CL_ERR_TYPE, "BIT-*: result-bit-array must be NIL, T, or a bit vector");
    return NULL;
}

/* Binary bitwise operation */
typedef uint32_t (*BvBinOp)(uint32_t a, uint32_t b);

static CL_Obj bitvec_binop(CL_Obj *args, int n, BvBinOp op)
{
    CL_BitVector *bv1, *bv2, *result;
    uint32_t nwords, i;
    CL_Obj opt_result = CL_NIL;

    if (!CL_BIT_VECTOR_P(args[0]))
        cl_error(CL_ERR_TYPE, "BIT-*: first argument not a bit vector");
    if (!CL_BIT_VECTOR_P(args[1]))
        cl_error(CL_ERR_TYPE, "BIT-*: second argument not a bit vector");
    bv1 = (CL_BitVector *)CL_OBJ_TO_PTR(args[0]);
    bv2 = (CL_BitVector *)CL_OBJ_TO_PTR(args[1]);
    if (bv1->length != bv2->length)
        cl_error(CL_ERR_ARGS, "BIT-*: bit vectors must be same length");
    if (n > 2) opt_result = args[2];

    nwords = CL_BV_WORDS(bv1->length);
    result = resolve_result(opt_result, bv1, bv1->length);
    /* resolve_result allocates when the result arg is NIL/omitted — the
     * source pointers dangle after the compaction; re-derive from the
     * rooted args[] before the copy loop. */
    bv1 = (CL_BitVector *)CL_OBJ_TO_PTR(args[0]);
    bv2 = (CL_BitVector *)CL_OBJ_TO_PTR(args[1]);

    for (i = 0; i < nwords; i++)
        result->data[i] = op(bv1->data[i], bv2->data[i]);
    bv_mask_trailing(result);
    return CL_PTR_TO_OBJ(result);
}

static uint32_t op_and(uint32_t a, uint32_t b) { return a & b; }
static uint32_t op_ior(uint32_t a, uint32_t b) { return a | b; }
static uint32_t op_xor(uint32_t a, uint32_t b) { return a ^ b; }
static uint32_t op_eqv(uint32_t a, uint32_t b) { return ~(a ^ b); }
static uint32_t op_nand(uint32_t a, uint32_t b) { return ~(a & b); }
static uint32_t op_nor(uint32_t a, uint32_t b) { return ~(a | b); }
static uint32_t op_andc1(uint32_t a, uint32_t b) { return ~a & b; }
static uint32_t op_andc2(uint32_t a, uint32_t b) { return a & ~b; }
static uint32_t op_orc1(uint32_t a, uint32_t b) { return ~a | b; }
static uint32_t op_orc2(uint32_t a, uint32_t b) { return a | ~b; }

static CL_Obj bi_bit_and(CL_Obj *args, int n) { return bitvec_binop(args, n, op_and); }
static CL_Obj bi_bit_ior(CL_Obj *args, int n) { return bitvec_binop(args, n, op_ior); }
static CL_Obj bi_bit_xor(CL_Obj *args, int n) { return bitvec_binop(args, n, op_xor); }
static CL_Obj bi_bit_eqv(CL_Obj *args, int n) { return bitvec_binop(args, n, op_eqv); }
static CL_Obj bi_bit_nand(CL_Obj *args, int n) { return bitvec_binop(args, n, op_nand); }
static CL_Obj bi_bit_nor(CL_Obj *args, int n) { return bitvec_binop(args, n, op_nor); }
static CL_Obj bi_bit_andc1(CL_Obj *args, int n) { return bitvec_binop(args, n, op_andc1); }
static CL_Obj bi_bit_andc2(CL_Obj *args, int n) { return bitvec_binop(args, n, op_andc2); }
static CL_Obj bi_bit_orc1(CL_Obj *args, int n) { return bitvec_binop(args, n, op_orc1); }
static CL_Obj bi_bit_orc2(CL_Obj *args, int n) { return bitvec_binop(args, n, op_orc2); }

/* BIT-NOT: unary */
static CL_Obj bi_bit_not(CL_Obj *args, int n)
{
    CL_BitVector *bv, *result;
    uint32_t nwords, i;
    CL_Obj opt_result = CL_NIL;

    if (!CL_BIT_VECTOR_P(args[0]))
        cl_error(CL_ERR_TYPE, "BIT-NOT: not a bit vector");
    bv = (CL_BitVector *)CL_OBJ_TO_PTR(args[0]);
    if (n > 1) opt_result = args[1];

    nwords = CL_BV_WORDS(bv->length);
    result = resolve_result(opt_result, bv, bv->length);
    /* Re-derive the source after resolve_result's possible allocation. */
    bv = (CL_BitVector *)CL_OBJ_TO_PTR(args[0]);

    for (i = 0; i < nwords; i++)
        result->data[i] = ~bv->data[i];
    bv_mask_trailing(result);
    return CL_PTR_TO_OBJ(result);
}

/* ======================================================= */
/* Registration                                            */
/* ======================================================= */

void cl_builtins_bitvector_init(void)
{
    /* Predicates */
    defun("BIT-VECTOR-P", bi_bit_vector_p, 1, 1);
    defun("SIMPLE-BIT-VECTOR-P", bi_simple_bit_vector_p, 1, 1);

    /* Element access */
    defun("BIT", bi_bit, 2, 2);
    defun("SBIT", bi_sbit, 2, 2);
    cl_register_builtin("%SETF-BIT", bi_setf_bit, 3, 3, cl_package_amilisp);
    cl_register_builtin("%SETF-SBIT", bi_setf_sbit, 3, 3, cl_package_amilisp);

    /* Bitwise array operations */
    defun("BIT-AND", bi_bit_and, 2, 3);
    defun("BIT-IOR", bi_bit_ior, 2, 3);
    defun("BIT-XOR", bi_bit_xor, 2, 3);
    defun("BIT-EQV", bi_bit_eqv, 2, 3);
    defun("BIT-NAND", bi_bit_nand, 2, 3);
    defun("BIT-NOR", bi_bit_nor, 2, 3);
    defun("BIT-ANDC1", bi_bit_andc1, 2, 3);
    defun("BIT-ANDC2", bi_bit_andc2, 2, 3);
    defun("BIT-ORC1", bi_bit_orc1, 2, 3);
    defun("BIT-ORC2", bi_bit_orc2, 2, 3);
    defun("BIT-NOT", bi_bit_not, 1, 2);
}
