/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The alpha chunk of a lossy image: raw or lossless values and the inverse
 * filter.
 */

#include "alpha.h"

#include <errno.h>

/*
 * Decodes an ALPH chunk's payload (payload, size bytes, 1 or more: the
 * header byte, then the alpha bitstream) into alpha (the caller's width *
 * height bytes, rows top to bottom, width a row; width and height are the
 * image's, each 1..WEBPC_VP8_SIDE_MAX).
 */
int
webpc_alpha_decode(
	const uint8_t *payload,
	size_t size,
	unsigned width,
	unsigned height,
	uint8_t *alpha)
{
	UNUSED_PARAMETER(payload);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(width);
	UNUSED_PARAMETER(height);
	UNUSED_PARAMETER(alpha);

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Undoes an alpha prediction filter in place (alpha as webpc_alpha_decode
 * lays it out; method WEBPC_ALPHA_FILTER_*), in scan-line order so each
 * prediction uses values already restored (RFC 9649 2.7.1.2, Figure 11):
 * value = (predictor + stored) modulo 256 with the predictor 0 for method 0,
 * A (left) for 1, B (above) for 2, clip(A + B - C) for 3 (C above-left, clip
 * to 0..255).
 */
void
webpc_alpha_unfilter(
	uint8_t *alpha,
	unsigned width,
	unsigned height,
	unsigned method)
{
	UNUSED_PARAMETER(alpha);
	UNUSED_PARAMETER(width);
	UNUSED_PARAMETER(height);
	UNUSED_PARAMETER(method);

	/* Not implemented yet. */
}
