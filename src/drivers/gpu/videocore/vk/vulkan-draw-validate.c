/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Draw preparation verifies shader-consumed logical intervals and exact dynamic state before acquiring native execution owners. */
#include <uapi/errno.h>

#include "drivers/gpu/videocore/vk/vulkan-draw.h"

static int validate_indices(const struct bcm2711_vulkan_draw_state *state, const struct bcm2711_vulkan_record *draw);
static int validate_vertices(const struct bcm2711_vulkan_draw_state *state, const struct bcm2711_vulkan_record *draw);
static int validate_attribute(const struct bcm2711_vulkan_draw_state *state, const struct bcm2711_vulkan_record *draw, const VkVertexInputAttributeDescription *attribute);
static int validate_uniforms(const struct bcm2711_vulkan_draw_state *state, const struct bcm2711_shader_binary *program);
static int validate_texture(const struct bcm2711_vulkan_draw_state *state, const struct bcm2711_vulkan_descriptor *descriptor);
static int validate_storage_image(const struct bcm2711_vulkan_descriptor *descriptor);
static int validate_feedback(const struct bcm2711_vulkan_resource *image, uint32_t address, struct bcm2711_vulkan_image_view *target_view);

/*
 * Checks the complete actual attachment backing before any clear-only or drawing pass can be prepared.
 */
int
bcm2711_vulkan_draw_target(
	const struct bcm2711_vulkan_record *record)
{
	struct bcm2711_vulkan_framebuffer *framebuffer;
	struct bcm2711_vulkan_image_view *image_view;
	struct bcm2711_vulkan_resource *image;
	struct bcm2711_v3d_view *view;
	void *cpu;
	uint32_t address;
	uint32_t slot;
	int error;

	/* Immutable framebuffer creation already bounds its extent; current native storage may subsequently be quarantined. */
	framebuffer = record->objects[1]->payload;
	for (slot = 0; slot < BCM2711_VULKAN_PASS_COLOURS; slot++) {
		if (framebuffer->colours[slot] == NULL)
			continue;
		image_view = framebuffer->colours[slot]->payload;
		image = image_view->owner.parent->payload;
		error = bcm2711_vulkan_resource_backing(image, 0, image->bytes, &view, &address, &cpu);
		if (error != 0)
			return error;
	}

	/* A depth/stencil attachment's image is bound to current storage as well. */
	if (framebuffer->depth != NULL) {
		image_view = framebuffer->depth->payload;
		image = image_view->owner.parent->payload;
		error = bcm2711_vulkan_resource_backing(image, 0, image->bytes, &view, &address, &cpu);
		if (error != 0)
			return error;
	}

	/* Succeeded: even a pass containing no draw can own the exact complete attachment storage. */
	return 0;
}

/*
 * Validates a complete borrowed draw state and every exact shader-consumed native input interval.
 */
int
bcm2711_vulkan_draw_validate(
	const struct bcm2711_vulkan_draw_state *state,
	const struct bcm2711_vulkan_record *draw)
{
	struct bcm2711_vulkan_pass *pass;
	uint32_t stage;
	int error;

	/* No default dynamic state, target or pipeline is invented for an incomplete recording; a static viewport or scissor is the pipeline's. */
	if (state->pass == NULL || state->pipeline == NULL)
		return EINVAL;
	if ((state->viewport == NULL && !state->pipeline->static_viewport) ||
	    (state->scissor == NULL && !state->pipeline->static_scissor))
		return EINVAL;

	/* Pipeline and current pass may use independently created but compatible single-colour definitions. */
	pass = state->pass->objects[0]->payload;
	error = bcm2711_vulkan_pass_compatible(pass, state->pipeline->pass->payload);
	if (error != 0)
		return error;

	/* Coordinate and render shaders share an exact fetch interface; a zero-count draw performs no vertex memory reads. */
	if (draw->words[0] != 0 && draw->words[1] != 0) {
		if (draw->opcode == GPU_OP_CMD_DRAW_INDEXED) {
			error = validate_indices(state, draw);
			if (error != 0)
				return error;
		}
		error = validate_vertices(state, draw);
		if (error != 0)
			return error;
	}

	/* Every native uniform stream consumes only defined, compatible push words and descriptor spans. */
	for (stage = 0; stage < 3; stage++) {
		error = validate_uniforms(state, state->pipeline->programs[stage]);
		if (error != 0)
			return error;
	}

	/* Succeeded: the callback can prepare this exact target, fetch and shader interface without an unchecked logical read. */
	return 0;
}

/*
 * Validates one dispatch through the view of the compute bind point: a
 * bound compute pipeline whose program has defined push words and bound
 * descriptors of the right type in current native storage.
 */
int
bcm2711_vulkan_dispatch_validate(
	const struct bcm2711_vulkan_draw_state *view,
	const struct bcm2711_vulkan_record *dispatch)
{
	int error;

	/* No pipeline is invented for an incomplete recording; recording bounded the direct counts. */
	(void)dispatch;
	if (view->pipeline == NULL || !view->pipeline->compute)
		return EINVAL;

	/* Every uniform the program reads. */
	error = validate_uniforms(view, view->pipeline->programs[0]);
	if (error != 0)
		return error;

	/* Succeeded: the dispatch's inputs are defined and bounded. */
	return 0;
}

/*
 * Resolves one shader descriptor through its current compatible, undisturbed bound set and canonical binding number.
 */
int
bcm2711_vulkan_draw_descriptor(
	const struct bcm2711_vulkan_draw_state *state,
	const struct bcm2711_shader_uniform *uniform,
	VkDescriptorType type,
	const struct bcm2711_vulkan_descriptor **descriptor)
{
	const struct bcm2711_vulkan_pipeline_layout *pipeline_layout;
	const struct bcm2711_vulkan_set_layout *set_layout;
	const struct bcm2711_vulkan_draw_set *bound;
	uint32_t index;
	int error;

	/* A disturbed or never-selected set supplies no default descriptor. */
	*descriptor = NULL;
	if (state->pipeline == NULL || uniform->set >= BCM2711_VULKAN_PIPELINE_SETS)
		return EINVAL;
	bound = &state->sets[uniform->set];
	if (bound->set == NULL || bound->layout == NULL)
		return EINVAL;

	/* Binding compatibility includes every lower set and exact push range grouping. */
	pipeline_layout = state->pipeline->owner.parent->payload;
	error = bcm2711_vulkan_layout_compatible(bound->layout, pipeline_layout, uniform->set);
	if (error != 0)
		return error;

	/* Canonical declared binding numbers are independent of their application declaration order; a uniform buffer may be dynamic. */
	set_layout = bound->set->layout->payload;
	for (index = 0; index < set_layout->count; index++) {
		if (set_layout->bindings[index].number != uniform->binding || set_layout->bindings[index].element != uniform->element)
			continue;
		error = bcm2711_vulkan_layout_type_matches(set_layout->bindings[index].type, type);
		if (error != 0)
			return error;
		*descriptor = &bound->set->bindings[index];
		break;
	}

	/* No declared binding can supply this compiled shader transaction. */
	if (*descriptor == NULL)
		return EINVAL;

	/* Succeeded: the caller has a borrowed exact current binding under the controller mutex. */
	return 0;
}

/*
 * Reports where a shader's uniform buffer starts in its buffer: the
 * descriptor's offset, plus the bind's dynamic offset for a dynamic one.
 *
 * The whole range the descriptor names must lie in the buffer at that
 * offset, as Vulkan requires of a dynamic offset.
 */
int
bcm2711_vulkan_draw_dynamic_offset(
	const struct bcm2711_vulkan_draw_state *state,
	const struct bcm2711_shader_uniform *uniform,
	const struct bcm2711_vulkan_descriptor *descriptor,
	uint64_t *offset)
{
	const struct bcm2711_vulkan_draw_set *bound;
	const struct bcm2711_vulkan_resource *resource;
	uint32_t slot;

	/* The descriptor's canonical slot in its bound set, which bcm2711_vulkan_draw_descriptor resolved. */
	bound = &state->sets[uniform->set];
	slot = (uint32_t)(descriptor - bound->set->bindings);
	if (slot >= BCM2711_VULKAN_LAYOUT_BINDINGS || descriptor->buffer == NULL)
		return EINVAL;

	/* The offset, whose range stays in the buffer. */
	resource = descriptor->buffer->payload;
	*offset = descriptor->offset + bound->dynamic[slot];
	if (*offset > resource->bytes || descriptor->bytes > resource->bytes - *offset)
		return EINVAL;

	/* Succeeded: the uniform buffer starts at the offset. */
	return 0;
}

/* Checks that an indexed draw's indices lie inside the bound index buffer's current storage. */
static int
validate_indices(
	const struct bcm2711_vulkan_draw_state *state,
	const struct bcm2711_vulkan_record *draw)
{
	struct bcm2711_v3d_view *view;
	void *cpu;
	uint64_t begin;
	uint64_t bytes;
	uint32_t address;
	uint32_t size;
	int error;

	/* No index buffer is invented for an indexed draw recorded before any bind. */
	if (state->indices == NULL)
		return EINVAL;

	/* Recording admitted only the 16- and 32-bit index types. */
	size = 2U;
	if (state->index_type == VK_INDEX_TYPE_UINT32)
		size = 4U;

	/* The first index and the count are bounded in 64-bit arithmetic against the buffer's logical bytes. */
	begin = state->index_offset + (uint64_t)draw->words[2] * size;
	bytes = (uint64_t)draw->words[0] * size;
	if (begin > state->indices->bytes || bytes > state->indices->bytes - begin)
		return EINVAL;

	/* The whole interval lies in current coherent, nonquarantined storage. */
	error = bcm2711_vulkan_resource_backing(state->indices, begin, bytes, &view, &address, &cpu);
	if (error != 0)
		return error;

	/* Succeeded: the hardware reads only indices of this buffer. */
	return 0;
}

/* Checks each shader-consumed declared attribute once against its entire nonindexed vertex range. */
static int
validate_vertices(
	const struct bcm2711_vulkan_draw_state *state,
	const struct bcm2711_vulkan_record *draw)
{
	const struct bcm2711_shader_binary *program;
	const VkVertexInputAttributeDescription *attribute;
	uint32_t index;
	uint32_t input;
	bool used;
	int error;

	/* Unused attribute declarations do not force an application to bind an unread vertex stream. */
	program = state->pipeline->programs[BCM2711_SHADER_VERTEX];
	for (index = 0; index < state->pipeline->attribute_count; index++) {
		attribute = &state->pipeline->attributes[index];
		used = false;
		for (input = 0; input < program->input_count; input++) {
			if (program->inputs[input].location == attribute->location)
				used = true;
		}

		/* Complete native fetch bytes for each consumed attribute stay inside the resource's logical size. */
		if (!used)
			continue;
		error = validate_attribute(state, draw, attribute);
		if (error != 0)
			return error;
	}

	/* Succeeded: every shader-consumed attribute can form a complete bounded native fetch record. */
	return 0;
}

/* Resolves an exact binding stride and bounds its last fetched vertex without relying on allocation padding. */
static int
validate_attribute(
	const struct bcm2711_vulkan_draw_state *state,
	const struct bcm2711_vulkan_record *draw,
	const VkVertexInputAttributeDescription *attribute)
{
	const VkVertexInputBindingDescription *binding;
	struct bcm2711_vulkan_resource *resource;
	struct bcm2711_v3d_view *view;
	void *cpu;
	uint64_t begin;
	uint64_t bytes;
	uint32_t address;
	uint32_t index;
	uint32_t components;
	uint32_t first;
	uint32_t count;
	int error;

	/* Pipeline creation bounds native binding numbers and admitted float formats. */
	if (attribute->binding >= BCM2711_VULKAN_VERTEX_BINDINGS)
		return EINVAL;
	resource = state->vertices[attribute->binding];
	if (resource == NULL)
		return EINVAL;
	binding = NULL;
	for (index = 0; index < state->pipeline->binding_count; index++) {
		if (state->pipeline->bindings[index].binding == attribute->binding)
			binding = &state->pipeline->bindings[index];
	}

	/* Missing declarations cannot inherit an implicit stride. */
	if (binding == NULL)
		return EINVAL;
	components = 0;

	/* These are the contiguous 32-bit float and integer formats admitted by native pipeline creation. */
	switch (attribute->format) {
	case VK_FORMAT_R32_SFLOAT:
	case VK_FORMAT_R32_SINT:
	case VK_FORMAT_R32_UINT:
		components = 1;
		break;
	case VK_FORMAT_R32G32_SFLOAT:
	case VK_FORMAT_R32G32_SINT:
	case VK_FORMAT_R32G32_UINT:
		components = 2;
		break;
	case VK_FORMAT_R32G32B32_SFLOAT:
	case VK_FORMAT_R32G32B32_SINT:
	case VK_FORMAT_R32G32B32_UINT:
		components = 3;
		break;
	case VK_FORMAT_R32G32B32A32_SFLOAT:
	case VK_FORMAT_R32G32B32A32_SINT:
	case VK_FORMAT_R32G32B32A32_UINT:
		components = 4;
		break;
	default:
		return ENOTSUP;
	}

	/*
	 * The elements the draw fetches: its vertices, or for a per-instance
	 * binding its instances.  An indexed draw's vertices are whatever its
	 * indices name, which the fetch's maximum index keeps inside the
	 * buffer, so only the first element is checked here.
	 */
	if (binding->inputRate == VK_VERTEX_INPUT_RATE_INSTANCE) {
		first = draw->words[3];
		if (draw->opcode == GPU_OP_CMD_DRAW_INDEXED)
			first = draw->words[4];
		count = draw->words[1];
	} else if (draw->opcode == GPU_OP_CMD_DRAW_INDEXED) {
		first = 0;
		count = 1;
	} else {
		first = draw->words[2];
		count = draw->words[0];
	}

	/* The first element plus stride and complete attribute bytes are bounded in 64-bit arithmetic. */
	begin = state->offsets[attribute->binding];
	if (begin > resource->bytes || attribute->offset > resource->bytes - begin)
		return EINVAL;
	begin += attribute->offset;
	bytes = (uint64_t)first * binding->stride;
	if (bytes > resource->bytes - begin)
		return EINVAL;
	begin += bytes;
	bytes = (uint64_t)(count - 1U) * binding->stride + components * 4U;
	error = bcm2711_vulkan_resource_backing(resource, begin, bytes, &view, &address, &cpu);
	if (error != 0)
		return error;

	/* Succeeded: the complete native fetch interval lies inside current coherent, nonquarantined logical storage. */
	return 0;
}

/* Verifies all actual native uniform transactions against current stage-specific pushed values and bound descriptors. */
static int
validate_uniforms(
	const struct bcm2711_vulkan_draw_state *state,
	const struct bcm2711_shader_binary *program)
{
	const struct bcm2711_vulkan_pipeline_layout *layout;
	const struct bcm2711_shader_uniform *uniform;
	const struct bcm2711_vulkan_descriptor *descriptor;
	struct bcm2711_vulkan_resource *resource;
	struct bcm2711_v3d_view *view;
	void *cpu;
	uint64_t offset;
	uint64_t base;
	uint32_t address;
	uint32_t stage;
	uint32_t index;
	uint32_t word;
	int error;

	/* Both coordinate and vertex native variants consume the vertex Vulkan stage's independently pushed words. */
	layout = state->pipeline->owner.parent->payload;
	stage = 0;
	if (program->stage == BCM2711_SHADER_FRAGMENT)
		stage = 1;
	for (index = 0; index < program->uniform_count; index++) {
		uniform = &program->uniforms[index];

		/* Each compiled uniform kind has its own exact draw-time input contract. */
		switch (uniform->kind) {
		case BCM2711_SHADER_PUSH:
			if ((uniform->offset & 3U) != 0 || uniform->offset >= 128)
				return EINVAL;
			word = uniform->offset / 4U;
			if (state->push_layouts[stage][word] == NULL)
				return EINVAL;
			error = bcm2711_vulkan_layout_push_compatible(state->push_layouts[stage][word], layout);
			if (error != 0)
				return error;
			break;
		case BCM2711_SHADER_BLOCK:
			error = bcm2711_vulkan_draw_descriptor(state, uniform, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &descriptor);
			if (error != 0)
				return error;
			if (descriptor->buffer == NULL || (uniform->offset & 3U) != 0)
				return EINVAL;
			if (uniform->offset > descriptor->bytes || 4U > descriptor->bytes - uniform->offset)
				return EINVAL;

			/* The word's place in the buffer, after a dynamic buffer's offset. */
			error = bcm2711_vulkan_draw_dynamic_offset(state, uniform, descriptor, &base);
			if (error != 0)
				return error;
			resource = descriptor->buffer->payload;
			offset = base + uniform->offset;
			if (offset < base)
				return EOVERFLOW;
			error = bcm2711_vulkan_resource_backing(resource, offset, 4, &view, &address, &cpu);
			if (error != 0)
				return error;
			break;
		case BCM2711_SHADER_TEXTURE:
		case BCM2711_SHADER_SAMPLER:
			error = bcm2711_vulkan_draw_descriptor(state, uniform, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &descriptor);
			if (error != 0)
				return error;
			error = validate_texture(state, descriptor);
			if (error != 0)
				return error;
			break;
		case BCM2711_SHADER_STORAGE_BASE:
		case BCM2711_SHADER_STORAGE_LIMIT:
		case BCM2711_SHADER_STORAGE_SIZE:
			/* A compute or vertex program's storage buffer: a whole bound range in current native storage. */
			if (program->stage == BCM2711_SHADER_FRAGMENT)
				return EINVAL;
			error = bcm2711_vulkan_draw_descriptor(state, uniform, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &descriptor);
			if (error != 0)
				return error;
			if (descriptor->buffer == NULL || descriptor->bytes == 0)
				return EINVAL;
			resource = descriptor->buffer->payload;
			error = bcm2711_vulkan_resource_backing(resource, descriptor->offset, descriptor->bytes, &view, &address, &cpu);
			if (error != 0)
				return error;
			break;
		case BCM2711_SHADER_IMAGE_BASE:
		case BCM2711_SHADER_IMAGE_LIMIT:
		case BCM2711_SHADER_IMAGE_PITCH:
		case BCM2711_SHADER_IMAGE_LAYER:
		case BCM2711_SHADER_IMAGE_SIZE:
			/* A compute program's storage image: a storage view in the general layout, its image in current native storage. */
			if (program->stage != BCM2711_SHADER_COMPUTE)
				return EINVAL;
			error = bcm2711_vulkan_draw_descriptor(state, uniform, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &descriptor);
			if (error != 0)
				return error;
			error = validate_storage_image(descriptor);
			if (error != 0)
				return error;
			break;
		case BCM2711_SHADER_SHARED_BASE:
		case BCM2711_SHADER_SHARED_LIMIT:
			/* The dispatch's arena; the program declares shared memory. */
			if (program->stage != BCM2711_SHADER_COMPUTE || program->shared_bytes == 0)
				return EINVAL;
			break;
		case BCM2711_SHADER_GROUP_COUNT:
		case BCM2711_SHADER_GROUP_BASE:
			/* The dispatch supplies them along one of its three axes. */
			if (program->stage != BCM2711_SHADER_COMPUTE || uniform->bits > 2)
				return EINVAL;
			break;
		case BCM2711_SHADER_TEXTURE_SIZE:
			/* The view's size: a bound combined image sampler with a view. */
			error = bcm2711_vulkan_draw_descriptor(state, uniform, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &descriptor);
			if (error != 0)
				return error;
			if (descriptor->view == NULL || uniform->bits > 3U)
				return EINVAL;
			break;
		case BCM2711_SHADER_VERTEX_BASE:
		case BCM2711_SHADER_INSTANCE_BASE:
			/* The draw supplies a vertex program's bases. */
			if (program->stage != BCM2711_SHADER_COORDINATE && program->stage != BCM2711_SHADER_VERTEX)
				return EINVAL;
			break;
		case BCM2711_SHADER_VIEW_INDEX:
			/* The pass supplies the view of a graphics program. */
			if (program->stage == BCM2711_SHADER_COMPUTE)
				return EINVAL;
			break;
		case BCM2711_SHADER_CONSTANT:
		case BCM2711_SHADER_VIEWPORT_X:
		case BCM2711_SHADER_VIEWPORT_Y:
		case BCM2711_SHADER_VIEWPORT_Z:
		case BCM2711_SHADER_DEPTH_OFFSET:
			break;
		default:
			return ENOTSUP;
		}
	}

	/* Succeeded: every actual compiled uniform transaction has defined, compatible and bounded native inputs. */
	return 0;
}

/* Refuses a storage image descriptor without a storage view in the general layout or without current native storage. */
static int
validate_storage_image(
	const struct bcm2711_vulkan_descriptor *descriptor)
{
	struct bcm2711_vulkan_image_view *image_view;
	struct bcm2711_vulkan_resource *image;
	struct bcm2711_v3d_view *view;
	void *cpu;
	uint32_t address;
	uint64_t bytes;
	int error;

	/* The written view of an image with storage usage. */
	if (descriptor->view == NULL || descriptor->sampler != NULL)
		return EINVAL;
	image_view = descriptor->view->payload;
	image = image_view->owner.parent->payload;
	if ((image->usage & VK_IMAGE_USAGE_STORAGE_BIT) == 0 || descriptor->image_layout != VK_IMAGE_LAYOUT_GENERAL)
		return EINVAL;

	/* Every layer and level of the image remains in current coherent native storage until the job's owner retires. */
	bytes = image->storage_bytes;
	if (bytes == 0)
		bytes = image->bytes;
	error = bcm2711_vulkan_resource_backing(image, 0, bytes, &view, &address, &cpu);
	if (error != 0)
		return error;

	/* Succeeded: the dispatch may read and write the image. */
	return 0;
}

/* Refuses undefined sampled inputs, attachment feedback and aliasing before exposing a full image interval to native TMU state. */
static int
validate_texture(
	const struct bcm2711_vulkan_draw_state *state,
	const struct bcm2711_vulkan_descriptor *descriptor)
{
	struct bcm2711_vulkan_image_view *image_view;
	struct bcm2711_vulkan_framebuffer *framebuffer;
	struct bcm2711_vulkan_resource *image;
	struct bcm2711_v3d_view *view;
	void *cpu;
	uint32_t address;
	uint32_t slot;
	int error;

	/* Ordinary sampled images require an initialized exact sampler/view and an implemented read layout. */
	if (descriptor->view == NULL || descriptor->sampler == NULL)
		return EINVAL;
	image_view = descriptor->view->payload;
	image = image_view->owner.parent->payload;
	if ((image->usage & VK_IMAGE_USAGE_SAMPLED_BIT) == 0)
		return EINVAL;
	if (descriptor->image_layout != VK_IMAGE_LAYOUT_GENERAL &&
	    descriptor->image_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
	    (descriptor->image_layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL || !bcm2711_vulkan_depth_format(image->format)))
		return EINVAL;

	/* A complete single-level raster image remains in current coherent native storage until the eventual job's owner retires. */
	error = bcm2711_vulkan_resource_backing(image, 0, image->bytes, &view, &address, &cpu);
	if (error != 0)
		return error;

	/* The current implementation has no attachment feedback path: no colour target may be the sampled image or share its storage. */
	framebuffer = state->pass->objects[1]->payload;
	for (slot = 0; slot < BCM2711_VULKAN_PASS_COLOURS; slot++) {
		if (framebuffer->colours[slot] == NULL)
			continue;
		error = validate_feedback(image, address, framebuffer->colours[slot]->payload);
		if (error != 0)
			return error;
	}

	/* Succeeded: the borrowed descriptor supplies a complete nonfeedback image/sampler selection. */
	return 0;
}

/* Refuses a sampled image (bound at address) that is a colour target's image or shares its storage. */
static int
validate_feedback(
	const struct bcm2711_vulkan_resource *image,
	uint32_t address,
	struct bcm2711_vulkan_image_view *target_view)
{
	struct bcm2711_vulkan_resource *target;
	struct bcm2711_v3d_view *target_backing;
	void *target_cpu;
	uint32_t target_address;
	int error;

	/* The same image, or an overlapping binding of the same memory. */
	target = target_view->owner.parent->payload;
	if (image == target)
		return ENOTSUP;
	if (image->memory == target->memory) {
		if (image->offset < target->offset + target->bytes && target->offset < image->offset + image->bytes)
			return ENOTSUP;
	}

	/* Resolve the attachment's actual mapped span as well, because separate imports may own one underlying native buffer. */
	error = bcm2711_vulkan_resource_backing(target, 0, target->bytes, &target_backing, &target_address, &target_cpu);
	if (error != 0)
		return error;

	/* Independent imported memory identities may still refer to the same native mapped storage interval. */
	if ((uint64_t)address < (uint64_t)target_address + target->bytes &&
	    (uint64_t)target_address < (uint64_t)address + image->bytes)
		return ENOTSUP;

	/* Succeeded: the target is not the sampled image. */
	return 0;
}
