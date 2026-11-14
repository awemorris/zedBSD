/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * SVG document fragments built into pictures of painting commands.
 */

#include "svg/svg-picture.h"

#include <errno.h>

/*
 * Not implemented yet.
 */
void
svg_picture_init(
	struct svg_picture *picture)
{
	(void)picture;
}

/*
 * Not implemented yet.
 */
void
svg_picture_release(
	struct svg_picture *picture)
{
	(void)picture;
}

/*
 * Not implemented yet.
 */
int
svg_picture_add_path(
	struct svg_picture *picture,
	struct vec_path *path,
	size_t *index)
{
	(void)picture;
	(void)path;
	(void)index;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
int
svg_picture_build(
	const struct svg_tree *tree,
	const struct svg_node *root,
	float width,
	float height,
	float font_size,
	struct svg_picture *picture)
{
	(void)tree;
	(void)root;
	(void)width;
	(void)height;
	(void)font_size;
	(void)picture;

	/* Not implemented yet. */
	return ENOSYS;
}
