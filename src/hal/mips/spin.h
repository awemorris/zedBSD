/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The spin locks the MIPS HAL keeps its own tables under.
 *
 * The HAL cannot use the kernel's locks: it runs below them, and the
 * kernel's locks call into it.  A HAL lock is a word that is zero when
 * free.  It is taken with interrupts off, so an interrupt handler never
 * spins on a lock the code it interrupted holds.
 */

#ifndef KERN_HAL_MIPS_SPIN_H
#define KERN_HAL_MIPS_SPIN_H

#include <hal/hal.h>
#include "asm.h"

/*
 * Turns interrupts off and reports whether they were on.
 *
 * The release 2 di instruction clears Status.IE and gives the old Status
 * in one step; the hazard barrier lets no later instruction run with
 * interrupts still on.
 */
static __inline bool
mips_irq_save(
	void)
{
	uint32_t status;

	/* Turns interrupts off and takes the old Status. */
	__asm__ volatile("di %0\n\tehb" : "=r"(status) : : "memory");

	/* Reports whether interrupts were on. */
	if ((status & MIPS_STATUS_INTERRUPTS) != 0)
		return true;

	/* Interrupts were already off. */
	return false;
}

/*
 * Turns interrupts back on when they were on before mips_irq_save().
 */
static __inline void
mips_irq_restore(
	bool enabled)
{
	/* Interrupts stay off when they were off. */
	if (!enabled)
		return;

	/* Turns interrupts on and waits out the hazard. */
	__asm__ volatile("ei\n\tehb" : : : "memory");
}

/*
 * Takes a HAL lock with interrupts off.
 *
 * Reports whether interrupts were on, which the matching unlock restores.
 */
static __inline bool
mips_spin_lock(
	volatile unsigned *lock)
{
	bool enabled;
	bool acquired;

	/* Keeps the interrupt handlers out while the lock is held. */
	enabled = mips_irq_save();

	/* Spins until the word goes from free to held. */
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
mips_spin_unlock(
	volatile unsigned *lock,
	bool enabled)
{
	/* Publishes every write made under the lock with the release. */
	__atomic_store_n(lock, 0U, __ATOMIC_RELEASE);

	/* Interrupts come back only if they were on when the lock was taken. */
	mips_irq_restore(enabled);
}

#endif
