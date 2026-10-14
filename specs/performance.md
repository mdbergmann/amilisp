# AmiLisp: Performance Optimization Specification

## Goal

Maximize throughput and minimize latency of the AmiLisp bytecode VM and runtime, with primary focus on the 68020 @ 14MHz / 8MB RAM target. All optimizations must preserve correctness and CL compliance.

## Constraints

| Constraint | Detail |
|-----------|--------|
| CPU | 68020 — no hardware multiply (microcode, ~70 cycles), no hardware divide, limited branch prediction |
| RAM | 8MB — GC pressure directly impacts usability |
| Compiler | C89/C99, m68k-amigaos-gcc (GCC-based) |
| Correctness | All 656+ host tests and Amiga test suite must continue to pass |
| Code size | Binary must remain practical for floppy distribution (~100KB target) |

---

## Tier 1 — High Impact

### 1.1 CLOS Method Dispatch Cache ✅ DONE (b315f2a, enhanced)

**Problem**: Every generic function call performs a full linear scan of all methods, O(n^2) insertion sort by specificity, and repeated CPL walks. No results are cached. For a GF with 50 methods, this is the dominant bottleneck in ASDF, Quicklisp, and FSet.

**Design**: Multi-level dispatch cache with three modes:

**Phase 1: Effective Method Closure (EMF) caching** — cache the fully-built method combination closure, not just the method list. On cache hit: bind `*current-method-args*` and `(apply emf args)` — zero closure allocation per call. Negative caching for no-applicable-method cases.

**Phase 2: Multi-dispatch cache** — nested `eq` hash tables for GFs specializing on 2+ args. `cacheable-p` returns N (number of specialized positions) instead of boolean. For N=1: identical to single-dispatch. For N>1: navigate N-1 intermediate hash tables keyed by `class-of` each arg.

**Phase 3: EQL specializer cache** — mixed EQL/class cache for GFs with `(eql value)` specializers. Each cache level is a `(eql-ht . class-ht)` cons pair. Per-position EQL value sets determine whether to route through the EQL or class hash table.

**Implementation**:
- Expanded `standard-generic-function` from 5→8 slots (`dispatch-cache`, `cacheable-p`, `eql-value-sets`)
- `%compute-gf-cacheable-p`: returns integer N (class positions), `:eql` (has EQL specializers)
- `%build-effective-method`: builds args-independent closure from sorted method list
- `%make-method-chain` / `%make-around-chain`: use `*current-method-args*` dynamic var instead of captured args
- `%gf-dispatch-cached`: nested hash tables for N specialized positions, stores EMF closures
- `%gf-dispatch-eql`: mixed `(eql-ht . class-ht)` cache with per-position EQL value sets
- `%compute-eql-value-sets`: scans methods, builds per-position hash tables of known EQL values
- Invalidation: per-GF on `defmethod` (clears cache + recomputes cacheable-p + EQL sets), all GFs on `defclass`
- 20 new host tests (118 total CLOS) + 30 new Amiga tests; Fiveam 114/114, FSet 17/17 pass

**Expected gain**: 10-100x for dispatch-heavy code paths; eliminates per-call closure allocation.

**Files**: `lib/clos.lisp`, `tests/test_clos.c`, `tests/amiga/run-tests.lisp`

---

### 1.2 Pre-compiled FASL for Boot Files ✅ DONE (7f51164)

**Problem**: `boot.lisp` (1949 lines) and `clos.lisp` (1388 lines) are parsed from source on every startup. The reader, compiler, and macro expander all run at full cost.

**Current behavior**:
- `load_boot_file()` reads `lib/boot.lisp` as source text
- `cl_eval_string("(require \"clos\")")` reads `lib/clos.lisp` as source text
- FASL infrastructure already exists (`fasl.c`, `compile-file`, `.fasl` loading)

**Design**:
- Add a build step that pre-compiles `boot.lisp` and `clos.lisp` to `.fasl`
- Modify `load_boot_file()` to prefer `lib/boot.fasl` over `lib/boot.lisp`
- Modify CLOS require to prefer `lib/clos.fasl` over `lib/clos.lisp`
- Ship `.fasl` files alongside `.lisp` sources
- Add Makefile target: `make fasl` that builds the host binary and uses it to compile the boot files

**Fallback**: If `.fasl` is missing or load fails, fall back to source loading (current behavior).

**Expected gain**: 2-5x faster startup on Amiga.

**Files**: `src/core/builtins_io.c` (load paths), `Makefile`, `Makefile.cross`

---

### 1.3 `(declaim (optimize speed))` Support — Emit-Time Speed-Gated Optimization ✅ DONE (2026-07-10)

This is the realistic, low-risk home for `(declaim (optimize (speed 3)))` support. It groups
constant folding, dead-branch elimination, and safety-gated check elision into a single
"decisions made during the single emit pass" effort. The companion true peephole post-pass is
1.8 below; **do 1.3 first** — it captures most of the win with near-zero corruption risk.

**Architecture findings (current state, 2026-06-12)**

- `cl_optimize_settings` (`compiler.h:105`, struct `{speed, safety, debug, space}`, default `{1,1,1,1}`)
  **already exists** and is parsed from `declaim`/`proclaim`/`declare`
  (`compiler_extra.c:2049` `cl_process_declaration_specifier`, `:2167` `compile_declaim`;
  `builtins.c:744` `bi_proclaim`). It is **read in exactly one place today** —
  `compile_the()` at `compiler_extra.c:2219` gates `OP_ASSERT_TYPE` on `safety >= 1`.
  Nothing consumes `speed` yet. The plumbing for "what speed are we at" is therefore already present.
- The compiler is **single-pass, emit-as-you-go** into a flat `c->code[]` byte buffer
  (`cl_emit`/`cl_emit_u16`/`cl_emit_i32`, `compiler.c:195+`). There is **no IR and no post-pass**.
- **Builtin inlining already exists** (`inline_builtin_opcode`, `compiler.c:3018`): `+ - * < > <= >=
  = eq car cdr cons null not` map directly to VM opcodes. But **constant folding does not**:
  `(+ 1 2)` still emits `CONST 1; CONST 2; OP_ADD`.
- `CL_Bytecode` (`types.h:216`) does **not** store the optimize settings that were active when it
  was compiled — speed only affects the current global state during compilation.

**Problem**: No compile-time evaluation — `(+ 1 2)` emits `CONST 1, CONST 2, ADD` (3 opcodes, 2
constant pool entries) instead of `CONST 3` (1 opcode, 1 entry). No `speed` consumer exists.

**Design** — all of these are emit-time decisions gated on `cl_optimize_settings`; because they
never reorganize already-emitted bytecode, **no jump relocation is needed** (that is what keeps
them safe — see the relative-jump constraint in 1.8):

1. **Constant folding** of arithmetic/logic calls. In `compile_call()`, after recognizing a known
   pure builtin, if all arguments are compile-time constants (literal fixnums or already-folded
   results), evaluate at compile time and emit a single `OP_CONST`. Supported initially:
   `+`, `-`, `*`, `ash`, `logand`, `logior`, `logxor`, `not`, `1+`, `1-`, and the comparisons.
2. **Dead-branch elimination** for constant tests: `(if <const> a b)` compiles only the live
   branch; `(when nil ...)` / `(and ... nil ...)` collapse. Easy once the test folds to a literal.
3. **Safety-gated check elision** (the actual `(speed 3) (safety 0)` story): at low safety, skip
   emitting `OP_ASSERT_TYPE` (already done for `the`), arg-count checks, and bounds checks.

**Correctness prerequisite — local-declare scoping**: `cl_optimize_settings` is a single global
mutable. A `(locally (declare (optimize (speed 3))) ...)` or per-`defun` declaration must
**save the old settings, apply, compile the body, then restore** — otherwise a local declaration
leaks into sibling top-level forms. This must land alongside (or before) any `speed` consumer.

**Scope**: Only fold fixnum arithmetic and boolean constants initially. Bignum/float/ratio folding
can follow later.

**Expected gain**: 10-15% smaller bytecode, fewer VM dispatch cycles; check elision adds further
gains at `(safety 0)`.

**Files**: `src/core/compiler.c` (`compile_call`, `compile_if`), `src/core/compiler_extra.c`
(local-declare save/restore around `locally`/`defun`/`lambda` bodies).

**Implementation (2026-07-10)**:
- **Scoping**: `cl_optimize_settings` is now the *effective* settings; a new
  `cl_optimize_global` holds the DECLAIM/PROCLAIM baseline
  (`cl_process_declaration_specifier` grew a `proclaimed` flag). Every
  `cl_tail_push` snapshots the effective settings into the tail frame (4 bytes);
  the postludes of declaration-accepting bodies (LET, LOCALLY, MVB, FLET,
  LABELS, MACROLET, SYMBOL-MACROLET) restore them, `compile_body` brackets
  lambda/defun/destructuring-bind bodies, and the NLX compiler-chain unwind
  resets effective = global when no compilation is active.
- **Constant folding** (`try_fold_constant`, gated `speed >= 1` — the default):
  `+ - * 1+ 1- ash logand logior logxor not null = < > <= >=` over fixnum
  constants (incl. nested and quoted); any overflow / non-fixnum operand /
  local-function shadow / `notinline` declines to the normal emit path.
  Allocation-free by construction, so GC-neutral.
- **Dead-branch elimination** (in `compile_if`, gated `speed >= 1`): a foldable
  constant test compiles only the live branch — no JNIL/JMP, dead branch never
  emitted; the live branch keeps tail position.
- **Check elision at `(safety 0)`**: destructuring-bind too-few/too-many
  guards elided (CLHS 1.4.2.3 "should signal" = safe code only); `the` was
  already elided. **Deferred**: arity and array bounds checks are enforced
  inside the VM's OP_CALL/OP_ASET handlers (not emitted per call site), so
  eliding them needs a bytecode/`CL_Bytecode` format change — 1.8-era work.
- **Bonus conformance fix**: `inline_builtin_opcode` now honors
  `(declare (notinline ...))` (CLHS 3.2.2.3), matching the folding path.
- **Measured** (bench-opt, host M3 Ultra): opt.const-fold 39→20 ms,
  opt.dead-branch 49→35 ms, vm.fixnum-loop 72→61 ms, vm.local-shuffle
  63→51 ms. See docs/benchmarks.md 2026-07-10 entry.
- **Tests**: `tests/test_optimize.c` (21 host tests), optimize section in
  `tests/amiga/run-tests.lisp`, gc-stress case in
  `tests/test_gc_stress_regression.sh`.

---

### 1.4 VM Computed Goto Dispatch ✅ DONE

**Problem**: The VM dispatch loop uses `switch(opcode)` with 100+ cases. On 68020, the compiler generates an indirect jump through a bounds-checked table, adding overhead on every opcode.

**Design**:
- Replace `switch(op)` with GCC computed goto: `static void *dispatch_table[256]` with labels-as-values
- Each opcode handler ends with `VM_BREAK` macro that fetches the next opcode and jumps
- Guard with `#ifdef __GNUC__` — fall back to switch for non-GCC compilers (vbcc)
- Disable with `-DCL_NO_COMPUTED_GOTO` if needed
- Four macros abstract both paths: `VM_CASE(op)`, `VM_BREAK`, `VM_DISPATCH()`, `VM_DEFAULT`

**Implementation**:
- 256-entry dispatch table with designated initializers; unassigned slots → unknown opcode handler
- All 71 opcode handlers use `VM_CASE`/`VM_BREAK` macros — single source for both paths
- Both paths tested: computed goto (default) and switch fallback (`-DCL_NO_COMPUTED_GOTO`)
- All 658+ host tests, fiveam 114/114, FSet 17/17 pass on both paths

**Expected gain**: 5-15% overall VM throughput (eliminates branch through single prediction point).

**Files**: `src/core/vm.c`

---

### 1.5 TLV Bypass (Thread-Local Value Fast Path) ✅ DONE

**Problem**: After threading, every `cl_symbol_value()` call probes the TLV hash table (30-50 cycles average) even when no dynamic bindings are active, which is the common case.

**Design**:
- Add `tlv_entry_count` field to `CL_Thread` — tracks number of active TLV entries
- Incremented in `cl_tlv_set()` on new entry creation, decremented in `cl_tlv_remove()`
- In `cl_symbol_value()`, `cl_set_symbol_value()`, `cl_symbol_boundp()`: check `tlv_entry_count == 0` before probing TLV table — skip entirely when no dynamic bindings active
- `cl_tlv_snapshot()` recomputes count from copied table for thread inheritance

**Expected gain**: Eliminates 30-50 cycle TLV probe on every global variable read when no dynamic bindings are active (the common case). Effectively removes the threading tax from `OP_GLOAD`/`OP_GSTORE`.

**Files**: `src/core/thread.h`, `src/core/thread.c`

---

### 1.6 Hash Table Rehashing + Hash Distribution Fix ✅ DONE (3c58761)

**Problem**: Hash tables used a fixed 16-bucket array with no rehashing. Inserting 100K entries created chains of ~6250 average length — O(n) lookup. Additionally, the EQUAL/EQUALP hash function for fixnums returned raw tagged bits (`(uint32_t)obj`), wasting 50% of buckets for sequential integer keys (all fixnums are odd due to tag bit).

**Design**:
- **Hash mixer**: Extract `hash_mix()` helper (xor-shift-multiply avalanche), apply to all fixnum/identity paths including EQUAL/EQUALP (was only used for EQ)
- **Growable buckets**: Add `bucket_vec` field to `CL_Hashtable` — when `CL_NIL`, use inline `buckets[]` flexible array; after rehash, points to a `CL_Vector` holding the new bucket array
- **Automatic rehashing**: `ht_maybe_rehash()` triggers when load factor > 75%. Allocates new `CL_Vector` with doubled bucket count, redistributes entries by relinking existing cons cells (zero allocation during redistribution). GC-safe: protects `ht_obj` during vector allocation
- **`:size` parameter**: `make-hash-table :size N` now computes initial bucket count as `ceil(N / 0.75)` rounded to power-of-2, preventing immediate rehash after filling
- **Updated all access paths**: `ht_get_buckets()` helper for bucket indirection in gethash, setf-gethash, remhash, maphash, clrhash, hash-table-pairs, and `builtins_struct.c` CLOS class table lookup
- **GC marking**: marks `bucket_vec` field; when non-NIL, vector's own marking traverses bucket contents

**Result**: hash 100K benchmark: **4.129s → 0.037s (112x speedup)**. ECL: 0.025s.

**Files**: `src/core/builtins_hashtable.c`, `src/core/types.h`, `src/core/mem.c`, `src/core/builtins_struct.c`

---

### 1.7 Bignum 32-bit Limb Multiplication on 64-bit Hosts ✅ DONE (fbb7e29)

**Problem**: Bignum multiplication used 16-bit limbs with `uint32_t` intermediates (designed for 68020). On 64-bit hosts, this wastes the processor's native word size — `1000!` requires ~534 16-bit limbs, giving ~285K inner-loop iterations for schoolbook O(n²) multiply.

**Design**:
- **`CL_Bignum` heap format unchanged**: 16-bit limbs for Amiga compatibility, FASL serialization, GC
- **Pack/unpack at computation boundary**: `pack_16_to_32()` pairs 16-bit limbs into 32-bit limbs; `unpack_32_to_16()` converts back after computation
- **`bignum_mul_mag32()`**: schoolbook multiplication with `uint32_t` limbs and `uint64_t` intermediates — halves limb count, giving ~4x fewer inner-loop iterations
- **Stack buffers for small operands** (≤256 32-bit limbs), heap-allocated for larger
- **Amiga path unchanged**: `#else` branch keeps 16-bit limbs with `uint32_t` products
- Guarded by `#ifdef PLATFORM_POSIX`

**Result**: factorial 1000 x5000 benchmark: **5.908s → 2.677s (2.2x speedup)**. ECL (GMP): 1.348s. Remaining gap is algorithmic (schoolbook O(n²) vs GMP's Karatsuba O(n^1.585)).

**Files**: `src/core/bignum.c`

---

### 1.8 Bytecode Peephole Post-Pass (gated behind `speed >= 2`; since 4.3: `speed >= 1`) ✅ DONE (2026-07-10)

The higher-ceiling, higher-risk companion to 1.3. Build this **only after 1.3 ships and profiling
shows the emit-time wins are insufficient** — and build it as a proper decode → rewrite → re-encode
pass, never as in-place byte twiddling.

**Shipped** as `src/core/peephole.c`, exactly the decode → rewrite → re-encode shape below, with
these deltas from the plan:

- `OP_STORE` **peeks** (locals[n] = TOS without popping), so the store-reload pattern is
  `STORE n; POP; LOAD n → STORE n` (deletes two instructions), not the planned `DUP; STORE n`.
- The pure-pop whitelist is `CONST/NIL/T/LOAD/DUP/UPVAL` only — `CAR`/arith signal type errors
  that must survive at any speed (ANSI), so they are never deletable.
- `cl_mv_count` discipline: `CONST/NIL/T/UPVAL` write it, `LOAD/DUP/POP` don't. Deleting a
  writing pair (and dropping `OP_NOT` in the branch fusion) requires a forward scan proving the
  write is masked by another writer before any observer (`MV_LOAD/MV_TO_LIST/NTH_VALUE/RET/CALL`);
  when unprovable, the NOT fusion substitutes `OP_MV_RESET` (same length) instead of deleting.
- `OP_TAILCALL` **falls through** for builtin callees (vm.c pushes the result and continues to
  the following RET) — classifying it unconditional deleted live RETs; caught by `make test`.
- NLX landing pads (`CATCH/UWPROT/BLOCK_PUSH/TAGBODY_PUSH` i32 offsets) relocate like jumps and
  are never threaded; the pc-keyed source line map is remapped during re-encode.
- **Single source of truth**: `opcodes.h` is now an X-macro (`CL_OPCODE_LIST`) carrying operand
  shape + dataflow flags per opcode; the enum, the disassembler (whose private table had drifted —
  ~10 newer opcodes missing) and the peephole decoder all derive from it. Unknown/undecodable
  bytes make the pass bail out and leave the bytecode untouched — a misclassified future opcode
  costs a missed optimization, never a miscompile.
- Gate: high-water mark of the effective speed during the compile (`CL_Compiler.peep_speed_max`),
  because body `(declare (optimize ...))` is scope-restored before finalization.
- **Differential harness**: `CLAMIGA_FORCE_SPEED=0..3` pins the effective speed process-wide
  (like `CLAMIGA_GC_STRESS`), so ANY corpus/suite A/Bs the pass unmodified.
  `tests/test_peephole_diff.sh` runs `tests/peephole-corpus.lisp` at 0/2/3 and requires identical
  output; wired into `make test` and (under forced compaction) `make test-gc-stress`.
  `tests/test_peephole.c` covers every pattern, every bail-out, and decoder exhaustiveness over
  the full opcode list.

**Measured** (host, M3 Ultra, `CLAMIGA_FORCE_SPEED=3` vs default speed 1, bench-opt best-of-3):
vm.fixnum-loop 58→53 ms, vm.local-shuffle 49→45 ms, vm.call-return 57→50 ms,
safety1.svref-loop 44→39 ms — ~8–12% on VM-dispatch-heavy loops; see docs/benchmarks.md.

**The dominating constraint — relative jumps**: jumps encode a **relative i32 offset**
(`cl_patch_jump`, `compiler.c:254`: `offset = code_pos - (patch_pos + 4)`; `cl_emit_loop_jump`
similarly). Any pass that changes the *length* of emitted bytecode must relocate **every** jump or
it silently corrupts control flow — exactly the bug class this project fears. This is why naive
in-place peephole over the flat buffer is unsafe.

**Robust shape (decode → rewrite → re-encode)**:
1. Decode `c->code[0..code_pos]` into an instruction list, resolving each jump's target to an
   *instruction index* and marking every instruction that is a jump target (basic-block boundary —
   never fuse a pattern across one).
2. Run peephole rewrites on the instruction list.
3. Re-emit, recomputing all relative offsets from scratch.

**Candidate patterns** (stack-VM classics):
- `OP_STORE n; OP_LOAD n` → `OP_DUP; OP_STORE n` (avoid store-then-reload)
- drop `OP_POP` of an unused pure value
- `OP_NOT; OP_JNIL` → `OP_JTRUE` (fuse)
- jump-to-jump threading
- dead code after `OP_RET` / `OP_JMP` up to the next jump target

**Bonus**: the JIT walker consumes the same bytecode, so a peephole pass improves **both the
interpreter and the JIT** for free.

**Risk / validation**: a mis-relocated jump is a silent miscompile. Requires a dedicated
differential fuzz harness in the spirit of gc-stress — compile representative forms at `speed 0`
and `speed 3` and assert identical results — before it can be trusted on by default.

**Files**: new `src/core/peephole.c` (decode/rewrite/re-encode), called from the bytecode
finalization path in `src/core/compiler.c` when `cl_optimize_settings.speed >= 2` — lowered
to `>= 1` by 4.3, which also added the superinstruction fusion step and three rewrites.

---

### 1.9 MAKE-LOAD-FORM Pre-Pass Hash Index ✅ DONE (2026-07-05)

**Problem** (found by profiling, not planned): `fasl_mlf_seen_p` and `cl_fasl_mlf_lookup`
in `src/core/fasl.c` were linear scans, making the FASL writer's MAKE-LOAD-FORM pre-pass
**O(n²)** in the number of unique heap objects reachable from a file's constants. The
`%make-load-form-active-p` gate that was meant to make the pre-pass zero-cost is
effectively always open in real sessions — cl-ppcre, serapeum, log4cl, local-time, trivia,
cffi, ironclad, and fset all define `make-load-form` methods, so once any of them loads,
every subsequent `compile-file` pays the full walk. Measured at **~85% of all
cold-compile CPU** (14,713 of ~17k leaf samples) during `(asdf:load-system "sento"
:force :all)`.

**Design**: open-addressing hash indices (`CL_Obj` → array position+1, golden-ratio
hash, linear probing) over the existing GC-rooted `seen[]`/`objs[]` arrays, in the style
of the writer's shared-object dedup table. The arrays and their GC mark/update hooks are
unchanged. Compaction hazard: a `%FASL-LOAD-FORM` call can trigger a moving GC, which
rewrites the arrays and invalidates every hashed position — `cl_fasl_gc_update_mlf`
marks the indices dirty and they rebuild lazily before the next probe (rebuilds use
`platform_alloc` only, never the arena, so they cannot themselves trigger GC). On table
OOM the index is disabled and callers fall back to the original linear scan.

**Result**: full sento dependency-tree recompile **18.28s → 1.86s (9.8x)**;
`bench-opt` `compile.file-mlf` **123ms → 23ms** (parity with the gate-closed
`compile.file-plain` at 22ms). No FASL version bump — writer-internal only.
Regression test: `tests/test_make_load_form.sh` case 4 (3000-cons graph with nested MLF
instance); the gc-stress suite's MLF case covers the dirty-rebuild-after-compaction path.

**Post-fix profile note**: with the pre-pass fixed, cold-compile leaf samples are led by
the macro-lookup chain (`cl_macro_p` + `cl_get_macro` + `cl_get_compiler_macro` ≈ 1350
samples vs 874 for the whole VM loop) — the next profiling-driven candidate.

**Files**: `src/core/fasl.c`, `tests/test_make_load_form.sh`, `trunk/bench-opt.lisp`

---

## Benchmark Summary (vs ECL 24.5.10)

Benchmark suite: `bench.lisp` — factorial, fibonacci, Takeuchi, list-ops, hash-table.

| Benchmark | AmiLisp (before) | AmiLisp (after) | ECL | vs ECL |
|-----------|-------------------|------------------|-----|--------|
| factorial 1000 x5000 | 5.908s | **2.677s** | 1.348s | 2.0x slower |
| fib-iter 1000 x5000 | 0.590s | 0.595s | 0.919s | **1.5x faster** |
| tak 18 12 6 x100 | 0.469s | 0.473s | 0.853s | **1.8x faster** |
| list-ops 10000 | 0.032s | 0.034s | 0.062s | **1.8x faster** |
| hash 100000 | 4.129s | **0.037s** | 0.025s | 1.5x slower |

AmiLisp beats ECL on 3/5 benchmarks. The two remaining gaps are in bignum-heavy arithmetic (schoolbook vs Karatsuba) and hash table implementation (cons-chain vs open addressing).

---

## Tier 2 — Medium Impact

### 2.1 VM Debug Instrumentation Gating ✅ DONE (7f51164)

**Problem**: `dbg_last_op`, `dbg_last_ip`, and trace buffer writes execute on every opcode dispatch in production builds.

**Design**:
- Move all `dbg_*` variable updates behind `#ifdef DEBUG_VM`
- Move trace buffer writes behind `#ifdef DEBUG_VM`
- Keep the zero-opcode trap (`__builtin_expect(op == 0x00, 0)`) — it's already branch-predicted away

**Expected gain**: 3-5% (removes unnecessary memory writes per dispatch cycle).

**Effort**: Very low — mechanical `#ifdef` wrapping.

**Files**: `src/core/vm.c`

---

### 2.2 Rotate-XOR Hash Function ✅ DONE (7f51164)

**Problem**: FNV-1a hash uses `hash *= 16777619`, which takes ~70 cycles on 68020 (microcode multiply). Symbol interning and hash table operations are pervasive.

**Current code** (`src/core/symbol.c`):
```c
hash = 2166136261u;
for (...) { hash ^= byte; hash *= 16777619u; }
```

**Design**:
- Replace with rotate-XOR hash:
  ```c
  hash = 0;
  for (...) { hash = ((hash << 5) | (hash >> 27)) ^ byte; }
  ```
- Rotate + XOR uses shift and OR — 2-3 cycles on 68020 vs ~70 for multiply
- Verify hash quality: run collision test on CL symbol corpus (2000+ symbols)
- Apply to all hash functions: symbol hash, string hash, `sxhash`, hash table key hash

**Expected gain**: 2-5x faster hashing. Affects every symbol intern, hash table lookup, and `sxhash` call.

**Risk**: Hash distribution change could affect hash table load balance. Validate with collision statistics.

**Files**: `src/core/symbol.c`, `src/core/builtins_hashtable.c`

---

### 2.3 Hash Table Power-of-2 Buckets ✅ DONE (7f51164)

**Problem**: Bucket index computed as `hash % bucket_count` — division is extremely slow on 68020 (~140 cycles for 32-bit divide).

**Design**:
- Ensure all hash table bucket counts are powers of 2
- Replace `hash % bucket_count` with `hash & (bucket_count - 1)`
- Adjust growth strategy: double bucket count on resize (already natural for power-of-2)
- Store `bucket_mask = bucket_count - 1` in hash table header to avoid recomputing
- Also store hash value alongside each key in chain entries — compare hash before calling equality test (avoids expensive EQUAL/EQUALP on hash mismatch)

**Expected gain**: 5-10% on hash-table-heavy code (CLOS slot lookup, symbol tables, ASDF).

**Files**: `src/core/builtins_hashtable.c`, possibly `src/core/mem.c` (hash table struct layout)

---

### 2.4 Set Operations as C Builtins — DEPRIORITIZED (measured 2026-07-05)

**Reality check**: instrumenting `intersection`/`union`/`set-difference`/`subsetp`/`adjoin`
with call counters during a full `(ql:quickload "sento")` measured **zero** calls to
`intersection`/`union`/`subsetp` and 111 trivial calls total (`adjoin` 91,
`set-difference` 20, all lists ≤ 40 elements — microseconds of work). The "hot path in
ASDF" premise below is false for real workloads; the bundled runtime has only 3
definition-time call sites. Kept as backlog for algorithmic completeness; the
`set.*` benchmarks in `trunk/bench-opt.lisp` stand ready if this is ever picked up.

**Problem**: `union`, `intersection`, `set-difference`, `subsetp` are pure Lisp with O(n*m) nested `dolist` + `member`.

**Current code** (`lib/boot.lisp`):
```lisp
(defun intersection (list1 list2 &key key test test-not)
  (let ((result nil))
    (dolist (x list1 (nreverse result))
      (when (%set-member x list2 key test test-not)
        (push x result)))))
```

**Design**:
- Implement `intersection`, `union`, `set-difference`, `set-exclusive-or`, `subsetp` as C builtins
- For small lists (< 32 elements): use current O(n*m) algorithm (cache-friendly, no allocation)
- For large lists: build temporary hash set from shorter list, probe with longer list — O(n+m)
- Support `:key`, `:test`, `:test-not` keyword arguments
- Remove Lisp implementations from `boot.lisp` (or keep as fallback behind feature flag)

**Expected gain**: 5-20x for large sets; modest gain for small sets due to eliminated interpreter overhead.

**Files**: new `src/core/builtins_set.c` or add to `src/core/builtins_sequence.c`, `lib/boot.lisp`

---

### 2.5 Free-List Size Class Segregation

**Problem**: Free-list is a single linked list scanned linearly. After heap fragmentation, allocation degrades to O(n) where n = number of free blocks.

**Design**:
- Segregate free list into size classes: 8, 16, 32, 64, 128, 256, 512, 1024, 2048+ bytes
- On free: insert block into appropriate size class list
- On alloc: check exact size class first, then next larger class (split if needed)
- On GC sweep: coalesce adjacent free blocks, then re-segregate
- Data structure: array of 9 list heads (one per size class) — 36 bytes overhead

**Expected gain**: O(1) allocation for common sizes (cons cells = 8 bytes, small vectors = 16-32 bytes). Eliminates pathological O(n) scans.

**Files**: `src/core/mem.c`

---

### 2.6 Build Flags: `-O3` and `-flto` ✅ DONE (7f51164)

**Problem**: Cross-compile uses `-O2`. LTO would allow GCC to inline across translation units — critical for small helper functions called from the VM loop (`cl_vm_push`, `cl_vm_pop`, type checks).

**Design**:
- Change `Makefile.cross` CFLAGS from `-O2` to `-O3`
- Add `-flto` to both CFLAGS and LDFLAGS
- Verify binary size stays reasonable (< 150KB)
- Verify all tests still pass (aggressive optimization can expose UB)
- If binary size is a concern, use `-Os -flto` as alternative

**Expected gain**: 5-10% from cross-TU inlining and loop optimizations.

**Risk**: `-O3` may expose latent undefined behavior. Run full test suite. `-flto` may not be supported by all m68k-amigaos-gcc versions — verify toolchain support.

**Files**: `Makefile.cross`

---

## Tier 3 — Lower Impact / Quick Wins

### 3.1 CLOS Slot Access Optimization — ✅ DONE (2026-07-05, both halves)

**Problem**: `slot-value` performs a hash table lookup on every access. Defstruct already compiles direct `%struct-ref` with known offset — but `slot-value` on a *struct* instance was far worse than the spec assumed: the 2026-07-05 sento runtime profile showed it resolving the slot on EVERY access via a linear walk of the prepended struct-registry alist (`find_struct_entry` — early-defined types pay a full O(types) walk) plus a freshly consed slot-name list (`cl_struct_slot_names`) matched linearly in Lisp. Slot-access machinery was the largest attackable CPU cluster after the VM loop itself (~1,900 leaf samples: `get_slot_specs` 1,028, `bi_gethash` 512, plus `cl_struct_slot_names` / `bi_class_of` / `bi_struct_ref`).

**Done (first half — C side, registry + struct slot resolution, 2026-07-05)**:
- Struct registry hash index in `builtins_struct.c`: open-addressing table name symbol → registry entry (FASL MLF-index style — dirty-marked on registration and on compaction, rebuilt lazily with `platform_alloc` only, linear-walk fallback on OOM/malformed cell). Makes `find_struct_entry` / `cl_is_struct_type` / `typep` on struct types O(1).
- New zero-allocation `%STRUCT-SLOT-INDEX` builtin; `%find-struct-slot-index` (clos.lisp) now resolves struct slots through it — no consing on `SLOT-VALUE` / `(SETF SLOT-VALUE)` / `SLOT-BOUNDP`.
- Measured (bench-opt `struct.*`, type buried behind 256 registrations): `struct.slot-value` 143 ms / 6.4 MB consed → **53 ms / 0 bytes**; `struct.typep` 50 → **12 ms**; compile.file-* 27 → 20 ms. Struct slot-value is now at parity with the CLOS-instance hash path (clos.slot-value 55 ms).

**Done (second half — fused fast path + C inline-cache probe, 2026-07-05)**:
- New fused builtins `%STRUCT-SLOT-VALUE` / `%STRUCT-SLOT-STORE` (builtins_struct.c): type-name → registry entry (O(1) hash probe) → slot index (EQ scan of the specs) → direct slot read/write, in ONE non-erroring, non-allocating builtin call. `SLOT-VALUE`, `(SETF SLOT-VALUE)` and `SLOT-BOUNDP` front the full protocol with it; DEFCLASS-generated `:accessor`/`:reader`/`:writer` methods inline it directly. Any miss (not a struct, `:CLASS` slot, unbound slot, extended slot-access protocol, no such slot) falls back to the unchanged full protocol. The design deliberately re-resolves through the registry on EVERY access instead of capturing the index in a closure — correct across class redefinition and for subclass instances whose inherited slots sit at different indices, with the O(1) hash probe making the re-resolution nearly free.
- New `%GF-IC-EMF` builtin: the arity-specialized 1/2-arg GF discriminators' inline-cache hit path — read GF slot 8, compute receiver class(es) (`class_of_type_name` + `*CLASS-TABLE*` eq-lookup, both in C), compare, return EMF — fused into one builtin call. Misses go through the unchanged Lisp slow path, which still populates the cache. This one speeds up ALL 1/2-arg GF dispatch, not just accessors.
- Measured (bench-opt, host): `clos.slot-value` 55 → **18 ms**; `clos.accessor` 92 → **28 ms**; `struct.slot-value` 53 → **18 ms**; `clos.make-instance` 91 → **72 ms**. Remaining accessor gap vs `struct.accessor` (28 vs 6 ms) is generic dispatch itself (funcallable-instance unwrap + discriminator + method-function calls).

**Files**: `src/core/builtins_struct.c`, `lib/clos.lisp`

**Follow-up — ✅ DONE (2026-07-10): SLOT-VALUE compile-time inline + pair index.**
After the reader-GF fast dispatch work, `SLOT-VALUE` was still ~4× slower than
a promoted reader — inverted from what users expect.  The remaining cost was
the full Lisp call frame of the `SLOT-VALUE`/`%SET-SLOT-VALUE` DEFUNs plus the
linear specs-list walk in `struct_slot_resolve`.  Fixed without a new opcode
or FASL format change: compiler macros on both DEFUNs splice the fast-path
test into every compiled call site (`lib/clos.lisp`); `compile_setf` routes
defsetf updaters through `compile_call` so setter compiler macros can fire at
all (`compiler.c`); and a `(type-name, slot-name) → index` pair table built
alongside the registry hash index makes resolution O(1) at any slot position
(`builtins_struct.c`).  Measured (host, 8-unrolled, best-of-3): read 77.5 →
**56 ns**, write 72.5 → **51 ns**, deep slot 11-of-12 68 → **56 ns** (flat).
Tracked by `clos.slot-value` / `clos.slot-value-deep` / `struct.slot-value`
in bench-opt.  See docs/benchmarks.md 2026-07-10.

---

### 3.2 Keyword Argument Pre-computation

**Problem**: Keyword argument matching in `OP_CALL` is O(n_keys * n_supplied) — nested loop scanning supplied args for each defined keyword.

**Design**:
- At function definition time, build a small hash map or sorted array of `(keyword-symbol → local-slot-index)` pairs
- At call site, iterate supplied keyword args once, looking up each in the map — O(n_supplied) total
- For functions with <= 4 keyword args, use linear scan (faster than hash for small n)
- Store the keyword map in the bytecode function object

**Expected gain**: Noticeable for functions with many keyword args (e.g., `make-instance`, `format`).

**Files**: `src/core/vm.c`, `src/core/compiler.c`

---

### 3.3 `cl_mv_count` Write Reduction

**Problem**: Many opcodes unconditionally set `cl_mv_count = 1` even when it is already 1. This is a memory write on every arithmetic op, load, store, etc.

**Design**:
- Only set `cl_mv_count = 1` in opcodes that may follow a multiple-values-producing opcode (e.g., after `OP_CALL`, `OP_VALUES`)
- Alternatively, set `cl_mv_count = 1` once at the top of the dispatch loop and only change it in `OP_VALUES` / `OP_CALL` that produce multiple values
- Audit all opcodes to determine which ones actually need to reset mv_count

**Expected gain**: Small (1-2%) — eliminates unnecessary store on hot path.

**Files**: `src/core/vm.c`

---

## Tier 4 — 2× vs ECL (profiled 2026-09-08)

Goal: **at least 2× on real workloads on the host**, with every change landing in
the shared runtime (`vm.c`, the builtins, `clos.lisp`) so m68k and PPC gain the
same way.  A host-only native backend is explicitly out of scope for this tier.
Reference workload: the sento actor pipeline (`trunk/profile-sento-bench.lisp`,
pinned dispatcher, `tell`), where ECL 26.5.5 is ~2.9× faster.

### 4.0 Where the time goes (baseline measurements)

Host, Apple M3 Ultra, master at `1e76a19d`.  The pinned/tell cell is bound by the
single actor thread (the senders sit in backpressure sleep), so the per-message
figure is that thread's cost.  Full numbers in docs/benchmarks.md 2026-09-08.

| | msg/s | µs per message |
| --- | ---: | ---: |
| amilisp 0.9, 4 load threads | 175k | 5.7 |
| ECL 26.5.5 (benchmarks.md 2026-07-15, 8 load threads) | 512k | 2.0 |

**Compile speed is not part of the gap**: `(asdf:load-system "sento" :force t)`
recompiles in 0.39 s on amilisp and 15.06 s on ECL (gcc per file).

**Sampled profile** (macOS `sample`, on-CPU samples only): about **70% self time
in `cl_vm_run`** — opcode dispatch plus the OP_CALL/OP_RET protocol, which is
inlined into the loop.  The remaining ~30% is a handful of runtime taxes, each
attributable to one call site:

| tax | share | what it does per operation |
| --- | ---: | --- |
| `struct_slot_resolve` + `cl_tables_rdlock` | ~6% | every SLOT-VALUE takes the tables rwlock and probes two hash indexes |
| `call_builtin` overhead | ~7% | `cl_check_c_stack` (→ `platform_stack_headroom`), `last_builtin_name = cl_symbol_name(...)` into process-wide statics, two TLS lookups, the pre-call MV copy loop |
| `bi_gethash` from Lisp | ~6% | dispatch caches / class tables probed from Lisp |
| `typep_symbol` | ~2% | a cascade of ~45 `strcmp` calls before a struct/class name is reached; sento's `(declare (type ...))` emits ~11 OP_ASSERT_TYPE per message |
| `cl_symbol_value` | ~2% | TLS lookup for the thread instead of the `thr` the VM already holds |
| `bi_gf_ic_emf` | ~1% | `class_of_type_name` **interns** "FIXNUM"/"SYMBOL"/... per dispatch on a non-instance argument, then takes the rwlock |
| `cl_cons` | ~1% | `CL_GC_PROTECT` of both args on every cons, `memset` for an 8-byte object |

**Opcode counts** (`DEBUG_FLAGS=-DPROFILE_OPCODES` build, 501k messages):
~1450 bytecodes and **139 calls per message** (both sides).  LOAD/POP/STORE/
CONST/NIL are 50% of all dispatches (STORE is always followed by POP: 21% of
dispatches as a pair), FLOAD+CALL+TAILCALL+RET 21%, GLOAD 5% (72 per message —
`*slot-access-protocol-extended-p*` and `*slot-unbound-marker*` are each read
twice per slot access by the SLOT-VALUE compiler macro), MV_RESET 3%.

**Per-primitive cost** (`trunk/bench-prims.lisp`, ns net of the loop baseline —
30 ns amilisp / 20 ns ECL — so an absolute 1-arg call is 48 vs 13 ns):

| primitive | amilisp | ECL | notes |
| --- | ---: | ---: | --- |
| call, 1 arg | 18 | 0 | OP_CALL: safepoint via TLS, 2 funcallable probes, arity check, arg-shift loop, NIL fill, 8-field frame, 4 validations; OP_RET: 2 validations |
| call, &key 2 of 3 | 44 | 3 | keyword parse in OP_CALL |
| multiple-value-bind | 50 | 0 | STORE/POP/MV_LOAD plumbing |
| flet call | 54 | 0 | allocates a closure per entry |
| generic call, 1 method | 68 | 24 | OP_CALL → discriminator frame → `%GF-IC-EMF` → EMF → method |
| generic call with :around / call-next-method | 525 | 79 | `&rest` chain closures, 3 dynamic bindings, APPLY per level |
| accessor read / write | 4 / 5 | 0 | reader/writer IC in OP_CALL — the target shape |
| slot-value read | 45 | 1 | 2 GLOADs + rwlock + 2 hash probes |
| with-slots incf | 134 | 22 | |
| make-struct, 2 keywords | 72 | 34 | |
| list of 4 | 52 | 25 | |
| unwind-protect | 93 | 2 | setjmp, 15-field NLX frame, `cl_compiler_mark`, `cl_printer_state_save`, saved-pending `strncpy`, MV_TO_LIST + VALUES-LIST |
| handler-case | 104 | 52 | cons + closure + CATCH + BLOCK_PUSH + HANDLER_PUSH |
| catch/throw | 37 | 1 | |
| with-lock-held, uncontended | 117 | 44 | = unwind-protect + acquire + release |
| typep on a class | 88 | 51 | |

`CLAMIGA_FORCE_SPEED=3` changes **no row** — the emit-time and peephole work of
1.3/1.8 is exhausted; the remaining cost is in the runtime C and the protocol.

### 4.1 Phase 1 — remove the taxes ✅ DONE (2026-09-08)

Each item is pure C, no format change, measurable in isolation on its
bench-prims row and as a `sample` share.  **Result: +37.6% on the sento
pinned/tell acceptance cell** (155,204 → 213,543 msg/s, both measured in the
same session; full table in docs/benchmarks.md 2026-09-08).  Behaviour is
pinned by `tests/test_tier4_phase1.sh` (34 checks, also run under
`make test-gc-stress`) and by the Tier-4 section of
`tests/amiga/run-tests.lisp`.

1. **`call_builtin` slimming** ✅ — no `cl_check_c_stack` per builtin call; it
   moved to the head of `cl_vm_apply`, which is where the C stack actually
   grows (a Lisp-to-Lisp call stays inside one `cl_vm_run` activation, so
   every real C recursion point — `cl_vm_apply`, `cl_vm_apply_list`,
   `cl_vm_eval`, `cl_vm_run`, and `cl_check_recursion_guards` in the reader
   and compiler — is still guarded; verified to produce the identical clean
   error on the same three overflow shapes as before).  The
   `last_builtin_name/fptr/obj` process-wide statics became ONE per-thread
   `CL_Obj` store (`CL_Thread.last_builtin`); `main.c`'s fatal handler
   derives the name and code pointer from it, which is also strictly safer
   than the old `cl_symbol_name` char pointer that dangled after compaction.
   `thr` is a parameter now instead of two TLS lookups, and the
   `pre_call_mv_values` copy is one store in the `mv_count == 1` case.
2. **SLOT-VALUE resolution cache** ✅ — implemented as a per-thread
   `(type, slot) -> index` cache in front of the shared pair index
   (`CL_Thread.slot_ic`, `struct_slot_resolve`) rather than as per-call-site
   cells in the compiler macros.  A repeat access costs three compares and
   **no tables rdlock and no hash probe**, which was the measured cost; it
   needs no `load-time-value` plumbing, no FASL change and no `make fasl`
   regeneration, and it covers `SLOT-BOUNDP` and the accessor fallbacks that
   a compiler-macro cell would have missed.  Per-thread by construction, so
   there is no multi-word publication to tear.  Invalidation is the global
   `cl_struct_layout_gen`, bumped by both events that set
   `struct_index.dirty`: a type (re)registration, and a collection that moves
   the symbols the keys are made of.  **Not done**: folding the
   `*slot-access-protocol-extended-p*` latch and the unbound-marker compare
   into the builtin — that part still costs two GLOADs per access and is
   carried into Phase 2, where the codegen is being touched anyway.
3. **GF inline cache without intern or lock** ✅ — `class_of_type_name` reads
   pre-interned, GC-registered class-name symbols instead of calling
   `cl_intern_in` per dispatch.  `%GF-IC-EMF` drops the `*CLASS-TABLE*`
   lookup entirely by validating in the other direction: it compares the
   receiver's class name against the *name of the class already in the
   cache* (slot 0 of a class metaobject), so there is no table and no
   `cl_tables_rdlock` — and no generation counter was needed, because every
   event that can stale a cached class already clears the GF's slot 8.
4. **`typep_symbol` by symbol identity** ✅ — each standard type name's
   COMMON-LISP symbol carries its type code in the top byte of its own
   `flags` word (`CL_SYM_TYPECODE`), so TYPEP is one load and a switch.
   Struct / condition / class specifiers, which used to run the whole
   cascade before reaching their first real check, now skip it entirely.  A
   symbol that merely shares a standard type name still resolves, by name,
   on the cold path.  Worth more on 68k, where strcmp is dearer.
5. **`cl_symbol_value_on(thr, sym)`** ✅ plus an inline OP_GLOAD fast path
   when `tlv_entry_count == 0`.
6. **VM-rooted cons** ✅ — `cl_cons_rooted` takes its operands by ADDRESS and
   reads them after the allocation, so OP_CONS, OP_LIST and the three &rest
   builders cons from GC-rooted VM-stack / extra-arg slots with no root-stack
   push per operand.  The zeroing is inlined at exactly `CL_MIN_ALLOC_SIZE`;
   measurement showed a tuned `memset` beating a scalar word loop from 32
   bytes up, so a wider cutoff is a pessimization.

**Layout lesson (cost a full measurement cycle):** the first version put the
1KB `slot_ic` table in the middle of `CL_Thread` and lost ~3ns on *every*
call row of bench-prims — the VM's hot fields had moved onto different cache
lines.  New per-thread tables go at the END of the struct.  This is the same
sensitivity CLAUDE.md records for `vm.o` and LTO.

### 4.2 Phase 2 — the call and unwind protocol ✅ DONE (2026-09-08)

Items 1, 2, 3, 4, 6 and 7 landed in `e60e71db`; item 5, the handler-case
JIT template and the debug-only gating of the `OP_CALL_GLOBAL`
constant-pool check (`#ifdef DEBUG_VM`, like the other structural checks
of item 1 — the review had kept it unconditional) in the follow-up
commit.  Behaviour is
pinned by `tests/test_tier4_phase2.sh` (141 checks, also run under `make
test-gc-stress`) and the "Tier-4 phase 2" block of
`tests/amiga/run-tests.lisp`.  Full numbers in docs/benchmarks.md
2026-09-08 (phase 2).  `CL_FASL_VERSION` is 32: six new opcodes.

**Per-primitive result** (absolute ns, min of 5, same session): call-1arg
51 → 43, call-3arg 58 → 51, &key call 79 → 71, unwind-protect 123 → 81,
handler-case 118 → 55, with-lock-held 134 → 89, lock+condvar 151 → 102,
make-struct 98 → 55, gf-around+primary 539 → 485, call-next-method 541 →
490, gf 1-arg 91 → 78 (full table in docs/benchmarks.md).  No row
regressed.  The targets in the original
plan (1-arg call ~28, unwind-protect ~15) were not reached: what remains
in a call is the frame push itself, the arity/NIL-fill work and the
`OP_RET` restore, and an unwind-protect still pushes a 26-field frame and
a saved-pending record.

**Acceptance cell**: sento pinned/tell **207,293 → 208,088 msg/s (+0.4%)**
— unchanged — while the CPU time the run burns dropped 10% (206.6 → 186.3
CPU-seconds per 122 s of wall time).  The cell is no longer bound by the
actor thread's VM work; see "What the sento cell is bound by" below before
planning phase 3 against it.

1. **OP_CALL / OP_RET slimming** ✅ — the six structural validations are
   `#ifdef DEBUG_VM`; the safepoint reads `thr`; one header-type read
   classifies the callee (was three `cl_funcallable_instance_p` calls per
   call); `cl_trace_count` and every other per-thread field the loop
   touches is read through `thr` (each was a `cl_get_current_thread` — a
   TLS lookup once a second thread exists); the arguments stay where they
   were pushed and the new `CL_Frame.fslot` tells `OP_RET` whether a
   function slot sits under `bp` (the per-call arg-shift loop is gone);
   `FLOAD sym; CALL n` is one `OP_CALL_GLOBAL` (and `OP_TAILCALL_GLOBAL`),
   which resolves the symbol after the arguments and pushes nothing.  The
   m68k JIT walker has templates for both (self-recursive TCO kept).
2. **NLX frames without setjmp** ✅ — one `jmp_buf` per `cl_vm_run`
   activation, armed lazily by the first NLX push; a frame carries a
   pointer to it (`CL_NLXFrame.landing`; NULL for the C- and JIT-owned
   frames, which keep their own `buf`), and one landing block restores
   from the frame at `cl_nlx_top`.  A transfer that originates in the
   same activation (`RETURN-FROM`, `GO`, a re-throw) is a `goto` — no
   longjmp at all.  The marks are read off `thr` (no `cl_compiler_mark` /
   `cl_printer_state_save` calls), and the 512-byte error-message copy in
   the saved-pending record happens only when an error is actually
   pending.  `cl_nlx_jump()` is now the only way to transfer to a frame.
3. **handler-case codegen** ✅ — `HANDLER-CASE` expands to the special
   form `CLAMIGA::%HANDLER-CASE` (`OP_HANDLER_CASE_PUSH` / `_POP`): one
   NLX frame plus one handler binding per clause whose "handler" is the
   clause index as a fixnum; `cl_signal_condition` transfers to the frame
   instead of calling a function (through interposed cleanups as pending
   kind 3), and the landing is a table of per-clause `OP_JMP`s.  A
   `:no-error` clause is rewritten first, exactly as CLHS 9.2.23 shows.
   The m68k JIT walker has a template for both opcodes (follow-up
   commit): the BLOCK_PUSH inline-setjmp shape, whose longjmp arm pushes
   the condition and dispatches on the matched clause index to that
   clause's table entry (`cl_jit_runtime_handler_case_*`); the
   `walker-hc-*` checks of `tests/amiga/test-jit.lisp` pin that the
   functions attach native code and land in the right clause.
4. **unwind-protect value passing** ✅ — `OP_MV_SAVE` parks the protected
   form's values on a per-thread save stack (`CL_Thread.mv_save_buf`,
   fixnum-tagged count on top, GC-marked as a whole) and `OP_MV_RESTORE`
   pops them after the cleanup: no list, no `VALUES-LIST` call, no local
   slot.  Every NLX landing and error frame drops the records of
   abandoned cleanups.  The JIT has templates for both.
5. **Non-escaping `flet`/`labels`** ✅ (2026-09-08, follow-up commit) —
   inline expansion at each call site.  A local function that is never
   `#'`-referenced, never declared `notinline`, never called from inside
   a closure (a `lambda`, a sibling's or nested local function's body, a
   `handler-bind` handler, a `restart-case` clause — the body is only
   inlined into the defining function's own code) and, for `labels`,
   never referenced from any member's body (recursion, direct or mutual)
   keeps no closure at all: the call compiles to `args; STORE params;
   (locally . body)` — wrapped in `(block name ...)` only when the body
   returns from it — with every name the call site bound since the
   definition (variables, local functions, macrolet, symbol-macrolet,
   blocks, go tags) hidden from the body through skip bands in the
   lookup tables (`CL_CompEnv.hide_*`, `CL_Compiler.hide_block/tagbody_*`),
   so free names resolve in the definition environment (CLHS 3.1.1).
   The escape analysis is one macro-aware `nlx_scan` walk per form
   (`NLX_FUNUSE` mode; it also sees through symbol-macros and, since
   2026-09-29, `macrolet` macros — the walk installs a `macrolet`'s
   expanders for the scan of its body, as the boxing scan does).  Shape
   limits: required parameters only (≤ 8), no special parameter or
   `(special ...)` declaration, no `load-time-value` anywhere in the body
   (the walk finds it through macros; an inlined body is compiled — and
   the form evaluated — once per call site, which broke serapeum's
   `static-let`, 2026-09-09), and a body over 48 conses is inlined only
   when called once.  `(optimize (space > speed))` or `(debug 3)` keeps
   the closures (and the local's backtrace frame — an inlined call has
   none, the error line still points into the body);
   `CLAMIGA_NO_LOCAL_INLINE=1` does so process-wide.  A macro that
   expands differently between the analysis and the compile is reported
   as a clear compile error, never compiled as a call through the empty
   slot.  What it saves per `flet` entry: the closure allocation; per
   call: the frame push, arity check and `OP_RET` (bench-prims
   `flet-call` 73 → 49 ns absolute, i.e. the body alone; every other row
   within ±2 ns or inside its run-to-run spread — docs/benchmarks.md
   2026-09-08 item 5).  Not covered: a
   `labels` helper called from a recursive sibling (that call is from
   inside the sibling's closure — a cross-compiler expansion, which would
   need the hiding bands to span the env chain and the outer block/tag
   tables; the closure path stays).  Tests: `tests/test_local_inline.sh`
   (72 checks, DISASSEMBLE-pinned, also under gc-stress), the "item 5"
   block of `tests/amiga/run-tests.lisp`.
6. **Keyword constructors** ✅ — every `defstruct` keyword constructor
   carries a compiler macro (`%struct-keyword-ctor-expand`) that turns a
   literal-keyword call into a positional `%make-struct`, binding the
   argument forms to temporaries in call order and taking the unsupplied
   slots' constant init-forms.  It declines (leaving the keyword call) on a
   non-keyword or unknown key, a duplicate, `:allow-other-keys`, an odd
   count, or an unsupplied slot whose init-form is not a constant.
7. **:around / call-next-method chains** ✅ — one special, `*CNM*`, bound
   to `(next . args)` per level, where `next` is the per-EMF LIST of the
   method functions still to run (built once, never mutated; terminated
   by T when the last one never uses CALL-NEXT-METHOD, so it is called
   bare); `CALL-NEXT-METHOD` pops it, `NEXT-METHOD-P` asks whether it is a
   cons.  A level costs one cons, one binding and inline CAR/CDR instead
   of three bindings, a closure and an `&rest` list.  A first version
   used a simple-vector plus an index and was *slower* than the closures
   it replaced: the LENGTH and SVREF builtin calls per level cost more
   than the two bindings and the closure they saved (measured in
   bench-prims: 539 → 592 vs the list's 539 → ~430).

**A compiler bug this exposed and fixed**: a *local* `RETURN-FROM` or `GO`
from inside the argument list of a call — `(block b (list 1 (return-from
b 2)))` — left the call's already-pushed arguments on the operand stack
(the old `handler-case` expansion never hit it because its `catch` forced
the NLX path).  `nlx_scan` now routes such exits through the NLX frame,
whose landing restores the stack pointer.

**What the sento cell is bound by** (resolved in the follow-up commit,
`trunk/sento-bench-loadthreads.lisp`, numbers in docs/benchmarks.md
2026-09-08 item 5): the consumer.  Over a producer-count sweep the rate is
highest with ONE producer (287k msg/s average, 271k median), drops to
~190k with 2–4 and recovers to 267k with 8, with the collector at 1–1.4%
of wall time throughout.  At one producer the actor thread spends ~3.5 µs
per message — the ~1,450 bytecodes of the message path at ~2.4 ns each —
so the path is VM-bound and phase 3's superinstructions will move it.
The 2–4 producer dip is the message-box hand-off (producers alternately
overfilling past `:wait-if-queue-larger-than` and draining it, so the
consumer sleeps and wakes per burst), which is why the 4-producer cell
used as this phase's gate stayed flat while CPU time per run fell 10%.
**Gate phase 3 on `:load-threads 1`** (median of record: 271k).

### 4.3 Phase 3 — superinstructions ✅ DONE (2026-09-09)

The peephole pass (1.8) already decodes and re-encodes bytecode, so fused
opcodes are emitted there without touching the compiler.  Two decisions
shaped the phase:

- **The pass runs at every speed above 0 now** (`cl_peephole_optimize`
  gated on `peep_speed_max >= 1`, was `>= 2`).  Third-party code — sento
  included — compiles at the default `(speed 1)`, so a `speed >= 2` gate
  would have left the acceptance workload untouched.  `(speed 0)` and
  `CLAMIGA_FORCE_SPEED=0` keep the bytecode as emitted, which is what the
  differential harness diffs against (now at 0 vs 1, 2 and 3);
  `CLAMIGA_NO_FUSE=1` keeps the rewrites but not the fusion.
- **The table came from a pair profile, not from the single-opcode counts
  of 4.0.**  `PROFILE_OPCODES` builds now count adjacent pairs
  (`cl_op_pair_counts`, dumped by `%op-counts-dump`); the sento message
  path at one producer gave STORE→POP 10.8%, POP→LOAD 8.4%, LOAD→LOAD
  5.2%, LOAD→CALL_GLOBAL 4.8%, LOAD→STRUCT_REF 2.7%, LOAD→RET 2.8%,
  LOAD→MV_RESET 2.6%, EQ→JNIL 1.9%, GLOAD→JNIL 1.6% — and `CONST; EQ; JNIL`
  from the plan barely registers (`GLOAD; EQ; JNIL`, the slot-protocol
  marker test, is what is there).  A second profile of the fused stream
  chose the five round-two shapes.

**Shipped** (`peephole.c` `peep_fuse`, `opcodes.h` 0xB2–0xBF, `vm.c`,
`jit.c`, `builtins_io.c`; `CL_FASL_VERSION` 33):

| fused opcode | members | operands |
| --- | --- | --- |
| `STORE_POP n` | STORE n; POP | u8 |
| `LOAD_LOAD a b` | LOAD a; LOAD b | u8 u8 |
| `LOAD_CONST s k` | LOAD s; CONST k | u8 u16 |
| `LOAD_CALL_GLOBAL s f n` | LOAD s; CALL_GLOBAL f n | u8 u16 u8 |
| `GLOAD_CALL_GLOBAL v f n` | GLOAD v; CALL_GLOBAL f n | u16 u16 u8 |
| `LOAD_STRUCT_REF s i` | LOAD s; STRUCT_REF i | u8 u8 |
| `LOAD_STORE_POP a b` | LOAD a; STORE b; POP | u8 u8 |
| `POP_LOAD s` | POP; LOAD s | u8 |
| `LOAD_MV_RESET s` | LOAD s; MV_RESET | u8 |
| `LOAD_RET s` | LOAD s; RET | u8 |
| `LOAD_JNIL s t` | LOAD s; JNIL t | u8 i32 |
| `EQ_JNIL t` | EQ; JNIL t | i32 |
| `GLOAD_JNIL v t` | GLOAD v; JNIL t | u16 i32 |
| `GLOAD_EQ_JNIL v t` | GLOAD v; EQ; JNIL t | u16 i32 |

A fused opcode's operands are its members' operands concatenated, so the
pass builds it by chaining the members (`fused_with`) and the encoder
copies their bytes in order; `cl_opnd_len` / `cl_opnd_jrel_pos` are the two
shape facts every decoder (peephole, disassembler, JIT prescan, the
exhaustiveness test) reads.  Fusion is one greedy left-to-right pass after
the deleting rewrites reach their fixpoint, triples before pairs, never
across a branch target (a jump landing on the second member would run
only the tail), and a fused head is never re-fused.  Each `vm.c` handler
is exactly the pair; the ones with a heavy tail (`CALL_GLOBAL`,
`STRUCT_REF`, `RET`) push the local and fall into the tail's handler, so
the tail exists once.  The m68k walker has a template for every one (the
heavy-tail ones reuse the tail's template the same way) and the prescan
knows their shapes, so JIT coverage is unchanged.  The line map attributes
an absorbed member to its head, and the error paths of the fused opcodes
(and of `STRUCT_REF`/`STRUCT_SET`/`GLOAD`/`CALL_GLOBAL` themselves) now
sync the frame's ip first, so a backtrace names the form's line rather
than the previous call's.

**Three rewrites came out of looking at the fused listings**, each a
generic win independent of fusion:

- *Dead store before RET*: every function ended in `STORE slot; RET` —
  the DEFUN block's result slot, stored whether or not a RETURN-FROM
  exists — one dispatch per call, gone (`peep_dead_store_ret`, also
  through a `MV_RESET`).
- *Jump to return*: a `JMP` whose live target is `RET` becomes the `RET`
  (`peep_thread_jumps`), so a block arm returns where it ends.
- *Return site*: a local RETURN-FROM compiled to `STORE slot; POP; JMP L`
  with `L: LOAD slot; [MV_RESET;] RET`; the site now returns in place
  (`peep_return_site`), and once no site targets `L`, the fall-through
  `STORE; POP; LOAD; RET` collapses through the two rules above.

Because a function now returns wherever an arm ends, the m68k walker no
longer stops at the first `OP_RET` (it used to resolve its forward patches
there); it walks on, emits an epilogue per `RET`, and resolves the patches
at the end.  Its two shape matchers learned the fused forms
(`CONST k; RET` for a trivial leaf, `LOAD_MV_RESET j; RET` for a
pass-through).

**The one regression the speed-1 gate flushed out**: `HANDLER_CASE_PUSH`'s
landing is a table of one 5-byte `JMP` per clause that `cl_handler_case_transfer`
indexes at `landing + 5*k`; only the first entry is a target from the
bytecode's own control flow, so the dead-code rewrite deleted the rest
(and the clauses behind them — the second clause of any two-clause
`handler-case` silently became the first).  The decoder now pins every
table entry (`PEEP_PINNED`: always a target, never shrunk), the
differential corpus carries multi-clause handler-cases, and
`tests/test_peephole.c` has the byte-level case.

**Tests**: `tests/test_peephole.c` (47 cases: every fusion, the three
rewrites, the pinned table, the line map, the speed policy),
`tests/test_tier4_phase3.sh` (78 checks: shapes through DISASSEMBLE at
speed 1 / speed 0 / `CLAMIGA_NO_FUSE`, semantics and error paths of every
fused opcode, dynamic bindings, backtrace lines, multi-clause
`handler-case`, `compile-file` round trip, allocation loops; also under
`make test-gc-stress`), the "Tier-4 phase 3" block of
`tests/amiga/run-tests.lisp` (semantics plus JIT-coverage proofs through
`%jit-invoke-count`), and `tests/peephole-corpus.lisp` at four speeds.

**Measured**: docs/benchmarks.md 2026-09-09 (phase 3).

### 4.4 The string-scan fast path ✅ DONE (2026-09-16)

**The finding** (the Clamacs-in-Lisp spike, `clamacs/specs/clamacs-lisp.md`
"What the spike found", item 1): scanning text character by character —
what an editor does on every RET, TAB, paren match and colouring pass —
cost 18-33 us per character on a real 68040-class machine (the declared
and the naive scan after 4.3 and the JIT call fix), i.e. well over a
thousand instructions for `SCHAR` + `CASE` + a list push.  The bytecode
showed why: `(schar s i)` and `(char= c #\x)` were global calls to
builtins (a full call dispatch each), a `CASE` key test compiled to `EQ;
JTRUE body; JMP next` (two dispatches on the miss path), a loop test to
`LT; JNIL`, `(push i stack)` to a LET temporary plus `CONS` plus `STORE`,
and `(incf i)` to six opcodes through two LET temporaries.

**Shipped** (`opcodes.h` 0xC0-0xC4, `compiler.c`, `compiler_extra.c`,
`peephole.c`, `vm.c`, `jit.c`, `runtime.c`, `builtins_array.c`,
`lib/boot.lisp`; `CL_FASL_VERSION` 34):

| opcode | operands | emitted by | what it is |
| --- | --- | --- | --- |
| `AREF k` | u8 kind | compiler: a two-argument `AREF` / `SVREF` / `CHAR` / `SCHAR` | pop index, pop array, push the element.  The VM inlines a fixnum index into a simple base string (every kind) or a simple general vector (`AREF`/`SVREF`); everything else — wide strings, fill-pointer / displaced / bit / byte vectors and every error — is `cl_vector_ref1`, which keeps each accessor's own type gate and error text.  The JIT calls that helper. |
| `CHAREQ` | — | compiler: a two-argument `CHAR=` | pop two characters, push `T`/`NIL`; the builtin's type error otherwise.  Inline in the JIT (tag test + `CMP.L`). |
| `CMP_BR k t` | u8 kind, i32 | peephole: `LT/GT/LE/GE/NUMEQ/CHAREQ; JNIL/JTRUE t` | the comparison and the branch in one: fixnum (or character) fast path, the member's full type gate and cross-type compare otherwise (`cl_vm_compare_kind`).  The kind byte is *synthesized* by the fusion (comparison in bits 0-2, bit 3 = the JTRUE shape) — the first fused opcode whose operands are not just its members' bytes.  The JIT branches straight to the target on the m68k condition. |
| `PUSH_LOCAL s` | u8 slot | compiler: `(push item var)` on an unboxed lexical variable | pop the item, `locals[s] = (cons item locals[s])`, push it.  The item on the VM stack and the list in its slot are the cons's roots. |
| `POP_LOCAL s` | u8 slot | compiler: `(pop var)` on an unboxed lexical variable | push the car, store the cdr; `CAR`'s type error on a non-list. |

`PUSH`/`POP` are intercepted the way `CASE`/`COND` are — ahead of macro
expansion, only for the shape `(push item sym)` / `(pop sym)` where `sym`
is a local of the current function that is neither boxed (captured and
mutated), nor special, nor a symbol macro; every other place, and every
`macroexpand`, sees the genuine macro in `lib/boot.lisp`.  Three shapes
changed with it: `CASE` emits `EQ; JNIL next` for a clause's last key
(one dispatch fewer on the miss path, and it fuses to `EQ_JNIL`); the
`PUSH` macro's variable case is `(setq var (cons item var))` with no
temporary (the item is evaluated first either way); `INCF`/`DECF` on a
variable with a literal delta is `(setq var (+ var delta))` — three
opcodes instead of six.  A new peephole rewrite deletes a `JMP` to the
instruction after it (the exit jump of a `CASE`/`COND` last arm, a `LOOP`
prologue).

**The declared scan, per ordinary character** (state 0, no key
matches): 41 dispatches plus a builtin call before; 26 dispatches and no
call after.  Eight of the 26 are the `CASE` chain over four character
keys (`LOAD_CONST; EQ_JNIL` per key) — a jump table on small integer and
character keys is the next lever if one is needed.

**Measured** (docs/benchmarks.md 2026-09-16, string-scan): on a Vampire
V4 the scan alone went from 18.3 to 2.4-2.8 us per character (naive) and
from 7.6 to 2.0-2.4 (declared, `trunk/bench-scan.lisp`); the Clamacs
spike's RET from about 50 to about 34 ms.  On the host `char-loop` went
from 104 to 66 ms and the new `string-scan` row from 26 to 16 ms (ECL
3.7, SBCL 0.6); no `bench-opt` row got slower.

**Tests**: `tests/test_scan_opcodes.sh` (135 checks in its summary
line — 134 `check_contains`/`check_absent` calls plus the inline count of
`JTRUE`s in a `CASE`: every shape through DISASSEMBLE at speed 1 /
speed 0 / `CLAMIGA_NO_FUSE`, every string and vector representation
through `AREF`, the builtin error texts including `SVREF` on a string,
`CMP_BR` on every number kind and both polarities, `PUSH`/`POP` value and
order on every kind of place, `INCF`/`DECF` on every kind of place, the
`compile-file` round trip, allocation loops; also under
`make test-gc-stress`), `tests/test_peephole.c` (the `CMP_BR` fusion for
every comparison and polarity, the blocked and backward cases, the
jump-to-next rule), `tests/peephole-corpus.lisp` (the scan and each
opcode at four speeds), `tests/test_gc_stress_regression.sh` (the scan
over base and wide strings under forced compaction), and the `jss-`
block of `tests/amiga/test-jit.lisp` (every template's fast, slow and
error path on real m68k, with the native-code proof).

### Expected result and validation

- Phases 1+2 ≈ 1.6–1.7× on sento pinned/tell; all three ≈ 2×.  ECL parity
  (~2.9×) is another 1.5× beyond that and only a host native backend gets
  there.
- Harness: `trunk/bench-prims.lisp` row by row (each item names its row),
  `trunk/bench-opt.lisp` `vm.*`/`mt.*` rows for regressions in the loop,
  `trunk/profile-sento-bench.lisp` + `sample` for the CPU shares, and the
  sento 6-cell matrix (`trunk/sento-bench-matrix.lisp`) as the acceptance
  gate.  Record every step in docs/benchmarks.md.
- Gates: `make test`, `make test-gc-stress` (every item touches allocating
  or rooting paths), `make test-plus`, and `make -f Makefile.cross test-amiga`
  for anything in Phase 2/3 (frame layout, NLX, new opcodes reach the JIT
  walker).

---

## Implementation Order

Recommended sequence balancing impact vs. risk:

| Phase | Items | Status |
|-------|-------|--------|
| 1 | 2.1 (debug gating), 2.2 (hash function), 2.6 (build flags) | ✅ DONE (7f51164) |
| 2 | 2.3 (HT power-of-2), 1.2 (pre-FASL boot), 3.3 (mv_count) | ✅ 2.3+1.2 DONE (7f51164), 3.3 pending |
| 3 | 1.1 (CLOS dispatch cache) | ✅ DONE (b315f2a) |
| 4 | 1.4 (computed goto), 1.5 (TLV bypass) | ✅ DONE |
| 5 | 1.6 (HT rehash + hash fix), 1.7 (32-bit limb bignum mul) | ✅ DONE (3c58761, fbb7e29) |
| 6 | 1.9 (MLF pre-pass hash index — found by profiling; 9.8x cold compile) | ✅ DONE (2026-07-05) |
| 7 | 1.3 (declaim-speed: const-fold + dead-branch + check-elision + local-declare scoping) | ✅ DONE (2026-07-10) |
| 8 | 1.8 (bytecode peephole post-pass, after 1.3 + profiling), 2.5 (free-list segregation) | ✅ 1.8 DONE (2026-07-10); 2.5 pending |
| 9 | 3.1 (slot access), 3.2 (keyword pre-comp) | ✅ 3.1 DONE (2026-07-05: registry hash index + fused slot-access builtins + C GF inline-cache probe); 3.2 pending |
| — | 2.4 (set ops in C) | Deprioritized — measured near-zero real-world use (2026-07-05) |
| 10 | 4.1 (runtime taxes: call_builtin, SLOT-VALUE resolution cache, GF IC without intern/lock, typep by identity, symbol_value(thr), rooted cons) | ✅ DONE (2026-09-08) — +37.6% on sento pinned/tell |
| 11 | 4.2 (call + NLX protocol: OP_CALL/OP_RET slimming, CALL_GLOBAL, setjmp-free NLX frames, handler-case/unwind-protect codegen, inlined local functions, keyword constructors, CNM chains) | ✅ DONE (2026-09-08) |
| 12 | 4.3 (superinstructions via the peephole pass at every speed above 0; the return-shape rewrites; FASL v33; JIT walker templates) | ✅ DONE (2026-09-09) |

Lesson from phase 6: **profile a real workload before picking the next item** — the
biggest win so far (1.9) was not in the plan, and a planned item (2.4) measured
irrelevant. The macro-lookup chain (`cl_macro_p`/`cl_get_macro`/`cl_get_compiler_macro`)
is the current cold-compile leader; re-profile before starting phase 7.

## Validation

- **Every pending item has a dedicated micro-benchmark in `trunk/bench-opt.lisp`**
  (deterministic workloads, closed-form result verification, machine-parseable
  output). Capture a before/after delta against the baseline logged in
  [docs/benchmarks.md](../docs/benchmarks.md) when landing an optimization,
  and append the new numbers there.
- **Tier 4 items map to rows of `trunk/bench-prims.lisp`** (per-primitive ns,
  portable to ECL/SBCL so each row carries a native reference point); the
  sento pinned/tell cell is the acceptance gate for the tier as a whole.
- All 656+ host tests must pass after each phase
- Amiga test suite must pass via FS-UAE after each phase
- Integration tests: `load-and-test-5am.lisp` (57/57), `load-and-test-fset.lisp` (17/17)
- Quicklisp install + `(ql:quickload :alexandria)` must succeed at 24M heap
- No increase in heap usage beyond 5% for equivalent workloads
- Binary size must stay under 150KB for Amiga target
