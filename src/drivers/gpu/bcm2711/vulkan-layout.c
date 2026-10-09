/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Native layouts copy exact client wire semantics and retain immutable dependencies through every later pipeline/set owner. */
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/vulkan-layout.h"

/* The shader compiler exposes finite graphics-only descriptor and push interfaces. */
#define VULKAN_LAYOUT_STAGES (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)

static int create_set_layout(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int create_pipeline_layout(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int creation_header(struct i915_wire_reader *reader, uint32_t expected, uint64_t *device_id);
static int creation_tail(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, uint64_t device_id, struct bcm2711_vulkan_object **device, uint64_t *identity);
static int publish_layout(struct bcm2711_vulkan_session *session, enum i915_vk_object_kind kind, uint64_t identity, struct bcm2711_vulkan_object *device, struct bcm2711_vulkan_input_owner *payload, int (*destroy)(struct bcm2711_vulkan_session *, void *), struct i915_wire_writer *reply);
static int destroy_layout(struct bcm2711_vulkan_session *session, enum i915_vk_object_kind kind, struct i915_wire_reader *reader);
static int release_set_layout(struct bcm2711_vulkan_session *session, void *payload);
static int release_pipeline_layout(struct bcm2711_vulkan_session *session, void *payload);
static void layout_reply(struct i915_wire_writer *reply, VkResult status, uint64_t identity);

/*
 * Routes immutable descriptor-set and pipeline-layout ownership without granting unsupported shader interface capabilities.
 */
int
bcm2711_vulkan_layout_dispatch(
	struct bcm2711_vulkan_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	/* Every implemented layout opcode has ordinary client creation or echoed-opcode destruction semantics. */
	*handled = 1;
	if (opcode != GPU_OP_CREATE_DESCRIPTOR_SET_LAYOUT && opcode != GPU_OP_DESTROY_DESCRIPTOR_SET_LAYOUT &&
	    opcode != GPU_OP_CREATE_PIPELINE_LAYOUT && opcode != GPU_OP_DESTROY_PIPELINE_LAYOUT) {
		*handled = 0;
		return 0;
	}

	/* Creation requires its acknowledged identity; destruction may still request only the opcode echo. */
	if (requested > 1)
		return EINVAL;
	if ((opcode == GPU_OP_CREATE_DESCRIPTOR_SET_LAYOUT || opcode == GPU_OP_CREATE_PIPELINE_LAYOUT) && requested != 1)
		return EINVAL;

	/* Typed dispatch keeps descriptor and pipeline layouts distinct even for equal numeric identities. */
	if (opcode == GPU_OP_CREATE_DESCRIPTOR_SET_LAYOUT)
		error = create_set_layout(session, reader, reply);
	else if (opcode == GPU_OP_CREATE_PIPELINE_LAYOUT)
		error = create_pipeline_layout(session, reader, reply);
	else if (opcode == GPU_OP_DESTROY_DESCRIPTOR_SET_LAYOUT)
		error = destroy_layout(session, I915_VK_OBJ_DESCRIPTOR_SET_LAYOUT, reader);
	else
		error = destroy_layout(session, I915_VK_OBJ_PIPELINE_LAYOUT, reader);
	if (error != 0)
		return error;

	/* Succeeded: one exact layout operation completed under the controller mutex. */
	return 0;
}

/* Copies bounded single-element bindings and independently retains every immutable sampler in canonical binding order. */
static int
create_set_layout(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct bcm2711_vulkan_set_layout description;
	struct bcm2711_vulkan_set_layout *layout;
	struct bcm2711_vulkan_binding_layout binding;
	struct bcm2711_vulkan_object *device;
	struct bcm2711_vulkan_object *sampler;
	struct bcm2711_vulkan_input_owner *owner;
	uint64_t immutable[BCM2711_VULKAN_LAYOUT_BINDINGS];
	uint64_t device_id;
	uint64_t identity;
	uint64_t count;
	uint64_t saved;
	uint32_t elements;
	uint32_t index;
	uint32_t previous;
	int error;
	int retired;

	/* The full record prefix owns no pointers or allocations. */
	error = creation_header(reader, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &device_id);
	if (error != 0)
		return error;
	kern_memset(&description, 0, sizeof(description));
	kern_memset(immutable, 0, sizeof(immutable));
	description.count = drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || description.count > BCM2711_VULKAN_LAYOUT_BINDINGS || count != description.count)
		return ENOTSUP;

	/* Each encoded binding names one sampled image or uniform buffer with only supported graphics-stage visibility. */
	for (index = 0; index < description.count; index++) {
		binding.number = drv_i915_wire_read_u32(reader);
		binding.type = (VkDescriptorType)drv_i915_wire_read_u32(reader);
		elements = drv_i915_wire_read_u32(reader);
		binding.stages = drv_i915_wire_read_u32(reader);
		binding.immutable = NULL;
		count = drv_i915_wire_read_u64(reader);
		if (reader->error != 0 || elements != 1 || count > 1 ||
		    binding.stages == 0 || (binding.stages & ~VULKAN_LAYOUT_STAGES) != 0)
			return ENOTSUP;
		if (binding.type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
			description.textures++;
		else if (binding.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
			description.uniforms++;
		else
			return ENOTSUP;

		/* Immutable sampler arrays are meaningful only for the supported combined-image binding. */
		saved = 0;
		if (count != 0) {
			if (binding.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
				return ENOTSUP;
			saved = drv_i915_wire_read_u64(reader);
			if (reader->error != 0 || saved == 0)
				return EINVAL;
		}

		/* Canonical ordering makes equivalent layouts independent of the client's array order. */
		previous = index;
		while (previous != 0 && description.bindings[previous - 1U].number > binding.number) {
			description.bindings[previous] = description.bindings[previous - 1U];
			immutable[previous] = immutable[previous - 1U];
			previous--;
		}

		/* A duplicate binding cannot alias another retained immutable descriptor. */
		if (previous != 0 && description.bindings[previous - 1U].number == binding.number)
			return EINVAL;
		description.bindings[previous] = binding;
		immutable[previous] = saved;
	}

	/* Limits agree with the queried finite native texture/uniform interface. */
	if (description.textures > 8 || description.uniforms > 4)
		return ENOTSUP;
	error = creation_tail(session, reader, device_id, &device, &identity);
	if (error != 0)
		return error;
	layout = kern_calloc(1, sizeof(*layout));
	if (layout == NULL) {
		layout_reply(reply, VK_ERROR_OUT_OF_HOST_MEMORY, 0);
		return 0;
	}

	/* The copied layout has no dependency ownership until each corresponding retain succeeds. */
	*layout = description;
	for (index = 0; index < layout->count; index++) {
		if (immutable[index] == 0)
			continue;
		sampler = bcm2711_vulkan_object_find(session, I915_VK_OBJ_SAMPLER, immutable[index]);
		if (sampler == NULL) {
			retired = release_set_layout(session, layout);
			if (retired != 0)
				return retired;
			return EINVAL;
		}

		/* The immutable sampler belongs to the exact logical device selected by the descriptor layout. */
		owner = sampler->payload;
		if (owner->device != device) {
			retired = release_set_layout(session, layout);
			if (retired != 0)
				return retired;
			return EINVAL;
		}

		/* Acquire only the immutable sampler edge validated for this exact device. */
		error = bcm2711_vulkan_object_retain(sampler);
		if (error != 0) {
			retired = release_set_layout(session, layout);
			if (retired != 0)
				return retired;
			return error;
		}

		/* Publish ownership only after the independent sampler reference exists. */
		layout->bindings[index].immutable = sampler;
	}

	/* The common layout publication additionally retains the exact device root. */
	error = publish_layout(session, I915_VK_OBJ_DESCRIPTOR_SET_LAYOUT, identity, device, &layout->owner, release_set_layout, reply);
	if (error != 0)
		return error;

	/* Succeeded: one canonical immutable descriptor interface is independently owned. */
	return 0;
}

/* Copies finite set interfaces and exact graphics-stage push permissions before retaining their layout dependencies. */
static int
create_pipeline_layout(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct bcm2711_vulkan_pipeline_layout description;
	struct bcm2711_vulkan_pipeline_layout *layout;
	struct bcm2711_vulkan_set_layout *set;
	struct bcm2711_vulkan_object *device;
	struct bcm2711_vulkan_object *object;
	uint64_t identities[BCM2711_VULKAN_PIPELINE_SETS];
	uint64_t device_id;
	uint64_t identity;
	uint64_t count;
	uint32_t ranges;
	uint32_t index;
	uint32_t word;
	uint32_t stages;
	uint32_t offset;
	uint32_t bytes;
	uint32_t textures;
	uint32_t uniforms;
	uint32_t declared_stages;
	int error;
	int retired;

	/* The pipeline interface references no generated-arena pointers. */
	error = creation_header(reader, VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, &device_id);
	if (error != 0)
		return error;
	kern_memset(&description, 0, sizeof(description));
	description.count = drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || description.count > BCM2711_VULKAN_PIPELINE_SETS || count != description.count)
		return ENOTSUP;
	for (index = 0; index < description.count; index++)
		identities[index] = drv_i915_wire_read_u64(reader);

	/* Every retained push word has exact vertex/fragment visibility inside the queried 128-byte range. */
	ranges = drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || ranges > 2 || count != ranges)
		return ENOTSUP;
	declared_stages = 0;
	for (index = 0; index < ranges; index++) {
		stages = drv_i915_wire_read_u32(reader);
		offset = drv_i915_wire_read_u32(reader);
		bytes = drv_i915_wire_read_u32(reader);
		if (reader->error != 0 || stages == 0 || (stages & ~VULKAN_LAYOUT_STAGES) != 0 ||
		    bytes == 0 || (offset & 3U) != 0 || (bytes & 3U) != 0 || offset > 128 || bytes > 128 - offset)
			return ENOTSUP;

		/* Each stage has one complete declared push range, even when separate ranges would not overlap in bytes. */
		if ((declared_stages & stages) != 0)
			return EINVAL;
		declared_stages |= stages;
		for (word = offset / 4U; word < (offset + bytes) / 4U; word++) {
			if ((description.push[word] & stages) != 0)
				return EINVAL;
			description.push[word] |= stages;
		}
	}

	/* All ordinary creation fields precede dependency acquisition and possible OOM replies. */
	error = creation_tail(session, reader, device_id, &device, &identity);
	if (error != 0)
		return error;
	layout = kern_calloc(1, sizeof(*layout));
	if (layout == NULL) {
		layout_reply(reply, VK_ERROR_OUT_OF_HOST_MEMORY, 0);
		return 0;
	}

	/* One pipeline interface bounds the combined descriptors across all its retained set layouts. */
	*layout = description;
	textures = 0;
	uniforms = 0;
	for (index = 0; index < layout->count; index++) {
		object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DESCRIPTOR_SET_LAYOUT, identities[index]);
		if (object == NULL) {
			retired = release_pipeline_layout(session, layout);
			if (retired != 0)
				return retired;
			return EINVAL;
		}

		/* Interface limits follow actual copied layouts, never just the number of declared set slots. */
		set = object->payload;
		textures += set->textures;
		uniforms += set->uniforms;
		if (set->owner.device != device || textures > 8 || uniforms > 4) {
			retired = release_pipeline_layout(session, layout);
			if (retired != 0)
				return retired;
			return ENOTSUP;
		}

		/* Acquire the validated set interface before assigning its owned pipeline slot. */
		error = bcm2711_vulkan_object_retain(object);
		if (error != 0) {
			retired = release_pipeline_layout(session, layout);
			if (retired != 0)
				return retired;
			return error;
		}

		/* This set slot owns exactly the dependency whose retain succeeded. */
		layout->sets[index] = object;
	}

	/* Complete publication retains the same device root independently of its set dependencies. */
	error = publish_layout(session, I915_VK_OBJ_PIPELINE_LAYOUT, identity, device, &layout->owner, release_pipeline_layout, reply);
	if (error != 0)
		return error;

	/* Succeeded: one immutable pipeline interface owns all its exact set and push permissions. */
	return 0;
}

/* Reads an exact ordinary creation record prefix without accepting unimplemented chained semantics. */
static int
creation_header(
	struct i915_wire_reader *reader,
	uint32_t expected,
	uint64_t *device_id)
{
	uint64_t present;
	uint64_t chain;
	uint32_t type;
	uint32_t flags;

	/* Parent, input pointer and standard header have the exact client field order. */
	*device_id = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	type = drv_i915_wire_read_u32(reader);
	chain = drv_i915_wire_read_u64(reader);
	flags = drv_i915_wire_read_u32(reader);
	if (reader->error != 0 || present != 1 || type != expected || chain != 0 || flags != 0)
		return ENOTSUP;

	/* Succeeded: the selected finite record body follows this supported prefix. */
	return 0;
}

/* Reads complete null allocator/output framing and resolves the actual live same-open device parent. */
static int
creation_tail(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	uint64_t device_id,
	struct bcm2711_vulkan_object **device,
	uint64_t *identity)
{
	uint64_t allocator;
	uint64_t present;

	/* Ordinary output publication never carries guest application allocator pointers into the kernel. */
	allocator = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	*identity = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || allocator != 0 || present != 1 || *identity == 0)
		return EINVAL;
	*device = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DEVICE, device_id);
	if (*device == NULL)
		return EINVAL;

	/* Succeeded: payload construction can acquire independent dependencies without leaving unread creation bytes. */
	return 0;
}

/* Acquires the device edge and publishes one complete layout, unwinding every partial owner on refusal. */
static int
publish_layout(
	struct bcm2711_vulkan_session *session,
	enum i915_vk_object_kind kind,
	uint64_t identity,
	struct bcm2711_vulkan_object *device,
	struct bcm2711_vulkan_input_owner *payload,
	int (*destroy)(struct bcm2711_vulkan_session *, void *),
	struct i915_wire_writer *reply)
{
	struct bcm2711_vulkan_object *object;
	int error;
	int retired;

	/* Dependency acquisition precedes typed registry publication. */
	error = bcm2711_vulkan_object_retain(device);
	if (error != 0) {
		retired = destroy(session, payload);
		if (retired != 0)
			return retired;
		return error;
	}

	/* The payload now owns its device independently of every child and public identity. */
	payload->device = device;
	error = bcm2711_vulkan_object_publish(session, kind, identity, payload, destroy, &object);
	if (error != 0) {
		retired = destroy(session, payload);
		if (retired != 0)
			return retired;
		if (error == ENOMEM || error == ENOSPC) {
			layout_reply(reply, VK_ERROR_OUT_OF_HOST_MEMORY, 0);
			return 0;
		}

		/* Identity refusal remains distinct from ordinary storage exhaustion. */
		return error;
	}

	/* Acknowledge only this fully owned typed layout. */
	layout_reply(reply, VK_SUCCESS, identity);

	/* Succeeded: the layout registry owns one complete immutable dependency graph. */
	return 0;
}

/* Withdraws one exact typed layout identity while sets, pipelines and prepared owners retain independent references. */
static int
destroy_layout(
	struct bcm2711_vulkan_session *session,
	enum i915_vk_object_kind kind,
	struct i915_wire_reader *reader)
{
	struct bcm2711_vulkan_object *device;
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_input_owner *owner;
	uint64_t device_id;
	uint64_t identity;
	uint64_t allocator;
	int error;

	/* Layout destruction has an opcode echo and no parameter reply body. */
	device_id = drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	allocator = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || allocator != 0)
		return EINVAL;
	if (identity == 0)
		return 0;
	device = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DEVICE, device_id);
	object = bcm2711_vulkan_object_find(session, kind, identity);
	if (device == NULL || object == NULL)
		return EINVAL;
	owner = object->payload;
	if (owner->device != device)
		return EINVAL;
	error = bcm2711_vulkan_object_remove(session, kind, identity);
	if (error != 0)
		return error;

	/* Succeeded: only this layout's public identity ownership retired. */
	return 0;
}

/* Releases every retained immutable sampler and the descriptor layout's independent device edge. */
static int
release_set_layout(
	struct bcm2711_vulkan_session *session,
	void *payload)
{
	struct bcm2711_vulkan_set_layout *layout;
	uint32_t index;
	int error;
	int retired;

	/* Partial construction stores only dependencies whose individual retain succeeded. */
	(void)session;
	layout = payload;
	error = 0;
	for (index = 0; index < layout->count; index++) {
		retired = bcm2711_vulkan_object_release(layout->bindings[index].immutable);
		if (retired != 0 && error == 0)
			error = retired;
	}

	/* Root metadata retires after all immutable sampler dependencies. */
	retired = bcm2711_vulkan_object_release(layout->owner.device);
	kern_free(layout);
	if (error != 0)
		return error;
	if (retired != 0)
		return retired;

	/* Succeeded: no immutable descriptor dependency remains owned by this layout. */
	return 0;
}

/* Releases every retained descriptor interface before retiring the pipeline layout's independent device edge. */
static int
release_pipeline_layout(
	struct bcm2711_vulkan_session *session,
	void *payload)
{
	struct bcm2711_vulkan_pipeline_layout *layout;
	uint32_t index;
	int error;
	int retired;

	/* Null slots have no acquired reference during partial interface construction. */
	(void)session;
	layout = payload;
	error = 0;
	for (index = 0; index < layout->count; index++) {
		retired = bcm2711_vulkan_object_release(layout->sets[index]);
		if (retired != 0 && error == 0)
			error = retired;
	}

	/* The root remains alive until all retained interfaces have released their own dependencies. */
	retired = bcm2711_vulkan_object_release(layout->owner.device);
	kern_free(layout);
	if (error != 0)
		return error;
	if (retired != 0)
		return retired;

	/* Succeeded: no retained pipeline interface or root remains owned by this layout. */
	return 0;
}

/* Writes the ordinary creation outcome and exact typed output identity. */
static void
layout_reply(
	struct i915_wire_writer *reply,
	VkResult status,
	uint64_t identity)
{
	/* The output pointer remains present even for a null failed creation identity. */
	drv_i915_wire_reply_u32(reply, (uint32_t)status);
	drv_i915_wire_reply_u64(reply, 1);
	drv_i915_wire_reply_u64(reply, identity);
}
