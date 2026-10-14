#!/bin/sh
# Regression test: (DEFVAR name) with no init-form proclaims the variable
# SPECIAL at LOAD time, not only at compile time.
#
# compile_defvar set CL_SYM_SPECIAL on the symbol while compiling, which made
# the variable special in the SAME session.  But (defvar *x*) with no init-form
# emitted NO bytecode to mark it special, so when loaded from a *precompiled*
# FASL in a fresh process the variable was NOT special — a dynamic (let ((*x*
# ...))) then bound it lexically and code reading *x* saw an unbound global.
# This made serapeum's DEFPLACE test (which uses (defvar *place*)) fail
# nondeterministically depending on whether its FASL was freshly compiled.
#
# The fix emits a runtime (%MARK-SPECIAL 'name) so the proclamation also takes
# effect at load.  Crucially the variable must stay UNBOUND (CLHS): %MARK-SPECIAL
# only sets the flag, it never stores a value.  This test compiles a bare defvar
# to a FASL in one process and verifies, in a SEPARATE fresh process that only
# loads the FASL, that the variable is special (dynamic LET works) yet unbound.
#
# Run: sh tests/test_defvar_special_fasl.sh [path-to-amilisp]

AMILISP="${1:-build/host/amilisp}"
passed=0
failed=0
TMP="${TMPDIR:-/tmp}/clamiga-dvfasl-$$"
mkdir -p "$TMP"
cleanup() { rm -rf "$TMP" "$HOME/.cache/common-lisp/amilisp-"*"$TMP"* 2>/dev/null; }
trap cleanup EXIT

SRC="$TMP/dv.lisp"
FASL="$TMP/dv.fasl"
cat > "$SRC" <<'EOF'
(defvar *dvfasl-var*)
(defun dvfasl-read () *dvfasl-var*)
(defun dvfasl-dyn () (let ((*dvfasl-var* 77)) (dvfasl-read)))
EOF

# Process 1: compile only (do NOT load/eval, so the compile-time flag in this
# process cannot leak into process 2).
COMPILE_DRIVER="$TMP/compile.lisp"
cat > "$COMPILE_DRIVER" <<EOF
(compile-file "$SRC" :output-file "$FASL")
EOF
AMILISP_NO_USERINIT=1 "$AMILISP" --no-userinit --non-interactive \
    --load "$COMPILE_DRIVER" </dev/null >/dev/null 2>&1

if [ ! -f "$FASL" ]; then
    echo "  FAIL  compile_produced_fasl (no FASL at $FASL)"
    failed=$((failed + 1))
else
    echo "  ok  compile_produced_fasl"
    passed=$((passed + 1))
fi

# Process 2: fresh process, only loads the FASL, then checks special + unbound.
LOAD_DRIVER="$TMP/load.lisp"
cat > "$LOAD_DRIVER" <<EOF
(load "$FASL")
(format t "~&BOUNDP=~a DYN=~a~%"
        (if (boundp '*dvfasl-var*) :yes :no)
        (dvfasl-dyn))
EOF
out=$(AMILISP_NO_USERINIT=1 "$AMILISP" --no-userinit --non-interactive \
          --load "$LOAD_DRIVER" </dev/null 2>&1)

bound=$(printf '%s\n' "$out" | sed -n 's/.*BOUNDP=\([^ ]*\) .*/\1/p' | head -1)
dyn=$(printf '%s\n' "$out" | sed -n 's/.*DYN=\(.*\)/\1/p' | head -1)

# A bare (defvar *x*) must leave the variable unbound (CLHS).
if [ "$bound" = "NO" ]; then
    echo "  ok  stays_unbound_after_fasl_load"
    passed=$((passed + 1))
else
    echo "  FAIL  stays_unbound_after_fasl_load (BOUNDP=$bound)"
    failed=$((failed + 1))
fi

# The variable must be special: a dynamic LET binding is visible to a separately
# compiled reader.  Before the fix this errored with "Unbound variable".
if [ "$dyn" = "77" ]; then
    echo "  ok  special_dynamic_binding_after_fasl_load"
    passed=$((passed + 1))
else
    echo "  FAIL  special_dynamic_binding_after_fasl_load (DYN=$dyn)"
    printf '%s\n' "$out" | grep -iE "BOUNDP|DYN|error|unbound" | head -5
    failed=$((failed + 1))
fi

echo ""
echo "test_defvar_special_fasl: $passed passed, $failed failed"
[ "$failed" -eq 0 ]
