/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host check of the Mali display node's import check
 * (src/drivers/gpu/mali/display/scanout.c): an image the render node
 * exported, with the page list of its backing, is taken when it is a
 * linear packed four-byte image inside one contiguous run of RAM, and
 * refused with ENOTSUP otherwise (so the swapchain copies instead).
 *
 * cc -std=c11 -Wall -Wextra -Werror -Isrc -Iinclude tests/mali-scanout-host-test.c \
 *    src/drivers/gpu/mali/display/scanout.c -o <out> && <out>
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <drivers/gpu/gpu-scanout.h>
#include <uapi/errno.h>
#include <uapi/gpu.h>

#include "drivers/gpu/mali/display/scanout.h"

/* The pages of the backings: a run from 0x80000000, and one with a gap. */
#define PAGE_BYTES	4096U
#define PAGE_COUNT	2048U

/* How many checks failed and ran. */
static int failures;
static int checks;

/* The page lists the backings borrow. */
static uint64_t contiguous_pages[PAGE_COUNT];
static uint64_t broken_pages[PAGE_COUNT];

static void expect(const char *name, uint64_t got, uint64_t want);
static void good_image(struct gpu_image_descriptor *image);
static void good_backing(struct drv_gpu_scanout_backing *backing);

/* Runs every check and reports the count. */
int
main(void)
{
	struct gpu_image_descriptor image;
	struct drv_gpu_scanout_backing backing;
	const char *reason;
	uint64_t physical;
	unsigned index;
	int error;

	/* The page lists. */
	for (index = 0; index < PAGE_COUNT; index++) {
		contiguous_pages[index] = 0x80000000ULL + (uint64_t)index * PAGE_BYTES;
		broken_pages[index] = contiguous_pages[index];
	}
	broken_pages[PAGE_COUNT / 2U] += PAGE_BYTES * 16U;

	/* A 1920 by 1080 BGRA image of rows of 7680 bytes at the run's start. */
	good_image(&image);
	good_backing(&backing);
	error = mali_scanout_check(&image, &backing, &physical, &reason);
	expect("bgra taken", (uint64_t)error, 0);
	expect("bgra physical", physical, 0x80000000ULL);
	expect("bgra no reason", reason == NULL, 1);

	/* The same as RGBA, 256 bytes into the run. */
	image.format = GPU_PIXEL_RGBA8888;
	image.offset = 256U;
	error = mali_scanout_check(&image, &backing, &physical, &reason);
	expect("rgba at an offset taken", (uint64_t)error, 0);
	expect("rgba physical", physical, 0x80000100ULL);

	/* Another format. */
	good_image(&image);
	image.format = 7U;
	error = mali_scanout_check(&image, &backing, &physical, &reason);
	expect("other format refused", (uint64_t)error, ENOTSUP);
	expect("refusal physical", physical, 0);

	/* Not linear. */
	good_image(&image);
	image.tiling = 2U;
	error = mali_scanout_check(&image, &backing, &physical, &reason);
	expect("tiled refused", (uint64_t)error, ENOTSUP);

	/* Rows not of whole pixels, rows shorter than the width, a start off a pixel, no extent. */
	good_image(&image);
	image.stride = 7682U;
	error = mali_scanout_check(&image, &backing, &physical, &reason);
	expect("odd stride refused", (uint64_t)error, ENOTSUP);
	good_image(&image);
	image.stride = 7676U;
	error = mali_scanout_check(&image, &backing, &physical, &reason);
	expect("short stride refused", (uint64_t)error, ENOTSUP);
	good_image(&image);
	image.offset = 2U;
	error = mali_scanout_check(&image, &backing, &physical, &reason);
	expect("odd offset refused", (uint64_t)error, ENOTSUP);
	good_image(&image);
	image.height = 0U;
	error = mali_scanout_check(&image, &backing, &physical, &reason);
	expect("empty refused", (uint64_t)error, ENOTSUP);

	/* Rows past the allocation. */
	good_image(&image);
	image.offset = (uint64_t)PAGE_COUNT * PAGE_BYTES - 7680U * 1080U + 4U;
	error = mali_scanout_check(&image, &backing, &physical, &reason);
	expect("past the allocation refused", (uint64_t)error, ENOTSUP);
	image.offset = (uint64_t)PAGE_COUNT * PAGE_BYTES - 7680U * 1080U;
	error = mali_scanout_check(&image, &backing, &physical, &reason);
	expect("to the allocation's end taken", (uint64_t)error, 0);

	/* An allocation larger than its pages. */
	good_image(&image);
	good_backing(&backing);
	backing.bytes = (uint64_t)PAGE_COUNT * PAGE_BYTES + 1U;
	error = mali_scanout_check(&image, &backing, &physical, &reason);
	expect("past the pages refused", (uint64_t)error, ENOTSUP);

	/* Device memory, no pages, and pages that are not one run. */
	good_backing(&backing);
	backing.flags |= DRV_GPU_BACKING_DEVICE;
	error = mali_scanout_check(&image, &backing, &physical, &reason);
	expect("device memory refused", (uint64_t)error, ENOTSUP);
	good_backing(&backing);
	backing.pages = NULL;
	error = mali_scanout_check(&image, &backing, &physical, &reason);
	expect("no pages refused", (uint64_t)error, ENOTSUP);
	good_backing(&backing);
	backing.pages = broken_pages;
	error = mali_scanout_check(&image, &backing, &physical, &reason);
	expect("a gap refused", (uint64_t)error, ENOTSUP);
	expect("a gap has a reason", reason != NULL, 1);

	/* The report. */
	if (failures != 0) {
		printf("%d of %d checks failed\n", failures, checks);
		return 1;
	}

	printf("all checks passed (%d)\n", checks);
	return 0;
}

/* Compares one value and records a failure. */
static void
expect(
	const char *name,
	uint64_t got,
	uint64_t want)
{
	/* One check more; a difference is said and counted. */
	checks++;
	if (got != want) {
		printf("FAIL %s: got %llx want %llx\n", name, (unsigned long long)got, (unsigned long long)want);
		failures++;
	}
}

/* Fills a 1920 by 1080 linear BGRA image of 7680-byte rows at offset 0. */
static void
good_image(
	struct gpu_image_descriptor *image)
{
	/* Every field the check reads. */
	image->version = GPU_ABI_VERSION;
	image->size = sizeof(*image);
	image->width = 1920U;
	image->height = 1080U;
	image->format = GPU_PIXEL_BGRA8888;
	image->stride = 7680U;
	image->offset = 0U;
	image->allocation_bytes = (uint64_t)PAGE_COUNT * PAGE_BYTES;
	image->memory_type = 0U;
	image->usage = 16U;
	image->tiling = GPU_IMAGE_LINEAR;
	image->layout_parameter = 0U;
	image->device_id = 0U;
}

/* Fills a backing of the contiguous pages, as the render node hands it out. */
static void
good_backing(
	struct drv_gpu_scanout_backing *backing)
{
	/* The run's pages, all of them used. */
	backing->pages = contiguous_pages;
	backing->page_count = PAGE_COUNT;
	backing->bytes = (uint64_t)PAGE_COUNT * PAGE_BYTES;
	backing->page_bytes = PAGE_BYTES;
	backing->flags = DRV_GPU_BACKING_CONTIGUOUS;
}
