# Modularizing amilisp to Reduce Binary Size

Status: design notes / proposal (2026-05-21). Not yet implemented.

Goal: reduce the AmigaOS binary size (and eventually peak RAM) by making
optional subsystems excludable at build time and/or loadable on request,
rather than baking everything into one monolithic executable.

## Where the bytes actually are

The Amiga binary is **~497 KB text, ~580 KB on disk — pure C**. The ~280 KB of
`boot.fasl` / `clos.fasl` / `asdf` is **not embedded**; it is loaded from `lib/`
on demand via `require` (see `cl_module_require` path in
`src/core/builtins_io.c` and the boot load in `src/core/repl.c`). So the Lisp
library is already modular. The C core is the size target.

Reliable object-size buckets (from `build/cross/*.o`):

| Subsystem | ~Size | Isolation today |
|---|---|---|
| JIT (`jit` + `runtime` + `asm_m68k` + `codebuf`) | ~42 KB | already behind `JIT_M68K` |
| Numeric tower (`bignum` + `float_math` + `ratio` + `float`) | ~42 KB | none |
| Threads/MP (`builtins_thread` + `thread` + `platform_thread_amiga`) | ~18 KB | none |
| Introspection (`inspect` + `describe` + `debugger`) | ~16 KB | none |
| `format` | ~10 KB | none |
| FFI | ~4 KB | none |

Note: the interactive debugger (`debugger.c`) is only ~4 KB — a poor *size*
target on its own, but an excellent *pattern* candidate (see Strategy 2). The
real weight is JIT, the numeric tower, and threads.

The `vm_trace_dump` symbol appears as ~27 KB in `nm --size-sort`; this is an
artifact of the hunk-format binary attributing the `static cl_vm_run` dispatch
loop to the preceding global symbol. Trust the `.o` sizes, not per-symbol nm
sizes.

## Section garbage-collection — TRIED, net loss (do NOT redo)

Tested 2026-05-21: added `-ffunction-sections -fdata-sections` to `CFLAGS` and
`-Wl,--gc-sections` to `LDFLAGS` in `Makefile.cross`, clean-rebuilt.

The linker **does** support and run it — `--print-gc-sections` confirmed it
removed dozens of genuinely-unreferenced functions (`cl_bignum_from_int32`,
`cl_macroexpand_1`, `cl_princ`, `cl_read`, `cl_repl_init`, …). But the binary
got **bigger**, not smaller:

| | on-disk | text | bss |
|---|---|---|---|
| baseline | 580,820 | 497,408 | 113,492 |
| + gc-sections | 616,148 | 509,280 | 113,492 |
| delta | **+35,328 (+6.1%)** | +11,872 | 0 |

Root cause: this is an ELF-oriented optimization that backfires on the AmigaOS
**hunk** executable format. `-ffunction-sections` puts every function in its own
section; the linker can only GC at section granularity, so you must pay
per-function **alignment padding** + **per-section hunk-table metadata** for
hundreds of tiny sections. That overhead exceeds the dead code reclaimed (and
text rises even though sections were dropped). Reverted; a note is left in
`Makefile.cross` so it is not re-attempted.

Implication: real size wins must come from *not compiling* code in (Strategy 1)
or *not linking* it (Strategy 2/3) — section-level dead-stripping is not viable
on this toolchain/format.

## Three modularization strategies, ranked for this architecture

### 1. Compile-time feature flags → build variants (cheap, reliable, recommended)

The codebase already has a clean seam: every subsystem registers through a
`cl_*_init()` / `cl_register_builtins(table, count, pkg)` call (see
`src/core/builtins.c`). Wrap each init call + its `Makefile.cross` source entry
in `#ifdef CL_FEATURE_X`, exactly like `JIT_M68K` already does
(`src/jit/jit.c`, gated init, `cl_jit_enabled()`).

Ship e.g. `amilisp` (full) and `amilisp-min` (no JIT/threads/FFI/introspection).

Caveats — not all are cleanly severable:
- `bignum` / `ratio` are reached on integer overflow; dropping them needs a
  fallback (signal an error on overflow) rather than a silent removal.
- `format` is used internally for error/condition messages.
- JIT, threads, FFI, inspect/describe are the clean cuts.

This yields smaller binaries but is a **build-time** choice — not "load on
request."

### 2. Push optional features down into Lisp `require`-modules (best fit for "load on request")

This is the mechanism the system already uses (`require` loads FASL/Lisp from
`lib/`), and it is the honest answer to the debugger example. The interactive
debugger is mostly a *UI*: print condition, walk backtrace, list restarts,
read-eval loop. The primitives it needs already exist in C (`ext:backtrace`,
`ext:frame-locals`). So:

- shrink `debugger.c` to the thin primitive hooks,
- move the interactive loop to `lib/debugger.lisp`,
- lazily `(require "debugger")` only when an unhandled error actually drops into
  it.

The same shape works for `describe` / `inspect` (introspection UIs over
primitive accessors) and arguably the pretty-printer. Result: genuine on-demand
loading, **portable to host**, no Amiga dynamic-linking machinery. Cost: those
paths run as bytecode (slower, +arena RAM while loaded) — acceptable, since none
are hot paths.

### 3. True runtime dynamic loading of C code (heavy — probably not worth it)

On AmigaOS the options are `.library` (LVO jump table + own data segment),
linker overlays, or hand-rolled `LoadSeg()` plugins. All collide badly with this
design: a single global arena, global VM/symbol state, and **arena-relative
offset pointers**. A `.library` boundary does not share globals or the arena
cleanly; you would pass a host vtable into each plugin and hand-manage
relocation. High effort, fragile, Amiga-only. Avoid unless a specific feature
*must* be C *and* *must* be optional at runtime.

## Recommended sequence

(Section garbage-collection was tried first and rejected — see above. Real wins
require build-time exclusion or runtime loading.)

1. Generalize the `JIT_M68K` pattern into `CL_FEATURE_*` flags for the
   clean-cut subsystems (JIT, threads, FFI, inspect/describe); add a `min`
   build target. This is the highest-confidence size win.
2. For "load on request," adopt Strategy 2: move the debugger UI to
   `lib/debugger.lisp` as the pilot, keeping only primitive hooks in C. If it
   works well, do `describe` / `inspect` next.
