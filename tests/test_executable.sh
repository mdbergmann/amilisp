#!/bin/sh
# End-to-end tests for delivered executables:
#   (ext:save-image "app" :executable t :toplevel 'main [:heap-size n])
# writes a copy of the running amilisp with the image appended — one file
# that starts the program, with no lib/, no image file and no amilisp
# options beside it.
#
# Covered:
#   the file is executable and runs from a directory with nothing else in it
#   the whole command line is the program's (EXT:*COMMAND-LINE-ARGS*,
#     verbatim: --help, --heap, -- are arguments like any other)
#   no banner, no ~/.amilisprc, EXT:*IMAGE-RESTORED-P* = T, restore hooks
#     run before the :TOPLEVEL function, exit hooks after it
#   exit status: 0 when :TOPLEVEL returns, n after (quit n), 1 after an
#     error nothing handled (and the error is printed)
#   :HEAP-SIZE is the arena the program starts with
#   the embedded image wins over a amilisp.img lying in the cwd
#   started through a symlink (POSIX)
#   without :TOPLEVEL the executable is a REPL with the program loaded
#   a delivered executable can save the next one (the runtime is copied
#     without the image it carries)
#   build-time errors: :TOPLEVEL / :HEAP-SIZE without :EXECUTABLE, an
#     undefined :TOPLEVEL function, a bad :HEAP-SIZE
#   a damaged executable refuses to start (exit 1) instead of becoming a REPL
#
# Run: sh tests/test_executable.sh build/host/amilisp

CLAMIGA="${1:-build/host/amilisp}"
case "$CLAMIGA" in
    /*|[A-Za-z]:/*) : ;;
    *) CLAMIGA="$(pwd)/$CLAMIGA" ;;
esac
# A Windows program is found by its extension.
case "$CLAMIGA" in
    *.exe) X=.exe ;;
    *) X= ;;
esac

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)
if [ -z "$TIMEOUT" ]; then
    echo "SKIP test_executable: neither timeout nor gtimeout on PATH"
    exit 0
fi

passed=0
failed=0

WORK=$(mktemp -d "${TMPDIR:-/tmp}/clamiga_exe_XXXXXX") || exit 1
trap 'rm -rf "$WORK"' EXIT INT TERM
cd "$WORK" || exit 1
mkdir away home

CLI="--no-userinit --no-image --non-interactive"
unset CLAMIGA_HOME

fail() {
    desc="$1"; why="$2"; out="$3"
    failed=$((failed + 1))
    echo "  FAIL  $desc ($why)"
    echo "    output: $(echo "$out" | head -8)"
}

ok() { passed=$((passed + 1)); echo "  ok  $desc"; }

check() {
    desc="$1"; want_ec="$2"; ec="$3"; out="$4"
    shift 4
    if [ "$ec" -eq 124 ]; then
        fail "$desc" "timed out" "$out"
        return 1
    fi
    if [ "$ec" -ne "$want_ec" ]; then
        fail "$desc" "exit code $ec, wanted $want_ec" "$out"
        return 1
    fi
    for marker in "$@"; do
        if ! echo "$out" | grep -q -- "$marker"; then
            fail "$desc" "missing marker $marker" "$out"
            return 1
        fi
    done
    ok
    return 0
}

check_absent() {
    desc="$1"; out="$2"; marker="$3"
    if echo "$out" | grep -q -- "$marker"; then
        fail "$desc" "unexpected marker $marker" "$out"
        return 1
    fi
    ok
    return 0
}

# --- The program ----------------------------------------------------------

cat > app.lisp <<'EOF2'
(defpackage :ex-app (:use :cl) (:export #:main))
(in-package :ex-app)
(defvar *trace* '())
(defclass greeter () ((name :initarg :name :reader greeter-name)))
(defgeneric greet (g))
(defmethod greet ((g greeter)) (format nil "hello ~a" (greeter-name g)))
(defun fib (n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))
(defun main ()
  (push :main *trace*)
  (format t "ARGS=~S~%" ext:*command-line-args*)
  (format t "RESTORED=~S TRACE=~S~%" ext:*image-restored-p* (reverse *trace*))
  (format t "GREET=~a FIB=~a~%" (greet (make-instance 'greeter :name "amiga"))
          (fib 15))
  (let ((cmd (first ext:*command-line-args*)))
    (cond ((equal cmd "quit7") (cl-user::quit 7))
          ((equal cmd "boom") (error "kaputt ~a" 42))
          ((equal cmd "room") (room))
          ((equal cmd "resave")
           (setf *trace* '())
           (ext:save-image (second ext:*command-line-args*)
                           :executable t :toplevel 'main))))
  (format t "MAIN-RETURNS~%"))
(push (lambda () (push :restore-hook *trace*)) ext:*restore-hooks*)
(ext:add-exit-hook (lambda () (format t "EXIT-HOOK-RAN~%")))
EOF2

cat > build.lisp <<EOF2
(load "app.lisp")
(ext:save-image "app$X" :executable t :toplevel 'ex-app:main :quit t)
EOF2

out=$("$TIMEOUT" 60 "$CLAMIGA" $CLI --load build.lisp </dev/null 2>&1); ec=$?
check "save_executable" 0 "$ec" "$out" "Executable saved to"

desc="file_is_executable"
if [ -x "app$X" ]; then ok; else fail "$desc" "no x bit / missing" "$(ls -l)"; fi

# Nothing but the one file, in a directory of its own.
cp "app$X" away/
echo '(format t "RC-RAN~%")' > home/.amilisprc
echo 'not an image' > away/amilisp.img

run_app() {   # run_app ARG... — from away/, output in $out, status in $ec
    out=$(cd away && HOME="$WORK/home" "$TIMEOUT" 30 "./app$X" "$@" </dev/null 2>&1)
    ec=$?
}

run_app --help --heap 1M "a b" -- x
check "runs_alone_and_returns_0" 0 "$ec" "$out" \
    "GREET=hello amiga FIB=610" "MAIN-RETURNS"
check "command_line_is_the_programs_verbatim" 0 "$ec" "$out" \
    'ARGS=("--help" "--heap" "1M" "a b" "--" "x")'
check "restore_hooks_run_before_toplevel" 0 "$ec" "$out" \
    "RESTORED=T TRACE=(:RESTORE-HOOK :MAIN)"
check "exit_hooks_run_after_toplevel" 0 "$ec" "$out" "EXIT-HOOK-RAN"
check_absent "no_banner" "$out" "AmiLisp v"
check_absent "no_user_init_file" "$out" "RC-RAN"
check_absent "no_library_needed" "$out" "cannot locate its runtime library"
check_absent "embedded_image_wins_over_cwd_clamiga_img" "$out" "--image"

run_app
check "no_arguments" 0 "$ec" "$out" "ARGS=NIL"

run_app quit7
check "quit_sets_the_exit_status" 7 "$ec" "$out" "ARGS=(\"quit7\")"
check_absent "quit_skips_the_rest_of_toplevel" "$out" "MAIN-RETURNS"

run_app boom
check "unhandled_error_is_printed_and_exits_1" 1 "$ec" "$out" "kaputt 42"

if [ -z "$X" ]; then
    ln -s "$WORK/away/app" away/link
    out=$(cd "$WORK/home" && "$TIMEOUT" 30 "$WORK/away/link" via-link </dev/null 2>&1); ec=$?
    check "started_through_a_symlink" 0 "$ec" "$out" 'ARGS=("via-link")'
fi

# --- :HEAP-SIZE -----------------------------------------------------------

cat > build-heap.lisp <<EOF2
(load "app.lisp")
(ext:save-image "away/big$X" :executable t :toplevel 'ex-app:main
                :heap-size (* 12 1024 1024) :quit t)
EOF2
out=$("$TIMEOUT" 60 "$CLAMIGA" $CLI --load build-heap.lisp </dev/null 2>&1); ec=$?
check "save_executable_with_heap_size" 0 "$ec" "$out" "Executable saved to"
out=$(cd away && "$TIMEOUT" 30 "./big$X" room </dev/null 2>&1); ec=$?
check "heap_size_is_the_arena_at_start" 0 "$ec" "$out" "/ 12582912 bytes used"

# --- No :TOPLEVEL: a REPL with the program loaded ---------------------------

cat > build-repl.lisp <<EOF2
(load "app.lisp")
(ext:save-image "away/repl$X" :executable t :quit t)
EOF2
out=$("$TIMEOUT" 60 "$CLAMIGA" $CLI --load build-repl.lisp </dev/null 2>&1); ec=$?
check "save_executable_without_toplevel" 0 "$ec" "$out" "Executable saved to"
out=$(cd away && echo '(format t "REPL-SAYS ~a ~s~%" (ex-app::fib 10) ext:*command-line-args*)' |
      "$TIMEOUT" 30 "./repl$X" --no-such-option 2>&1); ec=$?
check "without_toplevel_it_is_a_repl" 0 "$ec" "$out" \
    'REPL-SAYS 55 ("--no-such-option")' "AmiLisp v"

# --- A delivered executable saves the next one ------------------------------

run_app resave "gen2$X"
check "delivered_executable_saves_another" 0 "$ec" "$out" "Executable saved to"
out=$(cd away && "$TIMEOUT" 30 "./gen2$X" second </dev/null 2>&1); ec=$?
check "second_generation_runs" 0 "$ec" "$out" \
    'ARGS=("second")' "TRACE=(:RESTORE-HOOK :MAIN)" "FIB=610"
desc="runtime_is_copied_without_the_old_image"
s1=$(wc -c < "away/app$X"); s2=$(wc -c < "away/gen2$X"); sr=$(wc -c < "$CLAMIGA")
# gen2 = runtime + one image: far below runtime + two images.
if [ "$s2" -lt $((s1 + (s1 - sr) / 2)) ] && [ "$s2" -gt "$sr" ]; then ok
else fail "$desc" "sizes app=$s1 gen2=$s2 runtime=$sr" ""; fi

# --- Build-time errors ------------------------------------------------------

build_err() {   # build_err DESC FORM MARKER
    desc="$1"
    rm -f "bad$X"
    out=$("$TIMEOUT" 60 "$CLAMIGA" $CLI --load app.lisp --eval "$2" </dev/null 2>&1); ec=$?
    if [ -e "bad$X" ]; then fail "$desc" "a file was written" "$out"; return; fi
    check "$desc" 0 "$ec" "$out" "$3"
}
build_err "toplevel_needs_executable" \
    "(ext:save-image \"bad$X\" :toplevel 'ex-app:main :quit t)" "add :EXECUTABLE T"
build_err "heap_size_needs_executable" \
    "(ext:save-image \"bad$X\" :heap-size 8388608 :quit t)" "add :EXECUTABLE T"
build_err "undefined_toplevel_fails_the_build" \
    "(ext:save-image \"bad$X\" :executable t :toplevel 'ex-app::no-such-fn :quit t)" \
    "the :TOPLEVEL function NO-SUCH-FN is not defined"
build_err "toplevel_must_be_a_function_designator" \
    "(ext:save-image \"bad$X\" :executable t :toplevel 42 :quit t)" \
    "SAVE-IMAGE :TOPLEVEL: not a function"
build_err "bad_heap_size" \
    "(ext:save-image \"bad$X\" :executable t :heap-size -1 :quit t)" \
    ":HEAP-SIZE must be a positive"
build_err "heap_size_above_32_bits" \
    "(ext:save-image \"bad$X\" :executable t :heap-size 4294967296 :quit t)" \
    ":HEAP-SIZE must be a positive"

# --- A damaged executable ---------------------------------------------------

# The trailer intact, 100 bytes of the image gone: the offsets no longer
# add up to the file.
size=$(wc -c < "away/app$X")
head -c $((size - 120)) "away/app$X" > "away/cut$X"
tail -c 20 "away/app$X" >> "away/cut$X"
chmod +x "away/cut$X"
out=$(cd away && echo '(format t "IN-REPL~%")' | "$TIMEOUT" 30 "./cut$X" 2>&1); ec=$?
check "damaged_executable_refuses_to_start" 1 "$ec" "$out" "cannot be used"
check_absent "damaged_executable_is_no_repl" "$out" "IN-REPL"

# --- Report -----------------------------------------------------------------

echo "test_executable: $passed passed, $failed failed"
[ "$failed" -eq 0 ]
