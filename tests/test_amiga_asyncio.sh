#!/bin/sh
# Host-side test of lib/amiga/asyncio.lisp (AMIGA.ASYNCIO, the DOS-packet
# double-buffered async file I/O module): tests/test_amiga_asyncio.lisp
# checks that the module loads, refuses cleanly without AmigaOS, and that
# examples/amiga/asyncio/copyfile.lisp compiles and bows out.  The
# functional half needs a filesystem handler and runs on the Amiga in
# tests/amiga/test-asyncio.lisp.
#
# Run: sh tests/test_amiga_asyncio.sh build/host/amilisp

AMILISP="${1:-build/host/amilisp}"
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TMPD=$(mktemp -d "${TMPDIR:-/tmp}/test_amiga_asyncio_XXXXXX") || exit 1
trap 'rm -rf "$TMPD"' EXIT INT TERM

cd "$ROOT" || exit 1
OUT="$TMPD/out.log"
AMILISP_NO_USERINIT=1 "$AMILISP" --non-interactive --heap 24M \
    --load tests/test_amiga_asyncio.lisp > "$OUT" 2>&1
status=$?

grep -E '^(PASS|FAIL):' "$OUT"
if [ $status -ne 0 ]; then
    echo "test_amiga_asyncio: amilisp exited with status $status"
    tail -20 "$OUT"
    exit 1
fi
if ! grep -q '^ALL ASYNCIO HOST CHECKS PASSED' "$OUT"; then
    echo "test_amiga_asyncio: FAILED"
    grep -E 'FAIL|ERROR|rror:' "$OUT" | head -20
    exit 1
fi
echo "test_amiga_asyncio: OK"
exit 0
