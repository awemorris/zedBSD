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
#define GPU_CAP_CAPSET			2U
#define GPU_CAP_BLOB			4U
#define GPU_CAP_TRANSFER		8U
#define GPU_CAP_COMMAND			16U
#define GPU_CAP_PRESENT			32U
#define GPU_BLOB_MAPPABLE		1U
#define GPU_COPY_MAX			65536U
#define GPU_COMMAND_MAX			65536U
#define GPU_CAPSET_MAX			256U
#define GPU_PIXEL_BGRA8888		1U
#define GPU_PIXEL_RGBA8888		2U
#define GPU_RESOURCE_USAGE_STORAGE	1U
#define GPU_SESSION_RESOURCE_MAX	32U

#define GPU_GET_INFO			_IOWR('G', 0, struct gpu_info)
#define GPU_RESOURCE_CREATE		_IOWR('G', 1, struct gpu_resource_create)
#define GPU_RESOURCE_DESTROY		_IOW('G', 2, struct gpu_resource_destroy)
#define GPU_GET_CAPSET			_IOWR('G', 3, struct gpu_capset)
#define GPU_BLOB_CREATE			_IOWR('G', 4, struct gpu_blob_create)
#define GPU_RESOURCE_READ		_IOW('G', 5, struct gpu_transfer)
#define GPU_RESOURCE_WRITE		_IOW('G', 6, struct gpu_transfer)
#define GPU_COMMAND			_IOW('G', 7, struct gpu_command)
#define GPU_PRESENT			_IOW('G', 8, struct gpu_present)

/*
 * One capability snapshot describing supported operations and allocation limits.
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

/* A bounded backend capability payload copied inline without user pointers. */
struct gpu_capset {
	uint32_t version;
	uint32_t size;
	uint32_t capset_id;
	uint32_t capset_version;
	uint32_t capacity;
	uint32_t bytes;
	uint8_t data[GPU_CAPSET_MAX];
};

/* A mapped backend blob whose handle and protocol resource ID belong to a session. */
struct gpu_blob_create {
	uint32_t version;
	uint32_t size;
	uint64_t bytes;
	uint64_t blob_id;
	uint64_t handle;
	uint32_t flags;
	uint32_t resource_id;
};

/* One bounded copy between a session-owned resource and an encoded user address. */
struct gpu_transfer {
	uint32_t version;
	uint32_t size;
	uint64_t handle;
	uint64_t offset;
	uint64_t address;
	uint32_t bytes;
	uint32_t reserved;
};

/* One backend command stream; receipt does not imply rendering completion. */
struct gpu_command {
	uint32_t version;
	uint32_t size;
	uint64_t address;
	uint32_t bytes;
	uint32_t flags;
};

/* One packed four-byte pixel image in an owned storage resource, not a blob. */
struct gpu_present {
	uint32_t version;
	uint32_t size;
	uint64_t handle;
	uint64_t offset;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t format;
	uint64_t frame;
};

#endif
