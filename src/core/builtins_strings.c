#include "builtins.h"
#include "symbol.h"
#include "package.h"
#include "mem.h"
#include "error.h"
#include "printer.h"
#include "vm.h"
#include "string_utils.h"
#include "bignum.h"
#include "../platform/platform.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#if defined(PLATFORM_POSIX) || defined(PLATFORM_WIN32)
#include <wctype.h>
#endif

/* Unicode character classification helpers.
 * POSIX: use wctype.h functions for full Unicode support.
 * Windows: the same, but only up to U+FFFF — see CL_WCTYPE_RANGE below.
 * AmigaOS: Latin-1 range checks (sufficient for most CL code).
 *
 * wint_t is 32-bit on POSIX and every codepoint fits, but on Windows
 * (mingw-w64/UCRT) it is 16 bits wide — WEOF is 0xFFFF — while
 * CHAR-CODE-LIMIT is #x110000 under CL_WIDE_STRINGS.  Handing an astral
 * codepoint to one of the isw/tow functions there silently truncates it to
 * its low half, so CHAR-UPCASE of U+10428 (DESERET SMALL LETTER LONG I)
 * came back as U+0428
 * (CYRILLIC CAPITAL LETTER SHA), and (STRING-UPCASE "<emoji>") corrupted its
 * argument.  Above the BMP on Windows the answer is "not a letter, no case"
 * — wrong for the handful of cased astral scripts (Deseret, Adlam, Warang
 * Citi, Osage, Vithkuqi), right for everything else including every emoji,
 * and never destructive. */
#if defined(PLATFORM_WIN32)
#define CL_WCTYPE_RANGE(c) ((c) > 127 && (c) <= 0xFFFF)
#else
#define CL_WCTYPE_RANGE(c) ((c) > 127)
#endif

static int cl_isalpha(int c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return 1;
#if defined(PLATFORM_POSIX) || defined(PLATFORM_WIN32)
    if (CL_WCTYPE_RANGE(c)) return iswalpha((wint_t)c);
#else
    /* Latin-1 Supplement letters */
    if (c >= 0xC0 && c <= 0xD6) return 1;  /* À-Ö */
    if (c >= 0xD8 && c <= 0xF6) return 1;  /* Ø-ö */
    if (c >= 0xF8 && c <= 0xFF) return 1;  /* ø-ÿ */
#endif
    return 0;
}

static int cl_isupper(int c)
{
    if (c >= 'A' && c <= 'Z') return 1;
#if defined(PLATFORM_POSIX) || defined(PLATFORM_WIN32)
    if (CL_WCTYPE_RANGE(c)) return iswupper((wint_t)c);
#else
    if (c >= 0xC0 && c <= 0xD6) return 1;  /* À-Ö */
    if (c >= 0xD8 && c <= 0xDE) return 1;  /* Ø-Þ */
#endif
    return 0;
}

static int cl_islower(int c)
{
    if (c >= 'a' && c <= 'z') return 1;
#if defined(PLATFORM_POSIX) || defined(PLATFORM_WIN32)
    if (CL_WCTYPE_RANGE(c)) return iswlower((wint_t)c);
#else
    if (c >= 0xE0 && c <= 0xF6) return 1;  /* à-ö */
    if (c >= 0xF8 && c <= 0xFF) return 1;  /* ø-ÿ */
#endif
    return 0;
}

static int cl_toupper(int c)
{
    if (c >= 'a' && c <= 'z') return c - 32;
#if defined(PLATFORM_POSIX) || defined(PLATFORM_WIN32)
    if (CL_WCTYPE_RANGE(c)) return (int)towupper((wint_t)c);
#else
    if (c >= 0xE0 && c <= 0xF6) return c - 32;  /* à-ö → À-Ö */
    if (c >= 0xF8 && c <= 0xFE) return c - 32;  /* ø-þ → Ø-Þ */
#endif
    return c;
}

static int cl_tolower(int c)
{
    if (c >= 'A' && c <= 'Z') return c + 32;
#if defined(PLATFORM_POSIX) || defined(PLATFORM_WIN32)
    if (CL_WCTYPE_RANGE(c)) return (int)towlower((wint_t)c);
#else
    if (c >= 0xC0 && c <= 0xD6) return c + 32;  /* À-Ö → à-ö */
    if (c >= 0xD8 && c <= 0xDE) return c + 32;  /* Ø-Þ → ø-þ */
#endif
    return c;
}

/* --- Character functions --- */

DEFINE_TYPE_PREDICATE(bi_characterp, CL_CHAR_P)

/* Char comparison chains all share one body shape; FOLD selects case
 * sensitivity: CL_CHAR_VAL for char=/char</... (exact), char_upcase_val for
 * char-equal/char-lessp/... (case-insensitive).  These are body-generating
 * macros — NOT a runtime mode switch — so the emitted code is byte-identical
 * to the hand-written comparators (char=/char< are hot). */
#define DEFINE_CHAR_ORDER_CMP(fn, who, FOLD, OP)                      \
    static CL_Obj fn(CL_Obj *args, int n)                            \
    {                                                                \
        int i;                                                       \
        for (i = 0; i < n; i++)                                      \
            if (!CL_CHAR_P(args[i]))                                 \
                cl_error(CL_ERR_TYPE, who ": not a character");      \
        for (i = 1; i < n; i++)                                      \
            if (!(FOLD(args[i-1]) OP FOLD(args[i])))                 \
                return CL_NIL;                                       \
        return SYM_T;                                                \
    }

#define DEFINE_CHAR_NE_CMP(fn, who, FOLD)                            \
    static CL_Obj fn(CL_Obj *args, int n)                           \
    {                                                               \
        int i, j;                                                   \
        for (i = 0; i < n; i++)                                     \
            if (!CL_CHAR_P(args[i]))                                \
                cl_error(CL_ERR_TYPE, who ": not a character");     \
        for (i = 0; i < n; i++)                                     \
            for (j = i + 1; j < n; j++)                             \
                if (FOLD(args[i]) == FOLD(args[j]))                 \
                    return CL_NIL;                                  \
        return SYM_T;                                               \
    }

DEFINE_CHAR_ORDER_CMP(bi_char_eq, "CHAR=",  CL_CHAR_VAL, ==)
DEFINE_CHAR_NE_CMP   (bi_char_ne, "CHAR/=", CL_CHAR_VAL)
DEFINE_CHAR_ORDER_CMP(bi_char_lt, "CHAR<",  CL_CHAR_VAL, <)
DEFINE_CHAR_ORDER_CMP(bi_char_gt, "CHAR>",  CL_CHAR_VAL, >)
DEFINE_CHAR_ORDER_CMP(bi_char_le, "CHAR<=", CL_CHAR_VAL, <=)
DEFINE_CHAR_ORDER_CMP(bi_char_ge, "CHAR>=", CL_CHAR_VAL, >=)

/* Type-check O is a character and return its code point; signals a TYPE-ERROR
 * tagged with WHO otherwise.  Collapses the check/extract idiom repeated across
 * the single-arg char accessors. */
static int require_char(CL_Obj o, const char *who)
{
    if (!CL_CHAR_P(o))
        cl_error(CL_ERR_TYPE, "%s: not a character", who);
    return CL_CHAR_VAL(o);
}

static CL_Obj bi_char_code(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    return CL_MAKE_FIXNUM(require_char(args[0], "CHAR-CODE"));
}

static CL_Obj bi_code_char(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    if (!CL_FIXNUM_P(args[0]))
        cl_error(CL_ERR_TYPE, "CODE-CHAR: not an integer");
    return CL_MAKE_CHAR(CL_FIXNUM_VAL(args[0]));
}

/* (char-int character) => integer  — implementation-specific integer rep.
 * In amilisp this is identical to char-code (Unicode code point). */
static CL_Obj bi_char_int(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    return CL_MAKE_FIXNUM(require_char(args[0], "CHAR-INT"));
}

/* (character object) => character
 * Coerce a character designator to a character.
 * Designators: a character (returned as-is), a string of length 1,
 * or a symbol whose name has length 1. */
static CL_Obj bi_character(CL_Obj *args, int n)
{
    CL_Obj obj = args[0];
    CL_UNUSED(n);

    if (CL_CHAR_P(obj))
        return obj;

    if (CL_ANY_STRING_P(obj)) {
        if (cl_string_length(obj) != 1)
            cl_error(CL_ERR_TYPE, "CHARACTER: string designator must have length 1");
        return CL_MAKE_CHAR(cl_string_char_at(obj, 0));
    }

    if (CL_SYMBOL_P(obj) || CL_NULL_P(obj)) {
        CL_Symbol *sym = CL_NULL_P(obj)
            ? (CL_Symbol *)CL_OBJ_TO_PTR(SYM_NIL)
            : (CL_Symbol *)CL_OBJ_TO_PTR(obj);
        CL_String *s = (CL_String *)CL_OBJ_TO_PTR(sym->name);
        if (s->length != 1)
            cl_error(CL_ERR_TYPE, "CHARACTER: symbol name must have length 1");
        return CL_MAKE_CHAR((unsigned char)s->data[0]);
    }

    cl_error(CL_ERR_TYPE, "CHARACTER: not a character designator");
    return CL_NIL;
}

static CL_Obj bi_char_upcase(CL_Obj *args, int n)
{
    int c;
    CL_UNUSED(n);
    c = require_char(args[0], "CHAR-UPCASE");
    return CL_MAKE_CHAR(cl_toupper(c));
}

static CL_Obj bi_char_downcase(CL_Obj *args, int n)
{
    int c;
    CL_UNUSED(n);
    c = require_char(args[0], "CHAR-DOWNCASE");
    return CL_MAKE_CHAR(cl_tolower(c));
}

static CL_Obj bi_upper_case_p(CL_Obj *args, int n)
{
    int c;
    CL_UNUSED(n);
    c = require_char(args[0], "UPPER-CASE-P");
    return cl_isupper(c) ? SYM_T : CL_NIL;
}

static CL_Obj bi_lower_case_p(CL_Obj *args, int n)
{
    int c;
    CL_UNUSED(n);
    c = require_char(args[0], "LOWER-CASE-P");
    return cl_islower(c) ? SYM_T : CL_NIL;
}

static CL_Obj bi_alpha_char_p(CL_Obj *args, int n)
{
    int c;
    CL_UNUSED(n);
    c = require_char(args[0], "ALPHA-CHAR-P");
    return cl_isalpha(c) ? SYM_T : CL_NIL;
}

static CL_Obj bi_digit_char_p(CL_Obj *args, int n)
{
    int c, radix, weight;
    c = require_char(args[0], "DIGIT-CHAR-P");
    radix = (n > 1 && CL_FIXNUM_P(args[1])) ? CL_FIXNUM_VAL(args[1]) : 10;
    if (c >= '0' && c <= '9')
        weight = c - '0';
    else if (c >= 'A' && c <= 'Z')
        weight = c - 'A' + 10;
    else if (c >= 'a' && c <= 'z')
        weight = c - 'a' + 10;
    else
        return CL_NIL;
    return (weight < radix) ? CL_MAKE_FIXNUM(weight) : CL_NIL;
}

/* --- Symbol functions --- */

static CL_Obj bi_symbol_name(CL_Obj *args, int n)
{
    CL_Symbol *s;
    CL_UNUSED(n);
    if (CL_NULL_P(args[0]))
        return cl_make_string("NIL", 3);
    if (!CL_SYMBOL_P(args[0]))
        cl_signal_type_error(args[0], "SYMBOL", "SYMBOL-NAME");
    s = (CL_Symbol *)CL_OBJ_TO_PTR(args[0]);
    return s->name;
}

static CL_Obj bi_symbol_package(CL_Obj *args, int n)
{
    CL_Symbol *s;
    CL_UNUSED(n);
    if (CL_NULL_P(args[0]))
        return cl_package_cl;
    if (!CL_SYMBOL_P(args[0]))
        cl_signal_type_error(args[0], "SYMBOL", "SYMBOL-PACKAGE");
    s = (CL_Symbol *)CL_OBJ_TO_PTR(args[0]);
    return s->package;
}

static CL_Obj bi_make_symbol(CL_Obj *args, int n)
{
    CL_Obj name = args[0];
    CL_UNUSED(n);
    /* cl_make_uninterned_symbol, not bare cl_make_symbol: it also fills
     * the symbol's cached name hash.  The package tables bucket by that
     * hash, so an uninterned symbol that is later IMPORTed / EXPORTed /
     * SHADOWING-IMPORTed into a package (CLHS 11.1.1.2.4 — a legal and
     * common thing to do with a MAKE-SYMBOL result) used to land in bucket
     * 0 and be invisible to FIND-SYMBOL forever.  A wide-string name has
     * no byte-based hash (package names are byte strings); it keeps the
     * old behaviour. */
    if (CL_STRING_P(name)) {
        return cl_make_uninterned_symbol(name);
    }
    if (CL_ANY_STRING_P(name)) {
        return cl_make_symbol(name);
    }
    /* Adjustable / fill-pointer / displaced character vectors: copy the
     * active portion (length = fill-pointer when present) into a fresh
     * simple TYPE_STRING.  Per CLHS 11.1.3, MAKE-SYMBOL accepts any
     * STRING — including non-simple vectors of CHARACTER. */
    if (CL_STRING_VECTOR_P(name)) {
        CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(name);
        uint32_t active = cl_vector_active_length(v);
        CL_Obj str;
        uint32_t i;
        CL_Obj *data;
        CL_GC_PROTECT(name);
        str = cl_make_string(NULL, active);
        CL_GC_UNPROTECT(1);
        v = (CL_Vector *)CL_OBJ_TO_PTR(name);
        data = cl_vector_data(v);
        for (i = 0; i < active; i++) {
            CL_Obj ch = data[i];
            cl_string_set_char_at(str, i, CL_CHAR_P(ch) ? CL_CHAR_VAL(ch) : 0);
        }
        return CL_STRING_P(str) ? cl_make_uninterned_symbol(str) : cl_make_symbol(str);
    }
    cl_signal_type_error(args[0], "STRING", "MAKE-SYMBOL");
    return CL_NIL;
}

static CL_Obj bi_keywordp(CL_Obj *args, int n)
{
    CL_Symbol *s;
    CL_UNUSED(n);
    if (CL_NULL_P(args[0])) return CL_NIL;
    if (!CL_SYMBOL_P(args[0])) return CL_NIL;
    s = (CL_Symbol *)CL_OBJ_TO_PTR(args[0]);
    return (s->package == cl_package_keyword) ? SYM_T : CL_NIL;
}

/* --- String functions --- */

/* Coerce a string designator (string, symbol) to a CL_Obj string.
 * Returns the string object (TYPE_STRING or TYPE_WIDE_STRING).
 * For symbols, returns the symbol's name string.
 * Sets *out_len to string length.
 * Returns CL_NIL if not a valid string designator. */
/* Thread-local scratch buffer for character-as-string-designator.
 * Only one active at a time per string comparison call. */
static CL_Obj char_desig_str = CL_NIL;

static CL_Obj string_designator_to_obj(CL_Obj obj, uint32_t *out_len)
{
    if (CL_ANY_STRING_P(obj)) {
        *out_len = cl_string_length(obj);
        return obj;
    }
    /* Adjustable / fill-pointer character vector — a valid CL string.  The
     * read accessors (cl_string_length / cl_string_char_at) are vector-aware,
     * so return it as-is; no allocation, no GC hazard for the two-arg
     * comparison callers. */
    if (CL_STRING_VECTOR_P(obj)) {
        *out_len = cl_string_length(obj);
        return obj;
    }
    if (CL_NULL_P(obj)) {
        CL_Symbol *sym = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_NIL);
        *out_len = cl_string_length(sym->name);
        return sym->name;
    }
    if (CL_SYMBOL_P(obj)) {
        CL_Symbol *sym = (CL_Symbol *)CL_OBJ_TO_PTR(obj);
        *out_len = cl_string_length(sym->name);
        return sym->name;
    }
    if (CL_CHAR_P(obj)) {
        char buf[2];
        buf[0] = (char)CL_CHAR_VAL(obj);
        buf[1] = '\0';
        char_desig_str = cl_make_string(buf, 1);
        *out_len = 1;
        return char_desig_str;
    }
    return CL_NIL;
}

/* Coerce both comparison arguments.  GC SAFETY: coercing the SECOND
 * designator can allocate (a character designator conses a fresh 1-char
 * string) and compact, staling an already-coerced first string — so `a`
 * is rooted across the second coercion. */
static void string_designators_2(CL_Obj *args, CL_Obj *a_out, uint32_t *la,
                                 CL_Obj *b_out, uint32_t *lb)
{
    CL_Obj a = string_designator_to_obj(args[0], la);
    CL_Obj b;
    CL_GC_PROTECT(a);
    b = string_designator_to_obj(args[1], lb);
    CL_GC_UNPROTECT(1);
    *a_out = a;
    *b_out = b;
}

/* Compare characters of two strings in subranges.
 * Returns <0, 0, or >0 like memcmp. */
static int string_compare_range(CL_Obj a, uint32_t a_start,
                                CL_Obj b, uint32_t b_start, uint32_t len)
{
    uint32_t i;
    for (i = 0; i < len; i++) {
        int ca = cl_string_char_at(a, a_start + i);
        int cb = cl_string_char_at(b, b_start + i);
        if (ca != cb) return ca - cb;
    }
    return 0;
}

/* Parse :start1 :end1 :start2 :end2 keyword args for string comparison.
 * args[0] and args[1] are the two strings; args[2..n-1] are keyword pairs.
 * Sets s1,e1,s2,e2 to the substring bounds. */
static void parse_string_bounds(CL_Obj *args, int n,
    uint32_t la, uint32_t lb,
    uint32_t *s1, uint32_t *e1, uint32_t *s2, uint32_t *e2)
{
    int i;
    *s1 = 0; *e1 = la; *s2 = 0; *e2 = lb;
    for (i = 2; i + 1 < n; i += 2) {
        CL_Obj key = args[i];
        int32_t val = CL_FIXNUM_P(args[i + 1]) ? CL_FIXNUM_VAL(args[i + 1]) : 0;
        if (CL_NULL_P(args[i + 1])) continue; /* NIL means default */
        if (key == cl_intern_keyword("START1", 6)) *s1 = (uint32_t)val;
        else if (key == cl_intern_keyword("END1", 4)) *e1 = (uint32_t)val;
        else if (key == cl_intern_keyword("START2", 6)) *s2 = (uint32_t)val;
        else if (key == cl_intern_keyword("END2", 4)) *e2 = (uint32_t)val;
    }
    if (*e1 > la) *e1 = la;
    if (*e2 > lb) *e2 = lb;
    if (*s1 > *e1) *s1 = *e1;
    if (*s2 > *e2) *s2 = *e2;
}

static CL_Obj bi_string_eq(CL_Obj *args, int n)
{
    uint32_t la, lb, s1, e1, s2, e2;
    CL_Obj a, b;
    string_designators_2(args, &a, &la, &b, &lb);
    if (CL_NULL_P(a) || CL_NULL_P(b)) cl_error(CL_ERR_TYPE, "STRING=: not a string designator");
    parse_string_bounds(args, n, la, lb, &s1, &e1, &s2, &e2);
    if ((e1 - s1) != (e2 - s2)) return CL_NIL;
    return (string_compare_range(a, s1, b, s2, e1 - s1) == 0) ? SYM_T : CL_NIL;
}

static CL_Obj bi_string_equal(CL_Obj *args, int n)
{
    uint32_t la, lb, s1, e1, s2, e2, i;
    CL_Obj a, b;
    string_designators_2(args, &a, &la, &b, &lb);
    if (CL_NULL_P(a) || CL_NULL_P(b)) cl_error(CL_ERR_TYPE, "STRING-EQUAL: not a string designator");
    parse_string_bounds(args, n, la, lb, &s1, &e1, &s2, &e2);
    if ((e1 - s1) != (e2 - s2)) return CL_NIL;
    for (i = 0; i < (e1 - s1); i++) {
        int ca = cl_string_char_at(a, s1 + i);
        int cb = cl_string_char_at(b, s2 + i);
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return CL_NIL;
    }
    return SYM_T;
}

/* Case-insensitive string comparison (for STRING-LESSP etc.) */
static int string_compare_range_ci(CL_Obj a, uint32_t a_start,
                                   CL_Obj b, uint32_t b_start, uint32_t len)
{
    uint32_t i;
    for (i = 0; i < len; i++) {
        int ca = cl_string_char_at(a, a_start + i);
        int cb = cl_string_char_at(b, b_start + i);
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return ca - cb;
    }
    return 0;
}

/* Comparison ops for string_cmp_op */
#define STR_CMP_LT  0  /* cmp < 0  || (cmp == 0 && len1 <  len2) */
#define STR_CMP_GT  1  /* cmp > 0  || (cmp == 0 && len1 >  len2) */
#define STR_CMP_LE  2  /* cmp < 0  || (cmp == 0 && len1 <= len2) */
#define STR_CMP_GE  3  /* cmp > 0  || (cmp == 0 && len1 >= len2) */

typedef int (*str_cmp_fn)(CL_Obj, uint32_t, CL_Obj, uint32_t, uint32_t);

static CL_Obj string_cmp_op(CL_Obj *args, int n, int op, str_cmp_fn cmp_fn,
                             const char *name)
{
    uint32_t la, lb, s1, e1, s2, e2, len1, len2, min_len;
    CL_Obj a, b;
    int cmp;
    string_designators_2(args, &a, &la, &b, &lb);
    if (CL_NULL_P(a) || CL_NULL_P(b)) cl_error(CL_ERR_TYPE, "%s: not a string designator", name);
    parse_string_bounds(args, n, la, lb, &s1, &e1, &s2, &e2);
    len1 = e1 - s1; len2 = e2 - s2;
    min_len = len1 < len2 ? len1 : len2;
    cmp = cmp_fn(a, s1, b, s2, min_len);
    switch (op) {
    case STR_CMP_LT: return (cmp < 0 || (cmp == 0 && len1 <  len2)) ? SYM_T : CL_NIL;
    case STR_CMP_GT: return (cmp > 0 || (cmp == 0 && len1 >  len2)) ? SYM_T : CL_NIL;
    case STR_CMP_LE: return (cmp < 0 || (cmp == 0 && len1 <= len2)) ? SYM_T : CL_NIL;
    case STR_CMP_GE: return (cmp > 0 || (cmp == 0 && len1 >= len2)) ? SYM_T : CL_NIL;
    default: return CL_NIL;
    }
}

/* Case-sensitive: STRING<, STRING>, STRING<=, STRING>= */
static CL_Obj bi_string_lt(CL_Obj *a, int n) { return string_cmp_op(a, n, STR_CMP_LT, string_compare_range, "STRING<"); }
static CL_Obj bi_string_gt(CL_Obj *a, int n) { return string_cmp_op(a, n, STR_CMP_GT, string_compare_range, "STRING>"); }
static CL_Obj bi_string_le(CL_Obj *a, int n) { return string_cmp_op(a, n, STR_CMP_LE, string_compare_range, "STRING<="); }
static CL_Obj bi_string_ge(CL_Obj *a, int n) { return string_cmp_op(a, n, STR_CMP_GE, string_compare_range, "STRING>="); }

/* Case-insensitive: STRING-LESSP, STRING-GREATERP, STRING-NOT-GREATERP, STRING-NOT-LESSP */
static CL_Obj bi_string_lessp(CL_Obj *a, int n) { return string_cmp_op(a, n, STR_CMP_LT, string_compare_range_ci, "STRING-LESSP"); }
static CL_Obj bi_string_greaterp(CL_Obj *a, int n) { return string_cmp_op(a, n, STR_CMP_GT, string_compare_range_ci, "STRING-GREATERP"); }
static CL_Obj bi_string_not_greaterp(CL_Obj *a, int n) { return string_cmp_op(a, n, STR_CMP_LE, string_compare_range_ci, "STRING-NOT-GREATERP"); }
static CL_Obj bi_string_not_lessp(CL_Obj *a, int n) { return string_cmp_op(a, n, STR_CMP_GE, string_compare_range_ci, "STRING-NOT-LESSP"); }

static CL_Obj bi_string_neq(CL_Obj *args, int n)
{
    /* STRING/= — case-sensitive not-equal */
    return CL_NULL_P(bi_string_eq(args, n)) ? SYM_T : CL_NIL;
}

static CL_Obj bi_string_not_equal(CL_Obj *args, int n)
{
    /* STRING-NOT-EQUAL — case-insensitive not-equal */
    return CL_NULL_P(bi_string_equal(args, n)) ? SYM_T : CL_NIL;
}

/* Coerce a string designator (string, symbol, or character) to CL_Obj string.
 * Per CL spec, string designators are accepted by string functions. */
static CL_Obj coerce_to_string_obj(CL_Obj obj, const char *func_name)
{
    if (CL_ANY_STRING_P(obj))
        return obj;
    /* Adjustable / fill-pointer character vector — a valid CL string.
     * Returned as-is; callers (string-upcase/-trim/...) copy it via
     * cl_string_copy, which materializes a fresh simple string. */
    if (CL_STRING_VECTOR_P(obj))
        return obj;
    if (CL_NULL_P(obj)) {
        CL_Symbol *sym = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_NIL);
        return sym->name;
    }
    if (CL_SYMBOL_P(obj)) {
        CL_Symbol *sym = (CL_Symbol *)CL_OBJ_TO_PTR(obj);
        return sym->name;
    }
    if (CL_CHAR_P(obj)) {
        char buf[2];
        buf[0] = (char)CL_CHAR_VAL(obj);
        buf[1] = '\0';
        return cl_make_string(buf, 1);
    }
    (void)func_name;
    cl_error(CL_ERR_TYPE, "not a string designator");
    return CL_NIL;
}

/* Parse the trailing :START/:END keyword args shared by the case-conversion
 * family (STRING-UPCASE/-DOWNCASE/-CAPITALIZE and NSTRING-* — CLHS 21.2).
 * args[1..n-1] are alternating keyword/value pairs; first occurrence of a
 * keyword wins and :ALLOW-OTHER-KEYS T admits unknown keywords, matching
 * bi_parse_integer.  Validates 0 <= start <= end <= len and signals a
 * descriptive error otherwise; :END NIL means end of string. */
static void parse_case_bounds(const char *func_name, CL_Obj *args, int n,
                              uint32_t len, uint32_t *start_out,
                              uint32_t *end_out)
{
    int32_t start = 0, end = (int32_t)len;
    int k, seen_start = 0, seen_end = 0, allow_other = 0;

    *start_out = 0;
    *end_out = len;
    if (n <= 1)
        return;
    if ((n - 1) & 1)
        cl_error(CL_ERR_ARGS, "%s: odd number of keyword arguments",
                 func_name);
    /* First pass: detect :ALLOW-OTHER-KEYS T so unknown keys don't error. */
    for (k = 1; k + 1 < n; k += 2) {
        if (!CL_SYMBOL_P(args[k]))
            cl_error(CL_ERR_ARGS, "%s: keyword arg not a symbol", func_name);
        if (strcmp(cl_symbol_name(args[k]), "ALLOW-OTHER-KEYS") == 0
            && !allow_other)
            allow_other = !CL_NULL_P(args[k + 1]);
    }
    for (k = 1; k + 1 < n; k += 2) {
        const char *kn = cl_symbol_name(args[k]);
        CL_Obj val = args[k + 1];
        if (strcmp(kn, "START") == 0) {
            if (seen_start) continue;
            seen_start = 1;
            if (!CL_FIXNUM_P(val))
                cl_signal_type_error(val, "INTEGER", func_name);
            start = CL_FIXNUM_VAL(val);
        } else if (strcmp(kn, "END") == 0) {
            if (seen_end) continue;
            seen_end = 1;
            if (CL_NULL_P(val)) continue;   /* :END NIL = string length */
            if (!CL_FIXNUM_P(val))
                cl_signal_type_error(val, "(OR NULL INTEGER)", func_name);
            end = CL_FIXNUM_VAL(val);
        } else if (strcmp(kn, "ALLOW-OTHER-KEYS") == 0) {
            /* consumed in first pass */
        } else if (!allow_other) {
            cl_error(CL_ERR_ARGS, "%s: unknown keyword :%s", func_name, kn);
        }
    }
    if (start < 0 || start > end || end > (int32_t)len)
        cl_error(CL_ERR_ARGS,
                 "%s: bad bounding indices :START %ld :END %ld for a string "
                 "of length %lu",
                 func_name, (long)start, (long)end, (unsigned long)len);
    *start_out = (uint32_t)start;
    *end_out = (uint32_t)end;
}

static CL_Obj bi_string_upcase(CL_Obj *args, int n)
{
    CL_Obj str, result;
    uint32_t i, start, end;
    str = coerce_to_string_obj(args[0], "STRING-UPCASE");
    result = cl_string_copy(str);
    parse_case_bounds("STRING-UPCASE", args, n,
                      cl_string_length(result), &start, &end);
    for (i = start; i < end; i++) {
        int ch = cl_string_char_at(result, i);
        int up = cl_toupper(ch);
        if (up != ch)
            cl_string_set_char_at(result, i, up);
    }
    return result;
}

static CL_Obj bi_string_downcase(CL_Obj *args, int n)
{
    CL_Obj str, result;
    uint32_t i, start, end;
    str = coerce_to_string_obj(args[0], "STRING-DOWNCASE");
    result = cl_string_copy(str);
    parse_case_bounds("STRING-DOWNCASE", args, n,
                      cl_string_length(result), &start, &end);
    for (i = start; i < end; i++) {
        int ch = cl_string_char_at(result, i);
        int lo = cl_tolower(ch);
        if (lo != ch)
            cl_string_set_char_at(result, i, lo);
    }
    return result;
}

/* Check if character ch is in the char-bag (a sequence of characters).
 * Per CL spec, char-bag can be a string, list, or vector of characters. */
static int trim_char_in_set(int ch, CL_Obj set)
{
    if (CL_ANY_STRING_P(set)) {
        uint32_t i, len = cl_string_length(set);
        for (i = 0; i < len; i++)
            if (cl_string_char_at(set, i) == ch) return 1;
        return 0;
    }
    if (CL_CONS_P(set) || CL_NULL_P(set)) {
        CL_Obj cursor = set;
        while (CL_CONS_P(cursor)) {
            CL_Obj elt = cl_car(cursor);
            if (CL_CHAR_P(elt) && CL_CHAR_VAL(elt) == (uint32_t)ch) return 1;
            cursor = cl_cdr(cursor);
        }
        return 0;
    }
    if (CL_VECTOR_P(set)) {
        CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(set);
        uint32_t i, len = cl_vector_active_length(v);
        CL_Obj *data = cl_vector_data(v);
        for (i = 0; i < len; i++) {
            if (CL_CHAR_P(data[i]) && CL_CHAR_VAL(data[i]) == (uint32_t)ch) return 1;
        }
        return 0;
    }
    cl_error(CL_ERR_TYPE, "STRING-TRIM: character bag must be a sequence");
    return 0;
}

static CL_Obj bi_string_trim(CL_Obj *args, int n)
{
    CL_Obj set, str;
    uint32_t start, end;
    CL_UNUSED(n);
    /* GC SAFETY (audit tier 4, FS10): coerce_to_string_obj allocates for a
     * character/symbol designator — read the char-bag from the rooted
     * args[0] slot AFTER the coerce, not before. */
    str = coerce_to_string_obj(args[1], "STRING-TRIM");
    set = args[0];
    start = 0;
    end = cl_string_length(str);
    while (start < end && trim_char_in_set(cl_string_char_at(str, start), set)) start++;
    while (end > start && trim_char_in_set(cl_string_char_at(str, end - 1), set)) end--;
    return cl_string_substring(str, start, end);
}

static CL_Obj bi_string_left_trim(CL_Obj *args, int n)
{
    CL_Obj set, str;
    uint32_t start, len;
    CL_UNUSED(n);
    /* GC SAFETY (audit tier 4, FS10): re-read set after the allocating coerce */
    str = coerce_to_string_obj(args[1], "STRING-LEFT-TRIM");
    set = args[0];
    start = 0;
    len = cl_string_length(str);
    while (start < len && trim_char_in_set(cl_string_char_at(str, start), set)) start++;
    return cl_string_substring(str, start, len);
}

static CL_Obj bi_string_right_trim(CL_Obj *args, int n)
{
    CL_Obj set, str;
    uint32_t end;
    CL_UNUSED(n);
    /* GC SAFETY (audit tier 4, FS10): re-read set after the allocating coerce */
    str = coerce_to_string_obj(args[1], "STRING-RIGHT-TRIM");
    set = args[0];
    end = cl_string_length(str);
    while (end > 0 && trim_char_in_set(cl_string_char_at(str, end - 1), set)) end--;
    return cl_string_substring(str, 0, end);
}

static CL_Obj bi_subseq(CL_Obj *args, int n)
{
    int32_t start, end;
    if (!CL_FIXNUM_P(args[1]))
        cl_error(CL_ERR_TYPE, "SUBSEQ: start must be an integer");
    start = CL_FIXNUM_VAL(args[1]);

    if (CL_ANY_STRING_P(args[0])) {
        uint32_t slen = cl_string_length(args[0]);
        end = (n > 2 && !CL_NULL_P(args[2]) && CL_FIXNUM_P(args[2]))
            ? CL_FIXNUM_VAL(args[2]) : (int32_t)slen;
        if (start < 0) start = 0;
        if (end > (int32_t)slen) end = (int32_t)slen;
        if (start > end) start = end;
        return cl_string_substring(args[0], (uint32_t)start, (uint32_t)end);
    }
    /* String-vector subseq (adjustable / fill-pointer character vector):
     * a valid CL STRING, so SUBSEQ must return a STRING, not a general
     * vector (SUBSEQ-STRING.3 / SUBSEQ.SPECIALIZED-STRING.1). */
    if (CL_STRING_VECTOR_P(args[0])) {
        uint32_t len = cl_string_length(args[0]);
        CL_Obj result;
        int32_t i, rlen;
        end = (n > 2 && !CL_NULL_P(args[2]) && CL_FIXNUM_P(args[2]))
            ? CL_FIXNUM_VAL(args[2]) : (int32_t)len;
        if (start < 0) start = 0;
        if (end > (int32_t)len) end = (int32_t)len;
        if (start > end) start = end;
        rlen = end - start;
        /* args[0] is an already-rooted VM-stack slot: forwarded on
         * compaction without (and never with) an extra CL_GC_PROTECT. */
#ifdef CL_WIDE_STRINGS
        {
            int wide = 0;
            for (i = 0; i < rlen; i++)
                if (cl_string_char_at(args[0], (uint32_t)(start + i)) > 255) { wide = 1; break; }
            result = wide ? cl_make_wide_string(NULL, (uint32_t)rlen)
                          : cl_make_string(NULL, (uint32_t)rlen);
        }
#else
        result = cl_make_string(NULL, (uint32_t)rlen);
#endif
        for (i = 0; i < rlen; i++)
            cl_string_set_char_at(result, (uint32_t)i,
                                  cl_string_char_at(args[0], (uint32_t)(start + i)));
        return result;
    }
    /* Vector subseq */
    if (CL_VECTOR_P(args[0])) {
        CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(args[0]);
        uint32_t len = cl_vector_active_length(v);
        CL_Obj *data;
        CL_Obj result;
        int32_t i, rlen;
        end = (n > 2 && !CL_NULL_P(args[2]) && CL_FIXNUM_P(args[2]))
            ? CL_FIXNUM_VAL(args[2]) : (int32_t)len;
        if (start < 0) start = 0;
        if (end > (int32_t)len) end = (int32_t)len;
        if (start > end) start = end;
        rlen = end - start;
        result = cl_make_vector((uint32_t)rlen);
        v = (CL_Vector *)CL_OBJ_TO_PTR(args[0]);
        data = cl_vector_data(v);
        {
            CL_Vector *rv = (CL_Vector *)CL_OBJ_TO_PTR(result);
            CL_Obj *rdata = cl_vector_data(rv);
            for (i = 0; i < rlen; i++)
                rdata[i] = data[start + i];
            /* Preserve the declared element type (serapeum VECT-TYPE / ansi
             * SUBSEQ.SPECIALIZED-VECTOR.2): a subsequence of a specialized
             * numeric vector must report the same ARRAY-ELEMENT-TYPE. */
            rv->elt_type = v->elt_type;
        }
        return result;
    }
    if (CL_BIT_VECTOR_P(args[0])) {
        CL_BitVector *bv = (CL_BitVector *)CL_OBJ_TO_PTR(args[0]);
        uint32_t len = cl_bv_active_length(bv);
        CL_Obj result;
        int32_t i, rlen;
        end = (n > 2 && !CL_NULL_P(args[2]) && CL_FIXNUM_P(args[2]))
            ? CL_FIXNUM_VAL(args[2]) : (int32_t)len;
        if (start < 0) start = 0;
        if (end > (int32_t)len) end = (int32_t)len;
        if (start > end) start = end;
        rlen = end - start;
        result = cl_make_bit_vector((uint32_t)rlen);
        bv = (CL_BitVector *)CL_OBJ_TO_PTR(args[0]);
        {
            CL_BitVector *rv = (CL_BitVector *)CL_OBJ_TO_PTR(result);
            for (i = 0; i < rlen; i++)
                cl_bv_set_bit(rv, (uint32_t)i, cl_bv_get_bit(bv, (uint32_t)(start + i)));
        }
        return result;
    }
    /* Byte-vector subseq: packed source and result — a single memcpy. */
    if (CL_BYTE_VECTOR_P(args[0])) {
        CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(args[0]);
        uint32_t len = cl_bytevec_active_length(bv);
        int is_signed = bv->is_signed;
        int elt_shift = bv->elt_shift;
        CL_Obj result;
        int32_t rlen;
        end = (n > 2 && !CL_NULL_P(args[2]) && CL_FIXNUM_P(args[2]))
            ? CL_FIXNUM_VAL(args[2]) : (int32_t)len;
        if (start < 0) start = 0;
        if (end > (int32_t)len) end = (int32_t)len;
        if (start > end) start = end;
        rlen = end - start;
        result = cl_make_byte_vector((uint32_t)rlen, is_signed, elt_shift);
        bv = (CL_ByteVector *)CL_OBJ_TO_PTR(args[0]);  /* re-fetch after alloc */
        memcpy(((CL_ByteVector *)CL_OBJ_TO_PTR(result))->data,
               bv->data + ((size_t)start << elt_shift),
               (size_t)rlen << elt_shift);
        return result;
    }
    /* List subseq */
    {
        CL_Obj list = args[0], result = CL_NIL, tail = CL_NIL;
        int32_t i = 0;
        /* `list` is the source cursor: it starts at args[0] (a VM-stack root)
         * but is immediately reassigned by cl_cdr, so from the first step it
         * lives ONLY in this C local — no longer aliased by any root.  The
         * cl_cons below can trigger a compacting GC (single-thread, or a peer
         * thread's stop-the-world compaction while we are parked at cl_alloc's
         * safepoint), which relocates the source list and would leave `list`
         * a stale offset.  Protect it alongside result/tail. */
        CL_GC_PROTECT(list);
        CL_GC_PROTECT(result);
        CL_GC_PROTECT(tail);
        end = (n > 2 && !CL_NULL_P(args[2]) && CL_FIXNUM_P(args[2]))
            ? CL_FIXNUM_VAL(args[2]) : 0x7FFFFFFF;
        while (!CL_NULL_P(list) && i < end) {
            if (i >= start) {
                CL_Obj cell = cl_cons(cl_car(list), CL_NIL);
                if (CL_NULL_P(result)) result = cell;
                else ((CL_Cons *)CL_OBJ_TO_PTR(tail))->cdr = cell;
                tail = cell;
            }
            list = cl_cdr(list);
            i++;
        }
        CL_GC_UNPROTECT(3);
        return result;
    }
}

/* (setf (subseq seq start end) new-seq)
 * → (%setf-subseq new-seq seq start &optional end)
 * Per CLHS: copies min(end-start, length(new-seq)) elements from
 * new-seq into seq[start..end).  Returns new-seq. */
static CL_Obj bi_setf_subseq(CL_Obj *args, int n)
{
    CL_Obj new_seq = args[0], seq = args[1];
    int32_t start, end, seq_len, new_len, copy_len, i;

    if (!CL_FIXNUM_P(args[2]))
        cl_error(CL_ERR_TYPE, "(SETF SUBSEQ): start must be an integer");
    start = CL_FIXNUM_VAL(args[2]);

    /* Get sequence length */
    if (CL_ANY_STRING_P(seq))
        seq_len = (int32_t)cl_string_length(seq);
    else if (CL_VECTOR_P(seq)) {
        CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(seq);
        seq_len = (int32_t)cl_vector_active_length(v);
    } else if (CL_BYTE_VECTOR_P(seq)) {
        CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(seq);
        seq_len = (int32_t)cl_bytevec_active_length(bv);
    } else if (CL_CONS_P(seq) || CL_NULL_P(seq)) {
        int32_t cnt = 0;
        CL_Obj tmp = seq;
        while (!CL_NULL_P(tmp)) { cnt++; tmp = cl_cdr(tmp); }
        seq_len = cnt;
    } else {
        cl_error(CL_ERR_TYPE, "(SETF SUBSEQ): not a sequence");
        return new_seq;
    }

    end = (n > 3 && !CL_NULL_P(args[3]) && CL_FIXNUM_P(args[3]))
        ? CL_FIXNUM_VAL(args[3]) : seq_len;
    if (start < 0) start = 0;
    if (end > seq_len) end = seq_len;
    if (start > end) start = end;

    /* Get new-seq length */
    if (CL_ANY_STRING_P(new_seq))
        new_len = (int32_t)cl_string_length(new_seq);
    else if (CL_VECTOR_P(new_seq)) {
        CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(new_seq);
        new_len = (int32_t)cl_vector_active_length(v);
    } else if (CL_BYTE_VECTOR_P(new_seq)) {
        CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(new_seq);
        new_len = (int32_t)cl_bytevec_active_length(bv);
    } else if (CL_CONS_P(new_seq) || CL_NULL_P(new_seq)) {
        int32_t cnt = 0;
        CL_Obj tmp = new_seq;
        while (!CL_NULL_P(tmp)) { cnt++; tmp = cl_cdr(tmp); }
        new_len = cnt;
    } else {
        cl_error(CL_ERR_TYPE, "(SETF SUBSEQ): new value is not a sequence");
        return new_seq;
    }

    copy_len = (end - start < new_len) ? (end - start) : new_len;

    /* String target */
    if (CL_ANY_STRING_P(seq)) {
        for (i = 0; i < copy_len; i++) {
            int ch;
            if (CL_ANY_STRING_P(new_seq))
                ch = cl_string_char_at(new_seq, (uint32_t)i);
            else if (CL_VECTOR_P(new_seq)) {
                CL_Obj elem = cl_vector_data((CL_Vector *)CL_OBJ_TO_PTR(new_seq))[i];
                ch = CL_CHAR_P(elem) ? CL_CHAR_VAL(elem) : 0;
            } else {
                /* list */
                CL_Obj elem = CL_NIL;
                CL_Obj tmp = new_seq;
                int32_t j;
                for (j = 0; j < i && !CL_NULL_P(tmp); j++) tmp = cl_cdr(tmp);
                elem = CL_NULL_P(tmp) ? CL_NIL : cl_car(tmp);
                ch = CL_CHAR_P(elem) ? CL_CHAR_VAL(elem) : 0;
            }
            cl_string_set_char_at(seq, (uint32_t)(start + i), ch);
        }
        return new_seq;
    }

    /* Vector target */
    if (CL_VECTOR_P(seq)) {
        CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(seq);
        CL_Obj *data = cl_vector_data(v);
        for (i = 0; i < copy_len; i++) {
            CL_Obj elem;
            if (CL_ANY_STRING_P(new_seq))
                elem = CL_MAKE_CHAR(cl_string_char_at(new_seq, (uint32_t)i));
            else if (CL_VECTOR_P(new_seq))
                elem = cl_vector_data((CL_Vector *)CL_OBJ_TO_PTR(new_seq))[i];
            else if (CL_BYTE_VECTOR_P(new_seq))
                elem = CL_MAKE_FIXNUM(cl_bytevec_get(
                    (CL_ByteVector *)CL_OBJ_TO_PTR(new_seq), (uint32_t)i));
            else {
                CL_Obj tmp = new_seq;
                int32_t j;
                for (j = 0; j < i && !CL_NULL_P(tmp); j++) tmp = cl_cdr(tmp);
                elem = CL_NULL_P(tmp) ? CL_NIL : cl_car(tmp);
            }
            data[start + i] = elem;
        }
        return new_seq;
    }

    /* Byte-vector target: fast raw copy when the source is a byte vector of
     * the same element width, else element-wise with range checks. */
    if (CL_BYTE_VECTOR_P(seq)) {
        CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(seq);
        if (CL_BYTE_VECTOR_P(new_seq) &&
            ((CL_ByteVector *)CL_OBJ_TO_PTR(new_seq))->elt_shift ==
                bv->elt_shift) {
            CL_ByteVector *src = (CL_ByteVector *)CL_OBJ_TO_PTR(new_seq);
            if (copy_len > 0)
                memcpy(bv->data + ((size_t)start << bv->elt_shift), src->data,
                       (size_t)copy_len << bv->elt_shift);
            return new_seq;
        }
        for (i = 0; i < copy_len; i++) {
            CL_Obj elem;
            if (CL_VECTOR_P(new_seq))
                elem = cl_vector_data((CL_Vector *)CL_OBJ_TO_PTR(new_seq))[i];
            else if (CL_BYTE_VECTOR_P(new_seq))
                /* Different element width — go element-wise with checks. */
                elem = CL_MAKE_FIXNUM(cl_bytevec_get(
                    (CL_ByteVector *)CL_OBJ_TO_PTR(new_seq), (uint32_t)i));
            else {
                CL_Obj tmp = new_seq;
                int32_t j;
                for (j = 0; j < i && !CL_NULL_P(tmp); j++) tmp = cl_cdr(tmp);
                elem = CL_NULL_P(tmp) ? CL_NIL : cl_car(tmp);
            }
            cl_bytevec_set(bv, start + i,
                           cl_bytevec_check_value(elem, bv->is_signed,
                                                  bv->elt_shift,
                                                  "(SETF SUBSEQ) on a byte vector"));
        }
        return new_seq;
    }

    /* List target */
    if (CL_CONS_P(seq)) {
        CL_Obj cell = seq;
        int32_t j;
        /* Skip to start */
        for (j = 0; j < start && !CL_NULL_P(cell); j++)
            cell = cl_cdr(cell);
        for (i = 0; i < copy_len && !CL_NULL_P(cell); i++) {
            CL_Obj elem;
            if (CL_ANY_STRING_P(new_seq))
                elem = CL_MAKE_CHAR(cl_string_char_at(new_seq, (uint32_t)i));
            else if (CL_VECTOR_P(new_seq))
                elem = cl_vector_data((CL_Vector *)CL_OBJ_TO_PTR(new_seq))[i];
            else {
                CL_Obj tmp = new_seq;
                int32_t k;
                for (k = 0; k < i && !CL_NULL_P(tmp); k++) tmp = cl_cdr(tmp);
                elem = CL_NULL_P(tmp) ? CL_NIL : cl_car(tmp);
            }
            ((CL_Cons *)CL_OBJ_TO_PTR(cell))->car = elem;
            cell = cl_cdr(cell);
        }
        return new_seq;
    }

    return new_seq;
}

/* Helper: count total elements across sequences args[1..n-1] */
static uint32_t concat_total_length(CL_Obj *args, int n)
{
    uint32_t total = 0;
    int i;
    for (i = 1; i < n; i++) {
        if (CL_ANY_STRING_P(args[i])) {
            total += cl_string_length(args[i]);
        } else if (CL_VECTOR_P(args[i])) {
            CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(args[i]);
            total += cl_vector_active_length(v);
        } else if (CL_NULL_P(args[i])) {
            /* empty */
        } else if (CL_CONS_P(args[i])) {
            CL_Obj p = args[i];
            while (CL_CONS_P(p)) { total++; p = cl_cdr(p); }
        } else if (CL_BIT_VECTOR_P(args[i])) {
            CL_BitVector *bv = (CL_BitVector *)CL_OBJ_TO_PTR(args[i]);
            total += cl_bv_active_length(bv);
        } else if (CL_BYTE_VECTOR_P(args[i])) {
            CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(args[i]);
            total += cl_bytevec_active_length(bv);
        } else {
            cl_error(CL_ERR_TYPE, "CONCATENATE: argument is not a sequence");
        }
    }
    return total;
}

/* Helper: iterate over sequence seq, calling cb(elem, ctx) for each element */
typedef void (*concat_cb)(CL_Obj elem, void *ctx);

static void concat_iterate(CL_Obj seq, concat_cb cb, void *ctx)
{
    /* GC SAFETY: the callback can allocate (the LIST result conses per
     * element) and compact — protect seq, re-derive object pointers per
     * element instead of caching them, and root the list cursor. */
    CL_GC_PROTECT(seq);
    if (CL_ANY_STRING_P(seq)) {
        uint32_t slen = cl_string_length(seq), j;
        for (j = 0; j < slen; j++)
            cb(CL_MAKE_CHAR(cl_string_char_at(seq, j)), ctx);
    } else if (CL_VECTOR_P(seq)) {
        uint32_t len = cl_vector_active_length((CL_Vector *)CL_OBJ_TO_PTR(seq));
        uint32_t j;
        for (j = 0; j < len; j++)
            cb(cl_vector_data((CL_Vector *)CL_OBJ_TO_PTR(seq))[j], ctx);
    } else if (CL_NULL_P(seq)) {
        /* empty */
    } else if (CL_CONS_P(seq)) {
        CL_Obj p = seq;
        CL_GC_PROTECT(p);
        while (CL_CONS_P(p)) {
            cb(cl_car(p), ctx);
            p = cl_cdr(p);
        }
        CL_GC_UNPROTECT(1);
    } else if (CL_BIT_VECTOR_P(seq)) {
        uint32_t alen =
            cl_bv_active_length((CL_BitVector *)CL_OBJ_TO_PTR(seq));
        uint32_t j;
        for (j = 0; j < alen; j++)
            cb(CL_MAKE_FIXNUM(
                   cl_bv_get_bit((CL_BitVector *)CL_OBJ_TO_PTR(seq), j)), ctx);
    } else if (CL_BYTE_VECTOR_P(seq)) {
        uint32_t alen =
            cl_bytevec_active_length((CL_ByteVector *)CL_OBJ_TO_PTR(seq));
        uint32_t j;
        for (j = 0; j < alen; j++)
            cb(CL_MAKE_FIXNUM(
                   cl_bytevec_get((CL_ByteVector *)CL_OBJ_TO_PTR(seq), j)), ctx);
    } else {
        CL_GC_UNPROTECT(1);
        cl_error(CL_ERR_TYPE, "CONCATENATE: argument is not a sequence");
        return;
    }
    CL_GC_UNPROTECT(1);
}

/* Callback context for vector result */
typedef struct { CL_Obj vec; uint32_t pos; } concat_vec_ctx;

/* String result: stage the characters into a GC vector (shared concat_vec_ctx
 * shape).  A fixed C buffer silently truncated results past 4096 chars and,
 * in wide builds, put a ~20KB frame on the (64K Amiga) C stack. */
static void concat_str_vec_cb(CL_Obj elem, void *ctx_)
{
    concat_vec_ctx *ctx = (concat_vec_ctx *)ctx_;
    CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(ctx->vec);
    if (!CL_CHAR_P(elem))
        cl_error(CL_ERR_TYPE,
                 "CONCATENATE: element is not a character for string result");
    if (ctx->pos < cl_vector_active_length(v))
        cl_vector_data(v)[ctx->pos++] = elem;
}
static void concat_vec_cb(CL_Obj elem, void *ctx_)
{
    concat_vec_ctx *ctx = (concat_vec_ctx *)ctx_;
    CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(ctx->vec);
    if (ctx->pos < cl_vector_active_length(v))
        cl_vector_data(v)[ctx->pos++] = elem;
}

/* Callback context for bit-vector result */
typedef struct { CL_Obj bv; uint32_t pos; } concat_bv_ctx;
static void concat_bv_cb(CL_Obj elem, void *ctx_)
{
    concat_bv_ctx *ctx = (concat_bv_ctx *)ctx_;
    CL_BitVector *bv = (CL_BitVector *)CL_OBJ_TO_PTR(ctx->bv);
    if (ctx->pos < cl_bv_active_length(bv)) {
        uint32_t p = ctx->pos;  /* cl_bv_set_bit evaluates its index twice */
        if (!CL_FIXNUM_P(elem) ||
            (CL_FIXNUM_VAL(elem) != 0 && CL_FIXNUM_VAL(elem) != 1))
            cl_error(CL_ERR_TYPE, "CONCATENATE: element is not a bit for bit-vector result");
        cl_bv_set_bit(bv, p, CL_FIXNUM_VAL(elem) != 0);
        ctx->pos = p + 1;
    }
}

/* Callback context for list result */
typedef struct { CL_Obj head; CL_Obj tail; } concat_list_ctx;
static void concat_list_cb(CL_Obj elem, void *ctx_)
{
    concat_list_ctx *ctx = (concat_list_ctx *)ctx_;
    CL_Obj cell = cl_cons(elem, CL_NIL);
    if (CL_NULL_P(ctx->head)) {
        ctx->head = cell;
        ctx->tail = cell;
    } else {
        ((CL_Cons *)CL_OBJ_TO_PTR(ctx->tail))->cdr = cell;
        ctx->tail = cell;
    }
}

/* Callback context for a packed byte-vector result. */
typedef struct { CL_Obj bv; uint32_t pos; int is_signed; int elt_shift; } concat_bytevec_ctx;
static void concat_bytevec_cb(CL_Obj elem, void *ctx_)
{
    concat_bytevec_ctx *ctx = (concat_bytevec_ctx *)ctx_;
    CL_ByteVector *bv;
    int32_t v = cl_bytevec_check_value(elem, ctx->is_signed, ctx->elt_shift,
                                       "CONCATENATE");
    /* Re-derive after the check — its error path can allocate/compact. */
    bv = (CL_ByteVector *)CL_OBJ_TO_PTR(ctx->bv);
    if (ctx->pos < cl_bytevec_active_length(bv)) {
        cl_bytevec_set(bv, ctx->pos, v);
        ctx->pos++;
    }
}

static CL_Obj bi_concatenate(CL_Obj *args, int n)
{
    CL_Obj result_type = args[0];
    const char *tname;
    int i;
    int32_t len_con = -1;  /* declared length constraint, or -1 if none */
    int bv_signed = -1;    /* >=0: build a packed byte vector (0=unsigned, 1=signed) */
    int bv_shift = 0;      /* packed element width: 0 = 8-bit, 1 = 16-bit */

    if (CL_NULL_P(result_type))
        cl_error(CL_ERR_TYPE, "CONCATENATE: result type must not be NIL");

    /* Expand user deftypes while the result-type is a bare symbol, so e.g.
     * babel's UNICODE-STRING -> (vector unicode-char *) is understood. */
    {
        extern CL_Obj cl_get_type_expander(CL_Obj name);
        int guard = 16;
        while (guard-- > 0 && CL_SYMBOL_P(result_type)) {
            CL_Obj ex = cl_get_type_expander(result_type);
            if (CL_NULL_P(ex)) break;
            result_type = cl_vm_apply(ex, NULL, 0);
        }
    }

    /* Normalize a compound type specifier — e.g. ironclad's
     * (simple-array (unsigned-byte 8) (*)) or (vector character) — down to a
     * base STRING/VECTOR result type.  A character element type means a
     * string; any other vector/array type is built as a general vector. */
    if (CL_CONS_P(result_type)) {
        extern CL_Obj cl_package_cl;
        CL_Obj head = cl_car(result_type);
        if (CL_SYMBOL_P(head)) {
            const char *hname = cl_symbol_name(head);
            CL_Obj rest = cl_cdr(result_type);
            if (strcmp(hname, "SIMPLE-ARRAY") == 0 || strcmp(hname, "ARRAY") == 0 ||
                strcmp(hname, "VECTOR") == 0 || strcmp(hname, "SIMPLE-VECTOR") == 0) {
                /* (vector elt-type [length]) — capture the length constraint. */
                CL_Obj lenarg = (CL_NULL_P(rest) || CL_NULL_P(cl_cdr(rest)))
                                    ? CL_NIL : cl_car(cl_cdr(rest));
                int is_char = 0, is_wide = 0, is_bit = 0;
                int is_u8 = 0, is_s8 = 0, is_u16 = 0, is_s16 = 0;
                if (CL_FIXNUM_P(lenarg)) len_con = CL_FIXNUM_VAL(lenarg);
                /* GC SAFETY (audit tier 4, FS11): the elt-type classifier can
                 * run a deftype expander (cl_vm_apply — compacts), e.g.
                 * (concatenate '(vector octet) …).  Keep the compound type
                 * rooted across it; rest is not reused afterwards. */
                CL_GC_PROTECT(result_type);
                /* Shared with MAKE-ARRAY/TYPEP so the result's element type
                 * agrees with what those report — a (vector (unsigned-byte 8))
                 * result must actually be the packed byte-vector kind. */
                if (!CL_NULL_P(rest))
                    cl_classify_array_elt_type(cl_car(rest), 16,
                                               &is_char, &is_wide, &is_bit,
                                               &is_u8, &is_s8, &is_u16, &is_s16);
                CL_GC_UNPROTECT(1);
                if (is_char)
                    result_type = cl_intern_in("STRING", 6, cl_package_cl);
                else if (is_bit)
                    result_type = cl_intern_in("BIT-VECTOR", 10, cl_package_cl);
                else {
                    if (is_u8 || is_s8 || is_u16 || is_s16) {
                        bv_signed = (is_s8 || is_s16) ? 1 : 0;
                        bv_shift  = (is_u16 || is_s16) ? 1 : 0;
                    }
                    result_type = cl_intern_in("VECTOR", 6, cl_package_cl);
                }
            } else {
                /* (string [length]) / (bit-vector [length]) etc. — the length,
                 * if any, is the first compound argument. */
                if (!CL_NULL_P(rest) && CL_FIXNUM_P(cl_car(rest)))
                    len_con = CL_FIXNUM_VAL(cl_car(rest));
                result_type = head;
            }
        } else {
            cl_error(CL_ERR_TYPE, "CONCATENATE: invalid result type");
        }
    }

    if (!CL_SYMBOL_P(result_type))
        cl_error(CL_ERR_TYPE, "CONCATENATE: result type must be a type specifier");
    tname = cl_symbol_name(result_type);

    /* A declared length must match the actual concatenated length. */
    if (len_con >= 0 && (uint32_t)len_con != concat_total_length(args, n))
        cl_error(CL_ERR_TYPE, "CONCATENATE: result length does not match result-type");

    /* String result types */
    if (strcmp(tname, "STRING") == 0 || strcmp(tname, "SIMPLE-STRING") == 0 ||
        strcmp(tname, "BASE-STRING") == 0 || strcmp(tname, "SIMPLE-BASE-STRING") == 0) {
        uint32_t total = concat_total_length(args, n);
        concat_vec_ctx ctx;
        CL_Obj result;
        uint32_t j;
        ctx.vec = cl_make_vector(total);
        ctx.pos = 0;
        CL_GC_PROTECT(ctx.vec);
        for (i = 1; i < n; i++)
            concat_iterate(args[i], concat_str_vec_cb, &ctx);
        {
            CL_Vector *kv = (CL_Vector *)CL_OBJ_TO_PTR(ctx.vec);
            CL_Obj *kd = cl_vector_data(kv);
#ifdef CL_WIDE_STRINGS
            int wide = 0;
            for (j = 0; j < ctx.pos; j++) {
                if (CL_CHAR_VAL(kd[j]) > 0x7F) { wide = 1; break; }
            }
            result = wide ? cl_make_wide_string(NULL, ctx.pos)
                          : cl_make_string(NULL, ctx.pos);
#else
            result = cl_make_string(NULL, ctx.pos);
#endif
            /* Re-derive after the result allocation, then fill (no
             * allocation in this loop, so kd stays valid). */
            kv = (CL_Vector *)CL_OBJ_TO_PTR(ctx.vec);
            kd = cl_vector_data(kv);
            for (j = 0; j < ctx.pos; j++)
                cl_string_set_char_at(result, j, CL_CHAR_VAL(kd[j]));
        }
        CL_GC_UNPROTECT(1);
        return result;
    }

    /* Vector result types */
    if (strcmp(tname, "VECTOR") == 0 || strcmp(tname, "SIMPLE-VECTOR") == 0) {
        uint32_t total = concat_total_length(args, n);
        concat_vec_ctx ctx;
        if (bv_signed >= 0) {
            /* Packed byte-vector result — (vector (unsigned-byte 8)) etc. */
            concat_bytevec_ctx bctx;
            bctx.bv = cl_make_byte_vector(total, bv_signed, bv_shift);
            bctx.pos = 0;
            bctx.is_signed = bv_signed;
            bctx.elt_shift = bv_shift;
            CL_GC_PROTECT(bctx.bv);
            for (i = 1; i < n; i++)
                concat_iterate(args[i], concat_bytevec_cb, &bctx);
            CL_GC_UNPROTECT(1);
            return bctx.bv;
        }
        ctx.vec = cl_make_vector(total);
        ctx.pos = 0;
        CL_GC_PROTECT(ctx.vec);
        for (i = 1; i < n; i++)
            concat_iterate(args[i], concat_vec_cb, &ctx);
        CL_GC_UNPROTECT(1);
        return ctx.vec;
    }

    /* Bit-vector result types */
    if (strcmp(tname, "BIT-VECTOR") == 0 || strcmp(tname, "SIMPLE-BIT-VECTOR") == 0) {
        uint32_t total = concat_total_length(args, n);
        concat_bv_ctx ctx;
        ctx.bv = cl_make_bit_vector(total);
        ctx.pos = 0;
        CL_GC_PROTECT(ctx.bv);
        for (i = 1; i < n; i++)
            concat_iterate(args[i], concat_bv_cb, &ctx);
        CL_GC_UNPROTECT(1);
        return ctx.bv;
    }

    /* NULL result type: only the empty concatenation is well typed. */
    if (strcmp(tname, "NULL") == 0) {
        if (concat_total_length(args, n) != 0)
            cl_error(CL_ERR_TYPE, "CONCATENATE: non-empty result for result-type NULL");
        return CL_NIL;
    }

    /* List result type */
    if (strcmp(tname, "LIST") == 0 || strcmp(tname, "CONS") == 0) {
        concat_list_ctx ctx = { CL_NIL, CL_NIL };
        CL_GC_PROTECT(ctx.head);
        CL_GC_PROTECT(ctx.tail);
        for (i = 1; i < n; i++)
            concat_iterate(args[i], concat_list_cb, &ctx);
        CL_GC_UNPROTECT(2);
        return ctx.head;
    }

    cl_error(CL_ERR_TYPE, "CONCATENATE: unsupported result type %s", tname);
    return CL_NIL;
}

static CL_Obj bi_char_accessor(CL_Obj *args, int n)
{
    int32_t idx;
    CL_UNUSED(n);
    /* Accept non-simple strings too: a fill-pointer/adjustable character vector
     * (string-vector) is a valid CL string — stringp/typep 'string/class-of all
     * report it as a string, so CHAR must work on it as well. */
    if (!CL_ANY_STRING_P(args[0]) && !CL_STRING_VECTOR_P(args[0]))
        cl_error(CL_ERR_TYPE, "CHAR: not a string");
    if (!CL_FIXNUM_P(args[1]))
        cl_error(CL_ERR_TYPE, "CHAR: index must be an integer");
    idx = CL_FIXNUM_VAL(args[1]);
    if (idx < 0 || (uint32_t)idx >= cl_string_length(args[0]))
        cl_error(CL_ERR_ARGS, "CHAR: index %d out of range", (int)idx);
    return CL_MAKE_CHAR(cl_string_char_at(args[0], (uint32_t)idx));
}

/* SCHAR requires a *simple-string* (CLHS §17.1) — no fill-pointer/adjustable vectors. */
static CL_Obj bi_schar_accessor(CL_Obj *args, int n)
{
    int32_t idx;
    CL_UNUSED(n);
    if (!CL_ANY_STRING_P(args[0]))
        cl_error(CL_ERR_TYPE, "SCHAR: not a simple-string");
    if (!CL_FIXNUM_P(args[1]))
        cl_error(CL_ERR_TYPE, "SCHAR: index must be an integer");
    idx = CL_FIXNUM_VAL(args[1]);
    if (idx < 0 || (uint32_t)idx >= cl_string_length(args[0]))
        cl_error(CL_ERR_ARGS, "SCHAR: index %d out of range", (int)idx);
    return CL_MAKE_CHAR(cl_string_char_at(args[0], (uint32_t)idx));
}

static CL_Obj bi_string_coerce(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    if (CL_ANY_STRING_P(args[0])) return args[0];
    if (CL_NULL_P(args[0])) return cl_make_string("NIL", 3);
    if (CL_SYMBOL_P(args[0])) {
        CL_Symbol *sym = (CL_Symbol *)CL_OBJ_TO_PTR(args[0]);
        return sym->name;
    }
    if (CL_CHAR_P(args[0])) {
        int code = CL_CHAR_VAL(args[0]);
#ifdef CL_WIDE_STRINGS
        if (code > 0x7F) {
            uint32_t cp = (uint32_t)code;
            return cl_make_wide_string(&cp, 1);
        }
#endif
        {
            char c = (char)code;
            return cl_make_string(&c, 1);
        }
    }
    cl_error(CL_ERR_TYPE, "STRING: cannot coerce %s to string",
             cl_type_name(args[0]));
    return CL_NIL;
}

/* CLHS whitespace[1] for PARSE-INTEGER: space, tab, newline (LF), return,
 * page (FF), and (where defined) linefeed.  Anything else (including
 * digits and signs) is not whitespace. */
static int parse_integer_is_ws(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v';
}

static CL_Obj bi_parse_integer(CL_Obj *args, int n)
{
    int32_t start = 0, end, radix = 10;
    int32_t i;
    int sign = 1, found = 0, junk_allowed = 0;
    CL_Obj radix_obj, result;

    /* Accept simple/wide strings AND character vectors (incl. those
     * with fill-pointer or :adjustable T).  Convert string vectors to
     * a base-string view so the rest of the parser can use the simple
     * cl_string_char_at API. */
    if (CL_STRING_VECTOR_P(args[0])) {
        CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(args[0]);
        uint32_t alen = cl_vector_active_length(v);
        CL_Obj copy;
        char *buf = (char *)platform_alloc(alen + 1);
        uint32_t k;
        for (k = 0; k < alen; k++) {
            CL_Obj ch = cl_vector_data(v)[k];
            buf[k] = (char)CL_CHAR_VAL(ch);
        }
        buf[alen] = '\0';
        copy = cl_make_string(buf, alen);
        platform_free(buf);
        args[0] = copy;
    } else if (!CL_ANY_STRING_P(args[0])) {
        cl_signal_type_error(args[0], "STRING", "PARSE-INTEGER");
    }
    end = (int32_t)cl_string_length(args[0]);

    /* Odd number of trailing args → :KEY without value → PROGRAM-ERROR. */
    if ((n - 1) & 1)
        cl_error(CL_ERR_ARGS,
                 "PARSE-INTEGER: odd number of keyword arguments");

    {
        int k;
        int seen_start = 0, seen_end = 0, seen_radix = 0, seen_junk = 0;
        int allow_other = 0;
        /* First pass: detect :ALLOW-OTHER-KEYS T so unknown keys don't
         * error.  Per CLHS, the first occurrence wins for a given key. */
        for (k = 1; k + 1 < n; k += 2) {
            const char *kn;
            if (!CL_SYMBOL_P(args[k]))
                cl_error(CL_ERR_ARGS,
                         "PARSE-INTEGER: keyword arg not a symbol");
            kn = cl_symbol_name(args[k]);
            if (strcmp(kn, "ALLOW-OTHER-KEYS") == 0) {
                if (!allow_other) allow_other = !CL_NULL_P(args[k+1]);
            }
        }
        /* Second pass: extract recognized keyword values, first occurrence
         * wins, reject unknown unless :ALLOW-OTHER-KEYS T. */
        for (k = 1; k + 1 < n; k += 2) {
            const char *kn = cl_symbol_name(args[k]);
            if (strcmp(kn, "START") == 0) {
                if (!seen_start) {
                    if (CL_FIXNUM_P(args[k+1])) start = CL_FIXNUM_VAL(args[k+1]);
                    seen_start = 1;
                }
            } else if (strcmp(kn, "END") == 0) {
                if (!seen_end) {
                    if (CL_FIXNUM_P(args[k+1])) end = CL_FIXNUM_VAL(args[k+1]);
                    /* :END NIL means default (string length) — leave end. */
                    seen_end = 1;
                }
            } else if (strcmp(kn, "RADIX") == 0) {
                if (!seen_radix) {
                    if (CL_FIXNUM_P(args[k+1])) radix = CL_FIXNUM_VAL(args[k+1]);
                    seen_radix = 1;
                }
            } else if (strcmp(kn, "JUNK-ALLOWED") == 0) {
                if (!seen_junk) {
                    junk_allowed = !CL_NULL_P(args[k+1]);
                    seen_junk = 1;
                }
            } else if (strcmp(kn, "ALLOW-OTHER-KEYS") == 0) {
                /* already consumed in first pass */
            } else if (!allow_other) {
                cl_error(CL_ERR_ARGS,
                         "PARSE-INTEGER: unknown keyword %s", kn);
            }
        }
    }

    /* Skip leading whitespace */
    while (start < end && parse_integer_is_ws(cl_string_char_at(args[0], start)))
        start++;

    /* Optional sign */
    if (start < end) {
        int c = cl_string_char_at(args[0], start);
        if (c == '+' || c == '-') {
            if (c == '-') sign = -1;
            start++;
        }
    }

    radix_obj = CL_MAKE_FIXNUM(radix);
    result = CL_MAKE_FIXNUM(0);
    CL_GC_PROTECT(result);

    for (i = start; i < end; i++) {
        int digit;
        int c = cl_string_char_at(args[0], (uint32_t)i);
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'A' && c <= 'Z') digit = c - 'A' + 10;
        else if (c >= 'a' && c <= 'z') digit = c - 'a' + 10;
        else break;
        if (digit >= radix) break;
        /* Bignum-safe accumulation: result = result*radix + digit. */
        result = cl_arith_mul(result, radix_obj);
        result = cl_arith_add(result, CL_MAKE_FIXNUM(digit));
        found = 1;
    }

    /* No digits → either return NIL (junk-allowed) or signal PARSE-ERROR. */
    if (!found) {
        CL_GC_UNPROTECT(1);
        if (junk_allowed) {
            cl_mv_values[0] = CL_NIL;
            cl_mv_values[1] = CL_MAKE_FIXNUM(i);
            cl_mv_count = 2;
            return CL_NIL;
        }
        cl_error(CL_ERR_PARSE, "PARSE-INTEGER: no digits found");
    }

    /* If trailing chars remain that are not whitespace, signal PARSE-ERROR
     * unless :JUNK-ALLOWED is true. */
    if (!junk_allowed) {
        int32_t j = i;
        while (j < end && parse_integer_is_ws(cl_string_char_at(args[0], j)))
            j++;
        if (j < end) {
            CL_GC_UNPROTECT(1);
            cl_error(CL_ERR_PARSE,
                     "PARSE-INTEGER: junk after digits");
        }
        i = j;  /* second value is past trailing whitespace */
    }

    if (sign == -1)
        result = cl_arith_negate(result);
    CL_GC_UNPROTECT(1);

    cl_mv_values[0] = result;
    cl_mv_values[1] = CL_MAKE_FIXNUM(i);
    cl_mv_count = 2;
    return result;
}

/* Keywords for write-to-string (same as write) */
static CL_Obj KW_WTS_ESCAPE;
static CL_Obj KW_WTS_READABLY;
static CL_Obj KW_WTS_BASE;
static CL_Obj KW_WTS_RADIX;
static CL_Obj KW_WTS_LEVEL;
static CL_Obj KW_WTS_LENGTH;
static CL_Obj KW_WTS_CASE;
static CL_Obj KW_WTS_GENSYM;
static CL_Obj KW_WTS_ARRAY;
static CL_Obj KW_WTS_CIRCLE;
static CL_Obj KW_WTS_PRETTY;
static CL_Obj KW_WTS_RIGHT_MARGIN;

static CL_Obj bi_write_to_string(CL_Obj *args, int n)
{
    char buf[1024];
    int len, i;
    /* Keyword overrides are THREAD-LOCAL dynamic binds (tier-4 FS16, was
     * FS12's rooted global save/restore): mutating the global cells raced
     * every peer thread's printer between set and restore.  The dyn stack
     * is GC-marked, so no manual rooting of saved values is needed. */
    int dyn_mark = cl_dyn_top;
    int has_keywords = (n > 1);

    if (has_keywords) {
        for (i = 1; i + 1 < n; i += 2) {
            CL_Obj kw = args[i];
            CL_Obj val = args[i + 1];
            if (kw == KW_WTS_ESCAPE)        cl_dynbind_c(SYM_PRINT_ESCAPE, val);
            else if (kw == KW_WTS_READABLY) cl_dynbind_c(SYM_PRINT_READABLY, val);
            else if (kw == KW_WTS_BASE)     cl_dynbind_c(SYM_PRINT_BASE, val);
            else if (kw == KW_WTS_RADIX)    cl_dynbind_c(SYM_PRINT_RADIX, val);
            else if (kw == KW_WTS_LEVEL)    cl_dynbind_c(SYM_PRINT_LEVEL, val);
            else if (kw == KW_WTS_LENGTH)   cl_dynbind_c(SYM_PRINT_LENGTH, val);
            else if (kw == KW_WTS_CASE)     cl_dynbind_c(SYM_PRINT_CASE, val);
            else if (kw == KW_WTS_GENSYM)   cl_dynbind_c(SYM_PRINT_GENSYM, val);
            else if (kw == KW_WTS_ARRAY)    cl_dynbind_c(SYM_PRINT_ARRAY, val);
            else if (kw == KW_WTS_CIRCLE)   cl_dynbind_c(SYM_PRINT_CIRCLE, val);
            else if (kw == KW_WTS_PRETTY)   cl_dynbind_c(SYM_PRINT_PRETTY, val);
            else if (kw == KW_WTS_RIGHT_MARGIN) cl_dynbind_c(SYM_PRINT_RIGHT_MARGIN, val);
        }
    }

    len = cl_prin1_to_string(args[0], buf, sizeof(buf));

    if (has_keywords)
        cl_dynbind_restore_to(dyn_mark);

    return cl_make_string(buf, (uint32_t)len);
}

static CL_Obj bi_prin1_to_string_fn(CL_Obj *args, int n)
{
    char buf[1024];
    int len;
    CL_UNUSED(n);
    len = cl_prin1_to_string(args[0], buf, sizeof(buf));
    return cl_make_string(buf, (uint32_t)len);
}

static CL_Obj bi_princ_to_string_fn(CL_Obj *args, int n)
{
    char buf[1024];
    int len;
    CL_UNUSED(n);
    len = cl_princ_to_string(args[0], buf, sizeof(buf));
    return cl_make_string(buf, (uint32_t)len);
}

/* --- Phase 8 Step 2: Additional string/char operations --- */

static CL_Obj bi_string_capitalize(CL_Obj *args, int n)
{
    CL_Obj str, result;
    uint32_t i, start, end;
    int in_word = 0;
    str = coerce_to_string_obj(args[0], "STRING-CAPITALIZE");
    result = cl_string_copy(str);
    parse_case_bounds("STRING-CAPITALIZE", args, n,
                      cl_string_length(result), &start, &end);
    for (i = start; i < end; i++) {
        int c = cl_string_char_at(result, i);
        int is_alpha_c = cl_isalpha(c);
        int is_alnum = is_alpha_c || (c >= '0' && c <= '9');
        if (!in_word && is_alpha_c) {
            cl_string_set_char_at(result, i, cl_toupper(c));
            in_word = 1;
        } else if (in_word && is_alnum) {
            cl_string_set_char_at(result, i, cl_tolower(c));
        } else {
            in_word = is_alnum;
        }
    }
    return result;
}

static CL_Obj bi_nstring_upcase(CL_Obj *args, int n)
{
    uint32_t i, start, end;
    if (!CL_ANY_STRING_P(args[0]))
        cl_signal_type_error(args[0], "STRING", "NSTRING-UPCASE");
    parse_case_bounds("NSTRING-UPCASE", args, n,
                      cl_string_length(args[0]), &start, &end);
    for (i = start; i < end; i++) {
        int ch = cl_string_char_at(args[0], i);
        int up = cl_toupper(ch);
        if (up != ch)
            cl_string_set_char_at(args[0], i, up);
    }
    return args[0];
}

static CL_Obj bi_nstring_downcase(CL_Obj *args, int n)
{
    uint32_t i, start, end;
    if (!CL_ANY_STRING_P(args[0]))
        cl_signal_type_error(args[0], "STRING", "NSTRING-DOWNCASE");
    parse_case_bounds("NSTRING-DOWNCASE", args, n,
                      cl_string_length(args[0]), &start, &end);
    for (i = start; i < end; i++) {
        int ch = cl_string_char_at(args[0], i);
        int lo = cl_tolower(ch);
        if (lo != ch)
            cl_string_set_char_at(args[0], i, lo);
    }
    return args[0];
}

static CL_Obj bi_nstring_capitalize(CL_Obj *args, int n)
{
    uint32_t i, start, end;
    int in_word = 0;
    if (!CL_ANY_STRING_P(args[0]))
        cl_signal_type_error(args[0], "STRING", "NSTRING-CAPITALIZE");
    parse_case_bounds("NSTRING-CAPITALIZE", args, n,
                      cl_string_length(args[0]), &start, &end);
    for (i = start; i < end; i++) {
        int c = cl_string_char_at(args[0], i);
        int is_alpha_c = cl_isalpha(c);
        int is_alnum = is_alpha_c || (c >= '0' && c <= '9');
        if (!in_word && is_alpha_c) {
            cl_string_set_char_at(args[0], i, cl_toupper(c));
            in_word = 1;
        } else if (in_word && is_alnum) {
            cl_string_set_char_at(args[0], i, cl_tolower(c));
        } else {
            in_word = is_alnum;
        }
    }
    return args[0];
}

/* char-name table — primary name first for each code, aliases follow */
static const struct { int code; const char *name; } char_names[] = {
    {'\0', "Null"},
    {' ',  "Space"},
    {'\n', "Newline"},
    {'\t', "Tab"},
    {'\r', "Return"},
    {'\b', "Backspace"},
    {127,  "Rubout"},
    {'\f', "Page"},
    {'\n', "Linefeed"},
    {0x1B, "Escape"},
    {0x0B, "Vt"},
    {0x07, "Bell"},
    {127,  "Delete"},
    {0x01, "Soh"}, {0x02, "Stx"}, {0x03, "Etx"}, {0x04, "Eot"},
    {0x05, "Enq"}, {0x06, "Ack"}, {0x0E, "So"},  {0x0F, "Si"},
    {0x10, "Dle"}, {0x11, "Dc1"}, {0x12, "Dc2"}, {0x13, "Dc3"},
    {0x14, "Dc4"}, {0x15, "Nak"}, {0x16, "Syn"}, {0x17, "Etb"},
    {0x18, "Can"}, {0x19, "Em"},  {0x1A, "Sub"}, {0x1C, "Fs"},
    {0x1D, "Gs"},  {0x1E, "Rs"},  {0x1F, "Us"},
    {0xA0, "No-Break_Space"},
    {0x3000, "Ideographic_Space"},
    {0xFFFD, "Replacement_Character"},
    {0, NULL}
};

static CL_Obj bi_char_name(CL_Obj *args, int n)
{
    int c, i;
    CL_UNUSED(n);
    c = require_char(args[0], "CHAR-NAME");
    for (i = 0; char_names[i].name; i++) {
        if (char_names[i].code == c)
            return cl_make_string(char_names[i].name,
                                  (uint32_t)strlen(char_names[i].name));
    }
    return CL_NIL;
}

#ifdef PLATFORM_AMIGA
/* vbcc has no <strings.h> — provide case-insensitive compare */
static int cl_local_strcasecmp(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}
#define STRCASECMP cl_local_strcasecmp
#else
#include <strings.h>
#define STRCASECMP strcasecmp
#endif

/* Normalize character for name comparison: uppercase, treat _ and - as equal */
static int char_name_normalize(int c)
{
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c == '_') c = '-';
    return c;
}

static CL_Obj bi_name_char(CL_Obj *args, int n)
{
    uint32_t len, j;
    int i;
    CL_UNUSED(n);
    if (!CL_ANY_STRING_P(args[0]))
        cl_error(CL_ERR_TYPE, "NAME-CHAR: not a string");
    len = cl_string_length(args[0]);
    /* Check table entries (case-insensitive, _ == -) */
    for (i = 0; char_names[i].name; i++) {
        const char *cname = char_names[i].name;
        uint32_t clen = (uint32_t)strlen(cname);
        if (clen != len) continue;
        for (j = 0; j < len; j++) {
            int ca = char_name_normalize(cl_string_char_at(args[0], j));
            int cb = char_name_normalize((unsigned char)cname[j]);
            if (ca != cb) break;
        }
        if (j == len)
            return CL_MAKE_CHAR(char_names[i].code);
    }
    /* Check U+XXXX hex syntax */
    if (len >= 3 && len <= 8) {
        int c0 = cl_string_char_at(args[0], 0);
        int c1 = cl_string_char_at(args[0], 1);
        if ((c0 == 'U' || c0 == 'u') && c1 == '+') {
            unsigned long cp = 0;
            for (j = 2; j < len; j++) {
                int ch = cl_string_char_at(args[0], j);
                if (ch >= '0' && ch <= '9') cp = cp * 16 + (unsigned long)(ch - '0');
                else if (ch >= 'A' && ch <= 'F') cp = cp * 16 + (unsigned long)(ch - 'A' + 10);
                else if (ch >= 'a' && ch <= 'f') cp = cp * 16 + (unsigned long)(ch - 'a' + 10);
                else break;
            }
            if (j == len && cp <= 0x10FFFF)
                return CL_MAKE_CHAR((int)cp);
        }
    }
    return CL_NIL;
}

static CL_Obj bi_graphic_char_p(CL_Obj *args, int n)
{
    int c;
    CL_UNUSED(n);
    c = require_char(args[0], "GRAPHIC-CHAR-P");
    return (c >= 32 && c < 127) ? SYM_T : CL_NIL;
}

static CL_Obj bi_standard_char_p(CL_Obj *args, int n)
{
    int c;
    CL_UNUSED(n);
    c = require_char(args[0], "STANDARD-CHAR-P");
    /* Standard chars: graphic chars + space + newline */
    return ((c >= 32 && c < 127) || c == '\n') ? SYM_T : CL_NIL;
}

static CL_Obj bi_alphanumericp(CL_Obj *args, int n)
{
    int c;
    CL_UNUSED(n);
    c = require_char(args[0], "ALPHANUMERICP");
    return (cl_isalpha(c) || (c >= '0' && c <= '9')) ? SYM_T : CL_NIL;
}

static CL_Obj bi_digit_char(CL_Obj *args, int n)
{
    int32_t weight, radix = 10;
    CL_UNUSED(n);
    if (!CL_FIXNUM_P(args[0]))
        cl_error(CL_ERR_TYPE, "DIGIT-CHAR: not an integer");
    weight = CL_FIXNUM_VAL(args[0]);
    if (n >= 2 && CL_FIXNUM_P(args[1]))
        radix = CL_FIXNUM_VAL(args[1]);
    if (weight < 0 || weight >= radix)
        return CL_NIL;
    if (weight < 10)
        return CL_MAKE_CHAR('0' + weight);
    return CL_MAKE_CHAR('A' + weight - 10);
}

static CL_Obj bi_both_case_p(CL_Obj *args, int n)
{
    int c;
    CL_UNUSED(n);
    c = require_char(args[0], "BOTH-CASE-P");
    return cl_isalpha(c) ? SYM_T : CL_NIL;
}

static int char_upcase_val(CL_Obj c)
{
    int v = CL_CHAR_VAL(c);
    if (v >= 'a' && v <= 'z') v -= 32;
    return v;
}

DEFINE_CHAR_ORDER_CMP(bi_char_equal,        "CHAR-EQUAL",        char_upcase_val, ==)
DEFINE_CHAR_NE_CMP   (bi_char_not_equal,    "CHAR-NOT-EQUAL",    char_upcase_val)
DEFINE_CHAR_ORDER_CMP(bi_char_lessp,        "CHAR-LESSP",        char_upcase_val, <)
DEFINE_CHAR_ORDER_CMP(bi_char_greaterp,     "CHAR-GREATERP",     char_upcase_val, >)
DEFINE_CHAR_ORDER_CMP(bi_char_not_greaterp, "CHAR-NOT-GREATERP", char_upcase_val, <=)
DEFINE_CHAR_ORDER_CMP(bi_char_not_lessp,    "CHAR-NOT-LESSP",    char_upcase_val, >=)

/* --- MAKE-STRING --- */

static CL_Obj KW_INITIAL_ELEMENT;
static CL_Obj KW_ELEMENT_TYPE;

#ifdef CL_WIDE_STRINGS
/* Decide whether MAKE-STRING with element-type ET should allocate a wide
 * string so stores of code points > 255 are lossless.  CHARACTER and
 * EXTENDED-CHAR (and deftypes expanding to them, e.g. babel's UNICODE-CHAR)
 * need a wide string; BASE-CHAR / STANDARD-CHAR stay narrow.  DEPTH bounds
 * deftype-expansion recursion. */
static int make_string_want_wide(CL_Obj et, int depth)
{
    extern CL_Obj cl_get_type_expander(CL_Obj name);
    if (depth <= 0) return 0;
    if (CL_SYMBOL_P(et)) {
        const char *nm = cl_symbol_name(et);
        if (strcmp(nm, "CHARACTER") == 0 || strcmp(nm, "EXTENDED-CHAR") == 0)
            return 1;
        if (strcmp(nm, "BASE-CHAR") == 0 || strcmp(nm, "STANDARD-CHAR") == 0)
            return 0;
        {
            CL_Obj ex = cl_get_type_expander(et);
            if (!CL_NULL_P(ex))
                return make_string_want_wide(cl_vm_apply(ex, NULL, 0), depth - 1);
        }
    }
    return 0;
}
#endif

static CL_Obj bi_make_string_fn(CL_Obj *args, int n)
{
    uint32_t size;
    int fill_char = ' ';  /* default fill (full code point) */
    int want_wide = 0;
    int i;
    CL_Obj result;

    if (!CL_FIXNUM_P(args[0]) || CL_FIXNUM_VAL(args[0]) < 0)
        cl_error(CL_ERR_TYPE, "MAKE-STRING: size must be a non-negative integer");
    size = (uint32_t)CL_FIXNUM_VAL(args[0]);

    /* Parse keyword arguments */
    for (i = 1; i + 1 < n; i += 2) {
        if (args[i] == KW_INITIAL_ELEMENT) {
            if (!CL_CHAR_P(args[i + 1]))
                cl_error(CL_ERR_TYPE, "MAKE-STRING: :initial-element must be a character");
            fill_char = (int)CL_CHAR_VAL(args[i + 1]);
        } else if (args[i] == KW_ELEMENT_TYPE) {
#ifdef CL_WIDE_STRINGS
            /* Honor an explicit general CHARACTER element-type by widening
             * so wide chars stored later are not truncated.  A plain
             * (make-string n) keeps the memory-efficient narrow default. */
            if (make_string_want_wide(args[i + 1], 16))
                want_wide = 1;
#endif
        }
    }

#ifdef CL_WIDE_STRINGS
    /* A wide initial-element also forces a wide string. */
    if (fill_char > 255) want_wide = 1;
    result = want_wide ? cl_make_wide_string(NULL, size)
                       : cl_make_string(NULL, size);
#else
    result = cl_make_string(NULL, size);
#endif
    {
        uint32_t j;
        for (j = 0; j < size; j++)
            cl_string_set_char_at(result, j, fill_char);
    }
    return result;
}

/* --- Registration --- */

void cl_builtins_strings_init(void)
{
    /* Character functions */
    defun("CHARACTERP", bi_characterp, 1, 1);
    defun("CHAR=", bi_char_eq, 1, -1);
    defun("CHAR/=", bi_char_ne, 1, -1);
    defun("CHAR<", bi_char_lt, 1, -1);
    defun("CHAR>", bi_char_gt, 1, -1);
    defun("CHAR<=", bi_char_le, 1, -1);
    defun("CHAR>=", bi_char_ge, 1, -1);
    defun("CHAR-CODE", bi_char_code, 1, 1);
    defun("CODE-CHAR", bi_code_char, 1, 1);
    defun("CHAR-INT", bi_char_int, 1, 1);
    defun("CHARACTER", bi_character, 1, 1);
    defun("CHAR-UPCASE", bi_char_upcase, 1, 1);
    defun("CHAR-DOWNCASE", bi_char_downcase, 1, 1);
    defun("UPPER-CASE-P", bi_upper_case_p, 1, 1);
    defun("LOWER-CASE-P", bi_lower_case_p, 1, 1);
    defun("ALPHA-CHAR-P", bi_alpha_char_p, 1, 1);
    defun("DIGIT-CHAR-P", bi_digit_char_p, 1, 2);

    /* Symbol functions */
    defun("SYMBOL-NAME", bi_symbol_name, 1, 1);
    defun("SYMBOL-PACKAGE", bi_symbol_package, 1, 1);
    defun("MAKE-SYMBOL", bi_make_symbol, 1, 1);
    defun("KEYWORDP", bi_keywordp, 1, 1);

    /* String functions */
    defun("STRING=", bi_string_eq, 2, -1);
    defun("STRING/=", bi_string_neq, 2, -1);
    defun("STRING-EQUAL", bi_string_equal, 2, -1);
    defun("STRING-NOT-EQUAL", bi_string_not_equal, 2, -1);
    defun("STRING<", bi_string_lt, 2, -1);
    defun("STRING>", bi_string_gt, 2, -1);
    defun("STRING<=", bi_string_le, 2, -1);
    defun("STRING>=", bi_string_ge, 2, -1);
    defun("STRING-LESSP", bi_string_lessp, 2, -1);
    defun("STRING-GREATERP", bi_string_greaterp, 2, -1);
    defun("STRING-NOT-GREATERP", bi_string_not_greaterp, 2, -1);
    defun("STRING-NOT-LESSP", bi_string_not_lessp, 2, -1);
    defun("STRING-UPCASE", bi_string_upcase, 1, -1);
    defun("STRING-DOWNCASE", bi_string_downcase, 1, -1);
    defun("STRING-TRIM", bi_string_trim, 2, 2);
    defun("STRING-LEFT-TRIM", bi_string_left_trim, 2, 2);
    defun("STRING-RIGHT-TRIM", bi_string_right_trim, 2, 2);
    defun("SUBSEQ", bi_subseq, 2, 3);
    cl_register_builtin("%SETF-SUBSEQ", bi_setf_subseq, 3, 4, cl_package_clamiga);
    defun("CONCATENATE", bi_concatenate, 1, -1);
    defun("CHAR", bi_char_accessor, 2, 2);
    defun("SCHAR", bi_schar_accessor, 2, 2);
    defun("STRING", bi_string_coerce, 1, 1);
    defun("PARSE-INTEGER", bi_parse_integer, 1, -1);
    defun("WRITE-TO-STRING", bi_write_to_string, 1, -1);

    /* Initialize write-to-string keywords */
    KW_WTS_ESCAPE   = cl_intern_in("ESCAPE",   6, cl_package_keyword);
    KW_WTS_READABLY = cl_intern_in("READABLY", 8, cl_package_keyword);
    KW_WTS_BASE     = cl_intern_in("BASE",     4, cl_package_keyword);
    KW_WTS_RADIX    = cl_intern_in("RADIX",    5, cl_package_keyword);
    KW_WTS_LEVEL    = cl_intern_in("LEVEL",    5, cl_package_keyword);
    KW_WTS_LENGTH   = cl_intern_in("LENGTH",   6, cl_package_keyword);
    KW_WTS_CASE     = cl_intern_in("CASE",     4, cl_package_keyword);
    KW_WTS_GENSYM   = cl_intern_in("GENSYM",   6, cl_package_keyword);
    KW_WTS_ARRAY    = cl_intern_in("ARRAY",    5, cl_package_keyword);
    KW_WTS_CIRCLE   = cl_intern_in("CIRCLE",   6, cl_package_keyword);
    KW_WTS_PRETTY   = cl_intern_in("PRETTY",   6, cl_package_keyword);
    KW_WTS_RIGHT_MARGIN = cl_intern_in("RIGHT-MARGIN", 12, cl_package_keyword);
    defun("PRIN1-TO-STRING", bi_prin1_to_string_fn, 1, 1);
    defun("PRINC-TO-STRING", bi_princ_to_string_fn, 1, 1);

    /* Phase 8 Step 2 */
    defun("STRING-CAPITALIZE", bi_string_capitalize, 1, -1);
    defun("NSTRING-UPCASE", bi_nstring_upcase, 1, -1);
    defun("NSTRING-DOWNCASE", bi_nstring_downcase, 1, -1);
    defun("NSTRING-CAPITALIZE", bi_nstring_capitalize, 1, -1);
    defun("CHAR-NAME", bi_char_name, 1, 1);
    defun("NAME-CHAR", bi_name_char, 1, 1);
    defun("GRAPHIC-CHAR-P", bi_graphic_char_p, 1, 1);
    defun("STANDARD-CHAR-P", bi_standard_char_p, 1, 1);
    defun("ALPHANUMERICP", bi_alphanumericp, 1, 1);
    defun("DIGIT-CHAR", bi_digit_char, 1, 2);
    defun("BOTH-CASE-P", bi_both_case_p, 1, 1);
    defun("CHAR-EQUAL", bi_char_equal, 1, -1);
    defun("CHAR-NOT-EQUAL", bi_char_not_equal, 1, -1);
    defun("CHAR-LESSP", bi_char_lessp, 1, -1);
    defun("CHAR-GREATERP", bi_char_greaterp, 1, -1);
    defun("CHAR-NOT-GREATERP", bi_char_not_greaterp, 1, -1);
    defun("CHAR-NOT-LESSP", bi_char_not_lessp, 1, -1);
    defun("MAKE-STRING", bi_make_string_fn, 1, -1);

    KW_INITIAL_ELEMENT = cl_intern_keyword("INITIAL-ELEMENT", 15);
    KW_ELEMENT_TYPE    = cl_intern_keyword("ELEMENT-TYPE", 12);

    /* Register cached symbols for GC compaction forwarding */
    cl_gc_register_root(&KW_WTS_ESCAPE);
    cl_gc_register_root(&KW_WTS_READABLY);
    cl_gc_register_root(&KW_WTS_BASE);
    cl_gc_register_root(&KW_WTS_RADIX);
    cl_gc_register_root(&KW_WTS_LEVEL);
    cl_gc_register_root(&KW_WTS_LENGTH);
    cl_gc_register_root(&KW_WTS_CASE);
    cl_gc_register_root(&KW_WTS_GENSYM);
    cl_gc_register_root(&KW_WTS_ARRAY);
    cl_gc_register_root(&KW_WTS_CIRCLE);
    cl_gc_register_root(&KW_WTS_PRETTY);
    cl_gc_register_root(&KW_WTS_RIGHT_MARGIN);
    cl_gc_register_root(&KW_INITIAL_ELEMENT);
    cl_gc_register_root(&KW_ELEMENT_TYPE);
}
