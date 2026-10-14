# sento Benchmark Results — 0.10 (speed 3)

Point-in-time measurements of the sento actor pipeline on cl-amiga (host)
for the 0.10 cycle — master ahead of the 0.10 version bump — run at
`(optimize (speed 3))` with the same protocol as the 0.8 entry, plus a
same-session A/B against binaries built from the 0.9 and 0.8 bump commits
and from the 0.10 tree *before* the MP locks became heap words.

**Headline: 0.10 is the fastest entry in this series on every cell, by a
wide margin, and every cell runs at the bench's standard configuration.**
Against the same-session 0.8 binary it is +50% to +360% per cell
(pinned/tell 165k → 302k msg/s, pinned/ask-s 88k → 132k, shared/tell 27k →
122k), and it beats the 0.4 binary on every cell. The gain has two parts,
both in the 0.10 cycle: the Tier-4 work (`specs/performance.md` 4.1–4.3:
runtime taxes, the call/unwind protocol, superinstructions) is worth +25%
to +106% per cell over 0.8, and the MP locks as heap words
(`specs/mp-locks-heap-words.md`, `04a2a410`) add +12% on pinned/tell, +20%
on pinned/ask-s and +194% on shared/tell on top of that, cost −15% on
shared/ask-s, and let shared/ask run at the default queue cap again. The
first 0.10 matrix (same day, before the lock change) had found that cell
**filling amilisp's 16384-slot lock table** — the faster sender side kept
more asynchronous asks in flight than the table held — and had measured it
at a lowered queue cap only. There is no lock table any more; the section
"shared/ask and the lock table" keeps the analysis and the before/after
figures. The 0.9 and 0.8 binaries read the same within noise.

Companion documents: [sento-bench-results-0.2.md](sento-bench-results-0.2.md)
records the 0.2 baseline and describes the benchmark itself (N sender
threads flooding one receiver actor whose body increments a counter — the
numbers measure framework message-plumbing overhead, not application work);
[sento-bench-results-0.3.md](sento-bench-results-0.3.md) is the speed 1 vs
speed 3 comparison; [sento-bench-results-0.4.md](sento-bench-results-0.4.md)
the 0.4 matrix; [sento-bench-results-0.8.md](sento-bench-results-0.8.md)
the previous matrix, its root-cause bisects and the "after the fixes"
column this entry's deltas are computed against;
[benchmarks.md](benchmarks.md) has the Tier-4 phase entries (per-primitive
costs, opcode profiles, acceptance cells), the "MP locks as heap words"
entry (the lock primitives' own costs) and the interleaved-A/B method note
this entry follows.

## Environment

- **Commit**: `04a2a410` (`master` HEAD, PR #37: the heap-word locks and
  their docs commits squashed onto `6989f7aa`; the legs were built from
  the same tree before the squash). The binary still reports `0.9.0`,
  FASL v33 — the 0.10 bump had not happened yet, so the version-keyed
  cache directory is `cl-amiga-0.9-fasl33`.
- **A/B binaries**: "pre-lock" built from `6989f7aa` (the 0.10 tree the
  first matrix of this entry ran on: Tier 4 complete, locks still in the
  table; reports `0.9.0`, FASL v33); 0.9 built from `224340cf` (the v0.9
  bump; reports `0.9.0`, FASL v31); 0.8 built from `cabfed45` (the v0.8
  bump; reports `0.8.0`, FASL v31) — each with the same `make host` in a
  detached worktree. Between the two bump commits there is no
  runtime-performance commit (audio, release tooling, FFI diagnostics, the
  off-heap shutdown fixes); between the 0.9 bump and `6989f7aa` are the
  three Tier-4 phases and their follow-ups; between `6989f7aa` and HEAD the
  lock change and two docs commits.
- **Host**: macOS 26.6.2, arm64 (Apple M3 Ultra) — the same machine and OS
  build as the 0.8 entry. All four binaries were built today with Apple
  clang 21.0.0 (Xcode 26.6), the Makefile's default `make host`
  (whole-program LTO except `vm.o`, as since the 0.8 fixes); the 0.8 entry
  did not record its compiler version. A Time Machine backup was running
  during the series; the machine was otherwise idle.
- **Binary**: `build/host/amilisp` of each worktree, `--heap 192M`,
  `CLAMIGA_FORCE_SPEED=3`.
- **Collector**: generational (host default), TLABs active — all four
  binaries.
- **sento**: 3.4.5, `~/Development/MySources/cl-gserver` at `82cd1e1`,
  pinned through a detached worktree of that commit (the checkout's
  working tree carries an uncommitted change to the timed `ask-s` wait,
  which the bench does not exercise). The 0.8 entry ran 3.4.4 (`013ab63`).
  3.4.5 (2026-09) changed the reply path in `message-box.lisp`: a
  shared-dispatcher `ask-s` now gets its own lock and condition variable
  per message item (it used to share one per box, the cross-wired-replies
  bug), the reply-side notify runs under that lock, and the waiter loops on
  a `done-p` flag instead of trusting the wakeup. Every leg of the matrix
  ran 3.4.5, so the same-session A/B is clean; the "0.8 doc" column below
  folds the sento change in with the runtime change, and the "sento 3.4.4
  legs" section separates the two.
- **bordeaux-threads**: the fork in `~/quicklisp/local-projects` at
  `941ff8f`, which passes `acquire-lock`'s `:timeout` through to the new
  three-argument `MP:ACQUIRE-LOCK`. The three older binaries have a
  two-argument `ACQUIRE-LOCK`, so their legs load the fork's previous
  commit `92113d6` from a detached worktree (see Reproduction).
- **atomics**: `~/quicklisp/local-projects/atomics` fork at `1caed1a`.
  `cas`/`atomic-incf` map onto `mp:compare-and-swap` in all four binaries.
- **Date**: 2026-09-10 (evening; the first matrix of this entry ran the
  same morning on `6989f7aa`).

## Reproduction

Same driver and protocol as the 0.8 entry: `trunk/sento-bench-matrix.lisp`
loads `trunk/load-sento-bench.lisp`, runs the six cells back-to-back in one
session and prints a `CELL ... STATS` line per cell with the
`(ext:%gc-time-stats)` / `(ext:%gengc-stats)` deltas and the derived GC
share. Run with a **cold ASDF cache** so sento and every dependency compile
at speed 3. Rather than wiping `~/.cache/common-lisp/<version dir>` (shared
with any other amilisp session on the machine), point
`CLAMIGA_FASL_CACHE_DIR` at a fresh directory — the runtime namespaces it
by version and FASL format the same way, and the warm repeat reuses it:

```
CLAMIGA_FASL_CACHE_DIR=/path/to/fresh/dir CLAMIGA_FORCE_SPEED=3 \
    ./build/host/amilisp --no-userinit --heap 192M --non-interactive \
    --load trunk/sento-bench-matrix.lisp
```

For the A/B legs, `git worktree add --detach <dir> <commit>`, `make host`
there, copy the current driver into its `trunk/`, and run the same command
from that directory with its own cache directory. The driver's
`ATOMICS_DIR` hook takes one or more directories separated by `:` and
pushes each onto `asdf:*central-registry*`, ahead of quicklisp's
local-projects and the dist; it prints which `atomics.asd`,
`bordeaux-threads.asd` and `sento.asd` loaded. Every 3.4.5 leg of this
entry passed a worktree of sento `82cd1e1` through it (the 3.4.4 legs one
of `013ab63`); the three older binaries also got a worktree of the
bordeaux-threads fork at `92113d6`, because the
fork's current version calls `MP:ACQUIRE-LOCK` with the timeout argument
that only the heap-word locks accept. `SENTO_QUEUE_CAP=<n>` passes `n` as
`:wait-if-queue-larger-than` to every cell; it was needed for the shared/ask
cell on the pre-lock binary (see below) and is not needed on HEAD. Do not
run the whole matrix with it: a pre-lock run of all six cells at cap 2000
read 202,933 ±32,591 on pinned/tell against 274,098 at the default cap —
the cap throttles the `tell` senders there — while ask-s, ask and
shared/tell were within 6% of their default-cap figures. The
supplementary runs in this entry used a single-cell copy of the driver
(the matrix driver with the other five `run-cell` lines removed) with the
same arguments.

Legs ran strictly sequentially, in this order: HEAD cold, HEAD warm
repeat, pre-lock cold, 0.9 cold, 0.8 cold, HEAD cold against sento 3.4.4,
the supplementary shared/ask cell at queue cap 2000 on pre-lock and on
HEAD (warm caches), then 0.8 cold against sento 3.4.4. A cold load
(quickload of sento, serapeum, cl-unicode, log4cl and trivial-benchmark at
speed 3 — 284 FASLs written into the fresh cache directory) takes under
ten seconds on any of the four binaries, so a leg is the sum of its cells
(~3.3 minutes on HEAD, where shared/tell no longer drains a backlog) and
the whole series ran in about 25 minutes.

## Reply-mode / dispatcher matrix

Config: `:num-shared-workers 8`, `:load-threads 8`, `:duration 5`,
`:num-iterations 6`, the bench's default `:wait-if-queue-larger-than
10000`. `AVG` is the throughput figure of record (msg/s) from the
cold-cache run; "repeat" is the warm-cache rerun of the same binary; GC
share = total collector time (STW + phases + minors) / cell wall time;
"0.8 doc" is the "after the fixes" column of sento-bench-results-0.8.md
(sento 3.4.4, 2026-08-30); "0.4" is that entry's same-session 0.4 binary.

| Dispatcher | Reply mode | AVG 0.10 s3 | Dev | repeat | GC share | 0.8 doc | 0.10 vs 0.8 doc | 0.4 | 0.10 vs 0.4 |
| ---------- | ---------- | ----------: | --: | -----: | -------: | ------: | --------------: | --: | ----------: |
| **PINNED** | tell       | **302,005** | 34,517 | 281,736 | 1.5% | 175,227 | **+72.3%** | 191,254 | **+57.9%** |
| PINNED     | ask-s      | **132,478** |  5,471 | 134,981 | 1.2% |  97,733 | **+35.6%** | 103,441 | **+28.1%** |
| PINNED     | ask        |  **35,421** |    348 |  33,320 | 1.8% |  28,976 | +22.2% |  33,239 | +6.6% |
| SHARED     | tell       | **122,427** |  1,684 | 117,583 | 2.5% |  29,748 | **+311.5%** |  30,204 | **+305.3%** |
| SHARED     | ask-s      |  **63,594** |    494 |  61,787 | 1.7% |  45,849 | **+38.7%** |  49,673 | **+28.0%** |
| SHARED     | ask        |  **29,747** |    318 |  28,244 | 1.4% |  22,302 | **+33.4%** |  25,466 | +16.8% |

All six cells ran to completion in both HEAD legs. The two runs agree
within 2% on pinned/ask-s and 3% on shared/ask-s, within 4–7% on the
other four (the warm repeat reads lower on five of six); pinned/tell is
the noisiest cell in the series (deviation 11% of the mean across its six
iterations, min 258k, max 357k) and its 302k is the cold run's average,
with the warm repeat at 282k. Shared/ask, which aborted in both runs of
the first matrix of this entry, now runs at the default cap and reads
29.7k; the supplementary section below has the like-for-like figure
against the pre-lock binary's cap-2000 measurement.

## Same-session A/B: 0.10 vs pre-lock 0.10 vs 0.9 vs 0.8 binaries

Same machine, same session, same sento 3.4.5, all four cold-compiled at
speed 3. "pre-lock" is the 0.10 tree before the heap-word locks
(`6989f7aa`); its shared/ask cell aborts (lock table full) at the default
cap, so that row carries its cap-2000 figure from the supplementary run,
marked. The last three columns are the per-message cost the two 0.10
binaries remove relative to 0.8.

| Cell         | 0.10 AVG | pre-lock AVG | 0.9 AVG | 0.8 AVG | 0.10 vs pre-lock | 0.10 vs 0.8 | pre-lock vs 0.8 | 0.9 vs 0.8 | 0.10 µs/msg | pre-lock µs/msg | 0.8 µs/msg |
| ------------ | -------: | -----------: | ------: | ------: | ---------------: | ----------: | --------------: | ---------: | ----------: | --------------: | ---------: |
| PINNED tell  | **302,005** | 270,318 | 168,770 | 165,239 | **+11.7%** | **+82.8%** | +63.6% | +2.1% |  3.31 |  3.70 |  6.05 |
| PINNED ask-s | **132,478** | 110,296 |  91,704 |  88,050 | **+20.1%** | **+50.5%** | +25.3% | +4.1% |  7.55 |  9.07 | 11.36 |
| PINNED ask   |  **35,421** |  36,419 |  24,841 |  22,460 | −2.7% | **+57.7%** | +62.1% | +10.6% | 28.23 | 27.46 | 44.52 |
| SHARED tell  | **122,427** |  41,636 |  27,125 |  26,629 | **+194.0%** | **+359.8%** | +56.4% | +1.9% |  8.17 | 24.02 | 37.55 |
| SHARED ask-s |  **63,594** |  75,171 |  35,938 |  36,496 | **−15.4%** | **+74.2%** | +106.0% | −1.5% | 15.72 | 13.30 | 27.40 |
| SHARED ask   |  **29,747** | *28,558 (cap 2000)* |  18,618 |  17,569 | (+4.2%) | **+69.3%** | (+62.5%) | +6.0% | 33.62 | *35.02* | 56.92 |

0.9 and 0.8 are the same runtime for this workload (six cells within −1.5%
to +10.6%; the +10.6% is pinned/ask, where the 0.8 binary reads 6% under
its morning figure — see the 3.4.4 section), which is what the commit
range between the two bumps predicts. The pre-lock column reproduces the
morning's matrix on the same commit within −1.4% to +9% (274k / 108.6k /
33.4k / 39.1k / 72.3k then; the cap-2000 cell 28.7k then, 28.6k now), so
the two 0.10 columns can be read against each other: the lock change is
+12% on pinned/tell, +20% on pinned/ask-s, +194% on shared/tell, noise on
pinned/ask, −15% on shared/ask-s, and turns shared/ask from an abort into
a cell that runs at the standard configuration 4% above the pre-lock
binary's throttled figure. Everything else in the 0.10 column — the +25%
to +106% of the pre-lock column over 0.8 — is the Tier-4 work between the
0.9 bump and `6989f7aa`.

**GC telemetry** — the one place the lock change shows up beyond the
throughput. The pre-lock binary's pinned/ask-s cell ran 215 collections
(209 minors) with 732 stop-the-world stops and 1.22 s of STW time in 31 s
of wall (5.7% of wall in the collector); the same cell on HEAD ran 24
collections (18 minors), 32 stops, 0.007 s of STW, 1.2%. Pinned/ask: 223
collections / 690 stops / 0.93 s before, 32 / 66 / 0.03 s after.
Shared/ask-s: 155 / 321 / 0.40 s before, 20 / 23 / 0.003 s after. The
tell cells, which allocate no locks per message, ran the same 18 and 24
collections on both binaries. The 0.8 and 0.9 binaries sit in between
(pinned/ask-s 174 and 181 collections, 0.34 and 0.39 s of STW). Those
extra collections were the lock table's: every synchronous ask allocates a
lock and a condition variable per message on the pinned box (both sento
versions), an asynchronous ask allocates three locks, and a slot in the
16384-entry table was only reclaimed when the collector ran the wrapping
object's finalizer, so at 90–130k lock allocations per second the table
filled every 0.1–0.2 s and `MAKE-LOCK` forced a minor collection each
time — the seven collections per second the pre-lock binary shows on
every reply cell, and the "collector's share went up, in stop-the-world
time" observation the morning's matrix had recorded against Tier 4. A
lock is now an ordinary heap object; the collector runs when the
allocator says so, and HEAD spends 1.0–2.5% of wall in it across the
matrix against 1.2–5.7% before. Mark/sweep time is zero in every cell of
every leg; the worst single stop-the-world pause on the pinned cells is
5.7 ms (HEAD) vs 8.9 ms (pre-lock) vs 6.1 ms (0.8).

## shared/ask and the lock table

Before `04a2a410`, `MP:MAKE-LOCK` handed out ids from a fixed
16384-entry table (`CL_MAX_LOCKS`, `src/core/thread.h`; 256 on the
Amiga); a slot was reclaimed when the wrapping `CL_Lock` object was
collected, and on a full table `MAKE-LOCK` ran a minor collection, then a
full one, and errored only if both freed nothing. The error therefore
meant more than 16384 locks were *reachable* at once.

The bench's asynchronous `ask` (`sento.actor:ask`, identical in 3.4.4 and
3.4.5) allocates a temporary waiting actor per message — an
`async-waitor-actor` with its own `message-box/dp` (one lock for the box,
one lock and one condition variable for its queue) plus a future (one
lock) — that stays reachable until the reply has been delivered through
the shared dispatcher and the waiter has stopped itself. Three locks per
in-flight ask put the ceiling at ~5,400 asks in flight. The bench
throttles its 8 senders on the *receiver's* queue depth (sleep when it
exceeds `wait-if-queue-larger-than`, default 10000, checked every 1000
sends per sender), which bounds nothing near that. The 0.8 and 0.9
binaries never hit the ceiling; the Tier-4 sender side (CLOS
instantiation, dispatch, the submit path) was fast enough to push the
working set past it, and the cell aborted 5–10 s in with
`MP:MAKE-LOCK: lock table full (max 16384)` — today's pre-lock leg again,
after 5.1 s, with 72 compactions in those 5 s (46% of wall in the
collector) from the retry path. A probe of the allocator itself had read
≈3 µs per `MAKE-LOCK` with 15,000 live locks — the linear slot scan was
not what limited this cell, the cap was.

With locks as heap words there is no table, no cap and no finalizer: a
lock is a state word (owner and a contended bit) and a recursion depth in
the object, a condition variable a waiter count, and a blocked thread
parks on its own per-thread handle (`specs/mp-locks-heap-words.md`). The
cell runs at the default cap on HEAD — both matrix legs, 29,747 and
28,244 — and the like-for-like comparison against the throttled pre-lock
measurement is this, one run per binary in the same session, each in a
process of its own (warm cache):

| Binary | SHARED ask, cap 2000 | at default cap 10000 | GC share (cap 2000) |
| ------ | -------------------: | -------------------: | ------------------: |
| 0.10 (HEAD) | **29,499** (dev 551) | **29,747** (dev 318) | 2.9% |
| pre-lock (`6989f7aa`) | 28,558 (dev 288) | *lock table full* | 3.4% |
| 0.9    | 18,624 (dev 331, morning) | 18,618 | 1.5% |
| 0.8    | 18,617 (dev 206, morning) | 17,569 | 2.1% |

HEAD reads the same at either cap, as the older binaries always did (their
in-flight set stayed under the ceiling either way; their cap-2000 figures
are the morning's, the default-cap ones today's). The pre-lock binary's
28.6k at cap 2000 is within 0.4% of the morning's 28.7k — the throttle
did not cost it throughput, it only kept the working set under the table —
so the lock change is worth +4% on this cell's message path (33.6 µs
per message vs 35.0), and the pre-lock binary's cap-2000 figure stands in
for it in the A/B table above. The cell is 5.6× more GC-active on the
pre-lock binary for the reason the telemetry paragraph gives (168
collections against 30 at cap 2000).

The morning's matrix had left a runtime follow-up here — grow the tables
in chunks, or raise the host cap and add a free list. Neither was done;
the tables are gone instead, and `SENTO_QUEUE_CAP` remains in the driver
only for running binaries older than `04a2a410`.

## sento 3.4.4 legs

To separate the sento upgrade from the runtime change, the HEAD and 0.8
binaries also ran the full matrix against sento 3.4.4 (`013ab63`, the
checkout the 0.8 entry used) — cold cache, same session, after the legs
above. "0.8 doc" is again the 0.8 entry's "after the fixes" column, which
was measured on 3.4.4.

| Cell         | 0.10 / 3.4.4 | 0.10 / 3.4.5 | 3.4.5 vs 3.4.4 on 0.10 | 0.8 / 3.4.4 | 0.8 / 3.4.5 | 0.8 doc (3.4.4, Aug) | 0.8 / 3.4.4 today vs doc |
| ------------ | -----------: | -----------: | ---------------------: | ----------: | ----------: | -------------------: | -----------------------: |
| PINNED tell  | **306,616** | 302,005 | −1.5% | 164,784 | 165,239 | 175,227 |  −6.0% |
| PINNED ask-s | **141,878** | 132,478 | −6.6% |  89,573 |  88,050 |  97,733 |  −8.3% |
| PINNED ask   |  **34,017** |  35,421 | +4.1% |  22,934 |  22,460 |  28,976 | **−20.9%** |
| SHARED tell  | **120,651** | 122,427 | +1.5% |  26,956 |  26,629 |  29,748 |  −9.4% |
| SHARED ask-s |  **55,927** |  63,594 | **+13.7%** |  37,556 |  36,496 |  45,849 | **−18.1%** |
| SHARED ask   |  **28,256** |  29,747 | +5.3% |  17,158 |  17,569 |  22,302 | **−23.1%** |

Three things follow.

- **The sento version is worth a few percent either way, and the sign
  depends on the cell.** On HEAD, 3.4.5 costs 6.6% on pinned/ask-s (the
  per-item finalize in an `unwind-protect` and the notify under the item's
  lock on the reply path) and 1.5% on pinned/tell, and gains 13.7% on
  shared/ask-s and 4–5% on the two async cells — the caller now waits on a
  condition variable after an asynchronous dispatch instead of 3.4.4's
  synchronous hop through the worker's own reply path, and with the
  heap-word locks the per-item lock and condition variable cost less than
  that hop did. The morning's pre-lock matrix had the same shape at
  smaller amplitude (3.4.5 −2% on the tell cells, +3.4% on shared/ask-s).
  On the 0.8 binary 3.4.5 is −1% to −3% on four cells (pinned/ask-s
  −1.7%, shared/ask-s −2.8%), flat on pinned/tell and +2.4% on shared/ask
  — the sento upgrade is not what moved any column of this entry.
- **shared/ask runs on HEAD with either sento** (28.3k on 3.4.4, 29.7k on
  3.4.5); on the pre-lock binary it filled the lock table with either,
  4.2 s into the cell on 3.4.4 in the morning's run. The asynchronous-ask
  path is identical in the two versions; the cap was a runtime effect, as
  the section above says.
- **The 0.8 binary reads under its earlier figures, today more than in
  the morning.** Against the morning's same-day 0.8 leg it is −1% to −6%
  on five cells (pinned/tell 165k vs 171k, pinned/ask-s 88k vs 93k,
  pinned/ask 22.5k vs 23.8k) and flat on shared/ask-s; against the August
  column it is −6% to −23%, the same three cells the morning had flagged
  (pinned/ask, shared/ask-s, shared/ask) reading lowest. The runtime is
  the same, the atomics fork differs from August by documentation commits
  only, the OS build is the same; a Time Machine backup ran during
  today's series and the compiler version was not recorded in August.
  Whatever moves between sessions moves every binary in a session
  together — the pre-lock binary reproduced its morning figures within a
  few percent — which is why this entry's figures of record are the
  same-session A/B columns and not the doc-to-doc deltas: "+35.6%" and
  "+22.2%" against the 0.8 doc on pinned/ask-s and pinned/ask are the
  conservative reading, and the same-session numbers say +50.5% and
  +57.7%.

## Observations

- **Where the gain is.** Per message, pinned/tell dropped from 6.05 µs
  (0.8) to 3.31 µs and pinned/ask-s from 11.4 to 7.6 — the plain mailbox
  handoff and the synchronous reply, where the 0.8 entry had found ~1 µs
  of runtime regression, now sit well under the 0.4 binary (5.2 and 9.7
  µs). Tier 4 took the first 2.4 and 2.3 µs off those two cells (the
  pre-lock column: 3.70 and 9.07 µs) and the lock change the next 0.4 and
  1.5 — on the reply cell, mostly the collections the lock table had been
  forcing. The async cells and the shared dispatcher gained the most in
  absolute terms: −12 to −29 µs per message on shared/tell, pinned/ask,
  shared/ask-s and shared/ask. Tier 4's share of that is the Lisp-side
  work per message — CLOS instantiation of the waiting actor, the
  dispatcher's `ask-s` hop to a worker, `handler-case` around every
  handler invocation, `unwind-protect` cleanup, slot access on the message
  items — which is exactly what it targeted (CALL_GLOBAL, `%handler-case`
  as a special form, the multiple-value save stack, the per-thread slot
  cache, the superinstructions); the lock change's share is the
  contended paths, below.
- **Shared/tell: the thundering herd is gone.** 41.6k → 122.4k msg/s on
  the lock change alone. Eight shared workers block on one queue lock;
  the table-backed lock woke every waiter on every release and they
  re-contended, the heap-word lock wakes one. The cell's wall time tells
  the same story from the other side: 47 s for six 5-second iterations
  on the pre-lock binary (52 s on 0.9 and 0.8) — the queue-drain overhead
  the 0.4 entry describes, fire-and-forget senders outrunning the workers
  and each iteration finishing its backlog after the send window closes —
  is 31.5–31.9 s on HEAD, the same as every other cell. The workers now
  keep up with eight senders.
- **Shared/ask-s is the one cell the lock change costs**, −15% against
  the pre-lock binary (75.2k → 63.6k) in a matrix leg and −4 to −6% when
  the "MP locks as heap words" entry in benchmarks.md re-measured it
  interleaved. Every condition-variable wait and notify takes the
  process-wide `cl_thread_list_lock` to walk the registered waiters, and
  this cell does one wait and one notify per message on the shared box
  (3.4.5's per-item condition variable) on top of the dispatcher's
  queue handoff. The spec's "Implementation notes" record the follow-up —
  a per-object waiter chain of thread serials — that would take the global
  lock off that path. At 63.6k the cell is still +74% over 0.8 and +28%
  over 0.4.
- **Pinned/ask is flat on the lock change** (36.4k → 35.4k, −2.7%, inside
  the cold/warm spread) despite its three locks per message: the locks
  are uncontended there (one waiting actor, one reply), so the change
  neither helps nor hurts the message path, and the collections it
  removes (223 → 32) were cheap — 0.9 s of STW in 31 s.
- **Speed 3 vs the default speed.** Not measured in this entry. Since
  phase 3 the peephole pass runs at every speed above 0, so the
  superinstruction gain does not depend on the forced speed; the 0.3
  entry's finding that speed 3 adds little on top holds a fortiori.
- **Cold-load MT soak at speed 3, six times over** (HEAD twice,
  pre-lock, 0.9, 0.8 twice; two sento versions): each leg cold-compiled
  sento and its full dependency stack through the peephole at speed 3
  and pushed 11–21M messages across the multi-threaded cells on the
  generational collector; the only failure in the series is the
  pre-lock binary's lock-table cap.
