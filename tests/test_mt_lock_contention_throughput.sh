#!/bin/sh
# Regression test (GC audit tier 4 follow-up): contended MP:ACQUIRE-LOCK
# handoff must be wakeup-driven, not sleep-polled.
#
# The tier-4 I5 fix made blocking acquire-lock interruptible by replacing
# platform_mutex_lock with a trylock loop; past the 256-yield spin phase
# it slept 10ms per round.  A waiter that escalated once (long hold, GC
# pause, preemption) stayed on the 10ms grid for the rest of that acquire
# even after the lock came free — under sento's message-box contention
# (8 producers + workers on one queue lock) that collapsed message
# throughput ~6x and its mixed ask/tell counter test failed its 3s
# assert-cond window deterministically.
#
# The fix parked contended acquirers on a shared condvar that release-lock
# broadcast; since specs/mp-locks-heap-words.md each contended acquirer
# parks on its own per-thread handle and a release unparks exactly one
# registered waiter — a handoff still wakes the waiter immediately, which
# is what the probe below keeps pinned.
#
# Probe: hold the lock while a waiter escalates past its spin phase
# (60ms), then release and measure release->acquired latency.  Sleep-poll
# wakes on the 10ms grid (avg ~5ms per round); parking wakes on the
# broadcast (sub-ms).  Average over 20 rounds, assert < 3ms — a wide
# margin on both sides, and a latency probe stays valid on a loaded CI
# host where a total-throughput deadline would flake.
#
# Also hammers one lock from 8 threads with work under the lock and
# checks the final counter: mutual exclusion + every parked waiter woken.
#
# Run: sh tests/test_mt_lock_contention_throughput.sh build/host/amilisp

AMILISP="${1:-build/host/amilisp}"

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)
if [ -z "$TIMEOUT" ]; then
    echo "SKIP test_mt_lock_contention_throughput: neither timeout nor gtimeout on PATH"
    exit 0
fi

tmp=$(mktemp "${TMPDIR:-/tmp}/amilisp_mtlockthr_XXXXXX") || exit 1
trap 'rm -f "$tmp"' EXIT

cat > "$tmp" <<'EOF'
;; Probe 1: parked-waiter handoff latency.
(defvar *lk* (mp:make-lock))
(defvar *acq-time* nil)
(defvar *lat-sum-ms* 0)
(defconstant +rounds+ 20)
(dotimes (i +rounds+)
  (mp:acquire-lock *lk*)
  (let ((waiter (mp:make-thread
                 (lambda ()
                   (mp:acquire-lock *lk*)
                   (setq *acq-time* (get-internal-real-time))
                   (mp:release-lock *lk*)))))
    ;; Give the waiter time to exhaust its yield-spin phase and park.
    (sleep 0.06)
    (let ((rel-time (get-internal-real-time)))
      (mp:release-lock *lk*)
      (mp:join-thread waiter)
      (setq *lat-sum-ms*
            (+ *lat-sum-ms*
               (round (* 1000 (- *acq-time* rel-time))
                      internal-time-units-per-second))))))
(format t "HANDOFF-LATENCY-TOTAL-MS: ~a over ~a rounds~%" *lat-sum-ms* +rounds+)
(if (< *lat-sum-ms* (* 3 +rounds+))
    (format t "HANDOFF-LATENCY-OK~%")
    (format t "HANDOFF-LATENCY-BAD: avg ~ams >= 3ms — sleep-poll regression~%"
            (/ *lat-sum-ms* +rounds+)))

;; Probe 2: contended-hammer correctness — mutual exclusion holds and
;; every parked waiter is eventually woken (no lost broadcast).
(defvar *counter* 0)
(defconstant +threads+ 8)
(defconstant +iters+ 1500)
(let ((workers
        (loop repeat +threads+
              collect (mp:make-thread
                       (lambda ()
                         (dotimes (i +iters+)
                           (mp:acquire-lock *lk*)
                           (let ((work 0))
                             (dotimes (j 400) (setq work (+ work j)))
                             (setq *counter* (+ (1+ *counter*) (* 0 work))))
                           (mp:release-lock *lk*)))))))
  (dolist (th workers) (mp:join-thread th)))
(if (= *counter* (* +threads+ +iters+))
    (format t "CONTENTION-COUNTER-OK~%")
    (format t "CONTENTION-COUNTER-BAD: ~a (expected ~a)~%"
            *counter* (* +threads+ +iters+)))
(quit)
EOF

out=$("$TIMEOUT" 120 "$AMILISP" --load "$tmp" </dev/null 2>&1)
status=$?

if [ $status -ne 0 ]; then
    echo "FAIL test_mt_lock_contention_throughput: timed out or crashed (status $status)"
    echo "$out" | tail -20
    exit 1
fi

echo "$out" | grep "HANDOFF-LATENCY-TOTAL-MS"

pass=0
fail=0
if echo "$out" | grep -q "HANDOFF-LATENCY-OK"; then
    echo "  ok  parked-waiter handoff latency (wakeup-driven, avg < 3ms)"
    pass=$((pass + 1))
else
    echo "FAIL parked-waiter handoff latency:"
    echo "$out" | grep "HANDOFF-LATENCY" | tail -3
    fail=$((fail + 1))
fi
if echo "$out" | grep -q "CONTENTION-COUNTER-OK"; then
    echo "  ok  contended hammer counter (8x1500, mutual exclusion + wakeups)"
    pass=$((pass + 1))
else
    echo "FAIL contended hammer counter:"
    echo "$out" | grep "CONTENTION-COUNTER" | tail -3
    fail=$((fail + 1))
fi

echo "$pass passed, $fail failed, $((pass + fail)) total"
[ $fail -eq 0 ] || exit 1
exit 0
