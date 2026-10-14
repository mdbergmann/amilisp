#!/bin/sh
# The AArch64 walker (specs/native-backend-a64.md): functions run as native
# code built from the interpreter's own helpers (phase 1) plus inline fast
# paths and a top-of-stack register cache (phase 2), so every result must be
# the interpreter's.  Five parts:
#
#   1. tests/amiga/test-jit.lisp -- the m68k JIT suite's behavioural checks
#      -- run against this backend, eager (the file sets the hot threshold
#      to 0 itself).  Its #+m68k checks are the m68k backend's own.
#   2. Checks of this backend: the shapes it compiles, results across GC
#      and errors, deep and self-tail recursion, redefinition, the
#      interpreter's multiple-value state, and a call-free native loop in
#      one thread while another collects (the loop poll, spec "Rule 4" --
#      without it the collection waits forever, so the run is bounded).
#      Phase 2: every inline fast path against its slow path -- fixnum
#      boundaries, other number types, type errors -- with a heap value
#      held in the register cache while the slow path runs, which under
#      AMILISP_GC_STRESS allocates and moves it.
#   3. Phase 3: the NLX frames, dynamic binding, multiple values, closures
#      and &key functions, each against the interpreter's results (also
#      under the classic collector), and native frames in the backtrace and
#      FRAME-LOCALS.
#   4. Direct native-to-native calls, against the same script with direct
#      calls off.
#   5. &optional and &rest: the inline &optional prologue, the &rest/&key
#      helper, self tail calls that change the count, direct calls into
#      such callees -- against the interpreter, direct calls off and the
#      classic collector.
#
# A build without the backend (x86-64, Windows, `make host JIT=0`)
# skips them all.  Also run by `make test-gc-stress` (AMILISP_GC_STRESS=1).
# Run: sh tests/test_jit_a64_walk.sh [path-to-amilisp]

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
            echo "    got: $(echo "$haystack" | tail -20)"
            failed=$((failed + 1)) ;;
    esac
}

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)

WORK="${TMPDIR:-/tmp}/jit_a64_walk_$$"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

backend=$("$AMILISP" --no-userinit --non-interactive \
    --eval '(defun probe-fn () 1)' \
    --eval '(format t "BACKEND ~A~%" (if (amilisp::%jit-compile-stub (function probe-fn)) "A64" "NONE"))' \
    </dev/null 2>&1)
case "$backend" in
*"BACKEND A64"*) ;;
*)
    echo "  (no AArch64 backend in this build: walker checks skipped)"
    check_contains "%JIT-COMPILE-STUB is NIL without a backend" "BACKEND NONE" "$backend"
    echo "$passed passed, $failed failed, $total total"
    [ "$failed" -eq 0 ]
    exit $? ;;
esac

# 32M: four threads consing at once exhaust the 4M default heap, and a
# thread that does dies (MP:*THREAD-DEATH-HOOKS*) instead of signalling.
run() {
    if [ -n "$TIMEOUT" ]; then
        "$TIMEOUT" 300 "$AMILISP" --heap 32M --no-userinit --non-interactive --load "$1" </dev/null 2>&1
    else
        "$AMILISP" --heap 32M --no-userinit --non-interactive --load "$1" </dev/null 2>&1
    fi
}

# --- Part 1: the m68k suite's behavioural checks ------------------------------
cat > "$WORK/suite.lisp" <<'EOF'
(setq *pass-count* 0)
(setq *fail-count* 0)
(defmacro check (name expected actual)
  (let ((e (gensym)) (a (gensym)) (c (gensym)))
    `(handler-case
         (let ((,e ,expected) (,a ,actual))
           (if (equal ,e ,a)
               (setq *pass-count* (+ *pass-count* 1))
               (progn (setq *fail-count* (+ *fail-count* 1))
                      (format t "FAIL: ~A - expected ~S got ~S~%" ,name ,e ,a))))
       (error (,c)
         (setq *fail-count* (+ *fail-count* 1))
         (format t "FAIL: ~A - signaled error: ~A~%" ,name ,c)))))
(defmacro stress-check (name expected actual) `(check ,name ,expected ,actual))
(load "tests/amiga/test-jit.lisp")
(format t "~%END ~S~%" (and (boundp '*test-jit-loaded-to-end*) (symbol-value '*test-jit-loaded-to-end*)))
(format t "~%SUITE pass=~D fail=~D~%" *pass-count* *fail-count*)
EOF
out=$(run "$WORK/suite.lisp")
check_contains "test-jit.lisp ran" "SUITE pass=" "$out"
# A reader error (AMIGA.FFI on a host, 2026-10-02) ends the LOAD early and
# silently: everything after it never ran.  The file's last form says it got there.
check_contains "test-jit.lisp loaded to its last form" "END T" "$out"
check_contains "every behavioural check of test-jit.lisp passes" "fail=0" "$out"
# Its checks number in the hundreds: guard against the file running only a few.
pass_n=$(echo "$out" | sed -n 's/.*SUITE pass=\([0-9]*\).*/\1/p')
total=$((total + 1))
if [ -n "$pass_n" ] && [ "$pass_n" -ge 500 ]; then
    echo "  ok  test-jit.lisp ran $pass_n checks"; passed=$((passed + 1))
else
    echo "  FAIL  test-jit.lisp ran only '$pass_n' checks"; failed=$((failed + 1))
fi

# --- Part 2: this backend ------------------------------------------------------
cat > "$WORK/walk.lisp" <<'EOF'
(amilisp::%jit-set-hot-threshold 0)
;; Under AMILISP_GC_STRESS every allocation compacts: fewer conses give the
;; same coverage of the collections the consing loops run into.
(defparameter *churn-n* (if (ext:getenv "AMILISP_GC_STRESS") 300 50000))
(defmacro chk (name form)
  `(format t "~A ~S~%" ,name
           (handler-case ,form (error (c) (list :error (princ-to-string c))))))
(defun native-p (f) (not (null (amilisp::%jit-dump-bytes f))))

;; Shapes phase 1 compiles: stack and locals, constants, branches and
;; loops, calls, arithmetic, lists, structs, vectors, strings, globals.
(defvar *w-global* 0)
(defstruct wpt x y)
(defun w-add (a b) (+ a b))
(defun w-sum (n) (let ((s 0)) (dotimes (i n s) (setq s (+ s i)))))
(defun w-fact (n) (if (<= n 1) 1 (* n (w-fact (- n 1)))))
(defun w-count (n acc) (if (= n 0) acc (w-count (- n 1) (+ acc 1))))
(defun w-deep (n) (if (= n 0) 0 (+ 1 (w-deep (- n 1)))))
(defun w-lists (a b c) (list a b c (cons a b) (car (list c)) (cdr (list a b)) (eq a a) (not b)))
(defun w-chars (s) (let ((n 0)) (dotimes (i (length s) n) (when (char= (char s i) #\a) (setq n (+ n 1))))))
(defun w-push (lst) (let ((acc nil)) (dolist (x lst acc) (push (* x x) acc))))
(defun w-pop (lst) (let ((acc 0)) (loop (when (null lst) (return acc)) (setq acc (+ acc (pop lst))))))
(defun w-struct (p) (setf (wpt-y p) (+ (wpt-x p) 1)) (wpt-y p))
(defun w-vec (v) (setf (aref v 1) 'x) (list (svref v 0) (aref v 1)))
(defun w-apply (f args) (apply f args))
(defun w-funcall (f x) (funcall f x))
(defun w-global () (setq *w-global* (+ *w-global* 1)))
(defun w-const () (list 'w-sym "str" '(1 2) 3.5))
(defun w-other (x) (w-add x 1))
(defun w-type (x) (the fixnum x))
(chk "NATIVE"
     (mapcar #'native-p (list #'w-add #'w-sum #'w-fact #'w-count #'w-deep #'w-lists
                              #'w-chars #'w-push #'w-pop #'w-struct #'w-vec #'w-apply
                              #'w-funcall #'w-global #'w-const #'w-other #'w-type)))

(chk "ADD" (list (w-add 1 2) (w-add most-positive-fixnum 1) (w-add 1/2 1/3) (w-add 1.5 2)))
(chk "SUM" (w-sum 100000))
(chk "FACT" (w-fact 25))
(chk "LISTS" (w-lists 1 nil 3))
(chk "CHARS" (w-chars "banana bandana"))
(chk "PUSH" (w-push '(1 2 3)))
(chk "POP" (w-pop '(1 2 3 4)))
(chk "STRUCT" (w-struct (make-wpt :x 41)))
(chk "VEC" (w-vec (vector 'a 'b)))
(chk "APPLY" (list (w-apply #'+ '(1 2 3)) (w-apply '+ (make-list 300 :initial-element 1))
                   (w-funcall #'1+ 9) (w-funcall 'car '(7))))
(chk "GLOBAL" (progn (setq *w-global* 10) (w-global) (w-global)))
(chk "TAIL-OTHER" (w-other 41))
(chk "TYPE" (w-type 5))

;; The native call stack: a deep non-tail recursion, a self tail call that
;; runs in one frame, and a runaway recursion that signals and recovers.
(chk "DEEP" (w-deep 900))
;; Each native call has its CL_Frame, as an interpreted one: the same
;; 1024-frame limit, and the interpreter's error.
(chk "DEEP-LIMIT" (handler-case (w-deep 2000) (error (e) (princ-to-string e))))
(chk "SELF-TAIL" (w-count 1000000 0))
;; Mutual tail calls between native functions hand the callee to
;; cl_jit_invoke instead of nesting a C call: constant space.
(defun w-even (n) (if (= n 0) :even (w-odd (- n 1))))
(defun w-odd (n) (if (= n 0) :odd (w-even (- n 1))))
(chk "MUTUAL-TAIL" (list (native-p #'w-even) (w-even 1000000) (w-even 1000001)))
(chk "RUNAWAY" (handler-case (w-deep 10000000) (error () :overflowed)))
(chk "AFTER-RUNAWAY" (w-deep 10))

;; Errors from a helper unwind through native frames.
(chk "TYPE-ERROR" (handler-case (w-add 'a 1) (type-error () :te)))
(chk "THE-ERROR" (handler-case (w-type "s") (type-error () :te)))
(chk "UNDEFINED" (handler-case (funcall (compile nil '(lambda () (w-undefined-fn 1))))
                   (undefined-function () :undef)))
(chk "AFTER-ERRORS" (w-add 2 3))

;; Values held by native frames across collections (moving under gengc).
(defun w-gc (x) (let ((a (cons x (list x 'w-sym)))) (ext:gc) (list (car a) (cddr a) (w-const))))
(chk "GC" (w-gc "keep"))
(defun w-churn (n) (let ((acc nil)) (dotimes (i n) (setq acc (cons (list i i) acc))) (length acc)))
(chk "CHURN" (progn (w-churn *churn-n*) (ext:gc) (= (w-churn *churn-n*) *churn-n*)))
(chk "CONST-AFTER-GC" (progn (ext:gc) (w-const)))

;; Redefinition recompiles; the old code is not run again.
(defun w-add (a b) (- a b))
(chk "REDEFINED" (list (w-add 5 3) (native-p #'w-add) (w-other 41)))   ; w-other: 41-1

;; The interpreter's multiple-value state: a single-valued opcode resets it.
(defun w-mv () (values 1 2 3))
(defun w-car (x) (car x))
(chk "MV" (list (multiple-value-list (w-mv))
                (multiple-value-list (progn (w-mv) (w-car '(9))))
                (multiple-value-list (w-funcall #'values 4))))

;; The loop poll: a call-free native loop in a worker while this thread
;; collects.  The worker reaches a safepoint only at its loop header.
(defvar *w-stop* nil)
(defun w-spin () (let ((n 0)) (loop (when *w-stop* (return (> n 0))) (setq n (+ n 1)))))
(chk "SPIN-NATIVE" (native-p #'w-spin))
(chk "LOOP-POLL"
     (let ((th (mp:make-thread #'w-spin)))
       (sleep 0.05)
       (dotimes (i 3) (ext:gc))
       (setq *w-stop* t)
       (mp:join-thread th)))
;; Native code on several threads at once, consing (each collection stops
;; all).
(chk "THREADS"
     (let ((out (make-array 4 :initial-element nil)))
       (mapc #'mp:join-thread
             (loop for k below 4
                   collect (let ((k k))
                             (mp:make-thread
                              (lambda () (setf (svref out k) (list (w-churn *churn-n*) (w-fact 10))))))))
       (every (lambda (r) (equal r (list *churn-n* 3628800))) out)))

;; --- Phase 2: the fast paths and the top-of-stack cache ---------------------
;; X is a fresh heap object loaded before each operation, so it sits in the
;; cache while the operation's slow path calls out (and, for a bignum, a
;; ratio or a float, allocates: a moving collection under gc-stress).
(defstruct p2s a b c)
(defvar *p2-var* 10)
(defvar *p2-unbound*)
(defun p2-arith (x a b) (list x (+ a b) x (- a b) x (* a b) x))
(defun p2-cmp (x a b) (list x (< a b) x (> a b) x (<= a b) x (>= a b) x (= a b)))
(defun p2-br (a b)
  (let ((n 0))
    (when (< a b) (setq n (+ n 1)))
    (when (> a b) (setq n (+ n 10)))
    (when (<= a b) (setq n (+ n 100)))
    (when (>= a b) (setq n (+ n 1000)))
    (when (= a b) (setq n (+ n 10000)))
    (unless (< a b) (setq n (+ n 100000)))
    n))
(defun p2-chr (a b) (if (char= a b) :same :diff))
(defun p2-car (x l) (list x (car l) x (cdr l) x))
(defun p2-st (x s v) (list x (p2s-a s) x (setf (p2s-c s) v) x (p2s-c s)))
(defun p2-sref-bad (s) (amilisp::%struct-ref s 7))
(defun p2-sset-bad (s v) (amilisp::%struct-set s 7 v))
(defun p2-gread () *p2-var*)
(defun p2-gwrite (v) (setq *p2-var* v) *p2-var*)
(defun p2-gbr () (if *p2-var* :yes :no))
(defun p2-geq (x) (if (eq x *p2-var*) :eq :ne))
(defun p2-gub () *p2-unbound*)
(defun p2-pkg (p) (setq *package* p) (package-name *package*))
(defun p2-deep (a b c d e) (+ a (+ b (+ c (+ d (+ e (car (list a))))))))
(defun p2-deep-heap (a b c d) (list a b c d (+ (length a) most-positive-fixnum) a b c d))
(defun p2-eqnot (x y) (list x (eq x y) x (not x) (not nil) y))
(defun p2-mv (a) (+ a 1))
(chk "P2-NATIVE"
     (mapcar #'native-p (list #'p2-arith #'p2-cmp #'p2-br #'p2-chr #'p2-car #'p2-st
                              #'p2-sref-bad #'p2-sset-bad #'p2-gread #'p2-gwrite
                              #'p2-gbr #'p2-geq #'p2-gub #'p2-pkg #'p2-deep
                              #'p2-deep-heap #'p2-eqnot #'p2-mv)))
(defun p2-k () (list "k"))
(chk "P2-FIX" (p2-arith (p2-k) 3 4))
(chk "P2-MAX" (p2-arith (p2-k) most-positive-fixnum 1))
(chk "P2-MIN" (p2-arith (p2-k) most-negative-fixnum 1))
(chk "P2-MIN-1" (p2-arith (p2-k) most-negative-fixnum -1))
(chk "P2-SQ" (p2-arith (p2-k) 46341 46341))
(chk "P2-SQ-FITS" (p2-arith (p2-k) 23170 -23170))
(chk "P2-RATIO-FLOAT" (p2-arith (p2-k) 1/2 0.5))
(chk "P2-ARITH-TE" (handler-case (p2-arith (p2-k) 'a 1) (type-error () :te)))
(chk "P2-ARITH-TE2" (handler-case (p2-arith (p2-k) 1 "s") (type-error () :te)))
(chk "P2-CMP" (list (p2-cmp (p2-k) 1 2) (p2-cmp (p2-k) 2 2) (p2-cmp (p2-k) -3 -4)))
(chk "P2-CMP-MIXED" (list (p2-cmp (p2-k) 1 1.0) (p2-cmp (p2-k) 1/3 1) (p2-cmp (p2-k) (expt 2 40) 1)))
(chk "P2-CMP-TE" (handler-case (p2-cmp (p2-k) 'a 1) (type-error () :te)))
(chk "P2-CMP-COMPLEX" (handler-case (p2-cmp (p2-k) #c(1 2) 1) (type-error () :te)))
(chk "P2-BR" (list (p2-br 1 2) (p2-br 2 1) (p2-br 3 3) (p2-br 1.5 2) (p2-br 2 3/2)
                   (p2-br most-negative-fixnum most-positive-fixnum) (p2-br (expt 2 40) 0)))
(chk "P2-BR-TE" (handler-case (p2-br 1 'b) (type-error () :te)))
(chk "P2-CHR" (list (p2-chr #\a #\a) (p2-chr #\a #\b)
                    (handler-case (p2-chr 1 #\a) (type-error () :te))))
(chk "P2-CAR" (list (p2-car (p2-k) (cons 1 2)) (p2-car (p2-k) nil)))
(chk "P2-CAR-TE" (list (handler-case (p2-car (p2-k) 5) (type-error () :te))
                       (handler-case (p2-car (p2-k) (make-p2s)) (type-error () :te))
                       (handler-case (p2-car (p2-k) #\a) (type-error () :te))))
(chk "P2-STRUCT" (let ((s (make-p2s :a 'one))) (list (p2-st (p2-k) s (list 'v)) (p2s-c s))))
(chk "P2-STRUCT-TE" (list (handler-case (p2-st (p2-k) (cons 1 2) 3) (type-error () :te))
                          (handler-case (p2-st (p2-k) nil 3) (type-error () :te))
                          (handler-case (p2-st (p2-k) 7 3) (type-error () :te))))
(chk "P2-STRUCT-RANGE" (list (handler-case (p2-sref-bad (make-p2s)) (error () :range))
                             (handler-case (p2-sset-bad (make-p2s) 1) (error () :range))))
(chk "P2-GLOBAL" (progn (setq *p2-var* 10)
                        (list (p2-gread) (p2-gbr) (p2-geq 10) (p2-geq 11)
                              (let ((*p2-var* 20))
                                (list (p2-gread) (p2-gwrite 30) (p2-gread) (p2-geq 30)))
                              (p2-gread) (p2-gwrite nil) (p2-gbr) (p2-gwrite (list 1)))))
(chk "P2-UNBOUND" (handler-case (p2-gub) (unbound-variable () :unbound)))
(chk "P2-PACKAGE" (let ((*package* *package*))
                    (list (p2-pkg (find-package :keyword)) (package-name *package*)
                          (symbol-package (intern "P2-PKG-PROBE")))))
(chk "P2-DEEP" (list (p2-deep 1 2 3 4 5) (p2-deep most-positive-fixnum 1 1 1 1)))
(chk "P2-DEEP-HEAP" (p2-deep-heap "abc" (p2-k) (p2-k) (p2-k)))
(chk "P2-EQNOT" (list (p2-eqnot 'a 'a) (p2-eqnot nil 1)))
(chk "P2-MV" (list (multiple-value-list (progn (w-mv) (p2-mv 1)))
                   (multiple-value-list (progn (w-mv) (p2-mv most-positive-fixnum)))
                   (multiple-value-list (progn (w-mv) (p2-gread)))))
;; A struct slot written on four threads at once (the publication barrier).
(chk "P2-THREADS"
     (let ((out (make-array 4 :initial-element nil)))
       (mapc #'mp:join-thread
             (loop for k below 4
                   collect (let ((k k))
                             (mp:make-thread
                              (lambda ()
                                (let ((s (make-p2s)))
                                  (dotimes (i 2000) (p2-st (p2-k) s (list i)))
                                  (setf (svref out k) (p2s-c s))))))))
       (every (lambda (r) (equal r '(1999))) out)))

(chk "DISASM" (with-output-to-string (*standard-output*) (amilisp::%jit-disassemble #'w-add)))
(format t "DONE~%")
EOF
out=$(run "$WORK/walk.lisp")
check_contains "the script ran to the end" "DONE" "$out"
check_contains "the phase-1 shapes compile" \
    "NATIVE (T T T T T T T T T T T T T T T T T)" "$out"
check_contains "arithmetic: fixnum, bignum overflow, ratio, float" "ADD (3 1073741824 5/6 3.5)" "$out"
check_contains "a counting loop" "SUM 4999950000" "$out"
check_contains "a recursive bignum product" "FACT 15511210043330985984000000" "$out"
check_contains "list, cons, car, cdr, eq, not" "LISTS (1 NIL 3 (1) 3 (NIL) T T)" "$out"
check_contains "string scan: char, char=" "CHARS 6" "$out"
check_contains "push onto a local" "PUSH (9 4 1)" "$out"
check_contains "pop from a local" "POP 10" "$out"
check_contains "struct slot read and write" "STRUCT 42" "$out"
check_contains "vector aref, svref, aset" "VEC (A X)" "$out"
check_contains "apply and funcall, 300 spread arguments" "APPLY (6 300 10 7)" "$out"
check_contains "a special variable read and set" "GLOBAL 12" "$out"
check_contains "a tail call to another function returns its value" "TAIL-OTHER 42" "$out"
check_contains "THE passes a fixnum" "TYPE 5" "$out"
check_contains "deep native recursion" "DEEP 900" "$out"
check_contains "native frames share the interpreter's frame limit" 'DEEP-LIMIT "Call stack overflow"' "$out"
check_contains "a self tail call reuses its frame" "SELF-TAIL 1000000" "$out"
check_contains "mutual native tail calls run in constant space" "MUTUAL-TAIL (T :EVEN :ODD)" "$out"
check_contains "runaway native recursion signals" "RUNAWAY :OVERFLOWED" "$out"
check_contains "native code runs after the overflow" "AFTER-RUNAWAY 10" "$out"
check_contains "a type error unwinds through native code" "TYPE-ERROR :TE" "$out"
check_contains "THE signals a type error" "THE-ERROR :TE" "$out"
check_contains "an undefined function signals" "UNDEFINED :UNDEF" "$out"
check_contains "native code runs after the errors" "AFTER-ERRORS 5" "$out"
check_contains "locals survive a collection" 'GC ("keep" (W-SYM) (W-SYM "str" (1 2) 3.5))' "$out"
check_contains "consing loops across collections" "CHURN T" "$out"
check_contains "constants after a collection" 'CONST-AFTER-GC (W-SYM "str" (1 2) 3.5)' "$out"
check_contains "a redefinition recompiles" "REDEFINED (2 T 40)" "$out"
check_contains "multiple values: kept, reset, forwarded" "MV ((1 2 3) (9) (4))" "$out"
check_contains "the spin loop is native" "SPIN-NATIVE T" "$out"
check_contains "a native loop lets another thread collect" "LOOP-POLL T" "$out"
check_contains "native code on four threads" "THREADS T" "$out"
check_contains "the phase-2 shapes compile" \
    "P2-NATIVE (T T T T T T T T T T T T T T T T T T)" "$out"
check_contains "fixnum + - *" 'P2-FIX (("k") 7 ("k") -1 ("k") 12 ("k"))' "$out"
check_contains "+ overflows at the top" 'P2-MAX (("k") 1073741824 ("k") 1073741822 ("k") 1073741823 ("k"))' "$out"
check_contains "- overflows at the bottom, * reaches it" 'P2-MIN (("k") -1073741823 ("k") -1073741825 ("k") -1073741824 ("k"))' "$out"
check_contains "* of the minimum by -1 overflows" 'P2-MIN-1 (("k") -1073741825 ("k") -1073741823 ("k") 1073741824 ("k"))' "$out"
check_contains "a square past the fixnum range" 'P2-SQ (("k") 92682 ("k") 0 ("k") 2147488281 ("k"))' "$out"
check_contains "a negative product inside it" 'P2-SQ-FITS (("k") 0 ("k") 46340 ("k") -536848900 ("k"))' "$out"
check_contains "ratio and float arithmetic" 'P2-RATIO-FLOAT (("k") 1.0 ("k") 0.0 ("k") 0.25 ("k"))' "$out"
check_contains "+ of a symbol signals" "P2-ARITH-TE :TE" "$out"
check_contains "+ of a string signals" "P2-ARITH-TE2 :TE" "$out"
check_contains "fixnum comparisons" 'P2-CMP ((("k") T ("k") NIL ("k") T ("k") NIL ("k") NIL) (("k") NIL ("k") NIL ("k") T ("k") T ("k") T) (("k") NIL ("k") T ("k") NIL ("k") T ("k") NIL))' "$out"
check_contains "mixed comparisons" 'P2-CMP-MIXED ((("k") NIL ("k") NIL ("k") T ("k") T ("k") T) (("k") T ("k") NIL ("k") T ("k") NIL ("k") NIL) (("k") NIL ("k") T ("k") NIL ("k") T ("k") NIL))' "$out"
check_contains "< of a symbol signals" "P2-CMP-TE :TE" "$out"
check_contains "< of a complex signals" "P2-CMP-COMPLEX :TE" "$out"
check_contains "fused compare-and-branch" "P2-BR (101 101010 111100 101 101010 101 101010)" "$out"
check_contains "a fused < of a symbol signals" "P2-BR-TE :TE" "$out"
check_contains "char= in a branch" "P2-CHR (:SAME :DIFF :TE)" "$out"
check_contains "car and cdr of a cons and of NIL" 'P2-CAR ((("k") 1 ("k") 2 ("k")) (("k") NIL ("k") NIL ("k")))' "$out"
check_contains "car of a fixnum, a struct, a char signals" "P2-CAR-TE (:TE :TE :TE)" "$out"
check_contains "struct slot read and write" 'P2-STRUCT ((("k") ONE ("k") (V) ("k") (V)) (V))' "$out"
check_contains "a struct accessor on a non-struct signals" "P2-STRUCT-TE (:TE :TE :TE)" "$out"
check_contains "a slot index past the struct signals" "P2-STRUCT-RANGE (:RANGE :RANGE)" "$out"
check_contains "special variables: global, bound, set" "P2-GLOBAL (10 :YES :EQ :NE (20 30 30 :EQ) 10 NIL :NO (1))" "$out"
check_contains "an unbound variable signals" "P2-UNBOUND :UNBOUND" "$out"
check_contains "setq of *package* switches the package" 'P2-PACKAGE ("KEYWORD" "KEYWORD" #<PACKAGE KEYWORD>)' "$out"
check_contains "a deep expression spills the cache" "P2-DEEP (16 2147483650)" "$out"
check_contains "heap values across a spill and a bignum" 'P2-DEEP-HEAP ("abc" ("k") ("k") ("k") 1073741826 "abc" ("k") ("k") ("k"))' "$out"
check_contains "eq and not from the cache" "P2-EQNOT ((A T A NIL T A) (NIL NIL NIL T T 1))" "$out"
check_contains "the fast and slow paths reset the values count" "P2-MV ((2) (1073741824) ((1)))" "$out"
check_contains "struct writes on four threads" "P2-THREADS T" "$out"
check_contains "the disassembler decodes the frame" "stp x29, x30, [sp, #-96]!" "$out"
check_contains "the disassembler decodes a fast path" "tbz w9, #0," "$out"
check_contains "the disassembler decodes a helper call" "blr x16" "$out"

# --- Part 3: phase 3 -- NLX frames, dynamic binding, multiple values,
# closures, &key, native frames ------------------------------------------------
# Every shape compiles (P3-NATIVE), and every result is the interpreter's:
# transfers through native frames in both directions, values held across them
# (each "k" is fresh, so under AMILISP_GC_STRESS every one moves), the errors
# of the &key prologue and the helpers, and the same on four threads.
cat > "$WORK/p3.lisp" <<'EOF'
(amilisp::%jit-set-hot-threshold 0)
(defmacro chk (name form)
  `(format t "~A ~S~%" ,name
           (handler-case ,form (error (c) (list :error (princ-to-string c))))))
(defun native-p (f) (not (null (amilisp::%jit-dump-bytes f))))
;; A fresh heap value: under AMILISP_GC_STRESS each one moves what is live.
(defun p3-k () (list "k"))

;; --- BLOCK / RETURN-FROM ---------------------------------------------------
(defun p3-blk (x) (block b (when (> x 3) (return-from b (list :big x))) (list :small x)))
;; RETURN-FROM out of a closure: an NLX through the closure's frame.
(defun p3-blk-fn (f) (funcall f))
(defun p3-blk-nlx (x)
  (let ((k (p3-k)))
    (list k (block b (p3-blk-fn (lambda () (return-from b (list x (p3-k))))) :not-here) k)))
;; Multiple values travel with RETURN-FROM, also through an UNWIND-PROTECT.
(defun p3-blk-mv () (multiple-value-list (block b (return-from b (values 1 (p3-k) 3)))))
(defvar *p3-log* nil)
(defun p3-blk-uwp ()
  (multiple-value-list
   (block b (unwind-protect (p3-blk-fn (lambda () (return-from b (values :a (p3-k)))))
              (push :cleanup *p3-log*)))))

;; --- CATCH / THROW --------------------------------------------------------
(defun p3-throw (tag v) (throw tag v))
(defun p3-catch (tag) (let ((k (p3-k))) (list k (catch tag (p3-throw tag (p3-k)) :no) k)))
(defun p3-catch-inner () (catch :inner (p3-throw :outer :through)))
(defun p3-catch-outer () (catch :outer (p3-catch-inner) :no))
(defun p3-catch-mv () (multiple-value-list (catch :m (throw :m (values 7 8 9)))))
(defun p3-catch-normal () (catch :n (p3-k)))

;; --- TAGBODY / GO across closures --------------------------------------
(defun p3-tb (n)
  (let ((i 0) (acc nil))
    (tagbody
     top
       (when (>= i n) (go out))
       (push i acc)
       (incf i)
       (p3-blk-fn (lambda () (go top)))
     out)
    acc))
(defun p3-tb-uwp ()
  (let ((log nil))
    (tagbody
       (unwind-protect (p3-blk-fn (lambda () (go done)))
         (push :cleanup log))
       (push :skipped log)
     done)
    log))

;; --- UNWIND-PROTECT ------------------------------------------------------
(defun p3-uwp-values () (multiple-value-list (unwind-protect (values 1 (p3-k) 3) (p3-k))))
(defun p3-uwp-error ()
  (let ((log nil))
    (list (handler-case (unwind-protect (error "boom") (push :cleanup log))
            (error (e) (princ-to-string e)))
          log)))
(defun p3-uwp-nested ()
  (let ((log nil))
    (catch :x
      (unwind-protect (throw :x :out)
        (unwind-protect (push :inner log) (push :inner-cleanup log))
        (push :outer-cleanup log)))
    (reverse log)))
(defun p3-uwp-rethrow ()
  ;; A throw from the cleanup replaces the transfer in flight.
  (catch :b (catch :a (unwind-protect (throw :a :first) (throw :b :second)))))

;; --- HANDLER-CASE, HANDLER-BIND, RESTART-CASE ------------------------------
(defun p3-hc (what)
  (handler-case
      (case what
        (:type (car 5))
        (:simple (error "simple ~A" 1))
        (:warn (warn "w") :warned)
        (t (list :ok (p3-k))))
    (type-error () :type-error)
    (simple-error (e) (list :simple (princ-to-string e)))
    (warning () :warning)))
(defun p3-hb ()
  (let ((seen nil))
    (handler-bind ((warning (lambda (c) (push (princ-to-string c) seen) (muffle-warning c))))
      (warn "first")
      (warn "second"))
    seen))
(defun p3-restart (v)
  (restart-case (progn (invoke-restart 'p3-use (p3-k)) :not-here)
    (p3-use (x) (list :used x v))))
(defun p3-restart-handler ()
  (handler-bind ((error (lambda (c) (declare (ignore c)) (invoke-restart 'p3-skip :skipped))))
    (restart-case (error "x") (p3-skip (v) v))))

;; --- Dynamic binding, PROGV ------------------------------------------------
(defvar *p3-d* :global)
(defun p3-dyn-read () *p3-d*)
(defun p3-dyn (v) (let ((*p3-d* v)) (list (p3-dyn-read) (let ((*p3-d* :inner)) (p3-dyn-read)) (p3-dyn-read))))
(defun p3-dyn-err ()
  (list (handler-case (let ((*p3-d* :bound)) (error "x")) (error () (p3-dyn-read)))
        (catch :t (let ((*p3-d* :bound)) (throw :t (p3-dyn-read))))
        (p3-dyn-read)))
(defun p3-progv (syms vals) (progv syms vals (if (boundp '*p3-d*) (p3-dyn-read) :unbound)))
(defun p3-progv-pkg () (progv '(*package*) (list (find-package :keyword)) (package-name *package*)))

;; --- Multiple values -------------------------------------------------------
(defun p3-mv (n) (values n (p3-k) (* n 2)))
(defun p3-mvb () (multiple-value-bind (a b c d) (p3-mv 3) (list a b c d)))
(defun p3-nth (i) (nth-value i (p3-mv 5)))
(defun p3-mvl () (multiple-value-list (p3-mv 4)))
(defun p3-mvcall () (multiple-value-call #'list (p3-mv 1) (p3-mv 2)))
(defun p3-nth-bad (i) (nth-value i (p3-mv 5)))

;; --- Closures and cells ----------------------------------------------------
(defun p3-counter ()
  (let ((n 0)) (lambda () (setq n (+ n 1)) (list n (p3-k)))))
(defun p3-adders (xs) (mapcar (lambda (x) (lambda (y) (+ x y))) xs))
;; A closure inside a closure: captures the outer closure's upvalue.
(defun p3-nest (a)
  (let ((b (p3-k)))
    (lambda (c) (let ((f (lambda () (list a b c)))) (funcall f)))))
(defun p3-shared ()
  (let ((v 0))
    (let ((inc (lambda () (incf v))) (get (lambda () v)))
      (funcall inc) (funcall inc) (setq v (+ v 10))
      (list (funcall get) v))))

;; --- &key ------------------------------------------------------------------
(defun p3-key (a &key (b 2) (c (list a b) cp)) (list a b c cp))
(defun p3-key-aok (&key x &allow-other-keys) x)
(defun p3-key-call () (list (p3-key 1) (p3-key 1 :c 3) (p3-key 1 :b 5 :b 6)
                            (p3-key 1 :zz 9 :allow-other-keys t)
                            (p3-key-aok :x 4 :y 5)))
(defun p3-key-tail (x) (p3-key x :b (p3-k)))
(defun p3-key-heap () (p3-key (p3-k) :c (p3-k) :b (p3-k)))

(chk "P3-NATIVE"
     (mapcar #'native-p
             (list #'p3-blk #'p3-blk-nlx #'p3-blk-mv #'p3-blk-uwp #'p3-catch #'p3-catch-outer
                   #'p3-catch-mv #'p3-tb #'p3-tb-uwp #'p3-uwp-values #'p3-uwp-error
                   #'p3-uwp-nested #'p3-uwp-rethrow #'p3-hc #'p3-hb #'p3-restart
                   #'p3-restart-handler #'p3-dyn #'p3-dyn-err #'p3-progv #'p3-mvb #'p3-nth
                   #'p3-mvl #'p3-mvcall #'p3-counter #'p3-adders #'p3-nest #'p3-shared
                   #'p3-key #'p3-key-aok #'p3-key-call #'p3-key-tail)))
(chk "P3-BLOCK" (list (p3-blk 1) (p3-blk 9)))
(chk "P3-BLOCK-NLX" (p3-blk-nlx 5))
(chk "P3-BLOCK-MV" (p3-blk-mv))
(chk "P3-BLOCK-UWP" (list (p3-blk-uwp) *p3-log*))
(chk "P3-CATCH" (list (p3-catch :t) (p3-catch-outer) (p3-catch-mv) (p3-catch-normal)))
(chk "P3-THROW-NONE" (p3-throw :nobody 1))
(chk "P3-TAGBODY" (list (p3-tb 4) (p3-tb 0) (p3-tb-uwp)))
(chk "P3-UWP" (list (p3-uwp-values) (p3-uwp-error) (p3-uwp-nested) (p3-uwp-rethrow)))
(chk "P3-HC" (list (p3-hc :type) (p3-hc :simple) (p3-hc :warn) (p3-hc :none)))
(chk "P3-HB" (p3-hb))
(chk "P3-RESTART" (list (p3-restart 1) (p3-restart-handler)))
(chk "P3-DYN" (list (p3-dyn :a) (p3-dyn-err) (p3-dyn-read)))
(chk "P3-PROGV" (list (p3-progv '(*p3-d*) '(:pv)) (p3-progv '(*p3-d*) nil) (p3-dyn-read)
                      (p3-progv-pkg) (package-name *package*)))
(chk "P3-PROGV-BAD" (p3-progv '(5) '(1)))
(chk "P3-MV" (list (p3-mvb) (p3-nth 0) (p3-nth 1) (p3-nth 2) (p3-nth 7) (p3-mvl) (p3-mvcall)))
(chk "P3-NTH-BAD" (p3-nth-bad :x))
(chk "P3-CLOSURE" (let ((c (p3-counter)))
                    (list (funcall c) (funcall c) (funcall c)
                          (mapcar (lambda (f) (funcall f 10)) (p3-adders '(1 2 3)))
                          (funcall (p3-nest :a) :c) (p3-shared))))
(chk "P3-KEY" (p3-key-call))
(chk "P3-KEY-TAIL" (p3-key-tail 7))
(chk "P3-KEY-HEAP" (p3-key-heap))
(chk "P3-KEY-ODD" (p3-key 1 :b))
(chk "P3-KEY-UNKNOWN" (p3-key 1 :nope 2))
(chk "P3-KEY-NONSYM" (p3-key 1 42 2))
(chk "P3-KEY-NONE" (p3-key))
;; NLX frames on four threads at once: each thread's own NLX stack.
(chk "P3-THREADS"
     (let ((out (make-array 4 :initial-element nil)))
       (mapc #'mp:join-thread
             (loop for k below 4
                   collect (let ((k k))
                             (mp:make-thread
                              (lambda ()
                                (let ((r nil))
                                  (dotimes (i 300)
                                    (setq r (list (p3-catch :t) (p3-blk-nlx i) (p3-hc :type)
                                                  (p3-dyn k) (p3-tb 3))))
                                  (setf (svref out k) r)))))))
       (loop for k below 4
             always (equal (svref out k)
                           (list (p3-catch :t) (p3-blk-nlx 299) (p3-hc :type)
                                 (list k :inner k) (p3-tb 3))))))

;; A template compiled just now is young: the closure's allocation moves it
;; (the classic collector compacts under AMILISP_GC_STRESS), so the helper
;; must read it from the constants after allocating.
(chk "P3-FRESH-TEMPLATE"
     (let ((r nil))
       (dotimes (i 20)
         (let* ((junk (make-list 50))
                (f (compile nil `(lambda (x) (lambda () (list x ,i))))))
           (setq junk nil)
           (push (funcall (funcall f i)) r)))
       (list (native-p (compile nil '(lambda (x) (lambda () x))))
             (every (lambda (p) (eql (first p) (second p))) r)
             (length r))))

;; --- Native frames: FRAME-LOCALS reads a native caller's locals -------------
(defun p3-fl-inner () (ext:frame-locals 1))
(defun p3-fl-outer (a b) (let ((c (+ a b))) (list (p3-fl-inner) c)))
(chk "P3-FRAME-LOCALS" (list (native-p #'p3-fl-outer) (p3-fl-outer 3 4)))
(format t "DONE~%")
EOF
out=$(run "$WORK/p3.lisp")
check_contains "the phase-3 script ran to the end" "DONE" "$out"
check_contains "the phase-3 shapes compile" \
    "P3-NATIVE (T T T T T T T T T T T T T T T T T T T T T T T T T T T T T T T T)" "$out"
check_contains "block and return-from" "P3-BLOCK ((:SMALL 1) (:BIG 9))" "$out"
check_contains "return-from out of a closure" 'P3-BLOCK-NLX (("k") (5 ("k")) ("k"))' "$out"
check_contains "return-from carries multiple values" 'P3-BLOCK-MV (1 ("k") 3)' "$out"
check_contains "return-from through an unwind-protect" 'P3-BLOCK-UWP ((:A ("k")) (:CLEANUP))' "$out"
check_contains "catch and throw" 'P3-CATCH ((("k") ("k") ("k")) :THROUGH (7 8 9) ("k"))' "$out"
check_contains "a throw without a catch signals" 'P3-THROW-NONE (:ERROR "No catch for tag NOBODY")' "$out"
check_contains "go out of a closure, repeatedly, and through a cleanup" \
    "P3-TAGBODY ((3 2 1 0) NIL (:CLEANUP))" "$out"
check_contains "unwind-protect: values, error, nesting, a throw from a cleanup" \
    'P3-UWP ((1 ("k") 3) ("boom" (:CLEANUP)) (:INNER :INNER-CLEANUP :OUTER-CLEANUP) :SECOND)' "$out"
check_contains "handler-case picks the clause" \
    'P3-HC (:TYPE-ERROR (:SIMPLE "simple 1") :WARNING (:OK ("k")))' "$out"
check_contains "handler-bind" 'P3-HB ("second" "first")' "$out"
check_contains "restart-case and invoke-restart" 'P3-RESTART ((:USED ("k") 1) :SKIPPED)' "$out"
check_contains "special bindings, restored by an error and a throw" \
    "P3-DYN ((:A :INNER :A) (:GLOBAL :BOUND :GLOBAL) :GLOBAL)" "$out"
check_contains "progv: bound, unbound, *package*" \
    'P3-PROGV (:PV :UNBOUND :GLOBAL "KEYWORD" "COMMON-LISP-USER")' "$out"
check_contains "progv of a non-symbol signals" \
    'P3-PROGV-BAD (:ERROR "PROGV: expected symbol, got non-symbol")' "$out"
check_contains "multiple-value-bind, nth-value, -list, -call" \
    'P3-MV ((3 ("k") 6 NIL) 5 ("k") 10 NIL (4 ("k") 8) (1 ("k") 2 2 ("k") 4))' "$out"
check_contains "nth-value of a non-number signals" \
    'P3-NTH-BAD (:ERROR "NTH-VALUE: index must be a number")' "$out"
check_contains "closures: a counter, adders, a nested capture, a shared cell" \
    'P3-CLOSURE ((1 ("k")) (2 ("k")) (3 ("k")) (11 12 13) (:A ("k") :C) (12 12))' "$out"
check_contains "&key: defaults, supplied-p, duplicates, allow-other-keys" \
    "P3-KEY ((1 2 (1 2) NIL) (1 2 3 T) (1 5 (1 5) NIL) (1 2 (1 2) NIL) 4)" "$out"
check_contains "a tail call into an &key function" 'P3-KEY-TAIL (7 ("k") (7 ("k")) NIL)' "$out"
check_contains "&key arguments across collections" 'P3-KEY-HEAP (("k") ("k") ("k") T)' "$out"
check_contains "&key: an odd count signals" 'P3-KEY-ODD (:ERROR "odd number of keyword arguments")' "$out"
check_contains "&key: an unknown keyword signals" 'P3-KEY-UNKNOWN (:ERROR "Unknown keyword argument: NOPE")' "$out"
check_contains "&key: a non-symbol keyword signals" \
    'P3-KEY-NONSYM (:ERROR "Invalid keyword argument: not a symbol")' "$out"
check_contains "&key: a missing required argument signals" \
    'P3-KEY-NONE (:ERROR "Too few arguments to P3-KEY' "$out"
check_contains "NLX frames on four threads" "P3-THREADS T" "$out"
check_contains "a closure over a template that moves" "P3-FRESH-TEMPLATE (T T 20)" "$out"
# The same under the classic collector, which moves old objects too.
out0=$(AMILISP_GENGC=0 run "$WORK/p3.lisp")
total=$((total + 1))
if [ "$out0" = "$out" ]; then
    echo "  ok  the classic collector gives the same results"; passed=$((passed + 1))
else
    echo "  FAIL  the classic collector gives other results"
    echo "$out" > "$WORK/p3-gengc.txt"
    echo "$out0" > "$WORK/p3-classic.txt"
    diff "$WORK/p3-gengc.txt" "$WORK/p3-classic.txt" | head -20
    failed=$((failed + 1))
fi
check_contains "FRAME-LOCALS of a native frame" \
    'P3-FRAME-LOCALS (T (((#:ARG0 . 3) (#:ARG1 . 4) (#:LOCAL2) (#:LOCAL3 . 7)) 7))' "$out"

# A native function's frame is the interpreter's: the backtrace names each
# native frame at the line of the form it is in.
cat > "$WORK/bt.lisp" <<'EOF'
(amilisp::%jit-set-hot-threshold 0)
(defun p3-bt-inner (x) (car x))
(defun p3-bt-outer (y)
  (let ((z (list y)))
    (p3-bt-inner y)
    z))
(format t "BT-NATIVE ~S~%" (mapcar (lambda (f) (not (null (amilisp::%jit-dump-bytes f))))
                                   (list #'p3-bt-inner #'p3-bt-outer)))
(p3-bt-outer 5)
EOF
out=$(run "$WORK/bt.lisp")
check_contains "the backtrace's functions are native" "BT-NATIVE (T T)" "$out"
check_contains "a native frame at the line of its error" "0: P3-BT-INNER ($WORK/bt.lisp:2)" "$out"
check_contains "a suspended native caller at its call's line" "1: P3-BT-OUTER ($WORK/bt.lisp:5)" "$out"

# --- Part 4: direct native-to-native calls ------------------------------------
# A call site whose cell holds the current call generation and a native callee
# enters it directly (runtime_vmstack.c, "Direct native-to-native calls").
# Every value must be the helper path's: the script runs twice, direct calls
# on and off (AMILISP_JIT_DIRECT=0), and every line but the counters (STAT-)
# must agree.  Each check calls its site at least twice: the first call
# misses and fills, the second hits.
cat > "$WORK/p4.lisp" <<'EOF'
(amilisp::%jit-set-hot-threshold 0)
(defparameter *churn-n* (if (ext:getenv "AMILISP_GC_STRESS") 20 2000))
(defmacro chk (name form)
  `(format t "~A ~S~%" ,name
           (handler-case ,form (error (c) (list :error (princ-to-string c))))))
(defun native-p (f) (not (null (amilisp::%jit-dump-bytes f))))
(defun ds (k) (getf (amilisp::%jit-direct-call-stats) k))
(defun p4-k () (list "k"))

;; --- A hit: one fill, then no more misses ---------------------------------
(defun p4-leaf (a b) (+ a b))
(defun p4-caller (n) (let ((s 0)) (dotimes (i n s) (setq s (p4-leaf s 1)))))
(chk "P4-NATIVE" (mapcar #'native-p (list #'p4-leaf #'p4-caller)))
(chk "P4-HIT" (p4-caller 1000))
(defun p4-measure ()
  (let* ((s0 (amilisp::%jit-direct-call-stats))
         (r (p4-caller 1000))
         (s1 (amilisp::%jit-direct-call-stats)))
    (list r (- (getf s1 :fills) (getf s0 :fills))
          (- (getf s1 :misses) (getf s0 :misses)))))
;; The second run: the p4-caller site hits; only the second stats call -- a
;; builtin, which every call misses -- counts a miss.
(p4-measure)
(format t "STAT-HIT ~S~%" (p4-measure))

;; --- Redefinition through every public path -------------------------------
(defun p4-red () 1)
(defun p4-red-caller () (list (p4-red)))
(chk "P4-REDEF"
     (list (p4-red-caller) (p4-red-caller)
           (progn (setf (fdefinition 'p4-red) (lambda () 2)) (p4-red-caller))
           (progn (setf (symbol-function 'p4-red) (lambda () 3)) (p4-red-caller))
           (progn (eval '(defun p4-red () 4)) (p4-red-caller))
           (progn (fmakunbound 'p4-red)
                  (handler-case (p4-red-caller)
                    (undefined-function (e) (list :undefined (cell-error-name e)))))))
;; A redefinition with another arity: the call's error is the helper path's.
(defun p4-ar (a) a)
(defun p4-ar-caller () (list (p4-ar 1)))
(chk "P4-ARITY" (list (p4-ar-caller) (p4-ar-caller)
                      (progn (eval '(defun p4-ar (a b) (+ a b)))
                             (handler-case (p4-ar-caller)
                               (error (c) (subseq (princ-to-string c) 0 27))))))

;; --- FUNCALL: the site's func must be the operand's -------------------------
(defun p4-fc (f x) (list (funcall f x)))
(defun p4-mk (n) (lambda (x) (+ x n)))
(chk "P4-FUNCALL"
     (let ((a (lambda (x) (+ x 1))) (b (lambda (x) (* x 10))) (r nil))
       (dotimes (i 6) (push (p4-fc (if (evenp i) a b) i) r))
       (nreverse r)))
;; Closures over one template: same code, different upvalues.
(chk "P4-CLOSURES"
     (let ((fs (list (p4-mk 1) (p4-mk 100))) (r nil))
       (dotimes (i 4) (push (p4-fc (nth (mod i 2) fs) i) r))
       (nreverse r)))
;; A global function that is a closure, replaced by another one.
(defun p4-gcl-caller (x) (list (p4-gcl x)))
(chk "P4-GLOBAL-CLOSURE"
     (progn (setf (fdefinition 'p4-gcl) (p4-mk 5))
            (list (p4-gcl-caller 1) (p4-gcl-caller 2)
                  (progn (setf (fdefinition 'p4-gcl) (p4-mk 7)) (p4-gcl-caller 1)))))

;; --- Collections between calls: the cell's func must never be stale --------
(defun p4-gc-caller (n)
  (let ((s 0) (keep nil))
    (dotimes (i n)
      (setq s (+ s (p4-leaf i 1)))
      (push (p4-k) keep)
      (when (zerop (mod i 97)) (gc)))
    (list s (length keep))))
(chk "P4-GC" (p4-gc-caller 300))
;; ABA: a closure called through a site, dropped, collected, and a new one of
;; the same size at the same site.
(defun p4-aba ()
  (let ((r nil))
    (dotimes (i 12)
      (let ((f (p4-mk i)))
        (push (car (p4-fc f 0)) r)
        (push (car (p4-fc f 0)) r))
      (gc))
    (nreverse r)))
(chk "P4-ABA" (p4-aba))

;; --- TRACE after the site filled, UNTRACE after --------------------------
(defun p4-tr (x) (* x 2))
(defun p4-tr-caller (x) (list (p4-tr x)))
(chk "P4-TRACE"
     (let* ((a (list (p4-tr-caller 1) (p4-tr-caller 2)))
            (b nil)
            (out (with-output-to-string (*trace-output*)
                   (trace p4-tr)
                   (setq b (p4-tr-caller 3))
                   (untrace p4-tr)))
            (c nil)
            (quiet (with-output-to-string (*trace-output*)
                     (setq c (p4-tr-caller 4)))))
       (list (append a (list b c)) (not (null (search "P4-TR" (string-upcase out))))
             (length quiet))))

;; --- Multiple values through a filled site ---------------------------------
(defun p4-mv (n) (values n (p4-k) (* n 2)))
(defun p4-none () (values))
(defun p4-mv-caller (n) (list (multiple-value-list (p4-mv n))
                              (multiple-value-list (p4-none))
                              (nth-value 2 (p4-mv n))))
(chk "P4-MV" (list (p4-mv-caller 1) (p4-mv-caller 2)))

;; --- &key callees: the same entry ABI, the callee's own prologue -----------
(defun p4-kw (a &key (b 2) (c nil cp)) (list a b c cp))
(defun p4-kw-caller (x) (list (p4-kw x) (p4-kw x :b 5) (p4-kw x :c (p4-k) :b 6)))
(defun p4-kw-bad (x) (p4-kw x :nope 1))
(defun p4-kw-bad-caller (x) (list (p4-kw-bad x)))
(chk "P4-KW" (list (p4-kw-caller 1) (p4-kw-caller 2)))
(chk "P4-KW-ERROR" (list (handler-case (p4-kw-bad-caller 1) (error (c) (princ-to-string c)))
                         (handler-case (p4-kw-bad-caller 1) (error (c) (princ-to-string c)))))

;; --- The callee hands a tail call on (a64_tail_finish): 3000 calls, more
;; than the frame stack holds, so a frame left pushed overflows it ---------
(defun p4-t2 (x) (+ x 1))
(defun p4-t1 (x) (p4-t2 x))
(defun p4-tc (n) (let ((s 0)) (dotimes (i n s) (setq s (+ s (p4-t1 i))))))
(chk "P4-TAIL-HANDOFF" (p4-tc 3000))
;; Errors out of a direct callee, 3000 times: the frame is popped by the unwind.
(defun p4-err (x) (if (oddp x) (error "odd ~A" x) x))
(defun p4-err-caller (n)
  (let ((k 0) (msg nil))
    (dotimes (i n (list k msg))
      (handler-case (progn (p4-err i) (incf k))
        (error (c) (setq msg (princ-to-string c)))))))
(chk "P4-ERRORS" (p4-err-caller 3000))
;; Deep direct recursion, and past the frame stack.
(defun p4-deep (n) (if (= n 0) 0 (+ 1 (p4-deep (- n 1)))))
(chk "P4-DEEP" (list (p4-deep 10) (p4-deep 900)))
(chk "P4-DEEP-LIMIT" (p4-deep 100000))
(chk "P4-AFTER-LIMIT" (p4-deep 10))

;; --- The interpreter's frames: backtrace and FRAME-LOCALS on a hit ---------
(defun p4-bi () (ext:backtrace 4))
(defun p4-bm (q) (list q (p4-bi)))
(defun p4-bo () (cadr (p4-bm 7)))
(chk "P4-BACKTRACE" (progn (p4-bo) (mapcar #'second (p4-bo))))
(defun p4-fl-inner () (ext:frame-locals 1))
(defun p4-fl-outer (a b) (let ((c (+ a b))) (list (p4-fl-inner) c)))
(chk "P4-FRAME-LOCALS" (progn (p4-fl-outer 1 2) (p4-fl-outer 3 4)))

;; --- Frames off: no fills (the hit path pushes a frame), same values -------
(defun p4-two-leaves () (list (p4-leaf 1 2) (p4-leaf 3 4)))
(chk "P4-FRAMES-OFF"
     (progn (amilisp::%jit-set-frames nil)
            (prog1 (list (p4-caller 100) (p4-two-leaves) (p4-two-leaves))
              (amilisp::%jit-set-frames t))))
;; The kill switch: values identical, no fills.
(amilisp::%jit-set-direct-calls nil)
(let ((f0 (ds :fills)))
  (chk "P4-KILL" (list (p4-caller 100) (p4-fc #'1+ 1) (p4-kw-caller 3)))
  (format t "STAT-KILL-FILLS ~S~%" (- (ds :fills) f0)))
(amilisp::%jit-set-direct-calls (not (ext:getenv "AMILISP_JIT_DIRECT_OFF")))

;; --- Threads: per-thread counters; a collector thread while one calls ------
(chk "P4-THREAD-STATS"
     ;; The builtin read directly: a call to DS would fill its own site first.
     (mp:join-thread (mp:make-thread
                      (lambda () (list (getf (amilisp::%jit-direct-call-stats) :fills)
                                       (p4-caller 10))))))
;; Under AMILISP_GC_STRESS the collector thread stops the world without a
;; pause and the caller advances only between collections: 200 rounds took
;; 13-125 s there (one STW-scheduling lottery, master alike) and crossed the
;; 300 s limit in the Linux container.  20 rounds under stress.
(chk "P4-STW"
     (let* ((done nil)
            (n (min 200 *churn-n*))
            (gcer (mp:make-thread
                   (lambda () (loop until done do (make-list 200) (gc)))))
            (r (loop repeat n collect (p4-caller 100))))
       (setq done t)
       (mp:join-thread gcer)
       (list (= (length r) n) (every (lambda (x) (= x 100)) r))))
(chk "P4-INTERRUPT"
     (let* ((stop nil) (hit nil)
            (th (mp:make-thread (lambda () (loop until stop do (p4-caller 100))))))
       (sleep 0.05)
       (mp:interrupt-thread th (lambda () (setq hit t)))
       (loop repeat 200 until hit do (sleep 0.01))
       (setq stop t)
       (mp:join-thread th)
       hit))
(format t "DONE~%")
EOF
out=$(run "$WORK/p4.lisp")
check_contains "the direct-call script ran to the end" "DONE" "$out"
check_contains "caller and callee are native" "P4-NATIVE (T T)" "$out"
check_contains "a native caller calls a native leaf" "P4-HIT 1000" "$out"
if [ -n "$AMILISP_GC_STRESS" ]; then
    # Every allocation collects and bumps the generation: the first stats
    # call's own plist invalidates both sites on the path (P4-MEASURE's call
    # of P4-CALLER, P4-CALLER's of P4-LEAF), each refilled once -- then the
    # 1000 calls hit.
    check_contains "a filled site hits (GC stress: one refill per site)" "STAT-HIT (1000 2 3)" "$out"
else
    check_contains "a filled site hits: no fill and no miss in 1000 calls" "STAT-HIT (1000 0 1)" "$out"
fi
check_contains "redefinition: setf fdefinition, setf symbol-function, defun, fmakunbound" \
    "P4-REDEF ((1) (1) (2) (3) (4) (:UNDEFINED P4-RED))" "$out"
check_contains "a callee redefined with another arity signals" \
    'P4-ARITY ((1) (1) "Too few arguments to P4-AR ")' "$out"
check_contains "funcall of two functions through one site" \
    "P4-FUNCALL ((1) (10) (3) (30) (5) (50))" "$out"
check_contains "closures over one template through one site" "P4-CLOSURES ((1) (101) (3) (103))" "$out"
check_contains "a global closure, replaced" "P4-GLOBAL-CLOSURE ((6) (7) (8))" "$out"
check_contains "collections between calls" "P4-GC (45150 300)" "$out"
check_contains "a collected closure's successor at the same site" \
    "P4-ABA (0 0 1 1 2 2 3 3 4 4 5 5 6 6 7 7 8 8 9 9 10 10 11 11)" "$out"
check_contains "trace after a fill reaches the trace, untrace stops it" \
    "P4-TRACE (((2) (4) (6) (8)) T 0)" "$out"
check_contains "multiple values and (values) through a site" \
    'P4-MV (((1 ("k") 2) NIL 2) ((2 ("k") 4) NIL 4))' "$out"
check_contains "&key callees called directly" \
    'P4-KW (((1 2 NIL NIL) (1 5 NIL NIL) (1 6 ("k") T)) ((2 2 NIL NIL) (2 5 NIL NIL) (2 6 ("k") T)))' "$out"
check_contains "a keyword error from a direct callee" \
    'P4-KW-ERROR ("Unknown keyword argument: NOPE" "Unknown keyword argument: NOPE")' "$out"
check_contains "a tail-call handoff after a direct call pops its frame" "P4-TAIL-HANDOFF 4501500" "$out"
check_contains "errors out of direct callees pop their frames" 'P4-ERRORS (1500 "odd 2999")' "$out"
check_contains "direct recursion" "P4-DEEP (10 900)" "$out"
check_contains "direct recursion reaches the frame limit" 'P4-DEEP-LIMIT (:ERROR "Call stack overflow")' "$out"
check_contains "and runs afterwards" "P4-AFTER-LIMIT 10" "$out"
check_contains "the backtrace through hits" "P4-BACKTRACE (P4-BI P4-BM P4-BO" "$out"
check_contains "FRAME-LOCALS of a direct caller" \
    'P4-FRAME-LOCALS (((#:ARG0 . 3) (#:ARG1 . 4) (#:LOCAL2) (#:LOCAL3 . 7)) 7)' "$out"
check_contains "frames off: the same values" "P4-FRAMES-OFF (100 (3 7) (3 7))" "$out"
check_contains "the kill switch: the same values" \
    "P4-KILL (100 (2) ((3 2 NIL NIL) (3 5 NIL NIL) (3 6 (\"k\") T)))" "$out"
check_contains "the kill switch: no fills" "STAT-KILL-FILLS 0" "$out"
check_contains "the counters are the calling thread's" "P4-THREAD-STATS (0 10)" "$out"
check_contains "a collector thread while direct calls run" "P4-STW (T T)" "$out"
check_contains "an interrupt reaches a thread looping on direct calls" "P4-INTERRUPT T" "$out"
# The same script with direct calls off: every line but the counters.
offout=$(AMILISP_JIT_DIRECT=0 AMILISP_JIT_DIRECT_OFF=1 run "$WORK/p4.lisp")
total=$((total + 1))
echo "$out" | grep -v '^STAT-' > "$WORK/p4-on.txt"
echo "$offout" | grep -v '^STAT-' > "$WORK/p4-off.txt"
if cmp -s "$WORK/p4-on.txt" "$WORK/p4-off.txt"; then
    echo "  ok  direct calls on and off give the same results"; passed=$((passed + 1))
else
    echo "  FAIL  direct calls on and off differ"
    diff "$WORK/p4-on.txt" "$WORK/p4-off.txt" | head -20
    failed=$((failed + 1))
fi

# --- Part 5: &optional and &rest ------------------------------------------------
# Every lambda list compiles: the inline &optional prologue (a missing optional
# is NIL, OP_ARGC reads the entry's count), the &rest/&key helper
# (cl_jit_vmstack_ll_prologue), a self tail call that changes the count, and
# direct calls into such callees.  The values are the HyperSpec's (3.4.1); the
# script also runs interpreted, with direct calls off and under the classic
# collector, and every line but P5-NATIVE and the counters must agree.
cat > "$WORK/p5.lisp" <<'EOF'
(amilisp::%jit-set-hot-threshold 0)
;; Under AMILISP_GC_STRESS every allocation compacts: fewer consing rounds.
(defparameter *p5-n* (if (ext:getenv "AMILISP_GC_STRESS") 20 1000))
(defmacro chk (name form)
  `(format t "~A ~S~%" ,name
           (handler-case ,form (error (c) (list :error (princ-to-string c))))))
(defun native-p (f) (not (null (amilisp::%jit-dump-bytes f))))
(defun p5-k () (list "k"))
(defvar *p5-s* :global)

;; --- &optional: defaults see the earlier parameters, supplied-p -----------
(defun p5-o (a &optional (b 10) (c (list a b) cp)) (list a b c cp))
(defun p5-o-heap (&optional (x (p5-k)) (y (p5-k))) (list x y))
(defun p5-o-special (&optional (s *p5-s*)) s)
(defun p5-o-err (&optional (x (error "default ~A" 1))) x)
(defun p5-o-nil (&optional a b) (list a b))
;; --- &rest ----------------------------------------------------------------
(defun p5-r (a &rest r) (list a r))
(defun p5-r-gc (&rest xs) (gc) xs)
(defun p5-r-sum (&rest xs) (let ((s 0)) (dolist (x xs s) (setq s (+ s x)))))
;; --- Mixed ----------------------------------------------------------------
(defun p5-or (a &optional (b 2 bp) &rest r) (list a b bp r))
(defun p5-ok (a &optional b &key (k 7 kp)) (list a b k kp))
(defun p5-rk (&rest r &key x y &allow-other-keys) (list r x y))
(defun p5-ork (&optional (o :o) &rest r &key x) (list o r x))
;; --- Self tail calls that change the count --------------------------------
(defun p5-acc (n &optional (s 0)) (if (= n 0) s (p5-acc (- n 1) (+ s n))))
(defun p5-st (n &optional (a :dflt a-p))
  (cond ((= n 0) (list a a-p)) ((= n 1) (p5-st 0)) (t (p5-st (- n 1) n))))
(defun p5-st-up (n &optional (a :dflt a-p))
  (if (= n 0) (list a a-p) (if (= n 3) (p5-st-up (- n 1) :x) (p5-st-up (- n 1)))))
;; A self tail call into &rest is an ordinary tail call.
(defun p5-rt (n &rest r) (if (= n 0) (length r) (p5-rt (- n 1) n n)))
;; --- Callers: direct calls with every count -------------------------------
(defun p5-call-o () (list (p5-o 1) (p5-o 1 2) (p5-o 1 2 3)))
(defun p5-call-r () (list (p5-r 1) (p5-r 1 2) (p5-r 1 2 (p5-k) 4)))
;; Two-argument + only: a three-argument one calls the builtin, which misses.
(defun p5-call-loop (n) (let ((s 0)) (dotimes (i n s) (setq s (+ s (+ (p5-acc 3) (p5-r-sum i 1)))))))
(defun p5-tail-o (x) (p5-o x))
(defun p5-tail-r (x) (p5-r x x))
(defun p5-tail-loop (n) (let ((s 0)) (dotimes (i n s) (setq s (+ s (car (p5-tail-o i)) (car (p5-tail-r i)))))))
(defun p5-too-many () (p5-o 1 2 3 4))
(defun p5-too-few () (p5-o))
(defun p5-mk (n) (lambda (&optional (x n) &rest r) (list x r)))
(defun p5-fc (f) (list (funcall f) (funcall f 1) (funcall f 1 2 3)))

(chk "P5-NATIVE" (mapcar #'native-p
                         (list #'p5-o #'p5-o-heap #'p5-o-special #'p5-o-err #'p5-o-nil
                               #'p5-r #'p5-r-gc #'p5-r-sum #'p5-or #'p5-ok #'p5-rk #'p5-ork
                               #'p5-acc #'p5-st #'p5-st-up #'p5-rt (p5-mk 0))))
(chk "P5-OPT" (list (p5-o 1) (p5-o 1 2) (p5-o 1 2 3)))
(chk "P5-OPT-HEAP" (list (p5-o-heap) (p5-o-heap :a) (p5-o-heap :a :b)))
(chk "P5-OPT-SPECIAL" (list (p5-o-special) (let ((*p5-s* :bound)) (p5-o-special)) (p5-o-special 3)))
(chk "P5-OPT-ERR" (list (p5-o-err 5) (handler-case (p5-o-err) (error (e) (princ-to-string e)))))
(chk "P5-OPT-NIL" (list (p5-o-nil) (p5-o-nil 1) (p5-o-nil 1 2)))
(chk "P5-REST" (list (p5-r 1) (p5-r 1 2 3) (p5-r-gc (p5-k) (p5-k) (p5-k))))
(chk "P5-REST-MANY" (list (p5-r-sum) (apply #'p5-r-sum (loop for i from 1 to 250 collect i))
                          (length (apply #'p5-r (make-list 200)))))
(chk "P5-REST-300" (apply #'p5-r-sum (make-list 300 :initial-element 1)))
(chk "P5-OPT-REST" (list (p5-or 1) (p5-or 1 5) (p5-or 1 5 6 (p5-k))))
(chk "P5-OPT-KEY" (list (p5-ok 1) (p5-ok 1 2) (p5-ok 1 2 :k 9) (p5-ok 1 2 :k 9 :k 10)))
(chk "P5-OPT-KEY-BAD" (p5-ok 1 2 :q 3))
(chk "P5-OPT-KEY-ODD" (p5-ok 1 2 :k))
(chk "P5-REST-KEY" (list (p5-rk) (p5-rk :x 1 :y (p5-k) :z 3)))
(chk "P5-OPT-REST-KEY" (list (p5-ork) (p5-ork :a) (p5-ork :a :x 2)))
(chk "P5-OPT-REST-KEY-ODD" (p5-ork :a :x))
(chk "P5-SELF-TAIL" (list (p5-acc 20000) (p5-st 3) (p5-st 2) (p5-st 0 5) (p5-st-up 5)))
(chk "P5-REST-TAIL" (p5-rt 3000))
(chk "P5-DIRECT" (list (p5-call-o) (p5-call-o) (p5-call-r) (p5-call-r) (p5-call-loop 100)))
(chk "P5-TAIL-HANDOFF" (p5-tail-loop 3000))
(chk "P5-TOO-MANY" (subseq (princ-to-string (nth-value 1 (ignore-errors (p5-too-many)))) 0 28))
(chk "P5-TOO-FEW" (subseq (princ-to-string (nth-value 1 (ignore-errors (p5-too-few)))) 0 27))
(chk "P5-CLOSURES" (list (p5-fc (p5-mk 1)) (p5-fc (p5-mk (p5-k)))))
(defun p5-measure ()
  (let* ((s0 (amilisp::%jit-direct-call-stats))
         (r (p5-call-loop *p5-n*))
         (s1 (amilisp::%jit-direct-call-stats)))
    (list r (- (getf s1 :fills) (getf s0 :fills)) (- (getf s1 :misses) (getf s0 :misses)))))
(p5-measure)
(format t "STAT-P5 ~S~%" (p5-measure))
(chk "P5-THREADS"
     (let ((ths (loop for i below 4
                      collect (let ((i i))
                                (mp:make-thread
                                 (lambda ()
                                   (loop repeat (floor *p5-n* 3)
                                         always (and (equal (p5-r i (p5-k)) (list i (list (list "k"))))
                                                     (equal (p5-o i) (list i 10 (list i 10) nil))
                                                     (equal (p5-or i) (list i 2 nil nil))))))))))
       (mapcar #'mp:join-thread ths)))
(format t "DONE~%")
EOF
out=$(run "$WORK/p5.lisp")
check_contains "the &optional/&rest script ran to the end" "DONE" "$out"
check_contains "every lambda list compiles" \
    "P5-NATIVE (T T T T T T T T T T T T T T T T T)" "$out"
check_contains "&optional defaults see earlier parameters; supplied-p" \
    "P5-OPT ((1 10 (1 10) NIL) (1 2 (1 2) NIL) (1 2 3 T))" "$out"
check_contains "&optional defaults that allocate" \
    'P5-OPT-HEAP ((("k") ("k")) (:A ("k")) (:A :B))' "$out"
check_contains "an &optional default reads the dynamic binding" \
    "P5-OPT-SPECIAL (:GLOBAL :BOUND 3)" "$out"
check_contains "an &optional default that signals" 'P5-OPT-ERR (5 "default 1")' "$out"
check_contains "an &optional without a default is NIL" "P5-OPT-NIL ((NIL NIL) (1 NIL) (1 2))" "$out"
check_contains "&rest: none, some, across a collection" \
    'P5-REST ((1 NIL) (1 (2 3)) (("k") ("k") ("k")))' "$out"
check_contains "&rest with 250 and 200 arguments" "P5-REST-MANY (0 31375 2)" "$out"
check_contains "&optional, supplied-p and &rest" \
    'P5-OPT-REST ((1 2 NIL NIL) (1 5 T NIL) (1 5 T (6 ("k"))))' "$out"
check_contains "&optional then &key" \
    "P5-OPT-KEY ((1 NIL 7 NIL) (1 2 7 NIL) (1 2 9 T) (1 2 9 T))" "$out"
check_contains "&optional then &key: an unknown keyword" \
    'P5-OPT-KEY-BAD (:ERROR "Unknown keyword argument: Q")' "$out"
check_contains "&optional then &key: an odd count" \
    'P5-OPT-KEY-ODD (:ERROR "odd number of keyword arguments")' "$out"
check_contains "&rest holds every keyword pair" \
    'P5-REST-KEY ((NIL NIL NIL) ((:X 1 :Y ("k") :Z 3) 1 ("k")))' "$out"
check_contains "&optional, &rest and &key" \
    "P5-OPT-REST-KEY ((:O NIL NIL) (:A NIL NIL) (:A (:X 2) 2))" "$out"
check_contains "&optional, &rest and &key: an odd count" \
    'P5-OPT-REST-KEY-ODD (:ERROR "odd number of keyword arguments")' "$out"
check_contains "self tail calls with another argument count" \
    "P5-SELF-TAIL (200010000 (:DFLT NIL) (:DFLT NIL) (5 T) (:DFLT NIL))" "$out"
check_contains "a tail call into &rest, 3000 deep" "P5-REST-TAIL 2" "$out"
check_contains "direct calls into &optional and &rest callees" \
    'P5-DIRECT (((1 10 (1 10) NIL) (1 2 (1 2) NIL) (1 2 3 T)) ((1 10 (1 10) NIL) (1 2 (1 2) NIL) (1 2 3 T)) ((1 NIL) (1 (2)) (1 (2 ("k") 4))) ((1 NIL) (1 (2)) (1 (2 ("k") 4))) 5650)' "$out"
check_contains "tail-call handoffs into &optional and &rest callees" "P5-TAIL-HANDOFF 8997000" "$out"
check_contains "too many arguments to an &optional callee" \
    'P5-TOO-MANY "Too many arguments to P5-O ("' "$out"
check_contains "too few arguments to an &optional callee" \
    'P5-TOO-FEW "Too few arguments to P5-O ("' "$out"
check_contains "&optional/&rest closures through one site" \
    'P5-CLOSURES (((1 NIL) (1 NIL) (1 (2 3))) ((("k") NIL) (1 NIL) (1 (2 3))))' "$out"
if [ -z "$AMILISP_GC_STRESS" ]; then
    check_contains "the sites into &optional/&rest callees hit: no fill, no miss" \
        "STAT-P5 (506500 0 1)" "$out"
fi
check_contains "&optional and &rest on four threads" "P5-THREADS (T T T T)" "$out"
# The same values interpreted, with direct calls off, under the classic collector.
echo "$out" | grep -v '^STAT-\|^P5-NATIVE' > "$WORK/p5-jit.txt"
p5_same() {
    desc="$1"; shift
    total=$((total + 1))
    echo "$1" | grep -v '^STAT-\|^P5-NATIVE' > "$WORK/p5-other.txt"
    if cmp -s "$WORK/p5-jit.txt" "$WORK/p5-other.txt"; then
        echo "  ok  $desc"; passed=$((passed + 1))
    else
        echo "  FAIL  $desc"
        diff "$WORK/p5-jit.txt" "$WORK/p5-other.txt" | head -20
        failed=$((failed + 1))
    fi
}
if [ -n "$TIMEOUT" ]; then
    interp=$("$TIMEOUT" 300 "$AMILISP" --no-jit --heap 32M --no-userinit --non-interactive \
                 --load "$WORK/p5.lisp" </dev/null 2>&1)
else
    interp=$("$AMILISP" --no-jit --heap 32M --no-userinit --non-interactive \
                 --load "$WORK/p5.lisp" </dev/null 2>&1)
fi
p5_same "the interpreter gives the same results" "$interp"
p5_same "direct calls off give the same results" "$(AMILISP_JIT_DIRECT=0 run "$WORK/p5.lisp")"
p5_same "the classic collector gives the same results" "$(AMILISP_GENGC=0 run "$WORK/p5.lisp")"

echo "$passed passed, $failed failed, $total total"
[ "$failed" -eq 0 ]
