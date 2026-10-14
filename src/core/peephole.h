#ifndef CL_PEEPHOLE_H
#define CL_PEEPHOLE_H

/*
 * Bytecode peephole post-pass (spec 1.8) and superinstruction fusion
 * (spec 4.3), run at (optimize (speed >= 1)) — only (speed 0) keeps the
 * bytecode as emitted.  See peephole.c for the full design notes.
 * Fail-safe: on anything it does not fully understand it leaves the
 * bytecode untouched.
 */

#include <stdint.h>
#include "types.h"   /* CL_Obj, CL_LineEntry */

/* Functions larger than this skip the pass (the pc-keyed line map is
 * uint16_t anyway, and the temporary instruction list for a buffer this
 * size is already ~1.2MB — too much to ask of an 8MB Amiga). */
#define CL_PEEPHOLE_MAX_CODE 60000u

struct CL_Compiler_s;

/* Optimize C's emitted bytecode in place if its effective (declare
 * (optimize (speed ...))) high-water mark is >= 1.  Called from the two
 * bytecode finalization sites (compile_lambda, cl_compile_env) after the
 * final OP_RET/OP_HALT is emitted and all jumps are patched. */
void cl_peephole_optimize(struct CL_Compiler_s *c);

/* Process-wide switch for the superinstruction fusion step alone
 * (AMILISP_NO_FUSE=1 clears it in main.c): the deleting rewrites still run,
 * the compiler's own opcodes are left unfused.  For A/B runs and bisects. */
extern int cl_peephole_fuse_enabled;

/* Raw engine (exposed for unit tests): rewrite CODE[0..*CODE_LEN) in place,
 * shrink-only.  CONSTANTS/N_CONSTANTS are needed to decode OP_CLOSURE's
 * variable-length capture descriptors.  LINES/N_LINES (may be NULL/NULL) is
 * the pc-keyed source line map, remapped in place.  Returns 1 if the code
 * changed, 0 if nothing applied or the pass bailed out. */
int cl_peephole_run(uint8_t *code, int *code_len,
                    const CL_Obj *constants, int n_constants,
                    CL_LineEntry *lines, int *n_lines);

/* 1 when CODE[0..LEN) contains a jump (or NLX landing offset) that points
 * backwards -- a loop.  The JIT's hot compilation compiles such a function
 * on its first call instead of waiting for the call count (jit.h).
 * Decodes like the pass itself; bytecode it does not understand answers 0.
 * CONSTANTS/N_CONSTANTS as for cl_peephole_run. */
int cl_bytecode_has_backward_jump(const uint8_t *code, uint32_t len,
                                  const CL_Obj *constants, int n_constants);

#endif /* CL_PEEPHOLE_H */
