/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The legacy single-file FAT swap activation.
 */

#include <kern/swap-fat.h>

#include <kern/mount.h>
#include <kern/namei.h>
#include <kern/swap-source.h>

#include <errno.h>
#include <string.h>

/*
 * Activates the fixed /swapfile below a mount point as the only swap source.
 *
 * Compatibility adapter for the original single FAT /swapfile caller.  New
 * boot code prepares parameter-selected sources through swap-source.h.
 */
int
swap_fat_activate(
	struct cwdinfo *cwd,
	const char *mount_path)
{
	static struct kern_swap_source_set legacy_set;
	struct kern_swap_source source;
	struct path path;
	char name[ZEDBSD_PATH_MAX];
	size_t length;
	int error;

	/* Rejects a missing lookup context or mount path. */
	if (cwd == NULL || mount_path == NULL)
		return EINVAL;

	/* Refuses a second activation while the legacy set is active. */
	if (legacy_set.active)
		return EBUSY;

	/* Builds the fixed swap file path below the mount point. */
	length = strlen(mount_path);
	if (length + sizeof("/swapfile") > sizeof(name))
		return ENAMETOOLONG;
	memcpy(name, mount_path, length);
	memcpy(name + length, "/swapfile", sizeof("/swapfile"));

	/* Looks the swap file up. */
	path_init(&path);
	error = namei_path_at(cwd, name, &path);
	if (error != 0)
		return error;

	/* Prepares the file as a swap source and drops the path. */
	kern_swap_source_init(&source);
	error = kern_swap_source_prepare_file(&path, 0, &source);
	path_release(&path);
	if (error != 0)
		return error;

	/* Activates a set holding only that source. */
	kern_swap_source_set_init(&legacy_set);
	error = kern_swap_source_set_add(&legacy_set, &source);
	if (error == 0)
		error = kern_swap_source_set_activate(&legacy_set);

	/* Tears the source and the set down after a failure. */
	if (error != 0) {
		kern_swap_source_destroy(&source);
		(void)kern_swap_source_set_abort(&legacy_set);
	}

	/* Reports the activation result. */
	return error;
}

/*
 * Reports the number of extents of the active file swap source.
 */
unsigned
swap_fat_extent_count(
	void)
{
	unsigned count;

	/* Asks the swap source layer. */
	count = kern_swap_source_file_extent_count();

	/* Reports the extent count. */
	return count;
}
