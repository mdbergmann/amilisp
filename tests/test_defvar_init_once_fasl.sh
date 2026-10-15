#!/bin/sh
# DEFVAR's initial-value form from a compiled FASL (CLHS DEFVAR: "evaluated
# only if name is not already bound").
#
# OP_DEFVAR checked boundness only when STORING, so the form itself ran on
# every load: its side effects repeated although the value stayed.  The fix
# tests (AMILISP::%GLOBALLY-BOUND-P 'name) first and jumps over the form; the
# bound branch still runs OP_DEFVAR so that a variable bound by a plain SETQ
# before the FASL is loaded becomes special at load time.  The same change
# took the value form of DEFVAR / DEFPARAMETER / DEFCONSTANT out of tail
# position: as the last form of a function body it compiled to a tail call,
# and the store after it was dead code.
#
# Process 1 compiles only; process 2 is a fresh session that loads the FASL
# twice, so nothing the compiler did at compile time can leak into the checks.
#
# Run: sh tests/test_defvar_init_once_fasl.sh [path-to-amilisp]

AMILISP="${1:-build/host/amilisp}"
passed=0
failed=0
TMP="${TMPDIR:-/tmp}/amilisp-dvonce-$$"
mkdir -p "$TMP"
cleanup() { rm -rf "$TMP" "$HOME/.cache/common-lisp/amilisp-"*"$TMP"* 2>/dev/null; }
trap cleanup EXIT

SRC="$TMP/dvonce.lisp"
FASL="$TMP/dvonce.fasl"
cat > "$SRC" <<'EOF'
(defvar *dvonce-count* 0)
(defvar *dvonce-var* (incf *dvonce-count*))
(defvar *dvonce-pre* (error "DEFVAR evaluated the form of a bound variable"))
(defun dvonce-pre-read () *dvonce-pre*)
(defun dvonce-tail-var () (defvar *dvonce-tail-v* (list 1 2)))
(defun dvonce-tail-par () (defparameter *dvonce-tail-p* (list 3 4)))
(defun dvonce-tail-con () (defconstant +dvonce-tail-c+ (+ 40 2)))
EOF

COMPILE_DRIVER="$TMP/compile.lisp"
cat > "$COMPILE_DRIVER" <<EOF
(compile-file "$SRC" :output-file "$FASL")
EOF
AMILISP_NO_USERINIT=1 "$AMILISP" --no-userinit --non-interactive \
    --load "$COMPILE_DRIVER" </dev/null >/dev/null 2>&1

check() {  # name expected actual
    if [ "$2" = "$3" ]; then
        echo "  ok  $1"
        passed=$((passed + 1))
    else
        echo "  FAIL  $1 (expected '$2', got '$3')"
        failed=$((failed + 1))
    fi
}

if [ ! -f "$FASL" ]; then
    echo "  FAIL  compile_produced_fasl (no FASL at $FASL)"
    echo ""
    echo "test_defvar_init_once_fasl: 0 passed, 1 failed"
    exit 1
fi
check compile_produced_fasl yes yes

# *DVONCE-PRE* is bound by a plain SETQ before the load: DEFVAR must neither
# evaluate its (signalling) form nor leave the variable non-special.
LOAD_DRIVER="$TMP/load.lisp"
cat > "$LOAD_DRIVER" <<EOF
(setq *dvonce-pre* 5)
(load "$FASL")
(load "$FASL")
(format t "~&COUNT=~a VAR=~a~%" *dvonce-count* *dvonce-var*)
(format t "~&PRE=~a DYN=~a~%" *dvonce-pre*
        (let ((*dvonce-pre* 6)) (dvonce-pre-read)))
(format t "~&TAILV=~s ~s~%" (dvonce-tail-var) *dvonce-tail-v*)
(format t "~&TAILP=~s ~s~%" (dvonce-tail-par) *dvonce-tail-p*)
(format t "~&TAILC=~s ~s~%" (dvonce-tail-con) +dvonce-tail-c+)
EOF
out=$(AMILISP_NO_USERINIT=1 "$AMILISP" --no-userinit --non-interactive \
          --load "$LOAD_DRIVER" </dev/null 2>&1)

field() { printf '%s\n' "$out" | sed -n "s/^$1=//p" | head -1; }

check init_form_evaluated_once_across_two_loads "1 VAR=1" "$(field COUNT)"
check bound_variable_keeps_value_and_becomes_special "5 DYN=6" "$(field PRE)"
check defvar_in_tail_position_stores "*DVONCE-TAIL-V* (1 2)" "$(field TAILV)"
check defparameter_in_tail_position_stores "*DVONCE-TAIL-P* (3 4)" "$(field TAILP)"
check defconstant_in_tail_position_stores "+DVONCE-TAIL-C+ 42" "$(field TAILC)"

if [ "$failed" -gt 0 ]; then
    printf '%s\n' "$out" | grep -iE "error|unbound|warning" | head -5
fi

echo ""
echo "test_defvar_init_once_fasl: $passed passed, $failed failed"
[ "$failed" -eq 0 ]
