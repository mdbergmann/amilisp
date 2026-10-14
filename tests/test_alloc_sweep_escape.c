/* Regression test for the "sweep-forever" GC pathology.
 *
 * Symptom: cold-compiling a large system (e.g. chipi-ui's ~700-file dep
 * closure) took ~9 minutes at some heap sizes and ~1 minute at others, with
 * the slow runs spending nearly all their time in the GC.  Collection counts
 * were wildly non-monotonic in heap size (e.g. 192M -> 1293 collections / 264s,
 * 256M -> 15 / 54s, 512M -> 203 / 135s) — a *bigger* heap triggering *more*
 * collections is the tell that this is a trigger pathology, not heap pressure.
 *
 * Root cause (mem.c, cl_alloc slow path):
 *   - Once the bump pointer is exhausted, allocation falls back to a bounded
 *     (CL_FREELIST_PROBE_LIMIT) first-fit free-list probe.
 *   - On a miss, cl_alloc ran a full mark-and-sweep cl_gc().  But a sweep-only
 *     GC never resets the bump pointer — it only re-coalesces the free list.
 *   - Compaction (which DOES reset the bump pointer and defragment) only fired
 *     when the *post-sweep* probe also missed.  When the post-sweep probe
 *     happened to find a fit (a recently-dead object swept onto the list head),
 *     ptr != NULL and compaction was skipped — so the next allocation was right
 *     back in the same state, sweeping again.  Result: a full-heap mark-sweep
 *     on nearly every allocation, forever.
 *
 * Fix: track sweeps-since-last-compaction.  After GC_SWEEPS_BEFORE_COMPACT
 * bump-exhausted sweeps without lasting progress (and when the heap is not
 * genuinely near-full), compact instead of sweeping — resetting the bump
 * pointer, defragmenting, and restoring a long run of O(1) bump allocations.
 *
 * This test rebuilds the sweep-forever shape and asserts the escape hatch
 * holds: collections stay bounded and compaction fires.  Validated to actually
 * reproduce: with the fix disabled (-DGC_SWEEPS_BEFORE_COMPACT large) this same
 * workload drives the collection count into the thousands; with the fix it
 * stays an order of magnitude smaller.
 */
#include "test.h"
#include "core/types.h"
#include "core/mem.h"
#include "core/error.h"
#include "core/package.h"
#include "core/symbol.h"
#include "core/reader.h"
#include "core/printer.h"
#include "core/compiler.h"
#include "core/vm.h"
#include "core/builtins.h"
#include "core/repl.h"
#include "core/thread.h"
#include "platform/platform.h"

#include <stdio.h>

/* Classic-backend policy (the sweep-forever escape): trivial pass under
 * another backend. */
#ifdef CL_GC_CLASSIC

/* A small, fixed arena keeps the heap fill fast and deterministic. */
#define TEST_HEAP_SIZE (8 * 1024 * 1024)

/* Element counts (CL_Obj-sized slots) for the building blocks.  TINY holes must
 * be far smaller than a MED request so the bounded free-list probe misses them;
 * the SEED is one MED-sized slot that recycles each round. */
#define TINY_ELEMS  60     /* ~250 B isolated holes — too small for a MED req */
#define LIVE_ELEMS 150     /* ~620 B rooted blocks separating the holes        */
#define MED_ELEMS  900     /* ~3.6 KB request — fits only the recyclable slot  */
#define SEED_ELEMS 950     /* ~3.8 KB seed slot, dropped to prime the loop     */

static void setup(void)
{
    /* This suite pins the CLASSIC collector's sweep-forever escape
     * heuristics; the generational collector has no sweep regime (majors
     * are always compactions), so run the binary in classic mode. */
    setenv("AMILISP_GENGC", "0", 1);
    platform_init();
    cl_thread_init();
    cl_error_init();
    cl_mem_init(TEST_HEAP_SIZE);
    cl_package_init();
    cl_symbol_init();
    cl_reader_init();
    cl_printer_init();
    cl_compiler_init();
    cl_vm_init(0, 0);
    cl_builtins_init();
    cl_quiet_boot = 1;
    cl_repl_init_no_userinit(1);
}

static void teardown(void)
{
    cl_mem_shutdown();
    platform_shutdown();
}

/* Build the pathological heap: bump pointer pushed to the very top, the only
 * free space being many TINY isolated holes (each smaller than a MED request,
 * and non-adjacent so a sweep cannot coalesce them into a usable block) plus a
 * single MED-sized recyclable seed slot near the top of the arena.  Crucially
 * the live set stays well under the arena (so the heap is NOT near-full and the
 * compaction escape hatch is in play, exactly as in the real cold-compile where
 * the live set is a fraction of the heap). */
static void build_sweep_forever_heap(void)
{
    char buf[128];
    uint32_t reserve;
    int guard = 0;

    cl_eval_string("(defparameter *live* nil)");   /* rooted: stays live */
    cl_eval_string("(defparameter *dead* nil)");   /* dropped: becomes holes */

    /* Interleave one rooted block and one transient block per step.  The
     * transient sits between two live blocks, so once dropped its hole is
     * isolated.  Stop a little short of the top to leave room for the seed. */
    reserve = (SEED_ELEMS + 64) * 4 + 4096;
    while (cl_heap.arena_size - cl_heap.bump > reserve && ++guard < 20000) {
        sprintf(buf,
                "(progn (push (make-array %u) *live*)"
                "       (push (make-array %u) *dead*) nil)",
                (unsigned)LIVE_ELEMS, (unsigned)TINY_ELEMS);
        cl_eval_string(buf);
    }

    /* The seed: one MED-sized rooted slot at the highest address.  Dropped
     * below, it primes the first iteration and, being highest, sweeps to the
     * free-list head each round. */
    sprintf(buf, "(defparameter *seed* (make-array %u))", (unsigned)SEED_ELEMS);
    cl_eval_string(buf);

    /* Drop the transients and the seed, then sweep: the free list is now a long
     * run of tiny isolated holes with the MED seed slot at its head. */
    cl_eval_string("(setf *dead* nil)");
    cl_eval_string("(setf *seed* nil)");
    cl_gc();
}

TEST(collections_stay_bounded_when_bump_exhausted)
{
    uint32_t gc0, gc_delta, compact0, compact_delta;
    int i;

    build_sweep_forever_heap();
    ASSERT(cl_heap.free_list != 0);     /* a fragmented free list exists */

    gc0 = cl_heap.gc_count;
    compact0 = cl_heap.compact_count;

    /* Hot loop: MED-sized vectors allocated directly (NOT via cl_eval_string,
     * whose per-form compilation would inject unrelated garbage and mask the
     * effect).  Each result is dropped on the floor — not rooted, not on the VM
     * stack — so it is dead before the next allocation.  Bump is exhausted and
     * no standing hole fits a MED request, but the previous iteration's now-dead
     * vector can be swept back in: exactly the condition under which the old
     * code swept on every single iteration without ever compacting. */
    for (i = 0; i < 5000; i++) {
        volatile CL_Obj v = cl_make_vector(MED_ELEMS);
        (void)v;
    }

    gc_delta = cl_heap.gc_count - gc0;
    compact_delta = cl_heap.compact_count - compact0;

    printf("    [5000 MED allocs on an exhausted/fragmented heap: "
           "collections=%u, compactions=%u]\n",
           (unsigned)gc_delta, (unsigned)compact_delta);

    /* The slow path WAS exercised: these allocations forced real GC work. */
    ASSERT(gc_delta > 0);
    /* The escape hatch fired: compaction reset the bump pointer rather than
     * letting the sweep loop run away. */
    ASSERT(compact_delta >= 1);
    /* The decisive bound: collections never approach the allocation count.
     * The fix caps sweeps at ~GC_SWEEPS_BEFORE_COMPACT between compactions, and
     * each compaction buys a long run of bump allocations, so total collections
     * stay an order of magnitude below 5000.  The old (sweep-forever) code does
     * ~one collection PER allocation (4999) here — this assertion is what fails
     * without the fix. */
    ASSERT(gc_delta < 500);
    /* The ratio invariant the fix guarantees: never more than
     * ~GC_SWEEPS_BEFORE_COMPACT sweeps per compaction cycle.  (+1 cycle and a
     * few sweeps of slack for the final partial cycle / boundary effects.) */
    ASSERT(gc_delta <= (compact_delta + 1) * (GC_SWEEPS_BEFORE_COMPACT + 4));
}

int main(void)
{
    setup();
    RUN(collections_stay_bounded_when_bump_exhausted);
    teardown();
    REPORT();
}

#else /* !CL_GC_CLASSIC */

TEST(classic_backend_not_built) { ASSERT(1); }
int main(void) { test_init(); RUN(classic_backend_not_built); REPORT(); }

#endif /* CL_GC_CLASSIC */
