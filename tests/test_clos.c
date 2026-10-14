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

/* === Step 1: %class-of builtin === */

TEST(class_of_fixnum)
{
    ASSERT_STR_EQ(eval_print("(%class-of 42)"), "FIXNUM");
}

TEST(class_of_nil)
{
    ASSERT_STR_EQ(eval_print("(%class-of nil)"), "NULL");
}

TEST(class_of_symbol)
{
    ASSERT_STR_EQ(eval_print("(%class-of 'foo)"), "SYMBOL");
}

TEST(class_of_cons)
{
    ASSERT_STR_EQ(eval_print("(%class-of '(1 2))"), "CONS");
}

TEST(class_of_string)
{
    ASSERT_STR_EQ(eval_print("(%class-of \"hello\")"), "STRING");
}

TEST(class_of_character)
{
    ASSERT_STR_EQ(eval_print("(%class-of #\\A)"), "CHARACTER");
}

/* Regression: strings created via MAKE-ARRAY (TYPE_WIDE_STRING under
 * CL_WIDE_STRINGS) must report the STRING class, not T.  A wrong class
 * here makes CLOS dispatch miss STRING-specialized methods even though
 * STRINGP / TYPE-OF correctly say STRING — see fset Test-Misc-2 /
 * Test-Compare-Lexicographically ("No applicable method for ITERATOR"). */
TEST(class_of_make_array_character_string)
{
    ASSERT_STR_EQ(eval_print(
        "(%class-of (make-array 3 :element-type 'character :initial-element #\\a))"),
        "STRING");
}

TEST(class_of_make_array_base_char_string)
{
    ASSERT_STR_EQ(eval_print(
        "(%class-of (make-array '(1) :element-type 'base-char :initial-element #\\a))"),
        "STRING");
}

TEST(class_of_make_array_adjustable_string)
{
    ASSERT_STR_EQ(eval_print(
        "(%class-of (make-array 2 :element-type 'character :adjustable t "
        ":initial-element #\\a))"),
        "STRING");
}

TEST(class_of_function)
{
    ASSERT_STR_EQ(eval_print("(%class-of #'car)"), "FUNCTION");
}

TEST(class_of_lambda)
{
    ASSERT_STR_EQ(eval_print("(%class-of (lambda (x) x))"), "FUNCTION");
}

TEST(class_of_vector)
{
    ASSERT_STR_EQ(eval_print("(%class-of (vector 1 2 3))"), "VECTOR");
}

TEST(class_of_hashtable)
{
    ASSERT_STR_EQ(eval_print("(%class-of (make-hash-table))"), "HASH-TABLE");
}

TEST(class_of_struct)
{
    eval_print("(defstruct clos-test-pt (x 0) (y 0))");
    ASSERT_STR_EQ(eval_print("(%class-of (make-clos-test-pt :x 1 :y 2))"), "CLOS-TEST-PT");
}

TEST(class_of_float)
{
    ASSERT_STR_EQ(eval_print("(%class-of 3.14)"), "SINGLE-FLOAT");
}

TEST(class_of_ratio)
{
    ASSERT_STR_EQ(eval_print("(%class-of 1/3)"), "RATIO");
}

TEST(class_of_pathname)
{
    ASSERT_STR_EQ(eval_print("(%class-of #P\"/tmp/foo\")"), "PATHNAME");
}

TEST(class_of_package)
{
    ASSERT_STR_EQ(eval_print("(%class-of (find-package :cl))"), "PACKAGE");
}

TEST(class_of_stream)
{
    ASSERT_STR_EQ(eval_print("(%class-of (make-string-input-stream \"hi\"))"), "STREAM");
}

/* === Step 2: Bootstrap core classes === */

/* CLOS module is auto-loaded during init */
TEST(clos_require)
{
    /* CLOS is now loaded at startup; require returns NIL (already provided) */
    ASSERT_STR_EQ(eval_print("(require \"clos\")"), "NIL");
}

/* find-class returns a class metaobject */
TEST(find_class_integer)
{
    ASSERT_STR_EQ(eval_print("(structurep (find-class 'integer))"), "T");
}

/* class-name returns the class name symbol */
TEST(class_name_integer)
{
    ASSERT_STR_EQ(eval_print("(class-name (find-class 'integer))"), "INTEGER");
}

TEST(class_name_string)
{
    ASSERT_STR_EQ(eval_print("(class-name (find-class 'string))"), "STRING");
}

TEST(class_name_cons)
{
    ASSERT_STR_EQ(eval_print("(class-name (find-class 'cons))"), "CONS");
}

TEST(class_name_t)
{
    ASSERT_STR_EQ(eval_print("(class-name (find-class 't))"), "T");
}

/* find-class with errorp nil returns nil for unknown */
TEST(find_class_unknown_nil)
{
    ASSERT_STR_EQ(eval_print("(find-class 'no-such-class nil)"), "NIL");
}

/* find-class with errorp t (default) signals error */
TEST(find_class_unknown_error)
{
    ASSERT(strncmp(eval_print("(find-class 'no-such-class)"), "ERROR:", 6) == 0);
}

/* class-of returns class metaobject for built-in types */
TEST(class_of_42_is_fixnum_class)
{
    ASSERT_STR_EQ(eval_print("(class-name (class-of 42))"), "FIXNUM");
}

TEST(class_of_string_is_string_class)
{
    ASSERT_STR_EQ(eval_print("(class-name (class-of \"hello\"))"), "STRING");
}

/* Regression (CLOS dispatch field bug): CLASS-OF of a built-in-typed object
 * must NOT depend on *PACKAGE*.  bi_class_of used to intern the class name
 * ("SYMBOL", "CONS", ...) via *PACKAGE*-relative cl_intern; when *PACKAGE*
 * did not resolve the name to its COMMON-LISP-package symbol (KEYWORD here,
 * or another thread's *PACKAGE* leaking through the shared cl_current_package
 * global in a multi-threaded Sly/sento/log4cl session), CLASS-OF interned the
 * WRONG symbol, missed *class-table*, and silently fell back to the T class.
 * That made GF dispatch see arg class T instead of SYMBOL and signal
 * "No applicable method for ... (X SYMBOL)" even though a (t symbol) method
 * applied.  Pre-fix these return NIL (class-of gave the T class); post-fix T. */
TEST(class_of_symbol_independent_of_package)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((sc (find-class 'symbol)))"
        "  (let ((*package* (find-package :keyword)))"
        "    (eq sc (class-of 'foo))))"),
        "T");
}

TEST(class_of_builtins_independent_of_package)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((sc (find-class 'symbol))"
        "      (cc (find-class 'cons))"
        "      (strc (find-class 'string))"
        "      (fc (find-class 'fixnum)))"
        "  (let ((*package* (find-package :keyword)))"
        "    (and (eq sc (class-of 'foo))"
        "         (eq cc (class-of '(1 2)))"
        "         (eq strc (class-of \"x\"))"
        "         (eq fc (class-of 42)))))"),
        "T");
}

/* %class-of must yield a COMMON-LISP-package symbol regardless of *PACKAGE*,
 * so the *class-table* (CL-symbol-keyed) lookup in CLASS-OF hits. */
TEST(class_of_name_symbol_is_cl_package)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((*package* (find-package :keyword)))"
        "  (eq (find-package :common-lisp) (symbol-package (%class-of 'foo))))"),
        "T");
}

/* End-to-end: a (t symbol) method stays applicable even when the call is made
 * with *PACKAGE* bound to a package that does not inherit CL:SYMBOL — the
 * shape of the ASDF FIND-OPERATION field failure. */
TEST(dispatch_symbol_method_independent_of_package)
{
    eval_print("(defgeneric clos-symdisp (a b))");
    eval_print("(defmethod clos-symdisp ((a t) (b symbol)) :sym)");
    eval_print("(defmethod clos-symdisp ((a t) (b string)) :str)");
    ASSERT_STR_EQ(eval_print(
        "(let ((*package* (find-package :keyword)))"
        "  (clos-symdisp 42 'anything))"),
        ":SYM");
}

/* Regression: a STRING-specialized method must be applicable to a string
 * produced by MAKE-ARRAY (wide string), not just to a literal string. */
TEST(dispatch_string_method_on_make_array_string)
{
    eval_print("(defgeneric clos-strdisp (x))");
    eval_print("(defmethod clos-strdisp ((x string)) :str)");
    eval_print("(defmethod clos-strdisp ((x t)) :other)");
    ASSERT_STR_EQ(eval_print(
        "(clos-strdisp (make-array 3 :element-type 'character :initial-element #\\a))"),
        ":STR");
}

/* Regression: a stale/corrupt NEGATIVE dispatch-cache entry (present, value
 * NIL — "no method applies") must not be trusted blindly.  The cache is only
 * a memo of a pure function, so on a negative hit the dispatcher re-verifies
 * against the live method list and heals the entry when a method plainly
 * exists.  This is the mechanism behind the field report "No applicable
 * method for FIND-OPERATION with args of types (COMPILE-OP SYMBOL)" — a
 * method that clearly exists yet dispatch reported a miss because a corrupt
 * negative cache entry (from a GC relocation of the EQ-keyed cache or a
 * concurrent cache write) shadowed it. */
TEST(dispatch_negative_cache_self_heals)
{
    eval_print("(defclass neg-op () ())");
    eval_print("(defgeneric neg-find (ctx spec))");
    eval_print("(defmethod neg-find ((c t) (s neg-op)) (list :op s))");
    eval_print("(defmethod neg-find ((c t) (s symbol)) :sym)");
    eval_print("(defmethod neg-find ((c t) (s string)) :str)");
    /* Prime the cache with a real call so the nested table exists. */
    eval_print("(defparameter *no* (make-instance 'neg-op))");
    ASSERT_STR_EQ(eval_print("(neg-find *no* 'load-thing)"), ":SYM");
    /* Poke a stale NEGATIVE entry for (neg-op -> SYMBOL) and drop the inline
     * cache so the dispatch table is actually consulted. */
    eval_print(
        "(let* ((gf (fdefinition 'neg-find))"
        "       (cache (gf-dispatch-cache gf))"
        "       (inner (gethash (class-of *no*) cache)))"
        "  (setf (gethash (class-of 'x) inner) nil)"
        "  (%set-gf-inline-cache gf nil))");
    /* Before the fix this signaled "No applicable method"; now it heals. */
    ASSERT_STR_EQ(eval_print("(neg-find *no* 'prepare-thing)"), ":SYM");
}

/* The heal must not paper over a GENUINE no-applicable-method: recomputing
 * from the live method list still finds nothing for an unspecialized arg
 * class, so an error is still signaled. */
TEST(dispatch_genuine_miss_still_errors)
{
    eval_print("(defgeneric gm-only-str (x))");
    eval_print("(defmethod gm-only-str ((x string)) :str)");
    /* 42 is a fixnum — no applicable method; must error, not return NIL. */
    ASSERT_STR_EQ(eval_print("(gm-only-str 42)"), "ERROR:1");
}

/* Regression: standard combination must self-heal when handed an applicable
 * method set that is missing its primary (only :around/:before/:after) — the
 * "No applicable primary method" field report during asdf:load-system.  This
 * is the same intermittent dispatch-metadata corruption as the negative-cache
 * case (a GC relocation or concurrent method-list read dropping the primary),
 * so %DISPATCH-STANDARD-EMF re-verifies against the live method list before
 * letting %BUILD-EFFECTIVE-METHOD error. */
TEST(dispatch_no_primary_self_heals)
{
    eval_print("(defclass npf-op () ())");
    eval_print("(defclass npf-comp () ())");
    eval_print("(defgeneric npf (o c))");
    eval_print("(defmethod npf :around ((o t) (c t)) (list :around (call-next-method)))");
    eval_print("(defmethod npf ((o npf-op) (c npf-comp)) :prim)");
    eval_print("(defparameter *npf-o* (make-instance 'npf-op))");
    eval_print("(defparameter *npf-c* (make-instance 'npf-comp))");
    /* Normal dispatch: :around wraps the primary. */
    ASSERT_STR_EQ(eval_print("(npf *npf-o* *npf-c*)"), "(:AROUND :PRIM)");
    /* Feed %DISPATCH-STANDARD-EMF a primary-less applicable set (only the
     * :around) while the GF still has its primary; the heal recomputes and
     * dispatches correctly. */
    ASSERT_STR_EQ(eval_print(
        "(let* ((gf (fdefinition 'npf))"
        "       (around-only (remove-if (lambda (m) (null (method-qualifiers m)))"
        "                                (%compute-applicable-methods gf (list *npf-o* *npf-c*)))))"
        "  (let ((*current-method-args* (list *npf-o* *npf-c*)))"
        "    (funcall (%dispatch-standard-emf gf around-only) *npf-o* *npf-c*)))"),
        "(:AROUND :PRIM)");
}

/* But a GENUINE no-primary (only :around defined) must still error after the
 * recompute finds no primary either. */
TEST(dispatch_genuine_no_primary_still_errors)
{
    eval_print("(defgeneric onlyaround (x))");
    eval_print("(defmethod onlyaround :around ((x t)) (call-next-method))");
    ASSERT_STR_EQ(eval_print("(onlyaround 5)"), "ERROR:1");
}

/* Regression: the FIRST (uncached) dispatch miss must self-heal too.  The
 * negative-cache heal (%DISPATCH-NEGATIVE-HIT) only fires on a *second* call —
 * the first call, on a fresh cache, computes the applicable set directly and,
 * before this fix, immediately cached a negative AND signaled "No applicable
 * method" if that one computation came up empty.  That is the exact field
 * report shape "No applicable method for FIND-OPERATION with args of types
 * (PREPARE-OP SYMBOL)" hitting on the very first find-operation dispatch of a
 * load.  %DISPATCH-HEAL-EMPTY recomputes (bounded, yielding) from the live
 * method list before giving up, so a transient empty set (a GC relocation /
 * concurrent metadata mutation window) is corrected on the first miss.  Here we
 * drive the healer directly with args that DO have a method: it returns a
 * callable EMF and bumps *GF-CACHE-HEALS*. */
TEST(dispatch_fresh_miss_empty_self_heals)
{
    eval_print("(defclass fme-op () ())");
    eval_print("(defgeneric fme (ctx spec))");
    eval_print("(defmethod fme ((c t) (s symbol)) :sym)");
    eval_print("(defparameter *fme* (make-instance 'fme-op))");
    ASSERT_STR_EQ(eval_print(
        "(let* ((gf (fdefinition 'fme))"
        "       (h0 *gf-cache-heals*)"
        "       (emf (%dispatch-heal-empty gf (list *fme* 'a-sym))))"
        "  (list (and (functionp emf) (funcall emf *fme* 'a-sym))"
        "        (> *gf-cache-heals* h0)))"),
        "(:SYM T)");
}

/* The fresh-miss heal must NOT paper over a genuine no-applicable-method: for a
 * GF that has methods but none applicable to these argument types, the healer
 * recomputes empty every time and returns NIL, so the dispatcher signals the
 * error exactly as before. */
TEST(dispatch_fresh_miss_genuine_still_errors)
{
    eval_print("(defgeneric fmg (x))");
    eval_print("(defmethod fmg ((x string)) :str)");
    /* Healer returns NIL for a non-string arg (no method applies). */
    ASSERT_STR_EQ(eval_print(
        "(%dispatch-heal-empty (fdefinition 'fmg) (list 42))"), "NIL");
    /* And the ordinary call still errors. */
    ASSERT_STR_EQ(eval_print("(fmg 42)"), "ERROR:1");
}

/* The no-primary heal is gated on the GF's ROSTER actually containing a primary
 * method (%GF-ROSTER-HAS-PRIMARY-P): a GF that defines a primary but had it
 * transiently dropped from the applicable set retries-and-heals, while a GF
 * with only :around/:before/:after can never heal and must not waste retries.
 * %RECOMPUTE-METHODS-UNTIL is the shared bounded-retry-with-yield used by both
 * self-heals. */
TEST(dispatch_no_primary_roster_gate)
{
    eval_print("(defgeneric rg-prim (x))");
    eval_print("(defmethod rg-prim ((x integer)) :int)");
    eval_print("(defgeneric rg-around (x))");
    eval_print("(defmethod rg-around :around ((x t)) (call-next-method))");
    /* Roster gate: T for a GF with a primary, NIL for around-only. */
    ASSERT_STR_EQ(eval_print("(%gf-roster-has-primary-p (fdefinition 'rg-prim))"), "T");
    ASSERT_STR_EQ(eval_print("(%gf-roster-has-primary-p (fdefinition 'rg-around))"), "NIL");
    /* The shared retry helper returns a satisfying (has-primary) set for a GF
     * whose primary applies to the args. */
    ASSERT_STR_EQ(eval_print(
        "(let ((s (%recompute-methods-until (fdefinition 'rg-prim) (list 5)"
        "                                   #'%methods-have-primary-p)))"
        "  (and (consp s) (%methods-have-primary-p s)))"),
        "T");
    /* ...and NIL when no recompute can satisfy the predicate (no applicable
     * primary for these args). */
    ASSERT_STR_EQ(eval_print(
        "(%recompute-methods-until (fdefinition 'rg-prim) (list \"str\")"
        "                          #'%methods-have-primary-p)"), "NIL");
}

/* Regression: %GF-DISPATCH's uncached/variadic fallback branch (the T clause
 * taken when GF-CACHEABLE-P is neither an integer nor :EQL) used to call
 * %DISPATCH-HEAL-EMPTY before binding *CURRENT-METHOD-ARGS* to ARGS, unlike
 * every other dispatch resolver (%GF-DISPATCH-EQL, %GF-DISPATCH-CACHED,
 * %GF-DISPATCH-1-SLOW, %GF-2-RESOLVE).  %DISPATCH-HEAL-EMPTY's no-primary
 * fallback (via %DISPATCH-BUILD-EMF -> %DISPATCH-STANDARD-EMF) reads that
 * dynamic var, so a stale/default binding could recompute against the wrong
 * argument list.  Spy on %DISPATCH-HEAL-EMPTY to capture *CURRENT-METHOD-ARGS*
 * at the exact moment %GF-DISPATCH invokes it from the T branch. */
TEST(gf_dispatch_slow_path_binds_current_method_args_before_heal)
{
    eval_print("(defgeneric gdca-fn (x))");
    eval_print("(defmethod gdca-fn ((x symbol)) :sym)");
    /* Force the T fallback: a mode that is neither an integer nor :EQL
     * bypasses %GF-DISPATCH-1-SLOW's fast integer-mode branch, which falls
     * through to (%GF-DISPATCH GF ARGS). */
    eval_print("(%set-gf-cacheable-p (fdefinition 'gdca-fn) :forced-slow)");
    eval_print("(defparameter *gdca-captured* :unset)");
    eval_print(
        "(let ((orig (symbol-function '%dispatch-heal-empty)))"
        "  (unwind-protect"
        "      (progn"
        "        (setf (symbol-function '%dispatch-heal-empty)"
        "              (lambda (gf args)"
        "                (setq *gdca-captured* *current-method-args*)"
        "                (funcall orig gf args)))"
        "        (ignore-errors (gdca-fn 42)))"
        "    (setf (symbol-function '%dispatch-heal-empty) orig)))");
    /* 42 has no applicable GDCA-FN method, so %COMPUTE-APPLICABLE-METHODS
     * returns empty and %GF-DISPATCH calls %DISPATCH-HEAL-EMPTY with
     * ARGS = (42); *CURRENT-METHOD-ARGS* must already equal that by then. */
    ASSERT_STR_EQ(eval_print("*gdca-captured*"), "(42)");
}

TEST(class_of_nil_is_null_class)
{
    ASSERT_STR_EQ(eval_print("(class-name (class-of nil))"), "NULL");
}

TEST(class_of_cons_is_cons_class)
{
    ASSERT_STR_EQ(eval_print("(class-name (class-of '(1 2)))"), "CONS");
}

/* class-of returns class object (same as find-class) */
TEST(class_of_eq_find_class)
{
    ASSERT_STR_EQ(eval_print("(eq (class-of 42) (find-class 'fixnum))"), "T");
}

/* class-direct-superclasses */
TEST(direct_supers_integer)
{
    ASSERT_STR_EQ(eval_print(
        "(class-name (car (class-direct-superclasses (find-class 'integer))))"),
        "RATIONAL");
}

TEST(direct_supers_t)
{
    ASSERT_STR_EQ(eval_print(
        "(class-direct-superclasses (find-class 't))"), "NIL");
}

/* logical-pathname class exists and has PATHNAME as direct superclass */
TEST(logical_pathname_class_exists)
{
    ASSERT_STR_EQ(eval_print(
        "(class-name (find-class 'logical-pathname))"), "LOGICAL-PATHNAME");
}

TEST(logical_pathname_superclass_is_pathname)
{
    ASSERT_STR_EQ(eval_print(
        "(class-name (car (class-direct-superclasses (find-class 'logical-pathname))))"),
        "PATHNAME");
}

TEST(logical_pathname_defmethod_loads)
{
    ASSERT_STR_EQ(eval_print(
        "(progn (defgeneric lp-test (x)) (defmethod lp-test ((x logical-pathname)) :lp) :ok)"),
        ":OK");
}

/* file-stream class exists and has STREAM as direct superclass */
TEST(file_stream_class_exists)
{
    ASSERT_STR_EQ(eval_print(
        "(class-name (find-class 'file-stream))"), "FILE-STREAM");
}

TEST(file_stream_superclass_is_stream)
{
    ASSERT_STR_EQ(eval_print(
        "(class-name (car (class-direct-superclasses (find-class 'file-stream))))"),
        "STREAM");
}

TEST(file_stream_defmethod_loads)
{
    ASSERT_STR_EQ(eval_print(
        "(progn (defgeneric fs-test (x)) (defmethod fs-test ((x file-stream)) :fs) :ok)"),
        ":OK");
}

/* class-precedence-list */
TEST(cpl_fixnum)
{
    ASSERT_STR_EQ(eval_print(
        "(mapcar #'class-name (class-precedence-list (find-class 'fixnum)))"),
        "(FIXNUM INTEGER RATIONAL REAL NUMBER T)");
}

TEST(cpl_cons)
{
    ASSERT_STR_EQ(eval_print(
        "(mapcar #'class-name (class-precedence-list (find-class 'cons)))"),
        "(CONS LIST SEQUENCE T)");
}

TEST(cpl_null)
{
    /* NULL is a subclass of both SYMBOL and LIST.  The CPL must place T
       last; a naive append+dedup of the supers' CPLs put T in the middle
       (NULL SYMBOL T LIST SEQUENCE), which made method dispatch rank a
       T-specialized method above a LIST-specialized one for NIL. */
    ASSERT_STR_EQ(eval_print(
        "(mapcar #'class-name (class-precedence-list (find-class 'null)))"),
        "(NULL SYMBOL LIST SEQUENCE T)");
}

TEST(cpl_vector)
{
    /* VECTOR is a subclass of both ARRAY and SEQUENCE -- another diamond
       whose CPL must keep T last. */
    ASSERT_STR_EQ(eval_print(
        "(mapcar #'class-name (class-precedence-list (find-class 'vector)))"),
        "(VECTOR ARRAY SEQUENCE T)");
}

/* Dispatch: NIL is a LIST, so a method specialized on LIST must be chosen
   over the default (T) method.  Regression for the malformed NULL CPL. */
TEST(dispatch_nil_as_list)
{
    eval_print("(defgeneric dnl (x))");
    eval_print("(defmethod dnl (x) \"T\")");
    eval_print("(defmethod dnl ((x list)) \"LIST\")");
    ASSERT_STR_EQ(eval_print("(dnl nil)"), "\"LIST\"");
    ASSERT_STR_EQ(eval_print("(dnl '(1 2))"), "\"LIST\"");
    ASSERT_STR_EQ(eval_print("(dnl 5)"), "\"T\"");
}

/* Dispatch: an empty vector (a SEQUENCE) must select a SEQUENCE method
   over the default. */
TEST(dispatch_vector_as_sequence)
{
    eval_print("(defgeneric dvs (x))");
    eval_print("(defmethod dvs (x) \"T\")");
    eval_print("(defmethod dvs ((x sequence)) \"SEQ\")");
    ASSERT_STR_EQ(eval_print("(dvs #())"), "\"SEQ\"");
    ASSERT_STR_EQ(eval_print("(dvs #(1 2))"), "\"SEQ\"");
    ASSERT_STR_EQ(eval_print("(dvs 5)"), "\"T\"");
}

/* setf find-class */
TEST(setf_find_class)
{
    eval_print("(progn (setf (find-class 'my-test-class) (find-class 'integer)) nil)");
    ASSERT_STR_EQ(eval_print("(class-name (find-class 'my-test-class))"), "INTEGER");
}

/* class-direct-subclasses */
TEST(direct_subclasses_integer)
{
    /* INTEGER should have FIXNUM and BIGNUM as direct subclasses */
    ASSERT_STR_EQ(eval_print(
        "(let ((subs (mapcar #'class-name (class-direct-subclasses (find-class 'integer)))))"
        "  (and (member 'fixnum subs) (member 'bignum subs) t))"),
        "T");
}

/* === Phase 1: Slot Access Infrastructure === */

/* Helper: create a simple CLOS class with slot-index-table for testing.
   The table stores STANDARD-EFFECTIVE-SLOT-DEFINITION metaobjects; each
   esd's location (slot 2) is the struct index used by %STRUCT-REF. */
#define PHASE1_SETUP \
    "(progn " \
    "  (%register-struct-type 'test-point 2 nil '((x nil) (y nil))) " \
    "  (let* ((esd-x (%make-effective-slot-def 'x nil nil :instance 0)) " \
    "         (esd-y (%make-effective-slot-def 'y nil nil :instance 1)) " \
    "         (cls (%make-struct 'standard-class " \
    "                'test-point nil nil nil nil nil nil nil nil t nil nil))) " \
    "    (let ((idx-table (make-hash-table :test 'eq))) " \
    "      (setf (gethash 'x idx-table) esd-x) " \
    "      (setf (gethash 'y idx-table) esd-y) " \
    "      (%struct-set cls 4 (list esd-x esd-y)) " \
    "      (%struct-set cls 5 idx-table) " \
    "      (setf (find-class 'test-point) cls))) " \
    "  t)"

TEST(phase1_setup)
{
    ASSERT_STR_EQ(eval_print(PHASE1_SETUP), "T");
}

TEST(slot_value_read)
{
    eval_print("(defvar *test-pt* (%make-struct 'test-point 10 20))");
    ASSERT_STR_EQ(eval_print("(slot-value *test-pt* 'x)"), "10");
    ASSERT_STR_EQ(eval_print("(slot-value *test-pt* 'y)"), "20");
}

TEST(slot_value_write)
{
    ASSERT_STR_EQ(eval_print("(setf (slot-value *test-pt* 'x) 42)"), "42");
    ASSERT_STR_EQ(eval_print("(slot-value *test-pt* 'x)"), "42");
}

TEST(slot_boundp_true)
{
    ASSERT_STR_EQ(eval_print("(slot-boundp *test-pt* 'x)"), "T");
}

TEST(slot_makunbound_and_boundp)
{
    eval_print("(slot-makunbound *test-pt* 'x)");
    ASSERT_STR_EQ(eval_print("(slot-boundp *test-pt* 'x)"), "NIL");
}

TEST(slot_value_unbound_error)
{
    /* After makunbound, reading should error */
    ASSERT(strncmp(eval_print("(slot-value *test-pt* 'x)"), "ERROR:", 6) == 0);
}

/* --- UNBOUND-SLOT: the condition SLOT-UNBOUND signals (CLHS 7.7.11) ---
   Reading an unbound slot must signal a condition of type UNBOUND-SLOT — a
   CELL-ERROR subtype whose CELL-ERROR-NAME is the slot name and whose
   UNBOUND-SLOT-INSTANCE is the object.  Signalling a bare SIMPLE-ERROR would
   still be caught by an (error ...) clause, so these tests pin the type and
   both slots rather than merely "something was signalled". */

TEST(unbound_slot_condition_via_slot_value)
{
    eval_print("(defclass ubs-a () ((x :reader ubs-ax) (y :initform 7)))");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (slot-value (make-instance 'ubs-a) 'x) "
        "  (unbound-slot () :ok))"),
        ":OK");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (slot-value (make-instance 'ubs-a) 'x) "
        "  (unbound-slot (c) (cell-error-name c)))"),
        "X");
    ASSERT_STR_EQ(eval_print(
        "(let ((o (make-instance 'ubs-a))) "
        "  (handler-case (slot-value o 'x) "
        "    (unbound-slot (c) (eq (unbound-slot-instance c) o))))"),
        "T");
}

TEST(unbound_slot_condition_via_reader_gf)
{
    /* The reader-GF fast path answers accessor calls inside OP_CALL; a slot
       holding the unbound marker must fall out of it into the same protocol.
       Called twice so the warm inline cache is exercised too. */
    ASSERT_STR_EQ(eval_print(
        "(let ((o (make-instance 'ubs-a))) "
        "  (handler-case (ubs-ax o) "
        "    (unbound-slot (c) (list (cell-error-name c) "
        "                            (eq (unbound-slot-instance c) o)))))"),
        "(X T)");
    ASSERT_STR_EQ(eval_print(
        "(let ((o (make-instance 'ubs-a))) "
        "  (handler-case (ubs-ax o) "
        "    (unbound-slot (c) (list (cell-error-name c) "
        "                            (eq (unbound-slot-instance c) o)))))"),
        "(X T)");
}

TEST(unbound_slot_condition_after_makunbound)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((o (make-instance 'ubs-a))) "
        "  (slot-makunbound o 'y) "
        "  (handler-case (slot-value o 'y) "
        "    (unbound-slot (c) (cell-error-name c))))"),
        "Y");
}

TEST(unbound_slot_is_a_cell_error)
{
    ASSERT_STR_EQ(eval_print("(subtypep 'unbound-slot 'cell-error)"), "T");
    ASSERT_STR_EQ(eval_print("(subtypep 'unbound-slot 'error)"), "T");
    /* A handler for any supertype still catches it. */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (slot-value (make-instance 'ubs-a) 'x) "
        "  (cell-error () :cell))"),
        ":CELL");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (slot-value (make-instance 'ubs-a) 'x) (error () :err))"),
        ":ERR");
}

TEST(unbound_slot_report_names_slot_and_instance)
{
    /* :format-control / :format-arguments still interpolate, so the printed
       report is unchanged from the old bare-ERROR message. */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (slot-value (make-instance 'ubs-a) 'x) "
        "  (unbound-slot (c) (and (search \"The slot X is unbound\" "
        "                                 (princ-to-string c)) t)))"),
        "T");
}

TEST(unbound_slot_instance_accessor)
{
    /* Constructible by hand, and the accessor reads the :INSTANCE initarg. */
    ASSERT_STR_EQ(eval_print(
        "(typep (make-condition 'unbound-slot :name 'q :instance 42) 'unbound-slot)"),
        "T");
    ASSERT_STR_EQ(eval_print(
        "(unbound-slot-instance (make-condition 'unbound-slot :name 'q :instance 42))"),
        "42");
    /* Absent :instance reads as NIL, not an error. */
    ASSERT_STR_EQ(eval_print(
        "(unbound-slot-instance (make-condition 'unbound-slot :name 'q))"),
        "NIL");
    /* Not a condition -> type error. */
    ASSERT(strncmp(eval_print("(unbound-slot-instance 42)"), "ERROR:", 6) == 0);
}

/* --- SLOT-VALUE compiler-macro inline + (type, slot) pair index ---
   SLOT-VALUE and (SETF SLOT-VALUE) compile to an inlined fast-path test
   (compiler macros in clos.lisp; compile_setf routes defsetf updaters
   through compile_call so the %SET-SLOT-VALUE macro fires), and
   struct_slot_resolve answers through the pair index rebuilt alongside
   the struct type index.  These pin the behaviors the inline and the
   index must not change.  NOTE: they exercise the fast path, so they
   must run BEFORE anything defines a SLOT-VALUE-USING-CLASS method —
   *SLOT-ACCESS-PROTOCOL-EXTENDED-P* is a one-way latch. */

TEST(slot_value_inline_setf_eval_order)
{
    /* Place subforms evaluate once each, left-to-right, before the
       value (CLHS 5.1.1.1.1) — the gensym-let expansion and the
       compile_setf call-form rewrite must both preserve that.  The
       slot-name is a non-constant form here, covering the macro's
       gensym-bound name path. */
    eval_print("(defclass svi-a () ((x :initform 1)))");
    eval_print("(defvar *svi-log* nil)");
    eval_print("(defvar *svi-inst* (make-instance 'svi-a))");
    ASSERT_STR_EQ(eval_print(
        "(progn (setq *svi-log* nil) "
        "       (setf (slot-value (progn (push :obj *svi-log*) *svi-inst*) "
        "                         (progn (push :name *svi-log*) 'x)) "
        "             (progn (push :val *svi-log*) 5)) "
        "       (list (nreverse *svi-log*) (slot-value *svi-inst* 'x)))"),
        "((:OBJ :NAME :VAL) 5)");
}

TEST(slot_value_inline_read_eval_order)
{
    ASSERT_STR_EQ(eval_print(
        "(progn (setq *svi-log* nil) "
        "       (list (slot-value (progn (push :obj *svi-log*) *svi-inst*) "
        "                         (progn (push :name *svi-log*) 'x)) "
        "             (nreverse *svi-log*)))"),
        "(5 (:OBJ :NAME))");
}

TEST(slot_value_inline_setf_returns_value)
{
    ASSERT_STR_EQ(eval_print("(setf (slot-value *svi-inst* 'x) 11)"), "11");
    ASSERT_STR_EQ(eval_print("(slot-value *svi-inst* 'x)"), "11");
}

TEST(slot_value_inline_no_such_slot_errors)
{
    ASSERT(strncmp(eval_print("(slot-value *svi-inst* 'no-such-slot)"),
                   "ERROR:", 6) == 0);
    ASSERT(strncmp(eval_print("(setf (slot-value *svi-inst* 'no-such-slot) 1)"),
                   "ERROR:", 6) == 0);
}

TEST(slot_value_funcall_bypasses_inline)
{
    /* FUNCALL/APPLY of #'SLOT-VALUE goes through the DEFUN, not the
       compiler macro — same answer either way. */
    ASSERT_STR_EQ(eval_print("(funcall #'slot-value *svi-inst* 'x)"), "11");
    ASSERT_STR_EQ(eval_print("(apply #'slot-value (list *svi-inst* 'x))"), "11");
}

TEST(slot_value_pair_index_wide_class)
{
    /* Every slot of a 12-slot class resolves through the pair index —
       a wrong or colliding pair entry reads the wrong slot. */
    eval_print("(defclass svi-w () "
               "  ((s0 :initform 0) (s1 :initform 10) (s2 :initform 20) "
               "   (s3 :initform 30) (s4 :initform 40) (s5 :initform 50) "
               "   (s6 :initform 60) (s7 :initform 70) (s8 :initform 80) "
               "   (s9 :initform 90) (s10 :initform 100) (s11 :initform 110)))");
    ASSERT_STR_EQ(eval_print(
        "(let ((p (make-instance 'svi-w))) "
        "  (list (slot-value p 's0) (slot-value p 's5) (slot-value p 's11)))"),
        "(0 50 110)");
}

TEST(slot_value_pair_index_class_redef)
{
    /* Redefinition re-registers the type: the pair index must go dirty
       and rebuild, or a stale (type, slot) -> index entry silently
       reads the wrong slot of instances created after the redef. */
    eval_print("(defclass svi-r () ((a :initform 1) (b :initform 2)))");
    ASSERT_STR_EQ(eval_print("(slot-value (make-instance 'svi-r) 'b)"), "2");
    eval_print("(defclass svi-r () "
               "  ((pad1 :initform 0) (pad2 :initform 0) (b :initform 9)))");
    ASSERT_STR_EQ(eval_print("(slot-value (make-instance 'svi-r) 'b)"), "9");
    ASSERT_STR_EQ(eval_print(
        "(setf (slot-value (make-instance 'svi-r) 'pad1) 33)"), "33");
}

TEST(slot_exists_p_true)
{
    ASSERT_STR_EQ(eval_print("(slot-exists-p *test-pt* 'x)"), "T");
}

TEST(slot_exists_p_false)
{
    ASSERT_STR_EQ(eval_print("(slot-exists-p *test-pt* 'z)"), "NIL");
}

TEST(slot_value_no_such_slot)
{
    ASSERT(strncmp(eval_print("(slot-value *test-pt* 'z)"), "ERROR:", 6) == 0);
}

/* === Phase 2: C3 Linearization === */

/* Test C3 with single inheritance (should match simple CPL) */
TEST(c3_single_inheritance)
{
    /* fixnum already has a CPL from bootstrap; verify C3 gives the same */
    ASSERT_STR_EQ(eval_print(
        "(mapcar #'class-name "
        "  (%compute-class-precedence-list (find-class 'fixnum)))"),
        "(FIXNUM INTEGER RATIONAL REAL NUMBER T)");
}

/* Test C3 with multiple inheritance — the classic diamond */
TEST(c3_diamond)
{
    /* Create: A, B(A), C(A), D(B C) */
    eval_print(
        "(progn "
        "  (%make-bootstrap-class 'c3-a (list (find-class 't))) "
        "  (%make-bootstrap-class 'c3-b (list (find-class 'c3-a))) "
        "  (%make-bootstrap-class 'c3-c (list (find-class 'c3-a))) "
        "  t)");
    ASSERT_STR_EQ(eval_print(
        "(let ((d (%make-struct 'standard-class "
        "           'c3-d "
        "           (list (find-class 'c3-b) (find-class 'c3-c)) "
        "           nil nil nil nil nil nil nil t nil nil))) "
        "  (setf (find-class 'c3-d) d) "
        "  (%set-class-cpl d (%compute-class-precedence-list d)) "
        "  (mapcar #'class-name (class-precedence-list d)))"),
        "(C3-D C3-B C3-C C3-A T)");
}

/* C3 with T root — basic case */
TEST(c3_root)
{
    ASSERT_STR_EQ(eval_print(
        "(mapcar #'class-name "
        "  (%compute-class-precedence-list (find-class 't)))"),
        "(T)");
}

/* === Phase 3: defclass === */

TEST(defclass_basic)
{
    /* defclass returns class metaobject — hook should print it concisely */
    ASSERT_STR_EQ(eval_print(
        "(defclass point () "
        "  ((x :initarg :x :accessor point-x) "
        "   (y :initarg :y :accessor point-y)))"),
        "#<STANDARD-CLASS POINT>");
}

TEST(defclass_find_class)
{
    ASSERT_STR_EQ(eval_print("(class-name (find-class 'point))"), "POINT");
}

TEST(defclass_cpl)
{
    ASSERT_STR_EQ(eval_print(
        "(mapcar #'class-name (class-precedence-list (find-class 'point)))"),
        "(POINT STANDARD-OBJECT T)");
}

TEST(defclass_effective_slots)
{
    /* Should have 2 effective slots */
    ASSERT_STR_EQ(eval_print(
        "(length (class-effective-slots (find-class 'point)))"),
        "2");
}

TEST(defclass_slot_index_table)
{
    /* The index-table maps slot-name -> effective-slot-definition;
       SLOT-DEFINITION-LOCATION gives the struct storage index. */
    ASSERT_STR_EQ(eval_print(
        "(slot-definition-location "
        " (gethash 'x (class-slot-index-table (find-class 'point))))"),
        "0");
    ASSERT_STR_EQ(eval_print(
        "(slot-definition-location "
        " (gethash 'y (class-slot-index-table (find-class 'point))))"),
        "1");
}

TEST(defclass_accessor_defined)
{
    /* point-x and point-y should be functions */
    ASSERT_STR_EQ(eval_print("(functionp #'point-x)"), "T");
    ASSERT_STR_EQ(eval_print("(functionp #'point-y)"), "T");
}

TEST(defclass_inheritance)
{
    eval_print(
        "(defclass point3d (point) "
        "  ((z :initarg :z :accessor point-z)))");
    ASSERT_STR_EQ(eval_print(
        "(length (class-effective-slots (find-class 'point3d)))"),
        "3");
    ASSERT_STR_EQ(eval_print(
        "(mapcar #'class-name (class-precedence-list (find-class 'point3d)))"),
        "(POINT3D POINT STANDARD-OBJECT T)");
}

TEST(defclass_reader_writer)
{
    eval_print(
        "(defclass rw-test () "
        "  ((val :reader get-val :writer set-val)))");
    ASSERT_STR_EQ(eval_print("(functionp #'get-val)"), "T");
    ASSERT_STR_EQ(eval_print("(functionp #'set-val)"), "T");
}

TEST(defclass_no_supers)
{
    /* No explicit supers → defaults to standard-object */
    eval_print("(defclass empty-class () ())");
    ASSERT_STR_EQ(eval_print(
        "(mapcar #'class-name "
        "  (class-direct-superclasses (find-class 'empty-class)))"),
        "(STANDARD-OBJECT)");
}

/* === Phase 4: make-instance === */

TEST(make_instance_basic)
{
    /* point class already defined from Phase 3 tests */
    ASSERT_STR_EQ(eval_print(
        "(let ((p (make-instance 'point :x 10 :y 20))) "
        "  (list (point-x p) (point-y p)))"),
        "(10 20)");
}

TEST(make_instance_initform)
{
    eval_print(
        "(defclass counted () "
        "  ((count :initform 0 :accessor obj-count) "
        "   (name :initarg :name :accessor obj-name)))");
    ASSERT_STR_EQ(eval_print(
        "(let ((c (make-instance 'counted :name \"test\"))) "
        "  (list (obj-count c) (obj-name c)))"),
        "(0 \"test\")");
}

TEST(make_instance_initarg_overrides_initform)
{
    /* Define class with initarg AND initform on same slot */
    eval_print(
        "(defclass counter () "
        "  ((val :initarg :val :initform 0 :accessor counter-val)))");
    ASSERT_STR_EQ(eval_print(
        "(counter-val (make-instance 'counter :val 42))"),
        "42");
    /* Without initarg, should get initform default */
    ASSERT_STR_EQ(eval_print(
        "(counter-val (make-instance 'counter))"),
        "0");
}

TEST(make_instance_unbound_slot)
{
    /* Slot without initarg or initform stays unbound */
    ASSERT_STR_EQ(eval_print(
        "(let ((c (make-instance 'counted))) "
        "  (slot-boundp c 'name))"),
        "NIL");
}

TEST(make_instance_by_class_object)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((p (make-instance (find-class 'point) :x 1 :y 2))) "
        "  (point-x p))"),
        "1");
}

TEST(make_instance_inherited_slots)
{
    /* point3d inherits x, y from point, adds z */
    ASSERT_STR_EQ(eval_print(
        "(let ((p (make-instance 'point3d :x 1 :y 2 :z 3))) "
        "  (list (point-x p) (point-y p) (point-z p)))"),
        "(1 2 3)");
}

/* === Phase 5: defgeneric + defmethod + dispatch === */

TEST(defgeneric_basic)
{
    eval_print("(defgeneric greet (obj))");
    ASSERT_STR_EQ(eval_print(
        "(gethash 'greet *generic-function-table*)"),
        "#<STANDARD-GENERIC-FUNCTION GREET>");
}

TEST(defmethod_basic)
{
    eval_print(
        "(defmethod greet ((p point)) "
        "  (list 'point (point-x p) (point-y p)))");
    ASSERT_STR_EQ(eval_print(
        "(greet (make-instance 'point :x 10 :y 20))"),
        "(POINT 10 20)");
}

TEST(defmethod_dispatch_by_class)
{
    eval_print(
        "(defmethod greet ((p point3d)) "
        "  (list 'point3d (point-x p) (point-y p) (point-z p)))");
    /* point3d gets its own method */
    ASSERT_STR_EQ(eval_print(
        "(greet (make-instance 'point3d :x 1 :y 2 :z 3))"),
        "(POINT3D 1 2 3)");
    /* point still gets the point method */
    ASSERT_STR_EQ(eval_print(
        "(greet (make-instance 'point :x 5 :y 6))"),
        "(POINT 5 6)");
}

TEST(defmethod_call_next_method)
{
    eval_print("(defgeneric describe-obj (obj))");
    eval_print(
        "(defmethod describe-obj ((p point)) "
        "  (list 'base-point (point-x p)))");
    eval_print(
        "(defmethod describe-obj ((p point3d)) "
        "  (cons 'extended (call-next-method)))");
    ASSERT_STR_EQ(eval_print(
        "(describe-obj (make-instance 'point3d :x 7 :y 8 :z 9))"),
        "(EXTENDED BASE-POINT 7)");
}

TEST(defmethod_before_after)
{
    eval_print("(defvar *trace* nil)");
    eval_print("(defgeneric traced-op (obj))");
    eval_print(
        "(defmethod traced-op :before ((p point)) "
        "  (push 'before *trace*))");
    eval_print(
        "(defmethod traced-op ((p point)) "
        "  (push 'primary *trace*) 'done)");
    eval_print(
        "(defmethod traced-op :after ((p point)) "
        "  (push 'after *trace*))");
    eval_print("(setq *trace* nil)");
    ASSERT_STR_EQ(eval_print(
        "(traced-op (make-instance 'point :x 0 :y 0))"),
        "DONE");
    ASSERT_STR_EQ(eval_print(
        "(nreverse *trace*)"),
        "(BEFORE PRIMARY AFTER)");
}

TEST(defmethod_around)
{
    eval_print("(defgeneric wrapped-op (obj))");
    eval_print(
        "(defmethod wrapped-op ((p point)) "
        "  (point-x p))");
    eval_print(
        "(defmethod wrapped-op :around ((p point)) "
        "  (+ 100 (call-next-method)))");
    ASSERT_STR_EQ(eval_print(
        "(wrapped-op (make-instance 'point :x 5 :y 0))"),
        "105");
}

TEST(defmethod_unspecialized)
{
    eval_print("(defgeneric identity-gf (x))");
    eval_print("(defmethod identity-gf (x) x)");
    ASSERT_STR_EQ(eval_print("(identity-gf 42)"), "42");
    ASSERT_STR_EQ(eval_print("(identity-gf \"hello\")"), "\"hello\"");
}

TEST(defmethod_no_applicable)
{
    eval_print("(defgeneric specific-only (x))");
    eval_print(
        "(defmethod specific-only ((x point)) 'ok)");
    ASSERT(strncmp(eval_print("(specific-only 42)"), "ERROR:", 6) == 0);
}

TEST(next_method_p_test)
{
    eval_print("(defgeneric nmp-test (x))");
    eval_print(
        "(defmethod nmp-test ((x point)) "
        "  (if (next-method-p) 'has-next 'no-next))");
    ASSERT_STR_EQ(eval_print(
        "(nmp-test (make-instance 'point :x 0 :y 0))"),
        "NO-NEXT");
}

/* === Phase 6: with-slots === */

TEST(with_slots_read)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((p (make-instance 'point :x 10 :y 20))) "
        "  (with-slots (x y) p (list x y)))"),
        "(10 20)");
}

TEST(with_slots_write)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((p (make-instance 'point :x 1 :y 2))) "
        "  (with-slots (x) p (setf x 99)) "
        "  (point-x p))"),
        "99");
}

TEST(with_slots_renamed)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((p (make-instance 'point :x 5 :y 6))) "
        "  (with-slots ((px x) (py y)) p (+ px py)))"),
        "11");
}

/* === Phase 7: Generic function accessors + init protocol === */

TEST(gf_accessor)
{
    /* Accessors defined after Phase 7 should be generic functions */
    eval_print(
        "(defclass gf-point () "
        "  ((x :initarg :x :accessor gf-point-x) "
        "   (y :initarg :y :accessor gf-point-y)))");
    ASSERT_STR_EQ(eval_print(
        "(gethash 'gf-point-x *generic-function-table*)"),
        "#<STANDARD-GENERIC-FUNCTION GF-POINT-X>");
}

TEST(gf_accessor_works)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((p (make-instance 'gf-point :x 10 :y 20))) "
        "  (list (gf-point-x p) (gf-point-y p)))"),
        "(10 20)");
}

TEST(gf_accessor_setf)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((p (make-instance 'gf-point :x 1 :y 2))) "
        "  (setf (gf-point-x p) 99) "
        "  (gf-point-x p))"),
        "99");
}

/* Regression (local-time/local-time-duration): a CLOS :accessor named the same
 * in two different packages must produce DISTINCT (setf accessor) writer GFs.
 * Previously both shared the %SETF-<name> hidden symbol, so the second class's
 * writer GF clobbered the first's — (setf (accessor obj)) on the first class's
 * instance hit "No applicable method".  Now keyed by %SETF-<package>::<name>. */
TEST(gf_accessor_setf_distinct_per_package)
{
    /* Packages must exist before the package-qualified accessor symbols are
     * READ, so create them in their own eval_print calls (not inside one form). */
    eval_print("(defpackage :clos-setf-a (:use :cl))");
    eval_print("(defpackage :clos-setf-b (:use :cl))");
    eval_print("(defclass csa () ((v :accessor clos-setf-a::sec-of :initform 1)))");
    eval_print("(defclass csb () ((v :accessor clos-setf-b::sec-of :initform 2)))");
    ASSERT_STR_EQ(eval_print(
        "(let ((a (make-instance 'csa)) (b (make-instance 'csb)))"
        "  (setf (clos-setf-a::sec-of a) 100)"
        "  (setf (clos-setf-b::sec-of b) 200)"
        "  (list (clos-setf-a::sec-of a) (clos-setf-b::sec-of b)))"),
        "(100 200)");
}

TEST(gf_initialize_instance)
{
    /* initialize-instance should be a GF now — add a custom :after method */
    eval_print("(defvar *init-log* nil)");
    eval_print(
        "(defmethod initialize-instance :after ((p gf-point) &rest initargs) "
        "  (push 'gf-point-initialized *init-log*))");
    eval_print("(setq *init-log* nil)");
    eval_print("(make-instance 'gf-point :x 1 :y 2)");
    ASSERT_STR_EQ(eval_print("*init-log*"), "(GF-POINT-INITIALIZED)");
}

/* === Phase 8: change-class + reinitialize-instance === */

TEST(reinitialize_instance_basic)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((p (make-instance 'gf-point :x 1 :y 2))) "
        "  (reinitialize-instance p :x 99) "
        "  (gf-point-x p))"),
        "99");
}

TEST(change_class_basic)
{
    /* Define a color-point with x, y, color */
    eval_print(
        "(defclass color-point (gf-point) "
        "  ((color :initarg :color :initform :red :accessor point-color)))");
    ASSERT_STR_EQ(eval_print(
        "(let* ((p (make-instance 'gf-point :x 10 :y 20)) "
        "       (cp (change-class p 'color-point))) "
        "  (list (gf-point-x cp) (gf-point-y cp) (point-color cp)))"),
        "(10 20 :RED)");
}

TEST(change_class_with_initargs)
{
    ASSERT_STR_EQ(eval_print(
        "(let* ((p (make-instance 'gf-point :x 1 :y 2)) "
        "       (cp (change-class p 'color-point :color :blue))) "
        "  (point-color cp))"),
        ":BLUE");
}

/* === Phase 9: print-object === */

TEST(print_object_class)
{
    ASSERT_STR_EQ(eval_print("(find-class 'integer)"),
        "#<STANDARD-CLASS INTEGER>");
}

TEST(print_object_gf)
{
    ASSERT_STR_EQ(eval_print("(gethash 'greet *generic-function-table*)"),
        "#<STANDARD-GENERIC-FUNCTION GREET>");
}

TEST(print_object_custom)
{
    eval_print(
        "(defclass printable () "
        "  ((tag :initarg :tag :accessor printable-tag)))");
    eval_print(
        "(defmethod print-object ((obj printable) stream) "
        "  (concatenate 'string \"#<PRINTABLE \" "
        "    (printable-tag obj) \">\"))");
    ASSERT_STR_EQ(eval_print(
        "(make-instance 'printable :tag \"hello\")"),
        "#<PRINTABLE hello>");
}

/* CLHS 22.1.3.12: *print-level* applies BEFORE print-object dispatch — at
 * the level limit the object prints as "#" and the custom method is NOT
 * invoked.  Regression: mutually-referencing instances whose print-object
 * formats BOTH peers (fan-out 2) recursed hook->format->hook to the
 * printer's hard depth cap (2^60+ CLOS dispatches — chipi's item<->binding
 * log lines took minutes each). */
TEST(print_object_honors_print_level)
{
    eval_print(
        "(defclass plevel-node () "
        "  ((a :accessor plevel-a :initform nil) "
        "   (b :accessor plevel-b :initform nil)))");
    eval_print(
        "(defmethod print-object ((n plevel-node) s) "
        "  (format s \"#<pnode ~a ~a>\" (plevel-a n) (plevel-b n)))");
    ASSERT_STR_EQ(eval_print(
        "(let ((x (make-instance 'plevel-node)) "
        "      (y (make-instance 'plevel-node))) "
        "  (setf (plevel-a x) y (plevel-b x) y "
        "        (plevel-a y) x (plevel-b y) x) "
        "  (let ((*print-level* 2)) "
        "    (< (length (princ-to-string x)) 100)))"),
        "T");
}

/* Regression: a PRINT-OBJECT method whose output contains a non-ASCII
 * character comes back from the hook's with-output-to-string as a WIDE
 * string; the C hook-result check used to accept only narrow strings and
 * silently discarded the method's output, degrading the object to the raw
 * all-slots #S(...) form.  That exposes object-graph edges the method
 * deliberately elides — chipi's umlaut-labeled item graphs then printed
 * exponentially (minutes per log line in eta-hab). */
TEST(print_object_wide_string_result)
{
    eval_print(
        "(defclass wide-po-node () "
        "  ((w-label :accessor w-label :initform nil)))");
    eval_print(
        "(defmethod print-object ((n wide-po-node) s) "
        "  (format s \"#<wide-po ~a>\" (w-label n)))");
    eval_print("(defvar *wpo* (make-instance 'wide-po-node))");
    eval_print(
        "(setf (w-label *wpo*) "
        "  (concatenate 'string \"F\" (string (code-char 252)) \"llgrad\"))");
    /* method output honored, not raw #S(...) */
    ASSERT_STR_EQ(eval_print(
        "(eql 0 (search \"#<wide-po F\" (princ-to-string *wpo*)))"), "T");
    ASSERT_STR_EQ(eval_print(
        "(search \"#S(\" (princ-to-string *wpo*))"), "NIL");
    /* same through the format ~a C path */
    ASSERT_STR_EQ(eval_print(
        "(eql 0 (search \"#<wide-po\" (format nil \"~a\" *wpo*)))"), "T");
}

/* Same class of bug on the TYPE_CONDITION hook site: a :report whose
 * output contains a non-ASCII character must still reach the princ'd
 * text via the hook. */
TEST(condition_report_wide_string)
{
    eval_print(
        "(define-condition wide-po-cond (error) () "
        "  (:report (lambda (c s) (declare (ignore c)) "
        "     (format s \"kaputt: ~a!\" (string (code-char 252))))))");
    ASSERT_STR_EQ(eval_print(
        "(eql 0 (search \"kaputt: \" "
        "  (princ-to-string (make-condition 'wide-po-cond))))"), "T");
}

TEST(print_object_default_struct)
{
    /* Structs without a print-object method use default #S(...) */
    eval_print("(defstruct plain-st (a 1))");
    ASSERT_STR_EQ(eval_print("(make-plain-st)"),
        "#S(PLAIN-ST :A 1)");
}

/* === slot-value on DEFSTRUCT instances ===
 * Regression: lisp-namespace and other libraries use (with-slots ...) on
 * defstruct instances. slot-value must resolve slot names against the
 * struct's slot list when CLASS-OF returns a class without a
 * slot-index-table. */

TEST(slot_value_on_struct)
{
    eval_print("(defstruct point3 (x 0) (y 0) (z 0))");
    eval_print("(defvar *p3* (make-point3 :x 10 :y 20 :z 30))");
    ASSERT_STR_EQ(eval_print("(slot-value *p3* 'x)"), "10");
    ASSERT_STR_EQ(eval_print("(slot-value *p3* 'y)"), "20");
    ASSERT_STR_EQ(eval_print("(slot-value *p3* 'z)"), "30");
}

TEST(setf_slot_value_on_struct)
{
    eval_print("(defstruct named-box (name \"\") (value 0))");
    eval_print("(defvar *nb* (make-named-box :name \"n\" :value 1))");
    ASSERT_STR_EQ(eval_print("(setf (slot-value *nb* 'value) 99)"), "99");
    ASSERT_STR_EQ(eval_print("(slot-value *nb* 'value)"), "99");
    ASSERT_STR_EQ(eval_print("(named-box-value *nb*)"), "99");
}

TEST(with_slots_on_struct)
{
    eval_print("(defstruct ws-rec (alpha 1) (beta 2))");
    eval_print("(defvar *wsr* (make-ws-rec))");
    /* with-slots expands to slot-value — must work on structs */
    ASSERT_STR_EQ(eval_print(
        "(with-slots (alpha beta) *wsr* (+ alpha beta))"),
        "3");
    /* with-slots also supports setf of its bindings via symbol-macrolet */
    ASSERT_STR_EQ(eval_print(
        "(with-slots (alpha) *wsr* (setf alpha 42) alpha)"),
        "42");
    ASSERT_STR_EQ(eval_print("(ws-rec-alpha *wsr*)"), "42");
}

TEST(slot_boundp_on_struct)
{
    eval_print("(defstruct sb-rec (a 1))");
    eval_print("(defvar *sbr* (make-sb-rec))");
    /* Struct slots always report bound (they have defaults) */
    ASSERT_STR_EQ(eval_print("(slot-boundp *sbr* 'a)"), "T");
}

TEST(slot_exists_p_on_struct)
{
    eval_print("(defstruct sxp-rec (foo 1) (bar 2))");
    eval_print("(defvar *sxp* (make-sxp-rec))");
    ASSERT_STR_EQ(eval_print("(slot-exists-p *sxp* 'foo)"), "T");
    ASSERT_STR_EQ(eval_print("(slot-exists-p *sxp* 'bar)"), "T");
    ASSERT_STR_EQ(eval_print("(slot-exists-p *sxp* 'missing)"), "NIL");
}

TEST(slot_value_unknown_slot_struct)
{
    eval_print("(defstruct usk-rec (a 1))");
    eval_print("(defvar *usk* (make-usk-rec))");
    /* Accessing an unknown slot name signals an error */
    ASSERT(strncmp(eval_print("(slot-value *usk* 'nope)"), "ERROR:", 6) == 0);
}

/* === ERROR with :format-control formats its report (regression for ~S) === */

TEST(error_format_control_interpolates)
{
    /* Previously, (error "~S foo" x) left ~S literal in the report. */
    const char *out = eval_print(
        "(handler-case (error \"msg: ~S\" 42) "
        "  (simple-error (c) "
        "    (format nil \"~A\" c)))");
    /* Report must contain the interpolated "42", not the literal "~S" */
    ASSERT(strstr(out, "42") != NULL);
    ASSERT(strstr(out, "~S") == NULL);
}

/* === Phase 10: typep with multiple inheritance === */

TEST(typep_multiple_inheritance)
{
    /* Define a class hierarchy with multiple inheritance */
    eval_print("(defclass mixin-a () ())");
    eval_print("(defclass mixin-b () ())");
    eval_print("(defclass multi-child (mixin-a mixin-b) ())");
    eval_print("(defvar *mc* (make-instance 'multi-child))");

    /* Instance should be typep of both mixins */
    ASSERT_STR_EQ(eval_print("(typep *mc* 'multi-child)"), "T");
    ASSERT_STR_EQ(eval_print("(typep *mc* 'mixin-a)"), "T");
    ASSERT_STR_EQ(eval_print("(typep *mc* 'mixin-b)"), "T");
    ASSERT_STR_EQ(eval_print("(typep *mc* 'standard-object)"), "T");
    ASSERT_STR_EQ(eval_print("(typep *mc* 't)"), "T");
}

TEST(typep_multiple_inheritance_or)
{
    /* Compound (or ...) type with multiple inheritance */
    ASSERT_STR_EQ(eval_print(
        "(typep *mc* '(or mixin-a mixin-b))"), "T");
    ASSERT_STR_EQ(eval_print(
        "(typep *mc* '(and mixin-a mixin-b))"), "T");
}

TEST(typep_multiple_inheritance_negative)
{
    /* Should NOT match unrelated classes */
    eval_print("(defclass unrelated () ())");
    ASSERT_STR_EQ(eval_print("(typep *mc* 'unrelated)"), "NIL");
}

/* === Phase 11: Dispatch cache === */

TEST(cache_basic_dispatch)
{
    /* Define a GF and method, call it — should work with cache */
    eval_print(
        "(defgeneric cache-test-1 (x))");
    eval_print(
        "(defmethod cache-test-1 ((x point)) (point-x x))");
    ASSERT_STR_EQ(eval_print(
        "(cache-test-1 (make-instance 'point :x 42 :y 0))"),
        "42");
}

TEST(cache_hit_correct_result)
{
    /* Call GF twice with same-class arg — second call uses cache */
    ASSERT_STR_EQ(eval_print(
        "(cache-test-1 (make-instance 'point :x 77 :y 0))"),
        "77");
    /* Verify cache is populated (non-NIL) */
    ASSERT_STR_EQ(eval_print(
        "(let ((gf (gethash 'cache-test-1 *generic-function-table*))) "
        "  (not (null (gf-dispatch-cache gf))))"),
        "T");
}

TEST(cache_different_classes)
{
    /* GF dispatches correctly for different classes */
    eval_print(
        "(defmethod cache-test-1 ((x point3d)) (+ (point-x x) (point-z x)))");
    ASSERT_STR_EQ(eval_print(
        "(cache-test-1 (make-instance 'point3d :x 10 :y 0 :z 5))"),
        "15");
    /* point dispatch still works (cache rebuilt after defmethod) */
    ASSERT_STR_EQ(eval_print(
        "(cache-test-1 (make-instance 'point :x 33 :y 0))"),
        "33");
}

TEST(cache_invalidated_on_defmethod)
{
    /* Adding a method clears the cache */
    /* First populate cache */
    eval_print("(cache-test-1 (make-instance 'point :x 1 :y 0))");
    ASSERT_STR_EQ(eval_print(
        "(let ((gf (gethash 'cache-test-1 *generic-function-table*))) "
        "  (not (null (gf-dispatch-cache gf))))"),
        "T");
    /* Add another method — cache should be cleared */
    eval_print(
        "(defclass cache-thing () ((v :initarg :v :accessor cache-thing-v)))");
    eval_print(
        "(defmethod cache-test-1 ((x cache-thing)) (cache-thing-v x))");
    ASSERT_STR_EQ(eval_print(
        "(let ((gf (gethash 'cache-test-1 *generic-function-table*))) "
        "  (gf-dispatch-cache gf))"),
        "NIL");
    /* New method works */
    ASSERT_STR_EQ(eval_print(
        "(cache-test-1 (make-instance 'cache-thing :v 99))"),
        "99");
}

TEST(cache_invalidated_on_defclass)
{
    /* Defining a new class clears all GF caches */
    /* Populate cache */
    eval_print("(cache-test-1 (make-instance 'point :x 1 :y 0))");
    ASSERT_STR_EQ(eval_print(
        "(let ((gf (gethash 'cache-test-1 *generic-function-table*))) "
        "  (not (null (gf-dispatch-cache gf))))"),
        "T");
    /* Define a new class — all caches cleared */
    eval_print("(defclass cache-invalidation-test () ())");
    ASSERT_STR_EQ(eval_print(
        "(let ((gf (gethash 'cache-test-1 *generic-function-table*))) "
        "  (gf-dispatch-cache gf))"),
        "NIL");
}

TEST(cache_eql_specializer_bypass)
{
    /* GF with EQL specializer uses :EQL mode */
    eval_print(
        "(defgeneric cache-eql-test (x))");
    eval_print(
        "(defmethod cache-eql-test ((x (eql 42))) 'forty-two)");
    eval_print(
        "(defmethod cache-eql-test ((x integer)) 'other-int)");
    /* Should dispatch correctly */
    ASSERT_STR_EQ(eval_print("(cache-eql-test 42)"), "FORTY-TWO");
    ASSERT_STR_EQ(eval_print("(cache-eql-test 7)"), "OTHER-INT");
    /* cacheable-p should be :EQL */
    ASSERT_STR_EQ(eval_print(
        "(let ((gf (gethash 'cache-eql-test *generic-function-table*))) "
        "  (gf-cacheable-p gf))"),
        ":EQL");
}

TEST(cache_multi_dispatch_bypass)
{
    /* GF with specialization beyond arg1 uses multi-dispatch cache */
    eval_print(
        "(defgeneric cache-multi-test (x y))");
    eval_print(
        "(defmethod cache-multi-test ((x point) (y point)) 'both-points)");
    ASSERT_STR_EQ(eval_print(
        "(cache-multi-test (make-instance 'point :x 1 :y 2) "
        "                  (make-instance 'point :x 3 :y 4))"),
        "BOTH-POINTS");
    /* cacheable-p should be 2 (two specialized positions) */
    ASSERT_STR_EQ(eval_print(
        "(let ((gf (gethash 'cache-multi-test *generic-function-table*))) "
        "  (gf-cacheable-p gf))"),
        "2");
}

TEST(cache_no_applicable_method)
{
    /* No-applicable-method error should still work with cache */
    eval_print(
        "(defgeneric cache-err-test (x))");
    eval_print(
        "(defmethod cache-err-test ((x point)) 'point-ok)");
    /* This should work */
    ASSERT_STR_EQ(eval_print(
        "(cache-err-test (make-instance 'point :x 0 :y 0))"),
        "POINT-OK");
    /* Calling with a string (no applicable method) should error */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (cache-err-test \"hello\") "
        "  (error (c) 'got-error))"),
        "GOT-ERROR");
}

TEST(cache_before_after_around)
{
    /* Standard method combination works correctly with cache */
    eval_print("(defparameter *cache-trace* nil)");
    eval_print(
        "(defgeneric cache-combo-test (x))");
    eval_print(
        "(defmethod cache-combo-test :before ((x point)) "
        "  (push 'before *cache-trace*))");
    eval_print(
        "(defmethod cache-combo-test ((x point)) "
        "  (push 'primary *cache-trace*) 'done)");
    eval_print(
        "(defmethod cache-combo-test :after ((x point)) "
        "  (push 'after *cache-trace*))");
    eval_print(
        "(defmethod cache-combo-test :around ((x point)) "
        "  (push 'around *cache-trace*) (call-next-method))");
    /* First call */
    eval_print("(setq *cache-trace* nil)");
    ASSERT_STR_EQ(eval_print(
        "(cache-combo-test (make-instance 'point :x 0 :y 0))"),
        "DONE");
    ASSERT_STR_EQ(eval_print("(nreverse *cache-trace*)"),
        "(AROUND BEFORE PRIMARY AFTER)");
    /* Second call (should use cache) — same result */
    eval_print("(setq *cache-trace* nil)");
    ASSERT_STR_EQ(eval_print(
        "(cache-combo-test (make-instance 'point :x 1 :y 1))"),
        "DONE");
    ASSERT_STR_EQ(eval_print("(nreverse *cache-trace*)"),
        "(AROUND BEFORE PRIMARY AFTER)");
}

/* === Phase 12: EMF cache (effective method closure caching) === */

TEST(emf_cache_call_next_method_no_args)
{
    /* call-next-method with no args should pass original args through EMF cache */
    eval_print(
        "(defgeneric emf-cnm-test (x))");
    eval_print(
        "(defmethod emf-cnm-test ((x point3d)) "
        "  (list 'p3d (point-x x) (call-next-method)))");
    eval_print(
        "(defmethod emf-cnm-test ((x point)) "
        "  (list 'pt (point-x x)))");
    /* First call — populates cache */
    ASSERT_STR_EQ(eval_print(
        "(emf-cnm-test (make-instance 'point3d :x 10 :y 20 :z 30))"),
        "(P3D 10 (PT 10))");
    /* Second call — uses cached EMF, call-next-method still works */
    ASSERT_STR_EQ(eval_print(
        "(emf-cnm-test (make-instance 'point3d :x 99 :y 0 :z 0))"),
        "(P3D 99 (PT 99))");
}

TEST(emf_cache_call_next_method_with_args)
{
    /* call-next-method with explicit args should forward them */
    eval_print(
        "(defgeneric emf-cnm-args-test (x))");
    eval_print(
        "(defmethod emf-cnm-args-test ((x point3d)) "
        "  (call-next-method (make-instance 'point :x 42 :y 0)))");
    eval_print(
        "(defmethod emf-cnm-args-test ((x point)) "
        "  (point-x x))");
    ASSERT_STR_EQ(eval_print(
        "(emf-cnm-args-test (make-instance 'point3d :x 1 :y 2 :z 3))"),
        "42");
    /* Cached call */
    ASSERT_STR_EQ(eval_print(
        "(emf-cnm-args-test (make-instance 'point3d :x 7 :y 8 :z 9))"),
        "42");
}

TEST(emf_cache_before_after_around)
{
    /* Full method combination with EMF cache */
    eval_print("(defparameter *emf-trace* nil)");
    eval_print(
        "(defgeneric emf-combo (x))");
    eval_print(
        "(defmethod emf-combo :before ((x point)) "
        "  (push (list 'before (point-x x)) *emf-trace*))");
    eval_print(
        "(defmethod emf-combo ((x point)) "
        "  (push 'primary *emf-trace*) (point-x x))");
    eval_print(
        "(defmethod emf-combo :after ((x point)) "
        "  (push 'after *emf-trace*))");
    eval_print(
        "(defmethod emf-combo :around ((x point)) "
        "  (push 'around *emf-trace*) (call-next-method))");
    /* First call — builds EMF */
    eval_print("(setq *emf-trace* nil)");
    ASSERT_STR_EQ(eval_print(
        "(emf-combo (make-instance 'point :x 5 :y 0))"),
        "5");
    ASSERT_STR_EQ(eval_print("(nreverse *emf-trace*)"),
        "(AROUND (BEFORE 5) PRIMARY AFTER)");
    /* Second call — uses cached EMF, different arg values */
    eval_print("(setq *emf-trace* nil)");
    ASSERT_STR_EQ(eval_print(
        "(emf-combo (make-instance 'point :x 88 :y 0))"),
        "88");
    ASSERT_STR_EQ(eval_print("(nreverse *emf-trace*)"),
        "(AROUND (BEFORE 88) PRIMARY AFTER)");
}

TEST(emf_cache_next_method_p)
{
    /* next-method-p returns correct values in cached path */
    eval_print(
        "(defgeneric emf-nmp (x))");
    eval_print(
        "(defmethod emf-nmp ((x point3d)) "
        "  (list (next-method-p) (call-next-method)))");
    eval_print(
        "(defmethod emf-nmp ((x point)) "
        "  (next-method-p))");
    /* point3d has next, point does not */
    ASSERT_STR_EQ(eval_print(
        "(emf-nmp (make-instance 'point3d :x 0 :y 0 :z 0))"),
        "(T NIL)");
    /* Cached call */
    ASSERT_STR_EQ(eval_print(
        "(emf-nmp (make-instance 'point3d :x 1 :y 1 :z 1))"),
        "(T NIL)");
}

TEST(emf_cache_no_applicable_cached)
{
    /* Negative caching: no-applicable-method error is cached */
    eval_print(
        "(defgeneric emf-neg (x))");
    eval_print(
        "(defmethod emf-neg ((x point)) 'ok)");
    /* Works for point */
    ASSERT_STR_EQ(eval_print(
        "(emf-neg (make-instance 'point :x 0 :y 0))"),
        "OK");
    /* Errors for string — first call */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (emf-neg \"hello\") "
        "  (error (c) 'no-method))"),
        "NO-METHOD");
    /* Errors for string — second call uses negative cache */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (emf-neg \"world\") "
        "  (error (c) 'no-method))"),
        "NO-METHOD");
}

/* CLHS 7.6.6.3: NO-APPLICABLE-METHOD is a standard generic function called
 * when a GF is invoked with no applicable method.  Regression for the
 * serapeum NO-APPLICABLE-METHOD-ERROR test, which derives the dispatch-miss
 * condition type via (no-applicable-method #'no-applicable-method (list 1)). */
TEST(no_applicable_method_gf_exists)
{
    /* The standard function is bound and signals an error of type ERROR. */
    ASSERT_STR_EQ(eval_print("(fboundp 'no-applicable-method)"), "T");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (no-applicable-method #'no-applicable-method (list 1))"
        "  (error (e) (typep e 'error)))"),
        "T");
}

TEST(no_applicable_method_consistent_type)
{
    /* A direct call and a real dispatch miss signal the SAME condition type,
     * so the serapeum deftype (derived from the former) catches the latter. */
    eval_print("(defgeneric nam-gwnm (x))");
    ASSERT_STR_EQ(eval_print(
        "(eq (handler-case (no-applicable-method #'no-applicable-method (list 1))"
        "       (error (e) (type-of e)))"
        "    (handler-case (nam-gwnm 1)"
        "       (error (e) (type-of e))))"),
        "T");
}

TEST(no_applicable_method_serapeum_deftype)
{
    /* The full serapeum portability.lisp pattern: a deftype whose body
     * derives the dispatch-miss error type, then used to catch a real miss. */
    eval_print(
        "(deftype nam-err ()"
        "  (load-time-value"
        "   (handler-case (no-applicable-method #'no-applicable-method (list 1))"
        "     (error (e) (type-of e)))))");
    eval_print("(defgeneric nam-gwnm2 (x))");
    /* proper subtype of ERROR */
    ASSERT_STR_EQ(eval_print(
        "(and (subtypep 'nam-err 'error)"
        "     (not (nth-value 0 (subtypep 'error 'nam-err))))"),
        "T");
    /* catches a real dispatch miss */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (nam-gwnm2 1) (nam-err () 'caught))"),
        "CAUGHT");
}

/* CLHS 7.6.6.3: a user method on NO-APPLICABLE-METHOD that returns normally
 * must have its return values propagated — not silently discarded.  Regression
 * for %gf-dispatch using (when ...) instead of (return-from ... ...). */
TEST(no_applicable_method_return_value)
{
    eval_print("(defgeneric nam-rv-gf (x))");
    /* Install a method that returns a sentinel instead of signaling.
     * Eql-specialized on this GF so it doesn't affect any other test. */
    eval_print(
        "(defmethod no-applicable-method ((gf (eql #'nam-rv-gf)) &rest args)"
        "  (declare (ignore args))"
        "  42)");
    ASSERT_STR_EQ(eval_print("(nam-rv-gf 'anything)"), "42");
}

/* === Phase 13: Multi-dispatch cache === */

TEST(multi_cache_two_arg)
{
    /* 2-arg GF dispatches correctly with multi-dispatch cache */
    eval_print(
        "(defgeneric mc-test (x y))");
    eval_print(
        "(defmethod mc-test ((x point) (y point)) "
        "  (+ (point-x x) (point-x y)))");
    eval_print(
        "(defmethod mc-test ((x point3d) (y point)) "
        "  (+ (point-x x) (point-z x) (point-x y)))");
    ASSERT_STR_EQ(eval_print(
        "(mc-test (make-instance 'point :x 10 :y 0) "
        "         (make-instance 'point :x 20 :y 0))"),
        "30");
    ASSERT_STR_EQ(eval_print(
        "(mc-test (make-instance 'point3d :x 10 :y 0 :z 5) "
        "         (make-instance 'point :x 20 :y 0))"),
        "35");
}

TEST(multi_cache_hit)
{
    /* Second call with same class combo uses cache */
    ASSERT_STR_EQ(eval_print(
        "(mc-test (make-instance 'point :x 1 :y 0) "
        "         (make-instance 'point :x 2 :y 0))"),
        "3");
    /* Cache should be populated */
    ASSERT_STR_EQ(eval_print(
        "(let ((gf (gethash 'mc-test *generic-function-table*))) "
        "  (not (null (gf-dispatch-cache gf))))"),
        "T");
}

TEST(multi_cache_different_combos)
{
    /* Different class pairs cached separately */
    eval_print(
        "(defmethod mc-test ((x point) (y point3d)) "
        "  (+ (point-x x) (point-z y)))");
    ASSERT_STR_EQ(eval_print(
        "(mc-test (make-instance 'point :x 10 :y 0) "
        "         (make-instance 'point3d :x 1 :y 2 :z 30))"),
        "40");
    /* Previous combo still works */
    ASSERT_STR_EQ(eval_print(
        "(mc-test (make-instance 'point :x 5 :y 0) "
        "         (make-instance 'point :x 7 :y 0))"),
        "12");
}

TEST(multi_cache_cacheable_p_value)
{
    /* cacheable-p returns correct integer for multi-dispatch */
    ASSERT_STR_EQ(eval_print(
        "(let ((gf (gethash 'mc-test *generic-function-table*))) "
        "  (gf-cacheable-p gf))"),
        "2");
    /* Single-dispatch GF still returns 1 */
    ASSERT_STR_EQ(eval_print(
        "(let ((gf (gethash 'cache-test-1 *generic-function-table*))) "
        "  (gf-cacheable-p gf))"),
        "1");
}

TEST(multi_cache_invalidation)
{
    /* defmethod clears multi cache */
    /* Populate cache */
    eval_print(
        "(mc-test (make-instance 'point :x 1 :y 0) "
        "         (make-instance 'point :x 2 :y 0))");
    ASSERT_STR_EQ(eval_print(
        "(let ((gf (gethash 'mc-test *generic-function-table*))) "
        "  (not (null (gf-dispatch-cache gf))))"),
        "T");
    /* Add method — cache cleared */
    eval_print(
        "(defmethod mc-test ((x point3d) (y point3d)) "
        "  (+ (point-z x) (point-z y)))");
    ASSERT_STR_EQ(eval_print(
        "(let ((gf (gethash 'mc-test *generic-function-table*))) "
        "  (gf-dispatch-cache gf))"),
        "NIL");
    /* New method works */
    ASSERT_STR_EQ(eval_print(
        "(mc-test (make-instance 'point3d :x 0 :y 0 :z 7) "
        "         (make-instance 'point3d :x 0 :y 0 :z 3))"),
        "10");
}

TEST(multi_cache_call_next_method)
{
    /* call-next-method works in multi-dispatch */
    eval_print(
        "(defgeneric mc-cnm (x y))");
    eval_print(
        "(defmethod mc-cnm ((x point) (y point)) "
        "  (list 'base (point-x x) (point-x y)))");
    eval_print(
        "(defmethod mc-cnm ((x point3d) (y point)) "
        "  (list 'p3d (call-next-method)))");
    ASSERT_STR_EQ(eval_print(
        "(mc-cnm (make-instance 'point3d :x 10 :y 0 :z 5) "
        "        (make-instance 'point :x 20 :y 0))"),
        "(P3D (BASE 10 20))");
    /* Cached */
    ASSERT_STR_EQ(eval_print(
        "(mc-cnm (make-instance 'point3d :x 99 :y 0 :z 1) "
        "        (make-instance 'point :x 88 :y 0))"),
        "(P3D (BASE 99 88))");
}

/* === Phase 14: EQL specializer cache === */

TEST(eql_cache_basic)
{
    /* EQL dispatch works with cache */
    eval_print(
        "(defgeneric eql-c-test (x))");
    eval_print(
        "(defmethod eql-c-test ((x (eql 42))) 'forty-two)");
    eval_print(
        "(defmethod eql-c-test ((x integer)) 'some-int)");
    ASSERT_STR_EQ(eval_print("(eql-c-test 42)"), "FORTY-TWO");
    ASSERT_STR_EQ(eval_print("(eql-c-test 7)"), "SOME-INT");
}

TEST(eql_cache_hit)
{
    /* Second call with same EQL value uses cache */
    ASSERT_STR_EQ(eval_print("(eql-c-test 42)"), "FORTY-TWO");
    ASSERT_STR_EQ(eval_print("(eql-c-test 7)"), "SOME-INT");
    /* Cache should be populated */
    ASSERT_STR_EQ(eval_print(
        "(let ((gf (gethash 'eql-c-test *generic-function-table*))) "
        "  (not (null (gf-dispatch-cache gf))))"),
        "T");
}

TEST(eql_cache_mixed_eql_class)
{
    /* Multiple EQL specializers with class fallback */
    eval_print(
        "(defgeneric eql-mix (x))");
    eval_print(
        "(defmethod eql-mix ((x (eql :alpha))) 'got-alpha)");
    eval_print(
        "(defmethod eql-mix ((x (eql :beta))) 'got-beta)");
    eval_print(
        "(defmethod eql-mix ((x symbol)) 'some-symbol)");
    ASSERT_STR_EQ(eval_print("(eql-mix :alpha)"), "GOT-ALPHA");
    ASSERT_STR_EQ(eval_print("(eql-mix :beta)"), "GOT-BETA");
    ASSERT_STR_EQ(eval_print("(eql-mix :gamma)"), "SOME-SYMBOL");
    ASSERT_STR_EQ(eval_print("(eql-mix 'foo)"), "SOME-SYMBOL");
}

TEST(eql_cache_class_fallback)
{
    /* Non-EQL value falls back to class cache */
    ASSERT_STR_EQ(eval_print("(eql-c-test 100)"), "SOME-INT");
    ASSERT_STR_EQ(eval_print("(eql-c-test 200)"), "SOME-INT");
}

TEST(eql_cache_call_next_method)
{
    /* EQL method calls next class method via call-next-method */
    eval_print(
        "(defgeneric eql-cnm (x))");
    eval_print(
        "(defmethod eql-cnm ((x (eql 99))) "
        "  (list 'eql-99 (call-next-method)))");
    eval_print(
        "(defmethod eql-cnm ((x integer)) "
        "  (list 'int x))");
    ASSERT_STR_EQ(eval_print("(eql-cnm 99)"), "(EQL-99 (INT 99))");
    /* Cached */
    ASSERT_STR_EQ(eval_print("(eql-cnm 99)"), "(EQL-99 (INT 99))");
    /* Non-EQL path */
    ASSERT_STR_EQ(eval_print("(eql-cnm 50)"), "(INT 50)");
}

TEST(eql_cache_nil_value)
{
    /* (eql nil) specializer works correctly */
    eval_print(
        "(defgeneric eql-nil-test (x))");
    eval_print(
        "(defmethod eql-nil-test ((x (eql nil))) 'got-nil)");
    eval_print(
        "(defmethod eql-nil-test ((x t)) 'got-other)");
    ASSERT_STR_EQ(eval_print("(eql-nil-test nil)"), "GOT-NIL");
    ASSERT_STR_EQ(eval_print("(eql-nil-test 'foo)"), "GOT-OTHER");
    /* Cached */
    ASSERT_STR_EQ(eval_print("(eql-nil-test nil)"), "GOT-NIL");
}

TEST(eql_cache_arg2_eql)
{
    /* EQL on arg2 (ASDF pattern) */
    eval_print(
        "(defgeneric eql-arg2 (x y))");
    eval_print(
        "(defmethod eql-arg2 ((x t) (y (eql :load))) "
        "  (list 'loading x))");
    eval_print(
        "(defmethod eql-arg2 ((x t) (y (eql :compile))) "
        "  (list 'compiling x))");
    eval_print(
        "(defmethod eql-arg2 ((x t) (y t)) "
        "  (list 'default x y))");
    ASSERT_STR_EQ(eval_print("(eql-arg2 'foo :load)"), "(LOADING FOO)");
    ASSERT_STR_EQ(eval_print("(eql-arg2 'bar :compile)"), "(COMPILING BAR)");
    ASSERT_STR_EQ(eval_print("(eql-arg2 'baz :other)"), "(DEFAULT BAZ :OTHER)");
    /* Cached */
    ASSERT_STR_EQ(eval_print("(eql-arg2 'x :load)"), "(LOADING X)");
}

TEST(eql_cache_invalidation)
{
    /* Adding EQL method clears cache + recomputes sets */
    /* Populate cache */
    eval_print("(eql-c-test 42)");
    ASSERT_STR_EQ(eval_print(
        "(let ((gf (gethash 'eql-c-test *generic-function-table*))) "
        "  (not (null (gf-dispatch-cache gf))))"),
        "T");
    /* Add new EQL method — cache cleared */
    eval_print(
        "(defmethod eql-c-test ((x (eql 0))) 'zero)");
    ASSERT_STR_EQ(eval_print(
        "(let ((gf (gethash 'eql-c-test *generic-function-table*))) "
        "  (gf-dispatch-cache gf))"),
        "NIL");
    /* New method works */
    ASSERT_STR_EQ(eval_print("(eql-c-test 0)"), "ZERO");
    /* Old methods still work */
    ASSERT_STR_EQ(eval_print("(eql-c-test 42)"), "FORTY-TWO");
}

TEST(eql_cache_cacheable_p)
{
    /* cacheable-p returns :EQL for EQL-specialized GFs */
    ASSERT_STR_EQ(eval_print(
        "(let ((gf (gethash 'eql-c-test *generic-function-table*))) "
        "  (gf-cacheable-p gf))"),
        ":EQL");
}

/* === Slot Definition Metaobjects (MOP) === */

TEST(mop1_direct_slot_is_metaobject)
{
    /* class-direct-slots returns standard-direct-slot-definition instances */
    ASSERT_STR_EQ(eval_print(
        "(typep (first (class-direct-slots (find-class 'point))) "
        "       'standard-direct-slot-definition)"),
        "T");
    /* Also satisfies slot-definition and standard-slot-definition */
    ASSERT_STR_EQ(eval_print(
        "(typep (first (class-direct-slots (find-class 'point))) "
        "       'slot-definition)"),
        "T");
    ASSERT_STR_EQ(eval_print(
        "(typep (first (class-direct-slots (find-class 'point))) "
        "       'standard-slot-definition)"),
        "T");
}

TEST(mop1_effective_slot_is_metaobject)
{
    /* class-slots returns standard-effective-slot-definition instances */
    ASSERT_STR_EQ(eval_print(
        "(typep (first (class-slots (find-class 'point))) "
        "       'standard-effective-slot-definition)"),
        "T");
}

TEST(mop1_class_slots_alias)
{
    /* class-slots is an alias for class-effective-slots */
    ASSERT_STR_EQ(eval_print(
        "(equal (class-slots (find-class 'point)) "
        "       (class-effective-slots (find-class 'point)))"),
        "T");
    ASSERT_STR_EQ(eval_print(
        "(length (class-slots (find-class 'point)))"),
        "2");
}

TEST(mop1_slot_def_accessors_name)
{
    ASSERT_STR_EQ(eval_print(
        "(slot-definition-name "
        "  (first (class-direct-slots (find-class 'point))))"),
        "X");
    ASSERT_STR_EQ(eval_print(
        "(slot-definition-name "
        "  (second (class-direct-slots (find-class 'point))))"),
        "Y");
}

TEST(mop1_slot_def_accessors_initargs)
{
    /* slot-definition-initargs returns a list (per AMOP) */
    ASSERT_STR_EQ(eval_print(
        "(slot-definition-initargs "
        "  (first (class-direct-slots (find-class 'point))))"),
        "(:X)");
}

TEST(mop1_slot_def_accessors_readers_writers)
{
    /* readers and writers are exposed on direct slot defs */
    ASSERT_STR_EQ(eval_print(
        "(slot-definition-readers "
        "  (first (class-direct-slots (find-class 'point))))"),
        "(POINT-X)");
    ASSERT_STR_EQ(eval_print(
        "(slot-definition-writers "
        "  (first (class-direct-slots (find-class 'point))))"),
        "((SETF POINT-X))");
}

TEST(mop1_slot_def_accessors_allocation)
{
    /* Default allocation is :instance */
    ASSERT_STR_EQ(eval_print(
        "(slot-definition-allocation "
        "  (first (class-direct-slots (find-class 'point))))"),
        ":INSTANCE");
}

TEST(mop1_slot_def_accessors_initform)
{
    /* A class with an explicit :initform */
    eval_print(
        "(defclass mop1-has-initform () "
        "  ((n :initarg :n :initform 42)))");
    ASSERT_STR_EQ(eval_print(
        "(slot-definition-initform "
        "  (first (class-direct-slots (find-class 'mop1-has-initform))))"),
        "42");
    ASSERT_STR_EQ(eval_print(
        "(functionp "
        "  (slot-definition-initfunction "
        "    (first (class-direct-slots (find-class 'mop1-has-initform)))))"),
        "T");
    /* initfunction returns the initform value when called */
    ASSERT_STR_EQ(eval_print(
        "(funcall "
        "  (slot-definition-initfunction "
        "    (first (class-direct-slots (find-class 'mop1-has-initform)))))"),
        "42");
    /* make-instance uses the initform */
    ASSERT_STR_EQ(eval_print(
        "(slot-value (make-instance 'mop1-has-initform) 'n)"),
        "42");
}

TEST(mop1_effective_slot_location)
{
    /* effective slot location is the integer instance index */
    ASSERT_STR_EQ(eval_print(
        "(slot-definition-location "
        "  (first (class-slots (find-class 'point))))"),
        "0");
    ASSERT_STR_EQ(eval_print(
        "(slot-definition-location "
        "  (second (class-slots (find-class 'point))))"),
        "1");
}

TEST(mop1_direct_slot_definition_class_gf)
{
    /* direct-slot-definition-class is a GF returning the standard class */
    ASSERT_STR_EQ(eval_print(
        "(class-name "
        "  (direct-slot-definition-class (find-class 'standard-class)))"),
        "STANDARD-DIRECT-SLOT-DEFINITION");
    ASSERT_STR_EQ(eval_print(
        "(class-name "
        "  (effective-slot-definition-class (find-class 'standard-class)))"),
        "STANDARD-EFFECTIVE-SLOT-DEFINITION");
}

TEST(mop1_direct_slot_definition_class_customizable)
{
    /* Users can add an :around method — shows the GF dispatches properly */
    eval_print(
        "(defvar *mop1-dsdc-calls* 0)");
    eval_print(
        "(defmethod direct-slot-definition-class :around "
        "    ((class standard-class) &rest initargs) "
        "  (declare (ignore initargs)) "
        "  (incf *mop1-dsdc-calls*) "
        "  (call-next-method))");
    eval_print(
        "(direct-slot-definition-class (find-class 'standard-class))");
    /* The :around method ran at least once */
    ASSERT_STR_EQ(eval_print("(> *mop1-dsdc-calls* 0)"), "T");
    /* And still returned the standard slot-definition class */
    ASSERT_STR_EQ(eval_print(
        "(class-name "
        "  (direct-slot-definition-class (find-class 'standard-class)))"),
        "STANDARD-DIRECT-SLOT-DEFINITION");
}

TEST(mop1_inheritance_merges_slots)
{
    /* Effective slot count is the union of direct slots across CPL */
    eval_print(
        "(defclass mop1-base () "
        "  ((a :initarg :a :initform 'a-val)))");
    eval_print(
        "(defclass mop1-sub (mop1-base) "
        "  ((b :initarg :b :initform 'b-val)))");
    ASSERT_STR_EQ(eval_print(
        "(length (class-slots (find-class 'mop1-sub)))"),
        "2");
    /* Inherited slot's initfunction survives the merge */
    ASSERT_STR_EQ(eval_print(
        "(slot-value (make-instance 'mop1-sub) 'a)"),
        "A-VAL");
    ASSERT_STR_EQ(eval_print(
        "(slot-value (make-instance 'mop1-sub) 'b)"),
        "B-VAL");
}

TEST(mop1_inheritance_initargs_unioned)
{
    /* Per AMOP, initargs are unioned across all direct slots for an
       effective slot of that name. */
    eval_print(
        "(defclass mop1-p1 () "
        "  ((x :initarg :a)))");
    eval_print(
        "(defclass mop1-p2 (mop1-p1) "
        "  ((x :initarg :b)))");
    /* Both :a and :b should be valid initargs for the effective x slot */
    ASSERT_STR_EQ(eval_print(
        "(slot-value (make-instance 'mop1-p2 :a 1) 'x)"),
        "1");
    ASSERT_STR_EQ(eval_print(
        "(slot-value (make-instance 'mop1-p2 :b 2) 'x)"),
        "2");
}

/* === Class finalization protocol (MOP) === */

TEST(finalize_class_finalized_p)
{
    /* A defclassed class is finalized */
    eval_print("(defclass cf-f1 () ((x :initarg :x)))");
    ASSERT_STR_EQ(eval_print(
        "(class-finalized-p (find-class 'cf-f1))"),
        "T");
    /* Bootstrap classes are pre-finalized too */
    ASSERT_STR_EQ(eval_print(
        "(class-finalized-p (find-class 'standard-object))"),
        "T");
}

TEST(finalize_inheritance_idempotent)
{
    /* Calling finalize-inheritance on a finalized class must not error
       and must leave it finalized. */
    eval_print("(defclass cf-f2 () ((n :initarg :n :initform 7)))");
    ASSERT_STR_EQ(eval_print(
        "(progn (finalize-inheritance (find-class 'cf-f2)) "
        "       (class-finalized-p (find-class 'cf-f2)))"),
        "T");
    /* Twice is still fine. */
    ASSERT_STR_EQ(eval_print(
        "(progn (finalize-inheritance (find-class 'cf-f2)) "
        "       (finalize-inheritance (find-class 'cf-f2)) "
        "       (class-finalized-p (find-class 'cf-f2)))"),
        "T");
    /* Slots are still accessible — core machinery intact. */
    ASSERT_STR_EQ(eval_print(
        "(slot-value (make-instance 'cf-f2) 'n)"),
        "7");
}

TEST(compute_class_precedence_list_default)
{
    /* Default compute-class-precedence-list matches class-precedence-list */
    eval_print("(defclass cf-a () ())");
    eval_print("(defclass cf-b (cf-a) ())");
    ASSERT_STR_EQ(eval_print(
        "(mapcar #'class-name "
        "  (compute-class-precedence-list (find-class 'cf-b)))"),
        "(CF-B CF-A STANDARD-OBJECT T)");
}

TEST(compute_slots_returns_effective)
{
    eval_print("(defclass cf-cs () ((a :initarg :a) (b :initarg :b)))");
    /* compute-slots returns a fresh list of effective slot defs */
    ASSERT_STR_EQ(eval_print(
        "(length (compute-slots (find-class 'cf-cs)))"),
        "2");
    ASSERT_STR_EQ(eval_print(
        "(mapcar #'slot-definition-name "
        "  (compute-slots (find-class 'cf-cs)))"),
        "(A B)");
}

TEST(compute_effective_slot_definition_default)
{
    /* compute-effective-slot-definition merges a list of direct slots
       into a single effective slot. */
    eval_print("(defclass cf-ce () ((k :initarg :k :initform 1)))");
    ASSERT_STR_EQ(eval_print(
        "(slot-definition-name "
        "  (compute-effective-slot-definition "
        "    (find-class 'cf-ce) 'k "
        "    (class-direct-slots (find-class 'cf-ce))))"),
        "K");
    ASSERT_STR_EQ(eval_print(
        "(typep "
        "  (compute-effective-slot-definition "
        "    (find-class 'cf-ce) 'k "
        "    (class-direct-slots (find-class 'cf-ce))) "
        "  'standard-effective-slot-definition)"),
        "T");
}

TEST(compute_slots_around_hook)
{
    /* A :around method on compute-slots observes the standard result
       and can post-process — this is the key MOP acceptance test. */
    eval_print("(defclass cf-around () ((x :initarg :x)))");
    eval_print("(defvar *cf-around-saw* nil)");
    eval_print(
        "(defmethod compute-slots :around ((class standard-class)) "
        "  (let ((slots (call-next-method))) "
        "    (setq *cf-around-saw* (length slots)) "
        "    slots))");
    /* Force a re-finalization by redefining — defclass calls ensure-class
       which runs finalize-inheritance, which runs compute-slots. */
    eval_print(
        "(defclass cf-around () ((x :initarg :x) (y :initarg :y)))");
    ASSERT_STR_EQ(eval_print("*cf-around-saw*"), "2");
    /* Clean up the :around method so it doesn't bleed into later tests */
    eval_print(
        "(remove-method #'compute-slots "
        "  (find-method #'compute-slots '(:around) "
        "    (list (find-class 'standard-class))))");
}

TEST(compute_default_initargs_inherits)
{
    /* Default initargs defined on a superclass are inherited by the
       subclass's effective default-initargs. */
    eval_print(
        "(defclass cf-di-base () "
        "  ((a :initarg :a)) "
        "  (:default-initargs :a 99))");
    eval_print("(defclass cf-di-sub (cf-di-base) ())");
    /* The sub inherits :a default => slot a defaults to 99 */
    ASSERT_STR_EQ(eval_print(
        "(slot-value (make-instance 'cf-di-sub) 'a)"),
        "99");
    /* compute-default-initargs returns the merged list */
    ASSERT_STR_EQ(eval_print(
        "(first (first (compute-default-initargs "
        "                (find-class 'cf-di-sub))))"),
        ":A");
}

TEST(compute_default_initargs_most_specific_wins)
{
    /* When parent and child both specify a default for the same key,
       the child wins. */
    eval_print(
        "(defclass cf-di-p () "
        "  ((a :initarg :a)) "
        "  (:default-initargs :a 'parent))");
    eval_print(
        "(defclass cf-di-c (cf-di-p) () "
        "  (:default-initargs :a 'child))");
    ASSERT_STR_EQ(eval_print(
        "(slot-value (make-instance 'cf-di-c) 'a)"),
        "CHILD");
}

TEST(validate_superclass_default)
{
    /* validate-superclass returns T for all pairs (single-metaclass world) */
    ASSERT_STR_EQ(eval_print(
        "(validate-superclass (find-class 'standard-class) "
        "                     (find-class 'standard-object))"),
        "T");
    ASSERT_STR_EQ(eval_print(
        "(validate-superclass (find-class 't) (find-class 't))"),
        "T");
}

TEST(class_direct_default_initargs_accessor)
{
    eval_print(
        "(defclass cf-ddi () "
        "  ((x :initarg :x)) "
        "  (:default-initargs :x 42))");
    ASSERT_STR_EQ(eval_print(
        "(first (first (class-direct-default-initargs "
        "                (find-class 'cf-ddi))))"),
        ":X");
    /* The value slot is a lambda — invoking it yields the expression */
    ASSERT_STR_EQ(eval_print(
        "(funcall (second (first (class-direct-default-initargs "
        "                          (find-class 'cf-ddi)))))"),
        "42");
}

TEST(class_prototype_allocates)
{
    eval_print("(defclass cf-proto () ((x :initarg :x)))");
    /* class-prototype returns a fresh instance, cached on the class */
    ASSERT_STR_EQ(eval_print(
        "(typep (class-prototype (find-class 'cf-proto)) 'cf-proto)"),
        "T");
    /* Cached: same object every call */
    ASSERT_STR_EQ(eval_print(
        "(eq (class-prototype (find-class 'cf-proto)) "
        "    (class-prototype (find-class 'cf-proto)))"),
        "T");
}

TEST(ensure_class_creates)
{
    /* ensure-class creates a new class when none exists */
    eval_print(
        "(ensure-class 'cf-ec "
        "  :direct-superclasses '() "
        "  :direct-slots (list (%make-direct-slot-def "
        "                        'v '(:v) nil nil nil :instance nil nil nil)) "
        "  :direct-default-initargs '())");
    ASSERT_STR_EQ(eval_print(
        "(class-name (find-class 'cf-ec))"),
        "CF-EC");
    ASSERT_STR_EQ(eval_print(
        "(length (class-slots (find-class 'cf-ec)))"),
        "1");
}

TEST(ensure_class_using_class_dispatches)
{
    /* ensure-class-using-class is a GF that dispatches on the existing
       class (NIL when creating fresh). A user :around method sees the
       calls — observable regardless of whether the class exists. */
    eval_print("(defvar *cf-ecuc-calls* 0)");
    eval_print(
        "(defmethod ensure-class-using-class :around ((class t) name &rest keys) "
        "  (declare (ignore keys)) "
        "  (incf *cf-ecuc-calls*) "
        "  (call-next-method))");
    eval_print("(defclass cf-ecuc-a () ())");
    eval_print("(defclass cf-ecuc-b () ())");
    ASSERT_STR_EQ(eval_print("(>= *cf-ecuc-calls* 2)"), "T");
    /* Clean up so the :around method doesn't bleed into later tests */
    eval_print(
        "(remove-method #'ensure-class-using-class "
        "  (find-method #'ensure-class-using-class '(:around) "
        "    (list (find-class 't))))");
}

/* === Slot-access protocol (MOP) === */

TEST(svuc_fast_path_flag_clean)
{
    /* With no user methods installed, the extension flag remains NIL and
       slot-value takes the fast %struct-ref path. */
    eval_print("(defclass svuc-a () ((x :initarg :x :initform 10)))");
    ASSERT_STR_EQ(eval_print("*slot-access-protocol-extended-p*"), "NIL");
    ASSERT_STR_EQ(eval_print("(slot-value (make-instance 'svuc-a :x 7) 'x)"), "7");
}

TEST(svuc_default_method_matches_struct_ref)
{
    /* The default slot-value-using-class method must be observationally
       identical to %struct-ref — calling it directly yields the same
       value as slot-value. */
    eval_print("(defclass svuc-b () ((n :initarg :n)))");
    eval_print("(defvar *svuc-b-inst* (make-instance 'svuc-b :n 42))");
    ASSERT_STR_EQ(eval_print(
        "(slot-value-using-class (class-of *svuc-b-inst*) *svuc-b-inst* "
        "                        (first (class-slots (class-of *svuc-b-inst*))))"),
        "42");
}

TEST(svuc_around_observes_reads)
{
    /* A user :around method is invoked on every slot-value call and can
       see the effective-slot-definition. */
    eval_print("(defclass svuc-c () ((k :initarg :k :initform 'orig)))");
    eval_print("(defvar *svuc-c-log* nil)");
    eval_print(
        "(defmethod slot-value-using-class :around "
        "    ((class t) (inst svuc-c) slot) "
        "  (push (slot-definition-name slot) *svuc-c-log*) "
        "  (call-next-method))");
    ASSERT_STR_EQ(eval_print("*slot-access-protocol-extended-p*"), "T");
    ASSERT_STR_EQ(eval_print("(slot-value (make-instance 'svuc-c) 'k)"), "ORIG");
    ASSERT_STR_EQ(eval_print("(first *svuc-c-log*)"), "K");
    /* Clean up */
    eval_print(
        "(remove-method #'slot-value-using-class "
        "  (find-method #'slot-value-using-class '(:around) "
        "    (list (find-class 't) (find-class 'svuc-c) "
        "          (find-class 'standard-effective-slot-definition))))");
}

TEST(svuc_around_can_override_read)
{
    /* An :around method can transform the returned value by not calling
       call-next-method or by wrapping its result. */
    eval_print("(defclass svuc-d () ((v :initarg :v :initform 1)))");
    eval_print(
        "(defmethod slot-value-using-class :around "
        "    ((class t) (inst svuc-d) slot) "
        "  (declare (ignore slot)) "
        "  (* 10 (call-next-method)))");
    ASSERT_STR_EQ(eval_print("(slot-value (make-instance 'svuc-d :v 3) 'v)"), "30");
    /* Clean up */
    eval_print(
        "(remove-method #'slot-value-using-class "
        "  (find-method #'slot-value-using-class '(:around) "
        "    (list (find-class 't) (find-class 'svuc-d) "
        "          (find-class 'standard-effective-slot-definition))))");
}

TEST(svuc_setf_around_observes_writes)
{
    /* (setf (slot-value ...) val) dispatches through (setf slot-value-using-class)
       when the protocol is extended. */
    eval_print("(defclass svuc-e () ((w :initarg :w :initform 0)))");
    eval_print("(defvar *svuc-e-writes* nil)");
    eval_print(
        "(defmethod (setf slot-value-using-class) :around "
        "    (new-value (class t) (inst svuc-e) slot) "
        "  (push (cons (slot-definition-name slot) new-value) *svuc-e-writes*) "
        "  (call-next-method))");
    eval_print("(defvar *svuc-e-inst* (make-instance 'svuc-e))");
    eval_print("(setf (slot-value *svuc-e-inst* 'w) 99)");
    ASSERT_STR_EQ(eval_print("(slot-value *svuc-e-inst* 'w)"), "99");
    ASSERT_STR_EQ(eval_print("(cdr (first *svuc-e-writes*))"), "99");
    ASSERT_STR_EQ(eval_print("(car (first *svuc-e-writes*))"), "W");
    /* Clean up */
    eval_print(
        "(remove-method #'(setf slot-value-using-class) "
        "  (find-method #'(setf slot-value-using-class) '(:around) "
        "    (list (find-class 't) (find-class 't) (find-class 'svuc-e) "
        "          (find-class 'standard-effective-slot-definition))))");
}

TEST(svuc_boundp_dispatches)
{
    /* slot-boundp goes through slot-boundp-using-class when extended. */
    eval_print("(defclass svuc-f () ((q :initarg :q)))");
    eval_print("(defvar *svuc-f-saw* nil)");
    eval_print(
        "(defmethod slot-boundp-using-class :around "
        "    ((class t) (inst svuc-f) slot) "
        "  (declare (ignore slot)) "
        "  (setq *svuc-f-saw* t) "
        "  (call-next-method))");
    ASSERT_STR_EQ(eval_print(
        "(slot-boundp (make-instance 'svuc-f :q 1) 'q)"),
        "T");
    ASSERT_STR_EQ(eval_print("*svuc-f-saw*"), "T");
    ASSERT_STR_EQ(eval_print(
        "(slot-boundp (make-instance 'svuc-f) 'q)"),
        "NIL");
    eval_print(
        "(remove-method #'slot-boundp-using-class "
        "  (find-method #'slot-boundp-using-class '(:around) "
        "    (list (find-class 't) (find-class 'svuc-f) "
        "          (find-class 'standard-effective-slot-definition))))");
}

TEST(svuc_makunbound_dispatches)
{
    /* slot-makunbound goes through slot-makunbound-using-class when extended. */
    eval_print("(defclass svuc-g () ((r :initarg :r :initform 5)))");
    eval_print("(defvar *svuc-g-cleared* nil)");
    eval_print(
        "(defmethod slot-makunbound-using-class :around "
        "    ((class t) (inst svuc-g) slot) "
        "  (push (slot-definition-name slot) *svuc-g-cleared*) "
        "  (call-next-method))");
    eval_print("(defvar *svuc-g-inst* (make-instance 'svuc-g))");
    eval_print("(slot-makunbound *svuc-g-inst* 'r)");
    ASSERT_STR_EQ(eval_print("(slot-boundp *svuc-g-inst* 'r)"), "NIL");
    ASSERT_STR_EQ(eval_print("(first *svuc-g-cleared*)"), "R");
    eval_print(
        "(remove-method #'slot-makunbound-using-class "
        "  (find-method #'slot-makunbound-using-class '(:around) "
        "    (list (find-class 't) (find-class 'svuc-g) "
        "          (find-class 'standard-effective-slot-definition))))");
}

TEST(svuc_struct_fallback_unaffected)
{
    /* Slot access on DEFSTRUCT instances must not go through the protocol
       (structs have no effective-slot-definition). They keep using the
       struct-slot-name lookup path. */
    eval_print("(defstruct svuc-s a b)");
    eval_print("(defvar *svuc-s-inst* (make-svuc-s :a 100 :b 200))");
    /* Even after user methods extend the protocol for CLOS instances,
       struct slot-value should still bypass them. Install a method that
       would blow up if it saw a struct. */
    eval_print(
        "(defmethod slot-value-using-class :around ((class t) (inst t) slot) "
        "  (declare (ignore slot)) "
        "  (call-next-method))");
    ASSERT_STR_EQ(eval_print("(slot-value *svuc-s-inst* 'a)"), "100");
    ASSERT_STR_EQ(eval_print("(slot-value *svuc-s-inst* 'b)"), "200");
    /* Clean up */
    eval_print(
        "(remove-method #'slot-value-using-class "
        "  (find-method #'slot-value-using-class '(:around) "
        "    (list (find-class 't) (find-class 't) "
        "          (find-class 'standard-effective-slot-definition))))");
}

TEST(svuc_location_accessor)
{
    /* slot-definition-location returns the instance index on a finalized
       class — the value the default slot-value-using-class uses to find
       the slot in the struct layout. */
    eval_print("(defclass svuc-loc () ((first-slot :initarg :first) "
               "                       (second-slot :initarg :second)))");
    ASSERT_STR_EQ(eval_print(
        "(slot-definition-location "
        "  (first (class-slots (find-class 'svuc-loc))))"),
        "0");
    ASSERT_STR_EQ(eval_print(
        "(slot-definition-location "
        "  (second (class-slots (find-class 'svuc-loc))))"),
        "1");
}

TEST(svuc_setf_default_roundtrips)
{
    /* Calling the (setf slot-value-using-class) default directly
       round-trips. */
    eval_print("(defclass svuc-h () ((z :initarg :z :initform 1)))");
    eval_print("(defvar *svuc-h-inst* (make-instance 'svuc-h))");
    eval_print(
        "(setf (slot-value-using-class (class-of *svuc-h-inst*) *svuc-h-inst* "
        "                              (first (class-slots (class-of *svuc-h-inst*)))) "
        "      77)");
    ASSERT_STR_EQ(eval_print("(slot-value *svuc-h-inst* 'z)"), "77");
}

TEST(svuc_slot_unbound_still_fires)
{
    /* When a slot truly is unbound, the default method still dispatches
       to slot-unbound — even when the protocol is extended. */
    eval_print("(defclass svuc-i () ((u)))");
    eval_print(
        "(defmethod slot-unbound ((class t) (inst svuc-i) slot-name) "
        "  (declare (ignore class slot-name)) "
        "  :svuc-unbound-sentinel)");
    ASSERT_STR_EQ(eval_print(
        "(slot-value (make-instance 'svuc-i) 'u)"),
        ":SVUC-UNBOUND-SENTINEL");
    /* Clean up the slot-unbound method so later tests see the default */
    eval_print(
        "(remove-method #'slot-unbound "
        "  (find-method #'slot-unbound nil "
        "    (list (find-class 't) (find-class 'svuc-i) (find-class 't))))");
}

/* === :allocation :class — shared slots === */

TEST(calloc_two_instances_share)
{
    /* Two instances of a class with :allocation :class share writes
       to that slot (AMOP §5.5: class-allocated storage is per-class). */
    eval_print("(defclass cs-share () ((n :allocation :class :initform 0)))");
    eval_print("(defvar *cs-a* (make-instance 'cs-share))");
    eval_print("(defvar *cs-b* (make-instance 'cs-share))");
    eval_print("(setf (slot-value *cs-a* 'n) 42)");
    ASSERT_STR_EQ(eval_print("(slot-value *cs-b* 'n)"), "42");
}

TEST(calloc_subclass_inherits_cell)
{
    /* A subclass that does not redefine a class-allocated slot shares
       the same storage cell with its parent. */
    eval_print("(defclass cs-parent () ((x :allocation :class :initform 'hello)))");
    eval_print("(defclass cs-child (cs-parent) ())");
    eval_print("(defvar *cs-p* (make-instance 'cs-parent))");
    eval_print("(defvar *cs-c* (make-instance 'cs-child))");
    eval_print("(setf (slot-value *cs-p* 'x) 'world)");
    ASSERT_STR_EQ(eval_print("(slot-value *cs-c* 'x)"), "WORLD");
    ASSERT_STR_EQ(eval_print(
        "(eq (slot-definition-location "
        "      (first (class-slots (find-class 'cs-parent)))) "
        "    (slot-definition-location "
        "      (first (class-slots (find-class 'cs-child)))))"),
        "T");
}

TEST(calloc_subclass_redefines_gets_own_cell)
{
    /* A subclass that redefines a class-allocated slot in its own direct
       slots gets a fresh cell; writes stay separate from the parent. */
    eval_print("(defclass cs-p2 () ((x :allocation :class :initform 1)))");
    eval_print(
        "(defclass cs-c2 (cs-p2) "
        "  ((x :allocation :class :initform 2)))");
    eval_print("(defvar *cs-p2-inst* (make-instance 'cs-p2))");
    eval_print("(defvar *cs-c2-inst* (make-instance 'cs-c2))");
    ASSERT_STR_EQ(eval_print("(slot-value *cs-p2-inst* 'x)"), "1");
    ASSERT_STR_EQ(eval_print("(slot-value *cs-c2-inst* 'x)"), "2");
    eval_print("(setf (slot-value *cs-p2-inst* 'x) 100)");
    ASSERT_STR_EQ(eval_print("(slot-value *cs-c2-inst* 'x)"), "2");
    ASSERT_STR_EQ(eval_print("(slot-value *cs-p2-inst* 'x)"), "100");
}

TEST(calloc_location_is_cons)
{
    /* AMOP §5.5: SLOT-DEFINITION-LOCATION returns a cons cell for
       class-allocated slots. The cell's cdr is the actual storage —
       initialized by MAKE-INSTANCE (initforms run on first instance). */
    eval_print("(defclass cs-loc () ((shared :allocation :class :initform 'hi)))");
    ASSERT_STR_EQ(eval_print(
        "(consp (slot-definition-location "
        "  (first (class-slots (find-class 'cs-loc)))))"),
        "T");
    eval_print("(make-instance 'cs-loc)");
    ASSERT_STR_EQ(eval_print(
        "(cdr (slot-definition-location "
        "  (first (class-slots (find-class 'cs-loc)))))"),
        "HI");
}

TEST(calloc_location_mixed)
{
    /* Instance-allocated slots keep integer locations even when
       interleaved with class-allocated slots. */
    eval_print(
        "(defclass cs-mix () "
        "  ((a :initarg :a) "
        "   (b :allocation :class :initform 'shared) "
        "   (c :initarg :c)))");
    eval_print("(defvar *cs-mix-slots* (class-slots (find-class 'cs-mix)))");
    ASSERT_STR_EQ(eval_print(
        "(slot-definition-location "
        "  (find 'a *cs-mix-slots* :key #'slot-definition-name))"),
        "0");
    ASSERT_STR_EQ(eval_print(
        "(consp (slot-definition-location "
        "  (find 'b *cs-mix-slots* :key #'slot-definition-name)))"),
        "T");
    ASSERT_STR_EQ(eval_print(
        "(slot-definition-location "
        "  (find 'c *cs-mix-slots* :key #'slot-definition-name))"),
        "1");
    /* Reading still works for all three */
    eval_print("(defvar *cs-mix-i* (make-instance 'cs-mix :a 10 :c 30))");
    ASSERT_STR_EQ(eval_print("(slot-value *cs-mix-i* 'a)"), "10");
    ASSERT_STR_EQ(eval_print("(slot-value *cs-mix-i* 'b)"), "SHARED");
    ASSERT_STR_EQ(eval_print("(slot-value *cs-mix-i* 'c)"), "30");
}

TEST(calloc_boundp_and_makunbound)
{
    /* SLOT-BOUNDP and SLOT-MAKUNBOUND operate on the shared cell for
       class-allocated slots. */
    eval_print("(defclass cs-bp () ((s :allocation :class)))");
    eval_print("(defvar *cs-bp-a* (make-instance 'cs-bp))");
    eval_print("(defvar *cs-bp-b* (make-instance 'cs-bp))");
    ASSERT_STR_EQ(eval_print("(slot-boundp *cs-bp-a* 's)"), "NIL");
    eval_print("(setf (slot-value *cs-bp-a* 's) 7)");
    ASSERT_STR_EQ(eval_print("(slot-boundp *cs-bp-b* 's)"), "T");
    eval_print("(slot-makunbound *cs-bp-a* 's)");
    ASSERT_STR_EQ(eval_print("(slot-boundp *cs-bp-b* 's)"), "NIL");
}

TEST(calloc_initform_runs_once)
{
    /* A class-slot initform runs only when the cell is unbound — so the
       first MAKE-INSTANCE fills it and subsequent calls see the existing
       value. */
    eval_print("(defvar *cs-init-counter* 0)");
    eval_print(
        "(defclass cs-init () "
        "  ((k :allocation :class :initform (incf *cs-init-counter*))))");
    eval_print("(defvar *cs-init-a* (make-instance 'cs-init))");
    eval_print("(defvar *cs-init-b* (make-instance 'cs-init))");
    ASSERT_STR_EQ(eval_print("*cs-init-counter*"), "1");
    ASSERT_STR_EQ(eval_print("(slot-value *cs-init-a* 'k)"), "1");
    ASSERT_STR_EQ(eval_print("(slot-value *cs-init-b* 'k)"), "1");
}

TEST(calloc_initarg_writes_shared_cell)
{
    /* An :initarg targeting a class slot writes to the shared cell —
       so a later instance created without the initarg still sees that
       value (the cell is no longer unbound, initform is skipped). */
    eval_print(
        "(defclass cs-ia () "
        "  ((k :allocation :class :initarg :k :initform 0)))");
    eval_print("(defvar *cs-ia-a* (make-instance 'cs-ia :k 99))");
    eval_print("(defvar *cs-ia-b* (make-instance 'cs-ia))");
    ASSERT_STR_EQ(eval_print("(slot-value *cs-ia-a* 'k)"), "99");
    ASSERT_STR_EQ(eval_print("(slot-value *cs-ia-b* 'k)"), "99");
}

TEST(calloc_struct_smaller_than_effective_slots)
{
    /* With class slots, the underlying struct has fewer cells than the
       effective slot list — the bytecode layout must not alias a class
       slot onto an instance-slot struct index. */
    eval_print(
        "(defclass cs-sz () "
        "  ((ix :initarg :ix) "
        "   (shared :allocation :class :initform 'hi) "
        "   (iy :initarg :iy)))");
    eval_print("(defvar *cs-sz-inst* (make-instance 'cs-sz :ix 10 :iy 20))");
    /* Only 2 instance slots → struct size 2 → index 1 must be iy, not shared. */
    ASSERT_STR_EQ(eval_print("(%struct-ref *cs-sz-inst* 0)"), "10");
    ASSERT_STR_EQ(eval_print("(%struct-ref *cs-sz-inst* 1)"), "20");
}

/* === Reified EQL specializers (MOP) === */

TEST(eqlspec_intern_identity)
{
    /* Same value produces EQ metaobject (interned). */
    ASSERT_STR_EQ(eval_print(
        "(eq (intern-eql-specializer 42) (intern-eql-specializer 42))"),
        "T");
    ASSERT_STR_EQ(eval_print(
        "(eq (intern-eql-specializer 'foo) (intern-eql-specializer 'foo))"),
        "T");
    /* Different values → distinct metaobjects. */
    ASSERT_STR_EQ(eval_print(
        "(eq (intern-eql-specializer 42) (intern-eql-specializer 43))"),
        "NIL");
}

TEST(eqlspec_intern_eql_discriminates)
{
    /* Interning is by EQL, not EQ. Bignums that are EQL share a metaobject;
       objects that are neither EQ nor EQL do not. */
    ASSERT_STR_EQ(eval_print(
        "(eq (intern-eql-specializer 1.5) (intern-eql-specializer 1.5))"),
        "T");
    ASSERT_STR_EQ(eval_print(
        "(eq (intern-eql-specializer \"abc\") (intern-eql-specializer \"abc\"))"),
        "NIL");
}

TEST(eqlspec_object_accessor)
{
    ASSERT_STR_EQ(eval_print(
        "(eql-specializer-object (intern-eql-specializer 42))"),
        "42");
    ASSERT_STR_EQ(eval_print(
        "(eql-specializer-object (intern-eql-specializer 'hello))"),
        "HELLO");
    ASSERT_STR_EQ(eval_print(
        "(eql-specializer-object (intern-eql-specializer nil))"),
        "NIL");
}

TEST(eqlspec_p_predicate)
{
    ASSERT_STR_EQ(eval_print(
        "(eql-specializer-p (intern-eql-specializer 99))"),
        "T");
    /* Class objects are not eql-specializers. */
    ASSERT_STR_EQ(eval_print(
        "(eql-specializer-p (find-class 'integer))"),
        "NIL");
    /* Other things are not eql-specializers. */
    ASSERT_STR_EQ(eval_print("(eql-specializer-p 42)"), "NIL");
    ASSERT_STR_EQ(eval_print("(eql-specializer-p '(eql 42))"), "NIL");
    ASSERT_STR_EQ(eval_print("(eql-specializer-p nil)"), "NIL");
}

TEST(eqlspec_class_bootstrapped)
{
    ASSERT_STR_EQ(eval_print(
        "(class-name (class-of (intern-eql-specializer 42)))"),
        "EQL-SPECIALIZER");
    ASSERT_STR_EQ(eval_print(
        "(not (null (find-class 'eql-specializer)))"),
        "T");
}

TEST(eqlspec_dispatch_still_works)
{
    /* Regression guard: reification doesn't break EQL dispatch. */
    eval_print("(defgeneric es-disp (x))");
    eval_print("(defmethod es-disp ((x (eql 'alpha))) :got-alpha)");
    eval_print("(defmethod es-disp ((x (eql 'beta))) :got-beta)");
    eval_print("(defmethod es-disp ((x symbol)) :fallback)");
    ASSERT_STR_EQ(eval_print("(es-disp 'alpha)"), ":GOT-ALPHA");
    ASSERT_STR_EQ(eval_print("(es-disp 'beta)"),  ":GOT-BETA");
    ASSERT_STR_EQ(eval_print("(es-disp 'other)"), ":FALLBACK");
}

TEST(eqlspec_method_specializers_contains_metaobject)
{
    /* method-specializers now stores eql-specializer structs, not (eql V) cons. */
    eval_print("(defgeneric es-ms (x))");
    eval_print("(defmethod es-ms ((x (eql 7))) :seven)");
    ASSERT_STR_EQ(eval_print(
        "(let* ((gf (gethash 'es-ms *generic-function-table*)) "
        "       (m  (first (gf-methods gf))) "
        "       (s  (first (method-specializers m)))) "
        "  (eql-specializer-p s))"),
        "T");
    ASSERT_STR_EQ(eval_print(
        "(let* ((gf (gethash 'es-ms *generic-function-table*)) "
        "       (m  (first (gf-methods gf))) "
        "       (s  (first (method-specializers m)))) "
        "  (eql-specializer-object s))"),
        "7");
}

TEST(eqlspec_method_specializers_shared_across_methods)
{
    /* Two methods with the same EQL value share the same specializer
       metaobject (interned). */
    eval_print("(defgeneric es-share1 (x))");
    eval_print("(defgeneric es-share2 (x))");
    eval_print("(defmethod es-share1 ((x (eql :shared))) :one)");
    eval_print("(defmethod es-share2 ((x (eql :shared))) :two)");
    ASSERT_STR_EQ(eval_print(
        "(eq (first (method-specializers "
        "      (first (gf-methods (gethash 'es-share1 *generic-function-table*))))) "
        "    (first (method-specializers "
        "      (first (gf-methods (gethash 'es-share2 *generic-function-table*))))))"),
        "T");
}

TEST(eqlspec_extract_specializer_names_roundtrips)
{
    /* extract-specializer-names pulls specializer names from a
       specialized lambda list (AMOP). */
    ASSERT_STR_EQ(eval_print(
        "(extract-specializer-names '((x (eql 99)) (y string)))"),
        "((EQL 99) STRING)");
}

TEST(eqlspec_extract_class_specializers)
{
    /* extract-specializer-names from a specialized lambda list —
       class specializers returned as their class names. */
    ASSERT_STR_EQ(eval_print(
        "(extract-specializer-names '((x integer)))"),
        "(INTEGER)");
}

TEST(eqlspec_method_equal_replaces_on_redef)
{
    /* Redefining a method with the same EQL value replaces the old one
       (specializer lists compare via interned metaobject identity). */
    eval_print("(defgeneric es-rep (x))");
    eval_print("(defmethod es-rep ((x (eql 1))) :first)");
    eval_print("(defmethod es-rep ((x (eql 1))) :second)");
    ASSERT_STR_EQ(eval_print("(es-rep 1)"), ":SECOND");
    ASSERT_STR_EQ(eval_print(
        "(length (gf-methods (gethash 'es-rep *generic-function-table*)))"),
        "1");
}

TEST(eqlspec_cacheable_p_still_eql)
{
    /* cacheable-p flag remains :EQL after reification. */
    eval_print("(defgeneric es-cache (x))");
    eval_print("(defmethod es-cache ((x (eql 42))) :fortytwo)");
    eval_print("(defmethod es-cache ((x integer)) :int)");
    ASSERT_STR_EQ(eval_print(
        "(gf-cacheable-p (gethash 'es-cache *generic-function-table*))"),
        ":EQL");
}

TEST(calloc_svuc_protocol_sees_class_slots)
{
    /* The slot-value-using-class protocol fires on class-allocated slots
       too — user :around methods receive the effective-slot-definition. */
    eval_print("(defclass cs-svuc () ((tag :allocation :class :initform 'default)))");
    eval_print("(defvar *cs-svuc-log* nil)");
    eval_print(
        "(defmethod slot-value-using-class :around "
        "    ((class t) (inst cs-svuc) slot) "
        "  (push (slot-definition-name slot) *cs-svuc-log*) "
        "  (call-next-method))");
    ASSERT_STR_EQ(eval_print(
        "(slot-value (make-instance 'cs-svuc) 'tag)"),
        "DEFAULT");
    ASSERT_STR_EQ(eval_print("(first *cs-svuc-log*)"), "TAG");
    eval_print(
        "(remove-method #'slot-value-using-class "
        "  (find-method #'slot-value-using-class '(:around) "
        "    (list (find-class 't) (find-class 'cs-svuc) "
        "          (find-class 'standard-effective-slot-definition))))");
}

/* ==================================================================
 * Funcallable standard class (MOP)
 * ================================================================== */

TEST(funcallable_class_bootstrapped)
{
    /* Classes exist and are properly registered. */
    ASSERT_STR_EQ(eval_print(
        "(class-name (find-class 'funcallable-standard-class))"),
        "FUNCALLABLE-STANDARD-CLASS");
    ASSERT_STR_EQ(eval_print(
        "(class-name (find-class 'funcallable-standard-object))"),
        "FUNCALLABLE-STANDARD-OBJECT");
}

TEST(funcallable_gf_is_funcallable_standard_object)
{
    /* Every generic function is a funcallable-standard-object. */
    eval_print("(defgeneric fsc-foo (x))");
    eval_print("(defmethod fsc-foo ((x integer)) (* x 2))");
    ASSERT_STR_EQ(eval_print("(typep #'fsc-foo 'funcallable-standard-object)"), "T");
    ASSERT_STR_EQ(eval_print("(typep #'fsc-foo 'standard-generic-function)"), "T");
    ASSERT_STR_EQ(eval_print("(typep #'fsc-foo 'generic-function)"), "T");
    /* And it's typep 'function via the CPL chain. */
    ASSERT_STR_EQ(eval_print("(typep #'fsc-foo 'function)"), "T");
    ASSERT_STR_EQ(eval_print("(functionp #'fsc-foo)"), "T");
    /* Negative cases: non-generic-functions must not satisfy generic-function. */
    ASSERT_STR_EQ(eval_print("(typep 42 'generic-function)"), "NIL");
    ASSERT_STR_EQ(eval_print("(typep #'car 'generic-function)"), "NIL");
}

TEST(funcallable_class_of_gf)
{
    /* class-of on a GF returns standard-generic-function. */
    eval_print("(defgeneric fsc-co (x))");
    eval_print("(defmethod fsc-co ((x t)) x)");
    ASSERT_STR_EQ(eval_print(
        "(eq (class-of #'fsc-co) (find-class 'standard-generic-function))"),
        "T");
}

TEST(funcallable_gf_still_callable_via_symbol)
{
    /* Regression guard: installing the GF struct in symbol-function did not
       break ordinary (foo args) dispatch. */
    eval_print("(defgeneric fsc-call (x))");
    eval_print("(defmethod fsc-call ((x integer)) (* x 3))");
    eval_print("(defmethod fsc-call ((x string)) (length x))");
    ASSERT_STR_EQ(eval_print("(fsc-call 7)"), "21");
    ASSERT_STR_EQ(eval_print("(fsc-call \"hello\")"), "5");
}

TEST(funcallable_gf_via_funcall_and_apply)
{
    /* FUNCALL and APPLY of the GF metaobject both route through the
       discriminating function (cl_unwrap_funcallable). */
    eval_print("(defgeneric fsc-fa (x))");
    eval_print("(defmethod fsc-fa ((x integer)) (+ x 100))");
    ASSERT_STR_EQ(eval_print("(funcall #'fsc-fa 5)"), "105");
    ASSERT_STR_EQ(eval_print("(apply #'fsc-fa '(10))"), "110");
    /* Also works if you capture the GF struct separately. */
    ASSERT_STR_EQ(eval_print(
        "(funcall (ensure-generic-function 'fsc-fa) 3)"), "103");
}

TEST(funcallable_set_funcallable_instance_function)
{
    /* User can override dispatch completely via set-funcallable-instance-function. */
    eval_print("(defgeneric fsc-sfi (x))");
    eval_print("(defmethod fsc-sfi ((x integer)) :default)");
    ASSERT_STR_EQ(eval_print("(fsc-sfi 1)"), ":DEFAULT");
    eval_print(
        "(set-funcallable-instance-function "
        "  (ensure-generic-function 'fsc-sfi) "
        "  (lambda (&rest args) (cons :replaced args)))");
    ASSERT_STR_EQ(eval_print("(fsc-sfi 1)"), "(:REPLACED 1)");
    ASSERT_STR_EQ(eval_print("(funcall #'fsc-sfi 2 3)"), "(:REPLACED 2 3)");
}

TEST(funcallable_sfi_returns_gf)
{
    /* set-funcallable-instance-function returns the GF, per AMOP. */
    eval_print("(defgeneric fsc-sfir (x))");
    eval_print("(defmethod fsc-sfir ((x t)) nil)");
    ASSERT_STR_EQ(eval_print(
        "(eq (set-funcallable-instance-function "
        "      (ensure-generic-function 'fsc-sfir) "
        "      (lambda (&rest args) args)) "
        "    (ensure-generic-function 'fsc-sfir))"),
        "T");
}

TEST(funcallable_standard_instance_access)
{
    /* Raw slot access by integer location — bypasses GF dispatch. */
    eval_print("(defclass sia-p () ((a :initarg :a) (b :initarg :b)))");
    eval_print("(defvar *sia-inst* (make-instance 'sia-p :a 10 :b 20))");
    /* Layout: slot 0 = A, slot 1 = B. */
    ASSERT_STR_EQ(eval_print("(standard-instance-access *sia-inst* 0)"), "10");
    ASSERT_STR_EQ(eval_print("(standard-instance-access *sia-inst* 1)"), "20");
    /* SETF works. */
    eval_print("(setf (standard-instance-access *sia-inst* 0) 99)");
    ASSERT_STR_EQ(eval_print("(slot-value *sia-inst* 'a)"), "99");
}

TEST(funcallable_standard_instance_access_equals_struct_ref)
{
    /* Spec: (standard-instance-access inst idx) equals (%struct-ref inst idx). */
    eval_print("(defclass siaq () ((q :initarg :q)))");
    eval_print("(defvar *siaq-inst* (make-instance 'siaq :q 'hello))");
    ASSERT_STR_EQ(eval_print(
        "(eq (standard-instance-access *siaq-inst* 0) "
        "    (%struct-ref *siaq-inst* 0))"),
        "T");
}

TEST(funcallable_standard_instance_access_variant)
{
    /* funcallable-standard-instance-access is the funcallable twin. */
    eval_print("(defclass fsia () ((r :initarg :r)))");
    eval_print("(defvar *fsia-inst* (make-instance 'fsia :r 77))");
    ASSERT_STR_EQ(eval_print("(funcallable-standard-instance-access *fsia-inst* 0)"), "77");
    eval_print("(setf (funcallable-standard-instance-access *fsia-inst* 0) 88)");
    ASSERT_STR_EQ(eval_print("(slot-value *fsia-inst* 'r)"), "88");
}

TEST(funcallable_compute_discriminating_function_default)
{
    /* Default method returns the current discriminating function; it must
       be something callable. */
    eval_print("(defgeneric fsc-cdf (x))");
    eval_print("(defmethod fsc-cdf ((x integer)) :int-case)");
    ASSERT_STR_EQ(eval_print(
        "(functionp (compute-discriminating-function "
        "             (ensure-generic-function 'fsc-cdf)))"),
        "T");
    /* Calling the returned function does the same dispatch as the GF. */
    ASSERT_STR_EQ(eval_print(
        "(funcall (compute-discriminating-function "
        "           (ensure-generic-function 'fsc-cdf)) 5)"),
        ":INT-CASE");
}

TEST(funcallable_gf_type_of)
{
    eval_print("(defgeneric fsc-to (x))");
    eval_print("(defmethod fsc-to ((x t)) x)");
    ASSERT_STR_EQ(eval_print("(type-of #'fsc-to)"), "STANDARD-GENERIC-FUNCTION");
}

TEST(funcallable_gf_cpl_has_function)
{
    /* The CPL of standard-generic-function includes funcallable-standard-object
       and function above standard-object — callers checking (typep ... 'function)
       rely on that relationship. */
    ASSERT_STR_EQ(eval_print(
        "(let ((cpl-names (mapcar #'class-name "
        "                   (class-precedence-list "
        "                     (find-class 'standard-generic-function))))) "
        "  (list (find 'funcallable-standard-object cpl-names) "
        "        (find 'function cpl-names) "
        "        (find 'standard-object cpl-names) "
        "        (find 't cpl-names)))"),
        "(FUNCALLABLE-STANDARD-OBJECT FUNCTION STANDARD-OBJECT T)");
}

/* --- Custom generic-function metaclass (CLHS :generic-function-class) --- */

TEST(gfclass_custom_type_of)
{
    /* defgeneric (:generic-function-class C) tags the GF struct with C, so
       TYPE-OF and CLASS-OF report the custom class, not the base GF. */
    eval_print("(defclass rgf-a (cl:standard-generic-function) ()"
               "  (:metaclass funcallable-standard-class))");
    eval_print("(defgeneric rga (x) (:generic-function-class rgf-a)"
               "  (:method ((x integer)) (* x 2)))");
    ASSERT_STR_EQ(eval_print("(type-of #'rga)"), "RGF-A");
    ASSERT_STR_EQ(eval_print("(class-name (class-of #'rga))"), "RGF-A");
}

TEST(gfclass_custom_is_funcallable)
{
    /* A custom-class GF must still be FUNCTIONP and callable/funcallable,
       and TYPEP as both its own class and standard-generic-function. */
    eval_print("(defclass rgf-b (cl:standard-generic-function) ()"
               "  (:metaclass funcallable-standard-class))");
    eval_print("(defgeneric rgb (x) (:generic-function-class rgf-b)"
               "  (:method ((x integer)) (+ x 100)))");
    ASSERT_STR_EQ(eval_print("(functionp #'rgb)"), "T");
    ASSERT_STR_EQ(eval_print("(typep #'rgb 'rgf-b)"), "T");
    ASSERT_STR_EQ(eval_print("(typep #'rgb 'standard-generic-function)"), "T");
    ASSERT_STR_EQ(eval_print("(typep #'rgb 'function)"), "T");
    ASSERT_STR_EQ(eval_print("(rgb 5)"), "105");
    ASSERT_STR_EQ(eval_print("(funcall #'rgb 7)"), "107");
    ASSERT_STR_EQ(eval_print("(apply #'rgb '(8))"), "108");
}

TEST(gfclass_initialize_instance_after_fires)
{
    /* The make-instance protocol must run for a custom-class GF so user
       INITIALIZE-INSTANCE :AFTER methods fire (this is exactly how snooze's
       defroute self-registers each route). */
    eval_print("(defclass rgf-c (cl:standard-generic-function) ()"
               "  (:metaclass funcallable-standard-class))");
    eval_print("(defvar *rgf-registry* (make-hash-table))");
    eval_print("(defmethod initialize-instance :after"
               "    ((gf rgf-c) &rest args) (declare (ignore args))"
               "  (setf (gethash (generic-function-name gf) *rgf-registry*) gf))");
    eval_print("(defgeneric rgc (x) (:generic-function-class rgf-c)"
               "  (:method ((x t)) x))");
    /* The :after stored the GF under its name. */
    ASSERT_STR_EQ(eval_print("(eq (gethash 'rgc *rgf-registry*) #'rgc)"), "T");
}

TEST(gfclass_dispatch_on_gf_argument)
{
    /* A custom GF must dispatch as the specialized argument of *another* GF
       (snooze's api.lisp methods specialize on resource-generic-function). */
    eval_print("(defclass rgf-d (cl:standard-generic-function) ()"
               "  (:metaclass funcallable-standard-class))");
    eval_print("(defgeneric rgd (x) (:generic-function-class rgf-d)"
               "  (:method ((x t)) x))");
    eval_print("(defgeneric classify-gf (g))");
    eval_print("(defmethod classify-gf ((g rgf-d)) :custom)");
    eval_print("(defmethod classify-gf ((g standard-generic-function)) :standard)");
    eval_print("(defgeneric plain-gf (x))");
    ASSERT_STR_EQ(eval_print("(classify-gf #'rgd)"), ":CUSTOM");
    ASSERT_STR_EQ(eval_print("(classify-gf #'plain-gf)"), ":STANDARD");
}

TEST(gfclass_default_unchanged)
{
    /* An explicit :generic-function-class standard-generic-function (and the
       common no-option case) must keep the base behavior. */
    eval_print("(defgeneric gfd-explicit (x)"
               "  (:generic-function-class standard-generic-function)"
               "  (:method ((x t)) x))");
    ASSERT_STR_EQ(eval_print("(type-of #'gfd-explicit)"),
                  "STANDARD-GENERIC-FUNCTION");
    ASSERT_STR_EQ(eval_print("(gfd-explicit 42)"), "42");
}

/* ============================================================
 * Method metaobject protocol (MOP)
 * ============================================================ */

TEST(mmop_method_generic_function_backlink)
{
    /* DEFMETHOD stores the installing GF on the method so metaobject
       introspection can walk from method back to its GF. */
    eval_print("(defgeneric mmop-bl (x))");
    eval_print("(defmethod mmop-bl ((x integer)) :int)");
    ASSERT_STR_EQ(eval_print(
        "(let* ((gf (gethash 'mmop-bl *generic-function-table*)) "
        "       (m  (first (gf-methods gf)))) "
        "  (eq (method-generic-function m) gf))"),
        "T");
}

TEST(mmop_extract_lambda_list_plain)
{
    /* EXTRACT-LAMBDA-LIST strips specializers from required params. */
    ASSERT_STR_EQ(eval_print(
        "(extract-lambda-list '((x point) (y (eql 3)) z))"),
        "(X Y Z)");
}

TEST(mmop_extract_lambda_list_preserves_keywords)
{
    /* Non-required parameters pass through untouched. */
    ASSERT_STR_EQ(eval_print(
        "(extract-lambda-list '((x integer) &optional (y 5) &rest rest &key k))"),
        "(X &OPTIONAL (Y 5) &REST REST &KEY K)");
}

TEST(mmop_extract_specializer_names_padded_t)
{
    /* Unspecialized required params get T. */
    ASSERT_STR_EQ(eval_print(
        "(extract-specializer-names '(x (y integer) z))"),
        "(T INTEGER T)");
}

TEST(mmop_extract_specializer_names_stops_at_keywords)
{
    /* Specializer harvest ignores non-required tail. */
    ASSERT_STR_EQ(eval_print(
        "(extract-specializer-names '((a string) &optional b &key c))"),
        "(STRING)");
}

TEST(mmop_add_method_installs_new_method)
{
    /* Calling the ADD-METHOD GF with a programmatically-built method
       makes it dispatchable — same end state as DEFMETHOD. */
    eval_print("(defgeneric mmop-add (x))");
    eval_print("(defmethod mmop-add ((x t)) :default)");
    eval_print(
        "(let* ((gf (ensure-generic-function 'mmop-add)) "
        "       (fn (lambda (x) (declare (ignore x)) :added)) "
        "       (m (%make-struct 'standard-method "
        "            nil (list (find-class 'integer)) '() fn '(x)))) "
        "  (add-method gf m))");
    ASSERT_STR_EQ(eval_print("(mmop-add 42)"), ":ADDED");
    ASSERT_STR_EQ(eval_print("(mmop-add \"hi\")"), ":DEFAULT");
}

TEST(mmop_add_method_sets_back_link)
{
    /* ADD-METHOD fills in the method's generic-function slot so the
       metaobject knows where it lives. */
    eval_print("(defgeneric mmop-addbl (x))");
    eval_print(
        "(let* ((gf (ensure-generic-function 'mmop-addbl)) "
        "       (fn (lambda (x) (declare (ignore x)) :ok)) "
        "       (m (%make-struct 'standard-method "
        "            nil (list (find-class 't)) '() fn '(x)))) "
        "  (add-method gf m) "
        "  (setq *mmop-m* m) "
        "  (setq *mmop-gf* gf))");
    ASSERT_STR_EQ(eval_print("(eq (method-generic-function *mmop-m*) *mmop-gf*)"), "T");
}

TEST(mmop_add_method_returns_gf)
{
    /* AMOP: ADD-METHOD returns the GF. */
    eval_print("(defgeneric mmop-ret (x))");
    ASSERT_STR_EQ(eval_print(
        "(let* ((gf (ensure-generic-function 'mmop-ret)) "
        "       (m (%make-struct 'standard-method "
        "            nil (list (find-class 't)) '() "
        "            (lambda (x) (declare (ignore x)) :ok) '(x)))) "
        "  (eq (add-method gf m) gf))"),
        "T");
}

TEST(mmop_remove_method_drops_dispatch)
{
    /* REMOVE-METHOD eliminates the method — subsequent calls no longer
       see it. */
    eval_print("(defgeneric mmop-rm (x))");
    eval_print("(defmethod mmop-rm ((x t)) :default)");
    eval_print("(defmethod mmop-rm ((x integer)) :int)");
    ASSERT_STR_EQ(eval_print("(mmop-rm 42)"), ":INT");
    eval_print(
        "(let* ((gf (gethash 'mmop-rm *generic-function-table*)) "
        "       (m (find-if (lambda (m) "
        "                     (equal (method-specializers m) "
        "                            (list (find-class 'integer)))) "
        "                   (gf-methods gf)))) "
        "  (remove-method gf m))");
    ASSERT_STR_EQ(eval_print("(mmop-rm 42)"), ":DEFAULT");
}

TEST(mmop_remove_method_clears_backlink)
{
    /* REMOVE-METHOD severs method->GF back-link. */
    eval_print("(defgeneric mmop-rmbl (x))");
    eval_print("(defmethod mmop-rmbl ((x integer)) :int)");
    eval_print(
        "(let* ((gf (gethash 'mmop-rmbl *generic-function-table*)) "
        "       (m (first (gf-methods gf)))) "
        "  (setq *mmop-rm-m* m) "
        "  (remove-method gf m))");
    ASSERT_STR_EQ(eval_print("(method-generic-function *mmop-rm-m*)"), "NIL");
}

TEST(mmop_find_method_returns_existing)
{
    /* FIND-METHOD locates the method by qualifiers + specializers. */
    eval_print("(defgeneric mmop-fm (x))");
    eval_print("(defmethod mmop-fm ((x string)) :s)");
    eval_print("(defmethod mmop-fm :before ((x string)) :before)");
    ASSERT_STR_EQ(eval_print(
        "(let ((gf (gethash 'mmop-fm *generic-function-table*))) "
        "  (eq (find-method gf '() (list (find-class 'string))) "
        "      (find-if (lambda (m) (null (method-qualifiers m))) "
        "               (gf-methods gf))))"),
        "T");
}

TEST(mmop_find_method_accepts_class_names)
{
    /* FIND-METHOD resolves specializer class *names* to class
       metaobjects before comparing. */
    eval_print("(defgeneric mmop-fmn (x))");
    eval_print("(defmethod mmop-fmn ((x integer)) :int)");
    ASSERT_STR_EQ(eval_print(
        "(let ((gf (gethash 'mmop-fmn *generic-function-table*))) "
        "  (not (null (find-method gf '() '(integer)))))"),
        "T");
}

TEST(mmop_find_method_eql_names)
{
    /* FIND-METHOD resolves (EQL val) names to EQL specializer metaobjects. */
    eval_print("(defgeneric mmop-fmeql (x))");
    eval_print("(defmethod mmop-fmeql ((x (eql 7))) :seven)");
    ASSERT_STR_EQ(eval_print(
        "(let ((gf (gethash 'mmop-fmeql *generic-function-table*))) "
        "  (not (null (find-method gf '() '((eql 7))))))"),
        "T");
}

TEST(mmop_find_method_missing_errors)
{
    /* FIND-METHOD signals when no match and ERRORP is T (the default). */
    eval_print("(defgeneric mmop-fmmiss (x))");
    ASSERT_STR_EQ(eval_print(
        "(handler-case "
        "  (find-method (gethash 'mmop-fmmiss *generic-function-table*) "
        "               '() '(integer)) "
        "  (error () :signaled))"),
        ":SIGNALED");
}

TEST(mmop_find_method_missing_returns_nil)
{
    /* With ERRORP NIL, FIND-METHOD returns NIL. */
    eval_print("(defgeneric mmop-fmnil (x))");
    ASSERT_STR_EQ(eval_print(
        "(find-method (gethash 'mmop-fmnil *generic-function-table*) "
        "             '() '(integer) nil)"),
        "NIL");
}

TEST(mmop_ensure_method_installs)
{
    /* ENSURE-METHOD constructs and installs a method from a lambda. */
    eval_print("(defgeneric mmop-em (x))");
    eval_print(
        "(ensure-method 'mmop-em '(lambda (x) (declare (ignore x)) :added) "
        "               :specializers '(integer))");
    ASSERT_STR_EQ(eval_print("(mmop-em 99)"), ":ADDED");
}

TEST(mmop_ensure_method_default_specializers_are_t)
{
    /* Without :specializers, ENSURE-METHOD pads with T specializers,
       giving a catch-all default method. */
    eval_print("(defgeneric mmop-emdef (x))");
    eval_print(
        "(ensure-method 'mmop-emdef '(lambda (x) (declare (ignore x)) :any))");
    ASSERT_STR_EQ(eval_print("(mmop-emdef \"any\")"), ":ANY");
    ASSERT_STR_EQ(eval_print("(mmop-emdef 42)"), ":ANY");
}

TEST(mmop_ensure_method_returns_method)
{
    /* ENSURE-METHOD returns the newly constructed method metaobject. */
    eval_print("(defgeneric mmop-emret (x))");
    ASSERT_STR_EQ(eval_print(
        "(let ((m (ensure-method 'mmop-emret "
        "            '(lambda (x) (declare (ignore x)) :ok) "
        "            :specializers '(integer)))) "
        "  (eq (%struct-type-name m) 'standard-method))"),
        "T");
}

TEST(mmop_make_method_lambda_default_identity)
{
    /* MAKE-METHOD-LAMBDA returns (values lambda-expr nil) by default —
       a no-op hook point that metaclasses can specialise. */
    eval_print("(defgeneric mmop-mml-host (x))");
    ASSERT_STR_EQ(eval_print(
        "(let* ((gf (ensure-generic-function 'mmop-mml-host)) "
        "       (m (%make-struct 'standard-method "
        "            gf (list (find-class 't)) '() "
        "            (lambda (x) x) '(x)))) "
        "  (multiple-value-list "
        "    (make-method-lambda gf m '(lambda (x) x) nil)))"),
        "((LAMBDA (X) X) NIL)");
}

/* ============================================================
 * Method combination (MOP) — built-in short forms, user-defined
 * combinations (short + long), find-method-combination, and
 * :method-combination DEFGENERIC option.
 * ============================================================ */

TEST(mc_standard_default)
{
    /* Every GF carries a method-combination metaobject.  When DEFGENERIC
       omits :METHOD-COMBINATION, STANDARD is installed by default.
       The combination is registered with the CL symbol (not exported),
       so we compare on SYMBOL-NAME to stay package-agnostic. */
    eval_print("(defgeneric mc-std-default (x))");
    eval_print("(defmethod mc-std-default ((x t)) :ok)");
    ASSERT_STR_EQ(eval_print(
        "(symbol-name "
        "  (method-combination-name "
        "    (gf-method-combination "
        "      (gethash 'mc-std-default *generic-function-table*))))"),
        "\"STANDARD\"");
}

TEST(mc_plus_basic)
{
    /* The + built-in sums primary-method return values. */
    eval_print("(defgeneric mc-plus (x) (:method-combination +))");
    eval_print("(defmethod mc-plus + ((x t)) 1)");
    eval_print("(defmethod mc-plus + ((x integer)) 10)");
    eval_print("(defmethod mc-plus + ((x number)) 100)");
    ASSERT_STR_EQ(eval_print("(mc-plus 42)"), "111");
}

TEST(mc_plus_identity_with_one_argument)
{
    /* With a single primary method + uses its value directly (the
       :IDENTITY-WITH-ONE-ARGUMENT fast path). */
    eval_print("(defgeneric mc-plus-id (x) (:method-combination +))");
    eval_print("(defmethod mc-plus-id + ((x integer)) 7)");
    ASSERT_STR_EQ(eval_print("(mc-plus-id 1)"), "7");
}

TEST(mc_list_collects)
{
    /* LIST collects return values in most-specific-first order. */
    eval_print("(defgeneric mc-list (x) (:method-combination list))");
    eval_print("(defmethod mc-list list ((x t)) :any)");
    eval_print("(defmethod mc-list list ((x number)) :num)");
    eval_print("(defmethod mc-list list ((x integer)) :int)");
    ASSERT_STR_EQ(eval_print("(mc-list 42)"), "(:INT :NUM :ANY)");
}

TEST(mc_and_short_circuits)
{
    /* AND short-circuits: once a primary returns NIL, remaining methods
       are not invoked. */
    eval_print("(defvar *mc-and-count* 0)");
    eval_print("(defgeneric mc-and (x) (:method-combination and))");
    eval_print("(defmethod mc-and and ((x t)) "
               "  (incf *mc-and-count*) t)");
    eval_print("(defmethod mc-and and ((x integer)) "
               "  (incf *mc-and-count*) nil)");
    eval_print("(defmethod mc-and and ((x number)) "
               "  (incf *mc-and-count*) "
               "  (error \"should not be called\"))");
    ASSERT_STR_EQ(eval_print("(mc-and 42)"), "NIL");
    ASSERT_STR_EQ(eval_print("*mc-and-count*"), "1");
}

TEST(mc_or_returns_first_true)
{
    /* OR returns the first non-NIL value among primaries. */
    eval_print("(defgeneric mc-or (x) (:method-combination or))");
    eval_print("(defmethod mc-or or ((x t)) nil)");
    eval_print("(defmethod mc-or or ((x integer)) :found)");
    ASSERT_STR_EQ(eval_print("(mc-or 42)"), ":FOUND");
}

TEST(mc_progn_returns_last)
{
    /* PROGN returns the last primary-method value.  With
       :IDENTITY-WITH-ONE-ARGUMENT on a single primary it returns that
       method's value unchanged. */
    eval_print("(defgeneric mc-progn (x) (:method-combination progn))");
    eval_print("(defmethod mc-progn progn ((x t)) :a)");
    eval_print("(defmethod mc-progn progn ((x integer)) :b)");
    /* Most-specific-first: INTEGER then T -> last evaluated is T's :A */
    ASSERT_STR_EQ(eval_print("(mc-progn 42)"), ":A");
}

TEST(mc_append_concatenates)
{
    /* APPEND splices primary return lists together. */
    eval_print("(defgeneric mc-app (x) (:method-combination append))");
    eval_print("(defmethod mc-app append ((x t)) '(:t))");
    eval_print("(defmethod mc-app append ((x integer)) '(:int))");
    ASSERT_STR_EQ(eval_print("(mc-app 42)"), "(:INT :T)");
}

TEST(mc_max_reduces)
{
    /* MAX returns the largest primary return value. */
    eval_print("(defgeneric mc-max (x) (:method-combination max))");
    eval_print("(defmethod mc-max max ((x t)) 1)");
    eval_print("(defmethod mc-max max ((x integer)) 5)");
    eval_print("(defmethod mc-max max ((x number)) 3)");
    ASSERT_STR_EQ(eval_print("(mc-max 42)"), "5");
}

TEST(mc_around_wraps_short_form)
{
    /* :AROUND methods are honoured on short-form combinations.
       The :AROUND method wraps the primary combination (here +). */
    eval_print("(defgeneric mc-ar (x) (:method-combination +))");
    eval_print("(defmethod mc-ar + ((x t)) 1)");
    eval_print("(defmethod mc-ar + ((x integer)) 2)");
    eval_print("(defmethod mc-ar :around ((x t)) "
               "  (+ 1000 (call-next-method)))");
    ASSERT_STR_EQ(eval_print("(mc-ar 42)"), "1003");
}

TEST(mc_invalid_qualifier_errors)
{
    /* A short-form combination rejects methods whose qualifier is not
       the combination name (nor :AROUND). */
    eval_print("(defgeneric mc-invalid (x) (:method-combination +))");
    eval_print("(defmethod mc-invalid + ((x t)) 1)");
    eval_print("(defmethod mc-invalid :before ((x t)) :bad)");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (mc-invalid 1) (error () :rejected))"),
        ":REJECTED");
}

TEST(mc_standard_still_supports_before_after)
{
    /* Sanity: switching other GFs to non-standard combinations does not
       affect standard GFs — :BEFORE / :AFTER still work. */
    eval_print("(defvar *mc-std-log* nil)");
    eval_print("(defgeneric mc-std (x))");
    eval_print("(defmethod mc-std ((x t)) "
               "  (push :primary *mc-std-log*) :ok)");
    eval_print("(defmethod mc-std :before ((x t)) "
               "  (push :before *mc-std-log*))");
    eval_print("(defmethod mc-std :after ((x t)) "
               "  (push :after *mc-std-log*))");
    ASSERT_STR_EQ(eval_print("(mc-std 1)"), ":OK");
    ASSERT_STR_EQ(eval_print("(reverse *mc-std-log*)"),
                  "(:BEFORE :PRIMARY :AFTER)");
}

TEST(mc_find_method_combination_standard)
{
    /* FIND-METHOD-COMBINATION with NIL as GF designator returns the
       named combination metaobject from the registry.  Symbol-name
       keying means this works regardless of which package spells
       'STANDARD. */
    ASSERT_STR_EQ(eval_print(
        "(symbol-name "
        "  (method-combination-name "
        "    (find-method-combination nil 'standard nil)))"),
        "\"STANDARD\"");
}

TEST(mc_find_method_combination_type)
{
    /* Built-in short-form combos report type :SHORT. */
    ASSERT_STR_EQ(eval_print(
        "(method-combination-type "
        "  (find-method-combination nil '+ nil))"),
        ":SHORT");
}

TEST(mc_find_method_combination_unknown_errors)
{
    /* Unknown combination names are signalled. */
    ASSERT_STR_EQ(eval_print(
        "(handler-case "
        "  (find-method-combination nil 'no-such-combination nil) "
        "  (error () :signaled))"),
        ":SIGNALED");
}

TEST(mc_define_short_form_basic)
{
    /* DEFINE-METHOD-COMBINATION short form registers a combination that
       wraps primary return values with :OPERATOR. */
    eval_print("(define-method-combination mc-user-short "
               "  :operator * :identity-with-one-argument t)");
    eval_print("(defgeneric mc-us (x) (:method-combination mc-user-short))");
    eval_print("(defmethod mc-us mc-user-short ((x t)) 2)");
    eval_print("(defmethod mc-us mc-user-short ((x integer)) 3)");
    ASSERT_STR_EQ(eval_print("(mc-us 42)"), "6");
}

TEST(mc_define_short_form_identity_default)
{
    /* Without :IDENTITY-WITH-ONE-ARGUMENT, a single primary method is
       still wrapped through the operator (single-arg call). */
    eval_print("(define-method-combination mc-short-no-id "
               "  :operator list)");
    eval_print("(defgeneric mc-sni (x) "
               "  (:method-combination mc-short-no-id))");
    eval_print("(defmethod mc-sni mc-short-no-id ((x integer)) :v)");
    /* Single value wrapped: (list :v) -> (:V) */
    ASSERT_STR_EQ(eval_print("(mc-sni 1)"), "(:V)");
}

TEST(mc_define_short_form_operator_defaults_to_name)
{
    /* Without :OPERATOR the combination name is used as the operator. */
    eval_print("(define-method-combination mc-name-op)");
    eval_print("(defgeneric mc-no (x) (:method-combination mc-name-op))");
    eval_print("(defmethod mc-no mc-name-op ((x t)) 1)");
    ASSERT_STR_EQ(eval_print(
        "(method-combination-name "
        "  (gf-method-combination "
        "    (gethash 'mc-no *generic-function-table*)))"),
        "MC-NAME-OP");
}

TEST(mc_define_long_form_basic)
{
    /* Long form: (define-method-combination NAME () (GROUPS) BODY) —
       BODY returns a form that uses CALL-METHOD. */
    eval_print(
        "(define-method-combination mc-first ()"
        "  ((primary ()))"
        "  `(call-method ,(first primary)))");
    eval_print("(defgeneric mc-f1 (x) (:method-combination mc-first))");
    eval_print("(defmethod mc-f1 ((x t)) :t-method)");
    eval_print("(defmethod mc-f1 ((x integer)) :int-method)");
    /* Most-specific-first: (first primary) is the INTEGER method. */
    ASSERT_STR_EQ(eval_print("(mc-f1 42)"), ":INT-METHOD");
}

TEST(mc_define_long_form_multiple_groups)
{
    /* Long form with two groups: :AROUND and primary.  Body passes
       PRIMARY as :AROUND's next-methods so CALL-NEXT-METHOD inside the
       :AROUND body walks into the primary chain. */
    eval_print(
        "(define-method-combination mc-ar-aware ()"
        "  ((around (:around))"
        "   (primary ()))"
        "  (if around"
        "      `(call-method ,(first around) ,primary)"
        "      `(call-method ,(first primary))))");
    eval_print("(defgeneric mc-ara (x) (:method-combination mc-ar-aware))");
    eval_print("(defmethod mc-ara ((x t)) :primary)");
    eval_print("(defmethod mc-ara :around ((x t)) "
               "  (cons :around (call-next-method)))");
    ASSERT_STR_EQ(eval_print("(mc-ara 1)"), "(:AROUND . :PRIMARY)");
}

TEST(mc_define_long_form_qualifier_pattern)
{
    /* Long-form pattern with a specific qualifier selects methods by
       exact qualifier match. */
    eval_print(
        "(define-method-combination mc-tagged ()"
        "  ((tagged (tag)))"
        "  `(call-method ,(first tagged)))");
    eval_print("(defgeneric mc-tg (x) (:method-combination mc-tagged))");
    eval_print("(defmethod mc-tg tag ((x t)) :tagged)");
    ASSERT_STR_EQ(eval_print("(mc-tg 1)"), ":TAGGED");
}

TEST(mc_long_form_make_method)
{
    /* (MAKE-METHOD FORM) in a CALL-METHOD next-methods list builds an
       anonymous method whose body evaluates FORM.  serapeum's
       standard/context combination relies on this to wrap an around-form
       as the next method of a :context method. */
    eval_print(
        "(define-method-combination mc-mk ()"
        "  ((primary ()))"
        "  `(call-method ,(first primary) ((make-method 99))))");
    eval_print("(defgeneric mc-mkg (x) (:method-combination mc-mk))");
    eval_print("(defmethod mc-mkg ((x t)) (call-next-method))");
    /* The primary's CALL-NEXT-METHOD lands in the MAKE-METHOD body -> 99. */
    ASSERT_STR_EQ(eval_print("(mc-mkg 1)"), "99");
}

/* Regression: (make-method (call-next-method)) as the last in a next-methods
   list must signal "No next method" rather than infinitely recursing.
   Bug: simple-primary-p=T on anon methods triggered the leaf optimization,
   which returned the anon method's own function as *call-next-method-function*
   — so (call-next-method) inside the body re-entered itself. */
TEST(mc_make_method_cnm_signals_no_next_method)
{
    eval_print(
        "(define-method-combination mc-mmcnm ()"
        "  ((primary ()))"
        "  `(call-method ,(first primary)"
        "      ((make-method (call-next-method)))))");
    eval_print("(defgeneric mc-mmcnmg (x) (:method-combination mc-mmcnm))");
    eval_print("(defmethod mc-mmcnmg ((x t)) (call-next-method))");
    /* primary -> (call-next-method) -> anon method body -> (call-next-method)
       with no further next method -> must error, not loop */
    ASSERT_STR_EQ(
        eval_print("(handler-case (mc-mmcnmg 1) (error () :no-next))"),
        ":NO-NEXT");
}

TEST(mc_long_form_order_most_specific_last)
{
    /* The :ORDER :MOST-SPECIFIC-LAST group option reverses the matched
       methods, which otherwise arrive most-specific-first. */
    eval_print("(defclass msl-a () ())");
    eval_print("(defclass msl-b (msl-a) ())");
    eval_print(
        "(define-method-combination mc-msl ()"
        "  ((all () :order :most-specific-last))"
        "  `(list ,@(mapcar (lambda (m) `(call-method ,m ())) all)))");
    eval_print("(defgeneric mc-mslg (x) (:method-combination mc-msl))");
    eval_print("(defmethod mc-mslg ((x msl-a)) 'a)");
    eval_print("(defmethod mc-mslg ((x msl-b)) 'b)");
    /* most-specific-last => least-specific (A) first. */
    ASSERT_STR_EQ(eval_print("(mc-mslg (make-instance 'msl-b))"), "(A B)");
}

TEST(mc_long_form_standard_context)
{
    /* End-to-end reproduction of serapeum's STANDARD/CONTEXT combination:
       :context methods (most-specific-last) wrap :around methods, which
       wrap the primary, all via MAKE-METHOD + CALL-METHOD. */
    eval_print(
        "(define-method-combination sc ()"
        "  ((around (:around))"
        "   (context (:context) :order :most-specific-last)"
        "   (primary ()))"
        "  (let* ((form `(call-method ,(first primary)))"
        "         (around-form (if around"
        "                          `(call-method ,(first around)"
        "                                        (,@(rest around) (make-method ,form)))"
        "                          form)))"
        "    (if context"
        "        `(call-method ,(first context)"
        "                      (,@(rest context) (make-method ,around-form)))"
        "        around-form)))");
    eval_print("(defclass sc-a () ())");
    eval_print("(defclass sc-b (sc-a) ())");
    eval_print("(defgeneric scg (x) (:method-combination sc))");
    eval_print("(defmethod scg ((x sc-a)) '(prim))");
    eval_print("(defmethod scg :around ((x sc-a)) (cons 'a/ar (call-next-method)))");
    eval_print("(defmethod scg :around ((x sc-b)) (cons 'b/ar (call-next-method)))");
    eval_print("(defmethod scg :context ((x sc-a)) (cons 'a/cx (call-next-method)))");
    eval_print("(defmethod scg :context ((x sc-b)) (cons 'b/cx (call-next-method)))");
    /* context least-specific-first (A/CX B/CX), around most-specific-first
       (B/AR A/AR), then the primary. */
    ASSERT_STR_EQ(eval_print("(scg (make-instance 'sc-b))"),
                  "(A/CX B/CX B/AR A/AR PRIM)");
}

TEST(mc_method_combination_object_type)
{
    /* Registered combinations are STANDARD-METHOD-COMBINATION structs. */
    ASSERT_STR_EQ(eval_print(
        "(%struct-type-name (find-method-combination nil '+ nil))"),
        "STANDARD-METHOD-COMBINATION");
}

TEST(mc_method_combination_class_bootstrapped)
{
    /* STANDARD-METHOD-COMBINATION is a first-class class in the
       metaobject hierarchy. */
    ASSERT_STR_EQ(eval_print(
        "(class-name (find-class 'standard-method-combination))"),
        "STANDARD-METHOD-COMBINATION");
}

TEST(mc_call_method_uses_current_args)
{
    /* CALL-METHOD invokes a method's function on *CURRENT-METHOD-ARGS*,
       so long-form bodies do not need to thread arguments explicitly. */
    eval_print(
        "(define-method-combination mc-args ()"
        "  ((primary ()))"
        "  `(call-method ,(first primary)))");
    eval_print("(defgeneric mc-pass (x y) (:method-combination mc-args))");
    eval_print("(defmethod mc-pass ((x integer) (y integer)) (+ x y))");
    ASSERT_STR_EQ(eval_print("(mc-pass 3 4)"), "7");
}

TEST(mc_redefine_updates_combination)
{
    /* Re-DEFGENERICing a GF with a different :METHOD-COMBINATION swaps
       the combination metaobject on the existing GF. */
    eval_print("(defgeneric mc-swap (x))");
    eval_print("(defgeneric mc-swap (x) (:method-combination +))");
    ASSERT_STR_EQ(eval_print(
        "(method-combination-name "
        "  (gf-method-combination "
        "    (gethash 'mc-swap *generic-function-table*)))"),
        "+");
}

/* ==================================================================
 * Dependent maintenance protocol (MOP) — AMOP §5.4
 * ADD-DEPENDENT / REMOVE-DEPENDENT / MAP-DEPENDENTS / UPDATE-DEPENDENT
 * with notification hooks on ENSURE-CLASS, ADD-METHOD, REMOVE-METHOD,
 * ENSURE-GENERIC-FUNCTION.
 * ================================================================== */

TEST(dep_add_dependent_registers)
{
    /* ADD-DEPENDENT makes the dependent visible via MAP-DEPENDENTS. */
    eval_print("(defclass dep-c1 () ())");
    eval_print("(defvar *dep-d1* (list :observer))");
    eval_print("(add-dependent (find-class 'dep-c1) *dep-d1*)");
    ASSERT_STR_EQ(eval_print(
        "(let ((seen nil))"
        "  (map-dependents (find-class 'dep-c1) (lambda (d) (push d seen)))"
        "  (eq (first seen) *dep-d1*))"),
        "T");
}

TEST(dep_add_dependent_idempotent)
{
    /* Adding the same dependent twice registers it only once. */
    eval_print("(defclass dep-c2 () ())");
    eval_print("(defvar *dep-d2* (list :obs))");
    eval_print("(add-dependent (find-class 'dep-c2) *dep-d2*)");
    eval_print("(add-dependent (find-class 'dep-c2) *dep-d2*)");
    ASSERT_STR_EQ(eval_print(
        "(let ((n 0))"
        "  (map-dependents (find-class 'dep-c2) (lambda (d) (declare (ignore d)) (incf n)))"
        "  n)"),
        "1");
}

TEST(dep_remove_dependent_drops)
{
    /* REMOVE-DEPENDENT removes a previously registered dependent. */
    eval_print("(defclass dep-c3 () ())");
    eval_print("(defvar *dep-d3* (list :obs))");
    eval_print("(add-dependent (find-class 'dep-c3) *dep-d3*)");
    eval_print("(remove-dependent (find-class 'dep-c3) *dep-d3*)");
    ASSERT_STR_EQ(eval_print(
        "(let ((n 0))"
        "  (map-dependents (find-class 'dep-c3) (lambda (d) (declare (ignore d)) (incf n)))"
        "  n)"),
        "0");
}

TEST(dep_remove_unknown_is_silent)
{
    /* REMOVE-DEPENDENT silently ignores dependents that were never
       registered — returns NIL, no error. */
    eval_print("(defclass dep-c4 () ())");
    ASSERT_STR_EQ(eval_print(
        "(remove-dependent (find-class 'dep-c4) (list :ghost))"),
        "NIL");
}

TEST(dep_map_dependents_visits_all)
{
    /* MAP-DEPENDENTS applies FUNCTION to every registered dependent. */
    eval_print("(defclass dep-c5 () ())");
    eval_print("(defvar *dep-d5a* (list :a))");
    eval_print("(defvar *dep-d5b* (list :b))");
    eval_print("(add-dependent (find-class 'dep-c5) *dep-d5a*)");
    eval_print("(add-dependent (find-class 'dep-c5) *dep-d5b*)");
    ASSERT_STR_EQ(eval_print(
        "(let ((n 0))"
        "  (map-dependents (find-class 'dep-c5) (lambda (d) (declare (ignore d)) (incf n)))"
        "  n)"),
        "2");
}

TEST(dep_update_dependent_default_is_noop)
{
    /* The default UPDATE-DEPENDENT method returns NIL and does nothing. */
    eval_print("(defclass dep-c6 () ())");
    ASSERT_STR_EQ(eval_print(
        "(update-dependent (find-class 'dep-c6) (list :d) 'x)"),
        "NIL");
}

TEST(dep_add_method_notifies)
{
    /* Installing a new method on a GF broadcasts UPDATE-DEPENDENT with
       ('ADD-METHOD METHOD) to each registered dependent. */
    eval_print("(defgeneric dep-gf1 (x))");
    eval_print("(defvar *dep-log1* nil)");
    eval_print(
        "(defmethod update-dependent ((mo standard-generic-function) "
        "                             (d (eql :dep-obs-1)) &rest args)"
        "  (push args *dep-log1*))");
    eval_print("(add-dependent (ensure-generic-function 'dep-gf1) :dep-obs-1)");
    eval_print("(defmethod dep-gf1 ((x integer)) :int)");
    ASSERT_STR_EQ(eval_print(
        "(and (consp *dep-log1*)"
        "     (eq (caar *dep-log1*) 'add-method))"),
        "T");
}

TEST(dep_add_method_passes_method_object)
{
    /* The broadcast's second argument is the method metaobject that
       was just installed. */
    eval_print("(defgeneric dep-gf2 (x))");
    eval_print("(defvar *dep-log2* nil)");
    eval_print(
        "(defmethod update-dependent ((mo standard-generic-function) "
        "                             (d (eql :dep-obs-2)) &rest args)"
        "  (setq *dep-log2* args))");
    eval_print("(add-dependent (ensure-generic-function 'dep-gf2) :dep-obs-2)");
    eval_print("(defmethod dep-gf2 ((x integer)) :i)");
    ASSERT_STR_EQ(eval_print(
        "(eq (%struct-type-name (second *dep-log2*)) 'standard-method)"),
        "T");
}

TEST(dep_remove_method_notifies)
{
    /* REMOVE-METHOD broadcasts UPDATE-DEPENDENT with ('REMOVE-METHOD M). */
    eval_print("(defgeneric dep-gf3 (x))");
    eval_print("(defmethod dep-gf3 ((x integer)) :int)");
    eval_print("(defvar *dep-log3* nil)");
    eval_print(
        "(defmethod update-dependent ((mo standard-generic-function) "
        "                             (d (eql :dep-obs-3)) &rest args)"
        "  (push (first args) *dep-log3*))");
    eval_print("(add-dependent (ensure-generic-function 'dep-gf3) :dep-obs-3)");
    eval_print(
        "(remove-method (ensure-generic-function 'dep-gf3)"
        "               (find-method #'dep-gf3 nil (list (find-class 'integer))))");
    ASSERT_STR_EQ(eval_print("(first *dep-log3*)"), "REMOVE-METHOD");
}

TEST(dep_class_redefinition_notifies)
{
    /* Redefining a class via DEFCLASS fires UPDATE-DEPENDENT on the
       class with 'REINITIALIZE-INSTANCE as the first initarg. */
    eval_print("(defclass dep-c7 () ((a :initform 1)))");
    eval_print("(defvar *dep-log7* nil)");
    eval_print(
        "(defmethod update-dependent ((mo standard-class) "
        "                             (d (eql :dep-obs-7)) &rest args)"
        "  (push (first args) *dep-log7*))");
    eval_print("(add-dependent (find-class 'dep-c7) :dep-obs-7)");
    eval_print("(defclass dep-c7 () ((a :initform 2) (b :initform 3)))");
    ASSERT_STR_EQ(eval_print("(first *dep-log7*)"), "REINITIALIZE-INSTANCE");
}

TEST(class_redefinition_keeps_metaobject_identity)
{
    /* CLHS DEFCLASS / 4.3.6: redefining a class modifies the EXISTING
       class object — FIND-CLASS keeps returning the same metaobject, and
       that metaobject reflects the new definition. */
    eval_print("(defclass redef-id-c () ((a :initform 1)))");
    eval_print("(defvar *redef-id-c1* (find-class 'redef-id-c))");
    eval_print("(defclass redef-id-c () ((a :initform 2) (b :initform 3)))");
    ASSERT_STR_EQ(eval_print("(eq *redef-id-c1* (find-class 'redef-id-c))"), "T");
    ASSERT_STR_EQ(eval_print("(length (class-direct-slots *redef-id-c1*))"), "2");
    ASSERT_STR_EQ(eval_print("(slot-value (make-instance 'redef-id-c) 'b)"), "3");
    ASSERT_STR_EQ(eval_print("(eq (class-of (make-instance 'redef-id-c)) *redef-id-c1*)"),
                  "T");
}

TEST(class_redefinition_keeps_methods_dispatching)
{
    /* The lib/gray-streams.lisp reload bug: OUTPUT-STREAM-P's method is
       specialized on GRAY:FUNDAMENTAL-OUTPUT-STREAM; re-LOADing the file
       re-DEFCLASSes that class, and a stream class defined afterwards no
       longer dispatched to the method (dispatch compares class objects by
       EQ along the CPL, and the redefinition used to allocate a fresh
       metaobject).  A method specialized on a class must keep applying to
       instances of subclasses defined AFTER the class was redefined. */
    eval_print("(defclass redef-m-base () ())");
    eval_print("(defgeneric redef-m-gf (x))");
    eval_print("(defmethod redef-m-gf ((x redef-m-base)) :base)");
    eval_print("(defmethod redef-m-gf ((x t)) :t)");
    eval_print("(defclass redef-m-base () ())");
    eval_print("(defclass redef-m-sub (redef-m-base) ())");
    ASSERT_STR_EQ(eval_print("(redef-m-gf (make-instance 'redef-m-sub))"), ":BASE");
    ASSERT_STR_EQ(eval_print("(redef-m-gf (make-instance 'redef-m-base))"), ":BASE");
    /* Gray's exact shape: base AND intermediate both redefined, then a
       leaf defined below the redefined intermediate. */
    eval_print("(defclass redef-m-mid (redef-m-base) ())");
    eval_print("(defmethod redef-m-gf ((x redef-m-mid)) :mid)");
    eval_print("(defclass redef-m-base () ())");
    eval_print("(defclass redef-m-mid (redef-m-base) ())");
    eval_print("(defclass redef-m-leaf (redef-m-mid) ())");
    ASSERT_STR_EQ(eval_print("(redef-m-gf (make-instance 'redef-m-leaf))"), ":MID");
    /* The method's specializer IS the current class object. */
    ASSERT_STR_EQ(eval_print(
        "(eq (first (method-specializers "
        "       (find-method #'redef-m-gf nil (list (find-class 'redef-m-base)))))"
        "    (find-class 'redef-m-base))"), "T");
}

TEST(class_redefinition_refinalizes_subclasses)
{
    /* CLHS 4.3.6: redefining a class updates its subclasses.  A subclass
       defined BEFORE the redefinition sees the superclass's new slots,
       keeps its own, and stays linked as a direct subclass. */
    eval_print("(defclass redef-s-base () ((a :initform 1)))");
    eval_print("(defclass redef-s-sub (redef-s-base) ((b :initform 2)))");
    eval_print("(defclass redef-s-leaf (redef-s-sub) ((d :initform 4)))");
    ASSERT_STR_EQ(eval_print("(slot-exists-p (make-instance 'redef-s-sub) 'c)"), "NIL");
    eval_print("(defclass redef-s-base () ((a :initform 10) (c :initform 30)))");
    ASSERT_STR_EQ(eval_print(
        "(let ((o (make-instance 'redef-s-sub)))"
        "  (list (slot-value o 'a) (slot-value o 'b) (slot-value o 'c)))"),
        "(10 2 30)");
    /* transitive: the grandchild is re-finalized too */
    ASSERT_STR_EQ(eval_print(
        "(let ((o (make-instance 'redef-s-leaf)))"
        "  (list (slot-value o 'a) (slot-value o 'b) (slot-value o 'c) (slot-value o 'd)))"),
        "(10 2 30 4)");
    ASSERT_STR_EQ(eval_print(
        "(and (member (find-class 'redef-s-sub)"
        "             (class-direct-subclasses (find-class 'redef-s-base))) t)"), "T");
    ASSERT_STR_EQ(eval_print(
        "(and (member (find-class 'redef-s-base)"
        "             (class-precedence-list (class-of (make-instance 'redef-s-leaf)))) t)"),
        "T");
    /* A slot REMOVED from the superclass disappears from the subclass. */
    eval_print("(defclass redef-s-base () ((c :initform 300)))");
    ASSERT_STR_EQ(eval_print("(slot-exists-p (make-instance 'redef-s-sub) 'a)"), "NIL");
    ASSERT_STR_EQ(eval_print(
        "(let ((o (make-instance 'redef-s-leaf)))"
        "  (list (slot-value o 'b) (slot-value o 'c) (slot-value o 'd)))"),
        "(2 300 4)");
}

TEST(class_redefinition_changed_supers_relinks)
{
    /* A redefinition that changes the direct superclasses keeps the
       metaobject but moves it from the old super's direct-subclasses to
       the new one's; TYPEP follows the new hierarchy. */
    eval_print("(defclass redef-r-a () ())");
    eval_print("(defclass redef-r-b () ())");
    eval_print("(defclass redef-r-c (redef-r-a) ())");
    eval_print("(defvar *redef-r-c* (find-class 'redef-r-c))");
    eval_print("(defclass redef-r-c (redef-r-b) ())");
    ASSERT_STR_EQ(eval_print("(eq *redef-r-c* (find-class 'redef-r-c))"), "T");
    ASSERT_STR_EQ(eval_print(
        "(and (member *redef-r-c* (class-direct-subclasses (find-class 'redef-r-a))) t)"),
        "NIL");
    ASSERT_STR_EQ(eval_print(
        "(and (member *redef-r-c* (class-direct-subclasses (find-class 'redef-r-b))) t)"),
        "T");
    ASSERT_STR_EQ(eval_print("(typep (make-instance 'redef-r-c) 'redef-r-a)"), "NIL");
    ASSERT_STR_EQ(eval_print("(typep (make-instance 'redef-r-c) 'redef-r-b)"), "T");
    /* No duplicate link when the super is unchanged across a redefinition. */
    eval_print("(defclass redef-r-c (redef-r-b) ())");
    ASSERT_STR_EQ(eval_print(
        "(count *redef-r-c* (class-direct-subclasses (find-class 'redef-r-b)))"), "1");
}

TEST(class_redefinition_class_prototype_refreshed)
{
    /* The cached CLASS-PROTOTYPE was allocated with the old slot layout;
       a redefinition must drop it so the next call builds a fresh one. */
    eval_print("(defclass redef-p-c () ((a :initform 1)))");
    eval_print("(defvar *redef-p-proto* (class-prototype (find-class 'redef-p-c)))");
    eval_print("(defclass redef-p-c () ((a :initform 1) (b :initform 2)))");
    ASSERT_STR_EQ(eval_print(
        "(eq *redef-p-proto* (class-prototype (find-class 'redef-p-c)))"), "NIL");
    ASSERT_STR_EQ(eval_print(
        "(slot-exists-p (class-prototype (find-class 'redef-p-c)) 'b)"), "T");
}

TEST(class_redefinition_metaclass_keeps_identity)
{
    /* :metaclass path: redefining a class with the same user metaclass
       reinitializes the existing metaobject (AMOP ENSURE-CLASS-USING-CLASS
       on an existing class) — identity holds, methods keep dispatching,
       the metaclass's own slot is updated from the initargs, and a
       REINITIALIZE-INSTANCE :AROUND on the metaclass observes the
       redefinition (INITIALIZE-INSTANCE's :AROUND does not re-fire). */
    eval_print("(defclass redef-mc-meta (standard-class)"
               "  ((tag :initarg :tag :initform :none :reader redef-mc-tag)))");
    eval_print("(defvar *redef-mc-init* 0)");
    eval_print("(defvar *redef-mc-reinit* 0)");
    eval_print("(defmethod initialize-instance :around ((c redef-mc-meta) &rest args)"
               "  (declare (ignore args)) (incf *redef-mc-init*) (call-next-method))");
    eval_print("(defmethod reinitialize-instance :around ((c redef-mc-meta) &rest args)"
               "  (declare (ignore args)) (incf *redef-mc-reinit*) (call-next-method))");
    eval_print("(defclass redef-mc-c () ((a :initform 1)) (:metaclass redef-mc-meta))");
    eval_print("(defvar *redef-mc-c1* (find-class 'redef-mc-c))");
    eval_print("(defgeneric redef-mc-gf (x))");
    eval_print("(defmethod redef-mc-gf ((x redef-mc-c)) :c)");
    ASSERT_STR_EQ(eval_print("*redef-mc-init*"), "1");
    eval_print("(defclass redef-mc-c () ((a :initform 1) (b :initform 2))"
               "  (:metaclass redef-mc-meta))");
    ASSERT_STR_EQ(eval_print("(eq *redef-mc-c1* (find-class 'redef-mc-c))"), "T");
    ASSERT_STR_EQ(eval_print("(class-name (class-of (find-class 'redef-mc-c)))"),
                  "REDEF-MC-META");
    ASSERT_STR_EQ(eval_print("*redef-mc-init*"), "1");
    ASSERT_STR_EQ(eval_print("*redef-mc-reinit*"), "1");
    ASSERT_STR_EQ(eval_print("(slot-value (make-instance 'redef-mc-c) 'b)"), "2");
    eval_print("(defclass redef-mc-sub (redef-mc-c) () (:metaclass redef-mc-meta))");
    ASSERT_STR_EQ(eval_print("(redef-mc-gf (make-instance 'redef-mc-sub))"), ":C");
    /* Metaclass slot: keeps its value across a DEFCLASS redefinition
       (not re-supplied) and updates on a direct REINITIALIZE-INSTANCE.
       AMOP class reinitialization: :DIRECT-SLOTS / :DIRECT-SUPERCLASSES
       / :DIRECT-DEFAULT-INITARGS that are NOT supplied stay unchanged,
       and the super->subclass link is not duplicated. */
    ASSERT_STR_EQ(eval_print("(redef-mc-tag (find-class 'redef-mc-c))"), ":NONE");
    eval_print("(reinitialize-instance (find-class 'redef-mc-c) :tag :tagged)");
    ASSERT_STR_EQ(eval_print("(eq *redef-mc-c1* (find-class 'redef-mc-c))"), "T");
    ASSERT_STR_EQ(eval_print("(redef-mc-tag (find-class 'redef-mc-c))"), ":TAGGED");
    ASSERT_STR_EQ(eval_print("*redef-mc-reinit*"), "2");
    ASSERT_STR_EQ(eval_print("(slot-value (make-instance 'redef-mc-c) 'b)"), "2");
    ASSERT_STR_EQ(eval_print("(redef-mc-gf (make-instance 'redef-mc-sub))"), ":C");
    ASSERT_STR_EQ(eval_print(
        "(count *redef-mc-c1* (class-direct-subclasses (find-class 'standard-object)))"),
        "1");
    ASSERT_STR_EQ(eval_print(
        "(count (find-class 'redef-mc-sub) (class-direct-subclasses *redef-mc-c1*))"),
        "1");
    /* A metaclass CHANGE cannot reuse the struct: a fresh metaobject of
       the new metaclass replaces it, and the class still works. */
    eval_print("(defclass redef-mc-c () ((a :initform 5)))");
    ASSERT_STR_EQ(eval_print("(eq *redef-mc-c1* (find-class 'redef-mc-c))"), "NIL");
    ASSERT_STR_EQ(eval_print("(class-name (class-of (find-class 'redef-mc-c)))"),
                  "STANDARD-CLASS");
    ASSERT_STR_EQ(eval_print("(slot-value (make-instance 'redef-mc-c) 'a)"), "5");
}

TEST(class_redefinition_metaclass_subclass_reinit_keeps_supers)
{
    /* Regression: REINITIALIZE-INSTANCE on a class metaobject whose
       current direct superclass is itself an instance of a user-defined
       metaclass (not one of the whitelisted metaclasses FIND-CLASS
       round-trips a class object for), called WITHOUT re-supplying
       :DIRECT-SUPERCLASSES, must keep the current superclasses per
       AMOP's REINITIALIZE-INSTANCE contract instead of routing the
       already-resolved class objects back through FIND-CLASS (which
       fails to look them up by name and errors). */
    eval_print("(defclass redef-mc2-meta (standard-class) ())");
    eval_print("(defclass redef-mc2-c () () (:metaclass redef-mc2-meta))");
    eval_print("(defclass redef-mc2-sub (redef-mc2-c) () (:metaclass redef-mc2-meta))");
    eval_print("(defvar *redef-mc2-sub* (find-class 'redef-mc2-sub))");
    eval_print("(reinitialize-instance *redef-mc2-sub*)");
    ASSERT_STR_EQ(eval_print("(eq *redef-mc2-sub* (find-class 'redef-mc2-sub))"), "T");
    ASSERT_STR_EQ(eval_print(
        "(eq (car (class-direct-superclasses *redef-mc2-sub*)) (find-class 'redef-mc2-c))"),
        "T");
    ASSERT_STR_EQ(eval_print("(typep (make-instance 'redef-mc2-sub) 'redef-mc2-c)"), "T");
}

TEST(dep_class_redefinition_migrates_dependents)
{
    /* %ENSURE-CLASS updates the class metaobject in place on
       redefinition, so dependents stay attached across any number of
       redefinitions (the migration leg in ENSURE-CLASS-USING-CLASS only
       runs when the metaobject could not be reused). */
    eval_print("(defclass dep-c8 () ())");
    eval_print("(defvar *dep-log8* 0)");
    eval_print(
        "(defmethod update-dependent ((mo standard-class) "
        "                             (d (eql :dep-obs-8)) &rest args)"
        "  (declare (ignore args))"
        "  (incf *dep-log8*))");
    eval_print("(add-dependent (find-class 'dep-c8) :dep-obs-8)");
    eval_print("(defclass dep-c8 () ((x)))");
    eval_print("(defclass dep-c8 () ((x) (y)))");
    ASSERT_STR_EQ(eval_print("*dep-log8*"), "2");
}

TEST(dep_gf_combination_swap_notifies)
{
    /* Swapping a GF's :METHOD-COMBINATION via re-DEFGENERIC broadcasts
       UPDATE-DEPENDENT on the existing GF metaobject. */
    eval_print("(defgeneric dep-gf4 (x))");
    eval_print("(defvar *dep-log4* nil)");
    eval_print(
        "(defmethod update-dependent ((mo standard-generic-function) "
        "                             (d (eql :dep-obs-4)) &rest args)"
        "  (setq *dep-log4* args))");
    eval_print("(add-dependent (ensure-generic-function 'dep-gf4) :dep-obs-4)");
    eval_print("(defgeneric dep-gf4 (x) (:method-combination +))");
    ASSERT_STR_EQ(eval_print("(first *dep-log4*)"), "REINITIALIZE-INSTANCE");
}

TEST(dep_gf_same_combination_no_notify)
{
    /* Re-DEFGENERICing with the same combination must NOT broadcast —
       only real changes trigger the notification. */
    eval_print("(defgeneric dep-gf5 (x) (:method-combination +))");
    eval_print("(defvar *dep-log5* 0)");
    eval_print(
        "(defmethod update-dependent ((mo standard-generic-function) "
        "                             (d (eql :dep-obs-5)) &rest args)"
        "  (declare (ignore args))"
        "  (incf *dep-log5*))");
    eval_print("(add-dependent (ensure-generic-function 'dep-gf5) :dep-obs-5)");
    eval_print("(defgeneric dep-gf5 (x) (:method-combination +))");
    ASSERT_STR_EQ(eval_print("*dep-log5*"), "0");
}

TEST(dep_isolated_between_metaobjects)
{
    /* Dependents registered on one metaobject must not see notifications
       for another metaobject. */
    eval_print("(defgeneric dep-gf6a (x))");
    eval_print("(defgeneric dep-gf6b (x))");
    eval_print("(defvar *dep-log6* 0)");
    eval_print(
        "(defmethod update-dependent ((mo standard-generic-function) "
        "                             (d (eql :dep-obs-6)) &rest args)"
        "  (declare (ignore args))"
        "  (incf *dep-log6*))");
    eval_print("(add-dependent (ensure-generic-function 'dep-gf6a) :dep-obs-6)");
    eval_print("(defmethod dep-gf6b ((x integer)) :b)");
    ASSERT_STR_EQ(eval_print("*dep-log6*"), "0");
}

/* ==================================================================== */
/* Portable MOP shims (closer-mop compatibility layer)                   */
/* ==================================================================== */

TEST(cmshim_metaobject_class_registered)
{
    /* METAOBJECT names a real class — closer-mop imports the symbol and
       downstream code TYPEPs against it. */
    ASSERT_STR_EQ(eval_print("(class-name (find-class 'metaobject))"),
                  "METAOBJECT");
}

TEST(cmshim_specializer_class_registered)
{
    ASSERT_STR_EQ(eval_print("(class-name (find-class 'specializer))"),
                  "SPECIALIZER");
}

TEST(cmshim_forward_referenced_class_registered)
{
    ASSERT_STR_EQ(
        eval_print("(class-name (find-class 'forward-referenced-class))"),
        "FORWARD-REFERENCED-CLASS");
}

TEST(cmshim_accessor_method_subclass_of_standard_method)
{
    /* Accessor-method stubs sit below STANDARD-METHOD in the CPL so that
       libraries that `(typep m 'standard-method)` on generated accessors
       keep working. */
    ASSERT_STR_EQ(eval_print(
        "(not (null (member (find-class 'standard-method)"
        "                   (class-precedence-list"
        "                     (find-class 'standard-reader-method)))))"),
        "T");
}

TEST(cmshim_reader_and_writer_method_classes_distinct)
{
    ASSERT_STR_EQ(eval_print(
        "(not (eq (find-class 'standard-reader-method)"
        "         (find-class 'standard-writer-method)))"),
        "T");
}

TEST(cmshim_direct_slot_definition_abstract_parent)
{
    /* DIRECT-SLOT-DEFINITION is an abstract parent of our reified
       STANDARD-DIRECT-SLOT-DEFINITION; TYPEP succeeds. */
    eval_print("(defclass cmshim-c1 () ((x :initarg :x)))");
    ASSERT_STR_EQ(eval_print(
        "(typep (car (class-direct-slots (find-class 'cmshim-c1)))"
        "       'direct-slot-definition)"),
        "T");
}

TEST(cmshim_effective_slot_definition_abstract_parent)
{
    eval_print("(defclass cmshim-c2 () ((y :initarg :y)))");
    ASSERT_STR_EQ(eval_print(
        "(typep (car (class-slots (find-class 'cmshim-c2)))"
        "       'effective-slot-definition)"),
        "T");
}

TEST(cmshim_classp_true_for_standard_class_instance)
{
    ASSERT_STR_EQ(eval_print("(classp (find-class 'standard-object))"), "T");
}

TEST(cmshim_classp_false_for_non_class)
{
    ASSERT_STR_EQ(eval_print("(classp 42)"), "NIL");
    ASSERT_STR_EQ(eval_print("(classp \"foo\")"), "NIL");
    ASSERT_STR_EQ(eval_print("(classp '(a b c))"), "NIL");
}

TEST(cmshim_generic_function_name_alias)
{
    eval_print("(defgeneric cmshim-gf1 (x))");
    ASSERT_STR_EQ(eval_print("(generic-function-name #'cmshim-gf1)"),
                  "CMSHIM-GF1");
}

TEST(cmshim_generic_function_lambda_list_alias)
{
    eval_print("(defgeneric cmshim-gf2 (x y))");
    ASSERT_STR_EQ(eval_print("(generic-function-lambda-list #'cmshim-gf2)"),
                  "(X Y)");
}

TEST(cmshim_generic_function_methods_alias)
{
    eval_print("(defgeneric cmshim-gf3 (x))");
    eval_print("(defmethod cmshim-gf3 ((x integer)) :int)");
    ASSERT_STR_EQ(eval_print("(length (generic-function-methods #'cmshim-gf3))"),
                  "1");
}

TEST(cmshim_generic_function_method_combination_alias)
{
    eval_print("(defgeneric cmshim-gf4 (x) (:method-combination +))");
    ASSERT_STR_EQ(eval_print(
        "(method-combination-name (generic-function-method-combination "
        "                           #'cmshim-gf4))"),
        "+");
}

TEST(cmshim_generic_function_method_class_default)
{
    eval_print("(defgeneric cmshim-gf5 (x))");
    ASSERT_STR_EQ(eval_print(
        "(eq (generic-function-method-class #'cmshim-gf5)"
        "    (find-class 'standard-method))"),
        "T");
}

TEST(cmshim_generic_function_argument_precedence_order)
{
    eval_print("(defgeneric cmshim-gf6 (a b &optional c))");
    ASSERT_STR_EQ(eval_print(
        "(generic-function-argument-precedence-order #'cmshim-gf6)"),
        "(A B)");
}

TEST(cmshim_generic_function_declarations_empty)
{
    eval_print("(defgeneric cmshim-gf7 (x))");
    ASSERT_STR_EQ(eval_print("(generic-function-declarations #'cmshim-gf7)"),
                  "NIL");
}

TEST(cmshim_camuc_class_specializers_valid)
{
    /* With only class-specialized methods, the result is determined
       from classes alone — VALIDP is T. */
    eval_print("(defgeneric cmshim-camuc1 (x))");
    eval_print("(defmethod cmshim-camuc1 ((x integer)) :int)");
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-bind (ms validp)"
        "    (compute-applicable-methods-using-classes"
        "      #'cmshim-camuc1 (list (find-class 'integer)))"
        "  (list (length ms) validp))"),
        "(1 T)");
}

TEST(cmshim_camuc_eql_specializer_invalidates)
{
    /* When a method has an EQL specializer, we cannot decide purely from
       classes — the dispatcher falls back to COMPUTE-APPLICABLE-METHODS
       and closer-mop signals that via VALIDP=NIL. */
    eval_print("(defgeneric cmshim-camuc2 (x))");
    eval_print("(defmethod cmshim-camuc2 ((x (eql 1))) :one)");
    eval_print("(defmethod cmshim-camuc2 ((x integer)) :int)");
    ASSERT_STR_EQ(eval_print(
        "(multiple-value-bind (ms validp)"
        "    (compute-applicable-methods-using-classes"
        "      #'cmshim-camuc2 (list (find-class 'integer)))"
        "  validp)"),
        "NIL");
}

TEST(cmshim_compute_effective_method_single)
{
    /* COMPUTE-EFFECTIVE-METHOD returns a CALL-METHOD form for a single
       applicable method. */
    eval_print("(defgeneric cmshim-cem1 (x))");
    eval_print("(defmethod cmshim-cem1 ((x integer)) :int)");
    ASSERT_STR_EQ(eval_print(
        "(car (compute-effective-method"
        "       #'cmshim-cem1"
        "       (generic-function-method-combination #'cmshim-cem1)"
        "       (generic-function-methods #'cmshim-cem1)))"),
        "CALL-METHOD");
}

TEST(cmshim_ensure_generic_function_using_class_delegates)
{
    /* ENSURE-GENERIC-FUNCTION-USING-CLASS forwards to the primary
       ENSURE-GENERIC-FUNCTION regardless of the leading GF arg. */
    ASSERT_STR_EQ(eval_print(
        "(progn (ensure-generic-function-using-class nil 'cmshim-egf1"
        "         :lambda-list '(x))"
        "       (generic-function-name #'cmshim-egf1))"),
        "CMSHIM-EGF1");
}

TEST(cmshim_specializer_direct_methods_stub)
{
    /* Stubbed — we don't track the back-link, so return NIL. */
    ASSERT_STR_EQ(eval_print(
        "(specializer-direct-methods (find-class 'integer))"),
        "NIL");
}

TEST(cmshim_specializer_direct_generic_functions_stub)
{
    ASSERT_STR_EQ(eval_print(
        "(specializer-direct-generic-functions (find-class 'integer))"),
        "NIL");
}

TEST(cmshim_add_remove_direct_method_noop)
{
    /* No back-link maintenance — both calls return NIL and do not
       signal. */
    eval_print("(defgeneric cmshim-drm (x))");
    eval_print("(defmethod cmshim-drm ((x integer)) :i)");
    ASSERT_STR_EQ(eval_print(
        "(add-direct-method (find-class 'integer)"
        "                   (car (generic-function-methods #'cmshim-drm)))"),
        "NIL");
    ASSERT_STR_EQ(eval_print(
        "(remove-direct-method (find-class 'integer)"
        "                      (car (generic-function-methods #'cmshim-drm)))"),
        "NIL");
}

TEST(cmshim_add_direct_subclass_records)
{
    /* ADD-DIRECT-SUBCLASS must update CLASS-DIRECT-SUBCLASSES. */
    eval_print("(defclass cmshim-super1 () ())");
    eval_print("(defclass cmshim-sub1 () ())");
    eval_print(
        "(add-direct-subclass (find-class 'cmshim-super1)"
        "                     (find-class 'cmshim-sub1))");
    ASSERT_STR_EQ(eval_print(
        "(not (null (member (find-class 'cmshim-sub1)"
        "                   (class-direct-subclasses (find-class 'cmshim-super1))"
        "                   :test #'eq)))"),
        "T");
}

TEST(cmshim_add_direct_subclass_idempotent)
{
    /* Adding the same subclass twice must not duplicate the entry. */
    eval_print("(defclass cmshim-super2 () ())");
    eval_print("(defclass cmshim-sub2 () ())");
    eval_print(
        "(add-direct-subclass (find-class 'cmshim-super2)"
        "                     (find-class 'cmshim-sub2))");
    eval_print(
        "(add-direct-subclass (find-class 'cmshim-super2)"
        "                     (find-class 'cmshim-sub2))");
    ASSERT_STR_EQ(eval_print(
        "(count (find-class 'cmshim-sub2)"
        "       (class-direct-subclasses (find-class 'cmshim-super2))"
        "       :test #'eq)"),
        "1");
}

TEST(cmshim_remove_direct_subclass_drops)
{
    eval_print("(defclass cmshim-super3 () ())");
    eval_print("(defclass cmshim-sub3 () ())");
    eval_print(
        "(add-direct-subclass (find-class 'cmshim-super3)"
        "                     (find-class 'cmshim-sub3))");
    eval_print(
        "(remove-direct-subclass (find-class 'cmshim-super3)"
        "                        (find-class 'cmshim-sub3))");
    ASSERT_STR_EQ(eval_print(
        "(member (find-class 'cmshim-sub3)"
        "        (class-direct-subclasses (find-class 'cmshim-super3))"
        "        :test #'eq)"),
        "NIL");
}

TEST(cmshim_accessor_method_slot_definition_stub)
{
    /* We don't back-link accessors to their slot-defs. */
    eval_print("(defclass cmshim-c4 () ((z :accessor cmshim-c4-z)))");
    eval_print("(defmethod cmshim-c4-z ((x cmshim-c4)) (call-next-method))");
    ASSERT_STR_EQ(eval_print(
        "(accessor-method-slot-definition"
        "  (car (generic-function-methods #'cmshim-c4-z)))"),
        "NIL");
}

TEST(cmshim_reader_method_class_returns_standard)
{
    eval_print("(defclass cmshim-c5 () ((a)))");
    ASSERT_STR_EQ(eval_print(
        "(eq (reader-method-class (find-class 'cmshim-c5)"
        "                         (car (class-direct-slots"
        "                                (find-class 'cmshim-c5))))"
        "    (find-class 'standard-reader-method))"),
        "T");
}

TEST(cmshim_writer_method_class_returns_standard)
{
    eval_print("(defclass cmshim-c6 () ((a)))");
    ASSERT_STR_EQ(eval_print(
        "(eq (writer-method-class (find-class 'cmshim-c6)"
        "                         (car (class-direct-slots"
        "                                (find-class 'cmshim-c6))))"
        "    (find-class 'standard-writer-method))"),
        "T");
}

/* DOCUMENTATION is a generic function in CLHS.  Regression: libraries
 * like lisp-namespace add (defmethod documentation ((x symbol) (type (eql ...)))),
 * which should leave plain (documentation 'foo 'function) still answerable
 * via the (t t) fallback method — otherwise NO-APPLICABLE-METHOD blocks
 * loading of introspect-environment and everything downstream. */
TEST(documentation_generic_fallback_after_specialization)
{
    /* Fallback stores / retrieves through the existing doc-table */
    ASSERT_STR_EQ(eval_print(
        "(progn (setf (documentation 'docgf-sym 'function) \"docA\")"
        "       (documentation 'docgf-sym 'function))"),
        "\"docA\"");
    /* After a user defmethod on a specific EQL doc-type, the (t t)
     * method still handles the original doc-type */
    eval_print(
        "(defmethod documentation ((x symbol) (type (eql 'mynamespace)))"
        "  'specialized-result)");
    ASSERT_STR_EQ(eval_print(
        "(documentation 'docgf-sym 'mynamespace)"),
        "SPECIALIZED-RESULT");
    ASSERT_STR_EQ(eval_print(
        "(documentation 'docgf-sym 'function)"),
        "\"docA\"");
}

TEST(allocate_instance_gf_default_method)
{
    /* Regression: allocate-instance must be a GF so libraries (serapeum's
       abstract-standard-class, ContextL, etc.) can define methods on it.
       Without a default (class t) method, specializing on a user metaclass
       or a specific class breaks make-instance for all OTHER classes with
       "No applicable method for ALLOCATE-INSTANCE". */
    eval_print("(defclass ai-gf-base () ((x :initarg :x :initform 42)))");
    eval_print("(defclass ai-gf-special (ai-gf-base) ())");
    /* Specialize allocate-instance on the special class */
    eval_print(
        "(defmethod allocate-instance ((c (eql (find-class 'ai-gf-special))) &rest initargs)"
        "  (declare (ignore initargs))"
        "  (error \"forbidden\"))");
    /* Base class make-instance still works via the default (class t) method */
    ASSERT_STR_EQ(eval_print("(slot-value (make-instance 'ai-gf-base) 'x)"), "42");
    ASSERT_STR_EQ(eval_print("(slot-value (make-instance 'ai-gf-base :x 7) 'x)"), "7");
}

TEST(defmethod_aux_specialized_lambda_list)
{
    /* Regression: a specialized method lambda list may end in &aux bindings
       whose init-forms call accessors on the specialized args, e.g.
         (defmethod m ((x foo) y &aux (v (foo-s x))) ...)
       %parse-specialized-lambda-list must STOP collecting specializers at
       &aux (like &optional/&key) — otherwise the &aux binding (v (foo-s x))
       is mistaken for a specialized parameter and (foo-s x) is handed to
       FIND-CLASS ("No class named (FOO-S X)").  This broke loading cl-routes
       (drakma's URL router), whose UNIFY/IMPL methods use exactly this shape. */
    eval_print("(defclass aux-foo () ((s :initarg :s :accessor aux-foo-s)))");
    eval_print("(defgeneric aux-g (a b))");
    eval_print("(defmethod aux-g ((a aux-foo) b &aux (v (aux-foo-s a)))"
               "  (list v b))");
    ASSERT_STR_EQ(
        eval_print("(aux-g (make-instance 'aux-foo :s 99) 7)"),
        "(99 7)");
}

TEST(control_macros_macroexpand_conformantly)
{
    /* Regression: COND/AND/OR/CASE/TYPECASE are compiler-inlined special
       forms, but per CLHS they are MACROS and MACROEXPAND-1 must return a
       real expansion (not the form unchanged).  Code walkers (iterate) rely
       on this to descend into clause bodies; the old identity stubs made
       (macroexpand-1 '(cond ...)) a no-op, so walkers refused to enter and
       e.g. iterate's COLLECT inside a COND reached run time undefined. */
    ASSERT_STR_EQ(eval_print("(equal (macroexpand-1 '(and a b c))"
                             "        '(if a (and b c)))"), "T");
    /* or makes progress (expands away from the (or ...) head) */
    ASSERT_STR_EQ(eval_print("(not (eq (car (macroexpand-1 '(or a b))) 'or))"), "T");
    /* cond expands to an if, not to itself */
    ASSERT_STR_EQ(eval_print("(eq (car (macroexpand-1 '(cond (a 1) (t 2)))) 'if)"),
                  "T");
    /* macro-function is non-NIL AND expansion makes progress (not eq form) */
    ASSERT_STR_EQ(eval_print("(and (macro-function 'cond)"
                             "     (not (eq (macroexpand-1 '(cond (a 1)))"
                             "              '(cond (a 1)))))"), "T");
    /* case expands into a cond keyed by eql/member */
    ASSERT_STR_EQ(eval_print("(eq (car (macroexpand-1 '(case x (1 'a) (t 'b)))) 'let)"),
                  "T");
}

/* COMPUTE-APPLICABLE-METHODS is a standard CLHS/AMOP function; portable CLOS
 * clients (e.g. snooze) call it by name.  It must be FBOUND and return the
 * applicable methods most-specific-first. */
TEST(compute_applicable_methods_public)
{
    eval_print("(defgeneric cam-g (x))");
    eval_print("(defmethod cam-g ((x integer)) 'int)");
    eval_print("(defmethod cam-g ((x number)) 'num)");
    eval_print("(defmethod cam-g ((x string)) 'str)");
    ASSERT_STR_EQ(eval_print("(fboundp 'compute-applicable-methods)"), "T");
    /* integer arg => integer and number methods apply (2). */
    ASSERT_STR_EQ(eval_print("(length (compute-applicable-methods #'cam-g (list 5)))"), "2");
    /* string arg => only the string method (1). */
    ASSERT_STR_EQ(eval_print("(length (compute-applicable-methods #'cam-g (list \"a\")))"), "1");
}

/* Regression: an auxiliary (:before/:after) method must NOT observe the
 * CALL-NEXT-METHOD state of an ENCLOSING dispatch.  :before/:after methods
 * are applied raw, so the effective method must bind the call-next-method
 * special (*CNM*) to NIL around them; otherwise a non-conformant
 * :after doing (when (next-method-p) (call-next-method)) leaks into the outer
 * GF's method chain whenever the GF is dispatched from inside another GF's
 * method.  Real-world failure: a sento mailbox INITIALIZE-INSTANCE :after
 * jumped into hunchentoot's request handler when a make-instance ran on a
 * hunchentoot worker thread (whose enclosing dispatch had the specials bound);
 * the main thread worked only because its enclosing values happened to be NIL.
 * This reproduces it single-threaded by nesting the dispatch explicitly. */
TEST(aux_method_no_cnm_leak_from_enclosing_dispatch)
{
    eval_print("(defparameter *cnm-leak* nil)");
    /* Inner class whose :after consults next-method-p / call-next-method
     * (non-conformant, but exactly what sento's mailbox init does). */
    eval_print("(defclass cnm-inner () ())");
    eval_print("(defmethod initialize-instance :after ((o cnm-inner) &key)"
               "  (when (next-method-p) (call-next-method)))");
    /* Outer GF with a CALL-NEXT-METHOD chain: while the cnm-derived primary
     * runs, *next-method-p-function* is bound (cnm-base's method is next). */
    eval_print("(defclass cnm-base () ())");
    eval_print("(defclass cnm-derived (cnm-base) ())");
    eval_print("(defgeneric cnm-ping (x))");
    eval_print("(defmethod cnm-ping ((x cnm-base)) (setf *cnm-leak* t) :base)");
    /* Dispatch the inner make-instance while the outer specials are bound; do
     * NOT call-next-method here, so cnm-base's method is reachable ONLY if the
     * inner :after leaks the outer chain. */
    eval_print("(defmethod cnm-ping ((x cnm-derived)) (make-instance 'cnm-inner) :derived)");
    ASSERT_STR_EQ(eval_print("(cnm-ping (make-instance 'cnm-derived))"), ":DERIVED");
    ASSERT_STR_EQ(eval_print("*cnm-leak*"), "NIL");
}

/* === SLOT-VALUE / WITH-SLOTS on condition objects (CLHS 9.1) ===
 * Conditions are a distinct object type in amilisp (not CLOS instances), but
 * the standard slot-access functions must still work on them. */

TEST(slot_value_on_condition)
{
    eval_print("(define-condition sv-cond () "
               "  ((a :initarg :a) (b :initarg :b)))");
    eval_print("(defparameter *svc* (make-condition 'sv-cond :a 10 :b 20))");
    ASSERT_STR_EQ(eval_print("(slot-value *svc* 'a)"), "10");
    ASSERT_STR_EQ(eval_print("(slot-value *svc* 'b)"), "20");
}

TEST(slot_value_on_condition_nil_value)
{
    /* A slot explicitly initialized to NIL reads back as NIL (not an error). */
    eval_print("(define-condition sv-niln () ((x :initarg :x)))");
    eval_print("(defparameter *svn* (make-condition 'sv-niln :x nil))");
    ASSERT_STR_EQ(eval_print("(slot-value *svn* 'x)"), "NIL");
}

TEST(with_slots_on_condition)
{
    eval_print("(define-condition ws-cond () "
               "  ((s :initarg :s) (tt :initarg :tt)))");
    eval_print("(defparameter *wsc* (make-condition 'ws-cond :s 'foo :tt 'bar))");
    ASSERT_STR_EQ(eval_print(
        "(with-slots (s tt) *wsc* (list s tt))"), "(FOO BAR)");
}

TEST(setf_slot_value_on_condition)
{
    eval_print("(define-condition ss-cond () ((v :initarg :v)))");
    eval_print("(defparameter *ssc* (make-condition 'ss-cond :v 1))");
    eval_print("(setf (slot-value *ssc* 'v) 99)");
    ASSERT_STR_EQ(eval_print("(slot-value *ssc* 'v)"), "99");
}

TEST(slot_boundp_on_condition)
{
    eval_print("(define-condition sb-cond () "
               "  ((bound :initarg :bound) (unbound)))");
    eval_print("(defparameter *sbc* (make-condition 'sb-cond :bound 1))");
    ASSERT_STR_EQ(eval_print("(slot-boundp *sbc* 'bound)"), "T");
    ASSERT_STR_EQ(eval_print("(slot-boundp *sbc* 'unbound)"), "NIL");
}

/* === DEFSTRUCT CLOS classes expose slot metaobjects (MOP) ===
 * class-slots / compute-slots must reflect the struct's real layout so MOP
 * introspection (closer-mop callers such as trivia) works. */

TEST(struct_class_slots_populated)
{
    eval_print("(defstruct mop-point xx yy)");
    ASSERT_STR_EQ(eval_print(
        "(mapcar #'slot-definition-name (class-slots (find-class 'mop-point)))"),
        "(XX YY)");
    /* The initarg keyword is reified so trivia's slot lookup by :initarg works. */
    ASSERT_STR_EQ(eval_print(
        "(slot-definition-initargs (car (class-slots (find-class 'mop-point))))"),
        "(:XX)");
}

TEST(struct_class_slots_inherited)
{
    eval_print("(defstruct mop-base aa bb)");
    eval_print("(defstruct (mop-sub (:include mop-base)) cc)");
    /* Inherited slots first, then own — matching the physical layout. */
    ASSERT_STR_EQ(eval_print(
        "(mapcar #'slot-definition-name (class-slots (find-class 'mop-sub)))"),
        "(AA BB CC)");
    /* SLOT-VALUE still resolves through the structure fast-path. */
    ASSERT_STR_EQ(eval_print(
        "(slot-value (make-mop-sub :aa 1 :bb 2 :cc 3) 'cc)"), "3");
}

int main(void)
{
    test_init();
    setup();

    /* Step 1: %class-of */
    RUN(class_of_fixnum);
    RUN(class_of_nil);
    RUN(class_of_symbol);
    RUN(class_of_cons);
    RUN(class_of_string);
    RUN(class_of_character);
    RUN(class_of_make_array_character_string);
    RUN(class_of_make_array_base_char_string);
    RUN(class_of_make_array_adjustable_string);
    RUN(class_of_function);
    RUN(class_of_lambda);
    RUN(class_of_vector);
    RUN(class_of_hashtable);
    RUN(class_of_struct);
    RUN(class_of_float);
    RUN(class_of_ratio);
    RUN(class_of_pathname);
    RUN(class_of_package);
    RUN(class_of_stream);

    /* Step 2: Bootstrap core classes (requires loading clos.lisp) */
    RUN(clos_require);
    RUN(find_class_integer);
    RUN(class_name_integer);
    RUN(class_name_string);
    RUN(class_name_cons);
    RUN(class_name_t);
    RUN(find_class_unknown_nil);
    RUN(find_class_unknown_error);
    RUN(class_of_42_is_fixnum_class);
    RUN(class_of_string_is_string_class);
    RUN(class_of_symbol_independent_of_package);
    RUN(class_of_builtins_independent_of_package);
    RUN(class_of_name_symbol_is_cl_package);
    RUN(dispatch_symbol_method_independent_of_package);
    RUN(dispatch_string_method_on_make_array_string);
    RUN(dispatch_negative_cache_self_heals);
    RUN(dispatch_genuine_miss_still_errors);
    RUN(dispatch_no_primary_self_heals);
    RUN(dispatch_genuine_no_primary_still_errors);
    RUN(dispatch_fresh_miss_empty_self_heals);
    RUN(dispatch_fresh_miss_genuine_still_errors);
    RUN(dispatch_no_primary_roster_gate);
    RUN(gf_dispatch_slow_path_binds_current_method_args_before_heal);
    RUN(class_of_nil_is_null_class);
    RUN(class_of_cons_is_cons_class);
    RUN(class_of_eq_find_class);
    RUN(direct_supers_integer);
    RUN(direct_supers_t);
    RUN(logical_pathname_class_exists);
    RUN(logical_pathname_superclass_is_pathname);
    RUN(logical_pathname_defmethod_loads);
    RUN(file_stream_class_exists);
    RUN(file_stream_superclass_is_stream);
    RUN(file_stream_defmethod_loads);
    RUN(cpl_fixnum);
    RUN(cpl_cons);
    RUN(cpl_null);
    RUN(cpl_vector);
    RUN(dispatch_nil_as_list);
    RUN(dispatch_vector_as_sequence);
    RUN(setf_find_class);
    RUN(direct_subclasses_integer);

    /* Phase 1: Slot access */
    RUN(phase1_setup);
    RUN(slot_value_read);
    RUN(slot_value_write);
    RUN(slot_boundp_true);
    RUN(slot_makunbound_and_boundp);
    RUN(slot_value_unbound_error);
    RUN(unbound_slot_condition_via_slot_value);
    RUN(unbound_slot_condition_via_reader_gf);
    RUN(unbound_slot_condition_after_makunbound);
    RUN(unbound_slot_is_a_cell_error);
    RUN(unbound_slot_report_names_slot_and_instance);
    RUN(unbound_slot_instance_accessor);
    RUN(slot_value_inline_setf_eval_order);
    RUN(slot_value_inline_read_eval_order);
    RUN(slot_value_inline_setf_returns_value);
    RUN(slot_value_inline_no_such_slot_errors);
    RUN(slot_value_funcall_bypasses_inline);
    RUN(slot_value_pair_index_wide_class);
    RUN(slot_value_pair_index_class_redef);
    RUN(slot_exists_p_true);
    RUN(slot_exists_p_false);
    RUN(slot_value_no_such_slot);

    /* Phase 2: C3 linearization */
    RUN(c3_single_inheritance);
    RUN(c3_diamond);
    RUN(c3_root);

    /* Phase 3: defclass */
    RUN(defclass_basic);
    RUN(defclass_find_class);
    RUN(defclass_cpl);
    RUN(defclass_effective_slots);
    RUN(defclass_slot_index_table);
    RUN(defclass_accessor_defined);
    RUN(defclass_inheritance);
    RUN(defclass_reader_writer);
    RUN(defclass_no_supers);

    /* Phase 4: make-instance */
    RUN(make_instance_basic);
    RUN(make_instance_initform);
    RUN(make_instance_initarg_overrides_initform);
    RUN(make_instance_unbound_slot);
    RUN(make_instance_by_class_object);
    RUN(make_instance_inherited_slots);

    /* Phase 5: defgeneric + defmethod */
    RUN(defgeneric_basic);
    RUN(defmethod_basic);
    RUN(defmethod_dispatch_by_class);
    RUN(defmethod_call_next_method);
    RUN(defmethod_before_after);
    RUN(defmethod_around);
    RUN(defmethod_unspecialized);
    RUN(defmethod_no_applicable);
    RUN(next_method_p_test);

    /* Phase 6: with-slots */
    RUN(with_slots_read);
    RUN(with_slots_write);
    RUN(with_slots_renamed);

    /* Phase 7: GF accessors + init protocol */
    RUN(gf_accessor);
    RUN(gf_accessor_works);
    RUN(gf_accessor_setf);
    RUN(gf_accessor_setf_distinct_per_package);
    RUN(gf_initialize_instance);

    /* Phase 8: change-class + reinitialize-instance */
    RUN(reinitialize_instance_basic);
    RUN(change_class_basic);
    RUN(change_class_with_initargs);

    /* Phase 9: print-object */
    RUN(print_object_class);
    RUN(print_object_gf);
    RUN(print_object_custom);
    RUN(print_object_honors_print_level);
    RUN(print_object_wide_string_result);
    RUN(condition_report_wide_string);
    RUN(print_object_default_struct);

    /* slot-value on DEFSTRUCT instances */
    RUN(slot_value_on_struct);
    RUN(setf_slot_value_on_struct);
    RUN(with_slots_on_struct);
    RUN(slot_boundp_on_struct);
    RUN(slot_exists_p_on_struct);
    RUN(slot_value_unknown_slot_struct);

    /* ERROR :format-control formats its report */
    RUN(error_format_control_interpolates);

    /* Phase 10: typep with multiple inheritance */
    RUN(typep_multiple_inheritance);
    RUN(typep_multiple_inheritance_or);
    RUN(typep_multiple_inheritance_negative);

    /* Phase 11: dispatch cache */
    RUN(cache_basic_dispatch);
    RUN(cache_hit_correct_result);
    RUN(cache_different_classes);
    RUN(cache_invalidated_on_defmethod);
    RUN(cache_invalidated_on_defclass);
    RUN(cache_eql_specializer_bypass);
    RUN(cache_multi_dispatch_bypass);
    RUN(cache_no_applicable_method);
    RUN(cache_before_after_around);

    /* Phase 12: EMF cache */
    RUN(emf_cache_call_next_method_no_args);
    RUN(emf_cache_call_next_method_with_args);
    RUN(emf_cache_before_after_around);
    RUN(emf_cache_next_method_p);
    RUN(emf_cache_no_applicable_cached);
    RUN(no_applicable_method_gf_exists);
    RUN(no_applicable_method_consistent_type);
    RUN(no_applicable_method_serapeum_deftype);
    RUN(no_applicable_method_return_value);

    /* Phase 13: Multi-dispatch cache */
    RUN(multi_cache_two_arg);
    RUN(multi_cache_hit);
    RUN(multi_cache_different_combos);
    RUN(multi_cache_cacheable_p_value);
    RUN(multi_cache_invalidation);
    RUN(multi_cache_call_next_method);

    /* Phase 14: EQL specializer cache */
    RUN(eql_cache_basic);
    RUN(eql_cache_hit);
    RUN(eql_cache_mixed_eql_class);
    RUN(eql_cache_class_fallback);
    RUN(eql_cache_call_next_method);
    RUN(eql_cache_nil_value);
    RUN(eql_cache_arg2_eql);
    RUN(eql_cache_invalidation);
    RUN(eql_cache_cacheable_p);

    /* Slot definition metaobjects (MOP) */
    RUN(mop1_direct_slot_is_metaobject);
    RUN(mop1_effective_slot_is_metaobject);
    RUN(mop1_class_slots_alias);
    RUN(mop1_slot_def_accessors_name);
    RUN(mop1_slot_def_accessors_initargs);
    RUN(mop1_slot_def_accessors_readers_writers);
    RUN(mop1_slot_def_accessors_allocation);
    RUN(mop1_slot_def_accessors_initform);
    RUN(mop1_effective_slot_location);
    RUN(mop1_direct_slot_definition_class_gf);
    RUN(mop1_direct_slot_definition_class_customizable);
    RUN(mop1_inheritance_merges_slots);
    RUN(mop1_inheritance_initargs_unioned);

    /* Class finalization protocol (MOP) */
    RUN(finalize_class_finalized_p);
    RUN(finalize_inheritance_idempotent);
    RUN(compute_class_precedence_list_default);
    RUN(compute_slots_returns_effective);
    RUN(compute_effective_slot_definition_default);
    RUN(compute_slots_around_hook);
    RUN(compute_default_initargs_inherits);
    RUN(compute_default_initargs_most_specific_wins);
    RUN(validate_superclass_default);
    RUN(class_direct_default_initargs_accessor);
    RUN(class_prototype_allocates);
    RUN(ensure_class_creates);
    RUN(ensure_class_using_class_dispatches);

    /* Slot-access protocol (MOP) */
    RUN(svuc_fast_path_flag_clean);
    RUN(svuc_default_method_matches_struct_ref);
    RUN(svuc_location_accessor);
    RUN(svuc_setf_default_roundtrips);
    RUN(svuc_around_observes_reads);
    RUN(svuc_around_can_override_read);
    RUN(svuc_setf_around_observes_writes);
    RUN(svuc_boundp_dispatches);
    RUN(svuc_makunbound_dispatches);
    RUN(svuc_struct_fallback_unaffected);
    RUN(svuc_slot_unbound_still_fires);

    /* :allocation :class (MOP) */
    RUN(calloc_two_instances_share);
    RUN(calloc_subclass_inherits_cell);
    RUN(calloc_subclass_redefines_gets_own_cell);
    RUN(calloc_location_is_cons);
    RUN(calloc_location_mixed);
    RUN(calloc_boundp_and_makunbound);
    RUN(calloc_initform_runs_once);
    RUN(calloc_initarg_writes_shared_cell);
    RUN(calloc_struct_smaller_than_effective_slots);
    RUN(calloc_svuc_protocol_sees_class_slots);

    /* Reified EQL specializers (MOP) */
    RUN(eqlspec_intern_identity);
    RUN(eqlspec_intern_eql_discriminates);
    RUN(eqlspec_object_accessor);
    RUN(eqlspec_p_predicate);
    RUN(eqlspec_class_bootstrapped);
    RUN(eqlspec_dispatch_still_works);
    RUN(eqlspec_method_specializers_contains_metaobject);
    RUN(eqlspec_method_specializers_shared_across_methods);
    RUN(eqlspec_extract_specializer_names_roundtrips);
    RUN(eqlspec_extract_class_specializers);
    RUN(eqlspec_method_equal_replaces_on_redef);
    RUN(eqlspec_cacheable_p_still_eql);

    /* Funcallable standard class (MOP) */
    RUN(funcallable_class_bootstrapped);
    RUN(funcallable_gf_is_funcallable_standard_object);
    RUN(funcallable_class_of_gf);
    RUN(funcallable_gf_still_callable_via_symbol);
    RUN(funcallable_gf_via_funcall_and_apply);
    RUN(funcallable_set_funcallable_instance_function);
    RUN(funcallable_sfi_returns_gf);
    RUN(funcallable_standard_instance_access);
    RUN(funcallable_standard_instance_access_equals_struct_ref);
    RUN(funcallable_standard_instance_access_variant);
    RUN(funcallable_compute_discriminating_function_default);
    RUN(funcallable_gf_type_of);
    RUN(funcallable_gf_cpl_has_function);
    RUN(gfclass_custom_type_of);
    RUN(gfclass_custom_is_funcallable);
    RUN(gfclass_initialize_instance_after_fires);
    RUN(gfclass_dispatch_on_gf_argument);
    RUN(gfclass_default_unchanged);

    /* Method metaobject protocol (MOP) */
    RUN(mmop_method_generic_function_backlink);
    RUN(mmop_extract_lambda_list_plain);
    RUN(mmop_extract_lambda_list_preserves_keywords);
    RUN(mmop_extract_specializer_names_padded_t);
    RUN(mmop_extract_specializer_names_stops_at_keywords);
    RUN(mmop_add_method_installs_new_method);
    RUN(mmop_add_method_sets_back_link);
    RUN(mmop_add_method_returns_gf);
    RUN(mmop_remove_method_drops_dispatch);
    RUN(mmop_remove_method_clears_backlink);
    RUN(mmop_find_method_returns_existing);
    RUN(mmop_find_method_accepts_class_names);
    RUN(mmop_find_method_eql_names);
    RUN(mmop_find_method_missing_errors);
    RUN(mmop_find_method_missing_returns_nil);
    RUN(mmop_ensure_method_installs);
    RUN(mmop_ensure_method_default_specializers_are_t);
    RUN(mmop_ensure_method_returns_method);
    RUN(mmop_make_method_lambda_default_identity);

    /* Method combination (MOP) */
    RUN(mc_standard_default);
    RUN(mc_plus_basic);
    RUN(mc_plus_identity_with_one_argument);
    RUN(mc_list_collects);
    RUN(mc_and_short_circuits);
    RUN(mc_or_returns_first_true);
    RUN(mc_progn_returns_last);
    RUN(mc_append_concatenates);
    RUN(mc_max_reduces);
    RUN(mc_around_wraps_short_form);
    RUN(mc_invalid_qualifier_errors);
    RUN(mc_standard_still_supports_before_after);
    RUN(mc_find_method_combination_standard);
    RUN(mc_find_method_combination_type);
    RUN(mc_find_method_combination_unknown_errors);
    RUN(mc_define_short_form_basic);
    RUN(mc_define_short_form_identity_default);
    RUN(mc_define_short_form_operator_defaults_to_name);
    RUN(mc_define_long_form_basic);
    RUN(mc_define_long_form_multiple_groups);
    RUN(mc_define_long_form_qualifier_pattern);
    RUN(mc_long_form_make_method);
    RUN(mc_make_method_cnm_signals_no_next_method);
    RUN(mc_long_form_order_most_specific_last);
    RUN(mc_long_form_standard_context);
    RUN(mc_method_combination_object_type);
    RUN(mc_method_combination_class_bootstrapped);
    RUN(mc_call_method_uses_current_args);
    RUN(mc_redefine_updates_combination);

    /* Dependent maintenance protocol (MOP) */
    RUN(dep_add_dependent_registers);
    RUN(dep_add_dependent_idempotent);
    RUN(dep_remove_dependent_drops);
    RUN(dep_remove_unknown_is_silent);
    RUN(dep_map_dependents_visits_all);
    RUN(dep_update_dependent_default_is_noop);
    RUN(dep_add_method_notifies);
    RUN(dep_add_method_passes_method_object);
    RUN(dep_remove_method_notifies);
    RUN(dep_class_redefinition_notifies);
    RUN(dep_class_redefinition_migrates_dependents);

    /* Class redefinition keeps the metaobject (CLHS 4.3.6) */
    RUN(class_redefinition_keeps_metaobject_identity);
    RUN(class_redefinition_keeps_methods_dispatching);
    RUN(class_redefinition_refinalizes_subclasses);
    RUN(class_redefinition_changed_supers_relinks);
    RUN(class_redefinition_class_prototype_refreshed);
    RUN(class_redefinition_metaclass_keeps_identity);
    RUN(class_redefinition_metaclass_subclass_reinit_keeps_supers);
    RUN(dep_gf_combination_swap_notifies);
    RUN(dep_gf_same_combination_no_notify);
    RUN(dep_isolated_between_metaobjects);

    /* Portable MOP shims (closer-mop compatibility layer) */
    RUN(cmshim_metaobject_class_registered);
    RUN(cmshim_specializer_class_registered);
    RUN(cmshim_forward_referenced_class_registered);
    RUN(cmshim_accessor_method_subclass_of_standard_method);
    RUN(cmshim_reader_and_writer_method_classes_distinct);
    RUN(cmshim_direct_slot_definition_abstract_parent);
    RUN(cmshim_effective_slot_definition_abstract_parent);
    RUN(cmshim_classp_true_for_standard_class_instance);
    RUN(cmshim_classp_false_for_non_class);
    RUN(cmshim_generic_function_name_alias);
    RUN(cmshim_generic_function_lambda_list_alias);
    RUN(cmshim_generic_function_methods_alias);
    RUN(cmshim_generic_function_method_combination_alias);
    RUN(cmshim_generic_function_method_class_default);
    RUN(cmshim_generic_function_argument_precedence_order);
    RUN(cmshim_generic_function_declarations_empty);
    RUN(cmshim_camuc_class_specializers_valid);
    RUN(cmshim_camuc_eql_specializer_invalidates);
    RUN(cmshim_compute_effective_method_single);
    RUN(cmshim_ensure_generic_function_using_class_delegates);
    RUN(cmshim_specializer_direct_methods_stub);
    RUN(cmshim_specializer_direct_generic_functions_stub);
    RUN(cmshim_add_remove_direct_method_noop);
    RUN(cmshim_add_direct_subclass_records);
    RUN(cmshim_add_direct_subclass_idempotent);
    RUN(cmshim_remove_direct_subclass_drops);
    RUN(cmshim_accessor_method_slot_definition_stub);
    RUN(cmshim_reader_method_class_returns_standard);
    RUN(cmshim_writer_method_class_returns_standard);

    RUN(documentation_generic_fallback_after_specialization);
    RUN(allocate_instance_gf_default_method);
    RUN(defmethod_aux_specialized_lambda_list);
    RUN(control_macros_macroexpand_conformantly);
    RUN(compute_applicable_methods_public);
    RUN(aux_method_no_cnm_leak_from_enclosing_dispatch);

    RUN(slot_value_on_condition);
    RUN(slot_value_on_condition_nil_value);
    RUN(with_slots_on_condition);
    RUN(setf_slot_value_on_condition);
    RUN(slot_boundp_on_condition);
    RUN(struct_class_slots_populated);
    RUN(struct_class_slots_inherited);

    teardown();
    REPORT();
}
