#include "builtins.h"
#include "symbol.h"
#include "package.h"
#include "mem.h"
#include "error.h"
#include "string_utils.h"
#include "thread.h"
#include "compiler.h"  /* cl_get_type_expander */
#include "vm.h"        /* cl_vm_apply */
#include "opcodes.h"   /* CL_AREF_KIND_* */
#include "../platform/platform.h"
#include <string.h>

/* Create a fresh simple character array (string) of LENGTH.  When
 * WANT_WIDE is set (an explicit CHARACTER / EXTENDED-CHAR element-type, or
 * content that needs the full character range) this is a TYPE_WIDE_STRING
 * under CL_WIDE_STRINGS so chars > 255 stored later are not truncated.
 * Otherwise it is a narrow TYPE_STRING — a BASE-STRING — matching
 * MAKE-STRING's behaviour for a BASE-CHAR element-type.  Keeping the two
 * builtins consistent is required so (TYPEP s 'BASE-STRING) agrees with
 * how the string was requested (e.g. FSet's seq leaves rely on it). */
static CL_Obj make_char_array_string(uint32_t length, int want_wide)
{
#ifdef CL_WIDE_STRINGS
    if (want_wide)
        return cl_make_wide_string(NULL, length);
    return cl_make_string(NULL, length);
#else
    (void)want_wide;
    return cl_make_string(NULL, length);
#endif
}

/* Create a fresh string of LENGTH whose storage width matches SRC, so a
 * displaced/copy of a wide source preserves characters > 255. */
static CL_Obj make_string_like(CL_Obj src, uint32_t length)
{
#ifdef CL_WIDE_STRINGS
    if (CL_WIDE_STRING_P(src)) return cl_make_wide_string(NULL, length);
#endif
    (void)src;
    return cl_make_string(NULL, length);
}

/* Classify an array element-type specifier, EXPANDING user deftypes, so that
 * e.g. flexi-streams' (deftype char* () 'character) upgrades to a CHARACTER
 * array (string) rather than a general (vector t).  Sets the out-flags it can
 * determine and leaves the rest untouched:
 *   *is_char       — a CHARACTER subtype (build a string)
 *   *is_wide_char  — that subtype admits codes > 255 (CHARACTER / EXTENDED-CHAR)
 *   *is_bit        — a BIT subtype (build a bit-vector)
 *   *is_u8 / *is_s8 — an (unsigned-byte 8) / (signed-byte 8) subtype that is
 *                     NOT a bit subtype (build a packed byte vector).  Checked
 *                     in specialization order bit > u8 > s8, so (unsigned-byte 1)
 *                     stays a bit-vector and (integer 0 100) prefers unsigned.
 * NIL is handled by the callers; unrecognised specifiers stay a general T
 * array.  DEPTH bounds the deftype-expansion recursion.
 *
 * GC: cl_get_type_expander/cl_vm_apply may compact.  Callers pass an element
 * type that is already GC-rooted (a builtin arg slot) or protect their live
 * locals around the call; no unrooted CL_Obj is held across the apply here.
 * Shared with TYPEP/SUBTYPEP in builtins_type.c (declared in builtins.h) so
 * type tests agree with what MAKE-ARRAY actually builds. */
void cl_classify_array_elt_type(CL_Obj type, int depth,
                                int *is_char, int *is_wide_char, int *is_bit,
                                int *is_u8, int *is_s8,
                                int *is_u16, int *is_s16)
{
    const char *nm;
    CL_Obj ex;
    if (depth <= 0)
        return;
    /* Compound integer subtypes — e.g. (integer 0 1) / (unsigned-byte 1) /
     * (mod 2) upgrade to a bit-vector; (unsigned-byte 8) / (mod 256) /
     * (integer 0 255) to an unsigned byte vector; (signed-byte 8) /
     * (integer -128 127) to a signed byte vector; 9-16-bit ranges to the
     * 16-bit packed kinds.  Tested narrowest-first so a range that fits a
     * smaller representation packs tighter (mirrors CLHS upgading). */
    if (CL_CONS_P(type)) {
        extern int cl_type_is_bit_subtype(CL_Obj type);
        extern int cl_type_is_u8_subtype(CL_Obj type);
        extern int cl_type_is_s8_subtype(CL_Obj type);
        extern int cl_type_is_u16_subtype(CL_Obj type);
        extern int cl_type_is_s16_subtype(CL_Obj type);
        if (cl_type_is_bit_subtype(type)) *is_bit = 1;
        else if (cl_type_is_u8_subtype(type)) *is_u8 = 1;
        else if (cl_type_is_s8_subtype(type)) *is_s8 = 1;
        else if (cl_type_is_u16_subtype(type)) *is_u16 = 1;
        else if (cl_type_is_s16_subtype(type)) *is_s16 = 1;
        return;
    }
    if (!CL_SYMBOL_P(type))
        return;
    nm = cl_symbol_name(type);
    if (strcmp(nm, "CHARACTER") == 0 || strcmp(nm, "EXTENDED-CHAR") == 0) {
        *is_char = 1; *is_wide_char = 1; return;
    }
    if (strcmp(nm, "BASE-CHAR") == 0 || strcmp(nm, "STANDARD-CHAR") == 0) {
        *is_char = 1; return;
    }
    if (strcmp(nm, "BIT") == 0) { *is_bit = 1; return; }
    if (strcmp(nm, "T") == 0 || strcmp(nm, "*") == 0)
        return;
    /* A user deftype alias — expand once and recurse. */
    ex = cl_get_type_expander(type);
    if (!CL_NULL_P(ex))
        cl_classify_array_elt_type(cl_vm_apply(ex, NULL, 0), depth - 1,
                                is_char, is_wide_char, is_bit, is_u8, is_s8,
                                is_u16, is_s16);
}

/* Classify a general (non-bit, non-char) array element-type into a
 * CL_VEC_ELT_* code, expanding user deftypes.  Returns CL_VEC_ELT_T for T,
 * compound specifiers, and anything unrecognised — amilisp only specializes
 * the numeric element types real implementations would not upgrade to T
 * (FIXNUM, SINGLE/DOUBLE-FLOAT), so a general (VECTOR T) and a
 * (VECTOR FIXNUM) stay distinguishable.  Elements are still stored as tagged
 * CL_Obj regardless of code.  GC: cl_vm_apply may compact, but TYPE is a
 * GC-rooted builtin arg and no unrooted CL_Obj is held across the apply.
 * Shared with builtins_type.c's COERCE (declared in builtins.h) — it used to
 * carry a byte-identical `coerce_general_elt_code` copy. */
uint8_t cl_classify_vec_elt_code(CL_Obj type, int depth)
{
    const char *nm;
    CL_Obj ex;
    if (depth <= 0 || !CL_SYMBOL_P(type))
        return CL_VEC_ELT_T;
    nm = cl_symbol_name(type);
    if (strcmp(nm, "FIXNUM") == 0)
        return CL_VEC_ELT_FIXNUM;
    if (strcmp(nm, "SINGLE-FLOAT") == 0 || strcmp(nm, "SHORT-FLOAT") == 0)
        return CL_VEC_ELT_SINGLE_FLOAT;
    if (strcmp(nm, "DOUBLE-FLOAT") == 0 || strcmp(nm, "LONG-FLOAT") == 0)
        return CL_VEC_ELT_DOUBLE_FLOAT;
    ex = cl_get_type_expander(type);
    if (!CL_NULL_P(ex))
        return cl_classify_vec_elt_code(cl_vm_apply(ex, NULL, 0), depth - 1);
    return CL_VEC_ELT_T;
}

/* Build the (UNSIGNED-BYTE 8/16) / (SIGNED-BYTE 8/16) type-specifier list
 * for a byte vector.  Used by ARRAY-ELEMENT-TYPE and
 * UPGRADED-ARRAY-ELEMENT-TYPE.
 * GC: the tail cons is protected across the (allocating) intern. */
static CL_Obj bytevec_element_type_spec(int is_signed, int elt_shift)
{
    CL_Obj tail = cl_cons(CL_MAKE_FIXNUM(elt_shift ? 16 : 8), CL_NIL);
    CL_Obj sym;
    CL_GC_PROTECT(tail);
    sym = is_signed ? cl_intern("SIGNED-BYTE", 11)
                    : cl_intern("UNSIGNED-BYTE", 13);
    CL_GC_UNPROTECT(1);
    return cl_cons(sym, tail);
}

/* Map a CL_VEC_ELT_* code to its element-type symbol (T for the general
 * case).  Used by ARRAY-ELEMENT-TYPE and UPGRADED-ARRAY-ELEMENT-TYPE. */
static CL_Obj elt_code_to_type(uint8_t code)
{
    switch (code) {
    case CL_VEC_ELT_FIXNUM:       return cl_intern("FIXNUM", 6);
    case CL_VEC_ELT_SINGLE_FLOAT: return cl_intern("SINGLE-FLOAT", 12);
    case CL_VEC_ELT_DOUBLE_FLOAT: return cl_intern("DOUBLE-FLOAT", 12);
    case CL_VEC_ELT_U8:           return bytevec_element_type_spec(0, 0);
    case CL_VEC_ELT_S8:           return bytevec_element_type_spec(1, 0);
    case CL_VEC_ELT_U16:          return bytevec_element_type_spec(0, 1);
    case CL_VEC_ELT_S16:          return bytevec_element_type_spec(1, 1);
    default:                      return SYM_T;
    }
}

/* The exact element-type name of a packed vector kind, for diagnostics. */
const char *cl_bytevec_type_name(int is_signed, int elt_shift)
{
    if (elt_shift)
        return is_signed ? "(SIGNED-BYTE 16)" : "(UNSIGNED-BYTE 16)";
    return is_signed ? "(SIGNED-BYTE 8)" : "(UNSIGNED-BYTE 8)";
}

/* Range-check a byte-vector element VALUE against the vector's width and
 * signedness and return the raw element value to store (pass to
 * cl_bytevec_set).  Signals a catchable TYPE-ERROR naming the offending
 * value and the exact expected subtype otherwise.  Every 8/16-bit element
 * value fits a fixnum, so a non-fixnum is never in range.  Shared with the
 * sequence/string/VM/JIT store paths (declared in builtins.h). */
int32_t cl_bytevec_check_value(CL_Obj value, int is_signed, int elt_shift,
                               const char *ctx)
{
    int32_t v = 0, lo, hi;
    int bad = !CL_FIXNUM_P(value);
    if (elt_shift) { lo = is_signed ? -32768 : 0; hi = is_signed ? 32767 : 65535; }
    else           { lo = is_signed ? -128   : 0; hi = is_signed ? 127   : 255; }
    if (!bad) {
        v = CL_FIXNUM_VAL(value);
        bad = (v < lo || v > hi);
    }
    if (bad)
        cl_signal_type_error(value, cl_bytevec_type_name(is_signed, elt_shift),
                             ctx);
    return v;
}

/* Helper: get element from any 1D sequence (list, string, vector) */
static CL_Obj seq_elt(CL_Obj seq, uint32_t index)
{
    if (CL_ANY_STRING_P(seq)) {
        return CL_MAKE_CHAR(cl_string_char_at(seq, index));
    }
    if (CL_BIT_VECTOR_P(seq)) {
        return CL_MAKE_FIXNUM(
            cl_bv_get_bit((CL_BitVector *)CL_OBJ_TO_PTR(seq), index));
    }
    if (CL_BYTE_VECTOR_P(seq)) {
        return CL_MAKE_FIXNUM(
            cl_bytevec_get((CL_ByteVector *)CL_OBJ_TO_PTR(seq), index));
    }
    if (CL_VECTOR_P(seq)) {
        CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(seq);
        return cl_vector_data(v)[index];
    }
    /* List: iterate to index (slow, but correct) */
    {
        uint32_t i;
        CL_Obj cur = seq;
        for (i = 0; i < index && CL_CONS_P(cur); i++)
            cur = cl_cdr(cur);
        return CL_CONS_P(cur) ? cl_car(cur) : CL_NIL;
    }
}

static uint32_t seq_length(CL_Obj seq)
{
    if (CL_ANY_STRING_P(seq)) {
        return cl_string_length(seq);
    }
    if (CL_BIT_VECTOR_P(seq)) {
        return cl_bv_active_length((CL_BitVector *)CL_OBJ_TO_PTR(seq));
    }
    if (CL_BYTE_VECTOR_P(seq)) {
        return cl_bytevec_active_length((CL_ByteVector *)CL_OBJ_TO_PTR(seq));
    }
    if (CL_VECTOR_P(seq)) {
        CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(seq);
        return cl_vector_active_length(v);
    }
    /* List: count */
    {
        uint32_t len = 0;
        CL_Obj cur = seq;
        while (CL_CONS_P(cur)) { len++; cur = cl_cdr(cur); }
        return len;
    }
}

/* Does any character in 1D sequence SEQ exceed the narrow (base-char)
 * range?  Used to widen a character array whose declared element-type is
 * narrow but whose initial contents would otherwise be truncated. */
static int seq_has_wide_char(CL_Obj seq, uint32_t limit)
{
    uint32_t i, len = seq_length(seq);
    if (len > limit) len = limit;
    for (i = 0; i < len; i++) {
        CL_Obj e = seq_elt(seq, i);
        if (CL_CHAR_P(e) && (uint32_t)CL_CHAR_VAL(e) > 255u)
            return 1;
    }
    return 0;
}

/* --- Pre-interned keyword symbols --- */

static CL_Obj KW_INITIAL_ELEMENT = CL_NIL;
static CL_Obj KW_INITIAL_CONTENTS = CL_NIL;
static CL_Obj KW_FILL_POINTER = CL_NIL;
static CL_Obj KW_ADJUSTABLE = CL_NIL;
static CL_Obj KW_ELEMENT_TYPE = CL_NIL;
static CL_Obj KW_DISPLACED_TO = CL_NIL;
static CL_Obj KW_DISPLACED_INDEX_OFFSET = CL_NIL;

/* Helper: recursively flatten nested lists into array elements */
static void flatten_contents(CL_Obj contents, CL_Obj *elts, uint32_t total, uint32_t *j)
{
    while (!CL_NULL_P(contents) && *j < total) {
        CL_Obj elem = cl_car(contents);
        if (CL_CONS_P(elem)) {
            flatten_contents(elem, elts, total, j);
        } else {
            elts[(*j)++] = elem;
        }
        contents = cl_cdr(contents);
    }
}

/* ======================================================= */
/* MAKE-ARRAY                                              */
/* ======================================================= */

static CL_Obj bi_make_array(CL_Obj *args, int n)
{
    CL_Obj dim_arg = args[0];
    CL_Obj initial_element = CL_NIL;
    CL_Obj initial_contents = CL_NIL;
    CL_Obj element_type = CL_NIL;
    CL_Obj displaced_to = CL_NIL;
    int has_initial_element = 0;
    int has_initial_contents = 0;
    int has_displaced_to = 0;
    int has_element_type_spec = 0; /* non-NIL :element-type was given — captured before classify can compact */
    int element_type_bit = 0;
    int element_type_u8 = 0;   /* (unsigned-byte 8) subtype → packed byte vector */
    int element_type_s8 = 0;   /* (signed-byte 8) subtype → packed byte vector */
    int element_type_u16 = 0;  /* (unsigned-byte 16) subtype → packed, 2 bytes/elt */
    int element_type_s16 = 0;  /* (signed-byte 16) subtype → packed, 2 bytes/elt */
    int element_type_char = 0;
    /* Distinguishes the upgraded character width: an explicit CHARACTER /
     * EXTENDED-CHAR element-type yields a wide string (so chars > 255 are
     * not truncated), while BASE-CHAR / STANDARD-CHAR (and the NIL empty
     * case) yield a narrow base-string.  This mirrors MAKE-STRING; keeping
     * them consistent matters because (TYPEP s 'BASE-STRING) is how callers
     * like FSet track whether a string leaf holds only base characters. */
    int element_type_wide_char = 0;
    /* Declared element-type code for a general (non-bit/non-char) vector, so
     * (TYPEP a '(VECTOR FIXNUM)) / ARRAY-ELEMENT-TYPE can distinguish a
     * specialized numeric array from a general (VECTOR T).  Captured before
     * cl_classify_array_elt_type can compact (element_type may then be stale). */
    uint8_t element_type_code = CL_VEC_ELT_T;
    uint32_t fill_ptr = CL_NO_FILL_POINTER;
    uint32_t displaced_offset = 0;
    int fill_pointer_is_t = 0;
    uint8_t flags = 0;
    int i;

    /* Parse keyword args.
     * GC SAFETY: the :element-type branch runs deftype expanders via
     * cl_vm_apply, which can allocate and compact — any CL_Obj local copied
     * out of args[] BEFORE that point (dim_arg, initial_element, ...) would
     * then be a stale offset, and the later CL_GC_PROTECTs would root the
     * stale value.  So the loop only remembers each keyword's args[] INDEX
     * (plus immediate-derived flags); the object values are materialized
     * from the rooted args[] slots after the loop. */
    {
    int ie_idx = -1, ic_idx = -1, dt_idx = -1, et_idx = -1;
    for (i = 1; i + 1 < n; i += 2) {
        if (args[i] == KW_INITIAL_ELEMENT) {
            ie_idx = i + 1;
            has_initial_element = 1;
        } else if (args[i] == KW_INITIAL_CONTENTS) {
            ic_idx = i + 1;
            has_initial_contents = 1;
        } else if (args[i] == KW_FILL_POINTER) {
            /* :fill-pointer T means start at array size (CLHS), integer means that value */
            if (args[i + 1] == SYM_T) {
                fill_pointer_is_t = 1;
                flags |= CL_VEC_FLAG_FILL_POINTER;
            } else if (CL_FIXNUM_P(args[i + 1])) {
                fill_ptr = (uint32_t)CL_FIXNUM_VAL(args[i + 1]);
                flags |= CL_VEC_FLAG_FILL_POINTER;
            }
            /* NIL = no fill pointer (default) */
        } else if (args[i] == KW_ADJUSTABLE) {
            if (!CL_NULL_P(args[i + 1]))
                flags |= CL_VEC_FLAG_ADJUSTABLE;
        } else if (args[i] == KW_ELEMENT_TYPE) {
            et_idx = i + 1;
            element_type = args[i + 1];
            /* Capture nullness BEFORE cl_classify_array_elt_type, which calls
             * cl_vm_apply and can trigger a compacting GC.  After compaction,
             * element_type (a C local) may be a stale arena offset; the boolean
             * is safe to use later without re-reading element_type. */
            has_element_type_spec = !CL_NULL_P(element_type);
            /* (array nil (n)) is a STRING subtype (CLHS 15.1.2.2) — only
             * zero-length is well-defined since no value is storable, but
             * the result must satisfy STRINGP.  Folding it into the
             * character branch produces an empty TYPE_STRING. */
            if (!has_element_type_spec)
                element_type_char = 1;
            else
                /* Classify CHARACTER/BIT/byte element types, expanding
                 * deftypes so aliases like flexi-streams' CHAR* (=> CHARACTER)
                 * build a string, not a (vector t). */
                cl_classify_array_elt_type(element_type, 16, &element_type_char,
                                        &element_type_wide_char, &element_type_bit,
                                        &element_type_u8, &element_type_s8,
                                        &element_type_u16, &element_type_s16);
            /* Re-read: cl_classify_array_elt_type may have called cl_vm_apply
             * (deftype expansion) which can compact, making element_type stale. */
            element_type = args[i + 1];
            /* Record a specialized general (non-bit/non-char/non-byte)
             * element type so the array is distinguishable from a (VECTOR T). */
            if (has_element_type_spec && !element_type_char && !element_type_bit &&
                !element_type_u8 && !element_type_s8 &&
                !element_type_u16 && !element_type_s16)
                element_type_code = cl_classify_vec_elt_code(element_type, 16);
        } else if (args[i] == KW_DISPLACED_TO) {
            dt_idx = i + 1;
            has_displaced_to = !CL_NULL_P(args[i + 1]);
        } else if (args[i] == KW_DISPLACED_INDEX_OFFSET) {
            if (CL_FIXNUM_P(args[i + 1]))
                displaced_offset = (uint32_t)CL_FIXNUM_VAL(args[i + 1]);
        }
    }

    /* Materialize the object values from the rooted args[] slots — any
     * classify above may have compacted, so this is the only safe point to
     * take C-local copies. */
    dim_arg = args[0];
    if (ie_idx >= 0) initial_element  = args[ie_idx];
    if (ic_idx >= 0) initial_contents = args[ic_idx];
    if (dt_idx >= 0) displaced_to     = args[dt_idx];
    if (et_idx >= 0) element_type     = args[et_idx];
    }

    if (has_initial_element && has_initial_contents)
        cl_error(CL_ERR_ARGS, "MAKE-ARRAY: cannot specify both :initial-element and :initial-contents");

    if (has_displaced_to && (has_initial_element || has_initial_contents))
        cl_error(CL_ERR_ARGS, "MAKE-ARRAY: cannot specify :displaced-to with :initial-element or :initial-contents");

    /* A one-element dimension list (n) denotes the same rank-1 array as the
     * integer n.  Normalise it so the 1-D path below handles every option
     * (displacement, fill pointer, element type) uniformly — the list and
     * integer spellings must not diverge.  Without this, :displaced-to was
     * silently dropped for e.g. (make-array '(8) :displaced-to v), yielding a
     * fresh NIL-filled array instead of a displaced view. */
    if (CL_CONS_P(dim_arg) && CL_NULL_P(cl_cdr(dim_arg)) &&
        CL_FIXNUM_P(cl_car(dim_arg)))
        dim_arg = cl_car(dim_arg);

    /* --- 1D case: dim_arg is a fixnum --- */
    if (CL_FIXNUM_P(dim_arg)) {
        uint32_t length = (uint32_t)CL_FIXNUM_VAL(dim_arg);
        CL_Obj result;
        CL_Vector *v;

        /* CLHS: a negative dimension, or one exceeding ARRAY-DIMENSION-LIMIT,
         * must signal an error (a subtype of ERROR, hence catchable by
         * HANDLER-CASE) — not fatally abort the VM.  A bogus length (e.g. a
         * negative HTTP Range size computed by a caller) otherwise casts to a
         * near-2^32 uint32_t and trips cl_alloc's hard header-size guard via
         * cl_storage_error, which escapes ordinary (error () ...) handlers. */
        if (CL_FIXNUM_VAL(dim_arg) < 0)
            cl_error(CL_ERR_TYPE,
                     "MAKE-ARRAY: array dimension must be non-negative (got %d)",
                     (int)CL_FIXNUM_VAL(dim_arg));
        if (length > CL_ARRAY_DIMENSION_LIMIT)
            cl_error(CL_ERR_TYPE,
                     "MAKE-ARRAY: array dimension %u exceeds ARRAY-DIMENSION-LIMIT (%u)",
                     (unsigned)length, (unsigned)CL_ARRAY_DIMENSION_LIMIT);

        /* :fill-pointer T means fill-pointer = array size (CLHS) */
        if (fill_pointer_is_t)
            fill_ptr = length;

        /* --- :displaced-to handling --- */
        if (has_displaced_to) {
            /* Displaced to a string: copy substring (TYPE_STRING uses packed
               bytes, incompatible with CL_Obj element storage) */
            if (CL_ANY_STRING_P(displaced_to)) {
                uint32_t slen = cl_string_length(displaced_to);
                if (displaced_offset + length > slen)
                    cl_error(CL_ERR_ARGS, "MAKE-ARRAY: displaced bounds exceed target string length");
                /* amilisp can't install a true displaced view onto a packed
                 * base/wide string (its bytes are not CL_Obj element storage),
                 * so it copies the requested window.  A :fill-pointer or
                 * :adjustable request still has to be honoured, though, which a
                 * simple string can't carry — build a character vector
                 * (CL_VEC_FLAG_STRING) in that case so the fill pointer and
                 * adjustability survive (matches the non-displaced path). */
                if (flags & (CL_VEC_FLAG_FILL_POINTER | CL_VEC_FLAG_ADJUSTABLE)) {
                    CL_Obj sv;
                    uint32_t j;
                    CL_GC_PROTECT(displaced_to);
                    sv = cl_make_array(length, 0, NULL,
                                       flags | CL_VEC_FLAG_STRING, fill_ptr);
                    CL_GC_UNPROTECT(1);
                    for (j = 0; j < length; j++)
                        cl_vector_data((CL_Vector *)CL_OBJ_TO_PTR(sv))[j] =
                            CL_MAKE_CHAR(cl_string_char_at(displaced_to,
                                                           displaced_offset + j));
                    return sv;
                }
                /* Create new string with copied characters.  cl_make_string
                 * may GC-compact, so protect displaced_to until we've copied
                 * each character. */
                {
                    CL_Obj str;
                    uint32_t j;
                    CL_GC_PROTECT(displaced_to);
                    str = make_string_like(displaced_to, length);
                    CL_GC_UNPROTECT(1);
                    for (j = 0; j < length; j++)
                        cl_string_set_char_at(str, j,
                            cl_string_char_at(displaced_to, displaced_offset + j));
                    return str;
                }
            }
            /* Displaced to a character vector (CL_VEC_FLAG_STRING) */
            if (CL_VECTOR_P(displaced_to)) {
                CL_Vector *target = (CL_Vector *)CL_OBJ_TO_PTR(displaced_to);
                uint32_t target_total = target->length;
                uint32_t n_data;

                if (displaced_offset + length > target_total)
                    cl_error(CL_ERR_ARGS, "MAKE-ARRAY: displaced bounds exceed target array");

                /* Allocate vector with 2 data slots: backing ref + offset */
                n_data = 2;
                {
                    uint32_t alloc_size = sizeof(CL_Vector) + n_data * sizeof(CL_Obj);
                    CL_Vector *dv;
                    uint8_t dflags = flags | CL_VEC_FLAG_DISPLACED;

                    /* Preserve string flag from target */
                    if (target->flags & CL_VEC_FLAG_STRING)
                        dflags |= CL_VEC_FLAG_STRING;

                    CL_GC_PROTECT(displaced_to);
                    result = CL_PTR_TO_OBJ((CL_Vector *)cl_alloc(TYPE_VECTOR, alloc_size));
                    CL_GC_UNPROTECT(1);

                    dv = (CL_Vector *)CL_OBJ_TO_PTR(result);
                    dv->length = length;
                    dv->fill_pointer = fill_ptr;
                    dv->flags = dflags;
                    dv->rank = 0;
                    dv->elt_type = CL_VEC_ELT_T;
                    dv->_reserved = 0;
                    dv->data[0] = displaced_to;
                    dv->data[1] = CL_MAKE_FIXNUM((int32_t)displaced_offset);
                    return result;
                }
            }
            /* Displaced to a bit vector.  Like the string path above, this
             * does NOT install a true displaced view (the bit-vector heap
             * type has no displacement field) — it copies the requested
             * window into a fresh bit-vector.  Mutations of the target
             * therefore won't propagate; full displacement semantics are a
             * TODO for the array-chapter conformance work. */
            if (CL_BIT_VECTOR_P(displaced_to)) {
                CL_BitVector *src = (CL_BitVector *)CL_OBJ_TO_PTR(displaced_to);
                uint32_t src_total = src->length;
                CL_Obj bv_obj;
                CL_BitVector *dst;
                uint32_t j;

                if (displaced_offset + length > src_total)
                    cl_error(CL_ERR_ARGS, "MAKE-ARRAY: displaced bounds exceed target bit-vector");

                CL_GC_PROTECT(displaced_to);
                bv_obj = cl_make_bit_vector(length);
                CL_GC_UNPROTECT(1);
                /* Re-fetch src after potential GC. */
                src = (CL_BitVector *)CL_OBJ_TO_PTR(displaced_to);
                dst = (CL_BitVector *)CL_OBJ_TO_PTR(bv_obj);
                dst->flags = flags;
                dst->fill_pointer = fill_ptr;
                for (j = 0; j < length; j++) {
                    if (cl_bv_get_bit(src, displaced_offset + j))
                        dst->data[j / 32] |= (1u << (j % 32));
                }
                return bv_obj;
            }
            /* Displaced to a byte vector.  Like the bit-vector path above,
             * this does NOT install a true displaced view (the byte-vector
             * heap type has no displacement field) — it copies the requested
             * window into a fresh byte vector.  Mutations of the target
             * therefore won't propagate. */
            if (CL_BYTE_VECTOR_P(displaced_to)) {
                CL_ByteVector *src = (CL_ByteVector *)CL_OBJ_TO_PTR(displaced_to);
                uint32_t src_total = src->length;
                int src_signed = src->is_signed;
                int src_shift = src->elt_shift;
                CL_Obj nbv;
                CL_ByteVector *dst;

                if (displaced_offset + length > src_total)
                    cl_error(CL_ERR_ARGS, "MAKE-ARRAY: displaced bounds exceed target byte-vector");

                CL_GC_PROTECT(displaced_to);
                nbv = cl_make_byte_vector(length, src_signed, src_shift);
                CL_GC_UNPROTECT(1);
                /* Re-fetch src after potential GC. */
                src = (CL_ByteVector *)CL_OBJ_TO_PTR(displaced_to);
                dst = (CL_ByteVector *)CL_OBJ_TO_PTR(nbv);
                dst->flags = flags;
                dst->fill_pointer = fill_ptr;
                memcpy(dst->data,
                       src->data + ((size_t)displaced_offset << src_shift),
                       (size_t)length << src_shift);
                return nbv;
            }
            cl_error(CL_ERR_TYPE, "MAKE-ARRAY: :displaced-to target must be an array");
        }

        /* Bit vector path */
        if (element_type_bit) {
            CL_BitVector *bv;
            /* Protect initial_contents across the (compacting) allocation so the
             * seq_elt reads below stay valid — otherwise a stale offset crashes
             * or silently copies garbage bits under GC stress. */
            CL_GC_PROTECT(initial_contents);
            result = cl_make_bit_vector(length);
            CL_GC_UNPROTECT(1);
            bv = (CL_BitVector *)CL_OBJ_TO_PTR(result);
            bv->flags = flags;
            bv->fill_pointer = fill_ptr;
            if (has_initial_element) {
                if (!CL_FIXNUM_P(initial_element) ||
                    (CL_FIXNUM_VAL(initial_element) != 0 && CL_FIXNUM_VAL(initial_element) != 1))
                    cl_error(CL_ERR_TYPE, "MAKE-ARRAY: :initial-element for bit array must be 0 or 1");
                if (CL_FIXNUM_VAL(initial_element) == 1) {
                    uint32_t nw = CL_BV_WORDS(length), wi;
                    for (wi = 0; wi < nw; wi++)
                        bv->data[wi] = 0xFFFFFFFFu;
                    /* Mask trailing */
                    if (length % 32 != 0)
                        bv->data[nw - 1] &= (1u << (length % 32)) - 1;
                }
            } else if (has_initial_contents) {
                /* Any sequence (list, vector, string or bit-vector) works as
                 * initial-contents — read it element-by-element via seq_elt. */
                uint32_t j, ilen = seq_length(initial_contents);
                if (ilen > length) ilen = length;
                for (j = 0; j < ilen; j++) {
                    CL_Obj elem = seq_elt(initial_contents, j);
                    if (CL_FIXNUM_P(elem) && CL_FIXNUM_VAL(elem) == 1)
                        bv->data[j / 32] |= (1u << (j % 32));
                }
            }
            return result;
        }

        /* Adjustable byte arrays cannot use packed storage: ADJUST-ARRAY and
         * VECTOR-PUSH-EXTEND must grow them in place (preserving identity),
         * which only the general CL_Vector displacement machinery supports.
         * Build a general vector annotated CL_VEC_ELT_U8/S8/U16/S16 instead
         * so ARRAY-ELEMENT-TYPE / TYPEP still report the byte element type —
         * this keeps the drakma/chunga adjustable fill-pointer HTTP-buffer
         * idiom working exactly as before packing existed. */
        if ((element_type_u8 || element_type_s8 ||
             element_type_u16 || element_type_s16) &&
            (flags & CL_VEC_FLAG_ADJUSTABLE)) {
            element_type_code = element_type_u8  ? CL_VEC_ELT_U8
                              : element_type_s8  ? CL_VEC_ELT_S8
                              : element_type_u16 ? CL_VEC_ELT_U16
                                                 : CL_VEC_ELT_S16;
            element_type_u8 = element_type_s8 = 0;
            element_type_u16 = element_type_s16 = 0;
        }

        /* Packed byte-vector path — (unsigned-byte 8/16)/(signed-byte 8/16).
         * 1 or 2 bytes per element instead of a 4-byte tagged CL_Obj, and a
         * GC leaf object (contents never scanned). */
        if (element_type_u8 || element_type_s8 ||
            element_type_u16 || element_type_s16) {
            CL_ByteVector *bv;
            int is_signed = element_type_s8 || element_type_s16;
            int elt_shift = (element_type_u16 || element_type_s16) ? 1 : 0;
            /* Protect initial_contents across the (compacting) allocation so
             * the seq_elt reads below stay valid (mirrors the bit path). */
            CL_GC_PROTECT(initial_contents);
            result = cl_make_byte_vector(length, is_signed, elt_shift);
            CL_GC_UNPROTECT(1);
            bv = (CL_ByteVector *)CL_OBJ_TO_PTR(result);
            bv->flags = flags;
            bv->fill_pointer = fill_ptr;
            if (has_initial_element) {
                int32_t v = cl_bytevec_check_value(initial_element, is_signed,
                                                   elt_shift,
                                                   "MAKE-ARRAY :initial-element");
                if (v != 0) {
                    if (elt_shift == 0) {
                        memset(bv->data, (uint8_t)v, length);
                    } else {
                        uint32_t j;
                        for (j = 0; j < length; j++)
                            cl_bytevec_set(bv, j, v);
                    }
                }
            } else if (has_initial_contents) {
                /* Any sequence works as initial-contents — read it
                 * element-by-element via seq_elt, range-checking each. */
                uint32_t j, ilen = seq_length(initial_contents);
                if (ilen > length) ilen = length;
                for (j = 0; j < ilen; j++) {
                    CL_Obj elem = seq_elt(initial_contents, j);
                    cl_bytevec_set(bv, j, cl_bytevec_check_value(
                        elem, is_signed, elt_shift,
                        "MAKE-ARRAY :initial-contents"));
                }
            }
            return result;
        }

        /* Character element-type */
        if (element_type_char) {
            /* If fill-pointer or adjustable, use CL_Vector with STRING flag */
            if (flags & (CL_VEC_FLAG_FILL_POINTER | CL_VEC_FLAG_ADJUSTABLE)) {
                CL_Obj init_char = CL_MAKE_CHAR(' ');
                uint32_t j;
                if (has_initial_element) {
                    if (!CL_CHAR_P(initial_element))
                        cl_error(CL_ERR_TYPE, "MAKE-ARRAY: :initial-element for character array must be a character");
                    init_char = initial_element;
                }
                /* cl_make_array compacts (esp. under GC stress); protect
                 * initial_contents so the seq_elt reads below don't dereference
                 * a stale arena offset (which crashes inside MAKE-ARRAY). */
                CL_GC_PROTECT(initial_contents);
                result = cl_make_array(length, 0, NULL, flags | CL_VEC_FLAG_STRING, fill_ptr);
                CL_GC_UNPROTECT(1);
                v = (CL_Vector *)CL_OBJ_TO_PTR(result);
                for (j = 0; j < length; j++)
                    cl_vector_data(v)[j] = init_char;
                if (has_initial_contents) {
                    uint32_t ic_len = seq_length(initial_contents);
                    if (ic_len > length) ic_len = length;
                    for (j = 0; j < ic_len; j++)
                        cl_vector_data(v)[j] = seq_elt(initial_contents, j);
                }
                return result;
            }
            /* Simple string: narrow base-string unless the element-type is
             * CHARACTER/EXTENDED-CHAR or the contents need wide storage. */
            {
                int init_char = ' ';
                int want_wide = element_type_wide_char;
                CL_Obj str;
                if (has_initial_element) {
                    if (!CL_CHAR_P(initial_element))
                        cl_error(CL_ERR_TYPE, "MAKE-ARRAY: :initial-element for character array must be a character");
                    init_char = (int)CL_CHAR_VAL(initial_element);
                }
                if (init_char > 255) want_wide = 1;
                if (!want_wide && has_initial_contents &&
                    seq_has_wide_char(initial_contents, length))
                    want_wide = 1;
                CL_GC_PROTECT(initial_contents);
                str = make_char_array_string(length, want_wide);
                {
                    uint32_t j;
                    for (j = 0; j < length; j++)
                        cl_string_set_char_at(str, j, init_char);
                }
                if (has_initial_contents) {
                    uint32_t j;
                    uint32_t ic_len = seq_length(initial_contents);
                    if (ic_len > length) ic_len = length;
                    for (j = 0; j < ic_len; j++) {
                        CL_Obj elem = seq_elt(initial_contents, j);
                        if (CL_CHAR_P(elem))
                            cl_string_set_char_at(str, j, CL_CHAR_VAL(elem));
                    }
                }
                CL_GC_UNPROTECT(1);
                return str;
            }
        }

        /* For numeric element types, default initial-element to 0 */
        if (!has_initial_element && !has_initial_contents && has_element_type_spec) {
            if (!element_type_bit && !element_type_char) {
                initial_element = CL_MAKE_FIXNUM(0);
                has_initial_element = 1;
            }
        }

        if (flags == 0 && fill_ptr == CL_NO_FILL_POINTER && !has_initial_element && !has_initial_contents) {
            /* Fast path: simple vector */
            return cl_make_vector(length);
        }

        /* GC SAFETY: cl_make_array can compact — initial_element and
         * initial_contents are read AFTER it, so they must be forwarded. */
        CL_GC_PROTECT(initial_element);
        CL_GC_PROTECT(initial_contents);
        result = cl_make_array(length, 0, NULL, flags, fill_ptr);
        v = (CL_Vector *)CL_OBJ_TO_PTR(result);
        v->elt_type = element_type_code;

        if (has_initial_element) {
            uint32_t j;
            CL_Obj *elts = cl_vector_data(v);
            for (j = 0; j < length; j++)
                elts[j] = initial_element;
        } else if (has_initial_contents) {
            /* :initial-contents can be any sequence for 1D */
            uint32_t j;
            uint32_t ic_len = seq_length(initial_contents);
            CL_Obj *elts = cl_vector_data(v);
            if (ic_len > length) ic_len = length;
            for (j = 0; j < ic_len; j++)
                elts[j] = seq_elt(initial_contents, j);
        }
        CL_GC_UNPROTECT(2);
        return result;
    }

    /* --- Multi-dim case: dim_arg is a list of fixnums --- */
    if (CL_CONS_P(dim_arg) || CL_NULL_P(dim_arg)) {
        uint32_t dims[CL_ARRAY_MAX_RANK];
        uint8_t rank = 0;
        uint32_t total = 1;
        CL_Obj cur = dim_arg;
        CL_Obj result;
        CL_Vector *v;

        /* Count rank and compute total size.  Mirror the 1-D path's
         * dimension checks (negative / over ARRAY-DIMENSION-LIMIT must be
         * catchable type-errors), and check the running product for
         * uint32 overflow BEFORE multiplying: e.g. '(65536 65537) wraps
         * total to 65536, allocating a small vector whose stored dims
         * claim the full extent — later per-dimension-checked AREF
         * writes then land far outside the allocation. */
        while (!CL_NULL_P(cur) && rank < CL_ARRAY_MAX_RANK) {
            if (!CL_FIXNUM_P(cl_car(cur)))
                cl_error(CL_ERR_TYPE, "MAKE-ARRAY: dimension must be a fixnum");
            if (CL_FIXNUM_VAL(cl_car(cur)) < 0)
                cl_error(CL_ERR_TYPE,
                         "MAKE-ARRAY: array dimension must be non-negative (got %d)",
                         (int)CL_FIXNUM_VAL(cl_car(cur)));
            dims[rank] = (uint32_t)CL_FIXNUM_VAL(cl_car(cur));
            if (dims[rank] > CL_ARRAY_DIMENSION_LIMIT)
                cl_error(CL_ERR_TYPE,
                         "MAKE-ARRAY: array dimension %u exceeds ARRAY-DIMENSION-LIMIT (%u)",
                         (unsigned)dims[rank], (unsigned)CL_ARRAY_DIMENSION_LIMIT);
            if (dims[rank] != 0 && total > CL_ARRAY_TOTAL_SIZE_LIMIT / dims[rank])
                cl_error(CL_ERR_TYPE,
                         "MAKE-ARRAY: total array size exceeds ARRAY-TOTAL-SIZE-LIMIT (%u)",
                         (unsigned)CL_ARRAY_TOTAL_SIZE_LIMIT);
            total *= dims[rank];
            rank++;
            cur = cl_cdr(cur);
        }
        if (!CL_NULL_P(cur))
            cl_error(CL_ERR_TYPE,
                     "MAKE-ARRAY: array rank exceeds implementation limit (%u)",
                     (unsigned)CL_ARRAY_MAX_RANK);

        /* :fill-pointer T means fill-pointer = array size (CLHS) */
        if (fill_pointer_is_t && rank == 1)
            fill_ptr = dims[0];
        else if (fill_pointer_is_t && rank == 0)
            fill_ptr = 1;

        if (rank == 0) {
            /* 0-dimensional array: single element */
            return cl_make_array(1, 0, NULL, flags, fill_ptr);
        }
        if (rank == 1 && element_type_char) {
            /* 1D character array from list dimension: create string */
            int init_char = ' ';
            CL_Obj str;
            uint32_t j;
            /* :fill-pointer requires a CL_Vector backing (TYPE_STRING has
             * no fill-pointer field).  Build a string-vector instead so
             * (length name) and friends honor the active length. */
            if (fill_ptr != CL_NO_FILL_POINTER) {
                CL_Obj vec;
                CL_Vector *vv;
                CL_Obj *elts;
                CL_Obj fill;
                /* GC SAFETY: read fill/initial_contents only after the
                 * allocation, through protected locals. */
                CL_GC_PROTECT(initial_element);
                CL_GC_PROTECT(initial_contents);
                vec = cl_make_array(dims[0], 0, NULL,
                                    flags | CL_VEC_FLAG_STRING, fill_ptr);
                fill = has_initial_element ? initial_element
                                           : CL_MAKE_CHAR(' ');
                vv = (CL_Vector *)CL_OBJ_TO_PTR(vec);
                elts = cl_vector_data(vv);
                for (j = 0; j < dims[0]; j++) elts[j] = fill;
                if (has_initial_contents) {
                    uint32_t ic_len = seq_length(initial_contents);
                    if (ic_len > dims[0]) ic_len = dims[0];
                    for (j = 0; j < ic_len; j++) {
                        CL_Obj elem = seq_elt(initial_contents, j);
                        if (CL_CHAR_P(elem)) elts[j] = elem;
                    }
                }
                CL_GC_UNPROTECT(2);
                return vec;
            }
            /* :displaced-to to a string: copy active substring (mirrors the
             * fixnum-dim path above; without this, dims-as-list callers
             * silently lose the displacement). */
            if (has_displaced_to && CL_ANY_STRING_P(displaced_to)) {
                uint32_t slen = cl_string_length(displaced_to);
                if (displaced_offset + dims[0] > slen)
                    cl_error(CL_ERR_ARGS,
                             "MAKE-ARRAY: displaced bounds exceed target string length");
                CL_GC_PROTECT(displaced_to);
                str = make_string_like(displaced_to, dims[0]);
                CL_GC_UNPROTECT(1);
                for (j = 0; j < dims[0]; j++)
                    cl_string_set_char_at(str, j,
                        cl_string_char_at(displaced_to, displaced_offset + j));
                return str;
            }
            {
            int want_wide = element_type_wide_char;
            if (has_initial_element) {
                if (!CL_CHAR_P(initial_element))
                    cl_error(CL_ERR_TYPE, "MAKE-ARRAY: :initial-element for character array must be a character");
                init_char = (int)CL_CHAR_VAL(initial_element);
            }
            if (init_char > 255) want_wide = 1;
            if (!want_wide && has_initial_contents &&
                seq_has_wide_char(initial_contents, dims[0]))
                want_wide = 1;
            CL_GC_PROTECT(initial_contents);
            str = make_char_array_string(dims[0], want_wide);
            }
            for (j = 0; j < dims[0]; j++)
                cl_string_set_char_at(str, j, init_char);
            if (has_initial_contents) {
                uint32_t ic_len = seq_length(initial_contents);
                if (ic_len > dims[0]) ic_len = dims[0];
                for (j = 0; j < ic_len; j++) {
                    CL_Obj elem = seq_elt(initial_contents, j);
                    if (CL_CHAR_P(elem))
                        cl_string_set_char_at(str, j, CL_CHAR_VAL(elem));
                }
            }
            CL_GC_UNPROTECT(1);
            return str;
        }
        /* GC SAFETY: the allocations below can compact — initial_element /
         * initial_contents are read after them in the fill loops. */
        CL_GC_PROTECT(initial_element);
        CL_GC_PROTECT(initial_contents);
        if (rank == 1) {
            /* List of one element = 1D array */
            result = cl_make_array(dims[0], 0, NULL, flags, fill_ptr);
        } else {
            /* True multi-dim */
            if (fill_ptr != CL_NO_FILL_POINTER)
                cl_error(CL_ERR_ARGS, "MAKE-ARRAY: fill pointer only valid for vectors (1D)");
            flags |= CL_VEC_FLAG_MULTIDIM;
            if (has_displaced_to) {
                /* Displaced multi-dim array.  Layout: dims in data[0..rank-1],
                 * backing ref at data[rank], offset at data[rank+1] (see
                 * CL_DISP_BASE_IDX).  The backing must be a real array whose
                 * elements are CL_Objs — amilisp's packed strings (and copying
                 * bit-vectors) can't carry a live multi-dim view. */
                CL_Vector *dv;
                uint32_t n_data, alloc_size, di;
                uint8_t dflags;
                /* Displaced to a packed byte vector: like the rank-1
                 * byte-vector path, copy the window instead of installing a
                 * live view (the packed heap type has no CL_Obj element
                 * storage to point into).  The copy is a general multi-dim
                 * array annotated with the byte element type, matching what
                 * MAKE-ARRAY builds for multi-dim (unsigned-byte 8) arrays.
                 * Mutations of the target won't propagate — same documented
                 * limitation as the other copying displaced paths.
                 * (serapeum's RESHAPE displaces multi-dim onto RANGE vectors,
                 * which pack to byte vectors since slice 1.) */
                if (CL_BYTE_VECTOR_P(displaced_to)) {
                    CL_ByteVector *src =
                        (CL_ByteVector *)CL_OBJ_TO_PTR(displaced_to);
                    uint8_t src_code = src->elt_shift
                        ? (src->is_signed ? CL_VEC_ELT_S16 : CL_VEC_ELT_U16)
                        : (src->is_signed ? CL_VEC_ELT_S8  : CL_VEC_ELT_U8);
                    uint32_t j;
                    if (displaced_offset + total > src->length)
                        cl_error(CL_ERR_ARGS,
                                 "MAKE-ARRAY: displaced bounds exceed target "
                                 "byte-vector");
                    CL_GC_PROTECT(displaced_to);
                    result = cl_make_array(total, rank, dims, flags,
                                           CL_NO_FILL_POINTER);
                    CL_GC_UNPROTECT(1);
                    /* Re-fetch src after the (potentially compacting) alloc. */
                    src = (CL_ByteVector *)CL_OBJ_TO_PTR(displaced_to);
                    v = (CL_Vector *)CL_OBJ_TO_PTR(result);
                    v->elt_type = src_code;
                    for (j = 0; j < total; j++)
                        cl_vector_data(v)[j] = CL_MAKE_FIXNUM(
                            cl_bytevec_get(src, displaced_offset + j));
                    CL_GC_UNPROTECT(2);  /* initial_element, initial_contents */
                    return result;
                }
                if (!CL_VECTOR_P(displaced_to))
                    cl_error(CL_ERR_TYPE,
                             "MAKE-ARRAY: :displaced-to target for a multi-dimensional "
                             "array must be a (non-string, non-bit) array");
                {
                    CL_Vector *target = (CL_Vector *)CL_OBJ_TO_PTR(displaced_to);
                    if (displaced_offset + total > target->length)
                        cl_error(CL_ERR_ARGS,
                                 "MAKE-ARRAY: displaced bounds exceed target array");
                }
                n_data = (uint32_t)rank + 2;
                alloc_size = sizeof(CL_Vector) + n_data * sizeof(CL_Obj);
                dflags = flags | CL_VEC_FLAG_DISPLACED;
                CL_GC_PROTECT(displaced_to);
                result = CL_PTR_TO_OBJ((CL_Vector *)cl_alloc(TYPE_VECTOR, alloc_size));
                CL_GC_UNPROTECT(1);
                dv = (CL_Vector *)CL_OBJ_TO_PTR(result);
                dv->length = total;
                dv->fill_pointer = CL_NO_FILL_POINTER;
                dv->flags = dflags;
                dv->rank = rank;
                dv->elt_type = element_type_code;
                dv->_reserved = 0;
                for (di = 0; di < (uint32_t)rank; di++)
                    dv->data[di] = CL_MAKE_FIXNUM((int32_t)dims[di]);
                dv->data[rank] = displaced_to;
                dv->data[rank + 1] = CL_MAKE_FIXNUM((int32_t)displaced_offset);
                CL_GC_UNPROTECT(2);  /* initial_element, initial_contents */
                return result;
            }
            result = cl_make_array(total, rank, dims, flags, CL_NO_FILL_POINTER);
        }

        v = (CL_Vector *)CL_OBJ_TO_PTR(result);
        v->elt_type = element_type_code;

        if (has_initial_element) {
            uint32_t j;
            CL_Obj *elts = cl_vector_data(v);
            for (j = 0; j < total; j++)
                elts[j] = initial_element;
        } else if (has_initial_contents) {
            /* For multi-dim: flatten nested lists into row-major order */
            CL_Obj *elts = cl_vector_data(v);
            if (rank <= 1) {
                uint32_t j = 0;
                cur = initial_contents;
                while (!CL_NULL_P(cur) && j < total) {
                    elts[j++] = cl_car(cur);
                    cur = cl_cdr(cur);
                }
            } else {
                /* Recursive flatten: walk nested structure to arbitrary depth */
                uint32_t j = 0;
                flatten_contents(initial_contents, elts, total, &j);
            }
        }
        CL_GC_UNPROTECT(2);  /* initial_element, initial_contents */
        return result;
    }

    cl_error(CL_ERR_TYPE, "MAKE-ARRAY: dimensions must be a fixnum or list of fixnums");
    return CL_NIL;
}

/* ======================================================= */
/* AREF / SVREF                                            */
/* ======================================================= */

static CL_Obj bi_aref(CL_Obj *args, int n)
{
    CL_Vector *vec;
    if (CL_BIT_VECTOR_P(args[0])) {
        CL_BitVector *bv = (CL_BitVector *)CL_OBJ_TO_PTR(args[0]);
        int32_t idx;
        if (n < 2)
            cl_error(CL_ERR_ARGS, "AREF: too few arguments");
        if (!CL_FIXNUM_P(args[1]))
            cl_error(CL_ERR_TYPE, "AREF: index must be a fixnum");
        idx = CL_FIXNUM_VAL(args[1]);
        /* AREF accesses the underlying array, ignoring any fill pointer
         * (CLHS 'aref'): bound on the total length, not the active length. */
        if (idx < 0 || (uint32_t)idx >= bv->length)
            cl_error(CL_ERR_ARGS, "AREF: index %d out of range", (int)idx);
        return CL_MAKE_FIXNUM(cl_bv_get_bit(bv, (uint32_t)idx));
    }
    if (CL_ANY_STRING_P(args[0])) {
        int32_t idx;
        if (n < 2)
            cl_error(CL_ERR_ARGS, "AREF: too few arguments");
        if (!CL_FIXNUM_P(args[1]))
            cl_error(CL_ERR_TYPE, "AREF: index must be a fixnum");
        idx = CL_FIXNUM_VAL(args[1]);
        if (idx < 0 || (uint32_t)idx >= cl_string_length(args[0]))
            cl_error(CL_ERR_ARGS, "AREF: index %d out of range", (int)idx);
        return CL_MAKE_CHAR(cl_string_char_at(args[0], (uint32_t)idx));
    }
    if (CL_BYTE_VECTOR_P(args[0])) {
        CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(args[0]);
        int32_t idx;
        if (n < 2)
            cl_error(CL_ERR_ARGS, "AREF: too few arguments");
        if (!CL_FIXNUM_P(args[1]))
            cl_error(CL_ERR_TYPE, "AREF: index must be a fixnum");
        idx = CL_FIXNUM_VAL(args[1]);
        /* AREF ignores any fill pointer: bound on the total length. */
        if (idx < 0 || (uint32_t)idx >= bv->length)
            cl_error(CL_ERR_ARGS, "AREF: index %d out of range", (int)idx);
        return CL_MAKE_FIXNUM(cl_bytevec_get(bv, (uint32_t)idx));
    }
    if (!CL_VECTOR_P(args[0]))
        cl_error(CL_ERR_TYPE, "AREF: not an array");
    vec = (CL_Vector *)CL_OBJ_TO_PTR(args[0]);

    if (vec->rank > 1) {
        /* Multi-dimensional: compute row-major index */
        uint8_t rank = vec->rank;
        uint32_t row_major = 0;
        int d;
        if (n - 1 != (int)rank)
            cl_error(CL_ERR_ARGS, "AREF: expected %d indices, got %d",
                     (int)rank, n - 1);
        for (d = 0; d < (int)rank; d++) {
            uint32_t dim_size;
            int32_t idx;
            if (!CL_FIXNUM_P(args[d + 1]))
                cl_error(CL_ERR_TYPE, "AREF: index must be a fixnum");
            idx = CL_FIXNUM_VAL(args[d + 1]);
            dim_size = (uint32_t)CL_FIXNUM_VAL(vec->data[d]);
            if (idx < 0 || (uint32_t)idx >= dim_size)
                cl_error(CL_ERR_ARGS, "AREF: index %d out of range for dimension %d (size %d)",
                         (int)idx, d, (int)dim_size);
            row_major = row_major * dim_size + (uint32_t)idx;
        }
        return cl_vector_data(vec)[row_major];
    }

    /* 1D case */
    {
        int32_t idx;
        if (n < 2)
            cl_error(CL_ERR_ARGS, "AREF: too few arguments");
        if (!CL_FIXNUM_P(args[1]))
            cl_error(CL_ERR_TYPE, "AREF: index must be a fixnum");
        idx = CL_FIXNUM_VAL(args[1]);
        if (idx < 0 || (uint32_t)idx >= vec->length)
            cl_error(CL_ERR_ARGS, "AREF: index %d out of range", (int)idx);
        return cl_vector_data(vec)[idx];
    }
}

static CL_Obj bi_svref(CL_Obj *args, int n)
{
    CL_Vector *vec;
    int32_t idx;
    CL_UNUSED(n);
    if (!CL_VECTOR_P(args[0]))
        cl_error(CL_ERR_TYPE, "SVREF: not a simple vector");
    vec = (CL_Vector *)CL_OBJ_TO_PTR(args[0]);
    if (vec->rank > 1 || vec->flags != 0)
        cl_error(CL_ERR_TYPE, "SVREF: not a simple vector");
    if (!CL_FIXNUM_P(args[1]))
        cl_error(CL_ERR_TYPE, "SVREF: index must be a fixnum");
    idx = CL_FIXNUM_VAL(args[1]);
    if (idx < 0 || (uint32_t)idx >= vec->length)
        cl_error(CL_ERR_ARGS, "SVREF: index %d out of range", (int)idx);
    return cl_vector_data(vec)[idx];
}

/* --- OP_AREF's slow path (builtins.h) --- */

CL_Obj cl_vector_ref1(CL_Obj vec, CL_Obj idx, int kind)
{
    static const char *const who[4] = { "AREF", "SVREF", "CHAR", "SCHAR" };
    const char *w = who[kind & 3];
    int32_t i;
    CL_Vector *v;

    switch (kind) {
    case CL_AREF_KIND_CHAR:
        /* CHAR accepts every string, including a fill-pointer / adjustable
         * character vector (bi_char_accessor). */
        if (!CL_ANY_STRING_P(vec) && !CL_STRING_VECTOR_P(vec))
            cl_error(CL_ERR_TYPE, "CHAR: not a string");
        break;
    case CL_AREF_KIND_SCHAR:
        if (!CL_ANY_STRING_P(vec))
            cl_error(CL_ERR_TYPE, "SCHAR: not a simple-string");
        break;
    case CL_AREF_KIND_SVREF:
        if (!CL_VECTOR_P(vec))
            cl_error(CL_ERR_TYPE, "SVREF: not a simple vector");
        v = (CL_Vector *)CL_OBJ_TO_PTR(vec);
        if (v->rank > 1 || v->flags != 0)
            cl_error(CL_ERR_TYPE, "SVREF: not a simple vector");
        break;
    default:
        break;
    }
    if (!CL_FIXNUM_P(idx)) {
        if (kind == CL_AREF_KIND_CHAR || kind == CL_AREF_KIND_SCHAR)
            cl_error(CL_ERR_TYPE, "%s: index must be an integer", w);
        cl_error(CL_ERR_TYPE, "%s: index must be a fixnum", w);
    }
    i = CL_FIXNUM_VAL(idx);

    if (kind == CL_AREF_KIND_CHAR || kind == CL_AREF_KIND_SCHAR) {
        if (i < 0 || (uint32_t)i >= cl_string_length(vec))
            cl_error(CL_ERR_ARGS, "%s: index %d out of range", w, (int)i);
        return CL_MAKE_CHAR(cl_string_char_at(vec, (uint32_t)i));
    }
    if (kind == CL_AREF_KIND_AREF) {
        if (CL_BIT_VECTOR_P(vec)) {
            CL_BitVector *bv = (CL_BitVector *)CL_OBJ_TO_PTR(vec);
            if (i < 0 || (uint32_t)i >= bv->length)
                cl_error(CL_ERR_ARGS, "AREF: index %d out of range", (int)i);
            return CL_MAKE_FIXNUM(cl_bv_get_bit(bv, (uint32_t)i));
        }
        if (CL_ANY_STRING_P(vec)) {
            if (i < 0 || (uint32_t)i >= cl_string_length(vec))
                cl_error(CL_ERR_ARGS, "AREF: index %d out of range", (int)i);
            return CL_MAKE_CHAR(cl_string_char_at(vec, (uint32_t)i));
        }
        if (CL_BYTE_VECTOR_P(vec)) {
            CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(vec);
            if (i < 0 || (uint32_t)i >= bv->length)
                cl_error(CL_ERR_ARGS, "AREF: index %d out of range", (int)i);
            return CL_MAKE_FIXNUM(cl_bytevec_get(bv, (uint32_t)i));
        }
        if (!CL_VECTOR_P(vec))
            cl_error(CL_ERR_TYPE, "AREF: not an array");
        v = (CL_Vector *)CL_OBJ_TO_PTR(vec);
        if (v->rank > 1)
            cl_error(CL_ERR_ARGS, "AREF: expected %d indices, got 1", (int)v->rank);
    } else {
        v = (CL_Vector *)CL_OBJ_TO_PTR(vec);   /* SVREF: gated above */
    }
    /* AREF accesses the underlying array, ignoring any fill pointer
     * (CLHS 'aref'): bound on the total length, not the active length. */
    if (i < 0 || (uint32_t)i >= v->length)
        cl_error(CL_ERR_ARGS, "%s: index %d out of range", w, (int)i);
    return cl_vector_data(v)[i];
}

/* ======================================================= */
/* %SETF-AREF (multi-dimensional setter)                   */
/* ======================================================= */

/* (%SETF-AREF array value idx1 idx2 ...) → value */
static CL_Obj bi_setf_aref(CL_Obj *args, int n)
{
    CL_Vector *vec;
    CL_Obj array = args[0];
    CL_Obj value = args[1];
    int nindices = n - 2;
    uint32_t row_major = 0;
    int d;

    if (CL_BIT_VECTOR_P(array)) {
        CL_BitVector *bv = (CL_BitVector *)CL_OBJ_TO_PTR(array);
        int32_t idx, val;
        if (nindices != 1)
            cl_error(CL_ERR_ARGS, "%SETF-AREF: expected 1 index for bit vector");
        if (!CL_FIXNUM_P(args[2]))
            cl_error(CL_ERR_TYPE, "%SETF-AREF: index must be a fixnum");
        idx = CL_FIXNUM_VAL(args[2]);
        /* (SETF AREF) accesses the underlying array, ignoring fill pointers. */
        if (idx < 0 || (uint32_t)idx >= bv->length)
            cl_error(CL_ERR_ARGS, "%SETF-AREF: index %d out of range", (int)idx);
        if (!CL_FIXNUM_P(value))
            cl_error(CL_ERR_TYPE, "%SETF-AREF: value must be 0 or 1 for bit vector");
        val = CL_FIXNUM_VAL(value);
        if (val != 0 && val != 1)
            cl_error(CL_ERR_TYPE, "%SETF-AREF: value must be 0 or 1");
        cl_bv_set_bit(bv, (uint32_t)idx, val);
        return value;
    }

    if (CL_ANY_STRING_P(array)) {
        int32_t idx;
        if (nindices != 1)
            cl_error(CL_ERR_ARGS, "%SETF-AREF: expected 1 index for string");
        if (!CL_FIXNUM_P(args[2]))
            cl_error(CL_ERR_TYPE, "%SETF-AREF: index must be a fixnum");
        idx = CL_FIXNUM_VAL(args[2]);
        if (idx < 0 || (uint32_t)idx >= cl_string_length(array))
            cl_error(CL_ERR_ARGS, "%SETF-AREF: index %d out of range", (int)idx);
        if (!CL_CHAR_P(value))
            cl_error(CL_ERR_TYPE, "%SETF-AREF: value must be a character for string");
        cl_string_set_char_at(array, (uint32_t)idx, CL_CHAR_VAL(value));
        return value;
    }

    if (CL_BYTE_VECTOR_P(array)) {
        CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(array);
        int32_t idx;
        if (nindices != 1)
            cl_error(CL_ERR_ARGS, "%SETF-AREF: expected 1 index for byte vector");
        if (!CL_FIXNUM_P(args[2]))
            cl_error(CL_ERR_TYPE, "%SETF-AREF: index must be a fixnum");
        idx = CL_FIXNUM_VAL(args[2]);
        /* (SETF AREF) accesses the underlying array, ignoring fill pointers. */
        if (idx < 0 || (uint32_t)idx >= bv->length)
            cl_error(CL_ERR_ARGS, "%SETF-AREF: index %d out of range", (int)idx);
        cl_bytevec_set(bv, idx,
                       cl_bytevec_check_value(value, bv->is_signed,
                                              bv->elt_shift,
                                              "(SETF AREF) on a byte vector"));
        return value;
    }

    if (!CL_VECTOR_P(array))
        cl_error(CL_ERR_TYPE, "%SETF-AREF: not an array");
    vec = (CL_Vector *)CL_OBJ_TO_PTR(array);

    if (vec->rank > 1) {
        /* Multi-dimensional */
        if (nindices != (int)vec->rank)
            cl_error(CL_ERR_ARGS, "%SETF-AREF: expected %d indices, got %d",
                     (int)vec->rank, nindices);
        for (d = 0; d < nindices; d++) {
            uint32_t dim_size;
            int32_t idx;
            if (!CL_FIXNUM_P(args[d + 2]))
                cl_error(CL_ERR_TYPE, "%SETF-AREF: index must be a fixnum");
            idx = CL_FIXNUM_VAL(args[d + 2]);
            dim_size = (uint32_t)CL_FIXNUM_VAL(vec->data[d]);
            if (idx < 0 || (uint32_t)idx >= dim_size)
                cl_error(CL_ERR_ARGS, "%SETF-AREF: index %d out of range for dimension %d (size %d)",
                         (int)idx, d, (int)dim_size);
            row_major = row_major * dim_size + (uint32_t)idx;
        }
    } else {
        /* 1D fallback */
        int32_t idx;
        if (nindices != 1)
            cl_error(CL_ERR_ARGS, "%SETF-AREF: expected 1 index, got %d", nindices);
        if (!CL_FIXNUM_P(args[2]))
            cl_error(CL_ERR_TYPE, "%SETF-AREF: index must be a fixnum");
        idx = CL_FIXNUM_VAL(args[2]);
        if (idx < 0 || (uint32_t)idx >= vec->length)
            cl_error(CL_ERR_ARGS, "%SETF-AREF: index %d out of range", (int)idx);
        row_major = (uint32_t)idx;
    }

    cl_vector_data(vec)[row_major] = value;
    return value;
}

/* ======================================================= */
/* VECTOR / VECTORP / ARRAYP / SIMPLE-VECTOR-P /           */
/* ADJUSTABLE-ARRAY-P                                      */
/* ======================================================= */

static CL_Obj bi_vector(CL_Obj *args, int n)
{
    CL_Obj v;
    CL_Vector *vec;
    int i;
    v = cl_make_vector((uint32_t)n);
    vec = (CL_Vector *)CL_OBJ_TO_PTR(v);
    for (i = 0; i < n; i++)
        cl_vector_data(vec)[i] = args[i];
    return v;
}

static CL_Obj bi_vectorp(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    /* Strings, bit-vectors and byte-vectors are vectors per CL spec */
    if (CL_ANY_STRING_P(args[0])) return SYM_T;
    if (CL_BIT_VECTOR_P(args[0])) return SYM_T;
    if (CL_BYTE_VECTOR_P(args[0])) return SYM_T;
    if (!CL_VECTOR_P(args[0])) return CL_NIL;
    /* vectorp is false for multi-dim arrays */
    {
        CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(args[0]);
        return (v->rank <= 1) ? SYM_T : CL_NIL;
    }
}

/* (arrayp obj) — true for any array: vectors, multi-dim arrays, strings, bit-vectors */
static CL_Obj bi_arrayp(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    return (CL_VECTOR_P(args[0]) || CL_ANY_STRING_P(args[0]) ||
            CL_BIT_VECTOR_P(args[0]) || CL_BYTE_VECTOR_P(args[0])) ? SYM_T : CL_NIL;
}

/* (simple-vector-p obj) — true for 1D, element-type T, no fill-pointer, not adjustable */
static CL_Obj bi_simple_vector_p(CL_Obj *args, int n)
{
    CL_Vector *v;
    CL_UNUSED(n);
    if (!CL_VECTOR_P(args[0])) return CL_NIL;
    v = (CL_Vector *)CL_OBJ_TO_PTR(args[0]);
    return (v->rank <= 1 && v->flags == 0) ? SYM_T : CL_NIL;
}

/* (adjustable-array-p array) — true if array was created with :adjustable t */
static CL_Obj bi_adjustable_array_p(CL_Obj *args, int n)
{
    CL_Vector *v;
    CL_UNUSED(n);
    if (CL_BIT_VECTOR_P(args[0])) {
        CL_BitVector *bv = (CL_BitVector *)CL_OBJ_TO_PTR(args[0]);
        return (bv->flags & CL_VEC_FLAG_ADJUSTABLE) ? SYM_T : CL_NIL;
    }
    if (CL_BYTE_VECTOR_P(args[0])) {
        CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(args[0]);
        return (bv->flags & CL_VEC_FLAG_ADJUSTABLE) ? SYM_T : CL_NIL;
    }
    if (!CL_VECTOR_P(args[0]) && !CL_ANY_STRING_P(args[0]))
        cl_error(CL_ERR_TYPE, "ADJUSTABLE-ARRAY-P: not an array");
    if (CL_ANY_STRING_P(args[0])) return CL_NIL;  /* strings are never adjustable */
    v = (CL_Vector *)CL_OBJ_TO_PTR(args[0]);
    return (v->flags & CL_VEC_FLAG_ADJUSTABLE) ? SYM_T : CL_NIL;
}

/* ======================================================= */
/* ARRAY-DISPLACEMENT                                      */
/* ======================================================= */

/* (array-displacement array) → displaced-to, displaced-index-offset
   Returns two values: the array this one is displaced to (or NIL),
   and the displaced-index-offset (or 0). */
static CL_Obj bi_array_displacement(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    if (CL_VECTOR_P(args[0])) {
        CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(args[0]);
        if (v->flags & CL_VEC_FLAG_DISPLACED) {
            uint32_t base = CL_DISP_BASE_IDX(v);
            CL_Obj backing = v->data[base];
            uint32_t offset = 0;
            if (CL_FIXNUM_P(v->data[base + 1]))
                offset = (uint32_t)CL_FIXNUM_VAL(v->data[base + 1]);
            cl_mv_count = 2;
            cl_mv_values[0] = backing;
            cl_mv_values[1] = CL_MAKE_FIXNUM((int32_t)offset);
            return backing;
        }
    }
    /* Not displaced: return NIL, 0 */
    cl_mv_count = 2;
    cl_mv_values[0] = CL_NIL;
    cl_mv_values[1] = CL_MAKE_FIXNUM(0);
    return CL_NIL;
}

/* ======================================================= */
/* ARRAY-DIMENSIONS / ARRAY-RANK                           */
/* ======================================================= */

static CL_Obj bi_array_dimensions(CL_Obj *args, int n)
{
    CL_Vector *vec;
    CL_UNUSED(n);
    if (CL_BIT_VECTOR_P(args[0])) {
        CL_BitVector *bv = (CL_BitVector *)CL_OBJ_TO_PTR(args[0]);
        return cl_cons(CL_MAKE_FIXNUM(bv->length), CL_NIL);
    }
    if (CL_ANY_STRING_P(args[0]))
        return cl_cons(CL_MAKE_FIXNUM((int32_t)cl_string_length(args[0])), CL_NIL);
    if (CL_BYTE_VECTOR_P(args[0])) {
        CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(args[0]);
        return cl_cons(CL_MAKE_FIXNUM((int32_t)bv->length), CL_NIL);
    }
    if (!CL_VECTOR_P(args[0]))
        cl_error(CL_ERR_TYPE, "ARRAY-DIMENSIONS: not an array");
    vec = (CL_Vector *)CL_OBJ_TO_PTR(args[0]);
    if (vec->rank > 1) {
        /* Build list from stored dims */
        CL_Obj result = CL_NIL;
        CL_Obj arr = args[0];
        int i;
        CL_GC_PROTECT(result);
        CL_GC_PROTECT(arr);
        i = (int)vec->rank;
        while (i > 0) {
            i--;
            vec = (CL_Vector *)CL_OBJ_TO_PTR(arr);  /* re-deref after GC */
            result = cl_cons(vec->data[i], result);  /* dims are already fixnums */
        }
        CL_GC_UNPROTECT(2);
        return result;
    }
    return cl_cons(CL_MAKE_FIXNUM(vec->length), CL_NIL);
}

static CL_Obj bi_array_rank(CL_Obj *args, int n)
{
    CL_Vector *vec;
    CL_UNUSED(n);
    if (CL_BIT_VECTOR_P(args[0])) return CL_MAKE_FIXNUM(1);
    if (CL_ANY_STRING_P(args[0])) return CL_MAKE_FIXNUM(1);
    if (CL_BYTE_VECTOR_P(args[0])) return CL_MAKE_FIXNUM(1);
    if (!CL_VECTOR_P(args[0]))
        cl_error(CL_ERR_TYPE, "ARRAY-RANK: not an array");
    vec = (CL_Vector *)CL_OBJ_TO_PTR(args[0]);
    return CL_MAKE_FIXNUM(vec->rank > 1 ? vec->rank : 1);
}

/* ======================================================= */
/* ARRAY-DIMENSION / ARRAY-TOTAL-SIZE                      */
/* ======================================================= */

/* (array-dimension array axis-number) → dimension */
static CL_Obj bi_array_dimension(CL_Obj *args, int n)
{
    CL_Vector *vec;
    int32_t axis;
    CL_UNUSED(n);
    if (CL_BIT_VECTOR_P(args[0])) {
        CL_BitVector *bv = (CL_BitVector *)CL_OBJ_TO_PTR(args[0]);
        if (!CL_FIXNUM_P(args[1]) || CL_FIXNUM_VAL(args[1]) != 0)
            cl_error(CL_ERR_ARGS, "ARRAY-DIMENSION: axis must be 0 for bit vector");
        return CL_MAKE_FIXNUM(bv->length);
    }
    if (CL_ANY_STRING_P(args[0])) {
        if (!CL_FIXNUM_P(args[1]) || CL_FIXNUM_VAL(args[1]) != 0)
            cl_error(CL_ERR_ARGS, "ARRAY-DIMENSION: axis must be 0 for string");
        return CL_MAKE_FIXNUM((int32_t)cl_string_length(args[0]));
    }
    if (CL_BYTE_VECTOR_P(args[0])) {
        CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(args[0]);
        if (!CL_FIXNUM_P(args[1]) || CL_FIXNUM_VAL(args[1]) != 0)
            cl_error(CL_ERR_ARGS, "ARRAY-DIMENSION: axis must be 0 for byte vector");
        return CL_MAKE_FIXNUM((int32_t)bv->length);
    }
    if (!CL_VECTOR_P(args[0]))
        cl_error(CL_ERR_TYPE, "ARRAY-DIMENSION: not an array");
    if (!CL_FIXNUM_P(args[1]))
        cl_error(CL_ERR_TYPE, "ARRAY-DIMENSION: axis must be a fixnum");
    vec = (CL_Vector *)CL_OBJ_TO_PTR(args[0]);
    axis = CL_FIXNUM_VAL(args[1]);
    if (vec->rank > 1) {
        if (axis < 0 || axis >= (int32_t)vec->rank)
            cl_error(CL_ERR_ARGS, "ARRAY-DIMENSION: axis %d out of range (rank %d)",
                     (int)axis, (int)vec->rank);
        return vec->data[axis];  /* dims stored as fixnums */
    }
    /* 1D: only axis 0 is valid */
    if (axis != 0)
        cl_error(CL_ERR_ARGS, "ARRAY-DIMENSION: axis %d out of range (rank 1)", (int)axis);
    return CL_MAKE_FIXNUM(vec->length);
}

/* (array-total-size array) → total number of elements */
static CL_Obj bi_array_total_size(CL_Obj *args, int n)
{
    CL_Vector *vec;
    CL_UNUSED(n);
    if (CL_BIT_VECTOR_P(args[0])) {
        CL_BitVector *bv = (CL_BitVector *)CL_OBJ_TO_PTR(args[0]);
        return CL_MAKE_FIXNUM(bv->length);
    }
    if (CL_ANY_STRING_P(args[0]))
        return CL_MAKE_FIXNUM((int32_t)cl_string_length(args[0]));
    if (CL_BYTE_VECTOR_P(args[0])) {
        CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(args[0]);
        return CL_MAKE_FIXNUM((int32_t)bv->length);
    }
    if (!CL_VECTOR_P(args[0]))
        cl_error(CL_ERR_TYPE, "ARRAY-TOTAL-SIZE: not an array");
    vec = (CL_Vector *)CL_OBJ_TO_PTR(args[0]);
    return CL_MAKE_FIXNUM(vec->length);
}

/* ======================================================= */
/* ARRAY-ELEMENT-TYPE                                      */
/* ======================================================= */

/* (array-element-type array) → typespec
 * Returns the element type of the array.
 * AmiLisp: strings → CHARACTER, bit-vectors → BIT, all others → T. */
static CL_Obj bi_array_element_type(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    if (CL_ANY_STRING_P(args[0]))
        return cl_intern("CHARACTER", 9);
    if (CL_BIT_VECTOR_P(args[0]))
        return cl_intern("BIT", 3);
    if (CL_BYTE_VECTOR_P(args[0])) {
        CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(args[0]);
        return bytevec_element_type_spec(bv->is_signed, bv->elt_shift);
    }
    if (CL_VECTOR_P(args[0])) {
        CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(args[0]);
        if (v->flags & CL_VEC_FLAG_STRING)
            return cl_intern("CHARACTER", 9);
        return elt_code_to_type(v->elt_type);
    }
    cl_error(CL_ERR_TYPE, "ARRAY-ELEMENT-TYPE: not an array");
    return CL_NIL;
}

/* ======================================================= */
/* ARRAY-IN-BOUNDS-P                                       */
/* ======================================================= */

/* (array-in-bounds-p array &rest subscripts) → boolean
 * Returns T if all subscripts are valid indices for array, NIL otherwise. */
static CL_Obj bi_array_in_bounds_p(CL_Obj *args, int n)
{
    int nidx = n - 1;
    int i;

    if (CL_BIT_VECTOR_P(args[0])) {
        CL_BitVector *bv = (CL_BitVector *)CL_OBJ_TO_PTR(args[0]);
        if (nidx != 1) return CL_NIL;
        if (!CL_FIXNUM_P(args[1])) return CL_NIL;
        {
            int32_t idx = CL_FIXNUM_VAL(args[1]);
            return (idx >= 0 && (uint32_t)idx < bv->length) ? CL_T : CL_NIL;
        }
    }
    if (CL_ANY_STRING_P(args[0])) {
        uint32_t len = cl_string_length(args[0]);
        if (nidx != 1) return CL_NIL;
        if (!CL_FIXNUM_P(args[1])) return CL_NIL;
        {
            int32_t idx = CL_FIXNUM_VAL(args[1]);
            return (idx >= 0 && (uint32_t)idx < len) ? CL_T : CL_NIL;
        }
    }
    if (CL_BYTE_VECTOR_P(args[0])) {
        CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(args[0]);
        if (nidx != 1) return CL_NIL;
        if (!CL_FIXNUM_P(args[1])) return CL_NIL;
        {
            int32_t idx = CL_FIXNUM_VAL(args[1]);
            return (idx >= 0 && (uint32_t)idx < bv->length) ? CL_T : CL_NIL;
        }
    }
    if (!CL_VECTOR_P(args[0]))
        cl_error(CL_ERR_TYPE, "ARRAY-IN-BOUNDS-P: not an array");
    {
        CL_Vector *vec = (CL_Vector *)CL_OBJ_TO_PTR(args[0]);
        uint32_t rank = (vec->rank > 1) ? vec->rank : 1;
        if ((uint32_t)nidx != rank) return CL_NIL;
        for (i = 0; i < nidx; i++) {
            int32_t idx, dim;
            if (!CL_FIXNUM_P(args[i + 1])) return CL_NIL;
            idx = CL_FIXNUM_VAL(args[i + 1]);
            if (idx < 0) return CL_NIL;
            vec = (CL_Vector *)CL_OBJ_TO_PTR(args[0]); /* re-fetch */
            dim = (vec->rank > 1) ? CL_FIXNUM_VAL(vec->data[i]) : (int32_t)vec->length;
            if (idx >= dim) return CL_NIL;
        }
    }
    return CL_T;
}

/* ======================================================= */
/* UPGRADED-ARRAY-ELEMENT-TYPE                             */
/* ======================================================= */

/* (upgraded-array-element-type typespec &optional environment) → typespec
 * amilisp upgrades to CHARACTER (strings), BIT (bit-vectors), and the
 * specialized numeric types FIXNUM / SINGLE-FLOAT / DOUBLE-FLOAT (which it
 * keeps distinct from T so a (VECTOR FIXNUM) is not a (VECTOR T) — see
 * cl_classify_vec_elt_code).  Everything else upgrades to T. */
static CL_Obj bi_upgraded_array_element_type(CL_Obj *args, int n)
{
    CL_Obj typespec = args[0];
    int is_char = 0, is_wide_char = 0, is_bit = 0, is_u8 = 0, is_s8 = 0;
    int is_u16 = 0, is_s16 = 0;
    (void)n;

    /* Classify CHARACTER / BIT / byte subtypes, expanding user deftypes (so
     * an alias like flexi-streams' CHAR* upgrades to CHARACTER, not T). */
    cl_classify_array_elt_type(typespec, 16, &is_char, &is_wide_char, &is_bit,
                            &is_u8, &is_s8, &is_u16, &is_s16);
    /* Re-read: cl_classify_array_elt_type may have called cl_vm_apply (deftype
     * expansion) which can trigger a compacting GC, making typespec stale. */
    typespec = args[0];
    if (is_char)
        return cl_intern("CHARACTER", 9);
    if (is_bit)
        return cl_intern("BIT", 3);
    if (is_u8)
        return bytevec_element_type_spec(0, 0);
    if (is_s8)
        return bytevec_element_type_spec(1, 0);
    if (is_u16)
        return bytevec_element_type_spec(0, 1);
    if (is_s16)
        return bytevec_element_type_spec(1, 1);
    return elt_code_to_type(cl_classify_vec_elt_code(typespec, 16));
}

/* ======================================================= */
/* ARRAY-ROW-MAJOR-INDEX                                   */
/* ======================================================= */

/* (array-row-major-index array &rest subscripts) → index */
static CL_Obj bi_array_row_major_index(CL_Obj *args, int n)
{
    CL_Vector *vec;
    int nindices = n - 1;
    uint32_t row_major = 0;
    int d;

    if (CL_BIT_VECTOR_P(args[0])) {
        int32_t idx;
        if (nindices != 1)
            cl_error(CL_ERR_ARGS, "ARRAY-ROW-MAJOR-INDEX: expected 1 subscript for bit vector");
        if (!CL_FIXNUM_P(args[1]))
            cl_error(CL_ERR_TYPE, "ARRAY-ROW-MAJOR-INDEX: subscript must be a fixnum");
        idx = CL_FIXNUM_VAL(args[1]);
        if (idx < 0 || (uint32_t)idx >= ((CL_BitVector *)CL_OBJ_TO_PTR(args[0]))->length)
            cl_error(CL_ERR_ARGS, "ARRAY-ROW-MAJOR-INDEX: subscript out of range");
        return args[1];
    }

    if (CL_ANY_STRING_P(args[0])) {
        int32_t idx;
        if (nindices != 1)
            cl_error(CL_ERR_ARGS, "ARRAY-ROW-MAJOR-INDEX: expected 1 subscript for string");
        if (!CL_FIXNUM_P(args[1]))
            cl_error(CL_ERR_TYPE, "ARRAY-ROW-MAJOR-INDEX: subscript must be a fixnum");
        idx = CL_FIXNUM_VAL(args[1]);
        if (idx < 0 || (uint32_t)idx >= cl_string_length(args[0]))
            cl_error(CL_ERR_ARGS, "ARRAY-ROW-MAJOR-INDEX: subscript out of range");
        return args[1];
    }

    if (CL_BYTE_VECTOR_P(args[0])) {
        int32_t idx;
        if (nindices != 1)
            cl_error(CL_ERR_ARGS, "ARRAY-ROW-MAJOR-INDEX: expected 1 subscript for byte vector");
        if (!CL_FIXNUM_P(args[1]))
            cl_error(CL_ERR_TYPE, "ARRAY-ROW-MAJOR-INDEX: subscript must be a fixnum");
        idx = CL_FIXNUM_VAL(args[1]);
        if (idx < 0 || (uint32_t)idx >= ((CL_ByteVector *)CL_OBJ_TO_PTR(args[0]))->length)
            cl_error(CL_ERR_ARGS, "ARRAY-ROW-MAJOR-INDEX: subscript out of range");
        return args[1];
    }

    if (!CL_VECTOR_P(args[0]))
        cl_error(CL_ERR_TYPE, "ARRAY-ROW-MAJOR-INDEX: not an array");
    vec = (CL_Vector *)CL_OBJ_TO_PTR(args[0]);

    if (vec->rank > 1) {
        if (nindices != (int)vec->rank)
            cl_error(CL_ERR_ARGS, "ARRAY-ROW-MAJOR-INDEX: expected %d subscripts, got %d",
                     (int)vec->rank, nindices);
        for (d = 0; d < nindices; d++) {
            uint32_t dim_size;
            int32_t idx;
            if (!CL_FIXNUM_P(args[d + 1]))
                cl_error(CL_ERR_TYPE, "ARRAY-ROW-MAJOR-INDEX: subscript must be a fixnum");
            idx = CL_FIXNUM_VAL(args[d + 1]);
            dim_size = (uint32_t)CL_FIXNUM_VAL(vec->data[d]);
            if (idx < 0 || (uint32_t)idx >= dim_size)
                cl_error(CL_ERR_ARGS, "ARRAY-ROW-MAJOR-INDEX: subscript %d out of range for dimension %d",
                         (int)idx, d);
            row_major = row_major * dim_size + (uint32_t)idx;
        }
    } else {
        int32_t idx;
        if (nindices != 1)
            cl_error(CL_ERR_ARGS, "ARRAY-ROW-MAJOR-INDEX: expected 1 subscript, got %d", nindices);
        if (!CL_FIXNUM_P(args[1]))
            cl_error(CL_ERR_TYPE, "ARRAY-ROW-MAJOR-INDEX: subscript must be a fixnum");
        idx = CL_FIXNUM_VAL(args[1]);
        if (idx < 0 || (uint32_t)idx >= vec->length)
            cl_error(CL_ERR_ARGS, "ARRAY-ROW-MAJOR-INDEX: subscript %d out of range", (int)idx);
        row_major = (uint32_t)idx;
    }
    return CL_MAKE_FIXNUM(row_major);
}

/* ======================================================= */
/* ROW-MAJOR-AREF / (SETF ROW-MAJOR-AREF)                 */
/* ======================================================= */

/* (row-major-aref array index) → element */
static CL_Obj bi_row_major_aref(CL_Obj *args, int n)
{
    CL_Vector *vec;
    int32_t idx;
    CL_UNUSED(n);
    if (CL_BIT_VECTOR_P(args[0])) {
        CL_BitVector *bv = (CL_BitVector *)CL_OBJ_TO_PTR(args[0]);
        if (!CL_FIXNUM_P(args[1]))
            cl_error(CL_ERR_TYPE, "ROW-MAJOR-AREF: index must be a fixnum");
        idx = CL_FIXNUM_VAL(args[1]);
        if (idx < 0 || (uint32_t)idx >= bv->length)
            cl_error(CL_ERR_ARGS, "ROW-MAJOR-AREF: index out of range");
        return CL_MAKE_FIXNUM(cl_bv_get_bit(bv, (uint32_t)idx));
    }
    if (CL_ANY_STRING_P(args[0])) {
        if (!CL_FIXNUM_P(args[1]))
            cl_error(CL_ERR_TYPE, "ROW-MAJOR-AREF: index must be a fixnum");
        idx = CL_FIXNUM_VAL(args[1]);
        if (idx < 0 || (uint32_t)idx >= cl_string_length(args[0]))
            cl_error(CL_ERR_ARGS, "ROW-MAJOR-AREF: index %d out of range", (int)idx);
        return CL_MAKE_CHAR(cl_string_char_at(args[0], (uint32_t)idx));
    }
    if (CL_BYTE_VECTOR_P(args[0])) {
        CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(args[0]);
        if (!CL_FIXNUM_P(args[1]))
            cl_error(CL_ERR_TYPE, "ROW-MAJOR-AREF: index must be a fixnum");
        idx = CL_FIXNUM_VAL(args[1]);
        if (idx < 0 || (uint32_t)idx >= bv->length)
            cl_error(CL_ERR_ARGS, "ROW-MAJOR-AREF: index out of range");
        return CL_MAKE_FIXNUM(cl_bytevec_get(bv, (uint32_t)idx));
    }
    if (!CL_VECTOR_P(args[0]))
        cl_error(CL_ERR_TYPE, "ROW-MAJOR-AREF: not an array");
    if (!CL_FIXNUM_P(args[1]))
        cl_error(CL_ERR_TYPE, "ROW-MAJOR-AREF: index must be a fixnum");
    vec = (CL_Vector *)CL_OBJ_TO_PTR(args[0]);
    idx = CL_FIXNUM_VAL(args[1]);
    if (idx < 0 || (uint32_t)idx >= vec->length)
        cl_error(CL_ERR_ARGS, "ROW-MAJOR-AREF: index %d out of range (size %d)",
                 (int)idx, (int)vec->length);
    return cl_vector_data(vec)[idx];
}

/* (%SETF-ROW-MAJOR-AREF array index value) → value */
static CL_Obj bi_setf_row_major_aref(CL_Obj *args, int n)
{
    CL_Vector *vec;
    int32_t idx;
    CL_UNUSED(n);
    if (CL_BIT_VECTOR_P(args[0])) {
        CL_BitVector *bv = (CL_BitVector *)CL_OBJ_TO_PTR(args[0]);
        int32_t val;
        if (!CL_FIXNUM_P(args[1]))
            cl_error(CL_ERR_TYPE, "%SETF-ROW-MAJOR-AREF: index must be a fixnum");
        idx = CL_FIXNUM_VAL(args[1]);
        if (idx < 0 || (uint32_t)idx >= bv->length)
            cl_error(CL_ERR_ARGS, "%SETF-ROW-MAJOR-AREF: index out of range");
        if (!CL_FIXNUM_P(args[2]))
            cl_error(CL_ERR_TYPE, "%SETF-ROW-MAJOR-AREF: value must be 0 or 1 for bit vector");
        val = CL_FIXNUM_VAL(args[2]);
        if (val != 0 && val != 1)
            cl_error(CL_ERR_TYPE, "%SETF-ROW-MAJOR-AREF: value must be 0 or 1");
        cl_bv_set_bit(bv, (uint32_t)idx, val);
        return args[2];
    }
    if (CL_ANY_STRING_P(args[0])) {
        if (!CL_FIXNUM_P(args[1]))
            cl_error(CL_ERR_TYPE, "%SETF-ROW-MAJOR-AREF: index must be a fixnum");
        idx = CL_FIXNUM_VAL(args[1]);
        if (idx < 0 || (uint32_t)idx >= cl_string_length(args[0]))
            cl_error(CL_ERR_ARGS, "%SETF-ROW-MAJOR-AREF: index %d out of range", (int)idx);
        if (!CL_CHAR_P(args[2]))
            cl_error(CL_ERR_TYPE, "%SETF-ROW-MAJOR-AREF: value must be a character for string");
        cl_string_set_char_at(args[0], (uint32_t)idx, CL_CHAR_VAL(args[2]));
        return args[2];
    }
    if (CL_BYTE_VECTOR_P(args[0])) {
        CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(args[0]);
        if (!CL_FIXNUM_P(args[1]))
            cl_error(CL_ERR_TYPE, "%SETF-ROW-MAJOR-AREF: index must be a fixnum");
        idx = CL_FIXNUM_VAL(args[1]);
        if (idx < 0 || (uint32_t)idx >= bv->length)
            cl_error(CL_ERR_ARGS, "%SETF-ROW-MAJOR-AREF: index out of range");
        cl_bytevec_set(bv, idx,
                       cl_bytevec_check_value(args[2], bv->is_signed,
                                              bv->elt_shift,
                                              "(SETF ROW-MAJOR-AREF) on a byte vector"));
        return args[2];
    }
    if (!CL_VECTOR_P(args[0]))
        cl_error(CL_ERR_TYPE, "%SETF-ROW-MAJOR-AREF: not an array");
    if (!CL_FIXNUM_P(args[1]))
        cl_error(CL_ERR_TYPE, "%SETF-ROW-MAJOR-AREF: index must be a fixnum");
    vec = (CL_Vector *)CL_OBJ_TO_PTR(args[0]);
    idx = CL_FIXNUM_VAL(args[1]);
    if (idx < 0 || (uint32_t)idx >= vec->length)
        cl_error(CL_ERR_ARGS, "%SETF-ROW-MAJOR-AREF: index %d out of range (size %d)",
                 (int)idx, (int)vec->length);
    cl_vector_data(vec)[idx] = args[2];
    return args[2];
}

/* ======================================================= */
/* FILL-POINTER / (SETF FILL-POINTER) / HAS-FILL-POINTER  */
/* ======================================================= */

/* (fill-pointer vector) → fixnum */
static CL_Obj bi_fill_pointer(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    if (CL_BIT_VECTOR_P(args[0])) {
        CL_BitVector *bv = (CL_BitVector *)CL_OBJ_TO_PTR(args[0]);
        if (bv->fill_pointer == CL_NO_FILL_POINTER)
            cl_error(CL_ERR_TYPE, "FILL-POINTER: bit vector has no fill pointer");
        return CL_MAKE_FIXNUM((int32_t)bv->fill_pointer);
    }
    if (CL_BYTE_VECTOR_P(args[0])) {
        CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(args[0]);
        if (bv->fill_pointer == CL_NO_FILL_POINTER)
            cl_error(CL_ERR_TYPE, "FILL-POINTER: byte vector has no fill pointer");
        return CL_MAKE_FIXNUM((int32_t)bv->fill_pointer);
    }
    {
    CL_Vector *vec;
    if (!CL_VECTOR_P(args[0]))
        cl_error(CL_ERR_TYPE, "FILL-POINTER: not a vector");
    vec = (CL_Vector *)CL_OBJ_TO_PTR(args[0]);
    if (vec->fill_pointer == CL_NO_FILL_POINTER)
        cl_error(CL_ERR_TYPE, "FILL-POINTER: vector has no fill pointer");
    return CL_MAKE_FIXNUM((int32_t)vec->fill_pointer);
    }
}

/* (%SETF-FILL-POINTER vector new-fp) → new-fp */
static CL_Obj bi_setf_fill_pointer(CL_Obj *args, int n)
{
    CL_Vector *vec;
    int32_t new_fp;
    CL_UNUSED(n);
    if (CL_BIT_VECTOR_P(args[0])) {
        CL_BitVector *bv = (CL_BitVector *)CL_OBJ_TO_PTR(args[0]);
        if (bv->fill_pointer == CL_NO_FILL_POINTER)
            cl_error(CL_ERR_TYPE, "(SETF FILL-POINTER): bit vector has no fill pointer");
        if (!CL_FIXNUM_P(args[1]))
            cl_error(CL_ERR_TYPE, "(SETF FILL-POINTER): new value must be a fixnum");
        new_fp = CL_FIXNUM_VAL(args[1]);
        if (new_fp < 0 || (uint32_t)new_fp > bv->length)
            cl_error(CL_ERR_ARGS, "(SETF FILL-POINTER): %d out of range (0-%d)",
                     (int)new_fp, (int)bv->length);
        bv->fill_pointer = (uint32_t)new_fp;
        return args[1];
    }
    if (CL_BYTE_VECTOR_P(args[0])) {
        CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(args[0]);
        if (bv->fill_pointer == CL_NO_FILL_POINTER)
            cl_error(CL_ERR_TYPE, "(SETF FILL-POINTER): byte vector has no fill pointer");
        if (!CL_FIXNUM_P(args[1]))
            cl_error(CL_ERR_TYPE, "(SETF FILL-POINTER): new value must be a fixnum");
        new_fp = CL_FIXNUM_VAL(args[1]);
        if (new_fp < 0 || (uint32_t)new_fp > bv->length)
            cl_error(CL_ERR_ARGS, "(SETF FILL-POINTER): %d out of range (0-%d)",
                     (int)new_fp, (int)bv->length);
        bv->fill_pointer = (uint32_t)new_fp;
        return args[1];
    }
    if (!CL_VECTOR_P(args[0]))
        cl_error(CL_ERR_TYPE, "(SETF FILL-POINTER): not a vector");
    vec = (CL_Vector *)CL_OBJ_TO_PTR(args[0]);
    if (vec->fill_pointer == CL_NO_FILL_POINTER)
        cl_error(CL_ERR_TYPE, "(SETF FILL-POINTER): vector has no fill pointer");
    if (!CL_FIXNUM_P(args[1]))
        cl_error(CL_ERR_TYPE, "(SETF FILL-POINTER): new value must be a fixnum");
    new_fp = CL_FIXNUM_VAL(args[1]);
    if (new_fp < 0 || (uint32_t)new_fp > vec->length)
        cl_error(CL_ERR_ARGS, "(SETF FILL-POINTER): %d out of range (0-%d)",
                 (int)new_fp, (int)vec->length);
    vec->fill_pointer = (uint32_t)new_fp;
    return args[1];
}

/* (array-has-fill-pointer-p array) → boolean */
static CL_Obj bi_array_has_fill_pointer_p(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    if (CL_BIT_VECTOR_P(args[0])) {
        CL_BitVector *bv = (CL_BitVector *)CL_OBJ_TO_PTR(args[0]);
        return (bv->fill_pointer != CL_NO_FILL_POINTER) ? SYM_T : CL_NIL;
    }
    if (CL_BYTE_VECTOR_P(args[0])) {
        CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(args[0]);
        return (bv->fill_pointer != CL_NO_FILL_POINTER) ? SYM_T : CL_NIL;
    }
    if (CL_ANY_STRING_P(args[0])) return CL_NIL;
    {
    CL_Vector *vec;
    if (!CL_VECTOR_P(args[0]))
        cl_error(CL_ERR_TYPE, "ARRAY-HAS-FILL-POINTER-P: not an array");
    vec = (CL_Vector *)CL_OBJ_TO_PTR(args[0]);
    return (vec->fill_pointer != CL_NO_FILL_POINTER) ? SYM_T : CL_NIL;
    }
}

/* ======================================================= */
/* VECTOR-PUSH / VECTOR-PUSH-EXTEND                        */
/* ======================================================= */

/* (vector-push new-element vector) → index or NIL */
static CL_Obj bi_vector_push(CL_Obj *args, int n)
{
    CL_Vector *vec;
    uint32_t fp;
    CL_UNUSED(n);
    if (CL_BYTE_VECTOR_P(args[1])) {
        CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(args[1]);
        if (bv->fill_pointer == CL_NO_FILL_POINTER)
            cl_error(CL_ERR_TYPE, "VECTOR-PUSH: byte vector has no fill pointer");
        fp = bv->fill_pointer;
        if (fp >= bv->length)
            return CL_NIL;  /* full — return NIL */
        cl_bytevec_set(bv, fp,
                       cl_bytevec_check_value(args[0], bv->is_signed,
                                              bv->elt_shift,
                                              "VECTOR-PUSH on a byte vector"));
        bv->fill_pointer = fp + 1;
        return CL_MAKE_FIXNUM((int32_t)fp);
    }
    if (!CL_VECTOR_P(args[1]))
        cl_error(CL_ERR_TYPE, "VECTOR-PUSH: not a vector");
    vec = (CL_Vector *)CL_OBJ_TO_PTR(args[1]);
    if (vec->fill_pointer == CL_NO_FILL_POINTER)
        cl_error(CL_ERR_TYPE, "VECTOR-PUSH: vector has no fill pointer");
    fp = vec->fill_pointer;
    if (fp >= vec->length)
        return CL_NIL;  /* full — return NIL */
    cl_vector_data(vec)[fp] = args[0];
    vec->fill_pointer = fp + 1;
    return CL_MAKE_FIXNUM((int32_t)fp);
}

/* (vector-push-extend new-element vector &optional extension) → index
   If room: stores element at fill-pointer, increments fp, returns old fp.
   If no room: extends the vector (allocates new backing storage, copies data).
   Note: arena allocator cannot resize in-place, so the old vector object
   is patched with the new capacity by copying back over the old allocation
   if the new size fits, otherwise errors. Pre-allocate enough capacity. */
/* Non-static: exercised directly by tests/test_gc_mem_hardening.c to hit
 * the rank>1 displacement backstop without needing Lisp-level access to a
 * rank>1 fill-pointer/adjustable vector (unreachable via MAKE-ARRAY). */
CL_Obj bi_vector_push_extend(CL_Obj *args, int n)
{
    CL_Vector *vec;
    uint32_t fp, new_cap, old_len, i;
    CL_Obj new_arr;
    CL_Vector *new_vec;
    CL_Obj *old_data, *new_data;

    if (CL_BYTE_VECTOR_P(args[1])) {
        /* Byte vectors have no displacement mechanism, so the identity-
         * preserving in-place extension used for general vectors below is
         * not possible.  Push while there is room; fail loudly when full
         * instead of silently substituting a fresh object. */
        CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(args[1]);
        if (bv->fill_pointer == CL_NO_FILL_POINTER)
            cl_error(CL_ERR_TYPE, "VECTOR-PUSH-EXTEND: byte vector has no fill pointer");
        fp = bv->fill_pointer;
        if (fp < bv->length) {
            cl_bytevec_set(bv, fp,
                           cl_bytevec_check_value(args[0], bv->is_signed,
                                                  bv->elt_shift,
                                                  "VECTOR-PUSH-EXTEND on a byte vector"));
            bv->fill_pointer = fp + 1;
            return CL_MAKE_FIXNUM((int32_t)fp);
        }
        cl_error(CL_ERR_GENERAL,
                 "VECTOR-PUSH-EXTEND: cannot extend a packed %s vector - "
                 "pre-allocate enough capacity with MAKE-ARRAY",
                 cl_bytevec_type_name(bv->is_signed, bv->elt_shift));
    }
    if (!CL_VECTOR_P(args[1]))
        cl_error(CL_ERR_TYPE, "VECTOR-PUSH-EXTEND: not a vector");
    vec = (CL_Vector *)CL_OBJ_TO_PTR(args[1]);
    if (vec->fill_pointer == CL_NO_FILL_POINTER)
        cl_error(CL_ERR_TYPE, "VECTOR-PUSH-EXTEND: vector has no fill pointer");
    fp = vec->fill_pointer;
    if (fp < vec->length) {
        /* Room available — same as vector-push */
        cl_vector_data(vec)[fp] = args[0];
        vec->fill_pointer = fp + 1;
        return CL_MAKE_FIXNUM((int32_t)fp);
    }

    /* Need to extend: compute new capacity.  All arithmetic is capped
     * against CL_ARRAY_DIMENSION_LIMIT — a huge or negative extension
     * argument would otherwise wrap `old_len + ext` (uint32) and allocate
     * a tiny backing store whose new_data[fp] write lands out of bounds. */
    old_len = vec->length;
    if (old_len >= CL_ARRAY_DIMENSION_LIMIT)
        cl_error(CL_ERR_GENERAL,
                 "VECTOR-PUSH-EXTEND: cannot extend beyond ARRAY-DIMENSION-LIMIT (%u)",
                 (unsigned)CL_ARRAY_DIMENSION_LIMIT);
    new_cap = old_len * 2;
    if (new_cap < 4) new_cap = 4;
    /* Honor optional extension argument */
    if (n >= 3 && CL_FIXNUM_P(args[2])) {
        int32_t ext_val = CL_FIXNUM_VAL(args[2]);
        if (ext_val < 0)
            cl_error(CL_ERR_TYPE,
                     "VECTOR-PUSH-EXTEND: extension must be a non-negative integer (got %d)",
                     (int)ext_val);
        /* CLHS: the extension is the MINIMUM number of elements to add — if
         * honoring it would exceed ARRAY-DIMENSION-LIMIT, signal instead of
         * silently clamping below the requested minimum. */
        if ((uint32_t)ext_val > CL_ARRAY_DIMENSION_LIMIT - old_len)
            cl_error(CL_ERR_TYPE,
                     "VECTOR-PUSH-EXTEND: extension %d would exceed ARRAY-DIMENSION-LIMIT (%u)",
                     (int)ext_val, (unsigned)CL_ARRAY_DIMENSION_LIMIT);
        if (new_cap < old_len + (uint32_t)ext_val)
            new_cap = old_len + (uint32_t)ext_val;
    }
    /* The doubling path may overshoot the limit — clamping is fine there
     * (growth of at least one element is all that's required). */
    if (new_cap > CL_ARRAY_DIMENSION_LIMIT)
        new_cap = CL_ARRAY_DIMENSION_LIMIT;

    /* Allocate new backing vector (GC may fire).  args[] slots are already
     * GC-rooted VM-stack slots — protecting them again would double-forward
     * them on compaction (gc_forward is not idempotent). */
    new_arr = cl_make_array(new_cap, 0, NULL,
                            CL_VEC_FLAG_ADJUSTABLE, CL_NO_FILL_POINTER);

    /* Re-fetch after potential GC */
    vec = (CL_Vector *)CL_OBJ_TO_PTR(args[1]);
    new_vec = (CL_Vector *)CL_OBJ_TO_PTR(new_arr);

    /* Copy old data to new backing vector */
    old_data = cl_vector_data(vec);
    new_data = cl_vector_data(new_vec);
    for (i = 0; i < old_len; i++)
        new_data[i] = old_data[i];

    /* Store new element */
    new_data[fp] = args[0];

    /* Displace old vector to new backing vector.
     * GC displacement contract: the backing ref lives at
     * data[CL_DISP_BASE_IDX] — data[rank] for rank>1, data[0] otherwise.
     * This in-place write assumes rank<=1; on a rank>1 array the GC's
     * mark/relocate would read a dimension fixnum as the backing pointer
     * and silently corrupt.  Unreachable today (fill pointers exist only
     * on vectors), so fail loudly if the invariant ever breaks. */
    if (vec->rank > 1)
        cl_error(CL_ERR_GENERAL,
                 "VECTOR-PUSH-EXTEND: internal error - cannot displace a "
                 "rank-%u array in place", (unsigned)vec->rank);
    vec->data[0] = new_arr;
    vec->data[1] = CL_MAKE_FIXNUM(0);  /* displacement offset = 0 */
    vec->flags |= CL_VEC_FLAG_DISPLACED;
    vec->length = new_cap;
    vec->fill_pointer = fp + 1;

    return CL_MAKE_FIXNUM((int32_t)fp);
}

/* (vector-pop vector) → element
   Decrements fill pointer and returns the element at the new position. */
static CL_Obj bi_vector_pop(CL_Obj *args, int n)
{
    CL_Vector *vec;
    uint32_t fp;
    CL_UNUSED(n);
    if (CL_BYTE_VECTOR_P(args[0])) {
        CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(args[0]);
        if (bv->fill_pointer == CL_NO_FILL_POINTER)
            cl_error(CL_ERR_TYPE, "VECTOR-POP: byte vector has no fill pointer");
        fp = bv->fill_pointer;
        if (fp == 0)
            cl_error(CL_ERR_GENERAL, "VECTOR-POP: fill pointer is zero");
        bv->fill_pointer = fp - 1;
        return CL_MAKE_FIXNUM(cl_bytevec_get(bv, fp - 1));
    }
    if (!CL_VECTOR_P(args[0]))
        cl_error(CL_ERR_TYPE, "VECTOR-POP: not a vector");
    vec = (CL_Vector *)CL_OBJ_TO_PTR(args[0]);
    if (vec->fill_pointer == CL_NO_FILL_POINTER)
        cl_error(CL_ERR_TYPE, "VECTOR-POP: vector has no fill pointer");
    fp = vec->fill_pointer;
    if (fp == 0)
        cl_error(CL_ERR_GENERAL, "VECTOR-POP: fill pointer is zero");
    vec->fill_pointer = fp - 1;
    return cl_vector_data(vec)[fp - 1];
}

/* ======================================================= */
/* ADJUST-ARRAY                                            */
/* ======================================================= */

/* (adjust-array array new-dimensions &key :initial-element :fill-pointer) → new-array */
/* Non-static: see bi_vector_push_extend comment above. */
CL_Obj bi_adjust_array(CL_Obj *args, int n)
{
    CL_Vector *old_vec;
    CL_Vector *new_vec;
    CL_Obj new_arr;
    uint32_t new_len, old_len, copy_len, i;
    CL_Obj initial_element = CL_NIL;
    int has_ie = 0;
    uint32_t new_fp = CL_NO_FILL_POINTER;
    int has_fp = 0;

    int is_bytevec = CL_BYTE_VECTOR_P(args[0]);

    if (!is_bytevec && !CL_VECTOR_P(args[0]))
        cl_error(CL_ERR_TYPE, "ADJUST-ARRAY: not an array");
    old_vec = NULL;
    if (!is_bytevec) {
        old_vec = (CL_Vector *)CL_OBJ_TO_PTR(args[0]);
        if (old_vec->rank > 1)
            cl_error(CL_ERR_GENERAL, "ADJUST-ARRAY: multi-dimensional arrays not yet supported");
    }

    /* Parse new-dimensions (fixnum for 1D).  A negative dimension must be
     * rejected HERE: casting it to uint32_t wraps to a near-2^32 length whose
     * alloc_size computation wraps again inside cl_make_array — a silent heap
     * smash, not an error.  Same for dims over ARRAY-DIMENSION-LIMIT (mirrors
     * MAKE-ARRAY's checks; both are catchable type-errors per CLHS). */
    if (CL_FIXNUM_P(args[1])) {
        if (CL_FIXNUM_VAL(args[1]) < 0)
            cl_error(CL_ERR_TYPE,
                     "ADJUST-ARRAY: array dimension must be non-negative (got %d)",
                     (int)CL_FIXNUM_VAL(args[1]));
        new_len = (uint32_t)CL_FIXNUM_VAL(args[1]);
    } else if (CL_CONS_P(args[1])) {
        if (!CL_FIXNUM_P(cl_car(args[1])))
            cl_error(CL_ERR_TYPE, "ADJUST-ARRAY: dimension must be a fixnum");
        if (CL_FIXNUM_VAL(cl_car(args[1])) < 0)
            cl_error(CL_ERR_TYPE,
                     "ADJUST-ARRAY: array dimension must be non-negative (got %d)",
                     (int)CL_FIXNUM_VAL(cl_car(args[1])));
        new_len = (uint32_t)CL_FIXNUM_VAL(cl_car(args[1]));
    } else {
        cl_error(CL_ERR_TYPE, "ADJUST-ARRAY: invalid dimensions");
        return CL_NIL;
    }
    if (new_len > CL_ARRAY_DIMENSION_LIMIT)
        cl_error(CL_ERR_TYPE,
                 "ADJUST-ARRAY: array dimension %u exceeds ARRAY-DIMENSION-LIMIT (%u)",
                 (unsigned)new_len, (unsigned)CL_ARRAY_DIMENSION_LIMIT);

    /* Parse keyword args */
    for (i = 2; i + 1 < (uint32_t)n; i += 2) {
        if (args[i] == KW_INITIAL_ELEMENT) {
            initial_element = args[i + 1];
            has_ie = 1;
        } else if (args[i] == KW_FILL_POINTER) {
            if (args[i + 1] == SYM_T) {
                /* :fill-pointer T means set to total size (CLHS) */
                new_fp = new_len;
                has_fp = 1;
            } else if (CL_FIXNUM_P(args[i + 1])) {
                new_fp = (uint32_t)CL_FIXNUM_VAL(args[i + 1]);
                has_fp = 1;
            }
        }
    }

    /* Packed byte vector: never adjustable (make-array builds adjustable
     * byte arrays as annotated general vectors), so CLHS permits returning
     * a FRESH array instead of adjusting in place.  Copy the raw bytes,
     * fill any growth from :initial-element, carry the fill pointer. */
    if (is_bytevec) {
        CL_ByteVector *obv = (CL_ByteVector *)CL_OBJ_TO_PTR(args[0]);
        int is_signed = obv->is_signed;
        int elt_shift = obv->elt_shift;
        uint32_t src_len = obv->length;
        int32_t fill_val = 0;
        CL_Obj nbv;
        CL_ByteVector *nb;

        if (!has_fp && obv->fill_pointer != CL_NO_FILL_POINTER) {
            new_fp = obv->fill_pointer;
            if (new_fp > new_len) new_fp = new_len;
        }
        if (has_ie)
            fill_val = cl_bytevec_check_value(initial_element, is_signed,
                                              elt_shift,
                                              "ADJUST-ARRAY :initial-element");
        nbv = cl_make_byte_vector(new_len, is_signed, elt_shift);
        /* Re-fetch the source after the (compacting) allocation — args[0]
         * is an already-rooted VM-stack slot. */
        obv = (CL_ByteVector *)CL_OBJ_TO_PTR(args[0]);
        nb = (CL_ByteVector *)CL_OBJ_TO_PTR(nbv);
        copy_len = src_len < new_len ? src_len : new_len;
        if (copy_len > 0)
            memcpy(nb->data, obv->data, (size_t)copy_len << elt_shift);
        if (has_ie && new_len > src_len && fill_val != 0) {
            for (i = src_len; i < new_len; i++)
                cl_bytevec_set(nb, i, fill_val);
        }
        if (new_fp != CL_NO_FILL_POINTER)
            nb->flags |= CL_VEC_FLAG_FILL_POINTER;
        nb->fill_pointer = new_fp;
        return nbv;
    }

    /* Determine fill pointer for new array */
    if (!has_fp && old_vec->fill_pointer != CL_NO_FILL_POINTER) {
        /* Preserve existing fill pointer */
        new_fp = old_vec->fill_pointer;
        if (new_fp > new_len) new_fp = new_len;
    }

    old_len = old_vec->length;
    copy_len = old_len < new_len ? old_len : new_len;

    /* Allocate new backing array.  Mask out DISPLACED: the new array is a
     * FRESH backing store, not a displacement.  Carrying DISPLACED over from
     * an already-adjusted (hence displaced) input would make cl_make_array
     * produce a vector flagged DISPLACED but with no backing pointer in
     * data[0], so a later cl_vector_data() would follow garbage and crash.
     * This is exactly what repeated ADJUST-ARRAY growth (e.g. drakma/chunga
     * growing an (unsigned-byte 8) response buffer) triggers on the 2nd grow. */
    {
        uint8_t new_flags =
            (old_vec->flags | CL_VEC_FLAG_ADJUSTABLE) & ~CL_VEC_FLAG_DISPLACED;
        /* GC SAFETY: initial_element is read AFTER this allocation (the
         * new-element fill below) — keep it forwarded across the compaction.
         * args[0] is NOT protected: it is an already-rooted VM-stack slot,
         * and double-registering it would double-forward it. */
        CL_GC_PROTECT(initial_element);
        new_arr = cl_make_array(new_len, 0, NULL, new_flags, new_fp);
        CL_GC_UNPROTECT(1);
    }

    /* Re-fetch after potential GC */
    old_vec = (CL_Vector *)CL_OBJ_TO_PTR(args[0]);
    new_vec = (CL_Vector *)CL_OBJ_TO_PTR(new_arr);

    /* Copy old data */
    {
        CL_Obj *src = cl_vector_data(old_vec);
        CL_Obj *dst = cl_vector_data(new_vec);
        for (i = 0; i < copy_len; i++)
            dst[i] = src[i];

        /* Initialize new elements */
        if (has_ie && new_len > old_len) {
            for (i = old_len; i < new_len; i++)
                dst[i] = initial_element;
        }
        /* Preserve the declared element type (incl. the U8/S8 annotation of
         * adjustable byte arrays) on the fresh array — for the adjustable
         * in-place path below the identity object keeps its own elt_type. */
        new_vec->elt_type = old_vec->elt_type;
    }

    if (old_vec->flags & CL_VEC_FLAG_ADJUSTABLE) {
        /* Adjustable: modify in place via displacement (CL spec identity).
         * Same GC displacement contract as VECTOR-PUSH-EXTEND: the
         * data[0]/data[1] writes require rank<=1 (guarded at entry; this
         * backstop keeps the GC contract loud if that guard ever moves). */
        if (old_vec->rank > 1)
            cl_error(CL_ERR_GENERAL,
                     "ADJUST-ARRAY: internal error - cannot displace a "
                     "rank-%u array in place", (unsigned)old_vec->rank);
        old_vec->data[0] = new_arr;
        old_vec->data[1] = CL_MAKE_FIXNUM(0);  /* displacement offset = 0 */
        old_vec->flags |= CL_VEC_FLAG_DISPLACED;
        old_vec->length = new_len;
        old_vec->fill_pointer = new_fp;
        return args[0];  /* Same identity */
    }

    return new_arr;
}

/* ======================================================= */
/* Registration                                            */
/* ======================================================= */

void cl_builtins_array_init(void)
{
    /* Pre-intern keyword symbols */
    KW_INITIAL_ELEMENT  = cl_intern_keyword("INITIAL-ELEMENT", 15);
    KW_INITIAL_CONTENTS = cl_intern_keyword("INITIAL-CONTENTS", 16);
    KW_FILL_POINTER     = cl_intern_keyword("FILL-POINTER", 12);
    KW_ADJUSTABLE       = cl_intern_keyword("ADJUSTABLE", 10);
    KW_ELEMENT_TYPE     = cl_intern_keyword("ELEMENT-TYPE", 12);
    KW_DISPLACED_TO     = cl_intern_keyword("DISPLACED-TO", 12);
    KW_DISPLACED_INDEX_OFFSET = cl_intern_keyword("DISPLACED-INDEX-OFFSET", 22);

    /* Array construction */
    defun("MAKE-ARRAY", bi_make_array, 1, -1);
    defun("VECTOR", bi_vector, 0, -1);

    /* Access */
    defun("AREF", bi_aref, 2, -1);
    defun("SVREF", bi_svref, 2, 2);
    cl_register_builtin("%SETF-AREF", bi_setf_aref, 3, -1, cl_package_amilisp);

    /* Predicates */
    defun("VECTORP", bi_vectorp, 1, 1);
    defun("ARRAYP", bi_arrayp, 1, 1);
    defun("SIMPLE-VECTOR-P", bi_simple_vector_p, 1, 1);
    defun("ADJUSTABLE-ARRAY-P", bi_adjustable_array_p, 1, 1);
    defun("ARRAY-DISPLACEMENT", bi_array_displacement, 1, 1);

    /* Query */
    defun("ARRAY-DIMENSIONS", bi_array_dimensions, 1, 1);
    defun("ARRAY-RANK", bi_array_rank, 1, 1);
    defun("ARRAY-DIMENSION", bi_array_dimension, 2, 2);
    defun("ARRAY-TOTAL-SIZE", bi_array_total_size, 1, 1);
    defun("ARRAY-ELEMENT-TYPE", bi_array_element_type, 1, 1);
    defun("ARRAY-IN-BOUNDS-P", bi_array_in_bounds_p, 1, -1);
    defun("UPGRADED-ARRAY-ELEMENT-TYPE", bi_upgraded_array_element_type, 1, 2);
    defun("ARRAY-ROW-MAJOR-INDEX", bi_array_row_major_index, 2, -1);
    defun("ROW-MAJOR-AREF", bi_row_major_aref, 2, 2);
    cl_register_builtin("%SETF-ROW-MAJOR-AREF", bi_setf_row_major_aref, 3, 3, cl_package_amilisp);

    /* Fill pointer */
    defun("FILL-POINTER", bi_fill_pointer, 1, 1);
    cl_register_builtin("%SETF-FILL-POINTER", bi_setf_fill_pointer, 2, 2, cl_package_amilisp);
    defun("ARRAY-HAS-FILL-POINTER-P", bi_array_has_fill_pointer_p, 1, 1);
    defun("VECTOR-PUSH", bi_vector_push, 2, 2);
    defun("VECTOR-PUSH-EXTEND", bi_vector_push_extend, 2, 3);
    defun("VECTOR-POP", bi_vector_pop, 1, 1);
    defun("ADJUST-ARRAY", bi_adjust_array, 2, -1);

    /* Register cached symbols for GC compaction forwarding */
    cl_gc_register_root(&KW_INITIAL_ELEMENT);
    cl_gc_register_root(&KW_INITIAL_CONTENTS);
    cl_gc_register_root(&KW_FILL_POINTER);
    cl_gc_register_root(&KW_ADJUSTABLE);
    cl_gc_register_root(&KW_ELEMENT_TYPE);
    cl_gc_register_root(&KW_DISPLACED_TO);
    cl_gc_register_root(&KW_DISPLACED_INDEX_OFFSET);
}
