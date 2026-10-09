/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Exact CPU uniform streams are assembled at FIFO execution, then uploaded by the whole native job owner. */
#ifndef KERN_DRIVERS_GPU_BCM2711_VULKAN_UNIFORM_H
#define KERN_DRIVERS_GPU_BCM2711_VULKAN_UNIFORM_H

#include "drivers/gpu/bcm2711/vulkan-prepared.h"

/* One canonical descriptor slot borrows native record addresses from independently owned job storage; it grants no new DMA lifetime. */
struct bcm2711_vulkan_native_binding {
	uint32_t texture;
	uint32_t sampler;
};

/* One completed scalar stream owns only copied numerical words, with no borrowed descriptor, pipeline or command pointer. */
struct bcm2711_vulkan_uniform_words {
	uint32_t *words;
	uint32_t count;
};

/* Call under the controller mutex after earlier queue writes have retired; descriptor-address backing/lifetime belongs to the native job. */
int bcm2711_vulkan_uniform_create(const struct bcm2711_vulkan_prepared_event *draw, uint32_t stage, const struct bcm2711_vulkan_native_binding bindings[BCM2711_VULKAN_PIPELINE_SETS][BCM2711_VULKAN_LAYOUT_BINDINGS], struct bcm2711_vulkan_uniform_words **words);
void bcm2711_vulkan_uniform_release(struct bcm2711_vulkan_uniform_words *words);

#endif
