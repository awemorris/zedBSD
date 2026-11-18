/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Native synchronization supports one serialized graphics family and one complete coherent colour subresource per image. */
#include <uapi/errno.h>

#if CONFIG_DRIVER_RPI4_GPU || CONFIG_DRIVER_RPI5_GPU
#include "drivers/gpu/videocore/vk/vulkan-video.h"
#endif
#include "drivers/gpu/videocore/vk/vulkan-barrier.h"
#include "drivers/gpu/videocore/vk/vulkan-memory.h"

/* All admitted operations, compute dispatches included, complete serially; stages of unadvertised features remain refused. */
#define BARRIER_STAGES (VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT | VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)

/* Core Vulkan 1.0 access bits have complete stronger serial ordering; feature-dependent extension accesses are not advertised. */
#define BARRIER_ACCESS 0x1ffffU

static int validate_entry(const struct bcm2711_vulkan_barrier_entry *entry, struct bcm2711_vulkan_object *device);
static int validate_family(const struct bcm2711_vulkan_barrier_entry *entry, const struct bcm2711_vulkan_resource *resource);
static int validate_image(const struct bcm2711_vulkan_barrier_entry *entry, const struct bcm2711_vulkan_resource *resource);
static int colour_layout(VkImageLayout layout, bool destination);
static int validate_depth_image(const struct bcm2711_vulkan_barrier_entry *entry, const struct bcm2711_vulkan_resource *resource);
static int depth_layout(VkImageLayout layout, bool destination);
static bool ranges_overlap(const VkImageSubresourceRange *first, const VkImageSubresourceRange *second);
static bool runs_overlap(uint32_t first_base, uint32_t first_count, uint32_t second_base, uint32_t second_count);

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

		/* Two transitions of the same subresource of an image cannot form two different atomic transitions in one command. */
		if (barrier->entries[index].object == NULL || barrier->entries[index].object->kind != I915_VK_OBJ_IMAGE)
			continue;
		for (previous = 0; previous < index; previous++) {
			if (barrier->entries[previous].object == barrier->entries[index].object &&
			    ranges_overlap(&barrier->entries[previous].range, &barrier->entries[index].range))
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

	/* Every dependency selects same-device bound storage before any internal or external ownership scope can be admitted. */
	resource = entry->object->payload;
	if (resource->device != device || resource->memory == NULL)
		return EINVAL;
	error = validate_family(entry, resource);
	if (error != 0)
		return error;

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

/* Admits the sole graphics family and explicit external sharing only for an allocation declared for the native shared-memory contract. */
static int
validate_family(
	const struct bcm2711_vulkan_barrier_entry *entry,
	const struct bcm2711_vulkan_resource *resource)
{
	const struct bcm2711_vulkan_memory *memory;
	uint32_t external;

	/* Equal zero or ignored indices order the same native family and perform no ownership transfer. */
	if (entry->source_family == entry->destination_family) {
		if (entry->source_family != 0 && entry->source_family != VK_QUEUE_FAMILY_IGNORED)
			return ENOTSUP;
		return 0;
	}

	/* A real ownership transfer always connects the single native family to one explicit external endpoint. */
	if (entry->source_family == 0) {
		external = entry->destination_family;
	} else if (entry->destination_family == 0) {
		external = entry->source_family;
	} else {
		return ENOTSUP;
	}

	/* Foreign queues and unrelated family indices have no implemented provider or cache-visibility contract. */
	if (external != BCM2711_VULKAN_EXTERNAL_FAMILY)
		return ENOTSUP;
	memory = resource->memory->payload;
	if (memory->external_type == 0)
		return EINVAL;

	/* Succeeded: exact coherent shared storage uses whole native completion and callback/timeline ordering at the external boundary. */
	return 0;
}

/* Checks the exact implemented colour subresource and layout-specific declared usage before any record owner is acquired. */
static int
validate_image(
	const struct bcm2711_vulkan_barrier_entry *entry,
	const struct bcm2711_vulkan_resource *resource)
{
	uint32_t required;
	uint32_t levels;
	uint32_t layers;
	int error;

	/* A depth/stencil attachment image has its own aspects and layouts. */
	if (bcm2711_vulkan_depth_format(resource->format)) {
		error = validate_depth_image(entry, resource);
		if (error != 0)
			return error;
		return 0;
	}

	/* Colour; any run of the image's mip levels and array layers moves on its own. */
	if (entry->range.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT)
		return ENOTSUP;
	levels = resource->levels;
	if (levels == 0)
		levels = 1;
	if (entry->range.baseMipLevel >= levels || entry->range.levelCount == 0)
		return EINVAL;
	if (entry->range.levelCount != VK_REMAINING_MIP_LEVELS && entry->range.levelCount > levels - entry->range.baseMipLevel)
		return EINVAL;
	layers = bcm2711_vulkan_image_layers(resource);
	if (entry->range.baseArrayLayer >= layers || entry->range.layerCount == 0)
		return EINVAL;
	if (entry->range.layerCount != VK_REMAINING_ARRAY_LAYERS && entry->range.layerCount > layers - entry->range.baseArrayLayer)
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
#if CONFIG_DRIVER_RPI4_GPU || CONFIG_DRIVER_RPI5_GPU
	case (VkImageLayout)BCM2711_VIDEO_LAYOUT_DECODE_DST:
		/* A decode output. */
		required = BCM2711_VIDEO_USAGE_DECODE_DST;
		break;
	case (VkImageLayout)BCM2711_VIDEO_LAYOUT_DECODE_DPB:
		/* A decode reference. */
		required = BCM2711_VIDEO_USAGE_DECODE_DPB;
		break;
#endif
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
#if CONFIG_DRIVER_RPI4_GPU || CONFIG_DRIVER_RPI5_GPU
	case (VkImageLayout)BCM2711_VIDEO_LAYOUT_DECODE_DST:
	case (VkImageLayout)BCM2711_VIDEO_LAYOUT_DECODE_DPB:
		/* The layouts of decoded pictures. */
		break;
#endif
	default:
		return ENOTSUP;
	}

	/* Succeeded: this source or destination describes one implemented colour image state. */
	return 0;
}

/*
 * Checks a depth/stencil image's transition: every aspect of its format at
 * its only level and layer, between the layouts a depth/stencil pass and
 * its transfers use.
 */
static int
validate_depth_image(
	const struct bcm2711_vulkan_barrier_entry *entry,
	const struct bcm2711_vulkan_resource *resource)
{
	VkImageAspectFlags aspects;
	uint32_t layers;
	int error;

	/* A combined image moves with both aspects, a depth-only one with depth alone, at its one level. */
	aspects = VK_IMAGE_ASPECT_DEPTH_BIT;
	if (resource->format == VK_FORMAT_D24_UNORM_S8_UINT)
		aspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
	if (entry->range.aspectMask != aspects || entry->range.baseMipLevel != 0)
		return ENOTSUP;
	if (entry->range.levelCount != 1 && entry->range.levelCount != VK_REMAINING_MIP_LEVELS)
		return EINVAL;

	/* Any run of the image's array layers moves on its own. */
	layers = bcm2711_vulkan_image_layers(resource);
	if (entry->range.baseArrayLayer >= layers || entry->range.layerCount == 0)
		return EINVAL;
	if (entry->range.layerCount != VK_REMAINING_ARRAY_LAYERS && entry->range.layerCount > layers - entry->range.baseArrayLayer)
		return EINVAL;

	/* The source and destination layouts are the image's, the destination a defined one. */
	error = depth_layout(entry->before, false);
	if (error != 0)
		return error;
	error = depth_layout(entry->after, true);
	if (error != 0)
		return error;

	/* A layout for sampling, for a clear or a copy into it, or for a copy or blit from it needs the usage it serves. */
	if ((entry->after == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL || entry->after == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL) &&
	    (resource->usage & VK_IMAGE_USAGE_SAMPLED_BIT) == 0)
		return EINVAL;
	if (entry->after == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && (resource->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0)
		return EINVAL;
	if (entry->after == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && (resource->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0)
		return EINVAL;

	/* Succeeded: the image's whole layout lifecycle is one the native pass tracks. */
	return 0;
}

/* Refuses layouts a depth/stencil attachment image is never in, and undefined destinations. */
static int
depth_layout(
	VkImageLayout layout,
	bool destination)
{
	/* Undefined describes source state only. */
	if (layout == VK_IMAGE_LAYOUT_UNDEFINED) {
		if (destination)
			return EINVAL;
		return 0;
	}

	/* The attachment layout, the general one, a sampled one, or a transfer's (clears, copies and blits into and out of it). */
	if (layout != VK_IMAGE_LAYOUT_GENERAL &&
	    layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL &&
	    layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL &&
	    layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
	    layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
	    layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
		return ENOTSUP;

	/* Succeeded: the layout is one a depth/stencil image is in. */
	return 0;
}

/* Tells whether two image barrier ranges share a mip level of an array layer. */
static bool
ranges_overlap(
	const VkImageSubresourceRange *first,
	const VkImageSubresourceRange *second)
{
	bool levels;
	bool layers;

	/* Both the levels and the layers must meet for a subresource to be in both. */
	levels = runs_overlap(first->baseMipLevel, first->levelCount, second->baseMipLevel, second->levelCount);
	layers = runs_overlap(first->baseArrayLayer, first->layerCount, second->baseArrayLayer, second->layerCount);

	/* Succeeded: whether some subresource is in both. */
	return levels && layers;
}

/* Tells whether two runs, each a base and a count (VK_REMAINING_*: to the end), meet. */
static bool
runs_overlap(
	uint32_t first_base,
	uint32_t first_count,
	uint32_t second_base,
	uint32_t second_count)
{
	/* The later run starts inside the earlier one, or the earlier one runs to the end. */
	if (first_base <= second_base) {
		if (first_count == VK_REMAINING_MIP_LEVELS)
			return true;
		return second_base - first_base < first_count;
	}
	if (second_count == VK_REMAINING_MIP_LEVELS)
		return true;

	/* Succeeded: whether the first starts inside the second. */
	return first_base - second_base < second_count;
}
