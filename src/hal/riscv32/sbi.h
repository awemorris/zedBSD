/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The supervisor's calls into the machine-mode firmware (the SBI).
 */

#ifndef KERN_HAL_RISCV32_SBI_H
#define KERN_HAL_RISCV32_SBI_H

#include <hal/types.h>

/*
 * What one firmware call reported: an error code (0 is success) and a
 * value.
 */
struct riscv32_sbi_result {
	long error;
	long value;
};

/* Every hart, as the hart-mask base of a call that names harts. */
#define RISCV32_SBI_ALL_HARTS	(~0UL)

/* The firmware's hart states (the hart state management extension). */
#define RISCV32_SBI_HART_STARTED	0
#define RISCV32_SBI_HART_STOPPED	1

void
riscv32_sbi_probe(
	void);

void
riscv32_sbi_describe(
	void);

void
riscv32_sbi_version(
	unsigned long *major,
	unsigned long *minor);

void
riscv32_sbi_putchar(
	int character);

void
riscv32_sbi_set_timer(
	uint64_t deadline);

int
riscv32_sbi_send_ipi(
	unsigned long hart_mask,
	unsigned long hart_mask_base);

int
riscv32_sbi_remote_sfence_vma(
	unsigned long hart_mask,
	unsigned long hart_mask_base);

int
riscv32_sbi_remote_fence_i(
	unsigned long hart_mask,
	unsigned long hart_mask_base);

int
riscv32_sbi_hart_start(
	unsigned long hart,
	uintptr_t start_phys,
	uintptr_t opaque);

long
riscv32_sbi_hart_status(
	unsigned long hart);

void
riscv32_sbi_reset(
	int cold)
__attribute__((noreturn));

void
riscv32_sbi_shutdown(
	void)
 __attribute__((noreturn));

uint64_t
riscv32_sbi_mimpid(
	void);

uint64_t
riscv32_sbi_marchid(
	void);

#endif
