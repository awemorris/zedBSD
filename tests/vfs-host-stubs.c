/* Host-only process/credential stubs for isolated VFS and libc tests. */
#include "kern/cred.h"
#include "kern/file.h"
#include "kern/lock.h"
#include <hal/hal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

/* Isolated host VFS tests are single-threaded and have no HAL interrupt
 * controller.  Keep the kernel's IRQ-save critical-section contract visible
 * without pulling an architecture HAL into the host executable. */
bool
hal_irq_disable(void)
{
	return false;
}

void
hal_irq_enable(void)
{
}

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
{ lock->held.value = 0; lock->rank = rank; lock->name = name; }
unsigned long spin_lock_irqsave(struct spinlock *lock)
{ (void)lock; return 0; }
void spin_unlock_irqrestore(struct spinlock *lock, unsigned long irq)
{ (void)lock; (void)irq; }
int mutex_init(struct mutex *lock, enum lock_rank rank, const char *name)
{ (void)lock; (void)rank; (void)name; return 0; }
void mutex_lock(struct mutex *lock) { (void)lock; }
void mutex_unlock(struct mutex *lock) { (void)lock; }

void waitq_init(struct wait_queue *queue, const char *name)
{ queue->head = queue->tail = NULL; queue->sequence = 1; queue->name = name; }
uint64_t waitq_sequence(const struct wait_queue *queue)
{ return queue->sequence; }
int waitq_sleep(struct wait_queue *queue, struct spinlock *lock,
    uint64_t sequence, uint64_t deadline, unsigned flags)
{ (void)queue; (void)lock; (void)sequence; (void)deadline; (void)flags; return 0; }
void waitq_wake_one(struct wait_queue *queue) { queue->sequence++; }
void waitq_wake_all(struct wait_queue *queue) { queue->sequence++; }

void
zedbsd_clock_realtime(time_t *seconds, long *nanoseconds)
{
	if (seconds != NULL)
		*seconds = 1;
	if (nanoseconds != NULL)
		*nanoseconds = 0;
}

const struct ucred *
cred_current(void)
{
	return NULL;
}

int
vfs_access(const struct inode *inode, const struct ucred *cred, int requested)
{
	(void)inode;
	(void)cred;
	(void)requested;
	return 0;
}

void record_lock_inode_destroy(struct inode *inode) { (void)inode; }
const struct file_ops fifo_file_ops = { 0 };
