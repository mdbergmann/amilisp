# Lazy JIT (compile when hot)

Status: **implemented** (2026-09-22, for 0.12).  Proposed 2026-06-04; the
design below is what shipped, with the differences from the proposal noted
at the end.

## Problem

The m68k JIT compiled every `CL_Bytecode` the moment it was created: at
FASL load (`fasl.c`), in `compile_lambda`, and even for top-level forms
(`cl_compile`), whose native code nothing ever ran — `cl_vm_eval`
interprets, and native code is entered only through `OP_CALL`.  Measured
in FS-UAE's 68040 config (2026-09-21, snapshot binary, JIT vs `--no-jit`):

|                              | JIT       | no JIT  |
|------------------------------|-----------|---------|
| boot (boot library + CLOS)   | 700 ms    | 520 ms  |
| loading the editor           | 2140 ms   | 1540 ms |
| native code, boot only       | ~450 KB   | 0       |
| native code, boot + editor   | ~1.2 MB   | 0       |

The benefit is uneven (`docs/benchmarks.md`): a tight loop ~18× faster,
fixnum CASE ~4×, a plain call ~1.7×, the editor's per-key path ~5%.  Most
of what load time compiles is cold.

And heap images: `image.c` drops native code on restore (the buffers are
off-heap), and since the JIT ran only at creation, **nothing in an image
was ever compiled again** — every shipped `amilisp.img` ran boot + CLOS as
bytecode.

## Policy

- A function is compiled on its `threshold`-th interpreted call
  (default 8, `CL_JIT_HOT_DEFAULT`).
- A function containing a backward jump (a loop) is compiled on its first
  call: it pays for the compile inside that call.
- A function compiled under `(optimize (speed 3))` — anywhere in the
  lambda, a local `DECLARE` included — is compiled at definition.
- A function defined while the JIT is off (`--no-jit`,
  `(clamiga::%jit-set-active nil)`) stays bytecode for good, which keeps
  the A/B-benchmark idiom working.
- Threshold 0 is eager mode, the old behaviour: `--jit-eager`, or
  `(clamiga::%jit-set-hot-threshold 0)` (returns the previous value).
  The m68k tests that inspect native code right after a `DEFUN` bind it
  (`tests/amiga/test-jit.lisp`); the rest of `run-tests.lisp` runs at
  speed 3 and is eager through the hint.
- `%JIT-DISASSEMBLE` (and so `JITEXPAND`) compiles a function that is
  still counting, so a fresh definition can be inspected.

## Mechanism

**State: one byte, in padding.**  `CL_Bytecode.jit_hot` sits after
`n_keys`, where both the host and the m68k layout had a pad byte
(`sizeof(CL_Bytecode)` is unchanged: 0x80 host, 0x4a m68k — `image.c`
hashes it).  Bits 0-6 count interpreted calls; the value 0x7F
(`CL_BC_JIT_SETTLED`) means stop counting — compiled, rejected, or
defined with the JIT off.  Bit 7 (`CL_BC_JIT_SPEED`) is the speed-3 hint.

**Hint on the wire.**  The compiler sets the hint on every target (the
host compiles the FASLs the m68k binaries load); the FASL writer carries
it in bit 7 of the serialized flags byte, the reader strips it back out —
the in-memory `flags` keep bits 0-1, which the JIT's eligibility checks
compare against 0.  Part of `CL_FASL_VERSION` 38.

**Call paths.**  Every interpreted call is counted in one place, the VM's
`OP_CALL` (`vm.c`):

```c
if (!bc->native_code && CL_BC_JIT_COUNTING_P(bc))
    cl_jit_note_call(bc);
```

right before its native check, so the call that turns a function hot
already runs native.  That covers callers outside the VM too:
`cl_vm_apply`'s stub frame is an `OP_CALL`, and `jit_dispatch`
(`runtime.c`, every call from native code) hands an interpreted callee to
that stub frame.  `jit_dispatch` must not count as well — it did at first,
and a native caller's callee turned hot after half the threshold.  The `vm.c` hunks are inside `#ifdef JIT_M68K`: the
host VM is byte-for-byte what it was.  A settled function pays one byte
compare per call.

**Tail calls from bytecode.**  The VM's native fast path used to skip
tail calls (native code cannot reuse an interpreted frame).  Under eager
compilation that rarely mattered — the caller was native too.  Now the
caller of a hot loop is often interpreted (an entry function that runs
once and ends in `(main-loop)`, a benchmark's thunk), and the skip kept
the compiled loop running as bytecode for good: the first measurement
run showed `bench-jit-call.lisp` with no JIT gain at all in the default
mode.  A tail call into native code now calls it and returns its values
from the caller's frame (a jump into `OP_RET`'s body); the frame stays
for one native call, since the callee's own tail calls happen in native
code (self-recursive ones as a branch).

**Loop detection** is `cl_bytecode_has_backward_jump` (`peephole.c`, next
to the decoder it mirrors; host-tested in `tests/test_peephole.c`), run
once per function, on its first counted call — never for the majority of
functions, which are never called.

**GC.**  The JIT allocates only off-heap (`platform_alloc`), so
`cl_jit_note_call` cannot move the caller's raw `CL_Bytecode *`.

**Threads.**  Two threads can reach the threshold of one function
together.  The hot path (`jit_compile_impl(bc, 0)`) never frees native
code: it compiles into its own buffer and installs it only if the
bytecode still has none, otherwise throws its own away.  The codegen is
deterministic, so the rare pair that both install (one preempted between
the check and the stores) writes identical code and reloc tables — the
loser's buffer leaks, nothing is freed under a running caller.  No CAS,
no "compiling" state.

**Images.**  Restore resets the count and keeps the hint
(`bc->jit_hot &= CL_BC_JIT_SPEED`), so restored functions compile again
as they turn hot; a restored speed-3 function compiles on its first call.

## Tests

- `tests/test_peephole.c` `backward_jump_detection` — the decoder.
- `tests/test_fasl.c` `serialize_bytecode_speed3_jit_hint`,
  `compiler_records_speed3_jit_hint` — the hint, compiler to wire and back.
- `tests/test_image.c` `restore_restarts_jit_call_count_keeps_speed_hint`.
- `tests/amiga/test-jit.lisp`, "Hot compilation" — threshold, loop rule,
  speed 3, JIT-off definitions, native callers counting their callees,
  threshold 1, `%JIT-DISASSEMBLE`.
- `tests/amiga/image-verify.lisp` — a function restored from an image is
  native once hot.

## Differences from the 2026-06 proposal

- One byte in existing padding instead of a `uint16_t` counter plus a
  state byte (the struct would have grown by 4 bytes per function on m68k
  and changed `image.c`'s layout hash).
- No `JIT_COMPILING` state or CAS publish: deterministic codegen plus
  "never free on the hot path" makes the race harmless.
- A function reached only from native code turns hot like any other: its
  calls reach `OP_CALL` through `jit_dispatch`'s stub frame.
- The loop rule, the speed-3 hint and eager mode are new.
- The eager call sites were not removed but routed through
  `cl_jit_note_definition`, which is also where eager mode lives.
