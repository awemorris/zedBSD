/* Host-only process/credential stubs for isolated VFS and libc tests. */
#include "kern/cred.h"
#include "kern/lock.h"
#include <hal/hal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
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
