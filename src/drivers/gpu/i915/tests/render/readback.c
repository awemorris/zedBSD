/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the first draws left in their target, for the oracle test.
 *
 * Linked only into the test build, where it provides the draw checkpoint the
 * renderer calls after each of its first draws.  It logs how many pixels
 * differ from the corner pixel, dumps the first draw's target as hex RGB
 * rows the host rebuilds into an image for the independent oracle
 * (plan/ws014/tests/vkdemo_oracle.py, read by vkdump_verify.py), and draws
 * the first target as a small brightness map.
 */

#include "../../render/gfx.h"
#include "../../render/memory.h"

#include <kern/klog.h>

#include <libc/vulkan/vulkan_core.h>

#include <stddef.h>
#include <stdint.h>

/* How many pixels one dump line carries. */
#define I915_READBACK_DUMP_PIXELS	32U

/* The brightness map's size in cells. */
#define I915_READBACK_MAP_COLUMNS	64U
#define I915_READBACK_MAP_ROWS		20U

void drv_i915_gfx_draw_checkpoint(const struct i915_gfx_image *target, unsigned draw);

static void i915_readback_dump(const struct i915_gfx_image *target, const uint32_t *pixels);
static void i915_readback_map(const struct i915_gfx_image *target, const uint32_t *pixels, uint32_t first);
static uint32_t i915_readback_rgb(const struct i915_gfx_image *target, uint32_t pixel);

/*
 * Reports what a draw left in its target.
 */
void
drv_i915_gfx_draw_checkpoint(
	const struct i915_gfx_image *target,
	unsigned draw)
{
	const uint32_t *pixels;
	uint32_t first;
	uint32_t differ;
	uint32_t x;
	uint32_t y;

	/* Reaches the target's pixels through the CPU view of its allocation. */
	pixels = (const uint32_t *)(const void *)drv_i915_gfx_memory_cpu(target->memory, target->offset, target->bytes);
	if (pixels == NULL)
		return;

	/* Counts the pixels that differ from the corner, which is the clear colour. */
	first = pixels[0];
	differ = 0U;
	for (y = 0U; y < target->height; y++) {
		for (x = 0U; x < target->width; x++) {
			if (pixels[y * (target->pitch / 4U) + x] != first)
				differ++;
		}
	}

	kern_logf("i915: vk: draw %u: %u of %u pixels differ from pixel (0,0) = %08x\n",
	    draw,
	    differ,
	    target->width * target->height,
	    first);

	/* Only the first draw is dumped and mapped. */
	if (draw > 1U)
		return;

	/* Dumps the target for the oracle, then maps it for a reader of the log. */
	i915_readback_dump(target, pixels);
	i915_readback_map(target, pixels, first);
}

/* Dumps the target as hex RGB, 32 pixels to a line, keyed by row and column. */
static void
i915_readback_dump(
	const struct i915_gfx_image *target,
	const uint32_t *pixels)
{
	static const char hex[] = "0123456789abcdef";
	char text[I915_READBACK_DUMP_PIXELS * 6U + 1U];
	uint32_t pixel;
	uint32_t index;
	uint32_t channel;
	uint32_t x;
	uint32_t y;

	/* Prints every whole run of 32 pixels of every row. */
	for (y = 0U; y < target->height; y++) {
		for (x = 0U; x + I915_READBACK_DUMP_PIXELS <= target->width; x += I915_READBACK_DUMP_PIXELS) {
			/* Writes each pixel as six hex digits, red first. */
			for (index = 0U; index < I915_READBACK_DUMP_PIXELS; index++) {
				pixel = i915_readback_rgb(target, pixels[y * (target->pitch / 4U) + x + index]);
				for (channel = 0U; channel < 3U; channel++) {
					text[index * 6U + channel * 2U] = hex[(pixel >> (channel * 8U + 4U)) & 0xfU];
					text[index * 6U + channel * 2U + 1U] = hex[(pixel >> (channel * 8U)) & 0xfU];
				}
			}

			text[I915_READBACK_DUMP_PIXELS * 6U] = '\0';
			kern_logf("vkdump %u %u %s\n", y, x, text);
		}
	}
}

/* Draws the target as 64 x 20 cells, each the brightness of its centre pixel. */
static void
i915_readback_map(
	const struct i915_gfx_image *target,
	const uint32_t *pixels,
	uint32_t first)
{
	static const char shades[] = " .:-=+*#%@";
	char line[I915_READBACK_MAP_COLUMNS + 1U];
	uint32_t pixel;
	uint32_t luma;
	uint32_t column;
	uint32_t row;

	/* Prints one line per cell row. */
	for (row = 0U; row < I915_READBACK_MAP_ROWS; row++) {
		/* Shades each cell by its centre pixel; the clear colour stays blank. */
		for (column = 0U; column < I915_READBACK_MAP_COLUMNS; column++) {
			pixel = pixels[((row * 2U + 1U) * target->height / 40U) * (target->pitch / 4U) + (column * 2U + 1U) * target->width / 128U];
			luma = ((pixel & 0xffU) + ((pixel >> 8) & 0xffU) + ((pixel >> 16) & 0xffU)) / 3U;
			if (pixel == first) {
				line[column] = ' ';
			} else {
				line[column] = shades[luma * 9U / 255U];
			}
		}

		line[I915_READBACK_MAP_COLUMNS] = '\0';
		kern_logf("i915: vk: |%s|\n", line);
	}
}

/* Returns a pixel with red in the low byte, whatever the target's byte order. */
static uint32_t
i915_readback_rgb(
	const struct i915_gfx_image *target,
	uint32_t pixel)
{
	/* A BGRA target keeps blue in the low byte: swaps blue and red. */
	if (target->format == VK_FORMAT_B8G8R8A8_UNORM)
		return ((pixel >> 16) & 0xffU) | (pixel & 0xff00U) | ((pixel & 0xffU) << 16);

	/* An RGBA target already has red in the low byte. */
	return pixel;
}
