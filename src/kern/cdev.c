/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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

static int cdev_name_valid(const char *name);

/* Unpublishes every character device without invalidating retained refs. */
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

/* Publishes one legacy device and relinquishes the temporary owner ref. */
int
cdev_register(
	const char *name,
	dev_t rdev,
	const struct cdev_ops *ops,
	void *data)
{
	struct cdev *device;
	int error;

	error = cdev_register_managed(name, rdev, ops, data, NULL, &device);
	if (error != 0)
		return error;

	cdev_release(device);
	return 0;
}

/* Publishes one immutable managed device generation. */
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
	return 0;
}

/* Unpublishes exactly the supplied device generation. */
int
cdev_unregister(
	struct cdev *device)
{
	unsigned index;
	unsigned move;
	unsigned long irq;
	int found;

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

	if (!found)
		return ENOENT;

	/* Releases registry ownership after the namespace is invalidated. */
	cdev_release(device);
	return 0;
}

/* Retains one immutable device generation. */
void
cdev_ref(
	struct cdev *device)
{
	if (device != NULL)
		refcount_get(&device->refs);
}

/* Releases one generation and runs its terminal data finalizer once. */
void
cdev_release(
	struct cdev *device)
{
	cdev_finalizer_t finalizer;
	void *data;

	if (device == NULL)
		return;
	if (!refcount_put(&device->refs))
		return;

	finalizer = device->finalizer;
	data = device->data;
	if (finalizer != NULL)
		finalizer(data);
	kern_free(device);
}

/* Reports whether the exact generation remains in the visible registry. */
int
cdev_is_published(
	const struct cdev *device)
{
	if (device == NULL)
		return 0;

	return atomic_load_acquire(&device->published) != 0;
}

/* Returns the immutable identifier assigned at publication. */
uint64_t
cdev_generation(
	const struct cdev *device)
{
	if (device == NULL)
		return 0;

	return device->generation;
}

/* Finds and retains the currently published generation for one name. */
struct cdev *
cdev_find_ref(
	const char *name)
{
	struct cdev *device;
	unsigned index;
	unsigned long irq;

	if (name == NULL)
		return NULL;

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

	return device;
}

/* Retains one coherent snapshot of all currently published generations. */
unsigned
cdev_snapshot(
	struct cdev **snapshot,
	unsigned capacity)
{
	unsigned count;
	unsigned index;
	unsigned long irq;

	if (snapshot == NULL || capacity == 0)
		return 0;

	irq = spin_lock_irqsave(&registry_lock);
	count = device_count < capacity ? device_count : capacity;
	for (index = 0; index < count; index++) {
		snapshot[index] = devices[index];
		cdev_ref(snapshot[index]);
	}
	spin_unlock_irqrestore(&registry_lock, irq);

	return count;
}

unsigned
cdev_count(
	void)
{
	unsigned count;
	unsigned long irq;

	irq = spin_lock_irqsave(&registry_lock);
	count = device_count;
	spin_unlock_irqrestore(&registry_lock, irq);
	return count;
}

static const struct cdev *file_cdev(struct file *file)
{
	return file != NULL && file->f_inode != NULL ? file->f_inode->i_data : NULL;
}

static int cdev_open_file(struct file *file)
{
	const struct cdev *device;

	if (file == NULL)
		return ENODEV;
	device = file_cdev(file);
	file->f_data = device != NULL ? device->data : NULL;
	return device == NULL ? ENODEV :
		device->ops->open != NULL ? device->ops->open(file) : 0;
}

static int cdev_close_file(struct file *file)
{
	const struct cdev *device = file_cdev(file);
	return device != NULL && device->ops->close != NULL ?
		device->ops->close(file) : 0;
}

static ssize_t cdev_read_file(struct file *file, void *buffer, size_t size)
{
	const struct cdev *device = file_cdev(file);
	return device != NULL && device->ops->read != NULL ?
		device->ops->read(file, buffer, size) : -EOPNOTSUPP;
}

static ssize_t cdev_write_file(struct file *file, const void *buffer,
			       size_t size)
{
	const struct cdev *device = file_cdev(file);
	return device != NULL && device->ops->write != NULL ?
		device->ops->write(file, buffer, size) : -EOPNOTSUPP;
}

static int cdev_ioctl_file(struct file *file, unsigned long request,
			   uintptr_t argument)
{
	const struct cdev *device = file_cdev(file);
	return device != NULL && device->ops->ioctl != NULL ?
		device->ops->ioctl(file, request, argument) : EOPNOTSUPP;
}

static int cdev_poll_file(struct file *file, short events, short *revents)
{
	const struct cdev *device = file_cdev(file);
	if (revents == NULL)
		return EINVAL;
	if (device == NULL) {
		*revents = POLLERR | POLLHUP;
		return 0;
	}
	if (device->ops->poll == NULL) {
		*revents = 0;
		return 0;
	}
	return device->ops->poll(file, events, revents);
}

const struct file_ops cdev_file_ops = {
	.open = cdev_open_file,
	.close = cdev_close_file,
	.read = cdev_read_file,
	.write = cdev_write_file,
	.ioctl = cdev_ioctl_file,
	.poll = cdev_poll_file,
};

/* Validates one devfs component name. */
static int
cdev_name_valid(
	const char *name)
{
	size_t length;

	if (name == NULL)
		return 0;

	length = strlen(name);
	if (length == 0 || length >= sizeof(((struct cdev *)0)->name))
		return 0;
	if (strchr(name, '/') != NULL)
		return 0;

	return 1;
}
