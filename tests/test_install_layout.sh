#!/bin/sh
# `make image` + `make install` layout (Makefile): an installed amilisp
# starts from a heap image the way the binary release does.
#
#   make image           writes + verifies build/host/image/amilisp.img,
#                        OFF the startup discovery path (nothing beside the
#                        binary — a leftover there would be restored by every
#                        later test run and outlive a lib/boot.lisp change)
#   make install-layout  <prefix>/bin/amilisp, <prefix>/lib/amilisp/ (lib/ +
#                        the image), the copying part of `make install`
#   make uninstall       removes bin/amilisp and lib/amilisp/ wholesale
#
# Covered:
#   make image: exit 0, image present under build/<host>/image/, no
#     amilisp.img beside the binary
#   install-layout into a DESTDIR: binary, lib FASLs and the image (byte-
#     identical to the built one) in place
#   the installed binary, run from an unrelated cwd with no AMILISP_HOME:
#     restores from <prefix>/lib/amilisp/amilisp.img (BOOT-IMAGE-VERIFIED,
#     which includes REQUIRE finding the installed lib/)
#   --no-image on the installed binary still boots from the installed FASLs
#   uninstall from the DESTDIR: bin/amilisp and lib/amilisp/ (image included)
#     gone
#
# Drives make in the repo, so the binary argument every shell test receives
# is not used: `make image` and the layout take the Makefile's default
# BUILDDIR, and the installed copy is what gets exercised.
#
# Run: sh tests/test_install_layout.sh build/host/amilisp

ROOT=$(cd "$(dirname "$0")/.." && pwd)

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)
if [ -z "$TIMEOUT" ]; then
    echo "SKIP test_install_layout: neither timeout nor gtimeout on PATH"
    exit 0
fi

passed=0
failed=0

WORK=$(mktemp -d "${TMPDIR:-/tmp}/amilisp_install_XXXXXX") || exit 1
trap 'rm -rf "$WORK"' EXIT INT TERM
cd "$WORK" || exit 1

fail() {
    desc="$1"; why="$2"; out="$3"
    failed=$((failed + 1))
    echo "  FAIL  $desc ($why)"
    echo "    output: $(echo "$out" | tail -12)"
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

# exists DESC PATH / absent DESC PATH
exists() {
    desc="$1"
    if [ -s "$2" ]; then ok; else fail "$desc" "$2 missing or empty" ""; fi
}
absent() {
    desc="$1"
    if [ -e "$2" ]; then fail "$desc" "$2 exists" ""; else ok; fi
}

mk() {
    # MAKEFLAGS from an enclosing `make test` (CC_HOST=..., -j) is inherited
    # through the environment, so the sub-make sees the same configuration.
    "$TIMEOUT" 300 make -C "$ROOT" --no-print-directory "$@" </dev/null 2>&1
}

# --- make image ----------------------------------------------------------

out=$(mk image)
ec=$?
check "make_image" 0 "$ec" "$out" "Image saved to \"amilisp.img\"" \
    "^IMAGE-RESTORED-P T" "^BOOT-IMAGE-VERIFIED"

builddir=$(echo "$out" | sed -n 's/^rm -f \(.*\)\/image\/amilisp\.img$/\1/p' | head -1)
if [ -z "$builddir" ]; then
    fail "image_builddir_from_recipe" "no 'rm -f <builddir>/image/amilisp.img' line" "$out"
    echo "test_install_layout: $passed passed, $((failed)) failed"
    exit 1
fi
desc="image_builddir_from_recipe"; ok
case "$builddir" in
    /*) : ;;
    *) builddir="$ROOT/$builddir" ;;
esac

exists "image_written_under_builddir_image" "$builddir/image/amilisp.img"
absent "no_image_beside_the_binary" "$builddir/amilisp.img"
absent "no_image_in_the_repo_root" "$ROOT/amilisp.img"

# --- install-layout into a DESTDIR ---------------------------------------

DEST="$WORK/root"
PFX=/opt/amilisp
out=$(mk install-layout DESTDIR="$DEST" PREFIX="$PFX")
ec=$?
check "install_layout_into_destdir" 0 "$ec" "$out"

INST="$DEST$PFX"
exists "installed_binary" "$INST/bin/amilisp"
exists "installed_boot_fasl" "$INST/lib/amilisp/boot.fasl"
exists "installed_clos_fasl" "$INST/lib/amilisp/clos.fasl"
exists "installed_image_in_lib_amilisp" "$INST/lib/amilisp/amilisp.img"
absent "no_image_beside_installed_binary" "$INST/bin/amilisp.img"
absent "no_nested_lib_dir" "$INST/lib/amilisp/lib"
if cmp -s "$builddir/image/amilisp.img" "$INST/lib/amilisp/amilisp.img"; then
    desc="installed_image_is_the_built_one"; ok
else
    fail "installed_image_is_the_built_one" "differs from $builddir/image/amilisp.img" ""
fi

# --- the installed amilisp starts from its image ------------------------
# From an unrelated cwd (no amilisp.img here, no AMILISP_HOME): discovery has
# to take the <prefix>/lib/amilisp/ leg.  verify-boot-image.lisp also
# REQUIREs a module, i.e. the installed lib/ is found from the restored
# session.

mkdir -p elsewhere
out=$(cd elsewhere && AMILISP_HOME= "$TIMEOUT" 120 "$INST/bin/amilisp" \
    --no-userinit --non-interactive \
    --load "$ROOT/scripts/verify-boot-image.lisp" </dev/null 2>&1)
ec=$?
check "installed_binary_restores_from_lib_amilisp_image" 0 "$ec" "$out" \
    "^IMAGE-RESTORED-P T" "^BOOT-IMAGE-VERIFIED"

# --- --no-image still boots from the installed FASLs --------------------

out=$(cd elsewhere && AMILISP_HOME= "$TIMEOUT" 120 "$INST/bin/amilisp" \
    --no-userinit --no-image --non-interactive \
    --eval '(format t "NOIMG=~a SUM=~a~%" ext:*image-restored-p* (loop for i from 1 to 10 sum i))' \
    --eval '(progn (require "gray-streams") (format t "LIB=~a~%" (not (null (find-package "GRAY")))))' \
    </dev/null 2>&1)
ec=$?
check "installed_binary_no_image_boots_from_fasls" 0 "$ec" "$out" \
    "^NOIMG=NIL SUM=55" "^LIB=T"

# --- uninstall -----------------------------------------------------------

out=$(mk uninstall DESTDIR="$DEST" PREFIX="$PFX")
ec=$?
check "uninstall_from_destdir" 0 "$ec" "$out"
absent "uninstall_removed_binary" "$INST/bin/amilisp"
absent "uninstall_removed_lib_amilisp_with_image" "$INST/lib/amilisp"

# --- Report --------------------------------------------------------------

echo "test_install_layout: $passed passed, $failed failed"
[ "$failed" -eq 0 ]
