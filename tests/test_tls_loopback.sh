#!/bin/sh
# TLS end-to-end test: drives tests/tls-loopback.lisp — an MP server thread
# and the main thread handshake with each other over loopback through the
# native TLS layer (EXT:SOCKET-START-TLS), exchanging characters and binary
# data, checking LISTEN semantics, peer-certificate introspection,
# certificate-verification success AND failure, and EOF delivery.
#
# The Lisp script prints "TLS-LOOPBACK ALL-PASSED" only when every check
# passed; it also prints it when no TLS provider is installed on the host
# (the whole feature is runtime-optional), so this test never fails just
# because OpenSSL is missing.
#
# Also run by the gc-stress suite (CLAMIGA_GC_STRESS=1) so the TLS builtins'
# allocating paths (string args, peer-cert plist) see forced compaction.
#
# Run: sh tests/test_tls_loopback.sh build/host/amilisp

CLAMIGA="${1:-build/host/amilisp}"

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)
if [ -z "$TIMEOUT" ]; then
    echo "SKIP test_tls_loopback: neither timeout nor gtimeout on PATH"
    exit 0
fi

if [ ! -x "$CLAMIGA" ]; then
    echo "FAIL test_tls_loopback: $CLAMIGA not found (run make host first)"
    exit 1
fi

OUT=$("$TIMEOUT" 120 "$CLAMIGA" --no-userinit --non-interactive \
      --load tests/tls-loopback.lisp </dev/null 2>&1)
rc=$?

if [ $rc -ne 0 ]; then
    echo "FAIL test_tls_loopback: amilisp exited rc=$rc"
    echo "$OUT" | tail -20
    exit 1
fi

if echo "$OUT" | grep -q "TLS-LOOPBACK ALL-PASSED"; then
    exit 0
fi

echo "FAIL test_tls_loopback:"
echo "$OUT" | tail -25
exit 1
