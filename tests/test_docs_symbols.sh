#!/bin/sh
# docs/package-symbols.txt matches the packages' real export lists, and every
# AMILISP symbol docs/amilisp.md documents is still exported (`make docs-check`).
#
# The snapshot drifted by 75 exports while the check was a manual target only:
# a new export now fails `make test` until the docs/*.md prose mentions it and
# `make docs-update` has regenerated the snapshot.
#
# Run: sh tests/test_docs_symbols.sh [path-to-amilisp]

AMILISP="${1:-build/host/amilisp}"
case "$AMILISP" in
    /*|[A-Za-z]:/*) ;;
    *) AMILISP="$(cd "$(dirname "$AMILISP")" && pwd)/$(basename "$AMILISP")" ;;
esac
ROOT=$(CDPATH= cd "$(dirname "$0")/.." && pwd)

sh "$ROOT/tools/docs/package-symbols.sh" check "$AMILISP"
