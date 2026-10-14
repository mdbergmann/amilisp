#ifndef CL_VM_H
#define CL_VM_H

#include "types.h"
#include "printer.h"
#include <setjmp.h>
#include <stdio.h>

/*
 * Stack-based bytecode interpreter.
 */

#define CL_VM_STACK_SIZE  (16 * 1024)  /* 16K entries = 64KB */
#define CL_VM_FRAME_SIZE  1024         /* Max call frames */

/* CALL-ARGUMENTS-LIMIT: the most arguments OP_APPLY will spread onto the VM
 * stack for a single call.  Independent of LAMBDA-PARAMETERS-LIMIT (the cap on
 * *declared* parameters) because a &rest function accepts arbitrarily many
 * actual arguments.  Bounded well below CL_VM_STACK_SIZE so a large APPLY plus
 * the callee's frame/locals cannot overflow the stack. */
#define CL_CALL_ARGS_LIMIT  4096

/* Call frame */
typedef struct {
    CL_Obj bytecode;     /* Current CL_Bytecode or CL_Closure */
    uint8_t *code;       /* Code pointer */
    CL_Obj *constants;   /* Constants pool */
    uint32_t ip;         /* Instruction pointer */
    uint32_t bp;         /* Base pointer (locals start) */
    int n_locals;
    uint16_t nargs;      /* Actual argument count (for OP_ARGC); widened to
                            uint16_t so APPLY can spread > 255 args (up to
                            CL_CALL_ARGS_LIMIT) to a &rest function. */
    uint8_t stub_code[3]; /* For cl_vm_apply stub frames: OP_CALL,nargs,OP_HALT.
                             Embedded here so it survives longjmp past the
                             cl_vm_apply C frame (stack-use-after-return fix). */
    uint8_t fslot;       /* 1 if the caller's function object sits in the
                            stack slot just below bp (OP_CALL / OP_TAILCALL
                            push [func][args...] and the frame's bp is the
                            first argument), 0 if nothing is under the
                            arguments (OP_CALL_GLOBAL, OP_APPLY, the entry
                            frames of cl_vm_eval / cl_vm_apply).  OP_RET
                            restores sp = bp - fslot, which is what lets a
                            call leave its arguments where they were pushed
                            instead of shifting them down over the func. */
    int nlx_level;       /* NLX stack depth when this frame was entered.
                            Used by OP_TAILCALL to pop BLOCK/CATCH frames
                            from the current function before reusing the frame. */
} CL_Frame;

typedef struct {
    CL_Obj *stack;
    uint32_t stack_size; /* Number of entries */
    int sp;              /* Stack pointer (next free slot) */
    CL_Frame *frames;
    int frame_size;      /* Max call frames */
    int fp;              /* Frame pointer (current frame index) */
} CL_VM;

/* --- Dynamic (special) variable binding stack --- */

typedef struct {
    CL_Obj symbol;
    CL_Obj old_value;
} CL_DynBinding;

#define CL_MAX_DYN_BINDINGS 4096

/* Restore dynamic bindings down to mark */
void cl_dynbind_restore_to(int mark);

/* --- Multiple Values --- */
#define CL_MAX_MV 20

/* --- Non-Local eXit (NLX) stack for catch/throw and unwind-protect --- */

#define CL_NLX_CATCH    0
#define CL_NLX_UWPROT   1
#define CL_NLX_BLOCK    2
#define CL_NLX_TAGBODY  3
#define CL_NLX_HANDLER_CASE 4  /* OP_HANDLER_CASE_PUSH: the clause landing of a
                                * HANDLER-CASE.  Its handler bindings hold the
                                * clause index as a fixnum and point back at
                                * this frame (CL_HandlerBinding.nlx_index);
                                * cl_signal_condition transfers here instead
                                * of calling a handler function. */
#define CL_NLX_TYPE_COUNT 5
/* Main-thread NLX stack depth (nested CATCH / UNWIND-PROTECT / BLOCK /
 * TAGBODY / HANDLER-CASE frames).  The stack is platform_alloc'd up front,
 * outside the GC arena, for every amilisp process.
 *
 * 68k AmigaOS gets a smaller budget, 2x what its workers have always run
 * with (CL_WORKER_NLX_FRAMES): a frame is 330 bytes on m68k, so the
 * 2048-frame stack cost ~660 KB of Fast RAM
 * per process on an 8 MB machine, and Clamacs runs two.  Overflow past 512 is a
 * clean, HANDLER-CASE-catchable CL_ERR_OVERFLOW ("NLX stack overflow", the
 * OP_CATCH / OP_UWPROT / ... push guards in vm.c), never a crash; the Amiga
 * suite has a test for it ("nlx budget" in tests/amiga/run-tests.lisp).
 * MorphOS and the host have RAM to spare and keep 2048. */
#if defined(PLATFORM_AMIGA) && !defined(PLATFORM_MORPHOS)
#define CL_MAX_NLX_FRAMES 512
#else
#define CL_MAX_NLX_FRAMES 2048
#endif

typedef struct {
    uint8_t type;          /* CL_NLX_CATCH or CL_NLX_UWPROT */
    /* Where a non-local transfer to this frame lands.  A frame pushed by
     * the bytecode VM (OP_CATCH / OP_UWPROT / OP_BLOCK_PUSH / OP_TAGBODY_PUSH)
     * does NOT setjmp: `landing` points at the ONE jmp_buf its cl_vm_run
     * activation armed on its first NLX push, and the activation's shared
     * landing code restores everything from the frame at cl_nlx_top.  A
     * frame pushed by C code (bi_warn, the thread-entry abort frame) or by
     * JIT'd native code (which setjmps inline) has landing == NULL and owns
     * `buf`.  cl_nlx_jump() picks the right one; nothing else may longjmp
     * to an NLX frame directly. */
    jmp_buf *landing;
    jmp_buf buf;
    CL_JMPBUF_GUARD        /* MorphOS PPC setjmp overrun guard — see types.h */
    int vm_sp;
    int vm_fp;
    CL_Obj tag;            /* catch tag (CATCH only) */
    CL_Obj result;         /* value to propagate */
    uint32_t catch_ip;     /* IP after the CATCH/UWPROT instruction */
    int32_t offset;        /* jump offset to landing */
    uint8_t *code;         /* code pointer to restore */
    CL_Obj *constants;     /* constants pointer to restore */
    CL_Obj bytecode;       /* frame->bytecode at NLX push time */
    int base_fp;           /* base_fp of the cl_vm_eval call */
    int dyn_mark;          /* binding stack depth at frame creation */
    int handler_mark;      /* handler stack depth at frame creation */
    int restart_mark;      /* restart stack depth at frame creation */
    int error_mark;        /* C-level error-frame (CL_CATCH) stack depth at
                            * frame creation.  Restored when this NLX frame is
                            * unwound to, so error frames pushed in C functions
                            * deeper than this NLX — abandoned by the longjmp —
                            * are dropped.  Without this, a later cl_error /
                            * rethrow longjmps to a stale error frame whose
                            * setjmp SP points into an already-unwound (now
                            * reused) stack region, corrupting a live caller's
                            * frame (observed: COMPILE-FILE canary smash when an
                            * error unwinds through an unwind-protect during a
                            * compile-time eval). */
    int gc_root_mark;      /* GC root stack depth at frame creation */
    void *compiler_mark;   /* active compiler chain head at frame creation */
    uint64_t handler_active_mask; /* cl_handler_active_mask snapshot at frame
                            * creation.  Restored when this NLX frame is unwound
                            * to, so a CLHS 9.1.4 disabled-handler band set by a
                            * running handler is re-enabled when that handler
                            * exits via a non-local transfer through here (e.g.
                            * INVOKE-RESTART out of a HANDLER-BIND handler). */
    int mv_count;          /* multiple value count to preserve across NLX */
    CL_Obj mv_values[CL_MAX_MV]; /* multiple values to preserve across NLX */
    int saved_pending_mark; /* saved_pending_top depth at frame creation */
    int mv_save_mark;      /* CL_Thread.mv_save_top at frame creation: the
                            * UNWIND-PROTECT value records pushed above it
                            * (OP_MV_SAVE) belong to cleanups this transfer
                            * abandons, so the landing drops them. */
    int32_t hc_clause;     /* CL_NLX_HANDLER_CASE: index of the clause whose
                            * type matched, set by the transfer; the landing
                            * resumes at catch_ip + offset + 5 * hc_clause
                            * (the per-clause OP_JMP table). */
    int saved_jit_depth;   /* CT->jit_depth at frame creation.  Restored on
                            * every longjmp landing (like the CL_ErrorFrame
                            * twin): a THROW/RETURN-FROM/GO that unwinds past
                            * cl_jit_invoke skips its C epilogue, and a stale
                            * jit_depth makes the next JIT entry keep a stale
                            * jit_stack_top — the conservative GC scan window
                            * then excludes live JIT frames (missed roots). */
    char *saved_jit_c_floor; /* CT->jit_c_floor at frame creation, restored
                            * with saved_jit_depth (a throw out of a callback
                            * on a foreign stack leaves the floor parked). */
    CL_PrinterState printer_mark; /* printer flags at frame creation.  Restored
                            * on the longjmp landing so a THROW out of a print
                            * hook / pprint-dispatch fn can't leak pr_depth,
                            * pr_inprog_top, pr_pprint_dispatch_active or
                            * pr_circle_active (see printer.h; mirrors the
                            * CL_ErrorFrame.saved_printer twin). */
} CL_NLXFrame;

/* Transfer control to NLX frame F (never returns): longjmp to the owning
 * cl_vm_run activation's landing, or to the frame's own buf for a C/JIT
 * frame.  The caller has already set cl_nlx_top to F's index and stored the
 * result / pending state the landing consumes. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#endif
void cl_nlx_jump(CL_NLXFrame *f);

/* HANDLER-CASE clause transfer (never returns): handler binding BINDING_IDX
 * matched CONDITION.  Runs the interposing UNWIND-PROTECT cleanups (pending
 * kind 3, target frame index in cl_pending_tag) and lands in the owning
 * CL_NLX_HANDLER_CASE frame with the condition as its result. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#endif
void cl_handler_case_transfer(int binding_idx, CL_Obj condition);

/* --- UNWIND-PROTECT value save stack (OP_MV_SAVE / OP_MV_RESTORE) ---
 *
 * The values of a protected form must survive the cleanup forms, which
 * clobber the MV buffer.  Instead of consing them into a list and calling
 * VALUES-LIST afterwards, OP_MV_SAVE pushes one record — the values, then
 * their count as a FIXNUM on top — onto a small per-thread stack, and
 * OP_MV_RESTORE pops it back into the MV buffer.  Every word in the buffer
 * is a valid CL_Obj (the count is fixnum-tagged), so the GC marks and
 * forwards the whole used prefix.  A record costs count + 1 words; the
 * fixed size bounds the nesting of ACTIVE cleanups (a cleanup that recurses
 * into another unwind-protect) — 2048 words is one single-value record per
 * VM frame (CL_VM_FRAME_SIZE), so a single-valued cleanup recursion hits
 * "Call stack overflow" first, and a multi-valued one gets a clean
 * "value-save stack overflow" error. */
#define CL_MV_SAVE_SIZE 2048

/* --- Condition handler binding stack --- */

typedef struct {
    CL_Obj type_name;     /* Condition type symbol to match */
    CL_Obj handler;       /* Handler function (closure or function) — or, for a
                             HANDLER-CASE clause, the clause index as a FIXNUM:
                             no function is called, control transfers to the
                             NLX frame at nlx_index (cl_handler_case_transfer). */
    int nlx_index;        /* CL_NLX_HANDLER_CASE frame owning this binding, or
                             -1 for an ordinary HANDLER-BIND binding */
    int handler_mark;     /* Handler stack depth when this binding was established */
    /* The per-binding enabled/disabled state (CLHS 9.1.4) now lives in the
     * per-thread cl_handler_active_mask (bit j = binding j enabled), so it can
     * be snapshot/restored across non-local exits.  See thread.h. */
} CL_HandlerBinding;

#define CL_MAX_HANDLER_BINDINGS 64

/* Bit mask for handler indices [lo, hi) (used to disable a band in
 * cl_handler_active_mask).  hi may equal CL_MAX_HANDLER_BINDINGS (64); shifting
 * by 64 is undefined, so clamp that to the all-ones upper bound. */
#define CL_HANDLER_BAND_MASK(lo, hi) \
    ((((hi) >= 64) ? ~(uint64_t)0 : (((uint64_t)1 << (hi)) - 1)) \
     & ~(((uint64_t)1 << (lo)) - 1))
/* --- Restart binding stack --- */

typedef struct {
    CL_Obj name;        /* Restart name symbol (ABORT, CONTINUE, etc.) or NIL */
    CL_Obj handler;     /* Closure to invoke */
    CL_Obj tag;         /* Internal catch tag for transfer of control */
    CL_Obj restart;     /* First-class restart object (TYPE_RESTART) */
} CL_RestartBinding;

#define CL_MAX_RESTART_BINDINGS 64
/* Signal a condition — walks handler stack, returns NIL if no handler transferred */
CL_Obj cl_signal_condition(CL_Obj condition);

/* COMPILE warning/failure detection — see builtins_condition.c.  While
 * cl_compile_detect_depth > 0, signaled warnings/errors set these flags so
 * COMPILE can return warnings-p / failure-p. */
extern int cl_compile_detect_depth;
extern int cl_compile_warnings_p;
extern int cl_compile_failure_p;

/* Throw a value to a catch tag (for restart invocation from debugger) */
void cl_throw_to_tag(CL_Obj tag, CL_Obj value);

/* Create a condition from an error code and message */
CL_Obj cl_create_condition_from_error(int code, const char *msg);

/* --- Backtrace ---
 *
 * The formatted backtrace text lives in a per-thread buffer that starts as a
 * fixed inline block inside CL_Thread — so the common, shallow case costs no
 * allocation on the error path — and grows onto the C heap when a deep or
 * long-pathed backtrace needs more room.  Before it could grow, the 2048-byte
 * ceiling cut the text off mid-frame, which put a second, invisible limit on
 * top of the frame limit: `:bt 100` would have had nowhere to put the frames.
 */
#define CL_BACKTRACE_BUF_SIZE 2048          /* inline block; no malloc needed */
#define CL_BACKTRACE_BUF_MAX  (128 * 1024)  /* hard ceiling on heap growth */

/* Frames cl_capture_backtrace renders by default.  The debugger's `:bt N`
 * re-renders the same error-time window with a different limit. */
#define CL_BACKTRACE_DEFAULT_FRAMES 20

/* Ceiling on backtrace-buffer growth, in bytes (never below
 * CL_BACKTRACE_BUF_SIZE).  Defaults to CL_BACKTRACE_BUF_MAX so a runaway
 * backtrace cannot eat the 8MB an Amiga has; tests lower it to drive the
 * truncation path deterministically. */
extern int cl_backtrace_cap_limit;

/* Capture current VM call stack into cl_backtrace_buf */
void cl_capture_backtrace(void);

/* Re-render the error-time backtrace window that cl_capture_backtrace
 * snapshotted, with a different frame limit; MAX_FRAMES <= 0 means "every
 * frame".  Backs the debugger's `:bt [n|all]`.  Outside an error this renders
 * the live call stack, like (ext:backtrace). */
void cl_render_backtrace(int max_frames);

/* --- Structured backtrace introspection (Sly/SLYNK SLDB backend) ---
 *
 * These walk the error-time frame window (cl_debug_base_fp, snapshotted by
 * cl_capture_backtrace; falls back to the live frame pointer outside an
 * error).  Frame index 0 is the innermost frame. */

/* Backtrace as a list of (INDEX NAME FILE LINE) entries, innermost first.
 * NAME is the function-name symbol (NIL if anonymous); FILE a namestring
 * string (NIL if unknown); LINE a 1-based fixnum (NIL if unknown).
 * MAX_FRAMES <= 0 means "all". */
CL_Obj cl_vm_backtrace_list(int max_frames);

/* Locals of frame INDEX as a list of (NAME . VALUE), with placeholder names
 * (#:ARGn for argument slots, #:LOCALn for the rest).  Returns :NOT-AVAILABLE
 * for an out-of-range index. */
CL_Obj cl_vm_frame_locals(int index);

/* Initialize VM (0 = use default for either parameter) */
void cl_vm_init(uint32_t stack_size, int frame_size);

/* Shutdown VM (free dynamic allocations) */
void cl_vm_shutdown(void);

/* Ctrl-C check of the interpreter's backward jumps: when a break is
 * pending, report it and call BREAK.  For native loops (jit/runtime.c). */
void cl_vm_poll_break(void);

/* Execute a bytecode object, return result */
CL_Obj cl_vm_eval(CL_Obj bytecode);

/* Apply a function to arguments (builds temp bytecode, calls cl_vm_eval) */
CL_Obj cl_vm_apply(CL_Obj func, CL_Obj *args, int nargs);
/* (apply FUNC ARGLIST) for any count up to CALL-ARGUMENTS-LIMIT — runs the
 * inline OP_APPLY; cl_vm_apply's one-byte OP_CALL stub stops at 255. */
CL_Obj cl_vm_apply_list(CL_Obj func, CL_Obj arglist);
struct CL_Thread_s;
/* Call a C builtin whose arguments already sit on the VM stack (rooted),
 * exactly as cl_vm_run's OP_CALL does it: arity check, per-thread crash
 * diagnostics, MV bookkeeping, no frame.  The JIT's call paths
 * (jit/runtime_m68k.c, jit/runtime_vmstack.c) use it; every other caller goes through cl_vm_apply. */
CL_Obj cl_vm_call_builtin(struct CL_Thread_s *thr, CL_Function *func,
                          CL_Obj *args, int nargs);
/* Call a bytecode/closure callee through a stub OP_CALL frame (the tail of
 * cl_vm_apply).  ARGS in call order, or with REVERSED set args[i] = argument
 * nargs-1-i (the JIT's operand-stack layout).  At most 255 arguments. */
CL_Obj cl_vm_call_bytecode(struct CL_Thread_s *thr, CL_Obj func,
                           const CL_Obj *args, int nargs, int reversed);

/* Suffix for a THROW / RETURN-FROM / GO miss when the target exists but is
 * hidden below cl_nlx_floor (inside a foreign callback); "" otherwise.
 * TYPE = CL_NLX_* kind, -1 for any. */
const char *cl_nlx_boundary_hint(CL_Obj tag, int type);

/* Unwrap a funcallable instance (standard-generic-function struct) to its
 * discriminating function. Returns the input unchanged for other types. */
CL_Obj cl_unwrap_funcallable(CL_Obj obj);

/* True if OBJ is a funcallable instance (i.e. a GF struct). */
int cl_funcallable_instance_p(CL_Obj obj);

/* Register SYM (a struct-type name symbol) as a funcallable generic-function
 * type, so structs of that type are recognized by cl_funcallable_instance_p.
 * Used to support custom generic-function metaclasses (CLHS
 * :generic-function-class) — subclasses of STANDARD-GENERIC-FUNCTION whose
 * instances must still dispatch through their discriminating-function slot.
 * Idempotent; STANDARD-GENERIC-FUNCTION is always registered. */
void cl_register_funcallable_gf_type(CL_Obj sym);

/* Reader-GF inline-cache hit path (builtins_struct.c).  Returns the slot
 * value, or CL_UNBOUND for "miss — take the full dispatch".  Non-allocating.
 * Used by OP_CALL to answer a monomorphic accessor call without building a
 * frame; see %MAKE-READER-DISCRIMINATOR in lib/clos.lisp. */
CL_Obj cl_gf_reader_ic_probe(CL_Obj gfobj, CL_Obj obj);

/* Writer-GF inline-cache hit path (builtins_struct.c).  Stores VAL into the
 * cached slot and returns VAL, or CL_UNBOUND for "miss — take the full
 * dispatch".  Non-allocating.  Used by OP_CALL to answer a (setf accessor)
 * call without building a frame; see %MAKE-WRITER-DISCRIMINATOR in
 * lib/clos.lisp. */
CL_Obj cl_gf_writer_ic_probe(CL_Obj gfobj, CL_Obj val, CL_Obj obj);

/* Push/pop on VM value stack (for builtins) */
void cl_vm_push(CL_Obj val);
CL_Obj cl_vm_pop(void);

/* The comparison OP_CMP_BR fuses (CL_CMP_BR_* in opcodes.h), for the
 * operands the inline fixnum / character fast path does not cover: the
 * full REAL / NUMBER / CHARACTER type gate of the member opcode, the
 * cross-type numeric compare, the member's error text.  Returns 1 when
 * the comparison holds.  Shared with the JIT's slow path (runtime.c). */
int cl_vm_compare_kind(CL_Obj a, CL_Obj b, int cmp);

/* C stack overflow detection */
/* The C stack cl_check_c_stack lets a thread use, measured from its
 * c_stack_base (host; AmigaOS checks the task's real bounds first).  The
 * AArch64 JIT's direct calls compare against it inline. */
#if defined(CL_ASAN_BUILD) || defined(__SANITIZE_ADDRESS__)
#define CL_C_STACK_LIMIT (64 * 1024 * 1024)  /* ASAN frames are huge — bump cap */
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)       /* clang's spelling */
#define CL_C_STACK_LIMIT (64 * 1024 * 1024)
#endif
#endif
#ifndef CL_C_STACK_LIMIT
#define CL_C_STACK_LIMIT (3 * 1024 * 1024)   /* 3MB of 8MB, leave 5MB margin */
#endif
void cl_check_c_stack(const char *context);
/* cl_check_c_stack + GC-root-stack headroom; for per-nesting-level
 * recursion entry points (reader, compiler) only — see vm.c. */
void cl_check_recursion_guards(const char *context);

/* Heuristic sanity check on a builtin's native function pointer (used before
 * dispatch to catch a clobbered func slot).  Returns 1 if plausible, 0 if it
 * should be rejected as corrupt.  Exposed for unit testing — see vm.c for the
 * platform rationale (notably: host function pointers are NOT 4-byte aligned). */
int cl_vm_builtin_fptr_plausible(const void *fptr);

/* Opcode-frequency profiler. Counters only increment when built with
 * -DPROFILE_OPCODES; dump still works (prints "not compiled in" message). */
void cl_op_counts_reset(void);
void cl_op_counts_dump(FILE *out);

#include "thread.h"

#endif /* CL_VM_H */
