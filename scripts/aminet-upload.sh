#!/bin/bash
# aminet-upload.sh — push the binary release to Aminet (dev/lang/amilisp.lha).
#
# How an Aminet upload works (wiki.aminet.net/Uploading_instructions and
# wiki.aminet.net/The_Readme_file — the web form is offline, FTP is the way):
#
#   - anonymous FTP to ftp://main.aminet.net/new/, user "anonymous", your
#     e-mail address as the password.  Moderators move what lands in /new/
#     into the tree; it is not listable, so the only receipt is the FTP
#     status of the transfer.
#   - every file is accompanied by a .readme with the SAME base name
#     (amilisp.lha + amilisp.readme).  The readme starts with header fields,
#     then an empty line, then free text.  Mandatory fields: Short:,
#     Uploader:, Type:, Architecture:.  Short: is at most 40 characters and
#     names what the program does — no version, no platform.  Lines are at
#     most 78 characters, LF-terminated, no CR.
#   - file names: at most 30 characters, letters/digits/dot/underscore/
#     hyphen, NO version number — an update is uploaded under the SAME name
#     as the existing package and overwrites it (the preferred way; the
#     Replaces: field is for removing a package under a different path).
#     Version numbers live in the Version: field.
#   - Architecture: lists only what the binaries were compiled for
#     (m68k-amigaos, ppc-morphos, ...), optionally with "arch >= version".
#     Distribution: is "Aminet" or "NoCD".  Uploader: must be a readable
#     e-mail (Aminet strips the @ and dots when publishing it).
#   - one update per week at most; every update carries a new Version: and
#     clearly marked changes.
#
# What this script does:
#   1. takes the archive scripts/make-binary-release.sh produced
#      (build/release/amilisp-<version>-bin.lha) and copies it to
#      amilisp.lha — Aminet's name for the package;
#   2. writes amilisp.readme from scripts/aminet-readme.in: @VERSION@ and
#      @UPLOADER@ are filled in, @NOTES@ becomes the release notes — the
#      file given with --notes, else the message of the annotated tag
#      v<major>.<minor> (the release headline, see CLAUDE.md).  Typographic
#      punctuation (em dash, arrows, curly quotes) is transliterated and
#      the notes are folded to 78 columns; anything still non-ASCII fails
#      the run, since the Amiga cannot display UTF-8;
#   3. checks the pair against every rule above (plus `lha t` and the
#      archive's top-level directory when lha is installed) and prints the
#      readme for review;
#   4. uploads both with curl — the archive first, the readme last — after
#      a y/N confirmation (--yes skips it).  --dry-run stops after step 3
#      and prints the curl commands it would run.
#
# Usage:
#   AMINET_UPLOADER="you@example.org (Your Name)" scripts/aminet-upload.sh [options]
#
#   --dry-run          prepare + check, print the upload commands, no network
#   --yes              upload without the confirmation prompt
#   --notes FILE       release notes for the readme (default: tag message)
#   --archive FILE     the release .lha (default: build/release/amilisp-<version>-bin.lha;
#                      the version is then taken from the file name)
#   --out DIR          where amilisp.lha + amilisp.readme are staged
#                      (default: build/release/aminet)
#   --replaces PATH    emit "Replaces: PATH": dev/lang/clamiga.lha on the
#                      first upload under the amilisp name, so Aminet
#                      retires the package the project had as CL-Amiga;
#                      not needed for a same-name update
#   --template FILE    readme template (default: scripts/aminet-readme.in)
#   --uploader "EMAIL (NAME)"   same as AMINET_UPLOADER
#
#   AMINET_FTP_URL     upload directory (default ftp://main.aminet.net/new/)
#
# Exit status 0 = staged (and uploaded, unless --dry-run); anything else =
# nothing was uploaded.  Run after the release is tagged and pushed.

set -euo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

AMINET_FTP_URL=${AMINET_FTP_URL:-ftp://main.aminet.net/new/}
AMINET_NAME=amilisp            # the package's name on Aminet (dev/lang/amilisp.lha)

DRY_RUN=0; YES=0
NOTES=""; ARCHIVE=""; OUT="$ROOT/build/release/aminet"; REPLACES=""
TEMPLATE="$ROOT/scripts/aminet-readme.in"
UPLOADER=${AMINET_UPLOADER:-}

die() { echo "ERROR: $*" >&2; exit 1; }

# The release is pushed from a developer's macOS or Linux machine; the
# readme checks lean on the POSIX text tools behaving as they do there.
case "$(uname -s)" in
    MINGW*|MSYS*|CLANGARM64*|CLANG64*|UCRT64*)
        die "aminet-upload.sh runs from macOS or Linux, not from MSYS2 / Git Bash" ;;
esac

while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run)   DRY_RUN=1 ;;
        --yes)       YES=1 ;;
        --notes)     NOTES=${2:?--notes needs a file}; shift ;;
        --archive)   ARCHIVE=${2:?--archive needs a file}; shift ;;
        --out)       OUT=${2:?--out needs a directory}; shift ;;
        --replaces)  REPLACES=${2:?--replaces needs an Aminet path}; shift ;;
        --template)  TEMPLATE=${2:?--template needs a file}; shift ;;
        --uploader)  UPLOADER=${2:?--uploader needs "EMAIL (NAME)"}; shift ;;
        -h|--help)   sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)           die "unknown option: $1 (see --help)" ;;
    esac
    shift
done

# --- version + archive ------------------------------------------------------
if [ -n "$ARCHIVE" ]; then
    # an explicit archive names its version: amilisp-<version>-bin.lha
    VERSION=$(basename "$ARCHIVE" | sed -n 's/^amilisp-\([0-9][0-9.]*\)-bin\.lha$/\1/p')
    [ -n "$VERSION" ] || die "cannot read the version from '$(basename "$ARCHIVE")' (expected amilisp-<version>-bin.lha)"
else
    ver_field() { sed -n "s/^#define CL_VERSION_$1 \([0-9][0-9]*\)$/\1/p" src/core/types.h; }
    VMAJOR=$(ver_field MAJOR); VMINOR=$(ver_field MINOR); VPATCH=$(ver_field PATCH)
    [ -n "$VMAJOR" ] && [ -n "$VMINOR" ] && [ -n "$VPATCH" ] \
        || die "could not parse the version from src/core/types.h"
    VERSION="$VMAJOR.$VMINOR.$VPATCH"
    ARCHIVE="$ROOT/build/release/amilisp-$VERSION-bin.lha"
fi
[ -f "$ARCHIVE" ] || die "release archive not found: $ARCHIVE
       run scripts/make-binary-release.sh first (it writes build/release/amilisp-$VERSION-bin.lha)"
VMAJOR=${VERSION%%.*}; rest=${VERSION#*.}; VMINOR=${rest%%.*}

# --- uploader ---------------------------------------------------------------
[ -n "$UPLOADER" ] || die "set AMINET_UPLOADER=\"you@example.org (Your Name)\" (or --uploader):
       Aminet requires a readable e-mail address in the Uploader: field"
EMAIL=$(printf '%s\n' "$UPLOADER" | grep -o -E '[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]+' | head -1 || true)
[ -n "$EMAIL" ] || die "no e-mail address in AMINET_UPLOADER '$UPLOADER' — Aminet needs one (it is also the FTP password)"

# --- release notes ----------------------------------------------------------
TMPD=$(mktemp -d "${TMPDIR:-/tmp}/aminet-upload.XXXXXX")
trap 'rm -rf "$TMPD"' EXIT
if [ -n "$NOTES" ]; then
    [ -f "$NOTES" ] || die "notes file not found: $NOTES"
    cp "$NOTES" "$TMPD/notes.raw"
else
    TAG="v$VMAJOR.$VMINOR"
    if git rev-parse -q --verify "refs/tags/$TAG" > /dev/null 2>&1; then
        git for-each-ref --format='%(contents)' "refs/tags/$TAG" > "$TMPD/notes.raw"
    fi
    [ -s "${TMPD}/notes.raw" ] 2>/dev/null \
        || die "no release notes: no annotated tag $TAG in this checkout — pass --notes FILE"
    echo "notes: message of tag $TAG"
fi

# Typographic punctuation -> ASCII (the Amiga has no UTF-8), fold to 78
# columns on word boundaries, strip trailing blanks.  Whatever is still
# non-ASCII after this is a mistake the check below names by line.
LC_ALL=C sed \
    -e $'s/\xe2\x80\x94/-/g'  -e $'s/\xe2\x80\x93/-/g' \
    -e $'s/\xe2\x86\x92/->/g' -e $'s/\xe2\x86\x90/<-/g' \
    -e $'s/\xe2\x80\x9c/"/g'  -e $'s/\xe2\x80\x9d/"/g' \
    -e $'s/\xe2\x80\x98/\'/g' -e $'s/\xe2\x80\x99/\'/g' \
    -e $'s/\xe2\x80\xa6/.../g' -e $'s/\xc2\xa0/ /g' \
    -e $'s/\r$//' "$TMPD/notes.raw" \
  | fold -s -w 78 | sed 's/[[:space:]]*$//' > "$TMPD/notes.txt"

# --- stage the pair ---------------------------------------------------------
[ -f "$TEMPLATE" ] || die "readme template not found: $TEMPLATE"
mkdir -p "$OUT"
LHA_OUT="$OUT/$AMINET_NAME.lha"
README_OUT="$OUT/$AMINET_NAME.readme"
cp "$ARCHIVE" "$LHA_OUT"
# (literal substitution via index/substr — gsub would interpret & and \ in
# the uploader's name)
awk -v ver="$VERSION" -v upl="$UPLOADER" -v repl="$REPLACES" -v notes="$TMPD/notes.txt" '
    function subst(s, pat, rep,   i) {
        while ((i = index(s, pat)) > 0)
            s = substr(s, 1, i - 1) rep substr(s, i + length(pat))
        return s
    }
    { $0 = subst($0, "@VERSION@", ver); $0 = subst($0, "@UPLOADER@", upl) }
    /^@NOTES@$/ { while ((getline l < notes) > 0) print l; next }
    # a heading underline (=== or ---) follows the length of its heading
    /^(=+|-+)$/ && length(prev) > 0 { ch = substr($0, 1, 1); $0 = sprintf("%" length(prev) "s", ""); gsub(/ /, ch, $0) }
    { prev = $0; print }
    /^Version:/ && repl != "" { printf "Replaces:     %s\n", repl }
' "$TEMPLATE" | cat -s > "$README_OUT"      # (cat -s: one blank line between notes and footer)

# --- check the pair against the Aminet rules --------------------------------
problems=0
bad() { echo "  NOT OK  $*" >&2; problems=$((problems + 1)); }
good() { echo "  ok      $*"; }

echo "--- checking $README_OUT + $(basename "$LHA_OUT") ---"
for f in "$LHA_OUT" "$README_OUT"; do
    n=$(basename "$f")
    if [ ${#n} -le 30 ] && printf '%s' "$n" | LC_ALL=C grep -q -E '^[A-Za-z0-9._-]+$'; then
        good "file name '$n' (${#n} chars, [A-Za-z0-9._-])"
    else
        bad "file name '$n' must be at most 30 characters of [A-Za-z0-9._-]"
    fi
done
[ "${LHA_OUT%.lha}" = "${README_OUT%.readme}" ] || bad "archive and readme base names differ"

# line endings and character set
if LC_ALL=C grep -q $'\r' "$README_OUT"; then
    bad "readme has CR line endings (Aminet wants LF only)"
else
    good "LF line endings only"
fi
nonascii=$(LC_ALL=C grep -n '[^ -~]' "$README_OUT" || true)
if [ -n "$nonascii" ]; then
    bad "readme has characters outside printable ASCII (UTF-8, tabs — the Amiga cannot display UTF-8):"
    printf '%s\n' "$nonascii" | head -5 | sed 's/^/            line /' >&2
else
    good "printable ASCII only"
fi
long=$(awk 'length($0) > 78 { printf "%d (%d chars)\n", NR, length($0) }' "$README_OUT")
if [ -n "$long" ]; then
    bad "readme lines longer than 78 characters:"
    printf '%s\n' "$long" | head -5 | sed 's/^/            line /' >&2
else
    good "no line longer than 78 characters"
fi

# header block: fields up to the first empty line, Short: first
hdr_end=$(awk '/^$/ { print NR; exit }' "$README_OUT")
if [ -z "$hdr_end" ]; then
    bad "no empty line terminating the header fields"
    hdr_end=$(($(wc -l < "$README_OUT") + 1))
fi
HDR="$TMPD/header"
head -n $((hdr_end - 1)) "$README_OUT" > "$HDR"
if ! head -1 "$README_OUT" | grep -q '^Short: '; then
    bad "the first line must be the Short: field"
fi
if grep -v -E '^[A-Za-z]+:[ ]' "$HDR" | grep -q .; then
    bad "header lines before the first empty line that are not 'Field: value':"
    grep -v -E '^[A-Za-z]+:[ ]' "$HDR" | head -3 | sed 's/^/            /' >&2
fi
field() { sed -n "s/^$1:[ ]*//p" "$HDR"; }
for req in Short Uploader Type Architecture; do
    c=$(grep -c "^$req: " "$HDR" || true)
    [ "$c" = 1 ] || bad "mandatory field $req: must appear exactly once in the header (found $c)"
done
short=$(field Short)
if [ -n "$short" ] && [ ${#short} -le 40 ]; then
    good "Short: '$short' (${#short} chars)"
else
    bad "Short: must be 1..40 characters, got ${#short}: '$short'"
fi
[ "$(field Version)" = "$VERSION" ] && good "Version: $VERSION" \
    || bad "Version: '$(field Version)' does not match the archive's $VERSION"
printf '%s' "$(field Uploader)" | grep -q -F "$EMAIL" && good "Uploader: $(field Uploader)" \
    || bad "Uploader: must carry the e-mail address $EMAIL"
type=$(field Type)
printf '%s' "$type" | grep -q -E '^[a-z]+/[a-z0-9]+$' && good "Type: $type" \
    || bad "Type: '$type' is not an Aminet directory (e.g. dev/lang)"
arch_ok=1
IFS=';' read -r -a archs <<< "$(field Architecture)"
for a in ${archs[@]+"${archs[@]}"}; do   # (the guard: an empty array trips set -u on bash 3.2)
    a=$(printf '%s' "$a" | sed 's/^[ ]*//; s/[ ]*$//')
    name=${a%%[ <>=]*}
    case "$name" in
        m68k-amigaos|ppc-amigaos|ppc-morphos|ppc-powerup|ppc-warpup|i386-aros|i386-amithlon|generic) ;;
        *) arch_ok=0; bad "Architecture: '$a' is not one of m68k-amigaos ppc-amigaos ppc-morphos ppc-powerup ppc-warpup i386-aros i386-amithlon generic" ;;
    esac
    mod=${a#"$name"}; mod=$(printf '%s' "$mod" | sed 's/^[ ]*//')
    if [ -n "$mod" ] && ! printf '%s' "$mod" | grep -q -E '^(>=|<=|>|<|=) *[0-9]+(\.[0-9]+)*$'; then
        arch_ok=0; bad "Architecture: '$a' — the version qualifier must look like '>= 3.0.0'"
    fi
done
[ $arch_ok = 1 ] && good "Architecture: $(field Architecture)"
dist=$(field Distribution)
case "$dist" in
    Aminet|NoCD|"") good "Distribution: ${dist:-(unset)}" ;;
    *) bad "Distribution: must be 'Aminet' or 'NoCD', got '$dist'" ;;
esac
if grep -q '@[A-Z]*@' "$README_OUT"; then
    bad "unreplaced placeholder in the readme: $(grep -o '@[A-Z]*@' "$README_OUT" | sort -u | tr '\n' ' ')"
fi
if ! grep -q -F "$VERSION" "$TMPD/notes.txt" && ! grep -q -F "$VMAJOR.$VMINOR" "$TMPD/notes.txt"; then
    echo "  note    the release notes do not mention $VMAJOR.$VMINOR"
fi

# the archive itself
if command -v lha > /dev/null 2>&1; then
    if lha t "$LHA_OUT" > "$TMPD/lha-t.log" 2>&1; then
        good "lha t: archive is sound"
    else
        bad "lha t failed on $(basename "$LHA_OUT"):"; tail -3 "$TMPD/lha-t.log" | sed 's/^/            /' >&2
    fi
    tops=$(lha l "$LHA_OUT" | awk 'NR > 2 && $NF ~ /\// { sub(/\/.*/, "", $NF); print $NF }' | sort -u | tr '\n' ' ')
    if [ "$tops" = "amilisp-$VERSION " ]; then
        good "archive unpacks into amilisp-$VERSION/"
    else
        bad "archive top-level entries are '$tops', expected only amilisp-$VERSION/"
    fi
else
    echo "  note    lha not installed — archive integrity not checked here"
fi

if [ $problems -gt 0 ]; then
    echo "ERROR: $problems problem(s) — nothing uploaded; fix scripts/aminet-readme.in / the notes and rerun" >&2
    exit 1
fi

echo
echo "--- $README_OUT ---"
cat "$README_OUT"
echo "--- end of readme ($(wc -l < "$README_OUT" | tr -d ' ') lines) ---"
echo
ls -l "$LHA_OUT" "$README_OUT"
echo

# --- upload -----------------------------------------------------------------
CURL_ARCHIVE=(curl --fail --show-error --silent --upload-file "$LHA_OUT" --user "anonymous:$EMAIL" "$AMINET_FTP_URL")
CURL_README=(curl --fail --show-error --silent --upload-file "$README_OUT" --user "anonymous:$EMAIL" "$AMINET_FTP_URL")
if [ $DRY_RUN = 1 ]; then
    echo "DRY RUN — would upload to $AMINET_FTP_URL (anonymous, password $EMAIL):"
    printf '  '; printf '%q ' "${CURL_ARCHIVE[@]}"; echo
    printf '  '; printf '%q ' "${CURL_README[@]}"; echo
    exit 0
fi
if [ $YES != 1 ]; then
    printf 'Upload %s (%s bytes) + %s to %s as %s? [y/N] ' \
        "$(basename "$LHA_OUT")" "$(wc -c < "$LHA_OUT" | tr -d ' ')" "$(basename "$README_OUT")" \
        "$AMINET_FTP_URL" "$EMAIL"
    read -r answer
    case "$answer" in y|Y|yes|YES) ;; *) echo "aborted, nothing uploaded"; exit 2 ;; esac
fi
echo "--- uploading $(basename "$LHA_OUT") ---"
"${CURL_ARCHIVE[@]}" || die "archive upload failed — nothing to moderate yet, rerun when the connection is back"
echo "--- uploading $(basename "$README_OUT") ---"
"${CURL_README[@]}" || die "readme upload failed — the archive is in /new/ without its readme: rerun the script (same names overwrite)"
echo "=== uploaded amilisp.lha + amilisp.readme ($VERSION) to $AMINET_FTP_URL ==="
echo "The Aminet moderators move it into dev/lang/; the entry then updates at"
echo "https://aminet.net/package/dev/lang/amilisp (and the recent list). Usually a day or two."
