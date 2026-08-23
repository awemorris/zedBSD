/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */
#ifndef ZEDBSD_UAPI_SNAPSHOT_H
#define ZEDBSD_UAPI_SNAPSHOT_H

#include <stdint.h>

#define ZEDBSD_SNAPSHOT_VERSION	1U
#define ZEDBSD_SNAPSHOT_CREATE	1U
#define ZEDBSD_SNAPSHOT_DELETE	2U
#define ZEDBSD_SNAPSHOT_STATUS	3U
#define ZEDBSD_SNAPSHOT_F_ACTIVE	0x00000001U
#define ZEDBSD_SNAPSHOT_DEVICE_MAX	16U

struct snapshot_control {
	uint32_t size;
	uint32_t version;
	uint32_t command;
	uint32_t flags;
	uint64_t captured_sectors;
	uint64_t capacity_sectors;
	char device[ZEDBSD_SNAPSHOT_DEVICE_MAX];
};

#endif
