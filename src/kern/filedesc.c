/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include "kern/filedesc.h"
#include "kern/file.h"
#include "kern/kmem.h"

#include <errno.h>

struct filedesc *
filedesc_create(void)
{
	struct filedesc *fd = kern_calloc(1, sizeof(*fd));
	if (fd != NULL)
		fd->usecount = 1;
	return fd;
}

void
filedesc_destroy(struct filedesc *fd)
{
	int i;
	if (fd == NULL)
		return;
	if (fd->usecount > 1) {
		fd->usecount--;
		return;
	}
	for (i = 0; i < KERN_OPEN_MAX; i++)
		if (fd->files[i] != NULL)
			(void)file_close(fd->files[i]);
	kern_free(fd);
}

struct file *
filedesc_get(struct filedesc *fd, int descriptor)
{
	if (fd == NULL || descriptor < 0 || descriptor >= KERN_OPEN_MAX)
		return NULL;
	return fd->files[descriptor];
}

int
filedesc_install(struct filedesc *fd, struct file *file, int *descriptor)
{
	int i;
	if (fd == NULL || file == NULL || descriptor == NULL)
		return EINVAL;
	for (i = 0; i < KERN_OPEN_MAX; i++)
		if (fd->files[i] == NULL) {
			fd->files[i] = file;
			*descriptor = i;
			return 0;
		}
	return ENOSPC;
}
