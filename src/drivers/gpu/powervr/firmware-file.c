/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Reading the PowerVR firmware file from the root file system.
 *
 * The core is found while the board brings its devices up, before any
 * file system is mounted, so the read waits, for a bounded time, for the
 * root to become live.  The file is read whole into kernel memory and
 * kept there by the caller.
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

#include "drivers/gpu/powervr/firmware-file.h"

/* How long the read waits for the root file system (60 s), and each step of the wait (100 ms). */
#define ROOT_WAIT_TICKS		kern_ms_to_ticks(60000U)
#define ROOT_POLL_TICKS		kern_ms_to_ticks(100U)

/*
 * The file being read.
 *
 * The content lease fixes the size the reads see; the file stays open
 * until the close.
 */
struct open_firmware {
	struct file *file;
	struct file_content_lease lease;
	size_t size;
};

static int open_file(const char *path, struct open_firmware *open);
static void close_file(struct open_firmware *open);
static int read_whole(struct open_firmware *open, uint8_t *buffer, size_t length);
static struct mount *wait_root(void);

/*
 * Reads a firmware file whole into kernel memory.
 *
 * ETIMEDOUT means the root file system did not come up in time; ENOENT
 * that the file is absent; EFBIG that it is empty or past the limit.
 * On success the caller owns *bytes and frees it with kern_free.
 */
int
powervr_firmware_read(
	const char *path,
	size_t limit,
	uint8_t **bytes,
	size_t *size)
{
	struct open_firmware open;
	uint8_t *buffer;
	int error;

	/* Starts with nothing, so a failure hands back no bytes. */
	*bytes = NULL;
	*size = 0;

	/* Opens the file once the root is live. */
	error = open_file(path, &open);
	if (error != 0)
		return error;

	/* Refuses an empty file and one past the limit. */
	if (open.size == 0U || open.size > limit) {
		close_file(&open);
		return EFBIG;
	}

	/* Allocates the buffer the file is handed back in. */
	buffer = kern_malloc(open.size);
	if (buffer == NULL) {
		close_file(&open);
		return ENOMEM;
	}

	/* Reads the whole file, then closes it. */
	error = read_whole(&open, buffer, open.size);
	close_file(&open);
	if (error != 0) {
		kern_free(buffer);
		return error;
	}

	/* Succeeded: the caller owns the bytes. */
	*bytes = buffer;
	*size = open.size;
	return 0;
}

/* Opens a file of the root file system, with its size fixed. */
static int
open_file(
	const char *path,
	struct open_firmware *open)
{
	struct mount *root;
	struct cwdinfo context;
	struct path root_path;
	int error;

	/* Waits for the root file system the file lives on. */
	root = wait_root();
	if (root == NULL)
		return ETIMEDOUT;

	/* Roots a working directory of the driver's own at the mount. */
	path_init(&root_path);
	path_set(&root_path, root, root->m_root);
	error = cwdinfo_init(&context, &root_path);
	path_release(&root_path);
	mount_release(root);
	if (error != 0)
		return error;

	/* Opens the file; an absent one is ENOENT. */
	open->file = NULL;
	error = file_openat(&context, path, O_RDONLY, 0, &open->file);
	cwdinfo_destroy(&context);
	if (error != 0)
		return error;

	/* Takes the lease that fixes the size the reads see. */
	kern_memset(&open->lease, 0, sizeof(open->lease));
	error = file_content_lease_begin(open->file, &open->lease);
	if (error != 0) {
		(void)file_close(open->file);
		return error;
	}

	/* A negative size is not a file to read. */
	if (open->lease.size < 0) {
		file_content_lease_end(&open->lease);
		(void)file_close(open->file);
		return EINVAL;
	}

	/* Succeeded: the file is open with its size fixed. */
	open->size = (size_t)open->lease.size;
	return 0;
}

/* Ends the lease and closes the file. */
static void
close_file(
	struct open_firmware *open)
{
	/* Ends the lease, then closes the file. */
	file_content_lease_end(&open->lease);
	(void)file_close(open->file);
	open->file = NULL;
}

/* Reads the first length bytes of an open file, refusing a short read. */
static int
read_whole(
	struct open_firmware *open,
	uint8_t *buffer,
	size_t length)
{
	size_t done;
	ssize_t count;

	/* Reads until the whole range is in the buffer. */
	done = 0;
	while (done < length) {
		/* Reads the next part. */
		count = file_content_lease_pread(&open->lease, buffer + done, length - done, (off_t)done);
		if (count < 0)
			return (int)-count;

		/* A read that returns nothing, or more than asked, is not the file the lease measured. */
		if (count == 0 || (size_t)count > length - done)
			return EIO;

		/* Moves past what was read. */
		done += (size_t)count;
	}

	/* Succeeded: the range is in the buffer. */
	return 0;
}

/* Takes a reference to the live root file system, waiting up to the bound; NULL when none came. */
static struct mount *
wait_root(void)
{
	struct mount *root;
	uint64_t deadline;
	uint64_t now;

	/* Sleeps in short steps until the root is live or the bound is spent. */
	deadline = sched_ticks() + ROOT_WAIT_TICKS;
	for (;;) {
		/* Stops as soon as the kernel has a live root. */
		root = mount_root_get_ref();
		if (root != NULL)
			break;

		/* The bound is spent without a root. */
		now = sched_ticks();
		if (now >= deadline)
			return NULL;

		/* Sleeps one step; the later stage's thread may sleep. */
		sched_sleep(now + ROOT_POLL_TICKS);
	}

	/* Succeeded: the caller holds a reference to the root. */
	return root;
}
