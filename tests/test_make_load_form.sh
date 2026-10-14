#!/bin/sh
# End-to-end MAKE-LOAD-FORM round-trip tests (CLHS 7.6 / 3.2.4.4).
#
# A literal object reachable from compiled code whose class has a user
# MAKE-LOAD-FORM method must be serialized as FASL_TAG_LOAD_FORM (creation
# form + init form) and reconstructed at load time by evaluating those
# forms — NOT dumped slot-for-slot.  See src/core/fasl.c (the MLF pre-pass
# and the TYPE_STRUCT / FASL_TAG_LOAD_FORM cases) and lib/clos.lisp
# (MAKE-LOAD-FORM / MAKE-LOAD-FORM-SAVING-SLOTS / %ALLOCATE-FOR-LOAD).
#
# Each case compiles a source file to a .fasl, then loads that .fasl in a
# FRESH amilisp process so the reconstruction goes through real on-disk
# FASL serialization + deserialization (not in-memory bytecode reuse).
#
# Run: sh tests/test_make_load_form.sh [path-to-amilisp]

CLAMIGA="${1:-build/host/amilisp}"
passed=0
failed=0
TMP="${TMPDIR:-/tmp}/clamiga-mlf-$$"
mkdir -p "$TMP"
cleanup() { rm -rf "$TMP" "$HOME/.cache/common-lisp/cl-amiga-"*"$TMP"* 2>/dev/null; }
trap cleanup EXIT

# run_case DESC SRC CHECK
#   SRC   — source compiled to FASL (defines class+method+literal)
#   CHECK — a Lisp form evaluated AFTER loading the FASL; must be T
run_case() {
    desc="$1"
    src="$2"
    check="$3"
    srcfile="$TMP/$desc.lisp"
    faslfile="$TMP/$desc.fasl"
    printf '%s\n' "$src" > "$srcfile"

    # Process 1: compile the source to a FASL.
    comp_driver="$TMP/$desc-comp.lisp"
    cat > "$comp_driver" <<EOF
(handler-case
    (progn (compile-file "$srcfile" :output-file "$faslfile")
           (format t "~&COMPILED=ok~%"))
  (error (e) (format t "~&COMPILED=error ~a~%" e)))
EOF
    cout=$(CLAMIGA_NO_USERINIT=1 "$CLAMIGA" --no-userinit --non-interactive \
               --load "$comp_driver" </dev/null 2>&1)
    if ! printf '%s\n' "$cout" | grep -q "COMPILED=ok"; then
        echo "  FAIL  $desc (compile failed)"
        printf '%s\n' "$cout" | grep -iE "error|COMPILED" | head -5
        failed=$((failed + 1))
        return
    fi

    # Process 2 (fresh heap): load the FASL and run CHECK.
    load_driver="$TMP/$desc-load.lisp"
    cat > "$load_driver" <<EOF
(handler-case
    (progn (load "$faslfile")
           (format t "~&RESULT=~a~%" (if $check :pass :fail)))
  (error (e) (format t "~&RESULT=error ~a~%" e)))
EOF
    out=$(CLAMIGA_NO_USERINIT=1 "$CLAMIGA" --no-userinit --non-interactive \
              --load "$load_driver" </dev/null 2>&1)
    res=$(printf '%s\n' "$out" | sed -n 's/^RESULT=//p' | head -1)
    if [ "$res" = ":PASS" ] || [ "$res" = "PASS" ]; then
        echo "  ok  $desc"
        passed=$((passed + 1))
    else
        echo "  FAIL  $desc (RESULT=$res)"
        printf '%s\n' "$out" | grep -iE "error|RESULT|Undefined" | head -5
        failed=$((failed + 1))
    fi
}

# 1. CLOS instance: slot values survive the round-trip, and the
#    self-referential slot reconstructs as EQ to the object itself
#    (exercises the creation/init split and the OBJ_REF self-loop).
run_case "clos_saving_slots" \
'(defpackage :mlf1 (:use :cl))
(in-package :mlf1)
(defclass widget ()
  ((label :initarg :label :accessor widget-label)
   (cnt   :initarg :cnt   :accessor widget-cnt)
   (self  :accessor widget-self)))
(defmethod make-load-form ((w widget) &optional env)
  (declare (ignore env))
  (make-load-form-saving-slots w))
(defvar *w*
  #.(let ((w (make-instance (quote widget) :label "hi" :cnt 7)))
      (setf (widget-self w) w)
      w))' \
'(let ((w (symbol-value (find-symbol "*W*" "MLF1"))))
   (and (string= (funcall (find-symbol "WIDGET-LABEL" "MLF1") w) "hi")
        (= (funcall (find-symbol "WIDGET-CNT" "MLF1") w) 7)
        (eq w (funcall (find-symbol "WIDGET-SELF" "MLF1") w))))'

# 2. Custom MAKE-LOAD-FORM returning explicit creation+init forms (not
#    saving-slots): the creation form runs a constructor, the init form
#    sets a slot.
run_case "clos_custom_forms" \
'(defpackage :mlf2 (:use :cl))
(in-package :mlf2)
(defclass box ()
  ((v :initarg :v :accessor box-v)))
(defun rebuild-box (v) (make-instance (quote box) :v v))
(defmethod make-load-form ((b box) &optional env)
  (declare (ignore env))
  (values (list (quote rebuild-box) (list (quote quote) (box-v b)))
          nil))
(defvar *b* #.(make-instance (quote box) :v 42))' \
'(= (funcall (find-symbol "BOX-V" "MLF2") (symbol-value (find-symbol "*B*" "MLF2"))) 42)'

# 3. Object nested inside another literal (a list constant): the inner
#    instance still gets MAKE-LOAD-FORM treatment via the graph walk.
run_case "nested_in_list" \
'(defpackage :mlf3 (:use :cl))
(in-package :mlf3)
(defclass pt () ((x :initarg :x :accessor pt-x)))
(defmethod make-load-form ((p pt) &optional env)
  (declare (ignore env))
  (make-load-form-saving-slots p))
(defvar *lst* (quote #.(list 1 (make-instance (quote pt) :x 9) 3)))' \
'(let ((lst (symbol-value (find-symbol "*LST*" "MLF3"))))
   (and (= (first lst) 1)
        (= (funcall (find-symbol "PT-X" "MLF3") (second lst)) 9)
        (= (third lst) 3)))'

# 4. Large constant graph (thousands of unique conses) with a shared
#    substructure and an MLF instance mixed in — regression for the
#    pre-pass seen-set hash index (2026-07-05; formerly an O(n²) linear
#    scan).  Verifies the walk still terminates correctly on a big
#    graph and the nested instance is reconstructed via its load form.
#    (EQ identity of the shared tail is NOT asserted — the writer does
#    not preserve cons-level sharing today, verified pre-existing.)
run_case "large_graph_hash_index" \
'(defpackage :mlf4 (:use :cl))
(in-package :mlf4)
(defclass node () ((tag :initarg :tag :accessor node-tag)))
(defmethod make-load-form ((n node) &optional env)
  (declare (ignore env))
  (make-load-form-saving-slots n))
(defvar *big*
  (quote #.(let ((shared (list :s 1 2 3)) (acc nil))
             (dotimes (i 3000) (push (cons i shared) acc))
             (push (make-instance (quote node) :tag 77) acc)
             acc)))' \
'(let* ((big (symbol-value (find-symbol "*BIG*" "MLF4")))
        (node (first big))
        (pairs (rest big)))
   (and (= (length pairs) 3000)
        (= (funcall (find-symbol "NODE-TAG" "MLF4") node) 77)
        (equal (cdr (first pairs)) (list :s 1 2 3))
        (equal (cdr (first (last pairs))) (list :s 1 2 3))
        (= (car (first pairs)) 2999)
        (= (car (first (last pairs))) 0)))'

echo ""
echo "test_make_load_form: $passed passed, $failed failed"
[ "$failed" -eq 0 ]
