#!/bin/sh
# Tier-4 ST8: a dead file stream that was never CLOSEd must have its OS
# handle closed by the GC sweep finalizer (with a loud stderr warning)
# instead of leaking the fd forever.
# Run: sh tests/test_gc_stream_finalize.sh build/host/amilisp

CLAMIGA="${1:-build/host/amilisp}"
case "$CLAMIGA" in
  /*) ;;
  *) CLAMIGA="$(pwd)/$CLAMIGA" ;;
esac
passed=0
failed=0
total=0

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

cat > "$WORK/finalize.lisp" <<LISPEOF
;; Open a file stream and immediately drop the only reference.
(let ((junk nil))
  (open "$WORK/gc-finalize-test.tmp" :direction :output :if-exists :supersede)
  ;; Allocate enough to guarantee at least one full collection sweeps it.
  (dotimes (i 20000) (setf junk (list i junk))))
(gc)
(gc)
(format t "T7C-FIN-DONE~%")
(quit)
LISPEOF

out=$("$CLAMIGA" --no-userinit --load "$WORK/finalize.lisp" </dev/null 2>&1)

total=$((total + 1))
case "$out" in
  *"closing a file stream that was dropped without CLOSE"*)
    echo "  ok  gc_closes_dropped_file_stream"
    passed=$((passed + 1)) ;;
  *)
    echo "  FAIL  gc_closes_dropped_file_stream (no sweep-close warning)"
    echo "$out" | tail -5
    failed=$((failed + 1)) ;;
esac

total=$((total + 1))
case "$out" in
  *T7C-FIN-DONE*)
    echo "  ok  gc_stream_finalize_run_completes"
    passed=$((passed + 1)) ;;
  *)
    echo "  FAIL  gc_stream_finalize_run_completes"
    failed=$((failed + 1)) ;;
esac

cat > "$WORK/finalize-socket.lisp" <<LISPEOF
;; Open a listening socket stream (ephemeral port, no network dependency)
;; and immediately drop the only reference without CLOSE.
(let ((junk nil))
  (ext:socket-listen 0)
  ;; Allocate enough to guarantee at least one full collection sweeps it.
  (dotimes (i 20000) (setf junk (list i junk))))
(gc)
(gc)
(format t "T8-SOCK-FIN-DONE~%")
(quit)
LISPEOF

out=$("$CLAMIGA" --no-userinit --load "$WORK/finalize-socket.lisp" </dev/null 2>&1)

total=$((total + 1))
case "$out" in
  *"closing a socket stream that was dropped without CLOSE"*)
    echo "  ok  gc_closes_dropped_socket_stream"
    passed=$((passed + 1)) ;;
  *)
    echo "  FAIL  gc_closes_dropped_socket_stream (no sweep-close warning)"
    echo "$out" | tail -5
    failed=$((failed + 1)) ;;
esac

total=$((total + 1))
case "$out" in
  *T8-SOCK-FIN-DONE*)
    echo "  ok  gc_socket_stream_finalize_run_completes"
    passed=$((passed + 1)) ;;
  *)
    echo "  FAIL  gc_socket_stream_finalize_run_completes"
    failed=$((failed + 1)) ;;
esac

echo ""
echo "$passed passed, $failed failed, $total total"
[ "$failed" -eq 0 ]
