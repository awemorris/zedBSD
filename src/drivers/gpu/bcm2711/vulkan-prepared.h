/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Prepared CPU graphics snapshots freeze an independently retained primary graph until confirmed native retirement. */
#ifndef KERN_DRIVERS_GPU_BCM2711_VULKAN_PREPARED_H
#define KERN_DRIVERS_GPU_BCM2711_VULKAN_PREPARED_H

#include "drivers/gpu/bcm2711/vulkan-draw.h"

/* One native preparation point owns only consumed descriptor copies; other immutable inputs belong to the retained pending primary. */
struct bcm2711_vulkan_prepared_event {
	struct bcm2711_vulkan_prepared_event *next;
	uint32_t opcode;
	/* The independently retained primary keeps each immutable dependency record alive until native retirement. */
	const struct bcm2711_vulkan_record *record;
	const struct bcm2711_vulkan_record *pass;
	struct bcm2711_vulkan_pipeline *pipeline;
	struct bcm2711_vulkan_resource *vertices[BCM2711_VULKAN_VERTEX_BINDINGS];
	uint64_t offsets[BCM2711_VULKAN_VERTEX_BINDINGS];
	struct bcm2711_vulkan_descriptor descriptors[BCM2711_VULKAN_PIPELINE_SETS][BCM2711_VULKAN_LAYOUT_BINDINGS];
	uint32_t used[BCM2711_VULKAN_PIPELINE_SETS];
	uint32_t push[2][BCM2711_VULKAN_PUSH_WORDS];
	uint32_t viewport[6];
	VkRect2D scissor;
	uint32_t draw[4];
};

/* One distinct recorded ordinary set is kept immutable by a pending charge, including bindings a shader does not consume. */
struct bcm2711_vulkan_prepared_set {
	struct bcm2711_vulkan_prepared_set *next;
	struct bcm2711_vulkan_descriptor_set *set;
};

/* One prepared submission owns its primary reference, descriptor copies and pending charges through queue wait, DMA and uncertainty. */
struct bcm2711_vulkan_prepared {
	struct bcm2711_vulkan_object *command;
	struct bcm2711_vulkan_prepared_event *first;
	struct bcm2711_vulkan_prepared_event *last;
	struct bcm2711_vulkan_prepared_set *sets;
	uint32_t bytes;
	bool pending;
};

/* Creation/release are serialized; release is permitted only before launch or after completion/reset proves all native DMA retired. */
int bcm2711_vulkan_prepared_create(struct bcm2711_vulkan_object *command, struct bcm2711_vulkan_prepared **prepared);
int bcm2711_vulkan_prepared_release(struct bcm2711_vulkan_prepared *prepared, bool retired);

#endif
