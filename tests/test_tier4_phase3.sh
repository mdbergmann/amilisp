#!/bin/sh
# Regression tests for Tier-4 phase 3 — superinstructions
# (specs/performance.md 4.3).  The peephole pass now runs at every speed
# above 0 and, after its rewrites, fuses fourteen adjacent opcode pairs and
# triples into one dispatch each; three rewrites came with it (a dead STORE
# before RET, a JMP whose target is RET, a RETURN-FROM site returning in
# place).  Every case pins the behaviour those changes had to
# keep, and the shapes are pinned through DISASSEMBLE so a later change to
# the compiler's emission or to the fusion table cannot silently lose them.
#
#   sh tests/test_tier4_phase3.sh build/host/amilisp
#
# Covered:
#   1. Shapes: each fused opcode appears for its source pattern at the
#      default speed; none appears with CLAMIGA_FORCE_SPEED=0; with
#      CLAMIGA_NO_FUSE=1 the pairs stay apart while the rewrites still run;
#      the function-end STORE and the JMP-to-RET are gone.
#   2. Semantics of every fused opcode, including the error paths that must
#      survive fusion (a struct accessor on a non-struct, an unbound special,
#      an undefined or mis-called function) and the multiple-values contract
#      of a returned local.
#   3. LOAD_CALL_GLOBAL is still a global call: redefinition reaches the
#      site, TRACE sees it, the arity error names the callee.
#   4. GLOAD_JNIL reads the dynamic binding (LET, another thread), not just
#      the global cell.
#   5. Backtrace lines: an error raised inside a fused opcode is reported
#      at the line of the form.
#   6. A HANDLER-CASE with several clauses keeps its whole landing table
#      (the regression the pass first hit at speed 1).
#   7. COMPILE-FILE round trip: the FASL carries the fused opcodes and the
#      loaded functions behave the same.
#   8. The fused paths under allocation pressure (loops), also under
#      CLAMIGA_GC_STRESS.

CLAMIGA="${1:-build/host/amilisp}"

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)

passed=0
failed=0
total=0
TMPDIR="${TMPDIR:-/tmp}"
WORK="$TMPDIR/clamiga_tier4p3_$$"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

if [ ! -x "$CLAMIGA" ]; then
    echo "  SKIP  tier4 phase3: no binary at $CLAMIGA"
    echo "0 passed, 0 failed, 0 total"
    exit 0
fi

if [ -n "${CLAMIGA_GC_STRESS:-}" ]; then
    LOOPN=60
else
    LOOPN=20000
fi

run() {
    if [ -n "$TIMEOUT" ]; then
        "$TIMEOUT" 120 "$CLAMIGA" --no-userinit --heap 64M \
            --non-interactive --load "$1" 2>&1 </dev/null
    else
        "$CLAMIGA" --no-userinit --heap 64M --non-interactive --load "$1" 2>&1 </dev/null
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
# 1. Shapes
# ---------------------------------------------------------------------------
cat > "$WORK/shapes.lisp" <<'EOF'
(defstruct p3-pt x y)
(defvar *p3-flag* nil)
(defun p3-shape (x y p)
  (let ((a (car x)) (b (cdr y)))
    (if (eq a b)
        (list a b (p3-pt-x p))
        (if *p3-flag* (p3-pt-y p) (car (list (cdr x) a))))))
(disassemble 'p3-shape)
(defun p3-param (x) x)
(disassemble 'p3-param)
(defun p3-early (x) (when (car x) (return-from p3-early :early)) (cdr x))
(disassemble 'p3-early)
(defun p3-shape2 (a b)
  (let ((c a) (d 0))
    (setq d c)
    (setq d c)   ; the first SETQ finds C on top of the stack; this one loads it
    (list (if (eq b *p3-flag*) (list c :k) (cadr (list d *p3-flag*)))
          (progn (list 1) d))))
(disassemble 'p3-shape2)
(format t "S-DONE~%")
EOF
out=$(run "$WORK/shapes.lisp")
check_contains "STORE_POP for a LET binding"                    "STORE_POP" "$out"
check_contains "LOAD_LOAD for adjacent locals"                  "LOAD_LOAD" "$out"
check_contains "LOAD_CALL_GLOBAL for a local as last argument"  "LOAD_CALL_GLOBAL" "$out"
check_contains "LOAD_STRUCT_REF for an accessor on a local"     "LOAD_STRUCT_REF" "$out"
check_contains "EQ_JNIL for an EQ test"                         "EQ_JNIL" "$out"
check_contains "GLOAD_JNIL for a special-variable test"         "GLOAD_JNIL" "$out"
check_contains "LOAD_JNIL for a local test"                     "LOAD_JNIL" "$out"
check_contains "LOAD_MV_RESET for a returned local"             "LOAD_MV_RESET" "$out"
check_contains "LOAD_STORE_POP for a local-to-local SETQ"       "LOAD_STORE_POP" "$out"
check_contains "LOAD_CONST for a local then a constant"         "LOAD_CONST" "$out"
check_contains "GLOAD_EQ_JNIL for an EQ against a special"      "GLOAD_EQ_JNIL" "$out"
check_contains "GLOAD_CALL_GLOBAL for a special as last argument" "GLOAD_CALL_GLOBAL" "$out"
check_contains "POP_LOAD for a discarded value then a local"    "POP_LOAD" "$out"
check_absent   "no STORE left before the final RET"             "STORE  *[0-9]*$" "$(echo "$out" | grep -B1 'RET$' | grep -v RET)"
check_absent   "RETURN-FROM site returns in place: no JMP"      ": JMP" "$(echo "$out" | sed -n '/P3-EARLY/,/P3-SHAPE2/p')"
check_absent   "RETURN-FROM site returns in place: no STORE"    ": STORE" "$(echo "$out" | sed -n '/P3-EARLY/,/P3-SHAPE2/p')"
check_contains "shapes run to completion"                       "S-DONE" "$out"

out0=$(CLAMIGA_FORCE_SPEED=0 run "$WORK/shapes.lisp")
check_absent   "speed 0: no STORE_POP"                          "STORE_POP" "$out0"
check_absent   "speed 0: no LOAD_LOAD"                          "LOAD_LOAD" "$out0"
check_absent   "speed 0: no LOAD_CALL_GLOBAL"                   "LOAD_CALL_GLOBAL" "$out0"
check_absent   "speed 0: no LOAD_STRUCT_REF"                    "LOAD_STRUCT_REF" "$out0"
check_absent   "speed 0: no EQ_JNIL / GLOAD_JNIL / LOAD_JNIL"   "_JNIL" "$out0"
check_absent   "speed 0: no round-two opcodes"                  "LOAD_STORE_POP\|LOAD_CONST\|GLOAD_CALL_GLOBAL\|POP_LOAD" "$out0"
check_contains "speed 0: the function-end STORE is as emitted"  "STORE  *[0-9]*$" "$out0"
check_contains "speed 0: the RETURN-FROM JMP is as emitted"     ": JMP" "$(echo "$out0" | sed -n '/P3-EARLY/,/P3-SHAPE2/p')"

outn=$(CLAMIGA_NO_FUSE=1 run "$WORK/shapes.lisp")
check_absent   "NO_FUSE: pairs stay apart"                      "STORE_POP\|LOAD_LOAD\|LOAD_CALL_GLOBAL\|LOAD_STRUCT_REF\|_JNIL\|LOAD_MV_RESET\|LOAD_RET\|LOAD_CONST\|GLOAD_CALL_GLOBAL\|POP_LOAD" "$outn"
check_absent   "NO_FUSE: the rewrites still ran (no JMP to RET)" ": JMP" "$(echo "$outn" | sed -n '/P3-EARLY/,/P3-SHAPE2/p')"

# ---------------------------------------------------------------------------
# 2. Semantics of every fused opcode
# ---------------------------------------------------------------------------
cat > "$WORK/sem.lisp" <<'EOF'
(defstruct p3-pt x y)
(defvar *p3-flag* nil)
(defvar *p3-unbound*)
(defun p3-bind (x y)
  (let ((a (car x)) (b (cdr x)) (c (car y)))
    (list (list a b c) (list c b a) (list b) (list a c))))
(format t "R2a ~S~%" (p3-bind '(1 . 2) '(3)))
(defun p3-slot (p) (list (p3-pt-x p) (p3-pt-y p)))
(format t "R2b ~S~%" (p3-slot (make-p3-pt :x 1 :y 2)))
(format t "R2c ~S~%" (handler-case (p3-slot 5) (type-error (e) (list :te (type-error-datum e)))))
(defun p3-ret-local (x) (let ((v (floor x 2))) v))
(format t "R2d ~S~%" (multiple-value-list (p3-ret-local 9)))
(defun p3-ret-param (x) x)
(format t "R2e ~S~%" (multiple-value-list (p3-ret-param (values 1 2))))
(defun p3-tests (a b)
  (list (if (eq a b) :eq :ne) (if a :a :not-a) (if *p3-flag* :flag :no-flag)
        (when (eq a 'k) :k) (and a b) (or a b)))
(format t "R2f ~S~%" (list (p3-tests 'k 'k) (p3-tests nil 'k) (p3-tests 1 2)))
(defun p3-unbound-test () (if *p3-unbound* 1 2))
(format t "R2g ~S~%" (handler-case (p3-unbound-test) (unbound-variable (e) (list :ub (and (search "*P3-UNBOUND*" (princ-to-string e)) t)))))
(defun p3-nil-test () (if nil 1 2))
(format t "R2h ~S~%" (p3-nil-test))
(defun p3-early (x)
  (when (car x) (return-from p3-early :early))
  (if (cdr x) (return-from p3-early (list :cdr (cdr x))))
  (setq x (cons :end x)))
(format t "R2i ~S~%" (list (p3-early '(t)) (p3-early '(nil . 5)) (p3-early '(nil))))
(defun p3-setq-value (x) (setq x (+ x 1)))
(format t "R2j ~S~%" (p3-setq-value 41))
(defun p3-tail-builtin (x) (car x))
(format t "R2k ~S~%" (p3-tail-builtin '(7)))
(defun p3-mv-through (x) (let ((v (p3-ret-param x))) (values v (list v))))
(format t "R2l ~S~%" (multiple-value-list (p3-mv-through 3)))
(defun p3-eq-objs (a b) (if (eq a b) :same :different))
(format t "R2m ~S~%" (let ((c (list 1))) (list (p3-eq-objs c c) (p3-eq-objs c (list 1)) (p3-eq-objs #\a #\a) (p3-eq-objs 'x 'x))))
(defun p3-loop-count (l) (let ((n 0)) (loop while l do (setq l (cdr l)) (incf n)) n))
(format t "R2n ~S~%" (p3-loop-count '(1 2 3 4)))
(defun p3-block-nil (l) (dolist (x l) (when (eq x :stop) (return :stopped))))
(format t "R2o ~S~%" (list (p3-block-nil '(1 :stop 2)) (p3-block-nil '(1 2))))
(defun p3-two-locals-order (a b) (list a b (list b a)))
(format t "R2p ~S~%" (p3-two-locals-order 1 2))
(defun p3-set-then-read (x) (let ((a 1) (b 2)) (setq a x) (setq b a) (list a b)))
(format t "R2q ~S~%" (p3-set-then-read 9))
(defun p3-r2 (a b)
  (let ((c a) (d 0))
    (setq d c)
    (list (if (eq b *p3-flag*) (list c :k) (list c :n))
          (cadr (list d *p3-flag*)) (progn (list 1) d))))
(format t "R2r ~S~%" (list (p3-r2 1 nil) (p3-r2 2 :x) (let ((*p3-flag* :x)) (p3-r2 2 :x))))
(defun p3-r2u (a) (if (eq a *p3-unbound*) 1 2))
(defun p3-r2c () (car (list *p3-unbound*)))
(format t "R2s ~S~%" (list (handler-case (p3-r2u 1) (unbound-variable () :ub))
                           (handler-case (p3-r2c) (unbound-variable () :ub))))
(format t "SEM-DONE~%")
EOF
out=$(run "$WORK/sem.lisp")
check_contains "bindings and calls in every order"             'R2a ((1 2 3) (3 2 1) (2) (1 3))' "$out"
check_contains "struct slots through a local"                   'R2b (1 2)' "$out"
check_contains "accessor on a non-struct is the type error"     'R2c (:TE 5)' "$out"
check_contains "a returned LET local is one value"              'R2d (4)' "$out"
check_contains "a returned parameter is one value"              'R2e (1)' "$out"
check_contains "EQ / local / special tests"                     'R2f ((:EQ :A :NO-FLAG :K K K) (:NE :NOT-A :NO-FLAG NIL NIL K) (:NE :A :NO-FLAG NIL 2 1))' "$out"
check_contains "unbound special in a test is the unbound error" 'R2g (:UB T)' "$out"
check_contains "constant NIL test"                              'R2h 2' "$out"
check_contains "RETURN-FROM landings and the block result"      'R2i (:EARLY (:CDR 5) (:END NIL))' "$out"
check_contains "the value of a SETQ is returned"                'R2j 42' "$out"
check_contains "builtin tail call returns its value"            'R2k 7' "$out"
check_contains "values through a local"                         'R2l (3 (3))' "$out"
check_contains "EQ on objects and immediates"                   'R2m (:SAME :DIFFERENT :SAME :SAME)' "$out"
check_contains "LOAD_JNIL as a loop test"                       'R2n 4' "$out"
check_contains "RETURN from a DOLIST"                           'R2o (:STOPPED NIL)' "$out"
check_contains "argument order through LOAD_LOAD"               'R2p (1 2 (2 1))' "$out"
check_contains "SETQ then read"                                 'R2q (9 9)' "$out"
check_contains "round two: local copy, eq against a special, special as argument" 'R2r (((1 :K) NIL 1) ((2 :N) NIL 2) ((2 :K) :X 2))' "$out"
check_contains "round two: unbound special in EQ test and as argument" 'R2s (:UB :UB)' "$out"
check_contains "semantics run to completion"                    "SEM-DONE" "$out"

# ---------------------------------------------------------------------------
# 3. LOAD_CALL_GLOBAL is still a global call
# ---------------------------------------------------------------------------
cat > "$WORK/call.lisp" <<'EOF'
(defun p3-callee (a) (list :v1 a))
(defun p3-caller (a) (p3-callee a))
(format t "R3a ~S~%" (p3-caller 1))
(defun p3-callee (a) (list :v2 a))
(format t "R3b ~S~%" (p3-caller 2))
(defun p3-undef-caller (a) (p3-no-such-function a))
(format t "R3c ~S~%" (handler-case (p3-undef-caller 1) (undefined-function (e) (list :uf (and (search "P3-NO-SUCH-FUNCTION" (princ-to-string e)) t)))))
(defun p3-arity-caller (a) (p3-callee a a))
(format t "R3d ~S~%" (handler-case (p3-arity-caller 1) (program-error (e) (list :perr (and (search "P3-CALLEE" (princ-to-string e)) t)))))
(defun p3-mv-callee (a) (values a (* 2 a)))
(defun p3-mv-caller (a) (multiple-value-list (p3-mv-callee a)))
(format t "R3e ~S~%" (p3-mv-caller 2))
(trace p3-callee)
(p3-caller 3)
(untrace p3-callee)
(defun p3-tail-caller (a) (p3-callee a))
(format t "R3f ~S~%" (p3-tail-caller 4))
(defun p3-count-down (n) (if (= n 0) :done (p3-count-down (1- n))))
(format t "R3g ~S~%" (p3-count-down 100000))
(format t "CALL-DONE~%")
EOF
out=$(run "$WORK/call.lisp")
check_contains "fused call sees the definition"                 'R3a (:V1 1)' "$out"
check_contains "fused call sees the redefinition"               'R3b (:V2 2)' "$out"
check_contains "undefined function is catchable and named"      'R3c (:UF T)' "$out"
check_contains "arity error names the callee"                   'R3d (:PERR T)' "$out"
check_contains "multiple values pass through"                   'R3e (2 4)' "$out"
check_contains "TRACE sees the fused call"                      'P3-CALLEE.*3' "$out"
check_contains "tail position"                                  'R3f (:V2 4)' "$out"
check_contains "self tail call in constant space"               'R3g :DONE' "$out"
check_contains "calls run to completion"                        "CALL-DONE" "$out"

# ---------------------------------------------------------------------------
# 4. GLOAD_JNIL reads the dynamic binding
# ---------------------------------------------------------------------------
cat > "$WORK/dyn.lisp" <<'EOF'
(defvar *p3-dyn* nil)
(defun p3-dyn-test () (if *p3-dyn* :bound :global))
(format t "R4a ~S~%" (list (p3-dyn-test) (let ((*p3-dyn* t)) (p3-dyn-test)) (p3-dyn-test)))
(format t "R4b ~S~%" (let ((*p3-dyn* t))
                       (list (mp:join-thread (mp:make-thread (lambda () (p3-dyn-test))))
                             (mp:join-thread (mp:make-thread (lambda () (let ((*p3-dyn* nil)) (p3-dyn-test))))))))
(format t "R4c ~S~%" (progn (setq *p3-dyn* :g) (p3-dyn-test)))
(format t "DYN-DONE~%")
EOF
out=$(run "$WORK/dyn.lisp")
check_contains "LET binding seen, unbinding seen"               'R4a (:GLOBAL :BOUND :GLOBAL)' "$out"
check_contains "a thread sees the global, its own LET binding"  'R4b (:GLOBAL :GLOBAL)' "$out"
check_contains "SETQ of the global cell seen"                   'R4c :BOUND' "$out"
check_contains "dynamic tests run to completion"                "DYN-DONE" "$out"

# ---------------------------------------------------------------------------
# 5. Backtrace lines — every line number below is asserted, keep the layout
# ---------------------------------------------------------------------------
cat > "$WORK/lines.lisp" <<'EOF'
;; written by tests/test_tier4_phase3.sh -- the line numbers are asserted
(defstruct p3-bt-pt x)
(defun p3-bt-slot (p)
  (list 1)
  (p3-bt-pt-x p))

(defun p3-bt-outer (p)
  (list (p3-bt-slot p)
        (identity 2)))

(p3-bt-outer 5)
EOF
out=$(run "$WORK/lines.lisp")
check_contains "error inside LOAD_STRUCT_REF at the form's line" '0: P3-BT-SLOT (.*lines\.lisp:5)' "$out"
# Under forced compaction the suspended caller's frame reports no line at
# all — on the baseline binary too (pre-existing, unrelated to fusion), so
# that check is host-only.
if [ -z "${CLAMIGA_GC_STRESS:-}" ]; then
check_contains "suspended caller at its LOAD_CALL_GLOBAL line"  '1: P3-BT-OUTER (.*lines\.lisp:8)' "$out"
fi

# ---------------------------------------------------------------------------
# 6. HANDLER-CASE landing tables survive the pass
# ---------------------------------------------------------------------------
cat > "$WORK/hc.lisp" <<'EOF'
(defun p3-hc3 (k)
  (handler-case (case k (0 (warn "w")) (1 (error "e")) (t (signal 'condition)))
    (warning () :warning)
    (error () :error)
    (condition () :condition)))
(format t "R6a ~S~%" (list (p3-hc3 0) (p3-hc3 1) (p3-hc3 2)))
(defun p3-hc-last (x)
  (handler-case (if x (warn "w") (error "e"))
    (warning () :w)
    (error () :e)))
(format t "R6b ~S~%" (list (p3-hc-last t) (p3-hc-last nil)))
(defun p3-hc-nested (x)
  (handler-case
      (handler-case (if x (error "inner") (warn "w"))
        (warning () :inner-w))
    (error () :outer-e)
    (warning () :outer-w)))
(format t "R6c ~S~%" (list (p3-hc-nested t) (p3-hc-nested nil)))
(defun p3-hc-values (x)
  (handler-case (if x (error "e") (values 1 2))
    (error () (values :a :b :c))
    (warning () :w)))
(format t "R6d ~S~%" (list (multiple-value-list (p3-hc-values t)) (multiple-value-list (p3-hc-values nil))))
(format t "HC-DONE~%")
EOF
out=$(run "$WORK/hc.lisp")
check_contains "three clauses, each reached"                    'R6a (:WARNING :ERROR :CONDITION)' "$out"
check_contains "two clauses at the end of a function"           'R6b (:W :E)' "$out"
check_contains "nested handler-cases"                           'R6c (:OUTER-E :INNER-W)' "$out"
check_contains "clause values"                                  'R6d ((:A :B :C) (1 2))' "$out"
check_contains "handler-case tests run to completion"           "HC-DONE" "$out"

# ---------------------------------------------------------------------------
# 7. COMPILE-FILE round trip
# ---------------------------------------------------------------------------
cat > "$WORK/cf-src.lisp" <<'EOF'
(defstruct p3-cf-pt x y)
(defvar *p3-cf-flag* nil)
(defun p3-cf (x y p)
  (let ((a (car x)) (b (cdr y)))
    (if (eq a b) (list a b (p3-cf-pt-x p))
        (if *p3-cf-flag* (p3-cf-pt-y p) (if a a b)))))
(defun p3-cf-hc (x)
  (handler-case (if x (warn "w") (error "e")) (warning () :w) (error () :e)))
(defun p3-cf-r2 (a b) (let ((c a)) (if (eq b *p3-cf-flag*) (list c :k) (cadr (list c *p3-cf-flag*)))))
EOF
cat > "$WORK/cf.lisp" <<EOF
(compile-file "$WORK/cf-src.lisp" :output-file "$WORK/cf-src.fasl")
(load "$WORK/cf-src.fasl")
(disassemble 'p3-cf)
(disassemble 'p3-cf-r2)
(format t "R7b ~S~%" (list (p3-cf-r2 1 nil) (p3-cf-r2 1 :x)))
(let ((p (make-p3-cf-pt :x 3 :y 4)))
  (format t "R7a ~S~%" (list (p3-cf '(1) '(2 . 1) p) (p3-cf '(1) '(2 . 2) p)
                             (let ((*p3-cf-flag* t)) (p3-cf '(1) '(2 . 2) p))
                             (p3-cf-hc t) (p3-cf-hc nil))))
(format t "CF-DONE~%")
EOF
out=$(run "$WORK/cf.lisp")
check_contains "FASL carries LOAD_STRUCT_REF"                   "LOAD_STRUCT_REF" "$out"
check_contains "FASL carries EQ_JNIL"                           "EQ_JNIL" "$out"
check_contains "FASL carries GLOAD_JNIL"                        "GLOAD_JNIL" "$out"
check_contains "loaded functions behave the same"               'R7a ((1 1 3) 1 4 :W :E)' "$out"
check_contains "FASL carries GLOAD_EQ_JNIL"                     "GLOAD_EQ_JNIL" "$out"
check_contains "FASL carries GLOAD_CALL_GLOBAL"                 "GLOAD_CALL_GLOBAL" "$out"
check_contains "loaded round-two function behaves the same"     'R7b ((1 :K) NIL)' "$out"
check_contains "compile-file round trip completes"              "CF-DONE" "$out"

# ---------------------------------------------------------------------------
# 8. The fused paths under allocation pressure
# ---------------------------------------------------------------------------
cat > "$WORK/gc.lisp" <<EOF
(defstruct p3-gc-pt x y)
(defvar *p3-gc-flag* t)
(defun p3-gc-step (p acc)
  (let ((a (p3-gc-pt-x p)) (b (p3-gc-pt-y p)))
    (if (eq a b) (cons (list a b) acc)
        (if *p3-gc-flag* (cons (make-p3-gc-pt :x b :y a) acc) acc))))
(defun p3-gc-run (n)
  (let ((acc nil) (p (make-p3-gc-pt :x 1 :y 2)))
    (dotimes (i n) (setq acc (p3-gc-step p acc)) (when (> (length acc) 50) (setq acc nil)))
    (length acc)))
(format t "R8a ~S~%" (p3-gc-run $LOOPN))
(defun p3-gc-eq (n)
  (let ((hits 0) (k 'k))
    (dotimes (i n) (let ((s (if (evenp i) 'k (list i)))) (when (eq s k) (incf hits))))
    hits))
(format t "R8b ~S~%" (p3-gc-eq $LOOPN))
(format t "GC-DONE~%")
EOF
out=$(run "$WORK/gc.lisp")
check_contains "struct access + special test in a consing loop"  "R8a $(( LOOPN % 51 ))" "$out"
check_contains "EQ_JNIL in a consing loop"                       "R8b $(( (LOOPN + 1) / 2 ))" "$out"
check_contains "allocation loops run to completion"              "GC-DONE" "$out"

echo "$passed passed, $failed failed, $total total"
[ "$failed" -eq 0 ]
