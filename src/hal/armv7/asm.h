/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The armv7 HAL's access to the system control coprocessor and the
 * instructions C cannot express.
 *
 * A coprocessor register is named inside the instruction, so each
 * register the HAL uses has its own small function rather than one that
 * takes the register as an argument.
 *
 * Values referenced: ARM DDI 0406C, the CP15 register encodings (opc1,
 * CRn, CRm, opc2) of SCTLR, ACTLR, CPACR, TTBR0, TTBR1, TTBCR, DACR,
 * DFSR, IFSR, DFAR, IFAR, VBAR, CONTEXTIDR, TPIDRURW, TPIDRURO, TPIDRPRW,
 * MIDR, MPIDR, ID_MMFR3, the TLB maintenance operations and the generic
 * timer's CNTFRQ, CNTPCT, CNTP_CTL and CNTP_CVAL.
 */

#ifndef KERN_HAL_ARMV7_ASM_H
#define KERN_HAL_ARMV7_ASM_H

#include <hal/types.h>
#include "defs.h"

/*
 * Reads the current program status register.
 */
static __inline uint32_t
armv7_read_cpsr(
	void)
{
	uint32_t value;

	/* The mode, the interrupt masks and the flags. */
	__asm__ volatile("mrs %0, cpsr" : "=r"(value) : : "memory");

	/* Succeeded: the register's value. */
	return value;
}

/*
 * Turns the calling processor's interrupts off and reports whether they
 * were on.
 */
static __inline bool
armv7_irq_save(
	void)
{
	uint32_t previous;

	/* Reads the masks, then sets the IRQ mask. */
	__asm__ volatile("mrs %0, cpsr\n\t"
			 "cpsid i"
			 : "=r"(previous)
			 :
			 : "memory");

	/* Interrupts were on. */
	if ((previous & ARMV7_PSR_I) == 0)
		return true;

	/* Interrupts were off already. */
	return false;
}

/*
 * Turns the calling processor's interrupts back on when they were on
 * before.
 */
static __inline void
armv7_irq_restore(
	bool enabled)
{
	/* Interrupts come back only if armv7_irq_save() found them on. */
	if (enabled)
		__asm__ volatile("cpsie i" : : : "memory");
}

/*
 * Reports whether the calling processor's interrupts are off.
 *
 * Code that runs with them off cannot be preempted, so it stays on the
 * processor whose per-CPU record it read.
 */
static __inline bool
armv7_irq_masked(
	void)
{
	uint32_t status;

	/* Reads the IRQ mask. */
	status = armv7_read_cpsr();

	/* Masked: the caller is pinned to its processor. */
	if ((status & ARMV7_PSR_I) != 0)
		return true;

	/* Unmasked: an interrupt may move the caller to another processor. */
	return false;
}

/*
 * Waits for an interrupt, which ends the wait even while it is masked.
 */
static __inline void
armv7_wfi(
	void)
{
	/* Completes the earlier memory accesses, then waits. */
	__asm__ volatile("dsb\n\twfi" : : : "memory");
}

/*
 * Orders and completes every earlier memory access and table change.
 */
static __inline void
armv7_dsb(
	void)
{
	/* The full system, both directions. */
	__asm__ volatile("dsb" : : : "memory");
}

/*
 * Makes the instructions after it see the effect of a change to the
 * system registers.
 */
static __inline void
armv7_isb(
	void)
{
	/* Flushes the pipeline. */
	__asm__ volatile("isb" : : : "memory");
}

/*
 * Reads the system control register.
 */
static __inline uint32_t
armv7_read_sctlr(
	void)
{
	uint32_t value;

	/* The MMU, cache, alignment and access flag controls. */
	__asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(value));

	/* Succeeded: the register's value. */
	return value;
}

/*
 * Reads the auxiliary control register.
 */
static __inline uint32_t
armv7_read_actlr(
	void)
{
	uint32_t value;

	/* The processor's own controls, among them the coherency bits. */
	__asm__ volatile("mrc p15, 0, %0, c1, c0, 1" : "=r"(value));

	/* Succeeded: the register's value. */
	return value;
}

/*
 * Reads the coprocessor access control register.
 */
static __inline uint32_t
armv7_read_cpacr(
	void)
{
	uint32_t value;

	/* Which coprocessors, the floating-point unit's cp10 and cp11 among them, each mode may use. */
	__asm__ volatile("mrc p15, 0, %0, c1, c0, 2" : "=r"(value));

	/* Succeeded: the register's value. */
	return value;
}

/*
 * Writes the coprocessor access control register.
 */
static __inline void
armv7_write_cpacr(
	uint32_t value)
{
	/* The new access takes effect for the instructions after the barrier. */
	__asm__ volatile("mcr p15, 0, %0, c1, c0, 2\n\tisb" : : "r"(value) : "memory");
}

/*
 * Reads the main identification register.
 */
static __inline uint32_t
armv7_read_midr(
	void)
{
	uint32_t value;

	/* The implementer, variant, part and revision. */
	__asm__ volatile("mrc p15, 0, %0, c0, c0, 0" : "=r"(value));

	/* Succeeded: the register's value. */
	return value;
}

/*
 * Reads the memory model feature register 3.
 */
static __inline uint32_t
armv7_read_id_mmfr3(
	void)
{
	uint32_t value;

	/* Among its fields, whether the table walk is coherent with the caches. */
	__asm__ volatile("mrc p15, 0, %0, c0, c1, 7" : "=r"(value));

	/* Succeeded: the register's value. */
	return value;
}

/*
 * Reads the multiprocessor affinity register.
 */
static __inline uint32_t
armv7_read_mpidr(
	void)
{
	uint32_t value;

	/* The processor's place in the cluster. */
	__asm__ volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(value));

	/* Succeeded: the register's value. */
	return value;
}

/*
 * Points the exception vectors at a table.
 */
static __inline void
armv7_write_vbar(
	uintptr_t value)
{
	/* The table must be 32-byte aligned; SCTLR.V stays clear. */
	__asm__ volatile("mcr p15, 0, %0, c12, c0, 0\n\tisb" : : "r"(value) : "memory");
}

/*
 * Reads the first-level table TTBR0 selects.
 */
static __inline uint32_t
armv7_read_ttbr0(
	void)
{
	uint32_t value;

	/* The table's physical base and its walk attributes. */
	__asm__ volatile("mrc p15, 0, %0, c2, c0, 0" : "=r"(value));

	/* Succeeded: the register's value. */
	return value;
}

/*
 * Selects the first-level table of the lower half.
 */
static __inline void
armv7_write_ttbr0(
	uint32_t value)
{
	/* The caller orders the change against the context id. */
	__asm__ volatile("mcr p15, 0, %0, c2, c0, 0" : : "r"(value) : "memory");
}

/*
 * Selects the first-level table of the upper half.
 */
static __inline void
armv7_write_ttbr1(
	uint32_t value)
{
	/* The caller completes the change with isb. */
	__asm__ volatile("mcr p15, 0, %0, c2, c0, 1" : : "r"(value) : "memory");
}

/*
 * Sets how the address space is split between TTBR0 and TTBR1.
 */
static __inline void
armv7_write_ttbcr(
	uint32_t value)
{
	/* The caller completes the change with isb. */
	__asm__ volatile("mcr p15, 0, %0, c2, c0, 2" : : "r"(value) : "memory");
}

/*
 * Sets the context id, whose low byte is the address space identifier.
 */
static __inline void
armv7_write_contextidr(
	uint32_t value)
{
	/* The caller completes the change with isb. */
	__asm__ volatile("mcr p15, 0, %0, c13, c0, 1" : : "r"(value) : "memory");
}

/*
 * Reads the data fault status register.
 */
static __inline uint32_t
armv7_read_dfsr(
	void)
{
	uint32_t value;

	/* The last data abort's status. */
	__asm__ volatile("mrc p15, 0, %0, c5, c0, 0" : "=r"(value));

	/* Succeeded: the register's value. */
	return value;
}

/*
 * Reads the instruction fault status register.
 */
static __inline uint32_t
armv7_read_ifsr(
	void)
{
	uint32_t value;

	/* The last prefetch abort's status. */
	__asm__ volatile("mrc p15, 0, %0, c5, c0, 1" : "=r"(value));

	/* Succeeded: the register's value. */
	return value;
}

/*
 * Reads the data fault address register.
 */
static __inline uint32_t
armv7_read_dfar(
	void)
{
	uint32_t value;

	/* The address the last data abort was for. */
	__asm__ volatile("mrc p15, 0, %0, c6, c0, 0" : "=r"(value));

	/* Succeeded: the register's value. */
	return value;
}

/*
 * Reads the instruction fault address register.
 */
static __inline uint32_t
armv7_read_ifar(
	void)
{
	uint32_t value;

	/* The address the last prefetch abort was for. */
	__asm__ volatile("mrc p15, 0, %0, c6, c0, 2" : "=r"(value));

	/* Succeeded: the register's value. */
	return value;
}

/*
 * Reads the privileged thread register, which holds the per-CPU record.
 */
static __inline uintptr_t
armv7_read_tpidrprw(
	void)
{
	uintptr_t value;

	/* Only the kernel can read it, and it never holds anything else. */
	__asm__ volatile("mrc p15, 0, %0, c13, c0, 4" : "=r"(value));

	/* Succeeded: the record's address. */
	return value;
}

/*
 * Points the privileged thread register at the calling processor's record.
 */
static __inline void
armv7_write_tpidrprw(
	uintptr_t value)
{
	/* Every later per-CPU access on this processor reads through it. */
	__asm__ volatile("mcr p15, 0, %0, c13, c0, 4" : : "r"(value) : "memory");
}

/*
 * Sets the user's read-only thread register, the user's thread pointer.
 */
static __inline void
armv7_write_tpidruro(
	uint32_t value)
{
	/* User code reads it; only the kernel writes it. */
	__asm__ volatile("mcr p15, 0, %0, c13, c0, 3" : : "r"(value) : "memory");
}

/*
 * Drops every translation of the inner shareable domain, global or not.
 */
static __inline void
armv7_tlb_flush_all(
	void)
{
	/* TLBIALLIS, completed and seen by the following instructions. */
	__asm__ volatile("dsb ish\n\t"
			 "mcr p15, 0, %0, c8, c3, 0\n\t"
			 "dsb ish\n\t"
			 "isb"
			 : : "r"(0) : "memory");
}

/*
 * Drops every translation of one address space identifier.
 */
static __inline void
armv7_tlb_flush_asid(
	uint32_t asid)
{
	/* TLBIASIDIS, completed and seen by the following instructions. */
	__asm__ volatile("dsb ish\n\t"
			 "mcr p15, 0, %0, c8, c3, 2\n\t"
			 "dsb ish\n\t"
			 "isb"
			 : : "r"(asid & 0xffU) : "memory");
}

/*
 * Drops the translations of one page of one address space identifier.
 */
static __inline void
armv7_tlb_flush_page(
	uintptr_t address,
	uint32_t asid)
{
	/* TLBIMVAIS with the page and the identifier, completed. */
	__asm__ volatile("dsb ish\n\t"
			 "mcr p15, 0, %0, c8, c3, 1\n\t"
			 "dsb ish\n\t"
			 "isb"
			 : : "r"((address & ~(uintptr_t)(ARMV7_PAGE_SIZE - 1)) | (asid & 0xffU)) : "memory");
}

/*
 * Drops the translations of one kernel page, whatever their identifier.
 */
static __inline void
armv7_tlb_flush_kernel_page(
	uintptr_t address)
{
	/* TLBIMVAAIS with the page alone, completed. */
	__asm__ volatile("dsb ish\n\t"
			 "mcr p15, 0, %0, c8, c3, 3\n\t"
			 "dsb ish\n\t"
			 "isb"
			 : : "r"(address & ~(uintptr_t)(ARMV7_PAGE_SIZE - 1)) : "memory");
}

/*
 * Reads the generic timer's counter frequency.
 */
static __inline uint32_t
armv7_read_cntfrq(
	void)
{
	uint32_t value;

	/* The firmware, or the emulator, sets it before the kernel runs. */
	__asm__ volatile("mrc p15, 0, %0, c14, c0, 0" : "=r"(value));

	/* Succeeded: the frequency in hertz. */
	return value;
}

/*
 * Reads the generic timer's physical counter.
 */
static __inline uint64_t
armv7_read_cntpct(
	void)
{
	uint32_t low;
	uint32_t high;

	/* The isb keeps the read from being taken early. */
	__asm__ volatile("isb\n\tmrrc p15, 0, %0, %1, c14" : "=r"(low), "=r"(high) : : "memory");

	/* Succeeded: the 64-bit count. */
	return ((uint64_t)high << 32) | low;
}

/*
 * Sets the physical timer's compare value.
 */
static __inline void
armv7_write_cntp_cval(
	uint64_t value)
{
	uint32_t low;
	uint32_t high;

	/* The two halves in one 64-bit write. */
	low = (uint32_t)value;
	high = (uint32_t)(value >> 32);
	__asm__ volatile("mcrr p15, 2, %0, %1, c14\n\tisb" : : "r"(low), "r"(high) : "memory");
}

/*
 * Sets the physical timer's control register.
 */
static __inline void
armv7_write_cntp_ctl(
	uint32_t value)
{
	/* Bit 0 enables the timer, bit 1 masks its interrupt. */
	__asm__ volatile("mcr p15, 0, %0, c14, c2, 1\n\tisb" : : "r"(value) : "memory");
}

void armv7_halt_forever(void) __attribute__((noreturn));

#endif
