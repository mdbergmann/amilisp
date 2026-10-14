#!/bin/sh
# Regression test: the outbuf-table mutex (cl_stream_table_mutex) must not be
# leaked locked when the thread count changes mid-critical-section.
#
# The bug: cl_stream_alloc_outbuf / cl_stream_free_outbuf / cl_make_cbuf_input_stream
# (and the FASL active-object registries) gated BOTH the mutex lock and its
# matching unlock on `CL_MT()` == `cl_thread_count > 1`, evaluated live at each
# site.  A thread that locked the table while a peer worker was alive (CL_MT
# true) and then reached its unlock AFTER that worker exited/joined — dropping
# the count to 1 — saw CL_MT() == false and SKIPPED the unlock.  The mutex was
# then held forever with no owner, so the very next FORMAT / string-output-stream
# creation (which also FORMATs) deadlocked both threads in platform_mutex_lock.
#
# The fix captures the multithread decision ONCE per function (`int mt = CL_MT()`,
# mirroring mem.c's `int multi` idiom) so lock and unlock always agree.
#
# Repro: the main thread FORMATs in a tight loop (each FORMAT locks the outbuf
# table) while short-lived workers repeatedly flip the thread count 2<->1.  This
# lands the count-drop inside a locked critical section almost every run, so the
# buggy build hangs deterministically; the fixed build always prints the marker.
#
# Run: sh tests/test_mt_stream_mutex_leak.sh build/host/amilisp

CLAMIGA="${1:-build/host/amilisp}"

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)
if [ -z "$TIMEOUT" ]; then
    echo "SKIP test_mt_stream_mutex_leak: neither timeout nor gtimeout on PATH"
    exit 0
fi

tmp=$(mktemp "${TMPDIR:-/tmp}/clamiga_mtstreamlock_XXXXXX") || exit 1
cleanup() { rm -f "$tmp"; }
trap cleanup EXIT INT TERM

cat > "$tmp" <<'EOF'
;; Main FORMATs (locks the outbuf table) in a tight loop while a short-lived
;; worker exits under it, dropping the thread count 2->1.  A build that skips
;; the unlock leaks the table mutex, and the next FORMAT deadlocks.
(dotimes (gen 4000)
  (let ((w (mp:make-thread (lambda () nil) :name "x")))
    (dotimes (k 20)
      (format nil "~a-~a-~a" gen k (list k (* k 2))))
    (mp:join-thread w)))
(format t "STREAM-MUTEX-OK~%")
EOF

# Deterministic per run for the buggy build; a few runs guard against a
# lucky scheduler.
RUNS=3
i=0
while [ "$i" -lt "$RUNS" ]; do
    i=$((i + 1))
    out=$("$TIMEOUT" 30 "$CLAMIGA" --no-userinit --heap 4M --non-interactive \
          --load "$tmp" </dev/null 2>&1)
    ec=$?
    if [ "$ec" -eq 124 ]; then
        echo "  FAIL mt_stream_mutex_leak (run $i/$RUNS timed out — leaked table mutex)"
        echo ""; echo "0 passed, 1 failed, 1 total"; echo "FAIL"; exit 1
    elif [ "$ec" -ne 0 ]; then
        echo "  FAIL mt_stream_mutex_leak (run $i/$RUNS ec=$ec — crash)"
        echo "    output: $(echo "$out" | tail -5)"
        echo ""; echo "0 passed, 1 failed, 1 total"; echo "FAIL"; exit 1
    elif ! echo "$out" | grep -q "STREAM-MUTEX-OK"; then
        echo "  FAIL mt_stream_mutex_leak (run $i/$RUNS — no STREAM-MUTEX-OK marker)"
        echo "    output: $(echo "$out" | tail -5)"
        echo ""; echo "0 passed, 1 failed, 1 total"; echo "FAIL"; exit 1
    fi
done

echo "  ok  mt_stream_mutex_leak ($RUNS runs clean)"
echo ""
echo "1 passed, 0 failed, 1 total"
echo "PASS"
exit 0
