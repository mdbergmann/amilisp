#!/bin/sh
# Regression test: SLOT-VALUE / (SETF SLOT-VALUE) / SLOT-BOUNDP on
# DEFSTRUCT instances after the spec-3.1 registry hash index +
# %STRUCT-SLOT-INDEX optimization (builtins_struct.c, clos.lisp
# %find-struct-slot-index).
#
# The pre-optimization path resolved a struct slot on EVERY access via a
# linear walk of the prepended struct registry alist plus a freshly
# consed slot-name list.  These checks pin the semantics the fast path
# must preserve:
#   1. slot-value / setf / slot-boundp / with-slots on a struct instance
#   2. a type buried behind 300 later registrations still resolves
#   3. re-registering a type name: the NEWEST entry wins
#   4. missing slot signals an error naming the slot
#   5. lookups still resolve after an explicit compaction (index rebuild)
#
# Run: sh tests/test_struct_slot_access.sh build/host/amilisp

AMILISP="${1:-build/host/amilisp}"
passed=0
failed=0
total=0

run_quiet() {
    "$AMILISP" --no-userinit --batch 2>&1 <<EOF
(setf *load-verbose* nil)
$1
EOF
}

check_contains() {
    desc="$1"; pattern="$2"; out="$3"
    total=$((total + 1))
    if echo "$out" | grep -q "$pattern"; then
        echo "  ok  $desc"
        passed=$((passed + 1))
    else
        echo "  FAIL  $desc (missing: $pattern)"
        echo "    output: $(echo "$out" | head -5)"
        failed=$((failed + 1))
    fi
}

# --- 1. Basic slot-value protocol on a struct instance ---
out=$(run_quiet '
(defstruct ssa-pt (x 1) (y 2))
(let ((s (make-ssa-pt :x 3 :y 4)))
  (format t "SV:~a~%" (+ (slot-value s (quote x)) (slot-value s (quote y))))
  (setf (slot-value s (quote y)) 10)
  (format t "SETF:~a~%" (slot-value s (quote y)))
  (format t "BOUNDP:~a~%" (slot-boundp s (quote x)))
  (with-slots (x y) s
    (format t "WITH-SLOTS:~a~%" (+ x y))))')
check_contains "slot-value reads struct slots"        "SV:7"           "$out"
check_contains "setf slot-value writes struct slots"  "SETF:10"        "$out"
check_contains "slot-boundp true on struct slot"      "BOUNDP:T"       "$out"
check_contains "with-slots works on struct instance"  "WITH-SLOTS:13"  "$out"

# --- 2. Type buried behind 300 later registrations still resolves ---
out=$(run_quiet '
(defstruct ssa-early (a 100) (b 200))
(dotimes (i 300)
  (amilisp::%register-struct-type
   (make-symbol (format nil "SSA-FILLER-~D" i)) 1 nil (quote ((f nil)))))
(let ((s (make-ssa-early)))
  (format t "BURIED:~a~%" (+ (slot-value s (quote a)) (slot-value s (quote b))))
  (format t "TYPEP:~a~%" (typep s (quote ssa-early))))')
check_contains "buried type slot-value resolves" "BURIED:300" "$out"
check_contains "buried type typep resolves"      "TYPEP:T"    "$out"

# --- 3. Re-registration: newest entry wins ---
out=$(run_quiet '
(amilisp::%register-struct-type (quote ssa-re) 1 nil (quote ((a nil))))
(amilisp::%register-struct-type (quote ssa-re) 3 nil (quote ((p nil) (q nil) (r nil))))
(format t "COUNT:~a~%" (amilisp::%struct-slot-count (quote ssa-re)))
(format t "NEWSLOT:~a~%" (amilisp::%struct-slot-index (quote ssa-re) (quote r)))
(format t "OLDSLOT:~a~%" (amilisp::%struct-slot-index (quote ssa-re) (quote a)))')
check_contains "redefined type has new slot count"  "COUNT:3"     "$out"
check_contains "redefined type resolves new slot"   "NEWSLOT:2"   "$out"
check_contains "redefined type drops old slot"      "OLDSLOT:NIL" "$out"

# --- 4. Missing slot errors and names the slot ---
out=$(run_quiet '
(defstruct ssa-err (v 1))
(handler-case (slot-value (make-ssa-err) (quote no-such-slot))
  (error (e) (format t "ERR:~a~%" e)))')
check_contains "missing slot signals error"     "ERR:"         "$out"
check_contains "missing slot error names slot"  "NO-SUCH-SLOT" "$out"

# --- 4b. Fused fast path + accessor semantics (spec 3.1, second half) ---
# DEFCLASS accessors and SLOT-VALUE now front the full protocol with the
# fused %STRUCT-SLOT-VALUE / %STRUCT-SLOT-STORE builtins and the GF
# discriminators probe the inline cache via %GF-IC-EMF.  Pin the
# semantics the fast path must preserve: subclass instances through a
# superclass accessor (shifted slot index), unbound-slot signalling,
# :CLASS-allocated slots, and SLOT-VALUE-USING-CLASS interception.
out=$(run_quiet '
(defclass ssa-base ()
  ((x :initarg :x :accessor ssa-x)
   (u :accessor ssa-u)
   (c :allocation :class :initform 5 :accessor ssa-c)))
(defclass ssa-sub (ssa-base)
  ((z :initform 100 :accessor ssa-z)))
(let ((b (make-instance (quote ssa-base) :x 3))
      (s (make-instance (quote ssa-sub) :x 30)))
  (setf (ssa-x b) 4)
  (format t "ACC:~a~%" (ssa-x b))
  (format t "SUBACC:~a/~a~%" (ssa-x s) (ssa-z s))
  (format t "UNBOUND:~a~%"
          (handler-case (ssa-u b) (error () :signaled)))
  (setf (ssa-c b) 6)
  (format t "CLASSSLOT:~a~%" (ssa-c s)))
(defmethod slot-value-using-class :around ((class t) (inst ssa-base) slot)
  (declare (ignore slot))
  (* 100 (call-next-method)))
(format t "SVUC:~a~%" (ssa-x (make-instance (quote ssa-base) :x 7)))')
check_contains "accessor write/read round-trip"           "ACC:4"        "$out"
check_contains "subclass via superclass accessor (index)" "SUBACC:30/100" "$out"
check_contains "unbound slot signals through accessor"    "UNBOUND:SIGNALED" "$out"
check_contains ":class slot shared through accessor"      "CLASSSLOT:6"  "$out"
check_contains "accessor honors slot-value-using-class"   "SVUC:700"     "$out"

# --- 5. Lookups resolve after explicit compaction (index rebuild) ---
out=$(run_quiet '
(defstruct ssa-gc (v 41))
(format t "PRE:~a~%" (slot-value (make-ssa-gc) (quote v)))
(ext:gc-compact)
(format t "POST:~a~%" (slot-value (make-ssa-gc) (quote v)))
(ext:gc-compact)
(format t "IDX:~a~%" (amilisp::%struct-slot-index (quote ssa-gc) (quote v)))')
check_contains "slot-value before compaction"       "PRE:41"  "$out"
check_contains "slot-value after compaction"        "POST:41" "$out"
check_contains "slot index rebuilt after compaction" "IDX:0"   "$out"

# --- 6. DEFSTRUCT's hidden slot setter stays out of the user's namespace ----
# The function behind (SETF (acc obj) val) was %SET-<accessor> interned in the
# current package: (defstruct ssa-vw title) took over a user function named
# %SET-SSA-VW-TITLE, and the setter's compiler macro turned every direct call
# to the user's function into a slot write on its first argument (chipi-ui's
# %SET-VIEW-TITLE wrote the title into its nav-context's DEPTH slot).  It is
# AMILISP::%STRUCT-SET-<package>::<accessor> now.  Both definition orders, a
# typed (:type list) structure, and an accessor from another package.
out=$(run_quiet '
(defstruct ssa-nc body (depth 0))
(defstruct ssa-vw container page title)
(defun %set-ssa-vw-title (ctx title) (setf (ssa-nc-depth ctx) (length title)) :user)
(defun %set-ssa-late-x (a b) (list :user a b))
(defstruct ssa-late x)
(defstruct (ssa-lst (:type list)) p q)
(defun %set-ssa-lst-q (a b) (list :user-list a b))
(let ((c (make-ssa-nc)) (v (make-ssa-vw)) (l (make-ssa-late)) (s (make-ssa-lst)))
  (format t "DIRECT:~s/~s~%" (%set-ssa-vw-title c "abc") (ssa-nc-depth c))
  (setf (ssa-vw-title v) "T")
  (format t "SETF:~s/~s~%" (ssa-vw-title v) (ssa-nc-depth c))
  (setf (ssa-late-x l) 7)
  (format t "LATE:~s/~s~%" (%set-ssa-late-x 1 2) (ssa-late-x l))
  (setf (ssa-lst-q s) 9)
  (format t "LIST:~s/~s~%" (%set-ssa-lst-q 1 2) s)
  (incf (ssa-nc-depth c) 10)
  (format t "INCF:~s~%" (ssa-nc-depth c))
  (format t "NOSYM:~s~%" (find-symbol "%SET-SSA-NC-DEPTH"))
  (format t "HOME:~s~%" (symbol-package (amilisp::%struct-setter-name (quote ssa-nc-depth)))))')
check_contains "user %SET-<acc> function is called, not the slot setter" 'DIRECT::USER/3' "$out"
check_contains "SETF of the accessor still writes the slot"              'SETF:"T"/3'     "$out"
check_contains "DEFSTRUCT after the user function leaves it alone"       'LATE:(:USER 1 2)/7' "$out"
check_contains "typed (:type list) structure likewise"                   'LIST:(:USER-LIST 1 2)/(NIL 9)' "$out"
check_contains "INCF through the accessor"                               'INCF:13'        "$out"
check_contains "no %SET-<acc> symbol in the user package"                'NOSYM:NIL'      "$out"
check_contains "the setter lives in CLAMIGA"                             'HOME:#<PACKAGE "AMILISP">\|HOME:#<PACKAGE AMILISP>' "$out"

echo ""
echo "$passed passed, $failed failed, $total total"
if [ "$failed" -gt 0 ]; then
    echo "FAIL"
    exit 1
fi
echo "PASS"
exit 0
