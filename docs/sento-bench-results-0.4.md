# sento Benchmark Results — 0.4 (speed 3)

Point-in-time measurements of the sento actor pipeline on cl-amiga (host)
at version 0.4, run at `(optimize (speed 3))`. The 0.4 runtime is the
first to combine the two allocator/GC overhauls with the speed-3 compiler
path measured in 0.3: per-thread allocation buffers (TLAB — lock-free
`cl_alloc` under MT) and the generational collector (sliding nursery +
dirty-page tracking, host default).

Companion documents: [sento-bench-results-0.2.md](sento-bench-results-0.2.md)
records the 0.2 baseline and describes the benchmark itself (N sender
threads flooding one receiver actor whose body increments a counter — the
numbers measure framework message-plumbing overhead, not application work);
[sento-bench-results-0.3.md](sento-bench-results-0.3.md) is the 0.3
speed 1 vs speed 3 comparison this entry's deltas are computed against;
[benchmarks.md](benchmarks.md) has the TLAB and generational-GC A/B entries
(default speed) that decompose the 0.4 gains per feature.
[sento-bench-results-0.8.md](sento-bench-results-0.8.md) is the follow-up
measurement, with a same-session A/B against this entry's binary.

## Environment

- **Commit**: `e3ff53e` (branch `perf/gengc`; source identical to
  `cfd2bab`, the v0.4 version bump)
- **Host**: macOS 26.5.2, arm64 (Apple M3 Ultra)
- **Binary**: `build/host/amilisp`, `--heap 192M`
- **Collector**: generational (host default), TLABs active
- **Date**: 2026-07-15

## Reproduction

Run with a **cold ASDF cache** so sento and every dependency compiled at
speed 3 — cached FASLs bypass the compiler, so without the wipe the forced
speed changes almost nothing:

```
rm -rf ~/.cache/common-lisp/amilisp-0.4-fasl23
AMILISP_FORCE_SPEED=3 ./build/host/amilisp --no-userinit --heap 192M \
    --non-interactive --load <driver.lisp>
```

`AMILISP_FORCE_SPEED=3` pins the effective `speed` for the whole process,
overriding declaim/declare (see the peephole section in the README). The
driver loads `trunk/load-sento-bench.lisp` and calls
`sento.bench::run-benchmark` per cell; all six cells ran back-to-back in
one session, with `(ext:%gc-time-stats)` / `(ext:%gengc-stats)` snapshotted
around each cell for the GC-share column.

## Reply-mode / dispatcher matrix

Config: `:num-shared-workers 8`, `:load-threads 8`, `:duration 5`,
`:num-iterations 6`. `AVG` is the throughput figure of record (msg/s);
"0.3 s3" is the speed-3 column of the 0.3 matrix (commit `360abd4`);
GC share = total collector time (STW + phases + minors) / cell wall time.

| Dispatcher | Reply mode | AVG 0.4 s3 | Dev | GC share | 0.3 s3 | 0.4 vs 0.3 |
| ---------- | ---------- | ---------: | --: | -------: | -----: | ---------: |
| **PINNED** | tell       | **188,076** | 7,220 | 1.4% | 140,786 | **+33.6%** |
| PINNED     | ask-s      |  98,564 | 1,195 | 3.9% |  61,674 | **+59.8%** |
| PINNED     | ask        |  32,367 |   205 | 1.8% |  13,216 | **+144.9%** |
| SHARED     | tell       |  31,996 |   227 | 4.0% |  22,168 | **+44.3%** |
| SHARED     | ask-s      |  49,666 |   785 | 1.7% |  25,081 | **+98.0%** |
| SHARED     | ask        |  23,101 | 2,603 | 5.7% |  10,761 | **+114.7%** |

Every cell improved substantially version-over-version; the
allocation-heaviest reply modes gained the most (pinned/ask 2.4×,
shared/ask-s 2.0×, shared/ask 2.1×).

## Observations

- **The gains are the 0.4 allocator/GC work, compounded**: the TLAB entry
  (benchmarks.md, default speed, classic GC) and the generational-GC entry
  (default speed, TLAB base) together predict almost exactly these
  version-over-version deltas. Reply modes cons a future/promise plus a
  lock+condvar per message — the short-lived garbage a nursery reclaims
  for free, allocated on the lock-free TLAB path.
- **Speed 3 on top of the 0.4 runtime** adds ~+4–8% on the pinned cells
  against the default-speed gengc matrix in benchmarks.md (pinned/tell
  180,859 → 188,076, pinned/ask-s 91,230 → 98,564); the shared cells are
  within run-to-run noise (shared/ask has the widest spread, dev 2.6k on
  23.1k, min 17.4k). Same order as the ~4% speed-3 effect the 0.3
  long-run isolated — the per-message path is dominated by C, and the
  peephole/1.3 optimizations only touch its bytecode slice.
- **GC is no longer a first-order cost anywhere in the matrix**: worst
  cell 5.7% of wall (shared/ask, the only cell with significant
  compaction activity — 41 compactions), most cells ≤ 2%. Mark/sweep
  time is zero across all six cells — every collection was either a
  minor (nursery) cycle or a full compaction. Under the 0.3 classic
  collector the reply cells ran up to 21.5% GC share.
- **Cold-load MT soak at speed 3**: the run cold-compiled sento and its
  full dependency stack through the peephole at speed 3, then pushed
  roughly 12M messages across six heavily multi-threaded cells
  (8 sender threads plus dispatcher workers per cell) on the
  generational collector without incident.
- SHARED/tell's wall time (49s for 6×5s iterations vs ~31s in every
  other cell) is queue-drain overhead: fire-and-forget senders outrun
  the 8 shared workers, and each iteration finishes processing its
  backlog after the 5s send window closes.
