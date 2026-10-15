#!/bin/sh
# Regression test (GC audit tier 4, batch 6, T1): a thread wrapper whose
# table slot has been reaped and REUSED by an unrelated worker must not
# act on the new occupant.
#
# Pre-fix, cl_thread_table slots were keyed by index only.  When the
# table filled up, MAKE-THREAD's zombie reaper freed finished-but-
# unjoined workers and later MAKE-THREADs reused their slots.  A stale
# wrapper (thread_id = old index) then aliased the NEW occupant:
#   - THREAD-ALIVE-P reported the innocent occupant's liveness (T),
#   - INTERRUPT-THREAD / DESTROY-THREAD hit the innocent thread,
#   - JOIN-THREAD joined AND FREED the innocent worker (double free when
#     its own joiner finished).
# The fix adds a per-slot generation counter (cl_thread_table_gen);
# wrappers snapshot it at creation and every table lookup verifies it.
#
# The scenario is deterministic: CL_MAX_THREADS = 256.  A victim thread
# (lowest slot) finishes unjoined; 250 finished fillers (wrappers held so
# GC cannot release their slots) leave 4 free slots; 8 live spinners then
# force the table-full reaper, and spinner #5 reclaims the victim's slot.
#
# Run: sh tests/test_mt_thread_identity.sh build/host/amilisp

AMILISP="${1:-build/host/amilisp}"

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)
if [ -z "$TIMEOUT" ]; then
    echo "SKIP test_mt_thread_identity: neither timeout nor gtimeout on PATH"
    exit 0
fi

tmp=$(mktemp "${TMPDIR:-/tmp}/amilisp_mtident_XXXXXX") || exit 1
trap 'rm -f "$tmp"' EXIT

cat > "$tmp" <<'EOF'
;; Victim: finishes immediately, never joined; wrapper held in *old*.
(defvar *old* (mp:make-thread (lambda () 42) :name "victim"))
(sleep 0.3)

;; Fill the table with finished, unjoined workers whose wrappers we hold
;; (so GC finalize cannot release their slots).  250 fillers + main +
;; victim leave 4 free slots of the 256.
(defvar *fillers* nil)
(dotimes (i 250)
  (push (mp:make-thread (lambda () nil)) *fillers*)
  (when (zerop (mod i 50)) (sleep 0.05)))
(sleep 0.3)

;; 8 live spinners: the first 4 take the free tail slots, the 5th forces
;; the table-full zombie reaper (which frees the victim's slot among the
;; finished ones) and reclaims the LOWEST freed index — the victim's.
(defvar *stop* nil)
(defvar *live* nil)
(dotimes (i 8)
  (push (mp:make-thread (lambda () (loop until *stop* do (mp:thread-yield))))
        *live*))
(sleep 0.2)

;; The victim's slot now holds a LIVE unrelated worker.  All three
;; operations on the stale wrapper must refuse.
(format t "T1-ALIVE=~a~%" (mp:thread-alive-p *old*))
(format t "T1-INT=~a~%"
        (handler-case (progn (mp:interrupt-thread *old* (lambda () nil))
                             :sent)
          (error () :err)))
(setf *stop* t)
(dolist (th *live*) (mp:join-thread th))
(format t "T1-JOIN=~a~%"
        (handler-case (mp:join-thread *old*) (error () :err)))
(format t "MT-IDENTITY-DONE~%")
EOF

out=$("$TIMEOUT" 120 "$AMILISP" --no-userinit --non-interactive --load "$tmp" </dev/null 2>&1)
status=$?
fail() {
    echo "FAIL mt_thread_identity ($1)"
    echo "$out" | tail -8 | sed 's/^/    /'
    echo "0 passed, 1 failed, 1 total"
    exit 1
}

[ $status -eq 0 ] || fail "exit $status: crash/hang"
printf '%s' "$out" | grep -q "MT-IDENTITY-DONE" || fail "no completion marker"
# Pre-fix: T1-ALIVE=T (the live occupant of the reused slot) and
# T1-INT=SENT (interrupt delivered to the innocent worker).
printf '%s' "$out" | grep -q "T1-ALIVE=NIL" || fail "stale wrapper reported occupant as alive"
printf '%s' "$out" | grep -q "T1-INT=ERR" || fail "interrupt through stale wrapper was delivered"
printf '%s' "$out" | grep -q "T1-JOIN=ERR" || fail "join through stale wrapper did not error"

echo "  ok  mt_thread_identity (stale wrapper refused after slot reuse)"
echo "1 passed, 0 failed, 1 total"
exit 0
