/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The firmware provider (see firmware.h).
 *
 * A request waits, for a bounded time, until the root file system is live:
 * the device start runs before the kernel mounts the root, and the display
 * probe queues its DMC load while the mount may still be in progress.  The
 * file is then opened through a working directory of the provider's own,
 * rooted at that mount, read whole into a new buffer and closed.  An absent
 * file, or a root that never came, is ENOENT, as request_firmware() reports
 * it; the caller runs without the firmware.
 */

#include "firmware.h"
#include <kern/kcrt.h>

#include <uapi/errno.h>
#include <uapi/fcntl.h>
#include <stddef.h>

#include <kern/file.h>
#include <kern/clock.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/mount.h>
#include <kern/namei.h>
#include <kern/sched.h>

/* The directory every firmware name is looked up in. */
#define I915_FIRMWARE_DIRECTORY		"/lib/firmware/"

/* The longest path a request builds, with its terminating NUL. */
#define I915_FIRMWARE_PATH_BYTES	128U

/*
 * The largest image the provider reads.  The DMC programs are under 100 KiB;
 * the bound only refuses a file that cannot be firmware of this driver.
 */
#define I915_FIRMWARE_MAX_BYTES		(1024U * 1024U)

/* How long a request waits for the root file system, in scheduler ticks (30 s). */
#define I915_FIRMWARE_ROOT_WAIT_TICKS	(30U * KERN_CLOCK_HZ)

/* How long each sleep of that wait lasts, in scheduler ticks (100 ms). */
#define I915_FIRMWARE_ROOT_POLL_TICKS	(KERN_CLOCK_HZ / 10U)

/*
 * Test checkpoints.
 *
 * The test build defines the request checkpoint to serve a request with an
 * image of its own, or to report one as absent, while a unit test runs; it
 * sets *served when it answered, and the file system is not consulted.  The
 * image it serves is its own memory: the request leaves the allocation NULL,
 * so the release does not free it.  It is weak so that a production kernel
 * links without it and the call site is a null test.
 */
extern int drv_i915_firmware_test_request(struct i915_firmware *firmware, const char *name, int *served) __attribute__((weak));

static int i915_firmware_path(char *path, const char *name);
static struct mount *i915_firmware_root_wait(void);
static int i915_firmware_read(struct mount *root, const char *path, uint8_t **image, size_t *size);
static int i915_firmware_read_file(struct file *file, uint8_t **image, size_t *size);

/*
 * Reads a firmware image by name, as request_firmware() does.
 *
 * Returns 0 with the image's data and size filled in, ENOENT with data NULL
 * and size 0 when the file does not exist or no root file system came, or
 * another positive errno when the name is too long or the file cannot be
 * read whole (EFBIG for a file larger than any firmware of the driver).
 */
int
drv_i915_firmware_request(
	struct i915_firmware *firmware,
	const char *name)
{
	char path[I915_FIRMWARE_PATH_BYTES];
	struct mount *root;
	uint8_t *image;
	size_t size;
	int served;
	int error;

	/* Starts with no image, so every failure hands back no bytes. */
	firmware->data = NULL;
	firmware->size = 0U;
	firmware->allocation = NULL;

	/* A test build may serve the request instead of the file system. */
	if (drv_i915_firmware_test_request != NULL) {
		served = 0;
		error = drv_i915_firmware_test_request(firmware, name, &served);
		if (served != 0) {
			/* The test's answer stands: an absent image or an error. */
			if (error != 0)
				return error;

			/* Succeeded: the test handed back an image it keeps owning. */
			return 0;
		}
	}

	/* Names the file under the firmware directory. */
	error = i915_firmware_path(path, name);
	if (error != 0) {
		kern_logf("i915: firmware %s: name too long\n", name);
		return error;
	}

	/* Waits for the root file system the file lives on. */
	root = i915_firmware_root_wait();
	if (root == NULL) {
		kern_logf("i915: firmware %s: no root file system (error %d)\n", path, ENOENT);
		return ENOENT;
	}

	/* Reads the whole file into a buffer of its own. */
	image = NULL;
	size = 0U;
	error = i915_firmware_read(root, path, &image, &size);
	mount_release(root);
	if (error != 0) {
		kern_logf("i915: firmware %s: not loaded (error %d)\n", path, error);
		return error;
	}

	/* Hands back the image; the release frees the buffer. */
	firmware->data = image;
	firmware->size = (unsigned)size;
	firmware->allocation = image;
	kern_logf("i915: firmware %s: loaded (%u bytes)\n", path, firmware->size);

	/* Succeeded: the caller holds the image until the release. */
	return 0;
}

/*
 * Drops a firmware image, as release_firmware() does.
 *
 * The buffer a request read the file into is freed; an image the test build
 * served stays where it is.  A handle whose request failed holds nothing.
 */
void
drv_i915_firmware_release(
	struct i915_firmware *firmware)
{
	/* Frees the buffer the provider read the file into. */
	if (firmware->allocation != NULL)
		kern_free(firmware->allocation);

	/* Forgets the image. */
	firmware->allocation = NULL;
	firmware->data = NULL;
	firmware->size = 0U;
}

/* Builds the path of a firmware name under the firmware directory, or ENAMETOOLONG. */
static int
i915_firmware_path(
	char *path,
	const char *name)
{
	size_t directory_length;
	size_t name_length;

	/* Measures both parts. */
	directory_length = kern_strlen(I915_FIRMWARE_DIRECTORY);
	name_length = kern_strlen(name);

	/* Refuses a path that does not fit with its terminating NUL. */
	if (name_length >= I915_FIRMWARE_PATH_BYTES - directory_length)
		return ENAMETOOLONG;

	/* Joins the directory and the name. */
	kern_memcpy(path, I915_FIRMWARE_DIRECTORY, directory_length);
	kern_memcpy(path + directory_length, name, name_length + 1U);

	/* Succeeded: path names the file. */
	return 0;
}

/* Takes a reference to the live root file system, waiting for it up to the bound; NULL when none came. */
static struct mount *
i915_firmware_root_wait(void)
{
	struct mount *root;
	uint64_t deadline;
	uint64_t now;

	/* Sleeps in short steps until the root is live or the bound is spent. */
	deadline = sched_ticks() + I915_FIRMWARE_ROOT_WAIT_TICKS;
	for (;;) {
		/* Stops as soon as the kernel has a live root. */
		root = mount_root_get_ref();
		if (root != NULL)
			break;

		/* The bound is spent without a root. */
		now = sched_ticks();
		if (now >= deadline)
			return NULL;

		/* Sleeps one step; the request runs on a thread that may sleep. */
		sched_sleep(now + I915_FIRMWARE_ROOT_POLL_TICKS);
	}

	/* Succeeded: the caller holds a reference to the root. */
	return root;
}

/* Opens a file through a working directory rooted at the given mount and reads it whole. */
static int
i915_firmware_read(
	struct mount *root,
	const char *path,
	uint8_t **image,
	size_t *size)
{
	struct cwdinfo context;
	struct path root_path;
	struct file *file;
	int close_error;
	int error;

	/* Roots a working directory of the provider's own at the mount. */
	path_init(&root_path);
	path_set(&root_path, root, root->m_root);
	error = cwdinfo_init(&context, &root_path);
	path_release(&root_path);
	if (error != 0)
		return error;

	/* Opens the file; an absent one is ENOENT. */
	file = NULL;
	error = file_openat(&context, path, O_RDONLY, 0, &file);
	cwdinfo_destroy(&context);
	if (error != 0)
		return error;

	/* Reads the file whole, then closes it whatever the read reported. */
	error = i915_firmware_read_file(file, image, size);
	close_error = file_close(file);

	/* A failed read has already given its buffer back. */
	if (error != 0)
		return error;

	/* A failed close refuses the image, as any failed step of the read does. */
	if (close_error != 0) {
		kern_free(*image);
		*image = NULL;
		*size = 0U;
		return close_error;
	}

	/* Succeeded: *image holds the file. */
	return 0;
}

/* Reads an open file whole into a new buffer, refusing an empty or oversized file and a short read. */
static int
i915_firmware_read_file(
	struct file *file,
	uint8_t **image,
	size_t *size)
{
	struct file_content_lease lease;
	uint8_t *buffer;
	size_t length;
	size_t offset;
	ssize_t count;
	int error;

	/* Takes the content lease that fixes the size the read sees. */
	kern_memset(&lease, 0, sizeof(lease));
	error = file_content_lease_begin(file, &lease);
	if (error != 0)
		return error;

	/* Refuses an empty file and one larger than any firmware the driver loads. */
	buffer = NULL;
	length = 0U;
	if (lease.size <= 0) {
		error = EINVAL;
		goto out;
	}
	if ((uint64_t)lease.size > I915_FIRMWARE_MAX_BYTES) {
		error = EFBIG;
		goto out;
	}

	/* Allocates the buffer the image is handed back in. */
	length = (size_t)lease.size;
	buffer = kern_malloc(length);
	if (buffer == NULL) {
		error = ENOMEM;
		goto out;
	}

	/* Reads until the whole file is in the buffer; a failed or empty read refuses it. */
	offset = 0U;
	while (offset < length) {
		/* Reads the next part of the file. */
		count = file_content_lease_pread(&lease, buffer + offset, length - offset, (off_t)offset);
		if (count < 0) {
			error = (int)-count;
			goto out;
		}

		/* A file that ends early, or a read larger than asked, is not the file the lease measured. */
		if (count == 0 || (size_t)count > length - offset) {
			error = EIO;
			goto out;
		}

		/* Moves past what was read. */
		offset += (size_t)count;
	}

out:
	/* Ends the lease whatever the read reported. */
	file_content_lease_end(&lease);

	/* A refused read gives its buffer back: the caller receives nothing. */
	if (error != 0) {
		if (buffer != NULL)
			kern_free(buffer);

		return error;
	}

	/* Succeeded: the buffer holds the whole file. */
	*image = buffer;
	*size = length;
	return 0;
}
