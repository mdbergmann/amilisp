#include "builtins.h"
#include "bignum.h"
#include "ratio.h"
#include "float.h"
#include "symbol.h"
#include "package.h"
#include "mem.h"
#include "vm.h"
#include "error.h"
#include "../platform/platform.h"
#include <string.h>
#include <math.h>

/* m68k-amigaos-gcc's <math.h> does not define M_PI (it's a POSIX
 * extension, not C89/C99).  Provide a fallback so PHASE compiles. */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void check_number(CL_Obj obj, const char *op)
{
    if (!CL_NUMBER_P(obj))
        cl_signal_type_error(obj, "NUMBER", op);
}

static void check_integer(CL_Obj obj, const char *op)
{
    if (!CL_INTEGER_P(obj))
        cl_signal_type_error(obj, "INTEGER", op);
}

static void check_real(CL_Obj obj, const char *op)
{
    if (!CL_REALP(obj))
        cl_signal_type_error(obj, "REAL", op);
}

/* --- Rounding helpers --- */

/* Forward declaration (defined later in this file for integer-decode-float) */
static CL_Obj double_int_to_obj(double val);

/* Convert a whole-number double (positive or negative) to fixnum or bignum.
   For magnitudes beyond ~2^48 the old multi-limb chunking lost high bits
   (FLOOR / TRUNCATE of MOST-POSITIVE-SINGLE-FLOAT returned int32 garbage),
   so fall back to the exact IEEE-bit-pattern rational which is bignum-safe
   across the entire double-float range. The caller guarantees val is whole,
   so the result is an integer. */
static CL_Obj double_to_integer(double val)
{
    if (val >= (double)CL_FIXNUM_MIN && val <= (double)CL_FIXNUM_MAX)
        return CL_MAKE_FIXNUM((int32_t)val);
    return cl_float_to_exact_rational(cl_make_double_float(val));
}

/* Round-to-nearest-even (banker's rounding) */
static double cl_round_even(double val)
{
    double down = floor(val);
    double frac = val - down;
    if (frac > 0.5) return down + 1.0;
    if (frac < 0.5) return down;
    /* Exactly 0.5: round to nearest even */
    if (fmod(down, 2.0) == 0.0) return down;
    return down + 1.0;
}

enum { ROUND_FLOOR, ROUND_CEILING, ROUND_TRUNCATE, ROUND_ROUND };

/* Apply rounding mode to a double */
static double apply_round(double val, int mode)
{
    switch (mode) {
    case ROUND_FLOOR:    return floor(val);
    case ROUND_CEILING:  return ceil(val);
    case ROUND_TRUNCATE: return (val >= 0.0) ? floor(val) : ceil(val);
    case ROUND_ROUND:    return cl_round_even(val);
    }
    return val;
}

/* Core rounding: floor/ceiling/truncate/round with 1 or 2 args.
   If float_quotient is true, quotient is returned as float (f-variants). */
static CL_Obj do_rounding(CL_Obj *args, int n, int mode,
                           const char *name, int float_quotient)
{
    CL_Obj num = args[0];
    check_number(num, name);

    if (n == 1) {
        /* Single argument */
        if (CL_INTEGER_P(num)) {
            CL_Obj q, r;
            if (float_quotient) {
                q = cl_make_single_float((float)cl_to_double(num));
                r = CL_MAKE_FIXNUM(0);
            } else {
                q = num;
                r = CL_MAKE_FIXNUM(0);
            }
            cl_mv_count = 2;
            cl_mv_values[0] = q;
            cl_mv_values[1] = r;
            return q;
        }
        /* Ratio: (floor p/q) returns the integer quotient AND a ratio
         * remainder (num - quotient).  Cannot delegate to the 2-arg
         * (floor p q) form — that returns r = p - q*divisor (= an
         * integer), but the 1-arg form requires r = num - quotient
         * (= a ratio).  Compute via 2-arg, then patch r. */
        if (CL_RATIO_P(num)) {
            CL_Obj pair[2];
            CL_Obj q;
            pair[0] = cl_numerator(num);
            pair[1] = cl_denominator(num);
            /* pair[] is a plain C array — invisible to GC.  The recursive
             * call allocates throughout and re-reads both slots via its
             * args pointer, so root them here. */
            CL_GC_PROTECT(pair[0]);
            CL_GC_PROTECT(pair[1]);
            q = do_rounding(pair, 2, mode, name, float_quotient);
            CL_GC_UNPROTECT(2);
            CL_GC_PROTECT(q);
            if (float_quotient) {
                CL_Obj iq = double_to_integer(cl_to_double(q));
                cl_mv_values[1] = cl_arith_sub(args[0], iq);
            } else {
                cl_mv_values[1] = cl_arith_sub(args[0], q);
            }
            CL_GC_UNPROTECT(1);
            cl_mv_values[0] = q;
            cl_mv_count = 2;
            return q;
        }
        /* Float argument */
        {
            double val = cl_to_double(num);
            double dq = apply_round(val, mode);
            double dr = val - dq;
            int is_dbl = CL_DOUBLE_FLOAT_P(num);
            CL_Obj qobj, robj;
            if (float_quotient)
                qobj = is_dbl ? cl_make_double_float(dq)
                              : cl_make_single_float((float)dq);
            else
                qobj = double_to_integer(dq);
            /* Protect qobj: when dq is large, double_to_integer allocates a
             * bignum, and the cl_make_*_float(dr) below can compact, leaving
             * an unprotected qobj pointing at moved-from memory. */
            CL_GC_PROTECT(qobj);
            robj = is_dbl ? cl_make_double_float(dr)
                          : cl_make_single_float((float)dr);
            CL_GC_UNPROTECT(1);
            cl_mv_count = 2;
            cl_mv_values[0] = qobj;
            cl_mv_values[1] = robj;
            return qobj;
        }
    }

    /* Two arguments */
    check_number(args[1], name);

    if (CL_RATIONAL_P(num) && CL_RATIONAL_P(args[1])) {
        /* Rational path: exact arithmetic.
         * For ratios, cross-multiply to get integer division:
         * (floor a_n/a_d  b_n/b_d) = (floor a_n*b_d  a_d*b_n)
         * then remainder = num - q * args[1] */
        CL_Obj int_a, int_b, q, r;
        /* args[] is rooted (VM stack or a protected pair[] from the 1-arg
         * ratio path), but the num local copy goes stale after every
         * allocating call — re-read args[0] instead.  q and r stay
         * protected across the whole adjust sequence: each add/sub below
         * can allocate and would leave the other one stale. */
        int_a = cl_arith_mul(cl_numerator(num), cl_denominator(args[1]));
        CL_GC_PROTECT(int_a);
        int_b = cl_arith_mul(cl_denominator(args[0]), cl_numerator(args[1]));
        CL_GC_UNPROTECT(1);

        q = cl_arith_truncate(int_a, int_b);
        CL_GC_PROTECT(q);
        r = cl_arith_mul(q, args[1]);
        r = cl_arith_sub(args[0], r);
        CL_GC_PROTECT(r);

        /* Adjust based on rounding mode */
        if (mode != ROUND_TRUNCATE && !cl_arith_zerop(r)) {
            if (mode == ROUND_FLOOR) {
                if (cl_arith_minusp(r) != cl_arith_minusp(args[1])) {
                    q = cl_arith_sub(q, CL_MAKE_FIXNUM(1));
                    r = cl_arith_add(r, args[1]);
                }
            } else if (mode == ROUND_CEILING) {
                if (cl_arith_minusp(r) == cl_arith_minusp(args[1])) {
                    q = cl_arith_add(q, CL_MAKE_FIXNUM(1));
                    r = cl_arith_sub(r, args[1]);
                }
            } else { /* ROUND_ROUND */
                CL_Obj abs_2r, abs_b;
                int cmp;
                abs_2r = cl_arith_add(r, r);
                abs_2r = cl_arith_abs(abs_2r);
                CL_GC_PROTECT(abs_2r);
                abs_b = cl_arith_abs(args[1]);
                cmp = cl_arith_compare(abs_2r, abs_b);
                CL_GC_UNPROTECT(1);
                if (cmp > 0 || (cmp == 0 && !cl_arith_evenp(q))) {
                    if (cl_arith_minusp(args[0]) == cl_arith_minusp(args[1])) {
                        q = cl_arith_add(q, CL_MAKE_FIXNUM(1));
                        r = cl_arith_sub(r, args[1]);
                    } else {
                        q = cl_arith_sub(q, CL_MAKE_FIXNUM(1));
                        r = cl_arith_add(r, args[1]);
                    }
                }
            }
        }

        if (float_quotient)
            q = cl_make_single_float((float)cl_to_double(q));

        cl_mv_count = 2;
        cl_mv_values[0] = q;
        cl_mv_values[1] = r;
        CL_GC_UNPROTECT(2);
        return q;
    }

    /* Float path */
    {
        double a = cl_to_double(num);
        double b = cl_to_double(args[1]);
        double quot, dq, dr;
        int is_dbl = CL_DOUBLE_FLOAT_P(num) || CL_DOUBLE_FLOAT_P(args[1]);
        CL_Obj qobj, robj;
        if (b == 0.0) cl_error(CL_ERR_DIVZERO, "Division by zero");
        /* Compute the quotient at the operands' contagion precision: when
         * both are single floats, divide in single precision so the result
         * matches (/ a b).  Promoting to double first can yield e.g.
         * 6.9999998 where the single-float quotient rounds to 7.0f, which
         * would make (floor a b) disagree with (floor (/ a b)) and round
         * the wrong way at exact-integer boundaries (CLHS 12.1.4.4). */
        if (is_dbl) {
            /* Round the quotient through a genuine 64-bit store (volatile)
             * before rounding it to an integer.  On excess-precision FPUs
             * (68881 extended arithmetic, FPU emulators and FPGA FPUs that
             * ignore the FPCR precision field) a register-resident a/b can
             * sit a hair below an exact integer while the stored double
             * rounds up to it — apply_round must see exactly the double
             * that (/ a b) boxes, or (floor a b) answers 6 where
             * (floor (/ a b)) answers 7. */
            volatile double vq = a / b;
            quot = vq;
        } else {
            /* Same store-rounding for the single-precision quotient: the
             * (float) cast alone is not a reliable rounding point under
             * excess precision (gcc keeps the value in an extended FP
             * register), but a volatile float store is. */
            volatile float vqs = (float)a / (float)b;
            quot = (double)vqs;
        }
        dq = apply_round(quot, mode);
        /* Compute the remainder with the product rounded to a double in its
         * own statement (the `volatile` defeats FMA contraction of a-dq*b).
         * An FMA would evaluate a - dq*b to its exact value, which for FLOOR
         * can be a tiny remainder of the WRONG sign (e.g. -2e-17 against a
         * positive divisor) — violating the CL contract that the remainder
         * share the divisor's sign, disagreeing with the rounded quotient,
         * and signalling SVREF-out-of-range when callers index by it.
         * Rounding the product keeps r consistent with q (matches SBCL). */
        {
            volatile double prod = dq * b;
            dr = a - prod;
        }
        if (float_quotient)
            qobj = is_dbl ? cl_make_double_float(dq)
                          : cl_make_single_float((float)dq);
        else
            qobj = double_to_integer(dq);
        /* Protect qobj across the remainder allocation — see the 1-arg float
         * path above for why an unprotected bignum quotient goes stale here. */
        CL_GC_PROTECT(qobj);
        robj = is_dbl ? cl_make_double_float(dr)
                      : cl_make_single_float((float)dr);
        CL_GC_UNPROTECT(1);
        cl_mv_count = 2;
        cl_mv_values[0] = qobj;
        cl_mv_values[1] = robj;
        return qobj;
    }
}

/* --- Arithmetic --- */

static CL_Obj bi_add(CL_Obj *args, int n)
{
    CL_Obj result = CL_MAKE_FIXNUM(0);
    int i;
    /* (+) → 0; (+ x) → x identically (no float contagion against 0). */
    if (n == 0) return result;
    if (n == 1) { check_number(args[0], "+"); return args[0]; }
    CL_GC_PROTECT(result);
    for (i = 0; i < n; i++) {
        check_number(args[i], "+");
        result = cl_arith_add(result, args[i]);
    }
    CL_GC_UNPROTECT(1);
    return result;
}

static CL_Obj bi_sub(CL_Obj *args, int n)
{
    CL_Obj result;
    int i;
    if (n == 0) return CL_MAKE_FIXNUM(0);
    check_number(args[0], "-");
    if (n == 1) return cl_arith_negate(args[0]);
    result = args[0];
    CL_GC_PROTECT(result);
    for (i = 1; i < n; i++) {
        check_number(args[i], "-");
        result = cl_arith_sub(result, args[i]);
    }
    CL_GC_UNPROTECT(1);
    return result;
}

static CL_Obj bi_mul(CL_Obj *args, int n)
{
    CL_Obj result = CL_MAKE_FIXNUM(1);
    int i;
    /* (*) → 1; (* x) → x identically (no float contagion against 1). */
    if (n == 0) return result;
    if (n == 1) { check_number(args[0], "*"); return args[0]; }
    CL_GC_PROTECT(result);
    for (i = 0; i < n; i++) {
        check_number(args[i], "*");
        result = cl_arith_mul(result, args[i]);
    }
    CL_GC_UNPROTECT(1);
    return result;
}

static CL_Obj bi_div(CL_Obj *args, int n)
{
    CL_Obj result;
    int i;
    check_number(args[0], "/");
    for (i = 0; i < n; i++)
        check_number(args[i], "/");
    if (n == 1)
        return cl_arith_div(CL_MAKE_FIXNUM(1), args[0]);
    result = args[0];
    CL_GC_PROTECT(result);
    for (i = 1; i < n; i++)
        result = cl_arith_div(result, args[i]);
    CL_GC_UNPROTECT(1);
    return result;
}

/* --- Rounding builtins --- */

static CL_Obj bi_floor(CL_Obj *args, int n)
{ return do_rounding(args, n, ROUND_FLOOR, "FLOOR", 0); }

static CL_Obj bi_ceiling(CL_Obj *args, int n)
{ return do_rounding(args, n, ROUND_CEILING, "CEILING", 0); }

static CL_Obj bi_truncate(CL_Obj *args, int n)
{ return do_rounding(args, n, ROUND_TRUNCATE, "TRUNCATE", 0); }

static CL_Obj bi_round(CL_Obj *args, int n)
{ return do_rounding(args, n, ROUND_ROUND, "ROUND", 0); }

static CL_Obj bi_ffloor(CL_Obj *args, int n)
{ return do_rounding(args, n, ROUND_FLOOR, "FFLOOR", 1); }

static CL_Obj bi_fceiling(CL_Obj *args, int n)
{ return do_rounding(args, n, ROUND_CEILING, "FCEILING", 1); }

static CL_Obj bi_ftruncate(CL_Obj *args, int n)
{ return do_rounding(args, n, ROUND_TRUNCATE, "FTRUNCATE", 1); }

static CL_Obj bi_fround(CL_Obj *args, int n)
{ return do_rounding(args, n, ROUND_ROUND, "FROUND", 1); }

/* (rem number divisor) — truncation remainder (sign follows dividend) */
static CL_Obj bi_rem(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    check_number(args[0], "REM");
    check_number(args[1], "REM");
    if (CL_FLOATP(args[0]) || CL_FLOATP(args[1])) {
        double a = cl_to_double(args[0]);
        double b = cl_to_double(args[1]);
        double r;
        int is_dbl = CL_DOUBLE_FLOAT_P(args[0]) || CL_DOUBLE_FLOAT_P(args[1]);
        if (b == 0.0) cl_error(CL_ERR_DIVZERO, "Division by zero");
        r = fmod(a, b);
        return is_dbl ? cl_make_double_float(r) : cl_make_single_float((float)r);
    }
    /* Ratio case: rem(a,b) = a - truncate(a,b) * b.
     * args[] is rooted — read a/b from it fresh after each alloc instead
     * of keeping local copies that go stale. */
    if (CL_RATIO_P(args[0]) || CL_RATIO_P(args[1])) {
        /* Compute a/b as ratio, then truncate to integer */
        CL_Obj quot = cl_ratio_div(args[0], args[1]);
        CL_Obj trunc_q, result;
        if (CL_INTEGER_P(quot)) {
            trunc_q = quot;
        } else {
            /* Truncate ratio to integer: truncate(num/den) */
            trunc_q = cl_arith_truncate(cl_numerator(quot), cl_denominator(quot));
        }
        CL_GC_PROTECT(trunc_q);
        result = cl_arith_mul(trunc_q, args[1]);
        result = cl_arith_sub(args[0], result);
        CL_GC_UNPROTECT(1);
        return result;
    }
    /* Integer case */
    if (CL_FIXNUM_P(args[0]) && CL_FIXNUM_P(args[1])) {
        int32_t va = CL_FIXNUM_VAL(args[0]);
        int32_t vb = CL_FIXNUM_VAL(args[1]);
        if (vb == 0) cl_error(CL_ERR_DIVZERO, "Division by zero");
        return CL_MAKE_FIXNUM(va % vb);
    }
    {
        /* One allocating call per statement: args[0] is read only after the
         * product exists (mem.h, cl_list2). */
        CL_Obj q = cl_arith_truncate(args[0], args[1]);
        CL_Obj prod = cl_arith_mul(q, args[1]);
        return cl_arith_sub(args[0], prod);
    }
}

/* (mod number divisor) — floor remainder (sign follows divisor) */
static CL_Obj bi_mod(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    check_number(args[0], "MOD");
    check_number(args[1], "MOD");
    if (CL_FLOATP(args[0]) || CL_FLOATP(args[1])) {
        double a = cl_to_double(args[0]);
        double b = cl_to_double(args[1]);
        double r;
        int is_dbl = CL_DOUBLE_FLOAT_P(args[0]) || CL_DOUBLE_FLOAT_P(args[1]);
        if (b == 0.0) cl_error(CL_ERR_DIVZERO, "Division by zero");
        r = fmod(a, b);
        /* CL mod: result has same sign as divisor */
        if (r != 0.0 && ((r < 0.0) != (b < 0.0)))
            r += b;
        return is_dbl ? cl_make_double_float(r) : cl_make_single_float((float)r);
    }
    /* Ratio case: mod(a,b) = a - floor(a,b) * b.
     * Everything held across an allocating call is protected BEFORE the
     * alloc (the old code protected after — rooting already-stale values),
     * and a/b are re-read from the rooted args[] instead of local copies. */
    if (CL_RATIO_P(args[0]) || CL_RATIO_P(args[1])) {
        CL_Obj quot = cl_ratio_div(args[0], args[1]);
        CL_Obj floor_q, result;
        if (CL_INTEGER_P(quot)) {
            floor_q = quot;
        } else {
            /* Floor ratio: floor(num/den), adjusting toward -inf. */
            CL_Obj num = cl_numerator(quot);
            CL_Obj den = cl_denominator(quot);
            CL_Obj trunc_q, check;
            int neg = cl_arith_minusp(quot);
            CL_GC_PROTECT(num);
            CL_GC_PROTECT(den);
            trunc_q = cl_arith_truncate(num, den);
            CL_GC_PROTECT(trunc_q);
            check = cl_arith_mul(trunc_q, den);
            if (cl_arith_compare(check, num) != 0 && neg) {
                floor_q = cl_arith_sub(trunc_q, CL_MAKE_FIXNUM(1));
            } else {
                floor_q = trunc_q;
            }
            CL_GC_UNPROTECT(3);
        }
        CL_GC_PROTECT(floor_q);
        result = cl_arith_mul(floor_q, args[1]);
        result = cl_arith_sub(args[0], result);
        CL_GC_UNPROTECT(1);
        return result;
    }
    /* Integer case */
    return cl_arith_mod(args[0], args[1]);
}

static CL_Obj bi_1plus(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    check_number(args[0], "1+");
    return cl_arith_add(args[0], CL_MAKE_FIXNUM(1));
}

static CL_Obj bi_1minus(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    check_number(args[0], "1-");
    return cl_arith_sub(args[0], CL_MAKE_FIXNUM(1));
}

/* --- Comparison --- */

static CL_Obj bi_numeq(CL_Obj *args, int n)
{
    int i;
    check_number(args[0], "=");
    for (i = 1; i < n; i++) {
        check_number(args[i], "=");
        if (!cl_numeric_equal(args[0], args[i]))
            return CL_NIL;
    }
    return SYM_T;
}

static CL_Obj bi_numneq(CL_Obj *args, int n)
{
    int i, j;
    for (i = 0; i < n; i++) {
        check_number(args[i], "/=");
        for (j = i + 1; j < n; j++) {
            check_number(args[j], "/=");
            if (cl_numeric_equal(args[i], args[j]))
                return CL_NIL;
        }
    }
    return SYM_T;
}

/* The ordered comparators (<, <=, >, >=) require real arguments per
   CLHS 12.1.4.1 — complex is not in the domain.  They share one body shape:
   walk adjacent pairs and fail when cl_arith_compare violates the order.
   FAILOP is the *negated* relation (the chain fails when it holds), matching
   the hand-written comparators exactly. */
#define DEFINE_REAL_CHAIN_CMP(fn, who, FAILOP)                       \
    static CL_Obj fn(CL_Obj *args, int n)                            \
    {                                                                \
        int i;                                                       \
        for (i = 0; i < n - 1; i++) {                                \
            check_real(args[i], who);                                \
            check_real(args[i+1], who);                              \
            if (cl_arith_compare(args[i], args[i+1]) FAILOP 0)       \
                return CL_NIL;                                       \
        }                                                            \
        return SYM_T;                                                \
    }

DEFINE_REAL_CHAIN_CMP(bi_lt, "<",  >=)
DEFINE_REAL_CHAIN_CMP(bi_gt, ">",  <=)
DEFINE_REAL_CHAIN_CMP(bi_le, "<=", >)
DEFINE_REAL_CHAIN_CMP(bi_ge, ">=", <)

/* --- Numeric predicates --- */

static CL_Obj bi_zerop(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    check_number(args[0], "ZEROP");
    return cl_arith_zerop(args[0]) ? SYM_T : CL_NIL;
}

static CL_Obj bi_plusp(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    check_number(args[0], "PLUSP");
    return cl_arith_plusp(args[0]) ? SYM_T : CL_NIL;
}

static CL_Obj bi_minusp(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    check_number(args[0], "MINUSP");
    return cl_arith_minusp(args[0]) ? SYM_T : CL_NIL;
}

static CL_Obj bi_evenp(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    check_integer(args[0], "EVENP");
    return cl_arith_evenp(args[0]) ? SYM_T : CL_NIL;
}

static CL_Obj bi_oddp(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    check_integer(args[0], "ODDP");
    return cl_arith_evenp(args[0]) ? CL_NIL : SYM_T;
}

static CL_Obj bi_abs(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    check_number(args[0], "ABS");
    return cl_arith_abs(args[0]);
}

/* (signum n) — per CLHS:
 *   (zerop n) → n
 *   else     → (/ n (abs n))
 * For real n this yields ±1 of the input's exact/float kind; for
 * complex it yields the unit-magnitude direction. */
static CL_Obj bi_signum(CL_Obj *args, int n)
{
    CL_Obj abs_a, result;
    CL_UNUSED(n);
    check_number(args[0], "SIGNUM");
    if (cl_arith_zerop(args[0]))
        return args[0];
    abs_a = cl_arith_abs(args[0]);
    CL_GC_PROTECT(abs_a);
    /* args[0] re-read from the rooted slot — a local copy taken before
     * cl_arith_abs would be stale after its allocation. */
    result = cl_arith_div(args[0], abs_a);
    CL_GC_UNPROTECT(1);
    return result;
}

/* (phase n) — angle of a number in radians.
 * Per CLHS, the result is always a float (never an integer/ratio):
 *   complex   → atan2(imag, real); float kind follows the larger float
 *               component
 *   non-negative real → +0.0 of the same float kind (single-float for
 *               rationals)
 *   negative real → π of the same float kind (likewise single-float for
 *               rationals)
 * (phase 0) → 0.0, (phase 1) → 0.0, (phase 1.0d0) → 0.0d0. */
static CL_Obj bi_phase(CL_Obj *args, int n)
{
    CL_Obj x = args[0];
    CL_UNUSED(n);
    check_number(x, "PHASE");

    if (CL_COMPLEX_P(x)) {
        CL_Complex *cx = (CL_Complex *)CL_OBJ_TO_PTR(x);
        double r = cl_to_double(cx->realpart);
        double i = cl_to_double(cx->imagpart);
        double a = atan2(i, r);
        if (CL_DOUBLE_FLOAT_P(cx->realpart) || CL_DOUBLE_FLOAT_P(cx->imagpart))
            return cl_make_double_float(a);
        return cl_make_single_float((float)a);
    }
    {
        int is_double = CL_DOUBLE_FLOAT_P(x);
        int neg = cl_arith_minusp(x);
        double a = neg ? M_PI : 0.0;
        if (is_double) return cl_make_double_float(a);
        return cl_make_single_float((float)a);
    }
}

/* MAX/MIN track the winning INDEX, not a value copy: cl_arith_compare
 * allocates on the ratio and float-vs-rational paths, which would leave a
 * held CL_Obj copy stale — args[] itself is rooted and forwarded. */
/* MAX/MIN return the extreme argument; they differ only in the diagnostic tag
 * and which direction of cl_arith_compare wins. */
#define DEFINE_MINMAX(fn, who, PICKOP)                          \
    static CL_Obj fn(CL_Obj *args, int n)                       \
    {                                                           \
        int best = 0;                                           \
        int i;                                                  \
        check_number(args[0], who);                             \
        for (i = 1; i < n; i++) {                               \
            check_number(args[i], who);                         \
            if (cl_arith_compare(args[i], args[best]) PICKOP 0) \
                best = i;                                       \
        }                                                       \
        return args[best];                                      \
    }

DEFINE_MINMAX(bi_max, "MAX", >)
DEFINE_MINMAX(bi_min, "MIN", <)

/* --- Extended integer functions --- */

static CL_Obj bi_gcd(CL_Obj *args, int n)
{
    CL_Obj result;
    int i;
    if (n == 0) return CL_MAKE_FIXNUM(0);
    check_integer(args[0], "GCD");
    result = cl_arith_abs(args[0]);
    CL_GC_PROTECT(result);
    for (i = 1; i < n; i++) {
        check_integer(args[i], "GCD");
        result = cl_arith_gcd(result, args[i]);
    }
    CL_GC_UNPROTECT(1);
    return result;
}

static CL_Obj bi_lcm(CL_Obj *args, int n)
{
    CL_Obj result;
    int i;
    if (n == 0) return CL_MAKE_FIXNUM(1);
    check_integer(args[0], "LCM");
    result = cl_arith_abs(args[0]);
    CL_GC_PROTECT(result);
    for (i = 1; i < n; i++) {
        CL_Obj b, g;
        check_integer(args[i], "LCM");
        b = cl_arith_abs(args[i]);
        if (cl_arith_zerop(result) || cl_arith_zerop(b)) {
            result = CL_MAKE_FIXNUM(0);
            break;
        }
        /* b is read again after gcd/truncate allocate — keep it rooted. */
        CL_GC_PROTECT(b);
        g = cl_arith_gcd(result, b);
        g = cl_arith_truncate(result, g);
        result = cl_arith_mul(g, b);
        CL_GC_UNPROTECT(1);
    }
    CL_GC_UNPROTECT(1);
    return result;
}

static CL_Obj bi_expt(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    check_integer(args[0], "EXPT");
    check_integer(args[1], "EXPT");
    return cl_arith_expt(args[0], args[1]);
}

static CL_Obj bi_isqrt(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    check_integer(args[0], "ISQRT");
    return cl_arith_isqrt(args[0]);
}

static CL_Obj bi_integer_length(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    check_integer(args[0], "INTEGER-LENGTH");
    return CL_MAKE_FIXNUM(cl_arith_integer_length(args[0]));
}

static CL_Obj bi_ash(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    check_integer(args[0], "ASH");
    check_integer(args[1], "ASH");
    return cl_arith_ash(args[0], args[1]);
}

/* LOGAND/LOGIOR/LOGXOR fold their integer args with a per-op identity element.
 * result is GC-protected because cl_arith_log* can allocate a bignum. */
#define DEFINE_LOGFOLD(fn, who, IDENT, FOLDFN)                  \
    static CL_Obj fn(CL_Obj *args, int n)                       \
    {                                                           \
        CL_Obj result = CL_MAKE_FIXNUM(IDENT);                  \
        int i;                                                  \
        CL_GC_PROTECT(result);                                  \
        for (i = 0; i < n; i++) {                               \
            check_integer(args[i], who);                        \
            result = FOLDFN(result, args[i]);                   \
        }                                                       \
        CL_GC_UNPROTECT(1);                                     \
        return result;                                          \
    }

DEFINE_LOGFOLD(bi_logand, "LOGAND", -1, cl_arith_logand)
DEFINE_LOGFOLD(bi_logior, "LOGIOR",  0, cl_arith_logior)
DEFINE_LOGFOLD(bi_logxor, "LOGXOR",  0, cl_arith_logxor)

static CL_Obj bi_lognot(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    check_integer(args[0], "LOGNOT");
    return cl_arith_lognot(args[0]);
}

/* --- Bit manipulation --- */

static CL_Obj bi_logcount(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    check_integer(args[0], "LOGCOUNT");
    return CL_MAKE_FIXNUM(cl_arith_logcount(args[0]));
}

static CL_Obj bi_logbitp(CL_Obj *args, int n)
{
    int32_t index;
    CL_UNUSED(n);
    check_integer(args[0], "LOGBITP");
    check_integer(args[1], "LOGBITP");
    /* Index must be unsigned-byte; reject negatives */
    if (cl_arith_minusp(args[0]))
        cl_signal_type_error(args[0], "UNSIGNED-BYTE", "LOGBITP");
    /* Bignum index → outside any reasonable bit width.  For non-negative
     * integers no bit at that position is set (NIL); for negative
     * integers all infinitely-extending sign bits are set (T). */
    if (!CL_FIXNUM_P(args[0]))
        return cl_arith_minusp(args[1]) ? SYM_T : CL_NIL;
    index = CL_FIXNUM_VAL(args[0]);
    return cl_arith_logbitp(index, args[1]) ? SYM_T : CL_NIL;
}

static CL_Obj bi_logtest(CL_Obj *args, int n)
{
    CL_Obj result;
    CL_UNUSED(n);
    check_integer(args[0], "LOGTEST");
    check_integer(args[1], "LOGTEST");
    result = cl_arith_logand(args[0], args[1]);
    return cl_arith_zerop(result) ? CL_NIL : SYM_T;
}

/* byte spec is (size . position) */
static CL_Obj bi_byte(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    check_integer(args[0], "BYTE");
    check_integer(args[1], "BYTE");
    return cl_cons(args[0], args[1]);
}

static CL_Obj bi_byte_size(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    if (!CL_CONS_P(args[0]))
        cl_error(CL_ERR_TYPE, "BYTE-SIZE: not a byte specifier");
    return cl_car(args[0]);
}

static CL_Obj bi_byte_position(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    if (!CL_CONS_P(args[0]))
        cl_error(CL_ERR_TYPE, "BYTE-POSITION: not a byte specifier");
    return cl_cdr(args[0]);
}

/* (ldb bytespec integer) — extract bit field */
static CL_Obj bi_ldb(CL_Obj *args, int n)
{
    CL_Obj size, pos, neg, shifted, mask;
    CL_UNUSED(n);
    if (!CL_CONS_P(args[0]))
        cl_error(CL_ERR_TYPE, "LDB: not a byte specifier");
    check_integer(args[1], "LDB");
    /* Bytespec components are re-read from the rooted args[0] after each
     * allocating step — a captured copy goes stale once anything compacts
     * (only matters for bignum size/pos, but the re-read is free). */
    pos = cl_cdr(args[0]);
    neg = cl_arith_negate(pos);
    /* shifted = ash(integer, -position) */
    shifted = cl_arith_ash(args[1], neg);
    CL_GC_PROTECT(shifted);
    /* mask = (1 << size) - 1 = ash(1, size) - 1 */
    size = cl_car(args[0]);
    mask = cl_arith_ash(CL_MAKE_FIXNUM(1), size);
    mask = cl_arith_sub(mask, CL_MAKE_FIXNUM(1));
    CL_GC_UNPROTECT(1);
    return cl_arith_logand(shifted, mask);
}

/* (dpb newbyte bytespec integer) — deposit bit field */
static CL_Obj bi_dpb(CL_Obj *args, int n)
{
    CL_Obj size, pos, mask, shifted_mask, cleared, new_bits, notm;
    CL_UNUSED(n);
    check_integer(args[0], "DPB");
    if (!CL_CONS_P(args[1]))
        cl_error(CL_ERR_TYPE, "DPB: not a byte specifier");
    check_integer(args[2], "DPB");
    size = cl_car(args[1]);
    /* mask = (1 << size) - 1.  mask is a bignum whenever size >= 31, and
     * it is read again for new_bits below — keep it protected across ALL
     * the intervening allocating calls (the old code unprotected it before
     * lognot/logand and read it stale afterward). */
    mask = cl_arith_ash(CL_MAKE_FIXNUM(1), size);
    mask = cl_arith_sub(mask, CL_MAKE_FIXNUM(1));
    CL_GC_PROTECT(mask);
    /* shifted_mask = ash(mask, position) */
    pos = cl_cdr(args[1]);
    shifted_mask = cl_arith_ash(mask, pos);
    CL_GC_PROTECT(shifted_mask);
    /* Clear the field in integer */
    notm = cl_arith_lognot(shifted_mask);
    cleared = cl_arith_logand(args[2], notm);
    CL_GC_PROTECT(cleared);
    /* Shift new value into position, masked */
    new_bits = cl_arith_logand(args[0], mask);
    pos = cl_cdr(args[1]);
    new_bits = cl_arith_ash(new_bits, pos);
    CL_GC_UNPROTECT(3);
    return cl_arith_logior(cleared, new_bits);
}

/* (ldb-test bytespec integer) — test if field is nonzero */
static CL_Obj bi_ldb_test(CL_Obj *args, int n)
{
    CL_Obj val = bi_ldb(args, n);
    return cl_arith_zerop(val) ? CL_NIL : SYM_T;
}

/* (mask-field bytespec integer) — like ldb but preserves position */
static CL_Obj bi_mask_field(CL_Obj *args, int n)
{
    CL_Obj size, pos, mask, shifted_mask;
    CL_UNUSED(n);
    if (!CL_CONS_P(args[0]))
        cl_error(CL_ERR_TYPE, "MASK-FIELD: not a byte specifier");
    check_integer(args[1], "MASK-FIELD");
    size = cl_car(args[0]);
    mask = cl_arith_ash(CL_MAKE_FIXNUM(1), size);
    mask = cl_arith_sub(mask, CL_MAKE_FIXNUM(1));
    CL_GC_PROTECT(mask);
    pos = cl_cdr(args[0]);  /* re-read after the mask allocs */
    shifted_mask = cl_arith_ash(mask, pos);
    CL_GC_UNPROTECT(1);
    return cl_arith_logand(args[1], shifted_mask);
}

/* (deposit-field newbyte bytespec integer) — like dpb but from same position */
static CL_Obj bi_deposit_field(CL_Obj *args, int n)
{
    CL_Obj size, pos, mask, shifted_mask, cleared, new_bits;
    CL_UNUSED(n);
    check_integer(args[0], "DEPOSIT-FIELD");
    if (!CL_CONS_P(args[1]))
        cl_error(CL_ERR_TYPE, "DEPOSIT-FIELD: not a byte specifier");
    check_integer(args[2], "DEPOSIT-FIELD");
    size = cl_car(args[1]);
    mask = cl_arith_ash(CL_MAKE_FIXNUM(1), size);
    mask = cl_arith_sub(mask, CL_MAKE_FIXNUM(1));
    CL_GC_PROTECT(mask);
    pos = cl_cdr(args[1]);  /* re-read after the mask allocs */
    shifted_mask = cl_arith_ash(mask, pos);
    CL_GC_PROTECT(shifted_mask);
    /* Clear the field in integer.  shifted_mask stays protected — it is
     * read again for new_bits after lognot/logand allocate. */
    {
        CL_Obj notm = cl_arith_lognot(shifted_mask);
        cleared = cl_arith_logand(args[2], notm);
    }
    CL_GC_PROTECT(cleared);
    /* Extract field bits from newbyte at position (already in position) */
    new_bits = cl_arith_logand(args[0], shifted_mask);
    CL_GC_UNPROTECT(3);
    return cl_arith_logior(cleared, new_bits);
}

/* (boole op integer-1 integer-2) — boolean operation */
static CL_Obj bi_boole(CL_Obj *args, int n)
{
    int32_t op;
    CL_Obj a, b;
    CL_UNUSED(n);
    check_integer(args[0], "BOOLE");
    check_integer(args[1], "BOOLE");
    check_integer(args[2], "BOOLE");
    if (!CL_FIXNUM_P(args[0]))
        cl_error(CL_ERR_TYPE, "BOOLE: operation must be a fixnum");
    op = CL_FIXNUM_VAL(args[0]);
    a = args[1];
    b = args[2];
    switch (op) {
    case 0:  return CL_MAKE_FIXNUM(0);                        /* CLR */
    case 1:  return CL_MAKE_FIXNUM(-1);                       /* SET */
    case 2:  return a;                                          /* 1 */
    case 3:  return b;                                          /* 2 */
    case 4:  return cl_arith_lognot(a);                        /* C1 */
    case 5:  return cl_arith_lognot(b);                        /* C2 */
    case 6:  return cl_arith_logand(a, b);                     /* AND */
    case 7:  return cl_arith_logior(a, b);                     /* IOR */
    case 8:  return cl_arith_logxor(a, b);                     /* XOR */
    case 9:  return cl_arith_lognot(cl_arith_logxor(a, b));   /* EQV */
    case 10: return cl_arith_lognot(cl_arith_logand(a, b));   /* NAND */
    case 11: return cl_arith_lognot(cl_arith_logior(a, b));   /* NOR */
    /* ANDC1/ANDC2/ORC1/ORC2: lognot allocates, so the OTHER operand is
     * re-read from the rooted args[] after it — the a/b copies captured
     * above would be stale. */
    case 12: a = cl_arith_lognot(a);  return cl_arith_logand(a, args[2]); /* ANDC1 */
    case 13: b = cl_arith_lognot(b);  return cl_arith_logand(args[1], b); /* ANDC2 */
    case 14: a = cl_arith_lognot(a);  return cl_arith_logior(a, args[2]); /* ORC1 */
    case 15: b = cl_arith_lognot(b);  return cl_arith_logior(args[1], b); /* ORC2 */
    default:
        cl_error(CL_ERR_ARGS, "BOOLE: invalid operation %d", op);
        return CL_NIL;
    }
}

DEFINE_TYPE_PREDICATE(bi_numberp,   CL_NUMBER_P)
DEFINE_TYPE_PREDICATE(bi_integerp,  CL_INTEGER_P)
DEFINE_TYPE_PREDICATE(bi_floatp,    CL_FLOATP)
DEFINE_TYPE_PREDICATE(bi_realp,     CL_REALP)
DEFINE_TYPE_PREDICATE(bi_rationalp, CL_RATIONAL_P)

/* --- Complex number functions --- */

static CL_Obj bi_complex(CL_Obj *args, int n)
{
    CL_Obj real = args[0], imag;
    int real_is_float, imag_is_float, want_double;

    if (!CL_REALP(real))
        cl_signal_type_error(real, "REAL", "COMPLEX");
    imag = (n >= 2) ? args[1] : CL_MAKE_FIXNUM(0);
    if (!CL_REALP(imag))
        cl_signal_type_error(imag, "REAL", "COMPLEX");

    real_is_float = CL_FLOATP(real);
    imag_is_float = CL_FLOATP(imag);

    /* CLHS COMPLEX:
     *   - both rational, imag = integer 0 → return real (canonical)
     *   - both rational, imag != 0       → (complex rational) — keep as is
     *   - either is a float              → both coerced to common float
     *     type via the rule of float contagion (12.1.5.3.1).
     * (complex 5.0) → #C(5.0 0.0); (complex 5) → 5; (complex 5.0 0) →
     * #C(5.0 0.0). */
    if (!real_is_float && !imag_is_float) {
        if (CL_FIXNUM_P(imag) && CL_FIXNUM_VAL(imag) == 0)
            return real;
        return cl_make_complex(real, imag);
    }

    want_double = CL_DOUBLE_FLOAT_P(real) || CL_DOUBLE_FLOAT_P(imag);
    CL_GC_PROTECT(real);
    CL_GC_PROTECT(imag);
    if (want_double) {
        if (!CL_DOUBLE_FLOAT_P(real))
            real = cl_make_double_float(cl_to_double(real));
        if (!CL_DOUBLE_FLOAT_P(imag))
            imag = cl_make_double_float(cl_to_double(imag));
    } else {
        if (!CL_SINGLE_FLOAT_P(real))
            real = cl_make_single_float((float)cl_to_double(real));
        if (!CL_SINGLE_FLOAT_P(imag))
            imag = cl_make_single_float((float)cl_to_double(imag));
    }
    CL_GC_UNPROTECT(2);
    return cl_make_complex(real, imag);
}

DEFINE_TYPE_PREDICATE(bi_complexp, CL_COMPLEX_P)

static CL_Obj bi_realpart(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    if (CL_COMPLEX_P(args[0]))
        return ((CL_Complex *)CL_OBJ_TO_PTR(args[0]))->realpart;
    if (CL_REALP(args[0]))
        return args[0];
    cl_signal_type_error(args[0], "NUMBER", "REALPART");
    return CL_NIL;
}

static CL_Obj bi_imagpart(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    if (CL_COMPLEX_P(args[0]))
        return ((CL_Complex *)CL_OBJ_TO_PTR(args[0]))->imagpart;
    if (CL_REALP(args[0])) {
        /* (imagpart x) where x is real returns a zero of the same float
         * kind as x (per CLHS 12.2 — IMAGPART of a rational is 0,
         * imagpart of a float is a float zero of matching precision). */
        if (CL_DOUBLE_FLOAT_P(args[0])) return cl_make_double_float(0.0);
        if (CL_SINGLE_FLOAT_P(args[0])) return cl_make_single_float(0.0f);
        return CL_MAKE_FIXNUM(0);
    }
    cl_signal_type_error(args[0], "NUMBER", "IMAGPART");
    return CL_NIL;
}

static CL_Obj bi_conjugate(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    if (CL_COMPLEX_P(args[0])) {
        CL_Complex *cx = (CL_Complex *)CL_OBJ_TO_PTR(args[0]);
        /* Negate imaginary part */
        CL_Obj neg_imag = cl_arith_negate(cx->imagpart);
        return cl_make_complex(cx->realpart, neg_imag);
    }
    if (CL_REALP(args[0]))
        return args[0];
    cl_error(CL_ERR_TYPE, "CONJUGATE: not a number: %s", cl_type_name(args[0]));
    return CL_NIL;
}

/* --- Ratio accessors --- */

static CL_Obj bi_numerator(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    if (!CL_RATIONAL_P(args[0]))
        cl_error(CL_ERR_TYPE, "NUMERATOR: not a rational: %s", cl_type_name(args[0]));
    return cl_numerator(args[0]);
}

static CL_Obj bi_denominator(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    if (!CL_RATIONAL_P(args[0]))
        cl_error(CL_ERR_TYPE, "DENOMINATOR: not a rational: %s", cl_type_name(args[0]));
    return cl_denominator(args[0]);
}

/* (rational x) — exact rational equal in value to x.
   Per HyperSpec 12.2: when x is a float, the result is an exact
   rational representing the float's IEEE bit pattern (NOT a "simple"
   continued-fraction approximation — that's RATIONALIZE's job). */
static CL_Obj bi_rational(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    if (CL_RATIONAL_P(args[0]))
        return args[0];
    if (CL_FLOATP(args[0]))
        return cl_float_to_exact_rational(args[0]);
    cl_signal_type_error(args[0], "REAL", "RATIONAL");
    return CL_NIL;
}

/* (rationalize x) — same as rational for now */
static CL_Obj bi_rationalize(CL_Obj *args, int n)
{
    return bi_rational(args, n);
}

/* --- Float-specific functions (Step 9) --- */

static void check_float(CL_Obj obj, const char *op)
{
    if (!CL_FLOATP(obj))
        cl_error(CL_ERR_TYPE, "%s: not a float", op);
}

/* (float number &optional prototype) */
static CL_Obj bi_float(CL_Obj *args, int n)
{
    CL_Obj num = args[0];
    check_number(num, "FLOAT");
    if (n >= 2) {
        /* Prototype determines result type */
        check_float(args[1], "FLOAT");
        if (CL_DOUBLE_FLOAT_P(args[1]))
            return cl_make_double_float(cl_to_double(num));
        return cl_make_single_float(cl_to_float(num));
    }
    /* No prototype: floats unchanged, integers → single-float */
    if (CL_FLOATP(num))
        return num;
    return cl_make_single_float(cl_to_float(num));
}

/* (float-digits float) → fixnum */
static CL_Obj bi_float_digits(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    check_float(args[0], "FLOAT-DIGITS");
    if (CL_DOUBLE_FLOAT_P(args[0]))
        return CL_MAKE_FIXNUM(53);
    return CL_MAKE_FIXNUM(24);
}

/* (float-precision float) → fixnum
 * Returns the number of significant radix (base-2) digits in the float.
 * For normalized IEEE 754 floats this equals float-digits. */
static CL_Obj bi_float_precision(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    check_float(args[0], "FLOAT-PRECISION");
    if (CL_DOUBLE_FLOAT_P(args[0]))
        return CL_MAKE_FIXNUM(53);
    return CL_MAKE_FIXNUM(24);
}

/* (float-radix float) → fixnum (always 2) */
static CL_Obj bi_float_radix(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    check_float(args[0], "FLOAT-RADIX");
    return CL_MAKE_FIXNUM(2);
}

/* (float-sign float-1 &optional float-2) */
static CL_Obj bi_float_sign(CL_Obj *args, int n)
{
    double sign_val, mag;
    int is_double;
    check_float(args[0], "FLOAT-SIGN");
    sign_val = cl_to_double(args[0]);
    if (n >= 2) {
        check_float(args[1], "FLOAT-SIGN");
        mag = cl_to_double(args[1]);
        if (mag < 0.0) mag = -mag;
        is_double = CL_DOUBLE_FLOAT_P(args[1]);
    } else {
        mag = 1.0;
        is_double = CL_DOUBLE_FLOAT_P(args[0]);
    }
    if (sign_val < 0.0) mag = -mag;
    /* Handle negative zero */
    else if (sign_val == 0.0 && (1.0 / sign_val) < 0.0) mag = -mag;
    if (is_double)
        return cl_make_double_float(mag);
    return cl_make_single_float((float)mag);
}

/* (decode-float float) → significand, exponent, sign  (3 values) */
static CL_Obj bi_decode_float(CL_Obj *args, int n)
{
    double val, sig;
    int exp;
    CL_Obj sig_obj, exp_obj, sign_obj;
    int is_double;
    CL_UNUSED(n);
    check_float(args[0], "DECODE-FLOAT");
    is_double = CL_DOUBLE_FLOAT_P(args[0]);
    val = cl_to_double(args[0]);
    if (val == 0.0) {
        sig_obj = is_double ? cl_make_double_float(0.0) : cl_make_single_float(0.0f);
        exp_obj = CL_MAKE_FIXNUM(0);
        /* Sign preserves -0.0 */
        if ((1.0 / val) < 0.0)
            sign_obj = is_double ? cl_make_double_float(-1.0) : cl_make_single_float(-1.0f);
        else
            sign_obj = is_double ? cl_make_double_float(1.0) : cl_make_single_float(1.0f);
    } else {
        double abs_val = val < 0.0 ? -val : val;
        sig = frexp(abs_val, &exp);
        /* frexp returns [0.5, 1.0) which matches CL spec for decode-float */
        sig_obj = is_double ? cl_make_double_float(sig) : cl_make_single_float((float)sig);
        exp_obj = CL_MAKE_FIXNUM(exp);
        sign_obj = is_double
            ? cl_make_double_float(val < 0.0 ? -1.0 : 1.0)
            : cl_make_single_float(val < 0.0 ? -1.0f : 1.0f);
    }
    cl_mv_count = 3;
    cl_mv_values[0] = sig_obj;
    cl_mv_values[1] = exp_obj;
    cl_mv_values[2] = sign_obj;
    return sig_obj;
}

/* Helper: convert a positive double integer value to CL_Obj (fixnum or bignum).
   Value must be a non-negative whole number ≤ 2^53. */
static CL_Obj double_int_to_obj(double val)
{
    if (val <= (double)CL_FIXNUM_MAX)
        return CL_MAKE_FIXNUM((int32_t)val);
    /* Split into hi*65536 + lo using 16-bit chunks (like bignum limbs) */
    {
        uint32_t lo, mid, hi;
        CL_Obj result, term;
        lo = (uint32_t)fmod(val, 65536.0);
        val = (val - lo) / 65536.0;
        mid = (uint32_t)fmod(val, 65536.0);
        val = (val - mid) / 65536.0;
        hi = (uint32_t)val;
        /* result = hi * 65536^2 + mid * 65536 + lo */
        result = CL_MAKE_FIXNUM((int32_t)lo);
        CL_GC_PROTECT(result);
        term = cl_arith_mul(CL_MAKE_FIXNUM((int32_t)mid), CL_MAKE_FIXNUM(65536));
        result = cl_arith_add(result, term);
        if (hi > 0) {
            /* hi * 65536 * 65536 = hi * (65536^2) */
            term = cl_arith_mul(CL_MAKE_FIXNUM((int32_t)hi), CL_MAKE_FIXNUM(65536));
            term = cl_arith_mul(term, CL_MAKE_FIXNUM(65536));
            result = cl_arith_add(result, term);
        }
        CL_GC_UNPROTECT(1);
        return result;
    }
}

/* (integer-decode-float float) → significand(int), exponent(int), sign(int) */
static CL_Obj bi_integer_decode_float(CL_Obj *args, int n)
{
    double val;
    int is_double;
    CL_Obj sig_obj, exp_obj, sign_obj;
    CL_UNUSED(n);
    check_float(args[0], "INTEGER-DECODE-FLOAT");
    is_double = CL_DOUBLE_FLOAT_P(args[0]);
    val = cl_to_double(args[0]);
    if (val == 0.0) {
        sig_obj = CL_MAKE_FIXNUM(0);
        exp_obj = CL_MAKE_FIXNUM(0);
        sign_obj = CL_MAKE_FIXNUM((1.0 / val) < 0.0 ? -1 : 1);
    } else {
        double abs_val = val < 0.0 ? -val : val;
        int exp;
        double sig = frexp(abs_val, &exp);
        if (is_double) {
            /* Scale significand to integer: sig is in [0.5, 1.0), multiply by 2^53 */
            double int_sig = ldexp(sig, 53);
            int32_t adj_exp = exp - 53;
            sig_obj = double_int_to_obj(int_sig);
            exp_obj = CL_MAKE_FIXNUM(adj_exp);
        } else {
            /* Scale significand to integer: sig is in [0.5, 1.0), multiply by 2^24 */
            double int_sig = ldexp(sig, 24);
            int32_t adj_exp = exp - 24;
            int32_t isig = (int32_t)int_sig;
            sig_obj = CL_MAKE_FIXNUM(isig);
            exp_obj = CL_MAKE_FIXNUM(adj_exp);
        }
        sign_obj = CL_MAKE_FIXNUM(val < 0.0 ? -1 : 1);
    }
    cl_mv_count = 3;
    cl_mv_values[0] = sig_obj;
    cl_mv_values[1] = exp_obj;
    cl_mv_values[2] = sign_obj;
    return sig_obj;
}

/* (scale-float float integer) */
static CL_Obj bi_scale_float(CL_Obj *args, int n)
{
    double val, result;
    int32_t scale;
    CL_UNUSED(n);
    check_float(args[0], "SCALE-FLOAT");
    check_integer(args[1], "SCALE-FLOAT");
    val = cl_to_double(args[0]);
    scale = CL_FIXNUM_P(args[1]) ? CL_FIXNUM_VAL(args[1])
        : (int32_t)cl_to_double(args[1]);
    result = ldexp(val, scale);
    if (CL_DOUBLE_FLOAT_P(args[0]))
        return cl_make_double_float(result);
    return cl_make_single_float((float)result);
}

/* ---- IEEE-754 raw bit access (float-features :cl-amiga backend) ----
 *
 * float-features' SINGLE/DOUBLE-FLOAT-BITS accessors (and their inverses)
 * have no portable branch — on any unrecognised Lisp they signal
 * "Implementation not supported."  jzon relies on DOUBLE-FLOAT-BITS to
 * serialise floats to JSON, so cl-amiga would silently fail to emit any
 * float (breaking SSE item-change messages, etc.).  We expose the raw
 * IEEE-754 bit reinterpretation as CLAMIGA-package builtins; float-features'
 * #+cl-amiga branch calls these.  Registered in cl_builtins_arith_init. */

/* uint64 -> CL integer (fixnum if it fits, else 4-limb bignum). */
static CL_Obj float_bits_u64_to_obj(uint64_t v)
{
    CL_Obj bn;
    CL_Bignum *b;
    if (v <= (uint64_t)CL_FIXNUM_MAX)
        return CL_MAKE_FIXNUM((int32_t)v);
    bn = cl_make_bignum(4, 0);
    b = (CL_Bignum *)CL_OBJ_TO_PTR(bn);
    b->limbs[0] = (uint16_t)(v & 0xFFFF);
    b->limbs[1] = (uint16_t)((v >> 16) & 0xFFFF);
    b->limbs[2] = (uint16_t)((v >> 32) & 0xFFFF);
    b->limbs[3] = (uint16_t)((v >> 48) & 0xFFFF);
    return cl_bignum_normalize(bn);
}

/* CL integer -> low 64 bits (two's complement). */
static uint64_t float_bits_obj_to_u64(CL_Obj o, const char *op)
{
    if (CL_FIXNUM_P(o))
        return (uint64_t)(int64_t)CL_FIXNUM_VAL(o);
    if (CL_BIGNUM_P(o)) {
        CL_Bignum *b = (CL_Bignum *)CL_OBJ_TO_PTR(o);
        uint64_t v = 0;
        uint32_t i;
        for (i = 0; i < b->length && i < 4; i++)
            v |= ((uint64_t)b->limbs[i]) << (16 * i);
        if (b->sign)
            v = (uint64_t)(-(int64_t)v);
        return v;
    }
    cl_error(CL_ERR_TYPE, "%s: not an integer", op);
    return 0;
}

/* (single-float-bits float) -> (unsigned-byte 32) */
static CL_Obj bi_single_float_bits(CL_Obj *args, int n)
{
    float f;
    uint32_t bits;
    CL_UNUSED(n);
    check_float(args[0], "SINGLE-FLOAT-BITS");
    f = (float)cl_to_double(args[0]);
    memcpy(&bits, &f, sizeof bits);
    return float_bits_u64_to_obj((uint64_t)bits);
}

/* (double-float-bits float) -> (unsigned-byte 64) */
static CL_Obj bi_double_float_bits(CL_Obj *args, int n)
{
    double d;
    uint64_t bits;
    CL_UNUSED(n);
    check_float(args[0], "DOUBLE-FLOAT-BITS");
    d = cl_to_double(args[0]);
    memcpy(&bits, &d, sizeof bits);
    return float_bits_u64_to_obj(bits);
}

/* (bits-single-float (unsigned-byte 32)) -> single-float */
static CL_Obj bi_bits_single_float(CL_Obj *args, int n)
{
    uint32_t bits;
    float f;
    CL_UNUSED(n);
    bits = (uint32_t)float_bits_obj_to_u64(args[0], "BITS-SINGLE-FLOAT");
    memcpy(&f, &bits, sizeof f);
    return cl_make_single_float(f);
}

/* (bits-double-float (unsigned-byte 64)) -> double-float */
static CL_Obj bi_bits_double_float(CL_Obj *args, int n)
{
    uint64_t bits;
    double d;
    CL_UNUSED(n);
    bits = float_bits_obj_to_u64(args[0], "BITS-DOUBLE-FLOAT");
    memcpy(&d, &bits, sizeof d);
    return cl_make_double_float(d);
}

/* --- Registration ---
 *
 * Table-driven: hundreds of defun() call sites used to compile to
 * ~30 bytes of m68k code each (push 4 args + call + restore SP).
 * A descriptor entry is 12 bytes (name ptr + fn ptr + 2 * int16),
 * walked once by cl_register_builtins(). ~half the size, no extra
 * relocations beyond the function pointer the linker already needed. */
static const CL_BuiltinDesc arith_builtins[] = {
    /* Arithmetic */
    {"+",                    bi_add,                   0, -1},
    {"-",                    bi_sub,                   1, -1},
    {"*",                    bi_mul,                   0, -1},
    {"/",                    bi_div,                   1, -1},
    {"FLOOR",                bi_floor,                 1,  2},
    {"CEILING",              bi_ceiling,               1,  2},
    {"TRUNCATE",             bi_truncate,              1,  2},
    {"ROUND",                bi_round,                 1,  2},
    {"FFLOOR",               bi_ffloor,                1,  2},
    {"FCEILING",             bi_fceiling,              1,  2},
    {"FTRUNCATE",            bi_ftruncate,             1,  2},
    {"FROUND",               bi_fround,                1,  2},
    {"REM",                  bi_rem,                   2,  2},
    {"MOD",                  bi_mod,                   2,  2},
    {"1+",                   bi_1plus,                 1,  1},
    {"1-",                   bi_1minus,                1,  1},

    /* Comparison */
    {"=",                    bi_numeq,                 1, -1},
    {"/=",                   bi_numneq,                1, -1},
    {"<",                    bi_lt,                    1, -1},
    {">",                    bi_gt,                    1, -1},
    {"<=",                   bi_le,                    1, -1},
    {">=",                   bi_ge,                    1, -1},

    /* Numeric predicates */
    {"ZEROP",                bi_zerop,                 1,  1},
    {"PLUSP",                bi_plusp,                 1,  1},
    {"MINUSP",               bi_minusp,                1,  1},
    {"EVENP",                bi_evenp,                 1,  1},
    {"ODDP",                 bi_oddp,                  1,  1},
    {"ABS",                  bi_abs,                   1,  1},
    {"SIGNUM",               bi_signum,                1,  1},
    {"PHASE",                bi_phase,                 1,  1},
    {"MAX",                  bi_max,                   1, -1},
    {"MIN",                  bi_min,                   1, -1},
    {"NUMBERP",              bi_numberp,               1,  1},
    {"INTEGERP",             bi_integerp,              1,  1},
    {"FLOATP",               bi_floatp,                1,  1},
    {"REALP",                bi_realp,                 1,  1},
    {"RATIONALP",            bi_rationalp,             1,  1},

    /* Extended integer functions */
    {"GCD",                  bi_gcd,                   0, -1},
    {"LCM",                  bi_lcm,                   0, -1},
    {"EXPT",                 bi_expt,                  2,  2},
    {"ISQRT",                bi_isqrt,                 1,  1},
    {"INTEGER-LENGTH",       bi_integer_length,        1,  1},
    {"ASH",                  bi_ash,                   2,  2},
    {"LOGAND",               bi_logand,                0, -1},
    {"LOGIOR",               bi_logior,                0, -1},
    {"LOGXOR",               bi_logxor,                0, -1},
    {"LOGNOT",               bi_lognot,                1,  1},
    {"LOGCOUNT",             bi_logcount,              1,  1},
    {"LOGBITP",              bi_logbitp,               2,  2},
    {"LOGTEST",              bi_logtest,               2,  2},

    /* Byte operations */
    {"BYTE",                 bi_byte,                  2,  2},
    {"BYTE-SIZE",            bi_byte_size,             1,  1},
    {"BYTE-POSITION",        bi_byte_position,         1,  1},
    {"LDB",                  bi_ldb,                   2,  2},
    {"DPB",                  bi_dpb,                   3,  3},
    {"LDB-TEST",             bi_ldb_test,              2,  2},
    {"MASK-FIELD",           bi_mask_field,            2,  2},
    {"DEPOSIT-FIELD",        bi_deposit_field,         3,  3},
    {"BOOLE",                bi_boole,                 3,  3},

    /* Complex number functions */
    {"COMPLEX",              bi_complex,               1,  2},
    {"COMPLEXP",             bi_complexp,              1,  1},
    {"REALPART",             bi_realpart,              1,  1},
    {"IMAGPART",             bi_imagpart,              1,  1},
    {"CONJUGATE",            bi_conjugate,             1,  1},

    /* Ratio accessors */
    {"NUMERATOR",            bi_numerator,             1,  1},
    {"DENOMINATOR",          bi_denominator,           1,  1},
    {"RATIONAL",             bi_rational,              1,  1},
    {"RATIONALIZE",          bi_rationalize,           1,  1},

    /* Float-specific functions */
    {"FLOAT",                bi_float,                 1,  2},
    {"FLOAT-DIGITS",         bi_float_digits,          1,  1},
    {"FLOAT-PRECISION",      bi_float_precision,       1,  1},
    {"FLOAT-RADIX",          bi_float_radix,           1,  1},
    {"FLOAT-SIGN",           bi_float_sign,            1,  2},
    {"DECODE-FLOAT",         bi_decode_float,          1,  1},
    {"INTEGER-DECODE-FLOAT", bi_integer_decode_float,  1,  1},
    {"SCALE-FLOAT",          bi_scale_float,           2,  2},
};

void cl_builtins_arith_init(void)
{
    cl_register_builtins(arith_builtins,
                         sizeof(arith_builtins) / sizeof(arith_builtins[0]),
                         cl_package_cl);

    /* IEEE-754 raw bit access for the float-features :cl-amiga backend
     * (jzon float serialisation). Exposed in the AMILISP package. */
    cl_register_builtin("SINGLE-FLOAT-BITS", bi_single_float_bits, 1, 1, cl_package_amilisp);
    cl_register_builtin("DOUBLE-FLOAT-BITS", bi_double_float_bits, 1, 1, cl_package_amilisp);
    cl_register_builtin("BITS-SINGLE-FLOAT", bi_bits_single_float, 1, 1, cl_package_amilisp);
    cl_register_builtin("BITS-DOUBLE-FLOAT", bi_bits_double_float, 1, 1, cl_package_amilisp);

    /* Initialize bignum subsystem */
    cl_bignum_init();
}
