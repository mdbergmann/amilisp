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
 * 9x codes are ignored.  So the Amiga table must keep to the text pen,
 * the fill pen (SGR 33) and bold (SGR 1): these tests parse every entry
 * and reject any other parameter.  The host table is pinned as it is so
 * a change there is a deliberate one.
 *
 * color.h is a pure macro header, so the Amiga table is read by including
 * it a second time with PLATFORM_AMIGA defined.
 */

#include "core/color.h"

static const char *host_table[] = {
    CL_COLOR_LIGHT_BLUE, CL_COLOR_DIM_GREEN, CL_COLOR_DIM_CYAN, CL_COLOR_RED,
    CL_COLOR_YELLOW, CL_COLOR_BOLD_RED, CL_COLOR_DIM_MAGENTA, CL_COLOR_RESET
};
static const char *host_expected[] = {
    "\033[94m", "\033[32m", "\033[36m", "\033[31m",
    "\033[33m", "\033[1;31m", "\033[35m", "\033[0m"
};

#undef CL_COLOR_H
#undef CL_COLOR_LIGHT_BLUE
#undef CL_COLOR_DIM_GREEN
#undef CL_COLOR_DIM_CYAN
#undef CL_COLOR_RED
#undef CL_COLOR_YELLOW
#undef CL_COLOR_BOLD_RED
#undef CL_COLOR_DIM_MAGENTA
#undef CL_COLOR_RESET
#define PLATFORM_AMIGA 1
#include "core/color.h"
#undef PLATFORM_AMIGA

static const char *amiga_table[] = {
    CL_COLOR_LIGHT_BLUE, CL_COLOR_DIM_GREEN, CL_COLOR_DIM_CYAN, CL_COLOR_RED,
    CL_COLOR_YELLOW, CL_COLOR_BOLD_RED, CL_COLOR_DIM_MAGENTA, CL_COLOR_RESET
};
static const char *amiga_names[] = {
    "LIGHT_BLUE", "DIM_GREEN", "DIM_CYAN", "RED",
    "YELLOW", "BOLD_RED", "DIM_MAGENTA", "RESET"
};
#define NCOLORS 8

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

TEST(host_table_is_the_known_ansi_one)
{
    int i;
    for (i = 0; i < NCOLORS; i++)
        ASSERT_STR_EQ(host_table[i], host_expected[i]);
}

TEST(amiga_table_differs_from_host_where_host_uses_pens_4_to_7_or_9x)
{
    /* The entries that were unreadable on a stock Shell: 36 (pen 6, brown),
     * 35 (pen 5), 32 (pen 2, white on grey), 94 (ignored). */
    ASSERT(strcmp(amiga_table[2], host_expected[2]) != 0);   /* DIM_CYAN: the prompt */
    ASSERT(strcmp(amiga_table[6], host_expected[6]) != 0);   /* DIM_MAGENTA: Debug> */
    ASSERT(strcmp(amiga_table[1], host_expected[1]) != 0);   /* DIM_GREEN: values */
    ASSERT(strcmp(amiga_table[0], host_expected[0]) != 0);   /* LIGHT_BLUE: 9x */
}

TEST(amiga_table_entries_are_well_formed_sgr)
{
    int i, params[8];
    for (i = 0; i < NCOLORS; i++) {
        int n = parse_sgr(amiga_table[i], params, 8);
        if (n < 1) printf("  %s is not an SGR sequence\n", amiga_names[i]);
        ASSERT(n >= 1);
    }
}

TEST(amiga_table_uses_only_system_pens_and_bold)
{
    /* Allowed: 0 reset, 1 bold, 31 (the text pen), 33 (the fill pen).
     * Pen 0 is the background and pen 2 the shine (white on a grey Shell);
     * errors use bold alone, so a console that remaps "red" to its nearest
     * pen (OS 3.9's) cannot make them vanish either. */
    int i, j, params[8];
    for (i = 0; i < NCOLORS; i++) {
        int n = parse_sgr(amiga_table[i], params, 8);
        for (j = 0; j < n; j++) {
            int p = params[j];
            int ok = (p == 0 || p == 1 || p == 31 || p == 33);
            if (!ok) printf("  %s selects SGR %d\n", amiga_names[i], p);
            ASSERT(ok);
        }
    }
}

TEST(amiga_prompt_and_warnings_use_the_fill_pen)
{
    int params[8];
    ASSERT(parse_sgr(amiga_table[2], params, 8) == 1 && params[0] == 33); /* DIM_CYAN */
    ASSERT(parse_sgr(amiga_table[4], params, 8) == 1 && params[0] == 33); /* YELLOW */
    ASSERT(parse_sgr(amiga_table[6], params, 8) == 1 && params[0] == 33); /* DIM_MAGENTA */
    ASSERT(parse_sgr(amiga_table[0], params, 8) == 1 && params[0] == 33); /* LIGHT_BLUE */
}

TEST(amiga_errors_are_bold)
{
    int i, params[8], n, j, bold;
    static const int idx[] = { 3, 5 };   /* RED, BOLD_RED */
    for (i = 0; i < 2; i++) {
        n = parse_sgr(amiga_table[idx[i]], params, 8);
        bold = 0;
        for (j = 0; j < n; j++) if (params[j] == 1) bold = 1;
        ASSERT(bold);
    }
}

TEST(amiga_values_stay_in_the_text_pen)
{
    int params[8];
    int n = parse_sgr(amiga_table[1], params, 8);   /* DIM_GREEN */
    ASSERT(n == 1 && params[0] == 0);
}

TEST(reset_is_sgr_0_everywhere)
{
    ASSERT_STR_EQ(amiga_table[7], "\033[0m");
    ASSERT_STR_EQ(host_table[7], "\033[0m");
}

int main(void)
{
    test_init();
    RUN(host_table_is_the_known_ansi_one);
    RUN(amiga_table_differs_from_host_where_host_uses_pens_4_to_7_or_9x);
    RUN(amiga_table_entries_are_well_formed_sgr);
    RUN(amiga_table_uses_only_system_pens_and_bold);
    RUN(amiga_prompt_and_warnings_use_the_fill_pen);
    RUN(amiga_errors_are_bold);
    RUN(amiga_values_stay_in_the_text_pen);
    RUN(reset_is_sgr_0_everywhere);
    REPORT();
}
