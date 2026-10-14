#!/bin/sh
# End-to-end tests for EXT:*EXIT-HOOKS* — hooks must run on EVERY way amilisp
# leaves the process, because (QUIT) unwinds via CL_ERR_EXIT and deliberately
# skips UNWIND-PROTECT cleanups: exit hooks are the only place user code gets
# to observe shutdown.
#
# Covered exit paths (all four reach the `shutdown:` funnel in main.c):
#   --non-interactive falling off the end of its --eval/--load actions
#   (QUIT n) from a --eval, with the exit code preserved
#   --script running to the end of the file
#   the batch REPL hitting EOF on stdin
#
# Plus the properties that make hooks usable at shutdown: they run
# most-recently-added first, streams still work (a hook can write a file), and
# a hook that errors is reported without taking the process or the remaining
# hooks down with it.
#
# The in-process unit tests are tests/test_exit_hooks.c.
#
# Run: sh tests/test_exit_hooks.sh build/host/amilisp

CLAMIGA="${1:-build/host/amilisp}"

# Every case is also a "does it hang at exit?" check, so a timeout binary is a
# prerequisite (macOS ships it as gtimeout via coreutils).
TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)
if [ -z "$TIMEOUT" ]; then
    echo "SKIP test_exit_hooks: neither timeout nor gtimeout on PATH"
    exit 0
fi

passed=0
failed=0
total=0

# A path embedded in a generated Lisp form (below) must be spelled the way
# amilisp reads it: on Windows $TMPDIR is "C:\Users\..." and the reader would
# eat the backslashes as escapes.  Shell-side uses of $WORK are fine either way.
. "$(dirname "$0")/shpath.sh"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/clamiga_exit_hooks_XXXXXX") || exit 1
trap 'rm -rf "$WORK"' EXIT INT TERM
WORK_LISP=$(native_path "$WORK")

CLI="--no-userinit --heap 8M"

fail() {
    desc="$1"; why="$2"; out="$3"
    failed=$((failed + 1))
    echo "  FAIL  $desc ($why)"
    echo "    output: $(echo "$out" | head -8)"
}

ok() { passed=$((passed + 1)); echo "  ok  $desc"; }

# desc, expected-exit-code, actual-exit-code, output, marker...
# Asserts: no timeout, the expected code, and every marker present in order.
check() {
    desc="$1"; want_ec="$2"; ec="$3"; out="$4"
    shift 4
    total=$((total + 1))
    if [ "$ec" -eq 124 ]; then
        fail "$desc" "timed out — amilisp hung in the exit hooks" "$out"
        return
    fi
    if [ "$ec" -ne "$want_ec" ]; then
        fail "$desc" "exit code $ec, wanted $want_ec" "$out"
        return
    fi
    for marker in "$@"; do
        if ! echo "$out" | grep -q "$marker"; then
            fail "$desc" "missing marker $marker" "$out"
            return
        fi
    done
    ok
}

# --- Exit path 1: --non-interactive falls off the end of its actions ---
# Also pins the ordering: ADD-EXIT-HOOK pushes, so B (added last) runs first.
out=$("$TIMEOUT" 20 "$CLAMIGA" $CLI --non-interactive \
    --eval '(ext:add-exit-hook (lambda () (format t "HOOK-A~%")))' \
    --eval '(ext:add-exit-hook (lambda () (format t "HOOK-B~%")))' \
    --eval '(format t "BODY-DONE~%")' </dev/null 2>&1)
ec=$?
check "non_interactive_runs_hooks_at_exit" 0 "$ec" "$out" BODY-DONE HOOK-B HOOK-A
total=$((total + 1))
desc="hooks_run_most_recently_added_first"
if echo "$out" | tr '\n' ' ' | grep -q "BODY-DONE HOOK-B HOOK-A"; then
    ok
else
    fail "$desc" "wrong order" "$out"
fi

# --- Exit path 2: (QUIT n) — hooks run, and the exit code survives them ---
out=$("$TIMEOUT" 20 "$CLAMIGA" $CLI --non-interactive \
    --eval '(ext:add-exit-hook (lambda () (format t "QUIT-HOOK~%")))' \
    --eval '(quit 3)' </dev/null 2>&1)
check "quit_runs_hooks_and_keeps_exit_code" 3 "$?" "$out" QUIT-HOOK

# --- Exit path 3: --script running to the end of the file ---
cat > "$WORK/script.lisp" <<'EOF'
(ext:add-exit-hook (lambda () (format t "SCRIPT-HOOK~%")))
(format t "SCRIPT-BODY~%")
EOF
out=$("$TIMEOUT" 20 "$CLAMIGA" $CLI --script "$WORK/script.lisp" </dev/null 2>&1)
check "script_mode_runs_hooks_at_exit" 0 "$?" "$out" SCRIPT-BODY SCRIPT-HOOK

# --- Exit path 4: batch REPL reaching EOF on stdin ---
out=$(printf '(ext:add-exit-hook (lambda () (format t "EOF-HOOK~%%")))\n' \
      | "$TIMEOUT" 20 "$CLAMIGA" $CLI --batch 2>&1)
check "batch_repl_eof_runs_hooks" 0 "$?" "$out" EOF-HOOK

# --- Exit path 4b: the interactive REPL (cl_repl, not cl_repl_batch) on EOF ---
out=$(printf '(ext:add-exit-hook (lambda () (format t "REPL-HOOK~%%")))\n' \
      | "$TIMEOUT" 20 "$CLAMIGA" $CLI 2>&1)
check "interactive_repl_eof_runs_hooks" 0 "$?" "$out" "Bye." REPL-HOOK

# --- Hooks run while the streams are still alive: a hook can write a file ---
# This is the whole point of running them at the head of the shutdown funnel,
# before cl_stream_shutdown().
rm -f "$WORK/hook-out.txt"
out=$("$TIMEOUT" 20 "$CLAMIGA" $CLI --non-interactive \
    --eval "(ext:add-exit-hook
              (lambda ()
                (with-open-file (s \"$WORK_LISP/hook-out.txt\"
                                   :direction :output :if-exists :supersede)
                  (format s \"written-at-exit~%\"))))" \
    --eval '(quit 0)' </dev/null 2>&1)
ec=$?
total=$((total + 1))
desc="hook_can_still_do_file_io_at_shutdown"
if [ "$ec" -ne 0 ]; then
    fail "$desc" "exit code $ec" "$out"
elif [ ! -f "$WORK/hook-out.txt" ]; then
    fail "$desc" "hook did not create its file" "$out"
elif ! grep -q "written-at-exit" "$WORK/hook-out.txt"; then
    fail "$desc" "file content wrong: $(cat "$WORK/hook-out.txt")" "$out"
else
    ok
fi

# --- An erroring hook is reported, and the rest still run ---
out=$("$TIMEOUT" 20 "$CLAMIGA" $CLI --non-interactive \
    --eval '(ext:add-exit-hook (lambda () (format t "SURVIVOR~%")))' \
    --eval '(ext:add-exit-hook (lambda () (error "hook blew up")))' \
    --eval '(ext:add-exit-hook (quote no-such-function-at-all))' \
    --eval '(quit 0)' </dev/null 2>&1)
check "erroring_hook_reported_and_others_still_run" 0 "$?" "$out" \
    "SURVIVOR" "error in exit hook" "hook blew up" \
    "Undefined function: NO-SUCH-FUNCTION-AT-ALL"

# --- A hook that quits ends the sequence; its exit code wins ---
out=$("$TIMEOUT" 20 "$CLAMIGA" $CLI --non-interactive \
    --eval '(ext:add-exit-hook (lambda () (format t "NOT-REACHED~%")))' \
    --eval '(ext:add-exit-hook (lambda () (format t "QUITTER~%") (quit 5)))' \
    --eval '(quit 0)' </dev/null 2>&1)
ec=$?
check "hook_calling_quit_stops_the_sequence" 5 "$ec" "$out" QUITTER
total=$((total + 1))
desc="hook_quit_skips_remaining_hooks"
if echo "$out" | grep -q "NOT-REACHED"; then
    fail "$desc" "hooks after the quitting one still ran" "$out"
else
    ok
fi

# --- No hooks registered: shutdown is unchanged (and does not hang) ---
out=$("$TIMEOUT" 20 "$CLAMIGA" $CLI --non-interactive \
    --eval '(format t "NO-HOOKS~%")' </dev/null 2>&1)
check "no_hooks_registered_exits_cleanly" 0 "$?" "$out" NO-HOOKS

echo ""
echo "$passed passed, $failed failed, $total total"
if [ "$failed" -gt 0 ]; then
    echo "FAIL"
    exit 1
fi
echo "PASS"
exit 0
