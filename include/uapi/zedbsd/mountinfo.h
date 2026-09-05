/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_UAPI_MOUNTINFO_H
#define ZEDBSD_UAPI_MOUNTINFO_H

#include <stdint.h>
#include <stddef.h>
#include <sys/ioctl.h>

#define ZEDBSD_MOUNT_INFO_VERSION 1U
#define ZEDBSD_MOUNT_INFO_MAX 64U
#define ZEDBSD_MOUNT_INFO_PATH_MAX 256U
#define ZEDBSD_MOUNT_INFO_BIND 1U

struct zedbsd_mount_info {
	uint32_t device, flags;
	uint32_t kind, reserved;
	char source[ZEDBSD_MOUNT_INFO_PATH_MAX];
	char target[ZEDBSD_MOUNT_INFO_PATH_MAX];
	char type[16];
};

/* Entries immediately follow this header. size is the header size, capacity
 * is the allocated number of records. ENOSPC returns count, but no records.
 * Membership is captured together; paths use bounded getcwd-style resolution
 * (concurrent renames may cause failure). This is not a mutation reservation. */
struct zedbsd_mount_query {
	uint32_t version, struct_size, capacity, count;
	uint32_t reserved[4];
	struct zedbsd_mount_info entries[];
};

_Static_assert(sizeof(struct zedbsd_mount_query) == 32U, "mount query ABI");
_Static_assert(sizeof(struct zedbsd_mount_info) == 544U, "mount entry ABI");
#define ZEDBSD_SYSTEM_GET_MOUNTS _IOWR('s', 12, struct zedbsd_mount_query)

#endif
