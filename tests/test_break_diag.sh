#!/bin/sh
# Ctrl-C break-in + per-collection GC trace (runtime diagnostics).
#
# 1. CLAMIGA_GC_DIAG=1 must print one "[GC] ... pause=...us used=..." line
#    per collection (both the generational and the classic collector), and
#    must print NOTHING when the env var is unset.
#
# 2. SIGINT delivered to a clamiga wedged in a Lisp loop must interrupt it:
#    the VM polls platform_break_pending() at loop back-edges, captures a
#    backtrace at the interrupted opcode, and funcalls CL:BREAK.  In a
#    non-interactive run the debugger is unavailable, so invoke-debugger
#    returns and the break unwinds to top level — the observable contract is
#    the ";; Interrupt (Ctrl-C)" notice plus a backtrace naming the loop,
#    and the process exiting instead of spinning forever.
#
# 3. (expect(1) only, SKIPped without it) Interactive: Ctrl-C in a running
#    loop enters the debugger; selecting the CONTINUE restart by number
#    resumes the interrupted loop (regression: the debugger's numeric
#    restart selection applied the handler in place and threw its return
#    value to the restart tag, where the catch landing APPLYed it — "not a
#    callable function" on every numeric restart selection; it must throw
#    the (handler . args) dispatch cons like INVOKE-RESTART).
#
# Run: sh tests/test_break_diag.sh build/host/clamiga

CLAMIGA="${1:-build/host/clamiga}"

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)
if [ -z "$TIMEOUT" ]; then
    echo "SKIP test_break_diag: neither timeout nor gtimeout on PATH"
    exit 0
fi

tmp=$(mktemp "${TMPDIR:-/tmp}/clamiga_breakdiag_XXXXXX") || exit 1
out=$(mktemp "${TMPDIR:-/tmp}/clamiga_breakdiag_out_XXXXXX") || exit 1
trap 'rm -f "$tmp" "$out"' EXIT

fail=0

# ---- 1. GC trace ----
cat > "$tmp" <<'EOF'
(dotimes (i 3)
  (make-array 100000)
  (ext:gc))
EOF

"$TIMEOUT" 60 "$CLAMIGA" --non-interactive --load "$tmp" < /dev/null > "$out" 2>&1
if grep -q '^\[GC\] .*ms #[0-9]* .* pause=[0-9]*us used=[0-9]*K/[0-9]*K$' "$out"; then
    echo "FAIL: [GC] output with diagnostic unset"
    fail=1
fi

CLAMIGA_GC_DIAG=1 "$TIMEOUT" 60 "$CLAMIGA" --non-interactive --load "$tmp" < /dev/null > "$out" 2>&1
if ! grep -q '^\[GC\] [0-9]*ms #[0-9]* [a-z-]* pause=[0-9]*us used=[0-9]*K/[0-9]*K$' "$out"; then
    echo "FAIL: no [GC] trace line with CLAMIGA_GC_DIAG=1 (default collector)"
    cat "$out"
    fail=1
fi

CLAMIGA_GC_DIAG=1 CLAMIGA_GENGC=0 "$TIMEOUT" 60 "$CLAMIGA" --non-interactive --load "$tmp" < /dev/null > "$out" 2>&1
if ! grep -q '^\[GC\] [0-9]*ms #[0-9]* \(sweep\|immix\|compact\|compact-fallback\) pause=' "$out"; then
    echo "FAIL: no [GC] trace line with CLAMIGA_GC_DIAG=1 (classic collector)"
    cat "$out"
    fail=1
fi

# ---- 2. SIGINT break, non-interactive ----
#
# Windows: skipped, and not because the feature is missing.  clamiga installs
# a console control handler there, but nothing in this harness can deliver a
# Ctrl-C to it: a native process is not in the MSYS shell's signal world, so
# `kill -INT` on it terminates rather than interrupts.  The break-in itself is
# exercised by hand at the REPL.
case "$(uname -s)" in
    MINGW*|MSYS*|CLANGARM64*|CLANG64*|UCRT64*)
        echo "skip: SIGINT break leg (no signal delivery to a native process"
        echo "      from the MSYS shell — the console-control handler is"
        echo "      interactive-only here)"
        skip_sigint=1
        ;;
    *) skip_sigint=0 ;;
esac

if [ "$skip_sigint" = "0" ]; then
cat > "$tmp" <<'EOF'
(format t "SPIN-START~%")
(finish-output)
(loop for i from 0 do (identity i))
EOF

# No $TIMEOUT wrapper here: the SIGINT must reach clamiga itself, not the
# timeout process.  A manual watchdog below bounds the run instead.
"$CLAMIGA" --non-interactive --load "$tmp" < /dev/null > "$out" 2>&1 &
pid=$!
# Wait for the loop to be running (bounded).
n=0
while ! grep -q "SPIN-START" "$out" 2>/dev/null; do
    n=$((n + 1))
    if [ "$n" -gt 100 ]; then break; fi
    sleep 0.1
done
sleep 0.5
kill -INT "$pid" 2>/dev/null
# The break must terminate the run (unwind to top level -> load aborts).
n=0
while kill -0 "$pid" 2>/dev/null; do
    n=$((n + 1))
    if [ "$n" -gt 100 ]; then
        echo "FAIL: clamiga still spinning 10s after SIGINT"
        kill -9 "$pid" 2>/dev/null
        fail=1
        break
    fi
    sleep 0.1
done
wait "$pid" 2>/dev/null
if ! grep -q "Interrupt (Ctrl-C)" "$out"; then
    echo "FAIL: no ';; Interrupt (Ctrl-C)' notice after SIGINT"
    cat "$out"
    fail=1
fi
if ! grep -q "Backtrace:" "$out"; then
    echo "FAIL: no backtrace after SIGINT break"
    cat "$out"
    fail=1
fi
fi  # skip_sigint

# ---- 3. Interactive continue (needs a PTY -> expect) ----
EXPECT=$(command -v expect 2>/dev/null || true)
if [ -z "$EXPECT" ]; then
    echo "skip: interactive continue leg (expect(1) not on PATH)"
else
    cat > "$tmp" <<'EOF'
set timeout 20
spawn [lindex $argv 0]
expect "COMMON-LISP-USER>"
send "(loop for i from 0 do (identity i))\r"
sleep 1
send "\003"
expect {
    "Debugger entered" {}
    timeout { puts "EXPECT-FAIL: no debugger on Ctrl-C"; exit 1 }
}
expect "Debug>"
send "0\r"
sleep 2
send "\003"
expect {
    "Debugger entered" {}
    timeout { puts "EXPECT-FAIL: loop did not resume after CONTINUE"; exit 1 }
}
# Break -> CONTINUE resumed the loop -> second break: the regression is
# covered.  Don't script the debugger's exit (timing-sensitive); just kill
# the spawned process.
exec kill -9 [exp_pid]
exit 0
EOF
    if ! "$TIMEOUT" 60 "$EXPECT" -f "$tmp" "$CLAMIGA" > "$out" 2>&1; then
        echo "FAIL: interactive break/CONTINUE leg"
        cat "$out"
        fail=1
    fi
fi

exit $fail
