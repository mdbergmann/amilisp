#!/bin/sh
# (ext:%heap-verify): collect, then check every live object's header and
# child references, the registered static roots and the thread stacks
# against the real object starts.  A healthy heap answers 0 under both
# collectors, before and after a heavy load (compiling a library, which
# fragments and compacts), with a report that ends in the block count.
#
# Why: a corrupted heap surfaces as an unrelated error long after the
# fact, and on the Amiga stderr is never seen -- this is the probe a
# script can call after each LOAD to find where a corruption entered
# (the layout-dependent Amiga-written-FASL finding of 2026-09-18).
# Under CLAMIGA_GC_STRESS=1 (`make test-gc-stress`, the DEBUG_GC_STRESS build)
# every allocation compacts, so the churn shrinks to a few objects of each
# kind %heap-verify walks: what is checked there is that the verifier's own
# allocations (its collection, its report string) survive a compaction on
# every allocation, not the volume of heap it walks.
# Run: sh tests/test_heap_verify.sh [path-to-amilisp]

CLAMIGA="${1:-build/host/amilisp}"
passed=0
failed=0
total=0

if [ "${CLAMIGA_GC_STRESS:-0}" = 1 ]; then
    N_LISTS=8; LIST_LEN=20; N_TABLES=3; N_STRUCTS=10; N_CLOSURES=5
else
    N_LISTS=200; LIST_LEN=500; N_TABLES=50; N_STRUCTS=300; N_CLOSURES=100
fi

check_contains() {
    desc="$1"; needle="$2"; haystack="$3"
    total=$((total + 1))
    case "$haystack" in
        *"$needle"*) echo "  ok  $desc"; passed=$((passed + 1)) ;;
        *) echo "  FAIL  $desc"; echo "    expected to contain: $needle"
           echo "    got: $(echo "$haystack" | head -8)"; failed=$((failed + 1)) ;;
    esac
}

WORK="${TMPDIR:-/tmp}/heap_verify_$$"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

cat > "$WORK/hv.lisp" <<EOF
(defparameter *n-lists* $N_LISTS)
(defparameter *list-len* $LIST_LEN)
(defparameter *n-tables* $N_TABLES)
(defparameter *n-structs* $N_STRUCTS)
(defparameter *n-closures* $N_CLOSURES)
EOF
cat >> "$WORK/hv.lisp" <<'EOF'
(defun hv (tag)
  (multiple-value-bind (n r) (ext:%heap-verify)
    (format t "~&HV ~A: ~A~%~A" tag n r)))
(hv "boot")
;; churn: allocate, drop, keep, compact
(defvar *keep* nil)
(dotimes (i *n-lists*)
  (let ((junk (make-list *list-len* :initial-element i)))
    (when (zerop (mod i 7)) (push (copy-list junk) *keep*))))
(dotimes (i *n-tables*) (push (make-hash-table :test 'equal) *keep*))
(dolist (h *keep*) (when (hash-table-p h) (setf (gethash "k" h) (list 1 2 3))))
(defstruct hv-point x y)
(dotimes (i *n-structs*) (push (make-hv-point :x i :y (format nil "p~A" i)) *keep*))
(defun hv-closure-maker (n) (lambda (x) (+ x n)))
(dotimes (i *n-closures*) (push (hv-closure-maker i) *keep*))
(gc)
(hv "after churn")
(setf *keep* nil)
(gc)
(hv "after release")
(format t "~&HV-DONE~%")
EOF

for g in 1 0; do
    out=$(CLAMIGA_GENGC=$g "$CLAMIGA" --no-userinit --non-interactive --heap 8M \
          --load "$WORK/hv.lisp" </dev/null 2>&1)
    check_contains "gengc=$g: the script ran to the end" "HV-DONE" "$out"
    check_contains "gengc=$g: a fresh heap is clean" "HV boot: 0" "$out"
    check_contains "gengc=$g: clean after churn" "HV after churn: 0" "$out"
    check_contains "gengc=$g: clean after release" "HV after release: 0" "$out"
    check_contains "gengc=$g: the report ends with the block count" "fault(s)" "$out"
    check_contains "gengc=$g: nothing reported as a fault" "0 fault(s)" "$out"
done

echo "test_heap_verify: $passed/$total passed"
[ "$failed" -eq 0 ]
