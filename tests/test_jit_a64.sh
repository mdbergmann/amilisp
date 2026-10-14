#!/bin/sh
# The AArch64 JIT's plumbing end to end (specs/native-backend-a64.md, phase
# 0): %JIT-COMPILE-STUB assembles `mov w0, #0; ret` (asm_a64.c), installs it
# in the executable code heap (codeheap.c), and OP_CALL enters it through
# cl_jit_invoke -- so the stubbed function returns NIL whatever its body
# says.  Then the native code has to go away again: a redefinition drops
# it, and a collection releases the dead function's block.
#
# A host without the backend (x86-64, Linux, Windows, `make host JIT=0`)
# returns NIL from %JIT-COMPILE-STUB; the test then checks only that and
# passes.  Also run by `make test-gc-stress` (AMILISP_GC_STRESS=1).
# Run: sh tests/test_jit_a64.sh [path-to-amilisp]

AMILISP="${1:-build/host/amilisp}"
passed=0
failed=0
total=0

check_contains() {
    desc="$1"
    needle="$2"
    haystack="$3"
    total=$((total + 1))
    case "$haystack" in
        *"$needle"*)
            echo "  ok  $desc"
            passed=$((passed + 1)) ;;
        *)
            echo "  FAIL  $desc"
            echo "    expected to contain: $needle"
            echo "    got: $(echo "$haystack" | head -16)"
            failed=$((failed + 1)) ;;
    esac
}

WORK="${TMPDIR:-/tmp}/jit_a64_$$"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

cat > "$WORK/stub.lisp" <<'EOF'
;; The default hot threshold, also under `make test-jit-eager`: a function
;; redefined below must still carry no native code right after its DEFUN.
(amilisp::%jit-set-hot-threshold 8)
(defun stubbed-0 () 42)
(defun stubbed-2 (a b) (+ a b))
(format t "BACKEND ~A~%" (if (amilisp::%jit-compile-stub #'stubbed-0) "A64" "NONE"))
(when (amilisp::%jit-dump-bytes #'stubbed-0)
  (let ((c0 (amilisp::%jit-invoke-count))
        (b0 (amilisp::%jit-native-bytes)))
    ;; The stub runs instead of the body, once per call, and leaves the
    ;; operand stack as the interpreter expects it.
    (format t "STUB-0 ~S~%" (list (stubbed-0) (stubbed-0)))
    (format t "INVOKES ~S~%" (- (amilisp::%jit-invoke-count) c0))
    (amilisp::%jit-compile-stub #'stubbed-2)
    (format t "BYTES-ADDED ~S~%" (- (amilisp::%jit-native-bytes) b0))
    (format t "STUB-2 ~S~%" (list (stubbed-2 1 2) (+ 10 (or (stubbed-2 3 4) 5))))
    (format t "STACK-INTACT ~S~%" (list 1 (stubbed-2 5 6) 3 (stubbed-0) 5))
    ;; The words: movz w0, #0 ; ret, little-endian.
    (format t "WORDS ~S~%" (amilisp::%jit-dump-bytes #'stubbed-0))
    ;; From another thread: the entry takes the caller's own thread.
    (format t "THREADS ~S~%"
            (mapcar #'mp:join-thread
                    (loop repeat 4
                          collect (mp:make-thread (lambda () (list (stubbed-0) (stubbed-2 1 1)))))))
    ;; With shadow frames on, cl_jit_invoke pushes and pops a VM frame.
    (amilisp::%jit-set-frames t)
    (format t "FRAMES ~S~%" (list (stubbed-0) (stubbed-2 1 1)))
    (amilisp::%jit-set-frames nil)
    ;; Replacing the stub frees the old block; a redefinition drops the code.
    (amilisp::%jit-compile-stub #'stubbed-0)
    (format t "RESTUB ~S~%" (stubbed-0))
    (defun stubbed-0 () 43)
    (format t "REDEFINED ~S ~S~%" (stubbed-0) (amilisp::%jit-dump-bytes #'stubbed-0))
    ;; Many dead stubbed functions: the sweep hands their blocks back
    ;; (cl_jit_free_native); nothing may crash or run freed code.
    (dotimes (i 200)
      (let ((f (compile nil `(lambda () ,i))))
        (amilisp::%jit-compile-stub f)
        (unless (null (funcall f)) (format t "STUB-WRONG ~D~%" i))))
    (ext:gc)
    (format t "SWEEP-OK ~S~%" (stubbed-2 7 8))))
(amilisp::%jit-disassemble #'stubbed-2)
(format t "DONE~%")
EOF

out=$("$AMILISP" --no-userinit --non-interactive --load "$WORK/stub.lisp" </dev/null 2>&1)
check_contains "the script ran to the end" "DONE" "$out"

case "$out" in
*"BACKEND NONE"*)
    echo "  (no AArch64 backend in this build: stub checks skipped)"
    check_contains "%JIT-COMPILE-STUB is NIL without a backend" "BACKEND NONE" "$out" ;;
*)
    check_contains "the stub installs" "BACKEND A64" "$out"
    check_contains "the stub runs instead of the body" "STUB-0 (NIL NIL)" "$out"
    check_contains "one native entry per call" "INVOKES 2" "$out"
    check_contains "two 4-byte instructions installed" "BYTES-ADDED 8" "$out"
    check_contains "arguments are dropped as after an interpreted call" "STUB-2 (NIL 15)" "$out"
    check_contains "the caller's operand stack is intact" "STACK-INTACT (1 NIL 3 NIL 5)" "$out"
    check_contains "the words are movz w0, #0 and ret" "WORDS (0 0 128 82 192 3 95 214)" "$out"
    check_contains "native code runs on worker threads" \
        "THREADS ((NIL NIL) (NIL NIL) (NIL NIL) (NIL NIL))" "$out"
    check_contains "shadow frames push and pop" "FRAMES (NIL NIL)" "$out"
    check_contains "a stub replaces a stub" "RESTUB NIL" "$out"
    check_contains "a redefinition drops the native code" "REDEFINED 43 NIL" "$out"
    check_contains "no stub ran the body" "SWEEP-OK NIL" "$out"
    total=$((total + 1))
    case "$out" in
        *STUB-WRONG*) echo "  FAIL  a stub ran the body"; failed=$((failed + 1)) ;;
        *) echo "  ok  every stub of a dead function ran"; passed=$((passed + 1)) ;;
    esac
    check_contains "the disassembler decodes the stub" "movz w0, #0x0, lsl #0" "$out"
    check_contains "the disassembler decodes ret" "ret" "$out" ;;
esac

echo "$passed passed, $failed failed, $total total"
[ "$failed" -eq 0 ]
