/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef KERN_UAPI_BLOCK_H
#define KERN_UAPI_BLOCK_H

#include <stdint.h>
#include <stddef.h>
#include <sys/ioctl.h>

#define KERN_BLOCK_VERSION	1U
#define KERN_BLOCK_NAME_MAX	32U
#define KERN_BLOCK_READ_ONLY	1U
#define KERN_BLOCK_REMOVABLE	2U
#define KERN_BLOCK_PARTITION	4U
#define KERN_BLOCK_FILE_BACKED	8U

/*
 * Identity is a registration number, not an on-disk GUID.
 */
struct kern_block_info {
	uint32_t version, struct_size;
	uint32_t device, parent_device;
	uint32_t flags, sector_size;
	uint64_t sector_count, parent_offset;
	char name[KERN_BLOCK_NAME_MAX];
	uint32_t reserved[4];
};

/*
 * Privileged, synchronous, whole disk only; any mounted child -> EBUSY.
 * Userspace must fsync table writes first. No argument and no force mode.
 */
#define BLKGETINFO _IOWR('B', 2, struct kern_block_info)

/*
 * Privileged O_RDWR physical disk or direct partition. Input matches
 * BLKGETINFO.  Excludes other opens/mounts and backing users until
 * the final description close. Nonoverlapping siblings remain
 * usable. Dup/fork share the reservation.  No force or explicit
 * unlock. File-backed devices are unsupported.
 */
#define BLKREREADPART _IO('B', 3)
#define BLKRESERVE _IOW('B', 4, struct kern_block_info)

_Static_assert(sizeof(struct kern_block_info) == 88U, "block ABI size");
_Static_assert(offsetof(struct kern_block_info, sector_count) == 24U, "block ABI alignment");

#endif
