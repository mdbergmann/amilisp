#ifndef CL_BUILTINS_H
#define CL_BUILTINS_H

#include "types.h"

/*
 * Built-in CL functions implemented in C.
 * Registered into the CL package during initialization.
 */

void cl_builtins_init(void);

/* Set *DEFAULT-PATHNAME-DEFAULTS* from the process's cwd (builtins_pathname.c).
 * Run at init and after a heap-image restore, where the restored value would
 * still name the saving process's directory. */
void cl_pathname_default_from_cwd(void);

/* Release builtin-module side tables (struct/CLOS slot index, condition
 * hierarchy index) at process exit.  See builtins.c. */
void cl_builtins_shutdown(void);

/* A CLHS "non-negative integer" list index/count argument (NTH, NTHCDR,
 * LAST, BUTLAST, %SETF-NTH), as an int32_t a walk loop can count down:
 * a non-negative fixnum as itself, a positive bignum clamped to INT32_MAX
 * (longer than any list the arena can hold, so the walk falls off the end),
 * and anything else — a negative index, a non-integer — a TYPE-ERROR
 * naming FN_NAME.  Defined in builtins_lists.c. */
int32_t cl_extract_count_or_clamp(CL_Obj n_arg, const char *fn_name);

/* Anchor the GET-INTERNAL-REAL-TIME epoch at the current instant.  main()
 * calls this right after platform_init() so internal real time counts from
 * process start — (get-internal-real-time) then measures launch-to-here
 * directly (boot/load profiling).  Without the call the epoch is set lazily
 * on first use, which still satisfies CLHS (the epoch is arbitrary) but
 * makes the boot leg invisible from Lisp. */
void cl_internal_time_init(void);

/* Funcall every function designator on EXT:*EXIT-HOOKS* (most recently added
 * first), then clear the list.  main() calls this at the head of its shutdown
 * funnel, while the VM, streams and heap are all still live, so hooks can
 * print, close files and stop threads.  Each hook runs in its own CL_CATCH: an
 * error is reported and the remaining hooks still run; a hook calling (QUIT)
 * ends the sequence.  Idempotent — the list is taken before the first call. */
void cl_run_exit_hooks(void);

/* Funcall the hooks in MP:*THREAD-DEATH-HOOKS* with (THREAD MESSAGE) on the
 * current thread, which an unhandled error just ended: MSG is that error's
 * text.  Called by the thread entry while the thread is still registered.
 * Does nothing (and allocates nothing) while the list is empty.  Each hook
 * runs in its own CL_CATCH; a hook that fails -- the heap may be as full as
 * when the thread died -- is dropped, reported under CLAMIGA_THREAD_ERRORS. */
void cl_run_thread_death_hooks(const char *msg);

/* Register a builtin function in a specific package.
 * NAME must have static lifetime (every caller passes a string literal);
 * the image-relink registry below keeps the pointer for the life of the
 * process. */
void cl_register_builtin(const char *name, CL_CFunc func,
                          int min, int max, CL_Obj package);

/* --- Image-relink builtin registry (see specs/image-save-load.md) ---
 * cl_register_builtin is the single choke point through which every
 * builtin registration flows; it records (package-name, symbol-name) ->
 * CL_CFunc in an off-arena side table.  A restored image's TYPE_FUNCTION
 * objects carry the SAVING process's C function pointers (meaningless
 * under ASLR / AmigaOS LoadSeg relocation), so the restore walk resolves
 * each function's name through this registry to the current process's
 * pointer.  Unknown names get bi_stale_builtin, which signals a
 * descriptive error instead of jumping to a garbage address. */
CL_CFunc cl_builtin_registry_lookup(const char *pkg_name, uint32_t pkg_len,
                                    const char *sym_name, uint32_t sym_len);
CL_Obj bi_stale_builtin(CL_Obj *args, int nargs);

/* Register a builtin in PACKAGE and export it from that package.  The
 * shared body behind the per-package `*_defun` helpers; also guarantees
 * the registration reaches the image-relink registry above. */
void cl_register_builtin_exported(const char *name, CL_CFunc func,
                                  int min, int max, CL_Obj package);

/* AmigaOS library-call result kinds (builtins_amiga.c).  Encoded in bits
 * 28-31 of the OP_AMIGA_CALL regspec operand and of a CL_STUB_LIBCALL
 * stub's `a` field (bits 0-27 hold the seven register nibbles).  The
 * Lisp-visible AMIGA:%FFI-CALL / CALL-LIBRARY-FAST regspec is a positive
 * fixnum and can therefore only carry kinds 0-3 (bits 28-29) — kinds 4-8
 * reach the trampoline through DEFCFUN's stub (the C side packs them) or
 * CALL-LIBRARY's explicit result-kind argument.  Kind 1 (void) is the
 * former "bit 28 = void-p" flag — the encoding is backward compatible
 * with regspecs baked into existing FASLs. */
#define CL_AMIGA_RES_UNSIGNED 0   /* d0 as an unsigned 32-bit integer   */
#define CL_AMIGA_RES_VOID     1   /* discard d0, return NIL              */
#define CL_AMIGA_RES_POINTER  2   /* d0 as a foreign pointer, NULL -> NIL */
#define CL_AMIGA_RES_SIGNED   3   /* d0 as a signed 32-bit integer       */
#define CL_AMIGA_RES_BOOL     4   /* BOOL: d0.w (16-bit ABI width) != 0 -> T */
#define CL_AMIGA_RES_U16      5   /* UWORD: d0.w unsigned                */
#define CL_AMIGA_RES_I16      6   /* WORD:  d0.w sign-extended           */
#define CL_AMIGA_RES_U8       7   /* UBYTE: d0.b unsigned                */
#define CL_AMIGA_RES_I8       8   /* BYTE:  d0.b sign-extended           */
#define CL_AMIGA_RES_KIND_MAX 8
#define CL_AMIGA_RES_KIND(regspec) ((int)(((regspec) >> 28) & 0xF))
#define CL_AMIGA_REGSPEC_NIBBLES(regspec) ((uint32_t)(regspec) & 0x0FFFFFFFu)
#define CL_AMIGA_MAKE_REGSPEC(nibbles, kind) \
    (CL_AMIGA_REGSPEC_NIBBLES(nibbles) | ((uint32_t)(kind) << 28))

/* Box a raw d0 result according to KIND.  Compiled on every platform
 * (unit-tested on the host); the Amiga dispatch paths and the
 * CALL-LIBRARY* builtins all route their results through it. */
CL_Obj cl_amiga_box_result(uint32_t result, int kind);

/* The 32-bit register image of a register argument: integers (two's
 * complement), foreign pointers (address), NIL (0), T (1); anything else
 * is a TYPE-ERROR naming argument number ARG_INDEX (1-based), its register
 * REG_IDX (D0..D7 = 0..7, A0..A5 = 8..13) and the value.  Compiled on every
 * platform; the host dispatch stub applies it before its platform error. */
uint32_t cl_amiga_ffi_arg_to_u32(CL_Obj val, int arg_index, int reg_idx);

/* Resolve BASE_SYM (the library-base special variable a DEFCFUN names)
 * and call LVO OFFSET of that library with the register args in ARGS
 * (ARGS[0] = first register arg) per REGSPEC.  Shared by the VM's
 * OP_AMIGA_CALL and the CL_STUB_LIBCALL path of cl_ffi_stub_call so the
 * four error cases (unbound base, NIL base = library not open, non
 * foreign-pointer base, NULL base = a pointer a heap-image restore
 * zeroed) read identically everywhere.  ARGS must be GC-rooted (VM stack
 * slots).  Host builds signal the "only available on AmigaOS/MorphOS"
 * error after the base checks. */
CL_Obj cl_amiga_call_via_base_sym(CL_Obj base_sym, int16_t offset,
                                  uint32_t regspec, int n_args,
                                  CL_Obj *args);

/* Those four checks alone: the base address of BASE_SYM's library, never
 * 0 (the JIT's library call, jit/runtime_m68k.c, uses it too). */
uint32_t cl_amiga_library_base_address(CL_Obj base_sym);

/* Foreign-callback boundary (builtins_ffi.c; thread.h "Foreign-callback
 * boundary").  cl_ffi_deferred_error_check re-signals, on the Lisp side,
 * a condition that escaped a callback while the OS was between the caller
 * and the callback — every path that returns from foreign code calls it
 * (call-foreign, the OP_AMIGA_CALL dispatch, call-library).  No-op when
 * nothing is pending.  cl_callback_debugger_allowed answers whether the
 * interactive debugger may open on this thread right now: not while a
 * callback is running unless EXT:*CALLBACK-ERROR-POLICY* is :DEBUG. */
void cl_ffi_deferred_error_check(void);
int  cl_callback_debugger_allowed(void);

/* Invoke an FFI stub (types.h CL_FfiStub) with NARGS arguments in ARGS —
 * the runtime entry behind cl_vm_apply / OP_CALL / OP_APPLY / FUNCALL for
 * a stub function object.  Arity-checks against the stub's kind, then
 * performs the library call or the typed peek/poke.  ARGS must be
 * GC-rooted (callers copy them onto the VM stack).  builtins_ffi.c. */
CL_Obj cl_ffi_stub_call(CL_Obj stub, CL_Obj *args, int nargs);

/* Number of symbols TRACE currently has flagged, all threads (builtins.c). */
extern volatile uint32_t cl_traced_function_count;

/* FFI keyword/code tables and integer boxing shared with bindtab.c
 * (builtins_ffi.c).  *_from_keyword return -1 for an unknown keyword. */
int cl_ffi_ctype_from_keyword(CL_Obj kw);        /* :U8 .. :DOUBLE -> CL_STUB_CT_* */
int cl_ffi_res_kind_from_keyword(CL_Obj kw);     /* :UNSIGNED .. :I8 -> CL_AMIGA_RES_* */
const char *cl_ffi_ctype_name(int ctype);
const char *cl_ffi_res_kind_name(int kind);
int32_t cl_ffi_ctype_size(int ctype);            /* element bytes: 1/2/4/8 */
uint64_t cl_ffi_obj_to_u64(CL_Obj o);            /* integer -> low 64 bits (two's complement) */
CL_Obj cl_ffi_u64_to_obj(uint64_t v);            /* fixnum or bignum */
CL_Obj cl_ffi_i64_to_obj(int64_t v);

/* Number of arguments a stub takes (its fixed arity). */
int cl_ffi_stub_arity(CL_Obj stub);

/* EXT:FUNCTION-ARGLIST / EXT:FUNCTION-SOURCE-LOCATION on a function object
 * (builtins.c); DESCRIBE prints both.  :NOT-AVAILABLE when unknown. */
CL_Obj cl_function_arglist(CL_Obj fn);
CL_Obj cl_function_source_location(CL_Obj fn);

/* COMMON-LISP package handle (defined in package.c); declared here so the
 * shared defun() helper below can register into it without pulling in
 * package.h. */
extern CL_Obj cl_package_cl;

/* Shared helper: register a builtin into the COMMON-LISP package.
 * Every builtins_*.c used to carry its own byte-identical copy of this
 * (a GC-protected intern + make_function + set-function-cell); they now
 * share this one.  static inline so each TU that does not call it emits
 * nothing and no linker symbol is produced. */
static inline void defun(const char *name, CL_CFunc func, int min, int max)
{
    cl_register_builtin(name, func, min, max, cl_package_cl);
}

/* Define a one-argument type-predicate builtin whose body is just
 * `return PRED(args[0]) ? SYM_T : CL_NIL;`.  PRED is a predicate macro such as
 * CL_CHAR_P / CL_HASHTABLE_P.  Expands to the exact hand-written body (same
 * codegen), folding the ~20 near-identical `bi_*p` one-liners across the
 * builtins_*.c files into a single declaration each. */
#define DEFINE_TYPE_PREDICATE(fn, PRED)              \
    static CL_Obj fn(CL_Obj *args, int n)            \
    {                                                \
        CL_UNUSED(n);                                \
        return PRED(args[0]) ? SYM_T : CL_NIL;       \
    }

/* Static descriptor used by table-driven init.
 * min/max are int16 to keep the entry compact (max == -1 means &rest). */
typedef struct {
    const char *name;
    CL_CFunc    func;
    int16_t     min;
    int16_t     max;
} CL_BuiltinDesc;

/* Register an entire table of builtins into the given package.
 * Replaces hundreds of per-call defun() sequences with a single loop. */
void cl_register_builtins(const CL_BuiltinDesc *table, uint32_t count,
                          CL_Obj package);

/* Coerce a function designator (function or symbol) to a callable function.
   If obj is already a function/closure/bytecode, returns it unchanged.
   If obj is a symbol, returns its function binding.
   Otherwise signals an error with the given context string. */
CL_Obj cl_coerce_funcdesig(CL_Obj obj, const char *context);

/* GENSYM a fresh uninterned symbol with the given prefix. NULL prefix
 * defaults to "G". Shares the counter with CL GENSYM. */
CL_Obj cl_gensym_with_name(const char *prefix);

/* Classify a general (non-bit, non-char) array/vector element-type symbol into
 * a CL_VEC_ELT_* code, expanding user deftypes up to DEPTH levels.  Recognizes
 * only the numeric types amilisp specializes (FIXNUM, SINGLE/DOUBLE-FLOAT);
 * everything else → CL_VEC_ELT_T.  Defined in builtins_array.c and shared with
 * COERCE in builtins_type.c. */
uint8_t cl_classify_vec_elt_code(CL_Obj type, int depth);

/* Classify an array element-type specifier into the specialized storage
 * classes make-array uses, expanding user deftypes up to DEPTH levels:
 * *is_char (CHARACTER subtype → string), *is_wide_char, *is_bit
 * (→ bit-vector), *is_u8 / *is_s8 (→ packed byte vector; specialization
 * order bit > u8 > s8).  Flags it cannot determine stay untouched; T, * and
 * unrecognized specifiers set nothing (general storage).  Defined in
 * builtins_array.c; shared with TYPEP/SUBTYPEP in builtins_type.c so type
 * tests agree with what MAKE-ARRAY actually builds.
 * GC: may apply deftype expanders (cl_vm_apply) → objects can move; callers
 * must GC-protect live CL_Obj locals across the call. */
void cl_classify_array_elt_type(CL_Obj type, int depth,
                                int *is_char, int *is_wide_char, int *is_bit,
                                int *is_u8, int *is_s8,
                                int *is_u16, int *is_s16);

/* Range-check a packed-vector element VALUE against the vector's width
 * (elt_shift: 0 = 8-bit, 1 = 16-bit) and signedness; returns the raw value
 * for cl_bytevec_set or signals a catchable TYPE-ERROR.  Shared by every
 * byte-vector store path (arrays, sequences, VM/JIT ASET). */
int32_t cl_bytevec_check_value(CL_Obj value, int is_signed, int elt_shift,
                               const char *ctx);
/* "(UNSIGNED-BYTE 8)" etc. — the exact specifier name for diagnostics. */
const char *cl_bytevec_type_name(int is_signed, int elt_shift);

/* Non-interactive inspect display: write object and its components to
 * *standard-output*. Used by (inspect …) and exposed for testing. */
void cl_inspect_show_obj(CL_Obj obj);

/* --- Sequence-function keyword validation (CLHS 3.4.1.4) --- */
/* Allowed-keyword bit flags for cl_check_seq_keywords. */
#define SK_TEST          0x0001u
#define SK_TEST_NOT      0x0002u
#define SK_KEY           0x0004u
#define SK_START         0x0008u
#define SK_END           0x0010u
#define SK_COUNT         0x0020u
#define SK_FROM_END      0x0040u
#define SK_INITIAL_VALUE 0x0080u
#define SK_START1        0x0100u
#define SK_END1          0x0200u
#define SK_START2        0x0400u
#define SK_END2          0x0800u
/* Union of every sequence keyword — used by callers that accept the full set
 * and only need :bad / odd-cell / non-symbol-key rejection. */
#define SK_ALL           0x0FFFu
/* Per-function subsets for strict CLHS keyword validation. */
#define SK_FIND_KEYS    (SK_TEST | SK_TEST_NOT | SK_KEY | SK_START | SK_END | SK_FROM_END)
#define SK_FIND_IF_KEYS (SK_KEY | SK_START | SK_END | SK_FROM_END)

/* Validate the keyword plist in args[kw_start..n).  Signals a program-error
 * for an odd number of cells, a non-symbol key, or an unrecognized keyword
 * (unless :allow-other-keys is supplied non-nil).  `allowed` is an OR of the
 * SK_* flags naming the keywords this function accepts. */
void cl_check_seq_keywords(CL_Obj *args, int n, int kw_start, unsigned allowed);

/* One-index array read shared by OP_AREF (vm.c) and the JIT's helper: the
 * exact semantics of the two-argument AREF / SVREF / CHAR / SCHAR builtin
 * named by `kind` (CL_AREF_KIND_* in opcodes.h) — same type gates, same
 * bounds, same error text.  Non-allocating on the success path; signals
 * on any mismatch.  The callers inline the simple-string / simple-vector
 * fast path and land here for everything else. */
CL_Obj cl_vector_ref1(CL_Obj vec, CL_Obj idx, int kind);

/* Namestring parse/build (builtins_pathname.c).  The _syntax variants take
 * AMIGA = 1 for AmigaDOS slash rules (leading or empty component = :UP)
 * regardless of the platform; the plain ones use the platform's own. */
CL_Obj cl_parse_namestring(const char *str, uint32_t len);
CL_Obj cl_parse_namestring_syntax(const char *str, uint32_t len, int amiga);
uint32_t cl_pathname_to_namestring(CL_Pathname *pn, char *buf, uint32_t bufsz);
uint32_t cl_pathname_to_namestring_syntax(CL_Pathname *pn, char *buf,
                                          uint32_t bufsz, int amiga);

#endif /* CL_BUILTINS_H */
