/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Shares allocation ownership independently from a display image layout.
 */

#ifndef KERN_UAPI_GPU_ALLOCATION_H
#define KERN_UAPI_GPU_ALLOCATION_H

#include <uapi/gpu.h>

#define GPU_CAP_ALLOCATION_SHARE 1024U
#define GPU_ALLOCATION_METADATA_MAX 128U
#define GPU_ALLOCATION_EXPORT _IOWR('G', 14, struct gpu_allocation_export)
#define GPU_ALLOCATION_IMPORT _IOWR('G', 15, struct gpu_allocation_import)

/*
 * One immutable application description of a shared GPU allocation.
 * K assigns device_id and checks the resource size. Schema and metadata belong
 * to the communicating userspace libraries, not to a display or kernel codec.
 * A capability grants the entire allocation; metadata never narrows that grant
 * into a security boundary or proves that an image is eligible for scanout.
 * Unused metadata bytes must be zero. Import returns the stored description.
 */
struct gpu_allocation_descriptor {
	uint32_t version;
	uint32_t size;
	uint64_t allocation_bytes;
	uint64_t device_id;
	uint32_t schema;
	uint32_t metadata_bytes;
	uint8_t metadata[GPU_ALLOCATION_METADATA_MAX];
};

/*
 * One resource export whose independently owned fd is published after copyout.
 * Input fd is -1; flags use GPU_HANDLE_CLOEXEC and GPU_HANDLE_CLOFORK.
 */
struct gpu_allocation_export {
	uint32_t version;
	uint32_t size;
	uint64_t handle;
	uint32_t flags;
	int32_t fd;
	struct gpu_allocation_descriptor allocation;
};

/*
 * One allocation capability imported into an independent renderer context.
 * Only version, size and fd are inputs; flags, handle, resource_id, reserved
 * and the whole allocation are zero (EINVAL otherwise). Success returns an
 * owned resource and the sender's immutable metadata; the supplied fd
 * remains caller-owned.
 */
struct gpu_allocation_import {
	uint32_t version;
	uint32_t size;
	int32_t fd;
	uint32_t flags;
	uint64_t handle;
	uint32_t resource_id;
	uint32_t reserved;
	struct gpu_allocation_descriptor allocation;
};

#endif
