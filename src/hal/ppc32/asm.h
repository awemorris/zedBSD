/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The 32-bit PowerPC instructions the C code of the HAL uses, and the
 * routines of entry.S.
 */

#ifndef KERN_HAL_PPC32_ASM_H
#define KERN_HAL_PPC32_ASM_H

#include <hal/types.h>
#include "defs.h"

/* Reads the machine state. */
static __inline uint32_t
ppc32_read_msr(
	void)
{
	uint32_t value;

	__asm__ volatile("mfmsr %0" : "=r"(value) : : "memory");
	return value;
}

/* Writes the machine state. */
static __inline void
ppc32_write_msr(
	uint32_t value)
{
	__asm__ volatile("mtmsr %0\n\tisync" : : "r"(value) : "memory");
}

/*
 * Turns external interrupts off and reports whether they were on.
 */
static __inline bool
ppc32_irq_save(
	void)
{
	uint32_t state;

	/* Clears the enable bit and keeps the state as it was. */
	state = ppc32_read_msr();
	ppc32_write_msr(state & ~(uint32_t)PPC32_MSR_EE);
	if ((state & PPC32_MSR_EE) != 0)
		return true;
	return false;
}

/*
 * Turns external interrupts back on when ppc32_irq_save() found them on.
 */
static __inline void
ppc32_irq_restore(
	bool enabled)
{
	/* Only interrupts that were on come back. */
	if (enabled)
		ppc32_write_msr(ppc32_read_msr() | PPC32_MSR_EE);
}

/*
 * Reports whether external interrupts are off.
 */
static __inline bool
ppc32_irq_masked(
	void)
{
	/* Off when the enable bit is clear. */
	if ((ppc32_read_msr() & PPC32_MSR_EE) == 0)
		return true;
	return false;
}

/* Reads the time base, both halves, until the upper did not move. */
static __inline uint64_t
ppc32_read_timebase(
	void)
{
	uint32_t upper;
	uint32_t lower;
	uint32_t again;

	do {
		__asm__ volatile("mftbu %0" : "=r"(upper));
		__asm__ volatile("mftb %0" : "=r"(lower));
		__asm__ volatile("mftbu %0" : "=r"(again));
	} while (upper != again);
	return ((uint64_t)upper << 32) | lower;
}

/* Sets the decrementer. */
static __inline void
ppc32_write_decrementer(
	uint32_t value)
{
	__asm__ volatile("mtdec %0" : : "r"(value) : "memory");
}

/* Stops the processor for good. */
static __inline void __attribute__((noreturn))
ppc32_halt_forever(
	void)
{
	/* Interrupts stay off; the loop waits for nothing. */
	ppc32_write_msr(ppc32_read_msr() & ~(uint32_t)PPC32_MSR_EE);
	for (;;)
		__asm__ volatile("" : : : "memory");
}

struct hal_fpregs;

void
ppc32_firmware_call(
	void *arguments);

void
ppc32_cmain(
	uint32_t initrd_address,
	uint32_t initrd_size);

void
ppc32_trap_return(
	void);

void
ppc32_task_dispatch(
	uintptr_t *save,
	const uintptr_t *load);

void
ppc32_kernel_task_entry(
	void);

void
ppc32_user_frame_entry(
	void);

void
ppc32_cpu_idle(
	void);

void
ppc32_cpu_identify(
	void);

bool
ppc32_cpu_has_high_bats(
	void);

void
ppc32_cpu_power_init(
	bool save_power);

void
ppc32_mmu_load(
	const uint32_t *bats,
	const uint32_t *segments,
	uint32_t sdr1,
	uint32_t high_bats);

void
ppc32_load_user_segments(
	const uint32_t *segments);

void
ppc32_tlbie(
	uintptr_t address);

void
ppc32_fp_save(
	struct hal_fpregs *state);

void
ppc32_fp_restore(
	const struct hal_fpregs *state);

extern const uint32_t ppc32_vector_stub[];
extern const uint32_t ppc32_vector_stub_li[];
extern const uint32_t ppc32_vector_stub_end[];

#endif
