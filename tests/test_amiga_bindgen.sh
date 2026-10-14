#!/bin/sh
# Executable specification of scripts/gen-amiga-bindings.lisp — the
# generator behind lib/amiga/raw/ (the raw AmigaOS/MorphOS API bindings).
#
# Part 1 runs the generator on the small SDK fixture under
# tests/fixtures/bindgen/ (an NDK-style SFD, a MorphOS-style SFD with
# private slots, a moved function, a sysv entry and a MorphOS-only library,
# two assembler includes, a .gadget and a .class library whose tags live
# in C headers, a twin-less header other headers include, a C twin of a
# .i that must be ignored, and a MUI-SDK-style fd2sfd rendering with
# ##private gaps plus a libraries/mui.h under a SECOND C-header root) and
# checks the output with tests/test_amiga_bindgen.lisp: LVO assignment
# (==bias/==reserve/varargs/alias/private gaps), register and result-kind
# encoding, the >7-register plist path, skips (DOUBLE, A5, register pairs,
# sysv), version and platform guards, CL-name shadowing, constant
# evaluation (expressions, BITDEF, ENUM, DEVCMD, LIBDEF, forward and
# cross-file references), struct layouts, the class-library module paths
# (gadgets/ images/ classes/) and the C header reader (#define
# expressions, casts, suffixes, conditionals, #undef, enums, string
# constants, what is skipped).  A second generator run WITHOUT the MUI
# SDK pins the fallback: muimaster from the MorphOS function table alone.
#
# Part 2 loads every COMMITTED module in lib/amiga/raw/ on the host and
# checks well-known OS values, so a stale or hand-edited generated file
# fails the build.
#
# Both parts run twice: plain, and with :MORPHOS pushed on *FEATURES*, so
# both sides of every platform conditional execute.
#
# Run: sh tests/test_amiga_bindgen.sh build/host/amilisp

CLAMIGA="${1:-build/host/amilisp}"
ROOT=$(cd "$(dirname "$0")/.." && pwd)
FIX="$ROOT/tests/fixtures/bindgen"
CHECK="$ROOT/tests/test_amiga_bindgen.lisp"
TMPD=$(mktemp -d "${TMPDIR:-/tmp}/test_amiga_bindgen_XXXXXX") || exit 1
trap 'rm -rf "$TMPD"' EXIT INT TERM

fail=0

run_checks() {
    # $1 = mode, $2 = RAWDIR, $3 = "morphos" to push :MORPHOS first (or empty)
    if [ "$3" = morphos ]; then
        out=$(BINDGEN_CHECK="$1" RAWDIR="$2" "$CLAMIGA" --no-userinit --non-interactive \
              --heap 256M --eval "(push :morphos *features*)" --load "$CHECK" \
              </dev/null 2>&1 | grep -v '^; Loading')
    else
        out=$(BINDGEN_CHECK="$1" RAWDIR="$2" "$CLAMIGA" --no-userinit --non-interactive \
              --heap 256M --load "$CHECK" </dev/null 2>&1 | grep -v '^; Loading')
    fi
    # ok/FAIL lines, plus anything amilisp's LOAD recovery printed (a check
    # that errors out names its condition on its FAIL line; an ERROR line
    # here means a top-level form died and the checks after it never ran)
    echo "$out" | grep -E '^(FAIL|ok  |ERROR)' | sed 's/^/  /'
    summary=$(echo "$out" | grep '^BINDGEN-RESULT')
    if [ -z "$summary" ]; then
        echo "  FAIL: check script did not finish ($1, $3)"
        echo "$out" | tail -15 | sed 's/^/    /'
        fail=1
        return
    fi
    echo "  $summary"
    case "$summary" in
        *"fail=0"*) ;;
        *) fail=1 ;;
    esac
}

echo "=== test_amiga_bindgen: generator on fixtures ==="
GEN_OUT="$TMPD/out"
gen_log=$(BINDGEN_NDK_SFD="$FIX/sfd" BINDGEN_NDK_INCLUDE="$FIX/include" \
          BINDGEN_NDK_INCLUDE_H="$FIX/include_h" \
          BINDGEN_MOS_SFD="$FIX/mos-sfd" BINDGEN_MOS_ONLY=mosonly \
          BINDGEN_MUI_SFD="$FIX/mui-sfd" BINDGEN_MUI_INCLUDE_H="$FIX/mui-include_h" \
          BINDGEN_MUI5_SFD="$FIX/mui5-sfd" BINDGEN_MUI5_INCLUDE_H="$FIX/mui5-include_h" \
          BINDGEN_MOS_MUI_INCLUDE_H="$FIX/mos-mui-include_h" \
          BINDGEN_MCC_INCLUDE_H="$FIX/mcc-include_h" \
          BINDGEN_OUT="$GEN_OUT" \
          "$CLAMIGA" --no-userinit --non-interactive --heap 64M \
          --load "$ROOT/scripts/gen-amiga-bindings.lisp" </dev/null 2>&1 | grep -v '^; Loading')
echo "$gen_log" | sed 's/^/  /'
if ! echo "$gen_log" | grep -q '^9 modules:'; then
    echo "  FAIL: expected 9 modules (example, mosonly, muimaster, exec/exbase, gadgets/fixgad, classes/fixreq, reaction/reaction, mui/fixlist, mui/fixed)"
    fail=1
fi
if ! echo "$gen_log" | grep -q '^MCC headers: '; then
    echo "  FAIL: a run with BINDGEN_MCC_INCLUDE_H must name the MCC header root"
    fail=1
fi
if echo "$gen_log" | grep -q 'warnings:'; then
    echo "  FAIL: generator reported warnings on the fixture (must be clean)"
    fail=1
fi
# the MUI fd is checked against the MorphOS rendering of the same library,
# and against the MUI 5 SDK's (both cross-checks see the pristine 3.8
# table -- MUI_Show joins only afterwards)
if ! echo "$gen_log" | grep -q '^LVO cross-check MUI SDK vs MorphOS SDK: 5 functions agree, 0 differ'; then
    echo "  FAIL: expected the MUI-vs-MorphOS LVO cross-check to report 5 agreeing functions"
    fail=1
fi
if ! echo "$gen_log" | grep -q '^LVO cross-check MUI SDK vs MUI 5 SDK: 5 functions agree, 0 differ'; then
    echo "  FAIL: expected the MUI-vs-MUI5 LVO cross-check to report 5 agreeing functions"
    fail=1
fi
if ! echo "$gen_log" | grep -q '^MUI 5 SDK: muimaster gains 3 post-3.8 functions ((%version>= 20))'; then
    echo "  FAIL: expected 3 post-3.8 functions from the MUI 5 SDK sfd"
    fail=1
fi
if ! echo "$gen_log" | grep -q '^MUI 5 SDK libraries/mui.h: 5 new constants (additive), 3 skipped'; then
    echo "  FAIL: expected 5 new constants (3 skipped) from the MUI 5 header"
    fail=1
fi
if ! echo "$gen_log" | grep -q '^MorphOS SDK libraries/mui.h: 1 new constants (additive)'; then
    echo "  FAIL: expected 1 new constant from the MorphOS header"
    fail=1
fi
if ! echo "$gen_log" | grep -q '^MUI additive headers: 1 known value evolution kept at the value of the earliest source (MUIMASTER_VMIN)'; then
    echo "  FAIL: expected the MUIMASTER_VMIN evolution to be reported"
    fail=1
fi
for f in example.lisp mosonly.lisp muimaster.lisp exec/exbase.lisp gadgets/fixgad.lisp \
         classes/fixreq.lisp reaction/reaction.lisp mui/fixlist.lisp mui/fixed.lisp; do
    [ -f "$GEN_OUT/$f" ] || { echo "  FAIL: $f not generated"; fail=1; }
done
# class libraries are NOT top-level modules, a header with a .i twin
# yields no module of its own, the MUI header is claimed by muimaster, and
# an MCC module is named after the class, not the header (mui/fixed for
# MUI/Fixed_mcc.h — the directory's spelling is checked by the Lisp side,
# a file test here would pass on a case-insensitive file system)
for f in fixgad.lisp fixreq.lisp libraries/example.lisp libraries/mui.lisp \
         mui/Fixlist_mcc.lisp mui/fixlist-mcc.lisp; do
    [ -f "$GEN_OUT/$f" ] && { echo "  FAIL: $f must not be generated"; fail=1; }
done
run_checks fixture "$GEN_OUT" ""
run_checks fixture "$GEN_OUT" morphos

echo "=== test_amiga_bindgen: generator without the MUI SDK ==="
# muimaster then comes from the MorphOS SDK alone (on the MorphOS-only
# allowlist, as in the default configuration): function table, no
# constants, and the run says so
GEN_OUT2="$TMPD/out-nomui"
gen_log2=$(BINDGEN_NDK_SFD="$FIX/sfd" BINDGEN_NDK_INCLUDE="$FIX/include" \
           BINDGEN_NDK_INCLUDE_H="$FIX/include_h" \
           BINDGEN_MOS_SFD="$FIX/mos-sfd" BINDGEN_MOS_ONLY=mosonly,muimaster \
           BINDGEN_OUT="$GEN_OUT2" \
           "$CLAMIGA" --no-userinit --non-interactive --heap 64M \
           --load "$ROOT/scripts/gen-amiga-bindings.lisp" </dev/null 2>&1 | grep -v '^; Loading')
if ! echo "$gen_log2" | grep -q '^MUI SDK: none'; then
    echo "  FAIL: a run without BINDGEN_MUI_SFD must say 'MUI SDK: none'"
    fail=1
fi
if ! echo "$gen_log2" | grep -q '^7 modules:'; then
    echo "  FAIL: expected 7 modules without the MUI SDK (muimaster from the MorphOS allowlist)"
    fail=1
fi
if echo "$gen_log2" | grep -q 'warnings:'; then
    echo "  FAIL: generator reported warnings on the fixture without the MUI SDK"
    fail=1
fi
if [ -f "$GEN_OUT2/muimaster.lisp" ]; then
    if ! grep -q '^;;; 7 functions, 0 constants, 0 structs\.$' "$GEN_OUT2/muimaster.lisp"; then
        echo "  FAIL: without the MUI SDK muimaster must carry 7 functions and 0 constants"
        fail=1
    fi
    if grep -q 'MUI 3.8 SDK\|libraries/mui.h' "$GEN_OUT2/muimaster.lisp"; then
        echo "  FAIL: without the MUI SDK muimaster must not name it as a source"
        fail=1
    fi
    if ! grep -q '^;;;   MorphOS SDK muimaster_lib.fd' "$GEN_OUT2/muimaster.lisp"; then
        echo "  FAIL: without the MUI SDK muimaster must name the MorphOS SDK as its source"
        fail=1
    fi
    if ! grep -q '(:fn "MUI-GET-RGB-COLOR" -690 (:a0 :a1 :a2) :signed)' "$GEN_OUT2/muimaster.lisp"; then
        echo "  FAIL: without the MUI SDK the whole library is MorphOS-only: no :morphos guard on MUI_GetRGBColor"
        fail=1
    fi
    echo "  ok   muimaster without the MUI SDK: MorphOS function table only"
else
    echo "  FAIL: muimaster.lisp not generated without the MUI SDK"
    fail=1
fi

echo "=== test_amiga_bindgen: additive header conflicting with the baseline ==="
# a differing value for an existing name that is NOT a known evolution
# must name the conflict and stop the generator before anything is written
GEN_OUT3="$TMPD/out-conflict"
gen_log3=$(BINDGEN_NDK_SFD="$FIX/sfd" BINDGEN_NDK_INCLUDE="$FIX/include" \
           BINDGEN_NDK_INCLUDE_H="$FIX/include_h" \
           BINDGEN_MUI_SFD="$FIX/mui-sfd" BINDGEN_MUI_INCLUDE_H="$FIX/mui-include_h" \
           BINDGEN_MOS_MUI_INCLUDE_H="$FIX/mui-conflict_h" \
           BINDGEN_OUT="$GEN_OUT3" \
           "$CLAMIGA" --no-userinit --non-interactive --heap 64M \
           --load "$ROOT/scripts/gen-amiga-bindings.lisp" </dev/null 2>&1 | grep -v '^; Loading')
if ! echo "$gen_log3" | grep -q '^CONFLICT MUIA_Window_CloseRequest: baseline 2151868526, 305419896 from the MorphOS SDK'; then
    echo "  FAIL: expected the conflicting MUIA_Window_CloseRequest to be named"
    fail=1
fi
if ! echo "$gen_log3" | grep -q 'differing value for existing MUI names'; then
    echo "  FAIL: expected the generator to refuse on a conflicting additive value"
    fail=1
fi
if [ -f "$GEN_OUT3/muimaster.lisp" ]; then
    echo "  FAIL: no output may be written on a conflict"
    fail=1
fi
[ $fail -eq 0 ] && echo "  ok   conflicting additive value refused, nothing written"

echo "=== test_amiga_bindgen: committed lib/amiga/raw ==="
run_checks committed "$ROOT/lib/amiga/raw" ""
run_checks committed "$ROOT/lib/amiga/raw" morphos

if [ $fail -eq 0 ]; then
    echo "test_amiga_bindgen: PASS"
    exit 0
else
    echo "test_amiga_bindgen: FAIL"
    exit 1
fi
