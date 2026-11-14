/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The colors of linear, radial and conic gradients and their ramps.
 */

#include "vector/vec-gradient.h"

#include <errno.h>

/*
 * Not implemented yet.
 */
int
vec_gradient_fixup_css(
	struct vec_stop *stops,
	size_t *count)
{
	(void)stops;
	(void)count;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
int
vec_gradient_fixup_svg(
	struct vec_stop *stops,
	size_t count)
{
	(void)stops;
	(void)count;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
int
vec_gradient_extend_span(
	struct vec_stop *stops,
	size_t count,
	float *start,
	float *length)
{
	(void)stops;
	(void)count;
	(void)start;
	(void)length;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
int
vec_gradient_repeat_span(
	struct vec_stop *stops,
	size_t count,
	float *start,
	float *length)
{
	(void)stops;
	(void)count;
	(void)start;
	(void)length;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
uint32_t
vec_gradient_color_at(
	const struct vec_stop *stops,
	size_t count,
	int premultiplied,
	float position)
{
	(void)stops;
	(void)count;
	(void)premultiplied;
	(void)position;

	/* Not implemented yet. */
	return 0;
}

/*
 * Not implemented yet.
 */
uint32_t
vec_gradient_average(
	const struct vec_stop *stops,
	size_t count,
	int premultiplied)
{
	(void)stops;
	(void)count;
	(void)premultiplied;

	/* Not implemented yet. */
	return 0;
}

/*
 * Not implemented yet.
 */
int
vec_gradient_ramp_width(
	float length)
{
	(void)length;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
int
vec_gradient_ramp(
	const struct vec_gradient *gradient,
	int width,
	uint32_t *ramp)
{
	(void)gradient;
	(void)width;
	(void)ramp;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
float
vec_gradient_parameter(
	const struct vec_gradient *gradient,
	float x,
	float y,
	int *painted)
{
	(void)gradient;
	(void)x;
	(void)y;
	(void)painted;

	/* Not implemented yet. */
	return 0.0f;
}

/*
 * Not implemented yet.
 */
float
vec_gradient_spread(
	enum vec_spread spread,
	float t)
{
	(void)spread;
	(void)t;

	/* Not implemented yet. */
	return 0.0f;
}

/*
 * Not implemented yet.
 */
uint32_t
vec_gradient_sample(
	const struct vec_gradient *gradient,
	const uint32_t *ramp,
	int width,
	float x,
	float y)
{
	(void)gradient;
	(void)ramp;
	(void)width;
	(void)x;
	(void)y;

	/* Not implemented yet. */
	return 0;
}

/*
 * Not implemented yet.
 */
float
vec_gradient_line_length(
	const struct vec_gradient *gradient,
	const struct vec_rect *area)
{
	(void)gradient;
	(void)area;

	/* Not implemented yet. */
	return 0.0f;
}
