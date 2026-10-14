| ffi_dispatch_m68k.s — AmigaOS library call trampoline for AmiLisp FFI
|
| uint32_t platform_amiga_call(uint32_t lib_base, int16_t offset,
|                               uint32_t *regs, uint16_t reg_mask);
|
| Calls an AmigaOS shared library function using the standard register-based
| calling convention.  Loads d0-d7/a0-a4 from the regs[] array (14 uint32_t
| entries, indexed as d0=0..d7=7, a0=8..a5=13), sets a6 = lib_base, and
| calls through the library jump table at base+offset.
|
| a5 is used as scratch for the function address and cannot be used as an
| argument register (no standard AmigaOS function uses a5 as input).
|
| All 13 argument registers are loaded unconditionally from the pre-zeroed
| regs array.  reg_mask is accepted for API compatibility but not checked.
|
| Returns d0 (the standard AmigaOS return register).

	.text
	.globl	_platform_amiga_call
	.even

_platform_amiga_call:
	| Save callee-saved registers (d2-d7, a2-a6 = 11 regs = 44 bytes)
	movem.l	d2-d7/a2-a6,-(sp)

	| Stack layout after save:
	|   0(sp)..43(sp) = saved d2-d7/a2-a6
	|   44(sp)        = return address
	|   48(sp)        = lib_base   (uint32_t)
	|   52(sp)        = offset     (int16_t, promoted to int32)
	|   56(sp)        = regs       (uint32_t *)
	|   60(sp)        = reg_mask   (uint16_t, promoted to int32)

	| Load arguments from stack
	move.l	56(sp),a5		| a5 = regs array pointer
	move.l	48(sp),a4		| a4 = lib_base
	move.l	52(sp),d6		| d6 = offset (negative LVO)

	| Compute jump table entry address: lib_base + offset
	move.l	a4,a3
	add.l	d6,a3			| a3 = function address in jump table

	| Save lib_base and func_addr on stack so we can recover
	| them after loading all argument registers
	move.l	a4,-(sp)		| push lib_base   (sp -= 4)
	move.l	a3,-(sp)		| push func_addr  (sp -= 4)

	| Load all argument registers from regs[] array:
	|   regs[0..7]  -> d0-d7
	|   regs[8..12] -> a0-a4
	| a5 is our scratch (regs pointer), not loaded from array.
	| a6 will be set to lib_base below.
	movem.l	(a5),d0-d7/a0-a4	| load 13 registers

	| Recover func_addr and lib_base from stack
	move.l	(sp)+,a5		| a5 = func_addr (scratch)
	move.l	(sp)+,a6		| a6 = lib_base

	| Call the library function through the jump table
	jsr	(a5)

	| d0 now holds the return value

	| Restore callee-saved registers
	movem.l	(sp)+,d2-d7/a2-a6

	rts
