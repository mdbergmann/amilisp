#!/bin/sh
# Regression test: ROOM's "(N%)" on a heap large enough to have overflowed
# the old 32-bit `used * 100`.  A 256 MB heap with 215 MB in use printed
# "(0%)" (field report, 2026-11-09); any heap with more than 42 MB in use
# did.  Runs the real binary with --heap 64M, pins 48 MB of live data and
# checks that the percentage ROOM prints agrees with the byte counts on
# the same line.
#
#   sh tests/test_room_percent.sh build/host/amilisp

AMILISP="${1:-build/host/amilisp}"

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)

passed=0
failed=0
total=0
TMPDIR="${TMPDIR:-/tmp}"
WORK="$TMPDIR/amilisp_roompct_$$"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

if [ ! -x "$AMILISP" ]; then
    echo "  SKIP  room percent: no binary at $AMILISP"
    echo "0 passed, 0 failed, 0 total"
    exit 0
fi

check() {
    desc="$1"; want="$2"; got="$3"
    total=$((total + 1))
    if [ "$want" = "$got" ]; then
        echo "  ok    $desc"
        passed=$((passed + 1))
    else
        echo "  FAIL  $desc (expected '$want', got '$got')"
        failed=$((failed + 1))
    fi
}

# 12 vectors of 1M fixnums (ARRAY-DIMENSION-LIMIT is 1M) = 48 MB of
# payload, well past the 42 MB overflow point.
cat > "$WORK/room.lisp" <<'EOF'
(defvar *big* (loop repeat 12 collect (make-array 1000000 :initial-element 0)))
(room)
(format t "LEN=~D~%" (reduce #'+ *big* :key #'length))
EOF

if [ -n "$TIMEOUT" ]; then
    out=$("$TIMEOUT" 60 "$AMILISP" --no-userinit --heap 64M --non-interactive \
          --load "$WORK/room.lisp" 2>&1)
else
    out=$("$AMILISP" --no-userinit --heap 64M --non-interactive \
          --load "$WORK/room.lisp" 2>&1)
fi

check "the array is live when ROOM runs" "LEN=12000000" \
      "$(echo "$out" | grep '^LEN=')"

line=$(echo "$out" | grep ' bytes used (' | head -1)
used=$(echo "$line" | sed -n 's/^ *\([0-9][0-9]*\) \/ .*/\1/p')
tot=$(echo "$line" | sed -n 's/^ *[0-9]* \/ \([0-9][0-9]*\) bytes used.*/\1/p')
pct=$(echo "$line" | sed -n 's/.*(\([0-9][0-9]*\)%).*/\1/p')

check "ROOM prints the heap line" "yes" "$([ -n "$used" ] && [ -n "$tot" ] && [ -n "$pct" ] && echo yes)"
check "the arena is the 64M asked for" "67108864" "$tot"
check "more than 42 MB are in use (the overflow region)" "yes" \
      "$([ "${used:-0}" -gt 44040192 ] && echo yes)"
# Both numbers fit a shell integer; expr-free arithmetic for dash/ash.
want=$(( used * 100 / tot ))
check "the percentage agrees with the byte counts" "$want" "$pct"

echo "$passed passed, $failed failed, $total total"
[ "$failed" -eq 0 ]
