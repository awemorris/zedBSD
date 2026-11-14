/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The common ground of the vector drawing: the affine matrix and the checks of points and rectangles.
 */

#include "vector/vec-base.h"

#include <errno.h>

/*
 * Not implemented yet.
 */
int
vec_finite(
	float value)
{
	(void)value;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
int
vec_point_valid(
	struct vec_point point)
{
	(void)point;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
void
vec_matrix_identity(
	struct vec_matrix *matrix)
{
	(void)matrix;
}

/*
 * Not implemented yet.
 */
int
vec_matrix_valid(
	const struct vec_matrix *matrix)
{
	(void)matrix;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
void
vec_matrix_multiply(
	struct vec_matrix *out,
	const struct vec_matrix *first,
	const struct vec_matrix *then)
{
	(void)out;
	(void)first;
	(void)then;
}

/*
 * Not implemented yet.
 */
int
vec_matrix_invert(
	struct vec_matrix *out,
	const struct vec_matrix *matrix)
{
	(void)out;
	(void)matrix;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
struct vec_point
vec_matrix_apply(
	const struct vec_matrix *matrix,
	struct vec_point point)
{
	(void)matrix;
	(void)point;

	/* Not implemented yet. */
	return point;
}

/*
 * Not implemented yet.
 */
struct vec_point
vec_matrix_apply_vector(
	const struct vec_matrix *matrix,
	struct vec_point vector)
{
	(void)matrix;
	(void)vector;

	/* Not implemented yet. */
	return vector;
}

/*
 * Not implemented yet.
 */
float
vec_matrix_scale_max(
	const struct vec_matrix *matrix)
{
	(void)matrix;

	/* Not implemented yet. */
	return 0.0f;
}

/*
 * Not implemented yet.
 */
int
vec_matrix_translate(
	struct vec_matrix *out,
	float tx,
	float ty)
{
	(void)out;
	(void)tx;
	(void)ty;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
int
vec_matrix_scale(
	struct vec_matrix *out,
	float sx,
	float sy)
{
	(void)out;
	(void)sx;
	(void)sy;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
int
vec_matrix_rotate(
	struct vec_matrix *out,
	float degrees)
{
	(void)out;
	(void)degrees;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
int
vec_matrix_skew_x(
	struct vec_matrix *out,
	float degrees)
{
	(void)out;
	(void)degrees;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
int
vec_matrix_skew_y(
	struct vec_matrix *out,
	float degrees)
{
	(void)out;
	(void)degrees;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
void
vec_rect_include(
	struct vec_rect *rect,
	struct vec_point point)
{
	(void)rect;
	(void)point;
}

/*
 * Not implemented yet.
 */
void
vec_rect_empty(
	struct vec_rect *rect)
{
	(void)rect;
}
