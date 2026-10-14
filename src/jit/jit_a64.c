/* jit_a64.c — the AArch64 native backend (specs/native-backend-a64.md).
 *
 * The walker runs the operand stack, the locals and control flow natively
 * and sends every opcode it has no template for to the C helper behind it
 * -- a shared one (runtime.c, runtime_nlx.c) where its semantics are the
 * interpreter's, else a VM-stack one (runtime_vmstack.c).
 *
 * Every lambda list compiles: the prologue sets up the interpreter's frame
 * (missing optionals NIL, the &rest list after the positional parameters,
 * the &key matches) -- inline for required and &optional parameters, through
 * cl_jit_vmstack_ll_prologue with &rest or &key -- and OP_ARGC, which the
 * compiler's &optional prologue tests, reads the entry's nargs from x24.
 *
 * Phase 3 adds the m68k walker's other opcodes: the NLX frames (BLOCK,
 * CATCH, TAGBODY, UNWIND-PROTECT, HANDLER-CASE: an alloc helper, _setjmp
 * called from the native function itself, a commit helper; the longjmp arm
 * restores and resumes at the landing), dynamic binding and PROGV, the
 * multiple-value opcodes, closures, cells and upvalues, the handler and
 * restart bindings, and the &key prologue.  And the native function's frame
 * is the interpreter's: cl_jit_invoke pushes its CL_Frame, and the code
 * keeps the frame's ip at the opcode it is in, so backtraces, line
 * attribution and FRAME see native frames as interpreted ones.
 *
 * Phase 2 adds inline fast paths -- fixnum arithmetic and comparisons,
 * CAR/CDR, struct slots, special-variable reads and writes -- each
 * checking its operands before it changes anything, so a failed check
 * jumps to an out-of-line call of the same helper phase 1 used (the slow
 * path, emitted after the body) and rejoins with the result in w0.  And a
 * top-of-stack cache: up to three operand-stack values live in w13-w15
 * instead of cl_vm.stack, written out ("flushed") before every helper call,
 * every branch and every label, so the frame is the interpreter's whenever
 * C code can see it.
 *
 * Entry ABI (the spec's "Entry and frame layout"): one C signature for
 * every arity, the arguments left on cl_vm.stack where the caller pushed
 * them, so no trampoline is needed:
 *
 *     CL_Obj entry(CL_Thread *thr, CL_Obj *bp, uint32_t nargs, CL_Obj func,
 *                  CL_Frame *frame)
 *
 * The frame is the interpreter's, in cl_vm.stack:
 *
 *     bp[0 .. arity-1]         the arguments
 *     bp[arity .. n_locals-1]  the other locals, NIL on entry
 *     bp[n_locals]             FUNC, the function value the frame was
 *                              entered with (OP_UPVAL's closure; the self
 *                              tail call's guard reads it)
 *     bp[n_locals+1 ..]        the operand stack
 *
 * Registers (callee-saved, so they survive every helper call):
 *     x19  the current CL_Thread *          x20  the operand-stack top
 *     x21  bp                               x22  bc->constants
 *     x23  cl_arena_base                    x24  nargs (OP_ARGC)
 *     x25  the CL_Frame (its ip)            x27  cl_vm.stack (the thread's)
 *     x28  scratch across a non-allocating call
 * Caller-saved: w13-w15 the top-of-stack cache (never live across a call),
 * w10/w11 the operands a fast path read from memory, w0 a fast path's
 * result, x9, x12 and x16/x17 scratch.  x18 (reserved on Apple) is never
 * used.
 *
 * GC ("Rule 1"): before every helper call the code flushes the cache and
 * writes cl_vm.sp from x20, so every live value is below sp -- a root both collectors forward in
 * place -- and holds no CL_Obj in a register across the call.  Constants
 * are read from bc->constants at each use (that array never moves and the
 * collectors forward it, minor collections included), T through the
 * address of the CL_T global.  So native code bakes no heap object: no
 * relocation table, nothing for a collection to patch.  The backend never
 * sets jit_depth / jit_stack_top -- the conservative native-stack scan of
 * the m68k JIT does not apply.
 */

#include "jit/jit.h"

#ifdef JIT_A64

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include "jit/jit_backend.h"
#include "jit/asm_a64.h"
#include "jit/codebuf.h"
#include "jit/codeheap.h"
#include "jit/runtime.h"
#include "jit/runtime_vmstack.h"
#include "jit/runtime_nlx.h"
#include "core/mem.h"        /* cl_call_gen_bump */
#include "core/opcodes.h"
#include "core/stream.h"     /* cl_write_cstring_to_stdout */
#include "core/symbol.h"     /* SYM_STAR_PACKAGE */
#include "core/thread.h"     /* cl_vm, cl_get_current_thread, CL_Thread */
#include "core/vm.h"         /* cl_vm_apply_list */
#include "platform/platform.h"
#include "platform/platform_thread.h"   /* platform_memory_barrier */
#include <setjmp.h>          /* _setjmp, called from native code (NLX frames) */

typedef CL_Obj (*a64_entry_t)(CL_Thread *thr, CL_Obj *bp, uint32_t nargs,
                              CL_Obj func, CL_Frame *frame);

/* Register roles (see the banner). */
#define R_THR   19
#define R_TOP   20
#define R_BP    21
#define R_K     22
#define R_ARENA 23
#define R_NARGS 24
#define R_FRAME 25
#define R_STK   27
#define R_KEEP  28

/* CL_Thread fields the code reads through x19.  Each must fit the scaled
 * unsigned-offset form of its access size. */
#define OFF_VM_STACK      ((uint32_t)offsetof(CL_Thread, vm.stack))
#define OFF_VM_STACK_SIZE ((uint32_t)offsetof(CL_Thread, vm.stack_size))
#define OFF_VM_SP         ((uint32_t)offsetof(CL_Thread, vm.sp))
#define OFF_MV_COUNT      ((uint32_t)offsetof(CL_Thread, mv_count))
#define OFF_LOOP_CTR      ((uint32_t)offsetof(CL_Thread, jit_loop_ctr))
#define OFF_TLV_COUNT     ((uint32_t)offsetof(CL_Thread, tlv_entry_count))
typedef char a64_off_vm_stack_fits[(offsetof(CL_Thread, vm.stack) % 8 == 0 &&
                                    offsetof(CL_Thread, vm.stack) < 32760) ? 1 : -1];
typedef char a64_off_words_fit[(offsetof(CL_Thread, vm.stack_size) % 4 == 0 &&
                                offsetof(CL_Thread, vm.sp) % 4 == 0 &&
                                offsetof(CL_Thread, mv_count) % 4 == 0 &&
                                offsetof(CL_Thread, mv_count) < 16380 &&
                                offsetof(CL_Thread, tlv_entry_count) % 4 == 0 &&
                                offsetof(CL_Thread, tlv_entry_count) < 16380) ? 1 : -1];
typedef char a64_off_loop_ctr_fits[(offsetof(CL_Thread, jit_loop_ctr) % 2 == 0 &&
                                    offsetof(CL_Thread, jit_loop_ctr) < 8190) ? 1 : -1];
typedef char a64_sp_is_int[(sizeof(((CL_Thread *)0)->vm.sp) == 4 &&
                            sizeof(((CL_Thread *)0)->mv_count) == 4 &&
                            sizeof(((CL_Thread *)0)->jit_loop_ctr) == 2 &&
                            sizeof(((CL_Thread *)0)->tlv_entry_count) == 4) ? 1 : -1];

/* Heap layouts the fast paths read (types.h). */
#define OFF_CAR        ((uint32_t)offsetof(CL_Cons, car))
#define OFF_CDR        ((uint32_t)offsetof(CL_Cons, cdr))
#define OFF_SYM_VALUE  ((uint32_t)offsetof(CL_Symbol, value))
#define OFF_ST_NSLOTS  ((uint32_t)offsetof(CL_Struct, n_slots))
#define OFF_ST_SLOTS   ((uint32_t)offsetof(CL_Struct, slots))
#define OFF_CELL_VALUE ((uint32_t)offsetof(CL_Cell, value))
#define OFF_CL_UPVALS  ((uint32_t)offsetof(CL_Closure, upvalues))
#define OFF_FRAME_IP   ((uint32_t)offsetof(CL_Frame, ip))
typedef char a64_frame_ip_fits[(offsetof(CL_Frame, ip) % 4 == 0 &&
                                offsetof(CL_Frame, ip) < 16380 &&
                                sizeof(((CL_Frame *)0)->ip) == 4) ? 1 : -1];
typedef char a64_cell_closure_layout[(offsetof(CL_Cell, value) == 4 &&
                                      offsetof(CL_Closure, upvalues) == 8) ? 1 : -1];
typedef char a64_heap_layout[(offsetof(CL_Cons, car) == 4 && offsetof(CL_Cons, cdr) == 8 &&
                              offsetof(CL_Symbol, value) == 8 &&
                              offsetof(CL_Struct, n_slots) == 8 &&
                              offsetof(CL_Struct, slots) == 12 &&
                              CL_HDR_TYPE_SHIFT == 24 && TYPE_CONS == 0 &&
                              CL_UNBOUND == (CL_Obj)0xFFFFFFF6u) ? 1 : -1];

/* What a direct call's hit path reads and writes (emit_direct_call): the
 * thread's C-stack base, frame stack and NLX depth, the callee's bytecode,
 * and the CL_Frame it pushes.  Thread fields past the scaled-offset range
 * go through emit_thr_ldst. */
#define OFF_C_STACK_BASE  ((uint32_t)offsetof(CL_Thread, c_stack_base))
#define OFF_VM_FRAMES     ((uint32_t)offsetof(CL_Thread, vm.frames))
#define OFF_VM_FRAME_SIZE ((uint32_t)offsetof(CL_Thread, vm.frame_size))
#define OFF_VM_FP         ((uint32_t)offsetof(CL_Thread, vm.fp))
#define OFF_NLX_TOP       ((uint32_t)offsetof(CL_Thread, nlx_top))
#define OFF_TAIL_PENDING  ((uint32_t)offsetof(CL_Thread, jit_tail_pending))
#define OFF_CL_BYTECODE   ((uint32_t)offsetof(CL_Closure, bytecode))
#define OFF_BC_CODE       ((uint32_t)offsetof(CL_Bytecode, code))
#define OFF_BC_CONSTANTS  ((uint32_t)offsetof(CL_Bytecode, constants))
#define OFF_BC_NLOCALS    ((uint32_t)offsetof(CL_Bytecode, n_locals))
#define OFF_BC_NATIVE     ((uint32_t)offsetof(CL_Bytecode, native_code))
#define OFF_F_BYTECODE    ((uint32_t)offsetof(CL_Frame, bytecode))
#define OFF_F_CODE        ((uint32_t)offsetof(CL_Frame, code))
#define OFF_F_CONSTANTS   ((uint32_t)offsetof(CL_Frame, constants))
#define OFF_F_BP          ((uint32_t)offsetof(CL_Frame, bp))
#define OFF_F_NLOCALS     ((uint32_t)offsetof(CL_Frame, n_locals))
#define OFF_F_NARGS       ((uint32_t)offsetof(CL_Frame, nargs))
#define OFF_F_FSLOT       ((uint32_t)offsetof(CL_Frame, fslot))
#define OFF_F_NLX         ((uint32_t)offsetof(CL_Frame, nlx_level))
typedef char a64_direct_layout[(sizeof(((CL_Thread *)0)->c_stack_base) == 8 &&
                                sizeof(((CL_Thread *)0)->vm.frames) == 8 &&
                                sizeof(((CL_Thread *)0)->vm.fp) == 4 &&
                                sizeof(((CL_Thread *)0)->vm.frame_size) == 4 &&
                                sizeof(((CL_Thread *)0)->nlx_top) == 4 &&
                                sizeof(((CL_Thread *)0)->jit_tail_pending) == 4 &&
                                offsetof(CL_Closure, bytecode) == 4 &&
                                sizeof(((CL_Bytecode *)0)->code) == 8 &&
                                sizeof(((CL_Bytecode *)0)->constants) == 8 &&
                                sizeof(((CL_Bytecode *)0)->n_locals) == 2 &&
                                sizeof(((CL_Bytecode *)0)->native_code) == 8 &&
                                sizeof(((CL_Frame *)0)->bytecode) == 4 &&
                                sizeof(((CL_Frame *)0)->code) == 8 &&
                                sizeof(((CL_Frame *)0)->constants) == 8 &&
                                sizeof(((CL_Frame *)0)->bp) == 4 &&
                                sizeof(((CL_Frame *)0)->n_locals) == 4 &&
                                sizeof(((CL_Frame *)0)->nargs) == 2 &&
                                sizeof(((CL_Frame *)0)->fslot) == 1 &&
                                sizeof(((CL_Frame *)0)->nlx_level) == 4 &&
                                sizeof(CL_Frame) < 4096 &&
                                TYPE_CLOSURE < 4096) ? 1 : -1];

/* The frame the prologue pushes: x29/x30, x19-x25, x27 and x28 (x26 pads
 * its pair). */
#define FRAME_BYTES 96

/* Walker limits: the operand encodings below assume them. */
#define A64_MAX_LOCALS 1000

void cl_jit_backend_init(void)
{
    char envbuf[16];
    const char *v;
    cl_codeheap_init();
    /* Native frames are the interpreter's (cl_jit_invoke): visible to the
     * debugger unless %JIT-SET-FRAMES turns them off. */
    cl_jit_set_shadow_frames(1);
    /* AMILISP_JIT_HOT=N: the hot-call threshold at boot (0 = eager, every
     * function compiled at definition) -- the differential runs of the
     * whole suite against the interpreter (tests/test_jit_a64_walk.sh). */
    v = platform_getenv("AMILISP_JIT_HOT", envbuf, sizeof(envbuf));
    if (v != NULL && v[0] >= '0' && v[0] <= '9')
        cl_jit_set_hot_threshold(atoi(v));
    /* AMILISP_JIT_DIRECT=0: no direct native-to-native calls (A/B runs,
     * bisection), as on m68k. */
    v = platform_getenv("AMILISP_JIT_DIRECT", envbuf, sizeof(envbuf));
    if (v != NULL && v[0] == '0')
        cl_jit_set_direct_calls(0);
}

void cl_jit_backend_shutdown(void)
{
    cl_codeheap_shutdown();
}

void cl_jit_free_native(void *code)
{
    cl_codeheap_free(code);
}

/* Drop BC's native code and invalidate every call site that might cache
 * it: unhook first, then bump, then free (the order jit_m68k.c uses). */
static void drop_native(CL_Bytecode *bc)
{
    void *old = bc->native_code;
    if (bc->native_relocs) platform_free(bc->native_relocs);
    bc->native_relocs = NULL;
    bc->native_reloc_count = 0;
    bc->native_code = NULL;
    bc->native_len = 0;
    if (old) {
        cl_call_gen_bump("native code replaced");
        cl_codeheap_free(old);
    }
}

/* --- Emitter state ------------------------------------------------------ */

/* The top-of-stack cache.  The top NC values of the operand stack live in
 * registers CR[0] (the deepest of them) .. CR[NC-1] (TOS) instead of at
 * x20 and above; x20 points past the last value in memory.  Which register
 * holds which entry is compile-time state only. */
#define NCACHE 3
static const int cache_regs[NCACHE] = { 13, 14, 15 };

typedef struct {
    int nc;
    int cr[NCACHE];
} VState;

/* A slow path's arguments: a register, an immediate, or a constant. */
#define SA_NONE  0
#define SA_REG   1
#define SA_IMM   2
#define SA_CONST 3
typedef struct {
    uint8_t  kind;
    uint32_t val;
} SArg;

/* An out-of-line slow path: FN called with ARGS from the cache state IN
 * (the state the fast path branched away from), then IN restored -- the
 * cached values reloaded from cl_vm.stack, since a collection may have
 * moved them -- and back to JOIN with the helper's result in w0. */
typedef struct {
    A64Label    entry, join;
    const void *fn;
    SArg        args[3];
    VState      in;
    int         mv1;          /* cl_mv_count was known to be 1 at the branch */
    uint32_t    ip;           /* its opcode's frame ip */
} Slow;

typedef struct {
    A64Asm    a;
    CodeBuf   cb;
    uint64_t *lit_val;        /* 64-bit literals (helper and data addresses) */
    A64Label *lit_label;
    uint32_t  n_lits, cap_lits;
    Slow     *slow;
    uint32_t  n_slow, cap_slow;
    VState    vs;             /* the cache at the point being emitted */
    int       mv1;            /* cl_mv_count is known to be 1 here */
    uint32_t  cur_ip;         /* the bytecode IP after the opcode being emitted */
    uint32_t  ip_known;       /* the frame's ip holds this ... */
    int       ip_valid;       /* ... on every path to here */
    int       oom;
    uint64_t *sites;          /* the direct-call cells, one per call site */
    uint32_t  n_sites, max_sites;
} Gen;

/* The label of the 64-bit literal V, appended after the code by
 * finish_literals; equal values share one slot. */
static A64Label lit_label(Gen *g, uint64_t v)
{
    uint32_t i;
    for (i = 0; i < g->n_lits; i++)
        if (g->lit_val[i] == v) return g->lit_label[i];
    if (g->n_lits == g->cap_lits) {
        uint32_t cap = g->cap_lits ? g->cap_lits * 2 : 16;
        uint64_t *nv = (uint64_t *)platform_alloc((unsigned long)cap * sizeof(uint64_t));
        A64Label *nl = (A64Label *)platform_alloc((unsigned long)cap * sizeof(A64Label));
        if (nv == NULL || nl == NULL) {
            if (nv) platform_free(nv);
            if (nl) platform_free(nl);
            g->oom = 1;
            return 0;
        }
        for (i = 0; i < g->n_lits; i++) { nv[i] = g->lit_val[i]; nl[i] = g->lit_label[i]; }
        if (g->lit_val) platform_free(g->lit_val);
        if (g->lit_label) platform_free(g->lit_label);
        g->lit_val = nv;
        g->lit_label = nl;
        g->cap_lits = cap;
    }
    g->lit_val[g->n_lits] = v;
    g->lit_label[g->n_lits] = a64_label_new(&g->a);
    return g->lit_label[g->n_lits++];
}

/* The literals, 8-byte aligned, after the last instruction. */
static void finish_literals(Gen *g)
{
    uint32_t i;
    if (g->n_lits == 0) return;
    if (cb_len(&g->cb) & 7) a64_emit(&g->a, a64_brk(0));   /* never reached */
    for (i = 0; i < g->n_lits; i++) {
        a64_bind(&g->a, g->lit_label[i]);
        cb_emit_u32_le(&g->cb, (uint32_t)g->lit_val[i]);
        cb_emit_u32_le(&g->cb, (uint32_t)(g->lit_val[i] >> 32));
    }
}

#define E(insn) a64_emit(&g->a, (insn))

static void emit_ldr_lit64(Gen *g, int rt, const void *addr)
{
    a64_ldr_lit(&g->a, 8, rt, lit_label(g, (uint64_t)(uintptr_t)addr));
}

/* xRD = xRN + IMM, through x17 when IMM does not encode. */
static void emit_add_x(Gen *g, int rd, int rn, uint32_t imm)
{
    uint32_t w = a64_add_imm(1, rd, rn, imm);
    if (w != A64_BAD) { E(w); return; }
    a64_mov_imm(&g->a, 1, 17, imm);
    E(a64_add_reg(1, rd, rn, 17));
}

static void emit_sub_x(Gen *g, int rd, int rn, uint32_t imm)
{
    uint32_t w = a64_sub_imm(1, rd, rn, imm);
    if (w != A64_BAD) { E(w); return; }
    a64_mov_imm(&g->a, 1, 17, imm);
    E(a64_sub_reg(1, rd, rn, 17));
}

/* cl_vm.sp = (x20 - x27) / 4: every value of the frame is now a root. */
static void emit_sync_sp(Gen *g)
{
    E(a64_sub_reg(1, 9, R_TOP, R_STK));
    E(a64_lsr_imm(1, 9, 9, 2));
    E(a64_str_uoff(4, 9, R_THR, OFF_VM_SP));
}

/* The frame's ip = the opcode being emitted, as the interpreter writes it
 * before a call or a signalling operation (the IP after the opcode), so a
 * backtrace names its line and a suspended native caller its call's.  The
 * frame is the CL_Frame cl_jit_invoke pushed (x25; a scratch one when
 * shadow frames are off).  Elided while the frame already holds it. */
static void emit_frame_ip(Gen *g)
{
    if (g->ip_valid && g->ip_known == g->cur_ip) return;
    a64_mov_imm(&g->a, 0, 9, g->cur_ip);
    E(a64_str_uoff(4, 9, R_FRAME, OFF_FRAME_IP));
    g->ip_known = g->cur_ip;
    g->ip_valid = 1;
}

/* Call the C function FN with the arguments already in x0-x7.  The cache
 * must be empty (vs_flush): Rule 1.  The helper may write cl_mv_count. */
static void emit_call(Gen *g, const void *fn)
{
    if (g->vs.nc != 0) g->a.bad = 1;        /* a walker bug: decline */
    emit_frame_ip(g);
    emit_sync_sp(g);
    emit_ldr_lit64(g, 16, fn);
    E(a64_blr(16));
    g->mv1 = 0;
}

static void emit_push(Gen *g, int r)   { E(a64_str_post(4, r, R_TOP, 4)); }
/* The K-th value from the top (1 = TOS), not popped. */
static void emit_peek(Gen *g, int r, uint32_t k)
{
    uint32_t w = a64_ldur(4, r, R_TOP, -4 * (int32_t)k);
    if (w != A64_BAD) { E(w); return; }
    emit_sub_x(g, 17, R_TOP, 4 * k);
    E(a64_ldr_uoff(4, r, 17, 0));
}
static void emit_poke(Gen *g, int r, uint32_t k)
{
    E(a64_stur(4, r, R_TOP, -4 * (int32_t)k));
}
static void emit_drop(Gen *g, uint32_t n)
{
    if (n) emit_sub_x(g, R_TOP, R_TOP, 4 * n);
}
static void emit_load_local(Gen *g, int r, uint32_t slot)
{
    E(a64_ldr_uoff(4, r, R_BP, 4 * slot));
}
static void emit_store_local(Gen *g, int r, uint32_t slot)
{
    E(a64_str_uoff(4, r, R_BP, 4 * slot));
}
static void emit_load_const(Gen *g, int r, uint32_t idx)
{
    uint32_t w = a64_ldr_uoff(4, r, R_K, 4 * idx);
    if (w != A64_BAD) { E(w); return; }
    a64_mov_imm(&g->a, 0, 17, 4 * idx);
    E(a64_ldr_w_uxtw(r, R_K, 17));
}
/* xRD = &bc->constants[IDX]. */
static void emit_const_addr(Gen *g, int rd, uint32_t idx)
{
    emit_add_x(g, rd, R_K, 4 * idx);
}
/* wR = T, read through the CL_T global (a collection may move the symbol). */
static void emit_load_t(Gen *g, int r)
{
    emit_ldr_lit64(g, r, &CL_T);
    E(a64_ldr_uoff(4, r, r, 0));
}
/* cl_mv_count = 1: the interpreter's write after a single-valued opcode.
 * Elided while the count is known to be 1 already: from one such write to
 * the next helper call or label (the only things that can change it). */
static void emit_mv1_store(Gen *g)
{
    E(a64_movz(0, 9, 1, 0));
    E(a64_str_uoff(4, 9, R_THR, OFF_MV_COUNT));
}
static void emit_mv1(Gen *g)
{
    if (g->mv1) return;
    emit_mv1_store(g);
    g->mv1 = 1;
}
/* The loop poll (spec "Rule 4"): count CL_Thread.jit_loop_ctr down and, on
 * the borrow, run cl_jit_runtime_loop_poll (safepoint, interrupt, Ctrl-C),
 * which resets the countdown. */
static void emit_loop_poll(Gen *g)
{
    A64Label skip = a64_label_new(&g->a);
    int valid = g->ip_valid;
    uint32_t known = g->ip_known;
    E(a64_ldr_uoff(2, 9, R_THR, OFF_LOOP_CTR));
    E(a64_subs_imm(0, 9, 9, 1));
    E(a64_str_uoff(2, 9, R_THR, OFF_LOOP_CTR));
    a64_bcond(&g->a, A64_HS, skip);
    emit_call(g, (const void *)&cl_jit_runtime_loop_poll);
    a64_bind(&g->a, skip);
    g->ip_valid = valid;            /* the call is not on every path */
    g->ip_known = known;
    if (valid && known != g->cur_ip) g->ip_valid = 0;
}

/* --- The top-of-stack cache ------------------------------------------ */

/* Write the deepest cached values to memory until KEEP are left cached. */
static void vs_spill(Gen *g, int keep)
{
    int n = g->vs.nc - keep, i;
    if (n <= 0) return;
    for (i = 0; i < n; i++) E(a64_str_post(4, g->vs.cr[i], R_TOP, 4));
    for (i = 0; i < keep; i++) g->vs.cr[i] = g->vs.cr[i + n];
    g->vs.nc = keep;
}
/* Every cached value to memory: the frame is the interpreter's again. */
static void vs_flush(Gen *g) { vs_spill(g, 0); }

/* The register holding the K-th value from the top (1 = TOS), loading it
 * into SCRATCH when it is in memory.  Changes no state. */
static int vs_reg(Gen *g, uint32_t k, int scratch)
{
    if (k <= (uint32_t)g->vs.nc) return g->vs.cr[g->vs.nc - (int)k];
    emit_peek(g, scratch, k - (uint32_t)g->vs.nc);
    return scratch;
}

/* Drop the top N values, cached ones first. */
static void vs_pop(Gen *g, uint32_t n)
{
    uint32_t m = n < (uint32_t)g->vs.nc ? n : (uint32_t)g->vs.nc;
    g->vs.nc -= (int)m;
    emit_drop(g, n - m);
}

/* A new TOS: the cache register to write it into (spilling the deepest
 * entry when all three are taken).  The caller writes it next.  A register
 * vs_pop just freed may be returned, so read the operands first or write
 * the result last. */
static int vs_push(Gen *g)
{
    int i, j, r = -1;
    if (g->vs.nc == NCACHE) vs_spill(g, NCACHE - 1);
    for (i = 0; i < NCACHE && r < 0; i++) {
        r = cache_regs[i];
        for (j = 0; j < g->vs.nc; j++)
            if (g->vs.cr[j] == r) { r = -1; break; }
    }
    g->vs.cr[g->vs.nc++] = r;
    return r;
}
static void vs_push_reg(Gen *g, int src)
{
    int r = vs_push(g);
    if (r != src) E(a64_mov_reg(0, r, src));
}

/* Reload the cache of state IN from memory, where a flush left it. */
static void vs_reload(Gen *g, const VState *in)
{
    int i;
    for (i = 0; i < in->nc; i++)
        E(a64_ldur(4, in->cr[i], R_TOP, -4 * (in->nc - i)));
    emit_drop(g, (uint32_t)in->nc);
    g->vs = *in;
}

/* --- Slow paths ---------------------------------------------------------- */

static SArg sa_reg(int r)        { SArg a; a.kind = SA_REG; a.val = (uint32_t)r; return a; }
static SArg sa_imm(uint32_t v)   { SArg a; a.kind = SA_IMM; a.val = v; return a; }
static SArg sa_const(uint32_t k) { SArg a; a.kind = SA_CONST; a.val = k; return a; }
static SArg sa_none(void)        { SArg a; a.kind = SA_NONE; a.val = 0; return a; }

/* Register a slow path calling FN(A0, A1, A2) from the current state; the
 * fast path branches to the returned record's entry and binds its join. */
static Slow *slow_new(Gen *g, const void *fn, SArg a0, SArg a1, SArg a2)
{
    Slow *s;
    if (g->n_slow == g->cap_slow) {
        uint32_t i, cap = g->cap_slow ? g->cap_slow * 2 : 16;
        Slow *ns = (Slow *)platform_alloc((unsigned long)cap * sizeof(Slow));
        if (ns == NULL) { g->oom = 1; return NULL; }
        for (i = 0; i < g->n_slow; i++) ns[i] = g->slow[i];
        if (g->slow) platform_free(g->slow);
        g->slow = ns;
        g->cap_slow = cap;
    }
    s = &g->slow[g->n_slow++];
    s->entry = a64_label_new(&g->a);
    s->join = a64_label_new(&g->a);
    s->fn = fn;
    s->args[0] = a0; s->args[1] = a1; s->args[2] = a2;
    s->in = g->vs;
    s->mv1 = g->mv1;
    s->ip = g->cur_ip;
    /* The slow path writes the frame's ip, so after the join it is this
     * opcode's or the earlier one. */
    if (g->ip_valid && g->ip_known != g->cur_ip) g->ip_valid = 0;
    return s;
}

/* The slow paths, after the body.  An argument register is never w0-w2
 * (operands live in w10/w11 and w13-w15), so the moves cannot clobber one
 * another. */
static void emit_slow_paths(Gen *g)
{
    uint32_t i;
    int k;
    for (i = 0; i < g->n_slow && !g->a.bad && !g->oom; i++) {
        Slow s = g->slow[i];          /* by value: no growth here, but cheap */
        a64_bind(&g->a, s.entry);
        for (k = 0; k < 3; k++) {
            switch (s.args[k].kind) {
            case SA_REG:   E(a64_mov_reg(0, k, (int)s.args[k].val)); break;
            case SA_IMM:   a64_mov_imm(&g->a, 0, k, s.args[k].val); break;
            case SA_CONST: emit_load_const(g, k, s.args[k].val); break;
            default: break;
            }
        }
        g->vs = s.in;
        g->cur_ip = s.ip;
        g->ip_valid = 0;
        vs_flush(g);
        emit_call(g, s.fn);
        vs_reload(g, &s.in);
        if (s.mv1) emit_mv1_store(g);
        a64_b(&g->a, s.join);
    }
    g->vs.nc = 0;
}

/* Both W registers fixnums, else to the slow path. */
static void emit_check_fixnums(Gen *g, int ra, int rb, A64Label slow)
{
    E(a64_and_reg(0, 9, ra, rb));
    a64_tbz(&g->a, 9, 0, slow);
}

/* RA a heap object (low two bits clear) of header type TYPE, else to SLOW;
 * xBASE = its address.  NIL (0) goes to IF_NIL. */
static void emit_check_heap_type(Gen *g, int ra, uint32_t type, int base,
                                 A64Label if_nil, A64Label slow)
{
    a64_cbz(&g->a, 0, ra, if_nil);
    E(a64_lsl_imm(0, 9, ra, 30));
    a64_cbnz(&g->a, 0, 9, slow);
    E(a64_add_uxtw(base, R_ARENA, ra, 0));
    E(a64_ldr_uoff(4, 9, base, 0));
    E(a64_lsr_imm(0, 9, 9, CL_HDR_TYPE_SHIFT));
    if (type == 0) {
        a64_cbnz(&g->a, 0, 9, slow);
    } else {
        E(a64_cmp_imm(0, 9, type));
        a64_bcond(&g->a, A64_NE, slow);
    }
}

/* RA a structure with a slot IDX, else to SLOW; x12 = its address. */
static void emit_check_struct(Gen *g, int ra, uint32_t idx, A64Label slow)
{
    emit_check_heap_type(g, ra, TYPE_STRUCT, 12, slow, slow);
    E(a64_ldr_uoff(4, 9, 12, OFF_ST_NSLOTS));
    E(a64_cmp_imm(0, 9, idx));
    a64_bcond(&g->a, A64_LS, slow);              /* n_slots <= idx */
}

/* w0 = the value of the special variable bc->constants[K], else to SLOW:
 * a dynamic binding on this thread, an unbound value, or NIL as the
 * symbol (whose cell is SYM_NIL's) -- the helper's cases. */
static void emit_symbol_value(Gen *g, const CL_Bytecode *bc, uint32_t k, A64Label slow)
{
    if (bc->constants[k] == CL_NIL) { a64_b(&g->a, slow); return; }
    E(a64_ldr_uoff(4, 9, R_THR, OFF_TLV_COUNT));
    a64_cbnz(&g->a, 0, 9, slow);
    emit_load_const(g, 12, k);
    E(a64_add_uxtw(12, R_ARENA, 12, 0));
    E(a64_ldr_uoff(4, 0, 12, OFF_SYM_VALUE));
    E(a64_adds_imm(0, A64_ZR, 0, 0u - (uint32_t)CL_UNBOUND));   /* cmn w0, #10 */
    a64_bcond(&g->a, A64_EQ, slow);
}

/* Binary helper op: w0 = second, w1 = TOS (both still on the stack, so
 * rooted across the call), FN(w0, w1), the two replaced by the result. */
static void emit_binary_helper(Gen *g, const void *fn, int mv)
{
    emit_peek(g, 0, 2);
    emit_peek(g, 1, 1);
    emit_call(g, fn);
    emit_drop(g, 2);
    emit_push(g, 0);
    if (mv) emit_mv1(g);
}

/* --- NLX frames, closures --------------------------------------------- */

/* x20 = &cl_vm.stack[cl_vm.sp]: the operand-stack top an NLX landing's
 * post_longjmp helper restored (the frame's own, saved at its alloc). */
static void emit_reload_top(Gen *g)
{
    E(a64_ldr_uoff(4, 9, R_THR, OFF_VM_SP));
    E(a64_add_uxtw(R_TOP, R_STK, 9, 2));
}

/* The NLX frame push (runtime_nlx.h): ALLOC(x0, set by the caller) reserves
 * the frame and returns &nlx->buf, which _setjmp fills from THIS frame --
 * x19-x28 and sp, so a longjmp lands back here with every role register
 * intact (x20 aside, reloaded from cl_vm.sp).  The zero return branches to
 * the returned label, where the caller commits; the code emitted next is the
 * longjmp arm.  The cache is empty (Rule 1: setjmp is a call, and the arm
 * finds the frame in memory). */
static A64Label emit_nlx_setjmp(Gen *g, const void *alloc)
{
    A64Label normal = a64_label_new(&g->a);
    emit_call(g, alloc);
    emit_call(g, (const void *)&_setjmp);
    a64_cbz(&g->a, 0, 0, normal);
    return normal;
}

/* wR = upvalue IDX of the closure this frame was entered with (its
 * function slot), NIL when the function is not a closure -- OP_UPVAL.
 * R is not x9/x10/x12. */
static void emit_upval(Gen *g, int r, uint32_t n_locals, uint32_t idx)
{
    A64Label done = a64_label_new(&g->a);
    emit_load_local(g, 10, n_locals);
    E(a64_movz(0, r, 0, 0));
    emit_check_heap_type(g, 10, TYPE_CLOSURE, 12, done, done);
    E(a64_ldr_uoff(4, r, 12, OFF_CL_UPVALS + 4 * idx));
    a64_bind(&g->a, done);
}

/* --- Bytecode walk ------------------------------------------------------- */

#define T_TARGET 1      /* some branch lands here */
#define T_LOOP   2      /* a backward branch lands here: poll */

static int32_t read_i32_be(const uint8_t *p)
{
    return (int32_t)(((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                     ((uint32_t)p[2] << 8) | (uint32_t)p[3]);
}

static uint16_t read_u16_be(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/* landing = BASE + OFFSET within [0, code_len], else -1. */
static int32_t landing_ip(int32_t offset, uint32_t base, uint32_t code_len)
{
    int64_t t = (int64_t)base + offset;
    if (t < 0 || t > (int64_t)code_len) return -1;
    return (int32_t)t;
}

/* The operand bytes after opcode OP, or -1 for an opcode this walker does
 * not handle (the function then stays interpreted).  OP_CLOSURE's length
 * depends on its template: op_len. */
static int op_operand_len(uint8_t op)
{
    switch (op) {
    case OP_NIL: case OP_T: case OP_ARGC: case OP_POP: case OP_DUP: case OP_RET:
    case OP_CAR: case OP_CDR: case OP_CONS: case OP_NOT: case OP_EQ:
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV:
    case OP_LT: case OP_GT: case OP_LE: case OP_GE: case OP_NUMEQ:
    case OP_MV_RESET: case OP_RPLACA: case OP_RPLACD: case OP_ASET:
    case OP_APPLY: case OP_CHAREQ:
    case OP_MV_TO_LIST: case OP_NTH_VALUE:
    case OP_MV_SAVE: case OP_MV_RESTORE:
    case OP_MAKE_CELL: case OP_CELL_REF:
    case OP_BLOCK_POP: case OP_UNCATCH: case OP_TAGBODY_POP:
    case OP_UWPOP: case OP_UWRETHROW: case OP_HANDLER_CASE_POP:
    case OP_PROGV_BIND: case OP_PROGV_UNBIND:
        return 0;
    case OP_LOAD: case OP_STORE: case OP_CALL: case OP_TAILCALL:
    case OP_STRUCT_REF: case OP_STRUCT_SET: case OP_LIST:
    case OP_AREF: case OP_PUSH_LOCAL: case OP_POP_LOCAL:
    case OP_STORE_POP: case OP_LOAD_MV_RESET: case OP_LOAD_RET: case OP_POP_LOAD:
    case OP_UPVAL: case OP_CELL_SET_LOCAL: case OP_CELL_SET_UPVAL:
    case OP_MV_LOAD: case OP_DYNUNBIND: case OP_HANDLER_POP: case OP_RESTART_POP:
        return 1;
    case OP_CONST: case OP_GLOAD: case OP_GSTORE: case OP_FLOAD: case OP_FSTORE:
    case OP_ASSERT_TYPE:
    case OP_LOAD_LOAD: case OP_LOAD_STRUCT_REF: case OP_LOAD_STORE_POP:
    case OP_DYNBIND: case OP_HANDLER_PUSH: case OP_RESTART_PUSH:
    case OP_BLOCK_RETURN: case OP_TAGBODY_GO:
    case OP_CLOSURE:                            /* + 2 per capture: op_len */
        return 2;
    case OP_CALL_GLOBAL: case OP_TAILCALL_GLOBAL: case OP_LOAD_CONST:
        return 3;
    case OP_LOAD_CALL_GLOBAL:                   /* u8 slot, u16 sym, u8 n */
    case OP_JMP: case OP_JNIL: case OP_JTRUE: case OP_EQ_JNIL:   /* i32 */
    case OP_CATCH: case OP_UWPROT:
        return 4;
    case OP_GLOAD_CALL_GLOBAL:                  /* u16 sym, u16 sym, u8 n */
    case OP_LOAD_JNIL: case OP_CMP_BR:          /* u8, i32 */
        return 5;
    case OP_GLOAD_JNIL: case OP_GLOAD_EQ_JNIL:  /* u16 sym, i32 */
    case OP_BLOCK_PUSH: case OP_TAGBODY_PUSH: case OP_HANDLER_CASE_PUSH:
        return 6;
    default:
        return -1;
    }
}

/* OP_CLOSURE's template: the bytecode constant its u16 names, or NULL. */
static const CL_Bytecode *closure_template(const CL_Bytecode *bc, const uint8_t *o)
{
    uint16_t k = read_u16_be(o);
    if (k >= bc->n_constants || !CL_BYTECODE_P(bc->constants[k])) return NULL;
    return (const CL_Bytecode *)CL_OBJ_TO_PTR(bc->constants[k]);
}

/* The operand bytes of the opcode at IP (op_operand_len, plus OP_CLOSURE's
 * capture descriptors), or -1. */
static int op_len(const CL_Bytecode *bc, uint32_t ip)
{
    uint8_t op = bc->code[ip];
    int n = op_operand_len(op);
    if (op == OP_CLOSURE && n >= 0) {
        const CL_Bytecode *t;
        if (ip + 3 > bc->code_len) return -1;
        t = closure_template(bc, bc->code + ip + 1);
        if (t == NULL || t->n_upvalues > 255) return -1;
        n += 2 * (int)t->n_upvalues;
    }
    return n;
}

/* The length of HANDLER-CASE's clause TYPE list, or 0 when it is not one
 * the walker takes (none, or more than a cmp immediate's worth). */
static uint32_t handler_case_clauses(CL_Obj types)
{
    uint32_t n = 0;
    while (CL_CONS_P(types) && n <= 127) { n++; types = cl_cdr(types); }
    return (n == 0 || n > 127 || !CL_NULL_P(types)) ? 0 : n;
}

/* The opcodes with a cache-aware template.  Every other one starts from
 * an empty cache. */
static int op_cached(uint8_t op)
{
    switch (op) {
    case OP_NIL: case OP_T: case OP_ARGC: case OP_CONST: case OP_LOAD: case OP_STORE:
    case OP_POP: case OP_DUP: case OP_STORE_POP: case OP_LOAD_LOAD:
    case OP_LOAD_CONST: case OP_LOAD_STORE_POP: case OP_POP_LOAD:
    case OP_LOAD_MV_RESET: case OP_MV_RESET: case OP_LOAD_RET: case OP_RET:
    case OP_ADD: case OP_SUB: case OP_MUL:
    case OP_LT: case OP_GT: case OP_LE: case OP_GE: case OP_NUMEQ:
    case OP_EQ: case OP_NOT: case OP_CAR: case OP_CDR:
    case OP_GLOAD: case OP_GSTORE:
    case OP_STRUCT_REF: case OP_LOAD_STRUCT_REF: case OP_STRUCT_SET:
    case OP_JMP: case OP_JNIL: case OP_JTRUE: case OP_LOAD_JNIL: case OP_EQ_JNIL:
    case OP_GLOAD_JNIL: case OP_GLOAD_EQ_JNIL: case OP_CMP_BR:
    case OP_UPVAL: case OP_CELL_REF: case OP_CELL_SET_LOCAL:
        return 1;
    default:
        return 0;
    }
}

/* The NLX opcodes' landing operand: the i32's position after the opcode
 * byte (the landing is relative to the instruction's end), or -1. */
static int op_landing_pos(uint8_t op)
{
    switch (op) {
    case OP_CATCH: case OP_UWPROT: return 0;
    case OP_BLOCK_PUSH: case OP_TAGBODY_PUSH: case OP_HANDLER_CASE_PUSH: return 2;
    default: return -1;
    }
}

/* The branch operand of a branching opcode: its offset's position after the
 * opcode byte, or -1 for a straight-line opcode. */
static int op_branch_pos(uint8_t op)
{
    switch (op) {
    case OP_JMP: case OP_JNIL: case OP_JTRUE: case OP_EQ_JNIL: return 0;
    case OP_LOAD_JNIL: case OP_CMP_BR: return 1;
    case OP_GLOAD_JNIL: case OP_GLOAD_EQ_JNIL: return 2;
    default: return -1;
    }
}

/* Mark every branch target (and loop header) and validate the stream: every
 * opcode known, every operand inside the code, every constant index and
 * local slot in range, the symbol operands symbols.  0 = decline. */
static int prescan(const CL_Bytecode *bc, uint8_t *tgt, uint32_t *n_sites)
{
    uint32_t ip = 0;
    *n_sites = 0;
    while (ip < bc->code_len) {
        uint8_t op = bc->code[ip];
        int n = op_len(bc, ip), bpos;
        const uint8_t *o = bc->code + ip + 1;
        uint32_t end;
        if (n < 0) return 0;
        end = ip + 1 + (uint32_t)n;
        if (end > bc->code_len) return 0;
        if (op == OP_CALL || op == OP_CALL_GLOBAL ||
            op == OP_LOAD_CALL_GLOBAL || op == OP_GLOAD_CALL_GLOBAL)
            (*n_sites)++;   /* a direct-call cell each (emit_direct_call) */
        switch (op) {      /* operand validation */
        case OP_LOAD: case OP_STORE: case OP_STORE_POP: case OP_LOAD_MV_RESET:
        case OP_LOAD_RET: case OP_POP_LOAD: case OP_PUSH_LOCAL: case OP_POP_LOCAL:
        case OP_LOAD_JNIL: case OP_LOAD_STRUCT_REF: case OP_CELL_SET_LOCAL:
            if (o[0] >= bc->n_locals) return 0;
            break;
        case OP_UPVAL: case OP_CELL_SET_UPVAL:
            if (o[0] >= bc->n_upvalues) return 0;
            break;
        case OP_CLOSURE: {
            /* Captures: a local of this frame, or an upvalue of this
             * function's own closure. */
            uint32_t k, nu = closure_template(bc, o)->n_upvalues;
            for (k = 0; k < nu; k++) {
                uint8_t is_local = o[2 + 2 * k], idx = o[3 + 2 * k];
                if (is_local ? idx >= bc->n_locals : idx >= bc->n_upvalues) return 0;
            }
            break;
        }
        case OP_BLOCK_PUSH: case OP_TAGBODY_PUSH: case OP_BLOCK_RETURN:
        case OP_TAGBODY_GO: case OP_RESTART_PUSH:
            if (read_u16_be(o) >= bc->n_constants) return 0;
            break;
        case OP_HANDLER_PUSH: case OP_DYNBIND: {
            uint16_t k = read_u16_be(o);
            if (k >= bc->n_constants || !CL_SYMBOL_P(bc->constants[k])) return 0;
            break;
        }
        case OP_LOAD_LOAD: case OP_LOAD_STORE_POP:
            if (o[0] >= bc->n_locals || o[1] >= bc->n_locals) return 0;
            break;
        case OP_CONST: case OP_ASSERT_TYPE:
            if (read_u16_be(o) >= bc->n_constants) return 0;
            break;
        case OP_LOAD_CONST:
            if (o[0] >= bc->n_locals || read_u16_be(o + 1) >= bc->n_constants) return 0;
            break;
        case OP_GLOAD: case OP_GSTORE: case OP_FLOAD: case OP_FSTORE:
        case OP_CALL_GLOBAL: case OP_TAILCALL_GLOBAL:
        case OP_GLOAD_JNIL: case OP_GLOAD_EQ_JNIL: {
            uint16_t k = read_u16_be(o);
            if (k >= bc->n_constants || !CL_SYMBOL_P(bc->constants[k])) return 0;
            break;
        }
        case OP_LOAD_CALL_GLOBAL: {
            uint16_t k = read_u16_be(o + 1);
            if (o[0] >= bc->n_locals || k >= bc->n_constants ||
                !CL_SYMBOL_P(bc->constants[k])) return 0;
            break;
        }
        case OP_GLOAD_CALL_GLOBAL: {
            uint16_t k1 = read_u16_be(o), k2 = read_u16_be(o + 2);
            if (k1 >= bc->n_constants || k2 >= bc->n_constants ||
                !CL_SYMBOL_P(bc->constants[k1]) || !CL_SYMBOL_P(bc->constants[k2]))
                return 0;
            break;
        }
        case OP_AREF:
            if (o[0] > CL_AREF_KIND_SCHAR) return 0;
            break;
        case OP_CMP_BR:
            if ((o[0] & CL_CMP_BR_CMP_MASK) > CL_CMP_BR_CHAREQ ||
                (o[0] & ~(CL_CMP_BR_CMP_MASK | CL_CMP_BR_IF_TRUE)))
                return 0;
            break;
        default:
            break;
        }
        bpos = op_branch_pos(op);
        if (bpos >= 0) {
            int32_t t = landing_ip(read_i32_be(o + bpos), end, bc->code_len);
            if (t < 0) return 0;
            tgt[t] |= T_TARGET;
            if ((uint32_t)t <= ip) tgt[t] |= T_LOOP;
        }
        /* An NLX frame's landing: where a transfer to the frame resumes,
         * always after the form it protects.  HANDLER-CASE's is a table of
         * one OP_JMP per clause, every entry a landing. */
        bpos = op_landing_pos(op);
        if (bpos >= 0) {
            int32_t t = landing_ip(read_i32_be(o + bpos), end, bc->code_len);
            uint32_t k, nc = 1;
            if (t < 0 || (uint32_t)t < end) return 0;
            if (op == OP_HANDLER_CASE_PUSH) {
                uint16_t ki = read_u16_be(o);
                if (ki >= bc->n_constants) return 0;
                nc = handler_case_clauses(bc->constants[ki]);
                if (nc == 0 || (uint32_t)t + 5 * nc > bc->code_len) return 0;
            }
            for (k = 0; k < nc; k++) tgt[t + 5 * k] |= T_TARGET;
        }
        ip = end;
    }
    return 1;
}

/* The operand-stack depth at each IP is fixed by the bytecode (the
 * compiler keeps it equal on every path into a join), and the walker
 * tracks it: to size the prologue's overflow check, and to check that
 * equality -- an edge whose depth disagrees with its target declines the
 * function rather than guessing.  DEPTH_UNSET marks a target no edge has
 * reached yet. */
#define DEPTH_UNSET (-1)

typedef struct {
    int32_t *at;        /* depth on entry to each IP, or DEPTH_UNSET */
    int32_t  cur;
    int32_t  max;
    int      bad;
} Depth;

static void depth_need(Depth *d, int32_t n) { if (d->cur < n) d->bad = 1; }
static void depth_add(Depth *d, int32_t n)
{
    d->cur += n;
    if (d->cur < 0) d->bad = 1;
    if (d->cur > d->max) d->max = d->cur;
}
/* An edge to TARGET with the current depth (from the branch at FROM). */
static void depth_edge(Depth *d, int32_t target, uint32_t from)
{
    if ((uint32_t)target <= from) {          /* backward: already walked */
        if (d->at[target] != d->cur) d->bad = 1;
    } else if (d->at[target] == DEPTH_UNSET) {
        d->at[target] = d->cur;
    } else if (d->at[target] != d->cur) {
        d->bad = 1;
    }
}

/* An NLX landing at TARGET (from the frame push at FROM) with DEPTH: the
 * depth of the frame push itself, plus the value the arm pushes. */
static void depth_landing(Depth *d, int32_t target, uint32_t from, int32_t depth)
{
    int32_t cur = d->cur;
    d->cur = depth;
    if (depth > d->max) d->max = depth;
    depth_edge(d, target, from);
    d->cur = cur;
}

/* --- Direct calls (runtime_vmstack.c, "Direct native-to-native calls") --- */

static CL_Obj a64_tail_finish(CL_Thread *t);

/* A CL_Thread field of SIZE bytes at OFF into or from wRT/xRT, through x17
 * when OFF is past the scaled-offset range. */
static void emit_thr_ldst(Gen *g, int load, int size, int rt, uint32_t off)
{
    uint32_t w = load ? a64_ldr_uoff(size, rt, R_THR, off)
                      : a64_str_uoff(size, rt, R_THR, off);
    if (w != A64_BAD) { E(w); return; }
    emit_add_x(g, 17, R_THR, off);
    E(load ? a64_ldr_uoff(size, rt, 17, 0) : a64_str_uoff(size, rt, 17, 0));
}

/* A call site: OP_CALL (FUNCALL; the function value under the N arguments
 * on the operand stack) when SYMREF_K < 0, else a _GLOBAL call of the
 * function of constants[SYMREF_K].  The cache is flushed.  w0 = the value.
 *
 * The hit path, taken while the site's cell (one 64-bit word: gen | func <<
 * 32) holds the current cl_call_gen -- and, for OP_CALL, the function the
 * operand stack holds:
 *   - the callee's bytecode from func (through a closure), its native code
 *     (NULL once unhooked: a miss);
 *   - the C stack above c_stack_base - CL_C_STACK_LIMIT (with a 64K margin
 *     for the callee's helpers), and room in the frame stack -- else the
 *     miss path, whose checks signal the interpreter's errors;
 *   - the CL_Frame cl_jit_invoke would push (bp at the arguments, as the
 *     interpreter's OP_CALL_GLOBAL leaves them), the native entry called
 *     with the arguments where they lie, a pending tail-call handoff run by
 *     a64_tail_finish, the frame popped.
 * The miss path is the helper call every site made before direct calls,
 * now passing the cell to fill. */
static void emit_direct_call(Gen *g, uint32_t n, int32_t symref_k)
{
    A64Label l_miss = a64_label_new(&g->a);
    A64Label l_join = a64_label_new(&g->a);
    A64Label l_bc   = a64_label_new(&g->a);
    A64Label l_tail = a64_label_new(&g->a);
    A64Label l_pop  = a64_label_new(&g->a);
    uint64_t *cell;

    if (g->sites == NULL || g->n_sites >= g->max_sites) { g->a.bad = 1; return; }
    cell = &g->sites[g->n_sites++];
    emit_frame_ip(g);

    emit_ldr_lit64(g, 12, cell);
    E(a64_ldr_uoff(8, 9, 12, 0));                       /* gen | func << 32 */
    emit_ldr_lit64(g, 11, (const void *)&cl_call_gen);
    E(a64_ldr_uoff(4, 11, 11, 0));
    E(a64_cmp_reg(0, 9, 11));
    a64_bcond(&g->a, A64_NE, l_miss);
    E(a64_lsr_imm(1, 10, 9, 32));                       /* w10 = func */
    if (symref_k < 0) {
        emit_peek(g, 11, n + 1);
        E(a64_cmp_reg(0, 10, 11));
        a64_bcond(&g->a, A64_NE, l_miss);
    }
    /* x12 = the callee's CL_Bytecode, x14 = its native code. */
    E(a64_add_uxtw(12, R_ARENA, 10, 0));
    E(a64_ldr_uoff(4, 11, 12, 0));
    E(a64_lsr_imm(0, 11, 11, CL_HDR_TYPE_SHIFT));
    E(a64_cmp_imm(0, 11, TYPE_CLOSURE));
    a64_bcond(&g->a, A64_NE, l_bc);
    E(a64_ldr_uoff(4, 11, 12, OFF_CL_BYTECODE));
    E(a64_add_uxtw(12, R_ARENA, 11, 0));
    a64_bind(&g->a, l_bc);
    E(a64_ldr_uoff(8, 14, 12, OFF_BC_NATIVE));
    a64_cbz(&g->a, 1, 14, l_miss);
    /* The C-stack floor (c_stack_base NULL wraps it high: a miss). */
    emit_thr_ldst(g, 1, 8, 15, OFF_C_STACK_BASE);
    emit_sub_x(g, 15, 15, CL_C_STACK_LIMIT - 0x10000u);
    E(a64_mov_sp(16, A64_SP));
    E(a64_cmp_reg(1, 16, 15));
    a64_bcond(&g->a, A64_LS, l_miss);
    /* A free frame: x16 = &frames[fp], fp + 1. */
    emit_thr_ldst(g, 1, 4, 15, OFF_VM_FP);
    emit_thr_ldst(g, 1, 4, 16, OFF_VM_FRAME_SIZE);
    E(a64_cmp_reg(0, 15, 16));
    a64_bcond(&g->a, A64_GE, l_miss);
    emit_thr_ldst(g, 1, 8, 16, OFF_VM_FRAMES);
    E(a64_movz(0, 17, (uint32_t)sizeof(CL_Frame), 0));
    E(a64_smull(17, 15, 17));
    E(a64_add_reg(1, 16, 16, 17));
    E(a64_add_imm(0, 15, 15, 1));
    emit_thr_ldst(g, 0, 4, 15, OFF_VM_FP);
    /* The frame, as cl_jit_invoke fills it. */
    E(a64_str_uoff(4, 10, 16, OFF_F_BYTECODE));
    E(a64_ldr_uoff(8, 17, 12, OFF_BC_CODE));
    E(a64_str_uoff(8, 17, 16, OFF_F_CODE));
    E(a64_ldr_uoff(8, 17, 12, OFF_BC_CONSTANTS));
    E(a64_str_uoff(8, 17, 16, OFF_F_CONSTANTS));
    E(a64_str_uoff(4, A64_ZR, 16, OFF_FRAME_IP));
    E(a64_sub_reg(1, 17, R_TOP, R_STK));
    E(a64_lsr_imm(1, 17, 17, 2));
    E(a64_sub_imm(0, 17, 17, n));
    E(a64_str_uoff(4, 17, 16, OFF_F_BP));
    E(a64_ldr_uoff(2, 17, 12, OFF_BC_NLOCALS));
    E(a64_str_uoff(4, 17, 16, OFF_F_NLOCALS));
    E(a64_movz(0, 17, n, 0));
    E(a64_str_uoff(2, 17, 16, OFF_F_NARGS));
    emit_thr_ldst(g, 1, 4, 17, OFF_NLX_TOP);
    E(a64_str_uoff(4, 17, 16, OFF_F_NLX));
    E(a64_str_uoff(1, A64_ZR, 16, OFF_F_FSLOT));
    /* entry(thr, bp, nargs, func, frame) */
    E(a64_mov_reg(1, 0, R_THR));
    emit_sub_x(g, 1, R_TOP, 4 * n);
    E(a64_movz(0, 2, n, 0));
    E(a64_mov_reg(0, 3, 10));
    E(a64_mov_reg(1, 4, 16));
    E(a64_blr(14));
    emit_thr_ldst(g, 1, 4, 9, OFF_TAIL_PENDING);
    a64_cbnz(&g->a, 0, 9, l_tail);
    a64_bind(&g->a, l_pop);
    emit_thr_ldst(g, 1, 4, 9, OFF_VM_FP);
    E(a64_sub_imm(0, 9, 9, 1));
    emit_thr_ldst(g, 0, 4, 9, OFF_VM_FP);
    a64_b(&g->a, l_join);
    a64_bind(&g->a, l_tail);
    E(a64_mov_reg(1, 0, R_THR));
    emit_ldr_lit64(g, 16, (const void *)&a64_tail_finish);
    E(a64_blr(16));
    a64_b(&g->a, l_pop);

    /* The miss: the helper every site called before direct calls. */
    a64_bind(&g->a, l_miss);
    E(a64_mov_reg(1, 0, R_THR));
    E(a64_mov_reg(1, 1, R_TOP));
    E(a64_movz(0, 2, n, 0));
    if (symref_k < 0) {
        emit_ldr_lit64(g, 3, cell);
        emit_call(g, (const void *)&cl_jit_vmstack_call);
    } else {
        emit_const_addr(g, 3, (uint32_t)symref_k);
        emit_ldr_lit64(g, 4, cell);
        emit_call(g, (const void *)&cl_jit_vmstack_call_global);
    }
    a64_bind(&g->a, l_join);
    g->mv1 = 0;
}

/* The function's native code, or NULL to leave it interpreted.  *LEN_OUT
 * receives its length. */
static uint8_t *walk(const CL_Bytecode *bc, uint32_t *len_out,
                     uint64_t **sites_out)
{
    Gen gs, *g = &gs;
    uint8_t *tgt = NULL;
    A64Label *ip_lab = NULL;
    Depth d;
    uint32_t arity, n_pos, n_locals, ip, i;
    int is_kw, has_rest;
    int fell = 1;               /* control reaches the next IP from above */
    A64Label l_body, l_epilogue, l_overflow, l_check;
    uint8_t *code = NULL;
    uint32_t len = 0;
    uint32_t n_sites = 0;

    *sites_out = NULL;
    g->sites = NULL;
    /* The gate: any lambda list.  Required and &optional parameters get an
     * inline prologue, &rest and &key the helper (cl_jit_vmstack_ll_prologue). */
    if (bc->code == NULL || bc->code_len == 0) return NULL;
    is_kw = (bc->flags & 1) != 0;
    if (is_kw ? (bc->flags & ~3u) != 0 : (bc->flags != 0 || bc->n_keys != 0))
        return NULL;
    has_rest = (bc->arity & 0x8000) != 0;
    arity = bc->arity & 0x7FFF;
    n_pos = arity + bc->n_optional;
    n_locals = bc->n_locals;
    if (n_locals < n_pos + (has_rest ? 1u : 0u) || n_locals > A64_MAX_LOCALS)
        return NULL;
    if (bc->n_constants > 0 && bc->constants == NULL) return NULL;
    if (bc->n_keys > 0) {
        if (bc->key_syms == NULL || bc->key_slots == NULL) return NULL;
        for (i = 0; i < bc->n_keys; i++) {
            if (bc->key_slots[i] >= n_locals) return NULL;
            if (bc->key_suppliedp_slots && bc->key_suppliedp_slots[i] >= n_locals)
                return NULL;
        }
    }

    d.at = NULL;
    d.cur = 0; d.max = 0; d.bad = 0;
    tgt = (uint8_t *)platform_alloc((unsigned long)bc->code_len + 1);
    ip_lab = (A64Label *)platform_alloc((unsigned long)(bc->code_len + 1) * sizeof(A64Label));
    d.at = (int32_t *)platform_alloc((unsigned long)(bc->code_len + 1) * sizeof(int32_t));
    if (tgt == NULL || ip_lab == NULL || d.at == NULL) goto out;
    for (i = 0; i <= bc->code_len; i++) { tgt[i] = 0; d.at[i] = DEPTH_UNSET; }
    if (!prescan(bc, tgt, &n_sites)) goto out;
    /* The direct-call cells, empty (gen 0): ordinary memory, written by
     * the fill (C) while native code reads them. */
    g->n_sites = 0;
    g->max_sites = n_sites;
    if (n_sites > 0) {
        g->sites = (uint64_t *)platform_alloc((unsigned long)n_sites * sizeof(uint64_t));
        if (g->sites == NULL) goto out;
        for (i = 0; i < n_sites; i++) g->sites[i] = 0;
    }

    cb_init(&g->cb, 256);
    a64_asm_init(&g->a, &g->cb);
    g->lit_val = NULL; g->lit_label = NULL;
    g->n_lits = g->cap_lits = 0; g->oom = 0;
    g->slow = NULL; g->n_slow = g->cap_slow = 0;
    g->vs.nc = 0; g->mv1 = 0;
    g->cur_ip = 0; g->ip_known = 0; g->ip_valid = 0;
    for (i = 0; i <= bc->code_len; i++)
        ip_lab[i] = tgt[i] ? a64_label_new(&g->a) : 0;
    l_body = a64_label_new(&g->a);
    l_epilogue = a64_label_new(&g->a);
    l_overflow = a64_label_new(&g->a);
    l_check = a64_label_new(&g->a);

    /* Prologue: the frame record and the callee-saved registers, the
     * register roles, then the overflow check (its displacement is the
     * walk's maximum depth, known only at the end: a branch to a check
     * emitted after the body, which branches back), the locals and the
     * function slot. */
    E(a64_stp_pre(A64_FP, A64_LR, A64_SP, -FRAME_BYTES));
    E(a64_mov_sp(A64_FP, A64_SP));
    E(a64_stp_off(19, 20, A64_SP, 16));
    E(a64_stp_off(21, 22, A64_SP, 32));
    E(a64_stp_off(23, 24, A64_SP, 48));
    E(a64_stp_off(27, 28, A64_SP, 64));
    E(a64_stp_off(25, 26, A64_SP, 80));
    E(a64_mov_reg(1, R_THR, 0));
    E(a64_mov_reg(1, R_BP, 1));
    E(a64_mov_reg(1, R_FRAME, 4));
    E(a64_ldr_uoff(8, R_STK, R_THR, OFF_VM_STACK));
    emit_ldr_lit64(g, R_K, bc->constants);
    emit_ldr_lit64(g, R_ARENA, (const void *)&cl_arena_base);
    E(a64_ldr_uoff(8, R_ARENA, R_ARENA, 0));
    a64_b(&g->a, l_check);
    {
        A64Label l_back = a64_label_new(&g->a);
        a64_bind(&g->a, l_back);
        /* The argument count, for OP_ARGC (the &optional prologue's
         * defaults and supplied-p tests). */
        E(a64_mov_reg(0, R_NARGS, 2));
        if (is_kw || has_rest) {
            /* The extra arguments lie where the other locals go: the
             * helper sets up the whole frame, the function slot included.
             * w2 and w3 are still the entry's nargs and function. */
            E(a64_mov_reg(0, 0, 2));
            E(a64_mov_reg(1, 1, R_BP));
            E(a64_mov_reg(0, 2, 3));
            emit_add_x(g, R_TOP, R_BP, 4 * (n_locals + 1));
            emit_call(g, (const void *)&cl_jit_vmstack_ll_prologue);
        } else {
            /* An optional not passed is NIL (the caller checked the count
             * against the lambda list). */
            for (i = arity; i < n_pos; i++) {
                A64Label l_given = a64_label_new(&g->a);
                E(a64_cmp_imm(0, R_NARGS, i));
                a64_bcond(&g->a, A64_HI, l_given);
                emit_store_local(g, A64_ZR, i);
                a64_bind(&g->a, l_given);
            }
            for (i = n_pos; i < n_locals; i++)
                emit_store_local(g, A64_ZR, i);        /* NIL == 0 */
            emit_store_local(g, 3, n_locals);          /* func */
            emit_add_x(g, R_TOP, R_BP, 4 * (n_locals + 1));
        }
        a64_bind(&g->a, l_body);
        g->ip_valid = 0;        /* the self tail call branches here */

        ip = 0;
        while (ip < bc->code_len && !d.bad && !g->a.bad && !g->oom) {
            uint8_t op;
            const uint8_t *o;
            uint32_t insn = ip;
            if (tgt[ip]) {
                /* A label: the frame in memory on every edge into it. */
                if (fell) vs_flush(g);
                g->vs.nc = 0;
                g->mv1 = 0;
                g->ip_valid = 0;
                if (fell) {
                    if (d.at[ip] != DEPTH_UNSET && d.at[ip] != d.cur) { d.bad = 1; break; }
                    d.at[ip] = d.cur;
                } else if (d.at[ip] != DEPTH_UNSET) {
                    d.cur = d.at[ip];
                } else {
                    /* Reached only by a later backward branch: assume the
                     * depth control left with; that branch checks it. */
                    d.at[ip] = d.cur;
                }
                a64_bind(&g->a, ip_lab[ip]);
                if (tgt[ip] & T_LOOP) emit_loop_poll(g);
            }
            fell = 1;
            op = bc->code[ip];
            o = bc->code + ip + 1;
            ip += 1 + (uint32_t)op_len(bc, ip);
            g->cur_ip = ip;

            /* An opcode without a cache-aware template runs its phase-1
             * code, which addresses the operand stack in memory. */
            if (!op_cached(op)) vs_flush(g);

            switch (op) {
            case OP_NIL:
                E(a64_movz(0, vs_push(g), 0, 0));
                depth_add(&d, 1);
                emit_mv1(g);
                break;
            case OP_T:
                emit_load_t(g, vs_push(g));
                depth_add(&d, 1);
                emit_mv1(g);
                break;
            case OP_ARGC: {         /* the fixnum nargs: n << 1 | 1 */
                int r = vs_push(g);
                E(a64_add_reg(0, r, R_NARGS, R_NARGS));
                E(a64_add_imm(0, r, r, CL_TAG_FIXNUM));
                depth_add(&d, 1);
                emit_mv1(g);
                break;
            }
            case OP_CONST:
                emit_load_const(g, vs_push(g), read_u16_be(o));
                depth_add(&d, 1);
                emit_mv1(g);
                break;
            case OP_LOAD:
                emit_load_local(g, vs_push(g), o[0]);
                depth_add(&d, 1);
                break;
            case OP_STORE:
                depth_need(&d, 1);
                emit_store_local(g, vs_reg(g, 1, 9), o[0]);
                break;
            case OP_POP:
                depth_add(&d, -1);
                vs_pop(g, 1);
                break;
            case OP_DUP:
                depth_need(&d, 1);
                vs_push_reg(g, vs_reg(g, 1, 9));
                depth_add(&d, 1);
                break;
            case OP_STORE_POP:
                depth_add(&d, -1);
                emit_store_local(g, vs_reg(g, 1, 9), o[0]);
                vs_pop(g, 1);
                break;
            case OP_LOAD_LOAD:
                emit_load_local(g, vs_push(g), o[0]);
                emit_load_local(g, vs_push(g), o[1]);
                depth_add(&d, 2);
                break;
            case OP_LOAD_CONST:
                emit_load_local(g, vs_push(g), o[0]);
                emit_load_const(g, vs_push(g), read_u16_be(o + 1));
                depth_add(&d, 2);
                emit_mv1(g);
                break;
            case OP_LOAD_STORE_POP:
                emit_load_local(g, 9, o[0]);
                emit_store_local(g, 9, o[1]);
                break;
            case OP_POP_LOAD:
                depth_need(&d, 1);
                vs_pop(g, 1);
                emit_load_local(g, vs_push(g), o[0]);
                break;
            case OP_LOAD_MV_RESET:
                emit_load_local(g, vs_push(g), o[0]);
                depth_add(&d, 1);
                emit_mv1(g);
                break;
            case OP_MV_RESET:
                emit_mv1(g);
                break;

            /* A return needs no flush: the frame is discarded. */
            case OP_LOAD_RET:
                emit_load_local(g, 0, o[0]);
                a64_b(&g->a, l_epilogue);
                g->vs.nc = 0;
                fell = 0;
                break;
            case OP_RET:
                /* The interpreter returns NIL from an empty operand stack. */
                if (d.cur > 0) {
                    int r = vs_reg(g, 1, 0);
                    if (r != 0) E(a64_mov_reg(0, 0, r));
                    depth_add(&d, -1);
                } else {
                    E(a64_movz(0, 0, 0, 0));
                }
                a64_b(&g->a, l_epilogue);
                g->vs.nc = 0;
                fell = 0;
                break;

            /* Arithmetic and the numeric comparisons: fixnums inline (a
             * tagged fixnum is 2n+1, so a 32-bit overflow is exactly a
             * result outside the fixnum range, and tagged order is numeric
             * order), anything else -- or an overflow -- to the helper, the
             * interpreter's slow path. */
            case OP_ADD: case OP_SUB: case OP_MUL:
            case OP_LT: case OP_GT: case OP_LE: case OP_GE: case OP_NUMEQ: {
                const void *fn =
                    op == OP_ADD ? (const void *)&cl_jit_runtime_add :
                    op == OP_SUB ? (const void *)&cl_jit_runtime_sub :
                    op == OP_MUL ? (const void *)&cl_jit_runtime_mul :
                    op == OP_LT  ? (const void *)&cl_jit_runtime_lt :
                    op == OP_GT  ? (const void *)&cl_jit_runtime_gt :
                    op == OP_LE  ? (const void *)&cl_jit_runtime_le :
                    op == OP_GE  ? (const void *)&cl_jit_runtime_ge :
                                   (const void *)&cl_jit_runtime_numeq;
                int ra, rb;
                Slow *sl;
                A64Label slow, join;
                depth_need(&d, 2);
                ra = vs_reg(g, 2, 10);
                rb = vs_reg(g, 1, 11);
                sl = slow_new(g, fn, sa_reg(ra), sa_reg(rb), sa_none());
                if (sl == NULL) break;
                slow = sl->entry; join = sl->join;
                emit_check_fixnums(g, ra, rb, slow);
                switch (op) {
                case OP_ADD:        /* (2a+1) + (2b+1) - 1 */
                    E(a64_sub_imm(0, 9, rb, 1));
                    E(a64_adds_reg(0, 0, ra, 9));
                    a64_bcond(&g->a, A64_VS, slow);
                    break;
                case OP_SUB:        /* (2a+1) - (2b+1) + 1 */
                    E(a64_subs_reg(0, 0, ra, rb));
                    a64_bcond(&g->a, A64_VS, slow);
                    E(a64_add_imm(0, 0, 0, 1));
                    break;
                case OP_MUL:        /* a * 2b in 64 bits, + 1 if it fits 32 */
                    E(a64_asr_imm(0, 9, ra, 1));
                    E(a64_sub_imm(0, 12, rb, 1));
                    E(a64_smull(0, 9, 12));
                    E(a64_cmp_sxtw(0, 0));
                    a64_bcond(&g->a, A64_NE, slow);
                    E(a64_add_imm(0, 0, 0, 1));
                    break;
                default: {
                    A64Cond c = op == OP_LT ? A64_LT : op == OP_GT ? A64_GT :
                                op == OP_LE ? A64_LE : op == OP_GE ? A64_GE : A64_EQ;
                    E(a64_cmp_reg(0, ra, rb));
                    emit_load_t(g, 0);
                    E(a64_csel(0, 0, 0, A64_ZR, c));
                    break;
                }
                }
                a64_bind(&g->a, join);
                vs_pop(g, 2);
                vs_push_reg(g, 0);
                depth_add(&d, -1);
                emit_mv1(g);
                break;
            }
            case OP_DIV:
                depth_need(&d, 2);
                emit_binary_helper(g, (const void *)&cl_jit_runtime_div, 1);
                depth_add(&d, -1);
                break;
            case OP_CHAREQ:
                depth_need(&d, 2);
                emit_binary_helper(g, (const void *)&cl_jit_runtime_chareq, 1);
                depth_add(&d, -1);
                break;
            case OP_RPLACA: case OP_RPLACD:
                depth_need(&d, 2);
                emit_binary_helper(g, op == OP_RPLACA
                                   ? (const void *)&cl_jit_runtime_rplaca
                                   : (const void *)&cl_jit_runtime_rplacd, 1);
                depth_add(&d, -1);
                break;
            case OP_APPLY:
                /* The interpreter's own OP_APPLY, entered from C: spreads
                 * any argument count, resolves a symbol, &rest callees. */
                depth_need(&d, 2);
                emit_binary_helper(g, (const void *)&cl_vm_apply_list, 0);
                depth_add(&d, -1);
                break;

            case OP_EQ: case OP_NOT: {
                int r;
                if (op == OP_EQ) {
                    int ra, rb;
                    depth_add(&d, -1);
                    ra = vs_reg(g, 2, 10);
                    rb = vs_reg(g, 1, 11);
                    vs_pop(g, 2);
                    E(a64_cmp_reg(0, ra, rb));
                } else {
                    int ra;
                    depth_need(&d, 1);
                    ra = vs_reg(g, 1, 10);
                    vs_pop(g, 1);
                    E(a64_cmp_imm(0, ra, 0));
                }
                /* The flags survive the loads of T. */
                r = vs_push(g);
                emit_load_t(g, r);
                E(a64_csel(0, r, r, A64_ZR, A64_EQ));
                emit_mv1(g);
                break;
            }

            case OP_CAR: case OP_CDR: {
                /* NIL gives NIL, a cons its field, anything else the
                 * helper's type error. */
                int ra;
                Slow *sl;
                A64Label slow, join;
                depth_need(&d, 1);
                ra = vs_reg(g, 1, 10);
                sl = slow_new(g, op == OP_CAR ? (const void *)&cl_jit_runtime_car
                                              : (const void *)&cl_jit_runtime_cdr,
                              sa_reg(ra), sa_none(), sa_none());
                if (sl == NULL) break;
                slow = sl->entry; join = sl->join;
                E(a64_mov_reg(0, 0, ra));
                emit_check_heap_type(g, ra, TYPE_CONS, 12, join, slow);
                E(a64_ldr_uoff(4, 0, 12, op == OP_CAR ? OFF_CAR : OFF_CDR));
                a64_bind(&g->a, join);
                vs_pop(g, 1);
                vs_push_reg(g, 0);
                emit_mv1(g);
                break;
            }
            case OP_CONS:
                depth_need(&d, 2);
                emit_sub_x(g, 0, R_TOP, 8);
                emit_call(g, (const void *)&cl_jit_vmstack_cons);
                emit_drop(g, 2);
                emit_push(g, 0);
                depth_add(&d, -1);
                emit_mv1(g);
                break;
            case OP_LIST: {
                uint32_t n = o[0];
                depth_need(&d, (int32_t)n);
                emit_sub_x(g, 0, R_TOP, 4 * n);
                E(a64_movz(0, 1, n, 0));
                emit_call(g, (const void *)&cl_jit_vmstack_list);
                emit_drop(g, n);
                emit_push(g, 0);
                depth_add(&d, 1 - (int32_t)n);
                emit_mv1(g);
                break;
            }

            /* A special variable: with no dynamic binding on this thread
             * (tlv_entry_count 0) the value is the symbol's own cell, as
             * in the interpreter's VM_GLOBAL_VALUE.  NIL as the symbol, an
             * unbound value and *PACKAGE*'s store go to the helper. */
            case OP_GLOAD: {
                uint16_t k = read_u16_be(o);
                Slow *sl = slow_new(g, (const void *)&cl_jit_runtime_gload,
                                    sa_const(k), sa_none(), sa_none());
                if (sl == NULL) break;
                emit_symbol_value(g, bc, k, sl->entry);
                a64_bind(&g->a, sl->join);
                vs_push_reg(g, 0);
                depth_add(&d, 1);
                emit_mv1(g);
                break;
            }
            case OP_GSTORE: {
                uint16_t k = read_u16_be(o);
                int rv;
                Slow *sl;
                depth_need(&d, 1);
                rv = vs_reg(g, 1, 10);
                sl = slow_new(g, (const void *)&cl_jit_runtime_gstore,
                              sa_const(k), sa_reg(rv), sa_none());
                if (sl == NULL) break;
                if (bc->constants[k] == CL_NIL || bc->constants[k] == SYM_STAR_PACKAGE) {
                    a64_b(&g->a, sl->entry);
                } else {
                    E(a64_ldr_uoff(4, 9, R_THR, OFF_TLV_COUNT));
                    a64_cbnz(&g->a, 0, 9, sl->entry);
                    emit_load_const(g, 12, k);
                    E(a64_add_uxtw(12, R_ARENA, 12, 0));
                    E(a64_str_uoff(4, rv, 12, OFF_SYM_VALUE));
                }
                a64_bind(&g->a, sl->join);
                break;
            }
            case OP_FSTORE:
                depth_need(&d, 1);
                emit_load_const(g, 0, read_u16_be(o));
                emit_peek(g, 1, 1);
                emit_call(g, (const void *)&cl_jit_runtime_fstore);
                break;
            case OP_FLOAD:
                emit_load_const(g, 0, read_u16_be(o));
                emit_call(g, (const void *)&cl_jit_runtime_fload);
                emit_push(g, 0);
                depth_add(&d, 1);
                emit_mv1(g);
                break;

            case OP_LOAD_STRUCT_REF:
                emit_load_local(g, vs_push(g), o[0]);
                depth_add(&d, 1);
                o += 1;
                /* FALLTHROUGH */
            case OP_STRUCT_REF: {
                uint32_t idx = o[0];
                int ra;
                Slow *sl;
                depth_need(&d, 1);
                ra = vs_reg(g, 1, 10);
                sl = slow_new(g, (const void *)&cl_jit_runtime_struct_ref,
                              sa_reg(ra), sa_imm(idx), sa_none());
                if (sl == NULL) break;
                emit_check_struct(g, ra, idx, sl->entry);
                E(a64_ldr_uoff(4, 0, 12, OFF_ST_SLOTS + 4 * idx));
                a64_bind(&g->a, sl->join);
                vs_pop(g, 1);
                vs_push_reg(g, 0);
                emit_mv1(g);
                break;
            }
            case OP_STRUCT_SET: {
                uint32_t idx = o[0];
                int ra, rv;
                Slow *sl;
                A64Label l_st;
                depth_need(&d, 2);
                ra = vs_reg(g, 2, 10);
                rv = vs_reg(g, 1, 11);
                sl = slow_new(g, (const void *)&cl_jit_runtime_struct_set,
                              sa_reg(ra), sa_imm(idx), sa_reg(rv));
                if (sl == NULL) break;
                emit_check_struct(g, ra, idx, sl->entry);
                /* The interpreter's publication barrier: with peer threads,
                 * VAL's initializing stores before the slot that publishes
                 * it (vm.c, OP_STRUCT_SET). */
                l_st = a64_label_new(&g->a);
                emit_ldr_lit64(g, 9, (const void *)&cl_thread_count);
                E(a64_ldr_uoff(4, 9, 9, 0));
                E(a64_cmp_imm(0, 9, 1));
                a64_bcond(&g->a, A64_LS, l_st);
                E(a64_dmb_ish());
                a64_bind(&g->a, l_st);
                E(a64_str_uoff(4, rv, 12, OFF_ST_SLOTS + 4 * idx));
                E(a64_mov_reg(0, 0, rv));
                a64_bind(&g->a, sl->join);
                vs_pop(g, 2);
                vs_push_reg(g, 0);
                depth_add(&d, -1);
                emit_mv1(g);
                break;
            }
            case OP_ASET:
                depth_need(&d, 3);
                emit_peek(g, 0, 3);
                emit_peek(g, 1, 2);
                emit_peek(g, 2, 1);
                emit_call(g, (const void *)&cl_jit_runtime_aset);
                emit_drop(g, 3);
                emit_push(g, 0);
                depth_add(&d, -2);
                emit_mv1(g);
                break;
            case OP_ASSERT_TYPE:
                depth_need(&d, 1);
                emit_peek(g, 0, 1);
                emit_load_const(g, 1, read_u16_be(o));
                emit_call(g, (const void *)&cl_jit_runtime_assert_type);
                break;
            case OP_AREF:
                depth_need(&d, 2);
                emit_peek(g, 0, 2);
                emit_peek(g, 1, 1);
                E(a64_movz(0, 2, o[0], 0));
                emit_call(g, (const void *)&cl_jit_runtime_aref);
                emit_drop(g, 2);
                emit_push(g, 0);
                depth_add(&d, -1);
                emit_mv1(g);
                break;
            case OP_PUSH_LOCAL:
                depth_need(&d, 1);
                emit_sub_x(g, 0, R_TOP, 4);
                emit_add_x(g, 1, R_BP, 4u * o[0]);
                emit_call(g, (const void *)&cl_jit_vmstack_push_local);
                emit_poke(g, 0, 1);
                emit_mv1(g);
                break;
            case OP_POP_LOCAL:
                emit_add_x(g, 0, R_BP, 4u * o[0]);
                emit_call(g, (const void *)&cl_jit_runtime_pop_local);
                emit_push(g, 0);
                depth_add(&d, 1);
                emit_mv1(g);
                break;

            /* --- Calls ---------------------------------------------- */
            case OP_CALL: {
                uint32_t n = o[0];
                depth_need(&d, (int32_t)n + 1);
                emit_direct_call(g, n, -1);
                emit_drop(g, n + 1);
                emit_push(g, 0);
                depth_add(&d, -(int32_t)n);
                break;
            }
            case OP_LOAD_CALL_GLOBAL:
            case OP_GLOAD_CALL_GLOBAL:
            case OP_CALL_GLOBAL: {
                uint32_t n;
                uint16_t k;
                if (op == OP_LOAD_CALL_GLOBAL) {
                    emit_load_local(g, 9, o[0]);
                    emit_push(g, 9);
                    depth_add(&d, 1);
                    o += 1;
                } else if (op == OP_GLOAD_CALL_GLOBAL) {
                    /* The special's value; the callee sets the MV state. */
                    emit_load_const(g, 0, read_u16_be(o));
                    emit_call(g, (const void *)&cl_jit_runtime_gload);
                    emit_push(g, 0);
                    depth_add(&d, 1);
                    o += 2;
                }
                k = read_u16_be(o);
                n = o[2];
                depth_need(&d, (int32_t)n);
                emit_direct_call(g, n, (int32_t)k);
                emit_drop(g, n);
                emit_push(g, 0);
                depth_add(&d, 1 - (int32_t)n);
                break;
            }
            case OP_TAILCALL:
            case OP_TAILCALL_GLOBAL: {
                /* A self tail call (the callee runs this bytecode, the
                 * argument count fits an &optional-at-most lambda list)
                 * reuses the frame: the
                 * arguments over the parameters, the callee as the frame's
                 * function, the other locals NIL again, an empty operand
                 * stack, the loop poll, and back to the body.  Anything else
                 * is a call whose value this function returns.  The self
                 * path cannot be decided at compile time -- the binding may
                 * change -- so a guard decides each time. */
                int global = (op == OP_TAILCALL_GLOBAL);
                uint16_t k = global ? read_u16_be(o) : 0;
                uint32_t n = global ? o[2] : o[0];
                uint32_t fslot = global ? 0 : 1;
                int self = !is_kw && !has_rest && n >= arity && n <= n_pos &&
                           (!global || bc->constants[k] == bc->name);
                depth_need(&d, (int32_t)(n + fslot));
                if (self) {
                    A64Label l_not = a64_label_new(&g->a);
                    if (global) {
                        emit_load_const(g, 0, k);
                        emit_call(g, (const void *)&cl_jit_runtime_fload);
                    } else {
                        emit_peek(g, 0, n + 1);
                    }
                    /* The callee is held across a non-allocating guard. */
                    E(a64_mov_reg(0, R_KEEP, 0));
                    emit_load_local(g, 1, n_locals);
                    emit_call(g, (const void *)&cl_jit_vmstack_is_self);
                    a64_cbz(&g->a, 0, 0, l_not);
                    emit_sub_x(g, 10, R_TOP, 4 * n);
                    for (i = 0; i < n; i++) {
                        E(a64_ldr_uoff(4, 9, 10, 4 * i));
                        emit_store_local(g, 9, i);
                    }
                    emit_store_local(g, R_KEEP, n_locals);
                    for (i = n; i < n_locals; i++)
                        emit_store_local(g, A64_ZR, i);
                    if (n_pos > arity) {
                        /* The new count, for OP_ARGC and the frame. */
                        E(a64_movz(0, R_NARGS, n, 0));
                        E(a64_str_uoff(2, R_NARGS, R_FRAME, OFF_F_NARGS));
                    }
                    emit_add_x(g, R_TOP, R_BP, 4 * (n_locals + 1));
                    emit_loop_poll(g);
                    a64_b(&g->a, l_body);
                    a64_bind(&g->a, l_not);
                }
                /* Any other callee: the tail helper either calls it (w0 is
                 * the value) or hands a native one to cl_jit_invoke. */
                E(a64_mov_reg(1, 0, R_THR));
                E(a64_mov_reg(1, 1, R_TOP));
                E(a64_movz(0, 2, n, 0));
                E(a64_mov_reg(1, 3, R_BP));
                if (global) emit_const_addr(g, 4, k);
                else E(a64_mov_reg(1, 4, A64_ZR));
                emit_call(g, (const void *)&cl_jit_vmstack_tail);
                a64_b(&g->a, l_epilogue);
                /* The code after it (the compiler's OP_RET) is dead unless
                 * a branch lands there; its depth is the call's result. */
                depth_add(&d, 1 - (int32_t)(n + fslot));
                fell = 0;
                break;
            }

            /* --- Branches ------------------------------------------------
             * Every edge leaves with the cache flushed: a label's code
             * expects the frame in memory.  The tested value is read (and
             * popped) first; the flush only stores the values below it. */
            case OP_JMP: {
                int32_t t = landing_ip(read_i32_be(o), ip, bc->code_len);
                vs_flush(g);
                depth_edge(&d, t, insn);
                a64_b(&g->a, ip_lab[t]);
                fell = 0;
                break;
            }
            case OP_JNIL:
            case OP_JTRUE: {
                int32_t t = landing_ip(read_i32_be(o), ip, bc->code_len);
                int ra;
                depth_add(&d, -1);
                ra = vs_reg(g, 1, 10);
                vs_pop(g, 1);
                vs_flush(g);
                depth_edge(&d, t, insn);
                if (op == OP_JNIL) a64_cbz(&g->a, 0, ra, ip_lab[t]);
                else a64_cbnz(&g->a, 0, ra, ip_lab[t]);
                break;
            }
            case OP_LOAD_JNIL: {
                int32_t t = landing_ip(read_i32_be(o + 1), ip, bc->code_len);
                vs_flush(g);
                emit_load_local(g, 10, o[0]);
                depth_edge(&d, t, insn);
                a64_cbz(&g->a, 0, 10, ip_lab[t]);
                break;
            }
            case OP_EQ_JNIL: {
                int32_t t = landing_ip(read_i32_be(o), ip, bc->code_len);
                int ra, rb;
                depth_add(&d, -2);
                ra = vs_reg(g, 2, 10);
                rb = vs_reg(g, 1, 11);
                vs_pop(g, 2);
                vs_flush(g);
                emit_mv1(g);
                depth_edge(&d, t, insn);
                E(a64_cmp_reg(0, ra, rb));
                a64_bcond(&g->a, A64_NE, ip_lab[t]);
                break;
            }
            case OP_GLOAD_JNIL:
            case OP_GLOAD_EQ_JNIL: {
                uint16_t k = read_u16_be(o);
                int32_t t = landing_ip(read_i32_be(o + 2), ip, bc->code_len);
                Slow *sl = slow_new(g, (const void *)&cl_jit_runtime_gload,
                                    sa_const(k), sa_none(), sa_none());
                if (sl == NULL) break;
                emit_symbol_value(g, bc, k, sl->entry);
                a64_bind(&g->a, sl->join);
                if (op == OP_GLOAD_JNIL) {
                    vs_flush(g);
                    emit_mv1(g);
                    depth_edge(&d, t, insn);
                    a64_cbz(&g->a, 0, 0, ip_lab[t]);
                } else {
                    int ra;
                    depth_add(&d, -1);
                    ra = vs_reg(g, 1, 10);
                    vs_pop(g, 1);
                    vs_flush(g);
                    emit_mv1(g);
                    depth_edge(&d, t, insn);
                    E(a64_cmp_reg(0, ra, 0));
                    a64_bcond(&g->a, A64_NE, ip_lab[t]);
                }
                break;
            }
            case OP_CMP_BR: {
                /* Fixnums inline; anything else (and CHAR=, always) through
                 * cl_vm_compare_kind, the interpreter's slow arm.  Either
                 * way w0 is nonzero when the comparison holds. */
                uint8_t kind = o[0];
                uint32_t cmp = (uint32_t)(kind & CL_CMP_BR_CMP_MASK);
                int32_t t = landing_ip(read_i32_be(o + 1), ip, bc->code_len);
                int ra, rb;
                Slow *sl;
                depth_need(&d, 2);
                ra = vs_reg(g, 2, 10);
                rb = vs_reg(g, 1, 11);
                sl = slow_new(g, (const void *)&cl_jit_runtime_cmp_kind,
                              sa_reg(ra), sa_reg(rb), sa_imm(cmp));
                if (sl == NULL) break;
                if (cmp == CL_CMP_BR_CHAREQ) {
                    a64_b(&g->a, sl->entry);
                } else {
                    A64Cond c = cmp == CL_CMP_BR_LT ? A64_LT : cmp == CL_CMP_BR_GT ? A64_GT :
                                cmp == CL_CMP_BR_LE ? A64_LE : cmp == CL_CMP_BR_GE ? A64_GE : A64_EQ;
                    emit_check_fixnums(g, ra, rb, sl->entry);
                    E(a64_cmp_reg(0, ra, rb));
                    E(a64_cset(0, 0, c));
                }
                a64_bind(&g->a, sl->join);
                vs_pop(g, 2);
                vs_flush(g);
                emit_mv1(g);
                depth_add(&d, -2);
                depth_edge(&d, t, insn);
                if (kind & CL_CMP_BR_IF_TRUE)
                    a64_cbnz(&g->a, 0, 0, ip_lab[t]);
                else
                    a64_cbz(&g->a, 0, 0, ip_lab[t]);
                break;
            }

            /* --- NLX frames (runtime_nlx.h) ---------------------------
             * The frame push: alloc, an inline _setjmp, commit on the zero
             * return.  The longjmp arm runs post_longjmp, which restores
             * the frame's marks and cl_vm.sp, pushes what the transfer
             * brought (the value; TAGBODY's tag index) and branches to the
             * landing.  The pops are plain calls; RETURN-FROM and GO do not
             * return. */
            case OP_BLOCK_PUSH: case OP_TAGBODY_PUSH: case OP_CATCH: case OP_UWPROT: {
                int32_t t = landing_ip(read_i32_be(o + op_landing_pos(op)), ip,
                                       bc->code_len);
                int push = (op != OP_UWPROT);
                const void *alloc, *commit, *post;
                A64Label normal;
                if (op == OP_BLOCK_PUSH) {
                    alloc  = (const void *)&cl_jit_runtime_block_alloc;
                    commit = (const void *)&cl_jit_runtime_block_commit;
                    post   = (const void *)&cl_jit_runtime_block_post_longjmp;
                    emit_load_const(g, 0, read_u16_be(o));
                } else if (op == OP_TAGBODY_PUSH) {
                    alloc  = (const void *)&cl_jit_runtime_tagbody_alloc;
                    commit = (const void *)&cl_jit_runtime_tagbody_commit;
                    post   = (const void *)&cl_jit_runtime_tagbody_post_longjmp;
                    emit_load_const(g, 0, read_u16_be(o));
                } else if (op == OP_CATCH) {
                    /* The tag is popped first: the frame's sp is below it. */
                    alloc  = (const void *)&cl_jit_runtime_catch_alloc;
                    commit = (const void *)&cl_jit_runtime_catch_commit;
                    post   = (const void *)&cl_jit_runtime_catch_post_longjmp;
                    depth_add(&d, -1);
                    emit_peek(g, 0, 1);
                    emit_drop(g, 1);
                } else {
                    alloc  = (const void *)&cl_jit_runtime_uwprot_alloc;
                    commit = (const void *)&cl_jit_runtime_uwprot_commit;
                    post   = (const void *)&cl_jit_runtime_uwprot_post_longjmp;
                }
                normal = emit_nlx_setjmp(g, alloc);
                emit_call(g, post);
                emit_reload_top(g);
                if (push) emit_push(g, 0);
                depth_landing(&d, t, insn, d.cur + push);
                a64_b(&g->a, ip_lab[t]);
                a64_bind(&g->a, normal);
                emit_call(g, commit);
                break;
            }
            case OP_HANDLER_CASE_PUSH: {
                /* The landing is a table of one OP_JMP per clause; the arm
                 * pushes the condition and enters the matched clause's. */
                uint16_t ki = read_u16_be(o);
                int32_t t = landing_ip(read_i32_be(o + 2), ip, bc->code_len);
                uint32_t k, nc = handler_case_clauses(bc->constants[ki]);
                A64Label normal;
                emit_load_const(g, 0, ki);
                normal = emit_nlx_setjmp(g, (const void *)&cl_jit_runtime_handler_case_alloc);
                emit_call(g, (const void *)&cl_jit_runtime_handler_case_post_longjmp);
                emit_reload_top(g);
                emit_push(g, 0);
                emit_call(g, (const void *)&cl_jit_runtime_handler_case_clause);
                for (k = 0; k < nc; k++) {
                    depth_landing(&d, t + 5 * (int32_t)k, insn, d.cur + 1);
                    if (k + 1 < nc) {
                        E(a64_cmp_imm(0, 0, k));
                        a64_bcond(&g->a, A64_EQ, ip_lab[t + 5 * k]);
                    } else {
                        a64_b(&g->a, ip_lab[t + 5 * k]);
                    }
                }
                a64_bind(&g->a, normal);
                emit_call(g, (const void *)&cl_jit_runtime_handler_case_commit);
                break;
            }
            case OP_BLOCK_POP:
                emit_call(g, (const void *)&cl_jit_runtime_block_pop);
                break;
            case OP_UNCATCH:
                emit_call(g, (const void *)&cl_jit_runtime_catch_pop);
                break;
            case OP_TAGBODY_POP:
                emit_call(g, (const void *)&cl_jit_runtime_tagbody_pop);
                break;
            case OP_UWPOP:
                emit_call(g, (const void *)&cl_jit_runtime_uwprot_pop);
                break;
            case OP_HANDLER_CASE_POP:
                emit_call(g, (const void *)&cl_jit_runtime_handler_case_pop);
                break;
            case OP_UWRETHROW:
                emit_call(g, (const void *)&cl_jit_runtime_uwprot_rethrow);
                break;
            case OP_BLOCK_RETURN: case OP_TAGBODY_GO:
                /* The value (GO: the tag index) stays on the stack, rooted,
                 * until the transfer.  The compiler counts the form as a
                 * value, so the depth after it is unchanged; the code after
                 * it is dead unless a branch lands there. */
                depth_need(&d, 1);
                emit_load_const(g, 0, read_u16_be(o));
                emit_peek(g, 1, 1);
                emit_call(g, op == OP_BLOCK_RETURN
                             ? (const void *)&cl_jit_runtime_block_return
                             : (const void *)&cl_jit_runtime_tagbody_go);
                fell = 0;
                break;

            /* --- Dynamic binding, handlers, restarts ----------------- */
            case OP_DYNBIND:
                depth_add(&d, -1);
                emit_load_const(g, 0, read_u16_be(o));
                emit_peek(g, 1, 1);
                emit_call(g, (const void *)&cl_jit_runtime_dynbind);
                emit_drop(g, 1);
                break;
            case OP_DYNUNBIND: case OP_HANDLER_POP: case OP_RESTART_POP:
                E(a64_movz(0, 0, o[0], 0));
                emit_call(g, op == OP_DYNUNBIND ? (const void *)&cl_jit_runtime_dynunbind :
                             op == OP_HANDLER_POP ? (const void *)&cl_jit_runtime_handler_pop
                                                  : (const void *)&cl_jit_runtime_restart_pop);
                break;
            case OP_PROGV_BIND: case OP_PROGV_UNBIND:
                /* BIND: symbols, values -> the dynamic-binding mark;
                 * UNBIND: mark, result -> the result. */
                depth_need(&d, 2);
                emit_binary_helper(g, op == OP_PROGV_BIND
                                   ? (const void *)&cl_jit_runtime_progv_bind
                                   : (const void *)&cl_jit_runtime_progv_unbind, 0);
                depth_add(&d, -1);
                break;
            case OP_HANDLER_PUSH:
                depth_add(&d, -1);
                emit_load_const(g, 0, read_u16_be(o));
                emit_peek(g, 1, 1);
                emit_call(g, (const void *)&cl_jit_runtime_handler_push);
                emit_drop(g, 1);
                break;
            case OP_RESTART_PUSH:
                depth_need(&d, 5);
                emit_const_addr(g, 0, read_u16_be(o));
                emit_sub_x(g, 1, R_TOP, 20);
                emit_call(g, (const void *)&cl_jit_vmstack_restart_push);
                emit_drop(g, 5);
                depth_add(&d, -5);
                break;

            /* --- Multiple values ------------------------------------- */
            case OP_MV_LOAD:
                E(a64_movz(0, 0, o[0], 0));
                emit_call(g, (const void *)&cl_jit_runtime_mv_load);
                emit_push(g, 0);
                depth_add(&d, 1);
                break;
            case OP_MV_TO_LIST:
                depth_need(&d, 1);
                emit_peek(g, 0, 1);
                emit_call(g, (const void *)&cl_jit_runtime_mv_to_list);
                emit_poke(g, 0, 1);
                break;
            case OP_NTH_VALUE:
                depth_need(&d, 2);
                emit_binary_helper(g, (const void *)&cl_jit_runtime_nth_value, 0);
                depth_add(&d, -1);
                break;
            case OP_MV_SAVE:
                depth_add(&d, -1);
                emit_peek(g, 0, 1);
                emit_call(g, (const void *)&cl_jit_runtime_mv_save);
                emit_drop(g, 1);
                break;
            case OP_MV_RESTORE:
                emit_call(g, (const void *)&cl_jit_runtime_mv_restore);
                emit_push(g, 0);
                depth_add(&d, 1);
                break;

            /* --- Closures and cells ---------------------------------- */
            case OP_UPVAL:
                emit_upval(g, vs_push(g), n_locals, o[0]);
                depth_add(&d, 1);
                emit_mv1(g);
                break;
            case OP_CELL_REF: {
                /* No type check, as in the interpreter: the compiler only
                 * emits it on a cell. */
                int ra;
                depth_need(&d, 1);
                ra = vs_reg(g, 1, 10);
                E(a64_add_uxtw(12, R_ARENA, ra, 0));
                vs_pop(g, 1);
                E(a64_ldr_uoff(4, vs_push(g), 12, OFF_CELL_VALUE));
                break;
            }
            case OP_CELL_SET_LOCAL: {
                int rv;
                depth_need(&d, 1);
                rv = vs_reg(g, 1, 11);
                emit_load_local(g, 10, o[0]);
                E(a64_add_uxtw(12, R_ARENA, 10, 0));
                E(a64_str_uoff(4, rv, 12, OFF_CELL_VALUE));
                break;
            }
            case OP_CELL_SET_UPVAL:
                /* The helper checks that the upvalue is a cell. */
                depth_need(&d, 1);
                emit_load_local(g, 0, n_locals);
                E(a64_movz(0, 1, o[0], 0));
                emit_peek(g, 2, 1);
                emit_call(g, (const void *)&cl_jit_runtime_cell_set_upval);
                break;
            case OP_MAKE_CELL:
                depth_need(&d, 1);
                emit_peek(g, 0, 1);
                emit_call(g, (const void *)&cl_jit_runtime_make_cell);
                emit_poke(g, 0, 1);
                break;
            case OP_CLOSURE: {
                /* The captures go onto the operand stack -- rooted while the
                 * closure is allocated -- and the helper copies them in. */
                uint16_t ki = read_u16_be(o);
                uint32_t k, nu = closure_template(bc, o)->n_upvalues;
                for (k = 0; k < nu; k++) {
                    uint8_t is_local = o[2 + 2 * k], idx = o[3 + 2 * k];
                    if (is_local) emit_load_local(g, 11, idx);
                    else emit_upval(g, 11, n_locals, idx);
                    emit_push(g, 11);
                }
                depth_add(&d, (int32_t)nu);
                emit_const_addr(g, 0, ki);
                E(a64_movz(0, 1, nu, 0));
                emit_sub_x(g, 2, R_TOP, 4 * nu);
                emit_call(g, (const void *)&cl_jit_vmstack_make_closure);
                emit_drop(g, nu);
                emit_push(g, 0);
                depth_add(&d, 1 - (int32_t)nu);
                emit_mv1(g);
                break;
            }

            default:
                d.bad = 1;      /* op_operand_len and this switch disagree */
                break;
            }
        }
        /* The last opcode must not fall off the end of the code. */
        if (fell) d.bad = 1;
        if (d.bad || g->a.bad || g->oom) goto fail;

        emit_slow_paths(g);

        /* The overflow check: bp + locals + func slot + deepest operand
         * stack must fit the VM stack (the interpreter's push check). */
        a64_bind(&g->a, l_check);
        emit_add_x(g, 9, R_BP, 4 * (n_locals + 1 + (uint32_t)d.max));
        E(a64_ldr_uoff(4, 10, R_THR, OFF_VM_STACK_SIZE));
        E(a64_add_uxtw(10, R_STK, 10, 2));
        E(a64_cmp_reg(1, 9, 10));
        a64_bcond(&g->a, A64_HI, l_overflow);
        a64_b(&g->a, l_back);

        a64_bind(&g->a, l_overflow);
        emit_ldr_lit64(g, 16, (const void *)&cl_jit_vmstack_stack_overflow);
        E(a64_blr(16));                    /* never returns */

        /* The epilogue: the result is in w0. */
        a64_bind(&g->a, l_epilogue);
        E(a64_ldp_off(25, 26, A64_SP, 80));
        E(a64_ldp_off(27, 28, A64_SP, 64));
        E(a64_ldp_off(23, 24, A64_SP, 48));
        E(a64_ldp_off(21, 22, A64_SP, 32));
        E(a64_ldp_off(19, 20, A64_SP, 16));
        E(a64_ldp_post(A64_FP, A64_LR, A64_SP, FRAME_BYTES));
        E(a64_ret());
    }
    finish_literals(g);
    if (g->oom || !a64_finish(&g->a)) goto fail;
    code = cb_finish(&g->cb, &len);
    a64_asm_free(&g->a);
    if (code != NULL) {
        *sites_out = g->sites;          /* the caller's now */
        g->sites = NULL;
    }
    goto lits;

fail:
    a64_asm_free(&g->a);
    cb_free(&g->cb);
    code = NULL;
lits:
    if (g->slow) platform_free(g->slow);
    if (g->lit_val) platform_free(g->lit_val);
    if (g->lit_label) platform_free(g->lit_label);
out:
    if (g->sites) platform_free(g->sites);
    if (tgt) platform_free(tgt);
    if (ip_lab) platform_free(ip_lab);
    if (d.at) platform_free(d.at);
    *len_out = len;
    return code;
}

/* REPLACE: drop the code BC carries and install the new one
 * (cl_jit_compile).  Without it (the hot path) code a peer thread installed
 * meanwhile is kept, and this compile's result dropped. */
void cl_jit_backend_compile(CL_Bytecode *bc, int replace)
{
    uint8_t *code;
    uint32_t len = 0;
    uint64_t *sites = NULL;
    void *entry;

    if (bc == NULL || !cl_jitc_active) return;
    if (replace) drop_native(bc);
    else if (bc->native_code) return;

    code = walk(bc, &len, &sites);
    if (code == NULL) return;
    if (!replace && bc->native_code) {
        platform_free(code);
        if (sites) platform_free(sites);
        return;
    }
    entry = cl_codeheap_install(code, len);
    platform_free(code);
    if (entry == NULL) {
        if (sites) platform_free(sites);
        return;
    }
    /* The direct-call cells ride in native_relocs, which this backend
     * never needs for relocations (it bakes no heap object; the count
     * stays 0): the sweep, drop_native and an image restore free it with
     * the code. */
    bc->native_relocs = (uint32_t *)sites;
    bc->native_reloc_count = 0;
    /* The code is flushed; its bytes must be visible to a peer thread
     * before the pointer that leads there. */
    platform_memory_barrier();
    bc->native_code = (uint8_t *)entry;
    bc->native_len  = len;
    cl_jitc_native_bytes += len;
}

/* Tail calls between native functions (cl_jit_vmstack_tail): the
 * callee's arguments at BP, the callee above them; enter it from the same
 * base, in the frame SF (refilled when PUSHED: a real frame, not the
 * scratch one), until no handoff is pending.  Nothing allocates between
 * the handoff and the read. */
static CL_Obj a64_tail_chain(CL_Thread *t, int bp, CL_Frame *sf, int pushed)
{
    CL_Obj result = CL_NIL;
    while (t->jit_tail_pending) {
        uint32_t n = t->jit_tail_pending - 1;
        CL_Obj func = t->vm.stack[bp + (int)n];
        CL_Bytecode *cbc;
        t->jit_tail_pending = 0;
        t->vm.sp = bp + (int)n + 1;
        cbc = cl_jit_vmstack_native_callee(func, n);
        if (cbc == NULL)              /* defensive: dispatch it as a call */
            return cl_vm_apply(func, &t->vm.stack[bp], (int)n);
        t->jit_invoke_count++;
        if (pushed) {
            sf->bytecode  = func;
            sf->code      = cbc->code;
            sf->constants = cbc->constants;
            sf->n_locals  = cbc->n_locals;
            sf->nargs     = (uint16_t)n;
        }
        sf->ip = 0;
        result = ((a64_entry_t)cbc->native_code)(t, &t->vm.stack[bp], n, func, sf);
    }
    return result;
}

/* The tail-call handoff after a direct call (emit_direct_call): the
 * callee's frame is the top one, its bp the base the handoff left the
 * next callee's arguments at. */
static CL_Obj a64_tail_finish(CL_Thread *t)
{
    CL_Frame *sf = &t->vm.frames[t->vm.fp - 1];
    return a64_tail_chain(t, (int)sf->bp, sf, 1);
}

CL_Obj cl_jit_invoke(CL_Obj func_obj, CL_Bytecode *bc, int nargs)
{
    CL_Obj result;
    CL_Thread *t;
    CL_Frame scratch, *sf = &scratch;
    int saved_sp, bp;
    int pushed_frame = 0;

    if (bc == NULL || bc->native_code == NULL) return CL_NIL;
    t = cl_get_current_thread();
    t->jit_invoke_count++;
    saved_sp = t->vm.sp;
    bp = t->vm.sp - nargs;

    /* The native function's CL_Frame -- the interpreter's own frame, since
     * its locals are on the VM stack: EXT:BACKTRACE lists it, the native
     * code keeps its ip at the opcode it is in (line attribution), and
     * FRAME shows its locals.  On by default here (%JIT-SET-FRAMES turns it
     * off; the code then writes its ip into a scratch frame). */
    if (cl_jitc_shadow_frames) {
        if (t->vm.fp >= t->vm.frame_size)
            cl_error(CL_ERR_OVERFLOW, "Call stack overflow");   /* OP_CALL's */
        sf = &t->vm.frames[t->vm.fp++];
        sf->bytecode  = func_obj;
        sf->code      = bc->code;
        sf->constants = bc->constants;
        sf->bp        = (uint32_t)bp;
        sf->n_locals  = bc->n_locals;
        sf->nargs     = (uint16_t)nargs;
        sf->nlx_level = cl_nlx_top;
        sf->fslot     = 0;
        pushed_frame  = 1;
    }
    sf->ip = 0;

    result = ((a64_entry_t)bc->native_code)(t, &t->vm.stack[bp],
                                            (uint32_t)nargs, func_obj, sf);
    if (t->jit_tail_pending)
        result = a64_tail_chain(t, bp, sf, pushed_frame);

    /* The body wrote sp above its frame at every helper call; the caller
     * pops the arguments from where it pushed them. */
    t->vm.sp = saved_sp;
    if (pushed_frame) t->vm.fp--;
    return result;
}

/* %JIT-COMPILE-STUB: `mov w0, #0; ret` -- returns NIL whatever the
 * function's body would.  Proves the assembler -> code heap -> entry path
 * independently of the walker. */
int cl_jit_emit_stub(CL_Bytecode *bc)
{
    CodeBuf cb;
    A64Asm a;
    uint8_t *code;
    uint32_t len;
    void *entry;

    if (bc == NULL) return 0;
    cb_init(&cb, 8);
    a64_asm_init(&a, &cb);
    a64_emit(&a, a64_movz(0, 0, (uint32_t)CL_NIL, 0));
    a64_emit(&a, a64_ret());
    if (!a64_finish(&a)) { a64_asm_free(&a); cb_free(&cb); return 0; }
    a64_asm_free(&a);
    code = cb_finish(&cb, &len);
    if (code == NULL) return 0;
    entry = cl_codeheap_install(code, len);
    platform_free(code);
    if (entry == NULL) return 0;

    drop_native(bc);
    platform_memory_barrier();
    bc->native_code = (uint8_t *)entry;
    bc->native_len  = len;
    cl_jitc_native_bytes += len;
    return 1;
}

/* --- Disassembler -----------------------------------------------------
 * Decodes the forms the backend emits; anything else (the literal words
 * after the code among them) prints as .word.  Same line format as the
 * m68k one: offset, bytes, mnemonic. */

static const char *const a64_cond_names[16] = {
    "eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc",
    "hi", "ls", "ge", "lt", "gt", "le", "al", "nv"
};

static int32_t sext(uint32_t v, int bits)
{
    uint32_t m = (uint32_t)1 << (bits - 1);
    return (int32_t)((v ^ m) - m);
}

/* "w5", "x20", "wzr", "sp" ... ZR_IS_SP picks what 31 means. */
static const char *rn(char *buf, int sf, uint32_t r, int zr_is_sp)
{
    if (r == 31) return zr_is_sp ? "sp" : (sf ? "xzr" : "wzr");
    snprintf(buf, 8, "%c%u", sf ? 'x' : 'w', r);
    return buf;
}

static void disasm_word(uint32_t w, uint32_t off, char *out, size_t n)
{
    char a[8], b[8], c[8], d[8];
    int sf = (int)(w >> 31);
    uint32_t rd = w & 31u, r1 = (w >> 5) & 31u, r2 = (w >> 16) & 31u;

    if (w == a64_ret()) snprintf(out, n, "ret");
    else if (w == a64_nop()) snprintf(out, n, "nop");
    else if ((w & 0xFFE0001Fu) == 0xD4200000u)
        snprintf(out, n, "brk #0x%x", (w >> 5) & 0xFFFFu);
    else if ((w & 0x7F800000u) == 0x52800000u)
        snprintf(out, n, "movz %s, #0x%x, lsl #%u", rn(a, sf, rd, 0),
                 (w >> 5) & 0xFFFFu, ((w >> 21) & 3u) * 16);
    else if ((w & 0x7F800000u) == 0x72800000u)
        snprintf(out, n, "movk %s, #0x%x, lsl #%u", rn(a, sf, rd, 0),
                 (w >> 5) & 0xFFFFu, ((w >> 21) & 3u) * 16);
    else if ((w & 0x7F800000u) == 0x12800000u)
        snprintf(out, n, "movn %s, #0x%x, lsl #%u", rn(a, sf, rd, 0),
                 (w >> 5) & 0xFFFFu, ((w >> 21) & 3u) * 16);
    else if ((w & 0xFC000000u) == 0x14000000u || (w & 0xFC000000u) == 0x94000000u)
        snprintf(out, n, "%s %ld", (w >> 31) ? "bl" : "b",
                 (long)off + 4L * sext(w & 0x3FFFFFFu, 26));
    else if ((w & 0xFF000010u) == 0x54000000u)
        snprintf(out, n, "b.%s %ld", a64_cond_names[w & 15u],
                 (long)off + 4L * sext((w >> 5) & 0x7FFFFu, 19));
    else if ((w & 0x7E000000u) == 0x34000000u)
        snprintf(out, n, "%s %s, %ld", (w & 0x01000000u) ? "cbnz" : "cbz",
                 rn(a, sf, rd, 0), (long)off + 4L * sext((w >> 5) & 0x7FFFFu, 19));
    else if ((w & 0x7E000000u) == 0x36000000u)
        snprintf(out, n, "%s %s, #%u, %ld", (w & 0x01000000u) ? "tbnz" : "tbz",
                 rn(a, (int)(w >> 31), rd, 0), ((w >> 26) & 0x20u) | ((w >> 19) & 0x1Fu),
                 (long)off + 4L * sext((w >> 5) & 0x3FFFu, 14));
    else if (w == a64_dmb_ish())
        snprintf(out, n, "dmb ish");
    else if ((w & 0xFFFFFC1Fu) == 0xD63F0000u)
        snprintf(out, n, "blr x%u", r1);
    else if ((w & 0xFFFFFC1Fu) == 0xD61F0000u)
        snprintf(out, n, "br x%u", r1);
    else if ((w & 0xBF000000u) == 0x18000000u)
        snprintf(out, n, "ldr %s, %ld", rn(a, (int)((w >> 30) & 1u), rd, 0),
                 (long)off + 4L * sext((w >> 5) & 0x7FFFFu, 19));
    else if ((w & 0x3B000000u) == 0x39000000u) {
        /* LDR/STR (unsigned offset): size in 31:30, load in bit 22. */
        uint32_t size = w >> 30, scale = 1u << size;
        const char *mn = (w & 0x00400000u)
            ? (size == 0 ? "ldrb" : size == 1 ? "ldrh" : "ldr")
            : (size == 0 ? "strb" : size == 1 ? "strh" : "str");
        snprintf(out, n, "%s %s, [%s, #%u]", mn, rn(a, size == 3, rd, 0),
                 rn(b, 1, r1, 1), ((w >> 10) & 0xFFFu) * scale);
    } else if ((w & 0x3B200000u) == 0x38000000u) {
        /* LDUR/STUR, pre- and post-index. */
        uint32_t size = w >> 30, mode = (w >> 10) & 3u;
        int ld = (w & 0x00400000u) != 0;
        int32_t imm = sext((w >> 12) & 0x1FFu, 9);
        if (mode == 0)
            snprintf(out, n, "%s %s, [%s, #%d]", ld ? "ldur" : "stur",
                     rn(a, size == 3, rd, 0), rn(b, 1, r1, 1), (int)imm);
        else if (mode == 1)
            snprintf(out, n, "%s %s, [%s], #%d", ld ? "ldr" : "str",
                     rn(a, size == 3, rd, 0), rn(b, 1, r1, 1), (int)imm);
        else if (mode == 3)
            snprintf(out, n, "%s %s, [%s, #%d]!", ld ? "ldr" : "str",
                     rn(a, size == 3, rd, 0), rn(b, 1, r1, 1), (int)imm);
        else
            snprintf(out, n, ".word 0x%08x", w);
    } else if ((w & 0x3B200C00u) == 0x38200800u)
        snprintf(out, n, "%s %s, [%s, %s, uxtw]", (w & 0x00400000u) ? "ldr" : "str",
                 rn(a, (w >> 30) == 3, rd, 0), rn(b, 1, r1, 1), rn(c, 0, r2, 0));
    else if ((w & 0x7E000000u) == 0x28000000u && (w >> 30) == 2u) {
        /* STP/LDP of X registers: 23:22 index mode + load. */
        uint32_t mode = (w >> 23) & 3u, rt2 = (w >> 10) & 31u;
        int32_t imm = sext((w >> 15) & 0x7Fu, 7) * 8;
        const char *mn = (w & 0x00400000u) ? "ldp" : "stp";
        if (mode == 1)
            snprintf(out, n, "%s %s, %s, [%s], #%d", mn, rn(a, 1, rd, 0),
                     rn(b, 1, rt2, 0), rn(c, 1, r1, 1), (int)imm);
        else if (mode == 3)
            snprintf(out, n, "%s %s, %s, [%s, #%d]!", mn, rn(a, 1, rd, 0),
                     rn(b, 1, rt2, 0), rn(c, 1, r1, 1), (int)imm);
        else
            snprintf(out, n, "%s %s, %s, [%s, #%d]", mn, rn(a, 1, rd, 0),
                     rn(b, 1, rt2, 0), rn(c, 1, r1, 1), (int)imm);
    } else if ((w & 0x1F000000u) == 0x11000000u) {
        /* ADD/SUB(S) immediate; MOV to/from SP; CMP. */
        int sub = (w >> 30) & 1, s = (w >> 29) & 1;
        uint32_t imm = ((w >> 10) & 0xFFFu) << (((w >> 22) & 1u) ? 12 : 0);
        if (!sub && !s && imm == 0 && (rd == 31 || r1 == 31))
            snprintf(out, n, "mov %s, %s", rn(a, sf, rd, 1), rn(b, sf, r1, 1));
        else if (sub && s && rd == 31)
            snprintf(out, n, "cmp %s, #%u", rn(a, sf, r1, 1), imm);
        else
            snprintf(out, n, "%s%s %s, %s, #%u", sub ? "sub" : "add", s ? "s" : "",
                     rn(a, sf, rd, !s), rn(b, sf, r1, 1), imm);
    } else if ((w & 0x1F200000u) == 0x0B000000u) {
        /* ADD/SUB(S) shifted register (no shift emitted). */
        int sub = (w >> 30) & 1, s = (w >> 29) & 1;
        if (sub && s && rd == 31)
            snprintf(out, n, "cmp %s, %s", rn(a, sf, r1, 0), rn(b, sf, r2, 0));
        else
            snprintf(out, n, "%s%s %s, %s, %s", sub ? "sub" : "add", s ? "s" : "",
                     rn(a, sf, rd, 0), rn(b, sf, r1, 0), rn(c, sf, r2, 0));
    } else if ((w & 0xFFE0E000u) == 0x8B204000u)
        snprintf(out, n, "add %s, %s, %s, uxtw #%u", rn(a, 1, rd, 1),
                 rn(b, 1, r1, 1), rn(c, 0, r2, 0), (w >> 10) & 7u);
    else if ((w & 0x7FE0FFE0u) == 0x2A0003E0u)
        snprintf(out, n, "mov %s, %s", rn(a, sf, rd, 0), rn(b, sf, r2, 0));
    else if ((w & 0x7F800000u) == 0x53000000u) {
        /* UBFM: LSR (imms = 31/63) or LSL. */
        uint32_t immr = (w >> 16) & 63u, imms = (w >> 10) & 63u;
        uint32_t top = sf ? 63u : 31u;
        if (imms == top)
            snprintf(out, n, "lsr %s, %s, #%u", rn(a, sf, rd, 0), rn(b, sf, r1, 0), immr);
        else
            snprintf(out, n, "lsl %s, %s, #%u", rn(a, sf, rd, 0), rn(b, sf, r1, 0),
                     top - imms);
    } else if ((w & 0x7FE00C00u) == 0x1A800400u && r1 == 31 && r2 == 31)
        snprintf(out, n, "cset %s, %s", rn(a, sf, rd, 0),
                 a64_cond_names[((w >> 12) & 15u) ^ 1u]);
    else if ((w & 0x7F200000u) == 0x0A000000u)
        snprintf(out, n, "and %s, %s, %s", rn(a, sf, rd, 0), rn(b, sf, r1, 0),
                 rn(c, sf, r2, 0));
    else if ((w & 0xFFE0FC00u) == 0x9B207C00u)
        snprintf(out, n, "smull %s, %s, %s", rn(a, 1, rd, 0), rn(b, 0, r1, 0),
                 rn(c, 0, r2, 0));
    else if ((w & 0xFFE0FC1Fu) == 0xEB20C01Fu)
        snprintf(out, n, "cmp %s, %s, sxtw", rn(a, 1, r1, 1), rn(b, 0, r2, 0));
    else if ((w & 0x7F800000u) == 0x13000000u && ((w >> 10) & 63u) == (sf ? 63u : 31u))
        snprintf(out, n, "asr %s, %s, #%u", rn(a, sf, rd, 0), rn(b, sf, r1, 0),
                 (w >> 16) & 63u);
    else if ((w & 0x7FE00C00u) == 0x1A800000u)
        snprintf(out, n, "csel %s, %s, %s, %s", rn(a, sf, rd, 0), rn(b, sf, r1, 0),
                 rn(c, sf, r2, 0), a64_cond_names[(w >> 12) & 15u]);
    else
        snprintf(out, n, ".word 0x%08x", w);
    (void)d;
}

void cl_jit_disassemble(const uint8_t *code, uint32_t len)
{
    uint32_t off;
    char mnem[80];
    char line[144];
    for (off = 0; off + 4 <= len; off += 4) {
        uint32_t w = (uint32_t)code[off] | ((uint32_t)code[off + 1] << 8)
                   | ((uint32_t)code[off + 2] << 16) | ((uint32_t)code[off + 3] << 24);
        disasm_word(w, off, mnem, sizeof mnem);
        snprintf(line, sizeof line, "  %04lu: %02X %02X %02X %02X        %s\n",
                 (unsigned long)off, code[off], code[off + 1], code[off + 2],
                 code[off + 3], mnem);
        cl_write_cstring_to_stdout(line);
    }
}

#endif /* JIT_A64 */
