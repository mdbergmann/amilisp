#include "repl.h"
#include "reader.h"
#include "printer.h"
#include "compiler.h"
#include "vm.h"
#include "error.h"
#include "debugger.h"
#include "mem.h"
#include "symbol.h"
#include "package.h"
#include "stream.h"
#include "color.h"
#include "fasl.h"
#include "image.h"
#include "../platform/platform.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define REPL_BUF_SIZE 4096

/* Get current package name length */
static int pkg_name_len(void)
{
    if (CL_HEAP_P(cl_current_package)) {
        CL_Package *pkg = (CL_Package *)CL_OBJ_TO_PTR(cl_current_package);
        CL_String *name = (CL_String *)CL_OBJ_TO_PTR(pkg->name);
        return (int)name->length;
    }
    return 8; /* strlen("CL-AMIGA") */
}

/* Print REPL prompt showing current package name */
static void repl_prompt(void)
{
    cl_color_set(CL_COLOR_DIM_CYAN);
    if (CL_HEAP_P(cl_current_package)) {
        CL_Package *pkg = (CL_Package *)CL_OBJ_TO_PTR(cl_current_package);
        CL_String *name = (CL_String *)CL_OBJ_TO_PTR(pkg->name);
        cl_write_cstring_to_stdout(name->data);
    } else {
        cl_write_cstring_to_stdout("CL-AMIGA");
    }
    cl_write_cstring_to_stdout("> ");
    cl_color_reset();
}

/* Print continuation prompt (spaces matching package name width + "> ") */
static void repl_continuation_prompt(void)
{
    int i, len = pkg_name_len();
    cl_color_set(CL_COLOR_DIM_CYAN);
    for (i = 0; i < len; i++)
        cl_write_cstring_to_stdout(" ");
    cl_write_cstring_to_stdout("> ");
    cl_color_reset();
}

/* Load a file by path (C string). Used by --load and --script. */
void cl_load_file(const char *path)
{
    unsigned long size;
    char *buf = platform_file_read(path, &size);
    if (!buf) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Cannot open file: %s", path);
        cl_error(CL_ERR_GENERAL, msg);
        return;
    }

    {
        const char *prev_file = cl_current_source_file;
        const char *own_file;
        uint16_t own_file_id;
        uint16_t prev_file_id = cl_current_file_id;
        int prev_line = cl_reader_get_line();
        /* Per CL spec, LOAD binds *package* so in-package in loaded file
           doesn't affect the caller */
        CL_Obj saved_package = cl_current_package;
        CL_Obj stream, load_pathname_obj, load_truename_obj;
        CL_Obj saved_load_pathname, saved_load_truename;

        /* GC SAFETY: these saved dynamic values are restored after the load
         * evaluates arbitrary code — they must be forwarded roots or the
         * restore writes stale pre-compaction offsets into the globals. */
        CL_GC_PROTECT(saved_package);

        /* Bind *load-pathname* and *load-truename* per CL spec */
        {
            extern CL_Obj cl_parse_namestring(const char *str, uint32_t len);
            load_pathname_obj = cl_parse_namestring(path, (uint32_t)strlen(path));
        }
        load_truename_obj = load_pathname_obj;
        CL_GC_PROTECT(load_pathname_obj);
        CL_GC_PROTECT(load_truename_obj);
        saved_load_pathname = cl_symbol_value(SYM_STAR_LOAD_PATHNAME);
        saved_load_truename = cl_symbol_value(SYM_STAR_LOAD_TRUENAME);
        CL_GC_PROTECT(saved_load_pathname);
        CL_GC_PROTECT(saved_load_truename);
        cl_set_symbol_value(SYM_STAR_LOAD_PATHNAME, load_pathname_obj);
        cl_set_symbol_value(SYM_STAR_LOAD_TRUENAME, load_truename_obj);

        cl_current_source_file = cl_intern_source_file(path);
        cl_current_file_id++;
        cl_reader_reset_line();
        own_file = cl_current_source_file;
        own_file_id = cl_current_file_id;

        /* Use C-buffer stream — file content stays outside GC arena */
        stream = cl_make_cbuf_input_stream(buf, (uint32_t)size);
        if (CL_NULL_P(stream)) {
            /* Not reachable today — --load files run one after the other at
             * top level, so a slot is always free — but reading from NIL
             * would "load" nothing, quietly (see bi_load).  Restore the load
             * context mutated above before erroring — mirrors the
             * CL_ERR_EXIT branch below — so a future caller re-entering
             * cl_load_file mid-load doesn't see stale *LOAD-PATHNAME*,
             * *LOAD-TRUENAME*, or source-file/line state after the longjmp. */
            platform_free(buf);
            cl_set_symbol_value(SYM_STAR_LOAD_PATHNAME, saved_load_pathname);
            cl_set_symbol_value(SYM_STAR_LOAD_TRUENAME, saved_load_truename);
            CL_GC_UNPROTECT(5); /* saved_lt, saved_lp, lt_obj, lp_obj, saved_package */
            cl_current_source_file = prev_file;
            cl_current_file_id = prev_file_id;
            cl_reader_set_line(prev_line);
            cl_error(CL_ERR_GENERAL,
                     "LOAD: cannot open a source stream for %s - out of "
                     "memory, or over 250 source files being read at once", path);
            return;
        }
        CL_GC_PROTECT(stream);

        for (;;) {
            CL_Obj expr, bytecode;
            int err;
            int saved_gc_roots = gc_root_count;

            /* Clear any error-time frame snapshot left by a previous form's
             * (handler-case'd) error.  cl_debug_base_fp is set on every error
             * (cl_capture_backtrace) and is only meaningful while the debugger
             * inspects that error; left stale it makes an ad-hoc EXT:BACKTRACE
             * in a later form report a truncated frame window instead of the
             * live stack.  Resetting per top-level form keeps the snapshot
             * valid within the form that errors (debugger runs before the next
             * form) while preventing cross-form leakage. */
            cl_debug_base_fp = 0;

            /* see bi_load's file loop: an abandoned nested LOAD leaves its
             * file behind */
            cl_current_source_file = own_file;
            cl_current_file_id = own_file_id;
            expr = cl_read_from_stream(stream);
            if (cl_reader_eof()) break;

            CL_CATCH(err);
            if (err == CL_ERR_NONE) {
                CL_GC_PROTECT(expr);
                bytecode = cl_compile(expr);
                CL_GC_UNPROTECT(1);
                if (!CL_NULL_P(bytecode))
                    cl_vm_eval(bytecode);
                CL_UNCATCH();
            } else {
                gc_root_count = saved_gc_roots;
                CL_UNCATCH();
                /* Propagate exit request — don't swallow (quit) */
                if (err == CL_ERR_EXIT) {
                    cl_stream_close(stream);
                    platform_free(buf);
                    cl_set_symbol_value(SYM_STAR_LOAD_PATHNAME, saved_load_pathname);
                    cl_set_symbol_value(SYM_STAR_LOAD_TRUENAME, saved_load_truename);
                    CL_GC_UNPROTECT(1); /* stream */
                    CL_GC_UNPROTECT(5); /* saved_lt, saved_lp, lt_obj, lp_obj, saved_package */
                    cl_current_source_file = prev_file;
                    cl_current_file_id = prev_file_id;
                    cl_reader_set_line(prev_line);
                    cl_current_package = saved_package;
                    cl_set_symbol_value(SYM_STAR_PACKAGE, saved_package);
                    cl_error(CL_ERR_EXIT, "");
                }
                cl_error_print();
            }
        }

        CL_GC_UNPROTECT(1); /* stream */
        cl_stream_close(stream);
        platform_free(buf);

        /* Restore *load-pathname* and *load-truename* */
        cl_set_symbol_value(SYM_STAR_LOAD_PATHNAME, saved_load_pathname);
        cl_set_symbol_value(SYM_STAR_LOAD_TRUENAME, saved_load_truename);
        CL_GC_UNPROTECT(5); /* saved_lt, saved_lp, lt_obj, lp_obj, saved_package */

        cl_current_source_file = prev_file;
        cl_current_file_id = prev_file_id;
        cl_reader_set_line(prev_line);

        /* Restore *package* — in-package in loaded file must not leak */
        cl_current_package = saved_package;
        cl_set_symbol_value(SYM_STAR_PACKAGE, saved_package);
    }
}

/* The user init file on AmigaOS/MorphOS, first found wins: S:.amilisprc
 * is the documented name (the README, the release notes and Clamacs all
 * say so, and it mirrors ~/.amilisprc on the host); S:.clamigarc is the
 * name before the rename to AmiLisp (0.12) and S:clamiga.lisp what the
 * runtime read before 0.9.1 -- both stay as fallbacks so an existing file
 * keeps working.  tests/test_userinit_paths.c pins the contract. */
const char *const cl_userinit_paths_amiga[] = {
    "S:.amilisprc", "S:.clamigarc", "S:clamiga.lisp", NULL
};

/* Try to load user init file */
static void load_user_init(void)
{
#ifdef PLATFORM_AMIGA
    const char *const *paths = cl_userinit_paths_amiga;
#else
    static char user_init_path[512];
    static char user_init_path_old[512];
    static const char *paths[3];
    const char *home;
    /* Escape hatch for hermetic environments (the test suite, CI): when
       CLAMIGA_NO_USERINIT is set to a non-empty value, never read the
       developer's ~/.amilisprc.  C unit tests call cl_repl_init() directly and
       cannot pass the --no-userinit CLI flag, so they rely on this env var
       instead.  An empty value counts as unset so a child can override an
       inherited export via `CLAMIGA_NO_USERINIT= ...`. */
    {
        const char *e = getenv("CLAMIGA_NO_USERINIT");
        if (e != NULL && e[0] != '\0') {
            return;
        }
    }
    home = getenv("HOME");
    if (home) {
        /* ~/.amilisprc first, then the pre-rename ~/.clamigarc */
        snprintf(user_init_path, sizeof(user_init_path),
                 "%s/.amilisprc", home);
        snprintf(user_init_path_old, sizeof(user_init_path_old),
                 "%s/.clamigarc", home);
        paths[0] = user_init_path;
        paths[1] = user_init_path_old;
        paths[2] = NULL;
    } else {
        paths[0] = NULL;
    }
#endif
    {
        int i;
        for (i = 0; paths[i] != NULL; i++) {
            unsigned long size;
            char *buf = platform_file_read(paths[i], &size);
            if (buf) {
                CL_Obj cl_str, stream;
                const char *prev_file = cl_current_source_file;
                const char *own_file;
                uint16_t own_file_id;
                uint16_t prev_file_id = cl_current_file_id;
                int prev_line = cl_reader_get_line();
                cl_current_source_file = cl_intern_source_file(paths[i]);
                cl_current_file_id++;
                cl_reader_reset_line();
                own_file = cl_current_source_file;
                own_file_id = cl_current_file_id;

                cl_str = cl_make_string(buf, (int)size);
                CL_GC_PROTECT(cl_str);
                stream = cl_make_string_input_stream(cl_str, 0, (uint32_t)size);
                CL_GC_PROTECT(stream);

                platform_free(buf);

                for (;;) {
                    CL_Obj expr, bytecode;
                    int err;
                    int saved_gc_roots = gc_root_count;

                    cl_current_source_file = own_file;  /* as above */
                    cl_current_file_id = own_file_id;
                    expr = cl_read_from_stream(stream);
                    if (cl_reader_eof()) break;

                    CL_CATCH(err);
                    if (err == CL_ERR_NONE) {
                        CL_GC_PROTECT(expr);
                        bytecode = cl_compile(expr);
                        CL_GC_UNPROTECT(1);
                        if (!CL_NULL_P(bytecode))
                            cl_vm_eval(bytecode);
                        CL_UNCATCH();
                    } else {
                        gc_root_count = saved_gc_roots;
                        CL_UNCATCH();
                        if (err == CL_ERR_EXIT) {
                            CL_GC_UNPROTECT(2); /* stream, cl_str */
                            cl_current_source_file = prev_file;
                            cl_current_file_id = prev_file_id;
                            cl_reader_set_line(prev_line);
                            cl_error(CL_ERR_EXIT, "");
                        }
                        cl_error_print();
                    }
                }

                CL_GC_UNPROTECT(2);
                cl_current_source_file = prev_file;
                cl_current_file_id = prev_file_id;
                cl_reader_set_line(prev_line);
                return;  /* Loaded, stop trying */
            }
        }
    }
    /* No user init found — silently continue */
}

/* Compute net parenthesis depth of a string, skipping:
 * - String literals ("..." with \" escape handling)
 * - Line comments (; to end of line)
 * - Character literals (#\( #\) #\Space etc.) */
/* Skip whitespace and comments in paren-depth scanner, return new index */
static int pd_skip_ws(const char *str, int i)
{
    while (str[i] != '\0') {
        if (str[i] == ' ' || str[i] == '\t' || str[i] == '\n' || str[i] == '\r') {
            i++;
        } else if (str[i] == ';') {
            while (str[i] != '\0' && str[i] != '\n') i++;
        } else {
            break;
        }
    }
    return i;
}

/* Skip a single token (symbol/number) in paren-depth scanner, return new index */
static int pd_skip_token(const char *str, int i)
{
    while (str[i] != '\0' && str[i] != ' ' && str[i] != '\t' &&
           str[i] != '\n' && str[i] != '\r' && str[i] != '(' &&
           str[i] != ')' && str[i] != '"' && str[i] != ';')
        i++;
    return i;
}

/* Skip a balanced form in paren-depth scanner (for feature expressions).
 * Returns new index, or -1 if form is incomplete. */
static int pd_skip_form(const char *str, int i)
{
    i = pd_skip_ws(str, i);
    if (str[i] == '\0') return -1;
    if (str[i] == '(') {
        int d = 1;
        i++;
        while (str[i] != '\0' && d > 0) {
            if (str[i] == '(') d++;
            else if (str[i] == ')') d--;
            else if (str[i] == '"') {
                i++;
                while (str[i] != '\0') {
                    if (str[i] == '\\' && str[i + 1] != '\0') { i += 2; continue; }
                    if (str[i] == '"') break;
                    i++;
                }
            } else if (str[i] == ';') {
                while (str[i] != '\0' && str[i] != '\n') i++;
                continue;
            }
            if (str[i] != '\0') i++;
        }
        return (d == 0) ? i : -1;
    }
    if (str[i] == '"') {
        i++;
        while (str[i] != '\0') {
            if (str[i] == '\\' && str[i + 1] != '\0') { i += 2; continue; }
            if (str[i] == '"') { i++; return i; }
            i++;
        }
        return -1;
    }
    /* Token */
    if (str[i] == '#' || str[i] == '\'') return -1;  /* complex — be conservative */
    return pd_skip_token(str, i);
}

int cl_paren_depth(const char *str)
{
    int depth = 0;
    int i = 0;
    int pending_forms = 0;  /* forms still needed by #+/#- */

    while (str[i] != '\0') {
        char c = str[i];

        /* String literal — skip to closing quote */
        if (c == '"') {
            int closed = 0;
            i++;
            while (str[i] != '\0') {
                if (str[i] == '\\' && str[i + 1] != '\0') {
                    i += 2;  /* skip escaped char */
                    continue;
                }
                if (str[i] == '"') {
                    i++;
                    closed = 1;
                    break;
                }
                i++;
            }
            /* Unterminated string: a CLHS 2.4.5 string literal runs to the
             * closing quote, newlines included, so the form needs more input
             * — signal "incomplete" for the continuation prompt rather than
             * letting the reader raise "Unterminated string" (issue #14). */
            if (!closed)
                return 1;
            if (pending_forms > 0 && depth == 0) pending_forms--;
            continue;
        }

        /* Line comment — skip to end of line */
        if (c == ';') {
            while (str[i] != '\0' && str[i] != '\n')
                i++;
            continue;
        }

        /* Character literal #\x — skip the character name */
        if (c == '#' && str[i + 1] == '\\') {
            i += 2;  /* skip #\ */
            /* Skip character name (e.g., Space, Newline, or single char) */
            if (str[i] != '\0') {
                /* Check for named character (alphabetic sequence) */
                if ((str[i] >= 'A' && str[i] <= 'Z') ||
                    (str[i] >= 'a' && str[i] <= 'z')) {
                    while (str[i] != '\0' &&
                           ((str[i] >= 'A' && str[i] <= 'Z') ||
                            (str[i] >= 'a' && str[i] <= 'z')))
                        i++;
                } else {
                    i++;  /* single non-alpha char like #\( */
                }
            }
            if (pending_forms > 0 && depth == 0) pending_forms--;
            continue;
        }

        /* #+ / #- feature conditionals: need feature-expr + one more form */
        if (c == '#' && (str[i + 1] == '+' || str[i + 1] == '-')) {
            int j;
            i += 2;  /* skip #+ or #- */
            /* Skip the feature expression */
            j = pd_skip_form(str, i);
            if (j < 0) {
                /* Incomplete feature expr — definitely need more input */
                return 1;
            }
            i = j;
            /* Now we need one more form (the conditioned form) */
            pending_forms++;
            continue;
        }

        if (c == '(') {
            depth++;
        } else if (c == ')') {
            depth--;
            if (pending_forms > 0 && depth == 0) pending_forms--;
        } else if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            /* Start of an atom/token — skip it */
            int start = i;
            i = pd_skip_token(str, i);
            if (i > start && pending_forms > 0 && depth == 0) pending_forms--;
            continue;
        }

        i++;
    }

    if (pending_forms > 0) return 1;  /* still waiting for conditioned form */
    return depth;
}

CL_Obj cl_eval_string(const char *str)
{
    CL_ReadStream stream;
    CL_Obj expr, bytecode, result;

    stream.buf = str;
    stream.pos = 0;
    stream.len = (int)strlen(str);
    stream.line = 1;

    expr = cl_read_from_string(&stream);
    /* Nothing evaluated — leave a definitive "no values" signal rather than
     * whatever MV state the PREVIOUS call left behind, or a caller gating
     * its echo on cl_repl_result_printable() (the Debug> prompt) prints
     * NIL for input that produced nothing.
     *
     * KNOWN LIMITATION (deliberately not fixed here): a form that READS as
     * NIL — `nil`, `()` — is indistinguishable from "nothing was read",
     * since both yield CL_NULL_P(expr), so it is skipped instead of
     * evaluated and lands in this branch too.  cl_repl / cl_repl_batch
     * conflate the two exactly the same way, which is why a top-level `nil`
     * echoes nothing where SBCL prints NIL.  Separating them needs a reader
     * signal for "read nothing" (comment-only input relies on the
     * conflation today) and must land in all three loops at once — fixing
     * it here alone would only make the Debug> prompt disagree with the
     * REPL above it. */
    if (CL_NULL_P(expr)) {
        cl_mv_count = 0;
        return CL_NIL;
    }

    CL_GC_PROTECT(expr);
    bytecode = cl_compile(expr);
    CL_GC_UNPROTECT(1);

    if (CL_NULL_P(bytecode)) {
        cl_mv_count = 0;   /* nothing ran — same signal as above */
        return CL_NIL;
    }

    result = cl_vm_eval(bytecode);
    return result;
}

/* Does the form the REPL just evaluated have any values to print?
 *
 * cl_mv_count == 0 means the form returned zero values — (values) — and
 * per CLHS 25.1 the REPL prints the values of the form, so zero values
 * print nothing at all (not NIL).
 *
 * The non-NIL guard mirrors OP_MV_TO_LIST's disambiguation (vm.c): many
 * value-propagating opcodes (OP_LOAD, OP_DUP, OP_POP, ...) leave the MV
 * buffer untouched, so a non-NIL primary with count 0 is a stale leak
 * from an earlier (values) rather than a true zero-value return — print
 * it.  A real (values) leaves the primary NIL, so it still prints
 * nothing.  Call this immediately after cl_vm_eval, before anything that
 * could evaluate Lisp code and overwrite the MV state. */
int cl_repl_result_printable(CL_Obj primary)
{
    return cl_mv_count != 0 || !CL_NULL_P(primary);
}

/* All the values the form just returned, as a freshly consed list — the
 * REPL prints every one of them (CLHS 25.1: "the results of the evaluation
 * are printed"), and the same list becomes / .
 *
 * Mirrors OP_MV_TO_LIST, including its stale-count disambiguation: index 0
 * is taken from the returned primary rather than cl_mv_values[0], because
 * the opcodes that write `cl_mv_count = 1` without touching the buffer
 * (OP_CONST, OP_GLOAD, the arithmetic ops, …) leave slot 0 holding an
 * older value.  Call immediately after cl_vm_eval, before anything that
 * could run Lisp code and overwrite the MV state. */
CL_Obj cl_repl_values_list(CL_Obj primary)
{
    CL_Obj list = CL_NIL;
    int count = cl_mv_count;
    int i;

    if (count <= 0)
        return cl_repl_result_printable(primary) ? cl_cons(primary, CL_NIL)
                                                 : CL_NIL;
    if (count > CL_MAX_MV) count = CL_MAX_MV;

    /* cl_cons can compact; cl_mv_values[] is GC-marked and forwarded for
     * i < mv_count (mem.c), so re-read each slot inside the loop, and keep
     * the partially built list and the primary rooted. */
    CL_GC_PROTECT(primary);
    CL_GC_PROTECT(list);
    for (i = count - 1; i >= 1; i--)
        list = cl_cons(cl_mv_values[i], list);
    list = cl_cons(primary, list);
    CL_GC_UNPROTECT(2);
    return list;
}

/* Echo one value per line, SBCL-style: (floor 7 2) prints 3 then 1, and
 * (values) prints nothing at all.  VALUES is the list from
 * cl_repl_values_list; printing runs Lisp (PRINT-OBJECT methods), so it is
 * kept rooted across the walk. */
void cl_repl_print_values(CL_Obj values, int colorize)
{
    if (!CL_CONS_P(values))
        return;
    /* Root values before the fresh-line below: the write can block in the
     * stream layer (a GC safe region), letting a peer thread compact while
     * the list is otherwise invisible — cl_repl_batch passes it unrooted. */
    CL_GC_PROTECT(values);
    /* Values start on a fresh line (issue #14): after (format t "~a" "hi")
     * the NIL goes below the hi, not glued to it.  CLHS leaves the top-level
     * loop's format open — this matches SBCL.  A form that ended at column 0
     * (or printed nothing) gets no extra blank line, which is what makes
     * this FRESH-LINE and not TERPRI. */
    cl_stream_sym_fresh_line(SYM_STANDARD_OUTPUT);
    while (CL_CONS_P(values)) {
        if (colorize) cl_color_set(CL_COLOR_DIM_GREEN);
        cl_prin1(cl_car(values));
        if (colorize) cl_color_reset();
        cl_write_cstring_to_stdout("\n");
        values = cl_cdr(values);
    }
    CL_GC_UNPROTECT(1);
}

/* Update REPL history variables after a successful eval (CLHS 25.1.1).
 * form = the expression that was read, values = its values (cl_repl_values_list),
 * whose first element is the primary value stored in *. */
void cl_repl_update_history(CL_Obj form, CL_Obj values)
{
    CL_Obj primary = CL_CONS_P(values) ? cl_car(values) : CL_NIL;

    /* Shift ***: *** <- **, ** <- *, * <- primary value */
    cl_set_symbol_value(SYM_STARSTARSTAR, cl_symbol_value(SYM_STARSTAR));
    cl_set_symbol_value(SYM_STARSTAR, cl_symbol_value(SYM_STAR));
    cl_set_symbol_value(SYM_STAR, primary);

    /* Shift ///: /// <- //, // <- /, / <- the whole values list */
    cl_set_symbol_value(SYM_SLASHSLASHSLASH, cl_symbol_value(SYM_SLASHSLASH));
    cl_set_symbol_value(SYM_SLASHSLASH, cl_symbol_value(SYM_SLASH));
    cl_set_symbol_value(SYM_SLASH, values);

    /* Shift +++: +++ <- ++, ++ <- +, + <- form */
    cl_set_symbol_value(SYM_PLUSPLUSPLUS, cl_symbol_value(SYM_PLUSPLUS));
    cl_set_symbol_value(SYM_PLUSPLUS, cl_symbol_value(SYM_PLUS));
    cl_set_symbol_value(SYM_PLUS, form);
}

void cl_repl(void)
{
    char accum[REPL_BUF_SIZE];
    char line[1024];
    int accum_len = 0;
    int depth = 0;

    cl_debugger_enabled = 1;
    repl_prompt();

    while (platform_read_line(line, sizeof(line))) {
        int line_len = (int)strlen(line);

        /* Safe point: run pending compaction (no C locals hold CL_Obj here) */
        cl_gc_compact_if_pending();

        /* The Enter that delivered this line put the console cursor at
         * column 0 — mirror that in *standard-output*'s column tracking so
         * the FRESH-LINE in cl_repl_print_values is exact.  Batch mode
         * (cl_repl_batch) must NOT do this: it prints no prompts and its
         * input is never echoed, so there charpos is already the truth. */
        cl_stream_sym_note_bol(SYM_STANDARD_OUTPUT);

        /* When accumulator is empty, apply line-level skip rules */
        if (accum_len == 0) {
            /* Empty line: skip */
            if (line[0] == '\0') {
                repl_prompt();
                continue;
            }
            /* Skip lines starting with -- (CLI args leaked to stdin on AmigaOS) */
            if (line[0] == '-' && line[1] == '-') {
                repl_prompt();
                continue;
            }
        }

        /* Append line to accumulation buffer (with newline separator) */
        if (accum_len > 0 && accum_len < REPL_BUF_SIZE - 1) {
            accum[accum_len++] = '\n';
        }
        if (accum_len + line_len < REPL_BUF_SIZE - 1) {
            memcpy(accum + accum_len, line, line_len);
            accum_len += line_len;
        } else {
            /* Overflow used to drop the line silently, then evaluate the
             * truncated (often unbalanced or wrong) form.  Discard and say
             * so instead. */
            cl_write_cstring_to_stdout(
                "; Error: input form exceeds the REPL buffer (4095 chars) "
                "- discarded.\n"
                ";        Load long forms from a file with (load ...) "
                "instead.\n");
            accum_len = 0;
            depth = 0;
            repl_prompt();
            continue;
        }
        accum[accum_len] = '\0';

        /* Check paren depth */
        depth = cl_paren_depth(accum);

        if (depth > 0) {
            /* Incomplete expression — show the continuation prompt, but
             * only if we are actually about to wait for a human.  When the
             * rest of the form is already pending (a pasted multi-line
             * form, or an editor that sends the whole form at once) the
             * terminal has echoed those lines before we get here, so a
             * prompt now would land AFTER them, glued to the value's first
             * line (issue #14):
             *
             *     COMMON-LISP-USER> "
             *     "
             *                     > "        <- prompt after the echo
             *     "
             *
             * Skipping it gives the transcript a typed-form-by-form REPL
             * (SBCL, which has no continuation prompt at all) shows. */
            if (!platform_tty_char_avail())
                repl_continuation_prompt();
            continue;
        }

        /* Complete expression — evaluate */

        /* Quit command */
        if (strcmp(accum, "(quit)") == 0 || strcmp(accum, "(QUIT)") == 0 ||
            strcmp(accum, "(exit)") == 0 || strcmp(accum, "(EXIT)") == 0) {
            break;
        }

        {
            int err;
            CL_CATCH(err);
            if (err == CL_ERR_NONE) {
                /* Inline read/compile/eval for history variable support */
                CL_ReadStream stream;
                CL_Obj expr, bytecode, result;

                stream.buf = accum;
                stream.pos = 0;
                stream.len = accum_len;
                stream.line = 1;

                /* Top level has no source file; a LOAD that an earlier input
                 * left by a non-local exit never restored that (see bi_load's
                 * file loop). */
                cl_current_source_file = NULL;
                cl_current_file_id = 0;
                expr = cl_read_from_string(&stream);
                if (!CL_NULL_P(expr) && !cl_reader_eof()) {
                    /* Set - to current form */
                    cl_set_symbol_value(SYM_MINUS, expr);

                    /* expr stays protected through the EVAL too: the
                     * history update below stores it into +, and an
                     * unprotected expr is stale after eval compacts.
                     * On error the longjmp restores the root count. */
                    CL_GC_PROTECT(expr);
                    bytecode = cl_compile(expr);

                    if (!CL_NULL_P(bytecode)) {
                        CL_Obj values;
                        result = cl_vm_eval(bytecode);
                        /* Capture the MV state before anything else runs
                         * Lisp — the history update and PRINT-OBJECT
                         * methods both overwrite it. */
                        values = cl_repl_values_list(result);
                        CL_GC_PROTECT(values);

                        /* Update history: *, **, ***, /, //, ///, + ... */
                        cl_repl_update_history(expr, values);

                        cl_repl_print_values(values, 1);
                        CL_GC_UNPROTECT(1);  /* values */
                    }
                    CL_GC_UNPROTECT(1);
                }
                CL_UNCATCH();
            } else if (err == CL_ERR_EXIT) {
                CL_UNCATCH();
                break;
            } else {
                cl_error_print();
                /* Reset VM state after error (prevent stale frames) */
                cl_vm.sp = 0;
                cl_vm.fp = 0;
                CL_UNCATCH();
            }
        }

        /* Deferred EXT:SAVE-IMAGE dump: executes here, between top-level
         * forms, where the main thread is at rest (empty VM/dyn/NLX/
         * handler/restart/gc-root stacks).  Returns 1 for :quit t. */
        if (cl_image_save_run_if_pending())
            break;

        /* Reset accumulation buffer */
        accum_len = 0;
        depth = 0;
        repl_prompt();
    }

    cl_write_cstring_to_stdout("\nBye.\n");
}

void cl_repl_batch(void)
{
    char accum[REPL_BUF_SIZE];
    char line[1024];
    int accum_len = 0;
    int depth = 0;

    while (platform_read_line(line, sizeof(line))) {
        int line_len = (int)strlen(line);
        int i;

        /* When accumulator is empty, apply line-level skip rules */
        if (accum_len == 0) {
            /* Skip empty lines */
            if (line[0] == '\0') continue;

            /* Skip comment lines and non-Lisp lines */
            i = 0;
            while (line[i] == ' ' || line[i] == '\t') i++;
            if (line[i] == ';') continue;
            /* Skip lines starting with -- (CLI args leaked to stdin on AmigaOS) */
            if (line[i] == '-' && line[i + 1] == '-') continue;
        }

        /* Append line to accumulation buffer (with newline separator) */
        if (accum_len > 0 && accum_len < REPL_BUF_SIZE - 1) {
            accum[accum_len++] = '\n';
        }
        if (accum_len + line_len < REPL_BUF_SIZE - 1) {
            memcpy(accum + accum_len, line, line_len);
            accum_len += line_len;
        } else {
            /* See cl_repl: never evaluate a silently-truncated form. */
            cl_write_cstring_to_stdout(
                "; Error: input form exceeds the REPL buffer (4095 chars) "
                "- discarded.\n");
            accum_len = 0;
            depth = 0;
            continue;
        }
        accum[accum_len] = '\0';

        /* Check paren depth */
        depth = cl_paren_depth(accum);

        if (depth > 0) {
            /* Incomplete — read another line */
            continue;
        }

        /* Complete expression(s) — read and evaluate all forms in buffer */

        /* Quit command */
        if (strcmp(accum, "(quit)") == 0 || strcmp(accum, "(QUIT)") == 0 ||
            strcmp(accum, "(exit)") == 0 || strcmp(accum, "(EXIT)") == 0) {
            break;
        }

        {
            CL_ReadStream stream;
            int quit = 0;
            stream.buf = accum;
            stream.pos = 0;
            stream.len = accum_len;
            stream.line = 1;

            while (stream.pos < stream.len && !quit) {
                int err;
                CL_CATCH(err);
                if (err == CL_ERR_NONE) {
                    CL_Obj expr, bytecode, result;

                    cl_current_source_file = NULL;  /* as in cl_repl */
                    cl_current_file_id = 0;
                    expr = cl_read_from_string(&stream);
                    if (CL_NULL_P(expr) || cl_reader_eof()) {
                        CL_UNCATCH();
                        break;
                    }

                    CL_GC_PROTECT(expr);
                    bytecode = cl_compile(expr);
                    CL_GC_UNPROTECT(1);

                    if (!CL_NULL_P(bytecode)) {
                        /* One line per value; (values) prints nothing at
                         * all — see cl_repl */
                        result = cl_vm_eval(bytecode);
                        cl_repl_print_values(cl_repl_values_list(result), 0);
                    }
                    CL_UNCATCH();
                } else if (err == CL_ERR_EXIT) {
                    CL_UNCATCH();
                    quit = 1;
                } else {
                    cl_error_print();
                    cl_vm.sp = 0;
                    cl_vm.fp = 0;
                    CL_UNCATCH();
                    break; /* skip rest of buffer on error */
                }
            }
            if (quit) break;
        }

        /* Deferred EXT:SAVE-IMAGE dump (see cl_repl). */
        if (cl_image_save_run_if_pending())
            break;

        /* Reset accumulation buffer */
        accum_len = 0;
        depth = 0;
    }
}

static void init_history_symbol(CL_Obj sym)
{
    CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
    s->flags |= CL_SYM_SPECIAL;
    s->value = CL_NIL;
}

/* Boot-phase progress + timing.  Useful during the 30+ s lower-end Amiga
 * boot and for spotting startup-time regressions.  Opt-in: main.c clears
 * cl_quiet_boot only when --boot-log is given, so normal (and piped-test)
 * output stays clean. */
int cl_quiet_boot = 1;
static void boot_timing_log(const char *phase, uint32_t t_start, uint32_t *t_prev)
{
    uint32_t now = platform_time_ms();
    if (!cl_quiet_boot) {
        char buf[96];
        snprintf(buf, sizeof(buf), "; [boot] %5u ms (+%5u): %s\n",
                 (unsigned)(now - t_start),
                 (unsigned)(now - *t_prev),
                 phase);
        cl_write_cstring_to_stdout(buf);
    }
    *t_prev = now;
}
#define BOOT_TIME(phase) boot_timing_log((phase), t_start, &t_prev)

#ifndef PLATFORM_AMIGA
/* Executable-relative runtime-library locations, tried in this order by
 * boot loading (below), REQUIRE (builtins_io.c) and heap-image discovery
 * (main.c).  See repl.h for what each layout is. */
const char *const cl_lib_rel_dirs[CL_LIB_REL_COUNT] = {
    "lib/", "../lib/amilisp/", "../lib/clamiga/", "../../lib/"
};
#endif

/* A boot candidate EXISTED but its load errored (the CL_CATCH in
   try_load_boot_pair swallowed the longjmp).  Without this report the
   failure is indistinguishable from "file not found" and the closing
   "cannot locate its runtime library" message sends the user chasing the
   wrong problem (as happened on MorphOS when the C-stack guard fired
   mid-boot). */
static void report_boot_load_failure(const char *path)
{
    char msg[900];
    snprintf(msg, sizeof(msg),
             "; Error: found \"%s\" but loading it FAILED:\n"
             ";        %s\n",
             path, cl_error_msg);
    cl_write_cstring_to_stdout(msg);
}

/* Load boot from one (fasl, src) path pair.  Returns 1 if boot was loaded
   (from the FASL or, failing that, the source), 0 if neither file was present
   or loadable.  Factored out so the cwd-relative, Amiga PROGDIR:, and host
   $CLAMIGA_HOME locations are all tried with identical semantics (stale-FASL
   header validation + recompile-after-source).  cmd buffers are sized for
   absolute paths built from $CLAMIGA_HOME. */
static int try_load_boot_pair(const char *fasl_path, const char *src_path)
{
    int boot_loaded = 0;
    int fasl_fresh = 0;

    /* Try FASL if it exists and is not stale.  Pre-validate the header so a
       fasl from a previous CL_FASL_VERSION is dropped cleanly rather than
       longjmping out of cl_fasl_load with the VM stack/frame pointers
       half-unwound (see below). */
    if (platform_file_exists(fasl_path)) {
        uint32_t fasl_mt = platform_file_mtime(fasl_path);
        uint32_t src_mt  = platform_file_mtime(src_path);
        int header_ok = 0;
        if (fasl_mt > 0 && fasl_mt >= src_mt) {
            unsigned long hsize = 0;
            char *hbuf = platform_file_read(fasl_path, &hsize);
            if (hbuf) {
                if (hsize >= 6) {
                    uint32_t magic = ((uint32_t)(uint8_t)hbuf[0] << 24) |
                                     ((uint32_t)(uint8_t)hbuf[1] << 16) |
                                     ((uint32_t)(uint8_t)hbuf[2] << 8) |
                                     ((uint32_t)(uint8_t)hbuf[3]);
                    uint32_t fver  = ((uint32_t)(uint8_t)hbuf[4] << 8) |
                                     ((uint32_t)(uint8_t)hbuf[5]);
                    if (magic == CL_FASL_MAGIC && fver == CL_FASL_VERSION)
                        header_ok = 1;
                }
                platform_free(hbuf);
            }
            if (!header_ok) {
                /* Stale or unrecognised — drop it and fall through to source
                   so the next startup writes a fresh one. */
                platform_file_delete(fasl_path);
            }
        }
        if (header_ok) {
            int err; CL_CATCH(err);
            if (err == CL_ERR_NONE) {
                char cmd[720];
                snprintf(cmd, sizeof(cmd), "(load \"%s\")", fasl_path);
                cl_eval_string(cmd);
                boot_loaded = 1;
                fasl_fresh = 1;
            } else {
                /* cl_error_unwind clears nlx/dynbind/handlers/GC roots but
                   leaves cl_vm.sp / cl_vm.fp pointing into the aborted eval —
                   reset before the next cl_eval_string so it starts on a clean
                   VM stack. */
                cl_vm.sp = 0;
                cl_vm.fp = 0;
                report_boot_load_failure(fasl_path);
            }
            CL_UNCATCH();
        }
    }

    /* Fall back to source */
    if (!boot_loaded && platform_file_exists(src_path)) {
        int err; CL_CATCH(err);
        if (err == CL_ERR_NONE) {
            char cmd[720];
            snprintf(cmd, sizeof(cmd), "(load \"%s\")", src_path);
            cl_eval_string(cmd);
            boot_loaded = 1;
        } else {
            cl_vm.sp = 0;
            cl_vm.fp = 0;
            report_boot_load_failure(src_path);
        }
        CL_UNCATCH();
    }

    /* Recompile FASL if we loaded from source */
    if (boot_loaded && !fasl_fresh) {
        int err; CL_CATCH(err);
        if (err == CL_ERR_NONE) {
            char cmd[1440];
            snprintf(cmd, sizeof(cmd),
                "(compile-file \"%s\" :output-file \"%s\")",
                src_path, fasl_path);
            cl_eval_string(cmd);
        } else {
            cl_vm.sp = 0;
            cl_vm.fp = 0;
        }
        CL_UNCATCH();
    }

    return boot_loaded;
}

void cl_repl_init_no_userinit(int no_userinit)
{
    uint32_t t_start = platform_time_ms();
    uint32_t t_prev  = t_start;
    int boot_loaded = 0;
    /* Load standard library in CL package so macros/functions are
       accessible from any package that :use's CL */
    CL_Obj saved_pkg = cl_current_package;
    /* GC SAFETY: the boot loads below compact — saved_pkg must be a
     * forwarded root for the restore at the end. */
    CL_GC_PROTECT(saved_pkg);
    cl_current_package = cl_package_cl;
    {
        CL_Symbol *pkg_sym = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_STAR_PACKAGE);
        pkg_sym->value = cl_package_cl;
    }

    /* Pre-intern boot.lisp / clos.lisp internal helper names in CLAMIGA so
       that when boot/clos load (with *package* = CL) the reader resolves
       %FOO to the inherited CLAMIGA symbol rather than interning a fresh
       CL::%FOO.  CLAMIGA's exports are inherited by CL and CL-USER (see
       cl_package_init), so unqualified %FOO references work everywhere
       without source-level qualification.  The point of this is to keep
       these helpers OUT of CL's external-symbol set — boot/clos define
       ~140 such helpers and their leakage breaks the ANSI
       NO-EXTRA-SYMBOLS-EXPORTED-FROM-COMMON-LISP test. */
    cl_intern_clos_internals_in_clamiga();

    /* Export all CL symbols defined so far (from symbol_init + builtins_init).
       This must happen BEFORE any eval_string or boot.lisp loading that runs
       in the CL package — those passes intern stray symbols (e.g. macro
       parameter names like BODY/TEST) that should NOT be exported. */
    cl_package_export_all_cl_symbols();

    /* Define WHEN / UNLESS as macros so boot.lisp can use them.  These run
       AFTER cl_package_export_all_cl_symbols on purpose: the parameter
       symbols TEST and BODY interned by the reader stay private to CL and
       are filtered out by the later cl_package_export_defined_cl_symbols
       (no binding → not exported). */
    cl_eval_string("(defmacro when (test &rest body) (list 'if test (cons 'progn body)))");
    cl_eval_string("(defmacro unless (test &rest body) (list 'if test nil (cons 'progn body)))");
    BOOT_TIME("core setup");

    /* Suppress *load-verbose* during internal boot/clos loading */
    {
        CL_Symbol *lv = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_STAR_LOAD_VERBOSE);
        lv->value = CL_NIL;
    }

    /* Load boot — try pre-compiled FASL first, fall back to source.
       Skip stale FASLs (source newer than FASL) and recompile after
       loading from source so the next startup is fast. */
    {
        static const char *fasl_src_pairs[][2] = {
            { "lib/boot.fasl", "lib/boot.lisp" },
#ifdef PLATFORM_AMIGA
            { "PROGDIR:lib/boot.fasl", "PROGDIR:lib/boot.lisp" },
#endif
        };
        int bi;
        int npairs = (int)(sizeof(fasl_src_pairs) / sizeof(fasl_src_pairs[0]));

        for (bi = 0; bi < npairs && !boot_loaded; bi++) {
            boot_loaded = try_load_boot_pair(fasl_src_pairs[bi][0],
                                             fasl_src_pairs[bi][1]);
        }

#ifdef PLATFORM_AMIGA
        /* Executable-relative in-repo layout (build/amiga/amilisp with lib/
           two levels up) — mirrors the host's "../../" fallback below so
           amilisp can be launched from any cwd, e.g. an example project
           directory.  "PROGDIR://" cannot express this (parent-climb slashes
           only apply to cwd-relative paths), so the ancestor is resolved
           through dos.library ParentDir. */
        if (!boot_loaded) {
            char prefix[512];
            if (platform_executable_ancestor_prefix(2, prefix,
                                                    (int)sizeof(prefix))) {
                char fasl_path[700], src_path[700];
                snprintf(fasl_path, sizeof(fasl_path), "%slib/boot.fasl",
                         prefix);
                snprintf(src_path, sizeof(src_path), "%slib/boot.lisp",
                         prefix);
                boot_loaded = try_load_boot_pair(fasl_path, src_path);
            }
        }
#else
        /* Host fallback: when amilisp is launched from a directory other than
           its source root (e.g. an editor/Sly session whose cwd follows the
           file buffer), cwd-relative "lib/" won't exist.  Honour $CLAMIGA_HOME
           so the bundled lib/ is still found without forcing the process
           working directory — keeping the cwd free to track the buffer.  On
           Amiga the PROGDIR: pair above already plays this role. */
        if (!boot_loaded) {
            const char *home = getenv("CLAMIGA_HOME");
            if (home && home[0]) {
                char fasl_path[700], src_path[700];
                size_t hlen = strlen(home);
                int hcut = (hlen > 0 && home[hlen - 1] == '/') ? (int)(hlen - 1)
                                                               : (int)hlen;
                snprintf(fasl_path, sizeof(fasl_path), "%.*s/lib/boot.fasl",
                         hcut, home);
                snprintf(src_path, sizeof(src_path), "%.*s/lib/boot.lisp",
                         hcut, home);
                boot_loaded = try_load_boot_pair(fasl_path, src_path);
            }
        }

        /* Executable-relative fallback: covers a deployed layout (lib/ next
           to the amilisp binary), an install prefix (bin/amilisp +
           lib/amilisp/, the layout SBCL uses) and the
           in-repo build (build/host/amilisp with lib/ two levels up), so a
           plain `amilisp` on $PATH — symlink included — finds its runtime
           from any directory with no environment setup at all. */
        if (!boot_loaded) {
            char prefix[512];
            if (platform_executable_prefix(prefix, (int)sizeof(prefix))) {
                int ri;
                for (ri = 0; ri < CL_LIB_REL_COUNT && !boot_loaded; ri++) {
                    char fasl_path[700], src_path[700];
                    snprintf(fasl_path, sizeof(fasl_path), "%s%sboot.fasl",
                             prefix, cl_lib_rel_dirs[ri]);
                    snprintf(src_path, sizeof(src_path), "%s%sboot.lisp",
                             prefix, cl_lib_rel_dirs[ri]);
                    boot_loaded = try_load_boot_pair(fasl_path, src_path);
                }
            }
        }
#endif
        if (!boot_loaded) {
            /* Without boot.lisp the session is a bare C core: no standard
               macros, no CLOS, and every REQUIRE will fail.  Say exactly
               what is wrong and how to fix it instead of letting the first
               (require ...) die with a generic error. */
            cl_write_cstring_to_stdout(
                "; Error: amilisp cannot locate its runtime library (lib/boot.lisp).\n"
#ifdef PLATFORM_AMIGA
                ";        Searched lib/ under the current directory, PROGDIR:lib/\n"
                ";        and lib/ two levels above the binary (in-repo layout).\n"
                ";        Run amilisp from (or install it into) its distribution\n"
                ";        directory so lib/ sits next to the binary.\n"
#else
                ";        Searched lib/ under the current directory, $CLAMIGA_HOME/lib/,\n"
                ";        and - relative to the amilisp executable - lib/,\n"
                ";        ../lib/amilisp/ (installed layout) and ../../lib/ (in-repo build).\n"
                ";        Set CLAMIGA_HOME to your cl-amiga checkout/installation, e.g.\n"
                ";            export CLAMIGA_HOME=/path/to/cl-amiga\n"
#endif
                ";        Continuing without the standard library - most Common Lisp\n"
                ";        features and REQUIRE will not work.\n");
        }
    }
    BOOT_TIME("boot library");

    /* Load CLOS so defclass/defgeneric/defmethod are available.  Pointless
       without boot (require would just fail again) — the diagnostic above
       already explained the real problem. */
    if (boot_loaded)
        cl_eval_string("(require \"clos\")");
    BOOT_TIME("CLOS");

    /* Re-enable *load-verbose* for user code */
    {
        CL_Symbol *lv = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_STAR_LOAD_VERBOSE);
        lv->value = SYM_T;
    }

    /* Selectively export only NEW CL symbols that have real bindings
       (function, value, macro, type, struct, or CLOS class).
       Stray symbols interned by boot.lisp/clos.lisp macro bodies are skipped. */
    cl_package_export_defined_cl_symbols();
    cl_current_package = saved_pkg;
    {
        CL_Symbol *pkg_sym = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_STAR_PACKAGE);
        pkg_sym->value = saved_pkg;
    }
    CL_GC_UNPROTECT(1);  /* saved_pkg */

    if (!no_userinit) {
        load_user_init();
        BOOT_TIME("user init");
    }

    /* Look up *, +, -, / (already interned by builtins as arithmetic
     * functions — CLHS 25.1.1 gives them a value binding as well) */
    SYM_STAR  = cl_intern_in("*", 1, cl_package_cl);
    SYM_PLUS  = cl_intern_in("+", 1, cl_package_cl);
    SYM_MINUS = cl_intern_in("-", 1, cl_package_cl);
    SYM_SLASH = cl_intern_in("/", 1, cl_package_cl);

    /* Mark all 10 history symbols as special with initial value NIL */
    init_history_symbol(SYM_STAR);
    init_history_symbol(SYM_STARSTAR);
    init_history_symbol(SYM_STARSTARSTAR);
    init_history_symbol(SYM_PLUS);
    init_history_symbol(SYM_PLUSPLUS);
    init_history_symbol(SYM_PLUSPLUSPLUS);
    init_history_symbol(SYM_MINUS);
    init_history_symbol(SYM_SLASH);
    init_history_symbol(SYM_SLASHSLASH);
    init_history_symbol(SYM_SLASHSLASHSLASH);
    BOOT_TIME("ready");
}

void cl_repl_init(void)
{
    cl_repl_init_no_userinit(0);
}

/* Restored-boot init (--image; see repl.h).  Everything cl_repl_init
 * builds — boot.lisp, CLOS, the export passes, the history symbols — is
 * already present in the restored heap; only the per-process pieces run:
 * the system restore hooks, the user init file and the restore hooks.
 * EXT:*IMAGE-RESTORED-P* was set to T by cl_image_restore_staged BEFORE
 * this, so ~/.amilisprc can skip redundant loads. */
void cl_repl_init_from_image(int no_userinit, uint32_t image_ms)
{
    /* Start the clock image_ms in the past: the stage (read + verify) and
     * the restore (arena copy + relink walk) ran in main() before this
     * call, and they are what the FASL boot's "boot library" + "CLOS"
     * lines are to be compared against. */
    uint32_t t_start = platform_time_ms() - image_ms;
    uint32_t t_prev  = t_start;
    extern void cl_image_run_restore_hooks(void);
    extern void cl_image_run_system_restore_hooks(void);

    BOOT_TIME("image restored (read + relink)");

    /* The library bases the image's Amiga modules hold are zeroed foreign
     * pointers now; they are reopened before any user code runs. */
    cl_image_run_system_restore_hooks();

    if (!no_userinit) {
        load_user_init();
        BOOT_TIME("user init");
    }

    cl_image_run_restore_hooks();
    BOOT_TIME("ready");
}

void cl_repl_init_minimal(void)
{
    /* Minimal init: set up when/unless macros and export CL symbols,
     * but skip boot.lisp, CLOS, and user init.
     * For unit tests that need cl_compile/cl_eval_string. */
    CL_Obj saved_pkg = cl_current_package;
    CL_GC_PROTECT(saved_pkg);
    cl_current_package = cl_package_cl;
    {
        CL_Symbol *pkg_sym = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_STAR_PACKAGE);
        pkg_sym->value = cl_package_cl;
    }

    cl_eval_string("(defmacro when (test &rest body) (list 'if test (cons 'progn body)))");
    cl_eval_string("(defmacro unless (test &rest body) (list 'if test nil (cons 'progn body)))");

    cl_package_export_all_cl_symbols();

    cl_current_package = saved_pkg;
    {
        CL_Symbol *pkg_sym = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_STAR_PACKAGE);
        pkg_sym->value = saved_pkg;
    }
    CL_GC_UNPROTECT(1);  /* saved_pkg */
}
