#!/bin/sh
# The REPL banner (main.c, print_banner) tells the user which binary this
# is and what THIS run got:
#   - name + version, the platform line, the exit hint (what other tests
#     and users key on) are present, and only in the interactive REPL
#   - the heap figure follows --heap, spelled the way --heap spells it
#   - the JIT segment says "JIT" by default and "JIT off" after --no-jit
#     (absent on a build without a native backend)
#   - the boot line names the restored image file, or says there was none
#   - the text column is aligned beside the art
# Run: sh tests/test_banner.sh [path-to-amilisp]

AMILISP="${1:-build/host/amilisp}"
case "$AMILISP" in
    /*) : ;;
    *) AMILISP="$(pwd)/$AMILISP" ;;
esac

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)
if [ -z "$TIMEOUT" ]; then
    echo "SKIP test_banner: neither timeout nor gtimeout on PATH"
    exit 0
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/amilisp_banner_XXXXXX") || exit 1
trap 'rm -rf "$TMP"' EXIT

passed=0
failed=0

check_contains() {
    desc="$1"; needle="$2"; haystack="$3"
    case "$haystack" in
        *"$needle"*) echo "  ok  $desc"; passed=$((passed + 1)) ;;
        *) echo "  FAIL  $desc"; echo "    expected to contain: $needle"
           echo "    got:"; printf '%s\n' "$haystack" | sed 's/^/      /' | head -12
           failed=$((failed + 1)) ;;
    esac
}

check_absent() {
    desc="$1"; needle="$2"; haystack="$3"
    case "$haystack" in
        *"$needle"*) echo "  FAIL  $desc"; echo "    did not expect: $needle"
                     printf '%s\n' "$haystack" | sed 's/^/      /' | head -12
                     failed=$((failed + 1)) ;;
        *) echo "  ok  $desc"; passed=$((passed + 1)) ;;
    esac
}

# The interactive REPL with the given options; (quit) fed on stdin.
banner() {
    echo '(quit)' | "$TIMEOUT" 60 "$AMILISP" --no-color --no-userinit --no-image "$@" 2>&1 |
        grep -v '^; Loading'
}

# --- Default banner ---------------------------------------------------------

out=$(banner)
check_contains "name_and_version" "AmiLisp v" "$out"
check_contains "platform_line" "Common Lisp for " "$out"
check_contains "exit_hint_is_its_own_line" "
Type (quit) to exit.
" "$out"
check_contains "default_heap" "4M heap" "$out"
check_contains "collector_named" " GC" "$out"
check_contains "no_image_boot_line" "booted from lib/ (no image)" "$out"

# The art is 20 columns wide; every text line starts at column 21.
aligned=1
for needle in "AmiLisp v" "Common Lisp for" " heap, " "booted from"; do
    line=$(printf '%s\n' "$out" | grep -F -- "$needle" | head -1)
    col20=$(printf '%s' "$line" | cut -c20)
    col21=$(printf '%s' "$line" | cut -c21)
    if [ "$col20" != " " ] || [ "$col21" = " " ] || [ -z "$col21" ]; then
        aligned=0
        echo "    misaligned: [$line]"
    fi
done
if [ $aligned -eq 1 ]; then
    echo "  ok  text_column_aligned"; passed=$((passed + 1))
else
    echo "  FAIL  text_column_aligned"; failed=$((failed + 1))
fi

# No trailing blanks on the art-only lines (the first and the last).
first=$(printf '%s\n' "$out" | grep -F -- '  (((      \\' | head -1)
case "$first" in
    *' ') echo "  FAIL  no_trailing_blanks_on_art_lines"; failed=$((failed + 1)) ;;
    *)    echo "  ok  no_trailing_blanks_on_art_lines"; passed=$((passed + 1)) ;;
esac

# --- Heap figure follows --heap ---------------------------------------------

check_contains "heap_6M" "6M heap" "$(banner --heap 6M)"
check_contains "heap_1500K" "1500K heap" "$(banner --heap 1500K)"

# --- JIT state of this run --------------------------------------------------

case "$out" in
    *", JIT"*)
        check_contains "jit_on_by_default" ", JIT," "$out"
        check_contains "no_jit_says_off" "JIT off" "$(banner --no-jit)" ;;
    *)
        echo "  ok  jit_segment_absent_without_native_backend"; passed=$((passed + 1))
        check_absent "no_jit_segment_without_backend" "JIT" "$(banner --no-jit)" ;;
esac

# --- Boot line names the image ----------------------------------------------

cat > "$TMP/save.lisp" <<EOF
(ext:save-image "$TMP/amilisp.img" :quit t)
EOF
sout=$("$TIMEOUT" 120 "$AMILISP" --no-userinit --no-image --heap 16M \
        --load "$TMP/save.lisp" </dev/null 2>&1)
if [ -f "$TMP/amilisp.img" ]; then
    echo "  ok  image_saved"; passed=$((passed + 1))
    iout=$(echo '(quit)' | "$TIMEOUT" 60 "$AMILISP" --no-color --no-userinit \
            --heap 16M --image "$TMP/amilisp.img" 2>&1)
    check_contains "explicit_image_named" "booted from amilisp.img" "$iout"
    check_absent "explicit_image_not_no_image" "(no image)" "$iout"
    dout=$(cd "$TMP" && echo '(quit)' | "$TIMEOUT" 60 "$AMILISP" --no-color \
            --no-userinit --heap 16M 2>&1)
    check_contains "discovered_image_named" "booted from amilisp.img" "$dout"
else
    echo "  FAIL  image_saved"; printf '%s\n' "$sout" | sed 's/^/      /' | head -12
    failed=$((failed + 1))
fi

# --- Only the interactive REPL prints it -------------------------------------

bout=$(echo '(quit)' | "$TIMEOUT" 60 "$AMILISP" --no-color --no-userinit --no-image --batch 2>&1)
check_absent "batch_has_no_banner" "AmiLisp v" "$bout"
nout=$("$TIMEOUT" 60 "$AMILISP" --no-color --no-userinit --no-image --non-interactive </dev/null 2>&1)
check_absent "non_interactive_has_no_banner" "AmiLisp v" "$nout"

echo "test_banner: $passed passed, $failed failed"
[ $failed -eq 0 ]
