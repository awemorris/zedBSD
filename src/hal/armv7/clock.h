/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The armv7 scheduling tick.
 */

#ifndef KERN_HAL_ARMV7_CLOCK_H
#define KERN_HAL_ARMV7_CLOCK_H

#include <hal/hal.h>

void
armv7_clock_init(
	void);

uint32_t
armv7_clock_tick_irq(
	void);

void
armv7_clock_interrupt(
	hal_irq_ack_t acknowledge);

void
armv7_clock_start_kernel(
	void);

unsigned long
armv7_clock_ticks(
	void);

bool
armv7_clock_read(
	uint64_t *counter,
	uint64_t *frequency);

#endif
