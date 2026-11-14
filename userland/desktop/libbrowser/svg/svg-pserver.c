/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * SVG gradient elements resolved into vector gradients.
 */

#include "svg/svg-pserver.h"

#include <errno.h>

/*
 * Not implemented yet.
 */
void
svg_server_init(
	struct svg_server *server)
{
	(void)server;
}

/*
 * Not implemented yet.
 */
void
svg_server_release(
	struct svg_server *server)
{
	(void)server;
}

/*
 * Not implemented yet.
 */
int
svg_server_resolve(
	const struct svg_tree *tree,
	const struct svg_node *element,
	const struct svg_style *gradient_style,
	const struct vec_rect *bbox,
	const struct svg_context *context,
	struct svg_server *server)
{
	(void)tree;
	(void)element;
	(void)gradient_style;
	(void)bbox;
	(void)context;
	(void)server;

	/* Not implemented yet. */
	return ENOSYS;
}
