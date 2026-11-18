/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv32 scheduling tick.
 */

#ifndef KERN_HAL_RISCV32_CLOCK_H
#define KERN_HAL_RISCV32_CLOCK_H

void
riscv32_clock_init(
	void);

void
riscv32_clock_interrupt(
	void);

bool
riscv32_clock_ticked(
	void);

#endif
