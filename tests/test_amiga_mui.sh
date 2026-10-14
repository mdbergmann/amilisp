#!/bin/sh
# Host-side test of lib/amiga/mui.lisp (AMIGA.MUI, the MUI helpers over
# AMIGA.BOOPSI and muimaster.library) and of the examples/amiga/mui/
# programs: tests/test_amiga_mui.lisp checks the portable surface --
# AVAILABLE-P, CLASS-ID against every MUIC_ constant of the generated
# raw module, MAKE-ID, the MUIM_Notify and MUI_MakeObject argument
# packing, POOL-STRING-ARRAY, the mui.h size macros, every diagnostic --
# that the toolkit-neutral half is AMIGA.BOOPSI's re-exported, and loads
# every example: the host has no MUI, so each must compile completely and
# bow out with its "not available" line instead of failing.  The object
# half runs on the Amiga in tests/amiga/test-mui.lisp.
#
# Run: sh tests/test_amiga_mui.sh build/host/amilisp

CLAMIGA="${1:-build/host/amilisp}"
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TMPD=$(mktemp -d "${TMPDIR:-/tmp}/test_amiga_mui_XXXXXX") || exit 1
trap 'rm -rf "$TMPD"' EXIT INT TERM

cd "$ROOT" || exit 1
OUT="$TMPD/out.log"
CLAMIGA_NO_USERINIT=1 "$CLAMIGA" --non-interactive --heap 64M \
    --load tests/test_amiga_mui.lisp > "$OUT" 2>&1
status=$?

grep -E '^(PASS|FAIL):' "$OUT"
if [ $status -ne 0 ]; then
    echo "test_amiga_mui: amilisp exited with status $status"
    tail -20 "$OUT"
    exit 1
fi
if ! grep -q '^ALL MUI HOST CHECKS PASSED' "$OUT"; then
    echo "test_amiga_mui: FAILED"
    grep -E 'FAIL|ERROR|rror:' "$OUT" | head -20
    exit 1
fi
echo "test_amiga_mui: OK"
exit 0
