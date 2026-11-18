/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The 64-bit PowerPC instructions the C code of the HAL uses, and the
 * routines of entry.S.
 */

#ifndef KERN_HAL_PPC64_ASM_H
#define KERN_HAL_PPC64_ASM_H

#include <hal/types.h>
#include "defs.h"

/* Reads the machine state. */
static __inline uint64_t
ppc64_read_msr(
	void)
{
	uint64_t value;

	__asm__ volatile("mfmsr %0" : "=r"(value) : : "memory");
	return value;
}

/* Writes the machine state. */
static __inline void
ppc64_write_msr(
	uint64_t value)
{
	__asm__ volatile("mtmsrd %0\n\tisync" : : "r"(value) : "memory");
}

/*
 * Turns external interrupts off and reports whether they were on.
 */
static __inline bool
ppc64_irq_save(
	void)
{
	uint64_t state;

	/* Clears the enable bit and keeps the state as it was. */
	state = ppc64_read_msr();
	ppc64_write_msr(state & ~(uint64_t)PPC64_MSR_EE);
	if ((state & PPC64_MSR_EE) != 0)
		return true;
	return false;
}

/*
 * Turns external interrupts back on when ppc64_irq_save() found them on.
 */
static __inline void
ppc64_irq_restore(
	bool enabled)
{
	/* Only interrupts that were on come back. */
	if (enabled)
		ppc64_write_msr(ppc64_read_msr() | PPC64_MSR_EE);
}

/*
 * Reports whether external interrupts are off.
 */
static __inline bool
ppc64_irq_masked(
	void)
{
	/* Off when the enable bit is clear. */
	if ((ppc64_read_msr() & PPC64_MSR_EE) == 0)
		return true;
	return false;
}

/* Reads the time base. */
static __inline uint64_t
ppc64_read_timebase(
	void)
{
	uint64_t value;

	__asm__ volatile("mftb %0" : "=r"(value) : : "memory");
	return value;
}

/* Sets the decrementer. */
static __inline void
ppc64_write_decrementer(
	uint32_t value)
{
	__asm__ volatile("mtdec %0" : : "r"((uint64_t)value) : "memory");
}

/* Stops the processor for good. */
static __inline void __attribute__((noreturn))
ppc64_halt_forever(
	void)
{
	/* Interrupts stay off; the loop waits for nothing. */
	ppc64_write_msr(ppc64_read_msr() & ~(uint64_t)PPC64_MSR_EE);
	for (;;)
		__asm__ volatile("" : : : "memory");
}

struct hal_fpregs;

void
ppc64_firmware_call(
	void *arguments);

long
ppc64_hcall(
	uint64_t call,
	uint64_t *arguments);

void
ppc64_trap_return(
	void);

void
ppc64_task_dispatch(
	uintptr_t *save,
	const uintptr_t *load);

void
ppc64_kernel_task_entry(
	void);

void
ppc64_user_frame_entry(
	void);

void
ppc64_fp_save(
	struct hal_fpregs *state);

void
ppc64_fp_restore(
	const struct hal_fpregs *state);

extern const uint32_t ppc64_vector_stub[];
extern const uint32_t ppc64_vector_stub_li[];
extern const uint32_t ppc64_vector_stub_end[];

#endif
