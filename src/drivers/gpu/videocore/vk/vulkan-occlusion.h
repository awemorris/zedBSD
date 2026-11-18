/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Occlusion query pools.
 *
 * A pool keeps each query's samples and availability in kernel memory.  A
 * draw inside an active query names one of its pass's counters in its
 * binning state (the Occlusion Query Counter packet); the tile buffer adds
 * the samples that pass the depth and stencil tests to that counter, and
 * the CPU adds the counter into the query once the pass has completed.  A
 * query becomes available when the work recorded before its end has run.
 */

#ifndef KERN_DRIVERS_GPU_BCM2711_VULKAN_OCCLUSION_H
#define KERN_DRIVERS_GPU_BCM2711_VULKAN_OCCLUSION_H

#include <stdbool.h>

#include "drivers/gpu/videocore/vk/vulkan-private.h"
#include "drivers/gpu/videocore/vk/vulkan-record.h"

/* The most queries a pool holds. */
#define BCM2711_VULKAN_OCCLUSION_QUERIES 16384U

/*
 * The counters one native pass holds: one group of sixteen 32-bit
 * counters at consecutive addresses, the group 1024-byte aligned.
 * Values referenced: Mesa 26.2.6 src/broadcom/vulkan/v3dv_query.c
 * v3dv_CreateQueryPool (the occlusion counters' layout).
 */
#define BCM2711_VULKAN_OCCLUSION_COUNTERS 16U
#define BCM2711_VULKAN_OCCLUSION_GROUP_BYTES 1024U

/* One occlusion query pool: each query's samples and whether it is available. */
struct bcm2711_vulkan_occlusion_pool {
	struct bcm2711_vulkan_object *device;
	uint32_t count;
	uint64_t *samples;
	uint8_t *available;
};

/* Routes vkCreateQueryPool of an occlusion pool, and vkDestroyQueryPool and vkGetQueryPoolResults of one. */
int bcm2711_vulkan_occlusion_dispatch(struct bcm2711_vulkan_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);

/* Reports whether a recorded query command ([command buffer][pool]...) names an occlusion pool; the reader is left unchanged. */
bool bcm2711_vulkan_occlusion_named(struct bcm2711_vulkan_session *session, const struct i915_wire_reader *reader);

/* The occlusion pool a recorded begin, end or reset names, or NULL for any other record. */
struct bcm2711_vulkan_occlusion_pool *bcm2711_vulkan_occlusion_record(const struct bcm2711_vulkan_record *record);

/* Runs a reset (the queries' samples cleared, unavailable) or an end (available) after every earlier pass. */
void bcm2711_vulkan_occlusion_run(const struct bcm2711_vulkan_record *record);

/* Adds a completed pass's counter to its query. */
void bcm2711_vulkan_occlusion_add(struct bcm2711_vulkan_occlusion_pool *pool, uint32_t query, uint32_t samples);

#endif
