#!/bin/sh
# md2guide.sh — build the AmigaGuide documentation from the Markdown sources.
#
# Usage:
#   md2guide.sh <amilisp-binary> <output-dir>
#
# Converts the documentation the binary release ships into one .guide per
# .md under <output-dir>, laid out as the release root is: README-FIRST.guide,
# amilisp.guide (README.md) and clamacs.guide (the editor's README, when the
# clamacs/ submodule is checked out) at the top, next to the Workbench icons,
# and the package reference under docs/.  tools/docs/md2guide.lisp does the
# work, run by the given amilisp; the version and date stamped into every
# @$VER: line come from src/core/types.h, the single source of truth for the
# version.
#
# Called by `make guide` (into build/guide/), by scripts/make-binary-release.sh
# (into the staged release root), and by tests/test_md2guide.sh.  Exit status
# 1 with a `file:line: message` diagnostic when a source uses Markdown the
# converter does not handle, or contains a dangling link -- see
# specs/amigaguide-docs.md.

set -e

AMILISP_BIN="$1"
OUT="$2"
ROOT=$(CDPATH= cd "$(dirname "$0")/../.." && pwd)

if [ -z "$AMILISP_BIN" ] || [ -z "$OUT" ]; then
    echo "usage: $0 <amilisp-binary> <output-dir>" >&2
    exit 2
fi
if [ ! -x "$AMILISP_BIN" ]; then
    echo "md2guide.sh: amilisp binary not found: $AMILISP_BIN" >&2
    exit 2
fi

ver_field() { sed -n "s/^#define CL_VERSION_$1 \([0-9][0-9]*\)$/\1/p" "$ROOT/src/core/types.h"; }
VERSION="$(ver_field MAJOR).$(ver_field MINOR).$(ver_field PATCH)"
DATE=$(sed -n 's/^#define CL_VERSION_DATE "\([^"]*\)"$/\1/p' "$ROOT/src/core/types.h")
case "$VERSION" in
    *..*|.*|*.) echo "md2guide.sh: could not parse the version from src/core/types.h" >&2; exit 1 ;;
esac
[ -n "$DATE" ] || { echo "md2guide.sh: could not parse CL_VERSION_DATE from src/core/types.h" >&2; exit 1; }

mkdir -p "$OUT"
# Absolute, in the spelling amilisp uses (tests/shpath.sh: on an MSYS2 host
# the shell's path is not the native binary's path).
. "$ROOT/tests/shpath.sh"
OUT_NATIVE=$(native_path "$(CDPATH= cd "$OUT" && pwd)")

INPUTS='("README-FIRST.md" . "README-FIRST.guide")
        ("README.md" . "amilisp.guide")
        ("docs/README.md" . "docs/README.guide")
        ("docs/ext.md" . "docs/ext.guide")
        ("docs/mp.md" . "docs/mp.guide")
        ("docs/ffi.md" . "docs/ffi.guide")
        ("docs/gray.md" . "docs/gray.guide")
        ("docs/mop.md" . "docs/mop.guide")
        ("docs/amilisp.md" . "docs/amilisp.guide")
        ("docs/amiga.md" . "docs/amiga.guide")'
if [ -f "$ROOT/clamacs/README.md" ]; then
    INPUTS="$INPUTS
        (\"clamacs/README.md\" . \"clamacs.guide\")"
else
    echo "md2guide.sh: note: clamacs/ submodule not checked out, no clamacs.guide" >&2
fi

cd "$ROOT"
AMILISP_NO_USERINIT=1 "$AMILISP_BIN" --no-userinit --non-interactive \
    --load tools/docs/md2guide.lisp \
    --eval "(md2guide:cli '($INPUTS) \"$OUT_NATIVE/\" \"$VERSION\" \"$DATE\")" </dev/null
