/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Private allocation capabilities shared only by the two BCM2711 devices. */
#ifndef KERN_DRIVERS_GPU_BCM2711_SHARE_H
#define KERN_DRIVERS_GPU_BCM2711_SHARE_H

#include <stdbool.h>
#include <drivers/gpu/gpu-scanout.h>
#include "drivers/gpu/bcm2711/buffer.h"

struct bcm2711_shared;

int bcm2711_blob_allocate(const struct gpu_blob_create *request, const struct gpu_placement *placement, struct bcm2711_buffer **result);
int bcm2711_shared_create(struct bcm2711_buffer *buffer, const struct gpu_image_descriptor *image, struct bcm2711_shared **result);
void bcm2711_shared_release(struct bcm2711_shared *shared);
struct bcm2711_buffer *bcm2711_shared_buffer(struct bcm2711_shared *shared);
int bcm2711_shared_backing(struct bcm2711_shared *shared, struct drv_gpu_scanout_backing *backing);
int bcm2711_shared_lookup(const struct gpu_image_descriptor *image, const struct drv_gpu_scanout_backing *backing, struct bcm2711_buffer **result);
bool bcm2711_shared_description(struct bcm2711_shared *shared, struct gpu_image_descriptor *image);
int bcm2711_shared_image(const struct bcm2711_buffer *buffer, const struct gpu_image_descriptor *image);

#endif
