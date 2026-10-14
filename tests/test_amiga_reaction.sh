#!/bin/sh
# Host-side test of lib/amiga/reaction.lisp (AMIGA.REACTION) and of the
# examples/amiga/reaction/ programs: tests/test_amiga_reaction.lisp checks
# the ReAction-specific portable surface, that the toolkit-neutral half is
# AMIGA.BOOPSI's re-exported (tests/test_amiga_boopsi.sh covers it), and
# loads every example — the host has no ReAction, so each must compile
# completely and bow out with its "not available" line instead of
# failing.  The class half runs on the Amiga in
# tests/amiga/test-reaction.lisp.
#
# Run: sh tests/test_amiga_reaction.sh build/host/amilisp

AMILISP="${1:-build/host/amilisp}"
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TMPD=$(mktemp -d "${TMPDIR:-/tmp}/test_amiga_reaction_XXXXXX") || exit 1
trap 'rm -rf "$TMPD"' EXIT INT TERM

cd "$ROOT" || exit 1
OUT="$TMPD/out.log"
AMILISP_NO_USERINIT=1 "$AMILISP" --non-interactive --heap 64M \
    --load tests/test_amiga_reaction.lisp > "$OUT" 2>&1
status=$?

grep -E '^(PASS|FAIL):' "$OUT"
if [ $status -ne 0 ]; then
    echo "test_amiga_reaction: amilisp exited with status $status"
    tail -20 "$OUT"
    exit 1
fi
if ! grep -q '^ALL REACTION HOST CHECKS PASSED' "$OUT"; then
    echo "test_amiga_reaction: FAILED"
    grep -E 'FAIL|ERROR|rror:' "$OUT" | head -20
    exit 1
fi
echo "test_amiga_reaction: OK"
exit 0
