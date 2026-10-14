/* Regression test for the O(n^2) free-list allocation hang.
 *
 * Symptom: loading chipi (via quicklisp) wedged at 100% CPU right after
 * "Done compiling eisel-lemire.lisp".  jzon's eisel-lemire builds a 696-entry
 * power-of-ten table at FASL *load* time, doing millions of bignum
 * allocations.  On a fresh heap this is fast; under the full chipi heap it
 * hung forever.
 *
 * Root cause (mem.c):
 *   - alloc_from_free_list is first-fit and O(list length).
 *   - A sweep-only cl_gc() never resets the bump pointer, so once the arena
 *     high-water mark is reached (which the chipi compile easily does), every
 *     allocation is served from the free list.
 *   - When the free list is long AND fragmented (many small blocks in front of
 *     a block large enough to satisfy the request), each allocation walks past
 *     all the small blocks — O(n) per allocation, O(n^2) for the workload.
 *   - Compaction (which resets the bump pointer and clears fragmentation) only
 *     triggered when the free-list walk *fully failed*; here it always found a
 *     fit deep in the list, so compaction never fired and the walk never ended.
 *
 * Fix: cl_alloc probes only CL_FREELIST_PROBE_LIMIT blocks on the fast path; if
 * no fit turns up it compacts (resetting the bump pointer) instead of walking
 * the whole list.  A single unbounded walk remains as the last resort before
 * declaring the heap exhausted, so a genuine deep fit is never missed.
 *
 * This test rebuilds exactly that pathological heap shape and asserts the
 * total free-list walk stays bounded.  Without the fix the walk count explodes
 * to ~(allocations * free-list length); with it, it stays tiny because the
 * first miss compacts and restores the O(1) bump path.
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

/* Classic-backend mechanics (the free list and its bounded probe):
 * trivial pass under another backend. */
#ifdef CL_GC_CLASSIC

/* A small, fixed arena keeps the heap fill fast and deterministic. */
#define TEST_HEAP_SIZE (8 * 1024 * 1024)

static void setup(void)
{
    /* This suite tests the CLASSIC collector's free-list mechanics
     * (bounded probe, sweep/compact interplay).  Under the generational
     * collector the free list is intentionally unused (majors are always
     * compactions), so pin classic mode for the whole binary. */
    setenv("CLAMIGA_GENGC", "0", 1);
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
    /* Skip user-init (.amilisprc / asdf / quicklisp): it would fill this small
     * test heap with uncontrolled state and defeat the deterministic layout. */
    cl_quiet_boot = 1;
    cl_repl_init_no_userinit(1);
}

static void teardown(void)
{
    cl_mem_shutdown();
    platform_shutdown();
}

/* Fill the bump region with rooted vectors until fewer than `gap` bytes remain,
 * so the next allocation larger than `gap` must come from the free list (a
 * sweep-only GC never moves the bump pointer back down).  Converges in a few
 * iterations and never trips a GC: each vector is sized strictly smaller than
 * the space left, and is rooted on the Lisp list *fill*. */
static void fill_bump_to_gap(uint32_t gap)
{
    char buf[96];
    int guard = 0;
    cl_eval_string("(defparameter *fill* nil)");
    for (;;) {
        uint32_t remaining = cl_heap.arena_size - cl_heap.bump;
        uint32_t elems;
        if (remaining <= gap || ++guard > 200)
            break;
        /* Each vector consumes at most ~half the remaining bump space, so it
         * always fits and never triggers a GC (a GC here would compact away the
         * dead junk blocks we are trying to keep on the free list).  Remaining
         * halves each iteration, converging to < gap. */
        elems = (remaining / 2) / 4;
        if (elems < 1) elems = 1;
        sprintf(buf, "(push (make-array %u) *fill*)", (unsigned)elems);
        cl_eval_string(buf);
    }
}

/* Sanity (run first, while the heap is still un-fragmented): ordinary
 * allocation is pure bump allocation and never touches the free list, so the
 * diagnostic counter stays put. */
TEST(freelist_untouched_on_fresh_heap)
{
    uint32_t steps0 = cl_heap.freelist_steps;
    cl_eval_string("(dotimes (i 1000) (cons i i))");
    ASSERT_EQ_INT((int)(cl_heap.freelist_steps - steps0), 0);
}

/* Build a heap whose free list is long, fragmented, and has a big block buried
 * at the tail; then run medium allocations that can only be satisfied by that
 * deep block — the exact shape that degraded to an O(n^2) walk. */
TEST(freelist_walk_is_bounded_under_fragmentation)
{
    uint32_t steps0, steps_delta, compact0, compact_delta;

    /* A big donor object at a LOW arena address.  We drop it below so it
     * becomes the only free block large enough for the medium requests, and
     * (being lowest-address) it lands at the TAIL of the LIFO free list, behind
     * every tiny block.  Without a deep fit the unbounded walk would itself
     * fail and compact — the pathology needs allocations to keep *succeeding*
     * via long walks. */
    cl_eval_string("(defparameter *v* (make-array 200000))");   /* ~800 KB */

    /* Fragment the heap: build two interleaved lists, then drop one.  Each
     * dropped pair ends up isolated between live pairs, so a sweep cannot
     * coalesce them — a long free list of tiny (cons-sized) blocks results in
     * FRONT of the donor (which was allocated earlier, at a lower address, and
     * so lands at the tail of the LIFO free list).  Both pushes retain their
     * cons, so neither is dead-code-eliminated. */
    cl_eval_string("(defparameter *live* nil)");
    cl_eval_string("(defparameter *junk* nil)");
    cl_eval_string("(dotimes (i 4000) (push (cons i i) *live*) (push (cons i i) *junk*))");
    cl_eval_string("(setf *junk* nil)");

    /* Exhaust the bump region so subsequent allocations must use the free
     * list.  The 256-byte gap is far smaller than the request sized below. */
    fill_bump_to_gap(256);

    /* Drop the donor, then sweep: free list becomes [tiny ... tiny, BIG]. */
    cl_eval_string("(setf *v* nil)");
    cl_gc();

    ASSERT(cl_heap.free_list != 0);          /* a free list actually exists */

    /* Measure the largest non-donor free block, then size the request so ONLY
     * the deep donor can satisfy it — every request must therefore walk past
     * all the smaller blocks to reach the donor (the O(n) walk we are guarding
     * against).  Sizing from the measured maximum keeps the test robust to
     * incidental medium-sized blocks (boot/CLOS garbage, fill remainders). */
    {
        uint32_t off = cl_heap.free_list;
        int n = 0, donor = -1;
        uint32_t max_nondonor = 0, req_bytes;
        char buf[96];
        while (off) {
            CL_FreeBlock *b = (CL_FreeBlock *)(cl_heap.arena + off);
            if (b->size >= 700000) donor = n;
            else if (b->size > max_nondonor) max_nondonor = b->size;
            off = b->next_offset; n++;
        }
        printf("    [free_list len=%d, donor at %d, max non-donor block=%u]\n",
               n, donor, (unsigned)max_nondonor);
        ASSERT(donor > 1000);                 /* donor is buried deep */

        req_bytes = max_nondonor * 2 + 4096;  /* larger than any non-donor block */
        ASSERT(req_bytes < 700000);           /* but smaller than the donor */

        steps0 = cl_heap.freelist_steps;
        compact0 = cl_heap.compact_count;

        /* Each request fits ONLY in the donor.  Old code: walk all ~4000
         * smaller blocks to reach it, every time -> millions of steps, no
         * compaction.  Fixed code: the bounded probe misses, compaction resets
         * the bump pointer, and allocation returns to O(1). */
        sprintf(buf, "(dotimes (i 500) (make-array %u))",
                (unsigned)(req_bytes / 4));
        cl_eval_string(buf);
    }

    steps_delta = cl_heap.freelist_steps - steps0;
    compact_delta = cl_heap.compact_count - compact0;

    printf("    [freelist_steps delta=%u, compactions=%u]\n",
           (unsigned)steps_delta, (unsigned)compact_delta);

    /* The free-list path WAS exercised (bump was exhausted)... */
    ASSERT(steps_delta > 0);
    /* ...and compaction restored the bump path rather than letting the walk
     * run away. */
    ASSERT(compact_delta >= 1);
    /* The decisive bound: a single fast-path probe never exceeds the limit, so
     * total steps stay O(allocations * limit), nowhere near O(allocations *
     * list length).  The old (unbounded) code lands in the millions here. */
    ASSERT(steps_delta < 50000);
}

int main(void)
{
    setup();
    RUN(freelist_untouched_on_fresh_heap);
    RUN(freelist_walk_is_bounded_under_fragmentation);
    teardown();
    REPORT();
}

#else /* !CL_GC_CLASSIC */

TEST(classic_backend_not_built) { ASSERT(1); }
int main(void) { test_init(); RUN(classic_backend_not_built); REPORT(); }

#endif /* CL_GC_CLASSIC */
