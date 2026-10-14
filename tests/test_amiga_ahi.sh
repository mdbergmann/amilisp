#!/bin/sh
# Host-side test of lib/amiga/ahi.lisp (AMIGA.AHI, the opt-in AHI playback
# module): tests/test_amiga_ahi.lisp checks that the module (and the
# generated AMIGA.RAW.AHI table it builds on) loads, that OPEN-AHI answers
# NIL without AmigaOS, the pure helpers, the argument checks, the refusals
# of the function interface while nothing is open, and that
# examples/amiga/audio/ahi-play.lisp compiles and bows out.  The
# functional half needs ahi.device and runs on the Amiga in
# tests/amiga/test-ahi.lisp.
#
# Run: sh tests/test_amiga_ahi.sh build/host/amilisp

AMILISP="${1:-build/host/amilisp}"
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TMPD=$(mktemp -d "${TMPDIR:-/tmp}/test_amiga_ahi_XXXXXX") || exit 1
trap 'rm -rf "$TMPD"' EXIT INT TERM

cd "$ROOT" || exit 1
OUT="$TMPD/out.log"
AMILISP_NO_USERINIT=1 "$AMILISP" --non-interactive --heap 24M \
    --load tests/test_amiga_ahi.lisp > "$OUT" 2>&1
status=$?

grep -E '^(PASS|FAIL):' "$OUT"
if [ $status -ne 0 ]; then
    echo "test_amiga_ahi: amilisp exited with status $status"
    tail -20 "$OUT"
    exit 1
fi
if ! grep -q '^ALL AHI HOST CHECKS PASSED' "$OUT"; then
    echo "test_amiga_ahi: FAILED"
    grep -E 'FAIL|ERROR|rror:' "$OUT" | head -20
    exit 1
fi
echo "test_amiga_ahi: OK"
exit 0
