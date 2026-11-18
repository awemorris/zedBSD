/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * BCM43455: reading the package's three files from the root file system.
 *
 * The only part of the driver that touches the file system.  The caller
 * holds no lock of the driver: opening and reading take the file system's
 * own locks, which rank below the driver's.  The host test replaces this
 * file with one that hands it the files' bytes.
 */

#include "drivers/wifi/bcm43455/bcm43455-firmware.h"
#include <kern/file.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/mount.h>
#include <kern/vfs.h>
#include <uapi/errno.h>
#include <uapi/fcntl.h>

static int read_exact(const char *path, size_t size, uint8_t **bytes);
static int read_open_file(struct file *file, size_t size, uint8_t **bytes);

/*
 * Reads the three files and checks them.
 */
int
bcm43455_files_load(
	struct bcm43455_files *files)
{
	char version[BCM43455_VERSION_TEXT_MAX];
	int error;

	/* Refuses nowhere to put the files, and starts from none. */
	if (files == NULL)
		return EINVAL;
	kern_memset(files, 0, sizeof(*files));

	/*
	 * Reads the firmware, whose absence means the package is not installed.
	 */
	error = read_exact(BCM43455_FIRMWARE_PATH, BCM43455_FIRMWARE_SIZE, &files->firmware);
	if (error != 0)
		goto fail;
	files->firmware_length = BCM43455_FIRMWARE_SIZE;

	/* Reads the regulatory table. */
	error = read_exact(BCM43455_CLM_PATH, BCM43455_CLM_SIZE, &files->clm);
	if (error != 0)
		goto fail;
	files->clm_length = BCM43455_CLM_SIZE;

	/* Reads the board's settings. */
	error = read_exact(BCM43455_NVRAM_PATH, BCM43455_NVRAM_SIZE, &files->nvram);
	if (error != 0)
		goto fail;
	files->nvram_length = BCM43455_NVRAM_SIZE;

	/* Checks that the firmware is the one the package installs. */
	error = bcm43455_firmware_check(files->firmware, files->firmware_length, version, sizeof(version));
	if (error != 0) {
		kern_logf("bcm43455: %s is not the 43455 C0 firmware\n", BCM43455_FIRMWARE_PATH);
		goto fail;
	}

	/* Checks that the regulatory table is a CLM container. */
	error = bcm43455_clm_check(files->clm, files->clm_length);
	if (error != 0) {
		kern_logf("bcm43455: %s is not a CLM container\n", BCM43455_CLM_PATH);
		goto fail;
	}

	/* Checks that the board's settings are NVRAM text. */
	error = bcm43455_nvram_check(files->nvram, files->nvram_length);
	if (error != 0) {
		kern_logf("bcm43455: %s is not NVRAM text\n", BCM43455_NVRAM_PATH);
		goto fail;
	}

	/* Succeeded: the three files are in memory. */
	return 0;

fail:
	/* Gives back what was read. */
	bcm43455_files_release(files);
	return error;
}

/*
 * Reports whether the root file system is mounted.
 */
int
bcm43455_files_root_ready(void)
{
	struct mount *root;

	/* Takes and drops a reference to the live root, if there is one. */
	root = mount_root_get_ref();
	if (root == NULL)
		return 0;
	mount_release(root);

	/* Succeeded: the root is mounted. */
	return 1;
}

/*
 * Releases the files' buffers.
 */
void
bcm43455_files_release(
	struct bcm43455_files *files)
{
	/* Nothing to release. */
	if (files == NULL)
		return;

	/*
	 * Frees each buffer; the NVRAM's is cleared first, as settings of the
	 * board.
	 */
	if (files->firmware != NULL)
		kern_free(files->firmware);
	if (files->clm != NULL)
		kern_free(files->clm);
	if (files->nvram != NULL) {
		kern_memset(files->nvram, 0, files->nvram_length);
		kern_free(files->nvram);
	}

	/* Leaves the description empty. */
	kern_memset(files, 0, sizeof(*files));
}

/*
 * Reads a file that must be exactly size bytes into a new buffer.
 * ENOENT for an absent file, EINVAL for another size.
 */
static int
read_exact(
	const char *path,
	size_t size,
	uint8_t **bytes)
{
	struct file *file;
	int close_error;
	int error;

	/* Opens the file without following a link. */
	file = NULL;
	error = file_openat(&kern_cwdinfo, path, O_RDONLY | O_NOFOLLOW, 0, &file);
	if (error != 0) {
		kern_logf("bcm43455: %s not opened (%d)\n", path, error);
		return error;
	}

	/* Reads it whole, then closes it whatever the read reported. */
	error = read_open_file(file, size, bytes);
	close_error = file_close(file);
	if (error != 0) {
		kern_logf("bcm43455: %s not read (%d)\n", path, error);
		return error;
	}

	/* A failed close refuses the bytes, as any failed step does. */
	if (close_error != 0) {
		kern_free(*bytes);
		*bytes = NULL;
		return close_error;
	}

	/* Succeeded: *bytes holds the file. */
	return 0;
}

/*
 * Reads an open file of exactly size bytes into a new buffer.
 */
static int
read_open_file(
	struct file *file,
	size_t size,
	uint8_t **bytes)
{
	struct file_content_lease lease;
	uint8_t *buffer;
	size_t offset;
	ssize_t count;
	int error;

	/* Takes the content lease that fixes the size the read sees. */
	kern_memset(&lease, 0, sizeof(lease));
	error = file_content_lease_begin(file, &lease);
	if (error != 0)
		return error;

	/*
	 * Refuses a file of another size: another version, not checked with
	 * this driver.
	 */
	buffer = NULL;
	if (lease.size < 0 || (uint64_t)lease.size != (uint64_t)size) {
		error = EINVAL;
		goto out;
	}

	/* Allocates the buffer the file is handed back in. */
	buffer = kern_malloc(size);
	if (buffer == NULL) {
		error = ENOMEM;
		goto out;
	}

	/* Reads until the whole file is in the buffer. */
	offset = 0;
	while (offset < size) {
		/* Reads the next part. */
		count = file_content_lease_pread(&lease, buffer + offset, size - offset, (off_t)offset);
		if (count < 0) {
			error = (int)-count;
			goto out;
		}

		/*
		 * A file that ends early, or a read larger than asked, is not
		 * the file the lease measured.
		 */
		if (count == 0 || (size_t)count > size - offset) {
			error = EIO;
			goto out;
		}

		/* Moves past the part read. */
		offset += (size_t)count;
	}

out:
	/* Ends the lease whatever the read reported. */
	file_content_lease_end(&lease);

	/* A refused read gives its buffer back. */
	if (error != 0) {
		if (buffer != NULL)
			kern_free(buffer);
		return error;
	}

	/* Succeeded: *bytes holds the file. */
	*bytes = buffer;
	return 0;
}
