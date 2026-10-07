/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The copy of a frame into the presenter's canvas by its changed part
 * (BUG-221, see present-copy.h).
 */

#include "present-copy.h"

#include <string.h>

/*
 * Clips a changed part to a frame of width by height pixels: 1 with the
 * part inside the frame, 0 when nothing of it is (the frame is then taken
 * whole).
 */
int
keiui_present_part_clip(
	uint32_t width,
	uint32_t height,
	const struct kl_rect *part,
	struct kl_rect *clipped)
{
	int right;
	int bottom;

	/* The part's edges, inside the frame. */
	clipped->x = part->x;
	clipped->y = part->y;
	right = part->x + part->width;
	bottom = part->y + part->height;
	if (clipped->x < 0)
		clipped->x = 0;
	if (clipped->y < 0)
		clipped->y = 0;
	if (right > (int)width)
		right = (int)width;
	if (bottom > (int)height)
		bottom = (int)height;

	/* Nothing left of it. */
	if (right <= clipped->x || bottom <= clipped->y)
		return 0;

	/* Succeeded: the part inside the frame. */
	clipped->width = right - clipped->x;
	clipped->height = bottom - clipped->y;
	return 1;
}

/*
 * Copies an area of a frame (stride words a row) into the canvas (pitch
 * bytes a row); the canvas keeps the rest.  The area lies inside both.
 */
void
keiui_present_copy(
	unsigned char *canvas,
	size_t pitch,
	const uint32_t *pixels,
	size_t stride,
	const struct kl_rect *area)
{
	int row;

	/* The area's rows. */
	for (row = area->y; row < area->y + area->height; row++)
		memcpy(canvas + (size_t)row * pitch + (size_t)area->x * 4U, pixels + (size_t)row * stride + (size_t)area->x, (size_t)area->width * 4U);
}
