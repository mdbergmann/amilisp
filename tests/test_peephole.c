/* Tests for the bytecode peephole post-pass (spec 1.8, peephole.c) and its
 * superinstruction fusion step (spec 4.3); the pass runs at every speed
 * above 0.
 *
 * Layer 1 drives the raw engine (cl_peephole_run) with hand-built byte
 * streams: pattern-by-pattern before/after checks, jump/NLX-landing
 * relocation across deletions, line-map remapping, decoder exhaustiveness
 * over EVERY opcode in CL_OPCODE_LIST, and the fail-safe bail-outs
 * (unknown opcode, truncated operands, undecodable OP_CLOSURE).
 *
 * Layer 2 evaluates Lisp through the full compiler and asserts speed-3
 * functions behave identically to their speed-1 twins — including the ANSI
 * guarantees the pass must NOT break: discarded (car 5) still signals
 * type-error, multiple-values state is preserved, NLX (tagbody/go,
 * catch/throw, unwind-protect, block/return-from) survives relocation.
 *
 * The cross-implementation differential harness lives in
 * tests/test_peephole_diff.sh (same corpus at AMILISP_FORCE_SPEED=0 vs 1,
 * 2 and 3); the fused shapes are pinned through DISASSEMBLE by
 * tests/test_tier4_phase3.sh. */

#include "test.h"
#include "core/types.h"
#include "core/mem.h"
#include "core/error.h"
#include "core/package.h"
#include "core/symbol.h"
#include "core/reader.h"
#include "core/printer.h"
#include "core/compiler.h"
#include "core/vm.h"
#include "core/builtins.h"
#include "core/repl.h"
#include "core/opcodes.h"
#include "core/peephole.h"
#include "platform/platform.h"
#include <string.h>

static void setup(void)
{
    platform_init();
    cl_thread_init();
    cl_error_init();
    cl_mem_init(CL_DEFAULT_HEAP_SIZE);
    cl_package_init();
    cl_symbol_init();
    cl_reader_init();
    cl_printer_init();
    cl_compiler_init();
    cl_vm_init(0, 0);
    cl_builtins_init();
    cl_repl_init();
}

static void teardown(void)
{
    cl_mem_shutdown();
    platform_shutdown();
}

/* Evaluate EXPR; treat a non-NIL result as a passing predicate. */
static int truthy(const char *expr)
{
    return cl_eval_string(expr) != CL_NIL;
}

/* Run the raw engine over CODE/LEN with a one-entry constant pool holding
 * NIL (an OP_HANDLER_CASE_PUSH in a synthesized stream then has zero
 * clauses; without a pool the pass would bail on it) and no line map. */
static int run_peep(uint8_t *code, int *len)
{
    CL_Obj consts[1];
    consts[0] = CL_NIL;
    return cl_peephole_run(code, len, consts, 1, NULL, NULL);
}

TEST(handler_case_landing_table_pinned)
{
    /* HANDLER_CASE_PUSH 0 (two clause types) -> table at 10:
     *   0: HANDLER_CASE_PUSH 0 +3 -> 10   7: NIL   8: HANDLER_CASE_POP   9: RET
     *  10: JMP +5 -> 20 (entry 0)   15: JMP +5 -> 25 (entry 1)   <- pinned
     *  20: POP  21: CONST 1  24: RET   25: POP  26: CONST 2  29: RET
     * Entry 1 is unreachable by bytecode control flow and must survive as
     * a 5-byte JMP. */
    CL_Obj consts[3];
    uint8_t code[] = {
        OP_HANDLER_CASE_PUSH, 0, 0, 0, 0, 0, 3,   /* -> 10 */
        OP_NIL, OP_HANDLER_CASE_POP, OP_RET,
        OP_JMP, 0, 0, 0, 5,                       /* entry 0 -> 20 */
        OP_JMP, 0, 0, 0, 5,                       /* entry 1 -> 25 */
        OP_POP, OP_CONST, 0, 1, OP_RET,
        OP_POP, OP_CONST, 0, 2, OP_RET
    };
    uint8_t orig[sizeof(code)];
    int len = (int)sizeof(code);
    consts[0] = cl_eval_string("'(warning error)");
    consts[1] = CL_NIL;
    consts[2] = CL_NIL;
    memcpy(orig, code, sizeof(code));
    ASSERT_EQ_INT(cl_peephole_run(code, &len, consts, 3, NULL, NULL), 0);
    ASSERT(memcmp(code, orig, sizeof(orig)) == 0);
    /* an entry whose clause body IS a bare RET: still a JMP */
    {
        uint8_t code2[] = {
            OP_HANDLER_CASE_PUSH, 0, 0, 0, 0, 0, 3,
            OP_NIL, OP_HANDLER_CASE_POP, OP_RET,
            OP_JMP, 0, 0, 0, 5,                   /* -> 20 (RET) */
            OP_JMP, 0, 0, 0, 1,                   /* -> 21 (RET) */
            OP_RET, OP_RET
        };
        uint8_t orig2[sizeof(code2)];
        int len2 = (int)sizeof(code2);
        memcpy(orig2, code2, sizeof(code2));
        ASSERT_EQ_INT(cl_peephole_run(code2, &len2, consts, 3, NULL, NULL), 0);
        ASSERT(memcmp(code2, orig2, sizeof(orig2)) == 0);
    }
    /* Lisp: the second clause of a two-clause HANDLER-CASE is reached */
    ASSERT(truthy(
        "(progn"
        " (defun ph-hc2 (x) (handler-case (if x (warn \"w\") (error \"e\"))"
        "                    (warning () :as-warning) (error () :as-error)))"
        " (and (eq (ph-hc2 t) :as-warning) (eq (ph-hc2 nil) :as-error)))"));
}

/* --- Layer 1: raw engine --- */

TEST(opcode_info_exhaustive_and_rejects_gaps)
{
    /* Every opcode in the list must have decode info... */
#define CHECK_OP(op_sym, op_val, op_str, op_opnd, op_flags) \
    ASSERT(cl_opcode_info((uint8_t)(op_val)) != NULL); \
    ASSERT_STR_EQ(cl_opcode_info((uint8_t)(op_val))->name, op_str);
    CL_OPCODE_LIST(CHECK_OP)
#undef CHECK_OP
    /* ...and gap bytes must have none (a sample across the value space). */
    ASSERT(cl_opcode_info(0x00) == NULL);
    ASSERT(cl_opcode_info(0x07) == NULL);
    ASSERT(cl_opcode_info(0x1F) == NULL);
    ASSERT(cl_opcode_info(0x51) == NULL);
    ASSERT(cl_opcode_info(0xC5) == NULL);
    ASSERT(cl_opcode_info(0xFE) == NULL);
    /* ...and the two shape helpers agree with the decoder's contract. */
    ASSERT_EQ_INT(cl_opnd_len(CL_OPND_CLOSURE), -1);
    ASSERT_EQ_INT(cl_opnd_len(CL_OPND_U8_U16_U8), 4);
    ASSERT_EQ_INT(cl_opnd_len(CL_OPND_U8_JREL), 5);
    ASSERT_EQ_INT(cl_opnd_jrel_pos(CL_OPND_JREL), 1);
    ASSERT_EQ_INT(cl_opnd_jrel_pos(CL_OPND_U8_JREL), 2);
    ASSERT_EQ_INT(cl_opnd_jrel_pos(CL_OPND_U16_JREL), 3);
    ASSERT_EQ_INT(cl_opnd_jrel_pos(CL_OPND_U8_U16_U8), 0);
}

/* Decoder exhaustiveness: for every opcode (except OP_CLOSURE, whose
 * operand length depends on a constant-pool template — covered by the
 * Lisp-level tests), a stream of [X, LOAD 0, POP, NIL, RET] must decode far
 * enough that the LOAD/POP pair is optimized away.  A decode bail-out
 * would return 0 — so this catches any future opcode whose operand shape
 * in CL_OPCODE_LIST disagrees with what a synthesized stream contains.
 * (The NIL keeps X off the RET: STORE n; RET and JMP -> RET are rewritten
 * themselves, which would change the first byte.) */
TEST(decoder_knows_every_opcode)
{
    static const struct { uint8_t op; uint8_t opnd; } ops[] = {
#define OP_ENTRY(name, value, str, opnd, flags) { (uint8_t)(value & 0xFF), (uint8_t)(opnd) },
        CL_OPCODE_LIST(OP_ENTRY)
#undef OP_ENTRY
    };
    size_t i;
    for (i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
        uint8_t code[24];
        int len = 0, ret;
        uint8_t expect_first;
        uint32_t opnd_len;
        if (ops[i].opnd == CL_OPND_CLOSURE) continue;
        ASSERT(cl_opnd_len(ops[i].opnd) >= 0);
        opnd_len = (uint32_t)cl_opnd_len(ops[i].opnd);
        code[len++] = ops[i].op;
        /* zero operands: jump offsets of 0 target the next instruction */
        memset(code + len, 0, opnd_len);
        len += (int)opnd_len;
        code[len++] = OP_LOAD; code[len++] = 0;
        code[len++] = OP_POP;
        code[len++] = OP_NIL;
        code[len++] = OP_RET;
        ret = run_peep(code, &len);
        /* An OP_JMP with a zero offset jumps to the next instruction: the
         * jump-to-next rewrite deletes it, and the LOAD;POP behind it goes
         * with the pure-pop rule, so the stream starts at the NIL. */
        expect_first = (ops[i].op == OP_JMP) ? OP_NIL : ops[i].op;
        if (ret != 1 || code[0] != expect_first) {
            printf("  opcode 0x%02X: ret=%d first-byte=0x%02X\n",
                   ops[i].op, ret, code[0]);
        }
        ASSERT_EQ_INT(ret, 1);           /* decoded + optimized the pair */
        ASSERT_EQ_INT(code[0], expect_first); /* opcode itself preserved */
    }
}

TEST(store_pop_load_becomes_store)
{
    uint8_t code[] = { OP_STORE, 1, OP_POP, OP_LOAD, 1, OP_NIL, OP_RET };
    uint8_t want[] = { OP_STORE, 1, OP_NIL, OP_RET };
    int len = (int)sizeof(code);
    ASSERT_EQ_INT(run_peep(code, &len), 1);
    ASSERT_EQ_INT(len, (int)sizeof(want));
    ASSERT(memcmp(code, want, sizeof(want)) == 0);
}

TEST(store_pop_load_different_slot_not_elided)
{
    /* Different slot: no reload elision — the pair fuses instead, and the
     * LOAD before the RET fuses with it. */
    uint8_t code[] = { OP_STORE, 1, OP_POP, OP_LOAD, 2, OP_RET };
    uint8_t want[] = { OP_STORE_POP, 1, OP_LOAD_RET, 2 };
    int len = (int)sizeof(code);
    ASSERT_EQ_INT(run_peep(code, &len), 1);
    ASSERT_EQ_INT(len, (int)sizeof(want));
    ASSERT(memcmp(code, want, sizeof(want)) == 0);
}

TEST(pure_pop_elided_when_mv_write_masked)
{
    /* CONST writes mv state, but the following OP_NIL re-writes it before
     * anything can observe — pair provably dead. */
    uint8_t code[] = { OP_CONST, 0, 0, OP_POP, OP_NIL, OP_RET };
    uint8_t want[] = { OP_NIL, OP_RET };
    int len = (int)sizeof(code);
    ASSERT_EQ_INT(run_peep(code, &len), 1);
    ASSERT_EQ_INT(len, (int)sizeof(want));
    ASSERT(memcmp(code, want, sizeof(want)) == 0);
}

TEST(pure_pop_kept_when_mv_observer_follows)
{
    /* MV_TO_LIST observes mv state: deleting CONST's mv write would let a
     * previous call's values leak through — must not fire. */
    uint8_t code[] = { OP_CONST, 0, 0, OP_POP, OP_MV_TO_LIST, OP_RET };
    uint8_t orig[sizeof(code)];
    int len = (int)sizeof(code);
    memcpy(orig, code, sizeof(code));
    ASSERT_EQ_INT(run_peep(code, &len), 0);
    ASSERT(memcmp(code, orig, sizeof(orig)) == 0);
}

TEST(pure_pop_kept_before_ret)
{
    /* RET propagates mv state to the caller — a CONST;POP directly before
     * it can't be proven dead. */
    uint8_t code[] = { OP_CONST, 0, 0, OP_POP, OP_RET };
    uint8_t orig[sizeof(code)];
    int len = (int)sizeof(code);
    memcpy(orig, code, sizeof(code));
    ASSERT_EQ_INT(run_peep(code, &len), 0);
    ASSERT(memcmp(code, orig, sizeof(orig)) == 0);
}

TEST(load_pop_elided_without_mv_proof)
{
    /* OP_LOAD writes no mv state, so LOAD;POP is deletable even directly
     * before RET. */
    uint8_t code[] = { OP_LOAD, 3, OP_POP, OP_RET };
    uint8_t want[] = { OP_RET };
    int len = (int)sizeof(code);
    ASSERT_EQ_INT(run_peep(code, &len), 1);
    ASSERT_EQ_INT(len, (int)sizeof(want));
    ASSERT(memcmp(code, want, sizeof(want)) == 0);
}

TEST(cascaded_pairs_reach_fixpoint)
{
    /* LOAD a; LOAD b; POP; POP — inner pair first, outer pair on the next
     * round. */
    uint8_t code[] = { OP_LOAD, 0, OP_LOAD, 1, OP_POP, OP_POP, OP_RET };
    uint8_t want[] = { OP_RET };
    int len = (int)sizeof(code);
    ASSERT_EQ_INT(run_peep(code, &len), 1);
    ASSERT_EQ_INT(len, (int)sizeof(want));
    ASSERT(memcmp(code, want, sizeof(want)) == 0);
}

TEST(not_jnil_fused_to_jtrue_and_relocated)
{
    /* LOAD 0; NOT; JNIL +5 -> [target]; NIL; RET; [target] T; RET
     * Both successors re-write mv (NIL / T), so NOT is dropped outright and
     * the branch inverts; the +5 offset must shrink to +2 (the NIL;RET
     * fallthrough stays, only NOT's 1 byte disappears... offset is relative
     * so it stays +2 = NIL,RET). */
    uint8_t code[] = {
        OP_LOAD, 0,
        OP_NOT,
        OP_JNIL, 0, 0, 0, 2,   /* -> T */
        OP_NIL, OP_RET,
        OP_T, OP_RET
    };
    uint8_t want[] = {
        OP_LOAD, 0,
        OP_JTRUE, 0, 0, 0, 2,  /* -> T */
        OP_NIL, OP_RET,
        OP_T, OP_RET
    };
    int len = (int)sizeof(code);
    ASSERT_EQ_INT(run_peep(code, &len), 1);
    ASSERT_EQ_INT(len, (int)sizeof(want));
    ASSERT(memcmp(code, want, sizeof(want)) == 0);
}

TEST(not_branch_keeps_mv_write_when_unprovable)
{
    /* Successor paths end in LOAD;RET (no mv re-write): NOT's mv write must
     * survive as MV_RESET while the branch still inverts. */
    uint8_t code[] = {
        OP_NOT,
        OP_JNIL, 0, 0, 0, 3,   /* -> LOAD 2 */
        OP_LOAD, 1, OP_RET,
        OP_LOAD, 2, OP_RET
    };
    uint8_t want[] = {
        OP_MV_RESET,
        OP_JTRUE, 0, 0, 0, 2,
        OP_LOAD_RET, 1,          /* each LOAD;RET fused */
        OP_LOAD_RET, 2
    };
    int len = (int)sizeof(code);
    ASSERT_EQ_INT(run_peep(code, &len), 1);
    ASSERT_EQ_INT(len, (int)sizeof(want));
    ASSERT(memcmp(code, want, sizeof(want)) == 0);
}

TEST(jump_threading_and_dead_jump_removal)
{
    /* JNIL -> JMP -> T: the conditional threads to the final target and the
     * intermediate JMP becomes unreachable. */
    uint8_t code[] = {
        OP_JNIL, 0, 0, 0, 2,   /* -> JMP */
        OP_NIL, OP_RET,
        OP_JMP, 0, 0, 0, 0,    /* -> T (next insn) */
        OP_T, OP_RET
    };
    uint8_t want[] = {
        OP_JNIL, 0, 0, 0, 2,   /* -> T directly (over NIL;RET) */
        OP_NIL, OP_RET,
        OP_T, OP_RET
    };
    int len = (int)sizeof(code);
    ASSERT_EQ_INT(run_peep(code, &len), 1);
    ASSERT_EQ_INT(len, (int)sizeof(want));
    ASSERT(memcmp(code, want, sizeof(want)) == 0);
}

TEST(dead_code_after_jmp_removed)
{
    /* The JMP over the dead pair then targets the instruction after it
     * and is deleted in turn (jump-to-next). */
    uint8_t code[] = {
        OP_JMP, 0, 0, 0, 3,    /* over LOAD;POP -> NIL */
        OP_LOAD, 0, OP_POP,    /* unreachable */
        OP_NIL, OP_RET
    };
    uint8_t want[] = {
        OP_NIL, OP_RET
    };
    int len = (int)sizeof(code);
    ASSERT_EQ_INT(run_peep(code, &len), 1);
    ASSERT_EQ_INT(len, (int)sizeof(want));
    ASSERT(memcmp(code, want, sizeof(want)) == 0);
}

TEST(backward_jump_relocated_across_deletion)
{
    /* Loop head after a deletable pair: the backward offset must account
     * for the removed bytes.  Layout:
     *   0: LOAD 0; 2: POP          (deleted)
     *   3: NIL                     (loop head, target)
     *   4: JNIL -> head            (backward: offset -9)
     *   9: RET
     * After deletion the head sits at 0 and JNIL's offset becomes -6. */
    uint8_t code[] = {
        OP_LOAD, 0, OP_POP,
        OP_NIL,
        OP_JNIL, 0xFF, 0xFF, 0xFF, 0xF7,  /* -9 -> NIL */
        OP_RET
    };
    uint8_t want[] = {
        OP_NIL,
        OP_JNIL, 0xFF, 0xFF, 0xFF, 0xFA,  /* -6 -> NIL */
        OP_RET
    };
    int len = (int)sizeof(code);
    ASSERT_EQ_INT(run_peep(code, &len), 1);
    ASSERT_EQ_INT(len, (int)sizeof(want));
    ASSERT(memcmp(code, want, sizeof(want)) == 0);
}

/* cl_bytecode_has_backward_jump: the m68k JIT compiles a function that
 * loops on its first call instead of waiting for the hot threshold. */
static const CL_Bytecode *peep_fn_bytecode(CL_Obj fn)
{
    if (CL_CLOSURE_P(fn))
        fn = ((CL_Closure *)CL_OBJ_TO_PTR(fn))->bytecode;
    return CL_BYTECODE_P(fn) ? (const CL_Bytecode *)CL_OBJ_TO_PTR(fn) : NULL;
}

TEST(backward_jump_detection)
{
    uint8_t loop[] = {
        OP_NIL,
        OP_JNIL, 0xFF, 0xFF, 0xFF, 0xFA,   /* -6 -> NIL */
        OP_RET
    };
    uint8_t forward[] = {
        OP_NIL,
        OP_JNIL, 0x00, 0x00, 0x00, 0x01,   /* +1 -> RET, skipping NIL */
        OP_NIL,
        OP_RET
    };
    /* A fused member carrying the offset after another operand. */
    uint8_t fused_loop[] = {
        OP_LOAD_JNIL, 0, 0xFF, 0xFF, 0xFF, 0xFA,   /* -6 -> itself */
        OP_RET
    };
    uint8_t unknown[] = { 0x00, OP_NIL, OP_JNIL, 0xFF, 0xFF, 0xFF, 0xFA, OP_RET };
    uint8_t truncated[] = { OP_NIL, OP_JNIL, 0xFF, 0xFF };
    const CL_Bytecode *bc;

    ASSERT_EQ_INT(cl_bytecode_has_backward_jump(loop, sizeof(loop), NULL, 0), 1);
    ASSERT_EQ_INT(cl_bytecode_has_backward_jump(forward, sizeof(forward), NULL, 0), 0);
    ASSERT_EQ_INT(cl_bytecode_has_backward_jump(fused_loop, sizeof(fused_loop), NULL, 0), 1);
    /* What it cannot decode answers "no loop" (the count still applies). */
    ASSERT_EQ_INT(cl_bytecode_has_backward_jump(unknown, sizeof(unknown), NULL, 0), 0);
    ASSERT_EQ_INT(cl_bytecode_has_backward_jump(truncated, sizeof(truncated), NULL, 0), 0);
    ASSERT_EQ_INT(cl_bytecode_has_backward_jump(NULL, 0, NULL, 0), 0);

    /* Compiled code: DOTIMES loops, a straight-line body with an IF does not;
     * an inner LAMBDA (OP_CLOSURE's variable-length operand) is stepped over. */
    bc = peep_fn_bytecode(cl_eval_string(
        "(lambda (n) (let ((s 0)) (dotimes (i n) (setq s (+ s i))) s))"));
    ASSERT(bc != NULL);
    ASSERT_EQ_INT(cl_bytecode_has_backward_jump(bc->code, bc->code_len,
                                                bc->constants, bc->n_constants), 1);
    bc = peep_fn_bytecode(cl_eval_string(
        "(lambda (x y) (let ((f (lambda () (+ x y)))) (if (> x 0) (funcall f) (- y))))"));
    ASSERT(bc != NULL);
    ASSERT_EQ_INT(cl_bytecode_has_backward_jump(bc->code, bc->code_len,
                                                bc->constants, bc->n_constants), 0);
}

TEST(catch_landing_pad_relocated)
{
    /* OP_CATCH's i32 is an NLX landing pad — must be re-encoded like a jump
     * when a deletion shifts the pad.  Layout (byte positions):
     *   0: NIL                      (catch tag push)
     *   1: CATCH +9 -> 15           (operand ends at 6; 6+9 = T at 15)
     *   6: LOAD 0; 8: POP           (deleted, protected body)
     *   9: UNCATCH
     *  10: JMP +1 -> 16             (operand ends at 15; RET at 16)
     *  15: T                        (landing pad)
     *  16: RET
     */
    uint8_t code[] = {
        OP_NIL,
        OP_CATCH, 0, 0, 0, 9,   /* -> T at 15 */
        OP_LOAD, 0, OP_POP,
        OP_UNCATCH,
        OP_JMP, 0, 0, 0, 1,     /* -> RET at 16 */
        OP_T,                   /* landing pad */
        OP_RET
    };
    uint8_t want[] = {
        OP_NIL,
        OP_CATCH, 0, 0, 0, 2,   /* pad shifted back by the 3 deleted bytes
                                   and the 4 the JMP -> RET rewrite saved */
        OP_UNCATCH,
        OP_RET,                 /* was JMP -> RET */
        OP_T,
        OP_RET
    };
    int len = (int)sizeof(code);
    ASSERT_EQ_INT(run_peep(code, &len), 1);
    ASSERT_EQ_INT(len, (int)sizeof(want));
    ASSERT(memcmp(code, want, sizeof(want)) == 0);
}

TEST(line_map_remapped_and_deduped)
{
    /* Entries at the deleted pair remap to the next surviving instruction;
     * collapsing entries keep the first. */
    uint8_t code[] = { OP_LOAD, 0, OP_POP, OP_NIL, OP_RET };
    CL_LineEntry lines[3];
    int n_lines = 3;
    int len = (int)sizeof(code);
    lines[0].pc = 0; lines[0].line = 10;   /* at deleted LOAD */
    lines[1].pc = 3; lines[1].line = 11;   /* at NIL */
    lines[2].pc = 4; lines[2].line = 12;   /* at RET */
    ASSERT_EQ_INT(cl_peephole_run(code, &len, NULL, 0, lines, &n_lines), 1);
    ASSERT_EQ_INT(len, 2);
    /* pc 0 and pc 3 both land on the new NIL at 0 — first (line 10) wins */
    ASSERT_EQ_INT(n_lines, 2);
    ASSERT_EQ_INT(lines[0].pc, 0);
    ASSERT_EQ_INT(lines[0].line, 10);
    ASSERT_EQ_INT(lines[1].pc, 1);
    ASSERT_EQ_INT(lines[1].line, 12);
}

TEST(bails_on_unknown_opcode)
{
    uint8_t code[] = { OP_LOAD, 0, OP_POP, 0x00, OP_RET };
    uint8_t orig[sizeof(code)];
    int len = (int)sizeof(code);
    memcpy(orig, code, sizeof(code));
    ASSERT_EQ_INT(run_peep(code, &len), 0);
    ASSERT_EQ_INT(len, (int)sizeof(orig));
    ASSERT(memcmp(code, orig, sizeof(orig)) == 0);
}

TEST(bails_on_truncated_operand)
{
    uint8_t code[] = { OP_LOAD, 0, OP_POP, OP_CONST, 0 }; /* CONST missing a byte */
    uint8_t orig[sizeof(code)];
    int len = (int)sizeof(code);
    memcpy(orig, code, sizeof(code));
    ASSERT_EQ_INT(run_peep(code, &len), 0);
    ASSERT(memcmp(code, orig, sizeof(orig)) == 0);
}

TEST(bails_on_jump_landing_mid_instruction)
{
    uint8_t code[] = {
        OP_JMP, 0, 0, 0, 1,    /* lands inside LOAD's operand */
        OP_LOAD, 0, OP_POP,
        OP_RET
    };
    uint8_t orig[sizeof(code)];
    int len = (int)sizeof(code);
    memcpy(orig, code, sizeof(code));
    ASSERT_EQ_INT(run_peep(code, &len), 0);
    ASSERT(memcmp(code, orig, sizeof(orig)) == 0);
}

TEST(bails_on_closure_with_non_bytecode_constant)
{
    CL_Obj consts[1];
    uint8_t code[] = { OP_CLOSURE, 0, 0, OP_LOAD, 0, OP_POP, OP_RET };
    uint8_t orig[sizeof(code)];
    int len = (int)sizeof(code);
    consts[0] = CL_NIL;
    memcpy(orig, code, sizeof(code));
    ASSERT_EQ_INT(cl_peephole_run(code, &len, consts, 1, NULL, NULL), 0);
    ASSERT(memcmp(code, orig, sizeof(orig)) == 0);
}

TEST(jump_into_deleted_pair_retargets_forward)
{
    /* A branch landing exactly ON a deleted pure pair retargets to the next
     * live instruction — the pair was a push/pop no-op on that path too.
     *   0: JNIL +2 -> 7 (LOAD, pair start)
     *   5: NIL; 6: RET;  7: LOAD 0; 9: POP;  10: NIL; 11: RET */
    uint8_t code[] = {
        OP_JNIL, 0, 0, 0, 2,   /* -> LOAD at 7 */
        OP_NIL, OP_RET,
        OP_LOAD, 0, OP_POP,    /* deletable pair (pair start is the target) */
        OP_NIL, OP_RET
    };
    uint8_t want[] = {
        OP_JNIL, 0, 0, 0, 2,   /* -> NIL at 7 */
        OP_NIL, OP_RET,
        OP_NIL, OP_RET
    };
    int len = (int)sizeof(code);
    ASSERT_EQ_INT(run_peep(code, &len), 1);
    ASSERT_EQ_INT(len, (int)sizeof(want));
    ASSERT(memcmp(code, want, sizeof(want)) == 0);
}

TEST(jump_landing_inside_pattern_blocks_it)
{
    /* A branch landing on the LOAD of STORE;POP;LOAD executes only the tail
     * of the pattern — the reload elision must not fire.  The untargeted
     * STORE;POP still fuses, as does the LOAD;RET the branch lands on. */
    uint8_t code[] = {
        OP_JNIL, 0, 0, 0, 3,   /* -> the LOAD at 8 */
        OP_STORE, 1, OP_POP, OP_LOAD, 1,
        OP_RET
    };
    uint8_t want[] = {
        OP_JNIL, 0, 0, 0, 2,   /* -> the LOAD_RET at 7 */
        OP_STORE_POP, 1, OP_LOAD_RET, 1
    };
    int len = (int)sizeof(code);
    ASSERT_EQ_INT(run_peep(code, &len), 1);
    ASSERT_EQ_INT(len, (int)sizeof(want));
    ASSERT(memcmp(code, want, sizeof(want)) == 0);
}

/* --- Layer 1b: superinstruction fusion (spec 4.3) and the two rewrites
 * that came with it (dead store before RET, JMP -> RET) --- */

TEST(jump_to_next_deleted_conditional_kept)
{
    /* JMP to the following instruction is a no-op and goes; a JNIL to the
     * following instruction still pops and stays (fused with its LOAD). */
    uint8_t code[] = {
        OP_JMP, 0, 0, 0, 0,
        OP_LOAD, 0,
        OP_JNIL, 0, 0, 0, 0,
        OP_NIL, OP_RET
    };
    uint8_t want[] = {
        OP_LOAD_JNIL, 0, 0, 0, 0, 0,
        OP_NIL, OP_RET
    };
    int len = (int)sizeof(code);
    ASSERT_EQ_INT(run_peep(code, &len), 1);
    ASSERT_EQ_INT(len, (int)sizeof(want));
    ASSERT(memcmp(code, want, sizeof(want)) == 0);
}

TEST(fuse_cmp_br_every_comparison_both_polarities)
{
    /* LT..CHAREQ; JNIL/JTRUE t -> CMP_BR kind t with the synthesized kind
     * byte (comparison in bits 0-2, bit 3 = the JTRUE shape) and the jump
     * offset re-encoded after it (CL_OPND_U8_JREL). */
    static const struct { uint8_t cmp_op; uint8_t kind; } cmps[] = {
        { OP_LT, CL_CMP_BR_LT }, { OP_GT, CL_CMP_BR_GT },
        { OP_LE, CL_CMP_BR_LE }, { OP_GE, CL_CMP_BR_GE },
        { OP_NUMEQ, CL_CMP_BR_NUMEQ }, { OP_CHAREQ, CL_CMP_BR_CHAREQ }
    };
    size_t i;
    int pol;
    for (i = 0; i < sizeof(cmps) / sizeof(cmps[0]); i++) {
        for (pol = 0; pol < 2; pol++) {
            uint8_t code[] = {
                OP_LOAD, 0, OP_LOAD, 1,
                0 /* cmp */, 0 /* branch */, 0, 0, 0, 2,   /* -> over T;RET */
                OP_T, OP_RET,
                OP_NIL, OP_RET
            };
            uint8_t want[] = {
                OP_LOAD_LOAD, 0, 1,
                OP_CMP_BR, 0 /* kind */, 0, 0, 0, 2,
                OP_T, OP_RET,
                OP_NIL, OP_RET
            };
            int len = (int)sizeof(code);
            code[4] = cmps[i].cmp_op;
            code[5] = pol ? OP_JTRUE : OP_JNIL;
            want[4] = (uint8_t)(cmps[i].kind | (pol ? CL_CMP_BR_IF_TRUE : 0));
            ASSERT_EQ_INT(run_peep(code, &len), 1);
            ASSERT_EQ_INT(len, (int)sizeof(want));
            if (memcmp(code, want, sizeof(want)) != 0)
                printf("  cmp 0x%02X pol %d: got %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                       cmps[i].cmp_op, pol, code[0], code[1], code[2], code[3],
                       code[4], code[5], code[6], code[7], code[8]);
            ASSERT(memcmp(code, want, sizeof(want)) == 0);
        }
    }
}

TEST(cmp_br_not_fused_when_branch_is_a_target_and_backward_offset)
{
    /* A jump landing on the JNIL blocks the fusion; a backward CMP_BR
     * offset is relocated like any other branch. */
    uint8_t code[] = {
        OP_LOAD, 2,                  /* 0 */
        OP_JTRUE, 0, 0, 0, 5,        /* 2: -> the JNIL at 12 */
        OP_LOAD, 0, OP_LOAD, 1,      /* 7: fuses (the pass's one change) */
        OP_LT,                       /* 11 */
        OP_JNIL, 0, 0, 0, 1,         /* 12: -> NIL at 18 (over POP) */
        OP_POP,                      /* 17 */
        OP_NIL, OP_RET               /* 18 */
    };
    int len = (int)sizeof(code);
    ASSERT_EQ_INT(run_peep(code, &len), 1);
    ASSERT_EQ_INT(code[0], OP_LOAD);
    ASSERT_EQ_INT(code[7], OP_LOAD_LOAD);
    ASSERT_EQ_INT(code[10], OP_LT);      /* LT kept apart */
    ASSERT_EQ_INT(code[11], OP_JNIL);
    {
        uint8_t code2[] = {
            OP_NIL,                      /* 0: loop head */
            OP_LOAD, 0, OP_LOAD, 1,      /* 1 */
            OP_GE, OP_JTRUE, 0xFF, 0xFF, 0xFF, 0xF5,  /* 5: -11 -> 0 */
            OP_NIL, OP_RET
        };
        uint8_t want2[] = {
            OP_NIL,
            OP_LOAD_LOAD, 0, 1,
            OP_CMP_BR, (uint8_t)(CL_CMP_BR_GE | CL_CMP_BR_IF_TRUE),
            0xFF, 0xFF, 0xFF, 0xF6,      /* -10 -> 0 */
            OP_NIL, OP_RET
        };
        int len2 = (int)sizeof(code2);
        ASSERT_EQ_INT(run_peep(code2, &len2), 1);
        ASSERT_EQ_INT(len2, (int)sizeof(want2));
        ASSERT(memcmp(code2, want2, sizeof(want2)) == 0);
    }
}

TEST(fuse_store_pop)
{
    uint8_t code[] = { OP_STORE, 1, OP_POP, OP_NIL, OP_RET };
    uint8_t want[] = { OP_STORE_POP, 1, OP_NIL, OP_RET };
    int len = (int)sizeof(code);
    ASSERT_EQ_INT(run_peep(code, &len), 1);
    ASSERT_EQ_INT(len, (int)sizeof(want));
    ASSERT(memcmp(code, want, sizeof(want)) == 0);
}

TEST(fuse_load_load_and_load_call_global_greedy)
{
    /* Left to right: the first two LOADs pair up, the third pairs with the
     * call — two dispatches saved, and a fused head is never re-fused. */
    uint8_t code[] = { OP_LOAD, 0, OP_LOAD, 1, OP_LOAD, 2,
                       OP_CALL_GLOBAL, 0, 5, 3, OP_RET };
    uint8_t want[] = { OP_LOAD_LOAD, 0, 1, OP_LOAD_CALL_GLOBAL, 2, 0, 5, 3,
                       OP_RET };
    int len = (int)sizeof(code);
    ASSERT_EQ_INT(run_peep(code, &len), 1);
    ASSERT_EQ_INT(len, (int)sizeof(want));
    ASSERT(memcmp(code, want, sizeof(want)) == 0);
}

TEST(fuse_load_struct_ref_load_mv_reset_load_ret)
{
    {
        uint8_t code[] = { OP_LOAD, 0, OP_STRUCT_REF, 3, OP_NIL, OP_RET };
        uint8_t want[] = { OP_LOAD_STRUCT_REF, 0, 3, OP_NIL, OP_RET };
        int len = (int)sizeof(code);
        ASSERT_EQ_INT(run_peep(code, &len), 1);
        ASSERT_EQ_INT(len, (int)sizeof(want));
        ASSERT(memcmp(code, want, sizeof(want)) == 0);
    }
    {
        uint8_t code[] = { OP_LOAD, 4, OP_MV_RESET, OP_RET };
        uint8_t want[] = { OP_LOAD_MV_RESET, 4, OP_RET };
        int len = (int)sizeof(code);
        ASSERT_EQ_INT(run_peep(code, &len), 1);
        ASSERT_EQ_INT(len, (int)sizeof(want));
        ASSERT(memcmp(code, want, sizeof(want)) == 0);
    }
    {
        uint8_t code[] = { OP_LOAD, 2, OP_RET };
        uint8_t want[] = { OP_LOAD_RET, 2 };
        int len = (int)sizeof(code);
        ASSERT_EQ_INT(run_peep(code, &len), 1);
        ASSERT_EQ_INT(len, (int)sizeof(want));
        ASSERT(memcmp(code, want, sizeof(want)) == 0);
    }
}

TEST(fuse_eq_jnil_backward_offset_relocated)
{
    /* 0: T (loop head)  1: EQ  2: JNIL -7 -> 0  7: RET
     * The fused EQ_JNIL ends one byte earlier, so the backward offset
     * shrinks from -7 to -6. */
    uint8_t code[] = { OP_T, OP_EQ, OP_JNIL, 0xFF, 0xFF, 0xFF, 0xF9, OP_RET };
    uint8_t want[] = { OP_T, OP_EQ_JNIL, 0xFF, 0xFF, 0xFF, 0xFA, OP_RET };
    int len = (int)sizeof(code);
    ASSERT_EQ_INT(run_peep(code, &len), 1);
    ASSERT_EQ_INT(len, (int)sizeof(want));
    ASSERT(memcmp(code, want, sizeof(want)) == 0);
}

TEST(fuse_gload_jnil_and_load_jnil_forward)
{
    {
        /* GLOAD 7; JNIL +2 -> T; NIL; RET; T; RET */
        uint8_t code[] = { OP_GLOAD, 0, 7, OP_JNIL, 0, 0, 0, 2,
                           OP_NIL, OP_RET, OP_T, OP_RET };
        uint8_t want[] = { OP_GLOAD_JNIL, 0, 7, 0, 0, 0, 2,
                           OP_NIL, OP_RET, OP_T, OP_RET };
        int len = (int)sizeof(code);
        ASSERT_EQ_INT(run_peep(code, &len), 1);
        ASSERT_EQ_INT(len, (int)sizeof(want));
        ASSERT(memcmp(code, want, sizeof(want)) == 0);
    }
    {
        uint8_t code[] = { OP_LOAD, 3, OP_JNIL, 0, 0, 0, 2,
                           OP_NIL, OP_RET, OP_T, OP_RET };
        uint8_t want[] = { OP_LOAD_JNIL, 3, 0, 0, 0, 2,
                           OP_NIL, OP_RET, OP_T, OP_RET };
        int len = (int)sizeof(code);
        ASSERT_EQ_INT(run_peep(code, &len), 1);
        ASSERT_EQ_INT(len, (int)sizeof(want));
        ASSERT(memcmp(code, want, sizeof(want)) == 0);
    }
}

TEST(fuse_round_two_pairs_and_triples)
{
    {
        /* LOAD a; STORE b; POP  ->  LOAD_STORE_POP a b (the triple wins
         * over LOAD_LOAD / STORE_POP at the same head) */
        uint8_t code[] = { OP_LOAD, 0, OP_STORE, 3, OP_POP, OP_NIL, OP_RET };
        uint8_t want[] = { OP_LOAD_STORE_POP, 0, 3, OP_NIL, OP_RET };
        int len = (int)sizeof(code);
        ASSERT_EQ_INT(run_peep(code, &len), 1);
        ASSERT_EQ_INT(len, (int)sizeof(want));
        ASSERT(memcmp(code, want, sizeof(want)) == 0);
    }
    {
        /* GLOAD 7; EQ; JNIL +2  ->  GLOAD_EQ_JNIL 7 +2 */
        uint8_t code[] = { OP_GLOAD, 0, 7, OP_EQ, OP_JNIL, 0, 0, 0, 2,
                           OP_NIL, OP_RET, OP_T, OP_RET };
        uint8_t want[] = { OP_GLOAD_EQ_JNIL, 0, 7, 0, 0, 0, 2,
                           OP_NIL, OP_RET, OP_T, OP_RET };
        int len = (int)sizeof(code);
        ASSERT_EQ_INT(run_peep(code, &len), 1);
        ASSERT_EQ_INT(len, (int)sizeof(want));
        ASSERT(memcmp(code, want, sizeof(want)) == 0);
    }
    {
        /* a triple's second member as a branch target: the triple is
         * blocked, the pair headed by that member still forms */
        uint8_t code[] = { OP_JTRUE, 0, 0, 0, 3, OP_GLOAD, 0, 7, OP_EQ,
                           OP_JNIL, 0, 0, 0, 2, OP_NIL, OP_RET, OP_T, OP_RET };
        uint8_t want[] = { OP_JTRUE, 0, 0, 0, 3, OP_GLOAD, 0, 7,
                           OP_EQ_JNIL, 0, 0, 0, 2, OP_NIL, OP_RET, OP_T, OP_RET };
        int len = (int)sizeof(code);
        ASSERT_EQ_INT(run_peep(code, &len), 1);
        ASSERT_EQ_INT(len, (int)sizeof(want));
        ASSERT(memcmp(code, want, sizeof(want)) == 0);
    }
    {
        /* LOAD s; CONST k -> LOAD_CONST; GLOAD sym; CALL_GLOBAL f n ->
         * GLOAD_CALL_GLOBAL; a bare POP; LOAD s -> POP_LOAD */
        uint8_t code[] = { OP_LOAD, 2, OP_CONST, 0, 9,
                           OP_GLOAD, 0, 7, OP_CALL_GLOBAL, 0, 5, 3,
                           OP_POP, OP_LOAD, 1, OP_NIL, OP_RET };
        uint8_t want[] = { OP_LOAD_CONST, 2, 0, 9,
                           OP_GLOAD_CALL_GLOBAL, 0, 7, 0, 5, 3,
                           OP_POP_LOAD, 1, OP_NIL, OP_RET };
        int len = (int)sizeof(code);
        ASSERT_EQ_INT(run_peep(code, &len), 1);
        ASSERT_EQ_INT(len, (int)sizeof(want));
        ASSERT(memcmp(code, want, sizeof(want)) == 0);
    }
    {
        /* STORE;POP claims its POP before POP;LOAD can: STORE_POP + LOAD */
        uint8_t code[] = { OP_STORE, 1, OP_POP, OP_LOAD, 2, OP_NIL, OP_RET };
        uint8_t want[] = { OP_STORE_POP, 1, OP_LOAD, 2, OP_NIL, OP_RET };
        int len = (int)sizeof(code);
        ASSERT_EQ_INT(run_peep(code, &len), 1);
        ASSERT_EQ_INT(len, (int)sizeof(want));
        ASSERT(memcmp(code, want, sizeof(want)) == 0);
    }
}

TEST(fusion_blocked_by_jump_onto_second_member)
{
    /* JTRUE lands on the POP: fusing would run STORE on that path too. */
    uint8_t code[] = { OP_JTRUE, 0, 0, 0, 2, OP_STORE, 1, OP_POP, OP_RET };
    uint8_t orig[sizeof(code)];
    int len = (int)sizeof(code);
    memcpy(orig, code, sizeof(code));
    ASSERT_EQ_INT(run_peep(code, &len), 0);
    ASSERT(memcmp(code, orig, sizeof(orig)) == 0);
}

TEST(fusion_head_may_be_a_jump_target)
{
    uint8_t code[] = { OP_JTRUE, 0, 0, 0, 0, OP_STORE, 1, OP_POP, OP_RET };
    uint8_t want[] = { OP_JTRUE, 0, 0, 0, 0, OP_STORE_POP, 1, OP_RET };
    int len = (int)sizeof(code);
    ASSERT_EQ_INT(run_peep(code, &len), 1);
    ASSERT_EQ_INT(len, (int)sizeof(want));
    ASSERT(memcmp(code, want, sizeof(want)) == 0);
}

TEST(fusion_member_line_entry_maps_to_head)
{
    /* The STRUCT_REF's own line entry must not slide onto the NIL that
     * follows the pair (that would displace the NIL's entry); it belongs
     * to the fused head, where the head's entry already stands. */
    uint8_t code[] = { OP_LOAD, 0, OP_STRUCT_REF, 1, OP_NIL, OP_RET };
    CL_LineEntry lines[3];
    int n_lines = 3;
    int len = (int)sizeof(code);
    lines[0].pc = 0; lines[0].line = 10;
    lines[1].pc = 2; lines[1].line = 11;   /* at the absorbed STRUCT_REF */
    lines[2].pc = 4; lines[2].line = 12;   /* at NIL */
    ASSERT_EQ_INT(cl_peephole_run(code, &len, NULL, 0, lines, &n_lines), 1);
    ASSERT_EQ_INT(len, 5);
    ASSERT_EQ_INT(n_lines, 2);
    ASSERT_EQ_INT(lines[0].pc, 0);
    ASSERT_EQ_INT(lines[0].line, 10);
    ASSERT_EQ_INT(lines[1].pc, 3);
    ASSERT_EQ_INT(lines[1].line, 12);
}

TEST(dead_store_before_ret_removed)
{
    {
        uint8_t code[] = { OP_CONST, 0, 0, OP_STORE, 2, OP_RET };
        uint8_t want[] = { OP_CONST, 0, 0, OP_RET };
        int len = (int)sizeof(code);
        ASSERT_EQ_INT(run_peep(code, &len), 1);
        ASSERT_EQ_INT(len, (int)sizeof(want));
        ASSERT(memcmp(code, want, sizeof(want)) == 0);
    }
    {
        /* through the MV_RESET, then the LOAD fuses with it */
        uint8_t code[] = { OP_LOAD, 0, OP_STORE, 1, OP_MV_RESET, OP_RET };
        uint8_t want[] = { OP_LOAD_MV_RESET, 0, OP_RET };
        int len = (int)sizeof(code);
        ASSERT_EQ_INT(run_peep(code, &len), 1);
        ASSERT_EQ_INT(len, (int)sizeof(want));
        ASSERT(memcmp(code, want, sizeof(want)) == 0);
    }
    {
        /* a store that IS read afterwards stays */
        uint8_t code[] = { OP_STORE, 1, OP_POP, OP_NIL, OP_RET };
        uint8_t want[] = { OP_STORE_POP, 1, OP_NIL, OP_RET };
        int len = (int)sizeof(code);
        ASSERT_EQ_INT(run_peep(code, &len), 1);
        ASSERT(memcmp(code, want, sizeof(want)) == 0);
    }
}

TEST(jump_to_ret_becomes_ret)
{
    /* 0: JTRUE +6 -> T(11)  5: NIL  6: JMP +1 -> RET(12)  11: T  12: RET */
    uint8_t code[] = { OP_JTRUE, 0, 0, 0, 6, OP_NIL, OP_JMP, 0, 0, 0, 1,
                       OP_T, OP_RET };
    uint8_t want[] = { OP_JTRUE, 0, 0, 0, 2, OP_NIL, OP_RET, OP_T, OP_RET };
    int len = (int)sizeof(code);
    ASSERT_EQ_INT(run_peep(code, &len), 1);
    ASSERT_EQ_INT(len, (int)sizeof(want));
    ASSERT(memcmp(code, want, sizeof(want)) == 0);
}

TEST(return_site_returns_in_place)
{
    /* (defun f (x) (when (car x) (return-from f :early)) (cdr x)) as the
     * compiler emits it:
     *   0: LOAD 0   2: CAR   3: JNIL +11 -> 19   8: CONST 0   11: STORE 1
     *  13: POP   14: JMP +6 -> 25 (landing)   19: LOAD 0   21: CDR
     *  22: STORE 1   24: POP   25: LOAD 1   27: RET
     * Round 1: the site returns in place (STORE and POP gone, JMP -> RET);
     * round 2: the landing's LOAD is no longer a target, so STORE;POP;LOAD
     * folds to STORE, which is dead before the RET. */
    uint8_t code[] = {
        OP_LOAD, 0, OP_CAR,
        OP_JNIL, 0, 0, 0, 11,
        OP_CONST, 0, 0,
        OP_STORE, 1, OP_POP,
        OP_JMP, 0, 0, 0, 6,
        OP_LOAD, 0, OP_CDR,
        OP_STORE, 1, OP_POP,
        OP_LOAD, 1,
        OP_RET
    };
    uint8_t want[] = {
        OP_LOAD, 0, OP_CAR,
        OP_JNIL, 0, 0, 0, 4,
        OP_CONST, 0, 0,
        OP_RET,
        OP_LOAD, 0, OP_CDR,
        OP_RET
    };
    int len = (int)sizeof(code);
    ASSERT_EQ_INT(run_peep(code, &len), 1);
    ASSERT_EQ_INT(len, (int)sizeof(want));
    ASSERT(memcmp(code, want, sizeof(want)) == 0);
    /* (defun f (x) (when (car x) (return-from f :early)) x): the landing
     * resets the MV state, so the site keeps a reset in the STORE's place */
    {
        uint8_t code2[] = {
            OP_LOAD, 0, OP_CAR,
            OP_JNIL, 0, 0, 0, 11,
            OP_CONST, 0, 0,
            OP_STORE, 1, OP_POP,
            OP_JMP, 0, 0, 0, 5,
            OP_LOAD, 0,
            OP_STORE, 1, OP_POP,
            OP_LOAD, 1, OP_MV_RESET,
            OP_RET
        };
        uint8_t want2[] = {
            OP_LOAD, 0, OP_CAR,
            OP_JNIL, 0, 0, 0, 5,
            OP_CONST, 0, 0,
            OP_MV_RESET,
            OP_RET,
            OP_LOAD_MV_RESET, 0,
            OP_RET
        };
        int len2 = (int)sizeof(code2);
        ASSERT_EQ_INT(run_peep(code2, &len2), 1);
        ASSERT_EQ_INT(len2, (int)sizeof(want2));
        ASSERT(memcmp(code2, want2, sizeof(want2)) == 0);
    }
    /* without the POP (value kept on top at the site): same result, and
     * everything after the new RET is dead */
    {
        uint8_t code3[] = {
            OP_STORE, 1,
            OP_JMP, 0, 0, 0, 2,       /* -> LOAD 1 at 9 */
            OP_NIL, OP_RET,
            OP_LOAD, 1, OP_RET
        };
        uint8_t want3[] = { OP_RET };
        int len3 = (int)sizeof(code3);
        ASSERT_EQ_INT(run_peep(code3, &len3), 1);
        ASSERT_EQ_INT(len3, (int)sizeof(want3));
        ASSERT(memcmp(code3, want3, sizeof(want3)) == 0);
    }
    /* a different slot at the landing: not a return site */
    {
        uint8_t code4[] = {
            OP_STORE, 1, OP_POP,
            OP_JMP, 0, 0, 0, 2,       /* -> LOAD 2 at 10 */
            OP_NIL, OP_RET,           /* dead: nothing reaches it */
            OP_LOAD, 2, OP_RET
        };
        uint8_t want4[] = {         /* the JMP to the next live insn is gone */
            OP_STORE_POP, 1,
            OP_LOAD_RET, 2
        };
        int len4 = (int)sizeof(code4);
        ASSERT_EQ_INT(run_peep(code4, &len4), 1);
        ASSERT_EQ_INT(len4, (int)sizeof(want4));
        ASSERT(memcmp(code4, want4, sizeof(want4)) == 0);
    }
}

TEST(function_end_shape_collapses)
{
    /* The compiler's shape for (defun f (x) (if x (return-from f 0) nil)):
     *   0: LOAD 0   2: JNIL +10 -> 17   7: CONST 0   10: STORE 1
     *  12: JMP +1 -> 18 (the block landing)   17: NIL   18: STORE 1   20: RET
     * Round 1 drops the landing's dead STORE; round 2 turns the JMP into a
     * RET (its live target is now the RET) and drops the STORE before it;
     * fusion then folds LOAD;JNIL. */
    uint8_t code[] = {
        OP_LOAD, 0,
        OP_JNIL, 0, 0, 0, 10,
        OP_CONST, 0, 0,
        OP_STORE, 1,
        OP_JMP, 0, 0, 0, 1,
        OP_NIL,
        OP_STORE, 1,
        OP_RET
    };
    uint8_t want[] = {
        OP_LOAD_JNIL, 0, 0, 0, 0, 4,
        OP_CONST, 0, 0,
        OP_RET,
        OP_NIL,
        OP_RET
    };
    int len = (int)sizeof(code);
    ASSERT_EQ_INT(run_peep(code, &len), 1);
    ASSERT_EQ_INT(len, (int)sizeof(want));
    ASSERT(memcmp(code, want, sizeof(want)) == 0);
}

/* --- Layer 2: full-compiler semantic equivalence --- */

/* The bytecode object behind a function value (closure or bare bytecode). */
static CL_Bytecode *bytecode_of(CL_Obj fn)
{
    if (CL_CLOSURE_P(fn))
        fn = ((CL_Closure *)CL_OBJ_TO_PTR(fn))->bytecode;
    if (!CL_BYTECODE_P(fn)) return NULL;
    return (CL_Bytecode *)CL_OBJ_TO_PTR(fn);
}

/* Number of times opcode OP occurs in FN's code (decoded, so operand bytes
 * are never mistaken for opcodes; OP_CLOSURE stops the walk). */
static int count_opcode(CL_Obj fn, uint8_t op)
{
    CL_Bytecode *bc = bytecode_of(fn);
    uint32_t ip = 0;
    int n = 0;
    if (!bc) return -1;
    while (ip < bc->code_len) {
        const CL_OpcodeInfo *info = cl_opcode_info(bc->code[ip]);
        int olen;
        if (!info) return -1;
        if (bc->code[ip] == op) n++;
        olen = cl_opnd_len(info->operands);
        if (olen < 0) break;
        ip += 1 + (uint32_t)olen;
    }
    return n;
}

#define TWO_BINDINGS_BODY "(let ((a x) (b y)) (list b a))"

TEST(default_speed_fuses_speed0_does_not)
{
    /* The pass gates on the compile's speed high-water mark, so a body
     * (declare (optimize (speed 0))) cannot switch it off from inside a
     * function whose baseline is 1; the process-wide pin (what
     * AMILISP_FORCE_SPEED=0 sets) can. */
    extern int cl_optimize_force_speed;
    CL_Obj f1 = cl_eval_string("(lambda (x y) " TWO_BINDINGS_BODY ")");
    CL_Obj f0;
    cl_optimize_force_speed = 0;
    f0 = cl_eval_string("(lambda (x y) " TWO_BINDINGS_BODY ")");
    cl_optimize_force_speed = -1;
    ASSERT(count_opcode(f1, OP_STORE_POP) >= 1);      /* the first binding */
    ASSERT_EQ_INT(count_opcode(f0, OP_STORE_POP), 0);
    ASSERT(count_opcode(f0, OP_STORE) >= 2);          /* as emitted */
    ASSERT(truthy("(equal (funcall (lambda (x y) " TWO_BINDINGS_BODY ") 1 2)"
                  " '(2 1))"));
    cl_optimize_force_speed = 0;
    ASSERT(truthy("(equal (funcall (lambda (x y) " TWO_BINDINGS_BODY ") 1 2)"
                  " '(2 1))"));
    cl_optimize_force_speed = -1;
}

TEST(no_fuse_switch_keeps_pairs_but_still_rewrites)
{
    CL_Obj f;
    cl_peephole_fuse_enabled = 0;
    f = cl_eval_string("(lambda (x y) " TWO_BINDINGS_BODY ")");
    cl_peephole_fuse_enabled = 1;
    ASSERT_EQ_INT(count_opcode(f, OP_STORE_POP), 0);
    ASSERT_EQ_INT(count_opcode(f, OP_LOAD_LOAD), 0);
    /* the deleting rewrites still ran: no STORE left before the final RET */
    {
        CL_Bytecode *bc = bytecode_of(f);
        ASSERT(bc != NULL);
        ASSERT(bc->code_len >= 3);
        ASSERT(bc->code[bc->code_len - 1] == OP_RET);
        ASSERT(bc->code[bc->code_len - 3] != OP_STORE);
    }
}

TEST(fused_functions_same_results_as_speed0)
{
    /* One body exercising every fused shape, compiled at speed 0 and at
     * the default; both must agree, including the error paths. */
#define FUSE_BODY \
    "(let ((a (car x)) (b (cdr y)))" \
    "  (if (eq a b) (list a b (pt-x p))" \
    "      (if *fuse-flag* (pt-y p) (if a a (progn (setq a b) a)))))"
    ASSERT(truthy(
        "(progn"
        " (defvar *fuse-flag* nil)"
        " (defstruct pt x y)"
        " (defun fz-0 (x y p) (declare (optimize (speed 0))) " FUSE_BODY ")"
        " (defun fz-1 (x y p) " FUSE_BODY ")"
        " (let ((p (make-pt :x 3 :y 4)))"
        "   (and (equal (fz-0 '(1) '(2 . 1) p) (fz-1 '(1) '(2 . 1) p))"
        "        (equal (fz-0 '(1) '(2 . 2) p) (fz-1 '(1) '(2 . 2) p))"
        "        (equal (fz-0 '(nil) '(2 . 2) p) (fz-1 '(nil) '(2 . 2) p))"
        "        (let ((*fuse-flag* t))"
        "          (equal (fz-0 '(1) '(2 . 2) p) (fz-1 '(1) '(2 . 2) p)))"
        "        (eq (handler-case (fz-1 '(1) '(2 . 1) 5) (type-error () :te)) :te)"
        "        (eq (handler-case (fz-1 5 '(2 . 1) p) (type-error () :te)) :te))))"));
#undef FUSE_BODY
    /* GLOAD_JNIL on an unbound special is still the unbound-variable error */
    ASSERT(truthy(
        "(progn"
        " (defvar *fuse-unbound*)"
        " (defun fz-u () (if *fuse-unbound* 1 2))"
        " (eq (handler-case (fz-u) (unbound-variable () :ub)) :ub))"));
    /* LOAD_CALL_GLOBAL: redefinition reaches the fused site; undefined is
     * catchable; multiple values pass through */
    ASSERT(truthy(
        "(progn"
        " (defun fz-callee (a) (values a (* 2 a)))"
        " (defun fz-caller (a) (multiple-value-list (fz-callee a)))"
        " (and (equal (fz-caller 2) '(2 4))"
        "      (progn (defun fz-callee (a) (list :new a)) t)"
        "      (equal (fz-caller 2) '((:new 2)))"
        "      (eq (handler-case (funcall (lambda (q) (fz-nope q)) 1)"
        "            (undefined-function () :uf)) :uf)))"));
    /* LOAD_RET / LOAD_MV_RESET: a returned local is exactly one value */
    ASSERT(truthy(
        "(progn"
        " (defun fz-r (x) (let ((v (floor x 2))) v))"
        " (equal (multiple-value-list (fz-r 7)) '(3)))"));
    /* round two: LOAD_STORE_POP, LOAD_CONST, GLOAD_EQ_JNIL,
     * GLOAD_CALL_GLOBAL, POP_LOAD */
    ASSERT(truthy(
        "(progn"
        " (defvar *fz-g* :g)"
        " (defvar *fz-unb*)"
        " (defun fz-r2 (a b)"
        "   (let ((c a) (d 0))"
        "     (setq d c)"
        "     (list (if (eq b *fz-g*) (list c :k) (list c :n))"
        "           (cadr (list d *fz-g*)) (progn (list 1) d))))"
        " (defun fz-r2u (a) (if (eq a *fz-unb*) 1 2))"
        " (defun fz-r2c () (car (list *fz-unb*)))"
        " (and (equal (fz-r2 1 :g) '((1 :k) :g 1))"
        "      (equal (fz-r2 2 :x) '((2 :n) :g 2))"
        "      (let ((*fz-g* :x)) (equal (fz-r2 2 :x) '((2 :k) :x 2)))"
        "      (eq (handler-case (fz-r2u 1) (unbound-variable () :ub)) :ub)"
        "      (eq (handler-case (fz-r2c) (unbound-variable () :ub)) :ub)))"));
}

TEST(speed3_defun_same_results)
{
    ASSERT(truthy(
        "(progn"
        " (defun ph-f1 (x) (setq x (+ x 1)) (if (not (< x 10)) :big :small))"
        " (defun ph-f3 (x) (declare (optimize (speed 3)))"
        "   (setq x (+ x 1)) (if (not (< x 10)) :big :small))"
        " (and (eq (ph-f1 1) (ph-f3 1))"
        "      (eq (ph-f1 42) (ph-f3 42))))"));
}

TEST(speed3_body_declare_triggers_pass)
{
    /* The body (declare (optimize (speed 3))) is scope-restored before
     * finalization — the high-water mark must still trigger the pass.
     * Observable via behavior only: results must stay correct. */
    ASSERT(truthy(
        "(progn"
        " (defun ph-hw (n) (declare (optimize (speed 3)))"
        "   (let ((s 0)) (dotimes (i n) (setq s (+ s i))) s))"
        " (= (ph-hw 100) 4950))"));
}

TEST(speed3_discarded_car_still_signals)
{
    /* ANSI: (car 5) signals type-error even when its value is discarded —
     * OP_CAR must never be treated as deletable. */
    ASSERT(truthy(
        "(progn"
        " (defun ph-car3 (x) (declare (optimize (speed 3)))"
        "   (progn (car x) nil))"
        " (handler-case (progn (ph-car3 5) nil)"
        "   (type-error () t)))"));
}

TEST(speed3_multiple_values_preserved)
{
    ASSERT(truthy(
        "(progn"
        " (defun ph-mv3 () (declare (optimize (speed 3)))"
        "   (progn 42 (values 1 2 3)))"
        " (equal (multiple-value-list (ph-mv3)) '(1 2 3)))"));
    ASSERT(truthy(
        "(progn"
        " (defun ph-mv1 () (declare (optimize (speed 3)))"
        "   (values-list '(1 2)) 99)"
        " (equal (multiple-value-list (ph-mv1)) '(99)))"));
}

TEST(speed3_nlx_forms_survive_relocation)
{
    ASSERT(truthy(
        "(progn"
        " (defun ph-tag3 (n) (declare (optimize (speed 3)))"
        "   (let ((s 0) (i 0))"
        "     (tagbody"
        "      top (when (>= i n) (go done))"
        "          (setq s (+ s i)) (setq i (+ i 1)) (go top)"
        "      done)"
        "     s))"
        " (= (ph-tag3 100) 4950))"));
    ASSERT(truthy(
        "(progn"
        " (defun ph-catch3 (x) (declare (optimize (speed 3)))"
        "   (catch 'ph-tag (when x (throw 'ph-tag :thrown)) :fell))"
        " (and (eq (ph-catch3 t) :thrown) (eq (ph-catch3 nil) :fell)))"));
    ASSERT(truthy(
        "(progn"
        " (defvar *ph-cleanup* nil)"
        " (defun ph-uw3 (x) (declare (optimize (speed 3)))"
        "   (setq *ph-cleanup* nil)"
        "   (catch 'ph-out"
        "     (unwind-protect (when x (throw 'ph-out :out))"
        "       (setq *ph-cleanup* t)))"
        "   *ph-cleanup*)"
        " (and (ph-uw3 t) (ph-uw3 nil)))"));
    ASSERT(truthy(
        "(progn"
        " (defun ph-blk3 (x) (declare (optimize (speed 3)))"
        "   (block b (when x (return-from b :early)) :late))"
        " (and (eq (ph-blk3 t) :early) (eq (ph-blk3 nil) :late)))"));
}

TEST(speed3_closures_and_loops)
{
    ASSERT(truthy(
        "(progn"
        " (defun ph-clo3 (n) (declare (optimize (speed 3)))"
        "   (let ((acc nil))"
        "     (dotimes (i n) (push (let ((j i)) (lambda () j)) acc))"
        "     (let ((s 0)) (dolist (f acc) (setq s (+ s (funcall f)))) s)))"
        " (= (ph-clo3 10) 45))"));
}

TEST(speed3_not_under_if_and_while)
{
    ASSERT(truthy(
        "(progn"
        " (defun ph-not3 (xs) (declare (optimize (speed 3)))"
        "   (let ((n 0))"
        "     (dolist (x xs)"
        "       (unless (not (numberp x)) (setq n (+ n 1))))"
        "     n))"
        " (= (ph-not3 '(1 a 2 b 3)) 3))"));
}

TEST(speed1_default_results_correct)
{
    /* At default speed the pass runs too (since 4.3): a canary with an
     * obviously removable pattern keeps its exact behavior. */
    ASSERT(truthy(
        "(progn"
        " (defun ph-s1 (x) (setq x (+ x 1)) x)"
        " (= (ph-s1 41) 42))"));
}

int main(void)
{
    test_init();
    setup();

    RUN(opcode_info_exhaustive_and_rejects_gaps);
    RUN(decoder_knows_every_opcode);
    RUN(store_pop_load_becomes_store);
    RUN(store_pop_load_different_slot_not_elided);
    RUN(pure_pop_elided_when_mv_write_masked);
    RUN(pure_pop_kept_when_mv_observer_follows);
    RUN(pure_pop_kept_before_ret);
    RUN(load_pop_elided_without_mv_proof);
    RUN(cascaded_pairs_reach_fixpoint);
    RUN(not_jnil_fused_to_jtrue_and_relocated);
    RUN(not_branch_keeps_mv_write_when_unprovable);
    RUN(jump_threading_and_dead_jump_removal);
    RUN(dead_code_after_jmp_removed);
    RUN(backward_jump_relocated_across_deletion);
    RUN(backward_jump_detection);
    RUN(catch_landing_pad_relocated);
    RUN(line_map_remapped_and_deduped);
    RUN(bails_on_unknown_opcode);
    RUN(bails_on_truncated_operand);
    RUN(bails_on_jump_landing_mid_instruction);
    RUN(bails_on_closure_with_non_bytecode_constant);
    RUN(jump_into_deleted_pair_retargets_forward);
    RUN(jump_landing_inside_pattern_blocks_it);
    RUN(handler_case_landing_table_pinned);

    RUN(jump_to_next_deleted_conditional_kept);
    RUN(fuse_cmp_br_every_comparison_both_polarities);
    RUN(cmp_br_not_fused_when_branch_is_a_target_and_backward_offset);
    RUN(fuse_store_pop);
    RUN(fuse_load_load_and_load_call_global_greedy);
    RUN(fuse_load_struct_ref_load_mv_reset_load_ret);
    RUN(fuse_eq_jnil_backward_offset_relocated);
    RUN(fuse_gload_jnil_and_load_jnil_forward);
    RUN(fuse_round_two_pairs_and_triples);
    RUN(fusion_blocked_by_jump_onto_second_member);
    RUN(fusion_head_may_be_a_jump_target);
    RUN(fusion_member_line_entry_maps_to_head);
    RUN(dead_store_before_ret_removed);
    RUN(jump_to_ret_becomes_ret);
    RUN(return_site_returns_in_place);
    RUN(function_end_shape_collapses);

    RUN(speed3_defun_same_results);
    RUN(speed3_body_declare_triggers_pass);
    RUN(speed3_discarded_car_still_signals);
    RUN(speed3_multiple_values_preserved);
    RUN(speed3_nlx_forms_survive_relocation);
    RUN(speed3_closures_and_loops);
    RUN(speed3_not_under_if_and_while);
    RUN(speed1_default_results_correct);
    RUN(default_speed_fuses_speed0_does_not);
    RUN(no_fuse_switch_keeps_pairs_but_still_rewrites);
    RUN(fused_functions_same_results_as_speed0);

    teardown();
    REPORT();
}
