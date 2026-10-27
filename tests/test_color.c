#include "test.h"
#include <string.h>

/*
 * The REPL colour tables (src/core/color.h) -- one per kind of console.
 *
 * An AmigaOS / MorphOS console renders SGR 30-37 as the screen's pens 0-7,
 * not as ANSI colours.  Only pens 0-3 are the same on every Workbench (the
 * system pens: background, text, shine, fill); 4-7 hold whatever the
 * user's palette does -- on the stock 8-colour OS 3.x palette the "cyan"
 * prompt (pen 6) came out brown on grey and could hardly be read -- and
 * 9x codes are ignored.  So the Amiga table must keep to the text pen (31),
 * the fill pen (33), bold (1) and inverse video (7), which read on the grey
 * Shell and on a black one alike: these tests parse every entry and reject
 * any other parameter, and pin the look of each role.  The host table is
 * pinned as it is so a change there is a deliberate one.
 *
 * color.h is a pure macro header, so the Amiga table is read by including
 * it a second time with PLATFORM_AMIGA defined.
 */

#include "core/color.h"

enum {
    R_PROMPT, R_DEBUG_PROMPT, R_VALUE, R_HEADING, R_LABEL, R_ART_PARENS,
    R_ART_LAMBDA, R_WARNING_TAG, R_WARNING, R_ERROR_TAG, R_ERROR,
    R_ERROR_STRONG, R_RESET, NROLES
};

static const char *role_names[NROLES] = {
    "PROMPT", "DEBUG_PROMPT", "VALUE", "HEADING", "LABEL", "ART_PARENS",
    "ART_LAMBDA", "WARNING_TAG", "WARNING", "ERROR_TAG", "ERROR",
    "ERROR_STRONG", "RESET"
};

#define TABLE_OF(name) static const char *name[NROLES] = { \
    CL_COLOR_PROMPT, CL_COLOR_DEBUG_PROMPT, CL_COLOR_VALUE, CL_COLOR_HEADING, \
    CL_COLOR_LABEL, CL_COLOR_ART_PARENS, CL_COLOR_ART_LAMBDA, \
    CL_COLOR_WARNING_TAG, CL_COLOR_WARNING, CL_COLOR_ERROR_TAG, CL_COLOR_ERROR, \
    CL_COLOR_ERROR_STRONG, CL_COLOR_RESET }

TABLE_OF(host_table);

static const char *host_expected[NROLES] = {
    "\033[36m", "\033[35m", "\033[32m", "\033[36m", "\033[33m", "\033[94m",
    "\033[31m", "\033[33m", "\033[33m", "\033[31m", "\033[31m",
    "\033[1;31m", "\033[0m"
};

#undef CL_COLOR_H
#undef CL_COLOR_PROMPT
#undef CL_COLOR_DEBUG_PROMPT
#undef CL_COLOR_VALUE
#undef CL_COLOR_HEADING
#undef CL_COLOR_LABEL
#undef CL_COLOR_ART_PARENS
#undef CL_COLOR_ART_LAMBDA
#undef CL_COLOR_WARNING_TAG
#undef CL_COLOR_WARNING
#undef CL_COLOR_ERROR_TAG
#undef CL_COLOR_ERROR
#undef CL_COLOR_ERROR_STRONG
#undef CL_COLOR_RESET
#define PLATFORM_AMIGA 1
#include "core/color.h"
#undef PLATFORM_AMIGA

TABLE_OF(amiga_table);

/* Parse "ESC [ p1 ; p2 ... m" into params; -1 when the shape is wrong. */
static int parse_sgr(const char *s, int *params, int max)
{
    int n = 0;
    if (s[0] != '\033' || s[1] != '[') return -1;
    s += 2;
    for (;;) {
        int v = 0, digits = 0;
        while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; digits++; }
        if (!digits || n >= max) return -1;
        params[n++] = v;
        if (*s == ';') { s++; continue; }
        if (*s == 'm' && s[1] == '\0') return n;
        return -1;
    }
}

static int has_param(const char *s, int wanted)
{
    int params[8], n = parse_sgr(s, params, 8), i;
    for (i = 0; i < n; i++) if (params[i] == wanted) return 1;
    return 0;
}

static int param_count(const char *s)
{
    int params[8];
    return parse_sgr(s, params, 8);
}

TEST(host_table_is_the_known_ansi_one)
{
    int i;
    for (i = 0; i < NROLES; i++) {
        if (strcmp(host_table[i], host_expected[i]) != 0)
            printf("  host %s differs\n", role_names[i]);
        ASSERT_STR_EQ(host_table[i], host_expected[i]);
    }
}

TEST(amiga_table_entries_are_well_formed_sgr)
{
    int i;
    for (i = 0; i < NROLES; i++) {
        int n = param_count(amiga_table[i]);
        if (n < 1) printf("  %s is not an SGR sequence\n", role_names[i]);
        ASSERT(n >= 1);
    }
}

TEST(amiga_table_uses_only_system_pens_bold_and_inverse)
{
    /* Allowed: 0 reset, 1 bold, 7 inverse, 31 (the text pen), 33 (the fill
     * pen).  Pen 0 is the background and pen 2 the shine (white on a grey
     * Shell, invisible on a black one); 34-37 are the user's pens 4-7
     * (brown, salmon); 4x background pens and 9x codes are out too. */
    int i, j, params[8];
    for (i = 0; i < NROLES; i++) {
        int n = parse_sgr(amiga_table[i], params, 8);
        for (j = 0; j < n; j++) {
            int p = params[j];
            int ok = (p == 0 || p == 1 || p == 7 || p == 31 || p == 33);
            if (!ok) printf("  %s selects SGR %d\n", role_names[i], p);
            ASSERT(ok);
        }
    }
}

TEST(amiga_prompts_are_bold_fill_pen)
{
    ASSERT(has_param(amiga_table[R_PROMPT], 33) && has_param(amiga_table[R_PROMPT], 1));
    ASSERT(has_param(amiga_table[R_DEBUG_PROMPT], 33) && has_param(amiga_table[R_DEBUG_PROMPT], 1));
}

TEST(amiga_banner_text_and_parens_are_the_fill_pen_lambda_bold)
{
    ASSERT(has_param(amiga_table[R_HEADING], 33) && !has_param(amiga_table[R_HEADING], 1));
    ASSERT(has_param(amiga_table[R_ART_PARENS], 33));
    ASSERT(has_param(amiga_table[R_ART_LAMBDA], 1) && !has_param(amiga_table[R_ART_LAMBDA], 33));
}

TEST(amiga_tags_are_inverse_badges_warning_on_the_fill_pen)
{
    ASSERT(has_param(amiga_table[R_WARNING_TAG], 7) && has_param(amiga_table[R_WARNING_TAG], 33));
    ASSERT(has_param(amiga_table[R_ERROR_TAG], 7) && !has_param(amiga_table[R_ERROR_TAG], 33));
}

TEST(amiga_error_text_is_bold_warning_text_plain)
{
    ASSERT(has_param(amiga_table[R_ERROR], 1) && !has_param(amiga_table[R_ERROR], 7));
    ASSERT(has_param(amiga_table[R_ERROR_STRONG], 1));
    ASSERT(param_count(amiga_table[R_WARNING]) == 1 && has_param(amiga_table[R_WARNING], 0));
}

TEST(amiga_values_stay_in_the_text_pen)
{
    ASSERT(param_count(amiga_table[R_VALUE]) == 1 && has_param(amiga_table[R_VALUE], 0));
}

TEST(amiga_nothing_is_inverse_except_the_tags)
{
    int i;
    for (i = 0; i < NROLES; i++) {
        if (i == R_WARNING_TAG || i == R_ERROR_TAG) continue;
        if (has_param(amiga_table[i], 7)) printf("  %s is inverse\n", role_names[i]);
        ASSERT(!has_param(amiga_table[i], 7));
    }
}

TEST(reset_is_sgr_0_everywhere)
{
    ASSERT_STR_EQ(amiga_table[R_RESET], "\033[0m");
    ASSERT_STR_EQ(host_table[R_RESET], "\033[0m");
}

int main(void)
{
    test_init();
    RUN(host_table_is_the_known_ansi_one);
    RUN(amiga_table_entries_are_well_formed_sgr);
    RUN(amiga_table_uses_only_system_pens_bold_and_inverse);
    RUN(amiga_prompts_are_bold_fill_pen);
    RUN(amiga_banner_text_and_parens_are_the_fill_pen_lambda_bold);
    RUN(amiga_tags_are_inverse_badges_warning_on_the_fill_pen);
    RUN(amiga_error_text_is_bold_warning_text_plain);
    RUN(amiga_values_stay_in_the_text_pen);
    RUN(amiga_nothing_is_inverse_except_the_tags);
    RUN(reset_is_sgr_0_everywhere);
    REPORT();
}
