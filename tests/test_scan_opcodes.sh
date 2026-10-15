#!/bin/sh
# Regression tests for the string-scan fast path (specs/performance.md 4.4):
# five opcodes (AREF, CHAREQ, CMP_BR, PUSH_LOCAL, POP_LOCAL), the CASE
# branch shape and the INCF/DECF/PUSH macro shapes that came with them.
# Every shape is pinned through DISASSEMBLE so a later change to the
# compiler's emission or to the fusion table cannot silently lose it, and
# every opcode's semantics — including the error paths the builtin had —
# are checked against the HyperSpec.
#
#   sh tests/test_scan_opcodes.sh build/host/amilisp
#
# Covered:
#   1. Shapes at the default speed: AREF with the accessor kind for a
#      two-argument AREF/SVREF/CHAR/SCHAR (and NOT for other arities),
#      CHAREQ for a two-argument CHAR=, CMP_BR for every comparison under
#      IF/WHEN/UNLESS/DOTIMES (both polarities), PUSH_LOCAL/POP_LOCAL for
#      an unboxed lexical variable and NOT for a special, a captured or a
#      symbol-macro place, one EQ_JNIL per CASE key, INCF without
#      temporaries.  AMILISP_FORCE_SPEED=0 keeps the compiler-emitted
#      opcodes and drops the fusion; AMILISP_NO_FUSE=1 the same.
#   2. Semantics: every string / vector representation through AREF, the
#      type and bounds errors with the builtin's text, CHAR= on
#      non-characters, CMP_BR on fixnums / bignums / floats / ratios /
#      complex and on the fixnum boundaries, PUSH/POP value and order,
#      INCF/DECF on every kind of place.
#   3. COMPILE-FILE round trip: the FASL carries the opcodes.
#   4. The paths under allocation pressure (also under AMILISP_GC_STRESS).

AMILISP="${1:-build/host/amilisp}"

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)

passed=0
failed=0
total=0
TMPDIR="${TMPDIR:-/tmp}"
WORK="$TMPDIR/amilisp_scanops_$$"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

if [ ! -x "$AMILISP" ]; then
    echo "  SKIP  scan opcodes: no binary at $AMILISP"
    echo "0 passed, 0 failed, 0 total"
    exit 0
fi

if [ -n "${AMILISP_GC_STRESS:-}" ]; then
    LOOPN=40
else
    LOOPN=20000
fi

run() {
    if [ -n "$TIMEOUT" ]; then
        "$TIMEOUT" 120 "$AMILISP" --no-userinit --heap 64M \
            --non-interactive --load "$1" 2>&1 </dev/null
    else
        "$AMILISP" --no-userinit --heap 64M --non-interactive --load "$1" 2>&1 </dev/null
    fi
}

check_contains() {
    desc="$1"; pattern="$2"; actual="$3"
    total=$((total + 1))
    if printf '%s\n' "$actual" | grep -q -- "$pattern"; then
        echo "  ok  $desc"
        passed=$((passed + 1))
    else
        echo "  FAIL  $desc (expected /$pattern/)"
        printf '%s\n' "$actual" | tail -6 | sed 's/^/      /'
        failed=$((failed + 1))
    fi
}

check_absent() {
    desc="$1"; pattern="$2"; actual="$3"
    total=$((total + 1))
    if printf '%s\n' "$actual" | grep -q -- "$pattern"; then
        echo "  FAIL  $desc (saw /$pattern/)"
        printf '%s\n' "$actual" | grep -- "$pattern" | head -3 | sed 's/^/      /'
        failed=$((failed + 1))
    else
        echo "  ok  $desc"
        passed=$((passed + 1))
    fi
}

# ---------------------------------------------------------------------------
# 1. Shapes
# ---------------------------------------------------------------------------
cat > "$WORK/shapes.lisp" <<'EOF'
(defvar *sh-stack* nil)
(defun sh-schar (s i) (schar s i))
(defun sh-char (s i) (char s i))
(defun sh-aref (v i) (aref v i))
(defun sh-svref (v i) (svref v i))
(defun sh-aref2 (a i j) (aref a i j))
(defun sh-chareq (a b) (char= a b))
(defun sh-chareq3 (a b c) (char= a b c))
(defun sh-lt (a b) (if (< a b) 1 2))
(defun sh-gt (a b) (if (> a b) 1 2))
(defun sh-le (a b) (if (<= a b) 1 2))
(defun sh-ge (a b) (if (>= a b) 1 2))
(defun sh-numeq (a b) (if (= a b) 1 2))
(defun sh-chareq-br (a b) (if (char= a b) 1 2))
(defun sh-unless-lt (a b) (unless (< a b) 1))
(defun sh-not-lt (a b) (if (not (< a b)) 1 2))
(defun sh-dotimes (n) (let ((c 0)) (dotimes (i n) (setq c (+ c i))) c))
(defun sh-push (x) (let ((s nil)) (push x s) (push 2 s) s))
(defun sh-pop (l) (pop l))
(defun sh-push-special (x) (push x *sh-stack*))
(defun sh-push-captured (x) (let ((s nil)) (list (lambda () (push x s)) (lambda () s))))
(defun sh-push-sm (c) (symbol-macrolet ((s (car c))) (push 1 s)) c)
(defun sh-incf (i) (incf i) (decf i 2) i)
(defun sh-case (x) (case x (1 :a) ((2 3) :b) (t :c)))
(dolist (f '(sh-schar sh-char sh-aref sh-svref sh-aref2 sh-chareq sh-chareq3
             sh-lt sh-gt sh-le sh-ge sh-numeq sh-chareq-br sh-unless-lt sh-not-lt sh-dotimes
             sh-push sh-pop sh-push-special sh-push-captured sh-push-sm sh-incf sh-case))
  (format t "=== ~A~%" f)
  (disassemble f))
(format t "SHAPES-DONE~%")
EOF
out=$(run "$WORK/shapes.lisp")
check_contains "shapes ran" "SHAPES-DONE" "$out"
sect() { printf '%s\n' "$out" | awk -v f="=== $1" '$0==f{p=1;next} /^=== /{p=0} p'; }
check_contains "SCHAR -> AREF 3"          "AREF         3" "$(sect SH-SCHAR)"
check_contains "CHAR -> AREF 2"           "AREF         2" "$(sect SH-CHAR)"
check_contains "AREF -> AREF 0"           "AREF         0" "$(sect SH-AREF)"
check_contains "SVREF -> AREF 1"          "AREF         1" "$(sect SH-SVREF)"
check_absent   "3-arg AREF stays a call"  "AREF         " "$(sect SH-AREF2)"
check_contains "3-arg AREF calls AREF"    "CALL_GLOBAL.*AREF" "$(sect SH-AREF2)"
check_contains "CHAR= -> CHAREQ"          ": CHAREQ$" "$(sect SH-CHAREQ)"
check_absent   "3-arg CHAR= stays a call" ": CHAREQ$" "$(sect SH-CHAREQ3)"
check_contains "< ; JNIL -> CMP_BR 0"     "CMP_BR       0 " "$(sect SH-LT)"
check_contains "> ; JNIL -> CMP_BR 1"     "CMP_BR       1 " "$(sect SH-GT)"
check_contains "<= ; JNIL -> CMP_BR 2"    "CMP_BR       2 " "$(sect SH-LE)"
check_contains ">= ; JNIL -> CMP_BR 3"    "CMP_BR       3 " "$(sect SH-GE)"
check_contains "= ; JNIL -> CMP_BR 4"     "CMP_BR       4 " "$(sect SH-NUMEQ)"
check_contains "CHAREQ ; JNIL -> CMP_BR 5" "CMP_BR       5 " "$(sect SH-CHAREQ-BR)"
check_contains "UNLESS < : IF with a NIL arm -> CMP_BR 0" "CMP_BR       0 " "$(sect SH-UNLESS-LT)"
check_contains "NOT < : NOT;JNIL -> JTRUE -> CMP_BR 8" "CMP_BR       8 " "$(sect SH-NOT-LT)"
check_contains "DOTIMES >= ; JTRUE -> CMP_BR 11" "CMP_BR       11 " "$(sect SH-DOTIMES)"
check_absent   "no bare LT left under IF" "^ *[0-9]*: LT$" "$(sect SH-LT)"
check_contains "PUSH on a local -> PUSH_LOCAL" "PUSH_LOCAL" "$(sect SH-PUSH)"
check_absent   "PUSH on a local conses nothing by hand" ": CONS$" "$(sect SH-PUSH)"
check_contains "POP on a local -> POP_LOCAL"   "POP_LOCAL" "$(sect SH-POP)"
check_absent   "PUSH on a special: no PUSH_LOCAL" "PUSH_LOCAL" "$(sect SH-PUSH-SPECIAL)"
check_contains "PUSH on a special conses"         ": CONS$" "$(sect SH-PUSH-SPECIAL)"
check_absent   "PUSH on a captured variable: no PUSH_LOCAL" "PUSH_LOCAL" "$(sect SH-PUSH-CAPTURED)"
check_absent   "PUSH on a symbol-macro place: no PUSH_LOCAL" "PUSH_LOCAL" "$(sect SH-PUSH-SM)"
check_contains "INCF/DECF on a local: no temporaries (the block slot only)" "2 locals, 0 upvalues" "$(sect SH-INCF)"
check_contains "INCF on a local is an ADD"            "ADD" "$(sect SH-INCF)"
check_contains "CASE key test -> EQ_JNIL"  "EQ_JNIL" "$(sect SH-CASE)"
n_jtrue=$(sect SH-CASE | grep -c "JTRUE")
total=$((total + 1))
if [ "$n_jtrue" -eq 1 ]; then
    echo "  ok  CASE: only the non-last key of a multi-key clause uses JTRUE"
    passed=$((passed + 1))
else
    echo "  FAIL  CASE: expected 1 JTRUE, saw $n_jtrue"
    failed=$((failed + 1))
fi
check_absent   "CASE last key: no JTRUE-over-JMP" "JMP          +5 " "$(sect SH-CASE)"

out0=$(AMILISP_FORCE_SPEED=0 run "$WORK/shapes.lisp")
check_contains "speed 0: AREF still emitted by the compiler"  "AREF         3" "$out0"
check_contains "speed 0: CHAREQ still emitted"                "CHAREQ" "$out0"
check_contains "speed 0: PUSH_LOCAL still emitted"            "PUSH_LOCAL" "$out0"
check_absent   "speed 0: no CMP_BR (the fusion is the peephole's)" "CMP_BR" "$out0"
outnf=$(AMILISP_NO_FUSE=1 run "$WORK/shapes.lisp")
check_absent   "NO_FUSE: no CMP_BR"                            "CMP_BR" "$outnf"
check_contains "NO_FUSE: the LT stays"                         ": LT" "$outnf"

# ---------------------------------------------------------------------------
# 2. Semantics
# ---------------------------------------------------------------------------
cat > "$WORK/sem.lisp" <<'EOF'
(defmacro err-text (form)
  `(handler-case (progn ,form :no-error)
     (error (e) (let ((s (princ-to-string e)))
                  (subseq s 0 (min (length s) 60))))))
(defun r (name value) (format t "~A = ~S~%" name value))

;; --- AREF: every representation, through each accessor kind ---
(defun a-schar (s i) (schar s i))
(defun a-char (s i) (char s i))
(defun a-aref (v i) (aref v i))
(defun a-svref (v i) (svref v i))
(r "schar-base" (a-schar "hello" 1))
(r "char-base" (a-char "hello" 4))
(r "aref-string" (a-aref "hello" 0))
(let ((w (make-string 3 :initial-element (code-char 955))))
  (setf (char w 1) #\x)
  (r "schar-wide" (list (char-code (a-schar w 0)) (a-schar w 1)))
  (r "aref-wide" (char-code (a-aref w 2))))
(let ((fp (make-array 5 :element-type 'character :fill-pointer 3
                        :initial-contents "abcde")))
  (r "char-fill-pointer" (char-code (a-char fp 1)))
  (r "schar-fill-pointer" (err-text (a-schar fp 1)))
  (r "char-past-fill-pointer" (err-text (a-char fp 4)))
  (r "aref-past-fill-pointer" (a-aref fp 4)))
(r "svref" (a-svref (vector 1 2 3) 2))
(r "svref-string" (err-text (a-svref "abc" 0)))
(r "aref-vector" (a-aref (vector 1 2 3) 0))
(r "aref-bit" (list (a-aref #*1011 0) (a-aref #*1011 1)))
(r "aref-byte" (let ((b (make-array 4 :element-type '(unsigned-byte 8) :initial-contents '(1 2 250 4))))
                 (a-aref b 2)))
(r "aref-fixnum-vector" (a-aref (make-array 3 :element-type 'fixnum :initial-contents '(7 8 9)) 1))
(let* ((base (vector 0 1 2 3 4 5))
       (d (make-array 3 :displaced-to base :displaced-index-offset 2)))
  (r "aref-displaced" (a-aref d 1))
  (r "svref-displaced" (err-text (a-svref d 1))))
(let ((fv (make-array 5 :fill-pointer 2 :initial-contents '(a b c d e))))
  (r "aref-fill-pointer-vector-ignores-fp" (a-aref fv 4))
  (r "svref-fill-pointer-vector" (err-text (a-svref fv 0))))
(r "aref-2d-one-index" (err-text (a-aref (make-array '(2 2) :initial-element 0) 1)))
(r "aref-list" (err-text (a-aref '(1 2) 0)))
(r "schar-not-string" (err-text (a-schar 42 0)))
(r "char-not-string" (err-text (a-char 'sym 0)))
(r "schar-oob" (err-text (a-schar "abc" 3)))
(r "schar-negative" (err-text (a-schar "abc" -1)))
(r "char-float-index" (err-text (a-char "abc" 1.0)))
(r "aref-bignum-index" (err-text (a-aref "abc" (expt 2 40))))
(r "svref-oob" (err-text (a-svref (vector 1) 1)))
(r "aref-oob" (err-text (a-aref (vector 1) 5)))
(r "aref-string-oob" (err-text (a-aref "ab" 2)))

;; --- CHAR= ---
(defun c-eq (a b) (char= a b))
(defun c-br (a b) (if (char= a b) :same :diff))
(defun c-br-not (a b) (unless (char= a b) :diff))
(r "chareq" (list (c-eq #\a #\a) (c-eq #\a #\A) (c-eq (code-char 955) (code-char 955))))
(r "chareq-not-char" (err-text (c-eq #\a 1)))
(r "chareq-not-char-2" (err-text (c-eq "a" #\a)))
(r "chareq-br" (list (c-br #\x #\x) (c-br #\x #\y) (c-br-not #\x #\y) (c-br-not #\x #\x)))
(r "chareq-br-not-char" (err-text (c-br #\x 3)))
(r "chareq-nary-still-works" (char= #\a #\a #\a))

;; --- CMP_BR: every comparison, both polarities, every number kind ---
(defun k-lt (a b) (if (< a b) t nil))
(defun k-gt (a b) (if (> a b) t nil))
(defun k-le (a b) (if (<= a b) t nil))
(defun k-ge (a b) (if (>= a b) t nil))
(defun k-eq (a b) (if (= a b) t nil))
(defun k-lt-u (a b) (unless (< a b) t))
(defun k-ge-u (a b) (unless (>= a b) t))
(defun k-eq-u (a b) (unless (= a b) t))
(dolist (pair `((1 2) (2 1) (2 2)
                (,(expt 2 40) 1) (1 ,(expt 2 40)) (,(expt 2 40) ,(expt 2 40))
                (1.5 2) (2 1.5) (1.0 1) (1/2 1/3) (1/3 1/2) (1/2 0.5)
                (,most-positive-fixnum ,most-negative-fixnum)
                (,most-negative-fixnum ,most-positive-fixnum)
                (-1 0) (0 -1) (-5 -5)))
  (let ((a (first pair)) (b (second pair)))
    (format t "cmp ~S ~S = ~S~%" a b
            (list (k-lt a b) (k-gt a b) (k-le a b) (k-ge a b) (k-eq a b)
                  (k-lt-u a b) (k-ge-u a b) (k-eq-u a b)))))
(r "numeq-complex" (list (k-eq #c(1 2) #c(1 2)) (k-eq #c(1 2) #c(1 3))))
(r "lt-complex" (err-text (k-lt #c(1 2) 1)))
(r "lt-string" (err-text (k-lt "a" 1)))
(r "ge-symbol-second" (err-text (k-ge 1 'x)))
(r "numeq-string" (err-text (k-eq 1 "1")))
(r "dotimes-sum" (let ((s 0)) (dotimes (i 10) (setq s (+ s i))) s))
(r "loop-while-lt" (let ((i 0) (n 0)) (loop while (< i 5) do (setq n (+ n i)) (incf i)) n))

;; --- PUSH / POP on locals ---
(defun p-push (x) (let ((s nil)) (push x s) (push 2 s) s))
(defun p-push-value (x) (let ((s '(0))) (push x s)))
(defun p-pop (l) (pop l))
(defun p-pop-twice (l) (list (pop l) (pop l) l))
(defun p-order () (let ((s nil) (i 0)) (push (incf i) s) (push (incf i) s) s))
(defun p-order-2 () (let ((s '(a))) (push (setq s '(z)) s)))
(defun p-stack (n) (let ((s nil) (depth 0) (maxd 0))
                     (dotimes (i n) (push i s) (incf depth) (setq maxd (max maxd depth))
                       (when (evenp i) (pop s) (decf depth)))
                     (list (length s) maxd (first s))))
(defun p-param (l) (push :x l) l)
(r "push" (p-push 1))
(r "push-value" (p-push-value 5))
(r "pop" (list (p-pop '(1 2 3)) (p-pop nil)))
(r "pop-twice" (p-pop-twice '(1 2 3)))
(r "pop-non-list" (err-text (p-pop 42)))
(r "push-order" (p-order))
(r "push-order-2" (p-order-2))
(r "push-pop-stack" (p-stack 11))
(r "push-param" (p-param '(1)))
(defvar *p-special* '(s))
(r "push-special" (progn (push 1 *p-special*) *p-special*))
(r "pop-special" (list (pop *p-special*) *p-special*))
(r "push-captured" (let ((s nil))
                     (let ((f (lambda (x) (push x s))))
                       (funcall f 1) (funcall f 2) s)))
(r "push-symbol-macro" (let ((c (list nil))) (symbol-macrolet ((s (car c))) (push 1 s) (push 2 s)) c))
(r "pop-symbol-macro" (let ((c (list (list 1 2)))) (symbol-macrolet ((s (car c))) (list (pop s) c))))
(r "push-place" (let ((c (list nil))) (push 1 (car c)) c))

;; --- INCF / DECF ---
(defun i-local (i) (list (incf i) (decf i 2) i))
(defvar *i-special* 10)
(r "incf-local" (i-local 5))
(r "incf-float" (let ((x 1)) (incf x 2.5)))
(r "incf-bignum" (let ((i most-positive-fixnum)) (incf i) (list (typep i 'bignum) (- i most-positive-fixnum))))
(r "decf-bignum" (let ((i most-negative-fixnum)) (decf i) (- most-negative-fixnum i)))
(r "incf-special" (list (incf *i-special*) *i-special*))
(r "incf-car" (let ((c (list 1))) (incf (car c) 4) c))
(r "incf-symbol-macro" (let ((c (list 1))) (symbol-macrolet ((p (car c))) (incf p)) c))
(r "incf-delta-form-order" (let ((x 1)) (incf x (setq x 10))))
(r "incf-delta-variable" (let ((x 1) (d 3)) (incf x d)))
(r "incf-not-number" (err-text (let ((x 'a)) (incf x))))

;; --- CASE ---
(defun ca (x) (case x (1 :a) ((2 3) :b) (#\c :c) (t :d)))
(defun ca-empty-body (x) (case x (1) (t :d)))
(r "case" (list (ca 1) (ca 2) (ca 3) (ca #\c) (ca 4) (ca-empty-body 1) (ca-empty-body 2)))
(format t "SEM-DONE~%")
EOF
out=$(run "$WORK/sem.lisp")
check_contains "semantics ran" "SEM-DONE" "$out"
check_contains "schar base string"        'schar-base = #\\e' "$out"
check_contains "char base string"         'char-base = #\\o' "$out"
check_contains "aref on a string"         'aref-string = #\\h' "$out"
check_contains "schar wide string"        'schar-wide = (955 #\\x)' "$out"
check_contains "aref wide string"         'aref-wide = 955' "$out"
check_contains "char on a fill-pointer string" 'char-fill-pointer = 98' "$out"
check_contains "schar rejects a fill-pointer string" 'schar-fill-pointer = "SCHAR: not a simple-string' "$out"
check_contains "char bounds by the fill pointer (as the builtin)" 'char-past-fill-pointer = "CHAR: index 4 out of range' "$out"
check_contains "aref ignores the fill pointer" 'aref-past-fill-pointer = #\\e' "$out"
check_contains "svref"                    'svref = 3' "$out"
check_contains "svref rejects a string"   'svref-string = "SVREF: not a simple vector' "$out"
check_contains "aref general vector"      'aref-vector = 1' "$out"
check_contains "aref bit vector"          'aref-bit = (1 0)' "$out"
check_contains "aref byte vector"         'aref-byte = 250' "$out"
check_contains "aref fixnum vector"       'aref-fixnum-vector = 8' "$out"
check_contains "aref displaced"           'aref-displaced = 3' "$out"
check_contains "svref rejects displaced"  'svref-displaced = "SVREF: not a simple vector' "$out"
check_contains "aref on a fill-pointer vector reads past the fp" 'aref-fill-pointer-vector-ignores-fp = E' "$out"
check_contains "svref rejects a fill-pointer vector" 'svref-fill-pointer-vector = "SVREF: not a simple vector' "$out"
check_contains "aref 2d with one index"   'aref-2d-one-index = "AREF: expected 2 indices, got 1' "$out"
check_contains "aref on a list"           'aref-list = "AREF: not an array' "$out"
check_contains "schar on a non-string"    'schar-not-string = "SCHAR: not a simple-string' "$out"
check_contains "char on a symbol"         'char-not-string = "CHAR: not a string' "$out"
check_contains "schar out of bounds"      'schar-oob = "SCHAR: index 3 out of range' "$out"
check_contains "schar negative index"     'schar-negative = "SCHAR: index -1 out of range' "$out"
check_contains "char float index"         'char-float-index = "CHAR: index must be an integer' "$out"
check_contains "aref bignum index"        'aref-bignum-index = "AREF: index must be a fixnum' "$out"
check_contains "svref out of bounds"      'svref-oob = "SVREF: index 1 out of range' "$out"
check_contains "aref out of bounds"       'aref-oob = "AREF: index 5 out of range' "$out"
check_contains "aref string out of bounds" 'aref-string-oob = "AREF: index 2 out of range' "$out"

check_contains "char= values"             'chareq = (T NIL T)' "$out"
check_contains "char= non-character (second)" 'chareq-not-char = "CHAR=: not a character' "$out"
check_contains "char= non-character (first)"  'chareq-not-char-2 = "CHAR=: not a character' "$out"
check_contains "char= under a branch, both polarities" 'chareq-br = (:SAME :DIFF :DIFF NIL)' "$out"
check_contains "char= under a branch signals" 'chareq-br-not-char = "CHAR=: not a character' "$out"
check_contains "n-ary char= still works"  'chareq-nary-still-works = T' "$out"

check_contains "cmp 1 2"     'cmp 1 2 = (T NIL T NIL NIL NIL T T)' "$out"
check_contains "cmp 2 1"     'cmp 2 1 = (NIL T NIL T NIL T NIL T)' "$out"
check_contains "cmp 2 2"     'cmp 2 2 = (NIL NIL T T T T NIL NIL)' "$out"
check_contains "cmp bignum fixnum" 'cmp 1099511627776 1 = (NIL T NIL T NIL T NIL T)' "$out"
check_contains "cmp fixnum bignum" 'cmp 1 1099511627776 = (T NIL T NIL NIL NIL T T)' "$out"
check_contains "cmp bignum bignum" 'cmp 1099511627776 1099511627776 = (NIL NIL T T T T NIL NIL)' "$out"
check_contains "cmp float int"     'cmp 1.5 2 = (T NIL T NIL NIL NIL T T)' "$out"
check_contains "cmp int float"     'cmp 2 1.5 = (NIL T NIL T NIL T NIL T)' "$out"
check_contains "cmp 1.0 1"         'cmp 1.0 1 = (NIL NIL T T T T NIL NIL)' "$out"
check_contains "cmp ratios"        'cmp 1/2 1/3 = (NIL T NIL T NIL T NIL T)' "$out"
check_contains "cmp ratios 2"      'cmp 1/3 1/2 = (T NIL T NIL NIL NIL T T)' "$out"
check_contains "cmp ratio float"   'cmp 1/2 0.5 = (NIL NIL T T T T NIL NIL)' "$out"
check_contains "cmp fixnum max min" 'cmp 1073741823 -1073741824 = (NIL T NIL T NIL T NIL T)' "$out"
check_contains "cmp fixnum min max" 'cmp -1073741824 1073741823 = (T NIL T NIL NIL NIL T T)' "$out"
check_contains "cmp -1 0"          'cmp -1 0 = (T NIL T NIL NIL NIL T T)' "$out"
check_contains "cmp 0 -1"          'cmp 0 -1 = (NIL T NIL T NIL T NIL T)' "$out"
check_contains "cmp -5 -5"         'cmp -5 -5 = (NIL NIL T T T T NIL NIL)' "$out"
check_contains "= on complex"      'numeq-complex = (T NIL)' "$out"
check_contains "< on complex signals" 'lt-complex = "' "$out"
check_absent   "< on complex is not silently accepted" 'lt-complex = :NO-ERROR' "$out"
check_contains "< on a string signals REAL" 'lt-string = "' "$out"
check_absent   "< on a string not accepted" 'lt-string = :NO-ERROR' "$out"
check_absent   ">= with a symbol second not accepted" 'ge-symbol-second = :NO-ERROR' "$out"
check_absent   "= with a string not accepted" 'numeq-string = :NO-ERROR' "$out"
check_contains "dotimes through CMP_BR"  'dotimes-sum = 45' "$out"
check_contains "loop while through CMP_BR" 'loop-while-lt = 10' "$out"

check_contains "push on a local"          'push = (2 1)' "$out"
check_contains "push returns the new list" 'push-value = (5 0)' "$out"
check_contains "pop on a local"           'pop = (1 NIL)' "$out"
check_contains "pop twice"                'pop-twice = (1 2 (3))' "$out"
check_contains "pop on a non-list signals CAR's error" 'pop-non-list = "' "$out"
check_absent   "pop on a non-list is an error" 'pop-non-list = :NO-ERROR' "$out"
check_contains "push evaluates the item first" 'push-order = (2 1)' "$out"
check_contains "push reads the variable after the item" 'push-order-2 = ((Z) Z)' "$out"
check_contains "push/pop stack discipline" 'push-pop-stack = (5 6 9)' "$out"
check_contains "push on a parameter"      'push-param = (:X 1)' "$out"
check_contains "push on a special"        'push-special = (1 S)' "$out"
check_contains "pop on a special"         'pop-special = (1 (S))' "$out"
check_contains "push on a captured variable" 'push-captured = (2 1)' "$out"
check_contains "push on a symbol-macro place" 'push-symbol-macro = ((2 1))' "$out"
check_contains "pop on a symbol-macro place"  'pop-symbol-macro = (1 ((2)))' "$out"
check_contains "push on a car place"      'push-place = ((1))' "$out"

check_contains "incf/decf on a local"     'incf-local = (6 4 4)' "$out"
check_contains "incf float delta"         'incf-float = 3.5' "$out"
check_contains "incf into a bignum"       'incf-bignum = (T 1)' "$out"
check_contains "decf into a bignum"       'decf-bignum = 1' "$out"
check_contains "incf on a special"        'incf-special = (11 11)' "$out"
check_contains "incf on a car place"      'incf-car = (5)' "$out"
check_contains "incf on a symbol-macro place" 'incf-symbol-macro = (2)' "$out"
check_contains "incf with a side-effecting delta keeps the general order" 'incf-delta-form-order = 20' "$out"
check_contains "incf with a variable delta" 'incf-delta-variable = 4' "$out"
check_absent   "incf on a non-number signals" 'incf-not-number = :NO-ERROR' "$out"
check_contains "case clauses"             'case = (:A :B :B :C :D NIL :D)' "$out"

# ---------------------------------------------------------------------------
# 3. COMPILE-FILE round trip
# ---------------------------------------------------------------------------
cat > "$WORK/rt.lisp" <<'EOF'
(defun rt-scan (s)
  (declare (optimize (speed 3)))
  (let ((depth 0) (maxd 0) (stack nil) (i 0) (n (length s)))
    (loop while (< i n)
          do (let ((c (schar s i)))
               (cond ((char= c #\() (push i stack) (incf depth) (setq maxd (max maxd depth)))
                     ((char= c #\)) (pop stack) (decf depth))))
             (incf i))
    (list depth maxd stack)))
EOF
cat > "$WORK/rt-driver.lisp" <<EOF
(compile-file "$WORK/rt.lisp" :output-file "$WORK/rt.fasl")
(load "$WORK/rt.fasl")
(format t "RT = ~S~%" (rt-scan "(a (b) (c (d"))
(disassemble 'rt-scan)
(format t "RT-DONE~%")
EOF
out=$(run "$WORK/rt-driver.lisp")
check_contains "round trip ran"            "RT-DONE" "$out"
check_contains "loaded function result"    "RT = (3 3 (10 7 0))" "$out"
check_contains "FASL carries AREF"         "AREF         3" "$out"
check_contains "FASL carries CMP_BR"       "CMP_BR" "$out"
check_contains "FASL carries PUSH_LOCAL"   "PUSH_LOCAL" "$out"
check_contains "FASL carries POP_LOCAL"    "POP_LOCAL" "$out"

# ---------------------------------------------------------------------------
# 4. Under allocation pressure
# ---------------------------------------------------------------------------
cat > "$WORK/alloc.lisp" <<EOF
(defun al-scan (s)
  (let ((stack nil) (opens 0) (i 0) (n (length s)))
    (loop while (< i n)
          do (let ((c (char s i)))
               (when (char= c #\\() (push (list i (make-string 3)) stack) (incf opens))
               (when (char= c #\\)) (pop stack)))
             (incf i))
    (list opens (length stack) (car (first stack)))))
(let ((text (with-output-to-string (o)
              (dotimes (k 30) (format o "(defun f~D (x) (list x (car x))) " k))
              (format o "(let ((y "))))
  (let ((r nil))
    (dotimes (k $LOOPN) (setq r (al-scan text)))
    (format t "ALLOC = ~S~%" r)))
(format t "ALLOC-DONE~%")
EOF
out=$(run "$WORK/alloc.lisp")
check_contains "allocation loop ran"       "ALLOC-DONE" "$out"
check_contains "allocation loop result"    "ALLOC = (123 3 986)" "$out"

echo ""
echo "$passed passed, $failed failed, $total total"
[ "$failed" -eq 0 ]
