/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host test of libvulkan's dedicated-import check (ws103-p003, V3).
 *
 * vulkan_dedicated_check must accept an image, memory and row layout that read
 * the kernel's description exactly, and refuse a description that differs in
 * any one field a client could claim: size, pixel format, stride, offset,
 * allocation size, tiling, usage; and an image or memory request that differs
 * from the description.  An image of another layout than linear rows (NVIDIA
 * block linear here) is read by an optimal image placed by its layout record
 * in exactly that layout and with exactly the described uses.
 */

#include "internal.h"
#include <uapi/gpu.h>
#include <stdio.h>
#include <string.h>

/* One import, exact or forged in one value, that the check must decide. */
struct dedicated_case {
	const char *name;
	int accept;
	struct vulkan_image image;
	VkMemoryAllocateInfo info;
	VkMemoryRequirements requirements;
	VkSubresourceLayout layout;
	struct gpu_image_descriptor described;
};

static void dedicated_base(struct dedicated_case *item, const char *name, int accept);
static void dedicated_tiled(struct dedicated_case *item, const char *name, int accept);
static int dedicated_run(const struct dedicated_case *item);

/*
 * Runs every case and reports PASS or FAIL.
 */
int
main(
	void)
{
	struct dedicated_case item;
	int failures;

	/* No case has failed yet. */
	failures = 0;

	/* The description as the kernel gives it for the image the importer made. */
	dedicated_base(&item, "exact", 1);
	failures += dedicated_run(&item);

	/* The same bytes read as sRGB are still the described byte order. */
	dedicated_base(&item, "srgb-reading", 1);
	item.image.format = VK_FORMAT_B8G8R8A8_SRGB;
	failures += dedicated_run(&item);

	/* A client that claims a wider image than its allocation holds. */
	dedicated_base(&item, "width", 0);
	item.described.width = 1024U;
	failures += dedicated_run(&item);

	/* A taller one. */
	dedicated_base(&item, "height", 0);
	item.described.height = 800U;
	failures += dedicated_run(&item);

	/* Another pixel byte order. */
	dedicated_base(&item, "format", 0);
	item.described.format = GPU_PIXEL_RGBA8888;
	failures += dedicated_run(&item);

	/* Another row pitch. */
	dedicated_base(&item, "stride", 0);
	item.described.stride = 2560U;
	failures += dedicated_run(&item);

	/* Rows that start elsewhere in the allocation. */
	dedicated_base(&item, "offset", 0);
	item.described.offset = 4096U;
	failures += dedicated_run(&item);

	/* An allocation smaller than the image's memory. */
	dedicated_base(&item, "allocation-bytes", 0);
	item.described.allocation_bytes = 4096U;
	failures += dedicated_run(&item);

	/* A tiled capability claimed as linear. */
	dedicated_base(&item, "tiling", 0);
	item.described.tiling = 0U;
	failures += dedicated_run(&item);

	/* A capability that may not be sampled. */
	dedicated_base(&item, "usage", 0);
	item.described.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	failures += dedicated_run(&item);

	/* An import of more memory than the image's. */
	dedicated_base(&item, "allocation-size", 0);
	item.info.allocationSize = item.requirements.size + 4096U;
	failures += dedicated_run(&item);

	/* A memory type the image may not use. */
	dedicated_base(&item, "memory-type", 0);
	item.info.memoryTypeIndex = 3U;
	failures += dedicated_run(&item);

	/* A memory type index past the mask. */
	dedicated_base(&item, "memory-type-range", 0);
	item.info.memoryTypeIndex = 40U;
	failures += dedicated_run(&item);

	/* An optimally tiled image of the importer. */
	dedicated_base(&item, "image-tiling", 0);
	item.image.tiling = VK_IMAGE_TILING_OPTIMAL;
	failures += dedicated_run(&item);

	/* An image with two layers. */
	dedicated_base(&item, "image-layers", 0);
	item.image.array_layers = 2U;
	failures += dedicated_run(&item);

	/* An image with two levels. */
	dedicated_base(&item, "image-levels", 0);
	item.image.mip_levels = 2U;
	failures += dedicated_run(&item);

	/* A multisampled image. */
	dedicated_base(&item, "image-samples", 0);
	item.image.samples = VK_SAMPLE_COUNT_4_BIT;
	failures += dedicated_run(&item);

	/* A renderer layout whose rows differ from the capability's. */
	dedicated_base(&item, "layout-pitch", 0);
	item.layout.rowPitch = 2560U;
	failures += dedicated_run(&item);

	/* A linear capability read by an image that names another layout. */
	dedicated_base(&item, "linear-with-record", 0);
	item.image.layout_tiling = GPU_IMAGE_NV_BLOCK_LINEAR;
	failures += dedicated_run(&item);

	/* A block-linear capability read by an optimal image placed in that layout with its uses (no rows to ask). */
	dedicated_tiled(&item, "tiled-exact", 1);
	failures += dedicated_run(&item);

	/* The same, read as sRGB. */
	dedicated_tiled(&item, "tiled-srgb-reading", 1);
	item.image.format = VK_FORMAT_B8G8R8A8_SRGB;
	failures += dedicated_run(&item);

	/* An optimal image that names no layout. */
	dedicated_tiled(&item, "tiled-no-record", 0);
	item.image.layout_tiling = 0U;
	item.image.layout_parameter = 0U;
	failures += dedicated_run(&item);

	/* Another block height. */
	dedicated_tiled(&item, "tiled-parameter", 0);
	item.image.layout_parameter = 3U;
	failures += dedicated_run(&item);

	/* Fewer uses than described (the placement depends on them). */
	dedicated_tiled(&item, "tiled-usage", 0);
	item.image.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
	failures += dedicated_run(&item);

	/* A linear image of a block-linear capability. */
	dedicated_tiled(&item, "tiled-linear-image", 0);
	item.image.tiling = VK_IMAGE_TILING_LINEAR;
	failures += dedicated_run(&item);

	/* A block-linear capability that may not be sampled. */
	dedicated_tiled(&item, "tiled-unsampled", 0);
	item.described.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	item.image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	failures += dedicated_run(&item);

	/* An allocation smaller than the image's memory. */
	dedicated_tiled(&item, "tiled-allocation-bytes", 0);
	item.described.allocation_bytes = 4096U;
	failures += dedicated_run(&item);

	/* Reports the verdict. */
	if (failures != 0) {
		printf("dedicated-host: FAIL failures=%d\n", failures);
		return 1;
	}

	/* Succeeded: every case was decided as expected. */
	printf("dedicated-host: PASS\n");
	return 0;
}

/* Fills a case whose image, memory and layout match its description (an 800x600 BGRA linear image). */
static void
dedicated_base(
	struct dedicated_case *item,
	const char *name,
	int accept)
{
	/* The case's name and expected verdict. */
	memset(item, 0, sizeof(*item));
	item->name = name;
	item->accept = accept;

	/* The importer's image. */
	item->image.type = VK_IMAGE_TYPE_2D;
	item->image.format = VK_FORMAT_B8G8R8A8_UNORM;
	item->image.extent.width = 800U;
	item->image.extent.height = 600U;
	item->image.extent.depth = 1U;
	item->image.mip_levels = 1U;
	item->image.array_layers = 1U;
	item->image.samples = VK_SAMPLE_COUNT_1_BIT;
	item->image.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
	item->image.tiling = VK_IMAGE_TILING_LINEAR;

	/* The renderer's requirements and layout of that image. */
	item->requirements.size = 3200U * 600U;
	item->requirements.alignment = 4096U;
	item->requirements.memoryTypeBits = 0x3U;
	item->layout.offset = 0U;
	item->layout.rowPitch = 3200U;
	item->layout.size = 3200U * 600U;

	/* The import's request. */
	item->info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	item->info.allocationSize = item->requirements.size;
	item->info.memoryTypeIndex = 1U;

	/* The kernel's description of the capability. */
	item->described.version = GPU_ABI_VERSION;
	item->described.size = sizeof(item->described);
	item->described.width = 800U;
	item->described.height = 600U;
	item->described.format = GPU_PIXEL_BGRA8888;
	item->described.stride = 3200U;
	item->described.offset = 0U;
	item->described.allocation_bytes = 3200U * 600U;
	item->described.memory_type = 1U;
	item->described.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	item->described.tiling = GPU_IMAGE_LINEAR;

	/* Succeeded: the case is an exact import. */
	return;
}

/* Fills a case of an NVIDIA block-linear capability (block height 16 GOBs) and an optimal image placed in it. */
static void
dedicated_tiled(
	struct dedicated_case *item,
	const char *name,
	int accept)
{
	/* The linear case's sizes and memory. */
	dedicated_base(item, name, accept);

	/* The kernel's description: 832-byte-aligned rows of blocks, 640 rows (600 rounded up to 128). */
	item->described.tiling = GPU_IMAGE_NV_BLOCK_LINEAR;
	item->described.layout_parameter = 4U;
	item->described.stride = 3264U;
	item->described.allocation_bytes = 3264U * 640U;
	item->described.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

	/* The importer's optimal image with the layout record and the described uses; it has no rows. */
	item->image.tiling = VK_IMAGE_TILING_OPTIMAL;
	item->image.layout_tiling = GPU_IMAGE_NV_BLOCK_LINEAR;
	item->image.layout_parameter = 4U;
	item->image.usage = item->described.usage;
	item->requirements.size = 3264U * 640U;
	item->info.allocationSize = item->requirements.size;
	memset(&item->layout, 0, sizeof(item->layout));

	/* Succeeded: the case is an exact import. */
	return;
}

/* Runs one case; returns 1 when its verdict is not the expected one. */
static int
dedicated_run(
	const struct dedicated_case *item)
{
	VkResult status;
	int accepted;

	/* The library's verdict. */
	status = vulkan_dedicated_check(&item->image, &item->info, &item->requirements, &item->layout, &item->described);
	accepted = 0;
	if (status == VK_SUCCESS)
		accepted = 1;

	/* A verdict other than the expected one fails the case. */
	if (accepted != item->accept) {
		printf("case %s: FAIL (accepted=%d, expected %d, status=%d)\n", item->name, accepted, item->accept, (int)status);
		return 1;
	}

	/* Succeeded: the case was decided as expected. */
	printf("case %s: ok (%s)\n", item->name, accepted ? "accepted" : "refused");
	return 0;
}
