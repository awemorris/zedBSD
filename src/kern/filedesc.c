/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include "kern/filedesc.h"
#include "kern/file.h"
#include "kern/kmem.h"
#include "kern/poll.h"
#include "kern/record-lock.h"
#include "kern/test-checkpoint.h"

#include <errno.h>
#include <string.h>

static atomic_uint_t filedesc_live;

static void
slot_make_free(struct filedesc_entry *entry)
{
	entry->file = NULL;
	entry->flags = 0;
	entry->state = FILEDESC_SLOT_FREE;
	entry->reservation_id = 0;
}

static void
slot_make_live(struct filedesc_entry *entry, struct file *file, unsigned flags)
{
	entry->file = file;
	entry->flags = flags;
	entry->state = FILEDESC_SLOT_LIVE;
	entry->reservation_id = 0;
}

static void
slot_make_reserved(struct filedesc_entry *entry, uint64_t id)
{
	entry->file = NULL;
	entry->flags = 0;
	entry->state = FILEDESC_SLOT_RESERVED;
	entry->reservation_id = id;
}

struct filedesc *
filedesc_create(struct process *owner)
{
	struct filedesc *fd = kern_calloc(1, sizeof(*fd));
	if (fd != NULL) {
		refcount_init(&fd->refs, 1);
		spin_init(&fd->lock, LOCK_RANK_FILEDESC, "file descriptor table");
		waitq_init(&fd->reservation_waitq,
		    "file descriptor reservations");
		fd->owner = owner;
		fd->soft_limit = KERN_OPEN_MAX;
		fd->reservation_generation = 1;
		(void)atomic_fetch_add_relaxed(&filedesc_live, 1U);
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
		if (fd->entries[descriptor].state == FILEDESC_SLOT_LIVE)
			detached[count++] = fd->entries[descriptor].file;
		slot_make_free(&fd->entries[descriptor]);
	}
	spin_unlock_irqrestore(&fd->lock, irq);
	poll_notify();
	for (descriptor = 0; descriptor < count; descriptor++)
		if (detached[descriptor]->f_inode != NULL)
			record_lock_release_process_inode(fd->owner,
			    detached[descriptor]->f_inode);
	for (descriptor = 0; descriptor < count; descriptor++)
		(void)file_close(detached[descriptor]);
	(void)atomic_raw_fetch_add_relaxed(&filedesc_live.value, (unsigned)-1);
	kern_free(fd);
}

unsigned filedesc_count(void)
{ return atomic_load_acquire(&filedesc_live); }

struct file *
filedesc_get_ref(struct filedesc *fd, int descriptor)
{
	struct file *file;
	unsigned long irq;

	if (fd == NULL || descriptor < 0 || descriptor >= KERN_OPEN_MAX)
		return NULL;
	irq = spin_lock_irqsave(&fd->lock);
	file = fd->entries[descriptor].state == FILEDESC_SLOT_LIVE ?
	    fd->entries[descriptor].file : NULL;
	if (file != NULL) {
		KERN_TEST_CHECKPOINT(KERN_TEST_FD_LOOKUP_BEFORE_REF, file);
		file_ref(file);
	}
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
	    minimum >= KERN_OPEN_MAX || (flags & ~FILEDESC_FLAG_MASK) != 0)
		return EINVAL;
	irq = spin_lock_irqsave(&fd->lock);
	for (i = minimum; i < (int)fd->soft_limit; i++)
		if (fd->entries[i].state == FILEDESC_SLOT_FREE) {
			slot_make_live(&fd->entries[i], file, flags);
			*descriptor = i;
			spin_unlock_irqrestore(&fd->lock, irq);
			poll_notify();
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
	if ((unsigned)descriptor >= fd->soft_limit) {
		spin_unlock_irqrestore(&fd->lock, irq);
		return EMFILE;
	}
	if (fd->entries[descriptor].state != FILEDESC_SLOT_FREE) {
		spin_unlock_irqrestore(&fd->lock, irq);
		return EBUSY;
	}
	slot_make_live(&fd->entries[descriptor], file, 0);
	spin_unlock_irqrestore(&fd->lock, irq);
	poll_notify();
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
	if (fd->entries[descriptor].state != FILEDESC_SLOT_LIVE) {
		spin_unlock_irqrestore(&fd->lock, irq);
		return EBADF;
	}
	*result = fd->entries[descriptor].file;
	slot_make_free(&fd->entries[descriptor]);
	spin_unlock_irqrestore(&fd->lock, irq);
	poll_notify();
	if ((*result)->f_inode != NULL)
		record_lock_release_process_inode(fd->owner, (*result)->f_inode);
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
filedesc_clone(struct filedesc *source, struct process *owner,
    struct filedesc **result)
{
	struct filedesc *copy;
	unsigned long irq;
	int descriptor;

	if (source == NULL || result == NULL)
		return EINVAL;
	copy = filedesc_create(owner);
	if (copy == NULL)
		return ENOMEM;
	irq = spin_lock_irqsave(&source->lock);
	copy->soft_limit = source->soft_limit;
	for (descriptor = 0; descriptor < KERN_OPEN_MAX; descriptor++) {
		struct file *file;
		if (source->entries[descriptor].state != FILEDESC_SLOT_LIVE ||
		    (source->entries[descriptor].flags & FILEDESC_CLOFORK) != 0)
			continue;
		file = source->entries[descriptor].file;
		file_ref(file);
		slot_make_live(&copy->entries[descriptor], file,
		    source->entries[descriptor].flags);
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
	if (fd->entries[descriptor].state != FILEDESC_SLOT_LIVE) {
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
	if ((flags & ~FILEDESC_FLAG_MASK) != 0)
		return EINVAL;
	if (fd == NULL || descriptor < 0 || descriptor >= KERN_OPEN_MAX)
		return EBADF;
	irq = spin_lock_irqsave(&fd->lock);
	if (fd->entries[descriptor].state != FILEDESC_SLOT_LIVE) {
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
	    (flags & ~FILEDESC_FLAG_MASK) != 0)
		return oldfd < 0 || oldfd >= KERN_OPEN_MAX ? EBADF : EINVAL;
	irq = spin_lock_irqsave(&fd->lock);
	file = fd->entries[oldfd].state == FILEDESC_SLOT_LIVE ?
	    fd->entries[oldfd].file : NULL;
	if (file == NULL) {
		spin_unlock_irqrestore(&fd->lock, irq);
		return EBADF;
	}
	for (descriptor = minimum; descriptor < (int)fd->soft_limit; descriptor++)
		if (fd->entries[descriptor].state == FILEDESC_SLOT_FREE)
			break;
	if (descriptor == (int)fd->soft_limit) {
		spin_unlock_irqrestore(&fd->lock, irq);
		return EMFILE;
	}
	file_ref(file);
	slot_make_live(&fd->entries[descriptor], file, flags);
	*result = descriptor;
	spin_unlock_irqrestore(&fd->lock, irq);
	poll_notify();
	return 0;
}

int
filedesc_dup2(struct filedesc *fd, int oldfd, int newfd, unsigned flags,
    int reject_equal)
{
	struct file *file, *displaced;
	unsigned long irq;
	int error;

	if (fd == NULL || oldfd < 0 || oldfd >= KERN_OPEN_MAX || newfd < 0 ||
	    newfd >= KERN_OPEN_MAX || (flags & ~FILEDESC_FLAG_MASK) != 0)
		return EBADF;
	irq = spin_lock_irqsave(&fd->lock);
	for (;;) {
		uint64_t observed;

		file = fd->entries[oldfd].state == FILEDESC_SLOT_LIVE ?
		    fd->entries[oldfd].file : NULL;
		if (file == NULL) {
			spin_unlock_irqrestore(&fd->lock, irq);
			return EBADF;
		}
		if (oldfd == newfd) {
			spin_unlock_irqrestore(&fd->lock, irq);
			return reject_equal ? EINVAL : 0;
		}
		if ((unsigned)newfd >= fd->soft_limit) {
			spin_unlock_irqrestore(&fd->lock, irq);
			return EBADF;
		}
		if (fd->entries[newfd].state != FILEDESC_SLOT_RESERVED)
			break;
		observed = waitq_sequence(&fd->reservation_waitq);
		error = waitq_sleep(&fd->reservation_waitq, &fd->lock,
		    observed, 0, WAITQ_INTERRUPTIBLE);
		if (error == EAGAIN)
			continue;
		if (error != 0) {
			spin_unlock_irqrestore(&fd->lock, irq);
			return error;
		}
	}
	file_ref(file);
	displaced = fd->entries[newfd].state == FILEDESC_SLOT_LIVE ?
	    fd->entries[newfd].file : NULL;
	slot_make_live(&fd->entries[newfd], file, flags);
	spin_unlock_irqrestore(&fd->lock, irq);
	poll_notify();
	if (displaced != NULL && displaced->f_inode != NULL)
		record_lock_release_process_inode(fd->owner, displaced->f_inode);
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
	    ((first_flags | second_flags) & ~FILEDESC_FLAG_MASK) != 0)
		return EINVAL;
	irq = spin_lock_irqsave(&fd->lock);
	for (i = 0; i < (int)fd->soft_limit; i++) {
		if (fd->entries[i].state != FILEDESC_SLOT_FREE)
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
	slot_make_live(&fd->entries[a], first, first_flags);
	slot_make_live(&fd->entries[b], second, second_flags);
	result[0] = a;
	result[1] = b;
	spin_unlock_irqrestore(&fd->lock, irq);
	poll_notify();
	return 0;
}

int
filedesc_install_many(struct filedesc *fd, struct file **files,
	unsigned count, unsigned flags, int *result)
{
	struct filedesc_reservation reservation;
	int error = filedesc_reserve_many(fd, count, flags, &reservation);
	if (error != 0)
		return error;
	error = filedesc_commit_reserved(&reservation, files, result);
	if (error != 0)
		filedesc_abort_reserved(&reservation);
	return error;
}

int
filedesc_reserve_many(struct filedesc *fd, unsigned count, unsigned flags,
    struct filedesc_reservation *reservation)
{
	unsigned found = 0, index;
	unsigned long irq;
	if (fd == NULL || reservation == NULL || count > KERN_OPEN_MAX ||
	    (flags & ~FILEDESC_FLAG_MASK) != 0)
		return EINVAL;
	memset(reservation, 0, sizeof(*reservation));
	irq = spin_lock_irqsave(&fd->lock);
	for (index = 0; index < fd->soft_limit && found < count; index++)
		if (fd->entries[index].state == FILEDESC_SLOT_FREE)
			reservation->slots[found++] = (int)index;
	if (found != count) {
		spin_unlock_irqrestore(&fd->lock, irq);
		return EMFILE;
	}
	reservation->generation = ++fd->reservation_generation;
	if (reservation->generation == 0)
		reservation->generation = ++fd->reservation_generation;
	for (index = 0; index < count; index++)
		slot_make_reserved(&fd->entries[reservation->slots[index]],
		    reservation->generation);
	reservation->table = fd;
	reservation->count = count;
	reservation->flags = flags;
	reservation->active = 1;
	filedesc_ref(fd);
	spin_unlock_irqrestore(&fd->lock, irq);
	return 0;
}

int
filedesc_commit_reserved(struct filedesc_reservation *reservation,
    struct file **files, int *descriptors)
{
	struct filedesc *fd;
	unsigned index;
	unsigned long irq;
	if (reservation == NULL || !reservation->active ||
	    (reservation->count != 0 && (files == NULL || descriptors == NULL)))
		return EINVAL;
	fd = reservation->table;
	irq = spin_lock_irqsave(&fd->lock);
	for (index = 0; index < reservation->count; index++) {
		int slot = reservation->slots[index];
		if (slot < 0 || slot >= KERN_OPEN_MAX ||
		    fd->entries[slot].state != FILEDESC_SLOT_RESERVED ||
		    fd->entries[slot].reservation_id != reservation->generation) {
			spin_unlock_irqrestore(&fd->lock, irq);
			return EBUSY;
		}
	}
	for (index = 0; index < reservation->count; index++) {
		int slot = reservation->slots[index];
		slot_make_live(&fd->entries[slot], files[index],
		    reservation->flags);
		descriptors[index] = slot;
	}
	reservation->active = 0;
	waitq_wake_all(&fd->reservation_waitq);
	spin_unlock_irqrestore(&fd->lock, irq);
	filedesc_destroy(fd);
	poll_notify();
	return 0;
}

void
filedesc_abort_reserved(struct filedesc_reservation *reservation)
{
	struct filedesc *fd;
	unsigned index;
	unsigned long irq;
	if (reservation == NULL || !reservation->active)
		return;
	fd = reservation->table;
	irq = spin_lock_irqsave(&fd->lock);
	for (index = 0; index < reservation->count; index++) {
		int slot = reservation->slots[index];
		if (slot >= 0 && slot < KERN_OPEN_MAX &&
		    fd->entries[slot].state == FILEDESC_SLOT_RESERVED &&
		    fd->entries[slot].reservation_id == reservation->generation)
			slot_make_free(&fd->entries[slot]);
	}
	reservation->active = 0;
	waitq_wake_all(&fd->reservation_waitq);
	spin_unlock_irqrestore(&fd->lock, irq);
	filedesc_destroy(fd);
	poll_notify();
}

int
filedesc_set_limit(struct filedesc *fd, unsigned limit)
{
	unsigned long irq;
	if (fd == NULL || limit > KERN_OPEN_MAX)
		return EINVAL;
	irq = spin_lock_irqsave(&fd->lock);
	fd->soft_limit = limit;
	spin_unlock_irqrestore(&fd->lock, irq);
	return 0;
}

unsigned
filedesc_get_limit(struct filedesc *fd)
{
	unsigned limit;
	unsigned long irq;
	if (fd == NULL)
		return 0;
	irq = spin_lock_irqsave(&fd->lock);
	limit = fd->soft_limit;
	spin_unlock_irqrestore(&fd->lock, irq);
	return limit;
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
		if (fd->entries[descriptor].state != FILEDESC_SLOT_LIVE ||
		    (fd->entries[descriptor].flags & FILEDESC_CLOEXEC) == 0)
			continue;
		detached[count++] = fd->entries[descriptor].file;
		slot_make_free(&fd->entries[descriptor]);
	}
	spin_unlock_irqrestore(&fd->lock, irq);
	poll_notify();
	for (descriptor = 0; descriptor < count; descriptor++)
		if (detached[descriptor]->f_inode != NULL)
			record_lock_release_process_inode(fd->owner,
			    detached[descriptor]->f_inode);
	for (descriptor = 0; descriptor < count; descriptor++)
		(void)file_close(detached[descriptor]);
}
