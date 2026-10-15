#!/bin/sh
# Regression tests for local-function inlining (specs/performance.md 4.2
# item 5): a FLET/LABELS function the compiler proves non-escaping keeps no
# closure — every call compiles to its body in place, with the names the
# call site bound since the definition hidden from the body.
#
#   sh tests/test_local_inline.sh build/host/amilisp
#
# Covered:
#   1. Semantics of an inlined call: values, multiple values, argument
#      order and single evaluation, parameter shadowing, the implicit
#      BLOCK, declarations and docstrings, nested inlining, tail position,
#      unwind-protect / handler-case / loops inside the body, arity errors
#      (the same PROGRAM-ERROR as the closure call).
#   2. Definition-environment name resolution (CLHS 3.1.1): variables,
#      local functions, macrolet, symbol-macrolet, BLOCK names and GO tags
#      bound between the definition and the call site are invisible to the
#      inlined body.
#   3. What keeps a closure (and still works): #'f, (declare (notinline f)),
#      a call from inside a nested closure, a special parameter or
#      declaration, recursion in LABELS, a large body called twice, an
#      (optimize (space > speed)) or (debug 3) policy, and the
#      AMILISP_NO_LOCAL_INLINE=1 switch — each pinned through DISASSEMBLE.
#   4. The analysis sees through macros, MACROLET macros and symbol-macros
#      (so does the BLOCK / TAGBODY exit analysis); a macro that expands
#      differently on each expansion yields a clear compile error, never a
#      call to an empty slot.
#   5. COMPILE-FILE round trip; a method body; the bench-prims row shape.

AMILISP="${1:-build/host/amilisp}"

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)

passed=0
failed=0
total=0
TMPDIR="${TMPDIR:-/tmp}"
WORK="$TMPDIR/amilisp_linline_$$"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

if [ ! -x "$AMILISP" ]; then
    echo "  SKIP  local inline: no binary at $AMILISP"
    echo "0 passed, 0 failed, 0 total"
    exit 0
fi

if [ -n "${AMILISP_GC_STRESS:-}" ]; then
    TAILN=400
else
    TAILN=200000
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
# 1. Semantics of an inlined call
# ---------------------------------------------------------------------------
cat > "$WORK/t1.lisp" <<'EOF'
(defun li-twice (a) (flet ((twice (x) (* x 2))) (+ (twice a) (twice (1+ a)))))
(format t "R1a ~S~%" (li-twice 3))
(format t "R1b ~S~%" (flet ((f (x) (values x (* x 10)))) (multiple-value-list (f 4))))
(format t "R1c ~S~%" (let ((r (flet ((f (x) (values x (* x 10)))) (f 4)))) r))
(format t "R1d ~S~%" (let ((log nil))
                       (flet ((f (a b) (list a b)))
                         (list (f (progn (push 1 log) :a) (progn (push 2 log) :b))
                               (reverse log)))))
(format t "R1e ~S~%" (let ((x 1)) (flet ((f (x) (* x 10))) (list (f 5) x))))
(format t "R1f ~S~%" (flet ((f (x) (return-from f (* x 3)) :no)) (f 2)))
(format t "R1g ~S~%" (flet ((f (x) (declare (ignore x)) "doc" :ok)) (f 1)))
(format t "R1h ~S~%" (flet ((f (x) "doc" (declare (ignore x)) :ok2)) (f 1)))
(format t "R1i ~S~%" (flet ((g (x) (1+ x))) (flet ((f (x) (g (g x)))) (f 1))))
(defun li-count-down (n) (flet ((step (k) (if (zerop k) :done (li-count-down (1- k))))) (step n)))
(format t "R1j ~S~%" (li-count-down TAILN))
(format t "R1k ~S~%" (let ((cleaned nil))
                       (flet ((f () (unwind-protect (values 1 2) (setq cleaned t))))
                         (list (multiple-value-list (f)) cleaned))))
(format t "R1l ~S~%" (flet ((f (x) (handler-case (error "boom ~a" x)
                                     (error (e) (list :caught x (search "boom" (princ-to-string e)))))))
                       (f 3)))
(format t "R1m ~S~%" (flet ((sum-to (n) (let ((s 0)) (dotimes (i n s) (incf s i)))))
                       (list (sum-to 4) (sum-to 5))))
(format t "R1n ~S~%" (handler-case (flet ((f (x) x)) (f 1 2))
                       (program-error (e) (list :perr (princ-to-string e)))))
(format t "R1o ~S~%" (handler-case (flet ((f (x) x)) (f))
                       (program-error (e) (list :perr (princ-to-string e)))))
(format t "R1p ~S~%" (let ((count 0)) (flet ((bump () (incf count))) (bump) (bump) count)))
(format t "R1q ~S~%" (flet ((f (x) (setq x (1+ x)) (funcall (lambda () x)))) (f 1)))
(format t "R1r ~S~%" (labels ((leaf (x) (* x 2))
                              (rec (n acc) (if (zerop n) acc (rec (1- n) (+ acc (leaf n))))))
                       (rec 3 0)))
(format t "R1s ~S~%" (flet ((f () :zero)) (f)))
(format t "R1t ~S~%" (flet ((f (a b c d) (list d c b a))) (f 1 2 3 4)))
(format t "R1u ~S~%" (flet ((f (x) (if (> x 0) (f-global x) :neg))) (f -1)))
(defun li-arg-nlx (l)
  (block b (flet ((f (x) (* x 2))) (list 1 (f (if (car l) (return-from b :out) 5))))))
(format t "R1v ~S~%" (list (li-arg-nlx '(t)) (li-arg-nlx '(nil))))
(format t "R1w ~S~%" (flet ((f (x) (catch 'tag (throw 'tag (list :thrown x))))) (f 7)))
(format t "T1-DONE~%")
EOF
sed -i.bak "s/TAILN/$TAILN/" "$WORK/t1.lisp"
out=$(run "$WORK/t1.lisp")
check_contains "inlined helper, two call sites"                  "R1a 14" "$out"
check_contains "multiple values pass through"                    "R1b (4 40)" "$out"
check_contains "single value in a LET init"                      "R1c 4" "$out"
check_contains "arguments evaluated once, left to right"         "R1d ((:A :B) (1 2))" "$out"
check_contains "parameter shadows an outer variable"             "R1e (50 1)" "$out"
check_contains "implicit BLOCK: RETURN-FROM the function"        "R1f 6" "$out"
check_contains "declaration then docstring"                      "R1g :OK" "$out"
check_contains "docstring then declaration"                      "R1h :OK2" "$out"
check_contains "nested inlining (f calls inlined g)"             "R1i 3" "$out"
check_contains "inlined body in tail position"                   "R1j :DONE" "$out"
check_contains "unwind-protect inside the body"                  "R1k ((1 2) T)" "$out"
check_contains "handler-case inside the body"                    "R1l (:CAUGHT 3 0)" "$out"
check_contains "loop inside the body, two sites"                 "R1m (6 10)" "$out"
check_contains "too many arguments is a PROGRAM-ERROR"           "R1n (:PERR \"Too many arguments to F: expected 1, got 2\")" "$out"
check_contains "too few arguments is a PROGRAM-ERROR"            "R1o (:PERR \"Too few arguments to F: expected 1, got 0\")" "$out"
check_contains "body mutates a captured variable"                "R1p 2" "$out"
check_contains "parameter assigned and captured (boxed)"         "R1q 2" "$out"
check_contains "LABELS leaf inlined into recursive sibling"      "R1r 12" "$out"
check_contains "zero parameters"                                 "R1s :ZERO" "$out"
check_contains "four parameters, order kept"                     "R1t (4 3 2 1)" "$out"
check_contains "body calls a global of a similar name"           "R1u :NEG" "$out"
check_contains "non-local exit from an argument form"            "R1v (:OUT (1 10))" "$out"
check_contains "catch/throw inside the body"                     "R1w (:THROWN 7)" "$out"
check_contains "t1 completes"                                    "T1-DONE" "$out"

# ---------------------------------------------------------------------------
# 2. Definition-environment resolution
# ---------------------------------------------------------------------------
cat > "$WORK/t2.lisp" <<'EOF'
(format t "R2a ~S~%" (let ((x 1)) (flet ((f () x)) (let ((x 2)) (list x (f))))))
(format t "R2b ~S~%" (flet ((g () 1)) (flet ((f () (g))) (flet ((g () 2)) (list (g) (f))))))
(format t "R2c ~S~%" (macrolet ((m () 1)) (flet ((f () (m))) (macrolet ((m () 2)) (list (m) (f))))))
(format t "R2d ~S~%" (symbol-macrolet ((s 1)) (flet ((f () s)) (symbol-macrolet ((s 2)) (list s (f))))))
(format t "R2e ~S~%" (block b (flet ((f () (return-from b 1))) (block b (f) 2) :not-reached)))
(format t "R2f ~S~%" (let (r)
                       (tagbody
                          (flet ((f () (go out)))
                            (tagbody (f) out (setq r :inner-out) (go end)))
                        out (setq r :outer-out)
                        end)
                       r))
(format t "R2g ~S~%" (let ((x 1))
                       (flet ((f () x))
                         (let ((x 2))
                           (flet ((g () (list x (f))))
                             (let ((x 3))
                               (list x (g))))))))
(format t "R2h ~S~%" (let ((x 1)) (flet ((f (x) (list x))) (let ((x 2)) (f (+ x 10))))))
(format t "R2i ~S~%" (flet ((f (a) (list a))) (let ((a 5)) (f (list a)))))
(defvar *li-sp* :global)
(defun li-read-sp () *li-sp*)
(format t "R2j ~S~%" (flet ((f (*li-sp*) (li-read-sp))) (list (f :bound) *li-sp*)))
(defun li-read-x () (symbol-value 'li-x))
(format t "R2k ~S~%" (flet ((f (v) (let ((li-x v)) (declare (special li-x)) (li-read-x)))) (f 9)))
(format t "R2l ~S~%" (let ((x 1)) (flet ((f () x)) (let ((x 2)) (declare (ignorable x)) (f)))))
(format t "T2-DONE~%")
EOF
out=$(run "$WORK/t2.lisp")
check_contains "variable bound after the definition is hidden"   "R2a (2 1)" "$out"
check_contains "local function bound after is hidden"            "R2b (2 1)" "$out"
check_contains "macrolet bound after is hidden"                  "R2c (2 1)" "$out"
check_contains "symbol-macrolet bound after is hidden"           "R2d (2 1)" "$out"
check_contains "BLOCK opened after is hidden (outer B wins)"     "R2e 1" "$out"
check_contains "GO tag bound after is hidden (outer OUT wins)"   "R2f :OUTER-OUT" "$out"
check_contains "two levels of inlining, three bindings of X"     "R2g (3 (2 1))" "$out"
check_contains "argument sees the call-site X, body its param"   "R2h (12)" "$out"
check_contains "argument sees a binding the body must not"       "R2i ((5))" "$out"
check_contains "special parameter keeps a dynamic binding"       "R2j (:BOUND :GLOBAL)" "$out"
check_contains "special binding inside an inlined body"          "R2k 9" "$out"
check_contains "hidden binding with a declaration"               "R2l 1" "$out"
check_contains "t2 completes"                                    "T2-DONE" "$out"

# ---------------------------------------------------------------------------
# 3. What keeps a closure — pinned through DISASSEMBLE
# ---------------------------------------------------------------------------
cat > "$WORK/t3.lisp" <<'EOF'
(defun li-inl (a) (flet ((f (x) (* x 2))) (+ (f a) (f 1))))
(defun li-fref (a) (flet ((f (x) (* x 2))) (funcall #'f a)))
(defun li-noti (a) (flet ((f (x) (* x 2))) (declare (notinline f)) (f a)))
(defun li-noti2 (a) (flet ((f (x) (* x 2))) (locally (declare (notinline f)) (f a))))
(defun li-lam (l) (flet ((f (x) (* x 2))) (mapcar (lambda (y) (f y)) l)))
(defun li-hb (a) (flet ((f (x) (* x 2))) (handler-bind ((error (lambda (e) (declare (ignore e)) (f a)))) a)))
;; A LABELS member called from a sibling's body is called from inside a
;; closure (the sibling), so it keeps its closure; one called from the
;; LABELS body only is inlined.
(defun li-lab (n) (labels ((leaf (x) (* x 2))
                           (rec (n acc) (if (zerop n) acc (rec (1- n) (+ acc (leaf n))))))
                    (rec n 0)))
(defun li-lab2 (n) (labels ((leaf (x) (* x 2))
                            (rec (n acc) (if (zerop n) acc (rec (1- n) (+ acc n)))))
                     (+ (leaf 1) (rec n 0))))
(defun li-spdecl (a) (flet ((f (x) (declare (special x)) (* x 2))) (f a)))
(defun li-big2 (a)
  (flet ((f (x) (list (list x x x x x) (list x x x x x) (list x x x x x)
                      (list x x x x x) (list x x x x x) (list x x x x x)
                      (list x x x x x) (list x x x x x) (list x x x x x)
                      (list x x x x x) (list x x x x x) (list x x x x x))))
    (list (f a) (f (1+ a)))))
(defun li-big1 (a)
  (flet ((f (x) (list (list x x x x x) (list x x x x x) (list x x x x x)
                      (list x x x x x) (list x x x x x) (list x x x x x)
                      (list x x x x x) (list x x x x x) (list x x x x x)
                      (list x x x x x) (list x x x x x) (list x x x x x))))
    (f a)))
(defun li-space (a) (declare (optimize (space 2))) (flet ((f (x) (* x 2))) (f a)))
(defun li-debug (a) (declare (optimize (debug 3))) (flet ((f (x) (* x 2))) (f a)))
(defun li-opt (a) (flet ((f (x &optional (y 1)) (* x y))) (f a)))
(defun li-rest (a) (flet ((f (&rest r) r)) (f a)))
(defun li-setf-name (a) (flet (((setf li-acc) (v o) (list v o))) (setf (li-acc a) 1)))
;; A LOAD-TIME-VALUE in the body keeps the closure: an inlined body is
;; compiled — and the form evaluated — once per call site, and the value
;; must be one object per function (serapeum's STATIC-LET is built on
;; that).  The scan finds it through a macro and inside a nested lambda.
(defun li-ltv (a) (flet ((f (x) (cons x (load-time-value (list :ltv))))) (list (cdr (f a)) (cdr (f 1)))))
(defmacro li-ltv-mac () '(load-time-value (list :ltv)))
(defun li-ltv-hidden (a) (flet ((f (x) (cons x (li-ltv-mac)))) (list (cdr (f a)) (cdr (f 1)))))
(defun li-ltv-nested (a)
  (flet ((f (x) (funcall (lambda () (cons x (load-time-value (list :ltv)))))))
    (list (cdr (f a)) (cdr (f 1)))))
(dolist (fn '(li-inl li-fref li-noti li-noti2 li-lam li-hb li-lab li-lab2 li-spdecl
              li-big2 li-big1 li-space li-debug li-opt li-rest li-setf-name
              li-ltv li-ltv-hidden li-ltv-nested))
  (format t "~%=== DIS ~A ===~%" fn)
  (disassemble fn))
(format t "~%R3 ~S~%" (list (li-inl 3) (li-fref 3) (li-noti 3) (li-noti2 3) (li-lam '(1 2))
                            (li-hb 3) (li-lab 3) (li-lab2 3) (li-spdecl 3)
                            (length (li-big2 1)) (length (li-big1 1))
                            (li-space 3) (li-debug 3) (li-opt 3) (li-rest 3)))
(format t "R3ltv ~S~%" (flet ((same (r) (eq (first r) (second r))))
                         (list (same (li-ltv 5)) (same (li-ltv-hidden 5)) (same (li-ltv-nested 5)))))
;; serapeum's STATIC-LOAD-TIME-VALUE identity probe, and STATIC-LET's
;; initialize-once pattern: the init form runs on the first call only.
(format t "R3ltv2 ~S~%" (funcall (compile nil '(lambda ()
                                                (flet ((fn () (load-time-value (random most-positive-fixnum))))
                                                  (eql (fn) (fn)))))))
(defun li-ltv-once ()
  (let ((x 0))
    (flet ((foo () (let ((cell (load-time-value (list nil))))
                     (unless (car cell) (incf x) (setf (car cell) t))
                     42)))
      (foo) (foo) (foo)
      x)))
(format t "R3ltv3 ~S~%" (li-ltv-once))
(format t "T3-DONE~%")
EOF
out=$(run "$WORK/t3.lisp")
dis() { echo "$out" | awk -v fn="=== DIS $1 ===" '$0 == fn {p=1; next} /^=== DIS / {p=0} p'; }
check_absent   "inlined: no CLOSURE in the caller"               "CLOSURE" "$(dis LI-INL)"
check_absent   "inlined: no CALL of the helper"                  "CALL" "$(dis LI-INL)"
check_contains "#'f keeps a closure"                             "CLOSURE" "$(dis LI-FREF)"
check_contains "(declare (notinline f)) keeps a closure"         "CLOSURE" "$(dis LI-NOTI)"
check_contains "LOCALLY notinline keeps a closure"               "CLOSURE" "$(dis LI-NOTI2)"
check_contains "call from a nested LAMBDA keeps a closure"       "CLOSURE" "$(dis LI-LAM)"
check_contains "call from a HANDLER-BIND handler keeps a closure" "CLOSURE" "$(dis LI-HB)"
total=$((total + 1))
if [ "$(dis LI-LAB | grep -c CLOSURE)" = "2" ]; then
    echo "  ok    LABELS: a member called from a sibling keeps its closure"; passed=$((passed + 1))
else
    echo "  FAIL  LABELS: expected two CLOSUREs (got $(dis LI-LAB | grep -c CLOSURE))"; failed=$((failed + 1))
fi
total=$((total + 1))
if [ "$(dis LI-LAB2 | grep -c CLOSURE)" = "1" ]; then
    echo "  ok    LABELS: leaf called from the body is inlined, recursive member kept"; passed=$((passed + 1))
else
    echo "  FAIL  LABELS: expected exactly one CLOSURE (got $(dis LI-LAB2 | grep -c CLOSURE))"; failed=$((failed + 1))
fi
check_contains "(declare (special param)) keeps a closure"       "CLOSURE" "$(dis LI-SPDECL)"
check_contains "large body with two call sites keeps a closure"  "CLOSURE" "$(dis LI-BIG2)"
check_absent   "large body with one call site is inlined"        "CLOSURE" "$(dis LI-BIG1)"
check_contains "(optimize (space 2)) keeps a closure"            "CLOSURE" "$(dis LI-SPACE)"
check_contains "(optimize (debug 3)) keeps a closure"            "CLOSURE" "$(dis LI-DEBUG)"
check_contains "&optional keeps a closure"                       "CLOSURE" "$(dis LI-OPT)"
check_contains "&rest keeps a closure"                           "CLOSURE" "$(dis LI-REST)"
check_contains "(setf name) keeps a closure"                     "CLOSURE" "$(dis LI-SETF-NAME)"
check_contains "every shape still computes the right value"      "R3 (8 6 6 6 (2 4) 3 12 8 6 2 12 6 6 3 (3))" "$out"
check_contains "load-time-value in the body keeps a closure"     "CLOSURE" "$(dis LI-LTV)"
check_contains "load-time-value behind a macro keeps a closure"  "CLOSURE" "$(dis LI-LTV-HIDDEN)"
check_contains "load-time-value in a nested lambda keeps a closure" "CLOSURE" "$(dis LI-LTV-NESTED)"
check_contains "load-time-value is one object across call sites" "R3ltv (T T T)" "$out"
check_contains "load-time-value identity across calls (serapeum probe)" "R3ltv2 T" "$out"
check_contains "static-let pattern: init form runs once"         "R3ltv3 1" "$out"
check_contains "t3 completes"                                    "T3-DONE" "$out"

cat > "$WORK/t3b.lisp" <<'EOF'
(defun li-inl (a) (flet ((f (x) (* x 2))) (+ (f a) (f 1))))
(disassemble 'li-inl)
(format t "R3b ~S~%" (li-inl 3))
EOF
if [ -n "$TIMEOUT" ]; then
    out=$(AMILISP_NO_LOCAL_INLINE=1 "$TIMEOUT" 60 "$AMILISP" --no-userinit --non-interactive --load "$WORK/t3b.lisp" 2>&1)
else
    out=$(AMILISP_NO_LOCAL_INLINE=1 "$AMILISP" --no-userinit --non-interactive --load "$WORK/t3b.lisp" 2>&1)
fi
check_contains "AMILISP_NO_LOCAL_INLINE=1 keeps every closure"   "CLOSURE" "$out"
check_contains "... and the call still works"                    "R3b 8" "$out"

# ---------------------------------------------------------------------------
# 4. Macros and symbol-macros
# ---------------------------------------------------------------------------
cat > "$WORK/t4.lisp" <<'EOF'
(defmacro li-call-f (x) `(f ,x))
(defmacro li-ref-f () '#'f)
(defun li-mac-call (a) (flet ((f (x) (* x 3))) (li-call-f a)))
(defun li-mac-ref (a) (flet ((f (x) (* x 3))) (funcall (li-ref-f) a)))
(defun li-smac (a) (symbol-macrolet ((s #'f)) (flet ((f (x) (* x 3))) (funcall s a))))
(defun li-mac-lam (l) (flet ((f (x) (* x 3))) (mapcar (lambda (y) (li-call-f y)) l)))
(dolist (fn '(li-mac-call li-mac-ref li-smac li-mac-lam))
  (format t "~%=== DIS ~A ===~%" fn)
  (disassemble fn))
(format t "~%R4 ~S~%" (list (li-mac-call 2) (li-mac-ref 2) (li-smac 2) (li-mac-lam '(1 2))))
;; A macro whose expansion differs between the analysis and the compile.
;; Whatever the parity, the outcome must be a working call or the clean
;; compile-time diagnostic — never a call through an empty slot.
(defvar *li-flip* 0)
(defmacro li-flip () (if (evenp (incf *li-flip*)) '(f 1) '(funcall #'f 1)))
(dotimes (i 4)
  (format t "R4f ~S~%"
          (handler-case (eval '(flet ((f (x) (list :ok x))) (li-flip)))
            (error (e) (if (search "compiled inline" (princ-to-string e))
                           :clean-diagnostic
                           (princ-to-string e))))))
(format t "T4-DONE~%")
EOF
out=$(run "$WORK/t4.lisp")
dis() { echo "$out" | awk -v fn="=== DIS $1 ===" '$0 == fn {p=1; next} /^=== DIS / {p=0} p'; }
check_absent   "a call hidden in a macro is inlined"             "CLOSURE" "$(dis LI-MAC-CALL)"
check_contains "#'f hidden in a macro keeps a closure"           "CLOSURE" "$(dis LI-MAC-REF)"
check_contains "#'f hidden in a symbol-macro keeps a closure"    "CLOSURE" "$(dis LI-SMAC)"
check_contains "macro call inside a lambda keeps a closure"      "CLOSURE" "$(dis LI-MAC-LAM)"
check_contains "all four compute the right value"                "R4 (6 6 6 (3 6))" "$out"
check_absent   "flip-flop macro never reaches an empty slot"     "Not a function: NIL" "$out"
check_absent   "flip-flop macro never signals UNDEFINED-FUNCTION" "Undefined function" "$out"
total=$((total + 1))
nflip=$(echo "$out" | grep -c "R4f (:OK 1)\|R4f :CLEAN-DIAGNOSTIC")
if [ "$nflip" = "4" ]; then
    echo "  ok    flip-flop macro: working call or clean diagnostic, 4/4"; passed=$((passed + 1))
else
    echo "  FAIL  flip-flop macro: $nflip/4 acceptable outcomes"; echo "$out" | grep "R4f" | sed 's/^/      /'; failed=$((failed + 1))
fi
check_contains "t4 completes"                                    "T4-DONE" "$out"

# ---------------------------------------------------------------------------
# 4b. MACROLET: the analysis expands local macros like global ones
#     (flexi-streams' decoders: FILL-BUFFER called from the expansion of a
#     local macro, inside a nested FLET that keeps its closure)
# ---------------------------------------------------------------------------
cat > "$WORK/t4b.lisp" <<'EOF'
;; the call / the #' sits in the local macro's template
(defun li-ml-lam (n) (let ((p 0)) (flet ((f (e) (setq p e))) (macrolet ((via () '(f n))) (funcall (lambda () (via))) p))))
(defun li-ml-ref (n) (flet ((f (x) (* 2 x))) (macrolet ((fn () '#'f)) (mapcar (fn) (list n)))))
;; the local macro wraps its ARGUMENT in a closure
(defun li-ml-arg (n) (let ((p 0)) (flet ((f (e) (setq p e))) (macrolet ((delay (x) `(lambda () ,x))) (funcall (delay (f n))) p))))
;; the MACROLET encloses the FLET
(defun li-ml-out (n) (let ((p 0)) (macrolet ((via () '(f n))) (flet ((f (e) (setq p e))) (funcall (lambda () (via))) p))))
;; a direct call through a local macro is still inlined
(defun li-ml-inl (n) (flet ((f (x) (* 2 x))) (macrolet ((via (x) `(f ,x))) (+ (via n) (via 1)))))
;; symbol-macro inside the local macro's expansion (the flexi-streams shape)
(defun li-ml-flexi (n)
  (let ((pos 0) (log nil))
    (flet ((fill-it (end) (setq pos end) t))
      (fill-it 1)
      (macrolet ((iterate (tag)
                   `(symbol-macrolet ((getter (progn (fill-it n) pos)))
                      (flet ((get2 () (push (list ,tag getter getter getter getter getter getter
                                                  getter getter getter getter getter getter) log)
                               getter))
                        (list (get2) (get2))))))
        (list (iterate :a) (length log))))))
;; &environment and a local macro calling another one
(defun li-ml-chain (n)
  (flet ((f (x) (* 3 x)))
    (macrolet ((inner (x) `(f ,x))
               (outer (x &environment env) (macroexpand `(inner ,x) env)))
      (mapcar (lambda (y) (outer y)) (list n)))))
(dolist (fn '(li-ml-lam li-ml-ref li-ml-arg li-ml-out li-ml-inl li-ml-flexi li-ml-chain))
  (format t "~%=== DIS ~A ===~%" fn)
  (if (fboundp fn) (disassemble fn) (format t "UNDEFINED~%")))
(format t "~%R4b ~S~%" (list (li-ml-lam 5) (li-ml-ref 5) (li-ml-arg 6) (li-ml-out 7)
                             (li-ml-inl 3) (li-ml-flexi 9) (li-ml-chain 2)))
;; The same blind spot in the BLOCK / TAGBODY analysis: an exit hidden in a
;; local macro and crossing a closure needs the non-local path.
(defun li-ml-rf () (block b (macrolet ((ret () '(return-from b :ok))) (mapc (lambda (x) (declare (ignore x)) (ret)) '(1)) :wrong)))
(defun li-ml-ret () (block nil (macrolet ((ret () '(return :ok))) (mapc (lambda (x) (declare (ignore x)) (ret)) '(1)) :wrong)))
(defun li-ml-go ()
  (let ((n 0))
    (tagbody
       (macrolet ((wrap (x) `(funcall (lambda () ,x)))) (wrap (go out)))
       (setq n 1)
     out)
    n))
(defun li-ml-rf-out () (macrolet ((ret () '(return-from b :ok))) (block b (mapc (lambda (x) (declare (ignore x)) (ret)) '(1)) :wrong)))
(format t "R4c ~S~%" (list (li-ml-rf) (li-ml-ret) (li-ml-go) (li-ml-rf-out)))
;; A malformed binding is the compiler's error to report, not the scan's.
(format t "R4d ~S~%" (handler-case (eval '(flet ((f () 1)) (macrolet ((m)) (f))))
                       (error () :error)))
(format t "T4B-DONE~%")
EOF
out=$(run "$WORK/t4b.lisp")
dis() { echo "$out" | awk -v fn="=== DIS $1 ===" '$0 == fn {p=1; next} /^=== DIS / {p=0} p'; }
check_absent   "no inlining diagnostic for a local macro"        "compiled inline" "$out"
check_contains "call in a local macro, from a lambda: closure"   "CLOSURE" "$(dis LI-ML-LAM)"
check_contains "#'f in a local macro keeps a closure"            "CLOSURE" "$(dis LI-ML-REF)"
check_contains "local macro wraps its argument in a lambda"      "CLOSURE" "$(dis LI-ML-ARG)"
check_contains "MACROLET around the FLET"                        "CLOSURE" "$(dis LI-ML-OUT)"
check_absent   "direct call through a local macro is inlined"    "CLOSURE" "$(dis LI-ML-INL)"
check_contains "local macros compute the right values"           "R4b (5 (10) 6 7 8 ((9 9) 2) (6))" "$out"
check_contains "RETURN-FROM / RETURN / GO hidden in a local macro" "R4c (:OK :OK 0 :OK)" "$out"
check_contains "malformed MACROLET binding still answers"        "R4d " "$out"
check_contains "t4b completes"                                   "T4B-DONE" "$out"

# ---------------------------------------------------------------------------
# 5. COMPILE-FILE, methods, the bench row
# ---------------------------------------------------------------------------
cat > "$WORK/t5src.lisp" <<'EOF'
(defun li-cf (a) (flet ((twice (x) (* x 2))) (+ (twice a) (twice (1+ a)))))
(defun li-cf-hide (a) (let ((x a)) (flet ((f () x)) (let ((x (* a 100))) (list x (f))))))
;; COMPILE-FILE mints one memo cell per compiled LOAD-TIME-VALUE: an inlined
;; body would get one per call site.  The veto keeps the closure here too.
(defun li-cf-ltv (a) (flet ((f (x) (cons x (load-time-value (list :cf-ltv))))) (eq (cdr (f a)) (cdr (f 1)))))
EOF
cat > "$WORK/t5.lisp" <<EOF
(let ((fasl (compile-file "$WORK/t5src.lisp")))
  (load fasl))
(format t "R5a ~S~%" (list (li-cf 3) (li-cf-hide 2)))
(format t "R5ltv ~S~%" (li-cf-ltv 3))
(disassemble 'li-cf)
(defgeneric li-gf (x))
(defmethod li-gf ((x integer)) (flet ((f (y) (+ x y))) (list (f 1) (f 2))))
(format t "R5b ~S~%" (li-gf 10))
(defun li-bench (i) (flet ((h (x) (if x i x))) (h i)))
(format t "R5c ~S~%" (li-bench 7))
(disassemble 'li-bench)
(format t "T5-DONE~%")
EOF
out=$(run "$WORK/t5.lisp")
check_contains "compile-file round trip"                         "R5a (14 (200 2))" "$out"
check_contains "compile-file: load-time-value one object per function" "R5ltv T" "$out"
check_absent   "compile-file output has no closure for the helper" "CLOSURE" "$out"
check_contains "flet inside a method body"                       "R5b (11 12)" "$out"
check_contains "bench-prims flet-call shape"                     "R5c 7" "$out"
check_contains "t5 completes"                                    "T5-DONE" "$out"

echo "$passed passed, $failed failed, $total total"
[ "$failed" -eq 0 ]
