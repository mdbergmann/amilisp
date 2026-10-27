#!/bin/sh
# REPL colour (src/core/color.h, color.c): with --color the prompt, the
# values, a WARNING and an ERROR each sit in their own SGR sequence and
# every one is closed with a reset; with --no-color (and by default when
# not interactive) no escape byte is written at all.  The host table is
# the ANSI one (36 prompt, 32 values, 33 warnings, 31 errors); the
# AmigaOS table is pinned by tests/test_color.c.
# Run: sh tests/test_color.sh [path-to-amilisp]

AMILISP="${1:-build/host/amilisp}"
case "$AMILISP" in
    /*) : ;;
    *) AMILISP="$(pwd)/$AMILISP" ;;
esac

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)
if [ -z "$TIMEOUT" ]; then
    echo "SKIP test_color: neither timeout nor gtimeout on PATH"
    exit 0
fi

out=$(mktemp "${TMPDIR:-/tmp}/amilisp_color_XXXXXX") || exit 1
trap 'rm -f "$out"' EXIT

passed=0
failed=0
ESC=$(printf '\033')

check_contains() {
    desc="$1"; needle="$2"; haystack="$3"
    case "$haystack" in
        *"$needle"*) echo "  ok  $desc"; passed=$((passed + 1)) ;;
        *) echo "  FAIL  $desc"; echo "    expected to contain: $(printf '%s' "$needle" | od -c | head -2)"
           failed=$((failed + 1)) ;;
    esac
}

check_absent() {
    desc="$1"; needle="$2"; haystack="$3"
    case "$haystack" in
        *"$needle"*) echo "  FAIL  $desc"; failed=$((failed + 1)) ;;
        *) echo "  ok  $desc"; passed=$((passed + 1)) ;;
    esac
}

# The interactive REPL with the given options; forms fed on stdin.
repl() {
    printf '(+ 1 2)\n(warn "careful")\n(error "boom")\n(quit)\n' |
        "$TIMEOUT" 60 "$AMILISP" --no-userinit --no-image "$@" > "$out" 2>&1
    cat "$out"
}

# --- --color: every role in its sequence, closed by a reset ------------------

cout=$(repl --color)
check_contains "prompt_in_cyan"       "${ESC}[36mCOMMON-LISP-USER> ${ESC}[0m" "$cout"
check_contains "value_in_green"       "${ESC}[32m3${ESC}[0m" "$cout"
check_contains "warning_tag_and_text_in_yellow" "${ESC}[33mWARNING:${ESC}[0m${ESC}[33m careful${ESC}[0m" "$cout"
check_contains "error_tag_and_text_in_red"      "${ESC}[31mERROR:${ESC}[0m${ESC}[31m SIMPLE-ERROR: boom${ESC}[0m" "$cout"
check_contains "banner_art_parens"    "${ESC}[94m" "$cout"
check_contains "banner_art_lambda"    "${ESC}[31m" "$cout"
check_contains "banner_text_in_cyan"  "${ESC}[36mAmiLisp v" "$cout"

# --- --no-color: not one escape byte -----------------------------------------

nout=$(repl --no-color)
check_absent "no_color_has_no_escape" "${ESC}" "$nout"
check_contains "no_color_still_prompts" "COMMON-LISP-USER> 3" "$nout"
check_contains "no_color_still_warns"   "WARNING: careful" "$nout"

# --- default: off when not interactive ---------------------------------------

dout=$("$TIMEOUT" 60 "$AMILISP" --no-userinit --no-image --non-interactive \
           --eval '(warn "quiet")' 2>&1 </dev/null)
check_absent "non_interactive_default_has_no_escape" "${ESC}" "$dout"
check_contains "non_interactive_still_warns" "WARNING: quiet" "$dout"

echo "test_color: $passed passed, $failed failed"
[ "$failed" -eq 0 ]
