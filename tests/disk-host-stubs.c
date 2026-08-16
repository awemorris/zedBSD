/* Host-only synchronization stubs for synchronous disk fixtures. */
#include <kern/lock.h>
#include <kern/waitq.h>
#include <hal/hal.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

bool hal_irq_disable(void) { return false; }
void hal_irq_enable(void) { }
hal_cpu_id_t hal_cpu_current(void) { return 0; }
struct thread *thread_current(void) { return NULL; }

int
hal_pmem_alloc(const struct hal_pmem_request *request, struct hal_pmem *desc)
{
	void *memory;
	if (request == NULL || desc == NULL || request->size == 0)
		return HAL_ERR_INVALID;
	memory = calloc(1, request->size);
	if (memory == NULL)
		return HAL_ERR_NOMEM;
	desc->vaddr = memory;
	desc->paddr = (hal_physaddr_t)(uintptr_t)memory;
	desc->size = request->size;
	desc->type = request->type;
	desc->attr = request->attr;
	return HAL_OK;
}

int
hal_pmem_free(struct hal_pmem *desc)
{
	if (desc == NULL || desc->vaddr == NULL)
		return HAL_ERR_INVALID;
	free(desc->vaddr);
	desc->vaddr = NULL;
	desc->size = 0;
	return HAL_OK;
}

size_t hal_pmem_get_total_size(void) { return 64U * 1024U * 1024U; }

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

int mutex_init(struct mutex *lock, enum lock_rank rank, const char *name)
{ spin_init(&lock->guard, rank, name); lock->locked = 0; return 0; }
int mutex_lock_interruptible(struct mutex *lock)
{ lock->locked = 1; return 0; }
void mutex_lock(struct mutex *lock) { lock->locked = 1; }
void mutex_unlock(struct mutex *lock) { lock->locked = 0; }

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
