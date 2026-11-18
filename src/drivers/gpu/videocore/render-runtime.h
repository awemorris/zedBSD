/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Public Vulkan transport shares the renderer's existing ordered execution owner. */
#ifndef KERN_DRIVERS_GPU_BCM2711_RENDER_RUNTIME_H
#define KERN_DRIVERS_GPU_BCM2711_RENDER_RUNTIME_H

struct drv_gpu_ops;

void bcm2711_render_runtime_bind(struct drv_gpu_ops *operations);

#endif
