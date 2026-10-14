#!/bin/sh
# EXT:BACKTRACE in a top-level form that follows a form whose error was
# handled.  Every error snapshots its frame depth (cl_debug_base_fp, for the
# debugger to inspect the error-time stack); a loader that does not drop the
# snapshot between top-level forms leaves it stale, and a later
# (ext:backtrace) then reports only the frames below that old depth -- the
# innermost ones are cut off.  The --load loop reset it; LOAD of a source
# file, LOAD from a stream and LOAD of a FASL did not (found when a JIT
# shadow-frame check in tests/amiga/test-jit.lisp, which the suite loads
# through the FASL cache, lost its innermost frame).
# Run: sh tests/test_backtrace_after_handled_error.sh [path-to-amilisp]

CLAMIGA="${1:-build/host/amilisp}"
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
            echo "    got: $(echo "$haystack" | head -12)"
            failed=$((failed + 1)) ;;
    esac
}

WORK="${TMPDIR:-/tmp}/bt_after_err_$$"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

# Each kind of error is handled at a shallow depth; the backtrace after it
# runs three calls deeper and must still start at its own innermost frame.
cat > "$WORK/bt.lisp" <<'EOF'
(defun bt-leaf () (ext:backtrace))
(defun bt-mid () (let ((r (bt-leaf))) r))
(defun bt-top () (let ((r (bt-mid))) r))
(defun bt-names () (let ((n (mapcar #'second (bt-top)))) (list (first n) (second n) (third n))))
(defun bt-undef-caller () (no-such-function-for-bt-test))
(defun bt-one (a) a)
(format t "BEFORE ~S~%" (bt-names))
(handler-case (bt-undef-caller) (error () nil))
(format t "AFTER-UNDEFINED ~S~%" (bt-names))
(handler-case (funcall #'bt-one) (error () nil))
(format t "AFTER-ARITY ~S~%" (bt-names))
(handler-case (error "plain") (error () nil))
(format t "AFTER-ERROR ~S~%" (bt-names))
EOF

EXPECT_NAMES="(BT-LEAF BT-MID BT-TOP)"

leg() {
    tag="$1"
    out="$2"
    check_contains "$tag: before any error" "BEFORE $EXPECT_NAMES" "$out"
    check_contains "$tag: after a handled UNDEFINED-FUNCTION" "AFTER-UNDEFINED $EXPECT_NAMES" "$out"
    check_contains "$tag: after a handled arity error" "AFTER-ARITY $EXPECT_NAMES" "$out"
    check_contains "$tag: after a handled ERROR" "AFTER-ERROR $EXPECT_NAMES" "$out"
}

out=$("$CLAMIGA" --no-userinit --non-interactive --load "$WORK/bt.lisp" </dev/null 2>&1)
leg "--load" "$out"

echo "(load \"$WORK/bt.lisp\")" > "$WORK/drv-src.lisp"
out=$(CLAMIGA_FASL_CACHE=0 "$CLAMIGA" --no-userinit --non-interactive --load "$WORK/drv-src.lisp" </dev/null 2>&1)
leg "LOAD source" "$out"

cat > "$WORK/drv-stream.lisp" <<EOF
(with-open-file (s "$WORK/bt.lisp") (load s))
EOF
out=$("$CLAMIGA" --no-userinit --non-interactive --load "$WORK/drv-stream.lisp" </dev/null 2>&1)
leg "LOAD stream" "$out"

cat > "$WORK/drv-fasl.lisp" <<EOF
(compile-file "$WORK/bt.lisp" :output-file "$WORK/bt.fasl")
(load "$WORK/bt.fasl")
EOF
out=$("$CLAMIGA" --no-userinit --non-interactive --load "$WORK/drv-fasl.lisp" </dev/null 2>&1)
leg "LOAD FASL" "$out"

echo "$passed passed, $failed failed, $total total"
[ "$failed" -eq 0 ]
