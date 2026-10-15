#!/bin/sh
# Regression test: COMPILE-FILE must not write to the C-level stderr.
#
# bi_compile_file used to fprintf(stderr, "; Compiling ..." / "; Done
# compiling ...") unconditionally — leftover debug output duplicating the
# *COMPILE-VERBOSE*-gated stdout messages.  Besides being noise, it was a
# hang: on MorphOS (-noixemul/libnix) a detached process (Run >NIL:, or an
# agent-spawned child) has no valid console for stderr; the fprintf data
# sits in the stream buffer until main()'s exit-path fflush(NULL), whose
# Write to the dead console handle then blocks forever — the process
# completed all work but never exited.  Diagnostics from the runtime must go
# through the Lisp stream layer (stdout / *ERROR-OUTPUT*), never raw stderr.
#
# Run: sh tests/test_compile_file_stderr.sh [path-to-amilisp]

AMILISP="${1:-build/host/amilisp}"
TMP="${TMPDIR:-/tmp}/amilisp-cfstderr-$$"
mkdir -p "$TMP"
cleanup() { rm -rf "$TMP" 2>/dev/null; }
trap cleanup EXIT

printf '(defparameter *cfstderr-probe* 42)\n' > "$TMP/src.lisp"

failed=0

# Case 1: default (non-verbose) compile-file — stderr must stay empty.
"$AMILISP" --no-userinit --non-interactive \
    --eval "(compile-file \"$TMP/src.lisp\" :output-file \"$TMP/src.fasl\" :verbose nil)" \
    >"$TMP/out1.log" 2>"$TMP/err1.log"
if [ -s "$TMP/err1.log" ]; then
    echo "FAIL: non-verbose compile-file wrote to stderr:"
    cat "$TMP/err1.log"
    failed=1
fi

# Case 2: verbose compile-file — messages belong on stdout, stderr stays empty.
"$AMILISP" --no-userinit --non-interactive \
    --eval "(compile-file \"$TMP/src.lisp\" :output-file \"$TMP/src2.fasl\" :verbose t)" \
    >"$TMP/out2.log" 2>"$TMP/err2.log"
if [ -s "$TMP/err2.log" ]; then
    echo "FAIL: verbose compile-file wrote to stderr:"
    cat "$TMP/err2.log"
    failed=1
fi
if ! grep -q "; Compiling" "$TMP/out2.log"; then
    echo "FAIL: verbose compile-file did not announce on stdout"
    failed=1
fi

# Sanity: the compiles actually produced FASLs.
if [ ! -s "$TMP/src.fasl" ] || [ ! -s "$TMP/src2.fasl" ]; then
    echo "FAIL: compile-file produced no FASL output"
    failed=1
fi

exit $failed
