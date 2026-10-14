#!/bin/sh
# The program's own arguments: `--` ends amilisp's options and what follows
# is EXT:*COMMAND-LINE-ARGS* -- a list of strings, never loaded, whatever it
# looks like.  Before it, a bare argument is still a --load.
#
# Why: an application (the Clamacs editor is the first) started as
# `amilisp --image app.img -- file1 file2` must get its files without
# amilisp trying to LOAD them.
#
# Covered:
#   after --: every argument verbatim, in order, including ones that look
#     like options, empty strings and blanks
#   no --, or -- with nothing after it: NIL
#   a bare file before -- is loaded, the same file after -- is not
#   the variables are set BEFORE ~/.amilisprc and EXT:*RESTORE-HOOKS* run,
#     and a restored image reports the RESTORING process's arguments, not
#     the saver's (the image carries the saver's value; the restore replaces it)
#   the same in --script mode and in --batch mode
#   EXT:*WORKBENCH-STARTED-P* is NIL from a command line
#   both are EXT exports (no reader error on the qualified names)
#
# Run: sh tests/test_command_line_args.sh [path-to-amilisp]

AMILISP="${1:-build/host/amilisp}"
case "$AMILISP" in
    /*|[A-Za-z]:/*) ;;
    *) AMILISP="$(pwd)/$AMILISP" ;;
esac

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)
if [ -z "$TIMEOUT" ]; then
    echo "SKIP test_command_line_args: neither timeout nor gtimeout on PATH"
    exit 0
fi

passed=0
failed=0

WORK=$(mktemp -d "${TMPDIR:-/tmp}/clamiga_args_XXXXXX") || exit 1
trap 'rm -rf "$WORK"' EXIT INT TERM
cd "$WORK" || exit 1

CLI="--no-userinit --no-image"

check() {
    desc="$1"; expected="$2"; actual="$3"
    if [ "$actual" = "$expected" ]; then
        echo "  ok  $desc"; passed=$((passed + 1))
    else
        echo "  FAIL  $desc"
        echo "    expected: $expected"
        echo "    got:      $actual"
        failed=$((failed + 1))
    fi
}

run() {
    "$TIMEOUT" 30 "$AMILISP" "$@" </dev/null 2>&1 | tr -d '\r'
}

# one line, single blanks, no leading/trailing blank
norm() {
    tr '\n' ' ' | sed 's/  */ /g; s/^ //; s/ $//'
}

PRINT='(progn (print ext:*command-line-args*) (print ext:*workbench-started-p*))'

# --- after --: verbatim, in order ------------------------------------------
out=$(run $CLI --non-interactive --eval "$PRINT" -- a "b c" --load --heap "" x)
check "arguments after -- are the list, verbatim" \
      '("a" "b c" "--load" "--heap" "" "x") NIL' "$(printf '%s' "$out" | norm)"

# --- none ------------------------------------------------------------------
out=$(run $CLI --non-interactive --eval '(print ext:*command-line-args*)')
check "no -- gives NIL" "NIL" "$(printf '%s' "$out" | tr -d ' \n')"
out=$(run $CLI --non-interactive --eval '(print ext:*command-line-args*)' --)
check "-- with nothing after it gives NIL" "NIL" "$(printf '%s' "$out" | tr -d ' \n')"

# --- loaded before, not after ---------------------------------------------
cat > marker.lisp <<'EOF'
(format t "MARKER-LOADED~%")
EOF
out=$(run $CLI --non-interactive marker.lisp --eval '(print (length ext:*command-line-args*))')
check "a bare file before -- is loaded" "MARKER-LOADED 0" "$(printf '%s' "$out" | tr '\n' ' ' | sed 's/  */ /g; s/ $//')"
out=$(run $CLI --non-interactive --eval '(print ext:*command-line-args*)' -- marker.lisp)
check "the same file after -- is not loaded" '("marker.lisp")' "$(printf '%s' "$out" | norm)"
case "$out" in *MARKER-LOADED*) check "no MARKER-LOADED after --" "" "loaded";; esac

# --- script and batch modes ------------------------------------------------
cat > script.lisp <<'EOF'
(format t "SCRIPT-ARGS ~S~%" ext:*command-line-args*)
EOF
out=$(run $CLI --script script.lisp -- one two)
check "--script sees the arguments" 'SCRIPT-ARGS ("one" "two")' "$(printf '%s' "$out" | grep SCRIPT-ARGS)"
out=$(printf '(format t "BATCH-ARGS ~S~%%" ext:*command-line-args*)\n' | "$TIMEOUT" 30 "$AMILISP" $CLI --batch -- three 2>&1 | tr -d '\r' | grep BATCH-ARGS)
check "--batch sees the arguments" 'BATCH-ARGS ("three")' "$out"

# --- set before the user init file runs ------------------------------------
mkdir -p home
cat > home/.amilisprc <<'EOF'
(format t "RC-ARGS ~S RC-WB ~S~%" ext:*command-line-args* ext:*workbench-started-p*)
EOF
# (make test exports AMILISP_NO_USERINIT so no test reads the developer's
# rc file; this one wants its own read, so clear it as test_userinit.sh does)
out=$(HOME="$WORK/home" AMILISP_NO_USERINIT= "$TIMEOUT" 30 "$AMILISP" --no-image --non-interactive -- rc-arg </dev/null 2>&1 | tr -d '\r' | grep RC-ARGS)
check "~/.amilisprc already sees them" 'RC-ARGS ("rc-arg") RC-WB NIL' "$out"

# --- image: the restoring process's arguments, before the restore hooks ----
cat > save.lisp <<'EOF'
(push (lambda () (format t "HOOK-ARGS ~S~%" ext:*command-line-args*)) ext:*restore-hooks*)
(defvar *saved-args* ext:*command-line-args*)
(ext:save-image "args.img" :quit t)
EOF
out=$(run $CLI --non-interactive --load save.lisp -- saver-arg)
[ -f args.img ] || { echo "  FAIL  image not written:"; echo "$out"; failed=$((failed + 1)); }
out=$(run --no-userinit --image args.img --non-interactive \
          --eval '(format t "NOW-ARGS ~S SAVED ~S~%" ext:*command-line-args* *saved-args*)' -- restorer-arg "and two")
check "a restore hook sees the restoring process's arguments" \
      'HOOK-ARGS ("restorer-arg" "and two")' "$(printf '%s' "$out" | grep HOOK-ARGS)"
check "after the restore: the new list, the saver's own value untouched" \
      'NOW-ARGS ("restorer-arg" "and two") SAVED ("saver-arg")' "$(printf '%s' "$out" | grep NOW-ARGS)"
out=$(run --no-userinit --image args.img --non-interactive)
check "a restore without -- publishes NIL over the saver's value" \
      'HOOK-ARGS NIL' "$(printf '%s' "$out" | grep HOOK-ARGS)"

# --- exports ----------------------------------------------------------------
out=$(run $CLI --non-interactive --eval '(print (list (symbol-package (quote ext:*command-line-args*)) (symbol-package (quote ext:*workbench-started-p*))))')
check "both names are EXT externals" '(#<PACKAGE EXT> #<PACKAGE EXT>)' "$(printf '%s' "$out" | norm)"

echo ""
echo "test_command_line_args: $passed passed, $failed failed"
[ "$failed" -eq 0 ]
