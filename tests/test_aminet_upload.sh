#!/bin/sh
# Executable specification of scripts/aminet-upload.sh — the Aminet
# packaging rules (wiki.aminet.net/Uploading_instructions, The_Readme_file)
# as checks against the pair the script stages.  Runs without amilisp and
# without the network: every run is --dry-run.
#
# Part 1  staging: the release archive amilisp-<v>-bin.lha becomes
#         clamiga.lha (byte-identical — Aminet updates overwrite the SAME
#         name, the version lives in the readme), the readme is built from
#         the template with @VERSION@/@UPLOADER@/@NOTES@ filled in, notes
#         are folded to 78 columns and typographic punctuation is
#         transliterated (the Amiga has no UTF-8), Replaces: only on
#         request, the dry run prints the curl commands and uploads nothing.
# Part 2  the readme rules: Short: first and <= 40 chars, the four
#         mandatory fields, LF only, printable ASCII, <= 78 columns, the
#         header terminated by an empty line, Version: = the archive's.
# Part 3  refusals — each names its cause: no uploader / no e-mail in it,
#         no notes and no tag, a missing archive (points at
#         make-binary-release.sh), a non-transliterable character, a
#         too-long Short:, CR line endings, a mismatched archive top dir.
#
# Run: sh tests/test_aminet_upload.sh
#
# macOS / Linux only.  The upload is made from a developer's machine, and
# the script refuses to run under MSYS2 (where the CRLF-template refusal
# below did not reproduce with MSYS2's shell tools), so the Windows CI job
# skips this test.

case "$(uname -s)" in
    MINGW*|MSYS*|CLANGARM64*|CLANG64*|UCRT64*)
        echo "skip: test_aminet_upload (aminet-upload.sh runs from macOS or Linux, not from MSYS2)"
        exit 0 ;;
esac

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SCRIPT="$ROOT/scripts/aminet-upload.sh"
TMPD=$(mktemp -d "${TMPDIR:-/tmp}/test_aminet_upload_XXXXXX") || exit 1
trap 'rm -rf "$TMPD"' EXIT INT TERM

passed=0
failed=0
ok()   { passed=$((passed + 1)); echo "  ok    $1"; }
fail() { failed=$((failed + 1)); echo "  FAIL  $1"; [ -n "$2" ] && printf '%s\n' "$2" | sed 's/^/        /'; }
expect_fail() {  # expect_fail DESC PATTERN -- args...: script must exit non-zero and mention PATTERN
    desc=$1; pat=$2; shift 3
    out=$("$SCRIPT" "$@" 2>&1); rc=$?
    if [ $rc -ne 0 ] && echo "$out" | grep -q -- "$pat"; then
        ok "$desc"
    else
        fail "$desc (rc=$rc, expected non-zero and '$pat')" "$(echo "$out" | tail -6)"
    fi
}

# --- fixtures ---------------------------------------------------------------
# 9.9.9: no tag v9.9 exists, so the tag fallback is exercised as an error and
# every good run passes --notes.
V=9.9.9
mkdir -p "$TMPD/amilisp-$V/bin/aos3"
echo "fixture" > "$TMPD/amilisp-$V/README-FIRST.md"
echo "binary"  > "$TMPD/amilisp-$V/bin/aos3/amilisp"
HAVE_LHA=0
if command -v lha > /dev/null 2>&1; then
    HAVE_LHA=1
    ( cd "$TMPD" && lha aq "amilisp-$V-bin.lha" "amilisp-$V" ) || exit 1
    # a second archive whose top directory is NOT amilisp-<version>/
    mkdir -p "$TMPD/other" "$TMPD/wrongtop" "$TMPD/broken"
    cp -R "$TMPD/amilisp-$V" "$TMPD/other/amilisp-1.0.0"
    ( cd "$TMPD/other" && lha aq "$TMPD/wrongtop/amilisp-$V-bin.lha" "amilisp-1.0.0" ) || exit 1
    printf 'garbage' > "$TMPD/broken/amilisp-$V-bin.lha"
else
    printf 'not really an lha\n' > "$TMPD/amilisp-$V-bin.lha"
fi
ARCHIVE="$TMPD/amilisp-$V-bin.lha"
UPL="tester@example.org (Test Person)"

# notes with an em dash, an arrow, curly quotes and a 130-column line
LONG="the quick brown fox jumps over the lazy dog and keeps running because this line is deliberately far longer than seventy-eight columns end"
# (U+2014 em dash, U+2192 arrow, U+201C/U+201D curly quotes)
printf 'AmiLisp %s \342\200\224 heap images (EXT:SAVE-IMAGE / --image); footprint 1.42 MB \342\206\222 0.18 MB; \342\200\234quoted\342\200\235 done\n%s\n' \
    "$V" "$LONG" > "$TMPD/notes.txt"

run_ok() {  # run_ok OUTDIR [extra args...]
    o=$1; shift
    "$SCRIPT" --dry-run --archive "$ARCHIVE" --notes "$TMPD/notes.txt" --uploader "$UPL" --out "$o" "$@" 2>&1
}

# ---------------------------------------------------------------- Part 1
echo "=== test_aminet_upload: staging ==="
OUT1="$TMPD/out1"
out=$(run_ok "$OUT1"); rc=$?
if [ $rc -eq 0 ]; then ok "dry run exits 0"; else fail "dry run must exit 0 (rc=$rc)" "$(echo "$out" | tail -8)"; fi
if [ -f "$OUT1/clamiga.lha" ] && cmp -s "$ARCHIVE" "$OUT1/clamiga.lha"; then
    ok "clamiga.lha is the release archive, byte for byte (no version in the file name)"
else
    fail "clamiga.lha must be a copy of $ARCHIVE"
fi
[ -f "$OUT1/clamiga.readme" ] && ok "clamiga.readme staged next to it" || fail "clamiga.readme missing"
RM="$OUT1/clamiga.readme"
grep -q "^Version:      $V\$" "$RM" && ok "Version: $V from the archive name" || fail "Version: must be $V" "$(head -12 "$RM")"
grep -q "^Uploader:     $UPL\$" "$RM" && ok "Uploader: from --uploader" || fail "Uploader: line wrong" "$(grep Uploader "$RM")"
grep -q "cd Work:amilisp-$V\$" "$RM" && ok "@VERSION@ substituted in the body (getting-started cd line)" || fail "body must mention amilisp-$V"
grep -q '@[A-Z]*@' "$RM" && fail "unreplaced placeholder" "$(grep -n '@[A-Z]*@' "$RM")" || ok "no placeholder left"
grep -q "^AmiLisp $V - heap images" "$RM" && ok "notes inserted; em dash -> '-'" || fail "notes/em dash" "$(grep -n 'heap images' "$RM")"
grep -q '1.42 MB ->' "$RM" && ok "arrow -> '->'" || fail "arrow transliteration" "$(grep -n '1.42' "$RM")"
grep -q '"quoted" done' "$RM" && ok "curly quotes -> '\"'" || fail "curly quotes" "$(grep -n quoted "$RM")"
if grep -q 'far longer than' "$RM" && grep -q 'columns end$' "$RM" && ! grep -q "^$LONG\$" "$RM"; then
    ok "a 130-column notes line is folded, nothing lost"
else
    fail "long notes line must be folded" "$(grep -n 'quick brown' "$RM")"
fi
grep -q '^Replaces:' "$RM" && fail "Replaces: must not appear unless requested" || ok "no Replaces: by default (same-name update)"
echo "$out" | grep -q 'DRY RUN' && echo "$out" | grep -q 'ftp://main.aminet.net/new/' \
    && ok "dry run names the FTP target" || fail "dry run must print the target" "$(echo "$out" | tail -4)"
echo "$out" | grep -q -- '--upload-file .*clamiga\.lha .*anonymous:tester@example\.org' \
    && ok "curl command: archive as anonymous with the uploader's e-mail as password" \
    || fail "curl command line" "$(echo "$out" | grep curl)"
n_curl=$(echo "$out" | grep -c '^  curl ')
[ "$n_curl" = 2 ] && ok "two uploads: the archive, then the readme" || fail "expected 2 curl lines, got $n_curl"
echo "$out" | grep -q '^  ok      Short:' && ok "checks are reported" || fail "check report missing"
if [ $HAVE_LHA = 1 ]; then
    echo "$out" | grep -q 'lha t: archive is sound' && ok "lha t ran on the staged archive" || fail "lha t report missing" "$out"
    echo "$out" | grep -q "unpacks into amilisp-$V/" && ok "top-level directory amilisp-$V/ verified" || fail "top dir check" "$out"
fi

OUT2="$TMPD/out2"
out=$(run_ok "$OUT2" --replaces dev/lang/clamiga.lha); rc=$?
if [ $rc -eq 0 ] && grep -q '^Replaces:     dev/lang/clamiga\.lha$' "$OUT2/clamiga.readme"; then
    ok "--replaces emits Replaces: in the header"
else
    fail "--replaces" "$(head -12 "$OUT2/clamiga.readme" 2>/dev/null)"
fi
hdr_end=$(awk '/^$/ { print NR; exit }' "$OUT2/clamiga.readme")
repl_line=$(grep -n '^Replaces:' "$OUT2/clamiga.readme" | cut -d: -f1)
[ "$repl_line" -lt "$hdr_end" ] && ok "Replaces: sits inside the header block" || fail "Replaces: outside the header"

# ---------------------------------------------------------------- Part 2
echo "=== test_aminet_upload: readme rules ==="
head -1 "$RM" | grep -q '^Short: ' && ok "Short: is the first line" || fail "first line" "$(head -1 "$RM")"
short=$(sed -n '1s/^Short:[ ]*//p' "$RM")
[ ${#short} -le 40 ] && [ ${#short} -gt 0 ] && ok "Short: is ${#short} chars (<= 40)" || fail "Short: length ${#short}"
for f in Short Uploader Type Architecture; do
    [ "$(grep -c "^$f: " "$RM")" = 1 ] && ok "mandatory field $f: present once" || fail "mandatory field $f:"
done
hdr_end=$(awk '/^$/ { print NR; exit }' "$RM")
if [ -n "$hdr_end" ] && ! head -n $((hdr_end - 1)) "$RM" | grep -v -E '^[A-Za-z]+:[ ]' | grep -q .; then
    ok "header = 'Field: value' lines terminated by an empty line (line $hdr_end)"
else
    fail "header block shape" "$(head -n 12 "$RM")"
fi
LC_ALL=C grep -q "$(printf '\r')" "$RM" && fail "CR in readme" || ok "LF line endings only"
LC_ALL=C grep -q '[^ -~]' "$RM" && fail "non-ASCII in readme" "$(LC_ALL=C grep -n '[^ -~]' "$RM" | head -3)" || ok "printable ASCII only"
long=$(awk 'length($0) > 78' "$RM")
[ -z "$long" ] && ok "no line longer than 78 columns" || fail "line > 78 columns" "$long"
grep -q '^Type:         dev/lang$' "$RM" && ok "Type: dev/lang" || fail "Type:"
grep -q '^Architecture: m68k-amigaos >= 3.0.0; ppc-morphos$' "$RM" && ok "Architecture: both binaries' targets, qualified" || fail "Architecture:" "$(grep Architecture "$RM")"
grep -q '^Distribution: Aminet$' "$RM" && ok "Distribution: Aminet" || fail "Distribution:"
# the heading underline follows its heading
title_len=$(awk 'NR > 1 && /^AmiLisp / { print length($0); exit }' "$RM")
ul_len=$(awk 'NR > 1 && /^=+$/ { print length($0); exit }' "$RM")
[ -n "$title_len" ] && [ "$title_len" = "$ul_len" ] && ok "heading underline matches the heading ($title_len)" || fail "underline $ul_len vs heading $title_len"
[ "$(grep -c '^$' "$RM")" -gt 0 ] && awk 'NR > 1 && prev == "" && $0 == "" { bad = 1 } { prev = $0 } END { exit bad }' "$RM" \
    && ok "no doubled empty lines" || fail "doubled empty lines in the readme"

# ---------------------------------------------------------------- Part 3
echo "=== test_aminet_upload: refusals ==="
BASE="--dry-run --archive $ARCHIVE --notes $TMPD/notes.txt --out $TMPD/out3"
out=$(AMINET_UPLOADER= "$SCRIPT" $BASE 2>&1); rc=$?
[ $rc -ne 0 ] && echo "$out" | grep -q 'AMINET_UPLOADER' && ok "no uploader: refused, names AMINET_UPLOADER" || fail "no uploader (rc=$rc)" "$out"
expect_fail "uploader without an e-mail: refused" 'no e-mail address' -- $BASE --uploader "Just A Name"
expect_fail "no notes and no tag v9.9: refused, points at --notes" '--notes' -- --dry-run --archive "$ARCHIVE" --uploader "$UPL" --out "$TMPD/out3"
expect_fail "missing archive: points at make-binary-release.sh" 'make-binary-release.sh' -- --dry-run --archive "$TMPD/nowhere/amilisp-$V-bin.lha" --notes "$TMPD/notes.txt" --uploader "$UPL" --out "$TMPD/out3"
[ ! -e "$TMPD/out3/clamiga.lha" ] && ok "a refused run before staging leaves nothing behind" || fail "out3 populated"
expect_fail "archive name without a version: refused" 'amilisp-<version>-bin.lha' -- --dry-run --archive "$TMPD/clamiga.lha" --notes "$TMPD/notes.txt" --uploader "$UPL" --out "$TMPD/out3"

printf 'Umlaut \303\274 here\n' > "$TMPD/notes-umlaut.txt"
out=$("$SCRIPT" --dry-run --archive "$ARCHIVE" --notes "$TMPD/notes-umlaut.txt" --uploader "$UPL" --out "$TMPD/out4" 2>&1); rc=$?
if [ $rc -ne 0 ] && echo "$out" | grep -q 'outside printable ASCII' && echo "$out" | grep -q 'Umlaut'; then
    ok "a non-transliterable character is refused and the line shown"
else
    fail "umlaut must be refused with its line (rc=$rc)" "$(echo "$out" | grep -i -A3 'ascii')"
fi
echo "$out" | grep -q 'DRY RUN' && fail "a refused run must not reach the upload step" || ok "refused run stops before the upload step"

sed '1s/.*/Short:        This short description is far too long for Aminet, way past forty/' "$ROOT/scripts/aminet-readme.in" > "$TMPD/long-short.in"
expect_fail "Short: longer than 40 chars: refused" 'Short: must be 1..40' -- $BASE --uploader "$UPL" --template "$TMPD/long-short.in"

sed "s/\$/$(printf '\r')/" "$ROOT/scripts/aminet-readme.in" > "$TMPD/crlf.in"   # (BSD sed has no \r)
expect_fail "CRLF template: refused" 'CR line endings' -- $BASE --uploader "$UPL" --template "$TMPD/crlf.in"

sed '1s/^Short: /Shortx /' "$ROOT/scripts/aminet-readme.in" > "$TMPD/noshort.in"
expect_fail "readme not starting with Short: refused" 'first line must be the Short: field' -- $BASE --uploader "$UPL" --template "$TMPD/noshort.in"

sed 's/^Architecture: .*/Architecture: m68k-amigaos >= 3.0.0; ppc-amigaos4/' "$ROOT/scripts/aminet-readme.in" > "$TMPD/badarch.in"
expect_fail "unknown Architecture value: refused" 'not one of m68k-amigaos' -- $BASE --uploader "$UPL" --template "$TMPD/badarch.in"

sed 's/^Distribution: .*/Distribution: Everywhere/' "$ROOT/scripts/aminet-readme.in" > "$TMPD/baddist.in"
expect_fail "Distribution: other than Aminet\/NoCD: refused" "must be 'Aminet' or 'NoCD'" -- $BASE --uploader "$UPL" --template "$TMPD/baddist.in"

sed 's/^Uploader:     @UPLOADER@$/Uploader:     nobody (Anon)/' "$ROOT/scripts/aminet-readme.in" > "$TMPD/noemail.in"
expect_fail "template that drops the uploader's e-mail: refused" 'Uploader: must carry' -- $BASE --uploader "$UPL" --template "$TMPD/noemail.in"

if [ $HAVE_LHA = 1 ]; then
    expect_fail "archive whose top directory is not amilisp-$V/: refused" 'top-level entries' -- --dry-run --archive "$TMPD/wrongtop/amilisp-$V-bin.lha" --notes "$TMPD/notes.txt" --uploader "$UPL" --out "$TMPD/out5"
    expect_fail "corrupt archive: lha t fails the run" 'lha t failed' -- --dry-run --archive "$TMPD/broken/amilisp-$V-bin.lha" --notes "$TMPD/notes.txt" --uploader "$UPL" --out "$TMPD/out6"
else
    echo "  skip  lha not installed: archive integrity refusals not exercised"
fi

echo "test_aminet_upload: $passed passed, $failed failed"
[ $failed -eq 0 ]
