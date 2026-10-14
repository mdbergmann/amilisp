#!/bin/sh
# Host-side test of the examples/amiga/gfx/ programs (bouncing-lines,
# the NDK double-buffering port, the RKM hardware-sprite port, the
# screen grabber):
# tests/test_amiga_gfx_examples.lisp loads every one — the host has no
# Amiga, so each must compile completely and bow out with its "not
# available" line instead of failing — and checks that their RUN
# functions refuse cleanly and free what they allocated when the OS is
# absent.  The functional half runs in FS-UAE in
# tests/amiga/test-gfx-examples.lisp.
#
# Run: sh tests/test_amiga_gfx_examples.sh build/host/amilisp

AMILISP="${1:-build/host/amilisp}"
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TMPD=$(mktemp -d "${TMPDIR:-/tmp}/test_amiga_gfx_examples_XXXXXX") || exit 1
trap 'rm -rf "$TMPD"' EXIT INT TERM

cd "$ROOT" || exit 1
OUT="$TMPD/out.log"
AMILISP_NO_USERINIT=1 "$AMILISP" --non-interactive --heap 24M \
    --load tests/test_amiga_gfx_examples.lisp > "$OUT" 2>&1
status=$?

grep -E '^(PASS|FAIL):' "$OUT"
if [ $status -ne 0 ]; then
    echo "test_amiga_gfx_examples: amilisp exited with status $status"
    tail -20 "$OUT"
    exit 1
fi
if ! grep -q '^ALL GFX EXAMPLES HOST CHECKS PASSED' "$OUT"; then
    echo "test_amiga_gfx_examples: FAILED"
    grep -E 'FAIL|ERROR|rror:' "$OUT" | head -20
    exit 1
fi
echo "test_amiga_gfx_examples: OK"
exit 0
