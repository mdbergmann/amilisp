#ifndef CL_REPL_H
#define CL_REPL_H

/*
 * Read-Eval-Print Loop
 */

#include "types.h"

/* Run interactive REPL (returns on EOF or quit) */
void cl_repl(void);

/* Run batch REPL: no prompts, no result echo, comments skipped */
void cl_repl_batch(void);

/* Eval a single string (for testing) */
CL_Obj cl_eval_string(const char *str);

/* Load a file by path (for --load/--script) */
void cl_load_file(const char *path);

/* Init: load boot.lisp, optionally user init, set up history symbols */
void cl_repl_init(void);
void cl_repl_init_no_userinit(int no_userinit);

/* User init file candidates on AmigaOS/MorphOS, first found wins
 * (S:.amilisprc, then the pre-0.9.1 S:clamiga.lisp).  The host reads
 * ~/.amilisprc.  Exported so tests/test_userinit_paths.c can pin the
 * documented name; the list is compiled on every platform. */
extern const char *const cl_userinit_paths_amiga[];

/* Restored-boot init (--image): the heap already holds everything the
 * saving session had loaded, so boot.lisp/CLOS loading and the symbol
 * export passes are skipped entirely.  Runs the user init file (unless
 * suppressed) with EXT:*IMAGE-RESTORED-P* already T, then
 * EXT:*RESTORE-HOOKS*. */
/* Post-restore init (userinit + EXT:*RESTORE-HOOKS*).  image_ms is what
 * main() measured for staging + restoring the image, so --boot-log's
 * "image restored" line reports the real cost rather than 0. */
void cl_repl_init_from_image(int no_userinit, uint32_t image_ms);

/* When non-zero (the default), cl_repl_init suppresses "; [boot] ..."
 * progress lines.  main.c clears it only when --boot-log is given, so
 * normal output and piped tests that match output exactly stay clean. */
extern int cl_quiet_boot;
/* --no-fasl-cache: LOAD neither reads nor writes its implicit FASL cache
 * (builtins_io.c, load_fasl_cache_enabled). */
extern int cl_load_fasl_cache_off;

/* Minimal init: C builtins only, no boot.lisp/CLOS loading.
 * For unit tests that need cl_compile/cl_eval_string but not macros. */
void cl_repl_init_minimal(void);

/* Compute net parenthesis depth, skipping strings/comments/char literals */
int cl_paren_depth(const char *str);

/* Update REPL history variables (*, **, ***, /, //, ///, +, ++, +++).
 * VALUES is the list from cl_repl_values_list; its first element becomes *.
 * Called after each successful REPL eval. Exposed for testing. */
void cl_repl_update_history(CL_Obj form, CL_Obj values);

/* The list of ALL values the form just returned — what the REPL echoes and
 * what / is bound to.  `primary` is what cl_vm_eval returned; the rest come
 * from the current thread's MV state, so call this right after the eval,
 * before anything else can run Lisp code.  (values) yields NIL. */
CL_Obj cl_repl_values_list(CL_Obj primary);

/* Echo VALUES, one per line; nothing at all for the empty list.
 * COLORIZE wraps each value in the REPL's result color. */
void cl_repl_print_values(CL_Obj values, int colorize);

/* Non-zero if the form just evaluated produced at least one value, i.e.
 * whether the REPL should print anything for it — (values) returns zero
 * values and prints nothing.  `primary` is what cl_vm_eval returned; the
 * value count comes from the current thread's MV state, so call this
 * right after the eval.  Exposed for testing. */
int cl_repl_result_printable(CL_Obj primary);

#ifndef PLATFORM_AMIGA
/* Runtime-library directories to try relative to the amilisp executable's
 * own directory (platform_executable_prefix), in order — used for boot.lisp
 * (repl.c), REQUIRE modules (builtins_io.c) and heap-image discovery
 * (main.c), which must all agree on where an installation keeps lib/:
 *
 *   "lib/"             binary release / distribution dir: lib/ beside amilisp
 *   "../lib/amilisp/"  install prefix, the layout SBCL uses:
 *                      <prefix>/bin/amilisp + <prefix>/lib/amilisp/
 *   "../lib/clamiga/"  the same prefix layout as installed before the
 *                      rename to AmiLisp (0.12): an existing installation
 *                      keeps working
 *   "../../lib/"       in-repo build: build/host/amilisp, lib/ two levels up
 *
 * Each entry ends in '/' and is appended directly to the executable prefix.
 * AmigaOS uses PROGDIR: plus a ParentDir climb instead (see repl.c). */
extern const char *const cl_lib_rel_dirs[];
#define CL_LIB_REL_COUNT 4
#endif

#endif /* CL_REPL_H */
