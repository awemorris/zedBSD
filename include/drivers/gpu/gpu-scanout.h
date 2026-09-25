/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Optional display-only device pairing and checked foreign scanout import.
 */

#ifndef DRIVERS_GPU_SCANOUT_H
#define DRIVERS_GPU_SCANOUT_H

#include <uapi/gpu.h>
#include <uapi/gpu-scanout.h>

#define DRV_GPU_BACKING_COHERENT		1U
#define DRV_GPU_BACKING_DEVICE			2U
#define DRV_GPU_BACKING_CONTIGUOUS		4U

/*
 * Physical pages borrowed from an independently retained source allocation.
 * Pages have page_bytes bytes each; bytes describes the usable allocation.
 * The source guarantees immutable addresses and cache attributes while its
 * exported handle survives. DEVICE denotes MMIO rather than ordinary RAM.
 * This view carries no renderer-local IDs and never implies DMA reachability.
 */
struct drv_gpu_scanout_backing {
	const uint64_t *pages;
	uint64_t page_count;
	uint64_t bytes;
	uint32_t page_bytes;
	uint32_t flags;
};

/*
 * Optional callbacks run with the same open lifetime as ordinary GPU ioctls.
 * Query supplies roles and an optional companion; the core owns device_id.
 * Constraints are authoritative only for the requested display generation.
 * Foreign import validates format, offset, pitch, bounds, cache attributes,
 * placement and DMA reachability against the actual borrowed physical pages.
 * Success returns an owned native resource retired by resource_destroy; its
 * rendering resource ID is zero. Failure owns nothing. The core pins the
 * source capability until after resource_destroy returns, including on close.
 * A backend lacking physical backing access must return ENOTSUP, not reinterpret
 * a foreign renderer ID. Same-device scanout may use ordinary share import.
 */
struct drv_gpu_scanout_ops {
	int (*query_device)(void *, void *, struct gpu_device_info *);
	int (*constraints)(void *, void *, struct gpu_scanout_constraints *);
	int (*import_image)(void *, void *, const struct gpu_image_descriptor *, const struct drv_gpu_scanout_backing *, void **);
};

#endif
