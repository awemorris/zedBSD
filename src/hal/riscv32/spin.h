/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The spin locks the riscv32 HAL keeps its own tables under.
 *
 * The HAL cannot use the kernel's locks: it runs below them, and the
 * kernel's locks call into it.  A HAL lock is a word that is zero when
 * free.  It is taken with interrupts off on the taking hart, so an
 * interrupt handler on the same hart never spins on a lock its own hart
 * holds.
 */

#ifndef KERN_HAL_RISCV32_SPIN_H
#define KERN_HAL_RISCV32_SPIN_H

#include <hal/hal.h>
#include "asm.h"

/*
 * Takes a HAL lock with interrupts off.
 *
 * Reports whether interrupts were on, which the matching unlock restores.
 */
static __inline bool
riscv32_spin_lock(
	volatile unsigned *lock)
{
	bool enabled;
	bool acquired;

	/* Keeps this hart's interrupt handlers out while the lock is held. */
	enabled = riscv32_irq_save();

	/* Spins until the word goes from free to held under this hart. */
	for (;;) {
		acquired = hal_atomic_uint_try_acquire(lock);
		if (acquired)
			break;
		hal_atomic_relax();
	}

	/* Succeeded: the lock is held, and the caller keeps the interrupt state. */
	return enabled;
}

/*
 * Releases a HAL lock and puts the interrupt state back.
 */
static __inline void
riscv32_spin_unlock(
	volatile unsigned *lock,
	bool enabled)
{
	/* Publishes every write made under the lock with the release. */
	__atomic_store_n(lock, 0U, __ATOMIC_RELEASE);

	/* Interrupts come back only if they were on when the lock was taken. */
	riscv32_irq_restore(enabled);
}

#endif
