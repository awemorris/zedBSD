/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * GPU resources: storage objects, blobs, CPU mappings and sharing.
 *
 * Every resource is a session object (memory.h) bound into the session's
 * private address space.  Sharing lets one open of the node import what
 * another exported, on the same device.
 */

#ifndef DRIVERS_GPU_I915_RESOURCE_H
#define DRIVERS_GPU_I915_RESOURCE_H

struct drv_gpu_ops;

void drv_i915_resource_bind_ops(struct drv_gpu_ops *ops);

#endif
