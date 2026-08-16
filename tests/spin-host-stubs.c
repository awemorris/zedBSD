/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <kern/lock.h>

#include <threads.h>

void spin_init(struct spinlock *lock, enum lock_rank rank, const char *name)
{
	lock->held.value = 0;
	lock->rank = rank;
	lock->name = name;
	lock->owner_cpu = 0;
	lock->owner_valid = 0;
}

void spin_lock(struct spinlock *lock)
{
	while (!atomic_try_acquire_zero(&lock->held))
		thrd_yield();
}

int spin_trylock(struct spinlock *lock)
{
	return atomic_try_acquire_zero(&lock->held);
}

void spin_unlock(struct spinlock *lock)
{
	atomic_store_release(&lock->held, 0);
}

unsigned long spin_lock_irqsave(struct spinlock *lock)
{
	spin_lock(lock);
	return 0;
}

void spin_unlock_irqrestore(struct spinlock *lock, unsigned long irq)
{
	(void)irq;
	spin_unlock(lock);
}
