#!/bin/sh
# Container-side entry point: copy the read-only source mount to a
# writable tree, then run the requested command in it.  With no
# arguments, runs the fast test tier (make test).
#
# Expected mounts (see run.sh):
#   /amilisp   amilisp repo (ro)
set -e

echo "=== [1/2] Copying amilisp sources to /tmp/amilisp ==="
mkdir -p /tmp/amilisp
(cd /amilisp && tar cf - \
    --exclude=./build \
    --exclude=./tools \
    --exclude=./.git \
    --exclude=./verify/realamiga \
    . ) | tar xf - -C /tmp/amilisp
# tools/ holds the m68k toolchain and vendored SDK dumps (hundreds of
# thousands of files, none needed here), but tools/docs is the AmigaGuide
# converter that make test's md2guide gate runs -- copy just that back.
mkdir -p /tmp/amilisp/tools
cp -R /amilisp/tools/docs /tmp/amilisp/tools/docs

cd /tmp/amilisp
if [ $# -eq 0 ]; then
    set -- make test
fi
echo "=== [2/2] Running: $* ==="
exec "$@"
