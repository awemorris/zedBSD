/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Pool capacity follows retained set owners; reset and destruction withdraw identities without freeing prepared native data. */
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/vulkan-descriptor.h"

/* The finite session namespace bounds pool/set metadata independently of any native GPU allocation. */
#define VULKAN_POOL_SETS 4096U

static int create_pool(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int retire_pool(struct bcm2711_vulkan_session *session, bool reset, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int withdraw_sets(struct bcm2711_vulkan_session *session, struct bcm2711_vulkan_object *pool);
static int release_pool(struct bcm2711_vulkan_session *session, void *payload);
static void pool_reply(struct i915_wire_writer *reply, VkResult status, uint64_t identity);

/*
 * Routes finite descriptor-pool creation, checked reset and independently retained pool destruction.
 */
int
bcm2711_vulkan_descriptor_pool_dispatch(
	struct bcm2711_vulkan_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	bool reset;
	int error;

	/* Other descriptor routers own allocation, free and binding updates. */
	*handled = 1;
	if (opcode != GPU_OP_CREATE_DESCRIPTOR_POOL &&
	    opcode != GPU_OP_DESTROY_DESCRIPTOR_POOL && opcode != GPU_OP_RESET_DESCRIPTOR_POOL) {
		*handled = 0;
		return 0;
	}

	/* Ordinary void destruction still permits its echoed opcode; create/reset require their result body. */
	if (requested > 1 || (opcode != GPU_OP_DESTROY_DESCRIPTOR_POOL && requested != 1))
		return EINVAL;

	/* Reset retains the pool identity while withdrawing only its public child sets. */
	reset = false;
	if (opcode == GPU_OP_RESET_DESCRIPTOR_POOL)
		reset = true;
	if (opcode == GPU_OP_CREATE_DESCRIPTOR_POOL)
		error = create_pool(session, reader, reply);
	else
		error = retire_pool(session, reset, reader, reply);
	if (error != 0)
		return error;

	/* Succeeded: one exact pool operation retired or acquired only its specified ownership. */
	return 0;
}

/* Creates one immutable capacity declaration with exact combined-image/uniform limits and independent device ownership. */
static int
create_pool(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct bcm2711_vulkan_descriptor_pool *pool;
	struct bcm2711_vulkan_object *device;
	struct bcm2711_vulkan_object *object;
	uint64_t device_id;
	uint64_t present;
	uint64_t chain;
	uint64_t count;
	uint64_t allocator;
	uint64_t identity;
	uint32_t type;
	uint32_t flags;
	uint32_t maximum;
	uint32_t sizes;
	uint32_t index;
	uint32_t descriptors;
	uint32_t textures;
	uint32_t uniforms;
	int error;
	int retired;

	/* Decode the real client's standard pool prefix before reserving any capacity or metadata. */
	device_id = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	type = drv_i915_wire_read_u32(reader);
	chain = drv_i915_wire_read_u64(reader);
	flags = drv_i915_wire_read_u32(reader);
	maximum = drv_i915_wire_read_u32(reader);
	sizes = drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || present != 1 || type != VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO ||
	    chain != 0 || (flags & ~VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT) != 0 ||
	    maximum == 0 || maximum > VULKAN_POOL_SETS || sizes > 2 || count != sizes)
		return ENOTSUP;

	/* Capacity represents actual supported binding kinds, including bounded sums of repeated type declarations. */
	textures = 0;
	uniforms = 0;
	for (index = 0; index < sizes; index++) {
		type = drv_i915_wire_read_u32(reader);
		descriptors = drv_i915_wire_read_u32(reader);
		if (reader->error != 0 || descriptors == 0)
			return EINVAL;
		if (type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
			if (descriptors > VULKAN_POOL_SETS * 8U - textures)
				return ENOTSUP;
			textures += descriptors;
		} else if (type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
			if (descriptors > VULKAN_POOL_SETS * 4U - uniforms)
				return ENOTSUP;
			uniforms += descriptors;
		} else {
			return ENOTSUP;
		}
	}

	/* Complete creation framing precedes ordinary OOM replies, leaving the stream exactly at its next command. */
	allocator = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || allocator != 0 || present != 1 || identity == 0)
		return EINVAL;
	device = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DEVICE, device_id);
	if (device == NULL)
		return EINVAL;
	object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DESCRIPTOR_POOL, identity);
	if (object != NULL)
		return EEXIST;
	pool = kern_calloc(1, sizeof(*pool));
	if (pool == NULL) {
		pool_reply(reply, VK_ERROR_OUT_OF_HOST_MEMORY, 0);
		return 0;
	}

	/* A pool's logical device edge exists before its typed capacity declaration is published. */
	error = bcm2711_vulkan_object_retain(device);
	if (error != 0) {
		kern_free(pool);
		return error;
	}

	/* New pools have no charge; set constructors acquire it only with their independently retained complete graph. */
	pool->owner.device = device;
	pool->flags = flags;
	pool->maximum_sets = maximum;
	pool->maximum_textures = textures;
	pool->maximum_uniforms = uniforms;
	error = bcm2711_vulkan_object_publish(session, I915_VK_OBJ_DESCRIPTOR_POOL, identity, pool, release_pool, &object);
	if (error != 0) {
		retired = release_pool(session, pool);
		if (retired != 0)
			return retired;
		if (error == ENOMEM || error == ENOSPC) {
			pool_reply(reply, VK_ERROR_OUT_OF_HOST_MEMORY, 0);
			return 0;
		}

		/* Other namespace refusals preserve their exact native outcome. */
		return error;
	}

	/* Acknowledge only the fully owned immutable pool capacity declaration. */
	pool_reply(reply, VK_SUCCESS, identity);

	/* Succeeded: this open owns one same-device pool without granting any initialized set binding. */
	return 0;
}

/* Withdraws a selected pool's public sets and optionally its own identity while retained prepared owners remain valid. */
static int
retire_pool(
	struct bcm2711_vulkan_session *session,
	bool reset,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct bcm2711_vulkan_object *device;
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_descriptor_pool *pool;
	uint64_t device_id;
	uint64_t identity;
	uint64_t allocator;
	uint32_t flags;
	int error;
	int removed;

	/* Reset has a 32-bit flag word; ordinary destruction instead has its null allocator word. */
	device_id = drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	if (reset) {
		flags = drv_i915_wire_read_u32(reader);
		if (flags != 0)
			return ENOTSUP;
	} else {
		allocator = drv_i915_wire_read_u64(reader);
		if (allocator != 0)
			return EINVAL;
	}

	/* A null destroy has no pool or child identity to consume. */
	if (reader->error != 0)
		return EINVAL;
	if (!reset && identity == 0)
		return 0;
	device = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DEVICE, device_id);
	object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DESCRIPTOR_POOL, identity);
	if (device == NULL || object == NULL)
		return EINVAL;
	pool = object->payload;
	if (pool->owner.device != device)
		return EINVAL;

	/* Child identities retire before the pool's registry edge; old retained set charges remain until their final destructor. */
	error = withdraw_sets(session, object);
	if (!reset) {
		removed = bcm2711_vulkan_object_remove(session, I915_VK_OBJ_DESCRIPTOR_POOL, identity);
		if (error == 0)
			error = removed;
	}

	/* An actual native retirement failure cannot be reported as a successful pool reset. */
	if (error != 0)
		return error;
	if (reset)
		drv_i915_wire_reply_u32(reply, VK_SUCCESS);

	/* Succeeded: selected public identities retired while independent prepared data remained owned. */
	return 0;
}

/* Removes each selected pool child exactly once while preserving traversal ownership and every native retirement error. */
static int
withdraw_sets(
	struct bcm2711_vulkan_session *session,
	struct bcm2711_vulkan_object *pool)
{
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_object *next;
	struct bcm2711_vulkan_descriptor_set *set;
	int error;
	int removed;

	/* The pool's registry reference and each next child's registry reference keep this traversal stable. */
	error = 0;
	object = session->objects;
	while (object != NULL) {
		next = object->next;
		if (object->kind == I915_VK_OBJ_DESCRIPTOR_SET) {
			set = object->payload;
			if (set->pool == pool) {
				removed = bcm2711_vulkan_object_remove(session, I915_VK_OBJ_DESCRIPTOR_SET, object->identity);
				if (removed != 0 && error == 0)
					error = removed;
			}
		}

		/* Registry removal can retire only dependencies still protected by their own registry or independent owner. */
		object = next;
	}

	/* Every selected child is withdrawn even if one native dependency requires quarantine recovery. */
	if (error != 0)
		return error;

	/* Succeeded: only retained prepared set owners can still refer to this pool's old data. */
	return 0;
}

/* Releases pool metadata only after every current or old retained set has retired its capacity charge. */
static int
release_pool(
	struct bcm2711_vulkan_session *session,
	void *payload)
{
	struct bcm2711_vulkan_descriptor_pool *pool;
	int error;

	/* A set's independent pool reference keeps final destruction away from nonzero charges. */
	(void)session;
	pool = payload;
	if (pool->sets != 0 || pool->textures != 0 || pool->uniforms != 0)
		__builtin_trap();
	error = bcm2711_vulkan_object_release(pool->owner.device);
	kern_free(pool);
	if (error != 0)
		return error;

	/* Succeeded: no pool capacity, set or device dependency remains owned by this descriptor. */
	return 0;
}

/* Writes the ordinary pool creation result and exact typed output identity. */
static void
pool_reply(
	struct i915_wire_writer *reply,
	VkResult status,
	uint64_t identity)
{
	/* A failed ordinary creation has a present output pointer with a null identity. */
	drv_i915_wire_reply_u32(reply, (uint32_t)status);
	drv_i915_wire_reply_u64(reply, 1);
	drv_i915_wire_reply_u64(reply, identity);
}
