#!/bin/sh
# Host-side test of lib/amiga/iff.lisp (AMIGA.IFF, the iffparse.library
# module grown from the NDK 3.1 sift example): tests/test_amiga_iff.lisp
# checks that the module loads, the pure-Lisp helpers work, everything
# OS-facing refuses cleanly without AmigaOS, and that
# examples/amiga/iff/sift.lisp compiles and bows out.  The functional
# half needs iffparse.library and runs on the Amiga in
# tests/amiga/test-iff.lisp.
#
# Run: sh tests/test_amiga_iff.sh build/host/amilisp

AMILISP="${1:-build/host/amilisp}"
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TMPD=$(mktemp -d "${TMPDIR:-/tmp}/test_amiga_iff_XXXXXX") || exit 1
trap 'rm -rf "$TMPD"' EXIT INT TERM

cd "$ROOT" || exit 1
OUT="$TMPD/out.log"
AMILISP_NO_USERINIT=1 "$AMILISP" --non-interactive --heap 24M \
    --load tests/test_amiga_iff.lisp > "$OUT" 2>&1
status=$?

grep -E '^(PASS|FAIL):' "$OUT"
if [ $status -ne 0 ]; then
    echo "test_amiga_iff: amilisp exited with status $status"
    tail -20 "$OUT"
    exit 1
fi
if ! grep -q '^ALL IFF HOST CHECKS PASSED' "$OUT"; then
    echo "test_amiga_iff: FAILED"
    grep -E 'FAIL|ERROR|rror:' "$OUT" | head -20
    exit 1
fi
echo "test_amiga_iff: OK"
exit 0
