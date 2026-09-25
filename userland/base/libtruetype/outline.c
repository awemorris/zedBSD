/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Reading one glyph's outline out of glyf, and its advance out of hmtx.
 *
 * A glyph is a set of closed contours of points.  A point is either on the
 * curve or a control point of a quadratic curve; two control points in a row
 * imply an on-curve point halfway between them, which is how the format
 * saves space.  The curves are turned into line segments here, because the
 * rasterizer works on segments and the difference is invisible at the sizes
 * a screen uses.
 *
 * A composite glyph is built from other glyphs placed at offsets, which is
 * how accented letters are made.  Nesting is bounded so that a font cannot
 * ask this to recurse forever.
 */

#include "internal.h"

#include <errno.h>
#include <math.h>

/* The flags a simple glyph's points carry. */
#define ON_CURVE	0x01U
#define X_SHORT		0x02U
#define Y_SHORT		0x04U
#define REPEAT		0x08U
#define X_SAME		0x10U
#define Y_SAME		0x20U

/* The flags a composite glyph's components carry. */
#define ARG_WORDS	0x0001U
#define ARGS_XY		0x0002U
#define HAVE_SCALE	0x0008U
#define MORE_COMPONENTS	0x0020U
#define HAVE_XY_SCALE	0x0040U
#define HAVE_MATRIX	0x0080U

static int glyph_range(const struct truetype_face *face, unsigned glyph,
		       uint32_t *offset, uint32_t *length);
static int load_simple(struct truetype_face *face, const uint8_t *glyf,
		       uint32_t length, struct truetype_outline *outline);
static int load_composite(struct truetype_face *face, const uint8_t *glyf,
			  uint32_t length, struct truetype_outline *outline,
			  unsigned depth);
static void emit_quadratic(struct truetype_outline *outline,
			   struct truetype_point start,
			   struct truetype_point control,
			   struct truetype_point end);

/*
 * Returns the scale between font units and pixels.
 */
float
truetype_scale(
	const struct truetype_face *face)
{
	return (float)face->pixels / (float)face->units_per_em;
}

int
truetype_scale_up(
	const struct truetype_face *face,
	int value)
{
	return (int)ceilf((float)value * truetype_scale(face));
}

int
truetype_scale_down(
	const struct truetype_face *face,
	int value)
{
	return (int)floorf((float)value * truetype_scale(face));
}

/*
 * Finds where one glyph lies in glyf, using the loca index.
 *
 * A zero-length entry is a glyph with no outline, which is what a space is.
 */
static int
glyph_range(
	const struct truetype_face *face,
	unsigned glyph,
	uint32_t *offset,
	uint32_t *length)
{
	uint32_t start, end;

	/* Handles a glyph this font does not have. */
	if (glyph >= face->glyph_count)
		return EINVAL;

	/* Handles the selected index format. */
	if (face->long_loca) {
		/* Handles a loca table too short for this glyph. */
		if ((uint32_t)(glyph + 1U) * 4U + 4U > face->loca_size)
			return EINVAL;
		start = truetype_u32(face->loca + (size_t)glyph * 4U);
		end = truetype_u32(face->loca + (size_t)(glyph + 1U) * 4U);
	} else {
		/* Handles a loca table too short for this glyph. */
		if ((uint32_t)(glyph + 1U) * 2U + 2U > face->loca_size)
			return EINVAL;

		/* The short format stores half the offset. */
		start = (uint32_t)truetype_u16(
			face->loca + (size_t)glyph * 2U) * 2U;
		end = (uint32_t)truetype_u16(
			face->loca + (size_t)(glyph + 1U) * 2U) * 2U;
	}

	/* Handles an entry that names a range outside glyf. */
	if (end < start || end > face->glyf_size)
		return EINVAL;
	*offset = start;
	*length = end - start;

	/* Succeeded. */
	return 0;
}

/*
 * Implements the truetype advance operation.
 *
 * hmtx holds one entry per glyph up to hmetric_count, after which every
 * glyph keeps the last advance: a monospaced tail is stored once.
 */
int
truetype_advance(
	const struct truetype_face *face,
	unsigned glyph,
	int *advance)
{
	unsigned index;

	/* Handles a font with no advance widths at all. */
	if (face->hmetric_count == 0)
		return EINVAL;
	index = glyph < face->hmetric_count ?
		glyph : (unsigned)(face->hmetric_count - 1U);

	/* Handles an entry the table does not hold. */
	if ((uint32_t)index * 4U + 2U > face->hmtx_size)
		return EINVAL;
	*advance = truetype_scale_up(
		face, (int)truetype_u16(face->hmtx + (size_t)index * 4U));

	/* Succeeded. */
	return 0;
}

/*
 * Turns one quadratic curve into line segments.
 *
 * The number of segments follows how far the control point sits from the
 * chord: a nearly straight curve needs one, a tight one needs more.  At the
 * sizes a screen uses this is never many.
 */
static void
emit_quadratic(
	struct truetype_outline *outline,
	struct truetype_point start,
	struct truetype_point control,
	struct truetype_point end)
{
	float deviation, x, y, t, u;
	unsigned steps, step;

	deviation = fabsf(control.x - (start.x + end.x) * 0.5f) +
		    fabsf(control.y - (start.y + end.y) * 0.5f);
	steps = (unsigned)(deviation * 0.5f) + 2U;

	/* Handles a curve that would cost more segments than it is worth. */
	if (steps > 16U)
		steps = 16U;

	/* Process each segment of the curve. */
	for (step = 1; step <= steps; step++) {
		/* Stops rather than overrunning the point array. */
		if (outline->point_count >= TRUETYPE_POINTS_MAX)
			return;
		t = (float)step / (float)steps;
		u = 1.0f - t;
		x = u * u * start.x + 2.0f * u * t * control.x + t * t * end.x;
		y = u * u * start.y + 2.0f * u * t * control.y + t * t * end.y;
		outline->points[outline->point_count].x = x;
		outline->points[outline->point_count].y = y;
		outline->points[outline->point_count].on_curve = 1U;
		outline->point_count++;
	}
}

/*
 * Reads a simple glyph: its contours, flags and coordinates.
 */
static int
load_simple(
	struct truetype_face *face,
	const uint8_t *glyf,
	uint32_t length,
	struct truetype_outline *outline)
{
	struct truetype_point raw[TRUETYPE_POINTS_MAX];
	struct truetype_point start, previous, current, implied;
	uint8_t flags[TRUETYPE_POINTS_MAX];
	uint32_t position, instructions;
	unsigned contours, count, index, first, last, repeat, contour;
	unsigned started, have_control;
	float scale, x, y;
	int value;

	contours = truetype_u16(glyf);

	/* Handles a glyph with more contours than this holds. */
	if (contours > TRUETYPE_CONTOURS_MAX)
		return ENOTSUP;

	/* Handles a header the entry does not hold. */
	if (length < 10U + (uint32_t)contours * 2U + 2U)
		return EINVAL;
	count = truetype_u16(glyf + 10U + (size_t)(contours - 1U) * 2U) + 1U;

	/* Handles a glyph with more points than this holds. */
	if (count > TRUETYPE_POINTS_MAX)
		return ENOTSUP;
	position = 10U + (uint32_t)contours * 2U;
	instructions = truetype_u16(glyf + position);
	position += 2U;

	/* Handles instructions the entry does not hold. */
	if (instructions > length - position)
		return EINVAL;
	position += instructions;
	index = 0;

	/* Process each flag, which may repeat. */
	while (index < count) {
		/* Handles a flag the entry does not hold. */
		if (position >= length)
			return EINVAL;
		flags[index] = glyf[position];
		position++;
		index++;

		/* Handles a flag that is not repeated. */
		if ((flags[index - 1U] & REPEAT) == 0)
			continue;

		/* Handles a repeat count the entry does not hold. */
		if (position >= length)
			return EINVAL;
		repeat = glyf[position];
		position++;

		/* Process each repetition. */
		while (repeat > 0 && index < count) {
			flags[index] = flags[index - 1U];
			index++;
			repeat--;
		}
	}
	value = 0;

	/* Process each x coordinate, which is stored as a difference. */
	for (index = 0; index < count; index++) {
		/* Handles the short form, whose sign is in another flag. */
		if ((flags[index] & X_SHORT) != 0) {
			/* Handles a coordinate the entry does not hold. */
			if (position >= length)
				return EINVAL;
			value += (flags[index] & X_SAME) != 0 ?
				(int)glyf[position] : -(int)glyf[position];
			position++;
		} else if ((flags[index] & X_SAME) == 0) {
			/* Handles a coordinate the entry does not hold. */
			if (position + 2U > length)
				return EINVAL;
			value += truetype_s16(glyf + position);
			position += 2U;
		}
		raw[index].x = (float)value;
	}
	value = 0;

	/* Process each y coordinate. */
	for (index = 0; index < count; index++) {
		/* Handles the short form, whose sign is in another flag. */
		if ((flags[index] & Y_SHORT) != 0) {
			/* Handles a coordinate the entry does not hold. */
			if (position >= length)
				return EINVAL;
			value += (flags[index] & Y_SAME) != 0 ?
				(int)glyf[position] : -(int)glyf[position];
			position++;
		} else if ((flags[index] & Y_SAME) == 0) {
			/* Handles a coordinate the entry does not hold. */
			if (position + 2U > length)
				return EINVAL;
			value += truetype_s16(glyf + position);
			position += 2U;
		}
		raw[index].y = (float)value;
		raw[index].on_curve = (flags[index] & ON_CURVE) != 0;
	}
	scale = truetype_scale(face);

	/* The outline is kept in pixels, with y growing upward as the font has it. */
	for (index = 0; index < count; index++) {
		raw[index].x *= scale;
		raw[index].y *= scale;
	}
	first = 0;

	/* Process each contour, turning its curves into segments. */
	for (contour = 0; contour < contours; contour++) {
		last = truetype_u16(glyf + 10U + (size_t)contour * 2U);

		/* Handles an end point outside the point list. */
		if (last >= count || last < first)
			return EINVAL;

		/* Skips a contour with nothing in it. */
		if (last < first) {
			first = last + 1U;
			continue;
		}

		/*
		 * A contour may begin on a control point, in which case the
		 * start is the midpoint between it and the last point, or
		 * the last point itself when that one is on the curve.
		 */
		if (raw[first].on_curve) {
			start = raw[first];
			index = first + 1U;
		} else if (raw[last].on_curve) {
			start = raw[last];
			index = first;
		} else {
			start.x = (raw[first].x + raw[last].x) * 0.5f;
			start.y = (raw[first].y + raw[last].y) * 0.5f;
			start.on_curve = 1U;
			index = first;
		}

		/* Stops rather than overrunning the point array. */
		if (outline->point_count >= TRUETYPE_POINTS_MAX)
			return ENOSPC;
		outline->points[outline->point_count] = start;
		outline->point_count++;
		previous = start;
		have_control = 0;
		started = 0;
		x = 0.0f;
		y = 0.0f;

		/* Process each point of the contour, wrapping to the start. */
		while (started <= last - first) {
			current = raw[first + (index - first) %
					 (last - first + 1U)];
			index++;
			started++;

			/* Handles an on-curve point, which ends any curve. */
			if (current.on_curve) {
				/* Handles the curve waiting for its end. */
				if (have_control) {
					implied.x = x;
					implied.y = y;
					implied.on_curve = 0U;
					emit_quadratic(outline, previous,
						       implied, current);
					have_control = 0;
				} else {
					/* Stops rather than overrunning. */
					if (outline->point_count >=
					    TRUETYPE_POINTS_MAX)
						return ENOSPC;
					outline->points[outline->point_count] =
						current;
					outline->point_count++;
				}
				previous = current;
				continue;
			}

			/*
			 * Two control points in a row imply an on-curve point
			 * halfway between them, which ends the first curve.
			 */
			if (have_control) {
				implied.x = (x + current.x) * 0.5f;
				implied.y = (y + current.y) * 0.5f;
				implied.on_curve = 1U;
				current.on_curve = 0U;
				previous.on_curve = 1U;
				emit_quadratic(outline, previous,
					       (struct truetype_point){
						       x, y, 0U}, implied);
				previous = implied;
			}
			x = current.x;
			y = current.y;
			have_control = 1U;
		}

		/* Closes the contour back to where it started. */
		if (have_control) {
			emit_quadratic(outline, previous,
				       (struct truetype_point){x, y, 0U},
				       start);
		} else if (outline->point_count < TRUETYPE_POINTS_MAX) {
			outline->points[outline->point_count] = start;
			outline->point_count++;
		}

		/* Stops rather than overrunning the contour array. */
		if (outline->contour_count >= TRUETYPE_CONTOURS_MAX)
			return ENOSPC;
		outline->ends[outline->contour_count] = outline->point_count;
		outline->contour_count++;
		first = last + 1U;
	}

	/* Succeeded. */
	return 0;
}

/*
 * Reads a composite glyph: other glyphs placed at offsets.
 */
static int
load_composite(
	struct truetype_face *face,
	const uint8_t *glyf,
	uint32_t length,
	struct truetype_outline *outline,
	unsigned depth)
{
	uint32_t position;
	unsigned flags, component, first, index;
	float offset_x, offset_y, scale;
	int error;

	/* Handles a font that nests components deeper than this follows. */
	if (depth >= TRUETYPE_COMPOSITE_DEPTH)
		return ELOOP;
	position = 10U;
	scale = truetype_scale(face);

	/* Process each component. */
	do {
		/* Handles a component header the entry does not hold. */
		if (position + 4U > length)
			return EINVAL;
		flags = truetype_u16(glyf + position);
		component = truetype_u16(glyf + position + 2U);
		position += 4U;

		/* Handles arguments the entry does not hold. */
		if (position + ((flags & ARG_WORDS) != 0 ? 4U : 2U) > length)
			return EINVAL;

		/*
		 * The arguments are an offset when ARGS_XY is set, and point
		 * numbers to align otherwise.  Point alignment is rare and is
		 * not followed here; the component is placed unmoved.
		 */
		if ((flags & ARG_WORDS) != 0) {
			offset_x = (float)truetype_s16(glyf + position);
			offset_y = (float)truetype_s16(glyf + position + 2U);
			position += 4U;
		} else {
			offset_x = (float)(int8_t)glyf[position];
			offset_y = (float)(int8_t)glyf[position + 1U];
			position += 2U;
		}

		/* Handles alignment by point number, which is not followed. */
		if ((flags & ARGS_XY) == 0) {
			offset_x = 0.0f;
			offset_y = 0.0f;
		}

		/*
		 * A component may carry a scale or a full matrix.  Those are
		 * skipped: the shapes this draws are upright text, and a
		 * transformed component is placed without its transform
		 * rather than refusing the whole glyph.
		 */
		if ((flags & HAVE_SCALE) != 0)
			position += 2U;
		else if ((flags & HAVE_XY_SCALE) != 0)
			position += 4U;
		else if ((flags & HAVE_MATRIX) != 0)
			position += 8U;

		/* Handles a transform the entry does not hold. */
		if (position > length)
			return EINVAL;
		first = outline->point_count;
		error = truetype_outline_load(face, component, outline,
					      depth + 1U);

		/* Reports the failure. */
		if (error != 0)
			return error;

		/* Moves the points this component added into place. */
		for (index = first; index < outline->point_count; index++) {
			outline->points[index].x += offset_x * scale;
			outline->points[index].y += offset_y * scale;
		}
	} while ((flags & MORE_COMPONENTS) != 0);

	/* Succeeded. */
	return 0;
}

/*
 * Implements the truetype outline load operation.
 */
int
truetype_outline_load(
	struct truetype_face *face,
	unsigned glyph,
	struct truetype_outline *outline,
	unsigned depth)
{
	const uint8_t *entry;
	uint32_t offset, length;
	int contours, error;

	error = glyph_range(face, glyph, &offset, &length);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Handles a glyph with no outline, which is what a space is. */
	if (length == 0)
		return 0;

	/* Handles an entry too short to hold its own header. */
	if (length < 10U)
		return EINVAL;
	entry = face->glyf + offset;
	contours = truetype_s16(entry);

	/* Handles the selected glyph form. */
	if (contours >= 0)
		return load_simple(face, entry, length, outline);

	/* Returns the computed result. */
	return load_composite(face, entry, length, outline, depth);
}

/*
 * Implements the truetype outline bounds operation.
 */
void
truetype_outline_bounds(
	const struct truetype_outline *outline,
	float *minimum_x,
	float *minimum_y,
	float *maximum_x,
	float *maximum_y)
{
	unsigned index;

	/* Handles an outline with nothing in it. */
	if (outline->point_count == 0) {
		*minimum_x = 0.0f;
		*minimum_y = 0.0f;
		*maximum_x = 0.0f;
		*maximum_y = 0.0f;
		return;
	}
	*minimum_x = outline->points[0].x;
	*maximum_x = outline->points[0].x;
	*minimum_y = outline->points[0].y;
	*maximum_y = outline->points[0].y;

	/* Process each point. */
	for (index = 1; index < outline->point_count; index++) {
		/* Handles a point further left than any before it. */
		if (outline->points[index].x < *minimum_x)
			*minimum_x = outline->points[index].x;

		/* Handles a point further right than any before it. */
		if (outline->points[index].x > *maximum_x)
			*maximum_x = outline->points[index].x;

		/* Handles a point lower than any before it. */
		if (outline->points[index].y < *minimum_y)
			*minimum_y = outline->points[index].y;

		/* Handles a point higher than any before it. */
		if (outline->points[index].y > *maximum_y)
			*maximum_y = outline->points[index].y;
	}
}
