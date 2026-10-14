#!/bin/sh
# Regression test: amilisp must never hang after a --load completes.
#
# Bug: `make test` -> host-cold-test ran amilisp in INTERACTIVE mode without
# redirecting stdin, so once the loaded script (the sento suite) finished,
# main.c fell through to cl_repl(), which blocked in platform_read_line() on a
# terminal that never sends EOF.  `make test` hung forever after the suite had
# already passed.
#
# Two independent guarantees pin the fix the cold-test now relies on:
#   A. --non-interactive runs the --load/--eval actions then exits, never
#      entering the REPL — even when stdin stays open (here a `sleep` holds the
#      pipe's write end open past the timeout).
#   B. With stdin redirected from /dev/null, the post-load REPL sees EOF
#      immediately and exits cleanly.
# A regression on either path would block on stdin and trip the timeout (124).
#
# Run: sh tests/test_load_exit.sh build/host/amilisp

CLAMIGA="${1:-build/host/amilisp}"

# Both assertions need a timeout binary to bound the "does it hang?" check.
# Skip cleanly when absent (matches the repo's skip-when-prereq-missing rule).
TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)
if [ -z "$TIMEOUT" ]; then
    echo "SKIP test_load_exit: neither timeout nor gtimeout on PATH"
    exit 0
fi

passed=0
failed=0
total=0

tmp=$(mktemp "${TMPDIR:-/tmp}/clamiga_load_exit_XXXXXX") || exit 1
fifo=""
cleanup() { rm -f "$tmp"; [ -n "$fifo" ] && rm -f "$fifo"; }
trap cleanup EXIT INT TERM
printf '(format t "COLD-LOAD-DONE~%%")\n' > "$tmp"

# desc, exit-code, captured-output -> assert clean exit (not 124) + marker.
check() {
    desc="$1"; ec="$2"; out="$3"
    total=$((total + 1))
    if [ "$ec" -eq 124 ]; then
        echo "  FAIL  $desc (timed out, ec=124 — amilisp hung after load)"
        echo "    output: $(echo "$out" | head -5)"
        failed=$((failed + 1))
    elif [ "$ec" -ne 0 ]; then
        echo "  FAIL  $desc (ec=$ec)"
        echo "    output: $(echo "$out" | head -5)"
        failed=$((failed + 1))
    elif ! echo "$out" | grep -q "COLD-LOAD-DONE"; then
        echo "  FAIL  $desc (no COLD-LOAD-DONE marker)"
        echo "    output: $(echo "$out" | head -5)"
        failed=$((failed + 1))
    else
        echo "  ok  $desc"
        passed=$((passed + 1))
    fi
}

# --- Assert A: --non-interactive exits even when stdin never closes ---
# `sleep 12` holds the FIFO write-end open longer than the 10s timeout; a
# regression that drops into the REPL would block reading it and time out (124).
# Using a FIFO + background sleep avoids blocking $(...) for the full 12s
# (POSIX pipeline-wait would hold $(sleep 12 | ...) until sleep finishes).
fifo=$(mktemp -u "${TMPDIR:-/tmp}/clamiga_fifo_XXXXXX")
mkfifo "$fifo"
sleep 12 >"$fifo" &
sleep_pid=$!
out=$("$TIMEOUT" 10 "$CLAMIGA" --no-userinit --heap 8M --non-interactive --load "$tmp" <"$fifo" 2>&1)
ec=$?
kill "$sleep_pid" 2>/dev/null
wait "$sleep_pid" 2>/dev/null
rm -f "$fifo"; fifo=""
check "non_interactive_does_not_block_on_open_stdin" "$ec" "$out"

# --- Assert B: </dev/null gives the post-load REPL an immediate EOF ---
out=$("$TIMEOUT" 10 "$CLAMIGA" --no-userinit --heap 8M --load "$tmp" </dev/null 2>&1)
check "devnull_stdin_exits_post_load_repl" "$?" "$out"

echo ""
echo "$passed passed, $failed failed, $total total"
if [ "$failed" -gt 0 ]; then
    echo "FAIL"
    exit 1
fi
echo "PASS"
exit 0
