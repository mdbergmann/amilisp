#ifndef CL_COLOR_H
#define CL_COLOR_H

/* Global flag: 1 = emit ANSI color codes, 0 = plain text */
extern int cl_repl_color;

/* Emit ANSI escape to set color (no-op if cl_repl_color == 0) */
void cl_color_set(const char *ansi_code);

/* Emit ANSI reset (no-op if cl_repl_color == 0) */
void cl_color_reset(void);

/* The REPL's colours, one SGR sequence per role.
 *
 * Two tables.  A host terminal has ANSI colours.  An AmigaOS / MorphOS
 * console does not: SGR 30-37 select the screen's pens 0-7, and only pens
 * 0-3 mean the same on every Workbench (the system pens since 2.0: 0
 * background, 1 text, 2 shine, 3 fill -- grey, black, white, blue on a
 * stock palette).  Pens 4-7 hold whatever the user's palette does (brown
 * and salmon on the stock 8-colour OS 3.x palette, where 36 "cyan" came
 * out as unreadable brown on grey), a 4-colour screen folds them onto 0-3,
 * and 9x codes are ignored.  So the Amiga table draws with pen 3 (SGR 33,
 * the fill pen), bold (1), inverse video (7) and the text pen, which read
 * on the stock grey Shell and on a black one alike: the prompt in bold
 * blue, warnings behind a blue badge, errors behind a black one.  On a
 * console that renders ANSI colours after all (OS 3.9 on an RTG screen,
 * MorphOS) the same codes give bold yellow, a yellow badge and a white
 * one.  tests/test_color.c pins the table to those codes. */
#ifdef PLATFORM_AMIGA
#define CL_COLOR_PROMPT        "\033[1;33m"  /* PKG> : bold fill pen */
#define CL_COLOR_DEBUG_PROMPT  "\033[1;33m"  /* Debug> : the same */
#define CL_COLOR_VALUE         "\033[0m"     /* values: the text pen */
#define CL_COLOR_HEADING       "\033[33m"    /* banner text, Inspecting:, Inspect> */
#define CL_COLOR_LABEL         "\033[1m"     /* inspector paths and slot labels */
#define CL_COLOR_ART_PARENS    "\033[33m"    /* the banner's parens */
#define CL_COLOR_ART_LAMBDA    "\033[1;31m"  /* the banner's lambda: bold text pen */
#define CL_COLOR_WARNING_TAG   "\033[7;33m"  /* "WARNING:" inverse on the fill pen */
#define CL_COLOR_WARNING       "\033[0m"     /* the warning's text */
#define CL_COLOR_ERROR_TAG     "\033[7m"     /* "ERROR:" inverse */
#define CL_COLOR_ERROR         "\033[1m"     /* the error's text: bold */
#define CL_COLOR_ERROR_STRONG  "\033[1m"     /* "Debugger entered: ..." */
#else
#define CL_COLOR_PROMPT        "\033[36m"
#define CL_COLOR_DEBUG_PROMPT  "\033[35m"
#define CL_COLOR_VALUE         "\033[32m"
#define CL_COLOR_HEADING       "\033[36m"
#define CL_COLOR_LABEL         "\033[33m"
#define CL_COLOR_ART_PARENS    "\033[94m"
#define CL_COLOR_ART_LAMBDA    "\033[31m"
#define CL_COLOR_WARNING_TAG   "\033[33m"
#define CL_COLOR_WARNING       "\033[33m"
#define CL_COLOR_ERROR_TAG     "\033[31m"
#define CL_COLOR_ERROR         "\033[31m"
#define CL_COLOR_ERROR_STRONG  "\033[1;31m"
#endif
#define CL_COLOR_RESET         "\033[0m"

#endif
