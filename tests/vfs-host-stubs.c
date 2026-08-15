/* Host-only process/credential stubs for isolated VFS and libc tests. */
#include "kern/cred.h"
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
