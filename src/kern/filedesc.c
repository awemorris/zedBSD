/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include "kern/filedesc.h"
#include "kern/file.h"
#include "kern/kmem.h"

#include <errno.h>
#include <hal/hal.h>

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
	int descriptor;
	if (fd == NULL)
		return;
	if (fd->usecount > 1) {
		fd->usecount--;
		return;
	}
	for (descriptor = 0; descriptor < KERN_OPEN_MAX; descriptor++)
		if (fd->entries[descriptor].file != NULL)
			(void)file_close(fd->entries[descriptor].file);
	kern_free(fd);
}

struct file *
filedesc_get(struct filedesc *fd, int descriptor)
{
	if (fd == NULL || descriptor < 0 || descriptor >= KERN_OPEN_MAX)
		return NULL;
	return fd->entries[descriptor].file;
}

struct file *
filedesc_get_ref(struct filedesc *fd, int descriptor)
{
	struct file *file = filedesc_get(fd, descriptor);
	if (file != NULL)
		file_ref(file);
	return file;
}

int
filedesc_install(struct filedesc *fd, struct file *file, int *descriptor)
{
	return filedesc_install_from(fd, file, 0, 0, descriptor);
}

int
filedesc_install_from(struct filedesc *fd, struct file *file, unsigned flags,
		      int minimum, int *descriptor)
{
	int i;
	if (fd == NULL || file == NULL || descriptor == NULL || minimum < 0 ||
	    minimum >= KERN_OPEN_MAX || (flags & ~FILEDESC_CLOEXEC) != 0)
		return EINVAL;
	for (i = minimum; i < KERN_OPEN_MAX; i++)
		if (fd->entries[i].file == NULL) {
			fd->entries[i].file = file;
			fd->entries[i].flags = flags;
			*descriptor = i;
			return 0;
		}
	return EMFILE;
}

int
filedesc_install_at(struct filedesc *fd, struct file *file, int descriptor)
{
	if (fd == NULL || file == NULL || descriptor < 0 ||
	    descriptor >= KERN_OPEN_MAX)
		return EINVAL;
	if (fd->entries[descriptor].file != NULL)
		return EBUSY;
	fd->entries[descriptor].file = file;
	fd->entries[descriptor].flags = 0;
	return 0;
}

int
filedesc_take(struct filedesc *fd, int descriptor, struct file **result)
{
	if (fd == NULL || result == NULL || descriptor < 0 ||
	    descriptor >= KERN_OPEN_MAX || fd->entries[descriptor].file == NULL)
		return EBADF;
	*result = fd->entries[descriptor].file;
	fd->entries[descriptor].file = NULL;
	fd->entries[descriptor].flags = 0;
	return 0;
}

int
filedesc_close(struct filedesc *fd, int descriptor)
{
	struct file *file;
	int error = filedesc_take(fd, descriptor, &file);
	return error != 0 ? error : file_close(file);
}

int
filedesc_clone_stdio(struct filedesc *source, struct filedesc *destination)
{
	int descriptor;
	if (source == NULL || destination == NULL)
		return EINVAL;
	for (descriptor = 0; descriptor < 3; descriptor++) {
		struct file *file = filedesc_get(source, descriptor);
		int error;
		if (file == NULL)
			continue;
		file_ref(file);
		error = filedesc_install_at(destination, file, descriptor);
		if (error != 0) {
			(void)file_close(file);
			return error;
		}
	}
	return 0;
}

int
filedesc_clone(struct filedesc *source, struct filedesc **result)
{
	struct filedesc *copy;
	int descriptor;

	if (source == NULL || result == NULL)
		return EINVAL;
	copy = filedesc_create();
	if (copy == NULL)
		return ENOMEM;
	for (descriptor = 0; descriptor < KERN_OPEN_MAX; descriptor++) {
		struct file *file = source->entries[descriptor].file;
		if (file == NULL)
			continue;
		file_ref(file);
		copy->entries[descriptor].file = file;
		copy->entries[descriptor].flags = source->entries[descriptor].flags;
	}
	*result = copy;
	return 0;
}

int
filedesc_get_flags(struct filedesc *fd, int descriptor, unsigned *flags)
{
	if (fd == NULL || flags == NULL || descriptor < 0 ||
	    descriptor >= KERN_OPEN_MAX || fd->entries[descriptor].file == NULL)
		return EBADF;
	*flags = fd->entries[descriptor].flags;
	return 0;
}

int
filedesc_set_flags(struct filedesc *fd, int descriptor, unsigned flags)
{
	if ((flags & ~FILEDESC_CLOEXEC) != 0)
		return EINVAL;
	if (fd == NULL || descriptor < 0 || descriptor >= KERN_OPEN_MAX ||
	    fd->entries[descriptor].file == NULL)
		return EBADF;
	fd->entries[descriptor].flags = flags;
	return 0;
}

int
filedesc_dup(struct filedesc *fd, int oldfd, int minimum, unsigned flags,
    int *result)
{
	struct file *file;
	bool enabled;
	int error;

	if (fd == NULL || result == NULL || oldfd < 0 ||
	    oldfd >= KERN_OPEN_MAX || minimum < 0 || minimum >= KERN_OPEN_MAX ||
	    (flags & ~FILEDESC_CLOEXEC) != 0)
		return oldfd < 0 || oldfd >= KERN_OPEN_MAX ? EBADF : EINVAL;
	enabled = hal_irq_disable();
	file = fd->entries[oldfd].file;
	if (file == NULL)
		error = EBADF;
	else {
		file_ref(file);
		error = filedesc_install_from(fd, file, flags, minimum, result);
		if (error != 0)
			(void)file_close(file);
	}
	if (enabled)
		hal_irq_enable();
	return error;
}

int
filedesc_dup2(struct filedesc *fd, int oldfd, int newfd, unsigned flags,
    int reject_equal)
{
	struct file *file, *displaced = NULL;
	bool enabled;

	if (fd == NULL || oldfd < 0 || oldfd >= KERN_OPEN_MAX || newfd < 0 ||
	    newfd >= KERN_OPEN_MAX || (flags & ~FILEDESC_CLOEXEC) != 0)
		return EBADF;
	enabled = hal_irq_disable();
	file = fd->entries[oldfd].file;
	if (file == NULL) {
		if (enabled)
			hal_irq_enable();
		return EBADF;
	}
	if (oldfd == newfd) {
		if (enabled)
			hal_irq_enable();
		return reject_equal ? EINVAL : 0;
	}
	file_ref(file);
	displaced = fd->entries[newfd].file;
	fd->entries[newfd].file = file;
	fd->entries[newfd].flags = flags;
	if (enabled)
		hal_irq_enable();
	if (displaced != NULL)
		(void)file_close(displaced);
	return 0;
}

int
filedesc_install_pair(struct filedesc *fd, struct file *first,
    unsigned first_flags, struct file *second, unsigned second_flags,
    int result[2])
{
	bool enabled;
	int a = -1, b = -1, i;

	if (fd == NULL || first == NULL || second == NULL || result == NULL ||
	    ((first_flags | second_flags) & ~FILEDESC_CLOEXEC) != 0)
		return EINVAL;
	enabled = hal_irq_disable();
	for (i = 0; i < KERN_OPEN_MAX; i++) {
		if (fd->entries[i].file != NULL)
			continue;
		if (a < 0)
			a = i;
		else {
			b = i;
			break;
		}
	}
	if (b < 0) {
		if (enabled)
			hal_irq_enable();
		return EMFILE;
	}
	fd->entries[a].file = first;
	fd->entries[a].flags = first_flags;
	fd->entries[b].file = second;
	fd->entries[b].flags = second_flags;
	result[0] = a;
	result[1] = b;
	if (enabled)
		hal_irq_enable();
	return 0;
}

void
filedesc_close_on_exec(struct filedesc *fd)
{
	int descriptor;
	if (fd == NULL)
		return;
	for (descriptor = 0; descriptor < KERN_OPEN_MAX; descriptor++)
		if (fd->entries[descriptor].file != NULL &&
		    (fd->entries[descriptor].flags & FILEDESC_CLOEXEC) != 0)
			(void)filedesc_close(fd, descriptor);
}
