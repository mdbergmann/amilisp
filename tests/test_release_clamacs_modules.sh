#!/bin/sh
# The binary release compiles the editor's modules to lib/clamacs/*.fasl
# with the MUI frontend set and then insists that every module got one --
# except the sources that are no module on an Amiga: load.lisp, the
# run-from-source entry point clamacs.lisp, and the HOST editor's own
# modules (the webview frontend, its port, the TCP wire), which
# clamacs.lisp loads only off the Amiga.  This pins the two lists to each
# other: a host-only module clamacs.lisp adds must be exempted in
# scripts/make-binary-release.sh (or the release fails at the check, as
# 0.11's first snapshot did), and no exemption may name a module that
# clamacs.lisp does not treat as host-only (it would hide a missing FASL).
#
# Run: sh tests/test_release_clamacs_modules.sh   (no amilisp binary needed)

cd "$(dirname "$0")/.." || exit 1
SCRIPT=scripts/make-binary-release.sh
ENTRY=clamacs/lisp/clamacs.lisp
if [ ! -f "$ENTRY" ]; then
    echo "skip: test_release_clamacs_modules (clamacs submodule not checked out)"
    exit 0
fi

passed=0; failed=0
ok()   { passed=$((passed + 1)); echo "  ok    $1"; }
fail() { failed=$((failed + 1)); echo "  FAIL  $1"; }

# clamacs.lisp: the quoted names on the line(s) after the :amigaos branch of
# the frontend-files IF -- the set bound when :amigaos is NOT a feature.
host_set=$(awk '/frontend-mui/ {seen=1; next} seen && /\(/ {print; exit}' "$ENTRY" \
           | tr -c 'a-z0-9-\n' '\n' | grep -E '^(frontend|transport)-' | sort)
# the release script: the names of the case arm that skips the FASL check,
# minus the two non-modules.
exempt=$(grep -E '^\s*load\|clamacs\|.*\) continue ;;' "$SCRIPT" \
         | sed -e 's/^[[:space:]]*//' -e 's/) continue ;;.*//' | tr '|' '\n' \
         | grep -vE '^(load|clamacs)$' | sort)

echo "=== test_release_clamacs_modules ==="
[ -n "$host_set" ] && ok "clamacs.lisp names the host-only module set: $(echo $host_set)" \
                   || fail "could not read the host-only set from $ENTRY"
[ -n "$exempt" ] && ok "$SCRIPT exempts: $(echo $exempt)" \
                 || fail "could not read the exempted names from $SCRIPT"
if [ "$host_set" = "$exempt" ]; then
    ok "the release script exempts exactly clamacs.lisp's host-only modules"
else
    fail "host-only set and release exemptions differ"
    echo "        clamacs.lisp: $(echo $host_set)"
    echo "        release:      $(echo $exempt)"
fi
for n in $exempt; do
    [ -f "clamacs/lisp/$n.lisp" ] && ok "clamacs/lisp/$n.lisp exists" \
                                  || fail "exempted module clamacs/lisp/$n.lisp does not exist"
done
# and the Amiga set the release compiles is still what clamacs.lisp binds there
for n in frontend-mui transport-arexx; do
    grep -q "\"$n\"" "$SCRIPT" && ok "release compiles $n" || fail "release no longer compiles $n"
done

echo "test_release_clamacs_modules: $passed passed, $failed failed"
[ $failed -eq 0 ]
