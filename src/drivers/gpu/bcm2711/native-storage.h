/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Private uploaded storage retains its native VA until both DMA and translation retirement are proved. */
#ifndef KERN_DRIVERS_GPU_BCM2711_NATIVE_STORAGE_H
#define KERN_DRIVERS_GPU_BCM2711_NATIVE_STORAGE_H

#include "drivers/gpu/bcm2711/v3d-memory.h"
#include "drivers/gpu/bcm2711/shader.h"

/* One unpublished or prepared job owns this complete cached allocation through its independent native mapping reference. */
struct bcm2711_native_storage {
	struct bcm2711_v3d_space *space;
	struct bcm2711_v3d_view *view;
	uint64_t bytes;
};

/* The controller mutex serializes every operation with native execution and recovery. */
int bcm2711_native_storage_create(struct bcm2711_v3d_space *space, uint64_t bytes, struct bcm2711_native_storage **storage);
int bcm2711_native_storage_clean(struct bcm2711_native_storage *storage);
int bcm2711_native_program_upload(struct bcm2711_v3d_space *space, const struct bcm2711_shader_binary *binary, struct bcm2711_native_storage **storage);

/* retired=false preserves the whole owner; true consumes and nulls it even if final translation flush fails into the space quarantine. */
int bcm2711_native_storage_release(struct bcm2711_v3d_space *space, struct bcm2711_native_storage **storage, bool retired);

#endif
