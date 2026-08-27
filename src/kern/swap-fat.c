/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <kern/swap-fat.h>

#include <kern/mount.h>
#include <kern/namei.h>
#include <kern/swap-source.h>

#include <errno.h>
#include <string.h>

/*
 * Compatibility adapter for the original single FAT /swapfile caller.  New
 * boot code prepares parameter-selected sources through swap-source.h.
 */
int
swap_fat_activate(struct cwdinfo *cwd, const char *mount_path)
{
	static struct kern_swap_source_set legacy_set;
	struct kern_swap_source source;
	struct path path;
	char name[ZEDBSD_PATH_MAX];
	size_t length;
	int error;

	if (cwd == NULL || mount_path == NULL)
		return EINVAL;
	if (legacy_set.active)
		return EBUSY;
	length = strlen(mount_path);
	if (length + sizeof("/swapfile") > sizeof(name))
		return ENAMETOOLONG;
	memcpy(name, mount_path, length);
	memcpy(name + length, "/swapfile", sizeof("/swapfile"));
	path_init(&path);
	error = namei_path_at(cwd, name, &path);
	if (error != 0)
		return error;
	kern_swap_source_init(&source);
	error = kern_swap_source_prepare_file(&path, 0, &source);
	path_release(&path);
	if (error != 0)
		return error;
	kern_swap_source_set_init(&legacy_set);
	error = kern_swap_source_set_add(&legacy_set, &source);
	if (error == 0)
		error = kern_swap_source_set_activate(&legacy_set);
	if (error != 0) {
		kern_swap_source_destroy(&source);
		(void)kern_swap_source_set_abort(&legacy_set);
	}
	return error;
}

unsigned
swap_fat_extent_count(void)
{
	return kern_swap_source_file_extent_count();
}
