/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The MC68040 instructions the C code of the HAL uses, and the routines of
 * entry.S.
 */

#ifndef KERN_HAL_M68K_M68040_ASM_H
#define KERN_HAL_M68K_M68040_ASM_H

#include <hal/types.h>
#include "defs.h"

/*
 * Raises the interrupt mask to 7 and reports whether it was 0 before.
 */
static __inline bool
m68040_irq_save(
	void)
{
	uint16_t status;

	/* Reads the status register, then closes the mask. */
	__asm__ volatile("move.w %%sr,%0\n\tori.w #0x0700,%%sr" : "=d"(status) : : "memory");

	/* Interrupts were open when the mask was 0. */
	if ((status & M68040_SR_IPL_MASK) == 0)
		return true;
	return false;
}

/*
 * Opens the interrupt mask again when m68040_irq_save() found it open.
 */
static __inline void
m68040_irq_restore(
	bool enabled)
{
	/* Only a mask that was open is opened. */
	if (enabled)
		__asm__ volatile("andi.w #0xf8ff,%%sr" : : : "memory");
}

/*
 * Reports whether the interrupt mask is closed.
 */
static __inline bool
m68040_irq_masked(
	void)
{
	uint16_t status;

	/* The mask is the status register's three priority bits. */
	__asm__ volatile("move.w %%sr,%0" : "=d"(status) : : "memory");
	if ((status & M68040_SR_IPL_MASK) != 0)
		return true;
	return false;
}

/* Stops the processor for good. */
static __inline void __attribute__((noreturn))
m68040_halt_forever(
	void)
{
	/* The mask stays closed, so nothing wakes it. */
	for (;;)
		__asm__ volatile("stop #0x2700" : : : "memory");
}

void m68040_exception_entry(void);
void m68040_trap_return(void);
void m68040_task_dispatch(uintptr_t *save, const uintptr_t *load);
void m68040_kernel_task_entry(void);
void m68040_user_frame_entry(void);
void m68040_cpu_idle(void);
void m68040_set_vbr(const void *table);
void m68040_load_root(uint32_t root);
void m68040_flush_atc(void);
void m68040_mmu_enable(uint32_t ram_window, uint32_t device_window, uint32_t root);
void m68040_cache_push_all(void);
void m68040_cache_enable(void);
void m68040_fp_save(struct hal_fpregs *state);
void m68040_fp_restore(const struct hal_fpregs *state);

extern const uint32_t m68040_vector_table[256];

#endif
