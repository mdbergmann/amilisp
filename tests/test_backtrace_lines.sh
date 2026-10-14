#!/bin/sh
# Backtrace source lines.
#
# The line a backtrace frame reports comes from the frame's saved ip and the
# bytecode's pc->line map.  Two things used to go wrong:
#
#   1. The innermost frame's ip was only saved when a BYTECODE callee was
#      pushed.  An error signalled by a builtin, an FFI stub or an
#      OP_AMIGA_CALL (no frame of their own) was therefore reported at the
#      line of the previous bytecode call -- the reaction clicktab example
#      died on line 135 and the message said line 134.  The VM now saves ip
#      before every builtin / stub / library call (OP_CALL, OP_APPLY,
#      OP_AMIGA_CALL).
#
#   2. A suspended caller's ip is the RESUME point, just past its call.  When
#      the next form's code starts right there -- (list (f) (g)) with (g) on
#      the following line -- the lookup landed on (g)'s line.  The lookup now
#      maps ip - 1, the last byte of the call instruction.
#
# Run: sh tests/test_backtrace_lines.sh build/host/amilisp

CLAMIGA="${1:-build/host/amilisp}"

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)
if [ -z "$TIMEOUT" ]; then
    echo "SKIP test_backtrace_lines: neither timeout nor gtimeout on PATH"
    exit 0
fi

dir=$(mktemp -d "${TMPDIR:-/tmp}/clamiga_btlines_XXXXXX") || exit 1
trap 'rm -rf "$dir"' EXIT

src="$dir/lines.lisp"
out="$dir/out.txt"

# Every line number below is asserted -- keep the layout.
cat > "$src" <<'EOF'
;; written by tests/test_backtrace_lines.sh -- the line numbers are asserted
(require "amiga/ffi")
(defun btl-inner ()
  (list 1)
  (gethash 1 5))

(defun btl-outer ()
  (list (btl-inner)
        (identity 2)))

(btl-outer)

(defvar *btl-base* (ffi:make-foreign-pointer 4))
(amiga.ffi:defcfun btl-ffi *btl-base* -48 (:a0 gadget :d0 refresh))
(defun btl-ffi-bad ()
  (list 1)
  (btl-ffi nil "yes"))

(btl-ffi-bad)

(defun btl-ffi-ok ()
  (list 1)
  (btl-ffi nil t))

(btl-ffi-ok)

(defun btl-apply ()
  (list 1)
  (apply #'gethash (list 1 5)))

(btl-apply)

(defun btl-funcall-stub ()
  (list 1)
  (funcall #'btl-ffi nil 'refresh))

(btl-funcall-stub)
EOF

"$TIMEOUT" 60 "$CLAMIGA" --non-interactive --load "$src" < /dev/null > "$out" 2>&1

fail=0

expect_frame() {
    # $1 = description, $2 = grep pattern for one backtrace line
    if ! grep -q "$2" "$out"; then
        echo "FAIL: $1"
        echo "      expected a backtrace line matching: $2"
        fail=1
    fi
}

# 1. builtin error: the innermost frame is the builtin's call line (5), not
#    the line of the earlier bytecode call (4)
expect_frame "builtin error reported at its own line" \
    '0: BTL-INNER (.*lines\.lisp:5)'
# 2. the suspended caller: the line of ITS call (8), not of the next form (9)
expect_frame "suspended caller reported at its call line" \
    '1: BTL-OUTER (.*lines\.lisp:8)'
# OP_APPLY to a builtin, same rule
expect_frame "APPLY builtin error reported at its own line" \
    '0: BTL-APPLY (.*lines\.lisp:29)'

# 3. library call (OP_AMIGA_CALL) with an argument no register can carry:
#    the type error names the argument and is reported at the call's line
expect_frame "rejected FFI argument reported at the call's line" \
    '0: BTL-FFI-BAD (.*lines\.lisp:17)'
if ! grep -q 'register argument 2 (:D0) must be an integer, a foreign pointer, T or NIL -- got "yes"' "$out"; then
    echo "FAIL: rejected FFI argument message does not name argument, register and value"
    fail=1
fi
# a stub called through FUNCALL: the stub's own dispatch, same check
expect_frame "rejected FFI argument via FUNCALL reported at the call's line" \
    '0: BTL-FUNCALL-STUB (.*lines\.lisp:35)'
if ! grep -q 'register argument 2 (:D0) .* got REFRESH' "$out"; then
    echo "FAIL: rejected FFI argument via FUNCALL: message does not name the value"
    fail=1
fi

# 4. T is a register image (1): the call passes the argument check and, on
#    the host, stops at the platform error -- at the call's line
expect_frame "accepted FFI call reported at the call's line" \
    '0: BTL-FFI-OK (.*lines\.lisp:23)'
if ! grep -q 'only available on AmigaOS/MorphOS' "$out"; then
    echo "FAIL: FFI call with T did not reach the platform check"
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo "--- output ---"
    cat "$out"
else
    echo "PASS test_backtrace_lines"
fi
exit "$fail"
