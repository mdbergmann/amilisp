/*
 * Bytecode peephole post-pass (spec 1.8, superinstructions spec 4.3) —
 * runs at (optimize (speed >= 1)), i.e. everywhere but (speed 0).
 *
 * Runs over a function's finished bytecode after the single emit pass, as a
 * decode -> rewrite -> re-encode pipeline:
 *
 *   1. DECODE the flat byte buffer into an instruction list, resolving every
 *      jump/NLX-landing offset to an instruction INDEX and marking each
 *      branch target as a basic-block boundary.
 *   2. REWRITE patterns on the instruction list (never across a boundary):
 *        - jump-to-jump threading      JMP/JNIL/JTRUE -> JMP t   ==> direct t
 *        - jump-to-return              JMP -> RET                ==> RET
 *        - jump-to-next                JMP L; L:                 ==> -
 *        - return site                 STORE n; POP; JMP -> LOAD n; RET ==> RET
 *        - dead code                   unreachable after JMP/RET/... removed
 *        - store-then-reload           STORE n; POP; LOAD n      ==> STORE n
 *        - dead store before return    STORE n; [MV_RESET;] RET  ==> [MV_RESET;] RET
 *        - pure-push + pop elision     CONST/NIL/T/LOAD/DUP/UPVAL; POP ==> -
 *        - not-branch fusion           NOT; JNIL t               ==> JTRUE t
 *      then, once the above reach a fixpoint, FUSE adjacent pairs into the
 *      superinstructions of opcodes.h (one dispatch instead of two, same
 *      semantics — see peep_fuse for the table).  Fusion runs last so the
 *      deleting rewrites see the compiler's own opcodes only.
 *   3. RE-ENCODE, recomputing all relative offsets from scratch and
 *      remapping the pc-keyed source line table.
 *
 * FAIL-SAFE BY DESIGN: any byte the decoder does not recognize — an opcode
 * missing from CL_OPCODE_LIST, a jump landing between instructions, an
 * OP_CLOSURE whose template constant is not bytecode, an over-budget
 * function, an allocation failure — aborts the pass and leaves the original
 * bytecode untouched.  A new opcode someone forgets to classify costs a
 * missed optimization, never a miscompile.
 *
 * SEMANTIC GUARDS:
 *   - Only CL_OPF_PURE opcodes may be deleted with their OP_POP: they push
 *     exactly one value and cannot signal.  OP_CAR/arith/etc. must keep
 *     signaling type errors at any speed (ANSI: the error is observable even
 *     when the value is discarded).
 *   - cl_mv_count: CONST/NIL/T/UPVAL write it; LOAD/DUP/POP do not.  Deleting
 *     a writing pair is only allowed when a scan forward inside the basic
 *     block reaches another unconditional mv-writer before anything that
 *     observes mv state (MV_LOAD/MV_TO_LIST/NTH_VALUE/RET/CALL/...), so the
 *     deleted write is provably masked.  Same scan gates dropping OP_NOT in
 *     the not-branch fusion; when the scan can't prove it, the fusion keeps
 *     the mv write by substituting OP_MV_RESET (same 1-byte length).
 *   - NLX landing pads (CATCH/UWPROT/BLOCK_PUSH/TAGBODY_PUSH offsets) are
 *     treated exactly like jump targets; their offsets are re-encoded, but
 *     they are never threaded.  A HANDLER_CASE_PUSH landing is a TABLE of
 *     one 5-byte JMP per clause that C indexes by clause number, so every
 *     entry is pinned: a permanent target that keeps its shape.
 *   - Backward-jump GC safepoints survive: offsets are recomputed from final
 *     positions, so a real loop's back edge stays a backward jump.
 *
 * The pass allocates NO Lisp objects — it cannot trigger GC, so raw
 * CL_Bytecode pointers read from the constant pool (OP_CLOSURE templates)
 * cannot go stale while it runs.
 *
 * Debug: build with -DDEBUG_PEEPHOLE for per-function rewrite statistics.
 */

#include <string.h>
#ifdef DEBUG_PEEPHOLE
#include <stdio.h>
#endif

#include "types.h"
#include "opcodes.h"
#include "peephole.h"
#include "compiler.h"
#include "compiler_internal.h"
#include "../platform/platform.h"

/* --- Opcode info table (single source of truth: CL_OPCODE_LIST) --- */

static const CL_OpcodeInfo cl_opcode_table[256] = {
#define CL_OPCODE_TABLE_ENTRY(name, value, str, opnd, fl) \
    [value] = { str, (uint8_t)(opnd), (uint8_t)(fl) },
    CL_OPCODE_LIST(CL_OPCODE_TABLE_ENTRY)
#undef CL_OPCODE_TABLE_ENTRY
};

const CL_OpcodeInfo *cl_opcode_info(uint8_t op)
{
    const CL_OpcodeInfo *info = &cl_opcode_table[op];
    return info->name ? info : NULL;
}

int cl_opnd_len(uint8_t kind)
{
    switch ((CL_OperandKind)kind) {
    case CL_OPND_NONE:      return 0;
    case CL_OPND_U8:        return 1;
    case CL_OPND_U16:       return 2;
    case CL_OPND_JREL:      return 4;
    case CL_OPND_U16_JREL:  return 6;
    case CL_OPND_U16_U16:   return 4;
    case CL_OPND_U16_U8:    return 3;
    case CL_OPND_AMIGA:     return 9;
    case CL_OPND_U8_U8:     return 2;
    case CL_OPND_U8_U16_U8: return 4;
    case CL_OPND_U8_JREL:   return 5;
    case CL_OPND_U8_U16:    return 3;
    case CL_OPND_U16_U16_U8: return 5;
    case CL_OPND_CLOSURE:   return -1;
    }
    return -1;
}

int cl_opnd_jrel_pos(uint8_t kind)
{
    switch ((CL_OperandKind)kind) {
    case CL_OPND_JREL:     return 1;
    case CL_OPND_U16_JREL: return 3;
    case CL_OPND_U8_JREL:  return 2;
    default:               return 0;
    }
}

/* --- Instruction list --- */

#define PEEP_DELETED 0x01   /* instruction removed; jumps into it retarget forward */
#define PEEP_TARGET  0x02   /* branch/NLX-landing target: basic-block boundary */
#define PEEP_FUSED   0x04   /* deleted because a preceding head absorbed it */
#define PEEP_PINNED  0x08   /* an entry of a HANDLER-CASE landing table: always
                               a target (C enters it at landing + 5*k), and it
                               must stay a 5-byte OP_JMP */

typedef struct {
    uint32_t old_pos;     /* byte offset of the opcode in the input buffer */
    uint32_t new_pos;     /* byte offset in the output buffer (re-encode) */
    const uint8_t *raw;   /* points at the opcode byte in the input buffer */
    uint16_t raw_len;     /* total instruction length in bytes (a fused
                             instruction: 1 + all members' operand bytes) */
    int32_t jump_target;  /* instruction index for a jump/landing operand, else -1 */
    int32_t fused_with;   /* index of the next member folded into this one
                             (its operand bytes follow ours), else -1 */
    uint16_t own_len;     /* this instruction's operand byte count as
                             decoded (fusion changes raw_len, not this; the
                             JMP -> RET rewrite zeroes both) */
    uint8_t op;           /* current opcode (patterns may rewrite it) */
    uint8_t flags;        /* PEEP_* */
    uint8_t synth_len;    /* 1 when a fusion synthesized an operand byte
                             (CMP_BR's kind) that precedes the members'
                             operand bytes; raw_len counts it */
    uint8_t synth;        /* that byte */
} PeepInsn;

typedef struct {
    PeepInsn *insns;
    int32_t count;
    const CL_Obj *constants;
    int32_t n_constants;
} PeepCode;

/* Instruction byte length for re-encoding.  Everything keeps its decoded
 * length except opcode substitutions (1-byte <-> 1-byte) and fusions,
 * which set raw_len to the fused length when they fire. */
static uint32_t peep_insn_len(const PeepInsn *in)
{
    return in->raw_len;
}

/* Binary search: instruction index whose old_pos == pos, or -1. */
static int32_t peep_index_of_pos(const PeepCode *pc, uint32_t pos)
{
    int32_t lo = 0, hi = pc->count - 1;
    while (lo <= hi) {
        int32_t mid = lo + (hi - lo) / 2;
        if (pc->insns[mid].old_pos == pos) return mid;
        if (pc->insns[mid].old_pos < pos) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

static uint16_t peep_read_u16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static int32_t peep_read_i32(const uint8_t *p)
{
    return (int32_t)(((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                     ((uint32_t)p[2] << 8) | (uint32_t)p[3]);
}

static void peep_write_i32(uint8_t *p, int32_t v)
{
    p[0] = (uint8_t)((v >> 24) & 0xFF);
    p[1] = (uint8_t)((v >> 16) & 0xFF);
    p[2] = (uint8_t)((v >> 8) & 0xFF);
    p[3] = (uint8_t)(v & 0xFF);
}

/* --- Decode --- */

/* Decode CODE[0..LEN) into PC->insns.  Returns 1 on success, 0 to bail out
 * (unknown opcode, truncated operands, undecodable OP_CLOSURE). */
int cl_bytecode_has_backward_jump(const uint8_t *code, uint32_t len,
                                  const CL_Obj *constants, int n_constants)
{
    uint32_t ip = 0;

    if (code == NULL) return 0;
    while (ip < len) {
        const CL_OpcodeInfo *info = cl_opcode_info(code[ip]);
        uint32_t opnd_len;
        int jpos;

        if (!info) return 0;
        if (info->operands == CL_OPND_CLOSURE) {
            /* u16 template const index + 2 bytes per template upvalue. */
            uint16_t idx;
            CL_Obj tmpl;
            if (ip + 3 > len) return 0;
            idx = peep_read_u16(code + ip + 1);
            if (idx >= n_constants || !constants) return 0;
            tmpl = constants[idx];
            if (!CL_BYTECODE_P(tmpl)) return 0;
            opnd_len = 2 + 2u * ((const CL_Bytecode *)CL_OBJ_TO_PTR(tmpl))->n_upvalues;
        } else {
            int fixed = cl_opnd_len(info->operands);
            if (fixed < 0) return 0;
            opnd_len = (uint32_t)fixed;
        }
        if (ip + 1 + opnd_len > len) return 0;
        jpos = cl_opnd_jrel_pos(info->operands);
        /* The offset counts from the next instruction: negative = back. */
        if (jpos && peep_read_i32(code + ip + jpos) < 0)
            return 1;
        ip += 1 + opnd_len;
    }
    return 0;
}

static int peep_decode(PeepCode *pc, const uint8_t *code, uint32_t len)
{
    uint32_t ip = 0;
    int32_t n = 0;

    while (ip < len) {
        const CL_OpcodeInfo *info = cl_opcode_info(code[ip]);
        PeepInsn *in = &pc->insns[n];
        uint32_t opnd_len;

        if (!info) return 0;  /* unknown opcode — bail */

        if (info->operands == CL_OPND_CLOSURE) {
            /* u16 template const index + 2 bytes per template upvalue.
             * Needs the constant pool; anything unexpected -> bail. */
            uint16_t idx;
            CL_Obj tmpl;
            const CL_Bytecode *tmpl_bc;
            if (ip + 3 > len) return 0;
            idx = peep_read_u16(code + ip + 1);
            if (idx >= pc->n_constants || !pc->constants) return 0;
            tmpl = pc->constants[idx];
            if (!CL_BYTECODE_P(tmpl)) return 0;
            tmpl_bc = (const CL_Bytecode *)CL_OBJ_TO_PTR(tmpl);
            opnd_len = 2 + 2u * tmpl_bc->n_upvalues;
        } else {
            int fixed = cl_opnd_len(info->operands);
            if (fixed < 0) return 0;
            opnd_len = (uint32_t)fixed;
        }

        if (ip + 1 + opnd_len > len) return 0;  /* truncated — bail */

        in->old_pos = ip;
        in->new_pos = 0;
        in->raw = code + ip;
        in->raw_len = (uint16_t)(1 + opnd_len);
        in->jump_target = -1;
        in->fused_with = -1;
        in->own_len = (uint16_t)opnd_len;
        in->op = code[ip];
        in->flags = 0;
        in->synth_len = 0;
        in->synth = 0;
        ip += 1 + opnd_len;
        n++;
    }

    pc->count = n;

    /* Resolve jump/NLX-landing offsets to instruction indices. */
    {
        int32_t i;
        for (i = 0; i < pc->count; i++) {
            PeepInsn *in = &pc->insns[i];
            const CL_OpcodeInfo *info = cl_opcode_info(in->op);
            int jpos = cl_opnd_jrel_pos(info->operands);
            int32_t off;
            uint32_t tpos;
            int32_t tidx;

            if (!jpos) continue;
            off = peep_read_i32(in->raw + jpos);

            /* Offsets are relative to the first byte after the instruction. */
            tpos = in->old_pos + in->raw_len + (uint32_t)off;
            if (tpos > len) return 0;
            if (tpos == len) {
                /* Jump to end-of-code: legal only as "fall off the end"
                 * target; be conservative and bail (compiler never emits
                 * it — a final RET/HALT always exists). */
                return 0;
            }
            tidx = peep_index_of_pos(pc, tpos);
            if (tidx < 0) return 0;  /* lands mid-instruction — bail */
            in->jump_target = tidx;
        }
    }

    /* HANDLER-CASE landing tables.  OP_HANDLER_CASE_PUSH's landing is the
     * first of N consecutive 5-byte OP_JMPs (N = length of the clause type
     * list in its u16 constant); cl_handler_case_transfer enters entry k at
     * landing + 5*k, so from the bytecode's own control flow every entry but
     * the first looks unreachable.  Pin them: each is a permanent target
     * (never dead, never absorbed into a pattern) and keeps its shape (the
     * JMP -> RET rewrite skips it; threading keeps it a JMP).  Anything
     * unexpected — no constant pool, a non-list, a table entry that is not
     * a JMP — bails the pass. */
    {
        int32_t i;
        for (i = 0; i < pc->count; i++) {
            PeepInsn *in = &pc->insns[i];
            CL_Obj types;
            int32_t n = 0, k;
            uint16_t idx;
            if (in->op != OP_HANDLER_CASE_PUSH) continue;
            if (!pc->constants) return 0;
            idx = peep_read_u16(in->raw + 1);
            if (idx >= pc->n_constants) return 0;
            types = pc->constants[idx];
            while (CL_CONS_P(types)) { n++; types = cl_cdr(types); }
            if (!CL_NULL_P(types)) return 0;
            if (in->jump_target < 0 || in->jump_target + n > pc->count) return 0;
            for (k = 0; k < n; k++) {
                PeepInsn *e = &pc->insns[in->jump_target + k];
                if (e->op != OP_JMP || e->raw_len != 5) return 0;
                e->flags |= PEEP_PINNED;
            }
        }
    }
    return 1;
}

/* Recompute PEEP_TARGET flags from all live jumps/NLX landings; a pinned
 * landing-table entry is a target regardless. */
static void peep_mark_targets(PeepCode *pc)
{
    int32_t i;
    for (i = 0; i < pc->count; i++) {
        pc->insns[i].flags &= (uint8_t)~PEEP_TARGET;
        if (pc->insns[i].flags & PEEP_PINNED)
            pc->insns[i].flags |= PEEP_TARGET;
    }
    for (i = 0; i < pc->count; i++) {
        PeepInsn *in = &pc->insns[i];
        if (in->flags & PEEP_DELETED) continue;
        if (in->jump_target >= 0)
            pc->insns[in->jump_target].flags |= PEEP_TARGET;
    }
}

/* --- mv-state scan guard ---
 *
 * May the mv_count write of a deleted/dropped instruction be observed?
 * Scan straight-line code forward from FROM: if we reach an unconditional
 * mv-writer first, the write is masked (return 1 = provably dead).
 * Anything that observes mv state, branches, or ends the function means we
 * can't prove it (return 0).  Join points (PEEP_TARGET) are handled
 * conservatively: a straight-line scan stays sound across a join, but we
 * fail there anyway to keep the reasoning local — except at the scan's
 * entry instruction when ALLOW_ENTRY_TARGET is set (used for scanning a
 * branch's own landing point).  The TARGET flag is honored even on deleted
 * instructions: a jump that landed on a deleted pattern-pair retargets to
 * the following live instruction, which therefore sits on a join. */
static int peep_mv_write_is_dead(const PeepCode *pc, int32_t from,
                                 int allow_entry_target)
{
    int32_t i;
    int entry = 1;
    for (i = from; i < pc->count; i++) {
        const PeepInsn *in = &pc->insns[i];
        const CL_OpcodeInfo *info;
        if ((in->flags & PEEP_TARGET) && !(entry && allow_entry_target))
            return 0;                            /* join point */
        entry = 0;
        if (in->flags & PEEP_DELETED) continue;
        info = cl_opcode_info(in->op);
        if (info->flags & CL_OPF_MVR) return 0;  /* observer first — bail */
        if (info->flags & CL_OPF_MVW) return 1;  /* masked — provably dead */
        if (info->flags & CL_OPF_UNCOND) return 0;
        if (in->jump_target >= 0) return 0;      /* conditional branch */
    }
    return 0;
}

/* Next non-deleted instruction index after I, or pc->count. */
static int32_t peep_next_live(const PeepCode *pc, int32_t i)
{
    i++;
    while (i < pc->count && (pc->insns[i].flags & PEEP_DELETED)) i++;
    return i;
}

/* Like peep_next_live, but additionally reports whether the step crossed a
 * branch target — either a skipped deleted instruction carrying PEEP_TARGET
 * (a jump retargets through it onto the result) or the resulting live
 * instruction itself.  Multi-instruction patterns must reject such steps:
 * a jump landing INSIDE a pattern would execute only its tail, so the
 * members are not equivalent to a straight-line pair. */
static int32_t peep_next_live_unjoined(const PeepCode *pc, int32_t i,
                                       int *crossed_target)
{
    *crossed_target = 0;
    i++;
    while (i < pc->count && (pc->insns[i].flags & PEEP_DELETED)) {
        if (pc->insns[i].flags & PEEP_TARGET) *crossed_target = 1;
        i++;
    }
    if (i < pc->count && (pc->insns[i].flags & PEEP_TARGET))
        *crossed_target = 1;
    return i;
}

/* --- Rewrite patterns (each returns number of rewrites applied) --- */

/* Jump-to-jump threading: a JMP/JNIL/JTRUE whose target is an OP_JMP jumps
 * directly to that JMP's target.  NLX landing offsets are never threaded.
 * Chain chase is capped to stay clear of jump cycles.  A target that was
 * deleted stands for the next live instruction (that is where the encoder
 * retargets it), so the chase looks through deletions.
 *
 * JMP -> RET becomes RET: the return is one byte and unconditional, so
 * duplicating it at the jump site saves the dispatch and the MV state
 * flows into it unchanged (JMP writes nothing).  Every function's block
 * result lands this way — `STORE slot; JMP end` with `end: STORE slot;
 * RET` — and once the landing's dead STORE goes (peep_dead_store_ret) the
 * JMP's live target IS the RET. */
static int peep_thread_jumps(PeepCode *pc)
{
    int changed = 0;
    int32_t i;
    for (i = 0; i < pc->count; i++) {
        PeepInsn *in = &pc->insns[i];
        int hops;
        if (in->flags & PEEP_DELETED) continue;
        if (in->op != OP_JMP && in->op != OP_JNIL && in->op != OP_JTRUE)
            continue;
        if (in->jump_target < 0) continue;
        for (hops = 0; hops < 16; hops++) {
            int32_t t = in->jump_target;
            PeepInsn *ti;
            while (t < pc->count && (pc->insns[t].flags & PEEP_DELETED)) t++;
            if (t >= pc->count) break;
            ti = &pc->insns[t];
            if (in->op == OP_JMP && ti->op == OP_RET &&
                !(in->flags & PEEP_PINNED)) {
                in->op = OP_RET;
                in->jump_target = -1;
                in->raw_len = 1;
                in->own_len = 0;
                changed++;
                break;
            }
            if (ti->op != OP_JMP || ti->jump_target < 0) break;
            if (ti->jump_target == in->jump_target) break;  /* self-loop */
            in->jump_target = ti->jump_target;
            changed++;
        }
    }
    return changed;
}

/* JMP L  with  L the next live instruction  ==>  (deleted)
 * A jump to the instruction that follows it does nothing: the compiler
 * emits one at the end of a CASE/COND arm that happens to be the last
 * (the exit chain lands right after it) and at a LOOP prologue.  Never a
 * backward jump, so no safepoint is lost; a pinned table entry keeps its
 * shape. */
static int peep_jump_to_next(PeepCode *pc)
{
    int changed = 0;
    int32_t i;
    for (i = 0; i < pc->count; i++) {
        PeepInsn *in = &pc->insns[i];
        int32_t t, n;
        if (in->flags & PEEP_DELETED) continue;
        if (in->op != OP_JMP || in->jump_target < 0) continue;
        if (in->flags & PEEP_PINNED) continue;
        t = in->jump_target;
        while (t < pc->count && (pc->insns[t].flags & PEEP_DELETED)) t++;
        n = peep_next_live(pc, i);
        if (t != n || n >= pc->count) continue;
        in->flags |= PEEP_DELETED;
        in->jump_target = -1;
        changed++;
    }
    return changed;
}

/* STORE n; [POP;] JMP L   with  L: LOAD n; RET            ==>  RET
 * STORE n; [POP;] JMP L   with  L: LOAD n; MV_RESET; RET  ==>  MV_RESET; RET
 * The site of a local RETURN-FROM / RETURN: the compiler stores the value
 * into the block's result slot, pops it and jumps to the landing, which
 * reloads the slot and returns.  OP_STORE peeks, so with the STORE and the
 * POP gone the value is on top at the site — return it from there (the JMP
 * becomes the RET; with an MV_RESET landing the STORE becomes that reset).
 * Once no site jumps to the landing's LOAD any more, the fall-through path
 * `value; STORE n; POP; LOAD n; RET` collapses through the store-reload and
 * dead-store rules on the following rounds.  Neither the POP nor the JMP
 * may be a target: another path reaching them would have to guarantee the
 * value on top as well.  The STORE may be one (that path was about to
 * store the value it has on top). */
static int peep_return_site(PeepCode *pc)
{
    int changed = 0;
    int32_t i;
    for (i = 0; i < pc->count; i++) {
        PeepInsn *s = &pc->insns[i];
        PeepInsn *j, *l, *p = NULL;
        int32_t ji, t, ri;
        int joined, reset = 0;
        if (s->flags & PEEP_DELETED) continue;
        if (s->op != OP_STORE) continue;
        ji = peep_next_live_unjoined(pc, i, &joined);
        if (ji >= pc->count || joined) continue;
        j = &pc->insns[ji];
        if (j->op == OP_POP) {
            p = j;
            ji = peep_next_live_unjoined(pc, ji, &joined);
            if (ji >= pc->count || joined) continue;
            j = &pc->insns[ji];
        }
        if (j->op != OP_JMP || j->jump_target < 0) continue;
        t = j->jump_target;
        while (t < pc->count && (pc->insns[t].flags & PEEP_DELETED)) t++;
        if (t >= pc->count) continue;
        l = &pc->insns[t];
        if (l->op != OP_LOAD || l->raw[1] != s->raw[1]) continue;
        ri = peep_next_live(pc, t);
        if (ri < pc->count && pc->insns[ri].op == OP_MV_RESET) {
            reset = 1;
            ri = peep_next_live(pc, ri);
        }
        if (ri >= pc->count || pc->insns[ri].op != OP_RET) continue;
        if (reset) {
            s->op = OP_MV_RESET;
            s->raw_len = 1;
            s->own_len = 0;
        } else {
            s->flags |= PEEP_DELETED;
        }
        if (p) p->flags |= PEEP_DELETED;
        j->op = OP_RET;
        j->jump_target = -1;
        j->raw_len = 1;
        j->own_len = 0;
        changed++;
    }
    return changed;
}

/* STORE n; RET  ==>  RET      (also STORE n; MV_RESET; RET)
 * OP_STORE peeks, so the value RET returns is unchanged; the local it
 * wrote dies with the frame, and MV_RESET reads no local.  The compiler
 * emits this at the end of every function: a DEFUN body is a BLOCK whose
 * result slot the tail form is stored into, for the RETURN-FROM landing's
 * sake, whether or not a RETURN-FROM exists.  No join check is needed: a
 * jump landing on the RET (or the MV_RESET) skipped the store on that
 * path, and a jump landing on the STORE retargets past it. */
static int peep_dead_store_ret(PeepCode *pc)
{
    int changed = 0;
    int32_t i;
    for (i = 0; i < pc->count; i++) {
        PeepInsn *s = &pc->insns[i];
        int32_t ni;
        if (s->flags & PEEP_DELETED) continue;
        if (s->op != OP_STORE) continue;
        ni = peep_next_live(pc, i);
        if (ni < pc->count && pc->insns[ni].op == OP_MV_RESET)
            ni = peep_next_live(pc, ni);
        if (ni >= pc->count || pc->insns[ni].op != OP_RET) continue;
        s->flags |= PEEP_DELETED;
        changed++;
    }
    return changed;
}

/* Dead code: instructions that can only be reached by falling through an
 * UNCOND instruction (JMP/RET/TAILCALL/HALT/BLOCK_RETURN/TAGBODY_GO) and are
 * not branch targets.  Requires peep_mark_targets to be current. */
static int peep_dead_code(PeepCode *pc)
{
    int changed = 0;
    int reachable = 1;
    int32_t i;
    for (i = 0; i < pc->count; i++) {
        PeepInsn *in = &pc->insns[i];
        const CL_OpcodeInfo *info;
        if (in->flags & PEEP_TARGET) reachable = 1;
        if (in->flags & PEEP_DELETED) continue;
        if (!reachable) {
            in->flags |= PEEP_DELETED;
            in->jump_target = -1;
            changed++;
            continue;
        }
        info = cl_opcode_info(in->op);
        if (info->flags & CL_OPF_UNCOND) reachable = 0;
    }
    return changed;
}

/* STORE n; POP; LOAD n  ==>  STORE n
 * OP_STORE peeks (locals[n] = TOS without popping), so the reloaded value is
 * exactly the value STORE left on top — POP+LOAD is a no-op round trip.
 * POP and LOAD write no mv state, so no mv guard is needed. */
static int peep_store_reload(PeepCode *pc)
{
    int changed = 0;
    int32_t i;
    for (i = 0; i < pc->count; i++) {
        PeepInsn *s = &pc->insns[i];
        int32_t pi, li;
        int joined;
        PeepInsn *p, *l;
        if (s->flags & PEEP_DELETED) continue;
        if (s->op != OP_STORE) continue;
        pi = peep_next_live_unjoined(pc, i, &joined);
        if (pi >= pc->count || joined) continue;
        p = &pc->insns[pi];
        if (p->op != OP_POP) continue;
        li = peep_next_live_unjoined(pc, pi, &joined);
        if (li >= pc->count || joined) continue;
        l = &pc->insns[li];
        if (l->op != OP_LOAD) continue;
        if (s->raw[1] != l->raw[1]) continue;  /* different slot */
        p->flags |= PEEP_DELETED;
        l->flags |= PEEP_DELETED;
        changed++;
    }
    return changed;
}

/* <pure-push>; POP  ==>  (nothing)
 * Only CL_OPF_PURE opcodes qualify (no side effects, cannot signal).  When
 * the push writes mv state (CONST/NIL/T/UPVAL), deletion additionally
 * requires the mv-scan proof; LOAD/DUP write no mv state and always
 * qualify. */
static int peep_pure_pop(PeepCode *pc)
{
    int changed = 0;
    int32_t i;
    for (i = 0; i < pc->count; i++) {
        PeepInsn *u = &pc->insns[i];
        const CL_OpcodeInfo *info;
        int32_t pi;
        int joined;
        PeepInsn *p;
        if (u->flags & PEEP_DELETED) continue;
        info = cl_opcode_info(u->op);
        if (!(info->flags & CL_OPF_PURE)) continue;
        pi = peep_next_live_unjoined(pc, i, &joined);
        if (pi >= pc->count || joined) continue;
        p = &pc->insns[pi];
        if (p->op != OP_POP) continue;
        if ((info->flags & CL_OPF_MVW) &&
            !peep_mv_write_is_dead(pc, peep_next_live(pc, pi), 0))
            continue;
        u->flags |= PEEP_DELETED;
        p->flags |= PEEP_DELETED;
        changed++;
    }
    return changed;
}

/* NOT; JNIL t  ==>  JTRUE t     (and NOT; JTRUE t ==> JNIL t)
 * OP_NOT pops v and pushes its negation; the following conditional pops that
 * negation — fusing inverts the branch sense on the original value.  OP_NOT
 * also writes cl_mv_count: the write must be provably dead on BOTH
 * successor paths (fallthrough and branch target) to drop it outright;
 * otherwise it is preserved by substituting OP_MV_RESET (1 byte, same as
 * OP_NOT), which still removes the negate + pop/push round trip. */
static int peep_not_branch(PeepCode *pc)
{
    int changed = 0;
    int32_t i;
    for (i = 0; i < pc->count; i++) {
        PeepInsn *n = &pc->insns[i];
        int32_t ji;
        int joined;
        PeepInsn *j;
        if (n->flags & PEEP_DELETED) continue;
        if (n->op != OP_NOT) continue;
        ji = peep_next_live_unjoined(pc, i, &joined);
        if (ji >= pc->count || joined) continue;
        j = &pc->insns[ji];
        if (j->op != OP_JNIL && j->op != OP_JTRUE) continue;
        j->op = (j->op == OP_JNIL) ? OP_JTRUE : OP_JNIL;
        if (peep_mv_write_is_dead(pc, peep_next_live(pc, ji), 0) &&
            j->jump_target >= 0 &&
            peep_mv_write_is_dead(pc, j->jump_target, 1)) {
            n->flags |= PEEP_DELETED;
        } else {
            n->op = OP_MV_RESET;  /* keep the mv_count=1 write, drop the negate */
        }
        changed++;
    }
    return changed;
}

/* --- Superinstruction fusion (spec 4.3) ---
 *
 * One greedy left-to-right pass over the final instruction stream.  Each
 * rule folds an adjacent pair (head, next) into one opcode whose vm.c
 * handler is exactly the pair — same stack effect, same MV effect, same
 * errors — so no dataflow reasoning is needed here.  The only guard is the
 * basic-block one: a jump landing on the second member would execute only
 * the tail, so a pair whose second member is a branch target is never
 * fused (peep_next_live_unjoined).  The head keeps its place in the list
 * (its old_pos, its line entry and any branch landing on it are unaffected);
 * the absorbed member is marked deleted + PEEP_FUSED and chained through
 * fused_with, so the encoder emits its operand bytes after the head's and
 * the line remap attributes it to the head.  A branch operand moves from
 * the member to the head (jump_target); the encoder rewrites it at the
 * fused shape's offset.
 *
 * The scan resumes after the absorbed member, so a fused instruction is
 * never itself a head (LOAD a; LOAD b; LOAD c -> LOAD_LOAD a b; LOAD c).
 *
 * Triples (tried first at a head that starts one):
 *   LOAD a; STORE b; POP       -> LOAD_STORE_POP a b
 *   GLOAD sym; EQ; JNIL t      -> GLOAD_EQ_JNIL sym t
 * Pairs:
 *   STORE n; POP               -> STORE_POP n
 *   LOAD a; LOAD b             -> LOAD_LOAD a b
 *   LOAD s; CONST k            -> LOAD_CONST s k
 *   LOAD s; CALL_GLOBAL sym n  -> LOAD_CALL_GLOBAL s sym n
 *   LOAD s; STRUCT_REF i       -> LOAD_STRUCT_REF s i
 *   LOAD s; MV_RESET           -> LOAD_MV_RESET s
 *   LOAD s; RET                -> LOAD_RET s
 *   LOAD s; JNIL t             -> LOAD_JNIL s t
 *   POP; LOAD s                -> POP_LOAD s
 *   EQ; JNIL t                 -> EQ_JNIL t
 *   GLOAD sym; JNIL t          -> GLOAD_JNIL sym t
 *   GLOAD sym; CALL_GLOBAL f n -> GLOAD_CALL_GLOBAL sym f n
 *   LT|GT|LE|GE|NUMEQ|CHAREQ; JNIL t | JTRUE t
 *                              -> CMP_BR kind t   (spec 4.4; the kind byte
 *                                 is synthesized: the comparison in bits
 *                                 0-2, bit 3 set for the JTRUE shape)
 *
 * The table is the opcode-pair profile of the sento message path
 * (docs/benchmarks.md 2026-09-09): the first nine pairs were 31% of all
 * dispatches, the five added after re-profiling the fused stream another
 * 10% of what remained.  AMILISP_NO_FUSE=1 (main.c) leaves the pairs
 * alone — A/B runs, bisects. */

int cl_peephole_fuse_enabled = 1;

static uint8_t peep_fused_opcode3(uint8_t a, uint8_t b, uint8_t c)
{
    if (a == OP_LOAD && b == OP_STORE && c == OP_POP) return OP_LOAD_STORE_POP;
    if (a == OP_GLOAD && b == OP_EQ && c == OP_JNIL) return OP_GLOAD_EQ_JNIL;
    return 0;
}

/* CMP_BR's synthesized kind byte for the pair (a, b), or -1 when the pair
 * is not a comparison followed by a conditional branch. */
static int peep_cmp_br_kind(uint8_t a, uint8_t b)
{
    int cmp;
    switch (a) {
    case OP_LT:     cmp = CL_CMP_BR_LT; break;
    case OP_GT:     cmp = CL_CMP_BR_GT; break;
    case OP_LE:     cmp = CL_CMP_BR_LE; break;
    case OP_GE:     cmp = CL_CMP_BR_GE; break;
    case OP_NUMEQ:  cmp = CL_CMP_BR_NUMEQ; break;
    case OP_CHAREQ: cmp = CL_CMP_BR_CHAREQ; break;
    default:        return -1;
    }
    if (b == OP_JNIL)  return cmp;
    if (b == OP_JTRUE) return cmp | CL_CMP_BR_IF_TRUE;
    return -1;
}

static uint8_t peep_fused_opcode(uint8_t a, uint8_t b)
{
    switch (a) {
    case OP_STORE:
        return b == OP_POP ? OP_STORE_POP : 0;
    case OP_LOAD:
        switch (b) {
        case OP_LOAD:        return OP_LOAD_LOAD;
        case OP_CONST:       return OP_LOAD_CONST;
        case OP_CALL_GLOBAL: return OP_LOAD_CALL_GLOBAL;
        case OP_STRUCT_REF:  return OP_LOAD_STRUCT_REF;
        case OP_MV_RESET:    return OP_LOAD_MV_RESET;
        case OP_RET:         return OP_LOAD_RET;
        case OP_JNIL:        return OP_LOAD_JNIL;
        default:             return 0;
        }
    case OP_POP:
        return b == OP_LOAD ? OP_POP_LOAD : 0;
    case OP_EQ:
        return b == OP_JNIL ? OP_EQ_JNIL : 0;
    case OP_GLOAD:
        switch (b) {
        case OP_JNIL:        return OP_GLOAD_JNIL;
        case OP_CALL_GLOBAL: return OP_GLOAD_CALL_GLOBAL;
        default:             return 0;
        }
    default:
        return 0;
    }
}

/* Fold member M (index MI) into head A: A's operands grow by M's, a
 * branch operand moves to A, M is deleted and chained after PREV. */
static void peep_absorb(PeepInsn *a, PeepInsn *prev, PeepInsn *m, int32_t mi)
{
    a->raw_len = (uint16_t)(a->raw_len + m->own_len);
    if (m->jump_target >= 0) a->jump_target = m->jump_target;
    prev->fused_with = mi;
    m->flags |= PEEP_DELETED | PEEP_FUSED;
    m->jump_target = -1;
}

static int peep_fuse(PeepCode *pc)
{
    int changed = 0;
    int32_t i;
    if (!cl_peephole_fuse_enabled) return 0;
    for (i = 0; i < pc->count; i++) {
        PeepInsn *a = &pc->insns[i];
        PeepInsn *b, *c;
        int32_t bi, ci;
        int joined;
        uint8_t fused;
        if (a->flags & PEEP_DELETED) continue;
        bi = peep_next_live_unjoined(pc, i, &joined);
        if (bi >= pc->count || joined) continue;
        b = &pc->insns[bi];
        ci = peep_next_live_unjoined(pc, bi, &joined);
        if (ci < pc->count && !joined) {
            c = &pc->insns[ci];
            fused = peep_fused_opcode3(a->op, b->op, c->op);
            if (fused) {
                a->op = fused;
                peep_absorb(a, a, b, bi);
                peep_absorb(a, b, c, ci);
                changed++;
                i = ci;
                continue;
            }
        }
        fused = peep_fused_opcode(a->op, b->op);
        if (!fused) {
            int kind = peep_cmp_br_kind(a->op, b->op);
            if (kind < 0) continue;
            a->op = OP_CMP_BR;
            a->synth_len = 1;
            a->synth = (uint8_t)kind;
            a->raw_len = (uint16_t)(a->raw_len + 1);
            peep_absorb(a, a, b, bi);
            changed++;
            i = bi;
            continue;
        }
        a->op = fused;
        peep_absorb(a, a, b, bi);
        changed++;
        i = bi;   /* resume after the absorbed member */
    }
    return changed;
}

/* --- Re-encode --- */

/* Assign new positions, emit into OUT (size >= input length), rewrite the
 * pc-keyed line map in place.  Returns the new code length. */
static uint32_t peep_encode(PeepCode *pc, uint8_t *out,
                            CL_LineEntry *lines, int *n_lines)
{
    uint32_t pos = 0;
    int32_t i;

    /* Pass 1: assign new positions to live instructions. */
    for (i = 0; i < pc->count; i++) {
        PeepInsn *in = &pc->insns[i];
        if (in->flags & PEEP_DELETED) continue;
        in->new_pos = pos;
        pos += peep_insn_len(in);
    }

    /* Pass 2: emit bytes; jump offsets recomputed from new positions.  A
     * jump whose target instruction was deleted retargets to the next live
     * instruction (deleting a jump target is only ever done for pattern
     * pairs that are semantically no-ops on every path through them).  A
     * fused instruction's operand bytes are its members' operand bytes in
     * order (the fused_with chain); its jump offset, if any, is then
     * rewritten at the fused shape's position. */
    for (i = 0; i < pc->count; i++) {
        PeepInsn *in = &pc->insns[i];
        const CL_OpcodeInfo *info;
        uint8_t *dst;
        uint32_t w;
        int32_t m;
        if (in->flags & PEEP_DELETED) continue;
        info = cl_opcode_info(in->op);
        dst = out + in->new_pos;
        dst[0] = in->op;
        w = 1;
        if (in->synth_len) dst[w++] = in->synth;
        for (m = i; m >= 0; m = pc->insns[m].fused_with) {
            const PeepInsn *mem = &pc->insns[m];
            if (mem->own_len)
                memcpy(dst + w, mem->raw + 1, (size_t)mem->own_len);
            w += mem->own_len;
        }
        if (in->jump_target >= 0) {
            int32_t t = in->jump_target;
            uint32_t tpos;
            int32_t off;
            int jpos = cl_opnd_jrel_pos(info->operands);
            while (t < pc->count && (pc->insns[t].flags & PEEP_DELETED)) t++;
            /* The function's final RET/HALT is never deleted while a live
             * jump references past it (it is reachable via that jump), so a
             * live target always exists. */
            tpos = pc->insns[t].new_pos;
            off = (int32_t)tpos - (int32_t)(in->new_pos + in->raw_len);
            peep_write_i32(dst + jpos, off);
        }
    }

    /* Remap the source line table: each entry's pc moves to the new position
     * of the first live instruction at old_pos >= pc; entries that collapse
     * onto the same new pc keep only the first.  An entry on an absorbed
     * fusion member maps BACK to its head (the instruction that now runs
     * it), not forward — forward would hand the member's line to whatever
     * instruction follows the pair, displacing that one's own entry. */
    if (lines && n_lines && *n_lines > 0) {
        int out_n = 0;
        int li;
        int32_t last_pc = -1;
        for (li = 0; li < *n_lines; li++) {
            uint32_t want = lines[li].pc;
            int32_t idx;
            /* first instruction at old_pos >= want */
            {
                int32_t lo = 0, hi = pc->count;
                while (lo < hi) {
                    int32_t mid = lo + (hi - lo) / 2;
                    if (pc->insns[mid].old_pos < want) lo = mid + 1;
                    else hi = mid;
                }
                idx = lo;
            }
            if (idx < pc->count && (pc->insns[idx].flags & PEEP_FUSED)) {
                while (idx > 0 && (pc->insns[idx].flags & PEEP_DELETED))
                    idx--;
            }
            while (idx < pc->count && (pc->insns[idx].flags & PEEP_DELETED))
                idx++;
            if (idx >= pc->count) continue;  /* line mapped to deleted tail */
            if ((int32_t)pc->insns[idx].new_pos == last_pc) continue;
            last_pc = (int32_t)pc->insns[idx].new_pos;
            lines[out_n].pc = (uint16_t)pc->insns[idx].new_pos;
            lines[out_n].line = lines[li].line;
            out_n++;
        }
        *n_lines = out_n;
    }

    return pos;
}

/* --- Driver --- */

int cl_peephole_run(uint8_t *code, int *code_len,
                    const CL_Obj *constants, int n_constants,
                    CL_LineEntry *lines, int *n_lines)
{
    PeepCode pc;
    uint8_t *out;
    uint32_t len = (uint32_t)*code_len;
    uint32_t new_len;
    int rounds;
    int total = 0;

    if (*code_len <= 0 || (uint32_t)*code_len > CL_PEEPHOLE_MAX_CODE)
        return 0;

    pc.insns = (PeepInsn *)platform_alloc(len * sizeof(PeepInsn));
    if (!pc.insns) return 0;  /* OOM — skip optimization, never fail */
    pc.count = 0;
    pc.constants = constants;
    pc.n_constants = (int32_t)n_constants;

    if (!peep_decode(&pc, code, len)) {
        platform_free(pc.insns);
        return 0;
    }

    /* Fixpoint: deleting a pair can expose a new pair (LOAD a; LOAD b; POP;
     * POP).  Targets are recomputed each round because deleted jumps drop
     * their target marks.  Bounded — each round must delete/rewrite at
     * least one instruction to continue. */
    for (rounds = 0; rounds < 8; rounds++) {
        int changed = 0;
        peep_mark_targets(&pc);
        changed += peep_thread_jumps(&pc);
        changed += peep_jump_to_next(&pc);
        peep_mark_targets(&pc);  /* threading moves target marks */
        changed += peep_return_site(&pc);
        changed += peep_dead_code(&pc);
        changed += peep_store_reload(&pc);
        changed += peep_dead_store_ret(&pc);
        changed += peep_pure_pop(&pc);
        changed += peep_not_branch(&pc);
        total += changed;
        if (!changed) break;
    }

    /* Superinstructions, once, over the settled stream (marks may be stale
     * if the loop hit its round cap with changes still pending). */
    peep_mark_targets(&pc);
    total += peep_fuse(&pc);

    if (!total) {
        platform_free(pc.insns);
        return 0;
    }

    /* Validate: every live jump must retarget (forward past deletions) to a
     * live instruction.  The invariant "a targeted region always ends in a
     * live RET/HALT" makes failure impossible; if it ever breaks, bail out
     * rather than emit a corrupt offset. */
    {
        int32_t i;
        for (i = 0; i < pc.count; i++) {
            PeepInsn *in = &pc.insns[i];
            int32_t t;
            if (in->flags & PEEP_DELETED) continue;
            if (in->jump_target < 0) continue;
            t = in->jump_target;
            while (t < pc.count && (pc.insns[t].flags & PEEP_DELETED)) t++;
            if (t >= pc.count) {
                platform_free(pc.insns);
                return 0;
            }
        }
    }

    out = (uint8_t *)platform_alloc(len);
    if (!out) {
        platform_free(pc.insns);
        return 0;
    }

    new_len = peep_encode(&pc, out, lines, n_lines);
    memcpy(code, out, new_len);
    *code_len = (int)new_len;

#ifdef DEBUG_PEEPHOLE
    fprintf(stderr, "[peephole] %d rewrites, %u -> %u bytes\n",
            total, (unsigned)len, (unsigned)new_len);
#endif

    platform_free(out);
    platform_free(pc.insns);
    return 1;
}

void cl_peephole_optimize(struct CL_Compiler_s *c)
{
    /* (speed 0) keeps the bytecode exactly as emitted (AMILISP_FORCE_SPEED=0
     * is how the differential harness gets the reference output). */
    if (c->peep_speed_max < 1) return;
    cl_peephole_run(c->code, &c->code_pos, c->constants, c->const_count,
                    c->line_entries, &c->line_entry_count);
}
