#!/bin/sh
# Regression tests for COMPILE-FILE reader-package handling.
#
# During COMPILE-FILE the reader interns each form's symbols using the package
# established by the file's IN-PACKAGE forms.  A compile-time evaluation can
# transiently rebind *PACKAGE* (e.g. ASDF's session/visiting machinery unwinds
# dynamic *PACKAGE* bindings during the operate that drives compile-file), and
# that must NOT switch the reader to the wrong package mid-file.  If it does, a
# later form's symbols intern in the wrong package, producing a *different*
# symbol object than the file's earlier forms registered — observed in
# esrap/expressions.lisp as EXPRESSION-CASE interning in COMMON-LISP-USER and
# the compiler error "FUNCTION: OUTPUT is not a valid function name".
#
# COMPILE-FILE preserves the reader package across each top-level form, except
# IN-PACKAGE (including IN-PACKAGE nested in PROGN/EVAL-WHEN/LOCALLY/MACROLET),
# whose change must persist.
#
# Run fresh amilisp per case (a cold heap) so this exercises the compile path
# directly without the accumulated-heap slowdown of the C test harness.
# Run: sh tests/test_compile_file_package.sh [path-to-amilisp]

AMILISP="${1:-build/host/amilisp}"
passed=0
failed=0
TMP="${TMPDIR:-/tmp}/clamiga-cfpkg-$$"
mkdir -p "$TMP"
cleanup() { rm -rf "$TMP" "$HOME/.cache/common-lisp/amilisp-"*"$TMP"* 2>/dev/null; }
trap cleanup EXIT

run_case() {
    desc="$1"
    src="$2"
    check="$3"          # a Lisp form that must evaluate to T
    srcfile="$TMP/$desc.lisp"
    printf '%s\n' "$src" > "$srcfile"
    driver="$TMP/$desc-driver.lisp"
    cat > "$driver" <<EOF
(handler-case
    (progn
      (compile-file "$srcfile")
      (format t "~&RESULT=~a~%" (if $check :pass :fail)))
  (error (e) (format t "~&RESULT=error ~a~%" e)))
EOF
    out=$(AMILISP_NO_USERINIT=1 "$AMILISP" --no-userinit --non-interactive \
              --load "$driver" </dev/null 2>&1)
    res=$(printf '%s\n' "$out" | sed -n 's/^RESULT=//p' | head -1)
    if [ "$res" = ":PASS" ] || [ "$res" = "PASS" ]; then
        echo "  ok  $desc"
        passed=$((passed + 1))
    else
        echo "  FAIL  $desc (RESULT=$res)"
        printf '%s\n' "$out" | grep -iE "OUTPUT|error|RESULT" | head -5
        failed=$((failed + 1))
    fi
}

# 1. Plain top-level IN-PACKAGE switches the reader package for later forms.
run_case "plain_in_package" \
"(defpackage :cfpkg-a (:use :cl))
(in-package :cfpkg-a)
(defvar cfpkg-marker-a 1)" \
'(and (find-symbol "CFPKG-MARKER-A" :cfpkg-a) t)'

# 2. PROGN-wrapped IN-PACKAGE still switches the reader package (the per-form
#    package restore must be skipped via the nested IN-PACKAGE signal).
run_case "progn_wrapped_in_package" \
"(defpackage :cfpkg-b (:use :cl))
(progn (in-package :cfpkg-b))
(defvar cfpkg-marker-b 2)" \
'(and (find-symbol "CFPKG-MARKER-B" :cfpkg-b) t)'

# 3. EVAL-WHEN-wrapped IN-PACKAGE still switches the reader package.
run_case "eval_when_wrapped_in_package" \
"(defpackage :cfpkg-c (:use :cl))
(eval-when (:compile-toplevel :load-toplevel :execute) (in-package :cfpkg-c))
(defvar cfpkg-marker-c 3)" \
'(and (find-symbol "CFPKG-MARKER-C" :cfpkg-c) t)'

# 4. A non-IN-PACKAGE *PACKAGE* change during compile-time evaluation does NOT
#    leak into the reader package: the later DEFVAR still interns in the file's
#    IN-PACKAGE package, not the churned one.  (This is the esrap bug shape.)
run_case "package_churn_does_not_leak" \
"(defpackage :cfpkg-d (:use :cl))
(defpackage :cfpkg-other (:use :cl))
(in-package :cfpkg-d)
(eval-when (:compile-toplevel :load-toplevel :execute)
  (setf *package* (find-package :cfpkg-other)))
(defvar cfpkg-marker-d 4)" \
'(and (find-symbol "CFPKG-MARKER-D" :cfpkg-d)
      (null (find-symbol "CFPKG-MARKER-D" :cfpkg-other)))'

echo ""
echo "test_compile_file_package: $passed passed, $failed failed"
[ "$failed" -eq 0 ]
