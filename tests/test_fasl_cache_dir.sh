#!/bin/sh
# Regression test: AMILISP_FASL_CACHE_DIR overrides the FASL auto-cache location.
#
# COMPILE-FILE with no :output-file writes its .fasl to a per-implementation
# auto-cache (make_fasl_cache_path in src/core/builtins_io.c), normally under
# ~/.cache/common-lisp. The load-and-test runner sets AMILISP_FASL_CACHE_DIR to
# give each script its own cache, so scripts that compile the same library under
# different *features* (e.g. :hunchentoot-no-ssl) can't poison one another's
# FASLs. This verifies the override is honoured: the .fasl lands under the
# requested dir, and a different value sends it to a separate dir (isolation).
#
# Fully hermetic — everything stays under $TMP, the real ~/.cache is untouched.
# Run: sh tests/test_fasl_cache_dir.sh [path-to-amilisp]

AMILISP="${1:-build/host/amilisp}"
passed=0
failed=0
TMP="${TMPDIR:-/tmp}/amilisp-cachedir-$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

check() {
    desc="$1"; cond="$2"
    if [ "$cond" = "yes" ]; then
        echo "  ok  $desc"; passed=$((passed + 1))
    else
        echo "  FAIL  $desc"; failed=$((failed + 1))
    fi
}

src="$TMP/cachedir-probe.lisp"
printf '(defun cachedir-probe (x) (+ x 1))\n' > "$src"

compile_into() {
    # $1 = cache dir override; compile $src with no :output-file
    driver="$TMP/drv.lisp"
    printf '(compile-file "%s")\n' "$src" > "$driver"
    AMILISP_FASL_CACHE_DIR="$1" "$AMILISP" --no-userinit --non-interactive \
        --load "$driver" </dev/null >/dev/null 2>&1
}

cacheA="$TMP/cacheA"
cacheB="$TMP/cacheB"

compile_into "$cacheA"
nA=$(find "$cacheA" -name '*.fasl' 2>/dev/null | wc -l | tr -d ' ')
check "fasl_written_under_override_dir" "$([ "$nA" -ge 1 ] && echo yes || echo no)"

# A different override value must land in its own dir (proves the env var, not a
# fixed path, drives the location — the property the per-script isolation relies on).
compile_into "$cacheB"
nB=$(find "$cacheB" -name '*.fasl' 2>/dev/null | wc -l | tr -d ' ')
check "different_override_dir_isolated" "$([ "$nB" -ge 1 ] && echo yes || echo no)"

echo ""
echo "test_fasl_cache_dir: $passed passed, $failed failed"
[ "$failed" -eq 0 ]
