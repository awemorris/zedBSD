/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The conversion of a decoded image into the output layouts.
 */

#include "output.h"

#include <errno.h>

/*
 * Converts one pixel's Y', Cb (u) and Cr (v), each 0..255, to rgb[0..2]
 * (red, green, blue).
 */
void
webpc_output_yuv_pixel(
	unsigned y,
	unsigned u,
	unsigned v,
	uint8_t rgb[3])
{
	UNUSED_PARAMETER(y);
	UNUSED_PARAMETER(u);
	UNUSED_PARAMETER(v);
	UNUSED_PARAMETER(rgb);

	/* Not implemented yet. */
}

/*
 * Premultiplies a component x (0..255) by alpha a (0..255) as libwebp does:
 * (x * a) / 255, rounded down (x itself when a is 255).
 */
unsigned
webpc_output_premultiply(
	unsigned x,
	unsigned a)
{
	UNUSED_PARAMETER(x);
	UNUSED_PARAMETER(a);

	/* Not implemented yet. */
	return 0;
}

/*
 * Writes a lossy image as RGB rows: the row of image row r starts at
 * first_row + r * row_step (row_step negative to flip; the caller has
 * checked that every row fits its buffer).
 */
void
webpc_output_from_yuv(
	const struct webpc_yuv_view *image,
	const uint8_t *alpha,
	ptrdiff_t alpha_stride,
	int fancy,
	const struct webpc_layout *layout,
	uint8_t *first_row,
	ptrdiff_t row_step)
{
	UNUSED_PARAMETER(image);
	UNUSED_PARAMETER(alpha);
	UNUSED_PARAMETER(alpha_stride);
	UNUSED_PARAMETER(fancy);
	UNUSED_PARAMETER(layout);
	UNUSED_PARAMETER(first_row);
	UNUSED_PARAMETER(row_step);

	/* Not implemented yet. */
}

/*
 * Writes a lossless image (width * height ARGB pixels, rows top to bottom)
 * as rows of layout, the row of image row r at first_row + r * row_step.
 */
void
webpc_output_from_argb(
	const uint32_t *pixels,
	unsigned width,
	unsigned height,
	const struct webpc_layout *layout,
	uint8_t *first_row,
	ptrdiff_t row_step)
{
	UNUSED_PARAMETER(pixels);
	UNUSED_PARAMETER(width);
	UNUSED_PARAMETER(height);
	UNUSED_PARAMETER(layout);
	UNUSED_PARAMETER(first_row);
	UNUSED_PARAMETER(row_step);

	/* Not implemented yet. */
}
