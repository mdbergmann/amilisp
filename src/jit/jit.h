/* jit.h — public API of the native-code backends.
 *
 * This is the integration surface used by the rest of the runtime. Two
 * functions are called from the bytecode compiler / boot, and the rest
 * is implementation detail.
 *
 * Two backends implement it: the m68k template JIT (JIT_M68K, the Amiga
 * cross build; specs/native-backend.md) and the AArch64 one (JIT_A64,
 * arm64 hosts; specs/native-backend-a64.md).  Either defines
 * CL_JIT_NATIVE.  The CPU-independent half -- the hot-call policy, the
 * on/off switches, the counters -- lives in jit_common.c; each backend
 * supplies compile, invoke, the stub and the disassembler (jit_backend.h).
 *
 * Without a backend every entry point becomes an inline no-op so call
 * sites stay identical and Lisp code is unaware of the JIT's presence.
 */

#ifndef CL_JIT_H
#define CL_JIT_H

#include "core/types.h"

/* Hot-call threshold bounds (jit.h; specs/lazy-jit.md).  Kept outside the
 * CL_JIT_NATIVE guard below: %JIT-SET-HOT-THRESHOLD validates against
 * CL_JIT_HOT_MAX on every build, including host, where cl_jit_set_hot_threshold
 * is the no-op stub. */
#define CL_JIT_HOT_DEFAULT  8
#define CL_JIT_HOT_MAX      126    /* the count lives in 7 bits */

/* Native-to-native direct calls (specs/jit-direct-calls.md; AArch64:
 * specs/native-backend-a64.md, "Direct calls"): the counters of the
 * call-site miss path, in this order, for %JIT-DIRECT-CALL-STATS --
 * process-wide on m68k, the calling thread's on AArch64. */
enum {
    CL_JIT_DS_FILLS, CL_JIT_DS_MISSES, CL_JIT_DS_REFUSED_TRACE,
    CL_JIT_DS_REFUSED_SHADOW, CL_JIT_DS_REFUSED_ABI,
    CL_JIT_DS_REFUSED_NOT_NATIVE, CL_JIT_DS_COUNT
};

#if defined(JIT_M68K) || defined(JIT_A64)
#define CL_JIT_NATIVE 1
#endif

#ifdef CL_JIT_NATIVE

#ifdef JIT_M68K
/* The highest positional arity native code is entered with (cl_jit_invoke,
 * the pass-through matcher, the walker's gate, the call-site fill rule):
 * OP_CALL's count is a byte.  cl_jit_enter and a call site's hit path take
 * any count; the frame displacements stay d16 at this bound. */
#define CL_JIT_MAX_POSITIONAL 255
#endif

/* One-time init at boot, after cl_compiler_init. */
void   cl_jit_init(void);
/* Process exit, after cl_mem_shutdown has released every function's native
 * code: hand back what the backend maps itself (the AArch64 code heap). */
void   cl_jit_shutdown(void);

/* The direct-call kill switch (%JIT-SET-DIRECT-CALLS, AMILISP_JIT_DIRECT=0):
 * while off, no call site fills, so every call takes the helper path.
 * Toggling bumps cl_call_gen. */
void   cl_jit_set_direct_calls(int on);
int    cl_jit_direct_calls_enabled(void);
/* Copy the CL_JIT_DS_* counters into OUT[CL_JIT_DS_COUNT] (AArch64: the
 * calling thread's). */
void   cl_jit_direct_call_stats(uint32_t *out);

/* Optionally translate this bytecode to native code. May leave
 * bc->native_code == NULL if the function is ineligible or the JIT is
 * disabled — callers must always be ready to fall back to the bytecode
 * interpreter. */
void   cl_jit_compile(CL_Bytecode *bc);

/* When to compile (specs/lazy-jit.md).  A new
 * function is compiled at definition only in eager mode (hot threshold 0)
 * or when it was compiled under (optimize (speed 3)); otherwise the
 * interpreter counts its calls (bc->jit_hot) and compiles it on the
 * threshold-th one -- on the first when it contains a backward branch,
 * i.e. a loop.  Code that runs once is never compiled, which is what
 * makes a heap image (whose native code is dropped on restore) fast
 * again: its functions compile as they turn hot.
 *
 * cl_jit_note_definition replaces cl_jit_compile at the three places a
 * CL_Bytecode is born (compile_lambda, cl_compile, the FASL reader).
 * cl_jit_note_call is the call paths' slow arm, taken only while
 * CL_BC_JIT_COUNTING_P and native_code is NULL; it never allocates on the
 * heap (the JIT does not), so a caller's raw CL_Bytecode * stays valid. */
void   cl_jit_note_definition(CL_Bytecode *bc);
void   cl_jit_note_call(CL_Bytecode *bc);
/* Compile now a function that is still counting (%JIT-DISASSEMBLE, so that
 * JITEXPAND shows a fresh definition's code).  A settled one is left as it
 * is: the JIT already declined it, or it was defined with the JIT off. */
void   cl_jit_compile_if_counting(CL_Bytecode *bc);
void   cl_jit_set_hot_threshold(int calls);   /* 0 = eager; clamped */
int    cl_jit_hot_threshold(void);
/* Functions the hot path compiled / found ineligible since boot. */
uint32_t cl_jit_hot_compile_count(void);
/* Bytes of native code installed since boot (cumulative). */
uint32_t cl_jit_native_bytes(void);

/* Enter native code with the same calling convention the bytecode VM
 * uses (args already pushed on cl_vm.stack).  `func_obj` is the
 * function value the caller dispatched against — a CL_Closure when
 * reached via a defun/lambda binding, or a raw CL_Bytecode CL_Obj
 * otherwise; the JIT'd body reads it at 8(a6) for OP_UPVAL /
 * OP_CELL_SET_UPVAL.  Caller checks bc->native_code != NULL first.
 * Not wired into vm.c in the skeleton. */
CL_Obj cl_jit_invoke(CL_Obj func_obj, CL_Bytecode *bc, int nargs);

#ifdef JIT_M68K
/* src/jit/jit_enter_m68k.s: call native `entry` with A3 = `thread`,
 * argv[0..nargs-1] pushed in that order (argv[0] highest, i.e. the
 * operand-stack order of a native call site), then `func`; a non-NULL
 * `sp_slot` receives the stack pointer the callee is entered with (a
 * native CL_Frame's marker, jit_m68k.c).  Only cl_jit_invoke calls it.
 * See specs/jit-direct-calls.md §4. */
struct CL_Thread_s;
CL_Obj cl_jit_enter(void *entry, struct CL_Thread_s *thread, CL_Obj func,
                    const CL_Obj *argv, int32_t nargs, void *sp_slot);
/* Give the current thread's native frames the ip of the call each one is
 * in (native code stores none), for the backtrace's lines.  vm.c calls it
 * before it reads frame ips. */
void   cl_jit_resolve_frame_ips(void);
#endif

/* Release the native code of a dead or recompiled function (mem.c's
 * sweep).  m68k code is platform_alloc'd; AArch64 code lives in the
 * executable code heap (codeheap.c). */
void   cl_jit_free_native(void *code);

/* Runtime introspection: is the JIT compiled in and active? */
int    cl_jit_enabled(void);

/* Toggle whether cl_jit_compile attempts native codegen.  When inactive,
 * cl_jit_compile is a no-op so newly created CL_Bytecodes stay
 * native_code=NULL and run through the interpreter.  Already-compiled
 * functions keep their native code; flip before defining the function
 * to get a bytecode-only version.  Used by `--no-jit` and the
 * `%JIT-SET-ACTIVE` builtin to A/B benchmark JIT vs. bytecode. */
void   cl_jit_set_active(int active);
/* --no-jit: off for the whole session (a later %JIT-SET-ACTIVE T lifts it).
 * Unlike a %JIT-SET-ACTIVE NIL window, calls settle their callees then. */
void   cl_jit_disable_for_session(void);

/* Toggle the per-call shadow CL_Frame that makes JIT'd functions visible to
 * EXT:BACKTRACE / EXT:FRAME-LOCALS and the error-time backtrace.  Off by
 * default: the push costs a few percent on call-heavy code and is only
 * useful while introspecting (an error, the SLDB debugger, an explicit
 * ext:backtrace).  Turn on for a debug session (Sly/SLDB) or a test that
 * needs JIT frame introspection.  Exposed via `%JIT-SET-FRAMES`. */
void   cl_jit_set_shadow_frames(int on);
int    cl_jit_shadow_frames_enabled(void);

/* Diagnostic helper: emit a fixed NOP+RTS stub into bc->native_code.
 * Used by the `%JIT-COMPILE-STUB` builtin to exercise the encoder →
 * CodeBuf → CL_Bytecode pipeline before there's any actual codegen
 * (or any caller that enters native_code).  Returns 1 on success,
 * 0 if allocation failed.  Existing native_code is replaced. */
int    cl_jit_emit_stub(CL_Bytecode *bc);

/* Diagnostic counter: number of cl_jit_invoke entries since boot.
 * Bumped on every native-code dispatch from OP_CALL.  Exposed to Lisp
 * via the `%JIT-INVOKE-COUNT` builtin so end-to-end tests can prove
 * that a call actually went through the native path rather than just
 * being interpreted (which would happen to return the same value). */
uint32_t cl_jit_invoke_count_get(void);

/* Pretty-print the native bytes in `code` (length `len`) as one line of
 * assembly per instruction to platform_write_string.  Only decodes the
 * forms the JIT can emit; anything else falls through to ".word $xxxx".
 * Exposed to Lisp as `amilisp::%JIT-DISASSEMBLE`. */
void cl_jit_disassemble(const uint8_t *code, uint32_t len);

#else  /* no CL_JIT_NATIVE — targets without a backend get no-op stubs */

static inline void   cl_jit_init(void)                       { }
static inline void   cl_jit_shutdown(void)                   { }
static inline void   cl_jit_set_direct_calls(int on)          { (void)on; }
static inline int    cl_jit_direct_calls_enabled(void)       { return 0; }
static inline void   cl_jit_direct_call_stats(uint32_t *out)
{ int i; for (i = 0; i < CL_JIT_DS_COUNT; i++) out[i] = 0; }
static inline void   cl_jit_compile(CL_Bytecode *bc)         { (void)bc; }
static inline void   cl_jit_note_definition(CL_Bytecode *bc) { (void)bc; }
static inline void   cl_jit_note_call(CL_Bytecode *bc)       { (void)bc; }
static inline void   cl_jit_compile_if_counting(CL_Bytecode *bc) { (void)bc; }
static inline void   cl_jit_set_hot_threshold(int calls)      { (void)calls; }
static inline int    cl_jit_hot_threshold(void)              { return 0; }
static inline uint32_t cl_jit_hot_compile_count(void)        { return 0; }
static inline uint32_t cl_jit_native_bytes(void)             { return 0; }
static inline CL_Obj cl_jit_invoke(CL_Obj f, CL_Bytecode *bc, int n) { (void)f; (void)bc; (void)n; return CL_NIL; }
static inline int    cl_jit_enabled(void)                    { return 0; }
static inline void   cl_jit_set_active(int a)                 { (void)a; }
static inline void   cl_jit_disable_for_session(void)        { }
static inline void   cl_jit_set_shadow_frames(int on)        { (void)on; }
static inline int    cl_jit_shadow_frames_enabled(void)      { return 0; }
static inline int    cl_jit_emit_stub(CL_Bytecode *bc)       { (void)bc; return 0; }
static inline uint32_t cl_jit_invoke_count_get(void)         { return 0; }
static inline void   cl_jit_disassemble(const uint8_t *c, uint32_t n) { (void)c; (void)n; }
/* Without a backend no bytecode ever carries native code. */
static inline void   cl_jit_free_native(void *code)          { (void)code; }

#endif /* CL_JIT_NATIVE */

#endif /* CL_JIT_H */
