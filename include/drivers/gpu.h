/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * GPU backend operations and dynamic device registration.
 */

#ifndef DRIVERS_GPU_H
#define DRIVERS_GPU_H

#include <uapi/gpu.h>
#include <drivers/gpu-display.h>
#include <stdint.h>

#define DRV_GPU_INTERFACE_VERSION	3U

#define DRV_GPU_MAPPING_DEVICE 1U

struct drv_gpu_device;

/*
 * An immutable CPU view borrowed from one retained resource. The GPU core
 * retains the resource and its open file through every VM mapping and pin.
 * DEVICE distinguishes uncached MMIO from ordinary coherently mapped DMA RAM.
 */
struct drv_gpu_mapping {
	uint64_t physical;
	void *address;
	uint64_t bytes;
	uint32_t attributes;
};

/*
 * Immutable operations shared by instances of one backend.
 *
 *  - No callback runs under a core spinlock.
 *  - Distinct sessions may execute concurrently, a single session
 *    admits one ioctl at a time.
 *  - Open failure must unwind its own state.
 *  - Resource and blob allocation failures must unwind their own state.
 *  - Optional capabilities require their complete callback pair or operation.
 *  - All buffers passed to optional operations are validated kernel copies.
 *  - Resource read/write finish copying before returning; command receipt is
 *    independent of Vulkan execution completion.
 *  - Present accepts storage resources; the backend owns display arbitration.
 *  - Close and resource_destroy cannot fail and must finish using the state
 *    before returning.
 *  - Unregister preserves private_data until all sessions close; the owner
 *    must retry EBUSY before releasing device state.
 */
struct drv_gpu_ops {
	uint32_t version;
	uint32_t size;
	uint32_t capabilities;
	uint32_t reserved;

	int (*open)(void *, void **);
	void (*close)(void *, void *);
	int (*get_info)(void *, void *, struct gpu_info *);
	int (*resource_create)(void *, void *, const struct gpu_resource_create *, void **);
	void (*resource_destroy)(void *, void *, void *);
	int (*get_capset)(void *, void *, struct gpu_capset *);
	int (*blob_create)(void *, void *, const struct gpu_blob_create *, void **, uint32_t *);
	int (*resource_read)(void *, void *, void *, uint64_t, void *, uint32_t);
	int (*resource_write)(void *, void *, void *, uint64_t, const void *, uint32_t);
	int (*command)(void *, void *, const void *, uint32_t);
	int (*present)(void *, void *, void *, const struct gpu_present *);
	/* Optional display ownership and immutable mapping views retain the same session lifetime. */
	const struct drv_gpu_display_ops *display;
	int (*resource_map)(void *, void *, void *, struct drv_gpu_mapping *);
};

/*
 * Registers one initialized device using borrowed operations and private data.
 *
 *  - Every call creates an independent device, even when operations are shared.
 *  - The core owns the returned handle and its /dev/gpuN publication.
 *  - Failure leaves result NULL and does not consume operations or private_data.
 *  - Both borrowed objects must remain valid until unregister succeeds.
 */
int
drv_gpu_register(
	const struct drv_gpu_ops *ops,
	void *private_data,
	struct drv_gpu_device **result);

/*
 * Withdraws a device and releases its registration after all sessions close.
 *
 *  - EBUSY retains the handle and its borrowed state for a later retry.
 *  - Once withdrawal starts, new opens and ioctls fail with ENODEV.
 *  - Success consumes the handle, the caller may then release its private data
 *    and operations.
 *  - Old inode references retain only the offline core wrapper.
 */
int
drv_gpu_unregister(
	struct drv_gpu_device *device);

#endif
