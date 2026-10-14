#!/bin/sh
# Regression: non-ASCII arguments must survive the command line intact.
#
# Windows hands main() its arguments in the process's ANSI code page — the
# real command line is UTF-16, and the CRT converts it down, turning every
# character the ACP cannot represent into '?'.  amilisp reads argv as UTF-8
# (that is what it is on POSIX and AmigaOS), so `--eval` forms and `--load`
# paths holding non-ASCII arrived destroyed: the codepoints below came back as
# 63 63 (two question marks).  main.c rebuilds argv from the UTF-16 command
# line; this checks the whole path from the shell through to CHAR-CODE.
#
# Run: sh tests/test_argv_utf8.sh [path-to-amilisp]

AMILISP="${1:-build/host/amilisp}"
passed=0
failed=0

check() {
    desc="$1"; expected="$2"; actual="$3"
    if [ "$actual" = "$expected" ]; then
        echo "  ok  $desc"; passed=$((passed + 1))
    else
        echo "  FAIL  $desc"
        echo "    expected: $expected"
        echo "    got:      $actual"
        failed=$((failed + 1))
    fi
}

# U+65E5 U+672C ("日本"), whose UTF-8 octets are E6 97 A5 E6 9C AC.
#
# The expectation is those OCTETS, not the codepoints, and that is not a
# mistake: a C-string reader source (which is what --eval is) is byte
# oriented, so each octet becomes one character on every platform — only the
# file reader decodes UTF-8, because the decoding lives in the stream layer.
# That asymmetry is pre-existing and is not what this test is about.  What it
# guards is that the octets ARRIVE AT ALL: on Windows the CRT converted the
# UTF-16 command line down to the ANSI code page before main() saw it, and
# every octet came through as 63 ('?') — the argument destroyed beyond
# recovery rather than merely re-interpreted.
result=$("$AMILISP" --no-userinit --non-interactive     --eval '(princ (map (quote list) (function char-code) "日本"))'     </dev/null 2>&1 | tr -d '
')
check "eval_argument_octets_survive" "(230 151 165 230 156 172)" "$result"

# U+1F600, four octets F0 9F 98 80 — also proves the UTF-16 surrogate pair is
# rejoined into one codepoint before the UTF-8 conversion instead of being
# encoded twice.
result=$("$AMILISP" --no-userinit --non-interactive     --eval '(princ (map (quote list) (function char-code) "😀"))'     </dev/null 2>&1 | tr -d '
')
check "eval_argument_astral_octets_survive" "(240 159 152 128)" "$result"

echo ""
echo "test_argv_utf8: $passed passed, $failed failed"
[ "$failed" -eq 0 ]
