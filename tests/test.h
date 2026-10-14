#ifndef TEST_H
#define TEST_H

/*
 * Minimal test framework for AmiLisp.
 * Usage:
 *   TEST(name) { ASSERT(...); ASSERT_EQ(...); }
 *   int main() { RUN(name); REPORT(); }
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#ifdef PLATFORM_WIN32
#include "../src/platform/win32_compat.h"
#include <errno.h>
#endif

static int test_pass = 0;
static int test_fail = 0;
static int test_current_failed = 0;
static volatile const char *test_current_name = "";
static volatile unsigned int test_current_name_len = 0;

/* Per-test watchdog (seconds).  A test that blocks (e.g. a thread/GC
 * deadlock that only manifests under a particular scheduler) would
 * otherwise hang the whole CI job until GitHub's job timeout.  The
 * SIGALRM handler names the offending test on stderr (unbuffered, so it
 * survives a hung process) and aborts so the run fails loudly and fast.
 * Override with TEST_WATCHDOG_SECS=0 to disable, or any value to retune. */
#ifndef TEST_WATCHDOG_DEFAULT_SECS
#define TEST_WATCHDOG_DEFAULT_SECS 120
#endif

static volatile unsigned int test_watchdog_secs = TEST_WATCHDOG_DEFAULT_SECS;

static void test_watchdog_fire(void)
{
    /* async-signal-safe: write() + the literal test name only */
    static const char msg[] = "\n*** TEST WATCHDOG: timed out (likely deadlock) in test: ";
    write(2, msg, sizeof(msg) - 1);
    if (test_current_name)
        write(2, (const char *)test_current_name, test_current_name_len);
    write(2, "\n", 1);
    /* abort() raises SIGABRT -> core/backtrace; not strictly async-signal
     * -safe but acceptable for a fatal watchdog in a test harness. */
    abort();
}

#ifdef PLATFORM_WIN32
/* Windows has no alarm(2).  A timer-queue timer is the closest equivalent:
 * the callback runs on a pool thread once the deadline passes, which is
 * fine — it only writes a fixed string and aborts, and aborting from any
 * thread kills the process just as SIGALRM does. */
static HANDLE test_wd_timer = NULL;

static void CALLBACK test_watchdog_cb(PVOID arg, BOOLEAN fired)
{
    (void)arg; (void)fired;
    test_watchdog_fire();
}

static void test_watchdog_arm(unsigned int secs)
{
    if (test_wd_timer) {
        DeleteTimerQueueTimer(NULL, test_wd_timer, NULL);
        test_wd_timer = NULL;
    }
    if (secs)
        CreateTimerQueueTimer(&test_wd_timer, NULL, test_watchdog_cb, NULL,
                              secs * 1000u, 0, WT_EXECUTEONLYONCE);
}
#else
static void test_watchdog_handler(int sig)
{
    (void)sig;
    test_watchdog_fire();
}

static void test_watchdog_arm(unsigned int secs)
{
    alarm(secs);
}
#endif

/* One-time harness setup: unbuffered stdout (so per-test progress survives a
 * hang on CI, where stdout is a pipe) + the SIGALRM watchdog.  Called lazily
 * from RUN so it applies even to test mains that don't call test_init(). */
static void test_setup_once(void)
{
    static int done = 0;
    const char *env;
    if (done) return;
    done = 1;
    setvbuf(stdout, NULL, _IONBF, 0);
    env = getenv("TEST_WATCHDOG_SECS");
    if (env)
        test_watchdog_secs = (unsigned int)strtoul(env, NULL, 10);
#ifdef PLATFORM_WIN32
    /* The unit tests write scratch files to POSIX-style absolute paths
     * ("/tmp/cf-test1.lisp") — 239 literals across eleven files.  A native
     * Windows binary resolves that to \tmp on the current drive, which is
     * not a directory Windows ships: on a fresh checkout every one of those
     * tests fails with "cannot open file" instead of the assertion it meant
     * to make, and COMPILE-FILE-PATHNAME cannot even build a cache path
     * (make_fasl_cache_path realpath()s the source's DIRECTORY, which has to
     * exist).  Create it once, here, so a Windows checkout behaves like a
     * POSIX one where /tmp is simply always there.
     *
     * The shell tests take the other route — the Makefile hands them TMPDIR
     * in Windows spelling.  Teaching the C tests to honour TMPDIR as well
     * would be the tidier end state; it is a mechanical but large change to
     * those 239 literals, most of which sit inside Lisp forms. */
    if (mkdir("/tmp", 0777) != 0 && errno != EEXIST)
        fprintf(stderr, "WARNING: cannot create the scratch directory \\tmp "
                        "on this drive (%s) — tests that write scratch files "
                        "will fail\n", strerror(errno));
#endif
#ifndef PLATFORM_WIN32
    if (test_watchdog_secs)
        signal(SIGALRM, test_watchdog_handler);
#endif
}

#define TEST(name) static void test_##name(void)

#define RUN(name) do { \
    test_setup_once(); \
    test_current_name = #name; \
    test_current_name_len = (unsigned int)(sizeof(#name) - 1); \
    test_current_failed = 0; \
    if (test_watchdog_secs) test_watchdog_arm(test_watchdog_secs); \
    test_##name(); \
    if (test_watchdog_secs) test_watchdog_arm(0); \
    if (test_current_failed) { \
        printf("FAIL  %s\n", #name); \
        test_fail++; \
    } else { \
        printf("  ok  %s\n", #name); \
        test_pass++; \
    } \
    fflush(stdout); \
} while(0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("  ASSERT FAILED: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        test_current_failed = 1; \
    } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        printf("  ASSERT_EQ FAILED: %s:%d: %s != %s\n", \
               __FILE__, __LINE__, #a, #b); \
        test_current_failed = 1; \
    } \
} while(0)

#define ASSERT_EQ_INT(a, b) do { \
    int _a = (a), _b = (b); \
    if (_a != _b) { \
        printf("  ASSERT_EQ_INT FAILED: %s:%d: %s = %d, expected %d\n", \
               __FILE__, __LINE__, #a, _a, _b); \
        test_current_failed = 1; \
    } \
} while(0)

#define ASSERT_STR_EQ(a, b) do { \
    const char *_a = (a), *_b = (b); \
    if (strcmp(_a, _b) != 0) { \
        printf("  ASSERT_STR_EQ FAILED: %s:%d: \"%s\" != \"%s\"\n", \
               __FILE__, __LINE__, _a, _b); \
        test_current_failed = 1; \
    } \
} while(0)

#define REPORT() do { \
    printf("\n%d passed, %d failed, %d total\n", \
           test_pass, test_fail, test_pass + test_fail); \
    return test_fail > 0 ? 1 : 0; \
} while(0)

/* Common init for all tests — call at start of main() (optional: RUN also
 * lazily performs this setup, so tests that omit it are still covered). */
static void test_init(void)
{
    test_setup_once();

    /* Suppress unused warning when test.h is included */
    (void)test_pass;
    (void)test_fail;
    (void)test_current_failed;
    (void)test_current_name;
    (void)test_current_name_len;
    (void)test_watchdog_arm;
}

#endif /* TEST_H */
