#!/bin/sh
# Install the m68k-amigaos-gcc cross toolchain into tools/m68k-amigaos-gcc/prefix.
#
# Default behavior (no args):
#   - macOS arm64: download the prebuilt prefix tarball from the AmiLisp
#     release, verify sha256, extract.
#   - everything else: initialize the submodule and build from source via
#     the upstream Makefile (`make all`).
#
# Flags:
#   --build       force build-from-source (works on any host with the
#                 deps listed in tools/m68k-amigaos-gcc/README.md).
#   --download    force the download path (fails if no prebuilt tarball
#                 is published for the current host).
#   --force       reinstall even if the toolchain is already present.
#   -h, --help    show this help.

set -eu

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
TOOLCHAIN_DIR="$REPO_ROOT/tools/m68k-amigaos-gcc"
PREFIX_DIR="$TOOLCHAIN_DIR/prefix"
GCC_BIN="$PREFIX_DIR/bin/m68k-amigaos-gcc"

# --- Pinned prebuilt release (macOS arm64) ----------------------------------
# Updated together with the submodule pin in .gitmodules. To rebuild and
# republish, see tools/README-toolchain.md (or the AmiLisp README).
RELEASE_REPO="mdbergmann/amilisp"
RELEASE_TAG="toolchain-macos-arm64-6.5.0b-251015"
TARBALL_NAME="clamiga-toolchain-macos-arm64-6.5.0b-251015.tar.xz"
TARBALL_SHA256="e42963e984b5fc1704032ec6f4444a64389646cf4ee5677325ac316221adfb29"
TARBALL_URL="https://github.com/${RELEASE_REPO}/releases/download/${RELEASE_TAG}/${TARBALL_NAME}"

mode=auto
force=0

usage() {
    sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'
}

for arg in "$@"; do
    case "$arg" in
        --build)    mode=build ;;
        --download) mode=download ;;
        --force)    force=1 ;;
        -h|--help)  usage; exit 0 ;;
        *) echo "unknown flag: $arg" >&2; exit 2 ;;
    esac
done

log()  { printf '[setup-toolchain] %s\n' "$*"; }
fail() { printf '[setup-toolchain] error: %s\n' "$*" >&2; exit 1; }

if [ "$force" -eq 0 ] && [ -x "$GCC_BIN" ]; then
    log "toolchain already installed at $PREFIX_DIR"
    "$GCC_BIN" --version | head -1 | sed 's/^/[setup-toolchain]   /'
    log "pass --force to reinstall."
    exit 0
fi

# --- Pick mode ---------------------------------------------------------------
if [ "$mode" = auto ]; then
    case "$(uname -s)/$(uname -m)" in
        Darwin/arm64) mode=download ;;
        *)            mode=build ;;
    esac
    log "auto-selected mode: $mode (host $(uname -s)/$(uname -m))"
fi

# --- Download path -----------------------------------------------------------
do_download() {
    case "$(uname -s)/$(uname -m)" in
        Darwin/arm64) ;;
        *)
            fail "no prebuilt tarball published for $(uname -s)/$(uname -m).
Re-run with --build to compile from source."
            ;;
    esac

    if [ "$TARBALL_SHA256" = "__FILL_IN_AFTER_TARBALL_BUILD__" ]; then
        fail "TARBALL_SHA256 not yet pinned in this script.
Either pin it (after publishing the tarball) or use --build."
    fi

    tmp=$(mktemp -d -t amilisp-toolchain.XXXXXX)
    trap 'rm -rf "$tmp"' EXIT

    log "downloading $TARBALL_URL"
    if command -v curl >/dev/null 2>&1; then
        curl -fL --progress-bar -o "$tmp/$TARBALL_NAME" "$TARBALL_URL"
    elif command -v wget >/dev/null 2>&1; then
        wget --show-progress -O "$tmp/$TARBALL_NAME" "$TARBALL_URL"
    else
        fail "neither curl nor wget is available"
    fi

    log "verifying sha256"
    actual=$(shasum -a 256 "$tmp/$TARBALL_NAME" | awk '{print $1}')
    if [ "$actual" != "$TARBALL_SHA256" ]; then
        fail "sha256 mismatch
  expected: $TARBALL_SHA256
  actual:   $actual"
    fi

    log "extracting into $TOOLCHAIN_DIR"
    rm -rf "$PREFIX_DIR"
    mkdir -p "$TOOLCHAIN_DIR"
    tar -xJf "$tmp/$TARBALL_NAME" -C "$TOOLCHAIN_DIR"

    [ -x "$GCC_BIN" ] || fail "tarball did not produce $GCC_BIN"
    log "installed: $("$GCC_BIN" --version | head -1)"
}

# --- Build path --------------------------------------------------------------
do_build() {
    if [ ! -f "$TOOLCHAIN_DIR/Makefile" ]; then
        log "initializing submodule tools/m68k-amigaos-gcc"
        git -C "$REPO_ROOT" submodule update --init --recursive tools/m68k-amigaos-gcc
    fi

    log "building toolchain via upstream Makefile (this takes a while)"
    log "host build deps required — see tools/m68k-amigaos-gcc/README.md"
    jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
    ( cd "$TOOLCHAIN_DIR" && make all -j"$jobs" )

    [ -x "$GCC_BIN" ] || fail "build finished but $GCC_BIN is missing"
    log "installed: $("$GCC_BIN" --version | head -1)"
}

case "$mode" in
    download) do_download ;;
    build)    do_build ;;
    *) fail "unreachable: mode=$mode" ;;
esac

log "done."
