#!/bin/sh
# The bundled shim systems (lib/shims/: the cl+ssl facade and the swank
# stub) must shadow any package-manager-installed copy WITHOUT a
# filesystem installation step, so they work from a binary release and
# for ocicl users alike:
#   - loading ASDF (lib/asdf.lisp) pushes lib/shims/{cl+ssl,swank}/ onto
#     ASDF:*CENTRAL-REGISTRY*, which search-for-system-definition
#     consults BEFORE the searchers Quicklisp and ocicl append
#   - the registration resolves via *LOAD-TRUENAME*, so it must survive
#     every asdf.lisp load mode: source compile AND faslcache hit
#   - it must work in the deployed binary-release layout
#     (bin/aos3/amilisp two levels below the root, lib/ at the root)
#   - AMILISP_NO_SHIMS=1 disables it (to run the real cl+ssl on a host)
# Run: sh tests/test_shim_registry.sh [path-to-amilisp]

AMILISP="${1:-build/host/amilisp}"
case "$AMILISP" in
    /*) ABS_CLAMIGA="$AMILISP" ;;
    *)  ABS_CLAMIGA="$(pwd)/$AMILISP" ;;
esac
. "$(dirname "$0")/shpath.sh"
# Compared against paths amilisp PRINTS, so use amilisp's spelling.
ROOT="$(native_path "$(pwd)")"

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

passed=0
failed=0
total=0

check_contains() {
    desc="$1"
    needle="$2"
    haystack="$3"
    total=$((total + 1))
    case "$haystack" in
        *"$needle"*)
            echo "  ok  $desc"
            passed=$((passed + 1)) ;;
        *)
            echo "  FAIL  $desc"
            echo "    expected to contain: $needle"
            echo "    got: $(echo "$haystack" | tail -5)"
            failed=$((failed + 1)) ;;
    esac
}

check_not_contains() {
    desc="$1"
    needle="$2"
    haystack="$3"
    total=$((total + 1))
    case "$haystack" in
        *"$needle"*)
            echo "  FAIL  $desc"
            echo "    expected NOT to contain: $needle"
            failed=$((failed + 1)) ;;
        *)
            echo "  ok  $desc"
            passed=$((passed + 1)) ;;
    esac
}

# Two separate --eval forms: ASDF symbols are only readable after the
# (require "asdf") form has already been read AND evaluated.
EVAL_REQUIRE='(require "asdf")'
EVAL_RESOLVE='(progn
  (format t "CL+SSL-AT ~a~%"
          (asdf:system-source-directory (asdf:find-system "cl+ssl")))
  (format t "SWANK-AT ~a~%"
          (asdf:system-source-directory (asdf:find-system "swank"))))'

# Isolated FASL cache: keeps the suite independent of the developer's
# ~/.cache and makes the cold-vs-cached distinction below deterministic.
CACHE="$WORKDIR/faslcache"

# --- Source-compile load: cold cache, run from the repo root ---

result=$(AMILISP_FASL_CACHE_DIR="$CACHE" "$ABS_CLAMIGA" --no-userinit \
    --non-interactive --heap 48M \
    --eval "$EVAL_REQUIRE" --eval "$EVAL_RESOLVE" </dev/null 2>&1)
check_contains "cold_resolves_cl+ssl_shim" "CL+SSL-AT $ROOT/lib/shims/cl+ssl/" "$result"
check_contains "cold_resolves_swank_shim"  "SWANK-AT $ROOT/lib/shims/swank/" "$result"

# --- Faslcache-hit load: same cache, asdf.fasl now present.  LOAD binds
# *LOAD-TRUENAME* to the SOURCE path on a cache hit, so the registration
# must still point at lib/shims/ (not into the cache tree). ---

result=$(AMILISP_FASL_CACHE_DIR="$CACHE" "$ABS_CLAMIGA" --no-userinit \
    --non-interactive --heap 48M \
    --eval "$EVAL_REQUIRE" --eval "$EVAL_RESOLVE" </dev/null 2>&1)
check_contains "cached_loads_asdf_fasl"     "asdf.fasl" "$result"
check_contains "cached_resolves_cl+ssl_shim" "CL+SSL-AT $ROOT/lib/shims/cl+ssl/" "$result"

# --- Binary-release layout: binary two levels deep, lib/ at the root,
# run from an unrelated cwd with no AMILISP_HOME (mirrors the release
# smoke test and a real binary install). ---

mkdir -p "$WORKDIR/rel/bin/aos3"
cp "$ABS_CLAMIGA" "$WORKDIR/rel/bin/aos3/amilisp"
cp -R "$ROOT/lib" "$WORKDIR/rel/lib"
result=$(cd "$WORKDIR" && env -u AMILISP_HOME \
    AMILISP_FASL_CACHE_DIR="$CACHE" ./rel/bin/aos3/amilisp --no-userinit \
    --non-interactive --heap 48M \
    --eval "$EVAL_REQUIRE" --eval "$EVAL_RESOLVE" </dev/null 2>&1)
check_contains "release_layout_resolves_shim" "rel/lib/shims/cl+ssl/" "$result"

# --- AMILISP_NO_SHIMS=1: nothing registered; the shims must not resolve
# through the central registry (find-system with error-p nil). ---

result=$(AMILISP_NO_SHIMS=1 AMILISP_FASL_CACHE_DIR="$CACHE" "$ABS_CLAMIGA" \
    --no-userinit --non-interactive --heap 48M \
    --eval "$EVAL_REQUIRE" \
    --eval '(format t "REGISTRY=[~{~a~^ ~}]~%" asdf:*central-registry*)' \
    </dev/null 2>&1)
check_contains     "optout_prints_registry" "REGISTRY=[" "$result"
check_not_contains "optout_registry_empty_of_shims" "lib/shims" "$result"

# --- Summary ---

echo ""
echo "$passed passed, $failed failed, $total total"
if [ "$failed" -gt 0 ]; then
    echo "FAIL"
    exit 1
else
    echo "PASS"
    exit 0
fi
