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
#include "platform/platform.h"

/* Reader-GF fast dispatch lives in its own test image on purpose.
 *
 * A GF whose whole method set is DEFCLASS-generated readers for one slot is
 * "promoted": its inline cache holds up to 4 (TYPE-NAME . SLOT-INDEX)
 * entries and OP_CALL answers the call directly, without unwrapping to the
 * discriminating function.  Promotion requires
 * *SLOT-ACCESS-PROTOCOL-EXTENDED-P* to be NIL.
 *
 * That flag is a ONE-WAY LATCH: defining any SLOT-VALUE-USING-CLASS method
 * sets it for the life of the image, and REMOVE-METHOD does not clear it.
 * test_clos.c defines such a method partway through, so every reader GF in
 * that image is permanently demoted and these tests would pass vacuously on
 * the slow path.  Hence a separate binary.
 *
 * AMILISP:*READER-GFS* records promotion, so each test below asserts the fast
 * path is genuinely engaged before exercising it.
 */

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
    cl_repl_init();
}

static void teardown(void)
{
    cl_mem_shutdown();
    platform_shutdown();
}

/* Helper: eval a string, return printed result */
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

TEST(reader_gf_promoted_and_reads)
{
    eval_print("(defclass rg-p () ((x :initform 1 :reader rg-px)))");
    ASSERT_STR_EQ(eval_print("(and (gethash #'rg-px amilisp:*reader-gfs*) t)"), "T");
    ASSERT_STR_EQ(eval_print("(rg-px (make-instance 'rg-p))"), "1");
}

TEST(reader_gf_polymorphic_differing_slot_indices)
{
    /* Same reader on two classes where X sits at a DIFFERENT index.  A cache
     * keyed on a captured index rather than the receiver's type would return
     * the wrong slot for one of them. */
    eval_print("(defclass rg-q () ((pad :initform 0) (x :initform 2 :reader rg-px)))");
    ASSERT_STR_EQ(eval_print("(rg-px (make-instance 'rg-q))"), "2");
    ASSERT_STR_EQ(eval_print("(rg-px (make-instance 'rg-p))"), "1");
    ASSERT_STR_EQ(eval_print(
        "(let ((a (make-instance 'rg-p)) (b (make-instance 'rg-q)) (acc nil))"
        "  (dotimes (i 3) (push (rg-px a) acc) (push (rg-px b) acc))"
        "  (nreverse acc))"), "(1 2 1 2 1 2)");
}

TEST(reader_gf_polymorphic_ic_caches_all_classes)
{
    /* The IC is a list of (TYPE-NAME . SLOT-INDEX) entries.  After one call
     * per class both classes must be cached, and further alternation must be
     * all hits: a hit never rewrites slot 8, so the spine staying EQ proves
     * the fast path answered every call (the old monomorphic cache rewrote
     * the IC on every alternation). */
    eval_print("(defclass rg-ppa () ((x :initform :a :reader rg-ppx)))");
    eval_print("(defclass rg-ppb () ((pad :initform 0) (x :initform :b :reader rg-ppx)))");
    ASSERT_STR_EQ(eval_print("(and (gethash #'rg-ppx amilisp:*reader-gfs*) t)"), "T");
    ASSERT_STR_EQ(eval_print(
        "(let ((a (make-instance 'rg-ppa)) (b (make-instance 'rg-ppb)))"
        "  (rg-ppx a) (rg-ppx b)"
        "  (let ((ic (amilisp::gf-inline-cache #'rg-ppx)) (acc nil))"
        "    (dotimes (i 5) (push (rg-ppx a) acc) (push (rg-ppx b) acc))"
        "    (list (length ic)"
        "          (eq ic (amilisp::gf-inline-cache #'rg-ppx))"
        "          (remove-duplicates (nreverse acc)))))"),
        "(2 T (:A :B))");
}

TEST(reader_gf_polymorphic_eviction_cap)
{
    /* Five receiver classes exceed the 4-entry cap: the oldest entry is
     * evicted, the IC never grows past 4, and every class still reads its
     * own value (evicted classes just take the slow path again). */
    eval_print("(defclass rg-e1 () ((x :initform 1 :reader rg-ex)))");
    eval_print("(defclass rg-e2 () ((x :initform 2 :reader rg-ex)))");
    eval_print("(defclass rg-e3 () ((x :initform 3 :reader rg-ex)))");
    eval_print("(defclass rg-e4 () ((x :initform 4 :reader rg-ex)))");
    eval_print("(defclass rg-e5 () ((pad :initform 0) (x :initform 5 :reader rg-ex)))");
    ASSERT_STR_EQ(eval_print(
        "(let ((os (list (make-instance 'rg-e1) (make-instance 'rg-e2)"
        "                (make-instance 'rg-e3) (make-instance 'rg-e4)"
        "                (make-instance 'rg-e5)))"
        "      (acc nil) (maxlen 0))"
        "  (dotimes (i 3)"
        "    (dolist (o os)"
        "      (push (rg-ex o) acc)"
        "      (setq maxlen (max maxlen"
        "                        (length (amilisp::gf-inline-cache #'rg-ex))))))"
        "  (list maxlen (nreverse acc)))"),
        "(4 (1 2 3 4 5 1 2 3 4 5 1 2 3 4 5))");
}

TEST(reader_gf_polymorphic_unbound_mix)
{
    /* One class's slot bound, the other's unbound, alternating: the bound
     * class must keep hitting its IC entry while the unbound one signals
     * UNBOUND-SLOT every time — an unbound read is a probe miss and must
     * never install a wrong entry that masks the next signal. */
    eval_print("(defclass rg-mb () ((x :initform :ok :reader rg-mx)))");
    eval_print("(defclass rg-mu () ((x :reader rg-mx)))");
    ASSERT_STR_EQ(eval_print(
        "(let ((b (make-instance 'rg-mb)) (u (make-instance 'rg-mu)) (acc nil))"
        "  (dotimes (i 3)"
        "    (push (rg-mx b) acc)"
        "    (push (handler-case (rg-mx u) (unbound-slot () :unbound)) acc))"
        "  (nreverse acc))"),
        "(:OK :UNBOUND :OK :UNBOUND :OK :UNBOUND)");
}

TEST(reader_gf_inherited_by_subclass)
{
    eval_print("(defclass rg-r (rg-p) ((extra :initform 9)))");
    ASSERT_STR_EQ(eval_print("(rg-px (make-instance 'rg-r))"), "1");
}

TEST(reader_gf_nil_value_is_not_unbound)
{
    /* The VM path has no caller-supplied MISS sentinel; a slot holding NIL
     * must not be mistaken for the unbound marker. */
    eval_print("(defclass rg-niln () ((x :initform nil :reader rg-nilx)))");
    ASSERT_STR_EQ(eval_print("(and (gethash #'rg-nilx amilisp:*reader-gfs*) t)"), "T");
    ASSERT_STR_EQ(eval_print("(rg-nilx (make-instance 'rg-niln))"), "NIL");
    ASSERT_STR_EQ(eval_print("(rg-nilx (make-instance 'rg-niln))"), "NIL");
}

TEST(reader_gf_unbound_slot_signals)
{
    /* An unbound slot's storage holds the marker, which reads back as a cache
     * miss and routes to the SLOT-UNBOUND protocol.  It must signal an
     * UNBOUND-SLOT naming the slot and the instance every time, and never leak
     * the marker as a value.  Signalled twice: cold cache, then warm. */
    eval_print("(defclass rg-u () ((x :reader rg-ux)))");
    ASSERT_STR_EQ(eval_print("(and (gethash #'rg-ux amilisp:*reader-gfs*) t)"), "T");
    ASSERT_STR_EQ(eval_print(
        "(let ((o (make-instance 'rg-u))) "
        "  (handler-case (rg-ux o) "
        "    (unbound-slot (c) (list (cell-error-name c) "
        "                            (eq (unbound-slot-instance c) o)))))"), "(X T)");
    ASSERT_STR_EQ(eval_print(
        "(let ((o (make-instance 'rg-u))) "
        "  (handler-case (rg-ux o) "
        "    (unbound-slot (c) (list (cell-error-name c) "
        "                            (eq (unbound-slot-instance c) o)))))"), "(X T)");
    /* ... and reads normally once bound. */
    ASSERT_STR_EQ(eval_print(
        "(let ((o (make-instance 'rg-u)))"
        "  (setf (slot-value o 'x) :bound) (rg-ux o))"), ":BOUND");
}

TEST(reader_gf_no_applicable_method)
{
    eval_print("(defclass rg-unrelated () ((x :initform 5)))");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (rg-px (make-instance 'rg-unrelated)) (error () :nam))"), ":NAM");
    ASSERT_STR_EQ(eval_print("(rg-px (make-instance 'rg-p))"), "1");
}

TEST(reader_gf_around_method_demotes)
{
    eval_print("(defclass rg-s () ((x :initform 10 :reader rg-sx)))");
    ASSERT_STR_EQ(eval_print("(rg-sx (make-instance 'rg-s))"), "10");
    eval_print("(defmethod rg-sx :around ((o rg-s)) (* 100 (call-next-method)))");
    ASSERT_STR_EQ(eval_print("(and (gethash #'rg-sx amilisp:*reader-gfs*) t)"), "NIL");
    ASSERT_STR_EQ(eval_print("(rg-sx (make-instance 'rg-s))"), "1000");
}

TEST(reader_gf_foreign_primary_demotes)
{
    eval_print("(defclass rg-t1 () ((x :initform 7 :reader rg-tx)))");
    ASSERT_STR_EQ(eval_print("(rg-tx (make-instance 'rg-t1))"), "7");
    eval_print("(defclass rg-t2 () ())");
    eval_print("(defmethod rg-tx ((o rg-t2)) :from-t2)");
    ASSERT_STR_EQ(eval_print("(and (gethash #'rg-tx amilisp:*reader-gfs*) t)"), "NIL");
    ASSERT_STR_EQ(eval_print("(rg-tx (make-instance 'rg-t2))"), ":FROM-T2");
    ASSERT_STR_EQ(eval_print("(rg-tx (make-instance 'rg-t1))"), "7");
}

TEST(reader_gf_accessor_writer_roundtrip)
{
    eval_print("(defclass rg-w () ((x :initform 1 :accessor rg-wx)))");
    ASSERT_STR_EQ(eval_print(
        "(let ((o (make-instance 'rg-w)))"
        "  (list (rg-wx o) (progn (setf (rg-wx o) 42) (rg-wx o))))"), "(1 42)");
}

TEST(reader_gf_class_redefinition_reorders_slots)
{
    /* The cache stores a slot INDEX.  Redefining the class so the slot moves
     * must invalidate it, or the reader returns the wrong slot. */
    eval_print("(defclass rg-redef () ((a :initform :one :reader rg-redef-a)))");
    ASSERT_STR_EQ(eval_print("(rg-redef-a (make-instance 'rg-redef))"), ":ONE");
    eval_print("(defclass rg-redef () ((pad :initform :pad)"
               "                       (a :initform :two :reader rg-redef-a)))");
    ASSERT_STR_EQ(eval_print("(rg-redef-a (make-instance 'rg-redef))"), ":TWO");
}

TEST(reader_gf_tailcall_funcall_and_apply_paths)
{
    /* OP_TAILCALL is excluded from the VM fast path (it must unwind the
     * caller's frame); FUNCALL / APPLY answer from their own probes (see
     * reader_gf_trampoline_probes below).  All three must still produce
     * the right value. */
    eval_print("(defclass rg-tc () ((x :initform :val :reader rg-tcx)))");
    eval_print("(defun rg-tail-read (o) (rg-tcx o))");
    ASSERT_STR_EQ(eval_print("(rg-tail-read (make-instance 'rg-tc))"), ":VAL");
    ASSERT_STR_EQ(eval_print("(funcall #'rg-tcx (make-instance 'rg-tc))"), ":VAL");
    ASSERT_STR_EQ(eval_print("(apply #'rg-tcx (list (make-instance 'rg-tc)))"), ":VAL");
}

TEST(reader_gf_trampoline_probes)
{
    /* The C trampolines probe the reader IC before unwrapping the GF to its
     * discriminating function: cl_vm_apply (the path every m68k-JIT'd call
     * takes via cl_jit_runtime_call), bi_funcall, bi_apply, the VM's inline
     * OP_APPLY (compiled (apply ...) forms), and the sequence helpers'
     * call_1.  MAPCAR itself unwraps at designator-coercion time and
     * reaches the (also cached) Lisp discriminator — included here for value
     * agreement across paths.  A hit never rewrites slot 8, so the spine
     * staying EQ across the funcall/apply calls proves they were answered by
     * a cache hit, not by slow dispatch re-installing entries. */
    eval_print("(defclass rg-tp1 () ((x :initform 1 :reader rg-tpx)"
               "                     (u :reader rg-tpu)))");
    eval_print("(defclass rg-tp2 () ((pad :initform 0)"
               "                     (x :initform 2 :reader rg-tpx)))");
    ASSERT_STR_EQ(eval_print("(and (gethash #'rg-tpx amilisp:*reader-gfs*) t)"), "T");
    ASSERT_STR_EQ(eval_print(
        "(let ((a (make-instance 'rg-tp1)) (b (make-instance 'rg-tp2)))"
        "  (rg-tpx a) (rg-tpx b)"  /* fill the IC for both classes */
        "  (let ((ic (amilisp::gf-inline-cache #'rg-tpx)) (acc nil))"
        "    (dotimes (i 5)"
        "      (push (funcall #'rg-tpx a) acc)"
        "      (push (apply #'rg-tpx (list b)) acc))"
        "    (list (eq ic (amilisp::gf-inline-cache #'rg-tpx))"
        "          (remove-duplicates (nreverse acc))"
        "          (mapcar #'rg-tpx (list a b a b)))))"),
        "(T (1 2) (1 2 1 2))");
    /* a probe hit must leave exactly one value, like any reader call */
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (funcall #'rg-tpx (make-instance 'rg-tp1)))"), "(1)");
    /* indirect APPLY runs the bi_apply builtin (a compiled (apply ...) form
     * takes the inline OP_APPLY, which has its own probe) */
    ASSERT_STR_EQ(eval_print(
        "(funcall #'apply #'rg-tpx (list (make-instance 'rg-tp2)))"), "2");
    /* an unbound slot is a probe miss and must reach SLOT-UNBOUND */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (funcall #'rg-tpu (make-instance 'rg-tp1))"
        "  (unbound-slot (c) (cell-error-name c)))"), "U");
    /* demotion must disarm the trampoline probes too: after an :around
     * method, funcall/apply/mapcar have to run the full method chain */
    eval_print("(defmethod rg-tpx :around ((o rg-tp1)) (* 100 (call-next-method)))");
    ASSERT_STR_EQ(eval_print("(and (gethash #'rg-tpx amilisp:*reader-gfs*) t)"), "NIL");
    ASSERT_STR_EQ(eval_print(
        "(let ((a (make-instance 'rg-tp1)) (b (make-instance 'rg-tp2)))"
        "  (list (funcall #'rg-tpx a)"
        "        (apply #'rg-tpx (list a))"
        "        (mapcar #'rg-tpx (list a b))))"),
        "(100 100 (100 2))");
}

TEST(reader_gf_set_funcallable_instance_function_wins)
{
    /* OP_CALL bypasses slot 3 for a promoted reader GF, so retargeting slot 3
     * must first demote it -- otherwise FN would never run. */
    eval_print("(defclass rg-sfi () ((x :initform :orig :reader rg-sfix)))");
    ASSERT_STR_EQ(eval_print("(rg-sfix (make-instance 'rg-sfi))"), ":ORIG");
    eval_print("(set-funcallable-instance-function"
               "  #'rg-sfix (lambda (o) (declare (ignore o)) :hijacked))");
    ASSERT_STR_EQ(eval_print("(and (gethash #'rg-sfix amilisp:*reader-gfs*) t)"), "NIL");
    ASSERT_STR_EQ(eval_print("(rg-sfix (make-instance 'rg-sfi))"), ":HIJACKED");
    ASSERT_STR_EQ(eval_print("(rg-sfix (make-instance 'rg-sfi))"), ":HIJACKED");
    /* an unrelated reader keeps its fast path */
    ASSERT_STR_EQ(eval_print("(rg-px (make-instance 'rg-p))"), "1");
}

/* --- Writer-GF fast dispatch ---
 *
 * The mirror machinery for (setf (x obj) v): a GF whose whole method set is
 * DEFCLASS-generated writer methods for one slot is promoted, its IC entries
 * encode the slot index as (- -1 idx) — always negative — and OP_CALL's
 * 2-arg probe stores directly.  AMILISP:*WRITER-GFS* records promotion.
 * Same latch caveat as the reader tests: everything here must run before
 * reader_gf_slot_value_using_class_demotes_all. */

TEST(writer_gf_promoted_and_stores)
{
    eval_print("(defclass wg-p () ((x :initform 1 :accessor wg-px)))");
    ASSERT_STR_EQ(eval_print("(and (gethash #'(setf wg-px) amilisp:*writer-gfs*) t)"), "T");
    ASSERT_STR_EQ(eval_print(
        "(let ((o (make-instance 'wg-p)))"
        "  (list (wg-px o) (setf (wg-px o) 42) (wg-px o)))"), "(1 42 42)");
}

TEST(writer_gf_writer_option_promoted)
{
    /* :writer names the GF directly (no setf wrapper); it takes (VAL OBJ). */
    eval_print("(defclass wg-wo () ((x :initform 0 :writer wg-set-x :reader wg-wox)))");
    ASSERT_STR_EQ(eval_print("(and (gethash #'wg-set-x amilisp:*writer-gfs*) t)"), "T");
    ASSERT_STR_EQ(eval_print(
        "(let ((o (make-instance 'wg-wo)))"
        "  (list (wg-set-x 7 o) (wg-wox o)))"), "(7 7)");
}

TEST(writer_gf_polymorphic_differing_slot_indices)
{
    /* Same writer on two classes where X sits at a DIFFERENT index; the IC
     * caches one negative-encoded entry per class, and a hit never rewrites
     * slot 8 — the spine staying EQ proves the fast path stored them. */
    eval_print("(defclass wg-qa () ((x :initform 0 :accessor wg-qx)))");
    eval_print("(defclass wg-qb () ((pad :initform 0) (x :initform 0 :accessor wg-qx)))");
    ASSERT_STR_EQ(eval_print(
        "(let ((a (make-instance 'wg-qa)) (b (make-instance 'wg-qb)))"
        "  (setf (wg-qx a) 1) (setf (wg-qx b) 2)"
        "  (let ((ic (amilisp::gf-inline-cache #'(setf wg-qx))))"
        "    (dotimes (i 5) (setf (wg-qx a) i) (setf (wg-qx b) (* 10 i)))"
        "    (list (length ic)"
        "          (eq ic (amilisp::gf-inline-cache #'(setf wg-qx)))"
        "          (wg-qx a) (wg-qx b)"
        "          (slot-value b 'pad))))"),  /* neighbour slot untouched */
        "(2 T 4 40 0)");
}

TEST(writer_gf_arity_error_not_slot_read)
{
    /* The writer IC's negative index encoding exists so the 1-arg reader
     * probe can never answer a call to a promoted writer GF: a 1-arg call
     * must reach the slow path and signal, not return the slot's value. */
    eval_print("(defclass wg-ar () ((x :initform :leak :accessor wg-arx)))");
    eval_print("(let ((o (make-instance 'wg-ar))) (setf (wg-arx o) :leak))");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (funcall #'(setf wg-arx) (make-instance 'wg-ar))"
        "  (error () :err))"), ":ERR");
    /* ... and 3-arg calls must error too, not store */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (funcall #'(setf wg-arx) 1 (make-instance 'wg-ar) 2)"
        "  (error () :err))"), ":ERR");
}

TEST(writer_gf_setf_returns_value_and_single)
{
    eval_print("(defclass wg-rv () ((x :initform 0 :accessor wg-rvx)))");
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (setf (wg-rvx (make-instance 'wg-rv)) 5))"), "(5)");
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-list (funcall #'(setf wg-rvx) 6 (make-instance 'wg-rv)))"),
        "(6)");
}

TEST(writer_gf_unbound_slot_becomes_bound)
{
    /* Writing an unbound slot binds it — no SLOT-UNBOUND involvement. */
    eval_print("(defclass wg-u () ((x :accessor wg-ux)))");
    ASSERT_STR_EQ(eval_print(
        "(let ((o (make-instance 'wg-u)))"
        "  (list (slot-boundp o 'x) (setf (wg-ux o) :now) (wg-ux o)))"),
        "(NIL :NOW :NOW)");
}

TEST(writer_gf_trampoline_probes)
{
    /* funcall / apply / indirect apply answer from the C trampolines'
     * 2-arg writer probes; the spine staying EQ proves cache hits. */
    eval_print("(defclass wg-tp1 () ((x :initform 0 :accessor wg-tpx)))");
    eval_print("(defclass wg-tp2 () ((pad :initform 0) (x :initform 0 :accessor wg-tpx)))");
    ASSERT_STR_EQ(eval_print("(and (gethash #'(setf wg-tpx) amilisp:*writer-gfs*) t)"), "T");
    ASSERT_STR_EQ(eval_print(
        "(let ((a (make-instance 'wg-tp1)) (b (make-instance 'wg-tp2)))"
        "  (setf (wg-tpx a) 0) (setf (wg-tpx b) 0)"  /* fill the IC */
        "  (let ((ic (amilisp::gf-inline-cache #'(setf wg-tpx))))"
        "    (dotimes (i 5)"
        "      (funcall #'(setf wg-tpx) i a)"
        "      (apply #'(setf wg-tpx) (* 10 i) (list b)))"
        "    (funcall #'apply #'(setf wg-tpx) (list 99 a))"
        "    (list (eq ic (amilisp::gf-inline-cache #'(setf wg-tpx)))"
        "          (wg-tpx a) (wg-tpx b))))"),
        "(T 99 40)");
}

TEST(writer_gf_after_method_demotes)
{
    /* sento-style: an :after method on a setter must retract the fast path
     * — a probe hit would silently skip it. */
    eval_print("(defclass wg-s () ((x :initform 0 :accessor wg-sx)"
               "                   (log :initform nil :accessor wg-slog)))");
    eval_print("(let ((o (make-instance 'wg-s))) (setf (wg-sx o) 1))");
    eval_print("(defmethod (setf wg-sx) :after (val (o wg-s))"
               "  (push val (slot-value o 'log)))");
    ASSERT_STR_EQ(eval_print("(and (gethash #'(setf wg-sx) amilisp:*writer-gfs*) t)"), "NIL");
    ASSERT_STR_EQ(eval_print(
        "(let ((o (make-instance 'wg-s)))"
        "  (setf (wg-sx o) 7)"
        "  (funcall #'(setf wg-sx) 8 o)"          /* trampolines disarmed too */
        "  (list (wg-sx o) (wg-slog o)))"), "(8 (8 7))");
}

TEST(writer_gf_no_applicable_method)
{
    eval_print("(defclass wg-n () ((x :initform 0 :accessor wg-nx)))");
    eval_print("(defclass wg-unrelated () ((x :initform 5)))");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (setf (wg-nx (make-instance 'wg-unrelated)) 1)"
        "  (error () :nam))"), ":NAM");
    /* the unrelated class's slot was not written through the wrong IC */
    ASSERT_STR_EQ(eval_print(
        "(let ((o (make-instance 'wg-n))) (setf (wg-nx o) 3) (wg-nx o))"), "3");
}

TEST(writer_gf_class_redefinition_reorders_slots)
{
    /* The cache stores a slot INDEX; redefining the class so the slot moves
     * must invalidate it, or the store lands in the wrong slot. */
    eval_print("(defclass wg-redef () ((a :initform :one :accessor wg-redef-a)))");
    eval_print("(setf (wg-redef-a (make-instance 'wg-redef)) :warm)");
    eval_print("(defclass wg-redef () ((pad :initform :pad)"
               "                       (a :initform :two :accessor wg-redef-a)))");
    ASSERT_STR_EQ(eval_print(
        "(let ((o (make-instance 'wg-redef)))"
        "  (setf (wg-redef-a o) :new)"
        "  (list (wg-redef-a o) (slot-value o 'pad)))"), "(:NEW :PAD)");
}

TEST(writer_gf_set_funcallable_instance_function_wins)
{
    eval_print("(defclass wg-sfi () ((x :initform 0 :accessor wg-sfix)))");
    eval_print("(setf (wg-sfix (make-instance 'wg-sfi)) 1)");
    eval_print("(set-funcallable-instance-function"
               "  #'(setf wg-sfix) (lambda (v o) (declare (ignore v o)) :hijacked))");
    ASSERT_STR_EQ(eval_print("(and (gethash #'(setf wg-sfix) amilisp:*writer-gfs*) t)"), "NIL");
    ASSERT_STR_EQ(eval_print(
        "(funcall #'(setf wg-sfix) 9 (make-instance 'wg-sfi))"), ":HIJACKED");
    /* the hijacked function does not store — proof slot 3 was reached and
     * the old IC store path is disarmed (SETF itself returns the new value
     * by construction, so observe the slot instead) */
    ASSERT_STR_EQ(eval_print(
        "(let ((o (make-instance 'wg-sfi))) (setf (wg-sfix o) 9) (wg-sfix o))"), "0");
}

/* MUST BE THE LAST reader-GF test, and effectively the last test in this
 * file: defining a SLOT-VALUE-USING-CLASS method latches
 * *SLOT-ACCESS-PROTOCOL-EXTENDED-P* for the remainder of the image, demoting
 * every reader GF permanently. */
TEST(reader_gf_slot_value_using_class_demotes_all)
{
    ASSERT_STR_EQ(eval_print("amilisp:*slot-access-protocol-extended-p*"), "NIL");
    eval_print("(defclass rg-tracked () ((x :initform 3 :reader rg-tkx)))");
    eval_print("(defclass wg-tracked () ((x :initform 0 :accessor wg-tkx)))");
    ASSERT_STR_EQ(eval_print("(rg-tkx (make-instance 'rg-tracked))"), "3");
    eval_print("(setf (wg-tkx (make-instance 'wg-tracked)) 1)"); /* warm the IC */
    ASSERT_STR_EQ(eval_print("(and (gethash #'(setf wg-tkx) amilisp:*writer-gfs*) t)"), "T");
    eval_print("(defmethod slot-value-using-class :around"
               "    ((c standard-class) obj slotd)"
               "  (let ((v (call-next-method)))"
               "    (if (numberp v) (* 1000 v) v)))");
    ASSERT_STR_EQ(eval_print("amilisp:*slot-access-protocol-extended-p*"), "T");
    ASSERT_STR_EQ(eval_print("(and (gethash #'rg-tkx amilisp:*reader-gfs*) t)"), "NIL");
    ASSERT_STR_EQ(eval_print("(and (gethash #'rg-px amilisp:*reader-gfs*) t)"), "NIL");
    /* the latch demotes writer GFs too */
    ASSERT_STR_EQ(eval_print("(and (gethash #'(setf wg-tkx) amilisp:*writer-gfs*) t)"), "NIL");
    /* a demoted writer still stores (through the full protocol); the read
     * back shows the user's SLOT-VALUE-USING-CLASS method being honoured */
    ASSERT_STR_EQ(eval_print(
        "(let ((o (make-instance 'wg-tracked)))"
        "  (setf (wg-tkx o) 7) (wg-tkx o))"), "7000");
    /* the protocol is now honoured by readers, not bypassed */
    ASSERT_STR_EQ(eval_print("(rg-tkx (make-instance 'rg-tracked))"), "3000");
    ASSERT_STR_EQ(eval_print("(rg-px (make-instance 'rg-p))"), "1000");
    /* ... including through the trampoline probes (funcall/apply/mapcar) */
    ASSERT_STR_EQ(eval_print("(funcall #'rg-tkx (make-instance 'rg-tracked))"), "3000");
    ASSERT_STR_EQ(eval_print("(mapcar #'rg-px (list (make-instance 'rg-p)))"), "(1000)");
}

int main(void)
{
    test_init();
    setup();

    /* Order matters: reader_gf_slot_value_using_class_demotes_all latches
     * *SLOT-ACCESS-PROTOCOL-EXTENDED-P* for the rest of the image, demoting
     * every reader GF.  It must run last. */
    RUN(reader_gf_promoted_and_reads);
    RUN(reader_gf_polymorphic_differing_slot_indices);
    RUN(reader_gf_polymorphic_ic_caches_all_classes);
    RUN(reader_gf_polymorphic_eviction_cap);
    RUN(reader_gf_polymorphic_unbound_mix);
    RUN(reader_gf_inherited_by_subclass);
    RUN(reader_gf_nil_value_is_not_unbound);
    RUN(reader_gf_unbound_slot_signals);
    RUN(reader_gf_no_applicable_method);
    RUN(reader_gf_around_method_demotes);
    RUN(reader_gf_foreign_primary_demotes);
    RUN(reader_gf_accessor_writer_roundtrip);
    RUN(reader_gf_class_redefinition_reorders_slots);
    RUN(reader_gf_tailcall_funcall_and_apply_paths);
    RUN(reader_gf_trampoline_probes);
    RUN(reader_gf_set_funcallable_instance_function_wins);

    RUN(writer_gf_promoted_and_stores);
    RUN(writer_gf_writer_option_promoted);
    RUN(writer_gf_polymorphic_differing_slot_indices);
    RUN(writer_gf_arity_error_not_slot_read);
    RUN(writer_gf_setf_returns_value_and_single);
    RUN(writer_gf_unbound_slot_becomes_bound);
    RUN(writer_gf_trampoline_probes);
    RUN(writer_gf_after_method_demotes);
    RUN(writer_gf_no_applicable_method);
    RUN(writer_gf_class_redefinition_reorders_slots);
    RUN(writer_gf_set_funcallable_instance_function_wins);

    RUN(reader_gf_slot_value_using_class_demotes_all);

    teardown();
    REPORT();
}
