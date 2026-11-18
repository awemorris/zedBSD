/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Complete recording events validate only immutable interfaces; mutable descriptor contents are frozen at submission. */
#include <uapi/errno.h>

#include "drivers/gpu/videocore/vk/vulkan-record.h"
#include "drivers/gpu/videocore/vk/vulkan-descriptor.h"
#include "drivers/gpu/videocore/vk/vulkan-resource.h"
#include "drivers/gpu/videocore/vk/vulkan-occlusion.h"

static int validate_clear(struct bcm2711_vulkan_command_buffer *command, const struct bcm2711_vulkan_record *record);
static int validate_depth_clear(struct bcm2711_vulkan_command_buffer *command, const struct bcm2711_vulkan_record *record);
static int same_device(struct bcm2711_vulkan_object *object, struct bcm2711_vulkan_object *device);
static int validate_sets(struct bcm2711_vulkan_command_buffer *command, const struct bcm2711_vulkan_record *record);
static int validate_vertices(struct bcm2711_vulkan_command_buffer *command, const struct bcm2711_vulkan_record *record);
static int validate_push(struct bcm2711_vulkan_command_buffer *command, const struct bcm2711_vulkan_record *record);
static int validate_indices(struct bcm2711_vulkan_command_buffer *command, const struct bcm2711_vulkan_record *record);
static int validate_pass(struct bcm2711_vulkan_command_buffer *command, const struct bcm2711_vulkan_record *record);
static int validate_viewport(const struct bcm2711_vulkan_record *record);
static int validate_attachments(struct bcm2711_vulkan_command_buffer *command, const struct bcm2711_vulkan_record *record);
static int validate_pipeline(struct bcm2711_vulkan_command_buffer *command, const struct bcm2711_vulkan_record *record);
static int validate_indirect(struct bcm2711_vulkan_command_buffer *command, const struct bcm2711_vulkan_record *record);
static int validate_query(struct bcm2711_vulkan_command_buffer *command, const struct bcm2711_vulkan_record *record);

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
		error = validate_pipeline(command, record);
		break;
	case GPU_OP_CMD_DISPATCH:
		/* A dispatch runs outside a render pass with at most 65535 workgroups along each axis. */
		if (command->render_open)
			return EINVAL;
		if (record->words[0] > 65535U || record->words[1] > 65535U || record->words[2] > 65535U)
			return EINVAL;
		break;
	case GPU_OP_CMD_DISPATCH_INDIRECT:
		error = validate_indirect(command, record);
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
	case GPU_OP_CMD_CLEAR_COLOR_IMAGE:
		error = validate_clear(command, record);
		break;
	case GPU_OP_CMD_CLEAR_DEPTH_STENCIL_IMAGE:
		error = validate_depth_clear(command, record);
		break;
	case GPU_OP_CMD_END_RENDER_PASS:
		if (!command->render_open)
			return EINVAL;
		break;
	case GPU_OP_CMD_CLEAR_ATTACHMENTS:
		error = validate_attachments(command, record);
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
	case GPU_OP_CMD_SET_LINE_WIDTH:
		/* A width of 1 to 32 pixels (the reported range; NaN fails). */
		if (record->words[0] < 0x3f800000U || record->words[0] > 0x42000000U)
			return EINVAL;
		break;
	case GPU_OP_CMD_SET_DEPTH_BIAS:
		/* Any factors and clamp: the depthBiasClamp feature is present. */
		break;
	case GPU_OP_CMD_SET_BLEND_CONSTANTS:
		break;
	case GPU_OP_CMD_SET_STENCIL_COMPARE_MASK:
	case GPU_OP_CMD_SET_STENCIL_WRITE_MASK:
	case GPU_OP_CMD_SET_STENCIL_REFERENCE:
		/* The face mask names the front face, the back face or both. */
		if (record->first == 0 || (record->first & ~(uint32_t)VK_STENCIL_FACE_FRONT_AND_BACK) != 0)
			return EINVAL;
		break;
	case GPU_OP_CMD_BIND_INDEX_BUFFER:
		error = validate_indices(command, record);
		break;
	case GPU_OP_CMD_BEGIN_QUERY:
	case GPU_OP_CMD_END_QUERY:
	case GPU_OP_CMD_RESET_QUERY_POOL:
		error = validate_query(command, record);
		break;
	case GPU_OP_CMD_DRAW:
	case GPU_OP_CMD_DRAW_INDEXED:
		/* Triangle lists inside a pass; zero-count draws remain legitimate no-ops. */
		if (!command->render_open)
			return EINVAL;

		/* The first vertex (or index) and first instance plus their counts stay representable. */
		if (record->words[0] > 0xffffffffU - record->words[2])
			return EOVERFLOW;
		if (record->opcode == GPU_OP_CMD_DRAW && record->words[1] > 0xffffffffU - record->words[3])
			return EOVERFLOW;
		if (record->opcode == GPU_OP_CMD_DRAW_INDEXED && record->words[1] > 0xffffffffU - record->words[4])
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
	const struct bcm2711_vulkan_set_layout *set_layout;
	uint32_t index;
	uint32_t binding;
	uint32_t dynamic;
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

	/* The sets' dynamic uniform buffers take one offset each. */
	dynamic = 0;
	for (index = 0; index < record->count; index++) {
		set = record->objects[index + 1U]->payload;
		set_layout = set->layout->payload;
		for (binding = 0; binding < set_layout->count; binding++) {
			if (set_layout->bindings[binding].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
				dynamic++;
		}
	}

	/* Every offset is given, aligned as minUniformBufferOffsetAlignment says. */
	if (dynamic != record->dynamic_count)
		return EINVAL;
	for (index = 0; index < record->dynamic_count; index++) {
		if ((record->words[index] & 3U) != 0)
			return EINVAL;
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

/* Checks one index buffer selection: bound index storage on this device, a 16- or 32-bit type and an offset aligned to it. */
static int
validate_indices(
	struct bcm2711_vulkan_command_buffer *command,
	const struct bcm2711_vulkan_record *record)
{
	struct bcm2711_vulkan_resource *resource;
	uint32_t size;
	int error;

	/* The buffer belongs to the recording device and was created and bound for index reads. */
	error = same_device(record->objects[0], command->owner.device);
	if (error != 0)
		return error;
	if (record->objects[0]->kind != I915_VK_OBJ_BUFFER)
		return EINVAL;
	resource = record->objects[0]->payload;
	if (resource->memory == NULL || (resource->usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT) == 0)
		return EINVAL;

	/* Only the two Vulkan 1.0 index types exist; the offset is a multiple of the index size. */
	if (record->first == VK_INDEX_TYPE_UINT16)
		size = 2U;
	else if (record->first == VK_INDEX_TYPE_UINT32)
		size = 4U;
	else
		return ENOTSUP;
	if (record->offsets[0] >= resource->bytes || (record->offsets[0] & (size - 1U)) != 0)
		return EINVAL;

	/* Succeeded: draws can read whole indices from the selected offset. */
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

	/* Only nonempty implemented vertex, fragment and compute stage selections can receive raw push words. */
	if (record->stages == 0 ||
	    (record->stages & ~(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT)) != 0)
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
	uint32_t slot;
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

	/* A cleared colour attachment takes its value from its own entry; ignored entries were still consumed. */
	for (slot = 0; slot < pass->colour_count; slot++) {
		if (pass->colour_used[slot] &&
		    pass->colours[slot].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR &&
		    record->count <= pass->colour_attachment[slot])
			return EINVAL;
	}

	/* A cleared depth/stencil attachment takes its values from its own entry. */
	if (pass->depth &&
	    (pass->depth_stencil.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR ||
	     pass->depth_stencil.stencilLoadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) &&
	    record->count <= pass->depth_attachment)
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

/* Validates one independently retainable colour image clear of whole subresources without reading its mutable execution-time layout. */
static int
validate_clear(
	struct bcm2711_vulkan_command_buffer *command,
	const struct bcm2711_vulkan_record *record)
{
	struct bcm2711_vulkan_colour_target colour;
	struct bcm2711_vulkan_resource *image;
	uint32_t index;
	uint32_t level;
	uint32_t levels;
	uint32_t layer;
	uint32_t layers;
	int error;

	/* A complete void refusal becomes End's outcome, and image clears must occur outside every render pass. */
	if (record->semantic_error != 0)
		return record->semantic_error;

	/* Transfer clears do not inherit a render pass attachment or user draw state. */
	if (command->render_open)
		return EINVAL;

	/* Recording kept every range it consumed. */
	if (record->count == 0 || record->count > BCM2711_VULKAN_CLEAR_RANGES)
		return EINVAL;

	/* Only the implemented transfer destination and general layouts may be selected explicitly. */
	if (record->layout != VK_IMAGE_LAYOUT_GENERAL && record->layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
		return EINVAL;

	/* The exact typed image belongs to the primary logical device. */
	error = same_device(record->objects[0], command->owner.device);
	if (error != 0)
		return error;

	/* A different typed payload cannot describe image storage. */
	if (record->objects[0]->kind != I915_VK_OBJ_IMAGE)
		return EINVAL;

	/* The typed image must already have a complete memory binding and transfer destination usage. */
	image = record->objects[0]->payload;
	if (image->memory == NULL || (image->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0)
		return EINVAL;

	/* The tile buffer clears a colour attachment format (the decoder refused the 4.2's others). */
	error = bcm2711_vulkan_colour_target(image->format, &colour);
	if (error != 0)
		return ENOTSUP;

	/* Every range names levels and layers the image has; each level of a 3D image is cleared in all its slices. */
	for (index = 0; index < record->count; index++) {
		error = bcm2711_vulkan_image_clear_range(image, record->words[BCM2711_VULKAN_CLEAR_RANGE_WORD + index], &level, &levels, &layer, &layers);
		if (error != 0)
			return error;
	}

	/* Succeeded: current layout is checked later in FIFO execution after preceding transitions and native writes retire. */
	return 0;
}

/*
 * Validates a depth/stencil image clear: outside a render pass, in a
 * transfer destination or general layout, of a bound depth/stencil image
 * that may be written by transfers, of aspects its format has.
 */
static int
validate_depth_clear(
	struct bcm2711_vulkan_command_buffer *command,
	const struct bcm2711_vulkan_record *record)
{
	struct bcm2711_vulkan_resource *image;
	uint32_t aspects;
	int error;

	/* A refused range is End's outcome; an image clear is outside every render pass. */
	if (record->semantic_error != 0)
		return record->semantic_error;
	if (command->render_open)
		return EINVAL;
	if (record->count == 0 || record->count > 64U)
		return EINVAL;

	/* Only the transfer destination and general layouts may be named. */
	if (record->layout != VK_IMAGE_LAYOUT_GENERAL && record->layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
		return EINVAL;

	/* The image belongs to the primary's device and is a bound depth/stencil image a transfer may write. */
	error = same_device(record->objects[0], command->owner.device);
	if (error != 0)
		return error;
	if (record->objects[0]->kind != I915_VK_OBJ_IMAGE)
		return EINVAL;
	image = record->objects[0]->payload;
	if (image->memory == NULL ||
	    !bcm2711_vulkan_depth_format(image->format) ||
	    (image->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0)
		return EINVAL;

	/* The layers cleared are the image's. */
	if (record->first != VK_REMAINING_ARRAY_LAYERS && record->first > bcm2711_vulkan_image_layers(image))
		return EINVAL;

	/* Stencil only in D24S8; depth in every depth/stencil format. */
	aspects = VK_IMAGE_ASPECT_DEPTH_BIT;
	if (image->format == VK_FORMAT_D24_UNORM_S8_UINT)
		aspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
	if ((record->words[2] & ~aspects) != 0)
		return EINVAL;

	/* Succeeded: the layout is checked when the clear runs, after the work before it. */
	return 0;
}

/* Checks a clear of the current pass's attachments: inside a pass, with nonempty rectangles of nonnegative origin. */
static int
validate_attachments(
	struct bcm2711_vulkan_command_buffer *command,
	const struct bcm2711_vulkan_record *record)
{
	const uint32_t *rect;
	uint32_t index;

	/* The attachments are the open pass's. */
	if (!command->render_open)
		return EINVAL;

	/* Each rectangle is nonempty and its far edges stay representable; the pass's render area bounds it at execution. */
	for (index = 0; index < record->count; index++) {
		rect = &record->words[BCM2711_VULKAN_CLEAR_RECT_WORD + index * 4U];
		if ((int32_t)rect[0] < 0 ||
		    (int32_t)rect[1] < 0 ||
		    rect[2] == 0 ||
		    rect[3] == 0)
			return EINVAL;
		if (rect[2] > 0x7fffffffU - rect[0] || rect[3] > 0x7fffffffU - rect[1])
			return EINVAL;
	}

	/* Succeeded: the clear can run where the pass's draws do. */
	return 0;
}

/* Checks a pipeline bind: a pipeline of the device, of the kind its bind point takes. */
static int
validate_pipeline(
	struct bcm2711_vulkan_command_buffer *command,
	const struct bcm2711_vulkan_record *record)
{
	const struct bcm2711_vulkan_pipeline *pipeline;
	int error;

	/* The pipeline belongs to the recording device. */
	error = same_device(record->objects[0], command->owner.device);
	if (error != 0)
		return error;

	/* A compute pipeline binds only to the compute point (kept as the compute stage), a graphics one only to the graphics point. */
	pipeline = record->objects[0]->payload;
	if (pipeline->compute && record->stages != VK_SHADER_STAGE_COMPUTE_BIT)
		return EINVAL;
	if (!pipeline->compute && record->stages != 0)
		return EINVAL;

	/* Succeeded: later draws or dispatches find the pipeline at its own bind point. */
	return 0;
}

/* Checks an indirect dispatch: outside a render pass, its three aligned counts inside a bound indirect buffer of the device. */
static int
validate_indirect(
	struct bcm2711_vulkan_command_buffer *command,
	const struct bcm2711_vulkan_record *record)
{
	const struct bcm2711_vulkan_resource *resource;
	int error;

	/* A dispatch runs outside a render pass. */
	if (command->render_open)
		return EINVAL;

	/* The buffer belongs to the device, is bound and was created for indirect reads. */
	error = same_device(record->objects[0], command->owner.device);
	if (error != 0)
		return error;
	if (record->objects[0]->kind != I915_VK_OBJ_BUFFER)
		return EINVAL;
	resource = record->objects[0]->payload;
	if (resource->memory == NULL || (resource->usage & VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT) == 0)
		return EINVAL;

	/* Three words at a four-byte aligned offset, inside the buffer; the counts are read when the dispatch runs. */
	if ((record->offsets[0] & 3U) != 0 ||
	    record->offsets[0] > resource->bytes ||
	    resource->bytes - record->offsets[0] < 12U)
		return EINVAL;

	/* Succeeded: the dispatch can read its counts at execution. */
	return 0;
}

/*
 * Validates an occlusion query's begin, end or reset: a pool of the device,
 * queries inside it, a reset outside a render pass, and a begin's control
 * flags (only VK_QUERY_CONTROL_PRECISE_BIT).  Which query is active is the
 * draw walk's to check.
 */
static int
validate_query(
	struct bcm2711_vulkan_command_buffer *command,
	const struct bcm2711_vulkan_record *record)
{
	const struct bcm2711_vulkan_occlusion_pool *pool;

	/* An occlusion pool of the command buffer's device. */
	pool = bcm2711_vulkan_occlusion_record(record);
	if (pool == NULL || pool->device != command->owner.device)
		return EINVAL;

	/* The queries lie inside the pool. */
	if (record->count == 0U || record->first > pool->count || record->count > pool->count - record->first)
		return EINVAL;

	/* A reset is a transfer outside any render pass; a begin counts exactly whatever it asks. */
	if (record->opcode == GPU_OP_CMD_RESET_QUERY_POOL && command->render_open)
		return EINVAL;
	if (record->opcode == GPU_OP_CMD_BEGIN_QUERY && (record->words[0] & ~(uint32_t)VK_QUERY_CONTROL_PRECISE_BIT) != 0U)
		return EINVAL;

	/* Succeeded: the command can be recorded. */
	return 0;
}
