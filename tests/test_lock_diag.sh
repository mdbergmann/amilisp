#!/bin/sh
# Slow-lock-wait diagnostic (CLAMIGA_LOCK_DIAG): a blocking MP:ACQUIRE-LOCK
# that waits past the threshold must report — to stderr, from inside the
# stalled wait — the contended lock (name), the waiting thread, the
# current holder (name + wait state), and finally the total wait once the
# lock is acquired.  Built for triaging intermittent multi-minute stalls
# (a loader parked on a lock another thread holds) from amilisp's own
# output, where MP:DUMP-THREAD-WAITS would need a watchdog to fire at
# exactly the right moment.
#
# Also verifies the diagnostic is OFF by default: without the env var the
# same contended run must print nothing.
#
# Run: sh tests/test_lock_diag.sh build/host/amilisp

CLAMIGA="${1:-build/host/amilisp}"

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)
if [ -z "$TIMEOUT" ]; then
    echo "SKIP test_lock_diag: neither timeout nor gtimeout on PATH"
    exit 0
fi

tmp=$(mktemp "${TMPDIR:-/tmp}/clamiga_lockdiag_XXXXXX") || exit 1
trap 'rm -f "$tmp"' EXIT

# Worker takes the named lock, signals via a shared global (so the main
# thread cannot win the race and acquire first), holds it 0.6s, releases.
# Main then blocks on the lock well past the 100ms threshold.
cat > "$tmp" <<'EOF'
(defvar *held* nil)
(let* ((lk (mp:make-lock "device-registry"))
       (th (mp:make-thread
            (lambda ()
              (mp:acquire-lock lk)
              (setf *held* t)
              (sleep 0.6)
              (mp:release-lock lk))
            :name "device-init")))
  (loop until *held* do (sleep 0.01))
  (mp:acquire-lock lk)
  (mp:release-lock lk)
  (mp:join-thread th)
  (format t "LOCK-DIAG-DONE~%"))
EOF

fail() {
    echo "FAIL lock_diag ($1)"
    echo "$out" | tail -8 | sed 's/^/    /'
    echo "0 passed, 1 failed, 1 total"
    exit 1
}

# 1. Enabled at a 100ms threshold: both report forms must appear, naming
#    the lock, the waiter, and the holder.
out=$(CLAMIGA_LOCK_DIAG=100 "$TIMEOUT" 30 "$CLAMIGA" --no-userinit \
      --non-interactive --load "$tmp" </dev/null 2>&1)
status=$?
[ $status -eq 0 ] || fail "exit $status: hang or crash"
printf '%s' "$out" | grep -q "LOCK-DIAG-DONE" || fail "program did not complete"
printf '%s' "$out" | \
    grep -q 'waiting [0-9]* ms for lock "device-registry" held by tid=[0-9]* "device-init"' \
    || fail "no still-waiting report naming lock and holder"
printf '%s' "$out" | \
    grep -q 'acquired lock "device-registry" after [0-9]* ms' \
    || fail "no acquired-after report"

# 2. Default (env unset): the identical contended run must be silent.
out=$(env -u CLAMIGA_LOCK_DIAG "$TIMEOUT" 30 "$CLAMIGA" --no-userinit \
      --non-interactive --load "$tmp" </dev/null 2>&1)
status=$?
[ $status -eq 0 ] || fail "exit $status on default run: hang or crash"
printf '%s' "$out" | grep -q "LOCK-DIAG-DONE" || fail "default run did not complete"
printf '%s' "$out" | grep -q "MP:ACQUIRE-LOCK diag" \
    && fail "diagnostic printed with CLAMIGA_LOCK_DIAG unset"

echo "  ok  lock_diag (report + silence-by-default)"
echo "1 passed, 0 failed, 1 total"
exit 0
