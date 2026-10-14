# Raw OS bindings: pay-per-use footprint

Status: Phase 1, the side fixes AND Phase 2 IMPLEMENTED (2026-08-23,
branch `feat/ffi-stub-descriptors`).  See "Phase 1 — results" and
"Phase 2 — results" below for what landed and where it deviates from
the design text.  Phase 3 (EXT:SAVE-IMAGE :SHAKE-BINDINGS, for Closure
Tale's image-based deployment) IMPLEMENTED 2026-08-23 — see "Phase 3 —
results".  Phase 4 (rebase the curated modules onto raw) DESIGNED
2026-08-23, not yet implemented.
Date: 2026-08-22 (design), 2026-08-23 (results, Phase 3+4)
Scope: `lib/amiga/raw/*` (generated), `lib/amiga/ffi.lisp` (`defcfun`),
`lib/ffi.lisp` (`defcstruct`), the curated `lib/amiga/*.lisp` modules that
use both, and the runtime pieces that give them a representation.

## Problem

A raw binding module is *complete* by design (every function, constant
and struct of a library), and amilisp materialises all of it at load:
each constant is an interned symbol with a value cell, each function a
wrapper closure plus a compiler macro, each struct field a getter, a
setter and a `defsetf`.  The cost is paid whether or not a program
touches a single one of them.

Measured on the host (`make host` build, `--heap 64M`, heap delta
after `(require …)` + `(ext:gc)`, `ROOM`):

| module           | heap     | constants | `defcfun` | struct fields |
|------------------|---------:|----------:|----------:|--------------:|
| raw/exec         |  216 KB  |   370     |   113     |   184         |
| raw/intuition    |  424 KB  |   810     |   124     |   480         |
| raw/graphics     |  477 KB  |   711     |   159     |   586         |
| raw/dos          |  306 KB  |   507     |   154     |   298         |
| **the four**     | **1.42 MB** | 6679 symbols, 3475 fbound | | |

Per form, measured in isolation (1000 of each, same method):

| form                                         | heap each |
|----------------------------------------------|----------:|
| `defconstant`                                |   72 B    |
| `defcstruct` field (getter + setter + defsetf) |  551 B  |
| `defcfun` (wrapper defun + compiler macro)   |  557 B    |

For intuition that is 62 % struct accessors, 16 % function wrappers,
14 % constants, the rest package/export bookkeeping — and the figure
**undercounts**: `CL_Bytecode.code`, `.constants` and `.line_map` are
`platform_alloc`'d off-heap (`compiler.c:1449-1496`), so every one of
the ~1100 functions in intuition is also three small `AllocMem`s on the
Amiga (≈ 10 000 for the four modules — fragmentation and time, invisible
to `ROOM`).  The FASLs mirror it: `intuition.fasl` is 418 KB,
`graphics.fasl` 438 KB, four modules ≈ 1.35 MB of FASL to read and
deserialize on a 68020.

Three second-order costs ride along:

- `cl_get_compiler_macro` (`compiler.c:5353`) scans an **alist** on every
  `compile_call` of any symbol.  The four modules add ~550 entries, so
  every call form compiled afterwards walks them — a compile-time tax on
  unrelated code.
- `setf_table` (`compiler.c:12`, alist) gains one entry per struct field
  (~1550 for the four), scanned on every `(setf (f …))`.
- `exported_p_nolock` (`package.c:117`) is a **list** scan of the
  package's exports; a hit on `amiga.raw.intuition:foo` walks up to ~1500
  cells.  Per qualified read.

The memory note in `raw_os_bindings_generator` already records the
consequence: rebasing the curated modules onto raw was deferred because
"~0.4 MB heap per raw/intuition on an 8 MB 020" is not affordable.

### Why C and assembler pay nothing

In C the binding *is* the header: `#define WA_Left …` and
`struct Window {…}` exist only at compile time; `w->LeftEdge` is one
`move.w 4(a0),d0`; a constant is an immediate.  A library call through
`proto/`+`inline/` (or `#pragma amicall`, or `jsr _LVOOpenWindow(a6)`
in asm) is a handful of instructions *at the call site* — there is no
table of functions anywhere.  With the older `amiga.lib` link stubs the
linker pulls in only the referenced stub objects.  The runtime cost of
"binding intuition.library" in a C program is the 4-byte `IntuitionBase`.

The gap is structural, not a tuning problem: a static language resolves
names at build time and discards the tables; a Lisp image *materialises*
every name.  What follows is how to make amilisp pay per *used* name —
the Lisp analogue of linking — without giving up anything observable.

## Goals

1. `(require "amiga/raw/intuition")` costs O(bindings the program uses),
   not O(bindings that exist).  Target: a typical ReAction program that
   touches ~150 intuition names sits at ~10–30 KB for that module (today
   424 KB).
2. Direct calls stay what they are now: `(move-to rp x y)` compiles to a
   bare `OP_AMIGA_CALL`, `(window-width w)` to a peek — no slower than
   the current compiler-macro path, and the m68k JIT keeps its existing
   `OP_AMIGA_CALL` handling.
3. Indirect use keeps working unchanged: `#'`, `funcall`, `apply`,
   `fboundp`, `symbol-function`, `documentation`, `ext:function-arglist`,
   `trace`, `(setf (field ptr) v)`.
4. Once a name has been touched it is an ordinary symbol in an ordinary
   package — `find-symbol`, `eq`, `symbol-package`, `apropos`,
   `do-symbols` all behave per CLHS.
5. The curated `lib/amiga/*.lisp` modules (which use `defcfun` /
   `defcstruct` directly) get the cheaper representation for free.
6. FASLs stay host-built and portable to the m68k target (big-endian
   byte-vector payloads, ASCII names — the `AMILISP_FASL_PORTABLE=1`
   rules).
7. Amiga load time of a raw module drops from "deserialize ~2000
   top-level units" to "read one byte vector".

Non-goals: changing the raw API surface or names; lazy-loading the
module *file* (it is still one `require`); a general tree shaker
(sketched as Phase 3, not designed here).

## Design overview

Two phases, the first an enabler for the second:

- **Phase 1 — binding descriptors.**  A new heap type holds what a
  `defcfun` wrapper or a `defcstruct` accessor *is* (LVO + regspec, or
  C type + offset) in ~20 bytes, callable from `cl_vm_apply` and
  recognised by `compile_call` directly.  Replaces ~1100 closures +
  bytecode + compiler macros per module with ~1100 small data objects.
  Roughly 2.5× smaller, FASL ~3× smaller, no off-heap allocations, no
  compiler-macro entries.  Symbols remain the dominant residual cost
  (~1500 × ~84 B ≈ 126 KB for intuition), which is why Phase 2 exists.
- **Phase 2 — demand-interned packages.**  The module ships one packed
  *binding table*; the package starts empty and materialises a symbol
  (value, descriptor, export) the first time anything looks the name
  up.  Reading source *is* the reference step, so unused names never
  exist.  This only works economically because Phase 1 made
  materialising a function a 20-byte allocation with no compiler
  involvement.

Plus three small independent fixes surfaced by the measurement
(§ "Side fixes").

## Phase 1 — binding descriptors (`TYPE_FFI_STUB`)

### Representation

```c
/* types.h — new heap type, add to enum CL_ObjType after TYPE_BYTE_VECTOR
 * and bump CL_TYPE_MAX (the GC debug guard aborts on a stale max). */
typedef struct {
    CL_Header hdr;
    CL_Obj    name;     /* symbol — printing, arglist, trace, backtrace  */
    CL_Obj    aux;      /* STUB_LIBCALL: library-base variable (symbol);
                           field kinds: NIL                              */
    uint32_t  a;        /* STUB_LIBCALL: regspec — register nibbles in
                           bits 0-27, result kind in bits 28-31 (see
                           "result kinds" below); field kinds: byte offset */
    int16_t   b;        /* STUB_LIBCALL: LVO; *_IDX kinds: element size  */
    uint8_t   kind;     /* STUB_LIBCALL, STUB_PEEK, STUB_POKE,
                           STUB_PEEK_IDX, STUB_POKE_IDX, STUB_FIELD_PTR  */
    uint8_t   ctype;    /* field kinds: C type code (u8/i8/u16/i16/u32/
                           i32/pointer/fptr/single/double); STUB_LIBCALL:
                           nparams                                       */
} CL_FfiStub;           /* 20 bytes */
#define CL_FFI_STUB_P(obj) (CL_HEAP_P(obj) && CL_HDR_TYPE(CL_OBJ_TO_PTR(obj)) == TYPE_FFI_STUB)
```

One stub per accessor/wrapper, stored in the symbol's function cell like
any function object.  It is a *function* for every CL predicate:
`functionp`, `compiled-function-p` (builtins answer T today — same),
`typep 'function`, `type-of`/`class-of` → `FUNCTION`
(`types.c:110` switch).

### Calling

- `cl_vm_apply` (`vm.c:574`), `OP_CALL` (`vm.c:2326`), `OP_TAILCALL`,
  `OP_APPLY`: one new branch, `if (CL_FFI_STUB_P(func)) result =
  cl_ffi_stub_call(func, args, nargs);`, placed where `CL_FUNCTION_P`
  is tested.  Args are already on the VM stack (GC-rooted) on the OP_
  paths; `cl_vm_apply` copies them there first, as it does for builtins.
- `cl_ffi_stub_call` (new, `builtins_amiga.c` / `builtins_ffi.c`):
  checks `nargs` against the stub (STUB_LIBCALL: `ctype`; PEEK: 1;
  POKE: 2; `*_IDX`: 2/3; FIELD_PTR: 1) and signals the same
  program-error a wrapper defun would; then
  - STUB_LIBCALL: reuses the *existing* `OP_AMIGA_CALL` trampoline
    helper (`builtins_amiga.c`) — value of `aux` must be a foreign
    pointer (the open base), registers from `a`, LVO `b`, result
    conversion per kind.  Non-Amiga builds signal the same error the
    opcode does today.
  - STUB_PEEK/POKE/…: call the C bodies behind `bi_ffi_peek*` /
    `bi_ffi_poke*` / `make-foreign-pointer` with `(ptr, a)` — the
    bounds/NULL checks those builtins already do.
- JIT (m68k): calls on a symbol whose fdefinition is a stub go through
  `cl_jit_runtime_call` → `cl_vm_apply` — covered.  `jit/runtime.c` has
  four `TYPE_CLOSURE` checks; confirm none assumes "not a closure ⇒
  builtin".

### Result kinds — fold the post-processors into the opcode

Today `:result :u16/:i16/:u8/:i8/:bool` wrap the call in a Lisp
post-processor (`%result-u16` …, `ffi.lisp:189-196`) because the
Lisp-visible regspec is a 30-bit fixnum with only bits 28-29 free.  The
stub's `a` is a u32 and the `OP_AMIGA_CALL` operand is already emitted
as `i32` (`compiler.c:4432`), so widen the kind to **bits 28-31**:

```
0 unsigned  1 void  2 pointer  3 signed  4 bool  5 u16  6 i16  7 u8  8 i8
```

`CL_AMIGA_RES_KIND(regspec)` becomes `((regspec >> 28) & 0xF)`; the
trampoline masks/sign-extends d0 for 4–8.  The `amiga:%ffi-call` form
(curated modules, legacy) keeps passing a fixnum regspec — kinds 0–3
are unchanged, so nothing existing breaks.  JIT: `jit.c:2721` decodes
the operand — update alongside, under the `JIT_M68K` guards, verify via
FS-UAE.

### Compiler integration (replaces the compiler macros)

In `compile_call` (`compiler.c:4248`), *after* the compiler-macro
expansion block and *before* generic call emission: if `func` is a
symbol, not locally shadowed (`cl_env_lookup_local_fun` /
`cl_env_resolve_fun_upvalue`), not `notinline`, and its **global
function cell holds a stub**, emit directly:

- STUB_LIBCALL with `nargs == ctype`: compile the args in order, then
  `OP_AMIGA_CALL sym_idx(aux) b a nargs` — exactly the block at
  `compiler.c:4377-4437`, factored into a helper so `%ffi-call` and the
  stub path share it.  Wrong arity → fall through to a normal call so
  the runtime arity error is the one the user expects (the "decline
  expansion" rule the compiler macro follows today).
- STUB_PEEK: compile arg, `OP_CONST a`, `OP_CALL ffi:peek-<ctype> 2`.
  STUB_POKE: likewise with the value arg.  FIELD_PTR / `*_IDX` the same
  way with the arithmetic the current `defcstruct` bodies emit.
  (Optional 1b: `OP_PEEK`/`OP_POKE ctype u32` opcodes the JIT can turn
  into a single `move`; not needed for the footprint goal.)

Semantics are the same as today's compiler macros — the call site bakes
in what the global definition was at compile time; a later redefinition
does not reach already-compiled callers.  `cl_register_compiler_macro`
is no longer called by `defcfun`, removing ~550 entries from the alist
the compiler scans for every call.

`(setf (window-width w) v)`: keep `defsetf accessor %set-accessor`
(one cons per field); `compile_setf` expands to `(%set-window-width w v)`
and `compile_call` inlines the POKE stub.

### The macros

`amiga.ffi:defcfun` (`lib/amiga/ffi.lisp:197`) with ≤ 7 register args
expands to

```lisp
(progn
  (setf (symbol-function 'name)
        (amiga::%make-libcall-stub 'name 'library-base offset regspec nparams))
  (setf (documentation 'name 'function) doc)   ; only when DOC given, see below
  'name)
```

with `regspec` carrying the widened result kind; **no** compiler macro.
The > 7 register plist path (a dozen functions) keeps its `defun` via
`amiga:call-library`.  `ffi:defcstruct` emits `%make-field-stub` forms
for getter/setter/embedded/indexed fields (+ the `defsetf` and the
`*name-size*` defvar as today).

Docstrings: `(setf documentation)` is a hash entry + cons + string per
function (~130 B; intuition ≈ 16 KB).  Keep them on the host; let
`make fasl-amiga` bind `amiga.ffi:*defcfun-docstrings*` to NIL so the
Amiga FASLs omit them (the generator's own `*docstrings*` switch stays
on so the `.lisp` source keeps the C prototypes for reading).

### Introspection

- `ext:function-arglist` on a stub: synthesize `(ARG1 … ARGn)` from
  `nparams` (STUB_LIBCALL) or the fixed shapes; the C prototype in the
  docstring is the useful part anyway.
- `function-lambda-expression` → `NIL NIL name` (as for builtins).
- Printer (`printer.c:1225` neighbourhood): `#<FFI-STUB OPEN-WINDOW LVO
  -204>` / `#<FFI-STUB WINDOW-WIDTH :I16 @8>`.
- `describe` / `disassemble`: print the fields.
- `trace`: `is_func_traced` compares the function object — works.

### GC / FASL / image touch points

- `mem.c`: mark/update children (`name`, `aux`) in the `case
  TYPE_FUNCTION`-style macro at `mem.c:2340`, the sweep/compaction
  switches at `mem.c:5189/5261/5398`, and the `GC_DBG_MAX_TYPE` guard
  (`CL_TYPE_MAX`).  The stub holds no raw pointers, so nothing else.
- `fasl.c`: new `FASL_TAG_FFI_STUB` (`0x21`): `kind u8, ctype u8, b
  i16, a u32, name(obj), aux(obj)`.  **Bump `CL_FASL_VERSION`** (new
  tag + the OP_AMIGA_CALL result-kind widening) with the usual trailing
  comment; `make fasl` twice (self-hosting fix-point, see memory).
- `image.c`: nothing to relink (no C pointers); add the type to the
  switch at `image.c:1100/1298` so restore does not reject it.  Library
  bases are already invalidated/reopened on restore — the stub only
  names the variable.  **Bump `CL_IMAGE_VERSION`** (new heap type).

### Expected result (intuition, 424 KB today)

symbols + names + package/export conses ≈ 1500 × 84 B ≈ 126 KB; stubs
≈ 1100 × 20 B ≈ 22 KB; defsetf conses ≈ 5 KB; docstrings 16 KB (host) /
0 (Amiga); constants' values mostly immediate.  **≈ 150–170 KB, no
off-heap allocations, FASL ≈ 120 KB**, compiler-macro alist shorter by
~550.  The four modules: 1.42 MB → ~0.55 MB.

### Phase 1 — results (implemented 2026-08-23)

Measured with the appendix script (host, FASL-loaded, `make fasl-amiga`
= no docstrings, `ROOM` delta after `(ext:gc)`):

| module           | heap before | heap after | FASL before | FASL after |
|------------------|------------:|-----------:|------------:|-----------:|
| raw/exec         |   216 KB    |  106 KB    |     —       |  151 KB    |
| raw/intuition    |   424 KB    |  218 KB    |   418 KB    |  301 KB    |
| raw/graphics     |   477 KB    |  232 KB    |   438 KB    |  294 KB    |
| raw/dos          |   306 KB    |  140 KB    |     —       |  197 KB    |
| **the four**     | **1.42 MB** | **0.71 MB**|             |            |

Heap: 2.0× (the estimate above said 2.5×; the residual is what was
predicted — symbols, ~1970 of them in intuition at ~84 B, which is Phase
2's target).  No off-heap allocations remain for the bindings.  Load time
on the host 1–3 ms per module.

What the implementation looks like, where it deviates from the design
above, and why:

- `CL_FfiStub` as designed (20 bytes; `*_IDX` kinds pack the element
  count into the high 16 bits of `a` so array accessors bounds-check).
  `cl_ffi_stub_call` in `builtins_ffi.c`; `cl_amiga_call_via_base_sym`
  shared by `OP_AMIGA_CALL` and the LIBCALL stub; `ffi::%ffi-stub-info`
  (plist) for DESCRIBE/tests; `FASL_TAG_FFI_STUB` 0x21 (FASL v30),
  `CL_IMAGE_VERSION` 2; result kinds 4–8 boxed in C, the Lisp
  post-processors are gone.
- **Field stubs are not inlined by the compiler.**  `FLOAD sym; args;
  OP_CALL n` already lands in `cl_ffi_stub_call` with no wrapper frame —
  as cheap as an inlined builtin call — so the compiler hook covers only
  `CL_STUB_LIBCALL` → `OP_AMIGA_CALL`.
- **FASL size did not shrink 3×, and it taught something.**  A
  `compile-file` top-level unit costs ~200 bytes before its first symbol
  (the source path is written per unit, plus the header), and symbols are
  written by full name per unit.  The first cut — one `(setf
  (symbol-function …) (%make-…-stub …))` form per accessor — made
  intuition.fasl *grow* to 656 KB.  Two changes fixed it: `defcstruct`
  now expands to **one** `ffi::%define-cstruct-accessors` call per
  struct (the entry list is a single constant; the installer interns the
  `%SET-` setters and registers the defsetfs — and `compile_call`
  registers the same defsetf pairs at *compile* time when it compiles
  the installer call with a literal list, because the per-field
  `defsetf` forms had that immediate side effect wherever they were
  compiled and the Amiga suite's `(progn (defcstruct …) (setf (acc …)))`
  single-form test depends on it), and
  `compile-file` no longer emits a unit for a top-level `(quote x)`
  (every definer macro ends in `',name` — ~200 B per definition across
  the whole library, `boot.fasl` included).  What remains of
  intuition.fasl is ~810 `defconstant` units and the per-unit source path
  — exactly the part Phase 2's single binding table removes.  (A
  file-level source-path record in the FASL would save ~30 B × units
  everywhere; noted, not done — it needs reader-owned state that survives
  error unwinds.)
- `defcfun` installs the stub at compile time as well (`eval-when`), so
  intra-file callers of a compiled module inline the library call — more
  than the old runtime-registered compiler macro gave them.
- Side fixes: `exported_p_nolock` is O(1) for the home-package case via
  a new exact `CL_SYM_EXPORTED_HOME` flag (the list walk survives only
  for "exported from a non-home package"); `compiler_macro_table` and
  `setf_table` use `CL_AlistIndex`; the Amiga FASLs drop `defcfun`
  docstrings (`amiga.ffi:*defcfun-docstrings*`,
  `compile-lib-fasls.sh --no-docstrings`).

## Phase 2 — demand-interned packages

### Binding table

The module's payload becomes one `TYPE_BYTE_VECTOR` (portable: the FASL
writes byte vectors big-endian, v24/v25), built at **compile time** by a
macro so the `.lisp` source stays the human-readable single source of
truth:

```lisp
(amiga.ffi:define-binding-table "AMIGA.RAW.INTUITION"
  (:use "CL" "FFI" "AMIGA.FFI")
  (:base *intuition-base*)                 ; ordinary defvars precede this form
  (:constants  ("+WA-LEFT+" #x80000064) …)
  (:functions  ("OPEN-WINDOW" -204 (:a0 1) :pointer) …)   ; lvo regs result
  (:structs    ("WINDOW" 136 ("LEFT-EDGE" :i16 4) …) …)
  (:guards     ("ZIP-WINDOW" :not-morphos 46) …))          ; platform / min version
```

The macro packs a blob: header (magic, entry count, name-arena length),
then N entries `{kind u8, flags u8, ctype u8, pad u8, name_off u32,
payload u32, lvo_or_aux i16, …}` (12–16 B) sorted by name, then the
name arena (`u16 len` + bytes each; ASCII only), optionally a doc arena
on host builds.  Struct setters are derived (`%SET-` prefix) at probe
time rather than stored.  Expansion: `(amilisp::%register-binding-table
"AMIGA.RAW.INTUITION" #<byte-vector> '(*intuition-base*) …)` — one
top-level unit in the FASL instead of ~2000.  Loaded from source (no
FASL) the same macro runs at load and packs in Lisp — correct, slower,
and irrelevant for the shipped configuration (`lib/amiga` ships as
FASL).

Size: intuition ≈ 1950 entries × 12 B ≈ 23 KB + ≈ 25 KB of names
≈ **50 KB resident**, plus an optional u16 hash index (8 KB) built on
first probe.

*As built* (`src/core/bindtab.h` is the authority): 16-byte header,
16-byte entries, one name arena that also holds the out-of-line
payloads — wide integers (`CL_BT_F_WIDE`: u8 nbytes, u8 sign, magnitude)
and, since 2026-08-30, **string values** (`CL_BT_F_STRING`: u8 len +
ASCII bytes) for the string `#define`s of a C header (`mui.h`'s
`MUIC_Window "Window.mui"`, the `MUIX_*` text-style escapes); a
`(:const NAME "string")` row materialises as a `DEFCONSTANT` of a fresh
simple string.  No hash index: the probe binary-searches the sorted
entries.  Everything else is pay-per-use: symbol 36 B + name ≈ 28 B
+ 2 conses 24 B + stub 20 B ≈ **~110 B per touched binding**.

### Package hook

`CL_Package` gains `CL_Obj binding_table` (NIL for ordinary packages;
GC-traced; **`CL_IMAGE_VERSION` bump** — layout change).  `defpackage`
for a raw module no longer carries the `:export` list; the package is
created empty with its `:use` list and the table attached.

Lookup: `find_own_symbol` (`package.c:91`) is the single choke point
for `find-symbol`, `intern`, the reader's `pkg:sym` and `pkg::sym`
(`reader.c:823-830`) and use-list inheritance (`find_external_nolock`).
On a miss in a package whose `binding_table` is non-NIL → probe the
table (binary search / hash on the name bytes, string compare, guard
flags against `*features*` and the module's version variable — a
failed guard means *absent*, matching today's `(when …)` skip).  On a
hit → **materialise** and return the symbol as `:EXTERNAL`.

Materialise, respecting the package-lock discipline in `package.c`
("never allocate under the write lock — STW-vs-rwlock hang"):

1. release the read lock;
2. allocate everything outside any lock: the symbol (`cl_make_symbol`),
   its value (constant: fixnum, or bignum for `#xFFFFFFFF`-style
   values), its stub (function kinds), the two conses (bucket link,
   export link); set value/function cells and `CL_SYM_CONSTANT` *before*
   linking so no thread can observe a half-built symbol;
3. take the write lock, **re-check** the bucket (a peer may have won
   the race — if so drop ours, it is garbage), link via
   `package_link_symbol_cell`, push the export cons, register the setf
   pair for `%SET-` twins;
4. unlock; return.

`intern` on a lazy package probes the table first, so `(intern
"+WA-LEFT+" pkg)` yields the binding, not a fresh unbound symbol.

### Enumeration and mutation

Anything that enumerates or mutates the package's symbol set first
calls `%materialise-all(pkg)` — one shot, after which the package is
ordinary (`binding_table` cleared): `do-symbols`, `do-external-symbols`,
`%package-symbols` / `%package-external-symbols`
(`builtins_package.c:985`), `find-all-symbols`, `apropos`, `export`,
`unexport`, `unintern`, `shadow`, `import`, `delete-package`,
`rename-package`, `tools/docs/package-symbols.sh` (docs-check).  Sly
completion on a raw package therefore pays the full cost **once**, on
demand — an acceptable dev-time price; a table-walking
`%package-external-symbols` that avoids materialising can come later.
`do-symbols` on a package that *uses* a lazy package materialises the
used one too (CLHS: inherited symbols are included).

`use-package` conflict checking (CLHS 11.1.1.2.5) walks the table's
name arena against the using package's own symbols — string probes
only, no materialisation.

### What stays eager

`*intuition-base*`, `*intuition-version*`, `%version>=` and the
`provide` — a handful of ordinary definitions emitted before the table
form, exactly as today.

### Interactions

- **GC**: the blob is a byte vector; the index (if any) is platform
  memory rebuilt lazily — or a second byte vector.  No new root classes.
- **Image save/load**: the table is reachable from the package; nothing
  to re-register.  Stubs and materialised symbols survive as ordinary
  heap objects.
- **Cross-check test** (`tests/test_amiga_curated_vs_raw.lisp`) uses
  `find-symbol` / `symbol-value` on raw packages — materialises what it
  probes; still valid.
- **Constant folding** (`try_fold_constant`): the reader materialises
  `+wa-left+` before the compiler sees it, so the constant's value and
  flag are there.
- **M-.** on a materialised name: no bytecode, no `source_file`; the
  table records the module's source path once, and the source-location
  hook answers with "file + grep for the name".
- **Redefinition**: `(defun amiga.raw.intuition:open-window …)` — the
  reader materialises the stub, `defun` overwrites it; fine.

### Expected result

intuition resident ≈ 50 KB (table) + ~110 B × touched names.  A
ReAction example touching ~150 names ≈ **65 KB total vs 424 KB**; the
four common modules for a typical program ≈ 0.25 MB vs 1.42 MB, and the
load is one byte-vector read.  With everything touched (completion,
cross-check test) it converges to the Phase 1 figure — never worse.

### Phase 2 — results (implemented 2026-08-23)

Measured with the appendix script (host, FASL-cache load, `ROOM` delta
after `(ext:gc)`), alongside the two earlier columns:

| module           | eager (Phase 0) | Phase 1   | **Phase 2** | table entries / bytes | symbols at load | FASL (P0 / P1 / **P2**) |
|------------------|----------------:|----------:|------------:|----------------------:|----------------:|------------------------:|
| raw/exec         |   216 KB        |  106 KB   |  **37 KB**  |   725 / 22 KB         |  10             |   — / 151 / **25 KB**   |
| raw/intuition    |   424 KB        |  218 KB   |  **50 KB**  |  1485 / 47 KB         |  23             | 418 / 301 / **53 KB**   |
| raw/graphics     |   477 KB        |  232 KB   |  **55 KB**  |  1511 / 51 KB         |  33             | 438 / 294 / **58 KB**   |
| raw/dos          |   306 KB        |  140 KB   |  **34 KB**  |  1003 / 32 KB         |  10             |   — / 197 / **35 KB**   |
| **the four**     | **1.42 MB**     | **0.71 MB** | **0.18 MB** |                     |                 |                         |

All 131 raw FASLs together are 0.7 MB (`make fasl-amiga`).  On the
target (FS-UAE 68040/JIT, the suite's `; raw-bindings:` lines):
exec/dos/intuition/graphics **24/34/50/55 KB** (Phase 1: 90/135/210/223)
at 140–160 ms per module from FASL.  Touching
150 intuition names (a ReAction-program-sized working set) adds 14 KB
(~95 B per name: symbol + name + export cons + stub/value).  Load time
per module from a cached FASL is ~1 ms on the host; the FASL is one
unit carrying the byte-vector literal plus the handful of eager forms.
The "symbols at load" column is the base/version variables,
`%version>=`, the `:shadow` names and the parameter names of the
>7-register DEFCFUNs — cybergraphics, the worst case, has 46.

What landed, and where it deviates from the design above:

- **Blob**: `src/core/bindtab.c`, layout as designed (16-byte header,
  16-byte entries sorted by name, name arena), packed in **C** by
  `amilisp::%make-binding-table` — the `amiga.ffi:define-binding-table`
  macro only calls it at macroexpansion time, so a source load on the
  68020 packs in C too (the design allowed a Lisp packer).  Values are
  u32 / i32 / or a wide big-endian magnitude of any size (the NDK has a
  13-byte packed-string constant, `TEXTDTCLASS`); row kinds `:const`
  `:var` `:fn` `:field` `:struct` `:name` — `:var` is the
  `*NAME-SIZE*` special variable `defcstruct` used to `defvar`, `:name`
  an export-only row for the >7-register functions whose `defcfun` over
  `call-library` follows the table.  Guards are entry flags + a min
  version byte, evaluated at **probe** time against `*features*` and the
  `:version` variable.
- **Package hook**: `CL_Package.bindings` = `#(blob base-var
  version-var)`; one non-allocating lookup (`lookup_nolock` in
  `package.c`) behind `find-symbol`, `intern`, the reader and use-list
  inheritance: own table → own binding table → each used package's
  externals then its table.  A hit is materialised outside
  `cl_package_rwlock` and linked under it with a re-check; `cl_intern_in`
  re-probes under the write lock so a table registered between its fast
  path and its link cannot be shadowed by a fresh unbound symbol.
  `%SET-<field>` names probe their field entry (the writer is built with
  the reader and DEFSETF-registered).  Enumeration/mutation —
  `%package-symbols`, `%package-external-symbols`, `unintern`,
  `shadowing-import`, `unexport`, `import`, and `export` of a symbol not
  present — flip the package eager first.  `use-package` needed nothing
  (it never did conflict checking).  `CL_IMAGE_VERSION` 3 (package
  layout); no FASL wire change, so no FASL bump.
- **Deviation 1 — a failed guard is present, exported, unbound**, not
  absent.  That is exactly what `(when guard (defcfun …))` + the
  `:export` list produced, so `fboundp`-style feature probes
  (`tests/amiga/test-raw-bindings.lisp`, `test_amiga_bindgen.lisp`) and
  host-side reads of Amiga code (`amiga.raw.intuition:show-window` in a
  file compiled on the host) keep working.
- **Deviation 2 — registration fills present symbols.**  The generated
  `defpackage` keeps its `:shadow` list (`amiga.raw.dos:open`), and
  those symbols exist before the table does; `%register-binding-table`
  walks the package's present symbols and installs the table's
  definition on each one it names (with `force`, so a module reload
  redefines like the eager forms did).  The design's "symbol present ⇒
  never consult the table" invariant holds after that pass.
- **Setters** are derived at probe time as designed; an `(:struct)`
  embedded field has none.
- **Generator**: one `define-binding-table` per module; the C prototype
  moved from the `:doc` string to a trailing comment on each `:fn` row
  (no doc arena — `*defcfun-docstrings*` still governs the eager
  `defcfun`s); `;; skipped` comments stay; field-name clashes inside a
  module become `;; dropped` comments instead of a silent redefinition.
  Source size of intuition: 120 KB → 68 KB.
- **Runtime bugs fixed on the way**: (1) `%CALL-MACRO-EXPANDER` spread a
  macro form's arguments into a fixed `CL_Obj arg_array[255]` and
  **silently dropped everything past the 254th** — the 1005-row
  intuition table form lost 750 rows at macroexpansion.  `vm.c` gained
  `cl_vm_apply_list` (an `OP_APPLY` stub frame, which spreads up to
  `CALL-ARGUMENTS-LIMIT`) and the trampoline uses it beyond 254 args;
  `tests/test_binding_table.c` pins 255/1005/4000.  (2) `MAKE-SYMBOL`
  never filled the symbol's cached name hash (only
  `cl_make_uninterned_symbol` did), so `import` / `export` /
  `shadowing-import` of a fresh `make-symbol` symbol linked it into
  bucket 0 of the package table and `find-symbol` never saw it again —
  in any package.  Surfaced by the pre-commit review's lazy-package
  tests.  (3) The same review caught that the eager-flip calls in
  `cl_export_symbol` / `cl_import_symbol` / `unintern` / `unexport` /
  `shadowing-import` ran an allocating call with the package (and
  symbol) in unprotected C locals; they are rooted first now, with
  gc-stress cases for each of the five.
- **Tests**: `tests/test_binding_table.c` (packer, decoder, every row
  kind and validation message, materialisation through every entry
  point, guards/variants, inheritance, shadowing, flips, reload,
  compaction, `compile-file` round trip, the trampoline);
  `tests/test_amiga_bindgen.lisp` checks rows via
  `%binding-table-entries` instead of grepping source;
  `tests/test_amiga_curated_vs_raw.lisp` builds its LVO tables from the
  rows; `test_gc_stress_regression.sh` and `test_mt_intern_stw.sh`
  (both tiers) gained binding-table cases; `test_image.sh` saves and
  restores a table; `tests/amiga/test-raw-bindings.lisp` checks laziness
  on the target and keeps logging `ROOM` per module.

Not done (noted, cheap to add later): a file-level source-location
record for M-.; a table-walking `%package-external-symbols` that avoids
the flip for completion; MUI/AHI inputs.

## Phase 3 — image delivery: shed the binding tables at `EXT:SAVE-IMAGE` (IMPLEMENTED 2026-08-23)

Motivation: Closure Tale is moving to image-based deployment —
`amilisp` + `closure.img`, the second user story of
`specs/image-save-load.md` (today `run-amiga.sh` boots `amilisp --load
src/main-amiga.lisp` and pays the full load every start).  In an image
the set of referenced names is **closed** the moment the dump is
written; a binding table's only job — answering a name not seen before
— ends there, so in a delivered image the blobs are dead weight.

Measured (host, 2026-08-23): after Phase 2 the four common modules cost
179 KB of heap, of which **152.7 KB is the blobs** (exec 22,190 B,
intuition 47,491 B, graphics 50,815 B, dos 32,182 B; the name arena is
48–52 % of each).  A base image is 384 KB of heap, 560 KB with the four
loaded.  Shedding the tables takes the four modules to ~26 KB of eager
definitions plus ~95 B per name the program actually touched.

The original sketch here was the LispWorks/Allegro delivery model: scan
every bytecode constant vector for referenced symbols, unintern the
rest.  That inverts the value: the risky part (unintern by reachability)
chases only the ~95 B/name materialised residue, while the safe part —
dropping the tables — recovers 85 % of the cost.  So Level 1 below
drops the tables and is designed; the unintern pass stays a sketch
(Level 2), deliberately.

### Level 1 — `:shake-bindings`

Surface: `(ext:save-image path &key quit shake-bindings)`.  In
`bi_save_image` (image.c:1388) parse `KW_SHAKE_BINDINGS` next to
`KW_QUIT_IMG` (the keyword loop at image.c:1399), builtin registration
max args 3 → 5 (image.c:1441), the unknown-keyword message names both.
The flag rides the pending-save state (`image_pending_shake` next to
`image_pending_quit`, image.c:213; `cl_image_save_request` gains the
parameter — image.h:71).  `t` sheds every lazy package; no package-list
variant until something needs one (by construction only raw binding
modules are lazy).  Not an env var, not a `require` mode: shedding is a
property of one dump.

Execution: in `cl_image_save_run_if_pending` (image.c:619), after the
save hooks and the re-checked preconditions and **before** step 3's
`cl_gc(); cl_gc_compact();` (image.c:677) — so a table registered by a
save hook's own `require` is shed too, and the blobs are garbage by the
time the dump GC runs.  Walk `cl_package_registry` (package.c:31), call
the new `cl_bindtab_shed(pkg)` on every package whose `bindings` is
non-NIL, then print one line in the house style:

    ; SAVE-IMAGE: shed 4 binding tables (149 KB) - raw names not
    ; referenced before the save do not exist in the restored image

`cl_bindtab_shed` (bindtab.c) is **not** the eager flip
(`cl_bindtab_materialize_all`, bindtab.c:1050, would materialise ~5000
symbols — the opposite of the goal): under the write lock it stores NIL
into the vector's `CL_BT_SLOT_BLOB` and keeps the 3-slot vector — its
presence is the "this package *was* lazy" marker.  The save
preconditions guarantee a single thread (image.c:262), so the lock is
belt-and-braces.  The blob's only reference is that slot (the FASL
unit's constant vector is not retained after load — the measured
50 KB/module Phase 2 delta counts one copy), so the dump GC reclaims
it.

Two NIL-blob guards make the rest of the runtime treat a shed table as
a guaranteed miss with **no changes at the call sites**:
`cl_bindtab_probe_nolock` returns 0 when the blob slot is NIL (covers
the funnel checks at package.c:357/372/400/553/660/706), and
`cl_bindtab_materialize_all` no-ops — today it only checks
`pkg_bindings` for NIL (bindtab.c:1053) and would `view_blob` a NIL
blob, so this guard is load-bearing, not cosmetic.

### The closed-world contract (restored image)

- Every name touched before the save is an ordinary symbol with its
  stub/value — direct calls, `#'`, `funcall`, `(setf (field …))`,
  `trace` unchanged.
- A never-touched name is **gone**: `find-symbol` → NIL,NIL (per CLHS,
  indistinguishable from a name that never existed); `intern` makes a
  fresh unbound symbol.  Dynamic references — `(intern (format nil …))`,
  `read` of input naming raw symbols — must all happen before the save:
  the same closed-world assumption every delivery-model Lisp makes.
- **Read-time probes change behaviour**: under Phase 2,
  `(fboundp 'amiga.raw.x:untouched)` materialised the name and returned
  NIL; in a shed image the *reader* signals "Symbol … not exported"
  (reader.c:833).  Touch probed names before saving, or probe with
  `(find-symbol "NAME" pkg)`.
- Diagnosability: the kept 3-slot vector answers a new
  `cl_package_shed_p`; the reader's qualified-symbol miss appends
  ", whose binding table was shed by SAVE-IMAGE :SHAKE-BINDINGS - names
  not referenced before the image was saved do not exist in it";
  `%binding-table-info` returns `(:shed T :entries 0 …)`; `describe` of
  the package says it.
- Guard outcomes are **baked on the saving machine**: a materialised
  V47/MorphOS-guarded name carries the save machine's answer (the eager
  modules had the same property per load; an image carries it across
  machines).  Ship images built against the oldest configuration you
  support, or keep explicit `(amiga:%version>= n)`-style checks in app
  code.
- Escape hatch: hand-loading the module's FASL again re-registers a
  fresh table (`%register-binding-table` replaces any previous one,
  bindtab.h:99) — a restored dev session can buy the full name set
  back; `require` alone won't (the module is in `*modules*`).

No `CL_FASL_VERSION` bump (no wire change) and no `CL_IMAGE_VERSION`
bump (no layout change — a shed package is an ordinary package whose
bindings vector has a NIL slot; images are per-build anyway).

### Level 2 — sketch: uninterning the dead residue (not designed)

What remains per touched name is ~95 B (symbol + name + export cons +
stub/value).  Some touched names are dead at runtime — a folded
`+wa-left+` whose value sits in a constant vector while the symbol is
referenced by nothing but the package.  A precise pass exists: mark
from all roots with the shed packages' symbol tables and export lists
treated as weak, unintern the unmarked, let the dump GC sweep them.
Two anchors defeat the naive version — `setf_table` roots the
`(reader . %SET-reader)` pair of every materialised field accessor, and
the `documentation` hash roots host docstrings — so the pass needs
per-table weakness rules.  Payoff: a ReAction-sized working set of 150
names is 14 KB total, the dead subset less.  Revisit only if
measurement after Phase 4 shows tens of KB of dead residue; until then
the risk/benefit is upside down.

### Deployment recipe (Closure Tale)

Images are per-build and per-platform (docs/ext.md "Images are
per-build"), so the shipped image must be written by the shipped Amiga
binary — inside FS-UAE, the same harness `tests/amiga/image-save.lisp`
uses:

1. A build entry script loads everything `src/main-amiga.lisp` loads
   but stops **before** UI init: no open screen/window/file (OS handles
   don't survive — open streams and owned foreign pointers block the
   save, image.c:277-320; restored foreign pointers are invalidated).
2. Push the game entry onto `ext:*restore-hooks*`, then
   `(ext:save-image "closure.img" :shake-bindings t :quit t)`.
3. Shipped launch line: `amilisp --image closure.img` (restore hooks
   run after `~/.amilisprc`; add `--no-userinit` if rc interference
   matters).  `--eval "(tale:main)"` works too — actions run after the
   restore (main.c:747).
4. Ship `amilisp` + `closure.img` + the runtime data files (worlds/);
   `lib/` is needed only for post-restore `require`s of modules the
   image doesn't already contain.

### Tests

Host (`make test`):

- `tests/test_binding_table.c`: shed a registered table — following GC
  reclaims the blob (heap shrinks), probe misses, materialise-all
  no-ops, `%binding-table-info` reports `:shed`, re-registration
  restores laziness; the same under `AMILISP_GC_STRESS=1` (gc-stress
  tier).
- `tests/test_image.sh`: load raw/intuition on the host; touch a
  constant, a field accessor and `#'`a function;
  `(ext:save-image f :shake-bindings t)`.  Restored: touched names
  intact (value, `(setf (field …))`, `funcall` reaches the arity
  check), untouched name → `find-symbol` NIL, reader `pkg:untouched`
  error carries the shed hint, image file measurably smaller than an
  unshaken one.  `:shake-bindings` with no lazy package loaded: clean
  no-op.

Amiga (`make -f Makefile.cross test-amiga`): extend
`tests/amiga/image-save.lisp` / `image-verify.lisp` — save with a raw
module loaded and an OS call touched; the restored session makes the
call against the real library and logs `ROOM` next to the existing
`; raw-bindings:` suite lines.

Docs: `docs/ext.md` heap-images table (`&key quit shake-bindings`) plus
a delivery paragraph; one sentence in README "Heap images".

Effort: ≈ 1 day — `cl_bindtab_shed` + the two NIL-blob guards + keyword
plumbing + reader hint + tests.

### Phase 3 — results (implemented 2026-08-23)

Measured on the host (`--heap 64M`, FASL-cache load, image file size and
the `ROOM` heap the dump reports):

| image contents                     | plain      | `:shake-bindings t` | shed   |
|------------------------------------|-----------:|--------------------:|-------:|
| base session (no raw module)       |   589 KB   |      589 KB (no-op) |   0    |
| + raw/intuition                    |   665 KB   |          617 KB     |  47 KB |
| + exec, intuition, graphics, dos   |   781 KB   |          629 KB     | 150 KB |

Heap at the dump: 449 → 402 KB (intuition), 560 → 411 KB (the four).  The
saving is exactly the blob bytes — nothing else changes — and a session
with no lazy package is an untouched no-op.

What landed, and where it deviates from the design above:

- **`cl_bindtab_shed` / `cl_bindtab_shed_all` / `cl_bindtab_shed_p`**
  (`bindtab.c`) as designed: a plain store of NIL into
  `CL_BT_SLOT_BLOB` under the write lock, the 3-slot vector kept as the
  "was lazy" marker, the blob reclaimed by the dump GC.
  `EXT:SAVE-IMAGE` gained `:shake-bindings` (max args 3 → 5,
  `image_pending_shake`, shed at step 2b — after the save hooks, before
  `cl_gc(); cl_gc_compact()`).
- **The NIL-blob guard went in three places, not two.**  Besides
  `cl_bindtab_probe_nolock` and `cl_bindtab_materialize_all`,
  `cl_bindtab_materialize` needed it: it tests the bindings *vector* for
  NIL to detect a concurrent flip, and a shed package has a non-NIL
  vector.  All three now read the blob through one `pkg_blob()` helper,
  which is the invariant worth keeping: nothing reads
  `CL_BT_SLOT_BLOB` directly.
- **Deviation — an internal `amilisp::%shed-binding-tables` builtin.**
  The design had no Lisp surface at all, which would have made the
  operation testable only by writing an image (and untestable under
  `AMILISP_GC_STRESS`, where a dump is impractical).  It mirrors the
  existing `%BINDING-TABLE-MATERIALIZE-ALL` hook: internal, undocumented
  in `docs/ext.md`, returns the bytes shed.
- `%BINDING-TABLE-INFO` reports `:SHED T` with `:ENTRIES`/`:BYTES` 0;
  `%BINDING-TABLE-ENTRIES` returns NIL.  The reader's
  unknown-qualified-symbol error appends "that package's binding table
  was shed by SAVE-IMAGE :SHAKE-BINDINGS, so only names referenced
  before the image was saved exist in it" — the single most important
  line of the whole phase, since the alternative reads as a typo.
- **The printed line names the session, not just the image**: shedding
  is permanent for the running process too, and a write failure
  afterwards does not put the tables back.  `; SAVE-IMAGE: shed N
  binding table(s) (K KB) - the image and this session now resolve only
  the names already referenced in those packages`.
- **A GC bug caught before it shipped, in the hint itself.**  The first
  version asked `cl_bindtab_shed_p(package)` *after*
  `cl_find_symbol_with_status` — but `package` is an **unprotected**
  reader local, and that lookup allocates whenever it materialises an
  **inherited** name from a used package's table (status `:INHERITED`,
  which is exactly when the error path runs).  Any allocation may
  compact, leaving the local stale: the textbook failure of this
  codebase's GC discipline, in code whose only job is to print a
  message.  The shipped form **samples the flag before the lookup**
  (`int shed_pkg = cl_bindtab_shed_p(package);`) — no GC window by
  construction, no lock, and cheaper than the intermediate fix that
  re-derived the package with `cl_find_package` on the error path.
  `test_gc_stress_regression.sh` pins the window ("shed hint reads a
  LIVE package after an inherited materialisation") with a shed package
  that uses a still-lazy one.  Worth remembering: reading a package or
  symbol *after* a lookup is not obviously an allocating call site, but
  with demand-interning it is one.
- **Learned while testing**: materialising a field accessor also
  materialises its `%SET-` twin, so a shed package never has a
  half-present reader/writer pair — worth knowing, and now pinned.
- Not done: `DESCRIBE` of a package says nothing about its binding
  table (it never did — there is no package branch for it), so
  `%BINDING-TABLE-INFO` remains the introspection path.
- No `CL_FASL_VERSION` / `CL_IMAGE_VERSION` bump, as predicted (a shed
  package is an ordinary package whose bindings vector has a NIL slot).
- **Tests**: `tests/test_binding_table.c` (two cases: the closed-package
  semantics end to end, and `shed_all` proving the blobs become garbage
  by measuring `cl_heap.bump` across a compaction);
  `tests/test_image.sh` (10 new checks: the report line, the image
  actually shrinks, the restored session keeps touched names and misses
  untouched ones with the hint, an unshaken control image, the no-lazy-
  package no-op, the keyword error); `test_gc_stress_regression.sh`
  (shed under the every-allocation compaction storm — and `test_image.sh`
  already runs under `AMILISP_GC_STRESS=1` in that target);
  `tests/amiga/image-save.lisp` + `image-verify.lisp` now save with
  `:shake-bindings t` and check 9 target-side properties (the second
  Amiga image leg, `boot.img` → full suite, deliberately stays unshaken
  so shedding cannot mask a regression there).

## Phase 4 — rebase the curated modules onto raw (designed 2026-08-23)

The deferred item from `raw_os_bindings_generator`, finally affordable.
Motivation is **single source of truth**, not memory: every curated
LVO, offset and constant value duplicates a generated raw row and can
drift (the cross-check test exists precisely because it did); and for
Closure Tale's image deployment Phase 3 has nothing to shed until the
game's modules sit on the tables.  A bonus is speed: the curated
intuition/gadtools/exec/audio calls go through `amiga:call-library`'s
runtime plist path (consing per call); rebased onto raw `:fn` stubs
they compile to bare `OP_AMIGA_CALL`.

### Census (2026-08-23; the numbers the design rests on)

Seven curated modules (there is no curated `dos`), 285 exported names,
all with explicit `:export` string lists, none with `:shadow`:

- **Functions**: 73 entry points (15 `defcfun` in graphics; 36 hand
  `amiga:call-library` sites + `+LVO-…+` constants in
  intuition/gadtools/exec; audio funnels 10 exec calls through one
  `%exec`; reaction already calls raw packages; arexx has no FFI).
  **65 name-identical** to a raw `:fn` row (some in `raw/exec` /
  `raw/dos` rather than the module's own library), **8 renamed**,
  **0 missing**.  The renames are three generator conventions:
  `MOVE-TO/DRAW-TO` → raw `MOVE/DRAW` (no `-TO` in the SFD name),
  hump-splitting (`SET-DRMD`→`SET-DR-MD`, `GT-GET-IMSG`→`GT-GET-I-MSG`,
  `GT-REPLY-IMSG`→`GT-REPLY-I-MSG`, inverse `BEST-MODE-ID-A`→
  `BEST-MODE-IDA`), a dropped `%` (`%WRITE-CHUNKY-PIXELS`→
  `WRITE-CHUNKY-PIXELS`) and the foreign-library prefix
  (`+LVO-DOS-DELAY+`→`AMIGA.RAW.DOS:DELAY`).
- **Constants**: 172 non-LVO; **148 name-identical** (including the
  tag-expression ones — raw inlines the absolute values), 24 absent
  under the curated name: 5 renamed (`+JAM1+`→`+RP-JAM1+` family,
  `+WA-CUSTOMSCREEN+`→`+WA-CUSTOM-SCREEN+`), 5 living in another raw
  module (`+TAG-USER+`→utility, `+IECLASS-…+`/`+IEQUALIFIER-…+`→
  devices/inputevent), 3 kind-changed to the `:struct`-derived size
  variable (`+RASTPORT-SIZE+`→`*RASTPORT-SIZE*` etc.), and 11
  genuinely curated-only (minterm/timing/API constants + the
  `+WA-DUMMY+`/`+SA-DUMMY+` tag bases).  One value quirk:
  `+NM-BARLABEL+` is `#xFFFFFFFF` curated vs `-1` raw — same 32 bits.
- **Struct accessors**: 61; **38 identical, 22 renamed** (hump splits,
  `WINDOW-UPORT`→`WINDOW-USER-PORT`, the C-field-prefix conventions in
  NewMenu/InputEvent, and audio's flattened `IOAUDIO` vs raw's
  `IO-REQUEST` header + `IO-AUDIO` tail split), **1 missing**
  (`IOAUDIO-LN-PRI` — the Node priority; alias it to raw/exec's
  NODE/MESSAGE accessor if the generator emits one, else it stays a
  hand accessor).
- **No dynamic name lookups anywhere in `lib/amiga`** — zero `intern`
  / `find-symbol` / `read-from-string`; reaction's tag builders take
  resolved values, class names are strings passed to `NewObjectA`.
  Renames cannot break a reflective path.
- **The C runtime binds only to `AMIGA` and `KEYWORD` names**
  (package.c:1370, builtins_amiga.c:783-794 — nothing interns into
  `AMIGA.GFX` etc. by name).
- **Every external user is package-qualified against the curated
  names** (lambda-tale/src 280 refs, dominated by `amiga.gfx:set-a-pen`
  68 / `rect-fill` 39; one `:use` in `examples/amiga/gfx/`), so the
  curated API surface must stay stable — and can, cheaply.

### Design

Curated packages, names and exports stay exactly as they are.  What
changes is where the definitions come from:

1. Each curated module `(require "amiga/raw/<lib>")` (plus
   `raw/utility`, `raw/devices/inputevent`, `raw/exec`, `raw/dos`
   where the census says so) **before** its `defpackage` — the
   established order (see the `amiga/ffi`-before-`#+amigaos` rule).
2. **Name-identical entries (65 fn + 148 const + 38 accessors) become
   import-and-reexport**: the `defpackage` gains `(:import-from
   "AMIGA.RAW.GRAPHICS" "SET-A-PEN" …)`; the existing `:export` list
   already names them.  Importing materialises each name once
   (~107 B) — the curated surface is ~285 names, so the whole eager
   cost is ~30 KB and the tables stay lazy for everything else.  The
   curated `defcfun`s, hand LVO constants, `call-library` wrappers
   whose body is just the call, duplicated `defconstant`s and
   duplicated `defcstruct`s are deleted.
3. **Renamed functions/accessors (8 + 22) become aliases sharing the
   raw stub object**: `(setf (fdefinition 'move-to)
   #'amiga.raw.graphics:move)` — one line each; `compile_call` inlines
   through the curated name exactly as through the raw one (the global
   function cell holds a stub).  Writable renamed accessors also get
   `(defsetf rastport-fgpen amiga.raw.graphics::%set-rastport-fg-pen)`.
4. **Renamed/kind-changed constants** keep a curated `defconstant`
   whose value *is* the raw symbol: `(defconstant +jam1+
   amiga.raw.graphics:+rp-jam1+)`, `(defconstant +rastport-size+
   amiga.raw.graphics:*rastport-size*)` — one definition of the value,
   two names.  The 11 curated-only constants stay as they are.
5. **Base/version variables**: where the names differ (`*gfx-base*` vs
   the raw module's variable) the curated name becomes
   `define-symbol-macro` onto the raw variable, so open/close code and
   the stubs always see the same cell.  Curated open-library code sets
   the raw variable.
6. **`+NM-BARLABEL+` takes raw's `-1`** (import).  Verify the u32 tag
   poke path accepts the negative and the menu-bar label still renders
   (Amiga suite).
7. **What keeps hand definitions**: the Lisp-level API (`with-window`,
   `with-bitmap`, `write-chunky`'s planar fallback, `play-sample`, the
   arexx server, reaction's pool/tag helpers) — unchanged; audio's
   flattened `IOAUDIO` accessors become aliases onto
   `IO-REQUEST-*`/`IO-AUDIO-*` per the census, `IOAUDIO-LN-PRI` per
   above; reaction's 8 duplicated class constants stay duplicated
   *deliberately* (reaction.lisp:73-81 — the raw class modules open
   their class library at require time, which must not happen on a
   bare 3.1/host load) — the cross-check test keeps guarding their
   values; `+TAG-DONE+` can switch to an import from `raw/utility`
   (reaction already requires it).

Compile order: `compile-lib-fasls.sh` compiles each curated module
with its raw modules loaded (the `require` at the top of the file does
it — same requirement cross-compiling user code has today).  No
`CL_FASL_VERSION` / `CL_IMAGE_VERSION` change.

### Footprint accounting (state it honestly)

For a FASL-loaded program using only curated modules (lambda-tale's
set: exec/graphics/intuition/gadtools/audio, ~97 KB eager today), the
rebase **costs** memory: the raw tables it pulls in (~135 KB) plus
~30 KB of imports, minus the deleted duplicates.  The offsetting
levers: Phase 2b-style blob compaction (the name arena front-codes
77 KB → 49 KB across the big four; 12-byte entries save another
~19 KB) if the FASL-mode number matters, and **Phase 3**, which turns
the tables back into ~0 for the shipped image — rebase + shed is
strictly better than today (~60 KB vs ~97 KB, single source of truth,
no `call-library` consing).  A program loading curated *and* raw
modules (the ReAction examples) wins immediately (the duplicates go).

### Tests

- `tests/test_amiga_curated_vs_raw.lisp` changes role: identical-name
  entries become the *same symbol*, so (1) assert `eq` of the function
  cells for every import/alias pair, (2) assert the frozen 285-name
  export surface is intact (fboundp/boundp per name — API stability),
  (3) keep value-matching only for the documented hand definitions
  (reaction's 8, curated-only constants, any remaining hand
  accessor) — the "uncovered" report should shrink to exactly that
  list.
- Host: `make test` + gc-stress over the import-heavy load path;
  `tests/test_lib_fasl_portable.sh` (curated FASLs now carry
  `:import-from` units).
- Amiga: `test-gui.lisp`, `test-reaction.lisp`, `examples-amiga`
  screenshots unchanged — they are the regression net for the 36
  `call-library` sites now going through stubs; plus the
  `+NM-BARLABEL+` menu check.
- Downstream: run lambda-tale's and Closure's suites against the
  rebased checkout before a release (their repos, their gates — but
  they are the API's biggest users).

### Effort and order

≈ 1–2 days: exec (trivial) → graphics (the 2 `MOVE/DRAW` aliases +
15 defcfun deletions) → intuition → gadtools → audio → reaction
cleanup.  Independent of Phase 3; for Closure the pair lands as
rebase first, then shed.

## Side fixes (independent, small, worth doing regardless)

1. `exported_p_nolock` list scan → a second hash vector per package (or
   an `exported` bit on the bucket cell) — `package.c:117-137`.
2. `compiler_macro_table` and `setf_table` alists → hash index, same
   pattern as `struct_index` in `builtins_struct.c` (the "registry hash
   index" precedent).  Phase 1 removes the raw entries, but quicklisp
   systems add plenty of their own.
3. Amiga FASLs without `defcfun` docstrings
   (`amiga.ffi:*defcfun-docstrings*`, bound by `make fasl-amiga`).

## Generator (`scripts/gen-amiga-bindings.lisp`)

Phase 1: **no change** — `defcfun`/`defcstruct` change underneath it.
Phase 2: `emit-function` / `emit-struct` / the constants emitter write
rows of the `define-binding-table` form instead of individual forms;
`;; skipped …` comments stay; `(when (member :morphos *features*) …)`
and `(%version>= n)` guards become entry flags; `*x-size*` becomes a
constant entry; the `defpackage` loses its `:export` list.  The
generator test (`tests/test_amiga_bindgen.sh` + `.lisp`) should load the
generated fixture module and check the *materialised* symbols
(`find-symbol`, `symbol-value`, stub fields via `describe`/an
introspection builtin) rather than grepping forms — that keeps it
format-agnostic across both phases.

## Tests

Host (all in `make test`):

- `tests/test_ffi_stub.c`: create each stub kind; call through
  `cl_vm_apply` with right/wrong arity; `functionp`/`compiled-function-p`
  /`type-of`/printer; FASL write → read round trip; GC: stubs survive
  compaction with `name`/`aux` forwarded; image save/restore of a stub.
- shell: compile a module using `defcfun`/`defcstruct` to FASL, load it,
  check `fboundp`, `documentation`, `ext:function-arglist`, `#'`+
  `funcall`, `(setf (field p) v)`; `disassemble` shows `OP_AMIGA_CALL`
  for a direct call and a plain call for `funcall`; result-kind 4–8
  decoding (host trampoline stub returns a known d0).
- `tests/test_lazy_package.c` / `.sh`: `find-symbol` materialises with
  `:EXTERNAL`; `intern` on a lazy package; inheritance through `:use`;
  `do-symbols`/`apropos` materialise-all; `use-package` conflict
  detection against table names; failed guard ⇒ absent; `unintern` then
  re-`find-symbol`; two threads `find-symbol` the same name ⇒ `eq`
  (MT test, same harness as `tests/test_mt_*`).
- **gc-stress** (`make test-gc-stress`): materialisation allocates
  outside the lock and links under it — run the lazy-package tests with
  `AMILISP_GC_STRESS=1`; run the existing curated/raw cross-check under
  stress too.
- `tests/test_amiga_curated_vs_raw.sh`, `tests/test_lib_fasl_portable.sh`,
  `tests/test_amiga_bindgen.sh` keep passing.
- `docs-check` after materialise-all semantics are in.

Amiga (`make -f Makefile.cross test-amiga`, and `FPU=1`):

- `tests/amiga/test-raw-bindings.lisp` unchanged in intent — calls
  reach the OS through stubs (direct and `funcall`), struct accessors on
  live OS objects, every result kind; add a `ROOM` line before/after each
  `%raw-require` so the suite log carries the numbers.
- `test-gui.lisp`, `test-reaction.lisp`, `examples-amiga` (screenshots)
  unchanged.
- Record before/after heap and load time per module on the
  `test-amiga-lowend` (68020) config in `docs/benchmarks.md`.

Versioning: `CL_FASL_VERSION` (new tag, operand widening) and
`CL_IMAGE_VERSION` (new heap type; `CL_Package` field) — both bumps are
load-bearing.

## Risks and open questions

- **CLHS observability of laziness.**  After `unintern` of a
  materialised name the design flips the package eager first, so nothing
  resurrects silently.  `find-symbol` before any reference returns the
  (materialised) symbol — indistinguishable from eager.  The one
  honest deviation: a program that enumerates the package pays the
  eager cost at that moment, never less.
- **Lock discipline.**  Materialisation must follow `package.c`'s
  "allocate outside, link under the write lock, re-check" rule exactly;
  the MT + gc-stress tests are the guard.
- **Compile-time stub detection** bakes LVO/regspec into callers like
  the compiler macros do today.  Cross-compiling user code on the host
  for the Amiga needs the module loaded at compile time (`eval-when
  (:compile-toplevel …) (require …)`) — same requirement as today.
- **Source-load fallback on the Amiga** (no/stale FASL) packs the blob
  in interpreted Lisp; measure once on the 020 config; if it is too
  slow, ship raw modules FASL-only (already the release policy).
- **JIT**: confirm `jit/runtime.c`'s closure checks and the
  `OP_AMIGA_CALL` decoder; verify via FS-UAE, under `JIT_M68K` guards.
- **Sly source location** for materialised names is file-level only.

## Ordering and effort

1. Side fixes 1–3 (small, independent; can land first).
2. Phase 1: type + VM/`cl_vm_apply` branch + trampoline result kinds +
   `compile_call` hook + macros + FASL/image + tests (≈ 2–3 days).
   Measurable win on its own; curated modules benefit immediately.
3. Phase 2: table format + macro + package hook + enumeration/mutation
   flips + generator + tests (≈ 4–6 days).  Re-measure; then revisit
   rebasing the curated modules onto raw (the deferred item in
   `raw_os_bindings_generator`).
4. Phase 3 (≈ 1 day): worthwhile once an image-based deployment
   exists (Closure Tale).  For Closure the payoff arrives together
   with the curated rebase — the game uses only curated modules, so
   there is nothing to shed until those sit on the raw tables.
5. Phase 4 (≈ 1–2 days): independent of Phase 3, but for Closure the
   pair lands as rebase first, then shed.  FASL-mode footprint goes
   up slightly (see "Footprint accounting"); the image mode it
   enables ends below today's numbers.

## Appendix — measurement method

```lisp
;; host: AMILISP_NO_USERINIT=1 build/host/amilisp --no-userinit \
;;   --non-interactive --heap 64M --load measure.lisp </dev/null
(defun heap-used ()
  (ext:gc)
  (let ((s (with-output-to-string (*standard-output*) (room))))
    (parse-integer s :start (+ 8 (search "Heap:" s)) :junk-allowed t)))
(dolist (m '("amiga/raw/exec" "amiga/raw/intuition"
             "amiga/raw/graphics" "amiga/raw/dos"))
  (let ((before (heap-used)))
    (require m)
    (format t "~A: ~D KB~%" m (round (- (heap-used) before) 1024))))
```

Per-form costs: 1000 generated `defconstant`s, 50 `defcstruct`s × 10
`:i16` fields, 200 `defcfun`s with 3 register args, each block bracketed
by `heap-used`.  Remember the off-heap caveat above: bytecode bodies are
not in these numbers.
