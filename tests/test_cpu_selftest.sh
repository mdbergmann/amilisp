#!/bin/sh
# The CPU store self-test: at startup amilisp replays the five-store
# prologue chain the Apollo 68080 (Vampire V4, core 10760) is known to
# drop, warns loudly when a store is lost and puts :CPU-LOST-STORES on
# *FEATURES*; (ext:%cpu-store-selftest &optional rounds) repeats the test.
# The host has no such defect (and no m68k code), so two contracts are
# pinned here:
#   the quiet side -- no warning line, no feature, 0 from the probe at any
#     round count, a type error for a bad ROUNDS, and the AMILISP_CPU_CHECK=0
#     skip changing nothing visible;
#   the positive side, through AMILISP_CPU_CHECK=<n> (n > 0 makes the sound
#     host report n lost stores at startup, and says so): the warning text,
#     the feature, and -- the reason for the heap-image legs -- that a
#     restored image gets THIS process's verdict on the CPU, not the
#     saver's (an image carries the saving process's *FEATURES*).
# tests/amiga/run-tests.lisp runs the consistency check on the m68k build
# (FS-UAE: 0; a Vampire: > 0 AND the feature), where the asm chain actually
# executes.
# Run: sh tests/test_cpu_selftest.sh [path-to-amilisp]

AMILISP="${1:-build/host/amilisp}"
case "$AMILISP" in
    /*|[A-Za-z]:/*) ;;
    *) AMILISP="$(pwd)/$AMILISP" ;;
esac
passed=0
failed=0
total=0

check_contains() {
    desc="$1"; needle="$2"; haystack="$3"
    total=$((total + 1))
    case "$haystack" in
        *"$needle"*) echo "  ok  $desc"; passed=$((passed + 1)) ;;
        *) echo "  FAIL  $desc"; echo "    expected to contain: $needle"
           echo "    got: $(echo "$haystack" | head -8)"; failed=$((failed + 1)) ;;
    esac
}
check_lacks() {
    desc="$1"; needle="$2"; haystack="$3"
    total=$((total + 1))
    case "$haystack" in
        *"$needle"*) echo "  FAIL  $desc"; echo "    must not contain: $needle"
           echo "    got: $(echo "$haystack" | head -8)"; failed=$((failed + 1)) ;;
        *) echo "  ok  $desc"; passed=$((passed + 1)) ;;
    esac
}

run() {
    "$AMILISP" --no-userinit --non-interactive --heap 4M "$@" </dev/null 2>&1
}

FEAT='(format t "~&FEAT ~A~%" (count :cpu-lost-stores *features*))'

# --- The quiet side: a sound CPU ------------------------------------------

out=$(run --eval '(format t "~&PROBE ~A ~A ~A~%" (ext:%cpu-store-selftest) (ext:%cpu-store-selftest 1) (ext:%cpu-store-selftest 100000))' \
          --eval "$FEAT")
check_contains "the probe answers 0 at the default, 1 and 100000 rounds" "PROBE 0 0 0" "$out"
check_contains ":CPU-LOST-STORES is not on *FEATURES*" "FEAT 0" "$out"
check_lacks "no lost-stores warning at startup" "LOSES MEMORY STORES" "$out"

out=$(run --eval '(handler-case (ext:%cpu-store-selftest 0) (type-error (e) (format t "~&TE0 ~A~%" e)))' \
          --eval '(handler-case (ext:%cpu-store-selftest :many) (type-error (e) (format t "~&TEK ~A~%" e)))')
check_contains "ROUNDS 0 is a type error that names the argument" "TE0 %CPU-STORE-SELFTEST: ROUNDS must be a positive fixnum, got 0" "$out"
check_contains "a non-fixnum ROUNDS is a type error that shows the value" "TEK %CPU-STORE-SELFTEST: ROUNDS must be a positive fixnum, got :MANY" "$out"

out=$(AMILISP_CPU_CHECK=0 run --eval '(format t "~&SKIP ~A~%" (ext:%cpu-store-selftest))' --eval "$FEAT")
check_contains "AMILISP_CPU_CHECK=0: the probe still works, the feature stays off" "SKIP 0" "$out"
check_contains "AMILISP_CPU_CHECK=0: :CPU-LOST-STORES is not on *FEATURES*" "FEAT 0" "$out"
check_lacks "AMILISP_CPU_CHECK=0: nothing printed either" "LOSES MEMORY STORES" "$out"

# Only a number simulates: anything else is the normal test, which is quiet here.
for v in abc 3x -2 "" " 4"; do
    out=$(AMILISP_CPU_CHECK="$v" run --eval "$FEAT")
    check_lacks "AMILISP_CPU_CHECK='$v' is not a simulation: no warning" "LOSES MEMORY STORES" "$out"
    check_contains "AMILISP_CPU_CHECK='$v' is not a simulation: no feature" "FEAT 0" "$out"
done

# --- The positive side: AMILISP_CPU_CHECK=<n> simulates n lost stores -----

out=$(AMILISP_CPU_CHECK=7 run --eval '(format t "~&PROBE ~A~%" (ext:%cpu-store-selftest))' --eval "$FEAT")
check_contains "AMILISP_CPU_CHECK=7: the warning names the defect and the count" "THIS CPU LOSES MEMORY STORES: 7 of 4096 replays" "$out"
check_contains "AMILISP_CPU_CHECK=7: the warning points at the feature" ":CPU-LOST-STORES is on *FEATURES*" "$out"
check_contains "AMILISP_CPU_CHECK=7: the warning says it is simulated" "SIMULATED" "$out"
check_contains "AMILISP_CPU_CHECK=7: :CPU-LOST-STORES is on *FEATURES* exactly once" "FEAT 1" "$out"
check_contains "AMILISP_CPU_CHECK=7: the probe itself still answers what the CPU did (0)" "PROBE 0" "$out"

out=$(AMILISP_CPU_CHECK=99999 run --eval "$FEAT")
check_contains "AMILISP_CPU_CHECK=99999: the count is clamped to the replays run" "STORES: 4096 of 4096 replays" "$out"

out=$(AMILISP_CPU_CHECK=1 run --eval '(format t "~&KW ~S ~A~%" (find-symbol "CPU-LOST-STORES" "KEYWORD") (eq :cpu-lost-stores (car *features*)))')
check_contains "AMILISP_CPU_CHECK=1: the feature is the keyword, at the front of the list" "KW :CPU-LOST-STORES T" "$out"

# --- Heap images: the restored session gets THIS process's verdict --------
# An image carries the SAVING process's *FEATURES*.  The shipped amilisp.img
# is saved on a sound machine (FS-UAE) and started on a Vampire that loses
# stores; an image saved on that Vampire must not tell a sound machine the
# same.  Both directions, and the unchanged cases.

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)
if [ -z "$TIMEOUT" ]; then
    echo "  SKIP  heap-image legs: neither timeout nor gtimeout on PATH"
else
    WORK=$(mktemp -d "${TMPDIR:-/tmp}/amilisp_cpuself_XXXXXX") || exit 1
    trap 'rm -rf "$WORK"' EXIT INT TERM
    cd "$WORK" || exit 1

    IMGCLI="--no-userinit --no-image --heap 8M --non-interactive"
    RESTORED='(format t "~&RESTORED ~A FEAT ~A~%" ext:*image-restored-p* (count :cpu-lost-stores *features*))'

    out=$("$TIMEOUT" 60 "$AMILISP" $IMGCLI --eval "$FEAT" \
          --eval '(ext:save-image "sound.img")' </dev/null 2>&1)
    check_contains "the sound saver has no :CPU-LOST-STORES" "FEAT 0" "$out"
    check_contains "the sound saver writes its image" "Image saved" "$out"

    out=$(AMILISP_CPU_CHECK=5 "$TIMEOUT" 60 "$AMILISP" $IMGCLI --eval "$FEAT" \
          --eval '(ext:save-image "flagged.img")' </dev/null 2>&1)
    check_contains "the flagged saver has :CPU-LOST-STORES" "FEAT 1" "$out"
    check_contains "the flagged saver writes its image" "Image saved" "$out"

    out=$("$TIMEOUT" 60 "$AMILISP" --no-userinit --image sound.img --non-interactive \
          --eval "$RESTORED" </dev/null 2>&1)
    check_contains "sound image, sound CPU: no :CPU-LOST-STORES" "RESTORED T FEAT 0" "$out"
    check_lacks "sound image, sound CPU: no warning" "LOSES MEMORY STORES" "$out"

    out=$(AMILISP_CPU_CHECK=5 "$TIMEOUT" 60 "$AMILISP" --no-userinit --image sound.img \
          --non-interactive --eval "$RESTORED" </dev/null 2>&1)
    check_contains "sound image, flagged CPU: the restore adds :CPU-LOST-STORES" "RESTORED T FEAT 1" "$out"
    check_contains "sound image, flagged CPU: with the warning" "THIS CPU LOSES MEMORY STORES: 5 of 4096 replays" "$out"

    out=$("$TIMEOUT" 60 "$AMILISP" --no-userinit --image flagged.img --non-interactive \
          --eval "$RESTORED" </dev/null 2>&1)
    check_contains "flagged image, sound CPU: the restore drops the saver's :CPU-LOST-STORES" "RESTORED T FEAT 0" "$out"
    check_lacks "flagged image, sound CPU: no warning" "LOSES MEMORY STORES" "$out"

    out=$(AMILISP_CPU_CHECK=5 "$TIMEOUT" 60 "$AMILISP" --no-userinit --image flagged.img \
          --non-interactive --eval "$RESTORED" </dev/null 2>&1)
    check_contains "flagged image, flagged CPU: :CPU-LOST-STORES exactly once" "RESTORED T FEAT 1" "$out"

    out=$(AMILISP_CPU_CHECK=0 "$TIMEOUT" 60 "$AMILISP" --no-userinit --image flagged.img \
          --non-interactive --eval "$RESTORED" </dev/null 2>&1)
    check_contains "flagged image, AMILISP_CPU_CHECK=0: the skipped test leaves the feature off" "RESTORED T FEAT 0" "$out"
fi

echo "test_cpu_selftest: $passed/$total passed"
[ "$failed" -eq 0 ]
