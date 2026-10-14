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
#include "core/stream.h"
#include "core/repl.h"
#include "platform/platform.h"
#include <stdio.h>
#include <string.h>

/*
 * Regression tests for the single-source-of-truth version (types.h CL_VERSION_*).
 *
 * The version used to be spelled out twice: CL_VERSION_STRING "0.2" in types.h
 * (keying the FASL cache path) and a separate literal "0.2.0" inside
 * bi_lisp_implementation_version().  Bumping the header alone left the running
 * image reporting the old version, because nothing tied the two together.
 *
 * Two invariants are pinned here:
 *   1. LISP-IMPLEMENTATION-VERSION is DERIVED from CL_VERSION_* — it reports
 *      exactly CL_VERSION_STRING_FULL and carries CL_VERSION_STRING as a
 *      prefix.  Re-hardcoding a literal there fails these tests.
 *   2. The length is computed, not hardcoded.  The old call passed a literal
 *      5 for "0.2.0"; the first two-digit component ("0.10.0", 6 chars) would
 *      have silently truncated the string to "0.10.".
 */

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
    cl_builtins_init();
    cl_stream_init();
    cl_repl_init();
}

static void teardown(void)
{
    cl_stream_shutdown();
    cl_mem_shutdown();
    platform_shutdown();
}

static const char *eval_str(const char *expr)
{
    static char buf[256];
    int err;

    CL_CATCH(err);
    if (err == CL_ERR_NONE) {
        CL_Obj result = cl_eval_string(expr);
        cl_prin1_to_string(result, buf, sizeof(buf));
        CL_UNCATCH();
        return buf;
    } else {
        CL_UNCATCH();
        snprintf(buf, sizeof(buf), "ERROR:%d", err);
        return buf;
    }
}

/* --- Macro derivation (compile-time single source of truth) --- */

TEST(version_string_is_major_dot_minor)
{
    char expected[64];
    snprintf(expected, sizeof(expected), "%d.%d",
             CL_VERSION_MAJOR, CL_VERSION_MINOR);
    ASSERT_STR_EQ(expected, CL_VERSION_STRING);
}

TEST(version_string_full_is_major_dot_minor_dot_patch)
{
    char expected[64];
    snprintf(expected, sizeof(expected), "%d.%d.%d",
             CL_VERSION_MAJOR, CL_VERSION_MINOR, CL_VERSION_PATCH);
    ASSERT_STR_EQ(expected, CL_VERSION_STRING_FULL);
}

TEST(version_string_full_extends_version_string)
{
    /* The cache-path version must be a prefix of the full version, so the two
       can never drift apart the way the old duplicated literals did. */
    size_t n = strlen(CL_VERSION_STRING);
    ASSERT(strncmp(CL_VERSION_STRING_FULL, CL_VERSION_STRING, n) == 0);
    ASSERT(CL_VERSION_STRING_FULL[n] == '.');
}

TEST(version_len_full_matches_strlen)
{
    /* Guards the truncation bug: a hardcoded length silently cuts the string
       short once any component reaches two digits. */
    ASSERT_EQ_INT((int)CL_VERSION_LEN_FULL, (int)strlen(CL_VERSION_STRING_FULL));
}

TEST(version_stringify_expands_macro_not_name)
{
    /* Single-level stringification would yield "CL_VERSION_MAJOR"; the
       two-level CL_VERSION_STR indirection is what expands it to a number. */
    ASSERT(CL_VERSION_STRING[0] != 'C');
    ASSERT(strchr(CL_VERSION_STRING, '_') == NULL);
}

TEST(version_stringify_handles_two_digit_components)
{
    /* The macro machinery must survive the "0.10.0" case that broke the old
       hardcoded length. */
    ASSERT_STR_EQ("10", CL_VERSION_STR(10));
    ASSERT_EQ_INT(6, (int)strlen(CL_VERSION_STR(0) "." CL_VERSION_STR(10)
                                 "." CL_VERSION_STR(0)));
}

/* --- Runtime: the builtin reports the derived version --- */

TEST(lisp_implementation_version_is_derived)
{
    char expected[64];
    snprintf(expected, sizeof(expected), "\"%s\"", CL_VERSION_STRING_FULL);
    ASSERT_STR_EQ(expected, eval_str("(lisp-implementation-version)"));
}

TEST(lisp_implementation_version_not_truncated)
{
    /* prin1 adds the two quote characters. */
    char expected[64];
    snprintf(expected, sizeof(expected), "%d",
             (int)CL_VERSION_LEN_FULL + 2);
    ASSERT_STR_EQ(expected, eval_str("(length (prin1-to-string"
                                     " (lisp-implementation-version)))"));
}

TEST(lisp_implementation_version_starts_with_cache_version)
{
    ASSERT_STR_EQ("T", eval_str("(if (eql 0 (search \"" CL_VERSION_STRING "\""
                                " (lisp-implementation-version)))"
                                " t nil)"));
}

TEST(lisp_implementation_type_unchanged)
{
    ASSERT_STR_EQ("\"AmiLisp\"", eval_str("(lisp-implementation-type)"));
}

/* --- AmigaOS $VER: cookie (read by the Shell's Version command) --- */

TEST(version_cookie_has_amiga_layout)
{
    /* "$VER: <name> <ver>.<rev> (<DD.MM.YYYY>)" — the OS parses by prefix. */
    ASSERT(strncmp(cl_version_cookie, "$VER: amilisp ", 14) == 0);
    ASSERT(cl_version_cookie[strlen(cl_version_cookie) - 1] == ')');
}

TEST(version_cookie_carries_derived_version)
{
    /* Pins the cookie to CL_VERSION_* rather than a fourth hardcoded copy. */
    ASSERT(strstr(cl_version_cookie, " " CL_VERSION_STRING " ") != NULL);
    ASSERT(strstr(cl_version_cookie, "(" CL_VERSION_DATE ")") != NULL);
}

TEST(version_cookie_date_is_dd_mm_yyyy)
{
    /* AmigaOS Version rejects other date shapes; guard the separators. */
    ASSERT_EQ_INT(10, (int)strlen(CL_VERSION_DATE));
    ASSERT(CL_VERSION_DATE[2] == '.' && CL_VERSION_DATE[5] == '.');
}

int main(void)
{
    test_init();
    setup();

    RUN(version_string_is_major_dot_minor);
    RUN(version_string_full_is_major_dot_minor_dot_patch);
    RUN(version_string_full_extends_version_string);
    RUN(version_len_full_matches_strlen);
    RUN(version_stringify_expands_macro_not_name);
    RUN(version_stringify_handles_two_digit_components);
    RUN(lisp_implementation_version_is_derived);
    RUN(lisp_implementation_version_not_truncated);
    RUN(lisp_implementation_version_starts_with_cache_version);
    RUN(lisp_implementation_type_unchanged);
    RUN(version_cookie_has_amiga_layout);
    RUN(version_cookie_carries_derived_version);
    RUN(version_cookie_date_is_dd_mm_yyyy);

    teardown();
    REPORT();
}
