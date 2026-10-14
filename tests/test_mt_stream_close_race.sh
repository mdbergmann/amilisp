#!/bin/sh
# Regression test (GC audit tier 4, batch 6, ST2/ST3/ST5):
#
#  ST2  cl_stream_close checked the OPEN flag BEFORE acquiring the
#       iolock (TOCTOU): two concurrent CLOSE calls both passed the
#       check and both ran the platform close — the second one hitting
#       an fd/handle slot possibly already recycled for an unrelated
#       stream.  Fixed by re-checking under the iolock (files/sockets)
#       or claiming the flag with an atomic CAS (string streams).
#  ST3  cl_stream_free_outbuf checked the slot's .data outside the
#       table mutex — two racing frees could double-platform_free the
#       same buffer.  Check moved inside the mutex.
#  ST5  resolve_synonym accepted ANY heap object from the synonym's
#       symbol binding (only CL_HEAP_P checked) — closing a synonym
#       stream whose symbol was bound to e.g. a string treated the
#       string as a CL_Stream and scribbled stream fields into it.
#
# Run: sh tests/test_mt_stream_close_race.sh build/host/amilisp

CLAMIGA="${1:-build/host/amilisp}"

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)
if [ -z "$TIMEOUT" ]; then
    echo "SKIP test_mt_stream_close_race: neither timeout nor gtimeout on PATH"
    exit 0
fi

tmp=$(mktemp "${TMPDIR:-/tmp}/clamiga_closerace_XXXXXX") || exit 1
workdir=$(mktemp -d "${TMPDIR:-/tmp}/clamiga_closerace_dir_XXXXXX") || exit 1
trap 'rm -f "$tmp"; rm -rf "$workdir"' EXIT

cat > "$tmp" <<EOF
(defvar *dir* "$workdir")
EOF
cat >> "$tmp" <<'EOF'
;; ST5: closing a synonym stream whose symbol holds a NON-stream heap
;; object must be a no-op, not a type-confused field scribble.
(defvar *not-a-stream* (copy-seq "keep me intact"))
(handler-case (close (make-synonym-stream '*not-a-stream*))
  (error () nil))
(format t "ST5-INTACT=~a~%"
        (if (string= *not-a-stream* "keep me intact") "YES" "NO"))

;; ST2 (file iolock path): two threads race to close the same file
;; stream, repeatedly.  Pre-fix both performed the platform close —
;; double fclose / fd-slot reuse, typically a crash or heap abort.
(dotimes (round 30)
  (let ((s (open (format nil "~a/f~d.tmp" *dir* round)
                 :direction :output :if-exists :supersede)))
    (write-string "hello" s)
    (let ((t1 (mp:make-thread
               (lambda () (handler-case (close s) (error () nil)))))
          (t2 (mp:make-thread
               (lambda () (handler-case (close s) (error () nil))))))
      (mp:join-thread t1)
      (mp:join-thread t2))))

;; ST2/ST3 (string-stream CAS path + outbuf table): racing closes of a
;; string output stream — pre-fix both freed the outbuf slot.
(dotimes (round 30)
  (let ((ss (make-string-output-stream)))
    (write-string "abcdef" ss)
    (let ((t1 (mp:make-thread
               (lambda () (handler-case (close ss) (error () nil)))))
          (t2 (mp:make-thread
               (lambda () (handler-case (close ss) (error () nil))))))
      (mp:join-thread t1)
      (mp:join-thread t2))))

(format t "CLOSE-RACE-DONE~%")
EOF

runs=3
i=1
while [ $i -le $runs ]; do
    out=$("$TIMEOUT" 60 "$CLAMIGA" --no-userinit --non-interactive --load "$tmp" </dev/null 2>&1)
    status=$?
    if [ $status -ne 0 ]; then
        echo "FAIL mt_stream_close_race (run $i/$runs — exit $status: crash/hang)"
        echo "$out" | tail -5 | sed 's/^/    /'
        echo "0 passed, 1 failed, 1 total"
        exit 1
    fi
    if ! printf '%s' "$out" | grep -q "CLOSE-RACE-DONE"; then
        echo "FAIL mt_stream_close_race (run $i/$runs — no completion marker)"
        echo "$out" | tail -5 | sed 's/^/    /'
        echo "0 passed, 1 failed, 1 total"
        exit 1
    fi
    if ! printf '%s' "$out" | grep -q "ST5-INTACT=YES"; then
        echo "FAIL mt_stream_close_race (run $i/$runs — synonym close corrupted non-stream)"
        echo "$out" | grep "ST5-INTACT" | sed 's/^/    /'
        echo "0 passed, 1 failed, 1 total"
        exit 1
    fi
    if printf '%s' "$out" | grep -q "not allocated\|double free\|corrupted"; then
        echo "FAIL mt_stream_close_race (run $i/$runs — allocator abort)"
        echo "$out" | grep "not allocated\|double free\|corrupted" | head -3 | sed 's/^/    /'
        echo "0 passed, 1 failed, 1 total"
        exit 1
    fi
    i=$((i + 1))
done

echo "  ok  mt_stream_close_race ($runs runs clean)"
echo "1 passed, 0 failed, 1 total"
exit 0
