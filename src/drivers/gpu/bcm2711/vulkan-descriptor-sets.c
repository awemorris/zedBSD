/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Complete set batches acquire independent pool/layout/device ownership before any public allocation succeeds. */
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/vulkan-descriptor.h"

/* One immutable command keeps batch allocation/free bookkeeping finite on the kernel stack. */
#define VULKAN_SET_BATCH 64U

static int allocate_sets(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int free_sets(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int prepare_set(struct bcm2711_vulkan_session *session, struct bcm2711_vulkan_object *device, struct bcm2711_vulkan_object *pool, struct bcm2711_vulkan_object *layout, struct bcm2711_vulkan_descriptor_set **created);
static int release_set(struct bcm2711_vulkan_session *session, void *payload);
static void allocation_reply(struct i915_wire_writer *reply, VkResult status, uint32_t count, const uint64_t *identities);

/*
 * Routes all-or-nothing descriptor allocation batches and exact selected-set identity retirement.
 */
int
bcm2711_vulkan_descriptor_sets_dispatch(
	struct bcm2711_vulkan_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	/* Other typed routers retain pool creation/reset and mutable binding update responsibilities. */
	*handled = 1;
	if (opcode != GPU_OP_ALLOCATE_DESCRIPTOR_SETS && opcode != GPU_OP_FREE_DESCRIPTOR_SETS) {
		*handled = 0;
		return 0;
	}

	/* Both operations need their explicit Vulkan allocation/free result. */
	if (requested != 1)
		return EINVAL;
	if (opcode == GPU_OP_ALLOCATE_DESCRIPTOR_SETS)
		error = allocate_sets(session, reader, reply);
	else
		error = free_sets(session, reader, reply);
	if (error != 0)
		return error;

	/* Succeeded: the acknowledged batch has an exact independently retained ownership outcome. */
	return 0;
}

/*
 * Releases one descriptor's independently retained native resource edges while preserving the first retirement error.
 */
int
bcm2711_vulkan_descriptor_release(
	struct bcm2711_vulkan_descriptor *descriptor)
{
	int error;
	int retired;

	/* Each initialized field owns one exact reference; uninitialized fields have no ownership. */
	error = bcm2711_vulkan_object_release(descriptor->view);
	retired = bcm2711_vulkan_object_release(descriptor->sampler);
	if (retired != 0 && error == 0)
		error = retired;
	retired = bcm2711_vulkan_object_release(descriptor->buffer);
	if (retired != 0 && error == 0)
		error = retired;
	kern_memset(descriptor, 0, sizeof(*descriptor));

	/* Logical fields retire even if a native view remains in independent VA quarantine. */
	if (error != 0)
		return error;

	/* Succeeded: no native input is owned by this cleared descriptor. */
	return 0;
}

/* Allocates a complete selected batch, rolling back every partial identity, charge and dependency on ordinary failure. */
static int
allocate_sets(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct bcm2711_vulkan_descriptor_set *prepared[VULKAN_SET_BATCH];
	struct bcm2711_vulkan_object *published[VULKAN_SET_BATCH];
	struct bcm2711_vulkan_object *layouts[VULKAN_SET_BATCH];
	struct bcm2711_vulkan_object *device;
	struct bcm2711_vulkan_object *pool_object;
	struct bcm2711_vulkan_object *existing;
	struct bcm2711_vulkan_descriptor_pool *pool;
	struct bcm2711_vulkan_set_layout *layout;
	uint64_t layout_ids[VULKAN_SET_BATCH];
	uint64_t identities[VULKAN_SET_BATCH];
	uint64_t device_id;
	uint64_t pool_id;
	uint64_t present;
	uint64_t chain;
	uint64_t array;
	uint32_t type;
	uint32_t count;
	uint32_t index;
	uint32_t previous;
	uint32_t textures;
	uint32_t uniforms;
	int error;
	int retired;
	int retirement_error;

	/* Consume the real client's complete allocation record and guest-reserved output identity array before any publication. */
	device_id = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	type = drv_i915_wire_read_u32(reader);
	chain = drv_i915_wire_read_u64(reader);
	pool_id = drv_i915_wire_read_u64(reader);
	count = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || present != 1 || type != VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO ||
	    chain != 0 || count == 0 || count > VULKAN_SET_BATCH || array != count)
		return ENOTSUP;
	for (index = 0; index < count; index++)
		layout_ids[index] = drv_i915_wire_read_u64(reader);
	array = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || array != count)
		return EINVAL;
	for (index = 0; index < count; index++)
		identities[index] = drv_i915_wire_read_u64(reader);

	/* Device and pool identities are exact same-open typed owners. */
	if (reader->error != 0)
		return EINVAL;
	device = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DEVICE, device_id);
	pool_object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DESCRIPTOR_POOL, pool_id);
	if (device == NULL || pool_object == NULL)
		return EINVAL;
	pool = pool_object->payload;
	if (pool->owner.device != device)
		return EINVAL;

	/* Verify every same-device interface, fresh output and whole batch capacity before acquiring a set dependency. */
	textures = 0;
	uniforms = 0;
	for (index = 0; index < count; index++) {
		layouts[index] = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DESCRIPTOR_SET_LAYOUT, layout_ids[index]);
		if (layouts[index] == NULL || identities[index] == 0)
			return EINVAL;
		layout = layouts[index]->payload;
		if (layout->owner.device != device)
			return EINVAL;
		textures += layout->textures;
		uniforms += layout->uniforms;
		existing = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DESCRIPTOR_SET, identities[index]);
		if (existing != NULL)
			return EEXIST;
		for (previous = 0; previous < index; previous++) {
			if (identities[previous] == identities[index])
				return EEXIST;
		}
	}

	/* Old retained sets keep charges even after pool reset has removed their public identities. */
	if (pool->sets > pool->maximum_sets || pool->textures > pool->maximum_textures ||
	    pool->uniforms > pool->maximum_uniforms || count > pool->maximum_sets - pool->sets ||
	    textures > pool->maximum_textures - pool->textures || uniforms > pool->maximum_uniforms - pool->uniforms) {
		allocation_reply(reply, VK_ERROR_OUT_OF_POOL_MEMORY, 0, NULL);
		return 0;
	}

	/* Each construction and registry allocation can fail ordinarily; keep exact acquired owners for rollback. */
	kern_memset(prepared, 0, sizeof(prepared));
	kern_memset(published, 0, sizeof(published));
	error = 0;
	for (index = 0; index < count; index++) {
		error = prepare_set(session, device, pool_object, layouts[index], &prepared[index]);
		if (error != 0)
			break;
		error = bcm2711_vulkan_object_publish(session, I915_VK_OBJ_DESCRIPTOR_SET, identities[index], prepared[index], release_set, &published[index]);
		if (error != 0)
			break;
	}

	/* A refused batch acknowledges none of its outputs and restores every partial dependency and capacity charge. */
	if (error != 0) {
		retirement_error = 0;
		for (index = 0; index < count; index++) {
			retired = 0;
			if (published[index] != NULL)
				retired = bcm2711_vulkan_object_remove(session, I915_VK_OBJ_DESCRIPTOR_SET, identities[index]);
			else if (prepared[index] != NULL)
				retired = release_set(session, prepared[index]);
			if (retired != 0 && retirement_error == 0)
				retirement_error = retired;
		}

		/* Native retirement failure takes precedence over an otherwise structured ordinary allocation failure. */
		if (retirement_error != 0)
			return retirement_error;
		if (error == ENOMEM || error == ENOSPC) {
			allocation_reply(reply, VK_ERROR_OUT_OF_HOST_MEMORY, 0, NULL);
			return 0;
		}

		/* Other typed ownership refusals retain their exact native meaning. */
		return error;
	}

	/* Acknowledge only the complete independently retained batch in its original client output order. */
	allocation_reply(reply, VK_SUCCESS, count, identities);

	/* Succeeded: every requested set identity owns a complete pool/interface/resource dependency graph. */
	return 0;
}

/* Frees only exact selected same-pool public set identities after validating the entire batch. */
static int
free_sets(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct bcm2711_vulkan_object *device;
	struct bcm2711_vulkan_object *pool_object;
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_descriptor_pool *pool;
	struct bcm2711_vulkan_descriptor_set *set;
	uint64_t identities[VULKAN_SET_BATCH];
	uint64_t device_id;
	uint64_t pool_id;
	uint64_t array;
	uint32_t count;
	uint32_t index;
	uint32_t previous;
	int error;
	int retired;

	/* The client's explicit count and encoded array must describe the same complete bounded free batch. */
	device_id = drv_i915_wire_read_u64(reader);
	pool_id = drv_i915_wire_read_u64(reader);
	count = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > VULKAN_SET_BATCH || array != count)
		return EINVAL;
	for (index = 0; index < count; index++)
		identities[index] = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;
	device = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DEVICE, device_id);
	pool_object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DESCRIPTOR_POOL, pool_id);
	if (device == NULL || pool_object == NULL)
		return EINVAL;
	pool = pool_object->payload;
	if (pool->owner.device != device || (pool->flags & VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT) == 0)
		return EINVAL;

	/* No partial free occurs for a wrong pool, stale identity or repeated selection. */
	for (index = 0; index < count; index++) {
		object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DESCRIPTOR_SET, identities[index]);
		if (object == NULL)
			return EINVAL;
		set = object->payload;
		if (set->pool != pool_object || set->owner.device != device)
			return EINVAL;

		/* A pending prepared primary freezes every ordinary set before any selected free prefix is withdrawn. */
		if (set->pending != 0)
			return EBUSY;
		for (previous = 0; previous < index; previous++) {
			if (identities[previous] == identities[index])
				return EINVAL;
		}
	}

	/* Public identities retire now; prepared owners keep their old binding storage and pool charges independently. */
	error = 0;
	for (index = 0; index < count; index++) {
		retired = bcm2711_vulkan_object_remove(session, I915_VK_OBJ_DESCRIPTOR_SET, identities[index]);
		if (retired != 0 && error == 0)
			error = retired;
	}

	/* A native dependency retirement failure cannot masquerade as a successful free batch. */
	if (error != 0)
		return error;
	drv_i915_wire_reply_u32(reply, VK_SUCCESS);

	/* Succeeded: the selected public set ownership edges retired exactly once. */
	return 0;
}

/* Prepares one complete retained set and charges capacity only after each dependency and immutable sampler exists. */
static int
prepare_set(
	struct bcm2711_vulkan_session *session,
	struct bcm2711_vulkan_object *device,
	struct bcm2711_vulkan_object *pool_object,
	struct bcm2711_vulkan_object *layout_object,
	struct bcm2711_vulkan_descriptor_set **created)
{
	struct bcm2711_vulkan_descriptor_set *set;
	struct bcm2711_vulkan_descriptor_pool *pool;
	struct bcm2711_vulkan_set_layout *layout;
	struct bcm2711_vulkan_object *sampler;
	uint32_t index;
	int error;
	int retired;

	/* Partial preparation never transfers an incomplete payload to the batch rollback owner. */
	*created = NULL;
	set = kern_calloc(1, sizeof(*set));
	if (set == NULL)
		return ENOMEM;
	error = bcm2711_vulkan_object_retain(device);
	if (error == 0) {
		set->owner.device = device;
		error = bcm2711_vulkan_object_retain(pool_object);
	}

	/* Assign each ownership field only after its exact dependency retain succeeds. */
	if (error == 0) {
		set->pool = pool_object;
		error = bcm2711_vulkan_object_retain(layout_object);
	}

	/* The layout remains immutable while each individually retained sampler field is prepared. */
	if (error == 0) {
		set->layout = layout_object;
		layout = layout_object->payload;
		for (index = 0; index < layout->count; index++) {
			sampler = layout->bindings[index].immutable;
			if (sampler == NULL)
				continue;
			error = bcm2711_vulkan_object_retain(sampler);
			if (error != 0)
				break;
			set->bindings[index].sampler = sampler;
		}
	}

	/* One failed dependency unwinds only fields whose owned reference was actually published in this payload. */
	if (error != 0) {
		retired = release_set(session, set);
		if (retired != 0)
			return retired;
		return error;
	}

	/* The whole batch verified capacity before entering this serialized preparation loop. */
	pool = pool_object->payload;
	layout = layout_object->payload;
	pool->sets++;
	pool->textures += layout->textures;
	pool->uniforms += layout->uniforms;
	set->charged = true;
	*created = set;

	/* Succeeded: the batch owns one complete independently retained unpublished set payload. */
	return 0;
}

/* Retires initialized binding edges, capacity charges and every independent pool/layout/device dependency. */
static int
release_set(
	struct bcm2711_vulkan_session *session,
	void *payload)
{
	struct bcm2711_vulkan_descriptor_set *set;
	struct bcm2711_vulkan_descriptor_pool *pool;
	struct bcm2711_vulkan_set_layout *layout;
	uint32_t index;
	int error;
	int retired;

	/* Each binding retires its native owners even if another binding encountered uncertain storage retirement. */
	(void)session;
	set = payload;
	error = 0;
	for (index = 0; index < BCM2711_VULKAN_LAYOUT_BINDINGS; index++) {
		retired = bcm2711_vulkan_descriptor_release(&set->bindings[index]);
		if (retired != 0 && error == 0)
			error = retired;
	}

	/* Charge lifetime follows the last set owner, independently of public free/reset/destroy timing. */
	if (set->charged) {
		pool = set->pool->payload;
		layout = set->layout->payload;
		if (pool->sets == 0 || pool->textures < layout->textures || pool->uniforms < layout->uniforms)
			__builtin_trap();
		pool->sets--;
		pool->textures -= layout->textures;
		pool->uniforms -= layout->uniforms;
	}

	/* A pool cannot reach final metadata destruction until all old set charges have retired. */
	retired = bcm2711_vulkan_object_release(set->layout);
	if (retired != 0 && error == 0)
		error = retired;
	retired = bcm2711_vulkan_object_release(set->pool);
	if (retired != 0 && error == 0)
		error = retired;
	retired = bcm2711_vulkan_object_release(set->owner.device);
	if (retired != 0 && error == 0)
		error = retired;
	kern_free(set);
	if (error != 0)
		return error;

	/* Succeeded: no binding, set capacity or immutable dependency remains owned by this retired payload. */
	return 0;
}

/* Writes the complete batch result and exact native output identities in the client's original order. */
static void
allocation_reply(
	struct i915_wire_writer *reply,
	VkResult status,
	uint32_t count,
	const uint64_t *identities)
{
	uint32_t index;

	/* Failed batches have no acknowledged output identities; successful batches preserve the exact selected count. */
	drv_i915_wire_reply_u32(reply, (uint32_t)status);
	drv_i915_wire_reply_u64(reply, count);
	for (index = 0; index < count; index++)
		drv_i915_wire_reply_u64(reply, identities[index]);
}
