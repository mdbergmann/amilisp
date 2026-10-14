# sento Benchmark Results — 0.3 (speed 1 vs speed 3)

Point-in-time measurements of the sento actor pipeline on cl-amiga (host),
comparing `(optimize (speed 1))` against `(optimize (speed 3))` — i.e. the
spec 1.8 bytecode peephole post-pass plus the speed-gated spec 1.3
optimizations applied to sento and its entire dependency stack.

Companion to [sento-bench-results-0.2.md](sento-bench-results-0.2.md), which
records the 0.2 baseline and describes the benchmark itself (N sender threads
flooding one receiver actor whose body increments a counter — the numbers
measure framework message-plumbing overhead, not application work).

## Environment

- **Commit**: `360abd4` (branch `feat/peephole-speed2`)
- **Host**: macOS 26.5.2, arm64 (Apple M3 Ultra)
- **Binary**: `build/host/amilisp`, `--heap 192M`
- **Date**: 2026-07-11

## Reproduction

Each speed ran with a **cold ASDF cache** so sento and every dependency
compiled at the target speed — cached FASLs bypass the compiler, so without
the wipe a forced speed changes almost nothing:

```
rm -rf ~/.cache/common-lisp/amilisp-0.3-fasl22
AMILISP_FORCE_SPEED=1 ./build/host/amilisp --no-userinit --heap 192M \
    --non-interactive --load <driver.lisp>       # then again with =3
```

`AMILISP_FORCE_SPEED` pins the effective `speed` for the whole process,
overriding declaim/declare (see the peephole section in the README). The
driver loads `trunk/load-sento-bench.lisp` and calls
`sento.bench::run-benchmark` per configuration; the two speed runs executed
strictly sequentially to avoid CPU contention.

## Reply-mode / dispatcher matrix

Config: `:num-shared-workers 8`, `:load-threads 8`, `:duration 5`,
`:num-iterations 6`. `AVG` is the throughput figure of record (msg/s).

| Dispatcher | Reply mode | AVG s1 | AVG s3 | s3 vs s1 | Dev s1 | Dev s3 |
| ---------- | ---------- | -----: | -----: | -------: | -----: | -----: |
| **PINNED** | tell       | **142,836** | **140,786** | −1.4% | 4,350 | 5,056 |
| PINNED     | ask-s      |  62,644 |  61,674 | −1.5% |   180 | 1,160 |
| PINNED     | ask        |  13,387 |  13,216 | −1.3% |   112 |   171 |
| SHARED     | tell       |  22,249 |  22,168 | −0.4% |   278 |   198 |
| SHARED     | ask-s      |  25,073 |  25,081 | +0.0% |   493 |   267 |
| SHARED     | ask        |  10,754 |  10,761 | +0.1% |   117 |   108 |

Every difference is within one standard deviation of a single run — **the
6-iteration matrix cannot distinguish the two speeds**.

## Longer pinned/tell run (throughput ceiling)

Config: `:dispatcher :pinned`, `:with-reply-p nil`, `:num-shared-workers 8`,
`:load-threads 8`, `:duration 10`, `:num-iterations 60`.

| Metric (msg/s)  | speed 1 | speed 3 |
| --------------- | ------: | ------: |
| AVERAGE         | 139,960 | **145,592** |
| MEDIAN          | 139,253 | 146,032 |
| MIN             | 127,543 | 125,696 |
| MAX             | 152,956 | 159,927 |
| Deviation       |   6,658 |   7,648 |

At 60 iterations the standard error is under 1k msg/s, so the **~4% average
gain (medians +4.9%) is a real effect**, not noise — a plausible size for the
bytecode fraction of sento's per-message path receiving the peephole's
8–12% dispatch-loop improvement.

## Observations

- **Why the matrix is flat**: sento's message plumbing is dominated by C —
  thread handoff, locks/condvars, and the spec 3.1 C dispatch fast paths.
  Bytecode shape only touches the Lisp slice of the per-message path, and
  6×5s runs have too much variance to resolve a low-single-digit effect.
  The 60-iteration steady-state run resolves it: ~+4% for speed 3.
- **Against the 0.2 baseline** (commit `47c0182`): both speeds sit above it —
  matrix pinned/tell 121k → ~141–143k, long-run average 135k → 140k (s1) /
  145.6k (s3) — consistent with the optimizations landed since.
- **MT soak**: the speed-3 leg cold-compiled the full dependency stack through
  the peephole and ran a heavily multi-threaded, NLX-heavy workload for
  ~12 minutes without incident, on top of the sento system test suite
  (537/537) already passing at `AMILISP_FORCE_SPEED=3`.
