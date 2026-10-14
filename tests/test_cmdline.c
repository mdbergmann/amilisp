#include "test.h"
#include "core/cmdline.h"
#include <string.h>

/*
 * The argv builder of src/core/cmdline.c -- what a Workbench start on
 * AmigaOS feeds main() with: the tool's and the projects' ARGS tool types
 * split like a command line, then `--` and one path per project.  Pure C,
 * so its rules are pinned here on the host:
 *
 *   blanks and tabs separate arguments; runs of them are one separator
 *   an argument that starts with a double quote runs to the next quote,
 *     quotes removed, blanks kept; a quote inside a word is a character
 *   an unterminated quote runs to the end of the line
 *   an empty quoted stretch is an empty argument (--eval "")
 *   there is no escape character
 *   argv[argc] is NULL at every step
 *   an argument that does not fit (text or slot) is dropped, the overflow
 *     flag set, and everything already added is intact
 *   a NULL / empty line adds nothing
 */

#define BUFSZ 64
#define SLOTS 6

static char   buf[BUFSZ];
static char  *argv[SLOTS];
static CL_ArgvBuilder b;

static void fresh(void)
{
    memset(buf, 'X', sizeof(buf));
    cl_argv_builder_init(&b, buf, BUFSZ, argv, SLOTS);
}

TEST(init_is_empty_and_terminated)
{
    fresh();
    ASSERT_EQ(b.argc, 0);
    ASSERT(argv[0] == NULL);
    ASSERT_EQ(b.overflow, 0);
}

TEST(add_appends_verbatim)
{
    fresh();
    ASSERT_EQ(cl_argv_builder_add(&b, "amilisp"), 1);
    ASSERT_EQ(cl_argv_builder_add(&b, "two words"), 1);
    ASSERT_EQ(b.argc, 2);
    ASSERT_STR_EQ(argv[0], "amilisp");
    ASSERT_STR_EQ(argv[1], "two words");
    ASSERT(argv[2] == NULL);
}

TEST(split_at_blanks_and_tabs)
{
    fresh();
    ASSERT_EQ(cl_argv_builder_split(&b, "  --heap 8M\t--non-interactive  "), 3);
    ASSERT_EQ(b.argc, 3);
    ASSERT_STR_EQ(argv[0], "--heap");
    ASSERT_STR_EQ(argv[1], "8M");
    ASSERT_STR_EQ(argv[2], "--non-interactive");
    ASSERT(argv[3] == NULL);
}

TEST(split_quotes_group_and_vanish)
{
    fresh();
    ASSERT_EQ(cl_argv_builder_split(&b, "--eval \"(print 1)\" x"), 3);
    ASSERT_STR_EQ(argv[0], "--eval");
    ASSERT_STR_EQ(argv[1], "(print 1)");
    ASSERT_STR_EQ(argv[2], "x");
}

TEST(split_quote_inside_a_word_is_an_ordinary_character)
{
    /* only a quote that STARTS an argument groups: a"b c" is the two
     * arguments a"b and c" -- there is no escape and no mid-word quoting */
    fresh();
    ASSERT_EQ(cl_argv_builder_split(&b, "a\"b c\""), 2);
    ASSERT_STR_EQ(argv[0], "a\"b");
    ASSERT_STR_EQ(argv[1], "c\"");
}

TEST(split_unterminated_quote_runs_to_the_end)
{
    fresh();
    ASSERT_EQ(cl_argv_builder_split(&b, "--eval \"(print 1) --load"), 2);
    ASSERT_STR_EQ(argv[1], "(print 1) --load");
}

TEST(split_empty_quotes_is_an_empty_argument)
{
    fresh();
    ASSERT_EQ(cl_argv_builder_split(&b, "\"\" x"), 2);
    ASSERT_STR_EQ(argv[0], "");
    ASSERT_STR_EQ(argv[1], "x");
}

TEST(split_null_or_empty_adds_nothing)
{
    fresh();
    ASSERT_EQ(cl_argv_builder_split(&b, NULL), 0);
    ASSERT_EQ(cl_argv_builder_split(&b, ""), 0);
    ASSERT_EQ(cl_argv_builder_split(&b, " \t "), 0);
    ASSERT_EQ(b.argc, 0);
    ASSERT(argv[0] == NULL);
}

TEST(add_and_split_interleave_like_a_workbench_start)
{
    fresh();
    cl_argv_builder_add(&b, "amilisp");
    cl_argv_builder_split(&b, "--heap 8M");
    cl_argv_builder_add(&b, "--");
    cl_argv_builder_add(&b, "Work:src/a.lisp");
    ASSERT_EQ(b.argc, 5);
    ASSERT_STR_EQ(argv[0], "amilisp");
    ASSERT_STR_EQ(argv[1], "--heap");
    ASSERT_STR_EQ(argv[2], "8M");
    ASSERT_STR_EQ(argv[3], "--");
    ASSERT_STR_EQ(argv[4], "Work:src/a.lisp");
    ASSERT(argv[5] == NULL);
    ASSERT_EQ(b.overflow, 0);
}

TEST(slot_overflow_drops_and_flags)
{
    /* SLOTS = 6 means five arguments plus the NULL */
    fresh();
    ASSERT_EQ(cl_argv_builder_split(&b, "1 2 3 4 5 6 7"), 5);
    ASSERT_EQ(b.argc, 5);
    ASSERT_STR_EQ(argv[4], "5");
    ASSERT(argv[5] == NULL);
    ASSERT_EQ(b.overflow, 1);
    /* still full afterwards */
    ASSERT_EQ(cl_argv_builder_add(&b, "x"), 0);
    ASSERT_EQ(b.argc, 5);
}

TEST(text_overflow_drops_and_flags)
{
    char big[BUFSZ + 8];
    fresh();
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    ASSERT_EQ(cl_argv_builder_add(&b, "ok"), 1);
    ASSERT_EQ(cl_argv_builder_add(&b, big), 0);
    ASSERT_EQ(b.overflow, 1);
    ASSERT_EQ(b.argc, 1);
    ASSERT_STR_EQ(argv[0], "ok");
    ASSERT(argv[1] == NULL);
}

TEST(text_fills_the_buffer_exactly)
{
    /* 63 characters + NUL = the whole buffer */
    char s[BUFSZ];
    fresh();
    memset(s, 'b', BUFSZ - 1);
    s[BUFSZ - 1] = '\0';
    ASSERT_EQ(cl_argv_builder_add(&b, s), 1);
    ASSERT_EQ(b.bufpos, BUFSZ);
    ASSERT_EQ(cl_argv_builder_add(&b, ""), 0);   /* not even a NUL fits */
    ASSERT_EQ(b.overflow, 1);
}

TEST(split_stops_at_the_first_argument_that_does_not_fit)
{
    fresh();
    ASSERT_EQ(cl_argv_builder_split(&b, "a b c d e f g h"), 5);
    ASSERT_EQ(b.overflow, 1);
    ASSERT_STR_EQ(argv[0], "a");
    ASSERT_STR_EQ(argv[4], "e");
}

int main(void)
{
    test_init();

    RUN(init_is_empty_and_terminated);
    RUN(add_appends_verbatim);
    RUN(split_at_blanks_and_tabs);
    RUN(split_quotes_group_and_vanish);
    RUN(split_quote_inside_a_word_is_an_ordinary_character);
    RUN(split_unterminated_quote_runs_to_the_end);
    RUN(split_empty_quotes_is_an_empty_argument);
    RUN(split_null_or_empty_adds_nothing);
    RUN(add_and_split_interleave_like_a_workbench_start);
    RUN(slot_overflow_drops_and_flags);
    RUN(text_overflow_drops_and_flags);
    RUN(text_fills_the_buffer_exactly);
    RUN(split_stops_at_the_first_argument_that_does_not_fit);

    REPORT();
}
