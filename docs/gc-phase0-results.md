# GC Phase 0 — where a classic collection cycle spends its time

Measurement gate of `specs/gc-backend-immix.md` (2026-10-08).  Script:
`trunk/gc-phase0.lisp`; counters: `(ext:%gc-time-stats)` and the new
`(ext:%heap-stats)` (free-list probe steps + free-list shape).  Every run
is `--no-userinit --heap 8M --non-interactive`, the classic collector
(host: `CLAMIGA_GENGC=0`).  AmigaOS phase timers tick at 20 ms, so single
cycles are quantised; the churn rows are what to read.

Three workloads per binary:

- **explicit** — four `(ext:gc)` on a quiet heap (the "idle editor" pause);
- **churn** — allocate ~1.5 KB/step (50 conses, a 64-char string, a
  32-slot vector) with a rotating live window of ¼ of the free heap, until
  six allocation-triggered collections ran; per cycle: mark, sweep, the
  free-list steps the allocator walked between cycles, the wall time;
- **loaded** — the same after loading Clamacs (`lib/clamacs/load.lisp`,
  1.8 MB live after the first collection).

## Vampire (68080 FPGA, 500 MB Fast, OS 3.2) — real hardware

Binary: `build/cross/clamiga` from this branch, beside the 0.11 release's
`lib/` (`--no-image`).  `--no-jit` changed nothing (no JIT frame is live
when these collections run), so one table.

| | mark | sweep | sweep share | compact | wall / cycle | free-list steps / cycle |
|---|--:|--:|--:|--:|--:|--:|
| bare explicit (432 K live, bump 640 K) | 40 ms | 0–40 ms | ~25 % | — | 40–80 ms | 0 |
| bare churn, cycle 1 (bump phase) | 140 | 220 | 61 % | — | 3.2 s | 10 K |
| bare churn, cycles 2–6 (free-list phase) | 140–160 | 220 | 58–61 % | — | 6.2–11.9 s | 331 K |
| bare `(ext:gc-compact)` | — | — | — | 200 ms | | |
| loaded explicit, 1st after load | 100 | **2740** | 96 % | — | 2.8 s | 0 |
| loaded explicit, 2nd–4th (1.8 M live, bump 2.4 M) | 140–160 | 20–40 | 12–22 % | — | 160–180 ms | 0 |
| loaded churn, cycle 1 | 240 | 180 | 43 % | — | 2.6 s | 4 K |
| loaded churn, cycles 2–6 | 240–260 | 180–200 | 41–45 % | — | 4.4–7.5 s | 243–254 K |
| loaded `(ext:gc-compact)` | — | — | — | 540 ms | | |
| loaded explicit after compaction (bump 1.8 M) | 120 | 20 | 14 % | — | 140 ms | 0 |

Churn totals: bare 37,400 steps in 44.7 s, collector 2.2 s (4.9 %),
**1,136 µs/step** of mutator; loaded 28,000 steps in 30.4 s, collector
2.6 s (8.5 %), 993 µs/step.

What the rows say:

1. **Sweep is proportional to the arena, mark to the live set.**  With the
   8 MB arena full of garbage the sweep is a flat 180–220 ms whatever the
   live set; on a dense 1.8–2.4 MB heap it is 20–40 ms.  Mark goes
   40 → 140–160 → 240–260 ms as the live set goes 0.4 → 1.8 → 3 MB:
   roughly **80 ms per live MB**, ~2 µs per object.
2. **Allocation pays for the free list between cycles.**  Cycle 1 of each
   churn run allocates from the bump front (10 K probe steps); every later
   cycle allocates from the free list (331 K steps, ~1 per allocation) and
   the *same* work takes 2–4× the wall time: 3.2 s → 6.2–11.9 s per cycle
   bare, 2.6 s → 4.4–7.5 s loaded.  The GC share of wall (5–8 %)
   understates the collector's cost by that factor — the allocation path
   is where the time goes on a fragmented heap.
3. **The idle-editor pause is the mark.**  Explicit collections on the
   loaded, dense heap are 140–180 ms of which sweep is 20–40.  An Immix
   sweep does not touch that number; a cheaper mark loop or a smaller
   traced set would.
4. **The first collection after a FASL load is something else**: 2.74 s of
   "sweep" for a 2.4 MB walk.  The walk is the same as the 20 ms one two
   cycles later; the difference is `gc_finalize_dead` on thousands of
   dead top-level-form bytecodes, each releasing 4–7 `AllocVec`'d buffers
   — tens of thousands of `FreeVec` calls, whose cost grows with the
   fragmentation of exec's chunk list.  That is an off-heap allocator
   problem (a pool for bytecode buffers), not a collector one, and it does
   not occur after an image boot (dense heap, no dead loader bytecodes).
   Left as a separate item.
5. Compaction (sliding, whole arena): 200 ms bare, 540 ms loaded — the
   same order as one churn cycle's mark+sweep, cheaper than the 2–4×
   allocation tax it removes for a while.

## Host (M3 Ultra, macOS), `CLAMIGA_GENGC=0` — reference only

| | mark | sweep | sweep share | free-list steps / cycle | wall / cycle |
|---|--:|--:|--:|--:|--:|
| classic, bare explicit (432 K live) | 0.2 ms | 0–0.1 ms | — | 0 | ≤ 1 ms |
| classic, bare churn, cycle 1 (bump phase) | 0.7 | 0.8 | 53 % | 17 K | **7 ms** |
| classic, bare churn, cycles 2–6 (free-list phase) | 0.6–0.7 | 0.7–0.8 | 53 % | 320–331 K | **11–20 ms** |
| gengc (default), bare churn, cycles 1–6 | minors (not in these timers) | — | — | 0 | 6–9 ms |

Churn: classic 1.9 µs/step, gengc 1.4 µs/step.  Even with the host's
caches the free-list phase costs 1.6–2.9× the bump phase for the same
work, and the generational collector — which never leaves the bump front
— is the fastest of the three by exactly that margin.  The sweep share
itself (~53 %) matches the 68k.

## FS-UAE 68040 (JIT, `verify.fs-uae`)

Same binary and legs; Clamacs loaded **from source** here (no FASLs in
the build tree: 30 s compile, which filled the 8 MB heap to 8.1 MB
without a single collection).  `--no-jit` again identical.

| | mark | sweep | sweep share | compact | wall / cycle | free-list steps / cycle |
|---|--:|--:|--:|--:|--:|--:|
| bare explicit (432 K live) | 20–40 ms | 0–40 ms | ~40 % | — | 40–80 ms | 0 |
| bare churn, cycle 1 (bump phase) | 120 | 120 | 50 % | — | 2.6 s | 10 K |
| bare churn, cycles 2–6 (free-list phase) | 120 | 120–140 | 50–54 % | — | 5.7–11.5 s | 331 K |
| bare `(ext:gc-compact)` | — | — | — | 160 ms | | |
| loaded explicit, 1st after load (8.1 M bump) | 80 | **540** | 87 % | — | 620 ms | 0 |
| loaded explicit, 2nd–4th (1.86 M live) | 140–160 | 20 | 11–13 % | — | 160–180 ms | 0 |
| loaded churn, cycles 1–4 (incl. one forced compaction) | 155 avg | 25 avg | 14 % | 880 | 2.4 s / 4 cycles | 63 K |
| loaded churn, cycles 5–6 | 160 | 120 | 43 % | — | 5.9–8.1 s | 276 K |
| loaded `(ext:gc-compact)` | — | — | — | 420 ms | | |
| loaded explicit after compaction (bump 1.86 M) | 80–100 | 0–20 | ≤ 20 % | — | 100 ms | 0 |

Churn totals: bare 37,400 steps in 41.8 s, collector 1.5 s (3.6 %),
1,078 µs/step; loaded 11,200 steps in 16.5 s, collector 2.2 s (13 %),
1,277 µs/step.  The emulated 040 and the Vampire agree on every shape:
the same 2–4× wall-time tax once allocation leaves the bump front, the
same ~50 % sweep share under churn, the same 12–20 % on a dense heap, and
the same first-collection-after-load outlier (540 ms here, 27× the steady
sweep; smaller than the Vampire's 2.7 s because this exec's memory list
is fresh).

## FS-UAE 68020 (`verify-lowend.fs-uae`, cycle-exact, no JIT, 8 MB Fast)

The release's floor.  Same binary; Clamacs loaded from the FASL cache
the 68040 run left behind (58 s).  The bare churn alone is 20 minutes of
emulated time here.

| | mark | sweep | sweep share | compact | wall / cycle | free-list steps / cycle |
|---|--:|--:|--:|--:|--:|--:|
| bare explicit, 1st (432 K live, bump 640 K) | 900 ms | 1360 ms | 60 % | — | 2.3 s | 0 |
| bare explicit, 2nd–4th | 1120 | 160–180 | 13 % | — | 1.3 s | 0 |
| bare churn, cycle 1 (bump phase) | 4600 | 4780 | 51 % | — | **83 s** | 10 K |
| bare churn, cycles 2–6 (free-list phase) | 4600 | 4780–4800 | 51 % | — | **168–323 s** | 331 K |
| bare explicit after compaction (bump 432 K) | 880–900 | 120–140 | 12–14 % | — | 1.0 s | 0 |
| loaded explicit, 1st after load (bump 8.1 M) | 3140 | **7900** | 72 % | — | 11.1 s | 0 |
| loaded explicit, 2nd–4th (1.8 M live, bump 2.4 M) | 3580–3600 | 520 | 13 % | — | **4.1 s** | 0 |
| loaded churn, cycle 1 (bump phase) | 6460 | 4080 | 39 % | — | 71 s | 8 K |
| loaded churn, cycles 2–6 (free-list phase) | 6460 | 4080–4100 | 39 % | — | 118–206 s | 254 K |
| loaded `(ext:gc-compact)` | — | — | — | **14.7 s** | | |
| loaded explicit after compaction (bump 1.8 M) | 3120–3140 | 440–460 | 12–13 % | — | 3.6 s | 0 |

Churn: bare 37,400 steps in 1,200 s, collector 56 s (4.7 %),
**30.6 ms/step** of mutator; loaded 28,800 steps in 822 s, collector
63 s (7.7 %), 26.3 ms/step.  Scale against the Vampire: ×22–27 on every
phase and on the mutator — one machine, one shape.  What that means for
a 68020 user: a collection on a quiet 0.4 MB heap is 1–1.3 s; with the
editor resident it is **4.1 s, 87 % of it mark**; one on an 8 MB heap
full of garbage is 9.4–10.5 s, 40–50 % of it sweep; between two such
collections the program runs 2–4× slower than it would from a bump
front; and a full compaction is 15 s.

## MorphOS box

Offline on the day (no agent on .211/.65/.213/.212); not measured.

## Verdict against the gate

The gate in the spec was "sweep + free-list probing under ~30 % of
collector wall → Immix is the wrong lever".  Measured on real 68k:

- under allocation churn the sweep is **41–61 %** of the pause and
  free-list allocation **doubles to quadruples** the mutator's cost per
  step — both are exactly what a mark-region collector removes;
- on a quiet dense heap the pause is **85 % mark**, which it does not.

So Part 2 is justified for the churn case (compiling, editing with
garbage, any allocation-heavy program) and the allocation path, and is
*not* the fix for the idle-editor pause.  That one needs the mark loop
looked at on its own: 80 ms per live MB on the 68080 and ~2.4 s per
live MB on the 68020 is slow for a header-walk-and-push loop (~2 µs and
~55 µs per object respectively), and it is a smaller, independent piece
of work that the Part 1 seam makes easier to A/B.  Two more items fell
out, neither a collector change: the FASL-load finalization storm
(§Vampire 4) and the fact that a from-source Clamacs load fills an 8 MB
heap to the brim without one collection — the bump front is so cheap
relative to a cycle that the allocator never asks for one until it has
nothing left, then pays the whole arena's sweep at once.

Order of work this suggests: Part 1 (seam) → Part 2 (Immix) for the
churn/allocation tax → mark-loop pass on the 68k (bench via the seam)
→ bytecode-buffer pool for the loader storm.
