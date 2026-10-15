# `AMILISP` — Implementation Extras

The `AMILISP` package is mostly **implementation internals** — boot/CLOS/loop
helpers and metaobject plumbing prefixed with `%` or `*` (over 220 of them). Those
are not a stable API and are intentionally undocumented here.

A small number of `AMILISP` symbols are genuinely useful from user code; those are
documented below.

- **Package:** `AMILISP` (mutually `:use`d with `CL` during bootstrap)
- **Inherited by:** `COMMON-LISP-USER`.

## IEEE float bit access

Convert between floats and their raw IEEE-754 bit patterns. Used by the
**float-features** AmiLisp fork (e.g. so jzon can serialize floats).

| Signature | Kind | Description |
|-----------|------|-------------|
| `(single-float-bits float)` | function | The 32-bit integer bit pattern of a `single-float` |
| `(double-float-bits float)` | function | The 64-bit integer bit pattern of a `double-float` |
| `(bits-single-float bits)` | function | Reconstruct a `single-float` from a 32-bit pattern |
| `(bits-double-float bits)` | function | Reconstruct a `double-float` from a 64-bit pattern |

## Package-local nicknames (CDR-10)

| Signature | Kind | Description |
|-----------|------|-------------|
| `(add-package-local-nickname nick-name target-package &optional package)` | function | Add a local nickname for `target-package` within `package` (default: current package) |
| `(remove-package-local-nickname nick-name &optional package)` | function | Remove one |
| `(package-local-nicknames package)` | function | List the local nicknames defined in a package |

## Type predicates

| Signature | Kind | Description |
|-----------|------|-------------|
| `(structurep object)` | function | Whether an object is a `defstruct` instance |
| `(conditionp object)` | function | Whether an object is a condition |
| `(condition-type-name condition)` | function | The type name of a condition object |
| `(condition-slot-value condition slot-name)` | function | Read a slot from a condition object |

## Type expansion

The implementation's own `deftype` expanders, with the signature and return
convention of `sb-ext:typexpand` (CLISP's `ext:type-expand`). The
**introspect-environment** AmiLisp backend delegates to these; through it,
serapeum's `explode-type` resolves user `deftype` aliases. A type specifier is
a symbol, a cons, or a class object; standard type names, class objects and
compound types whose head names no `deftype` come back unchanged. The optional
environment is accepted and ignored.

| Signature | Kind | Description |
|-----------|------|-------------|
| `(typexpand-1 type &optional env)` | function | Expand `type` one step if it is a `deftype` alias. Returns the expansion and `T`, or `type` and `NIL` |
| `(typexpand type &optional env)` | function | Expand `type` until it is no longer a `deftype` alias. Returns the final expansion and `T` if anything expanded, or `type` and `NIL` |

```lisp
(deftype color () '(member :red :green :blue))
(deftype paint () 'color)
(amilisp:typexpand-1 'paint)   ; => COLOR, T
(amilisp:typexpand 'paint)     ; => (MEMBER :RED :GREEN :BLUE), T
(amilisp:typexpand 'string)    ; => STRING, NIL
```

## JIT & trace toggles (AmigaOS)

The m68k JIT is on by default; these `%`-prefixed entry points toggle and inspect
it (no-ops / compiled out on the host build). Useful for A/B benchmarking and
isolating JIT bugs around a single `defun`.

| Signature | Kind | Description |
|-----------|------|-------------|
| `(%jit-set-active enable)` | function | Enable/disable JIT translation for subsequent definitions |
| `(%jit-active-p)` | function | Whether JIT translation is currently on |
| `(%jit-set-frames enable)` | function | Native functions push a call frame so the backtrace sees them (on by default; off also stops native-to-native direct calls) |
| `(%jit-frames-p)` | function | Whether native call frames are on |
| `(%jit-disassemble fn)` | function | Disassemble a function's native m68k code |
| `(%jit-invoke-count)` | function | How many times the JIT path was entered |
| `(%jit-set-direct-calls enable)` | function | Enable/disable direct native-to-native calls (on by default; `AMILISP_JIT_DIRECT=0` starts with them off) — for A/B runs and bisecting |
| `(%jit-direct-call-stats)` | function | Call-site counters as a plist: `:fills`, `:misses`, the refusal reasons, `:gen`, `:enabled` |
| `(%trace-function name)` / `(%untrace-function name)` / `(%untrace-all)` | function | Low-level tracing primitives behind `trace`/`untrace` (`name` is a symbol) |
| `(%get-gc-count)` | function | Number of GCs performed (diagnostics) |

```lisp
(amilisp::%jit-set-active nil)   ; define the next function bytecode-only
(defun foo (x) (* x x))
(amilisp::%jit-set-active t)
```

> Everything else exported from `AMILISP` (the `%…` and `*…*` names) is internal
> plumbing for `lib/boot.lisp` / `lib/clos.lisp` and the compiler — not a public
> API and subject to change.

## Source of truth

Float bits: `tests/test_float.c`. Package-local nicknames: `tests/test_package.c`.
`structurep`: `tests/test_struct.c`. Type expansion: `tests/test_type_expander.c`.
JIT toggles/coverage:
`tests/amiga/test-jit.lisp`. See also the [JIT](../README.md#jit-m68k-and-aarch64) section of
the main README.
