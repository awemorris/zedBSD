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
#include "drivers/gpu/i915/vk/vk-codec.h"

struct bcm2711_render_session;
struct bcm2711_vulkan_object;

/*
 * The buckets of a session's identity index.  Every vkCmd a
 * client records looks up its command buffer and inputs by wire identity;
 * a power of two makes the bucket a shift of the hashed identity, and with
 * the session's limit of 4096 live objects a bucket holds four on average.
 */
#define BCM2711_VULKAN_OBJECT_BUCKETS 1024U

/* One logical Vulkan session outlives every prepared native payload and owns all published wire identities. */
struct bcm2711_vulkan_session {
	struct bcm2711_render_session *render;
	/* Every published identity, newest first, for the walks over one kind or the whole namespace. */
	struct bcm2711_vulkan_object *objects;
	/* The same identities by hashed identity, for the lookups each decoded command makes. */
	struct bcm2711_vulkan_object *buckets[BCM2711_VULKAN_OBJECT_BUCKETS];
	uint32_t object_count;
	uint32_t live_objects;
	bool closing;
	/* A legacy synchronous frame is executing: it cannot accept a QueueSubmit. */
	bool synchronous;
	/* The opcodes of the last command the decoder refused and the last it decoded, for diagnostics. */
	uint32_t failed_opcode;
	uint32_t last_opcode;
	struct i915_wire_arena arena;
#if CONFIG_DRIVER_RPI4_GPU || CONFIG_DRIVER_RPI5_GPU
	/* The capset promised H.264 video decode; latched for the session's life. */
	bool video;
#endif
};

/*
 * One typed wire identity has independent references from its registry, dependent objects and prepared native work.
 * Removing its identity drops only the registry reference; destruction waits until the final physical owner retires.
 */
struct bcm2711_vulkan_object {
	/* The next published identity in the session's list, and the pointer that points here, so removal unlinks at once. */
	struct bcm2711_vulkan_object *next;
	struct bcm2711_vulkan_object **link;
	/* The next published identity in the same bucket of the session's index. */
	struct bcm2711_vulkan_object *bucket_next;
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
