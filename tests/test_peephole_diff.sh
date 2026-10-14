#!/bin/sh
# Differential test for the bytecode peephole post-pass (spec 1.8).
#
# Runs tests/peephole-corpus.lisp with AMILISP_FORCE_SPEED=0 (pass disabled
# everywhere), =1 (the default: rewrites + superinstruction fusion), =2 and
# =3 (forced on for every compile in the process) and requires byte-identical
# output.  A mis-relocated jump, broken NLX
# landing pad, or over-eager deletion shows up as a diff (or a crash).
#
# AMILISP_FORCE_SPEED pins the effective (optimize (speed N)) for the whole
# process, overriding declaim/declare — so ANY corpus works unmodified;
# growing test coverage automatically grows peephole coverage.
#
#   sh tests/test_peephole_diff.sh build/host/amilisp
#
# Also runs under the gc-stress binary (make test-gc-stress) to exercise the
# speed-3 compile path under forced compaction.

AMILISP="${1:-build/host/amilisp}"
CORPUS="$(dirname "$0")/peephole-corpus.lisp"

# macOS has no `timeout`; coreutils installs it as `gtimeout` (same detection
# as the other shell tests — a hardcoded `timeout` fails every run with 127
# on the macOS CI runner).
TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)
if [ -z "$TIMEOUT" ]; then
    echo "SKIP test_peephole_diff: neither timeout nor gtimeout on PATH"
    echo "0 passed, 0 failed, 0 total"
    exit 0
fi

TMPDIR="${TMPDIR:-/tmp}"
WORK="$TMPDIR/clamiga_peep_$$"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

passed=0
failed=0

if [ ! -x "$AMILISP" ]; then
    echo "  SKIP  peephole diff: no binary at $AMILISP"
    echo "0 passed, 0 failed, 0 total"
    exit 0
fi

run_at_speed() {
    speed="$1"; out="$2"
    AMILISP_FORCE_SPEED="$speed" "$TIMEOUT" 120 "$AMILISP" --no-userinit \
        --non-interactive --load "$CORPUS" >"$out.raw" 2>&1
    rc=$?
    # strip "; [boot] N ms" style progress lines — timing noise, not results
    grep -v '^; ' "$out.raw" >"$out"
    return $rc
}

check() {
    name="$1"; cond="$2"
    if [ "$cond" = "0" ]; then
        echo "  ok  $name"
        passed=$((passed + 1))
    else
        echo "FAIL  $name"
        failed=$((failed + 1))
    fi
}

run_at_speed 0 "$WORK/s0.out"; rc0=$?
run_at_speed 1 "$WORK/s1.out"; rc1=$?
run_at_speed 2 "$WORK/s2.out"; rc2=$?
run_at_speed 3 "$WORK/s3.out"; rc3=$?

check "corpus_completes_speed0" "$rc0"
check "corpus_completes_speed1" "$rc1"
check "corpus_completes_speed2" "$rc2"
check "corpus_completes_speed3" "$rc3"

grep -q "CORPUS-DONE" "$WORK/s0.out"; check "corpus_done_speed0" "$?"
grep -q "CORPUS-DONE" "$WORK/s3.out"; check "corpus_done_speed3" "$?"

if diff "$WORK/s0.out" "$WORK/s1.out" >/dev/null 2>&1; then
    check "speed0_vs_speed1_identical" 0
else
    check "speed0_vs_speed1_identical" 1
    diff "$WORK/s0.out" "$WORK/s1.out" | head -20
fi

if diff "$WORK/s0.out" "$WORK/s2.out" >/dev/null 2>&1; then
    check "speed0_vs_speed2_identical" 0
else
    check "speed0_vs_speed2_identical" 1
    diff "$WORK/s0.out" "$WORK/s2.out" | head -20
fi

if diff "$WORK/s0.out" "$WORK/s3.out" >/dev/null 2>&1; then
    check "speed0_vs_speed3_identical" 0
else
    check "speed0_vs_speed3_identical" 1
    diff "$WORK/s0.out" "$WORK/s3.out" | head -20
fi

echo "$passed passed, $failed failed, $((passed + failed)) total"
[ "$failed" -eq 0 ]
