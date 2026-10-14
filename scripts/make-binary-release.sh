#!/bin/bash
# make-binary-release.sh — assemble the AmigaOS/MorphOS binary release.
#
# Produces build/release/amilisp-<version>/ and .zip/.lha archives:
#
#   amilisp-<version>/
#     bin/aos3/amilisp      AmigaOS 3+ (68020+) soft-float binary, runs on
#                           any CPU — cross-compiled here
#     bin/aos3-fpu/amilisp  AmigaOS 3+ hard-float binary (Makefile.cross
#                           FPU=1, -m68881) — REQUIRES an FPU (68881/68882,
#                           68040/68060, Vampire/PiStorm)
#     bin/mos/amilisp       MorphOS (PPC) binary, built natively on MorphOS
#     bin/*/amilisp.img     bare-boot heap image (boot + CLOS) beside EACH
#                           binary: startup restores it in one read instead
#                           of loading lib/boot.fasl + clos.fasl (see below)
#     bin/*/clamacs.img     Clamacs, the editor/IDE (clamacs/ submodule,
#                           written in Lisp), as a heap image beside EACH
#                           binary: the Clamacs launcher starts
#                           `amilisp --image clamacs.img --eval "(clamacs::run)"`.
#                           Saved by the staged m68k binaries in FS-UAE like
#                           amilisp.img, natively on MorphOS (CLAMACS_MOS_IMG).
#     lib/clamacs/          the editor's sources plus FASLs, and its two image
#                           scripts: what `--no-image` (or a refused image)
#                           starts the editor from
#     lib/                  runtime library — FASLs where portable, sources
#                           where compilation must happen on the target
#     docs/                 package API reference (signatures + descriptions)
#                           as Markdown AND as AmigaGuide (*.guide, generated
#                           here by tools/docs/md2guide.sh -- readable on the
#                           Amiga with MultiView)
#     examples/             example programs, as Lisp source
#     README-FIRST.guide    the getting-started page (README-FIRST.md), the
#     amilisp.guide        manual (README.md) and the editor's README
#     clamacs.guide         (docs/clamacs.md), as AmigaGuide in the package
#                           root next to the Workbench icons
#     *.guide.info          every guide has an icon (default tool MultiView),
#                           root and docs/
#     AmiLisp AmiLisp-FPU AmiLisp-MOS Clamacs Clamacs-FPU Clamacs-MOS (+ .info)
#                           Workbench launcher icons and their IconX scripts,
#                           one pair per binary (the MOS pair only with bin/mos)
#     README-FIRST.md README.md LICENSE
#   amilisp-<version>.info  beside the drawer in the archive: the drawer's own
#                           icon, whose window shows the root as a 3 x 3 grid
#                           (amilisp launchers, Clamacs launchers, guides --
#                           the root icons carry fixed positions) and only files with icons
#
# lib/ packaging policy (correctness, not preference):
#   FASL   boot clos ffi gray-streams dev-commands dev-repl dev-tcp
#          — self-contained, no reader conditionals / compile-time feature
#          detection, so a host-compiled FASL is portable (FASLs are
#          arch/endian-neutral; boot.fasl + clos.fasl have shipped this way
#          all along).  dev-commands + dev-repl are the EXT.DEV command
#          layer behind the ARexx development port: amiga/arexx REQUIREs
#          them, so without them Clamacs cannot connect to a released
#          amilisp at all; dev-tcp is the same port over TCP (what a
#          host Clamacs drives an Amiga amilisp through).
#   FASL + SOURCE
#          amiga/**       — the curated modules, AMIGA.REACTION and the
#                           generated raw OS bindings (lib/amiga/raw/**): no
#                           reader conditionals, all platform variance is
#                           load-time ((member :morphos *features*),
#                           (%version>= n)) and every struct layout is an
#                           explicit 32-bit offset, so ONE host-compiled FASL
#                           serves aos3, aos3-fpu and MorphOS.  Precompiled
#                           because the alternative is compiling ~1 MB of
#                           generated source on a 68020 at first REQUIRE
#                           (minutes, plus compile heap).  The sources ship
#                           too, as the readable API reference and as the
#                           fallback REQUIRE takes if a FASL is ever rejected;
#                           the FASLs are written AFTER the sources are
#                           copied, so REQUIRE's "FASL at least as new as the
#                           source" rule picks the FASL.
#   SOURCE asdf.lisp      — uiop's (detect-os) runs at compile time and bakes
#                           :os-unix branches of the *compiling* host in.
#          quicklisp.lisp — contains #+amigaos reader conditionals.
#          quicklisp-compat.lisp, quicklisp-install.lisp — reference
#                           quicklisp packages that don't exist at host
#                           compile time (macros would compile wrong).
#          shims/         — the cl+ssl facade and swank stub ASDF systems.
#                           lib/asdf.lisp auto-registers them on
#                           ASDF:*CENTRAL-REGISTRY* (searched before the
#                           Quicklisp/ocicl searchers), so they shadow any
#                           package-manager copy with no installation step.
#   Source-shipped files compile on the target on first (require ...) and are
#   cached under S:amilisp/faslcache/, so the cost is paid once.
#
#   Every FASL is compiled with AMILISP_FASL_PORTABLE=1: the m68k AmigaOS
#   binaries are byte-string builds with no decoder for FASL_TAG_WIDE_STRING
#   (MorphOS is a wide build but loads the same FASLs), so a
#   non-ASCII string literal in a shipped module fails the build HERE with
#   the source line, not on the Amiga with BAD_TAG at REQUIRE time
#   (tests/test_lib_fasl_portable.sh runs the same compile in `make test`).
#
# Heap images (specs/image-save-load.md): each binary starts from the
# amilisp.img beside it — a snapshot of the booted runtime (boot + CLOS),
# restored in one read instead of loading the two FASLs form by form.
#   - One image PER BINARY, next to it.  Images are per-build (a fingerprint
#     ties them to version/format/FPU/platform) and PROGDIR: is the first
#     executable-relative place startup looks.  A single image at the
#     release root would be found via the cwd by all three binaries and
#     refused by two of them on every start.
#   - The host cannot write them the way it writes FASLs.  The aos3 pair is
#     saved by the staged m68k binaries themselves, unattended in FS-UAE
#     (verify/realamiga/make-image.sh against the staged layout, so each
#     image is dumped from the very FASLs that ship, then restarted from the
#     release root to verify).  The MorphOS one is saved natively
#     (`make -f Makefile.mos image`) and passed in as MOS_IMG.
#   - boot.fasl + clos.fasl still ship: they are what --no-image boots from
#     and the fallback when an image is refused.
#
# The binaries sit two directory levels below the release root on purpose:
# both the boot search (repl.c) and REQUIRE resolve lib/ via the
# executable-ancestor fallback (PROGDIR: two levels up), so the release runs
# from any current directory without assigns or environment variables.
#
# Usage:
#   scripts/make-binary-release.sh [--no-smoke] [--snapshot [--with-mos]]
#
#   --snapshot     AmigaOS-only development snapshot of the working tree:
#                  no bin/mos/ (no MorphOS inputs needed), staged and
#                  archived as amilisp-<version>-snapshot-<git short sha>
#                  so it never masquerades as, or overwrites, a release.
#                  Everything else (FASLs, heap images, smoke test) is the
#                  release procedure.
#   --with-mos     With --snapshot: package bin/mos/ after all, from the
#                  MOS_BIN / MOS_IMG / CLAMACS_MOS_IMG inputs below (built
#                  on MorphOS from the snapshot's commit).  A full release
#                  always includes it.
#
#   MOS_BIN=path   MorphOS binary to package (default: ./amilisp-mos).
#                  There is no MorphOS cross toolchain here — build it
#                  natively with Makefile.mos and copy it over.
#   MOS_IMG=path   Its bare-boot heap image (default: ./amilisp-mos.img),
#                  saved on MorphOS by THAT binary from the same source
#                  tree: `make -f Makefile.mos image` writes
#                  build/morphos/amilisp.img (and verifies it).
#   CLAMACS_MOS_IMG=path
#                  The MorphOS Clamacs heap image (default: ./clamacs-mos.img),
#                  saved on MorphOS by the MOS_BIN binary from the same source
#                  tree: `make -f Makefile.mos editor-image` writes
#                  build/morphos/clamacs.img (and verifies it).
#
# The aos3 images need the FS-UAE setup of `make -f Makefile.cross
# test-amiga` (pkill fs-uae first if an emulator is lingering; the editor
# image also needs the MUI + TextEditor.mcc on that Workbench).  The editor
# needs the clamacs/ submodule checked out: `git submodule update --init
# clamacs`.  No toolchain: it is Lisp, compiled by the host binary.

set -euo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

SMOKE=1
SNAPSHOT=0
WITH_MOS=0
for arg in "$@"; do
    case "$arg" in
        --no-smoke) SMOKE=0 ;;
        --snapshot) SNAPSHOT=1 ;;
        --with-mos) WITH_MOS=1 ;;
        *) echo "usage: $0 [--no-smoke] [--snapshot [--with-mos]]" >&2; exit 2 ;;
    esac
done
# A release always ships MorphOS; a snapshot only when asked to.
[ "$SNAPSHOT" = 1 ] || WITH_MOS=1

# macOS ships no `timeout`; prefer coreutils' if present, else run unguarded.
if command -v timeout > /dev/null 2>&1; then TIMEOUT="timeout 300"
elif command -v gtimeout > /dev/null 2>&1; then TIMEOUT="gtimeout 300"
else TIMEOUT=""; fi

MOS_BIN=${MOS_BIN:-$ROOT/amilisp-mos}
MOS_IMG=${MOS_IMG:-$ROOT/amilisp-mos.img}
CLAMACS_MOS_IMG=${CLAMACS_MOS_IMG:-$ROOT/clamacs-mos.img}
FSUAE_BIN=verify/realamiga/FS-UAE.app/Contents/MacOS/fs-uae

# --- version from the single source of truth ------------------------------
ver_field() { sed -n "s/^#define CL_VERSION_$1 \([0-9][0-9]*\)$/\1/p" src/core/types.h; }
VMAJOR=$(ver_field MAJOR); VMINOR=$(ver_field MINOR); VPATCH=$(ver_field PATCH)
VERSION="$VMAJOR.$VMINOR.$VPATCH"
[ -n "$VMAJOR" ] && [ -n "$VMINOR" ] && [ -n "$VPATCH" ] || {
    echo "ERROR: could not parse version from src/core/types.h" >&2; exit 1; }

REL="amilisp-$VERSION"
SNAPSHOT_ID=""
if [ "$SNAPSHOT" = 1 ]; then
    SNAPSHOT_ID="$(git rev-parse --short HEAD)"
    git diff --quiet HEAD 2>/dev/null || SNAPSHOT_ID="$SNAPSHOT_ID-dirty"
    REL="$REL-snapshot-$SNAPSHOT_ID"
fi
OUT="$ROOT/build/release"
STAGE="$OUT/$REL"

if [ "$SNAPSHOT" = 1 ] && [ "$WITH_MOS" = 1 ]; then
    echo "=== AmiLisp AmigaOS + MorphOS snapshot $VERSION ($SNAPSHOT_ID) ==="
elif [ "$SNAPSHOT" = 1 ]; then
    echo "=== AmiLisp AmigaOS snapshot $VERSION ($SNAPSHOT_ID) ==="
else
    echo "=== AmiLisp binary release $VERSION ==="
fi

# --- inputs ---------------------------------------------------------------
if [ "$WITH_MOS" = 0 ]; then
    : # no MorphOS inputs in an AmigaOS-only snapshot
elif [ ! -f "$MOS_BIN" ]; then
    echo "ERROR: MorphOS binary not found: $MOS_BIN" >&2
    echo "       Build it natively on MorphOS (make -f Makefile.mos) and copy it" >&2
    echo "       here, or point MOS_BIN=... at it." >&2
    exit 1
elif [ ! -f "$MOS_IMG" ]; then
    echo "ERROR: MorphOS heap image not found: $MOS_IMG" >&2
    echo "       Save it on MorphOS with the binary in MOS_BIN (make -f Makefile.mos image" >&2
    echo "       writes build/morphos/amilisp.img) and copy it here, or point MOS_IMG=... at it." >&2
    exit 1
elif [ ! -f "$CLAMACS_MOS_IMG" ]; then
    echo "ERROR: MorphOS Clamacs heap image not found: $CLAMACS_MOS_IMG" >&2
    echo "       Save it on MorphOS with the binary in MOS_BIN (make -f Makefile.mos editor-image" >&2
    echo "       writes build/morphos/clamacs.img) and copy it here, or point CLAMACS_MOS_IMG=... at it." >&2
    exit 1
fi
if [ ! -f clamacs/lisp/load.lisp ] || [ ! -f clamacs/scripts/save-editor-image.lisp ]; then
    echo "ERROR: the clamacs/ submodule is not checked out." >&2
    echo "       Run: git submodule update --init clamacs" >&2
    exit 1
fi
if [ ! -x "$FSUAE_BIN" ]; then
    echo "ERROR: FS-UAE not found at $FSUAE_BIN — the aos3 heap images are saved" >&2
    echo "       in the emulator (same setup as make -f Makefile.cross test-amiga)." >&2
    exit 1
fi

# --- build ----------------------------------------------------------------
echo "--- Building host binary (FASL compiler) ---"
make host

echo "--- Cross-compiling AmigaOS 3 binary (soft-float) ---"
make -f Makefile.cross amiga

echo "--- Cross-compiling AmigaOS 3 binary (hard-float, FPU=1) ---"
make -f Makefile.cross amiga FPU=1

HOST_BIN="$ROOT/build/host/amilisp"
AOS3_BIN="$ROOT/build/cross/amilisp"
AOS3FPU_BIN="$ROOT/build/cross-fpu/amilisp"
[ -x "$HOST_BIN" ] || { echo "ERROR: $HOST_BIN missing" >&2; exit 1; }
[ -f "$AOS3_BIN" ] || { echo "ERROR: $AOS3_BIN missing" >&2; exit 1; }
[ -f "$AOS3FPU_BIN" ] || { echo "ERROR: $AOS3FPU_BIN missing" >&2; exit 1; }

# --- stage ----------------------------------------------------------------
echo "--- Staging $STAGE ---"
rm -rf "$STAGE"
mkdir -p "$STAGE/bin/aos3" "$STAGE/bin/aos3-fpu" "$STAGE/lib/amiga" "$STAGE/docs"

cp "$AOS3_BIN"    "$STAGE/bin/aos3/amilisp"
cp "$AOS3FPU_BIN" "$STAGE/bin/aos3-fpu/amilisp"
chmod +x "$STAGE/bin/aos3/amilisp" "$STAGE/bin/aos3-fpu/amilisp"
if [ "$WITH_MOS" = 1 ]; then
    mkdir -p "$STAGE/bin/mos"
    cp "$MOS_BIN" "$STAGE/bin/mos/amilisp"
    chmod +x "$STAGE/bin/mos/amilisp"
fi

# lib: FASL-portable modules, compiled by the just-built host binary so
# CL_FASL_VERSION matches the packaged binaries exactly.  The script compiles
# with AMILISP_FASL_PORTABLE=1 and refuses a module whose compile printed an
# error or produced no FASL (see its header).
FASL_LIBS="boot clos ffi gray-streams dev-commands dev-repl dev-tcp"
echo "--- compile-file $FASL_LIBS -> $REL/lib/*.fasl ---"
sh scripts/compile-lib-fasls.sh -o "$STAGE" -b "$HOST_BIN" \
    $(for m in $FASL_LIBS; do printf 'lib/%s.lisp ' "$m"; done) \
    || { echo "ERROR: lib FASLs not produced" >&2; exit 1; }

# lib: source-shipped modules (see policy above)
cp lib/asdf.lisp lib/quicklisp.lisp lib/quicklisp-compat.lisp \
   lib/quicklisp-install.lisp "$STAGE/lib/"
cp -R lib/shims "$STAGE/lib/shims"

# lib/amiga/**: sources first (only .lisp -- an in-repo `make fasl-amiga`
# output must not leak in), then the FASLs compiled on top of them so they
# are the newer of the pair and REQUIRE picks them (see policy above).
find lib/amiga -name '*.lisp' | while read -r f; do
    mkdir -p "$STAGE/$(dirname "$f")"
    cp "$f" "$STAGE/$f"
done
echo "--- compile-file lib/amiga/** -> $REL/lib/amiga/**/*.fasl ---"
# --no-docstrings: the DEFCFUN bindings ship without their C-prototype
# docstrings (~16 KB of heap per raw OS module on the target; the .lisp
# sources next to them keep the prototypes).
sh scripts/compile-lib-fasls.sh -o "$STAGE" -b "$HOST_BIN" --no-docstrings \
    || { echo "ERROR: lib/amiga FASLs not produced" >&2; exit 1; }

# lib/clamacs: the Clamacs editor (clamacs/lisp/, specs/clamacs-lisp.md
# over there) as sources plus the FASLs compiled from them in load order by
# the host binary, portable like lib/amiga's (AMILISP_FASL_PORTABLE=1:
# a string literal the Amiga could not load fails here).  Its load.lisp
# takes a FASL beside a source, so a `--no-image` start (or a refused
# image) loads the editor without compiling it on the target.  The two
# image scripts ship with them; the docstrings stay -- they are what
# `M-x` help shows.  It runs from the staging root: a portable FASL records
# its source relative to the cwd, so every function says lib/clamacs/x.lisp,
# where the file sits in the release, and not build/release/<rel>/lib/...,
# which exists on no Amiga (lib/amiga's FASLs get lib/amiga/... the same
# way, compiled from the tree root).
echo "--- lib/clamacs: sources + compile-file -> $REL/lib/clamacs/*.fasl ---"
mkdir -p "$STAGE/lib/clamacs"
cp clamacs/lisp/*.lisp clamacs/scripts/save-editor-image.lisp \
   clamacs/scripts/verify-editor-image.lisp "$STAGE/lib/clamacs/"
CLAMACS_LOG="$OUT/lib-clamacs.log"
( cd "$STAGE" && AMILISP_NO_USERINIT=1 AMILISP_FASL_PORTABLE=1 $TIMEOUT "$HOST_BIN" --no-userinit \
    --no-image --non-interactive --heap 32M \
    --eval '(defvar cl-user::*clamacs-frontend-files* (list "frontend-mui" "transport-arexx"))' \
    --eval '(defvar cl-user::*clamacs-compile-fasls* t)' \
    --load lib/clamacs/load.lisp \
    --eval '(format t "CLAMACS-FASLS ~a~%" (find-package "CLAMACS"))' \
    --eval '(quit)' </dev/null ) > "$CLAMACS_LOG" 2>&1 || true
grep -q "^CLAMACS-FASLS #<PACKAGE CLAMACS>" "$CLAMACS_LOG" &&
! grep -Eq '^ERROR:|^; Warning: FASL unit failed|^WARNING:' "$CLAMACS_LOG" || {
    echo "ERROR: lib/clamacs FASLs not produced — see $CLAMACS_LOG" >&2
    grep -E '^ERROR:|^; Warning: FASL unit failed|^WARNING:' "$CLAMACS_LOG" | head -5 >&2
    exit 1; }
# Every module load.lisp compiles must have left a FASL.  load.lisp itself
# and clamacs.lisp (the run-from-source entry point) are not modules: they
# ship as sources only and never get one.  Neither do the host editor's
# three (the webview frontend, its port and the TCP wire -- the set
# clamacs.lisp binds off the Amiga): they are compiled and loaded only on
# macOS/Linux/Windows, where the editor runs from the source tree.
for f in clamacs/lisp/*.lisp; do
    name=$(basename "${f%.lisp}")
    case "$name" in
        load|clamacs|frontend-host|transport-host|transport-tcp) continue ;;
    esac
    [ -s "$STAGE/lib/clamacs/$name.fasl" ] || {
        echo "ERROR: lib/clamacs/$name.fasl was not written — see $CLAMACS_LOG" >&2
        exit 1; }
done
# No shipped FASL may name a source by where this machine built it: that
# path is what a backtrace, M-. and EXT:FUNCTION-SOURCE-LOCATION print on
# the Amiga, and every image saved from the FASLs carries it too.
leaked=$(grep -rlaF -e "$ROOT/" -e "$REL/" --include='*.fasl' "$STAGE/lib" || true)
[ -z "$leaked" ] || {
    echo "ERROR: these FASLs record build-machine source paths (compiled from the wrong cwd?):" >&2
    echo "$leaked" | sed 's/^/    /' >&2
    exit 1; }

# heap images: one per binary, beside it (policy above).  The m68k pair is
# saved by the staged binaries from the staged layout in FS-UAE, and each is
# restarted from the release root — where no image sits, so discovery has to
# take the PROGDIR: leg — and must report the restore and this version.
REL_DIR="build/release/$REL"
for t in aos3 aos3-fpu; do
    echo "--- Saving + verifying $REL/bin/$t/amilisp.img in FS-UAE ---"
    verify/realamiga/make-image.sh "$REL_DIR/bin/$t" "$REL_DIR" \
        || { echo "ERROR: heap image for bin/$t not produced" >&2; exit 1; }
    grep -q "^IMAGE-VERSION $VERSION" build/amiga/image.log || {
        echo "ERROR: the bin/$t image run reported another version than $VERSION — see build/amiga/image.log" >&2
        exit 1; }
done
[ "$WITH_MOS" = 0 ] || cp "$MOS_IMG" "$STAGE/bin/mos/amilisp.img"

# The editor's image beside each binary, the same way: the staged binary
# loads the staged lib/clamacs/ in FS-UAE and dumps its heap, then starts
# the editor from that image (a window, the menu strip, a clean exit).
for t in aos3 aos3-fpu; do
    echo "--- Saving + verifying $REL/bin/$t/clamacs.img in FS-UAE ---"
    verify/realamiga/make-editor-image.sh "$REL_DIR/bin/$t" "$REL_DIR" \
        || { echo "ERROR: Clamacs heap image for bin/$t not produced" >&2; exit 1; }
    grep -q "^IMAGE-VERSION $VERSION" build/amiga/editor-image.log || {
        echo "ERROR: the bin/$t Clamacs image run reported another version than $VERSION — see build/amiga/editor-image.log" >&2
        exit 1; }
done
[ "$WITH_MOS" = 0 ] || cp "$CLAMACS_MOS_IMG" "$STAGE/bin/mos/clamacs.img"

# docs: package API reference only (no benchmarks/screenshots), plus the
# editor's README as docs/clamacs.md.
cp docs/README.md docs/amiga.md docs/amilisp.md docs/ext.md docs/ffi.md \
   docs/gray.md docs/mop.md docs/mp.md docs/package-symbols.txt \
   docs/amilisp-documented-symbols.txt "$STAGE/docs/"
cp clamacs/README.md "$STAGE/docs/clamacs.md"

# examples, as-is
cp -R examples "$STAGE/examples"

cp README-FIRST.md README.md LICENSE "$STAGE/"

# The AmigaGuide docs, laid out as the package root (specs/amigaguide-docs.md):
# README-FIRST.guide, amilisp.guide (the README) and clamacs.guide (the
# editor's) at the top next to the Workbench icons, the reference pages
# under docs/ next to their Markdown.  The converter runs in the host binary
# and fails the release on Markdown it cannot render or on a dangling link.
echo "--- AmigaGuide docs -> $REL/*.guide + $REL/docs/*.guide ---"
sh tools/docs/md2guide.sh "$HOST_BIN" "$STAGE" \
    || { echo "ERROR: AmigaGuide docs not produced" >&2; exit 1; }

# Workbench icons (icons/, generated by scripts/make-icons.py): three IconX
# project icons in the package root, each with its launcher script of the
# same name, one pair per binary -- a double-click on AmiLisp / AmiLisp-FPU
# / AmiLisp-MOS opens a console and starts that amilisp, Clamacs /
# Clamacs-FPU / Clamacs-MOS start the editor on the same build.  The
# scripts do no platform detection, so the MorphOS pair is only staged
# when bin/mos is.  And one icon per guide (default tool MultiView),
# so the documentation is visible and readable from Workbench.
LAUNCHERS="AmiLisp AmiLisp-FPU Clamacs Clamacs-FPU"
[ "$WITH_MOS" = 0 ] || LAUNCHERS="$LAUNCHERS AmiLisp-MOS Clamacs-MOS"
for n in $LAUNCHERS; do
    cp "icons/$n" "icons/$n.info" "$STAGE/"
done
# The root is a 3 x 3 grid (amilisp launchers, Clamacs launchers, guides as
# the columns; soft-float, FPU, MorphOS as the rows) by positions in these
# icons; the reference guides under docs/ get the unpositioned copy.
for n in README-FIRST cl-amiga clamacs; do
    cp "icons/$n.guide.info" "$STAGE/$n.guide.info"
done
for g in "$STAGE"/docs/*.guide; do
    cp icons/Guide.info "$g.info"
done
# The icon OF the package drawer, shipped beside it in the archive as
# amilisp-<version>.info: it carries the drawer window's size (big enough
# for the grid) and "show only files with icons, view by icon".
cp icons/Drawer.info "$OUT/$REL.info"

if [ "$SNAPSHOT" = 1 ] && [ "$WITH_MOS" = 1 ]; then
    cat > "$STAGE/SNAPSHOT.txt" <<EOF
*** DEVELOPMENT SNAPSHOT of AmiLisp $VERSION -- not a release ***

Built from commit $SNAPSHOT_ID on $(date +%Y-%m-%d), AmigaOS and MorphOS
binaries.  Everything else is the release layout.
EOF
elif [ "$SNAPSHOT" = 1 ]; then
    cat > "$STAGE/SNAPSHOT.txt" <<EOF
*** DEVELOPMENT SNAPSHOT of AmiLisp $VERSION -- not a release ***

Built from commit $SNAPSHOT_ID on $(date +%Y-%m-%d), AmigaOS binaries only:
there is no bin/mos/ in this package, and the MorphOS lines of
README-FIRST.guide do not apply.  Everything else is the release layout.
EOF
fi

# keep emulator/host metadata out of the archive
find "$STAGE" -name '*.uaem' -delete
find "$STAGE" -name '.DS_Store' -delete

# --- smoke test -----------------------------------------------------------
# Prove the deployed layout resolves lib/ executable-relatively: run a copy
# of the tree with the HOST binary substituted at bin/aos3/amilisp (same C
# search code as the Amiga builds), from an unrelated working directory.
if [ "$SMOKE" = 1 ]; then
    echo "--- Smoke test: lib resolution in deployed layout ---"
    SMOKEDIR=$(mktemp -d)
    trap 'rm -rf "$SMOKEDIR"' EXIT
    # -p: keep the staged mtimes, i.e. the FASL-newer-than-source ordering
    # that the archives preserve too and that REQUIRE decides on.
    cp -Rp "$STAGE" "$SMOKEDIR/rel"
    cp "$HOST_BIN" "$SMOKEDIR/rel/bin/aos3/amilisp"
    # Heap images are per-build: the staged bin/aos3/amilisp.img is the m68k
    # one, which the host binary would refuse and fall back to the FASLs — a
    # green run proving nothing about images.  Save a host image into the
    # copy with the same script the targets use, verify it from the release
    # root with the same check script, and require the main smoke run below
    # to have come from it.
    rm -f "$SMOKEDIR/rel/bin/aos3/amilisp.img"
    ( cd "$SMOKEDIR/rel/bin/aos3" && AMILISP_NO_USERINIT=1 AMILISP_HOME= $TIMEOUT \
        ./amilisp --no-userinit --no-image --non-interactive \
        --load "$ROOT/scripts/save-boot-image.lisp" ) > "$OUT/smoke-image.log" 2>&1 \
      && [ -s "$SMOKEDIR/rel/bin/aos3/amilisp.img" ] || {
        echo "ERROR: host image save in the release layout failed — see $OUT/smoke-image.log" >&2
        exit 1; }
    ( cd "$SMOKEDIR/rel" && AMILISP_NO_USERINIT=1 AMILISP_HOME= $TIMEOUT \
        bin/aos3/amilisp --no-userinit --non-interactive \
        --load "$ROOT/scripts/verify-boot-image.lisp" ) >> "$OUT/smoke-image.log" 2>&1 \
      && grep -q "^BOOT-IMAGE-VERIFIED" "$OUT/smoke-image.log" || {
        echo "ERROR: release layout did not start from bin/aos3/amilisp.img — see $OUT/smoke-image.log" >&2
        exit 1; }
    ( cd "$SMOKEDIR" && \
      AMILISP_NO_USERINIT=1 AMILISP_HOME= $TIMEOUT \
        "$SMOKEDIR/rel/bin/aos3/amilisp" --non-interactive --heap 48M \
        --eval '(format t "IMAGE-RESTORED ~a~%" ext:*image-restored-p*)' \
        --eval '(require "gray-streams")' \
        --eval '(require "dev-commands")' \
        --eval '(format t "DEV-COMMANDS ~a~%" (find-package "EXT.DEV"))' \
        --eval '(require "dev-repl")' \
        --eval '(format t "DEV-REPL ~a~%" (find-symbol "*REPL-THREAD-STACK-SIZE*" "EXT.DEV"))' \
        --eval '(require "dev-tcp")' \
        --eval '(format t "DEV-TCP ~a~%" (find-package "EXT.DEV.TCP"))' \
        --eval '(require "asdf")' \
        --eval '(format t "SHIM-AT ~a~%" (asdf:system-source-directory (asdf:find-system "cl+ssl")))' \
        --eval '(require "amiga/raw/exec")' \
        --eval '(require "amiga/reaction")' \
        --eval '(format t "MEMF-CHIP ~a~%" (symbol-value (find-symbol "+MEMF-CHIP+" "AMIGA.RAW.EXEC")))' \
        --eval '(format t "SMOKE-OK ~a~%" (lisp-implementation-version))' \
        --eval '(quit)' ) | tee "$OUT/smoke.log" | grep -q "SMOKE-OK $VERSION" || {
        echo "ERROR: smoke test failed — see $OUT/smoke.log" >&2; exit 1; }
    grep -q "^IMAGE-RESTORED T" "$OUT/smoke.log" || {
        echo "ERROR: the smoke session did not come from bin/aos3/amilisp.img — see $OUT/smoke.log" >&2
        exit 1; }
    # --no-image: the FASL boot must still bring the layout up — it is the
    # fallback when an image is refused.
    ( cd "$SMOKEDIR" && AMILISP_NO_USERINIT=1 AMILISP_HOME= $TIMEOUT \
        "$SMOKEDIR/rel/bin/aos3/amilisp" --non-interactive --no-image \
        --eval '(format t "FASL-BOOT ~a ~a~%" ext:*image-restored-p* (lisp-implementation-version))' \
        --eval '(quit)' ) | tee "$OUT/smoke-noimage.log" | grep -q "^FASL-BOOT NIL $VERSION" || {
        echo "ERROR: --no-image FASL boot of the release layout failed — see $OUT/smoke-noimage.log" >&2
        exit 1; }
    # The cl+ssl shim must resolve out of the release's own lib/shims/ —
    # this is what makes drakma/hunchentoot TLS work from a binary install
    # regardless of what Quicklisp/ocicl have on disk.
    grep -q "SHIM-AT .*rel/lib/shims/cl+ssl" "$OUT/smoke.log" || {
        echo "ERROR: cl+ssl shim did not resolve from the release lib/shims/ — see $OUT/smoke.log" >&2
        exit 1; }
    # lib/amiga must come up from the shipped FASLs (not the sources next to
    # them) and work: REQUIRE prints the file it loads.  REQUIRE's
    # executable-relative leg concatenates the layout onto the exedir and
    # never normalises, so the loaded path reads
    # rel/bin/aos3/../../lib/amiga/... — the `rel/` anchor plus `.*` still
    # pins the file to the staged copy, which is what this asserts.
    grep -q "; Loading .*rel/.*lib/amiga/raw/exec\.fasl" "$OUT/smoke.log" &&
    grep -q "; Loading .*rel/.*lib/amiga/reaction\.fasl" "$OUT/smoke.log" &&
    grep -q "^MEMF-CHIP 2" "$OUT/smoke.log" || {
        echo "ERROR: lib/amiga did not load from the release FASLs — see $OUT/smoke.log" >&2
        exit 1; }
    # The ARexx development port's command layer (what amiga/arexx REQUIREs
    # and Clamacs talks to) must ship and come up from its FASL.
    grep -q "; Loading .*rel/.*lib/dev-commands\.fasl" "$OUT/smoke.log" &&
    grep -q "^DEV-COMMANDS #<PACKAGE EXT.DEV>" "$OUT/smoke.log" || {
        echo "ERROR: dev-commands did not load from the release FASL — see $OUT/smoke.log" >&2
        exit 1; }
    # dev-repl is the other half of the EXT.DEV layer (the editor's REPL
    # thread, loaded on demand by REPL-ATTACH) — it must ship and load too.
    grep -q "; Loading .*rel/.*lib/dev-repl\.fasl" "$OUT/smoke.log" &&
    grep -q "^DEV-REPL \*REPL-THREAD-STACK-SIZE\*" "$OUT/smoke.log" || {
        echo "ERROR: dev-repl did not load from the release FASL — see $OUT/smoke.log" >&2
        exit 1; }
    # dev-tcp is the same port over TCP: what a host Clamacs drives an
    # Amiga amilisp through.
    grep -q "; Loading .*rel/.*lib/dev-tcp\.fasl" "$OUT/smoke.log" &&
    grep -q "^DEV-TCP #<PACKAGE EXT.DEV.TCP>" "$OUT/smoke.log" || {
        echo "ERROR: dev-tcp did not load from the release FASL — see $OUT/smoke.log" >&2
        exit 1; }
    # The AmigaGuide docs: one per shipped page, in the package root or under
    # docs/, each a real guide file (@DATABASE first) with its Workbench icon
    # -- the converter's own checks ran during staging.
    for g in README-FIRST amilisp clamacs docs/README docs/ext docs/mp docs/ffi \
             docs/gray docs/mop docs/amilisp docs/amiga; do
        [ "$(head -c 10 "$STAGE/$g.guide" 2>/dev/null)" = "@DATABASE " ] || {
            echo "ERROR: $g.guide missing or not an AmigaGuide file" >&2
            exit 1; }
        [ -s "$STAGE/$g.guide.info" ] || {
            echo "ERROR: $g.guide has no icon ($g.guide.info)" >&2
            exit 1; }
    done
    # The drawer icon beside the package: a WBDRAWER DiskObject (magic
    # E310, type 2 at offset 48) with its DrawerData.
    [ "$(od -An -tx1 -N2 "$OUT/$REL.info" | tr -d ' \n')" = "e310" ] &&
    [ "$(od -An -tx1 -j48 -N1 "$OUT/$REL.info" | tr -d ' \n')" = "02" ] || {
        echo "ERROR: $REL.info is not a drawer icon" >&2
        exit 1; }
    # The root guides link into docs/ and the reference links back up: the
    # paths must be relative to each guide's own drawer (AmigaDOS: a leading
    # slash is the parent).
    grep -q 'LINK "docs/README.guide/main"' "$STAGE/README-FIRST.guide" &&
    grep -q 'LINK "docs/ext.guide/' "$STAGE/amilisp.guide" &&
    grep -q 'LINK "/amilisp.guide/' "$STAGE/docs/amiga.guide" || {
        echo "ERROR: the guides do not link across the root/docs layout" >&2
        exit 1; }
    # The editor: its m68k images are beside the binaries (verified in
    # FS-UAE above), and lib/clamacs/ must come up from the shipped FASLs
    # on the host binary -- the fallback of a `--no-image` start.  The
    # GUI itself cannot run here, so the load stops at the pure modules
    # (the frontend files are Amiga-only in load.lisp).
    for t in aos3 aos3-fpu; do
        [ -s "$STAGE/bin/$t/clamacs.img" ] || {
            echo "ERROR: bin/$t/clamacs.img is missing or empty" >&2; exit 1; }
    done
    ( cd "$SMOKEDIR" && AMILISP_NO_USERINIT=1 AMILISP_HOME= $TIMEOUT \
        "$SMOKEDIR/rel/bin/aos3/amilisp" --non-interactive --no-image --heap 16M \
        --load "$SMOKEDIR/rel/lib/clamacs/load.lisp" \
        --eval '(format t "CLAMACS-LIB ~a ~a~%" (find-package "CLAMACS") (fboundp (find-symbol "MENU-STATE" "CLAMACS")))' \
        --eval '(quit)' ) > "$OUT/smoke-clamacs.log" 2>&1
    grep -q "^CLAMACS-LIB #<PACKAGE CLAMACS> T" "$OUT/smoke-clamacs.log" &&
    grep -q "; Loading .*rel/lib/clamacs/menu\.fasl" "$OUT/smoke-clamacs.log" || {
        echo "ERROR: lib/clamacs did not load from the release FASLs — see $OUT/smoke-clamacs.log" >&2
        exit 1; }
    # And the editor's image scripts round-trip on the host too: a host
    # clamacs.img saved beside the host binary, restored, holding the
    # editor (START itself needs MUI and is what FS-UAE verified).
    rm -f "$SMOKEDIR/rel/bin/aos3/clamacs.img"
    ( cd "$SMOKEDIR/rel/bin/aos3" && AMILISP_NO_USERINIT=1 AMILISP_HOME= $TIMEOUT \
        ./amilisp --no-userinit --no-image --non-interactive --heap 16M \
        --load ../../lib/clamacs/load.lisp \
        --load ../../lib/clamacs/save-editor-image.lisp ) >> "$OUT/smoke-clamacs.log" 2>&1 \
      && [ -s "$SMOKEDIR/rel/bin/aos3/clamacs.img" ] || {
        echo "ERROR: host Clamacs image save in the release layout failed — see $OUT/smoke-clamacs.log" >&2
        exit 1; }
    ( cd "$SMOKEDIR/rel" && AMILISP_NO_USERINIT=1 AMILISP_HOME= $TIMEOUT \
        bin/aos3/amilisp --no-userinit --non-interactive --heap 16M \
        --image bin/aos3/clamacs.img \
        --eval '(format t "CLAMACS-IMAGE ~a ~a~%" ext:*image-restored-p* (fboundp (find-symbol "MENU-STATE" "CLAMACS")))' \
        --eval '(quit)' ) >> "$OUT/smoke-clamacs.log" 2>&1
    grep -q "^CLAMACS-IMAGE T T" "$OUT/smoke-clamacs.log" || {
        echo "ERROR: the host Clamacs image did not restore the editor — see $OUT/smoke-clamacs.log" >&2
        exit 1; }
    echo "smoke test passed"
fi

# --- archives -------------------------------------------------------------
echo "--- Archiving ---"
rm -f "$OUT/$REL-bin.zip" "$OUT/$REL-bin.lha"
# the drawer icon travels beside the drawer, so the unpacked amilisp-<version>
# shows up on Workbench with its window laid out
( cd "$OUT" && zip -rq "$REL-bin.zip" "$REL" "$REL.info" )
if command -v lha > /dev/null 2>&1; then
    ( cd "$OUT" && lha aq "$REL-bin.lha" "$REL" "$REL.info" ) \
        || echo "warning: lha archiving failed — the .zip is still valid"
else
    echo "note: lha not found — only the .zip was created"
fi

echo "=== Done ==="
ls -lh "$OUT" | grep -E "$REL" || true
