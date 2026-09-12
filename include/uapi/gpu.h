/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Fixed-width requests for the first GPU framework interface.
 */

#ifndef KERN_UAPI_GPU_H
#define KERN_UAPI_GPU_H

#include <stdint.h>
#include <sys/ioctl.h>

#define GPU_ABI_VERSION			1U
#define GPU_CAP_RESOURCE		1U
#define GPU_RESOURCE_USAGE_STORAGE	1U
#define GPU_SESSION_RESOURCE_MAX	32U

#define GPU_GET_INFO			_IOWR('G', 0, struct gpu_info)
#define GPU_RESOURCE_CREATE		_IOWR('G', 1, struct gpu_resource_create)
#define GPU_RESOURCE_DESTROY		_IOW('G', 2, struct gpu_resource_destroy)

/*
 * One capability snapshot, unsupported rendering and mapping are absent.
 */
struct gpu_info {
	uint32_t version;
	uint32_t size;
	uint32_t capabilities;
	uint32_t max_resources;
	uint64_t max_resource_bytes;
	char driver_name[32];
};

/*
 * One storage allocation whose returned handle belongs to the open session.
 */
struct gpu_resource_create {
	uint32_t version;
	uint32_t size;
	uint64_t bytes;
	uint32_t usage;
	uint32_t flags;
	uint64_t handle;
};

/*
 * One release request, stale or foreign handles never identify a resource.
 */
struct gpu_resource_destroy {
	uint32_t version;
	uint32_t size;
	uint64_t handle;
};

#endif
