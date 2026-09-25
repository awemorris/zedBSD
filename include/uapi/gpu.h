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
#include <uapi/ioctl.h>

#define GPU_ABI_VERSION			1U
#define GPU_CAP_RESOURCE		1U
#define GPU_CAP_CAPSET			2U
#define GPU_CAP_BLOB			4U
#define GPU_CAP_TRANSFER		8U
#define GPU_CAP_COMMAND			16U
#define GPU_CAP_PRESENT			32U
#define GPU_CAP_MAPPING			64U
#define GPU_CAP_SHARE			256U
#define GPU_CAP_NOTIFICATION		512U
#define GPU_COMMAND_CONTEXT_FENCE	1U
#define GPU_WAIT_CONSUME			1U
#define GPU_SUBMIT_MAX			64U
#define GPU_BLOB_MAPPABLE		1U
#define GPU_BLOB_SHAREABLE		2U
#define GPU_BLOB_CROSS_DEVICE		4U
#define GPU_IMAGE_LINEAR		1U
#define GPU_HANDLE_CLOEXEC		1U
#define GPU_HANDLE_CLOFORK		2U
#define GPU_COPY_MAX			65536U
#define GPU_COMMAND_MAX			65536U
#define GPU_CAPSET_MAX			256U
#define GPU_PIXEL_BGRA8888		1U
#define GPU_PIXEL_RGBA8888		2U
#define GPU_RESOURCE_USAGE_STORAGE	1U

#define GPU_GET_INFO			_IOWR('G', 0, struct gpu_info)
#define GPU_RESOURCE_CREATE		_IOWR('G', 1, struct gpu_resource_create)
#define GPU_RESOURCE_DESTROY		_IOW('G', 2, struct gpu_resource_destroy)
#define GPU_GET_CAPSET			_IOWR('G', 3, struct gpu_capset)
#define GPU_BLOB_CREATE			_IOWR('G', 4, struct gpu_blob_create)
#define GPU_RESOURCE_READ		_IOW('G', 5, struct gpu_transfer)
#define GPU_RESOURCE_WRITE		_IOW('G', 6, struct gpu_transfer)
#define GPU_COMMAND			_IOW('G', 7, struct gpu_command)
#define GPU_PRESENT			_IOW('G', 8, struct gpu_present)
#define GPU_RESOURCE_MAP		_IOWR('G', 9, struct gpu_resource_map)
#define GPU_RESOURCE_EXPORT		_IOWR('G', 10, struct gpu_resource_export)
#define GPU_RESOURCE_IMPORT		_IOWR('G', 11, struct gpu_resource_import)
#define GPU_COMMAND_SUBMIT		_IOWR('G', 12, struct gpu_command_submit)
#define GPU_COMMAND_WAIT			_IOWR('G', 13, struct gpu_command_wait)

/*
 * One independently retained command and its session-owned completion identity.
 * Context fences select a backend-defined completion domain.
 * Submission success means acceptance; it never substitutes for a Vulkan result.
 */
struct gpu_command_submit {
	uint32_t version;
	uint32_t size;
	uint64_t address;
	uint32_t bytes;
	uint32_t flags;
	uint32_t timeline;
	uint32_t reserved;
	uint64_t sequence;
};

/*
 * One completion observation; status is a positive transport errno or zero.
 * Zero timeout only observes; UINT64_MAX waits until completion or interruption.
 * CONSUME retires a terminal record only after its complete output is copied.
 */
struct gpu_command_wait {
	uint32_t version;
	uint32_t size;
	uint64_t sequence;
	uint64_t timeout_ns;
	uint32_t flags;
	uint32_t status;
};


/*
 * One immutable linear image description retained with a shared allocation.
 * Version and size describe this 64-byte record. Device identity is assigned
 * by K on export; import returns the authoritative stored description.
 */
struct gpu_image_descriptor {
	uint32_t version;
	uint32_t size;
	uint32_t width;
	uint32_t height;
	uint32_t format;
	uint32_t stride;
	uint64_t offset;
	uint64_t allocation_bytes;
	uint32_t memory_type;
	uint32_t usage;
	uint32_t tiling;
	uint32_t reserved;
	uint64_t device_id;
};

/*
 * One allocation capability exported without exposing its rendering session.
 * Input fd is -1 and image.device_id is zero; successful fd installation
 * follows copyout of the complete output, including the device identity.
 */
struct gpu_resource_export {
	uint32_t version;
	uint32_t size;
	uint64_t handle;
	uint32_t flags;
	int32_t fd;
	struct gpu_image_descriptor image;
};

/*
 * One capability imported into this open's independent renderer context.
 * Version, size, fd and flags are inputs. Flags zero selects renderer import;
 * GPU_IMPORT_SCANOUT selects checked native display import. Success returns
 * an owned resource handle and immutable image metadata. A native-only import
 * returns resource_id zero and cannot be used as a renderer resource identity.
 */
struct gpu_resource_import {
	uint32_t version;
	uint32_t size;
	int32_t fd;
	uint32_t flags;
	uint64_t handle;
	uint32_t resource_id;
	uint32_t reserved;
	struct gpu_image_descriptor image;
};

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

/* One backend allocation whose optional map and independent sharing are selected by flags. */
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

/*
 * A session-local mmap offset, never a physical address. The caller supplies
 * a live resource handle and zero output fields; mmap uses the returned offset.
 */
struct gpu_resource_map {
	uint32_t version;
	uint32_t size;
	uint64_t handle;
	uint64_t offset;
	uint64_t bytes;
};

#endif
