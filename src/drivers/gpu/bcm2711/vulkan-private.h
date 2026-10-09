/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Private Vulkan state remains serialized by the renderer controller mutex, including native worker disposal. */
#ifndef KERN_DRIVERS_GPU_BCM2711_VULKAN_PRIVATE_H
#define KERN_DRIVERS_GPU_BCM2711_VULKAN_PRIVATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The existing Zlib wire cursors and generated record codec are device-independent read-only source. */
#include "drivers/gpu/i915/render/codec.h"

struct bcm2711_render_session;
struct bcm2711_vulkan_object;

/* One logical Vulkan session outlives every prepared native payload and owns all published wire identities. */
struct bcm2711_vulkan_session {
	struct bcm2711_render_session *render;
	struct bcm2711_vulkan_object *objects;
	uint32_t object_count;
	uint32_t live_objects;
	bool closing;
	struct i915_wire_arena arena;
};

/*
 * One typed wire identity has independent references from its registry, dependent objects and prepared native work.
 * Removing its identity drops only the registry reference; destruction waits until the final physical owner retires.
 */
struct bcm2711_vulkan_object {
	struct bcm2711_vulkan_object *next;
	struct bcm2711_vulkan_session *session;
	enum i915_vk_object_kind kind;
	uint64_t identity;
	uint32_t references;
	bool published;
	void *payload;
	int (*destroy)(struct bcm2711_vulkan_session *session, void *payload);
};

int bcm2711_vulkan_object_publish(struct bcm2711_vulkan_session *session, enum i915_vk_object_kind kind, uint64_t identity, void *payload, int (*destroy)(struct bcm2711_vulkan_session *, void *), struct bcm2711_vulkan_object **object);
struct bcm2711_vulkan_object *bcm2711_vulkan_object_find(struct bcm2711_vulkan_session *session, enum i915_vk_object_kind kind, uint64_t identity);
int bcm2711_vulkan_object_retain(struct bcm2711_vulkan_object *object);
int bcm2711_vulkan_object_release(struct bcm2711_vulkan_object *object);
int bcm2711_vulkan_object_remove(struct bcm2711_vulkan_session *session, enum i915_vk_object_kind kind, uint64_t identity);
int bcm2711_vulkan_objects_close(struct bcm2711_vulkan_session *session);
int bcm2711_vulkan_session_open(struct bcm2711_render_session *render, struct bcm2711_vulkan_session **session);
int bcm2711_vulkan_session_close(struct bcm2711_vulkan_session **session);
int bcm2711_vulkan_stream_execute(struct bcm2711_vulkan_session *session, const void *wire, uint32_t bytes, int (*dispatch)(struct bcm2711_vulkan_session *, uint32_t, uint32_t, struct i915_wire_reader *, struct i915_wire_writer *));

#endif
