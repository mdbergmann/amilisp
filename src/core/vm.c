#include "vm.h"
#include "opcodes.h"
#include "symbol.h"
#include "package.h"
#include "mem.h"
#include "bignum.h"
#include "ratio.h"
#include "float.h"
#include "error.h"
#include "compiler.h"
#include "fasl.h"       /* cl_fasl_{reader,writer}_unwind_to at the NLX landing */
#include "builtins.h"   /* cl_ffi_stub_call, cl_amiga_call_via_base_sym */
#include "printer.h"
#include "string_utils.h"
#include "stream.h"
#include "../platform/platform.h"
#include "../platform/platform_thread.h"
#include "../jit/jit.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

/* Shared byte-vector store range check (builtins_array.c, builtins.h). */
extern int32_t cl_bytevec_check_value(CL_Obj value, int is_signed,
                                      int elt_shift, const char *ctx);

/* Local macros for statics moved into CL_Thread */
#define vm_extra_args       (CT->vm_extra_args_buf)
#define vm_extra_args_count (CT->vm_extra_count)
#define vm_trace            (CT->vm_trace_buf)
#define vm_trace_idx        (CT->vm_trace_pos)
#define vm_eval_max_depth   (CT->vm_max_eval_depth)
#define VM_TRACE_SIZE       CL_VM_TRACE_SIZE

/* ---- Ctrl-C break-in ----
 *
 * The interpreter polls platform_break_pending() at loop back-edges
 * (backward OP_JMP) and at OP_CALL/OP_TAILCALL, counter-gated so the
 * platform check (an exec call on Amiga, a flag read on POSIX) runs only
 * every 1024th visit.  Together those sites cover every way Lisp code can
 * loop, so a wedged computation — including one spinning on a corrupted
 * data structure — can be interrupted and made to name its location.
 *
 * On a pending break: capture the backtrace AT THE INTERRUPTED OPCODE,
 * then funcall CL:BREAK ("Interrupted by Ctrl-C"), whose debugger shows
 * that backtrace and offers CONTINUE to resume the interrupted
 * computation exactly where it was.  Non-interactive runs (debugger
 * disabled/no tty) print the interrupt notice and continue; a second
 * Ctrl-C before the first is consumed force-exits (POSIX).  Before
 * boot.lisp has defined BREAK, fall back to printing the backtrace and
 * continuing.
 *
 * Safe between opcodes: bytecode code arrays live outside the arena
 * (platform_alloc), so the nested VM run inside BREAK — including any GC
 * it triggers — cannot move the interpreter's code pointer, and the
 * caller's frame/stack discipline is exactly that of OP_CALL.
 *
 * The poll counter lives in CL_Thread (break_poll_ctr), NOT in a static:
 * it is written on every call by every thread, and a shared static made
 * all threads bounce one cache line — a 3–4x slowdown of the 8-thread
 * call loop (bisected to the commit that introduced it; see
 * docs/sento-bench-results-0.8.md).  cl_vm_run already holds its thread
 * in `thr`, so the per-thread counter is free to reach. */

static void vm_handle_break(void)
{
    CL_Obj sym;
    CL_Symbol *s;
    if (!platform_break_pending())
        return;
    cl_capture_backtrace();
    cl_write_cstring_to_error("\n;; Interrupt (Ctrl-C)\n");
    sym = cl_find_symbol("BREAK", 5, cl_package_cl);
    s = CL_SYMBOL_P(sym) ? (CL_Symbol *)CL_OBJ_TO_PTR(sym) : NULL;
    if (s && !CL_NULL_P(s->function)) {
        CL_Obj msg = cl_make_string("Interrupted by Ctrl-C", 21);
        CL_GC_PROTECT(msg);
        cl_vm_apply(s->function, &msg, 1);
        CL_GC_UNPROTECT(1);
    } else {
        /* Break arrived before boot defined BREAK — report and continue. */
        cl_write_cstring_to_error(cl_backtrace_buf);
    }
}

/* The same check for native loops (cl_jit_runtime_loop_poll), which keep
 * their own countdown in CL_Thread.jit_loop_ctr. */
void cl_vm_poll_break(void)
{
    vm_handle_break();
}

/* Counter-gated poll, cheap enough for the OP_JMP/OP_CALL hot path.  Uses
 * cl_vm_run's `thr` local (per-thread counter — see the note above). */
#define VM_POLL_BREAK() \
    do { \
        if (((++thr->break_poll_ctr) & 1023) == 0) { \
            frame->ip = ip; \
            vm_handle_break(); \
        } \
    } while (0)

/* CL_SAFEPOINT on the thread the dispatch loop already holds.  The generic
 * macro (thread.h) re-derives the thread through cl_get_current_thread —
 * a TLS lookup on every OP_CALL once a second thread exists. */
#define VM_SAFEPOINT() \
    do { \
        if (thr->gc_requested) cl_gc_safepoint(); \
        if (thr->interrupt_pending) cl_thread_handle_interrupt(thr); \
    } while (0)

/* Debug: UWP stack watchpoint.  Activate with -DCL_DEBUG_UWP */
#ifdef CL_DEBUG_UWP
static int dbg_watch_idx = -1;
static CL_Obj dbg_watch_orig = 0;
static int dbg_watch_nlx = -1;
static void dbg_check_watch_impl(const char *where) {
    if (dbg_watch_idx >= 0 && dbg_watch_nlx >= 0 &&
        cl_nlx_top > dbg_watch_nlx &&
        cl_vm.stack[dbg_watch_idx] != dbg_watch_orig) {
        fprintf(stderr, "[WATCH] stack[%d] changed from 0x%x to 0x%x by %s, fp=%d sp=%d\n",
                dbg_watch_idx, (unsigned)dbg_watch_orig,
                (unsigned)cl_vm.stack[dbg_watch_idx], where, cl_vm.fp, cl_vm.sp);
        {
            int fi;
            for (fi = cl_vm.fp - 1; fi >= 0 && fi >= cl_vm.fp - 15; fi--) {
                CL_Frame *f = &cl_vm.frames[fi];
                const char *fn = "?";
                CL_Bytecode *fbc = NULL;
                if (CL_CLOSURE_P(f->bytecode)) {
                    CL_Closure *cc = (CL_Closure *)CL_OBJ_TO_PTR(f->bytecode);
                    fbc = (CL_Bytecode *)CL_OBJ_TO_PTR(cc->bytecode);
                } else if (CL_BYTECODE_P(f->bytecode)) {
                    fbc = (CL_Bytecode *)CL_OBJ_TO_PTR(f->bytecode);
                }
                if (fbc && fbc->name != CL_NIL) fn = cl_symbol_name(fbc->name);
                else if (fbc) fn = "<anon>";
                fprintf(stderr, "  frame[%d] fn=%s bp=%d n_locals=%d\n",
                        fi, fn, f->bp, f->n_locals);
            }
        }
        dbg_watch_idx = -1;
    }
}
#define DBG_CHECK_WATCH(w) dbg_check_watch_impl(w)
#else
#define DBG_CHECK_WATCH(w) ((void)0)
#endif

/* Opcode-frequency profiler — build with -DPROFILE_OPCODES.
 * Counters and reset/dump are always defined (no-op when disabled)
 * so the (amilisp::%op-counts-*) builtins are present unconditionally
 * and Lisp profiling scripts stay portable. */
#ifdef PROFILE_OPCODES
static uint32_t cl_op_counts_arr[256];
/* Adjacent-pair counts, [previous op][this op], for choosing
 * superinstructions (specs/performance.md 4.3).  The previous op is
 * tracked per cl_vm_run activation, so a pair that straddles a call into
 * a bytecode callee (CALL -> the callee's first op) is counted too; those
 * are not fusable and are easy to spot in the dump.  Counters are
 * process-wide and racy on purpose — this is a profile, not a test. */
static uint32_t cl_op_pair_counts[256][256];

static const char *cl_opcode_name(uint8_t op)
{
    const CL_OpcodeInfo *info = cl_opcode_info(op);
    return info ? info->name : "?";
}
#endif /* PROFILE_OPCODES */

void cl_op_counts_reset(void)
{
#ifdef PROFILE_OPCODES
    uint32_t i;
    for (i = 0; i < 256; i++) cl_op_counts_arr[i] = 0;
    memset(cl_op_pair_counts, 0, sizeof(cl_op_pair_counts));
#endif
}

void cl_op_counts_dump(FILE *out)
{
#ifdef PROFILE_OPCODES
    uint32_t i, j;
    uint32_t total = 0;
    /* indices sorted by count, desc */
    uint8_t idx[256];
    for (i = 0; i < 256; i++) { idx[i] = (uint8_t)i; total += cl_op_counts_arr[i]; }
    /* simple insertion sort by count desc — only ~80 non-zero entries */
    for (i = 1; i < 256; i++) {
        uint8_t k = idx[i];
        uint32_t kv = cl_op_counts_arr[k];
        j = i;
        while (j > 0 && cl_op_counts_arr[idx[j-1]] < kv) {
            idx[j] = idx[j-1]; j--;
        }
        idx[j] = k;
    }
    fprintf(out, "=== opcode counts (total=%lu) ===\n", (unsigned long)total);
    fprintf(out, "  %-18s %12s %8s\n", "op", "count", "pct");
    for (i = 0; i < 256; i++) {
        uint32_t c = cl_op_counts_arr[idx[i]];
        if (c == 0) break;
        fprintf(out, "  0x%02x %-13s %12lu %7.2f%%\n",
                idx[i], cl_opcode_name(idx[i]),
                (unsigned long)c,
                total ? (100.0 * (double)c / (double)total) : 0.0);
    }
    /* Top adjacent pairs: repeatedly pick the largest remaining cell. */
    fprintf(out, "=== top opcode pairs (prev -> this) ===\n");
    for (i = 0; i < 48; i++) {
        uint32_t best = 0, bp = 0, bo = 0, p, o;
        for (p = 0; p < 256; p++)
            for (o = 0; o < 256; o++)
                if (cl_op_pair_counts[p][o] > best) {
                    best = cl_op_pair_counts[p][o]; bp = p; bo = o;
                }
        if (best == 0) break;
        fprintf(out, "  %-13s -> %-13s %12lu %7.2f%%\n",
                cl_opcode_name((uint8_t)bp), cl_opcode_name((uint8_t)bo),
                (unsigned long)best,
                total ? (100.0 * (double)best / (double)total) : 0.0);
        cl_op_pair_counts[bp][bo] = 0;
    }
    fflush(out);
#else
    fprintf(out, "PROFILE_OPCODES not compiled in\n");
    fflush(out);
#endif
}

/* All per-thread state (VM, NLX, dyn/handler/restart stacks, MV,
 * trace, backtrace, pending throw) now lives in CL_Thread.
 * Compatibility macros in thread.h redirect the old names. */

void cl_dynbind_restore_to(int mark)
{
    CL_Thread *t = CT;
    int touched_package = 0;
    while (cl_dyn_top > mark) {
        cl_dyn_top--;
        {
            CL_Obj old_val = cl_dyn_stack[cl_dyn_top].old_value;
            CL_Obj sym = cl_dyn_stack[cl_dyn_top].symbol;
            if (sym == SYM_STAR_PACKAGE)
                touched_package = 1;
            if (old_val == CL_TLV_ABSENT)
                cl_tlv_remove(t, sym);
            else
                cl_tlv_set(t, sym, old_val);
        }
    }
    if (touched_package)
        cl_sync_current_package_from_dynamic();
}

void cl_gf_types_boot_init(void);   /* defined below with the GF type table */

void cl_vm_init(uint32_t stack_size, int frame_size)
{
    if (stack_size == 0) stack_size = CL_VM_STACK_SIZE;
    if (frame_size == 0) frame_size = CL_VM_FRAME_SIZE;

    cl_vm.stack = (CL_Obj *)platform_alloc(stack_size * sizeof(CL_Obj));
    cl_vm.stack_size = stack_size;
    cl_vm.frames = (CL_Frame *)platform_alloc(frame_size * sizeof(CL_Frame));
    cl_vm.frame_size = frame_size;

    cl_vm.sp = 0;
    cl_vm.fp = 0;
    cl_nlx_top = 0;
    cl_saved_pending_top = 0;
    cl_dyn_top = 0;
    cl_handler_top = 0;
    cl_handler_active_mask = 0;
    cl_restart_top = 0;
    cl_pending_throw = 0;
    cl_mv_count = 1;
    cl_trace_depth = 0;

    /* Funcallable-GF type table: register its GC roots HERE, while still
     * single-threaded (see cl_gf_types_boot_init — racing the old fully-
     * lazy registration double-registered roots, tier-4 audit V10). */
    cl_gf_types_boot_init();
}

void cl_vm_shutdown(void)
{
    if (cl_vm.stack) {
        platform_free(cl_vm.stack);
        cl_vm.stack = NULL;
    }
    if (cl_vm.frames) {
        platform_free(cl_vm.frames);
        cl_vm.frames = NULL;
    }
}

void cl_vm_push(CL_Obj val)
{
    if (cl_vm.sp >= (int)cl_vm.stack_size)
        cl_error(CL_ERR_OVERFLOW, "VM stack overflow");
#ifdef DEBUG_VM_PUSH_GUARD
    /* Catch the bad write at its source: if a "heap-tagged" CL_Obj points
     * past the arena, something just used a raw pointer where an arena
     * offset belongs.  Backtrace + abort on first occurrence. */
    if (CL_HEAP_P(val) && val >= cl_heap.arena_size) {
        fprintf(stderr,
                "[VM-PUSH-GUARD] cl_vm_push: val=0x%08x out of arena (size=0x%08x) sp=%d fp=%d\n",
                (unsigned)val, (unsigned)cl_heap.arena_size, cl_vm.sp, cl_vm.fp);
        cl_capture_backtrace();
        fprintf(stderr, "%s", cl_backtrace_buf);
        fflush(stderr);
        abort();
    }
#endif
    cl_vm.stack[cl_vm.sp++] = val;
#ifdef CL_DEBUG_UWP
    if (dbg_watch_idx >= 0 && (cl_vm.sp - 1) == dbg_watch_idx && val != dbg_watch_orig)
        DBG_CHECK_WATCH("cl_vm_push");
#endif
#ifdef DEBUG_GC
    /* Detect push of a freed (poison-filled) heap object */
    if (CL_HEAP_P(val) && val < cl_heap.arena_size) {
        uint8_t *p = (uint8_t *)CL_OBJ_TO_PTR(val);
        /* Check if bytes 8-11 (after CL_FreeBlock header) are poison 0xDE */
        if (p[8] == 0xDE && p[9] == 0xDE && p[10] == 0xDE && p[11] == 0xDE) {
            char buf[256];
            CL_Obj cur_bc = (cl_vm.fp > 0) ? cl_vm.frames[cl_vm.fp - 1].bytecode : CL_NIL;
            snprintf(buf, sizeof(buf),
                     "VM-PUSH-FREED: sp=%d val=0x%08x hdr=0x%08x bc=0x%08x fp=%d\n",
                     cl_vm.sp - 1, (unsigned)val,
                     (unsigned)*(uint32_t *)p,
                     (unsigned)cur_bc, cl_vm.fp);
            platform_write_string(buf);
            cl_capture_backtrace();
            platform_write_string(cl_backtrace_buf);
            platform_write_string("\n");
        }
    }
#endif
}

/* OP_CMP_BR's slow path (vm.h).  Mirrors OP_LT..OP_NUMEQ's non-fixnum arm
 * and OP_CHAREQ's gate exactly, error text included. */
int cl_vm_compare_kind(CL_Obj a, CL_Obj b, int cmp)
{
    static const char *const names[6] = { "<", ">", "<=", ">=", "=", "CHAR=" };
    const char *opname = names[cmp < 6 ? cmp : 0];
    if (cmp == CL_CMP_BR_CHAREQ) {
        if (!CL_CHAR_P(a) || !CL_CHAR_P(b))
            cl_error(CL_ERR_TYPE, "CHAR=: not a character");
        return a == b;
    }
    if (cmp == CL_CMP_BR_NUMEQ) {
        if (CL_FIXNUM_P(a) && CL_FIXNUM_P(b)) return a == b;
        if (!CL_NUMBER_P(a)) cl_signal_type_error(a, "NUMBER", opname);
        if (!CL_NUMBER_P(b)) cl_signal_type_error(b, "NUMBER", opname);
        return cl_numeric_equal(a, b);
    }
    {
        int c;
        if (CL_FIXNUM_P(a) && CL_FIXNUM_P(b)) {
            int32_t av = CL_FIXNUM_VAL(a), bv = CL_FIXNUM_VAL(b);
            c = av < bv ? -1 : av > bv ? 1 : 0;
        } else {
            if (!CL_REALP(a)) cl_signal_type_error(a, "REAL", opname);
            if (!CL_REALP(b)) cl_signal_type_error(b, "REAL", opname);
            c = cl_arith_compare(a, b);
        }
        switch (cmp) {
        case CL_CMP_BR_LT: return c <  0;
        case CL_CMP_BR_GT: return c >  0;
        case CL_CMP_BR_LE: return c <= 0;
        default:           return c >= 0;
        }
    }
}

CL_Obj cl_vm_pop(void)
{
    if (cl_vm.sp <= 0)
        cl_error(CL_ERR_OVERFLOW, "VM stack underflow");
    return cl_vm.stack[--cl_vm.sp];
}

/* Forward declarations */
static CL_Obj call_builtin(CL_Thread *thr, CL_Function *func,
                           CL_Obj *args, int nargs);
static CL_Obj cl_vm_run(int base_fp, int base_nlx);
void vm_trace_dump(void);

/* Forward decls for the backtrace helpers used by the overflow summary
 * (defined later in this file). */
static CL_Obj get_func_name(CL_Obj func_obj);
static int lookup_source_line(CL_Bytecode *bc, uint32_t ip);
static CL_Bytecode *get_frame_bytecode(CL_Frame *f);

/* m68k native frames carry no ip of their own: the JIT works it out from
 * the stack (jit_m68k.c) before anything here reads a frame's ip. */
static void bt_sync_native_ips(void)
{
#ifdef JIT_M68K
    cl_jit_resolve_frame_ips();
#endif
}

/* Summarize the NLX stack at an overflow.  Compiled in unconditionally because
 * an NLX overflow is fatal and we want this diagnostic in the field, not just
 * a bare "NLX stack overflow" error.  An overflow almost always means a frame
 * is being *leaked* (pushed but never popped) on some hot path, so we print:
 *  - a per-type histogram (which kind of frame is piling up),
 *  - the most-repeated tag among the top frames (the leaking construct), and
 *  - the function that owns the leaking frame (file:line), which points
 *    straight at the offending Lisp form. */
void cl_nlx_overflow_summary(const char *where)
{
    int k;
    int counts[CL_NLX_TYPE_COUNT] = {0,0,0,0,0};
    unsigned top_tag = 0; int top_tag_cnt = 0;
    int window = cl_nlx_top < 64 ? cl_nlx_top : 64;

    fprintf(stderr, "[NLX-OVERFLOW] %s top=%d max=%d\n",
            where, cl_nlx_top, cl_nlx_max);
    for (k = 0; k < cl_nlx_top && k < cl_nlx_max; k++) {
        int t = cl_nlx_stack[k].type;
        if (t >= 0 && t < CL_NLX_TYPE_COUNT) counts[t]++;
    }
    fprintf(stderr, "[NLX-OVERFLOW] by type: CATCH=%d UWPROT=%d BLOCK=%d TAGBODY=%d HANDLER-CASE=%d\n",
            counts[0], counts[1], counts[2], counts[3], counts[4]);
    /* Most-repeated tag among the top `window` frames = the leak signature. */
    for (k = cl_nlx_top - 1; k >= 0 && k > cl_nlx_top - window - 1; k--) {
        unsigned tg = (unsigned)cl_nlx_stack[k].tag;
        int cnt = 0, j;
        for (j = cl_nlx_top - 1; j >= 0; j--)
            if ((unsigned)cl_nlx_stack[j].tag == tg) cnt++;
        if (cnt > top_tag_cnt) { top_tag_cnt = cnt; top_tag = tg; }
    }
    fprintf(stderr, "[NLX-OVERFLOW] most-repeated tag=0x%08x count=%d\n",
            top_tag, top_tag_cnt);
    /* Function owning the leaking frame (vm_fp of the top NLX entry). */
    bt_sync_native_ips();
    if (cl_nlx_top > 0) {
        int lf = cl_nlx_stack[cl_nlx_top - 1].vm_fp - 1;
        if (lf >= 0 && lf < cl_vm.fp) {
            CL_Frame *f = &cl_vm.frames[lf];
            CL_Obj nm = get_func_name(f->bytecode);
            CL_Bytecode *bc = get_frame_bytecode(f);
            int line = bc ? lookup_source_line(bc, f->ip) : 0;
            const char *file = bc ? bc->source_file : NULL;
            fprintf(stderr, "[NLX-OVERFLOW] leaking frame[%d] func=%s file=%s line=%d\n",
                    lf, CL_SYMBOL_P(nm) ? cl_symbol_name(nm) : "<anon>",
                    file ? file : "?", line);
        }
    }
    cl_capture_backtrace();
    fprintf(stderr, "[NLX-OVERFLOW] backtrace:\n%s", cl_backtrace_buf);
    fflush(stderr);
}


#ifdef DEBUG_NLX
/* Route NLX debug output through the platform stdout handle (Output() on
 * AmigaOS/MorphOS) instead of C-stdio stderr.  Under -noixemul (and AmigaOS
 * generally) stderr is a separate handle that the shell's redirect does not
 * capture, so fprintf(stderr) never reaches the log that holds the boot/FATAL
 * lines.  cl_write_cstring_to_stdout writes to the same handle those use. */
void cl_nlx_dbg(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    cl_write_cstring_to_stdout(buf);
}

/* Dump the current NLX (non-local-exit) frame stack.  Built only with
 * -DDEBUG_NLX; used to diagnose platform-specific setjmp/longjmp divergence
 * (e.g. MorphOS PPC vs. real 68k).  Non-static so builtins_io.c (bi_throw)
 * can call it at the "No catch for tag" site. */
void cl_nlx_debug_dump(const char *where, unsigned tag)
{
    int k;
    static const char *tn[4] = { "CATCH", "UWPROT", "BLOCK", "TAGBODY" };
    cl_nlx_dbg("[NLX] %s tag=0x%08x top=%d\n", where, tag, cl_nlx_top);
    for (k = cl_nlx_top; k >= 0 && k < cl_nlx_max; k--) {
        int t = cl_nlx_stack[k].type;
        cl_nlx_dbg("        [%d] type=%s tag=0x%08x vm_sp=%d vm_fp=%d\n",
                k, (t >= 0 && t < 4) ? tn[t] : "?",
                (unsigned)cl_nlx_stack[k].tag,
                cl_nlx_stack[k].vm_sp, cl_nlx_stack[k].vm_fp);
    }
}
#endif

/* The tail of a "no catch / no block / no tagbody" error message when the
 * target does exist but sits below cl_nlx_floor — i.e. outside the foreign
 * callback that is running (thread.h "Foreign-callback boundary").  Empty
 * otherwise.  TYPE is the CL_NLX_* kind to look for, -1 for any. */
const char *cl_nlx_boundary_hint(CL_Obj tag, int type)
{
    int i;
    if (cl_nlx_floor <= 0) return "";
    for (i = cl_nlx_floor - 1; i >= 0; i--) {
        if ((type < 0 || cl_nlx_stack[i].type == type) &&
            cl_nlx_stack[i].tag == tag)
            return " -- the target lies outside the foreign callback that is "
                   "running: a non-local exit cannot cross the C frames of the "
                   "caller (signal an error instead; it is re-signaled once the "
                   "foreign call returns)";
    }
    return "";
}

/* Funcallable instance support: a standard-generic-function struct is a
 * callable object whose "discriminating function" lives at slot 3.  The
 * VM + funcall/apply treat it as transparent — they unwrap to slot 3 and
 * dispatch normally.  Slot 3 may itself be any callable (closure, bytecode,
 * builtin), including one installed by SET-FUNCALLABLE-INSTANCE-FUNCTION. */
/* Registered funcallable generic-function struct-type name symbols.  Slot 0 is
 * always STANDARD-GENERIC-FUNCTION; additional entries are subclasses declared
 * via :generic-function-class (CLHS) and registered from Lisp through
 * cl_register_funcallable_gf_type.  A struct is a funcallable instance iff its
 * type_desc matches one of these.  The set is tiny in practice (the common
 * case is a single entry), so a linear scan on the call hot path is cheap.
 *
 * Each slot is a GC root: the symbols are arena-resident and the compacting GC
 * relocates them, so without registration the cached offsets go stale after a
 * compaction and a live GF struct would no longer be recognized ("Not a
 * function: heap object type 10") — a heap-layout-dependent corruption seen
 * under GC stress by DEFCLASS redefinition. */
#define CL_MAX_GF_TYPES 16
static CL_Obj cl_gf_type_syms[CL_MAX_GF_TYPES];
static int cl_gf_type_count = 0;
static int cl_gf_types_inited = 0;

static int cl_gf_roots_registered = 0;

/* Root registration split out and run DETERMINISTICALLY from cl_vm_init
 * (single-threaded boot): two threads racing the old fully-lazy init
 * would both cl_gc_register_root the same addresses, and gc_forward is
 * not idempotent — a double-registered root is silently corrupted by the
 * next compaction (tier-4 audit V10).  The remaining lazy part (interning
 * slot 0) is benign to race: cl_intern is MT-safe and both racers store
 * the same symbol.
 *
 * This must run on EVERY cl_vm_init, including a heap re-initialization
 * (each C unit test, embedded restarts): the old one-shot guard skipped
 * the second run entirely, leaving all 16 slots holding PREVIOUS-arena
 * offsets while unregistered — the documented stale-static-table
 * corruption class — and desynchronizing the boot-root count the heap
 * image format relies on (specs/image-save-load.md).  cl_gc_register_root
 * itself is idempotent by address, so re-running is safe; the reset of
 * the count/inited latches makes the lazy re-derivation start over
 * against the fresh heap. */
void cl_gf_types_boot_init(void)
{
    int i;
    for (i = 0; i < CL_MAX_GF_TYPES; i++) {
        cl_gf_type_syms[i] = CL_NIL;
        cl_gc_register_root(&cl_gf_type_syms[i]);
    }
    cl_gf_roots_registered = 1;
    cl_gf_type_count = 0;
    cl_gf_types_inited = 0;
}

static void cl_gf_types_init(void)
{
    /* Fallback for harnesses that bypass cl_vm_init — must NOT re-run the
     * full boot init here once roots are registered, or a lazy call
     * mid-session would wipe the registered GF-type set. */
    if (!cl_gf_roots_registered)
        cl_gf_types_boot_init();
    cl_gf_type_syms[0] = cl_intern("STANDARD-GENERIC-FUNCTION", 25);
    cl_gf_type_count = 1;
    cl_gf_types_inited = 1;
}

void cl_register_funcallable_gf_type(CL_Obj sym)
{
    int i;
    if (!cl_gf_types_inited) cl_gf_types_init();
    if (!CL_SYMBOL_P(sym)) return;
    for (i = 0; i < cl_gf_type_count; i++)
        if (cl_gf_type_syms[i] == sym) return;  /* already registered */
    if (cl_gf_type_count < CL_MAX_GF_TYPES)
        cl_gf_type_syms[cl_gf_type_count++] = sym;
}

int cl_funcallable_instance_p(CL_Obj obj)
{
    CL_Obj td;
    int i;
    if (!CL_STRUCT_P(obj)) return 0;
    if (!cl_gf_types_inited) cl_gf_types_init();
    td = ((CL_Struct *)CL_OBJ_TO_PTR(obj))->type_desc;
    for (i = 0; i < cl_gf_type_count; i++)
        if (cl_gf_type_syms[i] == td) return 1;
    return 0;
}

CL_Obj cl_unwrap_funcallable(CL_Obj obj)
{
    if (cl_funcallable_instance_p(obj)) {
        CL_Struct *st = (CL_Struct *)CL_OBJ_TO_PTR(obj);
        if (st->n_slots > 3) return st->slots[3];
    }
    return obj;
}

CL_Obj cl_vm_apply(CL_Obj func, CL_Obj *args, int nargs)
{
    /*
     * Apply: C builtins called directly (no VM entry),
     * bytecode/closures use a stack-local stub frame + cl_vm_run
     * (no heap allocation, no cl_vm_eval recursion).
     */
    int i;

    /* Reader-GF fast path, same probe as OP_CALL's (see cl_vm_run): a
     * one-arg call to a promoted reader GF is answered right here — no
     * unwrap to the discriminating function, no stub frame, no VM entry.
     * This is the path every JIT'd call takes (cl_jit_runtime_call and
     * cl_jit_runtime_apply both land here with the raw GF struct), so on
     * the m68k target this probe IS the reader fast path; it also serves
     * host trampolines (MAPCAR/REDUCE/SORT keys and tests, APPLY).
     * cl_gf_reader_ic_probe is non-allocating and non-erroring; a miss
     * (or unbound slot) yields CL_UNBOUND and falls through to full
     * dispatch below.  Unlike OP_TAILCALL there is no frame to unwind —
     * cl_vm_apply is call-and-return — so no tail exclusion is needed.
     * On a hit, leave the same mv state a normal 1-value return would
     * (mirrors call_builtin). */
    if (nargs == 1 && cl_funcallable_instance_p(func)) {
        CL_Obj slot_v = cl_gf_reader_ic_probe(func, args[0]);
        if (slot_v != CL_UNBOUND) {
            cl_mv_count = 1;
            cl_mv_values[0] = slot_v;
            return slot_v;
        }
    }
    /* Writer-GF fast path, ditto for (setf (x obj) v): args are (VAL OBJ)
     * per CLHS 5.1.1.2.  Covers the JIT'd writer calls the same way. */
    if (nargs == 2 && cl_funcallable_instance_p(func)) {
        CL_Obj slot_v = cl_gf_writer_ic_probe(func, args[0], args[1]);
        if (slot_v != CL_UNBOUND) {
            cl_mv_count = 1;
            cl_mv_values[0] = slot_v;
            return slot_v;
        }
    }

    /* Unwrap funcallable instances (e.g. a generic-function struct) so the
     * standard call path sees the underlying discriminating function. */
    func = cl_unwrap_funcallable(func);

    /* C-stack guard for EVERY callee kind, not just the bytecode one below.
     * cl_vm_apply is the C-level recursion point (a builtin that applies a
     * function, MAPCAR/REDUCE/SORT, the FFI callback entry), so the check
     * that used to sit in call_builtin — where it ran on every builtin
     * dispatch — belongs here, where the C stack actually grows.  The
     * accessor probes above return without recursing and stay unguarded:
     * they are the JIT's hot call path. */
    cl_check_c_stack("cl_vm_apply");

    /* C builtins: call directly, no VM entry needed.
     *
     * GC-root the caller's args by copying them onto the VM stack and
     * passing that (rooted) slice to the builtin.  Builtins assume their
     * args are GC-rooted — true on the OP_CALL path, where args already sit
     * on the VM stack — but a caller of cl_vm_apply may hand us a C-array
     * local (MAPCAR/REDUCE/SORT/APPLY collecting elements) that the GC does
     * not know about.  A builtin that allocates while reading its own args
     * (e.g. LIST consing) would then read a relocated arg stale.  Copying to
     * the stack first makes every arg a forwarded root for the call. */
    if (CL_FUNCTION_P(func)) {
        CL_Function *f = (CL_Function *)CL_OBJ_TO_PTR(func);
        int saved_sp = cl_vm.sp;
        int base = cl_vm.sp;
        CL_Obj result;
        /* Builtins take an (int nargs, args[]) ABI, so there is no 255 cap —
         * only the VM stack bounds them (call-arguments-limit). */
        if (nargs > CL_CALL_ARGS_LIMIT ||
            base + nargs >= (int)cl_vm.stack_size - 16)
            cl_error(CL_ERR_ARGS,
                     "APPLY: too many arguments (call-arguments-limit is %d)",
                     CL_CALL_ARGS_LIMIT);
        for (i = 0; i < nargs; i++)
            cl_vm_push(args[i]);
        result = call_builtin(cl_get_current_thread(), f,
                              &cl_vm.stack[base], nargs);
        cl_vm.sp = saved_sp;
        return result;
    }

    /* FFI stub (DEFCFUN / DEFCSTRUCT binding descriptor): same shape as
     * the builtin path — copy the args onto the VM stack so they are
     * rooted across the call (a peek of an :fptr field or a library call
     * boxing a pointer allocates), then dispatch on the stub's kind.  No
     * frame, no VM entry. */
    if (CL_FFI_STUB_P(func)) {
        int saved_sp = cl_vm.sp;
        int base = cl_vm.sp;
        CL_Obj result;
        if (base + nargs >= (int)cl_vm.stack_size - 16)
            cl_error(CL_ERR_OVERFLOW, "APPLY: VM stack overflow");
        for (i = 0; i < nargs; i++)
            cl_vm_push(args[i]);
        result = cl_ffi_stub_call(func, &cl_vm.stack[base], nargs);
        cl_vm.sp = saved_sp;
        cl_mv_count = 1;
        cl_mv_values[0] = result;
        return result;
    }

    /* Bytecode / closure (and anything else that is not a builtin or an FFI
     * stub -- the error for a non-function lands in OP_CALL): the stub-frame
     * path. */
    return cl_vm_call_bytecode(cl_get_current_thread(), func, args, nargs, 0);
}

/* The stub-frame call: push a minimal frame whose code is OP_CALL n; OP_HALT,
 * then func and the arguments, and run the dispatch loop until the HALT.
 * The stub code lives in the frame itself (heap-allocated with the frame
 * array), so it survives a longjmp past this C frame.  ARGS holds the
 * arguments in call order, or -- REVERSED non-zero -- args[i] is argument
 * nargs-1-i, which is how the m68k JIT's operand stack lays them out
 * (jit_dispatch, src/jit/runtime_m68k.c, enters here for an interpreted callee
 * without copying them first).  The one-byte OP_CALL operand caps this path
 * at 255 arguments; cl_vm_apply_list runs the inline OP_APPLY for more.
 * The C-stack guard is cl_vm_run's own, on entry, before any bytecode runs. */
CL_Obj cl_vm_call_bytecode(CL_Thread *thr, CL_Obj func, const CL_Obj *args,
                           int nargs, int reversed)
{
    CL_Frame *frame;
    int base_fp, base_nlx, saved_sp, i;
    CL_Obj result;

    if (nargs > 255)
        cl_error(CL_ERR_ARGS,
                 "APPLY: too many arguments (got %d; this call path is "
                 "limited to 255)", nargs);
    if (thr->vm.fp >= (int)thr->vm.frame_size)
        cl_error(CL_ERR_OVERFLOW, "VM frame stack overflow");
    if (thr->vm.sp + nargs + 1 >= (int)thr->vm.stack_size)
        cl_error(CL_ERR_OVERFLOW, "VM stack overflow");

    base_fp = thr->vm.fp;
    saved_sp = thr->vm.sp;
    base_nlx = thr->nlx_top;

    /* The stub frame goes in BEFORE func+args: bp = sp before them,
     * n_locals = 0.  After OP_CALL consumes func+args and pushes the
     * result, sp = bp+1 so OP_HALT sees the result. */
    frame = &thr->vm.frames[thr->vm.fp++];
    frame->stub_code[0] = OP_CALL;
    frame->stub_code[1] = (uint8_t)nargs;
    frame->stub_code[2] = OP_HALT;
    frame->bytecode = CL_NIL;
    frame->code = frame->stub_code;
    frame->constants = NULL;
    frame->ip = 0;
    frame->bp = thr->vm.sp;
    frame->n_locals = 0;
    frame->nargs = 0;
    frame->nlx_level = thr->nlx_top;
    frame->fslot = 0;

    thr->vm.stack[thr->vm.sp++] = func;
    if (reversed) {
        for (i = 0; i < nargs; i++)
            thr->vm.stack[thr->vm.sp++] = args[nargs - 1 - i];
    } else {
        for (i = 0; i < nargs; i++)
            thr->vm.stack[thr->vm.sp++] = args[i];
    }

    result = cl_vm_run(base_fp, base_nlx);

    /* OP_HALT does not pop the stub frame; without this restore each call
     * would leave fp one higher and the caller's OP_RET would restore the
     * wrong frame. */
    thr->vm.fp = base_fp;
    thr->vm.sp = saved_sp;
    return result;
}

/* (apply FUNC ARGLIST) from C, for any argument count up to
 * CALL-ARGUMENTS-LIMIT.  cl_vm_apply's stub frame is an OP_CALL whose
 * nargs operand is one byte, so it refuses more than 255 arguments; this
 * variant runs the inline OP_APPLY instead, which spreads the list onto
 * the GC-rooted VM stack with a 16-bit count and handles &rest callees,
 * builtins and FFI stubs alike.  Used by the DEFMACRO expander trampoline
 * (%CALL-MACRO-EXPANDER) for macro forms with hundreds of arguments — a
 * generated binding module is one such form. */
CL_Obj cl_vm_apply_list(CL_Obj func, CL_Obj arglist)
{
    CL_Frame *frame;
    int base_fp, base_nlx, saved_sp;
    CL_Obj result;

    cl_check_c_stack("cl_vm_apply_list");
    base_fp = cl_vm.fp;
    saved_sp = cl_vm.sp;
    base_nlx = cl_nlx_top;
    if (cl_vm.fp >= cl_vm.frame_size)
        cl_error(CL_ERR_OVERFLOW, "VM frame stack overflow");
    if (cl_vm.sp + 2 >= (int)cl_vm.stack_size - 16)
        cl_error(CL_ERR_OVERFLOW, "APPLY: VM stack overflow");

    frame = &cl_vm.frames[cl_vm.fp++];
    frame->stub_code[0] = OP_APPLY;
    frame->stub_code[1] = OP_HALT;
    frame->stub_code[2] = OP_HALT;
    frame->bytecode = CL_NIL;
    frame->code = frame->stub_code;
    frame->constants = NULL;
    frame->ip = 0;
    frame->bp = cl_vm.sp;
    frame->n_locals = 0;
    frame->nargs = 0;
    frame->nlx_level = cl_nlx_top;
    frame->fslot = 0;

    cl_vm_push(func);
    cl_vm_push(arglist);
    result = cl_vm_run(base_fp, base_nlx);
    cl_vm.fp = base_fp;
    cl_vm.sp = saved_sp;
    return result;
}

static uint16_t read_u16(uint8_t *code, uint32_t *ip)
{
    uint16_t val = (code[*ip] << 8) | code[*ip + 1];
    *ip += 2;
    return val;
}

static int16_t read_i16(uint8_t *code, uint32_t *ip)
{
    return (int16_t)read_u16(code, ip);
}

static int32_t read_i32(uint8_t *code, uint32_t *ip)
{
    int32_t val = (int32_t)(((uint32_t)code[*ip] << 24) |
                            ((uint32_t)code[*ip + 1] << 16) |
                            ((uint32_t)code[*ip + 2] << 8) |
                            (uint32_t)code[*ip + 3]);
    *ip += 4;
    return val;
}

/* --- Trace helpers --- */

static CL_Obj get_func_name(CL_Obj func_obj)
{
    if (CL_FUNCTION_P(func_obj)) {
        return ((CL_Function *)CL_OBJ_TO_PTR(func_obj))->name;
    } else if (CL_CLOSURE_P(func_obj)) {
        CL_Closure *cl = (CL_Closure *)CL_OBJ_TO_PTR(func_obj);
        return ((CL_Bytecode *)CL_OBJ_TO_PTR(cl->bytecode))->name;
    } else if (CL_BYTECODE_P(func_obj)) {
        return ((CL_Bytecode *)CL_OBJ_TO_PTR(func_obj))->name;
    } else if (CL_FFI_STUB_P(func_obj)) {
        return ((CL_FfiStub *)CL_OBJ_TO_PTR(func_obj))->name;
    }
    return CL_NIL;
}

static int is_func_traced(CL_Obj func_obj)
{
    CL_Obj name;
    if (cl_trace_count <= 0) return 0;
    name = get_func_name(func_obj);
    if (!CL_NULL_P(name) && CL_SYMBOL_P(name)) {
        return (((CL_Symbol *)CL_OBJ_TO_PTR(name))->flags & CL_SYM_TRACED) != 0;
    }
    return 0;
}

/* GC SAFETY (both trace printers): the trace-stream writes and
 * cl_prin1_to_string can allocate (stream buffering, printer state) and
 * compact — the by-value parameters are C locals of this frame, so root
 * them for the duration.  `args` points at rooted VM-stack slots, which are
 * forwarded in place, so re-reading args[i] each iteration is safe. */
static void trace_print_entry(CL_Obj name_sym, CL_Obj *args, int nargs)
{
    char buf[128];
    int i;
    CL_GC_PROTECT(name_sym);
    for (i = 0; i < cl_trace_depth; i++)
        cl_write_cstring_to_trace("  ");
    snprintf(buf, sizeof(buf), "%d: (", cl_trace_depth);
    cl_write_cstring_to_trace(buf);
    cl_write_cstring_to_trace(cl_symbol_name(name_sym));
    for (i = 0; i < nargs; i++) {
        cl_write_cstring_to_trace(" ");
        cl_prin1_to_string(args[i], buf, sizeof(buf));
        cl_write_cstring_to_trace(buf);
    }
    cl_write_cstring_to_trace(")\n");
    CL_GC_UNPROTECT(1);
}

static void trace_print_exit(CL_Obj name_sym, CL_Obj result)
{
    char buf[128];
    int i;
    CL_GC_PROTECT(name_sym);
    CL_GC_PROTECT(result);
    for (i = 0; i < cl_trace_depth; i++)
        cl_write_cstring_to_trace("  ");
    snprintf(buf, sizeof(buf), "%d: ", cl_trace_depth);
    cl_write_cstring_to_trace(buf);
    cl_write_cstring_to_trace(cl_symbol_name(name_sym));
    cl_write_cstring_to_trace(" returned ");
    cl_prin1_to_string(result, buf, sizeof(buf));
    cl_write_cstring_to_trace(buf);
    cl_write_cstring_to_trace("\n");
    CL_GC_UNPROTECT(2);
}

/* --- Backtrace capture --- */

/* Look up the source line for a frame's saved IP in a bytecode's line map.
 * Returns 0 if no mapping found.  Entries are sorted by pc.
 *
 * A frame's ip is a RESUME point: it sits just past the instruction that
 * was executing when the frame was saved (an OP_CALL into a callee, a
 * builtin or FFI call that signalled, a Ctrl-C poll).  The line of that
 * instruction is wanted, so the lookup is done at ip - 1, the last byte of
 * the instruction.  At ip itself a call that is followed directly by the
 * next form's code -- `(list (f)\n (g))` -- would land on the entry the
 * compiler recorded for `(g)`, one line too far.  Entries only ever start
 * at instruction boundaries, so ip - 1 can never cross into a later one. */
static int lookup_source_line(CL_Bytecode *bc, uint32_t ip)
{
    int i, best_line = 0;
    if (!bc->line_map || bc->line_map_count == 0) return 0;
    if (ip > 0) ip--;
    for (i = 0; i < bc->line_map_count; i++) {
        if (bc->line_map[i].pc <= ip)
            best_line = bc->line_map[i].line;
        else
            break;
    }
    return best_line;
}

static CL_Bytecode *get_frame_bytecode(CL_Frame *f)
{
    CL_Obj bc_obj = f->bytecode;
    if (CL_BYTECODE_P(bc_obj))
        return (CL_Bytecode *)CL_OBJ_TO_PTR(bc_obj);
    if (CL_CLOSURE_P(bc_obj)) {
        CL_Closure *cl = (CL_Closure *)CL_OBJ_TO_PTR(bc_obj);
        if (CL_BYTECODE_P(cl->bytecode))
            return (CL_Bytecode *)CL_OBJ_TO_PTR(cl->bytecode);
    }
    return NULL;
}

/* "NAME (file:line)" of the function a frame is executing, at ip, for the
 * error messages the VM raises at a call site (arity, not a function):
 * the CALLEE is in the message already; this names the CALLER, so a
 * message read from a log on the Amiga -- where there is no debugger to
 * ask for a backtrace -- points at the calling form.  "?" for a stub or
 * a frame whose bytecode is not readable. */
static const char *frame_site_brief(CL_Frame *f, uint32_t ip, char *buf, int n)
{
    CL_Bytecode *bc;
    bt_sync_native_ips();
    /* A stub frame (cl_vm_apply, cl_vm_call_bytecode) is not the caller:
     * the frame under it made the call -- a native function's, its ip at
     * the call (jit_a64.c), or an interpreted one that called a builtin. */
    if (f && f->code == f->stub_code && f > &cl_vm.frames[0]) {
        f = f - 1;
        ip = f->ip;
    }
    bc = f ? get_frame_bytecode(f) : NULL;
    if (!bc) { snprintf(buf, n, "?"); return buf; }
    snprintf(buf, n, "%s (%s:%d)",
             (!CL_NULL_P(bc->name) && CL_SYMBOL_P(bc->name))
                 ? cl_symbol_name(bc->name) : "<lambda>",
             bc->source_file ? bc->source_file : "?",
             lookup_source_line(bc, ip));
    return buf;
}

/* The two call-site errors of the VM, formatted OUTSIDE cl_vm_run: their
 * buffers would otherwise sit in the hot function's frame, and the giant
 * cl_vm_run is sensitive to anything added to it (CLAUDE.md).  Both
 * messages carry the caller and the arguments as received: an arity
 * error or a non-function callee is the first symptom of a stack that is
 * off by one (the leading argument gone, the callee itself among the
 * arguments), and only the values tell that apart from a wrong call.
 * The callee is described by cl_obj_brief, so a corrupt word prints as
 * what it is; the "heap object type N" spelling stays for the tests that
 * grep it. */
static CL_NOINLINE void vm_arity_error(CL_Bytecode *callee_bc, CL_Frame *frame,
                                       uint32_t ip, CL_Obj *args, int nargs,
                                       int expected)
{
    char site[160], argv[256];
    CL_Obj fname = callee_bc->name;
    int few = nargs < expected;
    int n_opt = callee_bc->n_optional;
    int loose = n_opt || (callee_bc->arity & 0x8000) || (callee_bc->flags & 1);
    cl_error(CL_ERR_ARGS,
             "Too %s arguments to %s (%s:%u): expected %s%d, got %d, "
             "called from %s with (%s)",
             few ? "few" : "many",
             (!CL_NULL_P(fname) && CL_SYMBOL_P(fname)) ? cl_symbol_name(fname) : "<lambda>",
             callee_bc->source_file ? callee_bc->source_file : "?",
             (unsigned)callee_bc->source_line,
             few ? (loose ? "at least " : "") : (n_opt ? "at most " : ""),
             expected, nargs,
             frame_site_brief(frame, ip, site, sizeof(site)),
             cl_args_brief(args, nargs, argv, sizeof(argv)));
}

static CL_NOINLINE void vm_not_a_function_error(CL_Obj func_obj, CL_Frame *frame,
                                                uint32_t ip, CL_Obj *args, int nargs)
{
    char val[96], site[160], argv[256];
    const char *what;
    if (CL_NULL_P(func_obj)) {
        what = "NIL";
    } else if (CL_SYMBOL_P(func_obj)) {
        snprintf(val, sizeof(val), "symbol %s", cl_symbol_name(func_obj));
        what = val;
    } else if (CL_HEAP_P(func_obj) && func_obj < cl_heap.arena_size) {
        char one[64];
        snprintf(val, sizeof(val), "heap object type %u %s",
                 (unsigned)CL_HDR_TYPE(CL_OBJ_TO_PTR(func_obj)),
                 cl_obj_brief(func_obj, one, sizeof(one)));
        what = val;
    } else {
        /* a fixnum / character prints as itself; a non-object word as
         * "#<raw ...>" / "#<out of arena ...>" */
        what = cl_obj_brief(func_obj, val, sizeof(val));
    }
    cl_error(CL_ERR_TYPE, "Not a function: %s, called from %s with (%s)", what,
             frame_site_brief(frame, ip, site, sizeof(site)),
             cl_args_brief(args, nargs, argv, sizeof(argv)));
}

/* A cl_vm_apply stub frame carries no user function — its code points at
 * the frame's own embedded stub_code (the OP_CALL/OP_HALT trampoline set up
 * in cl_vm_apply).  JIT'd calls route through cl_vm_apply, so these stubs
 * sit between the JIT shadow frames and would otherwise pollute the
 * backtrace with anonymous entries; the backtrace readers skip them.  On
 * the bytecode-only host build there are no JIT shadow frames and ordinary
 * calls push real frames, so nothing is skipped there. */
static int frame_is_stub(CL_Frame *f)
{
    return f->code == f->stub_code;
}

/* Count the user-visible (non-stub) frames in [0, base). */
static int bt_count_real(int base)
{
    int i, n = 0;
    for (i = 0; i < base; i++)
        if (!frame_is_stub(&cl_vm.frames[i]))
            n++;
    return n;
}

int cl_backtrace_cap_limit = CL_BACKTRACE_BUF_MAX;

/* Every CL_Thread gets cl_thread_backtrace_init, so the buffer is already
 * valid here.  Re-establishing it is a cheap belt-and-braces guard: a NULL
 * buffer would turn the error path — the one place we most need a diagnostic
 * — into a second crash. */
static void bt_ensure_buf(void)
{
    if (!cl_backtrace_buf || cl_backtrace_cap <= 0)
        cl_thread_backtrace_init(CT);
}

/* Grow the current thread's backtrace buffer to hold at least NEED bytes
 * (terminating NUL included), moving it from the inline block to the C heap on
 * first growth.  Returns 1 when the capacity is available, 0 when it is not —
 * at the cl_backtrace_cap_limit ceiling or out of memory.  A 0 return leaves
 * the existing text intact and callers simply stop appending, so the worst
 * case is a truncated backtrace, never an overrun. */
static int bt_reserve(int need)
{
    int cap = cl_backtrace_cap;
    int limit = cl_backtrace_cap_limit;
    char *nb;

    if (need <= cap) return 1;
    if (limit < CL_BACKTRACE_BUF_SIZE) limit = CL_BACKTRACE_BUF_SIZE;
    if (need > limit) return 0;

    if (cap < CL_BACKTRACE_BUF_SIZE) cap = CL_BACKTRACE_BUF_SIZE;
    while (cap < need) cap *= 2;      /* bounded by limit, cannot overflow */
    if (cap > limit) cap = limit;

    nb = (char *)platform_alloc((unsigned long)cap);
    if (!nb) return 0;
    memcpy(nb, cl_backtrace_buf, (size_t)cl_backtrace_cap);
    if (cl_backtrace_buf != CT->backtrace_inline)
        platform_free(cl_backtrace_buf);
    cl_backtrace_buf = nb;
    cl_backtrace_cap = cap;
    return 1;
}

/* Append printf-formatted text at POS, growing the buffer to fit, and return
 * the new position (always the index of the terminating NUL).
 *
 * vsnprintf reports the length it WOULD have written, which is both the grow
 * signal and the historical hazard here: a plain `pos += vsnprintf(...)` walks
 * pos past the end, the next call computes a NEGATIVE remaining size that
 * wraps to a huge size_t, and the write runs unbounded past the buffer.
 * Deriving pos from what actually fits — never from the requested length —
 * keeps pos inside [0, cap-1] on every path, including the one where growth
 * is refused. */
static int bt_appendf(int pos, const char *fmt, ...)
{
    va_list ap;
    int n, avail;

    if (pos < 0) return 0;
    if (pos > cl_backtrace_cap - 1) pos = cl_backtrace_cap - 1;

    avail = cl_backtrace_cap - pos;      /* >= 1: room for at least the NUL */
    va_start(ap, fmt);
    n = vsnprintf(cl_backtrace_buf + pos, (size_t)avail, fmt, ap);
    va_end(ap);
    if (n < 0) return pos;               /* encoding error: leave pos put */
    if (n < avail) return pos + n;       /* fit whole */

    /* Did not fit.  Grow to exactly what this append needs and re-render. */
    if (!bt_reserve(pos + n + 1))
        return pos + avail - 1;          /* capped: keep the truncated text */

    avail = cl_backtrace_cap - pos;
    va_start(ap, fmt);
    n = vsnprintf(cl_backtrace_buf + pos, (size_t)avail, fmt, ap);
    va_end(ap);
    if (n < 0) return pos;
    if (n >= avail) n = avail - 1;
    return pos + n;
}

/* Format a single backtrace frame into buf+pos, return new pos */
static int bt_format_frame(int pos, int depth, CL_Obj name,
                           const char *file, int line)
{
    const char *nm = (!CL_NULL_P(name) && CL_SYMBOL_P(name))
                     ? cl_symbol_name(name) : "<anonymous>";
    if (file && line > 0)
        return bt_appendf(pos, "  %d: %s (%s:%d)\n", depth, nm, file, line);
    if (line > 0)
        return bt_appendf(pos, "  %d: %s (line %d)\n", depth, nm, line);
    return bt_appendf(pos, "  %d: %s\n", depth, nm);
}

/* Detect repeating frame pattern starting at frame index i (descending).
 * Returns the cycle length (2..8) if found, 0 otherwise.
 * A cycle is detected when the same sequence of (name, source_line) repeats
 * at least 3 times consecutively. */
static int bt_detect_cycle(int start, int *repeat_count)
{
    int total = start + 1;  /* number of frames available */
    int cycle_len;

    for (cycle_len = 2; cycle_len <= 8 && cycle_len * 3 <= total; cycle_len++) {
        int repeats = 1;
        int k;
        /* Check if the pattern at [start..start-cycle_len+1] repeats */
        for (k = 1; k * cycle_len <= total - cycle_len; k++) {
            int j, match = 1;
            for (j = 0; j < cycle_len; j++) {
                CL_Frame *f1 = &cl_vm.frames[start - j];
                CL_Frame *f2 = &cl_vm.frames[start - j - k * cycle_len];
                CL_Obj n1 = get_func_name(f1->bytecode);
                CL_Obj n2 = get_func_name(f2->bytecode);
                CL_Bytecode *bc1 = get_frame_bytecode(f1);
                CL_Bytecode *bc2 = get_frame_bytecode(f2);
                int line1 = bc1 ? lookup_source_line(bc1, f1->ip) : 0;
                int line2 = bc2 ? lookup_source_line(bc2, f2->ip) : 0;
                if (n1 != n2 || line1 != line2) { match = 0; break; }
            }
            if (match) repeats++; else break;
        }
        if (repeats >= 3) {
            *repeat_count = repeats;
            return cycle_len;
        }
    }
    return 0;
}

/* Render frames [0, BASE) into the backtrace buffer, innermost first, showing
 * at most MAX_SHOW of them; MAX_SHOW <= 0 means every frame.  The buffer grows
 * to fit whatever this produces (see bt_appendf), so the frame limit is the
 * only thing that decides how much the user sees. */
static void bt_render(int base, int max_show)
{
    int i, pos = 0;
    int depth = 0;
    int limit;

    bt_ensure_buf();
    cl_backtrace_buf[0] = '\0';
    if (base <= 0) return;
    bt_sync_native_ips();

    /* BASE frames is the most that can ever be shown, so it doubles as the
     * "no limit" sentinel — and keeps limit+5 below from overflowing. */
    limit = (max_show > 0 && max_show < base) ? max_show : base;

    /* Check for repeating patterns (likely infinite recursion).
     * Try starting from each of the first few frames to find the cycle. */
    for (i = base - 1; i >= base - 5 && i >= 0; i--) {
        int repeat_count = 0;
        int cycle_len = bt_detect_cycle(i, &repeat_count);
        if (cycle_len > 0 && repeat_count >= 5) {
            int j;
            int prefix_frames = base - 1 - i;
            int skip_to;
            /* Show prefix frames (before the cycle starts) */
            for (j = 0; j < prefix_frames; j++) {
                CL_Frame *f = &cl_vm.frames[base - 1 - j];
                CL_Obj nm = get_func_name(f->bytecode);
                CL_Bytecode *bc = get_frame_bytecode(f);
                int line = bc ? lookup_source_line(bc, f->ip) : 0;
                const char *file = bc ? bc->source_file : NULL;
                pos = bt_format_frame(pos, j, nm, file, line);
            }
            depth = prefix_frames;
            /* Show one cycle */
            for (j = 0; j < cycle_len; j++, depth++) {
                CL_Frame *f = &cl_vm.frames[i - j];
                CL_Obj nm = get_func_name(f->bytecode);
                CL_Bytecode *bc = get_frame_bytecode(f);
                int line = bc ? lookup_source_line(bc, f->ip) : 0;
                const char *file = bc ? bc->source_file : NULL;
                pos = bt_format_frame(pos, depth, nm, file, line);
            }
            pos = bt_appendf(pos,
                             "  --- above %d frames repeat %d times (%d frames total) ---\n",
                             cycle_len, repeat_count, cycle_len * repeat_count);
            /* Skip past the repeated frames, show remaining */
            skip_to = i - cycle_len * repeat_count;
            for (j = skip_to; j >= 0 && depth < limit + 5; j--, depth++) {
                CL_Frame *f = &cl_vm.frames[j];
                CL_Obj nm = get_func_name(f->bytecode);
                CL_Bytecode *bc = get_frame_bytecode(f);
                int line = bc ? lookup_source_line(bc, f->ip) : 0;
                const char *file = bc ? bc->source_file : NULL;
                pos = bt_format_frame(pos, depth, nm, file, line);
            }
            /* This loop prints frames [0, j] raw (no frame_is_stub filter,
             * unlike the normal path below) — so the "more frames" count
             * must stay on the same raw basis, not bt_count_real's filtered
             * one, or the two numbers in this message describe different
             * populations. */
            if (j >= 0)
                bt_appendf(pos, "  ... %d more frames\n", j + 1);
            return;
        }
    }

    /* Normal backtrace (no detected cycle) */
    for (i = base - 1; i >= 0 && depth < limit; i--) {
        CL_Frame *f = &cl_vm.frames[i];
        CL_Obj name;
        CL_Bytecode *bc;
        int line;
        const char *file;
        if (frame_is_stub(f)) continue;
        name = get_func_name(f->bytecode);
        bc = get_frame_bytecode(f);
        line = bc ? lookup_source_line(bc, f->ip) : 0;
        file = bc ? bc->source_file : NULL;
        pos = bt_format_frame(pos, depth, name, file, line);
        depth++;
    }

    /* Report what was left out, counting only frames the loop would actually
     * have printed.  The old `cl_vm.fp - max_show` counted raw frames, which
     * over-reported on the JIT path where cl_vm_apply stub frames are
     * interleaved but never shown. */
    if (i >= 0) {
        int remaining = bt_count_real(i + 1);
        if (remaining > 0)
            bt_appendf(pos, "  ... %d more frames\n", remaining);
    }
}

void cl_capture_backtrace(void)
{
    /* Snapshot the macroexpansion error context AT RAISE TIME: this is the
     * bottleneck every raise path funnels through (cl_error,
     * cl_raise_condition, the VM/builtin type-error helpers).  Both cells
     * are per-thread TLV values (see error.h) — this reads/clears only the
     * CURRENT thread's, so it can never race a peer thread's expansion.
     * cl_expanding_form_sym is live only while an expander runs, so
     * capturing it here can never attribute a later, unrelated error to an
     * expansion that already finished.  Clearing the live slot afterwards
     * matters too: the raise longjmps past cl_macroexpand_1_env's restore,
     * and a caught-and-recovered expansion error must not leave a stale
     * form behind to mislabel the next error raised outside any
     * expansion. */
    cl_tlv_set(CT, cl_error_expanding_form_sym,
               cl_expansion_ctx_get(cl_expanding_form_sym));
    cl_tlv_set(CT, cl_expanding_form_sym, CL_NIL);

    /* Snapshot the frame depth so a debugger-hook (SLDB) can introspect the
     * error-time backtrace via cl_vm_backtrace_list / cl_vm_frame_locals even
     * after the hook has pushed its own frames on top. */
    cl_debug_base_fp = cl_vm.fp;

    bt_render(cl_vm.fp, CL_BACKTRACE_DEFAULT_FRAMES);
}

/* --- Structured backtrace introspection (Sly/SLYNK SLDB backend) --- */

/* Resolve the number of frames to expose.  Inside the debugger this is the
 * depth captured at error time (cl_debug_base_fp); the live fp is higher
 * because the hook pushed its own frames.  Outside an error (stale or unset
 * snapshot) we fall back to the live frame pointer so an ad-hoc
 * (ext:backtrace) still reports the current stack. */
static int bt_resolve_base(void)
{
    int base = cl_debug_base_fp;
    if (base <= 0 || base > cl_vm.fp)
        base = cl_vm.fp;
    return base;
}

void cl_render_backtrace(int max_frames)
{
    bt_render(bt_resolve_base(), max_frames);
}

/* Map a logical backtrace index (0 = innermost non-stub frame) to a physical
 * cl_vm.frames index, or -1 when out of range.  Lets the structured readers
 * present a contiguous, stub-free frame numbering. */
static int bt_physical_frame(int base, int logical)
{
    int i, seen = 0;
    for (i = base - 1; i >= 0; i--) {
        if (frame_is_stub(&cl_vm.frames[i]))
            continue;
        if (seen == logical)
            return i;
        seen++;
    }
    return -1;
}

/* Build an uninterned placeholder symbol named ARG<idx> or LOCAL<idx>. */
static CL_Obj bt_slot_placeholder(int idx, int is_arg)
{
    char buf[24];
    int len = snprintf(buf, sizeof(buf), is_arg ? "ARG%d" : "LOCAL%d", idx);
    CL_Obj str = cl_make_string(buf, (uint32_t)len);
    CL_Obj sym;
    CL_GC_PROTECT(str);
    sym = cl_make_uninterned_symbol(str);
    CL_GC_UNPROTECT(1);
    return sym;
}

CL_Obj cl_vm_backtrace_list(int max_frames)
{
    int base = bt_resolve_base();
    int count = bt_count_real(base);
    int li;
    CL_Obj result = CL_NIL, entry = CL_NIL, name = CL_NIL, file_str = CL_NIL;

    if (max_frames > 0 && count > max_frames)
        count = max_frames;
    if (count <= 0)
        return CL_NIL;
    bt_sync_native_ips();

    /* result/entry/name/file_str all hold heap refs across cl_cons /
     * cl_make_string allocations below — protect all four. */
    CL_GC_PROTECT(result);
    CL_GC_PROTECT(entry);
    CL_GC_PROTECT(name);
    CL_GC_PROTECT(file_str);

    /* Walk logical indices outermost→innermost and prepend, so the final
     * list is innermost-first with index 0 = innermost non-stub frame.
     * Stub frames (cl_vm_apply trampolines, interleaved with JIT shadow
     * frames) are skipped via the logical→physical mapping. */
    for (li = count - 1; li >= 0; li--) {
        int phys;
        CL_Frame *f;
        CL_Bytecode *bc;
        int line;
        const char *file;
        CL_Obj line_obj;

        phys = bt_physical_frame(base, li);
        if (phys < 0) continue;
        f = &cl_vm.frames[phys];

        /* Read everything off the frame before allocating. */
        name = get_func_name(f->bytecode);
        bc = get_frame_bytecode(f);
        line = bc ? lookup_source_line(bc, f->ip) : 0;
        file = (bc && bc->source_file) ? bc->source_file : NULL;
        line_obj = (line > 0) ? CL_MAKE_FIXNUM(line) : CL_NIL;
        file_str = file ? cl_make_string(file, (uint32_t)strlen(file)) : CL_NIL;

        /* entry = (index name file line) */
        entry = cl_cons(line_obj, CL_NIL);
        entry = cl_cons(file_str, entry);
        entry = cl_cons(name, entry);
        entry = cl_cons(CL_MAKE_FIXNUM(li), entry);
        result = cl_cons(entry, result);
    }

    CL_GC_UNPROTECT(4);
    return result;
}

CL_Obj cl_vm_frame_locals(int index)
{
    int base = bt_resolve_base();
    int phys = bt_physical_frame(base, index);
    CL_Frame *f;
    int bp, n_locals, nargs, i;
    CL_Obj result = CL_NIL, pair = CL_NIL, nm = CL_NIL, val = CL_NIL;

    if (index < 0 || phys < 0)
        return cl_intern_in("NOT-AVAILABLE", 13, cl_package_keyword);

    f = &cl_vm.frames[phys];
    bp = (int)f->bp;
    n_locals = f->n_locals;
    nargs = f->nargs;

    CL_GC_PROTECT(result);
    CL_GC_PROTECT(pair);
    CL_GC_PROTECT(nm);
    CL_GC_PROTECT(val);

    /* Prepend slots in reverse so the list reads slot 0 first.  The slot's
     * value is read AFTER bt_slot_placeholder (which allocates) so it stays
     * consistent with any GC the allocation triggered; the VM value stack is
     * GC-rooted, so the slot itself is always a valid object (NIL-filled at
     * frame entry — never garbage). */
    for (i = n_locals - 1; i >= 0; i--) {
        nm = bt_slot_placeholder(i, i < nargs);
        val = cl_vm.stack[bp + i];
        pair = cl_cons(nm, val);
        result = cl_cons(pair, result);
    }

    CL_GC_UNPROTECT(4);
    return result;
}

/* Check if an NLX frame is stale (its target frame was reused by a different function).
 * This happens when a tail call or return reuses a frame after an NLX frame was established. */
static int nlx_frame_is_stale(CL_NLXFrame *nlx)
{
    CL_Frame *target;
#ifdef JIT_M68K
    /* A JIT-owned frame pushed with no VM frame below it (vm_fp 0) has no
     * target frame to have been reused: frames[-1] is not a frame.  Same
     * rule as the JIT's own jit_nlx_frame_is_stale.  Only a JIT build can
     * push such a frame (a VM-pushed one always sits in an activation), so
     * the host VM stays byte-for-byte what it was -- cl_vm_run is layout-
     * sensitive (see the Makefile note on vm.o). */
    if (nlx->vm_fp <= 0)
        return 0;
#endif
    target = &cl_vm.frames[nlx->vm_fp - 1];
    return target->code != nlx->code;
}

/* Transfer to NLX frame F — see vm.h.  A VM-pushed frame lands in its
 * cl_vm_run activation's shared landing; a C- or JIT-owned frame in its
 * own buf.  The caller has set cl_nlx_top and the frame's result / the
 * pending state. */
void cl_nlx_jump(CL_NLXFrame *f)
{
    if (f->landing)
        CL_LONGJMP(*f->landing, 1);
    CL_LONGJMP(f->buf, 1);
}

/* HANDLER-CASE clause transfer — see vm.h.  Called from cl_signal_condition
 * when the matching binding is a clause binding (fixnum handler).  Same
 * protocol as a THROW: an interposing (non-stale) UNWIND-PROTECT frame
 * above the target runs first, with the transfer parked as pending kind 3
 * — target frame index in cl_pending_tag as a fixnum, condition in
 * cl_pending_value — for OP_UWRETHROW / the JIT's uwprot_rethrow to
 * re-initiate; otherwise the frame lands directly. */
void cl_handler_case_transfer(int binding_idx, CL_Obj condition)
{
    int ni = cl_handler_stack[binding_idx].nlx_index;
    int k = (int)CL_FIXNUM_VAL(cl_handler_stack[binding_idx].handler);
    CL_NLXFrame *f;
    int j;

    if (ni < 0 || ni >= cl_nlx_top ||
        cl_nlx_stack[ni].type != CL_NLX_HANDLER_CASE)
        cl_error(CL_ERR_GENERAL,
                 "HANDLER-CASE: clause binding without its frame (nlx %d)", ni);
    f = &cl_nlx_stack[ni];
    f->hc_clause = k;
    for (j = cl_nlx_top - 1; j > ni; j--) {
        if (cl_nlx_stack[j].type == CL_NLX_UWPROT &&
            !nlx_frame_is_stale(&cl_nlx_stack[j])) {
            cl_pending_throw = 3;
            cl_pending_tag = CL_MAKE_FIXNUM(ni);
            cl_pending_value = condition;
            cl_pending_mv_count = 1;
            cl_pending_mv_values[0] = condition;
            cl_nlx_top = j;
            cl_nlx_jump(&cl_nlx_stack[j]);
        }
    }
    f->result = condition;
    f->mv_count = 1;
    f->mv_values[0] = condition;
    cl_nlx_top = ni;
    cl_nlx_jump(f);
}

/* Cheap heuristic: does fptr look like a real native code pointer rather than
 * a clobbered func slot?  Returns 1 if plausible, 0 if it should be rejected
 * as corrupt.  Kept as a separate, non-static function so it can be unit
 * tested directly (see tests/test_vm.c builtin_fptr_plausible).
 *
 * - All platforms: reject NULL and tiny addresses (< 0x1000) — a clobbered
 *   slot is typically a small tagged CL_Obj, not a code address.
 * - Amiga/m68k: instructions are 2-byte aligned, so a valid code pointer is
 *   always even; an odd value means the slot was clobbered.
 * - Host (x86-64/ARM64): the ABI does NOT guarantee any function alignment.
 *   With -O1 and the default -falign-functions, gcc routinely places
 *   functions on odd/2-byte boundaries (e.g. bi_inlined_macro_stub at
 *   0x...593), so an alignment test here produces false "corrupt func ptr"
 *   aborts that depend on link layout.  Only the low-address guard applies. */
int cl_vm_builtin_fptr_plausible(const void *fptr)
{
    uintptr_t fp_val = (uintptr_t)fptr;
    if (fp_val < 0x1000) return 0;
#ifdef PLATFORM_AMIGA
    if ((fp_val & 0x1) != 0) return 0;
#endif
    return 1;
}

/* Validate builtin call arguments — called before the actual fptr dispatch.
 * Separated from call_builtin to keep call_builtin's stack frame tiny
 * (minimizes surface area for stack corruption).
 *
 * Deliberately NO cl_check_c_stack here.  A builtin call does not by itself
 * grow the C stack — Lisp-to-Lisp calls stay inside one cl_vm_run
 * activation — so every path that actually recurses in C passes through a
 * guarded entry point first: cl_vm_apply / cl_vm_apply_list / cl_vm_eval /
 * cl_vm_run, and cl_check_recursion_guards in the reader and compiler.
 * Checking here too cost a platform_stack_headroom call on every builtin
 * dispatch (~0.5% of the sento profile) for a guard those entry points
 * already provide — verified by re-running the three overflow shapes
 * (nested source, runaway Lisp recursion, recursion through MAPCAR) and
 * getting byte-identical diagnostics. */
static void validate_builtin(CL_Function *func, int nargs)
{
    CL_CFunc fptr;
    if (nargs < func->min_args) {
        cl_error(CL_ERR_ARGS, "%s: too few arguments (got %d, need %d)",
                 CL_NULL_P(func->name) ? "?" : cl_symbol_name(func->name),
                 nargs, func->min_args);
    }
    if (func->max_args >= 0 && nargs > func->max_args) {
        cl_error(CL_ERR_ARGS, "%s: too many arguments (got %d, max %d)",
                 CL_NULL_P(func->name) ? "?" : cl_symbol_name(func->name),
                 nargs, func->max_args);
    }
    fptr = func->func;
    if (!fptr) {
        cl_error(CL_ERR_TYPE, "NULL C function pointer in %s",
                 CL_NULL_P(func->name) ? "?" : cl_symbol_name(func->name));
    }
    if (!cl_vm_builtin_fptr_plausible((const void *)fptr)) {
        fprintf(stderr, "[VM] call_builtin: CORRUPT func ptr %p in %s (obj=0x%08x)\n",
                (void *)fptr,
                CL_NULL_P(func->name) ? "?" : cl_symbol_name(func->name),
                (unsigned)CL_PTR_TO_OBJ(func));
        cl_capture_backtrace();
        fprintf(stderr, "%s", cl_backtrace_buf);
        cl_error(CL_ERR_GENERAL, "Corrupted C function pointer");
    }
}

/* Call a built-in C function.
 * MUST NOT be inlined: fptr calls may use setjmp/longjmp internally
 * (e.g. bi_compile_file), and inlining would put them in cl_vm_run's
 * frame, corrupting its register-cached locals after longjmp.
 *
 * Kept deliberately minimal (no large locals, no cached pointers that
 * survive across the fptr call) to avoid stack-corruption exposure.
 * All validation is done in validate_builtin() before we get here. */
/* THR is the calling thread, passed in rather than looked up: this used to
 * do two cl_get_current_thread() calls per builtin, and every caller
 * (cl_vm_run's OP_CALL / OP_APPLY, cl_vm_apply) already holds the pointer. */
static CL_Obj call_builtin(CL_Thread *thr, CL_Function *func,
                           CL_Obj *args, int nargs)
{
    CL_Obj result;
    int mc;
    validate_builtin(func, nargs);
    /* Crash diagnostics for main.c's fatal handler: ONE per-thread store.
     * The name is derived there, not here — cl_symbol_name is a call plus
     * an arena dereference on every builtin call, and the char * it hands
     * back dangles the moment compaction moves the name string. */
    thr->last_builtin = CL_PTR_TO_OBJ(func);
    /* Save pre-reset mv state so NLX builtins (THROW) can see the multiple
     * values of their last argument after call_builtin's reset.  The
     * single-value case — every call whose last argument was not a VALUES
     * / multiple-value producer — is one store instead of a loop. */
    mc = thr->mv_count;
    thr->pre_call_mv_count = mc;
    if (mc == 1) {
        thr->pre_call_mv_values[0] = thr->mv_values[0];
    } else {
        int mi;
        for (mi = 0; mi < mc && mi < CL_MAX_MV; mi++)
            thr->pre_call_mv_values[mi] = thr->mv_values[mi];
    }
    thr->mv_count = 1;
#if defined(DEBUG_GC) || defined(DEBUG_GC_STRESS) || defined(DEBUG_GC_ROOTS)
    /* A builtin must hand back the protect stack as it found it: an entry
     * it leaves behind is the ADDRESS of one of its dead C locals, and the
     * next compaction forwards an arena offset into whatever frame reuses
     * that slot -- a corrupted C local far from the culprit, layout-
     * dependent, invisible to the heap itself (the Amiga-only finding of
     * 2026-09-18: a "not a function" CONS in a local never assigned).
     * Checked here, at the one return, so the leaking builtin is named. */
    {
        int roots_before = gc_root_count;
        result = func->func(args, nargs);
        if (gc_root_count != roots_before) {
            const char *nm = CL_SYMBOL_P(func->name) ? cl_symbol_name(func->name) : "?";
            fprintf(stderr, "[GC-ROOTS] builtin %s returned with %d protect entr%s "
                    "left on the stack (had %d, has %d)\n", nm,
                    gc_root_count - roots_before,
                    gc_root_count - roots_before == 1 ? "y" : "ies",
                    roots_before, gc_root_count);
            cl_error(CL_ERR_GENERAL, "builtin %s returned with %d protect entries "
                     "left on the GC root stack (had %d, has %d)", nm,
                     gc_root_count - roots_before, roots_before,
                     gc_root_count);
        }
    }
#else
    result = func->func(args, nargs);
#endif
    thr->mv_values[0] = result;
    return result;
}

/* The JIT's entry to call_builtin (see vm.h).  Its own function so the
 * static one stays out of cl_vm_run's frame; the frame here is as small as
 * call_builtin's own. */
CL_Obj cl_vm_call_builtin(CL_Thread *thr, CL_Function *func,
                          CL_Obj *args, int nargs)
{
    return call_builtin(thr, func, args, nargs);
}

#define C_STACK_LIMIT CL_C_STACK_LIMIT   /* vm.h */

/* Deep-recursion guard for the reader and compiler: they push GC roots
 * once per nesting level, and cl_gc_push_root abort()s the process at the
 * CL_GC_ROOT_STACK_SIZE ceiling.  Signal a clean error while there is
 * still headroom for the error path's own pushes (NLX landings restore
 * gc_root_count, so unwinding from here is safe).  Deliberately NOT part
 * of cl_check_c_stack: condition handlers run BEFORE the stack unwinds
 * (CLHS 9.1), so the handler machinery (cl_vm_apply, builtins) executes
 * with gc_root_count still at the ceiling — if those paths ran this
 * check too, the handler would immediately re-signal and the original
 * error could never be caught.  The reserved margin is what the handler
 * runs in; cl_gc_push_root's hard abort remains the last-resort backstop. */
void cl_check_recursion_guards(const char *context)
{
    if (gc_root_count >= CL_GC_ROOT_STACK_SIZE - 64) {
        cl_error(CL_ERR_OVERFLOW,
                 "GC root stack nearly exhausted in %s "
                 "(%d of %d slots used): the form is nested too deeply.",
                 context, gc_root_count, CL_GC_ROOT_STACK_SIZE);
    }
    cl_check_c_stack(context);
}

void cl_check_c_stack(const char *context)
{
    volatile char probe;
    long used;

    /* Platform-exact check first: on AmigaOS the shell `stack` is small
     * and fixed (64K-400K), so the 3MB usage budget below can never fire
     * before physical overflow — measure real headroom against the task's
     * stack bounds instead.  16K margin covers the error-signaling path. */
    {
        long headroom = platform_stack_headroom();
        if (headroom >= 0 && headroom < 16384) {
            cl_error(CL_ERR_OVERFLOW,
                     "C stack nearly exhausted in %s (%ld bytes left): "
                     "the form is nested too deeply for this stack. "
                     "Increase the stack (e.g. `stack 400000` in the "
                     "Amiga shell) and retry.",
                     context, headroom);
        }
    }
#if defined(CL_ASAN_BUILD) || defined(__SANITIZE_ADDRESS__) || \
    (defined(__has_feature) && __has_feature(address_sanitizer))
    /* ASan's fake-stack puts each function's locals in a heap-allocated
     * region.  &probe and cl_c_stack_base end up in unrelated mappings,
     * so the simple subtraction returns nonsense values (often >1GB).
     * The check is meaningless under ASan — skip it. */
    (void)probe; (void)used; (void)context;
    return;
#else
    if (!cl_c_stack_base) cl_c_stack_base = (char *)&probe;
    used = (long)(cl_c_stack_base - (char *)&probe);
    if (used < 0) used = -used;  /* Handle stack growing up or down */
#ifdef DEBUG_VM
    if (used > c_stack_max_seen) {
        c_stack_max_seen = used;
        if (c_stack_max_seen > 1024 * 1024)
            fprintf(stderr, "[STACK] %s: %ldKB\n", context, used / 1024);
    }
#endif
    if (used > C_STACK_LIMIT) {
        cl_error(CL_ERR_OVERFLOW,
                 "C stack overflow in %s (used %ldKB, limit %ldKB)",
                 context, used / 1024, (long)C_STACK_LIMIT / 1024);
    }
#endif
}

/* Shared buffers for OP_CALL keyword processing and OP_APPLY argument flattening.
 * Moved out of cl_vm_eval to keep the per-recursion stack frame small.
 *
 * IMPORTANT: vm_extra_args holds CL_Obj values removed from the VM stack
 * during &rest/&key processing.  cl_cons (called to build the &rest list)
 * can trigger GC.  These arrays MUST be GC-rooted — see
 * cl_vm_gc_visit_extra_thread. */
/* vm_extra_args, vm_extra_args_count, vm_flat_args are now in CL_Thread.
 * Local macros above redirect the old names. */

/* GC root walk for the VM-internal buffers: the CL_Obj values held in a
 * thread's extra-args array while it is not on the VM stack (during &rest
 * list building).  Part of gc_visit_thread_roots (gc_trace.c), so the same
 * slots are marked and forwarded. */
void cl_vm_gc_visit_extra_thread(CL_Thread *t, void (*visit)(CL_Obj *))
{
    int i;
    for (i = 0; i < t->vm_extra_count; i++)
        visit(&t->vm_extra_args_buf[i]);
}

/* dbg_last_*, vm_trace[], vm_trace_idx are now in CL_Thread.
 * Local macros above redirect the old names. */

void vm_trace_dump(void)
{
    int i, idx;
    fprintf(stderr, "=== VM trace (last %d ops) ===\n", VM_TRACE_SIZE);
    for (i = 0; i < VM_TRACE_SIZE; i++) {
        idx = (vm_trace_idx + i) % VM_TRACE_SIZE;
        if (vm_trace[idx].code == NULL && vm_trace[idx].op == 0) continue;
        fprintf(stderr, "  [%d] op=0x%02x ip=%u fp=%d sp=%d code=%p\n",
                i, vm_trace[idx].op, vm_trace[idx].ip,
                vm_trace[idx].fp, vm_trace[idx].sp,
                (void *)vm_trace[idx].code);
    }
}

/*
 * cl_vm_run — the shared dispatch loop.
 * Expects a frame to already be pushed on cl_vm.frames[].
 * Runs until cl_vm.fp drops to base_fp, then returns the result.
 * Both cl_vm_eval and cl_vm_apply use this.
 */
static CL_Obj cl_vm_run(int base_fp, int base_nlx)
{
    cl_check_c_stack("cl_vm_run");

    /* Cache thread pointer once — push/pop/mv_count use it directly,
     * eliminating the cl_get_current_thread() call on each access.
     * CT itself is NOT redefined — only the hot-path accessors are shadowed. */
    CL_Thread *thr = cl_get_current_thread();

    /* Inline push/pop: ~2 function calls + 4 CT lookups saved per opcode.
     * VM_PUSH evaluates (v) into a temp to prevent UB when (v) contains
     * a pop (double sp modification without sequence point). */
#ifdef DEBUG_VM_PUSH_GUARD
#define cl_vm_push(v) do { \
        CL_Obj _pv = (v); \
        if (thr->vm.sp >= (int)thr->vm.stack_size) \
            cl_error(CL_ERR_OVERFLOW, "VM stack overflow"); \
        if (CL_HEAP_P(_pv) && _pv >= cl_heap.arena_size) { \
            fprintf(stderr, \
                "[VM-PUSH-GUARD] dispatch: val=0x%08x out of arena (size=0x%08x) sp=%d fp=%d ip=%u\n", \
                (unsigned)_pv, (unsigned)cl_heap.arena_size, thr->vm.sp, thr->vm.fp, ip); \
            cl_capture_backtrace(); \
            fprintf(stderr, "%s", cl_backtrace_buf); \
            fflush(stderr); \
            abort(); \
        } \
        thr->vm.stack[thr->vm.sp++] = _pv; \
    } while(0)
#else
#define cl_vm_push(v) do { \
        CL_Obj _pv = (v); \
        if (thr->vm.sp >= (int)thr->vm.stack_size) \
            cl_error(CL_ERR_OVERFLOW, "VM stack overflow"); \
        thr->vm.stack[thr->vm.sp++] = _pv; \
    } while(0)
#endif
#define cl_vm_pop() (thr->vm.stack[--thr->vm.sp])

    /* Cache all hot-path thread accessors through local thr pointer.
     * Without this, cl_vm/cl_mv_values/cl_mv_count each call
     * cl_get_current_thread() on every access, which at -O0 is a
     * full function call whose return value can be corrupted by
     * stack memory reuse between the callee's ret and the caller's
     * use of the return register. */
#undef cl_vm
#define cl_vm (thr->vm)
#undef cl_mv_count
#define cl_mv_count (thr->mv_count)
#undef cl_mv_values
#define cl_mv_values (thr->mv_values)
    /* The same shadowing for every other per-thread field the loop touches.
     * Each of these is a CT-> macro in thread.h, i.e. a cl_get_current_thread
     * call — a platform TLS lookup once a second thread exists.  An NLX push
     * reads a dozen of them, so under the MT sento matrix a single
     * UNWIND-PROTECT cost a dozen pthread_getspecific calls.  Every landing
     * reloads `thr` first, so the shadows stay valid across longjmp. */
#undef cl_nlx_stack
#define cl_nlx_stack (thr->nlx_stack)
#undef cl_nlx_top
#define cl_nlx_top (thr->nlx_top)
#undef cl_nlx_max
#define cl_nlx_max (thr->nlx_max)
#undef cl_nlx_floor
#define cl_nlx_floor (thr->nlx_floor)
#undef cl_dyn_top
#define cl_dyn_top (thr->dyn_top)
#undef cl_handler_stack
#define cl_handler_stack (thr->handler_stack)
#undef cl_handler_top
#define cl_handler_top (thr->handler_top)
#undef cl_handler_active_mask
#define cl_handler_active_mask (thr->handler_active_mask)
#undef cl_restart_stack
#define cl_restart_stack (thr->restart_stack)
#undef cl_restart_top
#define cl_restart_top (thr->restart_top)
#undef cl_error_frame_top
#define cl_error_frame_top (thr->error_frame_top)
#undef gc_root_count
#define gc_root_count (thr->gc_root_count)
#undef cl_saved_pending_stack
#define cl_saved_pending_stack (thr->saved_pending_stack)
#undef cl_saved_pending_top
#define cl_saved_pending_top (thr->saved_pending_top)
#undef cl_saved_pending_max
#define cl_saved_pending_max (thr->saved_pending_max)
#undef cl_pending_throw
#define cl_pending_throw (thr->pending_throw)
#undef cl_pending_tag
#define cl_pending_tag (thr->pending_tag)
#undef cl_pending_value
#define cl_pending_value (thr->pending_value)
#undef cl_pending_error_code
#define cl_pending_error_code (thr->pending_error_code)
#undef cl_pending_error_msg
#define cl_pending_error_msg (thr->pending_error_msg)
#undef cl_pending_mv_count
#define cl_pending_mv_count (thr->pending_mv_count)
#undef cl_pending_mv_values
#define cl_pending_mv_values (thr->pending_mv_values)
#undef cl_trace_count
#define cl_trace_count (thr->trace_count)
#undef cl_trace_depth
#define cl_trace_depth (thr->trace_depth)
#undef cl_pre_call_mv_count
#define cl_pre_call_mv_count (thr->pre_call_mv_count)
#undef cl_pre_call_mv_values
#define cl_pre_call_mv_values (thr->pre_call_mv_values)

    /* ---- The activation's single NLX landing ----
     *
     * Every NLX frame this activation pushes (OP_CATCH / OP_UWPROT /
     * OP_BLOCK_PUSH / OP_TAGBODY_PUSH) shares ONE jmp_buf, armed lazily by
     * the first push, instead of each push running its own setjmp: the
     * frame already records everything the landing needs (code, constants,
     * bytecode, sp/fp, the marks, catch_ip + offset), so a transfer to any
     * of them longjmps here and the code at `nlx_landing` restores from
     * cl_nlx_stack[cl_nlx_top].  A transfer that originates in THIS
     * activation's own loop (RETURN-FROM / GO / a re-throw) does not even
     * longjmp — it jumps to the landing directly.  On 68k a setjmp is a
     * full movem; an UNWIND-PROTECT used to pay it on every entry.
     *
     * landing_armed is volatile: it is written after the setjmp and read
     * after a longjmp, which C99 7.13.2.1 otherwise leaves indeterminate. */
    struct { jmp_buf buf; CL_JMPBUF_GUARD } landing;
    volatile int landing_armed = 0;

#define VM_NLX_ARM() do {                                                  \
        if (!landing_armed) {                                              \
            if (CL_SETJMP(landing.buf) != 0) goto nlx_landing;             \
            landing_armed = 1;                                             \
        }                                                                  \
    } while (0)

    /* Fill the NLX frame at NLX for a push by this activation.  The marks
     * are read straight off `thr` (no cl_compiler_mark / cl_printer_state_save
     * calls, which went through cl_get_current_thread). */
#define VM_NLX_FRAME_INIT(nlx, ty, tg, off) do {                            \
        (nlx)->type = (ty);                                                \
        (nlx)->landing = &landing.buf;                                     \
        (nlx)->vm_sp = cl_vm.sp;                                           \
        (nlx)->vm_fp = cl_vm.fp;                                           \
        (nlx)->tag = (tg);                                                 \
        (nlx)->result = CL_NIL;                                            \
        (nlx)->catch_ip = ip;                                              \
        (nlx)->offset = (off);                                             \
        (nlx)->code = code;                                                \
        (nlx)->constants = constants;                                      \
        (nlx)->bytecode = frame->bytecode;                                 \
        (nlx)->base_fp = base_fp;                                          \
        (nlx)->dyn_mark = cl_dyn_top;                                      \
        (nlx)->handler_mark = cl_handler_top;                              \
        (nlx)->handler_active_mask = cl_handler_active_mask;               \
        (nlx)->restart_mark = cl_restart_top;                              \
        (nlx)->error_mark = cl_error_frame_top;                            \
        (nlx)->gc_root_mark = gc_root_count;                               \
        (nlx)->compiler_mark = (void *)thr->active_compiler;               \
        (nlx)->printer_mark.depth = thr->pr_depth;                         \
        (nlx)->printer_mark.inprog_top = thr->pr_inprog_top;               \
        (nlx)->printer_mark.dispatch_active = thr->pr_pprint_dispatch_active; \
        (nlx)->printer_mark.circle_active = thr->pr_circle_active;         \
        (nlx)->mv_count = 1;                                               \
        (nlx)->saved_pending_mark = cl_saved_pending_top;                  \
        (nlx)->saved_jit_depth = thr->jit_depth;                           \
        (nlx)->saved_jit_c_floor = thr->jit_c_floor;                       \
        (nlx)->mv_save_mark = thr->mv_save_top;                            \
    } while (0)

    /* Transfer to NLX frame F: straight to this activation's landing when
     * F belongs to it, else through cl_nlx_jump (longjmp to the owning
     * activation, or to a C/JIT frame's own buf). */
#define VM_NLX_GOTO(f) do {                                                \
        if ((f)->landing == &landing.buf) goto nlx_landing_local;          \
        cl_nlx_jump(f);                                                    \
    } while (0)

    /* OP_LT/GT/LE/GE share one body: fixnum fast path, else a REAL type check
     * and cl_arith_compare.  OP is used identically in both paths so codegen is
     * byte-identical to the four hand-written cases.  Inline macro (not a
     * helper call) — these are hot. */
#define VM_ORDER_CMP(OP, opname) do {                                        \
        CL_Obj b = cl_vm_pop(), a = cl_vm_pop();                             \
        if (CL_FIXNUM_P(a) && CL_FIXNUM_P(b)) {                              \
            cl_vm_push(CL_FIXNUM_VAL(a) OP CL_FIXNUM_VAL(b) ? SYM_T : CL_NIL); \
        } else {                                                            \
            if (!CL_REALP(a)) cl_signal_type_error(a, "REAL", opname);      \
            if (!CL_REALP(b)) cl_signal_type_error(b, "REAL", opname);      \
            cl_vm_push(cl_arith_compare(a, b) OP 0 ? SYM_T : CL_NIL);       \
        }                                                                   \
        cl_mv_count = 1;                                                     \
    } while (0)

    /* Shared NULL-constant-pool bug guard for the const-referencing opcodes.
     * "Should never happen"; the fprintf + cl_error tag the offending opcode.
     * Requires a local `idx` in scope.  (OP_GLOAD keeps its own copy — it also
     * dumps a backtrace.) */
#define VM_REQUIRE_CONSTANTS(opname) do {                                    \
        if (!constants) {                                                    \
            fprintf(stderr, "[VM] BUG: " opname " with NULL constants "     \
                    "(idx=%u fp=%d ip=%u)\n", idx, cl_vm.fp, ip);           \
            cl_error(CL_ERR_GENERAL, opname " with NULL constants ptr");    \
        }                                                                   \
    } while (0)
    /* Same, for a handler whose index variable is not named `idx`. */
#define VM_REQUIRE_CONSTANTS_IDX(opname, idxv) do {                          \
        if (!constants) {                                                    \
            fprintf(stderr, "[VM] BUG: " opname " with NULL constants "     \
                    "(idx=%u fp=%d ip=%u)\n", (unsigned)(idxv), cl_vm.fp, ip); \
            cl_error(CL_ERR_GENERAL, opname " with NULL constants ptr");    \
        }                                                                   \
    } while (0)

    CL_Frame *frame = &cl_vm.frames[cl_vm.fp - 1];
    uint8_t *code = frame->code;
    CL_Obj *constants = frame->constants;
    uint32_t ip = frame->ip;
    uint8_t op;
#ifdef PROFILE_OPCODES
    uint8_t prof_prev_op = 0;   /* 0 is not an opcode: "activation entry" */
#endif

    /* ---- Computed goto dispatch (GCC/Clang) ----
     * Eliminates the switch overhead: each opcode handler jumps directly
     * to the next via the dispatch table.  5-15% VM throughput gain on
     * 68020 (no branch-prediction bottleneck through a single indirect). */
#if defined(__GNUC__) && !defined(CL_NO_COMPUTED_GOTO)
#define USE_COMPUTED_GOTO 1
    static void *dispatch_table[256] = {
        [0x00] = &&vm_op_invalid,
        [OP_CONST]       = &&vm_op_OP_CONST,
        [OP_LOAD]        = &&vm_op_OP_LOAD,
        [OP_STORE]       = &&vm_op_OP_STORE,
        [OP_GLOAD]       = &&vm_op_OP_GLOAD,
        [OP_GSTORE]      = &&vm_op_OP_GSTORE,
        [OP_UPVAL]       = &&vm_op_OP_UPVAL,
        [OP_POP]         = &&vm_op_OP_POP,
        [OP_DUP]         = &&vm_op_OP_DUP,
        [OP_CONS]        = &&vm_op_OP_CONS,
        [OP_CAR]         = &&vm_op_OP_CAR,
        [OP_CDR]         = &&vm_op_OP_CDR,
        [OP_ADD]         = &&vm_op_OP_ADD,
        [OP_SUB]         = &&vm_op_OP_SUB,
        [OP_MUL]         = &&vm_op_OP_MUL,
        [OP_DIV]         = &&vm_op_OP_DIV,
        [OP_EQ]          = &&vm_op_OP_EQ,
        [OP_LT]          = &&vm_op_OP_LT,
        [OP_GT]          = &&vm_op_OP_GT,
        [OP_LE]          = &&vm_op_OP_LE,
        [OP_GE]          = &&vm_op_OP_GE,
        [OP_NUMEQ]       = &&vm_op_OP_NUMEQ,
        [OP_NOT]         = &&vm_op_OP_NOT,
        [OP_JMP]         = &&vm_op_OP_JMP,
        [OP_JNIL]        = &&vm_op_OP_JNIL,
        [OP_JTRUE]       = &&vm_op_OP_JTRUE,
        [OP_CALL]        = &&vm_op_OP_CALL,
        [OP_TAILCALL]    = &&vm_op_OP_TAILCALL,
        [OP_CALL_GLOBAL] = &&vm_op_OP_CALL_GLOBAL,
        [OP_TAILCALL_GLOBAL] = &&vm_op_OP_TAILCALL_GLOBAL,
        [OP_RET]         = &&vm_op_OP_RET,
        [OP_CLOSURE]     = &&vm_op_OP_CLOSURE,
        [OP_APPLY]       = &&vm_op_OP_APPLY,
        [OP_LIST]        = &&vm_op_OP_LIST,
        [OP_NIL]         = &&vm_op_OP_NIL,
        [OP_T]           = &&vm_op_OP_T,
        [OP_FLOAD]       = &&vm_op_OP_FLOAD,
        [OP_DEFMACRO]    = &&vm_op_OP_DEFMACRO,
        [OP_CATCH]       = &&vm_op_OP_CATCH,
        [OP_ARGC]        = &&vm_op_OP_ARGC,
        [OP_UNCATCH]     = &&vm_op_OP_UNCATCH,
        [OP_UWPROT]      = &&vm_op_OP_UWPROT,
        [OP_UWPOP]       = &&vm_op_OP_UWPOP,
        [OP_UWRETHROW]   = &&vm_op_OP_UWRETHROW,
        [OP_MV_LOAD]     = &&vm_op_OP_MV_LOAD,
        [OP_MV_TO_LIST]  = &&vm_op_OP_MV_TO_LIST,
        [OP_MV_SAVE]     = &&vm_op_OP_MV_SAVE,
        [OP_MV_RESTORE]  = &&vm_op_OP_MV_RESTORE,
        [OP_HANDLER_CASE_PUSH] = &&vm_op_OP_HANDLER_CASE_PUSH,
        [OP_HANDLER_CASE_POP]  = &&vm_op_OP_HANDLER_CASE_POP,
        [OP_NTH_VALUE]   = &&vm_op_OP_NTH_VALUE,
        [OP_DYNBIND]     = &&vm_op_OP_DYNBIND,
        [OP_DYNUNBIND]   = &&vm_op_OP_DYNUNBIND,
        [OP_RPLACA]      = &&vm_op_OP_RPLACA,
        [OP_RPLACD]      = &&vm_op_OP_RPLACD,
        [OP_ASET]        = &&vm_op_OP_ASET,
        [OP_DEFTYPE]     = &&vm_op_OP_DEFTYPE,
        [OP_HANDLER_PUSH] = &&vm_op_OP_HANDLER_PUSH,
        [OP_HANDLER_POP]  = &&vm_op_OP_HANDLER_POP,
        [OP_RESTART_PUSH] = &&vm_op_OP_RESTART_PUSH,
        [OP_RESTART_POP]  = &&vm_op_OP_RESTART_POP,
        [OP_ASSERT_TYPE]  = &&vm_op_OP_ASSERT_TYPE,
        [OP_BLOCK_PUSH]   = &&vm_op_OP_BLOCK_PUSH,
        [OP_BLOCK_POP]    = &&vm_op_OP_BLOCK_POP,
        [OP_BLOCK_RETURN] = &&vm_op_OP_BLOCK_RETURN,
        [OP_FSTORE]       = &&vm_op_OP_FSTORE,
        [OP_MAKE_CELL]    = &&vm_op_OP_MAKE_CELL,
        [OP_CELL_REF]     = &&vm_op_OP_CELL_REF,
        [OP_CELL_SET_LOCAL]  = &&vm_op_OP_CELL_SET_LOCAL,
        [OP_CELL_SET_UPVAL]  = &&vm_op_OP_CELL_SET_UPVAL,
        [OP_TAGBODY_PUSH] = &&vm_op_OP_TAGBODY_PUSH,
        [OP_TAGBODY_POP]  = &&vm_op_OP_TAGBODY_POP,
        [OP_TAGBODY_GO]   = &&vm_op_OP_TAGBODY_GO,
        [OP_PROGV_BIND]   = &&vm_op_OP_PROGV_BIND,
        [OP_PROGV_UNBIND] = &&vm_op_OP_PROGV_UNBIND,
        [OP_DEFSETF]      = &&vm_op_OP_DEFSETF,
        [OP_DEFVAR]       = &&vm_op_OP_DEFVAR,
        [OP_MV_RESET]     = &&vm_op_OP_MV_RESET,
        [OP_AMIGA_CALL]   = &&vm_op_OP_AMIGA_CALL,
        [OP_STRUCT_REF]   = &&vm_op_OP_STRUCT_REF,
        [OP_STRUCT_SET]   = &&vm_op_OP_STRUCT_SET,
        [OP_STORE_POP]        = &&vm_op_OP_STORE_POP,
        [OP_LOAD_LOAD]        = &&vm_op_OP_LOAD_LOAD,
        [OP_LOAD_CALL_GLOBAL] = &&vm_op_OP_LOAD_CALL_GLOBAL,
        [OP_LOAD_STRUCT_REF]  = &&vm_op_OP_LOAD_STRUCT_REF,
        [OP_LOAD_MV_RESET]    = &&vm_op_OP_LOAD_MV_RESET,
        [OP_LOAD_RET]         = &&vm_op_OP_LOAD_RET,
        [OP_EQ_JNIL]          = &&vm_op_OP_EQ_JNIL,
        [OP_GLOAD_JNIL]       = &&vm_op_OP_GLOAD_JNIL,
        [OP_LOAD_JNIL]        = &&vm_op_OP_LOAD_JNIL,
        [OP_LOAD_CONST]       = &&vm_op_OP_LOAD_CONST,
        [OP_GLOAD_CALL_GLOBAL] = &&vm_op_OP_GLOAD_CALL_GLOBAL,
        [OP_GLOAD_EQ_JNIL]    = &&vm_op_OP_GLOAD_EQ_JNIL,
        [OP_LOAD_STORE_POP]   = &&vm_op_OP_LOAD_STORE_POP,
        [OP_POP_LOAD]         = &&vm_op_OP_POP_LOAD,
        [OP_AREF]             = &&vm_op_OP_AREF,
        [OP_CHAREQ]           = &&vm_op_OP_CHAREQ,
        [OP_CMP_BR]           = &&vm_op_OP_CMP_BR,
        [OP_PUSH_LOCAL]       = &&vm_op_OP_PUSH_LOCAL,
        [OP_POP_LOCAL]        = &&vm_op_OP_POP_LOCAL,
        [OP_HALT]         = &&vm_op_OP_HALT,
    };

    /* Fill uninitialized slots with unknown handler */
    {
        static int table_initialized = 0;
        if (!table_initialized) {
            int ti;
            for (ti = 0; ti < 256; ti++)
                if (!dispatch_table[ti])
                    dispatch_table[ti] = &&vm_op_unknown;
            table_initialized = 1;
        }
    }

#ifdef PROFILE_OPCODES
#define CL_OPCOUNT_TICK(op) do { \
        cl_op_counts_arr[(uint8_t)(op)]++; \
        cl_op_pair_counts[prof_prev_op][(uint8_t)(op)]++; \
        prof_prev_op = (uint8_t)(op); \
    } while (0)
#else
#define CL_OPCOUNT_TICK(op) ((void)0)
#endif

#define VM_DISPATCH() do { \
        extern int gc_compact_pending; \
        if (gc_compact_pending) { \
            frame->ip = ip; \
            cl_gc_compact_if_pending(); \
        } \
        op = code[ip++]; \
        CL_OPCOUNT_TICK(op); \
        if (__builtin_expect(!dispatch_table[op], 0)) { \
            cl_fatal_diag("[VM] NULL dispatch for op=0x%02x ip=%u fp=%d\n", op, ip-1, cl_vm.fp); \
            cl_capture_backtrace(); cl_fatal_diag("%s", cl_backtrace_buf); abort(); \
        } \
        goto *dispatch_table[op]; \
    } while(0)
#define VM_CASE(opname) vm_op_##opname
#define VM_BREAK VM_DISPATCH()
#define VM_DEFAULT vm_op_unknown

    /* Initial dispatch (skip compaction check on first op) */
    op = code[ip++];
    CL_OPCOUNT_TICK(op);
    if (__builtin_expect(!dispatch_table[op], 0)) {
        cl_fatal_diag("[VM] NULL initial dispatch for op=0x%02x ip=%u fp=%d\n", op, ip-1, cl_vm.fp);
        cl_capture_backtrace(); cl_fatal_diag("%s", cl_backtrace_buf); abort();
    }
    goto *dispatch_table[op];

#else /* Standard switch dispatch */

#define VM_DISPATCH() break
#define VM_CASE(opname) case opname
#define VM_BREAK break
#define VM_DEFAULT default

    for (;;) {

        /* Safe point: run pending compaction between opcodes.
         * All CL_Obj values are on the VM stack (updated during compaction).
         * code/constants point to platform_alloc'd memory (unaffected). */
        {
            extern int gc_compact_pending;
            if (gc_compact_pending) {
                frame->ip = ip;
                cl_gc_compact_if_pending();
            }
        }

        /* Record trace for crash diagnostics (hot path — debug only) */
#ifdef DEBUG_VM
        vm_trace[vm_trace_idx].op = code ? code[ip] : 0;
        vm_trace[vm_trace_idx].ip = ip;
        vm_trace[vm_trace_idx].fp = cl_vm.fp;
        vm_trace[vm_trace_idx].sp = cl_vm.sp;
        vm_trace[vm_trace_idx].code = code;
        vm_trace_idx = (vm_trace_idx + 1) % VM_TRACE_SIZE;
#endif

        if (!code) {
            const char *fn = "?";
            CL_Bytecode *fbc = NULL;
            if (frame && CL_CLOSURE_P(frame->bytecode)) {
                CL_Closure *cc = (CL_Closure *)CL_OBJ_TO_PTR(frame->bytecode);
                fbc = (CL_Bytecode *)CL_OBJ_TO_PTR(cc->bytecode);
            } else if (frame && CL_BYTECODE_P(frame->bytecode)) {
                fbc = (CL_Bytecode *)CL_OBJ_TO_PTR(frame->bytecode);
            }
            if (fbc && fbc->name != CL_NIL && CL_SYMBOL_P(fbc->name))
                fn = cl_symbol_name(fbc->name);
            fprintf(stderr, "[VM] NULL code pointer in dispatch: ip=%u fn=%s fp=%d sp=%d\n",
                    ip, fn, cl_vm.fp, cl_vm.sp);
            cl_capture_backtrace();
            fprintf(stderr, "%s", cl_backtrace_buf);
            cl_error(CL_ERR_GENERAL, "NULL code pointer in VM dispatch (fn=%s)", fn);
        }

        op = code[ip++];
        CL_OPCOUNT_TICK(op);

        /* Trap opcode 0 explicitly */
        if (__builtin_expect(op == 0x00, 0)) {
            const char *fn = "?";
            CL_Bytecode *fbc = NULL;
            if (CL_CLOSURE_P(frame->bytecode)) {
                CL_Closure *cc = (CL_Closure *)CL_OBJ_TO_PTR(frame->bytecode);
                if (CL_BYTECODE_P(cc->bytecode))
                    fbc = (CL_Bytecode *)CL_OBJ_TO_PTR(cc->bytecode);
            } else if (CL_BYTECODE_P(frame->bytecode)) {
                fbc = (CL_Bytecode *)CL_OBJ_TO_PTR(frame->bytecode);
            }
            if (fbc && fbc->name != CL_NIL && CL_SYMBOL_P(fbc->name))
                fn = cl_symbol_name(fbc->name);
            fprintf(stderr, "[VM] ZERO opcode at ip=%u fn=%s code=%p code_len=%u fp=%d sp=%d\n",
                    ip - 1, fn, (void *)code, fbc ? fbc->code_len : 0, cl_vm.fp, cl_vm.sp);
            if (fbc) {
                uint32_t j;
                fprintf(stderr, "[VM]   code bytes: ");
                for (j = 0; j < fbc->code_len && j < 32; j++)
                    fprintf(stderr, "%02x ", code[j]);
                fprintf(stderr, "\n");
                fprintf(stderr, "[VM]   bc->code bytes: ");
                for (j = 0; j < fbc->code_len && j < 32; j++)
                    fprintf(stderr, "%02x ", fbc->code[j]);
                fprintf(stderr, "\n");
                if (code != fbc->code)
                    fprintf(stderr, "[VM]   *** code ptrs DIFFER: frame->code=%p bc->code=%p ***\n",
                            (void *)code, (void *)fbc->code);
            }
            fflush(stderr);
            cl_capture_backtrace();
            fprintf(stderr, "%s", cl_backtrace_buf);
            vm_trace_dump();
            abort();
        }

        /* Debug: check watchpoint before instruction dispatch */
#ifdef CL_DEBUG_UWP
        if (dbg_watch_idx >= 0 && cl_vm.stack[dbg_watch_idx] != dbg_watch_orig) {
            DBG_CHECK_WATCH("pre-dispatch");
        }
#endif

        /* CRASH HUNT: check if we're about to dispatch CLASS-SLOT-INDEX-TABLE at fp=45 */
        if (cl_vm.fp == 45 && ip == 0) {
            CL_Bytecode *_chk = NULL;
            if (CL_BYTECODE_P(frame->bytecode))
                _chk = (CL_Bytecode *)CL_OBJ_TO_PTR(frame->bytecode);
            else if (CL_CLOSURE_P(frame->bytecode)) {
                CL_Closure *_cc = (CL_Closure *)CL_OBJ_TO_PTR(frame->bytecode);
                if (CL_BYTECODE_P(_cc->bytecode))
                    _chk = (CL_Bytecode *)CL_OBJ_TO_PTR(_cc->bytecode);
            }
            if (_chk && _chk->name != CL_NIL && CL_SYMBOL_P(_chk->name)) {
                const char *_fn = cl_symbol_name(_chk->name);
                if (_fn && _fn[0] == 'C' && _fn[5] == '-' && _fn[6] == 'S') {
                    fprintf(stderr, "[CRASH-HUNT] About to dispatch fp=%d ip=0 fn=%s code=%p code[0]=0x%02x thr=%p\n",
                            cl_vm.fp, _fn, (void *)code, (unsigned)code[0], (void *)thr);
                    fprintf(stderr, "[CRASH-HUNT] frame=%p frame->code=%p frame->bytecode=0x%08x base_fp=%d\n",
                            (void *)frame, (void *)frame->code, (unsigned)frame->bytecode, base_fp);
                    fflush(stderr);
                }
            }
        }

        /* Save last dispatched opcode for crash diagnostics (debug only) */
#ifdef DEBUG_VM
        dbg_last_op = op;
        dbg_last_ip = ip;
        dbg_last_fp = cl_vm.fp;
        dbg_last_code = code;

        /* Targeted: dump state when dispatching CLASS-SLOT-INDEX-TABLE */
        {
            CL_Bytecode *_fbc = NULL;
            if (CL_BYTECODE_P(frame->bytecode))
                _fbc = (CL_Bytecode *)CL_OBJ_TO_PTR(frame->bytecode);
            else if (CL_CLOSURE_P(frame->bytecode)) {
                CL_Closure *_cc = (CL_Closure *)CL_OBJ_TO_PTR(frame->bytecode);
                if (CL_BYTECODE_P(_cc->bytecode))
                    _fbc = (CL_Bytecode *)CL_OBJ_TO_PTR(_cc->bytecode);
            }
            if (_fbc && _fbc->name != CL_NIL && CL_SYMBOL_P(_fbc->name)) {
                const char *_fn = cl_symbol_name(_fbc->name);
                if (_fn && _fn[0] == 'C' && _fn[5] == '-' && _fn[6] == 'S' && _fn[10] == '-') {
                    fprintf(stderr, "[CSIT-DISP] op=0x%02x ip=%u fp=%d fn=%s code=%p frame_code=%p base_fp=%d\n",
                            op, ip-1, cl_vm.fp, _fn, (void *)code, (void *)frame->code, base_fp);
                    fflush(stderr);
                }
            }
        }
#endif
        /* [TRACE] suppressed */

        if (op == 0x00) goto unknown_opcode;
        switch (op) {
        case 0x00:
            goto unknown_opcode;

#endif /* USE_COMPUTED_GOTO */

    /* ---- Invalid opcode 0 (computed goto target) ---- */
#ifdef USE_COMPUTED_GOTO
    vm_op_invalid:
        op = code[ip - 1];
        goto vm_op_unknown;
#endif

    /* ---- Shared NLX landing (see the `landing` declaration above) ----
     *
     * Reached by longjmp (nlx_landing: reload the thread pointer first —
     * `thr` is an automatic cached at entry and is not reliably live across
     * a longjmp re-entry, notably under MorphOS's 68k emulation) or by a
     * direct goto from this activation's own loop (nlx_landing_local).
     * The thrower set cl_nlx_top to the target frame's index and stored
     * the result / pending state in it.  Everything the loop caches is
     * re-derived here, so nothing modified since the setjmp is read. */
    nlx_landing:
        thr = cl_get_current_thread();
    nlx_landing_local:
        {
            CL_NLXFrame *nlx = &cl_nlx_stack[cl_nlx_top];
            cl_dynbind_restore_to(nlx->dyn_mark);
            cl_handler_top = nlx->handler_mark;
            /* Restore the CLHS 9.1.4 disabled-handler band to its state at
             * this frame's establishment.  If the throw unwound out of a
             * running handler (e.g. INVOKE-RESTART), this re-enables the
             * band that handler had disabled, so the next SIGNAL is caught. */
            cl_handler_active_mask = nlx->handler_active_mask;
            cl_restart_top = nlx->restart_mark;
            cl_error_frame_top = nlx->error_mark;
            gc_root_count = nlx->gc_root_mark;
            cl_jit_restore_depth(nlx->saved_jit_depth, nlx->saved_jit_c_floor);
            cl_compiler_unwind_to(nlx->compiler_mark, CL_CAPTURE_SP());
            cl_fasl_reader_unwind_to(CL_CAPTURE_SP());
            cl_fasl_writer_unwind_to(CL_CAPTURE_SP());
            cl_printer_state_restore(nlx->printer_mark);
            cl_saved_pending_top = nlx->saved_pending_mark;
            /* Drop the UNWIND-PROTECT value records of the cleanups this
             * transfer abandoned (OP_MV_SAVE). */
            thr->mv_save_top = nlx->mv_save_mark;
            cl_vm.sp = nlx->vm_sp;
            cl_vm.fp = nlx->vm_fp;
            frame = &cl_vm.frames[cl_vm.fp - 1];
            code = nlx->code;
            constants = nlx->constants;
            base_fp = nlx->base_fp;
            ip = nlx->catch_ip + nlx->offset;
            /* Sync frame with NLX-restored state — a tail call between the
             * push and the transfer may have changed frame->code/constants/
             * bytecode to the tail target */
            frame->code = code;
            frame->constants = constants;
            frame->bytecode = nlx->bytecode;
#ifdef DEBUG_NLX
            cl_nlx_dbg("[NLX] landing type=%d top=%d sp=%d fp=%d ip=%u\n",
                       nlx->type, cl_nlx_top, cl_vm.sp, cl_vm.fp, ip);
#endif
            switch (nlx->type) {
            case CL_NLX_UWPROT:
                /* Pass-through, not a target: the transfer is still in
                 * flight, so cl_pending_throw stays set.  The saved slot was
                 * pushed at arming time (before the throw); update it now
                 * with the actual pending state that triggered this cleanup,
                 * so UWRETHROW can re-initiate it if the cleanup body
                 * completes without a new NLX of its own. */
                if (cl_saved_pending_top > 0) {
                    CL_SavedPending *sp =
                        &cl_saved_pending_stack[cl_saved_pending_top - 1];
                    sp->pending_throw    = cl_pending_throw;
                    sp->pending_tag      = cl_pending_tag;
                    sp->pending_value    = cl_pending_value;
                    sp->pending_mv_count = cl_pending_mv_count;
                    { int _mi; for (_mi = 0;
                                    _mi < cl_pending_mv_count && _mi < CL_MAX_MV;
                                    _mi++)
                        sp->pending_mv_values[_mi] = cl_pending_mv_values[_mi]; }
                    sp->pending_error_code = cl_pending_error_code;
                    strncpy(sp->pending_error_msg, cl_pending_error_msg,
                            sizeof(sp->pending_error_msg) - 1);
                    sp->pending_error_msg[sizeof(sp->pending_error_msg) - 1] = '\0';
                    sp->entered_via_longjmp = 1; /* our landing fired — UWRETHROW must rethrow */
                }
                /* The protected form produced no values on this path: park
                 * an EMPTY record for the OP_MV_RESTORE after the cleanup
                 * (it runs only when the transfer is parked for an enclosing
                 * cleanup rather than re-thrown). */
                if (thr->mv_save_top + 1 > CL_MV_SAVE_SIZE)
                    cl_error(CL_ERR_OVERFLOW,
                             "UNWIND-PROTECT value-save stack overflow");
                thr->mv_save_buf[thr->mv_save_top++] = CL_MAKE_FIXNUM(0);
                break;
            case CL_NLX_TAGBODY: {
                /* Target of a completed transfer (see the CATCH case). */
                CL_Obj tag_index = nlx->result;
                cl_pending_throw = 0;
                cl_mv_count = 1;
                /* Re-arm: keep the NLX frame active for repeated GO */
                cl_nlx_top++;
                cl_vm_push(tag_index);
                break;
            }
            case CL_NLX_HANDLER_CASE: {
                /* The clause bindings are gone (handler_top was restored to
                 * the frame's mark above).  Resume at the matched clause's
                 * OP_JMP in the landing table with the condition on the
                 * stack as the clause's sole value. */
                CL_Obj cond = nlx->result;
                cl_pending_throw = 0;
                cl_mv_count = 1;
                cl_mv_values[0] = cond;
                ip += 5 * (uint32_t)nlx->hc_clause;
                cl_vm_push(cond);
                break;
            }
            default: {
                /* CATCH / BLOCK: this landing IS the target of a non-local
                 * transfer, so that transfer is complete: drop any pending
                 * NLX state.  The pending state can be a FOREIGN one — an
                 * error unwind (cl_pending_throw == 2) that landed in an
                 * UNWIND-PROTECT cleanup, where the cleanup then threw out
                 * to this catch.  CLHS 5.2 says the original transfer is
                 * abandoned when a cleanup initiates its own; leaving the
                 * flag set instead let the next enclosing OP_UWRETHROW
                 * resurrect that abandoned error. */
                CL_Obj throw_result = nlx->result;
                int mi;
                cl_pending_throw = 0;
                /* Restore multiple values preserved across NLX */
                cl_mv_count = nlx->mv_count;
                for (mi = 0; mi < cl_mv_count && mi < CL_MAX_MV; mi++)
                    cl_mv_values[mi] = nlx->mv_values[mi];
                cl_vm_push(throw_result);
                break;
            }
            }
        }
        VM_DISPATCH();

        VM_CASE(OP_HALT): {
            CL_Obj result = (cl_vm.sp > (int)(frame->bp + frame->n_locals))
                            ? cl_vm_pop() : CL_NIL;
            /* Restore stack and NLX to before this run */
            cl_vm.sp = frame->bp;
            cl_vm.fp = base_fp;
            cl_nlx_top = base_nlx;
            return result;
        }

        VM_CASE(OP_CONST): {
            uint16_t idx = read_u16(code, &ip);
            VM_REQUIRE_CONSTANTS("OP_CONST");
            cl_vm_push(constants[idx]);
            cl_mv_count = 1;
            VM_BREAK;
        }

        VM_CASE(OP_NIL):
            cl_vm_push(CL_NIL);
            cl_mv_count = 1;
            VM_BREAK;

        VM_CASE(OP_T):
            cl_vm_push(SYM_T);
            cl_mv_count = 1;
            VM_BREAK;

        VM_CASE(OP_LOAD): {
            uint8_t slot = code[ip++];
            cl_vm_push(cl_vm.stack[frame->bp + slot]);
            VM_BREAK;
        }

        VM_CASE(OP_STORE): {
            uint8_t slot = code[ip++];
            cl_vm.stack[frame->bp + slot] = cl_vm.stack[cl_vm.sp - 1];
            DBG_CHECK_WATCH("OP_STORE");
            VM_BREAK;
        }

        /* ---- Superinstructions (specs/performance.md 4.3) ----
         * Each is the adjacent pair the peephole pass fused (opcodes.h): the
         * pair's semantics in one dispatch, with the intermediate push/pop
         * gone.  The MV effect is the pair's: a member that wrote
         * cl_mv_count still does, one that did not still does not.  The
         * three fused with a heavier tail (CALL_GLOBAL, STRUCT_REF, RET)
         * sit directly before that tail's handler and FALL THROUGH into it
         * after the LOAD, so the tail exists once. */

        VM_CASE(OP_STORE_POP): {          /* STORE n; POP */
            uint8_t slot = code[ip++];
            cl_vm.stack[frame->bp + slot] = cl_vm.stack[--cl_vm.sp];
            DBG_CHECK_WATCH("OP_STORE_POP");
            VM_BREAK;
        }

        VM_CASE(OP_LOAD_LOAD): {          /* LOAD a; LOAD b */
            uint8_t a = code[ip], b = code[ip + 1];
            ip += 2;
            cl_vm_push(cl_vm.stack[frame->bp + a]);
            cl_vm_push(cl_vm.stack[frame->bp + b]);
            VM_BREAK;
        }

        VM_CASE(OP_LOAD_CONST): {         /* LOAD s; CONST k */
            uint8_t slot = code[ip++];
            uint16_t idx = read_u16(code, &ip);
            VM_REQUIRE_CONSTANTS("OP_LOAD_CONST");
            cl_vm_push(cl_vm.stack[frame->bp + slot]);
            cl_vm_push(constants[idx]);
            cl_mv_count = 1;              /* OP_CONST's write */
            VM_BREAK;
        }

        VM_CASE(OP_LOAD_STORE_POP): {     /* LOAD a; STORE b; POP */
            uint8_t a = code[ip], b = code[ip + 1];
            ip += 2;
            cl_vm.stack[frame->bp + b] = cl_vm.stack[frame->bp + a];
            DBG_CHECK_WATCH("OP_LOAD_STORE_POP");
            VM_BREAK;
        }

        /* --- String-scan fast path (specs/performance.md 4.4) --- */

        VM_CASE(OP_AREF): {               /* pop idx, pop vec; push vec[idx] */
            uint8_t kind = code[ip++];
            CL_Obj idx_obj = cl_vm_pop();
            CL_Obj vec = cl_vm_pop();
            /* Inline: a fixnum index into a simple base string (every
             * accessor kind) or a simple general vector (AREF/SVREF).  The
             * unsigned compare folds the negative-index check into the
             * bound.  Everything else — wide strings, fill-pointer /
             * displaced / bit / byte vectors, the type and range errors —
             * is the builtin's own path in cl_vector_ref1. */
            if (CL_FIXNUM_P(idx_obj)) {
                uint32_t i = (uint32_t)CL_FIXNUM_VAL(idx_obj);
                if (kind != CL_AREF_KIND_SVREF && CL_STRING_P(vec)) {
                    CL_String *str = (CL_String *)CL_OBJ_TO_PTR(vec);
                    if (i < str->length) {
                        cl_vm_push(CL_MAKE_CHAR((unsigned char)str->data[i]));
                        cl_mv_count = 1;
                        VM_BREAK;
                    }
                } else if (kind < CL_AREF_KIND_CHAR && CL_VECTOR_P(vec)) {
                    CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(vec);
                    if (i < v->length && v->rank <= 1 &&
                        (kind == CL_AREF_KIND_AREF
                             ? !(v->flags & CL_VEC_FLAG_DISPLACED)
                             : v->flags == 0)) {
                        cl_vm_push(v->data[i]);
                        cl_mv_count = 1;
                        VM_BREAK;
                    }
                }
            }
            frame->ip = ip;               /* error attribution */
            cl_vm_push(cl_vector_ref1(vec, idx_obj, kind));
            cl_mv_count = 1;
            VM_BREAK;
        }

        VM_CASE(OP_CHAREQ): {             /* (char= a b) */
            CL_Obj b = cl_vm_pop(), a = cl_vm_pop();
            if (!CL_CHAR_P(a) || !CL_CHAR_P(b)) {
                frame->ip = ip;
                cl_error(CL_ERR_TYPE, "CHAR=: not a character");
            }
            /* Same code <=> same tagged word (CL_MAKE_CHAR). */
            cl_vm_push(a == b ? SYM_T : CL_NIL);
            cl_mv_count = 1;
            VM_BREAK;
        }

        VM_CASE(OP_CMP_BR): {             /* LT..CHAREQ; JNIL/JTRUE t */
            uint8_t kind = code[ip++];
            int32_t offset = read_i32(code, &ip);
            CL_Obj b = cl_vm_pop(), a = cl_vm_pop();
            int cmp = kind & CL_CMP_BR_CMP_MASK;
            int holds;
            cl_mv_count = 1;              /* the comparison's write */
            if (cmp < CL_CMP_BR_CHAREQ && CL_FIXNUM_P(a) && CL_FIXNUM_P(b)) {
                int32_t av = CL_FIXNUM_VAL(a), bv = CL_FIXNUM_VAL(b);
                switch (cmp) {
                case CL_CMP_BR_LT: holds = av <  bv; break;
                case CL_CMP_BR_GT: holds = av >  bv; break;
                case CL_CMP_BR_LE: holds = av <= bv; break;
                case CL_CMP_BR_GE: holds = av >= bv; break;
                default:           holds = av == bv; break;
                }
            } else if (cmp == CL_CMP_BR_CHAREQ && CL_CHAR_P(a) && CL_CHAR_P(b)) {
                holds = (a == b);
            } else {
                frame->ip = ip;           /* error attribution */
                holds = cl_vm_compare_kind(a, b, cmp);
            }
            if ((kind & CL_CMP_BR_IF_TRUE) ? holds : !holds) ip += offset;
            VM_BREAK;
        }

        VM_CASE(OP_PUSH_LOCAL): {         /* (push item local) */
            uint8_t slot = code[ip++];
            int base = frame->bp + slot;
            /* Both cons cells' sources are on the VM stack (the item at
             * TOS, the list in its slot): rooted, forwarded in place. */
            CL_Obj cell = cl_cons_rooted(&cl_vm.stack[cl_vm.sp - 1],
                                         &cl_vm.stack[base]);
            cl_vm.sp--;
            cl_vm.stack[base] = cell;
            cl_vm_push(cell);
            cl_mv_count = 1;
            VM_BREAK;
        }

        VM_CASE(OP_POP_LOCAL): {          /* (pop local) */
            uint8_t slot = code[ip++];
            CL_Obj *lp = &cl_vm.stack[frame->bp + slot];
            CL_Obj list = *lp;
            if (CL_CONS_P(list)) {
                CL_Cons *cell = (CL_Cons *)CL_OBJ_TO_PTR(list);
                *lp = cell->cdr;
                cl_vm_push(cell->car);
            } else {
                /* NIL pops NIL; anything else is CAR's type error. */
                CL_Obj car;
                frame->ip = ip;
                car = cl_car(list);
                *lp = cl_cdr(list);
                cl_vm_push(car);
            }
            cl_mv_count = 1;
            VM_BREAK;
        }

        VM_CASE(OP_POP_LOAD): {           /* POP; LOAD s */
            uint8_t slot = code[ip++];
            cl_vm.stack[cl_vm.sp - 1] = cl_vm.stack[frame->bp + slot];
            VM_BREAK;
        }

        VM_CASE(OP_LOAD_MV_RESET): {      /* LOAD s; MV_RESET */
            uint8_t slot = code[ip++];
            cl_vm_push(cl_vm.stack[frame->bp + slot]);
            cl_mv_count = 1;
            VM_BREAK;
        }

        VM_CASE(OP_LOAD_JNIL): {          /* LOAD s; JNIL t — nothing pushed */
            uint8_t slot = code[ip++];
            int32_t offset = read_i32(code, &ip);
            if (CL_NULL_P(cl_vm.stack[frame->bp + slot])) ip += offset;
            VM_BREAK;
        }

        VM_CASE(OP_EQ_JNIL): {            /* EQ; JNIL t — pop both, no push */
            int32_t offset = read_i32(code, &ip);
            CL_Obj b = cl_vm_pop(), a = cl_vm_pop();
            cl_mv_count = 1;              /* OP_EQ's write */
            if (a != b) ip += offset;
            VM_BREAK;
        }

        /* The symbol-value read OP_GLOAD and OP_GLOAD_JNIL share.  Inline
         * fast path: no dynamic binding active on this thread means the value
         * IS the symbol's global cell — no TLS lookup (thr is the loop's
         * cached thread) and no TLV probe.  GLOAD is 5% of all dispatches in
         * the sento profile. */
#define VM_GLOBAL_VALUE(sym) \
        ((thr->tlv_entry_count == 0 && !CL_NULL_P(sym)) \
             ? ((CL_Symbol *)CL_OBJ_TO_PTR(sym))->value \
             : cl_symbol_value_on(thr, (sym)))

        VM_CASE(OP_GLOAD_JNIL): {         /* GLOAD sym; JNIL t — nothing pushed */
            uint16_t idx = read_u16(code, &ip);
            int32_t offset = read_i32(code, &ip);
            CL_Obj sym, val;
            VM_REQUIRE_CONSTANTS("OP_GLOAD_JNIL");
            sym = constants[idx];
            val = VM_GLOBAL_VALUE(sym);
            if (val == CL_UNBOUND) {
                frame->ip = ip;
                cl_error_cell(CL_ERR_UNBOUND, sym, "Unbound variable: %s",
                         cl_symbol_name(sym));
            }
            cl_mv_count = 1;              /* OP_GLOAD's write */
            if (CL_NULL_P(val)) ip += offset;
            VM_BREAK;
        }

        VM_CASE(OP_GLOAD_EQ_JNIL): {      /* GLOAD sym; EQ; JNIL t — pop 1, no push */
            uint16_t idx = read_u16(code, &ip);
            int32_t offset = read_i32(code, &ip);
            CL_Obj sym, val, a;
            VM_REQUIRE_CONSTANTS("OP_GLOAD_EQ_JNIL");
            sym = constants[idx];
            val = VM_GLOBAL_VALUE(sym);
            if (val == CL_UNBOUND) {
                frame->ip = ip;
                cl_error_cell(CL_ERR_UNBOUND, sym, "Unbound variable: %s",
                         cl_symbol_name(sym));
            }
            a = cl_vm_pop();
            cl_mv_count = 1;              /* OP_EQ's write */
            if (a != val) ip += offset;
            VM_BREAK;
        }

        VM_CASE(OP_GLOAD_CALL_GLOBAL): {  /* GLOAD sym; CALL_GLOBAL f n */
            uint16_t idx = read_u16(code, &ip);
            CL_Obj sym, val;
            VM_REQUIRE_CONSTANTS("OP_GLOAD_CALL_GLOBAL");
            sym = constants[idx];
            val = VM_GLOBAL_VALUE(sym);
            if (val == CL_UNBOUND) {
                frame->ip = ip;
                cl_error_cell(CL_ERR_UNBOUND, sym, "Unbound variable: %s",
                         cl_symbol_name(sym));
            }
            cl_vm_push(val);              /* the callee establishes the MV state */
            op = OP_CALL_GLOBAL;          /* the shared body reads is_tail off op */
            goto vm_call_global_body;     /* its u16 + u8 are next */
        }

        VM_CASE(OP_GLOAD): {
            uint16_t idx = read_u16(code, &ip);
            CL_Obj sym;
            if (!constants) {
                fprintf(stderr, "[VM] BUG: OP_GLOAD with NULL constants (idx=%u fp=%d ip=%u)\n",
                        idx, cl_vm.fp, ip);
                cl_capture_backtrace();
                fprintf(stderr, "%s", cl_backtrace_buf);
                cl_error(CL_ERR_GENERAL, "OP_GLOAD with NULL constants ptr");
            }
            sym = constants[idx];
            CL_Obj val = VM_GLOBAL_VALUE(sym);
            if (val == CL_UNBOUND) {
                frame->ip = ip;   /* the backtrace names this line */
                cl_error_cell(CL_ERR_UNBOUND, sym, "Unbound variable: %s",
                         cl_symbol_name(sym));
            }
            cl_vm_push(val);
            cl_mv_count = 1;
            VM_BREAK;
        }

        VM_CASE(OP_GSTORE): {
            uint16_t idx = read_u16(code, &ip);
            CL_Obj sym;
            VM_REQUIRE_CONSTANTS("OP_GSTORE");
            sym = constants[idx];
            cl_set_symbol_value(sym, cl_vm.stack[cl_vm.sp - 1]);
            if (sym == SYM_STAR_PACKAGE)
                cl_sync_current_package_from_dynamic();
            VM_BREAK;
        }

        VM_CASE(OP_FLOAD): {
            uint16_t idx = read_u16(code, &ip);
            CL_Obj sym;
            VM_REQUIRE_CONSTANTS("OP_FLOAD");
            sym = constants[idx];
            /* Validate sym is a valid symbol before dereferencing */
            if (!CL_HEAP_P(sym) || sym >= cl_heap.arena_size ||
                CL_HDR_TYPE(CL_OBJ_TO_PTR(sym)) != TYPE_SYMBOL) {
                fprintf(stderr, "[VM] FLOAD: constant[%d] = 0x%08x is NOT a symbol "
                        "(heap=%d, arena_bound=%d, type=%d) fp=%d ip=%u bc=0x%08x\n",
                        idx, (unsigned)sym, CL_HEAP_P(sym),
                        (sym < cl_heap.arena_size),
                        (CL_HEAP_P(sym) && sym < cl_heap.arena_size)
                            ? CL_HDR_TYPE(CL_OBJ_TO_PTR(sym)) : -1,
                        cl_vm.fp, ip, (unsigned)frame->bytecode);
#ifdef DEBUG_VM
                {
                    /* Dump the offending bytecode's identity + full constant
                     * pool so we can tell a targeted marking gap (one bad
                     * const) from wholesale corruption, and name the function. */
                    CL_Bytecode *dbc = NULL;
                    CL_Obj bco = frame->bytecode;
                    if (CL_HEAP_P(bco) && bco < cl_heap.arena_size) {
                        void *bp = CL_OBJ_TO_PTR(bco);
                        if (CL_HDR_TYPE(bp) == TYPE_CLOSURE) {
                            CL_Closure *cc = (CL_Closure *)bp;
                            if (CL_HEAP_P(cc->bytecode) && cc->bytecode < cl_heap.arena_size)
                                dbc = (CL_Bytecode *)CL_OBJ_TO_PTR(cc->bytecode);
                        } else if (CL_HDR_TYPE(bp) == TYPE_BYTECODE) {
                            dbc = (CL_Bytecode *)bp;
                        }
                    }
                    if (dbc) {
                        int ci;
                        fprintf(stderr, "[VM] FLOAD: in bytecode '%s' (%s:%u) n_constants=%u\n",
                                (CL_HEAP_P(dbc->name) && CL_SYMBOL_P(dbc->name))
                                    ? cl_symbol_name(dbc->name) : "?",
                                dbc->source_file ? dbc->source_file : "?",
                                dbc->source_line, dbc->n_constants);
                        for (ci = 0; ci < dbc->n_constants; ci++) {
                            CL_Obj cv = dbc->constants[ci];
                            int t = (CL_HEAP_P(cv) && cv < cl_heap.arena_size)
                                        ? CL_HDR_TYPE(CL_OBJ_TO_PTR(cv)) : -1;
                            fprintf(stderr, "[VM]   const[%d] = 0x%08x type=%d %s\n",
                                    ci, (unsigned)cv, t,
                                    (t == TYPE_SYMBOL) ? cl_symbol_name(cv) : "");
                        }
                    }
                }
#endif /* DEBUG_VM */
                cl_capture_backtrace();
                fprintf(stderr, "%s", cl_backtrace_buf);
                cl_error(CL_ERR_GENERAL, "OP_FLOAD: corrupted constant pool entry");
            }
            {
            CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
            if (s->function != CL_UNBOUND) {
                CL_Obj fval = s->function;
                /* Validate function value is a sane heap object */
                if (CL_HEAP_P(fval) && fval >= cl_heap.arena_size) {
                    fprintf(stderr, "[VM] FLOAD: symbol %s function=0x%08x out of arena (size=0x%08x)\n",
                            cl_symbol_name(sym), (unsigned)fval, (unsigned)cl_heap.arena_size);
                    cl_capture_backtrace();
                    fprintf(stderr, "%s", cl_backtrace_buf);
                    cl_error(CL_ERR_GENERAL, "OP_FLOAD: corrupted function binding");
                }
                cl_vm_push(fval);
            } else if (cl_symbol_value_on(thr, sym) != CL_UNBOUND) {
                /* Fall back to value slot (for labels/flet value bindings) */
                cl_vm_push(cl_symbol_value_on(thr, sym));
            } else {
                cl_error_cell(CL_ERR_UNDEFINED, sym, "Undefined function: %s",
                         cl_symbol_name(sym));
            }
            cl_mv_count = 1;
            }
            VM_BREAK;
        }

        VM_CASE(OP_FSTORE): {
            uint16_t idx = read_u16(code, &ip);
            cl_symbol_set_function(constants[idx], cl_vm.stack[cl_vm.sp - 1]);
            VM_BREAK;
        }

        VM_CASE(OP_MAKE_CELL): {
            CL_Obj val = cl_vm_pop();
            cl_vm_push(cl_make_cell(val));
            VM_BREAK;
        }

        VM_CASE(OP_CELL_REF): {
            CL_Obj cell_obj = cl_vm_pop();
            CL_Cell *cell = (CL_Cell *)CL_OBJ_TO_PTR(cell_obj);
            cl_vm_push(cell->value);
            VM_BREAK;
        }

        VM_CASE(OP_CELL_SET_LOCAL): {
            uint8_t slot = code[ip++];
            CL_Obj cell_obj = cl_vm.stack[frame->bp + slot];
            CL_Cell *cell = (CL_Cell *)CL_OBJ_TO_PTR(cell_obj);
            cell->value = cl_vm.stack[cl_vm.sp - 1]; /* peek */
            VM_BREAK;
        }

        VM_CASE(OP_CELL_SET_UPVAL): {
            uint8_t index = code[ip++];
            CL_Obj cell_obj;
            CL_Cell *cell;
            if (CL_CLOSURE_P(frame->bytecode)) {
                CL_Closure *cl = (CL_Closure *)CL_OBJ_TO_PTR(frame->bytecode);
                cell_obj = cl->upvalues[index];
            } else {
                VM_BREAK; /* shouldn't happen */
            }
            if (!CL_CELL_P(cell_obj)) {
                cl_error(CL_ERR_TYPE, "OP_CELL_SET_UPVAL: upvalue[%u] is not a cell (internal compiler error)", (unsigned)index);
            }
            cell = (CL_Cell *)CL_OBJ_TO_PTR(cell_obj);
            cell->value = cl_vm.stack[cl_vm.sp - 1]; /* peek */
            VM_BREAK;
        }

        VM_CASE(OP_UPVAL): {
            /* Flat upvalue access: single index into closure's upvalues[] */
            uint8_t index = code[ip++];
            if (CL_CLOSURE_P(frame->bytecode)) {
                CL_Closure *cl = (CL_Closure *)CL_OBJ_TO_PTR(frame->bytecode);
                cl_vm_push(cl->upvalues[index]);
            } else {
                cl_vm_push(CL_NIL);
            }
            cl_mv_count = 1;
            VM_BREAK;
        }

        VM_CASE(OP_POP):
            cl_vm_pop();
            VM_BREAK;

        VM_CASE(OP_DUP):
            cl_vm_push(cl_vm.stack[cl_vm.sp - 1]);
            VM_BREAK;

        VM_CASE(OP_CONS): {
            /* PEEK, do not pop: below sp the operands are GC roots the
             * collector forwards in place, so cl_cons_rooted needs no
             * root-stack push for them.  Popping first would leave them
             * above sp — invisible to a compaction inside the allocation. */
            int cbase = cl_vm.sp - 2;
            CL_Obj cell = cl_cons_rooted(&cl_vm.stack[cbase],
                                         &cl_vm.stack[cbase + 1]);
            cl_vm.sp = cbase;
            cl_vm_push(cell);
            cl_mv_count = 1;
            VM_BREAK;
        }

        VM_CASE(OP_CAR): {
            CL_Obj obj = cl_vm_pop();
            cl_vm_push(cl_car(obj));
            cl_mv_count = 1;
            VM_BREAK;
        }

        VM_CASE(OP_CDR): {
            CL_Obj obj = cl_vm_pop();
            cl_vm_push(cl_cdr(obj));
            cl_mv_count = 1;
            VM_BREAK;
        }

        /* Arithmetic opcodes: fixnum fast path inline (no function call,
         * no extra type-check) with cl_arith_* fallback for bignum /
         * float / ratio / complex.  Slow path validates operands since
         * cl_arith_* dereferences non-number heap pointers — these
         * opcodes are emitted by the call-site inliner so the old
         * "trusted by special-form expander" contract no longer holds.
         *
         * Same pattern as OP_LT/OP_GT/etc. above. */
        VM_CASE(OP_ADD): {
            CL_Obj b = cl_vm_pop(), a = cl_vm_pop();
            if (CL_FIXNUM_P(a) && CL_FIXNUM_P(b)) {
                int32_t sum = CL_FIXNUM_VAL(a) + CL_FIXNUM_VAL(b);
                if (sum >= CL_FIXNUM_MIN && sum <= CL_FIXNUM_MAX) {
                    cl_vm_push(CL_MAKE_FIXNUM(sum));
                    cl_mv_count = 1;
                    VM_BREAK;
                }
            }
            if (!CL_NUMBER_P(a)) cl_signal_type_error(a, "NUMBER", "+");
            if (!CL_NUMBER_P(b)) cl_signal_type_error(b, "NUMBER", "+");
            cl_vm_push(cl_arith_add(a, b));
            cl_mv_count = 1;
            VM_BREAK;
        }

        VM_CASE(OP_SUB): {
            CL_Obj b = cl_vm_pop(), a = cl_vm_pop();
            if (CL_FIXNUM_P(a) && CL_FIXNUM_P(b)) {
                int32_t diff = CL_FIXNUM_VAL(a) - CL_FIXNUM_VAL(b);
                if (diff >= CL_FIXNUM_MIN && diff <= CL_FIXNUM_MAX) {
                    cl_vm_push(CL_MAKE_FIXNUM(diff));
                    cl_mv_count = 1;
                    VM_BREAK;
                }
            }
            if (!CL_NUMBER_P(a)) cl_signal_type_error(a, "NUMBER", "-");
            if (!CL_NUMBER_P(b)) cl_signal_type_error(b, "NUMBER", "-");
            cl_vm_push(cl_arith_sub(a, b));
            cl_mv_count = 1;
            VM_BREAK;
        }

        /* MUL fast path: only when both magnitudes fit in 15 bits, so
         * the 32-bit product is guaranteed to fit in a 31-bit fixnum
         * with no overflow check. Wider operands fall through to
         * cl_arith_mul, which has its own thorough overflow handling. */
        VM_CASE(OP_MUL): {
            CL_Obj b = cl_vm_pop(), a = cl_vm_pop();
            if (CL_FIXNUM_P(a) && CL_FIXNUM_P(b)) {
                int32_t va = CL_FIXNUM_VAL(a);
                int32_t vb = CL_FIXNUM_VAL(b);
                int32_t aa = va < 0 ? -va : va;
                int32_t ab = vb < 0 ? -vb : vb;
                if ((aa | ab) < 0x8000) {
                    cl_vm_push(CL_MAKE_FIXNUM(va * vb));
                    cl_mv_count = 1;
                    VM_BREAK;
                }
            }
            if (!CL_NUMBER_P(a)) cl_signal_type_error(a, "NUMBER", "*");
            if (!CL_NUMBER_P(b)) cl_signal_type_error(b, "NUMBER", "*");
            cl_vm_push(cl_arith_mul(a, b));
            cl_mv_count = 1;
            VM_BREAK;
        }

        /* DIV fast path: both fixnums, exact division (no remainder),
         * b non-zero, and not the FIXNUM_MIN/-1 overflow case.  Inexact
         * results would need a ratio object, which isn't worth inlining
         * — fall through to cl_arith_div. */
        VM_CASE(OP_DIV): {
            CL_Obj b = cl_vm_pop(), a = cl_vm_pop();
            if (CL_FIXNUM_P(a) && CL_FIXNUM_P(b)) {
                int32_t va = CL_FIXNUM_VAL(a);
                int32_t vb = CL_FIXNUM_VAL(b);
                if (vb != 0 && (va % vb) == 0 &&
                    !(va == CL_FIXNUM_MIN && vb == -1)) {
                    cl_vm_push(CL_MAKE_FIXNUM(va / vb));
                    cl_mv_count = 1;
                    VM_BREAK;
                }
            }
            if (!CL_NUMBER_P(a)) cl_signal_type_error(a, "NUMBER", "/");
            if (!CL_NUMBER_P(b)) cl_signal_type_error(b, "NUMBER", "/");
            cl_vm_push(cl_arith_div(a, b));
            cl_mv_count = 1;
            VM_BREAK;
        }

        VM_CASE(OP_EQ): {
            CL_Obj b = cl_vm_pop(), a = cl_vm_pop();
            cl_vm_push(a == b ? SYM_T : CL_NIL);
            cl_mv_count = 1;
            VM_BREAK;
        }

        /* Comparison opcodes: fixnum fast path (single int compare, no
         * function call) with cl_arith_compare fallback for bignums /
         * floats / ratios.  Slow path validates types since
         * cl_arith_compare assumes its operands are numbers — the
         * inliner now emits these for user calls so the old "expander
         * has already type-checked" contract no longer holds. */
        VM_CASE(OP_NUMEQ): {
            CL_Obj b = cl_vm_pop(), a = cl_vm_pop();
            if (CL_FIXNUM_P(a) && CL_FIXNUM_P(b)) {
                cl_vm_push(a == b ? SYM_T : CL_NIL);
            } else {
                if (!CL_NUMBER_P(a)) cl_signal_type_error(a, "NUMBER", "=");
                if (!CL_NUMBER_P(b)) cl_signal_type_error(b, "NUMBER", "=");
                cl_vm_push(cl_numeric_equal(a, b) ? SYM_T : CL_NIL);
            }
            cl_mv_count = 1;
            VM_BREAK;
        }

        /* Ordered comparators reject complex per CLHS 12.1.4.1. */
        VM_CASE(OP_LT): { VM_ORDER_CMP(<,  "<");  VM_BREAK; }
        VM_CASE(OP_GT): { VM_ORDER_CMP(>,  ">");  VM_BREAK; }
        VM_CASE(OP_LE): { VM_ORDER_CMP(<=, "<="); VM_BREAK; }
        VM_CASE(OP_GE): { VM_ORDER_CMP(>=, ">="); VM_BREAK; }

        VM_CASE(OP_NOT): {
            CL_Obj a = cl_vm_pop();
            cl_vm_push(CL_NULL_P(a) ? SYM_T : CL_NIL);
            cl_mv_count = 1;
            VM_BREAK;
        }

        VM_CASE(OP_JMP): {
            int32_t offset = read_i32(code, &ip);
            if (offset < 0) {                /* backward jump = loop body */
                CL_SAFEPOINT();
                VM_POLL_BREAK();
            }
            ip += offset;
            VM_BREAK;
        }

        VM_CASE(OP_JNIL): {
            int32_t offset = read_i32(code, &ip);
            CL_Obj val = cl_vm_pop();
            if (CL_NULL_P(val)) ip += offset;
            VM_BREAK;
        }

        VM_CASE(OP_JTRUE): {
            int32_t offset = read_i32(code, &ip);
            CL_Obj val = cl_vm_pop();
            if (!CL_NULL_P(val)) ip += offset;
            VM_BREAK;
        }

        VM_CASE(OP_LIST): {
            uint8_t n = code[ip++];
            int lbase = cl_vm.sp - n;
            CL_Obj list = CL_NIL;
            int i;
            /* Build the list from the stack (last element on top), leaving
             * sp alone until it is built: the elements stay GC roots for
             * the whole loop, so only the partial list needs protecting —
             * one root-stack entry for the opcode instead of two per cons. */
            CL_GC_PROTECT(list);
            for (i = n - 1; i >= 0; i--)
                list = cl_cons_rooted(&cl_vm.stack[lbase + i], &list);
            CL_GC_UNPROTECT(1);
            cl_vm.sp = lbase;
            cl_vm_push(list);
            cl_mv_count = 1;
            VM_BREAK;
        }

        VM_CASE(OP_CALL):
        VM_CASE(OP_TAILCALL): {
            uint8_t nargs = code[ip++];
            int is_tail = (op == OP_TAILCALL);
            /* fslot: 1 when the callee sits in a stack slot below the
             * arguments (OP_CALL / OP_TAILCALL), 0 when the arguments are
             * the only thing on the stack (OP_CALL_GLOBAL / OP_TAILCALL_GLOBAL
             * resolve the callee from a symbol constant).  Everything after
             * `do_call` is shared and parameterized by it. */
            int fslot = 1;
            CL_Obj *arg_base;
            CL_Obj func_obj;
            unsigned ftype;

            VM_SAFEPOINT();
            VM_POLL_BREAK();

            /* Stack: [func] [arg0] [arg1] ... [argN-1]
               func is below the args */
            arg_base = &cl_vm.stack[cl_vm.sp - nargs];
            func_obj = arg_base[-1];

            /* Resolve symbol to its function binding (for funcall/apply) */
            if (CL_SYMBOL_P(func_obj)) {
                CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(func_obj);
                func_obj = s->function;
                if (CL_NULL_P(func_obj) || func_obj == CL_UNBOUND)
                    cl_error_cell(CL_ERR_UNDEFINED, arg_base[-1],
                                  "Undefined function: %s",
                                  cl_symbol_name(arg_base[-1]));
                arg_base[-1] = func_obj;
            }
            goto do_call;

        VM_CASE(OP_LOAD_CALL_GLOBAL): {   /* LOAD s; CALL_GLOBAL sym n */
            uint8_t slot = code[ip++];
            cl_vm_push(cl_vm.stack[frame->bp + slot]);
            op = OP_CALL_GLOBAL;          /* the shared body reads is_tail off op */
            /* FALLTHROUGH into OP_CALL_GLOBAL: its u16 + u8 are next
             * (OP_GLOAD_CALL_GLOBAL arrives here by goto) */
        }
        vm_call_global_body:
        VM_CASE(OP_CALL_GLOBAL):
        VM_CASE(OP_TAILCALL_GLOBAL): {
            /* FLOAD sym; CALL n fused: the symbol's function binding is
             * resolved here, after the arguments were evaluated, and never
             * passes through the value stack — so there is no function slot
             * under the arguments and OP_RET has nothing extra to drop.
             * Same resolution rules as OP_FLOAD (function cell, then the
             * value cell for a LABELS/FLET binding, else UNDEFINED-FUNCTION).
             * The frame records fslot = 0 for OP_RET. */
            uint16_t sym_idx = read_u16(code, &ip);
            CL_Obj sym;
            CL_Symbol *s;
            nargs = code[ip++];
            is_tail = (op == OP_TAILCALL_GLOBAL);
            fslot = 0;
            VM_SAFEPOINT();
            VM_POLL_BREAK();
            VM_REQUIRE_CONSTANTS_IDX("OP_CALL_GLOBAL", sym_idx);
            sym = constants[sym_idx];
#ifdef DEBUG_VM
            /* The constant-pool validation OP_FLOAD performs.  Debug-only
             * here: the compiler only ever stores the call head's symbol in
             * this slot, so a non-symbol means heap corruption that a
             * -DDEBUG_VM build reports with the full backtrace; the
             * production build does not pay three compares on every
             * interpreted global call for it (a 68020 is the target). */
            if (!CL_HEAP_P(sym) || sym >= cl_heap.arena_size ||
                CL_HDR_TYPE(CL_OBJ_TO_PTR(sym)) != TYPE_SYMBOL) {
                fprintf(stderr, "[VM] CALL_GLOBAL: constant[%d] = 0x%08x is NOT a symbol "
                        "(heap=%d, arena_bound=%d, type=%d) fp=%d ip=%u\n",
                        sym_idx, (unsigned)sym, CL_HEAP_P(sym),
                        (sym < cl_heap.arena_size),
                        (CL_HEAP_P(sym) && sym < cl_heap.arena_size)
                            ? CL_HDR_TYPE(CL_OBJ_TO_PTR(sym)) : -1,
                        cl_vm.fp, ip);
                cl_capture_backtrace();
                fprintf(stderr, "%s", cl_backtrace_buf);
                cl_error(CL_ERR_GENERAL, "OP_CALL_GLOBAL: corrupted constant pool entry");
            }
#endif /* DEBUG_VM */
            s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
            func_obj = s->function;
            if (func_obj == CL_UNBOUND) {
                func_obj = cl_symbol_value_on(thr, sym);
                if (func_obj == CL_UNBOUND) {
                    frame->ip = ip;   /* the backtrace names this line */
                    cl_error_cell(CL_ERR_UNDEFINED, sym, "Undefined function: %s",
                             cl_symbol_name(sym));
                }
            }
            arg_base = &cl_vm.stack[cl_vm.sp - nargs];
        }

        do_call:
            /* Bounds-check before anything dereferences func_obj; one header
             * read then classifies the callee for every branch below (the
             * CL_*_P predicates each re-read it, and the stores in between
             * keep the compiler from merging them). */
            if (CL_HEAP_P(func_obj) && func_obj >= cl_heap.arena_size) {
                fprintf(stderr, "[VM] CALL: func_obj=0x%08x out of arena (size=0x%08x) nargs=%d fp=%d ip=%u\n",
                        (unsigned)func_obj, (unsigned)cl_heap.arena_size, nargs, cl_vm.fp, ip);
                cl_capture_backtrace();
                fprintf(stderr, "%s", cl_backtrace_buf);
                cl_error(CL_ERR_GENERAL, "OP_CALL: corrupted function object (out of arena)");
            }
            ftype = CL_HEAP_P(func_obj) ? CL_HDR_TYPE(CL_OBJ_TO_PTR(func_obj)) : 0xFFu;

            /* Funcallable instance (a generic function struct).  Only a
             * TYPE_STRUCT can be one, so a bytecode/builtin callee pays a
             * single compare here instead of three cl_funcallable_instance_p
             * calls. */
            if (ftype == TYPE_STRUCT && cl_funcallable_instance_p(func_obj)) {
                /* Reader-GF fast path: an accessor call is answered right
                 * here — no unwrap to the discriminating function, no frame,
                 * no bytecode body.  cl_gf_reader_ic_probe matches the
                 * receiver's type_desc against the GF's inline cache (up to
                 * 4 entries, so a call site alternating between a few
                 * receiver classes still hits) and yields CL_UNBOUND on a
                 * miss (or on an unbound slot), which falls through to
                 * ordinary dispatch below.  Non-allocating, so no GC can run
                 * here and nothing needs GC-protecting.
                 *
                 * OP_TAILCALL is excluded on purpose: it must unwind the
                 * caller's frame, which this shortcut does not do.
                 * Tail-position accessor calls still take the (already
                 * fast) Lisp reader discriminator.
                 *
                 * Bypassing slot 3 is only sound because a GF holding a
                 * reader IC is one CLOS promoted;
                 * SET-FUNCALLABLE-INSTANCE-FUNCTION demotes it before
                 * retargeting slot 3, so a user-installed function can
                 * never be skipped.  The writer probe is the mirror image
                 * for (setf (x obj) v): a 2-arg call to a promoted writer GF
                 * ((setf x) takes VAL OBJ, new-value first per CLHS
                 * 5.1.1.2) stores into the cached slot and returns VAL. */
                if (!is_tail) {
                    CL_Obj slot_v = CL_UNBOUND;
                    if (nargs == 1)
                        slot_v = cl_gf_reader_ic_probe(func_obj, arg_base[0]);
                    else if (nargs == 2)
                        slot_v = cl_gf_writer_ic_probe(func_obj, arg_base[0],
                                                       arg_base[1]);
                    if (slot_v != CL_UNBOUND) {
                        cl_vm.sp -= (nargs + fslot);
                        cl_vm_push(slot_v);
                        cl_mv_count = 1;
                        VM_BREAK;
                    }
                }
                /* Unwrap (GF struct → discriminating fn). */
                func_obj = cl_unwrap_funcallable(func_obj);
                if (fslot) arg_base[-1] = func_obj;
                if (CL_HEAP_P(func_obj) && func_obj >= cl_heap.arena_size)
                    cl_error(CL_ERR_GENERAL, "OP_CALL: corrupted discriminating function (out of arena)");
                ftype = CL_HEAP_P(func_obj) ? CL_HDR_TYPE(CL_OBJ_TO_PTR(func_obj)) : 0xFFu;
            }

            if (ftype == TYPE_FUNCTION) {
                /* Built-in C function */
                CL_Function *f = (CL_Function *)CL_OBJ_TO_PTR(func_obj);
                int traced = (cl_trace_count > 0) && is_func_traced(func_obj);
                CL_Obj result;
                if (traced) {
                    /* The trace print can allocate/compact — root func_obj
                     * across it (OP_CALL_GLOBAL has no stack slot holding
                     * it) and re-derive f before handing it to call_builtin. */
                    CL_GC_PROTECT(func_obj);
                    trace_print_entry(f->name, arg_base, nargs);
                    f = (CL_Function *)CL_OBJ_TO_PTR(func_obj);
                    cl_trace_depth++;
                }
                /* No frame is pushed for a builtin, so record where this
                 * frame is before the call: an error the builtin signals is
                 * reported (backtrace, EXT:BACKTRACE) at this call's line,
                 * not at the last bytecode call's.  One store per builtin
                 * call; the bytecode-callee path below saves ip anyway. */
                frame->ip = ip;
                result = call_builtin(thr, f, arg_base, nargs);
                if (traced) {
                    cl_trace_depth--;
                    /* f is stale after the builtin ran; result must survive
                     * the exit print's own allocations before the push. */
                    f = (CL_Function *)CL_OBJ_TO_PTR(func_obj);
                    CL_GC_PROTECT(result);
                    trace_print_exit(f->name, result);
                    CL_GC_UNPROTECT(2);   /* result, func_obj */
                }
                cl_vm.sp -= (nargs + fslot);
                cl_vm_push(result);
            } else if (ftype == TYPE_FFI_STUB) {
                /* FFI stub (DEFCFUN / DEFCSTRUCT descriptor): dispatch like
                 * a builtin — args are already rooted on the stack, no
                 * frame is pushed, the result replaces func+args.  Under
                 * OP_TAILCALL the following OP_RET returns it, exactly as
                 * for a builtin callee. */
                CL_Obj result;
                int traced = (cl_trace_count > 0) && is_func_traced(func_obj);
                if (traced) {
                    /* the trace print can compact — root func_obj across it */
                    CL_GC_PROTECT(func_obj);
                    trace_print_entry(get_func_name(func_obj), arg_base, nargs);
                    cl_trace_depth++;
                }
                frame->ip = ip;   /* as for a builtin: errors land on this line */
                result = cl_ffi_stub_call(func_obj, arg_base, nargs);
                if (traced) {
                    cl_trace_depth--;
                    CL_GC_PROTECT(result);
                    trace_print_exit(get_func_name(func_obj), result);
                    CL_GC_UNPROTECT(2);   /* result, func_obj */
                }
                cl_vm.sp -= (nargs + fslot);
                cl_vm_push(result);
                cl_mv_count = 1;
            } else if (ftype == TYPE_BYTECODE || ftype == TYPE_CLOSURE) {
                CL_Bytecode *callee_bc;
                CL_Frame *new_frame;
                int callee_arity, has_rest, n_opt, has_key, i;
                int min_args, max_args;

                if (CL_CLOSURE_P(func_obj)) {
                    CL_Closure *cl = (CL_Closure *)CL_OBJ_TO_PTR(func_obj);
                    /* Bounds-check bytecode field before type predicate dereferences it */
                    if (CL_HEAP_P(cl->bytecode) && cl->bytecode >= cl_heap.arena_size) {
                        fprintf(stderr, "[VM] CALL: closure 0x%08x bytecode=0x%08x out of arena (size=0x%08x)\n",
                                (unsigned)func_obj, (unsigned)cl->bytecode, (unsigned)cl_heap.arena_size);
                        cl_capture_backtrace();
                        fprintf(stderr, "%s", cl_backtrace_buf);
                        cl_error(CL_ERR_GENERAL, "OP_CALL: closure bytecode out of arena");
                    }
                    if (!CL_BYTECODE_P(cl->bytecode)) {
                        fprintf(stderr, "[VM] CORRUPT: closure 0x%x bytecode field 0x%x type=%u\n",
                                (unsigned)func_obj, (unsigned)cl->bytecode,
                                CL_HEAP_P(cl->bytecode) ?
                                (unsigned)CL_HDR_TYPE(CL_OBJ_TO_PTR(cl->bytecode)) : 999);
                        cl_capture_backtrace();
                        fprintf(stderr, "%s", cl_backtrace_buf);
                        cl_error(CL_ERR_TYPE, "Corrupted closure: bytecode field is type %u",
                                 CL_HEAP_P(cl->bytecode) ?
                                 (unsigned)CL_HDR_TYPE(CL_OBJ_TO_PTR(cl->bytecode)) : 999);
                    }
                    callee_bc = (CL_Bytecode *)CL_OBJ_TO_PTR(cl->bytecode);
                } else {
                    callee_bc = (CL_Bytecode *)CL_OBJ_TO_PTR(func_obj);
                }

                /* Validate bytecode has valid code pointer */
                if (!callee_bc->code || callee_bc->code_len == 0) {
                    CL_Obj fname = callee_bc->name;
                    cl_error(CL_ERR_TYPE, "Bytecode %s has NULL/empty code (code=%p len=%u)",
                             (!CL_NULL_P(fname) && CL_SYMBOL_P(fname))
                                 ? cl_symbol_name(fname) : "<anon>",
                             (void *)callee_bc->code, (unsigned)callee_bc->code_len);
                }

                callee_arity = callee_bc->arity & 0x7FFF;
                has_rest = callee_bc->arity & 0x8000;
                n_opt = callee_bc->n_optional;
                has_key = callee_bc->flags & 1;

                /* Arity check */
                min_args = callee_arity;
                max_args = (has_rest || has_key)
                           ? 255 : callee_arity + n_opt;

                if (nargs < min_args || nargs > max_args) {
                    frame->ip = ip;   /* the backtrace names this line */
                    vm_arity_error(callee_bc, frame, ip, arg_base, nargs,
                                   nargs < min_args ? min_args : max_args);
                }

                /* Native fast path (m68k JIT, opt-in via cl_jit_compile).
                 * Only bytecodes the JIT recognized as safe-to-replace
                 * carry native_code; today that's trivial-literal-leaf
                 * (0-arg constant return) and 1-arg identity.  Both
                 * allocate nothing and return their result in D0.
                 * Side-steps frame push and goes straight to
                 * result-push, mirroring the builtin dispatch above.
                 * Skipped under TAILCALL (would confuse the existing
                 * TAILCALL frame reuse) and when the callee is being
                 * traced (so TRACE output stays faithful).  Arity
                 * checks above have already enforced nargs == arity for
                 * these shapes (matchers reject optional/&key/&rest),
                 * so cl_jit_invoke can dispatch on nargs unconditionally. */
#ifdef CL_JIT_NATIVE
                /* Hot compilation (jit.h): count the interpreted call; the
                 * one that reaches the threshold compiles, and takes the
                 * native path right below.  Heap-free, so callee_bc stays
                 * valid.  Settled functions pay one byte compare. */
                if (!callee_bc->native_code && CL_BC_JIT_COUNTING_P(callee_bc))
                    cl_jit_note_call(callee_bc);
#endif
                if (callee_bc->native_code &&
                    !is_tail && !is_func_traced(func_obj)) {
                    /* Pass the function-object CL_Obj (closure or raw
                     * bytecode) alongside its unwrapped bytecode so JIT'd
                     * code reaches the closure's upvalues[] via OP_UPVAL
                     * / OP_CELL_SET_UPVAL.  Mirrors the VM's
                     * frame->bytecode channel — kept as a separate C arg
                     * because the dispatch needs the resolved bytecode
                     * pointer immediately. */
                    CL_Obj nresult = cl_jit_invoke(func_obj, callee_bc, nargs);
                    cl_vm.sp -= (nargs + fslot);
                    cl_vm_push(nresult);
                    VM_BREAK;
                }
#ifdef CL_JIT_NATIVE
                /* A TAIL call into native code: call it, then return its
                 * values from this frame (OP_RET's body, vm_native_tail_ret)
                 * instead of reusing the frame, which native code cannot
                 * enter.  Since functions compile once hot, the caller of
                 * a hot loop is often interpreted -- an entry function that
                 * runs once and ends in (main-loop) -- and the old skip ran
                 * the compiled loop as bytecode forever.  The frame stays
                 * for the length of the native call: one level, since the
                 * callee's own tail calls happen in native code. */
                if (is_tail && callee_bc->native_code && !is_func_traced(func_obj)) {
                    CL_Obj nresult = cl_jit_invoke(func_obj, callee_bc, nargs);
                    cl_vm.sp -= (nargs + fslot);
                    cl_vm_push(nresult);
                    goto vm_native_tail_ret;
                }
#endif

                /* GC-protect func_obj across the &rest consing below: on
                 * the tail path its stack slot is overwritten by the
                 * argument copy, and OP_CALL_GLOBAL never had one.  Without
                 * this, GC can sweep or move the closure/bytecode. */
                if (has_rest || has_key)
                    CL_GC_PROTECT(func_obj);

                if (is_tail && cl_vm.fp > base_fp + 1) {
                    CL_Obj *src = arg_base;
                    int n_extra = 0;
                    int n_positional = callee_arity + n_opt;

                    /* Trace: adjust depth for tail call transition */
                    if (cl_trace_count > 0) {
                        CL_Obj cur_name = get_func_name(frame->bytecode);
                        if (!CL_NULL_P(cur_name) && CL_SYMBOL_P(cur_name) &&
                            (((CL_Symbol *)CL_OBJ_TO_PTR(cur_name))->flags & CL_SYM_TRACED))
                            cl_trace_depth--;
                        if (is_func_traced(func_obj)) {
                            /* The trace print can allocate/compact: root
                             * func_obj across it — it is stored into
                             * frame->bytecode below — and re-derive the raw
                             * callee_bc pointer from it afterwards. */
                            CL_GC_PROTECT(func_obj);
                            trace_print_entry(callee_bc->name, arg_base, nargs);
                            CL_GC_UNPROTECT(1);
                            if (CL_CLOSURE_P(func_obj)) {
                                CL_Closure *tcl = (CL_Closure *)CL_OBJ_TO_PTR(func_obj);
                                callee_bc = (CL_Bytecode *)CL_OBJ_TO_PTR(tcl->bytecode);
                            } else {
                                callee_bc = (CL_Bytecode *)CL_OBJ_TO_PTR(func_obj);
                            }
                            cl_trace_depth++;
                        }
                    }

                    cl_vm.sp = frame->bp;

                    /* Push positional args */
                    for (i = 0; i < nargs && i < n_positional; i++) {
                        cl_vm_push(src[i]);
                        DBG_CHECK_WATCH("TAILCALL-PUSH-ARG");
                    }

                    /* Fill missing optionals with NIL */
                    while (cl_vm.sp < (int)(frame->bp + n_positional))
                        cl_vm_push(CL_NIL);

                    /* Save extra args for keyword processing */
                    if (has_key || has_rest) {
                        for (i = n_positional; i < nargs; i++) {
                            if (n_extra < 256) vm_extra_args[n_extra++] = src[i];
                        }
                        vm_extra_args_count = n_extra;
                    }

                    /* Handle &rest */
                    if (has_rest) {
                        CL_Obj rest = CL_NIL;
                        CL_Obj *xa = vm_extra_args;
                        int j;
                        /* vm_extra_args[0..vm_extra_args_count) is a marked
                         * and forwarded GC root (mem.c gc_mark_thread /
                         * gc_slot_is_rooted), so the elements need no
                         * protection — only the partial list does, and once
                         * for the whole loop rather than per cons. */
                        CL_GC_PROTECT(rest);
                        for (j = n_extra - 1; j >= 0; j--)
                            rest = cl_cons_rooted(&xa[j], &rest);
                        CL_GC_UNPROTECT(1);
                        cl_vm_push(rest);

                        /* Re-derive callee_bc: the consing above may trigger GC
                         * compaction which moves arena objects.  func_obj was
                         * GC-protected so its CL_Obj was updated, but the raw
                         * C pointer callee_bc still points to the OLD arena
                         * location (now potentially reused by new allocations). */
                        if (CL_CLOSURE_P(func_obj)) {
                            CL_Closure *cl2 = (CL_Closure *)CL_OBJ_TO_PTR(func_obj);
                            callee_bc = (CL_Bytecode *)CL_OBJ_TO_PTR(cl2->bytecode);
                        } else {
                            callee_bc = (CL_Bytecode *)CL_OBJ_TO_PTR(func_obj);
                        }
                    }

                    /* Fill remaining locals with NIL (including key slots) */
                    while (cl_vm.sp < (int)(frame->bp + callee_bc->n_locals))
                        cl_vm_push(CL_NIL);

                    /* Keyword matching */
                    if (has_key) {
                        int allow = (callee_bc->flags & 2) != 0;
                        int ki;
                        /* CLHS 3.4.1.4: signals program-error on odd kwarg count */
                        if (n_extra & 1)
                            cl_error(CL_ERR_ARGS,
                                     "odd number of keyword arguments");
                        /* Check for :allow-other-keys t in caller args */
                        if (!allow) {
                            for (ki = 0; ki + 1 < n_extra; ki += 2) {
                                if (vm_extra_args[ki] == KW_ALLOW_OTHER_KEYS &&
                                    !CL_NULL_P(vm_extra_args[ki + 1])) {
                                    allow = 1;
                                    break;
                                }
                            }
                        }
                        /* Iterate right-to-left so the leftmost duplicate
                         * keyword wins (CLHS 3.4.1.4.1). */
                        {
                            int last_ki = n_extra - 2;
                            if (last_ki & 1) last_ki--;
                            for (ki = last_ki; ki >= 0; ki -= 2) {
                            CL_Obj key = vm_extra_args[ki];
                            CL_Obj val = vm_extra_args[ki + 1];
                            int j, found = 0;
                            for (j = 0; j < callee_bc->n_keys; j++) {
                                if (key == callee_bc->key_syms[j]) {
                                    cl_vm.stack[frame->bp + callee_bc->key_slots[j]] = val;
                                    if (callee_bc->key_suppliedp_slots)
                                        cl_vm.stack[frame->bp + callee_bc->key_suppliedp_slots[j]] = CL_T;
                                    DBG_CHECK_WATCH("TAILCALL-KEY-SUPPLIEDP");
                                    found = 1;
                                    break;
                                }
                            }
                            if (!found && key != KW_ALLOW_OTHER_KEYS && !allow) {
                                if (!CL_SYMBOL_P(key))
                                    cl_error(CL_ERR_ARGS,
                                             "Invalid keyword argument: not a symbol");
                                cl_error(CL_ERR_ARGS, "Unknown keyword argument: %s",
                                         cl_symbol_name(key));
                            }
                            }
                        }
                    }

                    vm_extra_args_count = 0;
                    if (has_rest || has_key)
                        CL_GC_UNPROTECT(1);

                    frame->bytecode = func_obj;
                    frame->code = callee_bc->code;
                    frame->constants = callee_bc->constants;
                    frame->ip = 0;
                    frame->n_locals = callee_bc->n_locals;
                    frame->nargs = nargs;
                    code = callee_bc->code;
                    constants = callee_bc->constants;
                    ip = 0;

                    /* Targeted crash diagnostic: tailcall path */
                    if (0 && cl_vm.fp >= 44) {
                        const char *_fn = "?";
                        if (callee_bc->name != CL_NIL && CL_SYMBOL_P(callee_bc->name))
                            _fn = cl_symbol_name(callee_bc->name);
                        fprintf(stderr, "[DIAG-FP45] TAILCALL fp=%d fn=%s code=%p\n",
                                cl_vm.fp, _fn, (void *)code);
                        fflush(stderr);
                    }

                    if (!constants && callee_bc->n_constants > 0) {
                        CL_Obj fname = callee_bc->name;
                        fprintf(stderr, "[VM] BUG: tailcall '%s' has %d constants but NULL constants ptr\n",
                                (!CL_NULL_P(fname) && CL_SYMBOL_P(fname)) ? cl_symbol_name(fname) : "<anon>",
                                callee_bc->n_constants);
                        cl_capture_backtrace();
                        fprintf(stderr, "%s", cl_backtrace_buf);
                        cl_error(CL_ERR_GENERAL, "Bytecode %s has NULL constants with n_constants=%d",
                                 (!CL_NULL_P(fname) && CL_SYMBOL_P(fname)) ? cl_symbol_name(fname) : "<anon>",
                                 callee_bc->n_constants);
                    }
                } else {
                    /* Normal call: push new frame */
                    uint32_t new_bp;
                    int n_extra = 0;
                    int n_positional = callee_arity + n_opt;

                    /* Trace: entry for normal call */
                    if (cl_trace_count > 0 && is_func_traced(func_obj))  {
                        /* Root func_obj across the (allocating) trace print
                         * and re-derive callee_bc afterwards (see tailcall
                         * twin). */
                        CL_GC_PROTECT(func_obj);
                        trace_print_entry(callee_bc->name, arg_base, nargs);
                        CL_GC_UNPROTECT(1);
                        if (CL_CLOSURE_P(func_obj)) {
                            CL_Closure *tcl = (CL_Closure *)CL_OBJ_TO_PTR(func_obj);
                            callee_bc = (CL_Bytecode *)CL_OBJ_TO_PTR(tcl->bytecode);
                        } else {
                            callee_bc = (CL_Bytecode *)CL_OBJ_TO_PTR(func_obj);
                        }
                        cl_trace_depth++;
                    }

                    /* The arguments stay where they were pushed: the new
                     * frame's bp is their first slot, and the function slot
                     * (if any) sits just below bp, where OP_RET drops it
                     * via frame->fslot.  This used to shift every argument
                     * down one slot on every call. */
                    new_bp = cl_vm.sp - nargs;

                    /* Save extra args for keyword processing */
                    if (has_key || has_rest) {
                        for (i = n_positional; i < nargs; i++) {
                            if (n_extra < 256)
                                vm_extra_args[n_extra++] = cl_vm.stack[new_bp + i];
                        }
                        vm_extra_args_count = n_extra;
                    }

                    /* Truncate stack to positional args */
                    if (nargs > n_positional)
                        cl_vm.sp = new_bp + n_positional;

                    /* Fill missing optionals with NIL */
                    while (cl_vm.sp < (int)(new_bp + n_positional))
                        cl_vm_push(CL_NIL);

                    /* Handle &rest parameter */
                    if (has_rest) {
                        CL_Obj rest = CL_NIL;
                        CL_Obj *xa = vm_extra_args;
                        int j;
                        /* As in OP_TAILCALL above: the extra-args buffer is
                         * itself a forwarded GC root, so only the partial
                         * list needs protecting, once for the loop. */
                        CL_GC_PROTECT(rest);
                        for (j = n_extra - 1; j >= 0; j--)
                            rest = cl_cons_rooted(&xa[j], &rest);
                        CL_GC_UNPROTECT(1);
                        cl_vm_push(rest);

                        /* Re-derive callee_bc: the consing above may trigger GC
                         * compaction which moves arena objects.  func_obj was
                         * GC-protected so its CL_Obj was updated, but the raw
                         * C pointer callee_bc still points to the OLD arena
                         * location (now potentially reused by new allocations). */
                        if (CL_CLOSURE_P(func_obj)) {
                            CL_Closure *cl2 = (CL_Closure *)CL_OBJ_TO_PTR(func_obj);
                            callee_bc = (CL_Bytecode *)CL_OBJ_TO_PTR(cl2->bytecode);
                        } else {
                            callee_bc = (CL_Bytecode *)CL_OBJ_TO_PTR(func_obj);
                        }
                    }

                    /* Fill remaining locals with NIL (including key slots) */
                    while (cl_vm.sp < (int)(new_bp + callee_bc->n_locals))
                        cl_vm_push(CL_NIL);

                    /* Keyword matching */
                    if (has_key) {
                        int allow = (callee_bc->flags & 2) != 0;
                        int ki;
                        /* CLHS 3.4.1.4: signals program-error on odd kwarg count */
                        if (n_extra & 1)
                            cl_error(CL_ERR_ARGS,
                                     "odd number of keyword arguments");
                        /* Check for :allow-other-keys t in caller args */
                        if (!allow) {
                            for (ki = 0; ki + 1 < n_extra; ki += 2) {
                                if (vm_extra_args[ki] == KW_ALLOW_OTHER_KEYS &&
                                    !CL_NULL_P(vm_extra_args[ki + 1])) {
                                    allow = 1;
                                    break;
                                }
                            }
                        }
                        /* Right-to-left so leftmost duplicate keyword wins. */
                        {
                            int last_ki = n_extra - 2;
                            if (last_ki & 1) last_ki--;
                            for (ki = last_ki; ki >= 0; ki -= 2) {
                            CL_Obj key = vm_extra_args[ki];
                            CL_Obj val = vm_extra_args[ki + 1];
                            int j, found = 0;
                            for (j = 0; j < callee_bc->n_keys; j++) {
                                if (key == callee_bc->key_syms[j]) {
                                    cl_vm.stack[new_bp + callee_bc->key_slots[j]] = val;
                                    if (callee_bc->key_suppliedp_slots) {
                                        cl_vm.stack[new_bp + callee_bc->key_suppliedp_slots[j]] = CL_T;
                                        DBG_CHECK_WATCH("CALL-KEY-SUPPLIEDP");
                                    }
                                    found = 1;
                                    break;
                                }
                            }
                            if (!found && key != KW_ALLOW_OTHER_KEYS && !allow) {
                                if (!CL_SYMBOL_P(key))
                                    cl_error(CL_ERR_ARGS,
                                             "Invalid keyword argument: not a symbol");
                                cl_error(CL_ERR_ARGS, "Unknown keyword argument: %s",
                                         cl_symbol_name(key));
                            }
                            }
                        }
                    }

                    vm_extra_args_count = 0;
                    if (has_rest || has_key)
                        CL_GC_UNPROTECT(1);

                    /* Save current frame state */
                    frame->ip = ip;

                    if (cl_vm.fp >= cl_vm.frame_size)
                        cl_error(CL_ERR_OVERFLOW, "Call stack overflow");

                    new_frame = &cl_vm.frames[cl_vm.fp++];
                    new_frame->bytecode = func_obj;
                    new_frame->code = callee_bc->code;
                    new_frame->constants = callee_bc->constants;
                    new_frame->ip = 0;
                    new_frame->bp = new_bp;
                    new_frame->n_locals = callee_bc->n_locals;
                    new_frame->nargs = nargs;
                    new_frame->nlx_level = cl_nlx_top;
                    new_frame->fslot = (uint8_t)fslot;

                    frame = new_frame;
                    code = callee_bc->code;
                    constants = callee_bc->constants;
                    ip = 0;

#ifdef DEBUG_VM
                    /* Structural validations of the freshly pushed frame.
                     * These caught real GC corruption once (a swept-and-reused
                     * bytecode, a NULL constants pool) and stay available for
                     * a -DDEBUG_VM build, but they cost a handful of dependent
                     * loads on EVERY call, so the production loop trusts the
                     * checks at the top of this handler (non-NULL code, sane
                     * closure->bytecode) and the OP_RET pair below. */

                    /* Validate: code pointer must be valid */
                    if (!code) {
                        CL_Obj fname = callee_bc->name;
                        fprintf(stderr, "[VM] BUG: bytecode '%s' has NULL code ptr (bc=0x%08x)\n",
                                (!CL_NULL_P(fname) && CL_SYMBOL_P(fname)) ? cl_symbol_name(fname) : "<anon>",
                                (unsigned)func_obj);
                        cl_capture_backtrace();
                        fprintf(stderr, "%s", cl_backtrace_buf);
                        cl_error(CL_ERR_GENERAL, "Bytecode has NULL code pointer");
                    }
                    /* Validate: first byte of code should be a valid opcode.
                     * If code was freed, reading it may crash. Use a signal-safe
                     * probe: read the first byte and check it's a known opcode. */
                    {
                        uint8_t first_op = code[0]; /* may crash here if freed */
                        if (first_op == 0x00 && callee_bc->code_len > 1) {
                            CL_Obj fname = callee_bc->name;
                            fprintf(stderr,
                                "[VM] WARNING: bytecode '%s' starts with opcode 0x00 "
                                "(code=%p code_len=%u bc=0x%08x)\n",
                                (!CL_NULL_P(fname) && CL_SYMBOL_P(fname))
                                    ? cl_symbol_name(fname) : "<anon>",
                                (void *)code, callee_bc->code_len,
                                (unsigned)func_obj);
                            cl_capture_backtrace();
                            fprintf(stderr, "%s", cl_backtrace_buf);
                            fflush(stderr);
                        }
                    }

                    /* Validate: bytecode object must still be valid (not GC'd and reused) */
                    {
                        CL_Obj bc_obj = CL_CLOSURE_P(func_obj)
                            ? ((CL_Closure *)CL_OBJ_TO_PTR(func_obj))->bytecode
                            : func_obj;
                        if (!CL_BYTECODE_P(bc_obj)) {
                            fprintf(stderr,
                                "[VM] BUG: bytecode 0x%08x type=%u (expected BYTECODE=%u) "
                                "- GC swept/reused? fn=%s code=%p\n",
                                (unsigned)bc_obj,
                                (unsigned)CL_HDR_TYPE(CL_OBJ_TO_PTR(bc_obj)),
                                (unsigned)TYPE_BYTECODE,
                                callee_bc->name != CL_NIL && CL_SYMBOL_P(callee_bc->name)
                                    ? cl_symbol_name(callee_bc->name) : "<anon>",
                                (void *)callee_bc->code);
                            cl_capture_backtrace();
                            fprintf(stderr, "%s", cl_backtrace_buf);
                            cl_error(CL_ERR_GENERAL,
                                     "Bytecode object type changed - likely GC'd and reused");
                        }
                    }

                    /* Validate: bytecode with constants must have non-NULL constants ptr */
                    if (!constants && callee_bc->n_constants > 0) {
                        CL_Obj fname = callee_bc->name;
                        fprintf(stderr, "[VM] BUG: bytecode '%s' has %d constants but NULL constants ptr (code=%p)\n",
                                (!CL_NULL_P(fname) && CL_SYMBOL_P(fname)) ? cl_symbol_name(fname) : "<anon>",
                                callee_bc->n_constants, (void *)callee_bc->code);
                        cl_capture_backtrace();
                        fprintf(stderr, "%s", cl_backtrace_buf);
                        cl_error(CL_ERR_GENERAL, "Bytecode %s has NULL constants with n_constants=%d",
                                 (!CL_NULL_P(fname) && CL_SYMBOL_P(fname)) ? cl_symbol_name(fname) : "<anon>",
                                 callee_bc->n_constants);
                    }
                    /* Extra safety: if first opcode needs constants but ptr is NULL,
                     * catch it before we SIGSEGV.  This also catches corruption where
                     * n_constants was zeroed but code actually uses constants. */
                    if (!constants && code) {
                        uint8_t first_op = code[0];
                        if (first_op == OP_CONST || first_op == OP_FLOAD ||
                            first_op == OP_GLOAD || first_op == OP_GSTORE ||
                            first_op == OP_DYNBIND || first_op == OP_FSTORE ||
                            first_op == OP_CLOSURE) {
                            CL_Obj fname = callee_bc->name;
                            fprintf(stderr, "[VM] BUG: bytecode '%s' first_op=0x%02x needs constants but constants=NULL n_constants=%d\n",
                                    (!CL_NULL_P(fname) && CL_SYMBOL_P(fname)) ? cl_symbol_name(fname) : "<anon>",
                                    first_op, callee_bc->n_constants);
                            fprintf(stderr, "[VM]   bytecode=0x%08x code=%p code_len=%u\n",
                                    (unsigned)func_obj, (void *)code, callee_bc->code_len);
                            cl_capture_backtrace();
                            fprintf(stderr, "%s", cl_backtrace_buf);
                            cl_error(CL_ERR_GENERAL, "Bytecode %s: NULL constants but first op (0x%02x) needs them",
                                     (!CL_NULL_P(fname) && CL_SYMBOL_P(fname)) ? cl_symbol_name(fname) : "<anon>",
                                     first_op);
                        }
                    }
#endif /* DEBUG_VM */
                }
            } else {
                frame->ip = ip;   /* the backtrace names this line */
                vm_not_a_function_error(func_obj, frame, ip, arg_base, nargs);
            }
            VM_BREAK;
        }

        VM_CASE(OP_LOAD_RET): {           /* LOAD s; RET */
            uint8_t slot = code[ip++];
            cl_vm_push(cl_vm.stack[frame->bp + slot]);
            /* FALLTHROUGH into OP_RET, which pops it as the result */
        }
        VM_CASE(OP_RET): {
            CL_Obj result;
#ifdef CL_JIT_NATIVE
        vm_native_tail_ret:   /* OP_CALL: a tail call into native code returns here */
#endif
            result = (cl_vm.sp > (int)(frame->bp + frame->n_locals))
                            ? cl_vm_pop() : CL_NIL;

            /* Trace: print return value.  result was popped ABOVE sp (no
             * longer a rooted stack slot) and is pushed to the caller after
             * this — root it across the allocating trace print. */
            if (cl_trace_count > 0) {
                CL_Obj ret_name = get_func_name(frame->bytecode);
                if (!CL_NULL_P(ret_name) && CL_SYMBOL_P(ret_name) &&
                    (((CL_Symbol *)CL_OBJ_TO_PTR(ret_name))->flags & CL_SYM_TRACED)) {
                    cl_trace_depth--;
                    CL_GC_PROTECT(result);
                    trace_print_exit(ret_name, result);
                    CL_GC_UNPROTECT(1);
                }
            }

            /* Pop frame — and the function slot OP_CALL left under the
             * arguments (fslot = 1), which is what the caller pushed before
             * them; OP_CALL_GLOBAL, OP_APPLY and the entry frames have none. */
            cl_vm.sp = frame->bp - frame->fslot;
            cl_vm.fp--;

            if (cl_vm.fp <= base_fp) {
                /* Returned from top-level call.  Any NLX frame still above
                 * base_nlx was pushed by this activation and never popped;
                 * it points at this activation's landing, which is about to
                 * go away, so drop it (OP_HALT does the same). */
                cl_nlx_top = base_nlx;
                return result;
            }

            /* Restore caller frame */
            frame = &cl_vm.frames[cl_vm.fp - 1];
            code = frame->code;
            constants = frame->constants;
            ip = frame->ip;

#ifdef DEBUG_VM
            /* Structural validations of the restored caller frame — see the
             * matching DEBUG_VM block in OP_CALL: the ip-bounds check alone
             * is four dependent loads (frame -> closure -> bytecode ->
             * code_len) on every return. */

            /* Safety check: validate restored ip is within bytecode bounds */
            if (code && !CL_NULL_P(frame->bytecode)) {
                CL_Bytecode *ret_bc = NULL;
                if (CL_CLOSURE_P(frame->bytecode)) {
                    CL_Closure *cc = (CL_Closure *)CL_OBJ_TO_PTR(frame->bytecode);
                    ret_bc = (CL_Bytecode *)CL_OBJ_TO_PTR(cc->bytecode);
                } else if (CL_BYTECODE_P(frame->bytecode)) {
                    ret_bc = (CL_Bytecode *)CL_OBJ_TO_PTR(frame->bytecode);
                }
                if (ret_bc && ip > ret_bc->code_len) {
                    fprintf(stderr, "[VM] BUG: OP_RET restored ip=%u but code_len=%u\n",
                            ip, ret_bc->code_len);
                    fprintf(stderr, "[VM]   frame fp=%d bytecode=0x%08x code=%p bc->code=%p\n",
                            cl_vm.fp, (unsigned)frame->bytecode,
                            (void *)code, (void *)ret_bc->code);
                    if (code != ret_bc->code)
                        fprintf(stderr, "[VM]   *** frame->code DIFFERS from bytecode->code! ***\n");
                    if (CL_SYMBOL_P(ret_bc->name))
                        fprintf(stderr, "[VM]   name: %s src=%s:%u\n",
                                cl_symbol_name(ret_bc->name),
                                ret_bc->source_file ? ret_bc->source_file : "?",
                                ret_bc->source_line);
                    vm_trace_dump();
                    cl_capture_backtrace();
                    fprintf(stderr, "%s", cl_backtrace_buf);
                    cl_error(CL_ERR_GENERAL, "OP_RET: restored ip=%u exceeds code_len=%u",
                             ip, ret_bc->code_len);
                }
            }

            /* Safety check: if code is non-null and next instruction uses constants, constants must be valid */
            if (code && !constants) {
                uint8_t next_op = code[ip];
                if (next_op == OP_CONST || next_op == OP_FLOAD || next_op == OP_DYNBIND ||
                    next_op == OP_FSTORE || next_op == OP_CLOSURE || next_op == OP_DEFMACRO ||
                    next_op == OP_DEFTYPE || next_op == OP_DEFSETF || next_op == OP_DEFVAR ||
                    next_op == OP_ASSERT_TYPE || next_op == OP_BLOCK_PUSH ||
                    next_op == OP_TAGBODY_PUSH || next_op == OP_BLOCK_RETURN ||
                    next_op == OP_TAGBODY_GO || next_op == OP_HANDLER_PUSH ||
                    next_op == OP_RESTART_PUSH || next_op == OP_LOAD_CALL_GLOBAL ||
                    next_op == OP_GLOAD_JNIL) {
                    CL_Bytecode *fbc = NULL;
                    if (CL_CLOSURE_P(frame->bytecode)) {
                        CL_Closure *cc = (CL_Closure *)CL_OBJ_TO_PTR(frame->bytecode);
                        fbc = (CL_Bytecode *)CL_OBJ_TO_PTR(cc->bytecode);
                    } else if (CL_BYTECODE_P(frame->bytecode)) {
                        fbc = (CL_Bytecode *)CL_OBJ_TO_PTR(frame->bytecode);
                    }
                    fprintf(stderr, "[VM] BUG: RET restored frame with NULL constants but next op=0x%02x needs them\n", next_op);
                    fprintf(stderr, "[VM]   frame fp=%d ip=%u bytecode=0x%08x\n",
                            cl_vm.fp, ip, frame->bytecode);
                    if (fbc) {
                        fprintf(stderr, "[VM]   fn=%s src=%s:%u n_constants=%d\n",
                                (fbc->name != CL_NIL && CL_SYMBOL_P(fbc->name))
                                    ? cl_symbol_name(fbc->name) : "<anon>",
                                fbc->source_file ? fbc->source_file : "?",
                                fbc->source_line, fbc->n_constants);
                    }
                    cl_error(CL_ERR_GENERAL, "NULL constants in restored frame");
                }
            }
#endif /* DEBUG_VM */

            cl_vm_push(result);
            VM_BREAK;
        }

        VM_CASE(OP_CLOSURE): {
            uint16_t idx = read_u16(code, &ip);
            CL_Obj tmpl = constants[idx];
            CL_Bytecode *tmpl_bc = (CL_Bytecode *)CL_OBJ_TO_PTR(tmpl);
            int n_upvals = tmpl_bc->n_upvalues;
            CL_Closure *cl = (CL_Closure *)cl_alloc(TYPE_CLOSURE,
                sizeof(CL_Closure) + n_upvals * sizeof(CL_Obj));
            if (cl) {
                int i;
                /* Re-fetch the template offset from constants[] AFTER cl_alloc:
                 * a compacting GC during the allocation may have relocated the
                 * bytecode object, leaving the cached `tmpl` local holding a
                 * stale pre-move offset.  constants[] is GC-rooted (via
                 * frame->bytecode) and its entry is forwarded in place, so
                 * re-reading it yields the live offset.  Storing the stale
                 * `tmpl` here was the cause of "Corrupted closure: bytecode
                 * field is type 0" when a defun loaded under heap pressure. */
                cl->bytecode = constants[idx];
                /* Read capture descriptors and populate upvalues */
                for (i = 0; i < n_upvals; i++) {
                    uint8_t is_local = code[ip++];
                    uint8_t cap_idx = code[ip++];
                    if (is_local) {
                        /* Capture from current frame's local slot */
                        cl->upvalues[i] = cl_vm.stack[frame->bp + cap_idx];
                    } else {
                        /* Capture from current closure's upvalue slot */
                        if (CL_CLOSURE_P(frame->bytecode)) {
                            CL_Closure *parent_cl = (CL_Closure *)CL_OBJ_TO_PTR(frame->bytecode);
                            cl->upvalues[i] = parent_cl->upvalues[cap_idx];
                        } else {
                            cl->upvalues[i] = CL_NIL;
                        }
                    }
                }
                cl_vm_push(CL_PTR_TO_OBJ(cl));
            } else {
                /* Skip capture descriptors even on alloc failure */
                ip += n_upvals * 2;
                cl_vm_push(CL_NIL);
            }
            cl_mv_count = 1;
            VM_BREAK;
        }

        VM_CASE(OP_DEFMACRO): {
            uint16_t idx = read_u16(code, &ip);
            CL_Obj name = constants[idx];
            CL_Obj expander = cl_vm_pop();
            cl_register_macro(name, expander);
            /* Re-read from the constants pool: cl_register_macro allocates
             * (and can compact), staling the local — the pool lives in
             * platform memory with its entries forwarded in place. */
            cl_vm_push(constants[idx]);
            VM_BREAK;
        }

        VM_CASE(OP_DEFTYPE): {
            uint16_t idx = read_u16(code, &ip);
            CL_Obj name = constants[idx];
            CL_Obj expander = cl_vm_pop();
            cl_register_type(name, expander);
            /* Re-read after the allocating registrar (see OP_DEFMACRO). */
            cl_vm_push(constants[idx]);
            VM_BREAK;
        }

        VM_CASE(OP_DEFSETF): {
            uint16_t acc_idx = read_u16(code, &ip);
            uint16_t upd_idx = read_u16(code, &ip);
            CL_Obj accessor = constants[acc_idx];
            CL_Obj updater  = constants[upd_idx];
            /* Hash-indexed defsetf table; the registrar conses outside
             * the write lock (STW-vs-rwlock deadlock — see
             * cl_alist_index_prepend). */
            cl_register_setf_updater(accessor, updater);
            /* Re-read after the allocating conses (see OP_DEFMACRO). */
            cl_vm_push(constants[acc_idx]);
            VM_BREAK;
        }

        VM_CASE(OP_DEFVAR): {
            uint16_t sym_idx = read_u16(code, &ip);
            CL_Obj sym_obj = constants[sym_idx];
            CL_Symbol *sym = (CL_Symbol *)CL_OBJ_TO_PTR(sym_obj);
            CL_Obj val = cl_vm_pop();
            sym->flags |= CL_SYM_SPECIAL;
            if (sym->value == CL_UNBOUND)
                sym->value = val;
            VM_BREAK;
        }

        VM_CASE(OP_HANDLER_PUSH): {
            uint16_t idx = read_u16(code, &ip);
            CL_Obj type_sym = constants[idx];
            CL_Obj handler = cl_vm_pop();

            if (cl_handler_top >= CL_MAX_HANDLER_BINDINGS) {
#ifdef DEBUG_HANDLER_OVF
                int _hi;
                fprintf(stderr, "[HANDLER-OVF] dump of %d bindings:\n", cl_handler_top);
                for (_hi = 0; _hi < cl_handler_top; _hi++) {
                    char _b[64];
                    cl_prin1_to_string(cl_handler_stack[_hi].type_name, _b, sizeof(_b));
                    fprintf(stderr, "  [%d] %s mark=%d\n", _hi, _b,
                            cl_handler_stack[_hi].handler_mark);
                }
#endif
                cl_error(CL_ERR_OVERFLOW, "Handler stack overflow");
            }

            cl_handler_stack[cl_handler_top].type_name = type_sym;
            cl_handler_stack[cl_handler_top].handler = handler;
            cl_handler_stack[cl_handler_top].nlx_index = -1;
            cl_handler_stack[cl_handler_top].handler_mark = cl_handler_top;
            /* Mark this binding enabled in the active mask (see thread.h). */
            cl_handler_active_mask |= ((uint64_t)1 << cl_handler_top);
            cl_handler_top++;
            VM_BREAK;
        }

        VM_CASE(OP_HANDLER_CASE_PUSH): {
            /* (amilisp::%handler-case form (type (var) . body)...):
             * u16 = constant index of the clause TYPE list, i32 = offset of
             * the landing, a table of one OP_JMP (5 bytes) per clause.  One
             * NLX frame for the form and one handler binding per clause,
             * whose "handler" is the clause index as a fixnum.  Clause 0 is
             * pushed LAST so it is the innermost binding: cl_signal_condition
             * walks the stack top-down, and CLHS 9.1.4 wants the textually
             * first matching clause (same ordering as compile_handler_bind).
             * Nothing is allocated. */
            uint16_t idx = read_u16(code, &ip);
            int32_t hc_offset = read_i32(code, &ip);
            CL_Obj types;
            CL_NLXFrame *nlx;
            int n = 0, k, ni;
            VM_REQUIRE_CONSTANTS("OP_HANDLER_CASE_PUSH");
            types = constants[idx];
            { CL_Obj t = types; while (CL_CONS_P(t)) { n++; t = cl_cdr(t); } }
            if (cl_nlx_top >= cl_nlx_max) {
                cl_nlx_overflow_summary("vm");
                cl_error(CL_ERR_OVERFLOW, "NLX stack overflow");
            }
            if (cl_handler_top + n > CL_MAX_HANDLER_BINDINGS)
                cl_error(CL_ERR_OVERFLOW, "Handler stack overflow");
            VM_NLX_ARM();
            nlx = &cl_nlx_stack[cl_nlx_top];
            VM_NLX_FRAME_INIT(nlx, CL_NLX_HANDLER_CASE, types, hc_offset);
            nlx->hc_clause = 0;
            ni = cl_nlx_top++;
            for (k = n - 1; k >= 0; k--) {
                CL_Obj t = types;
                int j;
                for (j = 0; j < k; j++) t = cl_cdr(t);
                cl_handler_stack[cl_handler_top].type_name = cl_car(t);
                cl_handler_stack[cl_handler_top].handler = CL_MAKE_FIXNUM(k);
                cl_handler_stack[cl_handler_top].nlx_index = ni;
                cl_handler_stack[cl_handler_top].handler_mark = cl_handler_top;
                cl_handler_active_mask |= ((uint64_t)1 << cl_handler_top);
                cl_handler_top++;
            }
            VM_BREAK;
        }

        VM_CASE(OP_HANDLER_CASE_POP): {
            /* Normal exit: drop the clause bindings and the frame (search
             * backward for the frame, like the other *_POP opcodes).  The
             * MV state is the form's and is left alone. */
            int hi;
            for (hi = cl_nlx_top - 1; hi >= 0; hi--) {
                if (cl_nlx_stack[hi].type == CL_NLX_HANDLER_CASE) {
                    int old_top = cl_handler_top;
                    cl_handler_top = cl_nlx_stack[hi].handler_mark;
                    if (cl_handler_top < old_top)
                        cl_handler_active_mask &=
                            ~CL_HANDLER_BAND_MASK(cl_handler_top, old_top);
                    cl_nlx_top = hi;
                    break;
                }
            }
            if (hi < 0 && cl_nlx_top > 0)
                cl_nlx_top--;
            VM_BREAK;
        }

        VM_CASE(OP_HANDLER_POP): {
            uint8_t count = code[ip++];
            int old_top = cl_handler_top;
            cl_handler_top -= count;
            if (cl_handler_top < 0) cl_handler_top = 0;
            /* Clear the active bits of the popped bindings so a later snapshot
             * never carries a stale enabled bit for a reused slot. */
            cl_handler_active_mask &= ~CL_HANDLER_BAND_MASK(cl_handler_top, old_top);
            VM_BREAK;
        }

        VM_CASE(OP_RESTART_PUSH): {
            uint16_t idx = read_u16(code, &ip);
            CL_Obj name_sym = constants[idx];
            /* compile_restart_case pushes: handler, report, interactive,
             * test, tag — pop in reverse. */
            CL_Obj tag = cl_vm_pop();
            CL_Obj test = cl_vm_pop();
            CL_Obj interactive = cl_vm_pop();
            CL_Obj report = cl_vm_pop();
            CL_Obj handler = cl_vm_pop();
            CL_Obj restart;

            if (cl_restart_top >= CL_MAX_RESTART_BINDINGS)
                cl_error(CL_ERR_OVERFLOW, "Restart stack overflow");

            /* Build the first-class restart object.  cl_make_restart
             * GC-protects its own parameter copies, but the caller's C
             * locals (handler, tag, name_sym) are not registered and may
             * be stale after compaction.  Read canonical values back from
             * the newly allocated object. */
            restart = cl_make_restart(name_sym, handler, report,
                                      interactive, test, tag);
            {
                CL_Restart *rp = (CL_Restart *)CL_OBJ_TO_PTR(restart);
                cl_restart_stack[cl_restart_top].name    = rp->name;
                cl_restart_stack[cl_restart_top].handler = rp->function;
                cl_restart_stack[cl_restart_top].tag     = rp->tag;
                cl_restart_stack[cl_restart_top].restart = restart;
            }
            cl_restart_top++;
            VM_BREAK;
        }

        VM_CASE(OP_RESTART_POP): {
            uint8_t count = code[ip++];
            cl_restart_top -= count;
            if (cl_restart_top < 0) cl_restart_top = 0;
            VM_BREAK;
        }

        VM_CASE(OP_ASSERT_TYPE): {
            uint16_t idx = read_u16(code, &ip);
            CL_Obj type_spec = constants[idx];
            CL_Obj val = cl_vm.stack[cl_vm.sp - 1]; /* peek TOS */
            if (!cl_typep(val, type_spec)) {
                /* Build type-error condition with :datum and :expected-type.
                 * cl_typep can run SATISFIES/deftype code (cl_vm_apply) and
                 * compact — re-read val from its rooted stack slot and
                 * type_spec from the constants pool after it, and re-read val
                 * again between the two allocating cons lines. */
                CL_Obj slots = CL_NIL;
                CL_Obj cond;
                CL_Obj pair;
                type_spec = constants[idx];
                CL_GC_PROTECT(slots);
                /* Build each pair, THEN prepend — never nest as
                 * cl_cons(cl_cons(k,v), slots).  Once slots is a non-NIL heap
                 * list, C's unspecified argument evaluation order (right-to-left
                 * on GCC/x86-64) reads the outer `slots` operand before the inner
                 * cl_cons compacts, baking a stale offset into the cdr and
                 * producing a cyclic slots alist.  See apply_condition_slot_initforms. */
                pair = cl_cons(KW_EXPECTED_TYPE, type_spec);
                slots = cl_cons(pair, slots);
                val = cl_vm.stack[cl_vm.sp - 1];
                pair = cl_cons(KW_DATUM, val);
                slots = cl_cons(pair, slots);
                CL_GC_UNPROTECT(1);
                cond = cl_make_condition(SYM_TYPE_ERROR, slots, CL_NIL);
                cl_signal_condition(cond);
                /* If no handler transferred control, fall to C error —
                 * re-read both again: the condition machinery above ran
                 * arbitrary allocating handler code. */
                {
                    char buf[128];
                    char tbuf[64];
                    cl_prin1_to_string(cl_vm.stack[cl_vm.sp - 1], buf, sizeof(buf));
                    cl_prin1_to_string(constants[idx], tbuf, sizeof(tbuf));
                    cl_error(CL_ERR_TYPE, "THE: value %s is not of type %s", buf, tbuf);
                }
            }
            VM_BREAK;
        }

        VM_CASE(OP_BLOCK_PUSH): {
            /* Set up NLX block frame for return-from support */
            uint16_t tag_idx = read_u16(code, &ip);
            int32_t block_offset = read_i32(code, &ip);
            CL_Obj block_tag = constants[tag_idx];
            CL_NLXFrame *nlx;

            if (cl_nlx_top >= cl_nlx_max) {
                    cl_nlx_overflow_summary("vm");
                    cl_error(CL_ERR_OVERFLOW, "NLX stack overflow");
                }

            /* No setjmp here — see the shared landing. */
            VM_NLX_ARM();
            nlx = &cl_nlx_stack[cl_nlx_top];
            VM_NLX_FRAME_INIT(nlx, CL_NLX_BLOCK, block_tag, block_offset);
#ifdef DEBUG_NLX
            cl_nlx_dbg("[NLX] BLOCK_PUSH top=%d tag=0x%08x sp=%d fp=%d\n",
                    cl_nlx_top, (unsigned)block_tag, cl_vm.sp, cl_vm.fp);
#endif
            cl_nlx_top++;
            VM_BREAK;
        }

        VM_CASE(OP_BLOCK_POP): {
            /* Normal exit from block body — pop NLX frame.
             * Search for matching BLOCK, as tail calls in called
             * functions may have leaked BLOCK frames above. */
            {
                int bi;
                for (bi = cl_nlx_top - 1; bi >= 0; bi--) {
                    if (cl_nlx_stack[bi].type == CL_NLX_BLOCK) {
                        cl_nlx_top = bi;
                        break;
                    }
                }
                if (bi < 0 && cl_nlx_top > 0)
                    cl_nlx_top--;
            }
            VM_BREAK;
        }

        VM_CASE(OP_BLOCK_RETURN): {
            /* return-from: pop value, find matching block on NLX stack, longjmp */
            uint16_t tag_idx = read_u16(code, &ip);
            CL_Obj block_tag = constants[tag_idx];
            CL_Obj value = cl_vm_pop();
            int i;

#ifdef DEBUG_NLX
            cl_nlx_debug_dump("BLOCK_RETURN scan", (unsigned)block_tag);
#endif
            /* cl_nlx_floor: frames below it belong to Lisp outside the
             * foreign callback we may be running in — never a target. */
            for (i = cl_nlx_top - 1; i >= cl_nlx_floor; i--) {
                if (cl_nlx_stack[i].type == CL_NLX_BLOCK &&
                    cl_nlx_stack[i].tag == block_tag) {
                    int j;
#ifdef DEBUG_NLX
                    cl_nlx_dbg("[NLX] BLOCK_RETURN match at frame %d, longjmp\n", i);
#endif
                    /* Check for interposing UWPROT frames (skip stale ones) */
                    for (j = cl_nlx_top - 1; j > i; j--) {
                        if (cl_nlx_stack[j].type == CL_NLX_UWPROT &&
                            !nlx_frame_is_stale(&cl_nlx_stack[j])) {
                            cl_pending_throw = 1;
                            cl_pending_tag = block_tag;
                            cl_pending_value = value;
                            cl_pending_mv_count = cl_mv_count;
                            { int mi; for (mi = 0; mi < cl_mv_count && mi < CL_MAX_MV; mi++)
                                cl_pending_mv_values[mi] = cl_mv_values[mi]; }
                            cl_nlx_top = j;
                            VM_NLX_GOTO(&cl_nlx_stack[j]);
                        }
                    }
                    /* No interposing UWPROT — transfer directly to the block */
                    cl_nlx_stack[i].result = value;
                    /* Preserve multiple values across NLX */
                    cl_nlx_stack[i].mv_count = cl_mv_count;
                    { int mi; for (mi = 0; mi < cl_mv_count && mi < CL_MAX_MV; mi++)
                        cl_nlx_stack[i].mv_values[mi] = cl_mv_values[mi]; }
                    cl_nlx_top = i;
                    VM_NLX_GOTO(&cl_nlx_stack[i]);
                }
            }
            cl_error(CL_ERR_GENERAL, "RETURN-FROM: no block named %s%s",
                     CL_NULL_P(block_tag) ? "NIL" : cl_symbol_name(block_tag),
                     cl_nlx_boundary_hint(block_tag, CL_NLX_BLOCK));
            VM_BREAK;
        }

        VM_CASE(OP_TAGBODY_PUSH): {
            /* Set up NLX tagbody frame for cross-closure GO support */
            uint16_t id_idx = read_u16(code, &ip);
            int32_t tb_offset = read_i32(code, &ip);
            CL_Obj tagbody_id = constants[id_idx];
            CL_NLXFrame *nlx;

            if (cl_nlx_top >= cl_nlx_max) {
                    cl_nlx_overflow_summary("vm");
                    cl_error(CL_ERR_OVERFLOW, "NLX stack overflow");
                }

            /* No setjmp here — see the shared landing (which re-arms the
             * frame for repeated GO). */
            VM_NLX_ARM();
            nlx = &cl_nlx_stack[cl_nlx_top];
            VM_NLX_FRAME_INIT(nlx, CL_NLX_TAGBODY, tagbody_id, tb_offset);
#ifdef DEBUG_NLX
            cl_nlx_dbg("[NLX] TAGBODY_PUSH top=%d id=0x%08x sp=%d fp=%d\n",
                    cl_nlx_top, (unsigned)tagbody_id, cl_vm.sp, cl_vm.fp);
#endif
            cl_nlx_top++;
            VM_BREAK;
        }

        VM_CASE(OP_TAGBODY_POP): {
            /* Normal exit from tagbody — pop NLX frame.
             * Search for matching TAGBODY frame. */
            {
                int bi;
                for (bi = cl_nlx_top - 1; bi >= 0; bi--) {
                    if (cl_nlx_stack[bi].type == CL_NLX_TAGBODY) {
                        cl_nlx_top = bi;
                        break;
                    }
                }
                if (bi < 0 && cl_nlx_top > 0)
                    cl_nlx_top--;
            }
            VM_BREAK;
        }

        VM_CASE(OP_TAGBODY_GO): {
            /* Cross-closure GO: pop tag index, find matching tagbody, longjmp */
            uint16_t id_idx = read_u16(code, &ip);
            CL_Obj tagbody_id = constants[id_idx];
            CL_Obj tag_index = cl_vm_pop();
            int i;

#ifdef DEBUG_NLX
            cl_nlx_debug_dump("TAGBODY_GO scan", (unsigned)tagbody_id);
#endif
            for (i = cl_nlx_top - 1; i >= cl_nlx_floor; i--) {
                if (cl_nlx_stack[i].type == CL_NLX_TAGBODY &&
                    cl_nlx_stack[i].tag == tagbody_id) {
                    int j;
#ifdef DEBUG_NLX
                    cl_nlx_dbg("[NLX] TAGBODY_GO match at frame %d, longjmp\n", i);
#endif
                    /* Check for interposing UWPROT frames (skip stale ones) */
                    for (j = cl_nlx_top - 1; j > i; j--) {
                        if (cl_nlx_stack[j].type == CL_NLX_UWPROT &&
                            !nlx_frame_is_stale(&cl_nlx_stack[j])) {
                            cl_pending_throw = 1;
                            cl_pending_tag = tagbody_id;
                            cl_pending_value = tag_index;
                            cl_pending_mv_count = cl_mv_count;
                            { int mi; for (mi = 0; mi < cl_mv_count && mi < CL_MAX_MV; mi++)
                                cl_pending_mv_values[mi] = cl_mv_values[mi]; }
                            cl_nlx_top = j;
                            VM_NLX_GOTO(&cl_nlx_stack[j]);
                        }
                    }
                    /* No interposing UWPROT — transfer directly to the tagbody */
                    cl_nlx_stack[i].result = tag_index;
                    cl_nlx_top = i;
                    VM_NLX_GOTO(&cl_nlx_stack[i]);
                }
            }
#ifdef DEBUG_NLX
            cl_nlx_dbg("[NLX] TAGBODY_GO: NO MATCHING FRAME for id=0x%08x\n",
                    (unsigned)tagbody_id);
#endif
            cl_error(CL_ERR_GENERAL, "GO: tagbody frame not found%s",
                     cl_nlx_boundary_hint(tagbody_id, CL_NLX_TAGBODY));
            VM_BREAK;
        }

        VM_CASE(OP_PROGV_BIND): {
            /* Pop values-list, pop symbols-list, push dyn_mark, bind all */
            CL_Obj values_list = cl_vm_pop();
            CL_Obj symbols_list = cl_vm_pop();
            int mark = cl_dyn_top;
            CL_Thread *thr = CT;

            /* Push saved dyn_mark as fixnum */
            cl_vm_push(CL_MAKE_FIXNUM(mark));

            /* Iterate symbols, pair with values */
            while (!CL_NULL_P(symbols_list)) {
                CL_Obj sym_obj = cl_car(symbols_list);
                CL_Obj val;
                CL_Obj old_tlv;

                if (!CL_SYMBOL_P(sym_obj))
                    cl_error(CL_ERR_TYPE, "PROGV: expected symbol, got non-symbol");

                val = !CL_NULL_P(values_list) ? cl_car(values_list) : CL_UNBOUND;

                if (cl_dyn_top >= CL_MAX_DYN_BINDINGS)
                    cl_error(CL_ERR_OVERFLOW, "Dynamic binding stack overflow");

                old_tlv = cl_tlv_get(thr, sym_obj);
                cl_dyn_stack[cl_dyn_top].symbol = sym_obj;
                cl_dyn_stack[cl_dyn_top].old_value = old_tlv;
                cl_dyn_top++;
                cl_tlv_set(thr, sym_obj, val);

                symbols_list = cl_cdr(symbols_list);
                if (!CL_NULL_P(values_list))
                    values_list = cl_cdr(values_list);
            }
            VM_BREAK;
        }

        VM_CASE(OP_PROGV_UNBIND): {
            /* Pop body result, pop dyn_mark, restore bindings, push result */
            CL_Obj result = cl_vm_pop();
            CL_Obj mark_obj = cl_vm_pop();
            int mark = CL_FIXNUM_VAL(mark_obj);
            cl_dynbind_restore_to(mark);
            cl_vm_push(result);
            VM_BREAK;
        }

        VM_CASE(OP_ARGC): {
            cl_vm_push(CL_MAKE_FIXNUM(frame->nargs));
            cl_mv_count = 1;
            VM_BREAK;
        }

        VM_CASE(OP_APPLY): {
            /* (apply func arglist) — inline dispatch to avoid C stack nesting */
            CL_Obj arglist;
            CL_SAFEPOINT();
            arglist = cl_vm_pop();
            CL_Obj apply_func = cl_vm_pop();
            int nflat = 0;
            int ai;
            int args_base = cl_vm.sp;  /* spread args live at [args_base..sp) */

            /* apply_func was popped ABOVE sp and its old slot is overwritten
             * by the spread below — from here on it is invisible to the GC
             * unless rooted.  Root it for the whole opcode (the builtin call
             * and the &rest/trace allocations all run with it live); error
             * paths unwind the root via the error frame's gc_root_mark. */
            CL_GC_PROTECT(apply_func);

            /* Spread arglist directly onto the VM stack (GC-rooted, no fixed
             * buffer).  Neither cl_car/cl_cdr nor cl_vm_push allocate, so
             * `arglist` cannot move mid-loop.  Bounded by CALL-ARGUMENTS-LIMIT
             * and the VM stack so a huge APPLY cannot overflow. */
            {
                CL_Obj a = arglist;
                while (!CL_NULL_P(a)) {
                    if (nflat >= CL_CALL_ARGS_LIMIT)
                        cl_error(CL_ERR_ARGS,
                                 "APPLY: too many arguments "
                                 "(call-arguments-limit is %d)",
                                 CL_CALL_ARGS_LIMIT);
                    if (cl_vm.sp >= (int)cl_vm.stack_size - 16)
                        cl_error(CL_ERR_OVERFLOW, "APPLY: VM stack overflow");
                    cl_vm_push(cl_car(a));
                    nflat++;
                    a = cl_cdr(a);
                }
            }

            /* Resolve symbol to its function binding */
            if (CL_SYMBOL_P(apply_func)) {
                CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(apply_func);
                CL_Obj apply_sym = apply_func;
                apply_func = s->function;
                if (CL_NULL_P(apply_func) || apply_func == CL_UNBOUND)
                    cl_error_cell(CL_ERR_UNDEFINED, apply_sym,
                                  "APPLY: undefined function: %s",
                                  cl_symbol_name(apply_sym));
            }

            /* Reader-GF fast path — same probe as OP_CALL's, before the
             * unwrap below discards the GF identity.  OP_APPLY is
             * call-and-return (nothing to unwind), so unlike OP_TAILCALL
             * there is no tail exclusion.  Non-allocating, so the
             * apply_func root stays balanced on this early exit. */
            if (nflat == 1 && cl_funcallable_instance_p(apply_func)) {
                CL_Obj slot_v =
                    cl_gf_reader_ic_probe(apply_func, cl_vm.stack[args_base]);
                if (slot_v != CL_UNBOUND) {
                    cl_vm.sp = args_base;   /* drop the spread arg */
                    cl_vm_push(slot_v);
                    cl_mv_count = 1;
                    CL_GC_UNPROTECT(1);     /* apply_func */
                    VM_BREAK;
                }
            }
            /* Writer-GF fast path, ditto: (apply #'(setf x) val obj ...). */
            if (nflat == 2 && cl_funcallable_instance_p(apply_func)) {
                CL_Obj slot_v =
                    cl_gf_writer_ic_probe(apply_func, cl_vm.stack[args_base],
                                          cl_vm.stack[args_base + 1]);
                if (slot_v != CL_UNBOUND) {
                    cl_vm.sp = args_base;   /* drop the spread args */
                    cl_vm_push(slot_v);
                    cl_mv_count = 1;
                    CL_GC_UNPROTECT(1);     /* apply_func */
                    VM_BREAK;
                }
            }

            /* Unwrap funcallable instances (GF struct → discriminating fn). */
            apply_func = cl_unwrap_funcallable(apply_func);

            if (CL_FUNCTION_P(apply_func)) {
                CL_Function *f = (CL_Function *)CL_OBJ_TO_PTR(apply_func);
                int traced = is_func_traced(apply_func);
                CL_Obj result;
                if (traced) {
                    trace_print_entry(f->name, &cl_vm.stack[args_base], nflat);
                    /* Re-derive f after the allocating trace print —
                     * apply_func is rooted, the raw pointer is not. */
                    f = (CL_Function *)CL_OBJ_TO_PTR(apply_func);
                    cl_trace_depth++;
                }
                frame->ip = ip;   /* as in OP_CALL: errors land on this line */
                result = call_builtin(thr, f, &cl_vm.stack[args_base], nflat);
                cl_vm.sp = args_base;  /* drop the spread args */
                if (traced) {
                    cl_trace_depth--;
                    /* f is stale after the builtin ran; root result across
                     * the exit print before it is pushed. */
                    f = (CL_Function *)CL_OBJ_TO_PTR(apply_func);
                    CL_GC_PROTECT(result);
                    trace_print_exit(f->name, result);
                    CL_GC_UNPROTECT(1);
                }
#ifdef DEBUG_COMPILER
                /* Re-derive f — the builtin may have compacted (apply_func
                 * is rooted; the raw pointer is not). */
                f = (CL_Function *)CL_OBJ_TO_PTR(apply_func);
                /* Validate code pointer after builtin returns */
                if (!code) {
                    fprintf(stderr, "[VM] BUG: OP_APPLY builtin returned but code=NULL! fn=%s ip=%u fp=%d\n",
                            CL_SYMBOL_P(f->name) ? cl_symbol_name(f->name) : "?",
                            ip, cl_vm.fp);
                    fflush(stderr);
                } else {
                    /* Check if next opcode is valid */
                    uint8_t next_op = code[ip];
                    if (next_op == 0x00) {
                        CL_Bytecode *fbc = NULL;
                        if (CL_CLOSURE_P(frame->bytecode)) {
                            CL_Closure *cc = (CL_Closure *)CL_OBJ_TO_PTR(frame->bytecode);
                            fbc = (CL_Bytecode *)CL_OBJ_TO_PTR(cc->bytecode);
                        } else if (CL_BYTECODE_P(frame->bytecode)) {
                            fbc = (CL_Bytecode *)CL_OBJ_TO_PTR(frame->bytecode);
                        }
                        fprintf(stderr, "[VM] BUG: OP_APPLY builtin returned, next op=0x00 at code[%u]! "
                                "builtin=%s caller=%s code=%p frame->code=%p code_len=%u fp=%d\n",
                                ip,
                                CL_SYMBOL_P(f->name) ? cl_symbol_name(f->name) : "?",
                                (fbc && CL_SYMBOL_P(fbc->name)) ? cl_symbol_name(fbc->name) : "<anon>",
                                (void *)code, (void *)frame->code,
                                fbc ? fbc->code_len : 0, cl_vm.fp);
                        /* Dump bytecodes */
                        if (fbc) {
                            uint32_t j;
                            fprintf(stderr, "[VM]   bytecodes: ");
                            for (j = 0; j < fbc->code_len && j < 32; j++)
                                fprintf(stderr, "%02x ", fbc->code[j]);
                            fprintf(stderr, "\n");
                            fprintf(stderr, "[VM]   code ptr: ");
                            for (j = 0; j < fbc->code_len && j < 32; j++)
                                fprintf(stderr, "%02x ", code[j]);
                            fprintf(stderr, "\n");
                        }
                        fflush(stderr);
                    }
                }
#endif
                cl_vm_push(result);
            } else if (CL_FFI_STUB_P(apply_func)) {
                /* FFI stub: spread args are rooted at args_base; apply_func
                 * is rooted above.  Same shape as the builtin branch. */
                CL_Obj result;
                frame->ip = ip;   /* errors land on this line */
                result = cl_ffi_stub_call(apply_func,
                                          &cl_vm.stack[args_base], nflat);
                cl_vm.sp = args_base;  /* drop the spread args */
                cl_vm_push(result);
                cl_mv_count = 1;
            } else if (CL_BYTECODE_P(apply_func) || CL_CLOSURE_P(apply_func)) {
                /* The spread args already sit on the VM stack at args_base
                 * (no func slot beneath them).  Dispatch inline like OP_CALL —
                 * avoids cl_vm_apply C stack nesting. */
                {
                    int call_nargs = nflat;
                    CL_Obj call_func = apply_func;
                    CL_Bytecode *callee_bc;
                    CL_Frame *new_frame;
                    int callee_arity, has_rest, n_opt, has_key;
                    int min_args, max_args, new_bp, n_extra = 0;
                    int n_positional;

                    if (CL_CLOSURE_P(call_func)) {
                        CL_Closure *cl = (CL_Closure *)CL_OBJ_TO_PTR(call_func);
                        if (!CL_BYTECODE_P(cl->bytecode)) {
                            fprintf(stderr, "[VM] CORRUPT: APPLY closure 0x%x bytecode field 0x%x type=%u\n",
                                    (unsigned)call_func, (unsigned)cl->bytecode,
                                    CL_HEAP_P(cl->bytecode) ?
                                    (unsigned)CL_HDR_TYPE(CL_OBJ_TO_PTR(cl->bytecode)) : 999);
                            cl_capture_backtrace();
                            fprintf(stderr, "%s", cl_backtrace_buf);
                            cl_error(CL_ERR_TYPE, "APPLY: corrupted closure (bytecode type %u)",
                                     CL_HEAP_P(cl->bytecode) ?
                                     (unsigned)CL_HDR_TYPE(CL_OBJ_TO_PTR(cl->bytecode)) : 999);
                        }
                        callee_bc = (CL_Bytecode *)CL_OBJ_TO_PTR(cl->bytecode);
                    } else {
                        callee_bc = (CL_Bytecode *)CL_OBJ_TO_PTR(call_func);
                    }

                    /* Validate bytecode has valid code pointer */
                    if (!callee_bc->code || callee_bc->code_len == 0) {
                        CL_Obj fname = callee_bc->name;
                        cl_error(CL_ERR_TYPE, "APPLY: bytecode %s has NULL/empty code (code=%p len=%u)",
                                 (!CL_NULL_P(fname) && CL_SYMBOL_P(fname))
                                     ? cl_symbol_name(fname) : "<anon>",
                                 (void *)callee_bc->code, (unsigned)callee_bc->code_len);
                    }

                    callee_arity = callee_bc->arity & 0x7FFF;
                    has_rest = callee_bc->arity & 0x8000;
                    n_opt = callee_bc->n_optional;
                    has_key = callee_bc->flags & 1;
                    min_args = callee_arity;
                    max_args = (has_rest || has_key)
                                 ? CL_CALL_ARGS_LIMIT
                                 : callee_arity + n_opt;
                    n_positional = callee_arity + n_opt;

                    if (call_nargs < min_args)
                        cl_error(CL_ERR_ARGS, "APPLY: too few arguments (got %d, need %d)",
                                 call_nargs, min_args);
                    if (call_nargs > max_args)
                        cl_error(CL_ERR_ARGS, "APPLY: too many arguments (got %d, max %d)",
                                 call_nargs, max_args);

                    if (has_rest || has_key)
                        CL_GC_PROTECT(call_func);

                    /* Args already sit at args_base with no func slot beneath. */
                    new_bp = args_base;

                    /* Save extra args for &key matching only.  Keyword args are
                     * few in practice, so the bounded buffer is fine here; the
                     * &rest list (below) is built straight off the stack and is
                     * NOT subject to this cap. */
                    if (has_key) {
                        for (ai = n_positional; ai < call_nargs; ai++) {
                            if (n_extra < 256)
                                vm_extra_args[n_extra++] = cl_vm.stack[new_bp + ai];
                        }
                    }
                    /* Keep vm_extra_args_count truthful (the GC marks
                     * vm_extra_args[0..count)) before the &rest cons below can
                     * trigger compaction — n_extra is 0 unless &key populated. */
                    vm_extra_args_count = n_extra;

                    /* &rest: cons the surplus args directly off the stack, while
                     * their slots are still live (before truncation).  The
                     * stack slots are GC roots the collector forwards in
                     * place, so cl_cons_rooted reads them after the
                     * allocation and only the partial list is protected. */
                    {
                        CL_Obj rest = CL_NIL;
                        if (has_rest) {
                            int j;
                            CL_GC_PROTECT(rest);
                            for (j = call_nargs - 1; j >= n_positional; j--)
                                rest = cl_cons_rooted(&cl_vm.stack[new_bp + j],
                                                      &rest);
                            CL_GC_UNPROTECT(1);
                            /* Re-derive callee_bc: the consing may have triggered
                             * GC compaction.  call_func is GC-protected so its
                             * CL_Obj was forwarded, but the raw C pointer
                             * callee_bc still aimed at the old arena location. */
                            if (CL_CLOSURE_P(call_func)) {
                                CL_Closure *cl2 = (CL_Closure *)CL_OBJ_TO_PTR(call_func);
                                callee_bc = (CL_Bytecode *)CL_OBJ_TO_PTR(cl2->bytecode);
                            } else {
                                callee_bc = (CL_Bytecode *)CL_OBJ_TO_PTR(call_func);
                            }
                        }

                        /* Truncate to positional */
                        if (call_nargs > n_positional)
                            cl_vm.sp = new_bp + n_positional;

                        /* Fill missing optionals */
                        while (cl_vm.sp < (int)(new_bp + n_positional))
                            cl_vm_push(CL_NIL);

                        /* Push the &rest list built above. */
                        if (has_rest)
                            cl_vm_push(rest);
                    }

                    /* Fill remaining locals */
                    while (cl_vm.sp < (int)(new_bp + callee_bc->n_locals))
                        cl_vm_push(CL_NIL);

                    /* Keyword matching */
                    if (has_key) {
                        int allow = (callee_bc->flags & 2) != 0;
                        int ki;
                        /* CLHS 3.4.1.4: signals program-error on odd kwarg count */
                        if (n_extra & 1)
                            cl_error(CL_ERR_ARGS,
                                     "odd number of keyword arguments");
                        if (!allow) {
                            for (ki = 0; ki + 1 < n_extra; ki += 2) {
                                if (vm_extra_args[ki] == KW_ALLOW_OTHER_KEYS &&
                                    !CL_NULL_P(vm_extra_args[ki + 1])) {
                                    allow = 1; break;
                                }
                            }
                        }
                        /* Right-to-left so leftmost duplicate keyword wins. */
                        {
                            int last_ki = n_extra - 2;
                            if (last_ki & 1) last_ki--;
                            for (ki = last_ki; ki >= 0; ki -= 2) {
                            CL_Obj key = vm_extra_args[ki];
                            CL_Obj val = vm_extra_args[ki + 1];
                            int j, found = 0;
                            for (j = 0; j < callee_bc->n_keys; j++) {
                                if (key == callee_bc->key_syms[j]) {
                                    cl_vm.stack[new_bp + callee_bc->key_slots[j]] = val;
                                    if (callee_bc->key_suppliedp_slots)
                                        cl_vm.stack[new_bp + callee_bc->key_suppliedp_slots[j]] = CL_T;
                                    found = 1; break;
                                }
                            }
                            if (!found && key != KW_ALLOW_OTHER_KEYS && !allow) {
                                if (!CL_SYMBOL_P(key))
                                    cl_error(CL_ERR_ARGS,
                                             "APPLY: invalid keyword argument: not a symbol");
                                cl_error(CL_ERR_ARGS, "APPLY: unknown keyword argument: %s",
                                         cl_symbol_name(key));
                            }
                            }
                        }
                    }

                    vm_extra_args_count = 0;
                    if (has_rest || has_key)
                        CL_GC_UNPROTECT(1);

                    /* Save current frame state and push new frame */
                    frame->ip = ip;
                    if (cl_vm.fp >= cl_vm.frame_size)
                        cl_error(CL_ERR_OVERFLOW, "Call stack overflow");

                    new_frame = &cl_vm.frames[cl_vm.fp++];
                    new_frame->bytecode = call_func;
                    new_frame->code = callee_bc->code;
                    new_frame->constants = callee_bc->constants;
                    new_frame->ip = 0;
                    new_frame->bp = new_bp;
                    new_frame->n_locals = callee_bc->n_locals;
                    new_frame->nargs = call_nargs;
                    new_frame->nlx_level = cl_nlx_top;  /* mirror OP_CALL */
                    new_frame->fslot = 0;   /* apply_func was popped above */

                    /* DIAG: OP_APPLY frame push */
                    if (0 && cl_vm.fp >= 44) {
                        const char *_fn = "?";
                        if (callee_bc->name != CL_NIL && CL_SYMBOL_P(callee_bc->name))
                            _fn = cl_symbol_name(callee_bc->name);
                        fprintf(stderr, "[DIAG-FP45] APPLY fp=%d fn=%s code=%p\n",
                                cl_vm.fp, _fn, (void *)callee_bc->code);
                        fflush(stderr);
                    }

                    frame = new_frame;
                    code = callee_bc->code;
                    constants = callee_bc->constants;
                    ip = 0;
                }
            } else {
                cl_error(CL_ERR_TYPE, "APPLY: not a callable function");
            }
            CL_GC_UNPROTECT(1);  /* apply_func */
            VM_BREAK;
        }

        VM_CASE(OP_CATCH): {
            int32_t catch_offset = read_i32(code, &ip);
            CL_Obj catch_tag = cl_vm_pop();
            CL_NLXFrame *nlx;

            if (cl_nlx_top >= cl_nlx_max) {
                    cl_nlx_overflow_summary("vm");
                    cl_error(CL_ERR_OVERFLOW, "NLX stack overflow");
                }

            /* No setjmp here: the frame points at this activation's shared
             * landing (armed once), which restores from it on a throw. */
            VM_NLX_ARM();
            nlx = &cl_nlx_stack[cl_nlx_top];
            VM_NLX_FRAME_INIT(nlx, CL_NLX_CATCH, catch_tag, catch_offset);
            cl_nlx_top++;
            VM_BREAK;
        }

        VM_CASE(OP_UNCATCH): {
            /* Normal exit from catch body — pop NLX frame.
             * Search for matching CATCH, as tail calls may have
             * leaked BLOCK frames above it. */
            {
                int ci;
                for (ci = cl_nlx_top - 1; ci >= 0; ci--) {
                    if (cl_nlx_stack[ci].type == CL_NLX_CATCH) {
                        cl_nlx_top = ci;
                        break;
                    }
                }
                if (ci < 0 && cl_nlx_top > 0)
                    cl_nlx_top--;
            }
            VM_BREAK;
        }

        VM_CASE(OP_UWPROT): {
            int32_t uwp_offset = read_i32(code, &ip);
            CL_NLXFrame *nlx;

            if (cl_nlx_top >= cl_nlx_max) {
                    cl_nlx_overflow_summary("vm");
                    cl_error(CL_ERR_OVERFLOW, "NLX stack overflow");
                }

            /* No setjmp here (see the shared landing): the frame points at
             * this activation's landing, which — for an UWPROT frame —
             * updates the saved-pending slot below and jumps to the cleanup. */
            VM_NLX_ARM();
            nlx = &cl_nlx_stack[cl_nlx_top];
            VM_NLX_FRAME_INIT(nlx, CL_NLX_UWPROT, CL_NIL, uwp_offset);

            /* Save and clear pending throw state so that a nested
             * unwind-protect inside the cleanup cannot clobber the outer
             * non-local transfer. */
            if (cl_saved_pending_top >= cl_saved_pending_max)
                cl_error(CL_ERR_OVERFLOW, "saved-pending stack overflow");
            {
                CL_SavedPending *sp = &cl_saved_pending_stack[cl_saved_pending_top++];
                int pt = cl_pending_throw;
                sp->pending_throw    = pt;
                /* GC SAFETY (audit tier 4): with no throw in flight the
                 * tag/value globals still hold the last COMPLETED throw's
                 * objects.  This parking slot is deliberately skipped by
                 * the GC walks while pending_throw==0 (it may hold
                 * garbage), so parking those live offsets here lets the
                 * cleanup body's compactions move the objects out from
                 * under the copies; UWRETHROW then restores STALE offsets
                 * into the always-marked cl_pending_tag/value — the next
                 * mark walk follows them into arbitrary object interiors
                 * and ORs mark bits mid-object (observed: a GF dispatch
                 * cache's bucket array → circular chain → GC spins
                 * forever).  Park NILs instead; the values are
                 * meaningless without an armed throw.  The 512-byte error
                 * message is likewise only meaningful for a pending ERROR
                 * (pt == 2) — copying it on every entry was the single
                 * most expensive thing an UNWIND-PROTECT did. */
                if (pt) {
                    sp->pending_tag      = cl_pending_tag;
                    sp->pending_value    = cl_pending_value;
                    sp->pending_mv_count = cl_pending_mv_count;
                    { int _mi; for (_mi = 0; _mi < cl_pending_mv_count && _mi < CL_MAX_MV; _mi++)
                        sp->pending_mv_values[_mi] = cl_pending_mv_values[_mi]; }
                    sp->pending_error_code = cl_pending_error_code;
                    if (pt == 2) {
                        strncpy(sp->pending_error_msg, cl_pending_error_msg,
                                sizeof(sp->pending_error_msg) - 1);
                        sp->pending_error_msg[sizeof(sp->pending_error_msg) - 1] = '\0';
                    } else {
                        sp->pending_error_msg[0] = '\0';
                    }
                    cl_pending_throw    = 0;
                    cl_pending_tag      = CL_NIL;
                    cl_pending_value    = CL_NIL;
                    cl_pending_mv_count = 0;
                    cl_pending_error_code = 0;
                    cl_pending_error_msg[0] = '\0';
                } else {
                    sp->pending_tag      = CL_NIL;
                    sp->pending_value    = CL_NIL;
                    sp->pending_mv_count = 0;
                    sp->pending_error_code = 0;
                    sp->pending_error_msg[0] = '\0';
                }
                sp->entered_via_longjmp = 0; /* set to 1 by the landing if triggered */
            }
            /* The record above is part of this frame's dynamic extent: a
             * transfer to this frame must find it, not drop it. */
            nlx->saved_pending_mark = cl_saved_pending_top;
            cl_nlx_top++;

#ifdef CL_DEBUG_UWP
            if (frame->n_locals >= 1 &&
                cl_vm.stack[frame->bp] != CL_NIL &&
                cl_vm.stack[frame->bp] != CL_T &&
                cl_vm.fp >= 88 && dbg_watch_idx < 0) {
                dbg_watch_idx = frame->bp;
                dbg_watch_orig = cl_vm.stack[frame->bp];
                dbg_watch_nlx = cl_nlx_top - 1;
                fprintf(stderr, "[UWP-WATCH] armed on stack[%d]=0x%x fp=%d nlx=%d\n",
                        frame->bp, (unsigned)cl_vm.stack[frame->bp], cl_vm.fp, dbg_watch_nlx);
            }
#endif
            VM_BREAK;
        }

        VM_CASE(OP_UWPOP): {
            /* Normal exit from protected form — pop NLX frame.
             * Must find the matching UWPROT frame, as tail calls in called
             * functions may have leaked BLOCK/CATCH frames above it.
             * NOTE: we do NOT clear cl_pending_throw here; OP_UWPROT's
             * arming already cleared it, and OP_UWRETHROW restores it. */
            {
                int uwi;
                for (uwi = cl_nlx_top - 1; uwi >= 0; uwi--) {
                    if (cl_nlx_stack[uwi].type == CL_NLX_UWPROT) {
                        cl_nlx_top = uwi;
                        break;
                    }
                }
                if (uwi < 0 && cl_nlx_top > 0)
                    cl_nlx_top--;  /* fallback: pop whatever is on top */
            }
            VM_BREAK;
        }

        VM_CASE(OP_UWRETHROW): {
            /* After cleanup forms: pop our saved pending snapshot (pushed at
             * OP_UWPROT arming) and re-initiate pending throw/error if any.
             *
             * A UWRETHROW re-initiates a pending NLX in two situations:
             *   1. A new NLX was initiated during the cleanup body itself
             *      (cl_pending_throw != 0 at entry — rethrow_active).
             *   2. Our own UWPROT's longjmp was triggered (entered_via_longjmp=1
             *      in the saved entry) AND after restoring the saved state the
             *      pending NLX is still live. This handles the case where a nested
             *      CATCH inside the cleanup consumed an intermediate throw, clearing
             *      cl_pending_throw, even though the outer NLX (the one that armed
             *      our UWPROT via longjmp) still needs to be re-initiated.
             *
             * If neither condition holds — our UWPROT entered normally and no new
             * NLX fired during cleanup — leave the restored pending state parked in
             * the globals. The enclosing UWP's UWRETHROW (or the next NLX search
             * point) will see it. Re-initiating it here would skip cleanup code in
             * the enclosing UWP that runs after our call site. */
            int rethrow_active = (cl_pending_throw != 0);
            int should_rethrow = 0;
            {
                CL_SavedPending saved;
                if (cl_saved_pending_top > 0) {
                    saved = cl_saved_pending_stack[--cl_saved_pending_top];
                } else {
                    /* Should not happen; initialize to zero for safety. */
                    saved.pending_throw = 0;
                    saved.pending_tag = CL_NIL;
                    saved.pending_value = CL_NIL;
                    saved.pending_mv_count = 0;
                    saved.pending_error_code = 0;
                    saved.pending_error_msg[0] = '\0';
                    saved.entered_via_longjmp = 0;
                }
                if (cl_pending_throw == 0) {
                    /* Cleanup body completed without a new NLX — restore the
                     * pending state that was active before UWPROT armed. */
                    cl_pending_throw      = saved.pending_throw;
                    cl_pending_tag        = saved.pending_tag;
                    cl_pending_value      = saved.pending_value;
                    cl_pending_mv_count   = saved.pending_mv_count;
                    { int _mi; for (_mi = 0; _mi < saved.pending_mv_count && _mi < CL_MAX_MV; _mi++)
                        cl_pending_mv_values[_mi] = saved.pending_mv_values[_mi]; }
                    cl_pending_error_code = saved.pending_error_code;
                    strncpy(cl_pending_error_msg, saved.pending_error_msg,
                            sizeof(cl_pending_error_msg) - 1);
                    cl_pending_error_msg[sizeof(cl_pending_error_msg) - 1] = '\0';
                }
                /* If cl_pending_throw != 0 a new NLX was initiated during the
                 * cleanup; fall through to the rethrow logic below using the
                 * new (current) pending state, discarding `saved`. */
                should_rethrow = rethrow_active ||
                                 (saved.entered_via_longjmp && cl_pending_throw != 0);
            }
            if (should_rethrow && cl_pending_throw == 1) {
                /* Re-throw: find matching catch or block */
                CL_Obj ptag = cl_pending_tag;
                CL_Obj pval = cl_pending_value;
                int i;

                for (i = cl_nlx_top - 1; i >= cl_nlx_floor; i--) {
                    if ((cl_nlx_stack[i].type == CL_NLX_CATCH ||
                         cl_nlx_stack[i].type == CL_NLX_BLOCK ||
                         cl_nlx_stack[i].type == CL_NLX_TAGBODY) &&
                        cl_nlx_stack[i].tag == ptag) {
                        /* Check for interposing UWPROT (skip stale ones) */
                        int j;
                        for (j = cl_nlx_top - 1; j > i; j--) {
                            if (cl_nlx_stack[j].type == CL_NLX_UWPROT &&
                                !nlx_frame_is_stale(&cl_nlx_stack[j])) {
                                /* Jump to interposing UWPROT first */
                                cl_nlx_top = j;
                                VM_NLX_GOTO(&cl_nlx_stack[j]);
                            }
                        }
                        /* No interposing UWPROT, go directly to target */
                        cl_pending_throw = 0;
                        cl_nlx_stack[i].result = pval;
                        cl_nlx_stack[i].mv_count = cl_pending_mv_count;
                        { int mi; for (mi = 0; mi < cl_pending_mv_count && mi < CL_MAX_MV; mi++)
                            cl_nlx_stack[i].mv_values[mi] = cl_pending_mv_values[mi]; }
                        cl_nlx_top = i;
                        VM_NLX_GOTO(&cl_nlx_stack[i]);
                    }
                }
                /* No catch found — signal error */
                cl_pending_throw = 0;
                cl_error(CL_ERR_GENERAL, "No catch for tag during re-throw");
            } else if (should_rethrow && cl_pending_throw == 3) {
                /* Re-initiate a HANDLER-CASE clause transfer: the target
                 * frame index travels in cl_pending_tag (a fixnum — see
                 * cl_handler_case_transfer), the condition in
                 * cl_pending_value; hc_clause is already set in the frame. */
                int i = (int)CL_FIXNUM_VAL(cl_pending_tag);
                int j;
                if (i < cl_nlx_floor || i >= cl_nlx_top ||
                    cl_nlx_stack[i].type != CL_NLX_HANDLER_CASE) {
                    cl_pending_throw = 0;
                    cl_error(CL_ERR_GENERAL,
                             "HANDLER-CASE frame vanished during re-throw");
                }
                for (j = cl_nlx_top - 1; j > i; j--) {
                    if (cl_nlx_stack[j].type == CL_NLX_UWPROT &&
                        !nlx_frame_is_stale(&cl_nlx_stack[j])) {
                        cl_nlx_top = j;
                        VM_NLX_GOTO(&cl_nlx_stack[j]);
                    }
                }
                cl_pending_throw = 0;
                cl_nlx_stack[i].result = cl_pending_value;
                cl_nlx_stack[i].mv_count = 1;
                cl_nlx_stack[i].mv_values[0] = cl_pending_value;
                cl_nlx_top = i;
                VM_NLX_GOTO(&cl_nlx_stack[i]);
            } else if (should_rethrow && cl_pending_throw == 2) {
                /* Re-throw error: find interposing UWPROT or error frame (skip stale) */
                int i;
                for (i = cl_nlx_top - 1; i >= cl_nlx_floor; i--) {
                    if (cl_nlx_stack[i].type == CL_NLX_UWPROT &&
                        !nlx_frame_is_stale(&cl_nlx_stack[i])) {
                        cl_nlx_top = i;
                        VM_NLX_GOTO(&cl_nlx_stack[i]);
                    }
                }
                /* No more UWPROT — propagate to error handler.
                 * Restore VM state to before this cl_vm_eval call
                 * so subsequent evaluations start from a clean state,
                 * then longjmp via cl_error_frame_longjmp, which applies
                 * the per-frame snapshot restores (gc roots, jit depth,
                 * FASL readers, compiler chain, handler/restart tops) the
                 * bare longjmp here used to skip — a rethrow through an
                 * unwind-protect cleanup otherwise left gc_root_count
                 * pointing at unwound C stack slots. */
                {
                    int err_code = cl_pending_error_code;
                    cl_pending_throw = 0;
                    cl_vm.fp = base_fp;
                    cl_vm.sp = cl_vm.frames[base_fp].bp;
                    cl_error_code = err_code;
                    strncpy(cl_error_msg, cl_pending_error_msg, sizeof(cl_error_msg) - 1);
                    cl_error_msg[sizeof(cl_error_msg) - 1] = '\0';
                    /* Don't decrement here — CL_UNCATCH at the catch site pops */
                    cl_error_frame_longjmp(err_code);
                }
            }
            /* !should_rethrow: nop — pending state parked for enclosing UWP */
            VM_BREAK;
        }

        VM_CASE(OP_MV_LOAD): {
            uint8_t index = code[ip++];
            cl_vm_push((int)index < cl_mv_count ? cl_mv_values[index] : CL_NIL);
            VM_BREAK;
        }

        VM_CASE(OP_MV_TO_LIST): {
            /* Pops primary from stack, builds list from MV buffer.
             * For single values (inline opcodes), uses the popped primary.
             *
             * cl_mv_count == 0 normally means (values) — no values, empty
             * list.  But many value-propagating opcodes (OP_LOAD, OP_DUP,
             * OP_POP, …) don't reset cl_mv_count, so a non-NIL primary on
             * the stack with cl_mv_count == 0 indicates a stale leak from
             * an earlier (values) call rather than a true zero-value
             * return.  Treat non-NIL primary with count 0 as a single
             * value to preserve the value through (multiple-value-list …)
             * and unwind-protect's MV round-trip.  Legitimate (values)
             * keeps primary=NIL, so the empty-list case still fires for
             * it. */
            CL_Obj primary = cl_vm_pop();
            CL_Obj list = CL_NIL;
            if (cl_mv_count == 0) {
                if (!CL_NULL_P(primary))
                    list = cl_cons(primary, CL_NIL);
            } else if (cl_mv_count == 1) {
                list = cl_cons(primary, CL_NIL);
            } else {
                int i;
                for (i = cl_mv_count - 1; i >= 0; i--)
                    list = cl_cons(cl_mv_values[i], list);
            }
            cl_vm_push(list);
            cl_mv_count = 1;
            VM_BREAK;
        }

        VM_CASE(OP_MV_SAVE): {
            /* UNWIND-PROTECT value passing (vm.h): pop the primary and park
             * it with the rest of the MV buffer as one record on the
             * per-thread save stack — values first, then the count as a
             * fixnum on top.  No list, no local slot.  The record survives
             * the cleanup forms (which clobber the MV buffer) and is popped
             * by OP_MV_RESTORE; the landing drops it on a non-local exit. */
            CL_Obj primary = cl_vm_pop();
            int n = cl_mv_count;
            int i;
            if (n < 0) n = 0;
            if (n > CL_MAX_MV) n = CL_MAX_MV;
            if (thr->mv_save_top + n + 1 > CL_MV_SAVE_SIZE)
                cl_error(CL_ERR_OVERFLOW,
                         "UNWIND-PROTECT value-save stack overflow");
            if (n > 0) {
                thr->mv_save_buf[thr->mv_save_top++] = primary;
                for (i = 1; i < n; i++)
                    thr->mv_save_buf[thr->mv_save_top++] = cl_mv_values[i];
            }
            thr->mv_save_buf[thr->mv_save_top++] = CL_MAKE_FIXNUM(n);
            VM_BREAK;
        }

        VM_CASE(OP_MV_RESTORE): {
            /* Pop the record back into the MV buffer; push the primary. */
            int n, i;
            if (thr->mv_save_top < 1)
                cl_error(CL_ERR_GENERAL,
                         "OP_MV_RESTORE: value-save stack underflow");
            n = (int)CL_FIXNUM_VAL(thr->mv_save_buf[--thr->mv_save_top]);
            if (n < 0 || n > CL_MAX_MV || thr->mv_save_top < n)
                cl_error(CL_ERR_GENERAL,
                         "OP_MV_RESTORE: corrupt value-save record");
            thr->mv_save_top -= n;
            for (i = 0; i < n; i++)
                cl_mv_values[i] = thr->mv_save_buf[thr->mv_save_top + i];
            cl_mv_count = n;
            cl_vm_push(n > 0 ? cl_mv_values[0] : CL_NIL);
            VM_BREAK;
        }

        VM_CASE(OP_NTH_VALUE): {
            /* Stack: [index] [primary]  (primary on top)
             * Pops primary, pops index, pushes result. */
            CL_Obj primary = cl_vm_pop();
            CL_Obj idx_obj = cl_vm_pop();
            int idx;
            if (!CL_FIXNUM_P(idx_obj))
                cl_error(CL_ERR_TYPE, "NTH-VALUE: index must be a number");
            idx = CL_FIXNUM_VAL(idx_obj);
            if (idx == 0)
                cl_vm_push(primary);
            else
                cl_vm_push(idx > 0 && idx < cl_mv_count ? cl_mv_values[idx] : CL_NIL);
            cl_mv_count = 1;
            VM_BREAK;
        }

        VM_CASE(OP_DYNBIND): {
            uint16_t idx = read_u16(code, &ip);
            CL_Obj sym = constants[idx];
            CL_Obj new_val = cl_vm_pop();
            CL_Thread *thr = CT;
            CL_Obj old_tlv = cl_tlv_get(thr, sym);
            if (cl_dyn_top >= CL_MAX_DYN_BINDINGS)
                cl_error(CL_ERR_OVERFLOW, "Dynamic binding stack overflow");
            cl_dyn_stack[cl_dyn_top].symbol = sym;
            cl_dyn_stack[cl_dyn_top].old_value = old_tlv; /* CL_TLV_ABSENT if none */
            cl_dyn_top++;
            cl_tlv_set(thr, sym, new_val);
            if (sym == SYM_STAR_PACKAGE)
                cl_sync_current_package_from_dynamic();
            VM_BREAK;
        }

        VM_CASE(OP_DYNUNBIND): {
            uint8_t count = code[ip++];
            cl_dynbind_restore_to(cl_dyn_top - count);
            VM_BREAK;
        }

        VM_CASE(OP_MV_RESET):
            cl_mv_count = 1;
            VM_BREAK;

        VM_CASE(OP_LOAD_STRUCT_REF): {    /* LOAD s; STRUCT_REF i */
            uint8_t slot = code[ip++];
            cl_vm_push(cl_vm.stack[frame->bp + slot]);
            /* FALLTHROUGH into OP_STRUCT_REF: its u8 index is next */
        }
        VM_CASE(OP_STRUCT_REF): {
            uint8_t idx = code[ip++];
            CL_Obj obj = cl_vm_pop();
            CL_Struct *st;
            /* Error paths sync the frame's ip first, so the backtrace names
             * this form's line, not the previous call's. */
            if (!CL_STRUCT_P(obj)) {
                frame->ip = ip;
                cl_signal_type_error(obj, "STRUCTURE", "%STRUCT-REF");
            }
            st = (CL_Struct *)CL_OBJ_TO_PTR(obj);
            if ((uint32_t)idx >= st->n_slots) {
                frame->ip = ip;
                cl_error(CL_ERR_ARGS,
                         "%%STRUCT-REF: index %u out of range (n_slots=%u)",
                         (unsigned)idx, (unsigned)st->n_slots);
            }
            cl_vm_push(st->slots[idx]);
            cl_mv_count = 1;
            VM_BREAK;
        }

        VM_CASE(OP_STRUCT_SET): {
            uint8_t idx = code[ip++];
            CL_Obj val = cl_vm_pop();
            CL_Obj obj = cl_vm_pop();
            CL_Struct *st;
            if (!CL_STRUCT_P(obj)) {
                frame->ip = ip;
                cl_signal_type_error(obj, "STRUCTURE", "%STRUCT-SET");
            }
            st = (CL_Struct *)CL_OBJ_TO_PTR(obj);
            if ((uint32_t)idx >= st->n_slots) {
                frame->ip = ip;
                cl_error(CL_ERR_ARGS,
                         "%%STRUCT-SET: index %u out of range (n_slots=%u)",
                         (unsigned)idx, (unsigned)st->n_slots);
            }
            /* Publication barrier: this store may make a freshly-built
             * object graph reachable to peer threads that read the slot
             * WITHOUT a lock (CLOS dispatch metadata — gf-methods,
             * dispatch-cache, inline-cache — is lock-free on the reader
             * side by design).  On weakly-ordered CPUs (arm64 macOS) a
             * plain store can become visible before the stores that
             * initialized the object it points to, so a peer would read
             * uninitialized cons/struct/hashtable innards (observed as a
             * once-per-tens-of-millions %STRUCT-REF type error, or a hang
             * walking a half-visible hash table).  The full barrier orders
             * every prior initializing store before the publishing store;
             * readers are ordered by address dependency and need nothing.
             * On single-core m68k this is only a compiler barrier (free);
             * x86 TSO never reorders stores, so it is belt-and-braces
             * there. */
            if (CL_MT()) platform_memory_barrier();
            st->slots[idx] = val;
            cl_vm_push(val);
            cl_mv_count = 1;
            VM_BREAK;
        }

        VM_CASE(OP_AMIGA_CALL): {
            uint16_t sym_idx = read_u16(code, &ip);
            int16_t  off     = read_i16(code, &ip);
            uint32_t regspec = (uint32_t)read_i32(code, &ip);
            uint8_t  n_args  = code[ip++];
            CL_Obj base_sym, result;

            CL_SAFEPOINT();
            if (!constants)
                cl_error(CL_ERR_GENERAL,
                         "OP_AMIGA_CALL with NULL constants ptr");
            base_sym = constants[sym_idx];

            /* Args sit at [sp-n_args .. sp-1]; pop after dispatch.  The
             * base-variable checks and the trampoline are shared with the
             * CL_STUB_LIBCALL path (cl_amiga_call_via_base_sym).  Save ip
             * first: a rejected argument or an unopened library is then
             * reported at THIS call's line -- without it the backtrace
             * showed the line of the previous bytecode call (the clicktab
             * example's "line 134" for an error on line 135). */
            frame->ip = ip;
            result = cl_amiga_call_via_base_sym(
                base_sym, off, regspec, (int)n_args,
                &cl_vm.stack[cl_vm.sp - n_args]);
            cl_vm.sp -= n_args;
            cl_vm_push(result);
            cl_mv_count = 1;
            VM_BREAK;
        }

        VM_CASE(OP_RPLACA): {
            CL_Obj new_car = cl_vm_pop();
            CL_Obj cons_obj = cl_vm_pop();
            CL_Cons *cell;
            if (!CL_CONS_P(cons_obj))
                cl_error(CL_ERR_TYPE, "RPLACA: not a cons");
            cell = (CL_Cons *)CL_OBJ_TO_PTR(cons_obj);
            cell->car = new_car;
            cl_vm_push(new_car);
            cl_mv_count = 1;
            VM_BREAK;
        }

        VM_CASE(OP_RPLACD): {
            CL_Obj new_cdr = cl_vm_pop();
            CL_Obj cons_obj = cl_vm_pop();
            CL_Cons *cell;
            if (!CL_CONS_P(cons_obj))
                cl_error(CL_ERR_TYPE, "RPLACD: not a cons");
            cell = (CL_Cons *)CL_OBJ_TO_PTR(cons_obj);
            cell->cdr = new_cdr;
            cl_vm_push(new_cdr);
            cl_mv_count = 1;
            VM_BREAK;
        }

        VM_CASE(OP_ASET): {
            CL_Obj val = cl_vm_pop();
            CL_Obj idx_obj = cl_vm_pop();
            CL_Obj vec_obj = cl_vm_pop();
            int32_t idx;
            if (!CL_FIXNUM_P(idx_obj))
                cl_error(CL_ERR_TYPE, "ASET: index must be a number");
            idx = CL_FIXNUM_VAL(idx_obj);
            if (CL_BIT_VECTOR_P(vec_obj)) {
                CL_BitVector *bv = (CL_BitVector *)CL_OBJ_TO_PTR(vec_obj);
                int32_t v;
                if (idx < 0 || (uint32_t)idx >= cl_bv_active_length(bv))
                    cl_error(CL_ERR_ARGS, "ASET: index %d out of range", (int)idx);
                if (!CL_FIXNUM_P(val))
                    cl_error(CL_ERR_TYPE, "ASET: value must be 0 or 1 for bit vector");
                v = CL_FIXNUM_VAL(val);
                if (v != 0 && v != 1)
                    cl_error(CL_ERR_TYPE, "ASET: value must be 0 or 1 for bit vector");
                cl_bv_set_bit(bv, (uint32_t)idx, v);
            } else if (CL_ANY_STRING_P(vec_obj)) {
                uint32_t slen = cl_string_length(vec_obj);
                if (idx < 0 || (uint32_t)idx >= slen)
                    cl_error(CL_ERR_ARGS, "ASET: index %d out of range (0-%lu)",
                             (int)idx, (unsigned long)(slen - 1));
                if (!CL_CHAR_P(val))
                    cl_error(CL_ERR_TYPE, "ASET: value must be a character for string");
                cl_string_set_char_at(vec_obj, (uint32_t)idx, CL_CHAR_VAL(val));
            } else if (CL_BYTE_VECTOR_P(vec_obj)) {
                CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(vec_obj);
                if (idx < 0 || (uint32_t)idx >= bv->length)
                    cl_error(CL_ERR_ARGS, "ASET: index %d out of range", (int)idx);
                cl_bytevec_set(bv, idx,
                               cl_bytevec_check_value(val, bv->is_signed,
                                                      bv->elt_shift,
                                                      "ASET on a byte vector"));
            } else if (CL_VECTOR_P(vec_obj)) {
                CL_Vector *vec = (CL_Vector *)CL_OBJ_TO_PTR(vec_obj);
                if (idx < 0 || (uint32_t)idx >= vec->length)
                    cl_error(CL_ERR_ARGS, "ASET: index %d out of range (0-%lu)",
                             (int)idx, (unsigned long)(vec->length - 1));
                cl_vector_data(vec)[idx] = val;
            } else {
                cl_error(CL_ERR_TYPE, "ASET: not a vector");
            }
            cl_vm_push(val);
            cl_mv_count = 1;
            VM_BREAK;
        }

        VM_DEFAULT:
#ifndef USE_COMPUTED_GOTO
        unknown_opcode:
#endif
        {
            CL_Bytecode *dbg_bc;
            uint8_t dbg_type = CL_HDR_TYPE(CL_OBJ_TO_PTR(frame->bytecode));
            if (dbg_type == TYPE_CLOSURE) {
                CL_Closure *cl = (CL_Closure *)CL_OBJ_TO_PTR(frame->bytecode);
                dbg_bc = (CL_Bytecode *)CL_OBJ_TO_PTR(cl->bytecode);
                fprintf(stderr, "[VM] Unknown opcode 0x%02x at ip=%u (CLOSURE 0x%x -> BC 0x%x)\n",
                        op, ip - 1, (unsigned)frame->bytecode,
                        (unsigned)cl->bytecode);
            } else {
                dbg_bc = (CL_Bytecode *)CL_OBJ_TO_PTR(frame->bytecode);
                fprintf(stderr, "[VM] Unknown opcode 0x%02x at ip=%u (BYTECODE 0x%x)\n",
                        op, ip - 1, (unsigned)frame->bytecode);
            }
            fprintf(stderr, "[VM]   code_len=%u, code=%p\n",
                    dbg_bc->code_len, (void *)dbg_bc->code);
            if (CL_SYMBOL_P(dbg_bc->name))
                fprintf(stderr, "[VM]   name: %s\n",
                        cl_symbol_name(dbg_bc->name));
            if (dbg_bc->source_file)
                fprintf(stderr, "[VM]   source: %s:%u\n",
                        dbg_bc->source_file, dbg_bc->source_line);
            /* Print surrounding bytes from frame->code using code_len as limit */
            if (dbg_bc->code_len > 0) {
                uint32_t start = (ip > 10) ? ip - 10 : 0;
                uint32_t end = (ip + 10 < dbg_bc->code_len) ? ip + 10 : dbg_bc->code_len;
                uint32_t j;
                fprintf(stderr, "[VM]   bytes around ip=%u:", ip - 1);
                for (j = start; j < end; j++)
                    fprintf(stderr, " %02x", frame->code[j]);
                fprintf(stderr, "\n");
            }
            /* Dump all VM frames */
            {
                int fi;
                fprintf(stderr, "[VM]   fp=%d sp=%d base_fp=%d\n",
                        cl_vm.fp, cl_vm.sp, base_fp);
                for (fi = cl_vm.fp - 1; fi >= 0; fi--) {
                    CL_Frame *df = &cl_vm.frames[fi];
                    CL_Bytecode *dbc;
                    uint8_t ft = CL_HDR_TYPE(CL_OBJ_TO_PTR(df->bytecode));
                    if (ft == TYPE_CLOSURE) {
                        CL_Closure *dcl = (CL_Closure *)CL_OBJ_TO_PTR(df->bytecode);
                        dbc = (CL_Bytecode *)CL_OBJ_TO_PTR(dcl->bytecode);
                    } else {
                        dbc = (CL_Bytecode *)CL_OBJ_TO_PTR(df->bytecode);
                    }
                    fprintf(stderr, "[VM]   frame[%d] ip=%u/%u bp=%u",
                            fi, df->ip, dbc->code_len, df->bp);
                    if (CL_SYMBOL_P(dbc->name))
                        fprintf(stderr, " %s", cl_symbol_name(dbc->name));
                    fprintf(stderr, "\n");
                }
            }
            vm_trace_dump();
            cl_error(CL_ERR_GENERAL, "Unknown opcode: 0x%02x at ip=%u",
                     op, ip - 1);
            return CL_NIL;
        }
#ifndef USE_COMPUTED_GOTO
        }  /* end switch */
    }  /* end for */
#endif

/* end of inline hot-path macros */
#undef cl_vm_push
#undef cl_vm_pop
#undef cl_vm
#define cl_vm (CT->vm)
#undef cl_mv_count
#define cl_mv_count (CT->mv_count)
#undef cl_mv_values
#define cl_mv_values (CT->mv_values)
#undef cl_nlx_stack
#define cl_nlx_stack (CT->nlx_stack)
#undef cl_nlx_top
#define cl_nlx_top (CT->nlx_top)
#undef cl_nlx_max
#define cl_nlx_max (CT->nlx_max)
#undef cl_nlx_floor
#define cl_nlx_floor (CT->nlx_floor)
#undef cl_dyn_top
#define cl_dyn_top (CT->dyn_top)
#undef cl_handler_stack
#define cl_handler_stack (CT->handler_stack)
#undef cl_handler_top
#define cl_handler_top (CT->handler_top)
#undef cl_handler_active_mask
#define cl_handler_active_mask (CT->handler_active_mask)
#undef cl_restart_stack
#define cl_restart_stack (CT->restart_stack)
#undef cl_restart_top
#define cl_restart_top (CT->restart_top)
#undef cl_error_frame_top
#define cl_error_frame_top (CT->error_frame_top)
#undef gc_root_count
#define gc_root_count (CT->gc_root_count)
#undef cl_saved_pending_stack
#define cl_saved_pending_stack (CT->saved_pending_stack)
#undef cl_saved_pending_top
#define cl_saved_pending_top (CT->saved_pending_top)
#undef cl_saved_pending_max
#define cl_saved_pending_max (CT->saved_pending_max)
#undef cl_pending_throw
#define cl_pending_throw (CT->pending_throw)
#undef cl_pending_tag
#define cl_pending_tag (CT->pending_tag)
#undef cl_pending_value
#define cl_pending_value (CT->pending_value)
#undef cl_pending_error_code
#define cl_pending_error_code (CT->pending_error_code)
#undef cl_pending_error_msg
#define cl_pending_error_msg (CT->pending_error_msg)
#undef cl_pending_mv_count
#define cl_pending_mv_count (CT->pending_mv_count)
#undef cl_pending_mv_values
#define cl_pending_mv_values (CT->pending_mv_values)
#undef cl_trace_count
#define cl_trace_count (CT->trace_count)
#undef cl_trace_depth
#define cl_trace_depth (CT->trace_depth)
#undef cl_pre_call_mv_count
#define cl_pre_call_mv_count (CT->pre_call_mv_count)
#undef cl_pre_call_mv_values
#define cl_pre_call_mv_values (CT->pre_call_mv_values)

#undef VM_DISPATCH
#undef VM_CASE
#undef VM_BREAK
#undef VM_DEFAULT
#ifdef USE_COMPUTED_GOTO
#undef USE_COMPUTED_GOTO
#endif
}  /* end cl_vm_run */

/*
 * cl_vm_eval — execute a bytecode object.
 * Thin wrapper: pushes initial frame, calls cl_vm_run.
 */
CL_Obj cl_vm_eval(CL_Obj bytecode_obj)
{
    CL_Bytecode *bc;
    CL_Frame *frame;
    int base_fp, base_nlx, saved_sp;
    CL_Obj result;

    cl_check_c_stack("cl_vm_eval");
#ifdef DEBUG_VM
    vm_eval_depth++;
    if (vm_eval_depth > vm_eval_max_depth)
        vm_eval_max_depth = vm_eval_depth;
#endif

    if (CL_NULL_P(bytecode_obj)) return CL_NIL;

    /* Handle both bytecode and closure */
    if (CL_CLOSURE_P(bytecode_obj)) {
        CL_Closure *cl = (CL_Closure *)CL_OBJ_TO_PTR(bytecode_obj);
        bc = (CL_Bytecode *)CL_OBJ_TO_PTR(cl->bytecode);
    } else {
        bc = (CL_Bytecode *)CL_OBJ_TO_PTR(bytecode_obj);
    }

    if (!bc || !bc->code) return CL_NIL;

    /* Set up initial frame */
    base_fp = cl_vm.fp;
    saved_sp = cl_vm.sp;
    base_nlx = cl_nlx_top;
    if (cl_vm.fp >= cl_vm.frame_size)
        cl_error(CL_ERR_OVERFLOW, "VM frame stack overflow");
    frame = &cl_vm.frames[cl_vm.fp++];
    frame->bytecode = bytecode_obj;
    frame->code = bc->code;
    frame->constants = bc->constants;
    frame->ip = 0;
    frame->bp = cl_vm.sp;
    frame->n_locals = bc->n_locals;
    frame->nargs = 0;
    frame->nlx_level = cl_nlx_top;
    frame->fslot = 0;

    /* Allocate space for locals */
    {
        int i;
        for (i = 0; i < bc->n_locals; i++)
            cl_vm_push(CL_NIL);
    }

    /* Targeted crash diagnostic: cl_vm_eval path (disabled) */
#if 0
    if (bc->name != CL_NIL && CL_SYMBOL_P(bc->name)) {
        const char *_fn = cl_symbol_name(bc->name);
        if (_fn && _fn[0] == 'C' && _fn[1] == 'L' && _fn[2] == 'A' &&
            _fn[3] == 'S' && _fn[4] == 'S' && _fn[5] == '-' &&
            _fn[6] == 'S' && _fn[7] == 'L' && _fn[8] == 'O' &&
            _fn[9] == 'T') {
            fprintf(stderr, "[DIAG-EVAL] Entering %s: code=%p fp=%d sp=%d\n",
                    _fn, (void *)bc->code, cl_vm.fp, cl_vm.sp);
            fflush(stderr);
        }
    }
#endif

    result = cl_vm_run(base_fp, base_nlx);

    /* Restore fp/sp: OP_HALT doesn't decrement fp, so the eval frame
     * would leak.  Without this restore, each cl_vm_eval call leaves
     * fp one higher, corrupting the frame stack for the caller. */
    cl_vm.fp = base_fp;
    cl_vm.sp = saved_sp;

#ifdef DEBUG_VM
    vm_eval_depth--;
#endif
    return result;
}
