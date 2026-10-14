#!/bin/sh
# Regression test: FILE-POSITION on a Gray-stream instance must answer NIL
# ("position cannot be determined", CLHS), not signal an error.
#
# Bug: the C builtin FILE-POSITION rejects anything that is not a builtin
# CL_Stream with "FILE-POSITION: not a stream" — and a Gray stream is a CLOS
# instance.  The gray shim (lib/gray-streams.lisp) redefines the stream API
# but did not cover FILE-POSITION, so flexi-streams' MAYBE-REWIND probe —
# which calls (FILE-POSITION stream) on the underlying chunga stream inside
# its character-decoding READ-SEQUENCE path and expects NIL from
# non-positionable streams — crashed instead.  Net effect: every drakma
# response that was chunked + text + no content-length failed, on every
# transport (plain HTTP and HTTPS alike).
#
# The fix routes Gray instances to the trivial-gray-streams
# STREAM-FILE-POSITION protocol when that system is loaded and answers NIL
# otherwise (both directions: query and set); non-Gray streams keep the
# original C builtin.
#
# Run: sh tests/test_gray_file_position.sh build/host/amilisp

CLAMIGA="${1:-build/host/amilisp}"

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)
if [ -z "$TIMEOUT" ]; then
    echo "SKIP test_gray_file_position: neither timeout nor gtimeout on PATH"
    exit 0
fi

# Two --eval forms: the GRAY package must exist before the second form is
# READ (a single form would hit "Package GRAY not found" at read time).
OUT=$("$TIMEOUT" 60 "$CLAMIGA" --no-userinit --non-interactive \
  --eval '(load "lib/gray-streams.lisp")' \
  --eval '
(progn
  (defclass fp-probe-stream (gray:fundamental-character-input-stream) ())
  (let ((gs (make-instance (quote fp-probe-stream))))
    ;; Query and set on a Gray instance: NIL, and above all no error.
    (print (list :gray-query (file-position gs)))
    (print (list :gray-set (file-position gs 10))))
  ;; The original builtin still serves real streams.
  (with-open-file (f "README.md")
    (read-char f)
    (print (list :file-query-works (numberp (file-position f))))
    (print (list :file-set-works (and (file-position f 0) t))))
  (print :fp-done))' </dev/null 2>&1)
rc=$?

if [ $rc -ne 0 ]; then
    echo "FAIL test_gray_file_position: amilisp exited rc=$rc"
    echo "$OUT" | tail -15
    exit 1
fi

for marker in "(:GRAY-QUERY NIL)" "(:GRAY-SET NIL)" \
              "(:FILE-QUERY-WORKS T)" "(:FILE-SET-WORKS T)" ":FP-DONE"; do
    if ! echo "$OUT" | grep -qF "$marker"; then
        echo "FAIL test_gray_file_position: missing $marker"
        echo "$OUT" | tail -15
        exit 1
    fi
done
exit 0
