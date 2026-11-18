/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The platform-level interrupt controller (PLIC).
 */

#ifndef KERN_HAL_RISCV32_PLIC_H
#define KERN_HAL_RISCV32_PLIC_H

#include <hal/types.h>

void
riscv32_plic_init(
	void);

void
riscv32_plic_init_hart(
	void);

uint32_t
riscv32_plic_sources(
	void);

void
riscv32_plic_set_priority(
	uint32_t source,
	uint32_t priority);

void
riscv32_plic_enable(
	uint32_t source,
	int enable);

uint32_t
riscv32_plic_claim(
	void);

void
riscv32_plic_complete(
	uint32_t source);

#endif
