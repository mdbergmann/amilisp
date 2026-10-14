#!/bin/sh
# Regression test: the test harness must never read a developer's personal
# ~/.amilisprc user-init file.
#
# Bug: every C unit test calls cl_repl_init(), which loads ~/.amilisprc at
# startup.  When a developer's ~/.amilisprc pulled in quicklisp
#   (require :asdf) (load "~/quicklisp/setup.lisp") ...
# `make test-fast` ground to a crawl and HUNG in test_fasl (the userinit ran
# inside the test process; a relative (load ...) that failed from a temp CWD
# dropped into code that blocked).  C tests can't pass the --no-userinit CLI
# flag, so load_user_init() now honours the CLAMIGA_NO_USERINIT env var as an
# escape hatch, and the Makefile test targets export it.
#
# This test pins both opt-out paths (env var + CLI flag) and confirms the
# baseline (no opt-out) really does load the file — so the guard is meaningful.
#
# Run: sh tests/test_userinit.sh build/host/amilisp

CLAMIGA="${1:-build/host/amilisp}"
case "$CLAMIGA" in /*) ;; *) CLAMIGA="$PWD/$CLAMIGA";; esac

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)
if [ -z "$TIMEOUT" ]; then
    echo "SKIP test_userinit: neither timeout nor gtimeout on PATH"
    exit 0
fi

passed=0
failed=0
total=0

# Fake HOME holding a ~/.amilisprc that prints an unmistakable marker.
FAKE_HOME=$(mktemp -d "${TMPDIR:-/tmp}/clamiga_userinit_XXXXXX") || exit 1
cleanup() { rm -rf "$FAKE_HOME"; }
trap cleanup EXIT INT TERM
printf '(format t "~&USERINIT-WAS-LOADED~%%")\n' > "$FAKE_HOME/.amilisprc"

MARKER="USERINIT-WAS-LOADED"

# desc, expect (yes|no), captured-output -> assert marker presence/absence,
# and never a hang (124).
check() {
    desc="$1"; expect="$2"; ec="$3"; out="$4"
    total=$((total + 1))
    if [ "$ec" -eq 124 ]; then
        echo "  FAIL  $desc (timed out, ec=124 — amilisp hung)"
        failed=$((failed + 1)); return
    fi
    if echo "$out" | grep -q "$MARKER"; then seen=yes; else seen=no; fi
    if [ "$seen" = "$expect" ]; then
        echo "  ok  $desc"
        passed=$((passed + 1))
    else
        echo "  FAIL  $desc (expected userinit-loaded=$expect, got $seen)"
        echo "    output: $(echo "$out" | head -5)"
        failed=$((failed + 1))
    fi
}

# --- Baseline: with no opt-out, ~/.amilisprc IS loaded (fixture sanity) ---
out=$(echo '(quit)' | HOME="$FAKE_HOME" CLAMIGA_NO_USERINIT= "$TIMEOUT" 10 "$CLAMIGA" --batch 2>&1)
check "baseline_loads_userinit" yes "$?" "$out"

# --- The pre-rename ~/.clamigarc is still read when ~/.amilisprc is absent ---
OLD_HOME=$(mktemp -d "${TMPDIR:-/tmp}/clamiga_userinit_old_XXXXXX") || exit 1
trap 'rm -rf "$FAKE_HOME" "$OLD_HOME" "$BOTH_HOME"' EXIT INT TERM
printf '(format t "~&USERINIT-WAS-LOADED~%%")\n' > "$OLD_HOME/.clamigarc"
out=$(echo '(quit)' | HOME="$OLD_HOME" CLAMIGA_NO_USERINIT= "$TIMEOUT" 10 "$CLAMIGA" --batch 2>&1)
check "pre_rename_rc_still_loads" yes "$?" "$out"

# --- With both files, ~/.amilisprc wins and ~/.clamigarc is NOT read too ---
BOTH_HOME=$(mktemp -d "${TMPDIR:-/tmp}/clamiga_userinit_both_XXXXXX") || exit 1
printf '(format t "~&USERINIT-WAS-LOADED~%%")\n' > "$BOTH_HOME/.amilisprc"
printf '(format t "~&OLD-RC-WAS-LOADED~%%")\n' > "$BOTH_HOME/.clamigarc"
out=$(echo '(quit)' | HOME="$BOTH_HOME" CLAMIGA_NO_USERINIT= "$TIMEOUT" 10 "$CLAMIGA" --batch 2>&1)
check "new_rc_wins_over_old" yes "$?" "$out"
total=$((total + 1))
if echo "$out" | grep -q "OLD-RC-WAS-LOADED"; then
    echo "  FAIL  old_rc_not_loaded_too (~/.clamigarc ran although ~/.amilisprc exists)"
    failed=$((failed + 1))
else
    echo "  ok  old_rc_not_loaded_too"
    passed=$((passed + 1))
fi

# --- CLAMIGA_NO_USERINIT=1 env var suppresses it (the C fix) ---
out=$(echo '(quit)' | HOME="$FAKE_HOME" CLAMIGA_NO_USERINIT=1 "$TIMEOUT" 10 "$CLAMIGA" --batch 2>&1)
check "env_var_skips_userinit" no "$?" "$out"

# --- --no-userinit CLI flag suppresses it ---
out=$(echo '(quit)' | HOME="$FAKE_HOME" CLAMIGA_NO_USERINIT= "$TIMEOUT" 10 "$CLAMIGA" --no-userinit --batch 2>&1)
check "flag_skips_userinit" no "$?" "$out"

echo ""
echo "$passed passed, $failed failed, $total total"
[ "$failed" -eq 0 ]
