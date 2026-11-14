/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Pictures drawn through a sink as device-space fills.
 */

#include "svg/svg-draw.h"

#include <errno.h>

/*
 * Not implemented yet.
 */
int
svg_picture_draw(
	const struct svg_picture *picture,
	const struct vec_matrix *placement,
	float tolerance,
	const struct svg_sink *sink)
{
	(void)picture;
	(void)placement;
	(void)tolerance;
	(void)sink;

	/* Not implemented yet. */
	return ENOSYS;
}
