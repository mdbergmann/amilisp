#!/bin/sh
# Platform file-I/O trace (CLAMIGA_IO_DIAG): every file operation must print
# an "[IO] <t>ms <op> ..." entry line to stderr BEFORE the blocking call —
# built so a process found hanging inside an OS file call still names the
# operation, path, and handle it entered with (MorphOS quicklisp-install
# hang triage: task in WAIT state right after a compacting GC).  With the
# env var unset the same run must print no [IO] lines at all.
#
# Run: sh tests/test_io_diag.sh build/host/amilisp

CLAMIGA="${1:-build/host/amilisp}"

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)
if [ -z "$TIMEOUT" ]; then
    echo "SKIP test_io_diag: neither timeout nor gtimeout on PATH"
    exit 0
fi

tmp=$(mktemp "${TMPDIR:-/tmp}/clamiga_iodiag_XXXXXX") || exit 1
out=$(mktemp "${TMPDIR:-/tmp}/clamiga_iodiag_out_XXXXXX") || exit 1
dat=$(mktemp "${TMPDIR:-/tmp}/clamiga_iodiag_dat_XXXXXX") || exit 1
trap 'rm -f "$tmp" "$out" "$dat"' EXIT

fail=0

cat > "$tmp" <<EOF
(with-open-file (s "$dat" :direction :output :if-exists :supersede)
  (write-string "io-diag test payload" s))
(with-open-file (s "$dat" :direction :input)
  (read-line s))
EOF

# Off by default: no [IO] lines.
"$TIMEOUT" 60 "$CLAMIGA" --non-interactive --load "$tmp" < /dev/null > "$out" 2>&1
if grep -q '^\[IO\] ' "$out"; then
    echo "FAIL: [IO] output with diagnostic unset"
    cat "$out"
    fail=1
fi

# On: open of the payload file (with its path) and matching close traced.
# Match on the basename: TMPDIR can carry a trailing slash (macOS), which the
# Lisp pathname layer normalizes out of the traced path.
datbase=$(basename "$dat")
CLAMIGA_IO_DIAG=1 "$TIMEOUT" 60 "$CLAMIGA" --non-interactive --load "$tmp" < /dev/null > "$out" 2>&1
if ! grep -q "^\[IO\] [0-9]*ms open \".*$datbase\" mode=" "$out"; then
    echo "FAIL: no [IO] open line for the payload file"
    cat "$out"
    fail=1
fi
if ! grep -q '^\[IO\] [0-9]*ms open -> fh=[0-9]*' "$out"; then
    echo "FAIL: no [IO] open result line"
    fail=1
fi
if ! grep -q '^\[IO\] [0-9]*ms close fh=[0-9]*' "$out"; then
    echo "FAIL: no [IO] close line"
    fail=1
fi

exit $fail
