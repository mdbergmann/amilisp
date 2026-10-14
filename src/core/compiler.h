#ifndef CL_COMPILER_H
#define CL_COMPILER_H

#include "types.h"

/*
 * Single-pass recursive compiler: S-expression → bytecode.
 * Handles special forms, lexical scope, upvalue capture, tail calls.
 */

/* Hard limits for one compiled function.  The compiler's buffers start small
 * and grow up to these (compiler.c), so they cost nothing until used. */
#define CL_MAX_CODE_SIZE   262144
#define CL_MAX_CONSTANTS   8192

/* Compile a top-level expression, returns a CL_Bytecode object */
CL_Obj cl_compile(CL_Obj expr);

/* Compile EXPR with the macrolet/symbol-macrolet bindings in LEX_ENV (an
 * alist from cl_build_lex_env, or CL_NIL) in scope.  Used by compile-file
 * when a form is lifted out of a top-level MACROLET (CLHS 3.2.3.1). */
CL_Obj cl_compile_lex(CL_Obj expr, CL_Obj lex_env);

/* Build the lexical macro environment (alist) contributed by a top-level
 * MACROLET / SYMBOL-MACROLET, layered over INHERITED_LEX_ENV.  Result must be
 * GC-protected by the caller. */
CL_Obj cl_macrolet_lex_env(CL_Obj bindings, CL_Obj inherited_lex_env);
CL_Obj cl_symbol_macrolet_lex_env(CL_Obj bindings, CL_Obj inherited_lex_env);

/* Compile a defun/defmacro form */
CL_Obj cl_compile_defun(CL_Obj name, CL_Obj lambda_list, CL_Obj body);

/* Register a macro expander */
void cl_register_macro(CL_Obj name, CL_Obj expander);

/* Check if symbol names a macro */
int cl_macro_p(CL_Obj name);

/* Get macro expander for a symbol */
CL_Obj cl_get_macro(CL_Obj name);

/* Compiler-macro registry (CLHS 3.2.2.1).  Returns CL_NIL when no
 * compiler macro is associated with NAME.  Expander is invoked at
 * compile time as `(expander form env)` — if it returns the original
 * form (eq), the compiler treats it as a decline and uses the regular
 * function call path. */
void cl_register_compiler_macro(CL_Obj name, CL_Obj expander);
CL_Obj cl_get_compiler_macro(CL_Obj name);

/* DEFSETF registry: ACCESSOR -> UPDATER function name (setf_table).
 * cl_get_setf_updater returns CL_NIL when ACCESSOR has no defsetf.
 * Both hash-indexed (CL_AlistIndex below); the registrar conses outside
 * the tables write lock. */
void cl_register_setf_updater(CL_Obj accessor, CL_Obj updater);
CL_Obj cl_get_setf_updater(CL_Obj accessor);

/* Compaction moved the keys/entries of the deftype, compiler-macro and
 * defsetf table indexes — mark all three stale (mem.c, world stopped). */
void cl_compiler_indexes_gc_invalidate(void);

void cl_compiler_init(void);

/* True unless EXT:*CAPTURE-DOCUMENTATION* is NIL: whether the defining
 * forms record their docstrings (compiler_extra.c emit_doc_call). */
int cl_capture_documentation_p(void);

/* (ext:%compiler-pool-stats).  Blocks in use by a running compile are not
 * counted. */
typedef struct {
    uint32_t parked;          /* compiler blocks parked in the pool */
    uint32_t block_bytes;     /* size of one block */
    uint32_t buffer_bytes;    /* bytecode + constants buffers they hold */
    uint32_t largest_parked;  /* largest single buffer a parked block holds */
    uint32_t code_max;        /* largest bytecode buffer ever grown (bytes) */
    uint32_t const_max;       /* largest constants table ever grown (entries) */
} CL_CompilerPoolStats;
void cl_compiler_pool_stats(CL_CompilerPoolStats *st);

/* Release the pooled CL_Compiler blocks (8 pre-warmed) with their bytecode
 * and constants buffers, and the compiler's own locks.  Call at process exit
 * only, once no compile can be in flight — during a run the pool must keep
 * its blocks (see compiler.c). */
void cl_compiler_shutdown(void);

/* Release the interned source-file pool.  Must run AFTER cl_mem_shutdown:
 * bytecode objects point into this pool, and the arena walk there clears
 * those pointers first.  See compiler.c. */
void cl_compiler_release_source_pool(void);

/* Expand one level of macro (returns form unchanged if not a macro call) */
CL_Obj cl_macroexpand_1(CL_Obj form);

/* Same as cl_macroexpand_1, but installs LEX_ENV (an alist of
 * (SYMBOL . EXPANSION) pairs, or CL_NIL) as the current lexical
 * environment before invoking the expander, so the user's
 * &environment parameter sees it via %MACROEXPAND-ENV.  The previous
 * value is saved and restored around the expander call, so nested
 * macroexpansion is safe. */
CL_Obj cl_macroexpand_1_env(CL_Obj form, CL_Obj lex_env);

/* Sentinel symbol marking a local-macro entry inside a lex-env alist.
 * cl_build_lex_env emits both symbol-macros (as `(name . expansion)`)
 * and macrolet bindings (as `(SYM_LEX_LOCAL_MACRO . (name . expander))`)
 * so that &environment-aware macro expanders can macroexpand calls to
 * macrolet-bound macros. */
extern CL_Obj SYM_LEX_LOCAL_MACRO;

/* AMIGA::%FFI-CALL — sentinel symbol matched by compile_call to emit
 * OP_AMIGA_CALL.  Initialized by cl_builtins_amiga_init: the actual
 * symbol on PLATFORM_AMIGA, CL_NIL on host (fast-path inactive). */
extern CL_Obj cl_amiga_ffi_call_sym;

/* OP_AMIGA_CALL VM dispatch helper — defined in builtins_amiga.c.
 * Invokes the Amiga library trampoline and boxes the result (or returns
 * CL_NIL when bit 31 of regspec is set, the void-call elision flag).
 * On non-Amiga builds the body signals an error. */
CL_Obj cl_amiga_ffi_call_dispatch(uint32_t base_addr, int16_t offset,
                                  uint32_t regspec, int n_args,
                                  CL_Obj *arg_base);

/* AMILISP::%STRUCT-REF and AMILISP::%STRUCT-SET — sentinel symbols
 * matched by compile_call to emit OP_STRUCT_REF / OP_STRUCT_SET.
 * Initialized by cl_builtins_struct_init. */
extern CL_Obj cl_struct_ref_sym;
/* AMILISP::%HANDLER-CASE — the HANDLER-CASE special form (builtins_condition.c) */
extern CL_Obj cl_handler_case_sym;
extern CL_Obj cl_struct_set_sym;

/* Structure-layout dependencies of the file being compiled
 * (builtins_struct.c, "Layout dependencies of compiled code").
 * AMILISP::*STRUCT-LAYOUT-DEPS* is bound to NIL per file by LOAD and
 * COMPILE-FILE and collects the names DEFSTRUCT's compiler macros report;
 * cl_struct_layout_hash is what the FASL's DEPS trailer stores per name
 * (returns 0 when the type is not registered). */
extern CL_Obj cl_struct_deps_sym;
void cl_struct_note_layout_use(CL_Obj name);
int  cl_struct_layout_hash(CL_Obj type_name, uint32_t *hash_out);

/* FFI::%DEFINE-CSTRUCT-ACCESSORS — FFI:DEFCSTRUCT's bulk accessor
 * installer (builtins_ffi.c).  compile_call matches the symbol and, when
 * the argument is a quoted entry list, registers the ACCESSOR ->
 * %SET-ACCESSOR DEFSETF pairs at COMPILE time (cl_ffi_cstruct_register_setfs)
 * — the immediate side effect the per-field DEFSETF forms used to have —
 * then compiles the call as usual.  Initialized by cl_builtins_ffi_init. */
extern CL_Obj cl_ffi_define_cstruct_accessors_sym;
void cl_ffi_cstruct_register_setfs(CL_Obj entries);

/* Look up a global symbol-macro expansion (from DEFINE-SYMBOL-MACRO).
   Returns CL_NIL when the symbol has no global symbol-macro binding —
   but also when the binding expands to NIL.  To distinguish, use
   cl_lookup_global_symbol_macro_p. */
CL_Obj cl_lookup_global_symbol_macro(CL_Obj sym);

/* Like cl_lookup_global_symbol_macro, but writes the expansion to *out
   and returns 1 iff sym has a binding (even if it expands to NIL).
   Returns 0 and leaves *out untouched if there is no binding. */
int cl_lookup_global_symbol_macro_p(CL_Obj sym, CL_Obj *out);

/* Type expander table (for deftype) */
void cl_register_type(CL_Obj name, CL_Obj expander);
CL_Obj cl_get_type_expander(CL_Obj name);

/* Setf function table: (defun (setf accessor) ...) */
void cl_register_setf_function(CL_Obj accessor, CL_Obj setf_fn_sym);
/* Resolve the (setf FOO) function-name to the symbol holding its function. */
CL_Obj cl_setf_function_symbol(CL_Obj accessor);
/* Package-qualified hidden AMILISP symbol that stores accessor's (setf
 * accessor) function — shared by the compiler and CLOS so they never collide
 * on same-named accessors in different packages. */
CL_Obj cl_setf_store_symbol(CL_Obj accessor);

/* Optimization settings (used by (declare (optimize ...))) */
typedef struct {
    uint8_t speed;   /* 0-3, default 1 */
    uint8_t safety;  /* 0-3, default 1 */
    uint8_t debug;   /* 0-3, default 1 */
    uint8_t space;   /* 0-3, default 1 */
} CL_OptimizeSettings;

/* Opaque forward declaration — full definition in compiler_internal.h.
 * Only used here as a pointer type, mirroring thread.h's forward
 * declaration for CL_Thread.active_compiler. */
struct CL_Compiler_s;

/* Effective settings the compiler consults while emitting code now live in
 * CL_Compiler.optimize_settings (compiler_internal.h) instead of a shared
 * global: each compile has its own private CL_Compiler chain (never shared
 * across threads — see cl_active_compiler / CL_Thread.active_compiler), so
 * a body (declare (optimize ...)) override — scoped per CLHS 3.3.4 via
 * save/restore around the declaring body's postlude — can never leak into
 * a concurrent compile on another thread the way a single process-wide
 * cl_optimize_settings global could.  A freshly pushed CL_Compiler inherits
 * its parent's current effective settings, or cl_optimize_global when it is
 * the root of a fresh top-level compile. */

/* Proclaimed baseline, written only by DECLAIM/PROCLAIM under
 * cl_tables_wrlock().  Body-scoped declarations never touch it; it is what
 * seeds a fresh top-level compile's effective settings. */
extern CL_OptimizeSettings cl_optimize_global;

/* Thread-safety: rwlock protecting all compiler/definition tables */
extern void *cl_tables_rwlock;

/* Helpers around cl_tables_rwlock that maintain per-thread reader-hold
 * counts (CL_Thread.rdlock_tables_held) and a per-thread stack of the
 * rdlock call sites still outstanding.  Use these in preference to
 * platform_rwlock_{rd,wr,un}lock(cl_tables_rwlock) so that a stuck
 * writer can be diagnosed ("which thread leaked a reader, and from
 * where?") and so bi_condition_wait can refuse to sleep while holding
 * one.  The single-threaded fast path (CL_MT() == 0) skips the
 * underlying platform call entirely. */
void cl_tables_rdlock_at(const char *site);
void cl_tables_wrlock(void);
void cl_tables_rwunlock(void);
/* Prepend VALUE onto the (prepend-only, snapshot-walked) list at *TABLE_P
 * under the tables write lock, WITHOUT allocating while the lock is held.
 * Allocating inside cl_tables_wrlock deadlocks under MT: the allocation
 * can trigger a stop-the-world GC that waits for every peer thread to
 * park, while a peer blocked on cl_tables_rdlock/wrlock can never park —
 * a circular STW-vs-rwlock wait.  TABLE_P must be the address of a
 * GC-rooted global table head (stable address, marked from mem.c). */
void cl_table_prepend_locked(CL_Obj *table_p, CL_Obj value);
/* Snapshot per-thread cl_tables_rwlock reader counts (and the sites
 * that took them) to stderr. */
void cl_tables_dump_rdlock_holders(const char *header);

/* --- Generic symbol->entry hash index over a prepend-only alist ---
 *
 * O(1) lookup for the cl_tables_rwlock-guarded alists whose every entry
 * is keyed by a symbol in its CAR — the generalization of the struct
 * registry index (builtins_struct.c, spec 3.1).  TYPEP on a symbol probes
 * the struct registry, the condition hierarchy, AND the deftype table
 * before reaching the CLOS class hash table; with hundreds of registered
 * conditions/deftypes those linear walks made every TYPEP O(types) —
 * measured as multi-minute log4cl appender stalls when the print-object
 * hook ran one TYPEP per printed node (eta-hab item-definition phase,
 * 2026-07-14, diagnosed via AMILISP_LOCK_DIAG).
 *
 * Same protocol as the struct index: probes run under the tables rdlock
 * against a CLEAN index; a dirty/unbuilt index is rebuilt lazily under
 * the wrlock with platform_alloc only (no arena allocation, no cl_error
 * while the lock is held); compaction invalidates via
 * cl_alist_index_invalidate (world stopped); registration through
 * cl_alist_index_prepend dirty-marks in the same wrlock critical section
 * as the prepend; OOM or a malformed cell disables the index permanently
 * and lookups fall back to the linear snapshot walk — slower, never
 * wrong. */
typedef struct {
    CL_Obj name;    /* key symbol; CL_NIL (== 0) = empty slot */
    CL_Obj entry;   /* the full alist entry whose CAR is `name` */
} CL_AlistIndexSlot;

typedef struct {
    CL_Obj *table_p;          /* the indexed alist global (GC-rooted) */
    CL_AlistIndexSlot *slots;
    uint32_t cap;             /* power of two; 0 = unallocated */
    int dirty;                /* alist changed or compaction moved objects */
    int disabled;             /* OOM / malformed cell — permanent fallback */
} CL_AlistIndex;

/* Find the first entry whose CAR is KEY, or NIL.  First-match semantics
 * identical to the linear walk (head-most entry wins on re-registration). */
CL_Obj cl_alist_index_find(CL_AlistIndex *ix, CL_Obj key);
/* Prepend VALUE onto *ix->table_p and dirty-mark the index in one wrlock
 * critical section (allocation-free under the lock, like
 * cl_table_prepend_locked). */
void cl_alist_index_prepend(CL_AlistIndex *ix, CL_Obj value);
/* Mark the index stale (compaction moved keys/entries; world stopped). */
void cl_alist_index_invalidate(CL_AlistIndex *ix);
/* Free and re-enable the index (module re-init alongside the table's
 * own = CL_NIL reset). */
void cl_alist_index_reset(CL_AlistIndex *ix);

/* Compile-time stringification of __LINE__ so each rdlock site reports
 * its file:line in the diagnostic output. */
#define _CL_RWL_STR(x)  #x
#define _CL_RWL_XSTR(x) _CL_RWL_STR(x)
#define cl_tables_rdlock() \
    cl_tables_rdlock_at(__FILE__ ":" _CL_RWL_XSTR(__LINE__))

/* Process a single declaration specifier.  `proclaimed` selects the scope
 * of an (optimize ...) specifier: 1 for DECLAIM/PROCLAIM (updates the
 * global baseline AND, when C is non-NULL, the effective settings), 0 for
 * a body (declare ...) (updates only the effective settings, which the
 * enclosing body's postlude restores).  C is the compiler whose
 * optimize_settings should reflect the change immediately (NULL when
 * called outside any active compile, e.g. the PROCLAIM builtin invoked
 * from the top-level REPL — only the global baseline is updated then). */
void cl_process_declaration_specifier(struct CL_Compiler_s *c, CL_Obj spec, int proclaimed);

/* GC root walk: visit every CL_Obj slot held by a thread's active compiler
 * chain (instantiated by the marker and the compactor, see gc_trace.c). */
struct CL_Thread_s;
void cl_compiler_gc_visit_thread(struct CL_Thread_s *t, void (*visit)(CL_Obj *));

/* Save/restore active compiler chain across non-local exits.
 * cl_compiler_mark() returns an opaque snapshot.
 * cl_compiler_unwind_to() frees compilers allocated since the mark whose
 * owning C-stack frame was abandoned by the longjmp: LANDING_ANCHOR is the
 * landing site's own frame (CL_CAPTURE_SP() at the setjmp-return point), and
 * a compiler is freed only while its recorded anchor is strictly deeper.
 * A compiler owned by a still-live frame (an in-progress compile enclosing
 * the landing VM run) anchors shallower and stops the walk, so it is never
 * freed out from under the C code using it. */
void *cl_compiler_mark(void);
void cl_compiler_unwind_to(void *saved, void *landing_anchor);
/* Error-frame unwind variant: the C-level catch predates every abandoned
 * frame, so everything down to the mark is freed unconditionally. */
void cl_compiler_force_restore_to(void *saved);

/* Intern a source-file path into a process-lifetime pool and return
 * the stable pointer.  Use this anywhere a const char* is stored long-term
 * (in particular CL_Bytecode.source_file and cl_current_source_file when
 * the input came from a stack buffer or a CL_String that may be GC'd).
 * NULL/empty input returns NULL. */
const char *cl_intern_source_file(const char *path);

#endif /* CL_COMPILER_H */
