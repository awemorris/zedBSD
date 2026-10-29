/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The fractional sample interpolation of the software H.264 decoder
 * (8.4.2.2).
 */

#include "h264sw-interp.h"

#include <errno.h>
#include <stddef.h>

/*
 * Predicts a block of luma samples from a reference plane (8.4.2.2.1).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-interp.h.
 */
void
h264sw_interp_luma(
	const uint8_t *plane,
	size_t stride,
	unsigned plane_width,
	unsigned plane_height,
	int x,
	int y,
	unsigned frac_x,
	unsigned frac_y,
	unsigned width,
	unsigned height,
	uint8_t *out,
	size_t out_stride)
{
	UNUSED_PARAMETER(plane);
	UNUSED_PARAMETER(stride);
	UNUSED_PARAMETER(plane_width);
	UNUSED_PARAMETER(plane_height);
	UNUSED_PARAMETER(x);
	UNUSED_PARAMETER(y);
	UNUSED_PARAMETER(frac_x);
	UNUSED_PARAMETER(frac_y);
	UNUSED_PARAMETER(width);
	UNUSED_PARAMETER(height);
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(out_stride);

	/* Does nothing yet. */
	return;
}

/*
 * Predicts a block of chroma samples from a reference plane (8.4.2.2.2).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-interp.h.
 */
void
h264sw_interp_chroma(
	const uint8_t *plane,
	size_t stride,
	unsigned plane_width,
	unsigned plane_height,
	int x,
	int y,
	unsigned frac_x,
	unsigned frac_y,
	unsigned width,
	unsigned height,
	uint8_t *out,
	size_t out_stride)
{
	UNUSED_PARAMETER(plane);
	UNUSED_PARAMETER(stride);
	UNUSED_PARAMETER(plane_width);
	UNUSED_PARAMETER(plane_height);
	UNUSED_PARAMETER(x);
	UNUSED_PARAMETER(y);
	UNUSED_PARAMETER(frac_x);
	UNUSED_PARAMETER(frac_y);
	UNUSED_PARAMETER(width);
	UNUSED_PARAMETER(height);
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(out_stride);

	/* Does nothing yet. */
	return;
}
