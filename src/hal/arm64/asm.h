/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef KERN_HAL_ARM64_ASM_H
#define KERN_HAL_ARM64_ASM_H

#include <hal/types.h>

void arm64_irq_mask(void);
void arm64_irq_unmask(void);
void arm64_wfe(void);
void arm64_wfi(void);
uint64_t arm64_current_el(void);
void arm64_dsb_sy(void);
void arm64_isb(void);
uint64_t arm64_irq_save(void);
void arm64_irq_restore(uint64_t state);
void arm64_write_ttbr0(uintptr_t physical);
void arm64_write_ttbr1(uintptr_t physical);
void arm64_flush_tlb(void);
void arm64_flush_tlb_local(void);

/*
 * Reports whether the calling CPU's IRQs are masked.
 *
 * Code that runs with them masked cannot be preempted, so it stays on the
 * CPU whose per-CPU record it read.
 */
static __inline int
arm64_irq_masked(
	void)
{
	uint64_t state;

	/* DAIF's bit 7 is the IRQ mask. */
	__asm__ volatile("mrs %0, daif" : "=r"(state));

	/* Masked: the caller is pinned to its CPU. */
	if ((state & (1U << 7)) != 0)
		return 1;

	/* Unmasked: an interrupt may move the caller to another CPU. */
	return 0;
}

#endif
