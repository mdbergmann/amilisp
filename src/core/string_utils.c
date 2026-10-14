#include "string_utils.h"
#include "mem.h"
#include "../platform/platform.h"
#include <string.h>
#include <stdio.h>

/* Materialize a range [start,end) of an adjustable/fill-pointer character
 * vector (CL_STRING_VECTOR_P) into a fresh simple string.  Picks a wide
 * string if any char exceeds 255 (when wide strings are enabled), else a
 * base TYPE_STRING.  Allocates — GC-protects vobj across the allocation and
 * re-fetches the data pointer afterward (the source may have moved). */
static CL_Obj string_vector_range_to_string(CL_Obj vobj, uint32_t start, uint32_t end)
{
    uint32_t i, n = end - start;
    CL_Obj result;
    CL_Vector *v;
    CL_Obj *data;
    CL_GC_PROTECT(vobj);
#ifdef CL_WIDE_STRINGS
    {
        int wide = 0;
        v = (CL_Vector *)CL_OBJ_TO_PTR(vobj);
        data = cl_vector_data(v);
        for (i = 0; i < n; i++) {
            CL_Obj ch = data[start + i];
            if (CL_CHAR_P(ch) && (uint32_t)CL_CHAR_VAL(ch) > 255) { wide = 1; break; }
        }
        result = wide ? cl_make_wide_string(NULL, n) : cl_make_string(NULL, n);
    }
#else
    result = cl_make_string(NULL, n);
#endif
    CL_GC_UNPROTECT(1);
    v = (CL_Vector *)CL_OBJ_TO_PTR(vobj);
    data = cl_vector_data(v);
    for (i = 0; i < n; i++) {
        CL_Obj ch = data[start + i];
        cl_string_set_char_at(result, i, CL_CHAR_P(ch) ? CL_CHAR_VAL(ch) : 0);
    }
    return result;
}

CL_Obj cl_string_copy(CL_Obj str)
{
#ifdef CL_WIDE_STRINGS
    if (CL_WIDE_STRING_P(str)) {
        CL_WideString *ws = (CL_WideString *)CL_OBJ_TO_PTR(str);
        return cl_make_wide_string(ws->data, ws->length);
    }
#endif
    if (CL_STRING_VECTOR_P(str))
        return string_vector_range_to_string(str, 0, cl_string_length(str));
    {
        CL_String *s = (CL_String *)CL_OBJ_TO_PTR(str);
        return cl_make_string(s->data, s->length);
    }
}

uint32_t cl_string_to_utf8(CL_Obj str, char *buf, uint32_t buf_size)
{
#ifdef CL_WIDE_STRINGS
    if (CL_WIDE_STRING_P(str))
        return cl_wide_string_to_utf8(str, buf, buf_size);
#endif
    {
        CL_String *s = (CL_String *)CL_OBJ_TO_PTR(str);
        uint32_t n = s->length;
        uint32_t limit = (buf_size > 0) ? buf_size - 1 : 0;
        if (n > limit) n = limit;
        memcpy(buf, s->data, n);
        if (buf_size > 0) buf[n] = '\0';
        return n;
    }
}

CL_Obj cl_string_substring(CL_Obj str, uint32_t start, uint32_t end)
{
#ifdef CL_WIDE_STRINGS
    if (CL_WIDE_STRING_P(str)) {
        CL_WideString *ws = (CL_WideString *)CL_OBJ_TO_PTR(str);
        return cl_make_wide_string(ws->data + start, end - start);
    }
#endif
    if (CL_STRING_VECTOR_P(str))
        return string_vector_range_to_string(str, start, end);
    {
        CL_String *s = (CL_String *)CL_OBJ_TO_PTR(str);
        return cl_make_string(s->data + start, end - start);
    }
}

#ifdef CL_WIDE_STRINGS

CL_Obj cl_string_widen(CL_Obj base_str)
{
    CL_String *s = (CL_String *)CL_OBJ_TO_PTR(base_str);
    uint32_t len = s->length;
    uint32_t i;
    CL_WideString *ws;
    CL_Obj result;

    CL_GC_PROTECT(base_str);
    result = cl_make_wide_string(NULL, len);
    CL_GC_UNPROTECT(1);

    /* Re-deref after allocation (GC may have moved nothing, but base_str
       is arena-relative so pointer is still valid) */
    s = (CL_String *)CL_OBJ_TO_PTR(base_str);
    ws = (CL_WideString *)CL_OBJ_TO_PTR(result);
    for (i = 0; i < len; i++)
        ws->data[i] = (uint32_t)(unsigned char)s->data[i];
    return result;
}

CL_Obj cl_string_narrow(CL_Obj wide_str)
{
    CL_WideString *ws = (CL_WideString *)CL_OBJ_TO_PTR(wide_str);
    uint32_t len = ws->length;
    uint32_t i;
    CL_String *s;
    CL_Obj result;

    /* Check if all characters fit in a byte */
    for (i = 0; i < len; i++) {
        if (ws->data[i] > 255)
            return wide_str;  /* Can't narrow */
    }

    CL_GC_PROTECT(wide_str);
    result = cl_make_string(NULL, len);
    CL_GC_UNPROTECT(1);

    ws = (CL_WideString *)CL_OBJ_TO_PTR(wide_str);
    s = (CL_String *)CL_OBJ_TO_PTR(result);
    for (i = 0; i < len; i++)
        s->data[i] = (char)ws->data[i];
    s->data[len] = '\0';
    return result;
}

#endif /* CL_WIDE_STRINGS */

/* --- UTF-8 codec ---
 * cl_utf8_encode / cl_utf8_decode are pure codecs with no wide-string
 * dependency, and are used unconditionally across the runtime (builtins_io,
 * builtins_pathname, builtins_stream, printer, reader, stream, ...) — including
 * in non-wide (Amiga) builds.  They therefore live OUTSIDE the CL_WIDE_STRINGS
 * guard; without this the Amiga build fails to link (undefined cl_utf8_encode).
 */

int cl_utf8_encode(int cp, char *buf)
{
    if (cp < 0) return 0;
    if (cp <= 0x7F) {
        buf[0] = (char)cp;
        return 1;
    }
    if (cp <= 0x7FF) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp <= 0xFFFF) {
        /* Reject surrogates (U+D800..U+DFFF) */
        if (cp >= 0xD800 && cp <= 0xDFFF) return 0;
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp <= 0x10FFFF) {
        buf[0] = (char)(0xF0 | (cp >> 18));
        buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;  /* Code point out of range */
}

int cl_utf8_decode(const unsigned char *buf, uint32_t buflen, int *codepoint)
{
    unsigned char b0;
    int cp, expected;

    if (buflen == 0) return 0;
    b0 = buf[0];

    if (b0 <= 0x7F) {
        *codepoint = b0;
        return 1;
    }
    if ((b0 & 0xE0) == 0xC0) {
        cp = b0 & 0x1F;
        expected = 2;
    } else if ((b0 & 0xF0) == 0xE0) {
        cp = b0 & 0x0F;
        expected = 3;
    } else if ((b0 & 0xF8) == 0xF0) {
        cp = b0 & 0x07;
        expected = 4;
    } else {
        /* Invalid lead byte or continuation byte */
        *codepoint = 0xFFFD;  /* replacement character */
        return 1;
    }

    if ((uint32_t)expected > buflen) {
        /* Truncated sequence */
        *codepoint = 0xFFFD;
        return 1;
    }

    {
        int i;
        for (i = 1; i < expected; i++) {
            if ((buf[i] & 0xC0) != 0x80) {
                /* Invalid continuation byte */
                *codepoint = 0xFFFD;
                return 1;
            }
            cp = (cp << 6) | (buf[i] & 0x3F);
        }
    }

    /* Reject overlong encodings */
    if ((expected == 2 && cp < 0x80) ||
        (expected == 3 && cp < 0x800) ||
        (expected == 4 && cp < 0x10000)) {
        *codepoint = 0xFFFD;
        return expected;
    }

    /* Reject surrogates and out-of-range */
    if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
        *codepoint = 0xFFFD;
        return expected;
    }

    *codepoint = cp;
    return expected;
}

#ifdef CL_WIDE_STRINGS  /* wide-string <-> UTF-8 conversion helpers */

uint32_t cl_wide_string_to_utf8(CL_Obj wide_str, char *buf, uint32_t buf_size)
{
    CL_WideString *ws = (CL_WideString *)CL_OBJ_TO_PTR(wide_str);
    uint32_t i, pos = 0;
    uint32_t limit = (buf_size > 0) ? buf_size - 1 : 0;

    for (i = 0; i < ws->length && pos < limit; i++) {
        char tmp[4];
        int nb = cl_utf8_encode((int)ws->data[i], tmp);
        int j;
        if (nb == 0) nb = 1, tmp[0] = '?';  /* fallback for invalid */
        if (pos + (uint32_t)nb > limit) break;
        for (j = 0; j < nb; j++)
            buf[pos++] = tmp[j];
    }
    if (buf_size > 0) buf[pos] = '\0';
    return pos;
}

CL_Obj cl_utf8_to_cl_string(const char *buf, uint32_t len)
{
    uint32_t i;
    int has_multibyte = 0;

    /* Quick scan: if all bytes <= 0x7F, it's pure ASCII → base string */
    for (i = 0; i < len; i++) {
        if ((unsigned char)buf[i] > 0x7F) {
            has_multibyte = 1;
            break;
        }
    }

    if (!has_multibyte)
        return cl_make_string(buf, len);

    /* Decode UTF-8 into a wide string */
    {
        /* First pass: count code points */
        uint32_t cp_count = 0;
        uint32_t pos = 0;
        while (pos < len) {
            int cp;
            int nb = cl_utf8_decode((const unsigned char *)buf + pos,
                                    len - pos, &cp);
            if (nb == 0) break;
            cp_count++;
            pos += (uint32_t)nb;
        }

        /* Allocate wide string and decode again */
        {
            CL_Obj result = cl_make_wide_string(NULL, cp_count);
            CL_WideString *ws = (CL_WideString *)CL_OBJ_TO_PTR(result);
            uint32_t ci = 0;
            pos = 0;
            while (pos < len && ci < cp_count) {
                int cp;
                int nb = cl_utf8_decode((const unsigned char *)buf + pos,
                                        len - pos, &cp);
                if (nb == 0) break;
                ws->data[ci++] = (uint32_t)cp;
                pos += (uint32_t)nb;
            }
            return result;
        }
    }
}

#endif /* CL_WIDE_STRINGS */

/* platform.h: the environment lookup with the pre-rename fallback.  An
 * AMILISP_<X> variable that is not set is looked up as CLAMIGA_<X> (what it
 * was called before 0.12); any other name is passed through unchanged. */
const char *platform_getenv(const char *name, char *buf, int bufsize)
{
    const char *v = platform_getenv_raw(name, buf, bufsize);
    /* An empty value counts as unset (the runtime's convention, so a child
     * can clear an inherited export with AMILISP_X=), and an unset new name
     * falls back to the old one. */
    if ((v == NULL || v[0] == '\0') && strncmp(name, "AMILISP_", 8) == 0) {
        char legacy[128];
        if (snprintf(legacy, sizeof(legacy), "CLAMIGA_%s", name + 8)
            < (int)sizeof(legacy))
            v = platform_getenv_raw(legacy, buf, bufsize);
    }
    return v;
}

/* platform_getenv for one-shot reads where the caller has no buffer of its
 * own (startup and diagnostic switches): the value lives in a static buffer
 * until the next call, so it is not for concurrent or repeated use. */
const char *platform_getenv_s(const char *name)
{
    static char buf[256];
    return platform_getenv(name, buf, (int)sizeof(buf));
}
