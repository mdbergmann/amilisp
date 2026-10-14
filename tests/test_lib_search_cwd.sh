#!/bin/sh
# The runtime library (lib/) must be found when amilisp starts from a
# directory other than the source root:
#   - executable-relative fallback: build/host/amilisp resolves ../../lib/
#     (also through a $PATH symlink) with no environment setup
#   - install prefix (<prefix>/bin/amilisp + <prefix>/lib/amilisp/): resolves
#     ../lib/amilisp/, shims included, with no environment setup
#   - $CLAMIGA_HOME fallback: a bare copied binary finds lib/ via the env var
#   - when nothing is found: boot fails with a diagnostic naming
#     CLAMIGA_HOME instead of a generic REQUIRE error later, and REQUIRE
#     errors name the module they could not find
# Run: sh tests/test_lib_search_cwd.sh [path-to-amilisp]

CLAMIGA="${1:-build/host/amilisp}"
case "$CLAMIGA" in
    /*) ABS_CLAMIGA="$CLAMIGA" ;;
    *)  ABS_CLAMIGA="$(pwd)/$CLAMIGA" ;;
esac
ROOT="$(pwd)"

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
            echo "    got: $(echo "$haystack" | head -5)"
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

EVAL_REQUIRE='(progn (require "gray-streams") (format t "GS-OK R=~A~%" (+ 1 2)))'

# --- Executable-relative fallback: repo binary run from elsewhere ---

result=$(cd "$WORKDIR" && env -u CLAMIGA_HOME "$ABS_CLAMIGA" --no-userinit \
    --non-interactive --eval "$EVAL_REQUIRE" </dev/null 2>&1)
check_contains     "exedir_boot_and_require"  "GS-OK R=3" "$result"
check_not_contains "exedir_no_boot_error"     "cannot locate its runtime library" "$result"

# --- Executable-relative fallback through a $PATH-style symlink ---

ln -s "$ABS_CLAMIGA" "$WORKDIR/clamiga-link" 2>/dev/null
if [ -L "$WORKDIR/clamiga-link" ]; then
    result=$(cd "$WORKDIR" && env -u CLAMIGA_HOME ./clamiga-link --no-userinit \
        --non-interactive --eval "$EVAL_REQUIRE" </dev/null 2>&1)
    check_contains "symlink_boot_and_require" "GS-OK R=3" "$result"
else
    # MSYS2 without Developer Mode copies instead of linking, and a copy
    # would re-test the $CLAMIGA_HOME leg below rather than symlink
    # resolution.
    echo "  skip  symlink_boot_and_require (no symlink support on this host)"
fi

# --- Installed FHS layout: <prefix>/bin/amilisp + <prefix>/lib/amilisp/ ---
# An install prefix (the layout SBCL uses): the binary in bin/, the whole
# lib/ tree beside it in ../lib/amilisp/.  Must work with no env var and from a
# cwd that has no lib/ of its own — the only lib/ in reach here is the
# installed one (cwd has none, $CLAMIGA_HOME is unset, <exedir>/lib/ and
# <exedir>/../../lib/ do not exist), so finding gray-streams proves the
# ../lib/amilisp/ leg resolved.

mkdir -p "$WORKDIR/prefix/bin" "$WORKDIR/prefix/lib"
cp -pR "$ROOT/lib" "$WORKDIR/prefix/lib/amilisp"
cp "$ABS_CLAMIGA" "$WORKDIR/prefix/bin/amilisp"
result=$(cd "$WORKDIR" && env -u CLAMIGA_HOME "$WORKDIR/prefix/bin/amilisp" \
    --no-userinit --non-interactive --eval "$EVAL_REQUIRE" </dev/null 2>&1)
check_contains     "fhs_install_boot_and_require" "GS-OK R=3" "$result"
check_not_contains "fhs_install_no_boot_error"    "cannot locate its runtime library" "$result"

# The same prefix layout as installed BEFORE the rename to AmiLisp (0.12):
# <prefix>/lib/clamiga/.  An existing installation that only swapped the
# binary must keep booting, so ../lib/clamiga/ stays a leg of the search.
mkdir -p "$WORKDIR/prefix-old/bin" "$WORKDIR/prefix-old/lib"
cp -pR "$ROOT/lib" "$WORKDIR/prefix-old/lib/clamiga"
cp "$ABS_CLAMIGA" "$WORKDIR/prefix-old/bin/amilisp"
result=$(cd "$WORKDIR" && env -u CLAMIGA_HOME "$WORKDIR/prefix-old/bin/amilisp" \
    --no-userinit --non-interactive --eval "$EVAL_REQUIRE" </dev/null 2>&1)
check_contains     "fhs_install_pre_rename_lib_clamiga" "GS-OK R=3" "$result"

# The bundled ASDF shims live under the installed lib/ too — they are
# resolved from asdf.lisp's own load truename, so they must land inside
# <prefix>/lib/amilisp/shims/ rather than in the source tree.
result=$(cd "$WORKDIR" && env -u CLAMIGA_HOME "$WORKDIR/prefix/bin/amilisp" \
    --no-userinit --non-interactive --eval '(require "asdf")' \
    --eval '(dolist (d (symbol-value (find-symbol "*CENTRAL-REGISTRY*" "ASDF"))) (format t "REG ~A~%" d))' \
    </dev/null 2>&1)
# (matched by tail, not by absolute path: macOS resolves the temp dir's
# /var -> /private/var symlink in the truename.  "lib/amilisp/shims/" only
# exists in the installed tree — the source tree has "lib/shims/".)
check_contains "fhs_install_shims_under_prefix" \
    "/prefix/lib/amilisp/shims/cl+ssl/" "$result"

# --- $CLAMIGA_HOME fallback: bare copied binary, lib/ nowhere nearby ---

cp "$ABS_CLAMIGA" "$WORKDIR/clamiga-copy"
result=$(cd "$WORKDIR" && CLAMIGA_HOME="$ROOT" ./clamiga-copy --no-userinit \
    --non-interactive --eval "$EVAL_REQUIRE" </dev/null 2>&1)
check_contains "clamiga_home_boot_and_require" "GS-OK R=3" "$result"

# --- Nothing findable: clear diagnostics instead of a generic error ---

result=$(cd "$WORKDIR" && env -u CLAMIGA_HOME ./clamiga-copy --no-userinit \
    --non-interactive --eval '(require "gray-streams")' </dev/null 2>&1)
check_contains "lost_boot_names_problem"   "cannot locate its runtime library" "$result"
check_contains "lost_boot_names_fix"       "CLAMIGA_HOME" "$result"
check_contains "lost_require_names_module" 'cannot find module "gray-streams"' "$result"

# --- Boot candidate exists but fails to LOAD (e.g. reader error) ---
# Regression for two bugs found via the first MorphOS run (where the C-stack
# guard aborted the boot load): (1) the swallowed load error must be
# reported, not misdiagnosed as "cannot locate"; (2) the error unwind to the
# outermost CL_CATCH must restore the catch site's GC-root snapshot —
# it used to zero it, wiping repl-init's protected saved_pkg root and
# aborting with "pop_roots(1): gc_root_count went negative".

mkdir -p "$WORKDIR/poison/lib"
printf '(defmacro dummy () nil)\n((((( \n' > "$WORKDIR/poison/lib/boot.lisp"
cp "$ABS_CLAMIGA" "$WORKDIR/poison/clamiga-poison"
result=$(cd "$WORKDIR/poison" && env -u CLAMIGA_HOME ./clamiga-poison --no-userinit \
    --non-interactive --eval '(list 1 2)' </dev/null 2>&1)
check_contains     "poisoned_boot_names_load_failure" "but loading it FAILED" "$result"
check_contains     "poisoned_boot_still_reports_no_runtime" "cannot locate its runtime library" "$result"
check_not_contains "poisoned_boot_no_gc_root_abort"   "GC-ROOT-BUG" "$result"
check_not_contains "poisoned_boot_no_crash"           "FATAL" "$result"

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
