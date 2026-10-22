| cpu_store_probe_m68k.s -- does this CPU keep what is stored?  (m68k AmigaOS)
|
| uint32_t platform_cpu_store_probe_m68k(uint32_t replays);
|
| Replays the five-store chain gcc emits for a function prologue that
| initialises locals through a post-incremented pointer,
|
|     move.l (a4),(a0)+ ; move.l 4(a4),(a0)+ ; move.l <abs>,(a0)+ ; clr.l (a0)+ ; clr.l (a0)
|
| into a static buffer (see "The target" below), at every destination
| alignment mod 16 longs, and counts the replays after which the five words
| do not read back as written.  The Apollo 68080 (Vampire V4, core 10760, 2026-09-19) loses or
| delays the last store of that chain -- docs/vampire-68080-lost-stores.md
| is the record (core 11350 no longer has it).
|
| ONE absolute source, on purpose.  Whether the core loses the store
| depends on the ADDRESS of that source (mod 64: 8 of 16 longword offsets
| lose every replay, the other 8 never), and the data hunk lands somewhere
| else on every launch -- so on that core this test flags about every
| second launch, not every launch.  A sweep over sixteen sources at
| consecutive addresses, which would catch every launch, was tried twice
| (from C and as a standalone probe) and both times FROZE THE MACHINE within
| seconds, the display "out of range": switching the source address
| between replays makes the mis-executed store land outside our buffer, in
| hardware registers.  The single-source chain, which is what every gcc
| prologue on such a machine executes anyway, has run for millions of
| replays and a dozen launches without harm.  Keep it that way.
|
| Assembly on purpose: the chain must reach the CPU exactly as gcc emits
| it, and nothing the compiler might choose to store near it (a spilled
| loop variable, say) may sit in the defect's path -- a loop whose counter
| is lost never ends.  Every value that matters lives in a register and
| the only stores are the chain's own and the pattern fill before it
| (register source, which the defect leaves alone).
|
| The target is a STATIC buffer, not the stack: a lost store can also land
| LATE (one probe shape showed it), and a stack buffer is reused by the very next
| call's frame -- a version of this routine with its buffer 1K below the
| caller's frame crashed amilisp at startup with a CHK exception (guru
| 80000006: a return address overwritten), where the calls that follow the
| test put their frames.  The defect does not care where the destination
| is (the placement probe showed the address of the SOURCE decides), so a
| block of our own that nothing else ever reads is the safe target.
|
| Not for MorphOS (PPC): platform_amiga.c answers 0 there without this.

	.text
	.even

	.globl	_platform_cpu_store_probe_m68k
_platform_cpu_store_probe_m68k:
	movem.l	d2-d7/a2-a4,-(sp)	| 9 regs = 36 bytes
	move.l	40(sp),d7		| 36(sp) = return address, 40(sp) = replays
	moveq	#0,d6			| d6 = lost
	moveq	#0,d5			| d5 = replay
	lea	_cpu_probe_src,a4	| a4 = the two indirect sources, as gcc had it
	lea	_cpu_probe_buf,a2	| a2 = the static target block
	move.l	#0xDEAD0000,d4		| the pattern the chain must overwrite
	tst.l	d7
	beq	.Ldone
.Lloop:
	move.l	d5,d0
	and.l	#15,d0
	lsl.l	#2,d0
	lea	0(a2,d0.l),a3		| a3 = dst: every alignment mod 16 longs
	move.l	d4,(a3)			| dirty the five words (register source)
	move.l	d4,4(a3)
	move.l	d4,8(a3)
	move.l	d4,12(a3)
	move.l	d4,16(a3)
	movea.l	a3,a0
	move.l	(a4),(a0)+		| ---- the chain, back to back ----
	move.l	4(a4),(a0)+
	move.l	_cpu_probe_abs,(a0)+
	clr.l	(a0)+
	clr.l	(a0)			| the store the 68080 loses
	move.l	(a3),d0			| ---- read the five words back ----
	cmp.l	(a4),d0
	bne	.Llost
	move.l	4(a3),d0
	cmp.l	4(a4),d0
	bne	.Llost
	move.l	8(a3),d0
	cmp.l	_cpu_probe_abs,d0
	bne	.Llost
	tst.l	12(a3)
	bne	.Llost
	tst.l	16(a3)
	beq	.Lnext
.Llost:
	addq.l	#1,d6			| this replay lost a store
.Lnext:
	addq.l	#1,d5
	cmp.l	d7,d5
	bcs	.Lloop
.Ldone:
	move.l	d6,d0
	movem.l	(sp)+,d2-d7/a2-a4
	rts

	.data
	.balign	4
	.globl	_cpu_probe_src
_cpu_probe_src:
	.long	0x5EC10001, 0x5EC20002
	.globl	_cpu_probe_abs
_cpu_probe_abs:
	.long	0x0E410003

	.bss
	.balign	4
_cpu_probe_buf:
	.skip	128			| 16 alignments x 4 + 5 words, and slack for a late store
