#!/bin/sh
# Source names recorded in portable FASLs.
#
# Every function's FASL unit carries CL_Bytecode.source_file, and that is
# the name a backtrace, the debugger and EXT:FUNCTION-SOURCE-LOCATION show.
# COMPILE-FILE merges its input with *default-pathname-defaults*, so the
# recorded name was the build host's absolute path — and the binary release
# (compiled on a Mac, run on an Amiga) put
# "/Users/<builder>/Development/.../amilisp/lib/amiga/ahi.lisp" into every
# Amiga backtrace through a shipped module.
#
# With AMILISP_FASL_PORTABLE=1 (what the release script, `make fasl` and
# scripts/compile-lib-fasls.sh set) the recorded name is now relative to the
# compiling process's current directory when the source lies under it:
# "lib/amiga/ahi.lisp", which is also where the file sits in the deployed
# tree.  Off, or for a source outside the cwd, the absolute path stays (the
# Sly M-. backend on the host needs it).
#
#   1. portable, source under cwd  -> relative name in the loaded FASL's
#      backtrace and in EXT:FUNCTION-SOURCE-LOCATION
#   2. portable, source outside cwd -> absolute name kept
#   3. not portable, same source   -> absolute name kept
#
# Run: sh tests/test_fasl_source_name.sh build/host/amilisp

AMILISP="${1:-build/host/amilisp}"
case "$AMILISP" in /*|[A-Za-z]:/*) ;; *) AMILISP="$(pwd)/$AMILISP" ;; esac

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)
if [ -z "$TIMEOUT" ]; then
    echo "SKIP test_fasl_source_name: neither timeout nor gtimeout on PATH"
    exit 0
fi

dir=$(mktemp -d "${TMPDIR:-/tmp}/amilisp_srcname_XXXXXX") || exit 1
trap 'rm -rf "$dir"' EXIT
# The tree the compile runs in, and a source outside it.
mkdir -p "$dir/tree/lib/mod" "$dir/elsewhere"

cat > "$dir/tree/lib/mod/thing.lisp" <<'EOF'
(defun srcname-fn (x) (car x))
EOF
cp "$dir/tree/lib/mod/thing.lisp" "$dir/elsewhere/thing.lisp"

# Loads a FASL and prints the recorded source name two ways: the
# source-location primitive, and a backtrace through the function.
driver() {
    cat <<EOF
(load "$1")
(format t "SRCLOC ~a~%" (first (ext:function-source-location #'srcname-fn)))
(srcname-fn 5)
EOF
}

fail=0
check() {
    # $1 description, $2 grep pattern, $3 output file
    if grep -q "$2" "$3"; then
        echo "  ok    $1"
    else
        echo "  FAIL  $1"
        echo "        expected a line matching: $2"
        sed 's/^/        | /' "$3"
        fail=1
    fi
}
check_absent() {
    if grep -q "$2" "$3"; then
        echo "  FAIL  $1"
        echo "        found a line matching: $2"
        sed 's/^/        | /' "$3"
        fail=1
    else
        echo "  ok    $1"
    fi
}

compile_in_tree() {
    # $1 = source (relative to the tree or absolute), $2 = fasl, $3 = env
    ( cd "$dir/tree" && env $3 "$TIMEOUT" 60 "$AMILISP" --no-userinit --non-interactive \
        --eval "(compile-file \"$1\" :output-file \"$2\")" </dev/null >/dev/null 2>&1 )
    [ -s "$2" ]
}

# 1. portable, under cwd: relative
compile_in_tree "lib/mod/thing.lisp" "$dir/f1.fasl" "AMILISP_FASL_PORTABLE=1" \
    || { echo "FAIL: compile 1 produced no FASL"; exit 1; }
driver "$dir/f1.fasl" > "$dir/d1.lisp"
( cd "$dir/elsewhere" && "$TIMEOUT" 60 "$AMILISP" --no-userinit --non-interactive \
    --load "$dir/d1.lisp" </dev/null > "$dir/o1.txt" 2>&1 )
check "portable: source-location is relative to the compile cwd" \
      "^SRCLOC lib/mod/thing.lisp$" "$dir/o1.txt"
check "portable: backtrace frame names the relative source" \
      "SRCNAME-FN (lib/mod/thing.lisp:1)" "$dir/o1.txt"
check_absent "portable: no build-host absolute path in the backtrace" \
      "SRCNAME-FN ($dir" "$dir/o1.txt"

# 2. portable, outside cwd: absolute kept
compile_in_tree "$dir/elsewhere/thing.lisp" "$dir/f2.fasl" "AMILISP_FASL_PORTABLE=1" \
    || { echo "FAIL: compile 2 produced no FASL"; exit 1; }
driver "$dir/f2.fasl" > "$dir/d2.lisp"
"$TIMEOUT" 60 "$AMILISP" --no-userinit --non-interactive \
    --load "$dir/d2.lisp" </dev/null > "$dir/o2.txt" 2>&1
check "portable, source outside cwd: absolute name kept" \
      "^SRCLOC .*/elsewhere/thing.lisp$" "$dir/o2.txt"

# 3. not portable, under cwd: absolute kept (the host's M-. contract)
compile_in_tree "lib/mod/thing.lisp" "$dir/f3.fasl" "AMILISP_FASL_PORTABLE=0" \
    || { echo "FAIL: compile 3 produced no FASL"; exit 1; }
driver "$dir/f3.fasl" > "$dir/d3.lisp"
"$TIMEOUT" 60 "$AMILISP" --no-userinit --non-interactive \
    --load "$dir/d3.lisp" </dev/null > "$dir/o3.txt" 2>&1
check "not portable: absolute name kept" \
      "^SRCLOC .*/tree/lib/mod/thing.lisp$" "$dir/o3.txt"

if [ "$fail" -eq 0 ]; then
    echo "test_fasl_source_name: all passed"
fi
exit $fail
