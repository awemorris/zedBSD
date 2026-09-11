/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include <kern/cdev.h>
#include <kern/memory-device.h>
#include <uapi/poll.h>
#include <string.h>

static ssize_t null_read(struct file *file, void *buffer, size_t size);
static ssize_t zero_read(struct file *file, void *buffer, size_t size);
static ssize_t discard_write(struct file *file, const void *buffer, size_t size);
static int memory_poll(struct file *file, short events, short *revents);

/* Immutable operations shared by every open of the discard device. */
static const struct cdev_ops null_ops = {
	.read = null_read,
	.write = discard_write,
	.poll = memory_poll,
};

/* Immutable operations shared by every open of the zero source. */
static const struct cdev_ops zero_ops = {
	.read = zero_read,
	.write = discard_write,
	.poll = memory_poll,
};

/*
 * Publishes the memory pseudo-devices during common VFS initialization.
 */
int
drv_memory_device_register(
	void)
{
	struct cdev *null_device;
	struct cdev *zero_device;
	int error;

	/* Owns a reference so a partial registration can be rolled back. */
	error = cdev_register_managed("null", 0x00010003U, &null_ops,
	    NULL, NULL, &null_device);
	if (error != 0)
		return error;

	/* Publishes zero only after the discard device exists. */
	error = cdev_register_managed("zero", 0x00010004U, &zero_ops,
	    NULL, NULL, &zero_device);
	if (error != 0) {
		(void)cdev_unregister(null_device);
		cdev_release(null_device);
		return error;
	}

	/* Leaves both immutable devices owned by the registry. */
	cdev_release(zero_device);
	cdev_release(null_device);
	return 0;
}

/* Reports immediate end of file without touching the buffer. */
static ssize_t
null_read(
	struct file *file,
	void *buffer,
	size_t size)
{
	(void)file;
	(void)buffer;
	(void)size;

	/* No bytes are supplied by the discard device. */
	return 0;
}

/* Fills the kernel I/O buffer; the syscall layer owns userspace copying. */
static ssize_t
zero_read(
	struct file *file,
	void *buffer,
	size_t size)
{
	(void)file;

	/* A zero-length request does not dereference its buffer. */
	if (size != 0)
		memset(buffer, 0, size);

	/* Supplies every requested byte without retaining state. */
	return (ssize_t)size;
}

/* Consumes bytes without retaining them in either pseudo-device. */
static ssize_t
discard_write(
	struct file *file,
	const void *buffer,
	size_t size)
{
	(void)file;
	(void)buffer;

	/* The syscall layer has already validated the transfer size. */
	return (ssize_t)size;
}

/* Reports that reading or writing can complete immediately. */
static int
memory_poll(
	struct file *file,
	short events,
	short *revents)
{
	(void)file;

	/* EOF is readable, just as an unlimited source of zeros is readable. */
	*revents = events & (POLLIN | POLLOUT | POLLRDNORM | POLLWRNORM);
	return 0;
}
