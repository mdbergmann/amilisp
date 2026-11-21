#!/bin/sh
# (ext:%heap-census): compact, then say what the live heap consists of --
# objects and bytes by heap type, structures and instances by type name,
# closures by function, the largest general vectors and the fullest hash
# tables.
#
# Why: ROOM says how much of the arena is in use, not of what.  An image
# that runs for days and whose use only rises (the eta-hab finding of
# 2026-11-21) needs the answer that names the accumulating kind; this is
# the probe, and two reports some hours apart are the diagnosis.  What is
# checked: the sections are there, objects the script keeps are counted
# under their own name with the exact count, that count is gone once they
# are released, TOP bounds the listings, and a bad TOP is a type error.
# Under AMILISP_GC_STRESS=1 (`make test-gc-stress`) every allocation
# compacts, so the kept set shrinks: what is checked there is that the
# census's own collection and its report string survive that.
# Run: sh tests/test_heap_census.sh [path-to-amilisp]

AMILISP="${1:-build/host/amilisp}"
passed=0
failed=0
total=0

if [ "${AMILISP_GC_STRESS:-0}" = 1 ]; then
    N_STRUCTS=7; N_CLOSURES=5; N_VEC=600
else
    N_STRUCTS=1234; N_CLOSURES=321; N_VEC=54321
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

WORK="${TMPDIR:-/tmp}/heap_census_$$"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

cat > "$WORK/hc.lisp" <<EOF
(defparameter *n-structs* $N_STRUCTS)
(defparameter *n-closures* $N_CLOSURES)
(defparameter *n-vec* $N_VEC)
EOF
cat >> "$WORK/hc.lisp" <<'EOF'
(defstruct hc-point x y)
(defun hc-closure-maker (n) (lambda (x) (+ x n)))
(defvar *keep* nil)
(dotimes (i *n-structs*) (push (make-hc-point :x i) *keep*))
(dotimes (i *n-closures*) (push (hc-closure-maker i) *keep*))
(defvar *vec* (make-array *n-vec* :initial-element nil))
(defvar *table* (make-hash-table :test 'equal))
(dotimes (i 77) (setf (gethash (format nil "k~A" i) *table*) i))
(defun count-lines-between (report from to)
  "Lines of REPORT after the line starting with FROM, up to the one starting with TO."
  (let* ((a (search from report))
         (b (and a (search to report :start2 a))))
    (if (and a b)
        (1- (count #\Newline report :start a :end b))
        -1)))
(let ((r (ext:%heap-census)))
  (format t "~&HC-KEPT~%~A~&HC-KEPT-END~%" r))
(let ((r (ext:%heap-census 1)))
  (format t "~&HC-TOP1 structs=~A closures=~A~%"
          (count-lines-between r "structures and instances" (format nil "~%closures by function"))
          (count-lines-between r "closures by function" (format nil "~%largest general vectors"))))
(let ((r (ext:%heap-census 100000)))
  (format t "~&HC-CLAMPED ~A~%" (and (search "(top 100)" r) t)))
(setf *keep* nil)
(gc)
;; one instance may still sit in a register or a values slot: what must be
;; gone is the kept set, i.e. the line with its full count
(format t "~&HC-RELEASED ~A~%"
        (and (search (format nil "~9D  HC-POINT" *n-structs*) (ext:%heap-census)) t))
(dolist (bad (list 0 -3 "5" 2.0))
  (format t "~&HC-BAD ~S ~A~%" bad
          (handler-case (progn (ext:%heap-census bad) :no-error)
            (type-error () :type-error)
            (error () :other-error))))
(format t "~&HC-DONE~%")
EOF

for g in 1 0; do
    out=$(AMILISP_GENGC=$g "$AMILISP" --no-userinit --non-interactive --heap 8M \
          --load "$WORK/hc.lisp" </dev/null 2>&1)
    check_contains "gengc=$g: the script ran to the end" "HC-DONE" "$out"
    check_contains "gengc=$g: the report opens with the totals" "heap-census: " "$out"
    check_contains "gengc=$g: nothing dead after the compaction" " 0 dead blocks, 0 bytes" "$out"
    check_contains "gengc=$g: the by-type section" "by type:" "$out"
    check_contains "gengc=$g: conses are counted" "  CONS" "$out"
    check_contains "gengc=$g: uninterned symbols are counted" "symbols without a home package: " "$out"
    check_contains "gengc=$g: the kept structures, by name and count" "$(printf '%9s  HC-POINT' "$N_STRUCTS")" "$out"
    check_contains "gengc=$g: the kept closures, by function and count" "$(printf '%9s  (lambda)' "$N_CLOSURES")" "$out"
    check_contains "gengc=$g: the kept vector, by length" "$(printf '  %9s ' "$N_VEC")" "$out"
    check_contains "gengc=$g: the kept table, by entries and test" "       77 " "$out"
    check_contains "gengc=$g: the table's test is named" "EQUAL  @0x" "$out"
    check_contains "gengc=$g: TOP 1 lists one line per section" "HC-TOP1 structs=1 closures=1" "$out"
    check_contains "gengc=$g: TOP above 100 is clamped" "HC-CLAMPED T" "$out"
    check_contains "gengc=$g: released structures leave the report" "HC-RELEASED NIL" "$out"
    check_contains "gengc=$g: TOP 0 is a type error" "HC-BAD 0 TYPE-ERROR" "$out"
    check_contains "gengc=$g: a negative TOP is a type error" "HC-BAD -3 TYPE-ERROR" "$out"
    check_contains "gengc=$g: a string TOP is a type error" 'HC-BAD "5" TYPE-ERROR' "$out"
    check_contains "gengc=$g: a float TOP is a type error" "HC-BAD 2.0 TYPE-ERROR" "$out"
done

echo "test_heap_census: $passed/$total passed"
[ "$failed" -eq 0 ]
