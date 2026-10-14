#!/bin/sh
# package-symbols.sh — keep docs/*.md package symbol lists in sync with reality.
#
# Usage:
#   package-symbols.sh generate <amilisp-binary>   # print canonical snapshot to stdout
#   package-symbols.sh check    <amilisp-binary>   # diff against committed snapshot; exit 1 on drift
#
# The canonical snapshot (docs/package-symbols.txt) lists, as sorted
# "PACKAGE|SYMBOL" lines, the external symbols of every documented extension
# package EXCEPT CLAMIGA:
#   - EXT, MP, FFI, GRAY, MOP   -> queried live from the running image
#   - AMIGA, AMIGA.FFI/INTUITION/GFX/GADTOOLS/REACTION/... -> parsed from source
#       (these packages don't exist in the host build)
#
# CLAMIGA is handled separately: it exports ~224 internal %/*-prefixed helpers
# that we intentionally do not document, so an exact diff would be pure noise.
# Instead we verify that every symbol we DO document
# (docs/clamiga-documented-symbols.txt) still exists as a live CLAMIGA export.

set -e

MODE="$1"
CLAMIGA_BIN="$2"

ROOT=$(CDPATH= cd "$(dirname "$0")/../.." && pwd)
DUMP_LISP="$ROOT/tools/docs/dump-package-symbols.lisp"
SNAPSHOT="$ROOT/docs/package-symbols.txt"
CLAMIGA_DOC_LIST="$ROOT/docs/clamiga-documented-symbols.txt"

if [ -z "$MODE" ] || [ -z "$CLAMIGA_BIN" ]; then
    echo "usage: $0 {generate|check} <amilisp-binary>" >&2
    exit 2
fi
if [ ! -x "$CLAMIGA_BIN" ]; then
    echo "package-symbols.sh: amilisp binary not found: $CLAMIGA_BIN" >&2
    exit 2
fi

# --- live host packages (EXT MP FFI GRAY MOP CLAMIGA) ----------------------
live_dump() {
    # Filter to valid "PACKAGE|SYMBOL" lines — the binary may also emit boot
    # trace ("; [boot] ...") and other diagnostics to stdout.
    CLAMIGA_NO_USERINIT=1 "$CLAMIGA_BIN" --non-interactive --load "$DUMP_LISP" \
        | grep -E '^(EXT|MP|FFI|GRAY|MOP|CLAMIGA)\|'
}

# --- AMIGA package exports parsed from C source ----------------------------
amiga_from_source() {
    # amiga_defun("NAME", ...) plus any explicitly exported AMIGA symbols.
    grep -oE 'amiga_defun\("[^"]+"' "$ROOT/src/core/builtins_amiga.c" \
        | grep -oE '"[^"]+"' | tr -d '"' | while read -r n; do echo "AMIGA|$n"; done
    grep -oE 'cl_export_symbol\(cl_intern_in\("[^"]+", *[0-9]+, *cl_package_amiga' \
        "$ROOT/src/core/builtins_amiga.c" \
        | grep -oE '"[^"]+"' | tr -d '"' | while read -r n; do echo "AMIGA|$n"; done
}

# --- AMIGA.* package exports parsed from the defpackage :export forms ------
amiga_lib_exports() {
    pkg="$1"; file="$2"
    # Capture every "QUOTED" token from the (:export ...) clause to the
    # closing )) of the defpackage.  :use comes before :export in every file.
    awk -v pkg="$pkg" '
        /\(:export/ { e=1 }
        e {
            s=$0
            while (match(s, /"[^"]+"/)) {
                tok=substr(s, RSTART+1, RLENGTH-2)
                print pkg "|" tok
                s=substr(s, RSTART+RLENGTH)
            }
        }
        e && /\)\)/ { exit }
    ' "$file"
}

generate() {
    {
        # Live packages, minus CLAMIGA (tracked via the curated list instead).
        live_dump | grep -v '^CLAMIGA|'
        amiga_from_source
        amiga_lib_exports "AMIGA.FFI"       "$ROOT/lib/amiga/ffi.lisp"
        amiga_lib_exports "AMIGA.INTUITION" "$ROOT/lib/amiga/intuition.lisp"
        amiga_lib_exports "AMIGA.GFX"       "$ROOT/lib/amiga/graphics.lisp"
        amiga_lib_exports "AMIGA.GADTOOLS"  "$ROOT/lib/amiga/gadtools.lisp"
        amiga_lib_exports "AMIGA.BOOPSI"    "$ROOT/lib/amiga/boopsi.lisp"
        amiga_lib_exports "AMIGA.REACTION"  "$ROOT/lib/amiga/reaction.lisp"
        amiga_lib_exports "AMIGA.MUI"       "$ROOT/lib/amiga/mui.lisp"
        amiga_lib_exports "AMIGA.EXEC"      "$ROOT/lib/amiga/exec.lisp"
        amiga_lib_exports "AMIGA.AUDIO"     "$ROOT/lib/amiga/audio.lisp"
        amiga_lib_exports "AMIGA.AREXX"     "$ROOT/lib/amiga/arexx.lisp"
        # EXT.DEV is portable (it loads on the host too) but, like the AMIGA.*
        # packages, it only exists after a REQUIRE — so it is parsed from the
        # defpackage rather than dumped from a bare image.
        amiga_lib_exports "EXT.DEV"         "$ROOT/lib/dev-commands.lisp"
    } | LC_ALL=C sort -u
}

check_clamiga_documented() {
    # Every documented CLAMIGA symbol must still be a live export.
    [ -f "$CLAMIGA_DOC_LIST" ] || return 0
    live_clamiga=$(live_dump | grep '^CLAMIGA|' | sed 's/^CLAMIGA|//')
    missing=""
    # strip comments/blank lines from the curated list
    sed 's/#.*//' "$CLAMIGA_DOC_LIST" | tr -d ' \t' | while read -r sym; do
        [ -z "$sym" ] && continue
        if ! printf '%s\n' "$live_clamiga" | grep -qxF "$sym"; then
            echo "  CLAMIGA|$sym (in docs/clamiga.md, no longer exported)"
        fi
    done
}

case "$MODE" in
    generate)
        generate
        ;;
    check)
        tmp=$(mktemp)
        trap 'rm -f "$tmp"' EXIT
        generate > "$tmp"
        rc=0
        if [ ! -f "$SNAPSHOT" ]; then
            echo "docs-check: missing $SNAPSHOT — run 'make docs-update'" >&2
            rc=1
        elif ! diff -u "$SNAPSHOT" "$tmp" > /tmp/docs-symdiff.$$ 2>&1; then
            echo "docs-check: package exports drifted from docs/package-symbols.txt:" >&2
            echo "  (- = in snapshot, + = live; lines are PACKAGE|SYMBOL)" >&2
            sed -n '3,$p' /tmp/docs-symdiff.$$ | grep -E '^[+-]' >&2 || true
            echo "" >&2
            echo "  -> update the affected docs/*.md, then run 'make docs-update'." >&2
            rm -f /tmp/docs-symdiff.$$
            rc=1
        fi
        clamiga_missing=$(check_clamiga_documented)
        if [ -n "$clamiga_missing" ]; then
            echo "docs-check: docs/clamiga.md documents symbols that are no longer exported:" >&2
            echo "$clamiga_missing" >&2
            rc=1
        fi
        if [ "$rc" -eq 0 ]; then
            echo "docs-check: package symbol lists are in sync."
        fi
        exit "$rc"
        ;;
    *)
        echo "usage: $0 {generate|check} <amilisp-binary>" >&2
        exit 2
        ;;
esac
