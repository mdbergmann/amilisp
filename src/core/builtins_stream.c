/*
 * builtins_stream.c — Stream-related CL builtins
 *
 * Step 1: streamp, input-stream-p, output-stream-p, interactive-stream-p
 * Step 3: read-char, write-char, peek-char, unread-char, read-line,
 *         write-string, write-line, terpri, fresh-line, finish-output,
 *         force-output, clear-output, open-stream-p, close,
 *         make-string-input-stream, make-string-output-stream,
 *         get-output-stream-string
 */

#include "builtins.h"
#include "stream.h"
#include "symbol.h"
#include "package.h"
#include "mem.h"
#include "error.h"
#include "vm.h"
#include "bignum.h"
#include "readtable.h"
#include "reader.h"
#include "string_utils.h"
#include "compiler.h"  /* cl_tables_wrlock / cl_tables_rwunlock */
#include "../platform/platform.h"
#include "../platform/platform_thread.h"
#include "thread.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* Declared (non-static) in stream.c.  Resolves a synonym-stream /
 * two-way-stream designator to the concrete backing stream for the given
 * direction (0 = input side, else output side) — e.g. *TERMINAL-IO* is a
 * two-way-stream over *STANDARD-INPUT* and *STANDARD-OUTPUT* (cl_stream_init),
 * so console-specific field checks below must unwrap it to reach the actual
 * console CL_Stream carrying unread_char/flags/stream_type state. */
CL_Obj cl_stream_resolve_backing(CL_Obj stream, int writing);

/* --- Pre-interned keyword symbols --- */

static CL_Obj KW_START = CL_NIL;
static CL_Obj KW_END = CL_NIL;

/* Lazy-cached Gray stream class symbols for two-way stream acceptance */
static CL_Obj GRAY_FUND_INPUT_SYM  = CL_NIL;
static CL_Obj GRAY_FUND_OUTPUT_SYM = CL_NIL;
static CL_Obj KW_ABORT_KW = CL_NIL;
static CL_Obj KW_DIRECTION = CL_NIL;
static CL_Obj KW_INPUT = CL_NIL;
static CL_Obj KW_OUTPUT = CL_NIL;
static CL_Obj KW_IO = CL_NIL;
static CL_Obj KW_PROBE = CL_NIL;
static CL_Obj KW_IF_EXISTS = CL_NIL;
static CL_Obj KW_IF_DOES_NOT_EXIST = CL_NIL;
static CL_Obj KW_SUPERSEDE = CL_NIL;
static CL_Obj KW_APPEND = CL_NIL;
static CL_Obj KW_ERROR_KW = CL_NIL;
static CL_Obj KW_CREATE = CL_NIL;
static CL_Obj KW_NEW_VERSION = CL_NIL;
static CL_Obj KW_RENAME = CL_NIL;
static CL_Obj KW_RENAME_AND_DELETE = CL_NIL;
static CL_Obj KW_OVERWRITE = CL_NIL;
static CL_Obj KW_ELEMENT_TYPE_OPEN = CL_NIL;  /* OPEN :element-type */
static CL_Obj KW_EXTERNAL_FORMAT = CL_NIL;    /* OPEN :external-format */
static CL_Obj KW_LATIN_1 = CL_NIL;            /* :latin-1 (stream-external-format report) */

/* --- Stream argument resolution helpers --- */

CL_Obj cl_resolve_input_stream(CL_Obj *args, int n, int idx)
{
    CL_Obj s;
    if (idx >= n || CL_NULL_P(args[idx]))
        return cl_symbol_value(SYM_STANDARD_INPUT);
    s = args[idx];
    if (s == CL_T)
        return cl_symbol_value(SYM_TERMINAL_IO);
    if (!CL_STREAM_P(s))
        cl_error(CL_ERR_TYPE, "argument is not a stream");
    return s;
}

CL_Obj cl_resolve_output_stream(CL_Obj *args, int n, int idx)
{
    CL_Obj s;
    if (idx >= n || CL_NULL_P(args[idx]))
        return cl_symbol_value(SYM_STANDARD_OUTPUT);
    s = args[idx];
    if (s == CL_T)
        return cl_symbol_value(SYM_TERMINAL_IO);
    if (!CL_STREAM_P(s))
        cl_error(CL_ERR_TYPE, "argument is not a stream");
    return s;
}

/* --- Step 1 builtins --- */

/* (streamp obj) => T or NIL */
static CL_Obj bi_streamp(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    return CL_STREAM_P(args[0]) ? CL_T : CL_NIL;
}

/* (input-stream-p stream) => T or NIL */
static CL_Obj bi_input_stream_p(CL_Obj *args, int n)
{
    CL_Stream *st;
    CL_UNUSED(n);
    if (!CL_STREAM_P(args[0]))
        cl_error(CL_ERR_TYPE, "INPUT-STREAM-P: argument is not a stream");
    st = (CL_Stream *)CL_OBJ_TO_PTR(args[0]);
    return (st->direction == CL_STREAM_INPUT || st->direction == CL_STREAM_IO)
           ? CL_T : CL_NIL;
}

/* (output-stream-p stream) => T or NIL */
static CL_Obj bi_output_stream_p(CL_Obj *args, int n)
{
    CL_Stream *st;
    CL_UNUSED(n);
    if (!CL_STREAM_P(args[0]))
        cl_error(CL_ERR_TYPE, "OUTPUT-STREAM-P: argument is not a stream");
    st = (CL_Stream *)CL_OBJ_TO_PTR(args[0]);
    return (st->direction == CL_STREAM_OUTPUT || st->direction == CL_STREAM_IO)
           ? CL_T : CL_NIL;
}

/* (%make-test-stream direction type) => stream */
static CL_Obj bi_make_test_stream(CL_Obj *args, int n)
{
    uint32_t dir, stype;
    CL_UNUSED(n);
    if (!CL_FIXNUM_P(args[0]) || !CL_FIXNUM_P(args[1]))
        cl_error(CL_ERR_TYPE, "%MAKE-TEST-STREAM: arguments must be fixnums");
    dir = (uint32_t)CL_FIXNUM_VAL(args[0]);
    stype = (uint32_t)CL_FIXNUM_VAL(args[1]);
    return cl_make_stream(dir, stype);
}

/* Input-side backing of `stream` for state probes (LISTEN,
 * READ-CHAR-NO-HANG, INTERACTIVE-STREAM-P): cl_stream_resolve_backing(_, 0)
 * plus a descent through echo wrappers.  The read path deliberately stops
 * at an echo stream (the wrapper is what echoes), but the data — position,
 * EOF flag, console/socket readiness — lives in its input component.  The
 * descent stops at a wrapper holding a pushed-back char: that char is the
 * next thing read, so the wrapper itself answers "available". */
static CL_Obj probe_input_backing(CL_Obj stream)
{
    stream = cl_stream_resolve_backing(stream, 0);
    while (CL_STREAM_P(stream)) {
        CL_Stream *st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
        if (st->stream_type != CL_STREAM_ECHO || st->unread_char != -1)
            break;
        stream = cl_stream_resolve_backing(st->string_buf, 0);
    }
    return stream;
}

/* (interactive-stream-p stream) => T or NIL */
static CL_Obj bi_interactive_stream_p(CL_Obj *args, int n)
{
    CL_Obj rs;
    CL_UNUSED(n);
    if (!CL_STREAM_P(args[0]))
        cl_error(CL_ERR_TYPE, "INTERACTIVE-STREAM-P: argument is not a stream");
    /* args[0] may be a synonym-/two-way-/echo-stream (e.g. *TERMINAL-IO*)
     * wrapping a console stream; resolve both directions since either child
     * could be the console. */
    rs = probe_input_backing(args[0]);
    if (CL_STREAM_P(rs) && ((CL_Stream *)CL_OBJ_TO_PTR(rs))->stream_type == CL_STREAM_CONSOLE)
        return CL_T;
    rs = cl_stream_resolve_backing(args[0], 1);
    if (CL_STREAM_P(rs) && ((CL_Stream *)CL_OBJ_TO_PTR(rs))->stream_type == CL_STREAM_CONSOLE)
        return CL_T;
    return CL_NIL;
}

/* --- Step 3 builtins: Stream I/O --- */

/* (read-char &optional stream eof-error-p eof-value recursive-p) */
static CL_Obj bi_read_char(CL_Obj *args, int n)
{
    CL_Obj stream = cl_resolve_input_stream(args, n, 0);
    int eof_error_p = (n < 2 || !CL_NULL_P(args[1]));
    int ch;

    ch = cl_stream_read_char(stream);
    if (ch == -1) {
        if (eof_error_p)
            cl_error(CL_ERR_EOF, "READ-CHAR: end of file");
        /* Re-read the eof-value from the rooted args[] — the blocking read
         * can park in a safe region while a peer compacts, staling a copy
         * taken before it. */
        return (n >= 3) ? args[2] : CL_NIL;
    }
    return CL_MAKE_CHAR(ch);
}

/* (write-char character &optional stream) => character */
static CL_Obj bi_write_char(CL_Obj *args, int n)
{
    CL_Obj stream;
    if (!CL_CHAR_P(args[0]))
        cl_error(CL_ERR_TYPE, "WRITE-CHAR: argument is not a character");
    stream = cl_resolve_output_stream(args, n, 1);
    cl_stream_write_char(stream, CL_CHAR_VAL(args[0]));
    return args[0];
}

/* (read-byte stream &optional eof-error-p eof-value) => byte */
static CL_Obj bi_read_byte(CL_Obj *args, int n)
{
    CL_Obj stream = args[0];
    int eof_error_p = (n < 2 || !CL_NULL_P(args[1]));
    int byte;

    if (!CL_STREAM_P(stream))
        cl_error(CL_ERR_TYPE, "READ-BYTE: argument is not a stream");
    {
        /* Reading from an output-only stream (e.g. a broadcast stream) is an
         * error, not EOF (CLHS: input functions require an input stream). */
        CL_Stream *st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
        if (st->direction != CL_STREAM_INPUT && st->direction != CL_STREAM_IO)
            cl_error(CL_ERR_TYPE, "READ-BYTE: stream is not an input stream");
    }
    byte = cl_stream_read_byte(stream);
    if (byte == -1) {
        if (eof_error_p)
            cl_error(CL_ERR_EOF, "READ-BYTE: end of file");
        /* Re-read from rooted args[] — the blocking read can park in a
         * safe region while a peer compacts (see bi_read_char). */
        return (n >= 3) ? args[2] : CL_NIL;
    }
    return CL_MAKE_FIXNUM(byte);
}

/* (write-byte byte stream) => byte */
static CL_Obj bi_write_byte(CL_Obj *args, int n)
{
    CL_Obj stream;
    int32_t byte;
    (void)n;

    if (!CL_FIXNUM_P(args[0]))
        cl_error(CL_ERR_TYPE, "WRITE-BYTE: argument is not an integer");
    byte = CL_FIXNUM_VAL(args[0]);
    if (byte < 0 || byte > 255)
        cl_error(CL_ERR_GENERAL, "WRITE-BYTE: byte value %d out of range 0-255", byte);
    stream = args[1];
    if (!CL_STREAM_P(stream))
        cl_error(CL_ERR_TYPE, "WRITE-BYTE: second argument is not a stream");
    cl_stream_write_byte(stream, byte);
    return args[0];
}

/* (peek-char &optional peek-type stream eof-error-p eof-value recursive-p) */
static CL_Obj bi_peek_char(CL_Obj *args, int n)
{
    CL_Obj peek_type = (n >= 1) ? args[0] : CL_NIL;
    CL_Obj stream = cl_resolve_input_stream(args, n, 1);
    int eof_error_p = (n < 3 || !CL_NULL_P(args[2]));
    CL_Obj eof_value = (n >= 4) ? args[3] : CL_NIL;
    int ch;

    /* MT: a blocking read parks in a GC safe region — a peer compaction
     * moves the stream and eof_value; both are re-used across/after the
     * loops below, so root them. */
    CL_GC_PROTECT(stream);
    CL_GC_PROTECT(eof_value);

    if (CL_NULL_P(peek_type)) {
        /* NIL: just peek at next char */
        ch = cl_stream_peek_char(stream);
    } else if (peek_type == CL_T) {
        /* T: skip whitespace, peek at first non-whitespace */
        for (;;) {
            ch = cl_stream_read_char(stream);
            if (ch == -1) break;
            if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') {
                cl_stream_unread_char(stream, ch);
                break;
            }
        }
    } else if (CL_CHAR_P(peek_type)) {
        /* Character: skip until that char, peek at it */
        int target = CL_CHAR_VAL(peek_type);
        for (;;) {
            ch = cl_stream_read_char(stream);
            if (ch == -1) break;
            if (ch == target) {
                cl_stream_unread_char(stream, ch);
                break;
            }
        }
    } else {
        cl_error(CL_ERR_TYPE, "PEEK-CHAR: invalid peek-type");
        return CL_NIL;
    }

    CL_GC_UNPROTECT(2);
    if (ch == -1) {
        if (eof_error_p)
            cl_error(CL_ERR_EOF, "PEEK-CHAR: end of file");
        return eof_value;
    }
    return CL_MAKE_CHAR(ch);
}

/* (unread-char character &optional stream) => NIL */
static CL_Obj bi_unread_char(CL_Obj *args, int n)
{
    CL_Obj stream;
    CL_Stream *st;
    if (!CL_CHAR_P(args[0]))
        cl_error(CL_ERR_TYPE, "UNREAD-CHAR: argument is not a character");
    stream = cl_resolve_input_stream(args, n, 1);
    st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
    if (st->unread_char != -1)
        cl_error(CL_ERR_GENERAL, "UNREAD-CHAR: already unreading a character");
    cl_stream_unread_char(stream, CL_CHAR_VAL(args[0]));
    return CL_NIL;
}

/* (read-line &optional stream eof-error-p eof-value recursive-p)
 * => string, missing-newline-p (2 values) */
static CL_Obj bi_read_line(CL_Obj *args, int n)
{
    CL_Obj stream = cl_resolve_input_stream(args, n, 0);
    int eof_error_p = (n < 2 || !CL_NULL_P(args[1]));
    CL_Obj eof_value = (n >= 3) ? args[2] : CL_NIL;
    char stack_buf[256];
    char *buf = stack_buf;
    uint32_t cap = 256;
    uint32_t len = 0;
    int ch;
    int got_any = 0;
    int missing_newline = 1;
    CL_Obj result;

    /* MT: each blocking read can park in a safe region while a peer
     * compacts — stream and eof_value are re-used afterward. */
    CL_GC_PROTECT(stream);
    CL_GC_PROTECT(eof_value);

    for (;;) {
        ch = cl_stream_read_char(stream);
        if (ch == -1) break;
        got_any = 1;
        if (ch == '\n') {
            missing_newline = 0;
            break;
        }
#ifdef CL_WIDE_STRINGS
        /* Encode code points as UTF-8 into the byte buffer */
        {
            char utf8[4];
            int nb = cl_utf8_encode(ch, utf8);
            int j;
            if (nb == 0) { nb = 1; utf8[0] = '?'; }
            if (len + (uint32_t)nb >= cap) {
                uint32_t new_cap = cap * 2;
                char *new_buf;
                while (new_cap < len + (uint32_t)nb + 1) new_cap *= 2;
                new_buf = (char *)platform_alloc(new_cap);
                if (!new_buf) break;
                memcpy(new_buf, buf, len);
                if (buf != stack_buf) platform_free(buf);
                buf = new_buf;
                cap = new_cap;
            }
            for (j = 0; j < nb; j++)
                buf[len++] = utf8[j];
        }
#else
        if (len + 1 >= cap) {
            uint32_t new_cap = cap * 2;
            char *new_buf = (char *)platform_alloc(new_cap);
            if (!new_buf) break;
            memcpy(new_buf, buf, len);
            if (buf != stack_buf) platform_free(buf);
            buf = new_buf;
            cap = new_cap;
        }
        buf[len++] = (char)ch;
#endif
    }

    CL_GC_UNPROTECT(2);

    if (!got_any && ch == -1) {
        if (buf != stack_buf) platform_free(buf);
        if (eof_error_p)
            cl_error(CL_ERR_EOF, "READ-LINE: end of file");
        cl_mv_values[0] = eof_value;
        cl_mv_values[1] = CL_T;
        cl_mv_count = 2;
        return eof_value;
    }

#ifdef CL_WIDE_STRINGS
    /* Buffer contains UTF-8 bytes; decode to proper CL string */
    result = cl_utf8_to_cl_string(buf, len);
#else
    result = cl_make_string(buf, len);
#endif
    if (buf != stack_buf) platform_free(buf);

    cl_mv_values[0] = result;
    cl_mv_values[1] = missing_newline ? CL_T : CL_NIL;
    cl_mv_count = 2;
    return result;
}

/* (write-string string &optional stream &key :start :end) => string */
static CL_Obj bi_write_string(CL_Obj *args, int n)
{
    CL_Obj stream;
    uint32_t start = 0, end, slen;
    int i;

    if (!CL_ANY_STRING_P(args[0]) && !CL_STRING_VECTOR_P(args[0]))
        cl_error(CL_ERR_TYPE, "WRITE-STRING: argument is not a string");
    slen = cl_string_length(args[0]);
    end = slen;

    stream = cl_resolve_output_stream(args, n, 1);

    /* Parse keyword args starting from index 2 */
    for (i = 2; i + 1 < n; i += 2) {
        CL_Obj kw = args[i];
        CL_Obj val = args[i + 1];
        if (kw == KW_START && CL_FIXNUM_P(val))
            start = (uint32_t)CL_FIXNUM_VAL(val);
        else if (kw == KW_END && !CL_NULL_P(val) && CL_FIXNUM_P(val))
            end = (uint32_t)CL_FIXNUM_VAL(val);
    }

    if (start > slen) start = slen;
    if (end > slen) end = slen;
    if (start < end) {
        /* Fast path for base strings: cl_stream_write_lisp_string chunks
         * through a C buffer, re-deriving the source per chunk — passing
         * the raw arena pointer let a blocking file/socket write resume
         * copying from a dangling pointer after a peer MT compaction
         * (garbage bytes on the wire).  It also sidesteps the old m68k
         * `base_data + start` miscompilation workaround: the offset math
         * happens on a freshly derived pointer inside the helper. */
        if (cl_string_is_base(args[0])) {
            cl_stream_write_lisp_string(stream, args[0], start, end);
        } else {
            uint32_t j;
            /* MT: each write can park in a safe region — keep the stream
             * handle forwarded across the loop (args[0] is rooted). */
            CL_GC_PROTECT(stream);
            for (j = start; j < end; j++)
                cl_stream_write_char(stream, cl_string_char_at(args[0], j));
            CL_GC_UNPROTECT(1);
        }
    }

    return args[0];
}

/* (write-line string &optional stream &key :start :end) => string */
static CL_Obj bi_write_line(CL_Obj *args, int n)
{
    bi_write_string(args, n);
    {
        CL_Obj stream = cl_resolve_output_stream(args, n, 1);
        cl_stream_write_char(stream, '\n');
    }
    return args[0];
}

/* (terpri &optional stream) => NIL */
static CL_Obj bi_terpri(CL_Obj *args, int n)
{
    CL_Obj stream = cl_resolve_output_stream(args, n, 0);
    cl_stream_write_char(stream, '\n');
    return CL_NIL;
}

/* (fresh-line &optional stream) => T if newline written, NIL if at BOL */
/* FRESH-LINE core: consult the column of the stream that actually carries
 * it.  Synonym/two-way wrappers never update their own charpos, so unwrap to
 * the output backing; a broadcast stream performs the operation on every
 * component and returns the last component's result (CLHS 21.1.1.1.1).
 * Exported (non-static) so the REPL's value echo can fresh-line from C. */
int cl_stream_fresh_line(CL_Obj stream)
{
    CL_Stream *st;
    stream = cl_stream_resolve_backing(stream, 1);
    if (!CL_STREAM_P(stream)) return 0;
    st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
    if (st->stream_type == CL_STREAM_BROADCAST) {
        int r = 0;
        CL_Obj comps = st->string_buf;
        /* the recursive write can block and compact — keep the cursor rooted */
        CL_GC_PROTECT(comps);
        while (CL_CONS_P(comps)) {
            r = cl_stream_fresh_line(cl_car(comps));
            comps = cl_cdr(comps);
        }
        CL_GC_UNPROTECT(1);
        return r;
    }
    if (st->charpos != 0) {
        cl_stream_write_char(stream, '\n');
        return 1;
    }
    return 0;
}

static CL_Obj bi_fresh_line(CL_Obj *args, int n)
{
    CL_Obj stream = cl_resolve_output_stream(args, n, 0);
    return cl_stream_fresh_line(stream) ? CL_T : CL_NIL;
}

/* Flush a stream for finish/force-output.  Shared so a socket write deadline
 * that elapses while draining the buffer raises EXT:SOCKET-TIMEOUT here too,
 * rather than being silently swallowed (these are what hunchentoot calls to
 * push a response).
 *
 * Takes the stream OBJECT, not a CL_Stream*, because the first thing it must
 * do is unwrap synonym / two-way wrappers: *TERMINAL-IO* is a two-way stream
 * over the console (cl_stream_init), so dispatching on the wrapper's own
 * stream_type matched no branch and (finish-output t) flushed nothing. */
static void flush_output_stream(CL_Obj stream)
{
    CL_Stream *st;

    stream = cl_stream_resolve_backing(stream, 1);
    if (!CL_STREAM_P(stream))
        return;
    st = (CL_Stream *)CL_OBJ_TO_PTR(stream);

    if (st->stream_type == CL_STREAM_CONSOLE) {
        platform_flush_output();
    } else if (st->stream_type == CL_STREAM_FILE) {
        platform_file_flush((PlatformFile)st->handle_id);
    } else if (st->stream_type == CL_STREAM_SOCKET) {
        /* Read the deadline BEFORE the flush: draining the buffer parks in a
         * GC safe region, a peer can compact, and `st` is then a stale arena
         * pointer that must not be dereferenced for the message. */
        uint32_t wto = st->write_timeout_ms;
        if (platform_socket_flush((PlatformSocket)st->handle_id) == PLATFORM_SOCKET_TIMEOUT)
            cl_error(CL_ERR_TIMEOUT, "WRITE on socket stream timed out after %u ms",
                     (unsigned)wto);
    } else if (st->stream_type == CL_STREAM_BROADCAST) {
        /* Fan the flush out to every component (CLHS 21.1.1.1.1).  A child
         * flush can block in a GC safe region and compact the arena, so keep
         * the cursor rooted and re-derive each child from the rooted list. */
        CL_Obj comps = st->string_buf;
        CL_GC_PROTECT(comps);
        while (CL_CONS_P(comps)) {
            flush_output_stream(cl_car(comps));
            comps = cl_cdr(comps);
        }
        CL_GC_UNPROTECT(1);
    }
}

/* (finish-output &optional stream) => NIL
 *
 * The stream argument is a stream DESIGNATOR (CLHS 21.1.1.1): absent or NIL
 * means *STANDARD-OUTPUT*, T means *TERMINAL-IO*.  Both defaults used to fall
 * through the old `n > 0 && CL_STREAM_P(args[0])` guard, which made the bare
 * (finish-output) that batch scripts and test harnesses write a no-op. */
static CL_Obj bi_finish_output(CL_Obj *args, int n)
{
    flush_output_stream(cl_resolve_output_stream(args, n, 0));
    return CL_NIL;
}

/* (force-output &optional stream) => NIL */
static CL_Obj bi_force_output(CL_Obj *args, int n)
{
    flush_output_stream(cl_resolve_output_stream(args, n, 0));
    return CL_NIL;
}

/* (clear-output &optional stream) => NIL */
static CL_Obj bi_clear_output(CL_Obj *args, int n)
{
    CL_UNUSED(args); CL_UNUSED(n);
    return CL_NIL;
}

/* (open-stream-p stream) => T or NIL */
static CL_Obj bi_open_stream_p(CL_Obj *args, int n)
{
    CL_Stream *st;
    CL_UNUSED(n);
    if (!CL_STREAM_P(args[0]))
        cl_error(CL_ERR_TYPE, "OPEN-STREAM-P: argument is not a stream");
    st = (CL_Stream *)CL_OBJ_TO_PTR(args[0]);
    return (st->flags & CL_STREAM_FLAG_OPEN) ? CL_T : CL_NIL;
}

/* (close stream &key :abort) => T */
static CL_Obj bi_close(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    if (!CL_STREAM_P(args[0]))
        cl_error(CL_ERR_TYPE, "CLOSE: argument is not a stream");
    cl_stream_close(args[0]);
    return CL_T;
}

/* (make-string-input-stream string &optional start end) */
static CL_Obj bi_make_string_input_stream(CL_Obj *args, int n)
{
    uint32_t start = 0, end, slen;

    /* CLHS: STRING is any string, which includes an adjustable /
     * fill-pointer character vector (CL_STRING_VECTOR_P) -- the buffer
     * shape a parser builds with VECTOR-PUSH-EXTEND and then hands to
     * READ-FROM-STRING (yason's parse-number does exactly that).  The
     * stream reads through cl_string_char_at, which already handles that
     * representation; only this entry check and the length lookup were
     * base/wide-string only.  The active length honors the fill pointer. */
    if (!CL_ANY_STRING_P(args[0]) && !CL_STRING_VECTOR_P(args[0]))
        cl_error(CL_ERR_TYPE, "MAKE-STRING-INPUT-STREAM: argument is not a string");
    slen = cl_string_length(args[0]);
    end = slen;

    if (n >= 2 && CL_FIXNUM_P(args[1]))
        start = (uint32_t)CL_FIXNUM_VAL(args[1]);
    if (n >= 3 && !CL_NULL_P(args[2]) && CL_FIXNUM_P(args[2]))
        end = (uint32_t)CL_FIXNUM_VAL(args[2]);

    if (start > slen) start = slen;
    if (end > slen) end = slen;

    {
        CL_Obj s = cl_make_string_input_stream(args[0], start, end);
        if (!CL_NULL_P(s)) {
            /* handle_id is otherwise unused for CL_STREAM_STRING input
             * streams; repurpose it to remember the original :START so
             * FILE-POSITION :START can rewind to the stream's actual
             * beginning (CLHS position-spec :START = "the beginning of
             * the stream"), not absolute index 0 of the underlying
             * string when :START was non-zero. */
            CL_Stream *s_st = (CL_Stream *)CL_OBJ_TO_PTR(s);
            s_st->handle_id = start;
        }
        return s;
    }
}

/* (make-string-output-stream &key :element-type) */
static CL_Obj bi_make_string_output_stream(CL_Obj *args, int n)
{
    CL_UNUSED(args); CL_UNUSED(n);
    return cl_make_string_output_stream();
}

/* (make-synonym-stream symbol) */
static CL_Obj bi_make_synonym_stream(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    if (!CL_SYMBOL_P(args[0]))
        cl_error(CL_ERR_TYPE, "MAKE-SYNONYM-STREAM: argument must be a symbol");
    return cl_make_synonym_stream(args[0]);
}

/* (synonym-stream-symbol stream) */
static CL_Obj bi_synonym_stream_symbol(CL_Obj *args, int n)
{
    CL_Stream *st;
    CL_UNUSED(n);
    if (!CL_STREAM_P(args[0]))
        cl_error(CL_ERR_TYPE, "SYNONYM-STREAM-SYMBOL: argument is not a stream");
    st = (CL_Stream *)CL_OBJ_TO_PTR(args[0]);
    if (st->stream_type != CL_STREAM_SYNONYM)
        cl_error(CL_ERR_TYPE, "SYNONYM-STREAM-SYMBOL: not a synonym stream");
    return st->string_buf;
}

/* Gray stream helpers for make-two-way-stream */
extern int cl_clos_type_matches(CL_Obj obj_type, CL_Obj test_type);

static void gray_fund_syms_init(void)
{
    /* Guard the lazy init with the write lock so only one thread performs the
     * lookup; subsequent threads find cached values and skip the block.
     * cl_find_package / cl_package_find_symbol use cl_package_rwlock, which is
     * a separate lock from cl_tables_wrlock — no re-entrancy / deadlock risk. */
    cl_tables_wrlock();
    if (CL_NULL_P(GRAY_FUND_INPUT_SYM) || CL_NULL_P(GRAY_FUND_OUTPUT_SYM)) {
        CL_Obj gray_pkg = cl_find_package("GRAY", 4);
        if (!CL_NULL_P(gray_pkg)) {
            if (CL_NULL_P(GRAY_FUND_INPUT_SYM)) {
                CL_Obj sym = cl_package_find_symbol("FUNDAMENTAL-INPUT-STREAM", 24, gray_pkg);
                if (!CL_NULL_P(sym) && CL_SYMBOL_P(sym))
                    GRAY_FUND_INPUT_SYM = sym;
            }
            if (CL_NULL_P(GRAY_FUND_OUTPUT_SYM)) {
                CL_Obj sym = cl_package_find_symbol("FUNDAMENTAL-OUTPUT-STREAM", 25, gray_pkg);
                if (!CL_NULL_P(sym) && CL_SYMBOL_P(sym))
                    GRAY_FUND_OUTPUT_SYM = sym;
            }
        }
    }
    cl_tables_rwunlock();
}

static int obj_is_gray_subclass(CL_Obj obj, CL_Obj class_sym)
{
    CL_Struct *st;
    if (!CL_STRUCT_P(obj) || CL_NULL_P(class_sym)) return 0;
    st = (CL_Struct *)CL_OBJ_TO_PTR(obj);
    return cl_clos_type_matches(st->type_desc, class_sym);
}

static int is_input_stream_designator(CL_Obj o)
{
    if (CL_STREAM_P(o) && (((CL_Stream *)CL_OBJ_TO_PTR(o))->direction & CL_STREAM_INPUT))
        return 1;
    gray_fund_syms_init();
    return obj_is_gray_subclass(o, GRAY_FUND_INPUT_SYM);
}

static int is_output_stream_designator(CL_Obj o)
{
    if (CL_STREAM_P(o) && (((CL_Stream *)CL_OBJ_TO_PTR(o))->direction & CL_STREAM_OUTPUT))
        return 1;
    gray_fund_syms_init();
    return obj_is_gray_subclass(o, GRAY_FUND_OUTPUT_SYM);
}

/* (make-two-way-stream input-stream output-stream) */
static CL_Obj bi_make_two_way_stream(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    if (!is_input_stream_designator(args[0]))
        cl_error(CL_ERR_TYPE, "MAKE-TWO-WAY-STREAM: first argument is not an input stream");
    if (!is_output_stream_designator(args[1]))
        cl_error(CL_ERR_TYPE, "MAKE-TWO-WAY-STREAM: second argument is not an output stream");
    return cl_make_two_way_stream(args[0], args[1]);
}

/* (two-way-stream-input-stream stream) */
static CL_Obj bi_two_way_stream_input_stream(CL_Obj *args, int n)
{
    CL_Stream *st;
    CL_UNUSED(n);
    if (!CL_STREAM_P(args[0]))
        cl_error(CL_ERR_TYPE, "TWO-WAY-STREAM-INPUT-STREAM: argument is not a stream");
    st = (CL_Stream *)CL_OBJ_TO_PTR(args[0]);
    if (st->stream_type != CL_STREAM_TWO_WAY)
        cl_error(CL_ERR_TYPE, "TWO-WAY-STREAM-INPUT-STREAM: not a two-way stream");
    return st->string_buf;
}

/* (two-way-stream-output-stream stream) */
static CL_Obj bi_two_way_stream_output_stream(CL_Obj *args, int n)
{
    CL_Stream *st;
    CL_UNUSED(n);
    if (!CL_STREAM_P(args[0]))
        cl_error(CL_ERR_TYPE, "TWO-WAY-STREAM-OUTPUT-STREAM: argument is not a stream");
    st = (CL_Stream *)CL_OBJ_TO_PTR(args[0]);
    if (st->stream_type != CL_STREAM_TWO_WAY)
        cl_error(CL_ERR_TYPE, "TWO-WAY-STREAM-OUTPUT-STREAM: not a two-way stream");
    return st->element_type;
}

/* (make-echo-stream input-stream output-stream)
 * Bidirectional: reads come from input-stream and every element read is
 * echoed to output-stream; direct writes go to output-stream unechoed
 * (CLHS 21.1, 21.2).  The echo path in stream.c only dispatches to native
 * CL_Stream objects, so — as for broadcast/concatenated streams, and unlike
 * make-two-way-stream with its Lisp-level Gray shim — Gray-stream (CLOS)
 * components are rejected here rather than silently reading as EOF or
 * dropping the echo. */
static CL_Obj bi_make_echo_stream(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    if (!is_input_stream_designator(args[0]))
        cl_error(CL_ERR_TYPE, "MAKE-ECHO-STREAM: first argument is not an input stream");
    if (!is_output_stream_designator(args[1]))
        cl_error(CL_ERR_TYPE, "MAKE-ECHO-STREAM: second argument is not an output stream");
    if (!CL_STREAM_P(args[0]))
        cl_error(CL_ERR_TYPE, "MAKE-ECHO-STREAM: first argument is a Gray stream, which is "
                 "not supported as an echo-stream component");
    if (!CL_STREAM_P(args[1]))
        cl_error(CL_ERR_TYPE, "MAKE-ECHO-STREAM: second argument is a Gray stream, which is "
                 "not supported as an echo-stream component");
    return cl_make_echo_stream(args[0], args[1]);
}

/* (echo-stream-input-stream echo-stream) */
static CL_Obj bi_echo_stream_input_stream(CL_Obj *args, int n)
{
    CL_Stream *st;
    CL_UNUSED(n);
    if (!CL_STREAM_P(args[0]) ||
        ((CL_Stream *)CL_OBJ_TO_PTR(args[0]))->stream_type != CL_STREAM_ECHO)
        cl_error(CL_ERR_TYPE, "ECHO-STREAM-INPUT-STREAM: argument is not an echo stream");
    st = (CL_Stream *)CL_OBJ_TO_PTR(args[0]);
    return st->string_buf;
}

/* (echo-stream-output-stream echo-stream) */
static CL_Obj bi_echo_stream_output_stream(CL_Obj *args, int n)
{
    CL_Stream *st;
    CL_UNUSED(n);
    if (!CL_STREAM_P(args[0]) ||
        ((CL_Stream *)CL_OBJ_TO_PTR(args[0]))->stream_type != CL_STREAM_ECHO)
        cl_error(CL_ERR_TYPE, "ECHO-STREAM-OUTPUT-STREAM: argument is not an echo stream");
    st = (CL_Stream *)CL_OBJ_TO_PTR(args[0]);
    return st->element_type;
}

/* (make-broadcast-stream &rest streams)
 * Output written to the result is fanned out to every component stream;
 * with no components all output is discarded (CLHS 21.1).
 * The C fan-out in stream.c only dispatches to native CL_Stream objects, so
 * Gray-stream (CLOS) components — unlike make-two-way-stream, which has a
 * Lisp-level shim that dispatches to Gray generic functions — must be
 * rejected here rather than silently dropping their output. */
static CL_Obj bi_make_broadcast_stream(CL_Obj *args, int n)
{
    CL_Obj list = CL_NIL;
    CL_Obj s;
    int i;
    for (i = 0; i < n; i++) {
        if (!is_output_stream_designator(args[i]))
            cl_error(CL_ERR_TYPE,
                     "MAKE-BROADCAST-STREAM: argument %d is not an output stream",
                     i + 1);
        if (!CL_STREAM_P(args[i]))
            cl_error(CL_ERR_TYPE,
                     "MAKE-BROADCAST-STREAM: argument %d is a Gray stream, which is "
                     "not supported as a broadcast-stream component",
                     i + 1);
    }
    /* args[] is GC-rooted by cl_vm_apply; the list under construction is
     * not — protect it across the consing loop and the constructor. */
    CL_GC_PROTECT(list);
    for (i = n - 1; i >= 0; i--)
        list = cl_cons(args[i], list);
    s = cl_make_broadcast_stream_list(list);
    CL_GC_UNPROTECT(1);
    return s;
}

/* (broadcast-stream-streams broadcast-stream) */
static CL_Obj bi_broadcast_stream_streams(CL_Obj *args, int n)
{
    CL_Stream *st;
    CL_UNUSED(n);
    if (!CL_STREAM_P(args[0]) ||
        ((CL_Stream *)CL_OBJ_TO_PTR(args[0]))->stream_type != CL_STREAM_BROADCAST)
        cl_error(CL_ERR_TYPE, "BROADCAST-STREAM-STREAMS: argument is not a broadcast stream");
    st = (CL_Stream *)CL_OBJ_TO_PTR(args[0]);
    return st->string_buf;
}

/* (make-concatenated-stream &rest streams)
 * Reads components left to right, popping each at EOF; with no components
 * the stream is immediately at end of file (CLHS 21.1).
 * The C read loop in stream.c only dispatches to native CL_Stream objects, so
 * Gray-stream (CLOS) components must be rejected here rather than being
 * silently skipped as if they were already at EOF. */
static CL_Obj bi_make_concatenated_stream(CL_Obj *args, int n)
{
    CL_Obj list = CL_NIL;
    CL_Obj s;
    int i;
    for (i = 0; i < n; i++) {
        if (!is_input_stream_designator(args[i]))
            cl_error(CL_ERR_TYPE,
                     "MAKE-CONCATENATED-STREAM: argument %d is not an input stream",
                     i + 1);
        if (!CL_STREAM_P(args[i]))
            cl_error(CL_ERR_TYPE,
                     "MAKE-CONCATENATED-STREAM: argument %d is a Gray stream, which is "
                     "not supported as a concatenated-stream component",
                     i + 1);
    }
    CL_GC_PROTECT(list);
    for (i = n - 1; i >= 0; i--)
        list = cl_cons(args[i], list);
    s = cl_make_concatenated_stream_list(list);
    CL_GC_UNPROTECT(1);
    return s;
}

/* (concatenated-stream-streams concatenated-stream)
 * Returns the streams still to be read from, current one first (CLHS) —
 * exactly the internal remaining-components list. */
static CL_Obj bi_concatenated_stream_streams(CL_Obj *args, int n)
{
    CL_Stream *st;
    CL_UNUSED(n);
    if (!CL_STREAM_P(args[0]) ||
        ((CL_Stream *)CL_OBJ_TO_PTR(args[0]))->stream_type != CL_STREAM_CONCATENATED)
        cl_error(CL_ERR_TYPE, "CONCATENATED-STREAM-STREAMS: argument is not a concatenated stream");
    st = (CL_Stream *)CL_OBJ_TO_PTR(args[0]);
    return st->string_buf;
}

/* (get-output-stream-string stream) */
static CL_Obj bi_get_output_stream_string(CL_Obj *args, int n)
{
    CL_Stream *st;
    CL_UNUSED(n);
    if (!CL_STREAM_P(args[0]))
        cl_error(CL_ERR_TYPE, "GET-OUTPUT-STREAM-STRING: argument is not a stream");
    st = (CL_Stream *)CL_OBJ_TO_PTR(args[0]);
    if (st->stream_type != CL_STREAM_STRING || !(st->direction & CL_STREAM_OUTPUT))
        cl_error(CL_ERR_TYPE, "GET-OUTPUT-STREAM-STRING: not a string output stream");
    return cl_get_output_stream_string(args[0]);
}

/* --- File streams (Step 8) --- */

/* Helper: find keyword value in args list (keyword arg pairs start at idx) */
static CL_Obj find_keyword_arg(CL_Obj *args, int n, int start, CL_Obj keyword)
{
    int i;
    for (i = start; i + 1 < n; i += 2) {
        if (args[i] == keyword)
            return args[i + 1];
    }
    return CL_UNBOUND;  /* Not found */
}

/* Coerce pathname designator to a CL string object, modifying *arg in place.
   Returns the C string data pointer, or NULL if not a valid designator.
   Per CL spec, pathname designators include strings, pathnames, and
   file streams (which use the pathname associated with the stream). */
static const char *coerce_to_filename(CL_Obj *arg)
{
    if (CL_PATHNAME_P(*arg)) {
        char ns_buf[1024];
        extern const char *cl_coerce_to_namestring(CL_Obj, char *, uint32_t);
        cl_coerce_to_namestring(*arg, ns_buf, sizeof(ns_buf));
        *arg = cl_make_string(ns_buf, (uint32_t)strlen(ns_buf));
    }
    /* File streams are pathname designators — extract stored pathname */
    if (CL_STREAM_P(*arg)) {
        CL_Stream *st = (CL_Stream *)CL_OBJ_TO_PTR(*arg);
        if (st->stream_type == CL_STREAM_FILE && CL_ANY_STRING_P(st->string_buf)) {
            *arg = st->string_buf;
        } else {
            return NULL;
        }
    }
#ifdef CL_WIDE_STRINGS
    /* Convert wide string to base string (UTF-8) for OS path calls */
    if (CL_WIDE_STRING_P(*arg)) {
        char path_buf[1024];
        uint32_t nb = cl_wide_string_to_utf8(*arg, path_buf, sizeof(path_buf));
        *arg = cl_make_string(path_buf, nb);
    }
#endif
    if (!CL_STRING_P(*arg)) return NULL;
    /* Expand leading ~ to home directory */
    {
        CL_String *s = (CL_String *)CL_OBJ_TO_PTR(*arg);
        if (s->length > 0 && s->data[0] == '~') {
            char expand_buf[1024];
            const char *expanded = platform_expand_home(s->data,
                expand_buf, (int)sizeof(expand_buf));
            if (expanded != s->data) {
                *arg = cl_make_string(expanded, (uint32_t)strlen(expanded));
            }
        }
    }
    return ((CL_String *)CL_OBJ_TO_PTR(*arg))->data;
}

/* True if an :external-format designator names LATIN-1 / ISO-8859-1.
 * Accepts a bare keyword (:latin-1, :latin1, :iso-8859-1, :iso8859-1) or a
 * list whose first element is one of those (the (:latin-1 :eol-style :lf)
 * form some libraries pass).  Anything else — including :default and :utf-8 —
 * means the amilisp default (UTF-8), so we return 0. */
static int format_is_latin1(CL_Obj fmt)
{
    const char *name;
    if (fmt == CL_UNBOUND || CL_NULL_P(fmt))
        return 0;
    if (CL_CONS_P(fmt))
        fmt = ((CL_Cons *)CL_OBJ_TO_PTR(fmt))->car;
    if (!CL_SYMBOL_OR_NIL_P(fmt))
        return 0;
    name = cl_symbol_name(fmt);
    return strcmp(name, "LATIN-1") == 0
        || strcmp(name, "LATIN1") == 0
        || strcmp(name, "ISO-8859-1") == 0
        || strcmp(name, "ISO8859-1") == 0;
}

/* (open filename &key :direction :if-exists :if-does-not-exist :external-format) */
static CL_Obj bi_open(CL_Obj *args, int n)
{
    CL_String *path_str;
    CL_Obj direction_val, if_exists_val, if_dne_val;
    CL_Obj direction, if_exists, if_dne;
    int platform_mode;
    uint32_t stream_dir;
    PlatformFile fh;
    CL_Obj stream;
    CL_Stream *st;

    /* Per CL spec (and as LOAD already does): merge the filespec with
     * *default-pathname-defaults* so a relative pathname resolves against the
     * caller's bound default rather than just the process cwd.  Without this,
     * (with-open-file (f #P"fz.jpg") ...) under a rebound
     * *default-pathname-defaults* (e.g. a test harness) fails to find the
     * file.  Streams (pathname designators) are left to coerce_to_filename. */
    if (CL_PATHNAME_P(args[0]) || CL_ANY_STRING_P(args[0])) {
        extern CL_Obj cl_parse_namestring(const char *str, uint32_t len);
        extern const char *cl_coerce_to_namestring(CL_Obj arg, char *buf, uint32_t bufsz);
        extern CL_Obj bi_merge_pathnames(CL_Obj *margs, int mn);
        CL_Obj path_pn, merge_args[1];
        char ns_buf[1024];
        if (CL_PATHNAME_P(args[0])) {
            path_pn = args[0];
        } else {
#ifdef CL_WIDE_STRINGS
            if (CL_WIDE_STRING_P(args[0])) {
                char wbuf[1024];
                uint32_t wn = cl_wide_string_to_utf8(args[0], wbuf, sizeof(wbuf));
                path_pn = cl_parse_namestring(wbuf, wn);
            } else
#endif
            {
                CL_String *s = (CL_String *)CL_OBJ_TO_PTR(args[0]);
                path_pn = cl_parse_namestring(s->data, s->length);
            }
        }
        CL_GC_PROTECT(path_pn);
        merge_args[0] = path_pn;
        path_pn = bi_merge_pathnames(merge_args, 1);
        CL_GC_UNPROTECT(1);
        cl_coerce_to_namestring(path_pn, ns_buf, sizeof(ns_buf));
        args[0] = cl_make_string(ns_buf, (uint32_t)strlen(ns_buf));
    }

    if (!coerce_to_filename(&args[0]))
        cl_error(CL_ERR_TYPE, "OPEN: filename must be a string or pathname");
    path_str = (CL_String *)CL_OBJ_TO_PTR(args[0]);

    /* Parse keyword arguments */
    direction_val = find_keyword_arg(args, n, 1, KW_DIRECTION);
    if_exists_val = find_keyword_arg(args, n, 1, KW_IF_EXISTS);
    if_dne_val    = find_keyword_arg(args, n, 1, KW_IF_DOES_NOT_EXIST);

    /* Default direction is :input */
    direction = (direction_val != CL_UNBOUND) ? direction_val : KW_INPUT;

    /* Determine defaults based on direction */
    if (direction == KW_INPUT) {
        if_exists = (if_exists_val != CL_UNBOUND) ? if_exists_val : CL_NIL; /* don't care */
        if_dne    = (if_dne_val != CL_UNBOUND)    ? if_dne_val    : KW_ERROR_KW;
        stream_dir = CL_STREAM_INPUT;
        platform_mode = PLATFORM_FILE_READ;
    } else if (direction == KW_OUTPUT) {
        if_exists = (if_exists_val != CL_UNBOUND) ? if_exists_val : KW_NEW_VERSION;
        if_dne    = (if_dne_val != CL_UNBOUND)    ? if_dne_val    : KW_CREATE;
        stream_dir = CL_STREAM_OUTPUT;

        if (if_exists == KW_APPEND) {
            platform_mode = PLATFORM_FILE_APPEND;
        } else {
            /* :supersede, :new-version, :overwrite, :rename,
             * :rename-and-delete → truncate/create */
            platform_mode = PLATFORM_FILE_WRITE;
        }
    } else if (direction == KW_IO) {
        if_exists = (if_exists_val != CL_UNBOUND) ? if_exists_val : KW_NEW_VERSION;
        if_dne    = (if_dne_val != CL_UNBOUND)    ? if_dne_val    : KW_CREATE;
        stream_dir = CL_STREAM_IO;
        platform_mode = PLATFORM_FILE_APPEND; /* read+write, no truncate */
    } else if (direction == KW_PROBE) {
        /* :probe — check existence, optionally create, return NIL */
        if_dne = (if_dne_val != CL_UNBOUND) ? if_dne_val : CL_NIL;
        if (if_dne == KW_CREATE) {
            /* Create the file if it doesn't exist */
            PlatformFile test = platform_file_open(path_str->data, PLATFORM_FILE_READ);
            if (test != PLATFORM_FILE_INVALID) {
                platform_file_close(test);
            } else {
                test = platform_file_open(path_str->data, PLATFORM_FILE_WRITE);
                if (test != PLATFORM_FILE_INVALID)
                    platform_file_close(test);
            }
        } else if (if_dne == KW_ERROR_KW) {
            PlatformFile test = platform_file_open(path_str->data, PLATFORM_FILE_READ);
            if (test == PLATFORM_FILE_INVALID)
                cl_error(CL_ERR_GENERAL, "OPEN: file does not exist \"%s\"",
                         path_str->data);
            platform_file_close(test);
        }
        return CL_NIL;
    } else {
        cl_error(CL_ERR_GENERAL, "OPEN: invalid :direction");
        return CL_NIL;
    }

    /* Handle :if-exists for output/io directions */
    if (direction != KW_INPUT && direction != KW_PROBE) {
        PlatformFile test = platform_file_open(path_str->data, PLATFORM_FILE_READ);
        int file_exists = (test != PLATFORM_FILE_INVALID);
        if (file_exists) platform_file_close(test);

        if (file_exists) {
            if (if_exists == KW_ERROR_KW) {
                cl_error(CL_ERR_GENERAL, "OPEN: file already exists \"%s\"",
                         path_str->data);
            } else if (CL_NULL_P(if_exists)) {
                /* :if-exists nil — return NIL */
                return CL_NIL;
            } else if (if_exists == KW_RENAME || if_exists == KW_RENAME_AND_DELETE) {
                /* Rename existing file to .bak, then open fresh */
                char backup[512];
                snprintf(backup, sizeof(backup), "%s.bak", path_str->data);
                platform_file_rename(path_str->data, backup);
                if (if_exists == KW_RENAME_AND_DELETE) {
                    /* Delete backup after rename (per spec: old file is
                     * deleted once new stream is successfully opened) */
                    platform_file_delete(backup);
                }
            } else if (if_exists == KW_SUPERSEDE || if_exists == KW_NEW_VERSION
                       || if_exists == KW_OVERWRITE) {
                /* Delete existing file first so platform_file_open can
                 * create a fresh one (required on AmigaOS where
                 * MODE_NEWFILE fails on existing files) */
                platform_file_delete(path_str->data);
            }
            /* :append — handled by platform_mode above */
        }
    }

    /* Open the file */
    fh = platform_file_open(path_str->data, platform_mode);
    if (fh == PLATFORM_FILE_INVALID) {
        /* :if-does-not-exist nil — return NIL silently */
        if (CL_NULL_P(if_dne))
            return CL_NIL;
        cl_error(CL_ERR_GENERAL, "OPEN: cannot open file \"%s\"",
                 path_str->data);
        return CL_NIL;
    }

    /* Create stream object */
    stream = cl_make_stream(stream_dir, CL_STREAM_FILE);
    st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
    st->handle_id = (uint32_t)fh;
    st->string_buf = args[0];  /* Store pathname string for probe-file/pathname */

    /* Record :element-type so STREAM-ELEMENT-TYPE reports it.  A non-character
     * type (e.g. (unsigned-byte 8)) marks a binary stream — drakma's
     * SEND-CONTENT checks (subtypep (stream-element-type s) 'octet) before it
     * will stream a file body, so getting this right is what lets a file POST
     * (multipart form-data) work.  Default (absent) is CHARACTER. */
    {
        CL_Obj et = find_keyword_arg(args, n, 1, KW_ELEMENT_TYPE_OPEN);
        if (et != CL_UNBOUND && !CL_NULL_P(et))
            st->element_type = et;
    }

    /* Record :external-format.  amilisp character streams are UTF-8 by default;
     * :latin-1 / :iso-8859-1 selects the 8-bit-transparent path so each byte
     * 0..255 round-trips verbatim (needed for faithful binary I/O over a
     * character stream — e.g. rfc2388 writing a multipart upload to a temp
     * file).  Anything else (incl. :default / :utf-8) keeps the UTF-8 default. */
    {
        CL_Obj ef = find_keyword_arg(args, n, 1, KW_EXTERNAL_FORMAT);
        if (format_is_latin1(ef))
            st->flags |= CL_STREAM_FLAG_LATIN1;
    }

    return stream;
}

/* (file-position stream &optional position) => position or T/NIL */
static CL_Obj bi_file_position(CL_Obj *args, int n)
{
    CL_Stream *st;
    if (!CL_STREAM_P(args[0]))
        cl_error(CL_ERR_TYPE, "FILE-POSITION: not a stream");
    st = (CL_Stream *)CL_OBJ_TO_PTR(args[0]);
    if (st->stream_type == CL_STREAM_STRING) {
        if (n > 1 && !CL_NULL_P(args[1])) {
            /* Set position; discard any pushed-back char so the next read
             * starts cleanly at the requested index.  CLHS position-spec is
             * :START, :END, or a non-negative integer.  For input streams
             * out_buf_len is the end bound in code points; for output
             * streams it is the bytes written so far — either way it is
             * the stream's end. */
            uint32_t newpos;
            if (args[1] == KW_START)
                /* handle_id holds the stream's original :START offset for
                 * an input string stream (see bi_make_string_input_stream);
                 * 0 for output string streams and for input streams created
                 * with no explicit :START, so this is a no-op for those. */
                newpos = st->handle_id;
            else if (args[1] == KW_END)
                newpos = st->out_buf_len;
            else if (CL_FIXNUM_P(args[1]) && CL_FIXNUM_VAL(args[1]) >= 0)
                newpos = (uint32_t)CL_FIXNUM_VAL(args[1]);
            else
                cl_signal_type_error(args[1],
                                     "(OR (INTEGER 0 *) (MEMBER :START :END))",
                                     "FILE-POSITION");
            st->position = newpos;
            st->unread_char = -1;
            return CL_T;
        }
        /* A pushed-back char (from PEEK-CHAR/UNREAD-CHAR) has already advanced
         * st->position even though it will be re-served by the next READ-CHAR,
         * so the logical read position is one code point earlier.  This is what
         * makes WITH-INPUT-FROM-STRING's :INDEX (and thus PARSE-FLOAT/-INTEGER's
         * second value) correct when a parser peeks past trailing junk. */
        {
            int32_t pos = (int32_t)st->position;
            if (st->unread_char != -1 && pos > 0) pos--;
            return CL_MAKE_FIXNUM(pos);
        }
    }
    if (st->stream_type != CL_STREAM_FILE)
        return CL_NIL;
    if (n > 1 && !CL_NULL_P(args[1])) {
        /* Set position — CLHS position-spec is :START, :END, or a
         * non-negative integer; accept a bignum too: positions past
         * CL_FIXNUM_MAX (~1GB) are bignums on 32-bit, and the getter
         * below returns them. */
        long pos = 0;
        if (args[1] == KW_START) {
            pos = 0;
        } else if (args[1] == KW_END) {
            /* Bytes may still be sitting in the write-behind buffer (Amiga's
             * IOBuf); flush before asking the OS for the length, else :END
             * seeks to a stale, too-small position. */
            platform_file_flush((PlatformFile)st->handle_id);
            pos = platform_file_length((PlatformFile)st->handle_id);
            if (pos < 0) return CL_NIL;
        } else if (CL_FIXNUM_P(args[1])) {
            pos = (long)CL_FIXNUM_VAL(args[1]);
            if (pos < 0)
                cl_signal_type_error(args[1],
                                     "(OR (INTEGER 0 *) (MEMBER :START :END))",
                                     "FILE-POSITION");
        } else if (CL_BIGNUM_P(args[1])) {
            CL_Bignum *b = (CL_Bignum *)CL_OBJ_TO_PTR(args[1]);
            uint32_t v = 0;
            if (b->sign || b->length > 2)
                cl_error(CL_ERR_ARGS,
                         "FILE-POSITION: position out of range");
            if (b->length > 0) v = b->limbs[0];
            if (b->length > 1) v |= (uint32_t)b->limbs[1] << 16;
            if (v > 0x7FFFFFFFu)
                cl_error(CL_ERR_ARGS,
                         "FILE-POSITION: position out of range");
            pos = (long)v;
        } else {
            cl_signal_type_error(args[1],
                                 "(OR (INTEGER 0 *) (MEMBER :START :END))",
                                 "FILE-POSITION");
        }
        return platform_file_set_position((PlatformFile)st->handle_id, pos) == 0
            ? CL_T : CL_NIL;
    }
    /* Get position — positions past CL_FIXNUM_MAX must come back as a
     * bignum, not silently truncated by the fixnum tag shift (files past
     * ~1GB reported garbage offsets). */
    {
        long pos = platform_file_position((PlatformFile)st->handle_id);
        if (pos < 0) return CL_NIL;
        if (pos > (long)CL_FIXNUM_MAX)
            return cl_bignum_from_uint32((uint32_t)pos);
        return CL_MAKE_FIXNUM((int32_t)pos);
    }
}

/* (file-length stream) => integer or NIL */
static CL_Obj bi_file_length(CL_Obj *args, int n)
{
    CL_Stream *st;
    long cur, end;
    (void)n;
    if (!CL_STREAM_P(args[0]))
        cl_error(CL_ERR_TYPE, "FILE-LENGTH: not a stream");
    st = (CL_Stream *)CL_OBJ_TO_PTR(args[0]);
    if (st->stream_type != CL_STREAM_FILE)
        return CL_NIL;
    cur = platform_file_position((PlatformFile)st->handle_id);
    if (cur < 0) return CL_NIL;
    end = platform_file_length((PlatformFile)st->handle_id);
    platform_file_set_position((PlatformFile)st->handle_id, cur);
    if (end < 0) return CL_NIL;
    if (end > (long)CL_FIXNUM_MAX)
        return cl_bignum_from_uint32((uint32_t)end);
    return CL_MAKE_FIXNUM((int32_t)end);
}

/* --- Time functions (Step 10) --- */

/* (get-universal-time) => integer */
static CL_Obj bi_get_universal_time(CL_Obj *args, int n)
{
    uint32_t ut;
    CL_UNUSED(args); CL_UNUSED(n);
    ut = platform_universal_time();
    /* Universal time is large (>3.9 billion) — needs bignum on 32-bit */
    return cl_bignum_from_uint32(ut);
}

/* --- File system functions (Step 10) --- */

/* If RESOLVED (length RLEN, capacity BUFSIZE) names a directory on disk and
 * lacks a trailing slash, append one so the pathname is in CL "directory form"
 * (pathname-directory set; name/type NIL).  platform_realpath strips the
 * trailing slash even for directories, so we re-derive directory-ness from the
 * filesystem object type rather than from the caller's input.  Conformant
 * TRUENAME/PROBE-FILE report an existing directory in directory form whether or
 * not the input had a trailing slash; without this,
 * (directory-pathname-p (truename "/tmp/foo")) is NIL for a real directory
 * /tmp/foo, so uiop:directory-exists-p — (and (directory-pathname-p
 * (probe-file* x :truename t)) ...) — wrongly returns NIL (chipi
 * MAKE-PERSISTENCE--SIMPLE).  Returns the possibly-updated length. */
static size_t ensure_dir_form_slash(char *resolved, size_t rlen, size_t bufsize)
{
    if (rlen > 0 && resolved[rlen - 1] != '/' && rlen + 1 < bufsize &&
        platform_file_is_directory(resolved)) {
        resolved[rlen] = '/';
        resolved[rlen + 1] = '\0';
        rlen++;
    }
    return rlen;
}

/* (truename pathname) => resolved pathname (symlinks resolved)
 * Signals FILE-ERROR if the file does not exist. */
static CL_Obj bi_truename(CL_Obj *args, int n)
{
    const char *path;
    char resolved[512];
    size_t rlen;
    CL_UNUSED(n);
    path = coerce_to_filename(&args[0]);
    if (!path)
        cl_error(CL_ERR_TYPE, "TRUENAME: argument must be a pathname designator");
    if (!platform_realpath(path, resolved, (int)sizeof(resolved)))
        cl_error(CL_ERR_FILE, "TRUENAME: file does not exist \"%s\"", path);
    rlen = ensure_dir_form_slash(resolved, strlen(resolved), sizeof(resolved));
    {
        extern CL_Obj cl_parse_namestring(const char *str, uint32_t len);
        return cl_parse_namestring(resolved, (uint32_t)rlen);
    }
}

/* (probe-file pathname) => truename or NIL */
static CL_Obj bi_probe_file(CL_Obj *args, int n)
{
    const char *path;
    char resolved[512];
    CL_UNUSED(n);
    path = coerce_to_filename(&args[0]);
    if (!path)
        cl_error(CL_ERR_TYPE, "PROBE-FILE: argument must be a pathname designator");
    if (!platform_file_exists(path)) return CL_NIL;
    /* Return truename (symlinks resolved) per CL spec */
    if (platform_realpath(path, resolved, (int)sizeof(resolved))) {
        size_t rlen = ensure_dir_form_slash(resolved, strlen(resolved),
                                            sizeof(resolved));
        {
            extern CL_Obj cl_parse_namestring(const char *str, uint32_t len);
            return cl_parse_namestring(resolved, (uint32_t)rlen);
        }
    }
    {
        extern CL_Obj cl_parse_namestring(const char *str, uint32_t len);
        CL_String *s = (CL_String *)CL_OBJ_TO_PTR(args[0]);
        return cl_parse_namestring(s->data, s->length);
    }
}

/* (delete-file pathname) => T */
static CL_Obj bi_delete_file(CL_Obj *args, int n)
{
    const char *path;
    CL_UNUSED(n);
    path = coerce_to_filename(&args[0]);
    if (!path)
        cl_error(CL_ERR_TYPE, "DELETE-FILE: argument must be a pathname designator");
    if (platform_file_delete(path) != 0)
        cl_error(CL_ERR_FILE, "DELETE-FILE: cannot delete file");
    return CL_T;
}

/* (rename-file filespec new-name) => new-name, old-truename, new-truename (3 values) */
static CL_Obj bi_rename_file(CL_Obj *args, int n)
{
    char old_buf[1024];
    const char *old_path, *new_path;
    CL_UNUSED(n);
    old_path = coerce_to_filename(&args[0]);
    if (!old_path)
        cl_error(CL_ERR_TYPE, "RENAME-FILE: first argument must be a pathname designator");
    /* old_path points into the arena; the second coercion below allocates
     * (pathname/wide-string/~ paths) and compaction would leave it
     * dangling — silently renaming the WRONG file.  Copy it out first. */
    strncpy(old_buf, old_path, sizeof(old_buf) - 1);
    old_buf[sizeof(old_buf) - 1] = '\0';
    new_path = coerce_to_filename(&args[1]);
    if (!new_path)
        cl_error(CL_ERR_TYPE, "RENAME-FILE: second argument must be a pathname designator");
    if (platform_file_rename(old_buf, new_path) != 0)
        cl_error(CL_ERR_GENERAL, "RENAME-FILE: cannot rename '%s' to '%s'",
                 old_buf, new_path);
    /* Return 3 values: new-name, old-truename, new-truename */
    cl_mv_values[0] = args[1];
    cl_mv_values[1] = args[0];
    cl_mv_values[2] = args[1];
    cl_mv_count = 3;
    return args[1];
}

/* (file-write-date pathname) => universal-time or NIL */
static CL_Obj bi_file_write_date(CL_Obj *args, int n)
{
    const char *path;
    uint32_t mtime;
    CL_UNUSED(n);
    path = coerce_to_filename(&args[0]);
    if (!path)
        cl_error(CL_ERR_TYPE, "FILE-WRITE-DATE: argument must be a pathname designator");
    mtime = platform_file_mtime(path);
    if (mtime == 0) return CL_NIL;
    return cl_bignum_from_uint32(mtime);
}

/* (directory pathname &key) => list of pathnames */
static CL_Obj bi_directory(CL_Obj *args, int n)
{
    const char *pattern;
    char adjusted_pattern[1024];
    char **entries;
    int count, i;
    CL_Obj result = CL_NIL;
    CL_UNUSED(n);

    pattern = coerce_to_filename(&args[0]);
    if (!pattern)
        cl_error(CL_ERR_TYPE, "DIRECTORY: argument must be a pathname designator");

    /* CL wildcard *.*  means "any name, any type (including no type)".
       POSIX glob *.*  only matches names containing a dot.
       Detect trailing *.* and replace with just * so glob matches everything. */
    {
        size_t plen = strlen(pattern);
        if (plen >= 3 &&
            pattern[plen - 1] == '*' && pattern[plen - 2] == '.' &&
            pattern[plen - 3] == '*') {
            if (plen - 3 < sizeof(adjusted_pattern)) {
                memcpy(adjusted_pattern, pattern, plen - 2);
                adjusted_pattern[plen - 2] = '\0';
                pattern = adjusted_pattern;
            }
        }
    }

    entries = platform_directory(pattern, &count);
    if (!entries) return CL_NIL;

    CL_GC_PROTECT(result);
    {
        CL_Obj tail = CL_NIL;
        CL_GC_PROTECT(tail);
        for (i = 0; i < count; i++) {
            extern CL_Obj cl_parse_namestring(const char *str, uint32_t len);
            CL_Obj pn = cl_parse_namestring(entries[i], (uint32_t)strlen(entries[i]));
            CL_Obj cell = cl_cons(pn, CL_NIL);
            if (CL_NULL_P(result)) {
                result = cell;
                tail = cell;
            } else {
                ((CL_Cons *)CL_OBJ_TO_PTR(tail))->cdr = cell;
                tail = cell;
            }
            free(entries[i]);
        }
        CL_GC_UNPROTECT(1); /* tail */
    }
    free(entries);
    CL_GC_UNPROTECT(1); /* result */
    return result;
}

/* (%mkdir pathname) => T or NIL — internal for ensure-directories-exist */
static CL_Obj bi_mkdir(CL_Obj *args, int n)
{
    CL_String *str;
    CL_UNUSED(n);
    if (!CL_ANY_STRING_P(args[0]))
        cl_error(CL_ERR_TYPE, "%MKDIR: argument must be a string");
    str = (CL_String *)CL_OBJ_TO_PTR(args[0]);
    return (platform_mkdir(str->data) == 0) ? CL_T : CL_NIL;
}

/* (file-namestring pathname) => string */
static CL_Obj bi_file_namestring(CL_Obj *args, int n)
{
    CL_String *str;
    const char *data;
    uint32_t len, i, last_sep;
    CL_UNUSED(n);
    if (!CL_ANY_STRING_P(args[0]))
        cl_error(CL_ERR_TYPE, "FILE-NAMESTRING: argument must be a string");
    str = (CL_String *)CL_OBJ_TO_PTR(args[0]);
    data = str->data;
    len = str->length;
    last_sep = 0;
    for (i = 0; i < len; i++) {
        if (data[i] == '/' || data[i] == ':')
            last_sep = i + 1;
    }
    return cl_make_string(data + last_sep, len - last_sep);
}

/* (directory-namestring pathname) => string */
static CL_Obj bi_directory_namestring(CL_Obj *args, int n)
{
    CL_String *str;
    const char *data;
    uint32_t len, i, last_sep;
    CL_UNUSED(n);
    if (!CL_ANY_STRING_P(args[0]))
        cl_error(CL_ERR_TYPE, "DIRECTORY-NAMESTRING: argument must be a string");
    str = (CL_String *)CL_OBJ_TO_PTR(args[0]);
    data = str->data;
    len = str->length;
    last_sep = 0;
    for (i = 0; i < len; i++) {
        if (data[i] == '/' || data[i] == ':')
            last_sep = i + 1;
    }
    return cl_make_string(data, last_sep);
}

/* --- Readtable helpers --- */

/* Resolve readtable argument: fixnum index or NIL (= current) */
static int resolve_readtable_idx(CL_Obj *args, int n, int arg_idx)
{
    if (arg_idx < n && !CL_NULL_P(args[arg_idx])) {
        if (CL_FIXNUM_P(args[arg_idx])) {
            int idx = CL_FIXNUM_VAL(args[arg_idx]);
            if (idx >= 0 && idx < CL_RT_POOL_SIZE)
                return idx;
        }
        cl_error(CL_ERR_TYPE, "Not a valid readtable");
    }
    /* Default: current readtable */
    {
        CL_Obj val = cl_symbol_value(SYM_STAR_READTABLE);
        if (CL_FIXNUM_P(val)) {
            int idx = CL_FIXNUM_VAL(val);
            if (idx >= 0 && idx < CL_RT_POOL_SIZE)
                return idx;
        }
    }
    return 1; /* fallback */
}

/* (readtablep obj) => T or NIL */
static CL_Obj bi_readtablep(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    if (CL_FIXNUM_P(args[0])) {
        int idx = CL_FIXNUM_VAL(args[0]);
        if (idx >= 0 && idx < CL_RT_POOL_SIZE &&
            (cl_readtable_alloc_mask & (1u << idx)))
            return CL_T;
    }
    return CL_NIL;
}

/* Callable wrapper for the built-in #\" string reader, returned by
 * GET-MACRO-CHARACTER so libraries that grab and re-dispatch the standard
 * string reader (e.g. pythonic-string-reader) get a real function instead of
 * NIL.  Registered as AMILISP's %READ-STANDARD-STRING; kept here so
 * GET-MACRO-CHARACTER can hand it back.  GC-rooted in cl_builtins_stream_init. */
static CL_Obj standard_string_reader_fn = CL_NIL;

/* (%read-standard-string stream char) => string
 * Reads a "..." string from STREAM with the opening quote already consumed,
 * matching the standard macro-character protocol (the CHAR arg is ignored). */
static CL_Obj bi_read_standard_string(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    return cl_read_standard_string_from_stream(args[0]);
}

/* (get-macro-character char &optional readtable) => fn, non-terminating-p */
static CL_Obj bi_get_macro_character(CL_Obj *args, int n)
{
    int ch, rt_idx;
    CL_Readtable *rt;

    if (!CL_CHAR_P(args[0]))
        cl_error(CL_ERR_TYPE, "GET-MACRO-CHARACTER: first argument must be a character");
    ch = CL_CHAR_VAL(args[0]);
    rt_idx = resolve_readtable_idx(args, n, 1);
    rt = cl_readtable_get(rt_idx);

    if (ch >= 0 && ch < CL_RT_CHARS &&
        (rt->syntax[ch] == CL_CHAR_TERM_MACRO ||
         rt->syntax[ch] == CL_CHAR_NONTERM_MACRO)) {
        /* Return 2 values: function, non-terminating-p.  Built-in macro chars
         * are read inline in C, so macro_fn[ch] is NIL — but a caller that
         * grabs the standard reader expects a callable (pythonic-string-reader
         * funcalls the result).  Hand back the %READ-STANDARD-STRING wrapper
         * for #\" so that protocol works. */
        CL_Obj fn = rt->macro_fn[ch];
        if (CL_NULL_P(fn) && ch == '"')
            fn = standard_string_reader_fn;
        cl_mv_count = 2;
        cl_mv_values[0] = fn;
        cl_mv_values[1] = (rt->syntax[ch] == CL_CHAR_NONTERM_MACRO) ? CL_T : CL_NIL;
        return fn;
    }
    cl_mv_count = 2;
    cl_mv_values[0] = CL_NIL;
    cl_mv_values[1] = CL_NIL;
    return CL_NIL;
}

/* (set-macro-character char fn &optional non-terminating-p readtable) => T */
static CL_Obj bi_set_macro_character(CL_Obj *args, int n)
{
    int ch, rt_idx;
    CL_Readtable *rt;
    int non_term = 0;

    if (!CL_CHAR_P(args[0]))
        cl_error(CL_ERR_TYPE, "SET-MACRO-CHARACTER: first argument must be a character");
    ch = CL_CHAR_VAL(args[0]);
    if (ch < 0 || ch >= CL_RT_CHARS)
        cl_error(CL_ERR_TYPE, "SET-MACRO-CHARACTER: character out of range");

    /* args[1] = function */
    if (n > 2 && !CL_NULL_P(args[2]))
        non_term = 1;
    rt_idx = resolve_readtable_idx(args, n, 3);
    rt = cl_readtable_get(rt_idx);

    if (CL_MT()) platform_rwlock_wrlock(cl_readtable_rwlock);
    rt->syntax[ch] = non_term ? CL_CHAR_NONTERM_MACRO : CL_CHAR_TERM_MACRO;
    rt->macro_fn[ch] = args[1];
    if (CL_MT()) platform_rwlock_unlock(cl_readtable_rwlock);
    return CL_T;
}

/* (make-dispatch-macro-character char &optional non-terminating-p readtable) => T */
static CL_Obj bi_make_dispatch_macro_character(CL_Obj *args, int n)
{
    int ch, rt_idx;
    CL_Readtable *rt;
    int non_term = 0;

    if (!CL_CHAR_P(args[0]))
        cl_error(CL_ERR_TYPE, "MAKE-DISPATCH-MACRO-CHARACTER: first argument must be a character");
    ch = CL_CHAR_VAL(args[0]);
    if (ch < 0 || ch >= CL_RT_CHARS)
        cl_error(CL_ERR_TYPE, "MAKE-DISPATCH-MACRO-CHARACTER: character out of range");

    if (n > 1 && !CL_NULL_P(args[1]))
        non_term = 1;
    rt_idx = resolve_readtable_idx(args, n, 2);
    rt = cl_readtable_get(rt_idx);

    if (CL_MT()) platform_rwlock_wrlock(cl_readtable_rwlock);
    rt->syntax[ch] = non_term ? CL_CHAR_NONTERM_MACRO : CL_CHAR_TERM_MACRO;
    rt->macro_fn[ch] = CL_NIL; /* built-in dispatch handling */
    if (CL_MT()) platform_rwlock_unlock(cl_readtable_rwlock);
    return CL_T;
}

/* (set-dispatch-macro-character disp-char sub-char fn &optional readtable) => T */
static CL_Obj bi_set_dispatch_macro_character(CL_Obj *args, int n)
{
    int sub_ch, rt_idx;
    CL_Readtable *rt;

    if (!CL_CHAR_P(args[0]))
        cl_error(CL_ERR_TYPE, "SET-DISPATCH-MACRO-CHARACTER: disp-char must be a character");
    if (!CL_CHAR_P(args[1]))
        cl_error(CL_ERR_TYPE, "SET-DISPATCH-MACRO-CHARACTER: sub-char must be a character");
    sub_ch = CL_CHAR_VAL(args[1]);
    if (sub_ch < 0 || sub_ch >= CL_RT_CHARS)
        cl_error(CL_ERR_TYPE, "SET-DISPATCH-MACRO-CHARACTER: sub-char out of range");

    rt_idx = resolve_readtable_idx(args, n, 3);
    rt = cl_readtable_get(rt_idx);

    if (CL_MT()) platform_rwlock_wrlock(cl_readtable_rwlock);
    rt->dispatch_fn[sub_ch] = args[2]; /* fn */
    if (CL_MT()) platform_rwlock_unlock(cl_readtable_rwlock);
    return CL_T;
}

/* (get-dispatch-macro-character disp-char sub-char &optional readtable) => fn */
static CL_Obj bi_get_dispatch_macro_character(CL_Obj *args, int n)
{
    int sub_ch, rt_idx;
    CL_Readtable *rt;

    int disp_ch;

    if (!CL_CHAR_P(args[0]))
        cl_error(CL_ERR_TYPE, "GET-DISPATCH-MACRO-CHARACTER: disp-char must be a character");
    if (!CL_CHAR_P(args[1]))
        cl_error(CL_ERR_TYPE, "GET-DISPATCH-MACRO-CHARACTER: sub-char must be a character");
    disp_ch = CL_CHAR_VAL(args[0]);
    sub_ch = CL_CHAR_VAL(args[1]);

    rt_idx = resolve_readtable_idx(args, n, 2);
    rt = cl_readtable_get(rt_idx);

    /* CLHS get-dispatch-macro-character: signal an error if disp-char is not a
     * dispatching macro character in the readtable.  In amilisp a dispatching
     * macro character has macro syntax but NO plain macro-function installed —
     * it routes through the built-in dispatch path (e.g. #).  A character with
     * a macro-function (set via set-macro-character, e.g. a custom backquote)
     * is an ordinary macro character, not a dispatching one.  Libraries such as
     * named-readtables rely on this error to tell the two apart when merging
     * readtables; returning NIL instead silently misclassifies ordinary macro
     * characters as dispatching ones. */
    if (disp_ch < 0 || disp_ch >= CL_RT_CHARS ||
        (rt->syntax[disp_ch] != CL_CHAR_TERM_MACRO &&
         rt->syntax[disp_ch] != CL_CHAR_NONTERM_MACRO) ||
        !CL_NULL_P(rt->macro_fn[disp_ch]))
        cl_error(CL_ERR_TYPE,
                 "GET-DISPATCH-MACRO-CHARACTER: %c is not a dispatching macro character",
                 (disp_ch >= 32 && disp_ch < 127) ? disp_ch : '?');

    if (sub_ch < 0 || sub_ch >= CL_RT_CHARS)
        return CL_NIL;
    return rt->dispatch_fn[sub_ch];
}

/* (copy-readtable &optional from-readtable to-readtable) => readtable */
static CL_Obj bi_copy_readtable(CL_Obj *args, int n)
{
    int from_idx, to_idx, result;

    /* from: default = current readtable */
    from_idx = resolve_readtable_idx(args, n, 0);

    /* to: NIL = allocate new, or fixnum index */
    if (n > 1 && !CL_NULL_P(args[1])) {
        if (!CL_FIXNUM_P(args[1]))
            cl_error(CL_ERR_TYPE, "COPY-READTABLE: to argument must be a readtable or NIL");
        to_idx = CL_FIXNUM_VAL(args[1]);
    } else {
        to_idx = -1; /* allocate new */
    }

    result = cl_readtable_copy(from_idx, to_idx);
    if (result < 0)
        cl_error(CL_ERR_TYPE, "COPY-READTABLE: no free readtable slots");
    return CL_MAKE_FIXNUM(result);
}

/* (readtable-case &optional readtable) => keyword */
static CL_Obj bi_readtable_case(CL_Obj *args, int n)
{
    int rt_idx = resolve_readtable_idx(args, n, 0);
    CL_Readtable *rt = cl_readtable_get(rt_idx);
    switch (rt->case_mode) {
        case CL_RT_CASE_UPCASE:   return cl_intern_keyword("UPCASE", 6);
        case CL_RT_CASE_DOWNCASE: return cl_intern_keyword("DOWNCASE", 8);
        case CL_RT_CASE_PRESERVE: return cl_intern_keyword("PRESERVE", 8);
        case CL_RT_CASE_INVERT:   return cl_intern_keyword("INVERT", 6);
        default:                  return cl_intern_keyword("UPCASE", 6);
    }
}

/* (%setf-readtable-case readtable mode) => mode
 * defsetf passes place args first, then new value last */
static CL_Obj bi_setf_readtable_case(CL_Obj *args, int n)
{
    CL_Obj mode = args[1];
    int rt_idx;
    CL_Readtable *rt;
    const char *name;

    (void)n;
    rt_idx = resolve_readtable_idx(args, n, 0);
    rt = cl_readtable_get(rt_idx);

    name = cl_symbol_name(mode);
    if (!name)
        cl_error(CL_ERR_TYPE, "READTABLE-CASE: mode must be a keyword");

    if (strcmp(name, "UPCASE") == 0)
        rt->case_mode = CL_RT_CASE_UPCASE;
    else if (strcmp(name, "DOWNCASE") == 0)
        rt->case_mode = CL_RT_CASE_DOWNCASE;
    else if (strcmp(name, "PRESERVE") == 0)
        rt->case_mode = CL_RT_CASE_PRESERVE;
    else if (strcmp(name, "INVERT") == 0)
        rt->case_mode = CL_RT_CASE_INVERT;
    else
        cl_error(CL_ERR_TYPE, "READTABLE-CASE: invalid mode, expected :UPCASE, :DOWNCASE, :PRESERVE, or :INVERT");

    return mode;
}

/* (set-syntax-from-char to-char from-char &optional to-readtable from-readtable) => T */
static CL_Obj bi_set_syntax_from_char(CL_Obj *args, int n)
{
    int to_ch, from_ch, to_idx, from_idx = 0;
    CL_Readtable *to_rt, *from_rt;

    if (!CL_CHAR_P(args[0]))
        cl_error(CL_ERR_TYPE, "SET-SYNTAX-FROM-CHAR: to-char must be a character");
    if (!CL_CHAR_P(args[1]))
        cl_error(CL_ERR_TYPE, "SET-SYNTAX-FROM-CHAR: from-char must be a character");

    to_ch = CL_CHAR_VAL(args[0]);
    from_ch = CL_CHAR_VAL(args[1]);

    if (to_ch < 0 || to_ch >= CL_RT_CHARS)
        cl_error(CL_ERR_TYPE, "SET-SYNTAX-FROM-CHAR: to-char out of range");
    if (from_ch < 0 || from_ch >= CL_RT_CHARS)
        cl_error(CL_ERR_TYPE, "SET-SYNTAX-FROM-CHAR: from-char out of range");

    /* to-readtable: NIL = current readtable */
    to_idx = resolve_readtable_idx(args, n, 2);

    /* from-readtable: NIL = standard readtable (slot 0) per CL spec */
    if (n > 3 && !CL_NULL_P(args[3])) {
        if (CL_FIXNUM_P(args[3])) {
            from_idx = CL_FIXNUM_VAL(args[3]);
            if (from_idx < 0 || from_idx >= CL_RT_POOL_SIZE)
                cl_error(CL_ERR_TYPE, "SET-SYNTAX-FROM-CHAR: invalid from-readtable");
        } else {
            cl_error(CL_ERR_TYPE, "SET-SYNTAX-FROM-CHAR: from-readtable must be a readtable or NIL");
        }
    } else {
        from_idx = 0; /* standard readtable */
    }

    to_rt = cl_readtable_get(to_idx);
    from_rt = cl_readtable_get(from_idx);

    if (CL_MT()) platform_rwlock_wrlock(cl_readtable_rwlock);
    /* Copy syntax type */
    to_rt->syntax[to_ch] = from_rt->syntax[from_ch];

    /* Copy reader macro function */
    to_rt->macro_fn[to_ch] = from_rt->macro_fn[from_ch];
    if (CL_MT()) platform_rwlock_unlock(cl_readtable_rwlock);

    return CL_T;
}

/* ======================================================= */
/* Stream attribute functions                              */
/* ======================================================= */

/* Pre-interned :DEFAULT keyword for stream-external-format */
static CL_Obj KW_STREAM_DEFAULT = CL_NIL;

/* (dribble &optional pathname) => NIL
 * No-op: amilisp has no dribble facility. */
static CL_Obj bi_dribble(CL_Obj *args, int n)
{
    CL_UNUSED(args); CL_UNUSED(n);
    return CL_NIL;
}

/* (short-site-name) => NIL */
static CL_Obj bi_short_site_name(CL_Obj *args, int n)
{
    CL_UNUSED(args); CL_UNUSED(n);
    return CL_NIL;
}

/* (long-site-name) => NIL */
static CL_Obj bi_long_site_name(CL_Obj *args, int n)
{
    CL_UNUSED(args); CL_UNUSED(n);
    return CL_NIL;
}

/* (stream-element-type stream) => type-specifier
 * Socket streams are binary (UNSIGNED-BYTE 8); all others are CHARACTER. */
static CL_Obj bi_stream_element_type(CL_Obj *args, int n)
{
    CL_Stream *st;
    CL_UNUSED(n);
    if (!CL_STREAM_P(args[0]))
        cl_error(CL_ERR_TYPE, "STREAM-ELEMENT-TYPE: not a stream");
    st = (CL_Stream *)CL_OBJ_TO_PTR(args[0]);
    /* A two-way / echo stream reports its input component's element type:
     * an echo stream over binary file streams must answer (UNSIGNED-BYTE 8)
     * so that code choosing READ-BYTE vs READ-CHAR by element type works
     * through the wrapper (ansi-test make-echo-stream.4/.9). */
    while ((st->stream_type == CL_STREAM_TWO_WAY || st->stream_type == CL_STREAM_ECHO) &&
           CL_STREAM_P(st->string_buf))
        st = (CL_Stream *)CL_OBJ_TO_PTR(st->string_buf);
    /* A file stream's explicitly recorded element-type (from OPEN
     * :element-type '(unsigned-byte 8)) wins — this is how a binary file
     * stream reports itself.  Only FILE streams store a type here; for
     * TWO-WAY/ECHO/SYNONYM streams the element_type slot holds a child stream. */
    if (st->stream_type == CL_STREAM_FILE && !CL_NULL_P(st->element_type))
        return st->element_type;
    if (st->stream_type == CL_STREAM_SOCKET) {
        /* Return (UNSIGNED-BYTE 8) — split cons calls to prevent the inner
         * result from living only in a register when the outer cons runs. */
        CL_Obj ub = cl_intern_in("UNSIGNED-BYTE", 13, cl_package_cl);
        CL_Obj inner;
        CL_GC_PROTECT(ub);
        inner = cl_cons(CL_MAKE_FIXNUM(8), CL_NIL);
        CL_GC_PROTECT(inner);
        {
            CL_Obj r = cl_cons(ub, inner);
            CL_GC_UNPROTECT(2);
            return r;
        }
    }
    return cl_intern_in("CHARACTER", 9, cl_package_cl);
}

/* (stream-external-format stream) => format
 * amilisp uses UTF-8 for character streams (reported as :DEFAULT); a stream
 * opened with :external-format :latin-1 reports :LATIN-1. */
static CL_Obj bi_stream_external_format(CL_Obj *args, int n)
{
    CL_Stream *st;
    CL_UNUSED(n);
    if (!CL_STREAM_P(args[0]))
        cl_error(CL_ERR_TYPE, "STREAM-EXTERNAL-FORMAT: not a stream");
    st = (CL_Stream *)CL_OBJ_TO_PTR(args[0]);
    if (st->flags & CL_STREAM_FLAG_LATIN1)
        return KW_LATIN_1;
    return KW_STREAM_DEFAULT;
}

/* (listen &optional input-stream) => boolean
 * Returns T if a character is immediately available from the stream. */
static CL_Obj bi_listen(CL_Obj *args, int n)
{
    CL_Obj stream = cl_resolve_input_stream(args, n, 0);
    CL_Stream *st;
    /* Unwrap a synonym-/two-way-/echo-stream designator (e.g. *TERMINAL-IO*)
     * to the concrete input-side stream so unread_char/flags/stream_type
     * below are read from the object that actually carries that state — a
     * synonym/two-way wrapper's own fields are never touched by
     * READ-CHAR/UNREAD-CHAR, and an echo wrapper's only matter when it holds
     * a pushed-back char (probe_input_backing stops there). */
    stream = probe_input_backing(stream);
    if (!CL_STREAM_P(stream)) return CL_NIL;
    st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
    /* A concatenated stream listens on its current component.  Popping an
     * exhausted head is the read path's job, so when the head is at EOF but
     * a later component has data this stays conservatively NIL. */
    while (st->stream_type == CL_STREAM_CONCATENATED) {
        if (st->unread_char != -1) return CL_T;
        if (st->flags & CL_STREAM_FLAG_EOF) return CL_NIL;
        if (!CL_CONS_P(st->string_buf)) return CL_NIL;
        stream = probe_input_backing(cl_car(st->string_buf));
        if (!CL_STREAM_P(stream)) return CL_NIL;
        st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
    }
    if (st->unread_char != -1) return CL_T;
    if (st->flags & CL_STREAM_FLAG_EOF) return CL_NIL;
    if (st->stream_type == CL_STREAM_STRING || st->stream_type == CL_STREAM_CBUF)
        return (st->position < st->out_buf_len) ? CL_T : CL_NIL;
    if (st->stream_type == CL_STREAM_SOCKET) {
        /* Non-blocking readiness probe.  Returns T only when a character is
         * immediately available; NIL at end of file or when a read would block
         * — exactly the CLHS LISTEN contract.  For a listener stream "data
         * available" means a client connection is pending, which is what the
         * usocket AmiLisp backend's wait-for-input uses to drive
         * hunchentoot's accept loop. */
        /* Capture the handle BEFORE the probe and re-derive st AFTER it:
         * on AmigaOS the probe goes through the socket reactor's safe
         * region, so a peer's compaction can relocate the stream while we
         * wait — the EOF-flag RMW below must not go through a stale st. */
        uint32_t handle = st->handle_id;
        int r;
        CL_GC_PROTECT(stream);
        r = platform_socket_data_available((PlatformSocket)handle);
        st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
        CL_GC_UNPROTECT(1);
        if (r == 2)                       /* EOF: peer closed */
            st->flags |= CL_STREAM_FLAG_EOF;
        return (r == 1) ? CL_T : CL_NIL;
    }
    if (st->stream_type == CL_STREAM_CONSOLE) {
        /* Exact while EXT:TTY-RAW-MODE is active (raw console reads bypass
         * the C library's input buffering); best-effort conservative in
         * cooked mode.  This is what lets a TUI's input loop poll the
         * console without blocking. */
        return platform_tty_char_avail() ? CL_T : CL_NIL;
    }
    return CL_NIL;  /* conservative: unknown availability */
}

/* (clear-input &optional input-stream) => nil */
static CL_Obj bi_clear_input(CL_Obj *args, int n)
{
    CL_UNUSED(args); CL_UNUSED(n);
    return CL_NIL;
}

/* (read-char-no-hang &optional stream eof-error-p eof-value recursive-p) => char or nil
 * Returns a character if one is available, NIL if not without blocking,
 * or signals/returns eof-value at end of file. */
static CL_Obj bi_read_char_no_hang(CL_Obj *args, int n)
{
    CL_Obj stream = cl_resolve_input_stream(args, n, 0);
    int eof_error_p = (n < 2 || !CL_NULL_P(args[1]));
    CL_Obj eof_value = (n >= 3) ? args[2] : CL_NIL;
    /* Unwrap to the concrete input-side stream for the availability check —
     * same reasoning as bi_listen: unread_char/flags/stream_type live on the
     * backing stream, not on a synonym-/two-way-/echo-stream wrapper like
     * *TERMINAL-IO*.  `backing` is CL_NIL for a designator that fails to
     * resolve (e.g. a synonym whose symbol is unbound); leave `available`
     * false in that case and fall through to cl_stream_read_char(stream)
     * below, which resolves (and fails) the same way cl_stream_read_char
     * always has, keeping error/EOF behavior unchanged for that edge case. */
    CL_Obj backing = probe_input_backing(stream);
    CL_Stream *st = CL_STREAM_P(backing) ? (CL_Stream *)CL_OBJ_TO_PTR(backing) : NULL;
    int available;
    int in_memory;

    /* Check if a character is immediately available */
    in_memory = st && (st->stream_type == CL_STREAM_STRING || st->stream_type == CL_STREAM_CBUF);
    available = st && ((st->unread_char != -1) ||
                (!(st->flags & CL_STREAM_FLAG_EOF) && in_memory &&
                 st->position < st->out_buf_len));
    /* Console availability mirrors LISTEN: exact in raw mode, conservative
     * in cooked mode (see bi_listen). */
    if (!available && st &&
        st->stream_type == CL_STREAM_CONSOLE &&
        !(st->flags & CL_STREAM_FLAG_EOF) &&
        platform_tty_char_avail())
        available = 1;

    if (!available) {
        /* Check for EOF.  An in-memory stream can never block, so "nothing
         * available" IS end of file there — its read path never sets the
         * EOF flag, and answering NIL instead of eof-value made
         * (read-char-no-hang s nil :eof) on an exhausted string stream
         * indistinguishable from "no input yet". */
        if (st && ((st->flags & CL_STREAM_FLAG_EOF) || in_memory)) {
            if (eof_error_p)
                cl_error(CL_ERR_EOF, "READ-CHAR-NO-HANG: end of file");
            return eof_value;
        }
        return CL_NIL;
    }

    {
        int ch = cl_stream_read_char(stream);
        if (ch == -1) {
            if (eof_error_p)
                cl_error(CL_ERR_EOF, "READ-CHAR-NO-HANG: end of file");
            /* Re-read from rooted args[] (see bi_read_char). */
            return (n >= 3) ? args[2] : CL_NIL;
        }
        return CL_MAKE_CHAR(ch);
    }
}

/* (host-namestring pathname) => nil
 * amilisp has no networking pathname host component. */
static CL_Obj bi_host_namestring(CL_Obj *args, int n)
{
    CL_UNUSED(args); CL_UNUSED(n);
    return CL_NIL;
}

/* (file-author pathname) => nil
 * amilisp does not track file authorship. */
static CL_Obj bi_file_author(CL_Obj *args, int n)
{
    CL_UNUSED(args); CL_UNUSED(n);
    return CL_NIL;
}

/* (file-string-length stream object) => length or nil
 * Returns the number of octets that would be used to encode object on stream.
 * amilisp uses UTF-8; approximate for strings, exact for characters. */
static CL_Obj bi_file_string_length(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    if (!CL_STREAM_P(args[0]))
        cl_error(CL_ERR_TYPE, "FILE-STRING-LENGTH: first argument is not a stream");
    if (CL_CHAR_P(args[1])) {
        int ch = CL_CHAR_VAL(args[1]);
        int32_t len = (ch < 0x80) ? 1 : (ch < 0x800) ? 2 : (ch < 0x10000) ? 3 : 4;
        return CL_MAKE_FIXNUM(len);
    }
    if (CL_ANY_STRING_P(args[1])) {
        /* Sum UTF-8 byte lengths for each character */
        uint32_t i, slen = cl_string_length(args[1]);
        int32_t total = 0;
        for (i = 0; i < slen; i++) {
            int ch = cl_string_char_at(args[1], i);
            total += (ch < 0x80) ? 1 : (ch < 0x800) ? 2 : (ch < 0x10000) ? 3 : 4;
        }
        return CL_MAKE_FIXNUM(total);
    }
    return CL_NIL;
}

/* --- Registration --- */

void cl_builtins_stream_init(void)
{
    /* Register the cached-symbol roots BEFORE the interns below: each
     * cl_intern_keyword can allocate and compact, and a value assigned to a
     * not-yet-registered slot would go stale before its registration at the
     * end of init (ST9 — same shape as the fixed *READTABLE* boot bug;
     * registering a still-NIL slot is safe, the GC walkers skip NIL). */
    cl_gc_register_root(&GRAY_FUND_INPUT_SYM);
    cl_gc_register_root(&GRAY_FUND_OUTPUT_SYM);
    cl_gc_register_root(&KW_START);
    cl_gc_register_root(&KW_END);
    cl_gc_register_root(&KW_ABORT_KW);
    cl_gc_register_root(&KW_DIRECTION);
    cl_gc_register_root(&KW_INPUT);
    cl_gc_register_root(&KW_OUTPUT);
    cl_gc_register_root(&KW_IO);
    cl_gc_register_root(&KW_PROBE);
    cl_gc_register_root(&KW_IF_EXISTS);
    cl_gc_register_root(&KW_IF_DOES_NOT_EXIST);
    cl_gc_register_root(&KW_SUPERSEDE);
    cl_gc_register_root(&KW_APPEND);
    cl_gc_register_root(&KW_ERROR_KW);
    cl_gc_register_root(&KW_CREATE);
    cl_gc_register_root(&KW_NEW_VERSION);
    cl_gc_register_root(&KW_RENAME);
    cl_gc_register_root(&KW_RENAME_AND_DELETE);
    cl_gc_register_root(&KW_OVERWRITE);
    cl_gc_register_root(&KW_ELEMENT_TYPE_OPEN);
    cl_gc_register_root(&KW_EXTERNAL_FORMAT);
    cl_gc_register_root(&KW_LATIN_1);
    cl_gc_register_root(&KW_STREAM_DEFAULT);
    cl_gc_register_root(&standard_string_reader_fn);

    /* Pre-intern keyword symbols */
    KW_START   = cl_intern_keyword("START", 5);
    KW_END     = cl_intern_keyword("END", 3);
    KW_ABORT_KW = cl_intern_keyword("ABORT", 5);
    KW_DIRECTION = cl_intern_keyword("DIRECTION", 9);
    KW_INPUT   = cl_intern_keyword("INPUT", 5);
    KW_OUTPUT  = cl_intern_keyword("OUTPUT", 6);
    KW_IO      = cl_intern_keyword("IO", 2);
    KW_PROBE   = cl_intern_keyword("PROBE", 5);
    KW_IF_EXISTS = cl_intern_keyword("IF-EXISTS", 9);
    KW_IF_DOES_NOT_EXIST = cl_intern_keyword("IF-DOES-NOT-EXIST", 17);
    KW_SUPERSEDE = cl_intern_keyword("SUPERSEDE", 9);
    KW_APPEND  = cl_intern_keyword("APPEND", 6);
    KW_ERROR_KW = cl_intern_keyword("ERROR", 5);
    KW_CREATE  = cl_intern_keyword("CREATE", 6);
    KW_NEW_VERSION = cl_intern_keyword("NEW-VERSION", 11);
    KW_RENAME  = cl_intern_keyword("RENAME", 6);
    KW_RENAME_AND_DELETE = cl_intern_keyword("RENAME-AND-DELETE", 17);
    KW_OVERWRITE = cl_intern_keyword("OVERWRITE", 9);
    KW_ELEMENT_TYPE_OPEN = cl_intern_keyword("ELEMENT-TYPE", 12);
    KW_EXTERNAL_FORMAT = cl_intern_keyword("EXTERNAL-FORMAT", 15);
    KW_LATIN_1 = cl_intern_keyword("LATIN-1", 7);
    KW_STREAM_DEFAULT = cl_intern_keyword("DEFAULT", 7);

    /* Step 1 */
    defun("STREAMP", bi_streamp, 1, 1);
    defun("INPUT-STREAM-P", bi_input_stream_p, 1, 1);
    defun("OUTPUT-STREAM-P", bi_output_stream_p, 1, 1);
    defun("INTERACTIVE-STREAM-P", bi_interactive_stream_p, 1, 1);
    cl_register_builtin("%MAKE-TEST-STREAM", bi_make_test_stream, 2, 2, cl_package_amilisp);

    /* Step 3: Character I/O */
    defun("READ-CHAR", bi_read_char, 0, 4);
    defun("WRITE-CHAR", bi_write_char, 1, 2);
    defun("READ-BYTE", bi_read_byte, 1, 3);
    defun("WRITE-BYTE", bi_write_byte, 2, 2);
    defun("PEEK-CHAR", bi_peek_char, 0, 5);
    defun("UNREAD-CHAR", bi_unread_char, 1, 2);
    defun("READ-LINE", bi_read_line, 0, 4);
    defun("WRITE-STRING", bi_write_string, 1, -1);
    defun("WRITE-LINE", bi_write_line, 1, -1);
    defun("TERPRI", bi_terpri, 0, 1);
    defun("FRESH-LINE", bi_fresh_line, 0, 1);
    defun("FINISH-OUTPUT", bi_finish_output, 0, 1);
    defun("FORCE-OUTPUT", bi_force_output, 0, 1);
    defun("CLEAR-OUTPUT", bi_clear_output, 0, 1);
    defun("OPEN-STREAM-P", bi_open_stream_p, 1, 1);
    defun("CLOSE", bi_close, 1, -1);
    defun("MAKE-STRING-INPUT-STREAM", bi_make_string_input_stream, 1, 3);
    defun("MAKE-STRING-OUTPUT-STREAM", bi_make_string_output_stream, 0, -1);
    defun("GET-OUTPUT-STREAM-STRING", bi_get_output_stream_string, 1, 1);
    defun("MAKE-SYNONYM-STREAM", bi_make_synonym_stream, 1, 1);
    defun("SYNONYM-STREAM-SYMBOL", bi_synonym_stream_symbol, 1, 1);
    defun("MAKE-TWO-WAY-STREAM", bi_make_two_way_stream, 2, 2);
    defun("TWO-WAY-STREAM-INPUT-STREAM", bi_two_way_stream_input_stream, 1, 1);
    defun("TWO-WAY-STREAM-OUTPUT-STREAM", bi_two_way_stream_output_stream, 1, 1);
    defun("MAKE-BROADCAST-STREAM", bi_make_broadcast_stream, 0, -1);
    defun("BROADCAST-STREAM-STREAMS", bi_broadcast_stream_streams, 1, 1);
    defun("MAKE-CONCATENATED-STREAM", bi_make_concatenated_stream, 0, -1);
    defun("CONCATENATED-STREAM-STREAMS", bi_concatenated_stream_streams, 1, 1);
    defun("MAKE-ECHO-STREAM", bi_make_echo_stream, 2, 2);
    defun("ECHO-STREAM-INPUT-STREAM", bi_echo_stream_input_stream, 1, 1);
    defun("ECHO-STREAM-OUTPUT-STREAM", bi_echo_stream_output_stream, 1, 1);

    /* Step 8: File streams */
    defun("OPEN", bi_open, 1, -1);
    defun("FILE-POSITION", bi_file_position, 1, 2);
    defun("FILE-LENGTH", bi_file_length, 1, 1);

    /* Step 10: Time and file system */
    defun("GET-UNIVERSAL-TIME", bi_get_universal_time, 0, 0);
    defun("TRUENAME", bi_truename, 1, 1);
    defun("PROBE-FILE", bi_probe_file, 1, 1);
    defun("DELETE-FILE", bi_delete_file, 1, 1);
    defun("RENAME-FILE", bi_rename_file, 2, 2);
    defun("FILE-WRITE-DATE", bi_file_write_date, 1, 1);
    cl_register_builtin("%MKDIR", bi_mkdir, 1, 1, cl_package_amilisp);
    defun("DIRECTORY", bi_directory, 1, -1);
    defun("FILE-NAMESTRING", bi_file_namestring, 1, 1);
    defun("DIRECTORY-NAMESTRING", bi_directory_namestring, 1, 1);

    /* Stream attribute functions */
    defun("STREAM-ELEMENT-TYPE", bi_stream_element_type, 1, 1);
    defun("STREAM-EXTERNAL-FORMAT", bi_stream_external_format, 1, 1);
    defun("LISTEN", bi_listen, 0, 1);
    defun("CLEAR-INPUT", bi_clear_input, 0, 1);
    defun("READ-CHAR-NO-HANG", bi_read_char_no_hang, 0, 4);
    defun("HOST-NAMESTRING", bi_host_namestring, 1, 1);
    defun("FILE-AUTHOR", bi_file_author, 1, 1);
    defun("FILE-STRING-LENGTH", bi_file_string_length, 2, 2);
    defun("DRIBBLE", bi_dribble, 0, 1);
    defun("SHORT-SITE-NAME", bi_short_site_name, 0, 0);
    defun("LONG-SITE-NAME", bi_long_site_name, 0, 0);

    /* Step 12: Readtable */
    defun("READTABLEP", bi_readtablep, 1, 1);
    defun("GET-MACRO-CHARACTER", bi_get_macro_character, 1, 2);
    /* Callable backing GET-MACRO-CHARACTER's result for the built-in #\".
     * Registered through cl_register_builtin (not a bare cl_make_function)
     * so the image-relink registry can resolve it by name after a restore. */
    {
        CL_Obj ssr_sym;
        cl_register_builtin("%READ-STANDARD-STRING", bi_read_standard_string,
                            2, 2, cl_package_amilisp);
        ssr_sym = cl_intern_in("%READ-STANDARD-STRING",
                               (uint32_t)strlen("%READ-STANDARD-STRING"),
                               cl_package_amilisp);
        standard_string_reader_fn =
            ((CL_Symbol *)CL_OBJ_TO_PTR(ssr_sym))->function;
    }
    defun("SET-MACRO-CHARACTER", bi_set_macro_character, 2, 4);
    defun("MAKE-DISPATCH-MACRO-CHARACTER", bi_make_dispatch_macro_character, 1, 3);
    defun("SET-DISPATCH-MACRO-CHARACTER", bi_set_dispatch_macro_character, 3, 4);
    defun("GET-DISPATCH-MACRO-CHARACTER", bi_get_dispatch_macro_character, 2, 3);
    defun("COPY-READTABLE", bi_copy_readtable, 0, 2);
    defun("SET-SYNTAX-FROM-CHAR", bi_set_syntax_from_char, 2, 4);
    defun("READTABLE-CASE", bi_readtable_case, 0, 1);
    cl_register_builtin("%SETF-READTABLE-CASE", bi_setf_readtable_case, 2, 2, cl_package_amilisp);

}
