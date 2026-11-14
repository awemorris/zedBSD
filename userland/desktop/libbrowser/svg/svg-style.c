/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The presentation properties of SVG elements.
 */

#include "svg/svg-style.h"

#include <errno.h>

/*
 * Not implemented yet.
 */
void
svg_style_initial(
	struct svg_style *style)
{
	(void)style;
}

/*
 * Not implemented yet.
 */
void
svg_style_inherit(
	struct svg_style *child,
	const struct svg_style *parent)
{
	(void)child;
	(void)parent;
}

/*
 * Not implemented yet.
 */
int
svg_style_declare(
	struct svg_style *style,
	const struct svg_style *parent,
	const struct svg_tree *tree,
	const char *name,
	const char *value)
{
	(void)style;
	(void)parent;
	(void)tree;
	(void)name;
	(void)value;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
int
svg_style_apply(
	struct svg_style *style,
	const struct svg_style *parent,
	const struct svg_tree *tree,
	const struct svg_node *element)
{
	(void)style;
	(void)parent;
	(void)tree;
	(void)element;

	/* Not implemented yet. */
	return ENOSYS;
}
