/*
 * fpuregtest -- do the callee-saved integer registers survive task
 * switches on THIS machine while the task also has FPU state?
 *
 * regtest.c (2026-09-19) showed d2-d7/a2-a4 intact across thousands of
 * task switches -- for a task that never touched the FPU, whose context
 * save is therefore the integer registers alone.  amilisp's task DOES
 * carry FPU state (the FPU register images found in its stack dumps), and
 * exec's context restore reads the integer registers back from an offset
 * that depends on the FPU frame's size.  This program keeps constants in
 * d2-d7/a2-a4 across a loop that (a) uses the FPU every round, so every
 * switch saves and restores an FPU frame, (b) optionally executes an
 * instruction the CPU may have to trap and emulate (fsin, fmovecr), and
 * (c) yields with Delay() and takes timer interrupts.  It reports the
 * first register that changes, with its value.
 *
 *   fpuregtest [SECONDS] [swap|noswap] [fpu] [sin] [cr]
 *
 * Build: m68k-amigaos-gcc -noixemul -mcpu=68020 -O1 -o fpuregtest fpuregtest.c
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

static ULONG use_fpu, use_sin, use_cr;
static ULONG bad_reg, bad_val, bad_val2, bad_round, rounds_done;
static ULONG fpu_sink[3];   /* fmove.x stores 12 bytes: all of it is the sink */

static void loop(ULONG rounds)
{
    __asm__ volatile (
        "    .chip 68040\n"
        "    move.l  %[rounds],%%d1\n"
        "    move.l  #0x11111111,%%d2\n"
        "    move.l  #0x22222222,%%d3\n"
        "    move.l  #0x33333333,%%d4\n"
        "    move.l  #0x44444444,%%d5\n"
        "    move.l  #0x55555555,%%d6\n"
        "    move.l  #0x66666666,%%d7\n"
        "    move.l  #0xa2a2a2a2,%%a2\n"
        "    move.l  #0xa3a3a3a3,%%a3\n"
        "    move.l  #0xa4a4a4a4,%%a4\n"
        "1:\n"
        /* FPU work each round (idle frame at a switch) */
        "    tst.l   %[use_fpu]\n"
        "    beq     3f\n"
        "    fmove.l %%d1,%%fp0\n"
        "    fadd.x  %%fp0,%%fp1\n"
        "    fmul.x  %%fp1,%%fp2\n"
        "    fmove.x %%fp2,%%fp3\n"
        "    tst.l   %[use_sin]\n"
        "    beq     4f\n"
        "    fsin.x  %%fp0,%%fp4\n"        /* trapped/emulated on 040/060/080 */
        "4:\n"
        "    tst.l   %[use_cr]\n"
        "    beq     3f\n"
        "    fmovecr #0,%%fp5\n"            /* pi: unimplemented on 040+ */
        "3:\n"
        "    cmp.l   #0x11111111,%%d2\n"
        "    bne     8f\n"
        "    cmp.l   #0x22222222,%%d3\n"
        "    bne     8f\n"
        "    cmp.l   #0x33333333,%%d4\n"
        "    bne     8f\n"
        "    cmp.l   #0x44444444,%%d5\n"
        "    bne     8f\n"
        "    cmp.l   #0x55555555,%%d6\n"
        "    bne     8f\n"
        "    cmp.l   #0x66666666,%%d7\n"
        "    bne     8f\n"
        "    move.l  %%a2,%%d0\n"
        "    cmp.l   #0xa2a2a2a2,%%d0\n"
        "    bne     8f\n"
        "    move.l  %%a3,%%d0\n"
        "    cmp.l   #0xa3a3a3a3,%%d0\n"
        "    bne     8f\n"
        "    move.l  %%a4,%%d0\n"
        "    cmp.l   #0xa4a4a4a4,%%d0\n"
        "    bne     8f\n"
        "    move.l  %%d1,%%d0\n"
        "    and.l   #0x3fff,%%d0\n"
        "    bne     2f\n"
        /* yield: Delay(1) -- d1 saved on the stack, fp regs are ours */
        "    move.l  %%d1,-(%%sp)\n"
        "    move.l  %%a6,-(%%sp)\n"
        "    move.l  %[dosbase],%%a6\n"
        "    moveq   #1,%%d1\n"
        "    jsr     -198(%%a6)\n"
        "    move.l  (%%sp)+,%%a6\n"
        "    move.l  (%%sp)+,%%d1\n"
        "2:\n"
        "    subq.l  #1,%%d1\n"
        "    bne     1b\n"
        "    bra     9f\n"
        "8:\n"
        "    move.l  %%d1,%[bad_round]\n"
        "    move.l  %%d2,%[bad_val]\n"
        "    move.l  %%d3,%[bad_val2]\n"
        "    move.l  #1,%[bad_reg]\n"
        "9:\n"
        "    fmove.x %%fp3,%[sink]\n"
        "    .chip 68020\n"
        : [bad_reg] "=m" (bad_reg), [bad_val] "=m" (bad_val), [bad_val2] "=m" (bad_val2),
          [bad_round] "=m" (bad_round), [sink] "=m" (fpu_sink)
        : [rounds] "m" (rounds), [dosbase] "m" (DOSBase),
          [use_fpu] "m" (use_fpu), [use_sin] "m" (use_sin), [use_cr] "m" (use_cr)
        : "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "a0", "a1", "a2", "a3", "a4", "cc", "memory");
}

static ULONG rounds;

static int body(void)
{
    struct Task *t = FindTask(NULL);
    printf("stack now lower=%p upper=%p (%lu bytes)\n", t->tc_SPLower, t->tc_SPUpper,
           (unsigned long)((char *)t->tc_SPUpper - (char *)t->tc_SPLower));
    fflush(stdout);
    loop(rounds);
    return 0;
}

int main(int argc, char **argv)
{
    int secs = 10, swap = 1, i;
    struct Task *t = FindTask(NULL);
    for (i = 1; i < argc; i++) {
        if (atoi(argv[i]) > 0) secs = atoi(argv[i]);
        else if (!strcmp(argv[i], "noswap")) swap = 0;
        else if (!strcmp(argv[i], "swap")) swap = 1;
        else if (!strcmp(argv[i], "fpu")) use_fpu = 1;
        else if (!strcmp(argv[i], "sin")) use_fpu = use_sin = 1;
        else if (!strcmp(argv[i], "cr")) use_fpu = use_cr = 1;
    }
    printf("fpuregtest: %d s, %s%s%s%s  cpu flags 0x%04x\n", secs, swap ? "swap" : "noswap",
           use_fpu ? ", fpu" : "", use_sin ? ", fsin" : "", use_cr ? ", fmovecr" : "",
           (unsigned)SysBase->AttnFlags);
    fflush(stdout);
    /* ~1M rounds/s without the FPU on a fast 68k; fsin/fmovecr traps are slower */
    rounds = (ULONG)secs * (use_sin || use_cr ? 200000UL : 1000000UL);

    if (swap) {
        struct StackSwapStruct ss;
        void *mem_p = AllocVec(128 * 1024, MEMF_ANY);
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
    if (bad_reg)
        printf("REGISTER CHANGED with %lu rounds left: d2=0x%08lx d3=0x%08lx\n",
               (unsigned long)bad_round, (unsigned long)bad_val, (unsigned long)bad_val2);
    printf("fpuregtest: %s\n", bad_reg ? "REGISTERS CORRUPTED" : "registers intact");
    return bad_reg ? 20 : 0;
}
