#!/bin/sh
# run-examples.sh — run the GUI programs under examples/amiga/ (gfx/ and
# reaction/) unattended in FS-UAE and photograph them.
#
#   verify/realamiga/run-examples.sh [cross-binary]
#
# Uses the boot-override hook of call-on-ustartup: instead of the test
# suite, the emulator runs examples.lisp (every example with a 6 s
# event-loop timeout, see AMIGA.INTUITION:*EVENT-LOOP-TIMEOUT* and
# AMIGA.REACTION:*EVENT-LOOP-TIMEOUT*) with screen-grab.lisp detached
# beside it, which saves a PPM of the screen for every window that
# appears on the Workbench screen and for every custom screen that
# opens.  Afterwards the PPMs are converted to PNG (ffmpeg) under
# build/amiga/shots/ and the per-example results are summarised from
# build/amiga/test-results.log.
#
# Needs the FS-UAE setup of `make -f Makefile.cross test-amiga` (the OS 3.9
# Workbench image there has the ReAction classes) and a cross-built
# amilisp (default build/cross/amilisp).  Exit status 0 when every
# example reported EXAMPLE-OK (or EXAMPLE-SKIP).
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT" || exit 1
BIN="${1:-build/cross/amilisp}"
LOG=build/amiga/test-results.log

[ -x "$BIN" ] || { echo "ERROR: $BIN missing — make -f Makefile.cross amiga first" >&2; exit 1; }
if pgrep -x fs-uae >/dev/null 2>&1; then
    echo "ERROR: an FS-UAE is already running — pkill fs-uae first" >&2
    exit 1
fi

mkdir -p build/amiga/shots
rm -f build/amiga/shots/* build/amiga/screen-grab.log build/amiga/screen-grab-run.log
cp "$BIN" build/amiga/amilisp

# The AmigaDOS script call-on-ustartup executes instead of the suite.
# It must write the "=== run end ===" marker run-fs-uae.sh watches for,
# and quit the emulator itself.
cat > build/amiga/boot-override <<'EOF'
cd AmiLisp:
stack 128000
echo "=== run start ===" >build/amiga/test-results.log
date >>build/amiga/test-results.log
delete >NIL: T:examples-done
delete >NIL: T:screen-grab-ready
Run >build/amiga/screen-grab-run.log build/amiga/amilisp --no-userinit --heap 16M --non-interactive --load verify/realamiga/screen-grab.lisp
build/amiga/amilisp --no-userinit --heap 24M --non-interactive --load verify/realamiga/examples.lisp >>build/amiga/test-results.log
echo done >T:examples-done
wait 4
echo "=== run end ===" >>build/amiga/test-results.log
date >>build/amiga/test-results.log
C:UAEquit
EOF

echo "=== Launching FS-UAE — GUI examples, unattended ==="
KEEP_BOOT_OVERRIDE=1 verify/realamiga/run-fs-uae.sh verify/realamiga/verify.fs-uae

echo "=== Results ==="
[ -f "$LOG" ] || { echo "no $LOG"; exit 1; }
grep -E '^(EXAMPLE-|EXAMPLES-|=== example|doublebuffer:|sprite:)' "$LOG"
[ -f build/amiga/screen-grab.log ] && grep -E '^; (shot|new window|new screen|screen-grab|grab failed)' build/amiga/screen-grab.log

n=0
for f in build/amiga/shots/*.ppm; do
    [ -f "$f" ] || continue
    if command -v ffmpeg >/dev/null 2>&1; then
        ffmpeg -loglevel error -y -i "$f" "${f%.ppm}.png" && rm -f "$f" && n=$((n + 1))
    fi
done
echo "=== $n screenshot(s) under build/amiga/shots/ ==="

if grep -q '^EXAMPLE-FAIL' "$LOG" || ! grep -q '^EXAMPLES-DONE' "$LOG"; then
    echo "=== GUI examples: FAILED ==="
    exit 1
fi
echo "=== GUI examples: all OK ==="
exit 0
