#!/bin/sh
# Interactive REPL: multiple-value echo and the value history variables.
#
# CLHS 25.1 says the REPL prints "the results" of the form, plural — SBCL
# prints 3 and 1 for (floor 7 2).  amilisp used to print only the primary
# value, because printing all of them would have been WRONG more often than
# right: value-propagating opcodes leave the multiple-value buffer alone, so
# (let ((x (floor 7 2))) x) still carried floor's second value.  The
# compiler now normalizes the MV state in front of every observer
# (cl_mv_normalize), which is what makes the echo below trustworthy — so
# this test pins the echo AND the hygiene together, through the real
# interactive loop (test_batch.sh covers the batch loop).
#
# It also pins CLHS 25.1.1's value history: * is the primary value, / the
# list of all of them, // and /// the two before that.
#
# And it pins the REPL's line discipline (issue #14): values start on a
# fresh line (so (format t "x") echoes NIL below the x, with no spurious
# blank line when the form printed nothing), a string literal may span
# input lines (CLHS 2.4.5 — the scanner asks for more input instead of
# handing the reader an "Unterminated string"), and prin1 prints a newline
# inside a string literally, escaping only " and \ (CLHS 22.1.3.4).
#
# Run: sh tests/test_repl_values.sh build/host/amilisp

AMILISP="${1:-build/host/amilisp}"

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)
if [ -z "$TIMEOUT" ]; then
    echo "SKIP test_repl_values: neither timeout nor gtimeout on PATH"
    exit 0
fi

out=$(mktemp "${TMPDIR:-/tmp}/amilisp_replvals_XXXXXX") || exit 1
trap 'rm -f "$out"' EXIT

fail=0
passed=0

# Feed the interactive REPL (no --batch) and strip the banner, the prompts
# and the sign-off, leaving just the echoed values.
repl() {
    printf '%s\n(quit)\n' "$1" |
        "$TIMEOUT" 60 "$AMILISP" --no-userinit --no-color > "$out" 2>&1
    sed -e 's/COMMON-LISP-USER> //g' -e 's/^ *> //' "$out" |
        sed -n '/^Type (quit) to exit\./,$p' |
        grep -v '^Type (quit) to exit\.$' | grep -v '^Bye\.$' | grep -v '^$'
}

check() {
    desc="$1"; expected="$2"; actual="$3"
    if [ "$actual" = "$expected" ]; then
        echo "  ok  $desc"
        passed=$((passed + 1))
    else
        echo "  FAIL  $desc"
        echo "    expected: $expected"
        echo "    got:      $actual"
        fail=1
    fi
}

# --- Every value is echoed, one per line ---

check "repl_echoes_both_values_of_floor" "3
1" "$(repl '(floor 7 2)')"

check "repl_echoes_three_values" "1
2
3" "$(repl '(values 1 2 3)')"

check "repl_zero_values_echo_nothing" "" "$(repl '(values)')"

check "repl_single_value_unchanged" "42" "$(repl '42')"

# --- The echo tells the truth: a one-value form echoes one value ---

check "repl_variable_reference_echoes_one_value" "3" \
      "$(repl '(let ((x (floor 7 2))) x)')"

check "repl_setq_echoes_one_value" "1" \
      "$(repl '(let ((y 0)) (setq y (values 1 2)))')"

# --- Value history (CLHS 25.1.1): * is primary, / is the list ---

check "repl_history_star_and_slash" "3
1
(3 (3 1))" "$(repl '(floor 7 2)
(list * /)')"

# / of (values 1 2) is (1 2); the next form returns the single value 7, so
# / becomes (7) and (1 2) shifts into //.
check "repl_history_slash_shifts" "1
2
7
((7) (1 2) NIL)" "$(repl '(values 1 2)
7
(list / // ///)')"

# --- Values start on a fresh line (issue #14) ---

# Output without a trailing newline: the echoed NIL goes on its own line,
# not glued to the hello!.
check "repl_value_on_fresh_line_after_output" "hello!
NIL" "$(repl '(format t "~a" "hello!")')"

# Output that already ends at column 0 gets no extra blank line (the
# grep -v '^$' in repl() would hide one, so count the lines instead).
lines=$(repl '(format t "~a~%" "hello!")' | wc -l | tr -d ' ')
check "repl_no_double_newline_after_terpri" "2" "$lines"

# --- prin1 prints a newline inside a string literally (CLHS 22.1.3.4) ---

check "repl_prin1_newline_literal" '"a
"' "$(repl '(format nil "a~%")')"

# --- A string literal may span input lines (CLHS 2.4.5, issue #14) ---

check "repl_multiline_string_literal" '"x
y"' "$(repl '"x
y"')"

check "repl_multiline_string_length" "3" "$(repl '(length "x
y")')"

# --- Reader: backslash is a single escape (CLHS 2.4.5) ---

check "repl_string_backslash_n_is_letter_n" '#\n' "$(repl '(char "x\n" 1)')"

if [ "$fail" -eq 0 ]; then
    echo "test_repl_values: $passed passed, 0 failed"
    echo PASS
    exit 0
fi
echo "test_repl_values: FAILED"
exit 1
