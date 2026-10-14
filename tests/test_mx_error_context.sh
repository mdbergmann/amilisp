#!/bin/sh
# Errors escaping a macro expander must report WHICH form was being
# expanded ("While macroexpanding: (THREE-REQ 1)").  Without it, a
# malformed macro call — e.g. a feature-conditional argument that
# evaporated, like (defctype :size #+64-bit :uint64) with no 64-BIT
# feature — surfaces only as an arity error deep inside the expander.
# Also: a caught-and-recovered expansion error must NOT leave stale
# context that mislabels a later, unrelated error.
# Run: sh tests/test_mx_error_context.sh [path-to-amilisp]

CLAMIGA="${1:-build/host/amilisp}"
passed=0
failed=0
total=0

check_contains() {
    desc="$1"
    needle="$2"
    haystack="$3"
    total=$((total + 1))
    case "$haystack" in
        *"$needle"*)
            echo "  ok  $desc"
            passed=$((passed + 1)) ;;
        *)
            echo "  FAIL  $desc"
            echo "    expected to contain: $needle"
            echo "    got: $(echo "$haystack" | head -5)"
            failed=$((failed + 1)) ;;
    esac
}

check_not_contains() {
    desc="$1"
    needle="$2"
    haystack="$3"
    total=$((total + 1))
    case "$haystack" in
        *"$needle"*)
            echo "  FAIL  $desc"
            echo "    expected NOT to contain: $needle"
            failed=$((failed + 1)) ;;
        *)
            echo "  ok  $desc"
            passed=$((passed + 1)) ;;
    esac
}

# --- Error inside an expander names the form being expanded ---

result=$("$CLAMIGA" --no-userinit --non-interactive \
    --eval '(defmacro three-req (a b c) (list (quote list) a b c))' \
    --eval '(three-req 1)' </dev/null 2>&1)
check_contains "arity_error_reported"  "Too few arguments" "$result"
check_contains "context_names_form"    "While macroexpanding: (THREE-REQ 1)" "$result"

# --- Caught expansion error leaves no stale context on later errors ---

result=$("$CLAMIGA" --no-userinit --non-interactive \
    --eval '(defmacro two-req (a b) (list (quote list) a b))' \
    --eval '(handler-case (macroexpand (quote (two-req 1))) (error () (format t "CAUGHT~%")))' \
    --eval '(car 5)' </dev/null 2>&1)
check_contains     "expansion_error_caught"  "CAUGHT" "$result"
check_contains     "later_error_reported"    "ERROR:" "$result"
check_not_contains "no_stale_context"        "While macroexpanding" "$result"

# --- Successful expansions never emit context on unrelated errors ---

result=$("$CLAMIGA" --no-userinit --non-interactive \
    --eval '(progn (when t 1) (car 5))' </dev/null 2>&1)
check_contains     "plain_error_reported" "ERROR:" "$result"
check_not_contains "plain_error_no_context" "While macroexpanding" "$result"

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
