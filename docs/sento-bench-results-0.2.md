# sento Benchmark Results — 0.2

Point-in-time measurements of the sento actor pipeline running on cl-amiga
(host). Records the environment, exact reproduction commands, and results so
later runs can be compared like-for-like.

Related: [docs/benchmarks.md](benchmarks.md) is the general runtime/optimization
measurement log; this file focuses on the sento actor benchmark specifically.
[sento-bench-results-0.3.md](sento-bench-results-0.3.md) is the follow-up
measurement comparing `(optimize (speed 1))` vs `(speed 3)` (peephole post-pass).

The benchmark is `sento.bench::run-benchmark` from the cl-gserver/sento
`bench.lisp`. It stands up an actor system, spawns N sender threads that flood a
single receiver actor (whose body just increments a counter), and reports
messages/second via `trivial-benchmark`. Because the receiver does no real work,
the numbers measure **framework message-plumbing overhead**, not application
work — real actors will show smaller relative gaps between the variants.

## Environment

- **Commit**: `47c0182`
- **Host**: macOS 26.5.2, arm64
- **Binary**: `build/host/amilisp`, `--heap 192M`
- **Date**: 2026-07-06

## Reproduction

The bundled script only *loads* `bench.lisp` (which defines the functions); it
does not run anything. Load it, then call `run-benchmark`:

```lisp
(load "trunk/load-sento-bench.lisp")
(sento.bench::run-benchmark :dispatcher :pinned      ; or :shared
                            :with-reply-p nil        ; t => request/reply
                            :async-ask-p  nil        ; t (with reply) => async ask
                            :num-shared-workers 8
                            :duration 5
                            :num-iterations 6
                            :load-threads 8)
```

```
./build/host/amilisp --heap 192M --load <driver.lisp>
```

Cold cache wants ~192M (cl-unicode + serapeum). Once warm, `--heap 64M` is
plenty.

> **Cold-load flakiness**: on a fresh process the serapeum/closer-mop cold load
> is intermittently non-deterministic (e.g. `Package DIRECT-SLOT-DEFINITION not
> found`, `Undefined function: MAKE-SEMAPHORE`). It is a load-time race, not a
> benchmark issue — simply retry the run until the load succeeds.

## Reply-mode / dispatcher matrix

Config: `:num-shared-workers 8`, `:load-threads 8`, `:duration 5`,
`:num-iterations 6`. `AVERAGE` is the throughput figure of record;
`MIN`–`MAX` shows the per-iteration spread. (The raw `TOTAL` column is
trivial-benchmark summing per-iteration rates and is not meaningful.)

| Dispatcher | Reply mode                | AVG msg/s | MIN     | MAX     | MEDIAN  | Deviation |
| ---------- | ------------------------- | --------: | ------: | ------: | ------: | --------: |
| **PINNED** | tell (fire-and-forget)    | **120,989** | 116,331 | 125,209 | 120,016 | 2,805 |
| PINNED     | ask-s (synchronous reply) |    62,809 |  60,885 |  64,889 |  62,491 | 1,517 |
| PINNED     | ask (async reply)         |    12,620 |  12,150 |  12,872 |  12,691 |   249 |
| SHARED     | ask-s (synchronous reply) |    23,013 |  19,804 |  24,997 |  22,625 | 1,687 |
| SHARED     | tell (fire-and-forget)    |    21,051 |  20,826 |  21,430 |  20,895 |   232 |
| SHARED     | ask (async reply)         |    10,201 |  10,068 |  10,346 |  10,096 |   124 |

### Observations

- **Pinned dominates for `tell`**: 121k vs 21k msg/s — a **~5.7×** edge. A
  pinned actor owns one dedicated thread with a tight mailbox loop; the shared
  dispatcher routes every message through 8 shared workers, so per-message
  dispatch/queue-handoff overhead dwarfs the trivial `incf` receiver here.
- **Reply mode is the dominant cost**, in order `tell` → `ask-s` → `ask`:
  - `ask-s` (synchronous request/reply) roughly **halves** pinned throughput
    (121k → 63k) — each send blocks for a round-trip.
  - `ask` (async reply via future) is **~10× slower** than `tell` on pinned
    (121k → 13k) — it allocates a future/promise and reply plumbing per message.
- **Shared narrows the gap under reply load**: with `ask-s`, shared (23k) edges
  out shared-`tell` (21k) and closes on pinned's disadvantage — once each
  message already pays a round-trip, spreading across 8 workers helps more than
  it hurts. Pinned still wins every row outright.
- **Consistency**: all variants are tight (deviation ≤ ~2.4%), so the orderings
  are solid rather than noise. The jumpiest is SHARED/`ask-s`
  (min 19.8k, max 25k).

## Longer pinned/tell run (throughput ceiling)

Config: `:dispatcher :pinned`, `:with-reply-p nil`, `:num-shared-workers 8`,
`:load-threads 8`, `:duration 10`, `:num-iterations 60`.

| Metric              |     TOTAL |       MIN |       MAX |    MEDIAN |   AVERAGE | Deviation |
| ------------------- | --------: | --------: | --------: | --------: | --------: | --------: |
| MESSAGES-PER-SECOND | 8,099,294 | 120,246 | 148,591 | 135,394 | **134,988** | 6,196 |
| REAL-TIME (s)       |   613.99  |   10.109  |   10.333  |   10.211  |   10.233  | 0.064 |
| RUN-TIME (s)        |   613.99  |   10.109  |   10.333  |   10.211  |   10.233  | 0.064 |

Sustained ~135k msg/s average across 60 iterations / ~10.2 min, deviation ~4.6%.
The 5s×6 pinned/tell matrix cell (121k) undershoots this slightly because the
shorter run amortizes warm-up less; ~135k is the better steady-state figure.

## Notes

- Bumping senders from 4 → 8 threads did **not** raise pinned/tell average
  throughput (139k @ 4 threads on an earlier smoke run vs 135k @ 8 threads).
  With a single pinned receiver, that receiver's one processing thread is the
  bottleneck; extra senders add contention, not throughput. To push higher the
  lever is the receive side (more/faster receivers), not more load threads.
- Context vs history: memory records the sento actor bench previously moving
  55.7k → 118k msg/s after spec 3.1. The ~135k here (pinned/tell, warm) is
  consistent with that trajectory.
