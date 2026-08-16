/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <kern/lock.h>
#include <kern/waitq.h>

#include <errno.h>
#include <hal/hal.h>

extern uint64_t sched_ticks(void);
extern void sched_sleep(uint64_t);
extern int signal_pending_unblocked(const struct thread *);
extern struct thread *thread_current(void);

void
spin_init(struct spinlock *lock, enum lock_rank rank, const char *name)
{
	lock->held.value = 0;
	lock->rank = rank;
	lock->name = name;
	lock->owner_cpu = 0;
	lock->owner_valid = 0;
}

void spin_lock(struct spinlock *lock)
{ while (!atomic_try_acquire_zero(&lock->held)) ; }
int spin_trylock(struct spinlock *lock)
{ return atomic_try_acquire_zero(&lock->held); }
void spin_unlock(struct spinlock *lock)
{ atomic_store_release(&lock->held, 0); }
unsigned long spin_lock_irqsave(struct spinlock *lock)
{ unsigned long irq = hal_irq_disable() ? 1UL : 0UL; spin_lock(lock); return irq; }
void spin_unlock_irqrestore(struct spinlock *lock, unsigned long irq)
{ spin_unlock(lock); if (irq != 0) hal_irq_enable(); }

void waitq_init(struct wait_queue *queue, const char *name)
{ queue->head = queue->tail = NULL; queue->sequence = 1; queue->name = name; }
uint64_t waitq_sequence(const struct wait_queue *queue)
{ return queue->sequence; }
int
waitq_sleep(struct wait_queue *queue, struct spinlock *lock,
	uint64_t observed, uint64_t deadline, unsigned flags)
{
	struct thread *thread = thread_current();

	if (queue->sequence != observed)
		return EAGAIN;
	spin_unlock(lock);
	if (deadline != 0)
		sched_sleep(deadline);
	spin_lock(lock);
	if ((flags & WAITQ_INTERRUPTIBLE) != 0 && thread != NULL &&
	    signal_pending_unblocked(thread))
		return EINTR;
	if (deadline != 0 && sched_ticks() >= deadline)
		return ETIMEDOUT;
	return EAGAIN;
}
void waitq_wake_one(struct wait_queue *queue)
{ queue->sequence++; }
void waitq_wake_all(struct wait_queue *queue)
{ queue->sequence++; }
