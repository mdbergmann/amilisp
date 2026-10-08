#ifndef COMPILER_INTERNAL_H
#define COMPILER_INTERNAL_H

/*
 * Internal header shared between compiler translation units.
 * Not part of the public API — use compiler.h for that.
 */

#include "compiler.h"
#include "env.h"
#include <stdlib.h>
#include "opcodes.h"
#include "symbol.h"
#include "package.h"
#include "mem.h"
#include "float.h"
#include "vm.h"
#include "error.h"
#include "vm.h"
#include "debugger.h"
#include "reader.h"
#include "../platform/platform.h"
#include "printer.h"
#include "builtins.h"
#include <string.h>
#include <stdio.h>

/* Block tracking for return/return-from.  Trivia's MATCH macroexpansion
 * produces 30+ nested BLOCKs and serapeum dispatch-case can chain even
 * more, so this limit needs significant headroom.  256 covers the cases
 * we've seen in the wild (trivia, serapeum, cl-conspack); compile_block
 * raises a clean error on overflow rather than corrupting the adjacent
 * block_count field via OOB write. */
#define CL_MAX_BLOCKS 256
#define CL_MAX_BLOCK_PATCHES 16
/* Tagbodies and outer-tag tables don't see the same nesting depth in
 * practice (real programs rarely nest tagbody more than a few levels),
 * so they keep tighter limits to avoid bloating CL_Compiler size. */
#define CL_MAX_TAGBODIES 16
#define CL_MAX_OUTER_TAGS 64

/* Lexically-active (declare (notinline f ...)) operators.  Tracked so the
 * compiler can suppress compiler-macro expansion for those names (CLHS
 * 3.2.2.1.3): the standard compiler-macro fallback idiom expands
 *   (f ...) -> (locally (declare (notinline f)) (... (f ...)))
 * and relies on notinline to stop the macro re-firing on the inner call.
 * Without honoring it the expansion recurses forever.  Rare in practice, so
 * a small cap is plenty; overflow just falls back to the old behavior. */
#define CL_MAX_NOTINLINE 32

/* Tail-trampoline frame kinds.  When compile_expr trampolines on a form
 * that has a tail child (e.g. LET's last body form, PROGN's last form),
 * it pushes one of these onto the compiler's tail stack so the postlude
 * can be emitted after the chain unwinds.  Without this, the recursive
 * compile_expr → compile_let → compile_progn → compile_expr call chain
 * grows one C frame per source-level LET, exhausting the C stack on
 * deeply nested macro expansions (e.g. serapeum's optimized
 * dispatch-case output). */
typedef enum {
    CL_TAIL_PROGN = 0,             /* no postlude needed; placeholder */
    CL_TAIL_LET,                   /* pop locals, OP_DYNUNBIND if any specials */
    CL_TAIL_BLOCK_LOCAL,           /* local-jump block: store result, patch exits */
    CL_TAIL_BLOCK_NLX,             /* NLX block: emit OP_BLOCK_POP, patch jumps */
    CL_TAIL_RETURN_FROM_LOCAL,     /* DYNUNBIND, store result, JMP w/ patch */
    CL_TAIL_RETURN_FROM_NLX,       /* OP_BLOCK_RETURN tag_idx */
    CL_TAIL_OUTER_RETURN_FROM,     /* cross-closure: OP_BLOCK_RETURN tag_idx */
    CL_TAIL_LOCALLY,               /* no postlude — pure body wrapper */
    CL_TAIL_SYMBOL_MACROLET,       /* restore env->symbol_macro_count */
    CL_TAIL_MACROLET,              /* restore env->local_macro_count */
    CL_TAIL_PROGV,                 /* emit OP_PROGV_UNBIND, restore in_tail */
    CL_TAIL_IF_AFTER_THEN,         /* emit JMP, patch jnil_pos; dispatch ELSE (or emit NIL) */
    CL_TAIL_IF_AFTER_ELSE,         /* patch the JMP that bypasses ELSE (block_push_pos) */
    CL_TAIL_HANDLER_BIND,          /* emit OP_HANDLER_POP <count> */
    CL_TAIL_MULTIPLE_VALUE_BIND,   /* clear_boxed + restore env->local_count, in_tail */
    CL_TAIL_FLET,                  /* clear_boxed + restore local_count + local_fun_count */
    CL_TAIL_LABELS,                /* same as FLET (separate kind for symmetry/debug) */
    CL_TAIL_EVAL_WHEN,             /* no postlude — pure body wrapper */
    CL_TAIL_PROGN_ITER             /* emit OP_POP, dispatch next body form */
} CL_TailKind;

typedef struct {
    uint8_t  kind;
    /* LET postlude state (also reused: saved_local_count for BLOCK_LOCAL,
     * saved_tail for RETURN_FROM_*, etc.) */
    int      saved_local_count;
    int      special_count;
    int      saved_tail;
    int      n_gc_roots;             /* CL_GC_PROTECT calls to undo on drain */
    /* BLOCK / RETURN-FROM state */
    int      saved_block_count;      /* index of CL_BlockInfo for BLOCK */
    int      block_push_pos;         /* OP_BLOCK_PUSH offset slot (NLX block) */
    int      tag_idx;                /* constant index for NLX tag */
    int      result_slot;            /* local slot storing block result */
    int      unwind_count;           /* DYNUNBIND count for RETURN-FROM */
    int      bi_index;               /* CL_BlockInfo index for RETURN-FROM */
    /* MACROLET / SYMBOL-MACROLET state */
    int      saved_macro_count;      /* env->local_macro_count (MACROLET)
                                      * or env->symbol_macro_count (SYMBOL_MACROLET) */
    /* (declare (notinline ...)) scope: count to restore in the postlude so
     * notinline names declared in a body stop applying once the body ends. */
    int      saved_notinline_count;
    /* (declare (optimize ...)) scope: effective settings at body entry,
     * snapshotted unconditionally by cl_tail_push and restored by the
     * postludes of declaration-accepting bodies (LET, LOCALLY, FLET, ...)
     * so a body-local optimize declaration ends with the body (CLHS 3.3.4)
     * instead of leaking into sibling forms. */
    CL_OptimizeSettings saved_optimize;
    /* Multiple-values bookkeeping for control-flow joins: c->mv_state as of
     * the arm compiled BEFORE the postlude runs (IF_AFTER_ELSE stores
     * THEN's).  The postlude merges it — see cl_mv_join. */
    int      mv_state;
    /* Continuation form to dispatch when this postlude drains (used by
     * IF_AFTER_THEN to carry the ELSE form across THEN's compilation).
     * GC-traced via cl_compiler_gc_visit_thread's tail_stack walk.
     * CL_NIL when not used. */
    CL_Obj   cont_form;
} CL_TailFrame;

typedef struct {
    CL_Obj tag;
    int exit_patches[CL_MAX_BLOCK_PATCHES];
    int n_patches;
    int result_slot;  /* local slot where return value is stored */
    int uses_nlx;     /* 1 if NLX-based (compile_block), 0 if local-jump (loop forms) */
    int dyn_depth;    /* special binding depth at block entry (for local-jump unwinding) */
    /* Merge of c->mv_state over every local-jump RETURN-FROM exit into this
     * block; the block's postlude merges it with the fall-through path's.
     * exit_stale_mask marks which exit_patches[] entries were stale, so the
     * postlude can route just those through an OP_MV_RESET. */
    int mv_state;
    uint32_t exit_stale_mask;
} CL_BlockInfo;

/* Tagbody tracking for go */
#define CL_MAX_TAGBODY_TAGS 32

typedef struct {
    CL_Obj tag;
    int code_pos;          /* bytecode offset, -1 if forward */
    int forward_patches[CL_MAX_BLOCK_PATCHES];
    int n_forward;
} CL_TagInfo;

typedef struct {
    CL_TagInfo tags[CL_MAX_TAGBODY_TAGS];
    int n_tags;
    CL_Obj id;             /* unique tagbody identifier (for NLX) */
    int uses_nlx;          /* 1 if tagbody needs NLX support */
} CL_TagbodyInfo;

/* Outer tagbody tag info for cross-closure GO */
typedef struct {
    CL_Obj tag;            /* the go tag (symbol/integer) */
    CL_Obj tagbody_id;     /* identifier of the enclosing tagbody */
    int tag_index;         /* index of the tag within the tagbody */
} CL_OuterTagInfo;

/* Source line map tracking during compilation */
#define CL_MAX_LINE_ENTRIES 256

/* Parsed lambda list structure */
typedef struct {
    CL_Obj required[CL_MAX_LOCALS];
    int n_required;
    CL_Obj opt_names[CL_MAX_LOCALS];
    CL_Obj opt_defaults[CL_MAX_LOCALS]; /* CL_NIL if no default */
    CL_Obj opt_suppliedp[CL_MAX_LOCALS]; /* supplied-p var or CL_NIL */
    int n_optional;
    CL_Obj rest_name;       /* CL_NIL if no &rest */
    int has_rest;
    CL_Obj key_names[CL_MAX_LOCALS];
    CL_Obj key_keywords[CL_MAX_LOCALS]; /* keyword symbols */
    CL_Obj key_defaults[CL_MAX_LOCALS];
    CL_Obj key_suppliedp[CL_MAX_LOCALS]; /* supplied-p var or CL_NIL */
    int n_keys;
    int allow_other_keys;
    CL_Obj aux_names[CL_MAX_LOCALS];
    CL_Obj aux_inits[CL_MAX_LOCALS]; /* init form or CL_NIL */
    int n_aux;
} CL_ParsedLambdaList;

/* Compiler state */
typedef struct CL_Compiler_s {
    struct CL_Compiler_s *parent; /* for GC root chain */
    /* C-stack frame that owns this compiler (CL_CAPTURE_SP() in the pushing
     * function).  cl_compiler_unwind_to frees a compiler at an NLX landing
     * only if this anchor is strictly deeper than the landing frame — i.e.
     * the owning frame was abandoned by the longjmp.  Depth comparison
     * relies on the stack-grows-down ABI assumption documented at
     * CL_CAPTURE_SP (thread.h). */
    void *anchor;
    /* Bytecode and constants being built.  Both are platform_alloc'd and
     * grown on demand up to CL_MAX_CODE_SIZE / CL_MAX_CONSTANTS by cl_emit
     * and cl_add_constant, the only two places that append; everything else
     * indexes below code_pos / const_count.  A pooled block keeps them
     * across compiles (see cl_compiler_pool_acquire). */
    uint8_t *code;
    CL_Obj *constants;
    int code_cap;
    int const_cap;
    int code_pos;
    int const_count;
    CL_CompEnv *env;
    int in_tail;      /* Are we in tail position? */
    /* Multiple-values hygiene — one of CL_MV_STALE / CL_MV_ONE / CL_MV_MANY
     * describing the MV state (cl_mv_count / cl_mv_values) at the current
     * emission point.  See the enum and cl_mv_normalize. */
    int mv_state;
    int special_depth; /* Current number of active dynamic bindings */
    CL_BlockInfo blocks[CL_MAX_BLOCKS];
    int block_count;
    CL_TagbodyInfo tagbodies[CL_MAX_TAGBODIES];
    int tagbody_count;
    /* Source location tracking */
    CL_LineEntry line_entries[CL_MAX_LINE_ENTRIES];
    int line_entry_count;
    int current_line;   /* Current source line being compiled */
    /* Outer block names visible from enclosing scopes (for cross-closure return-from) */
    CL_Obj outer_blocks[CL_MAX_BLOCKS];
    int outer_block_count;
    /* Outer tagbody tags visible from enclosing scopes (for cross-closure go) */
    CL_OuterTagInfo outer_tags[CL_MAX_OUTER_TAGS];
    int outer_tag_count;
    /* Lambda compilation scratch (in struct to avoid stack overflow on AmigaOS) */
    CL_ParsedLambdaList ll;
    int key_slot_indices[CL_MAX_LOCALS];
    int key_suppliedp_indices[CL_MAX_LOCALS];
    CL_Obj param_vars[CL_MAX_LOCALS];
    int param_slots[CL_MAX_LOCALS];
    uint8_t lambda_needs_boxing[CL_MAX_LOCALS];
    /* Tail-trampoline stack — heap-allocated, grown on demand */
    CL_TailFrame *tail_stack;
    int tail_count;
    int tail_capacity;
    /* Lexically-active notinline operators (see CL_MAX_NOTINLINE).  A simple
     * stack scoped by saved_notinline_count markers in body postludes. */
    CL_Obj notinline_fns[CL_MAX_NOTINLINE];
    int notinline_count;
    /* Effective (declare (optimize ...)) settings for this compile chain.
     * Exclusively owned by the thread running this compile (CL_Compiler
     * structs are never shared across threads — see cl_active_compiler),
     * so no lock is needed to read/write it, unlike the proclaimed
     * baseline cl_optimize_global.  Seeded from the parent compiler (or
     * cl_optimize_global at the root of a fresh top-level compile) when
     * this struct is pushed onto the active-compiler chain; see
     * cl_compiler_seed_optimize_settings in compiler.c. */
    CL_OptimizeSettings optimize_settings;
    /* High-water mark of optimize_settings.speed over this compile.  Body
     * (declare (optimize ...)) settings are restored by the declaring
     * scope's postlude, so by finalization time optimize_settings.speed may
     * be back below the level the body asked for; the peephole post-pass
     * (peephole.c) gates on this mark instead so (defun f () (declare
     * (optimize (speed 3))) ...) still gets optimized.  Updated wherever
     * speed is written: cl_compiler_seed_optimize_settings and
     * cl_process_declaration_specifier. */
    uint8_t peep_speed_max;
    /* Local-function inlining (compiler_special.c).  hide_*: the BLOCK /
     * TAGBODY twins of CL_CompEnv's hiding bands — entries in [lo, hi) of
     * blocks[] / tagbodies[] are invisible to RETURN-FROM / GO while an
     * inlined body compiles.  funuse_*: scratch state of the escape scan
     * (one nlx_scan walk per FLET/LABELS form): the candidate names are
     * env->local_funs[funuse_lo .. funuse_lo + funuse_n), bit i of
     * funuse_escaped is set when candidate i must keep a closure, and
     * funuse_calls[i] counts its direct call sites (saturating).
     * funuse_cur is the candidate whose body the walk is inside (-1 for
     * the form's own body): a LOAD-TIME-VALUE found there vetoes that
     * candidate, since inlining would compile — and so evaluate — the
     * form once per call site.  Lives here rather than in statics because
     * compiles run concurrently on several threads and nest through
     * macro expansion. */
    int hide_block_lo, hide_block_hi;
    int hide_tagbody_lo, hide_tagbody_hi;
    int funuse_lo, funuse_n, funuse_cur;
    uint32_t funuse_escaped;
    uint8_t funuse_calls[32];
} CL_Compiler;

/* Local-function inlining knobs (compiler_special.c).  A local function
 * is compiled inline when it is called with a simple lambda list from its
 * own body scope only (never #'-referenced, never called from a nested
 * closure, never recursive) AND either has a single call site or a body of
 * at most CL_LOCAL_INLINE_MAX_SIZE conses; at most CL_LOCAL_INLINE_MAX_PARAMS
 * parameters.  cl_local_inline_enabled is the process-wide switch
 * (CLAMIGA_NO_LOCAL_INLINE=1 clears it — for A/B runs and bug isolation). */
#define CL_LOCAL_INLINE_MAX_PARAMS 8
#define CL_LOCAL_INLINE_MAX_SIZE   48
#define CL_LOCAL_INLINE_MAX_NAMES  32
extern int cl_local_inline_enabled;
/* CLAMIGA::%LOCAL-ARITY-ERROR (builtins.c): what an inlined call site with
 * the wrong argument count compiles to, so the runtime error matches the
 * one the closure call would have signalled. */
extern CL_Obj cl_local_arity_error_sym;
void compile_local_inline_call(CL_Compiler *c, int fun_idx, CL_Obj args,
                               int saved_tail);

/* --- Shared globals (defined in compiler.c) --- */

/* Cached, GC-registered symbols for the destructuring-bind runtime helpers
 * (%DBIND-TOO-FEW / %DBIND-TOO-MANY arity errors, %DBIND-CHECK-KEYS unknown-
 * keyword check).  Interned once at startup (cl_builtins_init) so the
 * compiler can emit the guard calls without allocating — keeping the
 * required-parameter walk of compile_destructure_pattern allocation-free, and
 * thus free of any GC root that could leak across a compile-time cl_error
 * longjmp. */
extern CL_Obj cl_dbind_too_few_sym;
extern CL_Obj cl_dbind_too_many_sym;
extern CL_Obj cl_dbind_check_keys_sym;

extern CL_Obj macro_table;
extern CL_Obj setf_table;
extern CL_Obj setf_fn_table;
/* Synthesize the package-qualified hidden CLAMIGA symbol that stores a
 * (setf ACCESSOR) function (see compiler.c). */
CL_Obj cl_setf_store_symbol(CL_Obj accessor);
extern CL_Obj setf_expander_table;
extern CL_Obj type_table;
extern CL_Obj SETF_SYM_CAR, SETF_SYM_CDR, SETF_SYM_FIRST, SETF_SYM_REST;
extern CL_Obj SETF_SYM_NTH, SETF_SYM_AREF, SETF_SYM_SVREF, SETF_SYM_CHAR, SETF_SYM_SCHAR;
extern CL_Obj SETF_SYM_SYMBOL_VALUE, SETF_SYM_SYMBOL_FUNCTION, SETF_SYM_FDEFINITION;
extern CL_Obj SETF_HELPER_NTH, SETF_HELPER_SV, SETF_HELPER_SF;
extern CL_Obj SETF_SYM_GETHASH, SETF_HELPER_GETHASH;
extern CL_Obj SETF_HELPER_AREF;
extern CL_Obj SETF_SYM_ROW_MAJOR_AREF, SETF_HELPER_ROW_MAJOR_AREF;
extern CL_Obj SETF_SYM_FILL_POINTER, SETF_HELPER_FILL_POINTER;
extern CL_Obj SETF_SYM_BIT, SETF_HELPER_BIT;
extern CL_Obj SETF_SYM_SBIT, SETF_HELPER_SBIT;
extern CL_Obj SETF_SYM_GET, SETF_HELPER_GET;
extern CL_Obj SYM_LEX_LOCAL_MACRO;

/* --- Emit helpers (defined in compiler.c) --- */

void cl_emit(CL_Compiler *c, uint8_t byte);
void cl_emit_u16(CL_Compiler *c, uint16_t val);
void cl_emit_i16(CL_Compiler *c, int16_t val);
void cl_emit_i32(CL_Compiler *c, int32_t val);
int cl_add_constant(CL_Compiler *c, CL_Obj obj);
void cl_emit_const(CL_Compiler *c, CL_Obj obj);
int cl_emit_jump(CL_Compiler *c, uint8_t op);
void cl_patch_jump(CL_Compiler *c, int patch_pos);

/* --- Multiple-values hygiene (CLHS 3.1.7 / 5.1 / 5.3) ---
 *
 * Only a handful of opcodes OBSERVE the multiple-value state (cl_mv_count /
 * cl_mv_values): OP_RET and OP_HALT hand it to the caller, OP_MV_TO_LIST,
 * OP_MV_LOAD and OP_NTH_VALUE read the buffer, OP_BLOCK_RETURN snapshots it
 * into the NLX frame.  Making every value-producing opcode write
 * `cl_mv_count = 1` would be correct but costs a store per OP_LOAD — and in
 * the m68k JIT a whole JSR (specs/native-backend.md).  So the compiler
 * instead tracks what the state means at each emission point and drops a
 * single OP_MV_RESET in front of an observer that would otherwise read some
 * earlier form's values.
 *
 *   CL_MV_MANY   the state describes THIS form's values and there may be
 *                more than one of them (the value came out of a call).
 *                Never reset — that is what makes (floor 7 2) two-valued.
 *   CL_MV_ONE    the state describes this form's single value (constant,
 *                arithmetic, OP_GLOAD, an explicit OP_MV_RESET, …).
 *                Resetting is a no-op, which is what lets a join keep it.
 *   CL_MV_STALE  the value was pushed by an opcode that leaves the buffer
 *                alone — OP_LOAD, OP_CELL_REF, the peeking stores — so the
 *                buffer still describes some EARLIER form.  Must be reset
 *                before an observer.
 *
 * compile_expr_step re-asserts CL_MV_MANY for every form, so a handler that
 * forgets to classify its result leaves the pre-existing leak in place
 * instead of emitting an OP_MV_RESET that would DESTROY real values. */
#define CL_MV_STALE 0
#define CL_MV_ONE   1
#define CL_MV_MANY  2

/* Emit OP_CALL / OP_TAILCALL with its u8 argument count.  Always use this
 * rather than two cl_emit calls: it is where a call's result is classified
 * CL_MV_MANY (the callee owns the MV state). */
void cl_emit_call(CL_Compiler *c, uint8_t op, uint8_t nargs);
void cl_emit_call_global(CL_Compiler *c, uint8_t op, uint16_t sym_idx,
                         uint8_t nargs);

/* Emit OP_MV_RESET if the state is CL_MV_STALE.  Call immediately before
 * any opcode that observes the MV state. */
void cl_mv_normalize(CL_Compiler *c);
/* Merge a control-flow join: the value arriving here came either from the
 * arm just compiled (c->mv_state) or from an already-compiled arm (OTHER). */
void cl_mv_join(CL_Compiler *c, int other);
/* The merge itself, for accumulating over N arms (CL_MV_ONE = identity). */
int cl_mv_merge(int a, int b);
/* Patch a join fed by two pending-jump chains (arms that left the state
 * self-describing / stale) plus the falling-through arm in c->mv_state,
 * giving the stale arms their own OP_MV_RESET when the arms disagree.
 * ANY_STALE and MERGED are accumulated by the caller over all arms. */
void cl_mv_close_join(CL_Compiler *c, int ok_chain, int stale_chain,
                      int any_stale, int merged);
/* Same, for a join fed by an explicit patch array (local-jump BLOCK / loop
 * exits); bit i of STALE_MASK marks patches[i] as a stale arm. */
void cl_mv_close_patch_join(CL_Compiler *c, const int *patches, int n,
                            uint32_t stale_mask, int merged);

/* Pending-jump chain threaded through the operand placeholders (see
 * compiler.c).  Chain heads start at CL_JUMP_CHAIN_END; cl_emit_jump_chain
 * returns the new head. */
#define CL_JUMP_CHAIN_END (-1)
int cl_emit_jump_chain(CL_Compiler *c, uint8_t op, int chain);
void cl_patch_jump_chain(CL_Compiler *c, int chain);
void cl_emit_loop_jump(CL_Compiler *c, uint8_t op, int target);

/* --- Compile functions shared across files --- */

/* compiler.c */
void compile_expr(CL_Compiler *c, CL_Obj expr);
void compile_body(CL_Compiler *c, CL_Obj forms);
void compile_progn(CL_Compiler *c, CL_Obj forms);
/* Trampoline-aware variants: compile non-tail forms (with OP_POP between)
 * and return the body's last form so the caller can splice it into an
 * outer trampoline.  Returns CL_NIL for an empty body. */
CL_Obj compile_progn_tail(CL_Compiler *c, CL_Obj forms);
CL_Obj compile_body_tail(CL_Compiler *c, CL_Obj forms);
void compile_lambda(CL_Compiler *c, CL_Obj form);
int alloc_temp_slot(CL_CompEnv *env);
/* Push a fresh tail frame onto c->tail_stack (grows it lazily); caller
 * fills in the kind and per-kind state.  Aborts on alloc failure. */
CL_TailFrame *cl_tail_push(CL_Compiler *c);

/* compiler_special.c
 *
 * compile_block / compile_return_from / compile_return are tail-trampoline
 * preludes: they emit the form's setup bytecode, push a CL_TailFrame for
 * the postlude, and return the body / value form for compile_expr's
 * trampoline to continue with (or CL_NIL when the body is empty). */
CL_Obj compile_block(CL_Compiler *c, CL_Obj form);
CL_Obj compile_return_from(CL_Compiler *c, CL_Obj form);
CL_Obj compile_return(CL_Compiler *c, CL_Obj form);
/* Postlude emitter, dispatched from compile_expr's drain loop. */
void emit_block_or_return_postlude(CL_Compiler *c, CL_TailFrame *tf);
void compile_tagbody(CL_Compiler *c, CL_Obj form);
void compile_go(CL_Compiler *c, CL_Obj form);
void compile_catch(CL_Compiler *c, CL_Obj form);
void compile_unwind_protect(CL_Compiler *c, CL_Obj form);
void compile_handler_case(CL_Compiler *c, CL_Obj form);
void compile_dolist(CL_Compiler *c, CL_Obj form);
void compile_dotimes(CL_Compiler *c, CL_Obj form);
void compile_do(CL_Compiler *c, CL_Obj form);
void compile_do_star(CL_Compiler *c, CL_Obj form);
/* Trampoline-aware: prelude binds locals / pushes handlers, postlude
 * restores env / pops handlers.  Returns the body's tail form (or CL_NIL
 * for an empty body — caller emits OP_NIL). */
CL_Obj compile_flet(CL_Compiler *c, CL_Obj form);
CL_Obj compile_labels(CL_Compiler *c, CL_Obj form);
void compile_destructuring_bind(CL_Compiler *c, CL_Obj form);
CL_Obj compile_handler_bind(CL_Compiler *c, CL_Obj form);
void compile_restart_case(CL_Compiler *c, CL_Obj form);
/* Trampoline-aware: return tail body form, push postlude frame.  */
CL_Obj compile_macrolet(CL_Compiler *c, CL_Obj form);
/* Install macrolet local-macro expanders into ENV (factored from
 * compile_macrolet); used by compile-file top-level macrolet processing. */
void cl_macrolet_install_expanders(CL_CompEnv *env, CL_Obj bindings);
CL_Obj compile_symbol_macrolet(CL_Compiler *c, CL_Obj form);
CL_Obj compile_progv(CL_Compiler *c, CL_Obj form);

/* compiler_extra.c */
void compile_and(CL_Compiler *c, CL_Obj form);
void compile_or(CL_Compiler *c, CL_Obj form);
void compile_cond(CL_Compiler *c, CL_Obj form);
void compile_quasiquote(CL_Compiler *c, CL_Obj form);
void compile_case(CL_Compiler *c, CL_Obj form, int error_if_no_match);
void compile_typecase(CL_Compiler *c, CL_Obj form, int error_if_no_match);
/* Trampoline-aware. */
CL_Obj compile_multiple_value_bind(CL_Compiler *c, CL_Obj form);
/* Trampoline-aware. */
CL_Obj compile_eval_when(CL_Compiler *c, CL_Obj form);
void compile_multiple_value_call(CL_Compiler *c, CL_Obj form);
void compile_multiple_value_list(CL_Compiler *c, CL_Obj form);
void compile_nth_value(CL_Compiler *c, CL_Obj form);
void compile_multiple_value_prog1(CL_Compiler *c, CL_Obj form);
void compile_load_time_value(CL_Compiler *c, CL_Obj form);
void compile_defsetf(CL_Compiler *c, CL_Obj form);
void compile_deftype(CL_Compiler *c, CL_Obj form);
void compile_defvar(CL_Compiler *c, CL_Obj form);
void compile_defparameter(CL_Compiler *c, CL_Obj form);
void compile_defconstant(CL_Compiler *c, CL_Obj form);
void compile_named_lambda(CL_Compiler *c, CL_Obj form);
void compile_defun(CL_Compiler *c, CL_Obj form);
void compile_defmacro(CL_Compiler *c, CL_Obj form);
CL_Obj defmacro_gensym(void);
int defmacro_is_ll_keyword(CL_Obj param);
int defmacro_needs_destructuring(CL_Obj ll);
void compile_declaim(CL_Compiler *c, CL_Obj form);
/* Trampoline-aware. */
CL_Obj compile_locally(CL_Compiler *c, CL_Obj form);
void compile_trace(CL_Compiler *c, CL_Obj form);
void compile_untrace(CL_Compiler *c, CL_Obj form);
void compile_time(CL_Compiler *c, CL_Obj form);
void compile_in_package(CL_Compiler *c, CL_Obj form);
void compile_the(CL_Compiler *c, CL_Obj form);

/* Declaration processing helpers */
CL_Obj process_body_declarations(CL_Compiler *c, CL_Obj body);
CL_Obj scan_local_specials(CL_Obj body);
int is_locally_special(CL_Obj var, CL_Obj local_specials);

/* Lexical notinline tracking (CLHS 3.2.2.1.3) */
void cl_compiler_push_notinline(CL_Compiler *c, CL_Obj func);
int cl_compiler_notinline_p(CL_Compiler *c, CL_Obj func);

/* Boxing analysis for mutable closure bindings */
void determine_boxed_vars(CL_Obj body, CL_Obj *vars, int n_vars,
                          uint8_t *boxed_out);
void scan_body_for_boxing(CL_Obj form, CL_Obj *vars, int n_vars,
                          uint8_t *mutated, uint8_t *captured,
                          int closure_depth);

#endif /* COMPILER_INTERNAL_H */
