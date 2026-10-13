/*
 * builtins_ffi.c — FFI (Foreign Function Interface) package builtins.
 *
 * Platform-independent foreign memory access: foreign pointers,
 * peek/poke, alloc/free, string conversion.
 */

#include "builtins.h"
#include "types.h"
#include "mem.h"
#include "error.h"
#include "symbol.h"
#include "package.h"
#include "bignum.h"
#include "float.h"
#include "string_utils.h"
#include "vm.h"
#include "compiler.h"   /* cl_register_setf_updater (DEFCSTRUCT setters) */
#include "stream.h"     /* cl_write_cstring_to_debug_io (callback diagnostics) */
#include "../platform/platform.h"
#include "../platform/platform_thread.h"
#include <string.h>
#include <stdio.h>

/* ================================================================
 * Integer <-> 64-bit C value conversion
 *
 * Foreign addresses and 64-bit integer arguments/results don't fit in a
 * 30-bit fixnum, so they round-trip through bignums (16-bit little-endian
 * limbs).  These helpers centralise that so every accessor agrees.
 * ================================================================ */

/* CL integer (fixnum or bignum) -> uint64_t (low 64 bits, two's complement). */
uint64_t cl_ffi_obj_to_u64(CL_Obj o)
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
    cl_error(CL_ERR_TYPE, "FFI: expected an integer");
    return 0;
}

/* uint64_t -> CL integer (fixnum if it fits, else normalized bignum). */
CL_Obj cl_ffi_u64_to_obj(uint64_t v)
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

/* int64_t -> CL integer (signed). */
CL_Obj cl_ffi_i64_to_obj(int64_t v)
{
    uint64_t mag;
    CL_Obj bn;
    CL_Bignum *b;
    if (v >= (int64_t)CL_FIXNUM_MIN && v <= (int64_t)CL_FIXNUM_MAX)
        return CL_MAKE_FIXNUM((int32_t)v);
    mag = (v < 0) ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;
    bn = cl_make_bignum(4, v < 0 ? 1 : 0);
    b = (CL_Bignum *)CL_OBJ_TO_PTR(bn);
    b->limbs[0] = (uint16_t)(mag & 0xFFFF);
    b->limbs[1] = (uint16_t)((mag >> 16) & 0xFFFF);
    b->limbs[2] = (uint16_t)((mag >> 32) & 0xFFFF);
    b->limbs[3] = (uint16_t)((mag >> 48) & 0xFFFF);
    return cl_bignum_normalize(bn);
}

/* ================================================================
 * Helper: register function in FFI package and export
 * ================================================================ */

/* Routed through cl_register_builtin_exported so the image-relink registry
 * sees the registration (builtins.h). */
static void ffi_defun(const char *name, CL_CFunc func, int min, int max)
{
    cl_register_builtin_exported(name, func, min, max, cl_package_ffi);
}

/* ================================================================
 * Foreign pointer builtins
 * ================================================================ */

/* (ffi:make-foreign-pointer address &optional size) → foreign-pointer
 *
 * ADDRESS is a *real* machine address (the value seen by
 * FFI:FOREIGN-POINTER-ADDRESS / CFFI POINTER-ADDRESS), which on a 64-bit
 * host does not fit in a CL_ForeignPtr handle.  We register the real pointer
 * so it round-trips through a handle.  The result is unowned (size is advisory
 * only and not used to free anything). */
static CL_Obj bi_ffi_make_fp(CL_Obj *args, int nargs)
{
    uint64_t addr;
    uint32_t size, handle;
    (void)size;
    if (!CL_FIXNUM_P(args[0]) && !CL_BIGNUM_P(args[0]))
        cl_error(CL_ERR_TYPE, "FFI:MAKE-FOREIGN-POINTER: address must be an integer");
    addr = cl_ffi_obj_to_u64(args[0]);
    size = (nargs > 1 && !CL_NULL_P(args[1])) ? (uint32_t)CL_FIXNUM_VAL(args[1]) : 0;
    handle = platform_ffi_register((void *)(uintptr_t)addr);
    return cl_make_foreign_pointer(handle, size, 0);
}

/* (ffi:foreign-pointer-address fp) → integer
 * Returns the *real* address (resolved through the side table on POSIX). */
static CL_Obj bi_ffi_fp_address(CL_Obj *args, int nargs)
{
    CL_ForeignPtr *fp;
    void *real;
    (void)nargs;
    if (!CL_FOREIGN_POINTER_P(args[0]))
        cl_error(CL_ERR_TYPE, "FFI:FOREIGN-POINTER-ADDRESS: argument must be a foreign pointer");
    fp = (CL_ForeignPtr *)CL_OBJ_TO_PTR(args[0]);
    real = platform_ffi_resolve(fp->address);
    return cl_ffi_u64_to_obj((uint64_t)(uintptr_t)real);
}

/* (ffi:foreign-pointer-p obj) → T or NIL */
static CL_Obj bi_ffi_fp_p(CL_Obj *args, int nargs)
{
    (void)nargs;
    return CL_FOREIGN_POINTER_P(args[0]) ? CL_T : CL_NIL;
}

/* (ffi:null-pointer-p fp) → T or NIL.  True when the real address is 0. */
static CL_Obj bi_ffi_null_p(CL_Obj *args, int nargs)
{
    CL_ForeignPtr *fp;
    (void)nargs;
    if (!CL_FOREIGN_POINTER_P(args[0]))
        cl_error(CL_ERR_TYPE, "FFI:NULL-POINTER-P: argument must be a foreign pointer");
    fp = (CL_ForeignPtr *)CL_OBJ_TO_PTR(args[0]);
    if (fp->address == 0) return CL_T;
    return (platform_ffi_resolve(fp->address) == NULL) ? CL_T : CL_NIL;
}

/* (ffi:pointer-eq a b) → T or NIL.  Compares real addresses. */
static CL_Obj bi_ffi_pointer_eq(CL_Obj *args, int nargs)
{
    CL_ForeignPtr *a, *b;
    (void)nargs;
    if (!CL_FOREIGN_POINTER_P(args[0]) || !CL_FOREIGN_POINTER_P(args[1]))
        cl_error(CL_ERR_TYPE, "FFI:POINTER-EQ: arguments must be foreign pointers");
    a = (CL_ForeignPtr *)CL_OBJ_TO_PTR(args[0]);
    b = (CL_ForeignPtr *)CL_OBJ_TO_PTR(args[1]);
    return (platform_ffi_resolve(a->address) == platform_ffi_resolve(b->address))
           ? CL_T : CL_NIL;
}

/* (ffi:alloc-foreign size) → foreign-pointer */
static CL_Obj bi_ffi_alloc(CL_Obj *args, int nargs)
{
    uint32_t size, handle;
    (void)nargs;
    if (!CL_FIXNUM_P(args[0]) || CL_FIXNUM_VAL(args[0]) <= 0)
        cl_error(CL_ERR_ARGS, "FFI:ALLOC-FOREIGN: size must be a positive integer");
    size = (uint32_t)CL_FIXNUM_VAL(args[0]);
    handle = platform_ffi_alloc(size);
    if (handle == 0)
        cl_error(CL_ERR_GENERAL, "FFI:ALLOC-FOREIGN: allocation of %u bytes failed",
                 (unsigned)size);
    return cl_make_foreign_pointer(handle, size, CL_FPTR_FLAG_OWNED);
}

/* (ffi:free-foreign fp) → T */
static CL_Obj bi_ffi_free(CL_Obj *args, int nargs)
{
    CL_ForeignPtr *fp;
    (void)nargs;
    if (!CL_FOREIGN_POINTER_P(args[0]))
        cl_error(CL_ERR_TYPE, "FFI:FREE-FOREIGN: argument must be a foreign pointer");
    fp = (CL_ForeignPtr *)CL_OBJ_TO_PTR(args[0]);
    if (!(fp->flags & CL_FPTR_FLAG_OWNED))
        cl_error(CL_ERR_GENERAL, "FFI:FREE-FOREIGN: pointer was not allocated by FFI");
    platform_ffi_free(fp->address, fp->size);
    fp->address = 0;
    fp->flags &= (uint8_t)~CL_FPTR_FLAG_OWNED;
    return CL_T;
}

/* ================================================================
 * Peek/Poke builtins
 *
 * (ffi:peek-u32 fp &optional offset) → integer
 * (ffi:poke-u32 fp value &optional offset) → value
 * ================================================================ */

/* Helper: extract address handle from foreign-pointer arg */
static uint32_t ffi_get_handle(CL_Obj arg)
{
    CL_ForeignPtr *fp;
    if (!CL_FOREIGN_POINTER_P(arg))
        cl_error(CL_ERR_TYPE, "FFI: argument must be a foreign pointer");
    fp = (CL_ForeignPtr *)CL_OBJ_TO_PTR(arg);
    return fp->address;
}

/* Helper: resolve (foreign-pointer arg + optional byte offset) to a real,
 * dereferenceable byte address.  OFFSET_IDX is the argument index of the
 * optional offset (used by the typed peek/poke accessors below). */
static uint8_t *ffi_resolve_at(CL_Obj *args, int nargs, int offset_idx)
{
    void *base = platform_ffi_resolve(ffi_get_handle(args[0]));
    uint32_t offset;
    if (!base)
        cl_error(CL_ERR_GENERAL, "FFI: dereference of an invalid/null foreign pointer");
    offset = (nargs > offset_idx && !CL_NULL_P(args[offset_idx]))
             ? (uint32_t)CL_FIXNUM_VAL(args[offset_idx]) : 0;
    return (uint8_t *)base + offset;
}

/* (ffi:peek-u32 fp &optional offset) */
static CL_Obj bi_ffi_peek32(CL_Obj *args, int nargs)
{
    uint32_t handle = ffi_get_handle(args[0]);
    uint32_t offset = (nargs > 1 && !CL_NULL_P(args[1]))
                      ? (uint32_t)CL_FIXNUM_VAL(args[1]) : 0;
    uint32_t val = platform_ffi_peek32(handle, offset);
    if (val <= (uint32_t)CL_FIXNUM_MAX)
        return CL_MAKE_FIXNUM((int32_t)val);
    else {
        CL_Obj bn = cl_make_bignum(2, 0);
        CL_Bignum *b = (CL_Bignum *)CL_OBJ_TO_PTR(bn);
        b->limbs[0] = (uint16_t)(val & 0xFFFF);
        b->limbs[1] = (uint16_t)(val >> 16);
        return bn;
    }
}

/* (ffi:peek-u16 fp &optional offset) */
static CL_Obj bi_ffi_peek16(CL_Obj *args, int nargs)
{
    uint32_t handle = ffi_get_handle(args[0]);
    uint32_t offset = (nargs > 1 && !CL_NULL_P(args[1]))
                      ? (uint32_t)CL_FIXNUM_VAL(args[1]) : 0;
    uint16_t val = platform_ffi_peek16(handle, offset);
    return CL_MAKE_FIXNUM((int32_t)val);
}

/* (ffi:peek-u8 fp &optional offset) */
static CL_Obj bi_ffi_peek8(CL_Obj *args, int nargs)
{
    uint32_t handle = ffi_get_handle(args[0]);
    uint32_t offset = (nargs > 1 && !CL_NULL_P(args[1]))
                      ? (uint32_t)CL_FIXNUM_VAL(args[1]) : 0;
    uint8_t val = platform_ffi_peek8(handle, offset);
    return CL_MAKE_FIXNUM((int32_t)val);
}

/* (ffi:poke-u32 fp value &optional offset) */
static CL_Obj bi_ffi_poke32(CL_Obj *args, int nargs)
{
    uint32_t handle = ffi_get_handle(args[0]);
    uint32_t val;
    uint32_t offset = (nargs > 2 && !CL_NULL_P(args[2]))
                      ? (uint32_t)CL_FIXNUM_VAL(args[2]) : 0;
    if (CL_FIXNUM_P(args[1]))
        val = (uint32_t)CL_FIXNUM_VAL(args[1]);
    else if (CL_BIGNUM_P(args[1])) {
        CL_Bignum *bn = (CL_Bignum *)CL_OBJ_TO_PTR(args[1]);
        val = (uint32_t)bn->limbs[0];
        if (bn->length > 1)
            val |= ((uint32_t)bn->limbs[1]) << 16;
    } else
        cl_error(CL_ERR_TYPE, "FFI:POKE-U32: value must be an integer");
    platform_ffi_poke32(handle, offset, val);
    return args[1];
}

/* (ffi:poke-u16 fp value &optional offset) */
static CL_Obj bi_ffi_poke16(CL_Obj *args, int nargs)
{
    uint32_t handle = ffi_get_handle(args[0]);
    uint32_t offset = (nargs > 2 && !CL_NULL_P(args[2]))
                      ? (uint32_t)CL_FIXNUM_VAL(args[2]) : 0;
    if (!CL_FIXNUM_P(args[1]))
        cl_error(CL_ERR_TYPE, "FFI:POKE-U16: value must be a fixnum");
    platform_ffi_poke16(handle, offset, (uint16_t)CL_FIXNUM_VAL(args[1]));
    return args[1];
}

/* (ffi:poke-u8 fp value &optional offset) */
static CL_Obj bi_ffi_poke8(CL_Obj *args, int nargs)
{
    uint32_t handle = ffi_get_handle(args[0]);
    uint32_t offset = (nargs > 2 && !CL_NULL_P(args[2]))
                      ? (uint32_t)CL_FIXNUM_VAL(args[2]) : 0;
    if (!CL_FIXNUM_P(args[1]))
        cl_error(CL_ERR_TYPE, "FFI:POKE-U8: value must be a fixnum");
    platform_ffi_poke8(handle, offset, (uint8_t)CL_FIXNUM_VAL(args[1]));
    return args[1];
}

/* ================================================================
 * Bulk byte transfer
 *
 * POKE-U8 in a Lisp loop costs a VM dispatch, a handle resolve and a
 * fixnum unbox per byte, which dominates any path that pushes a real
 * buffer into foreign memory (bitplane rows, chip-RAM masks).  These
 * move a whole span in one call.
 *
 * A CL string holds its bytes contiguously, so it memcpys; a general
 * vector holds one tagged CL_Obj per element (clamiga upgrades
 * (UNSIGNED-BYTE 8) to T — see UPGRADED-ARRAY-ELEMENT-TYPE), so it
 * unpacks in a tight C loop instead.  Both beat per-byte POKE-U8; only
 * the string form reaches memcpy speed.
 * ================================================================ */

/* Resolve the (SOURCE, START, END) span of a byte-source argument.
 * Returns the element count, and sets whichever of SVEC, SSTR and SBV
 * matches SOURCE's representation (exactly one is left non-NULL).  A packed
 * byte vector (SBV) is the ideal representation here — the copy in/out of
 * foreign memory degenerates to a single memcpy. */
static uint32_t ffi_byte_span(CL_Obj source, CL_Obj start_arg, CL_Obj end_arg,
                              const char *who, CL_Vector **svec, CL_String **sstr,
                              CL_ByteVector **sbv, uint32_t *start_out)
{
    uint32_t len, start, end;

    *svec = NULL;
    *sstr = NULL;
    *sbv = NULL;

    if (CL_STRING_P(source)) {
        *sstr = (CL_String *)CL_OBJ_TO_PTR(source);
        len = (*sstr)->length;
    } else if (CL_BYTE_VECTOR_P(source)) {
        *sbv = (CL_ByteVector *)CL_OBJ_TO_PTR(source);
        if ((*sbv)->elt_shift)
            cl_error(CL_ERR_TYPE,
                     "%s: source must hold octets - a packed "
                     "(UNSIGNED-BYTE 16)/(SIGNED-BYTE 16) vector holds "
                     "2-byte elements; use an (UNSIGNED-BYTE 8) vector", who);
        len = cl_bytevec_active_length(*sbv);
    } else if (CL_VECTOR_P(source)) {
        *svec = (CL_Vector *)CL_OBJ_TO_PTR(source);
        if ((*svec)->rank > 1)
            cl_error(CL_ERR_TYPE, "%s: source must be a vector, not a rank-%d array",
                     who, (int)(*svec)->rank);
        len = cl_vector_active_length(*svec);
    } else {
        cl_error(CL_ERR_TYPE, "%s: source must be a vector or a string", who);
        return 0;
    }

    start = CL_NULL_P(start_arg) ? 0 : (uint32_t)CL_FIXNUM_VAL(start_arg);
    end   = CL_NULL_P(end_arg)   ? len : (uint32_t)CL_FIXNUM_VAL(end_arg);
    if (!CL_NULL_P(start_arg) && !CL_FIXNUM_P(start_arg))
        cl_error(CL_ERR_TYPE, "%s: START must be a fixnum", who);
    if (!CL_NULL_P(end_arg) && !CL_FIXNUM_P(end_arg))
        cl_error(CL_ERR_TYPE, "%s: END must be a fixnum", who);
    if (end > len)
        cl_error(CL_ERR_GENERAL, "%s: END %u is past the end of a %u-element source",
                 who, (unsigned)end, (unsigned)len);
    if (start > end)
        cl_error(CL_ERR_GENERAL, "%s: START %u is past END %u",
                 who, (unsigned)start, (unsigned)end);

    *start_out = start;
    return end - start;
}

/* (ffi:poke-bytes fp source &optional offset start end) → count
 *
 * Copies SOURCE[START..END) into foreign memory at FP + OFFSET.  SOURCE is
 * a string (memcpy) or a vector of (INTEGER 0 255).  Returns the number of
 * bytes written. */
static CL_Obj bi_ffi_poke_bytes(CL_Obj *args, int nargs)
{
    CL_ForeignPtr *fp;
    CL_Vector *svec;
    CL_String *sstr;
    CL_ByteVector *sbv;
    uint8_t *dest;
    void *base;
    uint32_t fp_size, fp_addr;
    uint32_t offset, start, count, i;

    if (!CL_FOREIGN_POINTER_P(args[0]))
        cl_error(CL_ERR_TYPE, "FFI:POKE-BYTES: first argument must be a foreign pointer");
    /* Copy the fields out immediately: FP is a raw heap pointer, and no raw
     * heap pointer may stay live across anything that might allocate. */
    fp = (CL_ForeignPtr *)CL_OBJ_TO_PTR(args[0]);
    fp_size = fp->size;
    fp_addr = fp->address;

    if (nargs > 2 && !CL_NULL_P(args[2])) {
        if (!CL_FIXNUM_P(args[2]))
            cl_error(CL_ERR_TYPE, "FFI:POKE-BYTES: OFFSET must be a fixnum");
        offset = (uint32_t)CL_FIXNUM_VAL(args[2]);
    } else
        offset = 0;

    count = ffi_byte_span(args[1],
                          (nargs > 3) ? args[3] : CL_NIL,
                          (nargs > 4) ? args[4] : CL_NIL,
                          "FFI:POKE-BYTES", &svec, &sstr, &sbv, &start);

    /* A known allocation size lets us reject an overrun here rather than
     * corrupting whatever sits past the buffer.  size 0 = external memory
     * of unknown extent, so we have nothing to check against. */
    if (fp_size > 0 && (offset > fp_size || count > fp_size - offset))
        cl_error(CL_ERR_GENERAL,
                 "FFI:POKE-BYTES: writing %u bytes at offset %u overruns a %u-byte buffer",
                 (unsigned)count, (unsigned)offset, (unsigned)fp_size);

    base = platform_ffi_resolve(fp_addr);
    if (!base)
        cl_error(CL_ERR_GENERAL, "FFI:POKE-BYTES: invalid/null foreign pointer");
    dest = (uint8_t *)base + offset;

    /* Re-derive the source pointer from the (GC-rooted) argument now that
     * every check is done, rather than trusting the one FFI_BYTE_SPAN
     * produced earlier.  No allocation happens from here on, so neither
     * DEST nor the source data can be invalidated by a compaction mid-copy. */
    if (sstr) {
        sstr = (CL_String *)CL_OBJ_TO_PTR(args[1]);
    } else if (sbv) {
        sbv = (CL_ByteVector *)CL_OBJ_TO_PTR(args[1]);
    } else {
        svec = (CL_Vector *)CL_OBJ_TO_PTR(args[1]);
    }

    if (sstr) {
        memcpy(dest, sstr->data + start, count);
    } else if (sbv) {
        /* Packed byte vector: raw bytes straight into foreign memory. */
        memcpy(dest, sbv->data + start, count);
    } else {
        CL_Obj *elts = cl_vector_data(svec) + start;
        for (i = 0; i < count; i++) {
            CL_Obj e = elts[i];
            int32_t v;
            if (!CL_FIXNUM_P(e))
                cl_error(CL_ERR_TYPE,
                         "FFI:POKE-BYTES: element %u is not an integer",
                         (unsigned)(start + i));
            v = CL_FIXNUM_VAL(e);
            if (v < 0 || v > 255)
                cl_error(CL_ERR_TYPE,
                         "FFI:POKE-BYTES: element %u is %d, not in [0,255]",
                         (unsigned)(start + i), (int)v);
            dest[i] = (uint8_t)v;
        }
    }
    return CL_MAKE_FIXNUM((int32_t)count);
}

/* (ffi:peek-bytes fp vector &optional offset start end) → count
 *
 * The inverse: fills VECTOR[START..END) from foreign memory at FP + OFFSET
 * with fixnum byte values.  Returns the number of bytes read. */
static CL_Obj bi_ffi_peek_bytes(CL_Obj *args, int nargs)
{
    CL_ForeignPtr *fp;
    CL_Vector *svec;
    CL_String *sstr;
    CL_ByteVector *sbv;
    const uint8_t *src;
    void *base;
    uint32_t fp_size, fp_addr;
    uint32_t offset, start, count, i;

    if (!CL_FOREIGN_POINTER_P(args[0]))
        cl_error(CL_ERR_TYPE, "FFI:PEEK-BYTES: first argument must be a foreign pointer");
    /* See FFI:POKE-BYTES: copy the fields out before anything may allocate. */
    fp = (CL_ForeignPtr *)CL_OBJ_TO_PTR(args[0]);
    fp_size = fp->size;
    fp_addr = fp->address;

    if (nargs > 2 && !CL_NULL_P(args[2])) {
        if (!CL_FIXNUM_P(args[2]))
            cl_error(CL_ERR_TYPE, "FFI:PEEK-BYTES: OFFSET must be a fixnum");
        offset = (uint32_t)CL_FIXNUM_VAL(args[2]);
    } else
        offset = 0;

    count = ffi_byte_span(args[1],
                          (nargs > 3) ? args[3] : CL_NIL,
                          (nargs > 4) ? args[4] : CL_NIL,
                          "FFI:PEEK-BYTES", &svec, &sstr, &sbv, &start);

    if (fp_size > 0 && (offset > fp_size || count > fp_size - offset))
        cl_error(CL_ERR_GENERAL,
                 "FFI:PEEK-BYTES: reading %u bytes at offset %u overruns a %u-byte buffer",
                 (unsigned)count, (unsigned)offset, (unsigned)fp_size);

    base = platform_ffi_resolve(fp_addr);
    if (!base)
        cl_error(CL_ERR_GENERAL, "FFI:PEEK-BYTES: invalid/null foreign pointer");
    src = (const uint8_t *)base + offset;

    /* Re-derive from the GC-rooted argument; no allocation from here on. */
    if (sstr) {
        sstr = (CL_String *)CL_OBJ_TO_PTR(args[1]);
    } else if (sbv) {
        sbv = (CL_ByteVector *)CL_OBJ_TO_PTR(args[1]);
    } else {
        svec = (CL_Vector *)CL_OBJ_TO_PTR(args[1]);
    }

    if (sstr) {
        memcpy(sstr->data + start, src, count);
    } else if (sbv) {
        /* Packed byte vector: raw bytes straight out of foreign memory. */
        memcpy(sbv->data + start, src, count);
    } else {
        CL_Obj *elts = cl_vector_data(svec) + start;
        for (i = 0; i < count; i++)
            elts[i] = CL_MAKE_FIXNUM((int32_t)src[i]);
    }
    return CL_MAKE_FIXNUM((int32_t)count);
}

/* ================================================================
 * String conversion builtins
 * ================================================================ */

/* (ffi:foreign-string str) → foreign-pointer
 * Copies a CL string to null-terminated foreign memory. */
static CL_Obj bi_ffi_foreign_string(CL_Obj *args, int nargs)
{
    CL_String *s;
    uint32_t handle;
    void *dest;
    (void)nargs;
    if (!CL_STRING_P(args[0]))
        cl_error(CL_ERR_TYPE, "FFI:FOREIGN-STRING: argument must be a string");
    s = (CL_String *)CL_OBJ_TO_PTR(args[0]);
    handle = platform_ffi_alloc(s->length + 1);
    if (handle == 0)
        cl_error(CL_ERR_GENERAL, "FFI:FOREIGN-STRING: allocation failed");
    dest = platform_ffi_resolve(handle);
    if (dest) {
        memcpy(dest, s->data, s->length);
        ((char *)dest)[s->length] = '\0';
    }
    return cl_make_foreign_pointer(handle, s->length + 1, CL_FPTR_FLAG_OWNED);
}

/* (ffi:foreign-to-string fp &optional max-len) → string
 * Reads a null-terminated string from foreign memory: up to the NUL, or at
 * most MAX-LEN bytes when that is given.  No hidden cap without it -- a
 * 4096-byte default once cut every TextEditor export in Clamacs to 4 KB,
 * silently, saved files included. */
static CL_Obj bi_ffi_foreign_to_string(CL_Obj *args, int nargs)
{
    uint32_t handle;
    void *base;
    const char *cstr;
    uint32_t len, maxlen;

    handle = ffi_get_handle(args[0]);
    base = platform_ffi_resolve(handle);
    if (!base)
        cl_error(CL_ERR_GENERAL, "FFI:FOREIGN-TO-STRING: invalid foreign pointer");
    cstr = (const char *)base;
    maxlen = 0xFFFFFFFFUL;
    if (nargs > 1 && !CL_NULL_P(args[1])) {
        if (!CL_FIXNUM_P(args[1]) || CL_FIXNUM_VAL(args[1]) < 0) {
            char val[64];
            cl_error(CL_ERR_TYPE, "FFI:FOREIGN-TO-STRING: max-len must be a "
                     "non-negative integer or NIL (read to the NUL), got %s",
                     cl_obj_brief(args[1], val, sizeof(val)));
        }
        maxlen = (uint32_t)CL_FIXNUM_VAL(args[1]);
    }
    for (len = 0; len < maxlen && cstr[len] != '\0'; len++)
        ;
    return cl_make_string(cstr, len);
}

/* ================================================================
 * Pointer arithmetic
 * ================================================================ */

/* (ffi:pointer+ fp offset) → foreign-pointer
 * Returns a new (unowned) foreign pointer at the real address of FP + OFFSET. */
static CL_Obj bi_ffi_pointer_plus(CL_Obj *args, int nargs)
{
    CL_ForeignPtr *fp;
    int32_t offset;
    uint8_t *real;
    uint32_t handle;
    (void)nargs;
    if (!CL_FOREIGN_POINTER_P(args[0]))
        cl_error(CL_ERR_TYPE, "FFI:POINTER+: first argument must be a foreign pointer");
    if (!CL_FIXNUM_P(args[1]))
        cl_error(CL_ERR_TYPE, "FFI:POINTER+: offset must be a fixnum");
    fp = (CL_ForeignPtr *)CL_OBJ_TO_PTR(args[0]);
    offset = CL_FIXNUM_VAL(args[1]);
    real = (uint8_t *)platform_ffi_resolve(fp->address);
    handle = platform_ffi_register(real + offset);
    return cl_make_foreign_pointer(handle, 0, 0);
}

/* ================================================================
 * Typed peek/poke — signed, 64-bit, float/double, and pointer access.
 *
 * These resolve the foreign pointer to a real address and do a typed
 * load/store there.  Unaligned access is permitted (68020+ tolerates it,
 * as does every host CPU we target); the existing Amiga peek/poke already
 * relies on this.  Used by CFFI's %MEM-REF / %MEM-SET.
 * ================================================================ */

/* (ffi:peek-i8 fp &optional offset) → signed integer */
static CL_Obj bi_ffi_peek_i8(CL_Obj *args, int nargs)
{
    return CL_MAKE_FIXNUM((int32_t)*(int8_t *)ffi_resolve_at(args, nargs, 1));
}

/* (ffi:peek-i16 fp &optional offset) → signed integer */
static CL_Obj bi_ffi_peek_i16(CL_Obj *args, int nargs)
{
    int16_t v;
    memcpy(&v, ffi_resolve_at(args, nargs, 1), sizeof(v));
    return CL_MAKE_FIXNUM((int32_t)v);
}

/* (ffi:peek-i32 fp &optional offset) → signed integer */
static CL_Obj bi_ffi_peek_i32(CL_Obj *args, int nargs)
{
    int32_t v;
    memcpy(&v, ffi_resolve_at(args, nargs, 1), sizeof(v));
    return cl_bignum_normalize(cl_bignum_from_int32(v));
}

/* (ffi:peek-u64 fp &optional offset) → unsigned integer */
static CL_Obj bi_ffi_peek_u64(CL_Obj *args, int nargs)
{
    uint64_t v;
    memcpy(&v, ffi_resolve_at(args, nargs, 1), sizeof(v));
    return cl_ffi_u64_to_obj(v);
}

/* (ffi:peek-i64 fp &optional offset) → signed integer */
static CL_Obj bi_ffi_peek_i64(CL_Obj *args, int nargs)
{
    int64_t v;
    memcpy(&v, ffi_resolve_at(args, nargs, 1), sizeof(v));
    return cl_ffi_i64_to_obj(v);
}

/* (ffi:peek-single fp &optional offset) → single-float */
static CL_Obj bi_ffi_peek_single(CL_Obj *args, int nargs)
{
    float v;
    memcpy(&v, ffi_resolve_at(args, nargs, 1), sizeof(v));
    return cl_make_single_float(v);
}

/* (ffi:peek-double fp &optional offset) → double-float */
static CL_Obj bi_ffi_peek_double(CL_Obj *args, int nargs)
{
    double v;
    memcpy(&v, ffi_resolve_at(args, nargs, 1), sizeof(v));
    return cl_make_double_float(v);
}

/* (ffi:peek-pointer fp &optional offset) → foreign-pointer
 * Reads a machine pointer from memory and wraps it (unowned). */
static CL_Obj bi_ffi_peek_pointer(CL_Obj *args, int nargs)
{
    void *v;
    memcpy(&v, ffi_resolve_at(args, nargs, 1), sizeof(v));
    return cl_make_foreign_pointer(platform_ffi_register(v), 0, 0);
}

/* (ffi:poke-i8 fp value &optional offset) → value */
static CL_Obj bi_ffi_poke_i8(CL_Obj *args, int nargs)
{
    int8_t v = (int8_t)cl_ffi_obj_to_u64(args[1]);
    *(int8_t *)ffi_resolve_at(args, nargs, 2) = v;
    return args[1];
}

/* (ffi:poke-i16 fp value &optional offset) → value */
static CL_Obj bi_ffi_poke_i16(CL_Obj *args, int nargs)
{
    int16_t v = (int16_t)cl_ffi_obj_to_u64(args[1]);
    memcpy(ffi_resolve_at(args, nargs, 2), &v, sizeof(v));
    return args[1];
}

/* (ffi:poke-i32 fp value &optional offset) → value */
static CL_Obj bi_ffi_poke_i32(CL_Obj *args, int nargs)
{
    int32_t v = (int32_t)cl_ffi_obj_to_u64(args[1]);
    memcpy(ffi_resolve_at(args, nargs, 2), &v, sizeof(v));
    return args[1];
}

/* (ffi:poke-u64 fp value &optional offset) → value */
static CL_Obj bi_ffi_poke_u64(CL_Obj *args, int nargs)
{
    uint64_t v = cl_ffi_obj_to_u64(args[1]);
    memcpy(ffi_resolve_at(args, nargs, 2), &v, sizeof(v));
    return args[1];
}

/* (ffi:poke-i64 fp value &optional offset) → value (alias of poke-u64 bits) */
static CL_Obj bi_ffi_poke_i64(CL_Obj *args, int nargs)
{
    uint64_t v = cl_ffi_obj_to_u64(args[1]);
    memcpy(ffi_resolve_at(args, nargs, 2), &v, sizeof(v));
    return args[1];
}

/* (ffi:poke-single fp value &optional offset) → value */
static CL_Obj bi_ffi_poke_single(CL_Obj *args, int nargs)
{
    float v;
    if (!CL_REALP(args[1]))
        cl_error(CL_ERR_TYPE, "FFI:POKE-SINGLE: value must be a real number");
    v = cl_to_float(args[1]);
    memcpy(ffi_resolve_at(args, nargs, 2), &v, sizeof(v));
    return args[1];
}

/* (ffi:poke-double fp value &optional offset) → value */
static CL_Obj bi_ffi_poke_double(CL_Obj *args, int nargs)
{
    double v;
    if (!CL_REALP(args[1]))
        cl_error(CL_ERR_TYPE, "FFI:POKE-DOUBLE: value must be a real number");
    v = cl_to_double(args[1]);
    memcpy(ffi_resolve_at(args, nargs, 2), &v, sizeof(v));
    return args[1];
}

/* (ffi:poke-pointer fp value &optional offset) → value
 * VALUE is a foreign pointer; its real address is written to memory. */
static CL_Obj bi_ffi_poke_pointer(CL_Obj *args, int nargs)
{
    void *v;
    if (!CL_FOREIGN_POINTER_P(args[1]))
        cl_error(CL_ERR_TYPE, "FFI:POKE-POINTER: value must be a foreign pointer");
    v = platform_ffi_resolve(((CL_ForeignPtr *)CL_OBJ_TO_PTR(args[1]))->address);
    memcpy(ffi_resolve_at(args, nargs, 2), &v, sizeof(v));
    return args[1];
}

/* ================================================================
 * FFI stubs — binding descriptors that are functions (types.h CL_FfiStub)
 *
 * cl_ffi_stub_call is the runtime entry every call path lands on when the
 * callee is a stub (cl_vm_apply, OP_CALL/OP_TAILCALL, OP_APPLY, FUNCALL,
 * APPLY): arity check, then either the AmigaOS library call through the
 * stub's base variable (CL_STUB_LIBCALL — cl_amiga_call_via_base_sym,
 * the same helper OP_AMIGA_CALL uses) or a typed peek/poke at
 * ptr + offset (the field kinds FFI:DEFCSTRUCT installs).  The field
 * accessors reproduce the peek/poke builtins' conversions exactly (the
 * old DEFCSTRUCT expansion called those builtins), see stub_peek /
 * stub_poke.
 *
 * GC: a stub's fields are copied into C locals on entry; nothing below
 * touches the stub object after an allocation.  ARGS are the caller's
 * GC-rooted VM stack slots.
 * ================================================================ */

/* FFI::%DEFINE-CSTRUCT-ACCESSORS — the symbol compile_call matches to
 * run the compile-time DEFSETF registration (see compiler.h). */
CL_Obj cl_ffi_define_cstruct_accessors_sym = CL_NIL;

static const char *stub_display_name(CL_Obj name)
{
    return CL_SYMBOL_P(name) ? cl_symbol_name(name) : "anonymous FFI stub";
}

int cl_ffi_stub_arity(CL_Obj stub)
{
    CL_FfiStub *fs = (CL_FfiStub *)CL_OBJ_TO_PTR(stub);
    switch (fs->kind) {
    case CL_STUB_LIBCALL:   return (int)fs->ctype;
    case CL_STUB_PEEK:      return 1;
    case CL_STUB_POKE:      return 2;
    case CL_STUB_PEEK_IDX:  return 2;
    case CL_STUB_POKE_IDX:  return 3;
    case CL_STUB_FIELD_PTR: return 1;
    default:                return 0;
    }
}

/* Resolve the foreign-pointer argument of a field accessor to a real
 * address, + OFFSET.  Same two checks as ffi_resolve_at (type, then a
 * live non-null pointer), naming the accessor in the message. */
static uint8_t *stub_field_addr(CL_Obj fp_obj, uint32_t offset, CL_Obj name)
{
    void *base;
    if (!CL_FOREIGN_POINTER_P(fp_obj))
        cl_error(CL_ERR_TYPE,
                 "%s: argument must be a foreign pointer, got %s",
                 stub_display_name(name), cl_type_name(fp_obj));
    base = platform_ffi_resolve(((CL_ForeignPtr *)CL_OBJ_TO_PTR(fp_obj))->address);
    if (!base)
        cl_error(CL_ERR_GENERAL,
                 "%s: dereference of an invalid/null foreign pointer",
                 stub_display_name(name));
    return (uint8_t *)base + offset;
}

/* Index argument of an (:array T N) accessor: a fixnum in [0, count)
 * when the struct declared a count (count 0 = unchecked, the C way). */
static uint32_t stub_index_arg(CL_Obj idx, uint32_t count, CL_Obj name)
{
    if (!CL_FIXNUM_P(idx) || CL_FIXNUM_VAL(idx) < 0)
        cl_error(CL_ERR_TYPE,
                 "%s: index must be a non-negative fixnum, got %s",
                 stub_display_name(name), cl_type_name(idx));
    if (count != 0 && (uint32_t)CL_FIXNUM_VAL(idx) >= count)
        cl_error(CL_ERR_ARGS,
                 "%s: index %d is out of range for a %u-element array field",
                 stub_display_name(name), (int)CL_FIXNUM_VAL(idx),
                 (unsigned)count);
    return (uint32_t)CL_FIXNUM_VAL(idx);
}

/* Read a field of C type CTYPE at P and box it — the peek-* builtins'
 * conversions: u32/:pointer as an unsigned integer (bignum above the
 * fixnum range), i32 sign-extended, :fptr as a foreign pointer (NIL for
 * NULL), floats boxed. */
static CL_Obj stub_peek(uint8_t *p, int ctype)
{
    switch (ctype) {
    case CL_STUB_CT_U8:
        return CL_MAKE_FIXNUM((int32_t)*p);
    case CL_STUB_CT_I8:
        return CL_MAKE_FIXNUM((int32_t)*(int8_t *)p);
    case CL_STUB_CT_U16: {
        uint16_t v; memcpy(&v, p, sizeof(v));
        return CL_MAKE_FIXNUM((int32_t)v);
    }
    case CL_STUB_CT_I16: {
        int16_t v; memcpy(&v, p, sizeof(v));
        return CL_MAKE_FIXNUM((int32_t)v);
    }
    case CL_STUB_CT_U32:
    case CL_STUB_CT_POINTER: {
        uint32_t v; memcpy(&v, p, sizeof(v));
        return cl_amiga_box_result(v, CL_AMIGA_RES_UNSIGNED);
    }
    case CL_STUB_CT_I32: {
        uint32_t v; memcpy(&v, p, sizeof(v));
        return cl_amiga_box_result(v, CL_AMIGA_RES_SIGNED);
    }
    case CL_STUB_CT_FPTR: {
        /* 32-bit address in the struct (AmigaOS layout); NULL -> NIL.
         * Registered like FFI:MAKE-FOREIGN-POINTER does so the host's
         * handle table knows it. */
        uint32_t v; memcpy(&v, p, sizeof(v));
        if (v == 0) return CL_NIL;
        return cl_make_foreign_pointer(
            platform_ffi_register((void *)(uintptr_t)v), 0, 0);
    }
    case CL_STUB_CT_SINGLE: {
        float f; memcpy(&f, p, sizeof(f));
        return cl_make_single_float(f);
    }
    case CL_STUB_CT_DOUBLE: {
        double d; memcpy(&d, p, sizeof(d));
        return cl_make_double_float(d);
    }
    default:
        cl_error(CL_ERR_GENERAL, "FFI stub: corrupt field type code %d", ctype);
        return CL_NIL;
    }
}

/* Integer field value -> low 32 bits (fixnum or bignum, truncated to the
 * field width by the caller — the poke-* builtins truncate the same way). */
static uint32_t stub_int_arg(CL_Obj val, CL_Obj name)
{
    if (!CL_FIXNUM_P(val) && !CL_BIGNUM_P(val))
        cl_error(CL_ERR_TYPE, "%s: value must be an integer, got %s",
                 stub_display_name(name), cl_type_name(val));
    return (uint32_t)cl_ffi_obj_to_u64(val);
}

/* Store VAL into the CTYPE field at P (FFI:DEFCSTRUCT setter semantics:
 * :fptr takes a foreign pointer, an integer address or NIL). */
static void stub_poke(uint8_t *p, int ctype, CL_Obj val, CL_Obj name)
{
    switch (ctype) {
    case CL_STUB_CT_U8:
    case CL_STUB_CT_I8: {
        uint8_t v = (uint8_t)stub_int_arg(val, name);
        *p = v;
        break;
    }
    case CL_STUB_CT_U16:
    case CL_STUB_CT_I16: {
        uint16_t v = (uint16_t)stub_int_arg(val, name);
        memcpy(p, &v, sizeof(v));
        break;
    }
    case CL_STUB_CT_U32:
    case CL_STUB_CT_I32:
    case CL_STUB_CT_POINTER: {
        uint32_t v = stub_int_arg(val, name);
        memcpy(p, &v, sizeof(v));
        break;
    }
    case CL_STUB_CT_FPTR: {
        uint32_t v;
        if (CL_NULL_P(val))
            v = 0;
        else if (CL_FOREIGN_POINTER_P(val))
            /* the REAL address, as FFI:FOREIGN-POINTER-ADDRESS reports it */
            v = (uint32_t)(uintptr_t)platform_ffi_resolve(
                    ((CL_ForeignPtr *)CL_OBJ_TO_PTR(val))->address);
        else if (CL_FIXNUM_P(val) || CL_BIGNUM_P(val))
            v = (uint32_t)cl_ffi_obj_to_u64(val);
        else
            cl_error(CL_ERR_TYPE,
                     "%s: pointer field value must be a foreign pointer, "
                     "an integer address or NIL, got %s",
                     stub_display_name(name), cl_type_name(val));
        memcpy(p, &v, sizeof(v));
        break;
    }
    case CL_STUB_CT_SINGLE: {
        float f;
        if (!CL_REALP(val))
            cl_error(CL_ERR_TYPE, "%s: value must be a real number, got %s",
                     stub_display_name(name), cl_type_name(val));
        f = cl_to_float(val);
        memcpy(p, &f, sizeof(f));
        break;
    }
    case CL_STUB_CT_DOUBLE: {
        double d;
        if (!CL_REALP(val))
            cl_error(CL_ERR_TYPE, "%s: value must be a real number, got %s",
                     stub_display_name(name), cl_type_name(val));
        d = cl_to_double(val);
        memcpy(p, &d, sizeof(d));
        break;
    }
    default:
        cl_error(CL_ERR_GENERAL, "FFI stub: corrupt field type code %d", ctype);
    }
}

CL_Obj cl_ffi_stub_call(CL_Obj stub, CL_Obj *args, int nargs)
{
    CL_FfiStub *fs = (CL_FfiStub *)CL_OBJ_TO_PTR(stub);
    CL_Obj name = fs->name, aux = fs->aux;
    uint32_t a = fs->a;
    int16_t b = fs->b;
    uint8_t kind = fs->kind, ctype = fs->ctype;
    int expected = cl_ffi_stub_arity(stub);

    if (nargs != expected)
        cl_error(CL_ERR_ARGS, "Too %s arguments to %s: expected %d, got %d",
                 nargs < expected ? "few" : "many",
                 stub_display_name(name), expected, nargs);

    switch (kind) {
    case CL_STUB_LIBCALL:
        return cl_amiga_call_via_base_sym(aux, b, a, nargs, args);

    case CL_STUB_PEEK:
        return stub_peek(stub_field_addr(args[0], a, name), ctype);

    case CL_STUB_POKE:
        stub_poke(stub_field_addr(args[0], a, name), ctype, args[1], name);
        return args[1];

    case CL_STUB_PEEK_IDX: {
        /* a = offset (low 16) | element count (high 16), b = element size */
        uint32_t i = stub_index_arg(args[1], a >> 16, name);
        return stub_peek(stub_field_addr(args[0], (a & 0xFFFFu) + i * (uint32_t)b, name),
                         ctype);
    }
    case CL_STUB_POKE_IDX: {
        uint32_t i = stub_index_arg(args[1], a >> 16, name);
        stub_poke(stub_field_addr(args[0], (a & 0xFFFFu) + i * (uint32_t)b, name),
                  ctype, args[2], name);
        return args[2];
    }
    case CL_STUB_FIELD_PTR: {
        /* Embedded struct: a foreign pointer to ptr + offset (unowned),
         * registered like FFI:POINTER+ does. */
        uint8_t *p = stub_field_addr(args[0], a, name);
        return cl_make_foreign_pointer(platform_ffi_register(p), 0, 0);
    }
    default:
        cl_error(CL_ERR_GENERAL, "%s: corrupt FFI stub kind %d",
                 stub_display_name(name), (int)kind);
        return CL_NIL;
    }
}

/* --- Constructors and introspection (the Lisp-visible surface) --- */

static const char *const stub_kind_names[CL_STUB_KIND_MAX + 1] = {
    "LIBCALL", "PEEK", "POKE", "PEEK-IDX", "POKE-IDX", "FIELD-PTR" };
static const char *const stub_ct_names[CL_STUB_CT_MAX + 1] = {
    "U8", "I8", "U16", "I16", "U32", "I32", "POINTER", "FPTR", "SINGLE", "DOUBLE" };
static const char *const stub_res_names[CL_AMIGA_RES_KIND_MAX + 1] = {
    "UNSIGNED", "VOID", "POINTER", "SIGNED", "BOOL", "U16", "I16", "U8", "I8" };

/* Match a keyword argument against a name table; -1 if not a keyword or
 * not in the table. */
static int stub_keyword_index(CL_Obj kw, const char *const *names, int n)
{
    int i;
    if (!CL_SYMBOL_P(kw) ||
        ((CL_Symbol *)CL_OBJ_TO_PTR(kw))->package != cl_package_keyword)
        return -1;
    for (i = 0; i < n; i++)
        if (strcmp(cl_symbol_name(kw), names[i]) == 0)
            return i;
    return -1;
}

/* Byte size of a field element C type (the *-IDX kinds' stride). */
static int32_t stub_ct_size(int ctype)
{
    switch (ctype) {
    case CL_STUB_CT_U8: case CL_STUB_CT_I8:   return 1;
    case CL_STUB_CT_U16: case CL_STUB_CT_I16: return 2;
    case CL_STUB_CT_DOUBLE:                   return 8;
    default:                                  return 4;
    }
}

/* The keyword <-> code tables, shared with the binding-table packer
 * (bindtab.c) so DEFINE-BINDING-TABLE rows spell field types and result
 * kinds exactly as DEFCSTRUCT / DEFCFUN do. */
int cl_ffi_ctype_from_keyword(CL_Obj kw)
{
    return stub_keyword_index(kw, stub_ct_names, CL_STUB_CT_MAX + 1);
}
int cl_ffi_res_kind_from_keyword(CL_Obj kw)
{
    return stub_keyword_index(kw, stub_res_names, CL_AMIGA_RES_KIND_MAX + 1);
}
const char *cl_ffi_ctype_name(int ctype)
{
    return (ctype >= 0 && ctype <= CL_STUB_CT_MAX) ? stub_ct_names[ctype] : "?";
}
const char *cl_ffi_res_kind_name(int kind)
{
    return (kind >= 0 && kind <= CL_AMIGA_RES_KIND_MAX) ? stub_res_names[kind] : "?";
}
int32_t cl_ffi_ctype_size(int ctype)
{
    return stub_ct_size(ctype);
}

/* Validate and build one field stub.  WHO names the caller in errors;
 * KIND/CTYPE are decoded codes, COUNT the declared element count of an
 * array field (0 = unchecked). */
static CL_Obj make_field_stub_checked(const char *who, CL_Obj name, int kind,
                                      int ctype, int32_t offset,
                                      int32_t elt_size, int32_t count)
{
    const char *nm = cl_symbol_name(name);
    uint32_t a;

    if (offset < 0)
        cl_error(CL_ERR_ARGS, "%s: %s: offset must be a non-negative fixnum",
                 who, nm);
    a = (uint32_t)offset;
    if (kind == CL_STUB_PEEK_IDX || kind == CL_STUB_POKE_IDX) {
        if (elt_size != 1 && elt_size != 2 && elt_size != 4 && elt_size != 8)
            cl_error(CL_ERR_ARGS,
                     "%s: %s: array element size must be 1, 2, 4 or 8", who, nm);
        if (count < 0 || count > 0xFFFF)
            cl_error(CL_ERR_ARGS, "%s: %s: element count must be 0..65535",
                     who, nm);
        if (offset > 0xFFFF)
            cl_error(CL_ERR_ARGS,
                     "%s: %s: array field offset must be <= 65535", who, nm);
        a = (uint32_t)offset | ((uint32_t)count << 16);
    } else {
        elt_size = 0;
    }
    return cl_make_ffi_stub((uint8_t)kind, name, CL_NIL, a,
                            (int16_t)elt_size, (uint8_t)ctype);
}

/* (ffi::%make-field-stub name kind ctype offset &optional elt-size count)
 *   → FFI stub
 *
 * A field accessor descriptor (what FFI:DEFCSTRUCT installs, see
 * %DEFINE-CSTRUCT-ACCESSORS below for the bulk form it actually uses).
 * KIND is :peek / :poke / :peek-idx / :poke-idx / :field-ptr; CTYPE one of
 * :u8 :i8 :u16 :i16 :u32 :i32 :pointer :fptr :single :double (NIL for
 * :field-ptr); OFFSET the field's byte offset.  The *-idx kinds take the
 * element size (1/2/4/8) and, optionally, the declared element COUNT,
 * which the accessor then bounds-checks (0 = unchecked). */
static CL_Obj bi_ffi_make_field_stub(CL_Obj *args, int nargs)
{
    CL_Obj name = args[0];
    int kind, ctype = 0;
    int32_t elt_size = 0, count = 0;
    const char *nm;

    if (!CL_SYMBOL_P(name))
        cl_error(CL_ERR_TYPE, "%%MAKE-FIELD-STUB: NAME must be a symbol");
    nm = cl_symbol_name(name);
    kind = stub_keyword_index(args[1], stub_kind_names, CL_STUB_KIND_MAX + 1);
    if (kind < 0 || kind == CL_STUB_LIBCALL)
        cl_error(CL_ERR_ARGS,
                 "%%MAKE-FIELD-STUB: %s: kind must be :PEEK, :POKE, "
                 ":PEEK-IDX, :POKE-IDX or :FIELD-PTR", nm);
    if (kind != CL_STUB_FIELD_PTR) {
        ctype = stub_keyword_index(args[2], stub_ct_names, CL_STUB_CT_MAX + 1);
        if (ctype < 0)
            cl_error(CL_ERR_ARGS,
                     "%%MAKE-FIELD-STUB: %s: unknown field type %s (expected "
                     ":U8 :I8 :U16 :I16 :U32 :I32 :POINTER :FPTR :SINGLE :DOUBLE)",
                     nm, CL_SYMBOL_P(args[2]) ? cl_symbol_name(args[2]) : "?");
    }
    if (!CL_FIXNUM_P(args[3]))
        cl_error(CL_ERR_ARGS,
                 "%%MAKE-FIELD-STUB: %s: offset must be a non-negative fixnum", nm);
    if (kind == CL_STUB_PEEK_IDX || kind == CL_STUB_POKE_IDX) {
        if (nargs < 5 || !CL_FIXNUM_P(args[4]))
            cl_error(CL_ERR_ARGS,
                     "%%MAKE-FIELD-STUB: %s: array element size must be 1, 2, 4 or 8",
                     nm);
        elt_size = CL_FIXNUM_VAL(args[4]);
        if (nargs > 5 && !CL_NULL_P(args[5])) {
            if (!CL_FIXNUM_P(args[5]))
                cl_error(CL_ERR_ARGS,
                         "%%MAKE-FIELD-STUB: %s: element count must be 0..65535", nm);
            count = CL_FIXNUM_VAL(args[5]);
        }
    }
    return make_field_stub_checked("%MAKE-FIELD-STUB", name, kind, ctype,
                                   CL_FIXNUM_VAL(args[3]), elt_size, count);
}

/* The writer symbol of an accessor: %SET-<ACCESSOR>, interned in the
 * accessor's package (the current package for an uninterned accessor).
 * Allocates (intern) — callers keep ACC rooted. */
static CL_Obj cstruct_setter_symbol(CL_Obj acc)
{
    CL_String *an = (CL_String *)CL_OBJ_TO_PTR(((CL_Symbol *)CL_OBJ_TO_PTR(acc))->name);
    CL_Obj pkg = ((CL_Symbol *)CL_OBJ_TO_PTR(acc))->package;
    char sname[160];
    if (an->length + 5 >= sizeof(sname))
        cl_error(CL_ERR_ARGS, "DEFCSTRUCT: %s: accessor name too long",
                 cl_symbol_name(acc));
    memcpy(sname, "%SET-", 5);
    memcpy(sname + 5, an->data, an->length);
    if (CL_NULL_P(pkg)) pkg = cl_current_package;
    return cl_intern_in(sname, an->length + 5, pkg);
}

/* Compile-time half of %DEFINE-CSTRUCT-ACCESSORS (called by compile_call
 * when it compiles a call whose argument is a quoted entry list): register
 * every ACCESSOR -> %SET-ACCESSOR DEFSETF pair now, without installing
 * anything.  The per-field DEFSETF forms this installer replaced had
 * exactly this immediate side effect wherever they were compiled — top
 * level or inside a PROGN/LET/DEFUN body — and code relies on it:
 * (progn (defcstruct pt (x :u16 0)) (setf (pt-x p) 1)) in ONE form must
 * find PT-X's updater while the SETF is compiled, before the installer
 * has run.  Lenient: a malformed entry is skipped here and reported by
 * the runtime call.  Allocates (intern, cons) — rooted cursor. */
void cl_ffi_cstruct_register_setfs(CL_Obj entries)
{
    CL_Obj list = entries, acc = CL_NIL, setter = CL_NIL, kw_struct;
    kw_struct = cl_intern_keyword("STRUCT", 6);
    CL_GC_PROTECT(list);
    CL_GC_PROTECT(acc);
    CL_GC_PROTECT(setter);
    CL_GC_PROTECT(kw_struct);
    while (CL_CONS_P(list)) {
        CL_Obj entry = cl_car(list);
        CL_Obj type;
        list = cl_cdr(list);
        if (!CL_CONS_P(entry) || !CL_CONS_P(cl_cdr(entry))) continue;
        acc = cl_car(entry);
        type = cl_car(cl_cdr(entry));
        if (!CL_SYMBOL_P(acc)) continue;
        if (CL_CONS_P(type) && cl_car(type) == kw_struct) continue;  /* no setter */
        setter = cstruct_setter_symbol(acc);
        cl_register_setf_updater(acc, setter);
    }
    CL_GC_UNPROTECT(4);
}

/* (ffi::%define-cstruct-accessors entries) → T
 *
 * The bulk installer behind FFI:DEFCSTRUCT: ENTRIES is a list of
 * (ACCESSOR TYPE OFFSET) with TYPE a field type keyword, (:struct N) or
 * (:array ELT-TYPE COUNT).  For each entry the accessor's function cell
 * gets a reader stub; unless the field is an embedded struct, the setter
 * %SET-<ACCESSOR> is interned in the accessor's package, gets a writer
 * stub, and is registered as the accessor's DEFSETF updater — exactly
 * what one (setf symbol-function) + DEFSETF pair per field did before.
 *
 * Why one call per struct instead of a form per accessor: a COMPILE-FILE
 * top-level form is a FASL unit of ~200 bytes of fixed overhead plus
 * every symbol it names in full, so three forms per field made a 480
 * field module (intuition) cost ~270 KB of FASL and ~1400 units to
 * deserialize on a 68020.  The entry list is ONE constant (symbols and
 * keywords shared inside it), ~50 bytes per field.
 *
 * GC: the list is walked with a rooted cursor; every CL_Obj that lives
 * across the allocations (stub, intern) is rooted. */
static CL_Obj bi_ffi_define_cstruct_accessors(CL_Obj *args, int nargs)
{
    static const char who[] = "DEFCSTRUCT";
    CL_Obj list = args[0];
    CL_Obj acc = CL_NIL, type = CL_NIL, setter = CL_NIL, stub = CL_NIL;
    CL_Obj kw_struct, kw_array;
    (void)nargs;

    kw_struct = cl_intern_keyword("STRUCT", 6);
    kw_array = cl_intern_keyword("ARRAY", 5);
    CL_GC_PROTECT(list);
    CL_GC_PROTECT(acc);
    CL_GC_PROTECT(type);
    CL_GC_PROTECT(setter);
    CL_GC_PROTECT(stub);
    CL_GC_PROTECT(kw_struct);
    CL_GC_PROTECT(kw_array);

    while (!CL_NULL_P(list)) {
        CL_Obj entry = cl_car(list);
        CL_Obj offset_obj;
        int32_t offset;
        int ctype = 0;
        const char *nm;

        if (!CL_CONS_P(entry) || !CL_CONS_P(cl_cdr(entry)) ||
            !CL_CONS_P(cl_cdr(cl_cdr(entry))))
            cl_error(CL_ERR_ARGS,
                     "%s: malformed field entry (expected (ACCESSOR TYPE OFFSET))",
                     who);
        acc = cl_car(entry);
        type = cl_car(cl_cdr(entry));
        offset_obj = cl_car(cl_cdr(cl_cdr(entry)));
        if (!CL_SYMBOL_P(acc))
            cl_error(CL_ERR_TYPE, "%s: accessor name must be a symbol", who);
        nm = cl_symbol_name(acc);
        if (!CL_FIXNUM_P(offset_obj))
            cl_error(CL_ERR_ARGS, "%s: %s: offset must be a fixnum", who, nm);
        offset = CL_FIXNUM_VAL(offset_obj);

        if (CL_CONS_P(type) && cl_car(type) == kw_struct) {
            /* Embedded struct: reader only, returns ptr + offset. */
            stub = make_field_stub_checked(who, acc, CL_STUB_FIELD_PTR, 0,
                                           offset, 0, 0);
            cl_symbol_set_function(acc, stub);
        } else {
            int is_array = CL_CONS_P(type) && cl_car(type) == kw_array;
            CL_Obj elt_kw = is_array ? cl_car(cl_cdr(type)) : type;
            int32_t count = 0, elt_size;

            ctype = stub_keyword_index(elt_kw, stub_ct_names, CL_STUB_CT_MAX + 1);
            if (ctype < 0)
                cl_error(CL_ERR_ARGS,
                         "%s: %s: unknown field type %s (expected :U8 :I8 :U16 "
                         ":I16 :U32 :I32 :POINTER :FPTR :SINGLE :DOUBLE, "
                         "(:STRUCT n) or (:ARRAY type n))",
                         who, nm, CL_SYMBOL_P(elt_kw) ? cl_symbol_name(elt_kw) : "?");
            elt_size = stub_ct_size(ctype);
            if (is_array) {
                CL_Obj cnt = cl_car(cl_cdr(cl_cdr(type)));
                if (!CL_FIXNUM_P(cnt) || CL_FIXNUM_VAL(cnt) <= 0)
                    cl_error(CL_ERR_ARGS,
                             "%s: %s: array field needs a positive element count",
                             who, nm);
                count = CL_FIXNUM_VAL(cnt);
            }

            /* Reader */
            stub = make_field_stub_checked(who, acc,
                                           is_array ? CL_STUB_PEEK_IDX : CL_STUB_PEEK,
                                           ctype, offset, elt_size, count);
            cl_symbol_set_function(acc, stub);

            /* Writer: %SET-<accessor>, in the accessor's package (ACC is
             * rooted, so deriving the name after the reader stub's
             * allocation is safe). */
            setter = cstruct_setter_symbol(acc);
            stub = make_field_stub_checked(who, setter,
                                           is_array ? CL_STUB_POKE_IDX : CL_STUB_POKE,
                                           ctype, offset, elt_size, count);
            cl_symbol_set_function(setter, stub);
            cl_register_setf_updater(acc, setter);
        }
        list = cl_cdr(list);
    }
    CL_GC_UNPROTECT(7);
    return CL_T;
}

/* (ffi::%ffi-stub-info fn) → plist describing an FFI stub, or NIL when
 * FN is not one (a symbol is resolved through its function cell).
 *   (:kind :libcall :name N :base BASE-SYM :lvo -204 :regspec nibbles
 *    :result :pointer :nparams 1)
 *   (:kind :peek :name N :ctype :i16 :offset 8)      [+ :elt-size :count]
 * Used by DESCRIBE and the test suites; the fields are also what
 * EXT:FUNCTION-ARGLIST / DISASSEMBLE report. */
static CL_Obj stub_info_push(CL_Obj plist, const char *key, CL_Obj val)
{
    /* (key val . plist) built back to front; caller keeps plist rooted */
    CL_Obj k;
    CL_GC_PROTECT(val);
    plist = cl_cons(val, plist);
    CL_GC_UNPROTECT(1);
    CL_GC_PROTECT(plist);
    k = cl_intern_keyword(key, (uint32_t)strlen(key));
    CL_GC_UNPROTECT(1);
    return cl_cons(k, plist);
}

static CL_Obj stub_info_keyword(const char *name)
{
    return cl_intern_keyword(name, (uint32_t)strlen(name));
}

static CL_Obj bi_ffi_stub_info(CL_Obj *args, int nargs)
{
    CL_Obj fn = args[0];
    CL_Obj name, aux, plist = CL_NIL;
    uint32_t a;
    int16_t b;
    uint8_t kind, ctype;
    (void)nargs;

    if (CL_SYMBOL_P(fn))
        fn = ((CL_Symbol *)CL_OBJ_TO_PTR(fn))->function;
    if (!CL_FFI_STUB_P(fn))
        return CL_NIL;
    {
        CL_FfiStub *fs = (CL_FfiStub *)CL_OBJ_TO_PTR(fn);
        name = fs->name; aux = fs->aux; a = fs->a; b = fs->b;
        kind = fs->kind; ctype = fs->ctype;
    }
    if (kind > CL_STUB_KIND_MAX) return CL_NIL;

    CL_GC_PROTECT(name);
    CL_GC_PROTECT(aux);
    CL_GC_PROTECT(plist);
    /* built back to front so the plist reads (:kind ... :name ... ...).
     * Every keyword is interned into a local BEFORE the push: the intern
     * allocates, and `plist` passed in the same argument list would be
     * read before it — a stale offset once the collection inside moved
     * the list (found by the Immix stress regime, 2026-10-13; see
     * tests/test_gc_arg_order.sh for the rule). */
    if (kind == CL_STUB_LIBCALL) {
        int rk = CL_AMIGA_RES_KIND(a);
        CL_Obj kw;
        plist = stub_info_push(plist, "NPARAMS", CL_MAKE_FIXNUM((int32_t)ctype));
        kw = rk <= CL_AMIGA_RES_KIND_MAX
             ? stub_info_keyword(stub_res_names[rk]) : CL_NIL;
        plist = stub_info_push(plist, "RESULT", kw);
        plist = stub_info_push(plist, "REGSPEC",
                               CL_MAKE_FIXNUM((int32_t)CL_AMIGA_REGSPEC_NIBBLES(a)));
        plist = stub_info_push(plist, "LVO", CL_MAKE_FIXNUM((int32_t)b));
        plist = stub_info_push(plist, "BASE", aux);
    } else {
        if (kind == CL_STUB_PEEK_IDX || kind == CL_STUB_POKE_IDX) {
            plist = stub_info_push(plist, "COUNT", CL_MAKE_FIXNUM((int32_t)(a >> 16)));
            plist = stub_info_push(plist, "ELT-SIZE", CL_MAKE_FIXNUM((int32_t)b));
            plist = stub_info_push(plist, "OFFSET", CL_MAKE_FIXNUM((int32_t)(a & 0xFFFFu)));
        } else {
            plist = stub_info_push(plist, "OFFSET", CL_MAKE_FIXNUM((int32_t)a));
        }
        if (kind != CL_STUB_FIELD_PTR) {
            CL_Obj kw = ctype <= CL_STUB_CT_MAX
                        ? stub_info_keyword(stub_ct_names[ctype]) : CL_NIL;
            plist = stub_info_push(plist, "CTYPE", kw);
        }
    }
    plist = stub_info_push(plist, "NAME", name);
    {
        CL_Obj kw = stub_info_keyword(stub_kind_names[kind]);
        plist = stub_info_push(plist, "KIND", kw);
    }
    CL_GC_UNPROTECT(3);
    return plist;
}

/* ================================================================
 * Dynamic libraries + foreign function calls (host: dlopen + libffi)
 * ================================================================ */

/* Interned primitive type keywords, indexed by CLFFIType (filled in init).
 * CALL-FOREIGN accepts these; the CFFI backend maps its richer type set
 * down to them. */
static CL_Obj ffi_type_keywords[CL_FFI_POINTER + 1];

/* Copy a CL string (base or wide) into BUF as a NUL-terminated C string.
 * Library/symbol names are short ASCII; non-ASCII code points are truncated
 * to a byte.  Errors if ARG is not a string. */
static const char *ffi_string_to_cstr(CL_Obj arg, char *buf, uint32_t bufsize)
{
    uint32_t len, i, n;
    if (!CL_ANY_STRING_P(arg))
        cl_error(CL_ERR_TYPE, "FFI: expected a string");
    len = cl_string_length(arg);
    n = (len < bufsize - 1) ? len : bufsize - 1;
    for (i = 0; i < n; i++)
        buf[i] = (char)cl_string_char_at(arg, i);
    buf[n] = '\0';
    return buf;
}

/* Map a (presumed keyword) CL_Obj to a CLFFIType, or signal. */
static CLFFIType ffi_kw_to_type(CL_Obj kw)
{
    int i;
    for (i = 0; i <= (int)CL_FFI_POINTER; i++)
        if (ffi_type_keywords[i] == kw)
            return (CLFFIType)i;
    cl_error(CL_ERR_TYPE,
             "FFI:CALL-FOREIGN: unknown foreign type (expected one of :VOID "
             ":INT8 :UINT8 :INT16 :UINT16 :INT32 :UINT32 :INT64 :UINT64 "
             ":FLOAT :DOUBLE :POINTER)");
    return CL_FFI_VOID;
}

/* Extract the C value for one argument of type T from CL value V. */
static void ffi_marshal_arg(CLFFIType t, CL_Obj v, CLFFIValue *out)
{
    switch (t) {
    case CL_FFI_I8:   out->i8  = (int8_t)cl_ffi_obj_to_u64(v); break;
    case CL_FFI_U8:   out->u8  = (uint8_t)cl_ffi_obj_to_u64(v); break;
    case CL_FFI_I16:  out->i16 = (int16_t)cl_ffi_obj_to_u64(v); break;
    case CL_FFI_U16:  out->u16 = (uint16_t)cl_ffi_obj_to_u64(v); break;
    case CL_FFI_I32:  out->i32 = (int32_t)cl_ffi_obj_to_u64(v); break;
    case CL_FFI_U32:  out->u32 = (uint32_t)cl_ffi_obj_to_u64(v); break;
    case CL_FFI_I64:  out->i64 = (int64_t)cl_ffi_obj_to_u64(v); break;
    case CL_FFI_U64:  out->u64 = cl_ffi_obj_to_u64(v); break;
    case CL_FFI_FLOAT:
        if (!CL_REALP(v))
            cl_error(CL_ERR_TYPE, "FFI:CALL-FOREIGN: :FLOAT argument must be a real number");
        out->f = cl_to_float(v); break;
    case CL_FFI_DOUBLE:
        if (!CL_REALP(v))
            cl_error(CL_ERR_TYPE, "FFI:CALL-FOREIGN: :DOUBLE argument must be a real number");
        out->d = cl_to_double(v); break;
    case CL_FFI_POINTER:
        if (CL_NULL_P(v))
            out->p = NULL;
        else if (CL_FOREIGN_POINTER_P(v))
            out->p = platform_ffi_resolve(((CL_ForeignPtr *)CL_OBJ_TO_PTR(v))->address);
        else if (CL_INTEGER_P(v))
            out->p = (void *)(uintptr_t)cl_ffi_obj_to_u64(v);
        else
            cl_error(CL_ERR_TYPE, "FFI:CALL-FOREIGN: :POINTER argument must be a foreign pointer, integer, or NIL");
        break;
    case CL_FFI_VOID:
        cl_error(CL_ERR_TYPE, "FFI:CALL-FOREIGN: :VOID is not a valid argument type");
        break;
    }
}

/* Box a foreign return value of type T into a CL object. */
static CL_Obj ffi_box_result(CLFFIType t, CLFFIValue *r)
{
    switch (t) {
    case CL_FFI_VOID:    return CL_NIL;
    case CL_FFI_I8:      return CL_MAKE_FIXNUM((int32_t)r->i8);
    case CL_FFI_U8:      return CL_MAKE_FIXNUM((int32_t)r->u8);
    case CL_FFI_I16:     return CL_MAKE_FIXNUM((int32_t)r->i16);
    case CL_FFI_U16:     return CL_MAKE_FIXNUM((int32_t)r->u16);
    case CL_FFI_I32:     return cl_ffi_i64_to_obj((int64_t)r->i32);
    case CL_FFI_U32:     return cl_ffi_u64_to_obj((uint64_t)r->u32);
    case CL_FFI_I64:     return cl_ffi_i64_to_obj(r->i64);
    case CL_FFI_U64:     return cl_ffi_u64_to_obj(r->u64);
    case CL_FFI_FLOAT:   return cl_make_single_float(r->f);
    case CL_FFI_DOUBLE:  return cl_make_double_float(r->d);
    case CL_FFI_POINTER: return cl_make_foreign_pointer(platform_ffi_register(r->p), 0, 0);
    }
    return CL_NIL;
}

/* (ffi:load-library name) → foreign-pointer (library handle) or NIL.
 * NAME may be NIL to obtain a handle to the global/default symbol namespace. */
static CL_Obj bi_ffi_load_library(CL_Obj *args, int nargs)
{
    char buf[1024];
    const char *name = NULL;
    uint32_t h;
    (void)nargs;
    if (!CL_NULL_P(args[0]))
        name = ffi_string_to_cstr(args[0], buf, sizeof(buf));
    h = platform_ffi_dlopen(name);
    if (h == 0) return CL_NIL;
    return cl_make_foreign_pointer(h, 0, 0);
}

/* (ffi:close-library lib) → T */
static CL_Obj bi_ffi_close_library(CL_Obj *args, int nargs)
{
    (void)nargs;
    if (!CL_FOREIGN_POINTER_P(args[0]))
        cl_error(CL_ERR_TYPE, "FFI:CLOSE-LIBRARY: argument must be a library handle");
    platform_ffi_dlclose(((CL_ForeignPtr *)CL_OBJ_TO_PTR(args[0]))->address);
    return CL_T;
}

/* (ffi:symbol-pointer name &optional lib) → foreign-pointer or NIL.
 * LIB is a library handle from LOAD-LIBRARY; NIL searches the default
 * namespace. */
static CL_Obj bi_ffi_symbol_pointer(CL_Obj *args, int nargs)
{
    char buf[256];
    const char *name;
    uint32_t libh = 0, h;
    if (nargs > 1 && !CL_NULL_P(args[1])) {
        if (!CL_FOREIGN_POINTER_P(args[1]))
            cl_error(CL_ERR_TYPE, "FFI:SYMBOL-POINTER: LIB must be a library handle or NIL");
        libh = ((CL_ForeignPtr *)CL_OBJ_TO_PTR(args[1]))->address;
    }
    name = ffi_string_to_cstr(args[0], buf, sizeof(buf));
    h = platform_ffi_dlsym(libh, name);
    if (h == 0) return CL_NIL;
    return cl_make_foreign_pointer(h, 0, 0);
}

/* (ffi:call-foreign fn-ptr ret-type arg-types arg-values &optional n-fixed)
 *   FN-PTR     foreign pointer to the C function
 *   RET-TYPE   one primitive type keyword (or :VOID)
 *   ARG-TYPES  list of primitive type keywords
 *   ARG-VALUES list of CL values, parallel to ARG-TYPES
 *   N-FIXED    for variadic calls, the count of fixed (non-vararg) args;
 *              defaults to all args (a non-variadic call)
 * Returns the boxed C result (NIL for :VOID). */
static CL_Obj bi_ffi_call_foreign(CL_Obj *args, int nargs)
{
    void *fn;
    CLFFIType ret_type;
    CLFFIType atypes[CL_FFI_MAX_ARGS];
    CLFFIValue avals[CL_FFI_MAX_ARGS];
    CLFFIValue rv;
    int n = 0, nfixed;
    CL_Obj tlist, vlist;

    if (!CL_FOREIGN_POINTER_P(args[0]))
        cl_error(CL_ERR_TYPE, "FFI:CALL-FOREIGN: first argument must be a foreign pointer (function address)");
    fn = platform_ffi_resolve(((CL_ForeignPtr *)CL_OBJ_TO_PTR(args[0]))->address);
    if (!fn)
        cl_error(CL_ERR_GENERAL, "FFI:CALL-FOREIGN: null or invalid function pointer");
    ret_type = ffi_kw_to_type(args[1]);

    tlist = args[2];
    vlist = args[3];
    while (!CL_NULL_P(tlist) && !CL_NULL_P(vlist)) {
        if (n >= CL_FFI_MAX_ARGS)
            cl_error(CL_ERR_ARGS, "FFI:CALL-FOREIGN: too many arguments (max %d)", CL_FFI_MAX_ARGS);
        atypes[n] = ffi_kw_to_type(cl_car(tlist));
        ffi_marshal_arg(atypes[n], cl_car(vlist), &avals[n]);
        n++;
        tlist = cl_cdr(tlist);
        vlist = cl_cdr(vlist);
    }
    if (!CL_NULL_P(tlist) || !CL_NULL_P(vlist))
        cl_error(CL_ERR_ARGS, "FFI:CALL-FOREIGN: argument type/value count mismatch");

    nfixed = (nargs > 4 && !CL_NULL_P(args[4])) ? (int)CL_FIXNUM_VAL(args[4]) : n;

    if (platform_ffi_call(fn, ret_type, &rv, n, nfixed, atypes, avals) != 0)
        cl_error(CL_ERR_GENERAL,
                 "FFI:CALL-FOREIGN: foreign call failed (FFI calls are not supported on this platform)");
    cl_ffi_deferred_error_check();   /* a callback may have parked an error */

    return ffi_box_result(ret_type, &rv);
}

/* ================================================================
 * Callbacks — Lisp functions exposed as C function pointers
 *
 * Each live callback occupies a fixed slot whose LISP_FN field is a
 * registered GC root (the closure is C-allocated outside the arena, so the
 * Lisp function it references would otherwise be invisible to GC).  The
 * CFFI backend creates one closure per callback NAME and dispatches through
 * an indirection, so this fixed table is ample.
 * ================================================================ */

#define CL_FFI_MAX_CALLBACKS 64

typedef struct {
    int       in_use;
    CLFFIType ret_type;
    CLFFIType arg_types[CL_FFI_MAX_ARGS];
    int8_t    arg_regs[CL_FFI_MAX_ARGS];   /* CL_FFI_REG_STACK or a 68k register (platform.h) */
    int       nargs;
    void     *plat_closure;
    uint32_t  code_handle; /* side-table handle from platform_ffi_register(code) */
    CL_Obj    lisp_fn;     /* GC root — registered in init */
    /* A method-filtered BOOPSI dispatcher (%MAKE-METHOD-DISPATCHER): only
     * the MethodIDs listed here enter Lisp, every other message goes to
     * the superclass's dispatcher without leaving C.  filtered = 0 for an
     * ordinary callback. */
    int       filtered;
    uint32_t  n_methods;
    uint32_t *methods;     /* platform_alloc'd, NULL when n_methods == 0 */
    volatile uint32_t calls, passed;   /* %METHOD-DISPATCHER-STATS */
} FFICallback;

static FFICallback ffi_callbacks[CL_FFI_MAX_CALLBACKS];
/* Serializes slot claim/release: two MP threads scanning for a free slot
 * unlocked could claim the SAME slot, clobbering each other's lisp_fn /
 * plat_closure (foreign code then invokes the wrong Lisp function). */
static void *ffi_callback_lock = NULL;

/* EXT:*CALLBACK-ERROR-POLICY* — :DEFER (default) parks a condition that
 * escapes a callback until the foreign call returns; :DEBUG lets the
 * interactive debugger open on the foreign caller's stack (host only in
 * practice — on the target the OS is mid-dispatch under it). */
static CL_Obj SYM_CALLBACK_ERROR_POLICY = CL_NIL;
static CL_Obj kw_callback_debug = CL_NIL;

/* Calls of a callback from a task/thread that is not a Lisp thread (§10.3.2
 * of specs/mui-bindings.md: intuition invokes gadget-class methods from
 * input.device's task).  Counted here — nothing else is safe to do from
 * such a task — and reported by cl_ffi_deferred_error_check from the next
 * foreign call that returns on a Lisp thread. */
static volatile uint32_t ffi_foreign_task_calls = 0;
static uint32_t ffi_foreign_task_reported = 0;

/* (ext:%ffi-foreign-task-calls) — how many callback invocations arrived
 * from a task that is not a Lisp thread and were answered with 0 without
 * running Lisp.  The warning above goes to *DEBUG-IO*, which a detached
 * process on AmigaOS (`Run >log`) never shows; this counter is readable
 * from Lisp, e.g. over an ARexx port, so "the OS never calls my hook" and
 * "the OS calls my hook from input.device's task" can be told apart. */
static CL_Obj bi_ext_ffi_foreign_task_calls(CL_Obj *args, int n)
{
    CL_UNUSED(args); CL_UNUSED(n);
    return CL_MAKE_FIXNUM((int32_t)ffi_foreign_task_calls);
}

int cl_callback_debugger_allowed(void)
{
    if (CT->callback_depth == 0) return 1;
    return !CL_NULL_P(SYM_CALLBACK_ERROR_POLICY) &&
           cl_symbol_value(SYM_CALLBACK_ERROR_POLICY) == kw_callback_debug;
}

/* Invoked (via the platform trampoline) when foreign code calls a callback.
 * Marshals C args -> CL, applies the Lisp function, marshals the result back.
 *
 * This is the foreign-callback BOUNDARY (thread.h, "Foreign-callback
 * boundary"): the Lisp function runs on the foreign caller's stack, inside
 * its C frames, and nothing may longjmp across those — not an unhandled
 * error, not a THROW to a catch tag of the Lisp that made the foreign call,
 * not a restart of the REPL.  So for the dynamic extent of the call:
 *   - the NLX / handler / restart floors hide every frame established
 *     outside the callback (a THROW to one of them is a "no catch" error,
 *     a HANDLER-CASE around the foreign call does not see the condition
 *     until it is re-signaled below),
 *   - callback_depth keeps the debugger out (cl_invoke_debugger),
 *   - a CL_CATCH frame catches whatever unwinds: the VM stack is restored
 *     to what the foreign caller left, the condition is parked in the
 *     thread, the callback returns 0 / NULL.
 * cl_ffi_deferred_error_check, called by every foreign-call path once the
 * OS has returned, re-signals the parked condition on the caller's side of
 * the boundary — a HANDLER-CASE around (call-hook-pkt ...) or (do-method
 * ...) catches it there, the debugger shows it there. */
static void ffi_callback_handler(void *ud, const CLFFIValue *cargs, CLFFIValue *cret)
{
    FFICallback *cb = (FFICallback *)ud;
    CL_Obj clargs[CL_FFI_MAX_ARGS];
    CL_Obj result = CL_NIL;
    CL_Thread *t;
    int i, n = cb->nargs;
    int saved_sp, saved_fp, saved_nlx_top;
    int saved_nlx_floor, saved_handler_floor, saved_restart_floor;
    int err;

    if (cb->filtered) {
        /* (class, object, message): the MethodID is the message's first
         * longword.  A method the Lisp function did not ask for is the
         * superclass's -- DoSuperMethodA, no Lisp, no allocation, safe on
         * any task.  MUI sends an object dozens of methods per keystroke
         * (OM_SET, OM_GET, MUIM_Draw, MUIM_DrawBackground ...) of which a
         * class typically handles two or three. */
        const uint8_t *msg = (const uint8_t *)cargs[2].p;
        uint32_t id = 0, k;
        cb->calls++;
        if (msg != NULL) memcpy(&id, msg, 4);
        for (k = 0; k < cb->n_methods; k++)
            if (cb->methods[k] == id) break;
        if (msg == NULL || k == cb->n_methods) {
            cb->passed++;
            cret->u32 = platform_amiga_do_super_method(cargs[0].p, cargs[1].p,
                                                       cargs[2].p);
            return;
        }
    }

    if (!cl_thread_current_is_registered()) {
        /* No VM, no Lisp state, a stack of unknown size: the only safe
         * answer is the callback's zero result (cret is pre-zeroed by
         * every platform trampoline). */
        ffi_foreign_task_calls++;
        return;
    }
    t = CT;

    saved_sp = cl_vm.sp;
    saved_fp = cl_vm.fp;
    saved_nlx_top = cl_nlx_top;
    saved_nlx_floor = t->nlx_floor;
    saved_handler_floor = cl_handler_floor;
    saved_restart_floor = cl_restart_floor;
    t->nlx_floor = cl_nlx_top;
    cl_handler_floor = cl_handler_top;
    cl_restart_floor = cl_restart_top;
    t->callback_depth++;

    for (i = 0; i < n; i++) clargs[i] = CL_NIL;
    /* Protect every arg slot BEFORE any boxing allocates — a later box may
     * trigger GC that would otherwise strand earlier (already-boxed) args.
     * Pushed before CL_CATCH so the frame's root snapshot includes them and
     * the single CL_GC_UNPROTECT below balances both paths. */
    for (i = 0; i < n; i++) CL_GC_PROTECT(clargs[i]);
    CL_GC_PROTECT(result);

    CL_CATCH(err);
    if (err == CL_ERR_NONE) {
        for (i = 0; i < n; i++)
            clargs[i] = ffi_box_result(cb->arg_types[i], (CLFFIValue *)&cargs[i]);
        result = cl_vm_apply(cb->lisp_fn, clargs, n);
        if (cb->ret_type != CL_FFI_VOID)
            ffi_marshal_arg(cb->ret_type, result, cret);
        CL_UNCATCH();
    } else {
        CL_UNCATCH();
        /* The frame restore already dropped the roots, dynamic bindings,
         * handlers and restarts established inside; the VM stub frame
         * cl_vm_apply pushed and the NLX frames of the abandoned Lisp are
         * ours to drop. */
        cl_vm.sp = saved_sp;
        cl_vm.fp = saved_fp;
        cl_nlx_top = saved_nlx_top;
        if (t->callback_error_code == 0) {   /* the first escape wins */
            t->callback_error_code = err;
            t->callback_error = (err == CL_ERR_EXIT) ? CL_NIL : t->last_condition;
            strncpy(t->callback_error_msg, cl_error_msg,
                    sizeof(t->callback_error_msg) - 1);
            t->callback_error_msg[sizeof(t->callback_error_msg) - 1] = '\0';
        }
        memset(cret, 0, sizeof(*cret));
    }

    CL_GC_UNPROTECT(n + 1);
    t->callback_depth--;
    t->nlx_floor = saved_nlx_floor;
    cl_handler_floor = saved_handler_floor;
    cl_restart_floor = saved_restart_floor;
}

/* The caller's side of the boundary: re-signal what a callback parked.
 * Called by every path that returns from foreign code — FFI:CALL-FOREIGN,
 * the OP_AMIGA_CALL dispatch (VM, JIT and stub callers alike), AMIGA:CALL-
 * LIBRARY / -FAST.  Nested foreign calls are fine: a callback that itself
 * calls the OS re-signals the inner escape inside its own Lisp, which
 * either handles it or carries it to the outer boundary. */
void cl_ffi_deferred_error_check(void)
{
    CL_Thread *t = CT;
    int code;
    CL_Obj cond;

    if (ffi_foreign_task_calls != ffi_foreign_task_reported) {
        char buf[160];
        ffi_foreign_task_reported = ffi_foreign_task_calls;
        snprintf(buf, sizeof(buf),
                 "; WARNING: an FFI callback was invoked from a task that is not a "
                 "Lisp thread (%u call%s so far) and returned 0 -- Lisp cannot run "
                 "there (see FFI:MAKE-CALLBACK)\n",
                 (unsigned)ffi_foreign_task_reported,
                 ffi_foreign_task_reported == 1 ? "" : "s");
        cl_write_cstring_to_debug_io(buf);
    }

    if (t->callback_error_code == 0) return;
    code = t->callback_error_code;
    cond = t->callback_error;
    t->callback_error_code = 0;
    t->callback_error = CL_NIL;
    if (code == CL_ERR_EXIT)
        cl_error(CL_ERR_EXIT, "");
    if (!CL_NULL_P(cond) && CL_CONDITION_P(cond)) {
        /* Exactly ERROR's path: handlers, then the debugger with the
         * original condition (type, slots, PRINT-OBJECT intact).  Both
         * calls unwind via longjmp, which restores the root count. */
        CL_GC_PROTECT(cond);
        cl_signal_condition(cond);
        cl_error_from_condition(cond);
    }
    cl_error(code, "%s", t->callback_error_msg);
}

/* Map a register keyword of MAKE-CALLBACK's REGS list to platform.h's
 * encoding; NIL = the C stack. */
static int8_t ffi_callback_reg(CL_Obj kw)
{
    const char *name;
    if (CL_NULL_P(kw)) return CL_FFI_REG_STACK;
    if (!CL_SYMBOL_P(kw) ||
        ((CL_Symbol *)CL_OBJ_TO_PTR(kw))->package != cl_package_keyword)
        cl_error(CL_ERR_TYPE,
                 "FFI:MAKE-CALLBACK: REGS entries must be :D0-:D7, :A0-:A6 or NIL");
    name = cl_symbol_name(kw);
    if ((name[0] == 'D' || name[0] == 'A') && name[1] >= '0' && name[2] == '\0') {
        int n = name[1] - '0';
        if (name[0] == 'D' && n <= 7) return (int8_t)n;
        if (name[0] == 'A' && n <= 6) return (int8_t)(8 + n);
    }
    cl_error(CL_ERR_ARGS,
             "FFI:MAKE-CALLBACK: unknown register %s (expected :D0-:D7, :A0-:A6 or NIL)",
             name);
    return CL_FFI_REG_STACK;
}

/* Claim a callback slot for LISP_FN and build its entry.  METHODS (owned
 * by the slot from here on, released by FREE-CALLBACK or on failure) makes
 * it a method-filtered dispatcher when FILTERED is set.  WHO names the
 * caller in the errors. */
static CL_Obj ffi_callback_install(const char *who, CLFFIType ret_type, int n,
                                   const CLFFIType *atypes, const int8_t *aregs,
                                   CL_Obj lisp_fn, int filtered,
                                   uint32_t *methods, uint32_t n_methods)
{
    int slot, i;
    void *code, *plat = NULL;
    FFICallback *cb;

    /* Claim the slot under the lock (in_use=1 immediately) so a peer
     * thread's scan cannot pick the same slot; release it on failure. */
    if (ffi_callback_lock) platform_mutex_lock(ffi_callback_lock);
    for (slot = 0; slot < CL_FFI_MAX_CALLBACKS; slot++)
        if (!ffi_callbacks[slot].in_use) break;
    if (slot == CL_FFI_MAX_CALLBACKS) {
        if (ffi_callback_lock) platform_mutex_unlock(ffi_callback_lock);
        if (methods) platform_free(methods);
        cl_error(CL_ERR_GENERAL, "%s: too many live callbacks (max %d)", who, CL_FFI_MAX_CALLBACKS);
    }
    cb = &ffi_callbacks[slot];
    cb->in_use = 1;
    if (ffi_callback_lock) platform_mutex_unlock(ffi_callback_lock);

    cb->ret_type = ret_type;
    cb->nargs = n;
    for (i = 0; i < n; i++) {
        cb->arg_types[i] = atypes[i];
        cb->arg_regs[i] = aregs[i];
    }
    cb->lisp_fn = lisp_fn;  /* rooted slot — survives the alloc below */
    cb->filtered = filtered;
    cb->methods = methods;
    cb->n_methods = n_methods;
    cb->calls = 0;
    cb->passed = 0;

    code = platform_ffi_make_closure(ret_type, n, atypes, cb->arg_regs,
                                     ffi_callback_handler, cb, &plat);
    if (!code) {
        cb->lisp_fn = CL_NIL;
        cb->filtered = 0;
        cb->methods = NULL;
        cb->n_methods = 0;
        cb->in_use = 0;
        if (methods) platform_free(methods);
        cl_error(CL_ERR_GENERAL, "%s: callbacks are not supported on this platform", who);
    }
    cb->plat_closure = plat;
    cb->code_handle = platform_ffi_register(code);
    return cl_make_foreign_pointer(cb->code_handle, 0, 0);
}

/* (ffi:make-callback ret-type arg-types lisp-fn &optional regs) → foreign-pointer
 * Returns a C-callable function pointer that invokes LISP-FN.  REGS, a
 * list parallel to ARG-TYPES of :D0-:D7 / :A0-:A6 / NIL, names the 68k
 * register each argument arrives in — the struct Hook convention is
 * (:a0 :a2 :a1) for (hook object message), a BOOPSI dispatcher's the same
 * for (class object message); NIL (and an absent list) means the C stack.
 * The host ignores REGS: its callbacks are C-ABI functions, so a hook
 * entry made there is simply a C function of (hook, object, message). */
static CL_Obj bi_ffi_make_callback(CL_Obj *args, int nargs)
{
    int i, n = 0, nregs = 0;
    CLFFIType ret_type;
    CLFFIType atypes[CL_FFI_MAX_ARGS];
    int8_t aregs[CL_FFI_MAX_ARGS];
    CL_Obj tlist;

    ret_type = ffi_kw_to_type(args[0]);
    for (tlist = args[1]; !CL_NULL_P(tlist); tlist = cl_cdr(tlist)) {
        if (n >= CL_FFI_MAX_ARGS)
            cl_error(CL_ERR_ARGS, "FFI:MAKE-CALLBACK: too many argument types (max %d)", CL_FFI_MAX_ARGS);
        atypes[n++] = ffi_kw_to_type(cl_car(tlist));
    }
    for (i = 0; i < n; i++) aregs[i] = CL_FFI_REG_STACK;
    if (nargs > 3) {
        for (tlist = args[3]; !CL_NULL_P(tlist); tlist = cl_cdr(tlist)) {
            if (!CL_CONS_P(tlist))
                cl_error(CL_ERR_TYPE, "FFI:MAKE-CALLBACK: REGS must be a list");
            if (nregs >= n)
                cl_error(CL_ERR_ARGS,
                         "FFI:MAKE-CALLBACK: REGS has more entries than ARG-TYPES (%d)", n);
            aregs[nregs++] = ffi_callback_reg(cl_car(tlist));
        }
        if (nregs != 0 && nregs != n)
            cl_error(CL_ERR_ARGS,
                     "FFI:MAKE-CALLBACK: REGS names %d register%s for %d argument%s -- "
                     "give one entry per argument (NIL for a stack argument)",
                     nregs, nregs == 1 ? "" : "s", n, n == 1 ? "" : "s");
    }
    if (!CL_FUNCTION_P(args[2]) && !CL_CLOSURE_P(args[2]) &&
        !cl_funcallable_instance_p(args[2]))
        cl_error(CL_ERR_TYPE, "FFI:MAKE-CALLBACK: LISP-FN must be a function");
#ifdef PLATFORM_AMIGA
    /* The 68k stub / MorphOS gate return d0 only and do not touch fp0. */
    if (ret_type == CL_FFI_FLOAT || ret_type == CL_FFI_DOUBLE ||
        ret_type == CL_FFI_I64 || ret_type == CL_FFI_U64)
        cl_error(CL_ERR_ARGS,
                 "FFI:MAKE-CALLBACK: on AmigaOS/MorphOS a callback returns a 32-bit "
                 "integer or pointer in d0 -- :FLOAT, :DOUBLE, :INT64 and :UINT64 "
                 "results are not supported");
    for (i = 0; i < n; i++) {
        if (atypes[i] == CL_FFI_FLOAT || atypes[i] == CL_FFI_DOUBLE)
            cl_error(CL_ERR_ARGS,
                     "FFI:MAKE-CALLBACK: on AmigaOS/MorphOS :FLOAT / :DOUBLE callback "
                     "arguments are not supported (fp0 vs d0/d1 differs between the "
                     "soft-float and FPU builds)");
        if ((atypes[i] == CL_FFI_I64 || atypes[i] == CL_FFI_U64) &&
            aregs[i] != CL_FFI_REG_STACK)
            cl_error(CL_ERR_ARGS,
                     "FFI:MAKE-CALLBACK: a 64-bit argument cannot arrive in a "
                     "register (argument %d)", i + 1);
    }
#endif

    return ffi_callback_install("FFI:MAKE-CALLBACK", ret_type, n, atypes, aregs,
                                args[2], 0, NULL, 0);
}

/* (ffi:free-callback fp) → nil
 * Release a callback created by ffi:make-callback.  Frees the native closure
 * and reclaims the callback slot so it can be reused.  The foreign-pointer
 * argument becomes invalid after this call. */
static CL_Obj bi_ffi_free_callback(CL_Obj *args, int nargs)
{
    CL_ForeignPtr *fp;
    uint32_t h;
    int slot;
    (void)nargs;

    if (!CL_FOREIGN_POINTER_P(args[0]))
        cl_error(CL_ERR_TYPE, "FFI:FREE-CALLBACK: expected a foreign-pointer");

    fp = (CL_ForeignPtr *)CL_OBJ_TO_PTR(args[0]);
    h = fp->address;

    if (ffi_callback_lock) platform_mutex_lock(ffi_callback_lock);
    for (slot = 0; slot < CL_FFI_MAX_CALLBACKS; slot++) {
        FFICallback *cb = &ffi_callbacks[slot];
        if (cb->in_use && cb->code_handle == h) {
            platform_ffi_free_closure(cb->plat_closure);
            platform_ffi_release(cb->code_handle);
            cb->lisp_fn = CL_NIL;
            cb->plat_closure = NULL;
            cb->code_handle = 0;
            /* No OS call can be inside the entry any more (the caller's
             * contract: the class is gone), so the table goes with it. */
            if (cb->methods) platform_free(cb->methods);
            cb->methods = NULL;
            cb->n_methods = 0;
            cb->filtered = 0;
            cb->in_use = 0;
            if (ffi_callback_lock) platform_mutex_unlock(ffi_callback_lock);
            return CL_NIL;
        }
    }
    if (ffi_callback_lock) platform_mutex_unlock(ffi_callback_lock);
    cl_error(CL_ERR_GENERAL, "FFI:FREE-CALLBACK: not a live callback pointer");
    return CL_NIL;
}

/* (ffi::%make-method-dispatcher lisp-fn methods) → foreign-pointer
 * A BOOPSI class dispatcher -- (class, object, message) in a0/a2/a1,
 * result in d0 -- that calls LISP-FN only for the messages whose MethodID
 * is in METHODS (a list of unsigned 32-bit integers, possibly empty) and
 * hands every other one to the superclass natively (DoSuperMethodA).
 * AMIGA.FFI:MAKE-DISPATCHER's :METHODS; released with FFI:FREE-CALLBACK. */
static CL_Obj bi_ffi_make_method_dispatcher(CL_Obj *args, int nargs)
{
    static const char *who = "AMIGA.FFI:MAKE-DISPATCHER";
    CLFFIType atypes[3] = { CL_FFI_POINTER, CL_FFI_POINTER, CL_FFI_POINTER };
    int8_t aregs[3] = { 8, 10, 9 };   /* a0, a2, a1 */
    uint32_t *methods = NULL, n = 0, i;
    CL_Obj list;
    (void)nargs;

    if (!CL_FUNCTION_P(args[0]) && !CL_CLOSURE_P(args[0]) &&
        !cl_funcallable_instance_p(args[0]))
        cl_error(CL_ERR_TYPE, "%s: the dispatcher must be a function", who);
    for (list = args[1]; !CL_NULL_P(list); list = cl_cdr(list)) {
        if (!CL_CONS_P(list))
            cl_error(CL_ERR_TYPE, "%s: METHODS must be a proper list of method ids", who);
        if (!CL_INTEGER_P(cl_car(list)))
            cl_error(CL_ERR_TYPE, "%s: METHODS must hold unsigned 32-bit method ids (MUIM_ / OM_ constants)", who);
        n++;
    }
    if (!platform_amiga_push_method_prepare())
        cl_error(CL_ERR_GENERAL, "%s: cannot open utility.library v36, which the superclass call needs", who);
    if (n > 0) {
        methods = (uint32_t *)platform_alloc(n * sizeof(uint32_t));
        if (methods == NULL) cl_error(CL_ERR_GENERAL, "%s: out of memory", who);
        for (list = args[1], i = 0; i < n; list = cl_cdr(list), i++)
            methods[i] = (uint32_t)cl_ffi_obj_to_u64(cl_car(list));
    }
    return ffi_callback_install(who, CL_FFI_U32, 3, atypes, aregs, args[0],
                                1, methods, n);
}

/* (ffi::%method-dispatcher-stats fp) → (calls passed count) or NIL: how
 * many messages reached the dispatcher, how many of them went to the
 * superclass without entering Lisp, and how many method ids it lists.
 * NIL for a live callback that is not a method-filtered dispatcher. */
static CL_Obj bi_ffi_method_dispatcher_stats(CL_Obj *args, int nargs)
{
    CL_ForeignPtr *fp;
    uint32_t calls = 0, passed = 0, count = 0;
    int slot, found = 0, filtered = 0;
    (void)nargs;

    if (!CL_FOREIGN_POINTER_P(args[0]))
        cl_error(CL_ERR_TYPE, "AMIGA.FFI:DISPATCHER-STATS: expected a dispatcher (a foreign pointer)");
    fp = (CL_ForeignPtr *)CL_OBJ_TO_PTR(args[0]);
    if (ffi_callback_lock) platform_mutex_lock(ffi_callback_lock);
    for (slot = 0; slot < CL_FFI_MAX_CALLBACKS; slot++) {
        FFICallback *cb = &ffi_callbacks[slot];
        if (cb->in_use && cb->code_handle == fp->address) {
            found = 1;
            filtered = cb->filtered;
            calls = cb->calls; passed = cb->passed; count = cb->n_methods;
            break;
        }
    }
    if (ffi_callback_lock) platform_mutex_unlock(ffi_callback_lock);
    if (!found)
        cl_error(CL_ERR_GENERAL, "AMIGA.FFI:DISPATCHER-STATS: not a live callback pointer");
    if (!filtered) return CL_NIL;
    /* counters wrap into the fixnum range rather than cons a bignum */
    return cl_cons(CL_MAKE_FIXNUM((int32_t)(calls & 0x3FFFFFFF)),
                   cl_cons(CL_MAKE_FIXNUM((int32_t)(passed & 0x3FFFFFFF)),
                           cl_cons(CL_MAKE_FIXNUM((int32_t)count), CL_NIL)));
}

/* ================================================================
 * Init
 * ================================================================ */

void cl_builtins_ffi_init(void)
{
    {
        /* Callback Lisp-function slots are GC roots (referenced only from
         * C-allocated closures, invisible to the mark phase otherwise). */
        int ci;
        for (ci = 0; ci < CL_FFI_MAX_CALLBACKS; ci++) {
            ffi_callbacks[ci].lisp_fn = CL_NIL;
            cl_gc_register_root(&ffi_callbacks[ci].lisp_fn);
        }
        platform_mutex_init(&ffi_callback_lock);
    }

    /* EXT:*CALLBACK-ERROR-POLICY* (see cl_callback_debugger_allowed).
     * Rooted before the interning below allocates, like *EXIT-HOOKS*. */
    cl_gc_register_root(&SYM_CALLBACK_ERROR_POLICY);
    cl_gc_register_root(&kw_callback_debug);
    kw_callback_debug = cl_intern_keyword("DEBUG", 5);
    SYM_CALLBACK_ERROR_POLICY = cl_intern_in("*CALLBACK-ERROR-POLICY*", 23, cl_package_ext);
    {
        CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_CALLBACK_ERROR_POLICY);
        s->flags |= CL_SYM_SPECIAL;
        s->value = cl_intern_keyword("DEFER", 5);
    }
    cl_export_symbol(SYM_CALLBACK_ERROR_POLICY, cl_package_ext);
    cl_register_builtin_exported("%FFI-FOREIGN-TASK-CALLS", bi_ext_ffi_foreign_task_calls,
                                 0, 0, cl_package_ext);

    /* Register all keyword cache slots as GC roots BEFORE any cl_intern_keyword
     * call allocates — a compaction triggered mid-sequence would leave already-
     * stored slots with stale offsets if they aren't roots yet (same class of
     * bug as the funcallable-instance stale-static-cache). */
    {
        int ti;
        for (ti = 0; ti <= (int)CL_FFI_POINTER; ti++) {
            ffi_type_keywords[ti] = CL_NIL;
            cl_gc_register_root(&ffi_type_keywords[ti]);
        }
    }

    /* Primitive type keywords for CALL-FOREIGN (interned once). */
    ffi_type_keywords[CL_FFI_VOID]    = cl_intern_keyword("VOID", 4);
    ffi_type_keywords[CL_FFI_I8]      = cl_intern_keyword("INT8", 4);
    ffi_type_keywords[CL_FFI_U8]      = cl_intern_keyword("UINT8", 5);
    ffi_type_keywords[CL_FFI_I16]     = cl_intern_keyword("INT16", 5);
    ffi_type_keywords[CL_FFI_U16]     = cl_intern_keyword("UINT16", 6);
    ffi_type_keywords[CL_FFI_I32]     = cl_intern_keyword("INT32", 5);
    ffi_type_keywords[CL_FFI_U32]     = cl_intern_keyword("UINT32", 6);
    ffi_type_keywords[CL_FFI_I64]     = cl_intern_keyword("INT64", 5);
    ffi_type_keywords[CL_FFI_U64]     = cl_intern_keyword("UINT64", 6);
    ffi_type_keywords[CL_FFI_FLOAT]   = cl_intern_keyword("FLOAT", 5);
    ffi_type_keywords[CL_FFI_DOUBLE]  = cl_intern_keyword("DOUBLE", 6);
    ffi_type_keywords[CL_FFI_POINTER] = cl_intern_keyword("POINTER", 7);

    /* FFI stubs — DEFCSTRUCT's accessor constructor and the introspection
     * hook (internal: %-prefixed, not exported; DEFCSTRUCT spells it
     * ffi::%make-field-stub). */
    cl_register_builtin("%MAKE-FIELD-STUB", bi_ffi_make_field_stub, 4, 6, cl_package_ffi);
    cl_register_builtin("%DEFINE-CSTRUCT-ACCESSORS", bi_ffi_define_cstruct_accessors,
                        1, 1, cl_package_ffi);
    cl_register_builtin("%FFI-STUB-INFO",   bi_ffi_stub_info,       1, 1, cl_package_ffi);
    /* compile_call matches this exact symbol to register the DEFSETF pairs
     * at compile time (cl_ffi_cstruct_register_setfs) */
    cl_ffi_define_cstruct_accessors_sym =
        cl_intern_in("%DEFINE-CSTRUCT-ACCESSORS", 25, cl_package_ffi);
    cl_gc_register_root(&cl_ffi_define_cstruct_accessors_sym);

    /* Foreign pointer management */
    ffi_defun("MAKE-FOREIGN-POINTER",    bi_ffi_make_fp,        1, 2);
    ffi_defun("FOREIGN-POINTER-ADDRESS", bi_ffi_fp_address,     1, 1);
    ffi_defun("FOREIGN-POINTER-P",       bi_ffi_fp_p,           1, 1);
    ffi_defun("NULL-POINTER-P",          bi_ffi_null_p,         1, 1);
    ffi_defun("POINTER-EQ",              bi_ffi_pointer_eq,     2, 2);

    /* Allocation */
    ffi_defun("ALLOC-FOREIGN",           bi_ffi_alloc,          1, 1);
    ffi_defun("FREE-FOREIGN",            bi_ffi_free,           1, 1);

    /* Peek/Poke */
    ffi_defun("PEEK-U32",               bi_ffi_peek32,          1, 2);
    ffi_defun("PEEK-U16",               bi_ffi_peek16,          1, 2);
    ffi_defun("PEEK-U8",                bi_ffi_peek8,           1, 2);
    ffi_defun("POKE-U32",               bi_ffi_poke32,          2, 3);
    ffi_defun("POKE-U16",               bi_ffi_poke16,          2, 3);
    ffi_defun("POKE-U8",                bi_ffi_poke8,           2, 3);

    /* Bulk byte transfer */
    ffi_defun("POKE-BYTES",             bi_ffi_poke_bytes,      2, 5);
    ffi_defun("PEEK-BYTES",             bi_ffi_peek_bytes,      2, 5);

    /* Typed peek/poke: signed, 64-bit, float/double, pointer */
    ffi_defun("PEEK-I8",                bi_ffi_peek_i8,         1, 2);
    ffi_defun("PEEK-I16",               bi_ffi_peek_i16,        1, 2);
    ffi_defun("PEEK-I32",               bi_ffi_peek_i32,        1, 2);
    ffi_defun("PEEK-U64",               bi_ffi_peek_u64,        1, 2);
    ffi_defun("PEEK-I64",               bi_ffi_peek_i64,        1, 2);
    ffi_defun("PEEK-SINGLE",            bi_ffi_peek_single,     1, 2);
    ffi_defun("PEEK-DOUBLE",            bi_ffi_peek_double,     1, 2);
    ffi_defun("PEEK-POINTER",           bi_ffi_peek_pointer,    1, 2);
    ffi_defun("POKE-I8",                bi_ffi_poke_i8,         2, 3);
    ffi_defun("POKE-I16",               bi_ffi_poke_i16,        2, 3);
    ffi_defun("POKE-I32",               bi_ffi_poke_i32,        2, 3);
    ffi_defun("POKE-U64",               bi_ffi_poke_u64,        2, 3);
    ffi_defun("POKE-I64",               bi_ffi_poke_i64,        2, 3);
    ffi_defun("POKE-SINGLE",            bi_ffi_poke_single,     2, 3);
    ffi_defun("POKE-DOUBLE",            bi_ffi_poke_double,     2, 3);
    ffi_defun("POKE-POINTER",           bi_ffi_poke_pointer,    2, 3);

    /* String conversion */
    ffi_defun("FOREIGN-STRING",          bi_ffi_foreign_string,     1, 1);
    ffi_defun("FOREIGN-TO-STRING",       bi_ffi_foreign_to_string,  1, 2);

    /* Pointer arithmetic */
    ffi_defun("POINTER+",               bi_ffi_pointer_plus,        2, 2);

    /* Dynamic libraries + foreign function calls (host) */
    ffi_defun("LOAD-LIBRARY",           bi_ffi_load_library,        1, 1);
    ffi_defun("CLOSE-LIBRARY",          bi_ffi_close_library,       1, 1);
    ffi_defun("SYMBOL-POINTER",         bi_ffi_symbol_pointer,      1, 2);
    ffi_defun("CALL-FOREIGN",           bi_ffi_call_foreign,        4, 5);
    ffi_defun("MAKE-CALLBACK",          bi_ffi_make_callback,       3, 4);
    ffi_defun("FREE-CALLBACK",          bi_ffi_free_callback,       1, 1);
    /* Internal (unexported): AMIGA.FFI:MAKE-DISPATCHER / DISPATCHER-STATS
     * are the documented surface. */
    cl_register_builtin("%MAKE-METHOD-DISPATCHER",  bi_ffi_make_method_dispatcher,  2, 2, cl_package_ffi);
    cl_register_builtin("%METHOD-DISPATCHER-STATS", bi_ffi_method_dispatcher_stats, 1, 1, cl_package_ffi);
}
