#!/bin/sh
# run-gc-phase0.sh BINDIR [CONFIG]
#
# The GC measurement of specs/gc-backend-immix.md (trunk/gc-phase0.lisp:
# explicit collections, allocation churn, a compaction; bare and with
# Clamacs loaded) run unattended in FS-UAE with BINDIR/clamiga, so the
# classic and the Immix m68k binaries can be measured on the same emulated
# machine and read side by side (docs/gc-phase0-results.md).
#
#   BINDIR   repo-relative directory holding the m68k amilisp to measure:
#            build/cross (the default, Immix), build/cross-classic
#            (`make -f Makefile.cross amiga GC=classic`), build/cross-fpu, ...
#   CONFIG   the FS-UAE config (default verify/realamiga/verify.fs-uae, the
#            68040 JIT machine of `make -f Makefile.cross test-amiga`;
#            verify-lowend.fs-uae is the cycle-exact 68020 floor, where the
#            classic run takes about 40 minutes).
#   SCRIPT   (environment) a repo-relative Lisp file to run INSTEAD of the
#            phase-0 pair, e.g. SCRIPT=trunk/gc-mark-bench.lisp — the
#            mark-only measurement.  It must print a line matching
#            "=== ... end ===" when it is through.  No Clamacs staging;
#            the log is build/amiga/gc-phase0-<script>-<bindir>-<config>.log.
#
# The Amiga-side log lands in build/amiga/gc-phase0-<bindir>-<config>.log
# (the suite log is left alone).  Both legs go through the boot-override
# hook of call-on-ustartup in place of the test suite; the Clamacs leg
# loads the editor from build/editor-lib/lib/clamacs/ (staged here from
# the submodule the way `make editor-image-amiga` stages it).  Needs the
# FS-UAE setup of `make -f Makefile.cross test-amiga`.
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT" || exit 1

BINDIR="${1:?usage: run-gc-phase0.sh BINDIR [CONFIG.fs-uae]}"
CONFIG="${2:-verify/realamiga/verify.fs-uae}"

case "$BINDIR" in
    /*|*:*)
        echo "ERROR: BINDIR must be relative to the repo root — FS-UAE sees it as AmiLisp:$BINDIR" >&2
        exit 1 ;;
esac
BINDIR=${BINDIR%/}
[ -f "$BINDIR/amilisp" ] || {
    echo "ERROR: $BINDIR/amilisp missing — make -f Makefile.cross amiga [GC=immix] first" >&2; exit 1; }
[ -f "$CONFIG" ] || { echo "ERROR: $CONFIG not found" >&2; exit 1; }
if pgrep -x fs-uae >/dev/null 2>&1; then
    echo "ERROR: an FS-UAE is already running — pkill fs-uae first" >&2
    exit 1
fi
SCRIPT="${SCRIPT:-}"
TAG="$(basename "$BINDIR")-$(basename "$CONFIG" .fs-uae)"
if [ -n "$SCRIPT" ]; then
    [ -f "$SCRIPT" ] || { echo "ERROR: SCRIPT=$SCRIPT not found" >&2; exit 1; }
    TAG="$(basename "$SCRIPT" .lisp)-$TAG"
    AMI_SCRIPT="AmiLisp:$SCRIPT"
else
    [ -d clamacs/lisp ] || {
        echo "ERROR: clamacs/lisp missing — git submodule update --init clamacs" >&2; exit 1; }
    AMI_SCRIPT="AmiLisp:build/amiga/gc-phase0-clamacs.lisp"
fi
LOG="build/amiga/gc-phase0-$TAG.log"
mkdir -p build/amiga
rm -f "$LOG" "$LOG.uaem"

if [ -z "$SCRIPT" ]; then
    # The editor's sources, staged the way the release ships them (what the
    # loaded leg of the measurement LOADs).
    rm -rf build/editor-lib && mkdir -p build/editor-lib/lib/clamacs
    cp clamacs/lisp/*.lisp build/editor-lib/lib/clamacs/

    cat > build/amiga/gc-phase0-clamacs.lisp <<'EOF'
;; Phase 0 with the editor's live set: load Clamacs first, then measure.
(defparameter cl-user::*phase0-load* "AmiLisp:build/editor-lib/lib/clamacs/load.lisp")
(load "AmiLisp:trunk/gc-phase0.lisp")
EOF
fi

AMI_BIN="AmiLisp:$BINDIR/amilisp"
AMI_LOG="AmiLisp:$LOG"

# The AmigaDOS script call-on-ustartup executes instead of the suite.  It
# must write the "=== run end ===" marker run-fs-uae.sh watches for and
# quit the emulator itself.
cat > build/amiga/boot-override <<EOF
stack 128000
cd AmiLisp:
echo "=== gc-phase0 start: $AMI_BIN ($TAG) ===" >$AMI_LOG
date >>$AMI_LOG
$AMI_BIN --no-userinit --no-image --heap 8M --non-interactive --load $AMI_SCRIPT >>$AMI_LOG
echo "=== run end ===" >>$AMI_LOG
date >>$AMI_LOG
C:UAEquit
EOF

echo "=== gc-phase0: $BINDIR/amilisp on $CONFIG -> $LOG ==="
# A churn cycle on the 68020 floor can go 5 minutes without a line of
# output, and the whole run close to an hour; the suite's defaults
# (10 / 30 min) would kill a healthy measurement.
FSUAE_LOG="$LOG" KEEP_BOOT_OVERRIDE=1 \
    STALL_TIMEOUT="${STALL_TIMEOUT:-900}" HARD_TIMEOUT="${HARD_TIMEOUT:-7200}" \
    verify/realamiga/run-fs-uae.sh "$CONFIG"

if [ ! -f "$LOG" ]; then
    echo "=== gc-phase0 FAILED: $LOG was never written ===" >&2
    exit 1
fi
if ! grep -q "=== .* end ===" "$LOG"; then
    echo "=== gc-phase0 FAILED: the script did not run to its end marker ===" >&2
    tail -n 20 "$LOG" | sed 's/^/    /'
    exit 1
fi
echo "=== gc-phase0 done: $LOG ==="
