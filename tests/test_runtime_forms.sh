#!/bin/sh
# Forms and lists the C runtime builds itself, checked against the
# HyperSpec -- run in the fast tier and again under forced compaction.
#
# These are the places that used to build a list as nested calls,
#     cl_cons(SYM_LAMBDA, cl_cons(lambda_list, cl_cons(block_body, CL_NIL)))
# where C may read SYM_LAMBDA and lambda_list before the inner conses run.
# When one of those conses collected, the outer ones stored stale offsets: a
# DEFUN whose LAMBDA no longer was LAMBDA compiled as a function call, its
# parameters as global variables ("Unbound variable: V" in test_vm, which
# depended on nothing but the heap layout).  The call sites now allocate one
# list cell per statement or use cl_list2/3/4 and cl_list_star3 (mem.h), and
# tests/test_gc_arg_order.sh keeps the pattern out of src/.  This script
# covers what each rewritten site produces:
#
#   1. The reader: 'x, `x, ,x, ,@x, #'x (CLHS 2.4.3, 2.4.6, 2.4.8.2).
#   2. DEFUN, DEFMACRO and DEFTYPE lambdas, with their implicit BLOCK
#      (CLHS 3.1.2.1.2.4, 5.3 defun/defmacro, 4.4 deftype); a SETF function.
#   3. MACROLET expanders with &environment and destructuring after a
#      required parameter and after &body (CLHS 3.4.4, 3.4.4.1.2).
#   4. SETF of IF, LET and THE, of SECOND..TENTH and of composite c[ad]r
#      places; SETF of VALUES (CLHS 5.1.2.2, 5.1.2.3, 5.1.2.6).
#   5. RESTART-CASE clauses as lambdas (CLHS 9.1.4.2).
#   6. ECASE's TYPE-ERROR expected type (CLHS ecase: (MEMBER key...)).
#   7. LOAD-TIME-VALUE in a compiled file: evaluated once, at load time
#      (CLHS 3.2.2.2, load-time-value).
#   8. REM / MOD of bignums, ratio reduction (CLHS 12.1.3.2, 12.1.4.1).
#   9. PROVIDE adds to *MODULES*; *FEATURES* carries word size and byte
#      order; EXT symbols exported at init; EXT:FUNCTION-ARGLIST of a
#      builtin; TYPE-OF a packed byte vector; the GC statistics lists.
#
#   sh tests/test_runtime_forms.sh build/host/amilisp
#
# Also run under the gc-stress binary (make test-gc-stress), where every
# allocation compacts the heap.

AMILISP="${1:-build/host/amilisp}"
case "$AMILISP" in
  /*) ;;
  *) AMILISP="$(pwd)/$AMILISP" ;;
esac

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)

passed=0
failed=0
total=0
TMPDIR="${TMPDIR:-/tmp}"
WORK="$TMPDIR/amilisp_rtforms_$$"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

if [ ! -x "$AMILISP" ]; then
    echo "  SKIP  runtime forms: no binary at $AMILISP"
    echo "0 passed, 0 failed, 0 total"
    exit 0
fi

# Every allocation compacts under stress: repeat each form fewer times.
if [ -n "${AMILISP_GC_STRESS:-}" ]; then
    REPEAT=3
else
    REPEAT=200
fi

run() {
    if [ -n "$TIMEOUT" ]; then
        "$TIMEOUT" 300 "$AMILISP" --no-userinit --non-interactive --load "$1" 2>&1 </dev/null
    else
        "$AMILISP" --no-userinit --non-interactive --load "$1" 2>&1 </dev/null
    fi
}

# The Lisp side prints "RF <name> OK" or "RF <name> FAIL <details>".
check() {
    name="$1"; out="$2"
    total=$((total + 1))
    if printf '%s\n' "$out" | grep -qx "RF $name OK"; then
        passed=$((passed + 1))
        echo "  ok  $name"
    else
        failed=$((failed + 1))
        echo "  FAIL  $name"
        printf '%s\n' "$out" | grep "^RF $name " | head -3 | sed 's/^/        /'
    fi
}

cat > "$WORK/forms.lisp" <<EOF
(defvar *repeat* $REPEAT)
(defmacro rf-check (name form expected)
  \`(let ((got (handler-case ,form
                 (error (e) (list :error (princ-to-string e))))))
     (if (equal got ,expected)
         (format t "RF ~A OK~%" ,name)
         (format t "RF ~A FAIL got ~S~%" ,name got))))
(defmacro rf-repeat (name form expected)
  \`(let ((bad nil))
     (dotimes (i *repeat*)
       (let ((got (handler-case ,form
                    (error (e) (list :error (princ-to-string e))))))
         (unless (equal got ,expected) (setf bad got))))
     (if bad
         (format t "RF ~A FAIL got ~S~%" ,name bad)
         (format t "RF ~A OK~%" ,name))))

;; 1. Reader macros
(rf-repeat "reader-quote" (read-from-string "'rf-x") '(quote rf-x))
(rf-repeat "reader-function" (read-from-string "#'car") '(function car))
(rf-repeat "reader-backquote"
           (eval (read-from-string
                  "(let ((a 1) (b (list 2 3))) \`(0 ,a ,@b 4))"))
           '(0 1 2 3 4))

;; 2. DEFUN / DEFMACRO / DEFTYPE
(rf-repeat "defun-params"
           (progn (eval (read-from-string "(defun rf-f (k v ht) (list ht v k))"))
                  (funcall 'rf-f 1 2 3))
           '(3 2 1))
(rf-repeat "defun-block"
           (progn (eval (read-from-string
                         "(defun rf-g (x) (when (> x 0) (return-from rf-g (* x 2))) :neg)"))
                  (list (funcall 'rf-g 5) (funcall 'rf-g -1)))
           '(10 :neg))
(rf-repeat "defun-setf-function"
           (progn (eval (read-from-string
                         "(defun (setf rf-head) (new cell) (setf (car cell) new))"))
                  (let ((c (list 1 2)))
                    (eval \`(setf (rf-head ',c) 9))
                    c))
           '(9 2))
(rf-repeat "defmacro-block"
           (progn (eval (read-from-string
                         "(defmacro rf-m (x) (return-from rf-m (list 'list x x)))"))
                  (eval '(rf-m 3)))
           '(3 3))
(rf-repeat "deftype"
           (progn (eval (read-from-string "(deftype rf-digit () '(integer 0 9))"))
                  (list (typep 5 'rf-digit) (typep 10 'rf-digit)))
           '(t nil))

;; 3. MACROLET: &environment, destructuring
(rf-repeat "macrolet-destructure-required"
           (eval '(macrolet ((m ((a b) &body body) \`(list ,a ,b ,@body)))
                   (m (1 2) 3 4)))
           '(1 2 3 4))
(rf-repeat "macrolet-destructure-body"
           (eval '(macrolet ((m (a &body (x y)) \`(list ,a ,x ,y)))
                   (m 1 2 3)))
           '(1 2 3))
(rf-repeat "macrolet-environment"
           (eval '(macrolet ((inner () 42)
                             (outer (&environment env)
                               (macroexpand '(inner) env)))
                   (outer)))
           42)

;; 4. SETF places
(rf-repeat "setf-if"
           (let ((a (list 1)) (b (list 2)))
             (setf (if (car a) (car a) (car b)) 9)
             (setf (if nil (car a) (car b)) 8)
             (list a b))
           '((9) (8)))
(rf-repeat "setf-if-no-else"
           (let ((a (list 1)))
             (list (setf (if t (car a)) 5) a))
           '(5 (5)))
(rf-repeat "setf-let"
           (let ((c (list 1 2)))
             (setf (let ((x c)) (declare (ignorable x)) (car x)) 7)
             c)
           '(7 2))
(rf-repeat "setf-the"
           (let ((c (list 1)))
             (setf (the fixnum (car c)) 4)
             c)
           '(4))
(rf-repeat "setf-nth-accessor"
           (let ((l (list 1 2 3)))
             (setf (third l) 30)
             l)
           '(1 2 30))
(rf-repeat "setf-composite-cxr"
           (let ((l (list 1 (list 2 3) 4)))
             (setf (caadr l) 20)
             (setf (cddr l) (list 9))
             l)
           '(1 (20 3) 9))
(rf-repeat "setf-values"
           (let (a b)
             (setf (values a b) (floor 7 2))
             (list a b))
           '(3 1))

;; 5. RESTART-CASE
(rf-repeat "restart-case"
           (restart-case (invoke-restart 'rf-r 1 2)
             (rf-r (x y) (+ x y)))
           3)

;; 6. ECASE expected type
(rf-repeat "ecase-expected-type"
           (handler-case (ecase (+ 4 1) (1 :a) (2 :b))
             (type-error (e)
               (let ((et (type-error-expected-type e)))
                 (list (car et)
                       (null (set-exclusive-or (cdr et) '(1 2)))))))
           '(member t))

;; 8. Integer and ratio arithmetic
(rf-repeat "rem-bignum" (list (rem (expt 10 30) 7) (rem (- (expt 10 30)) 7)) '(1 -1))
(rf-repeat "mod-bignum" (list (mod (expt 10 30) 7) (mod (- (expt 10 30)) 7)) '(1 6))
(rf-repeat "ratio-reduce"
           (let ((r (/ (- (expt 2 70)) (expt 6 40))))
             (list (numerator r) (denominator r)))
           (list (- (expt 2 30)) (expt 3 40)))

;; 9. Init-time lists and the statistics builtins
(rf-check "provide"
          (progn (provide "rf-module")
                 (and (member "rf-module" *modules* :test #'string=) t))
          t)
(rf-check "features"
          (list (and (member :clamiga *features*) t)
                (and (or (member :64-bit *features*) (member :32-bit *features*)) t)
                (and (or (member :little-endian *features*)
                         (member :big-endian *features*)) t))
          '(t t t))
(rf-check "ext-exports"
          (mapcar (lambda (n) (nth-value 1 (find-symbol n "EXT")))
                  '("FUNCTION-ARGLIST" "FUNCTION-SOURCE-LOCATION" "BACKTRACE"
                    "FRAME-LOCALS" "INSPECT-PARTS" "ADD-EXIT-HOOK"
                    "REMOVE-EXIT-HOOK"))
          '(:external :external :external :external :external :external
            :external))
(rf-repeat "builtin-arglist"
           (list (and (member '&optional (ext:function-arglist #'read-line)) t)
                 (car (ext:function-arglist #'list)))
           '(t &rest))
(rf-repeat "type-of-byte-vector"
           (list (type-of (make-array 3 :element-type '(unsigned-byte 8)))
                 (type-of (make-array 2 :element-type '(signed-byte 16))))
           '((vector (unsigned-byte 8) 3) (vector (signed-byte 16) 2)))
(rf-repeat "gc-time-stats"
           (let ((s (ext:%gc-time-stats)))
             (list (length s) (floatp (third s)) (floatp (eighth s))))
           '(9 t t))
(rf-repeat "heap-stats"
           (let ((s (ext:%heap-stats)))
             (list (length s) (every #'integerp s)
                   (<= (second s) (first s))        ; bump <= arena
                   (<= (eighth s) (first s))        ; free bytes <= arena
                   (<= (ninth s) (max 1 (eighth s)))))  ; largest <= total free
           '(9 t t t t))
(rf-repeat "gengc-stats"
           (let ((sym (find-symbol "%GENGC-STATS" "EXT")))
             (if (and sym (fboundp sym))
                 (let ((s (funcall sym)))
                   (list (length s) (floatp (third s))))
                 '(6 t)))
           '(6 t))
EOF

out=$(run "$WORK/forms.lisp")
for name in reader-quote reader-function reader-backquote \
            defun-params defun-block defun-setf-function defmacro-block deftype \
            macrolet-destructure-required macrolet-destructure-body \
            macrolet-environment \
            setf-if setf-if-no-else setf-let setf-the setf-nth-accessor \
            setf-composite-cxr setf-values \
            restart-case ecase-expected-type \
            rem-bignum mod-bignum ratio-reduce \
            provide features ext-exports builtin-arglist type-of-byte-vector \
            gc-time-stats gengc-stats; do
    check "$name" "$out"
done

# 7. LOAD-TIME-VALUE through COMPILE-FILE: the form runs once, when the file
# is loaded (not when it is compiled), and every call sees that value.
cat > "$WORK/ltv.lisp" <<'EOF'
(defun rf-ltv () (load-time-value (incf *rf-ltv-count*)))
EOF
cat > "$WORK/ltv-driver.lisp" <<EOF
(defvar *rf-ltv-count* 0)
(compile-file "$WORK/ltv.lisp" :output-file "$WORK/ltv.fasl")
(format t "RF ltv-not-at-compile ~A~%" (if (= *rf-ltv-count* 0) "OK" *rf-ltv-count*))
(load "$WORK/ltv.fasl")
(let ((v (list (rf-ltv) (rf-ltv) *rf-ltv-count*)))
  (format t "RF ltv-once ~A~%" (if (equal v '(1 1 1)) "OK" v)))
EOF
out=$(run "$WORK/ltv-driver.lisp")
check ltv-not-at-compile "$out"
check ltv-once "$out"

echo ""
echo "$passed passed, $failed failed, $total total"
[ "$failed" -eq 0 ]
