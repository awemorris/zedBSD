/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The 68040's controls the second stage uses, and the trampoline that
 * leaves the ROM for the kernel.
 *
 * Mac OS runs in supervisor mode, so the second stage reads the
 * translation control, asks the MMU where a logical address is, and
 * pushes the caches itself.
 */

/*
 * The function code of supervisor data, which the loader's addresses
 * are in.
 *
 * Values referenced: Motorola, MC68040 User's Manual (MC68040UM/AD,
 * 1993) (the function codes; the MOVEC control registers TC, ITT0/1,
 * DTT0/1, CACR, VBR, MMUSR and DFC; PTESTR, CPUSHA, CINVA, PFLUSHA and
 * FRESTORE of a null frame, which the routines below use).
 */
#define SUPERVISOR_DATA		5

/*
 * uint32_t mac68k_cpu_translation_control(void)
 */
	.text
	.globl mac68k_cpu_translation_control
	.type mac68k_cpu_translation_control,@function
mac68k_cpu_translation_control:
	movec %tc,%d0
	rts
	.size mac68k_cpu_translation_control,.-mac68k_cpu_translation_control

/*
 * uint32_t mac68k_cpu_test_address(uint32_t logical)
 *
 * Reports the MMU status of a supervisor data address.
 */
	.globl mac68k_cpu_test_address
	.type mac68k_cpu_test_address,@function
mac68k_cpu_test_address:
	movea.l 4(%sp),%a0
	movec %dfc,%d1
	moveq #SUPERVISOR_DATA,%d0
	movec %d0,%dfc
	ptestr (%a0)
	movec %mmusr,%d0
	movec %d1,%dfc
	rts
	.size mac68k_cpu_test_address,.-mac68k_cpu_test_address

/*
 * void mac68k_cpu_push_caches(void)
 *
 * Writes the data cache back to memory and empties both caches, after
 * code was written as data.
 */
	.globl mac68k_cpu_push_caches
	.type mac68k_cpu_push_caches,@function
mac68k_cpu_push_caches:
	cpusha %bc
	nop
	rts
	.size mac68k_cpu_push_caches,.-mac68k_cpu_push_caches

/*
 * void mac68k_cpu_mask_interrupts(void)
 */
	.globl mac68k_cpu_mask_interrupts
	.type mac68k_cpu_mask_interrupts,@function
mac68k_cpu_mask_interrupts:
	ori.w #0x0700,%sr
	rts
	.size mac68k_cpu_mask_interrupts,.-mac68k_cpu_mask_interrupts

/*
 * void mac68k_handoff_trampoline(const struct mac68k_plan *plan)
 *
 * Copied to RAM whose logical and physical addresses are the same and
 * entered there; it does not return.  It writes the caches back and
 * turns them off, turns translation and the transparent windows off, so
 * that it keeps running at the same address, follows the plan's steps,
 * resets the vector base and the floating-point unit and enters the
 * kernel.  It is position independent and uses no stack after its entry.
 *
 * Procedure referenced: EMILE (github.com/vivier/EMILE, master)
 * second/enter_kernel040.S (translation and the transparent windows
 * turned off, then the kernel copied to its place and entered); the
 * cache push before, the moves of the plan and the floating-point reset
 * are this loader's own.
 */
	.globl mac68k_handoff_trampoline
	.type mac68k_handoff_trampoline,@function
mac68k_handoff_trampoline:
	movea.l 4(%sp),%a0
	move.w #0x2700,%sr

	/* Writes the caches back, then turns them off and empties them. */
	cpusha %bc
	nop
	moveq #0,%d0
	movec %d0,%cacr
	cinva %bc

	/* Turns translation off and clears the transparent windows. */
	movec %d0,%tc
	pflusha
	movec %d0,%itt0
	movec %d0,%itt1
	movec %d0,%dtt0
	movec %d0,%dtt1

	/* Follows the steps: a copy, or a zero fill. */
	move.l 12(%a0),%d1
	movea.l 16(%a0),%a1
1:	subq.l #1,%d1
	bmi.s 5f
	movea.l (%a1)+,%a2
	movea.l (%a1)+,%a3
	move.l (%a1)+,%d2
	move.l (%a1)+,%d3
	btst #0,%d3
	bne.s 3f
2:	tst.l %d2
	beq.s 1b
	move.b (%a2)+,(%a3)+
	subq.l #1,%d2
	bra.s 2b
3:	tst.l %d2
	beq.s 1b
	clr.b (%a3)+
	subq.l #1,%d2
	bra.s 3b

	/* Resets the vector base and, when there is one, the floating-point unit. */
5:	movec %d0,%vbr
	btst #0,11(%a0)
	beq.s 6f
	frestore 4(%a0)

	/* Enters the kernel. */
6:	movea.l (%a0),%a1
	jmp (%a1)
	.size mac68k_handoff_trampoline,.-mac68k_handoff_trampoline
	.globl mac68k_handoff_trampoline_end
mac68k_handoff_trampoline_end:
