#!/bin/sh
# Interactive debugger backtrace: `:bt [n|all]`, a growable buffer, and frames
# that survive evaluation at the Debug> prompt.
#
# All three legs are about one complaint: the debugger printed 20 frames and a
# "... N more frames" tail with no way to see the rest.
#
#   1. `:bt all` / `:bt N` re-render the ERROR-TIME frames at a chosen depth,
#      so the frames behind that tail become reachable.  Plain `:bt` keeps the
#      default window.  A bad argument explains itself instead of being
#      evaluated as Lisp.
#
#   2. The formatted text grows onto the C heap.  With ~40 frames carrying long
#      source paths, `:bt all` is well over the old fixed 2048-byte ceiling —
#      which used to cut the text off mid-frame, a second invisible limit
#      stacked on top of the frame limit.
#
#   3. (ext:backtrace) at the Debug> prompt reports the error-time stack.  The
#      mini-REPL used to zero cl_vm.sp/fp before evaluating, destroying the very
#      frames the user entered the debugger to inspect: (ext:backtrace) returned
#      its own single frame, and :bt N had nothing left to re-render.
#
# Run: sh tests/test_debugger_backtrace.sh build/host/amilisp

CLAMIGA="${1:-build/host/amilisp}"

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)
if [ -z "$TIMEOUT" ]; then
    echo "SKIP test_debugger_backtrace: neither timeout nor gtimeout on PATH"
    exit 0
fi

EXPECT=$(command -v expect 2>/dev/null || true)
if [ -z "$EXPECT" ]; then
    echo "SKIP test_debugger_backtrace: expect(1) not on PATH"
    exit 0
fi

# The debugger prompt only appears on a real tty, and the frame text has to be
# long enough to outgrow the inline buffer — hence a deep directory.
dir=$(mktemp -d "${TMPDIR:-/tmp}/clamiga_dbgbt_XXXXXX") || exit 1
trap 'rm -rf "$dir"' EXIT

deep="$dir/a-long-directory-name-making-every-backtrace-line-big/so-the-rendered-text-passes-the-old-two-kilobyte-ceiling"
mkdir -p "$deep" || exit 1

chain="$deep/chain.lisp"
: > "$chain"
echo '(defun lvl0 () (error "boom") 1)' >> "$chain"
i=1
while [ "$i" -le 39 ]; do
    echo "(defun lvl$i () (1+ (lvl$((i - 1)))))" >> "$chain"
    i=$((i + 1))
done
echo '(lvl39)' >> "$chain"

script="$dir/drive.exp"
out="$dir/out.txt"

# Markers delimit each leg's output so the greps below cannot be satisfied by
# some other section of the transcript.
cat > "$script" <<EOF
set timeout 40
spawn [lindex \$argv 0] --no-userinit
expect {
    "COMMON-LISP-USER>" {}
    timeout { puts "EXPECT-FAIL: no REPL prompt"; exit 1 }
}
send "(load \\"$chain\\")\r"
expect {
    "Debug>" {}
    timeout { puts "EXPECT-FAIL: error did not enter the debugger"; exit 1 }
}

# Leg 3: the error-time frames must still be here to count.
send "(format t \\"~%===LEN ~D===~%\\" (length (ext:backtrace)))\r"
expect "Debug>"

# Leg 1+2: every frame, past both the 20-frame window and the old byte ceiling.
send "(princ \\"===ALL===\\")\r"
expect "Debug>"
send ":bt all\r"
expect "Debug>"

# Leg 1: an explicit small window, with an accurate withheld-frame tail.
send "(princ \\"===FIVE===\\")\r"
expect "Debug>"
send ":bt 5\r"
expect "Debug>"

# A failed eval must not leave a different error's backtrace behind.
send "(this-function-does-not-exist)\r"
expect "Debug>"
send "(princ \\"===AFTERBAD===\\")\r"
expect "Debug>"
send ":bt\r"
expect "Debug>"

# A bad argument is a usage message, not a Lisp evaluation.
send "(princ \\"===USAGE===\\")\r"
expect "Debug>"
send ":bt banana\r"
expect "Debug>"

# A negative count is rejected the same way, not silently accepted.
send "(princ \\"===USAGE_NEG===\\")\r"
expect "Debug>"
send ":bt -1\r"
expect "Debug>"

# A count past this VM's hard frame ceiling is rejected too.
send "(princ \\"===USAGE_HUGE===\\")\r"
expect "Debug>"
send ":bt 999999999\r"
expect "Debug>"
send "(princ \\"===END===\\")\r"
expect "Debug>"

exec kill -9 [exp_pid]
exit 0
EOF

if ! "$TIMEOUT" 120 "$EXPECT" -f "$script" "$CLAMIGA" > "$out" 2>&1; then
    echo "FAIL: could not drive the interactive debugger"
    cat "$out"
    exit 1
fi

fail=0

# Extract the transcript between two markers.
section() {
    awk -v a="$1" -v b="$2" '
        index($0, a) { inside = 1; next }
        inside && index($0, b) { inside = 0 }
        inside { print }
    ' "$out"
}

# ---- 3. (ext:backtrace) sees the error-time stack, not just its own frame ----
len=$(sed -n 's/.*===LEN \([0-9]*\)===.*/\1/p' "$out" | head -1)
if [ -z "$len" ]; then
    echo "FAIL: (ext:backtrace) produced no length at the Debug> prompt"
    fail=1
elif [ "$len" -lt 40 ]; then
    echo "FAIL: (ext:backtrace) at Debug> saw $len frames, expected >= 40"
    echo "      (the mini-REPL is discarding the error-time frames again)"
    fail=1
fi

# ---- 1. :bt all reaches past the default 20-frame window ----
all=$(section "===ALL===" "===FIVE===")
if ! echo "$all" | grep -q "LVL39"; then
    echo "FAIL: ':bt all' did not reach LVL39 (outermost frame of the chain)"
    echo "$all"
    fail=1
fi
if echo "$all" | grep -q "more frames"; then
    echo "FAIL: ':bt all' still withheld frames"
    fail=1
fi

# ---- 2. the rendered text outgrew the old fixed 2048-byte buffer ----
bytes=$(echo "$all" | grep -c "^  *[0-9]*: ")
size=$(echo "$all" | wc -c)
if [ "$bytes" -lt 40 ]; then
    echo "FAIL: ':bt all' rendered only $bytes frame lines, expected >= 40"
    fail=1
fi
if [ "$size" -le 2048 ]; then
    echo "FAIL: ':bt all' produced $size bytes — too small to prove the buffer grew"
    fail=1
fi

# ---- 1. :bt N caps the window and reports the remainder ----
five=$(section "===FIVE===" "this-function-does-not-exist")
shown=$(echo "$five" | grep -c "^  *[0-9]*: ")
if [ "$shown" -ne 5 ]; then
    echo "FAIL: ':bt 5' showed $shown frames, expected 5"
    echo "$five"
    fail=1
fi
if ! echo "$five" | grep -q "\.\.\. [0-9]* more frames"; then
    echo "FAIL: ':bt 5' did not report the withheld frames"
    fail=1
fi

# ---- a failed eval must not replace the backtrace being debugged ----
afterbad=$(section "===AFTERBAD===" "===USAGE===")
if ! echo "$afterbad" | grep -q "LVL0"; then
    echo "FAIL: ':bt' after a failed eval lost the original error's backtrace"
    echo "$afterbad"
    fail=1
fi
if echo "$afterbad" | grep -q "THIS-FUNCTION-DOES-NOT-EXIST"; then
    echo "FAIL: ':bt' after a failed eval showed the typo's backtrace instead"
    fail=1
fi

# ---- a bad :bt argument explains itself ----
usage=$(section "===USAGE===" "===USAGE_NEG===")
if ! echo "$usage" | grep -q "Usage: :bt"; then
    echo "FAIL: ':bt banana' did not print usage"
    echo "$usage"
    fail=1
fi

# ---- a negative :bt count is rejected with the same usage message ----
usage_neg=$(section "===USAGE_NEG===" "===USAGE_HUGE===")
if ! echo "$usage_neg" | grep -q "Usage: :bt"; then
    echo "FAIL: ':bt -1' did not print usage"
    echo "$usage_neg"
    fail=1
fi

# ---- a :bt count past frame_size is rejected with the same usage message ----
usage_huge=$(section "===USAGE_HUGE===" "===END===")
if ! echo "$usage_huge" | grep -q "Usage: :bt"; then
    echo "FAIL: ':bt 999999999' did not print usage"
    echo "$usage_huge"
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    echo "PASS test_debugger_backtrace"
fi
exit "$fail"
