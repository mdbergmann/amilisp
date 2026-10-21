/*
 * apollo-repro.c -- minimal reproduction of a lost store on the AC68080
 * (Apollo V4 Standalone, core 10760; clean on 68040, 68060 and UAE).
 *
 * The instruction sequence is what gcc emits for a C function prologue
 * that initialises locals through a post-incremented pointer:
 *
 *     move.l  <absolute>,(a0)+   ; memory-to-memory move, ABSOLUTE source
 *     clr.l   (a0)+
 *     clr.l   (a0)               ; <-- this store does not reach memory
 *
 * After it the third longword still holds the pattern that was there
 * before.  Whether it happens depends on the addresses involved: the
 * address of the absolute source relative to the target, and where the
 * data was loaded.  This program keeps sixteen consecutive longwords as
 * candidate sources, 4..64 bytes below the target, tries each of them
 * for 20000 replays in turn, and prints a table.  On the 68080 one or
 * more rows lose (in every launch seen so far, though which row varies
 * with the load address: with the data at 0x1F67500 it was the source
 * 64 bytes below the target, alone); on a 68040, 68060 or UAE no row
 * ever loses.  A register source (move.l d0,(a0)+) or an indirect
 * source (move.l (a1),(a0)+) never loses.  The data cache off (CPU
 * NODATACACHE) and superscalar off (ApolloControl SS=0) change nothing.
 *
 * Build:  m68k-amigaos-gcc -noixemul -mcpu=68020 -O1 -o apollo-repro apollo-repro.c
 * Run:    apollo-repro         -> all sixteen distances; exit 20 if any lost
 *         apollo-repro 64      -> one distance (bytes below the target), 100000 replays
 */
#include <stdio.h>
#include <stdlib.h>

/* One static block, so the distance between source and target is fixed by
 * the binary: src[16 - d/4] is d bytes below buf[0].  The asm below names
 * the block by its linker symbol, so the name is pinned here. */
static struct {
    unsigned long src[16];
    unsigned long buf[8];
} D __asm__("_apollo_D");

/* The source is written into the instruction text as the symbol plus a
 * constant offset, which the assembler encodes as absolute long (opcode
 * 20f9).  An "m" input operand would NOT do: gcc is free to satisfy it
 * with any memory operand, and at -O1 it inlines replay() into run(),
 * puts the block's address in a4 and emits move.l (d,a4),(a0)+ -- an
 * indirect source, the form the comment above says never loses.
 * Check with: m68k-amigaos-objdump -d apollo-repro | grep 20f9  */
#define CHAIN(K) __asm__ volatile (                     \
        "movea.l %0,%%a0\n\t"                           \
        "move.l  _apollo_D+4*" #K ",(%%a0)+\n\t"        \
        "clr.l   (%%a0)+\n\t"                           \
        "clr.l   (%%a0)\n"                              \
        : : "r"(D.buf) : "a0", "memory")

/* K must be a literal: it is pasted into the instruction (a variable would
 * be an undefined assembler symbol, i.e. a build error, not a wrong test). */
static void replay(int k)
{
    switch (k) {
    case 0:  CHAIN(0);  break; case 1:  CHAIN(1);  break; case 2:  CHAIN(2);  break; case 3:  CHAIN(3);  break;
    case 4:  CHAIN(4);  break; case 5:  CHAIN(5);  break; case 6:  CHAIN(6);  break; case 7:  CHAIN(7);  break;
    case 8:  CHAIN(8);  break; case 9:  CHAIN(9);  break; case 10: CHAIN(10); break; case 11: CHAIN(11); break;
    case 12: CHAIN(12); break; case 13: CHAIN(13); break; case 14: CHAIN(14); break; default: CHAIN(15); break;
    }
}

/* ROUNDS replays with source K; returns how many lost a store. */
static unsigned long run(int k, unsigned long rounds)
{
    volatile unsigned long *d = D.buf;
    unsigned long expect = D.src[k], lost = 0, r;
    for (r = 0; r < rounds; r++) {
        D.buf[0] = 0xDEAD0000UL; D.buf[1] = 0xDEAD0001UL; D.buf[2] = 0xDEAD0002UL;
        replay(k);
        if (d[0] != expect || d[1] != 0 || d[2] != 0)
            lost++;
    }
    return lost;
}

int main(int argc, char **argv)
{
    unsigned long lost, total = 0;
    int k;

    for (k = 0; k < 16; k++)
        D.src[k] = 0x0E410000UL + (unsigned long)k;
    printf("apollo-repro: sources at %p..%p, target at %p\n",
           (void *)&D.src[0], (void *)&D.src[15], (void *)D.buf);

    if (argc > 1) {
        unsigned long dist = (unsigned long)atoi(argv[1]);
        if (dist < 4 || dist > 64 || dist % 4) {
            printf("apollo-repro: distance must be 4..64, a multiple of 4\n");
            return 10;
        }
        k = 16 - (int)(dist / 4);
        lost = run(k, 100000UL);
        printf("apollo-repro: source %lu bytes below the target: %lu of 100000 replays lost a store -- %s\n",
               dist, lost, lost ? "STORES LOST" : "all stores landed");
        return lost ? 20 : 0;
    }

    printf("  source below target   lost of 20000\n");
    for (k = 0; k < 16; k++) {
        lost = run(k, 20000UL);
        total += lost;
        printf("  %2d bytes (%p)   %5lu%s\n", (16 - k) * 4, (void *)&D.src[k], lost,
               lost ? "   <-- LOST" : "");
    }
    printf("apollo-repro: %lu lost store(s) over all sixteen -- %s\n", total,
           total ? "STORES LOST" : "all stores landed");
    return total ? 20 : 0;
}
