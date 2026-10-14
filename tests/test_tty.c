/* Tests for the TTY primitives behind EXT:TTY-RAW-MODE / EXT:TTY-SIZE and
 * the console-stream LISTEN / READ-CHAR-NO-HANG availability probe.
 *
 * Motivated by cl-tuition (TEA-style TUI library): without raw mode the
 * cooked tty echoes every keypress (arrow keys as literal ^[[A text) onto
 * the drawn frame, input only arrives after Enter, and READ-CHAR-NO-HANG
 * on the console never reported availability at all — so a TUI's input
 * loop starved forever.
 *
 * The tests run against a real pseudo-terminal: the pty slave is dup2'd
 * over stdin, which is exactly how the primitives see a user's terminal.
 * (The test runner starts us with stdin at /dev/null, so the pty is also
 * what makes raw mode *possible* here.)  POSIX host only — the Amiga leg
 * lives in tests/amiga/run-tests.lisp.
 *
 * Windows has no pty to dup2 over stdin (ConPTY is a pipe protocol, not a
 * terminal file descriptor), so the Windows leg below tests the other half
 * of the contract instead: what the console primitives must do when stdin
 * is NOT a console — which is exactly the situation the test runner, and
 * every `amilisp < file` invocation, creates.
 */

#include "test.h"
#include "core/types.h"
#include "core/mem.h"
#include "core/error.h"
#include "core/package.h"
#include "core/symbol.h"
#include "core/reader.h"
#include "core/printer.h"
#include "core/compiler.h"
#include "core/vm.h"
#include "core/builtins.h"
#include "core/repl.h"
#include "core/stream.h"
#include "core/thread.h"
#include "platform/platform.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef PLATFORM_WIN32
#include <termios.h>
#include <sys/ioctl.h>
#endif

#ifndef PLATFORM_WIN32

static int pty_master = -1;
static int pty_slave = -1;
static int saved_stdin = -1;

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
    cl_stream_init();   /* binds *STANDARD-INPUT* &co (the LISTEN tests) */
    cl_builtins_init();
    cl_repl_init();
}

static void teardown(void)
{
    cl_mem_shutdown();
    platform_shutdown();
}

/* Route stdin through a fresh pty slave.  Returns 0 on success. */
static int pty_stdin_setup(void)
{
    struct winsize ws;

    pty_master = posix_openpt(O_RDWR | O_NOCTTY);
    if (pty_master < 0) return -1;
    if (grantpt(pty_master) != 0 || unlockpt(pty_master) != 0) return -1;
    pty_slave = open(ptsname(pty_master), O_RDWR | O_NOCTTY);
    if (pty_slave < 0) return -1;

    /* Give the pty a definite size so the size query has ground truth. */
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = 132;
    ws.ws_row = 43;
    if (ioctl(pty_slave, TIOCSWINSZ, &ws) != 0) return -1;

    saved_stdin = dup(STDIN_FILENO);
    if (saved_stdin < 0) return -1;
    if (dup2(pty_slave, STDIN_FILENO) < 0) return -1;
    return 0;
}

static void pty_stdin_restore(void)
{
    if (saved_stdin >= 0) {
        dup2(saved_stdin, STDIN_FILENO);
        close(saved_stdin);
        saved_stdin = -1;
    }
    if (pty_slave >= 0) { close(pty_slave); pty_slave = -1; }
    if (pty_master >= 0) { close(pty_master); pty_master = -1; }
}

/* Poll the availability probe for up to ~1s — pty delivery is fast but
 * not synchronous with the master-side write(). */
static int wait_char_avail(void)
{
    int i;
    for (i = 0; i < 100; i++) {
        if (platform_tty_char_avail())
            return 1;
        usleep(10000);
    }
    return 0;
}

/* Eval a string and return the prin1 representation of the result
 * (same helper as test_format.c). */
static const char *eval_print(const char *str)
{
    static char buf[1024];
    int err;

    CL_CATCH(err);
    if (err == CL_ERR_NONE) {
        CL_Obj result = cl_eval_string(str);
        cl_prin1_to_string(result, buf, sizeof(buf));
        CL_UNCATCH();
        return buf;
    } else {
        CL_UNCATCH();
        cl_vm.sp = 0;
        cl_vm.fp = 0;
        snprintf(buf, sizeof(buf), "ERROR:%d", err);
        return buf;
    }
}

TEST(raw_mode_toggles_termios)
{
    struct termios t;

    ASSERT_EQ_INT(platform_tty_raw_active(), 0);
    ASSERT_EQ_INT(platform_tty_raw(1), 0);
    ASSERT_EQ_INT(platform_tty_raw_active(), 1);

    /* Raw really means raw: no canonical mode, no echo, no signals. */
    ASSERT_EQ_INT(tcgetattr(STDIN_FILENO, &t), 0);
    ASSERT((t.c_lflag & (tcflag_t)ICANON) == 0);
    ASSERT((t.c_lflag & (tcflag_t)ECHO) == 0);
    ASSERT((t.c_lflag & (tcflag_t)ISIG) == 0);

    /* Enabling twice is idempotent. */
    ASSERT_EQ_INT(platform_tty_raw(1), 0);

    ASSERT_EQ_INT(platform_tty_raw(0), 0);
    ASSERT_EQ_INT(platform_tty_raw_active(), 0);

    /* Disable restored the saved cooked state. */
    ASSERT_EQ_INT(tcgetattr(STDIN_FILENO, &t), 0);
    ASSERT((t.c_lflag & (tcflag_t)ICANON) != 0);
    ASSERT((t.c_lflag & (tcflag_t)ECHO) != 0);

    /* Disabling twice is idempotent too. */
    ASSERT_EQ_INT(platform_tty_raw(0), 0);
}

TEST(raw_read_avail_and_pushback)
{
    ASSERT_EQ_INT(platform_tty_raw(1), 0);

    /* Nothing typed yet. */
    ASSERT_EQ_INT(platform_tty_char_avail(), 0);

    /* One keypress arrives without any Enter. */
    ASSERT_EQ_INT((int)write(pty_master, "x", 1), 1);
    ASSERT_EQ_INT(wait_char_avail(), 1);
    ASSERT_EQ_INT(platform_getchar(), 'x');
    ASSERT_EQ_INT(platform_tty_char_avail(), 0);

    /* Pushback is visible to the probe and comes back first. */
    platform_ungetchar('y');
    ASSERT_EQ_INT(platform_tty_char_avail(), 1);
    ASSERT_EQ_INT(platform_getchar(), 'y');
    ASSERT_EQ_INT(platform_tty_char_avail(), 0);

    ASSERT_EQ_INT(platform_tty_raw(0), 0);
}

TEST(tty_size_reports_winsize)
{
    int cols = 0, rows = 0;
    ASSERT_EQ_INT(platform_tty_size(&cols, &rows), 0);
    ASSERT_EQ_INT(cols, 132);
    ASSERT_EQ_INT(rows, 43);
}

/* The Lisp surface: EXT builtins plus the LISTEN / READ-CHAR-NO-HANG
 * console wiring a TUI input loop depends on. */
TEST(lisp_tty_builtins_and_input_loop)
{
    ASSERT_STR_EQ(eval_print("(ext:tty-p)"), "T");
    ASSERT_STR_EQ(eval_print("(ext:tty-size)"), "(132 . 43)");

    ASSERT_STR_EQ(eval_print("(ext:tty-raw-mode t)"), "T");

    /* No input pending: the input loop's poll must not block, and per
     * CLHS it returns NIL (":no char yet" — the :none eof-value is only
     * for end of file). */
    ASSERT_STR_EQ(eval_print("(listen)"), "NIL");
    ASSERT_STR_EQ(eval_print("(read-char-no-hang *standard-input* nil :none)"),
                  "NIL");

    /* A keypress (no Enter) becomes visible to LISTEN and readable. */
    ASSERT_EQ_INT((int)write(pty_master, "q", 1), 1);
    ASSERT_EQ_INT(wait_char_avail(), 1);
    ASSERT_STR_EQ(eval_print("(listen)"), "T");
    ASSERT_STR_EQ(eval_print("(read-char-no-hang *standard-input* nil :none)"),
                  "#\\q");
    ASSERT_STR_EQ(eval_print("(read-char-no-hang *standard-input* nil :none)"),
                  "NIL");

    /* *TERMINAL-IO* is bidirectional (CLHS) — it was historically bound to
     * the output-only stdout singleton, so (read-char t) saw instant EOF
     * and a TUI's input loop reading from *terminal-io* starved. */
    ASSERT_STR_EQ(eval_print("(input-stream-p *terminal-io*)"), "T");
    ASSERT_STR_EQ(eval_print("(output-stream-p *terminal-io*)"), "T");
    ASSERT_EQ_INT((int)write(pty_master, "w", 1), 1);
    ASSERT_EQ_INT(wait_char_avail(), 1);
    ASSERT_STR_EQ(eval_print("(listen *terminal-io*)"), "T");
    ASSERT_STR_EQ(eval_print("(read-char t)"), "#\\w");

    /* UNREAD-CHAR keeps LISTEN truthful (stream-level pushback). */
    ASSERT_EQ_INT((int)write(pty_master, "z", 1), 1);
    ASSERT_EQ_INT(wait_char_avail(), 1);
    ASSERT_STR_EQ(eval_print("(unread-char (read-char))"), "NIL");
    ASSERT_STR_EQ(eval_print("(listen)"), "T");
    ASSERT_STR_EQ(eval_print("(read-char)"), "#\\z");

    /* UNREAD-CHAR pushback on *STANDARD-INPUT* must be visible through
     * *TERMINAL-IO* — they now share the same backing console-input stream
     * (cl_stream_init makes *TERMINAL-IO* a two-way-stream over
     * *STANDARD-INPUT* and *STANDARD-OUTPUT* rather than an independent console
     * stream object). Before that fix, the pushback lived in a private
     * unread_char slot invisible across the two stream objects: LISTEN on
     * *terminal-io* reported NIL despite a pending char, and READ-CHAR on
     * *terminal-io* blocked waiting for genuinely new input. */
    ASSERT_EQ_INT((int)write(pty_master, "v", 1), 1);
    ASSERT_EQ_INT(wait_char_avail(), 1);
    ASSERT_STR_EQ(eval_print(
        "(unread-char (read-char *standard-input*) *standard-input*)"),
        "NIL");
    ASSERT_STR_EQ(eval_print("(listen *terminal-io*)"), "T");
    ASSERT_STR_EQ(eval_print("(read-char *terminal-io*)"), "#\\v");

    ASSERT_STR_EQ(eval_print("(ext:tty-raw-mode nil)"), "T");
}

/* Cooked (canonical) mode: after platform_read_line took one line, the
 * rest of a pasted multi-line form is type-ahead the probe MUST report.
 * The REPL asks exactly this before printing its continuation prompt —
 * when the next line is already there, the terminal has echoed it, and a
 * prompt would land after the echo, glued to the value (issue #14).  On a
 * tty the type-ahead is still in the kernel (canonical reads return one
 * line each), so this pins the select() half of the probe. */
TEST(cooked_typeahead_visible_after_line_read)
{
    char line[64];

    ASSERT_EQ_INT(platform_tty_raw_active(), 0);
    ASSERT_EQ_INT(platform_tty_char_avail(), 0);

    /* Two lines arrive in one burst, like a paste. */
    ASSERT_EQ_INT((int)write(pty_master, "a\nb\n", 4), 4);
    ASSERT_EQ_INT(wait_char_avail(), 1);
    ASSERT_EQ_INT(platform_read_line(line, sizeof(line)), 1);
    ASSERT_STR_EQ(line, "a");

    /* The second line is pending: no prompt should be printed now. */
    ASSERT_EQ_INT(wait_char_avail(), 1);
    ASSERT_EQ_INT(platform_read_line(line, sizeof(line)), 1);
    ASSERT_STR_EQ(line, "b");

    /* Nothing left: a human is being waited for, the prompt is due. */
    ASSERT_EQ_INT(platform_tty_char_avail(), 0);
}

/* Same contract with stdin on a PIPE (an editor or `printf ... | amilisp`):
 * one read slurps both lines into stdio's buffer, where the fd-level
 * probe cannot see the second one — the probe has to look inside stdio.
 * That peek exists on the libcs whose FILE layout is public (glibc, the
 * BSDs / macOS); elsewhere the probe is allowed to answer 0 (conservative:
 * the prompt is printed, the pre-fix behavior), so that assertion is
 * guarded by the same condition platform_posix.c compiles the peek under.
 * The fd-level half is pinned unconditionally: a line that arrives AFTER
 * the first read is visible everywhere, and an empty pipe reports 0. */
TEST(pipe_typeahead_visible_after_line_read)
{
    int fds[2];
    int hold;
    char line[64];

    ASSERT_EQ_INT(pipe(fds), 0);
    hold = dup(STDIN_FILENO);
    ASSERT(hold >= 0);
    ASSERT(dup2(fds[0], STDIN_FILENO) >= 0);

    /* Both lines already there: fgets buffers the second one. */
    ASSERT_EQ_INT((int)write(fds[1], "a\nb\n", 4), 4);
    ASSERT_EQ_INT(platform_read_line(line, sizeof(line)), 1);
    ASSERT_STR_EQ(line, "a");
#if defined(__GLIBC__) || defined(__APPLE__) || defined(__FreeBSD__) || \
    defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
    ASSERT_EQ_INT(platform_tty_char_avail(), 1);
#endif
    ASSERT_EQ_INT(platform_read_line(line, sizeof(line)), 1);
    ASSERT_STR_EQ(line, "b");

    /* Writer still open, nothing written: not pending. */
    ASSERT_EQ_INT(platform_tty_char_avail(), 0);

    /* A line that lands after the read is in the pipe, not in stdio. */
    ASSERT_EQ_INT((int)write(fds[1], "c\n", 2), 2);
    ASSERT_EQ_INT(wait_char_avail(), 1);
    ASSERT_EQ_INT(platform_read_line(line, sizeof(line)), 1);
    ASSERT_STR_EQ(line, "c");
    ASSERT_EQ_INT(platform_tty_char_avail(), 0);

    dup2(hold, STDIN_FILENO);
    close(hold);
    close(fds[0]);
    close(fds[1]);
}

TEST(non_tty_fails_gracefully)
{
    int devnull = open("/dev/null", O_RDONLY);
    int hold = dup(STDIN_FILENO);
    ASSERT(devnull >= 0 && hold >= 0);
    dup2(devnull, STDIN_FILENO);

    ASSERT_EQ_INT(platform_tty_raw(1), -1);
    ASSERT_EQ_INT(platform_tty_raw_active(), 0);
    ASSERT_STR_EQ(eval_print("(ext:tty-p)"), "NIL");
    ASSERT_STR_EQ(eval_print("(ext:tty-raw-mode t)"), "NIL");

    dup2(hold, STDIN_FILENO);
    close(hold);
    close(devnull);
}

int main(void)
{
    setup();

    if (pty_stdin_setup() != 0) {
        /* No pty available (exotic CI sandbox): report loudly but don't
         * fail the suite on infrastructure, not code. */
        printf("SKIP test_tty: could not allocate a pseudo-terminal\n");
        pty_stdin_restore();
        teardown();
        return 0;
    }

    RUN(raw_mode_toggles_termios);
    RUN(raw_read_avail_and_pushback);
    RUN(tty_size_reports_winsize);
    RUN(lisp_tty_builtins_and_input_loop);
    RUN(cooked_typeahead_visible_after_line_read);
    RUN(pipe_typeahead_visible_after_line_read);
    RUN(non_tty_fails_gracefully);

    pty_stdin_restore();
    teardown();
    REPORT();
}

#else  /* PLATFORM_WIN32 */

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

/* Redirected stdin (how the test runner and `amilisp < file` both run):
 * raw mode is impossible and must SAY so rather than half-succeed, and the
 * REPL must not be told it is talking to a person. */
TEST(non_console_stdin_reports_honestly)
{
    ASSERT_EQ_INT(platform_stdin_is_interactive(), 0);
    ASSERT_EQ_INT(platform_tty_raw(1), -1);
    ASSERT_EQ_INT(platform_tty_raw_active(), 0);
    /* Leaving raw mode reports -1 too when there is no terminal at all —
     * same answer the POSIX implementation gives for a non-tty stdin, and
     * harmless: platform_shutdown() only calls it while raw mode is on. */
    ASSERT_EQ_INT(platform_tty_raw(0), -1);
}

/* Redirected stdin still has to answer the availability probe, and answer it
 * without blocking: a file always has bytes ready, which is what makes
 * READ-CHAR-NO-HANG terminate on piped input. */
TEST(char_avail_on_redirected_stdin_never_blocks)
{
    int avail = platform_tty_char_avail();
    ASSERT(avail == 0 || avail == 1);
}

/* EXT:TTY-SIZE must degrade to NIL rather than invent a size when there is
 * no console to measure (the runner's stdout is a pipe under `make test`). */
TEST(lisp_tty_builtins_degrade_without_console)
{
    int cols = 0, rows = 0;
    int rc = platform_tty_size(&cols, &rows);
    ASSERT(rc == 0 || rc == -1);
    if (rc == 0) {
        ASSERT(cols > 0);
        ASSERT(rows > 0);
    }
    /* EXT:TTY-SIZE must agree with the primitive: NIL exactly when the
     * primitive could not measure, a (cols . rows) cons otherwise. */
    {
        CL_Obj size = cl_eval_string("(ext:tty-size)");
        if (rc == 0) {
            ASSERT(CL_CONS_P(size));
        } else {
            ASSERT(CL_NULL_P(size));
        }
    }
    /* Leaving raw mode is always safe to call, console or not. */
    ASSERT(CL_NULL_P(cl_eval_string("(ext:tty-raw-mode nil)")) ||
           cl_eval_string("(ext:tty-raw-mode nil)") == CL_T);
}

int main(void)
{
    setup();
    RUN(non_console_stdin_reports_honestly);
    RUN(char_avail_on_redirected_stdin_never_blocks);
    RUN(lisp_tty_builtins_degrade_without_console);
    REPORT();
}

#endif  /* PLATFORM_WIN32 */
