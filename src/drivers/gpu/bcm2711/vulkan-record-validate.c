/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Complete recording events validate only immutable interfaces; mutable descriptor contents are frozen at submission. */
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/vulkan-record.h"
#include "drivers/gpu/bcm2711/vulkan-descriptor.h"
#include "drivers/gpu/bcm2711/vulkan-resource.h"

static int same_device(struct bcm2711_vulkan_object *object, struct bcm2711_vulkan_object *device);
static int validate_sets(struct bcm2711_vulkan_command_buffer *command, const struct bcm2711_vulkan_record *record);
static int validate_vertices(struct bcm2711_vulkan_command_buffer *command, const struct bcm2711_vulkan_record *record);
static int validate_push(struct bcm2711_vulkan_command_buffer *command, const struct bcm2711_vulkan_record *record);
static int validate_pass(struct bcm2711_vulkan_command_buffer *command, const struct bcm2711_vulkan_record *record);
static int validate_viewport(const struct bcm2711_vulkan_record *record);

/*
 * Validates one copied event against its exact device, primary recording and finite native graphics interfaces.
 */
int
bcm2711_vulkan_record_validate(
	struct bcm2711_vulkan_command_buffer *command,
	const struct bcm2711_vulkan_record *record)
{
	int error;

	/* Resource contents are absent from recording validation; ordinary descriptor mutations separately invalidate recorded set generations. */
	error = 0;
	switch (record->opcode) {
	case GPU_OP_CMD_BIND_PIPELINE:
		error = same_device(record->objects[0], command->owner.device);
		break;
	case GPU_OP_CMD_BIND_DESCRIPTOR_SETS:
		error = validate_sets(command, record);
		break;
	case GPU_OP_CMD_BIND_VERTEX_BUFFERS:
		error = validate_vertices(command, record);
		break;
	case GPU_OP_CMD_PUSH_CONSTANTS:
		error = validate_push(command, record);
		break;
	case GPU_OP_CMD_BEGIN_RENDER_PASS:
		error = validate_pass(command, record);
		break;
	case GPU_OP_CMD_END_RENDER_PASS:
		if (!command->render_open)
			return EINVAL;
		break;
	case GPU_OP_CMD_SET_VIEWPORT:
		error = validate_viewport(record);
		break;
	case GPU_OP_CMD_SET_SCISSOR:
		/* Nonnegative offsets and signed sums are the actual standard scissor bounds, independent of render target clipping. */
		if (record->area.offset.x < 0 || record->area.offset.y < 0)
			return EINVAL;
		if (record->area.extent.width > 0x7fffffffU - (uint32_t)record->area.offset.x ||
		    record->area.extent.height > 0x7fffffffU - (uint32_t)record->area.offset.y)
			return EINVAL;
		break;
	case GPU_OP_CMD_DRAW:
		/* Keiland uses ordinary nonindexed single-instance triangles; zero-count draws remain legitimate no-ops. */
		if (!command->render_open)
			return EINVAL;
		if (record->words[1] > 1 || record->words[3] != 0)
			return ENOTSUP;
		if (record->words[0] > 0xffffffffU - record->words[2])
			return EOVERFLOW;
		break;
	default:
		return ENOTSUP;
	}

	/* A semantically refused void command becomes the primary recording's first error, never partial executable work. */
	if (error != 0)
		return error;

	/* Succeeded: the immutable event can acquire independent typed ownership. */
	return 0;
}

/* Resolves a typed input's actual device edge without accepting any same-number object of another kind. */
static int
same_device(
	struct bcm2711_vulkan_object *object,
	struct bcm2711_vulkan_object *device)
{
	struct bcm2711_vulkan_input_owner *owner;
	struct bcm2711_vulkan_resource *resource;

	/* Null selections cannot produce an owned recording input. */
	if (object == NULL)
		return EINVAL;
	if (object->kind == I915_VK_OBJ_BUFFER || object->kind == I915_VK_OBJ_IMAGE) {
		resource = object->payload;
		if (resource->device != device)
			return EINVAL;
	} else {
		owner = object->payload;
		if (owner->device != device)
			return EINVAL;
	}

	/* Succeeded: this exact input belongs to the primary command's retained logical device. */
	return 0;
}

/* Validates exact set layout definitions while leaving their mutable resource bindings available for later updates. */
static int
validate_sets(
	struct bcm2711_vulkan_command_buffer *command,
	const struct bcm2711_vulkan_record *record)
{
	struct bcm2711_vulkan_pipeline_layout *layout;
	struct bcm2711_vulkan_descriptor_set *set;
	uint32_t index;
	int error;

	/* Binding layout and the complete requested slice belong to this exact device/interface. */
	error = same_device(record->objects[0], command->owner.device);
	if (error != 0)
		return error;
	layout = record->objects[0]->payload;
	if (record->first > layout->count || record->count > layout->count - record->first)
		return EINVAL;

	/* Independently created identical definitions are compatible; public identity equality is not required. */
	for (index = 0; index < record->count; index++) {
		error = same_device(record->objects[index + 1U], command->owner.device);
		if (error != 0)
			return error;
		set = record->objects[index + 1U]->payload;
		error = bcm2711_vulkan_layout_set_compatible(layout->sets[record->first + index]->payload, set->layout->payload);
		if (error != 0)
			return error;
	}

	/* Succeeded: all selected set interfaces can be retained without prematurely snapshotting their contents. */
	return 0;
}

/* Checks logical vertex offsets independently of the complete draw-time fetch interval validated at submission. */
static int
validate_vertices(
	struct bcm2711_vulkan_command_buffer *command,
	const struct bcm2711_vulkan_record *record)
{
	struct bcm2711_vulkan_resource *resource;
	uint32_t index;
	int error;

	/* Every selected buffer must have bound vertex storage on this device before recording an owned fetch selection. */
	for (index = 0; index < record->count; index++) {
		error = same_device(record->objects[index], command->owner.device);
		if (error != 0)
			return error;
		resource = record->objects[index]->payload;
		if (resource->memory == NULL || (resource->usage & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) == 0)
			return EINVAL;
		if (record->offsets[index] >= resource->bytes)
			return EINVAL;
	}

	/* Succeeded: each selected logical vertex offset has an independently retainable bound resource. */
	return 0;
}

/* Checks the declared per-word push permissions instead of implicitly granting all graphics stages. */
static int
validate_push(
	struct bcm2711_vulkan_command_buffer *command,
	const struct bcm2711_vulkan_record *record)
{
	struct bcm2711_vulkan_pipeline_layout *layout;
	uint32_t index;
	uint32_t first;
	uint32_t count;
	int error;

	/* Only nonempty implemented vertex/fragment stage selections can receive raw push words. */
	if (record->stages == 0 || (record->stages & ~(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)) != 0)
		return ENOTSUP;
	error = same_device(record->objects[0], command->owner.device);
	if (error != 0)
		return error;
	layout = record->objects[0]->payload;

	/* Every selected word must be authorized for every requested stage by the retained layout definition. */
	first = record->first / 4U;
	count = record->count / 4U;
	for (index = first; index < first + count; index++) {
		if ((layout->push[index] & record->stages) != record->stages)
			return EINVAL;
	}

	/* Succeeded: complete copied push contents have the exact declared shader visibility. */
	return 0;
}

/* Validates compatible pass/framebuffer owners, complete colour clear selection and the exact bounded render area. */
static int
validate_pass(
	struct bcm2711_vulkan_command_buffer *command,
	const struct bcm2711_vulkan_record *record)
{
	struct bcm2711_vulkan_pass *pass;
	struct bcm2711_vulkan_framebuffer *framebuffer;
	int error;

	/* Primary inline passes cannot nest, and both target owners must belong to the recording device. */
	if (command->render_open)
		return EINVAL;
	error = same_device(record->objects[0], command->owner.device);
	if (error != 0)
		return error;
	error = same_device(record->objects[1], command->owner.device);
	if (error != 0)
		return error;
	pass = record->objects[0]->payload;
	framebuffer = record->objects[1]->payload;
	error = bcm2711_vulkan_pass_compatible(pass, framebuffer->owner.parent->payload);
	if (error != 0)
		return error;

	/* Clear selection is mandatory only for the actual CLEAR attachment; ignored colour entries were still consumed. */
	if (pass->colour.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR && record->count == 0)
		return EINVAL;
	if (record->area.offset.x < 0 || record->area.offset.y < 0)
		return EINVAL;
	if ((uint32_t)record->area.offset.x > framebuffer->width || (uint32_t)record->area.offset.y > framebuffer->height)
		return EINVAL;
	if (record->area.extent.width == 0 || record->area.extent.height == 0)
		return EINVAL;
	if (record->area.extent.width > framebuffer->width - (uint32_t)record->area.offset.x ||
	    record->area.extent.height > framebuffer->height - (uint32_t)record->area.offset.y)
		return EINVAL;

	/* Succeeded: this complete pass begin selects owned target storage within its actual declared extent. */
	return 0;
}

/* Checks IEEE sign/exponent/magnitude fields without enabling kernel floating-point execution. */
static int
validate_viewport(
	const struct bcm2711_vulkan_record *record)
{
	uint32_t index;
	uint32_t magnitude;

	/* Coordinates fit the advertised -4096..4096 bounds; NaN and infinity are outside this finite interface. */
	for (index = 0; index < 2; index++) {
		magnitude = record->words[index] & 0x7fffffffU;
		if (magnitude > 0x45800000U)
			return EINVAL;
	}

	/* Vulkan 1.0 width and height are positive and at most the advertised 4096 pixels. */
	for (index = 2; index < 4; index++) {
		if (record->words[index] == 0 || record->words[index] > 0x45800000U)
			return EINVAL;
	}

	/* Each depth endpoint lies in 0..1, with negative zero equivalent to zero and reversed ranges still permitted. */
	for (index = 4; index < 6; index++) {
		magnitude = record->words[index] & 0x7fffffffU;
		if (magnitude == 0)
			continue;
		if (record->words[index] > 0x3f800000U)
			return EINVAL;
	}

	/* Succeeded: exact copied IEEE words describe a finite admitted dynamic viewport. */
	return 0;
}
