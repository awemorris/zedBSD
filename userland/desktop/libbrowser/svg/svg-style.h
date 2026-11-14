/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The presentation properties an SVG element is drawn with, as the
 * element's presentation attributes and its style attribute set them and
 * its parent passes the inherited ones down (SVG 2 §6 Styling, the
 * property definitions of §13 Painting, §3.2.3 display and visibility,
 * §3.6.1 opacity, CSS Masking 1 §5.1 and §6.2).
 *
 * Style sheets are not read here: the cascade from <style> elements and
 * the page's sheets is not applied (the properties are what the
 * attributes and the style attribute give).  Within an element, the style
 * attribute's declarations override its presentation attributes (SVG 2
 * §6.6: presentation attributes are author-level declarations of
 * specificity 0 that come before all others).  !important is ignored.
 */

#ifndef KEILAND_BROWSER_SVG_STYLE_H
#define KEILAND_BROWSER_SVG_STYLE_H

#include "svg/svg-values.h"
#include "vector/vec-raster.h"
#include "vector/vec-stroke.h"

/* The display values that matter here: none hides the element and its subtree (SVG 2 §3.2.3). */
#define SVG_DISPLAY_INLINE	0
#define SVG_DISPLAY_NONE	1

/* The deepest chain of ancestors svg_style_of follows. */
#define SVG_STYLE_DEPTH_MAX	64

/* visibility (SVG 2 §3.2.3): hidden and collapse do not paint the element itself (its children may). */
#define SVG_VISIBILITY_VISIBLE	0
#define SVG_VISIBILITY_HIDDEN	1

/*
 * The properties of one element, resolved as far as they can be without
 * a viewport (lengths stay lengths; svg-shape and svg-picture resolve
 * them).  Inherited (I) or not (N), and the initial value, per the
 * property definitions:
 *
 *   fill              I  black (SVG_PAINT_COLOR 0xff000000)
 *   fill_rule         I  nonzero
 *   fill_opacity      I  1, clamped to [0, 1]
 *   stroke            I  none
 *   stroke_width      I  1 (user units; a length, % of the normalized
 *                       diagonal; a negative value is invalid)
 *   stroke_opacity    I  1, clamped to [0, 1]
 *   stroke_linecap    I  butt
 *   stroke_linejoin   I  miter (miter-clip and arcs map to miter)
 *   stroke_miterlimit I  4 (a value < 1 is invalid)
 *   dashes            I  none (dash_count 0; a list with a negative
 *                       length is invalid, SVG 2 §13.5.7)
 *   dash_offset       I  0 (a length)
 *   color             I  black; currentColor in fill and stroke is
 *                       resolved against it when the element is drawn
 *                       (SVG 2 §13.3)
 *   visibility        I  visible
 *   clip_rule         I  nonzero
 *   font_size         I  16 (px; the em and ex base: absolute lengths, and
 *                       percentages and em of the parent's size)
 *   display           N  inline
 *   opacity           N  1, clamped to [0, 1]
 *   clip_path         N  none (has_clip_path 0); else the id it refers to
 *   stop_color        N  black (on 'stop' elements; currentColor allowed)
 *   stop_opacity      N  1
 *
 * stop_color_current and the fill and stroke paints of kind
 * SVG_PAINT_CURRENT_COLOR keep currentColor unresolved; color holds the
 * value it resolves to.
 */
struct svg_style {
	struct svg_paint fill;
	enum vec_fill_rule fill_rule;
	float fill_opacity;
	struct svg_paint stroke;
	struct svg_length stroke_width;
	float stroke_opacity;
	enum vec_cap stroke_linecap;
	enum vec_join stroke_linejoin;
	float stroke_miterlimit;
	struct svg_length dashes[VEC_DASHES_MAX];
	size_t dash_count;
	struct svg_length dash_offset;
	uint32_t color;
	int visibility;
	enum vec_fill_rule clip_rule;
	float font_size;
	int display;
	float opacity;
	int has_clip_path;
	char clip_path[SVG_ID_MAX + 1];
	uint32_t stop_color;
	int stop_color_current;
	float stop_opacity;
};

/* Sets every property to its initial value. */
void svg_style_initial(struct svg_style *style);

/*
 * Makes a child's starting style from its parent's: the inherited
 * properties copied, the others at their initial values.
 */
void svg_style_inherit(struct svg_style *child, const struct svg_style *parent);

/*
 * Applies one declaration to a style: the property by name (ASCII
 * case-insensitive, as CSS property names are), its value as text.  The
 * value inherit takes the parent's value (parent NULL: the initial value),
 * initial the initial value.  A property not listed in struct svg_style
 * is ignored (returns 0); a listed one with an invalid value leaves the
 * property as it was (returns EINVAL, which callers ignore: an invalid
 * declaration is dropped, CSS Syntax).  font-size takes a length in px,
 * absolute units, em or % (of the parent's size).
 *
 * Input: name and value NUL-terminated; value trimmed or not (it is).
 * Errors: EINVAL as above; ENOTSUP for a paint of context-fill or
 * context-stroke (the property keeps its value).
 */
int svg_style_declare(struct svg_style *style, const struct svg_style *parent, const struct svg_tree *tree, const char *name, const char *value);

/*
 * Applies an element's presentation attributes, then its style
 * attribute's declarations (name: value; ... as CSS declarations, a
 * malformed one skipped up to the next semicolon outside parentheses and
 * quotes), to a style made by svg_style_inherit from the parent's.
 *
 * The presentation attributes read are the attributes named as the
 * properties above (fill, fill-rule, fill-opacity, stroke, stroke-width,
 * stroke-opacity, stroke-linecap, stroke-linejoin, stroke-miterlimit,
 * stroke-dasharray, stroke-dashoffset, color, visibility, clip-rule,
 * font-size, display, opacity, clip-path, stop-color, stop-opacity).
 *
 * Errors: none from the values (invalid ones are dropped); returns 0.
 * The tree's functions are called on element only.
 */
int svg_style_apply(struct svg_style *style, const struct svg_style *parent, const struct svg_tree *tree, const struct svg_node *element);

/*
 * Computes the style of any element of the SVG fragment from its
 * ancestors, as the walk of the rendering tree would have: the chain of
 * elements from the fragment's root (the outermost ancestor whose parent
 * is not an element named by tree->name, i.e. the outermost 'svg' of the
 * SVG namespace) down to element, the root's style made by
 * svg_style_inherit from outer (or svg_style_initial when outer is NULL),
 * then each element's by svg_style_inherit from its parent's and
 * svg_style_apply.  This is how a referenced element that is not drawn
 * where it sits gets its inherited properties: a 'clipPath' (CSS Masking
 * 1 §6.1: properties inherit into it from its ancestors, not from the
 * element that references it), a gradient and its 'stop' children (SVG 2
 * §14.2.4: a stop inherits from its gradient element).
 *
 * Input: tree; element an element; outer the style the fragment's root
 * inherits from (the page gives the 'svg' element's inherited properties:
 * color, and fill and stroke when set on an HTML ancestor), or NULL.
 * Output: *style.
 * Errors: ENOBUFS when element is deeper than SVG_STYLE_DEPTH_MAX below
 * the root (*style is then the initial style).
 */
int svg_style_of(const struct svg_tree *tree, const struct svg_node *element, const struct svg_style *outer, struct svg_style *style);

#endif
