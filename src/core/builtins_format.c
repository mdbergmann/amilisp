/*
 * builtins_format.c — Advanced FORMAT directive implementation
 *
 * Supports: ~A ~S ~W ~D ~B ~O ~X ~C ~% ~& ~| ~~ ~R ~T ~* ~^ ~? ~(~) ~[~;~] ~{~}
 * With prefix parameters, colon/at-sign modifiers, padding, commas, sign.
 */

#include "builtins.h"
#include "vm.h"
#include "symbol.h"
#include "package.h"
#include "mem.h"
#include "error.h"
#include "printer.h"
#include "stream.h"
#include "string_utils.h"
#include "float.h"
#include "../platform/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================
 * Data Structures
 * ================================================================ */

/* ~E takes the most: w,d,e,k,overflowchar,padchar,exptchar */
#define FMT_MAX_PARAMS 7
#define FMT_PARAM_UNSET (-1)

typedef struct {
    int32_t params[FMT_MAX_PARAMS];
    int     param_given[FMT_MAX_PARAMS];
    int     n_params;
    int     colon;    /* : modifier */
    int     atsign;   /* @ modifier */
    char    directive; /* uppercase */
} FmtDirective;

typedef struct {
    CL_Obj       stream;
    const char  *fmt;       /* full format string */
    const char  *pos;       /* current scan position */
    CL_Obj      *args;
    int          nargs;
    int          ai;        /* current arg index */
    int          escape;    /* ~^ triggered */
} FmtCtx;

/* Forward declarations */
static void fmt_dispatch(FmtCtx *ctx, FmtDirective *d);
static void fmt_run(FmtCtx *ctx);

/*
 * Return a NUL-terminated UTF-8 byte buffer for a CL string, copied into a
 * fresh platform_alloc'd (non-arena) buffer.  *out_alloc receives the heap
 * pointer and the caller MUST platform_free it.
 *
 * The copy is essential, not just convenient: FmtCtx.fmt/.pos are raw byte
 * pointers walked across the whole directive run, and directives like ~A / ~S
 * print arbitrary objects whose printer (or a user print-object method) can
 * trigger a compacting GC.  If fmt pointed at the string's inline arena data,
 * that compaction would relocate the string and leave fmt/pos dangling — the
 * remainder of the control string would be read from freed/moved memory, so
 * trailing directives are silently dropped ("[~a]" prints "[boom" instead of
 * "[boom]" under GC stress).  Copying to non-arena memory pins the control
 * string for the lifetime of the run.  (The bracket sub-directives ~{ ~( ~[ ~<
 * already copy their bodies for the same reason.)
 * The returned pointer is safe to scan with byte-oriented code: '~' (0x7E)
 * only ever appears as a literal '~' in valid UTF-8.
 */
/* platform_alloc with a loud failure: several format paths memcpy into the
 * returned buffer unchecked, so a NULL from an exhausted C heap (plausible on
 * an 8MB Amiga) must error rather than crash or silently drop output. */
static void *fmt_alloc(uint32_t size)
{
    void *p = platform_alloc(size);
    if (!p)
        cl_error(CL_ERR_STORAGE,
                 "FORMAT: out of memory allocating %u bytes", (unsigned)size);
    return p;
}

static const char *fmt_str_as_utf8(CL_Obj str_obj, char **out_alloc)
{
    *out_alloc = NULL;
#ifdef CL_WIDE_STRINGS
    if (CL_WIDE_STRING_P(str_obj)) {
        CL_WideString *ws = (CL_WideString *)CL_OBJ_TO_PTR(str_obj);
        uint32_t budget = ws->length * 4 + 1;
        char *buf = (char *)fmt_alloc(budget);
        cl_wide_string_to_utf8(str_obj, buf, budget);
        *out_alloc = buf;
        return buf;
    }
#endif
    {
        CL_String *s = (CL_String *)CL_OBJ_TO_PTR(str_obj);
        uint32_t len = s->length;
        char *buf = (char *)fmt_alloc(len + 1);
        memcpy(buf, s->data, len);
        buf[len] = '\0';
        *out_alloc = buf;
        return buf;
    }
}

/* ================================================================
 * Parsing
 * ================================================================ */

/* Consume next format arg. Returns CL_NIL if exhausted. */
static CL_Obj fmt_next_arg(FmtCtx *ctx)
{
    if (ctx->ai < ctx->nargs)
        return ctx->args[ctx->ai++];
    return CL_NIL;
}

/* Peek at next arg without consuming */
static CL_Obj fmt_peek_arg(FmtCtx *ctx)
{
    if (ctx->ai < ctx->nargs)
        return ctx->args[ctx->ai];
    return CL_NIL;
}

/* Remaining arg count */
static int fmt_args_remaining(FmtCtx *ctx)
{
    return ctx->nargs - ctx->ai;
}

/* Stage the elements of `list` onto the VM stack so a sub-context can use
 * them as its args: GC-rooted across the following fmt_run (which allocates
 * and can compact) and uncapped beyond CALL-ARGUMENTS-LIMIT — a fixed 64-slot
 * C snapshot silently dropped elements past 64.  Returns the count pushed and
 * stores the pre-staging sp in *base (the caller reads args from
 * &cl_vm.stack[*base] and restores cl_vm.sp = *base after the sub-run).  On
 * overflow, restores sp and signals with `who` naming the directive. */
static int fmt_stage_list(CL_Obj list, int *base, const char *who)
{
    int n = 0;
    int start = cl_vm.sp;
    CL_Obj tmp = list;
    *base = start;
    while (!CL_NULL_P(tmp)) {
        if (n >= CL_CALL_ARGS_LIMIT) {
            cl_vm.sp = start;
            cl_error(CL_ERR_ARGS,
                     "FORMAT %s: too many arguments "
                     "(call-arguments-limit is %d)", who, CL_CALL_ARGS_LIMIT);
        }
        cl_vm_push(cl_car(tmp));
        n++;
        tmp = cl_cdr(tmp);
    }
    return n;
}

/*
 * Parse a format directive starting at ctx->pos (which points just past '~').
 * After return, ctx->pos points past the directive character.
 */
static void fmt_parse_directive(FmtCtx *ctx, FmtDirective *d)
{
    const char *p = ctx->pos;
    int pi = 0;
    int i;

    for (i = 0; i < FMT_MAX_PARAMS; i++) {
        d->params[i] = 0;
        d->param_given[i] = 0;
    }
    d->n_params = 0;
    d->colon = 0;
    d->atsign = 0;
    d->directive = 0;

    /* Parse prefix parameters: integers, 'char, V, # separated by commas */
    for (;;) {
        if (*p >= '0' && *p <= '9') {
            /* Decimal integer */
            int32_t val = 0;
            while (*p >= '0' && *p <= '9') {
                val = val * 10 + (*p - '0');
                p++;
            }
            if (pi < FMT_MAX_PARAMS) {
                d->params[pi] = val;
                d->param_given[pi] = 1;
            }
            pi++;
        } else if (*p == '+' || *p == '-') {
            /* Signed integer */
            int sign = (*p == '-') ? -1 : 1;
            int32_t val = 0;
            p++;
            while (*p >= '0' && *p <= '9') {
                val = val * 10 + (*p - '0');
                p++;
            }
            if (pi < FMT_MAX_PARAMS) {
                d->params[pi] = val * sign;
                d->param_given[pi] = 1;
            }
            pi++;
        } else if (*p == '\'') {
            /* Character literal */
            p++;
            if (*p) {
                if (pi < FMT_MAX_PARAMS) {
                    d->params[pi] = (int32_t)(unsigned char)*p;
                    d->param_given[pi] = 1;
                }
                pi++;
                p++;
            }
        } else if (*p == 'V' || *p == 'v') {
            /* V: consume next arg as parameter */
            CL_Obj arg = fmt_next_arg(ctx);
            if (pi < FMT_MAX_PARAMS) {
                if (CL_FIXNUM_P(arg)) {
                    d->params[pi] = CL_FIXNUM_VAL(arg);
                    d->param_given[pi] = 1;
                } else if (CL_CHAR_P(arg)) {
                    d->params[pi] = CL_CHAR_VAL(arg);
                    d->param_given[pi] = 1;
                }
                /* NIL arg = param not given */
            }
            pi++;
            p++;
        } else if (*p == '#') {
            /* #: remaining arg count */
            if (pi < FMT_MAX_PARAMS) {
                d->params[pi] = fmt_args_remaining(ctx);
                d->param_given[pi] = 1;
            }
            pi++;
            p++;
        } else if (*p == ',') {
            /* Empty parameter slot (or separator) */
            if (pi == 0 || (pi > 0 && *(p-1) == ',')) {
                /* Empty slot before comma or consecutive commas */
                pi++;
            }
            p++;
            continue;
        } else {
            break;
        }
        /* After a parameter, check for comma separator */
        if (*p == ',') {
            p++;
        } else {
            break;
        }
    }
    d->n_params = pi;

    /* Parse colon and at-sign modifiers (in any order) */
    while (*p == ':' || *p == '@') {
        if (*p == ':') d->colon = 1;
        if (*p == '@') d->atsign = 1;
        p++;
    }

    /* The directive character */
    if (*p) {
        d->directive = (*p >= 'a' && *p <= 'z') ? (*p - 32) : *p;
        p++;
    }

    ctx->pos = p;
}

/*
 * Find matching close directive, collecting separator positions.
 * Handles nesting of the same open/close pair.
 *
 * `p` points just past the opening directive (e.g., past '(' in ~().
 * `open` and `close` are uppercase directive chars (e.g., '(' and ')').
 * `seps` array receives positions of ~; separators at nesting level 0.
 * `n_seps` receives the count. `max_seps` is array capacity.
 *
 * Returns pointer to the character just past the close directive, or NULL.
 */
static const char *fmt_find_close(const char *p, char open, char close,
                                  const char **seps, int *n_seps, int max_seps)
{
    int depth = 1;        /* depth of (open, close) pair */
    int inner_depth = 0;  /* depth of OTHER bracketing directives (~( ~[ ~{ ~<) */
    *n_seps = 0;

    while (*p && depth > 0) {
        if (*p == '~') {
            const char *start = p;
            p++;
            /* Skip prefix params */
            while (*p && ((*p >= '0' && *p <= '9') || *p == ',' || *p == '\'' ||
                          *p == '+' || *p == '-' || *p == 'V' || *p == 'v' ||
                          *p == '#')) {
                if (*p == '\'') { p++; if (*p) p++; }
                else p++;
            }
            /* Skip modifiers */
            while (*p == ':' || *p == '@') p++;

            if (*p) {
                char dc = (*p >= 'a' && *p <= 'z') ? (*p - 32) : *p;
                if (dc == open) {
                    depth++;
                } else if (dc == close) {
                    if (inner_depth > 0 && (dc == ')' || dc == ']' ||
                                            dc == '}' || dc == '>')) {
                        /* Closes an inner directive, not the outer pair. */
                        inner_depth--;
                    } else {
                        depth--;
                        if (depth == 0) {
                            p++;
                            return p;
                        }
                    }
                } else if (dc == '(' || dc == '[' || dc == '{' || dc == '<') {
                    inner_depth++;
                } else if (dc == ')' || dc == ']' || dc == '}' || dc == '>') {
                    if (inner_depth > 0) inner_depth--;
                } else if (dc == ';' && depth == 1 && inner_depth == 0) {
                    /* Record separator — `start` points to the '~' before ';' */
                    if (*n_seps < max_seps) {
                        seps[*n_seps] = start;
                        (*n_seps)++;
                    }
                }
                p++;
            }
        } else {
            p++;
        }
    }
    return NULL; /* no matching close found */
}

/* Helper: get param with default */
static int32_t fmt_param(FmtDirective *d, int idx, int32_t defval)
{
    if (idx < d->n_params && idx < FMT_MAX_PARAMS && d->param_given[idx])
        return d->params[idx];
    return defval;
}

/* ================================================================
 * Integer formatting with padding/commas/sign
 * ================================================================ */

/* Render an integer to a stack buffer in the given base.
 * Returns pointer into buf (may not be buf[0]).
 * `len_out` receives the string length. */
static char *render_integer(CL_Obj obj, int32_t base, char *buf, int bufsz,
                            int *len_out)
{
    /* Use printer to render in given base.  Thread-local dynamic binds
     * (tier-4 FS16): mutating the global *PRINT-BASE* / *PRINT-RADIX*
     * cells raced peer threads' printers between set and restore.  The dyn
     * stack is GC-marked, so the saved values need no manual roots. */
    int dyn_mark = cl_dyn_top;
    int len;

    cl_dynbind_c(SYM_PRINT_BASE, CL_MAKE_FIXNUM(base));
    cl_dynbind_c(SYM_PRINT_RADIX, CL_NIL);
    len = cl_princ_to_string(obj, buf, bufsz);
    cl_dynbind_restore_to(dyn_mark);
    *len_out = len;
    return buf;
}

/*
 * Format a padded integer: handles mincol, padchar, commachar, comma-interval,
 * colon (commas), atsign (sign).
 */
static void fmt_padded_integer(FmtCtx *ctx, FmtDirective *d, int32_t base)
{
    CL_Obj arg = fmt_next_arg(ctx);
    /* Fixnums fit easily (32-bit is at most 32 binary digits + sign);
     * bignums have no digit bound and render through a string stream into
     * RAW_ALLOC below. */
    char fixbuf[64];
    /* Worst case: every digit gets a group separator (comma-interval 1) —
     * dlen digits + dlen-1 separators + NUL.  Heap-allocated to match the
     * unbounded bignum digit count (`(format nil "~,,,1:D" (expt 10 100))`
     * smashed the stack when this was a fixed 192-byte buffer). */
    char *with_commas_alloc = NULL;
    char *raw_alloc = NULL;
    const char *raw;
    int raw_len;
    int32_t mincol   = fmt_param(d, 0, 0);
    int32_t padchar  = fmt_param(d, 1, (int32_t)' ');
    int32_t commachar = fmt_param(d, 2, (int32_t)',');
    int32_t comma_int = fmt_param(d, 3, 3);
    const char *digits;
    int dlen;
    int negative;
    int final_len;
    int pad;
    int i;

    /* If not an integer, just princ it */
    if (!CL_INTEGER_P(arg)) {
        cl_princ_to_stream(arg, ctx->stream);
        return;
    }

    if (CL_FIXNUM_P(arg)) {
        render_integer(arg, base, fixbuf, sizeof(fixbuf), &raw_len);
        raw = fixbuf;
    } else {
        /* Bignum: unbounded digit count — render through a
         * string-output-stream and copy the ASCII digits into a heap
         * buffer sized to fit (a fixed 128-byte buffer here used to
         * truncate ~D of integers past ~127 digits).  ARG/SSTREAM are
         * bare C locals held across allocating calls — root them. */
        CL_Obj sstream, text;
        int dyn_mark;
        int j;
        CL_GC_PROTECT(arg);
        sstream = cl_make_string_output_stream();
        CL_GC_PROTECT(sstream);
        dyn_mark = cl_dyn_top;
        cl_dynbind_c(SYM_PRINT_BASE, CL_MAKE_FIXNUM(base));
        cl_dynbind_c(SYM_PRINT_RADIX, CL_NIL);
        cl_princ_to_stream(arg, sstream);
        cl_dynbind_restore_to(dyn_mark);
        text = cl_finish_string_output_stream(sstream);
        raw_len = (int)cl_string_length(text);
        raw_alloc = (char *)fmt_alloc((uint32_t)raw_len + 1);
        for (j = 0; j < raw_len; j++)
            raw_alloc[j] = (char)cl_string_char_at(text, (uint32_t)j);
        raw_alloc[raw_len] = '\0';
        raw = raw_alloc;
        CL_GC_UNPROTECT(2);
    }

    /* Separate sign from digits */
    negative = (raw[0] == '-');
    digits = negative ? raw + 1 : raw;
    dlen = negative ? raw_len - 1 : raw_len;

    /* Insert commas if :modifier */
    if (d->colon && comma_int > 0 && dlen > comma_int) {
        int wi = 0, ri;
        with_commas_alloc = (char *)fmt_alloc((uint32_t)(2 * dlen) + 1);
        for (ri = 0; ri < dlen; ri++) {
            if (ri > 0 && ((dlen - ri) % comma_int == 0))
                with_commas_alloc[wi++] = (char)commachar;
            with_commas_alloc[wi++] = digits[ri];
        }
        with_commas_alloc[wi] = '\0';
        digits = with_commas_alloc;
        dlen = wi;
    }

    /* Compute final length with sign */
    final_len = dlen;
    if (negative || d->atsign) final_len++;

    /* Pad */
    pad = (mincol > final_len) ? (int)(mincol - final_len) : 0;
    for (i = 0; i < pad; i++)
        cl_stream_write_char(ctx->stream, (int)padchar);

    /* Sign */
    if (negative)
        cl_stream_write_char(ctx->stream, '-');
    else if (d->atsign)
        cl_stream_write_char(ctx->stream, '+');

    /* Digits */
    cl_stream_write_string(ctx->stream, digits, (uint32_t)dlen);

    if (with_commas_alloc) platform_free(with_commas_alloc);
    if (raw_alloc) platform_free(raw_alloc);
}

/* ================================================================
 * Object formatting with padding (for ~A, ~S)
 * ================================================================ */

static void fmt_padded_obj(FmtCtx *ctx, FmtDirective *d, int escape)
{
    CL_Obj arg = fmt_next_arg(ctx);
    int32_t mincol = fmt_param(d, 0, 0);
    int32_t colinc = fmt_param(d, 1, 1);
    int32_t minpad = fmt_param(d, 2, 0);
    int32_t padchar = fmt_param(d, 3, (int32_t)' ');
    CL_Obj sstream, text;
    int len;
    int total_pad;
    int i;

    /* No padding requested (the overwhelmingly common ~A/~S): stream the
     * object directly.  The printed text has no a priori length bound —
     * a fixed rendering buffer here used to truncate long strings at its
     * capacity (511 chars), silently losing output. */
    if (mincol <= 0 && minpad <= 0) {
        if (escape)
            cl_prin1_to_stream(arg, ctx->stream);
        else
            cl_princ_to_stream(arg, ctx->stream);
        return;
    }

    /* Padded: the pad math needs the printed length first, so render into
     * a string-output-stream (unbounded), measure in characters, then emit.
     * ARG/SSTREAM/TEXT are bare C locals held across allocating calls
     * (stream creation, the printer, pad writes into a possibly growing
     * string destination) — root them against the compacting GC. */
    CL_GC_PROTECT(arg);
    sstream = cl_make_string_output_stream();
    CL_GC_PROTECT(sstream);
    if (escape)
        cl_prin1_to_stream(arg, sstream);
    else
        cl_princ_to_stream(arg, sstream);
    text = cl_finish_string_output_stream(sstream);
    CL_GC_PROTECT(text);
    len = (int)cl_string_length(text);

    /* Calculate padding needed */
    total_pad = minpad;
    if (mincol > len + total_pad) {
        int extra = (int)(mincol - len - total_pad);
        if (colinc > 1)
            extra = ((extra + colinc - 1) / colinc) * colinc;
        total_pad += extra;
    }

    if (d->atsign) {
        /* @A/@S: pad on left (right-justify) */
        for (i = 0; i < total_pad; i++)
            cl_stream_write_char(ctx->stream, (int)padchar);
        cl_princ_to_stream(text, ctx->stream);
    } else {
        /* ~A/~S: pad on right (left-justify) */
        cl_princ_to_stream(text, ctx->stream);
        for (i = 0; i < total_pad; i++)
            cl_stream_write_char(ctx->stream, (int)padchar);
    }
    CL_GC_UNPROTECT(3);
}

/* ================================================================
 * ~R — Radix printing (cardinal, ordinal, Roman)
 * ================================================================ */

static const char *cardinal_ones[] = {
    "", "one", "two", "three", "four", "five", "six", "seven",
    "eight", "nine", "ten", "eleven", "twelve", "thirteen",
    "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen"
};

static const char *cardinal_tens[] = {
    "", "", "twenty", "thirty", "forty", "fifty",
    "sixty", "seventy", "eighty", "ninety"
};

static const char *ordinal_ones[] = {
    "", "first", "second", "third", "fourth", "fifth", "sixth", "seventh",
    "eighth", "ninth", "tenth", "eleventh", "twelfth", "thirteenth",
    "fourteenth", "fifteenth", "sixteenth", "seventeenth", "eighteenth",
    "nineteenth"
};

static const char *ordinal_tens[] = {
    "", "", "twentieth", "thirtieth", "fortieth", "fiftieth",
    "sixtieth", "seventieth", "eightieth", "ninetieth"
};

/* Write cardinal English for 1-999 to stream.
 * Returns number of chars written (for tracking). */
static void fmt_cardinal_chunk(FmtCtx *ctx, int32_t n)
{
    if (n >= 100) {
        cl_stream_write_string(ctx->stream, cardinal_ones[n / 100],
                               (uint32_t)strlen(cardinal_ones[n / 100]));
        cl_stream_write_string(ctx->stream, " hundred", 8);
        n %= 100;
        if (n > 0) cl_stream_write_char(ctx->stream, ' ');
    }
    if (n >= 20) {
        cl_stream_write_string(ctx->stream, cardinal_tens[n / 10],
                               (uint32_t)strlen(cardinal_tens[n / 10]));
        n %= 10;
        if (n > 0) {
            cl_stream_write_char(ctx->stream, '-');
            cl_stream_write_string(ctx->stream, cardinal_ones[n],
                                   (uint32_t)strlen(cardinal_ones[n]));
        }
    } else if (n > 0) {
        cl_stream_write_string(ctx->stream, cardinal_ones[n],
                               (uint32_t)strlen(cardinal_ones[n]));
    }
}

static void fmt_cardinal(FmtCtx *ctx, int32_t n)
{
    int started = 0;

    if (n < 0) {
        cl_stream_write_string(ctx->stream, "negative ", 9);
        n = -n;
    }
    if (n == 0) {
        cl_stream_write_string(ctx->stream, "zero", 4);
        return;
    }

    if (n >= 1000000000) {
        fmt_cardinal_chunk(ctx, n / 1000000000);
        cl_stream_write_string(ctx->stream, " billion", 8);
        n %= 1000000000;
        started = 1;
    }
    if (n >= 1000000) {
        if (started) cl_stream_write_char(ctx->stream, ' ');
        fmt_cardinal_chunk(ctx, n / 1000000);
        cl_stream_write_string(ctx->stream, " million", 8);
        n %= 1000000;
        started = 1;
    }
    if (n >= 1000) {
        if (started) cl_stream_write_char(ctx->stream, ' ');
        fmt_cardinal_chunk(ctx, n / 1000);
        cl_stream_write_string(ctx->stream, " thousand", 9);
        n %= 1000;
        started = 1;
    }
    if (n > 0) {
        if (started) cl_stream_write_char(ctx->stream, ' ');
        fmt_cardinal_chunk(ctx, n);
    }
}

static void fmt_ordinal(FmtCtx *ctx, int32_t n)
{
    int32_t last_two;

    if (n < 0) {
        cl_stream_write_string(ctx->stream, "negative ", 9);
        n = -n;
    }
    if (n == 0) {
        cl_stream_write_string(ctx->stream, "zeroth", 6);
        return;
    }

    last_two = n % 100;

    /* Print all but the last part as cardinal */
    if (n >= 100) {
        int32_t prefix = n - last_two;
        if (last_two == 0) {
            /* The whole number ends in a "hundred"/"thousand" etc. */
            /* We need to figure out the ordinal suffix */
            if (n % 1000000000 == 0) {
                fmt_cardinal(ctx, n / 1000000000);
                cl_stream_write_string(ctx->stream, " billionth", 10);
                return;
            } else if (n % 1000000 == 0) {
                fmt_cardinal(ctx, n / 1000000);
                cl_stream_write_string(ctx->stream, " millionth", 10);
                return;
            } else if (n % 1000 == 0) {
                fmt_cardinal(ctx, n / 1000);
                cl_stream_write_string(ctx->stream, " thousandth", 11);
                return;
            } else if (n % 100 == 0) {
                int32_t above = n - (n % 1000);
                if (above > 0) {
                    fmt_cardinal(ctx, above);
                    cl_stream_write_char(ctx->stream, ' ');
                }
                fmt_cardinal_chunk(ctx, (n % 1000) / 100);
                cl_stream_write_string(ctx->stream, " hundredth", 10);
                return;
            }
        }
        /* Print prefix as cardinal, then ordinal suffix for last_two */
        fmt_cardinal(ctx, prefix);
        cl_stream_write_char(ctx->stream, ' ');
    }

    /* Ordinal for the last part (1-99) */
    if (last_two >= 20) {
        int ones = last_two % 10;
        if (ones == 0) {
            cl_stream_write_string(ctx->stream, ordinal_tens[last_two / 10],
                                   (uint32_t)strlen(ordinal_tens[last_two / 10]));
        } else {
            cl_stream_write_string(ctx->stream, cardinal_tens[last_two / 10],
                                   (uint32_t)strlen(cardinal_tens[last_two / 10]));
            cl_stream_write_char(ctx->stream, '-');
            cl_stream_write_string(ctx->stream, ordinal_ones[ones],
                                   (uint32_t)strlen(ordinal_ones[ones]));
        }
    } else if (last_two > 0) {
        cl_stream_write_string(ctx->stream, ordinal_ones[last_two],
                               (uint32_t)strlen(ordinal_ones[last_two]));
    }
}

static void fmt_roman(FmtCtx *ctx, int32_t n, int old_style)
{
    /* Standard subtractive pairs */
    static const int    vals[]  = {1000,900,500,400,100,90,50,40,10,9,5,4,1};
    static const char  *syms[]  = {"M","CM","D","CD","C","XC","L","XL","X","IX","V","IV","I"};
    /* Old-style: additive only */
    static const int    old_vals[] = {1000,500,100,50,10,5,1};
    static const char  *old_syms[] = {"M","D","C","L","X","V","I"};
    const int *vp;
    const char **sp;
    int count;
    int i;

    if (n <= 0 || n > 3999) {
        /* Fall back to decimal for out-of-range */
        char buf[32];
        int len = snprintf(buf, sizeof(buf), "%ld", (long)n);
        cl_stream_write_string(ctx->stream, buf, (uint32_t)len);
        return;
    }

    if (old_style) {
        vp = old_vals; sp = old_syms; count = 7;
    } else {
        vp = vals; sp = syms; count = 13;
    }

    for (i = 0; i < count; i++) {
        while (n >= vp[i]) {
            cl_stream_write_string(ctx->stream, sp[i], (uint32_t)strlen(sp[i]));
            n -= vp[i];
        }
    }
}

static void fmt_radix(FmtCtx *ctx, FmtDirective *d)
{
    if (d->n_params >= 1 && d->param_given[0]) {
        /* ~radix,mincol,padchar,commachar,commaintR — shift params past radix */
        FmtDirective shifted = *d;
        int i;
        int32_t radix = d->params[0];
        for (i = 0; i < FMT_MAX_PARAMS - 1; i++) {
            shifted.params[i] = d->params[i + 1];
            shifted.param_given[i] = d->param_given[i + 1];
        }
        shifted.params[FMT_MAX_PARAMS - 1] = 0;
        shifted.param_given[FMT_MAX_PARAMS - 1] = 0;
        shifted.n_params = d->n_params > 1 ? d->n_params - 1 : 0;
        fmt_padded_integer(ctx, &shifted, radix);
    } else {
        CL_Obj arg = fmt_next_arg(ctx);
        int32_t n;
        if (!CL_INTEGER_P(arg)) {
            cl_princ_to_stream(arg, ctx->stream);
            return;
        }
        if (CL_FIXNUM_P(arg))
            n = CL_FIXNUM_VAL(arg);
        else {
            /* Bignum — render decimal and parse (simple approach) */
            char buf[64];
            cl_princ_to_string(arg, buf, sizeof(buf));
            n = (int32_t)strtol(buf, NULL, 10);
        }

        if (d->colon && d->atsign) {
            /* ~:@R — old Roman */
            fmt_roman(ctx, n, 1);
        } else if (d->atsign) {
            /* ~@R — Roman */
            fmt_roman(ctx, n, 0);
        } else if (d->colon) {
            /* ~:R — ordinal */
            fmt_ordinal(ctx, n);
        } else {
            /* ~R — cardinal */
            fmt_cardinal(ctx, n);
        }
    }
}

/* ================================================================
 * ~T — Tabulate
 * ================================================================ */

static void fmt_tabulate(FmtCtx *ctx, FmtDirective *d)
{
    int32_t colnum = fmt_param(d, 0, 1);
    int32_t colinc = fmt_param(d, 1, 1);
    CL_Stream *st = (CL_Stream *)CL_OBJ_TO_PTR(ctx->stream);
    int32_t curpos = (int32_t)st->charpos;
    int spaces;
    int i;

    if (d->atsign) {
        /* ~@T: relative — output colnum spaces minimum, then pad to colinc */
        spaces = (int)colnum;
        if (colinc > 0) {
            int newpos = curpos + spaces;
            int rem = newpos % colinc;
            if (rem != 0)
                spaces += (int)(colinc - rem);
        }
    } else {
        /* ~T: absolute — output spaces to reach colnum */
        if (curpos < colnum) {
            spaces = (int)(colnum - curpos);
        } else if (colinc > 0) {
            /* Already at or past colnum — advance to next colnum + k*colinc */
            int past = curpos - colnum;
            int rem = past % colinc;
            spaces = (rem == 0) ? colinc : (int)(colinc - rem);
        } else {
            spaces = 0;
        }
    }

    for (i = 0; i < spaces; i++)
        cl_stream_write_char(ctx->stream, ' ');
}

/* ================================================================
 * ~* — Go-To Argument
 * ================================================================ */

static void fmt_goto(FmtCtx *ctx, FmtDirective *d)
{
    if (d->atsign) {
        /* ~n@* — go to absolute format arg position n (default 0).
         * Format args start at ctx->args[2] (past dest + fmt string),
         * so absolute arg n maps to ai = n + 2. */
        int32_t n = fmt_param(d, 0, 0);
        int target = (int)n + 2;
        if (target >= 2 && target <= ctx->nargs)
            ctx->ai = target;
    } else if (d->colon) {
        /* ~n:* — back up n args (default 1) */
        int32_t n = fmt_param(d, 0, 1);
        ctx->ai -= (int)n;
        if (ctx->ai < 0) ctx->ai = 0;
    } else {
        /* ~n* — skip forward n args (default 1).  Clamp both ends: a
         * negative parameter (e.g. `~-5*`, or V taking a negative arg)
         * would otherwise index before ctx->args[] — OOB read fed to the
         * printer (mirror the lower clamp in the `:` branch). */
        int32_t n = fmt_param(d, 0, 1);
        ctx->ai += (int)n;
        if (ctx->ai < 0) ctx->ai = 0;
        if (ctx->ai > ctx->nargs) ctx->ai = ctx->nargs;
    }
}

/* ================================================================
 * ~(~) — Case Conversion
 * ================================================================ */

static void fmt_case_convert(FmtCtx *ctx, FmtDirective *d)
{
    const char *body_start = ctx->pos;
    const char *seps[1];
    int n_seps = 0;
    const char *body_end;
    CL_Obj sstream;
    CL_Obj result;
    CL_String *rs;
    char *data;
    uint32_t len;
    uint32_t i;
    FmtCtx sub;

    body_end = fmt_find_close(body_start, '(', ')', seps, &n_seps, 0);
    if (!body_end) {
        cl_error(CL_ERR_GENERAL, "FORMAT: unmatched ~(");
        return;
    }

    /* Format body into a temp string stream */
    sstream = cl_make_string_output_stream();
    CL_GC_PROTECT(sstream);

    sub = *ctx;
    sub.stream = sstream;
    sub.pos = body_start;
    /* Run until we hit the close paren position */
    {
        /* We need to run the format on the body substring.
         * body_end points past ')', and body_start is just past '('.
         * The body ends at body_end - 2 (at the '~' before ')').
         * But since fmt_run runs until NUL or until the pos reaches a point,
         * we use a substring approach with a temp NUL. Instead, let's just
         * run format on the body text. */
        int body_len = (int)(body_end - body_start);
        char *body_copy;
        /* The body_end points past ')'. The body includes everything from
         * body_start up to the '~' before ')'. We need to find that. */
        /* Actually, body_end is returned by find_close as pointing past ')'.
         * The close '~)' is at body_end - 2 (the '~') and body_end - 1 (')').
         * So the body text is from body_start to body_end - 2. */
        body_len = (int)(body_end - 2 - body_start);
        if (body_len < 0) body_len = 0;
        body_copy = (char *)fmt_alloc((uint32_t)body_len + 1);
        memcpy(body_copy, body_start, (uint32_t)body_len);
        body_copy[body_len] = '\0';

        sub.fmt = body_copy;
        sub.pos = body_copy;
        fmt_run(&sub);
        platform_free(body_copy);
    }

    /* Get the result string and free the temp stream's outbuf */
    result = cl_finish_string_output_stream(sstream);
    CL_GC_UNPROTECT(1);

#ifdef CL_WIDE_STRINGS
    if (CL_WIDE_STRING_P(result)) {
        /* Wide result (body printed non-Latin-1 chars): the case loops below
         * assume 1-byte CL_String data — running them over UTF-32 code units
         * garbles the text (and reads the wrong length field).  Run the same
         * ASCII-range case ops per code point instead; non-ASCII code points
         * are left untouched (treated as word separators, matching the
         * narrow path's behavior for non-alpha bytes). */
        CL_WideString *ws = (CL_WideString *)CL_OBJ_TO_PTR(result);
        uint32_t *wd = ws->data;
        uint32_t wlen = ws->length;

        if (d->colon && d->atsign) {
            /* ~:@( — uppercase all */
            for (i = 0; i < wlen; i++) {
                if (wd[i] >= 'a' && wd[i] <= 'z')
                    wd[i] -= 32;
            }
        } else if (d->atsign) {
            /* ~@( — capitalize first word only */
            int first = 1;
            for (i = 0; i < wlen; i++) {
                if (wd[i] >= 'A' && wd[i] <= 'Z') {
                    if (!first) wd[i] += 32;
                    first = 0;
                } else if (wd[i] >= 'a' && wd[i] <= 'z') {
                    if (first) { wd[i] -= 32; first = 0; }
                }
            }
        } else if (d->colon) {
            /* ~:( — capitalize each word */
            int word_start = 1;
            for (i = 0; i < wlen; i++) {
                if (wd[i] >= 'A' && wd[i] <= 'Z') {
                    if (!word_start) wd[i] += 32;
                    word_start = 0;
                } else if (wd[i] >= 'a' && wd[i] <= 'z') {
                    if (word_start) wd[i] -= 32;
                    word_start = 0;
                } else {
                    word_start = 1;
                }
            }
        } else {
            /* ~( — lowercase all */
            for (i = 0; i < wlen; i++) {
                if (wd[i] >= 'A' && wd[i] <= 'Z')
                    wd[i] += 32;
            }
        }

        /* cl_stream_write_lisp_string is a base-string byte writer — a wide
         * string must go out per code point.  Re-derive the data pointer per
         * character: the write can allocate (stream buffer growth) or park
         * in a safe region (MT) and the string may move. */
        CL_GC_PROTECT(result);
        for (i = 0; i < wlen; i++) {
            uint32_t cp =
                ((CL_WideString *)CL_OBJ_TO_PTR(result))->data[i];
            cl_stream_write_char(ctx->stream, (int)cp);
        }
        CL_GC_UNPROTECT(1);
        ctx->ai = sub.ai;
        ctx->pos = body_end;
        return;
    }
#endif

    rs = (CL_String *)CL_OBJ_TO_PTR(result);
    data = rs->data;
    len = rs->length;

    if (d->colon && d->atsign) {
        /* ~:@( — uppercase all */
        for (i = 0; i < len; i++) {
            if (data[i] >= 'a' && data[i] <= 'z')
                data[i] -= 32;
        }
    } else if (d->atsign) {
        /* ~@( — capitalize first word only */
        int first = 1;
        for (i = 0; i < len; i++) {
            if (data[i] >= 'A' && data[i] <= 'Z') {
                if (!first) data[i] += 32;
                first = 0;
            } else if (data[i] >= 'a' && data[i] <= 'z') {
                if (first) { data[i] -= 32; first = 0; }
            } else if (data[i] == ' ' || data[i] == '\t' || data[i] == '\n') {
                /* Don't reset first — only capitalize very first alpha */
            }
        }
    } else if (d->colon) {
        /* ~:( — capitalize each word */
        int word_start = 1;
        for (i = 0; i < len; i++) {
            if (data[i] >= 'A' && data[i] <= 'Z') {
                if (!word_start) data[i] += 32;
                word_start = 0;
            } else if (data[i] >= 'a' && data[i] <= 'z') {
                if (word_start) data[i] -= 32;
                word_start = 0;
            } else {
                word_start = 1;
            }
        }
    } else {
        /* ~( — lowercase all */
        for (i = 0; i < len; i++) {
            if (data[i] >= 'A' && data[i] <= 'Z')
                data[i] += 32;
        }
    }

    /* Write to output stream — via the arena-safe chunked writer: data
     * points into the arena and a blocking file/socket write can park in
     * a safe region while a peer compaction moves it (MT). */
    cl_stream_write_lisp_string(ctx->stream, result, 0, len);

    /* Update parent arg index */
    ctx->ai = sub.ai;
    ctx->pos = body_end;
}

/* ================================================================
 * ~[~;~] — Conditional Selection
 * ================================================================ */

#define FMT_MAX_SEPS 64

static void fmt_conditional(FmtCtx *ctx, FmtDirective *d)
{
    const char *body_start = ctx->pos;
    const char *seps[FMT_MAX_SEPS];
    int n_seps = 0;
    const char *body_end;
    /* Collect clause boundaries: seps[i] point to '~' before ';' */
    const char *clause_starts[FMT_MAX_SEPS + 2];
    const char *clause_ends[FMT_MAX_SEPS + 2];
    int n_clauses;
    int has_default = 0;
    int selected = -1;
    int i;

    body_end = fmt_find_close(body_start, '[', ']', seps, &n_seps, FMT_MAX_SEPS);
    if (!body_end) {
        cl_error(CL_ERR_GENERAL, "FORMAT: unmatched ~[");
        return;
    }

    /* Build clause boundaries */
    n_clauses = n_seps + 1;
    clause_starts[0] = body_start;
    for (i = 0; i < n_seps; i++) {
        /* seps[i] points to '~' before ';'. The ';' may have ':' modifier. */
        const char *sp = seps[i] + 1; /* past '~' */
        /* Skip params/modifiers to find ';' */
        while (*sp == ':' || *sp == '@') sp++;
        /* sp now at ';' */
        clause_ends[i] = seps[i];
        clause_starts[i + 1] = sp + 1;

        /* Check if this separator has colon modifier (~:;) = default clause */
        {
            const char *tp = seps[i] + 1;
            while (*tp == ':' || *tp == '@') {
                if (*tp == ':') has_default = 1;
                tp++;
            }
        }
    }
    /* Last clause ends at the close bracket */
    clause_ends[n_seps] = body_end - 2; /* point to '~' before ']' */

    if (d->colon) {
        /* ~:[false~;true~] — boolean: nil=0, non-nil=1 */
        CL_Obj arg = fmt_next_arg(ctx);
        selected = CL_NULL_P(arg) ? 0 : 1;
    } else if (d->atsign) {
        /* ~@[clause~] — if arg non-nil, format clause with arg available */
        CL_Obj arg = fmt_peek_arg(ctx);
        if (!CL_NULL_P(arg)) {
            selected = 0; /* format the one clause */
        } else {
            ctx->ai++; /* consume nil */
            selected = -1; /* skip */
        }
    } else {
        /* ~[c0~;c1~;...~] — select by integer */
        CL_Obj arg = fmt_next_arg(ctx);
        int32_t idx = 0;
        if (CL_FIXNUM_P(arg))
            idx = CL_FIXNUM_VAL(arg);
        if (idx >= 0 && idx < n_clauses) {
            /* Check if this is the default clause */
            selected = (int)idx;
        } else if (has_default) {
            selected = n_clauses - 1; /* default is last */
        } else {
            selected = -1;
        }
    }

    if (selected >= 0 && selected < n_clauses) {
        /* Format the selected clause */
        const char *cs = clause_starts[selected];
        const char *ce = clause_ends[selected];
        int clen = (int)(ce - cs);
        if (clen > 0) {
            char *clause_copy = (char *)fmt_alloc((uint32_t)clen + 1);
            FmtCtx sub;
            memcpy(clause_copy, cs, (uint32_t)clen);
            clause_copy[clen] = '\0';
            sub = *ctx;
            sub.fmt = clause_copy;
            sub.pos = clause_copy;
            fmt_run(&sub);
            ctx->ai = sub.ai;
            platform_free(clause_copy);
        }
    }

    ctx->pos = body_end;
}

/* ================================================================
 * ~{~} — Iteration and ~^
 * ================================================================ */

#define FMT_MAX_ITERATIONS 1000

static void fmt_iteration(FmtCtx *ctx, FmtDirective *d)
{
    const char *body_start = ctx->pos;
    const char *seps_unused[1];
    int n_seps = 0;
    const char *body_end;
    int body_len;
    char *body_copy;
    int32_t max_iter = fmt_param(d, 0, FMT_MAX_ITERATIONS);
    int iter_count = 0;

    body_end = fmt_find_close(body_start, '{', '}', seps_unused, &n_seps, 0);
    if (!body_end) {
        cl_error(CL_ERR_GENERAL, "FORMAT: unmatched ~{");
        return;
    }

    body_len = (int)(body_end - 2 - body_start);
    if (body_len < 0) body_len = 0;
    body_copy = (char *)fmt_alloc((uint32_t)body_len + 1);
    memcpy(body_copy, body_start, (uint32_t)body_len);
    body_copy[body_len] = '\0';

    if (d->colon && d->atsign) {
        /* ~:@{body~} — remaining args are sublists */
        /* ~^ in sub-context only terminates current step, not whole iteration */
        while (ctx->ai < ctx->nargs && iter_count < max_iter) {
            CL_Obj sublist = fmt_next_arg(ctx);
            int sbase;
            int sub_n = fmt_stage_list(sublist, &sbase, "~~:@{");
            FmtCtx sub;

            sub.stream = ctx->stream;
            sub.fmt = body_copy;
            sub.pos = body_copy;
            sub.args = &cl_vm.stack[sbase];
            sub.nargs = sub_n;
            sub.ai = 0;
            sub.escape = 0;
            fmt_run(&sub);
            cl_vm.sp = sbase;
            /* ~^ only terminates current step for ~:@{ */
            iter_count++;
        }
    } else if (d->atsign) {
        /* ~@{body~} — remaining parent args used directly */
        while (ctx->ai < ctx->nargs && iter_count < max_iter) {
            FmtCtx sub;
            sub.stream = ctx->stream;
            sub.fmt = body_copy;
            sub.pos = body_copy;
            sub.args = ctx->args;
            sub.nargs = ctx->nargs;
            sub.ai = ctx->ai;
            sub.escape = 0;
            fmt_run(&sub);
            ctx->ai = sub.ai;
            if (sub.escape) break;
            iter_count++;
        }
    } else if (d->colon) {
        /* ~:{body~} — consume list of sublists */
        /* ~^ in sub-context only terminates current step, not whole iteration */
        CL_Obj list = fmt_next_arg(ctx);
        /* GC SAFETY: the list cursor is re-read after each fmt_run. */
        CL_GC_PROTECT(list);
        while (!CL_NULL_P(list) && iter_count < max_iter) {
            CL_Obj sublist = cl_car(list);
            int sbase;
            int sub_n = fmt_stage_list(sublist, &sbase, "~~:{");
            FmtCtx sub;

            sub.stream = ctx->stream;
            sub.fmt = body_copy;
            sub.pos = body_copy;
            sub.args = &cl_vm.stack[sbase];
            sub.nargs = sub_n;
            sub.ai = 0;
            sub.escape = 0;
            fmt_run(&sub);
            cl_vm.sp = sbase;
            /* ~^ only terminates current step for ~:{ */

            list = cl_cdr(list);
            iter_count++;
        }
        CL_GC_UNPROTECT(1);  /* list */
    } else {
        /* ~{body~} — consume list, elements become args.
         * CLHS 22.3.7.4: NIL → no iterations.  Non-list non-NIL is allowed
         * (SBCL behavior): treated as a non-empty source with no extractable
         * args, so iteration runs until V cap (or ~^ / FMT_MAX_ITERATIONS).
         * Termination otherwise: iteration consumed args AND exhausted them. */
        CL_Obj list = fmt_next_arg(ctx);
        int list_n = 0;
        CL_Obj list_args[256];
        int list_ai = 0;

        if (CL_NULL_P(list)) {
            platform_free(body_copy);
            ctx->pos = body_end;
            return;
        }

        if (CL_CONS_P(list)) {
            CL_Obj tmp = list;
            while (CL_CONS_P(tmp) && list_n < 256) {
                list_args[list_n++] = cl_car(tmp);
                tmp = cl_cdr(tmp);
            }
        }

        /* GC SAFETY: root the snapshot for the whole loop — fmt_run can
         * compact and the array is re-read across iterations. */
        {
            int k;
            for (k = 0; k < list_n; k++)
                CL_GC_PROTECT(list_args[k]);
        }
        while (iter_count < max_iter) {
            FmtCtx sub;
            int prev_ai;
            sub.stream = ctx->stream;
            sub.fmt = body_copy;
            sub.pos = body_copy;
            sub.args = list_args;
            sub.nargs = list_n;
            sub.ai = list_ai;
            sub.escape = 0;
            prev_ai = sub.ai;
            fmt_run(&sub);
            list_ai = sub.ai;
            if (sub.escape) break;
            iter_count++;
            if (sub.ai > prev_ai && sub.ai >= list_n) break;
        }
        cl_gc_pop_roots(list_n);
    }

    platform_free(body_copy);
    ctx->pos = body_end;
}

/* ================================================================
 * ~? — Recursive Format
 * ================================================================ */

static void fmt_recursive(FmtCtx *ctx, FmtDirective *d)
{
    CL_Obj fmt_str_obj = fmt_next_arg(ctx);
    char *alloc = NULL;
    const char *fmt;
    FmtCtx sub;

    /* The control argument may be a function (e.g. produced by FORMATTER) —
     * CLHS 22.3.5.3.  Apply it to the stream and the args; it performs its own
     * formatting.  ~? takes the args as a list; ~@? takes the remaining parent
     * args inline. */
    if (CL_FUNCTION_OBJ_P(fmt_str_obj) || cl_funcallable_instance_p(fmt_str_obj)) {
        /* cl_vm_apply pushes its frame over the VM-stack region that ctx->args
         * points into, clobbering the parent's not-yet-consumed args (so a
         * directive after ~? would read garbage).  Snapshot the parent args
         * into a GC-rooted vector across the apply, then copy the GC-updated
         * values back into the (stable) VM-stack arg slots.  A vector (not a
         * fixed C array) so >64-arg FORMAT calls keep all args — a 64-slot
         * snapshot silently dropped/garbled args past 64.  Scoped to this
         * rare function-control path so ordinary FORMAT pays nothing.
         *
         * The call args are staged on the VM stack (GC-rooted, no fixed cap);
         * cl_vm_apply copies them into its own frame before running any user
         * code, so this staging slice only needs to survive until the apply
         * starts. */
        CL_Obj saved_vec;
        CL_Obj *sd;
        int sn = ctx->nargs;
        int si;
        int cn = 0;
        int base, staged_sp;
        CL_GC_PROTECT(fmt_str_obj);
        saved_vec = cl_make_vector((uint32_t)sn);
        CL_GC_PROTECT(saved_vec);
        /* No allocation between here and the apply — sd stays valid and the
         * ctx->args slots read below are the (rooted, forwarded) originals. */
        sd = cl_vector_data((CL_Vector *)CL_OBJ_TO_PTR(saved_vec));
        for (si = 0; si < sn; si++)
            sd[si] = ctx->args[si];
        base = cl_vm.sp;
        staged_sp = cl_vm.sp;
        cl_vm_push(ctx->stream);
        cn++;
        if (d->atsign) {
            /* ~@? — pass remaining parent args inline.  The function returns
             * the unconsumed tail as a list (CLHS 22.3.6.4); use that to
             * advance ctx->ai by the number actually consumed rather than
             * assuming all remaining args were consumed. */
            int old_ai = ctx->ai;
            int n_passed;
            CL_Obj result;
            CL_Obj tmp;
            int remaining;
            while (ctx->ai < ctx->nargs) {
                if (cn >= CL_CALL_ARGS_LIMIT) {
                    cl_vm.sp = staged_sp;
                    cl_error(CL_ERR_ARGS,
                             "FORMAT ~~@?: too many arguments "
                             "(call-arguments-limit is %d)",
                             CL_CALL_ARGS_LIMIT);
                }
                cl_vm_push(sd[ctx->ai++]);
                cn++;
            }
            n_passed = cn - 1; /* exclude stream */
            result = cl_vm_apply(fmt_str_obj, &cl_vm.stack[base], cn);
            cl_vm.sp = staged_sp;
            remaining = 0;
            tmp = result;
            while (CL_CONS_P(tmp)) { remaining++; tmp = cl_cdr(tmp); }
            ctx->ai = old_ai + (n_passed - remaining);
        } else {
            /* ~? — next arg is a list of args for the control. */
            CL_Obj arg_list = fmt_next_arg(ctx);
            while (!CL_NULL_P(arg_list)) {
                if (cn >= CL_CALL_ARGS_LIMIT) {
                    cl_vm.sp = staged_sp;
                    cl_error(CL_ERR_ARGS,
                             "FORMAT ~~?: too many arguments in list "
                             "(call-arguments-limit is %d)",
                             CL_CALL_ARGS_LIMIT);
                }
                cl_vm_push(cl_car(arg_list));
                cn++;
                arg_list = cl_cdr(arg_list);
            }
            cl_vm_apply(fmt_str_obj, &cl_vm.stack[base], cn);
            cl_vm.sp = staged_sp;
        }
        /* Restore the parent args (relocated by any GC during the apply) into
         * their stable VM-stack slots so trailing directives read correctly. */
        sd = cl_vector_data((CL_Vector *)CL_OBJ_TO_PTR(saved_vec));
        for (si = 0; si < sn; si++)
            ctx->args[si] = sd[si];
        CL_GC_UNPROTECT(2);
        return;
    }

    if (!CL_ANY_STRING_P(fmt_str_obj)) {
        cl_error(CL_ERR_TYPE, "FORMAT ~?: argument must be a string or function");
        return;
    }
    fmt = fmt_str_as_utf8(fmt_str_obj, &alloc);

    sub.stream = ctx->stream;
    sub.fmt = fmt;
    sub.pos = fmt;
    sub.escape = 0;

    if (d->atsign) {
        /* ~@? — use remaining parent args */
        sub.args = ctx->args;
        sub.nargs = ctx->nargs;
        sub.ai = ctx->ai;
        fmt_run(&sub);
        ctx->ai = sub.ai;
    } else {
        /* ~? — consume format string + list of args, staged on the VM stack
         * (GC-rooted across the sub-run, uncapped). */
        CL_Obj arg_list = fmt_next_arg(ctx);
        int sbase;
        int sub_n = fmt_stage_list(arg_list, &sbase, "~~?");

        sub.args = &cl_vm.stack[sbase];
        sub.nargs = sub_n;
        sub.ai = 0;
        fmt_run(&sub);
        cl_vm.sp = sbase;
    }

    if (alloc) platform_free(alloc);
}

/* ================================================================
 * ~mincol,colinc,minpad,padchar<...~>  — Justification (CLHS 22.3.6.2)
 *
 * The body is divided into segments by ~; separators. Each segment is
 * rendered independently, then the segments are spread across a field of
 * at least `mincol` columns by inserting `padchar` padding in the gaps
 * between them:
 *
 *   - no modifiers:  segments are spread with the first against the left
 *                    margin and the last against the right margin
 *                    (padding only in the n-1 interior gaps).  A single
 *                    segment with no modifiers is right-justified.
 *   - `:` modifier:  padding is also inserted before the first segment.
 *   - `@` modifier:  padding is also inserted after the last segment.
 *
 * Padding is distributed as evenly as possible; when it does not divide
 * evenly, the later gaps receive the extra column(s) (matching SBCL).
 *
 * A leading `~:;` separator marks the first segment as the pretty-printer
 * overflow/per-line-prefix indicator; we do not track line width, so that
 * segment is skipped.  A `~<...~:>` logical block (mincol defaulting to 0)
 * reduces to emitting the segments in order, unchanged.
 *
 * Segment lengths are measured in CHARACTERS (not bytes) so that
 * multi-byte UTF-8 content (e.g. the scale glyphs ˫ + ˧) aligns correctly.
 * ================================================================ */

#define FMT_MAX_JSEGS (FMT_MAX_SEPS + 1)

/* Advance past a ~; separator directive. `sp` points at its leading '~';
 * the returned pointer is just past the ';' (the start of the next
 * segment). */
static const char *fmt_skip_separator(const char *sp)
{
    sp++; /* past '~' */
    while (*sp && ((*sp >= '0' && *sp <= '9') || *sp == ',' || *sp == '\'' ||
                   *sp == '+' || *sp == '-' || *sp == 'V' || *sp == 'v' ||
                   *sp == '#')) {
        if (*sp == '\'') { sp++; if (*sp) sp++; }
        else sp++;
    }
    while (*sp == ':' || *sp == '@') sp++;
    if (*sp == ';') sp++;
    return sp;
}

static void fmt_justify(FmtCtx *ctx, FmtDirective *d)
{
    const char *body_start = ctx->pos;
    const char *seps[FMT_MAX_SEPS];
    int n_seps = 0;
    const char *body_end;
    int first_seg_is_overflow = 0;
    int total_segs;
    int i;

    const char *seg_starts[FMT_MAX_JSEGS];
    const char *seg_ends[FMT_MAX_JSEGS];
    uint32_t *seg_cps[FMT_MAX_JSEGS];   /* rendered text as code points */
    int   seg_clen[FMT_MAX_JSEGS];      /* character count */
    int   n_segs = 0;

    int32_t mincol  = fmt_param(d, 0, 0);
    int32_t colinc  = fmt_param(d, 1, 1);
    int32_t minpad  = fmt_param(d, 2, 0);
    int32_t padchar = fmt_param(d, 3, (int32_t)' ');

    int pad_before, pad_after, n_gaps;
    int text_len, needed, field, total_pad, base, extra;
    int gap_idx;
    FmtCtx sub;

    if (colinc < 1) colinc = 1;
    if (minpad < 0)  minpad = 0;

    body_end = fmt_find_close(body_start, '<', '>', seps, &n_seps, FMT_MAX_SEPS);
    if (!body_end) {
        cl_error(CL_ERR_GENERAL, "FORMAT: unmatched ~<");
        return;
    }

    /* Detect a leading ~:; (overflow / per-line-prefix segment). */
    if (n_seps > 0) {
        const char *sp = seps[0] + 1; /* past '~' */
        while (*sp && ((*sp >= '0' && *sp <= '9') || *sp == ',' || *sp == '\'' ||
                       *sp == '+' || *sp == '-' || *sp == 'V' || *sp == 'v' ||
                       *sp == '#')) {
            if (*sp == '\'') { sp++; if (*sp) sp++; }
            else sp++;
        }
        if (*sp == ':') first_seg_is_overflow = 1;
    }

    /* Compute the byte range of every segment within the format string.
     * seps[i] points at the '~' of the i-th separator; the body ends two
     * chars before body_end (the '~' of the closing '~>'). */
    total_segs = n_seps + 1;
    {
        const char *cur = body_start;
        for (i = 0; i < total_segs; i++) {
            seg_starts[i] = cur;
            if (i < n_seps) {
                seg_ends[i] = seps[i];
                cur = fmt_skip_separator(seps[i]);
            } else {
                seg_ends[i] = body_end - 2;
            }
        }
    }

    /* Render each segment (skipping the overflow segment if present) into
     * its own C-heap string, recording byte and character lengths.  Arg
     * consumption flows across the segments via the shared sub context. */
    sub = *ctx;
    for (i = first_seg_is_overflow ? 1 : 0; i < total_segs; i++) {
        CL_Obj sstream, result;
        int blen = (int)(seg_ends[i] - seg_starts[i]);
        char *body_copy;
        int rlen;
        uint32_t *cps;
        int j;

        if (blen < 0) blen = 0;
        body_copy = (char *)fmt_alloc((uint32_t)blen + 1);
        memcpy(body_copy, seg_starts[i], (uint32_t)blen);
        body_copy[blen] = '\0';

        sstream = cl_make_string_output_stream();
        CL_GC_PROTECT(sstream);
        sub.stream = sstream;
        sub.fmt = body_copy;
        sub.pos = body_copy;
        sub.escape = 0;
        fmt_run(&sub);
        platform_free(body_copy);

        result = cl_finish_string_output_stream(sstream);
        /* Copy the code points out of the arena into C heap.  Using the
         * character accessors keeps base and wide (UTF-32) result strings
         * correct — the segment length must be measured in characters for
         * the padding math, and each character re-emitted via
         * cl_stream_write_char (which re-encodes wide code points). */
        rlen = (int)cl_string_length(result);
        cps = (uint32_t *)fmt_alloc((uint32_t)(rlen > 0 ? rlen : 1) *
                                         (uint32_t)sizeof(uint32_t));
        for (j = 0; j < rlen; j++)
            cps[j] = (uint32_t)cl_string_char_at(result, (uint32_t)j);
        CL_GC_UNPROTECT(1);

        seg_cps[n_segs] = cps;
        seg_clen[n_segs] = rlen;
        n_segs++;
    }

    /* Determine the padding gaps. */
    pad_before = d->colon ? 1 : 0;
    pad_after  = d->atsign ? 1 : 0;
    if (n_segs <= 1) {
        /* A single segment with no modifiers is right-justified. */
        if (!pad_before && !pad_after) pad_before = 1;
        n_gaps = pad_before + pad_after;
    } else {
        n_gaps = pad_before + pad_after + (n_segs - 1);
    }
    if (n_gaps < 1) n_gaps = 1; /* defensive: never divide by zero */

    text_len = 0;
    for (i = 0; i < n_segs; i++) text_len += seg_clen[i];

    /* Field width = smallest mincol + k*colinc (k>=0) that holds the text
     * plus the minimum required padding. */
    needed = text_len + minpad * n_gaps;
    field = mincol;
    while (field < needed) field += colinc;
    total_pad = field - text_len;
    if (total_pad < 0) total_pad = 0;
    base  = total_pad / n_gaps;
    extra = total_pad % n_gaps;

    /* Emit: optional leading pad, segments with interior pads, optional
     * trailing pad.  The last `extra` gaps each get one more padchar. */
    gap_idx = 0;
#define FMT_EMIT_GAP() do {                                                   \
        int _amt = base + ((gap_idx >= n_gaps - extra) ? 1 : 0);             \
        int _k;                                                              \
        for (_k = 0; _k < _amt; _k++)                                         \
            cl_stream_write_char(ctx->stream, (int)padchar);                  \
        gap_idx++;                                                            \
    } while (0)

    if (pad_before) FMT_EMIT_GAP();
    for (i = 0; i < n_segs; i++) {
        int j;
        for (j = 0; j < seg_clen[i]; j++)
            cl_stream_write_char(ctx->stream, (int)seg_cps[i][j]);
        platform_free(seg_cps[i]);
        if (i < n_segs - 1) FMT_EMIT_GAP();
    }
    if (pad_after) FMT_EMIT_GAP();
#undef FMT_EMIT_GAP

    /* Update parent state. */
    ctx->ai = sub.ai;
    ctx->pos = body_end;
}

/* ================================================================
 * ~F and ~$ — fixed-format floating point (CLHS 22.3.3.1, 22.3.3.4)
 * ================================================================ */

static int fmt_param_given(FmtDirective *d, int idx)
{
    return idx < d->n_params && idx < FMT_MAX_PARAMS && d->param_given[idx];
}

static int fmt_is_real(CL_Obj arg)
{
    return CL_FIXNUM_P(arg) ||
           (CL_HEAP_P(arg) &&
            (CL_HDR_TYPE(CL_OBJ_TO_PTR(arg)) == TYPE_SINGLE_FLOAT ||
             CL_HDR_TYPE(CL_OBJ_TO_PTR(arg)) == TYPE_DOUBLE_FLOAT ||
             CL_HDR_TYPE(CL_OBJ_TO_PTR(arg)) == TYPE_RATIO ||
             CL_HDR_TYPE(CL_OBJ_TO_PTR(arg)) == TYPE_BIGNUM));
}

static void fmt_write_n(FmtCtx *ctx, int ch, int32_t n)
{
    while (n-- > 0)
        cl_stream_write_char(ctx->stream, ch);
}

/* Print `buf` (digits) right-justified in a field of `w` columns:
   padding, then the sign character (0 for none), then the digits.
   With `sign_first` the sign goes before the padding (~:$). */
static void fmt_write_field(FmtCtx *ctx, int sign, const char *buf, int len,
                            int32_t w, int padchar, int sign_first)
{
    int32_t pad = w - (len + (sign ? 1 : 0));
    int i;
    if (sign && sign_first)
        cl_stream_write_char(ctx->stream, sign);
    fmt_write_n(ctx, padchar, pad);
    if (sign && !sign_first)
        cl_stream_write_char(ctx->stream, sign);
    for (i = 0; i < len; i++)
        cl_stream_write_char(ctx->stream, buf[i]);
}

/* A non-real argument to ~F / ~$ is printed as by ~wD: an object that
   isn't an integer comes out as by ~A, right-justified in w columns. */
static void fmt_fixed_non_number(FmtCtx *ctx, CL_Obj arg, int32_t w)
{
    char buf[400];
    int len = cl_princ_to_string(arg, buf, sizeof(buf));
    if (len < 0) len = 0;
    if (len > (int)sizeof(buf) - 1) len = (int)sizeof(buf) - 1;
    fmt_write_field(ctx, 0, buf, len, w, ' ', 0);
}

/* snprintf "%.*f" into buf; returns the length, clamped to the buffer. */
static int fmt_fixed_digits(char *buf, int bufsz, double val, int32_t d)
{
    int len;
    if (d > 300) d = 300;
    len = snprintf(buf, bufsz, "%.*f", (int)d, val);
    if (len < 0) len = 0;
    if (len > bufsz - 1) len = bufsz - 1;
    return len;
}

/* Remove a leading '-' from buf (the sign is emitted separately, so that
   padding goes in front of it); returns the new length. -0.0 keeps its
   sign this way. */
static int fmt_strip_minus(char *buf, int len, int *neg)
{
    *neg = (len > 0 && buf[0] == '-');
    if (*neg) {
        memmove(buf, buf + 1, (size_t)len);  /* includes the NUL */
        len--;
    }
    return len;
}

/* Guarantee a decimal point: "%.0f" gives "3", CLHS wants "3.". */
static int fmt_ensure_point(char *buf, int len, int bufsz)
{
    if (!memchr(buf, '.', (size_t)len) && len < bufsz - 1) {
        buf[len++] = '.';
        buf[len] = '\0';
    }
    return len;
}

/* ~w,d,k,overflowchar,padcharF */
static void fmt_fixed(FmtCtx *ctx, FmtDirective *d)
{
    CL_Obj arg = fmt_next_arg(ctx);
    int w_given = fmt_param_given(d, 0);
    int32_t w = fmt_param(d, 0, 0);
    int d_given = fmt_param_given(d, 1);
    int32_t digs = fmt_param(d, 1, 0);
    int32_t k = fmt_param(d, 2, 0);
    int padchar = (int)fmt_param(d, 4, (int32_t)' ');
    char buf[400];
    int len, neg, sign, signlen;
    double val;
    int as_double;

    if (!fmt_is_real(arg)) {
        fmt_fixed_non_number(ctx, arg, w);
        return;
    }
    val = cl_to_double(arg);
    as_double = !(CL_HEAP_P(arg) &&
                  CL_HDR_TYPE(CL_OBJ_TO_PTR(arg)) == TYPE_SINGLE_FLOAT);
    if (k != 0) {
        /* Scale factor: the number printed is arg * 10^k. */
        int32_t i;
        for (i = 0; i < k && i < 400; i++) val *= 10.0;
        for (i = 0; i > k && i > -400; i--) val /= 10.0;
    }

    if (d_given) {
        len = fmt_fixed_digits(buf, sizeof(buf), val, digs);
    } else {
        /* No d: free-format shortest round-trip digits, fixed-point only
           ("87.0", never "87" and never an exponent). */
        len = cl_float_fixed_shortest(buf, (int)sizeof(buf), val, as_double);
        if (len < 0) len = 0;
        if (len > (int)sizeof(buf) - 1) len = (int)sizeof(buf) - 1;
        buf[len] = '\0';
        if (w_given) {
            /* w without d: as many fraction digits as fit in w. */
            int n0 = (buf[0] == '-');
            int room = (int)w - ((n0 || d->atsign) ? 1 : 0);
            if (len - n0 > room) {
                const char *dot = strchr(buf, '.');
                int intlen = dot ? (int)(dot - buf) - n0 : len - n0;
                int32_t dd = room - intlen - 1;
                if (intlen == 1 && buf[n0] == '0')
                    dd++;  /* the leading zero will be dropped */
                if (dd < 0) dd = 0;
                if (dd == 0 && intlen == 1 && buf[n0] == '0')
                    dd = 1;  /* ".3", not "0." */
                len = fmt_fixed_digits(buf, sizeof(buf), val, dd);
            }
        }
    }
    len = fmt_strip_minus(buf, len, &neg);
    len = fmt_ensure_point(buf, len, (int)sizeof(buf));
    sign = neg ? '-' : (d->atsign ? '+' : 0);
    signlen = sign ? 1 : 0;

    /* The leading zero of a pure fraction is optional: drop it when it
       doesn't fit in w. */
    if (w_given && signlen + len > w && len > 1 && buf[0] == '0' && buf[1] == '.') {
        memmove(buf, buf + 1, (size_t)len);
        len--;
    }
    if (w_given && signlen + len > w && fmt_param_given(d, 3)) {
        fmt_write_n(ctx, (int)fmt_param(d, 3, '*'), w);
        return;
    }
    fmt_write_field(ctx, sign, buf, len, w, padchar, 0);
}

/* ~d,n,w,padchar$ */
static void fmt_dollar(FmtCtx *ctx, FmtDirective *d)
{
    CL_Obj arg = fmt_next_arg(ctx);
    int32_t digs = fmt_param(d, 0, 2);
    int32_t n = fmt_param(d, 1, 1);
    int32_t w = fmt_param(d, 2, 0);
    int padchar = (int)fmt_param(d, 3, (int32_t)' ');
    char buf[400];
    int len, neg, sign, intlen;
    const char *dot;

    if (!fmt_is_real(arg)) {
        fmt_fixed_non_number(ctx, arg, w);
        return;
    }
    len = fmt_fixed_digits(buf, sizeof(buf), cl_to_double(arg), digs);
    len = fmt_strip_minus(buf, len, &neg);
    len = fmt_ensure_point(buf, len, (int)sizeof(buf));
    sign = neg ? '-' : (d->atsign ? '+' : 0);

    /* At least n digits before the point: leading zeros, or with n = 0
       no "0" at all in front of a pure fraction. */
    dot = strchr(buf, '.');
    intlen = dot ? (int)(dot - buf) : len;
    if (n == 0 && intlen == 1 && buf[0] == '0') {
        memmove(buf, buf + 1, (size_t)len);
        len--;
    } else if (n > intlen) {
        int32_t z = n - intlen;
        if (z > (int32_t)sizeof(buf) - 1 - len)
            z = (int32_t)sizeof(buf) - 1 - len;
        memmove(buf + z, buf, (size_t)len + 1);
        memset(buf, '0', (size_t)z);
        len += (int)z;
    }
    fmt_write_field(ctx, sign, buf, len, w, padchar, d->colon);
}

/* ================================================================
 * Dispatch table
 * ================================================================ */

static void fmt_dispatch(FmtCtx *ctx, FmtDirective *d)
{
    switch (d->directive) {
    case 'A':
        fmt_padded_obj(ctx, d, 0);
        break;
    case 'S':
        fmt_padded_obj(ctx, d, 1);
        break;
    case 'W':
        if (ctx->ai < ctx->nargs)
            cl_write_to_stream(ctx->args[ctx->ai++], ctx->stream);
        break;
    case 'D':
        fmt_padded_integer(ctx, d, 10);
        break;
    case 'B':
        fmt_padded_integer(ctx, d, 2);
        break;
    case 'O':
        fmt_padded_integer(ctx, d, 8);
        break;
    case 'X':
        fmt_padded_integer(ctx, d, 16);
        break;
    case 'R':
        fmt_radix(ctx, d);
        break;
    case 'C':
        /* ~C: character */
        if (ctx->ai < ctx->nargs)
            cl_princ_to_stream(ctx->args[ctx->ai++], ctx->stream);
        break;
    case '%': {
        int32_t count = fmt_param(d, 0, 1);
        int i;
        for (i = 0; i < count; i++)
            cl_stream_write_char(ctx->stream, '\n');
        break;
    }
    case '&': {
        /* ~&: fresh-line. ~n& = one fresh-line then n-1 newlines */
        int32_t count = fmt_param(d, 0, 1);
        CL_Stream *st = (CL_Stream *)CL_OBJ_TO_PTR(ctx->stream);
        int i;
        if (count > 0) {
            if (st->charpos != 0)
                cl_stream_write_char(ctx->stream, '\n');
            for (i = 1; i < count; i++)
                cl_stream_write_char(ctx->stream, '\n');
        }
        break;
    }
    case '|': {
        int32_t count = fmt_param(d, 0, 1);
        int i;
        for (i = 0; i < count; i++)
            cl_stream_write_char(ctx->stream, '\f');
        break;
    }
    case '~': {
        int32_t count = fmt_param(d, 0, 1);
        int i;
        for (i = 0; i < count; i++)
            cl_stream_write_char(ctx->stream, '~');
        break;
    }
    case 'P': {
        /* ~P: plural — "s" if arg /= 1, else ""
           ~:P: backup then plural — backs up one arg, then plural
           ~@P: "y"/"ies" — "y" if arg == 1, else "ies" */
        CL_Obj arg;
        int is_one;
        if (d->colon && ctx->ai > 0)
            ctx->ai--;  /* back up one arg */
        arg = (ctx->ai < ctx->nargs) ? ctx->args[ctx->ai++] : CL_NIL;
        is_one = (CL_FIXNUM_P(arg) && CL_FIXNUM_VAL(arg) == 1);
        if (d->atsign) {
            if (is_one)
                cl_stream_write_char(ctx->stream, 'y');
            else {
                cl_stream_write_char(ctx->stream, 'i');
                cl_stream_write_char(ctx->stream, 'e');
                cl_stream_write_char(ctx->stream, 's');
            }
        } else {
            if (!is_one)
                cl_stream_write_char(ctx->stream, 's');
        }
        break;
    }
    case 'F':
        fmt_fixed(ctx, d);
        break;
    case '$':
        fmt_dollar(ctx, d);
        break;
    case 'E': {
        /* ~E: exponential format — simplified */
        CL_Obj arg = fmt_next_arg(ctx);
        double val = cl_to_double(arg);
        char buf[64];
        snprintf(buf, sizeof(buf), "%E", val);
        {
            const char *p = buf;
            while (*p) cl_stream_write_char(ctx->stream, *p++);
        }
        break;
    }
    case 'T':
        fmt_tabulate(ctx, d);
        break;
    case '*':
        fmt_goto(ctx, d);
        break;
    case '(':
        fmt_case_convert(ctx, d);
        break;
    case '[':
        fmt_conditional(ctx, d);
        break;
    case '{':
        fmt_iteration(ctx, d);
        break;
    case '^':
        /* ~^ — escape from iteration if no args remain */
        if (fmt_args_remaining(ctx) == 0)
            ctx->escape = 1;
        break;
    case '?':
        fmt_recursive(ctx, d);
        break;
    case '<':
        fmt_justify(ctx, d);
        break;
    case '_':
        /* Pretty-printer conditional newline (~_ ~:_ ~@_ ~:@_). We don't
         * track columns, so emit nothing — printable layout still ok. */
        break;
    case 'I':
        /* Pretty-printer indent (~I ~:I) — no-op without pretty printer. */
        break;
    case '\n':
        /* ~<newline> — ignore newline and any following whitespace */
        if (!d->colon) {
            while (*ctx->pos == ' ' || *ctx->pos == '\t')
                ctx->pos++;
        }
        break;
    default:
        /* Unknown directive — output literally */
        cl_stream_write_char(ctx->stream, '~');
        cl_stream_write_char(ctx->stream, d->directive);
        break;
    }
}

/* ================================================================
 * Main format loop
 * ================================================================ */

static void fmt_run(FmtCtx *ctx)
{
    /* GC-protect the destination stream IN PLACE for the whole run.  A
     * string-output-stream is an arena object, and directives like ~A/~S print
     * arbitrary objects whose printer (or a user print-object method) can
     * trigger a compacting GC mid-run.  ctx->stream is a bare CL_Obj offset in
     * the caller's FmtCtx; without rooting &ctx->stream a compaction would
     * relocate the stream and leave the offset stale, so every write after the
     * first relocating directive would land in freed/moved memory and be lost
     * ("[~a]" yields "[" instead of "[msg]" under GC stress).  Protecting here
     * (rather than at the cl_format_to_stream entry) also covers every nested
     * sub-context — ~{ ~( ~[ ~< ~? each run through fmt_run with their own
     * copied stream field.  (stdout/stderr don't move, so only string streams
     * were affected.) */
    CL_GC_PROTECT(ctx->stream);
    while (*ctx->pos && !ctx->escape) {
        unsigned char b = (unsigned char)*ctx->pos;
        if (b == '~') {
            FmtDirective d;
            ctx->pos++; /* skip '~' */
            if (!*ctx->pos) break;
            fmt_parse_directive(ctx, &d);
            fmt_dispatch(ctx, &d);
        } else if (b < 0x80) {
            cl_stream_write_char(ctx->stream, (int)b);
            ctx->pos++;
        } else {
#ifdef CL_WIDE_STRINGS
            /* UTF-8 lead byte — decode the codepoint so the stream re-encodes
             * it consistently (and charpos advances by one character, not
             * one byte). */
            const unsigned char *bytes = (const unsigned char *)ctx->pos;
            uint32_t avail = 0;
            int cp = 0xFFFD;
            int nb;
            while (avail < 4 && bytes[avail] != 0) avail++;
            nb = cl_utf8_decode(bytes, avail, &cp);
            if (nb < 1) nb = 1;
            cl_stream_write_char(ctx->stream, cp);
            ctx->pos += nb;
#else
            cl_stream_write_char(ctx->stream, (int)b);
            ctx->pos++;
#endif
        }
    }
    CL_GC_UNPROTECT(1);
}

/* ================================================================
 * %FORMATTER-INNER — internal helper for the FORMATTER macro (CLHS 22.3.6.4)
 *
 * (amilisp::%formatter-inner stream ctrl-str args-list) -> remaining-args-list
 *
 * Applies the format control string CTRL-STR to ARGS-LIST, writing to STREAM,
 * and returns the unconsumed argument tail as a list.  This lets the function
 * produced by FORMATTER report which args it did not consume, so ~@? can
 * advance the parent arg index correctly instead of assuming all remaining
 * args were consumed.
 *
 * Registered lazily on the first cl_format_to_stream call so the symbol
 * exists before any boot.lisp formatter lambda is invoked.
 * ================================================================ */

static CL_Obj bi__formatter_inner(CL_Obj *args, int nargs)
{
    CL_Obj stream, ctrl, arg_list;
    char *alloc = NULL;
    const char *fmt;
    FmtCtx ctx;
    int n = 0, i, consumed;
    CL_Obj tmp;
    int base, staged_sp;

    if (nargs < 3) return CL_NIL;
    stream   = args[0];
    ctrl     = args[1];
    arg_list = args[2];

    if (!CL_ANY_STRING_P(ctrl)) {
        cl_error(CL_ERR_TYPE, "%formatter-inner: control must be a string");
        return CL_NIL;
    }

    /* Stage stream+ctrl+args on the VM stack: GC-rooted for the whole
     * fmt_run (which can compact), and uncapped — a fixed 64-slot C buffer
     * silently dropped format args past 64. */
    base = cl_vm.sp;
    staged_sp = cl_vm.sp;
    cl_vm_push(stream);
    cl_vm_push(ctrl);
    tmp = arg_list;
    while (!CL_NULL_P(tmp) && CL_CONS_P(tmp)) {
        if (2 + n >= CL_CALL_ARGS_LIMIT) {
            cl_vm.sp = staged_sp;
            cl_error(CL_ERR_ARGS,
                     "%%formatter-inner: too many format arguments "
                     "(call-arguments-limit is %d)", CL_CALL_ARGS_LIMIT);
        }
        cl_vm_push(cl_car(tmp));
        n++;
        tmp = cl_cdr(tmp);
    }

    /* Root the original arg_list — the unconsumed tail is derived from it
     * after fmt_run. */
    cl_gc_push_root(&arg_list);

    fmt = fmt_str_as_utf8(cl_vm.stack[base + 1], &alloc);

    ctx.stream = cl_vm.stack[base];
    ctx.fmt    = fmt;
    ctx.pos    = fmt;
    ctx.args   = &cl_vm.stack[base];
    ctx.nargs  = 2 + n;
    ctx.ai     = 2;
    ctx.escape = 0;

    fmt_run(&ctx);

    consumed = ctx.ai - 2;
    cl_vm.sp = staged_sp;
    cl_gc_pop_roots(1); /* arg_list */
    if (alloc) platform_free(alloc);

    /* Return nthcdr(consumed, arg_list) — the unconsumed argument tail. */
    tmp = arg_list;
    for (i = 0; i < consumed && CL_CONS_P(tmp); i++)
        tmp = cl_cdr(tmp);
    return tmp;
}

/* Register %FORMATTER-INNER, the helper the FORMATTER macro expands into.
 * Called from cl_builtins_io_init so it is bound before any FORMATTER lambda is
 * invoked (including a direct funcall before any FORMAT call). */
void cl_format_builtins_init(void)
{
    cl_register_builtin("%FORMATTER-INNER", bi__formatter_inner,
                        3, 3, cl_package_amilisp);
}

/* ================================================================
 * Public entry point — called from builtins_io.c
 * ================================================================ */

void cl_format_to_stream(CL_Obj stream, CL_Obj *args, int n)
{
    char *alloc = NULL;
    const char *fmt;
    FmtCtx ctx;

    if (n < 2 || !CL_ANY_STRING_P(args[1]))
        return;

    fmt = fmt_str_as_utf8(args[1], &alloc);

    ctx.stream = stream;
    ctx.fmt = fmt;
    ctx.pos = fmt;
    ctx.args = args;
    ctx.nargs = n;
    ctx.ai = 2;  /* args start after destination and format string */
    ctx.escape = 0;

    fmt_run(&ctx);

    if (alloc) platform_free(alloc);
}
