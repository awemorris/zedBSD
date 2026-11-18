/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display node's check of an image the render node exported, before
 * it imports it for shared frames: a linear image of a packed four-byte
 * format whose rows lie in one contiguous run of RAM.  The check reads
 * only the descriptor and the borrowed page list, so it runs unchanged
 * in a host test.
 */

#ifndef KERN_DRIVERS_GPU_MALI_DISPLAY_SCANOUT_H
#define KERN_DRIVERS_GPU_MALI_DISPLAY_SCANOUT_H

#include <stdint.h>

#include <drivers/gpu/gpu-scanout.h>
#include <uapi/gpu.h>

/*
 * Checks an exported image and its backing: 0 with the physical address
 * of the image's first byte, or ENOTSUP with a reason (an immutable
 * phrase that follows "the image").
 */
int mali_scanout_check(const struct gpu_image_descriptor *image, const struct drv_gpu_scanout_backing *backing, uint64_t *physical, const char **reason);

#endif
