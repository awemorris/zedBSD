/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The linear layout of an image in memory, which the executor computes
 * for vkGetImageMemoryRequirements and vkGetImageSubresourceLayout and
 * writes into the descriptors, so that the driver and the GPU agree on
 * every byte.
 *
 *   Level l is w_l x h_l (x d_l for 3D) texels, w_l = max(1, width >> l)
 *   (the same for h and d).  A row is w_l texels of ps bytes, or
 *   ceil(w_l / 4) blocks of a compressed format, each four texel rows
 *   high, rounded up to a multiple of 64 bytes.  A multisampled image (2D,
 *   one level) has rows of at least P = roundup(width, 2) x S x ps bytes,
 *   the S samples of a pixel side by side, rounded up to 64 bytes in the
 *   same way.  A level's plane
 *   (one slice) is its rows; a level is its planes (a 3D level's d_l
 *   slices one after the other), rounded up to 64 bytes; a layer holds the
 *   levels back to back, and the layers are a layer stride apart, the sum
 *   of the levels.  Every level and layer starts 64-byte aligned.
 */

#ifndef GHCI_LAYOUT_H
#define GHCI_LAYOUT_H

#include <stdint.h>

/*
 * The alignment of a row, a plane and a layer.
 *
 * Values referenced: amaterrace-gpu d9252e4 docs/ghci.md (section 8.5.2,
 * the subresource placement and the 64-byte row alignment).
 */
#define GHCI_LAYOUT_ALIGN 64u

/*
 * What the layout of one image depends on: the bytes of a texel (or of a
 * 4x4 block when block is set), the extent, the levels, the layers and
 * the samples.
 */
struct ghci_layout_image {
	uint32_t texel_bytes;
	uint32_t block;
	uint32_t width;
	uint32_t height;
	uint32_t depth;
	uint32_t levels;
	uint32_t layers;
	uint32_t samples;
};

/* Reports the extent of a level of a dimension: max(1, size >> level). */
static inline uint32_t
ghci_layout_extent(
	uint32_t size,
	uint32_t level)
{
	uint32_t extent;

	/* Halves per level, down to one. */
	extent = size >> level;
	if (extent == 0)
		extent = 1;

	/* Succeeded: the level's extent. */
	return extent;
}

/*
 * Reports the bytes of one row of a level, rounded up to the alignment;
 * a multisampled image's row holds an even number of pixels, each with
 * its samples.
 *
 * Values referenced: amaterrace-gpu 288c459 docs/ghci.md (section 12.11,
 * Multisampled images: P_min = roundup(width, 2) x S x ps, the hardware
 * takes row_pitch_bytes as given, roundup(P_min, 64) recommended; section
 * 8.4, the attachment row pitch).
 */
static inline uint32_t
ghci_layout_row(
	const struct ghci_layout_image *image,
	uint32_t level)
{
	uint32_t width;
	uint32_t bytes;

	/* Whole 4x4 blocks of a compressed format, a multisampled image's pixels with their samples, or texels. */
	width = ghci_layout_extent(image->width, level);
	if (image->block)
		bytes = (width + 3u) / 4u * image->texel_bytes;
	else if (image->samples > 1u)
		bytes = (width + 1u) / 2u * 2u * image->samples * image->texel_bytes;
	else
		bytes = width * image->texel_bytes;

	/* Succeeded: the row, aligned. */
	return (bytes + GHCI_LAYOUT_ALIGN - 1u) / GHCI_LAYOUT_ALIGN * GHCI_LAYOUT_ALIGN;
}

/* Reports the bytes of one plane (one slice) of a level. */
static inline uint32_t
ghci_layout_plane(
	const struct ghci_layout_image *image,
	uint32_t level)
{
	uint32_t rows;

	/* Texel rows, or rows of blocks. */
	rows = ghci_layout_extent(image->height, level);
	if (image->block)
		rows = (rows + 3u) / 4u;

	/* Succeeded: the plane. */
	return rows * ghci_layout_row(image, level);
}

/* Reports the bytes of a level: one plane, or d_l of a 3D image, rounded up to the alignment. */
static inline uint32_t
ghci_layout_level_bytes(
	const struct ghci_layout_image *image,
	uint32_t level)
{
	uint32_t slices;
	uint32_t bytes;

	/* The level's slices. */
	slices = ghci_layout_extent(image->depth, level);
	bytes = ghci_layout_plane(image, level) * slices;

	/* Succeeded: the level, aligned. */
	return (bytes + GHCI_LAYOUT_ALIGN - 1u) / GHCI_LAYOUT_ALIGN * GHCI_LAYOUT_ALIGN;
}

/* Reports the offset of a level in its layer: the levels before it. */
static inline uint32_t
ghci_layout_level_offset(
	const struct ghci_layout_image *image,
	uint32_t level)
{
	uint32_t offset;
	uint32_t index;

	/* The levels before it. */
	offset = 0;
	for (index = 0; index < level; index++)
		offset += ghci_layout_level_bytes(image, index);

	/* Succeeded: the level's offset. */
	return offset;
}

/* Reports the stride of the layers: every level of one layer. */
static inline uint32_t
ghci_layout_layer_stride(
	const struct ghci_layout_image *image)
{
	/* Every level of the layer. */
	return ghci_layout_level_offset(image, image->levels);
}

/* Reports the bytes of the whole image. */
static inline uint64_t
ghci_layout_size(
	const struct ghci_layout_image *image)
{
	/* Every layer. */
	return (uint64_t)ghci_layout_layer_stride(image) * image->layers;
}

#endif /* GHCI_LAYOUT_H */
