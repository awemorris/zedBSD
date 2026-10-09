/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Draw preparation verifies shader-consumed logical intervals and exact dynamic state before acquiring native execution owners. */
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/vulkan-draw.h"

static int validate_vertices(const struct bcm2711_vulkan_draw_state *state, const struct bcm2711_vulkan_record *draw);
static int validate_attribute(const struct bcm2711_vulkan_draw_state *state, const struct bcm2711_vulkan_record *draw, const VkVertexInputAttributeDescription *attribute);
static int validate_uniforms(const struct bcm2711_vulkan_draw_state *state, const struct bcm2711_shader_binary *program);
static int validate_texture(const struct bcm2711_vulkan_draw_state *state, const struct bcm2711_vulkan_descriptor *descriptor);

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
	int error;

	/* Immutable framebuffer creation already bounds its extent; current native storage may subsequently be quarantined. */
	framebuffer = record->objects[1]->payload;
	image_view = framebuffer->view->payload;
	image = image_view->owner.parent->payload;
	error = bcm2711_vulkan_resource_backing(image, 0, image->bytes, &view, &address, &cpu);
	if (error != 0)
		return error;

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

	/* No default dynamic state, target or pipeline is invented for an incomplete recording. */
	if (state->pass == NULL ||
	    state->pipeline == NULL ||
	    state->viewport == NULL ||
	    state->scissor == NULL)
		return EINVAL;

	/* Pipeline and current pass may use independently created but compatible single-colour definitions. */
	pass = state->pass->objects[0]->payload;
	error = bcm2711_vulkan_pass_compatible(pass, state->pipeline->pass->payload);
	if (error != 0)
		return error;

	/* Coordinate and render shaders share an exact fetch interface; a zero-count draw performs no vertex memory reads. */
	if (draw->words[0] != 0 && draw->words[1] != 0) {
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

	/* Canonical declared binding numbers are independent of their application declaration order. */
	set_layout = bound->set->layout->payload;
	for (index = 0; index < set_layout->count; index++) {
		if (set_layout->bindings[index].number != uniform->binding)
			continue;
		if (set_layout->bindings[index].type != type)
			return EINVAL;
		*descriptor = &bound->set->bindings[index];
		break;
	}

	/* No declared binding can supply this compiled shader transaction. */
	if (*descriptor == NULL)
		return EINVAL;

	/* Succeeded: the caller has a borrowed exact current binding under the controller mutex. */
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
	program = state->pipeline->programs[BCM2711_SHADER_COORDINATE];
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

	/* These are the contiguous float formats admitted by native pipeline creation. */
	switch (attribute->format) {
	case VK_FORMAT_R32_SFLOAT:
		components = 1;
		break;
	case VK_FORMAT_R32G32_SFLOAT:
		components = 2;
		break;
	case VK_FORMAT_R32G32B32_SFLOAT:
		components = 3;
		break;
	case VK_FORMAT_R32G32B32A32_SFLOAT:
		components = 4;
		break;
	default:
		return ENOTSUP;
	}

	/* The first vertex plus stride and complete attribute bytes are bounded in 64-bit arithmetic. */
	begin = state->offsets[attribute->binding];
	if (begin > resource->bytes || attribute->offset > resource->bytes - begin)
		return EINVAL;
	begin += attribute->offset;
	bytes = (uint64_t)draw->words[2] * binding->stride;
	if (bytes > resource->bytes - begin)
		return EINVAL;
	begin += bytes;
	bytes = (uint64_t)(draw->words[0] - 1U) * binding->stride + components * 4U;
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
			resource = descriptor->buffer->payload;
			offset = descriptor->offset + uniform->offset;
			if (offset < descriptor->offset)
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

/* Refuses undefined sampled inputs, attachment feedback and aliasing before exposing a full image interval to native TMU state. */
static int
validate_texture(
	const struct bcm2711_vulkan_draw_state *state,
	const struct bcm2711_vulkan_descriptor *descriptor)
{
	struct bcm2711_vulkan_image_view *image_view;
	struct bcm2711_vulkan_framebuffer *framebuffer;
	struct bcm2711_vulkan_image_view *target_view;
	struct bcm2711_vulkan_resource *image;
	struct bcm2711_vulkan_resource *target;
	struct bcm2711_v3d_view *view;
	struct bcm2711_v3d_view *target_backing;
	void *cpu;
	void *target_cpu;
	uint32_t address;
	uint32_t target_address;
	int error;

	/* Ordinary sampled images require an initialized exact sampler/view and an implemented read layout. */
	if (descriptor->view == NULL || descriptor->sampler == NULL)
		return EINVAL;
	if (descriptor->image_layout != VK_IMAGE_LAYOUT_GENERAL && descriptor->image_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
		return EINVAL;
	image_view = descriptor->view->payload;
	image = image_view->owner.parent->payload;
	if ((image->usage & VK_IMAGE_USAGE_SAMPLED_BIT) == 0)
		return EINVAL;

	/* The current implementation has no attachment feedback path, including distinct image handles aliasing one memory span. */
	framebuffer = state->pass->objects[1]->payload;
	target_view = framebuffer->view->payload;
	target = target_view->owner.parent->payload;
	if (image == target)
		return ENOTSUP;
	if (image->memory == target->memory) {
		if (image->offset < target->offset + target->bytes && target->offset < image->offset + image->bytes)
			return ENOTSUP;
	}

	/* A complete single-level raster image remains in current coherent native storage until the eventual job's owner retires. */
	error = bcm2711_vulkan_resource_backing(image, 0, image->bytes, &view, &address, &cpu);
	if (error != 0)
		return error;

	/* Resolve the attachment's actual mapped span as well, because separate imports may own one underlying native buffer. */
	error = bcm2711_vulkan_resource_backing(target, 0, target->bytes, &target_backing, &target_address, &target_cpu);
	if (error != 0)
		return error;

	/* Independent imported memory identities may still refer to the same native mapped storage interval. */
	if ((uint64_t)address < (uint64_t)target_address + target->bytes &&
	    (uint64_t)target_address < (uint64_t)address + image->bytes)
		return ENOTSUP;

	/* Succeeded: the borrowed descriptor supplies a complete nonfeedback image/sampler selection. */
	return 0;
}
