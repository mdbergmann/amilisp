#!/bin/sh
# Regression test (GC audit tier 4, batch 6c, P-out_str): concurrent
# printing of arena-backed strings/symbols to a shared file stream while
# peers force compactions must not emit garbage or crash.
#
# Pre-fix, the printer passed raw arena pointers (s->data,
# cl_symbol_name(...)) into the stream write path.  Under MT the write
# can block on the contended per-stream iolock inside a GC safe region;
# a peer's compaction then relocates the string and the write keeps
# reading the pre-move address — emitting freed arena bytes.  The fix
# routes every such site through out_str_lisp / out_symbol_name_lisp
# (rooted CL_Obj, chunked C-buffer copies, per-chunk re-derive).
#
# Scheduler-dependent; runs as a smoke guard (the deterministic
# single-thread coverage lives in the gc-stress printer suite).
#
# Run: sh tests/test_mt_print_stress.sh build/host/amilisp

AMILISP="${1:-build/host/amilisp}"

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)
if [ -z "$TIMEOUT" ]; then
    echo "SKIP test_mt_print_stress: neither timeout nor gtimeout on PATH"
    exit 0
fi

tmp=$(mktemp "${TMPDIR:-/tmp}/amilisp_mtprint_XXXXXX") || exit 1
outdir=$(mktemp -d "${TMPDIR:-/tmp}/amilisp_mtprint_dir_XXXXXX") || exit 1
trap 'rm -f "$tmp"; rm -rf "$outdir"' EXIT

cat > "$tmp" <<EOF
(defvar *outpath* "$outdir/print-stress.txt")
EOF
cat >> "$tmp" <<'EOF'
;; 3 printers write freshly-consed strings and symbols to ONE file stream
;; (contended iolock => parked writers) while an allocator thread churns
;; the heap to trigger compactions mid-write.
(defvar *stream* (open *outpath* :direction :output :if-exists :supersede))
(defvar *stop* nil)
(defvar *alloc*
  (mp:make-thread
   (lambda ()
     (loop until *stop*
           do (make-string 4096 :initial-element #\y)))))
(defvar *printers*
  (let (acc)
    (dotimes (tid 3 acc)
      (push
       (let ((tid tid))
         (mp:make-thread
          (lambda ()
            (dotimes (i 120)
              ;; Fresh string (relocatable) + fresh symbol each round.
              (let ((s (make-string 300 :initial-element #\x))
                    (sym (intern (format nil "PRT-~d-~d" tid i))))
                (princ s *stream*)
                (prin1 sym *stream*)
                (terpri *stream*)))
            :ok)))
       acc))))
(let ((n 0))
  (dolist (th *printers*)
    (when (eq (mp:join-thread th) :ok) (incf n)))
  (setf *stop* t)
  (mp:join-thread *alloc*)
  (close *stream*)
  (format t "PRINT-STRESS-DONE=~d~%" n))
EOF

runs=3
i=1
while [ $i -le $runs ]; do
    out=$("$TIMEOUT" 90 "$AMILISP" --no-userinit --heap 16M \
              --non-interactive --load "$tmp" </dev/null 2>&1)
    status=$?
    fail() {
        echo "FAIL mt_print_stress (run $i/$runs — $1)"
        echo "$out" | tail -5 | sed 's/^/    /'
        echo "0 passed, 1 failed, 1 total"
        exit 1
    }
    [ $status -eq 0 ] || fail "exit $status: crash/hang"
    printf '%s' "$out" | grep -q "PRINT-STRESS-DONE=3" || fail "printers did not finish"
    # Concurrent writers legitimately interleave CHUNKS on the shared
    # stream, so line structure is not checkable — but the byte SET is:
    # only x, P, R, T, -, digits and newlines should appear.  ", \ and |
    # are also tolerated: *print-escape* is still a shared GLOBAL (the
    # FS16 / print-controls-to-TLV item, tier-4 batch 7), so a concurrent
    # PRIN1 can flip a peer's in-flight PRINC into escape mode and emit
    # quoting characters — a value race, not memory corruption.  Anything
    # else is freed-arena garbage read through a stale pointer mid-write.
    if LC_ALL=C grep -q '[^xPRT0-9"|\\-]' "$outdir/print-stress.txt"; then
        echo "FAIL mt_print_stress (run $i/$runs — garbage bytes in output)"
        LC_ALL=C grep -c '[^xPRT0-9"|\\-]' "$outdir/print-stress.txt" | head -1 | sed 's/^/    lines with garbage: /'
        echo "0 passed, 1 failed, 1 total"
        exit 1
    fi
    i=$((i + 1))
done

echo "  ok  mt_print_stress ($runs runs clean)"
echo "1 passed, 0 failed, 1 total"
exit 0
