/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Per-process file descriptor tables.
 *
 * A table is an array of slots that are free, live with an object and its
 * descriptor flags, or reserved for a multi-descriptor operation.  It starts
 * small and grows (filedesc_grow) when a search finds no free slot below the
 * process limit; a grown array replaces the old one under the lock, so a slot
 * is only ever addressed while the lock is held.
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
#include <kern/kcrt.h>

#include <uapi/errno.h>
#include <uapi/socket.h>

/* A received message's rights are claimed by one reservation. */
_Static_assert(KERN_MSG_FD_MAX <= FILEDESC_RESERVE_MAX,
    "a reservation must hold a message's descriptors");

/* How many descriptors close-on-exec detaches at a time. */
#define FILEDESC_BATCH	32U

/*
 * File-cache builds provide descriptor-close cancellation; other builds omit it.
 * A null callback means descriptor withdrawal has no cache work to invalidate.
 */
extern void file_readahead_invalidate(struct file *) __attribute__((weak));

/* Counts live tables until their final reference, independent of slot contents. */
static atomic_uint_t filedesc_live;

static void slot_make_free(struct filedesc_entry *entry);
static void slot_make_live(struct filedesc_entry *entry, const struct fd_object *object, unsigned flags);
static void descriptor_record_unlock(struct filedesc *fd, const struct fd_object *object);
static void descriptor_readahead_cancel(const struct fd_object *object);
static int filedesc_take_kind(struct filedesc *fd, int descriptor, enum fd_object_type kind, struct fd_object *result);
static void slot_make_reserved(struct filedesc_entry *entry, uint64_t id);
static int filedesc_grow(struct filedesc *fd, unsigned wanted);
static unsigned slot_span_locked(const struct filedesc *fd);
static void notify_table_pollers(struct filedesc *fd);

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
		fd->entries = kern_calloc(FILEDESC_INITIAL_SLOTS,
		    sizeof(*fd->entries));
		if (fd->entries == NULL) {
			kern_free(fd);

			/* Failed: no memory for the first slots. */
			return NULL;
		}
		fd->capacity = FILEDESC_INITIAL_SLOTS;
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
 * Drops a table reference and closes every object after the final reference.
 */
void
filedesc_destroy(
	struct filedesc *fd)
{
	struct filedesc_entry *entries;
	struct fd_object object;
	unsigned long irq;
	unsigned capacity;
	unsigned descriptor;
	int last;

	/* A missing table needs no cleanup. */
	if (fd == NULL)
		return;

	/* Reservations and sharing keep the table alive until their final put. */
	last = refcount_put(&fd->refs);
	if (!last)
		return;

	/*
	 * Withdraws the whole namespace before any callback can reenter a
	 * descriptor table: the slots leave the table and are walked here.
	 */
	irq = spin_lock_irqsave(&fd->lock);
	entries = fd->entries;
	capacity = fd->capacity;
	fd->entries = NULL;
	fd->capacity = 0;
	spin_unlock_irqrestore(&fd->lock, irq);

	/* Wake readiness scans after withdrawing the whole namespace. */
	notify_table_pollers(fd);

	/* Every process record lock goes first, before any file backend closes. */
	for (descriptor = 0; descriptor < capacity; descriptor++) {
		if (entries[descriptor].state == FILEDESC_SLOT_LIVE)
			descriptor_record_unlock(fd, &entries[descriptor].object);
	}

	/* Each live slot gives up its one ownership reference. */
	for (descriptor = 0; descriptor < capacity; descriptor++) {
		if (entries[descriptor].state != FILEDESC_SLOT_LIVE)
			continue;
		object = entries[descriptor].object;
		slot_make_free(&entries[descriptor]);
		descriptor_readahead_cancel(&object);
		(void)fd_object_put(&object);
	}

	/* The table no longer contributes to global descriptor accounting. */
	(void)atomic_raw_fetch_add_relaxed(&filedesc_live.value, (unsigned)-1);
	kern_free(entries);
	kern_free(fd);

	/* Succeeded: the final table and all detached references are retired. */
	return;
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
 * Retains the file behind a descriptor without interpreting handle payloads.
 */
struct file *
filedesc_get_ref(
	struct filedesc *fd,
	int descriptor)
{
	struct fd_object object;
	int error;

	/* The common lookup acquires ownership while the slot is still protected. */
	error = filedesc_get_object_ref(fd, descriptor, &object);
	if (error != 0)
		return NULL;

	/* File-only callers cannot use a kernel handle as an I/O description. */
	if (object.type != FD_OBJECT_FILE) {
		(void)fd_object_put(&object);
		return NULL;
	}

	/* Succeeded: the caller owns one file reference. */
	return object.data.file;
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
 * Installs an owned file reference at or above the requested minimum.
 */
int
filedesc_install_from(
	struct filedesc *fd,
	struct file *file,
	unsigned flags,
	int minimum,
	int *descriptor)
{
	struct fd_object object;
	int error;

	/* The generic installer consumes the file reference only on success. */
	object.type = FD_OBJECT_FILE;
	object.data.file = file;
	error = filedesc_install_object_from(fd, &object, flags, minimum, descriptor);
	if (error != 0)
		return error;

	/* Succeeded: the descriptor table owns the supplied file reference. */
	return 0;
}

/*
 * Installs an owned file reference in a specific free descriptor.
 */
int
filedesc_install_at(
	struct filedesc *fd,
	struct file *file,
	int descriptor)
{
	struct fd_object object;
	int error;

	/* Specific-slot publication follows the same ownership rule as other installs. */
	object.type = FD_OBJECT_FILE;
	object.data.file = file;
	error = filedesc_install_object_at(fd, &object, 0, descriptor);
	if (error != 0)
		return error;

	/* Succeeded: the selected slot owns the supplied file reference. */
	return 0;
}

/*
 * Detaches a file reference while leaving non-file descriptors untouched.
 */
int
filedesc_take(
	struct filedesc *fd,
	int descriptor,
	struct file **result)
{
	struct fd_object object;
	int error;

	/* A caller must provide storage for the ownership transfer. */
	if (result == NULL)
		return EBADF;

	/* Type selection and withdrawal are one operation under the table lock. */
	*result = NULL;
	error = filedesc_take_kind(fd, descriptor, FD_OBJECT_FILE, &object);
	if (error != 0)
		return error;

	/* Succeeded: the caller owns the detached file reference. */
	*result = object.data.file;
	return 0;
}

/*
 * Closes either object class through the common descriptor lifetime.
 */
int
filedesc_close(
	struct filedesc *fd,
	int descriptor)
{
	struct fd_object object;
	int error;

	/* Withdrawal prevents a concurrent lookup from resurrecting this reference. */
	error = filedesc_take_object(fd, descriptor, &object);
	if (error != 0)
		return error;

	/* Backend destruction runs after the descriptor-table lock is released. */
	error = fd_object_put(&object);
	if (error != 0)
		return error;

	/* Succeeded: the descriptor no longer contributes an ownership reference. */
	return 0;
}

/*
 * Copies standard descriptor positions while retaining their actual object types.
 */
int
filedesc_clone_stdio(
	struct filedesc *source,
	struct filedesc *destination)
{
	struct fd_object object;
	int descriptor;
	int error;

	/* Both namespaces must exist before any ownership can move. */
	if (source == NULL || destination == NULL)
		return EINVAL;

	/* A handle duplicated onto a standard descriptor remains a handle. */
	for (descriptor = 0; descriptor < 3; descriptor++) {
		/* Closed standard descriptors contribute nothing to the destination. */
		error = filedesc_get_object_ref(source, descriptor, &object);
		if (error != 0)
			continue;

		/* Publication consumes only this newly acquired reference. */
		error = filedesc_install_object_at(destination, &object, 0, descriptor);
		if (error != 0) {
			(void)fd_object_put(&object);
			return error;
		}
	}

	/* Succeeded: every inherited standard position owns its reference. */
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
	struct fd_object object;
	unsigned long irq;
	unsigned capacity;
	unsigned descriptor;
	int error;

	/* Rejects a missing source or result. */
	if (source == NULL || result == NULL)
		return EINVAL;

	/* Creates the table for the new owner. */
	copy = filedesc_create(owner);
	if (copy == NULL)
		return ENOMEM;

	/*
	 * Inherit the source namespace while close cannot withdraw its
	 * references.  The copy is first grown to the source's size; the
	 * source can grow meanwhile, which only means growing again.
	 */
	for (;;) {
		irq = spin_lock_irqsave(&source->lock);
		capacity = source->capacity;
		if (copy->capacity >= capacity)
			break;
		spin_unlock_irqrestore(&source->lock, irq);
		error = filedesc_grow(copy, capacity);
		if (error != 0) {
			filedesc_destroy(copy);
			return error;
		}
	}

	copy->soft_limit = source->soft_limit;

	/* Preserve each inheritable descriptor's object class, position and flags. */
	for (descriptor = 0; descriptor < capacity; descriptor++) {
		/* Unpublished slots and close-on-fork descriptors do not enter the child. */
		if (source->entries[descriptor].state != FILEDESC_SLOT_LIVE ||
		    (source->entries[descriptor].flags & FILEDESC_CLOFORK) != 0)
			continue;

		/* The child slot owns a new reference without consuming the parent's. */
		object = source->entries[descriptor].object;
		fd_object_get(&object);
		slot_make_live(&copy->entries[descriptor], &object, source->entries[descriptor].flags);
	}

	spin_unlock_irqrestore(&source->lock, irq);

	/* Publish the completed child namespace after leaving the source lock. */
	*result = copy;

	/* Succeeded: the child owns every inherited descriptor reference. */
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

	if ((unsigned)descriptor >= fd->capacity ||
	    fd->entries[descriptor].state != FILEDESC_SLOT_LIVE) {
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

	if ((unsigned)descriptor >= fd->capacity ||
	    fd->entries[descriptor].state != FILEDESC_SLOT_LIVE) {
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
	struct fd_object object;
	unsigned long irq;
	int descriptor;
	int error;

	/* Rejects a descriptor out of range, then any other bad operand. */
	if (oldfd < 0 || oldfd >= KERN_OPEN_MAX)
		return EBADF;

	/* Invalid destination policy must not consume a source reference. */
	if (fd == NULL ||
	    result == NULL ||
	    minimum < 0 ||
	    minimum >= KERN_OPEN_MAX ||
	    (flags & ~FILEDESC_FLAG_MASK) != 0)
		return EINVAL;

retry:
	/* Hold the source namespace stable through duplicate publication. */
	irq = spin_lock_irqsave(&fd->lock);

	/* A reserved or closed source exposes no object to duplicate. */
	if ((unsigned)oldfd >= fd->capacity ||
	    fd->entries[oldfd].state != FILEDESC_SLOT_LIVE) {
		spin_unlock_irqrestore(&fd->lock, irq);
		return EBADF;
	}

	/* Finds the first free slot under the limit. */
	for (descriptor = minimum; descriptor < (int)slot_span_locked(fd);
	     descriptor++) {
		/* Reservations remain unavailable until their transaction settles. */
		if (fd->entries[descriptor].state == FILEDESC_SLOT_FREE)
			break;
	}

	/* A full table below the limit grows, then the search runs again. */
	if (descriptor >= (int)slot_span_locked(fd) &&
	    descriptor < (int)fd->soft_limit) {
		spin_unlock_irqrestore(&fd->lock, irq);
		error = filedesc_grow(fd, (unsigned)descriptor + 1U);
		if (error != 0)
			return error;
		goto retry;
	}

	/* No destination below the current process limit could be selected. */
	if (descriptor >= (int)fd->soft_limit) {
		spin_unlock_irqrestore(&fd->lock, irq);
		return EMFILE;
	}

	/* The duplicate contributes its own reference to the same object. */
	object = fd->entries[oldfd].object;
	fd_object_get(&object);
	slot_make_live(&fd->entries[descriptor], &object, flags);
	*result = descriptor;

	spin_unlock_irqrestore(&fd->lock, irq);

	/* Readiness scanners must observe the newly published descriptor identity. */
	notify_table_pollers(fd);

	/* Succeeded: the duplicate owns its reference to the source object. */
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
	struct fd_object object;
	struct fd_object displaced;
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

	/* A target below the limit but past the table's end needs the table grown. */
	if ((unsigned)newfd < filedesc_get_limit(fd)) {
		error = filedesc_grow(fd, (unsigned)newfd + 1U);
		if (error != 0)
			return error;
	}

	/* Re-checks the source and waits while the target is reserved. */
	irq = spin_lock_irqsave(&fd->lock);

	/* A wakeup must revalidate the source and the destination reservation. */
	while (1) {
		/* Concurrent source close can invalidate the requested duplication. */
		if ((unsigned)oldfd >= fd->capacity ||
		    fd->entries[oldfd].state != FILEDESC_SLOT_LIVE) {
			spin_unlock_irqrestore(&fd->lock, irq);
			return EBADF;
		}

		/* Equal descriptor numbers preserve the existing ownership and flags. */
		if (oldfd == newfd) {
			spin_unlock_irqrestore(&fd->lock, irq);
			if (reject_equal)
				return EINVAL;

			/* Succeeded: a same-descriptor duplicate needs no ownership change. */
			return 0;
		}

		/* A changed process limit can make the target unavailable after a wakeup. */
		if ((unsigned)newfd >= fd->soft_limit ||
		    (unsigned)newfd >= fd->capacity) {
			spin_unlock_irqrestore(&fd->lock, irq);
			return EBADF;
		}

		/* Only a settled destination can be replaced atomically. */
		if (fd->entries[newfd].state != FILEDESC_SLOT_RESERVED)
			break;

		/* Wait for this reservation generation without losing the protected source check. */
		observed = waitq_sequence(&fd->reservation_waitq);
		error = waitq_sleep(&fd->reservation_waitq, &fd->lock, observed, 0, WAITQ_INTERRUPTIBLE);
		if (error == EAGAIN)
			continue;

		/* An interrupted reservation wait leaves both descriptor slots unchanged. */
		if (error != 0) {
			spin_unlock_irqrestore(&fd->lock, irq);
			return error;
		}
	}

	/* Retain the source before withdrawing any destination ownership. */
	object = fd->entries[oldfd].object;
	fd_object_get(&object);
	fd_object_clear(&displaced);

	/* An occupied destination transfers its old reference to cleanup. */
	if (fd->entries[newfd].state == FILEDESC_SLOT_LIVE)
		displaced = fd->entries[newfd].object;

	/* Publish the replacement while both descriptor numbers remain protected. */
	slot_make_live(&fd->entries[newfd], &object, flags);

	spin_unlock_irqrestore(&fd->lock, irq);

	/* Notify scanners after the atomic destination replacement is visible. */
	notify_table_pollers(fd);

	/* File record locks and either backend's destruction stay outside the lock. */
	descriptor_record_unlock(fd, &displaced);
	descriptor_readahead_cancel(&displaced);
	(void)fd_object_put(&displaced);

	/* Succeeded: the duplicate owns its reference to the source object. */
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
	struct fd_object first_object;
	struct fd_object second_object;
	unsigned long irq;
	int first_slot;
	int second_slot;
	int error;
	int i;

	/* Rejects a missing operand or unknown flags. */
	if (fd == NULL ||
	    first == NULL ||
	    second == NULL ||
	    result == NULL ||
	    ((first_flags | second_flags) & ~FILEDESC_FLAG_MASK) != 0)
		return EINVAL;

retry:
	/* Both required positions remain unselected until the locked table scan. */
	first_slot = -1;
	second_slot = -1;

	/* Finds the two lowest free slots under the limit. */
	irq = spin_lock_irqsave(&fd->lock);

	/* Select distinct free positions without publishing either file prematurely. */
	for (i = 0; i < (int)slot_span_locked(fd); i++) {
		/* Live and reserved slots cannot accept either supplied reference. */
		if (fd->entries[i].state != FILEDESC_SLOT_FREE)
			continue;

		/* Stop only after both members of the atomic pair have a position. */
		if (first_slot < 0) {
			first_slot = i;
		} else {
			second_slot = i;
			break;
		}
	}

	/* A full table below the limit grows, then the search runs again. */
	if (second_slot < 0 && fd->capacity < fd->soft_limit) {
		i = (int)fd->capacity;
		spin_unlock_irqrestore(&fd->lock, irq);
		error = filedesc_grow(fd, (unsigned)i + 2U);
		if (error != 0)
			return error;
		goto retry;
	}

	/* A single free position cannot satisfy this atomic pair installation. */
	if (second_slot < 0) {
		spin_unlock_irqrestore(&fd->lock, irq);
		return EMFILE;
	}

	/* Fills both slots without changing either supplied reference count. */
	first_object.type = FD_OBJECT_FILE;
	first_object.data.file = first;

	/* The second carrier independently describes its supplied file reference. */
	second_object.type = FD_OBJECT_FILE;
	second_object.data.file = second;

	/* Both references become visible in the same critical section. */
	slot_make_live(&fd->entries[first_slot], &first_object, first_flags);
	slot_make_live(&fd->entries[second_slot], &second_object, second_flags);
	result[0] = first_slot;
	result[1] = second_slot;

	spin_unlock_irqrestore(&fd->lock, irq);

	/* Publish readiness changes only after both descriptors are live. */
	notify_table_pollers(fd);

	/* Succeeded: the namespace owns both supplied file references. */
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
	unsigned capacity;
	unsigned found;
	unsigned index;
	unsigned long irq;
	int error;

	/* Rejects a missing operand, too many descriptors, or unknown flags. */
	if (fd == NULL ||
	    reservation == NULL ||
	    count > FILEDESC_RESERVE_MAX ||
	    (flags & ~FILEDESC_FLAG_MASK) != 0)
		return EINVAL;
	kern_memset(reservation, 0, sizeof(*reservation));

retry:
	/* Collects the lowest free slots under the limit. */
	found = 0;
	irq = spin_lock_irqsave(&fd->lock);

	for (index = 0; index < slot_span_locked(fd) && found < count; index++) {
		if (fd->entries[index].state == FILEDESC_SLOT_FREE)
			reservation->slots[found++] = (int)index;
	}

	/* A full table below the limit grows, then the search runs again. */
	if (found != count && fd->capacity < fd->soft_limit) {
		capacity = fd->capacity;
		spin_unlock_irqrestore(&fd->lock, irq);
		error = filedesc_grow(fd, capacity + (count - found));
		if (error != 0)
			return error;
		goto retry;
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
 * Installs owned file references into a reserved group of descriptors.
 */
int
filedesc_commit_reserved(
	struct filedesc_reservation *reservation,
	struct file **files,
	int *descriptors)
{
	struct fd_object objects[FILEDESC_RESERVE_MAX];
	unsigned index;
	int error;

	/* Validate the bounded group before constructing file carriers. */
	if (reservation == NULL || reservation->count > FILEDESC_RESERVE_MAX)
		return EINVAL;

	/* A nonempty reservation requires a file pointer array. */
	if (reservation->count != 0 && files == NULL)
		return EINVAL;

	/* The compatibility layer moves existing references without retaining them. */
	for (index = 0; index < reservation->count; index++) {
		objects[index].type = FD_OBJECT_FILE;
		objects[index].data.file = files[index];
	}

	/* The generic commit consumes all references together or consumes none. */
	error = filedesc_commit_objects(reservation, objects, descriptors);
	if (error != 0)
		return error;

	/* Succeeded: the reserved descriptors now own the file references. */
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
		    (unsigned)slot < fd->capacity &&
		    fd->entries[slot].state == FILEDESC_SLOT_RESERVED &&
		    fd->entries[slot].reservation_id == reservation->generation)
			slot_make_free(&fd->entries[slot]);
	}

	reservation->active = 0;
	waitq_wake_all(&fd->reservation_waitq);

	spin_unlock_irqrestore(&fd->lock, irq);

	/* Wakes this table's polls, then drops the reservation's reference. */
	notify_table_pollers(fd);
	filedesc_destroy(fd);
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
 * Closes every object marked close-on-exec before the new image executes.
 */
void
filedesc_close_on_exec(
	struct filedesc *fd)
{
	struct fd_object detached[FILEDESC_BATCH];
	unsigned long irq;
	unsigned descriptor;
	unsigned count;
	unsigned index;

	/* A process without a table has nothing to discard. */
	if (fd == NULL)
		return;

	/*
	 * Withdraws the marked slots a batch at a time, so the detached
	 * references fit on the stack; a withdrawn slot is free and is not
	 * found again.
	 */
	do {
		count = 0;
		irq = spin_lock_irqsave(&fd->lock);

		/* Close flags belong to descriptors, independently of their object class. */
		for (descriptor = 0; descriptor < fd->capacity &&
		     count < FILEDESC_BATCH; descriptor++) {
			/* Unmarked and reserved slots do not belong to this close pass. */
			if (fd->entries[descriptor].state != FILEDESC_SLOT_LIVE ||
			    (fd->entries[descriptor].flags & FILEDESC_CLOEXEC) == 0)
				continue;

			/* Each withdrawn slot transfers one ownership reference to cleanup. */
			detached[count++] = fd->entries[descriptor].object;
			slot_make_free(&fd->entries[descriptor]);
		}

		spin_unlock_irqrestore(&fd->lock, irq);

		/* Readiness scanners must see the now-closed descriptors. */
		if (count != 0)
			notify_table_pollers(fd);

		/* Drop file record locks before closing any file backend. */
		for (index = 0; index < count; index++)
			descriptor_record_unlock(fd, &detached[index]);

		/* Handle callbacks cannot execute while the descriptor lock is held. */
		for (index = 0; index < count; index++) {
			descriptor_readahead_cancel(&detached[index]);
			(void)fd_object_put(&detached[index]);
		}
	} while (count == FILEDESC_BATCH);

	/* Succeeded: the new process image inherits none of the marked descriptors. */
	return;
}

/*
 * Retains an I/O file while distinguishing non-file descriptors from absent ones.
 */
int
filedesc_get_file(
	struct filedesc *fd,
	int descriptor,
	struct file **result)
{
	struct fd_object object;
	int error;

	/* A caller must provide storage for its acquired file reference. */
	if (result == NULL)
		return EINVAL;

	/* Generic lookup captures one stable descriptor identity. */
	*result = NULL;
	error = filedesc_get_object_ref(fd, descriptor, &object);
	if (error != 0)
		return error;

	/* A live handle has no file status, inode attributes or file operations. */
	if (object.type != FD_OBJECT_FILE) {
		(void)fd_object_put(&object);
		return EOPNOTSUPP;
	}

	/* Succeeded: the caller owns the acquired file reference. */
	*result = object.data.file;
	return 0;
}

/*
 * Retains the actual object behind a live descriptor under its table lock.
 */
int
filedesc_get_object_ref(
	struct filedesc *fd,
	int descriptor,
	struct fd_object *result)
{
	unsigned long irq;

	/* Refuse an invalid namespace, descriptor number or result carrier. */
	if (fd == NULL ||
	    result == NULL ||
	    descriptor < 0 ||
	    descriptor >= KERN_OPEN_MAX)
		return EBADF;

	/* A failed lookup publishes no ownership. */
	fd_object_clear(result);
	irq = spin_lock_irqsave(&fd->lock);

	/* A reservation does not expose the future descriptor before commit. */
	if ((unsigned)descriptor >= fd->capacity ||
	    fd->entries[descriptor].state != FILEDESC_SLOT_LIVE) {
		spin_unlock_irqrestore(&fd->lock, irq);
		return EBADF;
	}

	/* Keep the existing file lookup checkpoint for race-sensitive fixtures. */
	*result = fd->entries[descriptor].object;
	if (result->type == FD_OBJECT_FILE) {
		KERN_TEST_CHECKPOINT(KERN_TEST_FD_LOOKUP_BEFORE_REF, result->data.file);
	}

	/* Close cannot detach the slot until this temporary ownership exists. */
	fd_object_get(result);

	spin_unlock_irqrestore(&fd->lock, irq);

	/* Succeeded: the caller owns a stable object reference. */
	return 0;
}

/*
 * Installs an owned object in the first available descriptor above a minimum.
 */
int
filedesc_install_object_from(
	struct filedesc *fd,
	const struct fd_object *object,
	unsigned flags,
	int minimum,
	int *descriptor)
{
	unsigned long irq;
	int error;
	int slot;
	int valid;

	/* Validate the namespace and descriptor flags before taking its lock. */
	if (fd == NULL ||
	    descriptor == NULL ||
	    minimum < 0 ||
	    minimum >= KERN_OPEN_MAX ||
	    (flags & ~FILEDESC_FLAG_MASK) != 0)
		return EINVAL;

	/* A live slot must always carry a supported object reference. */
	valid = fd_object_valid(object);
	if (!valid)
		return EINVAL;

retry:
	/* Find the lowest free slot under the current process limit. */
	irq = spin_lock_irqsave(&fd->lock);

	/* Reserved slots remain unavailable until their transaction finishes. */
	for (slot = minimum; slot < (int)slot_span_locked(fd); slot++) {
		/* The selected slot receives ownership without changing its reference count. */
		if (fd->entries[slot].state == FILEDESC_SLOT_FREE)
			break;
	}

	/* A full table below the limit grows, then the search runs again. */
	if (slot >= (int)slot_span_locked(fd) && slot < (int)fd->soft_limit) {
		spin_unlock_irqrestore(&fd->lock, irq);
		error = filedesc_grow(fd, (unsigned)slot + 1U);
		if (error != 0)
			return error;
		goto retry;
	}

	/* A full table leaves the caller's ownership unchanged. */
	if (slot >= (int)fd->soft_limit) {
		spin_unlock_irqrestore(&fd->lock, irq);
		return EMFILE;
	}

	/* Publication transfers the supplied reference to this descriptor. */
	slot_make_live(&fd->entries[slot], object, flags);
	*descriptor = slot;

	spin_unlock_irqrestore(&fd->lock, irq);

	/* Readiness scans must observe the newly live descriptor. */
	notify_table_pollers(fd);

	/* Succeeded: the table owns the supplied reference. */
	return 0;
}

/*
 * Installs an owned object in a specific free descriptor.
 */
int
filedesc_install_object_at(
	struct filedesc *fd,
	const struct fd_object *object,
	unsigned flags,
	int descriptor)
{
	unsigned long irq;
	int error;
	int valid;

	/* Reject invalid publication coordinates without consuming ownership. */
	if (fd == NULL ||
	    descriptor < 0 ||
	    descriptor >= KERN_OPEN_MAX ||
	    (flags & ~FILEDESC_FLAG_MASK) != 0)
		return EINVAL;

	/* A live slot cannot contain an empty or unknown object. */
	valid = fd_object_valid(object);
	if (!valid)
		return EINVAL;

	/* A position below the limit but past the table's end needs the table grown. */
	if ((unsigned)descriptor < filedesc_get_limit(fd)) {
		error = filedesc_grow(fd, (unsigned)descriptor + 1U);
		if (error != 0)
			return error;
	}

	/* Protect the limit check and publication as one operation. */
	irq = spin_lock_irqsave(&fd->lock);

	/* No new descriptor may exceed the current process soft limit. */
	if ((unsigned)descriptor >= fd->soft_limit ||
	    (unsigned)descriptor >= fd->capacity) {
		spin_unlock_irqrestore(&fd->lock, irq);
		return EMFILE;
	}

	/* Specific-slot installation never replaces a live or reserved descriptor. */
	if (fd->entries[descriptor].state != FILEDESC_SLOT_FREE) {
		spin_unlock_irqrestore(&fd->lock, irq);
		return EBUSY;
	}

	/* The slot takes the caller's existing reference. */
	slot_make_live(&fd->entries[descriptor], object, flags);

	spin_unlock_irqrestore(&fd->lock, irq);

	/* Notify waiters after the descriptor becomes visible. */
	notify_table_pollers(fd);

	/* Succeeded: the selected descriptor owns the supplied reference. */
	return 0;
}

/*
 * Detaches either object type and transfers the slot reference to the caller.
 */
int
filedesc_take_object(
	struct filedesc *fd,
	int descriptor,
	struct fd_object *result)
{
	int error;

	/* NONE selects any supported live object without weakening slot validation. */
	error = filedesc_take_kind(fd, descriptor, FD_OBJECT_NONE, result);
	if (error != 0)
		return error;

	/* Succeeded: the caller owns the detached descriptor reference. */
	return 0;
}

/*
 * Commits owned object references into an already reserved descriptor group.
 */
int
filedesc_commit_objects(
	struct filedesc_reservation *reservation,
	struct fd_object *objects,
	int *descriptors)
{
	struct filedesc *fd;
	unsigned index;
	unsigned long irq;
	int slot;
	int valid;

	/* Refuse malformed transactions before indexing their bounded slot arrays. */
	if (reservation == NULL ||
	    !reservation->active ||
	    reservation->count > FILEDESC_RESERVE_MAX ||
	    (reservation->count != 0 &&
	     (objects == NULL || descriptors == NULL)))
		return EINVAL;

	/* Validate every object before publishing any descriptor. */
	for (index = 0; index < reservation->count; index++) {
		valid = fd_object_valid(&objects[index]);
		if (!valid)
			return EINVAL;
	}

	/* The reservation's table reference protects this namespace until completion. */
	fd = reservation->table;
	irq = spin_lock_irqsave(&fd->lock);

	/* All slots must still belong to this exact reservation generation. */
	for (index = 0; index < reservation->count; index++) {
		slot = reservation->slots[index];
		if (slot < 0 ||
		    (unsigned)slot >= fd->capacity ||
		    fd->entries[slot].state != FILEDESC_SLOT_RESERVED ||
		    fd->entries[slot].reservation_id != reservation->generation) {
			spin_unlock_irqrestore(&fd->lock, irq);
			return EBUSY;
		}
	}

	/* Publication consumes all supplied references together, without a refcount gap. */
	for (index = 0; index < reservation->count; index++) {
		slot = reservation->slots[index];
		slot_make_live(&fd->entries[slot], &objects[index], reservation->flags);
		descriptors[index] = slot;
		fd_object_clear(&objects[index]);
	}

	/* Waiters may now reuse the completed reservation state. */
	reservation->active = 0;
	waitq_wake_all(&fd->reservation_waitq);

	spin_unlock_irqrestore(&fd->lock, irq);

	/* Wakes this table's polls before the reservation's reference goes. */
	notify_table_pollers(fd);

	/* The committed descriptors, not the reservation, now own their objects. */
	filedesc_destroy(fd);

	/* Succeeded: the table owns every committed reference. */
	return 0;
}

/*
 * Counts a poll that is about to scan this table's descriptor numbers.
 *
 * The count is published before the poll reads the channel sequence and
 * scans, so a table change either lands before the scan, which sees it,
 * or after, when notify_table_pollers() sees the count and wakes the poll.
 */
void
filedesc_poll_begin(
	struct filedesc *fd)
{
	/* Publishes the poll before it reads the channel sequence. */
	(void)__atomic_fetch_add(&fd->pollers, 1U, __ATOMIC_SEQ_CST);
}

/*
 * Withdraws a poll counted by filedesc_poll_begin().
 */
void
filedesc_poll_end(
	struct filedesc *fd)
{
	/* The poll no longer scans this table. */
	(void)__atomic_fetch_sub(&fd->pollers, 1U, __ATOMIC_SEQ_CST);
}

/*
 * Wakes the polls that may depend on a change to this table.
 *
 * A descriptor number's meaning matters only to a poll scanning this table;
 * a change to what an object is ready for is announced by the object's own
 * code.  So a table no poll is scanning, such as a short-lived child's,
 * wakes nobody.  The fence orders the change, already published under the
 * table lock, before the count is read.
 */
static void
notify_table_pollers(
	struct filedesc *fd)
{
	unsigned pollers;

	/* Wakes the poll channel only while a poll scans this table. */
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	pollers = __atomic_load_n(&fd->pollers, __ATOMIC_SEQ_CST);
	if (pollers != 0)
		poll_notify();
}

/* Marks a slot free. */
static void
slot_make_free(
	struct filedesc_entry *entry)
{
	/* Empty state withdraws every pointer and reservation authority. */
	fd_object_clear(&entry->object);
	entry->flags = 0;
	entry->state = FILEDESC_SLOT_FREE;
	entry->reservation_id = 0;

	/* Succeeded: the slot exposes neither ownership nor a reservation. */
	return;
}

/* Marks a slot live with an object and its descriptor flags. */
static void
slot_make_live(
	struct filedesc_entry *entry,
	const struct fd_object *object,
	unsigned flags)
{
	/* The live slot takes ownership while its caller still holds the table lock. */
	entry->object = *object;
	entry->flags = flags;
	entry->state = FILEDESC_SLOT_LIVE;
	entry->reservation_id = 0;

	/* Succeeded: the slot owns the supplied reference and descriptor flags. */
	return;
}

/* Marks a slot reserved under a reservation generation. */
static void
slot_make_reserved(
	struct filedesc_entry *entry,
	uint64_t id)
{
	/* Reserved slots expose no object until this exact generation commits. */
	fd_object_clear(&entry->object);
	entry->flags = 0;
	entry->state = FILEDESC_SLOT_RESERVED;
	entry->reservation_id = id;

	/* Succeeded: only this transaction generation may publish the slot. */
	return;
}

/* Releases process record locks only for objects that actually contain a file. */
static void
descriptor_record_unlock(
	struct filedesc *fd,
	const struct fd_object *object)
{
	/* Handles carry no filesystem record-lock identity. */
	if (object->type != FD_OBJECT_FILE)
		return;

	/* Pseudo files also have no inode record-lock identity. */
	if (object->data.file->f_inode == NULL)
		return;

	/* Closing any descriptor releases this process's locks on the file inode. */
	record_lock_release_process_inode(fd->owner, object->data.file->f_inode);

	/* Succeeded: this process retains no record locks for the closed inode. */
	return;
}

/* Cancels descriptor-owned readahead without applying file policy to handles. */
static void
descriptor_readahead_cancel(
	const struct fd_object *object)
{
	/* A handle contributes no file-cache work. */
	if (object->type != FD_OBJECT_FILE)
		return;

	/* Builds without the optional cache have no work to cancel. */
	if (file_readahead_invalidate == NULL)
		return;

	/* Descriptor withdrawal is distinct from a temporary syscall reference put. */
	file_readahead_invalidate(object->data.file);

	/* Succeeded: descriptor-owned cache work has been invalidated. */
	return;
}

/* Withdraws a selected object type while preserving concurrent slot lifetime. */
static int
filedesc_take_kind(
	struct filedesc *fd,
	int descriptor,
	enum fd_object_type kind,
	struct fd_object *result)
{
	unsigned long irq;

	/* Refuse invalid withdrawal coordinates before accessing the table. */
	if (fd == NULL ||
	    result == NULL ||
	    descriptor < 0 ||
	    descriptor >= KERN_OPEN_MAX)
		return EBADF;

	/* Failed withdrawals never hand the caller a reference. */
	fd_object_clear(result);
	irq = spin_lock_irqsave(&fd->lock);

	/* A reserved descriptor is not yet an object that close can consume. */
	if ((unsigned)descriptor >= fd->capacity ||
	    fd->entries[descriptor].state != FILEDESC_SLOT_LIVE) {
		spin_unlock_irqrestore(&fd->lock, irq);
		return EBADF;
	}

	/* File-only take must leave a handle slot completely unchanged. */
	if (kind != FD_OBJECT_NONE && fd->entries[descriptor].object.type != kind) {
		spin_unlock_irqrestore(&fd->lock, irq);
		return EBADF;
	}

	/* The detached carrier receives the slot's existing ownership reference. */
	*result = fd->entries[descriptor].object;
	slot_make_free(&fd->entries[descriptor]);

	spin_unlock_irqrestore(&fd->lock, irq);

	/* Descriptor side effects run before returning the ownership to its caller. */
	notify_table_pollers(fd);
	descriptor_record_unlock(fd, result);
	descriptor_readahead_cancel(result);

	/* Succeeded: the caller owns the reference and the descriptor is free. */
	return 0;
}

/*
 * Makes the table hold at least wanted slots.
 *
 * Called without the lock: the larger array is allocated first, then swapped
 * in under the lock unless another thread grew the table meanwhile.  The
 * capacity doubles, so a table reaches KERN_OPEN_MAX in a few steps.
 */
static int
filedesc_grow(
	struct filedesc *fd,
	unsigned wanted)
{
	struct filedesc_entry *entries;
	struct filedesc_entry *old;
	unsigned long irq;
	unsigned capacity;

	/* Rejects a size past the hard maximum. */
	if (wanted > KERN_OPEN_MAX)
		return EMFILE;

	/* Samples the current size. */
	irq = spin_lock_irqsave(&fd->lock);
	capacity = fd->capacity;
	spin_unlock_irqrestore(&fd->lock, irq);

	/* Handles a table already large enough, or one being destroyed. */
	if (capacity >= wanted)
		return 0;
	if (capacity == 0)
		return EBADF;

	/* Doubles until the wanted slot fits, within the hard maximum. */
	while (capacity < wanted)
		capacity *= 2U;
	if (capacity > KERN_OPEN_MAX)
		capacity = KERN_OPEN_MAX;

	/* Zeroed slots are free slots. */
	entries = kern_calloc(capacity, sizeof(*entries));
	if (entries == NULL)
		return ENOMEM;

	/* Swaps the larger array in, unless another thread already grew it. */
	old = NULL;
	irq = spin_lock_irqsave(&fd->lock);
	if (fd->capacity < capacity) {
		kern_memcpy(entries, fd->entries,
		    (size_t)fd->capacity * sizeof(*entries));
		old = fd->entries;
		fd->entries = entries;
		fd->capacity = capacity;
		entries = NULL;
	}
	spin_unlock_irqrestore(&fd->lock, irq);

	/* Frees whichever array is no longer used. */
	kern_free(old);
	kern_free(entries);

	/* Succeeded: the table holds the wanted slots. */
	return 0;
}

/* Reports how many slots a search may use: the smaller of the size and limit. */
static unsigned
slot_span_locked(
	const struct filedesc *fd)
{
	/* Succeeded: the searchable span. */
	return fd->capacity < fd->soft_limit ? fd->capacity : fd->soft_limit;
}
