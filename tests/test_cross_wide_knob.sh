#!/bin/sh
# Makefile.cross WIDE=1 knob: flag/object-dir wiring, checked via dry-run.
# Run: sh tests/test_cross_wide_knob.sh   (no m68k toolchain needed — make -n
# only prints recipes, so check-toolchain never executes)
#
# The knob contract (Makefile.cross):
#   default      -> build/cross,          no -DCL_WIDE_STRINGS  (narrow 8-bit)
#   WIDE=1       -> build/cross-wide,     -DCL_WIDE_STRINGS
#   FPU=1        -> build/cross-fpu,      no -DCL_WIDE_STRINGS
#   WIDE=1 FPU=1 -> build/cross-fpu-wide, -DCL_WIDE_STRINGS -m68881
#   default      -> -DCL_GC_IMMIX, gc_immix.c (the m68k default collector)
#   GC=classic   -> build/cross-classic,  -DCL_GC_CLASSIC, gc_classic.c in
#                   place of gc_immix.c (composes: FPU=1 GC=classic ->
#                   cross-fpu-classic, WIDE=1 FPU=1 GC=classic -> cross-fpu-wide-classic)
# Every combination has its own object dir so variants never mix.

passed=0
failed=0

check() {
    desc="$1"
    ok="$2"
    if [ "$ok" = "yes" ]; then
        echo "  ok  $desc"
        passed=$((passed + 1))
    else
        echo "  FAIL  $desc"
        failed=$((failed + 1))
    fi
}

dryrun() {
    # -n: print recipes without running them (works without the m68k
    # toolchain — check-toolchain never executes).  -B: treat everything as
    # out of date, so the full command list prints even when the variant's
    # object dir exists and is current.
    # MAKEFLAGS/MFLAGS and the knob variables cleared: `make test GC=immix`
    # (or FPU=1, WIDE=1) hands its command-line overrides to every sub-make
    # through MAKEFLAGS AND exports them into the recipe's environment,
    # where a `GC ?=` default would pick them up — and this test is about
    # Makefile.cross's OWN defaults.
    # shellcheck disable=SC2086
    env -u MAKEFLAGS -u MFLAGS -u GC -u FPU -u WIDE \
        make -f Makefile.cross -nB amiga $1 2>/dev/null
}

out=$(dryrun "")
check "default: objects under build/cross/" \
    "$(echo "$out" | grep -q 'build/cross/' && echo yes)"
check "default: no -DCL_WIDE_STRINGS" \
    "$(echo "$out" | grep -q 'DCL_WIDE_STRINGS' || echo yes)"
check "default: no -wide object dir" \
    "$(echo "$out" | grep -q 'cross-wide\|cross-fpu-wide' || echo yes)"

out=$(dryrun "WIDE=1")
check "WIDE=1: -DCL_WIDE_STRINGS present" \
    "$(echo "$out" | grep -q 'DCL_WIDE_STRINGS' && echo yes)"
check "WIDE=1: objects under build/cross-wide/" \
    "$(echo "$out" | grep -q 'build/cross-wide/' && echo yes)"

out=$(dryrun "FPU=1")
check "FPU=1 alone: no -DCL_WIDE_STRINGS" \
    "$(echo "$out" | grep -q 'DCL_WIDE_STRINGS' || echo yes)"
check "FPU=1 alone: objects under build/cross-fpu/ (not -wide)" \
    "$(echo "$out" | grep -q 'build/cross-fpu/' && echo yes)"

out=$(dryrun "WIDE=1 FPU=1")
check "WIDE=1 FPU=1: -DCL_WIDE_STRINGS present" \
    "$(echo "$out" | grep -q 'DCL_WIDE_STRINGS' && echo yes)"
check "WIDE=1 FPU=1: -m68881 present" \
    "$(echo "$out" | grep -q 'm68881' && echo yes)"
check "WIDE=1 FPU=1: objects under build/cross-fpu-wide/" \
    "$(echo "$out" | grep -q 'build/cross-fpu-wide/' && echo yes)"

# The GC knob (specs/gc-backend-immix.md): one backend per binary, its own
# object dir, composing with the other variants.
out=$(dryrun "")
check "default: -DCL_GC_IMMIX, gc_immix.c compiled (the m68k default)" \
    "$(echo "$out" | grep -q 'DCL_GC_IMMIX' && echo "$out" | grep -q 'core/gc_immix.c' && echo yes)"
check "default: no gc_classic.c, no -DCL_GC_CLASSIC, no -classic object dir" \
    "$(echo "$out" | grep -q 'core/gc_classic.c\|DCL_GC_CLASSIC\|cross-classic' || echo yes)"

out=$(dryrun "GC=classic")
check "GC=classic: -DCL_GC_CLASSIC, gc_classic.c compiled" \
    "$(echo "$out" | grep -q 'DCL_GC_CLASSIC' && echo "$out" | grep -q 'core/gc_classic.c' && echo yes)"
check "GC=classic: no gc_immix.c, no -DCL_GC_IMMIX" \
    "$(echo "$out" | grep -q 'core/gc_immix.c\|DCL_GC_IMMIX' || echo yes)"
check "GC=classic: objects under build/cross-classic/" \
    "$(echo "$out" | grep -q 'build/cross-classic/' && echo yes)"

out=$(dryrun "GC=immix")
check "GC=immix (explicit): objects under build/cross/, no suffix" \
    "$(echo "$out" | grep -q 'build/cross/' && echo "$out" | grep -q 'DCL_GC_IMMIX' && echo yes)"

out=$(dryrun "FPU=1 GC=classic")
check "FPU=1 GC=classic: objects under build/cross-fpu-classic/, -m68881, -DCL_GC_CLASSIC" \
    "$(echo "$out" | grep -q 'build/cross-fpu-classic/' && echo "$out" | grep -q 'm68881' && echo "$out" | grep -q 'DCL_GC_CLASSIC' && echo yes)"

out=$(dryrun "WIDE=1 FPU=1 GC=classic")
check "WIDE=1 FPU=1 GC=classic: objects under build/cross-fpu-wide-classic/" \
    "$(echo "$out" | grep -q 'build/cross-fpu-wide-classic/' && echo yes)"

check "GC=bogus: refused" \
    "$(dryrun "GC=bogus" >/dev/null 2>&1 || echo yes)"

echo "test_cross_wide_knob: $passed passed, $failed failed"
[ "$failed" -eq 0 ]
