#!/bin/sh
# Deterministic reproduction of the lost lock wakeup found by the sento
# shared/ask-s cell on the first heap-word lock implementation
# (specs/mp-locks-heap-words.md, "Implementation notes").
#
# The interleaving: a contended acquirer (past its spin phase) marks the
# holder's state word CONTENDED, the holder releases and scans the waiter
# list BEFORE the acquirer is registered (nobody to wake), a barging
# newcomer takes the now-free lock with a plain word, the acquirer — now
# registered — sees "held" and parks, and the newcomer's release, seeing no
# CONTENDED bit, never scans: the parked thread is stranded on a free lock.
# The fix registers first and then marks whoever holds the lock NOW.
#
# The three-way timing cannot be forced from Lisp, so this runs against a
# -DDEBUG_THREAD_RACE_HOOKS binary whose lock_acquire_slow sleeps
# AMILISP_RACE_LOCK_MARK_DELAY_MS between registering and marking
# (AMILISP_RACE_SELFTEST=0 keeps that binary's start-up self-test from
# running and exiting before the program).  With
# the window held open for 300 ms, the main thread releases the lock and
# immediately re-acquires it plain (the barge) inside that window every
# time; the waiter must still be woken by main's final release.
#
# Run: sh tests/test_mt_lock_barge_race.sh build/host/amilisp build/host-race/amilisp

AMILISP="${1:-build/host/amilisp}"
RACE="${2:-build/host-race/amilisp}"

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)
if [ -z "$TIMEOUT" ]; then
    echo "SKIP test_mt_lock_barge_race: neither timeout nor gtimeout on PATH"
    exit 0
fi
if [ ! -x "$RACE" ]; then
    echo "SKIP test_mt_lock_barge_race: no race-hook binary at $RACE"
    exit 0
fi

tmp=$(mktemp "${TMPDIR:-/tmp}/clamiga_barge_XXXXXX") || exit 1
trap 'rm -f "$tmp"' EXIT

cat > "$tmp" <<'EOF'
(defvar *lk* (mp:make-lock "barged"))
(defvar *got* nil)
(mp:acquire-lock *lk*)
(let ((w (mp:make-thread (lambda () (mp:acquire-lock *lk*) (setf *got* t) (mp:release-lock *lk*) :done))))
  ;; 100 ms: the waiter is past its spin phase, registered, and sleeping in
  ;; the widened window (300 ms) before it marks the holder.
  (sleep 0.1)
  (mp:release-lock *lk*)          ; scan finds the registered waiter: a token
  (mp:acquire-lock *lk*)          ; the barge: plain word, no CONTENDED bit
  (sleep 0.5)                     ; the waiter wakes inside this hold and
  (mp:release-lock *lk*)          ;   must have marked THIS word
  (format t "BARGE-RACE=~a,~a~%" (mp:join-thread w) (if *got* "GOT" "STRANDED")))

;; Same shape with the roles doubled: two waiters, two barges.
(defvar *lk2* (mp:make-lock "barged-2"))
(defvar *n2* 0)
(mp:acquire-lock *lk2*)
(let ((ws (loop repeat 2 collect
            (mp:make-thread (lambda () (mp:acquire-lock *lk2*) (incf *n2*) (mp:release-lock *lk2*))))))
  (sleep 0.1)
  (dotimes (i 2)
    (mp:release-lock *lk2*)
    (mp:acquire-lock *lk2*)
    (sleep 0.5))
  (mp:release-lock *lk2*)
  (dolist (w ws) (mp:join-thread w))
  (format t "BARGE-RACE-2=~a~%" *n2*))
EOF

out=$(AMILISP_RACE_SELFTEST=0 AMILISP_RACE_LOCK_MARK_DELAY_MS=300 "$TIMEOUT" 60 "$RACE" --no-userinit --non-interactive --load "$tmp" </dev/null 2>&1)
status=$?
fail() {
    echo "FAIL mt_lock_barge_race ($1)"
    echo "$out" | tail -6 | sed 's/^/    /'
    echo "0 passed, 1 failed, 1 total"
    exit 1
}
[ $status -eq 0 ] || fail "exit $status: the waiter was stranded (hang) or the run crashed"
printf '%s' "$out" | grep -q "BARGE-RACE=DONE,GOT" || fail "waiter did not acquire after the barge"
printf '%s' "$out" | grep -q "BARGE-RACE-2=2" || fail "two waiters did not both acquire after two barges"

# The default binary ignores the knob and must behave the same.
out=$(AMILISP_RACE_LOCK_MARK_DELAY_MS=300 "$TIMEOUT" 60 "$AMILISP" --no-userinit --non-interactive --load "$tmp" </dev/null 2>&1)
status=$?
[ $status -eq 0 ] || fail "default binary: exit $status"
printf '%s' "$out" | grep -q "BARGE-RACE=DONE,GOT" || fail "default binary: waiter stranded"

echo "  ok  mt_lock_barge_race (registered waiter re-marks the barging holder)"
echo "1 passed, 0 failed, 1 total"
exit 0
