/* jit_m68k.c — orchestration for the m68k template JIT.
 *
 * Two codegen paths layered front-to-back:
 *
 *   - Whole-function pattern matchers.  Recognize a small set of
 *     known-tight shapes and emit hand-written templates of optimal
 *     size (4–8 bytes for the body).  Today: 0-arg constant return
 *     (OP_NIL/OP_T/OP_CONST → moveq or move.l#imm32 + rts) and 1..k-arg
 *     parameter pass-through (move.l offset(sp),d0 + rts).
 *
 *   - Per-opcode walker.  Falls through when no matcher fires.  Walks
 *     the bytecode once and emits one m68k template per opcode using
 *     the m68k hardware stack as the operand stack and a LINK'd frame
 *     at A6 for locals.  Bails (leaves native_code NULL) the first
 *     time it sees an opcode it doesn't yet handle; the function then
 *     runs through the bytecode interpreter exactly as before.
 *
 *     Currently supported opcodes: OP_NIL, OP_T, OP_CONST, OP_LOAD,
 *     OP_STORE, OP_POP, OP_DUP, OP_JMP, OP_JNIL, OP_JTRUE, OP_ADD,
 *     OP_SUB, OP_MUL, OP_DIV, OP_EQ, OP_LT, OP_GT, OP_LE, OP_GE,
 *     OP_NUMEQ, OP_NOT, OP_CAR, OP_CDR, OP_CONS, OP_GLOAD, OP_GSTORE,
 *     OP_FLOAD, OP_CALL, OP_TAILCALL, OP_APPLY, OP_STRUCT_REF,
 *     OP_STRUCT_SET, OP_DYNBIND, OP_DYNUNBIND, OP_PROGV_BIND,
 *     OP_PROGV_UNBIND, OP_MV_RESET, OP_MV_TO_LIST, OP_BLOCK_PUSH,
 *     OP_BLOCK_POP, OP_BLOCK_RETURN, OP_CATCH, OP_UNCATCH,
 *     OP_UWPROT, OP_UWPOP, OP_UWRETHROW, OP_AMIGA_CALL, OP_RET.
 *     OP_CONS is the first allocating opcode handled directly.
 *     **Convention for any helper-calling emitter**: after popping
 *     operands into D0/D1 scratch regs, call `cache_flush` before the
 *     JSR.  This spills any remaining D5/D6/D7 cache values to the
 *     m68k stack so the conservative scan
 *     (mem.c::gc_scan_jit_native_stack) reaches them if the helper
 *     allocates and triggers GC — registers are not scanned, and
 *     helpers may allocate even on supposedly-non-allocating paths via
 *     cl_signal_type_error / cl_error condition objects.  All
 *     helper-calling cases below follow this rule.
 *     Adding an opcode means adding one
 *     emitter case and one or two new asm encoders — the walker
 *     handles the composition.
 *
 * See specs/native-backend.md §"Per-opcode emitter shape".
 */

#ifdef JIT_M68K

#include <stddef.h>   /* offsetof (JIT_C_FLOOR_DISP) */
#include <string.h>

#include "jit/jit.h"
#include "jit/codebuf.h"
#include "jit/asm_m68k.h"
#include "jit/codegen_m68k.h"
#include "jit/runtime.h"
#include "jit/runtime_m68k.h"
#include "jit/runtime_nlx.h"
#include "jit/jit_backend.h"
#include "core/opcodes.h"
#include "core/stream.h"
#include "core/thread.h"   /* cl_vm.stack[sp - nargs ... sp - 1] are the args */
#include "core/types.h"
#include "platform/platform.h"

extern int cl_quiet_boot;

void cl_jit_backend_init(void)
{
    cl_jit_runtime_init();
    cl_jit_codegen_init();
    /* Native functions push their CL_Frame (cl_jit_invoke, and inline at a
     * direct call's hit path): backtraces list them, as on arm64. */
    cl_jit_set_shadow_frames(1);
    {
        /* AMILISP_JIT_DIRECT=0: no native-to-native direct calls (A/B runs,
         * bisection); %JIT-SET-DIRECT-CALLS toggles it at run time. */
        char envbuf[8];
        const char *v = platform_getenv("AMILISP_JIT_DIRECT", envbuf, sizeof(envbuf));
        if (v != NULL && v[0] == '0')
            cl_jit_set_direct_calls(0);
    }

    if (!cl_quiet_boot) {
        cl_write_cstring_to_stdout("; [jit] m68k native backend enabled\n");
    }
}

/* --- JIT relocation table ------------------------------------------------
 *
 * The matchers and walker bake heap CL_Obj references (symbols, block /
 * tagbody tags, closure templates, the self-bytecode, FFI base symbols,
 * quoted constants) directly into the generated m68k code as 32-bit
 * immediate operands.  Those immediates are arena-relative offsets; the
 * moving compactor relocates the referenced objects but cannot see the
 * offsets buried in the platform_alloc'd code buffer, so without help
 * they go stale after a compaction and the next execution dereferences
 * garbage (classic symptom: "OP_FLOAD: JIT call site has non-symbol
 * constant 0x........" after a GC fired mid-loop).
 *
 * Fix: every emit of a heap CL_Obj immediate records the byte offset of
 * its 4-byte field here.  cl_jit_compile copies the finished table onto
 * the bytecode (bc->native_relocs); the compactor walks it and forwards
 * each immediate in place (mem.c, TYPE_BYTECODE).  Non-heap immediates
 * (arg counts, slot indices, regspecs, fixnum/char/NIL constants) never
 * move and are deliberately NOT recorded.
 *
 * Sticky OOM mirrors CodeBuf: if a growth allocation fails, `oom` latches
 * and cl_jit_compile discards the native code (the function falls back to
 * the interpreter) rather than ship code with un-forwardable immediates. */
typedef struct {
    uint32_t *offs;
    uint32_t  count;
    uint32_t  cap;
    int       oom;
} JitRelocs;

static void jit_relocs_init(JitRelocs *r)
{
    r->offs = NULL; r->count = 0; r->cap = 0; r->oom = 0;
}

static void jit_relocs_free(JitRelocs *r)
{
    if (r->offs) platform_free(r->offs);
    r->offs = NULL; r->count = 0; r->cap = 0;
}

/* Record that a 4-byte CL_Obj heap immediate lives at native-code byte
 * offset `off`.  Grows the table geometrically; latches `oom` on failure. */
static void jit_reloc_record(JitRelocs *r, uint32_t off)
{
    if (r->oom) return;
    if (r->count == r->cap) {
        uint32_t new_cap = (r->cap == 0) ? 8 : (r->cap * 2);
        uint32_t *grown = (uint32_t *)platform_alloc(
            (unsigned long)new_cap * sizeof(uint32_t));
        if (grown == NULL) { r->oom = 1; return; }
        if (r->offs) {
            uint32_t i;
            for (i = 0; i < r->count; i++) grown[i] = r->offs[i];
            platform_free(r->offs);
        }
        r->offs = grown;
        r->cap = new_cap;
    }
    r->offs[r->count++] = off;
}

/* Match `(defun f () <literal>)` and extract the returned CL_Obj.
 *
 * The compiler emits a fixed 6-byte epilogue for any zero-arg lambda
 * whose body is a single value-producing form:
 *
 *   <push-value>     1 byte  (OP_NIL / OP_T)
 *                or  3 bytes (OP_CONST u16)
 *   OP_STORE 0       2 bytes
 *   OP_POP           1 byte
 *   OP_LOAD  0       2 bytes
 *   OP_RET           1 byte
 *
 * Local 0 is the implicit block return-value slot.  Body has no side
 * effects beyond producing the value, which is what makes it safe to
 * replace with a single immediate-load.
 *
 * Returns 1 and stores the literal value in *value_out on a match.
 * Stays strict on every metadata field so a near-match (different
 * arity, locals, &rest, etc.) never silently lands wrong code.
 */
static int matches_trivial_leaf(const CL_Bytecode *bc, CL_Obj *value_out)
{
    /* The block-result epilogue as the compiler emits it; the peephole
     * pass (spec 4.3) reduces it to the bare RET at any speed above 0. */
    static const uint8_t epilogue[6] = {
        OP_STORE, 0x00,
        OP_POP,
        OP_LOAD,  0x00,
        OP_RET
    };
    uint32_t prefix_len;
    uint8_t op;

    if (bc->arity != 0) return 0;
    if (bc->n_optional != 0) return 0;
    if (bc->flags != 0) return 0;      /* no &key, no allow-other-keys */
    if (bc->n_keys != 0) return 0;
    if (bc->n_upvalues != 0) return 0;
    if (bc->n_locals > 1) return 0;
    if (bc->code_len < 2) return 0;

    op = bc->code[0];
    if (op == OP_NIL || op == OP_T) {
        prefix_len = 1;
    } else if (op == OP_CONST) {
        prefix_len = 3;
    } else {
        return 0;
    }
    if (bc->code_len == prefix_len + 1) {
        if (bc->code[prefix_len] != OP_RET) return 0;
    } else {
        if (bc->code_len != prefix_len + 6) return 0;
        if (memcmp(bc->code + prefix_len, epilogue, 6) != 0) return 0;
    }

    if (op == OP_NIL) {
        *value_out = CL_NIL;
    } else if (op == OP_T) {
        *value_out = CL_T;
    } else {
        /* OP_CONST <u16 index, big-endian> */
        uint16_t idx = ((uint16_t)bc->code[1] << 8) | bc->code[2];
        if (idx >= bc->n_constants || bc->constants == NULL) return 0;
        *value_out = bc->constants[idx];
    }
    return 1;
}

/* Match `(defun f (x1..xk) xj)` — k-arg parameter pass-through.
 *
 * Body `xj` compiles to `OP_LOAD <j>`; the implicit block-return
 * postlude adds `OP_STORE <k> ; OP_POP ; OP_LOAD <k>` (slot k is the
 * block-return cell since slots 0..k-1 hold the k parameters), and the
 * function trailer adds `OP_MV_RESET ; OP_RET`.  Total 9 bytes
 * regardless of k:
 *
 *   OP_LOAD  j   2 bytes   (0 <= j < k)
 *   OP_STORE k   2 bytes
 *   OP_POP       1 byte
 *   OP_LOAD  k   2 bytes
 *   OP_MV_RESET  1 byte
 *   OP_RET       1 byte
 *
 * The OP_MV_RESET is not optional: OP_LOAD leaves the multiple-value
 * buffer describing whatever ran before the call, so without it
 * `(f (values 1 2))` would return TWO values (compiler MV hygiene, see
 * cl_mv_normalize).  The emitter below therefore calls the same
 * one-line helper the walker's OP_MV_RESET case uses before loading the
 * argument — still far cheaper than a LINK'd walker frame.
 *
 * No allocation, no side effects otherwise — safe to collapse to
 * "reset mv, load arg, return".  Strict on metadata so optional/&key/
 * &rest variants don't sneak through.
 *
 * Capped at `CL_JIT_MAX_POSITIONAL` (jit.h), the positional arity
 * cl_jit_invoke accepts.
 *
 * Returns 1 and stores the source slot j in *slot_out on match. */

static int matches_passthrough(const CL_Bytecode *bc, uint8_t *slot_out)
{
    uint8_t arity, slot;

    if (bc->arity < 1 || bc->arity > CL_JIT_MAX_POSITIONAL) return 0;
    if (bc->n_optional != 0) return 0;
    if (bc->flags != 0) return 0;
    if (bc->n_keys != 0) return 0;
    if (bc->n_upvalues != 0) return 0;
    arity = (uint8_t)bc->arity;
    if (bc->n_locals != (uint16_t)(arity + 1)) return 0;

    /* The peephole pass (spec 4.3) leaves `LOAD_MV_RESET j; RET` of this
     * body; the 9-byte shape below is the compiler's own emission, seen
     * at (speed 0). */
    if (bc->code_len == 3) {
        if (bc->code[0] != OP_LOAD_MV_RESET) return 0;
        slot = bc->code[1];
        if (slot >= arity) return 0;
        if (bc->code[2] != OP_RET) return 0;
        *slot_out = slot;
        return 1;
    }
    if (bc->code_len != 9) return 0;

    if (bc->code[0] != OP_LOAD)  return 0;
    slot = bc->code[1];
    if (slot >= arity) return 0;
    if (bc->code[2] != OP_STORE) return 0;
    if (bc->code[3] != arity)    return 0;
    if (bc->code[4] != OP_POP)   return 0;
    if (bc->code[5] != OP_LOAD)  return 0;
    if (bc->code[6] != arity)    return 0;
    if (bc->code[7] != OP_MV_RESET) return 0;
    if (bc->code[8] != OP_RET)   return 0;

    *slot_out = slot;
    return 1;
}

/* Pick the shortest m68k encoding that materializes `val` in D0.
 * MOVEQ sign-extends an 8-bit immediate, so a CL_Obj whose 32-bit
 * value lies in [-128, 127] (interpreted as int32) round-trips
 * through it exactly.  Everything else needs the full 6-byte
 * MOVE.L #imm32. */
static void emit_load_imm_d0(CodeBuf *cb, CL_Obj val)
{
    int32_t sv = (int32_t)val;
    if (sv >= -128 && sv <= 127) {
        m68k_emit_moveq(cb, (int8_t)sv, REG_D0);
    } else {
        m68k_emit_move_l_imm32(cb, (uint32_t)val, REG_D0);
    }
}

/* --- Three-slot rotating stack cache -------------------------------------
 *
 * The walker keeps the top 1–3 elements of the VM operand stack in the
 * data registers D5/D6/D7, with anything below them on the m68k
 * hardware stack via predec/postinc.  Rather than shift values
 * between registers on every push/pop, the cache rotates: an emit-
 * time variable `cache_head ∈ {5, 6, 7}` tells us which register
 * currently holds TOS.  Successive pushes rotate head forward
 * (5→6→7→5); successive pops rotate it backward (7→6→5→7).  This
 * eliminates all register shuffling on push and pop — each is at
 * most one MOVE.L (the actual value transfer), independent of cache
 * depth.
 *
 * Layout invariant at any emit point with `depth >= 1`:
 *   - D{head}              = TOS
 *   - D{prev(head)}        = 2nd-from-top, valid when depth >= 2
 *   - D{prev(prev(head))}  = 3rd-from-top, valid when depth == 3
 *     (equivalently: D{next(head)} — three regs, two rotations back =
 *     one forward)
 *
 * Helpers (JSR) need their args on the m68k stack with the topmost
 * at (a7).  Ops that call helpers flush the cache first; the helper's
 * D0 return value is then pushed back through cache_push_dn so
 * subsequent ops continue in cache.
 *
 * Branches require the depth=0 invariant at every branch boundary: a
 * pre-scan computes is_branch_target[ip], and the walker flushes the
 * cache at every such IP before emitting that opcode's code.  Each
 * branch instruction also flushes before testing/jumping so the
 * target's depth=0 invariant always holds.
 *
 * D5/D6/D7 are callee-saved per the m68k SysV ABI; the walker's
 * prologue pushes them below the LINK frame and OP_RET restores via
 * A6-relative loads before UNLK.  See walker_compile.
 */

#define CL_JIT_CACHE_MAX 3

/* Rotate a head index forward (5→6, 6→7, 7→5) — used by push.  Static
 * inlining is fine for hot emitter helpers; the compiler will fold
 * branches when constants are known. */
static int cache_next(int head) { return (head == 7) ? 5 : head + 1; }
static int cache_prev(int head) { return (head == 5) ? 7 : head - 1; }

/* Emit the cache→stack spill for a given (head, depth) without
 * mutating any compile-time bookkeeping.  Order: bottom-of-cache first,
 * TOS last, so the spilled TOS ends up at (a7) — matching the
 * operand-stack layout uncached code expects. */
static void emit_cache_spill(CodeBuf *cb, int head, int depth)
{
    if (depth >= 3) {
        m68k_emit_move_l_dn_predec_an(cb, (M68kReg)cache_next(head), REG_A7);
    }
    if (depth >= 2) {
        m68k_emit_move_l_dn_predec_an(cb, (M68kReg)cache_prev(head), REG_A7);
    }
    if (depth >= 1) {
        m68k_emit_move_l_dn_predec_an(cb, (M68kReg)head, REG_A7);
    }
}

/* Inverse of emit_cache_spill: reload the same regs from the m68k stack
 * via postinc, restoring the cache to (head, depth) as it was before the
 * spill.  Used by slow-path shims of inline-fast-path opcodes to round-
 * trip cached heap pointers through the conservatively-scanned stack
 * across a helper JSR that may GC. */
static void emit_cache_reload(CodeBuf *cb, int head, int depth)
{
    if (depth >= 1) {
        m68k_emit_move_l_postinc_an_to_dn(cb, REG_A7, (M68kReg)head);
    }
    if (depth >= 2) {
        m68k_emit_move_l_postinc_an_to_dn(cb, REG_A7, (M68kReg)cache_prev(head));
    }
    if (depth >= 3) {
        m68k_emit_move_l_postinc_an_to_dn(cb, REG_A7, (M68kReg)cache_next(head));
    }
}

/* Spill the entire cache to the m68k stack.  After: depth=0.  Pushes
 * from the bottom of the cache (oldest) to the top so the spilled TOS
 * ends up at (a7), matching the operand-stack layout uncached code
 * expects.  Resets head to 7 (the default starting position; depth=0
 * means head is unused, but staying canonical keeps emitted code
 * deterministic for any given input). */
static void cache_flush(CodeBuf *cb, int *head, int *depth)
{
    emit_cache_spill(cb, *head, *depth);
    *depth = 0;
    *head = 7;
}

/* Make room at the top of the cache for one new value.  Rotates head
 * forward; if the cache was already full, spills the register that's
 * about to be overwritten (= the bottom-of-cache, sitting at
 * cache_next(head) before the rotation).  After this returns, the
 * register at D{*head} is free for the caller to load.
 *
 * Depth grows by 1 unless we were at MAX, in which case it stays at
 * MAX (the spilled item lives on the m68k stack instead). */
static void cache_make_room_for_push(CodeBuf *cb, int *head, int *depth)
{
    int new_head = cache_next(*head);
    if (*depth == CL_JIT_CACHE_MAX) {
        /* About to overwrite D{new_head}, which holds the bottom-of-
         * cache value.  Spill it to the stack first. */
        m68k_emit_move_l_dn_predec_an(cb, (M68kReg)new_head, REG_A7);
    } else {
        (*depth)++;
    }
    *head = new_head;
}

/* Push a 32-bit immediate value onto the cache. */
static void cache_push_imm(CodeBuf *cb, int *head, int *depth, uint32_t imm)
{
    cache_make_room_for_push(cb, head, depth);
    if ((int32_t)imm >= -128 && (int32_t)imm <= 127) {
        m68k_emit_moveq(cb, (int8_t)imm, (M68kReg)*head);
    } else {
        m68k_emit_move_l_imm32(cb, imm, (M68kReg)*head);
    }
}

/* Push the longword at (disp,An) into the cache as the new TOS. */
static void cache_push_disp_an(CodeBuf *cb, int *head, int *depth,
                               int16_t disp, M68kReg an)
{
    cache_make_room_for_push(cb, head, depth);
    m68k_emit_move_l_disp_an_to_dn(cb, disp, an, (M68kReg)*head);
}

/* Push the value in Dn into the cache as the new TOS.  Dn may be one
 * of the cache regs only if it happens to coincide with the new
 * head — the rotation may have just freed it (or it may have held a
 * stale value being overwritten), but emitting a self-move is
 * harmless.  In practice the walker only pushes from D0 (helper
 * return value / arith result). */
static void cache_push_dn(CodeBuf *cb, int *head, int *depth, M68kReg dn)
{
    cache_make_room_for_push(cb, head, depth);
    if (dn != (M68kReg)*head) {
        m68k_emit_move_l_dn_to_dm(cb, dn, (M68kReg)*head);
    }
}

/* Pop the cache's TOS into Dn.  Cached (depth>=1): one MOVE.L (or
 * zero if Dn == current TOS reg) plus a head rotation backward and a
 * depth decrement — no register shuffling.  Uncached (depth=0):
 * falls back to `move.l (a7)+,Dn`.
 *
 * Dn must not collide with one of the cache regs *unless* it
 * coincides with the current TOS reg (in which case the move is
 * skipped and the rotation reclaims it as the next-to-pop slot).
 * In practice callers use D0/D1/D2. */
static void cache_pop_to_dn(CodeBuf *cb, int *head, int *depth, M68kReg dn)
{
    if (*depth == 0) {
        m68k_emit_move_l_postinc_an_to_dn(cb, REG_A7, dn);
        return;
    }
    if (dn != (M68kReg)*head) {
        m68k_emit_move_l_dn_to_dm(cb, (M68kReg)*head, dn);
    }
    *head = cache_prev(*head);
    (*depth)--;
}

/* Pop the cache's TOS and discard the value.  Same rotation logic as
 * cache_pop_to_dn but no destination — used by OP_POP. */
static void cache_drop(CodeBuf *cb, int *head, int *depth)
{
    if (*depth == 0) {
        m68k_emit_addq_l_an(cb, 4, REG_A7);
        return;
    }
    *head = cache_prev(*head);
    (*depth)--;
}

/* Duplicate the cache's TOS (push another copy).  Cached: rotate
 * forward, then copy the old TOS register into the new TOS register.
 * Uncached: the existing `move.l (a7),-(a7)` memory dup. */
static void cache_dup(CodeBuf *cb, int *head, int *depth)
{
    if (*depth == 0) {
        m68k_emit_move_l_an_to_predec_am(cb, REG_A7, REG_A7);
        return;
    }
    {
        M68kReg old_tos = (M68kReg)*head;
        cache_make_room_for_push(cb, head, depth);
        m68k_emit_move_l_dn_to_dm(cb, old_tos, (M68kReg)*head);
    }
}

/* --- shared helper-call emit templates -----------------------------------
 *
 * The "flush the operand cache → push args as C-ABI operands → JSR the
 * runtime helper → drop args → push the D0 result" sequence is emitted
 * verbatim at ~a dozen opcodes.  The cache_flush is load-bearing: cached
 * D5/D6/D7 heap values are invisible to the conservative JIT-stack scan
 * (mem.c::gc_scan_jit_native_stack), so any helper that may allocate (or
 * merely cl_error / cl_signal_type_error) must run with the cache
 * spilled to the m68k stack.  Callers pop the operands into registers
 * first: D1=b/D0=a for the 2-arg form, D0 for the 1-arg form.  Emit-time
 * only — the generated m68k is byte-identical to the inline sequences. */
static void emit_helper_call_2(CodeBuf *cb, int *head, int *depth, uint32_t helper)
{
    cache_flush(cb, head, depth);
    m68k_emit_move_l_dn_predec_an(cb, REG_D1, REG_A7);
    m68k_emit_move_l_dn_predec_an(cb, REG_D0, REG_A7);
    m68k_emit_jsr_abs_l(cb, helper);
    m68k_emit_addq_l_an(cb, 8, REG_A7);
    cache_push_dn(cb, head, depth, REG_D0);
}

/* 1-arg form.  `flush` is 0 only for the handful of non-allocating,
 * non-signalling helpers (e.g. cl_jit_runtime_cell_ref) that are safe to
 * call with the cache still live. */
static void emit_helper_call_1(CodeBuf *cb, int *head, int *depth,
                               uint32_t helper, int flush)
{
    if (flush)
        cache_flush(cb, head, depth);
    m68k_emit_move_l_dn_predec_an(cb, REG_D0, REG_A7);
    m68k_emit_jsr_abs_l(cb, helper);
    m68k_emit_addq_l_an(cb, 4, REG_A7);
    cache_push_dn(cb, head, depth, REG_D0);
}

/* --- CL_Obj immediate emitters (relocation-aware) ------------------------
 *
 * Every place that bakes a heap CL_Obj as a literal operand routes
 * through one of these so the offset is recorded in the JitRelocs table
 * (see "JIT relocation table" above).  Raw-integer operands (arg counts,
 * slot/constant indices, regspecs, FFI offsets) are NOT heap references
 * and keep calling the bare m68k_emit_* encoders directly. */

/* Emit `move.l #obj,-(a7)` for a CL_Obj C-ABI arg, recording a reloc
 * when obj is a heap pointer.  (+2 skips the 2-byte opcode to point at
 * the 4-byte immediate field.) */
static void emit_obj_imm_predec(CodeBuf *cb, JitRelocs *r, CL_Obj obj)
{
    if (CL_HEAP_P(obj))
        jit_reloc_record(r, cb_len(cb) + 2);
    m68k_emit_move_l_imm32_predec(cb, (uint32_t)obj, REG_A7);
}

/* Load a CL_Obj literal into D0, recording a reloc for heap pointers.
 * Forces the 32-bit MOVE.L #imm form for heap objects (rather than the
 * 8-bit MOVEQ the plain helper might pick) so the reloc always patches a
 * full 4-byte field. */
static void emit_obj_imm_d0(CodeBuf *cb, JitRelocs *r, CL_Obj val)
{
    if (CL_HEAP_P(val)) {
        jit_reloc_record(r, cb_len(cb) + 2);
        m68k_emit_move_l_imm32(cb, (uint32_t)val, REG_D0);
    } else {
        emit_load_imm_d0(cb, val);
    }
}

/* Push a CL_Obj literal onto the operand cache, recording a reloc for
 * heap pointers and forcing the 32-bit immediate form (MOVEQ carries
 * only 8 bits and could not be reliably forwarded). */
static void cache_push_obj(CodeBuf *cb, int *head, int *depth,
                           JitRelocs *r, CL_Obj val)
{
    if (!CL_HEAP_P(val)) {
        cache_push_imm(cb, head, depth, (uint32_t)val);
        return;
    }
    cache_make_room_for_push(cb, head, depth);
    jit_reloc_record(r, cb_len(cb) + 2);
    m68k_emit_move_l_imm32(cb, (uint32_t)val, (M68kReg)*head);
}

/* --- Per-opcode walker ---------------------------------------------------
 *
 * The walker emits a complete native function for any bytecode built
 * from the small but growing set of opcodes below.  It uses A6 as a
 * frame pointer (set up via LINK) and the m68k hardware stack (A7) as
 * the operand stack.
 *
 * Stack frame, after `link a6,#-N` and the cache-reg save:
 *
 *   8(a6)            func_obj (closure or raw bytecode CL_Obj) —
 *                    pushed last by cl_jit_enter (or a native caller)
 *                    so OP_UPVAL / OP_CELL_SET_UPVAL can dereference
 *                    cl->upvalues[index]
 *  12(a6) + 4*(arity-1-i)
 *                    parameter i (i in [0, arity)), in operand-stack
 *                    order: the LAST parameter sits at 12(a6), the
 *                    first one highest (see slot_disp)
 *   4(a6)            return address (pushed by JSR)
 *   0(a6)            saved A6 (pushed by LINK)
 *  -4(a6) - 4*j      "extra" local j (j = slot - arity, j in [0, n_extra))
 *                    (&optional and &key: every slot is down here, see
 *                    slot_disp and emit_opt_prologue)
 *  -N-4(a6)          saved D7 (pushed first after LINK)
 *  -N-8(a6)          saved D6
 *  -N-12(a6)         saved D5
 *   (a7)             operand-stack TOS (uncached portion), grows downward
 *   D7 / D6 / D5     top 1–3 elements when cache_depth > 0 (see §"Three-
 *                    slot stack cache" earlier in this file)
 *
 * Registers: D0/D1/A0/A1 scratch, D5-D7 the stack cache, A6 the frame,
 * A7 the operand stack.  A3 = the current CL_Thread *, loaded by
 * cl_jit_enter (jit_enter_m68k.s) and callee-saved through every helper
 * and OS call; native call sites read CL_Thread.jit_c_floor through it
 * (specs/jit-direct-calls.md §4).  NEVER allocate or clobber A3 in a
 * template or a hand-written helper.  A2/A4/A5 are unused.
 *
 * For bytecode slot s, slot_disp() picks the right displacement.
 *
 * Branches are resolved in a single pass with a small forward-patch
 * list.  `bc_to_native[ip]` records the native-code offset at which the
 * opcode beginning at bytecode IP `ip` starts emitting.  Backward
 * branches read directly from this map; forward branches emit a Bcc.W
 * with placeholder 0 and remember (patch_off, target_bc_off) for later
 * fix-up.  Once the walker reaches OP_RET we resolve every pending
 * patch.  If a 16-bit displacement won't fit we bail out — the function
 * runs interpreted instead.  (32-bit Bcc.L exists on 68020+ but isn't
 * needed at the function sizes the walker handles today.)
 *
 * On any unsupported opcode the walker returns 0 and the caller drops
 * the partially-built buffer — the function then runs interpreted.
 */

/* Forward branch waiting for its target's native offset.  The branch
 * opcode word lives at (patch_off - 2); the 16-bit displacement field
 * the patcher overwrites lives at patch_off.  The displacement is
 * computed relative to patch_off itself, since m68k Bcc.W references
 * PC = (instruction_start + 2) = patch_off. */
typedef struct {
    uint32_t patch_off;      /* native byte offset of the disp word */
    uint32_t target_bc_off;  /* bytecode offset of the branch target */
} BranchPatch;

/* A6-relative displacement of bytecode slot `slot`.
 *
 * Required parameters only (in_frame == 0): the first `slot_anchor` slots (= the
 * required-arg count) live in the positional-arg slots above A6;
 * remaining slots are in the LINK frame below A6.  The arguments are
 * in OPERAND-STACK order — the order a caller's operand stack already
 * holds them in (last argument at the lowest address), so a native
 * caller can push `func_obj` over its arguments and JSR without
 * re-copying them (specs/jit-direct-calls.md §4).  After LINK a6,#-N
 * the call frame above A6 of an arity-n function is:
 *
 *   8(a6)            = `func_obj`   (closure or bytecode)
 *   12(a6)           = user arg n-1
 *   16(a6)           = user arg n-2
 *   …
 *   12+4*(n-1)(a6)   = user arg 0
 *
 * so user slot s sits at 12 + 4*(n-1-s).  Slot slot_anchor (first
 * non-arg local) sits at -4(a6), slot_anchor+1 at -8(a6), and so on.
 *
 * &optional, &rest and &key (in_frame == 1): every slot lives below A6
 * in the LINK frame, laid out FORWARD so the helper's plain C array
 * indexing `frame[i]` matches slot i.  The walker passes the helper a
 * pointer to the lowest slot (`-(4*(n_locals+1))(a6)`), so slot 0 → that
 * address and slot n_locals-1 → -8(a6); the argument count takes -4(a6)
 * (see emit_opt_prologue).  `slot_anchor` here is `n_locals + 1`. */
static int16_t slot_disp(uint8_t slot, uint16_t slot_anchor, int in_frame)
{
    if (in_frame) {
        return (int16_t)(-4 * ((int32_t)slot_anchor - (int32_t)slot));
    }
    if (slot < slot_anchor) {
        return (int16_t)(12 + 4 * ((int)slot_anchor - 1 - (int)slot));
    }
    return (int16_t)(-4 * ((int)slot - (int)slot_anchor + 1));
}

/* Decode a 4-byte big-endian signed integer from the bytecode stream.
 * Matches the encoding cl_emit_i32 produces for branch operands. */
static int32_t read_i32_be(const uint8_t *p)
{
    return (int32_t)(((uint32_t)p[0] << 24) |
                     ((uint32_t)p[1] << 16) |
                     ((uint32_t)p[2] <<  8) |
                      (uint32_t)p[3]);
}

/* Patch list grows on demand starting from this size.  A pessimistic
 * upper bound is code_len / 5 (every byte a branch instruction), but
 * realistic Lisp rarely needs more than a dozen forward branches per
 * function.  Doubling on overflow keeps amortised cost O(1). */
#define WALKER_PATCH_INITIAL_CAP 8

/* Emit a 2-operand arithmetic template (OP_ADD / OP_SUB).  Takes its
 * operands already in D0=a, D1=b; leaves the result in D0.  Doesn't
 * touch the operand stack — the caller's cache primitives handle
 * sourcing the operands and pushing the result back.  `is_sub == 0`
 * produces an ADD; `is_sub == 1` produces a SUB.
 *
 * Tag accounting: both operands carry bit-0 tag = 1.  For ADD one tag
 * is stripped from a FIRST (SUBQ #1 — a is odd, so this can't
 * overflow), and the ADD then yields the tagged sum 2(va+vb)+1
 * directly.  For SUB the raw tagged difference cancels both tags, so
 * ADDQ #1 sets bit-0 back.
 *
 * Why the tag is stripped before the ADD and not after: the V flag of
 * a 32-bit add is only an exact fixnum-overflow test when the value it
 * is computed on is the final tagged result.  Adding the two tagged
 * operands first gives 2(va+vb)+2, and for va+vb = -2^30-1 (e.g.
 * most-negative-fixnum + -1) that is exactly INT_MIN — representable,
 * V clear — and the SUBQ #1 that stripped the surplus tag afterwards
 * wrapped it to 0x7FFFFFFF: tagged most-positive-fixnum, silently.
 * With the tag stripped first every sum outside the fixnum range
 * overflows the 32-bit add, and every sum inside it does not.  The
 * SUB template never had the hole: 2(va-vb) is in range exactly when
 * va-vb is, and the ADDQ #1 cannot overflow from an even value.
 *
 * Overflow recovery (BVS taken): D0 holds the 32-bit-wrapped result.
 * For ADD, original a = wrapped - b + 1, so SUB d1,d0 / ADDQ #1,d0;
 * for SUB, original a = wrapped + b, so ADD d1,d0.  Either way D0 ends
 * up with a, D1 still holds b, and we fall into the common slow-path
 * JSR.
 *
 * Caller-saved discipline: D0/D1/A0/A1 only — D5/D6/D7 are the cache
 * regs and stay untouched. */
static void emit_arith_compute(CodeBuf *cb, int is_sub, uint32_t helper,
                               int cache_head, int cache_depth)
{
    int32_t beq_a_pc, beq_b_pc, bvs_pc, bra_pc;
    int32_t slow_off, done_off;

    m68k_emit_btst_imm_dn(cb, 0, REG_D0);
    beq_a_pc = (int32_t)cb_len(cb) + 2;
    m68k_emit_beq_w(cb, 0);
    m68k_emit_btst_imm_dn(cb, 0, REG_D1);
    beq_b_pc = (int32_t)cb_len(cb) + 2;
    m68k_emit_beq_w(cb, 0);

    if (is_sub) {
        m68k_emit_sub_l_dn_to_dm(cb, REG_D1, REG_D0);   /* 2(va-vb): tags cancel */
    } else {
        m68k_emit_subq_l_dn(cb, 1, REG_D0);             /* strip a's tag first */
        m68k_emit_add_l_dn_to_dm(cb, REG_D1, REG_D0);   /* 2(va+vb)+1: V is exact */
    }
    bvs_pc = (int32_t)cb_len(cb) + 2;
    m68k_emit_bvs_w(cb, 0);

    if (is_sub) m68k_emit_addq_l_dn(cb, 1, REG_D0);  /* tag was cancelled */
    bra_pc = (int32_t)cb_len(cb) + 2;
    m68k_emit_bra_w(cb, 0);

    /* Overflow-recovery shim: reconstruct original a in D0. */
    {
        int32_t overflow_off = (int32_t)cb_len(cb);
        if (is_sub) {
            m68k_emit_add_l_dn_to_dm(cb, REG_D1, REG_D0);
        } else {
            m68k_emit_sub_l_dn_to_dm(cb, REG_D1, REG_D0);
            m68k_emit_addq_l_dn(cb, 1, REG_D0);
        }
        m68k_patch_disp16(cb_data(cb), cb_len(cb),
                          (uint32_t)bvs_pc,
                          (int16_t)(overflow_off - bvs_pc));
    }

    /* Slow-path shim.  Cache spill lives here, not in the caller's
     * pre-amble, so the fixnum fast path skips it entirely.  The reload
     * after the JSR is required for GC correctness: cached heap pointers
     * in D5/D6/D7 are invisible to the conservative scan, so they round-
     * trip through the m68k stack where the scan reaches them — and where
     * the compactor will update them in place if the helper triggers GC.
     * D5/D6/D7 are callee-saved per the m68k SysV ABI so the helper does
     * not perturb the spilled slots between push and reload. */
    slow_off = (int32_t)cb_len(cb);
    emit_cache_spill(cb, cache_head, cache_depth);
    m68k_emit_move_l_dn_predec_an(cb, REG_D1, REG_A7);
    m68k_emit_move_l_dn_predec_an(cb, REG_D0, REG_A7);
    m68k_emit_jsr_abs_l(cb, helper);
    m68k_emit_addq_l_an(cb, 8, REG_A7);
    emit_cache_reload(cb, cache_head, cache_depth);
    done_off = (int32_t)cb_len(cb);

    m68k_patch_disp16(cb_data(cb), cb_len(cb), (uint32_t)beq_a_pc,
                      (int16_t)(slow_off - beq_a_pc));
    m68k_patch_disp16(cb_data(cb), cb_len(cb), (uint32_t)beq_b_pc,
                      (int16_t)(slow_off - beq_b_pc));
    m68k_patch_disp16(cb_data(cb), cb_len(cb), (uint32_t)bra_pc,
                      (int16_t)(done_off - bra_pc));
}

/* Emit a 2-operand comparison template (OP_LT / OP_GT / OP_LE /
 * OP_GE / OP_NUMEQ).  Takes its operands already in D0=a, D1=b;
 * leaves CL_T or CL_NIL in D0.  Same boundary-free contract as
 * emit_arith_compute.  `cond_code` is the m68k Bcc condition that
 * branches to load_t (e.g. 13=BLT, 14=BGT, 12=BGE, 15=BLE, 7=BEQ).
 * The slow-path helper itself returns CL_T/CL_NIL in D0. */
static void emit_compare_compute(CodeBuf *cb, uint8_t cond_code, uint32_t helper,
                                 int cache_head, int cache_depth, JitRelocs *relocs)
{
    int32_t beq_a_pc, beq_b_pc, taken_pc, bra1_pc, bra2_pc;
    int32_t slow_off, done_off, load_t_off;

    m68k_emit_btst_imm_dn(cb, 0, REG_D0);
    beq_a_pc = (int32_t)cb_len(cb) + 2;
    m68k_emit_beq_w(cb, 0);
    m68k_emit_btst_imm_dn(cb, 0, REG_D1);
    beq_b_pc = (int32_t)cb_len(cb) + 2;
    m68k_emit_beq_w(cb, 0);

    m68k_emit_cmp_l_dn_dm(cb, REG_D1, REG_D0);  /* flags = a - b */
    taken_pc = (int32_t)cb_len(cb) + 2;
    m68k_emit_bcc_w(cb, cond_code, 0);
    /* Not-taken: load CL_NIL (= 0) into D0. */
    m68k_emit_moveq(cb, 0, REG_D0);
    bra1_pc = (int32_t)cb_len(cb) + 2;
    m68k_emit_bra_w(cb, 0);

    /* Taken: load CL_T into D0 (reloc-recorded — CL_T is a heap symbol
     * whose offset shifts under a compacting GC). */
    load_t_off = (int32_t)cb_len(cb);
    emit_obj_imm_d0(cb, relocs, CL_T);
    bra2_pc = (int32_t)cb_len(cb) + 2;
    m68k_emit_bra_w(cb, 0);

    /* Slow-path shim: same spill/reload contract as emit_arith_compute
     * (see comment there). */
    slow_off = (int32_t)cb_len(cb);
    emit_cache_spill(cb, cache_head, cache_depth);
    m68k_emit_move_l_dn_predec_an(cb, REG_D1, REG_A7);
    m68k_emit_move_l_dn_predec_an(cb, REG_D0, REG_A7);
    m68k_emit_jsr_abs_l(cb, helper);
    m68k_emit_addq_l_an(cb, 8, REG_A7);
    emit_cache_reload(cb, cache_head, cache_depth);
    done_off = (int32_t)cb_len(cb);

    m68k_patch_disp16(cb_data(cb), cb_len(cb), (uint32_t)beq_a_pc,
                      (int16_t)(slow_off  - beq_a_pc));
    m68k_patch_disp16(cb_data(cb), cb_len(cb), (uint32_t)beq_b_pc,
                      (int16_t)(slow_off  - beq_b_pc));
    m68k_patch_disp16(cb_data(cb), cb_len(cb), (uint32_t)taken_pc,
                      (int16_t)(load_t_off - taken_pc));
    m68k_patch_disp16(cb_data(cb), cb_len(cb), (uint32_t)bra1_pc,
                      (int16_t)(done_off  - bra1_pc));
    m68k_patch_disp16(cb_data(cb), cb_len(cb), (uint32_t)bra2_pc,
                      (int16_t)(done_off  - bra2_pc));
}

static int patches_push(BranchPatch **patches, uint32_t *n, uint32_t *cap,
                        uint32_t patch_off, uint32_t target_bc_off)
{
    if (*n == *cap) {
        uint32_t new_cap = (*cap == 0) ? WALKER_PATCH_INITIAL_CAP : (*cap * 2);
        BranchPatch *grown = (BranchPatch *)platform_alloc(
            (unsigned long)(new_cap * sizeof(BranchPatch)));
        if (grown == NULL) return 0;
        if (*patches != NULL) {
            uint32_t i;
            for (i = 0; i < *n; i++) grown[i] = (*patches)[i];
            platform_free(*patches);
        }
        *patches = grown;
        *cap = new_cap;
    }
    (*patches)[*n].patch_off     = patch_off;
    (*patches)[*n].target_bc_off = target_bc_off;
    (*n)++;
    return 1;
}

/* Emit the shared tail of the four inline-setjmp NLX frames
 * (BLOCK/CATCH/TAGBODY/UWPROT).  The alloc sequence differs per NLX type
 * (const-pool tag, popped runtime tag, or no arg) and stays hand-written
 * at each site; from the setjmp onward the emitted m68k is identical:
 *
 *   push D0 (=&buf); JSR setjmp; drop arg
 *   TST.L D0; BEQ normal_path
 *   JSR post_longjmp; [push D0 result]; BRA landing
 *   normal_path: patch BEQ here; JSR commit
 *
 * `push_result` is 1 for BLOCK/CATCH/TAGBODY (the longjmp arm leaves the
 * block/catch value or tagbody tag_index on the operand stack) and 0 for
 * UWPROT (its post_longjmp returns void).  The landing branch is patched
 * immediately for an already-emitted backward target, else queued in the
 * forward-patch list.  Returns 0 (caller does `goto fail`) on a >16-bit
 * displacement or a patch-list allocation failure. */
static int emit_nlx_setjmp_tail(CodeBuf *cb, uint32_t post_longjmp_helper,
                                uint32_t commit_helper, int push_result,
                                uint32_t landing_bc_ip, uint32_t ip,
                                const int32_t *bc_to_native,
                                BranchPatch **patches, uint32_t *n_patches,
                                uint32_t *cap_patches)
{
    int32_t beq_pc, bra_pc, normal_path_off;

    /* setjmp(D0 = &buf): push D0, JSR setjmp, drop arg. */
    m68k_emit_move_l_dn_predec_an(cb, REG_D0, REG_A7);
    m68k_emit_jsr_abs_l(cb, cl_jit_setjmp_addr);
    m68k_emit_addq_l_an(cb, 4, REG_A7);

    /* TST.L D0 — Z set on the normal (zero) setjmp return. */
    m68k_emit_tst_l_dn(cb, REG_D0);
    beq_pc = (int32_t)cb_len(cb) + 2;
    m68k_emit_beq_w(cb, 0);   /* → normal_path, patched below */

    /* NLX path inline: post_longjmp → [push result] → BRA landing. */
    m68k_emit_jsr_abs_l(cb, post_longjmp_helper);
    if (push_result)
        m68k_emit_move_l_dn_predec_an(cb, REG_D0, REG_A7);
    bra_pc = (int32_t)cb_len(cb) + 2;
    if (landing_bc_ip < ip && bc_to_native[landing_bc_ip] >= 0) {
        int32_t disp = bc_to_native[landing_bc_ip] - bra_pc;
        if (disp < -32768 || disp > 32767) return 0;
        m68k_emit_bra_w(cb, (int16_t)disp);
    } else {
        if (!patches_push(patches, n_patches, cap_patches,
                          (uint32_t)bra_pc, landing_bc_ip)) return 0;
        m68k_emit_bra_w(cb, 0);
    }

    /* normal_path: patch the BEQ to land here, then commit. */
    normal_path_off = (int32_t)cb_len(cb);
    m68k_patch_disp16(cb_data(cb), cb_len(cb), (uint32_t)beq_pc,
                      (int16_t)(normal_path_off - beq_pc));
    m68k_emit_jsr_abs_l(cb, commit_helper);
    return 1;
}

/* Resolve a signed branch offset to an absolute bytecode IP, mirroring
 * the VM's dispatch arithmetic (landing = base ± offset).  `base` is the
 * IP the offset is measured from — i.e. the IP *after* the branch
 * operand has been consumed (catch_ip in VM terms): the prescan passes
 * `ip + operand_width` since ip still points at the opcode there, the
 * emitter passes plain `ip` since it has already advanced past the
 * operand.  Sets *ok=0 (and returns 0) when the target underflows past 0
 * or overflows past code_len; callers translate that into their local
 * failure action (`return 0` in the prescan, `goto fail` in the
 * emitter). */
static uint32_t compute_landing_ip(int32_t offset, uint32_t base,
                                   uint32_t code_len, int *ok)
{
    uint32_t landing;
    if (offset < 0) {
        uint32_t neg = (uint32_t)(-offset);
        if (neg > base) { *ok = 0; return 0; }
        landing = base - neg;
    } else {
        landing = base + (uint32_t)offset;
    }
    if (landing > code_len) { *ok = 0; return 0; }
    *ok = 1;
    return landing;
}

/* is_target[] flags.  JIT_TARGET: some branch lands here.  JIT_LOOP_HEAD:
 * a backward branch (target at or before the branch) lands here, so the
 * walker emits the loop poll at this IP (emit_loop_poll). */
#define JIT_TARGET    1
#define JIT_LOOP_HEAD 2
#define JIT_BRANCH_FLAGS(target, ip) \
    ((uint8_t)((target) <= (ip) ? (JIT_TARGET | JIT_LOOP_HEAD) : JIT_TARGET))

/* Walk the bytecode once to find every IP that is the target of a
 * branch.  Sets JIT_TARGET in `is_target[ip]` for each such IP, plus
 * JIT_LOOP_HEAD for the target of a backward branch.  Used by
 * walker_compile to know where to flush the cache so the cache state
 * at every branch boundary is depth=0 — see the comment above the
 * cache primitives — and where loops poll.
 *
 * Also doubles as a structural validator: returns 0 if it sees an
 * opcode the walker doesn't know how to handle or a malformed
 * operand (e.g. a branch target outside the code).  In that case the
 * caller bails before emitting anything.
 *
 * *DYN_EXTENT is set when the function establishes something with
 * dynamic extent -- an NLX frame (whose saved registers point into its
 * activation), a handler, a restart or a special binding: its tail calls
 * must stay calls (emit_tail_call_site). */
static int prescan_branch_targets(const CL_Bytecode *bc, uint8_t *is_target,
                                  int *dyn_extent)
{
    uint32_t ip = 0;
    *dyn_extent = 0;
    while (ip < bc->code_len) {
        uint8_t op = bc->code[ip];
        uint32_t step;
        switch (op) {
        case OP_CATCH: case OP_UWPROT: case OP_BLOCK_PUSH:
        case OP_TAGBODY_PUSH: case OP_HANDLER_PUSH: case OP_RESTART_PUSH:
        case OP_HANDLER_CASE_PUSH: case OP_DYNBIND: case OP_PROGV_BIND:
            *dyn_extent = 1;
            break;
        default:
            break;
        }
        switch (op) {
        case OP_NIL: case OP_T: case OP_POP: case OP_DUP: case OP_RET:
        case OP_CAR: case OP_CDR: case OP_CONS: case OP_NOT: case OP_EQ:
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV:
        case OP_LT: case OP_GT: case OP_LE: case OP_GE: case OP_NUMEQ:
        case OP_MV_RESET: case OP_BLOCK_POP:
        case OP_UWPOP: case OP_UWRETHROW: case OP_MV_TO_LIST:
        case OP_MV_SAVE: case OP_MV_RESTORE:
        case OP_MAKE_CELL: case OP_CELL_REF:
        case OP_RPLACA: case OP_RPLACD: case OP_ASET:
        case OP_ARGC: case OP_NTH_VALUE:
        case OP_TAGBODY_POP:
        case OP_UNCATCH:
        case OP_APPLY:
        case OP_PROGV_BIND: case OP_PROGV_UNBIND:
            step = 1; break;
        case OP_CHAREQ:
            step = 1; break;
        case OP_AREF: case OP_PUSH_LOCAL: case OP_POP_LOCAL:
            step = 2; break;
        case OP_LOAD: case OP_STORE: case OP_CALL: case OP_TAILCALL:
        case OP_STRUCT_REF: case OP_STRUCT_SET: case OP_DYNUNBIND:
        case OP_CELL_SET_LOCAL:
        case OP_UPVAL: case OP_CELL_SET_UPVAL:
        case OP_LIST:
        case OP_MV_LOAD:
        case OP_HANDLER_POP: case OP_RESTART_POP:
            step = 2; break;
        case OP_CONST: case OP_GLOAD: case OP_GSTORE:
        case OP_FLOAD: case OP_DYNBIND: case OP_BLOCK_RETURN:
        case OP_FSTORE:
        case OP_ASSERT_TYPE:
        case OP_HANDLER_PUSH: case OP_RESTART_PUSH:
        case OP_TAGBODY_GO:
            step = 3; break;
        case OP_CALL_GLOBAL: case OP_TAILCALL_GLOBAL:
            step = 4; break;   /* u16 sym_idx + u8 nargs */
        /* Superinstructions (opcodes.h): the members' operands in order. */
        case OP_STORE_POP: case OP_LOAD_MV_RESET: case OP_LOAD_RET:
        case OP_POP_LOAD:
            step = 2; break;
        case OP_LOAD_LOAD: case OP_LOAD_STRUCT_REF: case OP_LOAD_STORE_POP:
            step = 3; break;
        case OP_LOAD_CONST:
            step = 4; break;   /* u8 slot + u16 const_idx */
        case OP_LOAD_CALL_GLOBAL:
            step = 5; break;   /* u8 slot + u16 sym_idx + u8 nargs */
        case OP_GLOAD_CALL_GLOBAL:
            step = 6; break;   /* u16 sym_idx + u16 sym_idx + u8 nargs */
        case OP_EQ_JNIL: case OP_LOAD_JNIL: case OP_GLOAD_JNIL:
        case OP_GLOAD_EQ_JNIL: case OP_CMP_BR: {
            /* The i32 follows the opcode (EQ_JNIL), a u8 (LOAD_JNIL,
             * CMP_BR) or a u16 (GLOAD_JNIL, GLOAD_EQ_JNIL); it is relative
             * to the instruction's end. */
            uint32_t pre = (op == OP_EQ_JNIL) ? 0
                         : (op == OP_LOAD_JNIL || op == OP_CMP_BR) ? 1 : 2;
            int32_t offset;
            uint32_t target;
            int ok;
            step = 1 + pre + 4;
            if (ip + step > bc->code_len) return 0;
            offset = read_i32_be(bc->code + ip + 1 + pre);
            target = compute_landing_ip(offset, ip + step, bc->code_len, &ok);
            if (!ok) return 0;
            is_target[target] |= JIT_BRANCH_FLAGS(target, ip);
            break;
        }
        case OP_JMP: case OP_JNIL: case OP_JTRUE: {
            int32_t offset;
            uint32_t target;
            int ok;
            step = 5;
            if (ip + 5 > bc->code_len) return 0;
            offset = read_i32_be(bc->code + ip + 1);
            /* catch_ip in the VM = ip after the i32 = ip + 5. */
            target = compute_landing_ip(offset, ip + 5, bc->code_len, &ok);
            if (!ok) return 0;
            is_target[target] |= JIT_BRANCH_FLAGS(target, ip);
            break;
        }
        case OP_BLOCK_PUSH: {
            /* 1 + u16 tag_idx + i32 offset = 7 bytes.  The landing IP
             * (catch_ip + offset) is the post-BLOCK_POP continuation;
             * the NLX shim emitted at BLOCK_PUSH branches there on a
             * non-zero setjmp return, so it's a real branch target. */
            int32_t offset;
            uint32_t landing_ip;
            int ok;
            step = 7;
            if (ip + 7 > bc->code_len) return 0;
            offset = read_i32_be(bc->code + ip + 3);
            /* catch_ip in the VM = ip after reading u16 + i32 = ip + 7. */
            landing_ip = compute_landing_ip(offset, ip + 7, bc->code_len, &ok);
            if (!ok) return 0;
            is_target[landing_ip] |= JIT_TARGET;
            break;
        }
        case OP_TAGBODY_PUSH: {
            /* 1 + u16 id_idx + i32 offset = 7 bytes.  The landing IP
             * (catch_ip + offset, with catch_ip = ip+7 mirroring VM
             * semantics) is the dispatch shim emitted by
             * compile_tagbody right after the PUSH — the longjmp arm
             * BRA.W's there with tag_index on TOS so JTRUE per-tag
             * shims can route to the matching body.  Same shape as
             * OP_BLOCK_PUSH's prescan arm. */
            int32_t offset;
            uint32_t landing_ip;
            int ok;
            step = 7;
            if (ip + 7 > bc->code_len) return 0;
            offset = read_i32_be(bc->code + ip + 3);
            landing_ip = compute_landing_ip(offset, ip + 7, bc->code_len, &ok);
            if (!ok) return 0;
            is_target[landing_ip] |= JIT_TARGET;
            break;
        }
        case OP_CATCH: {
            /* 1 + i32 offset = 5 bytes.  The landing IP (catch_ip +
             * offset, with catch_ip = ip+5 mirroring VM semantics) is
             * the post-throw continuation where the NLX shim pushes
             * the thrown result and branches; mark it as a real
             * branch target so the cache flushes on arrival. */
            int32_t offset;
            uint32_t landing_ip;
            int ok;
            step = 5;
            if (ip + 5 > bc->code_len) return 0;
            offset = read_i32_be(bc->code + ip + 1);
            landing_ip = compute_landing_ip(offset, ip + 5, bc->code_len, &ok);
            if (!ok) return 0;
            is_target[landing_ip] |= JIT_TARGET;
            break;
        }
        case OP_UWPROT: {
            /* 1 + i32 offset = 5 bytes.  The landing IP (catch_ip +
             * offset, with catch_ip = ip+5 mirroring VM semantics) is
             * where the longjmp arm jumps to (= cleanup_start, both
             * normal and longjmp paths merge there in compile_unwind_
             * protect's emit). */
            int32_t offset;
            uint32_t landing_ip;
            int ok;
            step = 5;
            if (ip + 5 > bc->code_len) return 0;
            offset = read_i32_be(bc->code + ip + 1);
            landing_ip = compute_landing_ip(offset, ip + 5, bc->code_len, &ok);
            if (!ok) return 0;
            is_target[landing_ip] |= JIT_TARGET;
            break;
        }
        case OP_HANDLER_CASE_PUSH: {
            /* 1 + u16 types_idx + i32 offset = 7 bytes.  The landing
             * (catch_ip + offset, catch_ip = ip+7) is a table of one
             * 5-byte OP_JMP per clause; the longjmp arm the emitter
             * builds branches to entry k for clause k, so every entry is
             * a branch target.  The clause count is the length of the
             * TYPE list constant.  A clauseless form (n == 0) is left to
             * the interpreter: nothing can ever land in it. */
            int32_t offset;
            uint32_t landing_ip, k, n;
            uint16_t types_idx;
            int ok;
            CL_Obj types;
            step = 7;
            if (ip + 7 > bc->code_len) return 0;
            types_idx = ((uint16_t)bc->code[ip + 1] << 8) | bc->code[ip + 2];
            if (types_idx >= bc->n_constants || bc->constants == NULL) return 0;
            offset = read_i32_be(bc->code + ip + 3);
            landing_ip = compute_landing_ip(offset, ip + 7, bc->code_len, &ok);
            if (!ok) return 0;
            types = bc->constants[types_idx];
            n = 0;
            while (CL_CONS_P(types)) { n++; types = cl_cdr(types); }
            if (n == 0 || n > 127 || landing_ip + 5 * n > bc->code_len) return 0;
            for (k = 0; k < n; k++)
                is_target[landing_ip + 5 * k] |= JIT_TARGET;
            break;
        }
        case OP_HANDLER_CASE_POP:
            step = 1;
            break;
        case OP_AMIGA_CALL:
            /* 1 + u16 sym_idx + i16 offset + i32 regspec + u8 n_args
             * = 10 bytes.  Straight-line opcode, no branch targets. */
            step = 10;
            break;
        case OP_CLOSURE: {
            /* 1 + u16 const_idx + 2 bytes per captured upvalue.  We must
             * look up the template bytecode in the constants pool to know
             * n_upvalues; if anything looks wrong (out-of-range index,
             * non-bytecode constant, slot out of frame range), bail the
             * whole walker.  is_local descriptors are validated against
             * the local frame; is_local=0 (= capture from the parent
             * closure's upvalues) bounds-checks against bc->n_upvalues
             * — the JIT entry receives that closure as func_obj at
             * 8(a6) so the walker can emit a helper-routed load. */
            uint16_t idx;
            CL_Obj tmpl_obj;
            CL_Bytecode *tmpl;
            uint32_t n_upvals_local;
            uint32_t i;
            if (ip + 3 > bc->code_len) return 0;
            idx = ((uint16_t)bc->code[ip + 1] << 8) | bc->code[ip + 2];
            if (idx >= bc->n_constants || bc->constants == NULL) return 0;
            tmpl_obj = bc->constants[idx];
            if (!CL_BYTECODE_P(tmpl_obj)) return 0;
            tmpl = (CL_Bytecode *)CL_OBJ_TO_PTR(tmpl_obj);
            n_upvals_local = tmpl->n_upvalues;
            step = 3 + 2 * n_upvals_local;
            if (ip + step > bc->code_len) return 0;
            for (i = 0; i < n_upvals_local; i++) {
                uint8_t is_local = bc->code[ip + 3 + 2*i];
                uint8_t cap_idx  = bc->code[ip + 3 + 2*i + 1];
                if (is_local) {
                    if (cap_idx >= bc->n_locals) return 0;
                } else {
                    if (cap_idx >= bc->n_upvalues) return 0;
                }
            }
            break;
        }
        default:
            return 0;
        }
        /* Strict bound: the final opcode's operands must end exactly at
         * code_len ("+ 1" let them extend one byte past it, weakening the
         * validator the OP_CLOSURE arm relies on). */
        if (ip + step > bc->code_len) return 0;
        ip += step;
    }
    return 1;
}

/* Emit a Bcc.W to bytecode offset TARGET_BC_OFF.  CC selects the branch:
 * BCC_ALWAYS (BRA), BCC_EQ (BEQ: the Z flag the caller just set means
 * "NIL"), BCC_NE.  INSN_START is the branching instruction's own bytecode
 * offset: a target at or before it is a backward branch whose native
 * offset bc_to_native already holds; anything later is a forward branch
 * recorded for patching once the walk is done.  Returns 0 when the
 * displacement does not fit in 16 bits or the backward target was never
 * emitted (a jump into the middle of an instruction). */
/* `cc` is the m68k condition code itself (asm_m68k.h: 0=BRA, 6=BNE,
 * 7=BEQ, 12=BGE, 13=BLT, 14=BGT, 15=BLE). */
#define BCC_ALWAYS 0
#define BCC_NE     6
#define BCC_EQ     7
#define BCC_GE     12
#define BCC_LT     13
#define BCC_GT     14
#define BCC_LE     15
static int emit_bcc_to_bc(CodeBuf *cb, int cc, uint32_t target_bc_off,
                          uint32_t insn_start, const int32_t *bc_to_native,
                          BranchPatch **patches, uint32_t *n_patches,
                          uint32_t *cap_patches)
{
    uint32_t patch_off = cb_len(cb) + 2;   /* disp field of Bcc.W */
    int16_t disp = 0;
    if (target_bc_off <= insn_start) {
        int32_t target_native = bc_to_native[target_bc_off];
        int32_t disp32;
        if (target_native < 0) return 0;
        disp32 = target_native - (int32_t)patch_off;
        if (disp32 < -32768 || disp32 > 32767) return 0;
        disp = (int16_t)disp32;
    } else {
        if (!patches_push(patches, n_patches, cap_patches,
                          patch_off, target_bc_off)) return 0;
    }
    m68k_emit_bcc_w(cb, (uint8_t)cc, disp);
    return 1;
}

/* The m68k condition that branches when comparison `cmp` (CL_CMP_BR_*)
 * holds after `cmp.l d1,d0` (flags = a - b), or when it does not. */
static int cmp_br_condition(int cmp, int when_true)
{
    switch (cmp) {
    case CL_CMP_BR_LT: return when_true ? BCC_LT : BCC_GE;
    case CL_CMP_BR_GT: return when_true ? BCC_GT : BCC_LE;
    case CL_CMP_BR_LE: return when_true ? BCC_LE : BCC_GT;
    case CL_CMP_BR_GE: return when_true ? BCC_GE : BCC_LT;
    default:           return when_true ? BCC_EQ : BCC_NE;   /* NUMEQ, CHAREQ */
    }
}

/* OP_CHAREQ's compute template.  Operands in D0=a, D1=b; leaves CL_T or
 * CL_NIL in D0.  Same boundary-free contract as emit_compare_compute,
 * with the character tag test (CMPI.B against CL_TAG_CHAR on the low
 * byte) in place of the fixnum BTST; two equal characters are the same
 * tagged word, so CMP.L decides.  The slow-path helper signals CHAR='s
 * type error (it never returns for a non-character), so the shim keeps
 * the spill/reload only for the conservative scan of a signalled
 * condition's allocation. */
static void emit_chareq_compute(CodeBuf *cb, int cache_head, int cache_depth,
                                JitRelocs *relocs)
{
    uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_chareq;
    int32_t bne_a_pc, bne_b_pc, beq_pc, bra1_pc, bra2_pc;
    int32_t slow_off, done_off, load_t_off;

    m68k_emit_cmpi_b_imm_dn(cb, CL_TAG_CHAR, REG_D0);
    bne_a_pc = (int32_t)cb_len(cb) + 2;
    m68k_emit_bne_w(cb, 0);
    m68k_emit_cmpi_b_imm_dn(cb, CL_TAG_CHAR, REG_D1);
    bne_b_pc = (int32_t)cb_len(cb) + 2;
    m68k_emit_bne_w(cb, 0);

    m68k_emit_cmp_l_dn_dm(cb, REG_D1, REG_D0);
    beq_pc = (int32_t)cb_len(cb) + 2;
    m68k_emit_beq_w(cb, 0);
    m68k_emit_moveq(cb, 0, REG_D0);                  /* CL_NIL */
    bra1_pc = (int32_t)cb_len(cb) + 2;
    m68k_emit_bra_w(cb, 0);

    load_t_off = (int32_t)cb_len(cb);
    emit_obj_imm_d0(cb, relocs, CL_T);
    bra2_pc = (int32_t)cb_len(cb) + 2;
    m68k_emit_bra_w(cb, 0);

    slow_off = (int32_t)cb_len(cb);
    emit_cache_spill(cb, cache_head, cache_depth);
    m68k_emit_move_l_dn_predec_an(cb, REG_D1, REG_A7);
    m68k_emit_move_l_dn_predec_an(cb, REG_D0, REG_A7);
    m68k_emit_jsr_abs_l(cb, helper);
    m68k_emit_addq_l_an(cb, 8, REG_A7);
    emit_cache_reload(cb, cache_head, cache_depth);
    done_off = (int32_t)cb_len(cb);

    m68k_patch_disp16(cb_data(cb), cb_len(cb), (uint32_t)bne_a_pc,
                      (int16_t)(slow_off - bne_a_pc));
    m68k_patch_disp16(cb_data(cb), cb_len(cb), (uint32_t)bne_b_pc,
                      (int16_t)(slow_off - bne_b_pc));
    m68k_patch_disp16(cb_data(cb), cb_len(cb), (uint32_t)beq_pc,
                      (int16_t)(load_t_off - beq_pc));
    m68k_patch_disp16(cb_data(cb), cb_len(cb), (uint32_t)bra1_pc,
                      (int16_t)(done_off - bra1_pc));
    m68k_patch_disp16(cb_data(cb), cb_len(cb), (uint32_t)bra2_pc,
                      (int16_t)(done_off - bra2_pc));
}

/* --- Direct-call sites (specs/jit-direct-calls.md §2, §3) ----------------
 *
 * Every call the walker emits goes through a call site: an inline guard
 * that JSRs straight into the callee's native code when the site's cell
 * (CL_JitCallSite, runtime.h) is valid, else the _site miss helper, which
 * dispatches as before and fills the cell.  The cells are appended after
 * the function's code, 4-byte aligned; each site reaches its cell with
 * LEA (d16,PC),A1, whose displacement is patched once the walk is done.
 * The cells are data, read by native code and written only by C, and not
 * in native_relocs: the gen check makes relocating `func` unnecessary. */
/* Every LEA (d16,PC) that addresses a cell, and the cell it addresses: a
 * tail call's jump path and its fallback call share one cell (the miss
 * helper fills the cell it is handed, and both paths must see the fill). */
typedef struct {
    uint32_t lea_off;     /* offset of the LEA's disp word */
    uint32_t cell;        /* index of the cell it addresses */
} JitSiteRef;

typedef struct {
    JitSiteRef *refs;
    uint32_t    count;
    uint32_t    cap;
    uint32_t    n_cells;
    int         oom;
} JitSites;

/* Record the LEA whose disp word is at OFF.  CELL < 0 gives it a new cell;
 * otherwise it shares that one.  Returns the cell's index. */
static int32_t jit_sites_record(JitSites *s, uint32_t off, int32_t cell)
{
    if (cell < 0) cell = (int32_t)s->n_cells++;
    if (s->oom) return cell;
    if (s->count == s->cap) {
        uint32_t new_cap = (s->cap == 0) ? 8 : (s->cap * 2);
        JitSiteRef *grown = (JitSiteRef *)platform_alloc(
            (unsigned long)new_cap * sizeof(JitSiteRef));
        if (grown == NULL) { s->oom = 1; return cell; }
        if (s->refs) {
            uint32_t i;
            for (i = 0; i < s->count; i++) grown[i] = s->refs[i];
            platform_free(s->refs);
        }
        s->refs = grown;
        s->cap = new_cap;
    }
    s->refs[s->count].lea_off = off;
    s->refs[s->count].cell    = (uint32_t)cell;
    s->count++;
    return cell;
}

/* The hit path compares A7 against CL_Thread.jit_c_floor through A3. */
#define JIT_C_FLOOR_DISP ((int16_t)offsetof(CL_Thread, jit_c_floor))
typedef char jit_c_floor_disp_fits_d16[
    (offsetof(CL_Thread, jit_c_floor) < 32768) ? 1 : -1];

/* OP_ARGC writes CL_Thread.mv_count through A3. */
#define JIT_MV_COUNT_DISP ((int16_t)offsetof(CL_Thread, mv_count))
typedef char jit_mv_count_disp_fits_d16[
    (offsetof(CL_Thread, mv_count) < 32768) ? 1 : -1];
typedef char jit_mv_count_is_32bit[(sizeof(int) == 4) ? 1 : -1];
typedef char jit_fixnum_tag_is_1[(CL_TAG_FIXNUM == 1) ? 1 : -1];

/* The loop poll counts down CL_Thread.jit_loop_ctr through A3. */
#define JIT_LOOP_CTR_DISP ((int16_t)offsetof(CL_Thread, jit_loop_ctr))
typedef char jit_loop_ctr_disp_fits_d16[
    (offsetof(CL_Thread, jit_loop_ctr) < 32768) ? 1 : -1];

/* Emit the loop poll (runtime.h, cl_jit_runtime_loop_poll) at a loop
 * header: the target of a backward branch, or right before a self tail
 * call branches back to the entry.  The interpreter runs CL_SAFEPOINT and
 * the Ctrl-C poll on every backward jump; without this, a native loop that
 * calls nothing never stops for a peer's collection, an MP:INTERRUPT-THREAD
 * or Ctrl-C.  The cache must be empty (depth 0, which every branch target
 * is) and no flags are live.  14 bytes:
 *
 *     subq.w  #1,jit_loop_ctr(a3)
 *     bcc.w   .skip
 *     jsr     cl_jit_runtime_loop_poll
 *   .skip: */
static void emit_loop_poll(CodeBuf *cb)
{
    m68k_emit_subq_w_disp_an(cb, 1, JIT_LOOP_CTR_DISP, REG_A3);
    m68k_emit_bcc_w(cb, 4 /* BCC: no borrow */, 2 + 6);
    m68k_emit_jsr_abs_l(cb, (uint32_t)(uintptr_t)&cl_jit_runtime_loop_poll);
}

/* The hit path pushes the callee's CL_Frame through A3 (cl_jit_invoke's
 * frame, as the interpreter's OP_CALL would push one): EXT:BACKTRACE and
 * the error-time backtrace list native functions.  &frames[fp] is
 * frames + fp*34, computed as fp*32 + fp*2 -- a MULU.W costs a 68040 four
 * times as much.
 *
 * JIT_FRAME_SP: a native function's frame keeps in its `constants` word
 * the A7 it was entered with -- the address of its func slot, its A6 + 8
 * once LINKed, the return address into its caller just below.  The hit
 * path stores it, and cl_jit_enter for a frame cl_jit_invoke pushes.  An
 * interpreted frame holds a constants pool there, never a stack address,
 * so the word both marks a native activation and locates it: the tail
 * jump checks it to know that the frame on top is its own
 * (emit_tail_call_site). */
#define JIT_VM_FP_DISP      ((int16_t)offsetof(CL_Thread, vm.fp))
#define JIT_VM_FSIZE_DISP   ((int16_t)offsetof(CL_Thread, vm.frame_size))
#define JIT_VM_FRAMES_DISP  ((int16_t)offsetof(CL_Thread, vm.frames))
#define JIT_FR_BYTECODE     ((int16_t)offsetof(CL_Frame, bytecode))
#define JIT_FR_CODE         ((int16_t)offsetof(CL_Frame, code))
#define JIT_FR_CONSTANTS    ((int16_t)offsetof(CL_Frame, constants))
#define JIT_FR_IP           ((int16_t)offsetof(CL_Frame, ip))
#define JIT_FR_NLOCALS      ((int16_t)offsetof(CL_Frame, n_locals))
typedef char jit_vm_disps_fit_d16[
    (offsetof(CL_Thread, vm.fp) < 32768 &&
     offsetof(CL_Thread, vm.frame_size) < 32768 &&
     offsetof(CL_Thread, vm.frames) < 32768) ? 1 : -1];
typedef char jit_frame_is_34_bytes[(sizeof(CL_Frame) == 34) ? 1 : -1];
typedef char jit_frame_fields_are_32bit[
    (sizeof(((CL_Frame *)0)->bytecode) == 4 && sizeof(((CL_Frame *)0)->code) == 4 &&
     sizeof(((CL_Frame *)0)->constants) == 4 && sizeof(((CL_Frame *)0)->ip) == 4 &&
     sizeof(((CL_Frame *)0)->n_locals) == 4 &&
     sizeof(((CL_Thread *)0)->vm.fp) == 4 &&
     sizeof(((CL_Thread *)0)->vm.frame_size) == 4) ? 1 : -1];

/* Emit one call through a direct-call site.  The cache is flushed and the
 * NARGS arguments are on the operand stack ((a7) = the last); for OP_CALL /
 * OP_TAILCALL (GLOBAL == 0) the function value sits under them, for the
 * _GLOBAL forms SYM names the callee.  Leaves the result in D0 and the
 * arguments (and function slot) dropped:
 *
 *         lea     cell(pc),a1
 *         move.l  (a1)+,d0            ; gen     -- gen, func and entry are
 *         move.l  (a1)+,d1            ; func       read before gen is
 *         movea.l (a1),a0             ; entry      compared, runtime_m68k.c
 *         cmp.l   cl_call_gen,d0
 *         bne.w   .miss
 *       [ cmp.l   4n(a7),d1           ; OP_CALL: the same function value?
 *         bne.w   .miss ]
 *         cmpa.l  jit_c_floor(a3),a7  ; C stack below the floor: the helper
 *         bls.w   .miss               ; path signals "C stack exhausted"
 *         move.l  vm.fp(a3),d0
 *         cmp.l   vm.frame_size(a3),d0
 *         bge.w   .miss               ; no free frame: "Call stack overflow"
 *         move.l  d1,-(a7)            ; func -> the callee's 8(a6)
 *         move.l  d0,d1
 *         lsl.l   #5,d0
 *         add.l   d1,d1
 *         add.l   d1,d0               ; fp * sizeof(CL_Frame)
 *         move.l  4(a1),d1            ; the callee's bc->code (cell word 3)
 *         movea.l vm.frames(a3),a1
 *         adda.l  d0,a1               ; &frames[fp]
 *         move.l  (a7),bytecode(a1)
 *         move.l  d1,code(a1)
 *         move.l  a7,constants(a1)    ; the activation (JIT_FRAME_SP)
 *         clr.l   ip(a1)              ; the function's first line
 *         clr.l   n_locals(a1)        ; its arguments are not on the VM stack
 *         addq.l  #1,vm.fp(a3)
 *         moveq   #nargs,d1           ; the count, for an &optional callee
 *         jsr     (a0)
 *         subq.l  #1,vm.fp(a3)
 *         lea     4+drop(a7),a7
 *         bra.w   .done
 * .miss:  move.l  a7,a0               ; operand_top
 *         pea     -8(a1)              ; the cell (a1 = cell+8 here)
 *       [ move.l  #sym,-(a7) ]
 *         move.l  #nargs,-(a7)
 *         move.l  a0,-(a7)
 *         jsr     cl_jit_runtime_call[_global]_site
 *         lea     12|16+drop(a7),a7
 * .done:
 *
 * CELL is the site's cell: < 0 for a new one, else one a tail call's jump
 * path (emit_tail_call_site) already addresses. */
static void emit_call_site(CodeBuf *cb, JitRelocs *relocs, JitSites *sites,
                           CL_Obj sym, int global, uint8_t nargs, int32_t cell)
{
    uint32_t helper = global
        ? (uint32_t)(uintptr_t)&cl_jit_runtime_call_global_site
        : (uint32_t)(uintptr_t)&cl_jit_runtime_call_site;
    int16_t drop = (int16_t)(4 * ((int32_t)nargs + (global ? 0 : 1)));
    int32_t miss_pc[4];
    int n_miss = 0, k;
    int32_t bra_done_pc, miss_off, done_off;

    jit_sites_record(sites, cb_len(cb) + 2, cell);
    m68k_emit_lea_pc_disp_to_an(cb, 0, REG_A1);
    m68k_emit_move_l_postinc_an_to_dn(cb, REG_A1, REG_D0);
    m68k_emit_move_l_postinc_an_to_dn(cb, REG_A1, REG_D1);
    m68k_emit_movea_l_ind_an_to_am(cb, REG_A1, REG_A0);
    m68k_emit_cmp_l_abs_dn(cb, (uint32_t)(uintptr_t)&cl_call_gen, REG_D0);
    miss_pc[n_miss++] = (int32_t)cb_len(cb) + 2;
    m68k_emit_bne_w(cb, 0);
    if (!global) {
        m68k_emit_cmp_l_disp_an_dn(cb, (int16_t)(4 * (int32_t)nargs),
                                   REG_A7, REG_D1);
        miss_pc[n_miss++] = (int32_t)cb_len(cb) + 2;
        m68k_emit_bne_w(cb, 0);
    }
    m68k_emit_cmpa_l_disp_an_am(cb, JIT_C_FLOOR_DISP, REG_A3, REG_A7);
    miss_pc[n_miss++] = (int32_t)cb_len(cb) + 2;
    m68k_emit_bls_w(cb, 0);
    m68k_emit_move_l_disp_an_to_dn(cb, JIT_VM_FP_DISP, REG_A3, REG_D0);
    m68k_emit_cmp_l_disp_an_dn(cb, JIT_VM_FSIZE_DISP, REG_A3, REG_D0);
    miss_pc[n_miss++] = (int32_t)cb_len(cb) + 2;
    m68k_emit_bge_w(cb, 0);
    m68k_emit_move_l_dn_predec_an(cb, REG_D1, REG_A7);
    m68k_emit_move_l_dn_to_dm(cb, REG_D0, REG_D1);
    m68k_emit_lsl_l_imm_dn(cb, 5, REG_D0);
    m68k_emit_add_l_dn_to_dm(cb, REG_D1, REG_D1);
    m68k_emit_add_l_dn_to_dm(cb, REG_D1, REG_D0);
    m68k_emit_move_l_disp_an_to_dn(cb, 4, REG_A1, REG_D1);
    m68k_emit_movea_l_disp_an_to_am(cb, JIT_VM_FRAMES_DISP, REG_A3, REG_A1);
    m68k_emit_adda_l_dn_an(cb, REG_D0, REG_A1);
    m68k_emit_move_l_ind_an_to_disp_am(cb, REG_A7, JIT_FR_BYTECODE, REG_A1);
    m68k_emit_move_l_dn_to_disp_am(cb, REG_D1, JIT_FR_CODE, REG_A1);
    m68k_emit_move_l_an_direct_to_disp_am(cb, REG_A7, JIT_FR_CONSTANTS, REG_A1);
    m68k_emit_clr_l_disp_an(cb, JIT_FR_IP, REG_A1);
    m68k_emit_clr_l_disp_an(cb, JIT_FR_NLOCALS, REG_A1);
    m68k_emit_addq_l_disp_an(cb, 1, JIT_VM_FP_DISP, REG_A3);
    if (nargs <= 127)
        m68k_emit_moveq(cb, (int8_t)nargs, REG_D1);
    else
        m68k_emit_move_l_imm32(cb, (uint32_t)nargs, REG_D1);
    m68k_emit_jsr_ind_an(cb, REG_A0);
    m68k_emit_subq_l_disp_an(cb, 1, JIT_VM_FP_DISP, REG_A3);
    m68k_emit_lea_disp_an_to_am(cb, (int16_t)(4 + drop), REG_A7, REG_A7);
    bra_done_pc = (int32_t)cb_len(cb) + 2;
    m68k_emit_bra_w(cb, 0);

    miss_off = (int32_t)cb_len(cb);
    m68k_emit_move_l_an_to_am(cb, REG_A7, REG_A0);
    m68k_emit_pea_disp_an(cb, -8, REG_A1);
    if (global)
        emit_obj_imm_predec(cb, relocs, sym);
    m68k_emit_move_l_imm32_predec(cb, (uint32_t)nargs, REG_A7);
    m68k_emit_move_l_an_predec_am(cb, REG_A0, REG_A7);
    m68k_emit_jsr_abs_l(cb, helper);
    m68k_emit_lea_disp_an_to_am(cb, (int16_t)((global ? 16 : 12) + drop),
                                REG_A7, REG_A7);
    done_off = (int32_t)cb_len(cb);

    for (k = 0; k < n_miss; k++)
        m68k_patch_disp16(cb_data(cb), cb_len(cb), (uint32_t)miss_pc[k],
                          (int16_t)(miss_off - miss_pc[k]));
    m68k_patch_disp16(cb_data(cb), cb_len(cb), (uint32_t)bra_done_pc,
                      (int16_t)(done_off - bra_done_pc));
}

/* A tail call to another function, OP_TAILCALL / OP_TAILCALL_GLOBAL with
 * NARGS on the operand stack (the cache flushed): the jump path, which
 * hands this function's activation to the callee instead of calling it,
 * so chains of tail calls -- a state machine, mutually recursive
 * functions -- run in constant m68k stack, as the interpreter's frame
 * reuse does.  Every way off the jump path lands where the caller emits
 * the ordinary call site (emit_call_site sharing the returned cell) and
 * the return after it, so a tail call that cannot jump stays a call.
 *
 * The jump needs this function's own CL_Frame on top of the frame stack,
 * to become the callee's: its `constants` word holds the activation's
 * entry A7, A6 + 8 (the hit path's and cl_jit_enter's marker).  A
 * function entered while frames were off has none, and calls.  Our
 * caller drops 4 + 4 * (the count it pushed) after the return, which the
 * callee's arguments can only fill when NARGS <= ARITY (the walker's
 * condition), so the arguments go to 12(a6) upwards, the slots of the
 * count the callee's own caller would push; the slots above stay as they
 * are until the caller drops them.
 *
 *         subq.w  #1,jit_loop_ctr(a3) ; the loop poll: a tail call can be a
 *         bcc.w   .skip               ; loop, through other functions
 *         jsr     cl_jit_runtime_loop_poll
 * .skip:  move.l  vm.fp(a3),d0
 *         mulu.w  #34,d0
 *         movea.l vm.frames(a3),a1
 *         adda.l  d0,a1               ; &frames[fp]; ours is at -34(a1)
 *         lea     8(a6),a0
 *         cmpa.l  -34+constants(a1),a0
 *         bne.w   .call               ; not our frame
 *         move.l  a1,-(a7)
 *         lea     cell(pc),a1
 *         move.l  (a1)+,d0            ; gen, func and entry, read before
 *         move.l  (a1)+,d1            ; gen is compared (emit_call_site)
 *         movea.l (a1),a0
 *         cmp.l   cl_call_gen,d0
 *         bne.w   .miss
 *       [ cmp.l   4+4n(a7),d1         ; OP_TAILCALL: the same function?
 *         bne.w   .miss ]
 *         move.l  4(a1),d0            ; the callee's bc->code
 *         movea.l (a7)+,a1
 *         move.l  d1,-34+bytecode(a1) ; our frame is the callee's now
 *         move.l  d0,-34+code(a1)
 *         clr.l   -34+n_locals(a1)    ; cl_jit_invoke's arguments are ours
 *         move.l  d1,8(a6)            ; func
 *         move.l  (a7)+,12(a6)        ; the arguments, the last at 12(a6)
 *         ...                         ; (more than 8: a DBF loop)
 *         move.l  saved_d7(a6),d7     ; our caller's D5-D7
 *         move.l  saved_d6(a6),d6
 *         move.l  saved_d5(a6),d5
 *         unlk    a6
 *         moveq   #nargs,d1
 *         jmp     (a0)
 * .miss:  addq.l  #4,a7
 * .call:                              ; the caller's emit_call_site
 *
 * Returns the cell for that call site. */
static int32_t emit_tail_call_site(CodeBuf *cb, JitSites *sites, int global,
                                   uint8_t nargs, int16_t saved_d7_disp,
                                   int16_t saved_d6_disp, int16_t saved_d5_disp)
{
    int32_t call_pc[2], miss_pc[2];
    int n_call = 0, n_miss = 0, k;
    int32_t cell, miss_off, call_off;
    uint32_t i;

    emit_loop_poll(cb);
    m68k_emit_move_l_disp_an_to_dn(cb, JIT_VM_FP_DISP, REG_A3, REG_D0);
    m68k_emit_mulu_w_imm_dn(cb, (uint16_t)sizeof(CL_Frame), REG_D0);
    m68k_emit_movea_l_disp_an_to_am(cb, JIT_VM_FRAMES_DISP, REG_A3, REG_A1);
    m68k_emit_adda_l_dn_an(cb, REG_D0, REG_A1);
    m68k_emit_lea_disp_an_to_am(cb, 8, REG_A6, REG_A0);
    m68k_emit_cmpa_l_disp_an_am(cb, (int16_t)(JIT_FR_CONSTANTS - 34),
                                REG_A1, REG_A0);
    call_pc[n_call++] = (int32_t)cb_len(cb) + 2;
    m68k_emit_bne_w(cb, 0);

    m68k_emit_move_l_an_predec_am(cb, REG_A1, REG_A7);
    cell = jit_sites_record(sites, cb_len(cb) + 2, -1);
    m68k_emit_lea_pc_disp_to_an(cb, 0, REG_A1);
    m68k_emit_move_l_postinc_an_to_dn(cb, REG_A1, REG_D0);
    m68k_emit_move_l_postinc_an_to_dn(cb, REG_A1, REG_D1);
    m68k_emit_movea_l_ind_an_to_am(cb, REG_A1, REG_A0);
    m68k_emit_cmp_l_abs_dn(cb, (uint32_t)(uintptr_t)&cl_call_gen, REG_D0);
    miss_pc[n_miss++] = (int32_t)cb_len(cb) + 2;
    m68k_emit_bne_w(cb, 0);
    if (!global) {
        m68k_emit_cmp_l_disp_an_dn(cb, (int16_t)(4 + 4 * (int32_t)nargs),
                                   REG_A7, REG_D1);
        miss_pc[n_miss++] = (int32_t)cb_len(cb) + 2;
        m68k_emit_bne_w(cb, 0);
    }
    m68k_emit_move_l_disp_an_to_dn(cb, 4, REG_A1, REG_D0);
    m68k_emit_movea_l_postinc_an_to_am(cb, REG_A7, REG_A1);
    m68k_emit_move_l_dn_to_disp_am(cb, REG_D1, (int16_t)(JIT_FR_BYTECODE - 34),
                                   REG_A1);
    m68k_emit_move_l_dn_to_disp_am(cb, REG_D0, (int16_t)(JIT_FR_CODE - 34),
                                   REG_A1);
    m68k_emit_clr_l_disp_an(cb, (int16_t)(JIT_FR_NLOCALS - 34), REG_A1);
    m68k_emit_move_l_dn_to_disp_am(cb, REG_D1, 8, REG_A6);
    if (nargs <= 8) {
        for (i = 0; i < nargs; i++)
            m68k_emit_move_l_postinc_an_to_disp_am(cb, REG_A7,
                (int16_t)(12 + 4 * (int32_t)i), REG_A6);
    } else {
        int32_t loop_off;
        m68k_emit_lea_disp_an_to_am(cb, 12, REG_A6, REG_A1);
        if (nargs - 1 <= 127)
            m68k_emit_moveq(cb, (int8_t)(nargs - 1), REG_D0);
        else
            m68k_emit_move_l_imm32(cb, (uint32_t)(nargs - 1), REG_D0);
        loop_off = (int32_t)cb_len(cb);
        m68k_emit_move_l_postinc_an_to_postinc_am(cb, REG_A7, REG_A1);
        m68k_emit_dbf_w(cb, REG_D0,
                        (int16_t)(loop_off - ((int32_t)cb_len(cb) + 2)));
    }
    m68k_emit_move_l_disp_an_to_dn(cb, saved_d7_disp, REG_A6, REG_D7);
    m68k_emit_move_l_disp_an_to_dn(cb, saved_d6_disp, REG_A6, REG_D6);
    m68k_emit_move_l_disp_an_to_dn(cb, saved_d5_disp, REG_A6, REG_D5);
    m68k_emit_unlk_an(cb, REG_A6);
    if (nargs <= 127)
        m68k_emit_moveq(cb, (int8_t)nargs, REG_D1);
    else
        m68k_emit_move_l_imm32(cb, (uint32_t)nargs, REG_D1);
    m68k_emit_jmp_ind_an(cb, REG_A0);

    miss_off = (int32_t)cb_len(cb);
    m68k_emit_addq_l_an(cb, 4, REG_A7);
    call_off = (int32_t)cb_len(cb);
    for (k = 0; k < n_miss; k++)
        m68k_patch_disp16(cb_data(cb), cb_len(cb), (uint32_t)miss_pc[k],
                          (int16_t)(miss_off - miss_pc[k]));
    for (k = 0; k < n_call; k++)
        m68k_patch_disp16(cb_data(cb), cb_len(cb), (uint32_t)call_pc[k],
                          (int16_t)(call_off - call_pc[k]));
    return cell;
}

/* The line table (JIT_FRAME_SP's other use): which bytecode IP a return
 * address in this function's code belongs to, for the backtrace's line of
 * a native frame (cl_jit_resolve_frame_ips).  Native code stores no IP:
 * the backtrace finds the frame's return address on the m68k stack and
 * looks it up here, so the table costs memory, not time.  An entry
 * (native offset << 16 | IP) starts at an opcode that emitted a call --
 * only those leave a return address -- and only where the source line
 * changes; IP is the opcode's own + 1, the resume point lookup_source_line
 * expects.  The table and a trailer follow the cells:
 *
 *     [code][cells][entries: u32 ...][u32 code_end][u32 table_off][u32 magic]
 *
 * A function without calls, without a line map or past 64 KB (code or
 * bytecode) gets none, and its frames keep the first line. */
#define JIT_LINES_MAGIC 0x4A4C494EUL    /* "JLIN" */

typedef struct {
    uint32_t *ents;
    uint32_t  count;
    uint32_t  cap;
    int       off;          /* too large or out of memory: no table */
    int32_t   last_line;    /* the last entry's line; -1 before the first */
    uint32_t  lm;           /* cursor into bc->line_map */
} JitLines;

/* After the opcode at bytecode OP_IP, emitted from native OP_NATIVE on. */
static void jit_lines_note(JitLines *L, const CL_Bytecode *bc,
                           const CodeBuf *cb, uint32_t op_native, uint32_t op_ip)
{
    int32_t line;
    if (L->off || bc->line_map == NULL || bc->line_map_count == 0) return;
    if (cb->last_call <= op_native) return;        /* no call in it */
    if (op_native > 0xFFFF || op_ip + 1 > 0xFFFF) { L->off = 1; return; }
    while (L->lm + 1 < bc->line_map_count &&
           bc->line_map[L->lm + 1].pc <= op_ip)
        L->lm++;
    line = (bc->line_map[L->lm].pc <= op_ip) ? bc->line_map[L->lm].line : 0;
    if (line == L->last_line) return;
    L->last_line = line;
    if (L->count == L->cap) {
        uint32_t new_cap = (L->cap == 0) ? 8 : (L->cap * 2);
        uint32_t *grown = (uint32_t *)platform_alloc(
            (unsigned long)new_cap * sizeof(uint32_t));
        if (grown == NULL) { L->off = 1; return; }
        if (L->ents) {
            uint32_t i;
            for (i = 0; i < L->count; i++) grown[i] = L->ents[i];
            platform_free(L->ents);
        }
        L->ents = grown;
        L->cap = new_cap;
    }
    L->ents[L->count++] = (op_native << 16) | (op_ip + 1);
}

/* Append the table and its trailer.  CODE_END is where the instructions
 * end (the cells start). */
static void emit_line_table(CodeBuf *cb, const JitLines *L, uint32_t code_end)
{
    uint32_t i, table_off;
    if (L->off || L->count == 0) return;
    if (cb_len(cb) & 3) m68k_emit_nop(cb);     /* never reached */
    table_off = cb_len(cb);
    for (i = 0; i < L->count; i++) cb_emit_u32(cb, L->ents[i]);
    cb_emit_u32(cb, code_end);
    cb_emit_u32(cb, table_off);
    cb_emit_u32(cb, (uint32_t)JIT_LINES_MAGIC);
}

/* Append the cells of every site the walk recorded and point each site's
 * LEA at its own.  Returns 0 if a displacement does not fit. */
static int emit_call_site_cells(CodeBuf *cb, const JitSites *sites)
{
    uint32_t i, base;
    if (sites->n_cells == 0) return 1;
    if (cb_len(cb) & 3) m68k_emit_nop(cb);     /* never reached: after an RTS */
    base = cb_len(cb);
    for (i = 0; i < sites->n_cells; i++) {
        cb_emit_u32(cb, 0);     /* gen 0: never filled */
        cb_emit_u32(cb, 0);
        cb_emit_u32(cb, 0);
        cb_emit_u32(cb, 0);
    }
    for (i = 0; i < sites->count; i++) {
        const JitSiteRef *r = &sites->refs[i];
        int32_t disp = (int32_t)(base + 16 * r->cell) - (int32_t)r->lea_off;
        if (disp > 32767) return 0;
        m68k_patch_disp16(cb_data(cb), cb_len(cb), r->lea_off, (int16_t)disp);
    }
    return 1;
}

/* OP_CALL_GLOBAL's template — u16 sym_idx, u8 nargs at *IP — the fused
 * `FLOAD sym; CALL n`: a direct-call site (emit_call_site) with the symbol
 * baked as a relocated immediate for the miss helper and no function slot
 * on the operand stack.  Shared by OP_CALL_GLOBAL and the two fused heads
 * (OP_LOAD_CALL_GLOBAL, OP_GLOAD_CALL_GLOBAL) that push one more argument
 * first.  Returns 0 on a malformed operand. */
static int emit_call_global(const CL_Bytecode *bc, uint32_t *ip, CodeBuf *cb,
                            JitRelocs *relocs, JitSites *sites,
                            int *cache_head, int *cache_depth)
{
    uint16_t sym_idx;
    uint8_t nargs;
    CL_Obj sym;
    if (*ip + 2 >= bc->code_len) return 0;
    sym_idx = ((uint16_t)bc->code[*ip] << 8) | bc->code[*ip + 1];
    *ip += 2;
    nargs = bc->code[(*ip)++];
    if (sym_idx >= bc->n_constants || bc->constants == NULL) return 0;
    sym = bc->constants[sym_idx];
    if (!CL_SYMBOL_P(sym)) return 0;

    cache_flush(cb, cache_head, cache_depth);
    emit_call_site(cb, relocs, sites, sym, 1, nargs, -1);
    cache_push_dn(cb, cache_head, cache_depth, REG_D0);
    return 1;
}

/* The prologue of an &optional function (no &key, no &rest).  The caller
 * pushed NARGS arguments in operand-stack order -- argument i at
 * 12+4*(nargs-1-i)(a6) -- and passed NARGS in D1.  Copy them into slots
 * 0..nargs-1 of the LINK frame (frame_size(a6) upwards), NIL the slots
 * from nargs to n_locals-1 (a missing optional, the supplied-p variables
 * the compiler's prologue sets only when the argument was passed, and
 * the other locals, as the interpreter's frame setup does), and keep
 * NARGS at -4(a6) for OP_ARGC.  The dispatch and the call-site fill only
 * enter with arity <= nargs <= arity + n_optional (<= n_locals), so both
 * counts are >= 0.  D0/D1/A0/A1 are scratch here, as at any entry.
 *
 *         move.l  d1,-4(a6)
 *         move.l  d1,d0
 *         add.l   d0,d0
 *         add.l   d0,d0
 *         lea     12(a6),a0
 *         adda.l  d0,a0             ; one past argument 0
 *         lea     frame_size(a6),a1 ; slot 0
 *         move.l  d1,d0
 *         bra.w   .copy_next
 * .copy:  move.l  -(a0),(a1)+
 * .copy_next:
 *         dbf     d0,.copy
 *         move.l  #n_locals,d0
 *         sub.l   d1,d0
 *         bra.w   .nil_next
 * .nil:   clr.l   (a1)+
 * .nil_next:
 *         dbf     d0,.nil */
static void emit_opt_prologue(CodeBuf *cb, int16_t frame_size,
                              uint16_t n_locals)
{
    int32_t bra_pc, loop_off;

    m68k_emit_move_l_dn_to_disp_am(cb, REG_D1, -4, REG_A6);
    m68k_emit_move_l_dn_to_dm(cb, REG_D1, REG_D0);
    m68k_emit_add_l_dn_to_dm(cb, REG_D0, REG_D0);
    m68k_emit_add_l_dn_to_dm(cb, REG_D0, REG_D0);
    m68k_emit_lea_disp_an_to_am(cb, 12, REG_A6, REG_A0);
    m68k_emit_adda_l_dn_an(cb, REG_D0, REG_A0);
    m68k_emit_lea_disp_an_to_am(cb, frame_size, REG_A6, REG_A1);

    m68k_emit_move_l_dn_to_dm(cb, REG_D1, REG_D0);
    bra_pc = (int32_t)cb_len(cb) + 2;
    m68k_emit_bra_w(cb, 0);
    loop_off = (int32_t)cb_len(cb);
    m68k_emit_move_l_predec_an_to_postinc_am(cb, REG_A0, REG_A1);
    m68k_patch_disp16(cb_data(cb), cb_len(cb), (uint32_t)bra_pc,
                      (int16_t)((int32_t)cb_len(cb) - bra_pc));
    m68k_emit_dbf_w(cb, REG_D0,
                    (int16_t)(loop_off - ((int32_t)cb_len(cb) + 2)));

    m68k_emit_move_l_imm32(cb, (uint32_t)n_locals, REG_D0);
    m68k_emit_sub_l_dn_to_dm(cb, REG_D1, REG_D0);
    bra_pc = (int32_t)cb_len(cb) + 2;
    m68k_emit_bra_w(cb, 0);
    loop_off = (int32_t)cb_len(cb);
    m68k_emit_clr_l_postinc(cb, REG_A1);
    m68k_patch_disp16(cb_data(cb), cb_len(cb), (uint32_t)bra_pc,
                      (int16_t)((int32_t)cb_len(cb) - bra_pc));
    m68k_emit_dbf_w(cb, REG_D0,
                    (int16_t)(loop_off - ((int32_t)cb_len(cb) + 2)));
}

/* A self tail call of an &optional function: the NARGS new arguments are
 * on the operand stack ((a7) = the last).  Copy them into slots
 * 0..nargs-1, NIL slots nargs..n_locals-1 (as the prologue does, so a
 * supplied-p variable set by the previous round reads NIL again) and
 * store the new count at -4(a6).  The source lies below the frame, so the
 * order of the moves does not matter. */
static void emit_opt_self_tco_copy(CodeBuf *cb, uint8_t nargs,
                                   uint16_t slot_anchor, uint16_t n_locals)
{
    uint32_t i;
    for (i = 0; i < nargs; i++) {
        m68k_emit_move_l_disp_an_to_dn(cb, (int16_t)(4 * (int32_t)i),
                                       REG_A7, REG_D0);
        m68k_emit_move_l_dn_to_disp_am(cb, REG_D0,
            slot_disp((uint8_t)(nargs - 1 - i), slot_anchor, 1), REG_A6);
    }
    if (n_locals > nargs) {
        int32_t loop_off;
        m68k_emit_lea_disp_an_to_am(cb, slot_disp(nargs, slot_anchor, 1),
                                    REG_A6, REG_A1);
        m68k_emit_move_l_imm32(cb, (uint32_t)(n_locals - nargs - 1), REG_D0);
        loop_off = (int32_t)cb_len(cb);
        m68k_emit_clr_l_postinc(cb, REG_A1);
        m68k_emit_dbf_w(cb, REG_D0,
                        (int16_t)(loop_off - ((int32_t)cb_len(cb) + 2)));
    }
    if (nargs <= 127)
        m68k_emit_moveq(cb, (int8_t)nargs, REG_D0);
    else
        m68k_emit_move_l_imm32(cb, (uint32_t)nargs, REG_D0);
    m68k_emit_move_l_dn_to_disp_am(cb, REG_D0, -4, REG_A6);
}

static int walker_compile(const CL_Bytecode *bc, CodeBuf *cb, JitRelocs *relocs)
{
    uint16_t arity;
    uint16_t n_locals;
    uint32_t n_extra;
    uint32_t ip;
    uint32_t i;
    int saw_ret = 0;
    int16_t frame_size;
    int16_t saved_d7_disp, saved_d6_disp, saved_d5_disp;
    int32_t *bc_to_native = NULL;
    uint8_t *is_target = NULL;
    BranchPatch *patches = NULL;
    uint32_t n_patches = 0;
    uint32_t cap_patches = 0;
    JitSites sites = { NULL, 0, 0, 0, 0 };
    JitLines lines = { NULL, 0, 0, 0, -1, 0 };
    uint32_t op_ip = 0, op_native = 0, code_end;
    int cache_head = 7;
    int cache_depth = 0;
    int result = 0;
    int is_kw;
    int is_opt;
    int is_rest;
    int in_frame;
    uint16_t slot_anchor;
    int dyn_extent;
    int tail_jumps;     /* emit_tail_call_site applies to this function */

    /* Conservative gate.  The walker handles three shapes:
     *
     *   Required parameters only: the positional ABI.  The arguments
     *   stay where the caller pushed them, above A6; body locals live
     *   below A6 in the LINK frame.
     *
     *   &optional (no &key): the positional ABI too, plus the argument
     *   count in D1 (every entry sets it: cl_jit_enter, a call site's
     *   hit path).  Where an argument sits above A6 depends on the
     *   count, so emit_opt_prologue copies the arguments into the LINK
     *   frame, NILs the slots after them and keeps the count at -4(a6)
     *   for OP_ARGC: every slot lives below A6, as with &key.
     *
     *   &rest (no &key): the &optional frame, but the prologue is a
     *   JSR to cl_jit_runtime_rest_prologue, which conses the list of
     *   the arguments past the positional ones.  Any count above the
     *   positional ones arrives (cl_jit_invoke lifts its cap for &rest).
     *
     *   &key (with or without &optional / &rest): the same frame, the
     *   prologue a JSR to cl_jit_runtime_kw_prologue, which also matches
     *   the keyword arguments into their slots.  Being the positional
     *   ABI, a native call site's hit path reaches it too. */
    is_kw = (bc->flags & 1) != 0;
    is_rest = !is_kw && (bc->arity & 0x8000) != 0;
    /* is_opt: the positional ABI with every slot in the LINK frame and
     * the count at -4(a6) -- &optional, &rest, &key or any mix. */
    is_opt = is_kw || bc->n_optional != 0 || is_rest;
    in_frame = is_opt;
    /* n_upvalues > 0 (= inner closures that READ their captures via
     * OP_UPVAL / mutate them via OP_CELL_SET_UPVAL) is now walker-
     * supported via the func_obj first-C-arg ABI: both opcodes route
     * through a helper that reads cl->upvalues[index] off func_obj
     * (at 8(a6)).  See OP_UPVAL / OP_CELL_SET_UPVAL cases below. */
    if (!is_kw) {
        /* Pre-existing fixed-arity gate. */
        if (bc->flags != 0)  return 0;
        if (bc->n_keys != 0) return 0;
    } else {
        /* &key allows flags bit 1 (allow-other-keys); n_keys may be
         * 0 when the user wrote `(&key &allow-other-keys)` with no
         * actual keyword params. */
        if (bc->flags & ~3u) return 0;
    }

    arity = (uint16_t)(bc->arity & 0x7FFF);
    /* cl_jit_invoke accepts positional arities 0..CL_JIT_MAX_POSITIONAL
     * (OP_CALL's byte). */
    if (arity + (uint32_t)bc->n_optional > CL_JIT_MAX_POSITIONAL)
        return 0;

    n_locals = bc->n_locals;
    if (n_locals < arity) return 0;
    /* The &rest list's slot follows the positional ones. */
    if ((bc->arity & 0x8000) &&
        n_locals <= arity + (uint32_t)bc->n_optional) return 0;
    n_extra = (uint32_t)(n_locals - arity);
    /* LINK takes a 16-bit signed disp; cap so 4*frame_slots plus the
     * 12 bytes for saved D5/D6/D7 stays within range with headroom.
     * Without a frame frame_slots == n_extra; with one it's n_locals
     * because the arguments are copied into it too. */
    if (n_extra > 1000) return 0;
    if (in_frame && n_locals > 1000) return 0;

    /* In-frame shapes: one more slot below the locals, -4(a6), holds
     * nargs. */
    if (is_opt)
        frame_size = (int16_t)(-4 * ((int32_t)n_locals + 1));
    else
        frame_size = (int16_t)(-4 * (int32_t)n_extra);
    slot_anchor = is_opt ? (uint16_t)(n_locals + 1) : arity;
    /* Saved cache registers sit below the frame at -frame_size-4/-8/-12
     * (A6-relative).  D7 is pushed first after LINK so it occupies the
     * slot closest to the frame (-4 below it); D5 is pushed last so it
     * ends up furthest from A6.  The OP_RET epilogue restores from
     * these positions independent of where SP happens to be. */
    saved_d7_disp = (int16_t)(frame_size - 4);
    saved_d6_disp = (int16_t)(frame_size - 8);
    saved_d5_disp = (int16_t)(frame_size - 12);

    /* bc_to_native[i] = native offset where the opcode beginning at
     * bytecode position i starts emitting (post-branch-target flush),
     * or -1 if that position is mid-operand / not yet reached.
     * is_target[i] = 1 if some branch lands at bytecode position i —
     * the walker flushes the cache at every such IP so the depth=0
     * invariant holds at branch boundaries. */
    bc_to_native = (int32_t *)platform_alloc(
        (unsigned long)((bc->code_len + 1) * sizeof(int32_t)));
    if (bc_to_native == NULL) return 0;
    for (i = 0; i <= bc->code_len; i++) bc_to_native[i] = -1;

    is_target = (uint8_t *)platform_alloc((unsigned long)(bc->code_len + 1));
    if (is_target == NULL) { platform_free(bc_to_native); return 0; }
    for (i = 0; i <= bc->code_len; i++) is_target[i] = 0;
    if (!prescan_branch_targets(bc, is_target, &dyn_extent)) goto fail;
    tail_jumps = !dyn_extent;

    /* Prologue: LINK frame, then save callee-clobbered cache regs
     * D5/D6/D7 below the frame.  Order matters: D7 first so it lives
     * at -frame_size-4(a6), D6 at -frame_size-8, D5 at -frame_size-12. */
    m68k_emit_link_an_disp16(cb, REG_A6, frame_size);
    m68k_emit_move_l_dn_predec_an(cb, REG_D7, REG_A7);
    m68k_emit_move_l_dn_predec_an(cb, REG_D6, REG_A7);
    m68k_emit_move_l_dn_predec_an(cb, REG_D5, REG_A7);

    /* &rest and &key prologues: a helper fills the LINK frame.
     *
     *   move.l  d1,-4(a6)          ; the count, for OP_ARGC
     *   lea     frame_size(a6),a0  ; push &frame[0]
     *   move.l  a0,-(a7)
     *   lea     12(a6),a0          ; push the last argument's address
     *   move.l  a0,-(a7)
     *   move.l  d1,-(a7)           ; push nargs
     *   move.l  8(a6),-(a7)        ; push func
     *   jsr     cl_jit_runtime_rest_prologue | cl_jit_runtime_kw_prologue
     *   lea     16(a7),a7
     *
     * Both NIL every slot, copy the positional arguments and cons the
     * &rest list; the kw helper then matches the keyword arguments into
     * `bc->key_slots[]` / `bc->key_suppliedp_slots[]`, signalling on an
     * odd count or an unknown keyword.  The consing may collect, so the
     * helpers take func and re-derive the bytecode, never a raw bc. */
    if (is_rest || is_kw) {
        uint32_t helper = is_kw
            ? (uint32_t)(uintptr_t)&cl_jit_runtime_kw_prologue
            : (uint32_t)(uintptr_t)&cl_jit_runtime_rest_prologue;
        m68k_emit_move_l_dn_to_disp_am(cb, REG_D1, -4, REG_A6);
        m68k_emit_lea_disp_an_to_am(cb, frame_size, REG_A6, REG_A0);
        m68k_emit_move_l_an_predec_am(cb, REG_A0, REG_A7);
        m68k_emit_lea_disp_an_to_am(cb, 12, REG_A6, REG_A0);
        m68k_emit_move_l_an_predec_am(cb, REG_A0, REG_A7);
        m68k_emit_move_l_dn_predec_an(cb, REG_D1, REG_A7);
        m68k_emit_move_l_disp_an_predec_am(cb, 8, REG_A6, REG_A7);
        m68k_emit_jsr_abs_l(cb, helper);
        m68k_emit_lea_disp_an_to_am(cb, 16, REG_A7, REG_A7);
    } else if (is_opt) {
        emit_opt_prologue(cb, frame_size, n_locals);
    }

    ip = 0;
    while (ip < bc->code_len) {
        uint8_t op;
        if (ip > 0) jit_lines_note(&lines, bc, cb, op_native, op_ip);
        /* If this IP is a branch target, flush the cache so the
         * "depth=0 at every branch boundary" invariant holds for both
         * the falling-through path and the branch-arriving path. */
        if (is_target[ip] && cache_depth > 0) {
            cache_flush(cb, &cache_head, &cache_depth);
        }
        bc_to_native[ip] = (int32_t)cb_len(cb);
        op_ip = ip;
        op_native = cb_len(cb);
        /* A loop header: the backward branch lands on the poll. */
        if (is_target[ip] & JIT_LOOP_HEAD) emit_loop_poll(cb);
        op = bc->code[ip++];
        switch (op) {
        case OP_NIL:
            cache_push_imm(cb, &cache_head, &cache_depth, (uint32_t)CL_NIL);
            break;

        case OP_T:
            cache_push_obj(cb, &cache_head, &cache_depth, relocs, CL_T);
            break;

        case OP_CONST: {
            uint16_t idx;
            CL_Obj val;
            if (ip + 1 >= bc->code_len) goto fail;
            idx = ((uint16_t)bc->code[ip] << 8) | bc->code[ip + 1];
            ip += 2;
            if (idx >= bc->n_constants || bc->constants == NULL) goto fail;
            val = bc->constants[idx];
            cache_push_obj(cb, &cache_head, &cache_depth, relocs, val);
            break;
        }

        case OP_LOAD: {
            uint8_t slot;
            int16_t disp;
            if (ip >= bc->code_len) goto fail;
            slot = bc->code[ip++];
            if (slot >= n_locals) goto fail;
            disp = slot_disp(slot, slot_anchor, in_frame);
            cache_push_disp_an(cb, &cache_head, &cache_depth, disp, REG_A6);
            break;
        }

        case OP_STORE: {
            /* STORE peeks TOS (no pop) and writes to the local slot.
             * When TOS is cached, this is a register→memory write;
             * otherwise it's the existing memory→memory copy. */
            uint8_t slot;
            int16_t disp;
            if (ip >= bc->code_len) goto fail;
            slot = bc->code[ip++];
            if (slot >= n_locals) goto fail;
            disp = slot_disp(slot, slot_anchor, in_frame);
            if (cache_depth >= 1) {
                m68k_emit_move_l_dn_to_disp_am(cb, (M68kReg)cache_head,
                                               disp, REG_A6);
            } else {
                m68k_emit_move_l_an_to_disp_am(cb, REG_A7, disp, REG_A6);
            }
            break;
        }

        case OP_POP:
            cache_drop(cb, &cache_head, &cache_depth);
            break;

        case OP_DUP:
            cache_dup(cb, &cache_head, &cache_depth);
            break;

        case OP_ADD: {
            /* Operands: TOS = b, second = a.  Pop both, run the shared
             * compute, push result.  No pre-flush: the fixnum fast path
             * doesn't allocate, so spilling D5/D6/D7 up front is wasted
             * work on the hot path.  emit_arith_compute spills/reloads
             * around the slow-path JSR only, where the conservative
             * scan needs the cache values on the stack. */
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D1);
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D0);
            emit_arith_compute(cb, /*is_sub=*/0,
                               (uint32_t)(uintptr_t)&cl_jit_runtime_add,
                               cache_head, cache_depth);
            cache_push_dn(cb, &cache_head, &cache_depth, REG_D0);
            break;
        }

        case OP_SUB: {
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D1);
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D0);
            emit_arith_compute(cb, /*is_sub=*/1,
                               (uint32_t)(uintptr_t)&cl_jit_runtime_sub,
                               cache_head, cache_depth);
            cache_push_dn(cb, &cache_head, &cache_depth, REG_D0);
            break;
        }

        case OP_MUL: {
            /* JSR-only template — see runtime helper comment for why
             * there's no inline MUL fast path.  Pop b/a, push them as
             * C-ABI right-to-left args, JSR, drop args, push the D0
             * result back through the cache.  Flush after pops:
             * cl_arith_mul can allocate (bignum). */
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_mul;
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D1);   /* b */
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D0);   /* a */
            emit_helper_call_2(cb, &cache_head, &cache_depth, helper);
            break;
        }

        case OP_DIV: {
            /* Same shape as OP_MUL — no inline fixnum fast path, all
             * traffic goes through cl_jit_runtime_div which type-checks
             * NUMBER and delegates to cl_arith_div.  Today's compiler
             * doesn't emit OP_DIV from `/` (it routes through the
             * regular OP_CALL path), so this template is reached only
             * by handwritten OP_DIV emitters / future inliner work —
             * having it in place keeps the walker complete and avoids a
             * latent bail when an inliner does start using it. */
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_div;
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D1);   /* b */
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D0);   /* a */
            emit_helper_call_2(cb, &cache_head, &cache_depth, helper);
            break;
        }

        case OP_GLOAD: {
            /* See historical comments above: bake constants[idx] (a
             * SYMBOL) into the JSR site, helper resolves TLV / global
             * cell on each call.  With the cache, the only changes
             * vs. the pre-cache code are using cache_push_dn for the
             * result (no need to push it to stack), and a full flush
             * before the helper call because the helper expects the
             * operand stack on memory.
             *
             * Helpers preserve D5/D6/D7 per SysV callee-save, so the
             * cache content survives across the JSR — we only need to
             * flush if subsequent code below the helper's args
             * depends on the cached values being on the stack.  But
             * since cl_jit_runtime_gload takes a single immediate
             * arg (the symbol, baked in as a literal) and reads
             * nothing from the operand stack, we don't actually need
             * to spill the cache for *this* op.  Same applies to
             * GSTORE/FLOAD/DYNBIND/DYNUNBIND below — they all take
             * baked immediates as args, not operand-stack values
             * other than the explicit TOS handling we already
             * implement. */
            uint16_t idx;
            CL_Obj sym;
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_gload;
            if (ip + 1 >= bc->code_len) goto fail;
            idx = ((uint16_t)bc->code[ip] << 8) | bc->code[ip + 1];
            ip += 2;
            if (idx >= bc->n_constants || bc->constants == NULL) goto fail;
            sym = bc->constants[idx];
            if (!CL_SYMBOL_P(sym)) goto fail;

            cache_flush(cb, &cache_head, &cache_depth);
            emit_obj_imm_predec(cb, relocs, sym);
            m68k_emit_jsr_abs_l(cb, helper);
            m68k_emit_addq_l_an(cb, 4, REG_A7);
            cache_push_dn(cb, &cache_head, &cache_depth, REG_D0);
            break;
        }

        case OP_GSTORE: {
            /* Peek TOS as `val`, JSR cl_jit_runtime_gstore(sym, val),
             * leave TOS untouched (VM's "store without pop"
             * semantics).  Cache-aware peek picks D7 when TOS is
             * cached, (a7) otherwise. */
            uint16_t idx;
            CL_Obj sym;
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_gstore;
            if (ip + 1 >= bc->code_len) goto fail;
            idx = ((uint16_t)bc->code[ip] << 8) | bc->code[ip + 1];
            ip += 2;
            if (idx >= bc->n_constants || bc->constants == NULL) goto fail;
            sym = bc->constants[idx];
            if (!CL_SYMBOL_P(sym)) goto fail;

            /* Flush before peek so TOS lives at (a7) — the helper may
             * allocate (package sync of *PACKAGE* path, errors); the
             * conservative scan reaches stack but not cache regs. */
            cache_flush(cb, &cache_head, &cache_depth);
            m68k_emit_move_l_an_to_predec_am(cb, REG_A7, REG_A7);
            emit_obj_imm_predec(cb, relocs, sym);
            m68k_emit_jsr_abs_l(cb, helper);
            m68k_emit_addq_l_an(cb, 8, REG_A7);
            break;
        }

        case OP_FSTORE: {
            /* Same peek-and-store shape as OP_GSTORE, but writes
             * sym->function rather than the dynamic value cell.
             * Compiler emits OP_FSTORE for `(defun ...)` so any
             * top-level body containing a `defun` (or a `flet` /
             * `labels`-style emission that resolves to FSTORE) now
             * widens to the walker. */
            uint16_t idx;
            CL_Obj sym;
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_fstore;
            if (ip + 1 >= bc->code_len) goto fail;
            idx = ((uint16_t)bc->code[ip] << 8) | bc->code[ip + 1];
            ip += 2;
            if (idx >= bc->n_constants || bc->constants == NULL) goto fail;
            sym = bc->constants[idx];
            if (!CL_SYMBOL_P(sym)) goto fail;

            cache_flush(cb, &cache_head, &cache_depth);
            m68k_emit_move_l_an_to_predec_am(cb, REG_A7, REG_A7);
            emit_obj_imm_predec(cb, relocs, sym);
            m68k_emit_jsr_abs_l(cb, helper);
            m68k_emit_addq_l_an(cb, 8, REG_A7);
            break;
        }

        case OP_DYNBIND: {
            /* Pop val into D1, push it + sym, JSR, drop both. */
            uint16_t idx;
            CL_Obj sym;
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_dynbind;
            if (ip + 1 >= bc->code_len) goto fail;
            idx = ((uint16_t)bc->code[ip] << 8) | bc->code[ip + 1];
            ip += 2;
            if (idx >= bc->n_constants || bc->constants == NULL) goto fail;
            sym = bc->constants[idx];
            if (!CL_SYMBOL_P(sym)) goto fail;

            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D1);
            cache_flush(cb, &cache_head, &cache_depth);
            m68k_emit_move_l_dn_predec_an(cb, REG_D1, REG_A7);
            emit_obj_imm_predec(cb, relocs, sym);
            m68k_emit_jsr_abs_l(cb, helper);
            m68k_emit_addq_l_an(cb, 8, REG_A7);
            break;
        }

        case OP_DYNUNBIND: {
            uint8_t count;
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_dynunbind;
            if (ip >= bc->code_len) goto fail;
            count = bc->code[ip++];

            cache_flush(cb, &cache_head, &cache_depth);
            m68k_emit_move_l_imm32_predec(cb, (uint32_t)count, REG_A7);
            m68k_emit_jsr_abs_l(cb, helper);
            m68k_emit_addq_l_an(cb, 4, REG_A7);
            break;
        }

        case OP_PROGV_BIND: {
            /* Operand stack on entry: TOS = values_list, second =
             * symbols_list.  Pop both, flush, JSR helper, push the
             * returned mark fixnum.  Helper installs the dyn-bindings
             * and returns the saved cl_dyn_top as CL_MAKE_FIXNUM so
             * the matching OP_PROGV_UNBIND can restore exactly the
             * same prefix.  Helper signals on non-symbol / overflow
             * (allocates condition objects) → cache_flush mandatory. */
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_progv_bind;
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D1);  /* values_list */
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D0);  /* symbols_list */
            emit_helper_call_2(cb, &cache_head, &cache_depth, helper);
            break;
        }

        case OP_PROGV_UNBIND: {
            /* Operand stack on entry: TOS = body result, second =
             * mark fixnum from the matching OP_PROGV_BIND.  Pop both,
             * flush, JSR helper(mark, result), push result back.
             * Helper restores dyn-bindings and returns the result
             * verbatim so peek semantics are preserved.  The restore
             * itself is non-allocating but may invoke cl_set_package
             * for a *PACKAGE* rebinding which can allocate via error
             * paths → cache_flush before the JSR. */
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_progv_unbind;
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D1);  /* result */
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D0);  /* mark */
            emit_helper_call_2(cb, &cache_head, &cache_depth, helper);
            break;
        }

        case OP_BLOCK_PUSH: {
            /* u16 tag_idx, i32 offset.  Implementation: emit a JSR to
             * libc setjmp *inline* — setjmp's saved frame is the JIT'd
             * function itself, so a later longjmp (issued by
             * OP_BLOCK_RETURN's helper) rewinds back into our native
             * code at the post-JSR instruction with D0 != 0.
             *
             * Sequence:
             *   cache_flush             (BLOCK_PUSH itself isn't a
             *                            branch target but the JSR
             *                            needs depth=0 anyway)
             *   PEA tag                 -- alloc helper arg
             *   JSR block_alloc         -- D0 = &nlx->buf
             *   ADDQ #4,A7
             *   MOVE.L D0,-(A7)         -- setjmp arg
             *   JSR setjmp              -- D0 = 0 / non-zero
             *   ADDQ #4,A7
             *   TST.L D0
             *   BEQ.W normal_path
             *   JSR block_post_longjmp  -- D0 = nlx->result
             *   MOVE.L D0,-(A7)         -- push result onto operand stack
             *   BRA.W after_landing     -- forward to landing PC
             *   normal_path:
             *   JSR block_commit        -- cl_nlx_top++
             *   (fall through into body)
             *
             * The landing PC is recorded in is_target[] by the
             * prescan, so the walker will cache_flush at that IP when
             * it gets there — both paths arrive with depth=0 and the
             * block result sitting at (a7). */
            uint16_t tag_idx;
            CL_Obj tag;
            int32_t bc_offset;
            uint32_t landing_bc_ip;
            uint32_t alloc_helper      = (uint32_t)(uintptr_t)&cl_jit_runtime_block_alloc;
            uint32_t commit_helper     = (uint32_t)(uintptr_t)&cl_jit_runtime_block_commit;
            uint32_t post_longjmp_helper = (uint32_t)(uintptr_t)&cl_jit_runtime_block_post_longjmp;

            if (ip + 6 > bc->code_len) goto fail;
            tag_idx = ((uint16_t)bc->code[ip] << 8) | bc->code[ip + 1];
            ip += 2;
            bc_offset = read_i32_be(bc->code + ip);
            ip += 4;
            if (tag_idx >= bc->n_constants || bc->constants == NULL) goto fail;
            tag = bc->constants[tag_idx];

            /* landing_bc_ip mirrors VM: catch_ip = ip (now == start
             * of body), so landing = ip + bc_offset. */
            {
                int ok;
                landing_bc_ip = compute_landing_ip(bc_offset, ip,
                                                   bc->code_len, &ok);
                if (!ok) goto fail;
            }

            cache_flush(cb, &cache_head, &cache_depth);

            /* alloc: PEA tag (via imm32 predec), JSR alloc, drop arg. */
            emit_obj_imm_predec(cb, relocs, tag);
            m68k_emit_jsr_abs_l(cb, alloc_helper);
            m68k_emit_addq_l_an(cb, 4, REG_A7);

            /* setjmp → dispatch → push result → BRA landing → commit. */
            if (!emit_nlx_setjmp_tail(cb, post_longjmp_helper, commit_helper,
                                      /*push_result*/1, landing_bc_ip, ip,
                                      bc_to_native, &patches, &n_patches,
                                      &cap_patches)) goto fail;
            /* Cache depth stays 0; body executes from here. */
            break;
        }

        case OP_BLOCK_POP: {
            /* Helper does the search-backward decrement of cl_nlx_top.
             * Operand stack is untouched — the body left its value
             * there, and the next op (typically right after the
             * landing IP, which we already mark as a branch target)
             * picks it up. */
            uint32_t pop_helper = (uint32_t)(uintptr_t)&cl_jit_runtime_block_pop;
            /* Helper is non-allocating but reads CT pointer.  Cache
             * survives across the JSR (D5/D6/D7 callee-saved per
             * m68k SysV), so no flush needed. */
            m68k_emit_jsr_abs_l(cb, pop_helper);
            break;
        }

        case OP_BLOCK_RETURN: {
            /* u16 tag_idx.  Helper never returns (longjmps back to
             * the matching BLOCK_PUSH's setjmp site).  The walker
             * keeps emitting subsequent ops past this point; that
             * code is dead at runtime but harmless. */
            uint16_t tag_idx;
            CL_Obj tag;
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_block_return;
            if (ip + 1 >= bc->code_len) goto fail;
            tag_idx = ((uint16_t)bc->code[ip] << 8) | bc->code[ip + 1];
            ip += 2;
            if (tag_idx >= bc->n_constants || bc->constants == NULL) goto fail;
            tag = bc->constants[tag_idx];

            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D1);   /* value */
            cache_flush(cb, &cache_head, &cache_depth);

            /* push value, then tag (so tag at (a7), value at 4(a7)). */
            m68k_emit_move_l_dn_predec_an(cb, REG_D1, REG_A7);
            emit_obj_imm_predec(cb, relocs, tag);
            m68k_emit_jsr_abs_l(cb, helper);
            /* Unreachable past here — helper longjmps.  Don't bother
             * with the addq; if we ever fall through (we don't), it'd
             * be a logic bug worth detecting via the leftover args
             * rather than silently masking. */
            break;
        }

        case OP_CATCH: {
            /* i32 offset; tag popped from operand stack.  Same
             * JIT-inline-setjmp shape as OP_BLOCK_PUSH — see that
             * comment for the full sequence.  Differences:
             *   - tag is a runtime value (cache_pop_to_dn before the
             *     JSR), not a const-pool lookup;
             *   - the matching pop is OP_UNCATCH, not OP_BLOCK_POP. */
            int32_t bc_offset;
            uint32_t landing_bc_ip;
            uint32_t alloc_helper        = (uint32_t)(uintptr_t)&cl_jit_runtime_catch_alloc;
            uint32_t commit_helper       = (uint32_t)(uintptr_t)&cl_jit_runtime_catch_commit;
            uint32_t post_longjmp_helper = (uint32_t)(uintptr_t)&cl_jit_runtime_catch_post_longjmp;

            if (ip + 4 > bc->code_len) goto fail;
            bc_offset = read_i32_be(bc->code + ip);
            ip += 4;

            /* landing_bc_ip mirrors VM: catch_ip = ip (now == start
             * of body), so landing = ip + bc_offset. */
            {
                int ok;
                landing_bc_ip = compute_landing_ip(bc_offset, ip,
                                                   bc->code_len, &ok);
                if (!ok) goto fail;
            }

            /* Tag (TOS) goes through D0 → (a7) as the alloc helper's
             * sole arg.  Flush after the pop so all other cached
             * values land on the m68k stack and the conservative scan
             * keeps them rooted across the alloc (cl_error on NLX
             * overflow allocates a condition). */
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D0);
            cache_flush(cb, &cache_head, &cache_depth);
            m68k_emit_move_l_dn_predec_an(cb, REG_D0, REG_A7);
            m68k_emit_jsr_abs_l(cb, alloc_helper);
            m68k_emit_addq_l_an(cb, 4, REG_A7);

            /* setjmp → dispatch → push result → BRA landing → commit. */
            if (!emit_nlx_setjmp_tail(cb, post_longjmp_helper, commit_helper,
                                      /*push_result*/1, landing_bc_ip, ip,
                                      bc_to_native, &patches, &n_patches,
                                      &cap_patches)) goto fail;
            /* Cache depth stays 0; body executes from here. */
            break;
        }

        case OP_UNCATCH: {
            /* Mirror of OP_BLOCK_POP: helper does the search-backward
             * decrement of cl_nlx_top for the matching CATCH frame.
             * Non-allocating; D5/D6/D7 cache regs are callee-saved
             * across the JSR per m68k SysV, so no flush needed. */
            uint32_t pop_helper = (uint32_t)(uintptr_t)&cl_jit_runtime_catch_pop;
            m68k_emit_jsr_abs_l(cb, pop_helper);
            break;
        }

        case OP_TAGBODY_PUSH: {
            /* u16 id_idx, i32 offset.  Same inline-setjmp shape as
             * OP_BLOCK_PUSH (see that comment for the full sequence
             * and rationale).  Two differences:
             *
             *   - The longjmp arm runs the tagbody_post_longjmp helper
             *     which returns the tag_index in D0; we push D0 onto
             *     the operand stack so the dispatch shim emitted right
             *     after PUSH (OP_DUP/OP_CONST/OP_EQ/OP_JTRUE per tag)
             *     can route to the right body.
             *   - The post_longjmp helper itself re-arms the frame
             *     (cl_nlx_top++), so GO into this tagbody can fire
             *     repeatedly until OP_TAGBODY_POP eventually unwinds. */
            uint16_t id_idx;
            CL_Obj tagbody_id;
            int32_t bc_offset;
            uint32_t landing_bc_ip;
            uint32_t alloc_helper        = (uint32_t)(uintptr_t)&cl_jit_runtime_tagbody_alloc;
            uint32_t commit_helper       = (uint32_t)(uintptr_t)&cl_jit_runtime_tagbody_commit;
            uint32_t post_longjmp_helper = (uint32_t)(uintptr_t)&cl_jit_runtime_tagbody_post_longjmp;

            if (ip + 6 > bc->code_len) goto fail;
            id_idx = ((uint16_t)bc->code[ip] << 8) | bc->code[ip + 1];
            ip += 2;
            bc_offset = read_i32_be(bc->code + ip);
            ip += 4;
            if (id_idx >= bc->n_constants || bc->constants == NULL) goto fail;
            tagbody_id = bc->constants[id_idx];

            /* landing_bc_ip mirrors VM: catch_ip == ip after consuming
             * the 6-byte operand, so landing = ip + bc_offset. */
            {
                int ok;
                landing_bc_ip = compute_landing_ip(bc_offset, ip,
                                                   bc->code_len, &ok);
                if (!ok) goto fail;
            }

            cache_flush(cb, &cache_head, &cache_depth);

            /* alloc(tagbody_id) → D0 = &nlx->buf. */
            emit_obj_imm_predec(cb, relocs, tagbody_id);
            m68k_emit_jsr_abs_l(cb, alloc_helper);
            m68k_emit_addq_l_an(cb, 4, REG_A7);

            /* setjmp → dispatch → push tag_index → BRA landing → commit.
             * The pushed D0 is the tag_index the dispatch shim (emitted
             * right after the landing) routes on. */
            if (!emit_nlx_setjmp_tail(cb, post_longjmp_helper, commit_helper,
                                      /*push_result*/1, landing_bc_ip, ip,
                                      bc_to_native, &patches, &n_patches,
                                      &cap_patches)) goto fail;
            /* Cache depth stays 0; tagbody body executes from here. */
            break;
        }

        case OP_TAGBODY_POP: {
            /* Search-backward decrement of cl_nlx_top.  Operand stack
             * is untouched.  Non-allocating, cache survives the JSR. */
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_tagbody_pop;
            m68k_emit_jsr_abs_l(cb, helper);
            break;
        }

        case OP_TAGBODY_GO: {
            /* u16 id_idx (tagbody identifier baked into the JSR).
             * Operand stack: TOS = tag_index (popped).  Helper never
             * returns (longjmps to the matching tagbody's setjmp
             * site).  Code emitted past this point is dead at runtime
             * but harmless — matches OP_BLOCK_RETURN's pattern. */
            uint16_t id_idx;
            CL_Obj tagbody_id;
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_tagbody_go;
            if (ip + 1 >= bc->code_len) goto fail;
            id_idx = ((uint16_t)bc->code[ip] << 8) | bc->code[ip + 1];
            ip += 2;
            if (id_idx >= bc->n_constants || bc->constants == NULL) goto fail;
            tagbody_id = bc->constants[id_idx];

            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D1);    /* tag_index */
            cache_flush(cb, &cache_head, &cache_depth);

            /* Helper signature: (tagbody_id, tag_index).  C-ABI order:
             * id at (a7), tag_index at 4(a7).  Push tag_index first
             * (high address), then id (low address). */
            m68k_emit_move_l_dn_predec_an(cb, REG_D1, REG_A7);
            emit_obj_imm_predec(cb, relocs, tagbody_id);
            m68k_emit_jsr_abs_l(cb, helper);
            /* Unreachable past here — helper longjmps. */
            break;
        }

        case OP_UWPROT: {
            /* i32 offset.  Same inline-setjmp shape as OP_BLOCK_PUSH
             * (see that comment for the full rationale).  Differences:
             *
             *   - No tag operand (UWPROT frames are unkeyed; the
             *     UWPROT alloc helper takes no args).
             *   - Both the normal-exit and longjmp-arrival paths
             *     converge at the same bytecode IP (= cleanup_start,
             *     which the compiler patches identically for the
             *     UWPROT offset and the immediately-following JMP).
             *     The longjmp path BRA's there; the normal path runs
             *     the protected form which ends with an OP_JMP to the
             *     same landing.  Both paths arrive with cache depth 0
             *     and no value on the operand stack — the protected
             *     form's MVs were captured into a local slot via
             *     OP_MV_TO_LIST in the compiled epilogue.
             *   - uwprot_post_longjmp returns void (no value to push)
             *     because the cleanup runs purely for side effects;
             *     it just restores the marks the alloc captured. */
            int32_t bc_offset;
            uint32_t landing_bc_ip;
            uint32_t alloc_helper        = (uint32_t)(uintptr_t)&cl_jit_runtime_uwprot_alloc;
            uint32_t commit_helper       = (uint32_t)(uintptr_t)&cl_jit_runtime_uwprot_commit;
            uint32_t post_longjmp_helper = (uint32_t)(uintptr_t)&cl_jit_runtime_uwprot_post_longjmp;

            if (ip + 4 > bc->code_len) goto fail;
            bc_offset = read_i32_be(bc->code + ip);
            ip += 4;

            /* landing_bc_ip = ip + offset (catch_ip == ip after the i32). */
            {
                int ok;
                landing_bc_ip = compute_landing_ip(bc_offset, ip,
                                                   bc->code_len, &ok);
                if (!ok) goto fail;
            }

            cache_flush(cb, &cache_head, &cache_depth);

            /* alloc: no arg.  D0 = &nlx->buf. */
            m68k_emit_jsr_abs_l(cb, alloc_helper);

            /* setjmp → dispatch → BRA landing → commit.  push_result=0:
             * uwprot_post_longjmp returns void, nothing goes on the
             * operand stack (cleanup runs for side effects only). */
            if (!emit_nlx_setjmp_tail(cb, post_longjmp_helper, commit_helper,
                                      /*push_result*/0, landing_bc_ip, ip,
                                      bc_to_native, &patches, &n_patches,
                                      &cap_patches)) goto fail;
            break;
        }

        case OP_UWPOP: {
            /* Normal-exit pop: search-backward decrement of cl_nlx_top
             * and clear cl_pending_throw.  Non-allocating, cache
             * survives across the JSR (D5/D6/D7 callee-saved). */
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_uwprot_pop;
            m68k_emit_jsr_abs_l(cb, helper);
            break;
        }

        case OP_UWRETHROW: {
            /* If cl_pending_throw is set, helper longjmps and never
             * returns; if not, helper is a nop and returns to us.  The
             * longjmp path may eventually allocate condition objects
             * via cl_error, so flush the cache so the conservative
             * scan can reach any spilled cache values across the
             * unwinding chain. */
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_uwprot_rethrow;
            cache_flush(cb, &cache_head, &cache_depth);
            m68k_emit_jsr_abs_l(cb, helper);
            break;
        }

        case OP_MV_TO_LIST: {
            /* Pops primary into D0, calls helper which allocates the
             * list from cl_mv_values, pushes result back through cache.
             * Allocating, so flush before the JSR so the conservative
             * scan can reach any cached operand-stack values. */
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_mv_to_list;
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D0);
            emit_helper_call_1(cb, &cache_head, &cache_depth, helper, 1);
            break;
        }

        case OP_MV_SAVE: {
            /* Pops the primary into D0 and parks it with the MV buffer on
             * the per-thread save stack (compile_unwind_protect, after the
             * protected form).  Non-allocating helper, nothing pushed back:
             * emit the call by hand rather than through emit_helper_call_1,
             * which always pushes D0. */
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_mv_save;
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D0);
            cache_flush(cb, &cache_head, &cache_depth);
            m68k_emit_move_l_dn_predec_an(cb, REG_D0, REG_A7);
            m68k_emit_jsr_abs_l(cb, helper);
            m68k_emit_addq_l_an(cb, 4, REG_A7);
            break;
        }

        case OP_MV_RESTORE: {
            /* Pops the save-stack record back into the MV buffer; the
             * helper returns the primary in D0, which becomes TOS. */
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_mv_restore;
            cache_flush(cb, &cache_head, &cache_depth);
            m68k_emit_jsr_abs_l(cb, helper);
            cache_push_dn(cb, &cache_head, &cache_depth, REG_D0);
            break;
        }

        case OP_MV_RESET: {
            /* Explicit OP_MV_RESET — emitted by the compiler between
             * (and …) / (or …) arms so a falsy result doesn't carry
             * stale multiple-values into the next test.  Helper just
             * writes `cl_mv_count = 1` to the current thread, no
             * allocation, doesn't read/write the operand stack.
             *
             * No cache_flush: the helper can't trigger GC and only
             * the m68k caller-saved D0/D1 are at risk across the
             * JSR — both already understood as scratch by the
             * walker.  D5/D6/D7 (cache regs) are callee-saved per
             * the m68k C ABI, so they survive the call untouched.
             *
             * This is the narrow fix to the open jit-mv-count
             * memory item — only handles the explicit op, NOT the
             * broader "reset on every value-producing opcode" that
             * previously broke CLOS. */
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_mv_reset;
            m68k_emit_jsr_abs_l(cb, helper);
            break;
        }

        case OP_FLOAD: {
            uint16_t idx;
            CL_Obj sym;
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_fload;
            if (ip + 1 >= bc->code_len) goto fail;
            idx = ((uint16_t)bc->code[ip] << 8) | bc->code[ip + 1];
            ip += 2;
            if (idx >= bc->n_constants || bc->constants == NULL) goto fail;
            sym = bc->constants[idx];
            if (!CL_SYMBOL_P(sym)) goto fail;

            cache_flush(cb, &cache_head, &cache_depth);
            emit_obj_imm_predec(cb, relocs, sym);
            m68k_emit_jsr_abs_l(cb, helper);
            m68k_emit_addq_l_an(cb, 4, REG_A7);
            cache_push_dn(cb, &cache_head, &cache_depth, REG_D0);
            break;
        }

        case OP_CALL: {
            /* u8 nargs.  The helper reads its args off the m68k
             * stack, so we flush the cache fully first to make sure
             * the operand top + args live in memory at known
             * addresses.  After the flush, the existing CALL template
             * works unchanged; the helper's D0 result then gets
             * pushed back through the cache for downstream ops. */
            uint8_t nargs;
            if (ip >= bc->code_len) goto fail;
            nargs = bc->code[ip++];

            cache_flush(cb, &cache_head, &cache_depth);
            emit_call_site(cb, relocs, &sites, CL_NIL, 0, nargs, -1);
            cache_push_dn(cb, &cache_head, &cache_depth, REG_D0);
            break;
        }

        case OP_TAILCALL: {
            /* u8 nargs.  Two code paths:
             *
             *   1. Self-recursive TCO.  When `nargs == arity` and
             *      `bc->name` is a SYMBOL (so the call could plausibly
             *      have come from `OP_FLOAD <name>; ...; OP_TAILCALL n`),
             *      emit a runtime guard that compares the func value
             *      sitting at 4*N(a7) against this bytecode's CL_Obj.
             *      If equal — the symbol still resolves to *this*
             *      function — copy the N args from the operand stack
             *      into the A6 frame slots, drop the operand stack,
             *      and `bra.w` back to entry_after_prologue
             *      (bc_to_native[0], set before any opcode emits).
             *      Zero m68k-stack growth: the same LINK frame is
             *      reused for the next iteration.
             *
             *   2. Fallback / non-self.  If the guard misses, or the
             *      shape doesn't qualify (arity mismatch, anonymous
             *      lambda), fall through to the generic helper path:
             *      a call site (emit_call_site), restore cache
             *      regs, UNLK + RTS.  This is identical to OP_CALL's
             *      sequence except we RTS with D0 directly instead of
             *      pushing the result onto our own operand stack only
             *      for the compiler-mandated trailing OP_RET to pop
             *      it again.  The OP_RET below still emits its own
             *      restore + unlk + rts as dead code, harmless.
             *
             * Redefinition correctness: if `(setf (symbol-function
             * 'foo) #'bar)` runs after foo has been JIT-compiled,
             * subsequent `(foo …)` calls go through OP_FLOAD which
             * returns bar.  Our guard then compares bar != self_bc,
             * misses, and we route through the call site as the
             * VM would — semantics preserved.  Compaction moving the
             * bytecode also lands on the fallback (immediate stale ≠
             * new CL_Obj), which is correct.
             *
             * Out-of-range backward branch: if bc_to_native[0] is
             * further than 32 KB back from this OP_TAILCALL, the
             * Bcc.W displacement won't fit and we bail the whole
             * walker compile (the function runs interpreted).  Loop
             * bodies fit well under that limit in practice. */
            uint8_t nargs;
            int self_tco;
            int32_t cell;
            int32_t guard_branch_pc = 0;
            if (ip >= bc->code_len) goto fail;
            nargs = bc->code[ip++];

            cache_flush(cb, &cache_head, &cache_depth);

            /* Self-recursive TCO is disabled for keyworded shapes:
             * the frame copy below does no keyword matching (that is
             * cl_jit_runtime_kw_prologue's).  Recursive &key calls take
             * the call-site path (still tail-position correct, just at
             * the cost of one extra m68k frame per call).
             * An &optional function takes any count its lambda list
             * accepts: emit_opt_self_tco_copy rewrites the frame.  So
             * does an &rest one, up to the positional count -- the copy
             * NILs the rest slot, the empty list such a call conses. */
            self_tco = !is_kw && nargs >= arity &&
                       nargs <= arity + bc->n_optional &&
                       CL_SYMBOL_P(bc->name);

            if (self_tco) {
                uint32_t self_obj = (uint32_t)CL_PTR_TO_OBJ(bc);
                uint32_t guard = (uint32_t)(uintptr_t)&cl_jit_runtime_is_self_tco;
                int32_t entry_off, bra_disp;
                uint32_t i;

                /* Guard: call cl_jit_runtime_is_self_tco(func, self_bc).
                 * Helper dereferences closures (the dominant defun
                 * shape — compile_lambda emits OP_CLOSURE on every
                 * defun, so sym->function is always a CL_Closure
                 * wrapping the bytecode, never the bytecode CL_Obj
                 * itself).  Inline cmp on raw CL_Obj would always
                 * miss.  Helper returns 1/0 in D0.
                 *
                 * Push self_bc first (2nd C-ABI arg), then func.
                 * After the second predec, func's original slot is
                 * at 4*(N+2)(a7) — we read it via the
                 * disp-an-predec-am encoder which uses A7 for both
                 * source and dest (source EA evaluated before the
                 * predec, so the read picks up the pre-predec slot
                 * just fine). */
                emit_obj_imm_predec(cb, relocs, self_obj);
                m68k_emit_move_l_disp_an_predec_am(cb,
                    (int16_t)(4 * ((int32_t)nargs + 1)), REG_A7, REG_A7);
                m68k_emit_jsr_abs_l(cb, guard);
                m68k_emit_addq_l_an(cb, 8, REG_A7);
                m68k_emit_tst_l_dn(cb, REG_D0);
                guard_branch_pc = (int32_t)cb_len(cb) + 2;
                m68k_emit_beq_w(cb, 0);   /* D0==0 → not self → fallback */

                /* Copy args into frame slots.  src[i] = 4*i(a7) holds
                 * arg N-1-i, whose parameter slot is at
                 * slot_disp(N-1-i, arity) = 12+4*i(a6): both sides are
                 * in operand-stack order, so this is a straight block
                 * copy (ascending, and the source lies below the
                 * frame, so the order of the moves does not matter). */
                if (is_opt) {
                    emit_opt_self_tco_copy(cb, nargs, slot_anchor, n_locals);
                } else for (i = 0; i < nargs; i++) {
                    int16_t src_disp = (int16_t)(4 * (int32_t)i);
                    /* Required parameters only: slot_anchor == arity
                     * here and the positional layout applies. */
                    int16_t dst_disp = slot_disp((uint8_t)(nargs - 1 - i),
                                                 slot_anchor, in_frame);
                    m68k_emit_move_l_disp_an_to_dn(cb, src_disp, REG_A7, REG_D0);
                    m68k_emit_move_l_dn_to_disp_am(cb, REG_D0, dst_disp, REG_A6);
                }

                /* Reset A7 to the operand-stack base — right below the
                 * three saved cache regs.  Can't just `lea 4*(N+1)(a7)`
                 * because the operand stack may hold spilled cache
                 * values below this OP_TAILCALL's args (whatever the
                 * function pushed before reaching us, then flushed
                 * above).  Going through A6 ignores all of that and
                 * lands at the canonical empty-operand-stack address
                 * the bra target expects. */
                m68k_emit_lea_disp_an_to_am(cb,
                    (int16_t)(frame_size - 12), REG_A6, REG_A7);

                /* The self tail call is a loop: poll before going round. */
                emit_loop_poll(cb);
                entry_off = bc_to_native[0];
                bra_disp = entry_off - ((int32_t)cb_len(cb) + 2);
                if (bra_disp < -32768 || bra_disp > 32767) goto fail;
                m68k_emit_bra_w(cb, (int16_t)bra_disp);

                /* Patch the guard's BEQ to land at the fallback (right here). */
                m68k_patch_disp16(cb_data(cb), cb_len(cb),
                                  (uint32_t)guard_branch_pc,
                                  (int16_t)((int32_t)cb_len(cb) - guard_branch_pc));
            }

            /* Another function: jump into it where emit_tail_call_site
             * can, else (and on its way out) a call site, then return. */
            cell = -1;
            if (tail_jumps && nargs <= arity)
                cell = emit_tail_call_site(cb, &sites, 0, nargs,
                                           saved_d7_disp, saved_d6_disp,
                                           saved_d5_disp);
            emit_call_site(cb, relocs, &sites, CL_NIL, 0, nargs, cell);

            /* Result already in D0; restore callee-saved cache regs
             * from their A6-relative slots and return to our caller.
             * Cache is depth=0 from the flush above, matching the
             * canonical state at branch boundaries — safe for any
             * later branch target that lands past us. */
            m68k_emit_move_l_disp_an_to_dn(cb, saved_d7_disp, REG_A6, REG_D7);
            m68k_emit_move_l_disp_an_to_dn(cb, saved_d6_disp, REG_A6, REG_D6);
            m68k_emit_move_l_disp_an_to_dn(cb, saved_d5_disp, REG_A6, REG_D5);
            m68k_emit_unlk_an(cb, REG_A6);
            m68k_emit_rts(cb);
            break;
        }

        case OP_LOAD_CALL_GLOBAL: {
            /* u8 slot, then OP_CALL_GLOBAL's operands: push the local,
             * then the call. */
            uint8_t slot;
            if (ip >= bc->code_len) goto fail;
            slot = bc->code[ip++];
            if (slot >= n_locals) goto fail;
            cache_push_disp_an(cb, &cache_head, &cache_depth,
                               slot_disp(slot, slot_anchor, in_frame), REG_A6);
            if (!emit_call_global(bc, &ip, cb, relocs, &sites, &cache_head, &cache_depth))
                goto fail;
            break;
        }

        case OP_GLOAD_CALL_GLOBAL: {
            /* u16 sym_idx (the special), then OP_CALL_GLOBAL's operands:
             * the OP_GLOAD helper (value in D0) pushed, then the call. */
            uint16_t idx;
            CL_Obj sym;
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_gload;
            if (ip + 1 >= bc->code_len) goto fail;
            idx = ((uint16_t)bc->code[ip] << 8) | bc->code[ip + 1];
            ip += 2;
            if (idx >= bc->n_constants || bc->constants == NULL) goto fail;
            sym = bc->constants[idx];
            if (!CL_SYMBOL_P(sym)) goto fail;
            cache_flush(cb, &cache_head, &cache_depth);
            emit_obj_imm_predec(cb, relocs, sym);
            m68k_emit_jsr_abs_l(cb, helper);
            m68k_emit_addq_l_an(cb, 4, REG_A7);
            cache_push_dn(cb, &cache_head, &cache_depth, REG_D0);
            if (!emit_call_global(bc, &ip, cb, relocs, &sites, &cache_head, &cache_depth))
                goto fail;
            break;
        }

        case OP_CALL_GLOBAL:
            if (!emit_call_global(bc, &ip, cb, relocs, &sites, &cache_head, &cache_depth))
                goto fail;
            break;

        case OP_TAILCALL_GLOBAL: {
            /* u16 sym_idx, u8 nargs — `FLOAD sym; TAILCALL n` fused.  Same
             * two paths as OP_TAILCALL: self-recursive TCO when the symbol
             * IS this function's name (decided at walk time — a call to any
             * other name can never be a self call — and confirmed at run
             * time by cl_jit_runtime_is_self_tco on the resolved binding, so
             * a redefinition still takes the generic path), else the
             * call-site fallback + UNLK/RTS.  The only
             * layout difference from OP_TAILCALL: no func slot under the
             * args, so the copy loop's sources are unchanged and the drop is
             * 4*N, not 4*(N+1). */
            uint16_t sym_idx;
            uint8_t nargs;
            CL_Obj sym;
            int self_tco;
            int32_t cell;
            int32_t guard_branch_pc = 0;
            if (ip + 2 >= bc->code_len) goto fail;
            sym_idx = ((uint16_t)bc->code[ip] << 8) | bc->code[ip + 1];
            ip += 2;
            nargs = bc->code[ip++];
            if (sym_idx >= bc->n_constants || bc->constants == NULL) goto fail;
            sym = bc->constants[sym_idx];
            if (!CL_SYMBOL_P(sym)) goto fail;

            cache_flush(cb, &cache_head, &cache_depth);

            self_tco = !is_kw && nargs >= arity &&
                       nargs <= arity + bc->n_optional &&
                       CL_SYMBOL_P(bc->name) && sym == bc->name;

            if (self_tco) {
                uint32_t self_obj = (uint32_t)CL_PTR_TO_OBJ(bc);
                uint32_t fload = (uint32_t)(uintptr_t)&cl_jit_runtime_fload;
                uint32_t guard = (uint32_t)(uintptr_t)&cl_jit_runtime_is_self_tco;
                int32_t entry_off, bra_disp;
                uint32_t i;

                /* D0 = current binding of the symbol (may signal
                 * UNDEFINED-FUNCTION exactly as the VM would). */
                emit_obj_imm_predec(cb, relocs, sym);
                m68k_emit_jsr_abs_l(cb, fload);
                m68k_emit_addq_l_an(cb, 4, REG_A7);
                /* cl_jit_runtime_is_self_tco(func, self_bc): push self_bc
                 * (2nd arg) then func (1st). */
                emit_obj_imm_predec(cb, relocs, self_obj);
                m68k_emit_move_l_dn_predec_an(cb, REG_D0, REG_A7);
                m68k_emit_jsr_abs_l(cb, guard);
                m68k_emit_addq_l_an(cb, 8, REG_A7);
                m68k_emit_tst_l_dn(cb, REG_D0);
                guard_branch_pc = (int32_t)cb_len(cb) + 2;
                m68k_emit_beq_w(cb, 0);   /* D0==0 → not self → fallback */

                /* Copy args into frame slots — see OP_TAILCALL. */
                if (is_opt) {
                    emit_opt_self_tco_copy(cb, nargs, slot_anchor, n_locals);
                } else for (i = 0; i < nargs; i++) {
                    int16_t src_disp = (int16_t)(4 * (int32_t)i);
                    int16_t dst_disp = slot_disp((uint8_t)(nargs - 1 - i),
                                                 slot_anchor, in_frame);
                    m68k_emit_move_l_disp_an_to_dn(cb, src_disp, REG_A7, REG_D0);
                    m68k_emit_move_l_dn_to_disp_am(cb, REG_D0, dst_disp, REG_A6);
                }
                m68k_emit_lea_disp_an_to_am(cb,
                    (int16_t)(frame_size - 12), REG_A6, REG_A7);
                emit_loop_poll(cb);   /* a loop, as in OP_TAILCALL */
                entry_off = bc_to_native[0];
                bra_disp = entry_off - ((int32_t)cb_len(cb) + 2);
                if (bra_disp < -32768 || bra_disp > 32767) goto fail;
                m68k_emit_bra_w(cb, (int16_t)bra_disp);

                m68k_patch_disp16(cb_data(cb), cb_len(cb),
                                  (uint32_t)guard_branch_pc,
                                  (int16_t)((int32_t)cb_len(cb) - guard_branch_pc));
            }

            /* Another function: as OP_TAILCALL, but no func slot. */
            cell = -1;
            if (tail_jumps && nargs <= arity)
                cell = emit_tail_call_site(cb, &sites, 1, nargs,
                                           saved_d7_disp, saved_d6_disp,
                                           saved_d5_disp);
            emit_call_site(cb, relocs, &sites, sym, 1, nargs, cell);

            m68k_emit_move_l_disp_an_to_dn(cb, saved_d7_disp, REG_A6, REG_D7);
            m68k_emit_move_l_disp_an_to_dn(cb, saved_d6_disp, REG_A6, REG_D6);
            m68k_emit_move_l_disp_an_to_dn(cb, saved_d5_disp, REG_A6, REG_D5);
            m68k_emit_unlk_an(cb, REG_A6);
            m68k_emit_rts(cb);
            break;
        }

        case OP_APPLY: {
            /* Operand stack on entry: TOS = arglist, second = func.
             * Pop both, flush, JSR cl_jit_runtime_apply(func, arglist),
             * push D0 result.  Helper allocates (calls cl_vm_apply
             * which may invoke user code), so cache_flush is mandatory
             * — any residual cached heap pointers must live on the
             * m68k stack for the conservative scan.
             *
             * The compiler emits OP_APPLY for `(apply fn …args… list)`
             * after compiling each arg + N-1 OP_CONS instructions to
             * build the full arglist.  Result becomes the new TOS so
             * downstream ops see the apply value directly. */
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_apply;
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D1);  /* arglist */
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D0);  /* func */
            emit_helper_call_2(cb, &cache_head, &cache_depth, helper);
            break;
        }

        case OP_CAR:
        case OP_CDR: {
            /* Pop obj into D0, push it as the helper arg, JSR, drop,
             * push D0 result back to cache.  cl_car / cl_cdr are
             * non-allocating on the happy path, but type-error throws
             * via cl_signal_type_error allocate a condition object —
             * flush before JSR so any cached heap pointers are on the
             * m68k stack and reachable to the conservative scan. */
            uint32_t helper = (op == OP_CAR)
                ? (uint32_t)(uintptr_t)&cl_jit_runtime_car
                : (uint32_t)(uintptr_t)&cl_jit_runtime_cdr;
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D0);
            emit_helper_call_1(cb, &cache_head, &cache_depth, helper, 1);
            break;
        }

        case OP_LIST: {
            /* u8 n.  Pop `n` values, cons them into a list (top-of-
             * stack ends up at the tail), push the list.
             *
             * Same dispatch shape as OP_AMIGA_CALL: flush the cache so
             * the n operand-stack values land on the m68k stack, capture
             * A7 in A0 (= operand_top), then push the two C args
             * right-to-left.  The helper iterates operand_top[0..n-1]
             * with operand_top[0] = TOS = last cons-into-list.  After
             * the JSR we drop the 2 C args (8 bytes) and the n operand
             * slots (4*n bytes).  GC: helper allocates n cons cells
             * and CL_GC_PROTECTs the partial list; the JIT-stack
             * forwarding pass rewrites operand_top slots in place. */
            uint8_t  n;
            int16_t  drop_operand;
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_list;
            if (ip >= bc->code_len) goto fail;
            n = bc->code[ip++];

            cache_flush(cb, &cache_head, &cache_depth);
            /* A0 = current SP = &operand_top[0] before any C-arg pushes. */
            m68k_emit_move_l_an_to_am(cb, REG_A7, REG_A0);
            /* Push operand_top, then n (right-to-left, so n lands at
             * 4(a7) after JSR pushes the return address). */
            m68k_emit_move_l_an_predec_am(cb, REG_A0, REG_A7);    /* operand_top */
            m68k_emit_move_l_imm32_predec(cb, (uint32_t)n, REG_A7);
            m68k_emit_jsr_abs_l(cb, helper);
            m68k_emit_addq_l_an(cb, 8, REG_A7);                   /* drop 2 C args */
            drop_operand = (int16_t)(4 * (int32_t)n);
            if (drop_operand > 0)
                m68k_emit_lea_disp_an_to_am(cb, drop_operand, REG_A7, REG_A7);
            cache_push_dn(cb, &cache_head, &cache_depth, REG_D0);
            break;
        }

        case OP_RPLACA: {
            /* Pop new_car (TOS), pop cons_obj, helper does the type-
             * check + write and returns new_car as the new TOS.
             *
             * Mirrors OP_STRUCT_SET's two-arg shape.  Flush before the
             * JSR because cl_error on type mismatch unwinds through
             * the JIT frame and the conservative scan only sees stack
             * (not cache regs).  cl_error itself is non-allocating in
             * the steady-state, but the unwind machinery may touch
             * the heap when building condition objects, so the same
             * "flush before helper that may signal" discipline as
             * OP_STRUCT_SET applies. */
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_rplaca;
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D1);  /* new_car */
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D0);  /* cons_obj */
            emit_helper_call_2(cb, &cache_head, &cache_depth, helper);
            break;
        }

        case OP_RPLACD: {
            /* Mirror of OP_RPLACA for the cdr slot.  Same cache-flush
             * + JSR shape, different helper. */
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_rplacd;
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D1);  /* new_cdr */
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D0);  /* cons_obj */
            emit_helper_call_2(cb, &cache_head, &cache_depth, helper);
            break;
        }

        case OP_ARGC: {
            /* The fixnum nargs, as the interpreter's frame->nargs: the
             * count every in-frame prologue keeps at -4(a6).  Only the
             * &optional prologue emits OP_ARGC, so a shape without a
             * frame cannot reach it.  Sets mv_count = 1 like the VM's
             * OP_ARGC. */
            if (!in_frame) goto fail;
            m68k_emit_move_l_disp_an_to_dn(cb, -4, REG_A6, REG_D0);
            m68k_emit_add_l_dn_to_dm(cb, REG_D0, REG_D0);
            m68k_emit_addq_l_dn(cb, 1, REG_D0);          /* CL_TAG_FIXNUM */
            m68k_emit_moveq(cb, 1, REG_D1);
            m68k_emit_move_l_dn_to_disp_am(cb, REG_D1, JIT_MV_COUNT_DISP,
                                           REG_A3);
            cache_push_dn(cb, &cache_head, &cache_depth, REG_D0);
            break;
        }

        case OP_MV_LOAD: {
            /* u8 index.  Single-arg JSR.  Helper is non-allocating
             * (reads cl_mv_values[idx]) so no cache_flush. */
            uint8_t index;
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_mv_load;
            if (ip >= bc->code_len) goto fail;
            index = bc->code[ip++];
            m68k_emit_move_l_imm32_predec(cb, (uint32_t)index, REG_A7);
            m68k_emit_jsr_abs_l(cb, helper);
            m68k_emit_addq_l_an(cb, 4, REG_A7);
            cache_push_dn(cb, &cache_head, &cache_depth, REG_D0);
            break;
        }

        case OP_NTH_VALUE: {
            /* Stack layout: TOS = primary, second = idx.  Helper
             * signature is (idx_obj, primary), so push primary first
             * (high address) then idx_obj (low address = first C arg).
             * Helper may cl_error on non-fixnum index, which can
             * allocate a condition object — cache_flush before JSR. */
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_nth_value;
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D1);  /* primary */
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D0);  /* idx_obj */
            emit_helper_call_2(cb, &cache_head, &cache_depth, helper);
            break;
        }

        case OP_ASSERT_TYPE: {
            /* u16 const idx for the type-spec.  Peek-only: TOS stays
             * the value being checked.  Helper takes (val, type_spec);
             * val sits at the (cached) operand-stack TOS, so flush
             * first then push from (a7) and the baked-in type_spec
             * literal.  Helper allocates the condition object on
             * mismatch — cache_flush mandatory. */
            uint16_t idx;
            CL_Obj type_spec;
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_assert_type;
            if (ip + 1 >= bc->code_len) goto fail;
            idx = ((uint16_t)bc->code[ip] << 8) | bc->code[ip + 1];
            ip += 2;
            if (idx >= bc->n_constants || bc->constants == NULL) goto fail;
            type_spec = bc->constants[idx];

            cache_flush(cb, &cache_head, &cache_depth);
            /* After flush: (a7) = val (operand TOS).  Push type_spec
             * first so it lands at 8(a7) post-JSR, then dup val from
             * 4(a7) to (a7) so it lands at 4(a7) post-JSR.  Helper sig
             * is (val, type_spec). */
            emit_obj_imm_predec(cb, relocs, type_spec);
            m68k_emit_move_l_disp_an_predec_am(cb, 4, REG_A7, REG_A7); /* dup val */
            m68k_emit_jsr_abs_l(cb, helper);
            m68k_emit_addq_l_an(cb, 8, REG_A7);
            break;
        }

        case OP_HANDLER_PUSH: {
            /* u16 const idx → type-symbol literal baked into the JSR
             * call site.  Operand stack: TOS = handler closure (popped).
             * Helper signature is (type_sym, handler) — C-ABI args,
             * type_sym at (a7), handler at 4(a7) after JSR.  Helper is
             * non-allocating on the normal path but calls cl_error on
             * stack overflow (allocates a condition), so cache_flush
             * before the JSR keeps any cached operand-stack values
             * reachable through the conservative m68k-stack scan. */
            uint16_t idx;
            CL_Obj type_sym;
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_handler_push;
            if (ip + 1 >= bc->code_len) goto fail;
            idx = ((uint16_t)bc->code[ip] << 8) | bc->code[ip + 1];
            ip += 2;
            if (idx >= bc->n_constants || bc->constants == NULL) goto fail;
            type_sym = bc->constants[idx];

            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D1);    /* handler */
            cache_flush(cb, &cache_head, &cache_depth);
            m68k_emit_move_l_dn_predec_an(cb, REG_D1, REG_A7);          /* push handler */
            emit_obj_imm_predec(cb, relocs, type_sym); /* push type_sym */
            m68k_emit_jsr_abs_l(cb, helper);
            m68k_emit_addq_l_an(cb, 8, REG_A7);
            break;
        }

        case OP_HANDLER_POP: {
            /* u8 count.  Single-arg JSR; helper just decrements
             * cl_handler_top.  Non-allocating, so no cache_flush —
             * mirrors OP_ARGC's reasoning (helper only touches caller-
             * saved D0/D1; cache regs D5/D6/D7 are callee-saved). */
            uint8_t count;
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_handler_pop;
            if (ip >= bc->code_len) goto fail;
            count = bc->code[ip++];
            m68k_emit_move_l_imm32_predec(cb, (uint32_t)count, REG_A7);
            m68k_emit_jsr_abs_l(cb, helper);
            m68k_emit_addq_l_an(cb, 4, REG_A7);
            break;
        }

        case OP_HANDLER_CASE_PUSH: {
            /* u16 types_idx, i32 offset.  Same inline-setjmp shape as
             * OP_BLOCK_PUSH (see that comment for the sequence and the
             * rationale), with a different landing arm: the VM resumes a
             * matched clause at landing + 5 * k — the k-th OP_JMP of the
             * per-clause table compile_handler_case emits — so after the
             * post_longjmp helper has pushed the condition, the arm reads
             * the clause index and dispatches:
             *
             *   JSR post_longjmp        -- D0 = condition
             *   MOVE.L D0,-(A7)         -- the clause's sole value
             *   JSR clause              -- D0 = k
             *   MOVEQ #0,D1; CMP.L D1,D0; BEQ.W entry_0
             *   ...                     -- one pair per clause but the last
             *   BRA.W entry_{n-1}
             *   normal_path:
             *   JSR commit              -- frame + clause bindings
             *
             * The prescan marked every table entry as a branch target, so
             * both arms meet the walker at depth 0 with the condition at
             * (a7).  Every entry sits after the form (compile_handler_case
             * emits push, form, pop, jmp, table), so all of them are
             * forward patches. */
            uint16_t types_idx;
            CL_Obj types;
            int32_t bc_offset;
            uint32_t landing_bc_ip, n, k;
            int32_t beq_pc, normal_path_off;
            uint32_t alloc_helper        = (uint32_t)(uintptr_t)&cl_jit_runtime_handler_case_alloc;
            uint32_t commit_helper       = (uint32_t)(uintptr_t)&cl_jit_runtime_handler_case_commit;
            uint32_t post_longjmp_helper = (uint32_t)(uintptr_t)&cl_jit_runtime_handler_case_post_longjmp;
            uint32_t clause_helper       = (uint32_t)(uintptr_t)&cl_jit_runtime_handler_case_clause;

            if (ip + 6 > bc->code_len) goto fail;
            types_idx = ((uint16_t)bc->code[ip] << 8) | bc->code[ip + 1];
            ip += 2;
            bc_offset = read_i32_be(bc->code + ip);
            ip += 4;
            if (types_idx >= bc->n_constants || bc->constants == NULL) goto fail;
            types = bc->constants[types_idx];
            n = 0;
            { CL_Obj t = types; while (CL_CONS_P(t)) { n++; t = cl_cdr(t); } }
            if (n == 0 || n > 127) goto fail;
            {
                int ok;
                landing_bc_ip = compute_landing_ip(bc_offset, ip,
                                                   bc->code_len, &ok);
                if (!ok) goto fail;
            }
            if (landing_bc_ip + 5 * n > bc->code_len) goto fail;

            cache_flush(cb, &cache_head, &cache_depth);

            /* alloc(types) → D0 = &nlx->buf.  The helper may cl_error on
             * NLX / handler stack overflow — the flush above keeps every
             * cached operand reachable through the conservative scan. */
            emit_obj_imm_predec(cb, relocs, types);
            m68k_emit_jsr_abs_l(cb, alloc_helper);
            m68k_emit_addq_l_an(cb, 4, REG_A7);

            /* setjmp(D0): push D0, JSR setjmp, drop arg; Z on the zero
             * (normal) return. */
            m68k_emit_move_l_dn_predec_an(cb, REG_D0, REG_A7);
            m68k_emit_jsr_abs_l(cb, cl_jit_setjmp_addr);
            m68k_emit_addq_l_an(cb, 4, REG_A7);
            m68k_emit_tst_l_dn(cb, REG_D0);
            beq_pc = (int32_t)cb_len(cb) + 2;
            m68k_emit_beq_w(cb, 0);   /* → normal_path, patched below */

            /* Longjmp arm: condition onto the operand stack, then the
             * clause dispatch. */
            m68k_emit_jsr_abs_l(cb, post_longjmp_helper);
            m68k_emit_move_l_dn_predec_an(cb, REG_D0, REG_A7);
            m68k_emit_jsr_abs_l(cb, clause_helper);
            for (k = 0; k + 1 < n; k++) {
                int32_t bcc_pc;
                m68k_emit_moveq(cb, (int8_t)k, REG_D1);
                m68k_emit_cmp_l_dn_dm(cb, REG_D1, REG_D0);
                bcc_pc = (int32_t)cb_len(cb) + 2;
                if (!patches_push(&patches, &n_patches, &cap_patches,
                                  (uint32_t)bcc_pc, landing_bc_ip + 5 * k))
                    goto fail;
                m68k_emit_beq_w(cb, 0);
            }
            {
                int32_t bra_pc = (int32_t)cb_len(cb) + 2;
                if (!patches_push(&patches, &n_patches, &cap_patches,
                                  (uint32_t)bra_pc, landing_bc_ip + 5 * (n - 1)))
                    goto fail;
                m68k_emit_bra_w(cb, 0);
            }

            /* normal_path: patch the BEQ to land here, then commit. */
            normal_path_off = (int32_t)cb_len(cb);
            m68k_patch_disp16(cb_data(cb), cb_len(cb), (uint32_t)beq_pc,
                              (int16_t)(normal_path_off - beq_pc));
            m68k_emit_jsr_abs_l(cb, commit_helper);
            /* Cache depth stays 0; the protected form executes from here. */
            break;
        }

        case OP_HANDLER_CASE_POP: {
            /* Mirror of OP_BLOCK_POP: the helper searches backward for
             * the frame, drops its clause bindings and the frame.  The
             * operand stack is untouched (the form's value stays), it is
             * non-allocating, and D5/D6/D7 survive the JSR — no flush. */
            uint32_t pop_helper = (uint32_t)(uintptr_t)&cl_jit_runtime_handler_case_pop;
            m68k_emit_jsr_abs_l(cb, pop_helper);
            break;
        }

        case OP_RESTART_PUSH: {
            /* u16 const idx → name-symbol literal baked in.  Operand
             * stack on entry (compiler pushes handler, report,
             * interactive, test, tag), so after a full flush:
             *   (a7)=tag 4=test 8=interactive 12=report 16=handler.
             * Six-arg helper (name, handler, report, interactive, test,
             * tag): capture A7 in A0 (à la OP_ASET) and push from
             * disp(a0) so the C args land at (a7)..20(a7) after JSR.
             * The helper builds the first-class restart object (allocates)
             * and may cl_error on overflow — both reachable via the
             * conservative m68k-stack scan after the flush. */
            uint16_t idx;
            CL_Obj name_sym;
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_restart_push;
            if (ip + 1 >= bc->code_len) goto fail;
            idx = ((uint16_t)bc->code[ip] << 8) | bc->code[ip + 1];
            ip += 2;
            if (idx >= bc->n_constants || bc->constants == NULL) goto fail;
            name_sym = bc->constants[idx];

            cache_flush(cb, &cache_head, &cache_depth);
            m68k_emit_move_l_an_to_am(cb, REG_A7, REG_A0);            /* A0 = operand_top */
            m68k_emit_move_l_disp_an_predec_am(cb, 0, REG_A0, REG_A7);  /* push tag */
            m68k_emit_move_l_disp_an_predec_am(cb, 4, REG_A0, REG_A7);  /* push test */
            m68k_emit_move_l_disp_an_predec_am(cb, 8, REG_A0, REG_A7);  /* push interactive */
            m68k_emit_move_l_disp_an_predec_am(cb, 12, REG_A0, REG_A7); /* push report */
            m68k_emit_move_l_disp_an_predec_am(cb, 16, REG_A0, REG_A7); /* push handler */
            emit_obj_imm_predec(cb, relocs, name_sym); /* push name */
            m68k_emit_jsr_abs_l(cb, helper);
            /* Drop 24 C-arg bytes + 20 operand-slot bytes = 44 bytes. */
            m68k_emit_lea_disp_an_to_am(cb, 44, REG_A7, REG_A7);
            break;
        }

        case OP_RESTART_POP: {
            /* u8 count.  Same shape as OP_HANDLER_POP. */
            uint8_t count;
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_restart_pop;
            if (ip >= bc->code_len) goto fail;
            count = bc->code[ip++];
            m68k_emit_move_l_imm32_predec(cb, (uint32_t)count, REG_A7);
            m68k_emit_jsr_abs_l(cb, helper);
            m68k_emit_addq_l_an(cb, 4, REG_A7);
            break;
        }

        case OP_ASET: {
            /* Three-arg helper: helper(vec_obj, idx_obj, val).  After
             * the cache flush the three operand-stack values sit at
             * (a7) = val (TOS), 4(a7) = idx, 8(a7) = vec.  Capture A7
             * in A0, then push from disp(a0) right-to-left so the C
             * args end up at 4..12(a7) after JSR.
             *
             * Using disp-from-A0 (rather than popping into 3 data regs
             * and re-pushing) avoids spending an extra scratch reg —
             * the cache only uses D5/D6/D7, so D0/D1 alone suffice for
             * the cache_pop_to_dn cases above, and here we don't need
             * any data reg for the values at all. */
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_aset;
            cache_flush(cb, &cache_head, &cache_depth);
            m68k_emit_move_l_an_to_am(cb, REG_A7, REG_A0);            /* A0 = operand_top */
            m68k_emit_move_l_disp_an_predec_am(cb, 0, REG_A0, REG_A7); /* push val */
            m68k_emit_move_l_disp_an_predec_am(cb, 4, REG_A0, REG_A7); /* push idx */
            m68k_emit_move_l_disp_an_predec_am(cb, 8, REG_A0, REG_A7); /* push vec */
            m68k_emit_jsr_abs_l(cb, helper);
            /* Drop 12 C args + 12 operand slots = 24 bytes. */
            m68k_emit_lea_disp_an_to_am(cb, 24, REG_A7, REG_A7);
            cache_push_dn(cb, &cache_head, &cache_depth, REG_D0);
            break;
        }

        case OP_CONS: {
            /* The first allocating opcode the walker handles directly.
             * Bytecode shape: pop cdr (TOS), pop car, push cl_cons(car,
             * cdr).  Helper takes args in C order — car at (a7), cdr at
             * 4(a7) — so after popping cdr→D1 and car→D0 we predec
             * cdr first, then car, leaving car at the lowest stack
             * address as the JSR's first argument.
             *
             * GC: cl_cons allocates and may GC.  The conservative scan
             * (mem.c::gc_scan_jit_native_stack) reaches values on the
             * m68k stack between current SP and the JIT-entry SP, but
             * not values held in cache regs D5/D6/D7.  We therefore
             * flush the cache before the JSR so any residual cached
             * heap pointers land on the m68k stack and are seen by the
             * scan with offset validation.  After the helper returns,
             * the result (a fresh CL_Cons) goes back into the cache via
             * cache_push_dn; D5/D6/D7 are callee-saved across the JSR
             * by the m68k C ABI so the previously-cached values (now
             * also living on the stack from the flush) remain valid
             * uncached operand-stack entries beneath the new TOS. */
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_cons;
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D1);  /* cdr */
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D0);  /* car */
            emit_helper_call_2(cb, &cache_head, &cache_depth, helper);
            break;
        }

        /* Compare ops: fixnum fast path inlines BTST + CMP + Bcc and
         * doesn't allocate, so no pre-flush.  The slow-path JSR can
         * allocate (type-error condition object, cross-type
         * bignum/ratio intern) — emit_compare_compute spills/reloads
         * inside its slow-path shim so cached heap pointers survive
         * GC via the conservatively-scanned stack. */
        case OP_LT:
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D1);
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D0);
            emit_compare_compute(cb, /*BLT*/13,
                                 (uint32_t)(uintptr_t)&cl_jit_runtime_lt,
                                 cache_head, cache_depth, relocs);
            cache_push_dn(cb, &cache_head, &cache_depth, REG_D0);
            break;

        case OP_GT:
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D1);
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D0);
            emit_compare_compute(cb, /*BGT*/14,
                                 (uint32_t)(uintptr_t)&cl_jit_runtime_gt,
                                 cache_head, cache_depth, relocs);
            cache_push_dn(cb, &cache_head, &cache_depth, REG_D0);
            break;

        case OP_LE:
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D1);
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D0);
            emit_compare_compute(cb, /*BLE*/15,
                                 (uint32_t)(uintptr_t)&cl_jit_runtime_le,
                                 cache_head, cache_depth, relocs);
            cache_push_dn(cb, &cache_head, &cache_depth, REG_D0);
            break;

        case OP_GE:
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D1);
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D0);
            emit_compare_compute(cb, /*BGE*/12,
                                 (uint32_t)(uintptr_t)&cl_jit_runtime_ge,
                                 cache_head, cache_depth, relocs);
            cache_push_dn(cb, &cache_head, &cache_depth, REG_D0);
            break;

        case OP_NUMEQ:
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D1);
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D0);
            emit_compare_compute(cb, /*BEQ*/7,
                                 (uint32_t)(uintptr_t)&cl_jit_runtime_numeq,
                                 cache_head, cache_depth, relocs);
            cache_push_dn(cb, &cache_head, &cache_depth, REG_D0);
            break;

        case OP_EQ: {
            /* Pure pointer compare — no helper.  Pop b/a, CMP, set
             * D0 to CL_T or CL_NIL based on Z, push D0 back. */
            int32_t beq_pc, bra_pc;
            int32_t load_t_off, done_off;

            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D1);
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D0);
            m68k_emit_cmp_l_dn_dm(cb, REG_D1, REG_D0);
            beq_pc = (int32_t)cb_len(cb) + 2;
            m68k_emit_beq_w(cb, 0);
            m68k_emit_moveq(cb, 0, REG_D0);              /* CL_NIL */
            bra_pc = (int32_t)cb_len(cb) + 2;
            m68k_emit_bra_w(cb, 0);

            load_t_off = (int32_t)cb_len(cb);
            emit_obj_imm_d0(cb, relocs, CL_T);
            done_off = (int32_t)cb_len(cb);

            m68k_patch_disp16(cb_data(cb), cb_len(cb), (uint32_t)beq_pc,
                              (int16_t)(load_t_off - beq_pc));
            m68k_patch_disp16(cb_data(cb), cb_len(cb), (uint32_t)bra_pc,
                              (int16_t)(done_off - bra_pc));
            cache_push_dn(cb, &cache_head, &cache_depth, REG_D0);
            break;
        }

        case OP_NOT: {
            /* Pop value, test, push CL_T iff zero (NIL) else CL_NIL.
             * cache_pop_to_dn into D0 sets D0; an explicit TST.L
             * captures the Z flag (since cache_pop may not have used
             * a flag-setting move). */
            int32_t beq_pc, bra_pc;
            int32_t load_t_off, done_off;

            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D0);
            m68k_emit_tst_l_dn(cb, REG_D0);
            beq_pc = (int32_t)cb_len(cb) + 2;
            m68k_emit_beq_w(cb, 0);
            m68k_emit_moveq(cb, 0, REG_D0);              /* CL_NIL */
            bra_pc = (int32_t)cb_len(cb) + 2;
            m68k_emit_bra_w(cb, 0);

            load_t_off = (int32_t)cb_len(cb);
            emit_obj_imm_d0(cb, relocs, CL_T);
            done_off = (int32_t)cb_len(cb);

            m68k_patch_disp16(cb_data(cb), cb_len(cb), (uint32_t)beq_pc,
                              (int16_t)(load_t_off - beq_pc));
            m68k_patch_disp16(cb_data(cb), cb_len(cb), (uint32_t)bra_pc,
                              (int16_t)(done_off - bra_pc));
            cache_push_dn(cb, &cache_head, &cache_depth, REG_D0);
            break;
        }

        case OP_LOAD_STRUCT_REF: {
            /* u8 slot, then OP_STRUCT_REF's u8 index: push the local and
             * fall through. */
            uint8_t slot;
            if (ip >= bc->code_len) goto fail;
            slot = bc->code[ip++];
            if (slot >= n_locals) goto fail;
            cache_push_disp_an(cb, &cache_head, &cache_depth,
                               slot_disp(slot, slot_anchor, in_frame), REG_A6);
        }
        /* FALLTHROUGH */
        case OP_STRUCT_REF: {
            uint8_t idx;
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_struct_ref;
            if (ip >= bc->code_len) goto fail;
            idx = bc->code[ip++];

            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D0);
            cache_flush(cb, &cache_head, &cache_depth);
            m68k_emit_move_l_imm32_predec(cb, (uint32_t)idx, REG_A7);
            m68k_emit_move_l_dn_predec_an(cb, REG_D0, REG_A7);
            m68k_emit_jsr_abs_l(cb, helper);
            m68k_emit_addq_l_an(cb, 8, REG_A7);
            cache_push_dn(cb, &cache_head, &cache_depth, REG_D0);
            break;
        }

        case OP_STRUCT_SET: {
            uint8_t idx;
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_struct_set;
            if (ip >= bc->code_len) goto fail;
            idx = bc->code[ip++];

            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D1);   /* val */
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D0);   /* obj */
            cache_flush(cb, &cache_head, &cache_depth);
            m68k_emit_move_l_dn_predec_an(cb, REG_D1, REG_A7);
            m68k_emit_move_l_imm32_predec(cb, (uint32_t)idx, REG_A7);
            m68k_emit_move_l_dn_predec_an(cb, REG_D0, REG_A7);
            m68k_emit_jsr_abs_l(cb, helper);
            m68k_emit_addq_l_an(cb, 8, REG_A7);
            m68k_emit_addq_l_an(cb, 4, REG_A7);
            cache_push_dn(cb, &cache_head, &cache_depth, REG_D0);
            break;
        }

        case OP_MAKE_CELL: {
            /* Pop value, alloc a CL_Cell wrapping it, push the cell.
             * cl_make_cell allocates → flush cache so cached operand-
             * stack values land on the m68k stack for the conservative
             * GC scan.  Helper is one C arg: pushed via predec; addq.l
             * 4 drops it after the JSR returns. */
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_make_cell;
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D0);
            emit_helper_call_1(cb, &cache_head, &cache_depth, helper, 1);
            break;
        }

        case OP_CELL_REF: {
            /* Pop cell, push cell->value.  Helper is non-allocating
             * (plain deref), but it dereferences an arena-relative
             * CL_Obj — needs CL_OBJ_TO_PTR which uses cl_heap.arena_base.
             * Routing through the helper keeps the addressing in C
             * where the base is in scope and matches the VM exactly.
             * Non-allocating → emit_helper_call_1 with flush=0. */
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_cell_ref;
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D0);
            emit_helper_call_1(cb, &cache_head, &cache_depth, helper, 0);
            break;
        }

        case OP_CELL_SET_LOCAL: {
            /* u8 slot.  cell_obj = frame[slot], val = TOS (peek — TOS is
             * NOT popped).  Helper does cell->value = val and returns
             * val.  Non-allocating, so no need to flush the cache; we
             * route val through D1 (or load from (a7) if cache empty)
             * and route cell_obj from the frame slot through D0.  After
             * the JSR, TOS is unchanged.  Mirrors VM's OP_CELL_SET_LOCAL. */
            uint8_t slot;
            int16_t disp;
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_cell_set;
            if (ip >= bc->code_len) goto fail;
            slot = bc->code[ip++];
            if (slot >= n_locals) goto fail;
            disp = slot_disp(slot, slot_anchor, in_frame);

            /* Read TOS without popping into D1. */
            if (cache_depth >= 1) {
                m68k_emit_move_l_dn_to_dm(cb, (M68kReg)cache_head, REG_D1);
            } else {
                m68k_emit_move_l_disp_an_to_dn(cb, 0, REG_A7, REG_D1);
            }
            /* Read cell_obj from frame[slot] into D0. */
            m68k_emit_move_l_disp_an_to_dn(cb, disp, REG_A6, REG_D0);

            /* Push C args right-to-left: val (D1), then cell_obj (D0). */
            m68k_emit_move_l_dn_predec_an(cb, REG_D1, REG_A7);
            m68k_emit_move_l_dn_predec_an(cb, REG_D0, REG_A7);
            m68k_emit_jsr_abs_l(cb, helper);
            m68k_emit_addq_l_an(cb, 8, REG_A7);
            /* Result in D0 is the stored value, but we already left TOS
             * intact (peek semantics) — discard D0. */
            break;
        }

        case OP_UPVAL: {
            /* u8 index.  Push cl->upvalues[index] for whatever closure
             * the JIT frame was entered with.  func_obj lives at
             * 8(a6) as the first C arg.  Helper handles the non-closure
             * fall-through (returns CL_NIL — matches VM).  Non-
             * allocating, so we don't need cache_flush. */
            uint8_t index;
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_upval_ref;
            if (ip >= bc->code_len) goto fail;
            index = bc->code[ip++];
            /* Push C args R→L: index (imm32), func_obj (from 8(a6)). */
            m68k_emit_move_l_imm32_predec(cb, (uint32_t)index, REG_A7);
            m68k_emit_move_l_disp_an_predec_am(cb, 8, REG_A6, REG_A7);
            m68k_emit_jsr_abs_l(cb, helper);
            m68k_emit_addq_l_an(cb, 8, REG_A7);
            cache_push_dn(cb, &cache_head, &cache_depth, REG_D0);
            break;
        }

        case OP_CELL_SET_UPVAL: {
            /* u8 index.  cell_obj = func_obj->upvalues[index], val = TOS
             * (peek — TOS is NOT popped).  Helper does cell->value = val.
             * Helper may cl_error on type-mismatch (non-cell upvalue),
             * but no allocation happens in the success path; cache stays
             * intact so OP_CELL_SET_UPVAL plays nicely as an inline
             * SETF for a closed-over local.  Mirrors OP_CELL_SET_LOCAL's
             * shape but sources cell_obj from a closure dereference
             * instead of a frame slot. */
            uint8_t index;
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_cell_set_upval;
            if (ip >= bc->code_len) goto fail;
            index = bc->code[ip++];

            /* Read TOS without popping into D1. */
            if (cache_depth >= 1) {
                m68k_emit_move_l_dn_to_dm(cb, (M68kReg)cache_head, REG_D1);
            } else {
                m68k_emit_move_l_disp_an_to_dn(cb, 0, REG_A7, REG_D1);
            }

            /* Push C args R→L: val (D1), index (imm32), func_obj (from 8(a6)). */
            m68k_emit_move_l_dn_predec_an(cb, REG_D1, REG_A7);
            m68k_emit_move_l_imm32_predec(cb, (uint32_t)index, REG_A7);
            m68k_emit_move_l_disp_an_predec_am(cb, 8, REG_A6, REG_A7);
            m68k_emit_jsr_abs_l(cb, helper);
            /* Drop 3 C args (12 bytes).  addq.l only encodes 1..8, so
             * use LEA — same trap as the OP_CLOSURE epilogue. */
            m68k_emit_lea_disp_an_to_am(cb, 12, REG_A7, REG_A7);
            /* D0 result is the stored value; peek semantics leaves TOS
             * intact, so discard. */
            break;
        }

        case OP_CLOSURE: {
            /* u16 const_idx + n_upvals * (u8 is_local, u8 cap_idx).
             * Prescan has already validated: idx in range, constant is a
             * BYTECODE, every capture is_local=1 with cap_idx < n_locals.
             * Walker reads the descriptor stream, emits per-capture
             * loads onto the m68k stack to build values[], snapshots A7
             * as &values[0], then JSRs the helper.
             *
             * Stack growth order: predec pushes from values[n-1] down to
             * values[0], so the final A7 = &values[0].  Then we push the
             * 3 C args (right-to-left: values, n_upvals, tmpl_obj),
             * leaving tmpl_obj at (a7) on JSR entry.  Drop 12 bytes of
             * C args, then n_upvals*4 bytes of values, then push D0
             * result onto the operand cache.
             *
             * GC: cl_alloc may compact only when no JIT frame is active —
             * we're inside one, so mark-and-sweep keeps tmpl_obj and the
             * values[] entries reachable via the m68k stack (values[]
             * lives on it; tmpl_obj as a fresh imm32 push and again as
             * the JSR's 1st C arg both occupy stack slots for the scan).
             * The helper's return value (a fresh CL_Closure) drops into
             * D0 (callee-saved D5/D6/D7 are intact across the JSR), then
             * cache_push_dn promotes it back to the operand stack. */
            uint16_t idx;
            CL_Obj tmpl_obj;
            CL_Bytecode *tmpl;
            uint32_t n_upvals_local;
            uint32_t i;
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_make_closure;
            if (ip + 1 >= bc->code_len) goto fail;
            idx = ((uint16_t)bc->code[ip] << 8) | bc->code[ip + 1];
            ip += 2;
            if (idx >= bc->n_constants || bc->constants == NULL) goto fail;
            tmpl_obj = bc->constants[idx];
            if (!CL_BYTECODE_P(tmpl_obj)) goto fail;
            tmpl = (CL_Bytecode *)CL_OBJ_TO_PTR(tmpl_obj);
            n_upvals_local = tmpl->n_upvalues;
            if (ip + 2 * n_upvals_local > bc->code_len) goto fail;
            /* Gate n_upvalues like the n_extra>1000 gate: the LEA stack
             * drop below is a 16-bit displacement (12 + 4*n), which wraps
             * negative past ~8188 upvalues — unreachable from the real
             * compiler (uint8_t capture indices), but a corrupt FASL sets
             * the field freely and would get silent stack corruption. */
            if (n_upvals_local > 1000) goto fail;

            cache_flush(cb, &cache_head, &cache_depth);

            /* Push captures in reverse so A7 ends pointing at values[0].
             * Two descriptor forms:
             *   is_local=1 — load from this frame's local slot via
             *                slot_disp (positional arg or LINK-frame
             *                local, depending on slot vs slot_anchor).
             *   is_local=0 — load from this function's own closure's
             *                upvalue at cap_idx, by calling
             *                cl_jit_runtime_upval_ref(func_obj=8(a6),
             *                cap_idx).  Helper is non-allocating; D0
             *                receives the upvalue, then we predec onto
             *                the values[] stack like the local case. */
            for (i = n_upvals_local; i > 0; i--) {
                uint8_t is_local = bc->code[ip + 2*(i-1)];
                uint8_t cap_idx  = bc->code[ip + 2*(i-1) + 1];
                if (is_local) {
                    int16_t cap_disp;
                    if (cap_idx >= n_locals) goto fail;
                    cap_disp = slot_disp(cap_idx, slot_anchor, in_frame);
                    m68k_emit_move_l_disp_an_to_dn(cb, cap_disp, REG_A6, REG_D0);
                    m68k_emit_move_l_dn_predec_an(cb, REG_D0, REG_A7);
                } else {
                    uint32_t up_helper =
                        (uint32_t)(uintptr_t)&cl_jit_runtime_upval_ref;
                    if (cap_idx >= bc->n_upvalues) goto fail;
                    m68k_emit_move_l_imm32_predec(cb, (uint32_t)cap_idx, REG_A7);
                    m68k_emit_move_l_disp_an_predec_am(cb, 8, REG_A6, REG_A7);
                    m68k_emit_jsr_abs_l(cb, up_helper);
                    m68k_emit_addq_l_an(cb, 8, REG_A7);
                    m68k_emit_move_l_dn_predec_an(cb, REG_D0, REG_A7);
                }
            }
            ip += 2 * n_upvals_local;

            /* Snapshot &values[0] into A0, then push 3 C args R→L. */
            m68k_emit_move_l_an_to_am(cb, REG_A7, REG_A0);
            m68k_emit_move_l_an_predec_am(cb, REG_A0, REG_A7);            /* values */
            m68k_emit_move_l_imm32_predec(cb, n_upvals_local, REG_A7);    /* n_upvals */
            emit_obj_imm_predec(cb, relocs, tmpl_obj);                    /* tmpl_obj */
            m68k_emit_jsr_abs_l(cb, helper);
            /* Drop 3 C args + n_upvals*4 bytes of values via one LEA.
             * Can't use addq.l for the 12-byte drop alone — addq.l only
             * encodes immediates 1..8 (12 silently truncates to 4 in the
             * encoder).  Folding into the same LEA also saves an
             * instruction. */
            {
                int16_t drop = (int16_t)(12 + 4 * (int32_t)n_upvals_local);
                m68k_emit_lea_disp_an_to_am(cb, drop, REG_A7, REG_A7);
            }
            cache_push_dn(cb, &cache_head, &cache_depth, REG_D0);
            break;
        }

        case OP_AMIGA_CALL: {
            /* OP_AMIGA_CALL layout: u16 sym_idx, i16 offset,
             * i32 regspec, u8 n_args (10 bytes total incl. opcode).
             *
             * Args are on the m68k operand stack at [A7..A7+4*(n-1)],
             * with argN-1 at A7 (the most recent push) — same layout
             * OP_CALL expects.  After flushing the cache to memory we
             * grab the operand-top into A0, then push the 5 C-ABI args
             * right-to-left: operand_top, n_args, regspec, offset
             * (sign-extended from i16), base_sym (CL_Obj literal baked
             * from constants[idx]).  JSR helper, drop the 5 C args,
             * drop the n_args operand slots, push D0 result.
             *
             * GC: any cached operand values are spilled by
             * cache_flush, so the conservative scan reaches them
             * across the helper if it allocates (bignum box for
             * over-fixnum return values, or cl_error condition
             * objects on unbound/wrong-type/arity errors).
             *
             * Without this walker case every defcfun-generated
             * wrapper (move-to, draw-to, rect-fill, ...) compiled
             * around a single OP_AMIGA_CALL bails the walker and runs
             * via the bytecode interpreter, forcing a native↔bytecode
             * bounce per FFI call from JIT'd hot loops. */
            uint16_t sym_idx;
            int16_t  offset;
            int32_t  offset_se;
            uint32_t regspec;
            uint8_t  n_args;
            int16_t  drop_bytes;
            CL_Obj   base_sym;
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_amiga_call;

            if (ip + 9 > bc->code_len) goto fail;
            sym_idx = ((uint16_t)bc->code[ip] << 8) | bc->code[ip + 1];
            offset  = (int16_t)(((uint16_t)bc->code[ip + 2] << 8) |
                                bc->code[ip + 3]);
            regspec = read_i32_be(bc->code + ip + 4);
            n_args  = bc->code[ip + 8];
            ip += 9;

            if (sym_idx >= bc->n_constants || bc->constants == NULL)
                goto fail;
            base_sym = bc->constants[sym_idx];
            if (!CL_SYMBOL_P(base_sym)) goto fail;
            if (n_args > 7) goto fail;

            offset_se = (int32_t)offset;
            drop_bytes = (int16_t)(4 * (int32_t)n_args);

            cache_flush(cb, &cache_head, &cache_depth);
            /* A0 = current SP = &arg[n_args-1] before any C-arg pushes. */
            m68k_emit_move_l_an_to_am(cb, REG_A7, REG_A0);
            /* Push C args right-to-left so they end up in argN..arg0
             * order at 4..20(A7) after JSR (which itself pushes the
             * return address at 0(A7)). */
            m68k_emit_move_l_an_predec_am(cb, REG_A0, REG_A7);   /* operand_top */
            m68k_emit_move_l_imm32_predec(cb, (uint32_t)n_args, REG_A7);
            m68k_emit_move_l_imm32_predec(cb, regspec, REG_A7);
            m68k_emit_move_l_imm32_predec(cb, (uint32_t)offset_se, REG_A7);
            emit_obj_imm_predec(cb, relocs, base_sym);
            m68k_emit_jsr_abs_l(cb, helper);
            /* Drop the 5 C args (20 bytes), then the n_args operand
             * slots that the dispatch consumed. */
            m68k_emit_lea_disp_an_to_am(cb, 20, REG_A7, REG_A7);
            if (drop_bytes > 0)
                m68k_emit_lea_disp_an_to_am(cb, drop_bytes, REG_A7, REG_A7);
            cache_push_dn(cb, &cache_head, &cache_depth, REG_D0);
            break;
        }

        case OP_JMP:
        case OP_JNIL:
        case OP_JTRUE: {
            /* All branches flush the cache before testing/jumping so
             * the target (which the per-IP flush above also reaches
             * with depth=0) sees a consistent state.  For JNIL/JTRUE
             * we pop the TOS into D0 — when the cache had it, that's
             * a register move + decrement (no spill needed); MOVE.L
             * sets Z based on the source value, so BEQ/BNE branches
             * on whether the popped value was CL_NIL (0). */
            int32_t offset;
            uint32_t target_bc_off;
            uint32_t insn_start = ip - 1;
            int ok;
            if (ip + 4 > bc->code_len) goto fail;
            offset = read_i32_be(bc->code + ip);
            ip += 4;
            /* catch_ip == ip after consuming the i32 operand. */
            target_bc_off = compute_landing_ip(offset, ip, bc->code_len, &ok);
            if (!ok) goto fail;

            if (op == OP_JNIL || op == OP_JTRUE) {
                cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D0);
            }
            /* Flush whatever's left so we cross the branch with
             * depth=0 — matches the target's expected state.  This
             * happens AFTER the pop so the flush's MOVE.L D{5..7},-(A7)
             * predec pushes (which also set N/Z) don't pollute the
             * flag state we need for the branch test below. */
            cache_flush(cb, &cache_head, &cache_depth);
            if (op == OP_JNIL || op == OP_JTRUE) {
                /* Re-establish Z based on D0 — cache_pop_to_dn's
                 * internal register shifts (at depth >= 2) and the
                 * subsequent flush both clobber flags between the
                 * pop's initial move and the branch. */
                m68k_emit_tst_l_dn(cb, REG_D0);
            }
            if (!emit_bcc_to_bc(cb, op == OP_JMP ? BCC_ALWAYS :
                                    op == OP_JNIL ? BCC_EQ : BCC_NE,
                                target_bc_off, insn_start, bc_to_native,
                                &patches, &n_patches, &cap_patches))
                goto fail;
            break;
        }

        /* ---- Superinstructions (opcodes.h, specs/performance.md 4.3) ----
         * Each is the pair the peephole pass fused; the template is the
         * two members' templates back to back, minus the push/pop the
         * cache would have absorbed anyway.  The four with a heavier tail
         * (CALL_GLOBAL, STRUCT_REF, MV_RESET, RET) push the local and FALL
         * THROUGH into the tail's case, whose operands follow in the
         * stream — so the tail's template exists once. */

        /* ---- String-scan fast path (opcodes.h 0xC0-0xC4, spec 4.4) ---- */

        case OP_AREF: {
            /* u8 kind.  Three-arg helper (vec, idx, kind) = cl_vector_ref1,
             * the builtin's own path; it signals type / bounds errors, so
             * the cache is flushed first (same contract as STRUCT_REF). */
            uint8_t kind;
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_aref;
            if (ip >= bc->code_len) goto fail;
            kind = bc->code[ip++];
            if (kind > CL_AREF_KIND_SCHAR) goto fail;
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D1);   /* idx */
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D0);   /* vec */
            cache_flush(cb, &cache_head, &cache_depth);
            m68k_emit_move_l_imm32_predec(cb, (uint32_t)kind, REG_A7);
            m68k_emit_move_l_dn_predec_an(cb, REG_D1, REG_A7);
            m68k_emit_move_l_dn_predec_an(cb, REG_D0, REG_A7);
            m68k_emit_jsr_abs_l(cb, helper);
            m68k_emit_lea_disp_an_to_am(cb, 12, REG_A7, REG_A7);
            cache_push_dn(cb, &cache_head, &cache_depth, REG_D0);
            break;
        }

        case OP_CHAREQ:
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D1);
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D0);
            emit_chareq_compute(cb, cache_head, cache_depth, relocs);
            cache_push_dn(cb, &cache_head, &cache_depth, REG_D0);
            break;

        case OP_CMP_BR: {
            /* u8 kind, i32 offset: the comparison and the branch in one.
             * Pop b/a, flush (a branch boundary: the target expects
             * depth 0, and the slow helper may signal), then the inline
             * fixnum (or character) test branches straight to the target
             * on the effective condition; the slow path calls the kind
             * helper and branches on its T/NIL. */
            uint8_t kind;
            int cmp, when_true;
            int32_t offset;
            uint32_t target_bc_off;
            uint32_t insn_start = ip - 1;
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_cmp_kind;
            int32_t tag_a_pc, tag_b_pc, bra_pc, slow_off, done_off;
            int ok;
            if (ip + 5 > bc->code_len) goto fail;
            kind = bc->code[ip++];
            offset = read_i32_be(bc->code + ip);
            ip += 4;
            target_bc_off = compute_landing_ip(offset, ip, bc->code_len, &ok);
            if (!ok) goto fail;
            cmp = kind & CL_CMP_BR_CMP_MASK;
            when_true = (kind & CL_CMP_BR_IF_TRUE) != 0;
            if (cmp > CL_CMP_BR_CHAREQ || (kind & ~(CL_CMP_BR_CMP_MASK | CL_CMP_BR_IF_TRUE)))
                goto fail;

            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D1);   /* b */
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D0);   /* a */
            cache_flush(cb, &cache_head, &cache_depth);

            if (cmp == CL_CMP_BR_CHAREQ) {
                m68k_emit_cmpi_b_imm_dn(cb, CL_TAG_CHAR, REG_D0);
                tag_a_pc = (int32_t)cb_len(cb) + 2;
                m68k_emit_bne_w(cb, 0);
                m68k_emit_cmpi_b_imm_dn(cb, CL_TAG_CHAR, REG_D1);
                tag_b_pc = (int32_t)cb_len(cb) + 2;
                m68k_emit_bne_w(cb, 0);
            } else {
                m68k_emit_btst_imm_dn(cb, 0, REG_D0);
                tag_a_pc = (int32_t)cb_len(cb) + 2;
                m68k_emit_beq_w(cb, 0);
                m68k_emit_btst_imm_dn(cb, 0, REG_D1);
                tag_b_pc = (int32_t)cb_len(cb) + 2;
                m68k_emit_beq_w(cb, 0);
            }
            m68k_emit_cmp_l_dn_dm(cb, REG_D1, REG_D0);             /* flags = a - b */
            if (!emit_bcc_to_bc(cb, cmp_br_condition(cmp, when_true),
                                target_bc_off, insn_start, bc_to_native,
                                &patches, &n_patches, &cap_patches))
                goto fail;
            bra_pc = (int32_t)cb_len(cb) + 2;
            m68k_emit_bra_w(cb, 0);

            /* Slow path: helper(a, b, cmp) -> T/NIL in D0. */
            slow_off = (int32_t)cb_len(cb);
            m68k_emit_move_l_imm32_predec(cb, (uint32_t)cmp, REG_A7);
            m68k_emit_move_l_dn_predec_an(cb, REG_D1, REG_A7);
            m68k_emit_move_l_dn_predec_an(cb, REG_D0, REG_A7);
            m68k_emit_jsr_abs_l(cb, helper);
            m68k_emit_lea_disp_an_to_am(cb, 12, REG_A7, REG_A7);
            m68k_emit_tst_l_dn(cb, REG_D0);
            if (!emit_bcc_to_bc(cb, when_true ? BCC_NE : BCC_EQ,
                                target_bc_off, insn_start, bc_to_native,
                                &patches, &n_patches, &cap_patches))
                goto fail;
            done_off = (int32_t)cb_len(cb);

            m68k_patch_disp16(cb_data(cb), cb_len(cb), (uint32_t)tag_a_pc,
                              (int16_t)(slow_off - tag_a_pc));
            m68k_patch_disp16(cb_data(cb), cb_len(cb), (uint32_t)tag_b_pc,
                              (int16_t)(slow_off - tag_b_pc));
            m68k_patch_disp16(cb_data(cb), cb_len(cb), (uint32_t)bra_pc,
                              (int16_t)(done_off - bra_pc));
            break;
        }

        case OP_PUSH_LOCAL: {
            /* u8 slot.  Pop the item, hand the helper its value and the
             * slot's ADDRESS in the LINK frame; it conses, stores and
             * returns the new list.  cl_cons allocates: flush first so the
             * cached operands are on the m68k stack for the scan. */
            uint8_t slot;
            int16_t disp;
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_push_local;
            if (ip >= bc->code_len) goto fail;
            slot = bc->code[ip++];
            if (slot >= n_locals) goto fail;
            disp = slot_disp(slot, slot_anchor, in_frame);
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D0);   /* item */
            cache_flush(cb, &cache_head, &cache_depth);
            m68k_emit_lea_disp_an_to_am(cb, disp, REG_A6, REG_A0);
            m68k_emit_move_l_an_predec_am(cb, REG_A0, REG_A7);         /* &slot */
            m68k_emit_move_l_dn_predec_an(cb, REG_D0, REG_A7);         /* item */
            m68k_emit_jsr_abs_l(cb, helper);
            m68k_emit_addq_l_an(cb, 8, REG_A7);
            cache_push_dn(cb, &cache_head, &cache_depth, REG_D0);
            break;
        }

        case OP_POP_LOCAL: {
            /* u8 slot.  Helper takes the slot's address, returns the car
             * and stores the cdr.  Non-allocating, but CAR's type error
             * allocates a condition: flush. */
            uint8_t slot;
            int16_t disp;
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_pop_local;
            if (ip >= bc->code_len) goto fail;
            slot = bc->code[ip++];
            if (slot >= n_locals) goto fail;
            disp = slot_disp(slot, slot_anchor, in_frame);
            cache_flush(cb, &cache_head, &cache_depth);
            m68k_emit_lea_disp_an_to_am(cb, disp, REG_A6, REG_A0);
            m68k_emit_move_l_an_predec_am(cb, REG_A0, REG_A7);
            m68k_emit_jsr_abs_l(cb, helper);
            m68k_emit_addq_l_an(cb, 4, REG_A7);
            cache_push_dn(cb, &cache_head, &cache_depth, REG_D0);
            break;
        }

        case OP_STORE_POP: {
            uint8_t slot;
            int16_t disp;
            if (ip >= bc->code_len) goto fail;
            slot = bc->code[ip++];
            if (slot >= n_locals) goto fail;
            disp = slot_disp(slot, slot_anchor, in_frame);
            if (cache_depth >= 1) {
                m68k_emit_move_l_dn_to_disp_am(cb, (M68kReg)cache_head,
                                               disp, REG_A6);
            } else {
                m68k_emit_move_l_an_to_disp_am(cb, REG_A7, disp, REG_A6);
            }
            cache_drop(cb, &cache_head, &cache_depth);
            break;
        }

        case OP_LOAD_LOAD: {
            uint8_t a, b;
            if (ip + 1 >= bc->code_len) goto fail;
            a = bc->code[ip];
            b = bc->code[ip + 1];
            ip += 2;
            if (a >= n_locals || b >= n_locals) goto fail;
            cache_push_disp_an(cb, &cache_head, &cache_depth,
                               slot_disp(a, slot_anchor, in_frame), REG_A6);
            cache_push_disp_an(cb, &cache_head, &cache_depth,
                               slot_disp(b, slot_anchor, in_frame), REG_A6);
            break;
        }

        case OP_EQ_JNIL: {
            /* OP_EQ pushes T when the two are the same object and JNIL
             * jumps on NIL: pop both, compare, branch when NOT equal. */
            int32_t offset;
            uint32_t target_bc_off;
            uint32_t insn_start = ip - 1;
            int ok;
            if (ip + 4 > bc->code_len) goto fail;
            offset = read_i32_be(bc->code + ip);
            ip += 4;
            target_bc_off = compute_landing_ip(offset, ip, bc->code_len, &ok);
            if (!ok) goto fail;
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D1);   /* b */
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D0);   /* a */
            cache_flush(cb, &cache_head, &cache_depth);
            m68k_emit_cmp_l_dn_dm(cb, REG_D1, REG_D0);
            if (!emit_bcc_to_bc(cb, BCC_NE, target_bc_off, insn_start,
                                bc_to_native, &patches, &n_patches,
                                &cap_patches))
                goto fail;
            break;
        }

        case OP_GLOAD_JNIL: {
            /* The OP_GLOAD helper call (value in D0, nothing pushed), then
             * the JNIL test on D0. */
            uint16_t idx;
            CL_Obj sym;
            int32_t offset;
            uint32_t target_bc_off;
            uint32_t insn_start = ip - 1;
            int ok;
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_gload;
            if (ip + 6 > bc->code_len) goto fail;
            idx = ((uint16_t)bc->code[ip] << 8) | bc->code[ip + 1];
            ip += 2;
            offset = read_i32_be(bc->code + ip);
            ip += 4;
            target_bc_off = compute_landing_ip(offset, ip, bc->code_len, &ok);
            if (!ok) goto fail;
            if (idx >= bc->n_constants || bc->constants == NULL) goto fail;
            sym = bc->constants[idx];
            if (!CL_SYMBOL_P(sym)) goto fail;
            cache_flush(cb, &cache_head, &cache_depth);
            emit_obj_imm_predec(cb, relocs, sym);
            m68k_emit_jsr_abs_l(cb, helper);
            m68k_emit_addq_l_an(cb, 4, REG_A7);
            m68k_emit_tst_l_dn(cb, REG_D0);
            if (!emit_bcc_to_bc(cb, BCC_EQ, target_bc_off, insn_start,
                                bc_to_native, &patches, &n_patches,
                                &cap_patches))
                goto fail;
            break;
        }

        case OP_GLOAD_EQ_JNIL: {
            /* The OP_GLOAD helper (value in D0), the other operand popped
             * into D1 — after the flush, so nothing clobbers D0 — then
             * compare and branch when NOT the same object. */
            uint16_t idx;
            CL_Obj sym;
            int32_t offset;
            uint32_t target_bc_off;
            uint32_t insn_start = ip - 1;
            int ok;
            uint32_t helper = (uint32_t)(uintptr_t)&cl_jit_runtime_gload;
            if (ip + 6 > bc->code_len) goto fail;
            idx = ((uint16_t)bc->code[ip] << 8) | bc->code[ip + 1];
            ip += 2;
            offset = read_i32_be(bc->code + ip);
            ip += 4;
            target_bc_off = compute_landing_ip(offset, ip, bc->code_len, &ok);
            if (!ok) goto fail;
            if (idx >= bc->n_constants || bc->constants == NULL) goto fail;
            sym = bc->constants[idx];
            if (!CL_SYMBOL_P(sym)) goto fail;
            cache_flush(cb, &cache_head, &cache_depth);
            emit_obj_imm_predec(cb, relocs, sym);
            m68k_emit_jsr_abs_l(cb, helper);
            m68k_emit_addq_l_an(cb, 4, REG_A7);
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D1);   /* a, from (a7)+ */
            m68k_emit_cmp_l_dn_dm(cb, REG_D1, REG_D0);
            if (!emit_bcc_to_bc(cb, BCC_NE, target_bc_off, insn_start,
                                bc_to_native, &patches, &n_patches,
                                &cap_patches))
                goto fail;
            break;
        }

        case OP_LOAD_CONST: {
            uint8_t slot;
            uint16_t idx;
            if (ip + 2 >= bc->code_len) goto fail;
            slot = bc->code[ip++];
            idx = ((uint16_t)bc->code[ip] << 8) | bc->code[ip + 1];
            ip += 2;
            if (slot >= n_locals) goto fail;
            if (idx >= bc->n_constants || bc->constants == NULL) goto fail;
            cache_push_disp_an(cb, &cache_head, &cache_depth,
                               slot_disp(slot, slot_anchor, in_frame), REG_A6);
            cache_push_obj(cb, &cache_head, &cache_depth, relocs,
                           bc->constants[idx]);
            break;
        }

        case OP_LOAD_STORE_POP: {
            /* locals[b] = locals[a], through D0; the cache is untouched. */
            uint8_t a, b;
            if (ip + 1 >= bc->code_len) goto fail;
            a = bc->code[ip];
            b = bc->code[ip + 1];
            ip += 2;
            if (a >= n_locals || b >= n_locals) goto fail;
            m68k_emit_move_l_disp_an_to_dn(cb, slot_disp(a, slot_anchor, in_frame),
                                           REG_A6, REG_D0);
            m68k_emit_move_l_dn_to_disp_am(cb, REG_D0,
                                           slot_disp(b, slot_anchor, in_frame),
                                           REG_A6);
            break;
        }

        case OP_POP_LOAD: {
            uint8_t slot;
            if (ip >= bc->code_len) goto fail;
            slot = bc->code[ip++];
            if (slot >= n_locals) goto fail;
            cache_drop(cb, &cache_head, &cache_depth);
            cache_push_disp_an(cb, &cache_head, &cache_depth,
                               slot_disp(slot, slot_anchor, in_frame), REG_A6);
            break;
        }

        case OP_LOAD_JNIL: {
            /* The local straight into D0 (MOVE.L to a data register sets
             * Z), after the flush so nothing clobbers the flags. */
            uint8_t slot;
            int16_t disp;
            int32_t offset;
            uint32_t target_bc_off;
            uint32_t insn_start = ip - 1;
            int ok;
            if (ip + 5 > bc->code_len) goto fail;
            slot = bc->code[ip++];
            offset = read_i32_be(bc->code + ip);
            ip += 4;
            target_bc_off = compute_landing_ip(offset, ip, bc->code_len, &ok);
            if (!ok) goto fail;
            if (slot >= n_locals) goto fail;
            disp = slot_disp(slot, slot_anchor, in_frame);
            cache_flush(cb, &cache_head, &cache_depth);
            m68k_emit_move_l_disp_an_to_dn(cb, disp, REG_A6, REG_D0);
            if (!emit_bcc_to_bc(cb, BCC_EQ, target_bc_off, insn_start,
                                bc_to_native, &patches, &n_patches,
                                &cap_patches))
                goto fail;
            break;
        }

        case OP_LOAD_MV_RESET: {
            uint8_t slot;
            if (ip >= bc->code_len) goto fail;
            slot = bc->code[ip++];
            if (slot >= n_locals) goto fail;
            cache_push_disp_an(cb, &cache_head, &cache_depth,
                               slot_disp(slot, slot_anchor, in_frame), REG_A6);
            m68k_emit_jsr_abs_l(cb, (uint32_t)(uintptr_t)&cl_jit_runtime_mv_reset);
            break;
        }

        case OP_LOAD_RET: {
            uint8_t slot;
            if (ip >= bc->code_len) goto fail;
            slot = bc->code[ip++];
            if (slot >= n_locals) goto fail;
            cache_push_disp_an(cb, &cache_head, &cache_depth,
                               slot_disp(slot, slot_anchor, in_frame), REG_A6);
        }
        /* FALLTHROUGH into OP_RET */

        case OP_RET:
            /* Pop result into D0 (cache-aware), restore the callee-
             * saved cache regs from their A6-relative slots (the
             * positions are independent of where SP currently is),
             * then UNLK / RTS.  The walk goes on: since the peephole's
             * return rewrites (spec 4.3) a function returns wherever a
             * RETURN-FROM or a block arm ends, so more code — reachable
             * only as a branch target, where the cache is empty by the
             * depth-0 invariant — may follow.  The forward patches are
             * resolved after the walk. */
            bc_to_native[ip] = (int32_t)cb_len(cb);  /* fall-through target */
            cache_pop_to_dn(cb, &cache_head, &cache_depth, REG_D0);
            m68k_emit_move_l_disp_an_to_dn(cb, saved_d7_disp, REG_A6, REG_D7);
            m68k_emit_move_l_disp_an_to_dn(cb, saved_d6_disp, REG_A6, REG_D6);
            m68k_emit_move_l_disp_an_to_dn(cb, saved_d5_disp, REG_A6, REG_D5);
            m68k_emit_unlk_an(cb, REG_A6);
            m68k_emit_rts(cb);
            cache_depth = 0;
            saw_ret = 1;
            break;

        default:
            goto fail;
        }
    }
    /* Falling off the end without any OP_RET = malformed bytecode. */
    if (!saw_ret) goto fail;
    jit_lines_note(&lines, bc, cb, op_native, op_ip);

    for (i = 0; i < n_patches; i++) {
        BranchPatch *p = &patches[i];
        int32_t target_native;
        int32_t disp32;
        if (p->target_bc_off > bc->code_len) goto fail;
        target_native = bc_to_native[p->target_bc_off];
        if (target_native < 0) goto fail;
        disp32 = target_native - (int32_t)p->patch_off;
        if (disp32 < -32768 || disp32 > 32767) goto fail;
        m68k_patch_disp16(cb_data(cb), cb_len(cb),
                          p->patch_off, (int16_t)disp32);
    }
    code_end = cb_len(cb);
    if (sites.oom || !emit_call_site_cells(cb, &sites)) goto fail;
    emit_line_table(cb, &lines, code_end);
    result = 1;
    goto cleanup;

fail:
    result = 0;
cleanup:
    if (bc_to_native) platform_free(bc_to_native);
    if (is_target)    platform_free(is_target);
    if (patches)      platform_free(patches);
    if (sites.refs) platform_free(sites.refs);
    if (lines.ents) platform_free(lines.ents);
    return result;
}

/* REPLACE: drop native code the bytecode already carries and install the
 * new one (cl_jit_compile: re-JIT after a repeated FASL load, the stub
 * builtin's callers).  Without it (the hot path) existing code is never
 * touched: another thread that reached the threshold of the same function
 * may already be running it, so a compile that finds code installed when
 * it is done throws its own away.  The codegen is deterministic, so the
 * rare pair of compiles that both install (preempted between the check
 * and the stores) writes identical code and reloc tables -- the loser's
 * buffer leaks, nothing is freed under a running caller. */
void cl_jit_backend_compile(CL_Bytecode *bc, int replace)
{
    CodeBuf cb;
    JitRelocs relocs;
    uint8_t *code;
    uint32_t len;
    CL_Obj value;
    uint8_t slot;

    if (bc == NULL || !cl_jitc_active) return;
    if (!replace) {
        if (bc->native_code) return;
    } else {
        /* Drop any stale native code + relocs from a prior compile of this
         * bytecode (re-JIT after redefinition / repeated FASL load).  A
         * call site may cache the old entry: invalidate them all.  Unhook
         * BEFORE the bump, so a site fill that reads the new generation
         * can no longer see the old pointer (runtime_m68k.c, the fill rule). */
        void *old_code = bc->native_code;
        if (bc->native_relocs) { platform_free(bc->native_relocs); }
        bc->native_code   = NULL;
        bc->native_len    = 0;
        bc->native_relocs = NULL;
        bc->native_reloc_count = 0;
        if (old_code) {
            cl_call_gen_bump("native code replaced");
            platform_free(old_code);
        }
    }

    cb_init(&cb, 8);
    jit_relocs_init(&relocs);

    if (matches_trivial_leaf(bc, &value)) {
        /* Emit: load <value> into D0 ; rts.  m68k SysV ABI returns the
         * function result in D0, so the load + rts is a complete leaf
         * function callable from C with no register save/restore (the
         * load only touches D0, which is caller-saved).  emit_obj_imm_d0
         * records a reloc when <value> is a heap object (e.g. a quoted
         * symbol/list) so the compactor can forward the baked literal. */
        emit_obj_imm_d0(&cb, &relocs, value);
        m68k_emit_rts(&cb);
    } else if (matches_passthrough(bc, &slot)) {
        /* Emit: jsr mv_reset ; move.l (8 + 4*(n-1-slot))(sp),d0 ; rts.
         * After the JSR the return address is at (sp) and `func` at
         * 4(sp); above it the n arguments sit in operand-stack order
         * (the positional native ABI, see slot_disp), so user-arg j is
         * at (8 + 4*(n-1-j))(sp).  No heap immediates.
         *
         * The helper call comes FIRST so the displacement is computed
         * against the unchanged A7 (JSR's pushed return address is gone
         * again by the time the move runs), and so the load into D0 —
         * a caller-saved register the helper is free to clobber — is
         * the last thing that happens before RTS.  It writes
         * `cl_mv_count = 1`, which the matched shape requires: OP_LOAD
         * does not touch the MV buffer (see matches_passthrough). */
        int16_t disp = (int16_t)(8 + 4 * ((int)bc->arity - 1 - (int)slot));
        m68k_emit_jsr_abs_l(&cb, (uint32_t)(uintptr_t)&cl_jit_runtime_mv_reset);
        m68k_emit_move_l_disp_an_to_dn(&cb, disp, REG_A7, REG_D0);
        m68k_emit_rts(&cb);
    } else if (walker_compile(bc, &cb, &relocs)) {
        /* Walker handled it — full per-opcode template emission with
         * LINK frame + m68k-stack operand stack.  Larger code than the
         * matchers' tight templates, but covers any shape built from
         * the supported opcodes. */
    } else {
        /* Walker bailed (unsupported opcode, oversized frame, etc.) —
         * discard whatever it emitted into cb and fall back to the
         * bytecode interpreter for this function. */
        jit_relocs_free(&relocs);
        cb_free(&cb);
        return;
    }

    /* If the reloc table couldn't be grown (OOM), we can't guarantee the
     * compactor will forward every baked heap immediate — shipping this
     * code would risk stale offsets after a move.  Fall back to the
     * interpreter instead. */
    if (relocs.oom || relocs.count > 0xFFFF) {
        jit_relocs_free(&relocs);
        cb_free(&cb);
        return;
    }

    code = cb_finish(&cb, &len);
    if (code == NULL) { jit_relocs_free(&relocs); return; }
    if (!replace && bc->native_code) {
        /* A peer thread compiled it meanwhile (see above). */
        jit_relocs_free(&relocs);
        platform_free(code);
        return;
    }

    /* Install RELOCS BEFORE native_code: the compactor forwards baked
     * immediates only when BOTH fields are non-NULL (mem.c), so the
     * reverse order would ship stale immediates if a compaction could
     * ever intervene between the two stores.  Today this window is
     * allocation-free under cooperative STW (compaction cannot occur
     * mid-compile) — the ordering makes the invariant structural rather
     * than incidental. */
    if (relocs.count > 0) {
        uint32_t *tab = (uint32_t *)platform_alloc(
            (unsigned long)relocs.count * sizeof(uint32_t));
        if (tab == NULL) {
            /* Can't store the table → can't safely keep the native code. */
            jit_relocs_free(&relocs);
            platform_free(code);
            return;
        }
        {
            uint32_t i;
            for (i = 0; i < relocs.count; i++) tab[i] = relocs.offs[i];
        }
        bc->native_relocs      = tab;
        bc->native_reloc_count = (uint16_t)relocs.count;
    }
    jit_relocs_free(&relocs);

    bc->native_code = code;
    bc->native_len  = len;
    cl_jitc_native_bytes += len;

    /* Flush I-cache so the freshly written bytes aren't served stale.
     * REQUIRED on every 68020+ — the 020/030 also have a 256-byte
     * I-cache; only the 68000/010 lack one.  Do NOT gate this on
     * 040/060. */
    platform_cache_clear(code, len);
}

/* --- Disassembler -----------------------------------------------------
 *
 * Decodes the m68k instructions the JIT can emit and prints one line of
 * assembly per instruction to platform_write_string.  Not a general
 * m68k disassembler — only the ~13 instruction forms the matchers and
 * walker use.  Anything else falls through to ".word $xxxx" so the user
 * still sees the raw word.
 *
 * Exposed to Lisp as `amilisp::%JIT-DISASSEMBLE`; the `jitexpand` macro
 * in lib/boot.lisp wraps the common case.
 */

/* Decode one effective-address mode/reg pair, advancing *pos past any
 * extension words.  size_l selects between 32-bit (immediate .L) and
 * 16-bit immediates — only matters for mode=7/reg=4.  Returns 0 on
 * success, -1 if the EA is one the JIT never emits (so we don't have to
 * cover the full m68k EA space here). */
static int disasm_ea(int mode, int reg, const uint8_t *code, uint32_t len,
                     uint32_t *pos, int size_l, char *buf, int bufsize)
{
    switch (mode) {
    case 0: snprintf(buf, (size_t)bufsize, "d%d", reg); return 0;
    case 1: snprintf(buf, (size_t)bufsize, "a%d", reg); return 0;
    case 2: snprintf(buf, (size_t)bufsize, "(a%d)", reg); return 0;
    case 3: snprintf(buf, (size_t)bufsize, "(a%d)+", reg); return 0;
    case 4: snprintf(buf, (size_t)bufsize, "-(a%d)", reg); return 0;
    case 5: {
        int16_t d;
        if (*pos + 2 > len) return -1;
        d = (int16_t)(((uint16_t)code[*pos] << 8) | code[*pos + 1]);
        *pos += 2;
        snprintf(buf, (size_t)bufsize, "%d(a%d)", (int)d, reg);
        return 0;
    }
    case 7:
        if (reg == 4) {
            uint32_t v;
            if (size_l) {
                if (*pos + 4 > len) return -1;
                v = ((uint32_t)code[*pos]     << 24)
                  | ((uint32_t)code[*pos + 1] << 16)
                  | ((uint32_t)code[*pos + 2] << 8)
                  |  (uint32_t)code[*pos + 3];
                *pos += 4;
            } else {
                if (*pos + 2 > len) return -1;
                v = ((uint32_t)code[*pos] << 8) | code[*pos + 1];
                *pos += 2;
            }
            snprintf(buf, (size_t)bufsize, "#$%08lx", (unsigned long)v);
            return 0;
        }
        if (reg == 1) {                             /* (xxx).L */
            uint32_t v;
            if (*pos + 4 > len) return -1;
            v = ((uint32_t)code[*pos]     << 24)
              | ((uint32_t)code[*pos + 1] << 16)
              | ((uint32_t)code[*pos + 2] << 8)
              |  (uint32_t)code[*pos + 3];
            *pos += 4;
            snprintf(buf, (size_t)bufsize, "$%08lx", (unsigned long)v);
            return 0;
        }
        return -1;
    default:
        return -1;
    }
}

/* Decode one instruction at code+offset.  Returns its length in bytes
 * (>=2), or 0 if the buffer ran out before the operand words could be
 * read.  On unknown opcodes returns 2 with mnemonic ".word $xxxx" so
 * the caller still advances. */
static uint32_t disasm_one(const uint8_t *code, uint32_t len,
                           uint32_t offset, char *mnemonic, int msize)
{
    uint16_t op;
    uint32_t pos;
    int matched = 0;

    if (offset + 2 > len) return 0;
    op = (uint16_t)(((uint16_t)code[offset] << 8) | code[offset + 1]);
    pos = offset + 2;

    if (op == 0x4E71) {
        snprintf(mnemonic, (size_t)msize, "nop"); matched = 1;
    } else if (op == 0x4E75) {
        snprintf(mnemonic, (size_t)msize, "rts"); matched = 1;
    } else if ((op & 0xFFF8) == 0x4E50) {           /* LINK An,#d16 */
        int an = op & 7;
        int16_t d;
        if (pos + 2 > len) return 0;
        d = (int16_t)(((uint16_t)code[pos] << 8) | code[pos + 1]);
        pos += 2;
        snprintf(mnemonic, (size_t)msize, "link a%d,#%d", an, (int)d);
        matched = 1;
    } else if ((op & 0xFFF8) == 0x4E58) {           /* UNLK An */
        int an = op & 7;
        snprintf(mnemonic, (size_t)msize, "unlk a%d", an);
        matched = 1;
    } else if ((op & 0xFFF8) == 0x42A0) {           /* CLR.L -(An) */
        int an = op & 7;
        snprintf(mnemonic, (size_t)msize, "clr.l -(a%d)", an);
        matched = 1;
    } else if ((op & 0xFFF8) == 0x4298) {           /* CLR.L (An)+ */
        snprintf(mnemonic, (size_t)msize, "clr.l (a%d)+", op & 7);
        matched = 1;
    } else if ((op & 0xF1F8) == 0xD1C0) {           /* ADDA.L Dn,An */
        snprintf(mnemonic, (size_t)msize, "adda.l d%d,a%d", op & 7,
                 (op >> 9) & 7);
        matched = 1;
    } else if ((op & 0xFFF8) == 0x51C8) {           /* DBF Dn,disp */
        int16_t d;
        if (pos + 2 > len) return 0;
        d = (int16_t)(((uint16_t)code[pos] << 8) | code[pos + 1]);
        snprintf(mnemonic, (size_t)msize, "dbf d%d,%ld", op & 7,
                 (long)pos + (long)d);
        pos += 2;
        matched = 1;
    } else if ((op & 0xF100) == 0x7000) {           /* MOVEQ #imm,Dn */
        int dn = (op >> 9) & 7;
        int8_t imm = (int8_t)(op & 0xFF);
        snprintf(mnemonic, (size_t)msize, "moveq #%d,d%d", (int)imm, dn);
        matched = 1;
    } else if ((op & 0xF1F8) == 0x5088) {           /* ADDQ.L #data,An */
        int data = (op >> 9) & 7;
        int an = op & 7;
        if (data == 0) data = 8;
        snprintf(mnemonic, (size_t)msize, "addq.l #%d,a%d", data, an);
        matched = 1;
    } else if ((op & 0xF0FF) == 0x6000) {           /* Bcc.W (disp=0 in opcode) */
        /* Bcc with 8-bit disp field == 0 means a 16-bit displacement
         * word follows, relative to PC = (instr_start + 2).  Condition
         * is in bits 11-8: 0=BRA, 6=BNE, 7=BEQ, 9=BVS, 13=BLT.  Other
         * conditions decode as bXX.w with the numeric condition for
         * readability. */
        static const char *cc_names[16] = {
            "bra", "bsr", "bhi", "bls", "bcc", "bcs", "bne", "beq",
            "bvc", "bvs", "bpl", "bmi", "bge", "blt", "bgt", "ble"
        };
        int16_t d;
        int cond = (op >> 8) & 0xF;
        long target;
        if (pos + 2 > len) return 0;
        d = (int16_t)(((uint16_t)code[pos] << 8) | code[pos + 1]);
        pos += 2;
        target = (long)offset + 2L + (long)d;
        snprintf(mnemonic, (size_t)msize, "%s.w %ld",
                 cc_names[cond], target);
        matched = 1;
    } else if ((op & 0xF1C0) == 0x41C0) {           /* LEA (d16,An),Am */
        /* LEA: 0100 am 111 mmm rrr.  The JIT only emits the
         * (d16,An) source form (mode 5), so we decode mmm=101
         * here; anything else falls through to .word $xxxx. */
        int am_dst = (op >> 9) & 7;
        int src_mode = (op >> 3) & 7;
        int src_reg  = op & 7;
        if (src_mode == 5) {
            int16_t d;
            if (pos + 2 > len) return 0;
            d = (int16_t)(((uint16_t)code[pos] << 8) | code[pos + 1]);
            pos += 2;
            snprintf(mnemonic, (size_t)msize, "lea %d(a%d),a%d",
                     (int)d, src_reg, am_dst);
            matched = 1;
        } else if (src_mode == 7 && src_reg == 2) {  /* LEA (d16,PC),Am */
            int16_t d;
            if (pos + 2 > len) return 0;
            d = (int16_t)(((uint16_t)code[pos] << 8) | code[pos + 1]);
            snprintf(mnemonic, (size_t)msize, "lea %ld(pc),a%d",
                     (long)pos + (long)d, am_dst);
            pos += 2;
            matched = 1;
        }
    } else if ((op & 0xFFF8) == 0x4A80) {           /* TST.L Dn */
        int dn = op & 7;
        snprintf(mnemonic, (size_t)msize, "tst.l d%d", dn);
        matched = 1;
    } else if ((op & 0xFFF8) == 0x4E90) {           /* JSR (An) */
        snprintf(mnemonic, (size_t)msize, "jsr (a%d)", op & 7);
        matched = 1;
    } else if ((op & 0xFFF8) == 0x4ED0) {           /* JMP (An) */
        snprintf(mnemonic, (size_t)msize, "jmp (a%d)", op & 7);
        matched = 1;
    } else if ((op & 0xF1FF) == 0xC0FC) {           /* MULU.W #imm,Dn */
        uint16_t imm;
        if (pos + 2 > len) return 0;
        imm = (uint16_t)(((uint16_t)code[pos] << 8) | code[pos + 1]);
        pos += 2;
        snprintf(mnemonic, (size_t)msize, "mulu.w #%u,d%d", (unsigned)imm,
                 (op >> 9) & 7);
        matched = 1;
    } else if ((op & 0xFFF8) == 0x4868) {           /* PEA (d16,An) */
        int16_t d;
        if (pos + 2 > len) return 0;
        d = (int16_t)(((uint16_t)code[pos] << 8) | code[pos + 1]);
        pos += 2;
        snprintf(mnemonic, (size_t)msize, "pea %d(a%d)", (int)d, op & 7);
        matched = 1;
    } else if (op == 0x4EB9) {                      /* JSR (xxx).L */
        uint32_t addr;
        if (pos + 4 > len) return 0;
        addr = ((uint32_t)code[pos]     << 24)
             | ((uint32_t)code[pos + 1] << 16)
             | ((uint32_t)code[pos + 2] <<  8)
             |  (uint32_t)code[pos + 3];
        pos += 4;
        snprintf(mnemonic, (size_t)msize, "jsr $%08lx", (unsigned long)addr);
        matched = 1;
    } else if ((op & 0xFFC0) == 0x0800) {           /* BTST #imm,<ea> (static) */
        /* Two-word form: 0000 1000 00 mmm rrr + 16-bit imm word.  We
         * only emit BTST #imm,Dn (mode 000). */
        int mode = (op >> 3) & 7;
        int reg  = op & 7;
        uint16_t imm_word;
        if (pos + 2 > len) return 0;
        imm_word = (uint16_t)(((uint16_t)code[pos] << 8) | code[pos + 1]);
        pos += 2;
        if (mode == 0) {
            snprintf(mnemonic, (size_t)msize, "btst #%u,d%d",
                     (unsigned)(imm_word & 0x1F), reg);
        } else {
            snprintf(mnemonic, (size_t)msize, "btst #%u,<ea>",
                     (unsigned)(imm_word & 0x1F));
        }
        matched = 1;
    } else if ((op & 0xF000) == 0xC000
            && ((op >> 6) & 7) == 2
            && ((op >> 3) & 7) == 0) {              /* AND.L Dn,Dm */
        int dm = (op >> 9) & 7;
        int dn = op & 7;
        snprintf(mnemonic, (size_t)msize, "and.l d%d,d%d", dn, dm);
        matched = 1;
    } else if ((op & 0xF000) == 0xD000
            && ((op >> 6) & 7) == 2
            && ((op >> 3) & 7) == 0) {              /* ADD.L Dn,Dm */
        int dm = (op >> 9) & 7;
        int dn = op & 7;
        snprintf(mnemonic, (size_t)msize, "add.l d%d,d%d", dn, dm);
        matched = 1;
    } else if ((op & 0xF000) == 0x9000
            && ((op >> 6) & 7) == 2
            && ((op >> 3) & 7) == 0) {              /* SUB.L Dn,Dm */
        int dm = (op >> 9) & 7;
        int dn = op & 7;
        snprintf(mnemonic, (size_t)msize, "sub.l d%d,d%d", dn, dm);
        matched = 1;
    } else if ((op & 0xF000) == 0xB000
            && (((op >> 6) & 7) == 2 || ((op >> 6) & 7) == 7)) {
        /* CMP.L <ea>,Dn (opmode 010) / CMPA.L <ea>,An (opmode 111) */
        int rn = (op >> 9) & 7;
        char src[40];
        if (disasm_ea((op >> 3) & 7, op & 7, code, len, &pos, 1,
                      src, sizeof src) < 0) {
            snprintf(mnemonic, (size_t)msize, ".word $%04x", op);
            pos = offset + 2;
        } else if (((op >> 6) & 7) == 2) {
            snprintf(mnemonic, (size_t)msize, "cmp.l %s,d%d", src, rn);
        } else {
            snprintf(mnemonic, (size_t)msize, "cmpa.l %s,a%d", src, rn);
        }
        matched = 1;
    } else if ((op & 0xF1C0) == 0x5080) {           /* ADDQ.L #imm,Dn */
        int data = (op >> 9) & 7;
        int dn = op & 7;
        if (data == 0) data = 8;
        snprintf(mnemonic, (size_t)msize, "addq.l #%d,d%d", data, dn);
        matched = 1;
    } else if ((op & 0xF1F8) == 0x5168) {           /* SUBQ.W #imm,(d16,An) */
        int data = (op >> 9) & 7;
        int an = op & 7;
        int16_t d;
        if (pos + 2 > len) return 0;
        d = (int16_t)(((uint16_t)code[pos] << 8) | code[pos + 1]);
        pos += 2;
        if (data == 0) data = 8;
        snprintf(mnemonic, (size_t)msize, "subq.w #%d,%d(a%d)", data, (int)d, an);
        matched = 1;
    } else if ((op & 0xF1C0) == 0x5180) {           /* SUBQ.L #imm,Dn */
        int data = (op >> 9) & 7;
        int dn = op & 7;
        if (data == 0) data = 8;
        snprintf(mnemonic, (size_t)msize, "subq.l #%d,d%d", data, dn);
        matched = 1;
    } else if ((op & 0xF000) == 0x2000) {           /* MOVE.L src,dst */
        int dst_reg = (op >> 9) & 7;
        int dst_mode = (op >> 6) & 7;
        int src_mode = (op >> 3) & 7;
        int src_reg = op & 7;
        char src[40], dst[40];
        if (disasm_ea(src_mode, src_reg, code, len, &pos, 1,
                      src, sizeof src) < 0 ||
            disasm_ea(dst_mode, dst_reg, code, len, &pos, 1,
                      dst, sizeof dst) < 0) {
            snprintf(mnemonic, (size_t)msize, ".word $%04x", op);
            pos = offset + 2;  /* roll back; we'll only consume the opcode */
        } else {
            snprintf(mnemonic, (size_t)msize, "move.l %s,%s", src, dst);
        }
        matched = 1;
    }

    if (!matched) {
        snprintf(mnemonic, (size_t)msize, ".word $%04x", op);
    }
    return pos - offset;
}

/* Read a big-endian longword (call-site cells, for the disassembly). */
static uint32_t disasm_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static uint32_t disasm_u16(const uint8_t *p)
{
    return ((uint32_t)p[0] << 8) | (uint32_t)p[1];
}

/* The line table's trailer (emit_line_table): 1 and the table's bounds
 * when CODE carries one. */
static int jit_line_table(const uint8_t *code, uint32_t len,
                          uint32_t *code_end, uint32_t *table_off,
                          uint32_t *count)
{
    uint32_t te, to;
    if (code == NULL || len < 12 || disasm_u32(code + len - 4) != JIT_LINES_MAGIC)
        return 0;
    te = disasm_u32(code + len - 12);
    to = disasm_u32(code + len - 8);
    if (to > len - 12 || te > to || ((len - 12 - to) & 3) != 0) return 0;
    *code_end  = te;
    *table_off = to;
    *count     = (len - 12 - to) / 4;
    return 1;
}

void cl_jit_disassemble(const uint8_t *code, uint32_t len)
{
    uint32_t off = 0;
    uint32_t cells = len;   /* where the call-site cells begin (= the end) */
    uint32_t sites_end = len, code_end, table_off = len, n_lines = 0;
    char mnem[80];
    char line[160];
    if (jit_line_table(code, len, &code_end, &table_off, &n_lines)) {
        cells = code_end;
        sites_end = table_off;
    }
    while (off < len && off < cells) {
        uint32_t n = disasm_one(code, len, off, mnem, sizeof mnem);
        char hex[40];
        char *hp = hex;
        uint32_t i;
        if (n == 0) {
            snprintf(line, sizeof line, "  %04lu: <decode short> end\n",
                     (unsigned long)off);
            cl_write_cstring_to_stdout(line);
            return;
        }
        for (i = 0; i < n && (hp - hex) + 4 < (int)sizeof hex; i++) {
            int written = snprintf(hp, sizeof hex - (size_t)(hp - hex),
                                   "%s%02X", i == 0 ? "" : " ",
                                   code[off + i]);
            if (written < 0) break;
            hp += written;
        }
        snprintf(line, sizeof line, "  %04lu: %-18s %s\n",
                 (unsigned long)off, hex, mnem);
        cl_write_cstring_to_stdout(line);
        /* A call site's LEA (d16,PC) points into the cell area after the
         * code: the lowest such target ends the instruction stream. */
        if ((((uint16_t)code[off] << 8 | code[off + 1]) & 0xF1FF) == 0x41FA &&
            off + 4 <= len) {
            int16_t d = (int16_t)(((uint16_t)code[off + 2] << 8) | code[off + 3]);
            uint32_t target = off + 2 + (uint32_t)(int32_t)d;
            if ((int32_t)d > 0 && target < cells) cells = target;
        }
        off += n;
    }
    if (cells & 3) cells += 2;                  /* the cells' alignment NOP */
    for (off = cells; off + 16 <= sites_end; off += 16) {
        snprintf(line, sizeof line,
                 "  site #%lu: gen=%lu func=$%08lx entry=$%08lx code=$%08lx\n",
                 (unsigned long)((off - cells) / 16),
                 (unsigned long)disasm_u32(code + off),
                 (unsigned long)disasm_u32(code + off + 4),
                 (unsigned long)disasm_u32(code + off + 8),
                 (unsigned long)disasm_u32(code + off + 12));
        cl_write_cstring_to_stdout(line);
    }
    for (off = 0; off < n_lines; off++) {
        uint32_t e = disasm_u32(code + table_off + 4 * off);
        snprintf(line, sizeof line, "  line: from %04lu ip %lu\n",
                 (unsigned long)(e >> 16), (unsigned long)(e & 0xFFFF));
        cl_write_cstring_to_stdout(line);
    }
}

/* Enter native code.  Every shape takes the positional ABI, with the
 * function-object CL_Obj passed over the arguments so OP_UPVAL and
 * OP_CELL_SET_UPVAL can read the active closure's upvalues[] from
 * inside JIT'd code (mirrors the VM's `frame->bytecode` channel).
 * Every caller has checked that the lambda list accepts nargs (OP_CALL's
 * bounds, jit_dispatch's fit rule), so the call is sound.  cl_jit_enter
 * (jit_enter_m68k.s) pushes `cl_vm.stack[sp - nargs ... sp - 1]` first
 * to last, then `func`: operand-stack order on the m68k stack — the last
 * argument at `8(sp)` after the JSR, the first one highest — which is
 * the order a native caller's operand stack already holds them in
 * (walker reads them via slot_disp's A6-relative offsets after LINK) —
 * and passes nargs in D1 for an &optional/&rest/&key callee's prologue.
 *
 * `func` is the original CL_Obj on the stack — a CL_Closure when
 * the callee was reached via a closure (the common case for any
 * defun-with-captures or any function bound through OP_CLOSURE),
 * or a raw CL_Bytecode pointer otherwise.  The OP_UPVAL helper
 * tolerates both: closure → return upvalues[index]; bytecode →
 * CL_NIL (matching the VM's else-branch).
 *
 * Either way the caller is responsible for popping the args and
 * function object after we return.
 *
 * m68k SysV ABI: D0 holds the return value on RTS; callee preserves
 * D2..D7 and A2..A6.  The current templates clobber only D0/D1/A0/A1
 * (caller-saved).  cl_jit_enter loads A3 = this thread's CL_Thread for
 * the native code and restores the caller's A3 on return.
 *
 * A positional count above CL_JIT_MAX_POSITIONAL returns CL_NIL
 * without calling — that shouldn't happen because cl_jit_compile
 * gatekeeps which shapes get native_code, but keep the defensive branch
 * so a future matcher mismatch surfaces as a wrong value rather than a
 * wild jump. */
CL_Obj cl_jit_invoke(CL_Obj func_obj, CL_Bytecode *bc, int nargs)
{
    CL_Obj result = CL_NIL;
    CL_Thread *t;
    int     prev_depth;
    void   *prev_top;
    char   *prev_floor;
    int   pushed_frame = 0;
    void *sp_slot = NULL;

    if (bc == NULL || bc->native_code == NULL) return CL_NIL;
    cl_jitc_invoke_count++;

    /* Mark this thread as inside JIT'd code so the GC knows to scan
     * the m68k stack conservatively and to skip moving compaction.
     * jit_stack_top is captured only on the OUTERMOST entry — that
     * frame's address bounds the scan window for all nested JIT
     * calls.  See specs/native-backend.md §"GC interaction".
     *
     * On longjmp out of JIT'd code (cl_error → cl_error_unwind), the
     * unwind path restores jit_depth from the CL_ErrorFrame snapshot
     * via cl_jit_restore_depth, which also keeps the global counter
     * in sync — so we do not need to wrap this in setjmp/CL_CATCH. */
    t = cl_get_current_thread();
    if (cl_jitc_shadow_frames && cl_vm.fp >= cl_vm.frame_size)
        cl_error(CL_ERR_OVERFLOW, "Call stack overflow");   /* OP_CALL's */
    prev_depth = t->jit_depth;
    prev_top   = t->jit_stack_top;
    prev_floor = t->jit_c_floor;
    if (prev_depth == 0) {
        extern volatile int cl_jit_active_threads;
        long headroom;
        t->jit_stack_top = CL_CAPTURE_SP();
        cl_jit_active_threads++;
        /* The C-stack floor native code compares A7 against (through A3)
         * before a direct call: once per outermost entry, so the per-call
         * FindTask of cl_check_c_stack is gone.  The 16K margin is
         * cl_check_c_stack's.  Headroom unknown -> NULL, never fires. */
        headroom = platform_stack_headroom();
        t->jit_c_floor = (headroom >= 0)
            ? (char *)t->jit_stack_top - headroom + 16384 : NULL;
    } else {
        /* A nested entry normally runs on the outermost one's stack, between
         * the floor and jit_stack_top.  Outside that window it is either a
         * Lisp callback on another task's stack (a hook run by input.device),
         * where a floor measured on a different stack means nothing, or this
         * stack is already below the floor.  Either way every call site must
         * miss -- the helper path measures the real stack -- so park the
         * floor at the top of the address space until this entry returns. */
        char *sp = (char *)CL_CAPTURE_SP();
        if (sp > (char *)t->jit_stack_top ||
            (t->jit_c_floor != NULL && sp <= t->jit_c_floor))
            t->jit_c_floor = (char *)~(uintptr_t)0;
    }
    t->jit_depth = prev_depth + 1;

    /* Push the function's CL_Frame so EXT:BACKTRACE / EXT:FRAME-LOCALS and
     * the error-time backtrace can see it (a direct call's hit path pushes
     * the same frame inline, emit_call_site).  On by default;
     * %JIT-SET-FRAMES NIL turns it off, and the call sites then stop
     * filling.  bp points at the argument vector still live on cl_vm.stack
     * (the VM stack is GC-rooted), so frame-locals can recover the
     * arguments; interior let-bound locals stay on the m68k stack and
     * remain invisible.  ip = 0 maps to the function's first source line.
     * `constants` is the native frame's marker (JIT_FRAME_SP), which
     * cl_jit_enter writes.  A full frame stack is the interpreter's "Call stack overflow", so
     * runaway native recursion stops at the same depth.  On a non-local
     * exit (longjmp) the error/NLX unwind resets cl_vm.fp wholesale to a
     * pre-call snapshot, so skipping the pop below cannot leak the frame. */
    if (cl_jitc_shadow_frames) {
        CL_Frame *sf = &cl_vm.frames[cl_vm.fp++];
        sf->bytecode  = func_obj;
        sf->code      = bc->code;
        sf->constants = NULL;
        sf->ip        = 0;
        sf->bp        = (uint32_t)(cl_vm.sp - nargs);
        sf->n_locals  = nargs;
        sf->nargs     = (uint16_t)nargs;
        sf->nlx_level = cl_nlx_top;
        sf->fslot     = 0;
        sp_slot       = &sf->constants;
        pushed_frame  = 1;
    }

    /* Positional ABI: the arguments are cl_vm.stack[sp-nargs .. sp-1] in
     * natural order; cl_jit_enter lays them out in operand-stack order
     * (parameter i at 12+4*(nargs-1-i)(a6)).  OP_CALL's u8 count bounds
     * nargs; the guard keeps a mismatch a wrong value rather than a wild
     * call. */
    if (nargs >= 0 && nargs <= CL_JIT_MAX_POSITIONAL)
        result = cl_jit_enter(bc->native_code, t, func_obj,
                              &cl_vm.stack[cl_vm.sp - nargs], (int32_t)nargs,
                              sp_slot);

    if (pushed_frame) cl_vm.fp--;
    t->jit_depth     = prev_depth;
    t->jit_stack_top = prev_top;
    if (prev_depth == 0) {
        extern volatile int cl_jit_active_threads;
        cl_jit_active_threads--;
    } else {
        t->jit_c_floor = prev_floor;
    }
    return result;
}

/* --- The line of a native frame (the backtrace's) ---------------------
 *
 * Native code never writes a frame's ip, so a native frame would always
 * show its function's first line.  The backtrace calls this first: for
 * every native frame of the current thread (JIT_FRAME_SP marks them and
 * gives each activation's A6) it finds the return address of the call
 * the function is in -- the call that leads to the next frame, or to the
 * helper that is running -- and stores the IP the line table
 * (emit_line_table) has for it.  Nothing is left in the frame that the
 * interpreter would read: it never resumes a native frame.
 *
 * The return address is the lowest word of the activation: everything
 * the function pushed lies between it and its frame.  So the scan goes
 * down from below the frame slots and the saved D5-D7 (the function's
 * first instruction, LINK a6,#size, gives where that is; a slot not yet
 * stored to still holds whatever was there before) and takes the first
 * word that is a return address into the function's code -- right after
 * a JSR -- stopping at the next deeper native frame's return-address slot
 * (or the resolver's own frame for the innermost one).  Every word it
 * passes, the operand stack and a helper's arguments, was written by this
 * activation; a stale word below the return address is never reached. */
static const CL_Bytecode *jit_frame_bytecode(const CL_Frame *f)
{
    CL_Obj o = f->bytecode;
    if (CL_CLOSURE_P(o)) o = ((CL_Closure *)CL_OBJ_TO_PTR(o))->bytecode;
    return CL_BYTECODE_P(o) ? (const CL_Bytecode *)CL_OBJ_TO_PTR(o) : NULL;
}

/* At most this far below the frame: an operand stack and a helper's
 * arguments are far smaller. */
#define JIT_LINE_SCAN_MAX 16384

static uint32_t jit_scan_frame_ip(const CL_Bytecode *bc, const char *a6,
                                  const char *lower)
{
    const uint8_t *code = bc->native_code;
    uint32_t code_end, table_off, n, k;
    const char *p;
    if (!jit_line_table(code, bc->native_len, &code_end, &table_off, &n) ||
        disasm_u16(code) != 0x4E56)                     /* link a6,#size */
        return 0;
    /* Below the frame slots and the saved D7/D6/D5 (walker_compile). */
    p = a6 + (int16_t)disasm_u16(code + 2) - 16;
    if (p - lower > JIT_LINE_SCAN_MAX) lower = p - JIT_LINE_SCAN_MAX;
    for (; p >= lower; p -= 4) {
        uint32_t off = *(const uint32_t *)p - (uint32_t)(uintptr_t)code;
        uint32_t ip = 0;
        if (off < 2 || off > code_end) continue;
        if ((disasm_u16(code + off - 2) & 0xFFF8) != 0x4E90 &&    /* jsr (An) */
            !(off >= 6 && disasm_u16(code + off - 6) == 0x4EB9))  /* jsr abs.l */
            continue;
        for (k = 0; k < n; k++) {
            uint32_t e = disasm_u32(code + table_off + 4 * k);
            if ((e >> 16) > off - 1) break;
            ip = e & 0xFFFF;
        }
        return ip;
    }
    return 0;
}

void cl_jit_resolve_frame_ips(void)
{
    CL_Thread *t = cl_get_current_thread();
    const char *top = (const char *)t->jit_stack_top;
    const char *lower = (const char *)CL_CAPTURE_SP();
    int i;
    /* Not in native code, or in a nested entry on another task's stack
     * (cl_jit_invoke parks the floor): no activation to read. */
    if (t->jit_depth == 0 || top == NULL || lower > top ||
        t->jit_c_floor == (char *)~(uintptr_t)0)
        return;
    for (i = cl_vm.fp - 1; i >= 0; i--) {
        CL_Frame *f = &cl_vm.frames[i];
        const char *sp = (const char *)f->constants;
        const CL_Bytecode *bc;
        if (sp <= lower || sp > top || ((uintptr_t)sp & 1)) continue;
        bc = jit_frame_bytecode(f);
        if (bc == NULL || bc->native_code == NULL) continue;
        f->ip = jit_scan_frame_ip(bc, sp - 8, lower);
        lower = sp - 4;
    }
}

/* Every m68k code buffer is platform_alloc'd and goes back with its
 * function (cl_jit_free_native); nothing else is mapped. */
void cl_jit_backend_shutdown(void)
{
}

/* m68k native code is platform_alloc'd (cb_finish). */
void cl_jit_free_native(void *code)
{
    platform_free(code);
}

int cl_jit_emit_stub(CL_Bytecode *bc)
{
    CodeBuf cb;
    uint8_t *code;
    uint32_t len;

    if (bc == NULL) return 0;

    cb_init(&cb, 8);
    m68k_emit_nop(&cb);
    m68k_emit_rts(&cb);
    code = cb_finish(&cb, &len);
    if (code == NULL) return 0;

    /* The stub bakes no heap immediates — drop any reloc table the prior
     * native code carried so the compactor doesn't patch the stub. */
    if (bc->native_relocs) platform_free(bc->native_relocs);
    bc->native_relocs = NULL;
    bc->native_reloc_count = 0;
    platform_cache_clear(code, len);
    {
        /* Swap in the stub, then bump, then free: a call site may cache
         * the old entry (see cl_jit_backend_compile). */
        void *old_code = bc->native_code;
        bc->native_code = code;
        bc->native_len  = len;
        if (old_code) {
            cl_call_gen_bump("native code replaced");
            platform_free(old_code);
        }
    }
    return 1;
}

#endif /* JIT_M68K */
