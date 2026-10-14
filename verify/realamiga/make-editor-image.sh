#!/bin/sh
# make-editor-image.sh — save the Clamacs editor's heap image beside an
# Amiga amilisp binary, unattended in FS-UAE, and prove it starts the editor.
#
#   verify/realamiga/make-editor-image.sh BINDIR LIBROOT
#
#   BINDIR      repo-relative directory holding the m68k `amilisp` to save
#               the image for: build/cross, build/cross-fpu, or a staged
#               release's bin/aos3.  The image is written next to it as
#               BINDIR/clamacs.img -- what the Clamacs launcher passes as
#               `--image`.
#   LIBROOT     repo-relative directory holding lib/clamacs/ (the editor's
#               sources or FASLs plus its two image scripts): the repo root
#               for a build tree (where clamacs/lisp/ is linked in below),
#               or the staged release, whose lib/clamacs/ ships.
#
# Heap images are per-build (specs/image-save-load.md), so the Amiga binary
# itself loads the editor in FS-UAE and dumps its heap.  Both runs go
# through the boot-override hook of call-on-ustartup:
#
#   1. save:   cd BINDIR; amilisp --no-image --load LIBROOT/lib/clamacs/load.lisp
#                                            --load LIBROOT/lib/clamacs/save-editor-image.lisp
#   2. verify: cd LIBROOT; amilisp --image BINDIR/clamacs.img
#                                  --load LIBROOT/lib/clamacs/verify-editor-image.lisp
#              (the editor opens a window, builds its menu and quits)
#
# Needs the FS-UAE setup of `make -f Makefile.cross test-amiga` -- the
# Workbench image there has MUI and TextEditor.mcc.  Exit 0 only when the
# verify run printed EDITOR-IMAGE-VERIFIED; the emulator-side log stays in
# build/amiga/editor-image.log.  Counterpart of make-image.sh.
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT" || exit 1

BINDIR="${1:?usage: make-editor-image.sh BINDIR LIBROOT}"
LIBROOT="${2:?usage: make-editor-image.sh BINDIR LIBROOT}"
LOG=build/amiga/editor-image.log

for d in "$BINDIR" "$LIBROOT"; do
    case "$d" in
        /*|*:*)
            echo "ERROR: $d must be relative to the repo root — FS-UAE sees it as AmiLisp:$d" >&2
            exit 1 ;;
    esac
done
BINDIR=${BINDIR%/}
LIBROOT=${LIBROOT%/}
[ -f "$BINDIR/amilisp" ] || {
    echo "ERROR: $BINDIR/amilisp missing — make -f Makefile.cross amiga first" >&2; exit 1; }
[ -f "$LIBROOT/lib/clamacs/load.lisp" ] || {
    echo "ERROR: $LIBROOT/lib/clamacs/load.lisp missing — the editor's files are not under $LIBROOT/lib/clamacs/" >&2; exit 1; }
[ -f "$LIBROOT/lib/clamacs/save-editor-image.lisp" ] || {
    echo "ERROR: $LIBROOT/lib/clamacs/save-editor-image.lisp missing" >&2; exit 1; }
if pgrep -x fs-uae >/dev/null 2>&1; then
    echo "ERROR: an FS-UAE is already running — pkill fs-uae first" >&2
    exit 1
fi

IMG="$BINDIR/clamacs.img"
mkdir -p build/amiga
rm -f "$IMG" "$IMG.uaem" "$LOG" "$LOG.uaem"

AMI_BIN="AmiLisp:$BINDIR/amilisp"
AMI_BINDIR="AmiLisp:$BINDIR"
AMI_IMG="AmiLisp:$IMG"
AMI_LIB="AmiLisp:$LIBROOT/lib/clamacs"
AMI_LIBROOT="AmiLisp:$LIBROOT"
AMI_LOG="AmiLisp:$LOG"

# The editor's own exit trace must be this run's alone: the verify run
# leaves it, and its last line says whether the teardown completed.
cat > build/amiga/boot-override <<EOF
stack 128000
echo "=== editor image start ===" >$AMI_LOG
date >>$AMI_LOG
IF EXISTS T:clamacs-exit.log
  delete >NIL: T:clamacs-exit.log
ENDIF
echo "=== save: $AMI_BIN ===" >>$AMI_LOG
cd $AMI_BINDIR
$AMI_BIN --no-userinit --no-image --heap 8M --non-interactive --boot-log --load $AMI_LIB/load.lisp --load $AMI_LIB/save-editor-image.lisp >>$AMI_LOG
echo "=== verify from $AMI_LIBROOT ===" >>$AMI_LOG
cd $AMI_LIBROOT
$AMI_BIN --no-userinit --heap 8M --non-interactive --boot-log --image $AMI_IMG --load $AMI_LIB/verify-editor-image.lisp >>$AMI_LOG
echo "=== T:clamacs-exit.log ===" >>$AMI_LOG
IF EXISTS T:clamacs-exit.log
  type T:clamacs-exit.log >>$AMI_LOG
ENDIF
echo "=== run end ===" >>$AMI_LOG
date >>$AMI_LOG
C:UAEquit
EOF

echo "=== Launching FS-UAE — Clamacs heap image for $BINDIR/amilisp ==="
KEEP_BOOT_OVERRIDE=1 FSUAE_LOG="$LOG" \
    verify/realamiga/run-fs-uae.sh verify/realamiga/verify.fs-uae
rm -f build/amiga/boot-override "$IMG.uaem" "$LOG.uaem"

fail() {
    echo "ERROR: $1" >&2
    if [ -f "$LOG" ]; then
        echo "--- $LOG ---" >&2
        cat "$LOG" >&2
    fi
    rm -f "$IMG" "$IMG.uaem"
    exit 1
}

[ -f "$LOG" ] || fail "no $LOG — FS-UAE never ran the boot-override"

echo "=== Editor image run log (boot phases, markers) ==="
grep -E '^; \[boot\]|^=== (save|verify)|^IMAGE-|^; Image saved|IMAGE-FAILED|^EDITOR-IMAGE|^clamacs: exit' "$LOG" \
    | sed 's/^/    /'

grep -q '=== run end ===' "$LOG" \
    || fail "the run did not complete (watchdog kill? see the log)"
grep -q '^; Image saved to' "$LOG" \
    || fail "the save run wrote no image"
grep -q 'IMAGE-FAILED' "$LOG" \
    && fail "a check failed (see EDITOR-IMAGE-FAILED / SAVE-EDITOR-IMAGE-FAILED above)"
grep -q '^EDITOR-IMAGE-VERIFIED' "$LOG" \
    || fail "the restore check did not print EDITOR-IMAGE-VERIFIED"
[ -s "$IMG" ] || fail "$IMG is missing or empty after a verified run"

echo "=== $IMG: $(wc -c < "$IMG" | tr -d ' ') bytes, the editor started from it in FS-UAE ==="
exit 0
