#!/bin/sh
# Ctrl-D at the Debug> prompt exits the DEBUGGER, not the REPL (issue #13).
#
# SBCL semantics: EOF at the debugger prompt aborts to top level and the
# session keeps running; a second Ctrl-D at the top-level prompt exits.
# amilisp used to unwind to top level correctly but leave stdio's sticky
# EOF flag set, so the REPL's very next fgets returned EOF and the session
# died ("Bye.") anyway — the debugger now clears the flag
# (platform_clear_stdin_eof) and jumps to top level like :q.
#
# The interactive debugger only engages when stdin is a real tty, so this
# needs a PTY — driven with expect(1), and skipped where it isn't installed
# (Linux CI images, MSYS2).
#
# Run: sh tests/test_debugger_eof.sh build/host/amilisp

CLAMIGA="${1:-build/host/amilisp}"

EXPECT=$(command -v expect 2>/dev/null || true)
if [ -z "$EXPECT" ]; then
    echo "SKIP test_debugger_eof: expect(1) not on PATH (PTY needed)"
    exit 0
fi

script=$(mktemp "${TMPDIR:-/tmp}/clamiga_dbgeof_XXXXXX") || exit 1
trap 'rm -f "$script"' EXIT

cat > "$script" <<'EOF'
set timeout 30
spawn [lindex $argv 0] --no-userinit --no-color
expect {
    "COMMON-LISP-USER>" {}
    timeout { puts "TESTFAIL: no REPL prompt"; exit 1 }
}
send "(error \"eof test\")\r"
expect {
    "Debug>" {}
    timeout { puts "TESTFAIL: no Debug> prompt"; exit 1 }
}
# Ctrl-D: must abort to top level, NOT exit the session
send "\x04"
expect {
    "COMMON-LISP-USER>" {}
    eof { puts "TESTFAIL: Ctrl-D in debugger exited the REPL"; exit 1 }
    timeout { puts "TESTFAIL: no prompt after Ctrl-D in debugger"; exit 1 }
}
# The REPL must still be alive and evaluating
send "(+ 40 2)\r"
expect {
    "42" {}
    eof { puts "TESTFAIL: REPL dead after debugger EOF"; exit 1 }
    timeout { puts "TESTFAIL: REPL unresponsive after debugger EOF"; exit 1 }
}
# A second Ctrl-D at the top-level prompt still exits
send "\x04"
expect {
    eof { puts "TESTPASS"; exit 0 }
    timeout { puts "TESTFAIL: Ctrl-D at top level no longer exits"; exit 1 }
}
EOF

out=$("$EXPECT" -f "$script" "$CLAMIGA" 2>&1)
case "$out" in
    *TESTPASS*)
        echo "test_debugger_eof: 1 passed, 0 failed"
        echo PASS
        exit 0
        ;;
    *)
        echo "test_debugger_eof: FAILED"
        echo "$out" | tail -20
        exit 1
        ;;
esac
