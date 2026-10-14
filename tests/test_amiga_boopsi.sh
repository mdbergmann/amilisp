#!/bin/sh
# Host-side test of lib/amiga/boopsi.lisp (AMIGA.BOOPSI, the toolkit-neutral
# BOOPSI helpers shared by AMIGA.REACTION and AMIGA.MUI):
# tests/test_amiga_boopsi.lisp checks the portable half of the module
# (ULONG coercion, the foreign pool, NEW-LIST, WITH-TAGS, the diagnostics)
# and the layering — the module loads with no toolkit package present, and
# AMIGA.REACTION re-exports its symbols rather than homonyms.  The object
# half runs on the Amiga in tests/amiga/test-boopsi.lisp.
#
# Run: sh tests/test_amiga_boopsi.sh build/host/amilisp

AMILISP="${1:-build/host/amilisp}"
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TMPD=$(mktemp -d "${TMPDIR:-/tmp}/test_amiga_boopsi_XXXXXX") || exit 1
trap 'rm -rf "$TMPD"' EXIT INT TERM

cd "$ROOT" || exit 1
OUT="$TMPD/out.log"
AMILISP_NO_USERINIT=1 "$AMILISP" --non-interactive --heap 64M \
    --load tests/test_amiga_boopsi.lisp > "$OUT" 2>&1
status=$?

grep -E '^(PASS|FAIL):' "$OUT"
if [ $status -ne 0 ]; then
    echo "test_amiga_boopsi: amilisp exited with status $status"
    tail -20 "$OUT"
    exit 1
fi
if ! grep -q '^ALL BOOPSI HOST CHECKS PASSED' "$OUT"; then
    echo "test_amiga_boopsi: FAILED"
    grep -E 'FAIL|ERROR|rror:' "$OUT" | head -20
    exit 1
fi
echo "test_amiga_boopsi: OK"
exit 0
