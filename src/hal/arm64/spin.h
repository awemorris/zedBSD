/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The spin locks the arm64 HAL keeps its own tables under.
 *
 * The HAL cannot use the kernel's locks: it runs below them, and the
 * kernel's locks call into it.  A HAL lock is a word that is zero when
 * free.  It is taken with interrupts off on the taking CPU, so an
 * interrupt handler on the same CPU never spins on a lock its own CPU
 * holds.
 */

#ifndef KERN_HAL_ARM64_SPIN_H
#define KERN_HAL_ARM64_SPIN_H

#include <hal/hal.h>
#include "watch.h"

/*
 * Takes a HAL lock with interrupts off.
 *
 * Reports whether interrupts were on, which the matching unlock restores.
 */
static __inline bool
arm64_spin_lock(
	volatile unsigned *lock)
{
	bool enabled;
	bool acquired;
	unsigned tries;

	/* Keeps this CPU's interrupt handlers out while the lock is held. */
	enabled = hal_irq_disable();

	/* Spins until the word goes from free to held under this CPU, now and then looking for its own stall. */
	tries = 0;
	for (;;) {
		acquired = hal_atomic_uint_try_acquire(lock);
		if (acquired)
			break;
		hal_atomic_relax();
		tries++;
		if ((tries % ARM64_WATCH_SPIN_INTERVAL) == 0)
			arm64_watch_self_check();
	}

	/* Succeeded: the lock is held, and the caller keeps the interrupt state. */
	return enabled;
}

/*
 * Releases a HAL lock and puts the interrupt state back.
 */
static __inline void
arm64_spin_unlock(
	volatile unsigned *lock,
	bool enabled)
{
	/* Publishes every write made under the lock with the release. */
	__atomic_store_n(lock, 0U, __ATOMIC_RELEASE);

	/* Interrupts come back only if they were on when the lock was taken. */
	if (enabled)
		hal_irq_enable();
}

#endif
