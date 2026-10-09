/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* FIFO native preparation owns every uploaded input independently of public Vulkan identities. */
#ifndef KERN_DRIVERS_GPU_BCM2711_VULKAN_NATIVE_DRAW_H
#define KERN_DRIVERS_GPU_BCM2711_VULKAN_NATIVE_DRAW_H

#include "drivers/gpu/bcm2711/native-storage.h"
#include "drivers/gpu/bcm2711/native-bin.h"
#include "drivers/gpu/bcm2711/vulkan-uniform.h"

/* Each canonical slot needs at most one image and one descriptor allocation, plus three code/uniform pairs and fetch/default/shader storage. */
#define BCM2711_VULKAN_DRAW_STORAGE (BCM2711_VULKAN_PIPELINE_SETS * BCM2711_VULKAN_LAYOUT_BINDINGS * 2U + 9U)

/* One complete native draw retains all GPU inputs through uncertain completion; its enclosing job keeps the prepared primary pending. */
struct bcm2711_vulkan_native_draw {
	/* The enclosing pass links fully owned draw roots; individual release never traverses this link. */
	struct bcm2711_vulkan_native_draw *next;
	struct bcm2711_v3d_space *space;
	struct bcm2711_native_storage *storage[BCM2711_VULKAN_DRAW_STORAGE];
	struct bcm2711_vulkan_native_binding bindings[BCM2711_VULKAN_PIPELINE_SETS][BCM2711_VULKAN_LAYOUT_BINDINGS];
	uint64_t bytes;
	uint32_t count;
	uint32_t shader;
	uint32_t attributes;
	uint32_t vertices;
	/* Complete numerical BCL state contains only pointers into this root's independently owned mappings. */
	uint32_t bin_bytes;
	uint8_t bin[BCM2711_NATIVE_BIN_BYTES];
};

/* One unpublished internal texture quad borrows an actual retained source and owns copied geometry/programs until native upload finishes. */
struct bcm2711_vulkan_meta_draw {
	struct bcm2711_vulkan_pipeline pipeline;
	struct bcm2711_shader_key key;
	struct bcm2711_vulkan_resource *image;
	uint32_t vertices[36];
	uint32_t viewport[6];
	uint32_t width;
	uint32_t height;
	VkRect2D area;
	VkFilter filter;
	/* Copy preserves raw channel bytes; blit instead uses the source's canonical colour interpretation. */
	bool raw;
};

int bcm2711_vulkan_native_meta_draw_create(struct bcm2711_v3d_space *space, const struct bcm2711_vulkan_meta_draw *meta, uint64_t *available, struct bcm2711_vulkan_native_draw **draw);

/* The controller mutex and earlier queue completion/CPU visibility are prerequisites; available counts the enclosing job's remaining padded native budget. */
int bcm2711_vulkan_native_draw_create(struct bcm2711_v3d_space *space, const struct bcm2711_vulkan_prepared_event *event, uint64_t *available, struct bcm2711_vulkan_native_draw **draw);

/* true consumes/nulls all storage even when translation teardown fails; false keeps the whole root unchanged. */
int bcm2711_vulkan_native_draw_release(struct bcm2711_vulkan_native_draw **draw, bool retired);

/* Internal lowering runs only while the unpublished draw root and prepared event are protected by the controller mutex. */
int bcm2711_vulkan_native_bin_prepare(const struct bcm2711_vulkan_prepared_event *event, struct bcm2711_vulkan_native_draw *draw);

#endif
