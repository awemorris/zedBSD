/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Paths flattened into polylines within a tolerance.
 */

#include "vector/vec-flatten.h"

#include <errno.h>

/*
 * Not implemented yet.
 */
void
vec_lines_init(
	struct vec_lines *lines)
{
	(void)lines;
}

/*
 * Not implemented yet.
 */
void
vec_lines_release(
	struct vec_lines *lines)
{
	(void)lines;
}

/*
 * Not implemented yet.
 */
void
vec_lines_reset(
	struct vec_lines *lines)
{
	(void)lines;
}

/*
 * Not implemented yet.
 */
int
vec_lines_begin(
	struct vec_lines *lines,
	float x,
	float y,
	unsigned flags)
{
	(void)lines;
	(void)x;
	(void)y;
	(void)flags;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
int
vec_lines_add(
	struct vec_lines *lines,
	float x,
	float y,
	unsigned flags)
{
	(void)lines;
	(void)x;
	(void)y;
	(void)flags;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
void
vec_lines_close(
	struct vec_lines *lines)
{
	(void)lines;
}

/*
 * Not implemented yet.
 */
int
vec_lines_check(
	const struct vec_lines *lines)
{
	(void)lines;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
int
vec_flatten(
	const struct vec_path_view *path,
	const struct vec_matrix *matrix,
	float tolerance,
	struct vec_lines *out)
{
	(void)path;
	(void)matrix;
	(void)tolerance;
	(void)out;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
int
vec_lines_transform(
	struct vec_lines *lines,
	const struct vec_matrix *matrix)
{
	(void)lines;
	(void)matrix;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
int
vec_lines_bounds(
	const struct vec_lines *lines,
	struct vec_rect *bounds)
{
	(void)lines;
	(void)bounds;

	/* Not implemented yet. */
	return ENOSYS;
}
