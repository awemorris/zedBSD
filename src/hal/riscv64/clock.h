/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv64 scheduling tick.
 */

#ifndef KERN_HAL_RISCV64_CLOCK_H
#define KERN_HAL_RISCV64_CLOCK_H

void
riscv64_clock_init(
	void);

void
riscv64_clock_interrupt(
	void);

bool
riscv64_clock_ticked(
	void);

#endif
