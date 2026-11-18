/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The MIPS exception vectors, which EBase points at.
 *
 * The TLB refill walks the page tables of the space selected now and
 * writes the pair of pages the missed address is in.  The 32-bit
 * processor takes it at EBase + 0x000 and walks two levels; the 64-bit
 * one, whose user segment is xuseg (Status.UX), takes it at EBase +
 * 0x080 and walks three.  The general exception goes to the trap
 * handling of trap.S, and so does the interrupt vector at EBase + 0x200,
 * in case Cause.IV is set.  The refill the processor does not use and the
 * cache error report the exception on the console and stop: the
 * processor's state is passed to C on a stack of its own, since the
 * stack pointer of the code that faulted may be what failed.
 */

#include "asm.h"

	.set noreorder
	.set noat

	/*
	 * The vectors are at fixed offsets from EBase, which takes an
	 * address aligned to 4 KiB.
	 */
	.section .text.vectors, "ax"
	.balign 4096
	.global mips_vectors
mips_vectors:
	/*
	 * The TLB refill, with k0 and k1 alone and nothing else touched.
	 *
	 * It counts itself for the self-test, then reads the root entry of
	 * BadVAddr from the selected space's root.  An empty root entry
	 * writes an invalid pair, so that the retried access takes a TLB
	 * invalid exception, which the general handler reports as a fault.
	 * Otherwise the even and odd entries of the pair, shifted right by
	 * six, are EntryLo0 and EntryLo1.  The hardware already put the
	 * pair's VPN2 and the space's ASID into EntryHi.  A refill comes only
	 * for a pair the TLB does not hold, so tlbwr makes no duplicate.
	 */
	.org MIPS_VECTOR_REFILL
#if defined(HAL_ARCH_MIPS64)
	j mips_early_vector
	nop
#else
	.global mips_tlb_refill
mips_tlb_refill:
	la $26, mips_tlb_refill_count
	lw $27, 0($26)
	addiu $27, $27, 1
	sw $27, 0($26)
	mfc0 $26, MIPS_CP0_BAD_ADDRESS
	la $27, mips_tlb_root
	lw $27, 0($27)
	srl $26, $26, MIPS_ROOT_SHIFT
	sll $26, $26, 2
	addu $27, $27, $26
	lw $27, 0($27)
	beqz $27, 1f
	mfc0 $26, MIPS_CP0_BAD_ADDRESS
	srl $26, $26, MIPS_PAGE_SHIFT - 2
	andi $26, $26, (MIPS_TABLE_ENTRIES - 2) * 4
	addu $27, $27, $26
	lw $26, 0($27)
	lw $27, 4($27)
	srl $26, $26, MIPS_PTE_HARDWARE_SHIFT
	mtc0 $26, MIPS_CP0_ENTRY_LO0
	srl $27, $27, MIPS_PTE_HARDWARE_SHIFT
	mtc0 $27, MIPS_CP0_ENTRY_LO1
	ehb
	tlbwr
	eret
1:
	mtc0 $0, MIPS_CP0_ENTRY_LO0
	mtc0 $0, MIPS_CP0_ENTRY_LO1
	ehb
	tlbwr
	eret
#endif

	.org MIPS_VECTOR_REFILL64
#if defined(HAL_ARCH_MIPS64)
	j mips_xtlb_refill
	nop
#else
	j mips_early_vector
	nop
#endif

	.org MIPS_VECTOR_CACHE_ERROR
	j mips_early_vector
	nop

	.org MIPS_VECTOR_GENERAL
	j mips_trap_entry
	nop

	/*
	 * The interrupt's own vector.  locore.S clears Cause.IV, so that
	 * interrupts come to the general exception; a loader may have left
	 * it set, and this keeps an interrupt taken then on the same path.
	 */
	.org MIPS_VECTOR_INTERRUPT
	j mips_trap_entry
	nop

#if defined(HAL_ARCH_MIPS64)
	/*
	 * The 64-bit TLB refill, with k0 and k1 alone, the vector's 128
	 * bytes being too few for the walk.
	 *
	 * It counts itself for the self-test.  A miss at or above the 512
	 * GiB the tables reach (a kernel address of xkseg among them) writes
	 * nothing and goes to the general exception, where it is a fault.
	 * Otherwise it walks the root and the middle table of the selected
	 * space: an empty entry on the way writes an invalid pair, so that
	 * the retried access takes a TLB invalid exception, which the
	 * general handler reports as a fault.  The leaf's even and odd
	 * entries, shifted right by six, are EntryLo0 and EntryLo1; the
	 * hardware already put the pair's VPN2 and the space's ASID into
	 * EntryHi.  BadVAddr is read again at each level rather than kept,
	 * since only k0 and k1 are free.
	 */
	.text
	.balign 4
	.type mips_xtlb_refill, %function
mips_xtlb_refill:
	LOAD_ADDRESS($26, mips_tlb_refill_count)
	lw $27, 0($26)
	addiu $27, $27, 1
	sw $27, 0($26)
	dmfc0 $26, MIPS_CP0_BAD_ADDRESS
	dsrl $26, $26, MIPS_USER_SHIFT
	bnez $26, 2f
	nop
	LOAD_ADDRESS($27, mips_tlb_root)
	ld $27, 0($27)
	dmfc0 $26, MIPS_CP0_BAD_ADDRESS
	dsrl $26, $26, MIPS_ROOT_SHIFT - MIPS_TABLE_ENTRY_SHIFT
	andi $26, $26, (MIPS_TABLE_ENTRIES - 1) << MIPS_TABLE_ENTRY_SHIFT
	daddu $27, $27, $26
	ld $27, 0($27)
	beqz $27, 1f
	dmfc0 $26, MIPS_CP0_BAD_ADDRESS
	dsrl $26, $26, MIPS_MIDDLE_SHIFT - MIPS_TABLE_ENTRY_SHIFT
	andi $26, $26, (MIPS_TABLE_ENTRIES - 1) << MIPS_TABLE_ENTRY_SHIFT
	daddu $27, $27, $26
	ld $27, 0($27)
	beqz $27, 1f
	dmfc0 $26, MIPS_CP0_BAD_ADDRESS
	dsrl $26, $26, MIPS_PAGE_SHIFT - MIPS_TABLE_ENTRY_SHIFT
	andi $26, $26, (MIPS_TABLE_ENTRIES - 2) << MIPS_TABLE_ENTRY_SHIFT
	daddu $27, $27, $26
	ld $26, 0($27)
	ld $27, 8($27)
	dsrl $26, $26, MIPS_PTE_HARDWARE_SHIFT
	dmtc0 $26, MIPS_CP0_ENTRY_LO0
	dsrl $27, $27, MIPS_PTE_HARDWARE_SHIFT
	dmtc0 $27, MIPS_CP0_ENTRY_LO1
	ehb
	tlbwr
	eret
1:
	dmtc0 $0, MIPS_CP0_ENTRY_LO0
	dmtc0 $0, MIPS_CP0_ENTRY_LO1
	ehb
	tlbwr
	eret
2:
	j mips_trap_entry
	nop
	.size mips_xtlb_refill, . - mips_xtlb_refill
#endif

	/*
	 * Reports an exception the trap handling does not take:
	 * Cause, EPC, BadVAddr and Status go to mips_early_exception(), which
	 * does not return.
	 */
	.balign 4
	.type mips_early_vector, %function
mips_early_vector:
	LOAD_ADDRESS($29, mips_trap_stack_top)
	PTR_ADDIU $29, $29, -MIPS_ARGUMENT_HOME_SIZE
	mfc0 $4, MIPS_CP0_CAUSE
	mfc0 $5, MIPS_CP0_EXCEPTION_PC
	mfc0 $6, MIPS_CP0_BAD_ADDRESS
	jal mips_early_exception
	mfc0 $7, MIPS_CP0_STATUS

	/* The report does not return; waits here for good if it does. */
1:
	wait
	b 1b
	nop
	.size mips_early_vector, . - mips_early_vector

	/* The stack of the early exception report. */
	.section .bss.boot, "aw", %nobits
	.balign 16
mips_trap_stack:
	.space MIPS_TRAP_STACK_SIZE
mips_trap_stack_top:
