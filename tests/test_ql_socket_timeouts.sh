#!/bin/sh
# Regression: quicklisp's network layers must arm socket timeouts.
#
# Field hang (MorphOS, 2026-07-28): (amilisp-ql:install) wedged half-way
# through the client-tar download — the connection stalled and the READFILL
# parked in the socket reactor with NO deadline armed (socket_rtimeout
# defaults to 0 = block forever), leaving the whole image in Wait until
# Ctrl-C.  Both quicklisp network layers opened their connections with a
# bare (ext:open-tcp-stream host port):
#   - lib/quicklisp.lisp     (quickstart bootstrap, used by the installer)
#   - lib/quicklisp-compat.lisp (post-install dist downloads)
# The fix arms a 30s connect timeout and 60s read/write timeouts, so a
# stalled transfer raises EXT:SOCKET-TIMEOUT instead of hanging forever.
#
# This test loads the quickstart's network layer (no network access — the
# connection goes to a listener inside the same image) and asserts the
# stream returned by its OPEN-CONNECTION really has the timeouts armed.
# The timeout *mechanics* (expiry raises EXT:SOCKET-TIMEOUT) are covered
# by the socket tests in tests/amiga/run-tests.lisp.
#
# lib/quicklisp-compat.lisp cannot be loaded without a quicklisp install,
# so its (identical) wiring is asserted textually below instead.

AMILISP="${1:-build/host/amilisp}"
passed=0
failed=0
total=0

check() {
    desc="$1"
    expected="$2"
    actual="$3"
    total=$((total + 1))
    if [ "$actual" = "$expected" ]; then
        echo "  ok  $desc"
        passed=$((passed + 1))
    else
        echo "  FAIL  $desc"
        echo "    expected: $expected"
        echo "    got:      $actual"
        failed=$((failed + 1))
    fi
}

result=$("$AMILISP" --no-userinit --non-interactive --heap 24M --eval '
(progn
  (load "lib/quicklisp.lisp")
  (let ((l (ext:socket-listen 0 t)))
    (unwind-protect
         (let* ((p (ext:socket-local-port l))
                (c (funcall (find-symbol "OPEN-CONNECTION" "QLQS-NETWORK")
                            "127.0.0.1" p))
                (s (ext:socket-accept l)))
           (unwind-protect
                (format t "TIMEOUTS:~a ~a~%"
                        (= 60 (ext:socket-stream-timeout c :input))
                        (= 60 (ext:socket-stream-timeout c :output)))
             (close c) (close s)))
      (close l))))' </dev/null 2>&1 | grep "TIMEOUTS:")
check "quickstart open-connection arms read/write timeouts" \
      "TIMEOUTS:T T" "$result"

# quicklisp-compat.lisp needs an installed quicklisp to load, so pin its
# wiring textually: open-connection must pass a connect timeout and set
# both stream timeouts.
compat_ok=$(awk '/defun open-connection/,/^$/' lib/quicklisp-compat.lisp)
case "$compat_ok" in
  *"ext:open-tcp-stream host port 30"*) r1=yes ;;
  *) r1=no ;;
esac
case "$compat_ok" in
  *"(setf (ext:socket-stream-timeout stream :input) 60)"*) r2=yes ;;
  *) r2=no ;;
esac
check "compat open-connection passes connect timeout" "yes" "$r1"
check "compat open-connection arms read timeout"      "yes" "$r2"

echo ""
echo "$passed passed, $failed failed, $total total"
[ "$failed" -eq 0 ]
