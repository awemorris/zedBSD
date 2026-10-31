/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * shape-outside: the shape of a float that the lines beside it wrap around.
 *
 * A float's shape is worked out when the float is placed, from its computed
 * shape-outside, in the float's margin box, and turned into the coordinates
 * the float is laid out in (a vertical writing mode's logical ones: the
 * shape is given by physical coordinates, the layout runs in logical ones).
 * The room a band of lines has beside the float is the extent of the shape
 * (grown by shape-margin) over the band.
 * https://drafts.csswg.org/css-shapes-1/#shapes-from-box-values
 * https://drafts.csswg.org/css-shapes-1/#basic-shape-functions
 * https://drafts.csswg.org/css-shapes-1/#shapes-from-image
 */

#include "layout/layout.h"

#include <errno.h>
#include <math.h>
#include <string.h>

#define SHAPE_ELLIPSE	1
#define SHAPE_ROUNDED	2
#define SHAPE_POLYGON	3
#define SHAPE_ALPHA	4

/* How many rows are sampled in a pixel of a band, and the distance inside a band's edge a row must still be on the shape. */
#define SHAPE_ROWS_PER_PIXEL	8
#define SHAPE_EDGE		(1.0f / 128.0f)

/* The most intervals along a gradient line where it is opaque enough. */
#define SHAPE_INTERVALS	(CSS_GRADIENT_STOPS + 2)

/*
 * A float's shape in the coordinates the float is laid out in (pixels from
 * the margin box's top left).
 */
struct layout_shape {
	int kind;
	float width;
	float height;
	float margin;

	/* An ellipse: its center and radii. */
	float cx;
	float cy;
	float rx;
	float ry;

	/* A rectangle with rounded corners: its edges and the radii of its corners (top left, top right, bottom right, bottom left). */
	float x0;
	float y0;
	float x1;
	float y1;
	float corner_x[4];
	float corner_y[4];

	/* A polygon. */
	unsigned count;
	float point_x[CSS_SHAPE_POINTS];
	float point_y[CSS_SHAPE_POINTS];

	/* A gradient: the place along its line (x * tx + y * ty + t0) and the stretches of the line that are opaque enough, inside the image's area (x0 to x1, y0 to y1). */
	float tx;
	float ty;
	float t0;
	unsigned intervals;
	float from[SHAPE_INTERVALS];
	float to[SHAPE_INTERVALS];
};

static float shape_length(const struct css_length *length, float reference);
static void shape_point(int mode, float width, float height, float x, float y, float *out_x, float *out_y);
static void shape_set_rounded(struct layout_shape *shape, int mode, float width, float height, float left, float top, float right, float bottom, const float *radius_x, const float *radius_y);
static void shape_box_radii(const struct css_style *style, const layout_unit border[4], const layout_unit padding[4], int reference, float border_width, float border_height, float *radius_x, float *radius_y);
static void shape_clamp_radii(float width, float height, float *radius_x, float *radius_y);
static void shape_build_gradient(struct layout_shape *shape, const struct css_gradient *gradient, float threshold, int mode, float width, float height);
static void shape_gradient_positions(const struct css_gradient *gradient, float line, float *at);
static void shape_add_interval(struct layout_shape *shape, float from, float to);
static int shape_row(const struct layout_shape *shape, float y, float *left, float *right);

/* Computes a length against a reference size (a percentage is of it). */
static float
shape_length(
	const struct css_length *length,
	float reference)
{
	if (length->unit == CSS_UNIT_PERCENT)
		return length->value * reference / 100.0f + length->offset;
	return length->value + length->offset;
}

/*
 * Maps a point of the margin box from physical coordinates to the ones the
 * float is laid out in (width and height are the physical box's): a
 * vertical mode turns the axes, vertical-rl starts its blocks at the right
 * and sideways-lr its lines at the bottom.
 */
static void
shape_point(
	int mode,
	float width,
	float height,
	float x,
	float y,
	float *out_x,
	float *out_y)
{
	if (mode == CSS_WRITING_VERTICAL_LR) {
		*out_x = y;
		*out_y = x;
	} else if (mode == CSS_WRITING_VERTICAL_RL) {
		*out_x = y;
		*out_y = width - x;
	} else if (mode == LAYOUT_SIDEWAYS_LR) {
		*out_x = height - y;
		*out_y = x;
	} else {
		*out_x = x;
		*out_y = y;
	}
}

/* Keeps a rounded rectangle given in physical coordinates, in the float's coordinates: the corners go where the mapping takes them. */
static void
shape_set_rounded(
	struct layout_shape *shape,
	int mode,
	float width,
	float height,
	float left,
	float top,
	float right,
	float bottom,
	const float *radius_x,
	const float *radius_y)
{
	static const int at_right[4] = { 0, 1, 1, 0 };
	static const int at_bottom[4] = { 0, 0, 1, 1 };
	float corner_x[4];
	float corner_y[4];
	float mid_x;
	float mid_y;
	int index;
	int slot;
	int turned;

	/* The mapped corners decide the new box and which corner each is. */
	for (index = 0; index < 4; index++)
		shape_point(mode, width, height, at_right[index] ? right : left, at_bottom[index] ? bottom : top, &corner_x[index], &corner_y[index]);
	shape->x0 = corner_x[0];
	shape->x1 = corner_x[0];
	shape->y0 = corner_y[0];
	shape->y1 = corner_y[0];
	for (index = 1; index < 4; index++) {
		if (corner_x[index] < shape->x0)
			shape->x0 = corner_x[index];
		if (corner_x[index] > shape->x1)
			shape->x1 = corner_x[index];
		if (corner_y[index] < shape->y0)
			shape->y0 = corner_y[index];
		if (corner_y[index] > shape->y1)
			shape->y1 = corner_y[index];
	}
	mid_x = (shape->x0 + shape->x1) / 2.0f;
	mid_y = (shape->y0 + shape->y1) / 2.0f;

	/* The radii follow their corners, swapped when the axes are. */
	turned = mode != CSS_WRITING_HORIZONTAL_TB;
	for (index = 0; index < 4; index++) {
		slot = (corner_y[index] > mid_y ? 3 : 0);
		if (corner_x[index] > mid_x)
			slot = corner_y[index] > mid_y ? 2 : 1;
		shape->corner_x[slot] = turned ? radius_y[index] : radius_x[index];
		shape->corner_y[slot] = turned ? radius_x[index] : radius_y[index];
	}
}

/* Scales corner radii down together when two next to each other would overlap (css-backgrounds-3 5.5). */
static void
shape_clamp_radii(
	float width,
	float height,
	float *radius_x,
	float *radius_y)
{
	float factor;
	float sum;
	int index;

	/* Each side's radii must fit in the side. */
	factor = 1.0f;
	sum = radius_x[0] + radius_x[1];
	if (sum > width && width / sum < factor)
		factor = width / sum;
	sum = radius_x[3] + radius_x[2];
	if (sum > width && width / sum < factor)
		factor = width / sum;
	sum = radius_y[0] + radius_y[3];
	if (sum > height && height / sum < factor)
		factor = height / sum;
	sum = radius_y[1] + radius_y[2];
	if (sum > height && height / sum < factor)
		factor = height / sum;
	for (index = 0; index < 4; index++) {
		radius_x[index] *= factor;
		radius_y[index] *= factor;
	}
}

/*
 * The corner radii a shape box has (the border box's border-radius, grown
 * or shrunk to the padding box, the content box or the margin box).
 */
static void
shape_box_radii(
	const struct css_style *style,
	const layout_unit border[4],
	const layout_unit padding[4],
	int reference,
	float border_width,
	float border_height,
	float *radius_x,
	float *radius_y)
{
	float inset_x;
	float inset_y;
	int index;

	for (index = 0; index < 4; index++) {
		radius_x[index] = shape_length(&style->radius[index][0], border_width);
		radius_y[index] = shape_length(&style->radius[index][1], border_height);
		if (radius_x[index] < 0.0f)
			radius_x[index] = 0.0f;
		if (radius_y[index] < 0.0f)
			radius_y[index] = 0.0f;
	}

	/* The padding box and the content box are inside: each corner loses the borders (and padding) at it. */
	if (reference == CSS_SHAPE_PADDING_BOX || reference == CSS_SHAPE_CONTENT_BOX) {
		for (index = 0; index < 4; index++) {
			/* The corner's two sides: top left is top and left; top right is top and right; and so on. */
			inset_y = layout_to_px(index < 2 ? border[CSS_TOP] : border[CSS_BOTTOM]);
			inset_x = layout_to_px(index == 0 || index == 3 ? border[CSS_LEFT] : border[CSS_RIGHT]);
			if (reference == CSS_SHAPE_CONTENT_BOX) {
				inset_y += layout_to_px(index < 2 ? padding[CSS_TOP] : padding[CSS_BOTTOM]);
				inset_x += layout_to_px(index == 0 || index == 3 ? padding[CSS_LEFT] : padding[CSS_RIGHT]);
			}
			radius_x[index] = radius_x[index] > inset_x ? radius_x[index] - inset_x : 0.0f;
			radius_y[index] = radius_y[index] > inset_y ? radius_y[index] - inset_y : 0.0f;
		}
	}
}

/* Works out where each stop of a gradient is along its line (0 to 1), as the painting does. */
static void
shape_gradient_positions(
	const struct css_gradient *gradient,
	float line,
	float *at)
{
	unsigned index;
	unsigned from;
	unsigned to;
	unsigned between;
	float highest;

	/* The positions given, as fractions of the line. */
	for (index = 0; index < gradient->count; index++) {
		at[index] = -1.0f;
		if (gradient->stops[index].unit == CSS_GRADIENT_AT_PX)
			at[index] = gradient->stops[index].position / line;
		if (gradient->stops[index].unit == CSS_GRADIENT_AT_PERCENT)
			at[index] = gradient->stops[index].position / 100.0f;
	}

	/* The first and the last default to the ends. */
	if (gradient->stops[0].unit == CSS_GRADIENT_AT_AUTO)
		at[0] = 0.0f;
	if (gradient->stops[gradient->count - 1U].unit == CSS_GRADIENT_AT_AUTO)
		at[gradient->count - 1U] = 1.0f;

	/* No stop is before the one before it. */
	highest = at[0];
	for (index = 1; index < gradient->count; index++) {
		if (gradient->stops[index].unit == CSS_GRADIENT_AT_AUTO)
			continue;
		if (at[index] < highest)
			at[index] = highest;
		highest = at[index];
	}

	/* A run of stops without positions is spread evenly between the ones around it. */
	from = 0;
	while (from + 1U < gradient->count) {
		to = from + 1U;
		while (to < gradient->count && gradient->stops[to].unit == CSS_GRADIENT_AT_AUTO && to != gradient->count - 1U)
			to++;
		for (between = from + 1U; between < to; between++)
			at[between] = at[from] + (at[to] - at[from]) * (float)(between - from) / (float)(to - from);
		from = to;
	}
}

/* Adds a stretch of the gradient line to the shape's opaque ones, joined to the one before when they meet. */
static void
shape_add_interval(
	struct layout_shape *shape,
	float from,
	float to)
{
	if (to < from)
		return;
	if (shape->intervals != 0 && from <= shape->to[shape->intervals - 1U] + 1.0e-6f) {
		if (to > shape->to[shape->intervals - 1U])
			shape->to[shape->intervals - 1U] = to;
		return;
	}
	if (shape->intervals >= SHAPE_INTERVALS)
		return;
	shape->from[shape->intervals] = from;
	shape->to[shape->intervals] = to;
	shape->intervals++;
}

/*
 * Makes a gradient's shape: the area where its alpha is above the
 * threshold.  The gradient is drawn in the margin box (physical width by
 * height); its line is kept as a linear function of the float's coordinates.
 */
static void
shape_build_gradient(
	struct layout_shape *shape,
	const struct css_gradient *gradient,
	float threshold,
	int mode,
	float width,
	float height)
{
	float at[CSS_GRADIENT_STOPS];
	float direction_x;
	float direction_y;
	float norm;
	float line;
	float scale_x;
	float scale_y;
	float offset;
	float radians;
	float alpha_low;
	float alpha_high;
	float cross;
	unsigned index;
	const float infinity = 1.0e9f;

	/* The direction of the line, as the painting takes it. */
	if (gradient->corner == 0) {
		radians = gradient->angle * 3.14159265f / 180.0f;
		direction_x = sinf(radians);
		direction_y = -cosf(radians);
	} else if ((gradient->corner & (CSS_GRADIENT_TOP | CSS_GRADIENT_BOTTOM)) == 0) {
		direction_x = (gradient->corner & CSS_GRADIENT_RIGHT) != 0 ? 1.0f : -1.0f;
		direction_y = 0.0f;
	} else if ((gradient->corner & (CSS_GRADIENT_LEFT | CSS_GRADIENT_RIGHT)) == 0) {
		direction_x = 0.0f;
		direction_y = (gradient->corner & CSS_GRADIENT_BOTTOM) != 0 ? 1.0f : -1.0f;
	} else {
		norm = sqrtf(width * width + height * height);
		direction_x = ((gradient->corner & CSS_GRADIENT_RIGHT) != 0 ? 1.0f : -1.0f) * height / norm;
		direction_y = ((gradient->corner & CSS_GRADIENT_BOTTOM) != 0 ? 1.0f : -1.0f) * width / norm;
	}
	line = fabsf(width * direction_x) + fabsf(height * direction_y);
	if (line <= 0.0f)
		line = 1.0f;

	/* The place along the line in physical coordinates: x * scale_x + y * scale_y + offset. */
	scale_x = direction_x / line;
	scale_y = direction_y / line;
	offset = 0.5f - (width / 2.0f * direction_x + height / 2.0f * direction_y) / line;

	/* In the float's coordinates. */
	if (mode == CSS_WRITING_VERTICAL_LR) {
		shape->tx = scale_y;
		shape->ty = scale_x;
		shape->t0 = offset;
	} else if (mode == CSS_WRITING_VERTICAL_RL) {
		shape->tx = scale_y;
		shape->ty = -scale_x;
		shape->t0 = offset + scale_x * width;
	} else if (mode == LAYOUT_SIDEWAYS_LR) {
		shape->tx = -scale_y;
		shape->ty = scale_x;
		shape->t0 = offset + scale_y * height;
	} else {
		shape->tx = scale_x;
		shape->ty = scale_y;
		shape->t0 = offset;
	}

	/* Before the first stop and after the last the color holds; between them the alpha changes along a straight line. */
	shape_gradient_positions(gradient, line, at);
	alpha_low = (float)(gradient->stops[0].color >> 24) / 255.0f;
	if (alpha_low > threshold)
		shape_add_interval(shape, -infinity, at[0]);
	for (index = 0; index + 1U < gradient->count; index++) {
		alpha_low = (float)(gradient->stops[index].color >> 24) / 255.0f;
		alpha_high = (float)(gradient->stops[index + 1U].color >> 24) / 255.0f;
		if (at[index + 1U] <= at[index])
			continue;
		if (alpha_low > threshold && alpha_high > threshold) {
			shape_add_interval(shape, at[index], at[index + 1U]);
		} else if (alpha_low > threshold || alpha_high > threshold) {
			/* The alpha crosses the threshold inside the segment. */
			cross = at[index] + (at[index + 1U] - at[index]) * (threshold - alpha_low) / (alpha_high - alpha_low);
			if (alpha_low > threshold)
				shape_add_interval(shape, at[index], cross);
			else
				shape_add_interval(shape, cross, at[index + 1U]);
		}
	}
	alpha_high = (float)(gradient->stops[gradient->count - 1U].color >> 24) / 255.0f;
	if (alpha_high > threshold)
		shape_add_interval(shape, at[gradient->count - 1U], infinity);
}

/*
 * Makes the shape of a float: NULL (no error) when its shape-outside is none
 * or the float has no area to wrap.  container_width is the containing
 * block's width, which shape-margin's percentage is of.
 */
int
layout_shape_build(
	struct layout_tree *tree,
	const struct layout_box *box,
	layout_unit container_width,
	struct layout_shape **result)
{
	const struct css_style *style;
	const struct css_shape *spec;
	struct layout_shape *shape;
	layout_unit margin_sides[4];
	layout_unit border_sides[4];
	layout_unit padding_sides[4];
	float margin[4];
	float border[4];
	float padding[4];
	float radius_x[4];
	float radius_y[4];
	float width;
	float height;
	float logical_width;
	float logical_height;
	float left;
	float top;
	float right;
	float bottom;
	float reference_width;
	float reference_height;
	float center_x;
	float center_y;
	float radius;
	float value_x;
	float value_y;
	float mapped_x;
	float mapped_y;
	unsigned index;
	int mode;
	int side;

	*result = NULL;
	style = box->physical_style != NULL ? box->physical_style : &box->style;
	spec = style->shape_outside;
	if (spec == NULL)
		return 0;

	/* The margin box in physical coordinates, and the sides of its parts. */
	mode = tree->logical_mode;
	layout_sides_physical(box->margin, mode, margin_sides);
	layout_sides_physical(box->border, mode, border_sides);
	layout_sides_physical(box->padding, mode, padding_sides);
	for (side = 0; side < 4; side++) {
		margin[side] = layout_to_px(margin_sides[side]);
		border[side] = layout_to_px(border_sides[side]);
		padding[side] = layout_to_px(padding_sides[side]);
	}
	logical_width = layout_to_px(box->margin[CSS_LEFT] + box->border[CSS_LEFT] + box->padding[CSS_LEFT] + box->width +
	    box->padding[CSS_RIGHT] + box->border[CSS_RIGHT] + box->margin[CSS_RIGHT]);
	logical_height = layout_to_px(box->margin[CSS_TOP] + box->border[CSS_TOP] + box->padding[CSS_TOP] + box->height +
	    box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM] + box->margin[CSS_BOTTOM]);
	width = logical_width;
	height = logical_height;
	if (mode != CSS_WRITING_HORIZONTAL_TB) {
		width = logical_height;
		height = logical_width;
	}
	if (width <= 0.0f || height <= 0.0f)
		return 0;

	/* The reference box: the margin box unless a shape box names another. */
	left = 0.0f;
	top = 0.0f;
	right = width;
	bottom = height;
	if (spec->box != CSS_SHAPE_MARGIN_BOX) {
		left = margin[CSS_LEFT];
		top = margin[CSS_TOP];
		right = width - margin[CSS_RIGHT];
		bottom = height - margin[CSS_BOTTOM];
	}
	if (spec->box == CSS_SHAPE_PADDING_BOX || spec->box == CSS_SHAPE_CONTENT_BOX) {
		left += border[CSS_LEFT];
		top += border[CSS_TOP];
		right -= border[CSS_RIGHT];
		bottom -= border[CSS_BOTTOM];
	}
	if (spec->box == CSS_SHAPE_CONTENT_BOX) {
		left += padding[CSS_LEFT];
		top += padding[CSS_TOP];
		right -= padding[CSS_RIGHT];
		bottom -= padding[CSS_BOTTOM];
	}
	reference_width = right - left;
	reference_height = bottom - top;
	if (reference_width < 0.0f)
		reference_width = 0.0f;
	if (reference_height < 0.0f)
		reference_height = 0.0f;

	/* The shape, in the tree's arena. */
	shape = wb_arena_zalloc(&tree->arena, sizeof(*shape));
	if (shape == NULL)
		return ENOMEM;
	shape->width = logical_width;
	shape->height = logical_height;
	shape->margin = shape_length(&style->shape_margin, layout_to_px(container_width));
	if (shape->margin < 0.0f)
		shape->margin = 0.0f;

	switch (spec->kind) {
	case CSS_SHAPE_CIRCLE:
	case CSS_SHAPE_ELLIPSE:
		/* The center, from the start side or the end side of each axis. */
		center_x = shape_length(&spec->at[0], reference_width);
		center_y = shape_length(&spec->at[1], reference_height);
		center_x = spec->at_end[0] ? right - center_x : left + center_x;
		center_y = spec->at_end[1] ? bottom - center_y : top + center_y;

		/* The radii: a length, or the distance to the nearest or the farthest side. */
		if (spec->kind == CSS_SHAPE_CIRCLE) {
			value_x = fminf(fminf(fabsf(center_x - left), fabsf(right - center_x)), fminf(fabsf(center_y - top), fabsf(bottom - center_y)));
			value_y = fmaxf(fmaxf(fabsf(center_x - left), fabsf(right - center_x)), fmaxf(fabsf(center_y - top), fabsf(bottom - center_y)));
			if (spec->radius_keyword[0] == CSS_SHAPE_CLOSEST)
				radius = value_x;
			else if (spec->radius_keyword[0] == CSS_SHAPE_FARTHEST)
				radius = value_y;
			else
				radius = shape_length(&spec->radius[0], sqrtf((reference_width * reference_width + reference_height * reference_height) / 2.0f));
			radius_x[0] = radius;
			radius_y[0] = radius;
		} else {
			if (spec->radius_keyword[0] == CSS_SHAPE_CLOSEST)
				radius_x[0] = fminf(fabsf(center_x - left), fabsf(right - center_x));
			else if (spec->radius_keyword[0] == CSS_SHAPE_FARTHEST)
				radius_x[0] = fmaxf(fabsf(center_x - left), fabsf(right - center_x));
			else
				radius_x[0] = shape_length(&spec->radius[0], reference_width);
			if (spec->radius_keyword[1] == CSS_SHAPE_CLOSEST)
				radius_y[0] = fminf(fabsf(center_y - top), fabsf(bottom - center_y));
			else if (spec->radius_keyword[1] == CSS_SHAPE_FARTHEST)
				radius_y[0] = fmaxf(fabsf(center_y - top), fabsf(bottom - center_y));
			else
				radius_y[0] = shape_length(&spec->radius[1], reference_height);
		}
		shape->kind = SHAPE_ELLIPSE;
		shape_point(mode, width, height, center_x, center_y, &shape->cx, &shape->cy);
		shape->rx = mode == CSS_WRITING_HORIZONTAL_TB ? radius_x[0] : radius_y[0];
		shape->ry = mode == CSS_WRITING_HORIZONTAL_TB ? radius_y[0] : radius_x[0];
		break;
	case CSS_SHAPE_INSET:
		/* The edges moved in from the reference box, with the rounded corners. */
		value_x = left + shape_length(&spec->inset[CSS_LEFT], reference_width);
		value_y = top + shape_length(&spec->inset[CSS_TOP], reference_height);
		center_x = right - shape_length(&spec->inset[CSS_RIGHT], reference_width);
		center_y = bottom - shape_length(&spec->inset[CSS_BOTTOM], reference_height);
		if (center_x < value_x)
			center_x = value_x;
		if (center_y < value_y)
			center_y = value_y;
		memset(radius_x, 0, sizeof(radius_x));
		memset(radius_y, 0, sizeof(radius_y));
		if (spec->rounded) {
			for (index = 0; index < 4U; index++) {
				radius_x[index] = shape_length(&spec->round[index][0], center_x - value_x);
				radius_y[index] = shape_length(&spec->round[index][1], center_y - value_y);
			}
			shape_clamp_radii(center_x - value_x, center_y - value_y, radius_x, radius_y);
		}
		shape->kind = SHAPE_ROUNDED;
		shape_set_rounded(shape, mode, width, height, value_x, value_y, center_x, center_y, radius_x, radius_y);
		break;
	case CSS_SHAPE_BOX:
		/* The box itself with its rounded corners. */
		shape_box_radii(style, border_sides, padding_sides, spec->box, width - margin[CSS_LEFT] - margin[CSS_RIGHT],
		    height - margin[CSS_TOP] - margin[CSS_BOTTOM], radius_x, radius_y);
		shape_clamp_radii(reference_width, reference_height, radius_x, radius_y);
		shape->kind = SHAPE_ROUNDED;
		shape_set_rounded(shape, mode, width, height, left, top, right, bottom, radius_x, radius_y);
		break;
	case CSS_SHAPE_POLYGON:
		shape->kind = SHAPE_POLYGON;
		shape->count = spec->point_count;
		for (index = 0; index < spec->point_count; index++) {
			value_x = left + shape_length(&spec->points[index][0], reference_width);
			value_y = top + shape_length(&spec->points[index][1], reference_height);
			shape_point(mode, width, height, value_x, value_y, &mapped_x, &mapped_y);
			shape->point_x[index] = mapped_x;
			shape->point_y[index] = mapped_y;
		}
		break;
	case CSS_SHAPE_GRADIENT:
		if (spec->gradient == NULL || spec->gradient->count == 0)
			return 0;
		shape->kind = SHAPE_ALPHA;
		shape->x0 = 0.0f;
		shape->y0 = 0.0f;
		shape->x1 = logical_width;
		shape->y1 = logical_height;
		shape_build_gradient(shape, spec->gradient, style->shape_threshold, mode, width, height);
		break;
	default:
		return 0;
	}

	/* Succeeded: the shape is made. */
	*result = shape;
	return 0;
}

/* Finds the horizontal extent of the shape on the row at y (without shape-margin); 0 when the row is off the shape. */
static int
shape_row(
	const struct layout_shape *shape,
	float y,
	float *left,
	float *right)
{
	float low;
	float high;
	float edge;
	float distance;
	float inset_left;
	float inset_right;
	float x;
	float next_x;
	float from_y;
	float to_y;
	float step;
	unsigned index;
	unsigned next;
	unsigned interval;
	int found;

	if (shape->kind == SHAPE_ELLIPSE) {
		if (shape->rx <= 0.0f || shape->ry <= 0.0f)
			return 0;
		distance = (y - shape->cy) / shape->ry;
		if (distance < -1.0f || distance > 1.0f)
			return 0;
		edge = shape->rx * sqrtf(1.0f - distance * distance);
		*left = shape->cx - edge;
		*right = shape->cx + edge;
		return 1;
	}
	if (shape->kind == SHAPE_ROUNDED) {
		if (y < shape->y0 || y > shape->y1 || shape->x1 <= shape->x0 || shape->y1 <= shape->y0)
			return 0;
		inset_left = 0.0f;
		inset_right = 0.0f;
		for (index = 0; index < 4U; index++) {
			if (shape->corner_y[index] <= 0.0f || shape->corner_x[index] <= 0.0f)
				continue;
			if (index < 2U) {
				if (y >= shape->y0 + shape->corner_y[index])
					continue;
				distance = (shape->y0 + shape->corner_y[index] - y) / shape->corner_y[index];
			} else {
				if (y <= shape->y1 - shape->corner_y[index])
					continue;
				distance = (y - (shape->y1 - shape->corner_y[index])) / shape->corner_y[index];
			}
			edge = shape->corner_x[index] * (1.0f - sqrtf(fmaxf(0.0f, 1.0f - distance * distance)));
			if (index == 0 || index == 3U) {
				if (edge > inset_left)
					inset_left = edge;
			} else if (edge > inset_right) {
				inset_right = edge;
			}
		}
		*left = shape->x0 + inset_left;
		*right = shape->x1 - inset_right;
		return *right >= *left;
	}
	if (shape->kind == SHAPE_POLYGON) {
		found = 0;
		low = 0.0f;
		high = 0.0f;
		for (index = 0; index < shape->count; index++) {
			next = index + 1U < shape->count ? index + 1U : 0;
			from_y = shape->point_y[index];
			to_y = shape->point_y[next];
			x = shape->point_x[index];
			next_x = shape->point_x[next];
			if (from_y == to_y) {
				if (y != from_y)
					continue;
				edge = fminf(x, next_x);
				distance = fmaxf(x, next_x);
			} else {
				if (y < fminf(from_y, to_y) || y > fmaxf(from_y, to_y))
					continue;
				step = (y - from_y) / (to_y - from_y);
				edge = x + (next_x - x) * step;
				distance = edge;
			}
			if (!found || edge < low)
				low = edge;
			if (!found || distance > high)
				high = distance;
			found = 1;
		}
		*left = low;
		*right = high;
		return found;
	}
	if (shape->kind == SHAPE_ALPHA) {
		if (y < shape->y0 || y > shape->y1)
			return 0;
		found = 0;
		low = 0.0f;
		high = 0.0f;
		for (interval = 0; interval < shape->intervals; interval++) {
			if (fabsf(shape->tx) < 1.0e-6f) {
				edge = shape->ty * y + shape->t0;
				if (edge < shape->from[interval] || edge > shape->to[interval])
					continue;
				x = shape->x0;
				next_x = shape->x1;
			} else {
				x = (shape->from[interval] - shape->ty * y - shape->t0) / shape->tx;
				next_x = (shape->to[interval] - shape->ty * y - shape->t0) / shape->tx;
				if (next_x < x) {
					edge = x;
					x = next_x;
					next_x = edge;
				}
				if (x < shape->x0)
					x = shape->x0;
				if (next_x > shape->x1)
					next_x = shape->x1;
				if (next_x < x)
					continue;
			}
			if (!found || x < low)
				low = x;
			if (!found || next_x > high)
				high = next_x;
			found = 1;
		}
		*left = low;
		*right = high;
		return found;
	}
	return 0;
}

/*
 * Finds how far the shape reaches into a band of the float's margin box
 * (top to bottom, in layout units from its top): the left-most and the
 * right-most place of the shape, grown by shape-margin, clipped to the
 * margin box, from its left edge.  Returns 0 when the shape does not reach
 * into the band.
 */
int
layout_shape_extent(
	const struct layout_shape *shape,
	layout_unit top,
	layout_unit bottom,
	layout_unit *left,
	layout_unit *right)
{
	float band_top;
	float band_bottom;
	float row_left;
	float row_right;
	float low;
	float high;
	float distance;
	float reach;
	float row;
	float start;
	float span;
	int count;
	int index;
	int found;
	int inside;

	/* The band, as a thin one when it has no height. */
	band_top = layout_to_px(top);
	band_bottom = layout_to_px(bottom);
	if (band_bottom <= band_top)
		band_bottom = band_top + 1.0f / (float)LAYOUT_UNIT;

	/* Rows from the band grown by the margin, each reaching as far as the margin allows from its distance to the band. */
	start = band_top - shape->margin;
	span = band_bottom - band_top + 2.0f * shape->margin;
	count = (int)ceilf(span * (float)SHAPE_ROWS_PER_PIXEL);
	if (count < 1)
		count = 1;
	if (count > 4096)
		count = 4096;
	found = 0;
	low = 0.0f;
	high = 0.0f;
	for (index = 0; index <= count; index++) {
		/* The rows at the band's edges are taken a little inside it: a shape that only touches the edge is not in the band. */
		row = start + span * (float)index / (float)count;
		if (index == 0)
			row += SHAPE_EDGE;
		else if (index == count)
			row -= SHAPE_EDGE;
		inside = shape_row(shape, row, &row_left, &row_right);
		if (!inside)
			continue;

		/* How far the row is from the band (zero inside it), and how far the margin reaches sideways from there. */
		distance = 0.0f;
		if (row < band_top)
			distance = band_top - row;
		else if (row > band_bottom)
			distance = row - band_bottom;
		reach = 0.0f;
		if (shape->margin > 0.0f) {
			if (distance > shape->margin)
				continue;
			reach = sqrtf(shape->margin * shape->margin - distance * distance);
		}
		if (!found || row_left - reach < low)
			low = row_left - reach;
		if (!found || row_right + reach > high)
			high = row_right + reach;
		found = 1;
	}
	if (!found)
		return 0;

	/* The float's area is the shape within its margin box. */
	if (low < 0.0f)
		low = 0.0f;
	if (high > shape->width)
		high = shape->width;
	if (high < low)
		return 0;

	/* In layout units. */
	*left = layout_from_px(low);
	*right = layout_from_px(high);
	return 1;
}
