#include "test.h"
#include "core/types.h"
#include "core/mem.h"
#include "core/error.h"
#include "core/package.h"
#include "core/symbol.h"
#include "core/thread.h"
#include "core/vm.h"
#include "platform/platform.h"

#include <stdlib.h>
#include <string.h>

/*
 * Tests for the GC root enumeration in slot-visitor form (gc_trace.c:
 * gc_visit_thread_roots / gc_visit_registered_roots / gc_visit_shared_roots).
 *
 * The root set is enumerated ONCE and instantiated by the marker ("mark the
 * value") and by the compactor ("forward the slot in place"), so the two can
 * never disagree on what a root is.  What the tests pin down is the contract
 * that makes that safe:
 *
 *  - every live root slot is visited, and visited EXACTLY once per walk —
 *    gc_forward is not idempotent, so a slot reached twice is forwarded
 *    twice and ends up pointing at an unrelated object;
 *  - the stale words that are NOT object starts (nlx_stack[i].mv_values,
 *    pre_call_mv_values, pending_mv_values beyond the count or while no
 *    throw is in flight) are never visited — marking one sets a mark bit in
 *    the middle of some object, forwarding one rewrites garbage;
 *  - a CL_GC_PROTECT / cl_gc_register_root of a slot the thread or shared
 *    walk already covers is recognised as such (root_slot_independently_
 *    forwarded) and skipped by the compactor's registered-root pass — the
 *    end-to-end tests below protect such a slot and compact; on the tree
 *    before the unification the thread-slot one came back pointing at an
 *    unrelated object.
 */

typedef void (*gc_slot_fn)(CL_Obj *slot);
extern void gc_visit_roots(gc_slot_fn visit);
extern void gc_visit_thread_roots(CL_Thread *t, gc_slot_fn visit);
extern void gc_visit_registered_roots(gc_slot_fn visit);
extern void gc_visit_shared_roots(gc_slot_fn visit);
extern int  root_slot_independently_forwarded(CL_Obj *slot);
extern CL_Obj macro_table;   /* compiler.c — a shared-root table global */

#undef gc_root_count         /* thread.h macros shadow the t-> members */
#undef pending_lambda_name

static void setup(void)
{
    platform_init();
    cl_thread_init();
    cl_error_init();
    cl_mem_init(CL_DEFAULT_HEAP_SIZE);
    cl_package_init();
    cl_symbol_init();
}

static void teardown(void)
{
    cl_mem_shutdown();
    platform_shutdown();
}

/* --- Recording visitor ------------------------------------------------- */

#define REC_CAP 65536
static CL_Obj  *rec[REC_CAP];
static uint32_t rec_n;
static int      rec_overflow;

static void rec_visit(CL_Obj *slot)
{
    if (rec_n < REC_CAP) rec[rec_n++] = slot;
    else rec_overflow = 1;
}

static void rec_reset(void)
{
    rec_n = 0;
    rec_overflow = 0;
}

static uint32_t rec_count(const CL_Obj *slot)
{
    uint32_t i, n = 0;
    for (i = 0; i < rec_n; i++)
        if (rec[i] == slot) n++;
    return n;
}

static int slot_cmp(const void *a, const void *b)
{
    CL_Obj * const *pa = (CL_Obj * const *)a;
    CL_Obj * const *pb = (CL_Obj * const *)b;
    if (*pa < *pb) return -1;
    if (*pa > *pb) return 1;
    return 0;
}

/* Number of addresses the recorded walk visited more than once. */
static uint32_t rec_duplicates(void)
{
    uint32_t i, dups = 0;
    qsort(rec, rec_n, sizeof(CL_Obj *), slot_cmp);
    for (i = 1; i < rec_n; i++)
        if (rec[i] == rec[i - 1]) dups++;
    return dups;
}

/* --- A protected C local is a root, exactly once --- */

TEST(protected_local_visited_once)
{
    CL_Obj x = cl_cons(CL_MAKE_FIXNUM(1), CL_NIL);
    CL_Obj y = CL_NIL;

    rec_reset();
    gc_visit_roots(rec_visit);
    ASSERT(!rec_overflow);
    ASSERT_EQ_INT(rec_count(&x), 0);      /* not protected: not a root */

    CL_GC_PROTECT(x);
    rec_reset();
    gc_visit_roots(rec_visit);
    ASSERT_EQ_INT(rec_count(&x), 1);
    ASSERT_EQ_INT(rec_count(&y), 0);

    /* The registered part alone sees it; the thread part alone does not. */
    rec_reset();
    gc_visit_registered_roots(rec_visit);
    ASSERT_EQ_INT(rec_count(&x), 1);
    rec_reset();
    gc_visit_thread_roots(CT, rec_visit);
    ASSERT_EQ_INT(rec_count(&x), 0);

    CL_GC_UNPROTECT(1);
    rec_reset();
    gc_visit_roots(rec_visit);
    ASSERT_EQ_INT(rec_count(&x), 0);
}

/* --- A cl_gc_register_root'ed global is a root, exactly once --- */

static CL_Obj registered_global;

TEST(registered_global_visited_once)
{
    rec_reset();
    gc_visit_roots(rec_visit);
    ASSERT_EQ_INT(rec_count(&registered_global), 0);

    cl_gc_register_root(&registered_global);
    rec_reset();
    gc_visit_roots(rec_visit);
    ASSERT_EQ_INT(rec_count(&registered_global), 1);
    /* (There is no unregister; the slot stays a root for the process.) */
}

/* --- The per-thread fields are all in the walk, each once --- */

TEST(thread_fields_visited_once)
{
    CL_Thread *t = CT;
    rec_reset();
    gc_visit_thread_roots(t, rec_visit);
    ASSERT(!rec_overflow);

    ASSERT_EQ_INT(rec_count(&t->mv_values[0]), 1);
    ASSERT_EQ_INT(rec_count(&t->mv_values[CL_MAX_MV - 1]), 1);
    ASSERT_EQ_INT(rec_count(&t->pending_tag), 1);
    ASSERT_EQ_INT(rec_count(&t->pending_value), 1);
    ASSERT_EQ_INT(rec_count(&t->pending_lambda_name), 1);
    ASSERT_EQ_INT(rec_count(&t->callback_error), 1);
    ASSERT_EQ_INT(rec_count(&t->last_condition), 1);
    ASSERT_EQ_INT(rec_count(&t->name), 1);
    ASSERT_EQ_INT(rec_count(&t->result), 1);
    ASSERT_EQ_INT(rec_count(&t->interrupt_func), 1);
    ASSERT_EQ_INT(rec_count(&t->thread_obj), 1);
    ASSERT_EQ_INT(rec_count(&t->wait_obj), 1);
    ASSERT_EQ_INT(rec_count(&t->wait_lock), 1);
    ASSERT_EQ_INT(rec_count(&t->current_lex_env), 1);
    ASSERT_EQ_INT(rec_count(&t->current_package), 1);
    ASSERT_EQ_INT(rec_count(&t->rd_stream), 1);
    ASSERT_EQ_INT(rec_count(&t->rd_uninterned), 1);
    ASSERT_EQ_INT(rec_count(&t->rd_labels), 1);
    ASSERT_EQ_INT(rec_count(&t->pr_stream), 1);

    /* Bounded regions: nothing above the top is a root. */
    ASSERT_EQ_INT(rec_count(&t->dyn_stack[t->dyn_top].symbol), 0);
    ASSERT_EQ_INT(rec_count(&t->handler_stack[t->handler_top].handler), 0);
    ASSERT_EQ_INT(rec_count(&t->restart_stack[t->restart_top].restart), 0);
    ASSERT_EQ_INT(rec_count(&t->mv_save_buf[t->mv_save_top]), 0);
    ASSERT_EQ_INT(rec_count(&t->vm.stack[t->vm.sp]), 0);
}

/* --- The shared tables are in the walk, each once --- */

TEST(shared_tables_visited_once)
{
    rec_reset();
    gc_visit_shared_roots(rec_visit);
    ASSERT(!rec_overflow);
    ASSERT_EQ_INT(rec_count(&cl_package_registry), 1);
    ASSERT_EQ_INT(rec_count(&macro_table), 1);

    rec_reset();
    gc_visit_thread_roots(CT, rec_visit);
    ASSERT_EQ_INT(rec_count(&macro_table), 0);
}

/* --- Stale words that are not object starts are never visited --- */

TEST(stale_words_not_visited)
{
    CL_Thread *t = CT;
    int k = t->nlx_top;
    CL_NLXFrame *f = &t->nlx_stack[k];
    int saved_pending_throw = t->pending_throw;
    int saved_pending_count = t->pending_mv_count;

    ASSERT(k < t->nlx_max);
    /* A pushed NLX frame: tag/result/bytecode are live, mv_values is the
     * stale word the frame push leaves behind (an "offset" into nowhere). */
    f->tag = CL_NIL;
    f->result = CL_NIL;
    f->bytecode = CL_NIL;
    f->mv_count = 1;
    f->mv_values[0] = (CL_Obj)0xDEADBEEC;
    t->nlx_top = k + 1;

    /* pre_call_mv_values: a completed call's snapshot, never a root. */
    t->pre_call_mv_values[0] = (CL_Obj)0xDEADBEEC;

    rec_reset();
    gc_visit_thread_roots(t, rec_visit);
    ASSERT(!rec_overflow);
    ASSERT_EQ_INT(rec_count(&f->tag), 1);
    ASSERT_EQ_INT(rec_count(&f->result), 1);
    ASSERT_EQ_INT(rec_count(&f->bytecode), 1);
    ASSERT_EQ_INT(rec_count(&f->mv_values[0]), 0);
    ASSERT_EQ_INT(rec_count(&t->pre_call_mv_values[0]), 0);
    /* No throw in flight: the whole pending_mv_values array is stale. */
    ASSERT_EQ_INT(rec_count(&t->pending_mv_values[0]), 0);

    /* A throw in flight with two secondary values: exactly those two. */
    t->pending_throw = 1;
    t->pending_mv_count = 2;
    t->pending_mv_values[0] = CL_NIL;
    t->pending_mv_values[1] = CL_NIL;
    rec_reset();
    gc_visit_thread_roots(t, rec_visit);
    ASSERT_EQ_INT(rec_count(&t->pending_mv_values[0]), 1);
    ASSERT_EQ_INT(rec_count(&t->pending_mv_values[1]), 1);
    ASSERT_EQ_INT(rec_count(&t->pending_mv_values[2]), 0);

    t->pending_throw = saved_pending_throw;
    t->pending_mv_count = saved_pending_count;
    t->pre_call_mv_values[0] = CL_NIL;
    t->nlx_top = k;
}

/* --- No address is visited twice by one full walk --- */

TEST(no_slot_visited_twice)
{
    CL_Obj x = cl_cons(CL_MAKE_FIXNUM(2), CL_NIL);
    CL_GC_PROTECT(x);

    rec_reset();
    gc_visit_roots(rec_visit);
    ASSERT(!rec_overflow);
    ASSERT(rec_n > 0);
    ASSERT_EQ_INT(rec_duplicates(), 0);

    CL_GC_UNPROTECT(1);
}

/* --- Alias detection for the registered-root dedup --- */

TEST(alias_detection_follows_the_walk)
{
    CL_Obj local = CL_NIL;
    CL_Thread *t = CT;

    /* Slots the thread / shared walks own ... */
    ASSERT_EQ_INT(root_slot_independently_forwarded(&t->pending_tag), 1);
    ASSERT_EQ_INT(root_slot_independently_forwarded(&t->mv_values[3]), 1);
    ASSERT_EQ_INT(root_slot_independently_forwarded(&t->name), 1);
    ASSERT_EQ_INT(root_slot_independently_forwarded(&t->rd_stream), 1);
    ASSERT_EQ_INT(root_slot_independently_forwarded(&macro_table), 1);
    ASSERT_EQ_INT(root_slot_independently_forwarded(&cl_package_registry), 1);
    /* ... and the ones they do not. */
    ASSERT_EQ_INT(root_slot_independently_forwarded(&local), 0);
    ASSERT_EQ_INT(root_slot_independently_forwarded(&t->pre_call_mv_values[0]), 0);
    ASSERT_EQ_INT(root_slot_independently_forwarded(&t->vm.stack[t->vm.sp]), 0);
    ASSERT_EQ_INT(root_slot_independently_forwarded(&t->dyn_stack[t->dyn_top].old_value), 0);
    ASSERT_EQ_INT(root_slot_independently_forwarded(&registered_global), 0);
}

/* --- End to end: a redundant protect of a walked slot survives compaction ---
 *
 * Layout: [a][g][b][garbage...][x], all conses (same size), so that x's
 * slid-to offset coincides with the OLD offset of a live object: a second
 * gc_forward of x's slot then maps it through that object's forwarding
 * entry and the slot points somewhere else.  pending_tag is forwarded by
 * the thread walk; the CL_GC_PROTECT of the same slot must therefore be
 * skipped by the registered-root pass. */

TEST(aliased_protect_survives_compaction)
{
    CL_Thread *t = CT;
    CL_Obj a, b;
    int i;

    a = cl_cons(CL_MAKE_FIXNUM(10), CL_NIL);
    CL_GC_PROTECT(a);
    cl_cons(CL_MAKE_FIXNUM(99), CL_NIL);              /* g: garbage */
    b = cl_cons(CL_MAKE_FIXNUM(20), CL_NIL);
    CL_GC_PROTECT(b);
    for (i = 0; i < 64; i++)
        cl_cons(CL_MAKE_FIXNUM(99), CL_NIL);
    t->pending_tag = cl_cons(CL_MAKE_FIXNUM(30), CL_NIL);   /* x */
    CL_GC_PROTECT(t->pending_tag);                      /* the redundant one */

    cl_gc_compact();

    ASSERT(CL_CONS_P(a));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(a)), 10);
    ASSERT(CL_CONS_P(b));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(b)), 20);
    ASSERT(CL_CONS_P(t->pending_tag));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(t->pending_tag)), 30);
    ASSERT(t->pending_tag != b);

    CL_GC_UNPROTECT(3);
    t->pending_tag = CL_NIL;
}

/* --- The same for a protect of a shared table global --- */

TEST(aliased_global_protect_survives_compaction)
{
    CL_Obj saved = macro_table;
    CL_Obj a, b;
    int i;

    CL_GC_PROTECT(saved);
    a = cl_cons(CL_MAKE_FIXNUM(10), CL_NIL);
    CL_GC_PROTECT(a);
    cl_cons(CL_MAKE_FIXNUM(99), CL_NIL);
    b = cl_cons(CL_MAKE_FIXNUM(20), CL_NIL);
    CL_GC_PROTECT(b);
    for (i = 0; i < 64; i++)
        cl_cons(CL_MAKE_FIXNUM(99), CL_NIL);
    macro_table = cl_cons(CL_MAKE_FIXNUM(30), CL_NIL);
    CL_GC_PROTECT(macro_table);

    cl_gc_compact();

    ASSERT(CL_CONS_P(macro_table));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(macro_table)), 30);
    ASSERT(macro_table != b);

    CL_GC_UNPROTECT(4);
    macro_table = saved;
}

int main(void)
{
    setup();

    RUN(protected_local_visited_once);
    RUN(registered_global_visited_once);
    RUN(thread_fields_visited_once);
    RUN(shared_tables_visited_once);
    RUN(stale_words_not_visited);
    RUN(no_slot_visited_twice);
    RUN(alias_detection_follows_the_walk);
    RUN(aliased_protect_survives_compaction);
    RUN(aliased_global_protect_survives_compaction);

    teardown();
    REPORT();
}
