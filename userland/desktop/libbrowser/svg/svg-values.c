/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The value syntaxes of SVG attributes and the transform of a viewBox.
 */

#include "svg/svg-values.h"

#include <errno.h>

/*
 * Not implemented yet.
 */
int
svg_number_read(
	const char **cursor,
	float *value)
{
	(void)cursor;
	(void)value;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
int
svg_parse_number(
	const char *text,
	float *value)
{
	(void)text;
	(void)value;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
int
svg_parse_length(
	const char *text,
	struct svg_length *length)
{
	(void)text;
	(void)length;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
float
svg_length_resolve(
	const struct svg_length *length,
	enum svg_axis axis,
	const struct svg_context *context)
{
	(void)length;
	(void)axis;
	(void)context;

	/* Not implemented yet. */
	return 0.0f;
}

/*
 * Not implemented yet.
 */
int
svg_parse_numbers(
	const char *text,
	float *values,
	size_t capacity,
	size_t *count)
{
	(void)text;
	(void)values;
	(void)capacity;
	(void)count;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
int
svg_parse_lengths(
	const char *text,
	struct svg_length *values,
	size_t capacity,
	size_t *count)
{
	(void)text;
	(void)values;
	(void)capacity;
	(void)count;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
int
svg_parse_transform(
	const char *text,
	struct vec_matrix *matrix)
{
	(void)text;
	(void)matrix;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
int
svg_parse_view_box(
	const char *text,
	struct vec_rect *view_box)
{
	(void)text;
	(void)view_box;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
int
svg_parse_aspect(
	const char *text,
	struct svg_aspect *aspect)
{
	(void)text;
	(void)aspect;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
int
svg_view_box_transform(
	const struct vec_rect *view_box,
	const struct svg_aspect *aspect,
	float x,
	float y,
	float width,
	float height,
	struct vec_matrix *matrix)
{
	(void)view_box;
	(void)aspect;
	(void)x;
	(void)y;
	(void)width;
	(void)height;
	(void)matrix;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
int
svg_url_read(
	const char **cursor,
	int bare,
	char id[SVG_ID_MAX + 1])
{
	(void)cursor;
	(void)bare;
	(void)id;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
int
svg_parse_paint(
	const struct svg_tree *tree,
	const char *text,
	struct svg_paint *paint)
{
	(void)tree;
	(void)text;
	(void)paint;

	/* Not implemented yet. */
	return ENOSYS;
}
