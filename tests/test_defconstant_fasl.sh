#!/bin/sh
# Regression test: DEFCONSTANT marks the symbol constant at LOAD time, not only
# at compile time.
#
# compile_defconstant sets the CL_SYM_CONSTANT flag on the symbol while
# compiling, which makes CONSTANTP work in the SAME session.  But when a
# defconstant is loaded from a *precompiled* FASL in a fresh process, that
# compile-time flag never ran — so CONSTANTP used to return NIL for the
# constant even though its value was correctly stored.  This broke, e.g.,
# serapeum's EVAL-IF-CONSTANT (which does (when (constantp x) (eval x))).
#
# The fix makes defconstant emit a runtime (%MARK-CONSTANT 'name) call so the
# flag is set whenever the form is *loaded*, FASL or not.  This test compiles a
# defconstant to a FASL in one process and verifies CONSTANTP in a SEPARATE
# fresh process that only loads the FASL.
#
# Run: sh tests/test_defconstant_fasl.sh [path-to-amilisp]

CLAMIGA="${1:-build/host/amilisp}"
passed=0
failed=0
TMP="${TMPDIR:-/tmp}/clamiga-dcfasl-$$"
mkdir -p "$TMP"
cleanup() { rm -rf "$TMP" "$HOME/.cache/common-lisp/cl-amiga-"*"$TMP"* 2>/dev/null; }
trap cleanup EXIT

SRC="$TMP/dc.lisp"
FASL="$TMP/dc.fasl"
cat > "$SRC" <<'EOF'
(defconstant +dcfasl-k+ 42)
EOF

# Process 1: compile the defconstant to a FASL (do NOT load/eval it here, so the
# compile-time flag set in this process cannot leak into process 2).
COMPILE_DRIVER="$TMP/compile.lisp"
cat > "$COMPILE_DRIVER" <<EOF
(compile-file "$SRC" :output-file "$FASL")
EOF
CLAMIGA_NO_USERINIT=1 "$CLAMIGA" --no-userinit --non-interactive \
    --load "$COMPILE_DRIVER" </dev/null >/dev/null 2>&1

if [ ! -f "$FASL" ]; then
    echo "  FAIL  compile_produced_fasl (no FASL at $FASL)"
    failed=$((failed + 1))
else
    echo "  ok  compile_produced_fasl"
    passed=$((passed + 1))
fi

# Process 2: fresh process, only loads the FASL, then checks value + constantp.
LOAD_DRIVER="$TMP/load.lisp"
cat > "$LOAD_DRIVER" <<EOF
(load "$FASL")
(format t "~&VAL=~a CONSTANTP=~a~%"
        (symbol-value '+dcfasl-k+)
        (if (constantp '+dcfasl-k+) :yes :no))
EOF
out=$(CLAMIGA_NO_USERINIT=1 "$CLAMIGA" --no-userinit --non-interactive \
          --load "$LOAD_DRIVER" </dev/null 2>&1)

val=$(printf '%s\n' "$out" | sed -n 's/.*VAL=\([^ ]*\) .*/\1/p' | head -1)
cp=$(printf '%s\n' "$out" | sed -n 's/.*CONSTANTP=\(.*\)/\1/p' | head -1)

if [ "$val" = "42" ]; then
    echo "  ok  value_loaded_from_fasl"
    passed=$((passed + 1))
else
    echo "  FAIL  value_loaded_from_fasl (VAL=$val)"
    failed=$((failed + 1))
fi

if [ "$cp" = "YES" ]; then
    echo "  ok  constantp_true_after_fasl_load"
    passed=$((passed + 1))
else
    echo "  FAIL  constantp_true_after_fasl_load (CONSTANTP=$cp)"
    printf '%s\n' "$out" | grep -iE "VAL|CONSTANTP|error" | head -5
    failed=$((failed + 1))
fi

echo ""
echo "test_defconstant_fasl: $passed passed, $failed failed"
[ "$failed" -eq 0 ]
