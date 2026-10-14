# GC Phase 0 — where a classic collection cycle spends its time

Measurement gate of `specs/gc-backend-immix.md` (2026-10-08).  Script:
`trunk/gc-phase0.lisp`; counters: `(ext:%gc-time-stats)` and the new
`(ext:%heap-stats)` (free-list probe steps + free-list shape).  Every run
is `--no-userinit --heap 8M --non-interactive`, the classic collector
(host: `AMILISP_GENGC=0`).  AmigaOS phase timers tick at 20 ms, so single
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

Binary: `build/cross/amilisp` from this branch, beside the 0.11 release's
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

## Host (M3 Ultra, macOS), `AMILISP_GENGC=0` — reference only

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

# Phase 3 — the same measurement, Immix against classic (2026-10-13)

The same script on the same emulated machines, now with both m68k
binaries from the `gc/seam` branch built the same afternoon (the two
`GC=` variants of `make -f Makefile.cross amiga`, run through
`verify/realamiga/run-gc-phase0.sh BINDIR [CONFIG]`).  The
classic rows below are that day's re-run, not the 2026-10-08 ones above
(they agree within the 20 ms timer quantum).  The "sweep" column is the
Immix line sweep on the Immix rows; "free-list steps" is a classic-only
quantity (Immix allocates by bump into holes and never probes).  The
real hardware (the Vampire and the MorphOS box) was off that day — those
rows are still open.

## FS-UAE 68040 (JIT, `verify.fs-uae`)

| | mark | sweep | compact | wall / cycle | free-list steps / cycle |
|---|--:|--:|--:|--:|--:|
| **classic** bare explicit (432 K live) | 20–40 ms | 0–40 ms | — | 40–80 ms | 0 |
| **immix** bare explicit (433 K live) | 20–40 | 0–60 | — | 40–100 | — |
| **classic** bare churn, cycle 1 (bump phase) | 120 | 120 | — | 2.3 s | 5.6 K |
| **classic** bare churn, cycles 2–6 (free-list phase) | 100–120 | 120–140 | — | **5.7–8.6 s** | 331 K |
| **immix** bare churn, cycles 1–6 | 160 | 0–20 | — | **1.9–2.3 s** | — |
| **classic** bare `(ext:gc-compact)` | — | — | 280 | | |
| **immix** bare `(ext:gc-compact)` | — | — | **1140** | | |
| **classic** loaded, 1st gc after the Clamacs load (bump 8.1 M) | 100 | **540** | — | 640 ms | 0 |
| **immix** loaded, the one gc inside the Clamacs load | 120 | 380 | — | 500 ms | — |
| **classic** loaded explicit, 2nd–4th (1.86 M live) | 140–160 | 20–40 | — | 160–180 ms | 0 |
| **immix** loaded explicit (1.86 M live) | 120–140 | 0–20 | — | 120–140 ms | — |
| **classic** loaded churn, cycles 1–3 (one policy compaction) | 140 avg | 27 avg | 1140 | 2.5 s / 3 cycles | 63 K |
| **classic** loaded churn, cycles 4 and 6 (free-list phase) | 160 | 120 | — | **8.5–8.8 s** | 276 K |
| **immix** loaded churn, cycles 1–6 | 220–240 | 0–20 | — | **1.5–2.1 s** | — |
| **classic** loaded `(ext:gc-compact)` | — | — | 640 | | |
| **immix** loaded `(ext:gc-compact)` | — | — | **1440** | | |
| **classic** loaded explicit after compaction | 80–100 | 0–20 | — | 100 ms | 0 |
| **immix** loaded explicit after compaction | 120 | 0–20 | — | 120–140 ms | — |

Churn totals, the number that matters:

| | steps | wall | collector | mutator µs/step |
|---|--:|--:|--:|--:|
| classic bare | 37,400 | 36.5 s | 1.5 s (4.1 %) | **936** |
| immix bare | 37,400 | 11.9 s | 1.0 s (8.2 %) | **293** |
| classic loaded | 12,200 | 20.7 s | 2.4 s (11.6 %) | **1,503** |
| immix loaded | 25,000 | 10.1 s | 1.4 s (14.3 %) | **346** |

What the rows say:

1. **The allocation tax is gone.**  The same churn runs 3.1× faster bare
   and 4.3× faster with the editor loaded, and the per-cycle wall is flat
   (1.5–2.3 s) instead of tripling once the classic allocator leaves the
   bump front.  The collector's share of wall *rises* (8–14 %) only
   because the mutator got so much cheaper; in absolute terms the
   collector does less (sweep 0–20 ms instead of 120–140).
2. **The sweep is out of the pause.**  Line sweep: 0–20 ms per cycle
   against 120–140 ms, on a heap of the same size and shape.  The mark
   is unchanged (160 ms bare, 220–240 ms loaded — the mark stays the
   Phase 0 mark, 80 ms per live MB).
3. **The idle-editor pause is the same**, as predicted: 120–140 ms
   against 160–180, all of it mark.  The mark loop remains the next
   lever for that case.
4. **The first collection after a FASL load** shrinks from 640 to 500 ms;
   what stays is the finalization storm, which is off-heap work and not a
   collector cost (§Vampire 4 above).
5. **Compaction is 4–5× more expensive under Immix** (1140/1440 ms against
   280/640): the sliding compactor walks every dead header, and a heap
   the line sweep has been recycling carries its garbage as hundreds of
   thousands of un-coalesced corpses, where the classic sweep had already
   merged them into 1,700 free blocks.  It is also the rare path — the
   policy asked for **no** compaction in either churn (classic needed one
   in the loaded churn), so the Phase 4 question ("does the compactor run
   often enough to matter?") answers itself for this workload: it ran
   only when the script asked for it explicitly.  A corpse-coalescing
   walk, or evacuation, would be the fix if a workload ever makes it
   matter.
6. **Heap use**: Immix left the bump front at 8 MB throughout (holes are
   recycled in place, the front only moves at a compaction) and kept the
   same live set (1.86 M loaded) with 0.8 % of the heap in its line map.
   The Clamacs load itself ran one collection under Immix (the hole
   allocator asks for a cycle when no hole fits) where classic filled the
   heap to 8.1 M without one; both loads took 30–32 s.

## FS-UAE 68020 (`verify-lowend.fs-uae`, cycle-exact, no JIT, 8 MB Fast)

The classic rows are the 2026-10-08 ones from the first table (same
config, same binary up to the seam, which was neutral); the Immix rows
are the 2026-10-13 run.  Clamacs loaded from the FASL cache both times.

| | mark | sweep | compact | wall / cycle | free-list steps / cycle |
|---|--:|--:|--:|--:|--:|
| **classic** bare explicit, 2nd–4th (432 K live) | 1120 ms | 160–180 ms | — | 1.3 s | 0 |
| **immix** bare explicit, 2nd–4th (432 K live) | 1240 | 200 | — | 1.5 s | — |
| **classic** bare churn, cycle 1 (bump phase) | 4600 | 4780 | — | 83 s | 10 K |
| **classic** bare churn, cycles 2–6 (free-list phase) | 4600 | 4780–4800 | — | **168–323 s** | 331 K |
| **immix** bare churn, cycles 1–6 | 6020–6120 | 200–220 | — | **63–74 s** | — |
| **classic** bare `(ext:gc-compact)` | — | — | (not taken bare) | | |
| **immix** bare `(ext:gc-compact)` (bump 8 M, 1.9 M live) | — | — | **38.2 s** | | |
| **classic** loaded explicit, 1st after load (bump 8.1 M) | 3140 | **7900** | — | 11.1 s | 0 |
| **immix** loaded explicit, 1st after load (bump 8.1 M) | 4420 | 5960 | — | 10.4 s | — |
| **classic** loaded explicit, 2nd–4th (1.8 M live) | 3580–3600 | 520 | — | 4.1 s | 0 |
| **immix** loaded explicit, 2nd–4th (1.86 M live) | 4400 | 240 | — | 4.7 s | — |
| **classic** loaded churn, cycle 1 (bump phase) | 6460 | 4080 | — | 71 s | 8 K |
| **classic** loaded churn, cycles 2–6 (free-list phase) | 6460 | 4080–4100 | — | **118–206 s** | 254 K |
| **immix** loaded churn, cycles 1–6 | 8300–8480 | 240–260 | — | **53–69 s** | — |
| **immix** loaded `(ext:gc-compact)` (bump 8 M, 3.1 M live; classic 14.7 s) | — | — | **48.0 s** | | |

Churn totals:

| | steps | wall | collector | mutator per step |
|---|--:|--:|--:|--:|
| classic bare | 37,400 | 1,200 s | 56 s (4.7 %) | **30.6 ms** |
| immix bare | 37,600 | 390 s | 38 s (9.6 %) | **9.4 ms** |
| classic loaded | 28,800 | 822 s | 63 s (7.7 %) | **26.3 ms** |
| immix loaded | 25,200 | 339 s | 52 s (15.2 %) | **11.4 ms** |

What the rows say, with the 68040's numbers as the reference:

1. **The same 3× on the mutator** (30.6 → 9.4 ms per step bare, 26.3 →
   11.4 loaded), and the same flat cycle: 63–74 s against 83 s once and
   168–323 s thereafter bare, 53–69 s against 71 then 118–206 s loaded.
   For a 68020 user this is the difference between a program that
   allocates and one that crawls after its first collection.
2. **The sweep goes from 4.8 s to 0.2 s per cycle.**  The line sweep is
   O(lines), 65,536 bytes to walk, and it shows: 200–240 ms whatever the
   heap holds.
3. **The mark is 10–30 % slower under Immix here** (1240 vs 1120 ms on
   the quiet heap, 6.0–6.1 s vs 4.6 s under churn, 8.3–8.5 vs 6.5 s
   loaded).  On a cycle-exact 68020 there is no locality to lose, so this
   is work: the per-object line stamp (`GCB_ON_MARK`: line index from the
   offset, a `memset` of one or two bytes) on top of a ~55 µs object.  A
   single-line fast path in the stamp is the obvious first cut; it is the
   mark-loop pass Phase 0 already named, now with a second reason.
4. **The quiet-heap pause is unchanged in kind**: 4.1 → 4.7 s with the
   editor resident, 95 % of it mark.  Immix was never going to fix that
   one; item 3 is where it goes next.
5. **Compaction**: 38 s for an 8 MB Immix heap carrying 1.9 M live, 48 s
   carrying 3.1 M (classic: 14.7 s loaded), for the reason given under
   the 68040 — every corpse is walked.  No churn run asked for one.
6. **The first Immix run wedged in the loaded churn's third cycle**: the
   log stopped mid-line after cycle 2 and nothing followed for 15
   minutes, so the watchdog ended it.  That is the shape of the known
   FS-UAE wedge (`verify/realamiga`, a log line cut mid-write), and the
   re-run of the whole script with the same binary went through to the
   end: the loaded rows above are the re-run's, its cycles 1–2 match the
   wedged run's (68.6/54.7 s against 68.4/54.5 s), and cycles 3–6 sit at
   53–55 s with the same mark and sweep.

# Mark-loop pass — the trace itself (2026-10-14)

Phase 0 and Phase 3 both ended at the same place: on a quiet heap the
pause is the mark, and Immix does not touch it.  This pass is the mark
loop alone (`src/core/gc_trace.c`, shared by every backend — see the
spec's "As landed — the mark-loop pass" for what changed: the object is
marked when its reference is *visited*, leaf types never enter the stack,
the per-slot visit is inlined, the cons is scanned without the call into
the per-type switch, and Immix stamps a one-line object with one store).

What the 68020 disassembly of the Phase 3 binary showed is the reason it
was slow: at `-Os` every slot visit was a call with four tag tests before
it and every popped object a second call into the walk — ~110
instructions per cons, the ~55 µs per object of Phase 0.

The measurement is `trunk/gc-mark-bench.lisp`: a synthetic live set
(lists of fixnums, lists of 8-byte strings, 16-slot vectors of conses, a
hash table — 2.1 MB live after a cycle on the Amiga, 32 MB on the host),
then five explicit `(ext:gc)`; the number is the mark phase of
`(ext:%gc-time-stats)` per cycle.  Same script, same emulated machine,
binaries of the Phase 3 tree against this one.

## FS-UAE 68020 (`verify-lowend.fs-uae`, cycle-exact, no JIT), 2.1 MB live

| binary | mark per cycle | per live MB | vs. before |
|---|--:|--:|--:|
| classic, Phase 3 tree | 5,444 ms | 2,611 ms | |
| classic, step 1 (mark at the visit, leaf skip, inline visit) | 2,236 ms | 1,072 ms | 2.4× |
| **classic, + step 3 (the per-walk context)** | **1,648 ms** | **790 ms** | **3.3×** |
| Immix, Phase 3 tree | 7,624 ms | 3,603 ms | |
| Immix, step 1 | 4,080 ms | 1,928 ms | 1.9× |
| Immix, + step 2 (one-store line stamp) | 3,720 ms | 1,758 ms | 2.0× |
| **Immix, + step 3 (the per-walk context)** | **2,768 ms** | **1,308 ms** | **2.75×** |

Every cycle of the five is within 20 ms of the others (the emulator is
cycle-exact), so one run per binary is the number.

## Host (M3 Ultra, macOS), 32 MB live, best of three

| binary | mark per cycle |
|---|--:|
| classic (`AMILISP_GENGC=0`), Phase 3 tree | 11.3 ms |
| classic, this pass | 6.6 ms |
| Immix, Phase 3 tree | 13.2 ms |
| Immix, this pass | 8.5 ms |

The host at `-O3 -flto` had the calls inlined already; what it gains is
the leaf skip and the pop with no checks.  The one-store stamp is not
visible there (8.5–8.7 ms either way).

What the rows say:

1. **The idle pause is 3.3× cheaper on the 68020 under classic and
   2.75× under Immix; the Immix mark now sits at half of classic's
   Phase 3 number.**  The loop was the cost, not the memory system: on a
   cycle-exact machine the instruction count is the time, and the three
   steps removed about two thirds of it per visited slot.
2. **Step 3 is the 68020's step.**  Caching the arena base, the arena
   size, the stack, the mark sense, the epoch and the line map in the
   walk's locals (`GcMarkCtx`) is invisible on the host (`-O3` kept them
   in registers already) and worth 26 % on the 68020, where every
   absolute-address load is two extension words and a bus cycle.  The
   remaining Immix cost per visit — the sense XOR, the stamp, the byte
   count, the validation flag — is now 1.1 s per cycle on 2.1 MB; on the
   68040 and the host the locality Immix buys back under churn is worth
   more than that.
3. **Binary size**: the inlined visit costs about 6 KB (classic,
   897,484 bytes) and 12 KB (Immix, 907,572) of m68k text — the per-type
   switch expands the visit at every slot; the context made the
   expansion smaller than the global-reading form, not larger.

## The Immix per-visit trims (2026-10-14, same day)

The 1.1 s the Immix visit still cost over classic's was itemised and
two of the items taken out of the loop (spec, "The per-visit trims").
Same bench, same emulated 68020, same 640-unit live set; one run each
(the cycles agree within 20 ms).

| Immix binary | mark per cycle | over classic |
|---|--:|--:|
| mark-loop pass (above) | 2,768 ms | +1,120 ms |
| + the validation test out of the precise drain | 2,612 ms | +956 ms |
| **+ the mark set by flipping the bit** | **2,572 ms** | **+916 ms** |
| experiment: + no live byte count (not landed) | 2,540 ms | +884 ms |

Classic, same tree: 1,656 ms (1,648 before — the drain now exists twice
and the children walk takes a flag it folds away; noise).

What the rows say:

1. **The validation test was worth 6 %.**  It was one register test
   per visited slot, but the register it needed was one the 68020 does
   not have: the drain's disassembly showed the arena size, the mark
   sense and the line map spilled to the stack around it.  Now the
   precise drain has no trace of it and the two phases that need it (the
   conservative JIT roots, the overflow rescan) run a second
   instantiation of the same loop — 552 bytes of m68k text.
2. **The mark flip is worth another 1.5 %.**  An object the visit just
   found unmarked has its bit equal to the sense, so one `bchg` marks
   it; the general form needed the `~bit` mask *and* the `bit ^ sense`
   constant, and the constant lived on the stack.
3. **The live byte count stays.**  The experiment binary dropped the
   per-object `marked += size` and derived the count from the line map
   at sweep time instead: 32 ms per cycle, 1.2 %, is what one register
   add costs here — the size it adds is already in a register for the
   line stamp.  And the line-level count is not the same number: it
   reports 2,319 KB live for the same 2,167 KB of objects (every live
   line counts in full), which is exactly the quantity the compaction
   policy subtracts from the hole total to see line-level
   fragmentation; with the two equal, that test can never fire.  (The
   first form of the experiment, with no count at all, never finished:
   `ROOM` reported zero live bytes, the bench's size-driven build loop
   never ended, and the policy asked for a compaction after every
   cycle.)  `trunk/gc-mark-bench.lisp` gained `*mark-bench-units*` for
   that run — a fixed live set regardless of what the binary under test
   reports.

What is left of the gap — 916 ms on 2.1 MB, 0.5 µs per object — is
the design: the sense XOR on the marked test, the line stamp, the byte
add.  The remaining avoidable cost is register pressure (the arena size
and line map still sit on the stack), which `-Os` on the 68020 decides,
not the source.

m68k text: Immix 907,572 → 907,600 (+28 bytes net: the second drain
against the shorter visit); classic 897,484 → 897,880.

Host (M3 Ultra, 32 MB live, best of three on a quiet machine): classic
5.2 ms, Immix 6.0 ms per cycle.  Both are under the pass's rows above
(6.6 / 8.5), the classic one with no change to its hot path, so the
difference is the machine's state on the day, not the trims; the gap
between the two is what matters, and it went from 1.9 to 0.8 ms.

# Real hardware — Immix against classic (2026-10-14)

The Phase 3 measurement and the mark bench on the two machines that were
off on 2026-10-13, with both binaries built from the same tree (the
per-visit trims, `c569c96d`).  Everything the FS-UAE rows showed holds
on the hardware; the numbers below are the ones a user of either machine
will see.

## Vampire (68080 FPGA, 500 MB Fast, OS 3.2)

`build/cross/amilisp` (Immix) and `build/cross-classic/amilisp`, dropped
beside the 0.11 snapshot's `lib/` (`--no-image`, 8 MB heap); Clamacs
loaded from that tree's FASLs in 3.7 s.  The phase timers tick at 20 ms
on this machine, so a 20 ms difference is one quantum.  The classic
rows are the same tree's classic, i.e. *with* the mark-loop pass — the
2026-10-08 Vampire rows above are the pre-pass baseline.

| | mark | sweep | compact | wall / cycle | free-list steps / cycle |
|---|--:|--:|--:|--:|--:|
| **classic** bare explicit, 2nd–4th (433 K live) | 20–40 ms | 0–20 ms | — | 20–40 ms | 0 |
| **immix** bare explicit, 2nd–4th (433 K live) | 20–40 | 0–20 | — | 20–40 | — |
| **classic** bare churn, cycle 1 (bump phase) | 60 | 200 | — | 2.9 s | 5.7 K |
| **classic** bare churn, cycles 2–6 (free-list phase) | 60–80 | 180–200 | — | **8.2–9.6 s** | 331 K |
| **immix** bare churn, cycles 1–6 | 80–100 | 0–20 | — | **2.4–2.8 s** | — |
| **classic** bare `(ext:gc-compact)` | — | — | 300 | | |
| **immix** bare `(ext:gc-compact)` | — | — | **1260** | | |
| **classic** loaded, 1st gc after the FASL load (bump 2.4 M) | 60 | **2660** | — | 2.7 s | 0 |
| **immix** loaded, 1st gc after the FASL load (bump 2.4 M) | 80 | **2760** | — | 2.8 s | — |
| **classic** loaded explicit, 2nd–4th (1.8 M live) | 80 | 20–40 | — | 120 ms | 0 |
| **immix** loaded explicit, 2nd–4th (1.8 M live) | 80 | 0 | — | **80 ms** | — |
| **classic** loaded churn, cycle 1 (bump phase) | 120 | 180 | — | 2.3 s | 3.8 K |
| **classic** loaded churn, cycles 2–6 (free-list phase) | 120–140 | 160 | — | **4.6–7.7 s** | 243–254 K |
| **immix** loaded churn, cycles 1–6 | 120–140 | 0–20 | — | **1.9–2.2 s** | — |
| **classic** loaded `(ext:gc-compact)` | — | — | 680 | | |
| **immix** loaded `(ext:gc-compact)` | — | — | **1500** | | |
| **classic** loaded explicit after compaction | 60 | 20 | — | 80–100 ms | 0 |
| **immix** loaded explicit after compaction | 60–80 | 0–20 | — | 80–100 ms | — |

Churn totals:

| | steps | wall | collector | mutator µs/step |
|---|--:|--:|--:|--:|
| classic bare | 37,400 | 47.5 s | 1.5 s (3.2 %) | **1,229** |
| immix bare | 37,400 | 14.7 s | 0.5 s (3.7 %) | **379** |
| classic loaded | 28,000 | 30.9 s | 1.7 s (5.6 %) | **1,041** |
| immix loaded | 29,200 | 12.1 s | 0.8 s (7.0 %) | **384** |

The mark alone (`trunk/gc-mark-bench.lisp`, 640 units = 2.1 MB live,
mean of five explicit cycles): classic **64 ms**, Immix **96 ms** per
cycle (30.7 and 45.3 ms per live MB).

What the rows say:

1. **The same 3× on the mutator as in FS-UAE** (1,229 → 379 µs/step
   bare, 1,041 → 384 loaded) and the same flat cycle: 2.4–2.8 s against
   2.9 s once and 8.2–9.6 s thereafter bare, 1.9–2.2 s against 2.3 then
   4.6–7.7 s loaded.  The whole bare churn is 14.7 s instead of 47.5.
2. **The sweep is gone from the pause**: 0–20 ms against 160–200 on the
   8 MB heap.  The quiet-heap pause with the editor resident is 80 ms
   against 120, all mark.
3. **The mark-loop pass shows on the hardware too**: the classic mark
   under the loaded churn is 120–140 ms against 240–260 on 2026-10-08
   (same machine, same script, pre-pass binary), bare 60–80 against
   140–160.  The 80 ms per live MB of Phase 0 is now 31 (classic) and
   45 (Immix) on the mark bench.
4. **The Immix mark costs 50 % more than classic here** (96 against
   64 ms on 2.1 MB), the same ratio as the cycle-exact 68020 (2,572
   against 1,656).  Under churn the 20 ms quantum hides it (80–100
   against 60–80 bare; 120–140 both loaded).  It is the design cost the
   trims left — see the mark-loop pass above.
5. **Compaction is 4× slower** (1,260 / 1,500 ms against 300 / 680), as
   on the emulator and for the same reason; no churn run asked for one.
6. **The first collection after a FASL load is the same 2.7–2.8 s on
   both** — the finalization storm (§Vampire 4 of Phase 0) is off-heap
   and no collector's business.
7. **The Amiga suite under Immix on the box**: the first run froze the
   machine (hard, with a tone) at the AHI playback checks of
   `tests/amiga/test-ahi.lisp`, after 4,243 passes — the first time
   those checks had run on this machine, which also runs under the
   lost-store CPU condition the startup self-test flags (core 10760), so
   the freeze is the box's, not the collector's, until a classic run
   says otherwise.  The re-run with the AHI file skipped passed
   **5,181 of 5,188** in 18 minutes; the seven non-passes are the
   deployment and the machine, not the collector: `lib/boot.lisp` is not
   in the release tree (two file checks), `tools/docs/` is not deployed
   (md2guide), no CA bundle for TLS, the launcher passes no `--`
   arguments, a screenshot-matches-screen check on a 1280×720 true-colour
   Workbench, and a `floor` double remainder of 3.8e-13 on the FPGA FPU.
   The classic binary on the same tree and runner: **5,180 of 5,188** in
   16 minutes, the same seven plus the `audio-short-sample-completes`
   flake FS-UAE shows too.

## MorphOS box (PPC, MorphOS 3.20, 1.7 GB)

The checkout on the box at the branch's tree, `make -f Makefile.mos` and
`make -f Makefile.mos GC=immix` (the first Immix binary this machine has
run); same script, 8 MB heap, Clamacs loaded from the submodule's
sources (the Immix run compiled them into the FASL cache, 1.7 s; the
classic run after it loaded the cache in 0.2 s).  This machine is
20–30× the Vampire, so the 20 ms timer quantum swallows every phase on
a quiet heap: the per-phase columns read 0 or 20 and are not shown; the
wall per cycle and the mutator cost are what the box can measure.

| | wall / cycle | free-list steps / cycle |
|---|--:|--:|
| **classic** bare churn, cycle 1 (bump phase) | 60 ms | 5.6 K |
| **classic** bare churn, cycles 2–6 (free-list phase) | **200–440 ms** | 331 K |
| **immix** bare churn, cycles 1–6 | **40–60 ms** | — |
| **classic** loaded churn, cycle 1 (bump phase) | 60 ms | 7.9 K |
| **classic** loaded churn, cycles 2–6 (free-list phase) | **140–280 ms** | 254 K |
| **immix** loaded churn, cycles 1–6 | **40–60 ms** | — |
| `(ext:gc-compact)`, bare / loaded | classic 20 / 20, immix 20 / 40 | |

Churn totals:

| | steps | wall | mutator µs/step |
|---|--:|--:|--:|
| classic bare | 37,400 | 1.48 s | **38.0** |
| immix bare | 37,400 | 0.30 s | **7.5** |
| classic loaded | 28,800 | 1.04 s | **34.0** |
| immix loaded | 24,400 | 0.34 s | **12.3** |

The mark alone: on 2.1 MB live both binaries sit under the quantum
(4 / 8 ms per cycle from one tick in five).  On **24.7 MB live**
(`*mark-bench-mb*` 24, 128 MB heap): classic **28 ms**, Immix **36 ms** per
cycle — 1.2 and 1.5 ms per live MB, the same direction as the 68k with
a quantum's worth of uncertainty on the ratio.

What the rows say:

1. **The allocation tax is the biggest of the three machines here**:
   5.1× on the bare mutator (38.0 → 7.5 µs/step), 2.8× loaded, and the
   free-list phase is 3–7× the bump phase per cycle (200–440 ms against
   60) where Immix stays at 40–60 ms throughout.  On a fast CPU the
   free-list probe is a larger share of a cheap allocation, so the
   relative win grows.
2. **The collector itself is below the quantum on this box**: a 2 MB
   mark is single-digit milliseconds, a 25 MB one 30 ms.  The pause a
   MorphOS user can notice is the free-list allocation between cycles,
   not the cycle — the one thing Immix removes.
3. Compaction, the rare path, is 20–40 ms either way at this heap size.
4. **The Amiga suite on the box, both binaries**: 4625 of 4629 under
   Immix in 115 s, 4625 of 4629 under classic in 110 s, the same four
   non-passes in both — the three MorphOS-only `gfx-sprite-*` checks
   known since 2026-08 and `*command-line-args*`, which the launch
   script did not feed.  The first Immix run this machine has had.
