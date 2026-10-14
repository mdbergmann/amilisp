/*
 * builtins_type.c — Type system: type-of, typep, coerce, subtypep
 *
 * Handles compound type specifiers: (or ...), (and ...), (not ...),
 * (member ...), (eql ...), (satisfies ...), user-defined types.
 */

#include "builtins.h"
#include "symbol.h"
#include "package.h"
#include "mem.h"
#include "float.h"
#include "ratio.h"
#include "bignum.h"
#include "printer.h"
#include "error.h"
#include "vm.h"
#include "compiler.h"
#include "readtable.h"
#include "string_utils.h"
#include "../platform/platform.h"
#include <string.h>

/* Pre-interned compound type specifier head symbols */
static CL_Obj TYPE_SYM_OR = CL_NIL;
static CL_Obj TYPE_SYM_AND = CL_NIL;
static CL_Obj TYPE_SYM_NOT = CL_NIL;
static CL_Obj TYPE_SYM_MEMBER = CL_NIL;
static CL_Obj TYPE_SYM_EQL = CL_NIL;
static CL_Obj TYPE_SYM_SATISFIES = CL_NIL;
/* Numeric range type specifier head symbols */
static CL_Obj TYPE_SYM_INTEGER = CL_NIL;
static CL_Obj TYPE_SYM_RATIONAL = CL_NIL;
static CL_Obj TYPE_SYM_REAL = CL_NIL;
static CL_Obj TYPE_SYM_FLOAT = CL_NIL;
static CL_Obj TYPE_SYM_SINGLE_FLOAT = CL_NIL;
static CL_Obj TYPE_SYM_SHORT_FLOAT = CL_NIL;
static CL_Obj TYPE_SYM_DOUBLE_FLOAT = CL_NIL;
static CL_Obj TYPE_SYM_LONG_FLOAT = CL_NIL;
static CL_Obj TYPE_SYM_FIXNUM = CL_NIL;
static CL_Obj TYPE_SYM_BIGNUM = CL_NIL;
static CL_Obj TYPE_SYM_RATIO = CL_NIL;
static CL_Obj TYPE_SYM_NUMBER = CL_NIL;
static CL_Obj TYPE_SYM_STAR = CL_NIL; /* * for wildcard bounds in range types */
/* Lazy-cached symbol for Gray stream type check (set on first use) */
static CL_Obj TYPE_SYM_GRAY_FUNDAMENTAL_STREAM = CL_NIL;
/* Compound vector/array type specifier head symbols */
static CL_Obj TYPE_SYM_SIMPLE_VECTOR = CL_NIL;
static CL_Obj TYPE_SYM_VECTOR = CL_NIL;
static CL_Obj TYPE_SYM_SIMPLE_ARRAY = CL_NIL;
static CL_Obj TYPE_SYM_ARRAY = CL_NIL;
static CL_Obj TYPE_SYM_STRING = CL_NIL;
static CL_Obj TYPE_SYM_SIMPLE_STRING = CL_NIL;
static CL_Obj TYPE_SYM_BASE_STRING = CL_NIL;
static CL_Obj TYPE_SYM_SIMPLE_BASE_STRING = CL_NIL;
static CL_Obj TYPE_SYM_BIT_VECTOR = CL_NIL;
static CL_Obj TYPE_SYM_SIMPLE_BIT_VECTOR = CL_NIL;

/* Forward declarations */
static int typep_check(CL_Obj obj, CL_Obj type_spec);
int cl_type_is_bit_subtype(CL_Obj type);
int cl_type_is_u8_subtype(CL_Obj type);
int cl_type_is_s8_subtype(CL_Obj type);
int cl_type_is_u16_subtype(CL_Obj type);
int cl_type_is_s16_subtype(CL_Obj type);

/* --- Upgraded array element classes (shared by TYPEP and SUBTYPEP) ---
 *
 * amilisp specializes array storage on exactly these element classes:
 * BIT (bit-vectors), CHAR (strings), U8/S8 (packed byte vectors), and the
 * annotated general-vector codes FIXNUM / SINGLE-FLOAT / DOUBLE-FLOAT; every
 * other element type upgrades to T.  TYPEP and SUBTYPEP must both map an
 * element-type SPECIFIER to the same class MAKE-ARRAY would build for it —
 * including through user deftype aliases (flexi-streams' OCTET broke here:
 * the old inline checks only expanded aliases on some paths, so
 * (TYPEP u8-vec '(ARRAY OCTET *)) was NIL while
 * (TYPEP u8-vec '(ARRAY (UNSIGNED-BYTE 8) *)) was T). */
#define AEC_WILD   (-1)  /* * — no element-type constraint */
#define AEC_T      CL_VEC_ELT_T
#define AEC_FIXNUM CL_VEC_ELT_FIXNUM
#define AEC_SF     CL_VEC_ELT_SINGLE_FLOAT
#define AEC_DF     CL_VEC_ELT_DOUBLE_FLOAT
#define AEC_BIT    100
#define AEC_CHAR   101
#define AEC_U8     102
#define AEC_S8     103
#define AEC_U16    104
#define AEC_S16    105

/* Map an element-type SPECIFIER to its upgraded class, expanding deftype
 * aliases (specialization order bit > u8 > s8 mirrors
 * cl_classify_array_elt_type / make-array).
 * GC: expansion may run cl_vm_apply → objects can move; callers must
 * GC-protect live CL_Obj locals (obj, args, ...) across this call. */
static int elt_spec_class(CL_Obj et)
{
    int is_char = 0, is_wide = 0, is_bit = 0, is_u8 = 0, is_s8 = 0;
    int is_u16 = 0, is_s16 = 0;
    if (CL_SYMBOL_P(et) && strcmp(cl_symbol_name(et), "*") == 0)
        return AEC_WILD;
    cl_classify_array_elt_type(et, 16, &is_char, &is_wide, &is_bit,
                               &is_u8, &is_s8, &is_u16, &is_s16);
    if (is_bit)  return AEC_BIT;
    if (is_char) return AEC_CHAR;
    if (is_u8)   return AEC_U8;
    if (is_s8)   return AEC_S8;
    if (is_u16)  return AEC_U16;
    if (is_s16)  return AEC_S16;
    /* General storage: discriminate the annotated numeric codes from T
     * (also expands deftype aliases of FIXNUM / the float types). */
    return (int)cl_classify_vec_elt_code(et, 16);
}

/* The upgraded element class an array OBJECT actually stores.  Pure
 * inspection — never allocates. */
static int obj_elt_class(CL_Obj obj)
{
    if (CL_BIT_VECTOR_P(obj))  return AEC_BIT;
    if (CL_ANY_STRING_P(obj))  return AEC_CHAR;
    if (CL_BYTE_VECTOR_P(obj)) {
        CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(obj);
        if (bv->elt_shift) return bv->is_signed ? AEC_S16 : AEC_U16;
        return bv->is_signed ? AEC_S8 : AEC_U8;
    }
    if (CL_VECTOR_P(obj)) {
        CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(obj);
        if (v->flags & CL_VEC_FLAG_STRING) return AEC_CHAR;
        switch (v->elt_type) {
        case CL_VEC_ELT_U8:           return AEC_U8;
        case CL_VEC_ELT_S8:           return AEC_S8;
        case CL_VEC_ELT_U16:          return AEC_U16;
        case CL_VEC_ELT_S16:          return AEC_S16;
        case CL_VEC_ELT_FIXNUM:       return AEC_FIXNUM;
        case CL_VEC_ELT_SINGLE_FLOAT: return AEC_SF;
        case CL_VEC_ELT_DOUBLE_FLOAT: return AEC_DF;
        default:                      return AEC_T;
        }
    }
    return AEC_T;
}

/* --- typep for simple symbol type specifiers ---
 *
 * The standard type names are dispatched by a small integer code kept in
 * the top byte of the specifier symbol's own flags word (types.h,
 * CL_SYM_TYPECODE), assigned once at startup by cl_typep_codes_init.  What
 * used to be a cascade of up to ~45 strcmp calls per TYPEP — paid on every
 * OP_ASSERT_TYPE, so on every (declare (type ...)) in compiled code, and
 * paid in FULL by exactly the specifiers that are NOT standard names
 * (structure, condition and CLOS class types, which fell through the whole
 * cascade first) — is now one load and a switch.  On m68k, where strcmp is
 * far more expensive relative to the rest, the saving is larger still.
 *
 * The code is keyed on symbol IDENTITY, matching how the compound-specifier
 * heads are already compared (head == TYPE_SYM_INTEGER, below).  A symbol
 * that merely shares a standard type NAME without being the COMMON-LISP
 * symbol therefore reaches the user-type path; typep_symbol_by_name at the
 * end of that path re-checks it against the name table before signalling,
 * so such a specifier still resolves exactly as it used to, just slower. */

enum {
    TS_NONE = 0,
    TS_T, TS_NIL, TS_NULL, TS_BOOLEAN, TS_SYMBOL, TS_KEYWORD,
    TS_CONS, TS_LIST, TS_ATOM,
    TS_FIXNUM, TS_BIGNUM, TS_INTEGER, TS_UNSIGNED_BYTE, TS_SIGNED_BYTE,
    TS_BIT, TS_RATIO, TS_RATIONAL,
    TS_SINGLE_FLOAT, TS_DOUBLE_FLOAT, TS_FLOAT, TS_REAL, TS_COMPLEX,
    TS_NUMBER,
    TS_CHARACTER, TS_BASE_CHAR, TS_STANDARD_CHAR, TS_EXTENDED_CHAR,
    TS_STRING, TS_SIMPLE_STRING, TS_BASE_STRING, TS_SIMPLE_BASE_STRING,
    TS_BIT_VECTOR, TS_SIMPLE_BIT_VECTOR,
    TS_ARRAY, TS_SIMPLE_ARRAY, TS_VECTOR, TS_SIMPLE_VECTOR, TS_SEQUENCE,
    TS_FUNCTION, TS_GENERIC_FUNCTION, TS_COMPILED_FUNCTION,
    TS_HASH_TABLE, TS_PACKAGE, TS_RESTART,
    TS_STREAM, TS_SYNONYM_STREAM, TS_FILE_STREAM, TS_STRING_STREAM,
    TS_TWO_WAY_STREAM, TS_BROADCAST_STREAM, TS_CONCATENATED_STREAM,
    TS_ECHO_STREAM,
    TS_RANDOM_STATE, TS_PATHNAME, TS_LOGICAL_PATHNAME, TS_READTABLE,
    TS_STRUCTURE,
    TS_LAST
};

/* The standard type names, in COMMON-LISP, and their codes.  SHORT-FLOAT
 * and LONG-FLOAT are amilisp's single/double aliases; GENERIC-FUNCTION and
 * STANDARD-GENERIC-FUNCTION answer alike, as do STRUCTURE and
 * STRUCTURE-OBJECT.  Keep in sync with the switch in typep_builtin_code. */
static const struct { const char *name; uint8_t code; } typep_name_codes[] = {
    { "T", TS_T }, { "NIL", TS_NIL }, { "NULL", TS_NULL },
    { "BOOLEAN", TS_BOOLEAN }, { "SYMBOL", TS_SYMBOL },
    { "KEYWORD", TS_KEYWORD },
    { "CONS", TS_CONS }, { "LIST", TS_LIST }, { "ATOM", TS_ATOM },
    { "FIXNUM", TS_FIXNUM }, { "BIGNUM", TS_BIGNUM },
    { "INTEGER", TS_INTEGER },
    { "UNSIGNED-BYTE", TS_UNSIGNED_BYTE }, { "SIGNED-BYTE", TS_SIGNED_BYTE },
    { "BIT", TS_BIT }, { "RATIO", TS_RATIO }, { "RATIONAL", TS_RATIONAL },
    { "SINGLE-FLOAT", TS_SINGLE_FLOAT }, { "SHORT-FLOAT", TS_SINGLE_FLOAT },
    { "DOUBLE-FLOAT", TS_DOUBLE_FLOAT }, { "LONG-FLOAT", TS_DOUBLE_FLOAT },
    { "FLOAT", TS_FLOAT }, { "REAL", TS_REAL }, { "COMPLEX", TS_COMPLEX },
    { "NUMBER", TS_NUMBER },
    { "CHARACTER", TS_CHARACTER }, { "BASE-CHAR", TS_BASE_CHAR },
    { "STANDARD-CHAR", TS_STANDARD_CHAR },
    { "EXTENDED-CHAR", TS_EXTENDED_CHAR },
    { "STRING", TS_STRING }, { "SIMPLE-STRING", TS_SIMPLE_STRING },
    { "BASE-STRING", TS_BASE_STRING },
    { "SIMPLE-BASE-STRING", TS_SIMPLE_BASE_STRING },
    { "BIT-VECTOR", TS_BIT_VECTOR },
    { "SIMPLE-BIT-VECTOR", TS_SIMPLE_BIT_VECTOR },
    { "ARRAY", TS_ARRAY }, { "SIMPLE-ARRAY", TS_SIMPLE_ARRAY },
    { "VECTOR", TS_VECTOR }, { "SIMPLE-VECTOR", TS_SIMPLE_VECTOR },
    { "SEQUENCE", TS_SEQUENCE },
    { "FUNCTION", TS_FUNCTION },
    { "GENERIC-FUNCTION", TS_GENERIC_FUNCTION },
    { "STANDARD-GENERIC-FUNCTION", TS_GENERIC_FUNCTION },
    { "COMPILED-FUNCTION", TS_COMPILED_FUNCTION },
    { "HASH-TABLE", TS_HASH_TABLE }, { "PACKAGE", TS_PACKAGE },
    { "RESTART", TS_RESTART },
    { "STREAM", TS_STREAM }, { "SYNONYM-STREAM", TS_SYNONYM_STREAM },
    { "FILE-STREAM", TS_FILE_STREAM }, { "STRING-STREAM", TS_STRING_STREAM },
    { "TWO-WAY-STREAM", TS_TWO_WAY_STREAM },
    { "BROADCAST-STREAM", TS_BROADCAST_STREAM },
    { "CONCATENATED-STREAM", TS_CONCATENATED_STREAM },
    { "ECHO-STREAM", TS_ECHO_STREAM },
    { "RANDOM-STATE", TS_RANDOM_STATE }, { "PATHNAME", TS_PATHNAME },
    { "LOGICAL-PATHNAME", TS_LOGICAL_PATHNAME },
    { "READTABLE", TS_READTABLE },
    { "STRUCTURE", TS_STRUCTURE }, { "STRUCTURE-OBJECT", TS_STRUCTURE }
};

/* Stamp each standard type name's COMMON-LISP symbol with its code.  Called
 * from cl_builtins_type_init, after the package system is up.  The symbols
 * are interned (created if need be) so the code survives however they are
 * first referenced later; no GC rooting is needed because the code lives in
 * the symbol itself, which the collector moves as one object. */
static void cl_typep_codes_init(void)
{
    size_t i;
    for (i = 0; i < sizeof(typep_name_codes) / sizeof(typep_name_codes[0]);
         i++) {
        const char *nm = typep_name_codes[i].name;
        CL_Obj sym = cl_intern_in(nm, (uint32_t)strlen(nm), cl_package_cl);
        if (!CL_NULL_P(sym) && CL_SYMBOL_P(sym)) {
            CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
            s->flags = (s->flags & ~CL_SYM_TYPECODE_MASK)
                       | ((uint32_t)typep_name_codes[i].code
                          << CL_SYM_TYPECODE_SHIFT);
        }
    }
    /* NIL is the tag 0; its flags live on the SYM_NIL storage shadow. */
    if (!CL_NULL_P(SYM_NIL) && CL_SYMBOL_P(SYM_NIL)) {
        CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_NIL);
        s->flags = (s->flags & ~CL_SYM_TYPECODE_MASK)
                   | ((uint32_t)TS_NIL << CL_SYM_TYPECODE_SHIFT);
    }
}

/* Look a standard type name up by string.  Only the compatibility path at
 * the end of typep_symbol uses this, so the linear walk is off the hot
 * path entirely. */
static uint8_t typep_code_for_name(const char *tname)
{
    size_t i;
    for (i = 0; i < sizeof(typep_name_codes) / sizeof(typep_name_codes[0]);
         i++)
        if (strcmp(tname, typep_name_codes[i].name) == 0)
            return typep_name_codes[i].code;
    return TS_NONE;
}

static int typep_builtin_code(CL_Obj obj, uint8_t code)
{
    switch (code) {
    case TS_T:       return 1;
    case TS_NIL:     return 0;
    case TS_NULL:    return CL_NULL_P(obj);
    case TS_BOOLEAN: return CL_NULL_P(obj) || obj == SYM_T;
    case TS_SYMBOL:  return CL_NULL_P(obj) || CL_SYMBOL_P(obj);
    case TS_KEYWORD:
        if (!CL_NULL_P(obj) && CL_SYMBOL_P(obj)) {
            CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(obj);
            return s->package == cl_package_keyword;
        }
        return 0;
    case TS_CONS:    return CL_CONS_P(obj);
    case TS_LIST:    return CL_NULL_P(obj) || CL_CONS_P(obj);
    case TS_ATOM:    return !CL_CONS_P(obj);
    case TS_FIXNUM:  return CL_FIXNUM_P(obj);
    case TS_BIGNUM:  return CL_BIGNUM_P(obj);
    case TS_INTEGER: return CL_INTEGER_P(obj);
    /* Bare unsigned-byte / signed-byte / bit — no compound size given */
    case TS_UNSIGNED_BYTE:
        if (CL_FIXNUM_P(obj)) return CL_FIXNUM_VAL(obj) >= 0;
        if (CL_BIGNUM_P(obj))
            return ((CL_Bignum *)CL_OBJ_TO_PTR(obj))->sign == 0;
        return 0;
    case TS_SIGNED_BYTE: return CL_INTEGER_P(obj);
    case TS_BIT:
        return CL_FIXNUM_P(obj)
               && (CL_FIXNUM_VAL(obj) == 0 || CL_FIXNUM_VAL(obj) == 1);
    case TS_RATIO:        return CL_RATIO_P(obj);
    case TS_RATIONAL:     return CL_RATIONAL_P(obj);
    case TS_SINGLE_FLOAT: return CL_SINGLE_FLOAT_P(obj);
    case TS_DOUBLE_FLOAT: return CL_DOUBLE_FLOAT_P(obj);
    case TS_FLOAT:        return CL_FLOATP(obj);
    case TS_REAL:         return CL_REALP(obj);
    case TS_COMPLEX:      return CL_COMPLEX_P(obj);
    case TS_NUMBER:       return CL_NUMBER_P(obj);
    case TS_CHARACTER:    return CL_CHAR_P(obj);
#ifdef CL_WIDE_STRINGS
    case TS_BASE_CHAR:
        return CL_CHAR_P(obj) && CL_CHAR_VAL(obj) <= 255;
    case TS_STANDARD_CHAR:
        return CL_CHAR_P(obj) && CL_CHAR_VAL(obj) < 128;
    case TS_EXTENDED_CHAR:
        return CL_CHAR_P(obj) && CL_CHAR_VAL(obj) > 255;
    case TS_STRING:
        return CL_ANY_STRING_P(obj) || CL_STRING_VECTOR_P(obj);
    case TS_SIMPLE_STRING: return CL_ANY_STRING_P(obj);
#else
    case TS_BASE_CHAR:     return CL_CHAR_P(obj);
    case TS_STANDARD_CHAR: return CL_CHAR_P(obj);
    case TS_EXTENDED_CHAR: return 0;
    case TS_STRING:
        return CL_STRING_P(obj) || CL_STRING_VECTOR_P(obj);
    case TS_SIMPLE_STRING: return CL_STRING_P(obj);
#endif
    case TS_BASE_STRING:
        return CL_STRING_P(obj) || CL_STRING_VECTOR_P(obj);
    case TS_SIMPLE_BASE_STRING: return CL_STRING_P(obj);
    case TS_BIT_VECTOR:         return CL_BIT_VECTOR_P(obj);
    case TS_SIMPLE_BIT_VECTOR:
        if (!CL_BIT_VECTOR_P(obj)) return 0;
        { CL_BitVector *bv = (CL_BitVector *)CL_OBJ_TO_PTR(obj);
          return !(bv->flags & (CL_VEC_FLAG_FILL_POINTER |
                                CL_VEC_FLAG_ADJUSTABLE)); }
    case TS_ARRAY:
    case TS_SIMPLE_ARRAY: {
        int simple = (code == TS_SIMPLE_ARRAY);
        if (CL_ANY_STRING_P(obj)) return 1;
        if (CL_BIT_VECTOR_P(obj)) {
            if (simple) {
                CL_BitVector *bv = (CL_BitVector *)CL_OBJ_TO_PTR(obj);
                return !(bv->flags & (CL_VEC_FLAG_FILL_POINTER |
                                      CL_VEC_FLAG_ADJUSTABLE));
            }
            return 1;
        }
        if (CL_BYTE_VECTOR_P(obj)) {
            if (simple) {
                CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(obj);
                return !(bv->flags & (CL_VEC_FLAG_FILL_POINTER |
                                      CL_VEC_FLAG_ADJUSTABLE));
            }
            return 1;
        }
        if (!CL_VECTOR_P(obj)) return 0;
        if (simple) {
            CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(obj);
            /* simple = no fill-pointer and not adjustable (multidim ok) */
            return !(v->flags & (CL_VEC_FLAG_FILL_POINTER |
                                 CL_VEC_FLAG_ADJUSTABLE));
        }
        return 1;
    }
    case TS_VECTOR:
        if (CL_ANY_STRING_P(obj)) return 1;
        if (CL_BIT_VECTOR_P(obj)) return 1;
        if (CL_BYTE_VECTOR_P(obj)) return 1;
        if (!CL_VECTOR_P(obj)) return 0;
        { CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(obj);
          return v->rank <= 1; }
    case TS_SIMPLE_VECTOR: {
        /* 1D, element-type T (not string), no fill-pointer, not adjustable */
        CL_Vector *v;
        if (!CL_VECTOR_P(obj)) return 0;
        v = (CL_Vector *)CL_OBJ_TO_PTR(obj);
        return v->rank <= 1 && v->flags == 0;
    }
    case TS_SEQUENCE:
        /* Only lists and one-dimensional arrays (vectors) are sequences;
         * a multidimensional array is NOT a sequence (CLHS "sequence"). */
        if (CL_NULL_P(obj) || CL_CONS_P(obj) || CL_ANY_STRING_P(obj) ||
            CL_BIT_VECTOR_P(obj) || CL_BYTE_VECTOR_P(obj))
            return 1;
        if (CL_VECTOR_P(obj)) {
            CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(obj);
            return v->rank <= 1;
        }
        return 0;
    case TS_FUNCTION:
        return CL_FUNCTION_OBJ_P(obj) || cl_funcallable_instance_p(obj);
    case TS_GENERIC_FUNCTION:
        return cl_funcallable_instance_p(obj);
    case TS_COMPILED_FUNCTION:
        return CL_CLOSURE_P(obj) || CL_BYTECODE_P(obj);
    case TS_HASH_TABLE: return CL_HASHTABLE_P(obj);
    case TS_PACKAGE:    return CL_PACKAGE_P(obj);
    case TS_RESTART:    return CL_RESTART_P(obj);
    case TS_SYNONYM_STREAM:
        if (!CL_STREAM_P(obj)) return 0;
        return ((CL_Stream *)CL_OBJ_TO_PTR(obj))->stream_type
               == CL_STREAM_SYNONYM;
    case TS_FILE_STREAM:
        if (!CL_STREAM_P(obj)) return 0;
        { uint32_t st = ((CL_Stream *)CL_OBJ_TO_PTR(obj))->stream_type;
          return st == CL_STREAM_FILE || st == CL_STREAM_CONSOLE; }
    case TS_STRING_STREAM:
        if (!CL_STREAM_P(obj)) return 0;
        return ((CL_Stream *)CL_OBJ_TO_PTR(obj))->stream_type
               == CL_STREAM_STRING;
    case TS_TWO_WAY_STREAM:
        if (!CL_STREAM_P(obj)) return 0;
        return ((CL_Stream *)CL_OBJ_TO_PTR(obj))->stream_type
               == CL_STREAM_TWO_WAY;
    case TS_BROADCAST_STREAM:
        if (!CL_STREAM_P(obj)) return 0;
        return ((CL_Stream *)CL_OBJ_TO_PTR(obj))->stream_type
               == CL_STREAM_BROADCAST;
    case TS_CONCATENATED_STREAM:
        if (!CL_STREAM_P(obj)) return 0;
        return ((CL_Stream *)CL_OBJ_TO_PTR(obj))->stream_type
               == CL_STREAM_CONCATENATED;
    case TS_ECHO_STREAM:
        if (!CL_STREAM_P(obj)) return 0;
        return ((CL_Stream *)CL_OBJ_TO_PTR(obj))->stream_type
               == CL_STREAM_ECHO;
    case TS_RANDOM_STATE:     return CL_RANDOM_STATE_P(obj);
    case TS_PATHNAME:         return CL_PATHNAME_P(obj);
    case TS_LOGICAL_PATHNAME: return 0;
    case TS_READTABLE:
        /* Readtables are fixnum pool indices */
        if (!CL_FIXNUM_P(obj)) return 0;
        { int idx = CL_FIXNUM_VAL(obj);
          return idx >= 0 && idx < CL_RT_POOL_SIZE; }
    case TS_STRUCTURE: return CL_STRUCT_P(obj);
    case TS_STREAM: {
        if (CL_STREAM_P(obj)) return 1;
        /* Gray streams are CLOS instances (CL_STRUCT_P); check CPL for
         * GRAY::FUNDAMENTAL-STREAM.  The symbol is looked up lazily so
         * that a core build without gray-streams.lisp returns 0 cleanly. */
        if (CL_STRUCT_P(obj)) {
            extern int cl_clos_type_matches(CL_Obj obj_type, CL_Obj test_type);
            CL_Struct *st;
            /* Guard the lazy init with the write lock so only one thread
             * performs the lookup; subsequent threads will find the cached
             * value and skip the block entirely. */
            cl_tables_wrlock();
            if (CL_NULL_P(TYPE_SYM_GRAY_FUNDAMENTAL_STREAM)) {
                CL_Obj gray_pkg = cl_find_package("GRAY", 4);
                if (!CL_NULL_P(gray_pkg)) {
                    CL_Obj sym = cl_package_find_symbol(
                        "FUNDAMENTAL-STREAM", 18, gray_pkg);
                    if (!CL_NULL_P(sym) && CL_SYMBOL_P(sym))
                        TYPE_SYM_GRAY_FUNDAMENTAL_STREAM = sym;
                }
            }
            cl_tables_rwunlock();
            if (!CL_NULL_P(TYPE_SYM_GRAY_FUNDAMENTAL_STREAM)) {
                st = (CL_Struct *)CL_OBJ_TO_PTR(obj);
                return cl_clos_type_matches(st->type_desc,
                                            TYPE_SYM_GRAY_FUNDAMENTAL_STREAM);
            }
        }
        return 0;
    }
    default: break;
    }
    return 0;
}

/* Everything a standard type name does not answer: structure types,
 * condition types, DEFTYPE expanders and CLOS classes.  This is the path
 * sento's (declare (type <class>) ...) takes, and it used to run the whole
 * strcmp cascade above before reaching its first real check. */
static int typep_user_type(CL_Obj obj, CL_Obj type_sym)
{
    /* Structure types — check hierarchy for struct objects */
    {
        extern int cl_is_struct_type(CL_Obj type_sym);
        extern int cl_struct_type_matches(CL_Obj obj_type, CL_Obj test_type);
        extern int cl_clos_type_matches(CL_Obj obj_type, CL_Obj test_type);
        if (cl_is_struct_type(type_sym)) {
            if (!CL_STRUCT_P(obj)) return 0;
            {
                CL_Struct *st = (CL_Struct *)CL_OBJ_TO_PTR(obj);
                /* Try single-parent struct chain first, then CLOS CPL */
                if (cl_struct_type_matches(st->type_desc, type_sym))
                    return 1;
                return cl_clos_type_matches(st->type_desc, type_sym);
            }
        }
    }

    /* Condition types — check hierarchy for condition objects,
     * return 0 for non-condition objects with condition type specs */
    {
        extern int cl_condition_type_matches(CL_Obj cond_type, CL_Obj handler_type);
        extern int cl_is_condition_type(CL_Obj type_sym);
        if (cl_is_condition_type(type_sym)) {
            if (!CL_CONDITION_P(obj)) return 0;
            {
                CL_Condition *cond = (CL_Condition *)CL_OBJ_TO_PTR(obj);
                return cl_condition_type_matches(cond->type_name, type_sym);
            }
        }
    }

    /* Check user-defined types */
    {
        CL_Obj expander = cl_get_type_expander(type_sym);
        if (!CL_NULL_P(expander)) {
            /* Call expander with no args, get expanded type spec.
             * obj must survive the expander's allocations. */
            CL_Obj expanded;
            CL_GC_PROTECT(obj);
            expanded = cl_vm_apply(expander, NULL, 0);
            CL_GC_UNPROTECT(1);
            return typep_check(obj, expanded);
        }
    }

    /* CLOS class table fallback — handles bootstrap classes (e.g.
     * standard-object) that are in the class table but not the struct
     * registry, and any other CLOS-only type specifiers. */
    {
        extern int cl_clos_type_matches(CL_Obj obj_type, CL_Obj test_type);
        extern int cl_clos_class_exists(CL_Obj name);
        if (cl_clos_class_exists(type_sym)) {
            if (!CL_STRUCT_P(obj)) return 0;
            {
                CL_Struct *st = (CL_Struct *)CL_OBJ_TO_PTR(obj);
                return cl_clos_type_matches(st->type_desc, type_sym);
            }
        }
    }

    /* Compatibility net for a symbol that carries a standard type NAME but
     * is not the COMMON-LISP symbol — one interned in a package that does
     * not inherit CL, or reached through a FASL read under such a
     * *PACKAGE*.  The old name-keyed cascade answered those; keep doing so,
     * on this cold path only, rather than signalling a type error. */
    {
        uint8_t code = typep_code_for_name(cl_symbol_name(type_sym));
        if (code != TS_NONE)
            return typep_builtin_code(obj, code);
    }

    cl_error(CL_ERR_TYPE, "TYPEP: unknown type specifier %s",
             cl_symbol_name(type_sym));
    return 0;
}

/* TYPEP on a bare symbol specifier.  One flags load decides between the
 * standard-type switch and the user-type path; see the block comment on
 * typep_name_codes above. */
static int typep_symbol(CL_Obj obj, CL_Obj type_sym)
{
    uint8_t code;

    /* CL_NIL is the tag 0 — the type NIL, which nothing is of.  Handled
     * here so the flags read below never dereferences the null tag. */
    if (CL_NULL_P(type_sym))
        return 0;
    if (!CL_SYMBOL_P(type_sym))
        return typep_user_type(obj, type_sym);

    code = (uint8_t)CL_SYM_TYPECODE((CL_Symbol *)CL_OBJ_TO_PTR(type_sym));
    if (code != TS_NONE)
        return typep_builtin_code(obj, code);
    return typep_user_type(obj, type_sym);
}

/* --- Numeric range type check: (type [low [high]]) ---
 * low/high can be: * (unbounded), number (inclusive), (number) (exclusive) */
static int check_numeric_range(CL_Obj obj, CL_Obj head, CL_Obj args)
{
    CL_Obj low_spec, high_spec;
    int cmp;

    /* First check if obj is the right base type */
    if (head == TYPE_SYM_INTEGER || head == TYPE_SYM_FIXNUM ||
        head == TYPE_SYM_BIGNUM) {
        if (!CL_INTEGER_P(obj)) return 0;
        if (head == TYPE_SYM_FIXNUM && !CL_FIXNUM_P(obj)) return 0;
        if (head == TYPE_SYM_BIGNUM && !CL_BIGNUM_P(obj)) return 0;
    } else if (head == TYPE_SYM_RATIO) {
        if (!CL_RATIO_P(obj)) return 0;
    } else if (head == TYPE_SYM_RATIONAL) {
        if (!CL_RATIONAL_P(obj)) return 0;
    } else if (head == TYPE_SYM_FLOAT) {
        if (!CL_FLOATP(obj)) return 0;
    } else if (head == TYPE_SYM_SINGLE_FLOAT || head == TYPE_SYM_SHORT_FLOAT) {
        if (!CL_SINGLE_FLOAT_P(obj)) return 0;
    } else if (head == TYPE_SYM_DOUBLE_FLOAT || head == TYPE_SYM_LONG_FLOAT) {
        if (!CL_DOUBLE_FLOAT_P(obj)) return 0;
    } else if (head == TYPE_SYM_REAL) {
        if (!CL_REALP(obj)) return 0;
    } else if (head == TYPE_SYM_NUMBER) {
        if (!CL_NUMBER_P(obj)) return 0;
    } else {
        return 0;
    }

    /* No bounds specified: (type) = just the base type check */
    if (CL_NULL_P(args)) return 1;

    /* Parse low bound */
    low_spec = cl_car(args);
    args = cl_cdr(args);

    /* Check low bound.  cl_arith_compare allocates on the ratio and
     * float-vs-rational paths — obj and the args cursor are read again
     * for the high bound, so root them across it. */
    if (low_spec != TYPE_SYM_STAR && !CL_NULL_P(low_spec)) {
        int fail;
        CL_GC_PROTECT(obj);
        CL_GC_PROTECT(args);
        if (CL_CONS_P(low_spec)) {
            /* (number) = exclusive lower bound */
            CL_Obj bound = cl_car(low_spec);
            cmp = cl_arith_compare(obj, bound);
            fail = (cmp <= 0);       /* obj <= bound: fail */
        } else {
            /* number = inclusive lower bound */
            cmp = cl_arith_compare(obj, low_spec);
            fail = (cmp < 0);        /* obj < bound: fail */
        }
        CL_GC_UNPROTECT(2);
        if (fail) return 0;
    }

    /* No high bound specified */
    if (CL_NULL_P(args)) return 1;

    /* Parse high bound */
    high_spec = cl_car(args);

    /* Check high bound */
    if (high_spec != TYPE_SYM_STAR && !CL_NULL_P(high_spec)) {
        if (CL_CONS_P(high_spec)) {
            /* (number) = exclusive upper bound */
            CL_Obj bound = cl_car(high_spec);
            cmp = cl_arith_compare(obj, bound);
            if (cmp >= 0) return 0;  /* obj >= bound: fail */
        } else {
            /* number = inclusive upper bound */
            cmp = cl_arith_compare(obj, high_spec);
            if (cmp > 0) return 0;   /* obj > bound: fail */
        }
    }

    return 1;
}

/* --- Recursive typep for compound type specifiers --- */

static int typep_check(CL_Obj obj, CL_Obj type_spec)
{
    /* Symbol type specifier */
    if (CL_SYMBOL_P(type_spec) || CL_NULL_P(type_spec)) {
        return typep_symbol(obj, type_spec);
    }

    /* Compound type specifier: (head args...) */
    if (CL_CONS_P(type_spec)) {
        CL_Obj head = cl_car(type_spec);
        CL_Obj args = cl_cdr(type_spec);

        /* (values ...) is a values type specifier (CLHS 4.2.3); only valid in
         * THE / FTYPE contexts, not as a typep argument.  Be lenient and treat
         * a typep on a values-type-spec as always true so that pre-existing
         * FASLs containing OP_ASSERT_TYPE for a values spec don't break. */
        if (CL_SYMBOL_P(head) &&
            head == cl_intern_in("VALUES", 6, cl_package_cl)) {
            (void)args;
            return 1;
        }

        /* (or t1 t2 ...) — the recursion allocates whenever a branch hits a
         * deftype expander / satisfies / ratio range, so obj and the walk
         * cursor must be rooted across it. */
        if (head == TYPE_SYM_OR) {
            CL_Obj rest = args;
            CL_GC_PROTECT(obj);
            CL_GC_PROTECT(rest);
            while (!CL_NULL_P(rest)) {
                if (typep_check(obj, cl_car(rest))) {
                    CL_GC_UNPROTECT(2);
                    return 1;
                }
                rest = cl_cdr(rest);
            }
            CL_GC_UNPROTECT(2);
            return 0;
        }

        /* (and t1 t2 ...) */
        if (head == TYPE_SYM_AND) {
            CL_Obj rest = args;
            CL_GC_PROTECT(obj);
            CL_GC_PROTECT(rest);
            while (!CL_NULL_P(rest)) {
                if (!typep_check(obj, cl_car(rest))) {
                    CL_GC_UNPROTECT(2);
                    return 0;
                }
                rest = cl_cdr(rest);
            }
            CL_GC_UNPROTECT(2);
            return 1;
        }

        /* (not t) */
        if (head == TYPE_SYM_NOT) {
            if (CL_NULL_P(args))
                cl_error(CL_ERR_ARGS, "TYPEP: (NOT) requires a type argument");
            return !typep_check(obj, cl_car(args));
        }

        /* (member o1 o2 ...) */
        if (head == TYPE_SYM_MEMBER) {
            CL_Obj rest = args;
            while (!CL_NULL_P(rest)) {
                if (obj == cl_car(rest))  /* eql comparison */
                    return 1;
                rest = cl_cdr(rest);
            }
            return 0;
        }

        /* (eql o) */
        if (head == TYPE_SYM_EQL) {
            if (CL_NULL_P(args))
                cl_error(CL_ERR_ARGS, "TYPEP: (EQL) requires an argument");
            return obj == cl_car(args);
        }

        /* (satisfies pred) */
        if (head == TYPE_SYM_SATISFIES) {
            CL_Obj pred_name;
            CL_Symbol *pred_sym;
            CL_Obj pred_fn, result;
            if (CL_NULL_P(args))
                cl_error(CL_ERR_ARGS, "TYPEP: (SATISFIES) requires a predicate name");
            pred_name = cl_car(args);
            if (!CL_SYMBOL_P(pred_name))
                cl_error(CL_ERR_TYPE, "TYPEP: SATISFIES predicate must be a symbol");
            pred_sym = (CL_Symbol *)CL_OBJ_TO_PTR(pred_name);
            pred_fn = pred_sym->function;
            if (CL_NULL_P(pred_fn) || pred_fn == CL_UNBOUND) {
                pred_fn = cl_symbol_value(pred_name);
                if (CL_NULL_P(pred_fn) || pred_fn == CL_UNBOUND)
                    cl_error_cell(CL_ERR_UNDEFINED, pred_name, "TYPEP: undefined predicate %s",
                             cl_symbol_name(pred_name));
            }
            result = cl_vm_apply(pred_fn, &obj, 1);
            return !CL_NULL_P(result);
        }

        /* Numeric range type specifiers: (integer low high), (real low high), etc. */
        if (head == TYPE_SYM_INTEGER || head == TYPE_SYM_FIXNUM ||
            head == TYPE_SYM_BIGNUM || head == TYPE_SYM_RATIO ||
            head == TYPE_SYM_RATIONAL || head == TYPE_SYM_REAL ||
            head == TYPE_SYM_FLOAT || head == TYPE_SYM_SINGLE_FLOAT ||
            head == TYPE_SYM_SHORT_FLOAT || head == TYPE_SYM_DOUBLE_FLOAT ||
            head == TYPE_SYM_LONG_FLOAT || head == TYPE_SYM_NUMBER) {
            return check_numeric_range(obj, head, args);
        }

        /* (simple-vector size) */
        if (head == TYPE_SYM_SIMPLE_VECTOR) {
            if (!typep_symbol(obj, head)) return 0;
            if (!CL_NULL_P(args)) {
                CL_Obj size_spec = cl_car(args);
                if (CL_FIXNUM_P(size_spec)) {
                    CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(obj);
                    return v->length == (uint32_t)CL_FIXNUM_VAL(size_spec);
                }
                /* * means any size */
            }
            return 1;
        }

        /* (vector element-type size) */
        if (head == TYPE_SYM_VECTOR) {
            if (!typep_symbol(obj, head)) return 0;
            /* Element-type constraint: the vector matches iff its actual
             * upgraded element class equals the class ET upgrades to —
             * through deftype aliases (elt_spec_class expands them; the old
             * inline checks didn't, so flexi-streams' (VECTOR OCTET) never
             * matched a packed byte vector).  This is also what makes
             * (TYPEP fixnum-arr '(VECTOR T)) NIL (serapeum's VECT-TYPE)
             * while a general T vector still matches. */
            if (!CL_NULL_P(args)) {
                CL_Obj et = cl_car(args);
                int want;
                /* elt_spec_class may run deftype expanders → compaction can
                 * move obj/args while held in these C locals. */
                CL_GC_PROTECT(obj);
                CL_GC_PROTECT(args);
                want = elt_spec_class(et);
                CL_GC_UNPROTECT(2);
                if (want != AEC_WILD && obj_elt_class(obj) != want)
                    return 0;
            }
            if (!CL_NULL_P(args) && !CL_NULL_P(cl_cdr(args))) {
                CL_Obj size_spec = cl_car(cl_cdr(args));
                if (CL_FIXNUM_P(size_spec)) {
                    uint32_t len;
                    if (CL_BYTE_VECTOR_P(obj))
                        len = ((CL_ByteVector *)CL_OBJ_TO_PTR(obj))->length;
                    else if (CL_BIT_VECTOR_P(obj))
                        len = ((CL_BitVector *)CL_OBJ_TO_PTR(obj))->length;
                    else if (CL_ANY_STRING_P(obj))
                        len = cl_string_length(obj);
                    else
                        len = ((CL_Vector *)CL_OBJ_TO_PTR(obj))->length;
                    return len == (uint32_t)CL_FIXNUM_VAL(size_spec);
                }
            }
            return 1;
        }

        /* (string size), (simple-string size), (base-string size),
           (simple-base-string size) */
        if (head == TYPE_SYM_STRING || head == TYPE_SYM_SIMPLE_STRING ||
            head == TYPE_SYM_BASE_STRING || head == TYPE_SYM_SIMPLE_BASE_STRING) {
            if (!CL_ANY_STRING_P(obj)) return 0;
            if (!CL_NULL_P(args)) {
                CL_Obj size_spec = cl_car(args);
                if (CL_FIXNUM_P(size_spec)) {
                    CL_String *s = (CL_String *)CL_OBJ_TO_PTR(obj);
                    return s->length == (uint32_t)CL_FIXNUM_VAL(size_spec);
                }
            }
            return 1;
        }

        /* (bit-vector size), (simple-bit-vector size) */
        if (head == TYPE_SYM_BIT_VECTOR || head == TYPE_SYM_SIMPLE_BIT_VECTOR) {
            if (!CL_BIT_VECTOR_P(obj)) return 0;
            if (!CL_NULL_P(args)) {
                CL_Obj size_spec = cl_car(args);
                if (CL_FIXNUM_P(size_spec)) {
                    CL_BitVector *bv = (CL_BitVector *)CL_OBJ_TO_PTR(obj);
                    return bv->length == (uint32_t)CL_FIXNUM_VAL(size_spec);
                }
            }
            return 1;
        }

        /* (array element-type dims), (simple-array element-type dims) */
        if (head == TYPE_SYM_ARRAY || head == TYPE_SYM_SIMPLE_ARRAY) {
            if (!typep_symbol(obj, head)) return 0;
            /* Element-type constraint (CLHS: the object matches only if its
             * actual upgraded element type equals (upgraded-array-element-type
             * ET)).  elt_spec_class maps ET to the storage class MAKE-ARRAY
             * would build — through deftype aliases — and obj_elt_class reads
             * the class the object actually stores.  Without this check a
             * general T vector spuriously satisfied (simple-array bit (*)),
             * which made serapeum's WITH-TYPE-DISPATCH pick the BIT branch
             * and call SBIT on a non-bit vector; without the alias expansion
             * (ARRAY OCTET *) never matched a packed byte vector, killing
             * flexi-streams' STRING-TO-OCTETS. */
            if (!CL_NULL_P(args)) {
                CL_Obj et = cl_car(args);
                int want;
                /* elt_spec_class may run deftype expanders → compaction can
                 * move obj/args while held in these C locals. */
                CL_GC_PROTECT(obj);
                CL_GC_PROTECT(args);
                want = elt_spec_class(et);
                CL_GC_UNPROTECT(2);
                if (want != AEC_WILD && obj_elt_class(obj) != want)
                    return 0;
                /* Rank/dimensions, if provided. */
                if (!CL_NULL_P(cl_cdr(args))) {
                    CL_Obj dims = cl_car(cl_cdr(args));
                    if (CL_FIXNUM_P(dims)) {
                        /* dims is a rank; bit-vectors and strings are rank 1. */
                        uint32_t rank = 1;
                        if (CL_VECTOR_P(obj))
                            rank = ((CL_Vector *)CL_OBJ_TO_PTR(obj))->rank;
                        return rank == (uint32_t)CL_FIXNUM_VAL(dims);
                    }
                }
            }
            return 1;
        }

        /* (unsigned-byte n) — integer [0, 2^n) */
        if (CL_SYMBOL_P(head) &&
            strcmp(cl_symbol_name(head), "UNSIGNED-BYTE") == 0) {
            if (!CL_FIXNUM_P(obj) && !CL_BIGNUM_P(obj)) return 0;
            if (cl_arith_minusp(obj)) return 0;
            if (!CL_NULL_P(args) && CL_FIXNUM_P(cl_car(args))) {
                int32_t bits = CL_FIXNUM_VAL(cl_car(args));
                CL_Obj limit;
                int res;
                /* The ash can allocate a bignum limit — obj (itself a
                 * bignum here) must be forwarded across it. */
                CL_GC_PROTECT(obj);
                limit = cl_arith_ash(CL_MAKE_FIXNUM(1), CL_MAKE_FIXNUM(bits));
                res = cl_arith_minusp(cl_arith_sub(obj, limit));
                CL_GC_UNPROTECT(1);
                return res;
            }
            return 1;
        }

        /* (mod n) — non-negative integer [0, n)  (CLHS 4.2.3 / type MOD) */
        if (CL_SYMBOL_P(head) &&
            strcmp(cl_symbol_name(head), "MOD") == 0) {
            if (!CL_FIXNUM_P(obj) && !CL_BIGNUM_P(obj)) return 0;
            if (cl_arith_minusp(obj)) return 0;
            if (!CL_NULL_P(args)) {
                CL_Obj n = cl_car(args);
                if (CL_FIXNUM_P(n) || CL_BIGNUM_P(n)) {
                    /* obj < n */
                    return cl_arith_minusp(cl_arith_sub(obj, n));
                }
            }
            return 1;
        }

        /* (signed-byte n) — integer [-2^(n-1), 2^(n-1)) */
        if (CL_SYMBOL_P(head) &&
            strcmp(cl_symbol_name(head), "SIGNED-BYTE") == 0) {
            if (!CL_FIXNUM_P(obj) && !CL_BIGNUM_P(obj)) return 0;
            if (!CL_NULL_P(args) && CL_FIXNUM_P(cl_car(args))) {
                int32_t bits = CL_FIXNUM_VAL(cl_car(args));
                CL_Obj half, neg_half;
                int res;
                /* obj crosses the ash/negate/sub allocations; half is read
                 * again for the second sub after negate allocates. */
                CL_GC_PROTECT(obj);
                half = cl_arith_ash(CL_MAKE_FIXNUM(1),
                                    CL_MAKE_FIXNUM(bits - 1));
                CL_GC_PROTECT(half);
                neg_half = cl_arith_negate(half);
                /* obj >= -half && obj < half */
                res = !cl_arith_minusp(cl_arith_sub(obj, neg_half));
                if (res)
                    res = cl_arith_minusp(cl_arith_sub(obj, half));
                CL_GC_UNPROTECT(2);
                return res;
            }
            return 1;
        }

        /* (integer low high) — integer in range */
        if (CL_SYMBOL_P(head) &&
            strcmp(cl_symbol_name(head), "INTEGER") == 0) {
            if (!CL_FIXNUM_P(obj) && !CL_BIGNUM_P(obj)) return 0;
            /* Check bounds if provided.  The low-bound sub allocates for
             * bignums, so obj and the args cursor must survive it before
             * the high bound is walked/compared. */
            if (!CL_NULL_P(args)) {
                CL_Obj low = cl_car(args);
                int fail = 0;
                CL_GC_PROTECT(obj);
                CL_GC_PROTECT(args);
                if (CL_FIXNUM_P(low) || CL_BIGNUM_P(low)) {
                    if (cl_arith_minusp(cl_arith_sub(obj, low))) fail = 1;
                }
                if (!fail && !CL_NULL_P(cl_cdr(args))) {
                    CL_Obj high = cl_car(cl_cdr(args));
                    if (CL_FIXNUM_P(high) || CL_BIGNUM_P(high)) {
                        if (cl_arith_minusp(cl_arith_sub(high, obj))) fail = 1;
                    }
                }
                CL_GC_UNPROTECT(2);
                return !fail;
            }
            return 1;
        }

        /* (complex [part-type]) — complex with parts of part-type.
         * Wildcard * matches anything. Per CLHS, the part-type is matched
         * against the upgraded element type, so we accept a part if it's
         * typep part-type. */
        if (CL_SYMBOL_P(head) &&
            strcmp(cl_symbol_name(head), "COMPLEX") == 0) {
            CL_Complex *cx;
            CL_Obj part_type;
            if (!CL_COMPLEX_P(obj)) return 0;
            if (CL_NULL_P(args)) return 1;
            part_type = cl_car(args);
            if (CL_SYMBOL_P(part_type) &&
                strcmp(cl_symbol_name(part_type), "*") == 0)
                return 1;
            cx = (CL_Complex *)CL_OBJ_TO_PTR(obj);
            {
                /* Snapshot both parts first: the recursive check can
                 * allocate (deftype expansion), leaving cx dangling. */
                CL_Obj re = cx->realpart;
                CL_Obj im = cx->imagpart;
                int res;
                CL_GC_PROTECT(im);
                CL_GC_PROTECT(part_type);
                res = typep_check(re, part_type);
                if (res)
                    res = typep_check(im, part_type);
                CL_GC_UNPROTECT(2);
                return res;
            }
        }

        /* (cons [car-type [cdr-type]]) — cons cell type */
        if (CL_SYMBOL_P(head) &&
            strcmp(cl_symbol_name(head), "CONS") == 0) {
            if (!CL_CONS_P(obj)) return 0;
            /* Optionally check car/cdr types (symbol or compound).
             * obj and args are re-read after the car-type recursion,
             * which can allocate — root them across it. */
            if (!CL_NULL_P(args)) {
                int fail = 0;
                CL_Obj car_type;
                CL_GC_PROTECT(obj);
                CL_GC_PROTECT(args);
                car_type = cl_car(args);
                /* Skip only the * wildcard */
                if (!(CL_SYMBOL_P(car_type) &&
                      strcmp(cl_symbol_name(car_type), "*") == 0)) {
                    if (!typep_check(cl_car(obj), car_type)) fail = 1;
                }
                if (!fail && !CL_NULL_P(cl_cdr(args))) {
                    CL_Obj cdr_type = cl_car(cl_cdr(args));
                    if (!(CL_SYMBOL_P(cdr_type) &&
                          strcmp(cl_symbol_name(cdr_type), "*") == 0)) {
                        if (!typep_check(cl_cdr(obj), cdr_type)) fail = 1;
                    }
                }
                CL_GC_UNPROTECT(2);
                if (fail) return 0;
            }
            return 1;
        }

        /* User-defined parameterized type: (my-type args...) */
        if (CL_SYMBOL_P(head)) {
            CL_Obj expander = cl_get_type_expander(head);
            if (!CL_NULL_P(expander)) {
                CL_Obj arg_array[16];
                int nargs = 0;
                CL_Obj rest = args;
                CL_Obj expanded;
                while (!CL_NULL_P(rest) && nargs < 16) {
                    arg_array[nargs++] = cl_car(rest);
                    rest = cl_cdr(rest);
                }
                /* obj must survive the expander (arg_array is copied onto
                 * the rooted VM stack by cl_vm_apply itself). */
                CL_GC_PROTECT(obj);
                expanded = cl_vm_apply(expander, arg_array, nargs);
                CL_GC_UNPROTECT(1);
                return typep_check(obj, expanded);
            }
        }

        {
            char buf[128];
            cl_prin1_to_string(head, buf, sizeof(buf));
            cl_error(CL_ERR_TYPE,
                     "TYPEP: invalid compound type specifier head: %s", buf);
        }
    }

    /* Class object as type specifier: extract class name (slot 0) */
    if (CL_STRUCT_P(type_spec)) {
        CL_Struct *st = (CL_Struct *)CL_OBJ_TO_PTR(type_spec);
        CL_Obj class_name = st->slots[0];
        if (CL_SYMBOL_P(class_name))
            return typep_symbol(obj, class_name);
    }

    {
        char buf[128];
        cl_prin1_to_string(type_spec, buf, sizeof(buf));
        cl_error(CL_ERR_TYPE, "TYPEP: invalid type specifier: %s", buf);
    }
    return 0;
}

/* --- Builtins --- */

static CL_Obj bi_type_of(CL_Obj *args, int n)
{
    const char *name;
    CL_UNUSED(n);
    /* For conditions, return the specific condition type name */
    if (CL_CONDITION_P(args[0])) {
        CL_Condition *cond = (CL_Condition *)CL_OBJ_TO_PTR(args[0]);
        return cond->type_name;
    }
    /* For structures, return the struct type name */
    if (CL_STRUCT_P(args[0])) {
        CL_Struct *st = (CL_Struct *)CL_OBJ_TO_PTR(args[0]);
        return st->type_desc;
    }
    /* For bit vectors */
    if (CL_BIT_VECTOR_P(args[0])) {
        CL_BitVector *bv = (CL_BitVector *)CL_OBJ_TO_PTR(args[0]);
        name = (bv->flags == 0) ? "SIMPLE-BIT-VECTOR" : "BIT-VECTOR";
        return cl_intern(name, (uint32_t)strlen(name));
    }
    /* For byte vectors: (VECTOR (UNSIGNED-BYTE 8/16) len) — a specifier the
     * typep compound-VECTOR path recognizes, so (typep x (type-of x)) holds. */
    if (CL_BYTE_VECTOR_P(args[0])) {
        CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(args[0]);
        uint32_t blen = bv->length;
        int is_signed = bv->is_signed;
        int nbits = bv->elt_shift ? 16 : 8;
        CL_Obj tail, et_tail, et, mid, vec_sym;
        tail = cl_cons(CL_MAKE_FIXNUM((int32_t)blen), CL_NIL);
        CL_GC_PROTECT(tail);
        et_tail = cl_cons(CL_MAKE_FIXNUM(nbits), CL_NIL);
        CL_GC_PROTECT(et_tail);
        et = is_signed ? cl_intern("SIGNED-BYTE", 11)
                       : cl_intern("UNSIGNED-BYTE", 13);
        et = cl_cons(et, et_tail);
        CL_GC_UNPROTECT(1); /* et_tail */
        mid = cl_cons(et, tail);
        CL_GC_UNPROTECT(1); /* tail */
        CL_GC_PROTECT(mid);
        vec_sym = cl_intern("VECTOR", 6);
        CL_GC_UNPROTECT(1);
        return cl_cons(vec_sym, mid);
    }
    /* For vectors/arrays, return specific type */
    if (CL_VECTOR_P(args[0])) {
        CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(args[0]);
        if (v->flags & CL_VEC_FLAG_STRING)
            name = "STRING";
        else if (v->rank > 1)
            name = !(v->flags & (CL_VEC_FLAG_FILL_POINTER | CL_VEC_FLAG_ADJUSTABLE))
                   ? "SIMPLE-ARRAY" : "ARRAY";
        else if (v->flags == 0)
            name = "SIMPLE-VECTOR";
        else
            name = "VECTOR";
        return cl_intern(name, (uint32_t)strlen(name));
    }
    name = cl_type_name(args[0]);
    return cl_intern(name, (uint32_t)strlen(name));
}

static CL_Obj bi_typep(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    return typep_check(args[0], args[1]) ? SYM_T : CL_NIL;
}

/* (upgraded-complex-part-type type &optional env) — the implementation's
 * upgraded element type for a complex of `type` parts.
 *
 * cl-amiga doesn't specialize complex storage; CL_Complex holds two
 * untagged CL_Obj slots that can hold any real number.  Per CLHS the
 * returned upgraded type just has to:
 *   - be a supertype of the requested type, and
 *   - be a subtype of REAL.
 *
 * We therefore return REAL for any real-subtype request.  This keeps
 * (subtypep type (upgraded-complex-part-type type)) → T,
 * which is what the ANSI tests in numbers/upgraded-complex-part-type.lsp
 * verify. */
static CL_Obj bi_upgraded_complex_part_type(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    /* Ignore optional environment arg (args[1]); cl-amiga has no
     * environment-specific upgrades. */
    return TYPE_SYM_REAL;
}

static CL_Obj bi_coerce(CL_Obj *args, int n)
{
    CL_Obj obj = args[0];
    CL_Obj result_type = args[1];
    const char *tname;
    uint8_t coerce_vec_elt_code = CL_VEC_ELT_T; /* specialized elt type for VECTOR result */
    int coerce_bytevec_signed = -1; /* >=0: build a packed byte vector (0=unsigned, 1=signed) */
    int coerce_bytevec_shift = 0;   /* packed element width: 0 = 8-bit, 1 = 16-bit */
    CL_UNUSED(n);

    /* Expand user deftypes in the result-type so e.g. babel's
     * UNICODE-STRING -> (vector unicode-char *) is understood. */
    {
        int guard = 16;
        while (guard-- > 0 && CL_SYMBOL_P(result_type)) {
            CL_Obj ex = cl_get_type_expander(result_type);
            if (CL_NULL_P(ex)) break;
            result_type = cl_vm_apply(ex, NULL, 0);
        }
    }
    /* The args[0] SLOT is rooted and forwarded (VM stack, or protected by
     * the recursive complex call sites below), but the obj LOCAL COPY is
     * stale after the expander applies above — re-read it. */
    obj = args[0];

    /* Handle compound type specifiers like (simple-array fixnum (*)):
     * extract the base type name and coerce as that sequence type. */
    if (CL_CONS_P(result_type)) {
        extern CL_Obj cl_package_cl;
        CL_Obj head = cl_car(result_type);
        if (CL_SYMBOL_P(head)) {
            const char *hname = cl_symbol_name(head);
            /* (simple-array ...) / (array ...) / (vector ...) / (simple-vector ...) */
            if (strcmp(hname, "SIMPLE-ARRAY") == 0 || strcmp(hname, "ARRAY") == 0 ||
                strcmp(hname, "VECTOR") == 0 || strcmp(hname, "SIMPLE-VECTOR") == 0) {
                /* A character element type means a string, a BIT element
                 * type means a bit-vector, a byte-range element type means a
                 * packed byte vector; otherwise a general vector.  Classified
                 * by the shared MAKE-ARRAY/TYPEP classifier so the result's
                 * element type agrees with what those report — and so deftype
                 * aliases like OCTET expand here too. */
                /* eargs is re-read after cl_classify_array_elt_type and
                 * cl_classify_vec_elt_code, both of which can run an
                 * elt-type deftype expander via cl_vm_apply — root it. */
                CL_Obj eargs = cl_cdr(result_type);
                int is_char = 0, is_wide = 0, is_bit = 0;
                int is_u8 = 0, is_s8 = 0, is_u16 = 0, is_s16 = 0;
                CL_GC_PROTECT(eargs);
                if (!CL_NULL_P(eargs))
                    cl_classify_array_elt_type(cl_car(eargs), 16,
                                               &is_char, &is_wide, &is_bit,
                                               &is_u8, &is_s8, &is_u16, &is_s16);
                if (is_char)
                    result_type = cl_intern_in("STRING", 6, cl_package_cl);
                else if (is_bit)
                    result_type = cl_intern_in("BIT-VECTOR", 10, cl_package_cl);
                else if (is_u8 || is_s8 || is_u16 || is_s16) {
                    coerce_bytevec_signed = (is_s8 || is_s16) ? 1 : 0;
                    coerce_bytevec_shift  = (is_u16 || is_s16) ? 1 : 0;
                    result_type = cl_intern_in("VECTOR", 6, cl_package_cl);
                }
                else {
                    /* General vector: preserve a specialized numeric element
                     * type (e.g. (vector single-float)) on the result. */
                    if (!CL_NULL_P(eargs))
                        coerce_vec_elt_code = cl_classify_vec_elt_code(cl_car(eargs), 8);
                    result_type = cl_intern_in("VECTOR", 6, cl_package_cl);
                }
                CL_GC_UNPROTECT(1);
                obj = args[0];  /* re-read after the expander applies */
            } else if (strcmp(hname, "COMPLEX") == 0) {
                /* (coerce x (complex part-type)) — build a complex whose
                 * realpart and imagpart are coerced to part-type. */
                CL_Obj rest = cl_cdr(result_type);
                CL_Obj part_type = CL_CONS_P(rest) ? cl_car(rest)
                                                   : cl_intern_in("REAL", 4,
                                                                  cl_package_cl);
                CL_Obj re_obj, im_obj;
                CL_Obj coerce_args[2];
                if (CL_COMPLEX_P(obj)) {
                    CL_Complex *cx = (CL_Complex *)CL_OBJ_TO_PTR(obj);
                    re_obj = cx->realpart;
                    im_obj = cx->imagpart;
                } else if (CL_REALP(obj)) {
                    re_obj = obj;
                    im_obj = CL_MAKE_FIXNUM(0);
                } else {
                    cl_signal_type_error(obj, "NUMBER", "COERCE");
                    return CL_NIL;
                }
                CL_GC_PROTECT(part_type);
                CL_GC_PROTECT(im_obj);
                /* coerce_args is a C array, NOT the VM stack — the recursive
                 * bi_coerce re-reads its args[] slots after allocating, so
                 * the slots themselves must be rooted for each call. */
                coerce_args[0] = re_obj; coerce_args[1] = part_type;
                CL_GC_PROTECT(coerce_args[0]);
                CL_GC_PROTECT(coerce_args[1]);
                re_obj = bi_coerce(coerce_args, 2);
                CL_GC_UNPROTECT(2);
                CL_GC_PROTECT(re_obj);
                coerce_args[0] = im_obj; coerce_args[1] = part_type;
                CL_GC_PROTECT(coerce_args[0]);
                CL_GC_PROTECT(coerce_args[1]);
                im_obj = bi_coerce(coerce_args, 2);
                CL_GC_UNPROTECT(5);
                CL_GC_PROTECT(re_obj);
                CL_GC_PROTECT(im_obj);
                {
                    CL_Obj r = cl_make_complex(re_obj, im_obj);
                    CL_GC_UNPROTECT(2);
                    return r;
                }
            } else {
                /* For other compound types, try the base type name */
                result_type = head;
            }
        } else {
            cl_error(CL_ERR_TYPE, "COERCE: invalid result type");
        }
    }

    if (!CL_SYMBOL_P(result_type) && !CL_NULL_P(result_type))
        cl_error(CL_ERR_TYPE, "COERCE: result type must be a symbol");

    tname = cl_symbol_name(result_type);

    /* (coerce x 't) — identity */
    if (strcmp(tname, "T") == 0)
        return obj;

    /* (coerce x 'character) */
    if (strcmp(tname, "CHARACTER") == 0) {
        if (CL_CHAR_P(obj)) return obj;
        if (CL_FIXNUM_P(obj)) return CL_MAKE_CHAR((uint32_t)CL_FIXNUM_VAL(obj));
        if (CL_ANY_STRING_P(obj) && cl_string_length(obj) == 1)
            return CL_MAKE_CHAR(cl_string_char_at(obj, 0));
        cl_error(CL_ERR_TYPE, "COERCE: cannot coerce to CHARACTER");
        return CL_NIL;
    }

    /* (coerce x 'float) / 'single-float / 'short-float */
    if (strcmp(tname, "FLOAT") == 0 || strcmp(tname, "SINGLE-FLOAT") == 0 ||
        strcmp(tname, "SHORT-FLOAT") == 0) {
        if (CL_SINGLE_FLOAT_P(obj)) return obj;
        if (CL_NUMBER_P(obj)) return cl_make_single_float(cl_to_float(obj));
        cl_error(CL_ERR_TYPE, "COERCE: cannot coerce to SINGLE-FLOAT");
        return CL_NIL;
    }

    /* (coerce x 'double-float) / 'long-float */
    if (strcmp(tname, "DOUBLE-FLOAT") == 0 || strcmp(tname, "LONG-FLOAT") == 0) {
        if (CL_DOUBLE_FLOAT_P(obj)) return obj;
        if (CL_NUMBER_P(obj)) return cl_make_double_float(cl_to_double(obj));
        cl_error(CL_ERR_TYPE, "COERCE: cannot coerce to DOUBLE-FLOAT");
        return CL_NIL;
    }

    /* (coerce x 'integer) / 'fixnum / 'number */
    if (strcmp(tname, "INTEGER") == 0 || strcmp(tname, "FIXNUM") == 0 || strcmp(tname, "NUMBER") == 0) {
        if (CL_FIXNUM_P(obj)) return obj;
        if (CL_CHAR_P(obj)) return CL_MAKE_FIXNUM(CL_CHAR_VAL(obj));
        cl_error(CL_ERR_TYPE, "COERCE: cannot coerce to INTEGER");
        return CL_NIL;
    }

    /* (coerce x 'string) or 'simple-string or 'base-string or 'simple-base-string */
    if (strcmp(tname, "STRING") == 0 || strcmp(tname, "SIMPLE-STRING") == 0 ||
        strcmp(tname, "BASE-STRING") == 0 || strcmp(tname, "SIMPLE-BASE-STRING") == 0) {
        if (CL_ANY_STRING_P(obj)) return obj;
        /* NIL is the empty sequence here, not the symbol: CLHS COERCE
         * requires a SEQUENCE for a string result type, so (coerce nil
         * 'string) is "" (SBCL, CCL, ECL agree) -- unlike (string nil). */
        if (CL_NULL_P(obj)) return cl_make_string("", 0);
        if (CL_SYMBOL_P(obj)) {
            const char *sname = cl_symbol_name(obj);
            return cl_make_string(sname, (uint32_t)strlen(sname));
        }
        if (CL_CHAR_P(obj)) {
#ifdef CL_WIDE_STRINGS
            if (CL_CHAR_VAL(obj) > 0x7F) {
                uint32_t cp = (uint32_t)CL_CHAR_VAL(obj);
                return cl_make_wide_string(&cp, 1);
            }
#endif
            {
                char c = (char)CL_CHAR_VAL(obj);
                return cl_make_string(&c, 1);
            }
        }
        /* Coerce list of characters to string.  obj is rooted across the
         * result allocation and the cursor re-read from it afterward —
         * pre-fix, p walked a stale list after cl_make_string compacted.
         * A non-ASCII character anywhere promotes the result to a wide
         * string — cl_string_set_char_at on a narrow string silently
         * truncates the code point to its low byte. */
        if (CL_NULL_P(obj) || CL_CONS_P(obj)) {
            CL_Obj p = obj;
            uint32_t len = 0;
            uint32_t i;
            CL_Obj result;
#ifdef CL_WIDE_STRINGS
            int wide = 0;
            while (!CL_NULL_P(p)) {
                CL_Obj elem = cl_car(p);
                if (CL_CHAR_P(elem) && CL_CHAR_VAL(elem) > 0x7F) wide = 1;
                len++;
                p = cl_cdr(p);
            }
#else
            while (!CL_NULL_P(p)) { len++; p = cl_cdr(p); }
#endif
            CL_GC_PROTECT(obj);
#ifdef CL_WIDE_STRINGS
            result = wide ? cl_make_wide_string(NULL, len)
                          : cl_make_string(NULL, len);
#else
            result = cl_make_string(NULL, len);
#endif
            p = obj;
            CL_GC_UNPROTECT(1);
            for (i = 0; i < len; i++) {
                CL_Obj elem = cl_car(p);
                if (!CL_CHAR_P(elem))
                    cl_error(CL_ERR_TYPE, "COERCE: list element is not a character");
                cl_string_set_char_at(result, i, CL_CHAR_VAL(elem));
                p = cl_cdr(p);
            }
            return result;
        }
        /* Coerce vector of characters to string — derive the data pointer
         * only AFTER the result allocation.  Wide promotion as above. */
        if (CL_VECTOR_P(obj)) {
            uint32_t len =
                cl_vector_active_length((CL_Vector *)CL_OBJ_TO_PTR(obj));
            uint32_t i;
            CL_Obj result;
            CL_Obj *elts;
#ifdef CL_WIDE_STRINGS
            int wide = 0;
            elts = cl_vector_data((CL_Vector *)CL_OBJ_TO_PTR(obj));
            for (i = 0; i < len; i++) {
                if (CL_CHAR_P(elts[i]) && CL_CHAR_VAL(elts[i]) > 0x7F) wide = 1;
            }
#endif
            CL_GC_PROTECT(obj);
#ifdef CL_WIDE_STRINGS
            result = wide ? cl_make_wide_string(NULL, len)
                          : cl_make_string(NULL, len);
#else
            result = cl_make_string(NULL, len);
#endif
            elts = cl_vector_data((CL_Vector *)CL_OBJ_TO_PTR(obj));
            CL_GC_UNPROTECT(1);
            for (i = 0; i < len; i++) {
                if (!CL_CHAR_P(elts[i]))
                    cl_error(CL_ERR_TYPE, "COERCE: vector element is not a character");
                cl_string_set_char_at(result, i, CL_CHAR_VAL(elts[i]));
            }
            return result;
        }
        cl_error(CL_ERR_TYPE, "COERCE: cannot coerce to STRING");
        return CL_NIL;
    }

    /* (coerce x 'bit-vector) */
    if (strcmp(tname, "BIT-VECTOR") == 0 || strcmp(tname, "SIMPLE-BIT-VECTOR") == 0) {
        if (CL_BIT_VECTOR_P(obj)) return obj;
        if (CL_NULL_P(obj) || CL_CONS_P(obj)) {
            CL_Obj p = obj;
            uint32_t len = 0;
            uint32_t ii;
            CL_Obj bvobj;
            CL_BitVector *bv;
            while (!CL_NULL_P(p)) { len++; p = cl_cdr(p); }
            CL_GC_PROTECT(obj);
            bvobj = cl_make_bit_vector(len);
            bv = (CL_BitVector *)CL_OBJ_TO_PTR(bvobj);
            p = obj;  /* re-read from the rooted slot after the alloc */
            CL_GC_UNPROTECT(1);
            for (ii = 0; ii < len; ii++) {
                CL_Obj elem = cl_car(p);
                if (CL_FIXNUM_P(elem) && CL_FIXNUM_VAL(elem) == 1)
                    bv->data[ii / 32] |= (1u << (ii % 32));
                p = cl_cdr(p);
            }
            return bvobj;
        }
        cl_error(CL_ERR_TYPE, "COERCE: cannot coerce to BIT-VECTOR");
        return CL_NIL;
    }

    /* (coerce x 'list) */
    if (strcmp(tname, "LIST") == 0) {
        if (CL_NULL_P(obj) || CL_CONS_P(obj)) return obj;
        /* obj must be rooted too in the loops below: each cl_cons compacts
         * under stress, and the source pointer/char reads re-derive from
         * obj every iteration — pre-fix they re-derived from a stale copy. */
        if (CL_BIT_VECTOR_P(obj)) {
            CL_BitVector *bv = (CL_BitVector *)CL_OBJ_TO_PTR(obj);
            CL_Obj result = CL_NIL;
            uint32_t ii = bv->length;
            CL_GC_PROTECT(obj);
            CL_GC_PROTECT(result);
            while (ii > 0) {
                ii--;
                bv = (CL_BitVector *)CL_OBJ_TO_PTR(obj);
                result = cl_cons(CL_MAKE_FIXNUM(cl_bv_get_bit(bv, ii)), result);
            }
            CL_GC_UNPROTECT(2);
            return result;
        }
        if (CL_VECTOR_P(obj)) {
            CL_Vector *v;
            CL_Obj result = CL_NIL;
            uint32_t i;
            CL_GC_PROTECT(obj);
            CL_GC_PROTECT(result);
            v = (CL_Vector *)CL_OBJ_TO_PTR(obj);
            i = cl_vector_active_length(v);
            while (i > 0) {
                i--;
                v = (CL_Vector *)CL_OBJ_TO_PTR(obj);
                result = cl_cons(cl_vector_data(v)[i], result);
            }
            CL_GC_UNPROTECT(2);
            return result;
        }
        if (CL_ANY_STRING_P(obj)) {
            CL_Obj result = CL_NIL;
            uint32_t i;
            CL_GC_PROTECT(obj);
            CL_GC_PROTECT(result);
            i = cl_string_length(obj);
            while (i > 0) {
                i--;
                result = cl_cons(CL_MAKE_CHAR(cl_string_char_at(obj, i)), result);
            }
            CL_GC_UNPROTECT(2);
            return result;
        }
        if (CL_BYTE_VECTOR_P(obj)) {
            CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(obj);
            CL_Obj result = CL_NIL;
            uint32_t ii = cl_bytevec_active_length(bv);
            CL_GC_PROTECT(obj);
            CL_GC_PROTECT(result);
            while (ii > 0) {
                ii--;
                bv = (CL_ByteVector *)CL_OBJ_TO_PTR(obj);
                result = cl_cons(CL_MAKE_FIXNUM(cl_bytevec_get(bv, ii)), result);
            }
            CL_GC_UNPROTECT(2);
            return result;
        }
        cl_error(CL_ERR_TYPE, "COERCE: cannot coerce to LIST");
        return CL_NIL;
    }

    /* (coerce x 'vector) or (coerce x 'simple-vector) */
    if (strcmp(tname, "VECTOR") == 0 || strcmp(tname, "SIMPLE-VECTOR") == 0) {
        /* Packed byte-vector result — (coerce x '(vector (unsigned-byte 8))).
         * Sources: lists, general vectors, byte vectors, bit vectors. */
        if (coerce_bytevec_signed >= 0) {
            int is_signed = coerce_bytevec_signed;
            int elt_shift = coerce_bytevec_shift;
            uint32_t blen, bi;
            CL_Obj bvec;
            if (CL_BYTE_VECTOR_P(obj) &&
                ((CL_ByteVector *)CL_OBJ_TO_PTR(obj))->is_signed ==
                    (uint8_t)is_signed &&
                ((CL_ByteVector *)CL_OBJ_TO_PTR(obj))->elt_shift ==
                    (uint8_t)elt_shift)
                return obj;
            if (CL_NULL_P(obj) || CL_CONS_P(obj)) {
                CL_Obj p = obj;
                blen = 0;
                while (CL_CONS_P(p)) { blen++; p = cl_cdr(p); }
            } else if (CL_VECTOR_P(obj)) {
                blen = cl_vector_active_length((CL_Vector *)CL_OBJ_TO_PTR(obj));
            } else if (CL_BYTE_VECTOR_P(obj)) {
                blen = cl_bytevec_active_length((CL_ByteVector *)CL_OBJ_TO_PTR(obj));
            } else if (CL_BIT_VECTOR_P(obj)) {
                blen = cl_bv_active_length((CL_BitVector *)CL_OBJ_TO_PTR(obj));
            } else {
                cl_error(CL_ERR_TYPE, "COERCE: cannot coerce to VECTOR");
                return CL_NIL;
            }
            CL_GC_PROTECT(obj);
            bvec = cl_make_byte_vector(blen, is_signed, elt_shift);
            CL_GC_UNPROTECT(1);
            {
                CL_ByteVector *dst = (CL_ByteVector *)CL_OBJ_TO_PTR(bvec);
                CL_Obj p = obj;  /* list cursor; loop below never allocates */
                for (bi = 0; bi < blen; bi++) {
                    CL_Obj elem;
                    if (CL_CONS_P(p)) { elem = cl_car(p); p = cl_cdr(p); }
                    else if (CL_VECTOR_P(obj))
                        elem = cl_vector_data((CL_Vector *)CL_OBJ_TO_PTR(obj))[bi];
                    else if (CL_BYTE_VECTOR_P(obj))
                        elem = CL_MAKE_FIXNUM(cl_bytevec_get(
                            (CL_ByteVector *)CL_OBJ_TO_PTR(obj), bi));
                    else
                        elem = CL_MAKE_FIXNUM(cl_bv_get_bit(
                            (CL_BitVector *)CL_OBJ_TO_PTR(obj), bi));
                    cl_bytevec_set(dst, bi,
                                   cl_bytevec_check_value(elem, is_signed,
                                                          elt_shift,
                                                          "COERCE to a byte vector"));
                }
            }
            return bvec;
        }
        if (CL_VECTOR_P(obj) || CL_BYTE_VECTOR_P(obj)) return obj;
        /* All three source walks below read through obj after
         * cl_make_vector can compact — root obj across the alloc and
         * re-derive source pointers afterward. */
        if (CL_ANY_STRING_P(obj)) {
            uint32_t slen = cl_string_length(obj);
            uint32_t ii;
            CL_Obj vec;
            CL_Vector *v;
            CL_GC_PROTECT(obj);
            vec = cl_make_vector(slen);
            CL_GC_UNPROTECT(1);
            v = (CL_Vector *)CL_OBJ_TO_PTR(vec);
            for (ii = 0; ii < slen; ii++)
                cl_vector_data(v)[ii] = CL_MAKE_CHAR(cl_string_char_at(obj, ii));
            v->elt_type = coerce_vec_elt_code;
            return vec;
        }
        if (CL_BIT_VECTOR_P(obj)) {
            uint32_t bvlen = ((CL_BitVector *)CL_OBJ_TO_PTR(obj))->length;
            uint32_t ii;
            CL_Obj vec;
            CL_BitVector *bv;
            CL_Vector *v;
            CL_GC_PROTECT(obj);
            vec = cl_make_vector(bvlen);
            bv = (CL_BitVector *)CL_OBJ_TO_PTR(obj);
            CL_GC_UNPROTECT(1);
            v = (CL_Vector *)CL_OBJ_TO_PTR(vec);
            for (ii = 0; ii < bvlen; ii++)
                cl_vector_data(v)[ii] = CL_MAKE_FIXNUM(cl_bv_get_bit(bv, ii));
            v->elt_type = coerce_vec_elt_code;
            return vec;
        }
        if (CL_NULL_P(obj) || CL_CONS_P(obj)) {
            CL_Obj p = obj;
            uint32_t len = 0;
            uint32_t i;
            CL_Obj vec;
            CL_Vector *v;
            while (!CL_NULL_P(p)) {
                len++;
                p = cl_cdr(p);
            }
            CL_GC_PROTECT(obj);
            vec = cl_make_vector(len);
            v = (CL_Vector *)CL_OBJ_TO_PTR(vec);
            p = obj;  /* re-read from the rooted slot after the alloc */
            CL_GC_UNPROTECT(1);
            for (i = 0; i < len; i++) {
                cl_vector_data(v)[i] = cl_car(p);
                p = cl_cdr(p);
            }
            v->elt_type = coerce_vec_elt_code;
            return vec;
        }
        cl_error(CL_ERR_TYPE, "COERCE: cannot coerce to VECTOR");
        return CL_NIL;
    }

    /* (coerce x 'function) — CL spec: identity for functions,
     * fdefinition for symbols, compile for lambda exprs */
    if (strcmp(tname, "FUNCTION") == 0 ||
        strcmp(tname, "COMPILED-FUNCTION") == 0) {
        if (CL_FUNCTION_OBJ_P(obj))
            return obj;
        if (CL_SYMBOL_P(obj))
            return cl_coerce_funcdesig(obj, "COERCE");
        /* Lambda expression: (lambda (...) ...) — compile (function <expr>) */
        if (CL_CONS_P(obj)) {
            CL_Obj head = cl_car(obj);
            if (CL_SYMBOL_P(head) &&
                strcmp(cl_symbol_name(head), "LAMBDA") == 0) {
                extern CL_Obj cl_compile(CL_Obj form);
                extern CL_Obj cl_vm_eval(CL_Obj bytecode);
                extern CL_Obj cl_intern_in(const char *name, uint32_t len, CL_Obj pkg);
                extern CL_Obj cl_package_cl;
                /* Build the inner cons FIRST: nesting the two conses would
                 * leave fn_sym stale when the inner one compacts (FUNCTION
                 * is interned already, so the intern itself is a lookup). */
                CL_Obj form = cl_cons(obj, CL_NIL);
                CL_Obj fn_sym;
                CL_Obj bytecode;
                CL_GC_PROTECT(form);
                fn_sym = cl_intern_in("FUNCTION", 8, cl_package_cl);
                form = cl_cons(fn_sym, form);
                bytecode = cl_compile(form);
                CL_GC_UNPROTECT(1);
                if (CL_NULL_P(bytecode))
                    cl_error(CL_ERR_TYPE, "COERCE: failed to compile lambda");
                return cl_vm_eval(bytecode);
            }
        }
        cl_error(CL_ERR_TYPE, "COERCE: cannot coerce to FUNCTION");
        return CL_NIL;
    }

    /* (coerce x 'pathname) */
    if (strcmp(tname, "PATHNAME") == 0) {
        if (CL_PATHNAME_P(obj)) return obj;
        if (CL_ANY_STRING_P(obj)) {
            extern CL_Obj cl_parse_namestring(const char *str, uint32_t len);
            CL_String *s = (CL_String *)CL_OBJ_TO_PTR(obj);
            return cl_parse_namestring(s->data, s->length);
        }
        cl_error(CL_ERR_TYPE, "COERCE: cannot coerce to PATHNAME");
        return CL_NIL;
    }

    /* CLHS COERCE: "If the result-type is a recognizable subtype of the type
     * of object, ... object is returned."  In particular coercing a value to a
     * numeric type it already satisfies (RATIONAL, REAL, NUMBER, COMPLEX, RATIO,
     * (UNSIGNED-BYTE n), ...) is the identity — no conversion is performed.
     * Use the original (unreduced) result-type so length/range constraints are
     * honored. */
    if (cl_typep(obj, args[1]))
        return args[0]; /* args[] is GC-rooted; obj may be stale after cl_typep compacts */

    cl_error(CL_ERR_TYPE, "COERCE: unknown result type %s",
             cl_symbol_name(result_type));
    return CL_NIL;
}

/* --- subtypep --- */

/* Type ID enum for hierarchy checks */
enum TypeId {
    TID_UNKNOWN = -1,
    TID_NIL = 0,
    TID_NULL,
    TID_FIXNUM,
    TID_BIGNUM,
    TID_INTEGER,
    TID_RATIO,
    TID_RATIONAL,
    TID_REAL,
    TID_COMPLEX,
    TID_NUMBER,
    TID_CHARACTER,
    TID_KEYWORD,
    TID_SYMBOL,
    TID_CONS,
    TID_LIST,
    TID_STRING,
    TID_SIMPLE_VECTOR,
    TID_VECTOR,
    TID_SIMPLE_ARRAY,
    TID_ARRAY,
    TID_SEQUENCE,
    TID_COMPILED_FUNCTION,
    TID_FUNCTION,
    TID_HASH_TABLE,
    TID_PACKAGE,
    TID_ATOM,
    TID_CONDITION,
    TID_WARNING,
    TID_SERIOUS_CONDITION,
    TID_ERROR,
    TID_SIMPLE_CONDITION,
    TID_SIMPLE_ERROR,
    TID_SIMPLE_WARNING,
    TID_TYPE_ERROR,
    TID_PROGRAM_ERROR,
    TID_CONTROL_ERROR,
    TID_ARITHMETIC_ERROR,
    TID_DIVISION_BY_ZERO,
    TID_UNBOUND_VARIABLE,
    TID_UNDEFINED_FUNCTION,
    TID_SINGLE_FLOAT,
    TID_DOUBLE_FLOAT,
    TID_FLOAT,
    TID_STRUCTURE,
    TID_BIT_VECTOR,
    TID_SIMPLE_BIT_VECTOR,
    TID_PATHNAME,
    TID_BOOLEAN,
    TID_T,
    /* Virtual TIDs for compound array specifiers more restrictive than
     * any atomic type. Not reachable from type_name_to_id — only
     * produced by compound_type_to_tid on a CONS type specifier. */
    TID_SIMPLE_ARRAY_1D,   /* (simple-array * (*)) — 1-D simple-array */
#ifdef CL_WIDE_STRINGS
    TID_BASE_CHAR,
    TID_STANDARD_CHAR,
    TID_EXTENDED_CHAR,
    TID_BASE_STRING,
    TID_SIMPLE_BASE_STRING,
#endif
    TID_COUNT
};

static int type_name_to_id(const char *name)
{
    if (strcmp(name, "NIL") == 0) return TID_NIL;
    if (strcmp(name, "NULL") == 0) return TID_NULL;
    if (strcmp(name, "FIXNUM") == 0) return TID_FIXNUM;
    if (strcmp(name, "BIGNUM") == 0) return TID_BIGNUM;
    if (strcmp(name, "INTEGER") == 0) return TID_INTEGER;
    if (strcmp(name, "RATIO") == 0) return TID_RATIO;
    if (strcmp(name, "RATIONAL") == 0) return TID_RATIONAL;
    if (strcmp(name, "REAL") == 0) return TID_REAL;
    if (strcmp(name, "NUMBER") == 0) return TID_NUMBER;
    if (strcmp(name, "COMPLEX") == 0) return TID_COMPLEX;
    if (strcmp(name, "CHARACTER") == 0) return TID_CHARACTER;
#ifdef CL_WIDE_STRINGS
    if (strcmp(name, "BASE-CHAR") == 0) return TID_BASE_CHAR;
    if (strcmp(name, "STANDARD-CHAR") == 0) return TID_STANDARD_CHAR;
    if (strcmp(name, "EXTENDED-CHAR") == 0) return TID_EXTENDED_CHAR;
#else
    if (strcmp(name, "BASE-CHAR") == 0) return TID_CHARACTER;
    if (strcmp(name, "STANDARD-CHAR") == 0) return TID_CHARACTER;
#endif
    if (strcmp(name, "KEYWORD") == 0) return TID_KEYWORD;
    if (strcmp(name, "SYMBOL") == 0) return TID_SYMBOL;
    if (strcmp(name, "CONS") == 0) return TID_CONS;
    if (strcmp(name, "LIST") == 0) return TID_LIST;
    if (strcmp(name, "STRING") == 0) return TID_STRING;
    if (strcmp(name, "SIMPLE-STRING") == 0) return TID_STRING;
    if (strcmp(name, "BASE-STRING") == 0) return TID_STRING;
    if (strcmp(name, "SIMPLE-BASE-STRING") == 0) return TID_STRING;
    if (strcmp(name, "SIMPLE-VECTOR") == 0) return TID_SIMPLE_VECTOR;
    if (strcmp(name, "VECTOR") == 0) return TID_VECTOR;
    if (strcmp(name, "SIMPLE-ARRAY") == 0) return TID_SIMPLE_ARRAY;
    if (strcmp(name, "ARRAY") == 0) return TID_ARRAY;
    if (strcmp(name, "SEQUENCE") == 0) return TID_SEQUENCE;
    if (strcmp(name, "COMPILED-FUNCTION") == 0) return TID_COMPILED_FUNCTION;
    if (strcmp(name, "FUNCTION") == 0) return TID_FUNCTION;
    if (strcmp(name, "HASH-TABLE") == 0) return TID_HASH_TABLE;
    if (strcmp(name, "PACKAGE") == 0) return TID_PACKAGE;
    if (strcmp(name, "ATOM") == 0) return TID_ATOM;
    if (strcmp(name, "CONDITION") == 0) return TID_CONDITION;
    if (strcmp(name, "WARNING") == 0) return TID_WARNING;
    if (strcmp(name, "SERIOUS-CONDITION") == 0) return TID_SERIOUS_CONDITION;
    if (strcmp(name, "ERROR") == 0) return TID_ERROR;
    if (strcmp(name, "SIMPLE-CONDITION") == 0) return TID_SIMPLE_CONDITION;
    if (strcmp(name, "SIMPLE-ERROR") == 0) return TID_SIMPLE_ERROR;
    if (strcmp(name, "SIMPLE-WARNING") == 0) return TID_SIMPLE_WARNING;
    if (strcmp(name, "TYPE-ERROR") == 0) return TID_TYPE_ERROR;
    if (strcmp(name, "PROGRAM-ERROR") == 0) return TID_PROGRAM_ERROR;
    if (strcmp(name, "CONTROL-ERROR") == 0) return TID_CONTROL_ERROR;
    if (strcmp(name, "ARITHMETIC-ERROR") == 0) return TID_ARITHMETIC_ERROR;
    if (strcmp(name, "DIVISION-BY-ZERO") == 0) return TID_DIVISION_BY_ZERO;
    if (strcmp(name, "UNBOUND-VARIABLE") == 0) return TID_UNBOUND_VARIABLE;
    if (strcmp(name, "UNDEFINED-FUNCTION") == 0) return TID_UNDEFINED_FUNCTION;
    if (strcmp(name, "SINGLE-FLOAT") == 0 || strcmp(name, "SHORT-FLOAT") == 0) return TID_SINGLE_FLOAT;
    if (strcmp(name, "DOUBLE-FLOAT") == 0 || strcmp(name, "LONG-FLOAT") == 0) return TID_DOUBLE_FLOAT;
    if (strcmp(name, "FLOAT") == 0) return TID_FLOAT;
    if (strcmp(name, "STRUCTURE") == 0 || strcmp(name, "STRUCTURE-OBJECT") == 0) return TID_STRUCTURE;
    if (strcmp(name, "BIT-VECTOR") == 0) return TID_BIT_VECTOR;
    if (strcmp(name, "SIMPLE-BIT-VECTOR") == 0) return TID_SIMPLE_BIT_VECTOR;
    if (strcmp(name, "PATHNAME") == 0) return TID_PATHNAME;
    if (strcmp(name, "BOOLEAN") == 0) return TID_BOOLEAN;
    if (strcmp(name, "T") == 0) return TID_T;
    return TID_UNKNOWN;
}

/* Public API: check if a name is a built-in type recognized by typep */
int cl_is_builtin_type_name(const char *name)
{
    return type_name_to_id(name) != TID_UNKNOWN;
}

/* Map a compound array/vector/simple-array type specifier to a TID.
 * Returns TID_UNKNOWN if the compound can't be normalized.
 * Recognized patterns (et = element-type, dims = dimension-spec, any *):
 *   (simple-array * *)    → TID_SIMPLE_ARRAY
 *   (simple-array * (*))  → TID_SIMPLE_ARRAY_1D (virtual: rank-1 simple-array)
 *   (array * *)           → TID_ARRAY
 *   (array * (*))         → TID_VECTOR (vectors are by definition 1-D arrays)
 *   (vector ...)          → TID_VECTOR
 *   (simple-vector ...)   → TID_SIMPLE_VECTOR
 *   (string ...)          → TID_STRING
 *   (bit-vector ...)      → TID_BIT_VECTOR
 *   (simple-bit-vector …) → TID_SIMPLE_BIT_VECTOR
 */
static int compound_type_to_tid(CL_Obj type)
{
    CL_Obj head, et, dims, tail, rest;
    const char *hn;
    int et_star, dims_star, dims_rank1;
    if (!CL_CONS_P(type)) return TID_UNKNOWN;
    head = cl_car(type);
    if (!CL_SYMBOL_P(head)) return TID_UNKNOWN;
    hn = cl_symbol_name(head);

    tail = cl_cdr(type);
    /* et defaults to * if missing */
    et = (CL_NULL_P(tail)) ? TYPE_SYM_STAR : cl_car(tail);
    rest = (CL_NULL_P(tail)) ? CL_NIL : cl_cdr(tail);
    dims = (CL_NULL_P(rest)) ? TYPE_SYM_STAR : cl_car(rest);

    et_star    = (CL_SYMBOL_P(et) && strcmp(cl_symbol_name(et), "*") == 0);
    dims_star  = (CL_SYMBOL_P(dims) && strcmp(cl_symbol_name(dims), "*") == 0);
    dims_rank1 = (CL_CONS_P(dims) && CL_NULL_P(cl_cdr(dims)) &&
                  CL_SYMBOL_P(cl_car(dims)) &&
                  strcmp(cl_symbol_name(cl_car(dims)), "*") == 0);

    if (strcmp(hn, "SIMPLE-ARRAY") == 0) {
        if (et_star && dims_star)  return TID_SIMPLE_ARRAY;
        if (dims_rank1) {
            /* Specialize on common element types so that e.g.
             *   (simple-array character (*)) is recognized as a subtype of
             *   STRING / VECTOR / ARRAY / SEQUENCE (required by serapeum). */
            if (CL_SYMBOL_P(et)) {
                const char *etn = cl_symbol_name(et);
                if (strcmp(etn, "CHARACTER") == 0 ||
                    strcmp(etn, "BASE-CHAR") == 0 ||
                    strcmp(etn, "STANDARD-CHAR") == 0)
                    return TID_STRING;
                if (strcmp(etn, "BIT") == 0)
                    return TID_SIMPLE_BIT_VECTOR;
                if (strcmp(etn, "T") == 0)
                    return TID_SIMPLE_VECTOR;
            }
            return TID_SIMPLE_ARRAY_1D;
        }
        return TID_SIMPLE_ARRAY;
    }
    if (strcmp(hn, "ARRAY") == 0) {
        if (et_star && dims_star)  return TID_ARRAY;
        if (dims_rank1) {
            if (CL_SYMBOL_P(et)) {
                const char *etn = cl_symbol_name(et);
                if (strcmp(etn, "CHARACTER") == 0 ||
                    strcmp(etn, "BASE-CHAR") == 0 ||
                    strcmp(etn, "STANDARD-CHAR") == 0)
                    return TID_STRING;
                if (strcmp(etn, "BIT") == 0)
                    return TID_BIT_VECTOR;
            }
            return TID_VECTOR;
        }
        return TID_ARRAY;
    }
    if (strcmp(hn, "VECTOR") == 0) {
        /* (vector [et [size]]) — rank always 1.  Map the specialized element
         * types to their own TIDs — (vector character) ≡ STRING and
         * (vector bit) ≡ BIT-VECTOR — mirroring the (array et (*)) branch
         * above so both spellings sit at the same place in the hierarchy. */
        if (CL_SYMBOL_P(et)) {
            const char *etn = cl_symbol_name(et);
            if (strcmp(etn, "CHARACTER") == 0 ||
                strcmp(etn, "BASE-CHAR") == 0 ||
                strcmp(etn, "STANDARD-CHAR") == 0)
                return TID_STRING;
            if (strcmp(etn, "BIT") == 0)
                return TID_BIT_VECTOR;
        }
        return TID_VECTOR;
    }
    if (strcmp(hn, "SIMPLE-VECTOR") == 0) {
        return TID_SIMPLE_VECTOR;
    }
    if (strcmp(hn, "STRING") == 0 || strcmp(hn, "SIMPLE-STRING") == 0 ||
        strcmp(hn, "BASE-STRING") == 0 || strcmp(hn, "SIMPLE-BASE-STRING") == 0) {
        return TID_STRING;
    }
    if (strcmp(hn, "BIT-VECTOR") == 0)        return TID_BIT_VECTOR;
    if (strcmp(hn, "SIMPLE-BIT-VECTOR") == 0) return TID_SIMPLE_BIT_VECTOR;

    return TID_UNKNOWN;
}


/* Check if type1 is a subtype of type2 using the known hierarchy.
 * Returns: 1 = yes, 0 = no, -1 = unknown */
static int subtype_check(int id1, int id2)
{
    /* nil is subtype of everything */
    if (id1 == TID_NIL) return 1;
    /* everything is subtype of t */
    if (id2 == TID_T) return 1;
    /* same type */
    if (id1 == id2) return 1;
    /* t is NOT subtype of anything except t */
    if (id1 == TID_T) return 0;

    /* Numeric hierarchy: fixnum/bignum < integer < rational < real < number */
    if (id1 == TID_FIXNUM && (id2 == TID_INTEGER || id2 == TID_RATIONAL ||
                               id2 == TID_REAL || id2 == TID_NUMBER)) return 1;
    if (id1 == TID_BIGNUM && (id2 == TID_INTEGER || id2 == TID_RATIONAL ||
                               id2 == TID_REAL || id2 == TID_NUMBER)) return 1;
    if (id1 == TID_INTEGER && (id2 == TID_RATIONAL || id2 == TID_REAL ||
                                id2 == TID_NUMBER)) return 1;
    if (id1 == TID_RATIO && (id2 == TID_RATIONAL || id2 == TID_REAL ||
                              id2 == TID_NUMBER)) return 1;
    if (id1 == TID_RATIONAL && (id2 == TID_REAL || id2 == TID_NUMBER)) return 1;
    if (id1 == TID_REAL && id2 == TID_NUMBER) return 1;

    /* Float hierarchy: single-float/double-float < float < real < number */
    if (id1 == TID_SINGLE_FLOAT && (id2 == TID_FLOAT || id2 == TID_REAL ||
                                     id2 == TID_NUMBER)) return 1;
    if (id1 == TID_DOUBLE_FLOAT && (id2 == TID_FLOAT || id2 == TID_REAL ||
                                     id2 == TID_NUMBER)) return 1;
    if (id1 == TID_FLOAT && (id2 == TID_REAL || id2 == TID_NUMBER)) return 1;

    /* Complex hierarchy: complex < number */
    if (id1 == TID_COMPLEX && id2 == TID_NUMBER) return 1;

    /* List hierarchy: null < list, cons < list, list < sequence */
    if (id1 == TID_NULL && (id2 == TID_LIST || id2 == TID_SYMBOL || id2 == TID_SEQUENCE)) return 1;
    if (id1 == TID_CONS && (id2 == TID_LIST || id2 == TID_SEQUENCE)) return 1;
    if (id1 == TID_LIST && id2 == TID_SEQUENCE) return 1;

    /* Symbol hierarchy: keyword < symbol, null < symbol, boolean < symbol */
    if (id1 == TID_KEYWORD && id2 == TID_SYMBOL) return 1;
    if (id1 == TID_BOOLEAN && id2 == TID_SYMBOL) return 1;
    if (id1 == TID_NULL && id2 == TID_BOOLEAN) return 1;

    /* Vector/array hierarchy:
     * simple-vector < simple-array < array
     * simple-vector < vector < array
     * vector < sequence
     * string < vector < array
     */
    if (id1 == TID_SIMPLE_VECTOR && (id2 == TID_VECTOR || id2 == TID_SIMPLE_ARRAY ||
                                      id2 == TID_ARRAY || id2 == TID_SEQUENCE)) return 1;
    if (id1 == TID_SIMPLE_ARRAY && id2 == TID_ARRAY) return 1;
    if (id1 == TID_VECTOR && (id2 == TID_ARRAY || id2 == TID_SEQUENCE)) return 1;
    if (id1 == TID_STRING && (id2 == TID_VECTOR || id2 == TID_SIMPLE_ARRAY ||
                               id2 == TID_ARRAY || id2 == TID_SEQUENCE ||
                               id2 == TID_SIMPLE_ARRAY_1D)) return 1;
    if (id1 == TID_BIT_VECTOR && (id2 == TID_VECTOR || id2 == TID_ARRAY ||
                                   id2 == TID_SEQUENCE)) return 1;
    if (id1 == TID_SIMPLE_BIT_VECTOR && (id2 == TID_BIT_VECTOR || id2 == TID_VECTOR ||
                                          id2 == TID_SIMPLE_ARRAY || id2 == TID_ARRAY ||
                                          id2 == TID_SEQUENCE ||
                                          id2 == TID_SIMPLE_ARRAY_1D)) return 1;

    /* Virtual: (simple-array * (*)) — 1-D simple-array of any element type.
     * Subtypes: only types guaranteed simple AND 1-D — our atomic flags
     * SIMPLE-VECTOR and SIMPLE-BIT-VECTOR qualify.  STRING / BIT-VECTOR
     * may be non-simple; SIMPLE-ARRAY may be multi-dim. */
    if (id2 == TID_SIMPLE_ARRAY_1D) {
        if (id1 == TID_SIMPLE_VECTOR || id1 == TID_SIMPLE_BIT_VECTOR) return 1;
    }
    if (id1 == TID_SIMPLE_ARRAY_1D) {
        if (id2 == TID_SIMPLE_ARRAY || id2 == TID_ARRAY ||
            id2 == TID_VECTOR || id2 == TID_SEQUENCE) return 1;
    }

    /* Function hierarchy: compiled-function < function */
    if (id1 == TID_COMPILED_FUNCTION && id2 == TID_FUNCTION) return 1;

#ifdef CL_WIDE_STRINGS
    /* Character hierarchy: base-char < character, standard-char < base-char < character,
       extended-char < character */
    if (id1 == TID_BASE_CHAR && id2 == TID_CHARACTER) return 1;
    if (id1 == TID_STANDARD_CHAR && (id2 == TID_BASE_CHAR || id2 == TID_CHARACTER)) return 1;
    if (id1 == TID_EXTENDED_CHAR && id2 == TID_CHARACTER) return 1;
#endif

    /* Fixnum/integer/number are atoms */
    if (id2 == TID_ATOM) {
        if (id1 != TID_CONS && id1 != TID_LIST) return 1;
        if (id1 == TID_LIST) return 0;  /* list includes cons */
    }

    /* Condition hierarchy:
     * condition
     *   warning
     *     simple-warning  (also simple-condition)
     *   serious-condition
     *     error
     *       simple-error  (also simple-condition)
     *       type-error
     *       program-error
     *       control-error
     *       unbound-variable
     *       undefined-function
     *       arithmetic-error
     *         division-by-zero
     *   simple-condition
     */
    if (id2 == TID_CONDITION) {
        if (id1 == TID_WARNING || id1 == TID_SERIOUS_CONDITION ||
            id1 == TID_ERROR || id1 == TID_SIMPLE_CONDITION ||
            id1 == TID_SIMPLE_ERROR || id1 == TID_SIMPLE_WARNING ||
            id1 == TID_TYPE_ERROR || id1 == TID_PROGRAM_ERROR ||
            id1 == TID_CONTROL_ERROR || id1 == TID_ARITHMETIC_ERROR ||
            id1 == TID_DIVISION_BY_ZERO || id1 == TID_UNBOUND_VARIABLE ||
            id1 == TID_UNDEFINED_FUNCTION)
            return 1;
    }
    if (id2 == TID_WARNING) {
        if (id1 == TID_SIMPLE_WARNING) return 1;
    }
    if (id2 == TID_SERIOUS_CONDITION) {
        if (id1 == TID_ERROR || id1 == TID_SIMPLE_ERROR ||
            id1 == TID_TYPE_ERROR || id1 == TID_PROGRAM_ERROR ||
            id1 == TID_CONTROL_ERROR || id1 == TID_ARITHMETIC_ERROR ||
            id1 == TID_DIVISION_BY_ZERO || id1 == TID_UNBOUND_VARIABLE ||
            id1 == TID_UNDEFINED_FUNCTION)
            return 1;
    }
    if (id2 == TID_ERROR) {
        if (id1 == TID_SIMPLE_ERROR || id1 == TID_TYPE_ERROR ||
            id1 == TID_PROGRAM_ERROR || id1 == TID_CONTROL_ERROR ||
            id1 == TID_ARITHMETIC_ERROR || id1 == TID_DIVISION_BY_ZERO ||
            id1 == TID_UNBOUND_VARIABLE || id1 == TID_UNDEFINED_FUNCTION)
            return 1;
    }
    if (id2 == TID_SIMPLE_CONDITION) {
        if (id1 == TID_SIMPLE_ERROR || id1 == TID_SIMPLE_WARNING)
            return 1;
    }
    if (id2 == TID_ARITHMETIC_ERROR) {
        if (id1 == TID_DIVISION_BY_ZERO) return 1;
    }

    /* Structure hierarchy: structure < atom */
    if (id1 == TID_STRUCTURE && id2 == TID_ATOM) return 1;

    /* Pathname is an atom */
    if (id1 == TID_PATHNAME && id2 == TID_ATOM) return 1;

    return 0;
}

/* Structural equality on type specs: interned symbols are EQ, conses are
 * compared recursively.  Sufficient for the (deftype) bodies subtypep sees. */
static int tspec_equal_p(CL_Obj a, CL_Obj b)
{
    if (a == b) return 1;
    if (CL_CONS_P(a) && CL_CONS_P(b))
        return tspec_equal_p(cl_car(a), cl_car(b)) &&
               tspec_equal_p(cl_cdr(a), cl_cdr(b));
    return 0;
}

/* --- Bounded integer range support for subtypep ---
 *
 * Range bounds are represented as int64_t with INT64_MIN/INT64_MAX as
 * sentinels for ±∞.  Exclusive bounds `(N)` are normalized to inclusive
 * (lo: lo+1, hi: hi-1) so compare_ranges does plain ≥/≤.  Bignum bounds
 * are not supported — we punt to "uncertain" via the int_range_unknown
 * sentinel so the caller falls back to other rules. */
typedef struct {
    int64_t lo, hi;       /* inclusive after normalization */
    int known;            /* 0 = couldn't parse (e.g. bignum), 1 = ok */
    int integer_typed;    /* 1 if this is an integer-based type spec */
} int_range_t;

#define INT_RANGE_LO_INF INT64_MIN
#define INT_RANGE_HI_INF INT64_MAX

/* Parse a bound from `(integer L H)` or `(signed-byte N)` etc.  bound is
 * the raw form: `*`, an integer, or `(N)` (exclusive).  Returns the
 * inclusive value via *out, with *out_inf set if the bound is unbounded
 * (in the direction indicated by `is_low`).  Returns 1 on success, 0 if
 * we couldn't represent the bound (bignum etc.). */
static int parse_int_bound(CL_Obj bound, int is_low, int64_t *out, int *out_inf)
{
    *out_inf = 0;
    /* `*` (the symbol star) means unbounded. */
    if (CL_SYMBOL_P(bound)) {
        if (strcmp(cl_symbol_name(bound), "*") == 0) {
            *out_inf = 1;
            *out = is_low ? INT_RANGE_LO_INF : INT_RANGE_HI_INF;
            return 1;
        }
        return 0;
    }
    /* Exclusive bound: (N) — single-element list. */
    if (CL_CONS_P(bound) && CL_NULL_P(cl_cdr(bound))) {
        int64_t v;
        int inf;
        if (!parse_int_bound(cl_car(bound), is_low, &v, &inf)) return 0;
        if (inf) { *out_inf = 1; *out = v; return 1; }
        /* Exclude the bound: low → +1, high → -1. */
        *out = is_low ? v + 1 : v - 1;
        return 1;
    }
    /* Plain fixnum bound. */
    if (CL_FIXNUM_P(bound)) {
        *out = (int64_t)CL_FIXNUM_VAL(bound);
        return 1;
    }
    /* Bignum / non-integer: punt. */
    return 0;
}

/* Decompose an integer-typed compound spec into a range.  Returns:
 *  - integer_typed=0 if not an integer-based spec (caller should use other rules)
 *  - known=0 if we recognize it but can't compute bounds (bignums, etc.)
 *  - known=1 with lo/hi populated otherwise. */
static int_range_t parse_integer_spec(CL_Obj type)
{
    int_range_t r;
    r.lo = INT_RANGE_LO_INF;
    r.hi = INT_RANGE_HI_INF;
    r.known = 0;
    r.integer_typed = 0;

    /* Atomic INTEGER, FIXNUM, BIT. */
    if (CL_SYMBOL_P(type)) {
        const char *n = cl_symbol_name(type);
        if (strcmp(n, "INTEGER") == 0) {
            r.integer_typed = 1; r.known = 1;
            return r;
        }
        if (strcmp(n, "FIXNUM") == 0) {
            r.integer_typed = 1; r.known = 1;
            r.lo = (int64_t)CL_FIXNUM_MIN;
            r.hi = (int64_t)CL_FIXNUM_MAX;
            return r;
        }
        if (strcmp(n, "BIT") == 0) {
            r.integer_typed = 1; r.known = 1;
            r.lo = 0; r.hi = 1;
            return r;
        }
        /* Atomic UNSIGNED-BYTE = (integer 0 *), SIGNED-BYTE = INTEGER. */
        if (strcmp(n, "UNSIGNED-BYTE") == 0) {
            r.integer_typed = 1; r.known = 1;
            r.lo = 0;
            return r;
        }
        if (strcmp(n, "SIGNED-BYTE") == 0) {
            r.integer_typed = 1; r.known = 1;
            return r;
        }
        return r;  /* not integer-typed */
    }

    /* Compound forms. */
    if (!CL_CONS_P(type)) return r;
    {
        CL_Obj head = cl_car(type);
        if (!CL_SYMBOL_P(head)) return r;
        {
            const char *hn = cl_symbol_name(head);
            CL_Obj args = cl_cdr(type);
            if (strcmp(hn, "INTEGER") == 0) {
                int64_t lo = INT_RANGE_LO_INF, hi = INT_RANGE_HI_INF;
                int lo_inf = 1, hi_inf = 1;
                r.integer_typed = 1;
                if (CL_NULL_P(args)) { r.known = 1; return r; }
                if (!parse_int_bound(cl_car(args), 1, &lo, &lo_inf)) return r;
                args = cl_cdr(args);
                if (!CL_NULL_P(args)) {
                    if (!parse_int_bound(cl_car(args), 0, &hi, &hi_inf)) return r;
                }
                r.known = 1;
                r.lo = lo;
                r.hi = hi;
                return r;
            }
            if (strcmp(hn, "MOD") == 0) {
                /* (mod N) = (integer 0 (N-1)) */
                if (CL_NULL_P(args)) return r;
                {
                    CL_Obj n = cl_car(args);
                    r.integer_typed = 1;
                    if (CL_FIXNUM_P(n)) {
                        int64_t nv = (int64_t)CL_FIXNUM_VAL(n);
                        if (nv > 0) { r.known = 1; r.lo = 0; r.hi = nv - 1; }
                        else if (nv == 0) { /* (mod 0) = NIL — empty type */
                            r.known = 1; r.lo = 1; r.hi = 0;  /* lo>hi means empty */
                        }
                    }
                }
                return r;
            }
            if (strcmp(hn, "UNSIGNED-BYTE") == 0) {
                /* (unsigned-byte N) = (integer 0 (1- (expt 2 N))) */
                r.integer_typed = 1;
                if (CL_NULL_P(args)) {
                    /* No bound → INTEGER */
                    r.known = 1; r.lo = 0;
                    return r;
                }
                {
                    CL_Obj n = cl_car(args);
                    if (CL_SYMBOL_P(n)) {
                        const char *ns = cl_symbol_name(n);
                        if (strcmp(ns, "*") == 0) { r.known = 1; r.lo = 0; return r; }
                    } else if (CL_FIXNUM_P(n)) {
                        int32_t nv = CL_FIXNUM_VAL(n);
                        if (nv >= 0 && nv < 62) {
                            r.known = 1;
                            r.lo = 0;
                            r.hi = (int64_t)((uint64_t)1 << nv) - 1;
                            return r;
                        }
                        /* nv >= 62 won't fit in int64_t */
                    }
                }
                return r;
            }
            if (strcmp(hn, "SIGNED-BYTE") == 0) {
                /* (signed-byte N) = (integer (- (expt 2 (1- N))) (1- (expt 2 (1- N)))) */
                r.integer_typed = 1;
                if (CL_NULL_P(args)) { r.known = 1; return r; }
                {
                    CL_Obj n = cl_car(args);
                    if (CL_SYMBOL_P(n)) {
                        const char *ns = cl_symbol_name(n);
                        if (strcmp(ns, "*") == 0) { r.known = 1; return r; }
                    } else if (CL_FIXNUM_P(n)) {
                        int32_t nv = CL_FIXNUM_VAL(n);
                        if (nv >= 1 && nv <= 63) {
                            r.known = 1;
                            r.lo = -(int64_t)((uint64_t)1 << (nv - 1));
                            r.hi = (int64_t)((uint64_t)1 << (nv - 1)) - 1;
                            return r;
                        }
                    }
                }
                return r;
            }
        }
    }
    return r;
}

/* True if `type` is a numeric supertype strictly broader than INTEGER:
 * RATIONAL, REAL, FLOAT, NUMBER, COMPLEX, SHORT/SINGLE/DOUBLE/LONG-FLOAT,
 * etc.  Used to short-circuit `(subtypep <broader-numeric> <integer-spec>)`
 * to a certain NO (rationals/reals/floats aren't integers).  */
static int is_non_integer_numeric_supertype(CL_Obj type)
{
    if (!CL_SYMBOL_P(type)) return 0;
    {
        const char *n = cl_symbol_name(type);
        return strcmp(n, "RATIONAL") == 0 ||
               strcmp(n, "REAL") == 0 ||
               strcmp(n, "NUMBER") == 0 ||
               strcmp(n, "COMPLEX") == 0 ||
               strcmp(n, "FLOAT") == 0 ||
               strcmp(n, "SHORT-FLOAT") == 0 ||
               strcmp(n, "SINGLE-FLOAT") == 0 ||
               strcmp(n, "DOUBLE-FLOAT") == 0 ||
               strcmp(n, "LONG-FLOAT") == 0 ||
               strcmp(n, "RATIO") == 0;
    }
}

/* Attempt subtypep using bounded integer range arithmetic.
 * Returns:
 *   1 = (T T)         certain subtype
 *   0 = (NIL T)       certain non-subtype
 *  -1 = uncertain / not applicable; caller should try other rules. */
int cl_subtypep_integer_ranges(CL_Obj t1, CL_Obj t2)
{
    int_range_t r1 = parse_integer_spec(t1);
    int_range_t r2 = parse_integer_spec(t2);

    /* If neither side is integer-typed, this rule doesn't apply. */
    if (!r1.integer_typed && !r2.integer_typed) return -1;

    /* Type1 is a non-integer numeric supertype (RATIONAL, REAL, FLOAT,
     * NUMBER, etc.) and type2 is integer-typed.  These supertypes
     * include non-integer values (1/2, 0.5, etc.), so they're certainly
     * NOT subtypes of any integer-only type.  Returns certain NO. */
    if (!r1.integer_typed && r2.integer_typed
        && is_non_integer_numeric_supertype(t1)) {
        return 0;
    }

    /* T1 is integer-typed, T2 is a non-integer numeric supertype
     * (REAL, RATIONAL, NUMBER, ...).  Every integer is a real/rational/
     * number, so the subtype relation holds.  Without this rule
     * (subtypep 'BIT 'REAL) → NIL,NIL which breaks the
     * UPGRADED-COMPLEX-PART-TYPE conformance tests. */
    if (r1.integer_typed && !r2.integer_typed
        && is_non_integer_numeric_supertype(t2)) {
        const char *n2 = cl_symbol_name(t2);
        /* RATIONAL, REAL, NUMBER are all integer supertypes; FLOAT and
         * the specific float kinds are not.  Same for COMPLEX/RATIO. */
        if (strcmp(n2, "RATIONAL") == 0 || strcmp(n2, "REAL") == 0 ||
            strcmp(n2, "NUMBER") == 0)
            return 1;
    }

    /* If only one side is integer-typed, fall through to other rules
     * (the caller's atomic-INTEGER coalescing handles t1 integer ⊆ rational
     * etc., and t2 integer with non-integer t1 returns uncertain). */
    if (!r1.integer_typed || !r2.integer_typed) return -1;

    /* Either side known=0 (e.g. bignum bound) → uncertain. */
    if (!r1.known || !r2.known) return -1;

    /* Empty t1 (lo > hi) means t1 = NIL, which is a subtype of anything. */
    if (r1.lo > r1.hi) return 1;
    /* Empty t2 with non-empty t1 → certainly not a subtype. */
    if (r2.lo > r2.hi) return 0;

    /* r1 ⊆ r2 iff [r1.lo, r1.hi] ⊆ [r2.lo, r2.hi]. */
    return (r1.lo >= r2.lo && r1.hi <= r2.hi) ? 1 : 0;
}

/* Returns 1 if TYPE is an integer subtype whose value set is contained in
 * BIT = (integer 0 1), else 0.  Used by make-array / upgraded-array-element-type
 * so that element-type specs like (integer 0 1), (integer 0 (1)),
 * (unsigned-byte 1) and (mod 2) build a real bit-vector instead of a general
 * T vector — without this, serapeum's RANGE (whose int ranges use such bounds)
 * dispatched to SBIT on a non-bit vector. */
int cl_type_is_bit_subtype(CL_Obj type)
{
    int_range_t r = parse_integer_spec(type);
    return (r.integer_typed && r.known && r.lo >= 0 && r.hi <= 1) ? 1 : 0;
}

/* Returns 1 if TYPE is an integer subtype contained in (unsigned-byte 8) =
 * (integer 0 255), else 0.  Callers must test cl_type_is_bit_subtype FIRST —
 * (unsigned-byte 1) is also a u8 subtype but upgrades to the more specialized
 * bit-vector.  Used by make-array / upgraded-array-element-type so specs like
 * (unsigned-byte 8), (mod 256) and (integer 0 255) build a packed byte
 * vector instead of a general T vector. */
int cl_type_is_u8_subtype(CL_Obj type)
{
    int_range_t r = parse_integer_spec(type);
    return (r.integer_typed && r.known && r.lo >= 0 && r.hi <= 255) ? 1 : 0;
}

/* Returns 1 if TYPE is an integer subtype contained in (signed-byte 8) =
 * (integer -128 127), else 0.  Callers must test the unsigned classifiers
 * first so non-negative ranges prefer the unsigned representation. */
int cl_type_is_s8_subtype(CL_Obj type)
{
    int_range_t r = parse_integer_spec(type);
    return (r.integer_typed && r.known && r.lo >= -128 && r.hi <= 127) ? 1 : 0;
}

/* Same for (unsigned-byte 16) = (integer 0 65535).  Callers must test the
 * 8-bit (and bit) classifiers first so narrower ranges pack tighter. */
int cl_type_is_u16_subtype(CL_Obj type)
{
    int_range_t r = parse_integer_spec(type);
    return (r.integer_typed && r.known && r.lo >= 0 && r.hi <= 65535) ? 1 : 0;
}

/* Same for (signed-byte 16) = (integer -32768 32767). */
int cl_type_is_s16_subtype(CL_Obj type)
{
    int_range_t r = parse_integer_spec(type);
    return (r.integer_typed && r.known && r.lo >= -32768 && r.hi <= 32767) ? 1 : 0;
}

/* Rewrite a couple of exact two-element compound type identities to their
 * atomic equivalent so the pairwise hierarchy can settle them with certainty:
 *   (or null cons)   / (or cons null)     ==  LIST   (a list is NIL or a cons)
 *   (and symbol list) / (and list symbol) ==  NULL   (NIL is the only object
 *                                                      that is both)
 * Returns the canonical symbol when matched, else the type unchanged.  These
 * are genuine identities, so the rewrite is always conforming; it lets
 * ALEXANDRIA:TYPE= settle e.g. (type= 'list '(or null cons)). */
static CL_Obj normalize_type_spec(CL_Obj type)
{
    CL_Obj head, a, b, rest, rest2;
    int ida, idb;
    if (!CL_CONS_P(type)) return type;
    head = cl_car(type);
    if (head != TYPE_SYM_OR && head != TYPE_SYM_AND) return type;
    rest = cl_cdr(type);
    if (!CL_CONS_P(rest)) return type;
    a = cl_car(rest);
    rest2 = cl_cdr(rest);
    if (!CL_CONS_P(rest2)) return type;            /* fewer than 2 elements */
    b = cl_car(rest2);
    if (!CL_NULL_P(cl_cdr(rest2))) return type;     /* more than 2 elements */
    if (!(CL_SYMBOL_P(a) || CL_NULL_P(a))) return type;
    if (!(CL_SYMBOL_P(b) || CL_NULL_P(b))) return type;
    ida = type_name_to_id(cl_symbol_name(a));
    idb = type_name_to_id(cl_symbol_name(b));
    if (head == TYPE_SYM_OR) {
        if ((ida == TID_NULL && idb == TID_CONS) ||
            (ida == TID_CONS && idb == TID_NULL))
            return cl_intern_in("LIST", 4, cl_package_cl);
    } else { /* AND */
        if ((ida == TID_SYMBOL && idb == TID_LIST) ||
            (ida == TID_LIST && idb == TID_SYMBOL))
            return cl_intern_in("NULL", 4, cl_package_cl);
    }
    return type;
}

/* The empty type — NIL, or an element-less (member)/(or).  CLHS: `(member)`
 * with zero arguments denotes the empty type, and `(or)` is likewise empty.
 * Recognising these lets subtypep treat them as equivalent to NIL. */
static int tspec_is_empty(CL_Obj t)
{
    if (CL_NULL_P(t))
        return 1;
    if (CL_CONS_P(t)) {
        CL_Obj head = cl_car(t);
        if ((head == TYPE_SYM_MEMBER || head == TYPE_SYM_OR) &&
            CL_NULL_P(cl_cdr(t)))
            return 1;
    }
    return 0;
}

/* If T is an array-family type specifier (atomic or compound ARRAY /
 * SIMPLE-ARRAY / VECTOR / SIMPLE-VECTOR / STRING / BIT-VECTOR ...), store the
 * upgraded element class it constrains in *CLS (AEC_WILD when it admits any
 * element type) and return 1; return 0 for anything else.
 * GC: compound ARRAY/VECTOR specs route through elt_spec_class, which may run
 * deftype expanders → objects can move; callers protect live locals. */
static int array_family_elt_class(CL_Obj t, int *cls)
{
    const char *n;
    CL_Obj head, tail;
    if (CL_SYMBOL_P(t)) {
        n = cl_symbol_name(t);
        if (strcmp(n, "STRING") == 0 || strcmp(n, "SIMPLE-STRING") == 0 ||
            strcmp(n, "BASE-STRING") == 0 ||
            strcmp(n, "SIMPLE-BASE-STRING") == 0) {
            *cls = AEC_CHAR; return 1;
        }
        if (strcmp(n, "BIT-VECTOR") == 0 ||
            strcmp(n, "SIMPLE-BIT-VECTOR") == 0) {
            *cls = AEC_BIT; return 1;
        }
        if (strcmp(n, "SIMPLE-VECTOR") == 0) { *cls = AEC_T; return 1; }
        if (strcmp(n, "VECTOR") == 0 || strcmp(n, "ARRAY") == 0 ||
            strcmp(n, "SIMPLE-ARRAY") == 0) {
            *cls = AEC_WILD; return 1;
        }
        return 0;
    }
    if (!CL_CONS_P(t)) return 0;
    head = cl_car(t);
    if (!CL_SYMBOL_P(head)) return 0;
    n = cl_symbol_name(head);
    if (strcmp(n, "STRING") == 0 || strcmp(n, "SIMPLE-STRING") == 0 ||
        strcmp(n, "BASE-STRING") == 0 || strcmp(n, "SIMPLE-BASE-STRING") == 0) {
        *cls = AEC_CHAR; return 1;
    }
    if (strcmp(n, "BIT-VECTOR") == 0 || strcmp(n, "SIMPLE-BIT-VECTOR") == 0) {
        *cls = AEC_BIT; return 1;
    }
    if (strcmp(n, "SIMPLE-VECTOR") == 0) { *cls = AEC_T; return 1; }
    if (strcmp(n, "VECTOR") == 0 || strcmp(n, "ARRAY") == 0 ||
        strcmp(n, "SIMPLE-ARRAY") == 0) {
        tail = cl_cdr(t);
        if (CL_NULL_P(tail)) { *cls = AEC_WILD; return 1; }
        *cls = elt_spec_class(cl_car(tail));
        return 1;
    }
    return 0;
}

static CL_Obj bi_subtypep(CL_Obj *args, int n)
{
    CL_Obj type1 = args[0];
    CL_Obj type2 = args[1];
    int id1, id2, result;
    CL_UNUSED(n);

    /* Expand deftype'd symbols before anything else.  The OTHER type local
     * must survive the expander apply — pre-fix, rargs picked up a stale
     * copy captured before the apply compacted. */
    if (CL_SYMBOL_P(type1)) {
        CL_Obj expander = cl_get_type_expander(type1);
        if (!CL_NULL_P(expander)) {
            CL_Obj rargs[2];
            CL_GC_PROTECT(type2);
            type1 = cl_vm_apply(expander, &type1, 0);
            CL_GC_UNPROTECT(1);
            /* Recurse with expanded types */
            rargs[0] = type1; rargs[1] = type2;
            return bi_subtypep(rargs, 2);
        }
    }
    if (CL_SYMBOL_P(type2)) {
        CL_Obj expander = cl_get_type_expander(type2);
        if (!CL_NULL_P(expander)) {
            CL_Obj rargs[2];
            CL_GC_PROTECT(type1);
            type2 = cl_vm_apply(expander, &type2, 0);
            CL_GC_UNPROTECT(1);
            rargs[0] = type1; rargs[1] = type2;
            return bi_subtypep(rargs, 2);
        }
    }

    /* Canonicalize exact compound identities ((or null cons)==list,
     * (and symbol list)==null) to their atomic form, then re-decide. */
    {
        CL_Obj nt1 = normalize_type_spec(type1);
        CL_Obj nt2 = normalize_type_spec(type2);
        if (nt1 != type1 || nt2 != type2) {
            CL_Obj rargs[2];
            rargs[0] = nt1; rargs[1] = nt2;
            return bi_subtypep(rargs, 2);
        }
    }

    /* Structural equality fast-path: identical specs are subtypes. */
    if (tspec_equal_p(type1, type2)) {
        cl_mv_count = 2;
        cl_mv_values[0] = SYM_T;
        cl_mv_values[1] = SYM_T;
        return SYM_T;
    }

    /* T is the universal supertype: every type — including unknown / unbound
     * types — is a subtype of T (CLHS 4.2.5).  Fire this BEFORE TID
     * resolution so undefined deftype symbols (e.g. trivial-cltl2 not
     * loaded → serapeum's variable-type defaults to T) don't fall into
     * the (NIL NIL) "unknown" branch and trigger spurious "Required type
     * X is not a subtypep of declared type T" warnings under serapeum's
     * `assure` macro. */
    if (type2 == SYM_T) {
        cl_mv_count = 2;
        cl_mv_values[0] = SYM_T;
        cl_mv_values[1] = SYM_T;
        return SYM_T;
    }
    /* The empty type (NIL, or an element-less (member)/(or)) is a subtype of
     * every type — CLHS 4.2.3.  Fire this before TID resolution so an empty
     * (member) — which would otherwise reduce to a cons spec — is recognised. */
    if (tspec_is_empty(type1)) {
        cl_mv_count = 2;
        cl_mv_values[0] = SYM_T;
        cl_mv_values[1] = SYM_T;
        return SYM_T;
    }
    /* Symmetric: NIL is the empty type, so only the empty type is a subtype of
     * it (type1-empty is already handled above).  Most non-empty specs return
     * (NIL T), but a compound (AND ... (NOT ...)) may itself be empty — that's
     * the path UPGRADED-COMPLEX-PART-TYPE relies on, so let those fall through
     * to the AND handler. */
    if (tspec_is_empty(type2)) {
        if (!(CL_CONS_P(type1) && cl_car(type1) == TYPE_SYM_AND)) {
            cl_mv_count = 2;
            cl_mv_values[0] = CL_NIL;
            cl_mv_values[1] = SYM_T;
            return CL_NIL;
        }
    }

    /* Compound combinator rules: (OR ...), (AND ...), (MEMBER ...).
     * These fire after symbol-expansion so deftype'd aliases reduce to
     * their bodies before we get here. */
    {
        CL_Obj head1 = CL_CONS_P(type1) ? cl_car(type1) : CL_NIL;
        CL_Obj head2 = CL_CONS_P(type2) ? cl_car(type2) : CL_NIL;

        /* (or A B ...) ⊆ T2 — true iff every Ai ⊆ T2.  A single certain "no"
         * makes the whole thing a certain "no".
         *
         * IMPORTANT: head1=OR must be checked BEFORE head2=OR.  When both
         * type1 and type2 are OR, distributing T1 (recursing each Ai
         * against T2) is what gives the correct answer; the reverse
         * direction (T1 ⊆ Ci for some i) returns a spurious NIL because
         * a multi-branch T1 is rarely contained in any single Ci of T2.
         * Pre-fix, `(subtypep '(or A B C) '(or B C A))` returned (NIL T)
         * which broke serapeum's exhaustiveness checks under
         * typecase-of / etypecase-of. */
        if (head1 == TYPE_SYM_OR) {
            /* The recursion allocates whenever nested specs hit deftype
             * expanders — the walk cursor and the cross-used type local
             * must be rooted across it (same in the walkers below). */
            CL_Obj rest = cl_cdr(type1);
            int saw_unknown = 0;
            CL_GC_PROTECT(rest);
            CL_GC_PROTECT(type2);
            while (!CL_NULL_P(rest)) {
                CL_Obj sub_args[2]; CL_Obj prim;
                sub_args[0] = cl_car(rest); sub_args[1] = type2;
                prim = bi_subtypep(sub_args, 2);
                if (cl_mv_values[1] == SYM_T && prim == CL_NIL) {
                    CL_GC_UNPROTECT(2);
                    cl_mv_count = 2;
                    cl_mv_values[0] = CL_NIL;
                    cl_mv_values[1] = SYM_T;
                    return CL_NIL;
                }
                if (cl_mv_values[1] != SYM_T) saw_unknown = 1;
                rest = cl_cdr(rest);
            }
            CL_GC_UNPROTECT(2);
            cl_mv_count = 2;
            cl_mv_values[0] = saw_unknown ? CL_NIL : SYM_T;
            cl_mv_values[1] = saw_unknown ? CL_NIL : SYM_T;
            return cl_mv_values[0];
        }

        /* T1 ⊆ (or A B ...) — true if T1 ⊆ Ai for some i; otherwise unknown
         * unless every branch returned a certain "no". */
        if (head2 == TYPE_SYM_OR) {
            CL_Obj rest = cl_cdr(type2);
            int saw_unknown = 0;
            CL_GC_PROTECT(rest);
            CL_GC_PROTECT(type1);
            while (!CL_NULL_P(rest)) {
                CL_Obj sub_args[2]; CL_Obj prim;
                sub_args[0] = type1; sub_args[1] = cl_car(rest);
                prim = bi_subtypep(sub_args, 2);
                if (cl_mv_values[1] == SYM_T && prim == SYM_T) {
                    CL_GC_UNPROTECT(2);
                    cl_mv_count = 2;
                    cl_mv_values[0] = SYM_T;
                    cl_mv_values[1] = SYM_T;
                    return SYM_T;
                }
                if (cl_mv_values[1] != SYM_T) saw_unknown = 1;
                rest = cl_cdr(rest);
            }
            CL_GC_UNPROTECT(2);
            cl_mv_count = 2;
            cl_mv_values[0] = CL_NIL;
            cl_mv_values[1] = saw_unknown ? CL_NIL : SYM_T;
            return CL_NIL;
        }

        /* (and A B ...) ⊆ T2 — true if any Ai ⊆ T2 (intersection narrows).
         * Special case: if the AND contains both Xi and (NOT Yj) where
         * Xi ⊆ Yj, the intersection is empty — i.e. ⊆ NIL, hence ⊆
         * anything.  This is what makes
         *   (subtypep '(and integer (not real)) nil)
         * return T,T (the ANSI UPGRADED-COMPLEX-PART-TYPE tests rely on
         * this rule). */
        if (head1 == TYPE_SYM_AND) {
            CL_Obj rest;
            int saw_unknown = 0;
            /* type1/type2 stay rooted for the whole branch: every recursive
             * bi_subtypep below can compact, and the cursors re-derive from
             * type1 afterward. */
            CL_GC_PROTECT(type1);
            CL_GC_PROTECT(type2);
            /* Empty-intersection check: scan for (NOT Yj), then check each
             * non-NOT Xi ⊆ Yj. */
            {
                CL_Obj r1 = cl_cdr(type1);
                CL_GC_PROTECT(r1);
                while (!CL_NULL_P(r1)) {
                    CL_Obj cl1 = cl_car(r1);
                    if (CL_CONS_P(cl1) && cl_car(cl1) == TYPE_SYM_NOT &&
                        CL_CONS_P(cl_cdr(cl1))) {
                        CL_Obj y = cl_car(cl_cdr(cl1));
                        CL_Obj r2 = cl_cdr(type1);
                        /* cl1 is compared by identity against cl2 on every
                         * inner iteration — it must be forwarded too, or a
                         * stale offset makes the != test misfire. */
                        CL_GC_PROTECT(cl1);
                        CL_GC_PROTECT(y);
                        CL_GC_PROTECT(r2);
                        while (!CL_NULL_P(r2)) {
                            CL_Obj cl2 = cl_car(r2);
                            if (cl2 != cl1 &&
                                !(CL_CONS_P(cl2) && cl_car(cl2) == TYPE_SYM_NOT)) {
                                CL_Obj sub_args[2]; CL_Obj prim;
                                sub_args[0] = cl2; sub_args[1] = y;
                                prim = bi_subtypep(sub_args, 2);
                                if (cl_mv_values[1] == SYM_T && prim == SYM_T) {
                                    /* (and ... Xi ... (not Yj)) is empty */
                                    CL_GC_UNPROTECT(6);
                                    cl_mv_count = 2;
                                    cl_mv_values[0] = SYM_T;
                                    cl_mv_values[1] = SYM_T;
                                    return SYM_T;
                                }
                            }
                            r2 = cl_cdr(r2);
                        }
                        CL_GC_UNPROTECT(3);
                    }
                    r1 = cl_cdr(r1);
                }
                CL_GC_UNPROTECT(1);
            }
            rest = cl_cdr(type1);
            CL_GC_PROTECT(rest);
            while (!CL_NULL_P(rest)) {
                CL_Obj sub_args[2]; CL_Obj prim;
                sub_args[0] = cl_car(rest); sub_args[1] = type2;
                prim = bi_subtypep(sub_args, 2);
                if (cl_mv_values[1] == SYM_T && prim == SYM_T) {
                    CL_GC_UNPROTECT(3);
                    cl_mv_count = 2;
                    cl_mv_values[0] = SYM_T;
                    cl_mv_values[1] = SYM_T;
                    return SYM_T;
                }
                if (cl_mv_values[1] != SYM_T) saw_unknown = 1;
                rest = cl_cdr(rest);
            }
            CL_GC_UNPROTECT(3);
            cl_mv_count = 2;
            cl_mv_values[0] = CL_NIL;
            cl_mv_values[1] = saw_unknown ? CL_NIL : SYM_T;
            return CL_NIL;
        }

        /* T1 ⊆ (and A B ...) iff T1 ⊆ Ai for every i.  Any certain "no"
         * proves the whole result. */
        if (head2 == TYPE_SYM_AND) {
            CL_Obj rest = cl_cdr(type2);
            int saw_unknown = 0;
            CL_GC_PROTECT(rest);
            CL_GC_PROTECT(type1);
            while (!CL_NULL_P(rest)) {
                CL_Obj sub_args[2]; CL_Obj prim;
                sub_args[0] = type1; sub_args[1] = cl_car(rest);
                prim = bi_subtypep(sub_args, 2);
                if (cl_mv_values[1] == SYM_T && prim == CL_NIL) {
                    CL_GC_UNPROTECT(2);
                    cl_mv_count = 2;
                    cl_mv_values[0] = CL_NIL;
                    cl_mv_values[1] = SYM_T;
                    return CL_NIL;
                }
                if (cl_mv_values[1] != SYM_T) saw_unknown = 1;
                rest = cl_cdr(rest);
            }
            CL_GC_UNPROTECT(2);
            cl_mv_count = 2;
            cl_mv_values[0] = saw_unknown ? CL_NIL : SYM_T;
            cl_mv_values[1] = saw_unknown ? CL_NIL : SYM_T;
            return cl_mv_values[0];
        }

        /* T1 ⊆ (not B) iff T1 ∩ B = ∅.  We answer the certain "no" case: if
         * T1 ⊆ B (with certainty) and T1 is non-empty, then T1 overlaps B and
         * is certainly NOT a subtype of (not B).  e.g. (subtypep 'null '(not
         * null)) => (NIL T), which serapeum's SOFT-LIST-OF deftype relies on to
         * pick PROPER-LIST-WITHOUT-NIL? over PROPER-LIST?.  Full disjointness
         * reasoning (the certain "yes" case) is left uncertain. */
        if (head2 == TYPE_SYM_NOT && CL_CONS_P(cl_cdr(type2)) &&
            !CL_NULL_P(type1)) {
            CL_Obj inner = cl_car(cl_cdr(type2));
            CL_Obj sub_args[2]; CL_Obj prim;
            /* This branch can FALL THROUGH — type1/type2 (and head1, used
             * below) are still live after the allocating recursion. */
            CL_GC_PROTECT(type1);
            CL_GC_PROTECT(type2);
            sub_args[0] = type1; sub_args[1] = inner;
            prim = bi_subtypep(sub_args, 2);
            CL_GC_UNPROTECT(2);
            head1 = CL_CONS_P(type1) ? cl_car(type1) : head1;
            if (prim == SYM_T && cl_mv_values[1] == SYM_T) {
                cl_mv_count = 2;
                cl_mv_values[0] = CL_NIL;
                cl_mv_values[1] = SYM_T;
                return CL_NIL;
            }
            /* else fall through to the uncertain default */
        }

        /* (member x1 x2 ...) ⊆ T2 iff every xi is typep T2. */
        if (head1 == TYPE_SYM_MEMBER) {
            CL_Obj rest = cl_cdr(type1);
            CL_GC_PROTECT(rest);
            CL_GC_PROTECT(type2);
            while (!CL_NULL_P(rest)) {
                if (!cl_typep(cl_car(rest), type2)) {
                    CL_GC_UNPROTECT(2);
                    cl_mv_count = 2;
                    cl_mv_values[0] = CL_NIL;
                    cl_mv_values[1] = SYM_T;
                    return CL_NIL;
                }
                rest = cl_cdr(rest);
            }
            CL_GC_UNPROTECT(2);
            cl_mv_count = 2;
            cl_mv_values[0] = SYM_T;
            cl_mv_values[1] = SYM_T;
            return SYM_T;
        }
    }

    /* Class objects as type specifiers: extract class name (slot 0) */
    if (CL_STRUCT_P(type1)) {
        CL_Struct *st = (CL_Struct *)CL_OBJ_TO_PTR(type1);
        type1 = st->slots[0]; /* class name */
    }
    if (CL_STRUCT_P(type2)) {
        CL_Struct *st = (CL_Struct *)CL_OBJ_TO_PTR(type2);
        type2 = st->slots[0]; /* class name */
    }

    /* (complex part-type) — since cl-amiga doesn't specialize complex
     * storage, UPGRADED-COMPLEX-PART-TYPE always returns REAL.  Two
     * (complex X) specs are therefore mutually subtype, and any
     * (complex X) is equivalent to bare COMPLEX.  Collapse to atomic
     * COMPLEX so the rest of the chain handles them uniformly. */
    if (CL_CONS_P(type1)) {
        CL_Obj head = cl_car(type1);
        if (CL_SYMBOL_P(head) && strcmp(cl_symbol_name(head), "COMPLEX") == 0)
            type1 = cl_intern_in("COMPLEX", 7, cl_package_cl);
    }
    if (CL_CONS_P(type2)) {
        CL_Obj head = cl_car(type2);
        if (CL_SYMBOL_P(head) && strcmp(cl_symbol_name(head), "COMPLEX") == 0)
            type2 = cl_intern_in("COMPLEX", 7, cl_package_cl);
    }

    /* Compound integer-range specifiers: do range arithmetic before
     * collapsing to atomic INTEGER.  Handles `(integer L1 H1) ⊆ (integer L2 H2)`
     * (range subset), `(integer L1 H1) ⊆ FIXNUM` (bounds inside fixnum range),
     * and `(integer L1 H1) ⊆ (signed-byte N) / (unsigned-byte N) / (mod N) / BIT`
     * via canonicalization.  Returns 1 if the rule fired (and populated
     * cl_mv_values); 0 to fall through to atomic-INTEGER handling. */
    {
        extern int cl_subtypep_integer_ranges(CL_Obj t1, CL_Obj t2);
        int result_code = cl_subtypep_integer_ranges(type1, type2);
        if (result_code != -1) {
            /* result_code: 1 = certain yes, 0 = certain no */
            cl_mv_count = 2;
            cl_mv_values[0] = result_code ? SYM_T : CL_NIL;
            cl_mv_values[1] = SYM_T;
            return result_code ? SYM_T : CL_NIL;
        }
    }

    /* If we fall through, either the integer-range rule didn't apply
     * (one side wasn't an integer-y type) or the answer is uncertain.
     * Collapse compound integer types on the LEFT to atomic INTEGER so
     * the rest of the chain can use the type-id hierarchy. */
    if (CL_CONS_P(type1)) {
        CL_Obj head = cl_car(type1);
        if (CL_SYMBOL_P(head)) {
            const char *hn = cl_symbol_name(head);
            if (strcmp(hn, "INTEGER") == 0 ||
                strcmp(hn, "SIGNED-BYTE") == 0 ||
                strcmp(hn, "UNSIGNED-BYTE") == 0 ||
                strcmp(hn, "MOD") == 0)
                type1 = cl_intern_in("INTEGER", 7, cl_package_cl);
        }
    }
    /* Collapse a *bounded* float/real/rational range on the LEFT to its
     * atomic base type.  A bounded numeric interval (e.g. (single-float 0.0
     * (10.0))) is always a subtype of its unbounded base kind and that kind's
     * supertypes, so the type-id hierarchy below then settles
     *   (subtypep '(single-float 0.0 (10.0)) 'number)  => T
     *   (subtypep '(single-float 0.0 (10.0)) 'float)   => T
     * (serapeum's random-range-type asserts exactly these).  Only the LEFT
     * (candidate subtype) side is collapsed: a bounded numeric type on the
     * RIGHT stays unrecognized (id2 = UNKNOWN → uncertain) so we never falsely
     * report a narrowing such as (subtypep 'single-float '(single-float 0 1)).
     * Integer ranges are handled with exact arithmetic above and are left
     * untouched here. */
    if (CL_CONS_P(type1)) {
        CL_Obj head = cl_car(type1);
        if (CL_SYMBOL_P(head)) {
            const char *hn = cl_symbol_name(head);
            const char *base = NULL;
            if (strcmp(hn, "SINGLE-FLOAT") == 0 || strcmp(hn, "SHORT-FLOAT") == 0)
                base = "SINGLE-FLOAT";
            else if (strcmp(hn, "DOUBLE-FLOAT") == 0 || strcmp(hn, "LONG-FLOAT") == 0)
                base = "DOUBLE-FLOAT";
            else if (strcmp(hn, "FLOAT") == 0)
                base = "FLOAT";
            else if (strcmp(hn, "REAL") == 0)
                base = "REAL";
            else if (strcmp(hn, "RATIONAL") == 0)
                base = "RATIONAL";
            if (base)
                type1 = cl_intern_in(base, (int)strlen(base), cl_package_cl);
        }
    }
    if (CL_CONS_P(type2)) {
        CL_Obj head = cl_car(type2);
        if (CL_SYMBOL_P(head)) {
            const char *hn = cl_symbol_name(head);
            /* Bounded integer on the right with non-integer LHS — we
             * don't know.  (Integer-on-both-sides was handled by
             * cl_subtypep_integer_ranges above.) */
            if (strcmp(hn, "INTEGER") == 0 ||
                strcmp(hn, "SIGNED-BYTE") == 0 ||
                strcmp(hn, "UNSIGNED-BYTE") == 0 ||
                strcmp(hn, "MOD") == 0) {
                cl_mv_count = 2;
                cl_mv_values[0] = CL_NIL;
                cl_mv_values[1] = CL_NIL;
                return CL_NIL;
            }
        }
    }

    /* Array-family element-type guard (CLHS 15.1.2.2: array types are
     * subtypes only when their UPGRADED element types agree).  The TID
     * hierarchy below deliberately drops element types, which used to make
     * ALL specialized array types mutual subtypes — e.g.
     *   (subtypep '(simple-array (signed-byte 8) (*))
     *             '(simple-array integer (*)))          => T,T   (wrong)
     * contradicting TYPEP.  serapeum's WITH-TYPE-DISPATCH dedups its branches
     * via subtypep, so the contradiction collapsed all its specialized
     * branches and its ETYPECASE then rejected the very vector
     * (serapeum:range -5 0) had just built.  Distinct element classes are a
     * certain "no"; a wildcard LHS against a constrained RHS likewise (the
     * LHS admits arrays of every element class).  Compatible classes fall
     * through to the TID hierarchy for the shape/simplicity dimension. */
    {
        int cls1 = AEC_WILD, cls2 = AEC_WILD;
        int fam1, fam2;
        /* array_family_elt_class may run deftype expanders → compaction can
         * move type1/type2 while held in these C locals. */
        CL_GC_PROTECT(type1);
        CL_GC_PROTECT(type2);
        fam1 = array_family_elt_class(type1, &cls1);
        fam2 = fam1 ? array_family_elt_class(type2, &cls2) : 0;
        CL_GC_UNPROTECT(2);
        if (fam1 && fam2 && cls2 != AEC_WILD && cls1 != cls2) {
            cl_mv_count = 2;
            cl_mv_values[0] = CL_NIL;
            cl_mv_values[1] = SYM_T;
            return CL_NIL;
        }
    }

    /* Resolve TIDs.  Compound array/vector specifiers go through
     * compound_type_to_tid; symbol specifiers through type_name_to_id. */
    if (CL_SYMBOL_P(type1) || CL_NULL_P(type1)) {
        id1 = type_name_to_id(cl_symbol_name(type1));
    } else if (CL_CONS_P(type1)) {
        id1 = compound_type_to_tid(type1);
    } else {
        id1 = TID_UNKNOWN;
    }
    if (CL_SYMBOL_P(type2) || CL_NULL_P(type2)) {
        id2 = type_name_to_id(cl_symbol_name(type2));
    } else if (CL_CONS_P(type2)) {
        id2 = compound_type_to_tid(type2);
    } else {
        id2 = TID_UNKNOWN;
    }

    if (id1 == TID_UNKNOWN || id2 == TID_UNKNOWN) {
        /* Check struct type hierarchy (CLOS classes) */
        extern int cl_struct_type_matches(CL_Obj obj_type, CL_Obj test_type);
        extern int cl_is_struct_type(CL_Obj type_sym);
        if (CL_SYMBOL_P(type1) && CL_SYMBOL_P(type2) &&
            cl_is_struct_type(type1) && cl_is_struct_type(type2)) {
            result = cl_struct_type_matches(type1, type2);
            cl_mv_count = 2;
            cl_mv_values[0] = result ? SYM_T : CL_NIL;
            cl_mv_values[1] = SYM_T;
            return result ? SYM_T : CL_NIL;
        }
        /* Condition type hierarchy: walk the full (multi-parent) superclass
         * graph so (subtypep 'socket-error 'error) is certain T even when ERROR
         * is not the first superclass.  Same registry TYPEP / handler-case use. */
        {
            extern int cl_condition_type_matches(CL_Obj cond_type, CL_Obj handler_type);
            extern int cl_is_condition_type(CL_Obj type_sym);
            if (CL_SYMBOL_P(type1) && CL_SYMBOL_P(type2) &&
                cl_is_condition_type(type1) && cl_is_condition_type(type2)) {
                result = cl_condition_type_matches(type1, type2);
                cl_mv_count = 2;
                cl_mv_values[0] = result ? SYM_T : CL_NIL;
                cl_mv_values[1] = SYM_T;
                return result ? SYM_T : CL_NIL;
            }
        }
        /* Unknown type: return (NIL NIL) */
        cl_mv_count = 2;
        cl_mv_values[0] = CL_NIL;
        cl_mv_values[1] = CL_NIL;
        return CL_NIL;
    }

    result = subtype_check(id1, id2);
    cl_mv_count = 2;
    cl_mv_values[0] = result ? SYM_T : CL_NIL;
    cl_mv_values[1] = SYM_T;  /* second value = "certain" */
    return result ? SYM_T : CL_NIL;
}

/* (clamiga::%type-expander NAME) → the deftype expander closure registered
 * for the symbol NAME, or NIL if NAME has no deftype expander.
 *
 * Exposes the internal deftype type_table to Lisp so a portable typexpand
 * can be implemented on top of it (see the AmiLisp introspect-environment
 * fork's #+cl-amiga typexpand in default.lisp).  The returned closure takes the
 * compound type's arguments — e.g. the expander for `(deftype foo (a b) ...)`
 * is applied to (a b); an atom deftype's expander takes no arguments. */
static CL_Obj bi_type_expander(CL_Obj *args, int n)
{
    (void)n;
    if (!CL_SYMBOL_P(args[0]))
        return CL_NIL;
    return cl_get_type_expander(args[0]);
}

/* --- Public typep API --- */

int cl_typep(CL_Obj obj, CL_Obj type_spec)
{
    return typep_check(obj, type_spec);
}

/* --- Registration --- */

void cl_builtins_type_init(void)
{
    /* Pre-intern compound type specifier head symbols */
    TYPE_SYM_OR        = cl_intern_in("OR", 2, cl_package_cl);
    TYPE_SYM_AND       = cl_intern_in("AND", 3, cl_package_cl);
    TYPE_SYM_NOT       = cl_intern_in("NOT", 3, cl_package_cl);
    TYPE_SYM_MEMBER    = cl_intern_in("MEMBER", 6, cl_package_cl);
    TYPE_SYM_EQL       = cl_intern_in("EQL", 3, cl_package_cl);
    TYPE_SYM_SATISFIES = cl_intern_in("SATISFIES", 9, cl_package_cl);
    /* Numeric range type specifier head symbols */
    TYPE_SYM_INTEGER      = cl_intern_in("INTEGER", 7, cl_package_cl);
    TYPE_SYM_RATIONAL     = cl_intern_in("RATIONAL", 8, cl_package_cl);
    TYPE_SYM_REAL         = cl_intern_in("REAL", 4, cl_package_cl);
    TYPE_SYM_FLOAT        = cl_intern_in("FLOAT", 5, cl_package_cl);
    TYPE_SYM_SINGLE_FLOAT = cl_intern_in("SINGLE-FLOAT", 12, cl_package_cl);
    TYPE_SYM_SHORT_FLOAT  = cl_intern_in("SHORT-FLOAT", 11, cl_package_cl);
    TYPE_SYM_DOUBLE_FLOAT = cl_intern_in("DOUBLE-FLOAT", 12, cl_package_cl);
    TYPE_SYM_LONG_FLOAT   = cl_intern_in("LONG-FLOAT", 10, cl_package_cl);
    TYPE_SYM_FIXNUM       = cl_intern_in("FIXNUM", 6, cl_package_cl);
    TYPE_SYM_BIGNUM       = cl_intern_in("BIGNUM", 6, cl_package_cl);
    TYPE_SYM_RATIO        = cl_intern_in("RATIO", 5, cl_package_cl);
    TYPE_SYM_NUMBER       = cl_intern_in("NUMBER", 6, cl_package_cl);
    TYPE_SYM_STAR         = cl_intern_in("*", 1, cl_package_cl);
    /* Compound vector/array type specifier head symbols */
    TYPE_SYM_SIMPLE_VECTOR     = cl_intern_in("SIMPLE-VECTOR", 13, cl_package_cl);
    TYPE_SYM_VECTOR            = cl_intern_in("VECTOR", 6, cl_package_cl);
    TYPE_SYM_SIMPLE_ARRAY      = cl_intern_in("SIMPLE-ARRAY", 12, cl_package_cl);
    TYPE_SYM_ARRAY             = cl_intern_in("ARRAY", 5, cl_package_cl);
    TYPE_SYM_STRING            = cl_intern_in("STRING", 6, cl_package_cl);
    TYPE_SYM_SIMPLE_STRING     = cl_intern_in("SIMPLE-STRING", 13, cl_package_cl);
    TYPE_SYM_BASE_STRING       = cl_intern_in("BASE-STRING", 11, cl_package_cl);
    TYPE_SYM_SIMPLE_BASE_STRING = cl_intern_in("SIMPLE-BASE-STRING", 18, cl_package_cl);
    TYPE_SYM_BIT_VECTOR        = cl_intern_in("BIT-VECTOR", 10, cl_package_cl);
    TYPE_SYM_SIMPLE_BIT_VECTOR = cl_intern_in("SIMPLE-BIT-VECTOR", 17, cl_package_cl);

    /* Stamp the standard type names with their TYPEP dispatch codes. */
    cl_typep_codes_init();

    defun("TYPE-OF", bi_type_of, 1, 1);
    /* CLHS: typep and subtypep take an optional ENV argument.  We
     * don't carry environment objects — accept and ignore it. */
    defun("TYPEP", bi_typep, 2, 3);
    defun("UPGRADED-COMPLEX-PART-TYPE", bi_upgraded_complex_part_type, 1, 2);
    defun("COERCE", bi_coerce, 2, 2);
    defun("SUBTYPEP", bi_subtypep, 2, 3);
    /* Internal: expose the deftype expander table for a portable typexpand
     * (introspect-environment shim). */
    cl_register_builtin("%TYPE-EXPANDER", bi_type_expander, 1, 1,
                        cl_package_clamiga);

    /* Register cached symbols for GC compaction forwarding */
    cl_gc_register_root(&TYPE_SYM_OR);
    cl_gc_register_root(&TYPE_SYM_AND);
    cl_gc_register_root(&TYPE_SYM_NOT);
    cl_gc_register_root(&TYPE_SYM_MEMBER);
    cl_gc_register_root(&TYPE_SYM_EQL);
    cl_gc_register_root(&TYPE_SYM_SATISFIES);
    cl_gc_register_root(&TYPE_SYM_INTEGER);
    cl_gc_register_root(&TYPE_SYM_RATIONAL);
    cl_gc_register_root(&TYPE_SYM_REAL);
    cl_gc_register_root(&TYPE_SYM_FLOAT);
    cl_gc_register_root(&TYPE_SYM_SINGLE_FLOAT);
    cl_gc_register_root(&TYPE_SYM_SHORT_FLOAT);
    cl_gc_register_root(&TYPE_SYM_DOUBLE_FLOAT);
    cl_gc_register_root(&TYPE_SYM_LONG_FLOAT);
    cl_gc_register_root(&TYPE_SYM_FIXNUM);
    cl_gc_register_root(&TYPE_SYM_BIGNUM);
    cl_gc_register_root(&TYPE_SYM_RATIO);
    cl_gc_register_root(&TYPE_SYM_NUMBER);
    cl_gc_register_root(&TYPE_SYM_STAR);
    cl_gc_register_root(&TYPE_SYM_SIMPLE_VECTOR);
    cl_gc_register_root(&TYPE_SYM_VECTOR);
    cl_gc_register_root(&TYPE_SYM_SIMPLE_ARRAY);
    cl_gc_register_root(&TYPE_SYM_ARRAY);
    cl_gc_register_root(&TYPE_SYM_STRING);
    cl_gc_register_root(&TYPE_SYM_SIMPLE_STRING);
    cl_gc_register_root(&TYPE_SYM_BASE_STRING);
    cl_gc_register_root(&TYPE_SYM_SIMPLE_BASE_STRING);
    cl_gc_register_root(&TYPE_SYM_BIT_VECTOR);
    cl_gc_register_root(&TYPE_SYM_SIMPLE_BIT_VECTOR);
    cl_gc_register_root(&TYPE_SYM_GRAY_FUNDAMENTAL_STREAM);
}
