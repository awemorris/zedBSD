/* Host-only synchronization stubs for synchronous disk fixtures. */
#include <kern/lock.h>
#include <kern/waitq.h>
#include <hal/hal.h>

#include <errno.h>

bool hal_irq_disable(void) { return false; }
void hal_irq_enable(void) { }
hal_cpu_id_t hal_cpu_current(void) { return 0; }
struct thread *thread_current(void) { return NULL; }

void spin_init(struct spinlock *lock, enum lock_rank rank, const char *name)
{
	lock->held.value = 0;
	lock->rank = rank;
	lock->name = name;
	lock->owner_cpu = 0;
	lock->owner_valid = 0;
}

unsigned long spin_lock_irqsave(struct spinlock *lock)
{
	while (!atomic_try_acquire_zero(&lock->held))
		hal_compiler_barrier();
	return 0;
}

void spin_unlock_irqrestore(struct spinlock *lock, unsigned long irq)
{
	(void)irq;
	atomic_store_release(&lock->held, 0);
}

void waitq_init(struct wait_queue *queue, const char *name)
{
	queue->head = queue->tail = NULL;
	queue->sequence = 1;
	queue->name = name;
}

uint64_t waitq_sequence(const struct wait_queue *queue)
{
	return queue->sequence;
}

int waitq_sleep(struct wait_queue *queue, struct spinlock *lock,
	uint64_t sequence, uint64_t deadline, unsigned flags)
{
	(void)queue;
	(void)lock;
	(void)sequence;
	(void)deadline;
	(void)flags;
	return EINVAL;
}

void waitq_wake_one(struct wait_queue *queue)
{
	queue->sequence++;
}

void waitq_wake_all(struct wait_queue *queue)
{
	queue->sequence++;
}
