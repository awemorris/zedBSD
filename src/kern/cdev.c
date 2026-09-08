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
#include <string.h>

static struct cdev *devices[CDEV_MAX] __attribute__((section(".vfs_bss")));
static unsigned device_count __attribute__((section(".vfs_bss")));
static uint64_t next_generation __attribute__((section(".vfs_bss")));

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
static int cdev_name_valid(const char *name);

const struct file_ops cdev_file_ops = {
	.open = cdev_open_file,
	.close = cdev_close_file,
	.read = cdev_read_file,
	.write = cdev_write_file,
	.ioctl = cdev_ioctl_file,
	.poll = cdev_poll_file,
};

/*
 * Unpublishes every character device without invalidating retained refs.
 */
void
cdev_reset(
	void)
{
	struct cdev *retired[CDEV_MAX];
	unsigned count;
	unsigned index;
	unsigned long irq;

	/* Removes the complete visible registry in one locked operation. */
	irq = spin_lock_irqsave(&registry_lock);
	count = device_count;
	for (index = 0; index < count; index++) {
		retired[index] = devices[index];
		devices[index] = NULL;
		atomic_store_release(&retired[index]->published, 0);
	}
	device_count = 0;
	spin_unlock_irqrestore(&registry_lock, irq);

	/* Drops each registry ref after the namespace is atomically empty. */
	for (index = 0; index < count; index++)
		cdev_release(retired[index]);
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

	/* Reports the published device. */
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
	unsigned index;
	unsigned long irq;
	int error;

	/* Rejects a bad name or a missing operation table or result. */
	if (result != NULL)
		*result = NULL;
	if (!cdev_name_valid(name) || ops == NULL || result == NULL)
		return EINVAL;

	/* Allocates the generation before entering the publication lock. */
	device = kern_calloc(1, sizeof(*device));
	if (device == NULL)
		return ENOMEM;
	strcpy(device->name, name);
	device->rdev = rdev;
	device->ops = ops;
	device->data = data;
	device->finalizer = finalizer;
	refcount_init(&device->refs, 2);
	atomic_store_release(&device->published, 0);

	/* Validates uniqueness and assigns the immutable generation at publish. */
	error = 0;
	irq = spin_lock_irqsave(&registry_lock);
	for (index = 0; index < device_count; index++) {
		if (!strcmp(devices[index]->name, name)) {
			error = EEXIST;
			break;
		}
	}
	if (error == 0 && device_count >= CDEV_MAX)
		error = ENOSPC;
	if (error == 0 && next_generation == UINT64_MAX)
		error = EOVERFLOW;
	if (error == 0) {
		next_generation++;
		device->generation = next_generation;
		devices[device_count++] = device;
		atomic_store_release(&device->published, 1);
	}
	spin_unlock_irqrestore(&registry_lock, irq);

	/* A failed publication leaves data and its finalizer with the caller. */
	if (error != 0) {
		kern_free(device);
		return error;
	}

	*result = device;

	/* Reports the published generation. */
	return 0;
}

/*
 * Unpublishes exactly the supplied device generation.
 */
int
cdev_unregister(
	struct cdev *device)
{
	unsigned index;
	unsigned move;
	unsigned long irq;
	int found;

	/* Rejects a missing device. */
	if (device == NULL)
		return EINVAL;

	/* Removes the exact pointer so a same-name generation cannot alias it. */
	found = 0;
	irq = spin_lock_irqsave(&registry_lock);
	for (index = 0; index < device_count; index++) {
		if (devices[index] == device) {
			found = 1;
			break;
		}
	}
	if (found) {
		for (move = index + 1U; move < device_count; move++)
			devices[move - 1U] = devices[move];
		device_count--;
		devices[device_count] = NULL;
		atomic_store_release(&device->published, 0);
	}
	spin_unlock_irqrestore(&registry_lock, irq);

	/* Reports a generation that was not published. */
	if (!found)
		return ENOENT;

	/* Releases registry ownership after the namespace is invalidated. */
	cdev_release(device);

	/* Reports the unpublished generation. */
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

	/* Only the last reference finalizes. */
	if (device == NULL)
		return;
	if (!refcount_put(&device->refs))
		return;

	/* Finalizes the data, then frees the generation. */
	finalizer = device->finalizer;
	data = device->data;
	if (finalizer != NULL)
		finalizer(data);
	kern_free(device);
}

/*
 * Reports whether the exact generation remains in the visible registry.
 */
int
cdev_is_published(
	const struct cdev *device)
{
	/* A missing device is not published. */
	if (device == NULL)
		return 0;

	/* Reads the flag the registry maintains. */
	if (atomic_load_acquire(&device->published) != 0)
		return 1;
	return 0;
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
	unsigned index;
	unsigned long irq;

	/* Rejects a missing name. */
	if (name == NULL)
		return NULL;

	/* References the device with the name under the registry lock. */
	device = NULL;
	irq = spin_lock_irqsave(&registry_lock);
	for (index = 0; index < device_count; index++) {
		if (!strcmp(devices[index]->name, name)) {
			device = devices[index];
			cdev_ref(device);
			break;
		}
	}
	spin_unlock_irqrestore(&registry_lock, irq);

	/* Reports the referenced device, or none. */
	return device;
}

/*
 * Retains one coherent snapshot of all currently published generations.
 */
unsigned
cdev_snapshot(
	struct cdev **snapshot,
	unsigned capacity)
{
	unsigned count;
	unsigned index;
	unsigned long irq;

	/* Rejects a missing or empty snapshot array. */
	if (snapshot == NULL || capacity == 0)
		return 0;

	/* References as many devices as fit, in registry order. */
	irq = spin_lock_irqsave(&registry_lock);
	count = device_count;
	if (count > capacity)
		count = capacity;
	for (index = 0; index < count; index++) {
		snapshot[index] = devices[index];
		cdev_ref(snapshot[index]);
	}
	spin_unlock_irqrestore(&registry_lock, irq);

	/* Reports the number of devices in the snapshot. */
	return count;
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

	/* Reports the sampled count. */
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

	/* Opens through the device. */
	error = device->ops->open(file);

	/* Reports why the device's failed. */
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

	device = file_cdev(file);

	/* Without a device or a close operation there is nothing to do. */
	if (device == NULL)
		return 0;
	if (device->ops->close == NULL)
		return 0;

	/* Closes through the device. */
	error = device->ops->close(file);

	/* Reports why the device's failed. */
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

	device = file_cdev(file);

	/* A device that is gone or cannot read reports EOPNOTSUPP. */
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

	device = file_cdev(file);

	/* A device that is gone or cannot write reports EOPNOTSUPP. */
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

	device = file_cdev(file);

	/* A device that is gone or has no ioctl reports EOPNOTSUPP. */
	if (device == NULL)
		return EOPNOTSUPP;
	if (device->ops->ioctl == NULL)
		return EOPNOTSUPP;

	/* Forwards the request. */
	error = device->ops->ioctl(file, request, argument);

	/* Reports why the device's failed. */
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

	device = file_cdev(file);

	/* Rejects a missing result. */
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

	/* Reports why the device's failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Validates one devfs component name. */
static int
cdev_name_valid(
	const char *name)
{
	size_t length;

	/* Rejects a missing name. */
	if (name == NULL)
		return 0;

	/* The name must fit the record and contain no slash. */
	length = strlen(name);
	if (length == 0 || length >= sizeof(((struct cdev *)0)->name))
		return 0;
	if (strchr(name, '/') != NULL)
		return 0;

	/* Reports a usable name. */
	return 1;
}
