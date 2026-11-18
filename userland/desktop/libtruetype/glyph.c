/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The two calls a caller draws text with: where a glyph sits, and its pixels.
 *
 * Both load the outline, because the box a glyph occupies is the box its
 * outline reaches and nothing else records it at a given size.  A caller
 * that measures and then draws pays for the outline twice; that is the
 * price of not keeping a cache inside the library, which would have to
 * decide how much memory to hold.
 */

#include "internal.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>

/*
 * How much a bold glyph's strokes widen (truetype_set_bold), in ems:
 * across by more than up and down, since a bold face thickens its stems
 * more than its bars.
 */
#define BOLD_ACROSS		(1.0f / 22.0f)
#define BOLD_UPRIGHT		(1.0f / 36.0f)

static int measure(struct truetype_face *face, unsigned glyph,
		   struct truetype_outline *outline,
		   struct truetype_glyph *metrics);

/*
 * Loads one glyph's outline and works out the box it occupies.
 */
static int
measure(
	struct truetype_face *face,
	unsigned glyph,
	struct truetype_outline *outline,
	struct truetype_glyph *metrics)
{
	float minimum_x, minimum_y, maximum_x, maximum_y;
	float across;
	float upright;
	int left, top, right, bottom, error;

	outline->point_count = 0;
	outline->contour_count = 0;
	error = truetype_outline_load(face, glyph, outline, 0);

	/* Reports the failure. */
	if (error != 0)
		return error;
	error = truetype_advance(face, glyph, &metrics->advance);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/*
	 * A bold glyph's outline is widened to the right and upward, and its
	 * advance grows by the widening rounded to whole pixels (a space's
	 * too, so that a bold line keeps its rhythm).
	 */
	if (face->bold != 0U) {
		across = (float)face->pixels * BOLD_ACROSS;
		upright = (float)face->pixels * BOLD_UPRIGHT;
		error = truetype_outline_embolden(outline, across, upright);
		if (error != 0)
			return error;
		metrics->advance += (int)(across + 0.5f);
	}

	/* Handles a glyph with no outline, which is what a space is. */
	if (outline->point_count == 0) {
		metrics->width = 0;
		metrics->height = 0;
		metrics->left = 0;
		metrics->top = 0;

		/* Succeeded: a space advances the pen and draws nothing. */
		return 0;
	}
	truetype_outline_bounds(outline, &minimum_x, &minimum_y,
				&maximum_x, &maximum_y);

	/*
	 * The box is the whole pixels the outline reaches into.  Growing it
	 * outward keeps an edge that ends a fraction of a pixel inside the
	 * last column from losing that column.
	 */
	left = (int)floorf(minimum_x);
	bottom = (int)floorf(minimum_y);
	right = (int)ceilf(maximum_x);
	top = (int)ceilf(maximum_y);
	metrics->width = (unsigned)(right - left);
	metrics->height = (unsigned)(top - bottom);
	metrics->left = left;
	metrics->top = top;

	/* Handles a glyph larger than this will draw. */
	if (metrics->width > TRUETYPE_PIXELS_MAX * 2U ||
	    metrics->height > TRUETYPE_PIXELS_MAX * 2U)
		return ENOTSUP;

	/* Succeeded. */
	return 0;
}

/*
 * Implements the truetype glyph metrics operation.
 */
int
truetype_glyph_metrics(
	struct truetype_face *face,
	unsigned glyph,
	struct truetype_glyph *metrics)
{
	struct truetype_outline *outline;
	int error;

	/* Validates the arguments. */
	if (face == NULL || metrics == NULL)
		return EINVAL;

	/* The face among its companions that draws the glyph (companion.c). */
	face = truetype_resolve(face, &glyph);

	/* The outline is large; it belongs on the heap, not on the stack. */
	outline = malloc(sizeof(*outline));

	/* Handles the allocation failure. */
	if (outline == NULL)
		return ENOMEM;
	error = measure(face, glyph, outline, metrics);
	free(outline);

	/* Returns the computed result. */
	return error;
}

/*
 * Implements the truetype render glyph operation.
 */
int
truetype_render_glyph(
	struct truetype_face *face,
	unsigned glyph,
	struct truetype_glyph *metrics,
	uint8_t *bitmap,
	size_t stride,
	size_t size)
{
	struct truetype_outline *outline;
	int error;

	/* Validates the arguments. */
	if (face == NULL || metrics == NULL || bitmap == NULL)
		return EINVAL;

	/* The face among its companions that draws the glyph (companion.c). */
	face = truetype_resolve(face, &glyph);
	outline = malloc(sizeof(*outline));

	/* Handles the allocation failure. */
	if (outline == NULL)
		return ENOMEM;
	error = measure(face, glyph, outline, metrics);

	/* Reports the failure. */
	if (error != 0) {
		free(outline);
		return error;
	}

	/* Handles a glyph with nothing to draw. */
	if (metrics->width == 0 || metrics->height == 0) {
		free(outline);

		/* Succeeded: a space needs no pixels written. */
		return 0;
	}

	/*
	 * A bitmap that does not hold the glyph is refused rather than
	 * filled with part of it.  metrics has already been written, so the
	 * caller knows what to allocate and can ask again; that is what
	 * makes it safe to read a font from anywhere.
	 */
	if (stride < metrics->width ||
	    size / stride < (size_t)metrics->height) {
		free(outline);
		return EINVAL;
	}
	truetype_rasterize(outline, metrics, bitmap, stride);
	free(outline);

	/* Succeeded. */
	return 0;
}
