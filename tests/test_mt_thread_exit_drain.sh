#!/bin/sh
# Process exit waits for workers that already left the MP registry.
#
# A worker drops out of the registry (cl_thread_count, THREAD-ALIVE-P NIL) a
# few steps before its OS thread is gone.  On AmigaOS a worker still in that
# exit tail when main returns runs on into the unloaded program code — the
# intermittent "68k exception" in CL-Thread at the end of Clamacs drive runs
# (MorphOS, FS-UAE).  cl_thread_shutdown now drains such workers first.
#
# MP::%SET-THREAD-EXIT-DELAY holds the tail open so the window is hit every
# time instead of once in a few runs:
#   - the hooks: %OS-THREAD-COUNT still counts a finished worker while its
#     tail runs, %DRAIN-OS-THREADS waits it out (and gives up with the
#     straggler count when the timeout is shorter), JOIN-THREAD still works;
#   - exit: QUIT right after the worker left the registry must not return
#     before its tail ends (wall time >= the delay), without a warning;
#   - exit with a tail longer than the 2 s bound: the process still exits,
#     and says how many workers it left behind.
#
# Run: sh tests/test_mt_thread_exit_drain.sh build/host/amilisp

CLAMIGA="${1:-build/host/amilisp}"

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)
if [ -z "$TIMEOUT" ]; then
    echo "SKIP test_mt_thread_exit_drain: neither timeout nor gtimeout on PATH"
    exit 0
fi

tmp=$(mktemp -d "${TMPDIR:-/tmp}/clamiga_exitdrain_XXXXXX") || exit 1
trap 'rm -rf "$tmp"' EXIT
fails=0

fail() { echo "FAIL $1"; fails=$((fails + 1)); }
ok()   { echo "  ok  $1"; }

# A millisecond clock: perl where there is one, date +%N otherwise (GNU and
# recent BSD; an older BSD date prints a literal N).  Without either the
# wall-time checks below would compare against 0 and fail for the wrong
# reason, so say so and skip.
if command -v perl >/dev/null 2>&1; then
    now_ms() { perl -MTime::HiRes=time -e 'printf "%d\n", time*1000'; }
elif date +%N 2>/dev/null | grep -qx '[0-9][0-9]*'; then
    now_ms() { echo $(( $(date +%s%N) / 1000000 )); }
else
    echo "SKIP test_mt_thread_exit_drain: no millisecond clock (neither perl nor GNU date)"
    exit 0
fi

# --- the hooks -----------------------------------------------------------
cat > "$tmp/hooks.lisp" <<'EOF'
(defun wait-dead (th) (loop while (mp:thread-alive-p th) do (sleep 0.01)))
(defun ms-since (t0)
  (round (* 1000 (- (get-internal-real-time) t0)) internal-time-units-per-second))
(format t "BASE ~a~%" (mp::%os-thread-count))
(mp::%set-thread-exit-delay 500)
(let ((th (mp:make-thread (lambda () 42))))
  (wait-dead th)
  (format t "TAIL ~a~%" (mp::%os-thread-count))
  (format t "SHORT ~a~%" (mp::%drain-os-threads 0 0))
  (let ((t0 (get-internal-real-time)) (left (mp::%drain-os-threads 0 3000)))
    (format t "DRAINED ~a ~a~%" left (if (>= (ms-since t0) 200) :waited :early)))
  (format t "AFTER ~a~%" (mp::%os-thread-count))
  (format t "JOIN ~a~%" (mp:join-thread th)))
(format t "PREV ~a~%" (mp::%set-thread-exit-delay 0))
;; a joined worker without the delay is gone by the time JOIN returns on
;; AmigaOS (its tail runs forbidden); on POSIX drain it for the count
(mp:join-thread (mp:make-thread (lambda () nil)))
(format t "IDLE ~a ~a~%" (mp::%drain-os-threads 0 2000) (mp::%os-thread-count))
(format t "BADKEEP ~a~%" (handler-case (mp::%drain-os-threads -1 0) (type-error () :type-error)))
(format t "BADMS ~a~%" (handler-case (mp::%set-thread-exit-delay :x) (type-error () :type-error)))
EOF
out=$($TIMEOUT 20 "$CLAMIGA" --non-interactive --no-userinit --load "$tmp/hooks.lisp" </dev/null 2>&1)
for want in "BASE 0" "TAIL 1" "SHORT 1" "DRAINED 0 WAITED" "AFTER 0" "JOIN 42" \
            "PREV 500" "IDLE 0 0" "BADKEEP TYPE-ERROR" "BADMS TYPE-ERROR"; do
    if printf '%s\n' "$out" | grep -qx "$want"; then ok "hooks: $want"
    else fail "hooks: missing '$want'"; printf '%s\n' "$out" | sed 's/^/    /'; fi
done

# --- exit waits for a worker in its exit tail ----------------------------
cat > "$tmp/exit.lisp" <<'EOF'
(mp::%set-thread-exit-delay 1200)
(let ((th (mp:make-thread (lambda () nil))))
  (loop while (mp:thread-alive-p th) do (sleep 0.01))
  (format t "LEFT-REGISTRY ~a~%" (mp::%os-thread-count))
  (finish-output)
  (quit))
EOF
t0=$(now_ms)
out=$($TIMEOUT 20 "$CLAMIGA" --non-interactive --no-userinit --load "$tmp/exit.lisp" </dev/null 2>&1)
rc=$?
el=$(( $(now_ms) - t0 ))
if [ $rc -eq 0 ]; then ok "exit: rc 0"; else fail "exit: rc $rc"; fi
if printf '%s\n' "$out" | grep -qx "LEFT-REGISTRY 1"; then ok "exit: worker was in its tail at QUIT"
else fail "exit: worker not in its tail"; printf '%s\n' "$out" | sed 's/^/    /'; fi
# the tail was entered before QUIT; allow for the time spent polling alive-p
if [ $el -ge 1000 ]; then ok "exit: waited for the tail (${el} ms)"
else fail "exit: returned after ${el} ms, before the 1200 ms tail ended"; fi
if printf '%s\n' "$out" | grep -q "did not finish"; then fail "exit: spurious straggler warning"
else ok "exit: no straggler warning"; fi

# --- a tail beyond the bound: exit anyway, and say so --------------------
cat > "$tmp/stuck.lisp" <<'EOF'
(mp::%set-thread-exit-delay 6000)
(let ((th (mp:make-thread (lambda () nil))))
  (loop while (mp:thread-alive-p th) do (sleep 0.01))
  (quit))
EOF
t0=$(now_ms)
out=$($TIMEOUT 20 "$CLAMIGA" --non-interactive --no-userinit --load "$tmp/stuck.lisp" </dev/null 2>&1)
rc=$?
el=$(( $(now_ms) - t0 ))
if [ $rc -eq 0 ]; then ok "stuck: rc 0"; else fail "stuck: rc $rc"; fi
if printf '%s\n' "$out" | grep -q "1 exiting worker thread(s) did not finish within 2 s"; then
    ok "stuck: straggler reported"
else fail "stuck: no straggler report"; printf '%s\n' "$out" | sed 's/^/    /'; fi
if [ $el -lt 5500 ]; then ok "stuck: gave up at the bound (${el} ms)"
else fail "stuck: waited ${el} ms, past the 2 s bound"; fi

# --- a second worker unregisters mid-drain: the wait target must track it,
# not the registered count frozen when the drain started ------------------
# Worker A is already unregistered and mid-tail (a 1000 ms straggler) when
# QUIT runs, so cl_thread_shutdown's drain starts with keep=1 (worker B,
# still registered). Worker B then finishes its own 700 ms body and
# unregisters WHILE the drain is still polling for A's tail. A drain that
# snapshots keep=1 once declares "drained" the instant A's tail ends
# (~1000 ms) — B has by then already dropped out of the registry but its own
# 1000 ms tail is still ~700 ms from finishing. A correct drain re-samples
# the registered count and must wait out B's tail too (~1700 ms total).
cat > "$tmp/race.lisp" <<'EOF'
(mp::%set-thread-exit-delay 1000)
(let ((a (mp:make-thread (lambda () nil))))
  (loop while (mp:thread-alive-p a) do (sleep 0.01)))
(mp:make-thread (lambda () (sleep 0.7) nil))
(sleep 0.05)
(quit)
EOF
t0=$(now_ms)
out=$($TIMEOUT 20 "$CLAMIGA" --non-interactive --no-userinit --load "$tmp/race.lisp" </dev/null 2>&1)
rc=$?
el=$(( $(now_ms) - t0 ))
if [ $rc -eq 0 ]; then ok "race: rc 0"; else fail "race: rc $rc"; fi
if [ $el -ge 1300 ]; then ok "race: waited out the second worker's tail (${el} ms)"
else fail "race: returned after ${el} ms — only the first worker's ~1000 ms tail was waited for, the second worker's own tail was not"; fi
if printf '%s\n' "$out" | grep -q "did not finish"; then fail "race: spurious straggler warning"
else ok "race: no straggler warning"; fi

if [ $fails -ne 0 ]; then
    echo "test_mt_thread_exit_drain: $fails failure(s)"
    exit 1
fi
echo "test_mt_thread_exit_drain: all passed"
