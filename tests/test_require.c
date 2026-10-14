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
#include <stdlib.h>
#include <sys/stat.h>

/* Regression tests for REQUIRE/PROVIDE (builtins_io.c):
 *   - module-name accepts a character string designator (CLHS glossary)
 *   - GitHub #18: a symbol/keyword designator names the module in UPPER
 *     case ("ASDF" for :asdf) while the file is lib/asdf.lisp.  REQUIRE must
 *     retry the lib/ search with the lowercase spelling (only observable on a
 *     case-sensitive file system — Linux CI), and its "already loaded" test
 *     against *modules* must fold case so (require :gray-streams) after
 *     (require "gray-streams") does not load the module a second time.
 *     PROVIDE's own duplicate check stays exact, so a module may register
 *     both spellings like asdf.lisp does — matching SBCL's *modules*.
 *   - the host $CLAMIGA_HOME fallback lets amilisp find its lib/ when it is
 *     launched from a directory whose cwd-relative "lib/" does not contain the
 *     module — so an editor/Sly session can keep its working directory on the
 *     file buffer instead of pinning it to amilisp's source root.
 */

#define HOME_DIR "/tmp/cl_test_require_home"
#define HOME_LIB HOME_DIR "/lib"
#define HOME_MOD HOME_LIB "/cltestrequirehome.lisp"

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
    static char buf[4096];
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

/* --- Character string designator --- */

TEST(provide_accepts_character)
{
    /* A character denotes the singleton string containing it. */
    ASSERT_STR_EQ("T", eval_str("(provide #\\Z)"));
    ASSERT_STR_EQ("\"Z\"",
        eval_str("(find \"Z\" *modules* :test #'string=)"));
}

TEST(require_accepts_character_already_provided)
{
    eval_str("(provide #\\Q)");
    /* Already provided -> NIL, and crucially NOT a TYPE-ERROR on the char. */
    ASSERT_STR_EQ("NIL", eval_str("(require #\\Q)"));
}

TEST(provide_rejects_non_designator)
{
    /* A number is not a string designator. */
    ASSERT_STR_EQ("ERROR:2", eval_str("(provide 42)"));
}

/* --- GitHub #18: symbol designators and module-name case --- */

TEST(require_folds_case_against_modules)
{
    /* The module registered its (lowercase) file name; every designator
       spelling must see it as already loaded — no lib/ search, no error. */
    ASSERT_STR_EQ("T", eval_str("(provide \"case-fold-mod\")"));
    ASSERT_STR_EQ("NIL", eval_str("(require :case-fold-mod)"));
    ASSERT_STR_EQ("NIL", eval_str("(require 'case-fold-mod)"));
    ASSERT_STR_EQ("NIL", eval_str("(require \"CASE-FOLD-MOD\")"));
    ASSERT_STR_EQ("NIL", eval_str("(require \"Case-Fold-Mod\")"));
    /* ...and the other way round: provided in upper case, required in lower. */
    ASSERT_STR_EQ("T", eval_str("(provide \"UPPER-MOD\")"));
    ASSERT_STR_EQ("NIL", eval_str("(require \"upper-mod\")"));
    /* Folding is a membership rule, not a rewrite: *modules* keeps the
       spelling PROVIDE was given. */
    ASSERT_STR_EQ("\"case-fold-mod\"",
        eval_str("(find \"case-fold-mod\" *modules* :test #'string=)"));
    ASSERT_STR_EQ("NIL",
        eval_str("(find \"CASE-FOLD-MOD\" *modules* :test #'string=)"));
}

TEST(require_unknown_module_still_errors)
{
    /* Case folding must not turn a genuinely missing module into a silent
       NIL: nothing named this was provided, in any case. */
    ASSERT_STR_EQ("ERROR:1", eval_str("(require :cl-test-no-such-module-zz)"));
    ASSERT_STR_EQ("ERROR:1", eval_str("(require \"CL-TEST-NO-SUCH-MODULE-ZZ\")"));
}

TEST(provide_keeps_both_spellings)
{
    /* PROVIDE's duplicate check is exact (string=): asdf.lisp does
       (provide "asdf") (provide "ASDF") and both must land in *modules*,
       as in SBCL's ("ASDF" "asdf" "UIOP" "uiop"). */
    eval_str("(provide \"both-mod\")");
    eval_str("(provide \"BOTH-MOD\")");
    ASSERT_STR_EQ("2",
        eval_str("(count \"both-mod\" *modules* :test #'string-equal)"));
    /* Exact re-provide is still deduplicated. */
    eval_str("(provide \"BOTH-MOD\")");
    ASSERT_STR_EQ("2",
        eval_str("(count \"both-mod\" *modules* :test #'string-equal)"));
}

/* --- $CLAMIGA_HOME fallback --- */

static int write_home_module(void)
{
    FILE *f;
    mkdir(HOME_DIR, 0777);
    mkdir(HOME_LIB, 0777);
    f = fopen(HOME_MOD, "w");
    if (!f) return 0;
    fprintf(f, "(defvar *cl-test-require-home-ran* 42)\n");
    /* Load counter: DEFVAR would not reassign, so count the loads instead. */
    fprintf(f, "(defvar *cl-test-require-home-loads* 0)\n");
    fprintf(f, "(incf *cl-test-require-home-loads*)\n");
    fprintf(f, "(provide \"cltestrequirehome\")\n");
    fclose(f);
    return 1;
}

TEST(require_resolves_via_clamiga_home)
{
    /* The module does not exist under cwd-relative lib/, so the only way
       REQUIRE can find it is the $CLAMIGA_HOME/lib/ fallback. */
    ASSERT(write_home_module());
    setenv("CLAMIGA_HOME", HOME_DIR, 1);

    eval_str("(require \"cltestrequirehome\")");
    ASSERT_STR_EQ("42", eval_str("*cl-test-require-home-ran*"));

    unsetenv("CLAMIGA_HOME");
    remove(HOME_MOD);
    rmdir(HOME_LIB);
    rmdir(HOME_DIR);
}

TEST(require_clamiga_home_tolerates_trailing_slash)
{
    ASSERT(write_home_module());
    setenv("CLAMIGA_HOME", HOME_DIR "/", 1);

    /* Fresh symbol so the previous test's binding can't mask a failure. */
    eval_str("(makunbound '*cl-test-require-home-ran*)");
    eval_str("(setf *modules* "
             "  (remove \"cltestrequirehome\" *modules* :test #'string=))");
    eval_str("(require \"cltestrequirehome\")");
    ASSERT_STR_EQ("42", eval_str("*cl-test-require-home-ran*"));

    unsetenv("CLAMIGA_HOME");
    remove(HOME_MOD);
    rmdir(HOME_LIB);
    rmdir(HOME_DIR);
}

/* GitHub #18, the file-system half: (require :cltestrequirehome) hands
   REQUIRE the name "CLTESTREQUIREHOME", but the module lives in
   lib/cltestrequirehome.lisp.  On a case-insensitive file system (macOS,
   Amiga) the exact spelling happens to resolve; on Linux only the lowercase
   retry finds the file — that is the leg CI's ubuntu job exercises. */
TEST(require_symbol_designator_finds_lowercase_file)
{
    ASSERT(write_home_module());
    setenv("CLAMIGA_HOME", HOME_DIR, 1);

    eval_str("(makunbound '*cl-test-require-home-ran*)");
    eval_str("(makunbound '*cl-test-require-home-loads*)");
    eval_str("(setf *modules* "
             "  (remove \"cltestrequirehome\" *modules* :test #'string-equal))");
    ASSERT_STR_EQ("T", eval_str("(require :cltestrequirehome)"));
    ASSERT_STR_EQ("42", eval_str("*cl-test-require-home-ran*"));
    ASSERT_STR_EQ("1", eval_str("*cl-test-require-home-loads*"));
    /* The module provided its own (lowercase) name ... */
    ASSERT_STR_EQ("\"cltestrequirehome\"",
        eval_str("(find \"cltestrequirehome\" *modules* :test #'string=)"));
    /* ... and every later spelling is a no-op, not a reload. */
    ASSERT_STR_EQ("NIL", eval_str("(require :cltestrequirehome)"));
    ASSERT_STR_EQ("NIL", eval_str("(require 'cltestrequirehome)"));
    ASSERT_STR_EQ("NIL", eval_str("(require \"CLTESTREQUIREHOME\")"));
    ASSERT_STR_EQ("NIL", eval_str("(require \"cltestrequirehome\")"));
    ASSERT_STR_EQ("1", eval_str("*cl-test-require-home-loads*"));

    unsetenv("CLAMIGA_HOME");
    remove(HOME_MOD);
    rmdir(HOME_LIB);
    rmdir(HOME_DIR);
}

int main(void)
{
    test_init();
    setup();

    RUN(provide_accepts_character);
    RUN(require_accepts_character_already_provided);
    RUN(provide_rejects_non_designator);
    RUN(require_folds_case_against_modules);
    RUN(require_unknown_module_still_errors);
    RUN(provide_keeps_both_spellings);
    RUN(require_resolves_via_clamiga_home);
    RUN(require_clamiga_home_tolerates_trailing_slash);
    RUN(require_symbol_designator_finds_lowercase_file);

    teardown();
    REPORT();
}
