#!/bin/sh
# Regression tests for the Tier-4 Phase 2 call / unwind protocol work
# (specs/performance.md 4.2).  Each case pins the BEHAVIOUR that a
# performance change had to preserve, so a later optimization in the same
# area cannot quietly trade correctness for speed.
#
#   sh tests/test_tier4_phase2.sh build/host/amilisp
#
# Covered:
#   1. OP_CALL_GLOBAL / OP_TAILCALL_GLOBAL — a global call no longer pushes
#      the function: redefinition still takes effect at the call site, an
#      undefined function is a catchable UNDEFINED-FUNCTION, arity errors
#      name the callee, a global tail call runs in constant frame space,
#      TRACE still sees the call, and the (LABELS / FLET value-cell fallback
#      of OP_FLOAD) is preserved.
#   2. The frame's function slot (CL_Frame.fslot) — arguments stay where they
#      were pushed: every call shape (positional, &optional, &rest, &key,
#      APPLY, FUNCALL, closures, generic functions) returns and unwinds to
#      the right stack depth, deep mixes included.
#   3. One NLX landing per cl_vm_run activation instead of a setjmp per
#      CATCH / BLOCK / TAGBODY / UNWIND-PROTECT frame: transfers within one
#      activation, across a C trampoline (MAPCAR), out of closures, out of
#      threads, and through nested cleanups all land correctly; a cleanup's
#      own non-local exit abandons the transfer in flight (CLHS 5.2); the
#      re-thrown transfer resumes after a cleanup.
#   4. UNWIND-PROTECT keeps the protected form's values on the per-thread
#      save stack (OP_MV_SAVE / OP_MV_RESTORE) instead of consing a list:
#      0, 1, 2 and 20 values survive the cleanup; a cleanup that runs an
#      unwind-protect of its own while a transfer is parked; the save
#      stack's overflow is a clean, catchable error.
#   5. HANDLER-CASE is the special form AMILISP::%HANDLER-CASE (one NLX frame
#      + one binding per clause): clause order, values, :no-error, a special
#      clause variable, declarations, nesting, interposed cleanups, restarts,
#      SIGNAL with no match, warnings, threads, and the clause cap.
#   6. DEFSTRUCT keyword constructors carry a compiler macro that turns a
#      literal-keyword call into a positional %MAKE-STRUCT: argument
#      evaluation order, duplicate / unknown / :allow-other-keys keys,
#      non-constant init-forms, :include, FUNCALL / APPLY, dynamic keys.
#   7. CALL-NEXT-METHOD / NEXT-METHOD-P run on a per-EMF method list and
#      ONE special (*CNM*): :around / :before / :after ordering, explicit
#      arguments, "No next method", nested generic calls, auxiliary methods
#      seeing no next method, multiple values, long-form method
#      combinations, threads, redefinition.
#   8. A debugger abort to top level (:q) drops the value records of the
#      UNWIND-PROTECT cleanups it abandons (needs expect(1); skipped
#      otherwise).

AMILISP="${1:-build/host/amilisp}"

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)

passed=0
failed=0
total=0
TMPDIR="${TMPDIR:-/tmp}"
WORK="$TMPDIR/clamiga_tier4p2_$$"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

if [ ! -x "$AMILISP" ]; then
    echo "  SKIP  tier4 phase2: no binary at $AMILISP"
    echo "0 passed, 0 failed, 0 total"
    exit 0
fi

# Loop counts: the repeat loops exist to run the fast paths under GC
# pressure a few thousand times; under AMILISP_GC_STRESS every allocation
# forces a compaction, so far fewer iterations give the same coverage.
if [ -n "${AMILISP_GC_STRESS:-}" ]; then
    LOOPN=60; TAILN=400; DEEPN=40
else
    LOOPN=20000; TAILN=200000; DEEPN=400
fi

run() {
    if [ -n "$TIMEOUT" ]; then
        "$TIMEOUT" 120 "$AMILISP" --no-userinit --heap 64M \
            --non-interactive --load "$1" 2>&1
    else
        "$AMILISP" --no-userinit --heap 64M --non-interactive --load "$1" 2>&1
    fi
}

check_contains() {
    desc="$1"; pattern="$2"; actual="$3"
    total=$((total + 1))
    if echo "$actual" | grep -q "$pattern"; then
        echo "  ok    $desc"
        passed=$((passed + 1))
    else
        echo "  FAIL  $desc (expected /$pattern/)"
        echo "$actual" | tail -8 | sed 's/^/      /'
        failed=$((failed + 1))
    fi
}

check_absent() {
    desc="$1"; pattern="$2"; actual="$3"
    total=$((total + 1))
    if echo "$actual" | grep -q "$pattern"; then
        echo "  FAIL  $desc (unexpected /$pattern/)"
        echo "$actual" | grep "$pattern" | head -3 | sed 's/^/      /'
        failed=$((failed + 1))
    else
        echo "  ok    $desc"
        passed=$((passed + 1))
    fi
}

# ---------------------------------------------------------------------------
# 1. CALL_GLOBAL / TAILCALL_GLOBAL
# ---------------------------------------------------------------------------
cat > "$WORK/t1.lisp" <<EOF
(defun p2-callee (x) (list :v1 x))
(defun p2-caller (x) (p2-callee x))
(format t "R1a ~S~%" (p2-caller 1))
(defun p2-callee (x) (list :v2 x))
(format t "R1b ~S~%" (p2-caller 2))
(format t "R1c ~S~%" (handler-case (p2-undefined-fn 1)
                      (undefined-function () :undefined-function)))
(format t "R1d ~S~%" (handler-case (p2-callee 1 2 3)
                      (error (e) (let ((m (princ-to-string e)))
                                   (if (search "P2-CALLEE" m) :names-callee m)))))
(defun p2-tail (n acc) (if (= n 0) acc (p2-tail (1- n) (+ acc 1))))
(format t "R1e ~S~%" (p2-tail $TAILN 0))
(defun p2-mutual-a (n) (if (= n 0) :a-done (p2-mutual-b (1- n))))
(defun p2-mutual-b (n) (if (= n 0) :b-done (p2-mutual-a (1- n))))
(format t "R1f ~S~%" (p2-mutual-a $TAILN))
;; the OP_FLOAD value-cell fallback (a symbol bound as a function in its
;; value cell) must survive in the fused opcode
(setf (symbol-value 'p2-valcell) (lambda (x) (list :valcell x)))
(format t "R1g ~S~%" (handler-case (p2-valcell 3) (error () :err)))
(defun p2-traced (x) (* x 2))
(defun p2-calls-traced (x) (p2-traced x))
(trace p2-traced)
(format t "R1h ~S~%" (p2-calls-traced 21))
(untrace p2-traced)
(format t "R1i ~S~%" (list (funcall 'p2-callee 5) (apply #'p2-callee '(6))
                           (funcall (symbol-function 'p2-callee) 7)))
;; tail call to a builtin falls through to RET with the builtin's value
(defun p2-tail-builtin (a b) (+ a b))
(format t "R1j ~S~%" (p2-tail-builtin 40 2))
;; a global call whose symbol is later made a macro must not be confused
(defun p2-gen-fn (x) x)
(defmacro p2-gen-mac (x) (list 'quote x))
(format t "R1k ~S~%" (list (p2-gen-fn 1) (p2-gen-mac two)))
;; call through a function whose definition is a generic function
(defgeneric p2-gf (x))
(defmethod p2-gf ((x integer)) (* x 3))
(defun p2-calls-gf (x) (p2-gf x))
(format t "R1l ~S~%" (p2-calls-gf 5))
(format t "T1-DONE~%")
EOF
out=$(run "$WORK/t1.lisp")
check_contains "call sees the definition at call time"           "R1a (:V1 1)" "$out"
check_contains "redefinition reaches an already-compiled call"   "R1b (:V2 2)" "$out"
check_contains "undefined function is catchable"                 "R1c :UNDEFINED-FUNCTION" "$out"
check_contains "arity error names the callee"                    "R1d :NAMES-CALLEE" "$out"
check_contains "global self tail call runs in constant space"    "R1e $TAILN" "$out"
check_contains "mutual global tail calls"                        "R1f :A-DONE\|R1f :B-DONE" "$out"
check_contains "FLOAD value-cell fallback preserved"             "R1g (:VALCELL 3)" "$out"
check_contains "TRACE prints the fused call"                     "P2-TRACED" "$out"
check_contains "traced call returns its value"                   "R1h 42" "$out"
check_contains "funcall / apply / symbol-function still work"    "R1i ((:V2 5) (:V2 6) (:V2 7))" "$out"
check_contains "tail call of a builtin"                          "R1j 42" "$out"
check_contains "function and macro of similar names"            "R1k (1 TWO)" "$out"
check_contains "global call to a generic function"               "R1l 15" "$out"
check_contains "t1 completes"                                    "T1-DONE" "$out"

# ---------------------------------------------------------------------------
# 2. Frame function slot: every call shape returns to the right depth
# ---------------------------------------------------------------------------
cat > "$WORK/t2.lisp" <<EOF
(defun p2-id (x) x)
(defun p2-opt (a &optional (b 10) (c 20)) (list a b c))
(defun p2-rest (a &rest r) (list a r))
(defun p2-key (a &key (b 1) c) (list a b c))
(defun p2-mix (n)
  (let ((acc 0))
    (dotimes (i n)
      (setq acc (+ acc
                   (p2-id i)
                   (car (p2-opt i))
                   (length (second (p2-rest i i i)))
                   (second (p2-key i :b 2))
                   (funcall #'p2-id 1)
                   (apply #'p2-id (list 1))
                   (funcall (lambda (x) x) 1)
                   (apply (lambda (x &rest r) (+ x (length r))) 1 '(2 3)))))
    acc))
;; closed form: 2*sum(i) + n*(2 + 2 + 1 + 1 + 1 + 3)
(defun p2-expect (n) (+ (* n (1- n)) (* n 10)))
(format t "R2a ~S~%" (= (p2-mix $LOOPN) (p2-expect $LOOPN)))
(defclass p2-c () ((s :initarg :s :accessor p2-s)))
(defun p2-acc-loop (n)
  (let ((o (make-instance 'p2-c :s 0)))
    (dotimes (i n) (setf (p2-s o) (+ (p2-s o) 1)))
    (p2-s o)))
(format t "R2b ~S~%" (p2-acc-loop $LOOPN))
;; nested applies and multiple values through call/return
(defun p2-mv (x) (values x (* 2 x) (* 3 x)))
(format t "R2c ~S~%" (multiple-value-list (apply #'p2-mv (list (funcall #'p2-id 2)))))
(format t "R2d ~S~%" (multiple-value-list (funcall #'p2-mv (apply #'p2-id '(3)))))
;; deep non-tail recursion (frames push and pop through fslot=1 frames)
(defun p2-deep (n) (if (= n 0) 0 (+ 1 (p2-deep (1- n)))))
(format t "R2e ~S~%" (p2-deep $DEEPN))
;; backtrace names the callee of a fused call when a builtin errors in it
(defun p2-bt-inner (x) (car x))
(defun p2-bt-outer (x) (p2-bt-inner x))
(format t "R2f ~S~%" (block p2-bt
                      (handler-bind ((error (lambda (c)
                                              (declare (ignore c))
                                              (let ((bt (ext:backtrace)))
                                                (return-from p2-bt
                                                  (if (find "P2-BT-INNER" bt
                                                            :key (lambda (f) (princ-to-string (second f)))
                                                            :test #'string=)
                                                      :inner-in-backtrace bt))))))
                        (p2-bt-outer 5))))
(format t "T2-DONE~%")
EOF
out=$(run "$WORK/t2.lisp")
check_contains "every call shape returns to the right depth"    "R2a T" "$out"
check_contains "accessor IC calls keep the stack balanced"       "R2b $LOOPN" "$out"
check_contains "apply of funcall value keeps all values"         "R2c (2 4 6)" "$out"
check_contains "funcall of apply value keeps all values"         "R2d (3 6 9)" "$out"
check_contains "deep non-tail recursion"                         "R2e $DEEPN" "$out"
check_contains "backtrace names the fused-call callee"           "R2f :INNER-IN-BACKTRACE" "$out"
check_contains "t2 completes"                                    "T2-DONE" "$out"

# ---------------------------------------------------------------------------
# 3. The shared NLX landing
# ---------------------------------------------------------------------------
cat > "$WORK/t3.lisp" <<EOF
(defun p2-id (x) x)
(defvar *p2-log* nil)
(format t "R3a ~S~%" (catch 'tg (p2-id (throw 'tg 42))))
(format t "R3b ~S~%" (multiple-value-list (catch 'tg (throw 'tg (values 1 2 3)))))
(format t "R3c ~S~%" (block b (p2-id (return-from b 43))))
(format t "R3d ~S~%" (block b (mapcar (lambda (x) (when (> x 1) (return-from b x))) '(1 2 3))))
(format t "R3e ~S~%" (let ((n 0)) (tagbody top (incf n) (when (< n 5) (go top))) n))
(format t "R3f ~S~%" (let ((n 0)) (tagbody top (incf n) (mapcar (lambda (x) (when (< n 5) (go top))) '(1))) n))
(format t "R3g ~S~%" (catch 'x (unwind-protect (throw 'x 1) (push :cleanup *p2-log*))))
(format t "R3h ~S~%" (block b (unwind-protect (return-from b 2) (push :c2 *p2-log*))))
(format t "R3i ~S~%" (handler-case (unwind-protect (error "boom") (push :c3 *p2-log*))
                      (error (e) (list :caught (princ-to-string e)))))
(format t "R3j ~S~%" (reverse *p2-log*))
(setq *p2-log* nil)
(format t "R3k ~S~%" (catch 'outer (unwind-protect (catch 'inner (unwind-protect (throw 'outer :o) (push :in *p2-log*))) (push :out *p2-log*))))
(format t "R3l ~S~%" (reverse *p2-log*))
;; a cleanup's own throw abandons the transfer in flight (CLHS 5.2)
(setq *p2-log* nil)
(format t "R3m ~S~%" (catch 'a (unwind-protect (throw 'a 1) (catch 'b (unwind-protect (throw 'b 2) (push :inner-cleanup *p2-log*))))))
(format t "R3n ~S~%" (handler-case (catch 'z (unwind-protect (throw 'z 1) (error "in cleanup"))) (error () :cleanup-error-won)))
;; transfers across a C trampoline with a cleanup in between
(setq *p2-log* nil)
(format t "R3o ~S~%" (catch 'm (mapcar (lambda (x) (unwind-protect (when (= x 2) (throw 'm :thrown)) (push x *p2-log*))) '(1 2 3))))
(format t "R3p ~S~%" (reverse *p2-log*))
;; deep frames below the target: 300 nested catches, throw to the outermost
(defun p2-nest (n) (if (= n 0) (throw 'top :from-bottom) (catch (gensym) (p2-nest (1- n)))))
(format t "R3q ~S~%" (catch 'top (p2-nest 300)))
;; a loop of return-from through unwind-protect (landing reuse)
(defun p2-rf-loop (n) (let ((c 0)) (dotimes (i n) (block b (unwind-protect (return-from b i) (incf c)))) c))
(format t "R3r ~S~%" (p2-rf-loop $LOOPN))
;; go from a closure inside a loop
(format t "R3s ~S~%" (let ((n 0)) (tagbody again (incf n) (funcall (lambda () (when (< n $DEEPN) (go again))))) n))
;; throw out of a thread's own activation
(format t "R3t ~S~%" (mp:join-thread (mp:make-thread (lambda () (catch 'tt (unwind-protect (throw 'tt :thread-ok) (p2-id 1)))))))
;; restart invocation (a C-side throw) through cleanups
(setq *p2-log* nil)
(format t "R3u ~S~%" (restart-case (unwind-protect (invoke-restart 'r 7) (push :rc *p2-log*)) (r (v) (list :r v *p2-log*))))
;; muffle-warning: the C-owned NLX frame in WARN still works (landing = NULL path)
(format t "R3v ~S~%" (handler-bind ((warning (lambda (w) (declare (ignore w)) (muffle-warning)))) (warn "silenced") :after-warn))
;; a local RETURN-FROM / GO from inside a call's argument list used to leave
;; the call's pending arguments on the operand stack (pre-existing; exposed
;; by the CLHS :no-error expansion of HANDLER-CASE) — now routed via NLX
(format t "R3w ~S~%" (list :a (block b (list 1 (return-from b 2))) :c))
(format t "R3x ~S~%" (list :a (block er (multiple-value-call (lambda (v) (* v 10)) (block nr (return-from er :direct)))) :c))
(format t "R3y ~S~%" (list :a (block er (funcall (lambda (v) (* v 10)) (block nr (return-from er :fc)))) :c))
(format t "R3z ~S~%" (list :a (let ((n 0)) (tagbody a (list 1 (if (< (incf n) 3) (go a)))) n) :c))
(format t "R3A ~S~%" (list :a (block b (let ((q (list 1 (return-from b 2)))) q)) :c))
(format t "R3B ~S~%" (list :a (block nil (mapcar (lambda (x) x) (list 1 (return 2)))) :c))
;; ... and the other forms that hold earlier values pending: a parallel LET
;; (all inits before any binding), a parallel DO, PROGV's symbol list, THROW's tag
(format t "R3C ~S~%" (list :a (block er (let ((e 1) (a (return-from er 2)) (f 3)) (list e a f))) :c))
(format t "R3D ~S~%" (list :a (block er (do ((i 1) (j (return-from er 2))) (t (list i j)))) :c))
(format t "R3E ~S~%" (list :a (block er (progv '(*p2-pv*) (list (return-from er 2)) nil)) :c))
(format t "R3F ~S~%" (list :a (block er (throw 'never (return-from er 2))) :c))
(format t "R3G ~S~%" (list :a (block er (let* ((e 1) (a (return-from er 2))) (list e a))) :c))
(format t "T3-DONE~%")
EOF
out=$(run "$WORK/t3.lisp")
check_contains "catch/throw within one activation"               "R3a 42" "$out"
check_contains "throw carries multiple values"                   "R3b (1 2 3)" "$out"
check_contains "return-from within one activation"               "R3c 43" "$out"
check_contains "return-from out of a mapcar closure"             "R3d 2" "$out"
check_contains "local go"                                        "R3e 5" "$out"
check_contains "go out of a mapcar closure"                      "R3f 5" "$out"
check_contains "throw through unwind-protect"                    "R3g 1" "$out"
check_contains "return-from through unwind-protect"              "R3h 2" "$out"
check_contains "error through unwind-protect into handler-case"  "R3i (:CAUGHT \"boom\")" "$out"
check_contains "cleanups ran once each in order"                 "R3j (:CLEANUP :C2 :C3)" "$out"
check_contains "throw past an inner catch runs both cleanups"    "R3k :O" "$out"
check_contains "inner cleanup before outer cleanup"              "R3l (:IN :OUT)" "$out"
check_contains "cleanup throw abandons the pending transfer"     "R3m 1" "$out"
check_contains "cleanup error abandons the pending throw"        "R3n :CLEANUP-ERROR-WON" "$out"
check_contains "throw across MAPCAR with cleanup"                "R3o :THROWN" "$out"
check_contains "cleanup ran for the throwing element"            "R3p (1 2)" "$out"
check_contains "throw to a frame 300 catches below"              "R3q :FROM-BOTTOM" "$out"
check_contains "return-from through cleanup in a loop"           "R3r $LOOPN" "$out"
check_contains "go from a closure repeatedly"                    "R3s $DEEPN" "$out"
check_contains "throw inside a worker thread"                    "R3t :THREAD-OK" "$out"
check_contains "invoke-restart runs the cleanup first"           "R3u (:R 7 (:RC))" "$out"
check_contains "muffle-warning C-owned frame still lands"        "R3v :AFTER-WARN" "$out"
check_absent   "no warning text escaped muffle-warning"          "WARNING: silenced" "$out"
check_contains "local return-from inside call args (list)"       "R3w (:A 2 :C)" "$out"
check_contains "local return-from inside multiple-value-call"    "R3x (:A :DIRECT :C)" "$out"
check_contains "local return-from inside funcall args"           "R3y (:A :FC :C)" "$out"
check_contains "local go inside call args"                       "R3z (:A 3 :C)" "$out"
check_contains "local return-from inside a let init call"        "R3A (:A 2 :C)" "$out"
check_contains "return inside args of a mapcar call"             "R3B (:A 2 :C)" "$out"
check_contains "local return-from in a later parallel-LET init"  "R3C (:A 2 :C)" "$out"
check_contains "local return-from in a later parallel-DO init"   "R3D (:A 2 :C)" "$out"
check_contains "local return-from in PROGV's values form"        "R3E (:A 2 :C)" "$out"
check_contains "local return-from in THROW's value form"         "R3F (:A 2 :C)" "$out"
check_contains "local return-from in a LET* init"                "R3G (:A 2 :C)" "$out"
check_contains "t3 completes"                                    "T3-DONE" "$out"

# ---------------------------------------------------------------------------
# 4. UNWIND-PROTECT value passing (OP_MV_SAVE / OP_MV_RESTORE)
# ---------------------------------------------------------------------------
cat > "$WORK/t4.lisp" <<EOF
(defun p2-id (x) x)
(format t "R4a ~S~%" (multiple-value-list (unwind-protect (values) (p2-id 0))))
(format t "R4b ~S~%" (multiple-value-list (unwind-protect 5 (p2-id 0))))
(format t "R4c ~S~%" (multiple-value-list (unwind-protect (values 1 2) (p2-id 0))))
(format t "R4d ~S~%" (multiple-value-list (unwind-protect (floor 7 2) (values 9 9 9))))
(format t "R4e ~S~%" (multiple-value-list
  (unwind-protect (values 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20)
    (p2-id 0))))
;; a variable read as the protected form (MV state is stale there)
(format t "R4f ~S~%" (let ((x (floor 7 2))) (multiple-value-list (unwind-protect x (p2-id 0)))))
;; nested unwind-protects each restore their own values
(format t "R4g ~S~%" (multiple-value-list
  (unwind-protect (unwind-protect (values :a :b) (unwind-protect (values :x :y) (p2-id 0)))
    (unwind-protect (values :p :q) (p2-id 0)))))
;; an unwind-protect running INSIDE a cleanup while a transfer is parked
(format t "R4h ~S~%" (let (r) (catch 'a (unwind-protect (throw 'a 1)
                                          (setq r (multiple-value-list (unwind-protect (values 5 6) (p2-id 0))))))
                        r))
;; multiple-value-prog1 shape and with-lock-held (both unwind-protect users)
(defun p2-swap (k v ht) (multiple-value-prog1 (gethash k ht) (setf (gethash k ht) v)))
(defvar *p2-ht* (make-hash-table))
(setf (gethash 'k *p2-ht*) 1)
(format t "R4i ~S~%" (multiple-value-list (p2-swap 'k 2 *p2-ht*)))
(defvar *p2-lock* (mp:make-lock))
(format t "R4j ~S~%" (multiple-value-list (mp:with-lock-held (*p2-lock*) (gethash 'k *p2-ht*))))
;; values survive an allocating cleanup under GC pressure
(defun p2-uwp-loop (n) (let ((acc 0)) (dotimes (i n) (multiple-value-bind (q r) (unwind-protect (floor i 3) (make-list 10)) (incf acc (+ q r)))) acc))
(format t "R4k ~S~%" (p2-uwp-loop $LOOPN))
;; the save stack overflows cleanly: a many-valued cleanup recursion
(defun p2-cleanup-rec (n) (unwind-protect (values 1 2 3 4 5 6 7 8 9 10) (when (> n 0) (p2-cleanup-rec (1- n)))))
(format t "R4l ~S~%" (handler-case (progn (p2-cleanup-rec 400) :no-overflow)
                      (error (e) (if (search "value-save stack overflow" (princ-to-string e)) :clean-overflow-error (princ-to-string e)))))
;; ... and the runtime is still usable afterwards
(format t "R4m ~S~%" (multiple-value-list (unwind-protect (values 7 8) (p2-id 0))))
(format t "T4-DONE~%")
EOF
out=$(run "$WORK/t4.lisp")
check_contains "zero values survive the cleanup"                "R4a NIL" "$out"
check_contains "one value survives the cleanup"                 "R4b (5)" "$out"
check_contains "two values survive the cleanup"                 "R4c (1 2)" "$out"
check_contains "cleanup's own values are discarded"             "R4d (3 1)" "$out"
check_contains "twenty values survive the cleanup"              "R4e (1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20)" "$out"
check_contains "variable protected form is one value"          "R4f (3)" "$out"
check_contains "nested unwind-protects keep their own values"   "R4g (:A :B)" "$out"
check_contains "unwind-protect inside a cleanup with parked throw" "R4h (5 6)" "$out"
check_contains "multiple-value-prog1 keeps the second value"    "R4i (1 T)" "$out"
check_contains "with-lock-held keeps the second value"          "R4j (2 T)" "$out"
check_contains "values through allocating cleanups in a loop"   "R4k" "$out"
check_contains "save-stack overflow is a clean error"           "R4l :CLEAN-OVERFLOW-ERROR" "$out"
check_contains "runtime usable after the overflow"              "R4m (7 8)" "$out"
check_contains "t4 completes"                                   "T4-DONE" "$out"

# ---------------------------------------------------------------------------
# 5. HANDLER-CASE as a special form
# ---------------------------------------------------------------------------
cat > "$WORK/t5.lisp" <<EOF
(defun p2-id (x) x)
(defvar *p2-log* nil)
(format t "R5a ~S~%" (handler-case (error "x") (error () :ok)))
(format t "R5b ~S~%" (handler-case (error "x") (warning () :w) (error (e) (list :e (princ-to-string e)))))
;; textually first matching clause wins, even when a later one is more specific
(format t "R5c ~S~%" (handler-case (error 'simple-error :format-control "s") (error () :general) (simple-error () :specific)))
(format t "R5d ~S~%" (multiple-value-list (handler-case (values 1 2 3) (error () :ok))))
(format t "R5e ~S~%" (multiple-value-list (handler-case (values) (error () :ok))))
(format t "R5f ~S~%" (handler-case (handler-case (error "inner") (warning () :w)) (error () :outer-caught)))
(format t "R5g ~S~%" (handler-case (progn (handler-case (error "a") (error () (error "b"))) :no) (error (e) (princ-to-string e))))
(format t "R5h ~S~%" (handler-case (error "x") ((or warning error) (c) (type-of c))))
(format t "R5i ~S~%" (handler-case (+ 1 2) (:no-error (v) (* v 10))))
(format t "R5j ~S~%" (multiple-value-list (handler-case (values 1 2) (:no-error (a b) (list :ne a b)) (error () :e))))
(format t "R5k ~S~%" (handler-case (error "x") (:no-error (v) (* v 10)) (error () :err)))
(defvar *p2-sp* :global)
(format t "R5l ~S~%" (handler-case (error "x") (error (*p2-sp*) (list (type-of *p2-sp*) (type-of (symbol-value '*p2-sp*))))))
(format t "R5m ~S~%" *p2-sp*)
(format t "R5n ~S~%" (handler-case (error "x") (error (e) (declare (ignore e)) :declared)))
(format t "R5o ~S~%" (handler-case (unwind-protect (unwind-protect (error "x") (push :c1 *p2-log*)) (push :c2 *p2-log*)) (error () (reverse *p2-log*))))
(format t "R5p ~S~%" (block b (handler-case (return-from b :rf) (error () :e))))
(format t "R5q ~S~%" (catch 'tg (handler-case (throw 'tg :thrown) (error () :e))))
(format t "R5r ~S~%" (handler-case (mapcar (lambda (x) (when (= x 2) (error "two")) x) '(1 2 3)) (error () :mapcar-caught)))
(setq *p2-log* nil)
(format t "R5s ~S~%" (handler-bind ((error (lambda (c) (declare (ignore c)) (push :outer-hb *p2-log*)))) (handler-case (error "x") (error () :hc-first))))
(format t "R5t ~S~%" *p2-log*)
(format t "R5u ~S~%" (handler-case (handler-bind ((error (lambda (c) (declare (ignore c)) (push :hb *p2-log*)))) (error "x")) (error () (list :after-hb *p2-log*))))
(format t "R5v ~S~%" (handler-case (signal 'warning) (error () :e)))
(format t "R5w ~S~%" (handler-case (signal 'warning) (warning (w) (list :warn (type-of w)))))
(format t "R5x ~S~%" (multiple-value-list (ignore-errors (error "ig"))))
(defun p2-hc-loop (n) (let ((acc 0)) (dotimes (i n) (setq acc (+ acc (handler-case (if (evenp i) (error "e") i) (error () 1))))) acc))
(format t "R5y ~S~%" (p2-hc-loop $LOOPN))
(format t "R5z ~S~%" (mp:join-thread (mp:make-thread (lambda () (handler-case (error "in thread") (error () :thread-caught))))))
(format t "R5A ~S~%" (handler-case (restart-case (error "r") (retry () :retried)) (error () :hc)))
(format t "R5B ~S~%" (restart-case (handler-case (invoke-restart 'retry) (error () :hc)) (retry () :retried2)))
(defun p2-deep (n) (if (= n 0) (error "bottom") (1+ (p2-deep (1- n)))))
(format t "R5C ~S~%" (handler-case (p2-deep $DEEPN) (error () :deep-caught)))
(format t "R5D ~S~%" (handler-case (handler-case (error "x") (error (e) (error e))) (error (e) (list :rethrown (princ-to-string e)))))
(format t "R5E ~S~%" (let ((r nil)) (dolist (x '(1 2 3)) (handler-case (push (p2-id x) r) (error () nil))) r))
;; a clause variable captured by a closure and mutated
(format t "R5F ~S~%" (handler-case (error "x") (error (e) (let ((f (lambda () (setq e :mutated) e))) (funcall f)))))
;; the clause cap (64 = CL_MAX_HANDLER_BINDINGS) is a clean compile error
(format t "R5G ~S~%" (handler-case
                      (eval (list* 'handler-case '(error "x")
                                   (let (cl) (dotimes (i 65) (push '(error () :c) cl)) cl)))
                      (error (e) (if (search "too many clauses" (princ-to-string e)) :clean-cap-error (princ-to-string e)))))
(format t "R5H ~S~%" (macroexpand-1 '(handler-case (foo) (error (e) e))))
(format t "T5-DONE~%")
EOF
out=$(run "$WORK/t5.lisp")
check_contains "basic handler-case"                              "R5a :OK" "$out"
check_contains "non-matching clause skipped"                     "R5b (:E \"x\")" "$out"
check_contains "textually first matching clause wins"            "R5c :GENERAL" "$out"
check_contains "all values of the form on normal exit"           "R5d (1 2 3)" "$out"
check_contains "zero values on normal exit"                      "R5e NIL" "$out"
check_contains "unhandled inner passes to outer"                 "R5f :OUTER-CAUGHT" "$out"
check_contains "error inside a clause reaches the outer"         "R5g \"b\"" "$out"
check_contains "compound clause type"                            "R5h SIMPLE-ERROR" "$out"
check_contains ":no-error runs on normal exit"                   "R5i 30" "$out"
check_contains ":no-error receives all values"                   "R5j ((:NE 1 2))" "$out"
check_contains ":no-error skipped on a handled error"            "R5k :ERR" "$out"
check_contains "special clause variable is bound dynamically"    "R5l (SIMPLE-ERROR SIMPLE-ERROR)" "$out"
check_contains "special clause variable is unbound after"        "R5m :GLOBAL" "$out"
check_contains "clause declarations accepted"                    "R5n :DECLARED" "$out"
check_contains "cleanups run before the clause, inner first"     "R5o (:C1 :C2)" "$out"
check_contains "return-from out of the form"                     "R5p :RF" "$out"
check_contains "throw out of the form"                           "R5q :THROWN" "$out"
check_contains "error inside a MAPCAR closure"                   "R5r :MAPCAR-CAUGHT" "$out"
check_contains "inner handler-case beats outer handler-bind"     "R5s :HC-FIRST" "$out"
check_contains "outer handler-bind did not run"                  "R5t NIL" "$out"
check_contains "inner handler-bind runs, then handler-case"      "R5u (:AFTER-HB (:HB))" "$out"
check_contains "SIGNAL with no matching clause returns NIL"      "R5v NIL" "$out"
check_contains "warning clause"                                  "R5w (:WARN WARNING)" "$out"
check_contains "ignore-errors returns nil and the condition"     "R5x (NIL #<" "$out"
check_contains "handler-case in a loop with errors"              "R5y" "$out"
check_contains "handler-case inside a worker thread"             "R5z :THREAD-CAUGHT" "$out"
check_contains "handler-case around restart-case"                "R5A :HC" "$out"
check_contains "restart invoked from inside handler-case form"   "R5B :RETRIED2" "$out"
check_contains "error from deep recursion"                       "R5C :DEEP-CAUGHT" "$out"
check_contains "re-signal from a clause"                         "R5D (:RETHROWN \"x\")" "$out"
check_contains "handler-case as a non-tail loop body"            "R5E (3 2 1)" "$out"
check_contains "clause variable captured and mutated"            "R5F :MUTATED" "$out"
check_contains "clause cap is a clean compile error"             "R5G :CLEAN-CAP-ERROR" "$out"
check_contains "macroexpands to the special form"                "R5H (%HANDLER-CASE (FOO) (ERROR (E) E))" "$out"
check_contains "t5 completes"                                    "T5-DONE" "$out"

# ---------------------------------------------------------------------------
# 6. DEFSTRUCT keyword-constructor compiler macro
# ---------------------------------------------------------------------------
cat > "$WORK/t6.lisp" <<EOF
(defstruct p2-st (a 0) (b 0) (c 'sym))
(defvar *p2-order* nil)
(format t "R6a ~S~%" (make-p2-st :a 1 :b 2))
(format t "R6b ~S~%" (make-p2-st))
(format t "R6c ~S~%" (make-p2-st :c 5 :a 1))
(format t "R6d ~S~%" (make-p2-st :b (progn (push :b *p2-order*) 1) :a (progn (push :a *p2-order*) 2)))
(format t "R6e ~S~%" (reverse *p2-order*))
(format t "R6f ~S~%" (make-p2-st :a 1 :a 2))
(format t "R6g ~S~%" (handler-case (make-p2-st :zz 1) (error () :unknown-key-error)))
(format t "R6h ~S~%" (make-p2-st :a 1 :allow-other-keys t :zz 3))
(format t "R6i ~S~%" (list (funcall #'make-p2-st :a 9) (apply #'make-p2-st '(:b 8))))
(defstruct (p2-pt (:constructor mk-p2-pt) (:constructor mk-p2-pt-boa (x y))) x y)
(format t "R6j ~S~%" (list (mk-p2-pt :x 1 :y 2) (mk-p2-pt-boa 3 4)))
(defstruct p2-dyn (a (list 1 2)) (b (gensym)))
(format t "R6k ~S~%" (list (p2-dyn-a (make-p2-dyn)) (symbolp (p2-dyn-b (make-p2-dyn)))
                           (eq (p2-dyn-b (make-p2-dyn)) (p2-dyn-b (make-p2-dyn)))))
(let ((k :a)) (format t "R6l ~S~%" (make-p2-st k 7)))
(defstruct (p2-sub (:include p2-st)) (d 4))
(format t "R6m ~S~%" (make-p2-sub :a 1 :d 9))
;; the inlined form is a positional %MAKE-STRUCT; the declined one a keyword call
(disassemble (lambda (x) (make-p2-st :a x :b 2)))
(disassemble (lambda (x) (make-p2-dyn :a x)))
;; a slot default that is a quoted list is shared, as in the keyword call
(defstruct p2-q (l '(1 2)))
(format t "R6p ~S~%" (eq (p2-q-l (make-p2-q)) (p2-q-l (make-p2-q))))
(defun p2-mk-loop (n) (let ((acc 0)) (dotimes (i n) (incf acc (p2-st-a (make-p2-st :a i :b (list i))))) acc))
(format t "R6q ~S~%" (= (p2-mk-loop $LOOPN) (/ (* $LOOPN (1- $LOOPN)) 2)))
(format t "T6-DONE~%")
EOF
out=$(run "$WORK/t6.lisp")
check_contains "keyword constructor with two keys"              "R6a #S(P2-ST :A 1 :B 2 :C SYM)" "$out"
check_contains "no keys: defaults"                              "R6b #S(P2-ST :A 0 :B 0 :C SYM)" "$out"
check_contains "keys in any order"                              "R6c #S(P2-ST :A 1 :B 0 :C 5)" "$out"
check_contains "argument forms evaluated in call order"         "R6e (:B :A)" "$out"
check_contains "values land in the right slots"                 "R6d #S(P2-ST :A 2 :B 1 :C SYM)" "$out"
check_contains "leftmost duplicate key wins"                    "R6f #S(P2-ST :A 1 :B 0 :C SYM)" "$out"
check_contains "unknown key is an error"                        "R6g :UNKNOWN-KEY-ERROR" "$out"
check_contains ":allow-other-keys t honoured"                   "R6h #S(P2-ST :A 1 :B 0 :C SYM)" "$out"
check_contains "funcall and apply of the constructor"           "R6i (#S(P2-ST :A 9 :B 0 :C SYM) #S(P2-ST :A 0 :B 8 :C SYM))" "$out"
check_contains "named keyword and BOA constructors"             "R6j (#S(P2-PT :X 1 :Y 2) #S(P2-PT :X 3 :Y 4))" "$out"
check_contains "non-constant init-forms evaluated per call"     "R6k ((1 2) T NIL)" "$out"
check_contains "non-literal key takes the keyword call"         "R6l #S(P2-ST :A 7 :B 0 :C SYM)" "$out"
check_contains ":include slots included"                        "R6m #S(P2-SUB :A 1 :B 0 :C SYM :D 9)" "$out"
check_contains "literal-key call inlines to %MAKE-STRUCT"       "TAILCALL_GLOBAL.*%MAKE-STRUCT" "$out"
check_contains "non-constant default declines to the call"      "TAILCALL_GLOBAL.*MAKE-P2-DYN" "$out"
check_contains "quoted default is shared"                       "R6p T" "$out"
check_contains "constructor loop under GC pressure"             "R6q T" "$out"
check_contains "t6 completes"                                   "T6-DONE" "$out"

# ---------------------------------------------------------------------------
# 7. CALL-NEXT-METHOD chains on the per-EMF vector
# ---------------------------------------------------------------------------
cat > "$WORK/t7.lisp" <<EOF
(defclass p2-thing () ((a :initarg :a :initform 0 :accessor p2-thing-a)))
(defclass p2-other (p2-thing) ())
(defclass p2-third (p2-other) ())
(defvar *p2-log* nil)
(defgeneric p2-g1 (x))
(defmethod p2-g1 ((x p2-thing)) (push :thing *p2-log*) (list :thing))
(defmethod p2-g1 ((x p2-other)) (push :other *p2-log*) (cons :other (call-next-method)))
(defmethod p2-g1 ((x p2-third)) (push :third *p2-log*) (if (next-method-p) (cons :third (call-next-method)) :none))
(defmethod p2-g1 :around ((x p2-thing)) (push :around-thing *p2-log*) (list :at (call-next-method)))
(defmethod p2-g1 :around ((x p2-other)) (push :around-other *p2-log*) (list :ao (call-next-method)))
(defmethod p2-g1 :before ((x p2-thing)) (push :before *p2-log*))
(defmethod p2-g1 :after ((x p2-thing)) (push :after *p2-log*))
(format t "R7a ~S~%" (p2-g1 (make-instance 'p2-third)))
(format t "R7b ~S~%" (reverse *p2-log*))
(setq *p2-log* nil)
(format t "R7c ~S~%" (p2-g1 (make-instance 'p2-thing)))
(format t "R7d ~S~%" (reverse *p2-log*))
(defgeneric p2-g2 (x y))
(defmethod p2-g2 ((x p2-thing) y) (list :base y))
(defmethod p2-g2 ((x p2-other) y) (call-next-method x (* y 10)))
(format t "R7e ~S~%" (p2-g2 (make-instance 'p2-other) 4))
(defgeneric p2-g3 (x))
(defmethod p2-g3 ((x p2-thing)) (call-next-method))
(format t "R7f ~S~%" (handler-case (p2-g3 (make-instance 'p2-thing)) (error (e) (princ-to-string e))))
(defgeneric p2-g4 (x))
(defmethod p2-g4 ((x p2-thing)) (next-method-p))
(defmethod p2-g4 ((x p2-other)) (list (next-method-p) (call-next-method)))
(format t "R7g ~S~%" (list (p2-g4 (make-instance 'p2-thing)) (p2-g4 (make-instance 'p2-other))))
(defgeneric p2-inner (x))
(defmethod p2-inner ((x p2-thing)) (next-method-p))
(defgeneric p2-outer (x))
(defmethod p2-outer ((x p2-thing)) :outer-base)
(defmethod p2-outer ((x p2-other)) (list (p2-inner x) (call-next-method)))
(format t "R7h ~S~%" (p2-outer (make-instance 'p2-other)))
(defgeneric p2-g5 (x))
(defmethod p2-g5 ((x p2-thing)) :p)
(defmethod p2-g5 :after ((x p2-thing)) (push (next-method-p) *p2-log*))
(setq *p2-log* nil)
(format t "R7i ~S~%" (list (p2-g5 (make-instance 'p2-thing)) *p2-log*))
(defgeneric p2-mv (x))
(defmethod p2-mv ((x p2-thing)) (values 1 2))
(defmethod p2-mv ((x p2-other)) (call-next-method))
(defmethod p2-mv :around ((x p2-other)) (call-next-method))
(format t "R7j ~S~%" (multiple-value-list (p2-mv (make-instance 'p2-other))))
(defgeneric p2-g6 (x))
(defmethod p2-g6 ((x p2-thing)) :t6)
(defmethod p2-g6 ((x p2-other)) (funcall (lambda () (call-next-method))))
(format t "R7k ~S~%" (p2-g6 (make-instance 'p2-other)))
(defun p2-loop-cnm (n) (let ((o (make-instance 'p2-third)) (c 0)) (dotimes (i n) (when (p2-g1 o) (incf c))) c))
(format t "R7l ~S~%" (p2-loop-cnm $LOOPN))
(define-method-combination p2-comb () ((primary ())) (list 'list :comb (list 'call-method (first primary) (rest primary))))
(defgeneric p2-g7 (x) (:method-combination p2-comb))
(defmethod p2-g7 ((x p2-thing)) :t7)
(defmethod p2-g7 ((x p2-other)) (list :o7 (call-next-method)))
(format t "R7m ~S~%" (p2-g7 (make-instance 'p2-other)))
(format t "R7n ~S~%" (mp:join-thread (mp:make-thread (lambda () (p2-g1 (make-instance 'p2-other))))))
(defmethod p2-g1 ((x p2-third)) :redefined)
(format t "R7o ~S~%" (p2-g1 (make-instance 'p2-third)))
(defmethod print-object ((x p2-other) s) (call-next-method))
(format t "R7p ~S~%" (subseq (princ-to-string (make-instance 'p2-other)) 0 2))
;; a method with no next calling CALL-NEXT-METHOD from the LAST position of
;; an :around chain (the primary EMF is bare) still errors, never recurses
(defgeneric p2-g8 (x))
(defmethod p2-g8 ((x p2-thing)) (call-next-method))
(defmethod p2-g8 :around ((x p2-thing)) (call-next-method))
(format t "R7q ~S~%" (handler-case (p2-g8 (make-instance 'p2-thing)) (error (e) (princ-to-string e))))
(format t "T7-DONE~%")
EOF
out=$(run "$WORK/t7.lisp")
check_contains "full standard combination result"              "R7a (:AO (:AT (:THIRD :OTHER :THING)))" "$out"
check_contains "full standard combination order"               "R7b (:AROUND-OTHER :AROUND-THING :BEFORE :THIRD :OTHER :THING :AFTER)" "$out"
check_contains "single primary with around/before/after"       "R7c (:AT (:THING))" "$out"
check_contains "single primary order"                          "R7d (:AROUND-THING :BEFORE :THING :AFTER)" "$out"
check_contains "call-next-method with explicit arguments"      "R7e (:BASE 40)" "$out"
check_contains "no next method is an error"                    "R7f \"No next method\"" "$out"
check_contains "next-method-p in first and last"               "R7g (NIL (T NIL))" "$out"
check_contains "nested generic call sees its own chain"        "R7h (NIL :OUTER-BASE)" "$out"
check_contains ":after sees no next method"                    "R7i (:P (NIL))" "$out"
check_contains "multiple values through around and primary"   "R7j (1 2)" "$out"
check_contains "call-next-method from a closure in the method" "R7k :T6" "$out"
check_contains "chain calls under GC pressure"                 "R7l $LOOPN" "$out"
check_contains "long-form combination with call-method"        "R7m (:COMB (:O7 :T7))" "$out"
check_contains "chain inside a worker thread"                  "R7n (:AO (:AT (:OTHER :THING)))" "$out"
check_contains "method redefinition rebuilds the chain"        "R7o (:AO (:AT :REDEFINED))" "$out"
check_contains "print-object call-next-method"                 "R7p \"#<\"\|R7p \"#S\"" "$out"
check_contains "last primary calling next is an error"         "R7q \"No next method\"" "$out"
check_contains "t7 completes"                                  "T7-DONE" "$out"

# ---------------------------------------------------------------------------
# 8. The save stack after a debugger abort (:q / "Return to top level")
#    jump_to_top_level resets every other per-thread stack top; it must
#    drop the UNWIND-PROTECT value records of the cleanups it abandons
#    too, or the save stack only grows across aborts (found by the review
#    of the phase-2 commit).  The interactive debugger needs a real tty,
#    so this leg runs under expect(1) and is skipped where it isn't
#    installed (Linux CI images, MSYS2) — same as test_debugger_eof.sh.
# ---------------------------------------------------------------------------
EXPECT=$(command -v expect 2>/dev/null || true)
if [ -z "$EXPECT" ]; then
    echo "  SKIP  save stack dropped across debugger :q (expect(1) not on PATH)"
else
cat > "$WORK/t8.exp" <<'EOF'
set timeout 60
spawn [lindex $argv 0] --no-userinit --no-color
expect {
    "COMMON-LISP-USER>" {}
    timeout { puts "T8FAIL: no REPL prompt"; exit 1 }
}
# 50 nested cleanups, each with a 20-value record parked while it runs
# (50 x 21 words of the 2048-word stack); the innermost one lands in the
# debugger.
send "(defun p2-park (n err) (unwind-protect (values 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20) (if (> n 0) (p2-park (1- n) err) (when err (error \"park\")))))\r"
expect {
    "COMMON-LISP-USER>" {}
    timeout { puts "T8FAIL: defun did not return"; exit 1 }
}
send "(p2-park 49 t)\r"
expect {
    "Debug>" {}
    timeout { puts "T8FAIL: no Debug> prompt"; exit 1 }
}
send ":q\r"
expect {
    "COMMON-LISP-USER>" {}
    timeout { puts "T8FAIL: no prompt after :q"; exit 1 }
}
# A second nest of the same size overflows the stack if the first one's
# records were never dropped.  The marker is assembled at run time so the
# echoed input line cannot match it.
send "(format t \"~%~A ~S~%\" (concatenate 'string \"R8\" \"a\") (handler-case (progn (p2-park 49 nil) :ok) (error (e) (if (search \"value-save stack overflow\" (princ-to-string e)) :leaked (princ-to-string e)))))\r"
# Match the WHOLE result line, then look at it: expect tries its patterns on
# whatever has arrived so far, so a bare "R8a " catch-all fired whenever the
# pty delivered the line in two pieces ("R8a " first, ":OK" later) — a rare
# false "unexpected error after :q".
expect {
    -re {R8a ([^\r\n]*)[\r\n]} {
        set r8 $expect_out(1,string)
        if {$r8 eq ":OK"} {
            puts "T8PASS"
        } elseif {$r8 eq ":LEAKED"} {
            puts "T8FAIL: save stack leaked across :q"; exit 1
        } else {
            puts "T8FAIL: unexpected error after :q: $r8"; exit 1
        }
    }
    eof { puts "T8FAIL: REPL died"; exit 1 }
    timeout { puts "T8FAIL: no result after :q"; exit 1 }
}
send "\x04"
expect { eof {} timeout {} }
exit 0
EOF
out=$("$EXPECT" -f "$WORK/t8.exp" "$AMILISP" 2>&1)
check_contains "save stack dropped across debugger :q"         "T8PASS" "$out"
fi

echo "$passed passed, $failed failed, $total total"
[ "$failed" -eq 0 ]
