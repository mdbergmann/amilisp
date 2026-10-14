#!/bin/sh
# Integration tests for --batch mode piping
# Run: make test-batch (or sh tests/test_batch.sh)

AMILISP="${1:-build/host/amilisp}"
passed=0
failed=0
total=0

check() {
    desc="$1"
    expected="$2"
    actual="$3"
    total=$((total + 1))
    if [ "$actual" = "$expected" ]; then
        echo "  ok  $desc"
        passed=$((passed + 1))
    else
        echo "  FAIL  $desc"
        echo "    expected: $(echo "$expected" | head -5)"
        echo "    got:      $(echo "$actual" | head -5)"
        failed=$((failed + 1))
    fi
}

# --- Basic output ---

result=$(echo '(+ 1 2)' | "$AMILISP" --no-userinit --batch 2>&1)
check "batch_single_expr_prints_result" "3" "$result"

result=$(printf '(+ 1 2)\n(* 3 4)\n' | "$AMILISP" --no-userinit --batch 2>&1)
check "batch_two_exprs_separate_lines" "3
12" "$result"

# --- Multiple expressions on one line ---

result=$(echo '(+ 1 2) (* 3 4)' | "$AMILISP" --no-userinit --batch 2>&1)
check "batch_two_exprs_same_line" "3
12" "$result"

result=$(echo '(+ 1 2) (* 3 4) (- 10 3)' | "$AMILISP" --no-userinit --batch 2>&1)
check "batch_three_exprs_same_line" "3
12
7" "$result"

# --- Multi-line expression ---

result=$(printf '(+ 1\n   2)\n' | "$AMILISP" --no-userinit --batch 2>&1)
check "batch_multiline_expr" "3" "$result"

result=$(printf '(defun foo (x)\n  (* x x))\n(foo 7)\n' | "$AMILISP" --no-userinit --batch 2>&1)
check "batch_defun_and_call" "FOO
49" "$result"

# --- String results ---

result=$(echo '(string-upcase "hello")' | "$AMILISP" --no-userinit --batch 2>&1)
check "batch_string_result" '"HELLO"' "$result"

# --- NIL and T ---

result=$(echo '(null nil)' | "$AMILISP" --no-userinit --batch 2>&1)
check "batch_nil_t" "T" "$result"

# --- Comments and blank lines skipped ---

result=$(printf '; comment\n(+ 1 2)\n' | "$AMILISP" --no-userinit --batch 2>&1)
check "batch_skip_comment_line" "3" "$result"

result=$(printf '\n\n(+ 1 2)\n' | "$AMILISP" --no-userinit --batch 2>&1)
check "batch_skip_blank_lines" "3" "$result"

# --- List and nested results ---

result=$(echo '(list 1 2 3)' | "$AMILISP" --no-userinit --batch 2>&1)
check "batch_list_result" "(1 2 3)" "$result"

# --- Zero values print nothing (issue #6) ---
# (values) returns no values at all, so the REPL echoes nothing — not NIL.

result=$(echo '(values)' | "$AMILISP" --no-userinit --batch 2>&1)
check "batch_values_none_prints_nothing" "" "$result"

result=$(echo '(values-list nil)' | "$AMILISP" --no-userinit --batch 2>&1)
check "batch_values_list_empty_prints_nothing" "" "$result"

# One value that happens to be NIL is still a value and must print.
result=$(echo '(values nil)' | "$AMILISP" --no-userinit --batch 2>&1)
check "batch_values_nil_prints_nil" "NIL" "$result"

# A zero-value form must not silence the forms after it.
result=$(printf '(values)\n(+ 40 2)\n(values)\n(list)\n' | \
         "$AMILISP" --no-userinit --batch 2>&1)
check "batch_values_none_does_not_swallow_next" "42
NIL" "$result"

# --- Every value is echoed, one per line (CLHS 25.1) ---

result=$(echo '(floor 7 2)' | "$AMILISP" --no-userinit --batch 2>&1)
check "batch_two_values_one_per_line" "3
1" "$result"

result=$(echo '(values 1 2 3)' | "$AMILISP" --no-userinit --batch 2>&1)
check "batch_three_values_one_per_line" "1
2
3" "$result"

result=$(echo "(values-list '(:a \"b\"))" | "$AMILISP" --no-userinit --batch 2>&1)
check "batch_values_printed_readably" ':A
"b"' "$result"

# A form specified to return ONE value must not echo the multiple values of
# an earlier one — the value comes back through OP_LOAD, which leaves the
# MV buffer alone (compiler MV hygiene, see cl_mv_normalize).
result=$(echo '(let ((x (floor 7 2))) x)' | "$AMILISP" --no-userinit --batch 2>&1)
check "batch_single_value_form_echoes_one_value" "3" "$result"

result=$(printf '(defun mv-id (x) x)\n(mv-id (values 1 2))\n' | \
         "$AMILISP" --no-userinit --batch 2>&1)
check "batch_passthrough_defun_echoes_one_value" "MV-ID
1" "$result"

# --- REPL buffer overflow: loud discard, never a silently-truncated eval ---
# A multi-line form larger than the 4096-char accumulator used to have its
# overflowing lines silently dropped, then the mangled prefix was evaluated.
# It must now print a diagnostic, discard the form, and keep the session
# usable (the trailing (+ 40 2) still evaluates).

bigform=$(awk 'BEGIN{print "(list";for(i=0;i<60;i++){s="";for(j=0;j<40;j++)s=s" 1";print s};print ")"}')
result=$(printf '%s
(+ 40 2)
' "$bigform" | "$AMILISP" --no-userinit --batch 2>&1)
total=$((total + 1))
case "$result" in
  *"exceeds the REPL buffer"*)
    case "$result" in
      *42*) echo "  ok  repl_overflow_discards_loudly"; passed=$((passed + 1)) ;;
      *) echo "  FAIL  repl_overflow_discards_loudly (session dead after discard)"
         failed=$((failed + 1)) ;;
    esac ;;
  *)
    echo "  FAIL  repl_overflow_discards_loudly (no diagnostic printed)"
    failed=$((failed + 1)) ;;
esac

# --- Summary ---

echo ""
echo "$passed passed, $failed failed, $total total"
if [ "$failed" -gt 0 ]; then
    echo "FAIL"
    exit 1
else
    echo "PASS"
    exit 0
fi
