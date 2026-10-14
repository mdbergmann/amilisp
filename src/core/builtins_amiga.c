/*
 * builtins_amiga.c — AMIGA package builtins.
 *
 * AmigaOS-specific: shared library open/close, register-based library
 * calls, chip memory allocation.  Entire file is compiled on all platforms
 * but the init function is a no-op on POSIX (AMIGA package doesn't exist).
 */

#include "builtins.h"
#include "types.h"
#include "mem.h"
#include "error.h"
#include "symbol.h"
#include "package.h"
#include "compiler.h"
#include "string_utils.h"
#include "printer.h"
#include "thread.h"
#include <stddef.h>   /* offsetof (the string-key hook) */
#include "../platform/platform.h"
#include <string.h>
#include <stdio.h>

/* Symbol AMIGA::%FFI-CALL — sentinel that the compiler matches to emit
 * OP_AMIGA_CALL.  Interned on every platform so binding modules compile
 * identically everywhere (the host VM's dispatch stub then signals a clear
 * "not available on this platform" error if such a call is ever executed).
 * Declared in compiler.h so compile_call can reference it without a hard
 * dep on Amiga-only headers. */
CL_Obj cl_amiga_ffi_call_sym = CL_NIL;

/* Box a raw 32-bit d0 result per CL_AMIGA_RES_* (builtins.h).  Shared by
 * the VM/JIT dispatch and the CALL-LIBRARY* builtins; platform-neutral so
 * tests/test_amiga_ffi.c can pin the encoding on the host.
 *
 * Pointers: NULL becomes NIL so callers can write (or (open-x) (error ..))
 * instead of testing FFI:NULL-POINTER-P; the foreign pointer is unsized
 * and unowned (the OS owns the object).  Signed: a LONG result of -1 used
 * to come back as 4294967295 — the d0 pattern is sign-extended here
 * instead.  Anything that does not fit a fixnum is a 2-limb bignum. */
CL_Obj cl_amiga_box_result(uint32_t result, int kind)
{
    uint32_t mag;
    uint32_t sign = 0;
    CL_Obj bn;
    CL_Bignum *b;

    switch (kind) {
    case CL_AMIGA_RES_VOID:
        return CL_NIL;
    case CL_AMIGA_RES_POINTER:
        if (result == 0)
            return CL_NIL;
        return cl_make_foreign_pointer(result, 0, 0);
    /* Sub-32-bit C return types: only d0.w / d0.b are defined for a
     * BOOL/WORD/UWORD/BYTE/UBYTE result — the upper bits are whatever the
     * library left there, so mask before interpreting.  (These used to be
     * Lisp post-processors around an :unsigned call; boxing them here
     * lets DEFCFUN describe every binding as one descriptor.) */
    case CL_AMIGA_RES_BOOL:
        return (result & 0xFFFFu) ? CL_T : CL_NIL;
    case CL_AMIGA_RES_U16:
        return CL_MAKE_FIXNUM((int32_t)(result & 0xFFFFu));
    case CL_AMIGA_RES_I16:
        return CL_MAKE_FIXNUM((int32_t)(int16_t)(result & 0xFFFFu));
    case CL_AMIGA_RES_U8:
        return CL_MAKE_FIXNUM((int32_t)(result & 0xFFu));
    case CL_AMIGA_RES_I8:
        return CL_MAKE_FIXNUM((int32_t)(int8_t)(result & 0xFFu));
    case CL_AMIGA_RES_SIGNED: {
        int32_t sv = (int32_t)result;
        if (sv >= CL_FIXNUM_MIN && sv <= CL_FIXNUM_MAX)
            return CL_MAKE_FIXNUM(sv);
        if (sv < 0) {
            sign = 1;
            mag = (uint32_t)(-(int64_t)sv);
        } else {
            mag = (uint32_t)sv;
        }
        break;
    }
    default: /* CL_AMIGA_RES_UNSIGNED */
        if (result <= (uint32_t)CL_FIXNUM_MAX)
            return CL_MAKE_FIXNUM((int32_t)result);
        mag = result;
        break;
    }
    /* |value| >= 2^30 here, so the high limb is never zero — the bignum
     * is already normalized. */
    bn = cl_make_bignum(2, sign);
    b = (CL_Bignum *)CL_OBJ_TO_PTR(bn);
    b->limbs[0] = (uint16_t)(mag & 0xFFFF);
    b->limbs[1] = (uint16_t)(mag >> 16);
    return bn;
}

/* Register names for diagnostics, indexed like decode_register_keyword and
 * the regspec nibbles (D0..D7 = 0..7, A0..A5 = 8..13). */
static const char *const amiga_reg_names[14] = {
    "D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7",
    "A0", "A1", "A2", "A3", "A4", "A5"
};

/* The 32-bit register image of the Lisp value VAL, given as register
 * argument number ARG_INDEX (1-based, for the message) bound for register
 * REG_IDX: an integer as its two's-complement longword (a fixnum, or the
 * low 32 bits of a bignum -- negative LONGs wrap), a foreign pointer as its
 * address, NIL as 0 (NULL / FALSE) and T as 1 (TRUE).  That is the coercion
 * AMIGA.BOOPSI applies to tag values, so a BOOL or LONG flag parameter such
 * as RethinkLayout's `refresh` takes a Lisp boolean at the call site.
 * Anything else is a TYPE-ERROR that names the argument, its register and
 * the offending value.
 *
 * Shared by CALL-LIBRARY, CALL-LIBRARY-FAST and the OP_AMIGA_CALL dispatch
 * (VM, JIT and stub callers alike).  Platform-neutral so the host build
 * checks a binding's arguments before its "only on AmigaOS" error and
 * tests/test_amiga_ffi.c pins the rules. */
uint32_t cl_amiga_ffi_arg_to_u32(CL_Obj val, int arg_index, int reg_idx)
{
    char shown[80];

    if (CL_FIXNUM_P(val))
        return (uint32_t)CL_FIXNUM_VAL(val);
    if (CL_BIGNUM_P(val)) {
        CL_Bignum *bn = (CL_Bignum *)CL_OBJ_TO_PTR(val);
        uint32_t mag = (uint32_t)bn->limbs[0];
        if (bn->length > 1)
            mag |= ((uint32_t)bn->limbs[1]) << 16;
        return bn->sign ? (uint32_t)0u - mag : mag;
    }
    if (CL_FOREIGN_POINTER_P(val)) {
        CL_ForeignPtr *fp = (CL_ForeignPtr *)CL_OBJ_TO_PTR(val);
        return fp->address;
    }
    if (CL_NULL_P(val))
        return 0;
    if (val == CL_T)
        return 1;
    cl_prin1_to_string(val, shown, (int)sizeof(shown));
    cl_error(CL_ERR_TYPE,
             "AMIGA FFI: register argument %d (:%s) must be an integer, a "
             "foreign pointer, T or NIL -- got %s",
             arg_index,
             (reg_idx >= 0 && reg_idx < 14) ? amiga_reg_names[reg_idx] : "?",
             shown);
    return 0;
}

#ifdef PLATFORM_AMIGA

/* ================================================================
 * Helper: register function in AMIGA package and export
 * ================================================================ */

/* Routed through cl_register_builtin_exported so the image-relink registry
 * sees the registration (builtins.h). */
static void amiga_defun(const char *name, CL_CFunc func, int min, int max)
{
    cl_register_builtin_exported(name, func, min, max, cl_package_amiga);
}

/* Pre-interned register keyword symbols */
static CL_Obj kw_d0, kw_d1, kw_d2, kw_d3, kw_d4, kw_d5, kw_d6, kw_d7;
static CL_Obj kw_a0, kw_a1, kw_a2, kw_a3, kw_a4, kw_a5;

/* Map a keyword symbol to a register index (0-13), or -1 if unknown */
static int decode_register_keyword(CL_Obj kw)
{
    if (kw == kw_d0) return 0;
    if (kw == kw_d1) return 1;
    if (kw == kw_d2) return 2;
    if (kw == kw_d3) return 3;
    if (kw == kw_d4) return 4;
    if (kw == kw_d5) return 5;
    if (kw == kw_d6) return 6;
    if (kw == kw_d7) return 7;
    if (kw == kw_a0) return 8;
    if (kw == kw_a1) return 9;
    if (kw == kw_a2) return 10;
    if (kw == kw_a3) return 11;
    if (kw == kw_a4) return 12;
    /* :A5 (index 13) is deliberately REJECTED: ffi_dispatch_m68k.s only
     * loads d0-d7/a0-a4 (a5 is its scratch/frame register — see the
     * comment at the top of the dispatcher).  Accepting it here silently
     * dropped the argument. */
    if (kw == kw_a5)
        cl_error(CL_ERR_ARGS,
                 "AMIGA FFI: register :A5 is reserved by the call "
                 "dispatcher and cannot carry an argument (use d0-d7/a0-a4)");
    return -1;
}

/* ================================================================
 * Library management
 * ================================================================ */

/* (amiga:open-library name &optional version) → foreign-pointer */
static CL_Obj bi_amiga_open_library(CL_Obj *args, int nargs)
{
    CL_String *s;
    char namebuf[128];
    uint32_t version, base;

    if (!CL_STRING_P(args[0]))
        cl_error(CL_ERR_TYPE, "AMIGA:OPEN-LIBRARY: name must be a string");
    s = (CL_String *)CL_OBJ_TO_PTR(args[0]);
    if (s->length >= sizeof(namebuf))
        cl_error(CL_ERR_ARGS, "AMIGA:OPEN-LIBRARY: name too long");
    memcpy(namebuf, s->data, s->length);
    namebuf[s->length] = '\0';
    version = (nargs > 1 && !CL_NULL_P(args[1]))
              ? (uint32_t)CL_FIXNUM_VAL(args[1]) : 0;
    base = platform_amiga_open_library(namebuf, version);
    if (base == 0)
        return CL_NIL;
    return cl_make_foreign_pointer(base, 0, 0);
}

/* (amiga:close-library lib) → T */
static CL_Obj bi_amiga_close_library(CL_Obj *args, int nargs)
{
    CL_ForeignPtr *fp;
    (void)nargs;
    if (!CL_FOREIGN_POINTER_P(args[0]))
        cl_error(CL_ERR_TYPE, "AMIGA:CLOSE-LIBRARY: argument must be a foreign pointer");
    fp = (CL_ForeignPtr *)CL_OBJ_TO_PTR(args[0]);
    platform_amiga_close_library(fp->address);
    fp->address = 0;
    return CL_T;
}

/* Signal a clear error when the library base FP holds is 0: a call through
 * base 0 is a jump to address -LVO, which crashes the machine instead of
 * signalling.  Shared by CALL-LIBRARY and CALL-LIBRARY-FAST (WHO names the
 * caller in the message); the base-symbol paths make the same check in
 * cl_amiga_library_base_address. */
static void library_base_check(CL_ForeignPtr *fp, const char *who)
{
    if (fp->address == 0)
        cl_error(CL_ERR_GENERAL,
                 "%s: the library base is a NULL pointer - "
                 "the library is not open in this process (a heap image "
                 "zeroes every foreign pointer on restore: keep bases in "
                 "AMIGA.FFI:DEFINE-LIBRARY-VARIABLE variables)", who);
}

/* (amiga:call-library base offset reg-spec &optional result-kind) → object
 *
 * reg-spec is a plist: (:D0 val :A0 ptr :D1 42 ...)
 * Each keyword names a 68k register, each value is the argument: an
 * integer, a foreign pointer, NIL (0) or T (1) -- cl_amiga_ffi_arg_to_u32.
 * RESULT-KIND is one of the CL_AMIGA_RES_* fixnums (default 0 = d0 as an
 * unsigned integer) — the plist path has no regspec to carry it, and the
 * generated bindings use this entry for the handful of OS functions with
 * more than seven register arguments. */
static CL_Obj bi_amiga_call_library(CL_Obj *args, int nargs)
{
    CL_ForeignPtr *fp;
    int16_t offset;
    uint32_t regs[14];
    uint16_t reg_mask = 0;
    CL_Obj spec;
    uint32_t result;
    int kind = CL_AMIGA_RES_UNSIGNED;
    int arg_no = 0;

    if (!CL_FOREIGN_POINTER_P(args[0]))
        cl_error(CL_ERR_TYPE, "AMIGA:CALL-LIBRARY: base must be a foreign pointer");
    fp = (CL_ForeignPtr *)CL_OBJ_TO_PTR(args[0]);
    library_base_check(fp, "AMIGA:CALL-LIBRARY");

    if (!CL_FIXNUM_P(args[1]))
        cl_error(CL_ERR_TYPE, "AMIGA:CALL-LIBRARY: offset must be a fixnum");
    offset = (int16_t)CL_FIXNUM_VAL(args[1]);

    if (nargs > 3 && !CL_NULL_P(args[3])) {
        if (!CL_FIXNUM_P(args[3]) || CL_FIXNUM_VAL(args[3]) < 0 ||
            CL_FIXNUM_VAL(args[3]) > CL_AMIGA_RES_KIND_MAX)
            cl_error(CL_ERR_ARGS,
                     "AMIGA:CALL-LIBRARY: result-kind must be 0 (unsigned), "
                     "1 (void), 2 (pointer), 3 (signed), 4 (bool), 5 (u16), "
                     "6 (i16), 7 (u8) or 8 (i8)");
        kind = (int)CL_FIXNUM_VAL(args[3]);
    }

    /* Clear register array */
    memset(regs, 0, sizeof(regs));

    /* Parse register spec plist: (:D0 42 :A0 ptr ...) */
    spec = args[2];
    while (!CL_NULL_P(spec)) {
        CL_Obj kw, val;
        int reg_idx;
        if (!CL_CONS_P(spec))
            cl_error(CL_ERR_TYPE, "AMIGA:CALL-LIBRARY: malformed register spec");
        kw = cl_car(spec);
        spec = cl_cdr(spec);
        if (CL_NULL_P(spec))
            cl_error(CL_ERR_ARGS, "AMIGA:CALL-LIBRARY: odd number of elements in register spec");
        val = cl_car(spec);
        spec = cl_cdr(spec);

        reg_idx = decode_register_keyword(kw);
        if (reg_idx < 0)
            cl_error(CL_ERR_ARGS, "AMIGA:CALL-LIBRARY: unknown register keyword");
        regs[reg_idx] = cl_amiga_ffi_arg_to_u32(val, ++arg_no, reg_idx);
        reg_mask |= (uint16_t)(1 << reg_idx);
    }

    result = platform_amiga_call(fp->address, offset, regs, reg_mask);
    cl_ffi_deferred_error_check();   /* a Lisp hook the OS called may have parked an error */
    return cl_amiga_box_result(result, kind);
}

/* (amiga:call-library-fast base offset regspec &rest values) → object
 *
 * Fast path: register layout is encoded in REGSPEC as a fixnum of nibbles
 * (low to high), one per value argument:
 *   bits  0-3  -> register index for value 0
 *   bits  4-7  -> register index for value 1
 *   ...
 *   bits 24-27 -> register index for value 6
 * Indices match decode_register_keyword (D0..D7=0..7, A0..A5=8..13).
 * Bits 28-29 select the result kind (CL_AMIGA_RES_*, builtins.h).
 *
 * This avoids the per-call list allocation and keyword walk that
 * CALL-LIBRARY does, at the cost of capping at 7 register args
 * (sufficient for all but a dozen Amiga library calls — those go through
 * CALL-LIBRARY). */
static CL_Obj bi_amiga_call_library_fast(CL_Obj *args, int nargs)
{
    CL_ForeignPtr *fp;
    int16_t offset;
    uint32_t regspec;
    uint32_t regs[14];
    uint16_t reg_mask = 0;
    int n_regs, i;
    uint32_t result;

    if (!CL_FOREIGN_POINTER_P(args[0]))
        cl_error(CL_ERR_TYPE,
                 "AMIGA:CALL-LIBRARY-FAST: base must be a foreign pointer");
    fp = (CL_ForeignPtr *)CL_OBJ_TO_PTR(args[0]);
    library_base_check(fp, "AMIGA:CALL-LIBRARY-FAST");

    if (!CL_FIXNUM_P(args[1]))
        cl_error(CL_ERR_TYPE,
                 "AMIGA:CALL-LIBRARY-FAST: offset must be a fixnum");
    offset = (int16_t)CL_FIXNUM_VAL(args[1]);

    if (!CL_FIXNUM_P(args[2]))
        cl_error(CL_ERR_TYPE,
                 "AMIGA:CALL-LIBRARY-FAST: regspec must be a fixnum");
    regspec = (uint32_t)CL_FIXNUM_VAL(args[2]);

    n_regs = nargs - 3;
    if (n_regs < 0 || n_regs > 7)
        cl_error(CL_ERR_ARGS,
                 "AMIGA:CALL-LIBRARY-FAST: too many register args (max 7)");

    memset(regs, 0, sizeof(regs));
    for (i = 0; i < n_regs; i++) {
        int reg_idx = (int)((regspec >> (i * 4)) & 0xF);
        if (reg_idx > 13)
            cl_error(CL_ERR_ARGS,
                     "AMIGA:CALL-LIBRARY-FAST: invalid register index in regspec");
        regs[reg_idx] = cl_amiga_ffi_arg_to_u32(args[3 + i], i + 1, reg_idx);
        reg_mask |= (uint16_t)(1 << reg_idx);
    }

    result = platform_amiga_call(fp->address, offset, regs, reg_mask);
    cl_ffi_deferred_error_check();
    return cl_amiga_box_result(result, CL_AMIGA_RES_KIND(regspec));
}

/* OP_AMIGA_CALL dispatch helper — called from vm.c when the dedicated
 * bytecode op fires.  Decodes regspec into the trampoline registers,
 * invokes the library, and returns the boxed result.  The caller has
 * already validated base_addr.
 *
 * regspec layout: low 28 bits = 7 nibbles (one register index per arg),
 * bits 28-29 = result kind (CL_AMIGA_RES_*).  Kept inside the positive
 * fixnum range so defcfun can emit it as a literal fixnum. */
CL_Obj cl_amiga_ffi_call_dispatch(uint32_t base_addr, int16_t offset,
                                  uint32_t regspec, int n_args,
                                  CL_Obj *arg_base)
{
    uint32_t regs[14];
    uint16_t reg_mask = 0;
    int i;
    uint32_t result;

    if (n_args < 0 || n_args > 7)
        cl_error(CL_ERR_ARGS,
                 "OP_AMIGA_CALL: too many register args (max 7), got %d",
                 n_args);

    memset(regs, 0, sizeof(regs));
    for (i = 0; i < n_args; i++) {
        int reg_idx = (int)((regspec >> (i * 4)) & 0xF);
        if (reg_idx > 13)
            cl_error(CL_ERR_ARGS,
                     "OP_AMIGA_CALL: invalid register index in regspec");
        regs[reg_idx] = cl_amiga_ffi_arg_to_u32(arg_base[i], i + 1, reg_idx);
        reg_mask |= (uint16_t)(1 << reg_idx);
    }

    result = platform_amiga_call(base_addr, offset, regs, reg_mask);
    /* The OS may have called a Lisp hook / dispatcher in there (CallHookPkt,
     * DoMethod, MUI_NewObject...): re-signal what it parked, on this side of
     * the boundary.  Covers the VM's OP_AMIGA_CALL, the JIT helper and the
     * stub path alike. */
    cl_ffi_deferred_error_check();
    return cl_amiga_box_result(result, CL_AMIGA_RES_KIND(regspec));
}

/* (amiga:alloc-chip size) → foreign-pointer */
static CL_Obj bi_amiga_alloc_chip(CL_Obj *args, int nargs)
{
    uint32_t size, addr;
    (void)nargs;
    if (!CL_FIXNUM_P(args[0]) || CL_FIXNUM_VAL(args[0]) <= 0)
        cl_error(CL_ERR_ARGS, "AMIGA:ALLOC-CHIP: size must be a positive integer");
    size = (uint32_t)CL_FIXNUM_VAL(args[0]);
    addr = platform_amiga_alloc_chip(size);
    if (addr == 0)
        cl_error(CL_ERR_GENERAL, "AMIGA:ALLOC-CHIP: allocation of %u bytes failed",
                 (unsigned)size);
    return cl_make_foreign_pointer(addr, size,
                                    CL_FPTR_FLAG_OWNED | CL_FPTR_FLAG_CHIP);
}

/* (amiga:free-chip fp) → T */
static CL_Obj bi_amiga_free_chip(CL_Obj *args, int nargs)
{
    CL_ForeignPtr *fp;
    (void)nargs;
    if (!CL_FOREIGN_POINTER_P(args[0]))
        cl_error(CL_ERR_TYPE, "AMIGA:FREE-CHIP: argument must be a foreign pointer");
    fp = (CL_ForeignPtr *)CL_OBJ_TO_PTR(args[0]);
    if (!(fp->flags & CL_FPTR_FLAG_CHIP))
        cl_error(CL_ERR_GENERAL, "AMIGA:FREE-CHIP: pointer is not chip memory");
    platform_amiga_free_chip(fp->address, fp->size);
    fp->address = 0;
    fp->flags &= (uint8_t)~(CL_FPTR_FLAG_OWNED | CL_FPTR_FLAG_CHIP);
    return CL_T;
}

/* ================================================================
 * ARexx host port
 *
 * Thin Lisp bindings over platform_amiga_rexx.c.  The handler loop that
 * uses them lives in lib/amiga/arexx.lisp; the command semantics live in
 * lib/dev-commands.lisp.  Nothing here interprets a command.
 * ================================================================ */

/* Convert a Lisp string argument to a freshly allocated UTF-8 C string.
 * Caller platform_free()s it.  Returns NULL for NIL. */
static char *arexx_string_arg(CL_Obj obj, const char *fn, const char *what,
                              uint32_t *len_out)
{
    uint32_t nchars, cap, n;
    char *buf;
    if (len_out) *len_out = 0;
    if (CL_NULL_P(obj))
        return NULL;
    if (!CL_ANY_STRING_P(obj))
        cl_error(CL_ERR_TYPE, "%s: %s must be a string", fn, what);
    nchars = cl_string_length(obj);
    /* Worst case 4 UTF-8 bytes per code point, plus the NUL. */
    cap = nchars * 4u + 1u;
    buf = (char *)platform_alloc(cap);
    if (!buf)
        cl_error(CL_ERR_STORAGE, "%s: out of memory converting %s", fn, what);
    n = cl_string_to_utf8(obj, buf, cap);
    if (len_out) *len_out = n;
    return buf;
}

/* (amiga:arexx-open &optional basename) → port-name string.
 * Must be called from the thread that will run AREXX-WAIT. */
static CL_Obj bi_amiga_arexx_open(CL_Obj *args, int nargs)
{
    char name[64];
    char *base = NULL;
    int rc;
    if (nargs >= 1 && !CL_NULL_P(args[0]))
        base = arexx_string_arg(args[0], "AMIGA:AREXX-OPEN", "the port basename", NULL);
    rc = platform_arexx_open(base ? base : "AMILISP", name, (int)sizeof(name));
    if (base) platform_free(base);
    if (rc != PLATFORM_AREXX_OK)
        cl_error(CL_ERR_GENERAL, "AMIGA:AREXX-OPEN: %s", platform_arexx_strerror(rc));
    return cl_make_string(name, (uint32_t)strlen(name));
}

/* (amiga:arexx-close) → T.  Call from the owning thread. */
static CL_Obj bi_amiga_arexx_close(CL_Obj *args, int nargs)
{
    CL_UNUSED(args); CL_UNUSED(nargs);
    platform_arexx_close();
    return CL_T;
}

/* (amiga:arexx-port-name) → string, or NIL when no port is open */
static CL_Obj bi_amiga_arexx_port_name(CL_Obj *args, int nargs)
{
    char name[64];
    CL_UNUSED(args); CL_UNUSED(nargs);
    if (!platform_arexx_port_name(name, (int)sizeof(name)))
        return CL_NIL;
    return cl_make_string(name, (uint32_t)strlen(name));
}

/* (amiga:arexx-request-stop) → T.  Safe from any thread. */
static CL_Obj bi_amiga_arexx_request_stop(CL_Obj *args, int nargs)
{
    CL_UNUSED(args); CL_UNUSED(nargs);
    platform_arexx_request_stop();
    return CL_T;
}

/* (amiga:arexx-wait) → command string, or NIL when the port was stopped.
 * Blocks in exec Wait() inside a GC safe region. */
static CL_Obj bi_amiga_arexx_wait(CL_Obj *args, int nargs)
{
    const char *cmd = NULL;
    int rc;
    CL_UNUSED(args); CL_UNUSED(nargs);
    rc = platform_arexx_wait(&cmd);
    if (rc < 0)
        cl_error(CL_ERR_GENERAL, "AMIGA:AREXX-WAIT: %s", platform_arexx_strerror(rc));
    if (rc == 0)
        return CL_NIL;   /* stopped */
    if (!cmd)
        return cl_make_string("", 0);
    return cl_make_string(cmd, (uint32_t)strlen(cmd));
}

/* (amiga:arexx-reply rc &optional result) → T */
static CL_Obj bi_amiga_arexx_reply(CL_Obj *args, int nargs)
{
    char *res = NULL;
    uint32_t res_len = 0;
    int32_t rc;
    if (!CL_FIXNUM_P(args[0]))
        cl_error(CL_ERR_TYPE, "AMIGA:AREXX-REPLY: return code must be an integer");
    rc = CL_FIXNUM_VAL(args[0]);
    if (nargs >= 2)
        res = arexx_string_arg(args[1], "AMIGA:AREXX-REPLY", "the result", &res_len);
    platform_arexx_reply(rc, res, res_len);
    if (res) platform_free(res);
    return CL_T;
}

/* (amiga:arexx-send port command &optional (result-size 8192))
 *   → (values rc result-string)
 * Drives another application's ARexx port — and, in the test suite, our own. */
static CL_Obj bi_amiga_arexx_send(CL_Obj *args, int nargs)
{
    char *port = NULL, *cmd = NULL, *res = NULL;
    int32_t rc = PLATFORM_AREXX_RC_FATAL, rc2 = 0;
    int res_size = 8192;
    int status;
    CL_Obj result_obj;

    if (nargs >= 3) {
        if (!CL_FIXNUM_P(args[2]) || CL_FIXNUM_VAL(args[2]) <= 0)
            cl_error(CL_ERR_ARGS, "AMIGA:AREXX-SEND: result-size must be a positive integer");
        res_size = (int)CL_FIXNUM_VAL(args[2]);
    }
    /* Type-check the command before converting the port name: arexx_string_arg()
     * itself cl_error()s (a noreturn longjmp) on a bad type, which would
     * otherwise unwind straight past the platform_free(port) below and leak
     * port's platform_alloc()'d buffer. */
    if (!CL_NULL_P(args[1]) && !CL_ANY_STRING_P(args[1]))
        cl_error(CL_ERR_TYPE, "AMIGA:AREXX-SEND: the command must be a string");

    port = arexx_string_arg(args[0], "AMIGA:AREXX-SEND", "the port name", NULL);
    if (!port)
        cl_error(CL_ERR_ARGS, "AMIGA:AREXX-SEND: port name must not be NIL");
    cmd = arexx_string_arg(args[1], "AMIGA:AREXX-SEND", "the command", NULL);
    if (!cmd) {
        platform_free(port);
        cl_error(CL_ERR_ARGS, "AMIGA:AREXX-SEND: command must not be NIL");
    }
    res = (char *)platform_alloc((uint32_t)res_size);
    if (!res) {
        platform_free(port); platform_free(cmd);
        cl_error(CL_ERR_STORAGE, "AMIGA:AREXX-SEND: out of memory");
    }

    status = platform_arexx_send(port, cmd, &rc, res, res_size, &rc2);
    platform_free(port);
    platform_free(cmd);
    if (status != PLATFORM_AREXX_OK) {
        platform_free(res);
        cl_error(CL_ERR_GENERAL, "AMIGA:AREXX-SEND: %s", platform_arexx_strerror(status));
    }

    /* Amiga/MorphOS builds have no wide strings — an ARexx argstring is a
     * byte string, so a base string is the exact representation. */
    result_obj = cl_make_string(res, (uint32_t)strlen(res));
    platform_free(res);
    cl_mv_count = 2;
    cl_mv_values[0] = CL_MAKE_FIXNUM(rc);
    cl_mv_values[1] = result_obj;
    return cl_mv_values[0];
}

/* (amiga::%arexx-lib-users) → fixnum
 * References held on rexxsyslib: the open port plus every send/reply in
 * progress.  A test hook -- it must be 0 once no port is open and no send is
 * running, and the library must stay open under a send still in flight. */
static CL_Obj bi_amiga_arexx_lib_users(CL_Obj *args, int nargs)
{
    CL_UNUSED(args); CL_UNUSED(nargs);
    return CL_MAKE_FIXNUM((int32_t)platform_arexx_lib_users());
}

/* (amiga:wait-signals mask) → the signals received
 * exec Wait() inside a GC safe region -- the Wait an event loop uses when
 * another Lisp thread may collect meanwhile (platform_wait_signals). */
static CL_Obj bi_amiga_wait_signals(CL_Obj *args, int nargs)
{
    uint32_t mask;
    CL_UNUSED(nargs);
    if (!CL_FIXNUM_P(args[0]) && !CL_BIGNUM_P(args[0]))
        cl_error(CL_ERR_TYPE, "AMIGA:WAIT-SIGNALS: the signal mask must be an integer");
    mask = cl_amiga_ffi_arg_to_u32(args[0], 1, 0);
    if (mask == 0)
        cl_error(CL_ERR_ARGS, "AMIGA:WAIT-SIGNALS: an empty signal mask would wait forever");
    return cl_amiga_box_result(platform_wait_signals(mask), CL_AMIGA_RES_UNSIGNED);
}

#else /* !PLATFORM_AMIGA — host stubs.
 *
 * The AMIGA package and every builtin name exist on the host too, so that
 * the lib/amiga/ tree (including the generated lib/amiga/raw/ bindings) can
 * be read, compiled, FASL'd and unit-tested here.  Each stub signals a clear
 * error naming the function; the VM dispatch stub below covers the
 * OP_AMIGA_CALL path that defcfun compiles to. */

CL_Obj cl_amiga_ffi_call_dispatch(uint32_t base_addr, int16_t offset,
                                  uint32_t regspec, int n_args,
                                  CL_Obj *arg_base)
{
    int i;
    (void)base_addr; (void)offset;

    /* Same argument checks as the Amiga dispatch, so a binding called with
     * a value no register can carry (a string, a float) fails the same way
     * on the host as on the target -- that is what lets lib/amiga be unit-
     * tested here.  Only a call whose arguments would have been loaded
     * reaches the platform error. */
    if (n_args < 0 || n_args > 7)
        cl_error(CL_ERR_ARGS,
                 "OP_AMIGA_CALL: too many register args (max 7), got %d",
                 n_args);
    for (i = 0; i < n_args; i++) {
        int reg_idx = (int)((regspec >> (i * 4)) & 0xF);
        if (reg_idx > 13)
            cl_error(CL_ERR_ARGS,
                     "OP_AMIGA_CALL: invalid register index in regspec");
        (void)cl_amiga_ffi_arg_to_u32(arg_base[i], i + 1, reg_idx);
    }
    cl_error(CL_ERR_GENERAL,
             "AMIGA:%%FFI-CALL: AmigaOS library calls are only available on "
             "AmigaOS/MorphOS builds (this is the host build)");
    return CL_NIL;
}

#define AMIGA_HOST_STUB(cname, lispname)                                    \
    static CL_Obj cname(CL_Obj *args, int nargs)                            \
    {                                                                       \
        (void)args; (void)nargs;                                            \
        cl_error(CL_ERR_GENERAL, "AMIGA:" lispname " is only available on " \
                 "AmigaOS/MorphOS builds (this is the host build)");        \
        return CL_NIL;                                                      \
    }

AMIGA_HOST_STUB(bi_amiga_open_library,       "OPEN-LIBRARY")
AMIGA_HOST_STUB(bi_amiga_close_library,      "CLOSE-LIBRARY")
AMIGA_HOST_STUB(bi_amiga_call_library,       "CALL-LIBRARY")
AMIGA_HOST_STUB(bi_amiga_call_library_fast,  "CALL-LIBRARY-FAST")
AMIGA_HOST_STUB(bi_amiga_alloc_chip,         "ALLOC-CHIP")
AMIGA_HOST_STUB(bi_amiga_free_chip,          "FREE-CHIP")
AMIGA_HOST_STUB(bi_amiga_arexx_open,         "AREXX-OPEN")
AMIGA_HOST_STUB(bi_amiga_arexx_close,        "AREXX-CLOSE")
AMIGA_HOST_STUB(bi_amiga_arexx_port_name,    "AREXX-PORT-NAME")
AMIGA_HOST_STUB(bi_amiga_arexx_request_stop, "AREXX-REQUEST-STOP")
AMIGA_HOST_STUB(bi_amiga_arexx_wait,         "AREXX-WAIT")
AMIGA_HOST_STUB(bi_amiga_arexx_reply,        "AREXX-REPLY")
AMIGA_HOST_STUB(bi_amiga_arexx_send,         "AREXX-SEND")
AMIGA_HOST_STUB(bi_amiga_arexx_lib_users,    "%AREXX-LIB-USERS")
AMIGA_HOST_STUB(bi_amiga_wait_signals,       "WAIT-SIGNALS")

static void amiga_defun(const char *name, CL_CFunc func, int min, int max)
{
    cl_register_builtin_exported(name, func, min, max, cl_package_amiga);
}

#endif /* PLATFORM_AMIGA */

/* ================================================================
 * Platform-neutral: library call through a base variable, and the
 * DEFCFUN stub constructor.  Compiled everywhere so the bindings can be
 * loaded, FASL'd and introspected on the host; only the final
 * cl_amiga_ffi_call_dispatch differs per platform.
 * ================================================================ */

/* The library base a call through BASE_SYM goes to, or a clear error.  The
 * one check both library-call paths make (this file's for the VM and the
 * stubs, jit/runtime_m68k.c's for native code): a call through base 0 is a
 * jump to address -LVO, which crashes the machine instead of signalling.
 * A NULL base is what a heap-image restore leaves in a variable that was
 * not declared with AMIGA.FFI:DEFINE-LIBRARY-VARIABLE (every foreign
 * pointer of the image is zeroed: it belonged to the saving process). */
uint32_t cl_amiga_library_base_address(CL_Obj base_sym)
{
    CL_Obj base_val;
    CL_ForeignPtr *bfp;

    base_val = cl_symbol_value(base_sym);
    if (base_val == CL_UNBOUND)
        cl_error_cell(CL_ERR_UNBOUND, base_sym,
                 "OP_AMIGA_CALL: unbound library base %s",
                 cl_symbol_name(base_sym));
    if (CL_NULL_P(base_val))
        cl_error(CL_ERR_GENERAL,
                 "OP_AMIGA_CALL: library base %s is NIL - the library "
                 "is not open (bindings only open it on AmigaOS/MorphOS)",
                 cl_symbol_name(base_sym));
    if (!CL_FOREIGN_POINTER_P(base_val))
        cl_error(CL_ERR_TYPE,
                 "OP_AMIGA_CALL: %s is not a foreign pointer",
                 cl_symbol_name(base_sym));
    bfp = (CL_ForeignPtr *)CL_OBJ_TO_PTR(base_val);
    if (bfp->address == 0)
        cl_error(CL_ERR_GENERAL,
                 "OP_AMIGA_CALL: library base %s is a NULL pointer - the "
                 "library is not open in this process (a heap image "
                 "zeroes every foreign pointer on restore: declare the "
                 "base with AMIGA.FFI:DEFINE-LIBRARY-VARIABLE, or reopen "
                 "it in EXT:*RESTORE-HOOKS*)",
                 cl_symbol_name(base_sym));
    return bfp->address;
}

CL_Obj cl_amiga_call_via_base_sym(CL_Obj base_sym, int16_t offset,
                                  uint32_t regspec, int n_args,
                                  CL_Obj *args)
{
    uint32_t base = cl_amiga_library_base_address(base_sym);
    return cl_amiga_ffi_call_dispatch(base, offset, regspec, n_args, args);
}

/* (amiga:%make-libcall-stub name base-sym lvo reg-nibbles result-kind nparams)
 *   → FFI stub
 *
 * The object AMIGA.FFI:DEFCFUN installs in NAME's function cell (types.h
 * CL_FfiStub, kind CL_STUB_LIBCALL).  REG-NIBBLES is the 28-bit register
 * layout (one nibble per value arg, low to high, D0..D7=0..7, A0..A4=8..12);
 * RESULT-KIND one of CL_AMIGA_RES_* (0..8); NPARAMS the arity (0..7).
 * The two are packed here into the stub's u32 regspec, which is how the
 * sub-32-bit result kinds (bits 28-31) get past the fixnum range of a
 * Lisp-side regspec literal. */
static CL_Obj make_libcall_stub_checked(const char *who, CL_Obj *args)
{
    CL_Obj name = args[0], base_sym = args[1];
    int32_t lvo, nibbles, kind, nparams;
    int i;

    if (!CL_SYMBOL_P(name))
        cl_error(CL_ERR_TYPE, "%s: NAME must be a symbol", who);
    if (!CL_SYMBOL_P(base_sym))
        cl_error(CL_ERR_TYPE,
                 "%s: %s: library base must be a symbol "
                 "(the special variable holding the open library base)",
                 who, cl_symbol_name(name));
    if (!CL_FIXNUM_P(args[2]) || CL_FIXNUM_VAL(args[2]) > 0 ||
        CL_FIXNUM_VAL(args[2]) < -32768)
        cl_error(CL_ERR_ARGS,
                 "%s: %s: LVO offset must be a negative integer >= -32768",
                 who, cl_symbol_name(name));
    lvo = CL_FIXNUM_VAL(args[2]);
    if (!CL_FIXNUM_P(args[3]) || CL_FIXNUM_VAL(args[3]) < 0 ||
        CL_FIXNUM_VAL(args[3]) > 0x0FFFFFFF)
        cl_error(CL_ERR_ARGS,
                 "%s: %s: register spec must be a fixnum of up to 7 "
                 "register nibbles", who, cl_symbol_name(name));
    nibbles = CL_FIXNUM_VAL(args[3]);
    if (!CL_FIXNUM_P(args[4]) || CL_FIXNUM_VAL(args[4]) < 0 ||
        CL_FIXNUM_VAL(args[4]) > CL_AMIGA_RES_KIND_MAX)
        cl_error(CL_ERR_ARGS, "%s: %s: result kind must be 0..%d",
                 who, cl_symbol_name(name), CL_AMIGA_RES_KIND_MAX);
    kind = CL_FIXNUM_VAL(args[4]);
    if (!CL_FIXNUM_P(args[5]) || CL_FIXNUM_VAL(args[5]) < 0 ||
        CL_FIXNUM_VAL(args[5]) > 7)
        cl_error(CL_ERR_ARGS,
                 "%s: %s: argument count must be 0..7 "
                 "(more registers go through AMIGA:CALL-LIBRARY)",
                 who, cl_symbol_name(name));
    nparams = CL_FIXNUM_VAL(args[5]);
    /* Every used nibble must name a register the dispatcher loads
     * (D0-D7/A0-A4 = 0..12; 13 = A5 is its scratch register). */
    for (i = 0; i < nparams; i++) {
        int reg = (nibbles >> (i * 4)) & 0xF;
        if (reg > 12)
            cl_error(CL_ERR_ARGS,
                     "%s: %s: register index %d for argument %d is not one "
                     "of D0-D7/A0-A4", who, cl_symbol_name(name), reg, i);
    }
    return cl_make_ffi_stub(CL_STUB_LIBCALL, name, base_sym,
                            CL_AMIGA_MAKE_REGSPEC((uint32_t)nibbles, kind),
                            (int16_t)lvo, (uint8_t)nparams);
}

static CL_Obj bi_amiga_make_libcall_stub(CL_Obj *args, int nargs)
{
    (void)nargs;
    return make_libcall_stub_checked("%MAKE-LIBCALL-STUB", args);
}

/* (amiga::%defcfun name base-sym lvo reg-nibbles result-kind nparams) → name
 *
 * What AMIGA.FFI:DEFCFUN expands to: build the stub (same validation as
 * %MAKE-LIBCALL-STUB) and install it in NAME's function cell.  One
 * builtin call per binding keeps the generated modules' FASL units small
 * — the form names only NAME, the base variable and this function. */
static CL_Obj bi_amiga_defcfun(CL_Obj *args, int nargs)
{
    CL_Obj stub, name;
    (void)nargs;
    stub = make_libcall_stub_checked("DEFCFUN", args);
    name = args[0];   /* args are VM-stack slots: forwarded across the alloc */
    cl_symbol_set_function(name, stub);
    return name;
}

/* ================================================================
 * The native string-key hook (AMIGA.MUI:MAKE-STRING-KEY-HOOK)
 *
 * MUI 3.8's String gadget is an Intuition string gadget while it is
 * active, and Intuition runs a string gadget's edit hook -- what
 * MUIA_String_EditHook installs -- on input.device's task.  A Lisp hook
 * there is answered with 0 without running (ffi_callback_handler, the
 * foreign-task rule of specs/mui-bindings.md §10.3.2), so the key it was
 * meant to take goes to the gadget: TAB cycles the focus, C-g types
 * nothing.  This hook is C from end to end and needs no Lisp: it matches
 * the key's raw code and qualifiers against a table the Lisp side filled
 * at creation, and for a match it (1) makes the key a no-op for the
 * gadget -- the InputEvent becomes a release of no key, the SGWork's Code
 * is cleared, the default editing actions are undone and EditOp is
 * EO_NOOP -- and (2) queues the entry's value to the object with
 * MUIM_Application_PushMethod, so the Lisp method runs on the
 * application's own task once the gadget is done with the event.  The
 * answer for an SGH_KEY is always ~0 ("understood", sghooks.h); anything
 * else gets 0.
 *
 * The struct layouts (intuition/sghooks.h SGWork, devices/inputevent.h
 * InputEvent) are read at fixed offsets so that the matcher compiles and
 * runs on the host, where tests/test_amiga_mui.lisp calls the hook's
 * entry with hand-built structs.
 * ================================================================ */

#define SKH_SGH_KEY            1
#define SKH_SGW_IEVENT         20   /* struct InputEvent *IEvent */
#define SKH_SGW_CODE           24   /* UWORD Code */
#define SKH_SGW_ACTIONS        30   /* ULONG Actions */
#define SKH_SGW_EDITOP         42   /* UWORD EditOp */
#define SKH_IE_CODE            6    /* UWORD ie_Code */
#define SKH_IE_QUALIFIER       8    /* UWORD ie_Qualifier */
#define SKH_IECODE_UP_PREFIX   0x80
#define SKH_EO_NOOP            1
#define SKH_SGA_CLEAR          (0x01 | 0x02 | 0x04 | 0x08 | 0x20 | 0x40)
        /* SGA_USE | SGA_END | SGA_BEEP | SGA_REUSE | SGA_NEXTACTIVE | SGA_PREVACTIVE */
#define SKH_QUAL_LSHIFT        0x0001
#define SKH_QUAL_RSHIFT        0x0002
#define SKH_QUAL_LALT          0x0010
#define SKH_QUAL_RALT          0x0020
#define SKH_KEY_DONE           0xFFFFFFFFu

typedef struct {
    uint16_t code;        /* ie_Code of a key press (bit 7 clear) */
    uint16_t qual_mask;   /* the qualifier bits that matter ... */
    uint16_t qual_value;  /* ... and what they must be */
    uint16_t pad;
    uint32_t value;       /* pushed as the method's one argument */
} SKHEntry;

typedef struct StringKeyHook_ {
    uint32_t  app, object, push_method, method;
    uint32_t  count;
    SKHEntry *entries;
    void     *closure;    /* platform_ffi_make_closure handle */
    void     *code;       /* its entry, the hook's h_Entry */
    uint8_t   hook[20];   /* struct Hook: MinNode 8, h_Entry, h_SubEntry, h_Data */
    /* The foreign-pointer handle of HOOK -- owned by the one Lisp object
     * %MAKE-STRING-KEY-HOOK returned (its finalizer releases the side-table
     * slot on the host: one handle per object, mem.c), so the free path
     * nulls that object instead of releasing the slot itself. */
    uint32_t  hook_handle;
    /* diagnostics, readable with STRING-KEY-HOOK-STATS */
    volatile uint32_t calls, matches;
    struct StringKeyHook_ *next;   /* the live list (skh_live) */
} StringKeyHook;

static uint16_t skh_rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t skh_rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void skh_wr16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static void skh_wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

/* The hook's entry: (hook, sgw, msg) in a0/a2/a1.  Runs on whatever task
 * Intuition uses -- no Lisp, no allocation, no error. */
static void skh_handler(void *ud, const CLFFIValue *a, CLFFIValue *ret)
{
    StringKeyHook *h = (StringKeyHook *)ud;
    uint8_t *sgw = (uint8_t *)a[1].p;
    uint8_t *msg = (uint8_t *)a[2].p;
    uint8_t *ie;
    uint16_t code, qual, q;
    uint32_t i;

    ret->u32 = 0;
    h->calls++;
    if (sgw == NULL || msg == NULL) return;
    if (skh_rd32(msg) != SKH_SGH_KEY) return;
    ret->u32 = SKH_KEY_DONE;
    /* IEvent is a native pointer: 4 bytes on the Amiga, 8 in a hand-built
     * SGWork on the host (FFI:POKE-POINTER writes sizeof(void *)). */
    {
        void *p;
        memcpy(&p, sgw + SKH_SGW_IEVENT, sizeof p);
        ie = (uint8_t *)p;
    }
    if (ie == NULL) return;
    code = skh_rd16(ie + SKH_IE_CODE);
    if (code & SKH_IECODE_UP_PREFIX) return;         /* a release is never ours */
    qual = skh_rd16(ie + SKH_IE_QUALIFIER);
    /* Either Shift is Shift, either Alt is Alt (what the keymap and an
     * Emacs layer both say): fold the right-hand ones into the left. */
    q = qual;
    if (q & SKH_QUAL_RSHIFT) q = (uint16_t)((q & ~SKH_QUAL_RSHIFT) | SKH_QUAL_LSHIFT);
    if (q & SKH_QUAL_RALT)   q = (uint16_t)((q & ~SKH_QUAL_RALT) | SKH_QUAL_LALT);
    for (i = 0; i < h->count; i++) {
        const SKHEntry *e = &h->entries[i];
        if (e->code == code && (q & e->qual_mask) == e->qual_value) {
            h->matches++;
            /* A release of no key, no qualifier, no character: a no-op for
             * the class's own edit hook, which MUI 3.8 runs after this one
             * on the same SGWork. */
            skh_wr16(ie + SKH_IE_CODE, (uint16_t)(SKH_IECODE_UP_PREFIX | 0x7F));
            skh_wr16(ie + SKH_IE_QUALIFIER, 0);
            skh_wr16(sgw + SKH_SGW_CODE, 0);
            /* And undo the default editing when it ran before us. */
            skh_wr32(sgw + SKH_SGW_ACTIONS,
                     skh_rd32(sgw + SKH_SGW_ACTIONS) & ~(uint32_t)SKH_SGA_CLEAR);
            skh_wr16(sgw + SKH_SGW_EDITOP, SKH_EO_NOOP);
            platform_amiga_push_method(h->app, h->push_method, h->object,
                                       h->method, e->value);
            return;
        }
    }
}

static uint32_t skh_u32_arg(const char *who, const char *what, CL_Obj v)
{
    /* an integer (a MUIM_ constant is above the fixnum range: a bignum),
     * a foreign pointer, NIL/T -- the register-argument coercion */
    if (!CL_INTEGER_P(v) && !CL_FOREIGN_POINTER_P(v) && !CL_NULL_P(v) && v != CL_T)
        cl_error(CL_ERR_TYPE, "%s: %s must be an unsigned 32-bit integer or a foreign pointer", who, what);
    return cl_amiga_ffi_arg_to_u32(v, 0, 0);
}

/* The live hooks, so that a pointer handed to the builtins is looked up
 * rather than dereferenced: a stray one is refused, never read.  Made and
 * freed on Lisp threads only; the OS's calls into the hook never touch
 * the list. */
static StringKeyHook *skh_live = NULL;
static void          *skh_live_lock = NULL;

static StringKeyHook *skh_of(const char *who, CL_Obj v)
{
    CL_ForeignPtr *fp;
    StringKeyHook *h;
    void *p;
    if (!CL_FOREIGN_POINTER_P(v))
        cl_error(CL_ERR_TYPE, "%s: not a string-key hook (a foreign pointer from %%MAKE-STRING-KEY-HOOK)", who);
    fp = (CL_ForeignPtr *)CL_OBJ_TO_PTR(v);
    p = platform_ffi_resolve(fp->address);
    if (p != NULL) {
        if (skh_live_lock) platform_mutex_lock(skh_live_lock);
        for (h = skh_live; h != NULL; h = h->next)
            if ((void *)h->hook == p) break;
        if (skh_live_lock) platform_mutex_unlock(skh_live_lock);
        if (h != NULL) return h;
    }
    cl_error(CL_ERR_ARGS, "%s: the pointer is not a live string-key hook", who);
    return NULL;
}

static void skh_link(StringKeyHook *h, int add)
{
    StringKeyHook **pp;
    if (skh_live_lock) platform_mutex_lock(skh_live_lock);
    if (add) {
        h->next = skh_live;
        skh_live = h;
    } else {
        for (pp = &skh_live; *pp != NULL; pp = &(*pp)->next)
            if (*pp == h) { *pp = h->next; break; }
    }
    if (skh_live_lock) platform_mutex_unlock(skh_live_lock);
}

/* (amiga:%make-string-key-hook app object push-method method entries)
 *   → hook (a foreign pointer to the struct Hook)
 * ENTRIES: a list of (code qual-mask qual-value value), integers. */
static CL_Obj bi_amiga_make_string_key_hook(CL_Obj *args, int nargs)
{
    static const char *who = "AMIGA:%MAKE-STRING-KEY-HOOK";
    StringKeyHook *h;
    CL_Obj list, entry;
    uint32_t n = 0, i;
    CLFFIType types[3] = { CL_FFI_POINTER, CL_FFI_POINTER, CL_FFI_POINTER };
    int8_t regs[3] = { 8, 10, 9 };   /* a0, a2, a1 */
    (void)nargs;

    for (list = args[4]; !CL_NULL_P(list); list = cl_cdr(list)) {
        if (!CL_CONS_P(list))
            cl_error(CL_ERR_TYPE, "%s: ENTRIES must be a proper list", who);
        n++;
    }
    if (!platform_amiga_push_method_prepare())
        cl_error(CL_ERR_GENERAL, "%s: cannot open utility.library v36, which the push needs", who);

    h = (StringKeyHook *)platform_alloc(sizeof(StringKeyHook));
    if (h == NULL) cl_error(CL_ERR_GENERAL, "%s: out of memory", who);
    memset(h, 0, sizeof(*h));
    h->app         = skh_u32_arg(who, "APP", args[0]);
    h->object      = skh_u32_arg(who, "OBJECT", args[1]);
    h->push_method = skh_u32_arg(who, "PUSH-METHOD", args[2]);
    h->method      = skh_u32_arg(who, "METHOD", args[3]);
    if (n > 0) {
        h->entries = (SKHEntry *)platform_alloc(n * sizeof(SKHEntry));
        if (h->entries == NULL) { platform_free(h); cl_error(CL_ERR_GENERAL, "%s: out of memory", who); }
    }
    for (list = args[4], i = 0; i < n; list = cl_cdr(list), i++) {
        CL_Obj f[4];
        int k;
        entry = cl_car(list);
        for (k = 0; k < 4; k++) {
            if (!CL_CONS_P(entry)) {
                platform_free(h->entries); platform_free(h);
                cl_error(CL_ERR_TYPE, "%s: entry %u is not a list of four integers (code qual-mask qual-value value)", who, (unsigned)i);
            }
            f[k] = cl_car(entry);
            entry = cl_cdr(entry);
        }
        for (k = 0; k < 3; k++) {
            if (!CL_FIXNUM_P(f[k]) || CL_FIXNUM_VAL(f[k]) < 0 || CL_FIXNUM_VAL(f[k]) > 0xFFFF) {
                platform_free(h->entries); platform_free(h);
                cl_error(CL_ERR_ARGS, "%s: entry %u: code, qual-mask and qual-value must be integers 0..65535", who, (unsigned)i);
            }
        }
        if (CL_FIXNUM_VAL(f[0]) & SKH_IECODE_UP_PREFIX) {
            platform_free(h->entries); platform_free(h);
            cl_error(CL_ERR_ARGS, "%s: entry %u: code #x%X is a key release (bit 7 set); the hook matches presses", who, (unsigned)i, (unsigned)CL_FIXNUM_VAL(f[0]));
        }
        h->entries[i].code       = (uint16_t)CL_FIXNUM_VAL(f[0]);
        h->entries[i].qual_mask  = (uint16_t)CL_FIXNUM_VAL(f[1]);
        h->entries[i].qual_value = (uint16_t)CL_FIXNUM_VAL(f[2]);
        h->entries[i].value      = skh_u32_arg(who, "an entry's value", f[3]);
    }
    h->count = n;

    h->code = platform_ffi_make_closure(CL_FFI_U32, 3, types, regs, skh_handler, h, &h->closure);
    if (h->code == NULL) {
        platform_free(h->entries); platform_free(h);
        cl_error(CL_ERR_GENERAL, "%s: cannot build the hook entry (callbacks unsupported or out of memory)", who);
    }
    /* struct Hook: h_Entry at 8, h_SubEntry 12, h_Data 16.  The h_Entry
     * longword is what the OS jumps to; on the host (64-bit code
     * addresses) it is the low half and only STRING-KEY-HOOK-ENTRY is
     * exact -- as for AMIGA.FFI:MAKE-HOOK. */
    skh_wr32(h->hook + 8, (uint32_t)(uintptr_t)h->code);
    skh_wr32(h->hook + 12, 0);
    skh_wr32(h->hook + 16, (uint32_t)(uintptr_t)h);
    h->hook_handle = platform_ffi_register(h->hook);
    if (h->hook_handle == 0) {
        platform_ffi_free_closure(h->closure);
        platform_free(h->entries); platform_free(h);
        cl_error(CL_ERR_GENERAL, "%s: foreign pointer table full", who);
    }
    skh_link(h, 1);
    return cl_make_foreign_pointer(h->hook_handle, 20, 0);
}

/* (amiga:%free-string-key-hook hook) → NIL.  Only once no object holds
 * the hook any more: the entry stub is freed with it.  HOOK becomes a
 * null pointer, so a second free is refused instead of reading freed
 * memory, and its finalizer has nothing left to release. */
static CL_Obj bi_amiga_free_string_key_hook(CL_Obj *args, int nargs)
{
    StringKeyHook *h;
    CL_ForeignPtr *fp;
    (void)nargs;
    if (CL_NULL_P(args[0])) return CL_NIL;
    h = skh_of("AMIGA:%FREE-STRING-KEY-HOOK", args[0]);
    fp = (CL_ForeignPtr *)CL_OBJ_TO_PTR(args[0]);
    skh_link(h, 0);
    platform_ffi_release(h->hook_handle);
    fp->address = 0;
    fp->size = 0;
    h->hook_handle = 0;
    platform_ffi_free_closure(h->closure);
    if (h->entries) platform_free(h->entries);
    platform_free(h);
    return CL_NIL;
}

/* (amiga:%string-key-hook-entry hook) → the entry as a foreign pointer:
 * what the OS calls, exact on the host too (a C function of (hook, sgw,
 * msg) there), for tests that call it themselves.  A fresh handle each
 * time: the pointer object owns it (see StringKeyHook.hook_handle). */
static CL_Obj bi_amiga_string_key_hook_entry(CL_Obj *args, int nargs)
{
    StringKeyHook *h;
    (void)nargs;
    h = skh_of("AMIGA:%STRING-KEY-HOOK-ENTRY", args[0]);
    return cl_make_foreign_pointer(platform_ffi_register(h->code), 0, 0);
}

/* (amiga:%string-key-hook-stats hook) → (calls matches count): how often
 * the OS called the hook, how many keys it took, how many entries it
 * holds.  Diagnostics: "MUI never calls the hook" and "the hook never
 * matches" are different bugs. */
static CL_Obj bi_amiga_string_key_hook_stats(CL_Obj *args, int nargs)
{
    StringKeyHook *h;
    (void)nargs;
    h = skh_of("AMIGA:%STRING-KEY-HOOK-STATS", args[0]);
    return cl_cons(CL_MAKE_FIXNUM((int32_t)h->calls),
                   cl_cons(CL_MAKE_FIXNUM((int32_t)h->matches),
                           cl_cons(CL_MAKE_FIXNUM((int32_t)h->count), CL_NIL)));
}

/* (amiga:%last-pushed-method) → (app push-method object method value) or
 * NIL: the host's record of the last push (platform.h), for the tests.
 * NIL on AmigaOS, where the push really happens. */
static CL_Obj bi_amiga_last_pushed_method(CL_Obj *args, int nargs)
{
    uint32_t out[5];
    CL_Obj result = CL_NIL;
    int i;
    (void)args; (void)nargs;
    if (!platform_amiga_last_pushed_method(out)) return CL_NIL;
    CL_GC_PROTECT(result);
    for (i = 4; i >= 0; i--)
        result = cl_cons(cl_amiga_box_result(out[i], CL_AMIGA_RES_UNSIGNED), result);
    CL_GC_UNPROTECT(1);
    return result;
}

/* ================================================================
 * Init
 * ================================================================ */

void cl_builtins_amiga_init(void)
{
#ifdef PLATFORM_AMIGA
    /* Pre-intern register keywords */
    kw_d0 = cl_intern_in("D0", 2, cl_package_keyword);
    kw_d1 = cl_intern_in("D1", 2, cl_package_keyword);
    kw_d2 = cl_intern_in("D2", 2, cl_package_keyword);
    kw_d3 = cl_intern_in("D3", 2, cl_package_keyword);
    kw_d4 = cl_intern_in("D4", 2, cl_package_keyword);
    kw_d5 = cl_intern_in("D5", 2, cl_package_keyword);
    kw_d6 = cl_intern_in("D6", 2, cl_package_keyword);
    kw_d7 = cl_intern_in("D7", 2, cl_package_keyword);
    kw_a0 = cl_intern_in("A0", 2, cl_package_keyword);
    kw_a1 = cl_intern_in("A1", 2, cl_package_keyword);
    kw_a2 = cl_intern_in("A2", 2, cl_package_keyword);
    kw_a3 = cl_intern_in("A3", 2, cl_package_keyword);
    kw_a4 = cl_intern_in("A4", 2, cl_package_keyword);
    kw_a5 = cl_intern_in("A5", 2, cl_package_keyword);

    /* Register cached keyword symbols for GC compaction forwarding */
    cl_gc_register_root(&kw_d0);
    cl_gc_register_root(&kw_d1);
    cl_gc_register_root(&kw_d2);
    cl_gc_register_root(&kw_d3);
    cl_gc_register_root(&kw_d4);
    cl_gc_register_root(&kw_d5);
    cl_gc_register_root(&kw_d6);
    cl_gc_register_root(&kw_d7);
    cl_gc_register_root(&kw_a0);
    cl_gc_register_root(&kw_a1);
    cl_gc_register_root(&kw_a2);
    cl_gc_register_root(&kw_a3);
    cl_gc_register_root(&kw_a4);
    cl_gc_register_root(&kw_a5);
#endif /* PLATFORM_AMIGA */

    /* Register builtins in AMIGA package — the real implementations on
     * AmigaOS/MorphOS, the AMIGA_HOST_STUB errors elsewhere (same names
     * and arities, so the package surface is identical on every build). */
    /* The native string-key hook (AMIGA.MUI:MAKE-STRING-KEY-HOOK): real on
     * every build -- the matcher is C, the push is recorded on the host.
     * Internal (unexported, like %DEFCFUN): AMIGA.MUI is the documented
     * surface. */
    if (skh_live_lock == NULL) platform_mutex_init(&skh_live_lock);
    cl_register_builtin("%MAKE-STRING-KEY-HOOK",  bi_amiga_make_string_key_hook,  5, 5, cl_package_amiga);
    cl_register_builtin("%FREE-STRING-KEY-HOOK",  bi_amiga_free_string_key_hook,  1, 1, cl_package_amiga);
    cl_register_builtin("%STRING-KEY-HOOK-ENTRY", bi_amiga_string_key_hook_entry, 1, 1, cl_package_amiga);
    cl_register_builtin("%STRING-KEY-HOOK-STATS", bi_amiga_string_key_hook_stats, 1, 1, cl_package_amiga);
    cl_register_builtin("%LAST-PUSHED-METHOD",    bi_amiga_last_pushed_method,    0, 0, cl_package_amiga);

    amiga_defun("OPEN-LIBRARY",      bi_amiga_open_library,       1,  2);
#ifdef PLATFORM_AMIGA
    amiga_defun("CLOSE-LIBRARY",     bi_amiga_close_library,      1,  1);
#else
    /* AMIGA uses FFI, so AMIGA:CLOSE-LIBRARY *is* FFI:CLOSE-LIBRARY -- and
     * on the host that symbol's function is the real one (dlclose of a
     * LOAD-LIBRARY handle, builtins_ffi.c), registered before this.  The
     * host stub used to replace it here, so every host caller of
     * ffi:close-library got "only available on AmigaOS" (2026-09-25,
     * found by the Clamacs host frontend's smoke run).  On the Amiga the
     * Exec CloseLibrary registered above is the one that must win.  The
     * symbol stays exported from AMIGA, so `amiga:close-library' reads on
     * every build (lib/amiga/ffi.lisp spells it that way, and the host
     * compiles those files). */
    {
        CL_Obj sym = cl_intern_in("CLOSE-LIBRARY", 13, cl_package_amiga);
        CL_GC_PROTECT(sym);
        cl_export_symbol(sym, cl_package_amiga);
        CL_GC_UNPROTECT(1);
    }
    (void)bi_amiga_close_library;
#endif
    amiga_defun("CALL-LIBRARY",      bi_amiga_call_library,       3,  4);
    amiga_defun("CALL-LIBRARY-FAST", bi_amiga_call_library_fast,  3, -1);
    amiga_defun("ALLOC-CHIP",        bi_amiga_alloc_chip,         1,  1);
    amiga_defun("FREE-CHIP",         bi_amiga_free_chip,          1,  1);

    /* ARexx host port — transport primitives.  lib/amiga/arexx.lisp builds
     * the handler thread and the command protocol on top of these. */
    amiga_defun("AREXX-OPEN",         bi_amiga_arexx_open,         0,  1);
    amiga_defun("AREXX-CLOSE",        bi_amiga_arexx_close,        0,  0);
    amiga_defun("AREXX-PORT-NAME",    bi_amiga_arexx_port_name,    0,  0);
    amiga_defun("AREXX-REQUEST-STOP", bi_amiga_arexx_request_stop, 0,  0);
    amiga_defun("AREXX-WAIT",         bi_amiga_arexx_wait,         0,  0);
    amiga_defun("AREXX-REPLY",        bi_amiga_arexx_reply,        1,  2);
    amiga_defun("AREXX-SEND",         bi_amiga_arexx_send,         2,  3);
    cl_register_builtin("%AREXX-LIB-USERS", bi_amiga_arexx_lib_users, 0, 0, cl_package_amiga);

    /* exec Wait() as a GC-cooperative blocking call: the wait of every
     * event loop (lib/amiga/mui.lisp, reaction.lisp) and of a program that
     * parks its main task on a signal a worker thread raises. */
    amiga_defun("WAIT-SIGNALS",       bi_amiga_wait_signals,       1,  1);

    /* DEFCFUN's stub constructor (internal, not exported: AMIGA.FFI spells
     * it amiga::%make-libcall-stub; user code shouldn't write it). */
    cl_register_builtin("%MAKE-LIBCALL-STUB", bi_amiga_make_libcall_stub,
                        6, 6, cl_package_amiga);
    cl_register_builtin("%DEFCFUN", bi_amiga_defcfun, 6, 6, cl_package_amiga);

    /* Intern AMIGA::%FFI-CALL — compile_call matches against this exact
     * symbol object to emit OP_AMIGA_CALL.  Exported so Lisp code can
     * write a bare `(amiga:%ffi-call base-sym lvo regspec args...)`
     * without package gymnastics (the Amiga test suite does); DEFCFUN
     * itself no longer emits it — its stubs reach OP_AMIGA_CALL through
     * compile_call's stub hook. */
    cl_amiga_ffi_call_sym = cl_intern_in("%FFI-CALL", 9, cl_package_amiga);
    cl_export_symbol(cl_amiga_ffi_call_sym, cl_package_amiga);
    cl_gc_register_root(&cl_amiga_ffi_call_sym);
}
