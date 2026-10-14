#!/bin/sh
# Regression test: LOAD keyword argument handling.
#
# Bug: bi_load had CL_UNUSED(n) and parsed no keyword args, so
# (load "missing" :if-does-not-exist nil) unconditionally errored.
#
# Checks:
#   1. (load missing :if-does-not-exist nil)  -> returns NIL, no ERROR in output
#   2. (load missing)                          -> ERROR message includes path
#   3. (load missing :if-does-not-exist t)    -> ERROR message (same as default)
#   4. (load existing :if-does-not-exist nil) -> loads normally, prints LOADED
#
# Run: sh tests/test_load_keywords.sh build/host/amilisp

AMILISP="${1:-build/host/amilisp}"
passed=0
failed=0
total=0
TMPDIR="${TMPDIR:-/tmp}"
MISSING="/nonexistent/path_that_cannot_exist_$$.lisp"

# run amilisp in batch mode with *load-verbose* suppressed
run_quiet() {
    echo "(setf *load-verbose* nil) $1" | "$AMILISP" --no-userinit --batch 2>&1
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

check_not_contains() {
    desc="$1"; pattern="$2"; out="$3"
    total=$((total + 1))
    if echo "$out" | grep -q "$pattern"; then
        echo "  FAIL  $desc (unexpected: $pattern)"
        echo "    output: $(echo "$out" | head -5)"
        failed=$((failed + 1))
    else
        echo "  ok  $desc"
        passed=$((passed + 1))
    fi
}

# --- Test 1: :if-does-not-exist nil returns NIL, no error ---
out=$(run_quiet "(format t \"~a~%\" (load \"$MISSING\" :if-does-not-exist nil))")
check_not_contains "if-does-not-exist-nil-no-error"  "ERROR"   "$out"
check_contains     "if-does-not-exist-nil-prints-NIL" "NIL"    "$out"

# --- Test 2: default (no :if-does-not-exist) signals an error ---
out=$(run_quiet "(load \"$MISSING\")")
check_contains "default-if-does-not-exist-errors"  "ERROR"       "$out"
check_contains "default-error-includes-path"        "nonexistent" "$out"

# --- Test 3: :if-does-not-exist t also signals an error ---
out=$(run_quiet "(load \"$MISSING\" :if-does-not-exist t)")
check_contains "if-does-not-exist-t-errors" "ERROR" "$out"

# --- Test 4: :if-does-not-exist nil has no effect when file exists ---
tmp=$(mktemp "${TMPDIR}/test_load_kwarg_XXXXXX.lisp") || exit 1
trap 'rm -f "$tmp"' EXIT INT TERM
printf '(format t "LOADED~%%")\n' > "$tmp"
out=$(run_quiet "(load \"$tmp\" :if-does-not-exist nil)")
check_not_contains "existing-file-if-dne-nil-no-error" "ERROR"  "$out"
check_contains     "existing-file-if-dne-nil-loads"    "LOADED" "$out"

echo ""
echo "$passed passed, $failed failed, $total total"
if [ "$failed" -gt 0 ]; then
    echo "FAIL"
    exit 1
fi
echo "PASS"
exit 0
