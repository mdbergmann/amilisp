#!/bin/sh
# Regression tests for the Tier-4 Phase 1 runtime-tax removals
# (specs/performance.md 4.1).  Each case pins the BEHAVIOUR that a
# performance change had to preserve, so a later optimization in the same
# area cannot quietly trade correctness for speed.
#
#   sh tests/test_tier4_phase1.sh build/host/amilisp
#
# Covered:
#   1. call_builtin no longer copies the multiple-value buffer on every
#      call — THROW must still see its value form's extra values.
#   2. call_builtin no longer runs cl_check_c_stack — deep recursion must
#      still end in a catchable error, not a crash.
#   3. TYPEP dispatches standard type names by a code on the symbol — every
#      name must answer exactly as the strcmp cascade did, including for a
#      same-named symbol that is NOT the COMMON-LISP one.
#   4. The per-thread slot-index cache must be invalidated by class
#      redefinition (a warmed cache must not serve a stale slot index).
#   5. The GF inline cache validates by class NAME instead of a
#      *CLASS-TABLE* lookup — dispatch must stay correct across receiver
#      classes, subclasses and method redefinition.
#   6. OP_CONS / OP_LIST / &rest build their lists from GC-rooted stack
#      slots — the results must be well formed at every arity.
#   7. Because (5) validates by name, a class metaobject REPLACED by a
#      different object under the same name must still not be served from a
#      warm GF cache.  It is not, because class registration clears every
#      GF's cache — an invariant this optimization now depends on, so it
#      gets a test of its own.

CLAMIGA="${1:-build/host/amilisp}"

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)

passed=0
failed=0
total=0
TMPDIR="${TMPDIR:-/tmp}"
WORK="$TMPDIR/clamiga_tier4p1_$$"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

if [ ! -x "$CLAMIGA" ]; then
    echo "  SKIP  tier4 phase1: no binary at $CLAMIGA"
    echo "0 passed, 0 failed, 0 total"
    exit 0
fi

# Loop counts.  The point of the repeat loops is to WARM the caches under
# test, which takes a few dozen iterations, not thousands; the large counts
# are only there to make an ordinary run also exercise collection pressure.
# Under CLAMIGA_GC_STRESS every single allocation forces a full compaction,
# so the same coverage has to come from far fewer iterations.
if [ -n "${CLAMIGA_GC_STRESS:-}" ]; then
    WARM=40; GFN=8; CONSN=200; RESTN=40
else
    WARM=2000; GFN=200; CONSN=20000; RESTN=300
fi
GFN2=$((GFN * 2))

run() {
    if [ -n "$TIMEOUT" ]; then
        "$TIMEOUT" 60 "$CLAMIGA" --no-userinit --heap 48M \
            --non-interactive --load "$1" 2>&1
    else
        "$CLAMIGA" --no-userinit --heap 48M --non-interactive --load "$1" 2>&1
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
        echo "$actual" | tail -6 | sed 's/^/      /'
        failed=$((failed + 1))
    fi
}

check_absent() {
    desc="$1"; pattern="$2"; actual="$3"
    total=$((total + 1))
    if echo "$actual" | grep -q "$pattern"; then
        echo "  FAIL  $desc (saw /$pattern/)"
        echo "$actual" | grep "$pattern" | head -3 | sed 's/^/      /'
        failed=$((failed + 1))
    else
        echo "  ok    $desc"
        passed=$((passed + 1))
    fi
}

# --- Case 1: multiple values survive call_builtin's mv reset ---------------
# call_builtin saves the caller's mv state into pre_call_mv_* before
# resetting mv_count to 1, and THROW reads it so (throw tag (values ...))
# transfers ALL the values.  The save now takes a one-store fast path when
# mv_count is 1 and the loop only when it is not; both must be right.
cat > "$WORK/mv.lisp" <<'EOF'
(format t "MV3:~s~%"
        (multiple-value-list (catch 'k (throw 'k (values 1 2 3)))))
(format t "MV1:~s~%"
        (multiple-value-list (catch 'k (throw 'k 7))))
(format t "MV0:~s~%"
        (multiple-value-list (catch 'k (throw 'k (values)))))
;; through an interposing unwind-protect, which takes the pending-throw path
(format t "MVU:~s~%"
        (multiple-value-list
         (catch 'k (unwind-protect (throw 'k (values 4 5 6)) (list 1)))))
;; a builtin call between the values form and the throw must not leak values
(format t "MVX:~s~%"
        (multiple-value-list (catch 'k (progn (values 8 9) (throw 'k 1)))))
EOF
out=$(run "$WORK/mv.lisp")
check_contains "throw carries all values of a VALUES form"     'MV3:(1 2 3)' "$out"
check_contains "throw of a single value"                       'MV1:(7)'     "$out"
check_contains "throw of no values"                            'MV0:NIL'     "$out"
check_contains "throw through unwind-protect keeps its values" 'MVU:(4 5 6)' "$out"
check_contains "a later single-value throw does not inherit"   'MVX:(1)'     "$out"

# --- Case 2: recursion still ends in a catchable error --------------------
# The C-stack guard moved off the per-builtin path onto cl_vm_apply, which
# is where the C stack actually grows.  Runaway recursion must still stop
# with a reported error rather than a segfault.
cat > "$WORK/rec.lisp" <<'EOF'
(defun runaway (n) (+ 1 (runaway (+ n 1))))
(handler-case (runaway 0)
  (error (e) (format t "REC-CAUGHT:~a~%" (type-of e))))
(format t "REC-ALIVE~%")
EOF
out=$(run "$WORK/rec.lisp")
check_absent   "runaway recursion does not crash the process" "Segmentation" "$out"
check_contains "runaway recursion is reported"                "overflow\|OVERFLOW\|REC-CAUGHT" "$out"

# --- Case 3: TYPEP answers every standard type name as before -------------
cat > "$WORK/typep.lisp" <<'EOF'
(defun chk (label got want)
  (format t "~a:~a~%" label (if (eq (not got) (not want)) "OK" "BAD")))
(chk "T1"  (typep 5 't) t)                (chk "T2"  (typep 5 'nil) nil)
(chk "T3"  (typep nil 'null) t)           (chk "T4"  (typep nil 'list) t)
(chk "T5"  (typep '(1) 'cons) t)          (chk "T6"  (typep 5 'atom) t)
(chk "T7"  (typep 5 'fixnum) t)           (chk "T8"  (typep 5 'integer) t)
(chk "T9"  (typep -1 'unsigned-byte) nil) (chk "T10" (typep 1 'bit) t)
(chk "T11" (typep 3/4 'ratio) t)          (chk "T12" (typep 3/4 'rational) t)
(chk "T13" (typep 1.5 'float) t)          (chk "T14" (typep 1.5d0 'double-float) t)
(chk "T15" (typep 1.5 'real) t)           (chk "T16" (typep 5 'number) t)
(chk "T17" (typep #\a 'character) t)      (chk "T18" (typep #\a 'base-char) t)
(chk "T19" (typep "s" 'string) t)         (chk "T20" (typep "s" 'simple-string) t)
(chk "T21" (typep #*101 'bit-vector) t)   (chk "T22" (typep #*101 'simple-bit-vector) t)
(chk "T23" (typep (vector 1) 'vector) t)  (chk "T24" (typep (vector 1) 'simple-vector) t)
(chk "T25" (typep (vector 1) 'array) t)   (chk "T26" (typep '(1) 'sequence) t)
(chk "T27" (typep #'car 'function) t)     (chk "T28" (typep (make-hash-table) 'hash-table) t)
(chk "T29" (typep *package* 'package) t)  (chk "T30" (typep 'a 'symbol) t)
(chk "T31" (typep :a 'keyword) t)         (chk "T32" (typep 'a 'keyword) nil)
(chk "T33" (typep t 'boolean) t)          (chk "T34" (typep *standard-output* 'stream) t)
(chk "T35" (typep (make-random-state) 'random-state) t)
(chk "T36" (typep #p"/tmp/x" 'pathname) t)
(chk "T37" (typep #p"/tmp/x" 'logical-pathname) nil)
;; fill-pointer / adjustable vectors are arrays but not SIMPLE ones
(chk "T38" (typep (make-array 2 :adjustable t) 'simple-array) nil)
(chk "T39" (typep (make-array 2 :adjustable t) 'array) t)
(chk "T40" (typep (make-array 2 :fill-pointer 1) 'simple-vector) nil)
;; a multidimensional array is an ARRAY but not a VECTOR and not a SEQUENCE
(chk "T41" (typep (make-array '(2 2)) 'array) t)
(chk "T42" (typep (make-array '(2 2)) 'vector) nil)
(chk "T43" (typep (make-array '(2 2)) 'sequence) nil)
;; structures and conditions still route through the user-type path
(defstruct tp-s a)
(chk "T44" (typep (make-tp-s) 'tp-s) t)
(chk "T45" (typep (make-tp-s) 'structure-object) t)
(chk "T46" (typep 5 'tp-s) nil)
(defclass tp-c () ())
(defclass tp-d (tp-c) ())
(chk "T47" (typep (make-instance 'tp-d) 'tp-c) t)
(chk "T48" (typep (make-instance 'tp-c) 'tp-d) nil)
(chk "T49" (typep (make-condition 'simple-error) 'error) t)
;; DEFTYPE still wins over nothing, and never over a standard name
(deftype tp-small () '(integer 0 9))
(chk "T50" (typep 5 'tp-small) t)
(chk "T51" (typep 50 'tp-small) nil)
;; THE / (declare (type ...)) run the same path via OP_ASSERT_TYPE
(chk "T52" (eql (the fixnum 3) 3) t)
(handler-case (progn (the fixnum "x") (chk "T53" nil t))
  (error () (chk "T53" t t)))
;; a symbol that only SHARES a standard type name is still resolved
(defpackage :tp-other (:use))
(chk "T54" (typep 5 (intern "FIXNUM" :tp-other)) t)
(chk "T55" (typep "s" (intern "FIXNUM" :tp-other)) nil)
(format t "TYPEP-DONE~%")
EOF
out=$(run "$WORK/typep.lisp")
check_absent   "no BAD answer from any standard type name" ":BAD" "$out"
check_contains "typep case ran to completion"              "TYPEP-DONE" "$out"

# --- Case 4: the slot-index cache is invalidated by redefinition ----------
# struct_slot_resolve caches (type, slot) -> index per thread, stamped with
# a registry generation.  Redefining the class bumps that generation; a
# cache warmed on the OLD layout must not answer for the NEW one.
cat > "$WORK/slotic.lisp" <<EOF
(defclass sc () ((a :initform 'a1) (b :initform 'b1)))
(defvar *o1* (make-instance 'sc))
;; warm the cache, so the resolve path is definitely cached
(dotimes (i $WARM) (slot-value *o1* 'a) (slot-value *o1* 'b))
(format t "S1:~s~%" (list (slot-value *o1* 'a) (slot-value *o1* 'b)))
;; redefine with a slot PREPENDED: every index shifts by one
(defclass sc () ((z :initform 'z2) (a :initform 'a2) (b :initform 'b2)))
(defvar *o2* (make-instance 'sc))
(format t "S2:~s~%" (list (slot-value *o2* 'z) (slot-value *o2* 'a)
                          (slot-value *o2* 'b)))
(setf (slot-value *o2* 'a) 'set-a)
(format t "S3:~s~%" (list (slot-value *o2* 'z) (slot-value *o2* 'a)
                          (slot-value *o2* 'b)))
(format t "S4:~s~%" (with-slots (z a b) *o2* (list z a b)))
(format t "S5:~s~%" (list (slot-boundp *o2* 'z) (slot-boundp *o2* 'a)))
;; a slot that does not exist is a cached NEGATIVE — it must stay an error
(handler-case (slot-value *o2* 'nosuch)
  (error () (format t "S6:ERR~%")))
(handler-case (slot-value *o2* 'nosuch)
  (error () (format t "S7:ERR~%")))
;; two classes sharing slot names must not share cache entries
(defclass sd () ((b :initform 'd-b) (a :initform 'd-a)))
(defvar *d* (make-instance 'sd))
(dotimes (i $GFN) (slot-value *d* 'a) (slot-value *o2* 'a))
(format t "S8:~s~%" (list (slot-value *d* 'a) (slot-value *d* 'b)
                          (slot-value *o2* 'a)))
(format t "SLOTIC-DONE~%")
EOF
out=$(run "$WORK/slotic.lisp")
check_contains "slots read before redefinition"          'S1:(A1 B1)'         "$out"
check_contains "warmed cache does not survive redefine"  'S2:(Z2 A2 B2)'      "$out"
check_contains "slot write lands in the new layout"      'S3:(Z2 SET-A B2)'   "$out"
check_contains "with-slots agrees with slot-value"       'S4:(Z2 SET-A B2)'   "$out"
check_contains "slot-boundp agrees"                      'S5:(T T)'           "$out"
check_contains "missing slot errors (uncached)"          'S6:ERR'             "$out"
check_contains "missing slot errors again (cached)"      'S7:ERR'             "$out"
check_contains "same slot names in two classes"          'S8:(D-A D-B SET-A)' "$out"
check_contains "slot cache case ran to completion"       'SLOTIC-DONE'        "$out"

# --- Case 5: GF inline cache validated by class name ----------------------
# %GF-IC-EMF compares the receiver's class NAME against the name of the
# class held in the cache instead of resolving the name through
# *CLASS-TABLE* under the tables lock.  Dispatch must be unchanged.
cat > "$WORK/gfic.lisp" <<EOF
(defclass ga () ((v :initarg :v :accessor ga-v)))
(defclass gb (ga) ())
(defgeneric who (x))
(defmethod who ((x ga)) 'ga)
(defmethod who ((x gb)) 'gb)
(defvar *a* (make-instance 'ga :v 1))
(defvar *b* (make-instance 'gb :v 2))
;; alternate receivers so the single-entry IC is repeatedly invalidated
(format t "G1:~s~%" (let (r) (dotimes (i $GFN) (push (who *a*) r) (push (who *b*) r))
                      (list (first r) (second r) (length r))))
;; built-in receiver classes take the non-struct branch of class-of
(defgeneric kind (x))
(defmethod kind ((x integer)) 'int)
(defmethod kind ((x string)) 'str)
(defmethod kind ((x symbol)) 'sym)
(defmethod kind ((x cons))   'cons)
(defmethod kind ((x t))      'other)
(format t "G2:~s~%" (let (r) (dotimes (i $GFN)
                               (setq r (list (kind 1) (kind "s") (kind 'a)
                                             (kind '(1)) (kind 1.5))))
                      r))
;; two-argument dispatch uses the two-entry cache shape
(defgeneric pair (x y))
(defmethod pair ((x ga) (y integer)) 'ga-int)
(defmethod pair ((x ga) (y string))  'ga-str)
(defmethod pair ((x gb) (y integer)) 'gb-int)
(format t "G3:~s~%" (let (r) (dotimes (i $GFN)
                               (setq r (list (pair *a* 1) (pair *a* "s")
                                             (pair *b* 1))))
                      r))
;; adding a method must invalidate the cache
(defmethod who ((x gb)) 'gb-new)
(format t "G4:~s~%" (list (who *a*) (who *b*)))
;; accessors keep working through the reader/writer IC
(setf (ga-v *b*) 99)
(format t "G5:~s~%" (list (ga-v *a*) (ga-v *b*)))
(format t "GFIC-DONE~%")
EOF
out=$(run "$WORK/gfic.lisp")
check_contains "1-arg dispatch alternating receiver classes" "G1:(GB GA $GFN2)" "$out"
check_contains "dispatch on built-in receiver classes" 'G2:(INT STR SYM CONS OTHER)' "$out"
check_contains "2-arg dispatch"                        'G3:(GA-INT GA-STR GB-INT)' "$out"
check_contains "redefining a method invalidates the IC" 'G4:(GA GB-NEW)'  "$out"
check_contains "accessors still read and write"         'G5:(1 99)'       "$out"
check_contains "gf cache case ran to completion"        'GFIC-DONE'       "$out"

# --- Case 6: list building from rooted stack slots ------------------------
# OP_CONS, OP_LIST and the &rest builders now read their operands from
# GC-rooted VM stack / extra-arg slots after the allocation instead of
# pushing each operand onto the GC root stack.
cat > "$WORK/lists.lisp" <<EOF
(defun r0 (&rest r) r)
(defun r1 (a &rest r) (list a r))
(defun k1 (a &key b c &allow-other-keys) (list a b c))
(format t "L1:~s~%" (cons 1 2))
(format t "L2:~s~%" (list 1 2 3 4 5 6 7 8))
(format t "L3:~s~%" (r0))
(format t "L4:~s~%" (r0 1 2 3))
(format t "L5:~s~%" (r1 1 2 3))
(format t "L6:~s~%" (apply #'r0 (list 1 2 3 4)))
(format t "L7:~s~%" (k1 1 :b 2 :c 3))
;; a long &rest exercises the extra-args buffer past a single cons
(format t "L8:~s~%" (length (apply #'r0 (loop for i from 1 to $RESTN collect i))))
;; consing hard enough to force collections while lists are half built
(format t "L9:~s~%"
        (let ((acc nil))
          (dotimes (i $CONSN) (setq acc (list i (cons i i) (list i i i))))
          (length acc)))
(format t "LISTS-DONE~%")
EOF
out=$(run "$WORK/lists.lisp")
check_contains "OP_CONS"                 'L1:(1 . 2)'          "$out"
check_contains "OP_LIST at arity 8"      'L2:(1 2 3 4 5 6 7 8)' "$out"
check_contains "empty &rest"             'L3:NIL'              "$out"
check_contains "&rest collects args"     'L4:(1 2 3)'          "$out"
check_contains "required plus &rest"     'L5:(1 (2 3))'        "$out"
check_contains "&rest through APPLY"     'L6:(1 2 3 4)'        "$out"
check_contains "&key after required"     'L7:(1 2 3)'          "$out"
check_contains "long &rest through APPLY" "L8:$RESTN"          "$out"
check_contains "consing under collection pressure" 'L9:3'      "$out"
check_contains "list case ran to completion"       'LISTS-DONE' "$out"

# --- Case 7: a class REPLACED under an unchanged name ---------------------
# %GF-IC-EMF validates its cached entry by class NAME rather than by
# looking the name up in *CLASS-TABLE* on every dispatch, so it now RELIES
# on an invariant it did not rely on before: whenever a class metaobject is
# replaced by a different object under the same name, every GF's slot-8
# cache must already have been cleared.  It is — %FINALIZE-AND-REGISTER-CLASS
# (lib/clos.lisp) is the shared tail of both the standard and the
# metaclass-driven creation paths and ends in %INVALIDATE-ALL-GF-CACHES —
# but nothing else pins that, and the failure mode if it ever changes is a
# GF silently dispatching to the superseded class's methods forever.  A
# metaclass change is the sharpest form: it allocates a genuinely new class
# object whose name slot still reads the same symbol.
#
# No method is (re)installed on RC-TAG after the swap: %INSTALL-METHOD-IN-GF
# clears its own GF's cache on every ADD-METHOD, which would mask a broken
# %INVALIDATE-ALL-GF-CACHES behind that unrelated clear.  Instead a FALLBACK
# method on T is installed before the swap; the OLD class's method can never
# apply to an instance of the genuinely-new class object, so a correctly
# invalidated cache must fall through to slow dispatch and land on FALLBACK,
# while a stale slot-8 entry (matched by name only) would still invoke the
# old FIRST method directly.
cat > "$WORK/reclass.lisp" <<EOF
(defclass mc-a (standard-class) ())
(defclass mc-b (standard-class) ())
(defmethod validate-superclass ((c mc-a) (s standard-class)) t)
(defmethod validate-superclass ((c mc-b) (s standard-class)) t)
(defclass rc () ((v :initarg :v :accessor rc-v)) (:metaclass mc-a))
(defgeneric rc-tag (x))
(defmethod rc-tag ((x rc)) 'first)
(defmethod rc-tag ((x t)) 'fallback)
(defvar *rc* (make-instance 'rc :v 1))
;; warm the inline cache against the FIRST class object
(dotimes (i $GFN) (rc-tag *rc*))
(format t "R1:~s~%" (rc-tag *rc*))
(format t "R2:~s~%" (eq (find-class 'rc) (find-class 'rc)))
(defvar *rc-old-class* (find-class 'rc))
;; replace the class under the SAME name, via a metaclass change
(defclass rc () ((v :initarg :v :accessor rc-v)) (:metaclass mc-b))
(format t "R3:~s~%" (eq *rc-old-class* (find-class 'rc)))
(defvar *rc2* (make-instance 'rc :v 2))
(format t "R4:~s~%" (rc-tag *rc2*))
(format t "R5:~s~%" (rc-v *rc2*))
(format t "RECLASS-DONE~%")
EOF
out=$(run "$WORK/reclass.lisp")
check_contains "dispatch before the class is replaced" 'R1:FIRST' "$out"
check_contains "find-class is stable"                  'R2:T'     "$out"
check_contains "class replacement allocates a new class object" 'R3:NIL' "$out"
check_contains "a warm cache does not serve the superseded class" 'R4:FALLBACK' "$out"
check_contains "accessors follow the new class"        'R5:2'     "$out"
check_contains "class-replacement case ran to completion" 'RECLASS-DONE' "$out"

echo ""
echo "$passed passed, $failed failed, $total total"
[ "$failed" -eq 0 ] || exit 1
exit 0
