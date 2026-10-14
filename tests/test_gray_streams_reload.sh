#!/bin/sh
# Regression test: re-LOADing lib/gray-streams.lisp must stay idempotent.
#
# Bug: gray-streams.lisp installs its CL-I/O integration inside a top-level
# (LET ((orig-close (symbol-function 'close)) ...) ...).  Boot already loads
# the file once, and lib/quicklisp-compat.lisp LOADs it again unconditionally
# (and any library pulling trivial-gray-streams can trigger yet another LOAD).
# On the second LOAD every ORIG-X captured (SYMBOL-FUNCTION 'X) when X was
# ALREADY the gray-overridden generic function — so e.g. ORIG-CLOSE became the
# CLOSE GF itself.  The (STREAM T) method's (FUNCALL ORIG-CLOSE STREAM) then
# re-dispatched CLOSE on the same class, recursing forever:
#   %GF-DISPATCH-CACHED -> apply EMF -> orig-close -> CLOSE GF -> ... overflow
# This surfaced as a "%GF-DISPATCH-CACHED" VM frame stack overflow while
# quickloading (closing an ordinary file stream during ql:quickload :sento).
#
# The fix guards the integration LET with a one-shot symbol-plist marker so it
# installs exactly once per image; subsequent LOADs are a no-op.
#
# This test LOADs gray-streams.lisp a second time, then exercises CLOSE,
# READ-CHAR and WRITE-CHAR on ordinary streams.  Before the fix the CLOSE call
# overflowed the VM frame stack (non-zero exit / no DONE marker); after the fix
# it completes cleanly.
#
# Run: sh tests/test_gray_streams_reload.sh build/host/amilisp

AMILISP="${1:-build/host/amilisp}"

# Paths written into the generated .lisp file must be in amilisp's own
# spelling: under MSYS2 "$(pwd)" is /c/... but a native amilisp.exe resolves
# that against the current drive as C:/c/... and the LOAD fails.  (Before
# this, LOAD's per-form recovery swallowed that error on Windows and the test
# still printed its DONE marker — see the ERROR: check below.)
. "$(dirname "$0")/shpath.sh"

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)
if [ -z "$TIMEOUT" ]; then
    echo "SKIP test_gray_streams_reload: neither timeout nor gtimeout on PATH"
    exit 0
fi

# Resolve repo root from this script's location so lib/gray-streams.lisp loads
# regardless of the caller's CWD.
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
gray="$repo_root/lib/gray-streams.lisp"

if [ ! -f "$gray" ]; then
    echo "SKIP test_gray_streams_reload: $gray not found"
    exit 0
fi

tmp=$(mktemp "${TMPDIR:-/tmp}/clamiga_gray_reload_XXXXXX") || exit 1
data=$(mktemp "${TMPDIR:-/tmp}/clamiga_gray_data_XXXXXX") || exit 1
cleanup() { rm -f "$tmp" "$data"; }
trap cleanup EXIT INT TERM

printf 'hello gray streams\n' > "$data"

gray_lisp=$(native_path "$gray")
data_lisp=$(native_path "$data")

cat > "$tmp" <<EOF
(setq *load-verbose* nil)
;; Base boot does not pull gray-streams (it arrives via quicklisp-compat),
;; so reproduce the real double-load here: LOAD the file twice.  The second
;; LOAD re-runs the integration block, and before the fix that re-captured
;; ORIG-CLOSE et al as the gray GFs, so any subsequent CLOSE recursed to a
;; VM frame stack overflow.
(load "$gray_lisp")
(load "$gray_lisp")
;; Exercise the wrapped CL-I/O entry points on an ordinary file stream.
(let ((s (open "$data_lisp" :direction :input)))
  (read-char s)            ; READ-CHAR wrapper
  (close s))               ; CLOSE (stream t) -> must NOT re-dispatch itself
(let ((o (make-string-output-stream)))
  (write-char #\\X o)       ; WRITE-CHAR wrapper
  (close o))
(with-open-file (s "$data_lisp" :direction :input)
  (read-line s))
;; Second reload bug: the reload re-DEFCLASSes GRAY:FUNDAMENTAL-*-STREAM.
;; %ENSURE-CLASS used to allocate a FRESH class metaobject on redefinition,
;; so OUTPUT-STREAM-P / INPUT-STREAM-P (methods specialized on the OLD
;; metaobject) returned NIL for any Gray stream class defined AFTER the
;; reload (STREAMP stayed T).  CLHS 4.3.6: redefinition updates the existing
;; class object — FIND-CLASS identity and method dispatch must survive.
(defclass gr-out (gray:fundamental-character-output-stream) ())
(defmethod gray:stream-write-char ((s gr-out) c) (declare (ignore s c)) nil)
(defclass gr-in (gray:fundamental-character-input-stream) ())
(let ((o (make-instance 'gr-out)) (i (make-instance 'gr-in)))
  (format t "GRAY-RELOAD-PREDICATES:~a ~a ~a ~a~%"
          (output-stream-p o) (input-stream-p o)
          (input-stream-p i) (output-stream-p i)))
(format t "GRAY-RELOAD-DONE~%")
EOF

out=$("$TIMEOUT" 20 "$AMILISP" --no-userinit --heap 16M --non-interactive --load "$tmp" </dev/null 2>&1)
ec=$?

total=1
if [ "$ec" -eq 124 ]; then
    echo "  FAIL gray_streams_reload (timed out, ec=124 — CLOSE recursion hang)"
    echo "    output: $(echo "$out" | tail -5)"
    echo ""; echo "0 passed, 1 failed, $total total"; echo "FAIL"; exit 1
elif [ "$ec" -ne 0 ]; then
    echo "  FAIL gray_streams_reload (ec=$ec — likely VM frame stack overflow)"
    echo "    output: $(echo "$out" | tail -8)"
    echo ""; echo "0 passed, 1 failed, $total total"; echo "FAIL"; exit 1
elif ! echo "$out" | grep -q "GRAY-RELOAD-DONE"; then
    echo "  FAIL gray_streams_reload (no GRAY-RELOAD-DONE marker)"
    echo "    output: $(echo "$out" | tail -8)"
    echo ""; echo "0 passed, 1 failed, $total total"; echo "FAIL"; exit 1
elif echo "$out" | grep -q "^ERROR:"; then
    # LOAD recovers per form and carries on, so a failed (LOAD gray) — e.g. a
    # path amilisp cannot resolve — would still reach the DONE marker while
    # exercising nothing.  Any ERROR: line means the script did not run as
    # written.
    echo "  FAIL gray_streams_reload (ERROR: in output — a form failed and LOAD recovered past it)"
    echo "    output: $(echo "$out" | grep -A3 "^ERROR:" | head -12)"
    echo ""; echo "0 passed, 1 failed, $total total"; echo "FAIL"; exit 1
elif echo "$out" | grep -qi "overflow\|RUNAWAY"; then
    echo "  FAIL gray_streams_reload (overflow/runaway in output)"
    echo "    output: $(echo "$out" | tail -8)"
    echo ""; echo "0 passed, 1 failed, $total total"; echo "FAIL"; exit 1
elif ! echo "$out" | grep -q "GRAY-RELOAD-PREDICATES:T NIL T NIL"; then
    echo "  FAIL gray_streams_reload (OUTPUT-STREAM-P / INPUT-STREAM-P wrong on a Gray class defined after the reload — class redefinition lost the metaobject)"
    echo "    output: $(echo "$out" | grep GRAY-RELOAD-PREDICATES; echo "$out" | tail -4)"
    echo ""; echo "0 passed, 1 failed, $total total"; echo "FAIL"; exit 1
fi

echo "  ok  gray_streams_reload"
echo ""
echo "1 passed, 0 failed, $total total"
echo "PASS"
exit 0
