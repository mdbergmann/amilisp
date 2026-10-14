# Native Code Backend (AArch64)

Status: phases 0 and 1 on master, phase 2 on branch `feat/a64-jit-p2`, phase 3 on
`feat/a64-jit-p3` (2026-10-02; see each phase's status note).  Proposal from 2026-10-01; both
prerequisites are on master: direct native-to-native calls
(`specs/jit-direct-calls.md`, 27c74b00) and the loop poll
(`specs/native-backend.md`, "Status (2026-10-01, native loops poll)",
b8f57acc).

A template JIT for 64-bit ARM hosts, Apple Silicon macOS first, then
Linux.  It is the m68k backend's sibling (`specs/native-backend.md`): it
uses the same bytecode, lazy compilation policy (`specs/lazy-jit.md`),
runtime helpers (`src/jit/runtime.c`) and dispatch path.  Only instruction
selection, the frame layout and the executable-memory plumbing are new.

## Motivation

- **Speed on the development host.**  Interpreted code on an M-series
  Mac is fast, but fixnum loops, struct access and call-heavy code still
  pay for dispatch on every opcode.  The m68k numbers
  (`trunk/bench-jit-loop.lisp`, `trunk/bench-jit-call.lisp`) show what a
  template JIT takes off that.
- **JIT bugs found under `make test`.**  Today every JIT change is
  verified only in FS-UAE.  A second backend that shares the walker's
  structure and all of `runtime.c` runs the runtime helpers, the
  hot-compile policy, NLX through native frames and the GC interaction
  natively, under the gc-stress suite, in CI (`macos-latest` is arm64).
- **A second CPU before MorphOS.**  A PPC JIT would follow the same
  recipe; doing it once on the host proves the split.

## Non-goals

- No optimizing tier, no speculation, no deoptimization (see the
  evergreen assessment that led here: the template JIT's inline fast path
  with a slow-path call already behaves like speculation that never fails).
- No x86-64 backend.
- No Windows ARM64 in this spec: `longjmp` there unwinds with SEH and
  would need unwind tables for every JIT frame (evergreen's
  `native_transfer/win64.rs` is that work for x86-64).  The build keeps
  the stubs on Windows.

## What we take from evergreen, and what not

Evergreen (github.com/atgreen/evergreen) is GPL-3.0 with a Classpath
exception; amilisp is Apache-2.0.  **No code is copied.**  Encodings come
from the Arm ARM (DDI 0487, C4.1); the ideas below are design, not text.

Taken:

1. **Encoders are plain functions.**  One C function per instruction,
   returning the 32-bit word, or failing when an operand does not fit its
   field so the caller emits a longer form (`movz`/`movk` for a wide
   constant, a register-offset load for a far displacement).  Every
   encoder is pinned by a unit test.
2. **A sibling walker, not a portability layer.**  `jit_walk_a64.c`
   mirrors the m68k walker case by case, with its own register roles.
   No abstract "emit add" interface over both CPUs.
3. **One label/fixup pass.**  Branches go to labels that are bound later;
   every displacement is filled in and range-checked in one `finish` step.
4. **A first slice that cannot be wrong.**  Phase 1 runs the operand
   stack and control flow natively and sends every other opcode to the
   runtime, so its results are by construction the interpreter's.
5. **The JIT's frame is the interpreter's frame.**  Locals and the
   operand stack live on the GC-rooted `cl_vm.stack`, not on the native
   stack (see "GC interaction").
6. **Full instruction-cache maintenance.**  Their `jit.rs` documents an
   `ISB`-only flush that worked by accident.  We call
   `sys_icache_invalidate` on macOS and `__builtin___clear_cache` on Linux.

Not taken: tiering beyond lazy compile, type-feedback tables, deopt,
OSR, `regalloc2`.

## Architecture constraints from our runtime

- `CL_Obj` is a 32-bit arena offset, tag bit 0 = fixnum
  (`src/core/types.h`).  All Lisp values are handled in `w` registers;
  a heap address is `arena_base + uxtw(obj)`, which AArch64 addresses in
  one instruction: `ldr w0, [x23, w1, uxtw]`.
- VM stack slots are 4 bytes (`CL_Obj *cl_vm.stack`, index `cl_vm.sp`).
  The VM stack and the arena are allocated once and never move
  (`vm.c:257`, `mem.c:1052`).
- The host runs threads (per-thread `CL_Thread`, `cl_vm` = `CT->vm`),
  stop-the-world GC, and by default the generational collector, whose
  minors **move** young objects (`specs/generational-gc.md`).  The
  generational GC tracks old-space writes by `mprotect` faults, so stores
  from JIT code need no write barrier; the fault handler is address-based
  and does not care that the faulting PC is in JIT code.
- Host error frames use `_setjmp`/`_longjmp` (`src/core/error.h:29`), which
  on macOS and Linux arm64 restore registers without unwinding.  A
  `cl_error` from inside a helper drops the native frames above the
  catching C frame, exactly as on m68k.

## Register roles

AAPCS64 makes x19-x28 callee-saved, so these survive every helper call.
**x18 is reserved on Apple platforms and never used.**  x29/x30 form a
standard frame record so `lldb`, `sample` and crash reports walk through
JIT frames.

| Role                                  | m68k      | AArch64 |
|---------------------------------------|-----------|---------|
| current `CL_Thread *`                 | A3 (after direct-calls phase 2) | x19 |
| operand-stack pointer (next free slot in `cl_vm.stack`) | A7 (m68k stack) | x20 |
| locals base `bp` (`cl_vm.stack + bp`) | A6 frame  | x21 |
| literal pool (heap constants)         | baked immediates | x22 |
| arena base                            | A5        | x23 |
| top-of-stack cache (phase 2)          | D5-D7     | w24-w26 (built: w13-w15) |
| the function's `CL_Frame` (phase 3)   | -         | x25 |
| the entry's `nargs` (`OP_ARGC`)       | D1 at entry, then -4(a6) (`&optional`); 16(a6) (`&key`) | w24 |
| result / first helper argument        | D0        | w0 / x0 |
| scratch                               | D1-D3     | x9-x15 |
| far-call veneer                       | -         | x16, x17 |

Helper calls load the 64-bit helper address from a per-function constant
area at the end of the code (`ldr x16, lit; blr x16`).  Helper addresses
never move, so this area is never patched.

## Entry and frame layout

The native entry has one signature for every arity.  On m68k,
`cl_jit_invoke` enters native code through the assembly trampoline
`cl_jit_enter(void *entry, CL_Thread *thread, CL_Obj func, const CL_Obj
*argv, int32_t nargs)` (`src/jit/jit_enter_m68k.s`), which loads A3 and
copies the arguments onto the m68k stack.  AArch64 needs no trampoline:
the arguments stay where the caller pushed them, and the AAPCS64 argument
registers carry the rest, so `cl_jit_invoke` calls the entry directly:

```c
typedef CL_Obj (*cl_a64_entry_t)(CL_Thread *thr, CL_Obj *bp,
                                 uint32_t nargs, CL_Obj func_obj);
```

The arguments are already on `cl_vm.stack` (the caller pushed them).
The prologue:

1. saves x19-x26 and the frame record;
2. checks that `bp + n_locals + max_stack + 1` fits in the VM stack, and
   otherwise calls the helper that raises the interpreter's own
   stack-overflow error;
3. sets non-argument locals to NIL;
4. stores `func_obj` in a hidden slot right after the locals, so closure
   upvalues (`OP_UPVAL`) read it from rooted memory after any GC;
5. points x20 at the slot after that.

Frame in `cl_vm.stack`: `[args | other locals | func_obj | operand stack ...]`.
A `&key` function uses the same entry; its keyword prologue is the existing
`cl_jit_runtime_kw_prologue` helper, which already works on args in the VM
stack.  (Built: `cl_jit_vmstack_ll_prologue`, which also does `&rest`; see
"&optional and &rest" below.)

Who pops the arguments is unchanged: `cl_jit_invoke`'s contract with its
callers stays as it is today.

## GC interaction

The m68k JIT keeps its operand stack and interior locals on the CPU stack.
The GC therefore scans that stack conservatively and pins whatever it
finds (`specs/native-backend.md`, "GC interaction"), a scheme built for the
classic collector.  The generational collector has neither the header
index nor the pinning (`native-backend.md:451`).  The AArch64 backend
avoids all of it.

**Rule 1: at every call out of native code, every live `CL_Obj` is in
`cl_vm.stack` below `cl_vm.sp`.**
- Before a helper call (and before the `_setjmp` of an NLX frame) the
  code stores the top-of-stack cache registers to their slots and writes
  `cl_vm.sp` from x20.
- After the call it reloads x20 from `cl_vm.sp` and refills the cache
  lazily.
- No `CL_Obj` and no raw arena pointer lives in a register across a call.

Then the GC needs no knowledge of JIT frames: `cl_vm.stack` is already a
root, the hidden `func_obj` slot keeps the function and its bytecode
alive, and a moving minor or major collection forwards everything in
place.  `gc_scan_jit_native_stack` and the `jit_stack_top` bookkeeping
stay m68k-only.  This rule is the AArch64 form of the m68k
`cache_flush`-before-JSR rule, with memory instead of the CPU stack as
the target.

**Rule 2 (as built in phase 1): heap constants are read from
`bc->constants` at each use, never encoded in instructions.**  The prologue
loads the array's address (a 64-bit literal after the code) into x22, and
`OP_CONST` is `ldr w, [x22, #4*i]`.  The array is `platform_alloc`'d once
when the bytecode is born and never moves, and both collectors -- minor
collections included -- already forward its words in place, because the
interpreter reads constants from it.  `T` is read through the address of
the `CL_T` global.  So native code bakes no heap object at all: no pool, no
relocation table, and Rules 2 and 3 below as first proposed (kept for the
record) are not needed.

*As first proposed:* heap constants are loaded from a literal pool, never
encoded in instructions.  An AArch64 instruction cannot carry a 32-bit immediate
the way an m68k `move.l #imm` does, so the m68k scheme of patching
immediates inside the code does not carry over.
- Each compiled function gets a pool: an array of 32-bit words holding
  every heap `CL_Obj` the code uses (quoted constants, symbols, the
  self-bytecode, closure templates, `T`).
- The pool is ordinary `platform_alloc` memory, not executable memory.
  The prologue loads its address into x22.
- Code reads constants with `ldr w0, [x22, #4*i]`.

**Rule 3: both collectors forward every pool word.**
- On AArch64 `bc->native_relocs` points at the pool and
  `native_reloc_count` is its length.  The field is reused, so the
  `CL_Bytecode` layout is unchanged and neither `CL_IMAGE_VERSION` nor
  `CL_FASL_VERSION` changes.
- `GC_BYTECODE_TAIL` in `mem.c` gets an AArch64 branch that forwards each
  native-endian word in place.  Since the pool is data, no
  instruction-cache flush and no write-protect toggle is needed.
- The open point is **minor collections**: an old bytecode on a clean page
  is not visited by a minor, but its pool may hold a young constant that
  the minor moves.
  - Every installed pool is registered in a global list (added on
    install, removed when the bytecode's native code is freed).
  - Each collection, minor or major, forwards every word in that list
    after forwarding is computed.
  - A pool is a copy of `bc->constants`, so liveness still comes from the
    bytecode; the list only forwards and never marks.
- `tests/test_gc_jit_reloc.c` gets an AArch64 counterpart covering both
  collectors.

**Rule 4: native loops poll.**  The interpreter runs `CL_SAFEPOINT` and
the Ctrl-C poll on every backward jump (`vm.c`, `OP_JMP`).  On the host
a call-free native loop would otherwise stall every stop-the-world GC in
other threads forever.  Every loop header emits:

```
ldrh w9, [x19, #offsetof(CL_Thread, jit_loop_ctr)]
subs w9, w9, #1
strh w9, [x19, #offsetof(CL_Thread, jit_loop_ctr)]
b.lo poll_stub              ; borrow: sync sp, call cl_jit_runtime_loop_poll, reload
```

The poll counter is per-thread, as the hot-path rule in `CLAUDE.md`
requires.  The m68k walker lacked this poll too and gained it on
2026-10-01 (`specs/native-backend.md`, "Status (2026-10-01, native loops
poll)").  Its form is cheaper than the sequence above, and AArch64 should
copy it: a per-thread countdown `CL_Thread.jit_loop_ctr`, decremented at
every loop header and before each self-tail-call branch, and a call to
`cl_jit_runtime_loop_poll` every 256 iterations.  That helper runs the
safepoint, interrupt and Ctrl-C checks.  The `jit-loop-poll-*` checks in
`tests/amiga/test-jit.lisp` are the tests to port.

## Executable memory

A platform interface (`platform.h`, under `JIT_A64`), implemented in
`platform_posix.c` and used only by the code heap:

```c
void *platform_jit_map(uint32_t bytes);          /* a MAP_JIT chunk, or NULL  */
void  platform_jit_unmap(void *addr, uint32_t bytes);
void  platform_jit_write_begin(void);            /* this thread may write     */
void  platform_jit_write_end(void);              /* back to execute           */
void  platform_jit_flush(void *addr, uint32_t len); /* I-cache maintenance    */
```

- **macOS**: one `mmap(MAP_PRIVATE|MAP_ANON|MAP_JIT, PROT_READ|PROT_WRITE|
  PROT_EXEC)` region per chunk.
  - `pthread_jit_write_protect_np(0)` before writing and
    `pthread_jit_write_protect_np(1)` after; the toggle is per-thread, so
    a thread compiling does not stop others from running JIT code.
  - Then `sys_icache_invalidate(p, n)`.
  - A locally built binary is not signed with the hardened runtime and
    needs no entitlement.  A signed build would need
    `com.apple.security.cs.allow-jit`.
- **Linux** (phase 4): a `memfd` mapped twice, a writable view and an
  executable view, because flipping `mprotect` on a page another thread
  is executing from is not safe.  Then `__builtin___clear_cache`.
  - So `platform_jit_map(bytes, &writable)` returns both views (the same
    address on macOS) and `platform_jit_unmap` takes both; the write
    window calls are no-ops on Linux.
  - The code heap keeps its block headers and free list in the writable
    view and translates only at its edges: install returns the
    executable address, free takes it back.

**Code allocator** (`src/jit/codeheap.{c,h}`):
- Apple pages are 16 KB, so one mapping per function would waste most of
  the memory.  Functions are carved from 1 MB chunks with a first-fit
  free list (16-byte granules, a block header inside the chunk, under a
  mutex, since threads compile concurrently).  A block larger than a
  chunk gets a chunk of its own.  Free blocks are not coalesced yet.
- `cl_codeheap_install(code, len)` copies finished code in and flushes;
  `cl_codeheap_free` refuses pointers it did not hand out and double frees.
- The bytecode sweep frees native code through `cl_jit_free_native`
  (each backend's own allocator: `platform_free` on m68k, the code heap
  here).
- `cl_jit_shutdown`, called in `main()` after `cl_mem_shutdown` (which
  released every function's code), unmaps the chunks;
  `tests/test_memleak_tracked.sh` has the `no_leak_after_jit_stubs`
  scenario.

## Code layout in the tree

```
src/jit/jit_common.c        hot policy, on/off switches, stats, invoke
                            bookkeeping -- moved out of the m68k walker
src/jit/jit_m68k.c          the m68k walker and its compile driver
src/jit/jit_a64.c           the AArch64 walker and its compile driver
src/jit/asm_a64.{c,h}       encoders + label/fixup
src/jit/codeheap.{c,h}      executable-memory allocator (AArch64 only)
src/jit/runtime.{c,h}       helpers every backend shares (CPU-neutral slow
                            paths: arithmetic, cells, globals, structs, ...)
src/jit/runtime_m68k.{c,h}  the m68k walker's own: its operand stack is the
                            m68k stack (call sites, inline-setjmp NLX frames,
                            the &key prologue, OP_AMIGA_CALL)
src/jit/runtime_vmstack.{c,h}  a walker whose frame lives in cl_vm.stack
                            (cl_jit_vmstack_*: calls, tail calls, CONS/LIST)
```

The helpers are split by frame design, not by CPU (after phase 1, before
phase 2): a PPC or x86-64 walker built like the AArch64 one reuses
`runtime.c` and `runtime_vmstack.c` and adds only its walker and encoders.
Phase 0 and 1 below still say `jit.c` -- the m68k walker's name then.

- `jit.h` guards become `#if defined(JIT_M68K) || defined(JIT_A64)`.
- The `Makefile` defines `JIT_A64` when `uname -m` is `arm64`/`aarch64`
  and the system is Darwin or Linux (Linux since phase 4), and adds the
  new files.
- `--no-jit`, `%JIT-SET-ACTIVE`, `%JIT-SET-HOT-THRESHOLD` and the lazy
  policy behave as on m68k.
- `%JIT-DISASSEMBLE` gets a small decoder for the instruction forms
  `asm_a64.c` emits, plus a `.word` fallback, as on m68k.

## Phases

Each phase ends with `make test`, `make test-gc-stress` and
`make test-memleak` green on an arm64 Mac, and on an x86-64 host as
well, where the backend compiles to stubs.  Phase 0 touches `jit.c`,
so it also needs `make -f Makefile.cross test-amiga`.

### Phase 0: split and plumbing
- Move the CPU-independent parts of `jit.c` into `jit_common.c`: the
  hot-call policy (`cl_jit_note_definition`, `cl_jit_note_call`, the
  threshold), the on/off switches and the counters.  The m68k walker
  stays in `jit.c`, which keeps the change to the m68k binary small;
  `test-amiga` decides.  Moving the walker into a file of its own can
  come later, if it ever helps.
- Add `JIT_A64`, `asm_a64.c` with the encoders phase 1 needs, the code
  heap and the platform calls.
- Make `cl_jit_emit_stub` (`%JIT-COMPILE-STUB`) emit and run a `ret`
  stub.
- Tests:
  - `tests/test_asm_a64.c`: every encoder against its known word,
    recorded once from `clang -c` output; the test itself needs no
    assembler.
  - Encoder failure on out-of-range operands.
  - Label fixups, forward and backward, including the out-of-range
    error.
  - On arm64 only, assemble tiny functions and call them.

**Status (2026-10-01): done** on branch `feat/a64-jit`.
- `jit_backend.h` is the seam: `jit_common.c` owns the policy and the
  counters, and each backend supplies `cl_jit_backend_init/_compile/
  _shutdown`, `cl_jit_invoke`, the stub, the disassembler and
  `cl_jit_free_native`.  `cl_jit_invoke` stays per backend: the m68k one
  sets up the conservative-scan window and the C-stack floor, which
  AArch64 does not need.
- `jit_a64.c` declines every function.  `%JIT-COMPILE-STUB` installs
  `mov w0, #0; ret`, OP_CALL enters it, and a redefinition, a restub or
  the sweep frees it.  `%JIT-SET-DIRECT-CALLS` reads NIL (no call sites).
  No boot banner on the host.
- `asm_a64.c` is compiled on every host, so `tests/test_asm_a64.c` (89
  encodings pinned to clang, the `A64_BAD` contract, `a64_mov_imm`, label
  fixups and their failures) runs on x86-64 too.  `make host JIT=0`
  builds an arm64 Mac without the backend.
- Tests: `test_asm_a64.c`, `test_codeheap.c` (execution, reuse,
  splitting, refused frees, an oversize chunk, four threads churning,
  shutdown), `test_jit_a64.sh` (also in `make test-gc-stress`), and the
  `no_leak_after_jit_stubs` memleak scenario.

Done in phase 1:
- `cl_jitc_invoke_count` is a process-wide counter written on every native
  entry.  That is harmless for stubs, but under threads it is the
  shared-cache-line cost the hot-path rule in `CLAUDE.md` forbids; make
  it per-thread (or count only in debug builds) before native code runs
  hot.
- `vm.c` still gates the hot-call counting and the native tail call on
  `JIT_M68K`.  Phase 1 switches them to `CL_JIT_NATIVE`, and since that
  touches `cl_vm_run`, the `vm.*` rows of bench-opt must be compared
  against `docs/benchmarks.md`.

### Phase 1: the walker that cannot be wrong
- Native operand stack, locals, constants from the pool, jumps with the
  backward-branch poll, `OP_RET`, the entry/prologue above, and calls
  through the existing `jit_dispatch` helpers.
- Every other opcode the m68k walker handles is a call to its existing
  runtime helper, or a decline (the function stays interpreted).
- No inline fast paths and no register cache yet.
- Tests:
  - The behavioural sections of `tests/amiga/test-jit.lisp` run on the
    host through a new shell test, with JIT on, eager
    (`%JIT-SET-HOT-THRESHOLD 0`).  The m68k byte goldens are guarded with
    `#+m68k`, and new AArch64 goldens are added for the matcher shapes.
  - The same run under `AMILISP_GC_STRESS=1`.
  - The whole `make test` Lisp suite once with eager JIT, as a
    differential run against the interpreter.
  - A threaded test: a call-free native loop in one thread while another
    thread forces GCs, which must finish (Rule 4).

**Status (2026-10-01): done** on branch `feat/a64-jit`.
- `jit_a64.c` walks the bytecode once.  Covered: the stack and local
  opcodes and their superinstructions, constants, every branch (with the
  loop poll at each backward-branch target), `RET`, the calls (`CALL`,
  `CALL_GLOBAL` and its fused heads, `APPLY`), arithmetic and comparisons,
  `EQ`/`NOT`, `CAR`/`CDR`/`CONS`/`LIST`/`RPLACA`/`RPLACD`, the global and
  function cells, struct slots, `ASET`/`AREF`, `ASSERT_TYPE`, and the
  string-scan opcodes.  Each runs the helper the m68k walker calls, or an
  AArch64 one (now `runtime_vmstack.c`) that takes pointers into `cl_vm.stack`
  (`cl_jit_vmstack_*`), and writes `cl_mv_count` where the interpreter
  does.  `EQ`, `NOT` and the branches are inline.  Everything else declines:
  the NLX frames, dynamic binding, `PROGV`, the multiple-value opcodes,
  closures and upvalues, `&key`/`&optional`/`&rest`, handlers and restarts.
- The walker tracks the operand-stack depth; an edge whose depth disagrees
  with its target's declines the function, and the deepest point sizes the
  prologue's overflow check (bytecode carries no max-stack field).
- A self tail call reuses the frame behind a runtime guard
  (`cl_jit_vmstack_is_self`: same bytecode, nothing traced), storing the
  callee as the frame's function.  Any other tail call to a native callee is
  handed to `cl_jit_invoke` (`CL_Thread.jit_tail_pending`): the arguments
  are moved to the frame base and the callee entered from there, so mutual
  tail recursion between native functions runs in constant space, as in the
  interpreter.  A tail call into an interpreted function is still a nested
  call, as on m68k; hot compilation makes such a chain native quickly.
- `vm.c`'s hot-call counting and its tail call into native code are
  switched from `JIT_M68K` to `CL_JIT_NATIVE`; `%JIT-INVOKE-COUNT` counts
  per thread (`CL_Thread.jit_invoke_count`) on AArch64.
- `AMILISP_JIT_HOT=N` sets the hot threshold at boot; `make test-jit-eager`
  runs the fast tier with every function compiled at definition.
- Tests: `tests/test_jit_a64_walk.sh` (also under gc-stress) runs the
  behavioural checks of `tests/amiga/test-jit.lisp` eager -- its
  m68k-specific checks are now `#+m68k` -- and this backend's own: the
  covered shapes, bignum/ratio/float slow paths, deep and runaway
  recursion, self and mutual tail calls, errors unwinding through native
  frames, values across collections, redefinition, the multiple-value
  state, native code on four threads, and the loop poll (a call-free native
  loop in one thread while another collects).
- `make test-jit-eager` (the fast tier, every function compiled at
  definition) computes every result the interpreter does.  What fails
  there is introspection only: a native frame is not in the VM's frame
  list unless `%JIT-SET-FRAMES` is on, so the backtrace, line-attribution
  and `FRAME`-locals tests (`test_debugger_backtrace`,
  `test_backtrace_lines`, `test_backtrace_after_handled_error`,
  `test_call_diag`, `test_dev_commands`' frame session, two lines each of
  `test_tier4_phase2/3` and `test_fasl_source_name`) miss the native
  callee's frame -- the m68k JIT's open item too.  Phase 3's shadow frames
  close it.
- No inline fast path yet, but the dispatch is gone: on an M-series Mac
  `trunk/bench-jit-call.lisp` runs every row faster than the interpreter
  (builtin calls and native leaves 2-3x, `case` dispatch 3x, the mixed
  decode-key rows 1.8x; a call into a bytecode leaf or an `&optional` one
  equal), and none slower.  The `vm.*` and `mt.*` rows of `bench-opt`
  (interpreted) are unchanged within 1 ms against phase 0, three
  interleaved pairs.

### Phase 2: inline fast paths and the register cache
Fixnum templates check both operands before touching the stack (taken
from evergreen), so a failed check leaves the frame as the slow path
expects:

```
and  w9, wa, wb  ; tbz w9, #0, slow        ; both fixnums?
add:  sub w9, wb, #1 ; adds wr, wa, w9 ; b.vs slow
sub:  subs wr, wa, wb ; b.vs slow ; add wr, wr, #1
mul:  asr w9, wa, #1 ; sub w10, wb, #1 ; smull x11, w9, w10
      cmp x11, w11, sxtw ; b.ne slow ; orr wr, w11, #1
<,=:  cmp wa, wb ; cset / b.cond            ; tagged order = numeric order
```

Also inline: `EQ`, `NOT`, `CAR`/`CDR` with the type check, `GLOAD`/`GSTORE`
fast paths, `STRUCT_REF`/`STRUCT_SET`.  A 3-slot top-of-stack cache in
w24-w26 removes most `LOAD`/`STORE`/`POP` memory traffic.  Each new
template gets a gc-stress case that forces its slow path, i.e. a non-fixnum
or an allocating helper.

**Status (2026-10-02): done** on branch `feat/a64-jit-p2`.
- Inline: `+ - *` and `< > <= >= =` on fixnums (the templates above;
  `*` is `smull` with a 32-bit fit check, so it covers the whole fixnum
  range, not only the interpreter's 15-bit operands), the fused `CMP_BR`,
  `EQ`/`NOT`, `CAR`/`CDR` (NIL inline, a cons by its header),
  `STRUCT_REF`/`STRUCT_SET` (header type and slot count checked; the store
  keeps the interpreter's publication barrier, a `dmb ish` taken only
  while `cl_thread_count > 1`), and `GLOAD`/`GSTORE` and the fused
  `GLOAD_JNIL`/`GLOAD_EQ_JNIL` while the thread has no dynamic binding
  (`tlv_entry_count == 0`, as in the interpreter's `VM_GLOBAL_VALUE`).
  NIL as the symbol, an unbound value, `*PACKAGE*`'s store, `CHAR=`, `/`
  and every non-fixnum go to the phase-1 helper.  x23 holds
  `cl_arena_base`, loaded in the prologue (the frame is 80 bytes now).
- Each fast path checks its operands before it changes anything, so a
  failed check branches to an out-of-line slow path (after the body) with
  the cache as it was: it moves the operands into w0-w2, flushes, calls
  the helper, reloads the cache from `cl_vm.stack` (a collection may have
  moved what it held), rewrites `cl_mv_count` if the fast path knew it to
  be 1, and rejoins with the result in w0.
- The cache lives in the caller-saved w13-w15, not w24-w26: it is never
  live across a call (Rule 1), so callee-saved registers would only cost
  saves.  Up to three values; a fourth push spills the deepest.  Labels,
  branches and every opcode without a template see an empty cache; a
  return needs no flush.  A helper call with a non-empty cache is a walker
  bug and declines the function.
- `cl_mv_count = 1` is elided while it is known to be 1 already (from one
  such write to the next helper call or label).
- Tests: `tests/test_jit_a64_walk.sh`, phase-2 section (also under
  gc-stress): every template against its slow path -- fixnum boundaries
  for `+ - *` (`most-negative-fixnum * -1`, 46341^2), ratios, floats,
  complex, type errors, NIL/cons/struct/char under `CAR`, out-of-range
  struct slots, special variables global, bound, unbound and `*PACKAGE*`,
  a five-deep expression that spills, the values count, and struct writes
  on four threads -- each with a heap value held in the cache while the
  slow path allocates.  Dropping the cache reload after a slow-path call
  makes the gc-stress binary fail already at boot.  `make test-jit-eager`
  fails exactly the phase-1 introspection set.
- Bench (M-series, bench-opt with the JIT left on, eager): see
  `docs/benchmarks.md`, 2026-10-02.  The stop criterion (1.5x on `vm.*`)
  is met: 3-14x.

### Phase 3: parity with the m68k walker
- NLX frames (`BLOCK`, `CATCH`, `TAGBODY`, `UWPROT`, `HANDLER_CASE`) use
  the same split as m68k: helper `*_alloc`, inline `_setjmp`, helper
  `*_commit`, and `post_longjmp` on the second return.
  - `_setjmp` restores x19-x28, so x20 must be reloaded from `cl_vm.sp`
    at the landing, as after any call.
- Also: dynamic binding, PROGV, multiple values, closures and upvalues,
  `&key` prologues, the string-scan opcodes.
- Target: the m68k walker's opcode list, minus `OP_AMIGA_CALL`.
- Shadow frames (`%JIT-SET-FRAMES`) work unchanged and now show every
  local, since the locals are on the VM stack.

**Status (2026-10-02): done** on branch `feat/a64-jit-p3`.
- The NLX helpers, the value-save stack and the handler/restart bindings
  moved from `runtime_m68k.c` to `runtime_nlx.{c,h}` unchanged (`RESTART_PUSH`
  and the m68k `&key` prologue stay: their arguments are m68k-stack words);
  both walkers call them.  The m68k objects are otherwise the same code.
- The AArch64 walker now takes every opcode of the m68k walker but
  `OP_AMIGA_CALL` and `OP_ARGC` (only `&optional`/`&rest` prologues emit it,
  and those functions still decline): the five NLX frames, `DYNBIND`/
  `DYNUNBIND`, `PROGV_BIND`/`_UNBIND`, `MV_LOAD`/`MV_TO_LIST`/`NTH_VALUE`/
  `MV_SAVE`/`MV_RESTORE`, `CLOSURE`, `UPVAL`, the cell opcodes, the handler
  and restart bindings, and `&key` functions.
  - NLX: `*_alloc` returns `&nlx->buf`, the native code calls `_setjmp` on it
    itself (`blr`), `cbz` to the commit; the longjmp arm calls
    `*_post_longjmp`, reloads x20 from `cl_vm.sp`, pushes what the transfer
    brought and branches to the landing (HANDLER-CASE: to the matched
    clause's `OP_JMP` in the table).  The walker's depth check covers the
    landings: each gets the push's depth plus the arm's value.
  - `UPVAL`, `CELL_REF` and `CELL_SET_LOCAL` are inline and use the cache;
    the closure template and `RESTART_PUSH`'s name are passed as addresses
    of `bc->constants` words and read after the allocation
    (`cl_jit_vmstack_make_closure`, `_restart_push`), the captures staged on
    the VM stack.
  - `&key`: `cl_jit_vmstack_kw_prologue` runs first and builds the whole
    frame (the keyword pairs sit where the other locals go), with the
    interpreter's matcher and errors.  A self tail call stays off for `&key`.
- Frames: `cl_jit_invoke` pushes the native function's `CL_Frame` (bp,
  `n_locals`, the function) and passes it as a fifth entry argument (x25);
  before each helper call the code writes the opcode's ip into it (elided
  while unchanged), as the interpreter does before a call.  So
  `EXT:BACKTRACE`, the call-site diagnostics and `FRAME` see a native frame
  as an interpreted one.  On by default here; `%JIT-SET-FRAMES NIL` turns the
  push off (the ip then goes into a scratch frame).  A native call now
  counts against the 1024 frames and overflows with the interpreter's "Call
  stack overflow".  An arity error raised from the stub frame of a native
  call names the frame under the stub as the caller (`frame_site_brief`).
- Tests: `tests/test_jit_a64_walk.sh` part 3 (also under gc-stress, and once
  more with `AMILISP_GENGC=0`): each frame kind both ways through native
  frames with values held across, nested cleanups and a throw from one,
  handler-case/-bind, restarts, special bindings restored by an error and a
  throw, PROGV unbound and `*PACKAGE*`, multiple values, closures over
  locals, upvalues and a template that moves, every `&key` case and error,
  four threads, `FRAME-LOCALS`, and the backtrace lines.  Dropping the
  frame-ip store, the CATCH tag pop, or reading the closure template before
  the allocation (classic collector) each fails it.
- `make test-jit-eager`: the phase-1 introspection set passes now; the only
  remaining differences were the two expectations of this backend's own test
  (frame size, recursion depth).
- Bench (bench-opt, JIT on, eager, medians of three; `docs/benchmarks.md`,
  2026-10-02): phase 2 -> 3, `kw.call-8keys` 58 -> 26 ms, `mt.dynbind-x8`
  34 -> 11, the `clos.slot-value`/`struct.*` rows 2-4x; every other row
  within 2 ms of phase 2.  Frames on or off is within noise.

### Phase 4: Linux arm64
- Memfd double mapping, `JIT_A64` on Linux.
- An `ubuntu-24.04-arm` CI job.

Status (done, 2026-10-02):
- `platform_jit_map` returns two views and the code heap writes only
  through the writable one (see "Executable memory").  The walker needed
  no change: nothing it emits depends on Darwin (x18 is left alone
  anyway, glibc exports `_setjmp`).
- `test_codeheap` checks that both views hold the same bytes and, on
  Linux, that `/proc/self/maps` lists them `r-xs` and `rw-s`: no page is
  ever writable and executable.
- Gates on Linux arm64 (Debian bookworm, `verify/linux-arm64/run.sh`):
  `make test`, `test-jit-eager`, `test-gc-stress`, `test-memleak` 20/20,
  and the `JIT=0` build all pass; the walker test is 102/102.  The same
  gates pass on the arm64 Mac.
- CI: `ubuntu-24.04-arm` added to the host-test matrix.

### Phase 5: direct calls
Native-to-native calls without the C helper path, after the m68k walker's
design (`specs/jit-direct-calls.md`): every `OP_CALL`/`OP_CALL_GLOBAL` site
(and the fused heads) owns a cell guarded by `cl_call_gen`; a hit enters
the callee's native code directly, a miss takes the helper path as before
and fills the cell.  The `cl_call_gen` bumps, the setter funnel and its
lint are the m68k work, unchanged.

What differs from m68k:
- **The cell is one 64-bit word**, `gen | func << 32`, written in one store
  and read in one load.  The m68k three-word cell (gen, func, entry) relies
  on a single CPU; on a multi-core host a reader could pair one fill's
  func with another's entry.  Nothing else is cached: the hit re-derives
  the callee's bytecode from func (through a closure) and reads its
  `native_code`, NULL -> miss.
- **The hit path pushes the CL_Frame** `cl_jit_invoke` would (frames are on
  by default here), checks the C stack against `c_stack_base -
  CL_C_STACK_LIMIT` (64 K margin) and the frame stack's room -- either
  failing is a miss, whose helper signals the interpreter's errors -- and
  hands a pending tail call (`jit_tail_pending`) to `a64_tail_finish`.
- **`&key` callees fill too**: the AArch64 entry ABI is one signature for
  every lambda list, and the callee's own prologue matches the keywords.
- **The counters are per thread** (`CL_Thread.jit_ds`;
  `%JIT-DIRECT-CALL-STATS` reads the caller's): the miss path runs on every
  call to a builtin, so a process-wide counter is the shared cache line the
  hot-path rule forbids.
- Fill refuses while frames are off (`%JIT-SET-FRAMES NIL`), anything is
  traced, or the kill switch (`%JIT-SET-DIRECT-CALLS`,
  `AMILISP_JIT_DIRECT=0`) is off.
- The cells are ordinary memory hung on `bc->native_relocs` (this backend
  bakes no heap object, so the count stays 0), freed with the code by the
  sweep, `drop_native` and image restore.

Tests: `tests/test_jit_a64_walk.sh` part 4 -- hits, redefinition through
every public path, FUNCALL and closures through one site, collections and
ABA between calls, TRACE after a fill, multiple values, `&key`, tail-call
handoffs and errors out of direct callees (3000 each: a frame left pushed
overflows the frame stack), the frame limit, backtrace and FRAME-LOCALS
through hits, frames off, the kill switch, per-thread counters, a collector
thread, an interrupt -- and the whole script again with direct calls off,
every line but the counters identical.  Dropping the FUNCALL guard, the
generation check or the frame pop each fails it.

Bench (Apple M-series, interleaved, medians; `docs/benchmarks.md`
2026-10-02): `bench-jit-call` native leaf 25 -> 11 ms per 2 M calls,
FUNCALL 24 -> 12; bench-opt `vm.call-return` 13 -> 5 ms (6.4x the
interpreter), `mt.call-x8` 13 -> 6, `call-args` 20 -> 14,
`kw.call-8keys` 27 -> 23; no row slower.

### &optional and &rest

**Status (2026-10-02): done** on branch `feat/a64-optrest` (AArch64; m68k
`&optional` followed on 2026-10-03, see "Later").

The walker takes every lambda list.  The frame is the one the interpreter's
`OP_CALL` builds: the positional parameters (required and optional) where
the caller pushed them, a missing optional NIL, the `&rest` list in the slot
right after them, every other local NIL, `&key` matched into its slots.
- **`&optional` only**: an inline prologue -- per optional `cmp w24, #i;
  b.hi given; str wzr, [x21, #4i]` -- then the other locals and the
  function slot as for required parameters.  The defaults and supplied-p
  tests are the compiler's own bytecode prologue (`OP_ARGC; CONST k; GE;
  JTRUE`), which the walker compiles like any code.
- **`&rest` or `&key`**: `cl_jit_vmstack_ll_prologue` (it replaces the
  `&key` one) copies the arguments past the positional ones into
  `vm_extra_args_buf` (a root both collectors forward), NILs the rest of the
  frame, stores the function slot, conses the `&rest` list -- the only
  allocation, with everything rooted; the bytecode is re-derived from the
  function slot afterwards -- and matches the keywords.
- **`OP_ARGC`** pushes the fixnum `nargs`: the entry's w2, kept in x24
  (callee-saved; it was the padding of the x23/x24 pair).  Every entry --
  `cl_jit_invoke`, a direct call's hit, the tail-call chain -- passes the
  count in w2, so nothing goes through `jit_current_nargs` (m68k has since
  dropped it too: D1, see "Later").
- **Self tail calls** stay on for `&optional`-only functions with a count
  in `[arity, arity + n_optional]`: the copy NILs from the new count up,
  and the code writes the new count into x24 and the frame's `nargs`
  before branching back over the defaults.  Off with `&rest`/`&key` (a
  tail call that conses or matches goes through the tail helper).
- **The fit rule** (`cl_jit_vmstack_fits`, shared by the dispatch, the
  tail helper and the direct-call fill): the interpreter's `OP_CALL`
  bounds -- `arity <= nargs <= arity + n_optional`, or 255 with `&rest` or
  `&key`.  A call outside them takes the interpreter, which signals the
  arity error.  So a direct-call cell is filled only for a count the
  callee accepts, and the hit path needs no check.

Two compiler bugs surfaced and are fixed with it (interpreter and JIT
alike; `CL_FASL_VERSION` 40): with an optional's supplied-p variable and
`&rest`, the `&rest` slot came after the supplied-p slots, so the first
supplied-p store overwrote the list; and a supplied-p variable was bound only
after every default had run, so a later default could not see it (CLHS
3.4.1).  The compiler now reserves the optional, `&rest` and supplied-p slots
up front and reveals each name once it is bound.

Tests: `tests/test_jit_a64_walk.sh` part 5 -- every shape (`&optional`,
`&rest`, both, with `&key`), defaults that allocate, signal or read a
special, 250 and 300 arguments, self tail calls that change the count
(supplied-p must follow it), a 3000-deep tail into `&rest`, direct calls
and tail handoffs into such callees, closures, four threads -- each run
interpreted, with direct calls off and under the classic collector, every
line identical.  Dropping the self tail call's count update, the NIL of a
missing optional or `OP_ARGC`'s fixnum tag each fails it.

Part 1 now runs all of `tests/amiga/test-jit.lisp`: its FFI block read
`AMIGA.FFI:DEFCFUN`, a reader error on a host that ended the LOAD at line
2189, so a third of the file (direct calls, the entry ABI, the hot path)
had never run on this backend.  The FFI block is `#+amigaos` now, the
m68k-only checks it uncovered are `#+m68k`, the file ends with a marker
the walker test checks, and the minimum count is 500.

### Later
- **Done 2026-10-03:** `&optional` on m68k (specs/jit-direct-calls.md,
  "&optional callees").
- **Done 2026-10-03:** `&rest` on m68k (specs/jit-direct-calls.md,
  "&rest callees").

## Measuring

Run `trunk/bench-opt.lisp`, `trunk/bench-jit-loop.lisp` and
`trunk/bench-jit-call.lisp`, JIT on vs `--no-jit`, interleaved pairs on
one machine.  Record the results in `docs/benchmarks.md`.

**Success:**
- the loop and arithmetic rows at least 3x the interpreter;
- no call row slower than the interpreter.  The m68k JIT made calls
  slower until `jit_dispatch` was fixed, so this is checked from phase 1
  on.

**Stop:** if phase 2 does not reach 1.5x on the `vm.*` rows of
bench-opt, the backend is not worth its maintenance cost, and it stops
there.

## Open questions

- Is a full minor pass over the pool list cheap enough, or should pools
  of old bytecodes be skipped unless a constant was young at install?
  Measure the minor pause with `ext:%gengc-stats` on the Clamacs host
  frontend before optimizing.
- The m68k walker matches some whole-function patterns (constant return,
  argument passthrough).  Are they worth porting, or does the walker
  output come close enough on AArch64?  Decide from phase 1 disassembly.
