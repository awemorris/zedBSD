/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Single-thread host guards detect reentrant IRQ access and waiting under lock. */
#include <assert.h>
#include <kern/lock.h>

/*
 * Initializes an observable host guard; this does not model CPU memory ordering.
 */
void
spin_init(
	struct spinlock *lock,
	enum lock_rank rank,
	const char *name)
{
	/* Records the same identity as the kernel lock and starts unowned. */
	lock->held.value = 0;
	lock->rank = rank;
	lock->name = name;
	lock->owner_cpu = 0;
	lock->owner_valid = 0;
}

/*
 * Acquires one guard and rejects IRQ or caller reentry into a held section.
 */
unsigned long
spin_lock_irqsave(
	struct spinlock *lock)
{
	/* A model that delivers IRQ during locked publication must fail immediately. */
	assert(lock->held.value == 0);
	lock->held.value = 1;

	/* Succeeded: returns the model's always-enabled incoming IRQ state. */
	return 1;
}

/*
 * Releases a held guard after checking the matching host IRQ state.
 */
void
spin_unlock_irqrestore(
	struct spinlock *lock,
	unsigned long enabled)
{
	/* Verifies balanced release; architecture IRQ masking is outside this model. */
	assert(lock->held.value == 1);
	assert(enabled == 1);
	lock->held.value = 0;
}
