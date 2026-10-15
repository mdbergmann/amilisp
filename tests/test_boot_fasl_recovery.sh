#!/bin/sh
# Regression test: amilisp must recover gracefully from a stale lib/boot.fasl.
#
# Bug: when lib/boot.fasl carried an incompatible CL_FASL_VERSION (e.g. left
# over from a previous binary), cl_fasl_load fired cl_error("FASL: unsupported
# version") which longjmped past the in-flight cl_vm_eval.  cl_error_unwind
# resets nlx/dynbind/handlers/GC roots but NOT cl_vm.sp / cl_vm.fp, so the
# fall-through (load "lib/boot.lisp") ran on top of a corrupted VM stack —
# observed as 100% CPU then a segfault on 68040.
#
# Fix: pre-validate the fasl header at boot, drop a stale file before
# longjmping; and reset cl_vm.sp/fp on error in the boot CL_CATCH blocks.
#
# Run: sh tests/test_boot_fasl_recovery.sh build/host/amilisp

AMILISP="${1:-build/host/amilisp}"
AMILISP_ABS=$(cd "$(dirname "$AMILISP")" && pwd)/$(basename "$AMILISP")
TMPDIR="${TMPDIR:-/tmp}"
WORK="$TMPDIR/amilisp_boot_recover_$$"

passed=0
failed=0
total=0

cleanup() {
    rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

# Stage a working dir with a copy of lib/ so the corruption doesn't touch
# the real build artefact.
mkdir -p "$WORK/lib"
cp lib/boot.lisp "$WORK/lib/boot.lisp"
cp lib/boot.fasl "$WORK/lib/boot.fasl"
cp lib/clos.lisp "$WORK/lib/clos.lisp"

# Corrupt the version u16 (offset 4-5, big-endian) so the loader sees a
# magic match but a version mismatch — the path that used to wedge.
printf '\xff\xff' | dd of="$WORK/lib/boot.fasl" bs=1 seek=4 count=2 conv=notrunc 2>/dev/null

# Ensure mtime stays >= source so the loader still prefers the fasl.
touch "$WORK/lib/boot.fasl"

total=$((total + 1))
# Run with a timeout — the regression symptom is "100% CPU forever then
# crash", so a hung amilisp must not hang the test suite.
out=$(cd "$WORK" && \
      ( echo '(format t "~&BOOT-OK ~a~%" (+ 1 2)) (quit)' \
        | "$AMILISP_ABS" --no-userinit --batch 2>&1 ) &
      pid=$!
      ( sleep 30 && kill -9 $pid 2>/dev/null ) &
      killer=$!
      wait $pid
      ec=$?
      kill $killer 2>/dev/null
      exit $ec)
ec=$?

if [ $ec -ne 0 ]; then
    echo "  FAIL  boot_fasl_stale_recovery (exit=$ec)"
    echo "    output: $(echo "$out" | head -10)"
    failed=$((failed + 1))
elif ! echo "$out" | grep -q "BOOT-OK 3"; then
    echo "  FAIL  boot_fasl_stale_recovery (no BOOT-OK marker)"
    echo "    output: $(echo "$out" | head -10)"
    failed=$((failed + 1))
else
    echo "  ok  boot_fasl_stale_recovery"
    passed=$((passed + 1))
fi

echo ""
echo "$passed passed, $failed failed, $total total"
if [ "$failed" -gt 0 ]; then
    echo "FAIL"
    exit 1
fi
echo "PASS"
exit 0
