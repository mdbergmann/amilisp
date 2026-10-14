/*
 * cmdline.h — the program's own arguments and their Lisp surface.
 *
 * amilisp's command line is options; everything after a bare `--` is left
 * to the program (EXT:*COMMAND-LINE-ARGS*), never loaded.  A Workbench start
 * on AmigaOS has no command line at all: the platform layer synthesizes one
 * from the icons' tool types with the builder below (ARGS = the options,
 * every project icon = one argument after `--`), and EXT:*WORKBENCH-STARTED-P*
 * says so.  The builder is pure C so the host unit tests cover it.
 */
#ifndef CL_CMDLINE_H
#define CL_CMDLINE_H

#include "types.h"

/* An argv under construction in caller-owned storage: BUF holds the
 * NUL-terminated argument texts back to back, ARGV the pointers into it.
 * Nothing is allocated, so a Workbench start costs no off-heap memory
 * and there is nothing to hand back at exit. */
typedef struct {
    char  *buf;
    int    bufsize;
    int    bufpos;
    char **argv;
    int    argv_max;     /* slots, including the terminating NULL */
    int    argc;
    int    overflow;     /* set once an argument did not fit; it was dropped */
} CL_ArgvBuilder;

void cl_argv_builder_init(CL_ArgvBuilder *b, char *buf, int bufsize,
                          char **argv, int argv_max);

/* Append ARG verbatim as one argument.  Returns 1, or 0 (and sets
 * overflow) when it does not fit; argv[argc] stays NULL either way. */
int  cl_argv_builder_add(CL_ArgvBuilder *b, const char *arg);

/* Append the arguments written in LINE — an icon's ARGS tool type: split
 * at blanks and tabs, a double-quoted stretch is one argument with the
 * quotes removed (`--eval "(print 1)"`), and there is no escape character
 * (a tool type cannot hold a `"` inside quotes).  Returns the number of
 * arguments appended; a NULL or empty LINE appends none. */
int  cl_argv_builder_split(CL_ArgvBuilder *b, const char *line);

/* EXT:*COMMAND-LINE-ARGS* and EXT:*WORKBENCH-STARTED-P* (builtins init). */
void cl_cmdline_builtins_init(void);

/* Set the two variables for this process: ARGS are the N arguments after
 * `--`.  Conses, so it runs after the C init (and after an image restore,
 * whose heap carries the SAVING process's values) and before the user
 * init file and EXT:*RESTORE-HOOKS*, which may read them. */
void cl_cmdline_publish(int n, char **args, int workbench_started);

#endif /* CL_CMDLINE_H */
