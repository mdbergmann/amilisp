#!/bin/sh
# Tier-4 FS16: the *PRINT-* keyword overrides in WRITE-TO-STRING / WRITE /
# PPRINT / FORMAT's integer renderer must be THREAD-LOCAL dynamic binds.
# Pre-fix they mutated the GLOBAL control cells and restored them after the
# print, so a peer thread sampling *PRINT-BASE* while another thread runs
# (write-to-string x :base 2) observed base 2 (pre-fix: ~19.5k of 20k
# samples clobbered on the macOS scheduler; post-fix: zero).
# Run: sh tests/test_mt_print_controls.sh build/host/amilisp

CLAMIGA="${1:-build/host/amilisp}"
passed=0
failed=0
total=0

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

cat > "$WORK/mt-print-controls.lisp" <<'LISPEOF'
(defvar *seen-bad* 0)
(defvar *stop* nil)
(defvar *writer*
  (mp:make-thread
   (lambda ()
     (let ((l (make-list 200 :initial-element 12345)))
       (loop until *stop*
             do (write-to-string l :base 2))))
   :name "writer"))
(dotimes (i 20000)
  (unless (eql *print-base* 10)
    (setf *seen-bad* (1+ *seen-bad*))))
(setf *stop* t)
(mp:join-thread *writer*)
(format t "T7C-MT-PRINT-BAD:~a~%" *seen-bad*)
(quit)
LISPEOF

out=$("$CLAMIGA" --no-userinit --load "$WORK/mt-print-controls.lisp" </dev/null 2>&1)

total=$((total + 1))
case "$out" in
  *"T7C-MT-PRINT-BAD:0"*)
    echo "  ok  print_overrides_are_thread_local"
    passed=$((passed + 1)) ;;
  *T7C-MT-PRINT-BAD:*)
    echo "  FAIL  print_overrides_are_thread_local (peer observed a clobbered *print-base*)"
    echo "$out" | grep "T7C-MT-PRINT-BAD"
    failed=$((failed + 1)) ;;
  *)
    echo "  FAIL  print_overrides_are_thread_local (run died)"
    echo "$out" | tail -5
    failed=$((failed + 1)) ;;
esac

echo ""
echo "$passed passed, $failed failed, $total total"
[ "$failed" -eq 0 ]
