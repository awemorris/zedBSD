/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * Disk partition enumeration and boot VFS policy.
 */

#include "kern/vfs.h"
#include "kern/disk.h"
#include "kern/fat-vfs.h"
#include "kern/mount.h"
#include "kern/partition.h"
#include "kern/platform.h"
#include "kern/process.h"
#include "kern/cdev.h"
#include "kern/console-device.h"
#include "kern/graphics-device.h"
#include "kern/devfs.h"

#include <errno.h>
#include <string.h>

#define PHYSICAL_DISK_MAX 4U

struct cwdinfo kern_cwdinfo __attribute__((section(".vfs_bss")));

static int
disk_path(unsigned number, char path[BOOTS_PATH_MAX])
{
	unsigned at = 5;
	if (number == 0 || number > 99)
		return EINVAL;
	memcpy(path, "/disk", 5);
	if (number >= 10)
		path[at++] = (char)('0' + number / 10);
	path[at++] = (char)('0' + number % 10);
	path[at] = '\0';
	return 0;
}

int
kern_vfs_init(const struct boots_handoff *handoff,
	      const struct boots_device *devices, unsigned device_count)
{
	struct disk *physical[PHYSICAL_DISK_MAX];
	struct mount *boot_mount = NULL;
	struct disk *boot_disk = NULL;
	unsigned physical_count = 0, mounted = 0, i;
	int error;

	if (handoff == NULL)
		return EINVAL;
	for (i = 0; i < device_count; i++)
		if (devices[i].bios_id == handoff->boot_bios_id) {
			boot_disk = kern_platform_block_device(&devices[i]);
			break;
		}
	for (i = 0; i < disk_count() && physical_count < PHYSICAL_DISK_MAX; i++) {
		struct disk *disk = disk_at(i);
		if (disk != NULL && !(disk->d_flags & DISK_PARTITION))
			physical[physical_count++] = disk;
	}
	mount_reset();
	cdev_reset();
	partition_reset();
	error = filesystem_register(&fat_filesystem_type);
	if (error != 0)
		return error;
	error = filesystem_register(&devfs_type);
	if (error != 0)
		return error;
	error = console_device_register();
	if (error != 0)
		return error;
	error = graphics_device_register();
	if (error != 0)
		return error;
	error = mount_rootfs();
	if (error != 0)
		return error;
	error = mount("devfs", "/dev", 0, NULL);
	if (error != 0)
		return error;
	error = cwdinfo_init(&kern_cwdinfo, mount_root_inode());
	if (error != 0)
		return error;
	process_attach_boot_cwd(&kern_cwdinfo);
	for (i = 0; i < physical_count; i++) {
		struct partition entries[PARTITION_MAX];
		int count = partition_scan(physical[i], entries, PARTITION_MAX);
		int slot;
		if (count < 0)
			continue;
		for (slot = 0; slot < count; slot++) {
			struct fat_mount_args args;
			char path[BOOTS_PATH_MAX];
			if (entries[slot].p_block_count == 0 ||
			    partition_create_disk(&entries[slot]) != 0)
				continue;
			if (disk_path(mounted + 1U, path) != 0)
				continue;
			args.fspec = entries[slot].p_disk->d_name;
			error = mount("auto", path, 0, &args);
			if (error != 0)
				continue;
			mounted++;
			if (physical[i] == boot_disk &&
			    entries[slot].p_start_block ==
			    handoff->boot_partition_lba)
				boot_mount = mount_find(path);
		}
	}
	if (boot_mount != NULL)
		return fs_chdir(&kern_cwdinfo, boot_mount->m_path);
	return 0;
}
