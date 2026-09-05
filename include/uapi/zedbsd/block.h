/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_UAPI_BLOCK_H
#define ZEDBSD_UAPI_BLOCK_H

#include <stdint.h>
#include <stddef.h>
#include <sys/ioctl.h>

#define ZEDBSD_BLOCK_VERSION 1U
#define ZEDBSD_BLOCK_NAME_MAX 32U
#define ZEDBSD_BLOCK_READ_ONLY 1U
#define ZEDBSD_BLOCK_REMOVABLE 2U
#define ZEDBSD_BLOCK_PARTITION 4U

/* Identity is a registration number, not an on-disk GUID. */
struct zedbsd_block_info {
	uint32_t version, struct_size;
	uint32_t device, parent_device;
	uint32_t flags, sector_size;
	uint64_t sector_count, parent_offset;
	char name[ZEDBSD_BLOCK_NAME_MAX];
	uint32_t reserved[4];
};

_Static_assert(sizeof(struct zedbsd_block_info) == 88U, "block ABI size");
_Static_assert(offsetof(struct zedbsd_block_info, sector_count) == 24U,
    "block ABI alignment");
#define BLKGETINFO _IOWR('B', 2, struct zedbsd_block_info)
/* Privileged, synchronous, whole disk only; any mounted child -> EBUSY.
 * Userspace must fsync table writes first. No argument and no force mode. */
#define BLKREREADPART _IO('B', 3)

#endif
