/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Pictures drawn into bitmaps on the CPU.
 */

#include "svg/svg-bitmap.h"

#include <errno.h>

/*
 * Not implemented yet.
 */
int
svg_picture_render(
	const struct svg_picture *picture,
	const struct vec_matrix *placement,
	uint32_t *pixels,
	int width,
	int height,
	int stride)
{
	(void)picture;
	(void)placement;
	(void)pixels;
	(void)width;
	(void)height;
	(void)stride;

	/* Not implemented yet. */
	return ENOSYS;
}
