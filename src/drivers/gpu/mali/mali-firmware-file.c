/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Reading the Mali command stream firmware from the root file system.
 *
 * The GPU is found while the board brings its devices up, before the
 * root is mounted, so the later stage's thread waits for the root, for a
 * bounded time, and then reads the file whole into kernel memory.  The
 * caller loads the sections from that copy and frees it.
 */

#include <stddef.h>
#include <stdint.h>

#include <kern/clock.h>
#include <kern/file.h>
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <kern/mount.h>
#include <kern/namei.h>
#include <kern/sched.h>
#include <uapi/errno.h>
#include <uapi/fcntl.h>

#include "drivers/gpu/mali/mali-private.h"

/* The longest the read waits for the root (90 s), and how often it looks (every 200 ms). */
#define ROOT_WAIT_MS		90000U
#define ROOT_LOOK_MS		200U

static struct mount *root_when_mounted(void);
static int open_on_root(struct mount *root, const char *path, struct file **file);
static int copy_out(struct file_content_lease *lease, uint8_t *buffer, size_t length);

/*
 * Reads a firmware file of the root file system whole into kernel memory.
 *
 * ETIMEDOUT when the root did not come in time, ENOENT when the file is
 * absent, EFBIG when it is empty or longer than limit.  On success the
 * caller owns *bytes and frees it with kern_free.
 */
int
mali_firmware_read(
	const char *path,
	size_t limit,
	uint8_t **bytes,
	size_t *size)
{
	struct file_content_lease lease;
	struct mount *root;
	struct file *file;
	uint8_t *buffer;
	size_t length;
	int error;

	/* Hands nothing back until the whole file is read. */
	*bytes = NULL;
	*size = 0U;

	/* The root the file lives on. */
	root = root_when_mounted();
	if (root == NULL)
		return ETIMEDOUT;

	/* Opens the file from the root; the reference to the mount is not kept. */
	error = open_on_root(root, path, &file);
	mount_release(root);
	if (error != 0)
		return error;

	/* Fixes the size the reads see. */
	kern_memset(&lease, 0, sizeof(lease));
	error = file_content_lease_begin(file, &lease);
	if (error != 0) {
		(void)file_close(file);
		return error;
	}

	/* Refuses an empty file, a negative size and one past the limit. */
	if (lease.size <= 0 || (uint64_t)lease.size > (uint64_t)limit) {
		file_content_lease_end(&lease);
		(void)file_close(file);
		return EFBIG;
	}

	/* The copy the caller will own. */
	length = (size_t)lease.size;
	buffer = kern_malloc(length);
	if (buffer == NULL) {
		file_content_lease_end(&lease);
		(void)file_close(file);
		return ENOMEM;
	}

	/* Reads it all, then lets the file go whatever happened. */
	error = copy_out(&lease, buffer, length);
	file_content_lease_end(&lease);
	(void)file_close(file);
	if (error != 0) {
		kern_free(buffer);
		return error;
	}

	/* Succeeded: the caller owns the bytes. */
	*bytes = buffer;
	*size = length;
	return 0;
}

/* Takes a reference to the root once it is mounted, looking until the bound; NULL when it never came. */
static struct mount *
root_when_mounted(void)
{
	struct mount *root;
	uint64_t deadline;
	uint64_t now;

	/* Looks, then sleeps, until the root is there or the bound is spent. */
	deadline = sched_ticks() + kern_ms_to_ticks(ROOT_WAIT_MS);
	for (;;) {
		/* A mounted root ends the wait. */
		root = mount_root_get_ref();
		if (root != NULL)
			return root;

		/* The bound is spent. */
		now = sched_ticks();
		if (now >= deadline)
			break;

		/* The later stage's own thread may sleep. */
		sched_sleep(now + kern_ms_to_ticks(ROOT_LOOK_MS));
	}

	/* No root came. */
	return NULL;
}

/* Opens a path for reading, resolved from a mount's root. */
static int
open_on_root(
	struct mount *root,
	const char *path,
	struct file **file)
{
	struct cwdinfo context;
	struct path start;
	int error;

	/* A working directory of the driver's own, at the mount's root. */
	path_init(&start);
	path_set(&start, root, root->m_root);
	error = cwdinfo_init(&context, &start);
	path_release(&start);
	if (error != 0)
		return error;

	/* The file, read only; an absent one is ENOENT. */
	*file = NULL;
	error = file_openat(&context, path, O_RDONLY, 0, file);
	cwdinfo_destroy(&context);
	if (error != 0)
		return error;

	/* Succeeded: the caller closes the file. */
	return 0;
}

/* Copies the first length bytes of a leased file into a buffer; EIO on a short or long read. */
static int
copy_out(
	struct file_content_lease *lease,
	uint8_t *buffer,
	size_t length)
{
	size_t done;
	ssize_t count;

	/* Reads part after part until the buffer is full. */
	done = 0U;
	while (done < length) {
		/* The next part, at its offset. */
		count = file_content_lease_pread(lease, buffer + done, length - done, (off_t)done);
		if (count < 0)
			return (int)-count;

		/* Nothing, or more than asked, is not the file the lease measured. */
		if (count == 0 || (size_t)count > length - done)
			return EIO;

		done += (size_t)count;
	}

	/* Succeeded: the buffer holds the file. */
	return 0;
}
