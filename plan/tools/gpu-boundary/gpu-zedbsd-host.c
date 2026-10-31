/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host test of libkeiland-backend's check of a GPU buffer's description
 * (userland/desktop/libkeiland-backend-zedbsd/gpu-zedbsd.c, ws103-p004).
 *
 * kwl_gpu_buffer_decode must accept a description Vulkan can make an image
 * of and refuse, before any value reaches Vulkan, every description a client
 * could send to break it: a zero or too large size, another format or
 * tiling, a short or unaligned row, rows past the allocation, an offset that
 * wraps, a memory type the device does not have, another record length or
 * revision.
 */

#include "userland/desktop/libkeiland-backend-zedbsd/gpu-zedbsd.h"
#include <uapi/gpu.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void decode_base(struct gpu_image_descriptor *image);
static void decode_tiled(struct gpu_image_descriptor *image);
static int decode_run(const char *name, int accept, const struct gpu_image_descriptor *image, size_t size);

/*
 * Runs every case and reports PASS or FAIL.
 */
int
main(
	void)
{
	struct gpu_image_descriptor image;
	int failures;

	/* No case has failed yet. */
	failures = 0;

	/* A 800x600 BGRA image in its own allocation. */
	decode_base(&image);
	failures += decode_run("exact", 1, &image, sizeof(image));

	/* The other format. */
	decode_base(&image);
	image.format = GPU_PIXEL_RGBA8888;
	failures += decode_run("rgba", 1, &image, sizeof(image));

	/* Padded rows inside a larger allocation, at an offset. */
	decode_base(&image);
	image.stride = 4096U;
	image.offset = 8192U;
	image.allocation_bytes = 8192U + 4096U * 600U;
	failures += decode_run("padded", 1, &image, sizeof(image));

	/* A record of another length. */
	decode_base(&image);
	failures += decode_run("length", 0, &image, sizeof(image) - 4U);

	/* A record of another revision. */
	decode_base(&image);
	image.version = GPU_ABI_VERSION + 1U;
	failures += decode_run("version", 0, &image, sizeof(image));

	/* No width. */
	decode_base(&image);
	image.width = 0U;
	failures += decode_run("zero-width", 0, &image, sizeof(image));

	/* No height. */
	decode_base(&image);
	image.height = 0U;
	failures += decode_run("zero-height", 0, &image, sizeof(image));

	/* Wider than the device's largest image. */
	decode_base(&image);
	image.width = 16385U;
	image.stride = 16385U * 4U;
	image.allocation_bytes = (uint64_t)16385U * 4U * 600U;
	failures += decode_run("too-wide", 0, &image, sizeof(image));

	/* Another format. */
	decode_base(&image);
	image.format = 7U;
	failures += decode_run("format", 0, &image, sizeof(image));

	/* Tiled rows. */
	decode_base(&image);
	image.tiling = 0U;
	failures += decode_run("tiling", 0, &image, sizeof(image));

	/* Rows shorter than a row of the image. */
	decode_base(&image);
	image.stride = 3196U;
	failures += decode_run("short-row", 0, &image, sizeof(image));

	/* Rows of a part of a pixel. */
	decode_base(&image);
	image.stride = 3202U;
	image.allocation_bytes = 3202U * 600U;
	failures += decode_run("unaligned-row", 0, &image, sizeof(image));

	/* The last row past the allocation. */
	decode_base(&image);
	image.allocation_bytes = 3200U * 600U - 4U;
	failures += decode_run("past-end", 0, &image, sizeof(image));

	/* An offset that pushes the rows past the allocation. */
	decode_base(&image);
	image.offset = 4096U;
	failures += decode_run("offset", 0, &image, sizeof(image));

	/* An offset that wraps 64 bits. */
	decode_base(&image);
	image.offset = UINT64_MAX - 16U;
	image.allocation_bytes = UINT64_MAX;
	failures += decode_run("offset-wrap", 0, &image, sizeof(image));

	/* A memory type past the device's. */
	decode_base(&image);
	image.memory_type = 4U;
	failures += decode_run("memory-type", 0, &image, sizeof(image));

	/* A memory type past 32 (the shift the import makes of it). */
	decode_base(&image);
	image.memory_type = 40U;
	failures += decode_run("memory-type-shift", 0, &image, sizeof(image));

	/* A block-linear image that may be sampled. */
	decode_tiled(&image);
	failures += decode_run("tiled-exact", 1, &image, sizeof(image));

	/* A block-linear image that may not be sampled. */
	decode_tiled(&image);
	image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	failures += decode_run("tiled-unsampled", 0, &image, sizeof(image));

	/* A block-linear image that starts past its allocation. */
	decode_tiled(&image);
	image.offset = image.allocation_bytes;
	failures += decode_run("tiled-offset", 0, &image, sizeof(image));

	/* A block-linear image of a part of a pixel per row. */
	decode_tiled(&image);
	image.stride = 3202U;
	failures += decode_run("tiled-unaligned-row", 0, &image, sizeof(image));

	/* Reports the verdict. */
	if (failures != 0) {
		printf("gpu-zedbsd-host: FAIL failures=%d\n", failures);
		return 1;
	}

	/* Succeeded: every case was decided as expected. */
	printf("gpu-zedbsd-host: PASS\n");
	return 0;
}

/* Fills a description of an 800x600 BGRA linear image that exactly fills its allocation. */
static void
decode_base(
	struct gpu_image_descriptor *image)
{
	/* The record's revision and length. */
	memset(image, 0, sizeof(*image));
	image->version = GPU_ABI_VERSION;
	image->size = sizeof(*image);

	/* The image and its rows. */
	image->width = 800U;
	image->height = 600U;
	image->format = GPU_PIXEL_BGRA8888;
	image->stride = 3200U;
	image->offset = 0U;
	image->allocation_bytes = 3200U * 600U;
	image->memory_type = 1U;
	image->tiling = GPU_IMAGE_LINEAR;

	/* Succeeded: the description is one Vulkan can take. */
	return;
}

/* Fills a description of an 800x600 BGRA NVIDIA block-linear image (block height 16 GOBs) that may be sampled. */
static void
decode_tiled(
	struct gpu_image_descriptor *image)
{
	/* The linear image's size and memory. */
	decode_base(image);

	/* Rows of blocks 3264 bytes wide, 640 rows (600 rounded up to 128). */
	image->tiling = GPU_IMAGE_NV_BLOCK_LINEAR;
	image->layout_parameter = 4U;
	image->stride = 3264U;
	image->allocation_bytes = 3264U * 640U;
	image->usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

	/* Succeeded: the description is one Vulkan can take. */
	return;
}

/* Runs one case against a device of 16384-pixel images and 4 memory types; returns 1 when the verdict is not the expected one. */
static int
decode_run(
	const char *name,
	int accept,
	const struct gpu_image_descriptor *image,
	size_t size)
{
	struct kwl_gpu_limits limits;
	struct kwl_buffer_layout layout;
	unsigned char bytes[sizeof(*image)];
	int error;
	int accepted;

	/* The device the description is checked against. */
	limits.max_dimension = 16384U;
	limits.memory_type_count = 4U;

	/* The description as it arrives on the wire. */
	memcpy(bytes, image, sizeof(bytes));
	memset(&layout, 0, sizeof(layout));
	error = kwl_gpu_buffer_decode(bytes, size, &limits, &layout);
	accepted = 0;
	if (error == 0)
		accepted = 1;

	/* A verdict other than the expected one fails the case. */
	if (accepted != accept) {
		printf("case %s: FAIL (accepted=%d, expected %d, error=%d)\n", name, accepted, accept, error);
		return 1;
	}

	/* An accepted description keeps its values. */
	if (accepted &&
	    (layout.width != image->width ||
	     layout.height != image->height ||
	     layout.stride != image->stride ||
	     layout.offset != image->offset ||
	     layout.tiling != image->tiling ||
	     layout.parameter != image->layout_parameter ||
	     layout.usage != image->usage ||
	     layout.linear != (image->tiling == GPU_IMAGE_LINEAR ? 1U : 0U))) {
		printf("case %s: FAIL (layout differs)\n", name);
		return 1;
	}

	/* Succeeded: the case was decided as expected. */
	printf("case %s: ok (%s)\n", name, accepted ? "accepted" : "refused");
	return 0;
}
