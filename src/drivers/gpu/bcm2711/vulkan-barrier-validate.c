/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Native synchronization supports one serialized graphics family and one complete coherent colour subresource per image. */
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/vulkan-barrier.h"

/* All admitted operations complete serially; stages unsupported by the native advertised graphics features remain refused. */
#define BARRIER_STAGES (VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT | VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)

/* Core Vulkan 1.0 access bits have complete stronger serial ordering; feature-dependent extension accesses are not advertised. */
#define BARRIER_ACCESS 0x1ffffU

static int validate_entry(const struct bcm2711_vulkan_barrier_entry *entry, struct bcm2711_vulkan_object *device);
static int validate_image(const struct bcm2711_vulkan_barrier_entry *entry, const struct bcm2711_vulkan_resource *resource);
static int colour_layout(VkImageLayout layout, bool destination);

/*
 * Validates complete copied synchronization scopes and independently retainable resources without changing layout state.
 */
int
bcm2711_vulkan_barrier_validate(
	const struct bcm2711_vulkan_barrier *barrier,
	struct bcm2711_vulkan_object *device)
{
	uint32_t index;
	uint32_t previous;
	int error;

	/* Decode-time unsupported structure or stale typed selection survives complete framing as an ordinary void refusal. */
	if (barrier->semantic_error != 0)
		return barrier->semantic_error;
	if (barrier->count > BCM2711_VULKAN_BARRIERS)
		return EINVAL;
	if (barrier->source == 0 || barrier->destination == 0)
		return EINVAL;
	if ((barrier->source & ~BARRIER_STAGES) != 0 || (barrier->destination & ~BARRIER_STAGES) != 0)
		return ENOTSUP;
	if ((barrier->flags & ~VK_DEPENDENCY_BY_REGION_BIT) != 0)
		return ENOTSUP;

	/* Every complete typed range has the recording device and implemented single-family/subresource semantics. */
	for (index = 0; index < barrier->count; index++) {
		error = validate_entry(&barrier->entries[index], device);
		if (error != 0)
			return error;

		/* Overlapping layout transitions of this one-subresource image cannot form two different atomic transitions in one command. */
		if (barrier->entries[index].object == NULL || barrier->entries[index].object->kind != I915_VK_OBJ_IMAGE)
			continue;
		for (previous = 0; previous < index; previous++) {
			if (barrier->entries[previous].object == barrier->entries[index].object)
				return EINVAL;
		}
	}

	/* Succeeded: exact scopes can be implemented by complete serial native retirement and coherent CPU publication. */
	return 0;
}

/* Checks immutable ownership and range semantics for one global, buffer or image dependency. */
static int
validate_entry(
	const struct bcm2711_vulkan_barrier_entry *entry,
	struct bcm2711_vulkan_object *device)
{
	const struct bcm2711_vulkan_resource *resource;
	int error;

	/* All admitted core access scopes have stronger full-operation ordering; unsupported extension scopes cannot silently become no-ops. */
	if ((entry->source & ~BARRIER_ACCESS) != 0 || (entry->destination & ~BARRIER_ACCESS) != 0)
		return ENOTSUP;
	if (entry->object == NULL)
		return 0;

	/* The only queue family is zero; ignored ownership and same-family barriers need no external provider transfer. */
	if (entry->source_family != entry->destination_family)
		return ENOTSUP;
	if (entry->source_family != 0 && entry->source_family != VK_QUEUE_FAMILY_IGNORED)
		return ENOTSUP;
	resource = entry->object->payload;
	if (resource->device != device || resource->memory == NULL)
		return EINVAL;

	/* Bound buffer selections cover nonempty exact ranges or the rest of the logical allocation. */
	if (entry->object->kind == I915_VK_OBJ_BUFFER) {
		if (entry->offset >= resource->bytes || entry->bytes == 0)
			return EINVAL;
		if (entry->bytes != VK_WHOLE_SIZE && entry->bytes > resource->bytes - entry->offset)
			return EINVAL;
	} else {
		/* Image transitions validate only immutable descriptions here; old layout is checked later against FIFO-visible state. */
		if (entry->object->kind != I915_VK_OBJ_IMAGE)
			return EINVAL;
		error = validate_image(entry, resource);
		if (error != 0)
			return error;
	}

	/* Succeeded: this dependency can retain its same-device resource independently of the public handle. */
	return 0;
}

/* Checks the exact implemented colour subresource and layout-specific declared usage before any record owner is acquired. */
static int
validate_image(
	const struct bcm2711_vulkan_barrier_entry *entry,
	const struct bcm2711_vulkan_resource *resource)
{
	uint32_t required;
	int error;

	/* One mip and one array layer have no depth/stencil or partial-subresource ownership transition. */
	if (entry->range.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT ||
	    entry->range.baseMipLevel != 0 || entry->range.baseArrayLayer != 0)
		return ENOTSUP;
	if (entry->range.levelCount != 1 && entry->range.levelCount != VK_REMAINING_MIP_LEVELS)
		return EINVAL;
	if (entry->range.layerCount != 1 && entry->range.layerCount != VK_REMAINING_ARRAY_LAYERS)
		return EINVAL;
	error = colour_layout(entry->before, false);
	if (error != 0)
		return error;
	error = colour_layout(entry->after, true);
	if (error != 0)
		return error;

	/* The declared resource must support the exact operation implied by a specialized destination layout. */
	required = 0;
	switch (entry->after) {
	case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
		required = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		break;
	case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
		required = VK_IMAGE_USAGE_SAMPLED_BIT;
		break;
	case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
		required = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		break;
	case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
		required = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		break;
	default:
		break;
	}

	/* General and present layouts impose no additional transfer/shader usage selection. */
	if ((resource->usage & required) != required)
		return EINVAL;

	/* Succeeded: complete colour layout metadata has no unsupported range or missing destination usage. */
	return 0;
}

/* Refuses unsupported colour layouts and destination layouts which discard rather than define the resulting state. */
static int
colour_layout(
	VkImageLayout layout,
	bool destination)
{
	/* Undefined and preinitialized describe source state only, never a completed transition destination. */
	if (layout == VK_IMAGE_LAYOUT_UNDEFINED || layout == VK_IMAGE_LAYOUT_PREINITIALIZED) {
		if (destination)
			return EINVAL;
		return 0;
	}

	/* Implemented raster colour images have exactly these standard operation/presentation layouts. */
	switch (layout) {
	case VK_IMAGE_LAYOUT_GENERAL:
	case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
	case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
	case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
	case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
	case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
		break;
	default:
		return ENOTSUP;
	}

	/* Succeeded: this source or destination describes one implemented colour image state. */
	return 0;
}
