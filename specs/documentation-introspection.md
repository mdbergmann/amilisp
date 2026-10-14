# Documentation & Introspection (DOCUMENTATION / APROPOS / DESCRIBE / INSPECT)

## Goal

Make the standard CL self-documentation surface actually useful: docstrings
written in `defun`/`defmacro`/`defvar`/`defclass`/`defgeneric` should be
retrievable via `DOCUMENTATION`, discoverable via `APROPOS`, and surfaced by
`DESCRIBE` — on host and Amiga, and through editor tooling (Sly/icl doc
commands, which call `DOCUMENTATION` and `DESCRIBE` under the hood).

## Status (2026-09-09)

All five phases below are implemented; the audit that follows is kept as
the record of where things stood.

- Capture is a load-time `(AMILISP::%SET-DOCUMENTATION 'name 'doc-type
  "...")` call emitted by the compiler for `defun` / `defmacro` /
  `defvar` / `defparameter` / `defconstant` / `deftype`
  (`compiler_extra.c`, `emit_doc_call`) and by the Lisp macros for
  `defstruct` (`structure`), `define-condition` and `defclass` (`type`),
  `defgeneric` (`function`) and `define-method-combination`
  (`method-combination`).  `*documentation-table*` and
  `%SET-DOCUMENTATION` moved to the top of `boot.lisp` so a source boot can
  run the calls too.
- `EXT:*CAPTURE-DOCUMENTATION*` (default `T`, read at compile time) is the
  switch; `scripts/compile-lib-fasls.sh --no-docstrings` binds it `NIL`.
  With it on, `boot.fasl` grew by 16 KB and `clos.fasl` by 50 KB.
- Docstrings now reach the FASL as string literals, so the lib's
  docstrings were made ASCII (the em dashes and section signs) for the
  `AMILISP_FASL_PORTABLE=1` writer.
- `defmacro` hands the wrapper `(macro-function 'x)` returns the lambda
  list as written, so `EXT:FUNCTION-ARGLIST` (and DESCRIBE, and the editor)
  see `(a &body b)` rather than `(#:form #:env)`.
- `apropos` / `apropos-list` are Lisp in `boot.lisp`; `do-symbols` now
  iterates the *accessible* symbols (CLHS 11.1.1.2.1), which they and
  completion need.
- `describe` of a symbol prints `Lambda-list:`, `Macro:`, `Source:
  file:line` and the `Documentation:` / `Variable documentation:` / `Type
  documentation:` / `Structure documentation:` lines; of a compiled
  function, the captured lambda list instead of the bare arity.
- Tests: `tests/test_documentation.c`, the FASL leg in
  `tests/test_dev_commands.sh`, a gc-stress case, and the Amiga suite.
  The editor-facing commands (`ARGLIST`, `COMPLETE`, `DESCRIBE`,
  `APROPOS`, `SOURCE-LOCATION`, `MACROEXPAND`) are in
  `lib/dev-commands.lisp`.

Before this, the *API* was conformant but the pipeline that should feed it
was disconnected: the compiler parsed docstrings and threw them away.

## Current State (audit 2026-07-13)

### DONE

- **`DOCUMENTATION` / `(SETF DOCUMENTATION)`** — CLHS-conformant generic
  function (`lib/clos.lisp:4581-4598`; pre-CLOS bootstrap version in
  `lib/boot.lisp:1007-1016`). Storage: global `*documentation-table*`, an
  `equal` hash keyed on `(cons obj doc-type)`. Explicit
  `(setf (documentation 'foo 'function) "...")` works; `%SET-DOCUMENTATION`
  is hidden from CL externals via the `%`-prefix rule (`src/core/package.c:955`).
- **`DESCRIBE`** — real C implementation (`src/core/builtins_describe.c`)
  with per-type describers for ~20 types: symbols (name, package, value,
  function, plist, SPECIAL/CONSTANT/EXPORTED/TRACED/INLINE flags),
  functions/closures (name, arity, `&rest`, upvalues), structs/conditions
  (slots), packages, hash tables, streams, pathnames, numbers, etc.
- **`INSPECT`** — real interactive inspector (`src/core/builtins_inspect.c`):
  mini-REPL with numbered sub-components, 32-deep navigation stack,
  expression evaluation (implementation-defined interactivity per CLHS).
- **`EXT:FUNCTION-ARGLIST`** (`src/core/builtins.c:1382-1484`) — the exact
  written lambda-list is captured on `CL_Bytecode.source_lambda_list` at
  compile time and survives FASL round-trips (format v9+). C builtins get a
  reconstructed list from min/max arity with `#:ARG0` placeholders; generic
  functions route through `GF-LAMBDA-LIST` in the Lisp layer. Feeds Sly/icl
  arglist display.
- **`EXT:FUNCTION-SOURCE-LOCATION`** — `(FILE LINE)` from
  `CL_Bytecode.source_file/.source_line`; backs Sly M-. (find-definition).
- **MOP introspection** — `class-slots`, `slot-definition-name/-type/
  -initform/-allocation/-readers/-writers/-documentation`,
  `generic-function-lambda-list`, `method-lambda-list`,
  `method-specializers`, etc.
- **Slot-level `:documentation`** — the one docstring that *is* stored:
  `defclass` slot specs carry it into the direct slot definition
  (`lib/clos.lisp:1352`), readable via `slot-definition-documentation`
  (`lib/clos.lisp:517`).

### MISSING / disconnected

- **Docstrings are parsed but discarded.** `process_body_declarations`
  (`src/core/compiler_extra.c:2440`) skips body docstrings per the spec but
  never records them → `(documentation 'foo 'function)` is `NIL` for every
  `defun`/`defmacro` (verified live). Likewise:
  - `compile_defvar` / `compile_defparameter`
    (`src/core/compiler_extra.c:1681`) ignore the third (doc) argument —
    no `variable` doc-type entries.
  - `defgeneric` (`lib/clos.lisp:2882`) silently drops `(:documentation ...)`.
  - `defclass` (`lib/clos.lisp:1392`, `:3511`) ignores class-level
    `(:documentation ...)`.
  - `%define-short-method-combination` (`lib/clos.lisp:3943`) explicitly
    `(declare (ignore documentation))`.
  - `defstruct`, `deftype`, `define-condition` doc positions: same story.
- **`APROPOS` / `APROPOS-LIST` are unimplemented stubs**
  (`src/core/builtins.c:1775-1776`) — fbound so `FBOUNDP` is honest, but
  calling them signals "not yet implemented" (verified live).
- **`DESCRIBE` surfaces neither documentation nor the captured lambda-list**,
  even though `EXT:FUNCTION-ARGLIST` has the exact one on the bytecode object.

## Design Principles

1. **Doc capture must survive FASL.** A compile-time-only side effect is lost
   when loading precompiled FASLs — the same trap already solved for
   `%MARK-SPECIAL`/`%MARK-CONSTANT` (`compile_defvar`'s runtime leg). Emit a
   load-time `(%SET-DOCUMENTATION 'name 'doc-type "...")` call so docs exist
   after FASL load too. If the wire format changes, bump `CL_FASL_VERSION`.
2. **Memory-bounded on Amiga.** Docstrings are heap strings on a 8MB target;
   large libraries (log4cl has >4KB docstrings) can carry substantial doc
   text. Capture must be **off-switchable**: a build/runtime flag (e.g.
   `*capture-documentation*` checked at compile time, default T on host,
   considered per-heap-budget on Amiga) so lean images can drop docs.
3. **No new CL-package symbol leaks.** Helpers stay `%`-prefixed
   (`percent_helper_leaks_cl_external` rule); ANSI-hygiene test must stay green.
4. **DESCRIBE stays C, doc lookup stays Lisp.** `describe_symbol` can call
   into `DOCUMENTATION` via `cl_vm_apply` (GC-protect around it) or read
   `*documentation-table*` directly; either way no format change to the
   describer framework.

## Phased Plan

### Phase 1 — Docstring capture for functions/macros (compiler)

In the `defun`/`defmacro` compile path, after `process_body_declarations`
identifies a leading docstring, emit a load-time
`(%SET-DOCUMENTATION 'name 'function "...")` alongside the definition
(same pattern as `emit_mark_call` for `%MARK-SPECIAL`).

- Doc-types: `function` for `defun`/`defmacro` (CLHS says macros use
  `function` doc-type too).
- Tests: host `tests/test_*.c` + Amiga `run-tests.lisp` — docstring present
  after (a) direct eval, (b) `compile-file` + FASL load. Cover the "string is
  the only body form" case: `(defun f () "not a docstring")` must return the
  string, not record it as doc (CLHS 3.4.11).
- gc-stress: the emit path conses; protect cursors per GC Safety rules.

### Phase 2 — Variables, types, structs, conditions

- `compile_defvar`/`compile_defparameter`/`defconstant`: third arg →
  `variable` doc-type (load-time leg, same as Phase 1).
- `deftype` → `type`; `defstruct` leading docstring → `structure`;
  `define-condition` `(:documentation ...)` → `type`.

### Phase 3 — CLOS doc-types (Lisp only, no C changes)

- `defgeneric (:documentation ...)` → store under `function` doc-type on the
  GF name (and keep for `generic-function-documentation` MOP accessor).
- `defclass` class option `(:documentation ...)` → `type` doc-type.
- `define-method-combination` → `method-combination` doc-type; stop ignoring
  the argument in `%define-short-method-combination`.
- `make fasl` after `clos.lisp` edits (standing rule).

### Phase 4 — APROPOS / APROPOS-LIST

Replace the stubs with real implementations over the package tables
(`DO-ALL-SYMBOLS`-style iteration already exists for the debugger):

- `(apropos string &optional package)` — case-insensitive substring match on
  symbol-name; print each symbol plus value/function annotations, return
  no values (CLHS).
- `(apropos-list string &optional package)` — same match, return fresh list.
- Either C (fast, near `cl_package_export_all_cl_symbols` machinery) or Lisp
  in `boot.lisp` (simpler; iteration cost is fine for an interactive
  command). Prefer Lisp unless it measurably drags on Amiga.

### Phase 5 — DESCRIBE enrichment

- `describe_symbol`: print `Documentation:` (function + variable doc-types
  when present) and `Lambda-list:` via the same path as
  `EXT:FUNCTION-ARGLIST` when the symbol is fbound.
- `describe_closure` / `describe_function`: print the captured
  `source_lambda_list` instead of the bare arity numbers when available;
  print source file/line when recorded.
- Keep output stable enough for shell tests to grep.

## Non-goals

- `method` doc-type granularity (per-method docstrings) — defer until a
  library needs it.
- Documentation for compiler-macros, setf expanders — defer.
- Full SBCL-style `describe` verbosity — output stays terse for 80-column
  Amiga consoles.

## Verification

- `make test` (host) + `make -f Makefile.cross test-amiga` with new cases in
  both suites.
- gc-stress suite for every new allocating path (doc emit, apropos list
  building).
- Live sanity: `(documentation 'foo 'function)` after defun; `(apropos "MAP")`;
  `(describe 'mapcar)` shows arglist + doc. Re-run against a `compile-file`d
  FASL to prove the load-time leg.
