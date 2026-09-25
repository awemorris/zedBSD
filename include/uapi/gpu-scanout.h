/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Device pairing and immutable display allocation constraints.
 */

#ifndef KERN_UAPI_GPU_SCANOUT_H
#define KERN_UAPI_GPU_SCANOUT_H

#include <stdint.h>
#include <uapi/gpu.h>
#include <uapi/ioctl.h>

#define GPU_DEVICE_RENDER		1U
#define GPU_DEVICE_DISPLAY		2U
#define GPU_SCANOUT_SHARED		1U
#define GPU_SCANOUT_COPY			2U
#define GPU_SCANOUT_FOREIGN		4U
#define GPU_PLACEMENT_DMA32		1U
#define GPU_PLACEMENT_CONTIGUOUS		2U
#define GPU_PLACEMENT_COHERENT		4U
#define GPU_IMPORT_SCANOUT		1U

#define GPU_DEVICE_QUERY			_IOWR('G', 30, struct gpu_device_info)
#define GPU_DISPLAY_CONSTRAINTS		_IOWR('G', 31, struct gpu_scanout_constraints)
#define GPU_BLOB_CREATE_PLACED		_IOWR('G', 33, struct gpu_blob_create_placed)

/*
 * Physical allocation conditions requested before a blob becomes visible.
 * Flags are GPU_PLACEMENT_*; max_dma_address is inclusive, zero means no limit.
 * Alignment zero or one adds no restriction; otherwise it is a power of two
 * on the first DMA address. These are requirements, never hints to ignore.
 * A backend unable to establish a condition from actual backing returns ENOTSUP.
 */
struct gpu_placement {
	uint32_t flags;
	uint32_t reserved;
	uint64_t max_dma_address;
	uint64_t alignment;
};

/*
 * One placed blob uses the unchanged legacy prefix with its size set to 64.
 * Zero conditions use the existing allocator; nonzero conditions require the
 * optional placed allocator. Success guarantees every requested condition.
 * The original GPU_BLOB_CREATE remains a separate, unchanged 40-byte request.
 */
struct gpu_blob_create_placed {
	struct gpu_blob_create blob;
	struct gpu_placement placement;
};

/*
 * Kernel-assigned device identity and optional preferred rendering companion.
 * Only version and size are inputs. Companion zero means no preference, not
 * proof of sharing support. A renderer may own zero, one or several outputs.
 */
struct gpu_device_info {
	uint32_t version;
	uint32_t size;
	uint64_t device_id;
	uint64_t companion_id;
	uint32_t roles;
	uint32_t flags;
	uint64_t reserved;
};

/*
 * Constraints for one display generation, queried before allocation/import.
 * Inputs are version, size, display_id and generation; all others are zero.
 * Formats use GPU_DISPLAY_FORMAT_* bits. Zero alignment means byte alignment;
 * nonzero alignments are powers of two.
 * Placement is a requirement mask, not an allocation guarantee. A zero DMA
 * address limit means that this backend exposes no physical-address limit.
 * SHARED permits an import trial; FOREIGN additionally permits backing from
 * another device. Neither bit promises that an arbitrary allocation imports.
 * COPY explicitly supports ordinary storage and GPU_DISPLAY_PRESENT without
 * BLOB. The route is selected once for a swapchain, never per presentation.
 */
struct gpu_scanout_constraints {
	uint32_t version;
	uint32_t size;
	uint32_t display_id;
	uint32_t flags;
	uint64_t generation;
	uint32_t formats;
	uint32_t stride_alignment;
	uint32_t offset_alignment;
	uint32_t placement;
	uint64_t max_dma_address;
	uint64_t reserved;
};

#endif
