#!/bin/sh
# Regression test: a boot-time SOURCE compile of lib/boot.lisp / lib/clos.lisp
# must produce the same CLOS runtime as the shipped FASLs.
#
# Bug (real-Amiga finding 2026-08-09): boot.lisp/clos.lisp are read in
# COMMON-LISP when source-loaded at boot, but in COMMON-LISP-USER when
# `make fasl` compiles them.  A %-helper missing from clos_internal_names[]
# (src/core/package.c) therefore interned as a different symbol on each path,
# and any cross-path reference — the shipped boot.fasl calling a helper that
# a source-compiled clos.lisp just defined — silently landed on the unbound
# twin.  On the real Amiga (deploy stamped clos.lisp newer than clos.fasl)
# this lost the whole dispatch self-heal chain, the reader/writer-IC
# discriminators, and defstruct's %REGISTER-STRUCT-CLASS MOP hookup — while
# the load itself printed nothing.
#
# Checks, per boot mode (mixed = boot.fasl + clos.lisp source, the Amiga
# case; full-source = both .lisp newer):
#   1. the once-missing dispatch/IC/MOP helpers are fbound
#   2. defstruct produces a class whose MOP class-slots are populated
#   3. CLOS dispatch works end-to-end (defclass + defmethod + call)
#   4. the set of %-helper names visible from CL-USER matches the
#      fasl-boot set exactly — catches ANY future helper added to
#      boot.lisp/clos.lisp but forgotten in clos_internal_names[]
#
# Run: sh tests/test_boot_source_compile.sh build/host/amilisp

AMILISP="${1:-build/host/amilisp}"
case "$AMILISP" in /*) ;; *) AMILISP="$(pwd)/$AMILISP" ;; esac
passed=0
failed=0
total=0
TMPD=$(mktemp -d "${TMPDIR:-/tmp}/test_boot_srcc_XXXXXX") || exit 1
trap 'rm -rf "$TMPD"' EXIT INT TERM

REPO=$(cd "$(dirname "$0")/.." && pwd)
cp -R "$REPO/lib" "$TMPD/lib" || exit 1
mkdir -p "$TMPD/cache"

cat > "$TMPD/probe.lisp" <<'EOF'
(setf *load-verbose* nil)
(format t "HELPERS=~a~%"
        (and (fboundp '%recompute-methods-until)
             (fboundp '%gf-roster-has-primary-p)
             (fboundp '%dispatch-standard-emf)
             (fboundp '%dispatch-heal-empty)
             (fboundp '%dispatch-negative-hit)
             (fboundp '%note-reader-method)
             (fboundp '%note-writer-method)
             (fboundp '%register-struct-class)
             (fboundp '%slot-value-slow)
             (fboundp '%metaclass-p)
             t))
(defstruct bsc-probe-struct a b)
(format t "STRUCT-SLOTS=~a~%" (length (class-slots (find-class 'bsc-probe-struct))))
(defclass bsc-probe-class () ((x :initarg :x :accessor bsc-probe-x)))
(defmethod bsc-probe-gf ((o bsc-probe-class)) (* 2 (bsc-probe-x o)))
(format t "DISPATCH=~a~%" (bsc-probe-gf (make-instance 'bsc-probe-class :x 21)))
;; All %-helper names visible from CL-USER (own + inherited), sorted —
;; must be identical across boot modes.
(let ((names '()))
  (do-symbols (s (find-package :cl-user))
    (let ((n (symbol-name s)))
      (when (and (> (length n) 0) (char= (char n 0) #\%) (fboundp s))
        (push n names))))
  (setq names (sort (remove-duplicates names :test #'string=) #'string<))
  (format t "SET-COUNT=~a~%" (length names))
  (dolist (n names) (format t "SYM ~a~%" n)))
(quit)
EOF

run_boot() {
    # $1 = output file.  Boot from $TMPD (cwd lib/ wins the lib search),
    # isolated FASL cache, 8M heap like the Amiga suite.
    ( cd "$TMPD" && AMILISP_FASL_CACHE_DIR="$TMPD/cache" \
      "$AMILISP" --no-userinit --heap 8M --non-interactive --load probe.lisp \
      </dev/null 2>&1 ) > "$1"
}

check() {
    desc="$1"; pattern="$2"; file="$3"
    total=$((total + 1))
    if grep -q "$pattern" "$file"; then
        echo "  ok  $desc"
        passed=$((passed + 1))
    else
        echo "  FAIL  $desc (missing: $pattern)"
        grep -v "^SYM " "$file" | head -8 | sed 's/^/    /'
        failed=$((failed + 1))
    fi
}

# --- Reference: pure FASL boot (shipped fasls newest) ---
touch "$TMPD/lib/boot.fasl" "$TMPD/lib/clos.fasl"
run_boot "$TMPD/out-fasl.txt"
check "fasl-boot helpers fbound"      "HELPERS=T"      "$TMPD/out-fasl.txt"
check "fasl-boot struct class-slots"  "STRUCT-SLOTS=2" "$TMPD/out-fasl.txt"
check "fasl-boot dispatch works"      "DISPATCH=42"    "$TMPD/out-fasl.txt"

# --- Mixed boot: boot.fasl + clos.lisp SOURCE (the real-Amiga case) ---
rm -rf "$TMPD/cache"; mkdir -p "$TMPD/cache"
sleep 1
touch "$TMPD/lib/clos.lisp"
run_boot "$TMPD/out-mixed.txt"
check "mixed-boot helpers fbound"     "HELPERS=T"      "$TMPD/out-mixed.txt"
check "mixed-boot struct class-slots" "STRUCT-SLOTS=2" "$TMPD/out-mixed.txt"
check "mixed-boot dispatch works"     "DISPATCH=42"    "$TMPD/out-mixed.txt"

# --- Full-source boot: boot.lisp AND clos.lisp newest ---
rm -rf "$TMPD/cache"; mkdir -p "$TMPD/cache"
touch "$TMPD/lib/boot.lisp" "$TMPD/lib/clos.lisp"
run_boot "$TMPD/out-src.txt"
check "source-boot helpers fbound"     "HELPERS=T"      "$TMPD/out-src.txt"
check "source-boot struct class-slots" "STRUCT-SLOTS=2" "$TMPD/out-src.txt"
check "source-boot dispatch works"     "DISPATCH=42"    "$TMPD/out-src.txt"

# --- %-helper visibility sets must be identical across all three boots ---
for mode in mixed src; do
    total=$((total + 1))
    grep "^SYM " "$TMPD/out-fasl.txt" > "$TMPD/set-fasl.txt"
    grep "^SYM " "$TMPD/out-$mode.txt" > "$TMPD/set-$mode.txt"
    if [ -s "$TMPD/set-fasl.txt" ] && diff "$TMPD/set-fasl.txt" "$TMPD/set-$mode.txt" > "$TMPD/set-$mode.diff" 2>&1; then
        echo "  ok  $mode-boot %-helper set matches fasl boot"
        passed=$((passed + 1))
    else
        echo "  FAIL  $mode-boot %-helper set diverges from fasl boot"
        echo "    (a helper defined in boot.lisp/clos.lisp is probably missing"
        echo "     from clos_internal_names[] in src/core/package.c)"
        head -10 "$TMPD/set-$mode.diff" | sed 's/^/    /'
        failed=$((failed + 1))
    fi
done

echo ""
echo "$passed passed, $failed failed, $total total"
if [ "$failed" -gt 0 ]; then
    echo "FAIL"
    exit 1
fi
echo "PASS"
exit 0
