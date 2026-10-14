#!/bin/sh
# The call generation word from Lisp: (amilisp::%call-gen) must move across
# every event that could leave a m68k JIT call site with a stale cached
# callee (specs/jit-direct-calls.md §1) -- each public way of changing a
# function cell (DEFUN from source and from a FASL, (SETF FDEFINITION),
# (SETF SYMBOL-FUNCTION), FMAKUNBOUND, a SETF function), every collection,
# TRACE/UNTRACE, and the thread events a looping caller must notice
# (MP:INTERRUPT-THREAD, MP:DESTROY-THREAD) -- and must NOT move on a plain
# call, since nothing on the per-call path may write shared memory.
# tests/test_call_gen.c pins the exact counts at the C level.
#
# Also run by `make test-gc-stress` (AMILISP_GC_STRESS=1: compaction on
# every allocation, so every allocating form bumps -- the checks below
# assert "moved", which a stress run must still satisfy, and the no-bump
# check uses a call loop that does not allocate).
# Run: sh tests/test_call_gen.sh [path-to-amilisp]

AMILISP="${1:-build/host/amilisp}"
passed=0
failed=0
total=0

check_contains() {
    desc="$1"
    needle="$2"
    haystack="$3"
    total=$((total + 1))
    case "$haystack" in
        *"$needle"*)
            echo "  ok  $desc"
            passed=$((passed + 1)) ;;
        *)
            echo "  FAIL  $desc"
            echo "    expected to contain: $needle"
            echo "    got: $(echo "$haystack" | head -12)"
            failed=$((failed + 1)) ;;
    esac
}

WORK="${TMPDIR:-/tmp}/call_gen_$$"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

cat > "$WORK/defs.lisp" <<'EOF'
(defun call-gen-from-fasl () :fasl)
EOF

cat > "$WORK/gen.lisp" <<EOF
(defmacro moves (tag form)
  \`(let ((g (amilisp::%call-gen)))
     ,form
     (format t "~A ~A~%" ,tag (if (/= g (amilisp::%call-gen)) "MOVED" "STILL"))))
(format t "NONZERO ~A~%" (if (plusp (amilisp::%call-gen)) "YES" "NO"))
(moves "DEFUN" (defun cg-f () 1))
(moves "REDEFUN" (defun cg-f () 2))
(moves "SETF-FDEFINITION" (setf (fdefinition 'cg-f) (lambda () 3)))
(moves "SETF-SYMBOL-FUNCTION" (setf (symbol-function 'cg-f) (lambda () 4)))
(format t "VALUE ~A~%" (cg-f))
(moves "SETF-FUNCTION" (defun (setf cg-place) (v x) (list v x)))
(moves "FMAKUNBOUND" (fmakunbound 'cg-f))
(moves "FMAKUNBOUND-SETF" (fmakunbound '(setf cg-place)))
(format t "UNBOUND ~A~%" (list (fboundp 'cg-f) (fboundp '(setf cg-place))))
(compile-file "$WORK/defs.lisp" :output-file "$WORK/defs.fasl")
(moves "FASL-DEFUN" (load "$WORK/defs.fasl"))
(format t "FASL-VALUE ~A~%" (call-gen-from-fasl))
(defun cg-traced () 5)
(moves "TRACE" (trace cg-traced))
(moves "UNTRACE" (untrace cg-traced))
(moves "GC" (gc))
(let ((g (amilisp::%call-gen)))
  (dotimes (i 5) (gc))
  (format t "GC-EACH ~A~%" (if (>= (- (amilisp::%call-gen) g) 5) "YES" "NO")))
(defun cg-leaf (x) (+ x 1))
(defun cg-loop (n) (let ((s 0)) (dotimes (i n s) (setq s (cg-leaf s)))))
(cg-loop 10)
(let ((g (amilisp::%call-gen)))
  (cg-loop 10000)
  (format t "CALLS ~A~%" (if (= g (amilisp::%call-gen)) "STILL" "MOVED")))
;; Thread events: a caller looping on direct calls must miss and poll.
(let* ((stop nil)
       (th (mp:make-thread (lambda () (loop until stop do (mp:thread-yield)))
                           :name "cg-worker")))
  (sleep 0.05)
  (moves "INTERRUPT" (mp:interrupt-thread th (lambda () nil)))
  (moves "DESTROY" (mp:destroy-thread th))
  (setf stop t)
  (ignore-errors (mp:join-thread th)))
(let ((st (amilisp::%jit-direct-call-stats)))
  ;; Without a JIT backend there are no direct-call sites: every counter
  ;; 0, the sites never enabled.  The AArch64 backend has them (on by
  ;; default; the counters are the calling thread's integers).
  (format t "STATS-KEYS ~S~%" (loop for (k v) on st by #'cddr collect k))
  (format t "HAS-DIRECT ~A~%" (if (getf st :enabled) "YES" "NO"))
  (format t "STATS-INTEGERS ~A~%"
          (if (every (lambda (k) (typep (getf st k) '(integer 0)))
                     '(:fills :misses :refused-trace :refused-shadow
                       :refused-abi :refused-not-native))
              "YES" "NO"))
  (format t "STATS-ZERO ~A~%"
          (if (every #'zerop (list (getf st :fills) (getf st :misses)
                                   (getf st :refused-trace) (getf st :refused-shadow)
                                   (getf st :refused-abi) (getf st :refused-not-native)))
              "YES" "NO"))
  ;; Not EQL: under GC stress the plist's own conses collect, and every
  ;; collection bumps the generation past the one it recorded.
  (format t "STATS-GEN ~A~%"
          (let ((g (getf st :gen)))
            (if (and (integerp g) (plusp g) (<= g (amilisp::%call-gen))) "YES" "NO"))))
(format t "DIRECT ~S~%" (list (amilisp::%jit-set-direct-calls t)
                              (amilisp::%jit-set-direct-calls nil)))
(format t "GEN-DONE~%")
EOF

out=$("$AMILISP" --no-userinit --non-interactive --load "$WORK/gen.lisp" </dev/null 2>&1)

check_contains "the script ran to the end" "GEN-DONE" "$out"
check_contains "the word starts non-zero (0 = an empty site)" "NONZERO YES" "$out"
check_contains "DEFUN bumps" "DEFUN MOVED" "$out"
check_contains "redefinition bumps" "REDEFUN MOVED" "$out"
check_contains "(SETF FDEFINITION) bumps" "SETF-FDEFINITION MOVED" "$out"
check_contains "(SETF SYMBOL-FUNCTION) bumps" "SETF-SYMBOL-FUNCTION MOVED" "$out"
check_contains "the last write is the one called" "VALUE 4" "$out"
check_contains "a SETF function's definition bumps" "SETF-FUNCTION MOVED" "$out"
check_contains "FMAKUNBOUND bumps" "FMAKUNBOUND MOVED" "$out"
check_contains "FMAKUNBOUND of (SETF name) bumps" "FMAKUNBOUND-SETF MOVED" "$out"
check_contains "both are unbound afterwards" "UNBOUND (NIL NIL)" "$out"
check_contains "a DEFUN loaded from a FASL bumps" "FASL-DEFUN MOVED" "$out"
check_contains "the FASL's definition is called" "FASL-VALUE FASL" "$out"
check_contains "TRACE bumps" "TRACE MOVED" "$out"
check_contains "UNTRACE bumps" "UNTRACE MOVED" "$out"
check_contains "(GC) bumps" "GC MOVED" "$out"
check_contains "every collection bumps, not only the first" "GC-EACH YES" "$out"
check_contains "calling a function does not bump" "CALLS STILL" "$out"
check_contains "MP:INTERRUPT-THREAD bumps" "INTERRUPT MOVED" "$out"
check_contains "MP:DESTROY-THREAD bumps" "DESTROY MOVED" "$out"
check_contains "%JIT-DIRECT-CALL-STATS is a plist of every counter" \
    "STATS-KEYS (:FILLS :MISSES :REFUSED-TRACE :REFUSED-SHADOW :REFUSED-ABI :REFUSED-NOT-NATIVE :GEN :ENABLED)" "$out"
check_contains ":GEN is a call generation, not ahead of the current one" "STATS-GEN YES" "$out"
check_contains "every counter is a non-negative integer" "STATS-INTEGERS YES" "$out"
case "$out" in
    *"HAS-DIRECT YES"*)
        check_contains "direct calls (AArch64): %JIT-SET-DIRECT-CALLS switches them" \
            "DIRECT (T NIL)" "$out" ;;
    *)
        check_contains "no direct-call sites on host: every counter is 0" "STATS-ZERO YES" "$out"
        check_contains "no direct-call sites on host: %JIT-SET-DIRECT-CALLS stays NIL" \
            "DIRECT (NIL NIL)" "$out" ;;
esac

echo "$passed passed, $failed failed, $total total"
[ "$failed" -eq 0 ]
