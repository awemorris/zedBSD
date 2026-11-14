/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The document tree as the SVG modules see it: an opaque node and a table
 * of functions that walk it and read it.
 *
 * The SVG modules (svg/svg-*.h) do not know the page's DOM.  The page
 * gives them this table over its DOM (the element names in the SVG
 * namespace, the attributes as UTF-8, the lookup of an id); a host test
 * gives them a table over a small tree of its own.  So the modules are
 * built and tested on the host without the engine, and the engine can
 * change its DOM without touching them.
 *
 * Every function of the table must be safe on any node it was given by
 * the table itself; none may call back into the SVG modules.  The strings
 * it returns stay valid and unchanged until the build that asked for them
 * (svg_picture_build) returns.
 */

#ifndef KEILAND_BROWSER_SVG_TREE_H
#define KEILAND_BROWSER_SVG_TREE_H

#include <stddef.h>
#include <stdint.h>

/* A node of the tree: what it is belongs to the table's owner. */
struct svg_node;

/*
 * The functions over a tree, and the context they are given:
 *
 *   parent(context, node)        the parent node, or NULL for the root of
 *                                the document (an element whose parent is
 *                                not an element, such as the document
 *                                node, also gets NULL).
 *   first_child(context, node)   the first child node, or NULL.
 *   next_sibling(context, node)  the next sibling node, or NULL.
 *   name(context, node)          for an element in the SVG namespace, its
 *                                local name as a NUL-terminated ASCII
 *                                string ("rect", "linearGradient"); NULL
 *                                for any other node (text, comments,
 *                                elements of other namespaces).
 *   attribute(context, node, name)
 *                                the value of the element's attribute of
 *                                that local name in no namespace, as
 *                                NUL-terminated UTF-8; for "href" the
 *                                attribute href, or when it has none
 *                                xlink:href (SVG 2 §5.5's and §14.2's href);
 *                                NULL when it has none.
 *   find_id(context, id)         the element whose id is id (the first in
 *                                tree order) in the same document, or NULL.
 *   parse_color(context, text, length, color)
 *                                parses a CSS color (CSS Color 4: a named
 *                                color, a hex color, rgb(), hsl() ...) from
 *                                length bytes of text into straight
 *                                0xAARRGGBB, and returns 0, or EINVAL when it
 *                                is not one.  currentColor is not passed to
 *                                it (the SVG modules resolve it).
 */
struct svg_tree {
	void *context;
	const struct svg_node *(*parent)(void *context, const struct svg_node *node);
	const struct svg_node *(*first_child)(void *context, const struct svg_node *node);
	const struct svg_node *(*next_sibling)(void *context, const struct svg_node *node);
	const char *(*name)(void *context, const struct svg_node *node);
	const char *(*attribute)(void *context, const struct svg_node *node, const char *name);
	const struct svg_node *(*find_id)(void *context, const char *id);
	int (*parse_color)(void *context, const char *text, size_t length, uint32_t *color);
};

#endif
