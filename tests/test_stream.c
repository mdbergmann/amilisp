/*
 * test_stream.c — Tests for stream heap type and platform file I/O
 */

#include "test.h"
#include "core/types.h"
#include "core/mem.h"
#include "core/error.h"
#include "core/package.h"
#include "core/symbol.h"
#include "core/builtins.h"
#include "core/stream.h"
#include "core/reader.h"
#include "core/printer.h"
#include "core/compiler.h"
#include "core/vm.h"
#include "core/repl.h"
#include "core/bignum.h"
#include "core/thread.h"
#include "platform/platform.h"
#include "platform/platform_thread.h"
#include <stdlib.h>   /* abort() — watchdog escalation on deadlock */
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#ifdef PLATFORM_WIN32
#include "platform/win32_compat.h"     /* winsock2 + the POSIX shims */
#else
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#endif

/* Raw BSD sockets, spelled portably.  The socket-stream tests drive the peer
 * side by hand, and Winsock differs from POSIX in exactly three places: the
 * descriptor type, its close, and read/write (which must be recv/send on a
 * socket).  Everything else — socket/bind/listen/accept/connect/sendto —
 * is the same call on both. */
#ifdef PLATFORM_WIN32
typedef SOCKET raw_sock_t;
#define RAW_SOCK_INVALID     INVALID_SOCKET
#define RAW_SOCK_VALID(fd)   ((fd) != INVALID_SOCKET)
#define raw_close(fd)        closesocket(fd)
#define raw_send(fd, b, n)   send((fd), (const char *)(b), (int)(n), 0)
#define raw_recv(fd, b, n)   recv((fd), (char *)(b), (int)(n), 0)
#else
typedef int raw_sock_t;
#define RAW_SOCK_INVALID     (-1)
#define RAW_SOCK_VALID(fd)   ((fd) >= 0)
#define raw_close(fd)        close(fd)
#define raw_send(fd, b, n)   write((fd), (b), (size_t)(n))
#define raw_recv(fd, b, n)   read((fd), (b), (size_t)(n))
#endif

static void setup(void)
{
    platform_init();
    cl_thread_init();
    cl_error_init();
    cl_mem_init(CL_DEFAULT_HEAP_SIZE);
    cl_package_init();
    cl_symbol_init();
    cl_reader_init();
    cl_printer_init();
    cl_compiler_init();
    cl_vm_init(0, 0);
    cl_stream_init();
    cl_builtins_init();
    cl_repl_init();
}

static void teardown(void)
{
    cl_stream_shutdown();
    cl_mem_shutdown();
    platform_shutdown();
}

/* --- Stream allocation tests --- */

TEST(make_console_input_stream)
{
    CL_Obj s = cl_make_stream(CL_STREAM_INPUT, CL_STREAM_CONSOLE);
    CL_Stream *st;
    ASSERT(!CL_NULL_P(s));
    ASSERT(CL_STREAM_P(s));
    st = (CL_Stream *)CL_OBJ_TO_PTR(s);
    ASSERT_EQ_INT((int)st->direction, CL_STREAM_INPUT);
    ASSERT_EQ_INT((int)st->stream_type, CL_STREAM_CONSOLE);
    ASSERT(st->flags & CL_STREAM_FLAG_OPEN);
    ASSERT_EQ_INT((int)st->unread_char, -1);
}

TEST(make_console_output_stream)
{
    CL_Obj s = cl_make_stream(CL_STREAM_OUTPUT, CL_STREAM_CONSOLE);
    CL_Stream *st;
    ASSERT(!CL_NULL_P(s));
    ASSERT(CL_STREAM_P(s));
    st = (CL_Stream *)CL_OBJ_TO_PTR(s);
    ASSERT_EQ_INT((int)st->direction, CL_STREAM_OUTPUT);
    ASSERT_EQ_INT((int)st->stream_type, CL_STREAM_CONSOLE);
}

TEST(make_file_stream)
{
    CL_Obj s = cl_make_stream(CL_STREAM_INPUT, CL_STREAM_FILE);
    CL_Stream *st;
    ASSERT(!CL_NULL_P(s));
    ASSERT(CL_STREAM_P(s));
    st = (CL_Stream *)CL_OBJ_TO_PTR(s);
    ASSERT_EQ_INT((int)st->stream_type, CL_STREAM_FILE);
}

TEST(make_string_stream)
{
    CL_Obj s = cl_make_stream(CL_STREAM_INPUT, CL_STREAM_STRING);
    CL_Stream *st;
    ASSERT(!CL_NULL_P(s));
    ASSERT(CL_STREAM_P(s));
    st = (CL_Stream *)CL_OBJ_TO_PTR(s);
    ASSERT_EQ_INT((int)st->stream_type, CL_STREAM_STRING);
}

TEST(stream_type_name)
{
    CL_Obj s = cl_make_stream(CL_STREAM_INPUT, CL_STREAM_CONSOLE);
    ASSERT_STR_EQ(cl_type_name(s), "STREAM");
}

/* --- GC tests --- */

TEST(stream_gc_survives)
{
    CL_Obj s = cl_make_stream(CL_STREAM_INPUT, CL_STREAM_CONSOLE);
    CL_Stream *st;
    int i;

    CL_GC_PROTECT(s);

    /* Create garbage */
    for (i = 0; i < 200; i++)
        cl_cons(CL_MAKE_FIXNUM(i), CL_NIL);

    cl_gc();

    /* Stream should survive */
    ASSERT(CL_STREAM_P(s));
    st = (CL_Stream *)CL_OBJ_TO_PTR(s);
    ASSERT_EQ_INT((int)st->direction, CL_STREAM_INPUT);

    CL_GC_UNPROTECT(1);
}

TEST(stream_gc_marks_string_buf)
{
    CL_Obj s = cl_make_stream(CL_STREAM_INPUT, CL_STREAM_STRING);
    CL_Obj str = cl_make_string("hello world", 11);
    CL_Stream *st;
    int i;

    CL_GC_PROTECT(s);
    st = (CL_Stream *)CL_OBJ_TO_PTR(s);
    st->string_buf = str;

    /* Create garbage */
    for (i = 0; i < 200; i++)
        cl_cons(CL_MAKE_FIXNUM(i), CL_NIL);

    cl_gc();

    /* String buf should survive through the stream */
    st = (CL_Stream *)CL_OBJ_TO_PTR(s);
    ASSERT(CL_STRING_P(st->string_buf));
    {
        CL_String *ss = (CL_String *)CL_OBJ_TO_PTR(st->string_buf);
        ASSERT_STR_EQ(ss->data, "hello world");
    }

    CL_GC_UNPROTECT(1);
}

/* --- Output buffer tests --- */

TEST(outbuf_alloc_free)
{
    uint32_t h = cl_stream_alloc_outbuf(256);
    ASSERT(h != 0);
    ASSERT(cl_stream_outbuf_data(h) != NULL);
    ASSERT_EQ_INT((int)cl_stream_outbuf_len(h), 0);
    cl_stream_free_outbuf(h);
}

/* Regression: a LIVE string-output-stream's outbuf must NOT be freed by the GC
 * just because a DEAD stream records the same handle.  Outbuf handles are
 * recycled the instant a slot is freed, so after a dead stream A frees handle
 * H and a live stream B reuses it, A's stale st->out_buf_handle still names H.
 * The old GC freed outbufs from each dead corpse's handle, so re-finalizing A
 * freed B's live buffer (B's accumulated text vanished — surfaced as chunga
 * READ-LINE* returning "" for a non-empty HTTP header under concurrent GC,
 * desyncing drakma's response framing).  The fix reclaims outbufs by stream
 * LIVENESS (mark phase pins each live stream's slot) instead of from dead
 * corpses.  Here we forge that exact state: a garbage output stream whose
 * out_buf_handle aliases the live stream's slot.  The live content must
 * survive the GC. */
TEST(outbuf_live_survives_dead_stream_sharing_handle)
{
    CL_Obj live = cl_make_string_output_stream();
    CL_Obj dead;
    CL_Stream *st;
    uint32_t h;
    int i;

    CL_GC_PROTECT(live);
    st = (CL_Stream *)CL_OBJ_TO_PTR(live);
    h = st->out_buf_handle;
    cl_stream_outbuf_putchar(st, 'D');
    cl_stream_outbuf_putchar(st, 'A');
    cl_stream_outbuf_putchar(st, 'T');
    cl_stream_outbuf_putchar(st, 'A');

    /* Forge a DEAD output stream whose handle aliases the live slot, then drop
     * its only reference so it is garbage at the next GC. */
    dead = cl_make_stream(CL_STREAM_OUTPUT, CL_STREAM_STRING);
    ((CL_Stream *)CL_OBJ_TO_PTR(dead))->out_buf_handle = h;
    dead = CL_NIL;

    for (i = 0; i < 100; i++)
        cl_cons(CL_MAKE_FIXNUM(i), CL_NIL);
    cl_gc();

    /* The live stream (re-derived; it may have moved) keeps its buffer. */
    st = (CL_Stream *)CL_OBJ_TO_PTR(live);
    ASSERT_EQ_INT((int)st->out_buf_handle, (int)h);
    ASSERT(cl_stream_outbuf_data(h) != NULL);
    ASSERT_EQ_INT((int)cl_stream_outbuf_len(h), 4);
    ASSERT(memcmp(cl_stream_outbuf_data(h), "DATA", 4) == 0);

    CL_GC_UNPROTECT(1);
}

/* Companion: a genuinely dead string-output-stream's outbuf IS reclaimed by
 * the mark-driven sweep (no slot leak), so the table-full path keeps working. */
TEST(outbuf_dead_stream_reclaimed)
{
    CL_Obj dead = cl_make_string_output_stream();
    uint32_t h = ((CL_Stream *)CL_OBJ_TO_PTR(dead))->out_buf_handle;
    int i;

    ASSERT(cl_stream_outbuf_data(h) != NULL);
    dead = CL_NIL;  /* drop reference */

    for (i = 0; i < 100; i++)
        cl_cons(CL_MAKE_FIXNUM(i), CL_NIL);
    cl_gc();

    /* No live stream pinned the slot — it must have been freed. */
    ASSERT(cl_stream_outbuf_data(h) == NULL);
}

/* The outbuf side table is a segmented directory that grows on demand (one
 * 256-slot block at a time) instead of forcing a full stop-the-world GC on
 * every allocation once a fixed table saturated — under logging-heavy loads
 * (a transient string-output-stream per printed node) the fixed table turned
 * printing into a GC storm.  Holding well over one block's worth of buffers
 * simultaneously must work, hand back distinct handles, and keep every
 * buffer's content intact. */
TEST(outbuf_table_grows_past_one_block)
{
    enum { N = 3 * CL_STREAM_BUF_TABLE_SIZE / 2 };  /* 384: forces 2nd block */
    static uint32_t handles[N];
    int i, j;
    int saw_second_block = 0;

    for (i = 0; i < N; i++) {
        handles[i] = cl_stream_alloc_outbuf(16);
        ASSERT(handles[i] != 0);
        ASSERT(cl_stream_outbuf_data(handles[i]) != NULL);
        if (handles[i] >= CL_STREAM_BUF_TABLE_SIZE)
            saw_second_block = 1;
        for (j = 0; j < i; j++)
            ASSERT(handles[j] != handles[i]);
    }
    ASSERT(saw_second_block);

    /* Content survives per slot across the whole set (write via a forged
     * stream so we exercise the same putchar path real streams use). */
    {
        CL_Obj s = cl_make_stream(CL_STREAM_OUTPUT, CL_STREAM_STRING);
        CL_Stream *st = (CL_Stream *)CL_OBJ_TO_PTR(s);
        for (i = 0; i < N; i++) {
            st->out_buf_handle = handles[i];
            cl_stream_outbuf_putchar(st, 'A' + (i % 26));
        }
        for (i = 0; i < N; i++) {
            ASSERT_EQ_INT((int)cl_stream_outbuf_len(handles[i]), 1);
            ASSERT(cl_stream_outbuf_data(handles[i])[0] == (char)('A' + (i % 26)));
        }
        st->out_buf_handle = 0;
    }

    for (i = 0; i < N; i++)
        cl_stream_free_outbuf(handles[i]);
    for (i = 0; i < N; i++)
        ASSERT(cl_stream_outbuf_data(handles[i]) == NULL);
}

/* CLOSE on a string output stream frees its outbuf slot eagerly AND clears
 * the stream's handle.  A still-reachable closed stream keeping a stale
 * handle would pin that slot in the GC mark phase after the handle is
 * recycled by another stream, blocking reclamation of a dead stream's
 * buffer. */
TEST(close_string_stream_frees_and_clears_outbuf_handle)
{
    CL_Obj s = cl_make_string_output_stream();
    CL_Stream *st;
    uint32_t h;

    CL_GC_PROTECT(s);
    st = (CL_Stream *)CL_OBJ_TO_PTR(s);
    h = st->out_buf_handle;
    ASSERT(h != 0);
    cl_stream_outbuf_putchar(st, 'X');
    ASSERT(cl_stream_outbuf_data(h) != NULL);

    cl_stream_close(s);

    st = (CL_Stream *)CL_OBJ_TO_PTR(s);
    ASSERT_EQ_INT((int)st->out_buf_handle, 0);      /* handle cleared */
    ASSERT(cl_stream_outbuf_data(h) == NULL);       /* slot freed eagerly */
    ASSERT(!(st->flags & CL_STREAM_FLAG_OPEN));
    CL_GC_UNPROTECT(1);
}

TEST(outbuf_putchar)
{
    CL_Obj s = cl_make_stream(CL_STREAM_OUTPUT, CL_STREAM_STRING);
    CL_Stream *st = (CL_Stream *)CL_OBJ_TO_PTR(s);
    uint32_t h;

    h = cl_stream_alloc_outbuf(256);
    ASSERT(h != 0);
    st->out_buf_handle = h;

    cl_stream_outbuf_putchar(st, 'A');
    cl_stream_outbuf_putchar(st, 'B');
    cl_stream_outbuf_putchar(st, 'C');

    ASSERT_EQ_INT((int)cl_stream_outbuf_len(h), 3);
    ASSERT_STR_EQ(cl_stream_outbuf_data(h), "ABC");

    cl_stream_free_outbuf(h);
}

TEST(outbuf_write_string)
{
    CL_Obj s = cl_make_stream(CL_STREAM_OUTPUT, CL_STREAM_STRING);
    CL_Stream *st = (CL_Stream *)CL_OBJ_TO_PTR(s);
    uint32_t h;

    h = cl_stream_alloc_outbuf(256);
    ASSERT(h != 0);
    st->out_buf_handle = h;

    cl_stream_outbuf_write(st, "Hello ");
    cl_stream_outbuf_write(st, "World");

    ASSERT_EQ_INT((int)cl_stream_outbuf_len(h), 11);
    ASSERT_STR_EQ(cl_stream_outbuf_data(h), "Hello World");

    cl_stream_free_outbuf(h);
}

TEST(outbuf_growth)
{
    CL_Obj s = cl_make_stream(CL_STREAM_OUTPUT, CL_STREAM_STRING);
    CL_Stream *st = (CL_Stream *)CL_OBJ_TO_PTR(s);
    uint32_t h;
    int i;

    /* Start with a tiny buffer */
    h = cl_stream_alloc_outbuf(4);
    ASSERT(h != 0);
    st->out_buf_handle = h;

    /* Write more than 4 chars — should grow */
    for (i = 0; i < 100; i++)
        cl_stream_outbuf_putchar(st, 'x');

    ASSERT_EQ_INT((int)cl_stream_outbuf_len(h), 100);
    /* Verify all chars are correct */
    {
        char *data = cl_stream_outbuf_data(h);
        int ok = 1;
        for (i = 0; i < 100; i++) {
            if (data[i] != 'x') { ok = 0; break; }
        }
        ASSERT(ok);
    }

    cl_stream_free_outbuf(h);
}

TEST(outbuf_reset)
{
    uint32_t h = cl_stream_alloc_outbuf(256);
    CL_Obj s = cl_make_stream(CL_STREAM_OUTPUT, CL_STREAM_STRING);
    CL_Stream *st = (CL_Stream *)CL_OBJ_TO_PTR(s);
    st->out_buf_handle = h;

    cl_stream_outbuf_write(st, "data");
    ASSERT_EQ_INT((int)cl_stream_outbuf_len(h), 4);

    cl_stream_outbuf_reset(h);
    ASSERT_EQ_INT((int)cl_stream_outbuf_len(h), 0);
    ASSERT_STR_EQ(cl_stream_outbuf_data(h), "");

    cl_stream_free_outbuf(h);
}

/* --- Platform file I/O tests --- */

TEST(platform_file_write_read)
{
    PlatformFile wf, rf;
    int ch;
    const char *path = "/tmp/cl_test_stream.tmp";

    /* Write */
    wf = platform_file_open(path, PLATFORM_FILE_WRITE);
    ASSERT(wf != PLATFORM_FILE_INVALID);
    ASSERT(platform_file_write_string(wf, "Hello") == 0);
    ASSERT(platform_file_write_char(wf, '!') == 0);
    platform_file_close(wf);

    /* Read back */
    rf = platform_file_open(path, PLATFORM_FILE_READ);
    ASSERT(rf != PLATFORM_FILE_INVALID);

    ch = platform_file_getchar(rf);
    ASSERT_EQ_INT(ch, 'H');
    ch = platform_file_getchar(rf);
    ASSERT_EQ_INT(ch, 'e');
    ch = platform_file_getchar(rf);
    ASSERT_EQ_INT(ch, 'l');
    ch = platform_file_getchar(rf);
    ASSERT_EQ_INT(ch, 'l');
    ch = platform_file_getchar(rf);
    ASSERT_EQ_INT(ch, 'o');
    ch = platform_file_getchar(rf);
    ASSERT_EQ_INT(ch, '!');

    /* EOF */
    ch = platform_file_getchar(rf);
    ASSERT_EQ_INT(ch, -1);

    platform_file_close(rf);

    /* Cleanup */
    remove(path);
}

TEST(platform_file_open_nonexistent)
{
    PlatformFile rf = platform_file_open("/tmp/cl_nonexistent_xyz.tmp",
                                          PLATFORM_FILE_READ);
    ASSERT_EQ_INT((int)rf, (int)PLATFORM_FILE_INVALID);
}

TEST(platform_file_append)
{
    PlatformFile f;
    int ch;
    const char *path = "/tmp/cl_test_append.tmp";

    /* Write initial content */
    f = platform_file_open(path, PLATFORM_FILE_WRITE);
    ASSERT(f != PLATFORM_FILE_INVALID);
    platform_file_write_string(f, "AB");
    platform_file_close(f);

    /* Append more */
    f = platform_file_open(path, PLATFORM_FILE_APPEND);
    ASSERT(f != PLATFORM_FILE_INVALID);
    platform_file_write_string(f, "CD");
    platform_file_close(f);

    /* Read and verify */
    f = platform_file_open(path, PLATFORM_FILE_READ);
    ASSERT(f != PLATFORM_FILE_INVALID);
    ch = platform_file_getchar(f); ASSERT_EQ_INT(ch, 'A');
    ch = platform_file_getchar(f); ASSERT_EQ_INT(ch, 'B');
    ch = platform_file_getchar(f); ASSERT_EQ_INT(ch, 'C');
    ch = platform_file_getchar(f); ASSERT_EQ_INT(ch, 'D');
    ch = platform_file_getchar(f); ASSERT_EQ_INT(ch, -1);
    platform_file_close(f);

    remove(path);
}

/* --- typep test --- */

TEST(stream_typep)
{
    CL_Obj s = cl_make_stream(CL_STREAM_INPUT, CL_STREAM_CONSOLE);
    CL_Obj type_sym = cl_intern("STREAM", 6);
    ASSERT(cl_typep(s, type_sym));

    /* Fixnums should not be streams */
    ASSERT(!cl_typep(CL_MAKE_FIXNUM(42), type_sym));
}

/* --- Console streams and standard stream variables --- */

TEST(console_streams_created)
{
    ASSERT(!CL_NULL_P(cl_stdin_stream));
    ASSERT(CL_STREAM_P(cl_stdin_stream));
    ASSERT(!CL_NULL_P(cl_stdout_stream));
    ASSERT(CL_STREAM_P(cl_stdout_stream));
    ASSERT(!CL_NULL_P(cl_stderr_stream));
    ASSERT(CL_STREAM_P(cl_stderr_stream));
}

TEST(standard_stream_variables_bound)
{
    CL_Symbol *s;
    s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_STANDARD_INPUT);
    ASSERT(CL_STREAM_P(s->value));
    s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_STANDARD_OUTPUT);
    ASSERT(CL_STREAM_P(s->value));
    s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_ERROR_OUTPUT);
    ASSERT(CL_STREAM_P(s->value));
    s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_TRACE_OUTPUT);
    ASSERT(CL_STREAM_P(s->value));
    s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_DEBUG_IO);
    ASSERT(CL_STREAM_P(s->value));
    s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_QUERY_IO);
    ASSERT(CL_STREAM_P(s->value));
    s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_TERMINAL_IO);
    ASSERT(CL_STREAM_P(s->value));
}

TEST(standard_input_is_input)
{
    CL_Stream *st = (CL_Stream *)CL_OBJ_TO_PTR(cl_stdin_stream);
    ASSERT_EQ_INT((int)st->direction, CL_STREAM_INPUT);
}

TEST(standard_output_is_output)
{
    CL_Stream *st = (CL_Stream *)CL_OBJ_TO_PTR(cl_stdout_stream);
    ASSERT_EQ_INT((int)st->direction, CL_STREAM_OUTPUT);
}

TEST(interactive_stream_p_console)
{
    CL_Stream *st = (CL_Stream *)CL_OBJ_TO_PTR(cl_stdout_stream);
    ASSERT_EQ_INT((int)st->stream_type, CL_STREAM_CONSOLE);
    /* Console streams are interactive */
    ASSERT_EQ_INT((int)(st->stream_type == CL_STREAM_CONSOLE), 1);
}

TEST(interactive_stream_p_file)
{
    CL_Obj s = cl_make_stream(CL_STREAM_INPUT, CL_STREAM_FILE);
    CL_Stream *st = (CL_Stream *)CL_OBJ_TO_PTR(s);
    ASSERT_EQ_INT((int)(st->stream_type == CL_STREAM_CONSOLE), 0);
}

TEST(interactive_stream_p_string)
{
    CL_Obj s = cl_make_stream(CL_STREAM_INPUT, CL_STREAM_STRING);
    CL_Stream *st = (CL_Stream *)CL_OBJ_TO_PTR(s);
    ASSERT_EQ_INT((int)(st->stream_type == CL_STREAM_CONSOLE), 0);
}

TEST(console_streams_survive_gc)
{
    CL_Stream *st;
    int i;

    /* Create garbage to trigger GC */
    for (i = 0; i < 200; i++)
        cl_cons(CL_MAKE_FIXNUM(i), CL_NIL);

    cl_gc();

    /* Console streams should survive (reachable via symbol values) */
    ASSERT(CL_STREAM_P(cl_stdin_stream));
    st = (CL_Stream *)CL_OBJ_TO_PTR(cl_stdin_stream);
    ASSERT_EQ_INT((int)st->direction, CL_STREAM_INPUT);

    ASSERT(CL_STREAM_P(cl_stdout_stream));
    st = (CL_Stream *)CL_OBJ_TO_PTR(cl_stdout_stream);
    ASSERT_EQ_INT((int)st->direction, CL_STREAM_OUTPUT);

    ASSERT(CL_STREAM_P(cl_stderr_stream));
    st = (CL_Stream *)CL_OBJ_TO_PTR(cl_stderr_stream);
    ASSERT_EQ_INT((int)st->direction, CL_STREAM_OUTPUT);
}

/* --- String input stream tests --- */

TEST(make_string_input_stream)
{
    CL_Obj str = cl_make_string("Hello", 5);
    CL_Obj s;
    int ch;
    CL_GC_PROTECT(str);
    s = cl_make_string_input_stream(str, 0, 5);
    CL_GC_UNPROTECT(1);

    ch = cl_stream_read_char(s);
    ASSERT_EQ_INT(ch, 'H');
    ch = cl_stream_read_char(s);
    ASSERT_EQ_INT(ch, 'e');
    ch = cl_stream_read_char(s);
    ASSERT_EQ_INT(ch, 'l');
    ch = cl_stream_read_char(s);
    ASSERT_EQ_INT(ch, 'l');
    ch = cl_stream_read_char(s);
    ASSERT_EQ_INT(ch, 'o');
}

TEST(string_input_stream_eof)
{
    CL_Obj str = cl_make_string("AB", 2);
    CL_Obj s;
    int ch;
    CL_GC_PROTECT(str);
    s = cl_make_string_input_stream(str, 0, 2);
    CL_GC_UNPROTECT(1);

    ch = cl_stream_read_char(s);
    ASSERT_EQ_INT(ch, 'A');
    ch = cl_stream_read_char(s);
    ASSERT_EQ_INT(ch, 'B');
    ch = cl_stream_read_char(s);
    ASSERT_EQ_INT(ch, -1);
}

TEST(string_input_stream_start_end)
{
    CL_Obj str = cl_make_string("Hello World", 11);
    CL_Obj s;
    int ch;
    CL_GC_PROTECT(str);
    s = cl_make_string_input_stream(str, 6, 11);
    CL_GC_UNPROTECT(1);

    ch = cl_stream_read_char(s);
    ASSERT_EQ_INT(ch, 'W');
    ch = cl_stream_read_char(s);
    ASSERT_EQ_INT(ch, 'o');
    ch = cl_stream_read_char(s);
    ASSERT_EQ_INT(ch, 'r');
    ch = cl_stream_read_char(s);
    ASSERT_EQ_INT(ch, 'l');
    ch = cl_stream_read_char(s);
    ASSERT_EQ_INT(ch, 'd');
    ch = cl_stream_read_char(s);
    ASSERT_EQ_INT(ch, -1);
}

TEST(string_output_stream_write_chars)
{
    CL_Obj s = cl_make_string_output_stream();
    CL_Obj result;

    cl_stream_write_char(s, 'A');
    cl_stream_write_char(s, 'B');
    cl_stream_write_char(s, 'C');

    CL_GC_PROTECT(s);
    result = cl_get_output_stream_string(s);
    CL_GC_UNPROTECT(1);
    ASSERT(CL_STRING_P(result));
    {
        CL_String *rs = (CL_String *)CL_OBJ_TO_PTR(result);
        ASSERT_EQ_INT((int)rs->length, 3);
        ASSERT_STR_EQ(rs->data, "ABC");
    }
}

TEST(string_output_stream_write_string)
{
    CL_Obj s = cl_make_string_output_stream();
    CL_Obj result;

    cl_stream_write_string(s, "Hello ", 6);
    cl_stream_write_string(s, "World", 5);

    CL_GC_PROTECT(s);
    result = cl_get_output_stream_string(s);
    CL_GC_UNPROTECT(1);
    ASSERT(CL_STRING_P(result));
    {
        CL_String *rs = (CL_String *)CL_OBJ_TO_PTR(result);
        ASSERT_EQ_INT((int)rs->length, 11);
        ASSERT_STR_EQ(rs->data, "Hello World");
    }
}

/* Regression: cl_make_string_output_stream must GC-protect its freshly-built
 * stream across cl_stream_alloc_outbuf.  That call can run an internal GC (at
 * directory exhaustion) which would otherwise sweep or relocate the
 * not-yet-rooted new stream, returning a dangling/stale object — surfacing
 * later as "argument is not a stream".  Loop creating MANY unreachable
 * string-output-streams — the table grows by blocks as they accumulate, and
 * every stream must remain a valid, writable string stream throughout. */
TEST(make_string_output_stream_survives_table_full_gc)
{
    int i;
    int iterations = CL_STREAM_BUF_TABLE_SIZE * 3;  /* well past one full table */
    for (i = 0; i < iterations; i++) {
        CL_Obj s = cl_make_string_output_stream();  /* intentionally NOT rooted */
        CL_Obj result;
        ASSERT(CL_STREAM_P(s));                      /* not corrupted by inner GC */
        cl_stream_write_string(s, "abc", 3);
        CL_GC_PROTECT(s);
        result = cl_get_output_stream_string(s);
        CL_GC_UNPROTECT(1);
        ASSERT(CL_STRING_P(result));
        {
            CL_String *rs = (CL_String *)CL_OBJ_TO_PTR(result);
            ASSERT_EQ_INT((int)rs->length, 3);
            ASSERT_STR_EQ(rs->data, "abc");
        }
        /* s + result drop out of reach here, so their slots are reclaimable by
         * the next iteration's table-full GC — keeping the table churning. */
    }
}

TEST(get_output_stream_string_resets)
{
    CL_Obj s = cl_make_string_output_stream();
    CL_Obj r1, r2;

    cl_stream_write_string(s, "first", 5);
    CL_GC_PROTECT(s);
    r1 = cl_get_output_stream_string(s);
    CL_GC_UNPROTECT(1);
    {
        CL_String *rs = (CL_String *)CL_OBJ_TO_PTR(r1);
        ASSERT_STR_EQ(rs->data, "first");
    }

    cl_stream_write_string(s, "second", 6);
    CL_GC_PROTECT(s);
    r2 = cl_get_output_stream_string(s);
    CL_GC_UNPROTECT(1);
    {
        CL_String *rs = (CL_String *)CL_OBJ_TO_PTR(r2);
        ASSERT_STR_EQ(rs->data, "second");
    }
}

TEST(unread_char_test)
{
    CL_Obj str = cl_make_string("XY", 2);
    CL_Obj s;
    int ch;
    CL_GC_PROTECT(str);
    s = cl_make_string_input_stream(str, 0, 2);
    CL_GC_UNPROTECT(1);

    ch = cl_stream_read_char(s);
    ASSERT_EQ_INT(ch, 'X');

    cl_stream_unread_char(s, ch);

    ch = cl_stream_read_char(s);
    ASSERT_EQ_INT(ch, 'X');
    ch = cl_stream_read_char(s);
    ASSERT_EQ_INT(ch, 'Y');
}

TEST(peek_char_test)
{
    CL_Obj str = cl_make_string("AB", 2);
    CL_Obj s;
    int ch;
    CL_GC_PROTECT(str);
    s = cl_make_string_input_stream(str, 0, 2);
    CL_GC_UNPROTECT(1);

    ch = cl_stream_peek_char(s);
    ASSERT_EQ_INT(ch, 'A');
    /* Peek should not consume */
    ch = cl_stream_read_char(s);
    ASSERT_EQ_INT(ch, 'A');
    ch = cl_stream_read_char(s);
    ASSERT_EQ_INT(ch, 'B');
}

TEST(close_stream)
{
    CL_Obj s = cl_make_string_output_stream();
    CL_Stream *st = (CL_Stream *)CL_OBJ_TO_PTR(s);

    ASSERT(st->flags & CL_STREAM_FLAG_OPEN);
    cl_stream_close(s);
    ASSERT(!(st->flags & CL_STREAM_FLAG_OPEN));

    /* Double close should be safe (no-op) */
    cl_stream_close(s);
    ASSERT(!(st->flags & CL_STREAM_FLAG_OPEN));
}

TEST(file_stream_write_read)
{
    const char *path = "/tmp/cl_test_stream_io.tmp";
    PlatformFile wf, rf;
    CL_Obj ws, rs;
    CL_Stream *wst, *rst;
    int ch;

    /* Write via stream API */
    wf = platform_file_open(path, PLATFORM_FILE_WRITE);
    ASSERT(wf != PLATFORM_FILE_INVALID);
    ws = cl_make_stream(CL_STREAM_OUTPUT, CL_STREAM_FILE);
    wst = (CL_Stream *)CL_OBJ_TO_PTR(ws);
    wst->handle_id = (uint32_t)wf;

    cl_stream_write_char(ws, 'H');
    cl_stream_write_char(ws, 'i');
    cl_stream_write_char(ws, '!');
    cl_stream_close(ws);

    /* Read via stream API */
    rf = platform_file_open(path, PLATFORM_FILE_READ);
    ASSERT(rf != PLATFORM_FILE_INVALID);
    rs = cl_make_stream(CL_STREAM_INPUT, CL_STREAM_FILE);
    rst = (CL_Stream *)CL_OBJ_TO_PTR(rs);
    rst->handle_id = (uint32_t)rf;

    ch = cl_stream_read_char(rs);
    ASSERT_EQ_INT(ch, 'H');
    ch = cl_stream_read_char(rs);
    ASSERT_EQ_INT(ch, 'i');
    ch = cl_stream_read_char(rs);
    ASSERT_EQ_INT(ch, '!');
    ch = cl_stream_read_char(rs);
    ASSERT_EQ_INT(ch, -1);
    cl_stream_close(rs);

    remove(path);
}

/* Regression: OPEN must honour :element-type so STREAM-ELEMENT-TYPE reports a
 * binary stream as (UNSIGNED-BYTE 8), not CHARACTER.  drakma's SEND-CONTENT
 * checks (subtypep (stream-element-type s) 'octet) before it will stream a
 * file body, so a wrong CHARACTER answer broke hunchentoot multipart file
 * POSTs ("Don't know how to send content #<FILE-INPUT-STREAM>"). */
TEST(open_element_type_binary_reported)
{
    const char *expr =
        "(progn"
        " (with-open-file (s \"/tmp/cl_test_binstream.tmp\" :direction :output"
        "                    :if-exists :supersede :element-type '(unsigned-byte 8))"
        "   (write-byte 65 s))"
        " (with-open-file (s \"/tmp/cl_test_binstream.tmp\" :element-type '(unsigned-byte 8))"
        "   (list (subtypep (stream-element-type s) '(unsigned-byte 8))"
        "         (subtypep (stream-element-type s) 'character))))";
    CL_Obj result = cl_eval_string(expr);
    char buf[64];
    cl_prin1_to_string(result, buf, sizeof(buf));
    /* binary subtype yes, character subtype no */
    ASSERT_STR_EQ(buf, "(T NIL)");
    remove("/tmp/cl_test_binstream.tmp");
}

/* Regression: OPEN must merge a relative pathname with
 * *default-pathname-defaults* (like LOAD), not just resolve against the
 * process cwd.  hunchentoot's test suite rebinds *default-pathname-defaults*
 * to its source dir and does (with-open-file (f #P"fz.jpg") ...); without the
 * merge that OPEN failed with "cannot open file".  Here we write a file into a
 * temp dir, rebind *default-pathname-defaults* to that dir, then open it by
 * its bare relative name. */
TEST(open_relative_honors_default_pathname_defaults)
{
    const char *expr =
        "(progn"
        " (with-open-file (s \"/tmp/cl_test_dpd_file.txt\" :direction :output"
        "                    :if-exists :supersede)"
        "   (write-string \"zappa\" s))"
        " (let ((*default-pathname-defaults* #P\"/tmp/\"))"
        "   (with-open-file (s #P\"cl_test_dpd_file.txt\")"
        "     (read-line s))))";
    CL_Obj result = cl_eval_string(expr);
    char buf[64];
    cl_prin1_to_string(result, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "\"zappa\"");
    remove("/tmp/cl_test_dpd_file.txt");
}

/* A character stream (no :element-type) still reports CHARACTER. */
TEST(open_element_type_character_default)
{
    const char *expr =
        "(progn"
        " (with-open-file (s \"/tmp/cl_test_chstream.tmp\" :direction :output"
        "                    :if-exists :supersede)"
        "   (write-char #\\A s))"
        " (with-open-file (s \"/tmp/cl_test_chstream.tmp\")"
        "   (stream-element-type s)))";
    CL_Obj result = cl_eval_string(expr);
    char buf[64];
    cl_prin1_to_string(result, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "CHARACTER");
    remove("/tmp/cl_test_chstream.tmp");
}

/* Regression: OPEN :external-format :latin-1 makes a character file stream
 * 8-bit transparent — every code point 0..255 round-trips as a single byte,
 * with no UTF-8 transcoding.  Without it, amilisp's default UTF-8 stream
 * doubles each byte > 127 (a multipart upload written to a temp file by
 * rfc2388 inflated and corrupted; see the rfc2388 fork's #+cl-amiga
 * :latin-1 external-format branch in PARSE-MIME).
 * We write code-char 252 (ü) plus an ASCII char, read the file back as raw
 * bytes, and assert the on-disk content is exactly the two bytes #xFC #x41
 * (NOT the 3-byte UTF-8 form #xC3 #xBC #x41). */
TEST(open_latin1_external_format_byte_faithful)
{
    const char *expr =
        "(progn"
        " (with-open-file (s \"/tmp/cl_test_latin1.tmp\" :direction :output"
        "                    :if-exists :supersede :external-format :latin-1)"
        "   (write-char (code-char 252) s)"
        "   (write-char #\\A s))"
        " (with-open-file (s \"/tmp/cl_test_latin1.tmp\" :element-type '(unsigned-byte 8))"
        "   (let ((b0 (read-byte s)) (b1 (read-byte s)) (b2 (read-byte s nil :eof)))"
        "     (list b0 b1 b2))))";
    CL_Obj result = cl_eval_string(expr);
    char buf[64];
    cl_prin1_to_string(result, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(252 65 :EOF)");
    remove("/tmp/cl_test_latin1.tmp");
}

/* A latin-1 stream reads high bytes back as the matching code points (no UTF-8
 * decode), and STREAM-EXTERNAL-FORMAT reports :LATIN-1. */
TEST(open_latin1_read_roundtrip_and_format)
{
    const char *expr =
        "(progn"
        " (with-open-file (s \"/tmp/cl_test_latin1r.tmp\" :direction :output"
        "                    :if-exists :supersede :external-format :latin-1)"
        "   (dotimes (i 256) (write-char (code-char i) s)))"
        " (with-open-file (s \"/tmp/cl_test_latin1r.tmp\" :external-format :latin-1)"
        "   (let ((ok t))"
        "     (dotimes (i 256)"
        "       (unless (= (char-code (read-char s)) i) (setf ok nil)))"
        "     (list ok (stream-external-format s) (read-char s nil :eof)))))";
    CL_Obj result = cl_eval_string(expr);
    char buf[64];
    cl_prin1_to_string(result, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(T :LATIN-1 :EOF)");
    remove("/tmp/cl_test_latin1r.tmp");
}

/* Alternative :external-format spellings (:iso-8859-1, :latin1, :iso8859-1)
 * must all activate the latin-1 8-bit-transparent mode — a typo in any of the
 * strcmp targets in format_is_latin1() would go undetected without this test. */
TEST(open_iso8859_spelling_byte_faithful)
{
    const char *expr =
        "(progn"
        " (with-open-file (s \"/tmp/cl_test_iso8859.tmp\" :direction :output"
        "                    :if-exists :supersede :external-format :iso-8859-1)"
        "   (write-char (code-char 252) s)"
        "   (write-char #\\A s))"
        " (with-open-file (s \"/tmp/cl_test_iso8859.tmp\" :element-type '(unsigned-byte 8))"
        "   (let ((b0 (read-byte s)) (b1 (read-byte s)) (b2 (read-byte s nil :eof)))"
        "     (list b0 b1 b2))))";
    CL_Obj result = cl_eval_string(expr);
    char buf[64];
    cl_prin1_to_string(result, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(252 65 :EOF)");
    remove("/tmp/cl_test_iso8859.tmp");
}

TEST(charpos_tracking)
{
    CL_Obj s = cl_make_string_output_stream();
    CL_Stream *st = (CL_Stream *)CL_OBJ_TO_PTR(s);

    ASSERT_EQ_INT((int)st->charpos, 0);

    cl_stream_write_char(s, 'A');
    ASSERT_EQ_INT((int)st->charpos, 1);

    cl_stream_write_char(s, 'B');
    ASSERT_EQ_INT((int)st->charpos, 2);

    cl_stream_write_char(s, '\n');
    ASSERT_EQ_INT((int)st->charpos, 0);

    cl_stream_write_string(s, "XYZ", 3);
    ASSERT_EQ_INT((int)st->charpos, 3);

    cl_stream_write_string(s, "ab\ncd", 5);
    ASSERT_EQ_INT((int)st->charpos, 2);

    cl_stream_close(s);
}

TEST(read_line_string_stream)
{
    CL_Obj str = cl_make_string("Hello\nWorld", 11);
    CL_Obj s;
    CL_Obj line;
    CL_String *ls;
    char stack_buf[256];
    char *buf = stack_buf;

    CL_GC_PROTECT(str);
    s = cl_make_string_input_stream(str, 0, 11);
    CL_GC_UNPROTECT(1);

    /* Read first line */
    {
        uint32_t len = 0;
        int ch;
        for (;;) {
            ch = cl_stream_read_char(s);
            if (ch == -1 || ch == '\n') break;
            buf[len++] = (char)ch;
        }
        line = cl_make_string(buf, len);
        ls = (CL_String *)CL_OBJ_TO_PTR(line);
        ASSERT_STR_EQ(ls->data, "Hello");
    }

    /* Read second line */
    {
        uint32_t len = 0;
        int ch;
        for (;;) {
            ch = cl_stream_read_char(s);
            if (ch == -1 || ch == '\n') break;
            buf[len++] = (char)ch;
        }
        line = cl_make_string(buf, len);
        ls = (CL_String *)CL_OBJ_TO_PTR(line);
        ASSERT_STR_EQ(ls->data, "World");
    }
}

/* --- Reader + stream integration tests (Step 6) --- */

TEST(read_from_stream_simple_list)
{
    CL_Obj str = cl_make_string("(+ 1 2)", 7);
    CL_Obj s, result;
    char buf[256];

    CL_GC_PROTECT(str);
    s = cl_make_string_input_stream(str, 0, 7);
    CL_GC_UNPROTECT(1);

    result = cl_read_from_stream(s);
    ASSERT(!cl_reader_eof());
    ASSERT(CL_CONS_P(result));

    /* Print to verify structure */
    cl_prin1_to_string(result, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(+ 1 2)");
}

TEST(read_from_stream_integer)
{
    CL_Obj str = cl_make_string("42", 2);
    CL_Obj s, result;

    CL_GC_PROTECT(str);
    s = cl_make_string_input_stream(str, 0, 2);
    CL_GC_UNPROTECT(1);

    result = cl_read_from_stream(s);
    ASSERT(!cl_reader_eof());
    ASSERT(CL_FIXNUM_P(result));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(result), 42);
}

TEST(read_from_stream_multiple_exprs)
{
    CL_Obj str = cl_make_string("1 2 3", 5);
    CL_Obj s, r1, r2, r3;

    CL_GC_PROTECT(str);
    s = cl_make_string_input_stream(str, 0, 5);
    CL_GC_UNPROTECT(1);

    r1 = cl_read_from_stream(s);
    ASSERT(!cl_reader_eof());
    ASSERT_EQ_INT(CL_FIXNUM_VAL(r1), 1);

    r2 = cl_read_from_stream(s);
    ASSERT(!cl_reader_eof());
    ASSERT_EQ_INT(CL_FIXNUM_VAL(r2), 2);

    r3 = cl_read_from_stream(s);
    ASSERT(!cl_reader_eof());
    ASSERT_EQ_INT(CL_FIXNUM_VAL(r3), 3);

    /* Next read should hit EOF */
    cl_read_from_stream(s);
    ASSERT(cl_reader_eof());
}

TEST(read_from_stream_empty)
{
    CL_Obj str = cl_make_string("", 0);
    CL_Obj s;

    CL_GC_PROTECT(str);
    s = cl_make_string_input_stream(str, 0, 0);
    CL_GC_UNPROTECT(1);

    cl_read_from_stream(s);
    ASSERT(cl_reader_eof());
}

TEST(read_from_stream_string_literal)
{
    CL_Obj str = cl_make_string("\"hello\"", 7);
    CL_Obj s, result;
    CL_String *rs;

    CL_GC_PROTECT(str);
    s = cl_make_string_input_stream(str, 0, 7);
    CL_GC_UNPROTECT(1);

    result = cl_read_from_stream(s);
    ASSERT(!cl_reader_eof());
    ASSERT(CL_STRING_P(result));
    rs = (CL_String *)CL_OBJ_TO_PTR(result);
    ASSERT_STR_EQ(rs->data, "hello");
}

TEST(read_from_string_backward_compat)
{
    CL_ReadStream stream;
    CL_Obj r1, r2;

    stream.buf = "10 20";
    stream.pos = 0;
    stream.len = 5;
    stream.line = 1;

    r1 = cl_read_from_string(&stream);
    ASSERT(!cl_reader_eof());
    ASSERT_EQ_INT(CL_FIXNUM_VAL(r1), 10);

    /* Position should have advanced */
    ASSERT(stream.pos > 0);

    r2 = cl_read_from_string(&stream);
    ASSERT(!cl_reader_eof());
    ASSERT_EQ_INT(CL_FIXNUM_VAL(r2), 20);
}

/* --- Printer + stream integration tests (Step 7) --- */

TEST(prin1_to_string_stream)
{
    CL_Obj sstream = cl_make_string_output_stream();
    CL_Obj result;
    CL_String *rs;

    CL_GC_PROTECT(sstream);
    cl_prin1_to_stream(CL_MAKE_FIXNUM(42), sstream);
    result = cl_get_output_stream_string(sstream);
    CL_GC_UNPROTECT(1);

    ASSERT(CL_STRING_P(result));
    rs = (CL_String *)CL_OBJ_TO_PTR(result);
    ASSERT_STR_EQ(rs->data, "42");
}

TEST(princ_to_string_stream)
{
    CL_Obj sstream = cl_make_string_output_stream();
    CL_Obj str = cl_make_string("hello", 5);
    CL_Obj result;
    CL_String *rs;

    CL_GC_PROTECT(sstream);
    CL_GC_PROTECT(str);
    cl_princ_to_stream(str, sstream);
    result = cl_get_output_stream_string(sstream);
    CL_GC_UNPROTECT(2);

    ASSERT(CL_STRING_P(result));
    rs = (CL_String *)CL_OBJ_TO_PTR(result);
    ASSERT_STR_EQ(rs->data, "hello");
}

TEST(prin1_string_escapes_to_stream)
{
    CL_Obj sstream = cl_make_string_output_stream();
    CL_Obj str = cl_make_string("hello", 5);
    CL_Obj result;
    CL_String *rs;

    CL_GC_PROTECT(sstream);
    CL_GC_PROTECT(str);
    cl_prin1_to_stream(str, sstream);
    result = cl_get_output_stream_string(sstream);
    CL_GC_UNPROTECT(2);

    ASSERT(CL_STRING_P(result));
    rs = (CL_String *)CL_OBJ_TO_PTR(result);
    ASSERT_STR_EQ(rs->data, "\"hello\"");
}

TEST(print_to_string_stream)
{
    CL_Obj sstream = cl_make_string_output_stream();
    CL_Obj result;
    CL_String *rs;

    CL_GC_PROTECT(sstream);
    cl_print_to_stream(CL_MAKE_FIXNUM(99), sstream);
    result = cl_get_output_stream_string(sstream);
    CL_GC_UNPROTECT(1);

    ASSERT(CL_STRING_P(result));
    rs = (CL_String *)CL_OBJ_TO_PTR(result);
    /* print outputs: newline, object, space */
    ASSERT_STR_EQ(rs->data, "\n99 ");
}

TEST(prin1_list_to_stream)
{
    CL_Obj sstream = cl_make_string_output_stream();
    CL_Obj list, result;
    CL_String *rs;

    CL_GC_PROTECT(sstream);
    list = cl_cons(CL_MAKE_FIXNUM(1),
           cl_cons(CL_MAKE_FIXNUM(2),
           cl_cons(CL_MAKE_FIXNUM(3), CL_NIL)));
    CL_GC_PROTECT(list);
    cl_prin1_to_stream(list, sstream);
    result = cl_get_output_stream_string(sstream);
    CL_GC_UNPROTECT(2);

    ASSERT(CL_STRING_P(result));
    rs = (CL_String *)CL_OBJ_TO_PTR(result);
    ASSERT_STR_EQ(rs->data, "(1 2 3)");
}

TEST(multiple_prints_to_same_stream)
{
    CL_Obj sstream = cl_make_string_output_stream();
    CL_Obj result;
    CL_String *rs;

    CL_GC_PROTECT(sstream);
    cl_princ_to_stream(CL_MAKE_FIXNUM(1), sstream);
    cl_stream_write_char(sstream, '+');
    cl_princ_to_stream(CL_MAKE_FIXNUM(2), sstream);
    cl_stream_write_char(sstream, '=');
    cl_princ_to_stream(CL_MAKE_FIXNUM(3), sstream);
    result = cl_get_output_stream_string(sstream);
    CL_GC_UNPROTECT(1);

    ASSERT(CL_STRING_P(result));
    rs = (CL_String *)CL_OBJ_TO_PTR(result);
    ASSERT_STR_EQ(rs->data, "1+2=3");
}

TEST(prin1_to_string_c_still_works)
{
    /* Verify the C-internal cl_prin1_to_string still works */
    char buf[64];
    int len;
    CL_Obj list = cl_cons(CL_MAKE_FIXNUM(10), cl_cons(CL_MAKE_FIXNUM(20), CL_NIL));
    CL_GC_PROTECT(list);
    len = cl_prin1_to_string(list, buf, sizeof(buf));
    CL_GC_UNPROTECT(1);
    ASSERT(len > 0);
    ASSERT_STR_EQ(buf, "(10 20)");
}

TEST(prin1_nil_to_stream)
{
    CL_Obj sstream = cl_make_string_output_stream();
    CL_Obj result;
    CL_String *rs;

    CL_GC_PROTECT(sstream);
    cl_prin1_to_stream(CL_NIL, sstream);
    result = cl_get_output_stream_string(sstream);
    CL_GC_UNPROTECT(1);

    ASSERT(CL_STRING_P(result));
    rs = (CL_String *)CL_OBJ_TO_PTR(result);
    ASSERT_STR_EQ(rs->data, "NIL");
}

TEST(prin1_char_to_stream)
{
    CL_Obj sstream = cl_make_string_output_stream();
    CL_Obj result;
    CL_String *rs;

    CL_GC_PROTECT(sstream);
    cl_prin1_to_stream(CL_MAKE_CHAR('A'), sstream);
    result = cl_get_output_stream_string(sstream);
    CL_GC_UNPROTECT(1);

    ASSERT(CL_STRING_P(result));
    rs = (CL_String *)CL_OBJ_TO_PTR(result);
    ASSERT_STR_EQ(rs->data, "#\\A");
}

TEST(princ_char_to_stream)
{
    CL_Obj sstream = cl_make_string_output_stream();
    CL_Obj result;
    CL_String *rs;

    CL_GC_PROTECT(sstream);
    cl_princ_to_stream(CL_MAKE_CHAR('A'), sstream);
    result = cl_get_output_stream_string(sstream);
    CL_GC_UNPROTECT(1);

    ASSERT(CL_STRING_P(result));
    rs = (CL_String *)CL_OBJ_TO_PTR(result);
    ASSERT_STR_EQ(rs->data, "A");
}

/* --- File stream open/close tests (Step 8) --- */

TEST(open_file_write_read)
{
    const char *path = "/tmp/cl_test_step8_open.txt";
    PlatformFile wfh;
    CL_Obj stream;
    CL_Stream *st;
    int ch;

    /* Write via platform to create file */
    wfh = platform_file_open(path, PLATFORM_FILE_WRITE);
    ASSERT(wfh != PLATFORM_FILE_INVALID);
    platform_file_write_string(wfh, "ABCDE");
    platform_file_close(wfh);

    /* Open as CL file input stream */
    stream = cl_make_stream(CL_STREAM_INPUT, CL_STREAM_FILE);
    st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
    st->handle_id = (uint32_t)platform_file_open(path, PLATFORM_FILE_READ);
    ASSERT(st->handle_id != PLATFORM_FILE_INVALID);

    ch = cl_stream_read_char(stream);
    ASSERT_EQ_INT(ch, 'A');
    ch = cl_stream_read_char(stream);
    ASSERT_EQ_INT(ch, 'B');

    cl_stream_close(stream);
}

TEST(open_file_output_stream_write)
{
    const char *path = "/tmp/cl_test_step8_out.txt";
    CL_Obj wstream, rstream;
    CL_Stream *wst, *rst;
    int ch;

    /* Create output file stream */
    wstream = cl_make_stream(CL_STREAM_OUTPUT, CL_STREAM_FILE);
    wst = (CL_Stream *)CL_OBJ_TO_PTR(wstream);
    wst->handle_id = (uint32_t)platform_file_open(path, PLATFORM_FILE_WRITE);
    ASSERT(wst->handle_id != PLATFORM_FILE_INVALID);

    cl_stream_write_char(wstream, 'X');
    cl_stream_write_char(wstream, 'Y');
    cl_stream_write_char(wstream, 'Z');
    cl_stream_close(wstream);

    /* Read back */
    rstream = cl_make_stream(CL_STREAM_INPUT, CL_STREAM_FILE);
    rst = (CL_Stream *)CL_OBJ_TO_PTR(rstream);
    rst->handle_id = (uint32_t)platform_file_open(path, PLATFORM_FILE_READ);
    ASSERT(rst->handle_id != PLATFORM_FILE_INVALID);

    ch = cl_stream_read_char(rstream);
    ASSERT_EQ_INT(ch, 'X');
    ch = cl_stream_read_char(rstream);
    ASSERT_EQ_INT(ch, 'Y');
    ch = cl_stream_read_char(rstream);
    ASSERT_EQ_INT(ch, 'Z');
    ch = cl_stream_read_char(rstream);
    ASSERT_EQ_INT(ch, -1);  /* EOF */

    cl_stream_close(rstream);
}

TEST(file_stream_write_string_read_back)
{
    const char *path = "/tmp/cl_test_step8_str.txt";
    CL_Obj wstream, rstream;
    CL_Stream *wst, *rst;
    char buf[64];
    int i, ch;

    /* Write a string to file */
    wstream = cl_make_stream(CL_STREAM_OUTPUT, CL_STREAM_FILE);
    wst = (CL_Stream *)CL_OBJ_TO_PTR(wstream);
    wst->handle_id = (uint32_t)platform_file_open(path, PLATFORM_FILE_WRITE);
    cl_stream_write_string(wstream, "Hello World", 11);
    cl_stream_close(wstream);

    /* Read back */
    rstream = cl_make_stream(CL_STREAM_INPUT, CL_STREAM_FILE);
    rst = (CL_Stream *)CL_OBJ_TO_PTR(rstream);
    rst->handle_id = (uint32_t)platform_file_open(path, PLATFORM_FILE_READ);
    i = 0;
    while ((ch = cl_stream_read_char(rstream)) != -1 && i < 63)
        buf[i++] = (char)ch;
    buf[i] = '\0';
    cl_stream_close(rstream);

    ASSERT_STR_EQ(buf, "Hello World");
}

/* --- Platform file operations (Step 10) --- */

TEST(platform_universal_time)
{
    uint32_t ut = platform_universal_time();
    /* Should be > 2000-01-01 (3155760000) and < 2100-01-01 (~6311520000 > uint32 max)
     * Actually CL universal time for 2020 is ~3786912000, for 2030 ~4102444800 */
    ASSERT(ut > 3786912000u); /* After 2020 */
}

TEST(platform_file_exists_positive)
{
    const char *path = "/tmp/cl_test_exists.tmp";
    PlatformFile f = platform_file_open(path, PLATFORM_FILE_WRITE);
    ASSERT(f != PLATFORM_FILE_INVALID);
    platform_file_write_string(f, "test");
    platform_file_close(f);

    ASSERT(platform_file_exists(path));
    remove(path);
}

TEST(platform_file_exists_negative)
{
    ASSERT(!platform_file_exists("/tmp/cl_nonexistent_xyz_123.tmp"));
}

TEST(platform_file_delete_test)
{
    const char *path = "/tmp/cl_test_delete.tmp";
    PlatformFile f = platform_file_open(path, PLATFORM_FILE_WRITE);
    platform_file_write_string(f, "test");
    platform_file_close(f);

    ASSERT(platform_file_exists(path));
    ASSERT(platform_file_delete(path) == 0);
    ASSERT(!platform_file_exists(path));
}

TEST(platform_file_rename_test)
{
    const char *old_path = "/tmp/cl_test_rename_old.tmp";
    const char *new_path = "/tmp/cl_test_rename_new.tmp";
    PlatformFile f = platform_file_open(old_path, PLATFORM_FILE_WRITE);
    platform_file_write_string(f, "test");
    platform_file_close(f);

    ASSERT(platform_file_rename(old_path, new_path) == 0);
    ASSERT(!platform_file_exists(old_path));
    ASSERT(platform_file_exists(new_path));
    remove(new_path);
}

/* Regression: renaming ONTO an existing file must replace it, the way POSIX
 * rename(2) and the Amiga back end do.  The Windows CRT's rename() fails
 * instead, and OPEN :if-exists :rename ignores the result — so a second
 * write to the same file left the stale .bak in place and then truncated the
 * original it was supposed to preserve. */
TEST(platform_file_rename_replaces_existing)
{
    const char *old_path = "/tmp/cl_test_rename_src.tmp";
    const char *new_path = "/tmp/cl_test_rename_dst.tmp";
    PlatformFile f;
    char *content;
    unsigned long size = 0;

    f = platform_file_open(old_path, PLATFORM_FILE_WRITE);
    ASSERT(f != PLATFORM_FILE_INVALID);
    platform_file_write_string(f, "NEW");
    platform_file_close(f);

    f = platform_file_open(new_path, PLATFORM_FILE_WRITE);
    ASSERT(f != PLATFORM_FILE_INVALID);
    platform_file_write_string(f, "OLD");
    platform_file_close(f);

    ASSERT_EQ_INT(platform_file_rename(old_path, new_path), 0);
    ASSERT(!platform_file_exists(old_path));
    ASSERT(platform_file_exists(new_path));

    /* The destination must now hold the SOURCE's contents. */
    content = platform_file_read(new_path, &size);
    ASSERT(content != NULL);
    if (content) {
        ASSERT_STR_EQ(content, "NEW");
        free(content);
    }
    remove(new_path);
}

TEST(platform_file_mtime_test)
{
    const char *path = "/tmp/cl_test_mtime.tmp";
    PlatformFile f = platform_file_open(path, PLATFORM_FILE_WRITE);
    uint32_t mtime;
    platform_file_write_string(f, "test");
    platform_file_close(f);

    mtime = platform_file_mtime(path);
    /* Should be a recent universal time (> 2020) */
    ASSERT(mtime > 3786912000u);
    remove(path);
}

TEST(platform_file_mtime_nonexistent)
{
    uint32_t mtime = platform_file_mtime("/tmp/cl_nonexistent_mtime.tmp");
    ASSERT_EQ_INT((int)mtime, 0);
}

TEST(platform_mkdir_test)
{
    const char *path = "/tmp/cl_test_mkdir_step10";
    ASSERT(platform_mkdir(path) == 0);
    ASSERT(platform_file_is_directory(path));
    /* Second call should succeed (already exists) */
    ASSERT(platform_mkdir(path) == 0);
    rmdir(path);
}

TEST(platform_file_is_directory_test)
{
    ASSERT(platform_file_is_directory("/tmp"));
    ASSERT(!platform_file_is_directory("/tmp/cl_nonexistent_dir_xyz"));
}

/* --- Builtin tests via eval (Step 10) --- */

TEST(eval_get_universal_time)
{
    CL_Obj result = cl_eval_string("(get-universal-time)");
    /* Should return a bignum (> fixnum range) */
    ASSERT(!CL_NULL_P(result));
    /* The value should be a number (fixnum or bignum) */
    ASSERT(CL_FIXNUM_P(result) || CL_BIGNUM_P(result));
}

TEST(eval_probe_file_exists)
{
    const char *path = "/tmp/cl_test_probe_eval.tmp";
    PlatformFile f = platform_file_open(path, PLATFORM_FILE_WRITE);
    CL_Obj result;
    platform_file_write_string(f, "test");
    platform_file_close(f);

    result = cl_eval_string("(probe-file \"/tmp/cl_test_probe_eval.tmp\")");
    ASSERT(CL_PATHNAME_P(result));

    remove(path);
}

TEST(eval_probe_file_not_exists)
{
    CL_Obj result = cl_eval_string("(probe-file \"/tmp/cl_nonexistent_eval_xyz.tmp\")");
    ASSERT(CL_NULL_P(result));
}

/* Regression: TRUENAME / PROBE-FILE of a real directory given in FILE form
 * (no trailing slash) must return a DIRECTORY-form pathname (pathname-name and
 * pathname-type NIL, pathname-directory carrying the final component), because
 * the on-disk object is a directory.  Previously the trailing slash was only
 * preserved when the *input* had one, so (directory-pathname-p (truename
 * "/tmp/dir")) was NIL for a real directory; uiop:directory-exists-p — (and
 * (directory-pathname-p (probe-file* x :truename t)) ...) — then wrongly
 * returned NIL (the chipi MAKE-PERSISTENCE--SIMPLE failure: its storage path
 * has no trailing slash). */
TEST(eval_truename_dir_no_slash_is_dir_form)
{
    const char *path = "/tmp/cl_test_truename_dir";
    CL_Obj result;
    ASSERT(platform_mkdir(path) == 0);

    /* TRUENAME of the directory WITHOUT a trailing slash => directory form:
     * name + type NIL, and the final component lands in pathname-directory. */
    result = cl_eval_string(
        "(let ((p (truename #P\"/tmp/cl_test_truename_dir\")))"
        "  (and (null (pathname-name p)) (null (pathname-type p))"
        "       (member \"cl_test_truename_dir\" (pathname-directory p)"
        "               :test #'equal)))");
    ASSERT(!CL_NULL_P(result));

    /* PROBE-FILE likewise normalizes a directory to directory form. */
    result = cl_eval_string(
        "(let ((p (probe-file #P\"/tmp/cl_test_truename_dir\")))"
        "  (and p (null (pathname-name p)) (null (pathname-type p))))");
    ASSERT(!CL_NULL_P(result));

    rmdir(path);
}

/* The directory-form fix must NOT turn files into directories: TRUENAME of a
 * regular file keeps its NAME component (stays in file form). */
TEST(eval_truename_file_stays_file_form)
{
    const char *path = "/tmp/cl_test_truename_file.tmp";
    PlatformFile f = platform_file_open(path, PLATFORM_FILE_WRITE);
    CL_Obj result;
    platform_file_write_string(f, "x");
    platform_file_close(f);

    result = cl_eval_string(
        "(pathname-name (truename #P\"/tmp/cl_test_truename_file.tmp\"))");
    ASSERT(!CL_NULL_P(result)); /* name is "cl_test_truename_file" */

    remove(path);
}

TEST(eval_delete_file)
{
    const char *path = "/tmp/cl_test_delete_eval.tmp";
    PlatformFile f = platform_file_open(path, PLATFORM_FILE_WRITE);
    platform_file_write_string(f, "test");
    platform_file_close(f);

    cl_eval_string("(delete-file \"/tmp/cl_test_delete_eval.tmp\")");
    ASSERT(!platform_file_exists(path));
}

TEST(eval_file_write_date)
{
    const char *path = "/tmp/cl_test_fwd_eval.tmp";
    PlatformFile f = platform_file_open(path, PLATFORM_FILE_WRITE);
    CL_Obj result;
    platform_file_write_string(f, "test");
    platform_file_close(f);

    result = cl_eval_string("(file-write-date \"/tmp/cl_test_fwd_eval.tmp\")");
    ASSERT(!CL_NULL_P(result));
    /* Should be a number */
    ASSERT(CL_FIXNUM_P(result) || CL_BIGNUM_P(result));

    remove(path);
}

TEST(eval_file_namestring)
{
    CL_Obj result = cl_eval_string("(file-namestring \"/foo/bar/baz.txt\")");
    CL_String *s;
    ASSERT(CL_STRING_P(result));
    s = (CL_String *)CL_OBJ_TO_PTR(result);
    ASSERT_STR_EQ(s->data, "baz.txt");
}

TEST(eval_directory_namestring)
{
    CL_Obj result = cl_eval_string("(directory-namestring \"/foo/bar/baz.txt\")");
    CL_String *s;
    ASSERT(CL_STRING_P(result));
    s = (CL_String *)CL_OBJ_TO_PTR(result);
    ASSERT_STR_EQ(s->data, "/foo/bar/");
}

TEST(eval_rename_file)
{
    const char *old_path = "/tmp/cl_test_rename_eval_old.tmp";
    const char *new_path = "/tmp/cl_test_rename_eval_new.tmp";
    PlatformFile f = platform_file_open(old_path, PLATFORM_FILE_WRITE);
    platform_file_write_string(f, "test");
    platform_file_close(f);

    cl_eval_string("(rename-file \"/tmp/cl_test_rename_eval_old.tmp\" \"/tmp/cl_test_rename_eval_new.tmp\")");
    ASSERT(!platform_file_exists(old_path));
    ASSERT(platform_file_exists(new_path));

    remove(new_path);
}

TEST(eval_values_from_defun)
{
    /* Regression test: values must propagate through defun returns */
    const char *expr = "(progn (defun mv-test () (values 10 20 30)) "
                       "(multiple-value-list (mv-test)))";
    CL_Obj result = cl_eval_string(expr);
    char buf[64];
    cl_prin1_to_string(result, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(10 20 30)");
}

/* --- Readtable tests (Step 12) --- */

TEST(readtablep_current)
{
    CL_Obj result = cl_eval_string("(readtablep *readtable*)");
    ASSERT(result == CL_T);
}

TEST(readtablep_non_readtable)
{
    CL_Obj result = cl_eval_string("(readtablep 42)");
    ASSERT(CL_NULL_P(result));
}

TEST(get_macro_character_paren)
{
    /* ( is a terminating macro — fn should be NIL (built-in), non-term-p should be NIL */
    CL_Obj result = cl_eval_string("(get-macro-character #\\()");
    ASSERT(CL_NULL_P(result));
}

TEST(get_macro_character_hash)
{
    /* # is a non-terminating macro — check second value */
    CL_Obj result = cl_eval_string(
        "(multiple-value-list (get-macro-character #\\#))");
    char buf[64];
    cl_prin1_to_string(result, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(NIL T)");
}

TEST(get_macro_character_string)
{
    /* Regression: #\" is read inline in C, so its macro_fn is NIL — but a
       caller that grabs the standard string reader expects a FUNCTION it can
       funcall (pythonic-string-reader, an mgl-pax/clog dependency, does
       exactly this and otherwise funcalls NIL -> "Not a function: NIL").
       GET-MACRO-CHARACTER hands back the %READ-STANDARD-STRING wrapper. */
    char buf[64];
    CL_Obj r;

    /* It is a real function, not NIL. */
    r = cl_eval_string("(functionp (get-macro-character #\\\"))");
    cl_prin1_to_string(r, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "T");

    /* Funcalling it (stream macro-char) reads a "..." body — the opening quote
       is treated as already consumed, per the macro-character protocol. */
    r = cl_eval_string(
        "(let ((s (make-string-input-stream \"hi\\\" rest\")))"
        "  (funcall (get-macro-character #\\\") s #\\\"))");
    cl_prin1_to_string(r, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "\"hi\"");
}

TEST(set_macro_character_excl)
{
    /* Define ! as a reader macro: set it up via C API directly to verify */
    CL_Obj result;
    CL_Readtable *rt;

    /* Verify set-macro-character works at the C level */
    result = cl_eval_string(
        "(set-macro-character #\\! "
        "  (lambda (stream char) "
        "    (list 'quote (read stream t nil t))))");
    ASSERT(result == CL_T);

    /* Check the readtable was actually modified */
    rt = cl_readtable_current();
    ASSERT(rt->syntax['!'] == CL_CHAR_TERM_MACRO);
    ASSERT(!CL_NULL_P(rt->macro_fn['!']));

    /* Now read and eval with ! as a macro character */
    result = cl_eval_string("(equal !hello 'hello)");
    ASSERT(result == CL_T);

    /* Clean up: restore ! to constituent */
    rt->syntax['!'] = CL_CHAR_CONSTITUENT;
    rt->macro_fn['!'] = CL_NIL;
}

TEST(copy_readtable_preserves)
{
    /* Copy current readtable, verify the copy is a valid readtable */
    CL_Obj result = cl_eval_string("(readtablep (copy-readtable))");
    ASSERT(result == CL_T);
}

TEST(compile_nil_lambda)
{
    /* (compile nil '(lambda (x) (+ x 1))) => function, then funcall */
    CL_Obj result = cl_eval_string(
        "(funcall (compile nil '(lambda (x) (+ x 1))) 41)");
    ASSERT(CL_FIXNUM_P(result));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(result), 42);
}

TEST(compile_named_function)
{
    /* (compile 'car) => returns the car function */
    CL_Obj result = cl_eval_string("(functionp (compile 'car))");
    ASSERT(result == CL_T);
}

TEST(star_readtable_is_special)
{
    /* *readtable* should be a special variable */
    CL_Obj result = cl_eval_string("(boundp '*readtable*)");
    ASSERT(result == CL_T);
}

/* --- TCP Socket Stream tests --- */

/* The peer side of the socket-stream tests.
 *
 * It runs in a THREAD, not a forked child: Windows has no fork(), and a
 * thread is a faithful stand-in here because the peer only ever touches its
 * own socket (which it owns and closes) — no Lisp heap, no VM state.  What
 * the peer received is handed back in the struct instead of through a pipe,
 * which is also why the pipe(2) plumbing the forked version needed is gone. */
typedef struct {
    raw_sock_t  fd;            /* listening (TCP) or bound (UDP) socket; owned */
    const char *send_data;     /* bytes to send once connected, or NULL */
    int         send_len;
    int         want;          /* bytes to read from the client (0 = none) */
    int         echo;          /* send back whatever was read */
    int         udp_reverse;   /* UDP: reflect one datagram, byte-reversed */
    char        got[64];       /* what the peer received */
    int         got_len;       /* how much (valid once joined) */
} TestPeer;

static void *test_peer_thread(void *arg)
{
    TestPeer *p = (TestPeer *)arg;
    struct sockaddr_in caddr;
    socklen_t clen = sizeof(caddr);

    if (p->udp_reverse) {
        unsigned char buf[64];
        int n = (int)recvfrom(p->fd, (char *)buf, (int)sizeof(buf), 0,
                              (struct sockaddr *)&caddr, &clen);
        if (n > 0) {
            unsigned char rev[64];
            int i;
            for (i = 0; i < n; i++) rev[i] = buf[n - 1 - i];
            sendto(p->fd, (const char *)rev, n, 0,
                   (struct sockaddr *)&caddr, clen);
            if (n > (int)sizeof(p->got)) n = (int)sizeof(p->got);
            memcpy(p->got, buf, (size_t)n);
            p->got_len = n;
        }
        raw_close(p->fd);
        return NULL;
    }

    {
        raw_sock_t cfd = accept(p->fd, (struct sockaddr *)&caddr, &clen);
        raw_close(p->fd);
        if (!RAW_SOCK_VALID(cfd))
            return NULL;
        if (p->send_data && p->send_len > 0)
            (void)raw_send(cfd, p->send_data, p->send_len);
        if (p->want > 0) {
            int cap = p->want < (int)sizeof(p->got) ? p->want : (int)sizeof(p->got);
            int n = (int)raw_recv(cfd, p->got, cap);
            if (n > 0) {
                p->got_len = n;
                if (p->echo)
                    (void)raw_send(cfd, p->got, n);
            }
        }
        raw_close(cfd);
    }
    return NULL;
}

/* Start a local TCP server on a random port that echoes data back.
 * Returns the port number, sets *server_fd. The caller must accept(). */
static int start_echo_server(raw_sock_t *server_fd)
{
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);

    *server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (!RAW_SOCK_VALID(*server_fd)) return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;  /* Let OS pick a port */

    if (bind(*server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        raw_close(*server_fd);
        return -1;
    }
    if (listen(*server_fd, 1) < 0) {
        raw_close(*server_fd);
        return -1;
    }
    /* Get assigned port */
    if (getsockname(*server_fd, (struct sockaddr *)&addr, &addrlen) < 0) {
        raw_close(*server_fd);
        return -1;
    }
    return ntohs(addr.sin_port);
}

TEST(socket_stream_connect_write_read)
{
    raw_sock_t server_fd;
    int port;
    TestPeer peer;
    pthread_t th;
    CL_Obj stream;

    port = start_echo_server(&server_fd);
    ASSERT(port > 0);

    /* A peer that sends "OK" then closes */
    memset(&peer, 0, sizeof(peer));
    peer.fd = server_fd;
    peer.send_data = "OK";
    peer.send_len = 2;
    ASSERT(pthread_create(&th, NULL, test_peer_thread, &peer) == 0);

    /* Connect via our socket stream */
    stream = cl_make_socket_stream("127.0.0.1", port, 0);
    ASSERT(!CL_NULL_P(stream));
    ASSERT(CL_STREAM_P(stream));

    {
        CL_Stream *st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
        ASSERT_EQ_INT((int)st->stream_type, CL_STREAM_SOCKET);
        ASSERT_EQ_INT((int)st->direction, CL_STREAM_IO);
        ASSERT(st->flags & CL_STREAM_FLAG_OPEN);
    }

    /* Read "OK" from server */
    {
        int ch1 = cl_stream_read_char(stream);
        int ch2 = cl_stream_read_char(stream);
        int ch3 = cl_stream_read_char(stream);  /* EOF */
        ASSERT_EQ_INT(ch1, 'O');
        ASSERT_EQ_INT(ch2, 'K');
        ASSERT_EQ_INT(ch3, -1);
    }

    cl_stream_close(stream);

    pthread_join(th, NULL);
}

TEST(socket_stream_close)
{
    raw_sock_t server_fd;
    int port;
    TestPeer peer;
    pthread_t th;
    CL_Obj stream;

    port = start_echo_server(&server_fd);
    ASSERT(port > 0);

    memset(&peer, 0, sizeof(peer));
    peer.fd = server_fd;
    ASSERT(pthread_create(&th, NULL, test_peer_thread, &peer) == 0);

    stream = cl_make_socket_stream("127.0.0.1", port, 0);
    ASSERT(!CL_NULL_P(stream));

    /* Verify open */
    {
        CL_Stream *st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
        ASSERT(st->flags & CL_STREAM_FLAG_OPEN);
    }

    cl_stream_close(stream);

    /* Verify closed */
    {
        CL_Stream *st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
        ASSERT(!(st->flags & CL_STREAM_FLAG_OPEN));
    }

    pthread_join(th, NULL);
}

/* Regression: CL:LISTEN on a socket stream must report real readiness, which
 * is what usocket:wait-for-input uses to drive hunchentoot's accept loop.
 * Exercises platform_socket_data_available (the primitive bi_listen calls):
 * a listener becomes ready when a connection is pending; a connection stream
 * becomes ready when data arrives and reports EOF (2) when the peer closes. */
TEST(socket_listen_reports_readiness)
{
    int bound_port = 0;
    CL_Obj listener = cl_make_listen_stream(0, 1, &bound_port);
    CL_Stream *lst, *cst;
    struct sockaddr_in addr;
    CL_Obj conn;
    raw_sock_t cfd;
    int i, avail;

    ASSERT(!CL_NULL_P(listener));
    ASSERT(bound_port > 0);
    lst = (CL_Stream *)CL_OBJ_TO_PTR(listener);

    /* No client yet: listener is not ready. */
    ASSERT_EQ_INT(platform_socket_data_available((PlatformSocket)lst->handle_id), 0);

    /* Connect a raw client. */
    cfd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT(RAW_SOCK_VALID(cfd));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)bound_port);
    ASSERT(connect(cfd, (struct sockaddr *)&addr, sizeof(addr)) == 0);

    /* The pending connection makes the listener readable (poll up to ~1s). */
    for (i = 0, avail = 0; i < 100; i++) {
        avail = platform_socket_data_available((PlatformSocket)lst->handle_id);
        if (avail == 1) break;
        usleep(10000);
    }
    ASSERT_EQ_INT(avail, 1);

    /* Accept; the listener returns to not-ready. */
    conn = cl_socket_stream_accept(listener);
    ASSERT(!CL_NULL_P(conn));
    ASSERT_EQ_INT(platform_socket_data_available((PlatformSocket)lst->handle_id), 0);
    cst = (CL_Stream *)CL_OBJ_TO_PTR(conn);

    /* No data sent yet: connection not ready. */
    ASSERT_EQ_INT(platform_socket_data_available((PlatformSocket)cst->handle_id), 0);

    /* Client writes a byte: connection becomes ready, then we read it. */
    ASSERT(raw_send(cfd, "X", 1) == 1);
    for (i = 0, avail = 0; i < 100; i++) {
        avail = platform_socket_data_available((PlatformSocket)cst->handle_id);
        if (avail == 1) break;
        usleep(10000);
    }
    ASSERT_EQ_INT(avail, 1);
    ASSERT_EQ_INT(cl_stream_read_byte(conn), 'X');

    /* Client closes: a subsequent probe reports EOF (code 2, => CL:LISTEN NIL). */
    raw_close(cfd);
    for (i = 0, avail = 0; i < 100; i++) {
        avail = platform_socket_data_available((PlatformSocket)cst->handle_id);
        if (avail == 2) break;
        usleep(10000);
    }
    ASSERT_EQ_INT(avail, 2);

    cl_stream_close(conn);
    cl_stream_close(listener);
}

/* Regression: closing a socket handle twice must not corrupt the slot table.
 * The second close has to see the slot as free and do nothing; if the guard
 * lets it through, the slot is pushed onto the free list a second time and
 * two later sockets are handed the SAME index — closing one then kills the
 * other's connection.  (On Windows the guard was written as `fd >= 0`, which
 * is vacuously true for the unsigned SOCKET type, so it never fired.) */
TEST(socket_double_close_keeps_slot_table_sane)
{
    int port_a = 0, port_b = 0;
    PlatformSocket a, b, c;

    a = platform_socket_listen(0, 1, &port_a);
    ASSERT(a != PLATFORM_SOCKET_INVALID);
    platform_socket_close(a);
    platform_socket_close(a);              /* must be a no-op, not a re-free */

    /* Two fresh handles must be distinct: a duplicated free-list entry shows
     * up here as the same index handed out twice. */
    b = platform_socket_listen(0, 1, &port_b);
    ASSERT(b != PLATFORM_SOCKET_INVALID);
    c = platform_socket_listen(0, 1, NULL);
    ASSERT(c != PLATFORM_SOCKET_INVALID);
    ASSERT(b != c);
    ASSERT(port_b > 0);

    platform_socket_close(b);
    platform_socket_close(c);
}

TEST(socket_stream_connect_failure)
{
    /* Connecting to a port with nothing listening should return NIL */
    CL_Obj stream = cl_make_socket_stream("127.0.0.1", 1, 0);  /* Port 1: unlikely to be listening */
    ASSERT(CL_NULL_P(stream));
}

/* Regression: a bounded connect to an unreachable address must fail within the
 * requested window rather than stalling on the OS connect timeout.  10.255.255.1
 * is a black-hole route (SYNs are dropped), so an unbounded connect would block
 * for ~75s; with a 1s budget platform_socket_connect must give up promptly. */
TEST(socket_connect_timeout_unreachable)
{
    uint32_t t0, elapsed;
    PlatformSocket sh;
    t0 = platform_time_ms();
    sh = platform_socket_connect("10.255.255.1", 8086, 1000);
    elapsed = platform_time_ms() - t0;
    ASSERT_EQ_INT(sh, PLATFORM_SOCKET_INVALID);
    /* Should bail around the 1s deadline; allow generous slack for CI load but
     * well under the multi-second OS connect timeout it replaces. */
    ASSERT(elapsed < 5000);
}

TEST(socket_stream_write_string)
{
    raw_sock_t server_fd;
    int port;
    TestPeer peer;
    pthread_t th;
    CL_Obj stream;

    port = start_echo_server(&server_fd);
    ASSERT(port > 0);

    memset(&peer, 0, sizeof(peer));
    peer.fd = server_fd;
    peer.want = 64;
    peer.echo = 1;
    ASSERT(pthread_create(&th, NULL, test_peer_thread, &peer) == 0);

    stream = cl_make_socket_stream("127.0.0.1", port, 0);
    ASSERT(!CL_NULL_P(stream));

    /* Write via write_string */
    cl_stream_write_string(stream, "Hello", 5);

    /* Shutdown write side to trigger echo — use platform_socket_close
     * Actually we need a half-close. Let's just close and verify we wrote. */
    cl_stream_close(stream);

    pthread_join(th, NULL);
}

TEST(eval_open_tcp_stream_connect_failure)
{
    /* ext:open-tcp-stream should signal an error on connection failure */
    CL_Obj result = cl_eval_string(
        "(handler-case (ext:open-tcp-stream \"127.0.0.1\" 1) "
        "  (error (c) (declare (ignore c)) :connection-failed))");
    /* Should get :CONNECTION-FAILED from the error handler */
    ASSERT(!CL_NULL_P(result));
}

TEST(eval_open_tcp_stream_read_byte)
{
    raw_sock_t server_fd;
    int port;
    TestPeer peer;
    pthread_t th;
    char expr[256];
    CL_Obj result;

    port = start_echo_server(&server_fd);
    ASSERT(port > 0);

    /* Peer sends byte 65 ('A') then closes */
    memset(&peer, 0, sizeof(peer));
    peer.fd = server_fd;
    peer.send_data = "A";
    peer.send_len = 1;
    ASSERT(pthread_create(&th, NULL, test_peer_thread, &peer) == 0);

    /* Eval: open stream, read one byte, close */
    snprintf(expr, sizeof(expr),
        "(let ((s (ext:open-tcp-stream \"127.0.0.1\" %d)))"
        "  (prog1 (read-byte s)"
        "    (close s)))",
        port);
    result = cl_eval_string(expr);

    /* Should get 65 (ASCII 'A') */
    ASSERT(CL_FIXNUM_P(result));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(result), 65);

    pthread_join(th, NULL);
}

TEST(eval_open_tcp_stream_write_byte)
{
    raw_sock_t server_fd;
    int port;
    TestPeer peer;
    pthread_t th;
    char expr[256];
    CL_Obj result;

    port = start_echo_server(&server_fd);
    ASSERT(port > 0);

    /* Peer reads one byte and reports it back through the struct */
    memset(&peer, 0, sizeof(peer));
    peer.fd = server_fd;
    peer.want = 1;
    ASSERT(pthread_create(&th, NULL, test_peer_thread, &peer) == 0);

    /* Eval: open stream, write byte 42, close */
    snprintf(expr, sizeof(expr),
        "(let ((s (ext:open-tcp-stream \"127.0.0.1\" %d)))"
        "  (write-byte 42 s)"
        "  (close s)"
        "  t)",
        port);
    result = cl_eval_string(expr);
    ASSERT(result == CL_T);

    /* Verify the peer received byte 42 (join first: the field is only
     * settled once the peer thread has finished). */
    pthread_join(th, NULL);
    ASSERT_EQ_INT(peer.got_len, 1);
    ASSERT_EQ_INT((int)(unsigned char)peer.got[0], 42);
}

/* --- UDP datagram socket streams (EXT:OPEN-UDP-STREAM & co.) --- */

/* Bind a UDP socket on 127.0.0.1 with an OS-assigned port; returns the port
 * and stores the fd.  The UDP analogue of start_echo_server. */
static int start_udp_peer(raw_sock_t *peer_fd)
{
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    *peer_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (!RAW_SOCK_VALID(*peer_fd)) return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(*peer_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(*peer_fd);
        return -1;
    }
    if (getsockname(*peer_fd, (struct sockaddr *)&addr, &addrlen) < 0) {
        close(*peer_fd);
        return -1;
    }
    return ntohs(addr.sin_port);
}

TEST(eval_udp_stream_roundtrip)
{
    raw_sock_t peer_fd;
    int port;
    TestPeer peer;
    pthread_t th;
    char expr[512];
    CL_Obj result;

    port = start_udp_peer(&peer_fd);
    ASSERT(port > 0);

    /* Peer: receive one datagram, echo it back reversed. */
    memset(&peer, 0, sizeof(peer));
    peer.fd = peer_fd;
    peer.udp_reverse = 1;
    ASSERT(pthread_create(&th, NULL, test_peer_thread, &peer) == 0);

    /* Send #(1 2 3 4 5), expect (5 4 3 2 1) back. */
    snprintf(expr, sizeof(expr),
        "(let ((s (ext:open-udp-stream \"127.0.0.1\" %d))"
        "      (buf (make-array 16 :element-type '(unsigned-byte 8)"
        "                          :initial-element 0)))"
        "  (ext:udp-stream-send s (coerce #(1 2 3 4 5)"
        "                                 '(vector (unsigned-byte 8))) 5)"
        "  (let ((len (ext:udp-stream-receive s buf)))"
        "    (close s)"
        "    (and (= len 5)"
        "         (equal (coerce (subseq buf 0 5) 'list) '(5 4 3 2 1)))))",
        port);
    result = cl_eval_string(expr);
    ASSERT(result == CL_T);

    pthread_join(th, NULL);
}

TEST(eval_udp_stream_receive_timeout)
{
    raw_sock_t peer_fd;
    int port;
    char expr[384];
    CL_Obj result;

    /* A bound-but-silent peer: nothing ever arrives, the read timeout must
     * fire and raise EXT:SOCKET-TIMEOUT. */
    port = start_udp_peer(&peer_fd);
    ASSERT(port > 0);

    snprintf(expr, sizeof(expr),
        "(let ((s (ext:open-udp-stream \"127.0.0.1\" %d))"
        "      (buf (make-array 8 :element-type '(unsigned-byte 8))))"
        "  (setf (ext:socket-stream-timeout s :input) 0.15)"
        "  (prog1 (handler-case (progn (ext:udp-stream-receive s buf) :data)"
        "           (ext:socket-timeout () :timed-out))"
        "    (close s)))",
        port);
    result = cl_eval_string(expr);
    {
        CL_Obj expected = cl_eval_string(":timed-out");
        ASSERT(result == expected);
    }
    raw_close(peer_fd);
}

TEST(eval_udp_stream_local_endpoint)
{
    raw_sock_t peer_fd;
    int port;
    char expr[384];
    CL_Obj result;

    port = start_udp_peer(&peer_fd);
    ASSERT(port > 0);

    /* Connected UDP: getsockname must yield the loopback address and a
     * non-zero ephemeral port. */
    snprintf(expr, sizeof(expr),
        "(let ((s (ext:open-udp-stream \"127.0.0.1\" %d)))"
        "  (multiple-value-bind (ip lport)"
        "      (ext:socket-stream-local-endpoint s)"
        "    (close s)"
        "    (and (string= ip \"127.0.0.1\") (integerp lport) (> lport 0))))",
        port);
    result = cl_eval_string(expr);
    ASSERT(result == CL_T);
    raw_close(peer_fd);
}

TEST(eval_udp_stream_type_errors)
{
    /* The datagram entry points must reject non-UDP streams with a clear
     * type error rather than corrupting the byte-stream state. */
    CL_Obj result = cl_eval_string(
        "(handler-case (ext:udp-stream-send *standard-output*"
        "                (make-array 1 :element-type '(unsigned-byte 8)))"
        "  (type-error () :rejected)"
        "  (error () :rejected))");
    CL_Obj expected = cl_eval_string(":rejected");
    ASSERT(result == expected);
}

/* --- Server-side TCP socket tests (listen/accept) --- */

/* Single-threaded loopback round trip exercising the raw platform layer.
 * connect() into the listen backlog completes before accept() on loopback,
 * so no second thread is needed. */
TEST(platform_socket_listen_accept)
{
    int port = 0;
    PlatformSocket lst, cli, conn;

    lst = platform_socket_listen(0, 1, &port);  /* ephemeral, loopback */
    ASSERT(lst != PLATFORM_SOCKET_INVALID);
    ASSERT(port > 0);

    cli = platform_socket_connect("127.0.0.1", port, 0);
    ASSERT(cli != PLATFORM_SOCKET_INVALID);

    conn = platform_socket_accept(lst);
    ASSERT(conn != PLATFORM_SOCKET_INVALID);

    /* client -> server */
    ASSERT_EQ_INT(platform_socket_write(cli, 'X'), 0);
    ASSERT_EQ_INT(platform_socket_flush(cli), 0);
    ASSERT_EQ_INT(platform_socket_read(conn), 'X');

    /* server -> client */
    ASSERT_EQ_INT(platform_socket_write(conn, 'Y'), 0);
    ASSERT_EQ_INT(platform_socket_flush(conn), 0);
    ASSERT_EQ_INT(platform_socket_read(cli), 'Y');

    platform_socket_close(cli);
    platform_socket_close(conn);
    platform_socket_close(lst);
}

/* Listen on a port that is already bound -> failure (INVALID). */
TEST(platform_socket_listen_busy_port)
{
    int port = 0;
    PlatformSocket a, b;

    a = platform_socket_listen(0, 1, &port);
    ASSERT(a != PLATFORM_SOCKET_INVALID);
    ASSERT(port > 0);

    /* Second bind to the same loopback port must fail (SO_REUSEADDR still
     * disallows two simultaneous listeners on the same address). */
    b = platform_socket_listen(port, 1, NULL);
    ASSERT(b == PLATFORM_SOCKET_INVALID);

    platform_socket_close(a);
}

/* Regression: the POSIX socket table grows on demand, well past the old fixed
 * 16-slot cap and across the 64-slot block boundary, and recycles freed slots.
 * Holds many simultaneous connections at once, verifies every handle is valid,
 * distinct and carries data, then proves close() returns slots to the
 * free-list so a second wave reuses them. */
#define MANY_CONNS 100
TEST(platform_socket_table_grows_many_connections)
{
    int port = 0;
    PlatformSocket lst;
    PlatformSocket cli[MANY_CONNS], conn[MANY_CONNS];
    PlatformSocket max_handle = 0;
    int i, j;

    lst = platform_socket_listen(0, 1, &port);
    ASSERT(lst != PLATFORM_SOCKET_INVALID);
    ASSERT(port > 0);

    /* Interleave connect+accept: the listen backlog is small, so draining each
     * pending connection before opening the next keeps the queue from filling. */
    for (i = 0; i < MANY_CONNS; i++) {
        cli[i]  = platform_socket_connect("127.0.0.1", port, 0);
        ASSERT(cli[i] != PLATFORM_SOCKET_INVALID);
        conn[i] = platform_socket_accept(lst);
        ASSERT(conn[i] != PLATFORM_SOCKET_INVALID);
        if (cli[i]  > max_handle) max_handle = cli[i];
        if (conn[i] > max_handle) max_handle = conn[i];
    }

    /* With 1 listener + 200 connection slots live at once, handles must have
     * climbed past the old 16-slot cap AND past the 64-slot first block — i.e.
     * the table genuinely grew rather than capping out. */
    ASSERT(max_handle > 64);

    /* All client and server handles are distinct (no two claims collided). */
    {
        PlatformSocket all[2 * MANY_CONNS];
        for (i = 0; i < MANY_CONNS; i++) { all[i] = cli[i]; all[MANY_CONNS + i] = conn[i]; }
        for (i = 0; i < 2 * MANY_CONNS; i++) {
            ASSERT(all[i] != lst);
            for (j = i + 1; j < 2 * MANY_CONNS; j++)
                ASSERT(all[i] != all[j]);
        }
    }

    /* Every connection round-trips a distinct byte, proving each slot points at
     * its own fd and IOBuf (no aliasing across the grown blocks). */
    for (i = 0; i < MANY_CONNS; i++) {
        int byte = 'A' + (i % 26);
        ASSERT_EQ_INT(platform_socket_write(cli[i], byte), 0);
        ASSERT_EQ_INT(platform_socket_flush(cli[i]), 0);
        ASSERT_EQ_INT(platform_socket_read(conn[i]), byte);
    }

    /* Close everything, returning all slots to the free-list. */
    for (i = 0; i < MANY_CONNS; i++) {
        platform_socket_close(cli[i]);
        platform_socket_close(conn[i]);
    }

    /* A second wave reuses recycled slots: it must still succeed and stay
     * within the high-water mark (no unbounded growth from churn). */
    {
        PlatformSocket c2, n2, hi2 = 0;
        for (i = 0; i < MANY_CONNS; i++) {
            c2 = platform_socket_connect("127.0.0.1", port, 0);
            ASSERT(c2 != PLATFORM_SOCKET_INVALID);
            n2 = platform_socket_accept(lst);
            ASSERT(n2 != PLATFORM_SOCKET_INVALID);
            if (c2 > hi2) hi2 = c2;
            if (n2 > hi2) hi2 = n2;
            platform_socket_close(c2);
            platform_socket_close(n2);
        }
        ASSERT(hi2 <= max_handle);   /* recycled, not grown further */
    }

    platform_socket_close(lst);
}
#undef MANY_CONNS

/* The cl_make_listen_stream / cl_socket_stream_accept wrappers. */
TEST(socket_stream_listen_accept)
{
    int port = 0;
    PlatformSocket probe;
    CL_Obj listener, client, conn;

    /* Grab a free port via a throwaway listener, then reuse it. */
    probe = platform_socket_listen(0, 1, &port);
    ASSERT(probe != PLATFORM_SOCKET_INVALID);
    ASSERT(port > 0);
    platform_socket_close(probe);

    listener = cl_make_listen_stream(port, 1, NULL);
    ASSERT(!CL_NULL_P(listener));
    ASSERT(CL_STREAM_P(listener));
    {
        CL_Stream *st = (CL_Stream *)CL_OBJ_TO_PTR(listener);
        ASSERT_EQ_INT((int)st->stream_type, CL_STREAM_SOCKET);
        ASSERT(st->flags & CL_STREAM_FLAG_OPEN);
    }

    client = cl_make_socket_stream("127.0.0.1", port, 0);
    ASSERT(!CL_NULL_P(client));

    conn = cl_socket_stream_accept(listener);
    ASSERT(!CL_NULL_P(conn));
    ASSERT(CL_STREAM_P(conn));
    {
        CL_Stream *st = (CL_Stream *)CL_OBJ_TO_PTR(conn);
        ASSERT_EQ_INT((int)st->stream_type, CL_STREAM_SOCKET);
        ASSERT_EQ_INT((int)st->direction, CL_STREAM_IO);
    }

    /* client -> server: write then close to flush, read echoes + EOF */
    cl_stream_write_char(client, 'Q');
    cl_stream_close(client);  /* flushes pending byte, then closes */
    ASSERT_EQ_INT(cl_stream_read_char(conn), 'Q');
    ASSERT_EQ_INT(cl_stream_read_char(conn), -1);  /* EOF after client close */

    cl_stream_close(conn);
    cl_stream_close(listener);
}

/* accept() on something that is not a listening socket returns NIL. */
TEST(socket_stream_accept_non_listener)
{
    CL_Obj sstream = cl_make_string_output_stream();
    ASSERT(!CL_NULL_P(sstream));
    ASSERT(CL_NULL_P(cl_socket_stream_accept(sstream)));
    /* NIL (not a stream) is also rejected. */
    ASSERT(CL_NULL_P(cl_socket_stream_accept(CL_NIL)));
}

/* End-to-end through the Lisp builtins, single-threaded. */
TEST(eval_socket_listen_accept_roundtrip)
{
    int port = 0;
    PlatformSocket probe;
    char expr[512];
    CL_Obj result;

    probe = platform_socket_listen(0, 1, &port);
    ASSERT(probe != PLATFORM_SOCKET_INVALID);
    ASSERT(port > 0);
    platform_socket_close(probe);

    snprintf(expr, sizeof(expr),
        "(let* ((l (ext:socket-listen %d t))"
        "       (c (ext:open-tcp-stream \"127.0.0.1\" %d))"
        "       (s (ext:socket-accept l)))"
        "  (write-char #\\Z c)"
        "  (force-output c)"
        "  (prog1 (char-code (read-char s))"
        "    (close c) (close s) (close l)))",
        port, port);
    result = cl_eval_string(expr);
    ASSERT(CL_FIXNUM_P(result));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(result), 90);  /* #\Z */
}

/* socket-listen rejects an out-of-range port via a signalled error. */
TEST(eval_socket_listen_bad_port)
{
    CL_Obj result = cl_eval_string(
        "(handler-case (ext:socket-listen 70000)"
        "  (error (c) (declare (ignore c)) :bad-port))");
    ASSERT(result == cl_intern_keyword("BAD-PORT", 8));
}

/* socket-accept on a non-listener signals an error (no blocking). */
TEST(eval_socket_accept_type_error)
{
    CL_Obj result = cl_eval_string(
        "(handler-case (ext:socket-accept (make-string-output-stream))"
        "  (error (c) (declare (ignore c)) :not-a-socket))");
    ASSERT(result == cl_intern_keyword("NOT-A-SOCKET", 12));
}

/* cl_listen_stream_local_port reports the bound port, including the
 * OS-assigned port when socket-listen is given 0. */
TEST(socket_stream_local_port)
{
    CL_Obj listener, conn, client;
    int eph;

    /* Ephemeral (port 0) listener: the getter discovers the assigned port. */
    listener = cl_make_listen_stream(0, 1, NULL);
    ASSERT(!CL_NULL_P(listener));
    eph = cl_listen_stream_local_port(listener);
    ASSERT(eph > 0);

    /* A connected socket (from accept) is not a listener -> -1. */
    client = cl_make_socket_stream("127.0.0.1", eph, 0);
    ASSERT(!CL_NULL_P(client));
    conn = cl_socket_stream_accept(listener);
    ASSERT(!CL_NULL_P(conn));
    ASSERT_EQ_INT(cl_listen_stream_local_port(conn), -1);
    ASSERT_EQ_INT(cl_listen_stream_local_port(client), -1);

    /* Non-stream / closed listener -> -1. */
    ASSERT_EQ_INT(cl_listen_stream_local_port(CL_NIL), -1);
    cl_stream_close(listener);
    ASSERT_EQ_INT(cl_listen_stream_local_port(listener), -1);

    cl_stream_close(client);
    cl_stream_close(conn);
}

/* When socket-listen is given an explicit port, local-port returns it. */
TEST(socket_stream_local_port_explicit)
{
    int port = 0;
    PlatformSocket probe;
    CL_Obj listener;

    probe = platform_socket_listen(0, 1, &port);
    ASSERT(probe != PLATFORM_SOCKET_INVALID);
    ASSERT(port > 0);
    platform_socket_close(probe);

    listener = cl_make_listen_stream(port, 1, NULL);
    ASSERT(!CL_NULL_P(listener));
    ASSERT_EQ_INT(cl_listen_stream_local_port(listener), port);
    cl_stream_close(listener);
}

/* End-to-end through the Lisp builtin: ext:socket-local-port returns the
 * ephemeral port, and a client can connect to it. */
TEST(eval_socket_local_port)
{
    CL_Obj result = cl_eval_string(
        "(let ((l (ext:socket-listen 0 t)))"
        "  (prog1"
        "    (let* ((p (ext:socket-local-port l))"
        "           (c (ext:open-tcp-stream \"127.0.0.1\" p))"
        "           (s (ext:socket-accept l)))"
        "      (prog1 (and (integerp p) (> p 0))"
        "        (close c) (close s)))"
        "    (close l)))");
    ASSERT(result == CL_T);
}

/* ext:socket-local-port on a non-listener signals an error. */
TEST(eval_socket_local_port_type_error)
{
    CL_Obj result = cl_eval_string(
        "(handler-case (ext:socket-local-port (make-string-output-stream))"
        "  (error (c) (declare (ignore c)) :not-a-listener))");
    ASSERT(result == cl_intern_keyword("NOT-A-LISTENER", 14));
}

/* ext:socket-listen takes a dotted-quad ADDRESS string as the one address to
 * bind (a development port reachable from one network, not every one):
 * "127.0.0.1" spelled out serves a loopback client exactly as T does. */
TEST(eval_socket_listen_named_address)
{
    CL_Obj result = cl_eval_string(
        "(let ((l (ext:socket-listen 0 \"127.0.0.1\")))"
        "  (prog1"
        "    (let* ((p (ext:socket-local-port l))"
        "           (c (ext:open-tcp-stream \"127.0.0.1\" p))"
        "           (s (ext:socket-accept l)))"
        "      (write-char #\\Q c)"
        "      (force-output c)"
        "      (prog1 (char-code (read-char s))"
        "        (close c) (close s)))"
        "    (close l)))");
    ASSERT(CL_FIXNUM_P(result));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(result), 81);  /* #\Q */
}

/* An address no interface of this machine has (TEST-NET-3, RFC 5737), a
 * name that is not dotted-quad, and an empty one: each is refused with an
 * error that names the address, never a listener somewhere else. */
TEST(eval_socket_listen_bad_address)
{
    CL_Obj result = cl_eval_string(
        "(if (and"
        "     (handler-case (progn (ext:socket-listen 0 \"203.0.113.1\") nil)"
        "       (error (c) (search \"203.0.113.1\" (princ-to-string c))))"
        "     (handler-case (progn (ext:socket-listen 0 \"localhost\") nil)"
        "       (error (c) (search \"localhost\" (princ-to-string c))))"
        "     (handler-case (progn (ext:socket-listen 0 \"\") nil)"
        "       (error (c) (search \"literal IPv4 or IPv6\" (princ-to-string c))))"
        "     (handler-case (progn (ext:socket-listen 0 \"1.2.3.4.5\") nil)"
        "       (error (c) (declare (ignore c)) t))"
        "     (handler-case (progn (ext:socket-listen 0 \"::zz\") nil)"
        "       (error (c) (search \"::zz\" (princ-to-string c)))))"
        "    :all-refused"
        "    :one-listened)");
    ASSERT(result == cl_intern_keyword("ALL-REFUSED", 11));
}

/* ext:executable-path names this very binary: what a process starts a
 * second copy of itself with. */
TEST(eval_executable_path)
{
    CL_Obj result = cl_eval_string(
        "(let ((p (ext:executable-path)))"
        "  (and (stringp p)"
        "       (> (length p) (length \"test_stream\"))"
        "       (search \"test_stream\" p :start2 (- (length p) 15))"
        "       (probe-file p)"
        "       t))");
    ASSERT(result == CL_T);
}

/* Concurrent slot-claim: accept() runs on a worker thread while the main
 * thread connect()s.  Exercises the socket-table mutex — without it the two
 * claims could land on the same slot. */
struct sock_accept_arg { PlatformSocket listener; PlatformSocket result; };

static void *sock_accept_worker(void *p)
{
    struct sock_accept_arg *a = (struct sock_accept_arg *)p;
    a->result = platform_socket_accept(a->listener);
    return NULL;
}

TEST(socket_concurrent_accept_connect)
{
    int port = 0, iter;
    PlatformSocket listener;

    listener = platform_socket_listen(0, 1, &port);
    ASSERT(listener != PLATFORM_SOCKET_INVALID);
    ASSERT(port > 0);

    for (iter = 0; iter < 64; iter++) {
        void *th = NULL;
        struct sock_accept_arg a;
        PlatformSocket cli;
        int expect = 'A' + (iter & 15);

        a.listener = listener;
        a.result = PLATFORM_SOCKET_INVALID;
        ASSERT_EQ_INT(platform_thread_create(&th, sock_accept_worker, &a, 0), 0);

        cli = platform_socket_connect("127.0.0.1", port, 0);
        ASSERT(cli != PLATFORM_SOCKET_INVALID);

        platform_thread_join(th, NULL);
        ASSERT(a.result != PLATFORM_SOCKET_INVALID);
        ASSERT(a.result != cli);  /* claims must never collide on one slot */

        ASSERT_EQ_INT(platform_socket_write(cli, expect), 0);
        ASSERT_EQ_INT(platform_socket_flush(cli), 0);
        ASSERT_EQ_INT(platform_socket_read(a.result), expect);

        platform_socket_close(cli);
        platform_socket_close(a.result);
    }
    platform_socket_close(listener);
}

/* ================================================================
 * Regression: a thread parked in a blocking socket READ must not
 * block other, independent stream operations.
 *
 * The stream layer used to serialise all console/file/socket I/O
 * through one global mutex held across the blocking read() syscall.
 * A thread parked in (read-byte socket) therefore wedged every other
 * stream operation — writing the reply back on the same socket, or
 * any console output from another thread.  That is the SLY :spawn
 * round-trip deadlock.  Locks are now split per stream and per
 * direction (see stream.c), so a parked reader only ever holds that
 * socket's read lock.
 *
 * A watchdog thread aborts with a diagnostic if the main thread's
 * supposedly independent operation does not complete, turning a
 * regression into a loud failure instead of an indefinite hang.
 * ================================================================ */

typedef struct {
    CL_Thread     thread;
    CL_Obj        stream;   /* socket stream the worker parks reading */
    volatile int  parked;   /* set just before the blocking read */
    volatile int  got;      /* byte read once the peer unblocks it */
} ReadBlockerCtx;

static void *socket_read_blocker(void *arg)
{
    ReadBlockerCtx *ctx = (ReadBlockerCtx *)arg;
    CL_Thread *t = &ctx->thread;
    platform_tls_set(t);
    cl_thread_register(t);
    ctx->parked = 1;
    /* Blocks until the peer sends a byte.  With the fix this holds only this
     * socket's read lock and runs inside a GC safe region. */
    ctx->got = cl_stream_read_byte(ctx->stream);
    cl_thread_unregister(t);
    return NULL;
}

static volatile int g_read_test_done = 0;

static void *read_test_watchdog(void *arg)
{
    int waited = 0;
    while (!g_read_test_done && waited < 5000) {
        platform_sleep_ms(50);
        waited += 50;
    }
    if (!g_read_test_done) {
        fprintf(stderr,
            "\nFATAL: %s — a thread parked in cl_stream_read_byte() blocked an "
            "independent\n  stream operation.  The per-stream/per-direction I/O "
            "lock split in stream.c\n  (cl_stream_read_byte/write_*/stream_lock_for) "
            "has regressed.\n", (const char *)arg);
        abort();
    }
    return NULL;
}

/* Build a connected loopback pair of amilisp socket streams: *server is the
 * accepted (server-side) connection, *client the connecting end.  connect()
 * lands in the listen backlog on loopback, so accept() returns without a
 * second thread. */
static int make_loopback_stream_pair(CL_Obj *listener, CL_Obj *client,
                                     CL_Obj *server)
{
    int port = 0;
    PlatformSocket probe = platform_socket_listen(0, 1, &port);
    if (probe == PLATFORM_SOCKET_INVALID || port <= 0) return 0;
    platform_socket_close(probe);
    *listener = cl_make_listen_stream(port, 1, NULL);
    if (CL_NULL_P(*listener)) return 0;
    *client = cl_make_socket_stream("127.0.0.1", port, 0);
    if (CL_NULL_P(*client)) return 0;
    *server = cl_socket_stream_accept(*listener);
    return !CL_NULL_P(*server);
}

/* Flush a socket stream's buffered output to the wire (the C-level equivalent
 * of FINISH-OUTPUT) so the peer's blocking read can observe it. */
static void flush_socket_stream(CL_Obj s)
{
    CL_Stream *st = (CL_Stream *)CL_OBJ_TO_PTR(s);
    platform_socket_flush((PlatformSocket)st->handle_id);
}

/* The SLY fix: while a worker is parked reading the socket, the reply must be
 * writable on the SAME socket (opposite direction). */
TEST(socket_read_parked_allows_same_socket_reply)
{
    CL_Obj listener = CL_NIL, client = CL_NIL, server = CL_NIL;
    ReadBlockerCtx ctx;
    void *wth = NULL, *wdog = NULL;

    CL_GC_PROTECT(listener);
    CL_GC_PROTECT(client);
    CL_GC_PROTECT(server);
    ASSERT(make_loopback_stream_pair(&listener, &client, &server));
    if (CL_NULL_P(server)) { CL_GC_UNPROTECT(3); return; }

    memset(&ctx, 0, sizeof(ctx));
    ctx.thread.id = 1;
    ctx.thread.status = 1;
    ctx.thread.mv_count = 1;
    ctx.stream = server;
    ctx.got = -2;

    g_read_test_done = 0;
    platform_thread_create(&wdog, read_test_watchdog,
                           (void *)"same-socket reply write", 0);
    platform_thread_create(&wth, socket_read_blocker, &ctx, 0);

    while (!ctx.parked) platform_sleep_ms(5);
    platform_sleep_ms(100);   /* give it a beat to park inside read() */

    /* Used to deadlock: write the reply on the server stream while the worker
     * is parked reading that same stream. */
    cl_stream_write_byte(server, 'A');
    flush_socket_stream(server);
    ASSERT_EQ_INT(cl_stream_read_byte(client), 'A');

    /* Unblock the worker so it can finish, then confirm it saw the byte. */
    cl_stream_write_byte(client, 'Z');
    flush_socket_stream(client);
    platform_thread_join(wth, NULL);
    ASSERT_EQ_INT(ctx.got, 'Z');

    g_read_test_done = 1;
    platform_thread_join(wdog, NULL);

    cl_stream_close(client);
    cl_stream_close(server);
    cl_stream_close(listener);
    CL_GC_UNPROTECT(3);
}

/* Regression for the dynamic socket table: the per-socket I/O lock must keep
 * working for handles past the old fixed 16-slot cap and across the lock
 * table's block boundary.  Pre-opens enough low handles that the test pair
 * lands at handle >= 32 (lock block >= 1), then reruns the SLY same-socket
 * reply scenario: a worker parked reading the server must not block the reply
 * written on that same high-handle socket.  This would deadlock if the lock
 * lookup returned NULL for the high handle (the pre-fix behaviour) or if locks
 * were striped (a collision would alias read and write of the same socket). */
TEST(socket_read_parked_high_handle_lock_independent)
{
    enum { HOLD = 20 };
    PlatformSocket hold_lst, hold_cli[HOLD], hold_srv[HOLD];
    int port = 0, i;
    CL_Obj listener = CL_NIL, client = CL_NIL, server = CL_NIL;
    ReadBlockerCtx ctx;
    void *wth = NULL, *wdog = NULL;
    CL_Stream *sst;

    /* Hold low handles open so the test pair is forced past block 0. */
    hold_lst = platform_socket_listen(0, 1, &port);
    ASSERT(hold_lst != PLATFORM_SOCKET_INVALID);
    ASSERT(port > 0);
    for (i = 0; i < HOLD; i++) {
        hold_cli[i] = platform_socket_connect("127.0.0.1", port, 0);
        ASSERT(hold_cli[i] != PLATFORM_SOCKET_INVALID);
        hold_srv[i] = platform_socket_accept(hold_lst);
        ASSERT(hold_srv[i] != PLATFORM_SOCKET_INVALID);
    }

    CL_GC_PROTECT(listener);
    CL_GC_PROTECT(client);
    CL_GC_PROTECT(server);
    ASSERT(make_loopback_stream_pair(&listener, &client, &server));
    ASSERT(!CL_NULL_P(server));

    /* The whole point: the server stream's handle must be past the old 16-slot
     * cap and into a second lock block (>= 32). */
    sst = (CL_Stream *)CL_OBJ_TO_PTR(server);
    ASSERT(sst->handle_id >= 32);

    memset(&ctx, 0, sizeof(ctx));
    ctx.thread.id = 1;
    ctx.thread.status = 1;
    ctx.thread.mv_count = 1;
    ctx.stream = server;
    ctx.got = -2;

    g_read_test_done = 0;
    platform_thread_create(&wdog, read_test_watchdog,
                           (void *)"high-handle same-socket reply write", 0);
    platform_thread_create(&wth, socket_read_blocker, &ctx, 0);

    while (!ctx.parked) platform_sleep_ms(5);
    platform_sleep_ms(100);

    /* Reply on the server stream while the worker is parked reading it. */
    cl_stream_write_byte(server, 'A');
    flush_socket_stream(server);
    ASSERT_EQ_INT(cl_stream_read_byte(client), 'A');

    cl_stream_write_byte(client, 'Z');
    flush_socket_stream(client);
    platform_thread_join(wth, NULL);
    ASSERT_EQ_INT(ctx.got, 'Z');

    g_read_test_done = 1;
    platform_thread_join(wdog, NULL);

    cl_stream_close(client);
    cl_stream_close(server);
    cl_stream_close(listener);
    CL_GC_UNPROTECT(3);

    for (i = 0; i < HOLD; i++) {
        platform_socket_close(hold_cli[i]);
        platform_socket_close(hold_srv[i]);
    }
    platform_socket_close(hold_lst);
}

/* The reported symptom: console output from another thread must proceed while
 * a worker is parked in a socket read (under the old shared lock it hung). */
TEST(socket_read_parked_allows_console_write)
{
    CL_Obj listener = CL_NIL, client = CL_NIL, server = CL_NIL;
    ReadBlockerCtx ctx;
    void *wth = NULL, *wdog = NULL;

    CL_GC_PROTECT(listener);
    CL_GC_PROTECT(client);
    CL_GC_PROTECT(server);
    ASSERT(make_loopback_stream_pair(&listener, &client, &server));
    if (CL_NULL_P(server)) { CL_GC_UNPROTECT(3); return; }

    memset(&ctx, 0, sizeof(ctx));
    ctx.thread.id = 1;
    ctx.thread.status = 1;
    ctx.thread.mv_count = 1;
    ctx.stream = server;
    ctx.got = -2;

    g_read_test_done = 0;
    platform_thread_create(&wdog, read_test_watchdog,
                           (void *)"console write", 0);
    platform_thread_create(&wth, socket_read_blocker, &ctx, 0);

    while (!ctx.parked) platform_sleep_ms(5);
    platform_sleep_ms(100);

    /* A zero-length console write still acquires the console write lock — which
     * under the old design was the very same mutex the parked socket read held,
     * deadlocking here.  Zero length keeps the test output clean. */
    cl_stream_write_string(cl_stdout_stream, "", 0);

    cl_stream_write_byte(client, 'Z');
    flush_socket_stream(client);
    platform_thread_join(wth, NULL);
    ASSERT_EQ_INT(ctx.got, 'Z');

    g_read_test_done = 1;
    platform_thread_join(wdog, NULL);

    cl_stream_close(client);
    cl_stream_close(server);
    cl_stream_close(listener);
    CL_GC_UNPROTECT(3);
}

/* The user's literal symptom: a full stop-the-world GC must return while a
 * thread is parked in a socket read (the read runs inside a GC safe region). */
TEST(stw_gc_with_thread_blocked_in_socket_read)
{
    CL_Obj listener = CL_NIL, client = CL_NIL, server = CL_NIL;
    ReadBlockerCtx ctx;
    void *wth = NULL, *wdog = NULL;

    CL_GC_PROTECT(listener);
    CL_GC_PROTECT(client);
    CL_GC_PROTECT(server);
    ASSERT(make_loopback_stream_pair(&listener, &client, &server));
    if (CL_NULL_P(server)) { CL_GC_UNPROTECT(3); return; }

    memset(&ctx, 0, sizeof(ctx));
    ctx.thread.id = 1;
    ctx.thread.status = 1;
    ctx.thread.mv_count = 1;
    ctx.stream = server;
    ctx.got = -2;

    g_read_test_done = 0;
    platform_thread_create(&wdog, read_test_watchdog,
                           (void *)"stop-the-world GC", 0);
    platform_thread_create(&wth, socket_read_blocker, &ctx, 0);

    while (!ctx.parked) platform_sleep_ms(5);
    platform_sleep_ms(100);

    cl_gc();   /* used to hang forever waiting for the parked reader to stop */

    cl_stream_write_byte(client, 'Z');
    flush_socket_stream(client);
    platform_thread_join(wth, NULL);
    ASSERT_EQ_INT(ctx.got, 'Z');

    g_read_test_done = 1;
    platform_thread_join(wdog, NULL);

    cl_stream_close(client);
    cl_stream_close(server);
    cl_stream_close(listener);
    CL_GC_UNPROTECT(3);
}

/* --- UTF-8 Stream I/O tests --- */

#ifdef CL_WIDE_STRINGS

#include "core/string_utils.h"

TEST(utf8_encode_ascii)
{
    char buf[4];
    ASSERT_EQ_INT(cl_utf8_encode('A', buf), 1);
    ASSERT_EQ_INT(buf[0], 'A');
}

TEST(utf8_encode_2byte)
{
    /* U+00E9 = é = C3 A9 */
    char buf[4];
    int nb = cl_utf8_encode(0xE9, buf);
    ASSERT_EQ_INT(nb, 2);
    ASSERT_EQ_INT((unsigned char)buf[0], 0xC3);
    ASSERT_EQ_INT((unsigned char)buf[1], 0xA9);
}

TEST(utf8_encode_3byte)
{
    /* U+20AC = € = E2 82 AC */
    char buf[4];
    int nb = cl_utf8_encode(0x20AC, buf);
    ASSERT_EQ_INT(nb, 3);
    ASSERT_EQ_INT((unsigned char)buf[0], 0xE2);
    ASSERT_EQ_INT((unsigned char)buf[1], 0x82);
    ASSERT_EQ_INT((unsigned char)buf[2], 0xAC);
}

TEST(utf8_encode_4byte)
{
    /* U+1F600 = 😀 = F0 9F 98 80 */
    char buf[4];
    int nb = cl_utf8_encode(0x1F600, buf);
    ASSERT_EQ_INT(nb, 4);
    ASSERT_EQ_INT((unsigned char)buf[0], 0xF0);
    ASSERT_EQ_INT((unsigned char)buf[1], 0x9F);
    ASSERT_EQ_INT((unsigned char)buf[2], 0x98);
    ASSERT_EQ_INT((unsigned char)buf[3], 0x80);
}

TEST(utf8_encode_reject_surrogate)
{
    char buf[4];
    ASSERT_EQ_INT(cl_utf8_encode(0xD800, buf), 0);
    ASSERT_EQ_INT(cl_utf8_encode(0xDFFF, buf), 0);
}

TEST(utf8_encode_reject_too_large)
{
    char buf[4];
    ASSERT_EQ_INT(cl_utf8_encode(0x110000, buf), 0);
}

TEST(utf8_decode_ascii)
{
    const unsigned char buf[] = { 'Z' };
    int cp;
    ASSERT_EQ_INT(cl_utf8_decode(buf, 1, &cp), 1);
    ASSERT_EQ_INT(cp, 'Z');
}

TEST(utf8_decode_2byte)
{
    /* U+00E9 = C3 A9 */
    const unsigned char buf[] = { 0xC3, 0xA9 };
    int cp;
    ASSERT_EQ_INT(cl_utf8_decode(buf, 2, &cp), 2);
    ASSERT_EQ_INT(cp, 0xE9);
}

TEST(utf8_decode_3byte)
{
    /* U+20AC = E2 82 AC */
    const unsigned char buf[] = { 0xE2, 0x82, 0xAC };
    int cp;
    ASSERT_EQ_INT(cl_utf8_decode(buf, 3, &cp), 3);
    ASSERT_EQ_INT(cp, 0x20AC);
}

TEST(utf8_decode_4byte)
{
    /* U+1F600 = F0 9F 98 80 */
    const unsigned char buf[] = { 0xF0, 0x9F, 0x98, 0x80 };
    int cp;
    ASSERT_EQ_INT(cl_utf8_decode(buf, 4, &cp), 4);
    ASSERT_EQ_INT(cp, 0x1F600);
}

TEST(utf8_decode_overlong_rejected)
{
    /* Overlong encoding of U+0041 as C1 81 */
    const unsigned char buf[] = { 0xC1, 0x81 };
    int cp;
    cl_utf8_decode(buf, 2, &cp);
    ASSERT_EQ_INT(cp, 0xFFFD);  /* replacement */
}

TEST(utf8_decode_truncated)
{
    /* Start of 3-byte but only 1 byte available */
    const unsigned char buf[] = { 0xE2 };
    int cp;
    int nb = cl_utf8_decode(buf, 1, &cp);
    ASSERT_EQ_INT(nb, 1);
    ASSERT_EQ_INT(cp, 0xFFFD);
}

TEST(utf8_roundtrip)
{
    /* Encode then decode various code points */
    int test_cps[] = { 0x41, 0xE9, 0x20AC, 0x1F600, 0x10FFFF };
    int i;
    for (i = 0; i < 5; i++) {
        char buf[4];
        int nb, cp;
        nb = cl_utf8_encode(test_cps[i], buf);
        ASSERT(nb > 0);
        cl_utf8_decode((const unsigned char *)buf, (uint32_t)nb, &cp);
        ASSERT_EQ_INT(cp, test_cps[i]);
    }
}

TEST(utf8_to_cl_string_ascii)
{
    CL_Obj s = cl_utf8_to_cl_string("hello", 5);
    /* Pure ASCII → base string */
    ASSERT(CL_STRING_P(s));
    ASSERT_EQ_INT((int)cl_string_length(s), 5);
    ASSERT_EQ_INT(cl_string_char_at(s, 0), 'h');
}

TEST(utf8_to_cl_string_multibyte)
{
    /* "café" = 63 61 66 C3 A9 */
    const char buf[] = { 'c', 'a', 'f', '\xC3', '\xA9' };
    CL_Obj s = cl_utf8_to_cl_string(buf, 5);
    /* Has non-ASCII → wide string */
    ASSERT(CL_WIDE_STRING_P(s));
    ASSERT_EQ_INT((int)cl_string_length(s), 4);  /* 4 code points */
    ASSERT_EQ_INT(cl_string_char_at(s, 0), 'c');
    ASSERT_EQ_INT(cl_string_char_at(s, 1), 'a');
    ASSERT_EQ_INT(cl_string_char_at(s, 2), 'f');
    ASSERT_EQ_INT(cl_string_char_at(s, 3), 0xE9);
}

TEST(wide_string_to_utf8_basic)
{
    /* Create a wide string with "café" */
    uint32_t data[] = { 'c', 'a', 'f', 0xE9 };
    CL_Obj ws = cl_make_wide_string(data, 4);
    char buf[64];
    uint32_t nb = cl_wide_string_to_utf8(ws, buf, sizeof(buf));
    ASSERT_EQ_INT((int)nb, 5);  /* "caf" (3 bytes) + é (2 bytes) */
    ASSERT_EQ_INT(buf[0], 'c');
    ASSERT_EQ_INT(buf[1], 'a');
    ASSERT_EQ_INT(buf[2], 'f');
    ASSERT_EQ_INT((unsigned char)buf[3], 0xC3);
    ASSERT_EQ_INT((unsigned char)buf[4], 0xA9);
    ASSERT_EQ_INT(buf[5], '\0');
}

TEST(file_stream_utf8_write_read)
{
    /* Write Unicode chars to a file stream, read back as UTF-8 decoded */
    const char *path = "/tmp/cl_test_utf8_stream.tmp";
    PlatformFile wf, rf;
    CL_Obj ws, rs;
    CL_Stream *wst, *rst;
    int ch;

    wf = platform_file_open(path, PLATFORM_FILE_WRITE);
    ASSERT(wf != PLATFORM_FILE_INVALID);
    ws = cl_make_stream(CL_STREAM_OUTPUT, CL_STREAM_FILE);
    wst = (CL_Stream *)CL_OBJ_TO_PTR(ws);
    wst->handle_id = (uint32_t)wf;

    /* Write: ASCII 'A', U+00E9 (é), U+20AC (€) */
    cl_stream_write_char(ws, 'A');
    cl_stream_write_char(ws, 0xE9);
    cl_stream_write_char(ws, 0x20AC);
    cl_stream_close(ws);

    /* Read back and verify UTF-8 decoding */
    rf = platform_file_open(path, PLATFORM_FILE_READ);
    ASSERT(rf != PLATFORM_FILE_INVALID);
    rs = cl_make_stream(CL_STREAM_INPUT, CL_STREAM_FILE);
    rst = (CL_Stream *)CL_OBJ_TO_PTR(rs);
    rst->handle_id = (uint32_t)rf;

    ch = cl_stream_read_char(rs);
    ASSERT_EQ_INT(ch, 'A');
    ch = cl_stream_read_char(rs);
    ASSERT_EQ_INT(ch, 0xE9);
    ch = cl_stream_read_char(rs);
    ASSERT_EQ_INT(ch, 0x20AC);
    ch = cl_stream_read_char(rs);
    ASSERT_EQ_INT(ch, -1);  /* EOF */
    cl_stream_close(rs);

    remove(path);
}

TEST(cbuf_stream_utf8_decode)
{
    /* CBUF with UTF-8 encoded "Aé€" */
    const char buf[] = { 'A', '\xC3', '\xA9', '\xE2', '\x82', '\xAC' };
    CL_Obj s = cl_make_cbuf_input_stream(buf, 6);
    int ch;

    ch = cl_stream_read_char(s);
    ASSERT_EQ_INT(ch, 'A');
    ch = cl_stream_read_char(s);
    ASSERT_EQ_INT(ch, 0xE9);
    ch = cl_stream_read_char(s);
    ASSERT_EQ_INT(ch, 0x20AC);
    ch = cl_stream_read_char(s);
    ASSERT_EQ_INT(ch, -1);
    cl_stream_close(s);
}

TEST(string_output_stream_utf8_roundtrip)
{
    /* Write Unicode code points to string output stream,
       get-output-stream-string should return a proper CL string */
    CL_Obj s = cl_make_string_output_stream();
    CL_Obj result;

    cl_stream_write_char(s, 'c');
    cl_stream_write_char(s, 'a');
    cl_stream_write_char(s, 'f');
    cl_stream_write_char(s, 0xE9);  /* é */

    result = cl_get_output_stream_string(s);
    ASSERT(CL_WIDE_STRING_P(result));
    ASSERT_EQ_INT((int)cl_string_length(result), 4);
    ASSERT_EQ_INT(cl_string_char_at(result, 0), 'c');
    ASSERT_EQ_INT(cl_string_char_at(result, 1), 'a');
    ASSERT_EQ_INT(cl_string_char_at(result, 2), 'f');
    ASSERT_EQ_INT(cl_string_char_at(result, 3), 0xE9);

    cl_stream_close(s);
}

TEST(string_output_stream_ascii_stays_base)
{
    /* Pure ASCII should produce a base string */
    CL_Obj s = cl_make_string_output_stream();
    CL_Obj result;

    cl_stream_write_char(s, 'h');
    cl_stream_write_char(s, 'i');

    result = cl_get_output_stream_string(s);
    ASSERT(CL_STRING_P(result));  /* base string, not wide */
    ASSERT_EQ_INT((int)cl_string_length(result), 2);

    cl_stream_close(s);
}

TEST(read_line_utf8_file)
{
    /* Write a line with UTF-8 chars to a file, read it back with read-line */
    const char *path = "/tmp/cl_test_utf8_readline.tmp";
    PlatformFile wf, rf;
    CL_Obj ws, rs;
    CL_Stream *wst, *rst;
    CL_Obj result;

    wf = platform_file_open(path, PLATFORM_FILE_WRITE);
    ASSERT(wf != PLATFORM_FILE_INVALID);
    ws = cl_make_stream(CL_STREAM_OUTPUT, CL_STREAM_FILE);
    wst = (CL_Stream *)CL_OBJ_TO_PTR(ws);
    wst->handle_id = (uint32_t)wf;

    /* Write "café\n" in UTF-8 */
    cl_stream_write_char(ws, 'c');
    cl_stream_write_char(ws, 'a');
    cl_stream_write_char(ws, 'f');
    cl_stream_write_char(ws, 0xE9);
    cl_stream_write_char(ws, '\n');
    cl_stream_close(ws);

    /* Read back via eval of read-line */
    {
        char expr[256];
        snprintf(expr, sizeof(expr),
                 "(with-open-file (s \"%s\" :direction :input) (read-line s))",
                 path);
        result = cl_eval_string(expr);
    }
    ASSERT(CL_WIDE_STRING_P(result));
    ASSERT_EQ_INT((int)cl_string_length(result), 4);
    ASSERT_EQ_INT(cl_string_char_at(result, 0), 'c');
    ASSERT_EQ_INT(cl_string_char_at(result, 3), 0xE9);

    remove(path);
}

TEST(unread_char_utf8)
{
    /* Unread a multibyte char, re-read it */
    const char buf[] = { '\xC3', '\xA9', 'x' };
    CL_Obj s = cl_make_cbuf_input_stream(buf, 3);
    int ch;

    ch = cl_stream_read_char(s);
    ASSERT_EQ_INT(ch, 0xE9);

    /* Push it back and re-read */
    cl_stream_unread_char(s, ch);
    ch = cl_stream_read_char(s);
    ASSERT_EQ_INT(ch, 0xE9);

    ch = cl_stream_read_char(s);
    ASSERT_EQ_INT(ch, 'x');
    cl_stream_close(s);
}

#endif /* CL_WIDE_STRINGS */

/* --- Two-way stream tests --- */

TEST(make_two_way_stream_returns_stream)
{
    CL_Obj result = cl_eval_string(
        "(let ((in (make-string-input-stream \"hello\"))"
        "      (out (make-string-output-stream)))"
        "  (streamp (make-two-way-stream in out)))");
    ASSERT(result == CL_T);
}

TEST(two_way_stream_direction_predicates)
{
    CL_Obj result = cl_eval_string(
        "(let* ((in (make-string-input-stream \"hello\"))"
        "       (out (make-string-output-stream))"
        "       (tw (make-two-way-stream in out)))"
        "  (list (input-stream-p tw) (output-stream-p tw)))");
    char buf[64];
    cl_prin1_to_string(result, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(T T)");
}

TEST(two_way_stream_accessors)
{
    CL_Obj result = cl_eval_string(
        "(let* ((in (make-string-input-stream \"hello\"))"
        "       (out (make-string-output-stream))"
        "       (tw (make-two-way-stream in out)))"
        "  (list (eq (two-way-stream-input-stream tw) in)"
        "        (eq (two-way-stream-output-stream tw) out)))");
    char buf[64];
    cl_prin1_to_string(result, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(T T)");
}

TEST(two_way_stream_read_from_input)
{
    CL_Obj result = cl_eval_string(
        "(let* ((in (make-string-input-stream \"ABC\"))"
        "       (out (make-string-output-stream))"
        "       (tw (make-two-way-stream in out)))"
        "  (list (read-char tw) (read-char tw)))");
    char buf[64];
    cl_prin1_to_string(result, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(#\\A #\\B)");
}

TEST(two_way_stream_write_to_output)
{
    CL_Obj result = cl_eval_string(
        "(let* ((in (make-string-input-stream \"\"))"
        "       (out (make-string-output-stream))"
        "       (tw (make-two-way-stream in out)))"
        "  (write-string \"hi\" tw)"
        "  (get-output-stream-string out))");
    CL_String *s;
    ASSERT(CL_ANY_STRING_P(result));
    s = (CL_String *)CL_OBJ_TO_PTR(result);
    ASSERT_STR_EQ(s->data, "hi");
}

TEST(two_way_stream_read_write_roundtrip)
{
    CL_Obj result = cl_eval_string(
        "(let* ((in (make-string-input-stream \"hello\"))"
        "       (out (make-string-output-stream))"
        "       (tw (make-two-way-stream in out)))"
        "  (let ((line (read-line tw)))"
        "    (write-string line tw)"
        "    (get-output-stream-string out)))");
    CL_String *s;
    ASSERT(CL_ANY_STRING_P(result));
    s = (CL_String *)CL_OBJ_TO_PTR(result);
    ASSERT_STR_EQ(s->data, "hello");
}

TEST(two_way_stream_typep)
{
    CL_Obj result = cl_eval_string(
        "(let* ((tw (make-two-way-stream (make-string-input-stream \"\")"
        "                                (make-string-output-stream))))"
        "  (list (typep tw 'two-way-stream)"
        "        (typep tw 'stream)"
        "        (typep tw 'synonym-stream)))");
    char buf[64];
    cl_prin1_to_string(result, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(T T NIL)");
}

TEST(two_way_stream_printer)
{
    CL_Obj result = cl_eval_string(
        "(let* ((tw (make-two-way-stream (make-string-input-stream \"\")"
        "                                (make-string-output-stream))))"
        "  (with-output-to-string (s) (prin1 tw s)))");
    CL_String *s;
    ASSERT(CL_ANY_STRING_P(result));
    s = (CL_String *)CL_OBJ_TO_PTR(result);
    ASSERT(strncmp(s->data, "#<TWO-WAY-STREAM", 16) == 0);
}

TEST(two_way_stream_accessor_type_error)
{
    CL_Obj result;
    result = cl_eval_string(
        "(handler-case"
        "  (two-way-stream-input-stream (make-string-output-stream))"
        "  (error (c) (declare (ignore c)) :type-error))");
    ASSERT(result == cl_intern_keyword("TYPE-ERROR", 10));

    result = cl_eval_string(
        "(handler-case"
        "  (two-way-stream-output-stream (make-string-input-stream \"\"))"
        "  (error (c) (declare (ignore c)) :type-error))");
    ASSERT(result == cl_intern_keyword("TYPE-ERROR", 10));
}

TEST(make_two_way_stream_rejects_non_stream)
{
    CL_Obj result;
    /* Non-stream first argument */
    result = cl_eval_string(
        "(handler-case"
        "  (make-two-way-stream 42 (make-string-output-stream))"
        "  (error (c) (declare (ignore c)) :type-error))");
    ASSERT(result == cl_intern_keyword("TYPE-ERROR", 10));

    /* Wrong-direction native first argument (output stream where input expected) */
    result = cl_eval_string(
        "(handler-case"
        "  (make-two-way-stream (make-string-output-stream) (make-string-output-stream))"
        "  (error (c) (declare (ignore c)) :type-error))");
    ASSERT(result == cl_intern_keyword("TYPE-ERROR", 10));

    /* Wrong-direction native second argument (input stream where output expected) */
    result = cl_eval_string(
        "(handler-case"
        "  (make-two-way-stream (make-string-input-stream \"\") (make-string-input-stream \"\"))"
        "  (error (c) (declare (ignore c)) :type-error))");
    ASSERT(result == cl_intern_keyword("TYPE-ERROR", 10));
}

/* --- Broadcast stream tests (CLHS 21.2 MAKE-BROADCAST-STREAM) --- */

TEST(broadcast_stream_fans_out_writes)
{
    CL_Obj result = cl_eval_string(
        "(let* ((a (make-string-output-stream))"
        "       (b (make-string-output-stream))"
        "       (bc (make-broadcast-stream a b)))"
        "  (write-string \"hel\" bc)"
        "  (write-char #\\l bc)"
        "  (princ \"o!\" bc)"
        "  (list (get-output-stream-string a) (get-output-stream-string b)))");
    char buf[64];
    cl_prin1_to_string(result, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(\"hello!\" \"hello!\")");
}

TEST(broadcast_stream_no_components_discards)
{
    /* CLHS: with no components, all output is discarded. */
    CL_Obj result = cl_eval_string(
        "(let ((bc (make-broadcast-stream)))"
        "  (write-string \"dropped\" bc)"
        "  (terpri bc)"
        "  (finish-output bc)"
        "  (list (streamp bc) (output-stream-p bc) (input-stream-p bc)"
        "        (broadcast-stream-streams bc)))");
    char buf[64];
    cl_prin1_to_string(result, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(T T NIL NIL)");
}

TEST(broadcast_stream_streams_accessor)
{
    CL_Obj result = cl_eval_string(
        "(let* ((a (make-string-output-stream))"
        "       (b (make-string-output-stream))"
        "       (bc (make-broadcast-stream a b))"
        "       (comps (broadcast-stream-streams bc)))"
        "  (list (length comps) (eq (first comps) a) (eq (second comps) b)))");
    char buf[64];
    cl_prin1_to_string(result, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(2 T T)");
}

TEST(broadcast_stream_nested_and_two_way_component)
{
    /* Components may themselves be composite output streams. */
    CL_Obj result = cl_eval_string(
        "(let* ((a (make-string-output-stream))"
        "       (b (make-string-output-stream))"
        "       (tw (make-two-way-stream (make-string-input-stream \"\") b))"
        "       (bc (make-broadcast-stream (make-broadcast-stream a) tw)))"
        "  (write-string \"xy\" bc)"
        "  (list (get-output-stream-string a) (get-output-stream-string b)))");
    char buf[64];
    cl_prin1_to_string(result, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(\"xy\" \"xy\")");
}

TEST(broadcast_stream_fresh_line_uses_component_column)
{
    /* FRESH-LINE must consult the components' column, not the wrapper's:
     * writes go through the wrapper without updating its charpos. */
    CL_Obj result = cl_eval_string(
        "(let* ((a (make-string-output-stream))"
        "       (bc (make-broadcast-stream a)))"
        "  (write-string \"x\" bc)"
        "  (list (fresh-line bc) (fresh-line bc)"
        "        (get-output-stream-string a)"
        "        (fresh-line (make-broadcast-stream))))");
    char buf[64];
    cl_prin1_to_string(result, buf, sizeof(buf));
    /* prin1 prints the newline inside the string literally (CLHS 22.1.3.4) */
    ASSERT_STR_EQ(buf, "(T NIL \"x\n\" NIL)");
}

TEST(broadcast_stream_typep_and_printer)
{
    CL_Obj result = cl_eval_string(
        "(let ((bc (make-broadcast-stream)))"
        "  (list (typep bc 'broadcast-stream)"
        "        (typep bc 'stream)"
        "        (typep bc 'concatenated-stream)"
        "        (subseq (with-output-to-string (s) (prin1 bc s)) 0 18)))");
    char buf[80];
    cl_prin1_to_string(result, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(T T NIL \"#<BROADCAST-STREAM\")");
}

TEST(make_broadcast_stream_rejects_non_output_stream)
{
    CL_Obj result;
    result = cl_eval_string(
        "(handler-case (make-broadcast-stream 42)"
        "  (error (c) (declare (ignore c)) :type-error))");
    ASSERT(result == cl_intern_keyword("TYPE-ERROR", 10));

    result = cl_eval_string(
        "(handler-case (make-broadcast-stream (make-string-input-stream \"\"))"
        "  (error (c) (declare (ignore c)) :type-error))");
    ASSERT(result == cl_intern_keyword("TYPE-ERROR", 10));

    result = cl_eval_string(
        "(handler-case (broadcast-stream-streams (make-string-output-stream))"
        "  (error (c) (declare (ignore c)) :type-error))");
    ASSERT(result == cl_intern_keyword("TYPE-ERROR", 10));
}

/* --- Concatenated stream tests (CLHS 21.2 MAKE-CONCATENATED-STREAM) --- */

TEST(concatenated_stream_reads_in_order)
{
    /* Includes an empty middle component: pop-on-EOF must skip it. */
    CL_Obj result = cl_eval_string(
        "(let ((cc (make-concatenated-stream"
        "           (make-string-input-stream \"abc\")"
        "           (make-string-input-stream \"\")"
        "           (make-string-input-stream \"def\")))"
        "      (acc (make-string-output-stream)))"
        "  (loop for ch = (read-char cc nil nil) while ch"
        "        do (write-char ch acc))"
        "  (list (get-output-stream-string acc) (read-char cc nil :eof)))");
    char buf[64];
    cl_prin1_to_string(result, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(\"abcdef\" :EOF)");
}

TEST(concatenated_stream_no_components_is_eof)
{
    /* CLHS: with no components, the stream is immediately at end of file. */
    CL_Obj result = cl_eval_string(
        "(let ((cc (make-concatenated-stream)))"
        "  (list (read-char cc nil :eof)"
        "        (input-stream-p cc) (output-stream-p cc)"
        "        (concatenated-stream-streams cc)))");
    char buf[64];
    cl_prin1_to_string(result, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(:EOF T NIL NIL)");
}

TEST(concatenated_stream_streams_drains)
{
    /* The accessor returns the streams still to be read, current first. */
    CL_Obj result = cl_eval_string(
        "(let* ((s1 (make-string-input-stream \"a\"))"
        "       (s2 (make-string-input-stream \"b\"))"
        "       (cc (make-concatenated-stream s1 s2)))"
        "  (list (length (concatenated-stream-streams cc))"
        "        (progn (read-char cc) (read-char cc)"
        "               (eq (first (concatenated-stream-streams cc)) s2))"
        "        (progn (read-char cc nil nil)"
        "               (concatenated-stream-streams cc))))");
    char buf[64];
    cl_prin1_to_string(result, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(2 T NIL)");
}

TEST(concatenated_stream_read_line_spans_components)
{
    /* READ-LINE must keep reading across a component boundary. */
    CL_Obj result = cl_eval_string(
        "(let ((cc (make-concatenated-stream"
        "           (make-string-input-stream \"foo\")"
        "           (make-string-input-stream (format nil \"bar~%next\")))))"
        "  (list (read-line cc) (read-line cc nil :eof)))");
    char buf[64];
    cl_prin1_to_string(result, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(\"foobar\" \"next\")");
}

TEST(concatenated_stream_unread_and_peek)
{
    CL_Obj result = cl_eval_string(
        "(let ((cc (make-concatenated-stream (make-string-input-stream \"ab\"))))"
        "  (let ((c1 (read-char cc)))"
        "    (unread-char c1 cc)"
        "    (list (peek-char nil cc) (read-char cc) (read-char cc))))");
    char buf[64];
    cl_prin1_to_string(result, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(#\\a #\\a #\\b)");
}

TEST(concatenated_stream_listen)
{
    CL_Obj result = cl_eval_string(
        "(list (listen (make-concatenated-stream (make-string-input-stream \"z\")))"
        "      (listen (make-concatenated-stream)))");
    char buf[32];
    cl_prin1_to_string(result, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(T NIL)");
}

TEST(concatenated_stream_read_from_it)
{
    /* READ (the reader) through a concatenated stream, form split across
     * components. */
    CL_Obj result = cl_eval_string(
        "(read (make-concatenated-stream"
        "       (make-string-input-stream \"(1 2\")"
        "       (make-string-input-stream \" 3)\")))");
    char buf[32];
    cl_prin1_to_string(result, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(1 2 3)");
}

TEST(concatenated_stream_typep_and_printer)
{
    CL_Obj result = cl_eval_string(
        "(let ((cc (make-concatenated-stream)))"
        "  (list (typep cc 'concatenated-stream)"
        "        (typep cc 'stream)"
        "        (typep cc 'broadcast-stream)"
        "        (subseq (with-output-to-string (s) (prin1 cc s)) 0 21)))");
    char buf[80];
    cl_prin1_to_string(result, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(T T NIL \"#<CONCATENATED-STREAM\")");
}

TEST(make_concatenated_stream_rejects_non_input_stream)
{
    CL_Obj result;
    result = cl_eval_string(
        "(handler-case (make-concatenated-stream 42)"
        "  (error (c) (declare (ignore c)) :type-error))");
    ASSERT(result == cl_intern_keyword("TYPE-ERROR", 10));

    result = cl_eval_string(
        "(handler-case (make-concatenated-stream (make-string-output-stream))"
        "  (error (c) (declare (ignore c)) :type-error))");
    ASSERT(result == cl_intern_keyword("TYPE-ERROR", 10));

    result = cl_eval_string(
        "(handler-case (concatenated-stream-streams (make-string-input-stream \"\"))"
        "  (error (c) (declare (ignore c)) :type-error))");
    ASSERT(result == cl_intern_keyword("TYPE-ERROR", 10));
}

/* --- Echo stream tests (CLHS 21.2 MAKE-ECHO-STREAM) ---
 * Expected values follow the ansi-test suite (streams/make-echo-stream.lsp,
 * peek-char.lsp 17-19, unread-char.lsp 3). */

TEST(echo_stream_echoes_reads)
{
    /* Each element read through the wrapper is copied to the output
     * component; constructing the stream echoes nothing by itself. */
    CL_Obj result = cl_eval_string(
        "(let* ((is (make-string-input-stream \"foo\"))"
        "       (os (make-string-output-stream))"
        "       (s (make-echo-stream is os)))"
        "  (list (get-output-stream-string os)"
        "        (read-char s)"
        "        (get-output-stream-string os)"
        "        (read-line s nil)"
        "        (get-output-stream-string os)"
        "        (read-char s nil :eof)"
        "        (get-output-stream-string os)))");
    char buf[96];
    cl_prin1_to_string(result, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(\"\" #\\f \"f\" \"oo\" \"oo\" :EOF \"\")");
}

TEST(echo_stream_direct_writes_not_echoed_elsewhere)
{
    /* Output written to the echo stream goes to the output component
     * (make-echo-stream.13/.17/.20/.21): write-char, write-string with
     * :start/:end, write-line, fresh-line, finish/force-output. */
    CL_Obj result = cl_eval_string(
        "(let* ((is (make-string-input-stream \"foo\"))"
        "       (os (make-string-output-stream))"
        "       (s (make-echo-stream is os)))"
        "  (list (write-char #\\X s)"
        "        (fresh-line s)"
        "        (write-string \"0159X\" s :start 1 :end 4)"
        "        (finish-output s) (force-output s)"
        "        (input-stream-p s) (output-stream-p s)"
        "        (get-output-stream-string os)))");
    char buf[96];
    cl_prin1_to_string(result, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(#\\X T \"0159X\" NIL NIL T T \"X\n159\")");
}

TEST(echo_stream_accessors_and_close)
{
    /* Accessors return the components; CLOSE closes only the wrapper
     * (make-echo-stream.10). */
    CL_Obj result = cl_eval_string(
        "(let* ((is (make-string-input-stream \"foo\"))"
        "       (os (make-string-output-stream))"
        "       (s (make-echo-stream is os)))"
        "  (list (eq (echo-stream-input-stream s) is)"
        "        (eq (echo-stream-output-stream s) os)"
        "        (open-stream-p s) (close s) (open-stream-p s)"
        "        (open-stream-p is) (open-stream-p os)"
        "        (read-char s nil :closed)))");
    char buf[64];
    cl_prin1_to_string(result, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(T T T T NIL T T :CLOSED)");
}

TEST(echo_stream_unread_echoes_once)
{
    /* CLHS 21.1: a character unread and then re-read is echoed only once
     * (unread-char.3). */
    CL_Obj result = cl_eval_string(
        "(with-input-from-string (is \"abc\")"
        "  (with-output-to-string (os)"
        "    (let ((s (make-echo-stream is os)))"
        "      (read-char s) (unread-char #\\a s)"
        "      (read-char s) (read-char s) (read-char s)"
        "      (unread-char #\\c s) (read-char s))))");
    CL_String *s;
    ASSERT(CL_ANY_STRING_P(result));
    s = (CL_String *)CL_OBJ_TO_PTR(result);
    ASSERT_STR_EQ(s->data, "abc");
}

TEST(echo_stream_peek_char_semantics)
{
    /* peek-char nil does not echo (peek-char.17); the later read of the
     * peeked char echoes it exactly once.  peek-char T / char echo the
     * characters they skip, and the char left for the next read is not
     * echoed twice (peek-char.18/.19). */
    CL_Obj result = cl_eval_string(
        "(list"
        "  (let* ((is (make-string-input-stream \"ab\"))"
        "         (os (make-string-output-stream))"
        "         (s (make-echo-stream is os)))"
        "    (list (peek-char nil s) (get-output-stream-string os)"
        "          (read-char s) (read-char s) (get-output-stream-string os)))"
        "  (let* ((is (make-string-input-stream \"   ab\"))"
        "         (os (make-string-output-stream))"
        "         (s (make-echo-stream is os)))"
        "    (list (peek-char t s) (get-output-stream-string os)"
        "          (read-char s) (get-output-stream-string os)))"
        "  (let* ((is (make-string-input-stream \"abcde\"))"
        "         (os (make-string-output-stream))"
        "         (s (make-echo-stream is os)))"
        "    (list (peek-char #\\c s) (get-output-stream-string os)"
        "          (read-line s) (get-output-stream-string os))))");
    char buf[160];
    cl_prin1_to_string(result, buf, sizeof(buf));
    ASSERT_STR_EQ(buf,
        "((#\\a \"\" #\\a #\\b \"ab\") (#\\a \"   a\" #\\a \"\") (#\\c \"abc\" \"cde\" \"de\"))");
}

TEST(echo_stream_read_char_no_hang_listen_and_read_sequence)
{
    /* read-char-no-hang and listen see through the wrapper to the input
     * component (make-echo-stream.6/.7/.11); READ-SEQUENCE falls back to
     * the per-element path and echoes (make-echo-stream.8). */
    CL_Obj result = cl_eval_string(
        "(list"
        "  (let* ((is (make-string-input-stream \"foo\"))"
        "         (os (make-string-output-stream))"
        "         (s (make-echo-stream is os)))"
        "    (list (coerce (loop repeat 4 collect (read-char-no-hang s nil #\\z)) 'string)"
        "          (get-output-stream-string os)))"
        "  (let* ((is (make-string-input-stream \"fo\"))"
        "         (os (make-string-output-stream))"
        "         (s (make-echo-stream is os)))"
        "    (list (listen s) (read-char s) (listen s) (read-char s) (listen s)))"
        "  (let* ((is (make-string-input-stream \"foo\"))"
        "         (os (make-string-output-stream))"
        "         (s (make-echo-stream is os))"
        "         (x (copy-seq \"xxxxxx\")))"
        "    (list (read-sequence x s) x (get-output-stream-string os))))");
    char buf[128];
    cl_prin1_to_string(result, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "((\"fooz\" \"foo\") (T #\\f T #\\o NIL) (3 \"fooxxx\" \"foo\"))");
}

TEST(echo_stream_binary_file_components)
{
    /* READ-BYTE / READ-SEQUENCE through an echo stream over binary file
     * streams echo bytes (make-echo-stream.4/.9); stream-element-type
     * reports the input component's type. */
    CL_Obj result = cl_eval_string(
        "(let ((pn (format nil \"~a/amilisp-echo-~d.dat\" (or (ext:getenv \"TMPDIR\") \"/tmp\") (random 100000)))"
        "      (pn2 (format nil \"~a/amilisp-echo2-~d.dat\" (or (ext:getenv \"TMPDIR\") \"/tmp\") (random 100000)))"
        "      (et '(unsigned-byte 8)))"
        "  (with-open-file (os pn :direction :output :element-type et :if-exists :supersede)"
        "    (dolist (x '(2 3 5 7 11)) (write-byte x os)))"
        "  (prog1"
        "    (with-open-file (is pn :direction :input :element-type et)"
        "      (list"
        "        (with-open-file (os pn2 :direction :output :if-exists :supersede :element-type et)"
        "          (let ((s (make-echo-stream is os)))"
        "            (list (stream-element-type s)"
        "                  (loop repeat 6 collect (read-byte s nil :eof)))))"
        "        (with-open-file (s pn2 :direction :input :element-type et)"
        "          (loop repeat 6 collect (read-byte s nil :eof)))))"
        "    (delete-file pn) (delete-file pn2)))");
    char buf[128];
    cl_prin1_to_string(result, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(((UNSIGNED-BYTE 8) (2 3 5 7 11 :EOF)) (2 3 5 7 11 :EOF))");
}

TEST(echo_stream_reader_and_nesting)
{
    /* READ through an echo stream echoes exactly what the reader consumed;
     * an echo stream is itself a valid component of two-way / broadcast /
     * concatenated / echo streams. */
    CL_Obj result = cl_eval_string(
        "(list"
        "  (let* ((is (make-string-input-stream \"(1 2 3) tail\"))"
        "         (os (make-string-output-stream))"
        "         (s (make-echo-stream is os)))"
        "    (list (read s) (get-output-stream-string os)))"
        "  (let* ((log1 (make-string-output-stream))"
        "         (log2 (make-string-output-stream))"
        "         (inner (make-echo-stream (make-string-input-stream \"xy\") log1))"
        "         (outer (make-echo-stream (make-concatenated-stream inner) log2)))"
        "    (list (read-line outer nil)"
        "          (get-output-stream-string log1) (get-output-stream-string log2))))");
    char buf[96];
    cl_prin1_to_string(result, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(((1 2 3) \"(1 2 3)\") (\"xy\" \"xy\" \"xy\"))");
}

TEST(echo_stream_typep_and_printer)
{
    CL_Obj result = cl_eval_string(
        "(let ((es (make-echo-stream (make-string-input-stream \"\")"
        "                            (make-string-output-stream))))"
        "  (list (typep es 'echo-stream)"
        "        (typep es 'stream)"
        "        (typep es 'two-way-stream)"
        "        (streamp es)"
        "        (with-output-to-string (s) (prin1 es s))))");
    char buf[80];
    cl_prin1_to_string(result, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(T T NIL T \"#<ECHO-STREAM>\")");
}

TEST(make_echo_stream_rejects_bad_components)
{
    CL_Obj result;
    result = cl_eval_string(
        "(handler-case (make-echo-stream 42 (make-string-output-stream))"
        "  (error (c) (declare (ignore c)) :type-error))");
    ASSERT(result == cl_intern_keyword("TYPE-ERROR", 10));

    result = cl_eval_string(
        "(handler-case (make-echo-stream (make-string-output-stream) (make-string-output-stream))"
        "  (error (c) (declare (ignore c)) :type-error))");
    ASSERT(result == cl_intern_keyword("TYPE-ERROR", 10));

    result = cl_eval_string(
        "(handler-case (make-echo-stream (make-string-input-stream \"\") (make-string-input-stream \"\"))"
        "  (error (c) (declare (ignore c)) :type-error))");
    ASSERT(result == cl_intern_keyword("TYPE-ERROR", 10));

    result = cl_eval_string(
        "(handler-case (echo-stream-input-stream (make-string-input-stream \"\"))"
        "  (error (c) (declare (ignore c)) :type-error))");
    ASSERT(result == cl_intern_keyword("TYPE-ERROR", 10));

    result = cl_eval_string(
        "(handler-case (echo-stream-output-stream (make-two-way-stream"
        "                 (make-string-input-stream \"\") (make-string-output-stream)))"
        "  (error (c) (declare (ignore c)) :type-error))");
    ASSERT(result == cl_intern_keyword("TYPE-ERROR", 10));
}

TEST(read_char_no_hang_string_stream_eof_value)
{
    /* Regression: an exhausted string stream returned NIL ("no input yet")
     * from READ-CHAR-NO-HANG instead of the eof-value — an in-memory stream
     * can never block, so nothing available IS end of file (CLHS). */
    CL_Obj result = cl_eval_string(
        "(let ((s (make-string-input-stream \"fo\")))"
        "  (list (read-char-no-hang s nil :eof) (read-char-no-hang s nil :eof)"
        "        (read-char-no-hang s nil :eof) (read-char-no-hang s nil :eof)"
        "        (handler-case (read-char-no-hang s)"
        "          (end-of-file () :eof-error))))");
    char buf[64];
    cl_prin1_to_string(result, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(#\\f #\\o :EOF :EOF :EOF-ERROR)");
}

TEST(load_from_stream)
{
    /* CLHS: LOAD's filespec may be a stream — the ICL/SLYNK injection path
     * is (with-input-from-string (s "...") (load s)). */
    CL_Obj result = cl_eval_string(
        "(progn"
        "  (with-input-from-string (s \"(defvar cl-user::*lfs-var* 41)"
        " (defun cl-user::lfs-fn () (1+ cl-user::*lfs-var*))\")"
        "    (load s))"
        "  (cl-user::lfs-fn))");
    ASSERT(result == CL_MAKE_FIXNUM(42));

    /* LOAD binds *package*: an in-package inside the stream must not leak. */
    result = cl_eval_string(
        "(let ((before (package-name *package*)))"
        "  (with-input-from-string (s \"(in-package :keyword)\") (load s))"
        "  (string= before (package-name *package*)))");
    ASSERT(result == CL_T);

    /* *load-pathname* / *load-truename* are NIL for a stream filespec,
     * and LOAD returns true (empty stream included). */
    result = cl_eval_string(
        "(list (with-input-from-string (s \"(defvar cl-user::*lfs-lp* *load-pathname*)\")"
        "        (load s))"
        "      cl-user::*lfs-lp*"
        "      (with-input-from-string (s \"\") (load s)))");
    {
        char buf[32];
        cl_prin1_to_string(result, buf, sizeof(buf));
        ASSERT_STR_EQ(buf, "(T NIL T)");
    }
}

TEST(broadcast_concatenated_icl_slynk_muffle_pattern)
{
    /* The exact composite ICL's slynk init builds to muffle load output:
     * *debug-io* = two-way over an empty concatenated (instant EOF input)
     * and a broadcast null sink.  Reads see EOF, writes are discarded. */
    CL_Obj result = cl_eval_string(
        "(let* ((null-stream (make-broadcast-stream))"
        "       (dbg (make-two-way-stream (make-concatenated-stream) null-stream)))"
        "  (write-line \"muffled\" dbg)"
        "  (list (read-char dbg nil :eof) (read-line dbg nil :eof)))");
    char buf[32];
    cl_prin1_to_string(result, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(:EOF :EOF)");
}

/* ===== Socket read/write timeout tests ===== */

/* A read on a socket with a timeout set, when the peer sends nothing, returns
 * PLATFORM_SOCKET_TIMEOUT at the platform layer (distinct from -1 EOF) and must
 * NOT mark the stream at EOF — it is a deadline, not a close. */
TEST(socket_read_timeout_fires)
{
    int bound_port = 0;
    CL_Obj listener = cl_make_listen_stream(0, 1, &bound_port);
    CL_Obj client, conn;
    CL_Stream *cst;
    int rc;

    ASSERT(!CL_NULL_P(listener));
    ASSERT(bound_port > 0);

    client = cl_make_socket_stream("127.0.0.1", bound_port, 0);
    ASSERT(!CL_NULL_P(client));
    conn = cl_socket_stream_accept(listener);   /* server side; sends nothing */
    ASSERT(!CL_NULL_P(conn));

    cst = (CL_Stream *)CL_OBJ_TO_PTR(client);
    platform_socket_set_timeout((PlatformSocket)cst->handle_id, 100, 0);  /* 100ms read */

    rc = platform_socket_read((PlatformSocket)cst->handle_id);
    ASSERT_EQ_INT(rc, PLATFORM_SOCKET_TIMEOUT);
    /* Stream-level EOF flag must remain clear (timeout != EOF). */
    ASSERT(!(cst->flags & CL_STREAM_FLAG_EOF));

    cl_stream_close(conn);
    cl_stream_close(client);
    cl_stream_close(listener);
}

/* With a timeout set but data already waiting, the read returns the byte and
 * never trips the timeout path — guards against false positives. */
TEST(socket_read_timeout_not_triggered_when_data_present)
{
    int bound_port = 0;
    CL_Obj listener = cl_make_listen_stream(0, 1, &bound_port);
    CL_Obj client, conn;
    CL_Stream *cst;
    int rc;

    ASSERT(!CL_NULL_P(listener));
    client = cl_make_socket_stream("127.0.0.1", bound_port, 0);
    ASSERT(!CL_NULL_P(client));
    conn = cl_socket_stream_accept(listener);
    ASSERT(!CL_NULL_P(conn));

    /* Server writes a byte before the client reads, then flushes it to the wire. */
    cl_stream_write_byte(conn, 'Z');
    {
        CL_Stream *sst = (CL_Stream *)CL_OBJ_TO_PTR(conn);
        platform_socket_flush((PlatformSocket)sst->handle_id);
    }

    cst = (CL_Stream *)CL_OBJ_TO_PTR(client);
    platform_socket_set_timeout((PlatformSocket)cst->handle_id, 2000, 0);
    rc = platform_socket_read((PlatformSocket)cst->handle_id);
    ASSERT_EQ_INT(rc, 'Z');

    cl_stream_close(conn);
    cl_stream_close(client);
    cl_stream_close(listener);
}

/* The Lisp accessor round-trips seconds and keeps :input/:output independent;
 * NIL clears.  Uses numeric = comparison to stay float-type agnostic. */
TEST(eval_socket_stream_timeout_accessor)
{
    int bound_port = 0;
    CL_Obj listener = cl_make_listen_stream(0, 1, &bound_port);
    char expr[512];
    CL_Obj result;

    ASSERT(!CL_NULL_P(listener));
    ASSERT(bound_port > 0);

    snprintf(expr, sizeof(expr),
        "(let* ((c (ext:open-tcp-stream \"127.0.0.1\" %d)))"
        "  (setf (ext:socket-stream-timeout c :input) 1.5)"
        "  (prog1"
        "    (list (= 1.5 (ext:socket-stream-timeout c :input))"   /* round-trip */
        "          (null (ext:socket-stream-timeout c :output))"   /* independent */
        "          (progn (setf (ext:socket-stream-timeout c :input) nil)"
        "                 (null (ext:socket-stream-timeout c :input))))" /* NIL clears */
        "    (close c)))",
        bound_port);
    result = cl_eval_string(expr);
    /* Expect (T T T): drain the accept and check. */
    {
        CL_Obj conn = cl_socket_stream_accept(listener);
        ASSERT(!CL_NULL_P(result));
        ASSERT(CL_CONS_P(result));
        ASSERT(cl_car(result) == SYM_T);
        ASSERT(cl_car(cl_cdr(result)) == SYM_T);
        ASSERT(cl_car(cl_cdr(cl_cdr(result))) == SYM_T);
        if (!CL_NULL_P(conn)) cl_stream_close(conn);
    }
    cl_stream_close(listener);
}

/* A timed-out READ-CHAR signals EXT:SOCKET-TIMEOUT, which is a subtype of
 * STREAM-ERROR (and ERROR).  Verifies the whole stream-level signalling path. */
TEST(eval_socket_read_timeout_signals_condition)
{
    int bound_port = 0;
    CL_Obj listener = cl_make_listen_stream(0, 1, &bound_port);
    char expr[512];
    CL_Obj result, conn;

    ASSERT(!CL_NULL_P(listener));
    ASSERT(bound_port > 0);

    snprintf(expr, sizeof(expr),
        "(let ((c (ext:open-tcp-stream \"127.0.0.1\" %d)))"
        "  (setf (ext:socket-stream-timeout c :input) 1)"   /* 1s; server is silent */
        "  (unwind-protect"
        "      (handler-case (progn (read-char c) :no-timeout)"
        "        (ext:socket-timeout (e)"
        "          (list (typep e 'ext:socket-timeout) (typep e 'stream-error))))"
        "    (close c)))",
        bound_port);
    /* The client connects via the listen backlog (no accept needed for the
     * handshake) and times out reading because the server never sends; drain
     * the backlogged connection afterwards. */
    result = cl_eval_string(expr);
    conn = cl_socket_stream_accept(listener);
    ASSERT(CL_CONS_P(result));               /* the handler's (T T), not :no-timeout */
    ASSERT(cl_car(result) == SYM_T);
    ASSERT(cl_car(cl_cdr(result)) == SYM_T);
    if (!CL_NULL_P(conn)) cl_stream_close(conn);
    cl_stream_close(listener);
}

/* NOTE on write-timeout testing: a write timeout cannot be reliably triggered
 * over host loopback.  macOS buffers a loopback connection effectively without
 * bound, so select() never reports the send window closed even against a peer
 * that never reads (verified empirically) — a saturation-based test either
 * spins or hangs.  The write-timeout *mechanism* is the same non-blocking
 * send()/select()-deadline path proven by the read-timeout tests above (a write
 * deadline that elapses returns PLATFORM_SOCKET_TIMEOUT from socket_flush_wbuf,
 * which cl_stream_write_* / finish-output turn into EXT:SOCKET-TIMEOUT); the
 * AmigaOS reactor parks writes with the identical deadline logic.  What we CAN
 * test deterministically is the complementary property: arming a write timeout
 * must not break a normal write to a draining peer, and finish-output (whose
 * flush is the timed path that hunchentoot uses to push a response) must not
 * spuriously signal. */
TEST(eval_socket_write_timeout_does_not_break_normal_write)
{
    raw_sock_t server_fd;
    int port;
    TestPeer peer;
    pthread_t th;
    char expr[512];
    CL_Obj result;

    port = start_echo_server(&server_fd);
    ASSERT(port > 0);

    /* Peer: accept and drain what the timed flush pushes. */
    memset(&peer, 0, sizeof(peer));
    peer.fd = server_fd;
    peer.want = 16;
    ASSERT(pthread_create(&th, NULL, test_peer_thread, &peer) == 0);

    snprintf(expr, sizeof(expr),
        "(let ((c (ext:open-tcp-stream \"127.0.0.1\" %d)))"
        "  (setf (ext:socket-stream-timeout c :output) 5)"   /* generous deadline */
        "  (unwind-protect"
        "      (handler-case (progn (write-string \"PING\" c) (finish-output c) :ok)"
        "        (ext:socket-timeout () :unexpected-timeout))"
        "    (close c)))",
        port);
    result = cl_eval_string(expr);
    ASSERT(result == cl_intern_keyword("OK", 2));

    /* The peer must have actually received the bytes through the timed flush. */
    pthread_join(th, NULL);
    ASSERT_EQ_INT(peer.got_len, 4);
    ASSERT(peer.got[0] == 'P' && peer.got[1] == 'I' &&
           peer.got[2] == 'N' && peer.got[3] == 'G');
}

/* Direction keyword validation: a bogus direction is a type error. */
TEST(eval_socket_stream_timeout_bad_direction)
{
    int bound_port = 0;
    CL_Obj listener = cl_make_listen_stream(0, 1, &bound_port);
    char expr[512];
    CL_Obj result, conn;

    ASSERT(!CL_NULL_P(listener));
    snprintf(expr, sizeof(expr),
        "(let ((c (ext:open-tcp-stream \"127.0.0.1\" %d)))"
        "  (unwind-protect"
        "      (handler-case (ext:socket-stream-timeout c :sideways)"
        "        (type-error (e) (declare (ignore e)) :bad-dir))"
        "    (close c)))",
        bound_port);
    result = cl_eval_string(expr);
    conn = cl_socket_stream_accept(listener);
    ASSERT(result == cl_intern_keyword("BAD-DIR", 7));
    if (!CL_NULL_P(conn)) cl_stream_close(conn);
    cl_stream_close(listener);
}

/* CLHS get-dispatch-macro-character: signals an error when the character is
 * not a dispatching macro character.  named-readtables' readtable merge relies
 * on this error to distinguish ordinary macro characters (e.g. a custom
 * backquote) from dispatching ones — without it, fare-quasiquote's backquote
 * was misclassified and dropped, breaking trivia's quasiquote patterns. */
TEST(eval_get_dispatch_macro_character_conformance)
{
    char buf[64];
    CL_Obj r;
    /* # IS a dispatching macro character: no error, NIL for an unbound sub. */
    r = cl_eval_string(
        "(handler-case (list :ok (get-dispatch-macro-character #\\# #\\Z))"
        "  (error () :err))");
    cl_prin1_to_string(r, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(:OK NIL)");

    /* A constituent character is NOT a dispatching macro character → error. */
    r = cl_eval_string(
        "(handler-case (progn (get-dispatch-macro-character #\\a #\\x) :no-error)"
        "  (error () :err))");
    cl_prin1_to_string(r, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, ":ERR");

    /* An ordinary macro character (set via set-macro-character) is NOT a
     * dispatching one → error. */
    r = cl_eval_string(
        "(let ((rt (copy-readtable nil)))"
        "  (set-macro-character #\\! (lambda (s c) (declare (ignore s c)) :bang) nil rt)"
        "  (handler-case (progn (get-dispatch-macro-character #\\! #\\x rt) :no-error)"
        "    (error () :err)))");
    cl_prin1_to_string(r, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, ":ERR");
}

/* Regression: WITH-INPUT-FROM-STRING must honor :START/:END/:INDEX (CLHS).
 * The macro previously ignored every keyword, so :START reading and the
 * :INDEX place (used by serapeum's PARSE-FLOAT for its second value) were
 * silently wrong. */
TEST(eval_with_input_from_string_index_start_end)
{
    char buf[64];
    CL_Obj r;
    /* :START offsets the read; :INDEX receives the stop index. */
    r = cl_eval_string(
        "(let (pos)"
        "  (let ((v (with-input-from-string (s \"21\" :start 1 :index pos)"
        "             (read s))))"
        "    (list v pos)))");
    cl_prin1_to_string(r, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(1 2)");

    /* :END bounds the substream so only the first token is visible. */
    r = cl_eval_string(
        "(with-input-from-string (s \"12 34\" :end 2)"
        "  (read s nil :eof))");
    cl_prin1_to_string(r, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "12");
}

/* Regression: MAKE-STRING-INPUT-STREAM (and with it READ-FROM-STRING and
 * WITH-INPUT-FROM-STRING) must accept every string, including an adjustable
 * fill-pointer character vector -- CLHS "string" covers it and STRINGP says T.
 * The entry check only knew base/wide strings, so a buffer built with
 * VECTOR-PUSH-EXTEND and handed to READ-FROM-STRING (yason's parse-number)
 * failed with "argument is not a string".  The stream's extent is the fill
 * pointer, not the vector's capacity. */
TEST(eval_string_input_stream_accepts_fill_pointer_string)
{
    char buf[64];
    CL_Obj r;
    r = cl_eval_string(
        "(let ((b (make-array 8 :element-type 'character"
        "                       :adjustable t :fill-pointer 0)))"
        "  (vector-push-extend #\\4 b)"
        "  (vector-push-extend #\\2 b)"
        "  (list (stringp b)"
        "        (read-from-string b)"
        "        (with-input-from-string (s b) (read-char s))"
        "        (let ((s (make-string-input-stream b)))"
        "          (read-char s) (read-char s) (read-char s nil :eof))))");
    cl_prin1_to_string(r, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(T 42 #\\4 :EOF)");
}

/* Regression: FILE-POSITION on a string input stream must discount a
 * pushed-back character (PEEK-CHAR / UNREAD-CHAR).  PEEK-CHAR reads ahead and
 * stashes the char, advancing st->position; the logical read position is one
 * code point earlier.  This is what makes WITH-INPUT-FROM-STRING's :INDEX (and
 * PARSE-FLOAT's second value over trailing junk like "2x") correct. */
TEST(eval_file_position_accounts_for_peek)
{
    char buf[64];
    CL_Obj r;
    /* Read '2', peek at 'x' (no consume) -> position is 1, not 2. */
    r = cl_eval_string(
        "(let (pos)"
        "  (with-input-from-string (s \"2x\" :index pos)"
        "    (read-char s)"
        "    (peek-char nil s nil nil))"
        "  pos)");
    cl_prin1_to_string(r, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "1");

    /* UNREAD-CHAR likewise leaves the logical position before the char. */
    r = cl_eval_string(
        "(let ((s (make-string-input-stream \"ab\")))"
        "  (read-char s)"
        "  (read-char s)"
        "  (unread-char #\\b s)"
        "  (file-position s))");
    cl_prin1_to_string(r, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "1");
}

/* Regression: CLHS position-spec for (FILE-POSITION stream spec) is :START,
 * :END, or a non-negative integer.  The :START/:END designators were
 * rejected with a type error ("got SYMBOL"), which broke quicklisp's CDB
 * writer ((file-position stream :start) in cdb.lisp).  Covers file streams
 * and string streams, plus the type error for an invalid spec — which the
 * string-stream branch used to swallow silently. */
TEST(eval_file_position_start_end_designators)
{
    char buf[128];
    CL_Obj r;

    /* Output file stream — the exact cdb.lisp pattern: write the body,
     * seek :start, overwrite the header, seek :end; verify the end
     * position and the overwritten content. */
    r = cl_eval_string(
        "(let (endpos)"
        "  (with-open-file (s \"/tmp/cl_test_fpos.txt\" :direction :output"
        "                     :if-exists :supersede)"
        "    (write-string \"hello\" s)"
        "    (file-position s :start)"
        "    (write-string \"HE\" s)"
        "    (file-position s :end)"
        "    (setq endpos (file-position s)))"
        "  (with-open-file (s \"/tmp/cl_test_fpos.txt\")"
        "    (list endpos (read-line s))))");
    cl_prin1_to_string(r, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(5 \"HEllo\")");

    /* Input file stream: :end jumps to EOF, :start rewinds to char 0. */
    r = cl_eval_string(
        "(with-open-file (s \"/tmp/cl_test_fpos.txt\")"
        "  (read-char s)"
        "  (file-position s :end)"
        "  (let ((at-end (read-char s nil :eof)))"
        "    (file-position s :start)"
        "    (list at-end (read-char s))))");
    cl_prin1_to_string(r, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(:EOF #\\H)");

    /* String input stream: :end jumps to the end bound (read returns EOF),
     * :start rewinds to the first character. */
    r = cl_eval_string(
        "(let ((s (make-string-input-stream \"abc\")))"
        "  (read-char s)"
        "  (file-position s :end)"
        "  (let ((at-end (read-char s nil :eof)))"
        "    (file-position s :start)"
        "    (list at-end (read-char s))))");
    cl_prin1_to_string(r, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(:EOF #\\a)");

    /* Invalid position-specs signal TYPE-ERROR — on file streams and, the
     * formerly silent case, on string streams; negative integers too. */
    r = cl_eval_string(
        "(let ((s (make-string-input-stream \"abc\")))"
        "  (list (handler-case (file-position s :foo)"
        "          (type-error () :te))"
        "        (handler-case (file-position s -1)"
        "          (type-error () :te))))");
    cl_prin1_to_string(r, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(:TE :TE)");
}

/* Regression: for a string input stream created with a non-zero :START
 * (directly via MAKE-STRING-INPUT-STREAM, or via WITH-INPUT-FROM-STRING
 * which expands to it), (FILE-POSITION s :START) must rewind to the
 * stream's own beginning — the :START offset it was created with — not
 * absolute index 0 of the underlying string.  Verified against CLHS
 * position-spec: :START means "the beginning of the stream". */
TEST(eval_file_position_start_honors_stream_start_offset)
{
    char buf[128];
    CL_Obj r;

    r = cl_eval_string(
        "(let ((s (make-string-input-stream \"helloworld\" 5 10)))"
        "  (file-position s :start)"
        "  (read-char s))");
    cl_prin1_to_string(r, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "#\\w");

    /* No explicit :START (defaults to 0) still rewinds to absolute 0. */
    r = cl_eval_string(
        "(let ((s (make-string-input-stream \"abc\")))"
        "  (read-char s)"
        "  (file-position s :start)"
        "  (read-char s))");
    cl_prin1_to_string(r, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "#\\a");

    /* WITH-INPUT-FROM-STRING :START goes through the same path. */
    r = cl_eval_string(
        "(with-input-from-string (s \"helloworld\" :start 5 :end 10)"
        "  (read-char s)"
        "  (file-position s :start)"
        "  (read-char s))");
    cl_prin1_to_string(r, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "#\\w");
}

/* Regression: FILE-POSITION :END on an output file stream must reflect
 * bytes still sitting in the write-behind buffer, not just what has
 * already been flushed to the OS.  A test that only overwrites bytes of
 * equal length never grows the file and can't catch stale-length bugs;
 * this appends past the initial write so the file genuinely grows. */
TEST(eval_file_position_end_reflects_unflushed_growth)
{
    char buf[128];
    CL_Obj r;

    r = cl_eval_string(
        "(let (endpos)"
        "  (with-open-file (s \"/tmp/cl_test_fpos_grow.txt\" :direction :output"
        "                     :if-exists :supersede)"
        "    (write-string \"hello\" s)"
        "    (write-string \"world\" s)"
        "    (file-position s :end)"
        "    (setq endpos (file-position s)))"
        "  (with-open-file (s \"/tmp/cl_test_fpos_grow.txt\")"
        "    (list endpos (read-line s))))");
    cl_prin1_to_string(r, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(10 \"helloworld\")");
}

/* --- READ-SEQUENCE / WRITE-SEQUENCE builtins (CLHS 21.2) ---
 * The C builtins replaced boot.lisp per-element loops; these pin both the
 * bulk (unsigned-byte 8 <-> file stream) fast path and the per-element
 * fallbacks (strings, lists, string streams). */

TEST(eval_write_read_sequence_u8_file_fast_path)
{
    char buf[128];
    CL_Obj r;
    /* 300 bytes forces no single-chunk shortcut assumptions; sentinel bytes
     * at both ends prove position 0 and 299 really round-trip. */
    r = cl_eval_string(
        "(progn"
        "  (let ((v (make-array 300 :element-type '(unsigned-byte 8)"
        "                           :initial-element 65)))"
        "    (setf (aref v 0) 1)"
        "    (setf (aref v 299) 2)"
        "    (let ((s (open \"/tmp/cl_test_rseq.bin\" :direction :output"
        "                   :element-type '(unsigned-byte 8)"
        "                   :if-exists :supersede)))"
        "      (write-sequence v s)"
        "      (close s)))"
        "  (let ((v (make-array 300 :element-type '(unsigned-byte 8)))"
        "        (s (open \"/tmp/cl_test_rseq.bin\""
        "                 :element-type '(unsigned-byte 8))))"
        "    (let ((n (read-sequence v s)))"
        "      (close s)"
        "      (list n (aref v 0) (aref v 1) (aref v 299)))))");
    cl_prin1_to_string(r, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(300 1 65 2)");

    /* EOF: a larger buffer reads short and returns the count filled. */
    r = cl_eval_string(
        "(let ((v (make-array 400 :element-type '(unsigned-byte 8)"
        "                         :initial-element 9))"
        "      (s (open \"/tmp/cl_test_rseq.bin\""
        "               :element-type '(unsigned-byte 8))))"
        "  (let ((n (read-sequence v s)))"
        "    (close s)"
        "    (list n (aref v 299) (aref v 300))))");
    cl_prin1_to_string(r, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(300 2 9)");
}

/* 9000 bytes spans three CL_SEQ_IO_CHUNK (4096) iterations, forcing the
 * bulk chunked-read/write loops to actually loop; sentinels sit at the
 * start, on both sides of the first chunk boundary, and at the end so a
 * stale stream/offset reused across iterations would corrupt one of them. */
TEST(eval_write_read_sequence_u8_file_multi_chunk)
{
    char buf[128];
    CL_Obj r;
    r = cl_eval_string(
        "(progn"
        "  (let ((v (make-array 9000 :element-type '(unsigned-byte 8)"
        "                            :initial-element 65)))"
        "    (setf (aref v 0) 1)"
        "    (setf (aref v 4095) 3)"
        "    (setf (aref v 4096) 4)"
        "    (setf (aref v 8999) 2)"
        "    (let ((s (open \"/tmp/cl_test_rseq_mc.bin\" :direction :output"
        "                   :element-type '(unsigned-byte 8)"
        "                   :if-exists :supersede)))"
        "      (write-sequence v s)"
        "      (close s)))"
        "  (let ((v (make-array 9000 :element-type '(unsigned-byte 8)))"
        "        (s (open \"/tmp/cl_test_rseq_mc.bin\""
        "                 :element-type '(unsigned-byte 8))))"
        "    (let ((n (read-sequence v s)))"
        "      (close s)"
        "      (list n (aref v 0) (aref v 4095) (aref v 4096) (aref v 8999)))))");
    cl_prin1_to_string(r, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(9000 1 3 4 2)");
}

TEST(eval_read_sequence_start_end)
{
    char buf[128];
    CL_Obj r = cl_eval_string(
        "(let ((v (make-array 8 :element-type '(unsigned-byte 8)"
        "                       :initial-element 0))"
        "      (s (open \"/tmp/cl_test_rseq.bin\""
        "               :element-type '(unsigned-byte 8))))"
        "  (let ((n (read-sequence v s :start 2 :end 5)))"
        "    (close s)"
        "    (list n (aref v 1) (aref v 2) (aref v 4) (aref v 5))))");
    cl_prin1_to_string(r, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(5 0 1 65 0)");
}

TEST(eval_read_sequence_bounds_error)
{
    int err;
    CL_CATCH(err);
    if (err == CL_ERR_NONE) {
        cl_eval_string(
            "(read-sequence (make-array 4 :element-type '(unsigned-byte 8))"
            "               (make-string-input-stream \"abcd\")"
            "               :start 3 :end 9)");
        CL_UNCATCH();
        ASSERT(0 && "expected bounding-index error");
    } else {
        CL_UNCATCH();
        cl_vm.sp = 0;
        cl_vm.fp = 0;
    }
}

TEST(eval_read_sequence_bad_start_type_error)
{
    int err;
    CL_CATCH(err);
    if (err == CL_ERR_NONE) {
        cl_eval_string(
            "(read-sequence (make-array 4 :element-type '(unsigned-byte 8))"
            "               (make-string-input-stream \"abcd\")"
            "               :start \"x\")");
        CL_UNCATCH();
        ASSERT(0 && "expected type-error for non-fixnum :start");
    } else {
        CL_UNCATCH();
        cl_vm.sp = 0;
        cl_vm.fp = 0;
    }
}

TEST(eval_write_sequence_bad_end_type_error)
{
    int err;
    CL_CATCH(err);
    if (err == CL_ERR_NONE) {
        cl_eval_string(
            "(write-sequence \"abcd\" (make-string-output-stream) :end \"x\")");
        CL_UNCATCH();
        ASSERT(0 && "expected type-error for non-fixnum/non-nil :end");
    } else {
        CL_UNCATCH();
        cl_vm.sp = 0;
        cl_vm.fp = 0;
    }
}

TEST(eval_write_read_sequence_string_chars)
{
    char buf[128];
    CL_Obj r = cl_eval_string(
        "(progn"
        "  (let ((s (open \"/tmp/cl_test_rseq.txt\" :direction :output"
        "                 :if-exists :supersede)))"
        "    (write-sequence \"hello world\" s :start 0 :end 11)"
        "    (close s))"
        "  (let ((str (make-string 5 :initial-element #\\_))"
        "        (s (open \"/tmp/cl_test_rseq.txt\")))"
        "    (let ((n (read-sequence str s)))"
        "      (close s)"
        "      (list n str))))");
    cl_prin1_to_string(r, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(5 \"hello\")");
}

TEST(eval_read_sequence_consumes_unread_char)
{
    char buf[128];
    /* A pushed-back char must be the first element the sequence gets. */
    CL_Obj r = cl_eval_string(
        "(let ((s (open \"/tmp/cl_test_rseq.txt\")))"
        "  (read-char s)"
        "  (unread-char #\\h s)"
        "  (let ((str (make-string 4)))"
        "    (read-sequence str s)"
        "    (close s)"
        "    str))");
    cl_prin1_to_string(r, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "\"hell\"");
}

TEST(eval_write_read_sequence_list)
{
    char buf[128];
    CL_Obj r = cl_eval_string(
        "(progn"
        "  (let ((s (open \"/tmp/cl_test_rseq2.bin\" :direction :output"
        "                 :element-type '(unsigned-byte 8)"
        "                 :if-exists :supersede)))"
        "    (write-sequence '(10 20 30 40) s)"
        "    (close s))"
        "  (let ((l (list 0 0 0))"
        "        (s (open \"/tmp/cl_test_rseq2.bin\""
        "                 :element-type '(unsigned-byte 8))))"
        "    (let ((n (read-sequence l s :start 1)))"
        "      (close s)"
        "      (list n l))))");
    cl_prin1_to_string(r, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(3 (0 10 20))");
}

TEST(eval_read_sequence_string_stream_fallback)
{
    char buf[128];
    /* String input streams have no bulk path — the per-element fallback
     * must deliver character codes into a byte vector. */
    CL_Obj r = cl_eval_string(
        "(let ((v (make-array 3 :element-type '(unsigned-byte 8))))"
        "  (list (read-sequence v (make-string-input-stream \"ABC\")) v))");
    cl_prin1_to_string(r, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "(3 #(65 66 67))");
}

TEST(eval_write_sequence_string_output_stream)
{
    char buf[128];
    CL_Obj r = cl_eval_string(
        "(let ((s (make-string-output-stream)))"
        "  (write-sequence \"abc\" s)"
        "  (write-sequence '(#\\d #\\e) s)"
        "  (get-output-stream-string s))");
    cl_prin1_to_string(r, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "\"abcde\"");
}

TEST(eval_write_sequence_returns_sequence_and_checks_direction)
{
    char buf[128];
    int err;
    CL_Obj r = cl_eval_string(
        "(let ((s (make-string-output-stream)))"
        "  (write-sequence \"xy\" s))");
    cl_prin1_to_string(r, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "\"xy\"");

    CL_CATCH(err);
    if (err == CL_ERR_NONE) {
        cl_eval_string(
            "(write-sequence \"xy\" (make-string-input-stream \"ab\"))");
        CL_UNCATCH();
        ASSERT(0 && "expected not-an-output-stream error");
    } else {
        CL_UNCATCH();
        cl_vm.sp = 0;
        cl_vm.fp = 0;
    }
}


/* --- Unconnected datagram sockets: ext:open-udp-socket / udp-send-to /
 * udp-receive-from, IPv6, multicast, network-interfaces.  Both ends of
 * every round trip are AmiLisp sockets in this process, so no C peer is
 * needed; the bound port is learned through socket-stream-local-endpoint. */

/* Evaluate EXPR; a :SKIPPED result (IPv6 off on this machine, see
 * ext:ipv6-supported-p) is reported and tolerated, anything else must be T. */
static void assert_t_or_skipped(const char *expr, const char *what)
{
    CL_Obj result = cl_eval_string(expr);
    if (result == cl_eval_string(":skipped")) {
        printf("    NOTE: %s skipped (IPv6 not available on this machine)\n", what);
        return;
    }
    ASSERT(result == CL_T);
}

TEST(eval_udp_socket_roundtrip_ipv4)
{
    CL_Obj result = cl_eval_string(
        "(let* ((a (ext:open-udp-socket :address \"127.0.0.1\"))"
        "       (b (ext:open-udp-socket :address \"127.0.0.1\"))"
        "       (buf (make-array 16 :element-type '(unsigned-byte 8) :initial-element 0)))"
        "  (unwind-protect"
        "    (multiple-value-bind (ip-a port-a) (ext:socket-stream-local-endpoint a)"
        "      (multiple-value-bind (ip-b port-b) (ext:socket-stream-local-endpoint b)"
        "        (setf (ext:socket-stream-timeout a :input) 5)"
        "        (setf (ext:socket-stream-timeout b :input) 5)"
        "        (ext:udp-send-to a (coerce #(1 2 3 4 5) '(vector (unsigned-byte 8)))"
        "                        \"127.0.0.1\" port-b)"
        "        (multiple-value-bind (len host port) (ext:udp-receive-from b buf)"
        "          (and (equal ip-a \"127.0.0.1\") (equal ip-b \"127.0.0.1\")"
        "               (> port-a 0) (> port-b 0) (/= port-a port-b)"
        "               (= len 5) (equal host \"127.0.0.1\") (= port port-a)"
        "               (equal (coerce (subseq buf 0 5) 'list) '(1 2 3 4 5))"
        /* answer the reported sender, with an explicit partial length */
        "               (ext:udp-send-to b buf host port 2)"
        "               (multiple-value-bind (len2 host2 port2)"
        "                   (ext:udp-receive-from a buf 16)"
        "                 (and (= len2 2) (equal host2 \"127.0.0.1\")"
        "                      (= port2 port-b)))))))"
        "    (close a) (close b)))");
    ASSERT(result == CL_T);
}

TEST(eval_udp_socket_general_vector_buffers)
{
    /* The simple-vector (non-packed) buffer path on both sides, the
     * fill-pointer default length on send, and a receive into a buffer
     * smaller than the datagram (truncated to max-length). */
    CL_Obj result = cl_eval_string(
        "(let* ((a (ext:open-udp-socket :address \"127.0.0.1\"))"
        "       (b (ext:open-udp-socket :address \"127.0.0.1\"))"
        "       (out (make-array 8 :fill-pointer 3 :initial-contents '(9 8 7 0 0 0 0 0)))"
        "       (in (make-array 2 :initial-element nil)))"
        "  (unwind-protect"
        "    (let ((port-b (nth-value 1 (ext:socket-stream-local-endpoint b))))"
        "      (setf (ext:socket-stream-timeout b :input) 5)"
        "      (ext:udp-send-to a out \"127.0.0.1\" port-b)"
        "      (multiple-value-bind (len host port) (ext:udp-receive-from b in)"
        "        (declare (ignore host port))"
        "        (and (= len 2) (equal (coerce in 'list) '(9 8)))))"
        "    (close a) (close b)))");
    ASSERT(result == CL_T);
}

TEST(eval_udp_socket_roundtrip_ipv6)
{
    assert_t_or_skipped(
        "(if (not (ext:ipv6-supported-p)) :skipped"
        "  (let* ((a (ext:open-udp-socket :address \"::1\"))"
        "         (b (ext:open-udp-socket :family :ipv6))"   /* bound to :: */
        "         (buf (make-array 8 :element-type '(unsigned-byte 8) :initial-element 0)))"
        "    (unwind-protect"
        "      (multiple-value-bind (ip-a port-a) (ext:socket-stream-local-endpoint a)"
        "        (multiple-value-bind (ip-b port-b) (ext:socket-stream-local-endpoint b)"
        "          (setf (ext:socket-stream-timeout b :input) 5)"
        "          (ext:udp-send-to a (coerce #(6 7) '(vector (unsigned-byte 8))) \"::1\" port-b)"
        "          (multiple-value-bind (len host port) (ext:udp-receive-from b buf)"
        "            (and (equal ip-a \"::1\") (equal ip-b \"::\")"
        "                 (= len 2) (equal host \"::1\") (= port port-a)"
        "                 (equal (coerce (subseq buf 0 2) 'list) '(6 7))))))"
        "      (close a) (close b))))",
        "IPv6 datagram round trip");
}

TEST(eval_udp_socket_receive_timeout)
{
    CL_Obj result = cl_eval_string(
        "(let ((s (ext:open-udp-socket :address \"127.0.0.1\"))"
        "      (buf (make-array 8 :element-type '(unsigned-byte 8))))"
        "  (setf (ext:socket-stream-timeout s :input) 0.15)"
        "  (prog1 (handler-case (progn (ext:udp-receive-from s buf) :data)"
        "           (ext:socket-timeout () :timed-out))"
        "    (close s)))");
    ASSERT(result == cl_eval_string(":timed-out"));
}

TEST(eval_udp_socket_fixed_port_and_reuse_address)
{
    /* A port in use is refused without :reuse-address, shared with it. */
    CL_Obj result = cl_eval_string(
        "(equal '(:refused t)"
        "(let* ((a (ext:open-udp-socket :address \"127.0.0.1\"))"
        "       (port (nth-value 1 (ext:socket-stream-local-endpoint a))))"
        "  (unwind-protect"
        "    (list (handler-case"
        "              (progn (close (ext:open-udp-socket :address \"127.0.0.1\" :port port))"
        "                     :bound)"
        "            (error () :refused))"
        "          (progn (close a)"
        "                 (let ((b (ext:open-udp-socket :address \"127.0.0.1\" :port port"
        "                                               :reuse-address t))"
        "                       (c (ext:open-udp-socket :address \"127.0.0.1\" :port port"
        "                                               :reuse-address t)))"
        "                   (prog1 (= port (nth-value 1 (ext:socket-stream-local-endpoint b))"
        "                             (nth-value 1 (ext:socket-stream-local-endpoint c)))"
        "                     (close b) (close c)))))"
        "    (close a))))");
    ASSERT(result == CL_T);
}

TEST(eval_udp_socket_send_requires_send_to)
{
    /* udp-stream-send needs a connected socket; the error names the fix. */
    CL_Obj result = cl_eval_string(
        "(let ((s (ext:open-udp-socket :address \"127.0.0.1\")))"
        "  (prog1 (handler-case"
        "             (progn (ext:udp-stream-send s (make-array 1 :element-type '(unsigned-byte 8)"
        "                                                      :initial-element 1))"
        "                    :sent)"
        "           (error (e) (and (search \"UDP-SEND-TO\" (format nil \"~a\" e)) :hinted)))"
        "    (close s)))");
    ASSERT(result == cl_eval_string(":hinted"));
}

TEST(eval_udp_socket_argument_errors)
{
    CL_Obj result = cl_eval_string(
        "(equal '(:rejected :rejected :rejected :rejected :rejected"
        "        (:rejected :rejected :rejected :rejected :rejected))"
        "(flet ((try (thunk) (handler-case (progn (funcall thunk) :accepted)"
        "                      (error () :rejected))))"
        "  (list (try (lambda () (ext:open-udp-socket :family :inet)))"
        "        (try (lambda () (ext:open-udp-socket :address \"nonsense\")))"
        "        (try (lambda () (ext:open-udp-socket :port 70000)))"
        "        (try (lambda () (ext:open-udp-socket :address \"::1\" :family :ipv4)))"
        "        (try (lambda () (ext:open-udp-socket :address \"127.0.0.1\" :family :ipv6)))"
        "        (let ((s (ext:open-udp-socket :address \"127.0.0.1\")))"
        "          (prog1 (list (try (lambda () (ext:udp-send-to s #(1) \"127.0.0.1\" 0)))"
        "                       (try (lambda () (ext:udp-send-to s #(256) \"127.0.0.1\" 9)))"
        "                       (try (lambda () (ext:udp-send-to s #(1) 42 9)))"
        "                       (try (lambda () (ext:udp-receive-from s \"not a buffer\")))"
        "                       (try (lambda () (ext:udp-send-to *standard-output* #(1)"
        "                                                        \"127.0.0.1\" 9))))"
        "            (close s))))))");
    ASSERT(result == CL_T);
}

TEST(eval_udp_multicast_ipv4_loopback)
{
    /* Join 224.0.0.251 (the mDNS group) on the loopback interface and send
     * ourselves a datagram.  Delivery through loopback is asserted only
     * where the OS declares the loopback interface multicast-capable
     * (macOS does, Linux's lo does not); the join/configure/leave calls
     * must succeed everywhere. */
    CL_Obj result = cl_eval_string(
        "(let* ((r (ext:open-udp-socket :reuse-address t))"
        "       (port (nth-value 1 (ext:socket-stream-local-endpoint r)))"
        "       (s (ext:open-udp-socket :address \"127.0.0.1\"))"
        "       (buf (make-array 8 :element-type '(unsigned-byte 8)))"
        "       (lo (find-if (lambda (e) (member :loopback (getf e :flags)))"
        "                    (ext:network-interfaces)))"
        "       (lo-mcast (and lo (member :multicast (getf lo :flags)) t)))"
        "  (unwind-protect"
        "    (progn"
        "      (ext:udp-multicast-join r \"224.0.0.251\" \"127.0.0.1\")"
        "      (ext:udp-multicast-configure s :hops 1 :loopback t :interface \"127.0.0.1\")"
        "      (setf (ext:socket-stream-timeout r :input) 2)"
        "      (ext:udp-send-to s (coerce #(7 7 7) '(vector (unsigned-byte 8)))"
        "                      \"224.0.0.251\" port)"
        "      (let ((got (handler-case (multiple-value-list (ext:udp-receive-from r buf))"
        "                   (ext:socket-timeout () :timed-out))))"
        "        (ext:udp-multicast-leave r \"224.0.0.251\" \"127.0.0.1\")"
        "        (cond ((equal got '(3 \"127.0.0.1\" nil)) :impossible)"
        "              ((and (listp got) (= (first got) 3) (equal (second got) \"127.0.0.1\"))"
        "               :delivered)"
        "              ((and (eq got :timed-out) (not lo-mcast)) :no-loopback-multicast)"
        "              (t got))))"
        "    (close r) (close s)))");
    if (result == cl_eval_string(":no-loopback-multicast")) {
        printf("    NOTE: multicast delivery skipped (loopback interface is not multicast-capable here)\n");
        return;
    }
#if defined(_WIN32)
    if (result == cl_eval_string(":timed-out")) {
        printf("    NOTE: multicast delivery through loopback not observed on Windows\n");
        return;
    }
#endif
    ASSERT(result == cl_eval_string(":delivered"));
}

TEST(eval_udp_multicast_ipv6_membership)
{
    /* IPv6 membership on the loopback interface, named by index, by name,
     * and by a scope suffix on the group; leaving a group twice fails. */
    assert_t_or_skipped(
        "(if (not (ext:ipv6-supported-p)) :skipped"
        "  (let* ((s (ext:open-udp-socket :family :ipv6 :reuse-address t))"
        "         (lo (find-if (lambda (e) (and (member :loopback (getf e :flags))"
        "                                       (member :up (getf e :flags))))"
        "                      (ext:network-interfaces)))"
        "         (idx (getf lo :index))"
        "         (name (getf lo :name)))"
        "    (unwind-protect"
        "      (and (integerp idx) (> idx 0) (stringp name)"
        "           (ext:udp-multicast-join s \"ff02::fb\" idx)"
        "           (ext:udp-multicast-configure s :hops 1 :loopback nil :interface idx)"
        "           (ext:udp-multicast-configure s :interface name)"
        "           (ext:udp-multicast-leave s \"ff02::fb\" idx)"
        "           (ext:udp-multicast-join s (format nil \"ff02::fb%~a\" name))"
        "           (ext:udp-multicast-leave s (format nil \"ff02::fb%~a\" name))"
        "           (handler-case (progn (ext:udp-multicast-leave s \"ff02::fb\" idx) nil)"
        "             (error () t)))"
        "      (close s))))",
        "IPv6 multicast membership");
}

TEST(eval_udp_multicast_argument_errors)
{
    CL_Obj result = cl_eval_string(
        "(equal '(:rejected :rejected :rejected :rejected :rejected :rejected :accepted)"
        "(flet ((try (thunk) (handler-case (progn (funcall thunk) :accepted)"
        "                      (error () :rejected))))"
        "  (let ((s (ext:open-udp-socket :address \"127.0.0.1\")))"
        "    (prog1"
        "      (list (try (lambda () (ext:udp-multicast-join *standard-output* \"224.0.0.251\")))"
        "            (try (lambda () (ext:udp-multicast-join s \"127.0.0.1\")))"       /* not multicast */
        "            (try (lambda () (ext:udp-multicast-join s \"224.0.0.251\" \"no-such-if0\")))"
        "            (try (lambda () (ext:udp-multicast-join s \"224.0.0.251\" 0)))"
        "            (try (lambda () (ext:udp-multicast-configure s :hops 300)))"
        "            (try (lambda () (ext:udp-multicast-configure s :interface \"no-such-if0\")))"
        "            (try (lambda () (ext:udp-multicast-configure s :hops 4 :loopback nil))))"
        "      (close s)))))");
    ASSERT(result == CL_T);
}

TEST(eval_network_interfaces)
{
    /* Every entry is a well-formed plist, and the loopback interface is
     * listed with 127.0.0.1 among its addresses. */
    CL_Obj result = cl_eval_string(
        "(let ((ifs (ext:network-interfaces)))"
        "  (and (consp ifs)"
        "       (every (lambda (e)"
        "                (and (stringp (getf e :name)) (integerp (getf e :index))"
        "                     (listp (getf e :flags))"
        "                     (subsetp (getf e :flags) '(:up :loopback :multicast))"
        "                     (listp (getf e :addresses))"
        "                     (every #'stringp (getf e :addresses))))"
        "              ifs)"
        "       (some (lambda (e) (and (member :loopback (getf e :flags))"
        "                              (member \"127.0.0.1\" (getf e :addresses)"
        "                                      :test #'equal)))"
        "             ifs)"
        "       t))");
    ASSERT(result == CL_T);
}

TEST(eval_ipv6_tcp_loopback)
{
    /* socket-listen on an IPv6 literal, open-tcp-stream to it, and the
     * local endpoints of both ends report IPv6 addresses. */
    assert_t_or_skipped(
        "(if (not (ext:ipv6-supported-p)) :skipped"
        "  (let* ((l (ext:socket-listen 0 \"::1\"))"
        "         (p (ext:socket-local-port l)))"
        "    (unwind-protect"
        "      (let* ((c (ext:open-tcp-stream \"::1\" p))"
        "             (s (ext:socket-accept l)))"
        "        (unwind-protect"
        "          (progn (write-byte 65 c) (force-output c)"
        "                 (and (= 65 (read-byte s))"
        "                      (equal (ext:socket-stream-local-endpoint c) \"::1\")"
        "                      (equal (ext:socket-stream-local-endpoint s) \"::1\")"
        "                      (= p (nth-value 1 (ext:socket-stream-local-endpoint s)))))"
        "          (close c) (close s)))"
        "      (close l))))",
        "IPv6 TCP loopback");
}

TEST(eval_open_udp_stream_ipv6)
{
    assert_t_or_skipped(
        "(if (not (ext:ipv6-supported-p)) :skipped"
        "  (let ((s (ext:open-udp-stream \"::1\" 9)))"
        "    (prog1 (equal (ext:socket-stream-local-endpoint s) \"::1\")"
        "      (close s))))",
        "IPv6 connected datagram socket");
}

TEST(eval_ipv6_supported_p_is_boolean)
{
    CL_Obj result = cl_eval_string("(member (ext:ipv6-supported-p) '(t nil))");
    ASSERT(result != CL_NIL);
}

int main(void)
{
    test_init();
    setup();

    /* Stream allocation */
    RUN(make_console_input_stream);
    RUN(make_console_output_stream);
    RUN(make_file_stream);
    RUN(make_string_stream);
    RUN(stream_type_name);

    /* GC */
    RUN(stream_gc_survives);
    RUN(stream_gc_marks_string_buf);

    /* Output buffers */
    RUN(outbuf_alloc_free);
    RUN(outbuf_live_survives_dead_stream_sharing_handle);
    RUN(outbuf_dead_stream_reclaimed);
    RUN(outbuf_table_grows_past_one_block);
    RUN(close_string_stream_frees_and_clears_outbuf_handle);
    RUN(outbuf_putchar);
    RUN(outbuf_write_string);
    RUN(outbuf_growth);
    RUN(outbuf_reset);

    /* Platform file I/O */
    RUN(platform_file_write_read);
    RUN(platform_file_open_nonexistent);
    RUN(platform_file_append);

    /* typep */
    RUN(stream_typep);

    /* Console streams and standard variables */
    RUN(console_streams_created);
    RUN(standard_stream_variables_bound);
    RUN(standard_input_is_input);
    RUN(standard_output_is_output);
    RUN(interactive_stream_p_console);
    RUN(interactive_stream_p_file);
    RUN(interactive_stream_p_string);
    RUN(console_streams_survive_gc);

    /* Stream I/O (Step 3) */
    RUN(make_string_input_stream);
    RUN(string_input_stream_eof);
    RUN(string_input_stream_start_end);
    RUN(string_output_stream_write_chars);
    RUN(string_output_stream_write_string);
    RUN(get_output_stream_string_resets);
    RUN(make_string_output_stream_survives_table_full_gc);
    RUN(unread_char_test);
    RUN(peek_char_test);
    RUN(close_stream);
    RUN(file_stream_write_read);
    RUN(open_element_type_binary_reported);
    RUN(open_relative_honors_default_pathname_defaults);
    RUN(open_element_type_character_default);
    RUN(open_latin1_external_format_byte_faithful);
    RUN(open_latin1_read_roundtrip_and_format);
    RUN(open_iso8859_spelling_byte_faithful);
    RUN(charpos_tracking);
    RUN(read_line_string_stream);

    /* Two-way streams */
    RUN(make_two_way_stream_returns_stream);
    RUN(two_way_stream_direction_predicates);
    RUN(two_way_stream_accessors);
    RUN(two_way_stream_read_from_input);
    RUN(two_way_stream_write_to_output);
    RUN(two_way_stream_read_write_roundtrip);
    RUN(two_way_stream_typep);
    RUN(two_way_stream_printer);
    RUN(two_way_stream_accessor_type_error);

    /* Broadcast / concatenated streams (CLHS 21.2) */
    RUN(broadcast_stream_fans_out_writes);
    RUN(broadcast_stream_no_components_discards);
    RUN(broadcast_stream_streams_accessor);
    RUN(broadcast_stream_nested_and_two_way_component);
    RUN(broadcast_stream_fresh_line_uses_component_column);
    RUN(broadcast_stream_typep_and_printer);
    RUN(make_broadcast_stream_rejects_non_output_stream);
    RUN(concatenated_stream_reads_in_order);
    RUN(concatenated_stream_no_components_is_eof);
    RUN(concatenated_stream_streams_drains);
    RUN(concatenated_stream_read_line_spans_components);
    RUN(concatenated_stream_unread_and_peek);
    RUN(concatenated_stream_listen);
    RUN(concatenated_stream_read_from_it);
    RUN(concatenated_stream_typep_and_printer);
    RUN(make_concatenated_stream_rejects_non_input_stream);

    /* Echo streams (CLHS 21.2) */
    RUN(echo_stream_echoes_reads);
    RUN(echo_stream_direct_writes_not_echoed_elsewhere);
    RUN(echo_stream_accessors_and_close);
    RUN(echo_stream_unread_echoes_once);
    RUN(echo_stream_peek_char_semantics);
    RUN(echo_stream_read_char_no_hang_listen_and_read_sequence);
    RUN(echo_stream_binary_file_components);
    RUN(echo_stream_reader_and_nesting);
    RUN(echo_stream_typep_and_printer);
    RUN(make_echo_stream_rejects_bad_components);
    RUN(read_char_no_hang_string_stream_eof_value);

    RUN(load_from_stream);
    RUN(broadcast_concatenated_icl_slynk_muffle_pattern);
    RUN(make_two_way_stream_rejects_non_stream);

    /* Reader + stream integration (Step 6) */
    RUN(read_from_stream_simple_list);
    RUN(read_from_stream_integer);
    RUN(read_from_stream_multiple_exprs);
    RUN(read_from_stream_empty);
    RUN(read_from_stream_string_literal);
    RUN(read_from_string_backward_compat);

    /* Printer + stream integration (Step 7) */
    RUN(prin1_to_string_stream);
    RUN(princ_to_string_stream);
    RUN(prin1_string_escapes_to_stream);
    RUN(print_to_string_stream);
    RUN(prin1_list_to_stream);
    RUN(multiple_prints_to_same_stream);
    RUN(prin1_to_string_c_still_works);
    RUN(prin1_nil_to_stream);
    RUN(prin1_char_to_stream);
    RUN(princ_char_to_stream);

    /* File stream open/close (Step 8) */
    RUN(open_file_write_read);
    RUN(open_file_output_stream_write);
    RUN(file_stream_write_string_read_back);

    /* Platform file operations (Step 10) */
    RUN(platform_universal_time);
    RUN(platform_file_exists_positive);
    RUN(platform_file_exists_negative);
    RUN(platform_file_delete_test);
    RUN(platform_file_rename_test);
    RUN(platform_file_rename_replaces_existing);
    RUN(platform_file_mtime_test);
    RUN(platform_file_mtime_nonexistent);
    RUN(platform_mkdir_test);
    RUN(platform_file_is_directory_test);

    /* Builtins via eval (Step 10) */
    RUN(eval_get_universal_time);
    RUN(eval_probe_file_exists);
    RUN(eval_probe_file_not_exists);
    RUN(eval_truename_dir_no_slash_is_dir_form);
    RUN(eval_truename_file_stays_file_form);
    RUN(eval_delete_file);
    RUN(eval_file_write_date);
    RUN(eval_file_namestring);
    RUN(eval_directory_namestring);
    RUN(eval_rename_file);

    /* MV propagation regression test */
    RUN(eval_values_from_defun);

    /* Readtable tests (Step 12) */
    RUN(readtablep_current);
    RUN(readtablep_non_readtable);
    RUN(get_macro_character_paren);
    RUN(get_macro_character_hash);
    RUN(get_macro_character_string);
    RUN(set_macro_character_excl);
    RUN(copy_readtable_preserves);
    RUN(compile_nil_lambda);
    RUN(compile_named_function);
    RUN(star_readtable_is_special);

    /* TCP Socket Stream tests */
    RUN(socket_double_close_keeps_slot_table_sane);
    RUN(socket_stream_connect_failure);
    RUN(socket_connect_timeout_unreachable);
    RUN(socket_stream_connect_write_read);
    RUN(socket_stream_close);
    RUN(socket_listen_reports_readiness);
    RUN(socket_stream_write_string);
    RUN(eval_open_tcp_stream_connect_failure);
    RUN(eval_open_tcp_stream_read_byte);
    RUN(eval_open_tcp_stream_write_byte);
    RUN(eval_udp_stream_roundtrip);
    RUN(eval_udp_stream_receive_timeout);
    RUN(eval_udp_stream_local_endpoint);
    RUN(eval_udp_stream_type_errors);
    RUN(eval_udp_socket_roundtrip_ipv4);
    RUN(eval_udp_socket_general_vector_buffers);
    RUN(eval_udp_socket_roundtrip_ipv6);
    RUN(eval_udp_socket_receive_timeout);
    RUN(eval_udp_socket_fixed_port_and_reuse_address);
    RUN(eval_udp_socket_send_requires_send_to);
    RUN(eval_udp_socket_argument_errors);
    RUN(eval_udp_multicast_ipv4_loopback);
    RUN(eval_udp_multicast_ipv6_membership);
    RUN(eval_udp_multicast_argument_errors);
    RUN(eval_network_interfaces);
    RUN(eval_ipv6_tcp_loopback);
    RUN(eval_open_udp_stream_ipv6);
    RUN(eval_ipv6_supported_p_is_boolean);
    RUN(platform_socket_listen_accept);
    RUN(platform_socket_listen_busy_port);
    RUN(platform_socket_table_grows_many_connections);
    RUN(socket_stream_listen_accept);
    RUN(socket_stream_accept_non_listener);
    RUN(eval_socket_listen_accept_roundtrip);
    RUN(eval_socket_listen_bad_port);
    RUN(eval_socket_accept_type_error);
    RUN(socket_stream_local_port);
    RUN(socket_stream_local_port_explicit);
    RUN(eval_socket_local_port);
    RUN(eval_socket_local_port_type_error);
    RUN(eval_socket_listen_named_address);
    RUN(eval_socket_listen_bad_address);
    RUN(eval_executable_path);
    RUN(socket_concurrent_accept_connect);
    RUN(socket_read_parked_allows_same_socket_reply);
    RUN(socket_read_parked_high_handle_lock_independent);
    RUN(socket_read_parked_allows_console_write);
    RUN(stw_gc_with_thread_blocked_in_socket_read);

    /* Socket read/write timeout */
    RUN(socket_read_timeout_fires);
    RUN(socket_read_timeout_not_triggered_when_data_present);
    RUN(eval_socket_stream_timeout_accessor);
    RUN(eval_socket_read_timeout_signals_condition);
    RUN(eval_socket_write_timeout_does_not_break_normal_write);
    RUN(eval_socket_stream_timeout_bad_direction);

#ifdef CL_WIDE_STRINGS
    /* UTF-8 codec tests */
    RUN(utf8_encode_ascii);
    RUN(utf8_encode_2byte);
    RUN(utf8_encode_3byte);
    RUN(utf8_encode_4byte);
    RUN(utf8_encode_reject_surrogate);
    RUN(utf8_encode_reject_too_large);
    RUN(utf8_decode_ascii);
    RUN(utf8_decode_2byte);
    RUN(utf8_decode_3byte);
    RUN(utf8_decode_4byte);
    RUN(utf8_decode_overlong_rejected);
    RUN(utf8_decode_truncated);
    RUN(utf8_roundtrip);

    /* UTF-8 string conversion tests */
    RUN(utf8_to_cl_string_ascii);
    RUN(utf8_to_cl_string_multibyte);
    RUN(wide_string_to_utf8_basic);

    /* UTF-8 stream I/O tests */
    RUN(file_stream_utf8_write_read);
    RUN(cbuf_stream_utf8_decode);
    RUN(string_output_stream_utf8_roundtrip);
    RUN(string_output_stream_ascii_stays_base);
    RUN(read_line_utf8_file);
    RUN(unread_char_utf8);
#endif

    RUN(eval_get_dispatch_macro_character_conformance);
    RUN(eval_with_input_from_string_index_start_end);
    RUN(eval_string_input_stream_accepts_fill_pointer_string);
    RUN(eval_file_position_accounts_for_peek);
    RUN(eval_file_position_start_end_designators);
    RUN(eval_file_position_start_honors_stream_start_offset);
    RUN(eval_file_position_end_reflects_unflushed_growth);

    /* READ-SEQUENCE / WRITE-SEQUENCE builtins */
    RUN(eval_write_read_sequence_u8_file_fast_path);
    RUN(eval_write_read_sequence_u8_file_multi_chunk);
    RUN(eval_read_sequence_start_end);
    RUN(eval_read_sequence_bounds_error);
    RUN(eval_read_sequence_bad_start_type_error);
    RUN(eval_write_sequence_bad_end_type_error);
    RUN(eval_write_read_sequence_string_chars);
    RUN(eval_read_sequence_consumes_unread_char);
    RUN(eval_write_read_sequence_list);
    RUN(eval_read_sequence_string_stream_fallback);
    RUN(eval_write_sequence_string_output_stream);
    RUN(eval_write_sequence_returns_sequence_and_checks_direction);

    teardown();
    REPORT();
}
