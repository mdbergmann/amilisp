# The Vampire 68080 lost-store defect

A hardware defect of the Apollo 68080 core **10760** (Vampire V4
Standalone, AC68080 at 92 MHz, Kickstart 47.13), found 2026-09-19 while
the Clamacs editor suite failed differently on every launch of that
machine and nowhere else.  Core **11350** does not show it (verified
2026-10-21: the full amilisp suite, the Clamacs drive and a dozen launches
of the startup self-test, all clean).  The fix is a newer core; nothing
in software avoids the defect.

## The defect

The last store of a short chain does not reach memory, or lands late:

    move.l  <absolute>,(a0)+   ; memory-to-memory move, ABSOLUTE source
    clr.l   (a0)+
    clr.l   (a0)               ; <-- stays unwritten

A memory-to-memory move from an absolute address followed by two or more
further stores is what gcc emits for an ordinary function prologue that
initialises locals through a post-incremented pointer, so any C program
is exposed.  What decides whether a given chain loses its store is the
**address of the absolute source modulo 64**: of sixteen sources at
consecutive longword addresses exactly eight lose every replay and eight
never do (on core 10760: offsets 24, 32, 40 and 44..60 lose).  A
program's data hunk lands at a different address on every launch, so one
and the same binary fails on some launches and runs clean on others.
The code address, the destination's alignment, the data cache (`CPU
NODATACACHE`), superscalar mode (`ApolloControl SS=0`) and the compiler
flags make no difference.  Loading the source into a register first, or
an indirect source, is fine; a `nop` or an ALU instruction between the
stores, the displacement form, another address register and word-sized
clears still lose.

In amilisp the victim was a C local whose address is taken, initialised
in memory by such a prologue: it read back as whatever dead data its
stack slot held -- an FPU context image, the caller's function object --
and surfaced as random "not a function", "too few arguments", "keyword
key is not a symbol" and wrong values, a different one per launch.  exec's
context switch, the FPU frames and `StackSwap()` were each checked and
are not involved.

## What amilisp does about it

The m68k build replays the chain a few thousand times at startup
(`src/platform/cpu_store_probe_m68k.s`, milliseconds) and, when a store
is lost, prints a warning naming the defect and puts `:cpu-lost-stores`
on `*features*`; `(ext:%cpu-store-selftest &optional rounds)` repeats
the test, `AMILISP_CPU_CHECK=0` skips it and `AMILISP_CPU_CHECK=<n>`
simulates `n` lost stores on a sound machine.  The README's "CPU store
self-test" section is the user-facing description; `tests/test_cpu_selftest.sh`
and the `cpu-store-selftest` block of `tests/amiga/run-tests.lisp` pin it.
The test uses ONE absolute source on purpose: on an affected core it
therefore flags about every second launch, not every launch.  A sweep
over sixteen source addresses would flag every launch, but switching the
source between replays made the mis-executed store land outside the
buffer, in hardware registers, and froze the machine twice (display "out
of range", reboot needed).  The target buffer is static, never the stack:
a late store into a reused stack frame overwrote a return address once
(guru 80000006).  The Clamacs drive asks both ports for `:cpu-lost-stores`
before it drives anything and voids a flagged launch.

## Machines checked

| machine                                  | CPU / core                 | lost stores        | date       |
|------------------------------------------|----------------------------|--------------------|------------|
| Apollo V4 Standalone, KS 47.115          | AC68080 core 11350, 92 MHz | none               | 2026-10-21 |
| Apollo V4 Standalone, KS 47.13           | AC68080 core 10760, 92 MHz | on about half the launches | 2026-09-19 |
| FS-UAE (68040 JIT)                       | emulated 68040             | none               | 2026-09-19 |
| Amiga 4000, KS 47.102                    | 68060 Rev1 + 68882         | none               | 2026-09-20 |
| Amiga 1200 (Warp), KS 47.115             | 68040 + 68882              | none               | 2026-09-20 |

## The probes

The standalone programs that pinned the defect down -- `storeprobe`
(twenty-one shapes of the chain), `apollo-repro.c` (a ninety-line
one-file reproduction for the Apollo team), and `regtest`, `fpuregtest`
and `stackprobe`, which cleared exec's context switch, the FPU frames and
the stack swap -- together with their results file `PROBES.md` and the
`make -f Makefile.cross probes` target, were removed from
`verify/realamiga/` on 2026-10-22 once core 11350 had been verified clean.
They are in the history: `git show 62aa2d38:verify/realamiga/PROBES.md`
lists them and how to run them on a machine not in the table above.  To
report the defect to the Apollo team, give the board (`ApolloControl
BFN`), the core (`ApolloControl CORE`), the instruction sequence and the
address dependence above, and `apollo-repro.c` from that commit.
