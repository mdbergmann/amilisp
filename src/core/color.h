#ifndef CL_COLOR_H
#define CL_COLOR_H

/* Global flag: 1 = emit ANSI color codes, 0 = plain text */
extern int cl_repl_color;

/* Emit ANSI escape to set color (no-op if cl_repl_color == 0) */
void cl_color_set(const char *ansi_code);

/* Emit ANSI reset (no-op if cl_repl_color == 0) */
void cl_color_reset(void);

/* ANSI color code constants.
 *
 * The names say what the host terminal shows.  An AmigaOS / MorphOS console
 * does not have ANSI colours: SGR 30-37 select the screen's pens 0-7, and
 * only pens 0-3 mean the same thing on every Workbench (the system pens
 * since 2.0: 0 background, 1 text, 2 shine, 3 fill -- grey, black, white,
 * blue on a stock palette).  Pens 4-7 are whatever the user's palette holds
 * (brown and salmon on the stock 8-colour OS 3.x palette, where 36 "cyan"
 * came out as unreadable brown on grey), a 4-colour screen folds them onto
 * 0-3, and 9x codes are ignored.  So the Amiga table uses pen 3 as the
 * accent (SGR 33), bold (SGR 1, no pen: the console's own text colour)
 * for errors, and the text pen for the rest.  On a console that does
 * render ANSI colours (OS 3.9 on an RTG screen, MorphOS) that is yellow
 * and bold, still readable.  tests/test_color.c pins the table to those
 * codes. */
#ifdef PLATFORM_AMIGA
#define CL_COLOR_LIGHT_BLUE   "\033[33m"     /* banner parens: pen 3 */
#define CL_COLOR_DIM_GREEN    "\033[0m"      /* values: the text pen */
#define CL_COLOR_DIM_CYAN     "\033[33m"     /* prompt, headings: pen 3 */
#define CL_COLOR_RED          "\033[1m"      /* errors: bold */
#define CL_COLOR_YELLOW       "\033[33m"     /* warnings, labels: pen 3 */
#define CL_COLOR_BOLD_RED     "\033[1m"      /* debugger entry: bold */
#define CL_COLOR_DIM_MAGENTA  "\033[33m"     /* Debug> prompt: pen 3 */
#else
#define CL_COLOR_LIGHT_BLUE   "\033[94m"
#define CL_COLOR_DIM_GREEN    "\033[32m"
#define CL_COLOR_DIM_CYAN     "\033[36m"
#define CL_COLOR_RED          "\033[31m"
#define CL_COLOR_YELLOW       "\033[33m"
#define CL_COLOR_BOLD_RED     "\033[1;31m"
#define CL_COLOR_DIM_MAGENTA  "\033[35m"
#endif
#define CL_COLOR_RESET        "\033[0m"

#endif
