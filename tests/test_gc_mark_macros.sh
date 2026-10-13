#!/bin/sh
# The raw header mark-bit macros (CL_HDR_MARKED, CL_HDR_SET_MARK,
# CL_HDR_CLR_MARK, CL_HDR_MARK_BIT) may appear only where the mark bit's
# meaning is defined: types.h, the seam gc_backend.h, and the collector's
# own files src/core/gc_*.c.  Everything else reads and writes marks
# through GCB_HDR_MARKED / GCB_HDR_SET_MARK / GCB_HDR_CLR_MARK.
#
# Why a lint: under the Immix backend (specs/gc-backend-immix.md, "Marks
# without a clearing pass") the bit's sense flips every cycle, so a raw
# test of the bit reads a live object as dead on every other cycle — a
# memory-corruption bug that no single run reproduces reliably.  The
# classic backend's macros are the raw ones, which is exactly how such a
# site would pass every classic gate.
#
# Run: sh tests/test_gc_mark_macros.sh        (no clamiga binary needed)

ROOT=$(cd "$(dirname "$0")/.." && pwd)
passed=0
failed=0
total=0

ok()   { passed=$((passed + 1)); total=$((total + 1)); echo "  ok  $1"; }
fail() { failed=$((failed + 1)); total=$((total + 1)); echo "  FAIL  $1"; }

hits=$(grep -rnE 'CL_HDR_(MARKED|SET_MARK|CLR_MARK|MARK_BIT)\b' "$ROOT/src" \
        --include='*.c' --include='*.h' \
        | grep -vE '/src/core/types\.h:|/src/core/gc_backend\.h:|/src/core/gc_[a-z_]+\.c:' \
        | grep -vE '^[^:]*:[0-9]+:[[:space:]]*(/?\*|//)' || true)
if [ -z "$hits" ]; then
    ok "no raw mark-bit macro outside the collector"
else
    fail "raw mark-bit macro outside the collector (use the GCB_HDR_* forms):"
    echo "$hits" | sed 's/^/        /'
fi

echo "$passed passed, $failed failed, $total total"
[ "$failed" -eq 0 ]
