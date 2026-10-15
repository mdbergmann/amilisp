#!/bin/sh
# Ctrl-D at the Inspect> prompt exits the INSPECTOR, not the REPL (issue #25).
#
# Same contract as the debugger prompt (issue #13, tests/test_debugger_eof.sh):
# EOF leaves the interactive loop and hands control back — for INSPECT that
# means returning from the call like `q` does — and the session keeps running;
# a second Ctrl-D at the top-level prompt exits.  The inspector loop already
# fell out on EOF, but it left stdio's sticky EOF flag set, so the REPL's very
# next fgets returned EOF and the session died ("Bye.") anyway — the inspector
# now clears the flag (platform_clear_stdin_eof) before returning.
#
# The inspector prompt only makes sense at a real tty, so this needs a PTY —
# driven with expect(1), and skipped where it isn't installed (Linux CI
# images, MSYS2).
#
# Run: sh tests/test_inspect_eof.sh build/host/amilisp

AMILISP="${1:-build/host/amilisp}"

EXPECT=$(command -v expect 2>/dev/null || true)
if [ -z "$EXPECT" ]; then
    echo "SKIP test_inspect_eof: expect(1) not on PATH (PTY needed)"
    exit 0
fi

script=$(mktemp "${TMPDIR:-/tmp}/amilisp_inspeof_XXXXXX") || exit 1
trap 'rm -f "$script"' EXIT

cat > "$script" <<'EOF'
set timeout 30
spawn [lindex $argv 0] --no-userinit --no-color
expect {
    "COMMON-LISP-USER>" {}
    timeout { puts "TESTFAIL: no REPL prompt"; exit 1 }
}
send "(inspect (list 1 2 3))\r"
expect {
    "Inspect>" {}
    timeout { puts "TESTFAIL: no Inspect> prompt"; exit 1 }
}
# Ctrl-D: must leave the inspector, NOT exit the session
send "\x04"
expect {
    "COMMON-LISP-USER>" {}
    eof { puts "TESTFAIL: Ctrl-D in inspector exited the REPL"; exit 1 }
    timeout { puts "TESTFAIL: no prompt after Ctrl-D in inspector"; exit 1 }
}
# The REPL must still be alive and evaluating
send "(+ 40 2)\r"
expect {
    "42" {}
    eof { puts "TESTFAIL: REPL dead after inspector EOF"; exit 1 }
    timeout { puts "TESTFAIL: REPL unresponsive after inspector EOF"; exit 1 }
}
# EOF from a nested component must behave the same as from the root
send "(inspect (list 1 2 3))\r"
expect {
    "Inspect>" {}
    timeout { puts "TESTFAIL: no Inspect> prompt (second entry)"; exit 1 }
}
send "1\r"
expect {
    "Cdr" {}
    timeout { puts "TESTFAIL: navigation into component 1 failed"; exit 1 }
}
send "\x04"
expect {
    "COMMON-LISP-USER>" {}
    eof { puts "TESTFAIL: Ctrl-D below the root exited the REPL"; exit 1 }
    timeout { puts "TESTFAIL: no prompt after nested Ctrl-D"; exit 1 }
}
send "(* 6 7)\r"
expect {
    "42" {}
    eof { puts "TESTFAIL: REPL dead after nested inspector EOF"; exit 1 }
    timeout { puts "TESTFAIL: REPL unresponsive after nested inspector EOF"; exit 1 }
}
# A Ctrl-D at the top-level prompt still exits
send "\x04"
expect {
    eof { puts "TESTPASS"; exit 0 }
    timeout { puts "TESTFAIL: Ctrl-D at top level no longer exits"; exit 1 }
}
EOF

out=$("$EXPECT" -f "$script" "$AMILISP" 2>&1)
case "$out" in
    *TESTPASS*)
        echo "test_inspect_eof: 1 passed, 0 failed"
        echo PASS
        exit 0
        ;;
    *)
        echo "test_inspect_eof: FAILED"
        echo "$out" | tail -20
        exit 1
        ;;
esac
