/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The layout of a mip tree the texture unit samples, the same on V3D 4.2
 * and 7.1: the texture formatting unit writes each level in its place and
 * tiling, and the texture record names level 0 and the number of levels.
 */
#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/native-state.h"

/* The largest side a native texture has. */
#define NATIVE_TEXTURE_DIMENSION 4096U

/*
 * A mip tree's layout (Mesa's v3d_setup_slices and v3d_get_ub_pad,
 * src/gallium/drivers/v3d/v3d_resource.c, with v3d_screen.h's sizes): UIF
 * pages of 4096 bytes, eight banks of them in the page cache, and rows of
 * four 256-byte UIF blocks; a four-byte texel's utile is four by four and its
 * UIF block eight by eight.
 */
#define MIP_PAGE_BYTES 4096U
#define MIP_PAGE_CACHE_BYTES (MIP_PAGE_BYTES * 8U)
#define MIP_UIF_ROW_BYTES 1024U
#define MIP_PAGE_UB_ROWS (MIP_PAGE_BYTES / MIP_UIF_ROW_BYTES)
#define MIP_PAGE_UB_ROWS_1_5 ((MIP_PAGE_UB_ROWS * 3U) >> 1)
#define MIP_PAGE_CACHE_UB_ROWS (MIP_PAGE_CACHE_BYTES / MIP_UIF_ROW_BYTES)
#define MIP_PAGE_CACHE_MINUS_1_5_UB_ROWS (MIP_PAGE_CACHE_UB_ROWS - MIP_PAGE_UB_ROWS_1_5)
#define MIP_UTILE 4U
#define MIP_UIF_BLOCK 8U

static uint32_t mip_ub_pad(uint32_t height);
static uint32_t mip_align(uint32_t value, uint32_t alignment);

/*
 * Lays out a mip tree of four-byte texels as the texture unit reads it
 * (Mesa's v3d_setup_slices for a tiled texture whose level 0 need not be
 * UIF): from the smallest level up, each level tiled by its size (linear
 * tiles when a side fits a utile, rows of one or two UIF block columns when
 * the width fits them, else UIF with the UB pad that keeps page banks apart,
 * XOR when its block rows fill whole page caches), level 1 page-aligned when
 * a lower level could be UIF XOR, and the whole tree moved so that level 0
 * starts on a page.  Only power-of-two sides are laid out, where the
 * hardware's padding of the smaller levels to powers of two adds nothing.
 */
int
bcm2711_native_miptree_layout(
	uint32_t width,
	uint32_t height,
	uint32_t levels,
	struct bcm2711_native_miptree *tree)
{
	struct bcm2711_native_mip_level *level;
	uint32_t offset;
	uint32_t total;
	uint32_t pad;
	uint32_t padded_width;
	uint32_t index;

	/* A tree of two levels or more, within the levels a side has, of power-of-two sides. */
	if (tree == NULL)
		return EINVAL;
	kern_memset(tree, 0, sizeof(*tree));
	if (levels < 2U || levels > BCM2711_NATIVE_TEXTURE_LEVELS)
		return EINVAL;
	if (width == 0 || height == 0 || width > NATIVE_TEXTURE_DIMENSION || height > NATIVE_TEXTURE_DIMENSION)
		return EINVAL;
	if ((width & (width - 1U)) != 0 || (height & (height - 1U)) != 0)
		return ENOTSUP;
	if ((width >> (levels - 1U)) == 0 && (height >> (levels - 1U)) == 0)
		return EINVAL;

	/* From the smallest level to level 0. */
	offset = 0;
	index = levels;
	while (index > 0) {
		index--;
		level = &tree->level[index];
		level->width = width >> index;
		if (level->width == 0)
			level->width = 1;
		level->height = height >> index;
		if (level->height == 0)
			level->height = 1;

		/* The level's tiling and the padded extent it takes. */
		padded_width = level->width;
		level->padded_height = level->height;
		if (level->width <= MIP_UTILE || level->height <= MIP_UTILE) {
			level->tiling = BCM2711_NATIVE_TILING_LINEARTILE;
			padded_width = mip_align(padded_width, MIP_UTILE);
			level->padded_height = mip_align(level->padded_height, MIP_UTILE);
		} else if (level->width <= MIP_UIF_BLOCK) {
			level->tiling = BCM2711_NATIVE_TILING_UBLINEAR_1;
			padded_width = mip_align(padded_width, MIP_UIF_BLOCK);
			level->padded_height = mip_align(level->padded_height, MIP_UIF_BLOCK);
		} else if (level->width <= 2U * MIP_UIF_BLOCK) {
			level->tiling = BCM2711_NATIVE_TILING_UBLINEAR_2;
			padded_width = mip_align(padded_width, 2U * MIP_UIF_BLOCK);
			level->padded_height = mip_align(level->padded_height, MIP_UIF_BLOCK);
		} else {
			/* Four block columns wide, block rows high, and the UB pad. */
			padded_width = mip_align(padded_width, 4U * MIP_UIF_BLOCK);
			level->padded_height = mip_align(level->padded_height, MIP_UIF_BLOCK);
			level->ub_pad = mip_ub_pad(level->padded_height);
			level->padded_height += level->ub_pad * MIP_UIF_BLOCK;

			/* Rows of blocks that fill whole page caches use XOR to stay misaligned. */
			level->tiling = BCM2711_NATIVE_TILING_UIF;
			if ((level->padded_height / MIP_UIF_BLOCK) % MIP_PAGE_CACHE_UB_ROWS == 0)
				level->tiling = BCM2711_NATIVE_TILING_UIF_XOR;
		}

		/* The level's bytes, and level 1 page-aligned when a level below it could be UIF XOR. */
		level->bytes = padded_width * 4U * level->padded_height;
		level->offset = offset;
		total = level->bytes;
		if (index == 1U &&
		    padded_width > 4U * MIP_UIF_BLOCK &&
		    level->padded_height > MIP_PAGE_CACHE_MINUS_1_5_UB_ROWS * MIP_UIF_BLOCK)
			total = mip_align(total, MIP_PAGE_BYTES);
		offset += total;
	}

	/* Level 0 starts on a page; every level moves with it. */
	pad = mip_align(tree->level[0].offset, MIP_PAGE_BYTES) - tree->level[0].offset;
	for (index = 0; index < levels; index++)
		tree->level[index].offset += pad;
	tree->levels = levels;
	tree->bytes = mip_align(offset + pad, 64U);

	/* Succeeded: the tree's levels and size. */
	return 0;
}

/* The UIF block rows a level's height is padded by, so that page banks stay apart between block columns. */
static uint32_t
mip_ub_pad(
	uint32_t height)
{
	uint32_t rows;
	uint32_t in_cache;

	/* A height that fills whole page caches takes no pad (XOR misaligns it). */
	rows = height / MIP_UIF_BLOCK;
	in_cache = rows % MIP_PAGE_CACHE_UB_ROWS;
	if (in_cache == 0)
		return 0;

	/* Padded to at least one and a half pages off, unless the level fits in the page cache. */
	if (in_cache < MIP_PAGE_UB_ROWS_1_5) {
		if (rows < MIP_PAGE_CACHE_UB_ROWS)
			return 0;
		return MIP_PAGE_UB_ROWS_1_5 - in_cache;
	}

	/* Close to a whole page cache: rounded up to it, for XOR. */
	if (in_cache > MIP_PAGE_CACHE_MINUS_1_5_UB_ROWS)
		return MIP_PAGE_CACHE_UB_ROWS - in_cache;

	/* Far enough from both: no pad. */
	return 0;
}

/* Rounds value up to a power-of-two alignment. */
static uint32_t
mip_align(
	uint32_t value,
	uint32_t alignment)
{
	/* Succeeded: the next multiple. */
	return (value + alignment - 1U) & ~(alignment - 1U);
}
