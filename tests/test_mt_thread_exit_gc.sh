#!/bin/sh
# Regression tests for two thread-lifecycle GC bugs:
#
# 1. STW hang on thread exit (thread.c cl_thread_unregister):
#    cl_gc_stop_the_world parks in platform_condvar_wait(gc_condvar)
#    waiting for a straggler thread to reach a safepoint.  A worker that
#    had already passed its LAST safepoint and was exiting never stopped
#    — it unregistered from cl_thread_list without waking gc_condvar, so
#    the initiator (whose re-scan would have found the list shrunk) slept
#    forever: GC, and with it the whole process, hung.  The scenario
#    below hammers the exact window: many short-lived threads exiting
#    while the main thread forces GCs in a tight loop.  A hang shows up
#    as the watchdog timeout killing the run (non-zero exit).
#
#    That scenario exercises the code path under load but cannot be
#    relied on to actually land in the narrow race window on a given
#    host/scheduler — see Case 1b below for a deterministic trigger.
#
# 1b. Deterministic trigger for the same bug: when a second argument is
#    given, it must be a amilisp built with -DDEBUG_THREAD_RACE_HOOKS (see
#    `make test-mt-thread-exit-race`).  That build's constructor
#    (src/core/thread.c) spawns one worker that never reaches a safepoint
#    and forces it to unregister at the exact instant the STW initiator
#    parks in platform_condvar_wait, closing the same window Case 1 can
#    only hope to hit.  If cl_thread_unregister's wakeup regresses, this
#    hangs (caught by `timeout` below) instead of racily passing.
#
# 2. First-worker double-registered GC roots (builtins_thread.c):
#    two racing first-worker creations could both see the abort
#    handler/report caches as NIL and both cl_gc_register_root the same
#    address.  gc_forward is not idempotent, so a double-registered root
#    is forwarded twice on compaction and lands on an unrelated object.
#    Roots are now registered once at boot; (ext:%gc-audit-roots) must
#    report 0 violations after threads were created from two racing
#    parents.
#
# Run: sh tests/test_mt_thread_exit_gc.sh build/host/amilisp [race-hook-clamiga]

AMILISP="${1:-build/host/amilisp}"
RACE_CLAMIGA="$2"

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)
if [ -z "$TIMEOUT" ]; then
    echo "SKIP test_mt_thread_exit_gc: neither timeout nor gtimeout on PATH"
    exit 0
fi

if [ ! -x "$AMILISP" ]; then
    echo "SKIP test_mt_thread_exit_gc: no binary at $AMILISP"
    exit 0
fi

WORK="${TMPDIR:-/tmp}/clamiga_mt_exit_gc_$$"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

cat > "$WORK/exit-gc.lisp" <<'EOF'
;; Case 1: threads exiting continuously while main forces GCs.
;; Short-lived workers maximize the "exits after its last safepoint
;; while an STW is being requested" window.
(dotimes (round 30)
  (let ((threads '()))
    (dotimes (i 8)
      (push (mp:make-thread (lambda () (length (make-list 50))))
            threads))
    ;; Force GCs while the workers are running/exiting.
    (dotimes (g 6) (ext:gc))
    (dolist (thr threads) (mp:join-thread thr))))
(format t "EXIT-GC-OK~%")

;; Case 2: two racing parents each creating first workers, then audit
;; the root registries.  (The abort handler/report caches are built on
;; the make-thread path.)
(let ((parents '()))
  (dotimes (i 2)
    (push (mp:make-thread
           (lambda ()
             (let ((kids '()))
               (dotimes (k 4)
                 (push (mp:make-thread (lambda () (ext:gc) 42)) kids))
               (dolist (kid kids) (mp:join-thread kid)))))
          parents))
  (dolist (p parents) (mp:join-thread p)))
(ext:gc-compact)
(format t "ROOT-AUDIT:~d~%" (ext:%gc-audit-roots))
EOF

out=$("$TIMEOUT" 90 "$AMILISP" --no-userinit --heap 32M --non-interactive \
      --load "$WORK/exit-gc.lisp" 2>&1)
status=$?

failed=0

if [ $status -ne 0 ]; then
    echo "FAIL test_mt_thread_exit_gc: run timed out or crashed (exit $status)"
    echo "$out" | tail -6 | sed 's/^/    /'
    failed=1
fi

if echo "$out" | grep -q "EXIT-GC-OK"; then
    echo "  ok  GC while threads exit does not hang"
else
    echo "FAIL test_mt_thread_exit_gc: no EXIT-GC-OK marker (STW hang?)"
    failed=1
fi

if echo "$out" | grep -q "ROOT-AUDIT:0"; then
    echo "  ok  no double-registered GC roots after MT thread creation"
else
    echo "FAIL test_mt_thread_exit_gc: root audit reported violations"
    echo "$out" | grep "ROOT-AUDIT" | sed 's/^/    /'
    failed=1
fi

if [ -n "$RACE_CLAMIGA" ]; then
    if [ ! -x "$RACE_CLAMIGA" ]; then
        echo "FAIL test_mt_thread_exit_gc: no race-hook binary at $RACE_CLAMIGA"
        failed=1
    else
        race_out=$("$TIMEOUT" 30 "$RACE_CLAMIGA" 2>&1)
        race_status=$?
        if [ $race_status -eq 0 ] && echo "$race_out" | grep -q "RACE-SELFTEST-OK"; then
            echo "  ok  deterministic STW-hang-on-thread-exit trigger does not hang"
        else
            echo "FAIL test_mt_thread_exit_gc: deterministic race trigger hung or failed (exit $race_status)"
            echo "$race_out" | tail -6 | sed 's/^/    /'
            failed=1
        fi
    fi
else
    echo "  skip  deterministic race trigger (no race-hook binary given; see \`make test-mt-thread-exit-race\`)"
fi

if [ $failed -eq 0 ]; then
    echo "PASS test_mt_thread_exit_gc"
fi
exit $failed
