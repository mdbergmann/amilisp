# CPU / system probes for real Amigas

Small standalone m68k programs that test one machine behaviour each, built
for the hunt of a corruption that turned out to be a hardware defect of the
Apollo 68080 core (Vampire V4, core 10760, 2026-09-19).  Run them on any
Amiga you can reach; each prints a one-line verdict and exits with 0 (fine)
or 20 (defect seen).

Build all four on the host:

    make -f Makefile.cross probes        # -> build/cross/probes/{storeprobe,fpuregtest,stackprobe,regtest}

Copy the binaries to the Amiga any way you like (amiagent, FTP, SD card) and
run them from a Shell.  With an amiagent on the box and the amimcp client
on the host, `verify/realamiga/run-probes.py` does the upload and the runs:

    AMIGA_HOST=192.168.1.50 AMIGA_TOKEN=xyz verify/realamiga/run-probes.py

## storeprobe -- the one that matters

    storeprobe [SECONDS] [swap|noswap] [vN]

Replays, as one back-to-back instruction chain, the prologue gcc emits for a
C function that initialises five locals through a post-incremented pointer:

    move.l (a4),(a0)+ ; move.l 4(a4),(a0)+ ; move.l <absolute>,(a0)+ ; clr.l (a0)+ ; clr.l (a0)

into a pattern-filled buffer at every alignment mod 16, on the stack and in a
heap block, hundreds of thousands of times, with task switches mixed in, and
reads the five words back.  `vN` selects one of 21 shapes of the store chain
(the table is in the source); `v1` is the default.  `v21` freezes a
Vampire -- see below before running it.

Healthy CPU (68000..68060, UAE):     `storeprobe: N rounds, 0 lost store(s) -- all stores landed`
Vampire V4 core 10760 (68080, 92 MHz): `storeprobe: N rounds, ~2N lost store(s) -- STORES LOST` (on a launch whose source address loses; run it a few times)

On the Vampire the LAST store of that chain does not reach memory (or lands
late) in ~100% of replays.  The trigger, from the 20 variants: a
memory-to-memory MOVE whose source is an absolute address, followed by two
or more further stores nearby.  Loading the absolute source into a register
first, or indirect sources, are fine; a nop or an ALU op between the stores,
the displacement form (`clr.l 4(a0)`), a different address register, word
clears, all still lose it; superscalar off (`ApolloControl SS=0`) changes
nothing.  In amilisp this was bi_mismatch's `key_fn` local staying whatever
dead data the slot held before (an FPU context image, the caller's function
object), and every "Too few arguments to PREFIX-P" / "not a function" /
"keyword key is not a symbol" of the Clamacs editor suite on that machine.

**What decides it is the ADDRESS OF THE ABSOLUTE SOURCE** (found later the
same day with a scratch probe that ran the chain from 32 code offsets and
from 32 sources at consecutive addresses): the code address makes no
difference (all 32 copies lose), the destination makes no difference, but
of 16 sources at consecutive longword addresses exactly 8 lose every replay
and 8 never do -- with the source's address mod 64 deciding (on core 10760:
24, 32, 40 and 44..60 lose; 0..20, 28, 36 are fine).  That is why the "fine"
shapes flipped between builds and why one and the same amilisp binary
reported "4095 of 4096 lost" on one launch and 0 on the next: a program's
data hunk lands at a different address on every launch.  Variants 1..20
pass or fail per launch -- run them more than once.

**`v21` FREEZES THE MACHINE -- do not run it on a Vampire you cannot
reboot.**  It sweeps 16 absolute sources at consecutive addresses, one per
16 rounds, which would flag every launch; on core 10760 both it and an
equivalent C loop froze the box within seconds with the display "out of
range" (2026-09-19, twice, reboot needed each time): switching the source
address between replays makes the mis-executed store land outside the
target buffer, in hardware registers.  The placement probe that found the
address dependence switched sources only every 2000 rounds and survived
six runs.  It stays in the source as the record for the Apollo team.
amilisp's startup self-test (`src/platform/cpu_store_probe_m68k.s`) is
therefore the single-source chain and flags about every second launch on
that core.

`CPU NODATACACHE` changes nothing (2026-09-19: v1, v5 and v9 lose at
exactly the same rate with `DATA: NoCache NoBurst`), nor does
`ApolloControl SS=0`: the write path itself, not a cache or dual issue.

What to run elsewhere, and why:

- a real 68030/040/060: expected clean -- shows the defect is the 68080's.
- another Vampire (V2, V4, a different core revision): `v1` a few times
  (per launch!), then `v5`, `v9`, `v20`, `v14`, `v8` -- tells whether a
  newer core fixes it.  `v21` only if you can reboot the machine.
- Report to the Apollo team with: board (`ApolloControl BFN`), core
  (`ApolloControl CORE`), `ApolloControl CPU`, the verdict lines, the
  address dependence and the v21 freeze above, and this file's source.

## apollo-repro.c -- the one-file reproduction for the Apollo team

    m68k-amigaos-gcc -noixemul -mcpu=68020 -O1 -o apollo-repro verify/realamiga/apollo-repro.c
    apollo-repro            # sixteen source distances, 20000 replays each, a table; exit 20 if any lost
    apollo-repro 64         # one distance, 100000 replays

Ninety lines, no dependencies beyond libnix: `move.l <abs>,(a0)+ ; clr.l
(a0)+ ; clr.l (a0)` from a static source 4..64 bytes below a static
target, one distance at a time, and the third word read back.  On core
10760 (data loaded at 0x1F67514) the source 64 bytes below the target
loses 19999 of 20000 replays on every launch and the other fifteen lose
none; on a 68040, 68060 or UAE no row loses.  Which row loses depends on
the load address as well (the same layout inside amilisp, whose data
lands elsewhere on each launch, flagged only one launch in four), so the
table, not a single row, is the signature.  Switching the source only
every 20000 replays is the regime the placement probe survived; do not
shorten the blocks (see v21).

## Procedure for a machine not in the table below

Ten minutes, nothing to install on the Amiga beyond the four binaries.
Never run `v21`, and do not run amilisp itself for this -- the probes are
enough and cannot take the machine down.

1. `make -f Makefile.cross probes` on the host (needs the m68k toolchain
   under `tools/m68k-amigaos-gcc/prefix`, as `make -f Makefile.cross amiga`
   does).
2. Get the binaries onto the box: `run-probes.py` with `AMIGA_HOST` /
   `AMIGA_TOKEN` when an amiagent runs there (`AMIGA_DIR` for a drawer other
   than `T:`), otherwise copy `build/cross/probes/*` by hand, `Protect +e`
   them, and run the lines below from a Shell.
3. Record what the machine is, from a Shell: `Version` (Kickstart, exec),
   `CPU` (the 3.x command: CPU type, caches), and on a Vampire
   `ApolloControl CORE`, `ApolloControl BFN`, `ApolloControl CPU`.  On a
   MorphOS or emulated machine say so: the probes then measure the 68k
   emulation, a control for "healthy", not a core.
4. Run, and keep every verdict line:

       storeprobe 3 noswap v1      (FIVE times -- separate launches, since the
                                    verdict is per launch on a defective core)
       storeprobe 3 noswap v5
       storeprobe 3 noswap v9
       storeprobe 3 noswap v20
       storeprobe 3 noswap v14
       storeprobe 3 noswap v8
       storeprobe 3 swap v1
       fpuregtest 5 noswap fpu
       fpuregtest 5 noswap sin
       regtest 5 noswap
       stackprobe 5 noswap fpu

   `run-probes.py` with no arguments runs this set except the `swap` line.
   Run that one separately: `run-probes.py "storeprobe 3 swap v1"` --
   arguments replace the default set, they do not extend it.
   A healthy machine prints `0 lost store(s) -- all stores landed` on
   every storeprobe line, `registers intact` from fpuregtest and regtest
   and `0 foreign write(s) -- live stack intact` from stackprobe; exit
   code 0 throughout.  Any `STORES LOST` on any launch means
   the defect is present; five clean `v1` launches in a row mean it is
   absent with high confidence (on core 10760 the per-launch odds are
   about even).
5. Add a row to the table below and commit it.  For a Vampire with a
   different core than 10760, also note whether v1 EVER lost a store: that
   single bit is the question for the Apollo team ("does a newer core fix
   it?").

## Results so far

| machine                                   | CPU / core                      | v1 (5 launches)      | v5 / v9 / v20 / v14 / v8         | fpu / reg / stack | date       |
|-------------------------------------------|---------------------------------|----------------------|----------------------------------|-------------------|------------|
| Apollo V4 Standalone, KS 47.13, WB 3.2.3  | AC68080 core 10760, 92 MHz      | LOST on ~half        | LOST / LOST 74% / ok / ok / flips | clean             | 2026-09-19 |
| FS-UAE (verify.fs-uae, 68040 JIT)         | emulated 68040                  | clean                | clean                            | clean             | 2026-09-19 |
| Amiga 4000, KS 47.102, WB 47.5            | 68060 Rev1 + 68882, CopyBack    | clean                | clean                            | clean             | 2026-09-20 |
| Amiga 1200 (Warp), KS 47.115, WB 47.5     | 68040 + 68882, CopyBack         | clean                | clean                            | clean             | 2026-09-20 |

("flips" = passed in one build and lost 68% in the next: the source-address
dependence above, before it was understood.)

## fpuregtest, regtest, stackprobe -- the ones that exonerated the rest

    fpuregtest [SECONDS] [swap|noswap] [fpu] [sin] [cr]
    regtest    [SECONDS] [swap|noswap] [chip|reverse|clear]
    stackprobe [SECONDS] [swap|noswap] [fpu] [io] [chip|reverse|clear]

They keep constants in the callee-saved registers d2-d7/a2-a4 (regtest,
fpuregtest -- the latter with FPU state alive, including trapped `fsin` /
`fmovecr`) or a 16K sentinel array in a live stack frame (stackprobe) across
thousands of task switches (Delay + timer interrupts), on the Shell's stack
or on a 128K block reached through exec StackSwap, and report the first
change.  All three are clean on the Vampire: exec's context save/restore,
the FPU frames and the stack swap are not involved in the defect above.  On
a new machine they are a 30-second sanity check of the same paths.
