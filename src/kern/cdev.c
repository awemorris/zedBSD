/* -*- mode: c; c-file-style: "bsd"; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The character device registry.
 *
 * Each registration publishes an immutable, reference-counted device
 * generation under a name.  Files opened on a device keep their
 * generation alive after it is unpublished, and the data finalizer runs
 * once when the last reference goes away.
 */

#include "kern/cdev.h"
#include "kern/file.h"
#include "kern/kmem.h"
#include "kern/lock.h"
#include "kern/poll.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

/*
 * The kernel-lifetime publication list, initially empty before drivers run.
 * Registry ownership retains each node; registry_lock protects its links.
 * VFS mounting does not clear registrations made during device discovery.
 */
static struct cdev *devices;

/* The last published node permits ordered append under registry_lock. */
static struct cdev *device_tail;

/* The number of published nodes, protected by registry_lock until removal. */
static unsigned device_count;

/*
 * The next publication advances this kernel-lifetime generation counter.
 * registry_lock protects it; reset never reuses a generation seen by an inode.
 */
static uint64_t next_generation;

/* Serializes namespace changes and retained snapshots for the kernel lifetime. */
static struct spinlock registry_lock = {
	{ 0 }, LOCK_RANK_DEVICE, "cdev registry", 0, 0
};

static const struct cdev *file_cdev(struct file *file);
static int cdev_open_file(struct file *file);
static int cdev_close_file(struct file *file);
static ssize_t cdev_read_file(struct file *file, void *buffer, size_t size);
static ssize_t cdev_write_file(struct file *file, const void *buffer, size_t size);
static int cdev_ioctl_file(struct file *file, unsigned long request, uintptr_t argument);
static int cdev_poll_file(struct file *file, short events, short *revents);
static int cdev_mmap_file(struct file *file, off_t offset, size_t bytes, uint32_t prot, struct vm_device_mapping **result);
static int cdev_name_valid(const char *name);

const struct file_ops cdev_file_ops = {
	.open = cdev_open_file,
	.close = cdev_close_file,
	.read = cdev_read_file,
	.write = cdev_write_file,
	.ioctl = cdev_ioctl_file,
	.poll = cdev_poll_file,
	.mmap = cdev_mmap_file,
};

/*
 * Unpublishes every character device without invalidating retained refs.
 */
void
cdev_reset(
	void)
{
	struct cdev *retired;
	struct cdev *device;
	struct cdev *next;
	unsigned long irq;

	/* Detaches the whole namespace without touching retained generations. */
	irq = spin_lock_irqsave(&registry_lock);

	retired = devices;
	devices = NULL;
	device_tail = NULL;
	device_count = 0;

	/* Invalidates lookup eligibility before another publisher enters. */
	for (device = retired;
	     device != NULL;
	     device = device->registry_next) {
		atomic_store_release(&device->published, 0);
	}

	spin_unlock_irqrestore(&registry_lock, irq);

	/* Runs finalizers outside the lock after saving each following node. */
	device = retired;
	while (device != NULL) {
		next = device->registry_next;
		device->registry_next = NULL;
		cdev_release(device);
		device = next;
	}

	/* Succeeded: the detached registry no longer retains any generation. */
	return;
}

/*
 * Publishes one legacy device and relinquishes the temporary owner ref.
 */
int
cdev_register(
	const char *name,
	dev_t rdev,
	const struct cdev_ops *ops,
	void *data)
{
	struct cdev *device;
	int error;

	/* Publishes the device as a managed generation. */
	error = cdev_register_managed(name, rdev, ops, data, NULL, &device);
	if (error != 0)
		return error;

	/* Keeps only the registry's reference. */
	cdev_release(device);

	/* Succeeded: registry ownership retains the published device. */
	return 0;
}

/*
 * Publishes one immutable managed device generation.
 *
 * The caller receives its own reference besides the registry's.  A failed
 * publication leaves data and its finalizer with the caller.
 */
int
cdev_register_managed(
	const char *name,
	dev_t rdev,
	const struct cdev_ops *ops,
	void *data,
	cdev_finalizer_t finalizer,
	struct cdev **result)
{
	struct cdev *device;
	struct cdev *existing;
	unsigned long irq;
	int error;
	int valid;
	int comparison;

	/* Clears the output even when another operand is invalid. */
	if (result == NULL)
		return EINVAL;

	/* Leaves no caller reference on any rejected publication. */
	*result = NULL;

	/* Rejects names that cannot be represented by devfs. */
	valid = cdev_name_valid(name);
	if (!valid)
		return EINVAL;

	/* Every published device must supply an operation table. */
	if (ops == NULL)
		return EINVAL;

	/* Allocates the generation before entering the publication lock. */
	device = kern_calloc(1, sizeof(*device));
	if (device == NULL)
		return ENOMEM;

	/* Prepares immutable dispatch data and caller plus registry ownership. */
	strcpy(device->name, name);
	device->rdev = rdev;
	device->ops = ops;
	device->data = data;
	device->finalizer = finalizer;
	refcount_init(&device->refs, 2);
	atomic_store_release(&device->published, 0);

	/* Serializes duplicate detection with insertion into the namespace. */
	error = 0;
	irq = spin_lock_irqsave(&registry_lock);

	/* Refuses another currently published generation of this name. */
	for (existing = devices;
	     existing != NULL;
	     existing = existing->registry_next) {
		comparison = strcmp(existing->name, name);
		if (comparison == 0) {
			error = EEXIST;
			break;
		}
	}

	/* Keeps the externally reported count representable. */
	if (error == 0 && device_count == UINT_MAX)
		error = EOVERFLOW;

	/* Never wraps into a generation retained by an earlier inode. */
	if (error == 0 && next_generation == UINT64_MAX)
		error = EOVERFLOW;

	/* Appends the generation only after every publication check succeeds. */
	if (error == 0) {
		next_generation++;
		device->generation = next_generation;

		/* Preserves registration order, including the first publication. */
		if (device_tail == NULL)
			devices = device;
		else
			device_tail->registry_next = device;

		/* Commits namespace ownership and lookup eligibility together. */
		device_tail = device;
		device_count++;
		atomic_store_release(&device->published, 1);
	}

	spin_unlock_irqrestore(&registry_lock, irq);

	/* Leaves data and its finalizer with the caller on failed publication. */
	if (error != 0) {
		kern_free(device);
		return error;
	}

	/* Transfers the caller reference alongside the registry reference. */
	*result = device;

	/* Succeeded: the supplied name resolves to this retained generation. */
	return 0;
}

/*
 * Unpublishes exactly the supplied device generation.
 */
int
cdev_unregister(
	struct cdev *device)
{
	struct cdev **link;
	struct cdev *previous;
	unsigned long irq;
	int found;

	/* Rejects a missing generation rather than removing a name alone. */
	if (device == NULL)
		return EINVAL;

	/* Searches by identity so a same-name replacement cannot be removed. */
	found = 0;
	irq = spin_lock_irqsave(&registry_lock);

	/* Retains the predecessor to repair the append position on removal. */
	previous = NULL;
	link = &devices;
	while (*link != NULL) {
		/* Stops only at the exact generation supplied by the owner. */
		if (*link == device) {
			found = 1;
			break;
		}

		/* Advances the predecessor and its owning link as one cursor. */
		previous = *link;
		link = &previous->registry_next;
	}

	/* Detaches the node before any reference or private data can be freed. */
	if (found) {
		*link = device->registry_next;

		/* Moves the append position when the removed node was last. */
		if (device_tail == device)
			device_tail = previous;

		/* Removes this generation from the visible ownership count. */
		device->registry_next = NULL;
		device_count--;
		atomic_store_release(&device->published, 0);
	}

	spin_unlock_irqrestore(&registry_lock, irq);

	/* Reports a generation already removed or never published here. */
	if (!found)
		return ENOENT;

	/* Releases registry ownership after the namespace is invalidated. */
	cdev_release(device);

	/* Succeeded: retained references survive without namespace visibility. */
	return 0;
}

/*
 * Retains one immutable device generation.
 */
void
cdev_ref(
	struct cdev *device)
{
	/* Ignores a missing device. */
	if (device != NULL)
		refcount_get(&device->refs);
}

/*
 * Releases one generation and runs its terminal data finalizer once.
 */
void
cdev_release(
	struct cdev *device)
{
	cdev_finalizer_t finalizer;
	void *data;
	int last;

	/* Only the last reference finalizes. */
	if (device == NULL)
		return;

	/* Releases ownership and lets only the final holder destroy data. */
	last = refcount_put(&device->refs);
	if (!last)
		return;

	/* Finalizes the data, then frees the generation. */
	finalizer = device->finalizer;
	data = device->data;
	if (finalizer != NULL)
		finalizer(data);
	kern_free(device);

	/* Succeeded: the final generation owner released its dispatch data. */
	return;
}

/*
 * Reports whether the exact generation remains in the visible registry.
 */
int
cdev_is_published(
	const struct cdev *device)
{
	unsigned published;

	/* A missing device is not published. */
	if (device == NULL)
		return 0;

	/* Reads the flag the registry maintains. */
	published = atomic_load_acquire(&device->published);
	if (published == 0)
		return 0;

	/* Succeeded: this generation is still visible to namespace lookup. */
	return 1;
}

/*
 * Returns the immutable identifier assigned at publication.
 */
uint64_t
cdev_generation(
	const struct cdev *device)
{
	/* A missing device has no generation. */
	if (device == NULL)
		return 0;

	/* Reports the identifier. */
	return device->generation;
}

/*
 * Finds and retains the currently published generation for one name.
 */
struct cdev *
cdev_find_ref(
	const char *name)
{
	struct cdev *device;
	unsigned long irq;
	int comparison;

	/* A missing name cannot identify a published device. */
	if (name == NULL)
		return NULL;

	/* Retains the matching immutable generation before namespace changes. */
	irq = spin_lock_irqsave(&registry_lock);

	/* Searches the publication list in registration order. */
	device = devices;
	while (device != NULL) {
		comparison = strcmp(device->name, name);
		if (comparison == 0) {
			cdev_ref(device);
			break;
		}

		/* Advances only after this generation failed to match. */
		device = device->registry_next;
	}

	spin_unlock_irqrestore(&registry_lock, irq);

	/* A name removed before this lookup has no current generation. */
	if (device == NULL)
		return NULL;

	/* Succeeded: the caller retains the published generation. */
	return device;
}

/*
 * Retains as many current device generations as the supplied array can hold.
 */
unsigned
cdev_snapshot(
	struct cdev **snapshot,
	unsigned capacity)
{
	struct cdev *device;
	unsigned count;
	unsigned long irq;

	/* A missing or empty array cannot receive any references. */
	if (snapshot == NULL || capacity == 0)
		return 0;

	/* Copies one bounded, coherent prefix of the publication list. */
	irq = spin_lock_irqsave(&registry_lock);

	/* Holds each generation before allowing concurrent namespace removal. */
	count = 0;
	device = devices;
	while (device != NULL && count < capacity) {
		snapshot[count] = device;
		cdev_ref(device);
		count++;
		device = device->registry_next;
	}

	spin_unlock_irqrestore(&registry_lock, irq);

	/* Succeeded: the caller owns each reference in the reported prefix. */
	return count;
}

/*
 * Allocates and retains a complete coherent snapshot of the live registry.
 *
 * Allocation occurs outside the registry lock.  A concurrent registration
 * can require a larger allocation before the final locked copy is taken.
 */
int
cdev_snapshot_alloc(
	struct cdev ***result,
	unsigned *count_out)
{
	struct cdev **snapshot;
	struct cdev *device;
	size_t allocation_bytes;
	unsigned capacity;
	unsigned needed;
	unsigned count;
	unsigned long irq;

	/* Requires both ownership outputs before allocating or retaining data. */
	if (result == NULL || count_out == NULL)
		return EINVAL;

	/* Leaves no transferred ownership on every allocation failure. */
	*result = NULL;
	*count_out = 0;
	snapshot = NULL;
	capacity = 0;
	count = 0;

	/* Retries only when publishers outgrow the allocation just prepared. */
	for (;;) {
		/* Measures and, when possible, captures the whole current list. */
		irq = spin_lock_irqsave(&registry_lock);

		needed = device_count;

		/* An empty namespace needs neither pointer storage nor references. */
		if (needed == 0) {
			spin_unlock_irqrestore(&registry_lock, irq);
			break;
		}

		/* A sufficiently large array permits one atomic retained snapshot. */
		if (snapshot != NULL && needed <= capacity) {
			/* Retains every generation while removal remains excluded. */
			device = devices;
			while (device != NULL) {
				snapshot[count] = device;
				cdev_ref(device);
				count++;
				device = device->registry_next;
			}

			spin_unlock_irqrestore(&registry_lock, irq);
			break;
		}

		spin_unlock_irqrestore(&registry_lock, irq);

		/* Frees the unretained array before replacing its storage. */
		kern_free(snapshot);

		/* Rejects an array size that cannot be represented by the allocator. */
		allocation_bytes = (size_t)needed * sizeof(*snapshot);
		if (allocation_bytes / sizeof(*snapshot) != needed)
			return EOVERFLOW;

		/* Allocates enough pointers for the last observed namespace size. */
		snapshot = kern_calloc(needed, sizeof(*snapshot));
		if (snapshot == NULL)
			return ENOMEM;

		/* Remembers the pointer capacity available to the next locked copy. */
		capacity = needed;
	}

	/* Normalizes a namespace emptied during allocation to no array ownership. */
	if (count == 0) {
		kern_free(snapshot);
		snapshot = NULL;
	}

	/* Transfers both the array and all references obtained by its copy. */
	*result = snapshot;
	*count_out = count;

	/* Succeeded: the snapshot has no device-count truncation. */
	return 0;
}

/*
 * Reports the number of published devices.
 */
unsigned
cdev_count(
	void)
{
	unsigned count;
	unsigned long irq;

	/* Samples the count under the registry lock. */
	irq = spin_lock_irqsave(&registry_lock);

	count = device_count;

	spin_unlock_irqrestore(&registry_lock, irq);

	/* Succeeded: reports the current namespace size. */
	return count;
}

/* Finds the device generation behind a file's inode, or none. */
static const struct cdev *
file_cdev(
	struct file *file)
{
	/* A file without an inode has no device. */
	if (file == NULL)
		return NULL;
	if (file->f_inode == NULL)
		return NULL;

	/* Reports the generation the inode carries. */
	return file->f_inode->i_data;
}

/* Opens a file on a device through the device's open operation. */
static int
cdev_open_file(
	struct file *file)
{
	const struct cdev *device;
	int error;

	/* Rejects a missing file. */
	if (file == NULL)
		return ENODEV;

	/* Hands the device data to the file. */
	device = file_cdev(file);
	if (device != NULL)
		file->f_data = device->data;
	else
		file->f_data = NULL;

	/* A file whose device is gone cannot be opened. */
	if (device == NULL)
		return ENODEV;

	/* A device without an open operation opens trivially. */
	if (device->ops->open == NULL)
		return 0;

	/* Opens through the generation retained by the inode. */
	error = device->ops->open(file);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Closes a file on a device through the device's close operation. */
static int
cdev_close_file(
	struct file *file)
{
	const struct cdev *device;
	int error;

	/* Without a device or a close operation there is nothing to do. */
	device = file_cdev(file);
	if (device == NULL)
		return 0;
	if (device->ops->close == NULL)
		return 0;

	/* Closes through the device. */
	error = device->ops->close(file);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Reads from a device through its read operation. */
static ssize_t
cdev_read_file(
	struct file *file,
	void *buffer,
	size_t size)
{
	const struct cdev *device;
	ssize_t result;

	/* A device that is gone or cannot read reports EOPNOTSUPP. */
	device = file_cdev(file);
	if (device == NULL)
		return -EOPNOTSUPP;
	if (device->ops->read == NULL)
		return -EOPNOTSUPP;

	/* Reads through the device. */
	result = device->ops->read(file, buffer, size);

	/* Reports the device's result. */
	return result;
}

/* Writes to a device through its write operation. */
static ssize_t
cdev_write_file(
	struct file *file,
	const void *buffer,
	size_t size)
{
	const struct cdev *device;
	ssize_t result;

	/* A device that is gone or cannot write reports EOPNOTSUPP. */
	device = file_cdev(file);
	if (device == NULL)
		return -EOPNOTSUPP;
	if (device->ops->write == NULL)
		return -EOPNOTSUPP;

	/* Writes through the device. */
	result = device->ops->write(file, buffer, size);

	/* Reports the device's result. */
	return result;
}

/* Forwards an ioctl to a device's ioctl operation. */
static int
cdev_ioctl_file(
	struct file *file,
	unsigned long request,
	uintptr_t argument)
{
	const struct cdev *device;
	int error;

	/* A device that is gone or has no ioctl reports EOPNOTSUPP. */
	device = file_cdev(file);
	if (device == NULL)
		return EOPNOTSUPP;
	if (device->ops->ioctl == NULL)
		return EOPNOTSUPP;

	/* Forwards the request. */
	error = device->ops->ioctl(file, request, argument);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Polls a device through its poll operation. */
static int
cdev_poll_file(
	struct file *file,
	short events,
	short *revents)
{
	const struct cdev *device;
	int error;

	/* Rejects a missing result. */
	device = file_cdev(file);
	if (revents == NULL)
		return EINVAL;

	/* A device that is gone reports an error and hangup. */
	if (device == NULL) {
		*revents = POLLERR | POLLHUP;
		return 0;
	}

	/* A device without a poll operation is never ready. */
	if (device->ops->poll == NULL) {
		*revents = 0;
		return 0;
	}

	/* Polls through the device. */
	error = device->ops->poll(file, events, revents);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Returns a retained device view from this open generation's backend. */
static int
cdev_mmap_file(
	struct file *file,
	off_t offset,
	size_t bytes,
	uint32_t prot,
	struct vm_device_mapping **result)
{
	const struct cdev *device;
	int error;

	/* Leaves ownership empty on unsupported or failed requests. */
	if (result == NULL)
		return EINVAL;

	/* Resolves the immutable cdev generation retained by this open file. */
	*result = NULL;
	device = file_cdev(file);
	if (device == NULL || device->ops->mmap == NULL)
		return EOPNOTSUPP;

	/* The backend retains its resource before exposing any physical view. */
	error = device->ops->mmap(file, offset, bytes, prot, result);
	if (error != 0)
		return error;

	/* Succeeded: the caller owns the backend's retained immutable mapping view. */
	return 0;
}

/* Validates one devfs component name. */
static int
cdev_name_valid(
	const char *name)
{
	size_t length;
	const char *slash;

	/* Rejects a missing name. */
	if (name == NULL)
		return 0;

	/* The name must fit the record and contain no slash. */
	length = strlen(name);
	if (length == 0 || length >= sizeof(((struct cdev *)0)->name))
		return 0;

	/* A registry name must remain a single devfs path component. */
	slash = strchr(name, '/');
	if (slash != NULL)
		return 0;

	/* Succeeded: the name fits one devfs component. */
	return 1;
}
