/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv32 HAL's access to the supervisor registers and the
 * instructions C cannot express.
 *
 * A control and status register is named inside the instruction, so each
 * register the HAL uses has its own small function rather than one that
 * takes the register as an argument.
 */

#ifndef KERN_HAL_RISCV32_ASM_H
#define KERN_HAL_RISCV32_ASM_H

#include <hal/types.h>
#include "defs.h"

/*
 * Reads the supervisor status register.
 */
static __inline unsigned long
riscv32_read_sstatus(
	void)
{
	unsigned long value;

	/* The register holds the interrupt enable, the previous mode and the FP state. */
	__asm__ volatile("csrr %0, sstatus" : "=r"(value));

	/* Succeeded: the register's value. */
	return value;
}

/*
 * Sets bits of the supervisor status register.
 */
static __inline void
riscv32_set_sstatus(
	unsigned long bits)
{
	/* The memory clobber keeps an interrupt enable after the stores it guards. */
	__asm__ volatile("csrs sstatus, %0" : : "r"(bits) : "memory");
}

/*
 * Clears bits of the supervisor status register.
 */
static __inline void
riscv32_clear_sstatus(
	unsigned long bits)
{
	/* The memory clobber keeps an interrupt disable before the stores it guards. */
	__asm__ volatile("csrc sstatus, %0" : : "r"(bits) : "memory");
}

/*
 * Turns the calling hart's interrupts off and reports whether they were on.
 */
static __inline bool
riscv32_irq_save(
	void)
{
	unsigned long previous;

	/* Clears the enable bit and keeps the register as it was, in one instruction. */
	__asm__ volatile("csrrci %0, sstatus, 2" : "=r"(previous) : : "memory");

	/* Interrupts were on. */
	if ((previous & RISCV32_SSTATUS_SIE) != 0)
		return true;

	/* Interrupts were off already. */
	return false;
}

/*
 * Turns the calling hart's interrupts back on when they were on before.
 */
static __inline void
riscv32_irq_restore(
	bool enabled)
{
	/* Interrupts come back only if riscv32_irq_save() found them on. */
	if (enabled)
		__asm__ volatile("csrsi sstatus, 2" : : : "memory");
}

/*
 * Reports whether the calling hart's interrupts are off.
 *
 * Code that runs with them off cannot be preempted, so it stays on the
 * hart whose per-CPU record it read.
 */
static __inline bool
riscv32_irq_masked(
	void)
{
	unsigned long status;

	/* Reads the global supervisor interrupt enable. */
	status = riscv32_read_sstatus();

	/* Off: the caller is pinned to its hart. */
	if ((status & RISCV32_SSTATUS_SIE) == 0)
		return true;

	/* On: an interrupt may move the caller to another hart. */
	return false;
}

/*
 * Sets bits of the supervisor interrupt enable register.
 */
static __inline void
riscv32_set_sie(
	unsigned long bits)
{
	/* Each bit lets one kind of interrupt reach the hart. */
	__asm__ volatile("csrs sie, %0" : : "r"(bits) : "memory");
}

/*
 * Clears bits of the supervisor interrupt enable register.
 */
static __inline void
riscv32_clear_sie(
	unsigned long bits)
{
	/* Each bit keeps one kind of interrupt from the hart. */
	__asm__ volatile("csrc sie, %0" : : "r"(bits) : "memory");
}

/*
 * Clears pending bits of the supervisor interrupt pending register.
 */
static __inline void
riscv32_clear_sip(
	unsigned long bits)
{
	/* Only the software interrupt is writable from supervisor mode. */
	__asm__ volatile("csrc sip, %0" : : "r"(bits) : "memory");
}

/*
 * Points the hart's traps at a handler, in direct mode.
 */
static __inline void
riscv32_write_stvec(
	uintptr_t handler)
{
	/* The low two bits select direct mode when they are zero. */
	__asm__ volatile("csrw stvec, %0" : : "r"(handler) : "memory");
}

/*
 * Installs a root table and drops every translation the hart cached.
 */
static __inline void
riscv32_write_satp(
	unsigned long value)
{
	/* Orders the table stores before the switch and empties the TLB after it. */
	__asm__ volatile("sfence.vma\n\tcsrw satp, %0\n\tsfence.vma" : : "r"(value) : "memory");
}

/*
 * Reads the translation the hart uses.
 */
static __inline unsigned long
riscv32_read_satp(
	void)
{
	unsigned long value;

	/* The mode and the root table's page number. */
	__asm__ volatile("csrr %0, satp" : "=r"(value));

	/* Succeeded: the register's value. */
	return value;
}

/*
 * Drops every translation the calling hart cached.
 */
static __inline void
riscv32_sfence_vma_all(
	void)
{
	/* Also orders the table stores made before it. */
	__asm__ volatile("sfence.vma" : : : "memory");
}

/*
 * Makes the calling hart fetch instructions that were stored as data.
 */
static __inline void
riscv32_fence_i(
	void)
{
	/* Only the calling hart's instruction fetch is synchronized. */
	__asm__ volatile("fence.i" : : : "memory");
}

/*
 * Waits for an interrupt that the interrupt enable register lets in.
 */
static __inline void
riscv32_wfi(
	void)
{
	/* Returns when such an interrupt is pending, taken or not. */
	__asm__ volatile("wfi" : : : "memory");
}

/*
 * Reads the time counter, which runs at the device tree's timebase.
 */
static __inline uint64_t
riscv32_read_time(
	void)
{
	uint32_t high;
	uint32_t low;
	uint32_t again;

	/* Reads the halves until the high half did not move between them. */
	do {
		__asm__ volatile("rdtimeh %0" : "=r"(high) : : "memory");
		__asm__ volatile("rdtime %0" : "=r"(low) : : "memory");
		__asm__ volatile("rdtimeh %0" : "=r"(again) : : "memory");
	} while (high != again);

	/* Succeeded: the counter now. */
	return ((uint64_t)high << 32) | low;
}

/*
 * Reads the thread pointer, which holds the per-CPU record in the kernel.
 */
static __inline uintptr_t
riscv32_read_tp(
	void)
{
	uintptr_t value;

	/* The kernel's C code never uses tp for anything else. */
	__asm__ volatile("mv %0, tp" : "=r"(value));

	/* Succeeded: the record's address. */
	return value;
}

/*
 * Points the thread pointer at the calling hart's per-CPU record.
 */
static __inline void
riscv32_write_tp(
	uintptr_t value)
{
	/* Every later per-CPU access on this hart reads through it. */
	__asm__ volatile("mv tp, %0" : : "r"(value) : "memory");
}

void riscv32_halt_forever(void) __attribute__((noreturn));

#endif
