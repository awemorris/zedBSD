/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The check of an exported image the display node imports.  See
 * scanout.h.
 */

#include <stddef.h>
#include <stdint.h>

#include <drivers/gpu/gpu-scanout.h>
#include <uapi/errno.h>
#include <uapi/gpu.h>

#include "drivers/gpu/mali/display/scanout.h"

/* The bytes of one pixel of the two packed formats, and the alignment of a row and of the image's start. */
#define SCANOUT_PIXEL_BYTES	4U

/*
 * Checks an exported image and its backing: a linear image of the two
 * packed formats, whole pixels to a row, every row inside the backing,
 * and the backing one run of RAM pages, so one physical address names
 * the image and the processor can read it through the run's view.
 * Anything else is ENOTSUP, which leaves the swapchain to copy its
 * frames instead.
 */
int
mali_scanout_check(
	const struct gpu_image_descriptor *image,
	const struct drv_gpu_scanout_backing *backing,
	uint64_t *physical,
	const char **reason)
{
	uint64_t frame_bytes;
	uint64_t run_bytes;
	uint64_t index;
	uint64_t first;

	/* One of the two packed formats the screen takes, linear. */
	*physical = 0;
	if (image->format != GPU_PIXEL_RGBA8888 && image->format != GPU_PIXEL_BGRA8888) {
		*reason = "is not of a packed four-byte format";
		return ENOTSUP;
	}

	if (image->tiling != GPU_IMAGE_LINEAR) {
		*reason = "is not linear";
		return ENOTSUP;
	}

	/* A visible extent, rows of whole pixels at least as long as the width, a start on a pixel. */
	if (image->width == 0U || image->height == 0U) {
		*reason = "is empty";
		return ENOTSUP;
	}

	if ((image->stride % SCANOUT_PIXEL_BYTES) != 0U || (image->offset % SCANOUT_PIXEL_BYTES) != 0U) {
		*reason = "has rows or a start off a pixel";
		return ENOTSUP;
	}

	if ((uint64_t)image->stride < (uint64_t)image->width * SCANOUT_PIXEL_BYTES) {
		*reason = "has rows shorter than its width";
		return ENOTSUP;
	}

	/* RAM pages, listed. */
	if ((backing->flags & DRV_GPU_BACKING_DEVICE) != 0U) {
		*reason = "is device memory";
		return ENOTSUP;
	}

	if (backing->pages == NULL || backing->page_count == 0U || backing->page_bytes == 0U) {
		*reason = "has no pages";
		return ENOTSUP;
	}

	/* Every row inside the allocation and inside its pages. */
	frame_bytes = (uint64_t)image->stride * image->height;
	run_bytes = backing->page_count * backing->page_bytes;
	if (image->offset > backing->bytes || frame_bytes > backing->bytes - image->offset) {
		*reason = "runs past its allocation";
		return ENOTSUP;
	}

	if (backing->bytes > run_bytes) {
		*reason = "runs past its pages";
		return ENOTSUP;
	}

	/* The pages one after the other: one physical run. */
	first = backing->pages[0];
	for (index = 1; index < backing->page_count; index++) {
		if (backing->pages[index] != first + index * backing->page_bytes) {
			*reason = "is not one contiguous run";
			return ENOTSUP;
		}
	}

	/* Succeeded: the image starts at its offset into the run. */
	*reason = NULL;
	*physical = first + image->offset;
	return 0;
}
