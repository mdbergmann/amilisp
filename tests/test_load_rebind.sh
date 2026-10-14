#!/bin/sh
# Regression test: LOAD / COMPILE-FILE bind *PACKAGE*, *READTABLE*,
# *LOAD-PATHNAME*/*LOAD-TRUENAME* as REAL dynamic bindings (CLHS 23.2.7 /
# 24.2.2), so they are restored even when the load is ABORTED by a condition
# handled outside it.
#
# Bug (found by the real-Amiga suite 2026-08-09): bi_load restored *PACKAGE*
# with a C-local save/restore that only ran on normal return.  A file that did
# IN-PACKAGE and then errored, with the error handled by the CALLER's
# handler-case, unwound via NLX past bi_load's C frame — no restore ran, the
# caller was left in the dead file's package, and (in the Amiga suite) every
# later top-level form re-interned its symbols fresh: 156 cascading
# "Undefined function: CHECK" failures from one bad load.
#
# Also covers the sibling fix: C error frames (CL_CATCH) now restore
# cl_dyn_top on unwind (CL_ErrorFrame.saved_dyn_top), so a form aborted
# mid-LET-of-a-special no longer leaks its dynamic bindings into the rest
# of the load.
#
# Run: sh tests/test_load_rebind.sh build/host/amilisp

AMILISP="${1:-build/host/amilisp}"
passed=0
failed=0
total=0
TMPD=$(mktemp -d "${TMPDIR:-/tmp}/test_load_rebind_XXXXXX") || exit 1
trap 'rm -rf "$TMPD"' EXIT INT TERM

run_quiet() {
    # $1 = forms; stdin-batch mode, *load-verbose* off
    echo "(setf *load-verbose* nil) $1" | "$AMILISP" --no-userinit --batch 2>&1
}

check_contains() {
    desc="$1"; pattern="$2"; out="$3"
    total=$((total + 1))
    if echo "$out" | grep -q "$pattern"; then
        echo "  ok  $desc"
        passed=$((passed + 1))
    else
        echo "  FAIL  $desc (missing: $pattern)"
        echo "    output: $(echo "$out" | head -8)"
        failed=$((failed + 1))
    fi
}

# --- fixture files ---------------------------------------------------------

# in-package then error: the poster child for the abort-path restore
cat > "$TMPD/bomb.lisp" <<'EOF'
(defpackage :load-rebind-bomb (:use :cl))
(in-package :load-rebind-bomb)
(cl:error "boom")
EOF

# readtable mutation then error
cat > "$TMPD/rt-bomb.lisp" <<'EOF'
(setq *readtable* (copy-readtable nil))
(error "rt-boom")
EOF

# unhandled mid-file error: load prints + continues, later forms still eval
cat > "$TMPD/partial.lisp" <<'EOF'
(defpackage :load-rebind-partial (:use :cl))
(in-package :load-rebind-partial)
(cl:error "mid-file boom")
(cl:defvar cl-user::*load-rebind-after-err* 42)
EOF

# aborted LET-of-a-special: dyn bindings of the dead form must not leak
# into subsequent forms (CL_ErrorFrame.saved_dyn_top)
cat > "$TMPD/dynleak.lisp" <<'EOF'
(let ((*print-base* 16))
  (error "inside let"))
(format t "BASE-AFTER-FORM=~a~%" *print-base*)
EOF

# --- Test 1: package restored when load error is handled by the caller ---
out=$(run_quiet "
 (handler-case (load \"$TMPD/bomb.lisp\") (error () nil))
 (format t \"PKG=~a~%\" (package-name *package*))")
check_contains "package-restored-after-handled-abort" "PKG=COMMON-LISP-USER" "$out"

# --- Test 2: readtable restored when load error is handled by the caller ---
out=$(run_quiet "
 (defvar *rt* *readtable*)
 (handler-case (load \"$TMPD/rt-bomb.lisp\") (error () nil))
 (format t \"RT-SAME=~a~%\" (eq *rt* *readtable*))")
check_contains "readtable-restored-after-handled-abort" "RT-SAME=T" "$out"

# --- Test 3: *load-pathname* restored (NIL at toplevel) after abort ---
out=$(run_quiet "
 (handler-case (load \"$TMPD/bomb.lisp\") (error () nil))
 (format t \"LP-NIL=~a~%\" (null *load-pathname*))")
check_contains "load-pathname-restored-after-handled-abort" "LP-NIL=T" "$out"

# --- Test 4: unhandled mid-file error — load continues, package restored ---
out=$(run_quiet "
 (load \"$TMPD/partial.lisp\")
 (format t \"PKG=~a AFTER=~a~%\" (package-name *package*)
         (boundp 'cl-user::*load-rebind-after-err*))")
check_contains "package-restored-after-unhandled-error" "PKG=COMMON-LISP-USER AFTER=T" "$out"

# --- Test 5: nested load abort (inner file loads the bomb) ---
cat > "$TMPD/outer.lisp" <<EOF
(defpackage :load-rebind-outer (:use :cl))
(in-package :load-rebind-outer)
(cl:load "$TMPD/bomb.lisp")
EOF
out=$(run_quiet "
 (handler-case (load \"$TMPD/outer.lisp\") (error () nil))
 (format t \"PKG=~a~%\" (package-name *package*))")
check_contains "package-restored-after-nested-abort" "PKG=COMMON-LISP-USER" "$out"

# --- Test 6: FASL-cache path — second load of the bomb hits the auto-cache ---
out=$(run_quiet "
 (handler-case (load \"$TMPD/bomb.lisp\") (error () nil))
 (handler-case (load \"$TMPD/bomb.lisp\") (error () nil))
 (format t \"PKG=~a~%\" (package-name *package*))")
check_contains "package-restored-after-cached-abort" "PKG=COMMON-LISP-USER" "$out"

# --- Test 7: aborted form's LET bindings don't leak into later forms ---
out=$(run_quiet "(load \"$TMPD/dynleak.lisp\") (format t \"BASE-AT-TOP=~a~%\" *print-base*)")
check_contains "form-let-binding-popped-for-next-form" "BASE-AFTER-FORM=10" "$out"
check_contains "form-let-binding-popped-at-toplevel"   "BASE-AT-TOP=10"     "$out"

# --- Test 8: compile-file — package/readtable restored after handled abort ---
cat > "$TMPD/cf-bomb.lisp" <<'EOF'
(defpackage :load-rebind-cf (:use :cl))
(in-package :load-rebind-cf)
(eval-when (:compile-toplevel :load-toplevel :execute)
  (cl:error "cf-boom"))
EOF
out=$(run_quiet "
 (handler-case (compile-file \"$TMPD/cf-bomb.lisp\"
                             :output-file \"$TMPD/cf-bomb.fasl\")
   (error () nil))
 (format t \"PKG=~a~%\" (package-name *package*))")
check_contains "compile-file-package-restored-after-abort" "PKG=COMMON-LISP-USER" "$out"

# --- Test 9: abort-path restore under GC stress (compaction every alloc) ---
# Only meaningful when the binary was built with DEBUG_GC_STRESS; on a normal
# build AMILISP_GC_STRESS=1 is ignored and this is a cheap re-run of Test 1.
out=$(echo "(setf *load-verbose* nil)
 (handler-case (load \"$TMPD/bomb.lisp\") (error () nil))
 (format t \"PKG=~a~%\" (package-name *package*))" \
      | AMILISP_GC_STRESS=1 "$AMILISP" --no-userinit --batch 2>&1)
check_contains "package-restored-under-gc-stress" "PKG=COMMON-LISP-USER" "$out"

echo ""
echo "$passed passed, $failed failed, $total total"
if [ "$failed" -gt 0 ]; then
    echo "FAIL"
    exit 1
fi
echo "PASS"
exit 0
