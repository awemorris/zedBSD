/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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
#include "kern/system-device.h"
#include "kern/boot-device.h"
#include "kern/devfs.h"
#include "kern/swap-fat.h"

#include <errno.h>
#include <hal/hal.h>
#include <string.h>

#define PHYSICAL_DISK_MAX 4U

struct cwdinfo kern_cwdinfo __attribute__((section(".vfs_bss")));

static int
vfs_fail(const char *stage, int error)
{
	hal_printf("vfs: %s failed (error %d)\n", stage, error);
	return error;
}

static int
disk_name(unsigned number, char name[NAME_MAX + 1U])
{
	unsigned at = 4;
	if (number == 0 || number > 99)
		return EINVAL;
	memcpy(name, "disk", 4);
	if (number >= 10)
		name[at++] = (char)('0' + number / 10);
	name[at++] = (char)('0' + number % 10);
	name[at] = '\0';
	return 0;
}

int
kern_vfs_init(const struct zedbsd_handoff *handoff,
	      const struct zedbsd_device *devices, unsigned device_count)
{
	struct disk *physical[PHYSICAL_DISK_MAX];
	struct disk *boot_physical = NULL;
	struct disk *boot_partition = NULL;
	struct mount *boot_mount;
	struct path root_path;
	const char *failure_stage = "initialize root cwd";
	unsigned physical_count = 0, next_number = 2, i;
	int error;

	if (handoff == NULL)
		return vfs_fail("handoff", EINVAL);
	if (handoff->version == ZEDBSD_HANDOFF_VERSION_MULTIBOOT)
		hal_printf("vfs: boot BIOS=%02X MBR partition=%u devices=%u\n",
		    handoff->boot_bios_id, handoff->boot_partition_index,
		    device_count);
	else
		hal_printf("vfs: boot BIOS=%02X partition LBA=%u devices=%u\n",
		    handoff->boot_bios_id, handoff->boot_partition_lba,
		    device_count);
	for (i = 0; i < device_count; i++)
		if (devices[i].bios_id == handoff->boot_bios_id) {
			boot_physical = kern_platform_block_device(&devices[i]);
			break;
		}
	for (i = 0; i < disk_count() && physical_count < PHYSICAL_DISK_MAX; i++) {
		struct disk *disk = disk_at(i);
		if (disk != NULL && !(disk->d_flags & DISK_PARTITION))
			physical[physical_count++] = disk;
	}
	hal_printf("vfs: native boot disk=%s physical disks=%u\n",
	    boot_physical != NULL ? boot_physical->d_name : "none",
	    physical_count);
	mount_reset();
	cdev_reset();
	partition_reset();
	error = filesystem_register(&fat_filesystem_type);
	if (error != 0)
		return vfs_fail("register FAT", error);
	error = filesystem_register(&devfs_type);
	if (error != 0)
		return vfs_fail("register devfs", error);
	error = console_device_register();
	if (error != 0)
		return vfs_fail("register console", error);
	error = graphics_device_register();
	if (error != 0)
		return vfs_fail("register graphics", error);
	error = system_device_register();
	if (error != 0)
		return vfs_fail("register system", error);
	error = boot_device_register();
	if (error != 0)
		return vfs_fail("register boot device", error);

	for (i = 0; i < physical_count; i++) {
		struct partition entries[PARTITION_MAX];
		struct disk_geometry geometry;
		int geometry_error = disk_ioctl(physical[i],
		    DISK_IOCTL_GET_GEOMETRY, &geometry);
		int count = partition_scan(physical[i], entries, PARTITION_MAX);
		int slot;
		if (geometry_error == 0)
			hal_printf("vfs: scan %s H/S=%u/%u blocks=%u: %d entries\n",
			    physical[i]->d_name, geometry.heads,
			    geometry.sectors_per_track,
			    (uint32_t)physical[i]->d_block_count, count);
		else
			hal_printf("vfs: scan %s geometry error=%d: %d entries\n",
			    physical[i]->d_name, geometry_error, count);
		if (count < 0)
			continue;
		for (slot = 0; slot < count; slot++) {
			if (entries[slot].p_block_count == 0 ||
			    partition_create_disk(&entries[slot]) != 0)
				continue;
			hal_printf("vfs: %s partition %u start=%u data=%u blocks=%u\n",
			    physical[i]->d_name, (unsigned)slot + 1U,
			    (uint32_t)entries[slot].p_start_block,
			    (uint32_t)entries[slot].p_data_block,
			    (uint32_t)entries[slot].p_block_count);
			if (physical[i] == boot_physical &&
			    ((handoff->version ==
			      ZEDBSD_HANDOFF_VERSION_MULTIBOOT &&
			      handoff->boot_partition_scheme ==
			      ZEDBSD_PARTITION_SCHEME_MBR &&
			      entries[slot].p_index + 1U ==
			      handoff->boot_partition_index) ||
			     (handoff->version ==
			      ZEDBSD_HANDOFF_VERSION_PC98 &&
			      entries[slot].p_start_block ==
			      handoff->boot_partition_lba)))
				boot_partition = entries[slot].p_disk;
		}
	}
	if (boot_partition == NULL)
		return vfs_fail("find boot partition", ENXIO);
	{
		struct fat_mount_args args = { boot_partition->d_name };
		error = mount_root_create("auto", 0, &args, &boot_mount);
	}
	if (error != 0)
		return vfs_fail("mount boot FAT", error);
	path_set(&root_path, boot_mount, boot_mount->m_root);
	error = cwdinfo_init(&kern_cwdinfo, &root_path);
	if (error != 0)
		goto out_root;
	process_attach_boot_cwd(&kern_cwdinfo);
	failure_stage = "bind /disk1";
	error = mount_bind_at(&root_path, &root_path, "disk1", NULL);
	if (error != 0)
		goto out_root;
	failure_stage = "mount /dev";
	error = mount_at("devfs", &root_path, "dev", 0, NULL, NULL);
	if (error != 0)
		goto out_root;
	for (i = 0; i < partition_count(); i++) {
		const struct partition *partition = partition_at(i);
		struct fat_mount_args args;
		char name[NAME_MAX + 1U];
		if (partition == NULL || partition->p_disk == NULL ||
		    partition->p_disk == boot_partition ||
		    disk_name(next_number, name) != 0)
			continue;
		args.fspec = partition->p_disk->d_name;
		error = mount_at("auto", &root_path, name, 0, &args, NULL);
		if (error == 0)
			next_number++;
	}
	error = swap_fat_activate(&kern_cwdinfo, "/");
	if (error != 0 && error != ENOENT)
		hal_printf("swap: /swapfile disabled (%d)\n", error);
	path_release(&root_path);
	return 0;

out_root:
	path_release(&root_path);
	return vfs_fail(failure_stage, error);
}
