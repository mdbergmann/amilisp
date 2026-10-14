#!/bin/sh
# FINISH-OUTPUT / FORCE-OUTPUT must reach the OS through every stream
# DESIGNATOR the standard allows (CLHS 21.1.1.1), not just through an explicit
# concrete-stream argument.
#
# Regression: the builtins were guarded by `n > 0 && CL_STREAM_P(args[0])` and
# then dispatched on the argument's own stream_type, so
#
#   (finish-output)                   ; designator absent -> *STANDARD-OUTPUT*
#   (finish-output t)                 ; -> *TERMINAL-IO*, a two-way wrapper
#   (finish-output <synonym stream>)
#   (finish-output <two-way stream>)
#
# every one of them flushed nothing at all — silently, returning NIL as if it
# had.  Only an explicit file/socket/broadcast stream ever reached the platform
# layer.  That is the shape of bug an unattended batch run pays for: a harness
# that dutifully flushes its log after each step is writing into a buffer it
# never empties, and when the process dies the tail of the log is not where it
# died.
#
# The probe: a file stream carries a userland write buffer (4K IOBuf on
# AmigaOS, stdio's on the host), and is opened here in append/shared mode so a
# second, independent input stream may read the file while the writer is still
# open.  The payload can only be seen there if the flush actually happened.
# Order matters: read back BEFORE closing, since CLOSE flushes too and would
# hide the bug; the close afterwards is only cleanup.
#
# Run: sh tests/test_finish_output_flush.sh build/host/amilisp

AMILISP="${1:-build/host/amilisp}"

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)
if [ -z "$TIMEOUT" ]; then
    echo "SKIP test_finish_output_flush: neither timeout nor gtimeout on PATH"
    exit 0
fi

. "$(dirname "$0")/shpath.sh"

work=$(mktemp -d "${TMPDIR:-/tmp}/clamiga_fo_XXXXXX") || exit 1
tmp="$work/probe.lisp"
out="$work/out.txt"
trap 'rm -rf "$work"' EXIT

# The directory as amilisp must see it (Windows spelling under MSYS).
wdir=$(native_path "$work")

fail=0

cat > "$tmp" <<EOF
(defvar *probe-out* nil)

(defun peek (path)
  (with-open-file (in path :direction :input :if-does-not-exist nil)
    (if in (or (read-line in nil nil) "<empty>") "<missing>")))

;; Peek FIRST (that is the assertion), then close.  Closing after the peek
;; costs nothing -- the flush under test has already been proven by then --
;; and leaving the handles open leaks a lock on platforms that track them.
(defun probe (path stream)
  (prog1 (peek path) (close stream)))

(defun fresh (name)
  (let ((p (concatenate 'string "$wdir" "/" name)))
    (when (probe-file p) (delete-file p))
    (open p :direction :output :if-exists :append :if-does-not-exist :create)))

;; Absent designator -> *STANDARD-OUTPUT*.
(let* ((out (fresh "c1.txt")) (p (pathname out)))
  (let ((*standard-output* out))
    (write-string "PAYLOAD")
    (finish-output))
  (format t "case1=~A~%" (probe p out)))

;; T -> *TERMINAL-IO*, which is a two-way stream over the console.
(let* ((out (fresh "c2.txt")) (p (pathname out)))
  (write-string "PAYLOAD" out)
  (let ((*terminal-io* (make-two-way-stream (make-string-input-stream "") out)))
    (finish-output t))
  (format t "case2=~A~%" (probe p out)))

;; Synonym stream -> the symbol's value.
(let* ((out (fresh "c3.txt")) (p (pathname out)))
  (setq *probe-out* out)
  (write-string "PAYLOAD" out)
  (finish-output (make-synonym-stream '*probe-out*))
  (format t "case3=~A~%" (probe p out)))

;; Two-way stream -> its output child.
(let* ((out (fresh "c4.txt")) (p (pathname out)))
  (write-string "PAYLOAD" out)
  (finish-output (make-two-way-stream (make-string-input-stream "") out))
  (format t "case4=~A~%" (probe p out)))

;; FORCE-OUTPUT takes the same designator.
(let* ((out (fresh "c5.txt")) (p (pathname out)))
  (let ((*standard-output* out))
    (write-string "PAYLOAD")
    (force-output))
  (format t "case5=~A~%" (probe p out)))

;; Broadcast fans out to every component (this branch already worked).
(let* ((out (fresh "c6.txt")) (p (pathname out)))
  (write-string "PAYLOAD" out)
  (finish-output (make-broadcast-stream out))
  (format t "case6=~A~%" (probe p out)))

;; Nested: a synonym for a two-way whose output child is the file stream.
(let* ((out (fresh "c7.txt")) (p (pathname out)))
  (setq *probe-out* (make-two-way-stream (make-string-input-stream "") out))
  (write-string "PAYLOAD" out)
  (finish-output (make-synonym-stream '*probe-out*))
  (format t "case7=~A~%" (probe p out)))

;; Flushing a console stream must be a harmless no-op, not an error — this is
;; the form a batch harness writes after every step.
(finish-output *error-output*)
(force-output *standard-output*)
(format t "console-ok~%")
EOF

"$TIMEOUT" 60 "$AMILISP" --no-userinit --non-interactive --load "$tmp" < /dev/null > "$out" 2>&1

for c in 1 2 3 4 5 6 7; do
    if ! grep -q "^case$c=PAYLOAD\$" "$out"; then
        echo "FAIL: case$c did not reach the file — flush lost"
        grep "^case$c=" "$out" || echo "  (no case$c line at all)"
        fail=1
    fi
done

if ! grep -q '^console-ok$' "$out"; then
    echo "FAIL: flushing a console stream errored"
    cat "$out"
    fail=1
fi

exit $fail
