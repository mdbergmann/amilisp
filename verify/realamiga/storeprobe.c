/*
 * storeprobe -- do consecutive longword stores through a post-incremented
 * pointer all reach memory on THIS machine, at every alignment?
 *
 * The 2026-09-18/19 Clamacs finding on the Vampire: a C local of
 * bi_mismatch (amilisp, builtins_sequence2.c) that the function's
 * prologue initialises with
 *
 *     lea     -24(a5),a0
 *     move.l  (a4),(a0)+        ; seq1
 *     move.l  4(a4),(a0)+       ; seq2
 *     move.l  _SYM_EQL_FN,(a0)+ ; test_fn
 *     clr.l   (a0)+             ; test_not_fn
 *     clr.l   (a0)              ; key_fn
 *
 * later read back the DEAD data that was in the slot before (an FPU
 * context image one day, the caller's function object the next) -- as
 * if the initialising store never reached memory.  Whether it shows
 * depends on the layout, i.e. on the frame's alignment.  This program
 * replays that exact sequence into a pattern-filled buffer at every
 * offset mod 16, on the stack and in a heap block, millions of times,
 * with Delay() task switches mixed in, and reports every store that did
 * not land (offset, slot, value seen).
 *
 *   storeprobe [SECONDS] [swap|noswap] [vN]   (N = variant, see replay())
 *
 * Build: m68k-amigaos-gcc -noixemul -mcpu=68020 -O1 -o storeprobe storeprobe.c
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <exec/execbase.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

extern struct ExecBase *SysBase;

static ULONG src[2] = { 0x5EC10001, 0x5EC20002 };
static ULONG eqlfn = 0x0E410003;
static ULONG lost, rounds_done;
static ULONG cur_a0;
static char report[1024];
static int report_len;

#define MAX_VARIANT 21
static int variant = 1;

/* Variant 21 -- FREEZES A VAMPIRE (core 10760, twice on 2026-09-19, the
 * display "out of range", reboot needed): the v5 chain with SIXTEEN
 * absolute sources at consecutive longword addresses, the next every 16
 * rounds.  The finding behind it: whether the store is lost depends on
 * the ADDRESS OF THE ABSOLUTE SOURCE (mod 64) -- 8 of the 16 longword
 * offsets lose every time, the other 8 never; the code address and the
 * destination make no difference.  A program's data hunk lands somewhere
 * else on every launch, so a single source (v1..v20) passes or fails per
 * LAUNCH, and this sweep would fail on every launch -- but switching the
 * source between replays makes the mis-executed store land outside the
 * buffer, in hardware.  Kept as the record for the Apollo team; run it
 * only on a machine you can reboot.  amilisp's startup self-test is the
 * single-source chain for that reason. */
static ULONG eqs[16];
static ULONG eqs_lost[16];
static int cur_k;

/* One replay into DST (5 longs), each variant ONE inline-asm block so the
 * chain reaches the CPU back to back.  E = the absolute-address source
 * (eqlfn, a static), Z = the value zero.  exp[] holds what the 5 words
 * must read back as; words a variant does not write are preset.
 *   1  move.l (a4),(a0)+ ; move.l 4(a4),(a0)+ ; move.l E,(a0)+ ; clr.l (a0)+ ; clr.l (a0)  (bi_mismatch)
 *   2  same, nop before the last clr             3  same, last two = move.l d0(=0),(a0)+ ; move.l d0,(a0)
 *   4  same, last two = clr.l (a0)+ ; clr.l (a0)+ 5  move.l E,(a0)+ ; clr.l (a0)+ ; clr.l (a0)
 *   6  two indirect moves ; addq #4,a0 (skips slot 2) ; the two clrs (no E)   7  three clrs alone
 *   8  move.l E,d3 ; ... ; move.l d3,(a0)+ ; clr.l (a0)+ ; clr.l (a0)  (register source)
 *   9  move.l E,(a0) ; clr.l 4(a0) ; clr.l 8(a0)        -- displacement forms (-fno-auto-inc-dec output)
 *  10  move.l E,(a0)+ ; clr.l (a0)+ ; nop ; nop ; clr.l (a0)
 *  11  move.l E,(a0)+ ; clr.l (a0)+ ; move.l d5,d6 ; clr.l (a0)
 *  12  move.l E,(a0)+ ; nop ; clr.l (a0)+ ; clr.l (a0)
 *  13  move.l E,(a0)+ ; clr.l (a0)+ ; clr.l (a0)+ ; clr.l (a0)   (three clrs after E; d[1..4])
 *  14  move.l E,(a0)+ ; clr.l (a0)                                (one clr after E)
 *  15  move.l E,(a0)+ ; move.l d0(=0),(a0)+ ; clr.l (a0)
 *  16  move.l E,(a0)+ ; clr.l (a0)+ ; move.l d0(=0),(a0)
 *  17  move.l E,-(a0) ... predecrement chain: move.l E,-(a0) ; clr.l -(a0) ; clr.l -(a0)  (a0 = dst+5, writes d[4],d[3],d[2])
 *  18  move.l E,(a0)+ ; clr.l (a0)+ ; clr.l (a0)  with a1 as the pointer instead of a0
 *  19  move.l E,(a0)+ ; clr.w (a0)+ ; clr.w (a0)+ ; clr.l (a0)   (word clears)
 *  20  move.l E,4(a0) ; clr.l 8(a0) ; clr.l 12(a0)  (displacement, E first at +4)
 *  21  the v5 chain with 16 absolute sources at consecutive addresses, swept
 *      one per 16 rounds -- covers every source alignment mod 64, see eqs[] */
static int replay(ULONG *dst)
{
    volatile ULONG *d = dst;
    ULONG exp[5] = { src[0], src[1], eqlfn, 0, 0 };
    int k;
    /* preset everything the variant does not write */
    for (k = 0; k < 5; k++) dst[k] = exp[k];
    /* and re-dirty the slots it DOES write, so a lost store shows */
    switch (variant) {
    case 1: case 2: case 3: case 4: case 8:
        dst[0] = dst[1] = dst[2] = dst[3] = dst[4] = 0xDEAD0000u | variant; break;
    case 5: case 10: case 11: case 12: case 15: case 16: case 18: case 19:
        dst[2] = dst[3] = dst[4] = 0xDEAD0000u | variant; break;
    case 6:  /* writes d[0], d[1], d[3], d[4]; the addq skips d[2], which keeps its preset */
        dst[0] = dst[1] = dst[3] = dst[4] = 0xDEAD0000u | variant; break;
    case 7:  dst[2] = dst[3] = dst[4] = 0xDEAD0000u | variant; exp[2] = 0; break;
    case 9:  dst[2] = dst[3] = dst[4] = 0xDEAD0000u | variant; break;
    case 13: dst[1] = dst[2] = dst[3] = dst[4] = 0xDEAD0000u | variant; exp[1] = eqlfn; exp[2] = 0; break;
    case 14: dst[2] = dst[3] = 0xDEAD0000u | variant; break;
    case 17: dst[2] = dst[3] = dst[4] = 0xDEAD0000u | variant; exp[4] = eqlfn; exp[3] = 0; exp[2] = 0; break;
    case 20: dst[1] = dst[2] = dst[3] = 0xDEAD0000u | variant; exp[1] = eqlfn; exp[2] = 0; exp[3] = 0; exp[4] = src[1]; dst[4] = src[1]; break;
    case 21: dst[2] = dst[3] = dst[4] = 0xDEAD0000u | variant; exp[2] = eqs[cur_k]; break;
    }
#define A(...) __asm__ volatile (__VA_ARGS__)
#define S(K) A("movea.l %0,%%a0\n move.l %1,(%%a0)+\n clr.l (%%a0)+\n clr.l (%%a0)\n" : : "r"(dst+2), "m"(eqs[K]) : "a0","memory")
    if (variant == 21) {
        switch (cur_k) {
        case 0: S(0); break;   case 1: S(1); break;   case 2: S(2); break;   case 3: S(3); break;
        case 4: S(4); break;   case 5: S(5); break;   case 6: S(6); break;   case 7: S(7); break;
        case 8: S(8); break;   case 9: S(9); break;   case 10: S(10); break; case 11: S(11); break;
        case 12: S(12); break; case 13: S(13); break; case 14: S(14); break; default: S(15); break;
        }
    }
#undef S
    switch (variant) {
    case 1: A("movea.l %0,%%a0\n movea.l %1,%%a4\n move.l (%%a4),(%%a0)+\n move.l 4(%%a4),(%%a0)+\n move.l %2,(%%a0)+\n clr.l (%%a0)+\n clr.l (%%a0)\n" : : "r"(dst), "r"(src), "m"(eqlfn) : "a0","a4","memory"); break;
    case 2: A("movea.l %0,%%a0\n movea.l %1,%%a4\n move.l (%%a4),(%%a0)+\n move.l 4(%%a4),(%%a0)+\n move.l %2,(%%a0)+\n clr.l (%%a0)+\n nop\n clr.l (%%a0)\n" : : "r"(dst), "r"(src), "m"(eqlfn) : "a0","a4","memory"); break;
    case 3: A("movea.l %0,%%a0\n movea.l %1,%%a4\n move.l (%%a4),(%%a0)+\n move.l 4(%%a4),(%%a0)+\n move.l %2,(%%a0)+\n moveq #0,%%d0\n move.l %%d0,(%%a0)+\n move.l %%d0,(%%a0)\n" : : "r"(dst), "r"(src), "m"(eqlfn) : "a0","a4","d0","memory"); break;
    case 4: A("movea.l %0,%%a0\n movea.l %1,%%a4\n move.l (%%a4),(%%a0)+\n move.l 4(%%a4),(%%a0)+\n move.l %2,(%%a0)+\n clr.l (%%a0)+\n clr.l (%%a0)+\n" : : "r"(dst), "r"(src), "m"(eqlfn) : "a0","a4","memory"); break;
    case 5: A("movea.l %0,%%a0\n move.l %1,(%%a0)+\n clr.l (%%a0)+\n clr.l (%%a0)\n" : : "r"(dst+2), "m"(eqlfn) : "a0","memory"); break;
    case 6: A("movea.l %0,%%a0\n movea.l %1,%%a4\n move.l (%%a4),(%%a0)+\n move.l 4(%%a4),(%%a0)+\n addq.l #4,%%a0\n clr.l (%%a0)+\n clr.l (%%a0)\n" : : "r"(dst), "r"(src) : "a0","a4","memory"); break;
    case 7: A("movea.l %0,%%a0\n clr.l (%%a0)+\n clr.l (%%a0)+\n clr.l (%%a0)\n" : : "r"(dst+2) : "a0","memory"); break;
    case 8: A("movea.l %0,%%a0\n movea.l %1,%%a4\n move.l %2,%%d3\n move.l (%%a4),(%%a0)+\n move.l 4(%%a4),(%%a0)+\n move.l %%d3,(%%a0)+\n clr.l (%%a0)+\n clr.l (%%a0)\n" : : "r"(dst), "r"(src), "m"(eqlfn) : "a0","a4","d3","memory"); break;
    case 9: A("movea.l %0,%%a0\n move.l %1,(%%a0)\n clr.l 4(%%a0)\n clr.l 8(%%a0)\n" : : "r"(dst+2), "m"(eqlfn) : "a0","memory"); break;
    case 10: A("movea.l %0,%%a0\n move.l %1,(%%a0)+\n clr.l (%%a0)+\n nop\n nop\n clr.l (%%a0)\n" : : "r"(dst+2), "m"(eqlfn) : "a0","memory"); break;
    case 11: A("movea.l %0,%%a0\n move.l %1,(%%a0)+\n clr.l (%%a0)+\n move.l %%d5,%%d6\n clr.l (%%a0)\n" : : "r"(dst+2), "m"(eqlfn) : "a0","d6","memory"); break;
    case 12: A("movea.l %0,%%a0\n move.l %1,(%%a0)+\n nop\n clr.l (%%a0)+\n clr.l (%%a0)\n" : : "r"(dst+2), "m"(eqlfn) : "a0","memory"); break;
    case 13: A("movea.l %0,%%a0\n move.l %1,(%%a0)+\n clr.l (%%a0)+\n clr.l (%%a0)+\n clr.l (%%a0)\n" : : "r"(dst+1), "m"(eqlfn) : "a0","memory"); break;
    case 14: A("movea.l %0,%%a0\n move.l %1,(%%a0)+\n clr.l (%%a0)\n" : : "r"(dst+2), "m"(eqlfn) : "a0","memory"); break;
    case 15: A("movea.l %0,%%a0\n moveq #0,%%d0\n move.l %1,(%%a0)+\n move.l %%d0,(%%a0)+\n clr.l (%%a0)\n" : : "r"(dst+2), "m"(eqlfn) : "a0","d0","memory"); break;
    case 16: A("movea.l %0,%%a0\n moveq #0,%%d0\n move.l %1,(%%a0)+\n clr.l (%%a0)+\n move.l %%d0,(%%a0)\n" : : "r"(dst+2), "m"(eqlfn) : "a0","d0","memory"); break;
    case 17: A("movea.l %0,%%a0\n move.l %1,-(%%a0)\n clr.l -(%%a0)\n clr.l -(%%a0)\n" : : "r"(dst+5), "m"(eqlfn) : "a0","memory"); break;
    case 18: A("movea.l %0,%%a1\n move.l %1,(%%a1)+\n clr.l (%%a1)+\n clr.l (%%a1)\n" : : "r"(dst+2), "m"(eqlfn) : "a1","memory"); break;
    case 19: A("movea.l %0,%%a0\n move.l %1,(%%a0)+\n clr.w (%%a0)+\n clr.w (%%a0)+\n clr.l (%%a0)\n" : : "r"(dst+2), "m"(eqlfn) : "a0","memory"); break;
    case 20: A("movea.l %0,%%a0\n move.l %1,4(%%a0)\n clr.l 8(%%a0)\n clr.l 12(%%a0)\n" : : "r"(dst), "m"(eqlfn) : "a0","memory"); break;
    }
#undef A
    __asm__ volatile ("" ::: "memory");
    for (k = 0; k < 5; k++) if (d[k] != exp[k]) return 1;
    return 0;
}

static void fill(ULONG *buf, int n) { int k; for (k = 0; k < n; k++) buf[k] = 0xDEAD0000u + k; }

static void note_lost(const char *where, int off, ULONG *d)
{
    lost++;
    if (report_len < (int)sizeof(report) - 160)
        report_len += sprintf(report + report_len,
            "  LOST %s off=%d: %08lx %08lx %08lx %08lx %08lx\n", where, off,
            (unsigned long)d[0], (unsigned long)d[1], (unsigned long)d[2],
            (unsigned long)d[3], (unsigned long)d[4]);
}

static void run(ULONG rounds, ULONG *heap)
{
    ULONG stackbuf[64];      /* the on-stack target, all alignments via off */
    ULONG r;
    for (r = 0; r < rounds; r++) {
        int off = (int)(r & 15);           /* 0..15 longs => every mod-16 case */
        ULONG *ds = stackbuf + 8 + off;
        ULONG *dh = heap + 8 + off;
        cur_k = (int)((r >> 4) & 15);      /* v21: the source, every 16 rounds the next */
        fill(stackbuf, 64);
        if (replay(ds)) { note_lost("stack", off, ds); eqs_lost[cur_k]++; }
        fill(heap, 64);
        if (replay(dh)) { note_lost("heap", off, dh); eqs_lost[cur_k]++; }
        if ((r & 0x1fff) == 0) Delay(1);   /* a task switch for sure */
        rounds_done = r;
    }
}

static ULONG rounds;
static ULONG *heapbuf;

/* The swap runs the body on a stack of its own.  Everything touched after
 * StackSwap() must be reachable without the caller's stack: a compiler
 * that addresses locals through sp (GCC 16 at -O1 omits the frame
 * pointer and inlines body()) would otherwise read them from the NEW
 * stack and hand the second StackSwap() garbage -- so these live in
 * statics and body() is never inlined. */
static struct StackSwapStruct ss;
static void *mem_p;

static __attribute__((noinline)) int body(void)
{
    struct Task *t = FindTask(NULL);
    printf("stack now lower=%p upper=%p (%lu bytes)\n", t->tc_SPLower, t->tc_SPUpper,
           (unsigned long)((char *)t->tc_SPUpper - (char *)t->tc_SPLower));
    fflush(stdout);
    run(rounds, heapbuf);
    return 0;
}

int main(int argc, char **argv)
{
    int secs = 10, swap = 1, i;
    for (i = 1; i < argc; i++) {
        if (atoi(argv[i]) > 0) secs = atoi(argv[i]);
        else if (!strcmp(argv[i], "noswap")) swap = 0;
        else if (!strcmp(argv[i], "swap")) swap = 1;
        else if (argv[i][0] == 'v') variant = atoi(argv[i] + 1);
    }
    /* No case in replay() matches an unknown variant: nothing would run and
     * the untouched buffer would compare equal, a false "all stores landed".
     * Exit 10, not 20 -- 20 is reserved for the "STORES LOST" verdict. */
    if (variant < 1 || variant > MAX_VARIANT) {
        printf("storeprobe: variant %d does not exist\n"
               "usage: storeprobe [SECONDS] [swap|noswap] [vN]   (N = 1..%d, see replay())\n",
               variant, MAX_VARIANT);
        return 10;
    }
    printf("storeprobe: %d s, %s, variant %d  cpu flags 0x%04x\n", secs, swap ? "swap" : "noswap",
           variant, (unsigned)SysBase->AttnFlags);
    if (variant == 21)
        printf("storeprobe: WARNING -- variant 21 froze a Vampire V4 (core 10760) within seconds;\n"
               "            be ready to reboot (verify/realamiga/PROBES.md)\n");
    fflush(stdout);
    for (i = 0; i < 16; i++) eqs[i] = 0x0E410000u + (ULONG)i;
    rounds = (ULONG)secs * 300000UL;
    heapbuf = AllocVec(64 * sizeof(ULONG) + 16, MEMF_ANY);
    if (!heapbuf) { printf("no memory\n"); return 10; }

    if (swap) {
        mem_p = AllocVec(128 * 1024, MEMF_ANY);
        if (!mem_p) { printf("no memory\n"); return 10; }
        ss.stk_Lower = mem_p;
        ss.stk_Upper = (ULONG)mem_p + 128 * 1024;
        ss.stk_Pointer = (APTR)ss.stk_Upper;
        StackSwap(&ss);
        body();
        StackSwap(&ss);
        FreeVec(mem_p);
    } else {
        body();
    }
    FreeVec(heapbuf);
    printf("%s", report);
    if (variant == 21) {
        int k;
        printf("  source address mod 64 -> lost (of %lu replays each):\n", (unsigned long)(rounds_done + 1) / 8);
        for (k = 0; k < 16; k++)
            printf("  %2lu:%-7lu%s", (unsigned long)((ULONG)&eqs[k] & 63), (unsigned long)eqs_lost[k],
                   (k & 3) == 3 ? "\n" : "");
    }
    printf("storeprobe: %lu rounds, %lu lost store(s) -- %s\n", (unsigned long)rounds_done + 1,
           (unsigned long)lost, lost ? "STORES LOST" : "all stores landed");
    return lost ? 20 : 0;
}
