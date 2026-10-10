/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The types every part of the Vulkan executor shares.
 *
 * The executor decodes the Vulkan command stream that libvulkan submits
 * through the GPU node (the Kei GPU command protocol, uapi/gpu-op.h) and
 * turns it into Gen12 work.  This header holds the executor's device and
 * session, the wire cursors and the per-command arena.  Types private to
 * one part are defined inside that part.
 */

#ifndef DRIVERS_GPU_I915_RENDER_INTERNAL_H
#define DRIVERS_GPU_I915_RENDER_INTERNAL_H

#include <kern/lock.h>
#include <uapi/gpu-op.h>
#include <stddef.h>
#include <stdint.h>

struct i915_device;
struct i915_session;
struct i915_gem_object;
struct i915_gfx_memory;
struct i915_gfx_session;
struct i915_object_table;

/* The size of the scratch one command decodes its records into. */
#define I915_WIRE_ARENA_BYTES		(256U * 1024U)

/* The largest capset the executor reports, in 32-bit words (at least 156 bytes). */
#define I915_RENDER_CAPSET_WORDS	64U

/* One Vulkan object identity as carried on the wire. */
typedef uint64_t i915_vk_handle;

/*
 * The kinds of object the object table tells apart.
 *
 * The same wire identity may name objects of two kinds, so every lookup
 * names the kind it expects.
 */
enum i915_vk_object_kind {
	I915_VK_OBJ_NONE = 0,
	I915_VK_OBJ_INSTANCE,
	I915_VK_OBJ_PHYSICAL_DEVICE,
	I915_VK_OBJ_DEVICE,
	I915_VK_OBJ_QUEUE,
	I915_VK_OBJ_MEMORY,
	I915_VK_OBJ_BUFFER,
	I915_VK_OBJ_BUFFER_VIEW,
	I915_VK_OBJ_IMAGE,
	I915_VK_OBJ_IMAGE_VIEW,
	I915_VK_OBJ_SAMPLER,
	I915_VK_OBJ_DESCRIPTOR_SET_LAYOUT,
	I915_VK_OBJ_DESCRIPTOR_POOL,
	I915_VK_OBJ_DESCRIPTOR_SET,
	I915_VK_OBJ_PIPELINE_LAYOUT,
	I915_VK_OBJ_PIPELINE,
	I915_VK_OBJ_SHADER_MODULE,
	I915_VK_OBJ_RENDER_PASS,
	I915_VK_OBJ_FRAMEBUFFER,
	I915_VK_OBJ_COMMAND_POOL,
	I915_VK_OBJ_COMMAND_BUFFER,
	I915_VK_OBJ_FENCE,
	I915_VK_OBJ_SEMAPHORE,
	I915_VK_OBJ_EVENT,
	I915_VK_OBJ_QUERY_POOL,
	I915_VK_OBJ_SWAPCHAIN,
	I915_VK_OBJ_VIDEO_SESSION,
	I915_VK_OBJ_VIDEO_SESSION_PARAMETERS,
	I915_VK_OBJ_KIND_COUNT
};

/*
 * A bounded cursor over an inbound wire command.
 *
 * It lives on the stack of the call that decodes one submitted stream.
 * Once error is set it stays set: every later read returns zero without
 * advancing, and the stream decodes no further.
 */
struct i915_wire_reader {
	/* The stream and its length in bytes. */
	const uint8_t *base;
	size_t size;

	/* The next byte to decode. */
	size_t offset;

	/* Nonzero after a read ran past the end or the stream was refused. */
	int error;
};

/*
 * A bounded cursor building a reply.
 *
 * It points into the session blob the stream selected for its replies.
 * Once error is set it stays set, and the stream fails when it ends.
 */
struct i915_wire_writer {
	/* The reply region and its length in bytes. */
	uint8_t *base;
	size_t size;

	/* The next byte to write. */
	size_t offset;

	/* Nonzero after a write would have overflowed the region. */
	int error;
};

/*
 * The scratch a command decodes its records into.
 *
 * One exists per session and is emptied before each command, so a decoded
 * pointer lives until its command returns and no longer.
 */
struct i915_wire_arena {
	/* The scratch and its length in bytes; NULL for a session that decodes no records. */
	uint8_t *base;
	size_t size;

	/* How many bytes the current command has taken. */
	size_t used;
};

/*
 * The executor of one i915 device.
 *
 * It is attached before the GPU node is published and detached after the
 * node is withdrawn, so every session it serves opened and closed within
 * its lifetime.
 */
struct i915_render_device {
	/* The hardware device the executor serves. */
	struct i915_device *i915;

	/* Every Vulkan object the sessions of this device created, by kind and identity. */
	struct i915_object_table *objects;

	/*
	 * Every live VkDeviceMemory of the device's sessions, newest first, and
	 * the lock of the list: a session's command stream, another session's
	 * and a blob's attach or detach change or search it at once on
	 * different CPUs (BUG-117).
	 */
	struct i915_gfx_memory *memories;
	struct mutex memories_lock;

	/* The capset libvulkan reads before it opens the node, and its length in bytes. */
	uint32_t capset[I915_RENDER_CAPSET_WORDS];
	uint32_t capset_bytes;

	/*
	 * Nonzero when the device offers Vulkan video decode: video decode is
	 * offered (the default) and the GT has the video decode engine
	 * VCS0.  It is fixed at attach and decides the capset's native word,
	 * the second queue family and every video command.
	 */
	int video;

	/* The active display mode, filled by the display path. */
	uint32_t display_width;
	uint32_t display_height;
	uint32_t display_stride;
};

/*
 * The executor's side of one open of the GPU node.
 *
 * It wraps the node session (its address space and contexts) from the
 * node's open to its close.
 */
struct i915_render_session {
	/* The executor the session belongs to. */
	struct i915_render_device *vk;

	/* The node session whose address space the session draws into. */
	struct i915_session *gpu;

	/* The scratch the session's commands decode their records into. */
	struct i915_wire_arena arena;

	/* What the session's draws keep between them; NULL until the first draw. */
	struct i915_gfx_session *gfx;
};

/*
 * A Gen batch under construction: a session object and a dword cursor.
 *
 * It lives while one batch is written; error latches a write past the
 * capacity.
 */
struct i915_render_batch {
	/* The object that holds the batch and its CPU view. */
	struct i915_gem_object *object;
	uint32_t *map;

	/* The next dword to write and how many dwords fit. */
	uint32_t cursor;
	uint32_t capacity;

	/* Nonzero after a write past the capacity. */
	int error;
};

int drv_i915_render_errno(int vk_result);

#endif
