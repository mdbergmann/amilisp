#!/bin/sh
# Regression test (GC audit tier 4, batch 6, I5/T5): interrupt/destroy
# delivery to PARKED threads, and process shutdown with live workers.
#
# Pre-fix, interrupts were delivered only at safepoints.  A target parked
# in MP:CONDITION-WAIT (never notified) or a blocking MP:ACQUIRE-LOCK
# never runs another safepoint, so MP:DESTROY-THREAD of such a thread was
# deferred FOREVER — the destroyer's JOIN-THREAD hung.  The fix:
#   1. condition-wait re-checks interrupt_pending after publishing its
#      wait registration and skips the park (spurious wakeup) if set;
#   2. the interrupt/destroy publisher acquires the target's wait mutex
#      and broadcasts its condvar, so the target either saw the flag
#      pre-park or is parked and receives the broadcast;
#   3. blocking acquire-lock is a trylock loop (yield-spin phase, then
#      parked on the shared lock-parking condvar) that checks
#      interrupt_pending each round; the publisher broadcasts the parking
#      cv and a timed backstop bounds delivery.
# T5: exiting the process while spinner workers are still running must
# not crash in teardown (gc_mutex/list-lock destroyed under live threads
# pre-fix; now deliberately leaked when workers remain).
#
# Run: sh tests/test_mt_interrupt_parked.sh build/host/amilisp

CLAMIGA="${1:-build/host/amilisp}"

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)
if [ -z "$TIMEOUT" ]; then
    echo "SKIP test_mt_interrupt_parked: neither timeout nor gtimeout on PATH"
    exit 0
fi

tmp=$(mktemp "${TMPDIR:-/tmp}/clamiga_mtparked_XXXXXX") || exit 1
trap 'rm -f "$tmp"' EXIT

cat > "$tmp" <<'EOF'
;; I5a: destroy a thread parked in condition-wait on a never-notified cv.
(let* ((lk (mp:make-lock))
       (cv (mp:make-condition-variable))
       (th (mp:make-thread
            (lambda ()
              (mp:acquire-lock lk)
              ;; Loop: a spurious wakeup (permitted) must re-park, so the
              ;; only way out is the destroy.
              (loop (mp:condition-wait cv lk))))))
  (sleep 0.4)
  (mp:destroy-thread th)
  (handler-case (mp:join-thread th) (error () nil))
  (format t "I5-DESTROY-CONDWAIT-OK~%"))

;; I5b: destroy a thread parked in a blocking acquire-lock.
(let ((lk (mp:make-lock)))
  (mp:acquire-lock lk)
  (let ((th (mp:make-thread (lambda () (mp:acquire-lock lk) :got))))
    (sleep 0.4)
    (mp:destroy-thread th)
    (handler-case (mp:join-thread th) (error () nil))
    (mp:release-lock lk))
  (format t "I5-DESTROY-LOCKWAIT-OK~%"))

;; I5c: interrupt (not destroy) a thread parked in condition-wait: the
;; interrupt function must run and the thread must be able to finish.
(let* ((lk (mp:make-lock))
       (cv (mp:make-condition-variable))
       (hit nil)
       (done nil)
       (th (mp:make-thread
            (lambda ()
              (mp:acquire-lock lk)
              (loop until done do (mp:condition-wait cv lk))
              (mp:release-lock lk)
              :finished)))
       (r nil))
  (sleep 0.4)
  (mp:interrupt-thread th (lambda () (setf hit t done t)))
  ;; The wakeup lets the waiter reach a safepoint; the interrupt sets
  ;; done, the loop re-tests and exits.  Nudge with a broadcast in case
  ;; the interrupt ran between re-test and re-park.
  (dotimes (i 20)
    (unless done (sleep 0.05)))
  (mp:condition-broadcast cv)
  (setf r (mp:join-thread th))
  (format t "I5-INTERRUPT-CONDWAIT=~a,~a~%" (if hit "HIT" "MISS") r))

;; I5d: destroy a thread parked in a TIMED condition-wait (long timeout)
;; and one parked in a TIMED acquire-lock — delivery must not wait for
;; the timeout (heap-word locks: the publisher unparks the target directly).
(let* ((lk (mp:make-lock))
       (cv (mp:make-condition-variable))
       (t0 (get-internal-real-time))
       (th (mp:make-thread
            (lambda ()
              (mp:acquire-lock lk)
              (loop (mp:condition-wait cv lk 30))))))
  (sleep 0.4)
  (mp:destroy-thread th)
  (handler-case (mp:join-thread th) (error () nil))
  (format t "I5-DESTROY-TIMED-CONDWAIT=~a~%"
          (if (< (/ (- (get-internal-real-time) t0) internal-time-units-per-second) 5)
              "FAST" "SLOW")))
(let ((lk (mp:make-lock)) (t0 (get-internal-real-time)))
  (mp:acquire-lock lk)
  (let ((th (mp:make-thread (lambda () (mp:acquire-lock lk t 30) :got))))
    (sleep 0.4)
    (mp:destroy-thread th)
    (handler-case (mp:join-thread th) (error () nil))
    (mp:release-lock lk))
  (format t "I5-DESTROY-TIMED-LOCKWAIT=~a~%"
          (if (< (/ (- (get-internal-real-time) t0) internal-time-units-per-second) 5)
              "FAST" "SLOW")))

;; I5e: INTERRUPT (not destroy) a thread parked in a blocking acquire-lock:
;; the function runs while it waits, and the thread still gets the lock
;; once it is released.
(let* ((lk (mp:make-lock))
       (hit nil)
       (th nil))
  (mp:acquire-lock lk)
  (setf th (mp:make-thread (lambda () (mp:acquire-lock lk) (mp:release-lock lk) :finished)))
  (sleep 0.4)
  (mp:interrupt-thread th (lambda () (setf hit t)))
  (dotimes (i 40) (unless hit (sleep 0.05)))
  (mp:release-lock lk)
  (format t "I5-INTERRUPT-LOCKWAIT=~a,~a~%" (if hit "HIT" "MISS") (mp:join-thread th)))

;; I5f: a thread destroyed while parked on a lock must not strand the
;; OTHER waiters on that lock (the abandoned wait hands the lock on).
(let* ((lk (mp:make-lock))
       (got 0)
       (gate (mp:make-lock))
       (victim nil)
       (others nil))
  (mp:acquire-lock lk)
  (setf victim (mp:make-thread (lambda () (mp:acquire-lock lk) (mp:release-lock lk))))
  (sleep 0.2)
  (dotimes (i 3)
    (push (mp:make-thread (lambda () (mp:acquire-lock lk)
                            (mp:with-lock-held (gate) (incf got))
                            (mp:release-lock lk)))
          others))
  (sleep 0.3)
  (mp:destroy-thread victim)
  (handler-case (mp:join-thread victim) (error () nil))
  (mp:release-lock lk)
  (dolist (th others) (mp:join-thread th))
  (format t "I5-DESTROY-DOES-NOT-STRAND=~a~%" got))

;; I8: dropping the last reference to a HELD lock is plain garbage
;; collection now (no OS mutex to leak) — must not crash.
(let ((l (mp:make-lock)))
  (mp:acquire-lock l))
(dotimes (i 3) (gc))
(format t "I8-HELD-FINALIZE-OK~%")

;; T5: leave spinner workers running and exit the process — teardown
;; must not destroy the GC coordination primitives under them.
(defvar *spin-stop* nil)
(dotimes (i 3)
  (mp:make-thread (lambda () (loop until *spin-stop* do (mp:thread-yield)))))
(format t "T5-EXIT-WITH-WORKERS~%")
EOF

runs=3
i=1
while [ $i -le $runs ]; do
    out=$("$TIMEOUT" 60 "$CLAMIGA" --no-userinit --non-interactive --load "$tmp" </dev/null 2>&1)
    status=$?
    fail() {
        echo "FAIL mt_interrupt_parked (run $i/$runs — $1)"
        echo "$out" | tail -8 | sed 's/^/    /'
        echo "0 passed, 1 failed, 1 total"
        exit 1
    }
    [ $status -eq 0 ] || fail "exit $status: hang or crash"
    printf '%s' "$out" | grep -q "I5-DESTROY-CONDWAIT-OK" || fail "destroy of condwait-parked thread hung"
    printf '%s' "$out" | grep -q "I5-DESTROY-LOCKWAIT-OK" || fail "destroy of lock-parked thread hung"
    printf '%s' "$out" | grep -q "I5-INTERRUPT-CONDWAIT=HIT,FINISHED" || fail "interrupt of parked thread not delivered"
    printf '%s' "$out" | grep -q "I5-DESTROY-TIMED-CONDWAIT=FAST" || fail "destroy of timed-condwait-parked thread waited for the timeout"
    printf '%s' "$out" | grep -q "I5-DESTROY-TIMED-LOCKWAIT=FAST" || fail "destroy of timed-lock-parked thread waited for the timeout"
    printf '%s' "$out" | grep -q "I5-INTERRUPT-LOCKWAIT=HIT,FINISHED" || fail "interrupt of lock-parked thread not delivered or lock lost"
    printf '%s' "$out" | grep -q "I5-DESTROY-DOES-NOT-STRAND=3" || fail "destroying a parked waiter stranded the other waiters"
    printf '%s' "$out" | grep -q "I8-HELD-FINALIZE-OK" || fail "held-lock finalize crashed"
    printf '%s' "$out" | grep -q "T5-EXIT-WITH-WORKERS" || fail "no exit marker"
    i=$((i + 1))
done

echo "  ok  mt_interrupt_parked ($runs runs clean)"
echo "1 passed, 0 failed, 1 total"
exit 0
