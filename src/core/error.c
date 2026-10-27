#include "error.h"
#include "mem.h"
#include "string_utils.h"
#include "vm.h"
#include "symbol.h"
#include "debugger.h"
#include "printer.h"
#include "color.h"
#include "stream.h"
#include "fasl.h"
#include "compiler.h"
#include "../platform/platform.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* All error state now lives in CL_Thread.
 * Compatibility macros in thread.h redirect the old names. */

/* Restore the current thread's jit_depth to new_depth, keeping the
 * global cl_jit_active_threads counter consistent.  Used on every
 * cl_error unwind path — and on every NLX-frame longjmp landing in
 * vm.c / jit runtime — so a longjmp out of JIT'd code drops both the
 * per-thread depth and the global "any-thread-active" gate.
 * See specs/native-backend.md §"GC interaction" option A. */
void cl_jit_restore_depth(int new_depth, char *floor)
{
    extern volatile int cl_jit_active_threads;
    int cur = CT->jit_depth;
    if (cur > 0 && new_depth == 0) cl_jit_active_threads--;
    else if (cur == 0 && new_depth > 0) cl_jit_active_threads++;
    CT->jit_depth = new_depth;
    if (new_depth == 0) {
        CT->jit_stack_top = NULL;
        CT->jit_c_floor   = NULL;
    } else {
        CT->jit_c_floor   = floor;
    }
}

int cl_error_frame_push(void)
{
    if (cl_error_frame_top < CL_MAX_ERROR_FRAMES - CL_ERROR_FRAME_RESERVE) {
        CT->error_frame_overflowing = 0;
    } else if (!CT->error_frame_overflowing) {
        /* Callers used to get -1 here, which no CL_CATCH site tells apart
         * from a caught error: each went on to CL_UNCATCH a frame it never
         * pushed (popping its caller's) and reported an empty message. */
        CT->error_frame_overflowing = 1;
        cl_error(CL_ERR_OVERFLOW,
                 "LOAD, COMPILE-FILE or EVAL nested too deeply: %d C error "
                 "frames in use on this thread (limit %d)",
                 cl_error_frame_top,
                 CL_MAX_ERROR_FRAMES - CL_ERROR_FRAME_RESERVE);
    } else if (cl_error_frame_top >= CL_MAX_ERROR_FRAMES) {
        return -1;
    }
    cl_error_frames[cl_error_frame_top].active = 1;
    /* Snapshot gc_root_count so cl_error_unwind can drop any CL_GC_PROTECT
     * entries pushed by C stack frames we will unwind out of.  Must be
     * sequenced BEFORE setjmp so the value is well-defined on the longjmp
     * return path. */
    cl_error_frames[cl_error_frame_top].saved_gc_roots = gc_root_count;
    cl_error_frames[cl_error_frame_top].saved_jit_depth = CT->jit_depth;
    cl_error_frames[cl_error_frame_top].saved_jit_c_floor = CT->jit_c_floor;
    cl_error_frames[cl_error_frame_top].saved_debugger_depth = cl_debugger_depth;
    cl_error_frames[cl_error_frame_top].saved_in_debugger = cl_in_debugger;
    cl_error_frames[cl_error_frame_top].saved_fasl_readers = cl_fasl_reader_save_count();
    cl_error_frames[cl_error_frame_top].saved_fasl_writers = cl_fasl_writer_save_count();
    cl_error_frames[cl_error_frame_top].saved_active_compiler = cl_compiler_mark();
    cl_error_frames[cl_error_frame_top].saved_handler_top = cl_handler_top;
    cl_error_frames[cl_error_frame_top].saved_handler_active_mask = cl_handler_active_mask;
    cl_error_frames[cl_error_frame_top].saved_restart_top = cl_restart_top;
    cl_error_frames[cl_error_frame_top].saved_dyn_top = cl_dyn_top;
    cl_error_frames[cl_error_frame_top].saved_nlx_top = cl_nlx_top;
    cl_error_frames[cl_error_frame_top].saved_mv_save_top = CT->mv_save_top;
    cl_error_frames[cl_error_frame_top].saved_printer = cl_printer_state_save();
    return cl_error_frame_top++;
}

/* Storage for the symbols documented in error.h.  Lets the error report say
 * WHICH form's expansion blew up — e.g. a feature-conditional macro call
 * that read as too few arguments ((defctype :size #+64-bit :uint64) with no
 * 64-BIT feature) is invisible from the expander's own arity error without
 * this.  The symbols are interned (and GC-rooted) by cl_symbol_init, which
 * runs after cl_package_init — too late to do from cl_error_init, which
 * runs first (see main.c's init order) — so they start out CL_NIL here and
 * are only ever assigned once, at boot, before any second thread exists. */
CL_Obj cl_expanding_form_sym = CL_NIL;
CL_Obj cl_error_expanding_form_sym = CL_NIL;

/* Per-thread read of an expansion-context TLV cell (see error.h), with the
 * CL_TLV_ABSENT "no entry yet" sentinel normalized to CL_NIL so callers
 * never have to special-case it. */
CL_Obj cl_expansion_ctx_get(CL_Obj sym)
{
    CL_Obj v = cl_tlv_get(CT, sym);
    return (v == CL_TLV_ABSENT) ? CL_NIL : v;
}

void cl_error_init(void)
{
    cl_error_frame_top = 0;
    cl_error_code = CL_ERR_NONE;
    cl_error_msg[0] = '\0';
}

/* Verify CL_JMPBUF_GUARD is large enough to absorb this platform's setjmp()
 * write.  On MorphOS PPC (-noixemul) setjmp writes a register save area larger
 * than the toolchain's jmp_buf typedef reports, overrunning the fields that
 * follow an embedded jmp_buf (see types.h and the CL_NLXFrame / CL_ErrorFrame
 * layouts).  This measures the true overrun at startup and aborts with a
 * precise, actionable message if the compiled-in guard is too small — turning
 * a silent memory-corruption bug into a loud boot-time diagnostic.  On targets
 * with no overrun the measured value is 0 and this is a cheap no-op. */
void cl_setjmp_overrun_check(void)
{
    /* jmp_buf first, canary tail contiguous immediately after it (unsigned
     * char has alignment 1, so no padding gap can hide the overrun). */
    struct { jmp_buf b; unsigned char tail[1024]; } probe;
    int i, last = -1, overrun;
    char msg[192];

    memset(probe.tail, 0xA5, sizeof(probe.tail));
    /* setjmp only — we never longjmp back here; the return value is unused. */
    (void)CL_SETJMP(probe.b);
    for (i = 0; i < (int)sizeof(probe.tail); i++)
        if (probe.tail[i] != 0xA5) last = i;
    overrun = last + 1;  /* bytes setjmp wrote past sizeof(jmp_buf) */

#ifdef DEBUG_NLX
    snprintf(msg, sizeof(msg),
             "[NLX] setjmp overrun check: sizeof(jmp_buf)=%d overrun=%d guard=%d\n",
             (int)sizeof(jmp_buf), overrun, (int)CL_JMPBUF_GUARD_RESERVED_BYTES);
    cl_write_cstring_to_stdout(msg);
#endif

    /* Compare against the space actually reserved on THIS target
     * (CL_JMPBUF_GUARD_RESERVED_BYTES: 0 off MorphOS, CL_JMPBUF_GUARD_BYTES on
     * MorphOS) — not the bare CL_JMPBUF_GUARD_BYTES constant, which would
     * silently accept a nonzero overrun on a target where CL_JMPBUF_GUARD
     * expands to nothing. */
    if (overrun > CL_JMPBUF_GUARD_RESERVED_BYTES) {
        snprintf(msg, sizeof(msg),
                 "FATAL: setjmp() overruns jmp_buf by %d bytes but only %d are "
                 "reserved - raise CL_JMPBUF_GUARD_BYTES in types.h\n",
                 overrun, (int)CL_JMPBUF_GUARD_RESERVED_BYTES);
        cl_write_cstring_to_stdout(msg);
        /* cl_thread_init() (called just before this check runs — see main.c)
         * already overwrote this task's TLS slot (tc_UserData on MorphOS).
         * Restore it before aborting so the -noixemul crt0's SIGABRT teardown
         * doesn't dereference our CL_Thread* and freeze the machine — the same
         * hazard cl_thread_restore_main_tls() exists to close on the graceful
         * shutdown path. */
        cl_thread_restore_main_tls();
        abort();
    }
}

/* Longjmp to the current top C error frame, restoring the per-frame
 * snapshots exactly like cl_error's own unwind.  Shared by
 * cl_error_unwind and the deferred-error replay paths — vm.c's
 * OP_UWRETHROW (pending_throw == 2) and the JIT runtime's
 * uwprot-rethrow — which previously longjmp'd to the frame with NONE
 * of these restores, leaving gc_root_count pointing at unwound C stack
 * slots, jit_depth stale, stranded FASL readers and compilers, and
 * orphaned handler/restart bindings. */
CL_NORETURN void cl_error_frame_longjmp(int code)
{
    if (cl_error_frame_top > 1) {
        /* Nested error frame — jump to it without destroying global state.
         * The caller is responsible for restoring VM/binding state.
         * Don't decrement here — CL_UNCATCH at the catch site pops.
         *
         * Restore gc_root_count to the value captured at CL_CATCH push
         * time: any entries pushed since then live in C stack frames we
         * are unwinding out of and would dangle in gc_roots[]. */
        gc_root_count = cl_error_frames[cl_error_frame_top - 1].saved_gc_roots;
        cl_jit_restore_depth(cl_error_frames[cl_error_frame_top - 1].saved_jit_depth,
                             cl_error_frames[cl_error_frame_top - 1].saved_jit_c_floor);
        cl_debugger_depth = cl_error_frames[cl_error_frame_top - 1].saved_debugger_depth;
        cl_in_debugger = cl_error_frames[cl_error_frame_top - 1].saved_in_debugger;
        cl_fasl_reader_restore_count(cl_error_frames[cl_error_frame_top - 1].saved_fasl_readers);
        cl_fasl_writer_restore_count(cl_error_frames[cl_error_frame_top - 1].saved_fasl_writers);
        cl_compiler_force_restore_to(cl_error_frames[cl_error_frame_top - 1].saved_active_compiler);
        /* Drop any HANDLER-BIND / RESTART-CASE bindings established since this
         * frame was pushed — their POP opcodes are abandoned by the longjmp.
         * Leaving them stranded lets a later condition invoke a handler whose
         * catch tag is already gone (see CL_ErrorFrame.saved_handler_top). */
        if (cl_handler_top > cl_error_frames[cl_error_frame_top - 1].saved_handler_top)
            cl_handler_top = cl_error_frames[cl_error_frame_top - 1].saved_handler_top;
        /* Restore the disabled-handler band to its state at frame push, so a
         * band disabled by a handler we are unwinding out of is re-enabled
         * (mirrors the NLX-landing restore in vm.c). */
        cl_handler_active_mask = cl_error_frames[cl_error_frame_top - 1].saved_handler_active_mask;
        if (cl_restart_top > cl_error_frames[cl_error_frame_top - 1].saved_restart_top)
            cl_restart_top = cl_error_frames[cl_error_frame_top - 1].saved_restart_top;
        /* Pop dynamic bindings established inside the frame — their
         * OP_DYNUNBIND / cl_dynbind_restore_to call is abandoned by the
         * longjmp.  Writes each saved value back into its TLV (and re-syncs
         * cl_current_package for *PACKAGE*), exactly like the NLX landings
         * (see CL_ErrorFrame.saved_dyn_top). */
        cl_dynbind_restore_to(cl_error_frames[cl_error_frame_top - 1].saved_dyn_top);
        /* Repair printer state abandoned by prints we are unwinding out of
         * (aborted pprint-dispatch fn / print hook / stream error) — see
         * CL_ErrorFrame.saved_printer. */
        cl_printer_state_restore(cl_error_frames[cl_error_frame_top - 1].saved_printer);
        /* Drop the unwind-protect value records of abandoned cleanups. */
        CT->mv_save_top = cl_error_frames[cl_error_frame_top - 1].saved_mv_save_top;
        CL_LONGJMP(cl_error_frames[cl_error_frame_top - 1].buf, code);
    }

    /* Outermost error frame (REPL) — full cleanup.
     * NLX frames (catch/uwprot) are invalid once we leave the VM,
     * so clear the NLX stack and reset pending state.
     * Restore all dynamic bindings before leaving the VM.
     * Reset GC root stack — longjmp invalidates stack-local roots. */
    cl_nlx_top = 0;
    cl_nlx_floor = 0;
    CT->callback_depth = 0;
    cl_saved_pending_top = 0;
    cl_pending_throw = 0;
    CT->mv_save_top = 0;
    cl_dynbind_restore_to(0);
    cl_handler_top = 0;
    cl_restart_top = 0;
    cl_gc_reset_roots();
    cl_jit_restore_depth(0, NULL);
    cl_debugger_depth = 0;
    cl_in_debugger = 0;
    /* Reset printer state abandoned by an aborted print (see
     * CL_ErrorFrame.saved_printer) — at the outermost frame every print
     * has been unwound, so all four flags go back to idle. */
    cl_printer_state_reset();

    if (cl_error_frame_top > 0) {
        /* Drop any active FASL readers — their stack-local CL_FaslReaders are
         * unwound away.  Restore to the outermost frame's snapshot (normally 0). */
        cl_fasl_reader_restore_count(cl_error_frames[cl_error_frame_top - 1].saved_fasl_readers);
        cl_fasl_writer_restore_count(cl_error_frames[cl_error_frame_top - 1].saved_fasl_writers);
        /* Free any compilers stranded by the unwound compile (see field doc). */
        cl_compiler_force_restore_to(cl_error_frames[cl_error_frame_top - 1].saved_active_compiler);
        /* Restore the catch site's GC-root snapshot, exactly like the nested
         * branch above and the CL_ERR_EXIT path — cl_gc_reset_roots() above
         * zeroed the count, which is one too FEW when the outermost catch
         * site itself holds live CL_GC_PROTECT entries.  That is the case
         * during REPL init: try_load_boot_pair's CL_CATCH sits above the
         * protected saved_pkg, and wiping it made the boot-failure path
         * (e.g. a reader error in lib/boot.lisp) abort later with
         * "pop_roots(1): gc_root_count went negative" at the final
         * CL_GC_UNPROTECT.  At the REPL toplevel the snapshot is 0, so this
         * is identical to the old reset there. */
        gc_root_count = cl_error_frames[cl_error_frame_top - 1].saved_gc_roots;
        CL_LONGJMP(cl_error_frames[cl_error_frame_top - 1].buf, code);
    }

    /* No error frame — fatal */
    cl_color_set(CL_COLOR_ERROR_TAG);
    platform_write_string("FATAL ERROR:");
    cl_color_reset();
    cl_color_set(CL_COLOR_ERROR);
    platform_write_string(" ");
    platform_write_string(cl_error_msg);
    cl_color_reset();
    platform_write_string("\n");
    cl_fatal_exit(1);
}

/* Terminate the process from a fatal (non-recoverable) runtime path.
 * cl_thread_init() overwrites this task's TLS slot (tc_UserData on
 * MorphOS/AmigaOS) with our CL_Thread*; restoring it here — not just on the
 * graceful shutdown path in main.c — keeps every fatal exit() out of the
 * MorphOS -noixemul crt0 post-main-teardown freeze that
 * cl_thread_restore_main_tls() exists to prevent.  Safe/no-op on every other
 * platform (see cl_thread_restore_main_tls). */
void cl_fatal_diag(const char *fmt, ...)
{
    /* Static, not on the stack: several callers are stack-overflow and
     * stack-corruption paths where a 512-byte frame is exactly what must not
     * be taken.  Fatal paths never run concurrently in a way that makes the
     * shared buffer worse than losing the message. */
    static char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = '\0';
    platform_write_string(buf);
    platform_flush_output();
}

CL_NORETURN void cl_fatal_exit(int code)
{
    /* Push the diagnostic the caller just wrote out to the OS before the
     * process goes away.  Nothing above us is buffered today, but a fatal
     * exit is precisely where a stranded buffer costs the most: the log a
     * post-mortem is read from would end on an unrelated earlier line with
     * no trace of why the run stopped. */
    platform_flush_output();
    cl_thread_restore_main_tls();
    exit(code);
}

/* Unwind after the debugger returned — shared between cl_error and
 * cl_error_from_condition. Assumes cl_error_code/cl_error_msg are set. */
CL_NORETURN static void cl_error_unwind(int code)
{
    /* Check for interposing unwind-protect frames in NLX stack.
     * Skip stale frames whose VM frame was reused by a tail call —
     * longjmping to a stale UWPROT restores wrong code/constants.
     *
     * Bounded below by the innermost active C error frame's NLX watermark:
     * only UWPROT frames pushed inside that frame's dynamic extent sit
     * BETWEEN the error and the catch site we are about to longjmp to.  A
     * UWPROT below the watermark belongs to an enclosing scope the catch
     * site is not unwinding past — running its cleanup would skip the catch
     * entirely (see CL_ErrorFrame.saved_nlx_top).  With no error frame at
     * all the whole stack is in play, exactly as before. */
    {
        int i;
        int nlx_floor = (cl_error_frame_top > 0)
                        ? cl_error_frames[cl_error_frame_top - 1].saved_nlx_top
                        : 0;
        for (i = cl_nlx_top - 1; i >= nlx_floor; i--) {
            if (cl_nlx_stack[i].type == CL_NLX_UWPROT) {
                /* A JIT-owned frame pushed with no VM frame below it
                 * (vm_fp 0: a thread whose whole call chain is native)
                 * has nothing to have gone stale -- frames[-1] is not a
                 * frame.  Same rule as the JIT's jit_nlx_frame_is_stale. */
                if (cl_nlx_stack[i].vm_fp > 0) {
                    CL_Frame *tf = &cl_vm.frames[cl_nlx_stack[i].vm_fp - 1];
                    if (tf->code != cl_nlx_stack[i].code)
                        continue;
                }
                cl_pending_throw = 2;
                cl_pending_error_code = code;
                strncpy(cl_pending_error_msg, cl_error_msg,
                        sizeof(cl_pending_error_msg) - 1);
                cl_pending_error_msg[sizeof(cl_pending_error_msg) - 1] = '\0';
                cl_nlx_top = i;
                cl_nlx_jump(&cl_nlx_stack[i]);
            }
        }
    }

    /* No UWPROT found — propagate to the top C error frame. */
    cl_error_frame_longjmp(code);
}

static CL_NORETURN void error_raise(int code, CL_Obj cell_name);

void cl_error(int code, const char *fmt, ...)
{
    va_list ap;

    cl_error_code = code;

    va_start(ap, fmt);
    vsnprintf(cl_error_msg, sizeof(cl_error_msg), fmt, ap);
    va_end(ap);

    error_raise(code, CL_UNBOUND);
}

void cl_error_cell(int code, CL_Obj name, const char *fmt, ...)
{
    va_list ap;

    cl_error_code = code;

    va_start(ap, fmt);
    vsnprintf(cl_error_msg, sizeof(cl_error_msg), fmt, ap);
    va_end(ap);

    error_raise(code, name);
}

/* cl_error's body after the message is formatted.  CELL_NAME is
 * CL_UNBOUND for a plain error, else the CELL-ERROR's :name. */
static void error_raise(int code, CL_Obj cell_name)
{

    /* Macroexpansion error context is snapshot in cl_capture_backtrace —
     * the bottleneck every raise path (cl_error, cl_raise_condition, the
     * VM/builtin type-error helpers) funnels through. */

    /* Exit request: skip debugger/conditions, just unwind */
    if (code == CL_ERR_EXIT) {
        cl_nlx_top = 0;
        cl_saved_pending_top = 0;
        cl_pending_throw = 0;
        cl_dynbind_restore_to(0);
        cl_handler_top = 0;
        cl_handler_active_mask = 0;
        cl_restart_top = 0;
        cl_gc_reset_roots();
        cl_jit_restore_depth(0, NULL);
        cl_debugger_depth = 0;
        cl_in_debugger = 0;
        cl_printer_state_reset();
        if (cl_error_frame_top > 0) {
            /* Don't decrement here — CL_UNCATCH at the catch site pops.
             * For nested frames, restore gc_root_count to the catch-site
             * snapshot so any roots pushed in unwound C frames are dropped
             * (cl_gc_reset_roots above zeroed it; that suffices, but the
             * explicit restore is symmetric with the cl_error_unwind path
             * and keeps any permanent roots installed by the outer frame). */
            gc_root_count = cl_error_frames[cl_error_frame_top - 1].saved_gc_roots;
            cl_jit_restore_depth(cl_error_frames[cl_error_frame_top - 1].saved_jit_depth,
                             cl_error_frames[cl_error_frame_top - 1].saved_jit_c_floor);
            cl_fasl_reader_restore_count(cl_error_frames[cl_error_frame_top - 1].saved_fasl_readers);
        cl_fasl_writer_restore_count(cl_error_frames[cl_error_frame_top - 1].saved_fasl_writers);
            cl_compiler_force_restore_to(cl_error_frames[cl_error_frame_top - 1].saved_active_compiler);
            CL_LONGJMP(cl_error_frames[cl_error_frame_top - 1].buf, code);
        }
        cl_fatal_exit(cl_exit_code);
    }

    /* Capture backtrace while VM frames are still intact */
    cl_capture_backtrace();

    /* Signal through condition handler stack before unwinding */
    {
        CL_Obj cond;
        /* cell_name, cond: rooted across the allocations, the handlers
         * and the debugger, which allocate freely (bi_error's
         * discipline); the unwind below restores the root count, so no
         * explicit pop. */
        CL_GC_PROTECT(cell_name);
        cond = cl_create_condition_from_error(code, cl_error_msg);
        CL_GC_PROTECT(cond);
        if (cell_name != CL_UNBOUND && CL_CONDITION_P(cond)) {
            CL_Obj pair = cl_cons(KW_NAME, cell_name);
            pair = cl_cons(pair, ((CL_Condition *)CL_OBJ_TO_PTR(cond))->slots);
            ((CL_Condition *)CL_OBJ_TO_PTR(cond))->slots = pair;
        }
        cl_signal_condition(cond);
        /* Invoke debugger before unwinding (returns if user picks "top level") */
        cl_invoke_debugger(cond);
        /* The condition this unwind carries — read at a foreign-callback
         * boundary (thread.h last_condition).  Written last, so a nested
         * error handled inside a handler cannot leave its own condition
         * here. */
        CT->last_condition = cond;
    }

    cl_error_unwind(code);
}

/* Abort the current thread quietly.
 *
 * Used by destroy-thread: the "Thread destroyed" unwind is a controlled,
 * internal abort requested by another thread (mp:destroy-thread), not a
 * user-facing error that warrants the debugger.  We still signal the condition
 * so any handler-case / unwind-protect in the thread body runs (matching
 * cl_error's behaviour and CCL/SBCL thread-abort semantics), but we deliberately
 * skip cl_invoke_debugger so a killed SLY worker unwinds silently instead of
 * emitting a "Debugger entered" banner on every disconnect. */
CL_NORETURN void cl_abort_current_thread(const char *msg)
{
    /* A wanted death, not one to report (see cl_run_thread_death_hooks). */
    CT->destroyed = 1;
    cl_error_code = CL_ERR_GENERAL;
    strncpy(cl_error_msg, msg, sizeof(cl_error_msg) - 1);
    cl_error_msg[sizeof(cl_error_msg) - 1] = '\0';

    /* Capture backtrace while VM frames are still intact, then signal so
     * handler-case / unwind-protect cleanups run — but never enter the
     * debugger. */
    cl_capture_backtrace();
    {
        CL_Obj cond = cl_create_condition_from_error(CL_ERR_GENERAL, cl_error_msg);
        CL_GC_PROTECT(cond);
        cl_signal_condition(cond);
        CT->last_condition = cond;   /* see cl_error */
    }

    cl_error_unwind(CL_ERR_GENERAL);  /* no debugger — does not return */
}

/* Unwind with an existing condition object (e.g. one built by the Lisp
 * ERROR builtin). Caller is responsible for having already run
 * cl_signal_condition — we skip re-signaling to avoid running handlers twice.
 * Unlike cl_error, this preserves the original condition's type and slots
 * so the debugger can dispatch PRINT-OBJECT and show a meaningful report. */
void cl_error_from_condition(CL_Obj condition)
{
    CL_Condition *c;
    CL_Obj report = CL_NIL;

    if (!CL_CONDITION_P(condition)) {
        cl_error(CL_ERR_GENERAL, "cl_error_from_condition: not a condition");
    }

    /* Rooted across the print-object-hook dispatch below (cl_vm_apply, which
     * allocates freely) and the debugger invocation: this function re-derives
     * from `condition` afterward (CL_OBJ_TO_PTR, cl_invoke_debugger, and the
     * CT->last_condition store a GC-stress cycle can relocate) and the caller's
     * own CL_GC_PROTECT of its copy does not keep this parameter's separate
     * stack slot current across a compaction. Unprotected on exit, same as
     * cl_error/cl_abort_current_thread: cl_error_unwind's longjmp restores
     * gc_root_count. */
    CL_GC_PROTECT(condition);

    /* Map condition type → numeric code so callers (incl. C-level tests
     * via eval_print's "ERROR:N") see the same code as the legacy
     * cl_error(CL_ERR_*, ...) path. */
    {
        CL_Obj t = ((CL_Condition *)CL_OBJ_TO_PTR(condition))->type_name;
        if      (t == SYM_TYPE_ERROR)               cl_error_code = CL_ERR_TYPE;
        else if (t == SYM_UNBOUND_VARIABLE_COND)    cl_error_code = CL_ERR_UNBOUND;
        else if (t == SYM_UNDEFINED_FUNCTION_COND)  cl_error_code = CL_ERR_UNDEFINED;
        else if (t == SYM_DIVISION_BY_ZERO)         cl_error_code = CL_ERR_DIVZERO;
        else if (t == SYM_ARITHMETIC_ERROR)         cl_error_code = CL_ERR_OVERFLOW;
        else if (t == SYM_PROGRAM_ERROR)            cl_error_code = CL_ERR_ARGS;
        else if (t == SYM_FILE_ERROR)               cl_error_code = CL_ERR_FILE;
        else                                        cl_error_code = CL_ERR_GENERAL;
    }

    /* Try PRINT-OBJECT dispatch (e.g. ASDF conditions rely on it for their
     * report). Falls through to the condition's report_string and finally
     * the type name. */
    if (!CL_NULL_P(SYM_PRINT_OBJECT_HOOK)) {
        CL_Obj hook_val = cl_symbol_value(SYM_PRINT_OBJECT_HOOK);
        if (!CL_NULL_P(hook_val)) {
            CL_Obj hook_args[1];
            CL_Obj result;
            hook_args[0] = condition;
            result = cl_vm_apply(hook_val, hook_args, 1);
            /* The hook prints via WITH-OUTPUT-TO-STRING, which returns a
             * TYPE_WIDE_STRING whenever the report holds any non-ASCII
             * character — rejecting it dropped the message and left only
             * the type name. */
            if (!CL_NULL_P(result) && CL_ANY_STRING_P(result)) {
                report = result;
            }
        }
    }

    /* No PRINT-OBJECT text: the condition's own report — formatted
     * :format-control, report string, or the standard slot-derived report
     * of its type (a bare (error 'type-error :datum 5 :expected-type 'list)
     * used to print just "ERROR: TYPE-ERROR").  Allocates; condition is
     * rooted above. */
    if (CL_NULL_P(report)) {
        report = cl_condition_report(condition);
    }
    c = (CL_Condition *)CL_OBJ_TO_PTR(condition);

    if (!CL_NULL_P(report) && CL_ANY_STRING_P(report)) {
        /* Bounded UTF-8 conversion — the report is wide for any
         * non-ASCII message, and CL_String->data on a wide object reads
         * UTF-32 code units as bytes. */
        char rbuf[512];                 /* cl_error_msg's own capacity */
        char typebuf[96];
        cl_string_to_utf8(report, rbuf, sizeof(rbuf));
        cl_prin1_to_string(c->type_name, typebuf, sizeof(typebuf));
        snprintf(cl_error_msg, sizeof(cl_error_msg), "%s: %s",
                 typebuf, rbuf);
    } else {
        cl_prin1_to_string(c->type_name, cl_error_msg, sizeof(cl_error_msg));
    }

    cl_capture_backtrace();
    cl_invoke_debugger(condition);
    CT->last_condition = condition;   /* see cl_error */
    cl_error_unwind(cl_error_code);
}

void cl_error_print(void)
{
    /* The tag and the text are coloured apart: on an Amiga console the tag
     * is an inverse-video badge, the text bold (color.h). */
    cl_color_set(CL_COLOR_ERROR_TAG);
    cl_write_cstring_to_error("ERROR:");
    cl_color_reset();
    cl_color_set(CL_COLOR_ERROR);
    cl_write_cstring_to_error(" ");
    cl_write_cstring_to_error(cl_error_msg);
    cl_color_reset();
    cl_write_cstring_to_error("\n");
    {
        CL_Obj ef = cl_expansion_ctx_get(cl_error_expanding_form_sym);
        if (!CL_NULL_P(ef)) {
            char fbuf[256];
            cl_prin1_to_string(ef, fbuf, (int)sizeof(fbuf));
            cl_write_cstring_to_error("While macroexpanding: ");
            cl_write_cstring_to_error(fbuf);
            cl_write_cstring_to_error("\n");
            cl_tlv_set(CT, cl_error_expanding_form_sym, CL_NIL);
        }
    }
    if (cl_backtrace_buf[0] != '\0') {
        cl_write_cstring_to_error("Backtrace:\n");
        cl_write_cstring_to_error(cl_backtrace_buf);
    }
}
