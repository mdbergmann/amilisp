# Benchmark Results

Point-in-time performance measurements for future reference. Newest entries
first. Each entry records the commit, environment, exact reproduction
command, and results, so later runs can be compared like-for-like.

Related: [specs/performance.md](../specs/performance.md) is the optimization
*plan*; this file is the *measured results* log.

## 2026-10-21 — cross toolchain gcc 6.5.0b → GCC 16.2 (AmigaPorts v16.2-rc18): bytecode rows 2.6% slower at `-Os`

**Context**: the m68k toolchain moved from the 2025-10 gcc 6.5.0b build to
upstream's release set v16.2-rc18 (GCC 16.2.1b, binutils 2.46, libnix of
2026-10-18); same `Makefile.cross` flags (`-Os -fomit-frame-pointer`).
`.text` is 820,996 bytes against 821,748.

**Environment**: FS-UAE, 68040 JIT config, `--no-image --heap 24M`,
`trunk/bench-opt.lisp` with `*bo-scale*` 1/5 and `*bo-set-size*` 150
(JIT off by the suite's design), A = gcc 6 binary of the same tree, B =
GCC 16 binary, one boot running A B A B.  Best of the two rounds per
binary, ms; the clock ticks in 20 ms steps, so rows under 500 ms carry
one tick of noise.

| row                    | A gcc 6 | B GCC 16 | B/A  |
|------------------------|--------:|---------:|-----:|
| vm.fixnum-loop         |   2960  |   3000   | 1.01 |
| vm.local-shuffle       |   1860  |   1880   | 1.01 |
| vm.call-return         |   2860  |   3020   | 1.06 |
| alloc.cons-churn       |   1820  |   1840   | 1.01 |
| alloc.mixed-churn      |   5100  |   5080   | 1.00 |
| clos.make-instance     |   4000  |   4200   | 1.05 |
| clos.accessor          |    440  |    440   | 1.00 |
| kw.call-8keys          |   5360  |   5420   | 1.01 |
| mt.call-x8             |  24080  |  25100   | 1.04 |
| mt.dynbind-x8          |  26160  |  27180   | 1.04 |
| set.intersection-equal |   2160  |   2280   | 1.06 |
| set.intersection-small |   1680  |   1820   | 1.08 |
| set.union-small        |   2520  |   2640   | 1.05 |
| opt.const-fold         |   1280  |   1220   | 0.95 |
| opt.dead-branch        |   2100  |   2040   | 0.97 |
| safety0.call-args      |   2900  |   2880   | 0.99 |
| struct.typep           |    360  |    400   | 1.11 |

Geometric mean over all 32 rows: 1.026.  The C builtins (`set.*`,
`struct.typep`, `clos.make-instance`) and the call path lose 4 to 8%, the
opcode loops are within a tick, the compiler-side rows gain.  Every row
checked `ok` on both binaries.

**`-O2` with GCC 16** (same tree, `DEBUG_FLAGS="-O2"` after the `-Os`, so
it wins; `BUILDDIR=build/cross-o2`; A = the `-Os` GCC 16 binary, B =
`-O2`, A B A B in one boot, best of two): geometric mean 0.805 over all
34 rows, every row between 0.72 and 0.87, every result `ok`.  `.text`
grows from 820,996 to 1,030,120 bytes (+25%).  The `-O2` ban in
`Makefile.cross` records a gcc 6 miscompile of flexible-array pointer
arguments; the `volatile` workarounds for it are still in the tree.  Not
adopted by this entry: it needs the Amiga gates on the `-O2` binary and a
decision on the code size for 8 MB machines.

| row                    | A `-Os` | B `-O2` | B/A  |
|------------------------|--------:|--------:|-----:|
| vm.fixnum-loop         |   3040  |   2580  | 0.85 |
| vm.local-shuffle       |   1900  |   1600  | 0.84 |
| vm.call-return         |   3000  |   2560  | 0.85 |
| alloc.cons-churn       |   1840  |   1600  | 0.87 |
| alloc.mixed-churn      |   4980  |   3700  | 0.74 |
| clos.slot-value        |    800  |    620  | 0.78 |
| clos.make-instance     |   4100  |   3500  | 0.85 |
| kw.call-8keys          |   5380  |   4080  | 0.76 |
| compile.file-plain     |   3860  |   2840  | 0.74 |
| mt.call-x8             |  24860  |  21500  | 0.86 |
| set.intersection-equal |   2260  |   1780  | 0.79 |
| opt.const-fold         |   1220  |    880  | 0.72 |
| safety1.call-args      |   2900  |   2320  | 0.80 |

**Reproduce**: build the reference with
`make -f Makefile.cross amiga TOOLCHAIN=<old prefix> BUILDDIR=build/cross-gcc6`,
then a boot-override (`verify/realamiga/call-on-ustartup`) that runs each
binary with `--no-userinit --no-image --heap 24M --non-interactive --load
<file>` where the file sets the two knobs and loads the suite (the knobs
cannot go on an AmigaDOS command line: `*` is its escape character).

---

## 2026-10-03 — m68k JIT direct calls into `&key` callees: an `&key` leaf call 8.6 → 3.1 µs

**Context**: `specs/jit-direct-calls.md`, "&key callees": an `&key`
function takes the positional native ABI (count in D1, arguments above A6),
so a native call site fills for it and calls it directly instead of going
through the helper.

**Environment**: FS-UAE, 68040 JIT config, `make -f Makefile.cross amiga`.
A = 9a6a2132 (master), B = this change, `--no-image`, A B A B in one boot
(`ab-bench.sh` pattern: the boot-override runs both binaries interleaved).

**`trunk/bench-jit-call.lisp`** (200k calls per row; new row "call &key
leaf" = `(bjc-key-add s :b 1)` into `(a &key (b 1))`):

| row                   | A bc / JIT µs | B bc / JIT µs |
|-----------------------|---------------|---------------|
| call native leaf      | 13.3-13.4 / 0.90-1.0 | 13.3-13.6 / 1.0 |
| call `&optional` leaf | 16.8-16.9 / 1.3-1.4 | 16.9-17.1 / 1.3 |
| call `&key` leaf      | 19.7-19.8 / 8.6 | 19.8-20.2 / 3.1-3.2 |

The other rows are unchanged.

## 2026-10-03 — m68k JIT `&rest` prologue: an `&rest` leaf call 3× faster than bytecode

**Context**: `specs/jit-direct-calls.md`, "&rest callees": the m68k walker
compiles `&rest` (a helper conses the list), and direct calls fill for
positional `&rest` callees.  Same change: the trampoline arm's argument
copy left the C stack (1040 -> 36 bytes per native call into a generic
function).

**Environment**: FS-UAE, 68040 JIT config, `make -f Makefile.cross amiga`.
A = 33e2c186 (master), B = this change, `--no-image`, one boot per run.

**Clean-heap probe** (B, 50k calls per row, fresh process, the first round):
a native caller into the native `(a &rest r)` leaf with one extra argument
5.2-5.6 µs, a bytecode caller 16.8-17.6 µs; a fixed-arity native leaf that
conses the same cell itself (`(car (list b))`) 7.6-8.0 µs.

**`trunk/bench-jit-call.lisp`** (200k calls per row, A, B, A, B):

| row                   | A bc / JIT µs | B bc / JIT µs |
|-----------------------|---------------|---------------|
| call native leaf      | 13.4 / 0.90-1.0 | 13.4-13.7 / 1.0-1.1 |
| call `&optional` leaf | 17.0-17.3 / 1.4 | 17.0-17.2 / 1.3-1.4 |
| call `&rest` leaf     | 20.4-21.7 / 46.6-62.1 | 20.5-20.7 / 31.9-47.0 |

The `&rest` row's JIT column is timed after its bytecode column has consed
200k cells, and on the 68040 a cons gets dearer the longer a process has
consed -- interpreted or native alike: in the probe, a pure-bytecode consing
loop went 20 -> 60 µs per call within one run, and neither `ext:gc` nor
`ext:gc-compact` brought it back.  Cause not yet investigated.  The other
rows are unchanged.

## 2026-10-03 — m68k JIT `&optional` prologue: an `&optional` leaf call 12× faster

**Context**: `specs/jit-direct-calls.md`, "&optional callees": the m68k
walker compiles `&optional` (the count comes in D1, the prologue copies the
arguments into the frame), and direct calls fill for such callees.

**Environment**: FS-UAE, 68040 JIT config (`verify/realamiga/verify.fs-uae`),
`make -f Makefile.cross amiga`.  A = 483711c6 (`feat/a64-optrest`), B = this
change.  One boot, run alternately A, B, A, B with `--no-image`.

**Command**: `amilisp --no-userinit --no-image --heap 8M --non-interactive
--load trunk/bench-jit-call.lisp` (200k calls per row).

| row                     | A bc / JIT µs | B bc / JIT µs |
|-------------------------|---------------|---------------|
| call bytecode leaf      | 13.0 / 12.0   | 12.9-13.2 / 12.0 |
| call native leaf        | 13.2 / 0.90   | 13.1-13.3 / 0.90 |
| call `&optional` leaf   | 16.4-16.5 / 16.3-16.4 | 16.5-16.8 / 1.3-1.4 |
| funcall native leaf     | 11.7-11.8 / 1.0-1.1 | 11.6 / 1.0-1.1 |

The `&optional` leaf (`(a &optional (b 1))`, one argument) stayed bytecode
in A.  The `moveq #nargs,d1` every call site now carries costs the native
leaf nothing measurable.

## 2026-10-02 — AArch64 JIT `&optional`/`&rest` prologues: an `&optional` leaf call 5× the interpreter

**Context**: `specs/native-backend-a64.md`, "&optional and &rest": the
walker compiles every lambda list, so a function with `&optional` or `&rest`
parameters no longer stays bytecode.

**Environment**: Apple M-series, macOS, `make host`, branch
`feat/a64-optrest` on b10b3d1d.

**Command**: `./build/host/amilisp --no-userinit --heap 8M --non-interactive
--load trunk/bench-jit-call.lisp`, three runs.

| row (200k calls)        | bc ms | JIT ms |
|-------------------------|-------|--------|
| call native leaf        | 6     | 1      |
| call `&optional` leaf   | 7-8   | 1-2    |
| funcall native leaf     | 5-6   | 1      |

The `&optional` leaf (`(a &optional (b 1))`, called with one argument) was
equal before (8 ms, interpreted); it now costs what a required-argument leaf
does.  The default runs as the compiler's own bytecode prologue, compiled
like any code.

## 2026-10-02 — AArch64 JIT direct calls: native-to-native calls 2.3×, call-return 6× the interpreter

**Context**: `specs/native-backend-a64.md`, phase 5: a call site whose cell
holds the current call generation and a native callee enters it directly,
pushing the interpreter's frame inline, instead of going through
`vmstack_dispatch` / `cl_jit_invoke`.

**Environment**: Apple M-series, macOS, host build (`make host`), branch
`feat/a64-direct-calls`.  One binary, direct calls on vs off
(`AMILISP_JIT_DIRECT=0`), three interleaved rounds, medians.

`trunk/bench-jit-call.lisp` with `(defparameter cl-user::*bjc-n* 2000000)`,
ms per 2 M iterations:

| Row                      | bytecode | direct off | direct on |
|--------------------------|---------:|-----------:|----------:|
| call native leaf         | 65 | 25 | 11 |
| call same-state leaf     | 63 | 26 | 11 |
| funcall native leaf      | 58 | 24 | 12 |
| call bytecode leaf       | 64 | 53 | 52 |
| call &optional leaf      | 80 | 71 | 70 |
| builtin 2-arg (logtest)  | 68 | 17 | 18 |
| builtin gethash          | 102 | 54 | 54 |
| decode-key mix (native helper) | 273 | 153 | 148 |

`trunk/bench-opt.lisp` (JIT on, eager; command as in the phase-2 entry):

| Row (ms)          | interpreter | direct off | direct on |
|-------------------|------------:|-----------:|----------:|
| vm.call-return    | 32 | 13 |  5 |
| mt.call-x8        | 34 | 13 |  6 |
| safety1.call-args | 32 | 20 | 14 |
| kw.call-8keys     | 63 | 27 | 23 |
| clos.make-instance| 46 | 34 | 30 |

Every other row within 1 ms.  Calls into builtins and interpreted
functions miss on every call and are unchanged; `mt.call-x8` shows the
per-thread miss counters add no cross-thread traffic.

## 2026-10-02 — AArch64 JIT phase 3: `&key` calls 2.2×, dynamic binding 3×, CLOS slot reads 2×

**Context**: `specs/native-backend-a64.md`, phase 3: the NLX frames,
dynamic binding, multiple values, closures and `&key` functions compile,
and every native call pushes its `CL_Frame` (backtraces and line
attribution see native frames).

**Environment**: Apple M-series, macOS, host build (`make host`).
Interpreter = the phase-3 binary with `--no-jit`; phase 2 = 92940e5b built
in a worktree; phase 3 = branch `feat/a64-jit-p3`.  Three interleaved
rounds, medians.  Command as in the phase-2 entry below.

| Row (ms)                 | interpreter | phase 2 | phase 3 | phase 3 vs interp |
|--------------------------|------------:|--------:|--------:|------------------:|
| mt.dynbind-x8            | 35 | 34 | 11 | 3.2× |
| kw.call-8keys            | 58 | 58 | 26 | 2.2× |
| clos.make-instance       | 33 | 39 | 32 | 1.0× |
| clos.slot-value          |  8 |  8 |  4 | 2.0× |
| clos.accessor-write      |  6 |  6 |  3 | 2.0× |
| clos.slot-value-deep     |  8 |  8 |  4 | 2.0× |
| struct.slot-value        |  8 |  8 |  4 | 2.0× |
| struct.accessor          |  4 |  4 |  1 | 4.0× |
| vm.call-return           | 31 | 11 | 11 | 2.8× |
| vm.fixnum-loop           | 42 |  3 |  3 | 14×  |

The other rows are within 2 ms of phase 2.  The frame push costs nothing
measurable: phase 3 with `%JIT-SET-FRAMES NIL` gives the same rows
(call-return 13 vs 11 ms with frames, noise).

## 2026-10-02 — AArch64 JIT phase 2: a fixnum loop 14× the interpreter, call-return 3×

**Context**: `specs/native-backend-a64.md`, phase 2: inline fast paths
(fixnum `+ - *` and comparisons, `CAR`/`CDR`, struct slots, special
variables) with out-of-line slow paths, and a three-register top-of-stack
cache.  Phase 1 sent every opcode to its helper.

**Environment**: Apple M-series, macOS, host build (`make host`).
Interpreter = the phase-2 binary with `--no-jit`; phase 1 = master
f20f45d2 built in a worktree; phase 2 = branch `feat/a64-jit-p2`.  Three
interleaved rounds, best of each.

**Command**: `trunk/bench-opt.lisp` forces the JIT off, so a copy with the
`(amilisp::%jit-set-active nil)` line removed is loaded, eager:

```
sed 's/^(amilisp::%jit-set-active nil)$//' trunk/bench-opt.lisp > /tmp/bo-jit.lisp
AMILISP_JIT_HOT=0 ./build/host/amilisp --heap 64M --no-userinit --non-interactive [--no-jit] --load /tmp/bo-jit.lisp
```

| Row (ms)                 | interpreter | phase 1 | phase 2 | phase 2 vs interp |
|--------------------------|------------:|--------:|--------:|------------------:|
| vm.fixnum-loop           | 43 | 17 |  3 | 14.3× |
| vm.local-shuffle         | 21 |  7 |  2 | 10.5× |
| vm.call-return           | 32 | 21 | 10 |  3.2× |
| mt.call-x8               | 33 | 22 | 12 |  2.8× |
| opt.const-fold           | 14 |  9 |  7 |  2.0× |
| opt.dead-branch          | 23 | 15 | 12 |  1.9× |
| safety1.svref-loop       | 25 | 11 |  3 |  8.3× |
| safety1.call-args        | 32 | 23 | 17 |  1.9× |
| set.intersection-large   | 44 |  8 |  6 |  7.3× |
| set.union-large          | 64 | 12 | 10 |  6.4× |
| alloc.cons-churn         | 22 | 10 |  5 |  4.4× |
| alloc.mixed-churn        | 52 | 38 | 36 |  1.4× |

The `kw.*`, `clos.*`, `struct.*`, `type.*`, `mt.dynbind-x8` and `compile.*`
rows are unchanged within 1 ms: their time is in `&key` prologues,
dynamic binding and generic dispatch, which the walker still declines
(phase 3).  The spec's stop criterion -- 1.5× on the `vm.*` rows -- is met
with 3-14×.

`trunk/bench-jit-call.lisp` (`--heap 8M`), JIT ms per row, phase 1 → phase
2: builtin LOGTEST 3 → 1, call native leaf 4 → 2, fixnum `case` 4 → 2,
decode-key mix (native helper) 15 → 14.  No row is slower than the
interpreter; the `&optional` leaf stays equal (8 ms, still interpreted --
compiled since the `&optional`/`&rest` entry above).

## 2026-10-01 — JIT direct native-to-native calls: a call to a native leaf 6.2 → 0.9 us (FS-UAE 040)

**Context**: `specs/jit-direct-calls.md`.  A call site in native code now
jumps straight into a native callee whose positional ABI the call fits.
One generation word guards all the call-site cells.  Every other callee
takes the helper as before.

**Environment**: FS-UAE A4000/68040 (`verify/realamiga/verify.fs-uae`),
the cross-built `build/cross/amilisp` of branch `spec/jit-direct-calls`,
`--no-image --heap 8M`, `stack 128000`.  One boot runs three interleaved
rounds of the same binary, alternating the default with
`AMILISP_JIT_DIRECT=0` (the kill switch).  Medians, µs per iteration
including the loop, 200,000 iterations per row.

| Row                              | bytecode | JIT, direct off | JIT, direct on |
|----------------------------------|---------:|----------------:|---------------:|
| loop only                        |  7.6 |  0.5 |  0.5 |
| builtin 2-arg (LOGTEST)          | 12.8 |  4.1 |  4.1 |
| builtin 1-arg (HASH-TABLE-P)     | 12.6 |  3.5 |  3.5 |
| builtin GETHASH                  | 21.7 | 13.1 | 13.0 |
| call bytecode leaf               | 13.5 | 12.4 | 12.4 |
| call native leaf                 | 13.6 |  6.2 |  0.9 |
| call &optional leaf              | 17.3 | 16.9 | 16.9 |
| call same-state leaf             | 13.3 |  6.2 |  0.9 |
| FUNCALL native leaf              | 12.1 |  5.8 |  1.1 |
| local BLOCK/RETURN-FROM          | 13.6 |  3.7 |  3.7 |
| fixnum CASE (8 keys)             | 23.7 |  6.2 |  6.2 |
| FFI PEEK-U16                     | 11.5 |  3.9 |  3.9 |
| decode-key mix (native helper)   | 56.2 | 37.1 | 34.7 |
| decode-key mix (bytecode helper) | 56.1 | 39.2 | 39.2 |

Net of the loop, a native call costs 0.4 µs, against 5.7 through the
helper and 13.1 in the interpreter.  The first build checked the fill rule
*before* dispatching, which made every builtin call 0.2 µs slower with
direct calls on.  It also cancelled the mix's gain (native helper 39.5 vs
39.7) and made the bytecode-helper mix 7% slower (45.1 vs 42.3).  Moving
the fill into the dispatcher's native arm removed that, as the table shows.
The same change made the miss path look up the thread once instead of
twice, which is why the "off" column also beats the 2026-09-16 numbers.

The Clamacs per-key spike (`clamacs/spike/run-spike.sh 040`: 1,051 keys,
44 RETs) ran in four boots, on / off / on / off (`SPIKE_SETENV=
AMILISP_JIT_DIRECT=0` for off).  Per key, median / p90: 321 / 385 and
383 / 385 µs on, against 385 / 449 and 392 / 449 µs off.  RET median: 32.9
and 31.5 ms on, against 32.8 and 33.2 ms off.  The spike's clock ticks in
about 64 µs steps, so the per-key gain is about one tick at p90.  Explicit
full GCs are equal.

**Vampire V4: not measured yet** (the box was offline).

**Reproduce**: build with `make -f Makefile.cross amiga`, then boot with a
boot-override (see `verify/realamiga/call-on-ustartup`).  Per round it
runs `amilisp --no-userinit --no-image --heap 8M --non-interactive --load
trunk/bench-jit-call.lisp` twice, once plain and once after `setenv
AMILISP_JIT_DIRECT 0`, with `unsetenv` after it.

---

## 2026-09-28 — sento matrix for 0.11: five cells hold 0.10, pinned/tell −15% (open), sento 3.5.0 +2% to +132%

**Context**: the full matrix on `80572be7` (the 0.11 tree a week after
its bump), same protocol as the 0.10 entry — cold cache, speed 3, the
bench's default queue cap, sento 3.4.5 pinned — with a same-session A/B
against a binary built from the 0.10 bump (`814c7017`), and both binaries
again on sento 3.5.0. Full entry:
[sento-bench-results-0.11.md](sento-bench-results-0.11.md).

| Cell | 0.11 (3.4.5) | 0.10 binary (3.4.5) | 0.11 vs 0.10 | 0.11 (3.5.0) | 3.5.0 vs 3.4.5 |
| --- | ---: | ---: | ---: | ---: | ---: |
| pinned/tell | 287,984 | 325,110 | −11.4% (interleaved: −15%, 9 of 9 pairs) | 370,121 | +28.5% |
| pinned/ask-s | 135,934 | 142,576 | −4.7% (warm repeat +1.5%) | 138,258 | +1.7% |
| pinned/ask | 33,440 | 33,755 | −0.9% | 77,706 | +132% |
| shared/tell | 121,895 | 122,712 | −0.7% | 254,813 | +109% |
| shared/ask-s | 69,404 | 69,828 | −0.6% | 106,330 | +53.2% |
| shared/ask | 28,171 | 28,698 | −1.8% | 61,506 | +118% |

**Finding**: the 0.11 cycle left the host message path alone, and five
cells say so. Pinned/tell is 15% under the 0.10 binary in nine of nine
interleaved single-cell pairs while every row underneath it reads equal
or faster on 0.11 — all 34 `bench-opt` rows (the `vm.*` rows 4–10%
faster, `safety1.svref-loop` −20% from the AREF opcode), all 40
`bench-prims` rows (locks, condvars, handler-case, struct access, calls),
a contended eight-producer lock-and-condvar probe, and the GC telemetry.
An interleaved bisect (three pairs per step) gives a slope of +10% →
+5.5% → +1.6% → −5% → −2% across commits #24, #30, #31, #33, #36 of 48,
inside the probe's ±5% resolution, and the commits it brackets do not run
on the path; the entry argues for a cache-line layout effect (38 writable
statics differ between the binaries) and leaves it open. sento 3.5.0
(batch dequeue under one lock, direct dispatch into workers, futures
resolved from the reply, notify-when-waiting queues) doubles the two
async cells and shared/tell on either binary, so the 0.11 / 3.5.0 column
is the one to carry forward.

Reproduce: as the 0.10 entry, plus `ATOMICS_DIR=<sento worktree>` per
leg; single-cell pairs = the matrix driver with five `run-cell` lines
removed, one process per run, alternating binaries.

---

## 2026-09-22 — JIT compiles when hot: boot 720 → 520 ms, native code for boot + editor 857 KB → 35 KB

**Context**: the m68k JIT compiled every function at definition, which
cost load time and memory for code that mostly runs once, and a heap
image (native code is dropped on restore) was never compiled again.  Now
a function compiles on its 8th interpreted call, on its first when it
loops, and at definition under `(optimize (speed 3))`; `--jit-eager` is
the old policy.  Design: `specs/lazy-jit.md`.

**Environment**: FS-UAE A4000/68040 (`verify/realamiga/verify.fs-uae`),
`--heap 8M`, `stack 128000`, `--no-userinit`, FASL caches warm (the
editor's files compiled by a warm-up run first).  One cross-built binary
with the change, the policy picked on the command line; "0.11" is the
snapshot binary of `86f7b0e4` from its release tree, same session.
Milliseconds unless noted, two runs each (they agreed within 40 ms).

| | hot (default) | `--jit-eager` | `--no-jit` | 0.11 |
|---|--:|--:|--:|--:|
| boot, `--boot-log` "ready" | 520 / 540 | 720 / 720 | 580 / 560 | 700 |
| native code after boot | 10.8 KB (42 fns) | 245 KB | 0 | |
| loading the editor (`clamacs/lisp/load.lisp`) | 1380 / 1380 | 1900 / 1880 | 1460 / 1460 | |
| native code after boot + editor | 35 KB (114 fns) | 857 KB | 0 | |
| 20,000 generic-function calls | 620 / 600 | 600 / 620 | 840 / 880 | |
| 1,000 MAKE-INSTANCE | 680 / 660 | 660 / 660 | 740 / 780 | |
| 20,000 GF calls, restored from an image | 620 / 600 | | 860 / 880 | (as `--no-jit`) |
| 1,000 MAKE-INSTANCE, restored from an image | 740 / 680 | | 780 / 800 | (as `--no-jit`) |
| `trunk/bench-jit-loop.lisp`, calls/s | 192,308 / 191,571 / 194,553 | 194,175 / 193,798 / 193,798 | | 193,798 / 194,553 / 193,050 |

The hot policy boots faster than `--no-jit`: the few functions that turn
hot during CLOS's load (42) pay for their compile.  A restored image now
runs native code again (0.11's images ran as bytecode for good, the
`--no-jit` column); the native code a workload compiles is proportional to
what it runs — 21.8 KB for the CLOS rows after a restore.

`trunk/bench-jit-call.lisp` (`*bjc-n*` 50000, per iteration, bytecode →
JIT column): unchanged against `--jit-eager` and 0.11 — native leaf
13.6 → 5.6 us, fixnum CASE 23.6 → 6.0, decode-key mix 59.2 → 35.2.  The
first measurement run of the change had read *no* JIT gain in this file:
its thunk runs twice, so it stays interpreted, and the VM's native fast
path skipped tail calls — the thunk's tail call into the compiled driver
ran the driver as bytecode.  Tail calls from bytecode into native code
now call and return (`specs/lazy-jit.md`, "Tail calls from bytecode").

**Reproduce**: a boot-override script (see `verify/realamiga/make-image.sh`
for the pattern) running, per policy, `amilisp --no-userinit --no-image
--heap 8M --non-interactive --boot-log [--jit-eager|--no-jit]` with a
file that prints `(amilisp::%jit-native-bytes)`,
`(amilisp::%jit-hot-compile-count)` and the timings.

---

## 2026-09-16 — string-scan fast path: a character scan 3-7× faster on real hardware

**Context**: item 3 of the Clamacs-in-Lisp runtime cycle
(`clamacs/specs/clamacs-lisp.md`).  The spike measured 18-33 us per
character for an editor's indent scan on a Vampire V4; `(schar s i)` and
`(char= c #\x)` were builtin calls and a `CASE` key test two dispatches on
the miss path.  Five opcodes (`AREF`, `CHAREQ`, `CMP_BR`, `PUSH_LOCAL`,
`POP_LOCAL`), a `CASE` branch shape and leaner `PUSH`/`INCF`/`DECF`
expansions; design in `specs/performance.md` 4.4, JIT templates in
`specs/native-backend.md` "Status (2026-09-16, evening)".  The declared
scan's ordinary character went from 41 dispatches plus a builtin call to
26 dispatches.

**Environment**: Vampire V4 (Kickstart 47.115), `--heap 8M`, `stack
128000`, JIT on.  Before = `07cab514`, after = this change, both
cross-built and run from identically laid-out trees (the repo's `lib/`,
lib/amiga compiled into the FASL cache, warm).  Host: the Mac, `make host`
of both commits, same session, alternating runs, minimum taken.

**Scan only** (`trunk/bench-scan.lisp` through
`verify/realamiga/run-on-vampire.py`; three scans over a 1,007-char Lisp
text, 50 repetitions; the Amiga clock ticks in 20 ms, so a cell
resolves to about 0.4 us).  Microseconds per character:

| scan | before | after |
|------|-------:|------:|
| naive (`CHAR`, `COND` of `CHAR=`, no declarations) | 18.3 | 2.4-2.8 |
| declared state machine (`SCHAR`, `CASE`, fixnum declarations) | 7.6 | 2.0-2.4 |
| bench-general `scan-parens` | 8.3 | 2.0-2.4 |

In two of the four "after" runs one of the three functions ran 2.5-3×
slower than in the other runs, for the whole run, with the same binary and
the same code; which function was hit moved when an unrelated allocation
changed (the Lisp file's name).  That is a native-code placement effect on
this machine (the item-2 entry saw a 2 us version of it), not dispatch
work; compare builds over several runs.

**The spike** (`clamacs/spike/run-vamp.py`, two runs per binary):

| | before | after |
|---|---:|---:|
| per key, median | 651 / 634 us | 661 / 622 us |
| RET, median | 48.5 / 51.2 ms | 35.9 / 32.0 ms |
| RET: naive indent scan + indentation | 20.9 / 22.1 ms | 10.2 / 8.2 ms |
| RET: declared scan + indentation | 11.6 / 12.0 ms | 9.7 / 8.5 ms |
| full GC, first / later (warm run) | 354 / 68 ms | 357-377 / 65 ms |

The scan itself is now about 1.5 ms of a 657-char buffer; the remaining
~7 ms of each "indent" cell is the indentation decision after it (a
`POSITION-IF` with a closure, `SUBSEQ`, `MEMBER :test STRING-EQUAL` over
28 names) — editor code on generic sequence functions, the next thing to
look at on the Lisp side.  Per key and GC are unchanged.  (A first A/B ran
the old binary from the 0.10.0 release drawer and showed a 139 ms first
GC against 325-427 ms for the new one; from identical trees both take
~355 ms.  The first-collection cost depends on how the library was laid
out and loaded, not on the binary.)

**Host** (`trunk/bench-general.lisp`, min of 2; the new `string-scan` row
is `scan-parens` over 400 generated defuns, 5 passes):

| row | before | after | ECL | SBCL |
|-----|-------:|------:|----:|-----:|
| char-loop | 104 | 66 | 4.2 | 2.0 |
| string-scan | 26 | 16 | 3.7 | 0.6 |
| insertion-sort | 104 | 76 | | |
| float-vector | 61 | 50 | | |
| struct-bst | 60 | 55 | | |
| total (32 rows) | 3,683 | 3,596 | | |

`trunk/bench-opt.lisp` (min of 3): `vm.fixnum-loop` 50 → 43 ms,
`safety*.svref-loop` 29 → 26, `alloc.cons-churn` 26 → 21; no row slower
by more than 1 ms, `vm.*` and `mt.*` included.

## 2026-09-16 — JIT call dispatch: a JIT'd call to a native leaf 18.8 → 7.0 us on real hardware

**Context**: the Clamacs-in-Lisp spike (`clamacs/specs/clamacs-lisp.md`,
phase 0) found the m68k JIT making call-heavy generic code slower than the
interpreter (per keystroke 1.04 vs 0.71 ms on a Vampire V4).  Every call
from native code went through the generic `cl_vm_apply` trampoline and, for
a Lisp callee, a stub interpreter frame; `jit_dispatch` (`src/jit/runtime.c`)
now enters builtins, FFI stubs and native callees directly, and interpreted
callees through the stub frame without the copies and probes.  Design and
the full tables: `specs/native-backend.md`, "Status (2026-09-16)".

**Environment**: Vampire V4 (real 68040-class FPGA, Kickstart 47.115) and
FS-UAE A4000/68040 (`verify/realamiga/verify.fs-uae`), `--heap 8M`,
`stack 128000`, the cross-built `build/cross/amilisp` before (`c920aecd`)
and with the change.  Per-iteration cost in microseconds, 200,000
iterations per row, bytecode vs JIT columns from the same run.

| Row                            | Vampire bc | Vampire JIT before → after | FS-UAE bc | FS-UAE JIT before → after |
|--------------------------------|-----------:|---------------------------:|----------:|--------------------------:|
| loop only                      |  7.3 |  0.4 →  0.4 |  7.7 |  0.5 →  0.5 |
| builtin 2-arg (LOGTEST)        | 12.2 | 12.0 →  6.0 | 12.8 |  6.9 →  3.7 |
| builtin 1-arg (HASH-TABLE-P)   | 11.9 |  5.9 →  2.9 | 12.8 |  5.5 →  3.2 |
| builtin GETHASH                | 27.5 | 29.1 → 14.3 | 21.7 | 19.1 → 12.6 |
| call native leaf               | 11.8 | 18.8 →  7.0 | 11.8 | 13.4 →  5.1 |
| call bytecode leaf             | 12.5 | 19.5 → 12.7 | 13.7 | 15.2 → 12.1 |
| call &optional leaf            | 16.1 | 25.1 → 18.2 | 17.5 | 19.4 → 17.8 |
| FUNCALL native leaf            | 12.9 | 18.5 →  6.8 | 12.1 | 13.1 →  4.9 |
| local BLOCK/RETURN-FROM        | 13.0 |  6.3 →  3.3 | 13.7 |  5.8 →  3.5 |
| fixnum CASE (8 keys)           | 26.9 | 12.1 →  6.3 | 28.1 |  9.1 →  6.3 |
| FFI PEEK-U16                   | 10.9 |  7.1 →  3.2 | 11.8 |  6.7 →  3.6 |
| decode-key mix (native helper) | 69.2 | 89.4 → 50.0 | 59.7 | 59.4 → 34.2 |
| decode-key mix (bytecode helper) | 63.2 | 90.7 → 53.1 | 57.1 | 60.8 → 36.8 |

(The bytecode column moves 0-10% between runs; the "after" columns are
the committed binary's, each the last of several agreeing runs.  An
intermediate build that kept the trampoline's 1 KB argument copy in the
dispatcher's frame read 4.9 us on the native-leaf row on the Vampire;
moving it out -- needed for recursion depth -- costs that row 2 us there
and nothing on FS-UAE or on the decode-key mix, a stack-layout cache
effect of the machine.  The build actually committed, which re-derives
the callee's bytecode pointer after the safepoint, reads 7.7 / 3.6 /
43.2 us on the native-leaf / LOGTEST / decode-key rows: on the Vampire a
row moves by 2-3 us between layouts of the same code, so compare builds
on the mix rows and on the spike, not on one leaf row.)

The spike that raised the item (`clamacs/spike/run-vamp.py`, the per-key
Emacs layer over TextEditor.mcc), re-run on the Vampire with the change,
JIT on vs `--no-jit` in one session: per key 658 vs 689 us median (p90
1094 vs 1105), RET 50 vs 88 ms, explicit full GC 131 / 55 vs 133 / 50 ms.
Before the change the same spike read 1043 vs 706 us per key.  The
suite's own call loop (`trunk/bench-jit-loop.lisp`, two million calls
from a JIT'd caller into two native leaves, run after every
`test-amiga`) reads 214,133 calls/s on the FS-UAE 68040 config against
69,930 on 2026-08-09.

**Reproduce**:

```
make -f Makefile.cross amiga
# real hardware through amiagent (pushes the cross binary beside the release):
verify/realamiga/run-on-vampire.py trunk/bench-jit-call.lisp
# FS-UAE: a boot-override that runs the file, see verify/realamiga/call-on-ustartup
```

---

## 2026-09-10 — sento matrix for 0.10, refreshed after the heap-word locks: +50% to +360% per cell over 0.8

**Context**: the full matrix of the entry two below, re-run on `04a2a410`
(master with the heap-word locks, PR #37) with the same protocol —
cold cache, speed 3, the bench's default queue cap — and a same-session
A/B against the 0.10 tree before the lock change (`6989f7aa`), the 0.9
bump and the 0.8 bump. Full entry, now the 0.10 figures of record:
[sento-bench-results-0.10.md](sento-bench-results-0.10.md).

| Cell | 0.10 (`04a2a410`) | pre-lock 0.10 (`6989f7aa`) | 0.8 | vs pre-lock | vs 0.8 |
| --- | ---: | ---: | ---: | ---: | ---: |
| pinned/tell | 302,005 | 270,318 | 165,239 | +11.7% | +82.8% |
| pinned/ask-s | 132,478 | 110,296 | 88,050 | +20.1% | +50.5% |
| pinned/ask | 35,421 | 36,419 | 22,460 | −2.7% | +57.7% |
| shared/tell | 122,427 | 41,636 | 26,629 | +194% | +360% |
| shared/ask-s | 63,594 | 75,171 | 36,496 | −15.4% | +74.2% |
| shared/ask (default cap) | 29,747 | aborts: `lock table full` (28,558 at cap 2000) | 17,569 | (+4.2%) | +69.3% |

Every cell runs at the standard configuration; the lock-table abort the
entry two below found is gone with the table. **Finding**: the table had
been forcing collections. A slot was reclaimed only by the collector, so
at 90–130k lock allocations per second (a lock and a condition variable
per synchronous ask, three locks per asynchronous one) `MAKE-LOCK` hit a
full table every 0.1–0.2 s and ran a minor collection each time — 215
collections and 1.22 s of stop-the-world in the pre-lock pinned/ask-s
cell against 24 and 0.007 s now; the "collector's share went up in STW
time" observation of the first 0.10 matrix was this, not Tier 4. The
shared/tell drain overhead is gone too (47–52 s wall for six 5-second
iterations on every older binary, 31.5 s now). shared/ask-s is −15% in
the matrix leg for the `cl_thread_list_lock` reason the entry below gives.

Reproduce: as below; the driver's `ATOMICS_DIR` now takes `:`-separated
directories, so a base binary's leg can pin both a sento checkout and the
bordeaux-threads fork at `92113d6`.

---

## 2026-09-10 — MP locks as heap words: shared/ask runs at the default cap, shared/tell +321%

**Context**: [specs/mp-locks-heap-words.md](../specs/mp-locks-heap-words.md)
landed — a lock is a state word in the heap object, blocking parks each
thread on its own handle, no OS mutex and no lock table.  Same-session
A/B against a detached worktree of the pre-lock tree (`6989f7aa` plus the
bench commit of the entry below; both sit inside the `04a2a410` squash),
sento 3.4.5, cold caches, speed 3, the driver's default queue cap (10000).
The base binary's leg loads the bordeaux-threads fork's committed
version via `ATOMICS_DIR` (its `ACQUIRE-LOCK` takes two arguments).

| Cell | table locks (pre-lock tree) | heap-word locks | Δ |
| --- | ---: | ---: | ---: |
| pinned/tell | 243,587 | 299,260 | +22.9% |
| pinned/ask-s | 108,365 | 121,118 | +11.8% |
| pinned/ask | 55,995 | 55,311 | −1.2% (re-measured: +1..5%, noise) |
| shared/tell | 41,791 | 176,126 | +321% |
| shared/ask-s | 68,005 | 58,086 | −14.6% (re-measured twice: −4%, −6%) |
| shared/ask (default cap) | aborts: `lock table full` | 45,424 | runs |

shared/tell is the thundering herd gone (eight workers on one queue
lock were broadcast awake on every release; now one waiter wakes per
release).  shared/ask-s pays for the process-wide `cl_thread_list_lock`
that every condition wait and notify now takes; the spec's
"Implementation notes" record the per-object waiter chain that would
remove it.  `trunk/bench-prims.lisp` (min of 5): `lock-acquire-release`
55 → 52 ns, `lock+condvar-notify` 67 → 62 ns, every other row within a
nanosecond; `mt.call-x8` / `mt.dynbind-x8` unchanged at 35 / 36 ms.

Reproduce: `AMILISP_FASL_CACHE_DIR=<fresh dir> AMILISP_FORCE_SPEED=3
./build/host/amilisp --no-userinit --heap 192M --non-interactive --load
trunk/sento-bench-matrix.lisp`, one leg per binary, sequentially.

---

## 2026-09-10 — sento matrix for 0.10: +17% to +99% per cell over the 0.8 binary

**Context**: the full reply-mode / dispatcher matrix of the sento series,
re-run on master ahead of the 0.10 bump with the 0.8 protocol (cold cache,
speed 3) and a same-session A/B against binaries built from the 0.9 and
0.8 bump commits. Full entry: [sento-bench-results-0.10.md](sento-bench-results-0.10.md)
— **superseded the same evening** by the refreshed matrix two entries
above (the lock change landed; the doc now carries both columns).

| Cell | 0.10 | 0.8 (same session) | Δ |
| --- | ---: | ---: | ---: |
| pinned/tell | 274,098 | 170,779 | +60.5% |
| pinned/ask-s | 108,575 | 92,692 | +17.1% |
| pinned/ask | 33,419 | 23,841 | +40.2% |
| shared/tell | 39,056 | 27,808 | +40.4% |
| shared/ask-s | 72,277 | 36,314 | +99.0% |
| shared/ask (queue cap 2000) | 28,678 | 18,617 | +54.0% |

0.9 and 0.8 read the same; the whole delta is Tier 4 (the three entries
below). **Finding**: shared/ask at the bench's default queue cap (10000)
aborts on 0.10 with `MP:MAKE-LOCK: lock table full (max 16384)` — each
in-flight async ask holds three locks (waiting actor's box, its queue,
the future), and the faster sender side now keeps more than ~5,400 of
them in flight. The fixed table (`CL_MAX_LOCKS`) is the limit, not the
slot scan (≈3 µs per `MAKE-LOCK` at 15k live locks); a growable table or
a larger host cap with a free list is the follow-up.

---

## 2026-09-09 — General workloads: amilisp vs ECL vs SBCL

**Context**: the first comparison that is not sento-shaped.
`trunk/bench-general.lisp` times 31 WORKLOADS — the Gabriel shapes (tak,
fib, deriv, n-queens), sorting and list processing, hash tables, strings and
string streams, the reader, the printer, FORMAT, fixnum / float / bignum
arithmetic, arrays, CLOS, structs, conditions, LOOP, allocation churn — and
runs unchanged on amilisp, ECL and SBCL.  Every row returns an integer that
depends on all of its work, printed as `val=`; the three implementations
must agree on it (they do, except for the bignum rows — see finding 5).
Master at `8d8e89b3` (0.9 + Tier 4 phases 1–3), `make host`.  ECL 26.5.5
(Homebrew, native `compile-file` output) and SBCL 2.6.8 (Homebrew, `--load`
compiles natively).  All three at `(optimize (speed 1) (safety 1) (debug
1))`, the setting third-party code is compiled with by default.

**Environment**: Apple M3 Ultra, macOS 26.6.2, `--heap 64M`; amilisp compiled
the file from source into a scratch FASL cache each run
(`AMILISP_FASL_CACHE_DIR`).  Each row is the minimum of 5 in-process
repetitions, and each implementation ran twice in separate processes, one
after the other on a quiet machine (the table takes the lower of the two;
amilisp's runs agree within 2% on every row).  A `AMILISP_FORCE_SPEED=3` run
matched speed 1 on every row.

| row | amilisp | ECL | SBCL | ECL/amilisp | SBCL/amilisp |
| --- | ---: | ---: | ---: | ---: | ---: |
| tak | 122.0 | 33.0 | 16.3 | 3.70× | 7.48× |
| fib | 87.0 | 44.2 | 15.3 | 1.97× | 5.69× |
| nqueens | 71.0 | 20.8 | 14.1 | 3.41× | 5.04× |
| deriv | 115.0 | 34.3 | 8.6 | 3.35× | 13.37× |
| list-sort | 108.0 | 123.2 | 68.2 | 0.88× | 1.58× |
| list-ops | 80.0 | 44.2 | 9.2 | 1.81× | 8.70× |
| hof | 71.0 | 35.3 | 8.2 | 2.01× | 8.66× |
| assoc | 213.0 | 7.8 | 12.4 | 27.31× | 17.18× |
| getf | 11.0 | 7.6 | 5.9 | 1.45× | 1.86× |
| vector-sort | 1054.0 | 6.4 | 3.0 | 164.69× | 351.33× |
| insertion-sort | 99.0 | 29.5 | 11.9 | 3.36× | 8.32× |
| matmul | 62.0 | 34.7 | 12.4 | 1.79× | 5.00× |
| mandel | 49.0 | 69.1 | 8.8 | 0.71× | 5.57× |
| float-vector | 59.0 | 49.7 | 0.7 | 1.19× | 84.29× |
| bignum-fact | 53.0 | 20.4 | 9.0 | 2.60× | 5.89× |
| bignum-arith | 181.0 † | 6.4 | 14.3 | 28.28× | 12.66× |
| hash-fixnum | 45.0 | 36.2 | 12.0 | 1.24× | 3.75× |
| hash-string | 77.0 | 9.0 | 2.9 | 8.56× | 26.55× |
| string-ops | 64.0 | 86.2 | 10.3 | 0.74× | 6.21× |
| string-stream | 65.0 | 89.9 | 16.9 | 0.72× | 3.85× |
| char-loop | 103.0 | 3.9 | 2.0 | 26.41× | 51.50× |
| format | 52.0 | 685.5 | 46.4 | 0.08× | 1.12× |
| reader | 84.0 | 53.7 | 32.5 | 1.56× | 2.58× |
| printer | 58.0 | 100.9 | 76.9 | 0.57× | 0.75× |
| clos-dispatch | 73.0 | 33.0 | 6.1 | 2.21× | 11.97× |
| make-instance | 84.0 | 61.7 | 0.4 | 1.36× | 210.00× |
| struct-bst | 59.0 | 34.1 | 8.5 | 1.73× | 6.94× |
| fixnum-loop | 170.0 | 85.0 | 12.9 | 2.00× | 13.18× |
| loop-collect | 84.0 | 26.9 | 10.9 | 3.12× | 7.71× |
| conditions | 61.0 | 339.7 | 10.2 | 0.18× | 5.98× |
| alloc-churn | 79.0 | 54.2 | 12.4 | 1.46× | 6.37× |
| **total** | **3593** | **2266** | **480** | **1.59×** | **7.49×** |
| **geomean** |  |  |  | **2.22×** | **8.62×** |

Milliseconds; a ratio above 1 means amilisp is slower.  † amilisp's
bignum-arith cell is the re-measurement on the fixed binary (finding 5);
the first run's 33 ms was the bug's early bail-out.

**Headline**: ECL is 1.59× faster in total and 2.22× by geometric mean, but
the mean is carried by three pathological rows.  Without vector-sort, assoc
and char-loop the geometric mean is 1.59× and the totals are at parity
(amilisp 2,223 ms vs ECL 2,248 ms), because ECL's FORMAT and condition
signalling are so slow.  The plain interpretive tax of the VM against native
code is 2–3.7× (fixnum-loop 2.0, fib 2.0, tak 3.7, nqueens 3.4, deriv 3.4,
insertion-sort 3.4, loop-collect 3.1).  amilisp beats ECL on seven rows:
format 13×, conditions 5.6×, printer 1.7×, mandel 1.4×, string-stream 1.4×,
string-ops 1.35×, list-sort 1.14×.  SBCL is 7–8× ahead overall; its
make-instance (210×) and float-vector (84×) rows are its constructor
optimisation and unboxed double-float arrays.

**Findings** (ranked by what a fix would buy):

1. **vector-sort 165×** — `bi_sort` (`builtins_sequence2.c`) sorts lists
   with a merge sort but vectors with `vector_insertion_sort`, O(n²) with a
   predicate call per comparison: 20,000 elements take 1,054 ms as a vector
   and 108 ms as a list.  `stable-sort` shares the path.  A merge sort for
   vectors (or sort-through-a-list) is the fix.
2. **assoc 27× while getf is 1.45×** — `lib/boot.lisp` overrides the C
   `ASSOC` with a Lisp function (keyword parsing, then `funcall #'eql` per
   pair) to add `:test-not`/`:key`; every ASSOC in user code pays ~34 ns per
   list element.  Same shape for RASSOC, SUBLIS, SUBST, NSUBST.  Delegating
   to the C builtin when no keyword is supplied (or a compiler macro) would
   recover the 27×.
3. **char-loop 26×** — `char` and `char=` are builtin calls, not opcodes;
   the loop body is six calls per character.
4. **hash-string 8.6×** — EQUAL hash tables with string keys, 10,000 keys,
   50,000 operations.
5. **bignum division is WRONG above 4,095 bits** — `bignum.c:238` divides
   through fixed 16-bit-limb scratch buffers (`u_buf[256], v_buf[128]`,
   commented "should be enough for practical use"); a dividend of 4,096 bits
   (128 32-bit words, `(expt 2 4095)` and up) or a divisor above 2,048 bits
   silently yields quotient 0 and remainder = dividend, and `gcd` inherits
   it.  `logand`/`logior`/`logxor` (`bignum.c:1864`) truncate at 2,048 bits
   the same way.  `(mod (factorial 600) 1000003)` is 0 instead of 471663;
   400! is still right.  Multiplication is unaffected (`integer-length` of
   1000! is the correct 8,530), so bignum-fact's timing is the intended
   workload and only its `val` is wrong; bignum-arith's `floor` bails out
   early, so that amilisp cell is not a measurement.  **Fixed the same
   day** (heap scratch above the stack buffers, streaming LOGCOUNT /
   LOGBITP; `tests/test_bignum.c` large-operand tests).  The two rows
   re-measured on the fixed binary, `val` now agreeing with ECL and SBCL:
   bignum-fact 54 ms (unchanged), bignum-arith 181 ms — 28× behind ECL,
   whose integers are GMP's, because amilisp's Knuth division runs on
   16-bit limbs (the multiplication already packs to 32-bit limbs on the
   host).  A 32-bit-limb division is the follow-up if bignum-heavy code
   ever matters.
6. **bignum-fact 2.6×**, hash-fixnum 1.24×, alloc-churn 1.46×, matmul 1.8×,
   struct-bst 1.7×, clos-dispatch 2.2×: the runtime's C paths sit close to
   ECL; the rows dominated by bytecode do not.

**Reproduce**:

```
./build/host/amilisp --no-userinit --heap 64M --non-interactive --load trunk/bench-general.lisp
AMILISP_FORCE_SPEED=3 ./build/host/amilisp --no-userinit --heap 64M --non-interactive --load trunk/bench-general.lisp
ecl --norc --eval '(progn (load (compile-file "trunk/bench-general.lisp" :output-file "/tmp/bench-general.fas")) (quit))'
sbcl --non-interactive --no-userinit --no-sysinit --load trunk/bench-general.lisp
# Amiga: (defparameter cl-user::*bg-scale* 1/20) before loading
```

---

## 2026-09-09 — Tier 4 phase 3 landed: superinstructions, the peephole at every speed

**Context**: results for [specs/performance.md](../specs/performance.md) 4.3.
The peephole pass runs at every speed above 0 now (it was `speed >= 2`),
fuses fourteen adjacent opcode pairs and triples into one dispatch each,
and gained three rewrites on the way (the dead result-slot store before a
RET, a JMP whose target is a RET, a RETURN-FROM site returning in place).
Three binaries measured in the SAME session on a quiet machine: the item-5
tree (`17a20059`, a `git worktree`), the phase-3 tree with
`AMILISP_NO_FUSE=1` (the pass at speed 1 with its rewrites, no fusion —
"nofuse"), and the phase-3 tree.

**Environment**: Apple M3 Ultra, macOS 26.6.2, `--heap 64M` for the
microbench, `--heap 192M` for sento; every bench-prims run compiled the
file from source (the FASL cache entry was deleted before each run),
minimum of 5 interleaved runs per binary.

**Which opcode pairs to fuse** came from an adjacent-pair profile
(`PROFILE_OPCODES` builds count pairs now; `(amilisp::%op-counts-dump)`
prints the top 48), taken on the sento pinned/tell path at one producer,
4 s, before any fusion: 856.8M dispatches, 1,245 per message.  The top
fusable pairs — STORE→POP 10.8%, POP→LOAD 8.4%, LOAD→LOAD 5.2%,
LOAD→CALL_GLOBAL 4.8%, LOAD→RET 2.8%, LOAD→STRUCT_REF 2.7%,
LOAD→MV_RESET 2.6%, EQ→JNIL 1.9%, GLOAD→JNIL 1.6% — became the first nine
opcodes; `CONST; EQ; JNIL` from the plan does not occur on this path
(`GLOAD; EQ; JNIL`, the slot-protocol marker test, does).  A second
profile of the fused stream (1,044 per message) chose the five of round
two: LOAD→CONST 2.3%, GLOAD→CALL_GLOBAL 2.4%, GLOAD→EQ→JNIL 2.1%,
LOAD→STORE→POP 2.0%, POP→LOAD 1.8%.  After round two: 854.4M dispatches for 877k messages, **974 per message** (−22%).  The top remaining pairs are dynamic successors (JNIL→NIL, GLOAD_EQ_JNIL→LOAD_MV_RESET, a builtin call's result stored) or headed by a peeked STORE (STORE→GLOAD_EQ_JNIL 2.3%, STORE→CONST 1.7%) — the next round, if one is ever worth it, starts there.

**Per-primitive** (`trunk/bench-prims.lisp`, ABSOLUTE ns = net + the
empty-loop baseline).  The baseline itself moved: `(dotimes (i n) (setq
acc i))` is 25.0 → 22.0 (nofuse) → 16.0 ns per iteration, because its
body `LOAD i; STORE acc; POP` is one `LOAD_STORE_POP` now (the separate
7-run probe agrees: 25.8 → 17.3 ns), so about 9 ns of every row's delta
is loop overhead and the rest is the row's own.

| row | item 5 | nofuse | phase 3 | delta |
| --- | ---: | ---: | ---: | ---: |
| fixnum-add | 28 | 25 | 24 | −4 |
| call-1arg | 41 | 35 | 27 | −14 |
| call-3arg | 48 | 43 | 33 | −15 |
| call-&key-2of3 | 67 | 60 | 49 | −18 |
| call-&rest-2 | 67 | 60 | 51 | −16 |
| call-mvbind | 65 | 59 | 47 | −18 |
| flet-call | 50 | 43 | 42 | −8 |
| funcall-closure | 40 | 38 | 33 | −7 |
| closure-cell-incf | 59 | 53 | 45 | −14 |
| apply-3list | 59 | 53 | 45 | −14 |
| gf-1arg-1method | 76 | 64 | 57 | −19 |
| gf-2arg-2methods | 86 | 73 | 65 | −21 |
| gf-around+primary | 478 | 391 | 379 | **−99** |
| gf-call-next-method | 486 | 389 | 381 | **−105** |
| accessor-read | 31 | 28 | 23 | −8 |
| accessor-write | 35 | 33 | 28 | −7 |
| slot-value-read | 62 | 56 | 39 | −23 |
| slot-value-write | 61 | 59 | 48 | −13 |
| with-slots-incf | 136 | 127 | 94 | **−42** |
| struct-read | 27 | 24 | 22 | −5 |
| struct-write | 28 | 26 | 26 | −2 |
| struct-push-pop | 61 | 61 | 56 | −5 |
| make-instance-2init | 2,752 | 2,200 | 2,180 | **−572** |
| make-struct-2init | 52 | 51 | 37 | −15 |
| cons | 32 | 28 | 24 | −8 |
| list-4 | 65 | 60 | 55 | −10 |
| closure-alloc | 41 | 38 | 28 | −13 |
| special-read | 27 | 23 | 20 | −7 |
| special-bind | 37 | 34 | 28 | −9 |
| handler-case | 54 | 46 | 37 | −17 |
| handler-bind | 51 | 41 | 32 | −19 |
| unwind-protect | 80 | 68 | 61 | −19 |
| catch-throw | 55 | 49 | 43 | −12 |
| typep-class | 51 | 48 | 39 | −12 |
| case-keyword | 66 | 57 | 44 | −22 |
| gethash-eq | 39 | 36 | 29 | −10 |
| svref | 47 | 44 | 37 | −10 |
| lock-acquire-release | 85 | 80 | 70 | −15 |
| lock+condvar-notify | 99 | 92 | 81 | −18 |

No row regressed.  The "nofuse" column separates the two halves: the
rewrites alone (and the old peephole finally running on default-speed
code) take the CLOS rows most — `gf-around+primary` 478 → 391 and
`make-instance-2init` 2,752 → 2,200 are all rewrites, the dispatch and
CLOS code being full of RETURN-FROM sites and block results that used to
cost a store, a jump and a reload each — while the fusion takes the
loop-shaped rows: `slot-value-read` 56 → 39, `with-slots-incf` 127 → 94,
`case-keyword` 57 → 44, `closure-alloc` 38 → 28.

**Acceptance cells** — sento pinned/tell.  The single-producer cell
(`trunk/sento-bench-loadthreads.lisp`, `SENTO_LOAD_THREADS="1"`,
`:num-shared-workers 8`, `:duration 5`, `:num-iterations 6`), the
consumer's own number per the item-5 entry, two runs per binary,
alternating:

| | AVG msg/s | MEDIAN | MIN | MAX | GC share |
| --- | ---: | ---: | ---: | ---: | ---: |
| item 5, run 1 | 278,237 | 259,190 | 255,166 | 363,462 | 0.9% |
| item 5, run 2 | 285,595 | 269,744 | 264,015 | 368,289 | 0.9% |
| phase 3, run 1 | 368,352 | 355,421 | 333,897 | 444,975 | 0.8% |
| phase 3, run 2 | 370,798 | 358,162 | 329,632 | 455,591 | 0.8% |
| | **+30%** | **+33%** | +28% | +23% | |

Against the item-5 entry's median of record (271k): 357k, +32%.  The
4-producer cell of the phase-1/phase-2 entries (`trunk/profile-sento-bench.lisp`:
`:load-threads 4`, `:duration 15`, `:num-iterations 8`), the tier's gate
since 4.0 (175k there), one run per binary:

| | AVG msg/s | MEDIAN | MIN | MAX |
| --- | ---: | ---: | ---: | ---: |
| item 5 | 219,969 | 198,458 | 187,940 | 259,727 |
| phase 3 | 234,646 | 211,342 | 202,075 | 310,292 |
| | **+6.7%** | +6.5% | +7.5% | +19% |

That cell measures the message-box hand-off between several producers
and one consumer (see the item-5 entry), so it moves far less than the
consumer-bound cell above; it is reported for continuity with the earlier
phases, not as the phase-3 gate.

**Gates**: `make test` (incl. `tests/test_tier4_phase3.sh`, 78 checks,
and the 47 cases of `tests/test_peephole.c`), `make test-gc-stress` (which
found the latent C-frame NLX bug — see the commit — and now carries its
regression case), `make test-memleak`, `make -f Makefile.cross test-amiga` (fresh boot 4615/4616 — the one miss is the known FS-UAE-only `audio-short-sample-completes` — and restored image 4616/4616; a `--no-jit` run of the suite through the boot-override hook passes everything but the 74 checks that expect native code, so the fused VM is right on m68k independently of the walker).

**Reproduce**: `git worktree add --detach /tmp/base 17a20059 && (cd
/tmp/base && make host)`; per run `find ~/.cache/common-lisp -name
bench-prims.fasl -delete` then `amilisp --no-userinit --heap 64M
--non-interactive --load trunk/bench-prims.lisp`, alternating the three
binaries (`AMILISP_NO_FUSE=1` for the middle column), minimum per row;
the sento cells as named above; the pair profile with `make host
BUILDDIR=build/host-opprof DEBUG_FLAGS=-DPROFILE_OPCODES`, a warm-up
iteration, `(amilisp::%op-counts-reset)`, one 4 s iteration at one
producer, `(amilisp::%op-counts-dump)`.

---

## 2026-09-08 — Tier 4 phase 2, item 5: non-escaping local functions inlined

**Context**: the follow-up to the phase-2 commit closes its open items
([specs/performance.md](../specs/performance.md) 4.2): a `flet`/`labels`
function the compiler proves non-escaping is compiled into each call site
instead of a closure; the m68k JIT gets a template for the handler-case
opcodes; the `OP_CALL_GLOBAL` constant-pool check moves under `DEBUG_VM`.
Three binaries measured in the SAME session on a quiet machine: the
phase-2 tree (`563983cc`, a `git worktree`), that tree plus every change
except the `vm.c` gating ("novm"), and the full tree.

**Environment**: Apple M3 Ultra, macOS 26.6.2, `--heap 64M`; every run
compiled `trunk/bench-prims.lisp` from source (the FASL cache entry was
deleted before each run), minimum of 5 interleaved runs per binary,
absolute ns (baseline 25.0 ns in all three).  Rows that moved by 3 ns or
more:

| row | phase 2 | novm | item 5 | delta |
| --- | ---: | ---: | ---: | ---: |
| flet-call | 73 | 49 | 49 | **−24** |
| gf-around+primary | 476 | 464 | 462 | −14 (spread ±15) |
| gf-call-next-method | 484 | 472 | 466 | −18 (spread ±15) |
| make-instance-2init | 2,716 | 2,779 | 2,704 | −12 (spread ±60) |

Every other row is within ±2 ns; the call rows (`call-1arg` 40, `call-3arg`
48, `funcall-closure` 40) are identical across the three binaries, i.e. the
`DEBUG_VM` gating of the constant-pool check is not measurable on the host
(the 68020 saves three compares per interpreted global call).  `flet-call`
— `(flet ((h (x) (if x i x))) (h i))` — is now the body alone: no closure
allocation on entry, no frame, no `OP_RET`.  The three heavy CLOS rows
moved less than their run-to-run spread and nothing on those paths is a
`flet` (`defmethod` expands to a `named-lambda`; `call-next-method` is a
global function), so they are not attributed to this change.

**A measurement trap worth recording**: a first pass of this table showed
every row +5–6 ns except the empty loop.  It was contention — a unit-test
binary was being rebuilt and run on the same machine during runs 2–5 —
and it looked exactly like a layout regression.  A source-compiled and a
FASL-loaded run of the same file also time the same (checked as part of
the rerun), so "FASL-loaded code is slower" is not a thing either.

**What the sento pinned/tell cell is bound by** (the question the phase-2
entry below left open).  `trunk/sento-bench-loadthreads.lisp` runs the
cell over a producer-count sweep, everything else as in the matrix
(`:num-shared-workers 8`, `:duration 5`, `:num-iterations 6`, warm cache,
`--heap 192M`, this binary, quiet machine):

| producers (`:load-threads`) | AVG msg/s | MEDIAN | MIN | MAX | GC share |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1 | 286,970 | 271,495 | 263,098 | 366,989 | 1.0% |
| 2 | 191,665 | 190,193 | 180,042 | 201,220 | 1.4% |
| 4 | 188,315 | 186,248 | 179,958 | 195,913 | 1.4% |
| 8 | 266,679 | 257,826 | 253,606 | 290,000 | 1.2% |

The rate does not grow with the producers — one producer already reaches
the highest rate, so the producer side is not the limit; the collector
is not either (1–1.4% of wall time, stop-the-world 0–52 ms per 31 s).
The consumer is: at one producer the actor thread handles a message every
~3.5 µs, which is what the phase-1 profile's ~1,450 bytecodes per message
cost at ~2.4 ns each — the message path is VM-bound, so phase 3's
superinstructions will move this number.  The dip at 2–4 producers (−35%)
is a queue hand-off effect, not VM work: with `:wait-if-queue-larger-than
10000` several producers alternately overfill and drain the one message
box, so the consumer sleeps and wakes on the condition variable per
burst; with one producer the queue stays populated, with eight it is
always full, and in both cases the consumer never sleeps.  This is
why the 4-producer configuration used as the phase-2 gate stayed flat
while the CPU time per run fell 10%: that cell measures the hand-off
pattern, not the consumer.  **Use `:load-threads 1` (or 8) as the phase-3
acceptance cell**; the 1-producer median (271k) is the consumer's number.

**Gates**: `make test` (incl. `tests/test_local_inline.sh`, 72 checks),
`make test-gc-stress`, `make test-memleak`, `make -f Makefile.cross
test-amiga` (see the follow-up commit message for the counts).

**Reproduce**: `git worktree add --detach /tmp/base 563983cc && (cd
/tmp/base && make host)`; then per run `find ~/.cache/common-lisp -name
bench-prims.fasl -delete` before `amilisp --no-userinit --heap 64M
--non-interactive --load trunk/bench-prims.lisp`, alternating binaries,
and take the minimum per row.

---

## 2026-09-08 — Tier 4 phase 2 landed: call and unwind protocol

**Context**: results for [specs/performance.md](../specs/performance.md) 4.2
(items 1, 2, 3, 4, 6, 7; item 5 — direct calls for non-escaping local
functions — is not in this commit).  Both binaries built with `make host`
from the same tree state; the "before" is a `git worktree` of the phase-1
tree (`80e4b838`), built and measured in the SAME session.

**Environment**: Apple M3 Ultra, macOS 26.6.2. `--heap 192M` for sento,
`--heap 64M` for the microbench.

**Per-primitive** (`trunk/bench-prims.lisp`, ABSOLUTE ns — the empty-loop
baseline included, since it moved by 2 ns between the two runs — minimum
of 5 runs per row).  Rows that moved by 3 ns or more:

| row | before | after | delta |
| --- | ---: | ---: | ---: |
| call-1arg | 51 | 43 | −8 |
| call-3arg | 58 | 51 | −7 |
| call-&key-2of3 | 79 | 71 | −8 |
| call-&rest-2 | 77 | 67 | −10 |
| call-mvbind | 78 | 70 | −8 |
| flet-call | 84 | 78 | −6 |
| gf-1arg-1method | 91 | 78 | −13 |
| gf-2arg-2methods | 99 | 87 | −12 |
| gf-around+primary | 539 | 485 | **−54** |
| gf-call-next-method | 541 | 490 | **−51** |
| accessor-read | 35 | 31 | −4 |
| slot-value-write | 67 | 64 | −3 |
| with-slots-incf | 149 | 142 | −7 |
| make-instance-2init | 3,284 | 2,844 | −440 |
| make-struct-2init | 98 | 55 | **−43** |
| list-4 | 70 | 64 | −6 |
| closure-alloc | 46 | 43 | −3 |
| handler-case | 118 | 55 | **−63** |
| handler-bind | 57 | 51 | −6 |
| unwind-protect | 123 | 81 | **−42** |
| catch-throw | 64 | 55 | −9 |
| typep-class | 54 | 51 | −3 |
| svref | 55 | 48 | −7 |
| lock-acquire-release | 134 | 89 | **−45** |
| lock+condvar-notify | 151 | 102 | **−49** |

No row regressed by 3 ns or more (the largest move up is `case-keyword`,
+2).  The empty loop is 26.0 ns before and after (`(dotimes (i n) (setq
acc i))`, 7 process runs each).

**Which item paid**: the fused `CALL_GLOBAL` and the OP_CALL slimming took
every call row 6–10 ns and the two plain gf rows 12–13; the setjmp-free
NLX frames and the value save stack took `unwind-protect` 123 → 81 and
both lock rows −45/−49 (`with-lock-held` is an unwind-protect); the
special-form `handler-case` took its row 118 → 55 (that row used to cons a
box and a closure and push three NLX frames per entry); the constructor
compiler macro took `make-struct-2init` 98 → 55; the list-based
`call-next-method` chains took the two `call-next-method` rows −51/−54.

**A layout trap, again**: the first build of the NLX work lost ~0.5 ns on
*every* dispatch (empty loop 26 → 31 ns) with no change to any hot handler.
The disassembly showed why: clang merges every `VM_DISPATCH` into one
shared indirect branch and materializes `thr + <offset>` there for each
per-thread field the handlers touch whose offset exceeds the load
immediate range; the 32 KB `dyn_stack` sat near the top of `CL_Thread`, so
every scalar after it (the NLX/handler/restart tops, `mv_count`, the
printer state) cost a stack reload plus an add per dispatch, and the NLX
work added two more.  Moving every large table to the end of the struct
recovered all of it (and is the reorder that ships).  See the comment on
the "Large per-thread tables" section of `thread.h`.

**Acceptance cell** — sento pinned/tell (`trunk/profile-sento-bench.lisp`:
`:load-threads 4`, `:duration 15`, `:num-iterations 8`, warm ASDF cache):

| | AVG msg/s | MEDIAN | MIN | CPU s per 122 s wall |
| --- | ---: | ---: | ---: | ---: |
| before (phase-1 tree) | 207,293 | 192,906 | 177,940 | 206.6 |
| after (phase 2) | 208,088 | 193,614 | 180,772 | 186.3 |
| | **+0.4%** | +0.4% | +1.6% | **−10%** |

The throughput did not move, while the CPU time burned per run dropped
10%.  A per-thread `sample` at steady state (10 s, after "BENCH STEADY
STATE BEGIN") shows no lock contention (`__psynch_mutexwait` ≈ 0 on every
thread) and several threads saturated in `cl_vm_run` — the consumer is
not the only busy thread, and the bench creates a fresh actor system per
iteration, so the busy set changes over the run.  What limits the rate
with this configuration (the producer side, allocation and stop-the-world
collections across 20+ threads, or the consumer) was not resolved here;
the per-thread breakdown is the place to start before gating phase 3 on
this cell.  A `:load-threads 1` variant would isolate the consumer.

**Gates**: `make test` (incl. `tests/test_tier4_phase2.sh`, 141 checks),
`make test-gc-stress`, `make test-memleak`, `make -f Makefile.cross
test-amiga` (fresh boot 4555/4556 — the one miss is the known FS-UAE-only
`audio-short-sample-completes` — and restored image 4556/4556).

**Reproduce**: as for phase 1 below (`git worktree add --detach /tmp/base
80e4b838`, min-of-5 bench-prims, the sento cell).

---

## 2026-09-08 — Tier 4 phase 1 landed: runtime taxes removed (+37.6% sento)

**Context**: results for [specs/performance.md](../specs/performance.md) 4.1
(the six runtime-tax items).  Both binaries built with `make host` from the
same tree state — the "before" column is a `git worktree` of the unmodified
`1e76a19d`, built and measured in the SAME session as the "after" column,
because this host drifts by more between sessions than several of these
items are worth.

**Environment**: Apple M3 Ultra, macOS 26.6.2. `--heap 192M` for sento,
`--heap 64M` for the microbench.

**Acceptance cell** — sento pinned/tell (`trunk/profile-sento-bench.lisp`:
`:load-threads 4`, `:duration 15`, `:num-iterations 8`, warm ASDF cache):

| | AVG msg/s | MEDIAN | MIN | µs per message (avg) |
| --- | ---: | ---: | ---: | ---: |
| before (`1e77a16d` tree) | 155,204 | 150,540 | 148,684 | 6.4 |
| after (phase 1) | **213,543** | 202,691 | 185,854 | 4.7 |
| | **+37.6%** | +34.6% | +25.0% | |

That is above the 20–25% the phase was scoped for.  ECL 26.5.5 remains the
reference at ~512k msg/s (2026-07-15 entry below); the gap is now ~2.4×
rather than ~2.9×.

**Per-primitive** (`trunk/bench-prims.lisp`, ns net of the empty-loop
baseline, **minimum of 5 runs** per row — a single run on this host varies by
up to 20% on the short rows, enough to hide or invent a 5ns change).  Only
rows that moved beyond ±2ns are listed; the full output is reproducible with
the command below.

| row | before | after | delta |
| --- | ---: | ---: | ---: |
| typep-class | 89 | 27 | **−62** |
| gf-2arg-2methods | 105 | 68 | **−37** |
| gf-call-next-method | 525 | 490 | −35 |
| with-slots-incf | 134 | 115 | −19 |
| gf-around+primary | 512 | 499 | −13 |
| lock+condvar-notify | 127 | 116 | −11 |
| call-&rest-2 | 55 | 47 | −8 |
| slot-value-write | 46 | 38 | −8 |
| lock-acquire-release | 109 | 101 | −8 |
| slot-value-read | 45 | 38 | −7 |
| list-4 | 49 | 42 | −7 |
| gf-1arg-1method | 69 | 63 | −6 |
| struct-push-pop | 39 | 33 | −6 |
| svref | 29 | 25 | −4 |
| cons | 8 | 5 | −3 |
| handler-case | 97 | 94 | −3 |
| make-instance-2init | 3,461 | 3,119 | −342 |

`typep-class` is now *below* ECL's figure for that row (51 net / 71 absolute).
The empty-loop baseline was 26 before and 25 after, so the absolute cost of a
row is `net + 25`.

**Which item paid**, from the incremental measurements taken as each landed:
`call_builtin` slimming moved every builtin-heavy row at once (svref 30→22,
gethash-eq 16→11, catch-throw 38→31, unwind-protect 98→87); the rooted cons
took `cons` 10→3 and `&rest` 58→47; the type code took `typep-class` 93→26;
the GF cache took `gf-2arg` 105→68; the slot cache took the `slot-value` and
`with-slots` rows.

**A layout trap worth recording**: the first version of the per-thread slot
cache sat in the middle of `CL_Thread` and cost a uniform ~3ns on *every*
call row — the VM's hot fields had been pushed onto different cache lines.
Moving the 1KB table to the end of the struct recovered all of it.  Same
class of sensitivity as the `vm.o`/LTO note in CLAUDE.md; the bench-prims
call rows are what catches it.

**Gates**: `make test`, `make test-gc-stress` (516/516), `make test-plus`
(host-cold-test 585/585), `make test-memleak` (5/5), and
`tests/test_tier4_phase1.sh` (34 checks, run in the fast tier and again under
`AMILISP_GC_STRESS=1`).

**Reproduce**:

```
# before/after microbench, minimum of N runs (single runs are too noisy)
for i in 1 2 3 4 5; do ./build/host/amilisp --no-userinit --heap 64M \
    --non-interactive --load trunk/bench-prims.lisp; done   # take the min per row
# acceptance cell
./build/host/amilisp --heap 192M --load trunk/profile-sento-bench.lisp
# a fair "before": build the unmodified commit in a worktree and measure it
# in the same session, not against a number from another day
git worktree add --detach /tmp/base <commit> && make -C /tmp/base host
```

---

## 2026-09-08 — Tier 4 baseline: sento vs ECL, profile shares, per-primitive costs

**Context**: baseline for [specs/performance.md](../specs/performance.md) Tier 4
("2× vs ECL").  Master at `1e76a19d` (0.9), `make host`.  ECL 26.5.5 from
Homebrew, sento 3.4.4 local checkout in both.

**Environment**: Apple M3 Ultra, macOS 26.6.2, `--heap 192M` for sento,
`--heap 64M` for the microbench.

**sento pinned/tell** (`trunk/profile-sento-bench.lisp`: `:load-threads 4`,
`:duration 15`, `:num-iterations 8`, warm ASDF cache, default speed):

| | AVG msg/s | µs per message |
| --- | ---: | ---: |
| amilisp | **174,597** (dev 13,258) | 5.7 |
| ECL (2026-07-15 entry below, 8 load threads) | 512k | 2.0 |

ECL could not be re-run today: its `ql:quickload` of the bench dependencies
fails compiling the current serapeum (`level0/hash-tables`), so the July figure
stands.  sento itself loads and compiles on ECL.

**Recompile** `(asdf:load-system "sento" :force t)`, warm dependency cache:
amilisp **0.39 s**, ECL **15.06 s**.

**Profile** (`sample` 25 s at steady state, on-CPU samples ≈ 38.7k): `cl_vm_run`
self 26,646 (**69%**); `call_builtin` self 1,880 + its `cl_symbol_name` 323 +
`platform_stack_headroom` 190 + TLS 120 (~7%); `bi_gethash` inclusive 2,302
(6%); `struct_slot_resolve` inclusive 2,179 of which `pthread_rwlock_rdlock`
729 (6%); `cl_symbol_value` 617; `typep_symbol` → `strcmp` 596; `cl_alloc`
496; `cl_gc_push_root`/`pop_roots` 719 (of which 223 under `cl_cons`);
`bi_gf_ic_emf` → `cl_intern_in` 73.

**Opcode counts** (`make host BUILDDIR=build/host-opprof
DEBUG_FLAGS=-DPROFILE_OPCODES`, pinned, 2 load threads, 4 s, 501,405 messages):
727,953,971 ops = **1,452 per message**, 138 calls per message
(CALL 54.9M, TAILCALL 13.1M, APPLY 1.5M).  Top rows: LOAD 18.9%, POP 12.4%,
STORE 10.8%, FLOAD 8.7%, CALL 7.5%, JNIL 5.7%, GLOAD 5.0% (72/message), CONST
4.6%, MV_RESET 3.3%, RET 3.0%, STRUCT_REF 2.5%, ASSERT_TYPE 0.74%
(10.8/message).

**Per-primitive cost** (`trunk/bench-prims.lisp`, 1M iterations per row, ns
net of the empty-loop baseline: amilisp speed 1 = 30.0, speed 3 = 24.0,
ECL = 20.1; ECL rows are native `compile-file` output):

| row | amilisp s1 | amilisp s3 | ECL |
| --- | ---: | ---: | ---: |
| fixnum-add | -1 | 3 | -5 |
| call-1arg | 18 | 20 | -7 |
| call-3arg | 27 | 31 | -8 |
| call-&key-2of3 | 44 | 48 | 3 |
| call-&rest-2 | 56 | 59 | 13 |
| call-mvbind | 50 | 54 | -9 |
| flet-call | 54 | 55 | -10 |
| funcall-closure | 14 | 20 | -6 |
| closure-cell-incf | 32 | 36 | -7 |
| apply-3list | 30 | 34 | 5 |
| gf-1arg-1method | 68 | 70 | 24 |
| gf-2arg-2methods | 104 | 111 | 31 |
| gf-around+primary | 529 | 551 | 79 |
| gf-call-next-method | 521 | 534 | 79 |
| accessor-read | 4 | 10 | -4 |
| accessor-write | 5 | 11 | -3 |
| slot-value-read | 45 | 47 | 1 |
| slot-value-write | 43 | 49 | 5 |
| with-slots-incf | 134 | 131 | 22 |
| struct-read | -1 | 1 | -8 |
| struct-write | 1 | 2 | -11 |
| struct-push-pop | 39 | 41 | 10 |
| make-instance-2init | 3,526 | 3,580 | 1,564 |
| make-struct-2init | 72 | 70 | 34 |
| cons | 8 | 13 | -4 |
| list-4 | 52 | 54 | 25 |
| closure-alloc | 20 | 24 | 21 |
| special-read | -1 | 1 | -12 |
| special-bind | 8 | 12 | -9 |
| handler-case | 104 | 98 | 52 |
| handler-bind | 29 | 30 | 20 |
| unwind-protect | 93 | 94 | 2 |
| catch-throw | 37 | 41 | 1 |
| typep-class | 88 | 95 | 51 |
| case-keyword | 40 | 40 | -10 |
| gethash-eq | 14 | 18 | -11 |
| svref | 30 | 32 | -11 |
| lock-acquire-release | 117 | 115 | 44 |
| lock+condvar-notify | 131 | 139 | 43 |

Negative values are loop-overhead noise (the row is free).  Speed 3 changes no
row beyond noise.

**Reproduce**:

```
./build/host/amilisp --no-userinit --heap 64M --non-interactive --load trunk/bench-prims.lisp
AMILISP_FORCE_SPEED=3 ./build/host/amilisp --no-userinit --heap 64M --non-interactive --load trunk/bench-prims.lisp
ecl --norc --eval '(progn (load (compile-file "trunk/bench-prims.lisp" :output-file "/tmp/bench-prims.fas")) (quit))'
# profile: run trunk/profile-sento-bench.lisp in the background, wait for
#   "BENCH STEADY STATE BEGIN", then: sample <pid> 25 -file out.sample
# opcode counts: PROFILE_OPCODES build, (amilisp::%op-counts-reset) before and
#   (amilisp::%op-counts-dump) after one run-benchmark call
```

---

## 2026-08-30 — 0.8 regression root causes: per-thread break-poll counter + vm.o without LTO

**Context**: [sento-bench-results-0.8.md](sento-bench-results-0.8.md) found
master 9–27% below a same-session 0.4 binary on every sento cell. Three
bisects (all on deterministic micro probes, not on the noisy sento cell):

1. `8f9e85f` — the Ctrl-C poll counter was a process-wide `static` bumped on
   every `OP_CALL`/backward `OP_JMP` → all threads bounce one cache line.
   **Fixed**: counter in `CL_Thread`.
2. `d467727` — an LTO code-generation artifact in the giant `cl_vm_run`:
   every opcode 15–25% slower single-threaded with identical bytecode.
   **Mitigated**: `vm.o` built with `-fno-lto` (Makefile), rest of the
   runtime keeps LTO.
3. `d467727` — a residual layout effect on the local-variable loops that
   survives no-LTO; structural (one giant function), left as is.

**Environment**: Apple M3 Ultra, macOS 26.6.2, `make host`.  Probes: 8
threads each running a call loop / special-bind loop (wall ns per op);
bench-opt rows (ms, best of 3, `--heap 64M`).

| Probe                      | 0.4 (`cfd2bab`) | master before | + counter fix | + vm.o no-LTO |
| -------------------------- | --------------: | ------------: | ------------: | ------------: |
| 8-thread call loop (ns/op) |  6.8 |  27–29 | **7.5** | 6.8 |
| 8-thread special-bind      |  7.5 |  25–27 | **8.1** | 7.6 |
| vm.local-shuffle (ms)      |   47 |     56 |    56 | **52** |
| vm.fixnum-loop             |   58 |     66 |    66 | **60** |
| vm.call-return             |   56 |     61 |    61 | **54** |
| mt.call-x8 (new row)       |    — |      — |    64 | **57** |
| mt.dynbind-x8 (new row)    |    — |      — |    48 | **44** |

sento matrix (msg/s, cold speed-3 cache): pinned/ask 24,256 → **28,976**
(0.4: 33,239), shared/ask 19,191 → **22,302** (25,466), pinned/tell
169,018 → 175,227 (191,254); full table in the 0.8 results doc.

**Guards**: the `mt.*` rows are the 8-thread twins of `vm.call-return` /
a dynamic-binding loop — a shared write on the call path shows as 3–4×
the single-thread figure (healthy: within ~1.5×). After touching `vm.c`,
compare the `vm.*` rows against this table.

**Reproduce**: `echo '(quit)' | ./build/host/amilisp --heap 64M --load
trunk/bench-opt.lisp`; sento: `trunk/sento-bench-matrix.lisp`.

---

## 2026-07-15 — Generational GC (host): sliding nursery + dirty-page tracking

**Branch**: `perf/gengc` (on top of the TLAB branch).  Design:
[specs/generational-gc.md](../specs/generational-gc.md).  Host collections
are now mostly MINOR cycles that trace only live young objects (survivor
list collected during mark — dead nursery space is never walked) plus the
old-space pages actually written since the last GC (mprotect write-watch);
survivors slide onto the old-space watermark.  Majors are full compactions.
Also in this round: GC-epoch dedup of redundant back-to-back stop-the-world
collections, and minor-first reclamation for the bounded lock/condvar/
thread handle tables (their exhaustion path used to force a full
collection — the dominant GC cost of the ask benchmark).

**Environment**: Apple M3 Ultra, macOS 26.5.2, `make host`, `--heap 192M`.
Benchmark: `sento.bench::run-benchmark` (`:num-shared-workers 8,
:load-threads 8, :duration 5, :num-iterations 6`).  A/B on the SAME binary
via `AMILISP_GENGC=0` (classic collector, incl. the epoch dedup).

Full 6-cell matrix (single session per collector, cells back-to-back;
GC share = total collector time / cell wall time from the telemetry):

| Cell (avg msg/s) | classic | GC share | gengc | GC share | delta |
| ---------------- | ------: | -------: | ----: | -------: | ----: |
| PINNED tell      | 178,503 |  2.2% | 180,859 | 1.3% | +1.3% |
| PINNED ask-s     |  76,904 | 21.5% | **91,230** | 3.6% | **+18.6%** |
| PINNED ask       |  28,081 | 11.0% | **31,238** | 2.0% | **+11.2%** |
| SHARED tell      |  26,949 |  2.5% | **31,531** | 3.8% | **+17.0%** |
| SHARED ask-s     |  45,690 |  6.2% | **51,233** | 1.7% | **+12.1%** |
| SHARED ask       |  18,305 |  6.2% | **24,601** | 1.4% | **+34.4%** |

(An earlier 2-cell run measured pinned/tell 164.2k→186.1k (+13%) and
pinned/ask 25.8k→32.5k (+26%) — run-to-run variance on the tell cell is
substantial; the paired A/B within one session is the meaningful signal.)

Shape of the win: minors run at ~1.8–6ms (126 minors vs 139 full sweeps in
the pinned/ask cell; full compactions dropped 128 → 6 once the lock/condvar
table exhaustion stopped forcing them).  Worst single stop-the-world pause
dropped 52ms → 8.6ms.  The reply-mode cells gain the most — they cons a
lock+condvar per future, which both churns the handle tables (classic: a
full sweep every ~0.25s, up to 21.5% of wall) and produces exactly the
short-lived garbage a nursery reclaims for free.

**Reproduce**: load `trunk/load-sento-bench.lisp`, then run
`sento.bench::run-benchmark` with the config above (per cell:
`:with-reply-p`/`:async-ask-p`); read `(ext:%gc-time-stats)` /
`(ext:%gengc-stats)` around the run; A/B with `AMILISP_GENGC=0`.

---

## 2026-07-14 — condition-hierarchy + deftype table hash indexes (TYPEP)

**Commit**: on top of `fd52e4a`. **Env**: macOS host, `make host` (-O3).

TYPEP on a symbol type spec probes the struct registry (indexed since
spec 3.1), then `cl_is_condition_type` and `cl_get_type_expander` — both
were linear alist walks, so every symbol TYPEP paid O(registered
conditions + deftypes) even when the answer was "no".  Real-world impact
(eta-hab on linux-arm64, diagnosed via `AMILISP_LOCK_DIAG` + in-container
gdb): the print-object hook runs one TYPEP per printed node, so a single
log4cl log line printing an actor/queue graph held the serialized appender
lock for **~60s per appender**, stalling main/watcher/timer threads 1–2
minutes per log event during the item-definition phase.  Both tables now
use the generic `CL_AlistIndex` open-addressing index (compiler.h), same
protocol as the struct registry index.

Repro: `./build/host/amilisp --heap 64M --load trunk/bench-opt.lisp`
(bench registers 100 conditions + 100 deftypes first).

| Benchmark (ms, best of runs) | before | after | delta |
|---|---|---|---|
| type.typep-cond-deftype | 40 | **15** | 2.7× |
| struct.typep (control, already indexed) | 16 | 16 | — |

The micro-bench understates the field case: eta-hab registers several
hundred condition types (every loaded library's `define-condition`s), and
the walk cost scales with that count while the indexed probe stays O(1).

## 2026-07-11 — MT publication barrier: no sento throughput cost (A/B)

**Commit**: `f45a91a` (barrier `17dee11` + CLOS registry sync `f45a91a`).

Regression check after the arm64 publication-barrier fix, which issues
`platform_memory_barrier()` before every struct-slot store when threads are
live — sento's message path is the most barrier-dense MT workload we have.
Controlled A/B: two binaries identical except the four barrier sites
(`17dee11` applied vs reverted), interleaved runs (barrier / no-barrier /
barrier / no-barrier) so thermal and session drift cancel, warm speed-3
ASDF cache, otherwise the docs/sento-bench-results-0.3.md long-run config
at 30 iterations (`:dispatcher :pinned`, `:with-reply-p nil`,
`:load-threads 8`, `:duration 10`).

**Environment**: Apple M3 Ultra, macOS 26.5.2, `--heap 192M`,
`AMILISP_FORCE_SPEED=3`.

| Leg (msg/s AVG)   | barrier | no-barrier |
| ----------------- | ------: | ---------: |
| Round 1           | 144,378 |    143,076 |
| Round 2           | 144,910 |    146,141 |
| **Mean**          | **144,644** | **144,608** |

Dead even (+0.02%, standard error ≈ 1.1k), and both variants sit at the
0.3-documented speed-3 ceiling (145,592) — the sync-table CLOS registries
(loaded by both binaries) cost nothing either, as expected (registries are
not on the per-message path).

Method note: a plain sequential rerun of the full 0.3 protocol first showed
an apparent −5–9% across the board; the interleaved A/B exposed it as
session drift (the suspect legs ran after ~45 min of sustained all-core
load). For future regression checks, interleave the builds under test —
same-session sequential runs of this benchmark drift by more than the
effect sizes of interest.

---

## 2026-07-10 — spec 1.3: optimize support (const folding + dead branches + check elision)

**Commit**: follows `e388fe2`.

`(declaim (optimize ...))` / body `(declare (optimize ...))` now drive the
compiler (lexically scoped per CLHS 3.3.4).  At `speed >= 1` (the default),
calls to pure fixnum builtins with constant arguments fold to a single
`CONST` (`(+ 1 2 3)`: 20 bytes / 4 constants → 9 bytes / 1 constant, tested
via disassembly), and constant `IF` tests compile only the live branch.  At
`(safety 0)`, destructuring-bind arity guards and the `THE` type assert are
elided.  See specs/performance.md § 1.3 for the design and file list.

**Environment**: Apple M3 Ultra, macOS 26.5.2, `make host`, warm FASL cache,
`--heap 64M`, cwd = repo root.  Baseline = the 2026-07-05 bench-opt entry
below (same flags; note the 07-05 numbers predate the CLOS slot-access
specs, so only the rows below are attributable to 1.3).

**Reproduce**: `echo '(quit)' | ./build/host/amilisp --heap 64M --load trunk/bench-opt.lisp`

| Benchmark | before (07-05) | after | delta |
|---|---|---|---|
| opt.const-fold | 39 ms | **20 ms** | 1.95× |
| opt.dead-branch | 49 ms | **35 ms** | 1.4× |
| vm.fixnum-loop | 72 ms | **61 ms** | 1.18× |
| vm.local-shuffle | 63 ms | **51 ms** | 1.24× |
| vm.call-return | 63 ms | 60 ms | ~1.05× |
| safety1.svref-loop | 48 ms | 44 ms | ~1.09× |

All 30 bench-opt checks pass (`fails=0`).  The safety0.* rows are unchanged
because arity/bounds checks live in the VM's call/aref handlers, not at the
emit site — elision for those is deferred to the 1.8-era bytecode work.

---

## 2026-07-10 — writer-GF inline cache: (setf (x obj) v) fast dispatch (host)

**Commit**: follows `25e1fd9`.

The reader fast path left writers untouched: `(setf (x obj) v)` was a plain
2-arg GF call through the full discriminator (EMF inline cache + effective
method funcall + `%ACCESSOR-WRITER-BODY`'s per-store slot resolution).  This
entry adds the mirror machinery: a GF whose whole method set is
DEFCLASS-generated writer methods for one slot is promoted
(`AMILISP:*WRITER-GFS*`), and the same probe sites that answer reader calls
(`OP_CALL`, `cl_vm_apply` — the m68k-JIT funnel — `bi_funcall`, `bi_apply`,
the VM's inline `OP_APPLY`) store straight through the cached slot index on
a 2-arg call.  Writer IC entries reuse the reader's `(TYPE-NAME . FIXNUM)`
cons shape with the index encoded negative (`(- -1 idx)`), so the two probes
can never answer from each other's caches (a 1-arg call to a promoted writer
GF still signals its arity error) and every existing slot-8 invalidation
site covers writers for free.

**Environment**: macOS arm64, host bytecode VM, best-of-3.

**Reproduce**: 8 unrolled stores per iteration, 250k iterations, instance in
a local; the poly row alternates two classes whose slot sits at different
indices.

| ns per store | before | after | speedup |
|---|---|---|---|
| `(setf (x obj) v)`, 1 class | 144.5 | **16.0** | 9.0× |
| 2 classes alternating | 390.0 | **17.0** | 23× |
| via `FUNCALL #'(setf x)` | 618.5 | **17.0** | 36× |
| accessor read (control) | 13.5 | 13.5 | — |

Writes now cost the same as promoted reads — the symmetric result the reader
entries below established for the read side.  Real workloads write about as
often as they read (sento actors mutate state constantly), so this closes
the last accessor-shaped gap on the hot path.

Tracked by `clos.accessor-write` in `trunk/bench-opt.lisp` (7 ms, identical
to `clos.accessor`, zero allocation).

Verified: host `make test` + `test_clos_reader` 28/28 (11 new writer tests
incl. arity-error-not-slot-read, `:after`-method demotion, redefinition,
`SET-FUNCALLABLE-INSTANCE-FUNCTION`, latch demotion), gc-stress 391/391
(new writer-GF compact-every-alloc case), FS-UAE suite.

---

## 2026-07-10 — reader-GF inline cache reaches JIT'd callers + call trampolines (Amiga + host)

**Commit**: follows the SLOT-VALUE entry below.

The m68k JIT never executes the bytecode `OP_CALL`, so on the platform this
project exists for, every reader-GF access still unwrapped to the Lisp
discriminating function — the caveat noted in the two entries below.  No
native codegen was needed to close it: every JIT'd call funnels through
`cl_jit_runtime_call → cl_vm_apply`, and `cl_gf_reader_ic_probe` reads
everything it needs (GF slot 8, receiver `type_desc`) fresh at runtime, so
the probe now runs at the top of `cl_vm_apply` — before the GF unwrap
discards the identity the cache is keyed on.  No baked immediates, no
`native_relocs` involvement.  The same probe-before-unwrap was added to the
other C trampolines that lost the GF identity early: `bi_funcall`,
`bi_apply`, the VM's inline `OP_APPLY`, and the sequence helpers' `call_1`.

**Environment (Amiga)**: FS-UAE A4000/68040 + UAE JIT
(`verify/realamiga/verify.fs-uae`), `--heap 8M`, m68k JIT active.  Relative
deltas are what matter; absolute times are emulator-specific.
8-unrolled reader loop inside a JIT'd DEFUN, 240k calls per timing.

| µs per call (JIT'd caller) | before | after | speedup |
|---|---|---|---|
| reader, direct call | 26.17 | **5.83** | 4.5× |
| reader via FUNCALL | 25.75 | **5.67** | 4.5× |
| reader via APPLY | 24.00 | **3.33** | 7.2× |

**Host**: compiled `(apply #'reader (list o))` (inline `OP_APPLY`) halved,
77 → 40 ns/call (the remainder is the harness's per-call arglist consing).
Compiled `(funcall #'reader o)` emits plain `OP_CALL` and was already at the
28.5 ns tier.  `clos.accessor` / `clos.accessor-poly` in bench-opt unchanged
at 7 ms — the interpreter fast path is untouched.

Not covered (follow-up): MAPCAR-family and `:key`/`:test` callers unwrap the
GF at designator-coercion time (`cl_coerce_funcdesig`), so their per-element
calls still take the Lisp discriminator tier (~107 ns/elt host).  Routing
them through the probe means auditing the 28 `cl_coerce_funcdesig` call
sites' "returns one of three flat types" contract.

Verified: host `make test` + `test_clos_reader` 17/17, gc-stress 384/384,
FS-UAE suite 3674/3674 (new checks assert the IC spine stays EQ across
JIT'd-caller/funcall/apply calls, and that demotion by `:around` methods,
`SET-FUNCALLABLE-INSTANCE-FUNCTION`, and the slot-access-protocol latch
disarms every trampoline probe).

---

## 2026-07-10 — SLOT-VALUE compile-time inline + (type, slot) pair index (host)

**Commit**: follows `e0d1ca0`.

`SLOT-VALUE` and `(SETF (SLOT-VALUE ...))` are ordinary DEFUNs, so every
access paid a full Lisp call frame before reaching the fused
`%STRUCT-SLOT-VALUE`/`%STRUCT-SLOT-STORE` builtin — the frame, not the slot
lookup, was the bulk of the cost.  Three changes, no new opcode, no FASL
format change:

1. **Compiler macros** on `SLOT-VALUE` and `%SET-SLOT-VALUE` (clos.lisp)
   splice the DEFUN bodies' fast-path test into every compiled call site —
   the same treatment DEFCLASS accessors and `WITH-SLOTS` already got.
2. **`compile_setf` routes defsetf updaters through `compile_call`**
   (compiler.c) instead of hand-emitting `OP_FLOAD`/`OP_CALL`, so the
   `%SET-SLOT-VALUE` macro can fire at all (and any defsetf setter now
   benefits from compiler macros / builtin opcodes).
3. **A `(type-name, slot-name) → index` pair table** built alongside the
   struct-registry hash index (builtins_struct.c), same lock and same
   dirty/disabled lifecycle, replaces `struct_slot_resolve`'s linear
   specs-list walk — O(1) for any slot of any width class.

**Environment**: macOS arm64, host bytecode VM, best-of-3.

**Reproduce**: 8 unrolled accesses per iteration, 250k iterations, instance
in a local; 12-slot class for the depth rows.

| ns per access | before | after |
|---|---|---|
| `(slot-value p 'x)` read, slot 0 of 2 | 77.5 | 56.0 |
| `(setf (slot-value p 'x) v)` write | 72.5 | 51.0 |
| read, slot 0 of 12 | 59.0¹ | 56.0 |
| read, slot 11 of 12 | 68.0¹ | 56.0 |

¹ measured with the inline already applied, isolating the pair index: the
walk cost ~0.75 ns per slot position; now flat.

Tracked by `clos.slot-value` (18 → 15 ms; the loop harness dilutes the
per-access delta) and the new `clos.slot-value-deep` (15 ms, equal to the
shallow case) in `trunk/bench-opt.lisp`; `struct.slot-value` 19 → 15 ms.
The reader-GF accessor path (17.5 ns) remains faster — it answers in
`OP_CALL` with no call at all — but the "slot-value is 4× a reader" inversion
is now ~2×, and `FUNCALL`/`APPLY` of `#'SLOT-VALUE` still routes through the
DEFUN unchanged.

---

## 2026-07-10 — polymorphic reader-GF inline cache (host)

**Commit**: follows `58f524c`.

The reader IC (GF slot 8) was a single `(TYPE-NAME . SLOT-INDEX)` entry, so a
call site alternating between receiver classes missed on every call — and a
miss is the full slow dispatch plus `%COMPUTE-APPLICABLE-METHODS` plus an IC
rewrite.  Now the IC is a list of up to 4 entries, most-recently-missed
first; `cl_gf_reader_ic_probe` walks it with one word-compare per entry, and
the miss path carries surviving entries over instead of discarding them.

**Environment**: macOS arm64, host bytecode VM, best-of-3.

**Reproduce**: 8 unrolled reader calls per iteration on one GF; receivers
cycle through 1, 2 or 4 classes (subclasses of one base, each with its own
`type_desc`), 250k iterations.

| ns per slot access | before | after |
|---|---|---|
| 1 class (mono) | 16.5 | 17.5 |
| 2 classes alternating | ~1340 | 17.5 |
| 4 classes alternating | ~1340 | 18.5 |

The ~80x alternation penalty is gone; the monomorphic path pays one extra
spine dereference (~1 ns).  Beyond 4 receiver classes the cap evicts the
oldest entry and cycling receivers miss again — bounded, correct, and no
worse than the old behaviour.

Tracked by `clos.accessor-poly` in `trunk/bench-opt.lisp` (7 ms, identical to
the monomorphic `clos.accessor`, zero allocation).  The m68k JIT caveat from
the 2026-07-09 entry still applies: JIT'd callers reach the same probe
through the Lisp reader discriminator, so they get the polymorphic hits too,
at that tier's cost.  *(Resolved 2026-07-10 — see the JIT'd-callers entry
above.)*

---

## 2026-07-09 — reader-GF fast dispatch (host)

**Commits**: `5291e7d` (reader inline cache + Lisp discriminator), then
`05c2aa2` (answer the call in `OP_CALL`).

A GF whose whole method set is the DEFCLASS-generated readers for one slot is
promoted: its inline cache (GF slot 8) holds `(TYPE-NAME . SLOT-INDEX)`, and
`OP_CALL` answers the call by comparing the receiver's `type_desc` and reading
the slot — no unwrap to the discriminating function, no frame, and none of the
per-access `CLASS-OF` + `*CLASS-TABLE*` + slot-index hash probes that
`%STRUCT-SLOT-VALUE` pays.

**Environment**: macOS arm64, host bytecode VM (the JIT is m68k-only, so this
is pure bytecode). Best-of-3, ~1s per timed run.

**Reproduce**: a portable port of the `SLOT-ACCESS/READER` benchmark from
Daniel Kochmański's [*A brief note about slot access cost in Common
Lisp*](https://turtleware.eu/posts/A-brief-note-about-slot-access-cost-in-Common-Lisp.html)
— 100 unrolled reader calls per iteration on a 10-slot class. Cross-checked
against SBCL 2.6.5 and ECL 26.5.5 on the same machine.

| ns per slot access | before | + Lisp discriminator | + `OP_CALL` |
|---|---|---|---|
| reader GF | 148.3 | 72.0 | **28.5** |
| reader ÷ struct-ref (24.7 ns) | 6.00× | 2.91× | **1.20×** |
| reader ÷ plain 1-arg call (44.5 ns) | 3.32× | 1.62× | **0.65×** |
| reader ÷ `slot-value` (103.6 ns) | 1.47× | 0.67× | **0.27×** |

A reader now costs 20% more than a raw constant-index struct slot read, and
*less than an ordinary function call* — because there is no call.

Reference points on the same machine and benchmark (native compilers, so the
absolute times are not comparable to a bytecode VM; the ratio is):

| | reader GF | reader ÷ struct-ref |
|---|---|---|
| SBCL 2.6.5 | 2.61 ns | 5.14× |
| ECL 26.5.5 (`compile-file`) | 13.22 ns | 2.59× |
| amilisp (this entry) | 28.5 ns | **1.20×** |

Note: ECL's `--load` of *source* runs its bytecode interpreter (53 ns/access,
a meaningless 1.05× ratio). The number above is after `compile-file`.

Tracked by `clos.accessor` ÷ `struct.accessor` in `trunk/bench-opt.lisp`.
The m68k JIT does not route through the bytecode `OP_CALL`, so JIT'd callers
fall back to the Lisp reader discriminator (72 ns tier) until the JIT call
sequence learns the same probe.  *(Resolved 2026-07-10 — the probe moved into
`cl_vm_apply`, which the JIT call helper funnels through; see the
JIT'd-callers entry above.)*

---

## 2026-07-05 — fused slot access + C GF inline-cache probe (host)

**Commit**: follows `fd2ecf4`. Second half of spec item 3.1. Three changes:

- New fused `%STRUCT-SLOT-VALUE` / `%STRUCT-SLOT-STORE` builtins: type-name →
  registry entry (O(1) hash probe) → slot index → direct slot read/write in
  ONE non-erroring builtin call. `SLOT-VALUE` / `(SETF SLOT-VALUE)` /
  `SLOT-BOUNDP` front the full protocol with it (the old front paid ~4 Lisp
  calls + ~8 builtin dispatches per access: `class-of`, the index-table
  branch, `%STRUCT-SLOT-INDEX`, a separate `%STRUCT-REF`). Works for both
  DEFSTRUCT and CLOS instances; any non-simple case (`:CLASS` slot, unbound,
  extended protocol, wrong type) falls back to the unchanged full protocol.
- DEFCLASS-generated `:accessor`/`:reader`/`:writer` methods inline the fused
  fast path in the method body instead of calling `SLOT-VALUE`.
- New `%GF-IC-EMF` builtin fuses the 1/2-arg GF discriminators' inline-cache
  hit path (read GF slot 8 + receiver `class-of` + `*CLASS-TABLE*` lookup +
  compare) into one call — this part speeds up ALL 1/2-arg GF dispatch.

| Benchmark | pre (first half only) | post |
|---|---|---|
| clos.slot-value (200k accesses/run) | 55 ms | **18 ms** |
| clos.accessor (GF dispatch + read) | 92 ms | **28 ms** |
| struct.slot-value | 53 ms | **18 ms** |
| struct.accessor (constant-index %STRUCT-REF — control) | 6 ms | 6 ms |
| clos.make-instance (2k instances + accessor reads/run) | 91 ms | **72 ms** |

Isolated probe (1M reads of one slot, bytecode): accessor 243 → 141 ms,
`slot-value` 96 ms. The remaining accessor-vs-struct.accessor gap (28 vs
6 ms) is generic dispatch itself — funcallable-instance unwrap +
discriminator closure + method-function call — not slot resolution.

**Real-world impact** — sento actor smoke benchmark
(`trunk/run-sento-bench.lisp`, 2s x 2 iterations, 2 producers, `:pinned`
`tell`), the workload whose runtime profile motivated spec 3.1:

| | msg/s |
|---|---|
| baseline (2e5f7c4, pre-3.1) | 55,709 |
| after 3.1 both halves | **118,036 (2.1x)** |

**Environment**: Apple M3 Ultra, macOS 26.5.2, `make host`, warm FASL cache,
`--heap 64M`, cwd = repo root.

**Reproduce**:

```
echo '(quit)' | ./build/host/amilisp --heap 64M --load trunk/bench-opt.lisp
```

---

## 2026-07-05 — struct registry hash index + zero-alloc slot resolution (host)

**Commit**: follows `5308c96`. First half of spec item 3.1, driven by the
sento actor-benchmark runtime profile (the largest attackable CPU cluster
after the VM loop was slot-access machinery: `get_slot_specs` 1,028 +
`bi_gethash` 512 + `cl_struct_slot_names` / `bi_class_of` / `bi_struct_ref`
leaf samples). Two fixes:

- `find_struct_entry` now probes an O(1) open-addressing hash index over the
  struct registry instead of walking the prepended alist (early-defined
  types paid a full O(types) walk on every access); the index is marked
  dirty on registration and on compaction, and rebuilt lazily
  (`platform_alloc` only).
- `SLOT-VALUE` / `(SETF SLOT-VALUE)` / `SLOT-BOUNDP` on struct instances
  resolve slots via the new zero-allocation `%STRUCT-SLOT-INDEX` builtin —
  the old path consed a fresh slot-name list per access
  (`cl_struct_slot_names`) and matched it linearly in Lisp.

New `struct.*` benchmarks in bench-opt (the type is buried behind 256 later
registrations, matching real-session registry positions; the pre-existing
`clos.*` benches take the CLOS slot-index-table hash path and never hit the
struct registry):

| Benchmark | pre-fix | post-fix |
|---|---|---|
| struct.slot-value (200k accesses/run) | 143 ms, 6,400,008 bytes | **53 ms, 0 bytes** |
| struct.typep | 50 ms | **12 ms** |
| struct.accessor (constant-index %STRUCT-REF — target/control) | 6 ms | 6 ms |
| clos.slot-value (CLOS hash path — control) | 55 ms | 55 ms |
| compile.file-plain / compile.file-mlf (registry lookups at compile time) | 27 / 27 ms | **20 / 20 ms** |

struct.slot-value is now at parity with the CLOS-instance hash path. The
remaining gap to struct.accessor (53 vs 6 ms) is generic-lookup overhead —
`class-of` + the `%find-struct-slot-index` call chain — which is the second
half of 3.1 (direct-index accessor closures at class finalization).

**Environment**: Apple M3 Ultra, macOS 26.5.2, `make host`, warm FASL cache,
`--heap 64M`, cwd = repo root.

**Reproduce**:

```
echo '(quit)' | ./build/host/amilisp --heap 64M --load trunk/bench-opt.lisp
```

---

## 2026-07-05 — MAKE-LOAD-FORM pre-pass hash index (host)

**Commit**: follows `64d6d8f`. Fix for the profiling finding that
`fasl_mlf_seen_p` / `cl_fasl_mlf_lookup` linear scans made the FASL writer's
make-load-form pre-pass O(n²) — ~85% of all cold-compile CPU (14,713 of ~17k
leaf samples) once any loaded library defines a `make-load-form` method
(cl-ppcre, serapeum, log4cl, local-time, trivia, cffi, ironclad, fset all
do, so the gate is effectively always open in real sessions). See
specs/performance.md item 1.9.

**Environment**: Apple M3 Ultra, macOS 26.5.2, `make host`, warm FASL cache,
cwd = repo root.

| Measurement | pre-fix | post-fix |
|---|---|---|
| `(asdf:load-system "sento" :force :all)` full dep recompile, 192M heap | 18,283 ms | **1,864 ms (9.8x)** |
| bench-opt `compile.file-mlf` (2 compiles of a 20k-cons graph + load) | 123 ms | **23 ms** |
| bench-opt `compile.file-plain` (same, MLF gate closed — control) | 22 ms | 22 ms |
| warm `(ql:quickload "sento")` (unaffected — no compile) | 840 ms | 840 ms |

Post-fix, `cl_fasl_mlf_prepass` no longer appears in the profile at all; the
cold-compile leaf leaders are now the compiler's macro-lookup chain
(`cl_macro_p` 1009 + `cl_get_macro` 184 + `cl_get_compiler_macro` 157
samples) and the VM loop (`cl_vm_run` 874) — the next profiling candidates.

Also measured (context for deprioritizing spec item 2.4): set-operation call
counters during a full sento quickload — `intersection`/`union`/`subsetp`
0 calls; `adjoin` 91 and `set-difference` 20 calls, all on lists ≤ 40
elements.

**Reproduce**:

```
echo '(quit)' | ./build/host/amilisp --heap 64M --load trunk/bench-opt.lisp
# recompile timing: (ql:quickload "sento" :silent t) then
#   (time (asdf:load-system "sento" :force :all))  at --heap 192M
# profile: run the recompile in background, then: sample <pid> 20
```

---

## 2026-07-05 — bench-opt baseline (host, pre-optimization)

**Commit**: `ac39e3c` + new `trunk/bench-opt.lisp`. Baseline for the pending
items in [specs/performance.md](../specs/performance.md) — 1.3 (declaim-speed:
const folding / dead branches / check elision), 1.8 (peephole), 2.4 (set ops
in C), 2.5 (free-list size classes), 3.1 (CLOS slot access), 3.2 (keyword
pre-computation), 3.3 (mv_count writes) — captured **before** any of them
land. Each benchmark maps to a spec item (see the file header); results are
best-of-3 with a warmup run, pure bytecode, deterministic workloads verified
against closed-form expected values (`fails=0`).

**Environment**: Apple M3 Ultra, macOS 26.5.2, `make host`, warm FASL cache,
`--heap 64M`, cwd = repo root.

| Benchmark | ms | bytes consed | gc |
|---|---|---|---|
| vm.fixnum-loop | 72 | 0 | 0 |
| vm.local-shuffle | 63 | 0 | 0 |
| vm.call-return | 63 | 0 | 0 |
| opt.const-fold | 39 | 0 | 0 |
| opt.dead-branch | 49 | 0 | 0 |
| safety1.svref-loop | 48 | 0 | 0 |
| safety0.svref-loop | 48 | 0 | 0 |
| safety1.call-args | 53 | 0 | 0 |
| safety0.call-args | 53 | 0 | 0 |
| set.intersection-small | 36 | 432,000 | 0 |
| set.union-small | 53 | 1,200,000 | 0 |
| set.intersection-large | 67 | 24,080 | 0 |
| set.union-large | 99 | 72,080 | 0 |
| set.difference-large | 68 | 24,080 | 0 |
| set.subsetp-large | 82 | 0 | 0 |
| set.intersection-equal | 45 | 32,320 | 0 |
| set.intersection-key | 59 | 24,160 | 0 |
| kw.call-8keys | 85 | 0 | 0 |
| clos.make-instance | 91 | 12,000,000 | 0 |
| clos.slot-value | 60 | 0 | 0 |
| clos.accessor | 92 | 0 | 0 |
| alloc.mixed-churn | 82 | 57,603,416 | 1 |
| alloc.cons-churn | 53 | 16,000,016 | 0 |
| compile.file-plain * | 22 | 1,635,384 | 0 |
| compile.file-mlf * | 123 | 1,636,736 | 0 |
| **total** | **1460** | | |

\* The `compile.*` pair was added to the suite the same day, just before the
MLF pre-pass fix landed (see the entry above); values here are pre-fix.

Run-to-run stability: a second full run totaled 1452 ms (~0.5% variance);
individual benchmarks repeat within ±2 ms.

Notes for later comparisons:

- `safety0.*` vs `safety1.*` pairs time equal today (only `the`'s
  `OP_ASSERT_TYPE` is safety-gated); item 1.3's check elision should open a
  gap in favor of `safety0.*`.
- The `set.*-large` benchmarks are the O(n*m) → O(n+m) headline for 2.4;
  `set.*-small` guards against the C-builtin version regressing small inputs.
- `alloc.mixed-churn` is sized to cycle the GC (gc ≥ 1) so the free list is
  actually populated and exercised — keep it that way or 2.5 won't show.

**Reproduce**:

```
echo '(quit)' | ./build/host/amilisp --heap 64M --load trunk/bench-opt.lisp
```

On Amiga, scale down the repetition counts and the large-set size first:

```lisp
(defparameter cl-user::*bo-scale* 1/20)
(defparameter cl-user::*bo-set-size* 150)
(load "trunk/bench-opt.lisp")
```

---

## 2026-07-05 — GC mark phase on large live heaps (host, growable mark stack)

**Commit**: growable GC mark stack (follows `72fe7ed`). Found investigating
"`(asdf:load-system :chipi-ui/tests)` takes 21s from a Sly session vs 13s
from a fresh REPL" — the difference was never multi-threading (MT allocator
path, worker-thread eval, and slynk mREPL streaming all measured within
noise of the single-threaded baseline); it was the *live-heap size* of the
long-running Sly image. The fixed 4096-entry mark stack silently overflowed
on any object with more unmarked children, and each full-arena overflow
re-scan pass recovers only ~one-stack-full — quadratic marking.

**Environment**: Apple M3 Ultra, macOS 26.5.2, `make host`, warm FASL
cache, `--heap 512M`, cwd = repo root, chipi-ui dependency closure from
`~/Development/MySources/cl-hab/ocicl`.

**Single mark-sweep `(ext:gc)`, 210MB live** (160 x 65536-element vectors
of fresh conses — worst-case wide fan-out):

| Build | one GC cycle |
|---|---|
| fixed 4096-entry mark stack (pre-fix) | 49,248 ms |
| growable mark stack (this change) | 56 ms |

**`(asdf:load-system :chipi-ui/tests)`, warm FASLs, 512M heap**:

| Scenario | pre-fix | post-fix |
|---|---|---|
| fresh image, main thread | 12,800 ms (3 GCs) | 11,066 ms (3 GCs) |
| + 200MB live ballast (reproduces the user's Sly-session shape: 9 GCs, ~330MB used) | >10 min (killed) | 11,772 ms |

**Reproduce**: build ballast with
`(loop repeat 160 collect (let ((v (make-array 65536))) (dotimes (i 65536) (setf (aref v i) (cons i i))) v))`,
then `(time (ext:gc))`. Growth/fallback observability:
`(ext:%gc-mark-stats)` → `(capacity grows rescan-passes)`; a nonzero third
element means growth failed (cap/OOM) and the quadratic fallback ran.

---

## 2026-07-05 — sento actor throughput (host)

**Commit**: `2e5f7c4` (post tier-4 GC audit + contended acquire-lock
parking fix — contended `mp:acquire-lock` waiters now park on a condvar
broadcast by `release-lock` instead of sleep-polling on a 10ms grid).

**Environment**: Apple M3 Ultra (28 cores), 96GB RAM, macOS 26.5.2,
`make host` build, warm FASL cache, `--heap 192M`.

**Benchmark**: `sento.bench::run-benchmark` from cl-gserver `bench.lisp`
(sento 20260101 quicklisp dist). Producer threads `tell`/`ask-s` a counter
actor as fast as possible for the configured duration; backpressure pauses
producers when the queue exceeds 10k messages. Reported number is messages
processed per second, averaged over iterations.

**Smoke config** (as run by `trunk/run-sento-bench.lisp`: 2s x 2
iterations, 2 producer threads):

| Config | avg msg/s |
|---|---|
| `:pinned`, `tell` | 55,709 |

**Measurement config** (10s x 3 iterations, 8 producer threads):

| Config | avg msg/s | min–max | deviation |
|---|---|---|---|
| `:pinned`, fire-and-forget `tell` | 42,131 | 41.8k–42.5k | ±281 |
| `:shared` (4 workers), `tell` | 14,860 | 14.2k–15.2k | ±482 |
| `:shared` (4 workers), `ask-s` round-trip | 15,460 | 15.0k–16.0k | ±409 |

**Observations**:

- Iteration-to-iteration deviation is under 1–3% — no scheduling jitter
  from the lock layer (the pre-fix 10ms sleep-poll made 8-producer
  configs collapse ~6x and scatter widely).
- Synchronous `ask-s` (send + blocking reply round-trip, the heaviest
  user of the lock/condvar handoff path) matches plain async `tell` on
  the shared dispatcher — the round-trip machinery is not the
  bottleneck; shared-dispatcher fan-out dominates.
- `:pinned` (dedicated actor thread) is ~2.8x faster than `:shared`,
  as expected — shared dispatch adds a second queue hop through the
  worker pool.
- 2 producers outpace 8 (55.7k vs 42.1k pinned): mild fair-contention
  degradation, no collapse.

**Reproduce**:

```
# smoke (as committed):
./build/host/amilisp --heap 192M --load trunk/run-sento-bench.lisp

# measurement configs:
#   (sento.bench::run-benchmark :dispatcher :pinned :duration 10
#                               :num-iterations 3 :load-threads 8)
#   (... :dispatcher :shared :num-shared-workers 4)
#   (... :dispatcher :shared :num-shared-workers 4 :with-reply-p t)
```

---

## 2026-07-05 — JIT call loop (Amiga, FS-UAE)

**Commit**: `2e5f7c4`. Emulated 68020 via FS-UAE (bundled config,
`make -f Makefile.cross test-amiga`), run automatically after the Amiga
test suite (`trunk/bench-jit-loop.lisp`).

| Benchmark | result |
|---|---|
| JIT-BENCH (2,000,000 calls) | 25,120 ms → 79,618 calls/sec |

---

## 2026-07-10 — Spec 1.8: bytecode peephole post-pass (speed >= 2)

**Branch**: `feat/peephole-speed2`. Host, Apple M3 Ultra, `make host`,
`--heap 64M`, `trunk/bench-opt.lisp` best-of-3, pure bytecode, `fails=0`
in both runs. Comparison: default (`speed 1`, pass off) vs
`AMILISP_FORCE_SPEED=3` (pass on for every compile). The vm.* rows are
the pass's target workloads (store/load traffic, call-return around
tight loops); the set./clos./struct./alloc. rows are dominated by C
builtins and move little, as expected.

| Benchmark | speed 1 | forced speed 3 | delta |
|---|---|---|---|
| vm.fixnum-loop | 58 ms | 53 ms | -8.6% |
| vm.local-shuffle | 49 ms | 45 ms | -8.2% |
| vm.call-return | 57 ms | 50 ms | -12.3% |
| opt.dead-branch | 35 ms | 32 ms | -8.6% |
| safety1.svref-loop | 44 ms | 39 ms | -11.4% |
| safety1.call-args | 51 ms | 46 ms | -9.8% |

**Reproduce**:

```
./build/host/amilisp --no-userinit --heap 64M --non-interactive --load trunk/bench-opt.lisp
AMILISP_FORCE_SPEED=3 ./build/host/amilisp --no-userinit --heap 64M --non-interactive --load trunk/bench-opt.lisp
```

---

## 2026-07-15 — TLAB: per-thread allocation buffers (host, MT)

**Branch**: `perf/tlab-alloc`.  With more than one thread registered,
every `cl_alloc` used to serialize on the global `alloc_mutex`; at actor
message rates the mutex handoff dominated the per-message cost.  Each
thread now cuts objects lock-free from a private chunk (TLAB, default
32K, `AMILISP_TLAB_CHUNK` overrides, compiled out on Amiga) refilled
from the shared bump front / free list under the mutex once per chunk.
Uncut chunk remainders stay formatted as walkable free-block holes;
every GC cycle drops all TLABs during stop-the-world.

**Environment**: Apple M3 Ultra, macOS 26.5.2, `make host`,
`--heap 192M`, default speed.  Benchmark: `sento.bench::run-benchmark`
matrix as in [sento-bench-results-0.3.md](sento-bench-results-0.3.md)
(`:num-shared-workers 8, :load-threads 8, :duration 5,
:num-iterations 6`).  Baseline = the speed-1 column measured there
(commit `360abd4`).

| Dispatcher | Reply mode | baseline | TLAB | delta |
| ---------- | ---------- | -------: | ---: | ----: |
| PINNED     | tell       | 142,836 | **188,711** | +32% |
| PINNED     | ask-s      |  62,644 |  78,259 | +25% |
| PINNED     | ask        |  13,387 | **27,308** | +104% |
| SHARED     | tell       |  22,249 |  27,420 | +23% |
| SHARED     | ask-s      |  25,073 | **46,758** | +86% |
| SHARED     | ask        |  10,754 |  14,848 | +38% |

- The allocation-heaviest cells gained the most: `ask` allocates a
  future/promise per message and doubled on the pinned dispatcher;
  `shared/ask-s` (+86%) pays queue-handoff plus reply allocation.
- SHARED/`ask` is the noisiest cell (dev ±5.6k on 14.8k); treat its
  delta as indicative.
- Cross-implementation context (same machine, same config, measured
  2026-07-15): SBCL 2.6.5 pinned/tell 1.44M msg/s, ECL 26.5.5 512k.
  The TLAB moves amilisp from 1/10 to ~1/7.6 of SBCL on that cell.

**Reproduce**: run the 6-cell matrix via
`sento.bench::run-benchmark` after `(load "trunk/load-sento-bench.lisp")`;
A/B by setting `AMILISP_TLAB_CHUNK=0` (disables TLABs at runtime).

## 2026-08-23 — Raw OS binding modules: heap per `require` (host + FS-UAE)

`specs/raw-bindings-footprint.md`: a generated binding module
(`lib/amiga/raw/*`) used to materialise every function, constant and
struct field at load; Phase 1 replaced the wrappers with 20-byte FFI
stubs, Phase 2 ships the module as one packed binding table and builds a
name on first reference.  Heap delta of `(require m)` after `(ext:gc)`
(`ROOM`), FASL-loaded, no docstrings:

| module           | eager   | Phase 1 (stubs) | **Phase 2 (table)** | FS-UAE P1 | **FS-UAE P2** | FASL P1 → P2 |
|------------------|--------:|----------------:|--------------------:|----------:|--------------:|-------------:|
| raw/exec         | 216 KB  |   106 KB        |   **37 KB**         |  90 KB    |   **24 KB**   | 151 → 25 KB  |
| raw/dos          | 306 KB  |   140 KB        |   **34 KB**         | 135 KB    |   **34 KB**   | 197 → 35 KB  |
| raw/intuition    | 424 KB  |   218 KB        |   **50 KB**         | 210 KB    |   **50 KB**   | 301 → 53 KB  |
| raw/graphics     | 477 KB  |   232 KB        |   **55 KB**         | 223 KB    |   **55 KB**   | 294 → 58 KB  |
| the four         | 1.42 MB |   0.71 MB       |   **0.18 MB**       |           |               |              |

FS-UAE (68040/JIT config, `make -f Makefile.cross test-amiga`, the
`; raw-bindings:` lines of the suite log): 140–160 ms per module from
FASL; smaller modules (utility 5 KB, layers 8, gadtools 8, timer 2,
gadgets/button 4, classes/window 6).  Touching 150 intuition names on
the host adds 14 KB (~95 B per name).  `(amilisp::%binding-table-info
"AMIGA.RAW.INTUITION")` → 1485 entries in 47 KB, 23 symbols right after
load.

**Reproduce**: the appendix script of the spec on the host; on the
target the suite prints the per-module line itself
(`tests/amiga/test-raw-bindings.lisp`, `%raw-require`).
