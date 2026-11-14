/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Paths built one command at a time, arcs turned into cubic curves, and their tight bounding boxes.
 */

#include "vector/vec-path.h"

#include <errno.h>

/*
 * Not implemented yet.
 */
void
vec_path_init(
	struct vec_path *path)
{
	(void)path;
}

/*
 * Not implemented yet.
 */
void
vec_path_release(
	struct vec_path *path)
{
	(void)path;
}

/*
 * Not implemented yet.
 */
void
vec_path_reset(
	struct vec_path *path)
{
	(void)path;
}

/*
 * Not implemented yet.
 */
int
vec_path_error(
	const struct vec_path *path)
{
	(void)path;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
int
vec_path_move_to(
	struct vec_path *path,
	float x,
	float y)
{
	(void)path;
	(void)x;
	(void)y;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
int
vec_path_line_to(
	struct vec_path *path,
	float x,
	float y)
{
	(void)path;
	(void)x;
	(void)y;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
int
vec_path_quad_to(
	struct vec_path *path,
	float cx,
	float cy,
	float x,
	float y)
{
	(void)path;
	(void)cx;
	(void)cy;
	(void)x;
	(void)y;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
int
vec_path_cubic_to(
	struct vec_path *path,
	float c1x,
	float c1y,
	float c2x,
	float c2y,
	float x,
	float y)
{
	(void)path;
	(void)c1x;
	(void)c1y;
	(void)c2x;
	(void)c2y;
	(void)x;
	(void)y;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
int
vec_path_arc_to(
	struct vec_path *path,
	float rx,
	float ry,
	float rotation_degrees,
	int large_arc,
	int sweep,
	float x,
	float y)
{
	(void)path;
	(void)rx;
	(void)ry;
	(void)rotation_degrees;
	(void)large_arc;
	(void)sweep;
	(void)x;
	(void)y;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
int
vec_path_close(
	struct vec_path *path)
{
	(void)path;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
int
vec_path_append_rect(
	struct vec_path *path,
	float x,
	float y,
	float width,
	float height)
{
	(void)path;
	(void)x;
	(void)y;
	(void)width;
	(void)height;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
int
vec_path_append_rounded_rect(
	struct vec_path *path,
	float x,
	float y,
	float width,
	float height,
	const float radii[4][2])
{
	(void)path;
	(void)x;
	(void)y;
	(void)width;
	(void)height;
	(void)radii;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
int
vec_path_append_ellipse(
	struct vec_path *path,
	float cx,
	float cy,
	float rx,
	float ry)
{
	(void)path;
	(void)cx;
	(void)cy;
	(void)rx;
	(void)ry;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
int
vec_path_transform(
	struct vec_path *path,
	const struct vec_matrix *matrix)
{
	(void)path;
	(void)matrix;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
int
vec_path_copy(
	struct vec_path *destination,
	const struct vec_path *source)
{
	(void)destination;
	(void)source;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
void
vec_path_view_of(
	const struct vec_path *path,
	struct vec_path_view *view)
{
	(void)path;
	(void)view;
}

/*
 * Not implemented yet.
 */
int
vec_path_view_check(
	const struct vec_path_view *view)
{
	(void)view;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
int
vec_path_bounds(
	const struct vec_path_view *view,
	struct vec_rect *bounds)
{
	(void)view;
	(void)bounds;

	/* Not implemented yet. */
	return ENOSYS;
}
