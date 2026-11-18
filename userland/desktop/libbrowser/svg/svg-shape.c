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
#include <stdlib.h>
#include <string.h>

#include "svg/svg-pathdata.h"

/*
 * The shape elements, as the table below names them.
 */
enum shape_kind {
	SHAPE_KIND_NONE = 0,
	SHAPE_KIND_PATH,
	SHAPE_KIND_RECT,
	SHAPE_KIND_CIRCLE,
	SHAPE_KIND_ELLIPSE,
	SHAPE_KIND_LINE,
	SHAPE_KIND_POLYLINE,
	SHAPE_KIND_POLYGON
};

/*
 * One row of the table of shape element names: the element's local name
 * and the kind of shape it is.
 */
struct shape_name {
	const char *name;
	enum shape_kind kind;
};

/*
 * The local names of the shape elements.  The table is read-only and is
 * searched in order; names are matched case-sensitively, as the element
 * names of SVG are.
 *
 * Values referenced: SVG 2 §10.1 (the shape elements: circle, ellipse,
 * line, path, polygon, polyline and rect).
 */
static const struct shape_name shape_names[] = {
	{ "path", SHAPE_KIND_PATH },
	{ "rect", SHAPE_KIND_RECT },
	{ "circle", SHAPE_KIND_CIRCLE },
	{ "ellipse", SHAPE_KIND_ELLIPSE },
	{ "line", SHAPE_KIND_LINE },
	{ "polyline", SHAPE_KIND_POLYLINE },
	{ "polygon", SHAPE_KIND_POLYGON }
};

/* The number of rows of shape_names. */
#define SHAPE_NAME_COUNT	(sizeof(shape_names) / sizeof(shape_names[0]))

/* The forward declarations of the static functions. */
static enum shape_kind shape_kind_of(const char *name);
static int shape_in_range(const float *values, size_t count);
static int shape_length(const struct svg_tree *tree, const struct svg_node *element, const char *attribute, enum svg_axis axis, const struct svg_context *context, float *value);
static int shape_radius(const struct svg_tree *tree, const struct svg_node *element, const char *attribute, enum svg_axis axis, const struct svg_context *context, float *value);
static void shape_auto_radii(int rx_set, int ry_set, float *rx, float *ry);
static int shape_build_rect(const struct svg_tree *tree, const struct svg_node *element, const struct svg_context *context, struct vec_path *out, enum svg_shape_result *result);
static int shape_build_circle(const struct svg_tree *tree, const struct svg_node *element, const struct svg_context *context, struct vec_path *out, enum svg_shape_result *result);
static int shape_build_ellipse(const struct svg_tree *tree, const struct svg_node *element, const struct svg_context *context, struct vec_path *out, enum svg_shape_result *result);
static int shape_build_line(const struct svg_tree *tree, const struct svg_node *element, const struct svg_context *context, struct vec_path *out, enum svg_shape_result *result);
static int shape_build_points(const struct svg_tree *tree, const struct svg_node *element, int closed, struct vec_path *out, enum svg_shape_result *result);
static int shape_points_to_path(const float *values, size_t count, int closed, struct vec_path *out, enum svg_shape_result *result);
static int shape_build_path(const struct svg_tree *tree, const struct svg_node *element, struct vec_path *out, enum svg_shape_result *result);

/*
 * Tells whether an element name is one of the shape elements.
 *
 * The name is compared whole and case-sensitively; NULL is not a shape.
 */
int
svg_shape_is(
	const char *name)
{
	enum shape_kind kind;

	/* Looks the name up in the table of shape elements. */
	kind = shape_kind_of(name);

	/* Reports that the name is not a shape element. */
	if (kind == SHAPE_KIND_NONE)
		return 0;

	/* Reports that the name is a shape element. */
	return 1;
}

/*
 * Builds the equivalent path of a shape element.
 *
 * The element's name picks the construction.  Every construction appends
 * to out and sets *result to DRAWN only when it appended a path; a shape
 * that renders nothing leaves out as it was.  A failed path command (or a
 * length beyond the coordinate range) is the function's error, and the
 * caller draws nothing of the element then.
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
	enum shape_kind kind;
	int error;

	/* Refuses a missing result, so that no caller reads it unset. */
	if (result == NULL)
		return EINVAL;

	/* Starts with the element taken as rendering nothing. */
	*result = SVG_SHAPE_EMPTY;

	/* Refuses missing arguments, so that a NULL is never read. */
	if (tree == NULL || element == NULL || context == NULL || out == NULL)
		return EINVAL;
	if (tree->attribute == NULL)
		return EINVAL;

	/* Tells a name that is not a shape from one that is. */
	kind = shape_kind_of(name);
	if (kind == SHAPE_KIND_NONE) {
		*result = SVG_SHAPE_NONE;
		return 0;
	}

	/* Builds the path of the kind of shape the element is. */
	error = 0;
	switch (kind) {
	case SHAPE_KIND_PATH:
		error = shape_build_path(tree, element, out, result);
		break;
	case SHAPE_KIND_RECT:
		error = shape_build_rect(tree, element, context, out, result);
		break;
	case SHAPE_KIND_CIRCLE:
		error = shape_build_circle(tree, element, context, out, result);
		break;
	case SHAPE_KIND_ELLIPSE:
		error = shape_build_ellipse(tree, element, context, out, result);
		break;
	case SHAPE_KIND_LINE:
		error = shape_build_line(tree, element, context, out, result);
		break;
	case SHAPE_KIND_POLYLINE:
		error = shape_build_points(tree, element, 0, out, result);
		break;
	case SHAPE_KIND_POLYGON:
		error = shape_build_points(tree, element, 1, out, result);
		break;
	default:
		/* A kind the table does not hold is not a shape. */
		*result = SVG_SHAPE_NONE;
		break;
	}

	/* Draws nothing of an element whose path could not be built. */
	if (error != 0) {
		*result = SVG_SHAPE_EMPTY;
		return error;
	}

	/* Succeeded: *result tells what out holds. */
	return 0;
}

/*
 * Looks an element name up in the table of shape elements.
 */
static enum shape_kind
shape_kind_of(
	const char *name)
{
	size_t index;
	int different;

	/* Takes no name as no shape. */
	if (name == NULL)
		return SHAPE_KIND_NONE;

	/* Compares the name with each row of the table. */
	for (index = 0;
	     index < SHAPE_NAME_COUNT;
	     index++) {
		different = strcmp(name, shape_names[index].name);
		if (different == 0)
			return shape_names[index].kind;
	}

	/* Every other name is not a shape element. */
	return SHAPE_KIND_NONE;
}

/*
 * Tells whether resolved lengths are usable coordinates: the magnitude of
 * each is at most VEC_COORD_MAX (each is finite, as svg_length_resolve
 * gives).  Reports 1 for all of them, 0 when one is beyond.
 */
static int
shape_in_range(
	const float *values,
	size_t count)
{
	size_t index;

	/* Looks at each length. */
	for (index = 0;
	     index < count;
	     index++) {
		/* Refuses a length beyond the coordinate range of the vector modules. */
		if (values[index] > VEC_COORD_MAX)
			return 0;
		if (values[index] < -VEC_COORD_MAX)
			return 0;
	}

	/* Accepts lengths that are all within the range. */
	return 1;
}

/*
 * Reads an attribute as a length and resolves it to user units.
 *
 * Reports 1 and the value when the attribute is there and is a valid
 * length; reports 0 and a value of 0 when it is missing or invalid (SVG 2
 * §4.2: an invalid value is as if the attribute were not there).
 */
static int
shape_length(
	const struct svg_tree *tree,
	const struct svg_node *element,
	const char *attribute,
	enum svg_axis axis,
	const struct svg_context *context,
	float *value)
{
	const char *text;
	struct svg_length length;
	int error;

	/* Starts with the length taken as not given. */
	*value = 0.0f;

	/* Reads the attribute's text. */
	text = tree->attribute(tree->context, element, attribute);
	if (text == NULL)
		return 0;

	/* Parses the text as one length; invalid text is the attribute not being there. */
	error = svg_parse_length(text, &length);
	if (error != 0)
		return 0;

	/* Resolves the length to user units against the viewport. */
	*value = svg_length_resolve(&length, axis, context);

	/* Succeeded: the attribute gave a length. */
	return 1;
}

/*
 * Reads a radius attribute: a length that is not negative.
 *
 * Reports 1 and the value for a valid length of zero or more; reports 0
 * and a value of 0 for a missing, invalid or negative one, which is auto
 * (SVG 2 §10.2 and §10.4: a negative value is invalid and ignored).
 */
static int
shape_radius(
	const struct svg_tree *tree,
	const struct svg_node *element,
	const char *attribute,
	enum svg_axis axis,
	const struct svg_context *context,
	float *value)
{
	int given;

	/* Reads the radius as a length. */
	given = shape_length(tree, element, attribute, axis, context, value);
	if (!given)
		return 0;

	/* Takes a negative radius as invalid, which is auto. */
	if (*value < 0.0f) {
		*value = 0.0f;
		return 0;
	}

	/* Succeeded: the radius is a length of zero or more. */
	return 1;
}

/*
 * Gives an auto radius the value of the other one, and 0 to both when
 * both are auto (SVG 2 §10.2, the first two steps of the used values of
 * rx and ry; the ellipse follows them too, §10.4).
 */
static void
shape_auto_radii(
	int rx_set,
	int ry_set,
	float *rx,
	float *ry)
{
	/* Takes both radii as 0 when neither is given. */
	if (!rx_set && !ry_set) {
		*rx = 0.0f;
		*ry = 0.0f;
	} else if (rx_set && !ry_set) {
		/* Lets ry take the absolute value of rx. */
		*ry = *rx;
	} else if (!rx_set && ry_set) {
		/* Lets rx take the absolute value of ry. */
		*rx = *ry;
	}
}

/*
 * Builds the path of a rect: the rectangle, rounded when both used radii
 * are positive (SVG 2 §10.2).
 */
static int
shape_build_rect(
	const struct svg_tree *tree,
	const struct svg_node *element,
	const struct svg_context *context,
	struct vec_path *out,
	enum svg_shape_result *result)
{
	struct vec_rrect rrect;
	float geometry[4];
	float x;
	float y;
	float width;
	float height;
	float rx;
	float ry;
	float right;
	float bottom;
	float half;
	int rx_set;
	int ry_set;
	int in_range;
	int corner;
	int error;

	/* Reads the position and the size. */
	shape_length(tree, element, "x", SVG_AXIS_X, context, &x);
	shape_length(tree, element, "y", SVG_AXIS_Y, context, &y);
	shape_length(tree, element, "width", SVG_AXIS_X, context, &width);
	shape_length(tree, element, "height", SVG_AXIS_Y, context, &height);

	/* Renders nothing for a size of zero or less (a negative size is invalid, which is the initial 0). */
	if (width <= 0.0f || height <= 0.0f)
		return 0;

	/* Refuses a rectangle beyond the coordinate range. */
	geometry[0] = x;
	geometry[1] = y;
	geometry[2] = width;
	geometry[3] = height;
	in_range = shape_in_range(geometry, 4);
	if (!in_range)
		return EINVAL;

	/* Reads the radii and gives an auto one the value of the other. */
	rx_set = shape_radius(tree, element, "rx", SVG_AXIS_X, context, &rx);
	ry_set = shape_radius(tree, element, "ry", SVG_AXIS_Y, context, &ry);
	shape_auto_radii(rx_set, ry_set, &rx, &ry);

	/*
	 * Finds the far edges.  The radii are clamped against the sides as the
	 * edges give them, which are the given sizes up to rounding, so that
	 * the rounded path always fits the rectangle it is drawn in.
	 */
	right = x + width;
	bottom = y + height;

	/* Clamps rx to half of the width (SVG 2 §10.2, the last step). */
	half = (right - x) * 0.5f;
	if (rx > half)
		rx = half;

	/* Clamps ry to half of the height. */
	half = (bottom - y) * 0.5f;
	if (ry > half)
		ry = half;

	/* Adds a plain rectangle when either radius is zero (no arcs are drawn then). */
	if (rx <= 0.0f || ry <= 0.0f) {
		error = vec_path_append_rect(out, x, y, width, height);
		if (error != 0)
			return error;

		*result = SVG_SHAPE_DRAWN;
		return 0;
	}

	/* Takes the same radii at every corner. */
	rrect.rect.left = x;
	rrect.rect.top = y;
	rrect.rect.right = right;
	rrect.rect.bottom = bottom;
	for (corner = 0;
	     corner < 4;
	     corner++) {
		rrect.radii[corner][0] = rx;
		rrect.radii[corner][1] = ry;
	}

	/* Adds the rectangle with rounded corners. */
	error = vec_path_append_rounded_rect(out, &rrect);
	if (error != 0)
		return error;

	/* Succeeded: the path is the rounded rectangle. */
	*result = SVG_SHAPE_DRAWN;
	return 0;
}

/*
 * Builds the path of a circle: an ellipse of equal radii (SVG 2 §10.3).
 */
static int
shape_build_circle(
	const struct svg_tree *tree,
	const struct svg_node *element,
	const struct svg_context *context,
	struct vec_path *out,
	enum svg_shape_result *result)
{
	float geometry[3];
	float cx;
	float cy;
	float r;
	int in_range;
	int error;

	/* Reads the center and the radius (a percentage radius is of the diagonal, SVG 2 §8.9). */
	shape_length(tree, element, "cx", SVG_AXIS_X, context, &cx);
	shape_length(tree, element, "cy", SVG_AXIS_Y, context, &cy);
	shape_length(tree, element, "r", SVG_AXIS_OTHER, context, &r);

	/* Renders nothing for a radius of zero or less (a negative radius is invalid, which is the initial 0). */
	if (r <= 0.0f)
		return 0;

	/* Refuses a circle beyond the coordinate range. */
	geometry[0] = cx;
	geometry[1] = cy;
	geometry[2] = r;
	in_range = shape_in_range(geometry, 3);
	if (!in_range)
		return EINVAL;

	/* Adds the four quarter arcs of the circle as one closed subpath. */
	error = vec_path_append_ellipse(out, cx, cy, r, r);
	if (error != 0)
		return error;

	/* Succeeded: the path is the circle. */
	*result = SVG_SHAPE_DRAWN;
	return 0;
}

/*
 * Builds the path of an ellipse (SVG 2 §10.4).
 */
static int
shape_build_ellipse(
	const struct svg_tree *tree,
	const struct svg_node *element,
	const struct svg_context *context,
	struct vec_path *out,
	enum svg_shape_result *result)
{
	float geometry[4];
	float cx;
	float cy;
	float rx;
	float ry;
	int rx_set;
	int ry_set;
	int in_range;
	int error;

	/* Reads the center. */
	shape_length(tree, element, "cx", SVG_AXIS_X, context, &cx);
	shape_length(tree, element, "cy", SVG_AXIS_Y, context, &cy);

	/* Reads the radii and gives an auto one the value of the other (no clamping for an ellipse). */
	rx_set = shape_radius(tree, element, "rx", SVG_AXIS_X, context, &rx);
	ry_set = shape_radius(tree, element, "ry", SVG_AXIS_Y, context, &ry);
	shape_auto_radii(rx_set, ry_set, &rx, &ry);

	/* Renders nothing for a radius of zero, which is also what two auto radii come to. */
	if (rx <= 0.0f || ry <= 0.0f)
		return 0;

	/* Refuses an ellipse beyond the coordinate range. */
	geometry[0] = cx;
	geometry[1] = cy;
	geometry[2] = rx;
	geometry[3] = ry;
	in_range = shape_in_range(geometry, 4);
	if (!in_range)
		return EINVAL;

	/* Adds the four quarter arcs of the ellipse as one closed subpath. */
	error = vec_path_append_ellipse(out, cx, cy, rx, ry);
	if (error != 0)
		return error;

	/* Succeeded: the path is the ellipse. */
	*result = SVG_SHAPE_DRAWN;
	return 0;
}

/*
 * Builds the path of a line: a move to its start and a line to its end
 * (SVG 2 §10.5).
 */
static int
shape_build_line(
	const struct svg_tree *tree,
	const struct svg_node *element,
	const struct svg_context *context,
	struct vec_path *out,
	enum svg_shape_result *result)
{
	float x1;
	float y1;
	float x2;
	float y2;
	int error;

	/* Reads the two end points. */
	shape_length(tree, element, "x1", SVG_AXIS_X, context, &x1);
	shape_length(tree, element, "y1", SVG_AXIS_Y, context, &y1);
	shape_length(tree, element, "x2", SVG_AXIS_X, context, &x2);
	shape_length(tree, element, "y2", SVG_AXIS_Y, context, &y2);

	/* Starts the subpath at the first end point. */
	error = vec_path_move_to(out, x1, y1);
	if (error != 0)
		return error;

	/* Draws the line to the second end point. */
	error = vec_path_line_to(out, x2, y2);
	if (error != 0)
		return error;

	/* Succeeded: the path is the open line. */
	*result = SVG_SHAPE_DRAWN;
	return 0;
}

/*
 * Builds the path of a polyline or a polygon from its points attribute
 * (SVG 2 §10.6 and §10.7).
 *
 * The list of numbers is read into a buffer that is as large as the text
 * could hold numbers (at least one character each and one between two),
 * at most SVG_NUMBERS_MAX.  A list that is malformed or too long keeps the
 * numbers read before the fault.
 */
static int
shape_build_points(
	const struct svg_tree *tree,
	const struct svg_node *element,
	int closed,
	struct vec_path *out,
	enum svg_shape_result *result)
{
	const char *text;
	float *values;
	size_t capacity;
	size_t count;
	int error;
	int list_error;

	/* Reads the attribute's text; a missing attribute is the initial value, which renders nothing. */
	text = tree->attribute(tree->context, element, "points");
	if (text == NULL)
		return 0;

	/* Sizes the buffer by the text, within the limit of a list. */
	capacity = strlen(text) / 2 + 1;
	if (capacity > SVG_NUMBERS_MAX)
		capacity = SVG_NUMBERS_MAX;

	/* Allocates the buffer of numbers. */
	values = malloc(capacity * sizeof(*values));
	if (values == NULL)
		return ENOMEM;

	/*
	 * Reads the numbers.  A fault in the list (EINVAL or ENOBUFS) is not
	 * an error of the shape: the numbers before it are kept (SVG 2 §10.6).
	 */
	count = 0;
	list_error = svg_parse_numbers(text, values, capacity, &count);
	(void)list_error;

	/* Builds the path from the numbers, then gives the buffer back. */
	error = shape_points_to_path(values, count, closed, out, result);
	free(values);
	if (error != 0)
		return error;

	/* Succeeded: *result tells whether the path has any point. */
	return 0;
}

/*
 * Turns the numbers of a points attribute into a path: a move to the first
 * pair, a line to each other pair, and a close for a polygon.  An odd last
 * number is dropped (SVG 2 §10.6); no pair renders nothing.
 */
static int
shape_points_to_path(
	const float *values,
	size_t count,
	int closed,
	struct vec_path *out,
	enum svg_shape_result *result)
{
	size_t pairs;
	size_t index;
	int error;

	/* Drops a last number that has no partner. */
	pairs = count / 2;

	/* Renders nothing for a list without a pair. */
	if (pairs == 0)
		return 0;

	/* Starts the subpath at the first pair. */
	error = vec_path_move_to(out, values[0], values[1]);
	if (error != 0)
		return error;

	/* Draws a line to each other pair. */
	for (index = 1;
	     index < pairs;
	     index++) {
		error = vec_path_line_to(out, values[index * 2], values[index * 2 + 1]);
		if (error != 0)
			return error;
	}

	/* Closes the subpath of a polygon. */
	if (closed) {
		error = vec_path_close(out);
		if (error != 0)
			return error;
	}

	/* Succeeded: the path holds the points. */
	*result = SVG_SHAPE_DRAWN;
	return 0;
}

/*
 * Builds the path of a path element from its 'd' attribute (SVG 2 §9.2).
 *
 * The data is read as far as it is valid (SVG 2 §9.5.4); a path that holds
 * no command afterwards (no 'd', empty data, data that does not start with
 * a moveto) renders nothing.
 */
static int
shape_build_path(
	const struct svg_tree *tree,
	const struct svg_node *element,
	struct vec_path *out,
	enum svg_shape_result *result)
{
	const char *text;
	size_t verbs_before;
	int complete;
	int error;

	/* Reads the attribute's text; a missing 'd' draws nothing. */
	text = tree->attribute(tree->context, element, "d");
	if (text == NULL)
		return 0;

	/* Parses the data into the path; an error in the data keeps what came before it. */
	verbs_before = out->verb_count;
	complete = 0;
	error = svg_path_data_parse(text, out, &complete);
	if (error != 0)
		return error;

	/* Renders nothing when the data added no command. */
	if (out->verb_count == verbs_before)
		return 0;

	/* Succeeded: the path holds the data. */
	*result = SVG_SHAPE_DRAWN;
	return 0;
}
