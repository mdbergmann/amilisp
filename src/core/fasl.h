#ifndef CL_FASL_H
#define CL_FASL_H

/*
 * FASL (Fast Loading) — binary serialization of compiled bytecode.
 *
 * File format: big-endian binary (native for 68020).
 *
 * Header:
 *   magic:   4 bytes  "CLFA"
 *   version: 2 bytes  format version
 *   flags:   2 bytes  CL_FASL_FLAG_* (bit 0: a DEPS trailer follows the units)
 *   n_units: 4 bytes  number of top-level compilation units
 *
 * Per unit:
 *   bc_len:  4 bytes  length of serialized bytecode blob
 *   <serialized bytecode blob>
 *
 * Bytecode blob contains: opcodes, constant pool, arity, locals,
 * upvalues, optional/key info, source location, line map, name.
 *
 * Constants are type-tagged and serialized recursively.
 */

#include "types.h"
#include "../platform/platform.h"

#define CL_FASL_MAGIC    0x434C4641  /* "CLFA" */
#define CL_FASL_VERSION  40  /* v40: no wire change; load-bearing cache invalidation for two &optional fixes in the lambda-list prologue.  The &rest slot now comes right after the optionals (where every frame setup puts the list) instead of after their supplied-p slots, which lost the list to the first supplied-p store ((a &optional (b 2 bp) &rest r) saw R = NIL); and an optional's supplied-p variable is bound before the next default runs (CLHS 3.4.1: an init-form sees every parameter to its left, supplied-p variables included -- it read the global, unbound).  A FASL compiled before this keeps both bugs.  v39: bit 7 of a bytecode's serialized flags byte is the JIT's (optimize (speed 3)) hint (CL_BC_JIT_SPEED, specs/lazy-jit.md): a function compiled under speed 3 is JIT-compiled at definition, everything else once it turns hot.  The in-memory flags keep bits 0-1; an older reader would have taken bit 7 for a flag -- harmless there (the JIT declines such a function) but a different byte layout all the same.  v38: no wire change; every DEFVAR with an initial-value form compiles to different bytes.  It now tests (CLAMIGA::%GLOBALLY-BOUND-P 'name) and jumps over the form when the variable is bound (CLHS: the form "is evaluated only if name is not already bound" -- OP_DEFVAR alone checked only at the store, so the form's side effects ran again on every reload), and DEFVAR / DEFPARAMETER / DEFCONSTANT compile their value form out of tail position (in a function body it became a tail call and the store after it was dead code).  A FASL compiled before this keeps both bugs and, the other way round, a new FASL calls a builtin an old runtime lacks; same doctrine as v27/v37.  v37: no wire change; load-bearing cache invalidation for DEFSTRUCT's hidden slot setters.  The function behind (SETF (acc obj) val) is CLAMIGA::%STRUCT-SET-<home package>::<accessor> now; it was %SET-<accessor> interned in the package the DEFSTRUCT was read in, where it collided with user functions of that name ((defstruct view title) took over a user's %SET-VIEW-TITLE, and its compiler macro rewrote direct calls to the user's function into a slot write on the first argument).  A FASL compiled before this defines only the old names and calls them wherever a structure SETF was not inlined (NOTINLINE, or the structure unknown at compile time), so mixing it with a structure file compiled by the new DEFSTRUCT is an undefined function at run time; and it still carries the collision itself.  Same doctrine as v27.  v36: strings, wide strings, vectors and multi-dimensional arrays, bit vectors, packed byte vectors and pathnames join the shared-object set — each is wrapped in FASL_TAG_OBJ_DEF (u16 id) on first emission and becomes a 3-byte FASL_TAG_OBJ_REF afterwards, so a literal that occurs twice in one top-level form ('(#1="s" #1#)) is still one object after COMPILE-FILE + LOAD; before, only conses, symbols, structures, closures, bytecode and locks kept their identity and everything else came back as separate copies.  Every unit with a string or vector constant has different bytes.  A v35 reader would parse the wrapper (the tags are old) but registers a vector or pathname only after its elements, by which time a child OBJ_DEF has cleared the pending id — the OBJ_REF to it then resolves to NIL — so the bump is load-bearing.  v35: the header's flags word is no longer reserved — bit 0 (CL_FASL_FLAG_DEPS) announces a DEPS trailer after the last unit: "DEPS", u16 count, then per entry u16+bytes package name, u16+bytes symbol name, u32 layout hash, one per structure whose layout DEFSTRUCT's compiler macros baked into the file (inlined accessor slot indices, positional keyword-constructor calls, (:INCLUDE parent) slot copies).  The loader compares the hashes against the live registry before running any unit; LOAD's cache recompiles on a mismatch, an explicit FASL load signals.  DEFSTRUCT's accessor compiler macros also expand through the new CLAMIGA::%STRUCT-ACC-FORM, so every unit that defines a structure has different bytes.  The bump is load-bearing twice over: a v34 FASL carries no trailer and would keep loading unchecked forever, and its accessor macros never report a dependency, so files compiled against them would get no trailer either.  v34: five new opcodes 0xC0-0xC4 for the string-scan fast path (specs/performance.md 4.4) — AREF (u8 accessor kind: the compiler inlines two-argument AREF/SVREF/CHAR/SCHAR), CHAREQ (two-argument CHAR=), CMP_BR (u8 kind + i32: the peephole fuses LT/GT/LE/GE/NUMEQ/CHAREQ with a following JNIL or JTRUE), PUSH_LOCAL and POP_LOCAL (u8 slot: PUSH/POP on an unboxed lexical variable, compiled ahead of the macro).  CASE also emits `EQ; JNIL next` for a clause's last key instead of `EQ; JTRUE body; JMP next`.  A v33 reader would hit an unknown opcode byte, so the bump is load-bearing.  v33: fourteen superinstruction opcodes 0xB2-0xBF (specs/performance.md 4.3, Tier-4 phase 3), fused by the peephole pass from the compiler's adjacent pairs and triples — STORE_POP (u8), LOAD_LOAD (u8 u8), LOAD_CALL_GLOBAL (u8 u16 u8), LOAD_STRUCT_REF (u8 u8), LOAD_MV_RESET (u8), LOAD_RET (u8), EQ_JNIL (i32), GLOAD_JNIL (u16 i32), LOAD_JNIL (u8 i32), LOAD_CONST (u8 u16), GLOAD_CALL_GLOBAL (u16 u16 u8), GLOAD_EQ_JNIL (u16 i32), LOAD_STORE_POP (u8 u8), POP_LOAD (u8) — and the pass now runs at every speed above 0 (it was speed >= 2), so a default-speed compile writes these bytes; a v32 reader would hit an unknown opcode byte, so the bump is load-bearing.  v32: six new opcodes (specs/performance.md 4.2, Tier-4 phase 2).  OP_HANDLER_CASE_PUSH (0xB0, u16 clause-type-list const + i32 landing) / OP_HANDLER_CASE_POP (0xB1) — HANDLER-CASE is the special form CLAMIGA::%HANDLER-CASE now (one NLX frame + one fixnum-tagged handler binding per clause, a per-clause OP_JMP table as the landing) instead of a macro over CATCH + BLOCK + HANDLER-BIND that consed a box and a closure per entry.  OP_CALL_GLOBAL (0xAC) and OP_TAILCALL_GLOBAL (0xAD), each `u16 symbol-const-index, u8 nargs` — the fused form of `OP_FLOAD sym; OP_CALL n` that compile_call now emits for every call to a global function; the callee is resolved from the symbol in the handler, after the arguments, and never occupies a stack slot (the new CL_Frame.fslot field records that for OP_RET).  OP_MV_SAVE (0xAE) / OP_MV_RESTORE (0xAF), no operands — compile_unwind_protect keeps the protected form's values on a per-thread save stack across the cleanup forms instead of OP_MV_TO_LIST + a VALUES-LIST call.  A v31 reader would hit an unknown opcode byte, so the bump is load-bearing.  v31: no wire change; load-bearing cache invalidation for the multiple-values hygiene pass.  The compiler now emits OP_MV_RESET wherever a form's value reaches an MV observer (OP_RET/OP_HALT, OP_MV_TO_LIST, OP_MV_LOAD, OP_NTH_VALUE, OP_BLOCK_RETURN) through an opcode that leaves the MV buffer alone — OP_LOAD, OP_CELL_REF, the peeking stores — and moved AND/OR's reset ahead of the OP_DUP so the short-circuit exit is single-valued too.  A FASL compiled by an earlier build carries the old, reset-free bytecode, so every function in it keeps returning whatever multiple values happened to be left in the buffer ((let ((x (floor 7 2))) x) returning 3 and 1); the units have to be recompiled rather than trusted.  v30: new FASL_TAG_FFI_STUB (0x21) for the TYPE_FFI_STUB binding descriptor that AMIGA.FFI:DEFCFUN / FFI:DEFCSTRUCT now install instead of wrapper closures (specs/raw-bindings-footprint.md, Phase 1) — kind u8, ctype u8, b i16, a u32, name(obj), aux(obj); and the OP_AMIGA_CALL regspec operand's result kind widened from bits 28-29 to bits 28-31 (kinds 4-8: bool/u16/i16/u8/i8, formerly Lisp post-processors).  A v29 reader would reject the tag and would mis-decode a kind >= 4 regspec as kind 0-3, so the bump is load-bearing.  v29: no wire change; cache invalidation for function NAMEs that used to be NIL.  FLET/LABELS locals now carry the local function's own name, and DEFMETHOD expands to NAMED-LAMBDA so a method body carries its generic function's name — three frame kinds that backtraced as "<anonymous>" (a method frame sits directly under %GF-DISPATCH-*, so every CLOS backtrace was a hole).  The NAME field was always written; a FASL compiled by an earlier build carries NIL there permanently, and its defmethod units carry the old LAMBDA expansion, so cached files have to be recompiled rather than trusted.  Same doctrine as v26, which fixed the other half of this.  v28: no wire change; load-bearing cache invalidation for the SLYNK-IO-PACKAGE poisoning race.  cl_current_package used to be one SHARED C global re-synced from the calling thread on every *PACKAGE* bind/unbind, so while one thread ran COMPILE-FILE another thread's *PACKAGE* bind (slynk's control thread binds SLYNK-IO-PACKAGE around every wire message) redirected the compiling reader mid-file: random tokens interned into SLYNK-IO-PACKAGE / COMMON-LISP-USER, one source name became two symbols, and defmacro bodies compiled parameter references as GLOBAL lookups — the cached FASL then fails forever with "Unbound variable: FORMAT-CONTROL" (cl-ppcre errors.fasl, 2026-08-11) at macroexpansion time.  cl_current_package is per-thread now (CL_Thread.current_package), but any FASL compiled by an earlier build inside a Sly/slynk session may already carry the poison — syntactically valid, version-identical, undetectable at load.  Bumping abandons every pre-fix cache wholesale so poisoned units are recompiled under the fixed runtime.  Same doctrine as v26/v27.  v27: 47 late-added %-helpers (dispatch self-heal chain, reader/writer-IC discriminators, %REGISTER-STRUCT-CLASS, %SLOT-VALUE-SLOW family, the %DBIND- and %SEQ- expanders) are now pre-interned+exported in CLAMIGA via clos_internal_names[] (package.c), so their serialized home package changes from COMMON-LISP-USER (or COMMON-LISP, on a boot-time source compile) to CLAMIGA.  The wire format is unchanged; the bump is load-bearing cache invalidation: a FASL written before this carries the old homes, and mixing it with the new runtime resurrects the two-symbols-one-name split that broke CLOS dispatch and defstruct MOP on the real Amiga (2026-08-09).  v26: the bytecode NAME field is now correct for any function whose body contains a nested lambda.  The DEFUN -> lambda name hand-off (pending_lambda_name) was read only after the body had been compiled, so the first nested lambda claimed the name and the function itself serialized with NAME = NIL — every backtrace through it showed "<anonymous>".  The wire format is unchanged (NAME was always written); the bump is a cache invalidation, since a FASL compiled by the old compiler still carries the wrong names and would keep the anonymous backtraces forever.  Same rationale as v21-v23.  v25: FASL_TAG_BYTE_VECTOR gains a u8 elt_shift field after the signedness byte (packed (unsigned-byte 16)/(signed-byte 16) vectors, TYPE_BYTE_VECTOR with 2-byte elements); 16-bit payloads are serialized per-element big-endian so FASLs stay portable between LE hosts and the BE m68k target.  Old readers would misparse the extra byte as payload, so the bump is load-bearing.  v24: new FASL_TAG_BYTE_VECTOR (0x20) for the packed (unsigned-byte 8)/(signed-byte 8) TYPE_BYTE_VECTOR heap type — u32 length, u8 signedness, raw bytes.  Old readers would reject the new tag anyway, but the version bump keeps the rule uniform: any wire-format change invalidates cached FASLs.  v23: top-level DECLAIM now emits a runtime (PROCLAIM 'spec) call per specifier so proclamations survive the FASL round trip (CLHS: declaim ~ eval-when (:compile-toplevel :load-toplevel :execute) proclaim — previously only the compile-time leg existed and declaims evaporated from compiled files). Changes the bytecode emitted for every top-level declaim; bumping forces stale cached FASLs (compiled by the old compiler, still silently dropping declaims on load) to be recompiled. v22: (defvar name) with no init-form now emits a runtime (%MARK-SPECIAL 'name) call so the special proclamation works for variables loaded from a precompiled FASL (the compile-time CL_SYM_SPECIAL flag does not survive into a fresh load — bare defvars were non-special on load, breaking dynamic LET binding e.g. serapeum DEFPLACE). v21: defconstant emits a runtime (%MARK-CONSTANT 'name) call so CONSTANTP works for FASL-loaded constants. */

/* Header flags */
#define CL_FASL_FLAG_DEPS 0x0001   /* DEPS trailer after the last unit: the
                                      structure layouts the code was compiled
                                      against (fasl.c, "DEPS trailer") */

/* Serialized constant type tags */
#define FASL_TAG_NIL         0x00
#define FASL_TAG_T           0x01
#define FASL_TAG_FIXNUM      0x02
#define FASL_TAG_CHARACTER   0x03
#define FASL_TAG_SYMBOL      0x04
#define FASL_TAG_STRING      0x05
#define FASL_TAG_CONS        0x06
#define FASL_TAG_BYTECODE    0x07
#define FASL_TAG_SINGLE_FLOAT 0x08
#define FASL_TAG_DOUBLE_FLOAT 0x09
#define FASL_TAG_RATIO       0x0A
#define FASL_TAG_BIGNUM      0x0B
#define FASL_TAG_VECTOR      0x0C
#define FASL_TAG_UNBOUND     0x0E
#define FASL_TAG_BIT_VECTOR  0x0F
#define FASL_TAG_PATHNAME    0x10
#define FASL_TAG_CLOSURE     0x11
#define FASL_TAG_FUNCTION    0x12
#define FASL_TAG_CONST_REF   0x13  /* u16 index: back-reference to earlier constant in same bytecode */
#define FASL_TAG_GENSYM_DEF  0x14  /* u16 id, u16 name_len, bytes: define new uninterned symbol */
#define FASL_TAG_GENSYM_REF  0x15  /* u16 id: reference to previously defined gensym */
#define FASL_TAG_PACKAGE     0x16  /* u16 name_len, bytes: package lookup by name */
#define FASL_TAG_COMPLEX     0x17  /* realpart, imagpart: complex number */
#ifdef CL_WIDE_STRINGS
#define FASL_TAG_WIDE_STRING 0x18  /* u32 length, u32[] codepoints */
#endif
#define FASL_TAG_STRUCT      0x19  /* type_desc(sym), u32 n_slots, slots... */
#define FASL_TAG_OBJ_DEF    0x1A  /* u16 id: define shared object (followed by object data) */
#define FASL_TAG_OBJ_REF    0x1B  /* u16 id: back-reference to previously defined shared object */
#define FASL_TAG_CLASS_REF  0x1C  /* class-name(symbol): (find-class name) at load time.
                                     CLOS class metaobjects (STANDARD-CLASS/BUILT-IN-CLASS
                                     structs) are cyclic by design — preserves identity
                                     and avoids re-creating broken stand-ins. */
#define FASL_TAG_LOAD_FORM  0x1E  /* creation-form(obj), init-form(obj):
                                     a literal object whose class has a user
                                     MAKE-LOAD-FORM method.  Reconstructed at
                                     load time by evaluating the creation form
                                     to build the object, registering it at the
                                     enclosing OBJ_DEF id (so the init form's
                                     OBJ_REF self-reference resolves to it),
                                     then evaluating the init form for effect.
                                     The forms are plain data (cons trees);
                                     they are compiled+evaluated at load. */
#define FASL_TAG_LOCK       0x1D  /* u8 flags (bit 0 = recursive), name(obj):
                                     fresh-at-load-time mutex.  Identity is not
                                     preserved across host processes (the platform
                                     mutex is per-process state), but multiple
                                     references within one FASL share via OBJ_DEF/REF
                                     so a struct slot and its callers all see the
                                     same fresh lock after load. */
#define FASL_TAG_MD_ARRAY   0x1F  /* u32 length, u8 rank, u32[rank] dims, then
                                     `length` elements: a multidimensional array
                                     (rank>1).  FASL_TAG_VECTOR stays the compact
                                     rank<=1 form; only true multi-dim arrays use
                                     this tag, so the rank/dims are preserved
                                     across compile-file instead of collapsing to
                                     a flat 1-D vector. */
#define FASL_TAG_BYTE_VECTOR 0x20 /* u32 length, u8 is_signed, u8 elt_shift,
                                     then `length` elements: packed
                                     (unsigned/signed-byte 8/16) vector
                                     (TYPE_BYTE_VECTOR).  8-bit payload is raw
                                     bytes; 16-bit is per-element big-endian
                                     u16 (portable LE host -> BE m68k). */
#define FASL_TAG_FFI_STUB   0x21  /* u8 kind, u8 ctype, i16 b, u32 a,
                                     name(obj), aux(obj): an FFI binding
                                     descriptor (TYPE_FFI_STUB, types.h) —
                                     what DEFCFUN / DEFCSTRUCT install in a
                                     function cell.  Pure data, so unlike
                                     FASL_TAG_FUNCTION it is reconstructed,
                                     not looked up by name. */

/* Error codes for FASL operations */
#define FASL_OK              0
#define FASL_ERR_OVERFLOW    -1   /* Buffer too small */
#define FASL_ERR_TRUNCATED   -2   /* Unexpected end of data */
#define FASL_ERR_BAD_MAGIC   -3   /* Invalid magic number */
#define FASL_ERR_BAD_VERSION -4   /* Unsupported version */
#define FASL_ERR_BAD_TAG     -5   /* Unknown constant type tag */
#define FASL_ERR_TOO_DEEP   -6   /* Object graph too deep for C stack */
#define FASL_ERR_BAD_LENGTH -7   /* Implausible element count (corrupt file) */
#define FASL_ERR_NONPORTABLE -8  /* Writer refused a non-ASCII string literal
                                    in portable mode (CLAMIGA_FASL_PORTABLE=1):
                                    it would need FASL_TAG_WIDE_STRING, which
                                    byte-string builds (the AmigaOS release
                                    binaries) cannot decode. */

/* --- Portable-FASL mode (host -> Amiga lib FASLs) ---
 *
 * CLAMIGA_FASL_PORTABLE=1 (environment, read once) makes the writer refuse any
 * string that would be written as FASL_TAG_WIDE_STRING, i.e. a string literal
 * containing a character above U+007F, with FASL_ERR_NONPORTABLE.  The
 * compile-file diagnostic then names the source line and the offending code
 * point instead of the Amiga failing the whole unit with BAD_TAG at load time.
 * `make fasl`, `make fasl-amiga` and scripts/make-binary-release.sh set it;
 * see tests/test_lib_fasl_portable.sh.  Docstrings never reach the writer
 * (the compiler drops them) and comments never reach the reader, so only
 * strings that survive into the FASL count. */
int  cl_fasl_portable_mode(void);
void cl_fasl_set_portable_mode(int on);          /* explicit override (tests) */
/* Source name recorded for a compile-file input in portable mode: relative
 * to the cwd when the file lies under it, so a shipped FASL's backtraces
 * name "lib/amiga/ahi.lisp" and not the build host's absolute path. */
const char *cl_fasl_portable_source_name(const char *path);
/* cl_fasl_nonportable_detail() is declared below, after CL_FaslWriter. */

/* Max uninterned symbols tracked per FASL file (for gensym dedup) */
#define FASL_MAX_GENSYMS 1024

/* Max shared objects tracked per unit (for cycle/sharing detection).
 * Capped at uint16_t max because the wire format encodes IDs as u16.
 * Keeping a generous limit avoids re-serializing the same closure many
 * times when a single top-level form (e.g. a CLOS dispatch table) builds
 * thousands of structurally-shared closures — without dedup, the FASL
 * unit balloons exponentially. */
#define FASL_MAX_SHARED 65535

/* --- Serialization buffer --- */

typedef struct {
    uint8_t *data;
    uint32_t capacity;
    uint32_t pos;
    int error;          /* FASL_OK or error code */
    /* Uninterned symbol dedup table */
    CL_Obj   gensym_objs[FASL_MAX_GENSYMS];  /* original CL_Obj values */
    uint16_t gensym_count;
    /* Shared object dedup table (cycle/sharing detection for closures, bytecodes,
     * structs, symbols, locks, and conses).  shared_objs is indexed by id and
     * holds the original CL_Obj.  shared_hash maps obj -> id+1 (0 = empty slot)
     * via open-addressing linear probing — keeps writer-side dedup O(1) per
     * lookup even when shared_count grows into the tens of thousands (cons
     * dedup makes that realistic).  Both tables lazily allocated. */
    CL_Obj  *shared_objs;      /* heap-allocated, FASL_MAX_SHARED entries */
    uint16_t shared_count;
    uint16_t pad_;             /* explicit pad: keep 32-bit struct layout stable */
    uint16_t *shared_hash;     /* hash slots; size shared_hash_cap (power of two) */
    uint32_t shared_hash_cap;  /* 0 until allocated; always a power of two when set */
    /* Per-writer, not global: two writers (e.g. two MP threads each running
     * compile-file under CLAMIGA_FASL_PORTABLE=1) must not race on a shared
     * detail buffer and risk one thread's diagnostic naming the other's file. */
    char     nonportable_detail[200];
} CL_FaslWriter;

const char *cl_fasl_nonportable_detail(const CL_FaslWriter *w);  /* detail of w's last refusal */

/* --- Deserialization buffer --- */

typedef struct {
    const uint8_t *data;
    uint32_t size;
    uint32_t pos;
    int error;
    /* Uninterned symbol dedup table */
    CL_Obj   gensym_objs[FASL_MAX_GENSYMS];  /* deserialized symbol objects */
    uint16_t gensym_count;
    /* Shared object dedup table */
    CL_Obj  *shared_objs;      /* heap-allocated, FASL_MAX_SHARED entries */
    uint16_t shared_count;
    /* OBJ_DEF id awaiting registration.  Set by FASL_TAG_OBJ_DEF before
     * recursing into the body; consumed by struct/closure/bytecode body
     * deserializers right after they allocate their shell so cyclic
     * OBJ_REFs inside the body resolve to the partially-constructed
     * object.  0xFFFFFFFF = none pending. */
    uint32_t pending_obj_def_id;
} CL_FaslReader;

/* --- Writer API --- */

void cl_fasl_writer_init(CL_FaslWriter *w, uint8_t *buf, uint32_t capacity);

/* Free heap-allocated dedup tables (shared_objs, shared_hash).  Safe to call
 * multiple times and on a freshly-initialized writer.  Does NOT free the
 * caller-supplied output buffer (`w->data`). */
void cl_fasl_writer_release(CL_FaslWriter *w);

/* Write primitives (big-endian) */
void cl_fasl_write_u8(CL_FaslWriter *w, uint8_t val);
void cl_fasl_write_u16(CL_FaslWriter *w, uint16_t val);
void cl_fasl_write_u32(CL_FaslWriter *w, uint32_t val);
void cl_fasl_write_bytes(CL_FaslWriter *w, const void *data, uint32_t len);

/* Serialize a CL_Obj constant */
void cl_fasl_serialize_obj(CL_FaslWriter *w, CL_Obj obj);

/* Serialize a CL_Bytecode object (the full blob) */
void cl_fasl_serialize_bytecode(CL_FaslWriter *w, CL_Obj bc_obj);
void cl_fasl_reset_serialize_count(void);

/* Write a complete FASL file header */
void cl_fasl_write_header(CL_FaslWriter *w, uint32_t n_units);

/* --- Reader API --- */

void cl_fasl_reader_init(CL_FaslReader *r, const uint8_t *data, uint32_t size);

/* Read primitives (big-endian) */
uint8_t  cl_fasl_read_u8(CL_FaslReader *r);
uint16_t cl_fasl_read_u16(CL_FaslReader *r);
uint32_t cl_fasl_read_u32(CL_FaslReader *r);
void     cl_fasl_read_bytes(CL_FaslReader *r, void *out, uint32_t len);

/* Deserialize a CL_Obj constant */
CL_Obj cl_fasl_deserialize_obj(CL_FaslReader *r);

/* Deserialize a CL_Bytecode object (the full blob) */
CL_Obj cl_fasl_deserialize_bytecode(CL_FaslReader *r);

/* Read and validate a FASL file header; returns n_units or sets r->error */
uint32_t cl_fasl_read_header(CL_FaslReader *r);

/* --- Active-reader registry (GC rooting of dedup tables) ---
 * A reader's gensym_objs[]/shared_objs[] hold CL_Obj references that forward
 * GENSYM_REF/OBJ_REF resolve through; they must be treated as GC roots for the
 * duration of a load so compaction forwards them.  See fasl.c for the full
 * rationale ("Undefined function: PACKAGE" corruption under heap pressure). */
void cl_fasl_reader_register(CL_FaslReader *r);
void cl_fasl_reader_unregister(CL_FaslReader *r);
void cl_fasl_gc_visit_readers(void (*visit)(CL_Obj *));

/* --- MAKE-LOAD-FORM writer pre-pass (see FASL_TAG_LOAD_FORM) ---
 * cl_fasl_mlf_prepass walks the constant graph of the BC_COUNT bytecode
 * units in BC_VEC (a Lisp vector) and records a (creation . init) load
 * form for every literal object whose class has a user MAKE-LOAD-FORM
 * method, so the serializer can emit FASL_TAG_LOAD_FORM for them.  Call
 * it once before serializing a COMPILE-FILE run's units, and
 * cl_fasl_mlf_cleanup() once serialization (or an error path) is done.
 * The GC root walk visits the pre-pass state — the pre-pass calls user
 * code that can compact — and the compactor reports a relocation so the
 * offset-keyed dedup indexes rebuild. */
void cl_fasl_mlf_prepass(CL_Obj bc_vec, int bc_count);
void cl_fasl_mlf_cleanup(void);
/* Non-zero while a compile-file's MLF cache is live (prepass ran, cleanup
 * pending).  Lets LOAD reap a cache leaked by an unwound compile-file. */
int  cl_fasl_mlf_active_p(void);
void cl_fasl_gc_visit_mlf(void (*visit)(CL_Obj *));
void cl_fasl_mlf_gc_relocated(void);
/* Error-unwind snapshot/restore of the active-reader count (cl_error longjmps
 * past cl_fasl_reader_unregister; the error frame restores the count). */
int  cl_fasl_reader_save_count(void);
void cl_fasl_reader_restore_count(int n);
/* NLX-landing counterpart: a Lisp-level non-local exit passes no error frame.
 * Drops (and frees the off-heap table of) every reader of the calling thread
 * that lives deeper in the C stack than LANDING_ANCHOR = CL_CAPTURE_SP() at
 * the landing, i.e. in a frame the longjmp discarded.  Call it wherever
 * cl_compiler_unwind_to is called. */
void cl_fasl_reader_unwind_to(void *landing_anchor);

/* Active-WRITER registry: GC-roots a persistent writer's gensym dedup table
 * across bi_load's interleaved compile/eval/serialize (see fasl.c).
 * Unlike a reader, a registered writer is platform_alloc'd (not a C stack
 * local) — STACK_ANCHOR is CL_CAPTURE_SP() taken at the registering frame
 * (bi_load), kept alongside the writer purely so cl_fasl_writer_unwind_to
 * can tell an abandoned registration from a live one; it is never
 * dereferenced. */
void cl_fasl_writer_register(CL_FaslWriter *w, void *stack_anchor);
void cl_fasl_writer_unregister(CL_FaslWriter *w);
int  cl_fasl_writer_save_count(void);
void cl_fasl_writer_restore_count(int n);
void cl_fasl_gc_visit_writers(void (*visit)(CL_Obj *));
/* The registered writer's per-unit scratch buffer (bi_load's `unit_buf`) is
 * not reachable from the CL_FaslWriter itself, so the registry keeps its own
 * copy to free on abandonment — call this whenever the caller (re)allocates
 * it, including right after cl_fasl_writer_register. A writer with no slot
 * (unregistered, e.g. bi_compile_file's writer) is a silent no-op. */
void cl_fasl_writer_note_scratch(CL_FaslWriter *w, void *unit_buf);
/* NLX-landing counterpart of cl_fasl_reader_unwind_to (see above), for the
 * writer registry: drops every writer of the calling thread registered with
 * a stack_anchor deeper than LANDING_ANCHOR, freeing its scratch buffer,
 * shared/gensym dedup tables, output buffer (w->data) and the writer struct
 * itself — all off-heap and otherwise gone until reboot on AmigaOS. Call it
 * next to cl_fasl_reader_unwind_to. */
void cl_fasl_writer_unwind_to(void *landing_anchor);
/* Process exit: free registry arrays that grew past their static size. */
void cl_fasl_release_registries(void);

#ifdef DEBUG_FASL
/* --- Per-unit serialization histogram (debug builds only) ---
 *
 * Diagnostic instrumentation for investigating FASL bloat.  Bytes written
 * during cl_fasl_serialize_bytecode are attributed to a per-type bucket;
 * dedup hits (OBJ_REF inter-unit, CONST_REF intra-bytecode) are counted
 * separately so bytes-saved-by-dedup is visible.
 *
 * Usage: call cl_fasl_hist_begin() before cl_fasl_serialize_bytecode() and
 * cl_fasl_hist_dump(label) after if the unit overflowed (or unconditionally
 * for blanket profiling).  cl_fasl_hist_end() turns it off. */
void cl_fasl_hist_begin(void);
void cl_fasl_hist_end(void);
void cl_fasl_hist_dump(const char *label, uint32_t bytes_written);
#endif

/* --- High-level API --- */

/* Load a FASL file from memory buffer, executing each unit.
 * Returns CL_T on success, signals error on failure — including, before any
 * unit has run, a DEPS trailer that no longer matches the live structure
 * layouts.  cl_fasl_load_prechecked skips that comparison for a caller that
 * has just done it (LOAD's cache path). */
CL_Obj cl_fasl_load(const uint8_t *data, uint32_t size);
CL_Obj cl_fasl_load_prechecked(const uint8_t *data, uint32_t size);

/* --- DEPS trailer (structure-layout dependencies) ---
 * cl_fasl_deps_size: bytes cl_fasl_write_deps will append for the list of
 * structure names DEPS (0 = nothing to write).  cl_fasl_write_deps appends
 * the trailer after the last unit and sets CL_FASL_FLAG_DEPS in the header
 * already in W's buffer; the caller makes room first.
 * cl_fasl_check_deps: 1 = loadable, 0 = a structure's layout changed (its
 * name goes to DETAIL).  Never interns, allocates in the arena or signals. */
uint32_t cl_fasl_deps_size(CL_Obj deps);
void     cl_fasl_write_deps(CL_FaslWriter *w, CL_Obj deps);
int      cl_fasl_check_deps(const uint8_t *data, uint32_t size,
                            char *detail, uint32_t detail_size);

#endif /* CL_FASL_H */
