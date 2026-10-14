/* Compaction forwarding structures: size bound and correctness.
 *
 * Until 2026-09-24 the compactor forwarded through a table with one uint32
 * per CL_ALIGN granule of the used heap span — a transient off-heap block as
 * large as the heap itself on 32-bit.  An A1200 with its 8 MB heap in the
 * Zorro block could not AllocVec another 7-8 MB once the trapdoor Fast RAM
 * held the compiler pool and the stacks, so every compaction attempt failed
 * and fell back to a sweep, and every bump-exhausted allocation re-ran the
 * whole mark phase to fail again: test-amiga-lowend spent ~25 s per
 * allocation in the 'alloc sweep-forever escape' check.
 *
 * The compactor now derives forwarding addresses from a live-granule bitmap
 * plus a per-page slide cursor (mem.c, gc_compute_forwarding / gc_forward):
 * about 1/32 + 1/64 of the used span.  These tests pin the bound and check
 * that the derived addresses are right for a fragmented live set whose
 * objects range from a cons to many pages, across several compactions.  The
 * gc-stress build additionally cross-checks the lookup against the slide's
 * placement on every live object of every compaction (gc_slide).
 *
 * The last two tests cover the fallback for a block that cannot be had at
 * all (cl_gc_fwd_fail_over stands in for platform_alloc refusing it): the
 * compaction sweeps instead and says so once per episode, and while that
 * latch is set an allocation that only a deep free-list block can satisfy
 * takes it instead of re-running a doomed compaction.  The exit-time leak
 * check for the same scenario lives in tests/test_memleak_tracked.sh. */
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
#include <string.h>
#ifndef PLATFORM_WIN32
#include <unistd.h>     /* dup/dup2 — capture what the runtime writes to stdout */
#endif

/* Classic-backend mechanics (the forwarding table's size and OOM
 * fallback, the sweep-forever latch): trivial pass under another backend. */
#ifdef CL_GC_CLASSIC

#define TEST_HEAP_SIZE (8 * 1024 * 1024)

static void setup(void)
{
    /* Classic collector: explicit compactions of the whole arena, no
     * nursery — the forwarding structures cover the full bump span. */
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

/* Fill three quarters of the arena with rooted vectors so the bump front
 * (which sizes the structures) sits high, as it does when a compaction is
 * actually needed. */
static void fill_heap_to_three_quarters(void)
{
    int guard = 0;
    cl_eval_string("(defparameter *fill* nil)");
    while (cl_heap.bump < cl_heap.arena_size / 4 * 3 && ++guard < 100000)
        cl_eval_string("(progn (push (make-array 1000) *fill*) nil)");
}

TEST(forwarding_structures_are_a_small_fraction_of_the_used_span)
{
    uint32_t need, old_design;

    fill_heap_to_three_quarters();
    ASSERT(cl_heap.bump >= cl_heap.arena_size / 4 * 3);

    need = cl_gc_fwd_bytes();
    old_design = cl_heap.bump / CL_ALIGN * 4;   /* one uint32 per granule */
    printf("    [bump=%uK: forwarding structures %uK, per-granule table would be %uK]\n",
           (unsigned)(cl_heap.bump >> 10), (unsigned)(need >> 10),
           (unsigned)(old_design >> 10));

    ASSERT(need > 0);
    /* 1/32 + 1/64 of the span on 32-bit, half that on a 64-bit host: never
     * more than a sixteenth either way. */
    ASSERT(need <= cl_heap.bump / 16);
    /* And an order of magnitude below the design it replaces. */
    ASSERT(need * 8 <= old_design);

    /* The structures are transient: a compaction allocates and releases
     * them, so nothing about the heap changes their size afterwards. */
    cl_eval_string("(setf *fill* nil)");
    cl_gc_compact();
    ASSERT(cl_gc_fwd_bytes() < need);       /* the span shrank with the live set */
}

/* A live set that exercises every part of the lookup: objects from one
 * cons up to 12 KB (spanning many 512-byte pages, so page entries inside
 * an object matter), interleaved with dead objects of varying size so live
 * granules sit at every bit position of the bitmap words, and enough of
 * them that the cursor crosses thousands of pages.  Each vector carries a
 * value pattern derived from its index; every slot is checked after the
 * compaction, which catches a wrong forwarding address as a wrong element
 * or a wrong length. */
static const char *build_form =
    "(progn"
    "  (defparameter *live* (make-array 1500))"
    "  (defparameter *conses* nil)"
    "  (let ((dead nil))"
    "    (dotimes (i 1500)"
    "      (let* ((n (if (zerop (mod i 50)) 3000 (1+ (mod (* i 37) 300))))"
    "             (v (make-array n)))"
    "        (dotimes (j n) (setf (aref v j) (+ (* i 4000) j)))"
    "        (setf (aref *live* i) v)"
    "        (push (cons i (* i 2)) *conses*)"
    "        (setf dead (make-array (1+ (mod (* i 53) 200))))))"
    "    (setf dead nil))"
    "  t)";

static const char *verify_form =
    "(let ((ok t) (k 1499))"
    "  (dotimes (i 1500)"
    "    (let* ((v (aref *live* i))"
    "           (n (if (zerop (mod i 50)) 3000 (1+ (mod (* i 37) 300)))))"
    "      (unless (and (vectorp v) (= (length v) n))"
    "        (setf ok nil))"
    "      (when ok"
    "        (dotimes (j n)"
    "          (unless (eql (aref v j) (+ (* i 4000) j)) (setf ok nil))))))"
    "  (dolist (c *conses*)"
    "    (unless (and (eql (car c) k) (eql (cdr c) (* k 2))) (setf ok nil))"
    "    (decf k))"
    "  ok)";

TEST(compaction_forwards_a_fragmented_live_set_correctly)
{
    uint32_t compact0 = cl_heap.compact_count;
    uint32_t bump_before;
    CL_Obj r;
    int round;

    r = cl_eval_string(build_form);
    ASSERT(r == CL_T);
    bump_before = cl_heap.bump;

    /* Three rounds: each compaction places the survivors somewhere new
     * (the first drops the interleaved garbage, the later ones the
     * compiler's per-form garbage), so the lookup runs against three
     * different bitmaps over the same objects. */
    for (round = 0; round < 3; round++) {
        cl_gc_compact();
        ASSERT_EQ_INT(cl_heap.compact_count, compact0 + round + 1);
        r = cl_eval_string(verify_form);
        ASSERT(r == CL_T);
    }
    /* The first compaction reclaimed the dead interleaved vectors: the
     * bump front sits below where the build left it. */
    ASSERT(cl_heap.bump < bump_before);
    printf("    [3 compactions over 1500 vectors + 1500 conses: bump %uK -> %uK]\n",
           (unsigned)(bump_before >> 10), (unsigned)(cl_heap.bump >> 10));
}

/* --- Forwarding-block OOM: fallback, once-per-episode message, latch --- */

#ifndef PLATFORM_WIN32
static FILE *cap_file = NULL;
static int   cap_saved = -1;

/* From here until capture_end, everything written to stdout — the runtime's
 * platform_write_string included — goes to a scratch file. */
static void capture_begin(void)
{
    fflush(stdout);
    cap_file = tmpfile();
    cap_saved = dup(fileno(stdout));
    dup2(fileno(cap_file), fileno(stdout));
}

static void capture_end(char *buf, size_t cap)
{
    size_t n;
    fflush(stdout);
    dup2(cap_saved, fileno(stdout));
    close(cap_saved);
    rewind(cap_file);
    n = fread(buf, 1, cap - 1, cap_file);
    buf[n] = '\0';
    fclose(cap_file);
}

static int count_of(const char *hay, const char *needle)
{
    int n = 0;
    const char *p = hay;
    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p += strlen(needle);
    }
    return n;
}
#define ASSERT_REPORTS(out, n) \
    ASSERT_EQ_INT(count_of((out), "GC cannot compact"), (n))
#else
/* No portable way to capture the console here: the state assertions (sweep
 * ran, no compaction, allocation served) still hold, the message count is
 * left to the POSIX hosts. */
#define capture_begin() ((void)0)
#define capture_end(buf, cap) ((buf)[0] = '\0')
#define ASSERT_REPORTS(out, n) ((void)0)
#endif

TEST(forwarding_table_oom_sweeps_instead_and_says_so_once_per_episode)
{
    char out[512];
    char want[64];
    uint32_t need, c0, g0;
    CL_Obj r;

    cl_eval_string("(defparameter *oom-live* (make-array 100))");
    need = cl_gc_fwd_bytes();
    sprintf(want, "no %lu bytes", (unsigned long)need);
    c0 = cl_heap.compact_count;
    g0 = cl_heap.gc_count;

    /* The block is refused: the collection degrades to a sweep and names
     * the size it could not get. */
    cl_gc_fwd_fail_over = 1;
    capture_begin();
    cl_gc_compact();
    capture_end(out, sizeof(out));
    ASSERT_REPORTS(out, 1);
#ifndef PLATFORM_WIN32
    ASSERT(strstr(out, want) != NULL);
    ASSERT(strstr(out, "sweeping instead") != NULL);
#endif
    ASSERT_EQ_INT(cl_heap.compact_count, c0);       /* nothing was moved */
    ASSERT_EQ_INT(cl_heap.gc_count, g0 + 1);        /* the sweep ran */

    /* Same episode (the latch is set): the retry is silent. */
    capture_begin();
    cl_gc_compact();
    capture_end(out, sizeof(out));
    ASSERT_REPORTS(out, 0);
    ASSERT_EQ_INT(cl_heap.compact_count, c0);
    ASSERT_EQ_INT(cl_heap.gc_count, g0 + 2);

    /* The heap keeps working through it. */
    r = cl_eval_string("(length (make-list 1000))");
    ASSERT(r == CL_MAKE_FIXNUM(1000));

    /* A compaction that gets its block ends the episode... */
    cl_gc_fwd_fail_over = 0;
    capture_begin();
    cl_gc_compact();
    capture_end(out, sizeof(out));
    ASSERT_REPORTS(out, 0);
    ASSERT_EQ_INT(cl_heap.compact_count, c0 + 1);

    /* ...so the next refusal is reported again. */
    cl_gc_fwd_fail_over = 1;
    capture_begin();
    cl_gc_compact();
    capture_end(out, sizeof(out));
    ASSERT_REPORTS(out, 1);
    ASSERT_EQ_INT(cl_heap.compact_count, c0 + 1);

    cl_gc_fwd_fail_over = 0;
    cl_gc_compact();                    /* leave the latch clear */
    ASSERT_EQ_INT(cl_heap.compact_count, c0 + 2);
}

/* Small vectors 100 elements long (~0.4 KB), one deep free block of 1500
 * (~6 KB), and a request of 600 (~2.4 KB): fits the deep block only. */
#define OOM_SMALL_LEN  100
#define OOM_BIG_LEN    1500
#define OOM_TARGET_LEN 600

TEST(latched_allocation_takes_a_deep_free_block_instead_of_compacting)
{
    CL_Obj keep = CL_NIL, v = CL_NIL, r;
    char out[512];
    uint32_t c0, g0;

    CL_GC_PROTECT(keep);
    CL_GC_PROTECT(v);

    /* Start from a compacted heap (empty free list), lay down one dead
     * block that will end up LAST on the free list (sweeps push blocks at
     * the head, so the lowest address is deepest), then alternate live and
     * dead small vectors up to the arena's end: the dead ones are
     * separated by live objects, so none coalesces into a fit.  The bump
     * tail is left smaller than the request. */
    cl_gc_compact();
    ASSERT_EQ_INT(cl_heap.free_list, 0);
    (void)cl_make_vector(OOM_BIG_LEN);
    while (cl_heap.arena_size - cl_heap.bump >= 2048) {
        v = cl_make_vector(OOM_SMALL_LEN);
        keep = cl_cons(v, keep);
        (void)cl_make_vector(OOM_SMALL_LEN);
    }
    ASSERT(cl_heap.arena_size - cl_heap.bump < 4 * OOM_TARGET_LEN);

    /* Latch: the forwarding block is refused at this bump level. */
    cl_gc_fwd_fail_over = 1;
    capture_begin();
    cl_gc_compact();
    capture_end(out, sizeof(out));
    ASSERT_REPORTS(out, 1);
    ASSERT(cl_heap.free_list != 0);

    /* The bump tail cannot serve the request and the bounded probe (the
     * first CL_FREELIST_PROBE_LIMIT blocks) sees only small ones; with the
     * latch set the allocator must walk the whole list — one sweep, no
     * compaction attempt (which would mark the whole heap again just to be
     * refused: a second gc_count) — and land in the deep block.  Not
     * captured: a storage error here exits the process, and its message
     * must reach the console. */
    c0 = cl_heap.compact_count;
    g0 = cl_heap.gc_count;
    r = cl_make_vector(OOM_TARGET_LEN);
    ASSERT(CL_VECTOR_P(r));
    if (CL_VECTOR_P(r))
        ASSERT_EQ_INT(((CL_Vector *)CL_OBJ_TO_PTR(r))->length, OOM_TARGET_LEN);
    ASSERT_EQ_INT(cl_heap.compact_count, c0);
    ASSERT_EQ_INT(cl_heap.gc_count, g0 + 1);

    cl_gc_fwd_fail_over = 0;
    CL_GC_UNPROTECT(2);
    cl_gc_compact();                    /* success: clears the latch, frees the fill */
    ASSERT_EQ_INT(cl_heap.compact_count, c0 + 1);
}

int main(void)
{
    setup();
    RUN(forwarding_structures_are_a_small_fraction_of_the_used_span);
    RUN(compaction_forwards_a_fragmented_live_set_correctly);
    RUN(forwarding_table_oom_sweeps_instead_and_says_so_once_per_episode);
    RUN(latched_allocation_takes_a_deep_free_block_instead_of_compacting);
    teardown();
    REPORT();
}

#else /* !CL_GC_CLASSIC */

TEST(classic_backend_not_built) { ASSERT(1); }
int main(void) { test_init(); RUN(classic_backend_not_built); REPORT(); }

#endif /* CL_GC_CLASSIC */
