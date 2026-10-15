#!/bin/sh
# String-output-stream outbuf leak regressions, checked from Lisp via
# ext:%stream-outbuf-stats (used total).
#
# 1. Gray-streams printing fallbacks: princ/prin1/print/write/format/pprint
#    on a Gray stream capture through a temp string-output-stream.  That
#    temp must be CLOSEd (freeing its platform outbuf slot eagerly) — the
#    old code never closed it, leaking one slot per call until the next GC.
#    Under logging-heavy loads the leaked slots saturated the outbuf table
#    every ~200 printed nodes and (with the old fixed-size table) forced a
#    full stop-the-world GC per allocation — printing ran at GC speed
#    (eta-hab item-definition stalls, 60-470s per log event).
#
# 2. The outbuf table grows on demand past its 256-slot block: holding well
#    over one block's worth of simultaneously-open streams must work and
#    every stream's content must survive.
#
# Run: sh tests/test_stream_outbuf_leak.sh build/host/amilisp

AMILISP="${1:-build/host/amilisp}"

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)
if [ -z "$TIMEOUT" ]; then
    echo "SKIP test_stream_outbuf_leak: neither timeout nor gtimeout on PATH"
    exit 0
fi

tmp=$(mktemp "${TMPDIR:-/tmp}/amilisp_outbuf_XXXXXX") || exit 1
trap 'rm -f "$tmp"' EXIT

cat > "$tmp" <<'EOF'
(require "gray-streams")

;; Minimal Gray output stream that discards everything.
(defclass sink (gray:fundamental-character-output-stream) ())
(defmethod gray:stream-write-char ((s sink) ch) ch)
(defmethod gray:stream-write-string ((s sink) str &optional start end)
  (declare (ignore start end))
  str)

;; 1. Gray printing fallbacks must not leak outbuf slots.  50 iterations x
;;    5 printing calls = 250 slots if leaked (past a whole 256-slot block);
;;    with eager close the count stays at the nesting depth (single digits).
;;    Kept small enough that no GC runs mid-loop and reclaims the evidence.
(let ((s (make-instance 'sink))
      (before (car (ext:%stream-outbuf-stats))))
  (dotimes (i 50)
    (princ i s)
    (prin1 i s)
    (print i s)
    (write i :stream s)
    (format s "x~D" i))
  (let ((leaked (- (car (ext:%stream-outbuf-stats)) before)))
    (if (< leaked 10)
        (format t "GRAY-LEAK-OK~%")
        (format t "GRAY-LEAK-FAIL leaked=~D~%" leaked))))

;; 2. Table growth: >256 simultaneously-open string output streams, each
;;    holding distinct content that must survive until read back.
(let ((streams nil))
  (dotimes (i 300)
    (let ((s (make-string-output-stream)))
      (format s "content-~D" i)
      (push s streams)))
  (setf streams (nreverse streams))
  (let ((ok t) (i 0))
    (dolist (s streams)
      (unless (string= (get-output-stream-string s)
                       (format nil "content-~D" i))
        (setf ok nil))
      (incf i))
    (dolist (s streams) (close s))
    (format t "~A~%" (if ok "GROWTH-OK" "GROWTH-FAIL"))))
EOF

fail() {
    echo "FAIL stream_outbuf_leak ($1)"
    echo "$out" | tail -8 | sed 's/^/    /'
    echo "0 passed, 1 failed, 1 total"
    exit 1
}

out=$("$TIMEOUT" 60 "$AMILISP" --no-userinit --non-interactive --load "$tmp" \
      </dev/null 2>&1)
status=$?
[ $status -eq 0 ] || fail "exit $status: hang or crash"
printf '%s' "$out" | grep -q "GRAY-LEAK-OK" || fail "gray fallbacks leak outbufs"
printf '%s' "$out" | grep -q "GROWTH-OK" || fail "outbuf table growth broken"

echo "  ok  stream_outbuf_leak (gray eager close + table growth)"
echo "1 passed, 0 failed, 1 total"
exit 0
