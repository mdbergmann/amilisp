#ifndef CL_TYPES_H
#define CL_TYPES_H

/*
 * AmiLisp tagged pointer object representation (32-bit)
 *
 * Bit layout of CL_Obj (uint32_t):
 *   xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx1  = Fixnum (31-bit signed, tag bit 0)
 *   pppppppppppppppppppppppppppppp00  = Heap offset (4-byte aligned arena-relative)
 *   cccccccccccccccccccccccc00001010  = Character (24-bit, tag 0x0A)
 *   00000000000000000000000000000000  = NIL (offset 0)
 *
 * Heap pointers are stored as byte offsets from the arena base.
 * This allows the same 32-bit representation to work on both
 * 32-bit Amiga targets and 64-bit development hosts.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/*
 * Version — the ONE place to bump.
 *
 * Everything else derives from these three numbers, so a release bump touches
 * this block and nothing else.  Two consumers exist, and they need different
 * shapes:
 *
 *   CL_VERSION_STRING       "0.5"    major.minor — keys the FASL cache path
 *                                    (~/.cache/common-lisp/amilisp-<ver>-fasl<n>/),
 *                                    so it changes only on a minor bump and a
 *                                    patch release keeps its warm cache.
 *   CL_VERSION_STRING_FULL  "0.5.0"  major.minor.patch — LISP-IMPLEMENTATION-VERSION.
 *
 * Use CL_VERSION_LEN_FULL rather than a literal length when constructing a
 * Lisp string from it: a hardcoded length silently truncates the moment a
 * component reaches two digits (e.g. "0.10.0" is 6 chars, not 5).
 */
#define CL_VERSION_MAJOR 0
#define CL_VERSION_MINOR 12
#define CL_VERSION_PATCH 0

/* Release date, DD.MM.YYYY — the format AmigaOS's Version command expects. */
#define CL_VERSION_DATE "08.10.2026"

/* Two levels: the inner macro must see the expanded number, not the name. */
#define CL_VERSION_STR_(x) #x
#define CL_VERSION_STR(x)  CL_VERSION_STR_(x)

#define CL_VERSION_STRING \
    CL_VERSION_STR(CL_VERSION_MAJOR) "." CL_VERSION_STR(CL_VERSION_MINOR)

#define CL_VERSION_STRING_FULL \
    CL_VERSION_STRING "." CL_VERSION_STR(CL_VERSION_PATCH)

/* Compile-time length of CL_VERSION_STRING_FULL (excludes the NUL). */
#define CL_VERSION_LEN_FULL ((uint32_t)(sizeof(CL_VERSION_STRING_FULL) - 1))

/*
 * AmigaOS version cookie.  `Version amilisp` in a Shell scans the executable
 * for a "$VER: " marker and prints the rest of the line, so the layout is
 * fixed by the OS: "$VER: <name> <version>.<revision> (<DD.MM.YYYY>)".
 *
 * Defined as cl_version_cookie in types.c.  Nothing references it (the OS
 * reads raw bytes), so it must be marked CL_USED or -flto drops it.
 */
#define CL_VERSION_TAG \
    "$VER: amilisp " CL_VERSION_STRING " (" CL_VERSION_DATE ")"

/* Force emission of an otherwise-unreferenced definition (GNU extension; the
   m68k-amigaos and host toolchains are both GCC-compatible). */
#if defined(__GNUC__) || defined(__clang__)
#define CL_USED __attribute__((used))
#else
#define CL_USED
#endif

extern const char cl_version_cookie[];

/*
 * setjmp() buffer-overrun guard (MorphOS PPC).
 *
 * On MorphOS PPC with -noixemul, the C library's setjmp() writes a register
 * save area that is LARGER than the toolchain's own `jmp_buf` typedef reports
 * (observed sizeof(jmp_buf)==444, but the real save area also spills the
 * AltiVec vector registers v20-v31 = 192 bytes the header does not count).
 * The extra bytes overrun whatever struct fields follow an embedded jmp_buf —
 * this silently zeroed CL_NLXFrame.vm_sp/vm_fp/tag, so tagbody/GO could never
 * find its frame and every CL_CATCH/error frame was corrupted the same way.
 *
 * Fix: reserve dead space immediately AFTER any embedded jmp_buf so the
 * overrun lands there instead of in live fields.  512 bytes comfortably
 * covers the AltiVec area plus alignment slop; cl_setjmp_overrun_check()
 * (called at startup) measures the true overrun and aborts if this guard is
 * ever too small.  On every other target the overrun does not occur and this
 * expands to nothing, so there is zero cost off MorphOS.
 */
#define CL_JMPBUF_GUARD_BYTES 512
#if defined(PLATFORM_MORPHOS)
#  define CL_JMPBUF_GUARD  char _cl_jmpbuf_guard[CL_JMPBUF_GUARD_BYTES];
/* Bytes actually reserved after an embedded jmp_buf on this target — what
 * cl_setjmp_overrun_check() must compare the measured overrun against.
 * CL_JMPBUF_GUARD_BYTES alone is the wrong comparison off MorphOS: the
 * CL_JMPBUF_GUARD field above expands to nothing there, so the true
 * reserved space is 0, not 512. */
#  define CL_JMPBUF_GUARD_RESERVED_BYTES CL_JMPBUF_GUARD_BYTES
#else
#  define CL_JMPBUF_GUARD  /* no setjmp overrun on this target */
#  define CL_JMPBUF_GUARD_RESERVED_BYTES 0
#endif

typedef uint32_t CL_Obj;

/* Arena base pointer — needed for offset↔pointer conversion */
extern uint8_t *cl_arena_base;

/* NIL and T */
#define CL_NIL  ((CL_Obj)0)
extern CL_Obj CL_T;  /* Pre-allocated symbol, set during init */

/* Tag masks */
#define CL_TAG_FIXNUM   0x01
#define CL_TAG_CHAR     0x0A
#define CL_TAG_MASK_LO2 0x03

/* --- Fixnum (31-bit signed integer, tag bit 0) --- */

#define CL_FIXNUM_P(obj)    ((obj) & CL_TAG_FIXNUM)
#define CL_MAKE_FIXNUM(n)   ((CL_Obj)(((uint32_t)(int32_t)(n) << 1) | CL_TAG_FIXNUM))
#define CL_FIXNUM_VAL(obj)  ((int32_t)(obj) >> 1)

#define CL_FIXNUM_MAX  ((int32_t)0x3FFFFFFF)   /*  1073741823 */
#define CL_FIXNUM_MIN  ((int32_t)0xC0000000)   /* -1073741824 */

/* --- Character (24-bit value, tag byte 0x0A) --- */

#define CL_CHAR_P(obj)      (((obj) & 0xFF) == CL_TAG_CHAR)
#define CL_MAKE_CHAR(c)     ((CL_Obj)(((uint32_t)(c) << 8) | CL_TAG_CHAR))
#define CL_CHAR_VAL(obj)    ((int)((obj) >> 8))

/* --- Heap pointer (low 2 bits = 00, arena-relative offset) --- */

#define CL_HEAP_P(obj)      ((obj) != CL_NIL && ((obj) & CL_TAG_MASK_LO2) == 0)
#define CL_OBJ_TO_PTR(obj)  ((void *)(cl_arena_base + (obj)))
#define CL_PTR_TO_OBJ(ptr)  ((CL_Obj)((uint8_t *)(ptr) - cl_arena_base))

/* --- NIL checks --- */

#define CL_NULL_P(obj)      ((obj) == CL_NIL)

/* --- Heap object header --- */

/*
 * Header word layout: [type:8][gc_mark:1][size:23]
 * size = total allocation size in bytes (including header)
 */

#define CL_HDR_TYPE_SHIFT   24
#define CL_HDR_MARK_BIT     (1u << 23)
#define CL_HDR_SIZE_MASK    0x007FFFFFu

typedef struct {
    uint32_t header;
} CL_Header;

/* Heap object type tags */
enum CL_ObjType {
    TYPE_CONS = 0,
    TYPE_SYMBOL,
    TYPE_STRING,
    TYPE_FUNCTION,
    TYPE_CLOSURE,
    TYPE_BYTECODE,
    TYPE_VECTOR,
    TYPE_PACKAGE,
    TYPE_HASHTABLE,
    TYPE_CONDITION,
    TYPE_STRUCT,
    TYPE_BIGNUM,
    TYPE_SINGLE_FLOAT,
    TYPE_DOUBLE_FLOAT,
    TYPE_RATIO,
    TYPE_COMPLEX,
    TYPE_STREAM,
    TYPE_RANDOM_STATE,
    TYPE_BIT_VECTOR,
    TYPE_PATHNAME,
    TYPE_CELL,
    TYPE_THREAD,
    TYPE_LOCK,
    TYPE_CONDVAR,
    TYPE_FOREIGN_POINTER,
    TYPE_RESTART,
    TYPE_BYTE_VECTOR,
    TYPE_FFI_STUB
#ifdef CL_WIDE_STRINGS
    , TYPE_WIDE_STRING
#endif
};

/* Highest valid heap type tag.  Kept HERE, directly under the enum, so that
 * adding a member forces this definition into the same diff.  The GC debug
 * guards (GC_DBG_MAX_TYPE in mem.c) abort on any tag above it as heap
 * corruption — a stale max turns every valid object of a newly added type
 * into a false [GC-BADMARK] abort on DEBUG_GC/DEBUG_GC_STRESS builds
 * (regression: TYPE_BYTE_VECTOR was added without bumping the old copy in
 * mem.c; every byte-vector mark aborted the non-wide MorphOS stress build). */
#ifdef CL_WIDE_STRINGS
#define CL_TYPE_MAX TYPE_WIDE_STRING
#else
#define CL_TYPE_MAX TYPE_FFI_STUB
#endif

/* A header tag for a block that is NOT an object and never will be read
 * as one: a hole the Immix backend formatted for its allocation cursor, a
 * finalized corpse it neutralised, a sub-header gap the compactor could
 * not link.  Outside the enum (and above CL_TYPE_MAX) on purpose: a
 * dead block reached through a precise reference is heap corruption, and
 * the DEBUG_GC plausibility guards keep aborting on it; the arena walk,
 * the conservative JIT-stack scan and the finalizers know the tag and
 * step over it.  The classic backend's free blocks keep their raw-size
 * (type 0) header; see specs/gc-backend-immix.md. */
#define CL_TYPE_DEAD 0xFFu

/* Header access macros */
#define CL_HDR(ptr)         (((CL_Header *)(ptr))->header)
#define CL_HDR_TYPE(ptr)    ((CL_HDR(ptr) >> CL_HDR_TYPE_SHIFT) & 0xFF)
#define CL_HDR_SIZE(ptr)    (CL_HDR(ptr) & CL_HDR_SIZE_MASK)
#define CL_HDR_MARKED(ptr)  (CL_HDR(ptr) & CL_HDR_MARK_BIT)

#define CL_HDR_SET_MARK(ptr)   (CL_HDR(ptr) |= CL_HDR_MARK_BIT)
#define CL_HDR_CLR_MARK(ptr)   (CL_HDR(ptr) &= ~CL_HDR_MARK_BIT)

#define CL_MAKE_HDR(type, size) \
    (((uint32_t)(type) << CL_HDR_TYPE_SHIFT) | ((uint32_t)(size) & CL_HDR_SIZE_MASK))

/* Array size/rank bounds used by MAKE-ARRAY's dimension checks.  These must
 * track the Lisp-visible ARRAY-DIMENSION-LIMIT / ARRAY-TOTAL-SIZE-LIMIT
 * constants registered in bignum.c (def_const calls) — they are the
 * documented boundary a conforming program can query via
 * (array-dimension-limit) / (array-total-size-limit), so the guard and the
 * constant must agree.  CL_ARRAY_MAX_RANK mirrors the fixed-size `dims[16]`
 * stack buffers used across builtins_array.c/printer.c/fasl.c — it is an
 * implementation restriction, deliberately not tied to ARRAY-RANK-LIMIT
 * (256 in bignum.c), since CLHS permits make-array to impose additional
 * limits below array-rank-limit as long as it signals a proper error. */
#define CL_ARRAY_DIMENSION_LIMIT   (1u << 20)
#define CL_ARRAY_TOTAL_SIZE_LIMIT  (1u << 20)
#define CL_ARRAY_MAX_RANK          16u

/* --- Cons cell --- */

typedef struct {
    CL_Header hdr;
    CL_Obj car;
    CL_Obj cdr;
} CL_Cons;

#define CL_CONS_P(obj)  (CL_HEAP_P(obj) && CL_HDR_TYPE(CL_OBJ_TO_PTR(obj)) == TYPE_CONS)

/* --- Symbol --- */

typedef struct {
    CL_Header hdr;
    CL_Obj name;      /* CL_String pointer */
    CL_Obj value;     /* Global value (unbound marker if unset) */
    CL_Obj function;  /* Function binding */
    CL_Obj plist;     /* Property list */
    CL_Obj package;   /* Home package */
    uint32_t hash;    /* Cached name hash */
    uint32_t flags;   /* bit 0: IS_SPECIAL (declared by defvar/defparameter) */
} CL_Symbol;

/* Bits 24..31 of CL_Symbol.flags hold this symbol's standard type-specifier
 * code (0 = not a standard type name), set once at startup by
 * cl_typep_codes_init in builtins_type.c.  TYPEP on a symbol specifier is
 * then one load and a switch instead of a ~45-deep strcmp cascade — it runs
 * on every OP_ASSERT_TYPE, i.e. on every (declare (type ...)) in compiled
 * code.  Nothing else writes the upper byte: flags are only ever OR'd or
 * cleared bit-wise after cl_make_symbol zeroes them. */
#define CL_SYM_TYPECODE_SHIFT 24
#define CL_SYM_TYPECODE_MASK  0xFF000000u
#define CL_SYM_TYPECODE(s)    (((s)->flags & CL_SYM_TYPECODE_MASK) >> \
                               CL_SYM_TYPECODE_SHIFT)

#define CL_SYM_SPECIAL  0x01
#define CL_SYM_INLINE   0x02
#define CL_SYM_TRACED   0x04
#define CL_SYM_EXPORTED 0x08  /* hint: SOME package exports this symbol */
#define CL_SYM_CONSTANT 0x10
#define CL_SYM_LISTED   0x20  /* transient: bulk-export pass dedup marker */
/* Exact: the symbol is on its HOME package's exported_symbols list.  Lets
 * the reader's pkg:sym lookup and use-list inheritance answer "is this
 * symbol external in its home package?" in O(1) instead of walking the
 * home's export list (COMMON-LISP: ~980 cells, AMIGA.RAW.INTUITION:
 * ~1950 — per qualified read).  Maintained by package.c alongside the
 * list (export / unexport / bulk export) and cleared wherever a symbol
 * loses or changes its home (unintern, delete-package, add-symbol).
 * A symbol exported from a package that is NOT its home still takes the
 * list walk (CL_SYM_EXPORTED set, home differs) — correct, just slower. */
#define CL_SYM_EXPORTED_HOME 0x40

#define CL_SYMBOL_P(obj) (CL_HEAP_P(obj) && CL_HDR_TYPE(CL_OBJ_TO_PTR(obj)) == TYPE_SYMBOL)

/* (typep x 'symbol) — NIL is a symbol per CLHS, but its tagged
 * representation (0) means CL_SYMBOL_P alone misses it.  Use this in
 * argument checks for any builtin that operates on symbols (BOUNDP,
 * SYMBOL-VALUE, SYMBOL-FUNCTION, MAKUNBOUND, ...) so '(boundp nil) etc.
 * conform to ANSI without each call site spelling out the OR. */
#define CL_SYMBOL_OR_NIL_P(obj) (CL_NULL_P(obj) || CL_SYMBOL_P(obj))

#define CL_UNBOUND  ((CL_Obj)0xFFFFFFF6)  /* Sentinel for unbound variables (low 2 bits=10, not a valid CL_Obj) */

/* --- String --- */

typedef struct {
    CL_Header hdr;
    uint32_t length;   /* Character count */
    char data[];       /* Flexible array member (C99) */
} CL_String;

#define CL_STRING_P(obj) (CL_HEAP_P(obj) && CL_HDR_TYPE(CL_OBJ_TO_PTR(obj)) == TYPE_STRING)

#ifdef CL_WIDE_STRINGS
/* --- Wide String (UTF-32, 4 bytes per character) --- */

typedef struct {
    CL_Header hdr;
    uint32_t length;       /* Character count */
    uint32_t data[];       /* Code points (flexible array) */
} CL_WideString;

#define CL_WIDE_STRING_P(obj) \
    (CL_HEAP_P(obj) && CL_HDR_TYPE(CL_OBJ_TO_PTR(obj)) == TYPE_WIDE_STRING)

/* True for any string type (base or wide) */
#define CL_ANY_STRING_P(obj) (CL_STRING_P(obj) || CL_WIDE_STRING_P(obj))

#else
/* No wide string support — CL_ANY_STRING_P is just CL_STRING_P */
#define CL_ANY_STRING_P(obj)  CL_STRING_P(obj)
#endif /* CL_WIDE_STRINGS */

/* --- Function (built-in C function) --- */

typedef CL_Obj (*CL_CFunc)(CL_Obj *args, int nargs);

typedef struct {
    CL_Header hdr;
    CL_CFunc func;
    CL_Obj name;       /* Symbol */
    int min_args;
    int max_args;       /* -1 for variadic */
} CL_Function;

#define CL_FUNCTION_P(obj) (CL_HEAP_P(obj) && CL_HDR_TYPE(CL_OBJ_TO_PTR(obj)) == TYPE_FUNCTION)

/* --- Bytecode (compiled function template) --- */

/* Source line map entry: maps bytecode offset to source line number */
typedef struct {
    uint16_t pc;        /* Bytecode offset */
    uint16_t line;      /* Source line number */
} CL_LineEntry;

typedef struct {
    CL_Header hdr;
    uint8_t *code;      /* Bytecode array */
    CL_Obj *constants;  /* Constants pool */
    uint32_t code_len;
    uint16_t n_constants;
    uint16_t arity;     /* bits 0-14: required params, bit 15: has_rest */
    uint16_t n_locals;
    uint16_t n_upvalues;
    CL_Obj name;        /* Symbol or NIL */
    uint8_t n_optional; /* Number of &optional params */
    uint8_t flags;      /* bit 0: has_key, bit 1: allow_other_keys */
    uint8_t n_keys;     /* Number of &key params */
    /* Lazy-JIT state, CL_BC_JIT_* below (m68k JIT: jit_m68k.c cl_jit_note_call).
     * Occupies what was padding -- sizeof(CL_Bytecode) is unchanged on
     * every target, which image.c's layout hash depends on.  Only the
     * CL_BC_JIT_SPEED hint is FASL-serialized (bit 7 of the flags byte on
     * the wire); the call count starts over after a load or an image
     * restore. */
    uint8_t jit_hot;
    CL_Obj *key_syms;   /* Keyword symbols array (platform_alloc'd) */
    uint8_t *key_slots; /* Slot indices for each key param (platform_alloc'd) */
    uint8_t *key_suppliedp_slots; /* Slot indices for supplied-p vars (or 0xFF=none) */
    /* Source location tracking (platform_alloc'd) */
    CL_LineEntry *line_map;  /* Array of pc→line entries, sorted by pc */
    uint16_t line_map_count; /* Number of entries */
    uint16_t source_line;    /* Line where function was defined */
    const char *source_file; /* Filename (NULL for REPL) */
    /* Native code from the m68k template JIT.  NULL on host builds,
     * on cross builds where the JIT chose not to compile this
     * function, and before the JIT has been invoked.  See
     * specs/native-backend.md. */
    uint8_t *native_code;
    uint32_t native_len;
    /* Relocation table for CL_Obj immediates baked into native_code.
     * Each entry is a byte offset into native_code at which a 4-byte
     * big-endian CL_Obj *heap* reference (symbol, tag, template, …) was
     * emitted as a literal operand.  The moving compactor forwards each
     * one in place (mem.c gc_update_children, TYPE_BYTECODE) so that
     * relocating an object does not leave a stale arena offset baked
     * into the generated code.  platform_alloc'd, parallel to
     * native_code; NULL when the function baked no heap immediates.
     * Not FASL-serialized — rebuilt by cl_jit_compile on load, exactly
     * like native_code.  See specs/native-backend.md "GC interaction".
     * The AArch64 backend bakes no heap object, so it keeps its direct-call
     * cells here instead (uint64_t per call site, native_reloc_count 0):
     * freed with the code by the same paths. */
    uint32_t *native_relocs;
    uint16_t native_reloc_count;
    /* Original source lambda-list (the params form as written, including
     * &optional/&key default forms).  CL_NIL when not captured (top-level
     * forms, builtins).  Retained for arglist / Sly introspection; see
     * EXT:FUNCTION-ARGLIST.  GC-traced and FASL-serialized (format v9+). */
    CL_Obj source_lambda_list;
} CL_Bytecode;

#define CL_BYTECODE_P(obj) (CL_HEAP_P(obj) && CL_HDR_TYPE(CL_OBJ_TO_PTR(obj)) == TYPE_BYTECODE)

/* CL_Bytecode.jit_hot.  Bits 0-6 count the calls that ran interpreted;
 * the JIT compiles the function when the count reaches the hot threshold
 * (or on its first call when it contains a loop).  CL_BC_JIT_SETTLED
 * there means "stop counting": compiled, rejected by the JIT, or defined
 * while the JIT was off.  Bit 7 records (optimize (speed 3)) at compile
 * time, which compiles the function at definition instead. */
#define CL_BC_JIT_COUNT_MASK  0x7Fu
#define CL_BC_JIT_SETTLED     0x7Fu
#define CL_BC_JIT_SPEED       0x80u
#define CL_BC_JIT_COUNTING_P(bc) \
    ((((bc)->jit_hot) & CL_BC_JIT_COUNT_MASK) != CL_BC_JIT_SETTLED)

/* --- Closure (bytecode + captured upvalues) --- */

typedef struct {
    CL_Header hdr;
    CL_Obj bytecode;    /* CL_Bytecode pointer */
    CL_Obj upvalues[];  /* Captured variable values */
} CL_Closure;

#define CL_CLOSURE_P(obj) (CL_HEAP_P(obj) && CL_HDR_TYPE(CL_OBJ_TO_PTR(obj)) == TYPE_CLOSURE)

/* --- Vector / Array --- */

/* Vector flags */
#define CL_VEC_FLAG_FILL_POINTER  0x01
#define CL_VEC_FLAG_ADJUSTABLE    0x02
#define CL_VEC_FLAG_MULTIDIM      0x04
#define CL_VEC_FLAG_DISPLACED     0x08  /* data[0] = CL_Obj ref to backing vector */
#define CL_VEC_FLAG_STRING        0x10  /* character vector (elements are CL_MAKE_CHAR) */

/* Sentinel: no fill pointer */
#define CL_NO_FILL_POINTER  0xFFFFFFFFu

/* General-vector element-type codes (elt_type field).  CHARACTER and BIT
 * element types are tracked via CL_VEC_FLAG_STRING / a separate bit-vector
 * object, so they are not represented here.  amilisp stores all numeric
 * elements as tagged CL_Obj regardless of code; the code only records the
 * declared element type so ARRAY-ELEMENT-TYPE and (TYPEP x '(VECTOR fixnum))
 * can distinguish a specialized numeric array from a general (VECTOR T) one
 * (serapeum's VECT-TYPE).  Runtime-only: not preserved across FASL (which
 * reloads vectors as plain T vectors), matching the existing handling of the
 * fill-pointer/adjustable flags. */
#define CL_VEC_ELT_T              0  /* general / unspecified — element type T */
#define CL_VEC_ELT_FIXNUM         1
#define CL_VEC_ELT_SINGLE_FLOAT   2
#define CL_VEC_ELT_DOUBLE_FLOAT   3
/* Annotation codes for ADJUSTABLE (unsigned-byte 8/16)/(signed-byte 8/16)
 * arrays.  Non-adjustable byte arrays use the packed TYPE_BYTE_VECTOR
 * representation; adjustable ones must grow in place via the displacement
 * machinery, which only general CL_Vector storage supports — they store
 * tagged elements but keep reporting the byte element type through these
 * codes. */
#define CL_VEC_ELT_U8             4
#define CL_VEC_ELT_S8             5
#define CL_VEC_ELT_U16            6
#define CL_VEC_ELT_S16            7

typedef struct {
    CL_Header hdr;
    uint32_t length;          /* Total element count */
    uint32_t fill_pointer;    /* CL_NO_FILL_POINTER = none */
    uint8_t  flags;           /* CL_VEC_FLAG_* */
    uint8_t  rank;            /* 0 = simple 1D, >1 = multi-dim */
    uint8_t  elt_type;        /* CL_VEC_ELT_* (general-vector declared element type) */
    uint8_t  _reserved;       /* Alignment padding */
    CL_Obj data[];            /* For rank>1: dims in data[0..rank-1], elements at data[rank] */
} CL_Vector;

#define CL_VECTOR_P(obj) (CL_HEAP_P(obj) && CL_HDR_TYPE(CL_OBJ_TO_PTR(obj)) == TYPE_VECTOR)

/* String-like vector: CL_Vector with CL_VEC_FLAG_STRING (adjustable/fill-pointer character arrays) */
#define CL_STRING_VECTOR_P(obj) \
    (CL_VECTOR_P(obj) && (((CL_Vector *)CL_OBJ_TO_PTR(obj))->flags & CL_VEC_FLAG_STRING))

/* For a displaced vector, the backing ref and offset live just past any
 * dimension storage: data[rank]/data[rank+1] for a multi-dim displaced array
 * (rank>1, dims occupy data[0..rank-1]), else data[0]/data[1] for a 1-D
 * displaced vector (rank<=1, no dim storage). */
#define CL_DISP_BASE_IDX(v)  ((v)->rank > 1 ? (uint32_t)(v)->rank : 0u)

/* Access helpers: data pointer (skips dimension storage for multi-dim) */
/* For displaced vectors, follow the backing ref to the backing vector */
CL_Obj *cl_vector_data_fn(CL_Vector *v);
#define cl_vector_data(v)  \
    ((v)->flags & CL_VEC_FLAG_DISPLACED \
     ? cl_vector_data_fn(v) \
     : ((v)->rank > 1 ? &(v)->data[(v)->rank] : (v)->data))

/* Active length: fill pointer if present, else total length */
#define cl_vector_active_length(v) \
    ((v)->fill_pointer != CL_NO_FILL_POINTER ? (v)->fill_pointer : (v)->length)

/* --- Package --- */

typedef struct {
    CL_Header hdr;
    CL_Obj name;        /* CL_String */
    CL_Obj symbols;     /* Hash table (vector of lists) */
    CL_Obj use_list;    /* List of used packages */
    CL_Obj nicknames;   /* List of nickname strings */
    CL_Obj local_nicknames; /* CDR-10: alist ((nick-string . package) ...) */
    CL_Obj shadowing_symbols; /* List of shadowing symbols */
    CL_Obj exported_symbols;  /* List of symbols this package exports.
                               * Per-package because the symbol's home pkg
                               * may have it exported while a using/importing
                               * package does not (or vice versa). */
    CL_Obj bindings;    /* NIL, or the package's demand-interned binding
                         * table (bindtab.c): a vector #(blob base-var
                         * version-var) whose blob is a packed byte vector
                         * of (name, definition) entries.  A name that is
                         * not present in `symbols` but is in the blob is
                         * MATERIALISED — symbol, value/FFI stub, export —
                         * the first time any lookup asks for it, so a
                         * generated OS binding module costs O(names used)
                         * instead of O(names defined).  Enumeration and
                         * mutation of the symbol set flip the package
                         * eager (materialise all, clear this).  See
                         * specs/raw-bindings-footprint.md, Phase 2. */
    uint32_t sym_count;
} CL_Package;

#define CL_PACKAGE_P(obj) (CL_HEAP_P(obj) && CL_HDR_TYPE(CL_OBJ_TO_PTR(obj)) == TYPE_PACKAGE)

/* --- Hash Table --- */

/* Test function enum: stored in the test field */
#define CL_HT_TEST_EQ     0
#define CL_HT_TEST_EQL    1
#define CL_HT_TEST_EQUAL  2
#define CL_HT_TEST_EQUALP 3

typedef struct {
    CL_Header hdr;
    uint32_t test;          /* CL_HT_TEST_EQ/EQL/EQUAL */
    uint32_t count;         /* Number of entries */
    uint32_t bucket_count;  /* Number of buckets (always power of 2) */
    uint32_t flags;         /* CL_HT_FLAG_* bits (plain uint32, NOT a CL_Obj) */
    CL_Obj bucket_vec;      /* CL_NIL = use inline buckets[], else CL_Vector */
    CL_Obj buckets[];       /* Flexible array: initial buckets (used when bucket_vec==NIL) */
} CL_Hashtable;

/* Hard cap on bucket counts.  The bucket vector must fit the 23-bit
 * allocation-size header (2^20 buckets * 4 bytes = 4MB); a larger count
 * would overflow the uint32 size computations (a bucket_count of 2^31
 * once wrapped the allocation to ~24 bytes while gethash indexed 2^31
 * "buckets" — wild heap reads/writes).  Tables at the cap keep working
 * with longer chains; growth beyond it is skipped, never wrapped. */
#define CL_HT_MAX_BUCKETS (1u << 20)

/* Hash-table flags (CL_Hashtable.flags).  Runtime-only — never serialized, so
 * adding one needs no FASL version bump. */
#define CL_HT_FLAG_SYNC 0x1u   /* Thread-safe table: concurrent GETHASH / (SETF
                                * GETHASH) / REMHASH / CLRHASH are serialized so
                                * peer threads cannot corrupt the bucket chains.
                                * Used by the otherwise lock-free CLOS dispatch
                                * caches, which are read and written from
                                * multiple threads (main loader + worker/watcher
                                * threads dispatching the same GF).
                                *
                                * NOT covered: MAPHASH, %HASH-TABLE-PAIRS, and
                                * the raw bucket walks behind DESCRIBE/INSPECT.
                                * These walk every bucket chain without the
                                * lock, so a concurrent (SETF GETHASH)/REMHASH
                                * on the same table can be observed mid-splice
                                * or mid-rehash. This is an accepted gap, not
                                * an oversight: MAPHASH calls arbitrary user
                                * Lisp per entry, and holding ht_sync_mutex
                                * across a callback (which can itself allocate
                                * and park at a GC safepoint, or re-enter the
                                * same table) would violate the allocation-free
                                * critical-section invariant documented in
                                * builtins_hashtable.c and risk deadlock. Do
                                * not concurrently MAPHASH / %HASH-TABLE-PAIRS
                                * / DESCRIBE / INSPECT a CL_HT_FLAG_SYNC table
                                * while a peer thread may be mutating it. */

#define CL_HASHTABLE_P(obj) (CL_HEAP_P(obj) && CL_HDR_TYPE(CL_OBJ_TO_PTR(obj)) == TYPE_HASHTABLE)

/* --- Condition --- */

typedef struct {
    CL_Header hdr;
    CL_Obj type_name;      /* Symbol: SIMPLE-ERROR, TYPE-ERROR, etc. */
    CL_Obj slots;          /* Alist: ((slot-name . value) ...) */
    CL_Obj report_string;  /* Pre-formatted message string, or NIL */
} CL_Condition;

#define CL_CONDITION_P(obj) (CL_HEAP_P(obj) && CL_HDR_TYPE(CL_OBJ_TO_PTR(obj)) == TYPE_CONDITION)

/* --- Restart (first-class restart object, CLHS 9.1) --- */

typedef struct {
    CL_Header hdr;
    CL_Obj name;          /* Restart name symbol, or NIL for an anonymous restart */
    CL_Obj function;      /* Closure invoked when the restart is taken */
    CL_Obj report;        /* :report — report string, report function, or NIL */
    CL_Obj interactive;   /* :interactive — function returning an arg list, or NIL */
    CL_Obj test;          /* :test — applicability predicate, or NIL */
    CL_Obj tag;           /* Internal catch tag for transfer of control */
} CL_Restart;

#define CL_RESTART_P(obj) (CL_HEAP_P(obj) && CL_HDR_TYPE(CL_OBJ_TO_PTR(obj)) == TYPE_RESTART)

/* --- Structure --- */

typedef struct {
    CL_Header hdr;
    CL_Obj type_desc;      /* Symbol: struct type name */
    uint32_t n_slots;      /* Number of slots */
    CL_Obj slots[];        /* Positional slot values */
} CL_Struct;

#define CL_STRUCT_P(obj) (CL_HEAP_P(obj) && CL_HDR_TYPE(CL_OBJ_TO_PTR(obj)) == TYPE_STRUCT)

/* --- Bignum (arbitrary-precision integer) --- */

typedef struct {
    CL_Header hdr;
    uint32_t length;    /* Number of 16-bit limbs */
    uint32_t sign;      /* 0 = positive, 1 = negative */
    uint16_t limbs[];   /* Flexible array: little-endian (limbs[0] = least significant) */
} CL_Bignum;

#define CL_BIGNUM_P(obj) (CL_HEAP_P(obj) && CL_HDR_TYPE(CL_OBJ_TO_PTR(obj)) == TYPE_BIGNUM)
#define CL_INTEGER_P(obj) (CL_FIXNUM_P(obj) || CL_BIGNUM_P(obj))

/* --- Ratio (exact fraction p/q, always in lowest terms, q > 0) --- */

typedef struct {
    CL_Header hdr;          /* 4 bytes */
    CL_Obj numerator;       /* Integer (fixnum or bignum) */
    CL_Obj denominator;     /* Positive integer (fixnum or bignum), never 0 or 1 in normalized form */
} CL_Ratio;                 /* 12 bytes total */

#define CL_RATIO_P(obj) (CL_HEAP_P(obj) && CL_HDR_TYPE(CL_OBJ_TO_PTR(obj)) == TYPE_RATIO)
#define CL_RATIONAL_P(obj) (CL_INTEGER_P(obj) || CL_RATIO_P(obj))

/* --- Complex number (a + bi) --- */

typedef struct {
    CL_Header hdr;          /* 4 bytes */
    CL_Obj realpart;        /* Real part (any real number) */
    CL_Obj imagpart;        /* Imaginary part (any real number) */
} CL_Complex;               /* 12 bytes total */

#define CL_COMPLEX_P(obj) (CL_HEAP_P(obj) && CL_HDR_TYPE(CL_OBJ_TO_PTR(obj)) == TYPE_COMPLEX)

/* --- Stream --- */

/* Stream direction */
#define CL_STREAM_INPUT   1
#define CL_STREAM_OUTPUT  2
#define CL_STREAM_IO      3

/* Stream type */
#define CL_STREAM_CONSOLE 0
#define CL_STREAM_FILE    1
#define CL_STREAM_STRING  2
#define CL_STREAM_CBUF    3  /* C buffer input (outside GC arena) */
#define CL_STREAM_SYNONYM  4  /* Synonym stream: delegates to symbol's value */
#define CL_STREAM_SOCKET   5  /* TCP socket stream (bidirectional, binary) */
#define CL_STREAM_TWO_WAY  6  /* Two-way: input=string_buf child, output=element_type child */
#define CL_STREAM_BROADCAST     7  /* Broadcast: string_buf = list of output streams; writes fan out */
#define CL_STREAM_CONCATENATED  8  /* Concatenated: string_buf = list of remaining input streams */
#define CL_STREAM_ECHO          9  /* Echo: input=string_buf child, output=element_type child;
                                    * every element read is echoed to the output child */

/* Stream flags */
#define CL_STREAM_FLAG_OPEN  0x01
#define CL_STREAM_FLAG_EOF   0x02
/* External format is LATIN-1 (ISO-8859-1): characters map 1:1 to bytes 0..255,
 * with no UTF-8 transcoding on read/write.  Default (flag clear) is UTF-8.
 * Set by OPEN :external-format :latin-1; only meaningful for CL_WIDE_STRINGS
 * builds (non-wide builds are already raw 8-bit). */
#define CL_STREAM_FLAG_LATIN1 0x04
/* UDP datagram socket stream (stream_type CL_STREAM_SOCKET): I/O is
 * message-oriented via EXT:UDP-STREAM-SEND/-RECEIVE; the byte-stream
 * read/write paths reject it. */
#define CL_STREAM_FLAG_DGRAM  0x08

typedef struct {
    CL_Header hdr;
    uint32_t direction;      /* INPUT=1, OUTPUT=2, IO=3 */
    uint32_t stream_type;    /* CONSOLE=0, FILE=1, STRING=2 */
    uint32_t flags;          /* bit 0: open, bit 1: eof */
    uint32_t handle_id;      /* Index into platform file handle side table */
    CL_Obj   string_buf;     /* Source string for input streams, NIL for output */
    uint32_t position;       /* Read/write cursor */
    uint32_t out_buf_handle; /* Index into side table for growable C buffer */
    uint32_t out_buf_size;   /* Output buffer capacity */
    uint32_t out_buf_len;    /* Output buffer used length */
    int32_t  unread_char;    /* -1 if none, else pushed-back char */
    CL_Obj   element_type;   /* Symbol (CHARACTER) */
    uint32_t charpos;        /* Column position: 0 = at BOL, 0xFFFFFFFF = unknown */
    uint32_t line;           /* 1-based line number of next char to read */
    uint32_t read_timeout_ms;  /* Socket read timeout in ms; 0 = none (runtime-only, not in FASL) */
    uint32_t write_timeout_ms; /* Socket write timeout in ms; 0 = none (runtime-only, not in FASL) */
} CL_Stream;

#define CL_STREAM_P(obj) (CL_HEAP_P(obj) && CL_HDR_TYPE(CL_OBJ_TO_PTR(obj)) == TYPE_STREAM)

/* --- Random State (xorshift128) --- */

typedef struct {
    CL_Header hdr;
    uint32_t s[4];  /* xorshift128 state */
} CL_RandomState;

#define CL_RANDOM_STATE_P(obj) (CL_HEAP_P(obj) && CL_HDR_TYPE(CL_OBJ_TO_PTR(obj)) == TYPE_RANDOM_STATE)

/* --- Bit Vector (packed bit array) --- */

typedef struct {
    CL_Header hdr;
    uint32_t length;          /* Number of bits */
    uint32_t fill_pointer;    /* CL_NO_FILL_POINTER = none */
    uint8_t  flags;           /* CL_VEC_FLAG_FILL_POINTER | CL_VEC_FLAG_ADJUSTABLE */
    uint8_t  _pad[3];
    uint32_t data[];          /* ceil(length/32) packed words, LSB-first */
} CL_BitVector;

#define CL_BIT_VECTOR_P(obj) (CL_HEAP_P(obj) && CL_HDR_TYPE(CL_OBJ_TO_PTR(obj)) == TYPE_BIT_VECTOR)

/* Number of uint32_t words needed for n bits */
#define CL_BV_WORDS(n)  (((n) + 31) / 32)

/* Get bit i: LSB-first within each word */
#define cl_bv_get_bit(bv, i) \
    (((bv)->data[(i) / 32] >> ((i) % 32)) & 1u)

/* Set bit i to val (0 or 1) */
#define cl_bv_set_bit(bv, i, val) \
    do { \
        if (val) (bv)->data[(i) / 32] |= (1u << ((i) % 32)); \
        else     (bv)->data[(i) / 32] &= ~(1u << ((i) % 32)); \
    } while (0)

/* Active length: fill pointer if present, else total length */
#define cl_bv_active_length(bv) \
    ((bv)->fill_pointer != CL_NO_FILL_POINTER ? (bv)->fill_pointer : (bv)->length)

/* --- Byte Vector (packed (unsigned-byte 8/16) / (signed-byte 8/16) array) ---
 *
 * True packed storage — 1 byte per element for the 8-bit kinds, 2 bytes for
 * the 16-bit kinds — unlike the general CL_Vector which holds every element
 * as a 4-byte tagged CL_Obj regardless of declared element type.  A GC *leaf*
 * object (no CL_Obj children — raw numeric data), so the marker never scans
 * its contents; on an 8MB 68020 that makes large I/O, audio-sample and
 * graphics plane buffers 4x (or 2x) smaller and free to collect.
 *
 * elt_shift is log2 of the element width in bytes (0 = 8-bit, 1 = 16-bit).
 * The header keeps data[] 4-byte aligned, so 16-bit elements are accessed
 * as naturally-aligned uint16_t — the fast path on 68020. */

typedef struct {
    CL_Header hdr;
    uint32_t length;          /* Number of elements (NOT bytes) */
    uint32_t fill_pointer;    /* CL_NO_FILL_POINTER = none */
    uint8_t  flags;           /* CL_VEC_FLAG_FILL_POINTER | CL_VEC_FLAG_ADJUSTABLE */
    uint8_t  is_signed;       /* 0 = unsigned, 1 = signed element type */
    uint8_t  elt_shift;       /* log2 element bytes: 0 = 8-bit, 1 = 16-bit */
    uint8_t  _pad;
    uint8_t  data[];          /* length << elt_shift packed bytes */
} CL_ByteVector;

#define CL_BYTE_VECTOR_P(obj) \
    (CL_HEAP_P(obj) && CL_HDR_TYPE(CL_OBJ_TO_PTR(obj)) == TYPE_BYTE_VECTOR)

/* Byte size of the element storage. */
#define cl_bytevec_data_bytes(bv)  ((uint32_t)((bv)->length << (bv)->elt_shift))

/* 16-bit element load/store via memcpy: data[] has effective type
 * unsigned char, so accessing it through a uint16_t lvalue would violate
 * C's strict-aliasing rule under -O3 -flto.  memcpy is alias-safe and
 * GCC/vbcc both fold it into a single aligned move on this 4-byte-aligned
 * buffer. */
static inline uint16_t cl_bytevec_load_u16(const uint8_t *p) {
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static inline void cl_bytevec_store_u16(uint8_t *p, uint16_t v) {
    memcpy(p, &v, sizeof(v));
}

/* Element i as a signed 32-bit value honouring the declared width and
 * signedness (always fits a fixnum: [-32768, 65535]). */
#define cl_bytevec_get(bv, i) \
    ((bv)->elt_shift \
     ? ((bv)->is_signed \
        ? (int32_t)(int16_t)cl_bytevec_load_u16((bv)->data + ((i) << 1)) \
        : (int32_t)cl_bytevec_load_u16((bv)->data + ((i) << 1))) \
     : ((bv)->is_signed ? (int32_t)(int8_t)(bv)->data[(i)] \
                        : (int32_t)(bv)->data[(i)]))

/* Store element i.  V must already be range-checked for the vector's element
 * type (see cl_bytevec_check_value); the value is truncated to the width. */
#define cl_bytevec_set(bv, i, v) \
    do { \
        if ((bv)->elt_shift) \
            cl_bytevec_store_u16((bv)->data + ((i) << 1), (uint16_t)(v)); \
        else \
            (bv)->data[(i)] = (uint8_t)(v); \
    } while (0)

/* Active length: fill pointer if present, else total length */
#define cl_bytevec_active_length(bv) \
    ((bv)->fill_pointer != CL_NO_FILL_POINTER ? (bv)->fill_pointer : (bv)->length)

/* --- Pathname --- */

typedef struct {
    CL_Header hdr;
    CL_Obj host;       /* NIL or string */
    CL_Obj device;     /* NIL or string (Amiga volume/assign) */
    CL_Obj directory;  /* NIL or list: (:ABSOLUTE "d1" "d2") or (:RELATIVE ...) */
    CL_Obj name;       /* NIL or string */
    CL_Obj type;       /* NIL or string (extension, no dot) */
    CL_Obj version;    /* NIL or :NEWEST or fixnum */
} CL_Pathname;

#define CL_PATHNAME_P(obj) (CL_HEAP_P(obj) && CL_HDR_TYPE(CL_OBJ_TO_PTR(obj)) == TYPE_PATHNAME)

/* --- Cell (heap-boxed mutable binding for closures) --- */

typedef struct {
    CL_Header hdr;
    CL_Obj value;
} CL_Cell;

#define CL_CELL_P(obj) (CL_HEAP_P(obj) && CL_HDR_TYPE(CL_OBJ_TO_PTR(obj)) == TYPE_CELL)

/* --- Thread object (Lisp-visible wrapper for CL_Thread) --- */

typedef struct {
    CL_Header hdr;
    uint32_t thread_id;   /* Side table index -> CL_Thread* */
    uint32_t table_gen;   /* Generation of the table slot at wrapper-creation
                           * time (cl_thread_table_gen[thread_id]).  Table slots
                           * are reused after a join/reap; a wrapper whose gen no
                           * longer matches the slot's refers to a thread that
                           * has already exited — JOIN/INTERRUPT/DESTROY must not
                           * act on the slot's NEW occupant through it. */
    CL_Obj name;          /* CL string or NIL */
    CL_Obj result;        /* Worker's return value, published here before the
                           * worker unregisters.  Lives on the GC-managed
                           * wrapper (not the unregistered CL_Thread) so it stays
                           * marked+forwarded until JOIN-THREAD reads it. */
} CL_ThreadObj;

#define CL_THREAD_P(obj) (CL_HEAP_P(obj) && CL_HDR_TYPE(CL_OBJ_TO_PTR(obj)) == TYPE_THREAD)

/* --- Lock (MP:LOCK) ---
 *
 * A lock is data: the object holds its own state word and the runtime
 * never allocates an OS mutex for it (specs/mp-locks-heap-words.md).
 * `state` is 0 when free; otherwise the owner's thread serial shifted up
 * one bit, with bit 0 (CL_LOCK_CONTENDED) set once some thread has had to
 * park on it — the "futex mutex 2" encoding, so an uncontended release is
 * one CAS and never scans for waiters.  Blocking goes through the
 * per-thread park handle (platform_park), never through this object's
 * address, so a compaction may move a lock while threads are parked on it. */

#define CL_LOCK_FLAG_RECURSIVE 0x01u
#define CL_LOCK_CONTENDED      0x01u   /* bit 0 of `state` */

typedef struct {
    CL_Header hdr;
    volatile uint32_t state;   /* 0 = free; else owner_serial << 1 | CONTENDED */
    uint32_t depth;            /* nested acquires by the owner; 1 when held once */
    uint32_t flags;            /* CL_LOCK_FLAG_* — recorded so FASL recreates
                                * the same kind of lock at load time */
    CL_Obj name;               /* CL string or NIL */
} CL_Lock;

#define CL_LOCK_P(obj) (CL_HEAP_P(obj) && CL_HDR_TYPE(CL_OBJ_TO_PTR(obj)) == TYPE_LOCK)

/* --- Condition variable (MP:CONDITION-VARIABLE) ---
 * `waiters` counts registered waiters so a notify with nobody waiting
 * returns without taking the thread-list lock. */

typedef struct {
    CL_Header hdr;
    volatile uint32_t waiters;
    CL_Obj name;               /* CL string or NIL */
} CL_CondVar;

#define CL_CONDVAR_P(obj) (CL_HEAP_P(obj) && CL_HDR_TYPE(CL_OBJ_TO_PTR(obj)) == TYPE_CONDVAR)

/* --- Foreign Pointer (wraps a raw machine address, opaque to GC) --- */

#define CL_FPTR_FLAG_OWNED   0x01  /* We allocated this, should free on cleanup */
#define CL_FPTR_FLAG_CHIP    0x02  /* Amiga chip memory (MEMF_CHIP) */

typedef struct {
    CL_Header hdr;
    uint32_t  address;    /* Raw 32-bit machine address (Amiga) or side-table handle (POSIX) */
    uint32_t  size;       /* Allocated size in bytes (0 = unknown/external) */
    uint8_t   flags;      /* CL_FPTR_FLAG_* */
    uint8_t   _pad[3];
} CL_ForeignPtr;           /* 16 bytes */

#define CL_FOREIGN_POINTER_P(obj) \
    (CL_HEAP_P(obj) && CL_HDR_TYPE(CL_OBJ_TO_PTR(obj)) == TYPE_FOREIGN_POINTER)

/* --- FFI stub (binding descriptor — a data object that IS a function) ---
 *
 * What an AMIGA.FFI:DEFCFUN wrapper or an FFI:DEFCSTRUCT accessor *is*,
 * in 20 bytes: an OS library call (LVO + packed register spec + result
 * kind) or a typed peek/poke at a fixed offset.  Stored in a symbol's
 * function cell like any function object; callable through cl_vm_apply /
 * OP_CALL / OP_APPLY (cl_ffi_stub_call, builtins_ffi.c); recognized by
 * compile_call so a direct call to a CL_STUB_LIBCALL symbol compiles to a
 * bare OP_AMIGA_CALL.  Replaces, per binding, a wrapper closure + its
 * off-heap bytecode body + a compiler macro (~550 B and three AllocMems)
 * — the generated raw OS binding modules define thousands of them, so on
 * an 8 MB 68020 this is the difference between affordable and not.  See
 * specs/raw-bindings-footprint.md.
 *
 * No raw C pointers inside: nothing to relink on image restore, and the
 * FASL round-trips it as FASL_TAG_FFI_STUB.  GC children: name, aux. */
typedef struct {
    CL_Header hdr;
    CL_Obj    name;     /* symbol — printing, arglist, trace, backtrace      */
    CL_Obj    aux;      /* LIBCALL: library-base variable (symbol); else NIL */
    uint32_t  a;        /* LIBCALL: regspec — register nibbles bits 0-27,
                         *   result kind bits 28-31 (CL_AMIGA_RES_*);
                         * field kinds: byte offset of the field            */
    int16_t   b;        /* LIBCALL: LVO offset; *_IDX kinds: element size    */
    uint8_t   kind;     /* CL_STUB_*                                         */
    uint8_t   ctype;    /* LIBCALL: number of register args (0..7);
                         * field kinds: CL_STUB_CT_* element C type          */
} CL_FfiStub;           /* 20 bytes */

#define CL_FFI_STUB_P(obj) \
    (CL_HEAP_P(obj) && CL_HDR_TYPE(CL_OBJ_TO_PTR(obj)) == TYPE_FFI_STUB)

/* Stub kinds */
#define CL_STUB_LIBCALL   0   /* (f arg...)      AmigaOS library call        */
#define CL_STUB_PEEK      1   /* (f ptr)         read field at offset A       */
#define CL_STUB_POKE      2   /* (f ptr val)     write field, returns VAL     */
#define CL_STUB_PEEK_IDX  3   /* (f ptr i)       read element i, A + i*B      */
#define CL_STUB_POKE_IDX  4   /* (f ptr i val)   write element i              */
#define CL_STUB_FIELD_PTR 5   /* (f ptr)         foreign pointer to ptr + A   */
#define CL_STUB_KIND_MAX  5

/* Field element C types (CL_FfiStub.ctype for the field kinds).  Mirror
 * FFI:DEFCSTRUCT's type keywords: :pointer is the legacy "32-bit address
 * as an INTEGER", :fptr the foreign-pointer object (NIL for NULL). */
#define CL_STUB_CT_U8      0
#define CL_STUB_CT_I8      1
#define CL_STUB_CT_U16     2
#define CL_STUB_CT_I16     3
#define CL_STUB_CT_U32     4
#define CL_STUB_CT_I32     5
#define CL_STUB_CT_POINTER 6
#define CL_STUB_CT_FPTR    7
#define CL_STUB_CT_SINGLE  8
#define CL_STUB_CT_DOUBLE  9
#define CL_STUB_CT_MAX     9

/* Any of the four flat function representations — the thing OP_CALL can
 * invoke without a symbol indirection.  Funcallable instances (generic
 * functions) are CL_Structs and are NOT covered; callers that accept GFs
 * unwrap them first (cl_unwrap_funcallable).  Use this wherever a
 * "function designator or function object" check used to spell out
 * CL_FUNCTION_P || CL_BYTECODE_P || CL_CLOSURE_P. */
#define CL_FUNCTION_OBJ_P(obj) \
    (CL_FUNCTION_P(obj) || CL_BYTECODE_P(obj) || CL_CLOSURE_P(obj) || \
     CL_FFI_STUB_P(obj))

/* --- Convenience accessors --- */

CL_Obj cl_car(CL_Obj obj);
CL_Obj cl_cdr(CL_Obj obj);
CL_Obj cl_cons(CL_Obj car, CL_Obj cdr);

/* Type name for printing/errors */
const char *cl_type_name(CL_Obj obj);

/* A short, allocation-free, crash-proof description of any 32-bit value
 * for a diagnostic (a symbol's name, a fixnum, a short string, else the
 * type + raw value; a marker for a non-object).  Answers buf. */
const char *cl_obj_brief(CL_Obj obj, char *buf, int bufsize);
/* nargs values through cl_obj_brief, space-separated, "..." when cut. */
const char *cl_args_brief(CL_Obj *args, int nargs, char *buf, int bufsize);

/* Check if obj is of type type_spec (same as CL typep) */
int cl_typep(CL_Obj obj, CL_Obj type_spec);

/* Initialize type system (sets up CL_T, etc.) */
void cl_types_init(void);

#endif /* CL_TYPES_H */
