#!/bin/sh
# MP locks and condition variables as heap words (specs/mp-locks-heap-words.md).
#
# One regression test per clause of the design:
#   - cap gone: 100,000 live locks at once (the sento shared/ask wall was
#     16,384), then released and collected;
#   - misuse signals instead of hanging or corrupting: release by a
#     non-owner, release of a free lock, re-acquire of a plain lock by its
#     owner, condition-wait without the lock, bad timeouts;
#   - a recursive lock held at depth 2 across a condition-wait comes back at
#     depth 2 (two releases needed);
#   - one wakeup per release: N parked waiters, N releases, N acquisitions;
#   - acquire-lock's timeout argument (NIL forever, 0 one attempt, wait-p NIL
#     ignores it, NIL on timeout, T when the holder releases in time);
#   - timeout racing notify: a 1 ms timed wait against a notifier firing at
#     the same instant, many rounds — the waiter count must return to zero
#     every time (a notify that lands at the timeout instant is exactly one
#     of "notified" / "timed out");
#   - lost-wakeup soak: 8 threads ping-pong through a lock and a condvar with
#     randomised notify timing; a hang is the failure (the timeout below).
#
# Run: sh tests/test_mp_heap_locks.sh build/host/amilisp

AMILISP="${1:-build/host/amilisp}"

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)
if [ -z "$TIMEOUT" ]; then
    echo "SKIP test_mp_heap_locks: neither timeout nor gtimeout on PATH"
    exit 0
fi

tmp=$(mktemp "${TMPDIR:-/tmp}/clamiga_mpheap_XXXXXX") || exit 1
trap 'rm -f "$tmp"' EXIT

cat > "$tmp" <<'EOF'
(defvar *fails* 0)
(defmacro chk (name expected form)
  `(let ((got (handler-case ,form (error (e) (list :error (princ-to-string e))))))
     (if (equal got ,expected)
         (format t "  ok  ~a~%" ,name)
         (progn (format t "FAIL ~a: expected ~s got ~s~%" ,name ,expected got)
                (incf *fails*)))))
(defmacro chk-error (name form)
  `(let ((got (handler-case (progn ,form :no-error) (error () :error))))
     (if (eq got :error)
         (format t "  ok  ~a~%" ,name)
         (progn (format t "FAIL ~a: expected an error, got ~s~%" ,name got)
                (incf *fails*)))))

;; --- cap gone ---------------------------------------------------------
(chk "100k live locks and condvars at once" 200000
  (let ((v (make-array 200000)))
    (dotimes (i 100000)
      (setf (aref v i) (mp:make-lock))
      (setf (aref v (+ i 100000)) (mp:make-condition-variable)))
    ;; every one is usable
    (let ((n 0))
      (dotimes (i 100000)
        (mp:acquire-lock (aref v i))
        (mp:release-lock (aref v i))
        (incf n))
      (dotimes (i 100000)
        (mp:condition-notify (aref v (+ i 100000)))
        (incf n))
      (setf v nil)
      (gc)
      n)))

;; --- misuse errors ----------------------------------------------------
(chk "non-owner release leaves the lock held and errors in the worker" '(:err nil t)
  (let ((lk (mp:make-lock)))
    (mp:acquire-lock lk)
    (let ((r1 (mp:join-thread (mp:make-thread
                               (lambda () (handler-case (progn (mp:release-lock lk) :released)
                                            (error () :err))))))
          (r2 (mp:join-thread (mp:make-thread (lambda () (mp:acquire-lock lk nil))))))
      (mp:release-lock lk)
      (list r1 r2 (mp:join-thread (mp:make-thread (lambda () (mp:acquire-lock lk nil))))))))
(chk-error "release of a free lock signals"
  (mp:release-lock (mp:make-lock)))
(chk-error "re-acquire of a plain lock by its owner signals"
  (let ((lk (mp:make-lock)))
    (mp:acquire-lock lk)
    (mp:acquire-lock lk)))
(chk "re-acquire error leaves the plain lock held once" t
  (let ((lk (mp:make-lock)))
    (mp:acquire-lock lk)
    (handler-case (mp:acquire-lock lk) (error () nil))
    (mp:release-lock lk)
    (mp:acquire-lock lk nil)))
(chk-error "condition-wait without holding the lock signals"
  (mp:condition-wait (mp:make-condition-variable) (mp:make-lock)))
(chk-error "negative acquire timeout signals"
  (mp:acquire-lock (mp:make-lock) t -1))
(chk-error "non-numeric condition-wait timeout signals"
  (let ((lk (mp:make-lock)))
    (mp:acquire-lock lk)
    (mp:condition-wait (mp:make-condition-variable) lk "soon")))

;; --- recursive depth across a wait -------------------------------------
(chk "recursive lock held at depth 2 across a wait comes back at depth 2" '(nil t)
  (let* ((lk (mp:make-recursive-lock))
         (cv (mp:make-condition-variable))
         (go nil)
         (after-one nil)
         (after-two nil)
         (th (mp:make-thread
              (lambda ()
                (mp:acquire-lock lk)
                (mp:acquire-lock lk)          ; depth 2
                (loop until go do (mp:condition-wait cv lk))
                (mp:release-lock lk)          ; depth 1: still held
                (sleep 0.2)
                (mp:release-lock lk)))))      ; free
    (sleep 0.2)
    (mp:acquire-lock lk)
    (setf go t)
    (mp:condition-notify cv)
    (mp:release-lock lk)
    (sleep 0.1)
    (setf after-one (mp:acquire-lock lk nil))   ; must still be held by the worker
    (mp:join-thread th)
    (setf after-two (mp:acquire-lock lk nil))
    (when after-two (mp:release-lock lk))
    (list after-one after-two)))

;; --- one wakeup per release --------------------------------------------
(chk "N parked waiters, N releases, N acquisitions" '(6 6)
  (let* ((lk (mp:make-lock))
         (n 6)
         (acquired 0)
         (wakeups 0)
         (gate (mp:make-lock))
         (ths nil))
    (mp:acquire-lock lk)
    (dotimes (i n)
      (push (mp:make-thread
             (lambda ()
               (mp:acquire-lock lk)
               (mp:with-lock-held (gate) (incf acquired) (incf wakeups))
               (mp:release-lock lk)))
            ths))
    (sleep 0.3)                                  ; all six parked
    (mp:release-lock lk)                         ; wakes exactly one; each
    (dolist (th ths) (mp:join-thread th))        ; release hands on
    (list acquired wakeups)))

;; --- acquire-lock timeout ----------------------------------------------
(chk "timeout NIL waits, 0 tries once, wait-p NIL ignores timeout" '(nil nil t)
  (let ((lk (mp:make-lock)))
    (mp:acquire-lock lk)
    (let ((r0 (mp:join-thread (mp:make-thread (lambda () (mp:acquire-lock lk t 0)))))
          (r1 (mp:join-thread (mp:make-thread (lambda () (mp:acquire-lock lk nil 5))))))
      (mp:release-lock lk)
      (list r0 r1 (mp:acquire-lock lk t nil)))))
(chk "timed acquire returns NIL after the timeout, lock still held" '(nil t nil)
  (let ((lk (mp:make-lock)) (t0 0) (dt 0))
    (mp:acquire-lock lk)
    (let ((r (mp:join-thread
              (mp:make-thread
               (lambda ()
                 (setf t0 (get-internal-real-time))
                 (prog1 (mp:acquire-lock lk t 0.2)
                   (setf dt (/ (- (get-internal-real-time) t0)
                               internal-time-units-per-second))))))))
      (list r (>= dt 0.15) (mp:join-thread (mp:make-thread (lambda () (mp:acquire-lock lk nil))))))))
(chk "timed acquire succeeds when the holder releases in time" t
  (let ((lk (mp:make-lock)))
    (mp:acquire-lock lk)
    (let ((th (mp:make-thread (lambda () (prog1 (mp:acquire-lock lk t 5) (mp:release-lock lk))))))
      (sleep 0.1)
      (mp:release-lock lk)
      (mp:join-thread th))))
(chk "timed acquire of a recursive lock by its owner nests" 2
  (let ((lk (mp:make-recursive-lock)) (n 0))
    (when (mp:acquire-lock lk t 1) (incf n))
    (when (mp:acquire-lock lk t 1) (incf n))
    (mp:release-lock lk) (mp:release-lock lk)
    n))
(chk "huge timeout (34.7 days, past the old int32-deadline wrap point) still blocks until release" t
  ;; parse_timeout_ms used to clamp timeouts up to ~49.7 days, but
  ;; ms_until()'s deadline arithmetic only has signed-32-bit range
  ;; (~24.85 days); a longer deadline wrapped and read as already
  ;; expired on the first check, so the acquire below would return NIL
  ;; almost instantly instead of waiting for the release.
  (let ((lk (mp:make-lock)))
    (mp:acquire-lock lk)
    (let ((th (mp:make-thread (lambda () (mp:acquire-lock lk t 3000000)))))
      (sleep 0.2)
      (mp:release-lock lk)
      (mp:join-thread th))))

;; --- timeout racing notify ---------------------------------------------
(chk "1ms timed wait vs notify at the same instant: waiters back to zero every round" '(t t)
  (let* ((lk (mp:make-lock))
         (cv (mp:make-condition-variable))
         (rounds 3000)
         (ts 0) (nils 0)
         (bad 0)
         (stop nil)
         (notifier (mp:make-thread
                    (lambda ()
                      (loop until stop
                            do (mp:with-lock-held (lk) (mp:condition-notify cv)))))))
    (dotimes (i rounds)
      (mp:acquire-lock lk)
      (if (mp:condition-wait cv lk 0.001) (incf ts) (incf nils))
      (mp:release-lock lk)
      (unless (zerop (mp::%condvar-waiters cv)) (incf bad)))
    (setf stop t)
    (mp:join-thread notifier)
    (list (= (+ ts nils) rounds) (zerop bad))))

;; --- barging soak: spinners racing parked waiters ------------------------
;; The lost wakeup found by the sento shared/ask-s cell: a waiter marks the
;; holder's word CONTENDED, the holder releases and scans before the waiter
;; is registered, a SPINNING newcomer takes the free lock with a plain word,
;; the waiter (now registered) sees "held" and parks — and the newcomer's
;; release, seeing no CONTENDED bit, never scans.  Eight threads on one lock
;; with an empty critical section and occasional parks reproduce the three-
;; way interleaving within seconds; the fix marks the CURRENT holder after
;; registering.  A hang is the failure.
;; The same soak also caught a mutual-exclusion hole: the contended release
;; stored 0 without a release fence, so on ARM64 the store could become
;; visible before the critical section's own writes and the next owner
;; read stale data (one lost INCF in 320,000).  Besides the counter, each
;; thread stamps a shared cell on entry and checks the stamp before it
;; leaves — a second thread inside the section shows up as a foreign stamp.
(chk "8-thread empty-critical-section barging soak" '(t 0)
  (let* ((lk (mp:make-lock))
         (n 8) (rounds 40000)
         (total 0)
         (owner nil)
         (violations 0)
         (ths nil))
    (dotimes (i n)
      (let ((me i))
        (push (mp:make-thread
               (lambda ()
                 (dotimes (r rounds)
                   (mp:acquire-lock lk)
                   (setf owner me)
                   (incf total)
                   (when (zerop (logand r 1023)) (mp:thread-yield))
                   (unless (eql owner me) (incf violations))
                   (mp:release-lock lk))))
              ths)))
    (dolist (th ths) (mp:join-thread th))
    (list (= total (* n rounds)) violations)))

;; --- lost-wakeup soak --------------------------------------------------
(chk "8-thread lock+condvar ping-pong soak" t
  (let* ((lk (mp:make-lock))
         (cv (mp:make-condition-variable))
         (nthreads 8)
         (rounds 400)
         (turn 0)
         (total 0)
         (ths nil))
    (dotimes (i nthreads)
      (let ((me i))
        (push (mp:make-thread
               (lambda ()
                 (dotimes (r rounds)
                   (mp:acquire-lock lk)
                   (loop until (= turn me) do (mp:condition-wait cv lk))
                   (incf total)
                   (setf turn (mod (1+ turn) nthreads))
                   (when (zerop (random 3)) (mp:thread-yield))
                   (if (zerop (random 2))
                       (mp:condition-broadcast cv)
                       (progn (mp:condition-notify cv) (mp:condition-broadcast cv)))
                   (mp:release-lock lk))))
              ths)))
    (dolist (th ths) (mp:join-thread th))
    (and (= total (* nthreads rounds)) (zerop (mp::%condvar-waiters cv)))))

(format t "MP-HEAP-LOCKS-DONE fails=~a~%" *fails*)
(quit :code (if (zerop *fails*) 0 1))
EOF

out=$("$TIMEOUT" 240 "$AMILISP" --no-userinit --non-interactive --load "$tmp" </dev/null 2>&1)
status=$?
echo "$out" | grep -E "^  ok  |^FAIL "
if [ $status -ne 0 ] || ! echo "$out" | grep -q "MP-HEAP-LOCKS-DONE fails=0"; then
    echo "FAIL test_mp_heap_locks (status $status)"
    echo "$out" | grep -v "^  ok  " | tail -15
    echo "0 passed, 1 failed, 1 total"
    exit 1
fi
echo "1 passed, 0 failed, 1 total"
exit 0
