/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The inverse transforms of the lossless format.
 */

#include "vp8l-transform.h"

#include <errno.h>

/*
 * Gives DIV_ROUND_UP(size, 1 << bits) (RFC 9649 3.5.1), for size 1 or more
 * and bits 0..9.
 */
unsigned
webpc_vp8l_subsample(
	unsigned size,
	unsigned bits)
{
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(bits);

	/* Not implemented yet. */
	return 0;
}

/*
 * Undoes the predictor transform (RFC 9649 3.5.1) in place over pixels
 * (width by height, rows top to bottom), in scan-line order so that each
 * prediction uses pixels already restored.
 */
void
webpc_vp8l_inverse_predictor(
	uint32_t *pixels,
	unsigned width,
	unsigned height,
	const uint32_t *modes,
	unsigned size_bits)
{
	UNUSED_PARAMETER(pixels);
	UNUSED_PARAMETER(width);
	UNUSED_PARAMETER(height);
	UNUSED_PARAMETER(modes);
	UNUSED_PARAMETER(size_bits);

	/* Not implemented yet. */
}

/*
 * Undoes the colour transform (RFC 9649 3.5.2) in place over pixels.
 */
void
webpc_vp8l_inverse_color(
	uint32_t *pixels,
	unsigned width,
	unsigned height,
	const uint32_t *elements,
	unsigned size_bits)
{
	UNUSED_PARAMETER(pixels);
	UNUSED_PARAMETER(width);
	UNUSED_PARAMETER(height);
	UNUSED_PARAMETER(elements);
	UNUSED_PARAMETER(size_bits);

	/* Not implemented yet. */
}

/*
 * Undoes the subtract-green transform (RFC 9649 3.5.3) over count pixels:
 * red and blue each get green added modulo 256.
 */
void
webpc_vp8l_add_green(
	uint32_t *pixels,
	size_t count)
{
	UNUSED_PARAMETER(pixels);
	UNUSED_PARAMETER(count);

	/* Not implemented yet. */
}

/*
 * Undoes the colour-indexing transform (RFC 9649 3.5.4): the packed image
 * (packed_width = webpc_vp8l_subsample(width, width_bits) pixels a row,
 * height rows) becomes the image of width pixels a row in out.
 */
void
webpc_vp8l_inverse_indexing(
	const uint32_t *packed,
	unsigned width,
	unsigned height,
	unsigned width_bits,
	const uint32_t *table,
	uint32_t *out)
{
	UNUSED_PARAMETER(packed);
	UNUSED_PARAMETER(width);
	UNUSED_PARAMETER(height);
	UNUSED_PARAMETER(width_bits);
	UNUSED_PARAMETER(table);
	UNUSED_PARAMETER(out);

	/* Not implemented yet. */
}
