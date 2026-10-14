/* Tests for user-defined metaclass support (specs/mop.md Design Principle 3).
 *
 * Covers the two ecosystem patterns serapeum's mop.lisp exercises:
 *   - ABSTRACT-STANDARD-CLASS: a metaclass whose ALLOCATE-INSTANCE method
 *     refuses instantiation, so (make-instance abstract-class) signals.
 *   - TOPMOST-OBJECT-CLASS: a metaclass with a slot + :DEFAULT-INITARGS and an
 *     INITIALIZE-INSTANCE :AROUND that injects a superclass into the class it
 *     creates, so instances of that class are TYPEP the injected superclass.
 *
 * See tests/amiga/mop-metaclass-tests.lisp for the Amiga-side equivalents. */

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

/* === A metaclass is an instance of STANDARD-CLASS; its instances (classes)
 *     reuse the fixed 12-slot layout plus any metaclass slots === */

TEST(metaclass_defclass_registers_struct_type)
{
    /* A bare metaclass: subclass of standard-class, no extra slots. */
    eval_print("(defclass mc-abstract (standard-class) ())");
    /* Its struct type has the fixed 12 class slots. */
    ASSERT_STR_EQ(eval_print("(length (amilisp::%struct-slot-names 'mc-abstract))"),
                  "12");
}

TEST(metaclass_with_slot_has_13_slots)
{
    eval_print("(defclass mc-topclass (standard-class) "
               "  ((tc :initarg :tc :reader tc-of)))");
    /* 12 fixed + 1 metaclass slot. */
    ASSERT_STR_EQ(eval_print("(length (amilisp::%struct-slot-names 'mc-topclass))"),
                  "13");
}

TEST(class_with_metaclass_has_that_metaclass)
{
    eval_print("(defclass mc-meta1 (standard-class) ())");
    eval_print("(defclass mc-user1 () () (:metaclass mc-meta1))");
    /* (class-of (find-class 'mc-user1)) is the metaclass. */
    ASSERT_STR_EQ(eval_print("(class-name (class-of (find-class 'mc-user1)))"),
                  "MC-META1");
    /* A normal class is still an instance of STANDARD-CLASS. */
    eval_print("(defclass mc-normal1 () ())");
    ASSERT_STR_EQ(eval_print("(class-name (class-of (find-class 'mc-normal1)))"),
                  "STANDARD-CLASS");
}

/* === ABSTRACT-CLASS: allocate-instance on the metaclass refuses === */

TEST(abstract_class_make_instance_signals)
{
    eval_print("(defclass abstract-standard-class (standard-class) ())");
    eval_print("(defmethod allocate-instance ((a abstract-standard-class) &rest initargs)"
               "  (declare (ignore initargs))"
               "  (error \"Cannot allocate instances of abstract class ~s\""
               "         (class-name a)))");
    eval_print("(defclass abs-shape () () (:metaclass abstract-standard-class))");
    /* make-instance must signal (the error method on the metaclass fires). */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (progn (make-instance 'abs-shape) :no-error)"
        "  (error () :signalled))"),
        ":SIGNALLED");
}

TEST(abstract_metaclass_does_not_break_normal_classes)
{
    /* Defining allocate-instance on a metaclass must not stop normal
     * make-instance (the (class t) default still applies). */
    eval_print("(defclass abstract-standard-class (standard-class) ())");
    eval_print("(defmethod allocate-instance ((a abstract-standard-class) &rest initargs)"
               "  (declare (ignore initargs)) (error \"no\"))");
    eval_print("(defclass plain-pt () ((x :initarg :x :accessor plain-x)))");
    ASSERT_STR_EQ(eval_print("(plain-x (make-instance 'plain-pt :x 7))"), "7");
}

/* === TOPMOST-OBJECT: metaclass :around rewrites :direct-superclasses === */

TEST(topmost_object_superclass_injection)
{
    eval_print("(defclass my-topmost-object () ())");
    eval_print("(defclass topmost-object-class (standard-class)"
               "  ((topmost-class :initarg :topmost-class :reader topmost-class)))");
    eval_print("(defmethod validate-superclass "
               "  ((c1 topmost-object-class) (c2 standard-class)) t)");
    eval_print("(defun %supertypep (a b) (subtypep b a))");
    eval_print("(defun %insert-super (sc list)"
               "  (cond ((null list) (list sc))"
               "        ((subtypep sc (first list)) (cons sc list))"
               "        (t (cons (first list) (%insert-super sc (rest list))))))");
    eval_print("(defmethod initialize-instance :around"
               "  ((class topmost-object-class) &rest initargs"
               "   &key direct-superclasses topmost-class)"
               "  (if (find topmost-class direct-superclasses :test #'%supertypep)"
               "      (call-next-method)"
               "      (apply #'call-next-method class"
               "             :direct-superclasses"
               "             (%insert-super (find-class topmost-class) direct-superclasses)"
               "             initargs)))");
    eval_print("(defclass my-metaclass (topmost-object-class) ()"
               "  (:default-initargs :topmost-class 'my-topmost-object))");
    eval_print("(defclass my-class () () (:metaclass my-metaclass))");
    /* The metaclass injected my-topmost-object into my-class's supers, so an
     * instance of my-class is typep my-topmost-object. */
    ASSERT_STR_EQ(eval_print("(typep (make-instance 'my-class) 'my-topmost-object)"),
                  "T");
    /* The metaclass slot was populated from the :default-initargs. */
    ASSERT_STR_EQ(eval_print("(topmost-class (find-class 'my-class))"),
                  "MY-TOPMOST-OBJECT");
}

/* === CLHS 7.1.4: initform must be used when initarg is not supplied === */

TEST(metaclass_slot_initform_applied_when_initarg_absent)
{
    /* Metaclass with a slot that has :initarg :s and :initform 'dflt. */
    eval_print("(defclass mc-initform (standard-class)"
               "  ((s :initarg :s :initform 'dflt)))");
    /* Create a class via this metaclass WITHOUT supplying :s.
     * Per CLHS 7.1.4 the :initform must be evaluated and stored. */
    eval_print("(defclass mc-initform-user () () (:metaclass mc-initform))");
    ASSERT_STR_EQ(eval_print("(slot-value (find-class 'mc-initform-user) 's)"),
                  "DFLT");
    /* When :s IS supplied (via the metaclass's :default-initargs), that
     * value wins over the :initform.  A sub-metaclass supplies :s via
     * :default-initargs so the initform should NOT be used. */
    eval_print("(defclass mc-initform-with-default (mc-initform) ()"
               "  (:default-initargs :s 'supplied-dflt))");
    eval_print("(defclass mc-initform-supplied () ()"
               "  (:metaclass mc-initform-with-default))");
    ASSERT_STR_EQ(eval_print("(slot-value (find-class 'mc-initform-supplied) 's)"),
                  "SUPPLIED-DFLT");
}

int main(void)
{
    test_init();
    setup();

    RUN(metaclass_defclass_registers_struct_type);
    RUN(metaclass_with_slot_has_13_slots);
    RUN(class_with_metaclass_has_that_metaclass);
    RUN(abstract_class_make_instance_signals);
    RUN(abstract_metaclass_does_not_break_normal_classes);
    RUN(topmost_object_superclass_injection);
    RUN(metaclass_slot_initform_applied_when_initarg_absent);

    teardown();
    REPORT();
}
