/*
 * debugger.c — Interactive debugger for AmiLisp
 *
 * When an unhandled error occurs in the interactive REPL, displays
 * the condition, backtrace, and available restarts, then lets the
 * user choose a restart or evaluate expressions for inspection.
 */

#include "debugger.h"
#include "builtins.h"
#include "symbol.h"
#include "package.h"
#include "mem.h"
#include "error.h"
#include "vm.h"
#include "printer.h"
#include "repl.h"
#include "color.h"
#include "stream.h"
#include "string_utils.h"
#include "../platform/platform.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int cl_debugger_enabled = 0;
/* cl_in_debugger is now in CL_Thread (macro from thread.h) */
/* CL_DEBUGGER_MAX_DEPTH lives in debugger.h (shared with tests). */

static CL_Obj SYM_DEBUGGER_HOOK;

/* Helper to register a builtin AND export it from the CL package (plain defun()
 * from builtins.h only registers; cl_debugger_init runs after the bulk
 * cl_package_export_all_cl_symbols call, so these need an explicit export).
 * Routed through cl_register_builtin_exported so the image-relink registry
 * sees the registration (builtins.h). */
static void defun_exported(const char *name, CL_CFunc func, int min, int max)
{
    cl_register_builtin_exported(name, func, min, max, cl_package_cl);
}

/* Display condition info.
 *
 * Format: "Debugger entered: <TYPE>: <report>"
 * The report is obtained via *print-object-hook* if the condition's class
 * has a PRINT-OBJECT method (e.g. ASDF conditions), falling back to the
 * condition's :format-control/:format-arguments report string, and
 * finally to just the type name. */
static void display_condition(CL_Obj condition, void (*emit)(const char *),
                              int use_color)
{
    CL_Condition *cond;
    char buf[256];
    CL_Obj report_str = CL_NIL;

    /* Both the PRINT-OBJECT hook apply and every emit can allocate (emit
     * routes through the debug-io / error-output stream, which may be a
     * Gray stream dispatching into Lisp — the SLY configuration).  Root the
     * condition and the report string, and derive cond only after those
     * calls. */
    CL_GC_PROTECT(condition);
    CL_GC_PROTECT(report_str);

    /* Try PRINT-OBJECT dispatch via the hook set up by clos.lisp.
     * Protect with CL_CATCH: if the user's PRINT-OBJECT method itself
     * signals (we are already in the debugger printing the condition),
     * a raw cl_error here would longjmp out of cl_invoke_debugger before
     * it can reset cl_in_debugger / cl_debugger_depth, leaving the
     * debugger wedged.  On failure just fall back to the report string. */
    if (!CL_NULL_P(SYM_PRINT_OBJECT_HOOK)) {
        CL_Obj hook_val = cl_symbol_value(SYM_PRINT_OBJECT_HOOK);
        if (!CL_NULL_P(hook_val)) {
            CL_Obj hook_args[1];
            int err;
            hook_args[0] = condition;
            CL_CATCH(err);
            if (err == CL_ERR_NONE) {
                CL_Obj result = cl_vm_apply(hook_val, hook_args, 1);
                /* Wide results count: the hook's WITH-OUTPUT-TO-STRING
                 * returns a TYPE_WIDE_STRING whenever the report holds
                 * any non-ASCII character. */
                if (!CL_NULL_P(result) && CL_ANY_STRING_P(result)) {
                    report_str = result;
                }
                CL_UNCATCH();
            } else {
                CL_UNCATCH();
                /* PRINT-OBJECT method signalled — use fallback below. */
            }
        }
    }

    if (use_color) cl_color_set(CL_COLOR_BOLD_RED);
    emit("\nDebugger entered: ");
    cond = (CL_Condition *)CL_OBJ_TO_PTR(condition);
    cl_prin1_to_string(cond->type_name, buf, sizeof(buf));
    emit(buf);

    /* Prefer the hook result; else the condition's own report string.
     * Either way keep the string in the rooted report_str and re-derive
     * the CL_String* after each allocating emit. */
    if (CL_NULL_P(report_str)) {
        cond = (CL_Condition *)CL_OBJ_TO_PTR(condition);
        report_str = cond->report_string;
    }
    if (!CL_NULL_P(report_str) && CL_ANY_STRING_P(report_str)) {
        /* Bounded UTF-8 conversion: a wide report cast to CL_String*
         * emitted UTF-32 code units as bytes — and a zero-length wide
         * string has no data at all, so the old emit(s->data) read past
         * the object into the next arena cell.  The narrow path gains a
         * bound it never had (the report is capped at cl_error_msg size
         * everywhere else anyway). */
        char rbuf[512];
        cl_string_to_utf8(report_str, rbuf, sizeof(rbuf));
        emit(": ");
        emit(rbuf);
    }
    if (use_color) cl_color_reset();
    emit("\n");
    CL_GC_UNPROTECT(2);
}

/* Display backtrace */
static void display_backtrace(void (*emit)(const char *))
{
    if (cl_backtrace_buf[0] != '\0') {
        emit("\nBacktrace:\n");
        emit(cl_backtrace_buf);
        emit("\n");
    }
}

/* Handle ":bt [n|all]".  ARG is whatever followed the command word; FRAMES is
 * the session's current frame limit, updated in place when a count is given.
 *
 * With no argument this re-displays the text as already rendered.  With a count
 * it re-renders the SAME error-time frames at a different depth, which is the
 * only way past the "... N more frames" tail: those frames were never
 * formatted, so there was nothing for a plain :bt to show. */
static void handle_bt_command(const char *arg, int *frames)
{
    while (*arg == ' ' || *arg == '\t') arg++;

    if (*arg != '\0') {
        int want;
        if (strcmp(arg, "all") == 0 || strcmp(arg, "ALL") == 0) {
            want = 0;                       /* 0 = no limit */
        } else {
            char *endp;
            long n = strtol(arg, &endp, 10);
            /* frame_size is this VM's hard frame ceiling, so a larger count
             * could not show more — reject it rather than silently ignore. */
            if (endp == arg || *endp != '\0' || n < 0 || n > cl_vm.frame_size) {
                cl_write_cstring_to_debug_io(
                    "Usage: :bt [<n>|all] - show at most <n> frames "
                    "(0 or \"all\" for every frame)\n");
                return;
            }
            want = (int)n;
        }
        *frames = want;
        cl_render_backtrace(want);
    }
    display_backtrace(cl_write_cstring_to_debug_io);
}

/* Display available restarts and return count (including "top level") */
static int display_restarts(void)
{
    int i, idx = 0;
    char numbuf[16];

    cl_write_cstring_to_debug_io("Available restarts:\n");

    for (i = cl_restart_top - 1; i >= 0; i--) {
        char namebuf[128];
        snprintf(numbuf, sizeof(numbuf), "  %d: ", idx);
        cl_write_cstring_to_debug_io(numbuf);
        cl_prin1_to_string(cl_restart_stack[i].name, namebuf, sizeof(namebuf));
        cl_write_cstring_to_debug_io(namebuf);
        cl_write_cstring_to_debug_io("\n");
        idx++;
    }

    /* Always add "Return to top level" as last option */
    snprintf(numbuf, sizeof(numbuf), "  %d: ", idx);
    cl_write_cstring_to_debug_io(numbuf);
    cl_write_cstring_to_debug_io("Return to top level\n");

    return idx + 1;
}

/* Display help */
static void display_help(void)
{
    cl_write_cstring_to_debug_io("Debugger commands:\n");
    /* ASCII only: this reaches an ISO-8859-1 Amiga console, where a UTF-8
     * dash shows as garbage (tests/test_ascii_messages.sh). */
    cl_write_cstring_to_debug_io("  <number>  - invoke restart by number\n");
    cl_write_cstring_to_debug_io(
        "  :bt [n]   - show backtrace (n frames, or \"all\")\n");
    cl_write_cstring_to_debug_io("  :q        - return to top level\n");
    cl_write_cstring_to_debug_io("  :help     - show this help\n");
    cl_write_cstring_to_debug_io("  <expr>    - evaluate a Lisp expression\n");
}

/* Invoke restart at index (0 = topmost restart, counting down) */
static void invoke_restart_at(int idx)
{
    int stack_idx = cl_restart_top - 1 - idx;

    if (stack_idx < 0 || stack_idx >= cl_restart_top) {
        cl_write_cstring_to_debug_io("Invalid restart index\n");
        return;
    }

    /* Route through the INVOKE-RESTART dispatch protocol: the handler must
     * be applied at the restart-case catch landing AFTER the unwind.
     * Applying it here and throwing its return value handed the landing a
     * non-dispatch value that it then tried to APPLY — "not a callable
     * function" on every numeric restart selection. */
    cl_invoke_restart_by_stack_index(stack_idx);
    /* Does not return (longjmp) */
}

/* (invoke-debugger condition) — Lisp builtin */
static CL_Obj bi_invoke_debugger(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    if (!CL_CONDITION_P(args[0]))
        cl_error(CL_ERR_TYPE, "INVOKE-DEBUGGER: argument must be a condition");

    cl_invoke_debugger(args[0]);

    /* If debugger returns (user chose "top level"), signal error to unwind */
    cl_error(CL_ERR_GENERAL, "Return to top level");
    return CL_NIL; /* unreachable */
}

/* Jump directly to the top-level (REPL) error frame, clearing all
 * VM/NLX/handler/restart state.  Used by :q and "Return to top level"
 * so a single command always reaches the REPL prompt. */
static void jump_to_top_level(void)
{
    cl_in_debugger = 0;
    cl_debugger_depth = 0;
    cl_nlx_top = 0;
    cl_saved_pending_top = 0;
    cl_pending_throw = 0;
    cl_dynbind_restore_to(0);
    cl_handler_top = 0;
    cl_restart_top = 0;
    cl_gc_reset_roots();
    cl_vm.sp = 0;
    cl_vm.fp = 0;
    /* The UNWIND-PROTECT value records of the cleanups this abandons
     * (OP_MV_SAVE) would otherwise stay parked for the rest of the
     * session — the save stack only ever grew across :q. */
    CT->mv_save_top = 0;

    if (cl_error_frame_top > 0) {
        int code = cl_error_code;
        /* Reset to bottom-most frame (the REPL's error frame) */
        cl_error_frame_top = 1;
        cl_error_frames[0].active = 0;
        CL_LONGJMP(cl_error_frames[0].buf, code);
    }

    /* No error frame — fatal (should not happen in interactive REPL) */
    cl_write_cstring_to_debug_io("No error frame - cannot return to top level\n");
}

/* Core debugger loop */
void cl_invoke_debugger(CL_Obj condition)
{
    int num_restarts;
    char line[1024];
    /* Frame limit :bt renders at — tracks the last explicit `:bt N`, so a
     * re-render after an eval keeps the depth the user asked for. */
    int bt_frames = CL_BACKTRACE_DEFAULT_FRAMES;

    /* Recursion guard: if the interactive C debugger loop is already active
     * on this thread, don't re-enter it. */
    if (cl_in_debugger)
        return;

    /* Inside a foreign callback (thread.h "Foreign-callback boundary") the
     * debugger — and *DEBUGGER-HOOK*, whose SLDB would offer restarts that
     * unwind across the OS's frames — stays out unless
     * EXT:*CALLBACK-ERROR-POLICY* is :DEBUG.  The condition is parked at
     * the boundary and re-signaled, debugger included, once the foreign
     * call has returned. */
    if (!cl_callback_debugger_allowed())
        return;

    /* Depth guard.  The *debugger-hook* is invoked below while cl_in_debugger
     * is still 0, so a hook (or a restart it drives) that re-signals recurses
     * back into cl_invoke_debugger via cl_error_from_condition without the
     * binary guard ever firing.  slynk legitimately nests a few levels for
     * nested SLDB, but a runaway (e.g. an error raised while printing the
     * condition) would recurse the C stack into a SIGSEGV.  Cap it: on
     * overflow, abandon the nested debugger and unwind to top level with a
     * clear diagnostic.  cl_debugger_depth is restored on every error-frame
     * unwind (see error.c) so it cannot stay falsely elevated. */
    if (cl_debugger_depth >= CL_DEBUGGER_MAX_DEPTH) {
        cl_color_set(CL_COLOR_RED);
        cl_write_cstring_to_debug_io(
            "\nDebugger recursion limit reached "
            "(error while handling an error) - returning to top level.\n");
        cl_color_reset();
        jump_to_top_level(); /* longjmp — does not return */
        return;              /* unreachable */
    }
    cl_debugger_depth++;

    /* Always check *debugger-hook* per CL spec, even if interactive
     * debugger is disabled (e.g., batch mode or tests) */
    if (!CL_NULL_P(SYM_DEBUGGER_HOOK) && CL_SYMBOL_P(SYM_DEBUGGER_HOOK))
    {
        CL_Obj hook_val = cl_symbol_value(SYM_DEBUGGER_HOOK);

        if (!CL_NULL_P(hook_val)) {
            CL_Obj saved_hook = hook_val;
            CL_Obj hook_args[2];
            int err;

            /* Root condition and saved_hook across the hook apply: if the
             * hook returns normally, saved_hook is written back into
             * *DEBUGGER-HOOK*'s value cell (a stale offset there corrupts
             * every later debugger entry) and condition is displayed below.
             * A control-transferring hook longjmps past the unprotect —
             * the unwind restores the root count. */
            CL_GC_PROTECT(condition);
            CL_GC_PROTECT(saved_hook);

            /* Set *debugger-hook* to NIL before calling hook */
            cl_set_symbol_value(SYM_DEBUGGER_HOOK, CL_NIL);

            hook_args[0] = condition;
            hook_args[1] = saved_hook;

            /* Call hook protected by CL_CATCH — if hook transfers
             * control (longjmp), we never reach the restore */
            CL_CATCH(err);
            if (err == CL_ERR_NONE) {
                cl_vm_apply(saved_hook, hook_args, 2);
                CL_UNCATCH();
            } else {
                CL_UNCATCH();
                /* Hook signaled an error — restore and fall through */
            }

            /* Restore hook value */
            cl_set_symbol_value(SYM_DEBUGGER_HOOK, saved_hook);
            CL_GC_UNPROTECT(2);
        }
    }

    /* Interactive debugger loop only if enabled (suppressed in batch mode /
     * tests / during macroexpansion).  When disabled, return silently: the
     * caller (REPL error frame, test harness) is responsible for reporting. */
    if (!cl_debugger_enabled) {
        cl_debugger_depth--;
        return;
    }

    /* The interactive mini-REPL below blocks in platform_read_line() (fgets on
     * stdin).  That is only safe when there is a human at an interactive
     * terminal to answer it:
     *
     *   - CT == cl_main_thread_ptr — this is the REPL thread, the only thread
     *     that owns stdin; a SLY :spawn worker reaching here must never read,
     *   - platform_stdin_is_interactive() — stdin is a real tty, not a pipe or
     *     /dev/null (a `tail -f /dev/null | amilisp` launcher fails this).
     *
     * Otherwise (a background worker, or a non-interactive launcher) a read
     * would block forever on input no one will send — while holding a GC safe
     * region open — or spin on immediate EOF.  Fall back to ECL's --nodebug
     * behaviour: report the condition and backtrace to *error-output* and
     * return, letting cl_error_unwind abort this thread (or the REPL frame)
     * cleanly. */
    if (CT != cl_main_thread_ptr || !platform_stdin_is_interactive()) {
        display_condition(condition, cl_write_cstring_to_error, 0);
        display_backtrace(cl_write_cstring_to_error);
        cl_debugger_depth--;
        return;
    }

    cl_in_debugger = 1;

    display_condition(condition, cl_write_cstring_to_debug_io, 1);
    display_backtrace(cl_write_cstring_to_debug_io);
    num_restarts = display_restarts();
    display_help();

    /* Mini-REPL loop */
    cl_color_set(CL_COLOR_DIM_MAGENTA);
    cl_write_cstring_to_debug_io("\nDebug> ");
    cl_color_reset();

    while (platform_read_line(line, sizeof(line))) {
        /* Skip empty lines */
        if (line[0] == '\0') {
            cl_color_set(CL_COLOR_DIM_MAGENTA);
            cl_write_cstring_to_debug_io("Debug> ");
            cl_color_reset();
            continue;
        }

        /* Check for commands */
        if (strncmp(line, ":bt", 3) == 0 &&
            (line[3] == '\0' || line[3] == ' ' || line[3] == '\t')) {
            handle_bt_command(line + 3, &bt_frames);
            cl_color_set(CL_COLOR_DIM_MAGENTA);
            cl_write_cstring_to_debug_io("Debug> ");
            cl_color_reset();
            continue;
        }

        if (strcmp(line, ":q") == 0) {
            jump_to_top_level(); /* longjmp — does not return */
        }

        if (strcmp(line, ":help") == 0) {
            display_help();
            cl_color_set(CL_COLOR_DIM_MAGENTA);
            cl_write_cstring_to_debug_io("Debug> ");
            cl_color_reset();
            continue;
        }

        /* Check if input is a number (restart index) */
        {
            char *endp;
            long idx = strtol(line, &endp, 10);
            if (endp != line && *endp == '\0') {
                /* It's a number */
                if (idx >= 0 && idx < num_restarts) {
                    if (idx == num_restarts - 1) {
                        /* "Return to top level" */
                        jump_to_top_level(); /* longjmp — does not return */
                    }
                    /* Invoke the restart — this does not return (longjmp) */
                    cl_in_debugger = 0;
                    invoke_restart_at((int)idx);
                    /* If we somehow get here, re-enter debugger */
                    cl_in_debugger = 1;
                } else {
                    cl_write_cstring_to_debug_io("Invalid restart number\n");
                }
                cl_color_set(CL_COLOR_DIM_MAGENTA);
                cl_write_cstring_to_debug_io("Debug> ");
                cl_color_reset();
                continue;
            }
        }

        /* Otherwise, eval as Lisp expression */
        {
            int err;
            /* Evaluate ON TOP of the error-time frames rather than discarding
             * them.  Zeroing sp/fp here erased the very stack the user entered
             * the debugger to inspect: (ext:backtrace) and (ext:frame-locals)
             * at this prompt saw only their own call, and :bt N had no frames
             * left to re-render.  Nesting a cl_vm_apply on a live frame stack
             * is exactly what the *debugger-hook* path above already does, so
             * this is the well-trodden case, not a new one.
             *
             * Restore the snapshot afterwards — on the error path too, where
             * the nested unwind leaves sp/fp wherever it stopped — so every
             * later command, and any restart we longjmp into, still sees the
             * error-time window. */
            int saved_sp = cl_vm.sp;
            int saved_fp = cl_vm.fp;
            int saved_debug_fp = cl_debug_base_fp;

            CL_CATCH(err);
            if (err == CL_ERR_NONE) {
                CL_Obj result = cl_eval_string(line);
                /* One line per value, as at the top-level REPL: zero
                 * values — (values) — print nothing at all.  Snapshot the
                 * MV state before cl_prin1_to_string can run Lisp. */
                CL_Obj values = cl_repl_values_list(result);
                char buf[512];
                CL_GC_PROTECT(values);
                while (CL_CONS_P(values)) {
                    cl_prin1_to_string(cl_car(values), buf, sizeof(buf));
                    cl_write_cstring_to_debug_io(buf);
                    cl_write_cstring_to_debug_io("\n");
                    values = cl_cdr(values);
                }
                CL_GC_UNPROTECT(1);
                CL_UNCATCH();
            } else {
                CL_UNCATCH();
                cl_color_set(CL_COLOR_RED);
                cl_write_cstring_to_debug_io("Error during eval: ");
                cl_write_cstring_to_debug_io(cl_error_msg);
                cl_color_reset();
                cl_write_cstring_to_debug_io("\n");
            }

            cl_vm.sp = saved_sp;
            cl_vm.fp = saved_fp;

            /* An error inside the eval ran cl_capture_backtrace again, which
             * overwrote both the snapshot and the formatted text with the
             * typo's stack.  Put the error we are actually debugging back, so
             * a following :bt does not silently describe a different one. */
            cl_debug_base_fp = saved_debug_fp;
            cl_render_backtrace(bt_frames);
        }

        cl_color_set(CL_COLOR_DIM_MAGENTA);
        cl_write_cstring_to_debug_io("Debug> ");
        cl_color_reset();
    }

    /* platform_read_line hit EOF — Ctrl-D (POSIX) / Ctrl-\ (Amiga) /
     * Ctrl-Z (Windows) at the Debug> prompt.  Treat it as :q and abort to
     * top level, like SBCL, instead of letting the EOF take the whole
     * session down: stdio's EOF flag is sticky, so without clearing it the
     * top-level REPL's next read also returns EOF and the REPL exits
     * ("Bye.") with all the session's state (issue #13).  This loop only
     * runs when stdin is a real tty (checked on entry above), so after the
     * clear the next read blocks for fresh input — a second Ctrl-D at the
     * top-level prompt still exits; and if the terminal is really gone,
     * reads keep returning EOF and the top level exits as before. */
    platform_clear_stdin_eof();
    cl_write_cstring_to_debug_io("\n");  /* Ctrl-D echoes no newline */
    jump_to_top_level(); /* longjmp — returns only if there is no error
                          * frame, and then it has already zeroed
                          * cl_in_debugger/cl_debugger_depth. */
}

void cl_debugger_init(void)
{
    /* Intern *DEBUGGER-HOOK* as special variable, value NIL */
    SYM_DEBUGGER_HOOK = cl_intern_in("*DEBUGGER-HOOK*", 15, cl_package_cl);
    {
        CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_DEBUGGER_HOOK);
        s->flags |= CL_SYM_SPECIAL;
        s->value = CL_NIL;
    }
    cl_export_symbol(SYM_DEBUGGER_HOOK, cl_package_cl);

    /* Register invoke-debugger builtin */
    defun_exported("INVOKE-DEBUGGER", bi_invoke_debugger, 1, 1);

    /* Register cached symbols for GC compaction forwarding */
    cl_gc_register_root(&SYM_DEBUGGER_HOOK);
}
