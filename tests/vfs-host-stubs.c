/* Host-only process/credential stubs for isolated VFS and libc tests. */
#include "kern/cred.h"
#include <stddef.h>
#include <stdint.h>

void
zedbsd_clock_realtime(int32_t *seconds, int32_t *nanoseconds)
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
