/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <kern/lock.h>
#include <kern/waitq.h>
#include <kern/signal.h>
#include <kern/thread.h>

#include <errno.h>
#include <hal/hal.h>

void spin_init(struct spinlock *lock, enum lock_rank rank, const char *name)
{ lock->held.value = 0; lock->rank = rank; lock->name = name; }
int spin_trylock(struct spinlock *lock)
{
	return atomic_try_acquire_zero(&lock->held);
}
void spin_lock(struct spinlock *lock)
{ while (!spin_trylock(lock)) __asm__ volatile("" ::: "memory"); }
void spin_unlock(struct spinlock *lock)
{
	if (atomic_load_acquire(&lock->held) == 0) __builtin_trap();
	atomic_store_release(&lock->held, 0);
}
unsigned long spin_lock_irqsave(struct spinlock *lock)
{ unsigned long enabled = hal_irq_disable() ? 1UL : 0UL; spin_lock(lock); return enabled; }
void spin_unlock_irqrestore(struct spinlock *lock, unsigned long enabled)
{ spin_unlock(lock); if (enabled != 0) hal_irq_enable(); }

int mutex_init(struct mutex *mutex, enum lock_rank rank, const char *name)
{
	if (mutex == NULL) return EINVAL;
	spin_init(&mutex->guard, rank, name);
	mutex->owner = NULL; mutex->locked = 0;
	mutex->waiters = NULL;
	return 0;
}
int mutex_lock_interruptible(struct mutex *mutex)
{
	unsigned long irq;
	struct thread *thread = thread_current();
	if (mutex == NULL || thread == NULL) return EINVAL;
	irq = spin_lock_irqsave(&mutex->guard);
	if (mutex->owner == thread) __builtin_trap();
	while (mutex->locked) {
		spin_unlock_irqrestore(&mutex->guard, irq);
		if (signal_pending_unblocked(thread)) return EINTR;
		sched_yield();
		irq = spin_lock_irqsave(&mutex->guard);
	}
	mutex->locked = 1; mutex->owner = thread;
	spin_unlock_irqrestore(&mutex->guard, irq);
	return 0;
}
void mutex_lock(struct mutex *mutex)
{ while (mutex_lock_interruptible(mutex) == EINTR) ; }
void mutex_unlock(struct mutex *mutex)
{
	unsigned long irq;
	if (mutex == NULL) __builtin_trap();
	irq = spin_lock_irqsave(&mutex->guard);
	if (!mutex->locked || mutex->owner != thread_current()) __builtin_trap();
	mutex->owner = NULL; mutex->locked = 0;
	spin_unlock_irqrestore(&mutex->guard, irq);
}
