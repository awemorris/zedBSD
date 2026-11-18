/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the installer may install to, read and never changed: the firmware
 * the system started from, its memory, the disks with their partition
 * tables, each partition's identity and use, and each FAT's room and the
 * files that make it another system's.  One record a line, the fields
 * separated by TABs:
 *
 *   firmware uefi|other
 *   memory   BYTES
 *   disk     NAME REGISTRATION KIND SECTOR-BYTES SECTORS FLAGS LIVE TABLE RESTRICTIONS ENTRIES DIGEST
 *   part     DISK SLOT START SECTORS TYPE PARTUUID ATTRIBUTES DEVICE FSTYPE FSUUID MOUNTED SWAP NAME FSLABEL
 *   fat      DISK SLOT FREE-BYTES CFG BOOT-LOADER ZEDBSD-LOADER
 *   end
 *
 * KIND is nvme, removable or fixed; LIVE is 1 for a disk the running
 * system uses (its root, its boot FAT, its swap, the ESP it started
 * from); TABLE is gpt, mbr or none; DIGEST is the SHA-256 of the table
 * (probe_table_digest), which a request quotes back.  A field with
 * nothing in it is "-"; the names and labels have their TABs and control
 * characters replaced by '?'.  CFG and the loaders are yes, no or unknown
 * (a FAT that cannot be mounted read-only is unknown, which the installer
 * takes as "there").
 */

#ifndef USERLAND_DESKTOP_INSTALLER_ADMIN_PROBE_H
#define USERLAND_DESKTOP_INSTALLER_ADMIN_PROBE_H

#include <uapi/blkid.h>
#include <uapi/block.h>

#include <stdint.h>
#include <stdio.h>

/* The most block devices one probe looks at. */
#define PROBE_DEVICES_MAX	96U

/* Where the helper mounts a FAT read-only to look into it. */
#define PROBE_MOUNT_POINT	"/run/install-admin/probe"

/* One block device: what the kernel says of it and how the system uses it. */
struct probe_device {
	struct kern_block_info info;
	struct block_identity identity;
	int has_identity;
	int mounted;
	int swap;
	int live;
};

/* The block devices, as one scan found them. */
struct probe_scan {
	struct probe_device device[PROBE_DEVICES_MAX];
	unsigned count;
	int uefi;
};

struct dp_table;

int probe_scan_devices(struct probe_scan *scan);
const struct probe_device *probe_find_device(const struct probe_scan *scan, uint32_t registration);
const struct probe_device *probe_find_partition(const struct probe_scan *scan, uint32_t disk, uint64_t start, uint64_t sectors);
int probe_table_load(const struct probe_device *disk, struct dp_table *table);
void probe_table_digest(const struct dp_table *table, uint8_t digest[32]);
int probe_report(FILE *output);

#endif
