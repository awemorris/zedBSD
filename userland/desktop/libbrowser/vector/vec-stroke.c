/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The outline of a stroke as pieces that fill together with the nonzero rule.
 */

#include "vector/vec-stroke.h"

#include <errno.h>

/*
 * Not implemented yet.
 */
int
vec_stroke(
	const struct vec_lines *center,
	const struct vec_stroke_style *style,
	const struct vec_matrix *to_device,
	struct vec_lines *out)
{
	(void)center;
	(void)style;
	(void)to_device;
	(void)out;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
float
vec_contour_length(
	const struct vec_lines *lines,
	size_t contour)
{
	(void)lines;
	(void)contour;

	/* Not implemented yet. */
	return 0.0f;
}
