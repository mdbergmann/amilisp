#!/bin/sh
# Run a amilisp verification command in a Debian Bookworm arm64 container.
#
# Copies this repo into the container and runs the given command in the
# writable source tree (default: the fast test tier, `make test`).
#
# Usage:
#   verify/linux-arm64/run.sh                          # make test
#   verify/linux-arm64/run.sh make test-plus           # any other make target
#   verify/linux-arm64/run.sh sh -c 'make host && ./build/host/amilisp --version'
set -e

REPO=$(cd "$(dirname "$0")/../.." && pwd)
IMAGE=amilisp-verify-linux-arm64

docker build -q -t "$IMAGE" "$REPO/verify/linux-arm64" >/dev/null

exec docker run --rm --platform linux/arm64 \
    -v "$REPO":/cl-amiga:ro \
    "$IMAGE" /cl-amiga/verify/linux-arm64/container-entry.sh "$@"
