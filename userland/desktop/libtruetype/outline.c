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
#include <stdlib.h>

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

/*
 * One vertex of a contour being made bold (truetype_outline_embolden): the
 * direction and length of the edge that leaves it, and how far it moves.
 */
struct embolden_vertex {
	float direction_x;
	float direction_y;
	float length;
	float shift_x;
	float shift_y;
};

static int load_simple(struct truetype_face *face, const uint8_t *glyf, uint32_t length, struct truetype_outline *outline);
static int read_flags(const uint8_t *glyf, uint32_t length, unsigned count, uint32_t *position, uint8_t *flags);
static int read_coordinates(const uint8_t *glyf, uint32_t length, unsigned count, const uint8_t *flags, uint8_t short_flag, uint8_t same_flag, uint32_t *position, float *values);
static int trace_contour(struct truetype_outline *outline, const struct truetype_point *raw, unsigned first, unsigned last);
static int load_composite(struct truetype_face *face, const uint8_t *glyf, uint32_t length, struct truetype_outline *outline, unsigned depth);
static void emit_quadratic(struct truetype_outline *outline, struct truetype_point start, struct truetype_point control, struct truetype_point end);

/*
 * Reports the scale between font units and pixels.
 */
float
truetype_scale(
	const struct truetype_face *face)
{
	/* The pixels of an em over the units of an em. */
	return (float)face->pixels / (float)face->units_per_em;
}

/*
 * Scales a measurement in font units to whole pixels, rounding up.
 */
int
truetype_scale_up(
	const struct truetype_face *face,
	int value)
{
	float scale;
	float scaled;

	/* The measurement in pixels, and the whole pixel at or above it. */
	scale = truetype_scale(face);
	scaled = ceilf((float)value * scale);

	/* Reports the whole pixels. */
	return (int)scaled;
}

/*
 * Scales a measurement in font units to whole pixels, rounding down.
 */
int
truetype_scale_down(
	const struct truetype_face *face,
	int value)
{
	float scale;
	float scaled;

	/* The measurement in pixels, and the whole pixel at or below it. */
	scale = truetype_scale(face);
	scaled = floorf((float)value * scale);

	/* Reports the whole pixels. */
	return (int)scaled;
}

/*
 * Finds where one glyph lies in glyf, using the loca index.
 *
 * A zero-length entry is a glyph with no outline, which is what a space is.
 */
int
truetype_glyph_range(
	const struct truetype_face *face,
	unsigned glyph,
	uint32_t *offset,
	uint32_t *length)
{
	uint32_t start;
	uint32_t end;

	/* Refuses a glyph this font does not have. */
	if (glyph >= face->glyph_count)
		return EINVAL;

	/* Reads the glyph's start and the next glyph's, by the index's format. */
	if (face->long_loca) {
		/* Refuses a loca table too short for this glyph. */
		if ((uint32_t)(glyph + 1U) * 4U + 4U > face->loca_size)
			return EINVAL;

		/* The long format stores the offsets as they are. */
		start = truetype_u32(face->loca + (size_t)glyph * 4U);
		end = truetype_u32(face->loca + (size_t)(glyph + 1U) * 4U);
	} else {
		/* Refuses a loca table too short for this glyph. */
		if ((uint32_t)(glyph + 1U) * 2U + 2U > face->loca_size)
			return EINVAL;

		/* The short format stores half the offset. */
		start = (uint32_t)truetype_u16(face->loca + (size_t)glyph * 2U) * 2U;
		end = (uint32_t)truetype_u16(face->loca + (size_t)(glyph + 1U) * 2U) * 2U;
	}

	/* Refuses an entry that names a range outside glyf. */
	if (end < start || end > face->glyf_size)
		return EINVAL;

	/* Succeeded: the glyph's place and length in glyf. */
	*offset = start;
	*length = end - start;
	return 0;
}

/*
 * Reports a glyph's advance width in whole pixels.
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
	int units;

	/* Refuses a font with no advance widths at all. */
	if (face->hmetric_count == 0)
		return EINVAL;

	/* The glyph's own entry, or the last one for a glyph of the tail. */
	index = glyph;
	if (glyph >= face->hmetric_count)
		index = (unsigned)(face->hmetric_count - 1U);

	/* Refuses an entry the table does not hold. */
	if ((uint32_t)index * 4U + 2U > face->hmtx_size)
		return EINVAL;

	/* Succeeded: the advance in font units, rounded up to pixels. */
	units = (int)truetype_u16(face->hmtx + (size_t)index * 4U);
	*advance = truetype_scale_up(face, units);
	return 0;
}

/*
 * Adds one glyph's outline, in pixels, to an outline: a simple glyph's
 * contours, or a composite glyph's components (depth counts the nesting).
 *
 * Returns 0 (a glyph without an outline adds nothing), EINVAL for a glyph
 * the font does not hold whole, ENOTSUP for more contours or points than
 * an outline holds, ENOSPC when the outline fills up, or ELOOP for
 * components nested too deep.
 */
int
truetype_outline_load(
	struct truetype_face *face,
	unsigned glyph,
	struct truetype_outline *outline,
	unsigned depth)
{
	const uint8_t *entry;
	uint32_t offset;
	uint32_t length;
	int contours;
	int error;

	/* Finds the glyph in glyf. */
	error = truetype_glyph_range(face, glyph, &offset, &length);
	if (error != 0)
		return error;

	/* A glyph with no outline, which is what a space is, adds nothing. */
	if (length == 0)
		return 0;

	/* Refuses an entry too short to hold its own header. */
	if (length < 10U)
		return EINVAL;
	entry = face->glyf + offset;
	contours = truetype_s16(entry);

	/* A count of contours makes a simple glyph, a negative one a composite. */
	if (contours >= 0) {
		error = load_simple(face, entry, length, outline);
	} else {
		error = load_composite(face, entry, length, outline, depth);
	}

	/* Reports why the glyph could not be added. */
	if (error != 0)
		return error;

	/* Succeeded: the glyph's outline is added. */
	return 0;
}

/*
 * Reports the box around an outline's points (all zero for an empty one).
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

	/* An outline with nothing in it has an empty box. */
	if (outline->point_count == 0) {
		*minimum_x = 0.0f;
		*minimum_y = 0.0f;
		*maximum_x = 0.0f;
		*maximum_y = 0.0f;
		return;
	}

	/* The box starts at the first point. */
	*minimum_x = outline->points[0].x;
	*maximum_x = outline->points[0].x;
	*minimum_y = outline->points[0].y;
	*maximum_y = outline->points[0].y;

	/* Widens the box to each other point. */
	for (index = 1; index < outline->point_count; index++) {
		/* A point further left than any before it. */
		if (outline->points[index].x < *minimum_x)
			*minimum_x = outline->points[index].x;

		/* A point further right than any before it. */
		if (outline->points[index].x > *maximum_x)
			*maximum_x = outline->points[index].x;

		/* A point lower than any before it. */
		if (outline->points[index].y < *minimum_y)
			*minimum_y = outline->points[index].y;

		/* A point higher than any before it. */
		if (outline->points[index].y > *maximum_y)
			*maximum_y = outline->points[index].y;
	}
}

/*
 * Makes an outline bold: every contour is pushed out from the ink by half
 * a strength on each side, across (x_strength) and up and down
 * (y_strength), in pixels, then the whole is moved right and up by the
 * other half, so that the glyph still starts where it did and stands on
 * the baseline and only grows to the right and upward (BUG-205).
 *
 * A vertex moves along the bisector of the normals of its two edges, as far
 * as keeps both edges the strength away (FreeType's FT_Outline_EmboldenXY):
 * a straight run moves by half the strength, a corner by more.  An inner
 * corner moves no further than its shorter edge is long, so that short
 * edges do not cross over each other, and a vertex that turns almost all
 * the way back does not move.  Which side is the ink's follows the
 * outline's turning: TrueType's outer contours run clockwise (y up), but a
 * font whose contours run the other way is widened outward too.  The curves
 * are already segments here, so the widening is exact for what is drawn.
 *
 * Returns 0, or ENOMEM when the scratch for the vertices cannot be had.
 */
int
truetype_outline_embolden(
	struct truetype_outline *outline,
	float x_strength,
	float y_strength)
{
	struct embolden_vertex *vertices;
	const struct truetype_point *a;
	const struct truetype_point *b;
	float half_x;
	float half_y;
	float shorter;
	float area;
	float side;
	float length;
	float in_x;
	float in_y;
	float in_length;
	float out_x;
	float out_y;
	float out_length;
	float cosine;
	float sine;
	float normal_x;
	float normal_y;
	unsigned contour;
	unsigned first;
	unsigned end;
	unsigned count;
	unsigned index;
	unsigned edge;
	unsigned step;
	unsigned point;

	/* An outline with no points stays as it is. */
	if (outline->point_count == 0U)
		return 0;

	/* The scratch for the vertices of the largest contour, which is no larger than the outline. */
	vertices = malloc(sizeof(vertices[0]) * outline->point_count);
	if (vertices == NULL)
		return ENOMEM;

	/* Half the strength goes to each side of a stroke. */
	half_x = x_strength * 0.5f;
	half_y = y_strength * 0.5f;

	/* The outline's turning, twice its signed area: negative for TrueType's clockwise outer contours. */
	area = 0.0f;
	first = 0;
	for (contour = 0; contour < outline->contour_count; contour++) {
		end = outline->ends[contour];

		/* Each edge of the contour adds its part of the area. */
		for (index = first; index + 1U < end; index++) {
			a = &outline->points[index];
			b = &outline->points[index + 1U];
			area += a->x * b->y - b->x * a->y;
		}
		first = end;
	}

	/* The ink is on the right of a clockwise contour's travel; a font that runs the other way has it on the left. */
	side = 1.0f;
	if (area > 0.0f)
		side = -1.0f;

	/* Each contour on its own. */
	first = 0;
	for (contour = 0; contour < outline->contour_count; contour++) {
		end = outline->ends[contour];

		/*
		 * The contour's distinct vertices: it is closed by a last point
		 * that repeats its first, which moves with the first.
		 */
		count = end - first;
		if (count > 1U &&
		    outline->points[end - 1U].x == outline->points[first].x &&
		    outline->points[end - 1U].y == outline->points[first].y)
			count--;

		/* A contour of fewer than three vertices has no inside to widen; it only moves with the rest. */
		if (count < 3U) {
			for (point = first; point < end; point++) {
				outline->points[point].x += half_x;
				outline->points[point].y += half_y;
			}
			first = end;
			continue;
		}

		/* The direction of each edge, from a vertex to the next; an edge of no length has none. */
		for (edge = 0; edge < count; edge++) {
			a = &outline->points[first + edge];
			b = &outline->points[first + (edge + 1U) % count];
			vertices[edge].direction_x = b->x - a->x;
			vertices[edge].direction_y = b->y - a->y;
			length = sqrtf(vertices[edge].direction_x * vertices[edge].direction_x + vertices[edge].direction_y * vertices[edge].direction_y);
			vertices[edge].length = length;
			if (length > 0.0f) {
				vertices[edge].direction_x /= length;
				vertices[edge].direction_y /= length;
			}
		}

		/* How far each vertex moves, from the edges into and out of it. */
		for (point = 0; point < count; point++) {
			vertices[point].shift_x = 0.0f;
			vertices[point].shift_y = 0.0f;

			/* The edge into the vertex: the last one before it that has a length. */
			in_x = 0.0f;
			in_y = 0.0f;
			in_length = 0.0f;
			for (step = 1; step <= count; step++) {
				edge = (point + count - step) % count;
				if (vertices[edge].length > 0.0f) {
					in_x = vertices[edge].direction_x;
					in_y = vertices[edge].direction_y;
					in_length = vertices[edge].length;
					break;
				}
			}

			/* The edge out of it: the first one from it that has a length. */
			out_x = 0.0f;
			out_y = 0.0f;
			out_length = 0.0f;
			for (step = 0; step < count; step++) {
				edge = (point + step) % count;
				if (vertices[edge].length > 0.0f) {
					out_x = vertices[edge].direction_x;
					out_y = vertices[edge].direction_y;
					out_length = vertices[edge].length;
					break;
				}
			}

			/* A vertex that turns almost all the way back (or a contour of no length) stays. */
			cosine = in_x * out_x + in_y * out_y;
			if (cosine <= -0.9375f)
				continue;
			cosine += 1.0f;

			/* The shorter of its two edges, which an inner corner moves no further than. */
			shorter = in_length;
			if (out_length < shorter)
				shorter = out_length;

			/*
			 * The sum of the two edges' normals away from the ink, and
			 * the sine of the turn, positive where the contour turns
			 * into the ink (an inner corner).
			 */
			normal_x = -(in_y + out_y) * side;
			normal_y = (in_x + out_x) * side;
			sine = (in_x * out_y - in_y * out_x) * side;

			/* Across: a straight run or an outer corner moves by its miter; an inner corner is held to its shorter edge. */
			if (half_x * sine <= shorter * cosine) {
				vertices[point].shift_x = normal_x * half_x / cosine;
			} else {
				vertices[point].shift_x = normal_x * shorter / sine;
			}

			/* Up and down, the same. */
			if (half_y * sine <= shorter * cosine) {
				vertices[point].shift_y = normal_y * half_y / cosine;
			} else {
				vertices[point].shift_y = normal_y * shorter / sine;
			}
		}

		/* Every vertex moved, with the half that keeps the glyph's start and baseline. */
		for (point = 0; point < count; point++) {
			outline->points[first + point].x += vertices[point].shift_x + half_x;
			outline->points[first + point].y += vertices[point].shift_y + half_y;
		}

		/* The closing point follows the first. */
		if (count < end - first)
			outline->points[end - 1U] = outline->points[first];
		first = end;
	}

	/* Succeeded: the outline is bold. */
	free(vertices);
	return 0;
}

/* Reads a simple glyph (its contours, flags and coordinates) into the outline, in pixels. */
static int
load_simple(
	struct truetype_face *face,
	const uint8_t *glyf,
	uint32_t length,
	struct truetype_outline *outline)
{
	struct truetype_point raw[TRUETYPE_POINTS_MAX];
	float values[TRUETYPE_POINTS_MAX];
	uint8_t flags[TRUETYPE_POINTS_MAX];
	uint32_t position;
	uint32_t instructions;
	unsigned contours;
	unsigned count;
	unsigned index;
	unsigned first;
	unsigned last;
	unsigned contour;
	float scale;
	int error;

	/* The number of contours. */
	contours = truetype_u16(glyf);

	/* Refuses a glyph with more contours than an outline holds. */
	if (contours > TRUETYPE_CONTOURS_MAX)
		return ENOTSUP;

	/* Refuses a header the entry does not hold. */
	if (length < 10U + (uint32_t)contours * 2U + 2U)
		return EINVAL;

	/* The number of points: one past the last contour's end point. */
	count = truetype_u16(glyf + 10U + (size_t)(contours - 1U) * 2U) + 1U;

	/* Refuses a glyph with more points than an outline holds. */
	if (count > TRUETYPE_POINTS_MAX)
		return ENOTSUP;

	/* The instructions after the end points, which are not followed. */
	position = 10U + (uint32_t)contours * 2U;
	instructions = truetype_u16(glyf + position);
	position += 2U;

	/* Refuses instructions the entry does not hold, and passes over them. */
	if (instructions > length - position)
		return EINVAL;
	position += instructions;

	/* Reads each point's flags. */
	error = read_flags(glyf, length, count, &position, flags);
	if (error != 0)
		return error;

	/* Reads the x coordinates. */
	error = read_coordinates(glyf, length, count, flags, X_SHORT, X_SAME, &position, values);
	if (error != 0)
		return error;

	/* The points' x, in pixels. */
	scale = truetype_scale(face);
	for (index = 0; index < count; index++)
		raw[index].x = values[index];

	/* Reads the y coordinates. */
	error = read_coordinates(glyf, length, count, flags, Y_SHORT, Y_SAME, &position, values);
	if (error != 0)
		return error;

	/* The points' y, and whether each is on the curve. */
	for (index = 0; index < count; index++) {
		raw[index].y = values[index];
		raw[index].on_curve = (flags[index] & ON_CURVE) != 0;
	}

	/* The outline is kept in pixels, with y growing upward as the font has it. */
	for (index = 0; index < count; index++) {
		raw[index].x *= scale;
		raw[index].y *= scale;
	}

	/* Traces each contour, turning its curves into segments. */
	first = 0;
	for (contour = 0; contour < contours; contour++) {
		/* The contour's last point. */
		last = truetype_u16(glyf + 10U + (size_t)contour * 2U);

		/* Refuses an end point outside the point list, or before the contour's first. */
		if (last >= count || last < first)
			return EINVAL;

		/* Adds the contour's segments. */
		error = trace_contour(outline, raw, first, last);
		if (error != 0)
			return error;

		/* The next contour starts after this one. */
		first = last + 1U;
	}

	/* Succeeded: the glyph's contours are added. */
	return 0;
}

/*
 * Reads a simple glyph's flags, one a point; a flag with REPEAT is followed
 * by how many more points have the same flag.
 */
static int
read_flags(
	const uint8_t *glyf,
	uint32_t length,
	unsigned count,
	uint32_t *position,
	uint8_t *flags)
{
	unsigned index;
	unsigned repeat;

	/* Reads each flag until every point has one. */
	index = 0;
	while (index < count) {
		/* Refuses a flag the entry does not hold. */
		if (*position >= length)
			return EINVAL;

		/* The point's flag. */
		flags[index] = glyf[*position];
		(*position)++;
		index++;

		/* A flag without REPEAT is the only one. */
		if ((flags[index - 1U] & REPEAT) == 0)
			continue;

		/* Refuses a repeat count the entry does not hold. */
		if (*position >= length)
			return EINVAL;

		/* The number of points that repeat it. */
		repeat = glyf[*position];
		(*position)++;

		/* Gives the following points the same flag, within the glyph's points. */
		while (repeat > 0 && index < count) {
			flags[index] = flags[index - 1U];
			index++;
			repeat--;
		}
	}

	/* Succeeded: every point has its flag. */
	return 0;
}

/*
 * Reads a simple glyph's x or y coordinates, which are stored as
 * differences: a short one is a byte whose sign is the same flag, a long
 * one two bytes, and a point with the same flag and no short one keeps the
 * coordinate before it.
 */
static int
read_coordinates(
	const uint8_t *glyf,
	uint32_t length,
	unsigned count,
	const uint8_t *flags,
	uint8_t short_flag,
	uint8_t same_flag,
	uint32_t *position,
	float *values)
{
	unsigned index;
	int value;

	/* Adds each difference to the coordinate before it, from zero. */
	value = 0;
	for (index = 0; index < count; index++) {
		if ((flags[index] & short_flag) != 0) {
			/* The short form: refuses a byte the entry does not hold. */
			if (*position >= length)
				return EINVAL;

			/* The byte, positive when the same flag is set. */
			if ((flags[index] & same_flag) != 0) {
				value += (int)glyf[*position];
			} else {
				value -= (int)glyf[*position];
			}

			/* Past the byte. */
			(*position)++;
		} else if ((flags[index] & same_flag) == 0) {
			/* The long form: refuses two bytes the entry does not hold. */
			if (*position + 2U > length)
				return EINVAL;

			/* The signed difference. */
			value += truetype_s16(glyf + *position);
			*position += 2U;
		}

		/* The point's coordinate. */
		values[index] = (float)value;
	}

	/* Succeeded: every point has its coordinate. */
	return 0;
}

/*
 * Adds one contour of a simple glyph to the outline: its points from first
 * to last, the curves between control points turned into segments, closed
 * back to where it started.
 */
static int
trace_contour(
	struct truetype_outline *outline,
	const struct truetype_point *raw,
	unsigned first,
	unsigned last)
{
	struct truetype_point start;
	struct truetype_point previous;
	struct truetype_point current;
	struct truetype_point implied;
	struct truetype_point control;
	unsigned index;
	unsigned started;
	unsigned have_control;
	float x;
	float y;

	/*
	 * A contour may begin on a control point, in which case the start is
	 * the midpoint between it and the last point, or the last point itself
	 * when that one is on the curve.
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

	/* The contour starts at its start, with no control point waiting. */
	outline->points[outline->point_count] = start;
	outline->point_count++;
	previous = start;
	have_control = 0;
	started = 0;
	x = 0.0f;
	y = 0.0f;

	/* Follows each point of the contour, wrapping to its first. */
	while (started <= last - first) {
		/* The next point. */
		current = raw[first + (index - first) % (last - first + 1U)];
		index++;
		started++;

		/* An on-curve point ends the curve waiting for it, or is a line's end. */
		if (current.on_curve) {
			if (have_control) {
				/* The curve through the waiting control point. */
				implied.x = x;
				implied.y = y;
				implied.on_curve = 0U;
				emit_quadratic(outline, previous, implied, current);
				have_control = 0;
			} else {
				/* Stops rather than overrunning. */
				if (outline->point_count >= TRUETYPE_POINTS_MAX)
					return ENOSPC;

				/* The line's end. */
				outline->points[outline->point_count] = current;
				outline->point_count++;
			}

			/* The next curve or line starts here. */
			previous = current;
			continue;
		}

		/*
		 * Two control points in a row imply an on-curve point halfway
		 * between them, which ends the first curve.
		 */
		if (have_control) {
			implied.x = (x + current.x) * 0.5f;
			implied.y = (y + current.y) * 0.5f;
			implied.on_curve = 1U;
			current.on_curve = 0U;
			previous.on_curve = 1U;
			control.x = x;
			control.y = y;
			control.on_curve = 0U;
			emit_quadratic(outline, previous, control, implied);
			previous = implied;
		}

		/* The control point waits for the curve's end. */
		x = current.x;
		y = current.y;
		have_control = 1U;
	}

	/* Closes the contour back to where it started: by a curve, or by a line when there is room. */
	if (have_control) {
		control.x = x;
		control.y = y;
		control.on_curve = 0U;
		emit_quadratic(outline, previous, control, start);
	} else if (outline->point_count < TRUETYPE_POINTS_MAX) {
		outline->points[outline->point_count] = start;
		outline->point_count++;
	}

	/* Stops rather than overrunning the contour array. */
	if (outline->contour_count >= TRUETYPE_CONTOURS_MAX)
		return ENOSPC;

	/* Succeeded: the contour ends at the outline's last point. */
	outline->ends[outline->contour_count] = outline->point_count;
	outline->contour_count++;
	return 0;
}

/* Reads a composite glyph: other glyphs placed at offsets. */
static int
load_composite(
	struct truetype_face *face,
	const uint8_t *glyf,
	uint32_t length,
	struct truetype_outline *outline,
	unsigned depth)
{
	uint32_t position;
	uint32_t arguments;
	unsigned flags;
	unsigned component;
	unsigned first;
	unsigned index;
	float offset_x;
	float offset_y;
	float scale;
	int error;

	/* Refuses a font that nests components deeper than this follows. */
	if (depth >= TRUETYPE_COMPOSITE_DEPTH)
		return ELOOP;

	/* The components start after the header. */
	position = 10U;
	scale = truetype_scale(face);

	/* Adds each component, until one says it is the last. */
	do {
		/* Refuses a component header the entry does not hold. */
		if (position + 4U > length)
			return EINVAL;

		/* The component's flags and glyph. */
		flags = truetype_u16(glyf + position);
		component = truetype_u16(glyf + position + 2U);
		position += 4U;

		/* The arguments are two words or two bytes. */
		arguments = 2U;
		if ((flags & ARG_WORDS) != 0)
			arguments = 4U;

		/* Refuses arguments the entry does not hold. */
		if (position + arguments > length)
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

		/* Alignment by point number is not followed: the component stays where it is. */
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

		/* Refuses a transform the entry does not hold. */
		if (position > length)
			return EINVAL;

		/* Adds the component glyph's outline, one level deeper. */
		first = outline->point_count;
		error = truetype_outline_load(face, component, outline, depth + 1U);
		if (error != 0)
			return error;

		/* Moves the points this component added into place. */
		for (index = first; index < outline->point_count; index++) {
			outline->points[index].x += offset_x * scale;
			outline->points[index].y += offset_y * scale;
		}
	} while ((flags & MORE_COMPONENTS) != 0);

	/* Succeeded: every component is added. */
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
	float deviation;
	float x;
	float y;
	float t;
	float u;
	unsigned steps;
	unsigned step;

	/* How far the control point sits from the chord's middle, which decides the segments. */
	deviation = fabsf(control.x - (start.x + end.x) * 0.5f) + fabsf(control.y - (start.y + end.y) * 0.5f);
	steps = (unsigned)(deviation * 0.5f) + 2U;

	/* A curve is never worth more than sixteen segments. */
	if (steps > 16U)
		steps = 16U;

	/* Adds the end of each segment along the curve. */
	for (step = 1; step <= steps; step++) {
		/* Stops rather than overrunning the point array. */
		if (outline->point_count >= TRUETYPE_POINTS_MAX)
			return;

		/* The curve's point at the step. */
		t = (float)step / (float)steps;
		u = 1.0f - t;
		x = u * u * start.x + 2.0f * u * t * control.x + t * t * end.x;
		y = u * u * start.y + 2.0f * u * t * control.y + t * t * end.y;

		/* Adds it, on the curve. */
		outline->points[outline->point_count].x = x;
		outline->points[outline->point_count].y = y;
		outline->points[outline->point_count].on_curve = 1U;
		outline->point_count++;
	}
}
