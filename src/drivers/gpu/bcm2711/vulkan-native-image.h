/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Image transfers use independently owned native texture quads and the ordinary whole-pass DMA lifetime. */
#ifndef KERN_DRIVERS_GPU_BCM2711_VULKAN_NATIVE_IMAGE_H
#define KERN_DRIVERS_GPU_BCM2711_VULKAN_NATIVE_IMAGE_H

#include "drivers/gpu/bcm2711/vulkan-native-pass.h"
#include "drivers/gpu/bcm2711/vulkan-transfer.h"

int bcm2711_vulkan_native_image_create(struct bcm2711_v3d_space *space, const struct bcm2711_vulkan_transfer *transfer, uint32_t region, uint64_t *available, struct bcm2711_vulkan_native_pass **pass);

#endif
