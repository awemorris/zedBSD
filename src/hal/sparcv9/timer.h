/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The SPARC V9 HAL's periodic tick.
 */

#ifndef KERN_HAL_SPARCV9_TIMER_H
#define KERN_HAL_SPARCV9_TIMER_H

#include <hal/types.h>

/*
 * Starts the periodic tick from the processor's cycle counter.
 */
void
sparcv9_timer_init(
	uint64_t frequency);

/*
 * Handles one tick interrupt.
 */
void
sparcv9_timer_interrupt(
	void);

#endif
