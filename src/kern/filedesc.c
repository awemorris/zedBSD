/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Per-process file descriptor tables.
 *
 * A table is a fixed array of slots that are free, live with a file and
 * its descriptor flags, or reserved for a multi-descriptor operation.
 * Reservations let a socketpair or pipe claim its descriptors atomically
 * while dup2() waits for a reserved slot to settle.  Detaching a file
 * also releases the process's record locks on it.
 */

#include "kern/filedesc.h"
#include "kern/file.h"
#include "kern/kmem.h"
#include "kern/poll.h"
#include "kern/record-lock.h"
#include "kern/test-checkpoint.h"

#include <errno.h>
#include <string.h>

extern void file_readahead_invalidate(struct file *) __attribute__((weak));

static atomic_uint_t filedesc_live;

static void slot_make_free(struct filedesc_entry *entry);
static void slot_make_live(struct filedesc_entry *entry, struct file *file, unsigned flags);
static void slot_make_reserved(struct filedesc_entry *entry, uint64_t id);

/*
 * Creates an empty descriptor table for a process.
 */
struct filedesc *
filedesc_create(
	struct process *owner)
{
	struct filedesc *fd;

	/* Allocates the table with every slot free and the full limit. */
	fd = kern_calloc(1, sizeof(*fd));
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

	/* Reports the table, or none. */
	return fd;
}

/*
 * Takes a reference on a descriptor table.
 */
void
filedesc_ref(
	struct filedesc *fd)
{
	/* Ignores a missing table. */
	if (fd != NULL)
		refcount_get(&fd->refs);
}

/*
 * Drops a reference on a descriptor table and closes every file with the last.
 */
void
filedesc_destroy(
	struct filedesc *fd)
{
	struct file *detached[KERN_OPEN_MAX];
	unsigned long irq;
	int descriptor;
	int count;

	count = 0;

	/* Only the last reference destroys. */
	if (fd == NULL || !refcount_put(&fd->refs))
		return;

	/* Detaches every live file under the lock. */
	irq = spin_lock_irqsave(&fd->lock);

	for (descriptor = 0; descriptor < KERN_OPEN_MAX; descriptor++) {
		if (fd->entries[descriptor].state == FILEDESC_SLOT_LIVE)
			detached[count++] = fd->entries[descriptor].file;
		slot_make_free(&fd->entries[descriptor]);
	}

	spin_unlock_irqrestore(&fd->lock, irq);

	poll_notify();

	/* Releases the record locks, then closes the files. */
	for (descriptor = 0; descriptor < count; descriptor++) {
		if (detached[descriptor]->f_inode != NULL)
			record_lock_release_process_inode(fd->owner,
			    detached[descriptor]->f_inode);
	}

	for (descriptor = 0; descriptor < count; descriptor++) {
		if (file_readahead_invalidate != NULL)
			file_readahead_invalidate(detached[descriptor]);
		(void)file_close(detached[descriptor]);
	}

	(void)atomic_raw_fetch_add_relaxed(&filedesc_live.value, (unsigned)-1);
	kern_free(fd);
}

/*
 * Reports the number of live descriptor tables.
 */
unsigned
filedesc_count(
	void)
{
	unsigned count;

	count = atomic_load_acquire(&filedesc_live);

	/* Reports the sampled count. */
	return count;
}

/*
 * References the file behind a descriptor, or none.
 */
struct file *
filedesc_get_ref(
	struct filedesc *fd,
	int descriptor)
{
	struct file *file;
	unsigned long irq;

	/* Rejects a missing table or a descriptor out of range. */
	if (fd == NULL || descriptor < 0 || descriptor >= KERN_OPEN_MAX)
		return NULL;

	/* References the file of a live slot under the lock. */
	irq = spin_lock_irqsave(&fd->lock);

	if (fd->entries[descriptor].state == FILEDESC_SLOT_LIVE)
		file = fd->entries[descriptor].file;
	else
		file = NULL;
	if (file != NULL) {
		KERN_TEST_CHECKPOINT(KERN_TEST_FD_LOOKUP_BEFORE_REF, file);
		file_ref(file);
	}

	spin_unlock_irqrestore(&fd->lock, irq);

	/* Reports the referenced file, or none. */
	return file;
}

/*
 * Installs a file in the lowest free descriptor.
 */
int
filedesc_install(
	struct filedesc *fd,
	struct file *file,
	int *descriptor)
{
	int error;

	/* Reports why the installation failed. */
	error = filedesc_install_from(fd, file, 0, 0, descriptor);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Installs a file in the lowest free descriptor at or above a minimum.
 */
int
filedesc_install_from(
	struct filedesc *fd,
	struct file *file,
	unsigned flags,
	int minimum,
	int *descriptor)
{
	unsigned long irq;
	int i;

	/* Rejects a missing operand, a bad minimum, or unknown flags. */
	if (fd == NULL ||
	    file == NULL ||
	    descriptor == NULL ||
	    minimum < 0 ||
	    minimum >= KERN_OPEN_MAX ||
	    (flags & ~FILEDESC_FLAG_MASK) != 0)
		return EINVAL;

	/* Takes the first free slot under the soft limit. */
	irq = spin_lock_irqsave(&fd->lock);

	for (i = minimum; i < (int)fd->soft_limit; i++) {
		if (fd->entries[i].state == FILEDESC_SLOT_FREE) {
			slot_make_live(&fd->entries[i], file, flags);
			*descriptor = i;
			spin_unlock_irqrestore(&fd->lock, irq);
			poll_notify();
			return 0;
		}
	}

	spin_unlock_irqrestore(&fd->lock, irq);

	/* Reports a full table. */
	return EMFILE;
}

/*
 * Installs a file in a specific free descriptor.
 */
int
filedesc_install_at(
	struct filedesc *fd,
	struct file *file,
	int descriptor)
{
	unsigned long irq;

	/* Rejects a missing operand or a descriptor out of range. */
	if (fd == NULL ||
	    file == NULL ||
	    descriptor < 0 ||
	    descriptor >= KERN_OPEN_MAX)
		return EINVAL;

	/* The slot must be under the limit and free. */
	irq = spin_lock_irqsave(&fd->lock);

	if ((unsigned)descriptor >= fd->soft_limit) {
		spin_unlock_irqrestore(&fd->lock, irq);
		return EMFILE;
	}

	if (fd->entries[descriptor].state != FILEDESC_SLOT_FREE) {
		spin_unlock_irqrestore(&fd->lock, irq);
		return EBUSY;
	}

	/* Fills the slot. */
	slot_make_live(&fd->entries[descriptor], file, 0);

	spin_unlock_irqrestore(&fd->lock, irq);

	poll_notify();

	/* Reports the installed file. */
	return 0;
}

/*
 * Detaches the file behind a descriptor, handing its reference to the caller.
 */
int
filedesc_take(
	struct filedesc *fd,
	int descriptor,
	struct file **result)
{
	unsigned long irq;

	/* Rejects a missing operand or a descriptor out of range. */
	if (fd == NULL ||
	    result == NULL ||
	    descriptor < 0 ||
	    descriptor >= KERN_OPEN_MAX)
		return EBADF;

	/* Frees a live slot, keeping its file. */
	irq = spin_lock_irqsave(&fd->lock);

	if (fd->entries[descriptor].state != FILEDESC_SLOT_LIVE) {
		spin_unlock_irqrestore(&fd->lock, irq);
		return EBADF;
	}

	*result = fd->entries[descriptor].file;
	slot_make_free(&fd->entries[descriptor]);

	spin_unlock_irqrestore(&fd->lock, irq);

	poll_notify();

	/* Releases the process's record locks on the file. */
	if ((*result)->f_inode != NULL)
		record_lock_release_process_inode(fd->owner, (*result)->f_inode);

	/* Invalidates descriptor work separately from temporary syscall reference drops. */
	if (file_readahead_invalidate != NULL)
		file_readahead_invalidate(*result);

	/* Reports the detached file. */
	return 0;
}

/*
 * Closes a descriptor.
 */
int
filedesc_close(
	struct filedesc *fd,
	int descriptor)
{
	struct file *file;
	int error;

	/* Detaches the file, then closes it. */
	error = filedesc_take(fd, descriptor, &file);
	if (error != 0)
		return error;

	/* Reports why the close failed. */
	error = file_close(file);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Copies the standard descriptors of one table into another.
 */
int
filedesc_clone_stdio(
	struct filedesc *source,
	struct filedesc *destination)
{
	struct file *file;
	int descriptor;
	int error;

	/* Rejects a missing table. */
	if (source == NULL || destination == NULL)
		return EINVAL;

	/* Installs each open standard descriptor at the same number. */
	for (descriptor = 0; descriptor < 3; descriptor++) {
		file = filedesc_get_ref(source, descriptor);
		if (file == NULL)
			continue;
		error = filedesc_install_at(destination, file, descriptor);
		if (error != 0) {
			(void)file_close(file);
			return error;
		}
	}

	/* Reports the copied descriptors. */
	return 0;
}

/*
 * Copies a descriptor table for a forked process.
 *
 * Descriptors marked close-on-fork are left out.
 */
int
filedesc_clone(
	struct filedesc *source,
	struct process *owner,
	struct filedesc **result)
{
	struct filedesc *copy;
	struct file *file;
	unsigned long irq;
	int descriptor;

	/* Rejects a missing source or result. */
	if (source == NULL || result == NULL)
		return EINVAL;

	/* Creates the table for the new owner. */
	copy = filedesc_create(owner);
	if (copy == NULL)
		return ENOMEM;

	/* References every inherited file into the same slot. */
	irq = spin_lock_irqsave(&source->lock);

	copy->soft_limit = source->soft_limit;
	for (descriptor = 0; descriptor < KERN_OPEN_MAX; descriptor++) {
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

	/* Reports the copied table. */
	return 0;
}

/*
 * Reads the descriptor flags of a live descriptor.
 */
int
filedesc_get_flags(
	struct filedesc *fd,
	int descriptor,
	unsigned *flags)
{
	unsigned long irq;

	/* Rejects a missing operand or a descriptor out of range. */
	if (fd == NULL ||
	    flags == NULL ||
	    descriptor < 0 ||
	    descriptor >= KERN_OPEN_MAX)
		return EBADF;

	/* Reads the flags of a live slot. */
	irq = spin_lock_irqsave(&fd->lock);

	if (fd->entries[descriptor].state != FILEDESC_SLOT_LIVE) {
		spin_unlock_irqrestore(&fd->lock, irq);
		return EBADF;
	}

	*flags = fd->entries[descriptor].flags;

	spin_unlock_irqrestore(&fd->lock, irq);

	/* Reports the read flags. */
	return 0;
}

/*
 * Sets the descriptor flags of a live descriptor.
 */
int
filedesc_set_flags(
	struct filedesc *fd,
	int descriptor,
	unsigned flags)
{
	unsigned long irq;

	/* Rejects unknown flags, a missing table, or a descriptor out of range. */
	if ((flags & ~FILEDESC_FLAG_MASK) != 0)
		return EINVAL;
	if (fd == NULL || descriptor < 0 || descriptor >= KERN_OPEN_MAX)
		return EBADF;

	/* Sets the flags of a live slot. */
	irq = spin_lock_irqsave(&fd->lock);

	if (fd->entries[descriptor].state != FILEDESC_SLOT_LIVE) {
		spin_unlock_irqrestore(&fd->lock, irq);
		return EBADF;
	}

	fd->entries[descriptor].flags = flags;

	spin_unlock_irqrestore(&fd->lock, irq);

	/* Reports the set flags. */
	return 0;
}

/*
 * Duplicates a descriptor into the lowest free slot at or above a minimum.
 */
int
filedesc_dup(
	struct filedesc *fd,
	int oldfd,
	int minimum,
	unsigned flags,
	int *result)
{
	struct file *file;
	unsigned long irq;
	int descriptor;

	/* Rejects a descriptor out of range, then any other bad operand. */
	if (oldfd < 0 || oldfd >= KERN_OPEN_MAX)
		return EBADF;
	if (fd == NULL ||
	    result == NULL ||
	    minimum < 0 ||
	    minimum >= KERN_OPEN_MAX ||
	    (flags & ~FILEDESC_FLAG_MASK) != 0)
		return EINVAL;

	/* The source must be live. */
	irq = spin_lock_irqsave(&fd->lock);

	if (fd->entries[oldfd].state == FILEDESC_SLOT_LIVE)
		file = fd->entries[oldfd].file;
	else
		file = NULL;
	if (file == NULL) {
		spin_unlock_irqrestore(&fd->lock, irq);
		return EBADF;
	}

	/* Finds the first free slot under the limit. */
	for (descriptor = minimum; descriptor < (int)fd->soft_limit; descriptor++) {
		if (fd->entries[descriptor].state == FILEDESC_SLOT_FREE)
			break;
	}

	if (descriptor == (int)fd->soft_limit) {
		spin_unlock_irqrestore(&fd->lock, irq);
		return EMFILE;
	}

	/* Shares the file with the new slot. */
	file_ref(file);
	slot_make_live(&fd->entries[descriptor], file, flags);
	*result = descriptor;

	spin_unlock_irqrestore(&fd->lock, irq);

	poll_notify();

	/* Reports the duplicated descriptor. */
	return 0;
}

/*
 * Duplicates a descriptor onto a specific descriptor, closing what was there.
 *
 * A reserved target is waited for.  Duplicating a descriptor onto itself
 * does nothing, or fails with EINVAL when reject_equal is set.
 */
int
filedesc_dup2(
	struct filedesc *fd,
	int oldfd,
	int newfd,
	unsigned flags,
	int reject_equal)
{
	struct file *file;
	struct file *displaced;
	uint64_t observed;
	unsigned long irq;
	int error;

	/* Rejects a missing table, a descriptor out of range, or unknown flags. */
	if (fd == NULL ||
	    oldfd < 0 ||
	    oldfd >= KERN_OPEN_MAX ||
	    newfd < 0 ||
	    newfd >= KERN_OPEN_MAX ||
	    (flags & ~FILEDESC_FLAG_MASK) != 0)
		return EBADF;

	/* Re-checks the source and waits while the target is reserved. */
	irq = spin_lock_irqsave(&fd->lock);

	for (;;) {
		if (fd->entries[oldfd].state == FILEDESC_SLOT_LIVE)
			file = fd->entries[oldfd].file;
		else
			file = NULL;
		if (file == NULL) {
			spin_unlock_irqrestore(&fd->lock, irq);
			return EBADF;
		}

		if (oldfd == newfd) {
			spin_unlock_irqrestore(&fd->lock, irq);
			if (reject_equal)
				return EINVAL;
			return 0;
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

	/* Replaces the target, remembering the file it displaces. */
	file_ref(file);
	if (fd->entries[newfd].state == FILEDESC_SLOT_LIVE)
		displaced = fd->entries[newfd].file;
	else
		displaced = NULL;
	slot_make_live(&fd->entries[newfd], file, flags);

	spin_unlock_irqrestore(&fd->lock, irq);

	poll_notify();

	/* Closes the displaced file after releasing its record locks. */
	if (displaced != NULL && displaced->f_inode != NULL)
		record_lock_release_process_inode(fd->owner, displaced->f_inode);
	if (displaced != NULL) {
		if (file_readahead_invalidate != NULL)
			file_readahead_invalidate(displaced);
		(void)file_close(displaced);
	}

	/* Reports the duplicated descriptor. */
	return 0;
}

/*
 * Installs two files in the two lowest free descriptors atomically.
 */
int
filedesc_install_pair(
	struct filedesc *fd,
	struct file *first,
	unsigned first_flags,
	struct file *second,
	unsigned second_flags,
	int result[2])
{
	unsigned long irq;
	int first_slot;
	int second_slot;
	int i;

	first_slot = -1;
	second_slot = -1;

	/* Rejects a missing operand or unknown flags. */
	if (fd == NULL ||
	    first == NULL ||
	    second == NULL ||
	    result == NULL ||
	    ((first_flags | second_flags) & ~FILEDESC_FLAG_MASK) != 0)
		return EINVAL;

	/* Finds the two lowest free slots under the limit. */
	irq = spin_lock_irqsave(&fd->lock);

	for (i = 0; i < (int)fd->soft_limit; i++) {
		if (fd->entries[i].state != FILEDESC_SLOT_FREE)
			continue;
		if (first_slot < 0) {
			first_slot = i;
		} else {
			second_slot = i;
			break;
		}
	}

	if (second_slot < 0) {
		spin_unlock_irqrestore(&fd->lock, irq);
		return EMFILE;
	}

	/* Fills both slots. */
	slot_make_live(&fd->entries[first_slot], first, first_flags);
	slot_make_live(&fd->entries[second_slot], second, second_flags);
	result[0] = first_slot;
	result[1] = second_slot;

	spin_unlock_irqrestore(&fd->lock, irq);

	poll_notify();

	/* Reports the installed pair. */
	return 0;
}

/*
 * Installs several files in the lowest free descriptors atomically.
 */
int
filedesc_install_many(
	struct filedesc *fd,
	struct file **files,
	unsigned count,
	unsigned flags,
	int *result)
{
	struct filedesc_reservation reservation;
	int error;

	/* Reserves the descriptors, then commits the files into them. */
	error = filedesc_reserve_many(fd, count, flags, &reservation);
	if (error != 0)
		return error;
	error = filedesc_commit_reserved(&reservation, files, result);
	if (error != 0)
		filedesc_abort_reserved(&reservation);

	/* Reports why the installation failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Reserves the lowest free descriptors for a later commit.
 *
 * The reservation holds a table reference and a generation that the
 * commit or abort must present.
 */
int
filedesc_reserve_many(
	struct filedesc *fd,
	unsigned count,
	unsigned flags,
	struct filedesc_reservation *reservation)
{
	unsigned found;
	unsigned index;
	unsigned long irq;

	found = 0;

	/* Rejects a missing operand, too many descriptors, or unknown flags. */
	if (fd == NULL ||
	    reservation == NULL ||
	    count > KERN_OPEN_MAX ||
	    (flags & ~FILEDESC_FLAG_MASK) != 0)
		return EINVAL;
	memset(reservation, 0, sizeof(*reservation));

	/* Collects the lowest free slots under the limit. */
	irq = spin_lock_irqsave(&fd->lock);

	for (index = 0; index < fd->soft_limit && found < count; index++) {
		if (fd->entries[index].state == FILEDESC_SLOT_FREE)
			reservation->slots[found++] = (int)index;
	}

	if (found != count) {
		spin_unlock_irqrestore(&fd->lock, irq);
		return EMFILE;
	}

	/* Marks the slots reserved under a new non-zero generation. */
	fd->reservation_generation++;
	reservation->generation = fd->reservation_generation;
	if (reservation->generation == 0) {
		fd->reservation_generation++;
		reservation->generation = fd->reservation_generation;
	}

	for (index = 0; index < count; index++)
		slot_make_reserved(&fd->entries[reservation->slots[index]],
		    reservation->generation);

	/* Describes the reservation and holds the table for it. */
	reservation->table = fd;
	reservation->count = count;
	reservation->flags = flags;
	reservation->active = 1;
	filedesc_ref(fd);

	spin_unlock_irqrestore(&fd->lock, irq);

	/* Reports the reservation. */
	return 0;
}

/*
 * Installs files into reserved descriptors.
 */
int
filedesc_commit_reserved(
	struct filedesc_reservation *reservation,
	struct file **files,
	int *descriptors)
{
	struct filedesc *fd;
	unsigned index;
	unsigned long irq;
	int slot;

	/* Rejects an inactive reservation or missing files for a non-empty one. */
	if (reservation == NULL ||
	    !reservation->active ||
	    (reservation->count != 0 && (files == NULL || descriptors == NULL)))
		return EINVAL;
	fd = reservation->table;

	/* Every slot must still be reserved under this generation. */
	irq = spin_lock_irqsave(&fd->lock);

	for (index = 0; index < reservation->count; index++) {
		slot = reservation->slots[index];
		if (slot < 0 ||
		    slot >= KERN_OPEN_MAX ||
		    fd->entries[slot].state != FILEDESC_SLOT_RESERVED ||
		    fd->entries[slot].reservation_id != reservation->generation) {
			spin_unlock_irqrestore(&fd->lock, irq);
			return EBUSY;
		}
	}

	/* Fills the slots and wakes anyone waiting on them. */
	for (index = 0; index < reservation->count; index++) {
		slot = reservation->slots[index];
		slot_make_live(&fd->entries[slot], files[index],
		    reservation->flags);
		descriptors[index] = slot;
	}

	reservation->active = 0;
	waitq_wake_all(&fd->reservation_waitq);

	spin_unlock_irqrestore(&fd->lock, irq);

	/* Drops the reservation's table reference. */
	filedesc_destroy(fd);
	poll_notify();

	/* Reports the committed reservation. */
	return 0;
}

/*
 * Frees reserved descriptors without installing anything.
 */
void
filedesc_abort_reserved(
	struct filedesc_reservation *reservation)
{
	struct filedesc *fd;
	unsigned index;
	unsigned long irq;
	int slot;

	/* Ignores a missing or inactive reservation. */
	if (reservation == NULL || !reservation->active)
		return;
	fd = reservation->table;

	/* Frees the slots still reserved under this generation. */
	irq = spin_lock_irqsave(&fd->lock);

	for (index = 0; index < reservation->count; index++) {
		slot = reservation->slots[index];
		if (slot >= 0 &&
		    slot < KERN_OPEN_MAX &&
		    fd->entries[slot].state == FILEDESC_SLOT_RESERVED &&
		    fd->entries[slot].reservation_id == reservation->generation)
			slot_make_free(&fd->entries[slot]);
	}

	reservation->active = 0;
	waitq_wake_all(&fd->reservation_waitq);

	spin_unlock_irqrestore(&fd->lock, irq);

	/* Drops the reservation's table reference. */
	filedesc_destroy(fd);
	poll_notify();
}

/*
 * Sets the soft limit on descriptor numbers.
 */
int
filedesc_set_limit(
	struct filedesc *fd,
	unsigned limit)
{
	unsigned long irq;

	/* Rejects a missing table or a limit beyond the table size. */
	if (fd == NULL || limit > KERN_OPEN_MAX)
		return EINVAL;

	/* Records the limit. */
	irq = spin_lock_irqsave(&fd->lock);

	fd->soft_limit = limit;

	spin_unlock_irqrestore(&fd->lock, irq);

	/* Reports the changed limit. */
	return 0;
}

/*
 * Reads the soft limit on descriptor numbers.
 */
unsigned
filedesc_get_limit(
	struct filedesc *fd)
{
	unsigned limit;
	unsigned long irq;

	/* A missing table has no limit. */
	if (fd == NULL)
		return 0;

	/* Samples the limit under the lock. */
	irq = spin_lock_irqsave(&fd->lock);

	limit = fd->soft_limit;

	spin_unlock_irqrestore(&fd->lock, irq);

	/* Reports the sampled limit. */
	return limit;
}

/*
 * Closes every descriptor marked close-on-exec.
 */
void
filedesc_close_on_exec(
	struct filedesc *fd)
{
	struct file *detached[KERN_OPEN_MAX];
	unsigned long irq;
	int descriptor;
	int count;

	count = 0;

	/* Ignores a missing table. */
	if (fd == NULL)
		return;

	/* Detaches the marked files under the lock. */
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

	/* Releases the record locks, then closes the files. */
	for (descriptor = 0; descriptor < count; descriptor++) {
		if (detached[descriptor]->f_inode != NULL)
			record_lock_release_process_inode(fd->owner,
			    detached[descriptor]->f_inode);
	}

	for (descriptor = 0; descriptor < count; descriptor++) {
		if (file_readahead_invalidate != NULL)
			file_readahead_invalidate(detached[descriptor]);
		(void)file_close(detached[descriptor]);
	}
}

/* Marks a slot free. */
static void
slot_make_free(
	struct filedesc_entry *entry)
{
	entry->file = NULL;
	entry->flags = 0;
	entry->state = FILEDESC_SLOT_FREE;
	entry->reservation_id = 0;
}

/* Marks a slot live with a file and its descriptor flags. */
static void
slot_make_live(
	struct filedesc_entry *entry,
	struct file *file,
	unsigned flags)
{
	entry->file = file;
	entry->flags = flags;
	entry->state = FILEDESC_SLOT_LIVE;
	entry->reservation_id = 0;
}

/* Marks a slot reserved under a reservation generation. */
static void
slot_make_reserved(
	struct filedesc_entry *entry,
	uint64_t id)
{
	entry->file = NULL;
	entry->flags = 0;
	entry->state = FILEDESC_SLOT_RESERVED;
	entry->reservation_id = id;
}
