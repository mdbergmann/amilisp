#include "reader.h"
#include "readtable.h"
#include "symbol.h"
#include "package.h"
#include "bindtab.h"   /* cl_bindtab_shed_p — the shed-package hint below */
#include "mem.h"
#include "bignum.h"
#include "float.h"
#include "ratio.h"
#include "error.h"
#include "stream.h"
#include "string_utils.h"
#include "vm.h"
#include "compiler.h"
#include "../platform/platform.h"
#include <string.h>
#include "ascii_ctype.h"
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>

/* strcasecmp: available via <strings.h> on POSIX, provide fallback for AmigaOS */
#ifdef PLATFORM_AMIGA
static int cl_strcasecmp(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}
#define strcasecmp cl_strcasecmp
#else
#include <strings.h>
#endif

/* Reader state now lives in CL_Thread.  Local macros redirect old names. */
#define reader_stream  (CT->rd_stream)
#define eof_seen       (CT->rd_eof)
#define rd_suppress_depth (CT->rd_suppress)
#define reader_line    (CT->rd_line)
#define rd_uninterned  (CT->rd_uninterned)
#define rd_labels      (CT->rd_labels)
#define rd_label_backrefs (CT->rd_label_backrefs)
/* cl_current_source_file and cl_current_file_id are macros from thread.h */

/* Reader suppression (CLHS 2.4.8.17, 23.2).  Two independent sources:
 *
 *   rd_suppress_depth — internal counter, bumped while skipping the false
 *                       branch of a #+/#- feature conditional.
 *   *READ-SUPPRESS*   — the user-visible CL variable.  When true, read and
 *                       friends still parse their input but construct no
 *                       object, intern no symbol, and signal no error of
 *                       type reader-error; the primary value is NIL.
 *
 * Suppression is active when either holds.  Every call site is a cold path
 * (dispatch macros, error branches, end-of-token) — never per character — so
 * the dynamic lookup costs nothing in the common case.
 */
static int read_suppress_active(void)
{
    if (CT->rd_suppress) return 1;
    if (CL_NULL_P(SYM_READ_SUPPRESS)) return 0;  /* before symbol init */
    return !CL_NULL_P(cl_symbol_value(SYM_READ_SUPPRESS));
}
#define read_suppress (read_suppress_active())

/* Source location tracking (shared, not per-thread) */
CL_SrcLoc cl_srcloc_table[CL_SRCLOC_SIZE];

/* Reader error helper — prepends source file and line to the message so
 * diagnostics point at the actual location, not always "(line 1)". */
static void cl_reader_error(int code, const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    const char *file;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    file = cl_current_source_file;
    if (file && file[0]) {
        /* Show only the basename to keep messages compact */
        const char *base = file;
        const char *p;
        for (p = file; *p; p++) {
            if (*p == '/' || *p == ':') base = p + 1;
        }
        cl_error(code, "%s:%d: %s", base, reader_line, msg);
    } else {
        cl_error(code, "line %d: %s", reader_line, msg);
    }
}

static int read_char(void)
{
    int ch = cl_stream_read_char(reader_stream);
    if (ch == '\n') reader_line++;
    return ch;
}

static void unread_char(int ch)
{
    if (ch < 0) return;
    cl_stream_unread_char(reader_stream, ch);
    if (ch == '\n') reader_line--;
}

static int current_line(void)
{
    return reader_line;
}

static void srcloc_record(CL_Obj cons_obj, int line)
{
    uint32_t idx = CL_SRCLOC_INDEX(cons_obj);
    /* Ordered writes: this diagnostics-only table is written without a lock
     * (concurrent reader threads), so invalidate the key FIRST — a
     * concurrent cl_srcloc_lookup then never pairs the new key with the old
     * entry's line/file. */
    cl_srcloc_table[idx].cons_obj = CL_NIL;
    cl_srcloc_table[idx].line = (uint16_t)line;
    cl_srcloc_table[idx].file_id = cl_current_file_id;
    cl_srcloc_table[idx].cons_obj = cons_obj;
}

/* Skip whitespace and comments */
static void skip_whitespace(void)
{
    int ch;
    for (;;) {
        ch = read_char();
        if (ch < 0) { eof_seen = 1; return; }
        if (ch == ';') {
            /* Line comment */
            do { ch = read_char(); } while (ch >= 0 && ch != '\n');
            if (ch < 0) { eof_seen = 1; return; }
            continue;
        }
        if (!cl_ascii_isspace(ch)) {
            unread_char(ch);
            return;
        }
    }
}

static int is_delimiter(int ch)
{
    if (ch < 0) return 1;
    if (ch < CL_RT_CHARS) {
        CL_Readtable *rt = cl_readtable_current();
        uint8_t syn = rt->syntax[ch];
        return syn == CL_CHAR_WHITESPACE || syn == CL_CHAR_TERM_MACRO;
    }
    return 0;
}

/* Forward declarations */
static CL_Obj read_expr(void);
static CL_Obj read_list(void);

/* Sentinel value returned when #+/#- skips a form.
 * Low byte 0x06: not fixnum (bit 0 = 0), not char (not 0x0A), not NIL (not 0).
 * Never a valid arena offset. Only used internally by the reader. */
#define CL_READER_SKIP ((CL_Obj)0x06)

/* --- Feature conditional support (#+ / #-) --- */

/* Check if a keyword symbol is in the *features* list */
static int feature_member(CL_Obj keyword)
{
    CL_Obj list = cl_symbol_value(SYM_STAR_FEATURES);
    while (CL_CONS_P(list)) {
        if (cl_car(list) == keyword)
            return 1;
        list = cl_cdr(list);
    }
    return 0;
}

/* Keyword symbols for feature expression operators.
 * Initialized lazily on first use to avoid init-order issues. */
static CL_Obj kw_and = CL_NIL;
static CL_Obj kw_or  = CL_NIL;
static CL_Obj kw_not = CL_NIL;

static void ensure_feature_keywords(void)
{
    /* GC SAFETY (audit tier 4, R5): register each cached symbol as a root
     * IMMEDIATELY after its intern — the next cl_intern_keyword can compact,
     * and a root registered afterwards would pin a stale offset permanently
     * (these are one-shot registrations). */
    if (CL_NULL_P(kw_and)) {
        kw_and = cl_intern_keyword("AND", 3);
        cl_gc_register_root(&kw_and);
        kw_or  = cl_intern_keyword("OR", 2);
        cl_gc_register_root(&kw_or);
        kw_not = cl_intern_keyword("NOT", 3);
        cl_gc_register_root(&kw_not);
    }
}

/* Evaluate a feature expression (read as a form) against *features*.
 * Feature exprs: keyword atoms, (:and ...), (:or ...), (:not ...) */
static int eval_feature_expr(CL_Obj expr)
{
    if (CL_SYMBOL_P(expr)) {
        /* Should be a keyword — check membership */
        return feature_member(expr);
    }
    if (CL_CONS_P(expr)) {
        CL_Obj head, rest;
        /* First-call interns can compact — keep expr forwarded across the
         * ensure and read head/rest AFTER it (R5), so neither the expr cons
         * nor the freshly-registered kw_* roots are stale for the compares
         * below. */
        CL_GC_PROTECT(expr);
        ensure_feature_keywords();
        CL_GC_UNPROTECT(1);
        head = cl_car(expr);
        rest = cl_cdr(expr);
        if (head == kw_and) {
            /* (:and f1 f2 ...) — all must be true */
            while (CL_CONS_P(rest)) {
                if (!eval_feature_expr(cl_car(rest)))
                    return 0;
                rest = cl_cdr(rest);
            }
            return 1;
        }
        if (head == kw_or) {
            /* (:or f1 f2 ...) — any must be true */
            while (CL_CONS_P(rest)) {
                if (eval_feature_expr(cl_car(rest)))
                    return 1;
                rest = cl_cdr(rest);
            }
            return 0;
        }
        if (head == kw_not) {
            /* (:not f) — invert */
            if (CL_CONS_P(rest))
                return !eval_feature_expr(cl_car(rest));
            return 1;
        }
    }
    return 0;
}

/* Skip (read and discard) a single form from the reader.
 * Increments read_suppress so that errors (unknown packages, etc.) are suppressed.
 *
 * read_expr() returns CL_READER_SKIP (rather than a real datum) when it reads a
 * false #+/#- conditional, which yields no object.  A skipped datum may itself
 * be such a conditional — e.g. the `#+lispworks #+lispworks :element-type
 * 'lw:simple-char` idiom, where the OUTER #+ must skip a genuine datum, reading
 * PAST the inner #+ (which produced nothing) and discarding the form after it.
 * So loop until an actual datum is read-and-discarded (or EOF), mirroring how
 * read_list (line ~877) and the top-level read loops drop CL_READER_SKIP. */
static void skip_form(void)
{
    CL_Obj r;
    rd_suppress_depth++;
    do { r = read_expr(); } while (r == CL_READER_SKIP && !eof_seen);
    rd_suppress_depth--;
}

/* Read a number in the given radix (2, 8, 16, or arbitrary via #nR).
 * Handles optional sign, fixnum/bignum dispatch. */
static CL_Obj read_radix_number(int radix)
{
    char buf[256];
    int len = 0;
    int ch;
    int neg = 0;
    int i;

    /* Read token */
    for (;;) {
        ch = read_char();
        if (is_delimiter(ch)) { unread_char(ch); break; }
        if (len >= 255)
            cl_reader_error(CL_ERR_PARSE,
                "Number after #-radix prefix longer than 255 characters");
        buf[len++] = (char)cl_ascii_toupper(ch);
    }
    buf[len] = '\0';

    if (read_suppress) return CL_NIL;
    if (len == 0) {
        cl_reader_error(CL_ERR_PARSE, "No digits after #radix prefix");
        return CL_NIL;
    }

    /* Handle sign */
    i = 0;
    if (buf[0] == '+') i = 1;
    else if (buf[0] == '-') { neg = 1; i = 1; }

    if (i >= len) {
        cl_reader_error(CL_ERR_PARSE, "No digits after sign in #radix number");
        return CL_NIL;
    }

    /* Try fixnum range first using unsigned long accumulator */
    {
        unsigned long val = 0;
        int overflow = 0;
        int j;
        for (j = i; j < len; j++) {
            int dv;
            char c = buf[j];
            if (c >= '0' && c <= '9') dv = c - '0';
            else if (c >= 'A' && c <= 'Z') dv = c - 'A' + 10;
            else {
                cl_reader_error(CL_ERR_PARSE, "Invalid digit '%c' for radix %d", buf[j], radix);
                return CL_NIL;
            }
            if (dv >= radix) {
                cl_reader_error(CL_ERR_PARSE, "Invalid digit '%c' for radix %d", buf[j], radix);
                return CL_NIL;
            }
            if (!overflow) {
                unsigned long nv = val * (unsigned long)radix + (unsigned long)dv;
                if (nv / (unsigned long)radix != val) overflow = 1;
                else val = nv;
            }
        }
        if (!overflow) {
            /* Guard against unsigned→signed overflow on 32-bit:
               if val > LONG_MAX, (long)val wraps to negative.
               Check against fixnum range using unsigned comparisons. */
            if (!neg && val <= (unsigned long)CL_FIXNUM_MAX)
                return CL_MAKE_FIXNUM((int32_t)val);
            if (neg && val <= ((unsigned long)CL_FIXNUM_MAX + 1u))
                return CL_MAKE_FIXNUM(-(int32_t)val);
        }
    }

    /* Bignum: Horner's method with cl_arith_mul/cl_arith_add */
    {
        CL_Obj result = CL_MAKE_FIXNUM(0);
        CL_Obj radix_obj = CL_MAKE_FIXNUM(radix);
        int j;
        CL_GC_PROTECT(result);
        for (j = i; j < len; j++) {
            int dv;
            char c = buf[j];
            if (c >= '0' && c <= '9') dv = c - '0';
            else dv = c - 'A' + 10;
            result = cl_arith_mul(result, radix_obj);
            result = cl_arith_add(result, CL_MAKE_FIXNUM(dv));
        }
        if (neg) result = cl_arith_negate(result);
        CL_GC_UNPROTECT(1);
        return result;
    }
}

/* --- #n= / #n# shared-and-circular-structure reader macros (CLHS 2.4.8.15-16) ---
 *
 * rd_labels is an alist (label-fixnum . object) scoped to one top-level READ.
 * #n= registers a unique placeholder cons for label n, reads its object, then
 * rebinds n to that object and (only when a circular forward reference actually
 * occurred) patches the placeholder out of the structure.  #n# returns whatever
 * is currently bound to n — the final object, or the placeholder for a still-in-
 * progress (circular) definition. */

/* Locate the alist cell for label n; returns the (n . obj) cons or CL_NIL. */
static CL_Obj reader_label_cell(int n)
{
    CL_Obj p = rd_labels;
    CL_Obj key = CL_MAKE_FIXNUM(n);
    while (CL_CONS_P(p)) {
        CL_Obj cell = cl_car(p);
        if (CL_CONS_P(cell) && cl_car(cell) == key)
            return cell;
        p = cl_cdr(p);
    }
    return CL_NIL;
}

/* Replace every reference to `from` reachable through the CONS slots and
 * general-vector elements of `root` with `to` (#1=(a . #1#), #1=#(a #1#),
 * #1=#2A((#1#))).  Non-allocating (so no GC can move objects mid-walk); a
 * bounded visited set makes it terminate on the cyclic structure it is fixing
 * up.  Used only for genuinely circular #n= definitions — the common
 * shared-label case (e.g. jzon's #1=#:|| ... #1#) never reaches here. */
static int reader_patch_container_p(CL_Obj obj)
{
    if (CL_CONS_P(obj)) return 1;
    if (CL_VECTOR_P(obj)) {
        CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(obj);
        return !(v->flags & (CL_VEC_FLAG_STRING | CL_VEC_FLAG_DISPLACED));
    }
    return 0;
}

static void reader_patch_labels(CL_Obj root, CL_Obj from, CL_Obj to)
{
    enum { MAXN = 256 };
    CL_Obj stack[MAXN];
    CL_Obj visited[MAXN];
    int sp = 0, nv = 0, i;

    if (!reader_patch_container_p(root)) return;
    stack[sp++] = root;
    while (sp > 0) {
        CL_Obj node = stack[--sp];
        int seen = 0;
        for (i = 0; i < nv; i++)
            if (visited[i] == node) { seen = 1; break; }
        if (seen) continue;
        if (nv >= MAXN) return;        /* structure too large — degrade safely */
        visited[nv++] = node;
        if (CL_CONS_P(node)) {
            CL_Cons *c = (CL_Cons *)CL_OBJ_TO_PTR(node);
            if (c->car == from) c->car = to;
            else if (reader_patch_container_p(c->car) && sp < MAXN) stack[sp++] = c->car;
            if (c->cdr == from) c->cdr = to;
            else if (reader_patch_container_p(c->cdr) && sp < MAXN) stack[sp++] = c->cdr;
        } else {
            CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(node);
            CL_Obj *data = cl_vector_data(v);
            uint32_t k;
            for (k = 0; k < v->length; k++) {
                if (data[k] == from) data[k] = to;
                else if (reader_patch_container_p(data[k]) && sp < MAXN)
                    stack[sp++] = data[k];
            }
        }
    }
}

/* #n= — read and label an object. */
static CL_Obj read_labeled_object(int n)
{
    CL_Obj placeholder, cell, obj;
    int backrefs_before;

    if (read_suppress) return read_expr();   /* in a #+/#- false branch */

    if (!CL_NULL_P(reader_label_cell(n))) {
        cl_reader_error(CL_ERR_PARSE, "Reader label #%d= already defined", n);
        return CL_NIL;
    }

    /* Unique placeholder so any #n# inside the object can refer back to us.
     * CL_UNBOUND as car distinguishes it from a real (nil . nil) value — no
     * datum the reader builds has it.  It has to be a marker the collector
     * knows: the placeholder is a rooted heap cons while the labelled object
     * is read (which allocates), and CL_READER_SKIP here made a GC in that
     * window mark "offset 6" as an object. */
    placeholder = cl_cons(CL_UNBOUND, CL_NIL);
    CL_GC_PROTECT(placeholder);
    cell = cl_cons(CL_MAKE_FIXNUM(n), placeholder);
    CL_GC_PROTECT(cell);
    rd_labels = cl_cons(cell, rd_labels);

    backrefs_before = rd_label_backrefs;
    obj = read_expr();
    CL_GC_PROTECT(obj);

    /* Rebind label to the real object and, if the object referred back to the
     * placeholder (circular), splice the object in for the placeholder. */
    ((CL_Cons *)CL_OBJ_TO_PTR(cell))->cdr = obj;
    if (rd_label_backrefs > backrefs_before && obj != placeholder)
        reader_patch_labels(obj, placeholder, obj);

    CL_GC_UNPROTECT(3);              /* obj, cell, placeholder */
    return obj;
}

/* #n# — reference a previously labeled object. */
static CL_Obj read_label_reference(int n)
{
    CL_Obj cell;
    if (read_suppress) return CL_NIL;
    cell = reader_label_cell(n);
    if (CL_NULL_P(cell)) {
        cl_reader_error(CL_ERR_PARSE, "Reference to undefined reader label #%d#", n);
        return CL_NIL;
    }
    /* A reference to a not-yet-completed label (placeholder still in place)
     * means a circular forward reference — flag it so #n= patches afterward.
     * Detect the placeholder by its CL_UNBOUND car (not content-equality
     * against NIL/NIL, which would false-positive on a real (nil . nil) value). */
    {
        CL_Obj val = cl_cdr(cell);
        if (CL_CONS_P(val)) {
            CL_Cons *vc = (CL_Cons *)CL_OBJ_TO_PTR(val);
            if (vc->car == CL_UNBOUND)
                rd_label_backrefs++;
        }
        return val;
    }
}

/*
 * Try to parse buf as a float literal.
 * Returns the float object, or CL_NIL if not a valid float token.
 *
 * Recognized forms (input already uppercased by reader):
 *   1.0  .5  1.            — decimal point
 *   1E3  1.5E-2            — E exponent (default = single-float)
 *   1.0F0  1.5F-2          — F exponent (single-float)
 *   1.0S0  1.5S-2          — S exponent (short = single-float)
 *   1.0D0  1.5D-2          — D exponent (double-float)
 *   1.0L0  1.5L-2          — L exponent (long = double-float)
 */
static CL_Obj try_parse_float(const char *buf, int len)
{
    int i, has_dot = 0, has_exp = 0, has_digit = 0;
    int exp_pos = -1, is_double = 0;
    char parse_buf[256];
    double val;
    char *endp;

    i = 0;
    if (len > 0 && (buf[0] == '+' || buf[0] == '-')) i = 1;

    for (; i < len; i++) {
        if (cl_ascii_isdigit((unsigned char)buf[i])) {
            has_digit = 1;
        } else if (buf[i] == '.' && !has_dot && !has_exp) {
            has_dot = 1;
        } else if ((buf[i] == 'E' || buf[i] == 'F' || buf[i] == 'D' ||
                    buf[i] == 'S' || buf[i] == 'L') && !has_exp && has_digit) {
            has_exp = 1;
            exp_pos = i;
            if (buf[i] == 'D' || buf[i] == 'L')
                is_double = 1;
        } else if ((buf[i] == '+' || buf[i] == '-') && has_exp && i == exp_pos + 1) {
            /* Sign after exponent marker — valid */
        } else {
            return CL_NIL;  /* Invalid character for float */
        }
    }

    /* Must have at least one digit and either a dot or exponent marker */
    if (!has_digit || (!has_dot && !has_exp))
        return CL_NIL;

    /* Build parse buffer: replace the CL exponent marker with 'E' */
    if (len >= 256) return CL_NIL;
    memcpy(parse_buf, buf, (uint32_t)len);
    parse_buf[len] = '\0';
    if (exp_pos >= 0 && parse_buf[exp_pos] != 'E')
        parse_buf[exp_pos] = 'E';

    /* Exact, FPU-independent conversion (float_dtoa.c): correctly rounded
     * directly to the target format, bit-identical on every platform.
     * strtod is unusable here — on Amiga it runs through mathieee/FPU,
     * and Vampire-class FPUs misparse literals by several ulps; the old
     * (float)strtod(...) double also double-rounded singles. */
    {
        uint64_t fb;
        if (cl_parse_float_exact(parse_buf, is_double, &fb)) {
            if (is_double) {
                double d;
                memcpy(&d, &fb, sizeof d);
                return cl_make_double_float(d);
            } else {
                uint32_t b32 = (uint32_t)fb;
                float f;
                memcpy(&f, &b32, sizeof f);
                return cl_make_single_float(f);
            }
        }
    }

    /* Fallback (unreachable for reader-validated tokens): legacy strtod. */
    val = strtod(parse_buf, &endp);
    if (endp != parse_buf + len)
        return CL_NIL;  /* strtod didn't consume entire token */

    if (is_double)
        return cl_make_double_float(val);
    return cl_make_single_float((float)val);
}

/* Read an atom (number, symbol, keyword).
 *
 * If prefix is non-NULL, the first prefix_len characters are seeded into the
 * token buffer (already-consumed characters from upstream callers).  This is
 * used by read_list when it has read a leading `.` plus a non-delimiter
 * lookahead character but cannot push both back through the stream's
 * single-slot unread buffer. */
static CL_Obj read_atom_with_prefix(const char *prefix, int prefix_len)
{
    char buf[256];
    int len = 0;
    int ch;
    int is_number = 1;
    int has_digit = 0;
    int has_escape = 0;  /* Set if | or \ escaping was used */
    int i;
    CL_Readtable *rt = cl_readtable_current();

    if (prefix && prefix_len > 0) {
        int p;
        if (prefix_len > 255)
            cl_reader_error(CL_ERR_PARSE,
                "Token prefix longer than 255 characters (symbol limit)");
        for (p = 0; p < prefix_len; p++)
            buf[len++] = (char)cl_ascii_toupper((unsigned char)prefix[p]);
    }

    /* Token cap: 255 characters.  Exceeding it must be a reader error —
     * the old bounded loop simply stopped consuming, silently splitting a
     * long symbol into two tokens (and a 300-digit number into two
     * numbers!). */
#define ATOM_CAP_CHECK() \
    do { if (len >= 255) \
        cl_reader_error(CL_ERR_PARSE, \
            "Token longer than 255 characters (symbol/number limit)"); \
    } while (0)
    for (;;) {
        ch = read_char();
        if (ch < 0) break;  /* EOF */

        /* Multiple escape: |...| — read literally until closing | */
        if (ch < CL_RT_CHARS && rt->syntax[ch] == CL_CHAR_MULTI_ESCAPE) {
            has_escape = 1;
            for (;;) {
                ch = read_char();
                if (ch < 0) {
                    cl_reader_error(CL_ERR_PARSE, "Unterminated | in symbol name");
                    return CL_NIL;
                }
                if (ch < CL_RT_CHARS && rt->syntax[ch] == CL_CHAR_MULTI_ESCAPE)
                    break;  /* closing | */
                /* Single escape inside multiple escape */
                if (ch < CL_RT_CHARS && rt->syntax[ch] == CL_CHAR_ESCAPE) {
                    ch = read_char();
                    if (ch < 0) break;
                }
                ATOM_CAP_CHECK();
                buf[len++] = (char)ch;  /* NO case conversion */
            }
            continue;
        }

        /* Single escape: \ — read next char literally */
        if (ch < CL_RT_CHARS && rt->syntax[ch] == CL_CHAR_ESCAPE) {
            has_escape = 1;
            ch = read_char();
            if (ch < 0) break;
            ATOM_CAP_CHECK();
            buf[len++] = (char)ch;  /* NO case conversion */
            continue;
        }

        if (is_delimiter(ch)) {
            unread_char(ch);
            break;
        }
        ATOM_CAP_CHECK();
        buf[len++] = (char)cl_ascii_toupper(ch);
    }
#undef ATOM_CAP_CHECK
    buf[len] = '\0';

    if (len == 0 && !has_escape) {
#ifdef DEBUG_READER
        {
            char dbg[160];
            int pk = read_char();   /* peek the char that stopped us */
            snprintf(dbg, sizeof(dbg),
                "[READER] read_token len=0: peek=%d (%c) is_delim=%d eof=%d "
                "RT_CHARS=%d syn[peek]=%d\n",
                pk, (pk >= 32 && pk < 127) ? pk : '?',
                is_delimiter(pk), eof_seen, CL_RT_CHARS,
                (pk >= 0 && pk < CL_RT_CHARS) ? rt->syntax[pk] : -1);
            platform_write_string(dbg);
            if (pk >= 0) unread_char(pk);
        }
#endif
        cl_reader_error(CL_ERR_PARSE, "Unexpected end of input");
        return CL_NIL;
    }

    /* Escaped tokens (|...|, \x) are always symbols — skip numeric parsing
     * but still honor the keyword (':foo) and pkg:sym prefixes, otherwise
     * forms like ':|abcdefg|' get interned as a CL-USER symbol named
     * ":abcdefg" instead of a keyword |abcdefg|. */
    if (has_escape) goto check_keyword;

    /* Check for number: optional sign followed by digits */
    i = 0;
    if (buf[0] == '+' || buf[0] == '-') i = 1;
    for (; i < len; i++) {
        if (cl_ascii_isdigit((unsigned char)buf[i])) {
            has_digit = 1;
        } else {
            is_number = 0;
            break;
        }
    }
    if (is_number && has_digit) {
        int neg = 0;
        int start;
        int digit_count;
        i = 0;
        if (buf[0] == '-') { neg = 1; i = 1; }
        else if (buf[0] == '+') { i = 1; }
        start = i;
        digit_count = len - start;

        /* If too many digits for fixnum (> 10 digits), go straight to bignum */
        if (digit_count <= 9) {
            long val = 0;
            for (i = start; i < len; i++) {
                val = val * 10 + (buf[i] - '0');
            }
            if (neg) val = -val;
            if (val >= CL_FIXNUM_MIN && val <= CL_FIXNUM_MAX)
                return CL_MAKE_FIXNUM((int32_t)val);
        }
        /* Overflow or large number: create bignum */
        return cl_bignum_from_string(buf + start, digit_count, neg);
    }

    /* Try float literal (decimal point or exponent marker) */
    {
        CL_Obj float_obj = try_parse_float(buf, len);
        if (!CL_NULL_P(float_obj))
            return float_obj;
    }

    /* Check for ratio literal: [sign]digits/digits (e.g. 1/2, -3/4) */
    {
        int slash_pos = -1;
        int valid_ratio = 1;
        int ri;
        int rstart = 0;

        if (buf[0] == '+' || buf[0] == '-') rstart = 1;
        /* Find exactly one slash with digits on both sides */
        for (ri = rstart; ri < len; ri++) {
            if (buf[ri] == '/') {
                if (slash_pos >= 0) { valid_ratio = 0; break; }
                slash_pos = ri;
            } else if (!cl_ascii_isdigit((unsigned char)buf[ri])) {
                valid_ratio = 0;
                break;
            }
        }
        if (valid_ratio && slash_pos > rstart && slash_pos < len - 1) {
            /* Parse numerator */
            int num_neg = (buf[0] == '-');
            int num_start = rstart;
            int num_digits = slash_pos - num_start;
            CL_Obj num_obj, den_obj;

            if (num_digits <= 9) {
                long nv = 0;
                for (ri = num_start; ri < slash_pos; ri++)
                    nv = nv * 10 + (buf[ri] - '0');
                if (num_neg) nv = -nv;
                if (nv >= CL_FIXNUM_MIN && nv <= CL_FIXNUM_MAX)
                    num_obj = CL_MAKE_FIXNUM((int32_t)nv);
                else
                    num_obj = cl_bignum_from_string(buf + num_start, num_digits, num_neg);
            } else {
                num_obj = cl_bignum_from_string(buf + num_start, num_digits, num_neg);
            }

            /* Parse denominator (always positive) */
            {
                int den_start = slash_pos + 1;
                int den_digits = len - den_start;

                CL_GC_PROTECT(num_obj);

                if (den_digits <= 9) {
                    long dv = 0;
                    for (ri = den_start; ri < len; ri++)
                        dv = dv * 10 + (buf[ri] - '0');
                    if (dv >= CL_FIXNUM_MIN && dv <= CL_FIXNUM_MAX)
                        den_obj = CL_MAKE_FIXNUM((int32_t)dv);
                    else
                        den_obj = cl_bignum_from_string(buf + den_start, den_digits, 0);
                } else {
                    den_obj = cl_bignum_from_string(buf + den_start, den_digits, 0);
                }

                CL_GC_UNPROTECT(1);
            }

            return cl_make_ratio_normalized(num_obj, den_obj);
        }
    }

check_keyword:
    /* Under suppression the token has been parsed and consumed, but CLHS
     * 2.4.8.17 is explicit: "no symbol will be constructed or interned".
     * Bail out before the keyword / package-qualified / plain-symbol paths —
     * that is also what keeps `no-such-pkg:sym` from signalling. */
    if (read_suppress) return CL_NIL;

    /* Check for keyword */
    if (buf[0] == ':') {
        return cl_intern_keyword(buf + 1, (uint32_t)(len - 1));
    }

    /* Check for package-qualified symbol (pkg:sym or pkg::sym) */
    {
        int colon_pos = -1;
        int double_colon = 0;
        for (i = 0; i < len; i++) {
            if (buf[i] == ':') {
                colon_pos = i;
                if (i + 1 < len && buf[i + 1] == ':') {
                    double_colon = 1;
                }
                break;
            }
        }
        if (colon_pos > 0) {
            char pkg_name[256];
            char sym_name[256];
            CL_Obj package;
            int sym_start = double_colon ? colon_pos + 2 : colon_pos + 1;
            int sym_len = len - sym_start;

            if (sym_len <= 0) {
                if (read_suppress) return CL_NIL;
                cl_reader_error(CL_ERR_PARSE, "Missing symbol name after package qualifier");
            }

            memcpy(pkg_name, buf, (uint32_t)colon_pos);
            pkg_name[colon_pos] = '\0';
            memcpy(sym_name, buf + sym_start, (uint32_t)sym_len);
            sym_name[sym_len] = '\0';

            package = cl_find_package(pkg_name, (uint32_t)colon_pos);
            if (CL_NULL_P(package)) {
                if (read_suppress) return CL_NIL;
                cl_reader_error(CL_ERR_PARSE, "Package %s not found", pkg_name);
            }

            if (double_colon) {
                /* pkg::sym — intern as internal symbol */
                return cl_intern_in(sym_name, (uint32_t)sym_len, package);
            } else {
                /* pkg:sym — look up external symbol only.
                 * Use cl_find_symbol_with_status so that CL:NIL (whose
                 * symbol value IS CL_NIL) is distinguished from "not found"
                 * (which also returns CL_NIL). */
                int status = 0;
                CL_Obj sym;
                /* Sampled BEFORE the lookup on purpose.  A shed package
                 * (SAVE-IMAGE :SHAKE-BINDINGS) resolves only the names
                 * referenced before the image was saved, and saying so turns
                 * an apparent typo into an actionable message — but asking
                 * afterwards would mean reading `package`, an unprotected
                 * local, across a call that allocates whenever it
                 * materialises an inherited name.  Any allocation may
                 * compact, which would leave that local stale.  Two loads
                 * here, no lock, no GC window. */
                int shed_pkg = cl_bindtab_shed_p(package);
                sym = cl_find_symbol_with_status(sym_name, (uint32_t)sym_len, package, &status);
                if (status != 2) {
                    if (read_suppress) return CL_NIL;
                    /* The hint is only true when status is 0 (no entry
                     * found anywhere): shedding drops a package's own
                     * lazy blob outright, so a name it would have held
                     * turns into a clean miss, never a status 1
                     * (:INTERNAL) or 3 (:INHERITED) hit.  Attaching the
                     * hint to those would blame shedding for a miss it
                     * cannot cause — e.g. a name materialised through a
                     * used package's still-lazy table (:INHERITED). */
                    if (shed_pkg && status == 0)
                        cl_reader_error(CL_ERR_PARSE,
                                 "Symbol %s not exported from %s - that package's "
                                 "binding table was shed by SAVE-IMAGE "
                                 ":SHAKE-BINDINGS, so only names referenced before "
                                 "the image was saved exist in it",
                                 sym_name, pkg_name);
                    cl_reader_error(CL_ERR_PARSE, "Symbol %s not exported from %s",
                             sym_name, pkg_name);
                }
                return sym;
            }
        }
    }

    /* Check for NIL */
    if (len == 3 && buf[0] == 'N' && buf[1] == 'I' && buf[2] == 'L') {
        return CL_NIL;
    }

    /* Regular symbol */
    return cl_intern(buf, (uint32_t)len);
}

/* Per-thread stash for read_string's heap-grown buffer (see below).  Indexed
 * by CT->id, which is always < CL_MAX_THREADS (main thread is 0; mp:make-thread
 * assigns from the same range — see thread.c/builtins_thread.c). */
static char *rdstr_orphan_buf[CL_MAX_THREADS];

/* Read a string literal.  The buffer GROWS: real-world string literals
 * exceed any fixed cap (log4cl's pattern-layout docstring is >4KB), the old
 * fixed buffer silently truncated them, and erroring at a cap broke loading
 * conforming libraries — CLHS puts no length limit on string literals.
 * Growth is geometric into platform memory.
 *
 * read_char() can itself longjmp out from underneath this loop — e.g.
 * stream_raise_timeout() on a socket stream whose read deadline elapses
 * mid-literal — bypassing every explicit free below.  So the heap buffer is
 * also mirrored into a per-thread stash (rdstr_orphan_buf) on every grow;
 * the next read_string call on this thread reclaims (frees) any buffer left
 * there by a longjmp that skipped the normal free, bounding the leak to at
 * most one stale buffer per thread instead of one per aborted read. */
static CL_Obj read_string(void)
{
    char stack_buf[4096]; /* UTF-8 encoded; covers the common case */
    char *buf = stack_buf;
    uint32_t cap = (uint32_t)sizeof(stack_buf);
    uint32_t len = 0;
    int ch;
    CL_Obj result;

    if (rdstr_orphan_buf[CT->id]) {
        platform_free(rdstr_orphan_buf[CT->id]);
        rdstr_orphan_buf[CT->id] = NULL;
    }

#define RDSTR_ENSURE(n) \
    do { \
        if (len + (uint32_t)(n) + 1 > cap) { \
            uint32_t newcap; \
            char *nb_; \
            if (cap > UINT32_MAX / 2) { \
                if (buf != stack_buf) platform_free(buf); \
                rdstr_orphan_buf[CT->id] = NULL; \
                cl_reader_error(CL_ERR_STORAGE, \
                    "String literal too large (over %u bytes)", \
                    (unsigned)cap); \
            } \
            newcap = cap * 2; \
            while (len + (uint32_t)(n) + 1 > newcap) { \
                if (newcap > UINT32_MAX / 2) { \
                    if (buf != stack_buf) platform_free(buf); \
                    rdstr_orphan_buf[CT->id] = NULL; \
                    cl_reader_error(CL_ERR_STORAGE, \
                        "String literal too large (over %u bytes)", \
                        (unsigned)newcap); \
                } \
                newcap *= 2; \
            } \
            nb_ = (char *)platform_alloc(newcap); \
            if (!nb_) { \
                if (buf != stack_buf) platform_free(buf); \
                rdstr_orphan_buf[CT->id] = NULL; \
                cl_reader_error(CL_ERR_STORAGE, \
                    "String literal: out of memory at %u bytes", \
                    (unsigned)len); \
            } \
            memcpy(nb_, buf, len); \
            if (buf != stack_buf) platform_free(buf); \
            buf = nb_; \
            cap = newcap; \
            rdstr_orphan_buf[CT->id] = buf; \
        } \
    } while (0)

    for (;;) {
        ch = read_char();
        if (ch < 0) {
            if (buf != stack_buf) platform_free(buf);
            rdstr_orphan_buf[CT->id] = NULL;
            cl_reader_error(CL_ERR_PARSE, "Unterminated string");
            return CL_NIL;
        }
        if (ch == '"') break;
        if (ch == '\\') {
            ch = read_char();
            if (ch < 0) {
                if (buf != stack_buf) platform_free(buf);
                rdstr_orphan_buf[CT->id] = NULL;
                cl_reader_error(CL_ERR_PARSE, "Unterminated string escape");
                return CL_NIL;
            }
            /* CLHS 2.4.5: backslash is a single escape — the next character
             * is taken exactly as itself, so "a\nb" is the three characters
             * a n b.  CL has no C-style \n / \t escapes; a newline goes into
             * a string as a literal newline or via FORMAT's ~%. */
        }
#ifdef CL_WIDE_STRINGS
        if (ch > 0x7F) {
            char tmp[4];
            int nb = cl_utf8_encode(ch, tmp);
            int j;
            RDSTR_ENSURE(nb);
            for (j = 0; j < nb; j++)
                buf[len++] = tmp[j];
        } else
#endif
        {
            RDSTR_ENSURE(1);
            buf[len++] = (char)ch;
        }
    }
    buf[len] = '\0';
#undef RDSTR_ENSURE
#ifdef CL_WIDE_STRINGS
    result = cl_utf8_to_cl_string(buf, len);
#else
    result = cl_make_string(buf, len);
#endif
    if (buf != stack_buf) {
        platform_free(buf);
        rdstr_orphan_buf[CT->id] = NULL;
    }
    return result;
}

/* Read a standard "..." string body from STREAM, assuming the opening quote
 * has already been consumed — i.e. the standard reader-macro protocol, where a
 * macro-character function is called with (stream macro-char).  Exposed so
 * GET-MACRO-CHARACTER can hand callers a real callable for the built-in #\"
 * reader, which amilisp otherwise handles inline in C (so macro_fn[#\"] is
 * NIL).  Libraries such as pythonic-string-reader grab the standard #\" reader
 * and re-dispatch to it; without this they would funcall NIL. */
CL_Obj cl_read_standard_string_from_stream(CL_Obj stream)
{
    CL_Obj saved = reader_stream;
    CL_Obj result;
    /* read_string allocates (the result string); GC-protect the saved stream
     * offset so the restore below is still valid after a compaction. */
    CL_GC_PROTECT(saved);
    reader_stream = stream;
    result = read_string();
    reader_stream = saved;
    CL_GC_UNPROTECT(1);
    return result;
}

int cl_srcloc_lookup(CL_Obj cons_obj)
{
    uint32_t idx = CL_SRCLOC_INDEX(cons_obj);
    if (cl_srcloc_table[idx].cons_obj == cons_obj)
        return (int)cl_srcloc_table[idx].line;
    return 0;
}

/* Read a list */
static CL_Obj read_list(void)
{
    int start_line = current_line();
    CL_Obj head = CL_NIL;
    CL_Obj tail = CL_NIL;
    CL_Obj elem;

    CL_GC_PROTECT(head);
    CL_GC_PROTECT(tail);

    for (;;) {
        skip_whitespace();
        if (eof_seen) {
            CL_GC_UNPROTECT(2);
            cl_reader_error(CL_ERR_PARSE, "Unterminated list");
            return CL_NIL;
        }

        {
            int ch = read_char();
            if (ch == ')') {
                CL_GC_UNPROTECT(2);
                return head;
            }

            /* Dotted pair */
            if (ch == '.') {
                int next = read_char();
                if (is_delimiter(next)) {
                    unread_char(next);
                    /* Read cdr of dotted pair — skip past #+/#- voids (R6),
                     * else the 0x06 skip sentinel becomes the list's cdr */
                    do { elem = read_expr(); }
                    while (elem == CL_READER_SKIP && !eof_seen);
                    if (!CL_NULL_P(tail)) {
                        ((CL_Cons *)CL_OBJ_TO_PTR(tail))->cdr = elem;
                    }
                    skip_whitespace();
                    ch = read_char();
                    if (ch != ')') {
                        CL_GC_UNPROTECT(2);
                        cl_reader_error(CL_ERR_PARSE, "Expected ')' after dotted pair");
                    }
                    CL_GC_UNPROTECT(2);
                    return head;
                }
                /* Not a dotted-pair `.` — the token starts with `.` followed
                 * by a non-delimiter character (e.g. `.5`, `.foo`).  The
                 * stream only has a single-slot unread buffer, so we cannot
                 * push back both `ch` and `next`.  Push back only `next` and
                 * seed the atom reader with the consumed `.`. */
                unread_char(next);
                elem = read_atom_with_prefix(".", 1);
                goto have_elem;
            } else {
                unread_char(ch);
            }
        }

        elem = read_expr();
    have_elem:
        if (elem == CL_READER_SKIP) continue;  /* #+ / #- skipped form */

        {
            CL_Obj cell = cl_cons(elem, CL_NIL);
            if (CL_NULL_P(head)) {
                head = cell;
                srcloc_record(head, start_line);
            } else {
                ((CL_Cons *)CL_OBJ_TO_PTR(tail))->cdr = cell;
            }
            tail = cell;
        }
    }
}

/* #*0110 => simple bit vector.  Separate CL_NOINLINE function so the 4KB
 * scan buffer stays out of read_expr's frame — read_expr recurses once per
 * nesting level of the source, and on small fixed Amiga shell stacks a
 * multi-KB frame per level overflows on deeply nested files. */
static CL_NOINLINE CL_Obj read_bit_vector(void)
{
    char bits[4096];
    int blen = 0;
    uint32_t bi;
    CL_Obj bvobj;
    CL_BitVector *bv;
    for (;;) {
        int c2 = read_char();
        if (c2 == '0' || c2 == '1') {
            if (blen >= 4095)
                cl_reader_error(CL_ERR_PARSE,
                    "Bit-vector literal #* longer than 4095 bits");
            bits[blen++] = (char)c2;
        } else {
            if (c2 >= 0) unread_char(c2);
            break;
        }
    }
    bvobj = cl_make_bit_vector((uint32_t)blen);
    bv = (CL_BitVector *)CL_OBJ_TO_PTR(bvobj);
    for (bi = 0; bi < (uint32_t)blen; bi++) {
        if (bits[bi] == '1')
            bv->data[bi / 32] |= (1u << (bi % 32));
    }
    return bvobj;
}

/* Read a single expression */
static CL_Obj read_expr(void)
{
    int ch;
    CL_Obj obj;

    /* The reader recurses once per nesting level, consuming both C stack
     * (on AmigaOS the shell stack is small and fixed; overflow corrupts
     * memory silently) and GC-root-stack slots (hard abort at the
     * ceiling).  Fail with a clean, actionable error instead. */
    cl_check_recursion_guards("read");

    skip_whitespace();
    if (eof_seen) return CL_NIL;

    ch = read_char();
    if (ch < 0) { eof_seen = 1; return CL_NIL; }

    /* Check readtable for user-defined macro function */
    if (ch >= 0 && ch < CL_RT_CHARS) {
        CL_Readtable *rt = cl_readtable_current();
        uint8_t syn = rt->syntax[ch];
        if ((syn == CL_CHAR_TERM_MACRO || syn == CL_CHAR_NONTERM_MACRO) &&
            !CL_NULL_P(rt->macro_fn[ch])) {
            /* User-defined macro: call (fn stream char) */
            CL_Obj args[2];
            args[0] = reader_stream;
            args[1] = CL_MAKE_CHAR(ch);
            return cl_vm_apply(rt->macro_fn[ch], args, 2);
        }
    }

    /* Built-in macro dispatch */
    switch (ch) {
    case '(':
        return read_list();

    /* NOTE (audit tier 4, R6 / CLHS 2.4.8.17): a false #+/#- consumes its
     * feature expression AND the following form, yielding nothing — so
     * every "read one sub-form" site below must loop past CL_READER_SKIP
     * (e.g. '#+nope foo bar must read as 'BAR), or the internal 0x06
     * sentinel leaks into user structure. */
    case '\'': /* quote */
        do { obj = read_expr(); } while (obj == CL_READER_SKIP && !eof_seen);
        return cl_list2(SYM_QUOTE, obj);

    case '`': /* quasiquote */
        do { obj = read_expr(); } while (obj == CL_READER_SKIP && !eof_seen);
        return cl_list2(SYM_QUASIQUOTE, obj);

    case ',': /* unquote / unquote-splicing */
        ch = read_char();
        /* ,. is the destructive-splice variant of ,@ — ANSI CL permits
         * implementations to expand it identically to ,@ (the
         * difference is only whether APPEND or NCONC is used in the
         * expansion, and iterate relies on the form being accepted at
         * read time).  Treat them the same here. */
        if (ch == '@' || ch == '.') {
            do { obj = read_expr(); } while (obj == CL_READER_SKIP && !eof_seen);
            return cl_list2(SYM_UNQUOTE_SPLICING, obj);
        }
        unread_char(ch);
        do { obj = read_expr(); } while (obj == CL_READER_SKIP && !eof_seen);
        return cl_list2(SYM_UNQUOTE, obj);

    case '#': {
        int sub_ch = read_char();

        /* Check readtable for user-defined dispatch sub-character */
        if (sub_ch >= 0 && sub_ch < CL_RT_CHARS) {
            CL_Readtable *rt = cl_readtable_current();
            if (!CL_NULL_P(rt->dispatch_fn[sub_ch])) {
                /* User dispatch macro: call (fn stream sub-char nil) */
                CL_Obj args[3];
                args[0] = reader_stream;
                args[1] = CL_MAKE_CHAR(sub_ch);
                args[2] = CL_NIL; /* no numeric arg */
                return cl_vm_apply(rt->dispatch_fn[sub_ch], args, 3);
            }
        }

        /* Built-in dispatch macros */
        ch = sub_ch;
        if (ch == '|') {
            /* #|...|# block comment (nestable per CL spec) */
            int depth = 1;
            int prev = -1;
            while (depth > 0) {
                int c2 = read_char();
                if (c2 < 0) {
                    cl_reader_error(CL_ERR_PARSE, "Unterminated block comment #|...|#");
                    return CL_NIL;
                }
                if (prev == '#' && c2 == '|') {
                    depth++;
                    prev = -1;  /* reset to avoid #|| matching twice */
                    continue;
                }
                if (prev == '|' && c2 == '#') {
                    depth--;
                    prev = -1;
                    continue;
                }
                prev = c2;
            }
            return CL_READER_SKIP;
        }
        if (ch == '\'') {
            /* #'foo => (FUNCTION foo) — skip past #+/#- voids (R6) */
            do { obj = read_expr(); } while (obj == CL_READER_SKIP && !eof_seen);
            return cl_list2(SYM_FUNCTION, obj);
        }
        if (ch == '\\') {
            /* #\x => character literal */
            ch = read_char();
            if (ch < 0) {
                cl_reader_error(CL_ERR_PARSE, "Unexpected EOF in character literal");
                return CL_NIL;
            }
            /* Check for named characters */
            if (cl_ascii_isalpha(ch)) {
                char name[32];
                int nlen = 0;
                name[nlen++] = (char)ch;
                for (;;) {
                    ch = read_char();
                    if (is_delimiter(ch)) { unread_char(ch); break; }
                    if (nlen >= 31)
                        cl_reader_error(CL_ERR_PARSE,
                            "Character name after #\\ longer than 31 characters");
                    name[nlen++] = (char)ch;
                }
                name[nlen] = '\0';
                if (nlen == 1) return CL_MAKE_CHAR(name[0]);
                /* Named characters — CL standard */
                if (strcasecmp(name, "Null") == 0) return CL_MAKE_CHAR('\0');
                if (strcasecmp(name, "Nul") == 0) return CL_MAKE_CHAR('\0');
                if (strcasecmp(name, "Space") == 0) return CL_MAKE_CHAR(' ');
                if (strcasecmp(name, "Newline") == 0) return CL_MAKE_CHAR('\n');
                if (strcasecmp(name, "Linefeed") == 0) return CL_MAKE_CHAR('\n');
                if (strcasecmp(name, "Tab") == 0) return CL_MAKE_CHAR('\t');
                if (strcasecmp(name, "Return") == 0) return CL_MAKE_CHAR('\r');
                if (strcasecmp(name, "Backspace") == 0) return CL_MAKE_CHAR('\b');
                if (strcasecmp(name, "Page") == 0) return CL_MAKE_CHAR('\f');
                if (strcasecmp(name, "Rubout") == 0) return CL_MAKE_CHAR(0x7F);
                /* Extended character names (common across implementations) */
                if (strcasecmp(name, "Escape") == 0 ||
                    strcasecmp(name, "Esc") == 0) return CL_MAKE_CHAR(0x1B);
                if (strcasecmp(name, "Vt") == 0) return CL_MAKE_CHAR(0x0B);
                if (strcasecmp(name, "Bell") == 0 ||
                    strcasecmp(name, "Bel") == 0) return CL_MAKE_CHAR(0x07);
                if (strcasecmp(name, "Delete") == 0 ||
                    strcasecmp(name, "Del") == 0) return CL_MAKE_CHAR(0x7F);
                if (strcasecmp(name, "Soh") == 0) return CL_MAKE_CHAR(0x01);
                if (strcasecmp(name, "Stx") == 0) return CL_MAKE_CHAR(0x02);
                if (strcasecmp(name, "Etx") == 0) return CL_MAKE_CHAR(0x03);
                if (strcasecmp(name, "Eot") == 0) return CL_MAKE_CHAR(0x04);
                if (strcasecmp(name, "Enq") == 0) return CL_MAKE_CHAR(0x05);
                if (strcasecmp(name, "Ack") == 0) return CL_MAKE_CHAR(0x06);
                if (strcasecmp(name, "So") == 0) return CL_MAKE_CHAR(0x0E);
                if (strcasecmp(name, "Si") == 0) return CL_MAKE_CHAR(0x0F);
                if (strcasecmp(name, "Dle") == 0) return CL_MAKE_CHAR(0x10);
                if (strcasecmp(name, "Dc1") == 0) return CL_MAKE_CHAR(0x11);
                if (strcasecmp(name, "Dc2") == 0) return CL_MAKE_CHAR(0x12);
                if (strcasecmp(name, "Dc3") == 0) return CL_MAKE_CHAR(0x13);
                if (strcasecmp(name, "Dc4") == 0) return CL_MAKE_CHAR(0x14);
                if (strcasecmp(name, "Nak") == 0) return CL_MAKE_CHAR(0x15);
                if (strcasecmp(name, "Syn") == 0) return CL_MAKE_CHAR(0x16);
                if (strcasecmp(name, "Etb") == 0) return CL_MAKE_CHAR(0x17);
                if (strcasecmp(name, "Can") == 0) return CL_MAKE_CHAR(0x18);
                if (strcasecmp(name, "Em") == 0) return CL_MAKE_CHAR(0x19);
                if (strcasecmp(name, "Sub") == 0) return CL_MAKE_CHAR(0x1A);
                if (strcasecmp(name, "Fs") == 0) return CL_MAKE_CHAR(0x1C);
                if (strcasecmp(name, "Gs") == 0) return CL_MAKE_CHAR(0x1D);
                if (strcasecmp(name, "Rs") == 0) return CL_MAKE_CHAR(0x1E);
                if (strcasecmp(name, "Us") == 0) return CL_MAKE_CHAR(0x1F);
                /* Unicode-named whitespace characters */
                if (strcasecmp(name, "No-Break_Space") == 0 ||
                    strcasecmp(name, "No-break_space") == 0 ||
                    strcasecmp(name, "NO-BREAK_SPACE") == 0) return CL_MAKE_CHAR(0xA0);
                if (strcasecmp(name, "Ideographic_Space") == 0 ||
                    strcasecmp(name, "Ideographic_space") == 0 ||
                    strcasecmp(name, "IDEOGRAPHIC_SPACE") == 0) return CL_MAKE_CHAR(0x3000);
                /* U+FFFD REPLACEMENT CHARACTER — used by flexi-streams
                   (flex:*substitution-char*) via clack's hunchentoot handler. */
                if (strcasecmp(name, "Replacement_Character") == 0) return CL_MAKE_CHAR(0xFFFD);
                /* Hex codepoint: #\U+XXXX or #\u+XXXX */
                if ((name[0] == 'U' || name[0] == 'u') && name[1] == '+') {
                    char *endp;
                    unsigned long cp = strtoul(name + 2, &endp, 16);
                    if (*endp == '\0' && cp <= 0x10FFFF)
                        return CL_MAKE_CHAR((int)cp);
                }
                if (read_suppress) return CL_NIL;
                cl_reader_error(CL_ERR_PARSE, "Unknown character name: %s", name);
                return CL_NIL;
            }
            return CL_MAKE_CHAR(ch);
        }
        if (ch == ':') {
            /* #:sym — uninterned symbol.  Per CLHS 2.4.8.5, within a
             * single call to READ, all #:foo with the same name must
             * denote the *same* uninterned symbol.  We track this via
             * CT->rd_uninterned (alist of (name-string . symbol)),
             * saved/restored around each cl_read_from_stream call. */
            char sym_buf[256];
            int sym_len = 0;
            int ch2;
            int sym_has_escape = 0;
            CL_Obj name_str, new_sym, cell;
            CL_Readtable *rt = cl_readtable_current();
            for (;;) {
                ch2 = read_char();
                if (ch2 < 0) break;  /* EOF */
                /* Multiple escape: |...| — read literally with no
                 * case folding (CLHS 2.4.8.5 / 2.3.4).  An empty pair
                 * |...| is a valid zero-length name segment. */
                if (ch2 < CL_RT_CHARS &&
                    rt->syntax[ch2] == CL_CHAR_MULTI_ESCAPE) {
                    sym_has_escape = 1;
                    for (;;) {
                        ch2 = read_char();
                        if (ch2 < 0) {
                            cl_reader_error(CL_ERR_PARSE,
                                "Unterminated | in symbol name");
                            return CL_NIL;
                        }
                        if (ch2 < CL_RT_CHARS &&
                            rt->syntax[ch2] == CL_CHAR_MULTI_ESCAPE)
                            break;  /* closing | */
                        if (ch2 < CL_RT_CHARS &&
                            rt->syntax[ch2] == CL_CHAR_ESCAPE) {
                            ch2 = read_char();
                            if (ch2 < 0) break;
                        }
                        if (sym_len >= 255)
                            cl_reader_error(CL_ERR_PARSE,
                                "#: symbol name longer than 255 characters");
                        sym_buf[sym_len++] = (char)ch2;
                    }
                    continue;
                }
                /* Single escape: \ — next char literal, no case folding. */
                if (ch2 < CL_RT_CHARS && rt->syntax[ch2] == CL_CHAR_ESCAPE) {
                    sym_has_escape = 1;
                    ch2 = read_char();
                    if (ch2 < 0) break;
                    if (sym_len >= 255)
                        cl_reader_error(CL_ERR_PARSE,
                            "#: symbol name longer than 255 characters");
                    sym_buf[sym_len++] = (char)ch2;
                    continue;
                }
                if (is_delimiter(ch2)) { unread_char(ch2); break; }
                if (sym_len >= 255)
                    cl_reader_error(CL_ERR_PARSE,
                        "#: symbol name longer than 255 characters");
                sym_buf[sym_len++] = (char)cl_ascii_toupper(ch2);
            }
            sym_buf[sym_len] = '\0';
            if (sym_len == 0 && !sym_has_escape) {
                if (read_suppress) return CL_NIL;
                cl_reader_error(CL_ERR_PARSE, "Missing symbol name after #:");
            }
            /* Look up by name in the per-read alist. */
            {
                CL_Obj p = rd_uninterned;
                while (!CL_NULL_P(p)) {
                    CL_Obj pair = cl_car(p);
                    CL_Obj nm   = cl_car(pair);
                    if (CL_ANY_STRING_P(nm) &&
                        cl_string_length(nm) == (uint32_t)sym_len) {
                        uint32_t i;
                        int match = 1;
                        for (i = 0; i < (uint32_t)sym_len; i++) {
                            if ((char)cl_string_char_at(nm, i) != sym_buf[i]) {
                                match = 0; break;
                            }
                        }
                        if (match) return cl_cdr(pair);
                    }
                    p = cl_cdr(p);
                }
            }
            name_str = cl_make_string(sym_buf, (uint32_t)sym_len);
            CL_GC_PROTECT(name_str);
            new_sym = cl_make_uninterned_symbol(name_str);
            CL_GC_PROTECT(new_sym);
            cell = cl_cons(name_str, new_sym);
            CL_GC_PROTECT(cell);
            rd_uninterned = cl_cons(cell, rd_uninterned);
            CL_GC_UNPROTECT(3);
            return new_sym;
        }
        if (ch == '+' || ch == '-') {
            /* #+ / #- feature conditionals.
             * Per CL spec, feature expr is read with *package* = KEYWORD */
            CL_Obj saved_pkg = cl_current_package;
            CL_Obj feat_expr;
            int present;
            /* GC SAFETY: read_expr allocates and can compact — without a
             * root the restore below writes a stale package offset into the
             * cl_current_package global (the *PACKAGE*-clobber class). */
            CL_GC_PROTECT(saved_pkg);
            cl_current_package = cl_package_keyword;
            feat_expr = read_expr();
            cl_current_package = saved_pkg;
            CL_GC_UNPROTECT(1);
            present = eval_feature_expr(feat_expr);
            if (ch == '-') present = !present;
            if (present) {
                return read_expr();
            } else {
                skip_form();
                return CL_READER_SKIP;
            }
        }
        if (ch == '*') {
            /* #*0110 => simple bit vector (buffer hoisted off this frame) */
            return read_bit_vector();
        }
        if (ch == 'p' || ch == 'P') {
            /* #P"..." => pathname literal */
            CL_Obj path_str;
            extern CL_Obj cl_parse_namestring(const char *str, uint32_t len);
            skip_whitespace();
            ch = read_char();
            if (ch != '"') {
                if (read_suppress) { unread_char(ch); return CL_NIL; }
                cl_reader_error(CL_ERR_PARSE, "#P must be followed by a string");
                return CL_NIL;
            }
            path_str = read_string();
            {
                CL_String *s = (CL_String *)CL_OBJ_TO_PTR(path_str);
                return cl_parse_namestring(s->data, s->length);
            }
        }
        if (ch == 'c' || ch == 'C') {
            /* #C(real imag) => complex number */
            CL_Obj parts;
            if (read_suppress) {
                skip_whitespace();
                ch = read_char();
                if (ch == '(') { read_list(); }
                return CL_NIL;
            }
            skip_whitespace();
            ch = read_char();
            if (ch != '(') {
                cl_reader_error(CL_ERR_PARSE, "#C must be followed by (real imag)");
                return CL_NIL;
            }
            parts = read_list();
            if (CL_CONS_P(parts) && CL_CONS_P(cl_cdr(parts)) &&
                CL_NULL_P(cl_cdr(cl_cdr(parts)))) {
                CL_Obj real = cl_car(parts);
                CL_Obj imag = cl_car(cl_cdr(parts));
                int real_dbl, imag_dbl, want_double;
                if (!CL_REALP(real) || !CL_REALP(imag))
                    cl_reader_error(CL_ERR_PARSE, "#C components must be real numbers");
                /* Apply float contagion (CLHS 12.1.5.3): if either part is
                 * float, both must be the same float kind. Otherwise, an
                 * integer-zero imag part collapses to the real itself. */
                real_dbl = CL_DOUBLE_FLOAT_P(real);
                imag_dbl = CL_DOUBLE_FLOAT_P(imag);
                if (CL_FLOATP(real) || CL_FLOATP(imag)) {
                    want_double = real_dbl || imag_dbl;
                    CL_GC_PROTECT(real); CL_GC_PROTECT(imag);
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
                } else if (CL_FIXNUM_P(imag) && CL_FIXNUM_VAL(imag) == 0) {
                    /* (#C(x 0)) where both rational → return real */
                    return real;
                }
                return cl_make_complex(real, imag);
            }
            cl_reader_error(CL_ERR_PARSE, "#C requires exactly two elements: (real imag)");
            return CL_NIL;
        }
        if (ch == 's' || ch == 'S') {
            /* #S(type slot value slot value ...) => structure literal
             * (CLHS 2.4.8.13).  The type and slot/value plist are read as
             * objects, then the structure is built at read time by calling
             * the runtime helper %READ-EVAL-STRUCT, which locates the
             * default keyword constructor MAKE-<type> and applies it. */
            CL_Obj spec, quoted, form, helper_sym, bytecode, result;
            skip_whitespace();
            ch = read_char();
            if (ch != '(') {
                if (read_suppress) { if (ch >= 0) unread_char(ch); return CL_NIL; }
                cl_reader_error(CL_ERR_PARSE,
                    "#S must be followed by (type slot value ...)");
                return CL_NIL;
            }
            spec = read_list();          /* (type slot value ...) */
            if (read_suppress) return CL_NIL;
            CL_GC_PROTECT(spec);
            /* Build (%READ-EVAL-STRUCT (QUOTE spec)) and evaluate it. */
            quoted = cl_cons(spec, CL_NIL);                /* (spec) */
            CL_GC_PROTECT(quoted);
            quoted = cl_cons(SYM_QUOTE, quoted);           /* (QUOTE spec) */
            CL_GC_PROTECT(quoted);
            form = cl_cons(quoted, CL_NIL);                /* ((QUOTE spec)) */
            CL_GC_PROTECT(form);
            /* The helper lives in the AMILISP package (see boot.lisp); intern
             * there so the same symbol is found whether boot was loaded from
             * source or FASL (the current package at boot time is not fixed). */
            helper_sym = cl_intern_in("%READ-EVAL-STRUCT", 17, cl_package_amilisp);
            CL_GC_PROTECT(helper_sym);         /* protect across cl_cons below */
            form = cl_cons(helper_sym, form);  /* (%READ-EVAL-STRUCT (QUOTE spec)) */
            CL_GC_PROTECT(form);
            bytecode = cl_compile(form);
            if (!CL_NULL_P(bytecode))
                result = cl_vm_eval(bytecode);
            else
                result = CL_NIL;
            CL_GC_UNPROTECT(6);
            return result;
        }
        if (ch == '(') {
            /* #(e1 e2 ...) => simple vector */
            CL_Obj elems = CL_NIL;
            CL_Obj tail = CL_NIL;
            uint32_t count = 0;
            uint32_t i;
            CL_Obj vec;
            CL_Vector *v;
            CL_Obj elem;

            CL_GC_PROTECT(elems);
            CL_GC_PROTECT(tail);

            for (;;) {
                skip_whitespace();
                if (eof_seen) {
                    CL_GC_UNPROTECT(2);
                    cl_reader_error(CL_ERR_PARSE, "Unterminated #( vector literal");
                    return CL_NIL;
                }
                {
                    int c2 = read_char();
                    if (c2 == ')') break;
                    unread_char(c2);
                }
                elem = read_expr();
                if (elem == CL_READER_SKIP) continue;
                {
                    CL_Obj cell = cl_cons(elem, CL_NIL);
                    if (CL_NULL_P(elems)) {
                        elems = cell;
                    } else {
                        ((CL_Cons *)CL_OBJ_TO_PTR(tail))->cdr = cell;
                    }
                    tail = cell;
                    count++;
                }
            }

            vec = cl_make_vector(count);
            CL_GC_UNPROTECT(2);
            v = (CL_Vector *)CL_OBJ_TO_PTR(vec);
            for (i = 0; i < count; i++) {
                cl_vector_data(v)[i] = cl_car(elems);
                elems = cl_cdr(elems);
            }
            return vec;
        }
        if (ch == '.') {
            /* #. read-time eval */
            CL_Obj form;
            CL_Obj bytecode, result;
            CL_Symbol *re_sym;

            do { form = read_expr(); } while (form == CL_READER_SKIP && !eof_seen);

            if (read_suppress) return CL_NIL;

            /* Check *read-eval* */
            (void)re_sym;
            if (CL_NULL_P(cl_symbol_value(SYM_STAR_READ_EVAL)))
                cl_reader_error(CL_ERR_GENERAL, "#. disabled: *READ-EVAL* is NIL");

            CL_GC_PROTECT(form);
            bytecode = cl_compile(form);
            if (!CL_NULL_P(bytecode))
                result = cl_vm_eval(bytecode);
            else
                result = CL_NIL;
            CL_GC_UNPROTECT(1);
            return result;
        }
        if (ch == 'X' || ch == 'x') {
            return read_radix_number(16);
        }
        if (ch == 'B' || ch == 'b') {
            return read_radix_number(2);
        }
        if (ch == 'O' || ch == 'o') {
            return read_radix_number(8);
        }
        /* #nR — arbitrary radix, #nA — multi-dimensional array */
        if (ch >= '0' && ch <= '9') {
            int num_val = ch - '0';
            while (1) {
                ch = read_char();
                if (ch == 'R' || ch == 'r') break;
                if (ch == 'A' || ch == 'a') {
                    /* #nA(...) — n-dimensional array */
                    int rank = num_val;
                    CL_Obj contents;
                    uint32_t dims[8];
                    uint32_t total = 1;
                    int d;
                    CL_Obj arr;
                    CL_Vector *vp;
                    CL_Obj flat_elems = CL_NIL;
                    CL_Obj flat_tail = CL_NIL;

                    /* skip past #+/#- voids before the contents form (R6) */
                    do { contents = read_expr(); }
                    while (contents == CL_READER_SKIP && !eof_seen);

                    if (read_suppress) return CL_NIL;
                    if (rank < 0 || rank > 8) {
                        cl_reader_error(CL_ERR_PARSE, "#%dA: rank must be 0-8", rank);
                        return CL_NIL;
                    }

                    CL_GC_PROTECT(contents);
                    CL_GC_PROTECT(flat_elems);
                    CL_GC_PROTECT(flat_tail);

                    if (rank == 0) {
                        /* #0A datum — 0-dimensional array containing datum */
                        arr = cl_make_array(1, 0, NULL, 0, CL_NO_FILL_POINTER);
                        vp = (CL_Vector *)CL_OBJ_TO_PTR(arr);
                        cl_vector_data(vp)[0] = contents;
                        CL_GC_UNPROTECT(3);
                        return arr;
                    }

                    /* Derive dimensions from nested list structure */
                    {
                        CL_Obj probe = contents;
                        for (d = 0; d < rank; d++) {
                            if (CL_CONS_P(probe)) {
                                uint32_t len = 0;
                                CL_Obj p = probe;
                                while (CL_CONS_P(p)) { len++; p = cl_cdr(p); }
                                dims[d] = len;
                                probe = cl_car(probe);
                            } else {
                                dims[d] = 0;
                            }
                        }
                    }
                    for (d = 0; d < rank; d++) total *= dims[d];

                    /* Recursively flatten nested lists into flat_elems */
                    {
                        /* Use a simple iterative approach with a work stack.
                         * GC SAFETY (audit tier 4, R1): every cl_cons below
                         * can compact — the per-level cursors work[0..7] and
                         * the inner list cursor must be registered roots
                         * (rank is capped at 8 above), or the walk continues
                         * through pre-compaction offsets after the first
                         * element is consed. */
                        CL_Obj work[8]; /* One per dimension level */
                        CL_Obj lst = CL_NIL;
                        int level = 0;
                        uint32_t fi = 0;
                        int wi;
                        for (wi = 1; wi < 8; wi++) work[wi] = CL_NIL;
                        work[0] = contents;
                        CL_GC_PROTECT(work[0]);
                        CL_GC_PROTECT(work[1]);
                        CL_GC_PROTECT(work[2]);
                        CL_GC_PROTECT(work[3]);
                        CL_GC_PROTECT(work[4]);
                        CL_GC_PROTECT(work[5]);
                        CL_GC_PROTECT(work[6]);
                        CL_GC_PROTECT(work[7]);
                        CL_GC_PROTECT(lst);
                        while (fi < total) {
                            if (level == rank - 1) {
                                /* Innermost: collect elements from list */
                                lst = work[level];
                                while (CL_CONS_P(lst) && fi < total) {
                                    CL_Obj cell = cl_cons(cl_car(lst), CL_NIL);
                                    if (CL_NULL_P(flat_elems)) {
                                        flat_elems = cell;
                                    } else {
                                        ((CL_Cons *)CL_OBJ_TO_PTR(flat_tail))->cdr = cell;
                                    }
                                    flat_tail = cell;
                                    fi++;
                                    lst = cl_cdr(lst);
                                }
                                /* Go back up */
                                level--;
                                if (level < 0) break;
                                work[level] = cl_cdr(work[level]);
                            } else {
                                /* Descend into sublists */
                                if (CL_CONS_P(work[level])) {
                                    work[level + 1] = cl_car(work[level]);
                                    level++;
                                } else {
                                    level--;
                                    if (level < 0) break;
                                    work[level] = cl_cdr(work[level]);
                                }
                            }
                        }
                        CL_GC_UNPROTECT(9); /* work[0..7], lst */
                    }

                    arr = cl_make_array(total, (uint8_t)rank, dims, 0,
                                        CL_NO_FILL_POINTER);
                    CL_GC_UNPROTECT(3);
                    vp = (CL_Vector *)CL_OBJ_TO_PTR(arr);
                    {
                        uint32_t i;
                        CL_Obj *data = cl_vector_data(vp);
                        CL_Obj p = flat_elems;
                        for (i = 0; i < total; i++) {
                            if (CL_CONS_P(p)) {
                                data[i] = cl_car(p);
                                p = cl_cdr(p);
                            } else {
                                data[i] = CL_NIL;
                            }
                        }
                    }
                    return arr;
                }
                if (ch == '=') {
                    /* #n= — label the following object (CLHS 2.4.8.15) */
                    return read_labeled_object(num_val);
                }
                if (ch == '#') {
                    /* #n# — reference a labeled object (CLHS 2.4.8.16) */
                    return read_label_reference(num_val);
                }
                if (ch >= '0' && ch <= '9') {
                    num_val = num_val * 10 + (ch - '0');
                } else {
                    /* #n<char> where <char> is a user-defined dispatch
                     * sub-character (set-dispatch-macro-character): call
                     * (fn stream sub-char num-arg) with the infix integer.
                     * e.g. ironclad's #32@(...) array reader. */
                    if (ch >= 0 && ch < CL_RT_CHARS) {
                        CL_Readtable *rt = cl_readtable_current();
                        if (!CL_NULL_P(rt->dispatch_fn[ch])) {
                            CL_Obj args[3];
                            args[0] = reader_stream;
                            args[1] = CL_MAKE_CHAR(ch);
                            args[2] = CL_MAKE_FIXNUM(num_val);
                            return cl_vm_apply(rt->dispatch_fn[ch], args, 3);
                        }
                    }
                    cl_reader_error(CL_ERR_PARSE, "Invalid radix prefix #%d%c", num_val, ch);
                    return CL_NIL;
                }
            }
            if (num_val < 2 || num_val > 36) {
                cl_reader_error(CL_ERR_PARSE, "Radix %d out of range (2-36)", num_val);
                return CL_NIL;
            }
            return read_radix_number(num_val);
        }
        if (read_suppress) return CL_NIL;
        cl_reader_error(CL_ERR_PARSE, "Unknown dispatch macro: #%c", ch);
        return CL_NIL;
    }

    case '"':
        return read_string();

    case ')':
        cl_reader_error(CL_ERR_PARSE, "Unexpected ')'");
        return CL_NIL;

    default:
        unread_char(ch);
        return read_atom_with_prefix(NULL, 0);
    }
}

/* Public API */

void cl_reader_reset_line(void)
{
    reader_line = 1;
}

int cl_reader_get_line(void)
{
    return reader_line;
}

void cl_reader_set_line(int line)
{
    reader_line = line;
}

CL_Obj cl_read(void)
{
    CL_Obj result;
    CL_Obj saved_uninterned = rd_uninterned;
    /* Sampled before the read: a #. form inside the datum may itself assign
     * *READ-SUPPRESS*, and CLHS ties the NIL primary value to the value in
     * effect when the read began. */
    int suppressed = read_suppress;
    reader_stream = cl_stdin_stream;
    reader_line = 1;
    eof_seen = 0;
    rd_uninterned = CL_NIL;
    /* GC SAFETY (audit tier 4, R4): read_expr compacts — the saved
     * uninterned alist must stay forwarded for the nested-read restore. */
    CL_GC_PROTECT(saved_uninterned);
    do { result = read_expr(); } while (result == CL_READER_SKIP && !eof_seen);
    CT->rd_last_eof = eof_seen;
    rd_uninterned = saved_uninterned;
    CL_GC_UNPROTECT(1);
    if (suppressed) return CL_NIL;
    return result;
}

CL_Obj cl_read_from_stream(CL_Obj stream)
{
    /* Save/restore reader state so nested reads (e.g. read-from-string
     * called from inside a dispatch-macro function that is itself running
     * during a parent read) don't leak state back to the parent reader.
     * The innermost EOF is preserved in rd_last_eof for cl_reader_eof().
     *
     * Line numbers persist *per stream*: the stream's `line` field stores
     * the cumulative line position so repeated top-level reads on the same
     * stream (e.g. LOAD) report real source lines, not always-1. */
    CL_Obj saved_stream      = reader_stream;
    int    saved_eof         = eof_seen;
    int    saved_line        = reader_line;
    CL_Obj saved_uninterned  = rd_uninterned;
    CL_Obj saved_labels      = rd_labels;
    int    saved_backrefs    = rd_label_backrefs;
    CL_Stream *st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
    CL_Obj result;
    int    suppressed        = read_suppress;  /* see cl_read() */

    reader_stream = stream;
    eof_seen = 0;
    reader_line = st->line ? (int)st->line : 1;
    rd_uninterned = CL_NIL;
    rd_labels = CL_NIL;
    rd_label_backrefs = 0;
    /* Protect saved alists AND the stream object itself: read_expr() can
     * compact the heap, staling C locals. `st` is a raw pointer derived from
     * `stream` before the read — after compaction the stream object moves, so
     * we must GC-protect `stream` and re-derive `st` before writing back
     * st->line. Without this, st->line = reader_line writes to the stream's
     * stale (now-relocated) slot, corrupting whatever object — frequently a
     * cons cell of the freshly-read list — now occupies that memory.
     * saved_stream too (audit tier 4, R2): it was the one unprotected member
     * of the save set — restoring it stale left CT->rd_stream pointing into
     * a pre-compaction offset for the parent read. */
    CL_GC_PROTECT(stream);
    CL_GC_PROTECT(saved_labels);
    CL_GC_PROTECT(saved_uninterned);
    CL_GC_PROTECT(saved_stream);
    do { result = read_expr(); } while (result == CL_READER_SKIP && !eof_seen);

    st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
    st->line = (uint32_t)reader_line;
    CT->rd_last_eof = eof_seen;
    reader_stream = saved_stream;
    reader_line   = saved_line;
    eof_seen      = saved_eof;
    rd_uninterned = saved_uninterned;
    rd_labels     = saved_labels;
    rd_label_backrefs = saved_backrefs;
    CL_GC_UNPROTECT(4); /* stream, saved_labels, saved_uninterned, saved_stream */
    if (suppressed) return CL_NIL;
    return result;
}

CL_Obj cl_read_from_string(CL_ReadStream *stream)
{
    CL_Obj saved_stream      = reader_stream;
    int    saved_eof         = eof_seen;
    int    saved_line        = reader_line;
    CL_Obj saved_uninterned  = rd_uninterned;
    CL_Obj saved_labels      = rd_labels;
    int    saved_backrefs    = rd_label_backrefs;
    CL_Obj str, s;
    CL_Stream *st;
    CL_Obj result;
    int    suppressed        = read_suppress;  /* see cl_read() */

    /* Protect saved alists before any allocation — cl_make_string can compact.
     * saved_stream too (audit tier 4, R3): the setup allocs AND read_expr
     * compact; restoring a stale saved_stream corrupts the parent reader's
     * stream for the rest of its read. */
    CL_GC_PROTECT(saved_labels);
    CL_GC_PROTECT(saved_uninterned);
    CL_GC_PROTECT(saved_stream);
    str = cl_make_string(stream->buf, (uint32_t)stream->len);
    CL_GC_PROTECT(str);
    s = cl_make_string_input_stream(str, (uint32_t)stream->pos, (uint32_t)stream->len);
    CL_GC_UNPROTECT(1);
    /* GC-protect s so compaction during read_expr() updates it.
     * Without protection, s is stale after compaction and CL_OBJ_TO_PTR(s)
     * returns a wrong pointer, corrupting stream->pos on sync-back. */
    CL_GC_PROTECT(s);

#ifdef DEBUG_READER
    {
        char dbg[160];
        snprintf(dbg, sizeof(dbg),
            "[READER] from_string: len=%d pos=%d str=%s s=%s buf=\"%.40s\"\n",
            stream->len, stream->pos,
            CL_NULL_P(str) ? "NIL!" : "ok",
            CL_NULL_P(s) ? "NIL!" : "ok",
            stream->buf ? stream->buf : "(null)");
        platform_write_string(dbg);
        if (!CL_NULL_P(s)) {
            CL_Stream *dst = (CL_Stream *)CL_OBJ_TO_PTR(s);
            snprintf(dbg, sizeof(dbg),
                "[READER]   stream: type=%u dir=%u pos=%u out_buf_len=%u "
                "string_buf=%s slen=%u flags=%u\n",
                dst->stream_type, dst->direction, dst->position,
                dst->out_buf_len,
                CL_NULL_P(dst->string_buf) ? "NIL!" : "ok",
                CL_NULL_P(dst->string_buf) ? 0u : cl_string_length(dst->string_buf),
                dst->flags);
            platform_write_string(dbg);
        }
    }
#endif

    reader_stream = s;
    reader_line = stream->line ? stream->line : 1;
    eof_seen = 0;
    rd_uninterned = CL_NIL;
    rd_labels = CL_NIL;
    rd_label_backrefs = 0;

    do { result = read_expr(); } while (result == CL_READER_SKIP && !eof_seen);

    /* Sync position back — s is GC-protected so it's valid after compaction */
    st = (CL_Stream *)CL_OBJ_TO_PTR(s);
    stream->pos = (int)st->position;
    stream->line = reader_line;

    CT->rd_last_eof = eof_seen;
    reader_stream = saved_stream;
    reader_line   = saved_line;
    eof_seen      = saved_eof;
    rd_uninterned = saved_uninterned;
    rd_labels     = saved_labels;
    rd_label_backrefs = saved_backrefs;
    CL_GC_UNPROTECT(4); /* s, saved_labels, saved_uninterned, saved_stream */
    if (suppressed) return CL_NIL;
    return result;
}

int cl_reader_eof(void)
{
    /* Reflect the most recently completed top-level/embedded read.
     * When a nested reader invocation runs (e.g. read-from-string
     * inside a dispatch-macro), the saved eof_seen is restored on
     * return — rd_last_eof captures the innermost read's EOF state
     * so bi_read can still detect end-of-file on its own call. */
    return CT->rd_last_eof;
}

void cl_reader_init(void)
{
    memset(cl_srcloc_table, 0, sizeof(cl_srcloc_table));
    reader_line = 1;
    cl_current_source_file = NULL;
    cl_current_file_id = 0;
    cl_readtable_init();
}
