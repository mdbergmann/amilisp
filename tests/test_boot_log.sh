#!/bin/sh
# Boot progress logging is opt-in via --boot-log:
#   - default output carries no "; [boot]" / "; [clos]" / "; [jit]" lines
#   - --boot-log prints the streamlined boot phase timings
# Run: sh tests/test_boot_log.sh [path-to-amilisp]

AMILISP="${1:-build/host/amilisp}"
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
            echo "    got: $(echo "$haystack" | grep -F "$needle" | head -5)"
            failed=$((failed + 1)) ;;
        *)
            echo "  ok  $desc"
            passed=$((passed + 1)) ;;
    esac
}

# --- Default: quiet boot, no progress lines ---

result=$("$AMILISP" --no-userinit --non-interactive --eval '(format t "R=~A~%" (+ 1 2))' </dev/null 2>&1)
check_not_contains "default_no_boot_lines" "; [boot]" "$result"
check_not_contains "default_no_clos_lines" "; [clos]" "$result"
check_not_contains "default_no_jit_lines"  "; [jit]"  "$result"
check_contains     "default_still_evaluates" "R=3" "$result"

# --- --boot-log: streamlined phase timings appear ---

result=$("$AMILISP" --no-userinit --non-interactive --boot-log --eval '(format t "R=~A~%" (+ 1 2))' </dev/null 2>&1)
check_contains "bootlog_core_setup"   "; [boot]" "$result"
check_contains "bootlog_boot_library" "boot library" "$result"
check_contains "bootlog_clos_phase"   ": CLOS" "$result"
check_contains "bootlog_ready"        ": ready" "$result"
check_contains "bootlog_clos_trace"   "; [clos]" "$result"
check_contains "bootlog_still_evaluates" "R=3" "$result"

# --- Explicit --boot-log wins over --batch quieting ---

result=$(echo '(+ 1 2)' | "$AMILISP" --no-userinit --batch --boot-log 2>&1)
check_contains "bootlog_overrides_batch" "; [boot]" "$result"

# --- Plain --batch stays exactly clean (regression guard) ---

result=$(echo '(+ 1 2)' | "$AMILISP" --no-userinit --batch 2>&1)
check_not_contains "batch_stays_quiet" "; [boot]" "$result"

# --- --boot-log on an image restore: the restore line covers stage + restore ---
# main() times the staging (read + verify) and the restore and hands the sum
# to the post-restore init, whose own clock would otherwise report the one
# number this line exists for as 0 ms.  No FASL boot phases on this path.

WORK=$(mktemp -d "${TMPDIR:-/tmp}/amilisp_bootlog_XXXXXX")
trap 'rm -rf "$WORK"' EXIT INT TERM
"$AMILISP" --no-userinit --no-image --non-interactive \
    --eval "(ext:save-image \"$WORK/bootlog.img\")" </dev/null >/dev/null 2>&1
result=$("$AMILISP" --no-userinit --non-interactive --boot-log \
    --image "$WORK/bootlog.img" --eval '(format t "R=~A~%" (+ 1 2))' </dev/null 2>&1)
check_contains     "bootlog_image_restored_line"   "image restored (read + relink)" "$result"
check_not_contains "bootlog_image_skips_fasl_boot" "boot library" "$result"
check_contains     "bootlog_image_still_evaluates" "R=3" "$result"

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
