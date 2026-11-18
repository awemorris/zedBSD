/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The check of a dedicated import of an image capability (ws103-p003).
 *
 * An importer names the image it will bind to the imported memory
 * (VK_KHR_dedicated_allocation).  The kernel returned its own description of
 * the capability; the image, its memory requirements and its row layout must
 * read the allocation exactly as that description says, so that a client's
 * claim about its buffer cannot make the importer read the allocation as
 * another image.  The check has no side effect and is kept apart from the
 * import so that it can be tested on the host.
 */

#include "internal.h"
#include <uapi/gpu.h>

/*
 * Checks an image, its memory and its rows against a capability's description.
 *
 * The image is one linear 2D layer with one level and one sample of the
 * described size and pixel byte order; the memory is exactly the image's, in
 * a type it may use; the rows lie at the described offset and pitch; and the
 * capability itself is linear and may be sampled.
 */
VkResult
vulkan_dedicated_check(
	const struct vulkan_image *image,
	const VkMemoryAllocateInfo *info,
	const VkMemoryRequirements *requirements,
	const VkSubresourceLayout *layout,
	const struct gpu_image_descriptor *described)
{
	int format_matches;

	/* The capability itself is an image that may be sampled. */
	if ((described->usage & VK_IMAGE_USAGE_SAMPLED_BIT) == 0U)
		return VK_ERROR_INVALID_EXTERNAL_HANDLE;

	/*
	 * A linear capability is read by a linear image; one of another layout
	 * by an optimal image placed in that same layout (its creation's
	 * VkImageLayoutZEDBSD) with the described uses, which the placement
	 * depends on.
	 */
	if (described->tiling == GPU_IMAGE_LINEAR) {
		if (image->tiling != VK_IMAGE_TILING_LINEAR || image->layout_tiling != 0U)
			return VK_ERROR_INVALID_EXTERNAL_HANDLE;
	} else {
		if (image->tiling != VK_IMAGE_TILING_OPTIMAL)
			return VK_ERROR_INVALID_EXTERNAL_HANDLE;
		if (image->layout_tiling != described->tiling || image->layout_parameter != described->layout_parameter)
			return VK_ERROR_INVALID_EXTERNAL_HANDLE;
		if (image->usage != described->usage)
			return VK_ERROR_INVALID_EXTERNAL_HANDLE;
	}

	/* The image is one 2D layer with one level and one sample, as the capability is. */
	if (image->type != VK_IMAGE_TYPE_2D)
		return VK_ERROR_INVALID_EXTERNAL_HANDLE;
	if (image->mip_levels != 1U)
		return VK_ERROR_INVALID_EXTERNAL_HANDLE;
	if (image->array_layers != 1U)
		return VK_ERROR_INVALID_EXTERNAL_HANDLE;
	if (image->samples != VK_SAMPLE_COUNT_1_BIT)
		return VK_ERROR_INVALID_EXTERNAL_HANDLE;

	/* The image has the described size. */
	if (image->extent.width != described->width)
		return VK_ERROR_INVALID_EXTERNAL_HANDLE;
	if (image->extent.height != described->height)
		return VK_ERROR_INVALID_EXTERNAL_HANDLE;
	if (image->extent.depth != 1U)
		return VK_ERROR_INVALID_EXTERNAL_HANDLE;

	/* The image's format has the described byte order (read as unorm or sRGB). */
	format_matches = 0;
	if (described->format == GPU_PIXEL_BGRA8888) {
		/* Blue, green, red, alpha in memory. */
		if (image->format == VK_FORMAT_B8G8R8A8_UNORM || image->format == VK_FORMAT_B8G8R8A8_SRGB)
			format_matches = 1;
	} else if (described->format == GPU_PIXEL_RGBA8888) {
		/* Red, green, blue, alpha in memory. */
		if (image->format == VK_FORMAT_R8G8B8A8_UNORM || image->format == VK_FORMAT_R8G8B8A8_SRGB)
			format_matches = 1;
	}

	/* Another byte order would show other colors, or another pixel size other rows. */
	if (!format_matches)
		return VK_ERROR_INVALID_EXTERNAL_HANDLE;

	/* The memory is exactly the image's, in a type the image may use. */
	if (info->allocationSize != requirements->size)
		return VK_ERROR_INVALID_EXTERNAL_HANDLE;
	if (info->memoryTypeIndex >= 32U)
		return VK_ERROR_INVALID_EXTERNAL_HANDLE;
	if ((requirements->memoryTypeBits & (1U << info->memoryTypeIndex)) == 0U)
		return VK_ERROR_INVALID_EXTERNAL_HANDLE;

	/* The image's memory lies inside the capability's allocation. */
	if (requirements->size > described->allocation_bytes)
		return VK_ERROR_INVALID_EXTERNAL_HANDLE;

	/* A linear image's rows lie where the capability's rows are (another layout is placed by its record). */
	if (described->tiling == GPU_IMAGE_LINEAR && layout->offset != described->offset)
		return VK_ERROR_INVALID_EXTERNAL_HANDLE;
	if (described->tiling == GPU_IMAGE_LINEAR && layout->rowPitch != described->stride)
		return VK_ERROR_INVALID_EXTERNAL_HANDLE;

	/* Succeeded: the image reads the allocation as the kernel describes it. */
	return VK_SUCCESS;
}
