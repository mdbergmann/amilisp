#!/bin/sh
# The REPL's continuation prompt is only printed when the REPL is actually
# about to wait for a human (issue #14, second report).
#
# A form typed line by line gets the prompt after each incomplete line:
#
#     COMMON-LISP-USER> "
#                     > "
#     "
#     "
#
# A pasted multi-line form (or one an editor sends whole) arrives in one
# burst; the terminal echoes all of it before the REPL has read the first
# line, so a prompt printed then lands AFTER the echo, glued to the value:
#
#     COMMON-LISP-USER> "
#     "
#                     > "      <- looked like part of the value
#     "
#
# The REPL now asks the platform whether the rest of the form is already
# pending (platform_tty_char_avail) and skips the prompt when it is, so a
# paste reads like it does in SBCL.  A terminal is needed for the echo and
# for the kernel's line-at-a-time delivery — driven with expect(1) on a
# PTY, skipped where expect isn't installed (Linux CI images, MSYS2).
#
# Run: sh tests/test_repl_paste.sh build/host/amilisp

CLAMIGA="${1:-build/host/amilisp}"

EXPECT=$(command -v expect 2>/dev/null || true)
if [ -z "$EXPECT" ]; then
    echo "SKIP test_repl_paste: expect(1) not on PATH (PTY needed)"
    exit 0
fi

script=$(mktemp "${TMPDIR:-/tmp}/clamiga_replpaste_XXXXXX") || exit 1
trap 'rm -f "$script"' EXIT

# The pty echoes what is sent (CR -> CRLF) and the REPL's own newlines come
# out as CRLF too, so every expected transcript below is spelled with \r\n.
# Each pattern runs from the echo of the input through the next top-level
# prompt: a continuation prompt anywhere in between breaks the match.
cat > "$script" <<'EOF'
set timeout 20
spawn [lindex $argv 0] --no-userinit --no-color
expect {
    "COMMON-LISP-USER> " {}
    timeout { puts "TESTFAIL: no REPL prompt"; exit 1 }
}

# --- Typed line by line: the continuation prompt IS printed ---
send "\"\r"
expect {
    -re {"\r\n {16}> } {}
    timeout { puts "TESTFAIL: no continuation prompt after a typed unterminated string"; exit 1 }
}
send "\"\r"
expect {
    -re {"\r\n"\r\n"\r\nCOMMON-LISP-USER> } {}
    timeout { puts "TESTFAIL: multi-line string typed line by line: value not echoed as expected"; exit 1 }
}

# --- Pasted as one burst: NO continuation prompt ---
send "\"\r\"\r"
expect {
    -re {"\r\n"\r\n"\r\n"\r\nCOMMON-LISP-USER> } {}
    -re { +> } { puts "TESTFAIL: continuation prompt printed for a pasted multi-line string"; exit 1 }
    timeout { puts "TESTFAIL: pasted multi-line string: value not echoed as expected"; exit 1 }
}

# The reporter's second example, pasted: a newline inside a FORMAT argument.
send "(format nil \"~a\" \"\r\")\r"
expect {
    -re {\(format nil "~a" "\r\n"\)\r\n"\r\n"\r\nCOMMON-LISP-USER> } {}
    -re { +> } { puts "TESTFAIL: continuation prompt printed for a pasted multi-line form"; exit 1 }
    timeout { puts "TESTFAIL: pasted multi-line form: value not echoed as expected"; exit 1 }
}

# Typing again afterwards still gets the prompt (the probe is per line).
send "(list 1\r"
expect {
    -re {\(list 1\r\n {16}> } {}
    timeout { puts "TESTFAIL: no continuation prompt after a typed open form"; exit 1 }
}
send "2)\r"
expect {
    -re {2\)\r\n\(1 2\)\r\nCOMMON-LISP-USER> } {}
    timeout { puts "TESTFAIL: open form completed by typing: value not echoed as expected"; exit 1 }
}

send "(quit)\r"
expect {
    eof { puts "TESTPASS"; exit 0 }
    timeout { puts "TESTFAIL: (quit) did not end the session"; exit 1 }
}
EOF

out=$("$EXPECT" -f "$script" "$CLAMIGA" 2>&1)
case "$out" in
    *TESTPASS*)
        echo "test_repl_paste: 1 passed, 0 failed"
        echo PASS
        exit 0
        ;;
    *)
        echo "test_repl_paste: FAILED"
        echo "$out" | tail -20
        exit 1
        ;;
esac
