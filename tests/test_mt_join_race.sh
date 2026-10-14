#!/bin/sh
# Regression test: concurrent JOIN-THREAD of the same worker must not
# double-join / double-free, and ALL-THREADS / THREAD-ALIVE-P must not
# dereference a worker freed concurrently by the zombie reaper.
#
# Pre-fix bi_join_thread had no mutual exclusion: two joiners both read
# the same CL_Thread from cl_thread_table, both called
# platform_thread_join on the same handle (double pthread_join — UB and
# a double free of the AmigaThread/pthread wrapper), and both called
# cl_thread_free_worker(t) — the "pointer being freed was not
# allocated" crash.  The zombie reaper in MAKE-THREAD could also free a
# finished worker while a joiner was still parked inside
# platform_thread_join on its handle.
#
# The scenario spawns a short-lived worker plus TWO joiners racing to
# join it, while a churn loop creates/finishes workers to keep the
# reaper active and a poller hammers ALL-THREADS / THREAD-ALIVE-P.
# Reproduction is probabilistic per run, so the binary runs several
# times; any crash / hang / double-free abort in ANY run fails.
#
# Run: sh tests/test_mt_join_race.sh build/host/amilisp

AMILISP="${1:-build/host/amilisp}"

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)
if [ -z "$TIMEOUT" ]; then
    echo "SKIP test_mt_join_race: neither timeout nor gtimeout on PATH"
    exit 0
fi

tmp=$(mktemp "${TMPDIR:-/tmp}/clamiga_mtjoin_XXXXXX") || exit 1
trap 'rm -f "$tmp"' EXIT

cat > "$tmp" <<'EOF'
(defvar *stop* nil)

;; Poller: hammer the thread table readers while workers churn.
(defvar *poller*
  (mp:make-thread
   (lambda ()
     (loop until *stop*
           do (progn (mp:all-threads)
                     nil)))
   :name "poller"))

(dotimes (round 30)
  (let* ((worker (mp:make-thread (lambda () (* round 2)) :name "victim"))
         (r1 nil) (r2 nil)
         (j1 (mp:make-thread (lambda ()
                               (handler-case (setf r1 (mp:join-thread worker))
                                 (error () (setf r1 :err))))
                             :name "joiner-1"))
         (j2 (mp:make-thread (lambda ()
                               (handler-case (setf r2 (mp:join-thread worker))
                                 (error () (setf r2 :err))))
                             :name "joiner-2")))
    ;; Churn: give the reaper zombies to sweep during the joins.
    (dotimes (i 4)
      (mp:join-thread (mp:make-thread (lambda () i))))
    (mp:join-thread j1)
    (mp:join-thread j2)
    ;; At least one joiner must observe the worker's real result; the
    ;; other either the same result or a clean error — never a crash.
    (unless (or (eql r1 (* round 2)) (eql r2 (* round 2)))
      (format t "MT-JOIN-BAD-RESULT: ~s ~s round ~d~%" r1 r2 round))))

(setf *stop* t)
(mp:join-thread *poller*)
(format t "MT-JOIN-DONE~%")
EOF

runs=6
i=1
while [ $i -le $runs ]; do
    out=$("$TIMEOUT" 60 "$AMILISP" --no-userinit --non-interactive --load "$tmp" </dev/null 2>&1)
    status=$?
    if [ $status -ne 0 ]; then
        echo "FAIL mt_join_race (run $i/$runs — exit $status: crash/hang)"
        echo "$out" | tail -5 | sed 's/^/    /'
        echo "0 passed, 1 failed, 1 total"
        exit 1
    fi
    if ! printf '%s' "$out" | grep -q "MT-JOIN-DONE"; then
        echo "FAIL mt_join_race (run $i/$runs — no completion marker)"
        echo "$out" | tail -5 | sed 's/^/    /'
        echo "0 passed, 1 failed, 1 total"
        exit 1
    fi
    if printf '%s' "$out" | grep -q "MT-JOIN-BAD-RESULT\|not allocated\|corrupted\|Guru"; then
        echo "FAIL mt_join_race (run $i/$runs — corruption)"
        echo "$out" | grep "MT-JOIN-BAD-RESULT\|not allocated\|corrupted\|Guru" | head -3 | sed 's/^/    /'
        echo "0 passed, 1 failed, 1 total"
        exit 1
    fi
    i=$((i + 1))
done

echo "  ok  mt_join_race ($runs runs clean)"
echo "1 passed, 0 failed, 1 total"
exit 0
