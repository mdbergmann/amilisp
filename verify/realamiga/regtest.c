/*
 * regtest -- do the callee-saved registers survive task switches on THIS
 * machine, on the stack the Shell gave us and on a stack of our own
 * (exec StackSwap, as amilisp's platform_run_main does)?
 *
 * The 2026-09-18 Clamacs finding: C locals of a live frame come back
 * holding words that look like an FPU context save (0x3c000000,
 * 0x80000000, 0) after the process ran on a swapped stack -- never on a
 * Shell stack of >= 128K.  This program keeps constants in d2-d7/a2-a4
 * across a long loop that takes timer interrupts and Delay()s (task
 * switches) and reports the first register that changes.
 *
 *   regtest [SECONDS] [swap|noswap] [chip|reverse|clear]
 *
 * Build: m68k-amigaos-gcc -noixemul -mcpu=68020 -O1 -o regtest regtest.c
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

static volatile ULONG iterations;
static volatile ULONG failures;
static char report[512];

/* The loop, in asm so the registers are really the CPU's.  d2-d7 and
 * a2-a5 hold constants; every round checks them, and every 64K rounds
 * yields with Delay(1) (a task switch for sure).  d0/d1/a0/a1 scratch. */
static void reg_loop(ULONG rounds)
{
    ULONG bad_reg = 0, bad_val = 0, bad_round = 0;
    __asm__ volatile (
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
        "    and.l   #0xffff,%%d0\n"
        "    bne     2f\n"
        /* yield: Delay(1) through dos.library; d0/d1/a0/a1 are scratch
         * for the call, d1 saved on the stack */
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
        "    move.l  #1,%[bad_reg]\n"
        "9:\n"
        : [bad_reg] "+m" (bad_reg), [bad_val] "+m" (bad_val), [bad_round] "+m" (bad_round)
        : [rounds] "m" (rounds), [dosbase] "m" (DOSBase)
        : "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "a0", "a1", "a2", "a3", "a4", "cc", "memory");
    if (bad_reg) {
        failures++;
        sprintf(report, "REGISTER CHANGED at round %lu (d2 now 0x%08lx)\n",
                (unsigned long)bad_round, (unsigned long)bad_val);
    }
}

/* C-level check too: locals in callee-saved registers across Delay(). */
static void c_loop(ULONG rounds)
{
    volatile ULONG *fail = &failures;
    ULONG a = 0x12345678, b = 0x9abcdef0, c = 0x0f0f0f0f, d = 0xf0f0f0f0;
    ULONG i;
    for (i = 0; i < rounds; i++) {
        if ((i & 0x3fff) == 0) Delay(1);
        if (a != 0x12345678 || b != 0x9abcdef0 || c != 0x0f0f0f0f || d != 0xf0f0f0f0) {
            (*fail)++;
            sprintf(report, "C LOCAL CHANGED at %lu: %08lx %08lx %08lx %08lx\n",
                    (unsigned long)i, (unsigned long)a, (unsigned long)b,
                    (unsigned long)c, (unsigned long)d);
            return;
        }
        a ^= i; a ^= i;     /* keep them live */
    }
}

static ULONG rounds_asm, rounds_c;

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
    reg_loop(rounds_asm);
    c_loop(rounds_c);
    return 0;
}

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 10;
    int swap = argc > 2 ? strcmp(argv[2], "noswap") != 0 : 1;
    const char *mem = argc > 3 ? argv[3] : "";
    struct Task *t = FindTask(NULL);
    struct MemHeader *mh;

    printf("regtest: %d s, %s, mem=%s\n", secs, swap ? "swap" : "noswap", mem);
    printf("task stack lower=%p upper=%p\n", t->tc_SPLower, t->tc_SPUpper);
    printf("cpu flags 0x%04x\n", (unsigned)SysBase->AttnFlags);
    for (mh = (struct MemHeader *)SysBase->MemList.lh_Head; mh->mh_Node.ln_Succ;
         mh = (struct MemHeader *)mh->mh_Node.ln_Succ)
        printf("memory %p-%p attr 0x%04x %s\n", mh->mh_Lower, mh->mh_Upper,
               (unsigned)mh->mh_Attributes, mh->mh_Node.ln_Name ? mh->mh_Node.ln_Name : "");
    fflush(stdout);

    /* about 2M asm rounds/s and 200K C rounds/s on a fast 68k: scale by secs */
    rounds_asm = (ULONG)secs * 2000000UL;
    rounds_c = (ULONG)secs * 200000UL;
    report[0] = '\0';

    if (swap) {
        ULONG flags = MEMF_ANY;
        if (strstr(mem, "chip")) flags = MEMF_CHIP;
        if (strstr(mem, "reverse")) flags |= MEMF_REVERSE;
        if (strstr(mem, "clear")) flags |= MEMF_CLEAR;
        mem_p = AllocVec(128 * 1024, flags);
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
    printf("%s", report);
    printf("regtest: %lu failure(s) -- %s\n", (unsigned long)failures,
           failures ? "REGISTERS CORRUPTED" : "registers intact");
    return failures ? 20 : 0;
}
