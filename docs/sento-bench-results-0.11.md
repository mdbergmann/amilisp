# sento Benchmark Results — 0.11 (speed 3)

Point-in-time measurements of the sento actor pipeline on cl-amiga (host)
for the 0.11 cycle — the tree that will be tagged 0.11, one week after the
version bump — run at `(optimize (speed 3))` with the same protocol as the
0.8 and 0.10 entries, plus a same-session A/B against a binary built from
the 0.10 bump commit, and a second pair of legs on sento 3.5.0, the
version the sento checkout moved to during this cycle.

**Headline: 0.11 holds 0.10's figures on five of the six cells, and
reads 15% under it on pinned/tell.** Against the same-session 0.10 binary,
on the same sento 3.4.5 the 0.10 entry measured, pinned/ask-s, pinned/ask,
shared/tell, shared/ask-s and shared/ask are within −5% to +6% (cold and
warm runs bracketing the 0.10 figure on four of them). Pinned/tell — the
plain mailbox handoff, eight senders on one queue lock and one receiver
draining it, the noisiest cell in the series — is the exception: 0.11 read
under the 0.10 binary in nine of nine interleaved single-cell pairs,
medians 277k vs 330k msg/s, while every primitive on that path (the
`bench-opt` VM rows, the `bench-prims` call / struct / lock / condvar rows,
a contended lock-and-condvar probe, the GC telemetry) reads equal or
faster on 0.11; the section "pinned/tell" below has the measurements and
the bisect. Against the 0.10 document's column the matrix reads −5.6% to
+9.1%; against the 0.8 document it is +15% to +310%. No commit of the 0.11
cycle targeted the host message path — the cycle was Clamacs and its heap
image, the development port over TCP, the shutdown and exit-path fixes,
the m68k JIT's lazy compilation and the string-scan fast path. The second
finding is sento's: **3.5.0 is worth +2% to +132% per cell on the same
runtime** (pinned/ask 33k → 78k msg/s, shared/tell 122k → 255k, shared/ask
28k → 62k), and both binaries gain the same amount from it, so the sento
upgrade and the runtime change stay separable — the 0.11 column on 3.5.0
is the figure to carry forward.

Companion documents: [sento-bench-results-0.10.md](sento-bench-results-0.10.md)
is the previous matrix, with the Tier-4 and heap-word-lock A/B this entry's
deltas are computed against and the description of the driver;
[sento-bench-results-0.8.md](sento-bench-results-0.8.md) the "after the
fixes" column the 0.8 deltas refer to;
[sento-bench-results-0.2.md](sento-bench-results-0.2.md) describes the
benchmark itself (N sender threads flooding one receiver actor whose body
increments a counter — the numbers measure framework message-plumbing
overhead, not application work); [benchmarks.md](benchmarks.md) has the
per-primitive entries and the interleaved-A/B method note.

## Environment

- **Commit**: `80572be7` (`master` HEAD; the last runtime commit under it
  is `235e1063`, the shutdown reaper — everything after it is CI, README
  and Clamacs pins). The binary reports `0.11.0`, FASL v39; the version
  bump (`1dd27bbf`, 2026-09-21) had happened a week before.
- **A/B binary**: 0.10 built from `814c7017` (the v0.10 bump; reports
  `0.10.0`, FASL v33), same `make host` in a detached worktree. Between
  the two bump commits, on the host runtime path: the string-scan opcodes
  (`bfac0074`, v34), the growable compiler buffers (`ab6e1001`), the
  compaction forwarding bitmap (`21de5d45`), the nested-allocation
  argument-order fix (`c9f29162`), the DEFVAR init-once change
  (`864fcab4`, v38), the shutdown and exit-path work (`2f95ec9d`,
  `c77fccde`, `235e1063`). The lazy JIT (`134e7fb5`, v39) and the JIT
  call dispatch (`697ac7e9`) are m68k-only.
- **Host**: macOS 27.0 (26A428), arm64 (Apple M3 Ultra) — the machine of
  the 0.8 and 0.10 entries, one OS release later (the 0.10 entry ran on
  macOS 26.6.2). Both binaries built today with Apple clang 21.0.0
  (clang-2100.3.34.2; the 0.10 entry's build was clang-2100.x of Xcode
  26.6), the Makefile's default `make host` (whole-program LTO except
  `vm.o`). No backup was running; two idle amilisp processes of other
  sessions (0% CPU) were parked on the machine.
- **Binary**: `build/host/amilisp` of each worktree, `--heap 192M`,
  `CLAMIGA_FORCE_SPEED=3`.
- **Collector**: generational (host default), TLABs active — both
  binaries.
- **sento**: 3.4.5 at `82cd1e1`, pinned through a detached worktree of the
  `~/Development/MySources/cl-gserver` checkout — the commit the 0.10 entry
  measured, so the runtime A/B is like for like — and 3.5.0 at `700b382`,
  the checkout's current master, for the second pair of legs. 3.4.6–3.5.0
  (ten commits, 2026-09) rework the message path: a timed `ask-s` on a
  shared-dispatcher actor waits on a condition variable, a dispatcher run
  takes its whole batch out of the queue under one lock acquisition and
  dispatches straight into the workers, ask futures are resolved from the
  reply instead of through a waiter mailbox, queues notify only when a
  consumer is waiting and after releasing the lock, and the pinned box
  runs handlers without the reply lock held. `bench.lisp` is unchanged
  between the two.
- **bordeaux-threads**: the fork in `~/quicklisp/local-projects` at
  `941ff8f` (passes `acquire-lock`'s `:timeout` through to the
  three-argument `MP:ACQUIRE-LOCK`, which both binaries have).
- **atomics**: `~/quicklisp/local-projects/atomics` fork at `1caed1a`.
- **Date**: 2026-09-28 (afternoon).

## Reproduction

Same driver and protocol as the 0.10 entry: `trunk/sento-bench-matrix.lisp`
loads `trunk/load-sento-bench.lisp`, runs the six cells back-to-back in one
session and prints a `CELL ... STATS` line per cell with the collector
deltas. Cold ASDF cache per binary through `CLAMIGA_FASL_CACHE_DIR`, sento
pinned through the driver's `ATOMICS_DIR` hook:

```
CLAMIGA_FASL_CACHE_DIR=/path/to/fresh/dir CLAMIGA_FORCE_SPEED=3 \
    ATOMICS_DIR=/path/to/sento-worktree \
    ./build/host/amilisp --no-userinit --heap 192M --non-interactive \
    --load trunk/sento-bench-matrix.lisp
```

Both binaries ran from detached worktrees (`git worktree add --detach
<dir> <commit>`, `make host`, the current driver copied into `trunk/`),
each with its own cache directory. `SENTO_QUEUE_CAP` was not used — both
binaries have the heap-word locks, so shared/ask runs at the default cap.

Legs ran strictly sequentially: 0.11 cold on 3.4.5, 0.11 warm repeat, 0.10
cold on 3.4.5, 0.11 cold on 3.5.0, 0.10 cold on 3.5.0 — 3.2 minutes each,
16 minutes for the series — then the interleaved pinned/tell runs (a
single-cell copy of the driver, three pairs alternating 0.11 and 0.10,
warm caches, one process per run). All 30 matrix cells completed.

One difference at load time: the 0.10 binary's cold compile of serapeum
logs 31 errors that 0.11's does not — `Undefined function:
LET-OVER-LAMBDA` while macroexpanding `FBIND` forms (25 times, from
`serapeum/fbind.lisp:103`), `LET*: malformed binding (NIL NIL)` (5) and
`ASSORT` (1) — under LOAD's per-form recovery, so the affected serapeum
definitions are simply missing in that process. Nothing on the bench path
uses them (all six cells ran, and the 0.10 figures match the 0.10 entry's),
and 0.11 compiles the same files clean; the 0.11 compiler fixes between
the bumps (the MACROLET pre-scan, `d16e3ea9`; the two `make test-extra`
regressions, `d16191f4`) are where that went.

## Reply-mode / dispatcher matrix

Config: `:num-shared-workers 8`, `:load-threads 8`, `:duration 5`,
`:num-iterations 6`, the bench's default `:wait-if-queue-larger-than
10000`, sento 3.4.5. `AVG` is the throughput figure of record (msg/s) from
the cold-cache run; "repeat" is the warm-cache rerun of the same binary;
GC share = total collector time (STW + phases + minors) / cell wall time;
"0.10 doc" is the 0.10 entry's AVG column (same sento, 2026-09-10); "0.8
doc" the 0.8 entry's "after the fixes" column (sento 3.4.4, 2026-08-30).

| Dispatcher | Reply mode | AVG 0.11 s3 | Dev | repeat | GC share | 0.10 doc | 0.11 vs 0.10 doc | 0.8 doc | 0.11 vs 0.8 doc |
| ---------- | ---------- | ----------: | --: | -----: | -------: | -------: | ---------------: | ------: | --------------: |
| **PINNED** | tell       | **287,984** | 23,118 | 299,481 | 1.4% | 302,005 | −4.6% | 175,227 | **+64.3%** |
| PINNED     | ask-s      | **135,934** |  4,438 | 144,713 | 1.4% | 132,478 | +2.6% |  97,733 | **+39.1%** |
| PINNED     | ask        |  **33,440** |    479 |  35,771 | 1.5% |  35,421 | −5.6% |  28,976 | +15.4% |
| SHARED     | tell       | **121,895** |  2,275 | 126,004 | 2.7% | 122,427 | −0.4% |  29,748 | **+309.8%** |
| SHARED     | ask-s      |  **69,404** |  1,645 |  69,300 | 0.8% |  63,594 | **+9.1%** |  45,849 | **+51.4%** |
| SHARED     | ask        |  **28,171** |    247 |  29,594 | 1.1% |  29,747 | −5.3% |  22,302 | +26.3% |

Cold and warm agree within 1% on shared/ask-s, 3–4% on shared/tell and
pinned/tell, 5–7% on the other three (the warm repeat reads higher on five
of six this time — the 0.10 entry had it lower on five of six, so the
cold/warm difference is run-to-run noise, not a cache effect).
Pinned/tell is again the noisiest cell (deviation 8% and 11% of the mean
in the two runs; iterations from 252k to 346k). Shared/ask-s is the one
cell that moved against the 0.10 document, +9.1% — and the same-session
A/B below puts it at −0.6% against the 0.10 binary, so the 0.10 document's
63.6k was the low reading of a cell the "MP locks as heap words" entry in
benchmarks.md had already found to vary by a few percent between sessions.

## Same-session A/B: 0.11 vs the 0.10 binary

Same machine, same session, same sento 3.4.5, both cold-compiled at speed
3; the 0.11 warm repeat as a second reading of the same binary. The last
two columns are the per-message cost.

| Cell         | 0.11 cold | 0.11 warm | 0.10 cold | 0.11 cold vs 0.10 | 0.11 warm vs 0.10 | 0.11 µs/msg | 0.10 µs/msg |
| ------------ | --------: | --------: | --------: | ----------------: | ----------------: | ----------: | ----------: |
| PINNED tell  | **287,984** | 299,481 | 325,110 | −11.4% | −7.9% |  3.47 |  3.08 |
| PINNED ask-s | **135,934** | 144,713 | 142,576 |  −4.7% | +1.5% |  7.36 |  7.01 |
| PINNED ask   |  **33,440** |  35,771 |  33,755 |  −0.9% | +6.0% | 29.90 | 29.63 |
| SHARED tell  | **121,895** | 126,004 | 122,712 |  −0.7% | +2.7% |  8.20 |  8.15 |
| SHARED ask-s |  **69,404** |  69,300 |  69,828 |  −0.6% | −0.8% | 14.41 | 14.32 |
| SHARED ask   |  **28,171** |  29,594 |  28,698 |  −1.8% | +3.1% | 35.50 | 34.85 |

Five cells are a wash: the 0.10 figure sits between the two 0.11 readings
on pinned/ask-s, pinned/ask, shared/tell and shared/ask, and shared/ask-s
agrees within 1% three ways. Pinned/tell is the one cell where both 0.11
readings are under the 0.10 binary (−11% and −8%), which is inside its
noise band (the 0.10 run's own iterations spread from 286k to 361k, the
0.11 runs' from 252k to 346k) but the same sign twice, so it was
re-measured interleaved.

## pinned/tell

**Interleaved single-cell runs.** A copy of the driver with the other
five cells removed, one process per run, warm caches, alternating the two
binaries; three series (the third against a second build of the 0.10
commit, in the bisect worktree — same code, different directory):

| Series | 0.11 (msg/s, per run) | 0.10 (msg/s, per run) | medians |
| ------ | --------------------: | --------------------: | ------: |
| A | 277,234 · 299,322 · 263,073 | 330,155 · 349,036 · 295,325 | 277k vs 330k |
| B | 268,758 · 291,249 · 250,106 | 365,948 · 327,051 · 287,643 | 269k vs 327k |
| C (0.10 rebuilt) | 277,658 · 273,939 · 318,994 | 335,357 · 280,902 · 327,757 | 278k vs 328k |

Nine pairs, 0.11 lower in all nine, −15% on the pooled medians (3.6 vs
3.0 µs per message). Two things bound what that means. The cell's noise
is large and structured: consecutive processes of the *same* binary read
263k and 299k, a series drifts by 10% end to end, and two builds of the
same 0.10 commit from two directories read 283k · 304k · 350k against
304k · 300k · 337k when interleaved — so a single pair says nothing and a
solo series of one binary cannot be compared with another series; only
interleaved pairs count, and nine of nine with the same sign is well
past what the noise produces. The other bound is that the difference does
not show in anything smaller than the cell:

- `trunk/bench-opt.lisp` (best of three runs per binary, interleaved,
  fails=0 on both): 0.11 is equal or faster on all 34 rows — `vm.fixnum-loop`
  44 vs 49 ms, `vm.call-return` 31 vs 33, `vm.local-shuffle` 22 vs 23,
  `safety1.svref-loop` 24 vs 30 (the AREF opcode), `mt.call-x8` 33 vs 35,
  `alloc.cons-churn` 22 vs 27; total 920 vs 949 ms. The `vm.*` rows are
  the check the `vm.c` rule asks for, and they pass.
- `trunk/bench-prims.lisp` at speed 3 (min of two runs, interleaved): the
  40 rows of the sento per-message path are equal or faster on 0.11 —
  `call-1arg` 9 vs 10 ns, `funcall-closure` 16 vs 15, `handler-case` 21
  vs 21, `unwind-protect` 44 vs 44, `struct-read` 1 vs 5, `struct-write`
  6 vs 9, `cons` 4 vs 7, `special-bind` 10 vs 12, `lock-acquire-release`
  51 vs 53, `lock+condvar-notify` 61 vs 63, `gf-1arg-1method` 38 vs 39;
  the only rows up are `catch-throw` 30 vs 26 and `typep-class` 25 vs 22,
  neither on the tell path.
- A contended probe (eight threads pushing onto one list under one lock
  with a condvar notify, a ninth popping under the same lock — the pinned
  box's shape without sento): 0.11 2.2–2.5M handoffs/s in five processes,
  0.10 1.85–2.35M (bimodal between processes), 0.11 ahead in every pair.
- GC telemetry per cell identical (18 vs 21 collections, six compactions
  in 0.35 s on both, 1.4% vs 1.5% of wall).

So the receiver's instruction path, the lock and condvar primitives, the
contended handoff and the collector are all flat or better, and the whole
cell is 15% slower. What the cell adds on top of the probe is sento's
code between the two: CLOS dispatch on `submit`/`receive`, the
`message-item` structs, the queue's `pushq`/`popq` with their
`with-lock-held`, the eight senders' throttle check every 1000 sends, and
the `atomic-incf` on the counter — each of which the rows above time as
equal. The remaining candidates are effects the micro-rows do not see:
cache and code-placement effects in the eight-thread steady state (the
0.8 entry's break-poll counter was one such: a process-wide static on the
opcode path that cost 25% under MT and nothing single-threaded), or a
per-message runtime tax the primitives table has no row for.

**Bisect.** Over the 48 first-parent commits between the bumps that touch
`src/` or `lib/`, each probe built in one worktree and run on the tell
cell in three pairs interleaved with the HEAD build (a solo series per
commit is useless here — the 0.10 commit read 298k in its own series and
328k when interleaved with HEAD the next minute):

| Probe (commit # of 48) | probe median vs HEAD median | pairs probe ahead |
| ---------------------- | --------------------------: | ----------------: |
| 24 `3bbe74a2` (FLET cursor fix)        | 309k vs 280k, **+10%** | 3 of 3 |
| 30 `134e7fb5` (lazy JIT, v39)          | 308k vs 292k, +5.5%    | 2 of 3 |
| 31 `dd273215` (JIT hot-count; `src/jit/runtime.c` only, not compiled on the host — the same host binary as #30) | 284k vs 279k, +1.6% | 1 of 3 |
| 33 `3a369d3f` (reader line table follows a moved cons) | 291k vs 308k, −5% | 1 of 3 |
| 36 `44dd6015` (ARexx; same host binary as #33 but for `builtins_ffi.c`) | 299k vs 304k, −2% | 1 of 3 |
| 48 = HEAD | — | — |

That is a slope, not a step: the probe's resolution at three pairs is
about ±5% (#30 and #31 are the same host code and read +5.5% and +1.6%),
and the commits the slope brackets — `a0188bd6`, which changes how
`FFI:FOREIGN-TO-STRING` finds the NUL, and `3a369d3f`, which rehashes the
reader's source-location table once per collection — do not run on the
message path at all. The bisect therefore does not name a commit, and the
0.11 figures on this cell are reported as measured: −15% against the
0.10 binary, from a change that no single-threaded row, no primitive row
and no contended-handoff probe can see. The 0.8 entry's precedent for
that shape was a process-wide static on the opcode path bouncing one
cache line between threads (`docs/sento-bench-results-0.8.md`); the
0.11 binary's writable statics differ from 0.10's by 38 symbols (the
compaction bitmap's four, the heap-verify report, the CPU self-test flag,
the compiler's cap maxima, the live-thread counters among them), which is
enough to move what shares a line with what. A cheap next probe is
`trunk/bench-opt.lisp`'s `mt.*` rows with a per-message static write
added on purpose, or padding one of the new statics to a cache line and
re-running the three pairs; the recipe for the pairs is in Reproduction.
Open for the 0.11 cycle; it does not block the release, since every
reply cell, the shared dispatcher and the sento 3.5.0 column stand.

**GC telemetry** reads the same on both binaries — the same collection
counts on five cells (the tell cell 18 vs 21, all minors), 21–72
stop-the-world stops per cell, 0.005–0.037 s of STW in 31 s of wall on
every cell but shared/tell (0.27 s on 0.11, 0.21 s on 0.10: the eight
workers' queue lock makes the STW rendezvous wait), 0.8–2.7% of wall in
the collector, zero mark/sweep time, worst single stop 6.1 ms (0.11) vs
6.7 ms (0.10) on the pinned cells and 18.7 vs 24.3 ms on the shared ones.

| Cell         | 0.11 collections (minors) | stops | STW | share | 0.10 collections (minors) | stops | STW | share |
| ------------ | ------------------------: | ----: | --: | ----: | ------------------------: | ----: | --: | ----: |
| PINNED tell  | 18 (12) | 21 | 0.013 s | 1.4% | 21 (15) | 28 | 0.037 s | 1.5% |
| PINNED ask-s | 24 (18) | 35 | 0.008 s | 1.4% | 24 (18) | 34 | 0.006 s | 1.5% |
| PINNED ask   | 32 (25) | 72 | 0.027 s | 1.5% | 32 (25) | 56 | 0.017 s | 1.4% |
| SHARED tell  | 24 (18) | 42 | 0.273 s | 2.7% | 24 (18) | 40 | 0.206 s | 2.6% |
| SHARED ask-s | 26 (19) | 32 | 0.005 s | 0.8% | 26 (19) | 31 | 0.006 s | 0.7% |
| SHARED ask   | 30 (24) | 59 | 0.022 s | 1.1% | 30 (24) | 65 | 0.028 s | 1.1% |

## sento 3.5.0 legs

Both binaries also ran the full matrix against sento 3.5.0 (`700b382`),
cold cache, same session, after the 3.4.5 legs.

| Cell         | 0.11 / 3.5.0 | 0.11 / 3.4.5 | 3.5.0 vs 3.4.5 on 0.11 | 0.10 / 3.5.0 | 0.10 / 3.4.5 | 3.5.0 vs 3.4.5 on 0.10 | 0.11 vs 0.10 on 3.5.0 |
| ------------ | -----------: | -----------: | ---------------------: | -----------: | -----------: | ---------------------: | --------------------: |
| PINNED tell  | **370,121** | 287,984 | **+28.5%** | 413,286 | 325,110 | +27.1% | −10.4% |
| PINNED ask-s | **138,258** | 135,934 |   +1.7% | 139,472 | 142,576 |  −2.2% |  −0.9% |
| PINNED ask   |  **77,706** |  33,440 | **+132.4%** |  72,928 |  33,755 | +116.1% |  +6.6% |
| SHARED tell  | **254,813** | 121,895 | **+109.0%** | 256,115 | 122,712 | +108.7% |  −0.5% |
| SHARED ask-s | **106,330** |  69,404 |  **+53.2%** | 106,761 |  69,828 |  +52.9% |  −0.4% |
| SHARED ask   |  **61,506** |  28,171 | **+118.3%** |  60,434 |  28,698 | +110.6% |  +1.8% |

- **The sento upgrade is the story of this entry.** On the same runtime,
  3.5.0 doubles the two asynchronous-ask cells and shared/tell, adds half
  again on shared/ask-s and a quarter on pinned/tell, and leaves pinned
  ask-s where it was — the one cell whose path (one lock and one condition
  variable per reply on the pinned box) 3.5.0 did not change. The gains
  are where 3.5.0 took lock traffic out: the shared dispatcher's batch
  dequeue under one acquisition and the direct dispatch into workers
  (shared/tell, shared/ask-s), the ask future resolved from the reply
  without the waiter mailbox's own message box — three locks, a queue, a
  thread hop — per message (pinned/ask, shared/ask), and the
  notify-only-when-waiting queues (pinned/tell).
- **Both binaries gain the same.** The 3.5.0-vs-3.4.5 ratio agrees between
  0.11 and 0.10 within a few points on every cell (pinned/ask +132% vs
  +116%, shared/ask +118% vs +111% — the 0.11 side of each pair a little
  higher), and the 0.11-vs-0.10 column on 3.5.0 repeats the 3.4.5 A/B:
  four cells within ±2%, pinned/ask +6.6% for 0.11, pinned/tell −10% —
  the same cell, the same sign, the same size as on 3.4.5, treated in the
  interleaved section above.
- **Per message on 3.5.0 (0.11)**: pinned/tell 2.70 µs, pinned/ask-s 7.23,
  pinned/ask 12.9, shared/tell 3.92, shared/ask-s 9.40, shared/ask 16.3.
  The pinned/tell cell is also noisier on 3.5.0 (deviation 17% of the
  mean, iterations from 247k to 427k) — with less locking in the way, the
  eight senders' scheduling shows through more.
- sento's own README benchmarks (redone for 3.5.0 with `docs/perf-bench.lisp`
  on the same machine class) are a different protocol — fixed message
  counts, no queue throttle, three rounds, median — and are not comparable
  to these cells; they list Clamiga 0.9 next to SBCL, LispWorks, ECL and
  ABCL.

## Observations

- **Five cells did not move, and were not meant to.** The 0.11 commits
  that touch `vm.c` (the five string-scan opcodes) and the compiler
  (growable buffers, the pre-scan fix, DEFVAR) cost nothing on the reply
  cells or on the bench-opt `vm.*` rows — the acceptance the CLAUDE.md
  rule about `cl_vm_run` asks for after any `vm.c` change — and pinned/tell
  is the open item above. The compaction bitmap (`21de5d45`) changes how a
  compaction computes forwarding addresses; the tell cell ran six
  compactions in 0.35 s on both binaries, and the GC telemetry is
  identical.
- **The 0.10 document's figures reproduce**: 0.10 binary today vs the
  0.10 entry — pinned/tell 325k vs 302k (+8%, within that cell's band),
  pinned/ask-s 143k vs 132k (+8%), pinned/ask 33.8k vs 35.4k (−5%),
  shared/tell 123k vs 122k, shared/ask-s 69.8k vs 63.6k (+10%), shared/ask
  28.7k vs 29.7k (−4%). The session-to-session drift the 0.10 entry
  documented (its 0.8 binary reading up to 23% under the August figures)
  is smaller this time and in the other direction, on a newer OS and
  compiler; the same-session columns remain the figures of record.
- **Speed 3 vs the default speed.** Not measured; the 0.3 entry's finding
  (speed 3 adds little since the peephole runs at every speed above 0)
  stands.
- **Cold-load MT soak at speed 3, five legs, two sento versions**: each
  leg cold-compiled sento and its dependency stack through the peephole at
  speed 3 and pushed 10–30M messages across the multi-threaded cells on
  the generational collector; no failure, no straggler at exit (the 0.11
  shutdown now drains workers still in their exit tail — every leg's
  process ended cleanly).
