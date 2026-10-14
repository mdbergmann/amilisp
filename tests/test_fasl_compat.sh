#!/bin/sh
# Cross-session FASL compatibility tests
# Verifies that FASL files created in one session work correctly in another.
# Run: make test (or sh tests/test_fasl_compat.sh build/host/amilisp)

AMILISP="${1:-build/host/amilisp}"
passed=0
failed=0
total=0
TMPDIR="${TMPDIR:-/tmp}"
# Cache path includes the impl version and CL_FASL_VERSION ("-faslN"
# suffix) — wildcard on both so the test matches the current build and
# cleans every version's cache, independent of CL_VERSION_STRING bumps.
CACHE_GLOB="$HOME/.cache/common-lisp/amilisp-*"

check() {
    desc="$1"
    expected="$2"
    actual="$3"
    total=$((total + 1))
    if [ "$actual" = "$expected" ]; then
        echo "  ok  $desc"
        passed=$((passed + 1))
    else
        echo "  FAIL  $desc"
        echo "    expected: $(echo "$expected" | head -5)"
        echo "    got:      $(echo "$actual" | head -5)"
        failed=$((failed + 1))
    fi
}

check_contains() {
    desc="$1"
    pattern="$2"
    actual="$3"
    total=$((total + 1))
    if echo "$actual" | grep -q "$pattern"; then
        echo "  ok  $desc"
        passed=$((passed + 1))
    else
        echo "  FAIL  $desc"
        echo "    expected to contain: $pattern"
        echo "    got: $(echo "$actual" | head -5)"
        failed=$((failed + 1))
    fi
}

check_not_contains() {
    desc="$1"
    pattern="$2"
    actual="$3"
    total=$((total + 1))
    if echo "$actual" | grep -q "$pattern"; then
        echo "  FAIL  $desc"
        echo "    expected NOT to contain: $pattern"
        echo "    got: $(echo "$actual" | head -5)"
        failed=$((failed + 1))
    else
        echo "  ok  $desc"
        passed=$((passed + 1))
    fi
}

# Helper: run amilisp in batch mode with *load-verbose* suppressed
# Usage: run_quiet '(lisp expression)'
run_quiet() {
    echo "(setf *load-verbose* nil) $1" | "$AMILISP" --no-userinit --batch 2>&1
}

# Clean FASL cache to start fresh
rm -rf $CACHE_GLOB

# --- Test 1: Basic defun survives FASL round-trip ---

cat > "$TMPDIR/fasl_test_defun.lisp" << 'LISP'
(defun fasl-test-add (a b) (+ a b))
LISP

# Session 1: load source (creates FASL cache)
run_quiet '(load "'"$TMPDIR"'/fasl_test_defun.lisp")' > /dev/null

# Session 2: load again (should use FASL cache)
result=$(run_quiet '(load "'"$TMPDIR"'/fasl_test_defun.lisp") (fasl-test-add 10 20)')
check "fasl_defun_cross_session" "NIL
T
30" "$result"

# --- Test 2: defpackage + in-package survives FASL round-trip ---

cat > "$TMPDIR/fasl_test_pkg.lisp" << 'LISP'
(defpackage :fasl-test-pkg
  (:use :cl)
  (:export :greet))

(in-package :fasl-test-pkg)

(defun greet () "hello-from-pkg")

(in-package :cl-user)
LISP

# Session 1: load source
run_quiet '(load "'"$TMPDIR"'/fasl_test_pkg.lisp")' > /dev/null

# Session 2: load from FASL, verify package and function exist
result=$(run_quiet '(load "'"$TMPDIR"'/fasl_test_pkg.lisp") (not (null (find-package :fasl-test-pkg))) (fasl-test-pkg:greet)')
check "fasl_defpackage_cross_session" "NIL
T
T
\"hello-from-pkg\"" "$result"

# --- Test 3: defpackage with nicknames ---

cat > "$TMPDIR/fasl_test_nick.lisp" << 'LISP'
(defpackage :fasl-test-long-name
  (:use :cl)
  (:nicknames :ftn)
  (:export :val))

(in-package :fasl-test-long-name)
(defun val () 42)
(in-package :cl-user)
LISP

# Session 1
run_quiet '(load "'"$TMPDIR"'/fasl_test_nick.lisp")' > /dev/null

# Session 2: verify nickname works
result=$(run_quiet '(load "'"$TMPDIR"'/fasl_test_nick.lisp") (ftn:val)')
check "fasl_nickname_cross_session" "NIL
T
42" "$result"

# --- Test 4: defvar / defparameter survive FASL ---

cat > "$TMPDIR/fasl_test_vars.lisp" << 'LISP'
(defvar *fasl-test-var* 99)
(defparameter *fasl-test-param* "hello")
LISP

# Session 1
run_quiet '(load "'"$TMPDIR"'/fasl_test_vars.lisp")' > /dev/null

# Session 2
result=$(run_quiet '(load "'"$TMPDIR"'/fasl_test_vars.lisp") (list *fasl-test-var* *fasl-test-param*)')
check "fasl_defvar_cross_session" "NIL
T
(99 \"hello\")" "$result"

# --- Test 5: Multiple dependent files (load order) ---

cat > "$TMPDIR/fasl_test_base.lisp" << 'LISP'
(defpackage :fasl-dep-base (:use :cl) (:export :base-val))
(in-package :fasl-dep-base)
(defun base-val () 100)
(in-package :cl-user)
LISP

cat > "$TMPDIR/fasl_test_dep.lisp" << 'LISP'
(defpackage :fasl-dep-child (:use :cl :fasl-dep-base) (:export :child-val))
(in-package :fasl-dep-child)
(defun child-val () (+ (base-val) 50))
(in-package :cl-user)
LISP

# Session 1
run_quiet '(load "'"$TMPDIR"'/fasl_test_base.lisp") (load "'"$TMPDIR"'/fasl_test_dep.lisp")' > /dev/null

# Session 2: both files loaded from FASL
result=$(run_quiet '(load "'"$TMPDIR"'/fasl_test_base.lisp") (load "'"$TMPDIR"'/fasl_test_dep.lisp") (fasl-dep-child:child-val)')
check "fasl_dependent_files_cross_session" "NIL
T
T
150" "$result"

# --- Test 6: compile-file produces loadable FASL ---

cat > "$TMPDIR/fasl_test_cf.lisp" << 'LISP'
(defun cf-test-fn () :compiled-ok)
LISP

# Session 1: compile-file explicitly
run_quiet '(compile-file "'"$TMPDIR"'/fasl_test_cf.lisp")' > /dev/null

# Session 2: load should find the FASL cache
result=$(run_quiet '(load "'"$TMPDIR"'/fasl_test_cf.lisp") (cf-test-fn)')
check "fasl_compile_file_cross_session" "NIL
T
:COMPILED-OK" "$result"

# --- Test 7: ASDF compile-file integration ---
# Verify that ASDF-compiled FASLs are real binaries (not dummy markers)

cat > "$TMPDIR/fasl_test_asdf.lisp" << 'LISP'
(defpackage :fasl-asdf-test (:use :cl) (:export :asdf-val))
(in-package :fasl-asdf-test)
(defun asdf-val () :asdf-ok)
(in-package :cl-user)
LISP

# Session 1: compile with ASDF loaded
run_quiet '(require "asdf") (compile-file "'"$TMPDIR"'/fasl_test_asdf.lisp")' > /dev/null

# Session 2: load from FASL
result=$(run_quiet '(load "'"$TMPDIR"'/fasl_test_asdf.lisp") (fasl-asdf-test:asdf-val)')
check "fasl_asdf_compile_cross_session" "NIL
T
:ASDF-OK" "$result"

# --- Test 8: FASL loads from cache (verify cache is used) ---

cat > "$TMPDIR/fasl_test_cache.lisp" << 'LISP'
(defun cache-test-fn () :cached)
LISP

# Session 1: load to create cache
run_quiet '(load "'"$TMPDIR"'/fasl_test_cache.lisp")' > /dev/null

# Session 2: load with *load-verbose* to verify FASL path is used
result=$(echo '(setf *load-verbose* t) (load "'"$TMPDIR"'/fasl_test_cache.lisp") (cache-test-fn)' \
    | "$AMILISP" --no-userinit --batch 2>&1)
check_contains "fasl_loaded_from_cache" ".fasl" "$result"
check_contains "fasl_cached_result_correct" "CACHED" "$result"

# --- Test 9: Stale FASL (wrong version) is replaced by source recompile ---
# Regression: bumping CL_FASL_VERSION used to leave stale FASLs in the cache
# directory. The loader now (a) keys the cache directory on the FASL version
# AND (b) pre-validates the version field, so a stale FASL transparently
# falls back to recompiling from source.

cat > "$TMPDIR/fasl_test_stale.lisp" << 'LISP'
(defun stale-test-fn () :recompiled-ok)
LISP

# Session 1: load to create a fresh FASL cache.
run_quiet '(load "'"$TMPDIR"'/fasl_test_stale.lisp")' > /dev/null

# Locate the cached FASL and corrupt its version bytes (offset 4-5, big-endian
# u16). Setting them to 0xFFFF guarantees mismatch with any real version.
stale_fasl=$(find $CACHE_GLOB -name 'fasl_test_stale.fasl' 2>/dev/null | head -1)
if [ -z "$stale_fasl" ] || [ ! -f "$stale_fasl" ]; then
    echo "  FAIL  fasl_stale_version_recovery (could not find cached FASL)"
    failed=$((failed + 1))
    total=$((total + 1))
else
    printf '\xff\xff' | dd of="$stale_fasl" bs=1 seek=4 count=2 conv=notrunc 2>/dev/null

    # Make the FASL newer than the source so the loader prefers it. Without
    # the fix, this would error out with "FASL: unsupported version".
    touch "$stale_fasl"

    # Session 2: should silently recompile and return the right value.
    result=$(run_quiet '(load "'"$TMPDIR"'/fasl_test_stale.lisp") (stale-test-fn)')
    check "fasl_stale_version_recovery" "NIL
T
:RECOMPILED-OK" "$result"
fi

# --- Test 10: source lambda-list survives FASL (EXT:FUNCTION-ARGLIST) ---
# Regression for FASL v9: CL_Bytecode.source_lambda_list must round-trip so
# editor tooling (Sly arglist) sees the written lambda-list — including
# &optional default forms — after loading from a FASL.

cat > "$TMPDIR/fasl_test_arglist.lisp" << 'LISP'
(defun fasl-arglist-fn (a b &optional (c 7) &key kw) (list a b c kw))
LISP

# Session 1: load source (creates FASL cache)
run_quiet '(load "'"$TMPDIR"'/fasl_test_arglist.lisp")' > /dev/null

# Session 2: load from FASL, read the captured lambda-list back
result=$(run_quiet '(load "'"$TMPDIR"'/fasl_test_arglist.lisp") (ext:function-arglist (function fasl-arglist-fn))')
check "fasl_arglist_cross_session" "NIL
T
(A B &OPTIONAL (C 7) &KEY KW)" "$result"

# --- Tests for compile-file CLHS 3.2.3.1 top-level form handling ---
# All tests use a single amilisp process: compile-file + load in one session.

# Regression: compile-file must NOT evaluate DEFVAR initforms at compile time.
# Bug: (make-package :foo) was called twice (once during compile-file, once on
# load), causing "Package already exists" on the second call.

cat > "$TMPDIR/cf_defvar_trace.lisp" << 'LISP'
(defvar *cf-trace* nil)
(defvar *cf-side* (progn (push :compiled *cf-trace*) :v))
LISP

rm -rf $CACHE_GLOB
result=$(run_quiet '(compile-file "'"$TMPDIR"'/cf_defvar_trace.lisp" :output-file "'"$TMPDIR"'/cf_defvar_trace.fasl") (load "'"$TMPDIR"'/cf_defvar_trace.fasl") *cf-trace*')
check_contains "compile_file_does_not_eval_defvar_initform" "(:COMPILED)" "$result"
check_not_contains "compile_file_defvar_not_run_twice" "(:COMPILED :COMPILED)" "$result"
rm -f "$TMPDIR/cf_defvar_trace.lisp" "$TMPDIR/cf_defvar_trace.fasl"

# Exact repro of the original bug: compile-file + load must not error.
cat > "$TMPDIR/cf_pkg_once.lisp" << 'LISP'
(defvar *cf-pkg* (make-package :amilisp-bug-a-test-pkg))
LISP

rm -rf $CACHE_GLOB
result=$(run_quiet '(compile-file "'"$TMPDIR"'/cf_pkg_once.lisp" :output-file "'"$TMPDIR"'/cf_pkg_once.fasl") (load "'"$TMPDIR"'/cf_pkg_once.fasl") (not (null (find-package :amilisp-bug-a-test-pkg)))')
check_contains "compile_file_does_not_run_make_package_twice" "T" "$result"
check_not_contains "compile_file_no_package_already_exists_error" "already exists" "$result"
rm -f "$TMPDIR/cf_pkg_once.lisp" "$TMPDIR/cf_pkg_once.fasl"

# eval-when (:compile-toplevel) body runs at compile time.
cat > "$TMPDIR/cf_ew_ct.lisp" << 'LISP'
(eval-when (:compile-toplevel) (format t "CF-EW-CT-RAN~%"))
LISP

rm -rf $CACHE_GLOB
cf_out=$(echo "(setf *compile-verbose* nil) (compile-file \"$TMPDIR/cf_ew_ct.lisp\" :output-file \"$TMPDIR/cf_ew_ct.fasl\")" \
    | "$AMILISP" --no-userinit --batch 2>&1)
check_contains "compile_file_eval_when_compile_toplevel_runs" "CF-EW-CT-RAN" "$cf_out"
rm -f "$TMPDIR/cf_ew_ct.lisp" "$TMPDIR/cf_ew_ct.fasl"

# eval-when (:load-toplevel) body does NOT run at compile time, but DOES at load.
cat > "$TMPDIR/cf_ew_lt.lisp" << 'LISP'
(eval-when (:load-toplevel) (format t "CF-EW-LT-RAN~%"))
LISP

rm -rf $CACHE_GLOB
cf_out=$(echo "(setf *compile-verbose* nil) (compile-file \"$TMPDIR/cf_ew_lt.lisp\" :output-file \"$TMPDIR/cf_ew_lt.fasl\")" \
    | "$AMILISP" --no-userinit --batch 2>&1)
check_not_contains "compile_file_eval_when_load_toplevel_silent_at_compile" "CF-EW-LT-RAN" "$cf_out"
load_out=$(echo "(load \"$TMPDIR/cf_ew_lt.fasl\")" \
    | "$AMILISP" --no-userinit --batch 2>&1)
check_contains "compile_file_eval_when_load_toplevel_runs_at_load" "CF-EW-LT-RAN" "$load_out"
rm -f "$TMPDIR/cf_ew_lt.lisp" "$TMPDIR/cf_ew_lt.fasl"

# DEFMACRO defined in compile-file is available for subsequent forms.
cat > "$TMPDIR/cf_defmacro.lisp" << 'LISP'
(defmacro cf-mac (x) `(list :wrapped ,x))
(defparameter *cf-mac-r* (cf-mac 7))
LISP

rm -rf $CACHE_GLOB
result=$(run_quiet '(compile-file "'"$TMPDIR"'/cf_defmacro.lisp" :output-file "'"$TMPDIR"'/cf_defmacro.fasl") (load "'"$TMPDIR"'/cf_defmacro.fasl") *cf-mac-r*')
check_contains "compile_file_defmacro_available_for_later_forms" "(:WRAPPED 7)" "$result"
rm -f "$TMPDIR/cf_defmacro.lisp" "$TMPDIR/cf_defmacro.fasl"

# Non-defining top-level forms must NOT run at compile time (only at load).
cat > "$TMPDIR/cf_counter.lisp" << 'LISP'
(defparameter *cf-counter* 0)
(incf *cf-counter*)
LISP

rm -rf $CACHE_GLOB
result=$(run_quiet '(compile-file "'"$TMPDIR"'/cf_counter.lisp" :output-file "'"$TMPDIR"'/cf_counter.fasl") (load "'"$TMPDIR"'/cf_counter.fasl") (format t "COUNTER=~a~%" *cf-counter*)')
check_contains "compile_file_does_not_run_top_level_call_twice" "COUNTER=1" "$result"
check_not_contains "compile_file_counter_not_2" "COUNTER=2" "$result"
rm -f "$TMPDIR/cf_counter.lisp" "$TMPDIR/cf_counter.fasl"

# Regression: a COMPILE-FILE'd DEFSTRUCT with (:include ...) must keep its
# inherited slots in the default keyword constructor.  Bug: defstruct expanded
# to PROGN, so %register-struct-type ran only at load time; a later same-file
# (:include base) couldn't resolve the parent's slots at compile time and the
# compiled constructor dropped the inherited keyword args.  Both structs live in
# ONE file so the child is macroexpanded against the compile-time-registered base.
cat > "$TMPDIR/cf_include.lisp" << 'LISP'
(defstruct (cf-base (:constructor %make-cf-base)) (cf-comm nil) cf-a)
(defstruct (cf-sub (:include cf-base)) cf-b)
(defun cf-mk () (cf-sub-cf-comm (make-cf-sub :cf-comm :spawn :cf-a 1 :cf-b 2)))
LISP

rm -rf $CACHE_GLOB
result=$(run_quiet '(compile-file "'"$TMPDIR"'/cf_include.lisp" :output-file "'"$TMPDIR"'/cf_include.fasl") (load "'"$TMPDIR"'/cf_include.fasl") (format t "INC=~S~%" (cf-mk))')
check_contains "compile_file_include_struct_keeps_inherited_ctor_keywords" "INC=:SPAWN" "$result"
check_not_contains "compile_file_include_struct_no_unknown_keyword" "Unknown keyword" "$result"
rm -f "$TMPDIR/cf_include.lisp" "$TMPDIR/cf_include.fasl"

# Regression (log4cl): a top-level MACROLET whose body (a local-macro call)
# expands to a DEFPACKAGE must create the package at compile time, so a later
# same-file form can read symbols in that package (CLHS 3.2.3.1: macrolet body
# forms are processed as top-level forms with the local macros in scope).
# Before the fix, compile-file compiled the whole macrolet as one load-time
# unit, so the package did not exist when the reader hit 'cf-ml-pkg:marker' →
# "Package CF-ML-PKG not found" / "Unexpected ')'".
cat > "$TMPDIR/cf_ml_pkg.lisp" << 'LISP'
(macrolet ((def-it () '(defpackage :cf-ml-pkg (:use :cl) (:export #:marker))))
  (def-it))
(defun cf-ml-fn () (symbol-name 'cf-ml-pkg:marker))
LISP

rm -rf $CACHE_GLOB
result=$(run_quiet '(compile-file "'"$TMPDIR"'/cf_ml_pkg.lisp" :output-file "'"$TMPDIR"'/cf_ml_pkg.fasl") (load "'"$TMPDIR"'/cf_ml_pkg.fasl") (format t "ML=~A~%" (cf-ml-fn))')
check_contains "compile_file_toplevel_macrolet_defpackage_compile_time" "ML=MARKER" "$result"
check_not_contains "compile_file_toplevel_macrolet_no_package_error" "not found" "$result"
rm -f "$TMPDIR/cf_ml_pkg.lisp" "$TMPDIR/cf_ml_pkg.fasl"

# Regression: a DEFMACRO that lives inside a top-level MACROLET body must be
# installed at compile time so a later same-file form expands it (not compiled
# as a function call → load-time "Undefined function").
cat > "$TMPDIR/cf_ml_mac.lisp" << 'LISP'
(macrolet ((emit () '(defmacro cf-ml-mac (x) `(list :ml ,x))))
  (emit))
(defparameter *cf-ml-mac* (cf-ml-mac 7))
LISP

rm -rf $CACHE_GLOB
result=$(run_quiet '(compile-file "'"$TMPDIR"'/cf_ml_mac.lisp" :output-file "'"$TMPDIR"'/cf_ml_mac.fasl") (load "'"$TMPDIR"'/cf_ml_mac.fasl") *cf-ml-mac*')
check_contains "compile_file_toplevel_macrolet_defmacro_available_later" "(:ML 7)" "$result"
check_not_contains "compile_file_toplevel_macrolet_no_undefined_fn" "Undefined function" "$result"
rm -f "$TMPDIR/cf_ml_mac.lisp" "$TMPDIR/cf_ml_mac.fasl"

# Regression: a DEFMACRO inside a top-level LOCALLY must be installed at compile
# time too (locally body forms are top-level forms, CLHS 3.2.3.1).
cat > "$TMPDIR/cf_loc_mac.lisp" << 'LISP'
(locally (declare (optimize speed))
  (defmacro cf-loc-mac (x) `(list :loc ,x)))
(defparameter *cf-loc* (cf-loc-mac 9))
LISP

rm -rf $CACHE_GLOB
result=$(run_quiet '(compile-file "'"$TMPDIR"'/cf_loc_mac.lisp" :output-file "'"$TMPDIR"'/cf_loc_mac.fasl") (load "'"$TMPDIR"'/cf_loc_mac.fasl") *cf-loc*')
check_contains "compile_file_toplevel_locally_defmacro_available_later" "(:LOC 9)" "$result"
check_not_contains "compile_file_toplevel_locally_no_undefined_fn" "Undefined function" "$result"
rm -f "$TMPDIR/cf_loc_mac.lisp" "$TMPDIR/cf_loc_mac.fasl"

# --- Cleanup ---

rm -f "$TMPDIR"/fasl_test_*.lisp

# --- Summary ---

echo ""
echo "$passed passed, $failed failed, $total total"
if [ "$failed" -gt 0 ]; then
    echo "FAIL"
    exit 1
else
    echo "PASS"
    exit 0
fi
