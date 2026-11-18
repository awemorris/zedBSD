/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The HAL's locks.  The port is uniprocessor, so a lock is the interrupt
 * mask and a word that catches a nested take.
 */

#ifndef KERN_HAL_M68K_M68040_SPIN_H
#define KERN_HAL_M68K_M68040_SPIN_H

#include <hal/hal.h>
#include "asm.h"

/*
 * Takes a lock with interrupts off and reports whether they were on.
 */
static __inline bool
m68040_spin_lock(
	volatile unsigned *lock)
{
	bool enabled;

	/* Nothing else can run once the mask is closed. */
	enabled = m68040_irq_save();
	if (*lock != 0U)
		HAL_FATAL("m68040 lock taken twice");
	*lock = 1U;

	/* Succeeded: the caller holds the lock. */
	return enabled;
}

/*
 * Releases a lock and puts interrupts back as they were.
 */
static __inline void
m68040_spin_unlock(
	volatile unsigned *lock,
	bool enabled)
{
	/* Releases, then opens the mask if it was open. */
	*lock = 0U;
	m68040_irq_restore(enabled);
}

#endif
