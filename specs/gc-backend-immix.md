# GC backend seam + Immix-style mark-region collector

Status: IN PROGRESS.  Phase 0 measured 2026-10-08:
`docs/gc-phase0-results.md` (gate passed for the churn and allocation
cases; the idle-heap pause is the mark and needs its own work).  Phase 1
(the seam) landed in five steps 2026-10-08 … 2026-10-13, classic only.
Phase 2 (the Immix backend on the host, `make host GC=immix`) landed
2026-10-13 — see "As landed" under Part 2 for where it departs from the
design text below.  Phase 3 (the m68k/MorphOS builds) landed 2026-10-13:
Immix is the m68k default — see "As landed (Phase 3)" for the numbers,
the one multi-threading bug it surfaced, and what stays open.
Companion: `specs/generational-gc.md` (the host collector this leaves alone).

## Why

The host runs the generational collector and is done as far as throughput
goes: 0.8–3.8 % of wall in the sento cells, worst pause 8.6 ms
(`docs/benchmarks.md`, 2026-07-15 and the 0.11 results).  The m68k and
MorphOS builds run the **classic** mark-sweep-compact collector, and there
the cost is visible: a warm full collection in Clamacs on the Vampire is
~65 ms, the first one ~355 ms (`docs/benchmarks.md`, "The spike"), and a
14 MHz 68020 is an order of magnitude behind the Vampire.  That is an
editor stall, not a throughput number.

Two of the three phases of a classic cycle are proportional to the
*arena*, not to the live set:

- **sweep** walks every header in `[CL_ALIGN, bump)`, coalesces dead runs
  and rebuilds the free list (`gc_sweep`, gc_classic.c);
- **allocation** between cycles probes that free list
  (`alloc_from_free_list`, bounded by `CL_FREELIST_PROBE_LIMIT`), and when
  the probe keeps missing we fall into the "sweep-forever" regime that
  `GC_SWEEPS_BEFORE_COMPACT` exists to escape.

A generational scheme is the textbook answer but needs a write barrier,
and on a machine without page protection that means auditing every C
store site — the thing the generational spec explicitly refused (and the
Tier-4 audit measured ~85 bugs in exactly that class of code).  An
Immix-style mark-region collector needs **no barrier**: it is a tracing
collector with the same mark phase we have, whose sweep is proportional
to a small line map instead of the arena, whose allocation is always a
bump into holes, and whose compaction is opportunistic instead of
whole-arena.

Doing this at all requires a seam `mem.c` (6,893 lines) does not have:
allocator, two collectors, tracing, forwarding, root enumeration, rehash,
finalization, JIT pinning and verification are one translation unit with
shared statics.  So the work is two things, and the first is the bigger
one:

1. a **backend seam** — split `mem.c` so a collector is one file behind a
   small interface, with the object model, tracing, root enumeration and
   the sliding compactor shared;
2. an **Immix-style backend** on that seam, for the m68k/MorphOS builds
   (and as a host build for development, A/B and gc-stress).

## What stays fixed (the contract any backend honours)

- `CL_Obj` is a 32-bit arena-relative offset into **one contiguous
  arena**.  `CL_PTR_TO_OBJ`/`CL_OBJ_TO_PTR` are the only translation
  (types.h), the JIT bakes `cl_arena_base` into native code
  (`jit_a64.c`), `vm.c` and `jit/runtime*.c` bounds-check against
  `cl_heap.arena_size`, the image is the arena's bytes (`image.c`).
- The object header: 8-bit type, 1 mark bit, 23-bit size
  (`CL_HDR_*`, types.h:158).  There is no spare bit.
- **Linear walkability**: every byte of `[CL_ALIGN, bump)` is covered by
  an object or a formatted `CL_FreeBlock`, so a header chain from
  `CL_ALIGN` reaches every object.  Today this is assumed by the sweep,
  the compactor's three passes, the mark-stack-overflow rescan, the
  block-start index behind the conservative JIT scan, the image writer
  (three walks) and loader, `cl_mem_adopt_image_finish`, and the
  `DEBUG_GC` verifiers.  The Immix backend keeps it, with one explicit
  amendment (§"Walkability under holes").
- Roots: per-thread VM/dyn/nlx/handler/restart stacks (`gc_mark_thread_
  roots`), ~15 global tables, the registered-root array, the FASL
  reader/writer/MLF hooks, `cl_compiler_gc_mark`, plus the conservative
  scan of JIT native stacks that **pins** what it finds.
- Address-sensitive EQ/EQL hash tables and TLV tables are rehashed after
  any move; dead streams/locks/condvars/threads/bytecode/foreign pointers
  are finalized (`gc_finalize_dead`).
- Stop-the-world coordination lives in `thread.c` and is backend-neutral.
- The external API stays: `cl_alloc`, `cl_gc`, `cl_gc_compact`,
  `cl_gc_compact_if_pending`, `cl_gc_reclaim_young`, `cl_gc_if_stale`,
  `cl_cursor_retire`, `cl_mem_adopt_image_*`, the stats functions and the
  `ext:%gc-*` builtins that expose them.
- `CL_FASL_VERSION` and `CL_IMAGE_VERSION` are **not** touched by any of
  this: nothing changes on disk.  An image stays a dense arena (the
  writer compacts first, as today), and the loader normalises marks.

## Part 1 — the backend seam

### Selection: compile time, one backend per binary

`make host GC=immix` / `make -f Makefile.cross amiga GC=classic`; the
default is `GC=classic` on the host and MorphOS and `GC=immix` on the
m68k cross build (Phase 3 decided it on the numbers).  Rationale:
the backend's hooks sit on the per-object mark path (line marking) and on
the allocation fast path, where a function pointer or a runtime flag is
measurable on the 68020 and `vm.o`-sensitive on host; and the Amiga
binary must not carry two collectors.  Host A/B is two binaries, like the
`FPU=1` precedent.  The classic backend keeps its runtime
`CLAMIGA_GENGC=0/1` switch: gen mode is a mode *of* classic (sticky marks
on the same compactor), not a third backend.

### File split (mechanical first, then the three unifications)

```
src/core/mem.c          cl_alloc fast path, cl_mem_init/shutdown, cl_heap,
                        cl_make_* constructors, root stack, registered roots
src/core/gc_backend.h   the interface below + the GCB_* hot macros, selected
                        by CL_GC_CLASSIC / CL_GC_IMMIX
src/core/gc_internal.h  formerly-static state the split makes shared
                        (mark stack, pins, hdr index, phase timers)
src/core/gc_trace.c     mark stack, gc_mark_obj/push/children (X-macro),
                        root enumeration, overflow rescan, finalize hooks
src/core/gc_compact.c   forwarding bitmap/page table, pins/gaps, gc_forward,
                        slide, reference update, rehash  (SHARED defrag)
src/core/gc_jitscan.c   conservative JIT-stack scan, block-start index,
                        free-list snapshot
src/core/gc_verify.c    DEBUG_GC verifiers, heap-verify report (hv_*), audits
src/core/gc_classic.c   bump + free-list refill, gc_sweep, sweep-forever
                        policy, cl_gc, the whole CL_GENGC section
src/core/gc_immix.c     Part 2
```

Step 1 is a pure move (`git diff -M` clean, no logic change) and is gated
on being benchmark-neutral: `vm.*` rows of `trunk/bench-opt.lisp`, the
sento pinned/tell + pinned/ask pair (interleaved, see memory), the Amiga
suite.  `vm.o` stays non-LTO; the split files are compiled like `mem.c`
is today.

*Done 2026-10-08.*  Every function body is byte-identical to the one-file
`mem.c`; the only edits are `static` dropped from the 70-odd symbols that
cross a file boundary (declared in `gc_internal.h`) and the forward
declarations the header makes redundant.  Two deliberate deviations from
the table above, both for step 4: `alloc_from_bump` / `alloc_from_free_list`
stayed in `mem.c` (they are only called from `cl_alloc` and the TLAB
refill, and the m68k build has no LTO to re-inline a cross-file call —
Unification C turns them into `gcb_alloc_slow` anyway), and the classic
policy counters (`gc_last_compact_cycle`, `gc_sweeps_since_compact`) stay
in `mem.c` next to the `cl_alloc` triggers that read them until step 5
lifts the policy into the backend.  m68k `.text` +208 bytes (0.03 %);
`cl_alloc` 237→236 instructions.

### Unification A — one root walk, slot-visitor form

Today the roots are enumerated twice: `gc_mark_thread_roots` +
`gc_mark` (gc_trace.c, value form, `gc_mark_obj(x)`) and
`gc_update_thread_roots` + `gc_update_registered_roots` +
`gc_update_shared_roots` (gc_compact.c, slot form, `*slot =
gc_forward(*slot)`).  The children side is already one X-macro
(`GC_WALK_OBJ_CHILDREN`, gc_internal.h) instantiated twice.  Make the roots
the same shape:

```c
typedef void (*gc_slot_fn)(CL_Obj *slot);
void gc_visit_roots(gc_slot_fn visit);          /* every root slot once */
void gc_visit_thread_roots(CL_Thread *t, gc_slot_fn visit);
```

Mark instantiates it with "mark the value", the compactor with "forward
the slot in place".  The fields deliberately *not* marked today
(`nlx_stack[i].mv_values`, `pre_call_mv_values` — stale words that are
not object starts) are then by construction also not forwarded, which
closes a bug class rather than opening one.  The registered-root
de-duplication (`root_slot_independently_forwarded`, `gc_root_slot_buf`)
stays inside the forwarding visitor: forwarding an aliased slot twice is
wrong, marking it twice is harmless.  Part 2's optional evacuation needs
exactly this form (a slot is rewritten when its target has moved), which
is why it is in Part 1.

*Done 2026-10-08.*  `gc_visit_thread_roots` / `gc_visit_registered_roots`
/ `gc_visit_shared_roots` / `gc_visit_roots` in gc_trace.c (the walk keeps
the `GC_DBG_SRC` provenance for DEBUG_GC badmark reports); `gc_mark`
instantiates them with `gc_mark_slot`, `gc_update_all_roots` with
`gc_update_slot`.  The module-side holders became visit walkers too
(`cl_compiler_gc_visit_thread`, `cl_vm_gc_visit_extra_thread`,
`cl_fasl_gc_visit_readers/writers/mlf`; the MLF index invalidation moved
to the compactor as `cl_fasl_mlf_gc_relocated`).  Two differences the
unification settled in favour of the forwarding side: the compiler env
chain is walked once per compiler (the mark side used to re-walk every
ancestor env per descendant), and readtable slots are visited
unconditionally (both visitors are no-ops on NIL).  The registered-root
dedup no longer keeps its own list of "independently forwarded" ranges —
it runs the thread + shared walks once more with a probe visitor that flags
every registered address they visit, so the alias set *is* the walk and
cannot drift from it; `root_slot_independently_forwarded` (the audit and
the no-memory fallback) probes the same way.  That also made a protect of
a thread scalar or a table global (`CL_GC_PROTECT(CT->pending_tag)`,
`&macro_table`) safe, which the range list never covered.
`tests/test_gc_roots.c` pins the contract (every slot once, the stale
words never, the alias end-to-end through a real compaction — the
thread-slot variant fails on the step-1 tree, the slot coming back as an
unrelated object).

### Unification B — one arena iterator

```c
typedef struct { uint32_t off; uint32_t end; /* backend cursor */ } GcWalk;
void   gc_walk_begin(GcWalk *w);            /* CL_ALIGN .. cl_heap.bump */
int    gc_walk_next(GcWalk *w, uint32_t *off, uint32_t *size, uint8_t *type);
```

replaces the ~12 hand-written `while (ptr < end) { size = CL_HDR_SIZE(ptr);
if (!size) break; ...; ptr += size; }` loops in `mem.c` and `image.c`.
Classic: the bare chain.  Immix: the chain with hole clipping (§below).
The "size == 0 → stop" guard becomes a loud `cl_gc_verify_heap`-style
report in debug builds instead of a silent truncation.

*Done 2026-10-08.*  `GcWalk` / `gc_walk_begin` / `gc_walk_next` /
`gc_walk_seek` / `gc_walk_complete` live in `mem.h` as `static inline`
(image.c and the tests use them too, and the passes they serve are the
O(arena) loops of a build without LTO; the struct's address never escapes
to an out-of-line call, so the cursor stays in registers exactly like the
old `ptr`/`end` locals).  Eighteen loops went through it: forwarding,
reference update, slide, rehash, the gen-mode mark clear and the DEBUG_GC
hash-chain audit (gc_compact.c); the sweep (gc_classic.c — it re-seeks
past the block it coalesced); the mark-overflow rescan (gc_trace.c); the
post-mark and post-sweep verifiers (gc_verify.c); the JIT scan's
no-index fallback and its index cross-check (gc_jitscan.c); the image
adopter's two normalisations and the shutdown off-heap release (mem.c);
the save preconditions, the bytecode census, the blob writer and the
relink walk (image.c).  The cursor advances before the body sees the
block, so a body may rewrite the header it was handed.  A size-0 header
ends the walk with `gc_walk_complete()` false — a release build truncates
the pass as before, DEBUG_GC / DEBUG_GC_STRESS builds also reject an
unaligned size, a block crossing the walk end and an impossible type tag,
and name the header and abort (`gc_walk_broken`, mem.c).  Deliberately
*not* on the iterator: `cl_gc_verify_heap`'s pass 1 (it *is* the
chain check and must report, not abort), `cl_gc_audit_hdr_index` and the
gen-mode `gen_covering_object` / `gen_walk_dirty_objects` (page-bounded
walks from an index entry, with their own desync recovery), and the
sweep's inner coalescing loop (it reads the raw neighbours of the block
it is merging).  `tests/test_gc_walk.c`: contiguity from `CL_ALIGN` to
the bump front, free blocks and a bare sub-header gap as blocks, the
dense chain after a compaction, seek, and the broken-header stop.

### Unification C — one allocation cursor

The TLAB (`t->tlab_cur/tlab_end`, host multi-threaded only) and the
single-threaded `alloc_from_bump` are the same idea: a per-thread
`[cur, limit)` the fast path bumps, refilled by the backend.  `cl_alloc`
becomes

```
fast:  size = align; cut from t->alloc_cur if it fits   (no lock)
slow:  gcb_alloc_slow(t, type, size)                    (backend; may GC)
```

and both backends implement the refill: classic hands out a bump chunk
or a free-list block exactly as `tlab_refill_cut` does now; Immix hands
out the next hole.  Classic single-threaded keeps its current behaviour
(cursor = the whole remaining bump region) so Part 1 is neutral on the
Amiga too.  The TLAB invariants (remainder always a formatted
`CL_FreeBlock`; no remainder under `CL_MIN_ALLOC_SIZE`; all cursors
dropped under STW) become cursor invariants and hold for every backend.

*Done 2026-10-13.*  `cl_alloc` is the cursor cut and nothing else:
`cursor_cut_front` / `cursor_cut_chunk` (`CL_ALWAYS_INLINE`, mem.c) cut
the regime's cursor lock-free, and `gcb_alloc_slow` (mem.c, `CL_NOINLINE`,
the classic backend's half of the seam until step 5 moves it) refills
the chunk and cuts, or allocates directly under the unchanged classic
policy, and owns the accounting.  The classic backend's two cursors,
chosen by `multi` (which `cl_alloc` computes anyway): single-threaded
the **live bump front** `[cl_heap.bump, arena_size)` — a cut is the bump
allocation, exact for every reader with nothing to flush, and on m68k
`-Os` it is now inline where the old `alloc_from_bump` was being
*called* on every cons; multi-threaded the **chunk** of the former TLAB
(`alloc_cur` / `alloc_lim` / `alloc_consed` in thread.h, all builds;
`CL_TLAB` carves them, remainder a formatted hole, consed bytes
deferred).  "Cursor = the whole remaining bump region" was first
implemented literally — a per-thread snapshot `[bump, arena_size)` with
a kind flag — and measured: the thread-pointer load plus the dependent
cursor loads cost +2 ms on `alloc.cons-churn` (22–23 → 24–25, every
run) and +2 ms on `alloc.mixed-churn` on the host, and the snapshot
needs a kind check on every cut to survive a thread-count change (a
front snapshot cut after a second thread registered hands out bytes a
peer's chunk is carving; a chunk cut single-threaded would move the
front backwards).  The live front has neither cost nor hazard: a chunk
left over after the count dropped to one is simply never cut again
and is dropped by the next collection or slow-path entry.  The
parent-registers-newborn order (builtins_thread.c) means the thread
flipping the count to two is the sole thread; a thread registering
itself concurrently with another thread's allocations was never
allowed by the unlocked bump allocator either.  `gc_cursor_drop_all`
(was `gc_tlab_reset_all`, now unconditional) runs at every collection's
entry, on heap re-init and in `cl_mem_adopt_image_begin`;
`cl_cursor_retire` (was `cl_tlab_retire`) at thread exit.
`tests/test_gc_cursor.c`: front == bump after every cut with exact
accounting, the arena tail absorbed so the front lands on `arena_size`,
sweep / compaction / retire / image adoption leave the front consistent,
both regime transitions with a registered newborn (the leftover chunk
never cut, a walkable hole throughout, its accounting flushed), chunk
accounting flushed by a collection.  The Immix backend has one cursor
shape in both regimes — a hole, private by construction — so for it the
single-threaded cut is the chunk cut with the main thread's fields.

### The interface

```c
/* gc_backend.h — implemented once per backend, selected at build time */
int   gcb_init(uint32_t arena_size);      /* side tables; 0 = cannot */
void  gcb_shutdown(void);                 /* hand every byte back (memleak) */
void  gcb_reset(void);                    /* cl_mem_init re-init */
void *gcb_alloc_slow(CL_Thread *t, uint8_t type, uint32_t size);
void  gcb_collect(int why);               /* GCB_WHY_ALLOC / EXPLICIT /
                                             RECLAIM_YOUNG / COMPACT */
void  gcb_adopt_image(void);              /* rebuild tables from a dense
                                             [CL_ALIGN, bump) */
void  gcb_before_image_save(void);        /* make the arena dense+normal */
int   gcb_walk_clip(uint32_t off, uint32_t *size);   /* iterator hook */
void  gcb_stats(...);                     /* feeds ext:%gc-time-stats */

/* hot hooks, macros so the classic build pays nothing */
GCB_ON_MARK(off, size)        /* immix: mark lines; classic: (void)0 */
GCB_HDR_MARKED(ptr)           /* sense-aware (immix) or CL_HDR_MARKED */
GCB_HDR_SET_MARK(ptr) / GCB_HDR_CLR_MARK(ptr)
GCB_ON_ALLOC_FINALIZABLE(off) /* immix + gen: side-list note */
```

The shared compactor (`gc_compact.c`) is a *facility* every backend may
call: classic uses it as today; Immix uses it as its rare full
defragmentation and before an image save.  It is written against the
iterator and the visitor, so it needs no backend knowledge beyond
`gcb_walk_clip`.

*Done 2026-10-13 (step 5).*  `src/core/gc_backend.h` is the seam, included
by `gc_internal.h` and by any runtime file that needs a hook (image.c);
`mem.h` selects the backend (`CL_GC_CLASSIC` / `CL_GC_IMMIX`, exclusive,
classic the default when neither is defined — so the cross and MorphOS
makefiles need no change until Phase 3) and makes `CL_GENGC` a mode *of*
classic (`#if defined(CL_GC_CLASSIC) && …`).  The interface as landed
differs from the table above where the code said so:

- `gcb_arena_alloc(size)` / `gcb_arena_free(arena, size)` — the backend
  places the arena, because gen mode needs page-aligned, write-watched
  pages; `gcb_init()` (after `cl_heap` describes the arena; self-resetting,
  the stale-static rule) and `gcb_shutdown()` (before the off-heap release
  walk, so it must leave the arena readable) bracket the side tables.
- `gcb_alloc_fast(sizep, multi, &t)` (`CL_ALWAYS_INLINE`) joins
  `gcb_alloc_slow(t, sizep, multi)`: the cursor *shape* is the backend's
  (classic: front/chunk, Immix: one hole-shaped chunk in both regimes), so
  the fast path lives in the header next to the cuts it instantiates, and
  the chunk refill in `gc_classic.c` uses the same inlines.  `mem.c` keeps
  the per-thread chunk fields' drop/retire paths (`gc_cursor_drop`,
  `gc_cursor_drop_all`, `cl_cursor_retire`) — shared by construction.
- `gcb_collect(why)` takes only `GCB_WHY_EXPLICIT` (`cl_gc`) and
  `GCB_WHY_RECLAIM_YOUNG` (`cl_gc_reclaim_young`); allocation-driven
  collection is inside `gcb_alloc_slow`, and an explicit compaction is the
  shared `cl_gc_compact`.  `cl_gc` / `cl_gc_reclaim_young` are
  backend-neutral wrappers in `mem.c`; `gc_stop_world_timed` moved there
  too (every backend's cycle enters through it).
- The compactor got three hooks instead of a `gcb_before_image_save`-only
  contract: `gcb_compact_begin()` (gen: unprotect + clear the sticky
  marks), `gcb_compact_fallback(outcome)` (the non-moving cycle when the
  pin or forwarding table cannot be had — classic: `gc_sweep` + gen
  stand-down + counter reset) and `gcb_compact_done()` (classic: the
  sweep-forever counter; gen: new old space and dirty era).  What stays
  `#ifdef CL_GENGC` inside `gc_compact.c` / `gc_trace.c` — `gc_forward`'s
  survivor-list leg, the crossing-map notes in the forwarding and slide
  passes — is gen's dense-old-space bookkeeping, compiled out whenever
  `CL_GC_CLASSIC` is not defined.  `gc_compact_pending` and
  `gc_last_compact_cycle` are the compactor's own (safe-point flag and
  once-per-cycle gate); `gc_sweeps_since_compact` is classic policy.
- `gcb_adopt_image_begin()` / `gcb_adopt_image_finish()` (two phases, as
  `cl_mem_adopt_image_*` always had), `gcb_before_image_save()` (classic:
  full GC then compaction, as the writer did inline), `gcb_stats_report()`.
- Hot macros: `GCB_ON_MARK(off, ptr)` is called in `gc_mark_obj` (classic:
  the gen survivor note, `(void)0` without gen); `GCB_ON_ALLOC_FINALIZABLE`
  in `cl_alloc` (classic: `gen_fin_note`); `GCB_HDR_MARKED/SET/CLR` are
  defined but the raw `CL_HDR_*` call sites are not converted yet — that
  conversion, the grep lint, and the iterator's `GCB_WALK_CLIP` hook land
  with the backend that needs them (Part 2), so step 5 adds no token to
  `gc_walk_next`.
- `cl_gengc_enabled()` is declared unconditionally (0 without `CL_GENGC`),
  which also un-breaks the `CL_NO_GENGC` build of `test_mem` and
  `test_alloc_freelist_perf`; `cl_gc_if_stale` stays public under
  `CL_GC_CLASSIC` for `test_gc_threaded`'s dedup tests.
- `make host GC=classic|immix`: `GC_DEF` goes into both `CFLAGS_HOST` and
  `CFLAGS_TEST`, `GC_SRC` picks the backend file, `GC=immix` builds into
  `build/host-immix` (the two-binary A/B, like `FPU=1`) and currently
  stops at the header's `#error` because `gc_immix.c` does not exist.
- Tagged `#ifdef CL_GC_CLASSIC` (trivial pass otherwise):
  `test_gc_fwd_bitmap`, `test_gc_hdr_index`, `test_alloc_sweep_escape`,
  `test_gc_root_dedup`, `test_alloc_freelist_perf`, `test_gc_cursor`;
  `test_gengc*` were already gated on `CL_GENGC`.  Everything else must
  pass under either backend.  `test_gc_roots_visit.c` from the list below
  is `tests/test_gc_roots.c` (landed with step 2).

### Tests for Part 1

- All existing suites under `GC=classic`, unchanged results; gc-stress,
  memleak, `test-amiga`, `test-amiga FPU=1`, MorphOS native build.
- Classic-mechanics tests gain `#ifdef CL_GC_CLASSIC`:
  `test_gc_fwd_bitmap`, `test_gc_hdr_index`, `test_alloc_sweep_escape`,
  `test_gc_root_dedup`, `test_gengc*`, `test_gengc_watch`.  Everything
  else must pass under either backend.
- New `tests/test_gc_roots_visit.c`: the mark visitor and the update
  visitor see the same slot set (count + offsets) for a thread with every
  stack populated — the property Unification A exists to guarantee.
- New `tests/test_gc_walk.c`: the iterator against a hand-built arena
  (objects, free blocks, a sub-header gap, the bump end).

## Part 2 — the Immix-style backend (`gc_immix.c`)

### Layout

The arena is viewed as **blocks** of 32 KB made of **lines** of 128 B
(both build-time constants, `CL_GC_BLOCK_SHIFT`/`CL_GC_LINE_SHIFT`; 256 B
lines halve the map on a tight heap).  Side tables, allocated with
`platform_alloc` at init and released at shutdown:

| table | size (8 MB heap) | content |
|-------|-----------------:|---------|
| line map | 64 KB (1 B/line) | bit 7: *hole-start* flag; bits 0–6: mark epoch |
| block table | 256 B | state: FREE / RECYCLABLE / FULL / SPAN, free-line count |
| finalizable list | 4 B/entry | offsets of live finalizable objects (gen's `gen_fin`) |
| block-start index | 8 KB | unchanged from classic (JIT scan) |

Line marks are **exact**: `GCB_ON_MARK(off, size)` writes the current
epoch into every line of `[off, off+size)` — we know the size from the
header, so Immix's "conservatively mark the next line" shortcut is not
used and not needed.  A line is marked ⇔ `(byte & 0x7F) == epoch`; the
epoch advances mod 128 per cycle and the map is normalised (stale epochs
zeroed) every 127th cycle so an unmarked line can never alias to marked.

Objects may span lines and blocks (large objects do by design).  The
walk is the one global chain `[CL_ALIGN, bump)`, not per block; the block
table exists only for the allocator.

### Allocation

- **Small** (≤ line): per-thread cursor into the current hole; a hole is a
  maximal run of unmarked lines inside one block.  On exhaustion take the
  next hole of the block, then the next RECYCLABLE block, then a FREE
  block, then collect.
- **Medium** (line < size ≤ block/4): same cursor; a hole that is too
  small is skipped; after a bounded number of skips the medium cursor
  takes a FREE block of its own (Immix's overflow allocator).
- **Large** (> block/4): a run of FREE blocks found first-fit in the block
  table, placed at the run's start; the tail of the last block becomes a
  formatted hole the small cursor may use, so a 9 KB object does not waste
  23 KB.  Blocks inside the run are SPAN.
- Every hole is **formatted as a `CL_FreeBlock`** when claimed (as TLAB
  chunks are today), objects are cut from its front, and the block-start
  index is noted — the walk through a hole is exact by construction.
- Multi-threaded (host): block/hole acquisition under `alloc_mutex`,
  cutting lock-free, as the chunk cursor today.  Single-threaded (Amiga):
  the same code with the main thread's cursor — one kind in both regimes
  (the classic backend's front/chunk distinction, Unification C, exists
  only because its single-threaded cursor is the shared bump front).

The free list and its probe are gone; `cl_heap.freelist_steps` reads 0.
`cl_heap.bump` keeps its meaning as the walk bound: the end of the highest
block ever used (monotonic between compactions, reset by one).

### Walkability under holes

The one place the chain is not exact: a dead object that starts in a
marked line (shared with a live object) and extends into unmarked lines
that are later claimed as a hole.  Its header still claims a size that
reaches past the hole start, where fresh objects now live.  Rule, applied
by `gcb_walk_clip` inside the iterator:

> If the object at `off` extends past a line whose **hole-start** flag
> is set, the object is a dead spanner: clip it at that line and continue
> from there.

The flag is set on the first line of every hole the allocator claims and
cleared on every *interior* line of that hole (a stale flag inside a new,
larger hole would otherwise clip a live object allocated across it); the
compactor clears all flags when it rebuilds a dense arena.  A live object
can never extend past a hole-start line: all its lines are marked, so no
hole begins inside it — which is exactly why line marks must be exact.
The block-start index keeps its invariant unchanged (a noted header stays
a header until a slide; the walk *from* a noted header applies the rule).

### Marks without a clearing pass: mark-bit sense

Clearing header mark bits is the O(arena) walk we are removing, there is
no spare header bit, and a side bitmap costs 3 % of the heap plus a
per-cycle `memset` (~30 ms on a 68020 at 8 MB).  Instead the meaning of
the bit flips each cycle: marked ⇔ `bit == gc_mark_sense`; objects are
born with `bit == !sense`; a completed cycle flips `sense`, so every
survivor reads unmarked for the next trace at zero cost.

Consequence, stated as the invariant every consumer is checked against:

> An object that was live at cycle N reads **unmarked** at the start of
> cycle N+1.  An object that was dead at cycle N (and not yet overwritten)
> reads *stale-marked* at N+1.

Mark-driven side tables (srcloc invalidation, stream outbuf reclaim, weak
entries, the finalizable list) are exact under this: they process an
object as dead in the cycle it dies, and never look at it again.  The
hazard is confined to code that *discovers* objects by walking the arena
and trusts the bit:

- **Mark-stack-overflow rescan** (the growth-OOM fallback): a
  stale-marked dead object's slots are garbage; pushing them could set a
  mark bit mid-object.  Fix: the rescan pushes a child only if
  `gc_is_object_start(child)` (block-start index, ≤ one page of walking —
  this is already the OOM slow path).  A garbage slot that happens to
  name a real object start then retains that object for a cycle, which
  is harmless.
- **The shared compactor** walks the chain and would retain stale-marked
  garbage.  It therefore **normalises first**: one clearing walk (what
  `cl_gc_compact` already does under gen mode), then marks with a fixed
  sense.  It walks the whole arena anyway; the common Immix cycle never
  does.
- **Conservative JIT scan**: a stale-marked candidate is not traced
  (correct, it is dead) and is pinned (harmless; pins only matter in the
  compactor, which rebuilds them after normalising).
- **`DEBUG_GC` verifiers and `cl_gc_verify_heap`**: run only in the
  normalised state or use `GCB_HDR_MARKED`.
- Every `CL_HDR_MARKED/SET_MARK/CLR_MARK` outside the backend (14 files
  today) moves to the `GCB_` form; a grep for the raw macros outside
  `gc_*.c` becomes a `make test` lint.

### A cycle

Stop the world (unchanged), drop all cursors, then:

1. **Mark** — the shared tracer, with `GCB_ON_MARK` marking lines.
   Roots, hooks, JIT scan and pinning exactly as today.
2. **Finalize** — walk the finalizable list: entries whose object reads
   unmarked are finalized (`gc_finalize_dead`: off-heap native code,
   stream buffers, lock/condvar handles, foreign pointers) and dropped.
   The list is registered at `cl_alloc` time for the finalizable types
   (one site), rebuilt by the image loader's walk, forwarded by the
   compactor.  Dead objects are otherwise **never touched**.
3. **Line sweep** — one pass over the line map: per block, count unmarked
   lines → FREE (all) / RECYCLABLE (some) / FULL (none); build the
   recyclable and free lists.  O(lines): 64 K bytes at 8 MB.
4. Flip the mark sense, advance the line epoch, resume the world.

Mark-driven side-table work that runs after mark today (`cl_stream_
outbuf_gc_reclaim`, `gc_srcloc_invalidate_dead`) runs after step 1 as
now.

Pause = mark + O(finalizable) + O(lines).  Nothing in the cycle is
proportional to dead space.

### Full defragmentation

The shared sliding compactor, invoked by the backend when:

- a cycle freed lines but no hole fits the pending request (fragmentation
  defeats the hole allocator), or
- free space is mostly inside RECYCLABLE blocks with few FREE blocks and
  the ratio exceeds a threshold (policy constant, tuned in Phase 3), or
- `(ext:gc-compact)`, `cl_gc_compact_if_pending`, or an image save asks.

After a slide the arena is dense: the backend re-derives the block table
(blocks below `bump` FULL, above FREE), marks the lines of `[CL_ALIGN,
bump)`, clears hole flags, resets the sense to normalised.  gc-stress
(`DEBUG_GC_STRESS`: a compaction before every allocation) therefore works
unchanged under Immix and keeps catching unprotected `CL_Obj` locals; a
second stress knob forces an Immix cycle every K allocations so the line
sweep and the clipping rule get the same treatment.

### As landed (Phase 2, 2026-10-13)

`src/core/gc_immix.c` on the seam, `tests/test_gc_immix.c` the contract,
the whole suite plus gc-stress (both knobs) and memleak under `GC=immix`.
Three departures from the text above, each a simplification the
implementation forced or allowed:

- **No block table, no hole-start flag, no iterator clip.**  The line map
  is the only allocator state: a line's byte equals the current epoch when
  it is unavailable (stamped by the trace, or *claimed* by a cursor since —
  a claim stamps the lines it takes) and is anything else when available;
  the line sweep writes 0 into every line the trace did not stamp, so an
  epoch can never alias.  A hole is a run of available lines found by
  scanning the map from a monotonic cursor position (the chunk cursor
  takes the next hole, a request that does not fit it takes the first run
  ahead that does and leaves the small hole for a later refill; a request
  larger than a chunk gets a run of exactly its size).  Large objects
  span lines and blocks without any SPAN state.
- **The chain is kept exact at the claim, not clipped in the walk.**
  A claim finds the block covering the byte before the hole and, if it
  extends into the hole, truncates it there (its header stays); finds the
  block covering the hole's end byte and, if it starts inside the hole,
  writes a dead header for its tail at the hole's end; formats the hole
  as one block; and, above the bump front, writes a dead filler from the
  old front to the hole.  Both walks are bounded by the block-start index
  (about a page of headers).  A live object never touches an available
  line, so everything rewritten is dead.  The block-start index persists
  across cycles (nothing re-notes it) and a claim re-points the entries a
  hole erases: the pages inside it, and those within one chunk length
  after it that named a header inside (the only noted blocks that get cut
  into un-noted pieces are chunk claims).  `cl_gc_audit_hdr_index` runs
  after every claim under `DEBUG_GC` and after every collection under
  `DEBUG_GC_STRESS`.
- **`CL_TYPE_DEAD`** (types.h, 0xFF, above `CL_TYPE_MAX`) tags every hole,
  cursor remainder, filler, truncated or split tail, finalized corpse and
  the compactor's sub-header gaps (and, after a compaction, its pin gaps).
  The conservative JIT scan refuses such a block outright, the finalizers
  ignore it (so a corpse is finalized exactly once), and the `DEBUG_GC`
  plausibility guards keep aborting on a precise reference to one.

The sense flip is as designed (`gc_hdr_xor`, `GCB_HDR_*` in gc_backend.h;
`tests/test_gc_mark_macros.sh` is the grep lint), with the one consumer
the audit above under-estimated: a corpse that shares a line with a live
object keeps its header for as long as the neighbour lives and reads
*unmarked* on every other cycle, so a stale JIT spill naming it would
have its garbage slots traced.  The conservative scan therefore runs
after the precise trace has drained, and the pushes of the trace it
starts — and of the overflow rescan — are validated as object starts
through the block-start index (`GCB_PUSH_REJECT`; `tests/test_jit_gc_scan.c`,
"scan_traces_a_corpse_without_marking_interiors").  The compactor
normalises every bit before its trace only when a cycle has run since the
last normalisation (`gcb_marks_exact`), so the gc-stress regime pays no
extra walk.  The finalizable list is exactly the design's; a note that
cannot be made schedules a compaction, whose slide finalizes everything.

Policy, pending Phase 3 tuning: a cycle when no hole fits; after it, the
compactor when the holes hold under half of the true free space or under
a sixteenth of the arena while the live set is under 7/8 of it, and as
the last resort before a storage error (once per cycle, gated like
classic by the forwarding-table latch).  Measured on the host (8 MB heap,
200k iterations of a 50-cons churn): 33 cycles of ~1 ms, 0 compactions.

`make test-gc-stress GC=immix` runs the stress suite with the compaction
before every allocation as before, then the regression script once more
with `CLAMIGA_GC_STRESS_IMMIX=8`: of every 8 stressed allocations one
compacts and seven run a line-sweep cycle, so the claims, the finalizers
and the sense flip get the same per-allocation treatment.  That mixed
regime also sees a class of bug the classic regime is blind to: a
`CL_Obj` C local held unprotected across an allocating call *after* the
object was allocated.  Under compaction-per-allocation such an object is
the newest thing on a dense heap and never moves; under the mixed regime
the cycles scatter fresh objects into holes below the front, and the next
compaction moves them.  Its first run found two (both fixed with it):
`(setf gethash)` returned its `value` unrooted across the bucket-vector
rehash, and `%ffi-stub-info` passed its plist in the same argument list as
a keyword intern (the argument-order lint now also knows `*intern*` and
`*_keyword` helpers).  On a `[GC-BADMARK]`, the stress build prints the
claims that formatted the offending address and the stressed-allocation
index, so `CLAMIGA_GC_STRESS_AT=<index>` names the code at the compaction.

### As landed (Phase 3, 2026-10-13)

`Makefile.cross` and `Makefile.mos` take the host's `GC=` knob.  The m68k
cross build defaults to **Immix** (`build/cross/`; `GC=classic` goes to
`build/cross-classic/`, composing with `FPU=1`/`WIDE=1` as
`cross-fpu-classic` and so on); the MorphOS build keeps classic as its
default (`GC=immix` builds to `build/morphos-immix/`) until it has run
there — the box was off.  The object dir suffix names the *non-default*
backend so `scripts/make-binary-release.sh` and every helper that knows
`build/cross` keep working; make does not track `CFLAGS`, so the switch
of a default needs a `make -f Makefile.cross clean` once.

The numbers behind the default (`docs/gc-phase0-results.md`, "Phase 3",
both binaries built the same afternoon, `verify/realamiga/run-gc-phase0.sh`):
on the emulated 68040 the churn workload's mutator goes from 936 to 293
µs/step bare and from 1,503 to 346 loaded (3.1×/4.3×), on the cycle-exact
68020 from 30.6 to 9.4 ms/step bare and 26.3 to 11.4 loaded (3.3×/2.3×);
the per-cycle wall is flat (no
free-list phase) and the sweep is 0–20 ms (040) / 200–240 ms (020) per
cycle against 120–140 / 4,800.  The mark is the Phase 0 mark, 10–40 %
*slower* under Immix on a scattered heap (the per-object line stamp, and
on the emulated 040 the locality) — the mark-loop pass remains the next
lever for the idle-editor pause, which Immix leaves where it was.  The
sliding compactor is 4–5× slower on an Immix heap (it walks every
un-coalesced corpse: 1.1–1.4 s on the 040, 38–48 s on the 020) but the
policy asked for none in any churn run; it is the explicit
`(ext:gc-compact)` and the image-save path.  Binary: +3.4 KB over
classic (895,088 vs 891,716 bytes, soft-float).  `test-amiga` (68040)
under Immix is identical to classic, both binaries rebuilt from the
final tree: fresh 5284/5285 (the one failure is the FS-UAE audio flake),
restored 5275/5275, every image / executable / stack / Workbench leg.
`test-amiga-lowend` cannot finish inside its 30-minute hard timeout for
either backend; the Immix run was at 2,712 passes and no failure when
the watchdog stopped it.  The first 68020 Immix measurement wedged in
the loaded churn (the FS-UAE log-cut-mid-line shape); the re-run with
the same binary finished and its numbers are the ones recorded.

`ROOM` ends with the backend's lines (`gcb_stats_report` takes a writer
so `WITH-OUTPUT-TO-STRING` captures them): the free list's shape and
probe count for classic, the line map, holes claimed, cycles and policy
compactions for Immix; `trunk/gc-phase0.lisp` prints them after each
section.

**One multi-threading bug, found by `test_mt_dispatch_addmethod_race`
under `make test GC=immix` on the host (crashes and "CDR: argument is not
of type LIST" in two runs of six; never under classic).**  A claim walks
the header chain on the *live* heap — under `alloc_mutex`, but a hole's
neighbour can be another thread's chunk, whose owner rewrites its headers
lock-free on every cut — and `cl_alloc` zeroed the whole block, header
word included, before storing the object's header.  A claim walking
through a peer's chunk could read a zero header, or on a weakly-ordered
host the new object header followed by the stale bytes where the
remainder header had not landed yet, follow a garbage size, and truncate
a header inside the peer's live objects.  The fix keeps a covering header
in the slot at all times (`alloc_zero` skips the header word) and orders
remainder-header-before-object-header with a barrier when threads are
live (the Immix `gcb_cursor_cut` takes `multi`; a compiler barrier on the
single-core m68k).  `tests/test_gc_threaded.c`
"claims_vs_concurrent_cuts" fails every run without either half of the
fix and passes with it; the classic chunk never had the exposure because
nothing walks its chain outside stop-the-world.  (A lesson about that
test file: a worker-initiated stop-the-world hangs if the main thread
sits in `pthread_join` outside a safe region — the existing MT tests
either never collect or let the main thread drive the collections.)

Open after Phase 3: the real hardware (Vampire 68080, the MorphOS box)
has not run Immix; the Clamacs typing-latency spike was not taken.  The
policy constants were not tuned — no churn run asked for a compaction,
so there was nothing to tune against.

### Optional Phase 4 — opportunistic evacuation

Real Immix moves live objects out of fragmented blocks *during* marking
(copy on first visit, forward slot on every visit) so the whole-arena
slide becomes rare.  With Unification A in place this is an addition to
the mark visitor, not a new walk: candidate blocks are chosen from the
previous cycle's line counts, a copied object's old header becomes a
forwarding record (type = a new `TYPE_FORWARDED`, target in the first
payload word; every object is ≥ 16 bytes), blocks holding a JIT-pinned
object are never candidates, and address-sensitive hash tables are
rehashed when anything moved (classic's rule).  It is not needed for the
Part 2 wins and is deliberately not in the critical path.

### Interactions audit (each gets a test)

| Area | Handling |
|------|----------|
| Image save | `gcb_before_image_save` = compaction + normalise; writer unchanged |
| Image load | `gcb_adopt_image`: one walk marks lines, rebuilds the finalizable list, blocks below `bump` FULL |
| `cl_mem_init` re-init | `gcb_reset` zeroes every table (stale-static bug class) |
| JIT pins / conservative scan | unchanged; index notes on hole claims; stale-marked candidates pinned-not-traced |
| TLV / srcloc / outbuf side tables | mark-driven, exact under the sense invariant; forwarded by the compactor as today |
| EQ/EQL hashtable rehash | compaction only (no moves in a plain cycle) |
| `cl_gc_reclaim_young` | an Immix cycle (it is cheap); escalates to compaction like classic's `cl_gc()` fallback |
| Mark-stack OOM | rescan with object-start validation (see above) |
| Pin-table / forwarding-table OOM | compactor degrades to non-moving as now; the backend re-derives tables from the resulting state |
| Multi-threading | block acquisition locked, cutting lock-free, STW unchanged; `test_gc_threaded` runs under `GC=immix` |
| Off-heap accounting | all tables freed in `gcb_shutdown`; `make test-memleak GC=immix` |
| Amiga heap sizes | 4 MB default: 32 KB line map; `CL_GC_LINE_SHIFT=8` option for 256 B lines |

## Phases and gates

**Phase 0 — measure before cutting.**  `(ext:%gc-time-stats)` already
splits stw/mark/sweep/compact.  Record on: FS-UAE 68040 after the test
suite, FS-UAE 68020 (`test-amiga-lowend`), the Vampire with Clamacs loaded
(`clamacs/spike/run-vamp.py` has the full-GC cells), and host
`CLAMIGA_GENGC=0` on the sento pair for reference.  Also
`cl_heap.freelist_steps` per cycle.  **Gate**: if sweep + free-list
probing is under ~30 % of collector wall on the 68020/68040, Part 2 is the
wrong lever (the cost is the live-set trace) and we stop after Part 1 —
the seam is still worth having, the Immix file is not.

**Phase 1 — the seam** (classic only, everything green and neutral):
1. mechanical split; 2. slot-visitor roots; 3. arena iterator;
4. cursor + `gcb_alloc_slow`; 5. `gc_backend.h`, `GC=` build knob, test
tagging.  Each step its own commit through the review gate; gates after
every step: `make test-plus`, `make test-gc-stress`, `make test-memleak`,
`make -f Makefile.cross test-amiga` (+ `FPU=1`), bench rows unchanged.
Estimate: 1–2 weeks.

**Phase 2 — Immix on host** (`make host GC=immix`): side tables, hole
allocator, line sweep, sense flip, clipping rule, finalizable list,
compaction hand-off; `tests/test_gc_immix.c` (line map, hole finder,
medium/large placement, block recycling, hole-start flag set/clear,
clipping across a dead spanner, sense invariant, image round-trip, re-init)
plus the whole suite, gc-stress (both knobs), threaded, memleak under
`GC=immix`.  Estimate: 1–2 weeks.

**Phase 3 — m68k/MorphOS**: `Makefile.cross`/`Makefile.mos` `GC=` knob,
`test-amiga` and `test-amiga-lowend` under Immix, the Phase 0 measurements
repeated, binary size compared (classic is compiled out, so expect
neutral), Clamacs typing-latency spike on the Vampire and the 040.  Decide
the m68k release default on those numbers.  Estimate: 1 week.

**Phase 4 — evacuation**: only if Phase 3 shows the full compactor running
often enough to matter.

## Expected impact (honest)

- **Amiga/MorphOS pause**: the sweep phase leaves the pause entirely; the
  mark phase is unchanged.  How much that is in milliseconds is what
  Phase 0 measures — the Vampire's 65 ms warm collection is the number to
  beat, and the 68020's is the one that matters for Clamacs.
- **Amiga allocation**: always a bump; no free-list probes, no
  sweep-forever regime, compaction only on measured fragmentation.
- **Host**: no change by default (gen stays).  `GC=immix` is the
  development and A/B vehicle; it may also beat classic for the
  `CLAMIGA_GENGC=0` and Windows paths, which is a side effect, not a goal.
- **Memory**: +0.8 % of the heap in side tables on the Amiga (0.4 % with
  256 B lines) plus 4 bytes per live finalizable object.
- **Code**: Part 1 removes ~500 lines of duplicated root walking and ~12
  hand-written arena loops; Part 2 adds one file of roughly the size of
  the classic sweep + free-list code it replaces on the m68k build.

## Risks, named

1. The seam refactor touches the two things CLAUDE.md flags as fragile
   (the VM hot path via `cl_alloc`, and GC-safety of every `CL_Obj`
   holder); the per-step benchmark gate and gc-stress are the defence.
2. The sense flip is a global invariant with four enumerated consumers;
   a fifth one found later is a memory-corruption bug, not a slowdown.
   The grep lint and the `DEBUG_GC` object-start plausibility checks are
   the net.
3. The clipping rule is one function but every walk depends on it; the
   iterator unification must land *before* the rule so there is exactly
   one place to get it right.
4. `GC_HDR_PAGE` (4 KB) and the line map are two side structures over the
   same arena with different granularities; keep them independent rather
   than merging them (merging is the tempting cleanup that couples the
   JIT scan to the backend).
5. Phase 0 may say "it's the mark".  Then the honest next lever on the
   Amiga is a smaller live set (not re-tracing image-resident data), and
   that is a different spec.
