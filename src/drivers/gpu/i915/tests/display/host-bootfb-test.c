/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the boot framebuffer display's arithmetic
 * (display/bootfb.c): the framebuffer's checks and layout, the
 * clip of a frame, the choice of the red and blue swap, and the row copy.
 *
 *   sh run-bootfb-host-test.sh
 *
 * The node operations need the GPU and are not reached; the link drops
 * them.
 */

#include "host-test.h"

#include "../../display/bootfb.h"

#include <kern/boot.h>
#include <kern/platform.h>

#include <uapi/errno.h>
#include <uapi/gpu.h>

#include <stdio.h>
#include <string.h>

static void i915_host_bootfb_layout(void);
static void i915_host_bootfb_refusals(void);
static void i915_host_bootfb_clip(void);
static void i915_host_bootfb_swap(void);
static void i915_host_bootfb_rows(void);
static void i915_host_bootfb_fb(struct kern_boot_framebuffer *fb, uint32_t width, uint32_t height, uint32_t stride, uint32_t format);

/*
 * Checks the boot framebuffer display's arithmetic.
 */
int
main(void)
{
	int status;

	/* The layout of usable framebuffers, and the ones refused. */
	i915_host_bootfb_layout();
	i915_host_bootfb_refusals();

	/* The clip, the swap and the row copy. */
	i915_host_bootfb_clip();
	i915_host_bootfb_swap();
	i915_host_bootfb_rows();

	/* The tally. */
	status = i915_host_report("bootfb_host_test");
	if (status != 0)
		return status;

	/* Succeeded: the arithmetic is the one bootfb.c describes. */
	return 0;
}

/* Checks the layout of a 1920x1080 BGRX framebuffer, of an RGBX one with a padded stride, and of an odd width. */
static void
i915_host_bootfb_layout(void)
{
	struct kern_boot_framebuffer fb;
	struct i915_bootfb_layout layout;
	int error;

	/* The SV7's panel as GOP sets it: 1920x1080, stride 1920, B G R X. */
	i915_host_bootfb_fb(&fb, 1920U, 1080U, 1920U, KERN_BOOT_FRAMEBUFFER_BGRX8888);
	error = drv_i915_bootfb_layout(&fb, &layout);
	i915_host_check(error == 0 &&
			layout.width == 1920U &&
			layout.height == 1080U &&
			layout.fb_stride_bytes == 7680U &&
			layout.rgbx == 0 &&
			layout.fb_physical == 0x4000000000ULL &&
			layout.fb_bytes == 8294400U,
			"LAYOUT: 1920x1080 BGRX: 7680 bytes a row, 8294400 bytes in all, not RGBX");
	i915_host_check(layout.staging_stride_bytes == 7680U &&
			layout.staging_bytes == 8294400U,
			"LAYOUT: 1920x1080 staging: rows of 7680 bytes, 2025 whole pages");

	/* An RGBX framebuffer whose stride is wider than its rows (QEMU's VGA at 1280x800, stride 2048). */
	i915_host_bootfb_fb(&fb, 1280U, 800U, 2048U, KERN_BOOT_FRAMEBUFFER_RGBX8888);
	error = drv_i915_bootfb_layout(&fb, &layout);
	i915_host_check(error == 0 &&
			layout.fb_stride_bytes == 8192U &&
			layout.rgbx == 1 &&
			layout.fb_bytes == 8192ULL * 800U &&
			layout.staging_stride_bytes == 5120U &&
			layout.staging_bytes == 5120ULL * 800U,
			"LAYOUT: 1280x800 RGBX stride 2048: 8192 bytes a framebuffer row, 5120 a staging row");

	/* An odd width rounds a staging row up to 64 bytes and the area up to whole pages. */
	i915_host_bootfb_fb(&fb, 1366U, 768U, 1366U, KERN_BOOT_FRAMEBUFFER_BGRX8888);
	error = drv_i915_bootfb_layout(&fb, &layout);
	i915_host_check(error == 0 &&
			layout.staging_stride_bytes == 5504U &&
			layout.staging_bytes == ((5504ULL * 768U + 4095U) & ~4095ULL) &&
			(layout.staging_bytes % 4096U) == 0U,
			"LAYOUT: 1366x768: a staging row of 5504 bytes (5464 rounded to 64), the area in whole pages");
}

/* Checks the framebuffers the display refuses. */
static void
i915_host_bootfb_refusals(void)
{
	struct kern_boot_framebuffer fb;
	struct i915_bootfb_layout layout;
	int error;

	/* An empty extent. */
	i915_host_bootfb_fb(&fb, 0U, 1080U, 1920U, KERN_BOOT_FRAMEBUFFER_BGRX8888);
	error = drv_i915_bootfb_layout(&fb, &layout);
	i915_host_check(error == EINVAL, "REFUSE: a framebuffer 0 pixels wide");

	/* A stride shorter than a row. */
	i915_host_bootfb_fb(&fb, 1920U, 1080U, 1600U, KERN_BOOT_FRAMEBUFFER_BGRX8888);
	error = drv_i915_bootfb_layout(&fb, &layout);
	i915_host_check(error == EINVAL, "REFUSE: a stride shorter than the width");

	/* A format the loader does not report (0: none). */
	i915_host_bootfb_fb(&fb, 1920U, 1080U, 1920U, 0U);
	error = drv_i915_bootfb_layout(&fb, &layout);
	i915_host_check(error == EINVAL, "REFUSE: a format other than RGBX8888 and BGRX8888");

	/* A size that does not hold every visible row. */
	i915_host_bootfb_fb(&fb, 1920U, 1080U, 1920U, KERN_BOOT_FRAMEBUFFER_BGRX8888);
	fb.size = 8294400U - 4U;
	error = drv_i915_bootfb_layout(&fb, &layout);
	i915_host_check(error == EINVAL, "REFUSE: a size one pixel short of the visible rows");

	/* A side beyond the scanout layout's limit. */
	i915_host_bootfb_fb(&fb, 8193U, 16U, 8193U, KERN_BOOT_FRAMEBUFFER_BGRX8888);
	error = drv_i915_bootfb_layout(&fb, &layout);
	i915_host_check(error == EINVAL, "REFUSE: a framebuffer 8193 pixels wide");
}

/* Checks the clip of frames smaller than, equal to and larger than the framebuffer. */
static void
i915_host_bootfb_clip(void)
{
	struct kern_boot_framebuffer fb;
	struct i915_bootfb_layout layout;
	uint32_t width;
	uint32_t height;
	int error;

	/* A 1920x1080 framebuffer. */
	i915_host_bootfb_fb(&fb, 1920U, 1080U, 1920U, KERN_BOOT_FRAMEBUFFER_BGRX8888);
	(void)drv_i915_bootfb_layout(&fb, &layout);

	/* The same extent is copied whole. */
	error = drv_i915_bootfb_clip(&layout, 1920U, 1080U, &width, &height);
	i915_host_check(error == 0 && width == 1920U && height == 1080U, "CLIP: a 1920x1080 frame is copied whole");

	/* A smaller frame is copied whole to the top-left. */
	error = drv_i915_bootfb_clip(&layout, 320U, 240U, &width, &height);
	i915_host_check(error == 0 && width == 320U && height == 240U, "CLIP: a 320x240 frame is copied whole");

	/* A larger frame is clipped on each side separately. */
	error = drv_i915_bootfb_clip(&layout, 2560U, 1000U, &width, &height);
	i915_host_check(error == 0 && width == 1920U && height == 1000U, "CLIP: a 2560x1000 frame keeps 1920 columns and its 1000 rows");
	error = drv_i915_bootfb_clip(&layout, 1000U, 1440U, &width, &height);
	i915_host_check(error == 0 && width == 1000U && height == 1080U, "CLIP: a 1000x1440 frame keeps its 1000 columns and 1080 rows");

	/* An empty frame is refused. */
	error = drv_i915_bootfb_clip(&layout, 0U, 1080U, &width, &height);
	i915_host_check(error == EINVAL, "CLIP: an empty frame is refused");
}

/* Checks the swap for both frame formats on both framebuffer orders. */
static void
i915_host_bootfb_swap(void)
{
	struct i915_bootfb_layout layout;
	int swap;

	memset(&layout, 0, sizeof(layout));

	/* A BGRX framebuffer (GOP's usual order). */
	layout.rgbx = 0;
	swap = drv_i915_bootfb_swap(&layout, GPU_PIXEL_BGRA8888);
	i915_host_check(swap == 0, "SWAP: a BGRA frame onto BGRX goes as it is");
	swap = drv_i915_bootfb_swap(&layout, GPU_PIXEL_RGBA8888);
	i915_host_check(swap == 1, "SWAP: an RGBA frame onto BGRX is swapped");

	/* An RGBX framebuffer. */
	layout.rgbx = 1;
	swap = drv_i915_bootfb_swap(&layout, GPU_PIXEL_BGRA8888);
	i915_host_check(swap == 1, "SWAP: a BGRA frame onto RGBX is swapped");
	swap = drv_i915_bootfb_swap(&layout, GPU_PIXEL_RGBA8888);
	i915_host_check(swap == 0, "SWAP: an RGBA frame onto RGBX goes as it is");
}

/* Checks the row copy byte by byte, with and without the swap, and that it writes no column past the copied ones. */
static void
i915_host_bootfb_rows(void)
{
	static const uint8_t source_bytes[12] = {
		0x11U, 0x22U, 0x33U, 0x44U,
		0x55U, 0x66U, 0x77U, 0x88U,
		0x99U, 0xaaU, 0xbbU, 0xccU
	};
	static const uint8_t swapped_bytes[8] = {
		0x33U, 0x22U, 0x11U, 0x44U,
		0x77U, 0x66U, 0x55U, 0x88U
	};
	uint32_t source[3];
	uint32_t destination[3];
	uint8_t written[12];

	memcpy(source, source_bytes, sizeof(source));

	/* Without the swap: the same bytes, two columns, the third left alone. */
	memset(destination, 0xeeU, sizeof(destination));
	drv_i915_bootfb_copy_row(destination, source, 2U, 0);
	memcpy(written, destination, sizeof(written));
	i915_host_check(memcmp(written, source_bytes, 8U) == 0 &&
			written[8] == 0xeeU && written[11] == 0xeeU,
			"ROW: two columns copied as they are, the third untouched");

	/* With the swap: the first and third bytes of each pixel exchanged. */
	memset(destination, 0xeeU, sizeof(destination));
	drv_i915_bootfb_copy_row(destination, source, 2U, 1);
	memcpy(written, destination, sizeof(written));
	i915_host_check(memcmp(written, swapped_bytes, 8U) == 0 &&
			written[8] == 0xeeU,
			"ROW: two columns with red and blue swapped (11 22 33 44 -> 33 22 11 44), the third untouched");
}

/* Fills a boot handoff framebuffer record of the given extent, stride (pixels) and format, its size exactly its rows. */
static void
i915_host_bootfb_fb(
	struct kern_boot_framebuffer *fb,
	uint32_t width,
	uint32_t height,
	uint32_t stride,
	uint32_t format)
{
	memset(fb, 0, sizeof(*fb));
	fb->physical_base = 0x4000000000ULL;
	fb->size = (uint64_t)stride * 4U * height;
	fb->width = width;
	fb->height = height;
	fb->pitch = stride;
	fb->format = format;
}
