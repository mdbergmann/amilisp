#!/bin/sh
# tools/docs/md2guide.lisp -- the Markdown to AmigaGuide converter
# (specs/amigaguide-docs.md).
#
# Covered:
#   golden fixture: tests/md2guide/fixture.md + other.md + sub/child.md
#     convert to files byte-identical to the committed fixture.guide /
#     other.guide / sub/child.guide (every supported construct, every link
#     form, every mapped character, Latin-1 pass-through, cross-file links
#     in both directions and across a directory level: down as
#     `sub/child.guide/node`, up as `/fixture.guide/node` -- the AmigaDOS
#     parent -- with the output directory created by the converter)
#   error fixtures: each file under tests/md2guide/errors/ fails with a
#     `file:line:` diagnostic naming the construct, exit status 1, and no
#     output file is written
#   the real docs: tools/docs/md2guide.sh converts README-FIRST.md,
#     README.md and docs/*.md (plus clamacs/README.md when the submodule is
#     present) with zero diagnostics into the release layout -- the root
#     guides at the top, the reference under docs/ -- the drift gate: a
#     construct the converter does not handle, or a dangling link, in the
#     shipped documentation fails here
#   structural checks over that output, independent of the converter:
#     @DATABASE first (with the bare file name), @NODE/@ENDNODE balance,
#     every LINK target names an @NODE of the right file resolved relative
#     to the linking guide's directory, no byte above 0x7F other than the
#     Latin-1 pass-through set, no unescaped @ outside a command, no
#     unindented body line wider than 76 visible columns, the @$VER: line
#     carries the version from src/core/types.h, and the pages link across
#     the layout: README-FIRST to the manual/editor/index, the manual into
#     docs/, the AMIGA.* reference back up to the manual
#
# Run: sh tests/test_md2guide.sh build/host/amilisp

AMILISP="${1:-build/host/amilisp}"
ROOT=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1
. tests/shpath.sh

passed=0
failed=0
total=0
ok()   { total=$((total + 1)); passed=$((passed + 1)); echo "  ok  $1"; }
fail() { total=$((total + 1)); failed=$((failed + 1)); echo "  FAIL  $1"; shift; for l in "$@"; do echo "    $l"; done; }

WORK=$(mktemp -d "${TMPDIR:-/tmp}/clamiga_md2guide_XXXXXX")
trap 'rm -rf "$WORK"' EXIT INT TERM
WORK_NATIVE=$(native_path "$WORK")

run_convert() {
    # run_convert <inputs-as-lisp-alist> <output-dir>  -> stdout+stderr, rc in $rc
    AMILISP_NO_USERINIT=1 "$AMILISP" --no-userinit --non-interactive \
        --load tools/docs/md2guide.lisp \
        --eval "(md2guide:cli '($1) \"$2/\" \"0.0.0\" \"01.01.2000\")" </dev/null 2>&1
}

# --- golden fixture -------------------------------------------------------
mkdir -p "$WORK/fx"
# sub/ is not created here: the converter creates the output directories
out=$(run_convert '("tests/md2guide/fixture.md" . "fixture.guide") ("tests/md2guide/other.md" . "other.guide") ("tests/md2guide/sub/child.md" . "sub/child.guide")' "$WORK_NATIVE/fx")
rc=$?
if [ "$rc" -eq 0 ]; then ok "fixture_converts"; else fail "fixture_converts" "$out"; fi
for g in fixture other sub/child; do
    if [ -f "$WORK/fx/$g.guide" ] && cmp -s "$WORK/fx/$g.guide" "tests/md2guide/$g.guide"; then
        ok "golden_$g"
    else
        fail "golden_$g" "output differs from tests/md2guide/$g.guide:" \
             "$(diff "tests/md2guide/$g.guide" "$WORK/fx/$g.guide" 2>&1 | head -20)"
    fi
done

# --- error fixtures -------------------------------------------------------
expect_error() {
    # expect_error <name> <line> <message-fragment>
    name=$1; line=$2; frag=$3
    rm -rf "$WORK/err"; mkdir -p "$WORK/err"
    out=$(run_convert "(\"tests/md2guide/errors/$name.md\" . \"$name.guide\")" "$WORK_NATIVE/err")
    rc=$?
    if [ "$rc" -ne 1 ]; then
        fail "error_${name}_status" "expected exit 1, got $rc" "$out"
    elif ! printf '%s\n' "$out" | grep -q "tests/md2guide/errors/$name.md:$line: .*$frag"; then
        fail "error_${name}_diagnostic" "expected '$name.md:$line: ...$frag', got:" "$out"
    elif [ -n "$(ls "$WORK/err")" ]; then
        fail "error_${name}_no_output" "an output file was written despite the error"
    else
        ok "error_$name"
    fi
}
expect_error setext              4 "setext heading"
expect_error refstyle-link       3 "reference-style link"
expect_error html-block          3 "HTML block"
expect_error dangling-link       5 "dangling link: #nowhere"
expect_error unmapped-char       3 "unmapped character U+2603"
expect_error indented-code       5 "indented code block"
expect_error hard-break          3 "hard line break"
expect_error unmatched-backtick  3 "unmatched backtick"
expect_error lazy-list           4 "lazy continuation"
expect_error no-title            1 "level-1 heading"
expect_error table-cells         5 "table row has 3 cells, the header has 2"
expect_error unterminated-fence  3 "unterminated code fence"

# --- the real docs, through the wrapper ------------------------------------
# Not under GC stress: converting the 2000-line README with a compaction
# before every allocation takes a quarter of an hour, and the fixture set
# above already drives every allocating path of the converter.
if [ "${AMILISP_GC_STRESS:-0}" = 1 ]; then
    echo "  skip  real_docs (AMILISP_GC_STRESS=1: fixture set only)"
    echo ""
    echo "$passed passed, $failed failed, $total total"
    if [ "$failed" -gt 0 ]; then echo "FAIL"; exit 1; else echo "PASS"; exit 0; fi
fi
out=$(sh tools/docs/md2guide.sh "$AMILISP" "$WORK/rel" 2>&1)
rc=$?
if [ "$rc" -eq 0 ]; then ok "real_docs_convert"; else fail "real_docs_convert" "$out"; fi
# the release layout: the root guides at the top, the reference under docs/
for g in README-FIRST amilisp docs/README docs/ext docs/mp docs/ffi docs/gray docs/mop docs/amilisp docs/amiga; do
    [ -s "$WORK/rel/$g.guide" ] || fail "real_docs_present_$g" "missing $WORK/rel/$g.guide"
done
if [ -f clamacs/README.md ]; then
    [ -s "$WORK/rel/clamacs.guide" ] && ok "real_docs_clamacs" || fail "real_docs_clamacs" "no clamacs.guide although clamacs/README.md exists"
fi
stray=$(cd "$WORK/rel" && find . -name '*.guide' | grep -v '^\./\(docs/\)\{0,1\}[^/]*\.guide$')
[ -z "$stray" ] && ok "real_docs_layout" || fail "real_docs_layout" "guides outside the root and docs/: $stray"
total=$((total + 1)); passed=$((passed + 1)); echo "  ok  real_docs_present"

# --- structural checks (LC_ALL=C: the files carry Latin-1 bytes) -----------
ver_field() { sed -n "s/^#define CL_VERSION_$1 \([0-9][0-9]*\)$/\1/p" src/core/types.h; }
VERSION="$(ver_field MAJOR).$(ver_field MINOR).$(ver_field PATCH)"
VDATE=$(sed -n 's/^#define CL_VERSION_DATE "\([^"]*\)"$/\1/p' src/core/types.h)
export LC_ALL=C
cd "$WORK/rel" || exit 1
bad=""
for f in *.guide docs/*.guide; do
    base=${f##*/}
    [ "$(head -c 10 "$f")" = "@DATABASE " ] || bad="$bad $f:no-@DATABASE"
    [ "$(sed -n 1p "$f")" = "@DATABASE $base" ] || bad="$bad $f:@DATABASE-not-bare-name"
    [ "$(sed -n 2p "$f")" = "@\$VER: $base $VERSION ($VDATE)" ] || bad="$bad $f:bad-\$VER"
    [ "$(grep -c '^@NODE ' "$f")" -eq "$(grep -c '^@ENDNODE$' "$f")" ] || bad="$bad $f:node-balance"
    # every LINK "node" / LINK "path/file.guide/node" names an @NODE of that
    # file, the path taken relative to this guide's directory the way
    # amigaguide.library takes it: "docs/x.guide" is below, "/x.guide" is in
    # the parent (each leading slash one step up)
    grep -o 'LINK "[^"]*"' "$f" | sed 's/LINK "//; s/"$//' | sort -u | while read -r t; do
        case "$t" in
            */*) g=${t%/*}; n=${t##*/} ;;
            *)   g=$base; n=$t ;;
        esac
        d=$(dirname "$f")
        while [ "${g#/}" != "$g" ]; do g=${g#/}; d=$(dirname "$d"); done
        [ "$d" = "." ] && p=$g || p="$d/$g"
        grep -q "^@NODE $n " "$p" 2>/dev/null || echo "$f: dangling LINK $t"
    done > "$WORK/links.$base"
    [ -s "$WORK/links.$base" ] && bad="$bad $f:$(head -1 "$WORK/links.$base")"
    # an @ that is neither escaped nor a command
    if grep -v '^@\(DATABASE\|\$VER\|AUTHOR\|(C)\|WIDTH\|REM\|NODE\|ENDNODE\)' "$f" \
        | sed 's/\\\\//g; s/\\@//g; s/@{[^}]*}//g' | grep -q '@'; then
        bad="$bad $f:unescaped-@"
    fi
    # bytes above 0x7F: only the Latin-1 characters the docs use (§ ×, umlauts)
    if tr -d '\000-\177\247\327\300-\377' < "$f" | grep -q .; then
        bad="$bad $f:non-latin1-byte"
    fi
    # unindented body lines (paragraphs, list items, table entries, titles)
    # fit 76 visible columns; commands and code lines are exempt
    wide=$(grep -v '^@' "$f" | grep -v '^ ' | sed 's/@{[^}]*}//g; s/\\@/@/g; s/\\\\/\\/g' \
           | awk 'length > 76 { n++ } END { print n + 0 }')
    [ "$wide" -eq 0 ] || bad="$bad $f:$wide-lines-wider-than-76"
done
cd "$ROOT" || exit 1
if [ -z "$bad" ]; then ok "structure"; else fail "structure" "$bad"; fi
# the index page links to every reference page (siblings: bare names)
bad=""
for g in ext mp ffi gray mop amilisp amiga; do
    grep -q "LINK \"$g.guide/main\"" "$WORK/rel/docs/README.guide" || bad="$bad README.guide:no-link-to-$g"
done
[ -z "$bad" ] && ok "index_links_every_page" || fail "index_links_every_page" "$bad"
# the manual (root) links down into the reference (docs/)
grep -q 'LINK "docs/ext.guide/' "$WORK/rel/amilisp.guide" && ok "manual_links_reference" || fail "manual_links_reference"
# the AMIGA.* reference (docs/) links up to the manual (root): AmigaDOS parent
grep -q 'LINK "/amilisp.guide/' "$WORK/rel/docs/amiga.guide" && ok "reference_links_manual" || fail "reference_links_manual"
# the getting-started page links to the manual, the index and (when the
# submodule is present) the editor's guide, all by their release paths
bad=""
grep -q 'LINK "amilisp.guide/main"' "$WORK/rel/README-FIRST.guide" || bad="$bad no-link-to-manual"
grep -q 'LINK "docs/README.guide/main"' "$WORK/rel/README-FIRST.guide" || bad="$bad no-link-to-index"
grep -q 'LINK "docs/ext.guide/heap-images"' "$WORK/rel/README-FIRST.guide" || bad="$bad no-link-to-ext-heap-images"
grep -q 'LINK "amilisp.guide/arexx-port-amigaos--morphos"' "$WORK/rel/README-FIRST.guide" || bad="$bad no-link-to-arexx-section"
if [ -f clamacs/README.md ]; then
    grep -q 'LINK "clamacs.guide/main"' "$WORK/rel/README-FIRST.guide" || bad="$bad no-link-to-clamacs"
fi
[ -z "$bad" ] && ok "readme_first_links" || fail "readme_first_links" "$bad"

echo ""
echo "$passed passed, $failed failed, $total total"
if [ "$failed" -gt 0 ]; then echo "FAIL"; exit 1; else echo "PASS"; exit 0; fi
