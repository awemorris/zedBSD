/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The equivalent paths of the SVG shape elements.
 */

#include "svg/svg-shape.h"

#include <errno.h>

/*
 * Not implemented yet.
 */
int
svg_shape_is(
	const char *name)
{
	(void)name;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
int
svg_shape_path(
	const struct svg_tree *tree,
	const struct svg_node *element,
	const char *name,
	const struct svg_context *context,
	struct vec_path *out,
	enum svg_shape_result *result)
{
	(void)tree;
	(void)element;
	(void)name;
	(void)context;
	(void)out;
	(void)result;

	/* Not implemented yet. */
	return ENOSYS;
}
