/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include <kern/cdev.h>
#include <kern/memory-device.h>
#include <uapi/poll.h>
#include <uapi/errno.h>
#include <kern/file.h>
#include <stdint.h>
#include <kern/kcrt.h>
#include <kern/random.h>

static ssize_t null_read(struct file *file, void *buffer, size_t size);
static ssize_t zero_read(struct file *file, void *buffer, size_t size);
static ssize_t discard_write(struct file *file, const void *buffer, size_t size);
static ssize_t full_write(struct file *file, const void *buffer, size_t size);
static ssize_t random_read(struct file *file, void *buffer, size_t size);
static ssize_t urandom_read(struct file *file, void *buffer, size_t size);
static ssize_t random_write(struct file *file, const void *buffer, size_t size);
static int memory_poll(struct file *file, short events, short *revents);
static off_t memory_seek(struct file *file, off_t offset, int whence);

/* Where a move starts from, as lseek names the three origins. */
#define MEMORY_SEEK_SET 0
#define MEMORY_SEEK_CUR 1
#define MEMORY_SEEK_END 2

/* The range a position may take, which follows the width of off_t. */
#define MEMORY_OFF_MAX ((off_t)(sizeof(off_t) == 8 ? INT64_MAX : INT32_MAX))
#define MEMORY_OFF_MIN ((off_t)(-MEMORY_OFF_MAX - 1))

/* Immutable operations shared by every open of the discard device. */
static const struct cdev_ops null_ops = {
	.read = null_read,
	.write = discard_write,
	.poll = memory_poll,
	.seek = memory_seek,
};

/* Immutable operations shared by every open of the zero source. */
static const struct cdev_ops zero_ops = {
	.read = zero_read,
	.flags = CDEV_READ_NEVER_WAITS,
	.write = discard_write,
	.poll = memory_poll,
	.seek = memory_seek,
};

/*
 * The kernel random number generator (src/kern/random.c).  random waits
 * until the generator is seeded, urandom does not; both mix what is written
 * into the pool without crediting it.
 */
static const struct cdev_ops random_ops = {
	.read = random_read,
	.flags = CDEV_READ_NEVER_WAITS,
	.write = random_write,
	.poll = memory_poll,
	.seek = memory_seek,
};

static const struct cdev_ops urandom_ops = {
	.read = urandom_read,
	.flags = CDEV_READ_NEVER_WAITS,
	.write = random_write,
	.poll = memory_poll,
	.seek = memory_seek,
};

/*
 * Immutable operations shared by every open of the full device.
 *
 * It reads as zeros and refuses every write, which is how a program is
 * tested against a filesystem that has run out of room without having to
 * fill one.
 */
static const struct cdev_ops full_ops = {
	.read = zero_read,
	.flags = CDEV_READ_NEVER_WAITS,
	.write = full_write,
	.poll = memory_poll,
	.seek = memory_seek,
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
	struct cdev *full_device;
	struct cdev *random_device;
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

	/* Publishes the full device only after the other two exist. */
	error = cdev_register_managed("full", 0x00010005U, &full_ops,
	    NULL, NULL, &full_device);
	if (error != 0) {
		(void)cdev_unregister(zero_device);
		cdev_release(zero_device);
		(void)cdev_unregister(null_device);
		cdev_release(null_device);
		return error;
	}

	/* The random devices are optional: a failure leaves the others. */
	if (cdev_register_managed("random", 0x00010006U, &random_ops,
	    NULL, NULL, &random_device) == 0)
		cdev_release(random_device);
	if (cdev_register_managed("urandom", 0x00010007U, &urandom_ops,
	    NULL, NULL, &random_device) == 0)
		cdev_release(random_device);

	/* Leaves all three immutable devices owned by the registry. */
	cdev_release(full_device);
	cdev_release(zero_device);
	cdev_release(null_device);
	return 0;
}

/* Reads random bytes once the generator is seeded. */
static ssize_t
random_read(
	struct file *file,
	void *buffer,
	size_t size)
{
	int error;

	(void)file;
	error = kern_random_read(buffer, size, KERN_RANDOM_WAIT);

	/* Succeeded: every byte, or the wait's error. */
	return error != 0 ? -(ssize_t)error : (ssize_t)size;
}

/* Reads random bytes without waiting for the seed. */
static ssize_t
urandom_read(
	struct file *file,
	void *buffer,
	size_t size)
{
	int error;

	(void)file;
	error = kern_random_read(buffer, size, 0);

	/* Succeeded: every byte. */
	return error != 0 ? -(ssize_t)error : (ssize_t)size;
}

/* Mixes written bytes into the pool. */
static ssize_t
random_write(
	struct file *file,
	const void *buffer,
	size_t size)
{
	(void)file;
	kern_random_add(buffer, size);

	/* Succeeded: every byte was taken. */
	return (ssize_t)size;
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
		kern_memset(buffer, 0, size);

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

/*
 * Refuses every write, as a filesystem with no room left does.
 *
 * A zero-length write asks for nothing and is granted, which is what a
 * caller checking for room would expect.
 */
static ssize_t
full_write(
	struct file *file,
	const void *buffer,
	size_t size)
{
	(void)file;
	(void)buffer;

	/* Nothing was asked for, so nothing is refused. */
	if (size == 0)
		return 0;

	/* There is no room, and there never will be. */
	return -ENOSPC;
}

/*
 * Moves the position, which nothing here reads.
 *
 * These devices have no contents, so the position cannot change what a read
 * or a write does.  It is still kept and reported, because a program that
 * has redirected its output to one of them and then asks how far it has got
 * expects an answer: refusing the question is what makes such a program
 * treat the device as a pipe and take a different path.  The end is the
 * start, there being nothing in between.
 */
static off_t
memory_seek(
	struct file *file,
	off_t offset,
	int whence)
{
	off_t base;

	/* Dispatch the selected origin. */
	if (whence == MEMORY_SEEK_SET || whence == MEMORY_SEEK_END)
		base = 0;
	else if (whence == MEMORY_SEEK_CUR)
		base = file->f_offset;
	else
		return -EINVAL;

	/* Refuses a move that would not fit, or would end before the start. */
	if ((offset > 0 && base > MEMORY_OFF_MAX - offset) ||
	    (offset < 0 && base < MEMORY_OFF_MIN - offset))
		return -EOVERFLOW;
	if (base + offset < 0)
		return -EINVAL;
	file->f_offset = base + offset;

	/* Returns the computed result. */
	return file->f_offset;
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
