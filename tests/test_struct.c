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

/* --- Basic defstruct --- */

TEST(struct_basic_define)
{
    ASSERT_STR_EQ(eval_print("(defstruct point (x 0) (y 0))"), "POINT");
}

TEST(struct_constructor)
{
    eval_print("(defstruct point2 (x 0) (y 0))");
    /* Default values */
    ASSERT_STR_EQ(eval_print("(point2-x (make-point2))"), "0");
    ASSERT_STR_EQ(eval_print("(point2-y (make-point2))"), "0");
    /* Keyword arguments */
    ASSERT_STR_EQ(eval_print("(point2-x (make-point2 :x 10 :y 20))"), "10");
    ASSERT_STR_EQ(eval_print("(point2-y (make-point2 :x 10 :y 20))"), "20");
}

TEST(struct_predicate)
{
    eval_print("(defstruct dog3 (name \"Rex\"))");
    ASSERT_STR_EQ(eval_print("(dog3-p (make-dog3))"), "T");
    ASSERT_STR_EQ(eval_print("(dog3-p 42)"), "NIL");
    ASSERT_STR_EQ(eval_print("(dog3-p nil)"), "NIL");
}

TEST(struct_setf_accessor)
{
    eval_print("(defstruct box4 (width 0) (height 0))");
    ASSERT_STR_EQ(eval_print(
        "(let ((b (make-box4 :width 5 :height 10)))"
        "  (setf (box4-width b) 99)"
        "  (box4-width b))"), "99");
}

TEST(struct_copier)
{
    eval_print("(defstruct item5 (val 0))");
    ASSERT_STR_EQ(eval_print(
        "(let ((a (make-item5 :val 42)))"
        "  (let ((b (copy-item5 a)))"
        "    (setf (item5-val b) 99)"
        "    (item5-val a)))"), "42");
}

TEST(struct_typep)
{
    eval_print("(defstruct thing6 (data nil))");
    ASSERT_STR_EQ(eval_print("(typep (make-thing6) 'thing6)"), "T");
    ASSERT_STR_EQ(eval_print("(typep 42 'thing6)"), "NIL");
}

TEST(struct_type_of)
{
    eval_print("(defstruct widget7 (label \"x\"))");
    ASSERT_STR_EQ(eval_print("(type-of (make-widget7))"), "WIDGET7");
}

TEST(struct_printer)
{
    eval_print("(defstruct pt8 (x 0) (y 0))");
    ASSERT_STR_EQ(eval_print("(make-pt8 :x 1 :y 2)"), "#S(PT8 :X 1 :Y 2)");
}

TEST(struct_structurep)
{
    eval_print("(defstruct s9 (a 1))");
    ASSERT_STR_EQ(eval_print("(structurep (make-s9))"), "T");
    ASSERT_STR_EQ(eval_print("(structurep 42)"), "NIL");
    ASSERT_STR_EQ(eval_print("(structurep nil)"), "NIL");
}

/* --- Options --- */

TEST(struct_conc_name)
{
    eval_print("(defstruct (color10 (:conc-name c-)) (r 0) (g 0) (b 0))");
    ASSERT_STR_EQ(eval_print("(c-r (make-color10 :r 255))"), "255");
    ASSERT_STR_EQ(eval_print("(c-g (make-color10 :g 128))"), "128");
}

TEST(struct_conc_name_nil)
{
    /* nil conc-name means no prefix */
    eval_print("(defstruct (bare11 (:conc-name nil)) (field 99))");
    ASSERT_STR_EQ(eval_print("(field (make-bare11))"), "99");
}

TEST(struct_constructor_name)
{
    eval_print("(defstruct (vec12 (:constructor new-vec)) (x 0) (y 0))");
    ASSERT_STR_EQ(eval_print("(vec12-x (new-vec :x 7))"), "7");
}

TEST(struct_constructor_nil)
{
    /* No constructor generated */
    eval_print("(defstruct (noc13 (:constructor nil)) (a 1))");
    /* make-noc13 should not exist */
    ASSERT(strncmp(eval_print("(make-noc13)"), "ERROR:", 6) == 0);
}

TEST(struct_predicate_name)
{
    eval_print("(defstruct (pred14 (:predicate is-pred14)) (x 0))");
    ASSERT_STR_EQ(eval_print("(is-pred14 (make-pred14))"), "T");
}

TEST(struct_predicate_nil)
{
    eval_print("(defstruct (nop15 (:predicate nil)) (x 0))");
    /* nop15-p should not exist */
    ASSERT(strncmp(eval_print("(nop15-p (make-nop15))"), "ERROR:", 6) == 0);
}

TEST(struct_copier_name)
{
    eval_print("(defstruct (cp16 (:copier clone-cp16)) (x 0))");
    ASSERT_STR_EQ(eval_print("(cp16-x (clone-cp16 (make-cp16 :x 42)))"), "42");
}

TEST(struct_copier_nil)
{
    eval_print("(defstruct (nocp17 (:copier nil)) (x 0))");
    /* copy-nocp17 should not exist */
    ASSERT(strncmp(eval_print("(copy-nocp17 (make-nocp17))"), "ERROR:", 6) == 0);
}

/* --- Inheritance (:include) --- */

TEST(struct_include_basic)
{
    eval_print("(defstruct parent18 (a 1) (b 2))");
    eval_print("(defstruct (child18 (:include parent18)) (c 3))");
    ASSERT_STR_EQ(eval_print("(child18-a (make-child18 :a 10 :c 30))"), "10");
    ASSERT_STR_EQ(eval_print("(child18-b (make-child18))"), "2");
    ASSERT_STR_EQ(eval_print("(child18-c (make-child18 :c 30))"), "30");
}

TEST(struct_include_typep)
{
    eval_print("(defstruct parent19 (x 0))");
    eval_print("(defstruct (child19 (:include parent19)) (y 0))");
    ASSERT_STR_EQ(eval_print("(typep (make-child19) 'child19)"), "T");
    ASSERT_STR_EQ(eval_print("(typep (make-child19) 'parent19)"), "T");
    /* Parent is not a child */
    ASSERT_STR_EQ(eval_print("(typep (make-parent19) 'child19)"), "NIL");
}

TEST(struct_include_predicate)
{
    eval_print("(defstruct base20 (x 0))");
    eval_print("(defstruct (derived20 (:include base20)) (y 0))");
    /* Derived predicate works */
    ASSERT_STR_EQ(eval_print("(derived20-p (make-derived20))"), "T");
    /* Parent predicate also matches derived */
    ASSERT_STR_EQ(eval_print("(base20-p (make-derived20))"), "T");
}

/* --- Multiple slots / defaults --- */

TEST(struct_no_slots)
{
    eval_print("(defstruct empty21)");
    ASSERT_STR_EQ(eval_print("(empty21-p (make-empty21))"), "T");
    ASSERT_STR_EQ(eval_print("(type-of (make-empty21))"), "EMPTY21");
}

TEST(struct_slot_no_default)
{
    /* Slot with no default gets NIL */
    eval_print("(defstruct nd22 x y)");
    ASSERT_STR_EQ(eval_print("(nd22-x (make-nd22))"), "NIL");
    ASSERT_STR_EQ(eval_print("(nd22-y (make-nd22))"), "NIL");
    ASSERT_STR_EQ(eval_print("(nd22-x (make-nd22 :x 7))"), "7");
}

TEST(struct_boa_constructor)
{
    /* BOA constructor with positional args */
    eval_print("(defstruct (boa1 (:constructor make-boa1 (x y))) x y)");
    ASSERT_STR_EQ(eval_print("(boa1-x (make-boa1 10 20))"), "10");
    ASSERT_STR_EQ(eval_print("(boa1-y (make-boa1 10 20))"), "20");
    /* BOA with &optional — bare param inherits slot default (CL spec 3.4.6) */
    eval_print("(defstruct (boa2 (:constructor make-boa2 (a &optional b)))"
               "  (a nil) (b t))");
    ASSERT_STR_EQ(eval_print("(boa2-b (make-boa2 1))"), "T");
    ASSERT_STR_EQ(eval_print("(boa2-b (make-boa2 1 42))"), "42");
    /* BOA with &optional explicit default overrides slot default */
    eval_print("(defstruct (boa3 (:constructor make-boa3 (a &optional (b 99))))"
               "  (a nil) (b t))");
    ASSERT_STR_EQ(eval_print("(boa3-b (make-boa3 1))"), "99");
}

TEST(struct_multiple_constructors)
{
    /* CLHS 3.4.6.6: a struct may declare several (:constructor ...) options;
     * each one must be defined.  Regression: previously only the LAST option
     * was honored — earlier constructors were silently dropped (e.g. esrap's
     * failed-parse with both make-failed-parse and make-failed-parse/no-position
     * → "Undefined function: MAKE-FAILED-PARSE"). */
    eval_print("(defstruct (mc1"
               "             (:constructor mc1-full (a b c))"
               "             (:constructor mc1-short (a c)))"
               "  (a nil) (b 0) (c nil))");
    ASSERT_STR_EQ(eval_print("(fboundp 'mc1-full)"), "T");
    ASSERT_STR_EQ(eval_print("(fboundp 'mc1-short)"), "T");
    ASSERT_STR_EQ(eval_print("(mc1-a (mc1-full 'x 5 'z))"), "X");
    ASSERT_STR_EQ(eval_print("(mc1-b (mc1-full 'x 5 'z))"), "5");
    ASSERT_STR_EQ(eval_print("(mc1-c (mc1-full 'x 5 'z))"), "Z");
    /* mc1-short omits b → inherits the slot default (0) */
    ASSERT_STR_EQ(eval_print("(mc1-a (mc1-short 'p 'q))"), "P");
    ASSERT_STR_EQ(eval_print("(mc1-b (mc1-short 'p 'q))"), "0");
    ASSERT_STR_EQ(eval_print("(mc1-c (mc1-short 'p 'q))"), "Q");

    /* Mixed: a BOA constructor AND a keyword constructor on the same struct. */
    eval_print("(defstruct (mc2"
               "             (:constructor mc2-boa (x y))"
               "             (:constructor mc2-kw))"
               "  (x 1) (y 2))");
    ASSERT_STR_EQ(eval_print("(mc2-x (mc2-boa 10 20))"), "10");
    ASSERT_STR_EQ(eval_print("(mc2-y (mc2-kw :y 99))"), "99");
    ASSERT_STR_EQ(eval_print("(mc2-x (mc2-kw))"), "1");

    /* Multiple BOA constructors over an (:include)d parent's slots — the
     * exact shape esrap's failed-parse uses. */
    eval_print("(defstruct (mc-base (:constructor nil)) (e nil) (p 0) (d nil))");
    eval_print("(defstruct (mc-leaf (:include mc-base)"
               "             (:constructor mc-leaf-full (e p d))"
               "             (:constructor mc-leaf/no-pos (e d))))");
    ASSERT_STR_EQ(eval_print("(mc-base-e (mc-leaf-full 'ex 7 'de))"), "EX");
    ASSERT_STR_EQ(eval_print("(mc-base-p (mc-leaf-full 'ex 7 'de))"), "7");
    /* /no-pos omits p → inherited slot default (0) */
    ASSERT_STR_EQ(eval_print("(mc-base-p (mc-leaf/no-pos 'ex 'de))"), "0");
    ASSERT_STR_EQ(eval_print("(mc-base-d (mc-leaf/no-pos 'ex 'de))"), "DE");
}

TEST(struct_compiler_macro_environment)
{
    /* define-compiler-macro must strip &environment from its lambda list;
     * otherwise &environment + its var leak into the inner destructuring-bind
     * as bogus required params → spurious "too few elements" at compile time
     * (alexandria's of-type compiler-macro shape).  &environment may appear
     * after &whole and at any position. */
    eval_print("(defun cmenv-fn (x) x)");
    eval_print("(define-compiler-macro cmenv-fn (&whole form x &environment env)"
               "  (declare (ignorable env)) (if (integerp x) (* 2 x) form))");
    /* compiler macro folds a constant arg */
    ASSERT_STR_EQ(eval_print("(cmenv-fn 21)"), "42");
    /* declines (returns form) on a non-constant arg */
    eval_print("(defparameter *cmv* 5)");
    ASSERT_STR_EQ(eval_print("(cmenv-fn *cmv*)"), "5");

    /* &environment without &whole */
    eval_print("(defun cmenv2-fn (a b) (list a b))");
    eval_print("(define-compiler-macro cmenv2-fn (a b &environment env)"
               "  (declare (ignorable env)) `(list ,b ,a))");
    ASSERT_STR_EQ(eval_print("(cmenv2-fn 1 2)"), "(2 1)");
}

/* --- Typed-sequence structs: (:type vector) / (:type list) --- */

TEST(struct_type_vector)
{
    /* Bare (:type vector): value is a real vector, accessors index it. */
    eval_print("(defstruct (tv1 (:type vector)) a b)");
    ASSERT_STR_EQ(eval_print("(vectorp (make-tv1 :a 1 :b 2))"), "T");
    ASSERT_STR_EQ(eval_print("(tv1-a (make-tv1 :a 1 :b 2))"), "1");
    ASSERT_STR_EQ(eval_print("(tv1-b (make-tv1 :a 1 :b 2))"), "2");
    /* setf accessor mutates the underlying vector */
    eval_print("(defparameter *tv* (make-tv1 :a 1 :b 2))");
    eval_print("(setf (tv1-b *tv*) 99)");
    ASSERT_STR_EQ(eval_print("(tv1-b *tv*)"), "99");
}

TEST(struct_type_vector_compound)
{
    /* (:type (vector ELEMENT-TYPE)) — ironclad's digest-register form.  The
     * element type is advisory; the value is still a real CL:VECTOR usable
     * with REPLACE/AREF. */
    eval_print("(defstruct (tv2 (:type (vector (unsigned-byte 32)))"
               "             (:constructor make-tv2 ())) (a 7) (b 8))");
    ASSERT_STR_EQ(eval_print("(vectorp (make-tv2))"), "T");
    ASSERT_STR_EQ(eval_print("(tv2-a (make-tv2))"), "7");
    ASSERT_STR_EQ(eval_print("(aref (make-tv2) 1)"), "8");
    /* REPLACE works because it is a real sequence */
    eval_print("(defparameter *tv2* (make-tv2))");
    eval_print("(replace *tv2* #(11 22))");
    ASSERT_STR_EQ(eval_print("(tv2-a *tv2*)"), "11");
}

TEST(struct_type_list)
{
    /* (:type list) — rfc2388's header form.  Value is a real list, so
     * positional access (NTH / SECOND) works on it. */
    eval_print("(defstruct (tl1 (:type list)"
               "             (:constructor make-tl1 (name value parameters)))"
               "  name value parameters)");
    ASSERT_STR_EQ(eval_print("(listp (make-tl1 \"n\" \"v\" nil))"), "T");
    ASSERT_STR_EQ(eval_print("(tl1-value (make-tl1 \"n\" \"v\" nil))"), "\"v\"");
    /* second element of the list IS the value slot (snooze relies on this) */
    ASSERT_STR_EQ(eval_print("(second (make-tl1 \"n\" \"v\" nil))"), "\"v\"");
    eval_print("(defparameter *tl* (make-tl1 \"n\" \"v\" nil))");
    eval_print("(setf (tl1-value *tl*) \"x\")");
    ASSERT_STR_EQ(eval_print("(tl1-value *tl*)"), "\"x\"");
}

/* --- Registry hash index + %STRUCT-SLOT-INDEX (spec 3.1) --- */

TEST(struct_slot_index_builtin)
{
    eval_print("(defstruct si1 (a 1) (b 2) (c 3))");
    ASSERT_STR_EQ(eval_print("(amilisp::%struct-slot-index 'si1 'a)"), "0");
    ASSERT_STR_EQ(eval_print("(amilisp::%struct-slot-index 'si1 'b)"), "1");
    ASSERT_STR_EQ(eval_print("(amilisp::%struct-slot-index 'si1 'c)"), "2");
    ASSERT_STR_EQ(eval_print("(amilisp::%struct-slot-index 'si1 'nope)"), "NIL");
    ASSERT_STR_EQ(eval_print("(amilisp::%struct-slot-index 'si1-no-such-type 'a)"), "NIL");
}

TEST(struct_registry_buried_type)
{
    /* The registry is PREPENDED on registration: a type defined before
     * many others sits at the alist tail.  The hash index must find it
     * regardless of position (the pre-index linear walk paid a full
     * O(types) scan here). */
    int i;
    char buf[160];
    eval_print("(defstruct si2 (x 0) (y 0))");
    for (i = 0; i < 80; i++) {
        snprintf(buf, sizeof(buf),
                 "(amilisp::%%register-struct-type (make-symbol \"SI2-F%d\")"
                 " 1 nil '((f nil)))", i);
        eval_print(buf);
    }
    ASSERT_STR_EQ(eval_print("(amilisp::%struct-slot-index 'si2 'y)"), "1");
    ASSERT_STR_EQ(eval_print("(typep (make-si2) 'si2)"), "T");
    ASSERT_STR_EQ(eval_print("(si2-y (make-si2 :x 1 :y 2))"), "2");
}

TEST(struct_registry_redefinition_newest_wins)
{
    /* Re-registering a name prepends a fresh entry; lookups must see
     * the NEWEST entry — the linear walk's first-match semantics. */
    eval_print("(amilisp::%register-struct-type 'si3 1 nil '((a nil)))");
    eval_print("(amilisp::%register-struct-type 'si3 3 nil"
               " '((p nil) (q nil) (r nil)))");
    ASSERT_STR_EQ(eval_print("(amilisp::%struct-slot-count 'si3)"), "3");
    ASSERT_STR_EQ(eval_print("(amilisp::%struct-slot-index 'si3 'r)"), "2");
    ASSERT_STR_EQ(eval_print("(amilisp::%struct-slot-index 'si3 'a)"), "NIL");
}

/* --- Fused fast-path slot access (spec 3.1, second half) --- */

TEST(struct_slot_value_fastpath_builtin)
{
    /* Hit: resolves through the registry hash index in one call. */
    eval_print("(defstruct fp1 (a 10) (b 20))");
    eval_print("(defparameter *fp-miss* (make-symbol \"MISS\"))");
    ASSERT_STR_EQ(eval_print(
        "(amilisp::%struct-slot-value (make-fp1) 'b *fp-miss*)"), "20");
    /* Miss: no such slot -> the sentinel, never an error. */
    ASSERT_STR_EQ(eval_print(
        "(eq (amilisp::%struct-slot-value (make-fp1) 'nope *fp-miss*)"
        " *fp-miss*)"), "T");
    /* Miss: not a struct (fixnum, list, NIL). */
    ASSERT_STR_EQ(eval_print(
        "(eq (amilisp::%struct-slot-value 42 'a *fp-miss*) *fp-miss*)"), "T");
    ASSERT_STR_EQ(eval_print(
        "(eq (amilisp::%struct-slot-value '(1 2) 'a *fp-miss*) *fp-miss*)"),
        "T");
    ASSERT_STR_EQ(eval_print(
        "(eq (amilisp::%struct-slot-value nil 'a *fp-miss*) *fp-miss*)"), "T");
}

TEST(struct_slot_store_fastpath_builtin)
{
    eval_print("(defstruct fp2 (x 1))");
    eval_print("(defparameter *fp2* (make-fp2))");
    /* Hit: stores and returns T. */
    ASSERT_STR_EQ(eval_print(
        "(amilisp::%struct-slot-store *fp2* 'x 99)"), "T");
    ASSERT_STR_EQ(eval_print("(fp2-x *fp2*)"), "99");
    /* Miss: no such slot / not a struct -> NIL, nothing stored. */
    ASSERT_STR_EQ(eval_print(
        "(amilisp::%struct-slot-store *fp2* 'nope 5)"), "NIL");
    ASSERT_STR_EQ(eval_print(
        "(amilisp::%struct-slot-store 42 'x 5)"), "NIL");
    ASSERT_STR_EQ(eval_print("(fp2-x *fp2*)"), "99");
}

TEST(gf_ic_emf_probe)
{
    /* The IC probe returns the cached EMF only when the receivers'
     * classes match the populated inline cache; anything else — cold
     * cache, wrong class, non-GF — is NIL (slow path). */
    eval_print("(defstruct fp3 (v 7))");
    eval_print("(defgeneric fp-probe (x))");
    eval_print("(defmethod fp-probe ((x fp3)) (fp3-v x))");
    eval_print("(defparameter *fp3* (make-fp3))");
    /* Cold cache: probe misses, call still dispatches correctly. */
    ASSERT_STR_EQ(eval_print(
        "(functionp (amilisp::%gf-ic-emf"
        " (gethash 'fp-probe *generic-function-table*) *fp3*))"),
        "NIL");
    ASSERT_STR_EQ(eval_print("(fp-probe *fp3*)"), "7");
    /* Populated by the first dispatch: probe now returns the EMF. */
    ASSERT_STR_EQ(eval_print(
        "(functionp (amilisp::%gf-ic-emf"
        " (gethash 'fp-probe *generic-function-table*) *fp3*))"),
        "T");
    /* Wrong receiver class: miss. */
    ASSERT_STR_EQ(eval_print(
        "(amilisp::%gf-ic-emf"
        " (gethash 'fp-probe *generic-function-table*) 42)"), "NIL");
    /* Non-GF first argument: miss, not an error. */
    ASSERT_STR_EQ(eval_print("(amilisp::%gf-ic-emf 42 *fp3*)"), "NIL");
}

TEST(struct_registry_index_survives_compaction)
{
    /* Compaction relocates the name symbols and entries the index
     * caches; the GC hook marks it dirty and the next lookup must
     * rebuild and still resolve correctly. */
    eval_print("(defstruct si4 (v 41))");
    ASSERT_STR_EQ(eval_print("(amilisp::%struct-slot-index 'si4 'v)"), "0");
    cl_gc_compact();
    ASSERT_STR_EQ(eval_print("(amilisp::%struct-slot-index 'si4 'v)"), "0");
    ASSERT_STR_EQ(eval_print("(typep (make-si4) 'si4)"), "T");
    cl_gc_compact();
    ASSERT_STR_EQ(eval_print("(si4-v (make-si4))"), "41");
    ASSERT_STR_EQ(eval_print("(amilisp::%struct-slot-index 'si4 'nope)"), "NIL");
}

int main(void)
{
    test_init();
    setup();

    RUN(struct_basic_define);
    RUN(struct_constructor);
    RUN(struct_predicate);
    RUN(struct_setf_accessor);
    RUN(struct_copier);
    RUN(struct_typep);
    RUN(struct_type_of);
    RUN(struct_printer);
    RUN(struct_structurep);

    /* Options */
    RUN(struct_conc_name);
    RUN(struct_conc_name_nil);
    RUN(struct_constructor_name);
    RUN(struct_constructor_nil);
    RUN(struct_predicate_name);
    RUN(struct_predicate_nil);
    RUN(struct_copier_name);
    RUN(struct_copier_nil);

    /* Inheritance */
    RUN(struct_include_basic);
    RUN(struct_include_typep);
    RUN(struct_include_predicate);

    /* Multiple slots / defaults */
    RUN(struct_no_slots);
    RUN(struct_slot_no_default);
    RUN(struct_boa_constructor);
    RUN(struct_multiple_constructors);
    RUN(struct_compiler_macro_environment);

    /* Typed-sequence structs */
    RUN(struct_type_vector);
    RUN(struct_type_vector_compound);
    RUN(struct_type_list);

    /* Registry hash index + %STRUCT-SLOT-INDEX */
    RUN(struct_slot_index_builtin);
    RUN(struct_registry_buried_type);
    RUN(struct_registry_redefinition_newest_wins);
    RUN(struct_registry_index_survives_compaction);

    teardown();
    REPORT();
}
