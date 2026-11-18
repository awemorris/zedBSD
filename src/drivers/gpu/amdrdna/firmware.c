/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The firmware files of the amdrdna driver.
 *
 * The files live below /lib/firmware/amdgpu/ (the amd-navi-firmware
 * package).  Each one is read whole into kernel memory; the largest is
 * about 270 KB.  A read waits for the root file system, so it runs on the
 * bring-up thread.
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

#include "drivers/gpu/amdrdna/amdrdna-private.h"

/* The directory the files are read from. */
#define FIRMWARE_DIRECTORY		"/lib/firmware/amdgpu/"

/* The longest path a read builds, with its terminating zero. */
#define FIRMWARE_PATH_BYTES		128U

/* How long a read waits for the root file system (60 s), and each step of the wait (100 ms). */
#define FIRMWARE_ROOT_WAIT_TICKS	kern_ms_to_ticks(60000U)
#define FIRMWARE_ROOT_POLL_TICKS	kern_ms_to_ticks(100U)

/*
 * One firmware file open for reading.
 *
 * The content lease fixes the size the reads see; the file stays open until
 * the close.
 */
struct firmware_file {
	struct file *file;
	struct file_content_lease lease;
	size_t size;
};

static int firmware_open(const char *name, struct firmware_file *open);
static void firmware_close(struct firmware_file *open);
static int firmware_pread(struct firmware_file *open, uint8_t *buffer, size_t length);
static struct mount *firmware_root_wait(void);

/*
 * Reads one firmware file whole into kernel memory.
 *
 * Returns 0 with the bytes in blob (the caller frees them with
 * amdrdna_firmware_free), ENOENT when the file is absent (the firmware
 * package is not installed), or EFBIG when it is empty or larger than the
 * limit.
 */
int
amdrdna_firmware_read(
	const char *name,
	size_t limit,
	struct amdrdna_blob *blob)
{
	struct firmware_file open;
	uint8_t *data;
	int error;

	/* Starts with nothing, so a failure hands back no bytes. */
	blob->data = NULL;
	blob->size = 0U;

	/* Opens the file. */
	error = firmware_open(name, &open);
	if (error != 0)
		return error;

	/* Refuses an empty file and one past the limit. */
	if (open.size == 0U || open.size > limit) {
		firmware_close(&open);
		return EFBIG;
	}

	/* Allocates the buffer the file is handed back in. */
	data = kern_malloc(open.size);
	if (data == NULL) {
		firmware_close(&open);
		return ENOMEM;
	}

	/* Reads the whole file, then closes it. */
	error = firmware_pread(&open, data, open.size);
	firmware_close(&open);
	if (error != 0) {
		kern_free(data);
		return error;
	}

	/* Succeeded: the caller owns the bytes until it frees them. */
	blob->data = data;
	blob->size = open.size;
	return 0;
}

/*
 * Frees a file read whole.
 */
void
amdrdna_firmware_free(
	struct amdrdna_blob *blob)
{
	/* A blob that never loaded holds nothing. */
	if (blob->data != NULL)
		kern_free(blob->data);

	/* Forgets the bytes. */
	blob->data = NULL;
	blob->size = 0U;
}

/* Opens a file of the firmware directory on the root file system, with its size fixed. */
static int
firmware_open(
	const char *name,
	struct firmware_file *open)
{
	char path[FIRMWARE_PATH_BYTES];
	struct mount *root;
	struct cwdinfo context;
	struct path root_path;
	size_t directory_length;
	size_t name_length;
	int error;

	/* Joins the directory and the name, refusing a path that does not fit. */
	directory_length = kern_strlen(FIRMWARE_DIRECTORY);
	name_length = kern_strlen(name);
	if (name_length >= FIRMWARE_PATH_BYTES - directory_length)
		return ENAMETOOLONG;

	/* Builds the path: the directory, then the name with its zero. */
	kern_memcpy(path, FIRMWARE_DIRECTORY, directory_length);
	kern_memcpy(path + directory_length, name, name_length + 1U);

	/* Waits for the root file system the file lives on. */
	root = firmware_root_wait();
	if (root == NULL)
		return ENOENT;

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
firmware_close(
	struct firmware_file *open)
{
	/* Ends the lease, then closes the file. */
	file_content_lease_end(&open->lease);
	(void)file_close(open->file);
	open->file = NULL;
}

/* Reads the first length bytes of an open file whole, refusing a short read. */
static int
firmware_pread(
	struct firmware_file *open,
	uint8_t *buffer,
	size_t length)
{
	size_t done;
	ssize_t count;

	/* The range must lie inside the file. */
	if (length > open->size)
		return EINVAL;

	/* Reads until the whole range is in the buffer. */
	done = 0U;
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
firmware_root_wait(void)
{
	struct mount *root;
	uint64_t deadline;
	uint64_t now;

	/* Sleeps in short steps until the root is live or the bound is spent. */
	deadline = sched_ticks() + FIRMWARE_ROOT_WAIT_TICKS;
	for (;;) {
		/* Stops as soon as the kernel has a live root. */
		root = mount_root_get_ref();
		if (root != NULL)
			break;

		/* The bound is spent without a root. */
		now = sched_ticks();
		if (now >= deadline)
			return NULL;

		/* Sleeps one step; the bring-up thread may sleep. */
		sched_sleep(now + FIRMWARE_ROOT_POLL_TICKS);
	}

	/* Succeeded: the caller holds a reference to the root. */
	return root;
}
