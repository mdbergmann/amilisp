/*
 * stackprobe -- does anything write into the LIVE part of this process's
 * stack (above the stack pointer) while it runs, on the stack the Shell
 * gave us and on a block of our own that exec StackSwap put us on?
 *
 * The 2026-09-18/19 Clamacs finding on the Vampire (68080, exec 47.13):
 * C locals of live frames come back overwritten -- with words that look
 * like an FPU register save (0x3c000000 0x80000000 0x00000000) -- only
 * when amilisp runs on the 128K block platform_run_main swaps to; never
 * on a Shell stack of >= 128K; never under FS-UAE.  regtest.c showed the
 * callee-saved REGISTERS survive both ways.  This program watches the
 * stack MEMORY instead: a 16K sentinel array in a live frame, a deeper
 * frame spinning through task switches, and a scan of the sentinels
 * every few rounds.  Options add what amilisp does: FPU use, dos I/O.
 *
 *   stackprobe [SECONDS] [swap|noswap] [fpu] [io] [chip|reverse|clear]
 *
 * Reports every sentinel word that changed (offset, old, new, round) and
 * a final verdict; exit code 20 when anything was written.
 *
 * Build: m68k-amigaos-gcc -noixemul -mcpu=68020 -O1 -o stackprobe stackprobe.c
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
#include <stdarg.h>

extern struct ExecBase *SysBase;

#define SENTINEL 0xA5A5A5A5u
#define LIVE_WORDS 4096          /* 16K of live frame under watch */
#define PAD_WORDS  2048          /* 8K frame below it, where SP lives */

static ULONG rounds;
static int use_fpu, use_io;
static BPTR io_file;
static ULONG hits;
static char report[2048];
static int report_len;

static void note(const char *fmt, ...)
{
    va_list ap;
    if (report_len > (int)sizeof(report) - 120) return;
    va_start(ap, fmt);
    report_len += vsprintf(report + report_len, fmt, ap);
    va_end(ap);
}

static void touch_fpu(void)
{
    /* make this task an FPU user, so exec saves FPU context at every
     * task switch from now on */
    ULONG z = 0;
    __asm__ volatile (
        "    .chip 68040\n"
        "    fmove.l %0,%%fpcr\n"
        "    fmove.l %0,%%fp0\n"
        "    .chip 68020\n"
        : : "d" (z) : "memory");
}

static void scan(volatile ULONG *live, ULONG round)
{
    ULONG i;
    for (i = 0; i < LIVE_WORDS; i++) {
        if (live[i] != SENTINEL) {
            hits++;
            if (hits <= 24)
                note("  write at live[%lu] (%p): %08lx  round %lu\n",
                     (unsigned long)i, (void *)&live[i],
                     (unsigned long)live[i], (unsigned long)round);
            live[i] = SENTINEL;
        }
    }
}

/* The deeper frame: SP sits ~8K below the sentinels while this spins. */
static void spin(volatile ULONG *live)
{
    volatile ULONG pad[PAD_WORDS];
    ULONG r;
    char probe;
    pad[0] = 1; pad[PAD_WORDS - 1] = 1;
    printf("spin: live=%p..%p  sp~%p  rounds=%lu%s%s\n", (void *)live,
           (void *)&live[LIVE_WORDS], (void *)&probe, (unsigned long)rounds,
           use_fpu ? " fpu" : "", use_io ? " io" : "");
    fflush(stdout);
    if (use_fpu) touch_fpu();
    for (r = 0; r < rounds; r++) {
        volatile ULONG busy = 0;
        ULONG k;
        for (k = 0; k < 2000; k++) busy += k;
        if ((r & 0x3f) == 0) Delay(1);              /* a task switch for sure */
        if (use_fpu && (r & 0x0f) == 0) touch_fpu();
        if (use_io && (r & 0x0f) == 0) {
            char line[64];
            int n = sprintf(line, "round %lu\n", (unsigned long)r);
            if (io_file) Write(io_file, line, n);
            WaitForChar(Input(), 1);
        }
        if ((r & 0x07) == 0) scan(live, r);
    }
    scan(live, r);
    (void)pad;
}

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
    volatile ULONG live[LIVE_WORDS];
    struct Task *t = FindTask(NULL);
    ULONG i;
    printf("stack now lower=%p upper=%p (%lu bytes)\n", t->tc_SPLower, t->tc_SPUpper,
           (unsigned long)((char *)t->tc_SPUpper - (char *)t->tc_SPLower));
    fflush(stdout);
    for (i = 0; i < LIVE_WORDS; i++) live[i] = SENTINEL;
    spin(live);
    return 0;
}

int main(int argc, char **argv)
{
    int secs = 10, swap = 1, i;
    const char *mem = "";
    struct Task *t = FindTask(NULL);
    for (i = 1; i < argc; i++) {
        if (atoi(argv[i]) > 0) secs = atoi(argv[i]);
        else if (!strcmp(argv[i], "noswap")) swap = 0;
        else if (!strcmp(argv[i], "swap")) swap = 1;
        else if (!strcmp(argv[i], "fpu")) use_fpu = 1;
        else if (!strcmp(argv[i], "io")) use_io = 1;
        else mem = argv[i];
    }
    printf("stackprobe: %d s, %s%s%s, mem=%s\n", secs, swap ? "swap" : "noswap",
           use_fpu ? ", fpu" : "", use_io ? ", io" : "", mem);
    printf("task stack lower=%p upper=%p (%lu bytes)  cpu flags 0x%04x\n",
           t->tc_SPLower, t->tc_SPUpper,
           (unsigned long)((char *)t->tc_SPUpper - (char *)t->tc_SPLower),
           (unsigned)SysBase->AttnFlags);
    fflush(stdout);
    if (use_io) io_file = Open("T:stackprobe.io", MODE_NEWFILE);

    /* one round is ~2000 adds + a scan every 8: roughly 40K rounds/s */
    rounds = (ULONG)secs * 40000UL;
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
    if (io_file) Close(io_file);
    printf("%s", report);
    printf("stackprobe: %lu foreign write(s) -- %s\n", (unsigned long)hits,
           hits ? "LIVE STACK WRITTEN" : "live stack intact");
    return hits ? 20 : 0;
}
