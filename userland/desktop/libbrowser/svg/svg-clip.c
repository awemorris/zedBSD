/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * SVG clipPath elements built into clips of a picture.
 */

#include "svg/svg-clip.h"

#include <errno.h>

/*
 * Not implemented yet.
 */
int
svg_clip_build(
	const struct svg_tree *tree,
	const struct svg_node *clip_path,
	const struct vec_matrix *user_to_picture,
	const struct vec_rect *bbox,
	const struct svg_context *context,
	int outer,
	struct svg_picture *picture,
	int *index)
{
	(void)tree;
	(void)clip_path;
	(void)user_to_picture;
	(void)bbox;
	(void)context;
	(void)outer;
	(void)picture;
	(void)index;

	/* Not implemented yet. */
	return ENOSYS;
}
