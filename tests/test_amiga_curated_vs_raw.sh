#!/bin/sh
# The hand-written lib/amiga/*.lisp modules (AMIGA.EXEC, AMIGA.INTUITION,
# AMIGA.GFX, AMIGA.GADTOOLS, AMIGA.AUDIO, ...) still carry hand-typed OS
# constants, LVO offsets and DEFCFUN register assignments.  The generated
# lib/amiga/raw/ bindings are the authority (NDK 3.2 includes; LVOs
# cross-checked against the NDK's lvo/*.i by the generator), so this test
# loads both on the host and requires every hand-typed value to agree --
# see tests/test_amiga_curated_vs_raw.lisp for the three checks.  It would
# have caught AMIGA.INTUITION:+WFLG-REPORTMOUSE+ = #x4 (real value #x200).
#
# Run: sh tests/test_amiga_curated_vs_raw.sh build/host/amilisp

CLAMIGA="${1:-build/host/amilisp}"
ROOT=$(cd "$(dirname "$0")/.." && pwd)
CHECK="$ROOT/tests/test_amiga_curated_vs_raw.lisp"
case "$CLAMIGA" in /*) ;; *) CLAMIGA="$(pwd)/$CLAMIGA" ;; esac

cd "$ROOT" || exit 1

out=$(CLAMIGA_NO_USERINIT=1 "$CLAMIGA" --no-userinit --non-interactive \
      --heap 256M --load "$CHECK" </dev/null 2>&1 | grep -v '^; Loading')

echo "$out" | grep -E '^(FAIL|ok  |     uncovered)' | sed 's/^/  /'
errors=$(echo "$out" | grep -E '^ERROR:' | head -5)
summary=$(echo "$out" | grep '^CURATED-VS-RAW-RESULT')

fail=0
if [ -n "$errors" ]; then
    echo "  FAIL: errors while loading/checking:"
    echo "$errors" | sed 's/^/    /'
    fail=1
fi
if [ -z "$summary" ]; then
    echo "  FAIL: check script did not finish"
    echo "$out" | tail -15 | sed 's/^/    /'
    fail=1
else
    echo "  $summary"
    case "$summary" in
        *" fail=0 "*) ;;
        *) fail=1 ;;
    esac
fi

if [ $fail -eq 0 ]; then
    echo "test_amiga_curated_vs_raw: PASS"
    exit 0
else
    echo "test_amiga_curated_vs_raw: FAIL"
    exit 1
fi
