/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Image transfers use independently owned native texture quads and the ordinary whole-pass DMA lifetime. */
#ifndef KERN_DRIVERS_GPU_BCM2711_VULKAN_NATIVE_IMAGE_H
#define KERN_DRIVERS_GPU_BCM2711_VULKAN_NATIVE_IMAGE_H

#include "drivers/gpu/bcm2711/vk/vulkan-native-pass.h"
#include "drivers/gpu/bcm2711/vk/vulkan-transfer.h"
#include "drivers/gpu/bcm2711/vk/vulkan-buffer-copy.h"

/* ENOTSUP before any allocation means the copy is not one whole-image raster copy and stays a CPU copy. */
int bcm2711_vulkan_native_buffer_image_create(struct bcm2711_v3d_space *space, const struct bcm2711_vulkan_record *record, uint64_t *available, struct bcm2711_vulkan_native_pass **pass);
int bcm2711_vulkan_native_image_create(struct bcm2711_v3d_space *space, const struct bcm2711_vulkan_transfer *transfer, uint32_t region, uint64_t *available, struct bcm2711_vulkan_native_pass **pass);

#endif
