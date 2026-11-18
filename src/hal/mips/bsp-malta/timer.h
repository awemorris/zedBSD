/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The malta board's tick, from coprocessor 0's Count and Compare.
 */

#ifndef KERN_HAL_MIPS_BSP_MALTA_TIMER_H
#define KERN_HAL_MIPS_BSP_MALTA_TIMER_H

void
malta_timer_interrupt(
	void);

#endif
