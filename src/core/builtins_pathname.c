/*
 * builtins_pathname.c — Pathname type: parsing, construction, accessors
 *
 * CL-compliant structured pathnames with 6 components:
 *   host, device, directory, name, type, version
 *
 * Platform-aware: Amiga uses VOLUME:dir/file, POSIX uses /dir/file.
 */

#include "builtins.h"
#include "symbol.h"
#include "package.h"
#include "mem.h"
#include "error.h"
#include "vm.h"
#include "stream.h"
#include "string_utils.h"
#include "../platform/platform.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ================================================================
 * Namestring parsing
 * ================================================================ */

/* Is this character a directory separator in a namestring being PARSED?
 *
 * Windows accepts both, and both turn up in practice: the platform layer
 * normalises what it hands out to '/', but a string that reaches PATHNAME
 * from elsewhere does not go through it — $HOME is "C:\\Users\\you", and a
 * user typing a Windows path types backslashes.  Parsing those as ordinary
 * name characters silently produced one run-together component
 * ("C:\\Users\\x/.cache" -> (:RELATIVE "Usersx" ".cache")), which then failed
 * ASDF's absolute-pathname check.  Namestrings are still WRITTEN with '/'
 * only — this is about what we accept, not what we produce.
 *
 * Elsewhere (AmigaOS, POSIX) a backslash is an ordinary character in a file
 * name and must stay one. */
#ifdef PLATFORM_WIN32
#define CL_PATH_SEP_P(c) ((c) == '/' || (c) == '\\')
#else
#define CL_PATH_SEP_P(c) ((c) == '/')
#endif

#ifdef PLATFORM_WIN32
/* A one-letter device is a Windows drive ("C:").  Windows-only on purpose:
 * AmigaOS has one-letter ASSIGNs of its own ("S:startup-sequence",
 * "C:list"), and those are volume-style — absolute with no slash. */
static int device_is_drive_letter(CL_Obj device)
{
    CL_String *d;
    if (!CL_STRING_P(device)) return 0;
    d = (CL_String *)CL_OBJ_TO_PTR(device);
    if (d->length != 1) return 0;
    return (d->data[0] >= 'A' && d->data[0] <= 'Z') ||
           (d->data[0] >= 'a' && d->data[0] <= 'z');
}
#endif

/* AmigaDOS reads '/' differently from POSIX: a LEADING slash is the parent
 * of the current directory, not the root ("//x" = the grandparent's x), and
 * every EMPTY component between two slashes goes up one more level
 * ("a//b" = a's parent's b).  Only a device ("Work:") makes a path absolute.
 * Both syntaxes are compiled in so the host suite can test the Amiga one
 * (tests/test_pathname.c); the platform picks its own by default. */
#ifdef PLATFORM_AMIGA
#define CL_AMIGA_PATH_SYNTAX 1
#else
#define CL_AMIGA_PATH_SYNTAX 0
#endif

/* :UP / :BACK keywords, interned once by cl_builtins_pathname_init and
 * GC-rooted there — avoids a cl_find_symbol package lookup on every
 * directory component of every namestring built/parsed. */
static CL_Obj kw_up = CL_NIL;
static CL_Obj kw_back = CL_NIL;

/* Is this directory component :UP or :BACK?  Must not allocate — the
 * namestring builder runs on raw CL_Pathname pointers. */
static int dir_up_component_p(CL_Obj comp)
{
    if (!CL_SYMBOL_P(comp)) return 0;
    return comp == kw_up || comp == kw_back;
}

/*
 * Parse a namestring into pathname components.
 * Returns a pathname object.
 *
 * Amiga:  "VOLUME:dir1/dir2/name.type", "/up/name", "a//b/name"
 *         (each leading or empty component is :UP)
 * POSIX:  "/dir1/dir2/name.type" or "dir1/dir2/name.type"
 */
CL_Obj cl_parse_namestring(const char *str, uint32_t len)
{
    return cl_parse_namestring_syntax(str, len, CL_AMIGA_PATH_SYNTAX);
}

CL_Obj cl_parse_namestring_syntax(const char *str, uint32_t len, int amiga)
{
    CL_Obj host = CL_NIL;
    CL_Obj device = CL_NIL;
    CL_Obj directory = CL_NIL;
    CL_Obj pn_name = CL_NIL;
    CL_Obj pn_type = CL_NIL;
    CL_Obj version = CL_NIL;
    char path_copy[1024];
    char expand_buf[1024];
    const char *p;
    const char *end;

    /* The incoming `str` may point into the GC arena (callers pass a
     * CL_String's `data`).  This function allocates (cl_cons / cl_make_string)
     * while scanning `str`, and a compacting GC can relocate the source
     * string mid-parse — leaving str/p/end/filename dangling and producing
     * a truncated pathname (e.g. "/tmp/gcb.lisp" parsed as "/").  Copy into
     * a GC-stable stack buffer first so all scanning is over fixed memory. */
    if (len >= sizeof(path_copy))
        len = sizeof(path_copy) - 1;
    memcpy(path_copy, str, len);
    path_copy[len] = '\0';
    str = path_copy;

    /* Expand leading ~ to home directory */
    if (len > 0 && str[0] == '~' && len < sizeof(expand_buf)) {
        char tmp[1024];
        const char *expanded;
        memcpy(tmp, str, len);
        tmp[len] = '\0';
        expanded = platform_expand_home(tmp, expand_buf, (int)sizeof(expand_buf));
        if (expanded != tmp) {
            str = expanded;
            len = (uint32_t)strlen(expanded);
        }
    }

    p = str;
    end = str + len;
    const char *dir_start;
    const char *filename;
    const char *last_slash;
    const char *dot;

    CL_GC_PROTECT(host);
    CL_GC_PROTECT(device);
    CL_GC_PROTECT(directory);
    CL_GC_PROTECT(pn_name);
    CL_GC_PROTECT(pn_type);
    CL_GC_PROTECT(version);

    /* Empty string -> empty pathname */
    if (len == 0) {
        CL_Obj result = cl_make_pathname(CL_NIL, CL_NIL, CL_NIL,
                                          CL_NIL, CL_NIL, CL_NIL);
        CL_GC_UNPROTECT(6);
        return result;
    }

    dir_start = p;

    /* Check for Amiga device (VOLUME:...) */
    {
        const char *colon = NULL;
        const char *scan;
        for (scan = p; scan < end; scan++) {
            if (*scan == ':') { colon = scan; break; }
            if (CL_PATH_SEP_P(*scan)) break;  /* separator first = no device */
        }
        if (colon && colon > p) {
            device = cl_make_string(p, (uint32_t)(colon - p));
            dir_start = colon + 1;
        }
    }

    p = dir_start;

    /* Find last slash to separate directory from filename */
    last_slash = NULL;
    {
        const char *scan;
        for (scan = p; scan < end; scan++) {
            if (CL_PATH_SEP_P(*scan)) last_slash = scan;
        }
    }

    /* Parse directory components */
    if (last_slash != NULL || (p < end && CL_PATH_SEP_P(*p))) {
        /* There is directory info */
        const char *dir_end = last_slash ? last_slash : p;
        const char *dp = p;
        CL_Obj dir_list = CL_NIL;
        CL_Obj dir_tail = CL_NIL;
        CL_Obj dir_kind;

        CL_GC_PROTECT(dir_list);
        CL_GC_PROTECT(dir_tail);

        /* Determine absolute vs relative.  On AmigaDOS a leading slash is
         * NOT the root: it stays in place for the loop below to read as :UP. */
        if (!amiga && dp < end && CL_PATH_SEP_P(*dp)) {
            dir_kind = KW_ABSOLUTE;
            dp++;
        } else if (!CL_NULL_P(device)) {
#ifdef PLATFORM_WIN32
            /* A Windows drive letter with no slash after it is drive-RELATIVE:
             * "C:foo" names foo in the current directory of drive C:, a
             * different file from "C:/foo".  Recording that here is what lets
             * the namestring builder round-trip both spellings; treating it as
             * absolute (the Amiga rule below) silently rewrote one into the
             * other.  A longer device is an Amiga-style volume even here. */
            dir_kind = device_is_drive_letter(device) ? KW_RELATIVE : KW_ABSOLUTE;
#else
            /* Amiga: device present implies absolute (DH0:foo = absolute) */
            dir_kind = KW_ABSOLUTE;
#endif
        } else {
            dir_kind = KW_RELATIVE;
        }

        /* Start with (:ABSOLUTE) or (:RELATIVE) */
        dir_list = cl_cons(dir_kind, CL_NIL);
        dir_tail = dir_list;

        /* Parse directory components between slashes */
        while (dp <= dir_end && dp < end) {
            const char *comp_start = dp;
            while (dp < end && dp <= dir_end && !CL_PATH_SEP_P(*dp)) dp++;
            if (dp > comp_start) {
                CL_Obj comp;
                uint32_t clen = (uint32_t)(dp - comp_start);
                if (clen == 1 && *comp_start == '*')
                    comp = KW_WILD;
                else
                    comp = cl_make_string(comp_start, clen);
                {
                    CL_Obj cell = cl_cons(comp, CL_NIL);
                    ((CL_Cons *)CL_OBJ_TO_PTR(dir_tail))->cdr = cell;
                    dir_tail = cell;
                }
            } else if (amiga && dp < end && CL_PATH_SEP_P(*dp)) {
                /* Empty component = one level up (leading "/" or "a//b").
                 * dir_list/dir_tail are protected across the cons. */
                CL_Obj cell = kw_up;
                cell = cl_cons(cell, CL_NIL);
                ((CL_Cons *)CL_OBJ_TO_PTR(dir_tail))->cdr = cell;
                dir_tail = cell;
            }
            if (dp < end && CL_PATH_SEP_P(*dp)) dp++;
        }

        directory = dir_list;
        CL_GC_UNPROTECT(2);

        /* Filename starts after last slash */
        filename = last_slash ? last_slash + 1 : end;
    } else if (!CL_NULL_P(device)) {
        /* Device-only path (e.g. "PROGDIR:", "S:") — absolute with no subdirs */
        directory = cl_cons(KW_ABSOLUTE, CL_NIL);
        filename = p;
    } else {
        /* No directory — just a filename */
        filename = p;
    }

    /* Parse filename: name.type */
    if (filename < end) {
        uint32_t flen = (uint32_t)(end - filename);
        /* Find last dot for type separation */
        dot = NULL;
        {
            const char *scan;
            for (scan = filename; scan < end; scan++) {
                if (*scan == '.') dot = scan;
            }
        }
        if (dot && dot > filename) {
            uint32_t nlen = (uint32_t)(dot - filename);
            if (nlen == 1 && *filename == '*')
                pn_name = KW_WILD;
            else
                pn_name = cl_make_string(filename, nlen);
            if (dot + 1 < end) {
                uint32_t tlen = (uint32_t)(end - dot - 1);
                if (tlen == 1 && *(dot + 1) == '*')
                    pn_type = KW_WILD;
                else
                    pn_type = cl_make_string(dot + 1, tlen);
            }
        } else if (flen > 0) {
            /* No extension */
            if (flen == 1 && *filename == '*')
                pn_name = KW_WILD;
            else
                pn_name = cl_make_string(filename, flen);
        }
    }

    {
        CL_Obj result = cl_make_pathname(host, device, directory,
                                          pn_name, pn_type, version);
        CL_GC_UNPROTECT(6);
        return result;
    }
}

/* ================================================================
 * Namestring construction
 * ================================================================ */

/*
 * Build a namestring from pathname components into a buffer.
 * Returns the length written (not including NUL).
 */
uint32_t cl_pathname_to_namestring(CL_Pathname *pn, char *buf, uint32_t bufsz)
{
    return cl_pathname_to_namestring_syntax(pn, buf, bufsz,
                                            CL_AMIGA_PATH_SYNTAX);
}

uint32_t cl_pathname_to_namestring_syntax(CL_Pathname *pn, char *buf,
                                          uint32_t bufsz, int amiga)
{
    uint32_t pos = 0;

#define EMIT_CHAR(c) do { if (pos < bufsz - 1) buf[pos] = (c); pos++; } while(0)
#define EMIT_STR(s, slen) do { \
    uint32_t _i; \
    for (_i = 0; _i < (slen); _i++) EMIT_CHAR((s)[_i]); \
} while(0)

/* Check if a component is "empty" (NIL or :UNSPECIFIC) */
#define COMP_EMPTY(c) (CL_NULL_P(c) || (c) == KW_UNSPECIFIC)


    /* Device (Amiga: "DH0:") */
    if (!COMP_EMPTY(pn->device) && CL_STRING_P(pn->device)) {
        CL_String *dev = (CL_String *)CL_OBJ_TO_PTR(pn->device);
        EMIT_STR(dev->data, dev->length);
        EMIT_CHAR(':');
    }

    /* Directory */
    if (!COMP_EMPTY(pn->directory) && CL_CONS_P(pn->directory)) {
        CL_Obj dir = pn->directory;
        CL_Obj kind = cl_car(dir);
        CL_Obj rest = cl_cdr(dir);

        /* Volume syntax puts NO slash after the device: "DH0:foo" is
         * already absolute.  A Windows DRIVE LETTER is the opposite —
         * "C:foo" names a file relative to the current directory on drive
         * C:, a different file from "C:/foo" — so on Windows a one-character
         * device keeps its slash.  Longer device names stay volume-style
         * even there, so an Amiga namestring still round-trips unchanged
         * through a Windows build. */
        if (kind == KW_ABSOLUTE &&
#ifdef PLATFORM_WIN32
            (CL_NULL_P(pn->device) || device_is_drive_letter(pn->device))) {
#else
            CL_NULL_P(pn->device)) {
#endif
            EMIT_CHAR('/');
        }

        while (!CL_NULL_P(rest)) {
            CL_Obj comp = cl_car(rest);
            if (comp == KW_WILD) {
                EMIT_CHAR('*');
            } else if (dir_up_component_p(comp)) {
                /* AmigaDOS: an empty component is the parent ("a//b"). */
                if (!amiga) { EMIT_CHAR('.'); EMIT_CHAR('.'); }
            } else if (CL_STRING_P(comp)) {
                CL_String *cs = (CL_String *)CL_OBJ_TO_PTR(comp);
                EMIT_STR(cs->data, cs->length);
            }
            rest = cl_cdr(rest);
            /* Always emit trailing slash for directory components */
            EMIT_CHAR('/');
        }
    }

    /* Name */
    if (pn->name == KW_WILD) {
        EMIT_CHAR('*');
    } else if (!COMP_EMPTY(pn->name) && CL_STRING_P(pn->name)) {
        CL_String *nm = (CL_String *)CL_OBJ_TO_PTR(pn->name);
        EMIT_STR(nm->data, nm->length);
    }

    /* Type (with dot) */
    if (pn->type == KW_WILD) {
        EMIT_CHAR('.');
        EMIT_CHAR('*');
    } else if (!COMP_EMPTY(pn->type) && CL_STRING_P(pn->type)) {
        CL_String *tp = (CL_String *)CL_OBJ_TO_PTR(pn->type);
        EMIT_CHAR('.');
        EMIT_STR(tp->data, tp->length);
    }

    if (pos < bufsz) buf[pos] = '\0';
    else if (bufsz > 0) buf[bufsz - 1] = '\0';

#undef EMIT_CHAR
#undef EMIT_STR

    return pos;
}

/* Materialize any CL string designator — base string, wide string, or
 * adjustable/fill-pointer character vector — into a NUL-terminated C buffer
 * (UTF-8 for non-ASCII).  Returns 1 on success, 0 if ARG is not string-like.
 * CLHS pathname designators include all of these string flavours, not only
 * TYPE_STRING; without this, (pathname <fill-pointer-string>) and friends
 * wrongly signalled a type-error (hunchentoot's HTTP Range handler builds
 * such strings and passes them to PATHNAME). */

static int string_desig_to_cbuf(CL_Obj arg, char *buf, uint32_t bufsz)
{
    uint32_t len, i, pos = 0;
    if (CL_STRING_P(arg)) {
        CL_String *s = (CL_String *)CL_OBJ_TO_PTR(arg);
        uint32_t n = s->length < bufsz - 1 ? s->length : bufsz - 1;
        memcpy(buf, s->data, n);
        buf[n] = '\0';
        return 1;
    }
    if (!CL_ANY_STRING_P(arg) && !CL_STRING_VECTOR_P(arg))
        return 0;
    len = cl_string_length(arg);
    for (i = 0; i < len && pos < bufsz - 5; i++) {
        int ch = cl_string_char_at(arg, i);
        if (ch < 0x80) {
            buf[pos++] = (char)ch;
        } else {
#ifdef CL_WIDE_STRINGS
            char u8[4];
            int nb = cl_utf8_encode(ch, u8), j;
            for (j = 0; j < nb && pos < bufsz - 1; j++)
                buf[pos++] = u8[j];
#else
            /* Non-wide build: base/vector chars are single bytes (0–255). */
            buf[pos++] = (char)ch;
#endif
        }
    }
    buf[pos] = '\0';
    return 1;
}

/*
 * Get namestring as a C string from either a string or pathname argument.
 * Returns pointer to a static buffer.
 */
const char *cl_coerce_to_namestring(CL_Obj arg, char *buf, uint32_t bufsz)
{
    if (CL_STRING_P(arg)) {
        CL_String *s = (CL_String *)CL_OBJ_TO_PTR(arg);
        /* Expand leading ~ to home directory */
        return platform_expand_home(s->data, buf, (int)bufsz);
    }
    if (CL_PATHNAME_P(arg)) {
        CL_Pathname *pn = (CL_Pathname *)CL_OBJ_TO_PTR(arg);
        cl_pathname_to_namestring(pn, buf, bufsz);
        return buf;
    }
    /* File stream → its associated pathname (CLHS pathname designator). */
    if (CL_STREAM_P(arg)) {
        CL_Stream *st = (CL_Stream *)CL_OBJ_TO_PTR(arg);
        if (st->stream_type == CL_STREAM_FILE && CL_ANY_STRING_P(st->string_buf))
            return cl_coerce_to_namestring(st->string_buf, buf, bufsz);
    }
    /* Wide string / fill-pointer character vector — also valid strings. */
    if (string_desig_to_cbuf(arg, buf, bufsz)) {
        char tmp[1024];
        const char *expanded;
        if (buf[0] != '~') return buf;
        expanded = platform_expand_home(buf, tmp, (int)sizeof(tmp));
        if (expanded != buf) {
            uint32_t n = (uint32_t)strlen(expanded);
            if (n >= bufsz) n = bufsz - 1;
            memcpy(buf, expanded, n);
            buf[n] = '\0';
        }
        return buf;
    }
    cl_error(CL_ERR_TYPE, "Expected string or pathname");
    return "";
}

/* Coerce argument to pathname (string/wide/vector -> parse, pathname ->
 * identity, file stream -> its pathname). */
static CL_Obj coerce_to_pathname(CL_Obj arg)
{
    if (CL_PATHNAME_P(arg)) return arg;
    if (CL_STRING_P(arg)) {
        CL_String *s = (CL_String *)CL_OBJ_TO_PTR(arg);
        return cl_parse_namestring(s->data, s->length);
    }
    if (CL_STREAM_P(arg)) {
        CL_Stream *st = (CL_Stream *)CL_OBJ_TO_PTR(arg);
        if (st->stream_type == CL_STREAM_FILE && CL_ANY_STRING_P(st->string_buf))
            return coerce_to_pathname(st->string_buf);
    }
    {
        char buf[1024];
        if (string_desig_to_cbuf(arg, buf, sizeof(buf)))
            return cl_parse_namestring(buf, (uint32_t)strlen(buf));
    }
    cl_error(CL_ERR_TYPE, "PATHNAME: expected string, pathname, or file stream");
    return CL_NIL;
}

/* ================================================================
 * Builtins
 * ================================================================ */

/* (pathnamep thing) */
DEFINE_TYPE_PREDICATE(bi_pathnamep, CL_PATHNAME_P)

/* (pathname thing) — coerce string or pathname to pathname */
static CL_Obj bi_pathname(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    return coerce_to_pathname(args[0]);
}

/* (parse-namestring string) */
static CL_Obj bi_parse_namestring(CL_Obj *args, int n)
{
    CL_Obj arg = args[0];
    CL_UNUSED(n);
    if (CL_PATHNAME_P(arg)) return arg;
    if (CL_STRING_P(arg)) {
        CL_String *s = (CL_String *)CL_OBJ_TO_PTR(arg);
        return cl_parse_namestring(s->data, s->length);
    }
    cl_error(CL_ERR_TYPE, "PARSE-NAMESTRING: expected string or pathname");
    return CL_NIL;
}

/* (namestring pathname) — return namestring as string */
static CL_Obj bi_namestring(CL_Obj *args, int n)
{
    char buf[1024];
    uint32_t len;
    CL_Obj pn_obj;
    CL_Pathname *pn;
    CL_UNUSED(n);

    pn_obj = coerce_to_pathname(args[0]);
    pn = (CL_Pathname *)CL_OBJ_TO_PTR(pn_obj);
    len = cl_pathname_to_namestring(pn, buf, sizeof(buf));
    return cl_make_string(buf, len);
}

/* --- Component accessors --- */

/* (pathname-host p) */
static CL_Obj bi_pathname_host(CL_Obj *args, int n)
{
    CL_Obj pn_obj;
    CL_UNUSED(n);
    pn_obj = coerce_to_pathname(args[0]);
    return ((CL_Pathname *)CL_OBJ_TO_PTR(pn_obj))->host;
}

/* (pathname-device p) */
static CL_Obj bi_pathname_device(CL_Obj *args, int n)
{
    CL_Obj pn_obj;
    CL_UNUSED(n);
    pn_obj = coerce_to_pathname(args[0]);
    return ((CL_Pathname *)CL_OBJ_TO_PTR(pn_obj))->device;
}

/* (pathname-directory p) */
static CL_Obj bi_pathname_directory(CL_Obj *args, int n)
{
    CL_Obj pn_obj;
    CL_UNUSED(n);
    pn_obj = coerce_to_pathname(args[0]);
    return ((CL_Pathname *)CL_OBJ_TO_PTR(pn_obj))->directory;
}

/* (pathname-name p) */
static CL_Obj bi_pathname_name(CL_Obj *args, int n)
{
    CL_Obj pn_obj;
    CL_UNUSED(n);
    pn_obj = coerce_to_pathname(args[0]);
    return ((CL_Pathname *)CL_OBJ_TO_PTR(pn_obj))->name;
}

/* (pathname-type p) */
static CL_Obj bi_pathname_type(CL_Obj *args, int n)
{
    CL_Obj pn_obj;
    CL_UNUSED(n);
    pn_obj = coerce_to_pathname(args[0]);
    return ((CL_Pathname *)CL_OBJ_TO_PTR(pn_obj))->type;
}

/* (pathname-version p) */
static CL_Obj bi_pathname_version(CL_Obj *args, int n)
{
    CL_Obj pn_obj;
    CL_UNUSED(n);
    pn_obj = coerce_to_pathname(args[0]);
    return ((CL_Pathname *)CL_OBJ_TO_PTR(pn_obj))->version;
}

/* (make-pathname &key host device directory name type version defaults) */
static CL_Obj bi_make_pathname(CL_Obj *args, int n)
{
    CL_Obj host = CL_NIL, device = CL_NIL, directory = CL_NIL;
    CL_Obj pn_name = CL_NIL, pn_type = CL_NIL, version = CL_NIL;
    /* Track which keywords were explicitly provided so NIL overrides defaults */
    int has_host = 0, has_device = 0, has_directory = 0;
    int has_name = 0, has_type = 0, has_version = 0;
    int i;

    /* Two passes: coerce :defaults FIRST (it allocates when the designator
     * is a string — component values captured before it would go stale),
     * then collect the explicit components from the rooted args[], then
     * fill the unset ones from the protected defaults pathname. */
    CL_Obj def_pn = CL_NIL;
    CL_Obj result;

    for (i = 0; i + 1 < n; i += 2) {
        if (args[i] == KW_DEFAULTS) {
            def_pn = coerce_to_pathname(args[i + 1]);
            break;  /* leftmost :defaults wins (CLHS keyword semantics) */
        }
    }
    CL_GC_PROTECT(def_pn);

    for (i = 0; i + 1 < n; i += 2) {
        CL_Obj key = args[i];
        CL_Obj val = args[i + 1];
        if (key == KW_HOST) { host = val; has_host = 1; }
        else if (key == KW_DEVICE) { device = val; has_device = 1; }
        else if (key == KW_DIRECTORY) { directory = val; has_directory = 1; }
        else if (key == KW_NAME) { pn_name = val; has_name = 1; }
        else if (key == KW_TYPE) { pn_type = val; has_type = 1; }
        else if (key == KW_VERSION) { version = val; has_version = 1; }
    }

    if (!CL_NULL_P(def_pn)) {
        CL_Pathname *def = (CL_Pathname *)CL_OBJ_TO_PTR(def_pn);
        if (!has_host) host = def->host;
        if (!has_device) device = def->device;
        if (!has_directory) directory = def->directory;
        if (!has_name) pn_name = def->name;
        if (!has_type) pn_type = def->type;
        if (!has_version) version = def->version;
    }

    result = cl_make_pathname(host, device, directory, pn_name, pn_type, version);
    CL_GC_UNPROTECT(1);
    return result;
}

/* (merge-pathnames pathname &optional defaults default-version) */
CL_Obj bi_merge_pathnames(CL_Obj *args, int n)
{
    CL_Obj pn_obj;
    CL_Obj def_obj;
    CL_Pathname *pn, *def;
    CL_Obj host, device, directory, pn_name, pn_type, version;
    CL_Obj result;

    /* pn_obj must survive the coercion of the DEFAULTS designator (it
     * allocates for string designators / the make-pathname fallback), and
     * both stay rooted across the directory merge below. */
    pn_obj = coerce_to_pathname(args[0]);
    CL_GC_PROTECT(pn_obj);

    if (n >= 2) {
        def_obj = coerce_to_pathname(args[1]);
    } else {
        /* Use *default-pathname-defaults* */
        CL_Obj dpd_val = cl_symbol_value(SYM_STAR_DEFAULT_PATHNAME_DEFAULTS);
        if (!CL_NULL_P(dpd_val) && CL_PATHNAME_P(dpd_val))
            def_obj = dpd_val;
        else
            def_obj = cl_make_pathname(CL_NIL, CL_NIL, CL_NIL, CL_NIL, CL_NIL, CL_NIL);
    }
    CL_GC_PROTECT(def_obj);

    pn = (CL_Pathname *)CL_OBJ_TO_PTR(pn_obj);
    def = (CL_Pathname *)CL_OBJ_TO_PTR(def_obj);

    /* Merge directories FIRST — it is the only allocating step, and the
     * other components are extracted only afterward so they cannot go
     * stale (the old code captured them before the merge loop's conses
     * and passed stale offsets to cl_make_pathname). */
    if (CL_NULL_P(pn->directory)) {
        directory = def->directory;
    } else if (!CL_NULL_P(def->directory) && CL_CONS_P(pn->directory) &&
               cl_car(pn->directory) == KW_RELATIVE) {
        /* Build merged list: copy def->directory, then append cdr(pn->directory) */
        CL_Obj merged = CL_NIL, tail = CL_NIL;
        CL_Obj src = CL_NIL;
        CL_GC_PROTECT(merged);
        CL_GC_PROTECT(tail);
        /* src walks lists whose cells move on every cons — root the
         * cursor too. */
        CL_GC_PROTECT(src);

        /* Copy all of def->directory */
        for (src = def->directory; CL_CONS_P(src); src = cl_cdr(src)) {
            CL_Obj cell = cl_cons(cl_car(src), CL_NIL);
            if (CL_NULL_P(merged)) {
                merged = cell;
                tail = cell;
            } else {
                ((CL_Cons *)CL_OBJ_TO_PTR(tail))->cdr = cell;
                tail = cell;
            }
        }
        /* Append pn's relative components (skip :RELATIVE keyword) */
        pn = (CL_Pathname *)CL_OBJ_TO_PTR(pn_obj);
        for (src = cl_cdr(pn->directory); CL_CONS_P(src); src = cl_cdr(src)) {
            CL_Obj cell = cl_cons(cl_car(src), CL_NIL);
            if (CL_NULL_P(merged)) {
                merged = cell;
                tail = cell;
            } else {
                ((CL_Cons *)CL_OBJ_TO_PTR(tail))->cdr = cell;
                tail = cell;
            }
        }
        directory = merged;
        CL_GC_UNPROTECT(3);
        /* Re-fetch pointers after potential GC */
        pn = (CL_Pathname *)CL_OBJ_TO_PTR(pn_obj);
        def = (CL_Pathname *)CL_OBJ_TO_PTR(def_obj);
    } else {
        directory = pn->directory;
    }

    /* Fill in missing components from defaults — no allocation between
     * here and cl_make_pathname, so plain locals are safe. */
    host      = CL_NULL_P(pn->host)      ? def->host      : pn->host;
    device    = CL_NULL_P(pn->device)    ? def->device    : pn->device;
    pn_name   = CL_NULL_P(pn->name)     ? def->name      : pn->name;
    pn_type   = CL_NULL_P(pn->type)     ? def->type      : pn->type;
    version   = CL_NULL_P(pn->version)  ? def->version   : pn->version;

    if (n >= 3 && CL_NULL_P(version))
        version = args[2];

    result = cl_make_pathname(host, device, directory, pn_name, pn_type, version);
    CL_GC_UNPROTECT(2);
    return result;
}

/* (file-namestring pathname) — just the name.type part */
static CL_Obj bi_file_namestring(CL_Obj *args, int n)
{
    CL_Obj pn_obj;
    CL_Pathname *pn;
    char buf[512];
    uint32_t pos = 0;
    CL_UNUSED(n);

    pn_obj = coerce_to_pathname(args[0]);
    pn = (CL_Pathname *)CL_OBJ_TO_PTR(pn_obj);

    if (!COMP_EMPTY(pn->name) && CL_STRING_P(pn->name)) {
        CL_String *nm = (CL_String *)CL_OBJ_TO_PTR(pn->name);
        if (pos + nm->length < sizeof(buf)) {
            memcpy(buf + pos, nm->data, nm->length);
            pos += nm->length;
        }
    }
    if (!COMP_EMPTY(pn->type) && CL_STRING_P(pn->type)) {
        CL_String *tp = (CL_String *)CL_OBJ_TO_PTR(pn->type);
        if (pos + 1 + tp->length < sizeof(buf)) {
            buf[pos++] = '.';
            memcpy(buf + pos, tp->data, tp->length);
            pos += tp->length;
        }
    }
    buf[pos] = '\0';
    return cl_make_string(buf, pos);
}

/* (directory-namestring pathname) — just the directory part */
static CL_Obj bi_directory_namestring(CL_Obj *args, int n)
{
    CL_Obj pn_obj;
    CL_Pathname *pn;
    CL_Pathname temp;
    char buf[1024];
    uint32_t len;
    CL_UNUSED(n);

    pn_obj = coerce_to_pathname(args[0]);
    pn = (CL_Pathname *)CL_OBJ_TO_PTR(pn_obj);

    /* Build temp pathname with only directory (and device for Amiga) */
    memset(&temp, 0, sizeof(temp));
    temp.host = CL_NIL;
    temp.device = pn->device;
    temp.directory = pn->directory;
    temp.name = CL_NIL;
    temp.type = CL_NIL;
    temp.version = CL_NIL;

    len = cl_pathname_to_namestring(&temp, buf, sizeof(buf));
    return cl_make_string(buf, len);
}

/* (enough-namestring pathname &optional defaults) */
static CL_Obj bi_enough_namestring(CL_Obj *args, int n)
{
    /* Simplified: just return the namestring for now */
    char buf[1024];
    uint32_t len;
    CL_Obj pn_obj = coerce_to_pathname(args[0]);
    CL_Pathname *pn;
    CL_UNUSED(n);

    pn = (CL_Pathname *)CL_OBJ_TO_PTR(pn_obj);
    len = cl_pathname_to_namestring(pn, buf, sizeof(buf));
    return cl_make_string(buf, len);
}

/* ================================================================
 * Pathname equality (used by EQUAL)
 * ================================================================ */

int cl_pathname_equal(CL_Obj a, CL_Obj b)
{
    CL_Pathname *pa, *pb;
    if (!CL_PATHNAME_P(a) || !CL_PATHNAME_P(b)) return 0;
    pa = (CL_Pathname *)CL_OBJ_TO_PTR(a);
    pb = (CL_Pathname *)CL_OBJ_TO_PTR(b);

    /* Compare each component */
    /* host */
    if (pa->host != pb->host) {
        if (!CL_STRING_P(pa->host) || !CL_STRING_P(pb->host)) return 0;
        { CL_String *sa = (CL_String *)CL_OBJ_TO_PTR(pa->host);
          CL_String *sb = (CL_String *)CL_OBJ_TO_PTR(pb->host);
          if (sa->length != sb->length || memcmp(sa->data, sb->data, sa->length) != 0) return 0; }
    }
    /* device */
    if (pa->device != pb->device) {
        if (!CL_STRING_P(pa->device) || !CL_STRING_P(pb->device)) return 0;
        { CL_String *sa = (CL_String *)CL_OBJ_TO_PTR(pa->device);
          CL_String *sb = (CL_String *)CL_OBJ_TO_PTR(pb->device);
          if (sa->length != sb->length || memcmp(sa->data, sb->data, sa->length) != 0) return 0; }
    }
    /* name */
    if (pa->name != pb->name) {
        if (!CL_STRING_P(pa->name) || !CL_STRING_P(pb->name)) return 0;
        { CL_String *sa = (CL_String *)CL_OBJ_TO_PTR(pa->name);
          CL_String *sb = (CL_String *)CL_OBJ_TO_PTR(pb->name);
          if (sa->length != sb->length || memcmp(sa->data, sb->data, sa->length) != 0) return 0; }
    }
    /* type */
    if (pa->type != pb->type) {
        if (!CL_STRING_P(pa->type) || !CL_STRING_P(pb->type)) return 0;
        { CL_String *sa = (CL_String *)CL_OBJ_TO_PTR(pa->type);
          CL_String *sb = (CL_String *)CL_OBJ_TO_PTR(pb->type);
          if (sa->length != sb->length || memcmp(sa->data, sb->data, sa->length) != 0) return 0; }
    }
    /* version */
    if (pa->version != pb->version) return 0;
    /* directory: compare structurally via list equality */
    if (pa->directory != pb->directory) {
        CL_Obj da = pa->directory, db = pb->directory;
        while (CL_CONS_P(da) && CL_CONS_P(db)) {
            CL_Obj ca = cl_car(da), cb = cl_car(db);
            if (ca != cb) {
                if (!CL_STRING_P(ca) || !CL_STRING_P(cb)) return 0;
                { CL_String *sa = (CL_String *)CL_OBJ_TO_PTR(ca);
                  CL_String *sb = (CL_String *)CL_OBJ_TO_PTR(cb);
                  if (sa->length != sb->length || memcmp(sa->data, sb->data, sa->length) != 0) return 0; }
            }
            da = cl_cdr(da);
            db = cl_cdr(db);
        }
        if (da != db) return 0;  /* different lengths */
    }
    return 1;
}

/* (user-homedir-pathname &optional host) */
static CL_Obj bi_user_homedir_pathname(CL_Obj *args, int n)
{
    const char *home;
    CL_UNUSED(args);
    CL_UNUSED(n);
#ifdef PLATFORM_AMIGA
    home = "PROGDIR:";
#else
    home = getenv("HOME");
    if (!home) home = "/";
#endif
    {
        uint32_t hlen = (uint32_t)strlen(home);
        /* Ensure path ends with separator */
        if (hlen > 0 && home[hlen - 1] != '/'
#ifdef PLATFORM_AMIGA
            && home[hlen - 1] != ':'
#endif
           ) {
            char buf[512];
            if (hlen + 1 < sizeof(buf)) {
                memcpy(buf, home, hlen);
                buf[hlen] = '/';
                buf[hlen + 1] = '\0';
                return cl_parse_namestring(buf, hlen + 1);
            }
        }
        return cl_parse_namestring(home, hlen);
    }
}

/* wild-pathname-p — check for :WILD components */
static CL_Obj bi_wild_pathname_p(CL_Obj *args, int n)
{
    CL_Obj pn_obj = coerce_to_pathname(args[0]);
    CL_Pathname *pn = (CL_Pathname *)CL_OBJ_TO_PTR(pn_obj);
    CL_Obj field = (n >= 2 && !CL_NULL_P(args[1])) ? args[1] : CL_NIL;

    if (!CL_NULL_P(field)) {
        /* Check specific field */
        CL_Obj val = CL_NIL;
        if (field == KW_NAME)           val = pn->name;
        else if (field == KW_TYPE)      val = pn->type;
        else if (field == KW_DIRECTORY) {
            /* Check all directory components */
            if (CL_CONS_P(pn->directory)) {
                CL_Obj d = cl_cdr(pn->directory);
                while (CL_CONS_P(d)) {
                    if (cl_car(d) == KW_WILD) return CL_T;
                    d = cl_cdr(d);
                }
            }
            return CL_NIL;
        }
        else if (field == KW_HOST)      val = pn->host;
        else if (field == KW_DEVICE)    val = pn->device;
        else if (field == KW_VERSION)   val = pn->version;
        return (val == KW_WILD) ? CL_T : CL_NIL;
    }

    /* Check all components */
    if (pn->name == KW_WILD || pn->type == KW_WILD ||
        pn->host == KW_WILD || pn->device == KW_WILD ||
        pn->version == KW_WILD)
        return CL_T;
    if (CL_CONS_P(pn->directory)) {
        CL_Obj d = cl_cdr(pn->directory);
        while (CL_CONS_P(d)) {
            if (cl_car(d) == KW_WILD) return CL_T;
            d = cl_cdr(d);
        }
    }
    return CL_NIL;
}

/* pathname-match-p — simplified: compare namestrings for equality
 * (no wildcard support — just string equality) */
static CL_Obj bi_pathname_match_p(CL_Obj *args, int n)
{
    CL_Obj ns1, ns2;
    CL_String *s1, *s2;
    int eq;
    CL_UNUSED(n);
    /* ns1 must survive the second namestring call — it always allocates. */
    ns1 = bi_namestring(&args[0], 1);
    CL_GC_PROTECT(ns1);
    ns2 = bi_namestring(&args[1], 1);
    CL_GC_UNPROTECT(1);
    s1 = (CL_String *)CL_OBJ_TO_PTR(ns1);
    s2 = (CL_String *)CL_OBJ_TO_PTR(ns2);
    if (s1->length != s2->length) return CL_NIL;
    eq = memcmp(s1->data, s2->data, s1->length) == 0;
    return eq ? CL_T : CL_NIL;
}

/* translate-pathname — simplified: just return the source pathname
 * (no wildcard translation support) */
static CL_Obj bi_translate_pathname(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    return coerce_to_pathname(args[0]);
}

/* ================================================================
 * Registration
 * ================================================================ */

/* (ext::%amiga-namestring-roundtrip string) => (directory namestring)
 * Parses STRING with the AmigaDOS slash rules on any platform and rebuilds
 * its namestring with them — lets the host suites (the gc-stress one
 * included) exercise the Amiga-only :UP parsing path. */
static CL_Obj bi_amiga_namestring_roundtrip(CL_Obj *args, int n)
{
    char buf[1024];
    uint32_t len;
    CL_Obj pn_obj, ns, result;
    CL_String *s;
    CL_UNUSED(n);

    if (!CL_STRING_P(args[0]))
        cl_error(CL_ERR_TYPE, "%%AMIGA-NAMESTRING-ROUNDTRIP: expected a simple string");
    s = (CL_String *)CL_OBJ_TO_PTR(args[0]);
    pn_obj = cl_parse_namestring_syntax(s->data, s->length, 1);
    CL_GC_PROTECT(pn_obj);
    len = cl_pathname_to_namestring_syntax((CL_Pathname *)CL_OBJ_TO_PTR(pn_obj),
                                           buf, sizeof(buf), 1);
    ns = cl_make_string(buf, len);
    result = cl_cons(ns, CL_NIL);
    result = cl_cons(((CL_Pathname *)CL_OBJ_TO_PTR(pn_obj))->directory, result);
    CL_GC_UNPROTECT(1);
    return result;
}

void cl_builtins_pathname_init(void)
{
    kw_up = cl_intern_keyword("UP", 2);
    kw_back = cl_intern_keyword("BACK", 4);
    cl_gc_register_root(&kw_up);
    cl_gc_register_root(&kw_back);

    cl_register_builtin("%AMIGA-NAMESTRING-ROUNDTRIP",
                        bi_amiga_namestring_roundtrip, 1, 1, cl_package_ext);
    cl_gc_register_root(&SYM_STAR_DEFAULT_PATHNAME_DEFAULTS);
    defun("PATHNAMEP", bi_pathnamep, 1, 1);
    defun("PATHNAME", bi_pathname, 1, 1);
    defun("PARSE-NAMESTRING", bi_parse_namestring, 1, 1);
    defun("NAMESTRING", bi_namestring, 1, 1);
    defun("PATHNAME-HOST", bi_pathname_host, 1, 1);
    defun("PATHNAME-DEVICE", bi_pathname_device, 1, 1);
    defun("PATHNAME-DIRECTORY", bi_pathname_directory, 1, 1);
    defun("PATHNAME-NAME", bi_pathname_name, 1, 1);
    defun("PATHNAME-TYPE", bi_pathname_type, 1, 1);
    defun("PATHNAME-VERSION", bi_pathname_version, 1, 1);
    defun("MAKE-PATHNAME", bi_make_pathname, 0, -1);
    defun("MERGE-PATHNAMES", bi_merge_pathnames, 1, 3);
    defun("FILE-NAMESTRING", bi_file_namestring, 1, 1);
    defun("DIRECTORY-NAMESTRING", bi_directory_namestring, 1, 1);
    defun("ENOUGH-NAMESTRING", bi_enough_namestring, 1, 2);
    defun("USER-HOMEDIR-PATHNAME", bi_user_homedir_pathname, 0, 1);
    defun("WILD-PATHNAME-P", bi_wild_pathname_p, 1, 2);
    defun("PATHNAME-MATCH-P", bi_pathname_match_p, 2, 2);
    defun("TRANSLATE-PATHNAME", bi_translate_pathname, 3, 3);

    cl_pathname_default_from_cwd();
}

/* Set *default-pathname-defaults* to the process's current working
 * directory in absolute directory form.  Conforming implementations seed it
 * with the cwd so relative merges (merge-pathnames, ASDF :tree directives,
 * etc.) resolve to absolute pathnames.  An empty pathname here would leave
 * (merge-pathnames "x/" *default-pathname-defaults*) relative, which ASDF's
 * source-registry rejects ("Expected an absolute pathname").  Fall back to
 * the empty pathname only if the cwd cannot be determined.
 *
 * Called at init and again after a heap-image restore (image.c): the value
 * is heap state and comes back with the image, i.e. it would name the cwd
 * of the process that SAVED the image — for a shipped amilisp.img, a
 * directory on the build machine — and every relative LOAD/REQUIRE would
 * resolve there while the existence checks look at the real cwd. */
void cl_pathname_default_from_cwd(void)
{
    char cwd[512];
    CL_Obj dpd = CL_NIL;
    if (platform_getcwd(cwd, (int)sizeof(cwd))) {
        uint32_t clen = (uint32_t)strlen(cwd);
        /* Ensure directory form (trailing separator) so the namestring has
         * no name/type component. */
        if (clen > 0 && clen + 1 < sizeof(cwd)
            && cwd[clen - 1] != '/'
#ifdef PLATFORM_AMIGA
            && cwd[clen - 1] != ':'
#endif
           ) {
            cwd[clen] = '/';
            cwd[clen + 1] = '\0';
            clen++;
        }
        dpd = cl_parse_namestring(cwd, clen);
    }
    if (CL_NULL_P(dpd))
        dpd = cl_make_pathname(CL_NIL, CL_NIL, CL_NIL, CL_NIL, CL_NIL, CL_NIL);
    /* Dereference after all allocating calls so we hold a fresh pointer
     * even if GC compaction relocated the symbol object. */
    ((CL_Symbol *)CL_OBJ_TO_PTR(SYM_STAR_DEFAULT_PATHNAME_DEFAULTS))->value = dpd;
}
