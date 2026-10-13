/*
 * test_gc_root_dedup.c — regression tests for the two remaining tier-2
 * GC-audit items:
 *
 * 1. gc_forward non-idempotence hardening.  The forwarding table is keyed
 *    by pre-compaction offsets, so a root slot registered twice (double
 *    CL_GC_PROTECT, a global both cl_gc_register_root'ed and protected,
 *    or a protect of an already-rooted VM-stack slot) used to be
 *    forwarded twice: the second lookup mapped the already-forwarded
 *    offset through whatever object's OLD offset it collided with,
 *    silently rewriting the root to an unrelated object.  Fixed by
 *    gc_update_registered_roots (one sorted, deduplicated pass over
 *    global_roots[] + all thread gc_roots[], skipping VM-stack aliases).
 *
 *    The double_* tests construct the collision DETERMINISTICALLY:
 *    after a normalizing compaction the heap below bump is 100% live, so
 *    allocating [garbage cons][live cons C][live cons X] (equal sizes)
 *    makes X's forwarded offset equal C's old offset exactly — the
 *    pre-fix second forward relocated X's root onto C's new home.
 *
 * 2. bi_assoc / bi_rassoc / subst / sublis / bi_adjoin held their list
 *    cursors and the :test function in unprotected C locals across
 *    call_test (a Lisp :test can allocate and compact).  These builtins
 *    are shadowed by boot.lisp, so the tests run under
 *    cl_repl_init_minimal (no boot.lisp) and use a :test lambda that
 *    calls (ext:gc-compact) to force relocation on every comparison.
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

/* Drives the registered-root dedup through the classic backend's
 * compactions and free-list reuse: trivial pass under another backend. */
#ifdef CL_GC_CLASSIC

static void setup(void)
{
    platform_init();
    cl_thread_init();
    cl_error_init();
    cl_mem_init(CL_DEFAULT_HEAP_SIZE);
    cl_package_init();
    cl_symbol_init();
    cl_reader_init();
    cl_printer_init();
    cl_compiler_init();
    cl_vm_init(0, 0);
    cl_builtins_init();
    /* NO boot.lisp: keeps ASSOC/RASSOC/SUBST/SUBLIS/ADJOIN bound to the
     * C builtins under test (boot.lisp shadows them with Lisp code). */
    cl_repl_init_minimal();
}

static void teardown(void)
{
    cl_mem_shutdown();
    platform_shutdown();
}

/* Eval one form, return its printed (prin1) representation. */
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
        return buf;
    }
}

/* --- 1. Double-forward hardening ------------------------------------- */

TEST(double_protect_same_slot_survives_compaction)
{
    CL_Obj g1, c, x;

    /* Normalize: everything below bump live, free list empty, so the next
     * three conses are contiguous bump allocations. */
    cl_gc_compact();

    g1 = cl_cons(CL_MAKE_FIXNUM(111), CL_NIL);
    c  = cl_cons(CL_MAKE_FIXNUM(777), CL_NIL);
    x  = cl_cons(CL_MAKE_FIXNUM(888), CL_NIL);
    g1 = CL_NIL;   /* drop the only reference — garbage at the next GC */

    CL_GC_PROTECT(c);
    /* The hazard: the same slot on the root stack twice.  Pre-fix, x was
     * forwarded twice and its once-forwarded offset equals c's old offset
     * by construction — the second forward moved it onto c's new home. */
    CL_GC_PROTECT(x);
    CL_GC_PROTECT(x);

    cl_gc_compact();

    ASSERT(CL_CONS_P(x));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(x)), 888);
    ASSERT(CL_CONS_P(c));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(c)), 777);

    CL_GC_UNPROTECT(3);
}

static CL_Obj static_root_g;

TEST(global_root_plus_protect_survives_compaction)
{
    CL_Obj g1, c;

    cl_gc_compact();

    g1 = cl_cons(CL_MAKE_FIXNUM(111), CL_NIL);
    c  = cl_cons(CL_MAKE_FIXNUM(777), CL_NIL);
    static_root_g = cl_cons(CL_MAKE_FIXNUM(999), CL_NIL);
    g1 = CL_NIL;

    /* The hazard: one slot reachable from global_roots[] AND the thread
     * root stack.  Pre-fix each registry forwarded it once → twice total. */
    cl_gc_register_root(&static_root_g);
    CL_GC_PROTECT(c);
    CL_GC_PROTECT(static_root_g);

    cl_gc_compact();

    ASSERT(CL_CONS_P(static_root_g));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(static_root_g)), 999);
    ASSERT(CL_CONS_P(c));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(c)), 777);

    CL_GC_UNPROTECT(2);
    static_root_g = CL_NIL;   /* stays registered; keep it a harmless NIL */
}

TEST(protect_of_vm_stack_slot_is_harmless)
{
    CL_Obj g1, c, x;

    cl_gc_compact();

    g1 = cl_cons(CL_MAKE_FIXNUM(111), CL_NIL);
    c  = cl_cons(CL_MAKE_FIXNUM(777), CL_NIL);
    x  = cl_cons(CL_MAKE_FIXNUM(888), CL_NIL);
    g1 = CL_NIL;

    /* The hazard fixed at the CL_GC_PROTECT(args[i]) call sites: builtin
     * args live on the VM stack, which gc_update_thread_roots forwards
     * wholesale — protecting one of those slots registered it a second
     * time.  The dedup pass must skip root entries aliasing live VM-stack
     * slots. */
    cl_vm_push(x);
    CL_GC_PROTECT(c);
    CL_GC_PROTECT(cl_vm.stack[cl_vm.sp - 1]);

    cl_gc_compact();

    ASSERT(CL_CONS_P(cl_vm.stack[cl_vm.sp - 1]));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(cl_vm.stack[cl_vm.sp - 1])), 888);

    CL_GC_UNPROTECT(2);
    cl_vm.sp--;
}

/* --- 2. List builtins: cursors must survive a compacting :test -------- */

/* Every comparison forces a full moving compaction, so any unprotected
 * C-local cursor/key/test in the builtin goes stale immediately.  The
 * allocations after the compaction land at the new bump pointer — on top
 * of the "ghost" bytes the moved objects left behind — so a stale cursor
 * reads clobbered garbage instead of a still-intact old copy. */
#define COMPACTING_EQL \
    "(lambda (a b) (ext:gc-compact)" \
    " (make-string 8192) (make-string 8192) (make-string 8192)" \
    " (make-string 8192) (make-string 8192) (make-string 8192)" \
    " (eql a b))"

TEST(assoc_compacting_test)
{
    ASSERT_STR_EQ(eval_print(
        "(assoc 5 (list (cons 1 10) (cons 3 30) (cons 5 50) (cons 7 70))"
        " :test " COMPACTING_EQL ")"),
        "(5 . 50)");
    /* Miss path: the cursor walks the whole list under compaction. */
    ASSERT_STR_EQ(eval_print(
        "(assoc 9 (list (cons 1 10) (cons 3 30) (cons 5 50))"
        " :test " COMPACTING_EQL ")"),
        "NIL");
}

TEST(rassoc_compacting_test)
{
    ASSERT_STR_EQ(eval_print(
        "(rassoc 30 (list (cons 1 10) (cons 3 30) (cons 5 50))"
        " :test " COMPACTING_EQL ")"),
        "(3 . 30)");
}

TEST(subst_compacting_test)
{
    ASSERT_STR_EQ(eval_print(
        "(subst 'x 'y (list 'a (list 'y 'b (list 'y)) 'c)"
        " :test " COMPACTING_EQL ")"),
        "(A (X B (X)) C)");
}

TEST(sublis_compacting_test)
{
    ASSERT_STR_EQ(eval_print(
        "(sublis (list (cons 'a 1) (cons 'b 2)) (list 'a (list 'b) 'c)"
        " :test " COMPACTING_EQL ")"),
        "(1 (2) C)");
}

TEST(adjoin_compacting_test)
{
    ASSERT_STR_EQ(eval_print(
        "(adjoin 3 (list 1 2 3 4) :test " COMPACTING_EQL ")"),
        "(1 2 3 4)");
    ASSERT_STR_EQ(eval_print(
        "(adjoin 9 (list 1 2 3) :test " COMPACTING_EQL ")"),
        "(9 1 2 3)");
}

/* bi_acons takes no callback to hook a mid-call (ext:gc-compact) into, so
 * instead each call is sandwiched between a forced compaction and enough
 * throwaway allocation to make it a real, relocating one -- exercising the
 * same "args[] is VM-stack-rooted, no CL_GC_PROTECT needed" invariant the
 * other builtins above test, across acons's own two internal cl_cons calls
 * and across the alist argument threaded through repeated calls. */
TEST(acons_compacting_test)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((al (acons 1 10 nil)))"
        " (ext:gc-compact) (make-string 8192) (make-string 8192)"
        " (setf al (acons 3 30 al))"
        " (ext:gc-compact) (make-string 8192) (make-string 8192)"
        " (acons 5 50 al))"),
        "((5 . 50) (3 . 30) (1 . 10))");
}

int main(void)
{
    setup();

    RUN(double_protect_same_slot_survives_compaction);
    RUN(global_root_plus_protect_survives_compaction);
    RUN(protect_of_vm_stack_slot_is_harmless);
    RUN(assoc_compacting_test);
    RUN(rassoc_compacting_test);
    RUN(subst_compacting_test);
    RUN(sublis_compacting_test);
    RUN(adjoin_compacting_test);
    RUN(acons_compacting_test);

    teardown();
    REPORT();
}

#else /* !CL_GC_CLASSIC */

TEST(classic_backend_not_built) { ASSERT(1); }
int main(void) { test_init(); RUN(classic_backend_not_built); REPORT(); }

#endif /* CL_GC_CLASSIC */
