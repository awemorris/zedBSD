/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The value syntaxes of SVG attributes and presentation properties:
 * numbers, lengths, lists, the transform attribute, viewBox,
 * preserveAspectRatio, paint and url references, and the transform an SVG
 * viewport makes of its viewBox.
 *
 * Every parser takes text as NUL-terminated UTF-8 (an attribute value),
 * reads only up to the NUL, skips the white space SVG allows (space, tab,
 * line feed, carriage return, form feed) around the value, and refuses a
 * value with anything left over: the "invalid value" of SVG 2 §4.2, which
 * the caller treats as the attribute not being there (the property's
 * initial or inherited value).  A parser that fails leaves its output as
 * it was.
 */

#ifndef KEILAND_BROWSER_SVG_VALUES_H
#define KEILAND_BROWSER_SVG_VALUES_H

#include "svg/svg-tree.h"
#include "vector/vec-base.h"

/* The most numbers a list attribute gives (points, dash arrays, viewBox). */
#define SVG_NUMBERS_MAX		65536

/* The most functions a transform list holds. */
#define SVG_TRANSFORMS_MAX	256

/* The longest id an url reference names (bytes, without the NUL). */
#define SVG_ID_MAX		255

/*
 * The CSS pixels of the absolute units (CSS Values 3 §5.2, which SVG 2 §8.9
 * follows): 1in = 96px = 2.54cm, 1pt = 1/72in, 1pc = 12pt, 1Q = 1/4mm.
 */
#define SVG_PX_PER_IN		96.0f
#define SVG_PX_PER_CM		(96.0f / 2.54f)
#define SVG_PX_PER_MM		(96.0f / 25.4f)
#define SVG_PX_PER_Q		(96.0f / 101.6f)
#define SVG_PX_PER_PT		(96.0f / 72.0f)
#define SVG_PX_PER_PC		16.0f

/* The units of a length. */
enum svg_unit {
	SVG_UNIT_NUMBER = 0,	/* a bare number: user units */
	SVG_UNIT_PX,
	SVG_UNIT_PERCENT,
	SVG_UNIT_EM,
	SVG_UNIT_EX,
	SVG_UNIT_REM,
	SVG_UNIT_IN,
	SVG_UNIT_CM,
	SVG_UNIT_MM,
	SVG_UNIT_Q,
	SVG_UNIT_PT,
	SVG_UNIT_PC
};

/* A length as written: its number and unit. */
struct svg_length {
	float value;
	enum svg_unit unit;
};

/*
 * Which dimension a percentage length is of (SVG 2 §8.9, from SVG 1.1
 * §7.10): the viewport's width, its height, or its normalized diagonal
 * sqrt((width^2 + height^2) / 2).
 */
enum svg_axis {
	SVG_AXIS_X = 0,
	SVG_AXIS_Y = 1,
	SVG_AXIS_OTHER = 2
};

/*
 * What lengths are resolved against: the nearest viewport's size in user
 * units, the element's font size (em; ex is half of it, the fallback CSS
 * Values 3 §5.1.1 allows when the font gives no x-height) and the root's
 * (rem), all in user units.
 */
struct svg_context {
	float viewport_width;
	float viewport_height;
	float font_size;
	float root_font_size;
};

/* The alignments of preserveAspectRatio (SVG 2 §8.7). */
enum svg_align {
	SVG_ALIGN_NONE = 0,
	SVG_ALIGN_XMIN_YMIN,
	SVG_ALIGN_XMID_YMIN,
	SVG_ALIGN_XMAX_YMIN,
	SVG_ALIGN_XMIN_YMID,
	SVG_ALIGN_XMID_YMID,
	SVG_ALIGN_XMAX_YMID,
	SVG_ALIGN_XMIN_YMAX,
	SVG_ALIGN_XMID_YMAX,
	SVG_ALIGN_XMAX_YMAX
};

/* preserveAspectRatio: the alignment, and slice (1) or meet (0). */
struct svg_aspect {
	enum svg_align align;
	int slice;
};

/* The kinds of a paint (SVG 2 §13.2). */
enum svg_paint_kind {
	SVG_PAINT_NONE = 0,
	SVG_PAINT_COLOR,
	SVG_PAINT_CURRENT_COLOR,
	SVG_PAINT_URL
};

/*
 * A paint: none, a color, currentColor, or a reference to a paint server
 * (its id, without the '#') with a fallback that is none, a color or
 * currentColor (fallback_kind SVG_PAINT_URL meaning no fallback was
 * given, which SVG 2 §13.2 treats as none when the reference fails).
 * color is straight 0xAARRGGBB for SVG_PAINT_COLOR (and fallback_color for
 * a color fallback).
 */
struct svg_paint {
	enum svg_paint_kind kind;
	uint32_t color;
	char id[SVG_ID_MAX + 1];
	enum svg_paint_kind fallback_kind;
	uint32_t fallback_color;
};

/*
 * Reads one number at *cursor (the <number> of SVG 2 §4.2 and CSS:
 * an optional sign, digits with an optional fraction, or a fraction alone,
 * and an optional exponent e or E with an optional sign and digits).  No
 * white space is skipped.  On success *cursor moves past it.
 *
 * Errors: EINVAL when no number starts there or its value is not finite
 * as a float (overflow); *cursor and *value are then unchanged.
 */
int svg_number_read(const char **cursor, float *value);

/*
 * Parses a whole attribute as one number.  Errors: EINVAL.
 */
int svg_parse_number(const char *text, float *value);

/*
 * Parses a whole attribute as a length: a number and an optional unit
 * (px, %, em, ex, rem, in, cm, mm, Q, pt, pc; the unit's letters
 * matched without regard to ASCII case, as CSS units are).
 *
 * Errors: EINVAL (an unknown unit too).
 */
int svg_parse_length(const char *text, struct svg_length *length);

/*
 * Resolves a length to user units: absolute units by the SVG_PX_PER_*
 * factors (user units are CSS px in the initial viewport), em by
 * font_size, ex by font_size / 2, rem by root_font_size, a percentage of
 * the axis's dimension of the viewport.
 *
 * Returns the value; a result that is not finite gives 0.
 */
float svg_length_resolve(const struct svg_length *length, enum svg_axis axis, const struct svg_context *context);

/*
 * Parses a list of numbers separated by white space and/or one comma
 * (SVG 2 §4.2's comma-wsp), as 'points' and 'viewBox' are written.  An
 * empty or all-white list gives 0 numbers.
 *
 * Input: values room for capacity numbers (capacity <= SVG_NUMBERS_MAX).
 * Output: *count the numbers read.
 * Errors: EINVAL for a malformed list (a stray comma, a leading or
 * trailing comma, a non-number); ENOBUFS for more than capacity; *count
 * is then the numbers read before the failure (and values holds them),
 * which the caller of 'points' uses (SVG 2 §10.6: the list up to the
 * error is rendered).
 */
int svg_parse_numbers(const char *text, float *values, size_t capacity, size_t *count);

/*
 * Parses a list of lengths separated as numbers are (stroke-dasharray), or
 * the keyword none (count 0).  Input, output and errors as
 * svg_parse_numbers, except that nothing is kept from a malformed list
 * (*count 0).
 */
int svg_parse_lengths(const char *text, struct svg_length *values, size_t capacity, size_t *count);

/*
 * Parses the transform attribute (CSS Transforms 1 §7.2's grammar of the
 * SVG transform attribute, and §7.3's functions): matrix(a b c d e f),
 * translate(tx [ty]), scale(sx [sy]), rotate(a [cx cy]), skewX(a),
 * skewY(a), separated by white space and/or commas, arguments separated
 * the same way.  The attribute is the presentation attribute of the
 * transform property (CSS Transforms 1 §7.1), so besides bare numbers an
 * angle may carry deg, grad, rad or turn and a length of translate or
 * the centre of rotate px (units matched without regard to ASCII case;
 * 1turn = 360deg, 1grad = 0.9deg, 1rad = 180 / pi deg); a bare angle is in
 * degrees and a bare length in user units.  The functions compose
 * left to right as nested coordinate systems (the leftmost applies last
 * to a point: the matrix is M1 x M2 x ... ).  rotate(a cx cy) is
 * translate(cx cy) rotate(a) translate(-cx -cy).  An empty attribute is
 * the identity.
 *
 * Errors: EINVAL for any malformed part (the whole attribute is then
 * invalid and *matrix unchanged); ENOBUFS for more than
 * SVG_TRANSFORMS_MAX functions.
 */
int svg_parse_transform(const char *text, struct vec_matrix *matrix);

/*
 * Parses viewBox (SVG 2 §8.6): four numbers min-x, min-y, width, height
 * into a rectangle (left = min-x, right = min-x + width, ...).
 *
 * Errors: EINVAL for anything but four numbers, or a negative width or
 * height (§8.6: an error that disables the attribute); a zero width or
 * height is valid here and the caller does not render the element.
 */
int svg_parse_view_box(const char *text, struct vec_rect *view_box);

/*
 * Parses preserveAspectRatio (SVG 2 §8.7): one of the ten alignment
 * keywords, then optionally meet or slice (meet when absent).  Keywords
 * are matched case-sensitively.
 *
 * Errors: EINVAL.
 */
int svg_parse_aspect(const char *text, struct svg_aspect *aspect);

/*
 * Computes the transform of a viewBox into a viewport (SVG 2 §8.2, the
 * algorithm "Computing the equivalent transform of an SVG viewport"):
 * the viewport at (x, y) of width by height, the viewBox view_box, the
 * alignment aspect.  The result maps user units of the viewBox to the
 * viewport's coordinates.
 *
 * Input: view_box with positive width and height; width and height >= 0.
 * Output: *matrix.
 * Errors: EINVAL for a view_box without positive size or values that are
 * not finite; *matrix unchanged.
 */
int svg_view_box_transform(const struct vec_rect *view_box, const struct svg_aspect *aspect, float x, float y, float width, float height, struct vec_matrix *matrix);

/*
 * Reads an url reference to a local element: url(#id), url('#id'),
 * url("#id") with white space allowed inside the parentheses; or, with
 * bare set, a plain "#id" (as href is written).  The id is copied to id
 * (NUL-terminated) and *cursor moves past the reference.  A reference to
 * another document (anything but '#' first) is refused with ENOTSUP.
 *
 * Errors: EINVAL for a malformed reference or an empty id; ENOBUFS for an
 * id longer than SVG_ID_MAX; ENOTSUP; id and *cursor unchanged then.
 */
int svg_url_read(const char **cursor, int bare, char id[SVG_ID_MAX + 1]);

/*
 * Parses a paint (SVG 2 §13.2): none, currentColor (ASCII case-insensitive),
 * a color (through tree->parse_color, given the trimmed text), or an url
 * reference optionally followed by a fallback (none, currentColor or a
 * color).  context-fill and context-stroke are refused with ENOTSUP.
 *
 * Errors: EINVAL; ENOTSUP; *paint unchanged then.
 */
int svg_parse_paint(const struct svg_tree *tree, const char *text, struct svg_paint *paint);

#endif
