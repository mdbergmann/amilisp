/* Regression test for the growable GC mark stack.
 *
 * Symptom (found 2026-07-05 investigating "loading from Sly is 60% slower
 * than a fresh REPL"): with the mark stack FIXED at CL_GC_MARK_STACK_SIZE
 * (4096) entries, any object with more unmarked children than the stack's
 * free slots (a >4K-element vector, a big hash table) silently overflowed
 * it.  Overflow recovery re-scans the ENTIRE arena for marked objects and
 * re-pushes their children — but each pass recovers at most ~one-stack-full
 * of dropped children, so a large live heap needs live/capacity passes,
 * each O(live edges).  Measured on a 210MB live heap: 49 s per mark-sweep
 * vs 35 ms with a large-enough stack (1400x).  Long-lived images (Sly
 * sessions, big application loads) hit this on every GC cycle.
 *
 * Fix: the mark stack starts on a small static buffer and grows
 * geometrically on demand (heap-proportional cap), keeping the re-scan
 * loop only as the correctness fallback for growth failure (cap/OOM).
 *
 * This test asserts three things:
 *   1. A wide object graph (one vector with many cons children) marks
 *      WITHOUT any overflow re-scan pass — growth handled it.
 *   2. A compacting (moving) GC over the same graph relocates it intact.
 *   3. With growth artificially capped (test hook), the overflow re-scan
 *      fallback still marks the graph completely and correctly — nothing
 *      live is lost, it is merely slow.  This path was previously the ONLY
 *      path; it must stay correct for genuine OOM-during-GC situations.
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

/* Small arena: keeps the test fast and puts the heap-proportional growth cap
 * (arena/128 entries = 64K entries here) comfortably above what the WIDE
 * vector needs, while staying far below host RAM. */
#define TEST_HEAP_SIZE (8 * 1024 * 1024)

/* More children than the 4096-entry initial stack can hold, so marking the
 * vector forces growth (or, with the test-hook cap, overflow). */
#define WIDE_ELEMS 30000

static void setup(void)
{
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

/* Helper: eval a string, return printed result (error-safe). */
static const char *eval_print(const char *str)
{
    static char buf[512];
    int err;

    CL_CATCH(err);
    if (err == CL_ERR_NONE) {
        CL_Obj result = cl_eval_string(str);
        cl_prin1_to_string(result, buf, sizeof(buf));
        CL_UNCATCH();
        return buf;
    } else {
        CL_UNCATCH();
        snprintf(buf, sizeof(buf), "ERROR:%d", err);
        cl_vm.sp = 0;
        cl_vm.fp = 0;
        return buf;
    }
}

/* Build one wide vector: slot i holds (cons i (* i 2)).  A single marked
 * vector then pushes WIDE_ELEMS unmarked cons children at once — the exact
 * shape that overflows a fixed-size mark stack. */
static void build_wide_vector(void)
{
    char buf[160];
    sprintf(buf,
            "(progn (defparameter *wide* (make-array %d))"
            "       (dotimes (i %d) (setf (aref *wide* i) (cons i (* i 2))))"
            "       nil)",
            WIDE_ELEMS, WIDE_ELEMS);
    ASSERT_STR_EQ(eval_print(buf), "NIL");
}

/* Verify every slot of *wide* still holds the exact conses it was built
 * with.  Any child dropped by the mark phase is swept, so a car/cdr read
 * after GC would see freed/poisoned memory or a relocated stranger. */
static void assert_wide_vector_intact(void)
{
    char buf[200];
    sprintf(buf,
            "(let ((bad 0))"
            "  (dotimes (i %d)"
            "    (let ((c (aref *wide* i)))"
            "      (unless (and (consp c) (eql (car c) i) (eql (cdr c) (* i 2)))"
            "        (setq bad (+ bad 1)))))"
            "  bad)",
            WIDE_ELEMS);
    ASSERT_STR_EQ(eval_print(buf), "0");
}

TEST(wide_vector_marks_via_growth_no_rescan)
{
    uint32_t cap, grows, rescans;

    build_wide_vector();
    cl_gc();

    cl_gc_mark_stack_stats(&cap, &grows, &rescans);
    printf("    [after GC: cap=%u entries, grows=%u, rescan passes=%u]\n",
           (unsigned)cap, (unsigned)grows, (unsigned)rescans);

    /* The stack grew past its initial size to absorb the wide push... */
    ASSERT(grows > 0);
    ASSERT(cap > CL_GC_MARK_STACK_SIZE);
    ASSERT(cap >= (uint32_t)WIDE_ELEMS);
    /* ...so the quadratic overflow fallback never ran... */
    ASSERT_EQ(rescans, 0u);
    /* ...and the graph survived intact. */
    assert_wide_vector_intact();
}

TEST(wide_vector_survives_compaction)
{
    uint32_t cap, grows, rescans;

    /* Interleave garbage so compaction actually relocates the survivors. */
    ASSERT_STR_EQ(eval_print(
        "(progn (dotimes (i 2000) (make-array 32)) nil)"), "NIL");
    cl_gc_compact();

    cl_gc_mark_stack_stats(&cap, &grows, &rescans);
    ASSERT_EQ(rescans, 0u);
    /* The moving GC forwarded the vector and every one of its children. */
    assert_wide_vector_intact();
}

/* Runs on a FRESH runtime (see main): cl_mem_init reset the stack to the
 * static initial buffer, so its capacity is far below WIDE_ELEMS again. */
TEST(overflow_rescan_fallback_still_marks_completely)
{
    uint32_t cap0, cap, grows, rescans0, rescans;

    build_wide_vector();

    /* Freeze the stack at its CURRENT capacity: every further grow attempt
     * fails, so marking *wide* (WIDE_ELEMS children > cap) must overflow and
     * take the re-scan path — the pre-fix behavior, kept as OOM fallback. */
    cl_gc_mark_stack_stats(&cap0, NULL, &rescans0);
    ASSERT(cap0 < (uint32_t)WIDE_ELEMS);  /* graph is wider than the stack */
    cl_gc_mark_stack_set_test_limit(cap0);

    cl_gc();

    cl_gc_mark_stack_stats(&cap, &grows, &rescans);
    printf("    [capped at %u entries: rescan passes=%u]\n",
           (unsigned)cap, (unsigned)(rescans - rescans0));

    /* Growth was blocked... */
    ASSERT_EQ(cap, cap0);
    /* ...the overflow re-scan protocol ran... */
    ASSERT(rescans > rescans0);
    /* ...and STILL marked everything: correctness must not depend on the
     * stack being big enough. */
    assert_wide_vector_intact();

    /* Un-cap and confirm the next GC recovers to growth mode (the failed-
     * grow latch is per-cycle, not sticky). */
    cl_gc_mark_stack_set_test_limit(0);
    cl_gc();
    cl_gc_mark_stack_stats(&cap, NULL, &rescans0);
    ASSERT(cap > cap0);
    assert_wide_vector_intact();
}

/* ----------------------------------------------------------------
 * The trace marks an object when a reference to it is VISITED and queues
 * only objects that have children (gc_mark_visit, gc_trace.c).  Two
 * invariants that gives, each pinned below:
 *   1. A leaf (a string, a float, ...) never enters the mark stack, so a
 *      vector of WIDE_ELEMS fresh strings costs no stack growth at all —
 *      the pre-change trace pushed every one of them.
 *   2. An object the full stack could not take is marked but NOT yet
 *      scanned; the overflow re-scan must still reach ITS children.  The
 *      earlier fallback test cannot see this (its dropped conses hold
 *      fixnums); this one drops conses that each hold a fresh string.
 * ---------------------------------------------------------------- */

TEST(wide_vector_of_leaves_never_grows_the_stack)
{
    uint32_t grows0, grows, rescans;
    char buf[200];

    /* The growth the boot heap itself needs, if any, is the baseline. */
    cl_gc();
    cl_gc_mark_stack_stats(NULL, &grows0, NULL);

    sprintf(buf,
            "(progn (defparameter *leaves* (make-array %d))"
            "       (dotimes (i %d) (setf (aref *leaves* i) (make-string 4 :initial-element #\\a)))"
            "       nil)",
            WIDE_ELEMS, WIDE_ELEMS);
    ASSERT_STR_EQ(eval_print(buf), "NIL");
    cl_gc();

    cl_gc_mark_stack_stats(NULL, &grows, &rescans);
    printf("    [leaves: grows before=%u after=%u]\n",
           (unsigned)grows0, (unsigned)grows);
    ASSERT_EQ(grows, grows0);
    ASSERT_EQ(rescans, 0u);
    /* ...and every string is still there, untouched. */
    sprintf(buf,
            "(let ((bad 0))"
            "  (dotimes (i %d)"
            "    (unless (string= (aref *leaves* i) \"aaaa\") (setq bad (+ bad 1))))"
            "  bad)",
            WIDE_ELEMS);
    ASSERT_STR_EQ(eval_print(buf), "0");
}

/* Every slot i of *deep* holds (cons "abcd" i): the conses are what the
 * capped stack drops, the strings are what only a scan of a DROPPED cons
 * reaches. */
static void build_wide_vector_of_conses_with_strings(void)
{
    char buf[200];
    sprintf(buf,
            "(progn (defparameter *deep* (make-array %d))"
            "       (dotimes (i %d) (setf (aref *deep* i) (cons (make-string 4 :initial-element #\\b) i)))"
            "       nil)",
            WIDE_ELEMS, WIDE_ELEMS);
    ASSERT_STR_EQ(eval_print(buf), "NIL");
}

static void assert_deep_strings_intact(void)
{
    char buf[220];
    sprintf(buf,
            "(let ((bad 0))"
            "  (dotimes (i %d)"
            "    (let ((c (aref *deep* i)))"
            "      (unless (and (consp c) (eql (cdr c) i) (stringp (car c))"
            "                   (string= (car c) \"bbbb\"))"
            "        (setq bad (+ bad 1)))))"
            "  bad)",
            WIDE_ELEMS);
    ASSERT_STR_EQ(eval_print(buf), "0");
}

TEST(overflow_rescan_scans_the_dropped_objects_children)
{
    uint32_t cap0, cap, rescans0, rescans;
    char buf[160];

    build_wide_vector_of_conses_with_strings();

    cl_gc_mark_stack_stats(&cap0, NULL, &rescans0);
    ASSERT(cap0 < (uint32_t)WIDE_ELEMS);
    cl_gc_mark_stack_set_test_limit(cap0);

    cl_gc();

    cl_gc_mark_stack_stats(&cap, NULL, &rescans);
    ASSERT_EQ(cap, cap0);
    ASSERT(rescans > rescans0);

    /* A string the re-scan missed was swept: fill the free list with
     * strings of another letter so a dangling car READS as corrupted
     * instead of still showing its old bytes. */
    sprintf(buf, "(progn (dotimes (i %d) (make-string 4 :initial-element #\\z)) nil)",
            2 * WIDE_ELEMS);
    ASSERT_STR_EQ(eval_print(buf), "NIL");
    assert_deep_strings_intact();

    /* And through a moving collection, still capped. */
    cl_gc_compact();
    assert_deep_strings_intact();

    cl_gc_mark_stack_set_test_limit(0);
}

int main(void)
{
    setup();
    RUN(wide_vector_marks_via_growth_no_rescan);
    RUN(wide_vector_survives_compaction);
    teardown();

    /* Fresh runtime: cl_mem_init shrinks the mark stack back to the static
     * initial buffer, so the fallback test starts below WIDE_ELEMS again. */
    setup();
    RUN(overflow_rescan_fallback_still_marks_completely);
    teardown();

    setup();
    RUN(wide_vector_of_leaves_never_grows_the_stack);
    teardown();

    setup();
    RUN(overflow_rescan_scans_the_dropped_objects_children);
    teardown();
    REPORT();
}
