/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include "kern/filedesc.h"
#include "kern/file.h"
#include "kern/kmem.h"

#include <errno.h>

struct filedesc *
filedesc_create(void)
{
	struct filedesc *fd = kern_calloc(1, sizeof(*fd));
	if (fd != NULL) {
		refcount_init(&fd->refs, 1);
		spin_init(&fd->lock, LOCK_RANK_FILEDESC, "file descriptor table");
	}
	return fd;
}

void
filedesc_ref(struct filedesc *fd)
{
	if (fd != NULL)
		refcount_get(&fd->refs);
}

void
filedesc_destroy(struct filedesc *fd)
{
	struct file *detached[KERN_OPEN_MAX];
	unsigned long irq;
	int descriptor, count = 0;

	if (fd == NULL || !refcount_put(&fd->refs))
		return;
	irq = spin_lock_irqsave(&fd->lock);
	for (descriptor = 0; descriptor < KERN_OPEN_MAX; descriptor++) {
		if (fd->entries[descriptor].file != NULL)
			detached[count++] = fd->entries[descriptor].file;
		fd->entries[descriptor].file = NULL;
		fd->entries[descriptor].flags = 0;
	}
	spin_unlock_irqrestore(&fd->lock, irq);
	for (descriptor = 0; descriptor < count; descriptor++)
		(void)file_close(detached[descriptor]);
	kern_free(fd);
}

struct file *
filedesc_get_ref(struct filedesc *fd, int descriptor)
{
	struct file *file;
	unsigned long irq;

	if (fd == NULL || descriptor < 0 || descriptor >= KERN_OPEN_MAX)
		return NULL;
	irq = spin_lock_irqsave(&fd->lock);
	file = fd->entries[descriptor].file;
	if (file != NULL)
		file_ref(file);
	spin_unlock_irqrestore(&fd->lock, irq);
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
	unsigned long irq;
	int i;

	if (fd == NULL || file == NULL || descriptor == NULL || minimum < 0 ||
	    minimum >= KERN_OPEN_MAX || (flags & ~FILEDESC_CLOEXEC) != 0)
		return EINVAL;
	irq = spin_lock_irqsave(&fd->lock);
	for (i = minimum; i < KERN_OPEN_MAX; i++)
		if (fd->entries[i].file == NULL) {
			fd->entries[i].file = file;
			fd->entries[i].flags = flags;
			*descriptor = i;
			spin_unlock_irqrestore(&fd->lock, irq);
			return 0;
		}
	spin_unlock_irqrestore(&fd->lock, irq);
	return EMFILE;
}

int
filedesc_install_at(struct filedesc *fd, struct file *file, int descriptor)
{
	unsigned long irq;
	if (fd == NULL || file == NULL || descriptor < 0 ||
	    descriptor >= KERN_OPEN_MAX)
		return EINVAL;
	irq = spin_lock_irqsave(&fd->lock);
	if (fd->entries[descriptor].file != NULL) {
		spin_unlock_irqrestore(&fd->lock, irq);
		return EBUSY;
	}
	fd->entries[descriptor].file = file;
	fd->entries[descriptor].flags = 0;
	spin_unlock_irqrestore(&fd->lock, irq);
	return 0;
}

int
filedesc_take(struct filedesc *fd, int descriptor, struct file **result)
{
	unsigned long irq;
	if (fd == NULL || result == NULL || descriptor < 0 ||
	    descriptor >= KERN_OPEN_MAX)
		return EBADF;
	irq = spin_lock_irqsave(&fd->lock);
	if (fd->entries[descriptor].file == NULL) {
		spin_unlock_irqrestore(&fd->lock, irq);
		return EBADF;
	}
	*result = fd->entries[descriptor].file;
	fd->entries[descriptor].file = NULL;
	fd->entries[descriptor].flags = 0;
	spin_unlock_irqrestore(&fd->lock, irq);
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
		struct file *file = filedesc_get_ref(source, descriptor);
		int error;
		if (file == NULL)
			continue;
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
	unsigned long irq;
	int descriptor;

	if (source == NULL || result == NULL)
		return EINVAL;
	copy = filedesc_create();
	if (copy == NULL)
		return ENOMEM;
	irq = spin_lock_irqsave(&source->lock);
	for (descriptor = 0; descriptor < KERN_OPEN_MAX; descriptor++) {
		struct file *file = source->entries[descriptor].file;
		if (file == NULL)
			continue;
		file_ref(file);
		copy->entries[descriptor] = source->entries[descriptor];
	}
	spin_unlock_irqrestore(&source->lock, irq);
	*result = copy;
	return 0;
}

int
filedesc_get_flags(struct filedesc *fd, int descriptor, unsigned *flags)
{
	unsigned long irq;
	if (fd == NULL || flags == NULL || descriptor < 0 ||
	    descriptor >= KERN_OPEN_MAX)
		return EBADF;
	irq = spin_lock_irqsave(&fd->lock);
	if (fd->entries[descriptor].file == NULL) {
		spin_unlock_irqrestore(&fd->lock, irq);
		return EBADF;
	}
	*flags = fd->entries[descriptor].flags;
	spin_unlock_irqrestore(&fd->lock, irq);
	return 0;
}

int
filedesc_set_flags(struct filedesc *fd, int descriptor, unsigned flags)
{
	unsigned long irq;
	if ((flags & ~FILEDESC_CLOEXEC) != 0)
		return EINVAL;
	if (fd == NULL || descriptor < 0 || descriptor >= KERN_OPEN_MAX)
		return EBADF;
	irq = spin_lock_irqsave(&fd->lock);
	if (fd->entries[descriptor].file == NULL) {
		spin_unlock_irqrestore(&fd->lock, irq);
		return EBADF;
	}
	fd->entries[descriptor].flags = flags;
	spin_unlock_irqrestore(&fd->lock, irq);
	return 0;
}

int
filedesc_dup(struct filedesc *fd, int oldfd, int minimum, unsigned flags,
    int *result)
{
	struct file *file;
	unsigned long irq;
	int descriptor;

	if (fd == NULL || result == NULL || oldfd < 0 ||
	    oldfd >= KERN_OPEN_MAX || minimum < 0 || minimum >= KERN_OPEN_MAX ||
	    (flags & ~FILEDESC_CLOEXEC) != 0)
		return oldfd < 0 || oldfd >= KERN_OPEN_MAX ? EBADF : EINVAL;
	irq = spin_lock_irqsave(&fd->lock);
	file = fd->entries[oldfd].file;
	if (file == NULL) {
		spin_unlock_irqrestore(&fd->lock, irq);
		return EBADF;
	}
	for (descriptor = minimum; descriptor < KERN_OPEN_MAX; descriptor++)
		if (fd->entries[descriptor].file == NULL)
			break;
	if (descriptor == KERN_OPEN_MAX) {
		spin_unlock_irqrestore(&fd->lock, irq);
		return EMFILE;
	}
	file_ref(file);
	fd->entries[descriptor].file = file;
	fd->entries[descriptor].flags = flags;
	*result = descriptor;
	spin_unlock_irqrestore(&fd->lock, irq);
	return 0;
}

int
filedesc_dup2(struct filedesc *fd, int oldfd, int newfd, unsigned flags,
    int reject_equal)
{
	struct file *file, *displaced;
	unsigned long irq;

	if (fd == NULL || oldfd < 0 || oldfd >= KERN_OPEN_MAX || newfd < 0 ||
	    newfd >= KERN_OPEN_MAX || (flags & ~FILEDESC_CLOEXEC) != 0)
		return EBADF;
	irq = spin_lock_irqsave(&fd->lock);
	file = fd->entries[oldfd].file;
	if (file == NULL) {
		spin_unlock_irqrestore(&fd->lock, irq);
		return EBADF;
	}
	if (oldfd == newfd) {
		spin_unlock_irqrestore(&fd->lock, irq);
		return reject_equal ? EINVAL : 0;
	}
	file_ref(file);
	displaced = fd->entries[newfd].file;
	fd->entries[newfd].file = file;
	fd->entries[newfd].flags = flags;
	spin_unlock_irqrestore(&fd->lock, irq);
	if (displaced != NULL)
		(void)file_close(displaced);
	return 0;
}

int
filedesc_install_pair(struct filedesc *fd, struct file *first,
    unsigned first_flags, struct file *second, unsigned second_flags,
    int result[2])
{
	unsigned long irq;
	int a = -1, b = -1, i;

	if (fd == NULL || first == NULL || second == NULL || result == NULL ||
	    ((first_flags | second_flags) & ~FILEDESC_CLOEXEC) != 0)
		return EINVAL;
	irq = spin_lock_irqsave(&fd->lock);
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
		spin_unlock_irqrestore(&fd->lock, irq);
		return EMFILE;
	}
	fd->entries[a].file = first;
	fd->entries[a].flags = first_flags;
	fd->entries[b].file = second;
	fd->entries[b].flags = second_flags;
	result[0] = a;
	result[1] = b;
	spin_unlock_irqrestore(&fd->lock, irq);
	return 0;
}

void
filedesc_close_on_exec(struct filedesc *fd)
{
	struct file *detached[KERN_OPEN_MAX];
	unsigned long irq;
	int descriptor, count = 0;

	if (fd == NULL)
		return;
	irq = spin_lock_irqsave(&fd->lock);
	for (descriptor = 0; descriptor < KERN_OPEN_MAX; descriptor++) {
		if (fd->entries[descriptor].file == NULL ||
		    (fd->entries[descriptor].flags & FILEDESC_CLOEXEC) == 0)
			continue;
		detached[count++] = fd->entries[descriptor].file;
		fd->entries[descriptor].file = NULL;
		fd->entries[descriptor].flags = 0;
	}
	spin_unlock_irqrestore(&fd->lock, irq);
	for (descriptor = 0; descriptor < count; descriptor++)
		(void)file_close(detached[descriptor]);
}
