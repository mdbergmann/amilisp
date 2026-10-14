#!/bin/sh
# The two deployment scripts behind a shipped amilisp.img, run on the host
# in a release-shaped layout (bin/<target>/clamiga two levels below lib/):
#
#   scripts/save-boot-image.lisp    writes amilisp.img into the cwd (the
#                                   binary's own directory) and quits
#   scripts/verify-boot-image.lisp  from the release root: the session must
#                                   have been restored from that image,
#                                   boot/CLOS content must work, REQUIRE
#                                   must still find lib/; loud failure
#                                   otherwise (exit 1, BOOT-IMAGE-FAILED)
#
# Covered:
#   save from the binary's directory -> bin/aos3/amilisp.img exists
#   verify from the release root: discovery takes the beside-the-binary leg
#     (no amilisp.img in the cwd), BOOT-IMAGE-VERIFIED, exit 0
#   verify with --no-image (the FASL-boot fallback) is a FAILURE: exit 1,
#     names the missing restore — the script cannot green-wash a fallback
#   save refuses (exit 1, image untouched) when the session itself came
#     from an image — i.e. someone forgot --no-image with a stale image
#     beside the binary
#
# The Amiga side of the same scripts is verify/realamiga/make-image.sh
# (`make -f Makefile.cross image-amiga`), which runs them in FS-UAE.
#
# Run: sh tests/test_boot_image_scripts.sh build/host/amilisp

AMILISP="${1:-build/host/amilisp}"
case "$AMILISP" in
    /*) : ;;
    *) AMILISP="$(pwd)/$AMILISP" ;;
esac
ROOT=$(cd "$(dirname "$0")/.." && pwd)

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)
if [ -z "$TIMEOUT" ]; then
    echo "SKIP test_boot_image_scripts: neither timeout nor gtimeout on PATH"
    exit 0
fi

passed=0
failed=0

WORK=$(mktemp -d "${TMPDIR:-/tmp}/clamiga_bootimg_XXXXXX") || exit 1
trap 'rm -rf "$WORK"' EXIT INT TERM
cd "$WORK" || exit 1

fail() {
    desc="$1"; why="$2"; out="$3"
    failed=$((failed + 1))
    echo "  FAIL  $desc ($why)"
    echo "    output: $(echo "$out" | head -12)"
}

ok() { passed=$((passed + 1)); echo "  ok  $desc"; }

# check DESC WANT_EC EC OUT [PATTERN...]: exit status, then every pattern.
check() {
    desc="$1"; want_ec="$2"; ec="$3"; out="$4"
    shift 4
    if [ "$ec" -eq 124 ]; then
        fail "$desc" "timed out" "$out"
        return 1
    fi
    if [ "$ec" -ne "$want_ec" ]; then
        fail "$desc" "exit $ec, wanted $want_ec" "$out"
        return 1
    fi
    for pat in "$@"; do
        if ! echo "$out" | grep -q -- "$pat"; then
            fail "$desc" "missing /$pat/" "$out"
            return 1
        fi
    done
    ok
}

# --- Release-shaped layout: bin/aos3/amilisp, lib/ two levels up ---------

mkdir -p rel/bin/aos3 rel/lib
cp "$AMILISP" rel/bin/aos3/amilisp
cp "$ROOT"/lib/*.fasl "$ROOT"/lib/*.lisp rel/lib/

# --- save-boot-image.lisp from the binary's directory --------------------

out=$(cd rel/bin/aos3 && AMILISP_HOME= "$TIMEOUT" 120 ./amilisp \
    --no-userinit --no-image --heap 16M --non-interactive \
    --load "$ROOT/scripts/save-boot-image.lisp" </dev/null 2>&1)
ec=$?
check "save_writes_image_beside_binary" 0 "$ec" "$out" "Image saved to \"amilisp.img\""
if [ -s rel/bin/aos3/amilisp.img ]; then
    desc="image_file_exists"; ok
else
    fail "image_file_exists" "rel/bin/aos3/amilisp.img missing or empty" "$out"
fi

# --- verify-boot-image.lisp from the release root ------------------------
# No amilisp.img in the cwd: the restore must come from bin/aos3/.

out=$(cd rel && AMILISP_HOME= "$TIMEOUT" 120 bin/aos3/amilisp \
    --no-userinit --heap 16M --non-interactive \
    --load "$ROOT/scripts/verify-boot-image.lisp" </dev/null 2>&1)
ec=$?
check "verify_restored_from_beside_binary" 0 "$ec" "$out" \
    "^IMAGE-RESTORED-P T" "^BOOT-IMAGE-VERIFIED"
if echo "$out" | grep -q "BOOT-IMAGE-FAILED"; then
    fail "verify_reports_no_failed_check" "a BOOT-IMAGE-FAILED line" "$out"
else
    desc="verify_reports_no_failed_check"; ok
fi

# --- verify must not pass a FASL-boot fallback ---------------------------

out=$(cd rel && AMILISP_HOME= "$TIMEOUT" 120 bin/aos3/amilisp \
    --no-userinit --no-image --heap 16M --non-interactive \
    --load "$ROOT/scripts/verify-boot-image.lisp" </dev/null 2>&1)
ec=$?
check "verify_fails_loudly_without_restore" 1 "$ec" "$out" \
    "^IMAGE-RESTORED-P NIL" "^BOOT-IMAGE-FAILED: no image was restored" \
    "BOOT-IMAGE-FAILED: 1 check(s) failed"
if echo "$out" | grep -q "^BOOT-IMAGE-VERIFIED"; then
    fail "verify_no_verified_marker_on_fallback" "BOOT-IMAGE-VERIFIED printed" "$out"
else
    desc="verify_no_verified_marker_on_fallback"; ok
fi

# --- save refuses from a restored session (forgotten --no-image) ---------

cp rel/bin/aos3/amilisp.img before.img
out=$(cd rel/bin/aos3 && AMILISP_HOME= "$TIMEOUT" 120 ./amilisp \
    --no-userinit --heap 16M --non-interactive \
    --load "$ROOT/scripts/save-boot-image.lisp" </dev/null 2>&1)
ec=$?
check "save_refuses_from_restored_session" 1 "$ec" "$out" \
    "SAVE-BOOT-IMAGE-FAILED: .*restored from an image"
if cmp -s before.img rel/bin/aos3/amilisp.img; then
    desc="refused_save_leaves_image_untouched"; ok
else
    fail "refused_save_leaves_image_untouched" "amilisp.img changed" "$out"
fi

# --- Report --------------------------------------------------------------

echo "test_boot_image_scripts: $passed passed, $failed failed"
[ "$failed" -eq 0 ]
