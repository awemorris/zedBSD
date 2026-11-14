/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The outline of a stroke as pieces that fill together with the nonzero rule.
 *
 * Each contour of the center lines is cut into dashes (or kept whole); each
 * dash becomes a rectangle per segment, a join piece per vertex inside it and
 * a cap piece at each end, all computed in the units of the center lines and
 * mapped to the device when they are appended.
 */

#include "vector/vec-stroke.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/*
 * The ratio of a circle's circumference to its diameter in double
 * precision, for the angles of the arcs.
 */
#define STROKE_PI		3.14159265358979323846

/*
 * The most equal steps an arc is cut into (the limit of vec-stroke.h), and
 * the most vertices one piece can have: an arc of that many steps has one
 * more point than steps, a join sector one more for its center, and a
 * little room is left over.
 */
#define STROKE_ARC_STEPS_MAX	256
#define STROKE_POLY_MAX		(STROKE_ARC_STEPS_MAX + 8)

/*
 * The turn, in radians, under which two segments are taken as collinear
 * and have no join piece (the limit of vec-stroke.h).
 */
#define STROKE_COLLINEAR	1e-6

/* The most dash lengths after an odd list is repeated to an even one. */
#define STROKE_DASH_LIST_MAX	(VEC_DASHES_MAX * 2)

/* A point or a vector in double precision, the arithmetic type of the pieces before they are rounded to the output. */
struct stroke_point {
	double x;
	double y;
};

/*
 * The dash pattern of a style made ready: the lengths scaled by dash_scale
 * and repeated to an even count, their sum, and where in the pattern the
 * first dash starts (the "index" and the length left of it in SVG 2
 * §13.5.7's algorithm).  Made once per call; the walk through one contour
 * is struct stroke_walk.
 */
struct stroke_dashes {
	double list[STROKE_DASH_LIST_MAX];
	size_t count;
	double sum;
	size_t first_index;
	double first_length;
	int enabled;
};

/*
 * The position of the dash generator along one contour: the index into
 * the pattern last used, the distance reached, how many dashes were
 * produced, and whether it has started or is finished.
 */
struct stroke_walk {
	size_t index;
	double position;
	size_t produced;
	int started;
	int done;
};

/*
 * The state of one vec_stroke call: the style's numbers in double
 * precision, the contour being stroked with its cumulative lengths, the
 * output, and the scratch array of lengths reused from contour to contour.
 */
struct stroke_context {
	const struct vec_lines *center;
	struct vec_lines *out;
	const struct vec_matrix *matrix;
	double half;
	enum vec_cap cap;
	enum vec_join join;
	double miter_limit_squared;
	double tolerance;
	double scale;

	const struct vec_vertex *vertices;
	size_t vertex_count;
	int closed;
	size_t segment_count;
	double length;

	double *cumulative;
	size_t cumulative_capacity;
};

/* The forward declarations of the static functions. */
static int stroke_check_style(const struct vec_stroke_style *style);
static void stroke_dash_prepare(const struct vec_stroke_style *style, struct stroke_dashes *dashes);
static void stroke_dash_start(struct stroke_walk *walk);
static int stroke_dash_next(const struct stroke_dashes *dashes, struct stroke_walk *walk, double length, double *start, double *end);
static int stroke_reserve(struct stroke_context *context, size_t needed);
static int stroke_segment_vector(const struct stroke_context *context, size_t segment, struct stroke_point *direction);
static void stroke_measure(struct stroke_context *context);
static double stroke_cumulative(const struct stroke_context *context, size_t segment);
static struct stroke_point stroke_vertex(const struct stroke_context *context, size_t index);
static size_t stroke_find_segment(const struct stroke_context *context, double position);
static int stroke_corner(const struct stroke_context *context, size_t from, size_t to);
static int stroke_arc_steps(const struct stroke_context *context, double angle);
static struct stroke_point stroke_rotate(struct stroke_point vector, double angle);
static int stroke_emit(struct stroke_context *context, const struct stroke_point *points, size_t count);
static int stroke_emit_segment(struct stroke_context *context, struct stroke_point from, struct stroke_point to, struct stroke_point direction);
static int stroke_emit_join(struct stroke_context *context, struct stroke_point vertex, struct stroke_point before, struct stroke_point after, int corner);
static int stroke_emit_cap(struct stroke_context *context, struct stroke_point end, struct stroke_point outward);
static int stroke_emit_dot(struct stroke_context *context, double position);
static int stroke_emit_dash(struct stroke_context *context, double start, double end, int loop);
static int stroke_contour(struct stroke_context *context, const struct stroke_dashes *dashes, size_t index);

/*
 * Strokes polylines: appends the pieces of the stroke shape.
 *
 * The inputs are checked before anything is appended.  The contours are
 * then stroked one by one; when one fails, the counts of out are put back
 * to what they were, which drops everything the call added.
 */
int
vec_stroke(
	const struct vec_lines *center,
	const struct vec_stroke_style *style,
	const struct vec_matrix *to_device,
	struct vec_lines *out)
{
	struct stroke_context context;
	struct stroke_dashes dashes;
	size_t vertices_before;
	size_t contours_before;
	size_t i;
	int valid;
	int error;

	/* Refuses missing arguments, and an output that is the input. */
	if (center == NULL || style == NULL || out == NULL)
		return EINVAL;
	if (center == out)
		return EINVAL;

	/* Refuses polylines that are malformed. */
	error = vec_lines_check(center);
	if (error != 0)
		return error;

	/* Refuses a style with a value out of its range. */
	error = stroke_check_style(style);
	if (error != 0)
		return error;

	/* Refuses a matrix that is not valid, when one is given. */
	if (to_device != NULL) {
		valid = vec_matrix_valid(to_device);
		if (!valid)
			return EINVAL;
	}

	/* Makes the style's numbers ready for the contours. */
	memset(&context, 0, sizeof(context));
	context.center = center;
	context.out = out;
	context.matrix = to_device;
	context.half = (double)style->width * 0.5;
	context.cap = style->cap;
	context.join = style->join;
	context.miter_limit_squared = (double)style->miter_limit * (double)style->miter_limit;
	context.tolerance = (double)style->tolerance;
	context.scale = 1.0;
	if (to_device != NULL)
		context.scale = (double)vec_matrix_scale_max(to_device);
	stroke_dash_prepare(style, &dashes);

	/* Remembers how much out held, to put it back on failure. */
	vertices_before = out->vertex_count;
	contours_before = out->contour_count;

	/* Strokes every contour, stopping at the first failure. */
	error = 0;
	for (i = 0;
	     i < center->contour_count && error == 0;
	     i++)
		error = stroke_contour(&context, &dashes, i);

	/* Gives back the scratch array of lengths. */
	free(context.cumulative);

	/* Puts out back to what it held when a contour failed. */
	if (error != 0) {
		out->vertex_count = vertices_before;
		out->contour_count = contours_before;
		return error;
	}

	/* Succeeded: out has the pieces appended. */
	return 0;
}

/*
 * Measures the length of a contour of polylines.
 *
 * The sum is taken in double precision over the segments, with the closing
 * segment when the contour is closed.  A contour that does not lie in the
 * vertex array, or whose vertices give a length that is not finite, is
 * measured as 0.
 */
float
vec_contour_length(
	const struct vec_lines *lines,
	size_t contour)
{
	const struct vec_contour *entry;
	const struct vec_vertex *first;
	const struct vec_vertex *next;
	double sum;
	double dx;
	double dy;
	size_t segments;
	size_t i;
	int usable;

	/* Gives 0 for lines that are missing or a contour out of range. */
	if (lines == NULL)
		return 0.0f;
	if (contour >= lines->contour_count)
		return 0.0f;

	/* Gives 0 for a contour that does not lie in the vertex array. */
	entry = &lines->contours[contour];
	if (entry->count < 1)
		return 0.0f;
	if (entry->first > lines->vertex_count)
		return 0.0f;
	if (entry->count > lines->vertex_count - entry->first)
		return 0.0f;

	/* Counts the segments: one fewer than the vertices, and the closing one for a closed contour. */
	segments = entry->count - 1;
	if (entry->closed)
		segments = entry->count;

	/* Adds up the lengths of the segments. */
	first = &lines->vertices[entry->first];
	sum = 0.0;
	for (i = 0;
	     i < segments;
	     i++) {
		next = &first[(i + 1) % entry->count];
		dx = (double)next->x - (double)first[i].x;
		dy = (double)next->y - (double)first[i].y;
		sum += sqrt(dx * dx + dy * dy);
	}

	/* Gives 0 for a sum that is not usable (vertices that were not checked). */
	usable = sum >= 0.0 && sum < 1e38;
	if (!usable)
		return 0.0f;

	/* Succeeded: the length of the contour. */
	return (float)sum;
}

/*
 * Checks the style against the ranges of vec-stroke.h.
 */
static int
stroke_check_style(
	const struct vec_stroke_style *style)
{
	size_t i;
	int valid;

	/* Refuses a width that is not finite and positive. */
	valid = vec_finite(style->width);
	if (!valid)
		return EINVAL;
	if (style->width <= 0.0f)
		return EINVAL;

	/* Refuses a cap or a join that is not one of the enumerators. */
	if (style->cap != VEC_CAP_BUTT && style->cap != VEC_CAP_ROUND && style->cap != VEC_CAP_SQUARE)
		return EINVAL;
	if (style->join != VEC_JOIN_MITER && style->join != VEC_JOIN_ROUND && style->join != VEC_JOIN_BEVEL)
		return EINVAL;

	/* Refuses a miter limit that is not finite or is under 1. */
	valid = vec_finite(style->miter_limit);
	if (!valid)
		return EINVAL;
	if (style->miter_limit < 1.0f)
		return EINVAL;

	/* Refuses a dash list that is missing or too long. */
	if (style->dash_count > VEC_DASHES_MAX)
		return EINVAL;
	if (style->dash_count > 0 && style->dashes == NULL)
		return EINVAL;

	/* Refuses a dash length that is not finite or is negative. */
	for (i = 0;
	     i < style->dash_count;
	     i++) {
		valid = vec_finite(style->dashes[i]);
		if (!valid)
			return EINVAL;
		if (style->dashes[i] < 0.0f)
			return EINVAL;
	}

	/* Refuses an offset that is not finite. */
	valid = vec_finite(style->dash_offset);
	if (!valid)
		return EINVAL;

	/* Refuses a scale that is not finite and positive. */
	valid = vec_finite(style->dash_scale);
	if (!valid)
		return EINVAL;
	if (style->dash_scale <= 0.0f)
		return EINVAL;

	/* Refuses a tolerance that is not finite or is under the smallest accepted. */
	valid = vec_finite(style->tolerance);
	if (!valid)
		return EINVAL;
	if (style->tolerance < VEC_TOLERANCE_MIN)
		return EINVAL;

	/* Succeeded: every value of the style is in its range. */
	return 0;
}

/*
 * Makes the dash pattern ready.
 *
 * The lengths and the offset are multiplied by dash_scale, an odd list is
 * repeated to an even one, and the offset is reduced into one period of
 * the pattern (SVG 2 §13.5.7: a negative offset becomes sum - |offset|,
 * then the offset is taken modulo the sum).  The first dash is the first
 * index whose running sum reaches the offset, with the part of that entry
 * left after the offset.  A pattern whose lengths sum to 0 is no dashing.
 */
static void
stroke_dash_prepare(
	const struct vec_stroke_style *style,
	struct stroke_dashes *dashes)
{
	double offset;
	double running;
	size_t count;
	size_t i;

	/* Starts with no dashing, which is what a style without dashes keeps. */
	memset(dashes, 0, sizeof(*dashes));
	if (style->dash_count == 0)
		return;

	/* Scales the lengths into the units of the measured contour. */
	count = style->dash_count;
	for (i = 0;
	     i < count;
	     i++)
		dashes->list[i] = (double)style->dashes[i] * (double)style->dash_scale;

	/* Repeats an odd list to an even one. */
	if ((count % 2) != 0) {
		for (i = 0;
		     i < count;
		     i++)
			dashes->list[count + i] = dashes->list[i];
		count = count * 2;
	}
	dashes->count = count;

	/* Sums the lengths; a pattern of nothing but zeros is no dashing. */
	dashes->sum = 0.0;
	for (i = 0;
	     i < count;
	     i++)
		dashes->sum += dashes->list[i];
	if (!(dashes->sum > 0.0))
		return;

	/* Reduces the offset into [0, sum). */
	offset = (double)style->dash_offset * (double)style->dash_scale;
	offset = fmod(offset, dashes->sum);
	if (offset < 0.0)
		offset += dashes->sum;
	if (offset >= dashes->sum)
		offset = 0.0;

	/* Finds the first index whose running sum reaches the offset. */
	dashes->first_index = count - 1;
	dashes->first_length = 0.0;
	running = 0.0;
	for (i = 0;
	     i < count;
	     i++) {
		running += dashes->list[i];
		if (running >= offset) {
			dashes->first_index = i;
			dashes->first_length = running - offset;
			break;
		}
	}

	/* Marks the pattern in use. */
	dashes->enabled = 1;
}

/*
 * Starts the dash generator at the start of a contour.
 */
static void
stroke_dash_start(
	struct stroke_walk *walk)
{
	/* Zeroes the walk, which is the state before the first dash. */
	memset(walk, 0, sizeof(*walk));
}

/*
 * Produces the next dash of a contour as a pair of distances.
 *
 * This is the algorithm of SVG 2 §13.5.7 ("The dash positions for a given
 * subpath"), one pair at a time: the first entry is cut by the offset, each
 * later entry by the length left, and the pairs of the even entries (the
 * dashes, not the gaps) are produced.  When the dash produced would be
 * the VEC_DASH_PIECES_MAX'th, it is stretched to the end of the contour.
 *
 * Returns 1 with *start and *end set, or 0 when there are no more.
 */
static int
stroke_dash_next(
	const struct stroke_dashes *dashes,
	struct stroke_walk *walk,
	double length,
	double *start,
	double *end)
{
	double piece_start;
	double piece_end;
	double piece_length;
	double remaining;
	size_t index;

	/* Looks for the next even entry, passing over the gaps. */
	while (1) {
		/* Stops when the contour is used up. */
		if (walk->done)
			return 0;

		if (!walk->started) {
			/* Cuts the first entry by the offset and by the length of the contour. */
			walk->started = 1;
			index = dashes->first_index;
			piece_start = 0.0;
			piece_length = dashes->first_length;
			piece_end = piece_length;
			if (piece_length >= length) {
				piece_end = length;
				walk->done = 1;
			}
			walk->position = piece_end;
		} else {
			/* Stops at the end of the contour. */
			if (walk->position >= length) {
				walk->done = 1;
				return 0;
			}

			/* Takes the next entry of the pattern, cut by the length left. */
			index = (walk->index + 1) % dashes->count;
			piece_start = walk->position;
			piece_length = dashes->list[index];
			remaining = length - piece_start;
			piece_end = piece_start + piece_length;
			if (piece_length >= remaining) {
				piece_end = length;
				walk->done = 1;
			}
			walk->position = piece_end;
		}
		walk->index = index;

		/* Passes over a gap. */
		if ((index % 2) != 0)
			continue;

		/* Stretches the last dash that is allowed to the end of the contour. */
		if (walk->produced + 1 >= VEC_DASH_PIECES_MAX) {
			piece_end = length;
			walk->done = 1;
		}
		walk->produced++;

		/* Reports the dash. */
		*start = piece_start;
		*end = piece_end;
		return 1;
	}
}

/*
 * Makes the scratch array of cumulative lengths hold at least the given number of entries.
 */
static int
stroke_reserve(
	struct stroke_context *context,
	size_t needed)
{
	double *grown;

	/* Keeps the array when it is already large enough. */
	if (needed <= context->cumulative_capacity)
		return 0;

	/* Grows the array; the old one stays valid when this fails. */
	grown = realloc(context->cumulative, needed * sizeof(*grown));
	if (grown == NULL)
		return ENOMEM;

	/* Publishes the larger array. */
	context->cumulative = grown;
	context->cumulative_capacity = needed;

	/* Succeeded: the array holds the needed entries. */
	return 0;
}

/*
 * Reads a segment of the current contour as a unit direction.
 *
 * The segment is the unrolled index, so that a closed contour's segments
 * repeat after the last one.
 *
 * Returns 1 with *direction set, or 0 for a segment of zero length (which
 * has no direction).
 */
static int
stroke_segment_vector(
	const struct stroke_context *context,
	size_t segment,
	struct stroke_point *direction)
{
	struct stroke_point from;
	struct stroke_point to;
	double dx;
	double dy;
	double length;

	/* Reads the two ends of the segment. */
	from = stroke_vertex(context, segment);
	to = stroke_vertex(context, segment + 1);

	/* Takes the length of the segment. */
	dx = to.x - from.x;
	dy = to.y - from.y;
	length = sqrt(dx * dx + dy * dy);

	/* Reports a segment with no length as having no direction. */
	if (!(length > 0.0))
		return 0;

	/* Succeeded: the unit direction of the segment. */
	direction->x = dx / length;
	direction->y = dy / length;
	return 1;
}

/*
 * Measures the current contour: the distance along it at the start of each segment.
 *
 * The contour has segment_count segments, with the closing one when it
 * is closed; entry i of the array is the distance at the start of segment
 * i, and entry segment_count is the length.  The array has been reserved.
 */
static void
stroke_measure(
	struct stroke_context *context)
{
	struct stroke_point from;
	struct stroke_point to;
	double dx;
	double dy;
	double sum;
	size_t i;

	/* Adds up the lengths of the segments in order. */
	sum = 0.0;
	for (i = 0;
	     i < context->segment_count;
	     i++) {
		context->cumulative[i] = sum;
		from = stroke_vertex(context, i);
		to = stroke_vertex(context, i + 1);
		dx = to.x - from.x;
		dy = to.y - from.y;
		sum += sqrt(dx * dx + dy * dy);
	}

	/* Stores the length of the whole contour. */
	context->cumulative[context->segment_count] = sum;
	context->length = sum;
}

/*
 * Gives the distance along the contour at the start of an unrolled segment.
 *
 * A closed contour is walked more than once by a dash that wraps from its
 * end to its start; segment s is then segment s mod segment_count of the
 * next lap, a whole length further.
 */
static double
stroke_cumulative(
	const struct stroke_context *context,
	size_t segment)
{
	size_t laps;
	size_t within;

	/* Splits the index into whole laps and the segment within the lap. */
	laps = segment / context->segment_count;
	within = segment % context->segment_count;

	/* Succeeded: the distance in the lap plus the laps before it. */
	return context->cumulative[within] + (double)laps * context->length;
}

/*
 * Reads a vertex of the current contour by its unrolled index.
 */
static struct stroke_point
stroke_vertex(
	const struct stroke_context *context,
	size_t index)
{
	struct stroke_point point;
	const struct vec_vertex *vertex;

	/* Reads the vertex, wrapping round for a closed contour. */
	vertex = &context->vertices[index % context->vertex_count];
	point.x = (double)vertex->x;
	point.y = (double)vertex->y;

	/* Succeeded: the vertex position. */
	return point;
}

/*
 * Finds the segment that holds a distance, preferring the later one at a vertex.
 *
 * The segment is the first whose end is beyond the position, so a segment
 * of zero length is never chosen and a position at a vertex belongs to the
 * segment that starts there.
 *
 * Returns the index, or segment_count when the position is not before the
 * end of the contour.
 */
static size_t
stroke_find_segment(
	const struct stroke_context *context,
	double position)
{
	size_t low;
	size_t high;
	size_t middle;

	/* Bisects for the first segment whose end is beyond the position. */
	low = 0;
	high = context->segment_count;
	while (low < high) {
		middle = low + (high - low) / 2;
		if (context->cumulative[middle + 1] > position)
			high = middle;
		else
			low = middle + 1;
	}

	/* Succeeded: the segment, or the count when there is none. */
	return low;
}

/*
 * Tells whether any of the vertices from one index to another carries VEC_VERTEX_CORNER.
 *
 * The indices are unrolled ones; both ends are included.
 *
 * Returns 1 when one does, 0 otherwise.
 */
static int
stroke_corner(
	const struct stroke_context *context,
	size_t from,
	size_t to)
{
	size_t k;

	/* Looks at each vertex of the range. */
	for (k = from;
	     k <= to;
	     k++) {
		if ((context->vertices[k % context->vertex_count].flags & VEC_VERTEX_CORNER) != 0)
			return 1;
	}

	/* Succeeded: none of the vertices is a corner. */
	return 0;
}

/*
 * Gives the number of equal steps an arc of an angle is cut into.
 *
 * M = ceil(angle / (2 * acos(1 - tolerance / r))) with r the pen radius
 * after the device map; with tolerance >= r it is ceil(angle / (pi / 2)).
 * The result is at least 1 and at most STROKE_ARC_STEPS_MAX.
 */
static int
stroke_arc_steps(
	const struct stroke_context *context,
	double angle)
{
	double radius;
	double step;
	double steps;

	/* Takes the pen radius in the units of the output. */
	radius = context->half * context->scale;

	/* Takes the angle of one step, a quarter turn when the tolerance reaches the radius. */
	if (context->tolerance >= radius) {
		steps = angle / (STROKE_PI * 0.5);
	} else {
		step = 2.0 * acos(1.0 - context->tolerance / radius);
		if (!(step > 0.0))
			return STROKE_ARC_STEPS_MAX;
		steps = angle / step;
	}

	/* Rounds up and keeps the count within its limits before converting. */
	steps = ceil(steps);
	if (!(steps >= 1.0))
		return 1;
	if (steps > (double)STROKE_ARC_STEPS_MAX)
		return STROKE_ARC_STEPS_MAX;

	/* Succeeded: the count of steps. */
	return (int)steps;
}

/*
 * Turns a vector by an angle (positive towards the y axis from the x axis).
 */
static struct stroke_point
stroke_rotate(
	struct stroke_point vector,
	double angle)
{
	struct stroke_point turned;
	double cosine;
	double sine;

	/* Applies the rotation matrix. */
	cosine = cos(angle);
	sine = sin(angle);
	turned.x = vector.x * cosine - vector.y * sine;
	turned.y = vector.x * sine + vector.y * cosine;

	/* Succeeded: the turned vector. */
	return turned;
}

/*
 * Appends one piece to the output as a closed contour of positive signed area.
 *
 * Each point is mapped by to_device and checked to be a usable
 * coordinate.  The signed area is taken (the shoelace sum) of the mapped
 * points; a negative one reverses the order, which is also what a
 * mirroring matrix needs, and a piece of no area (the matrix collapses
 * it, or it is degenerate) is dropped.
 */
static int
stroke_emit(
	struct stroke_context *context,
	const struct stroke_point *points,
	size_t count)
{
	struct vec_point mapped[STROKE_POLY_MAX];
	double x;
	double y;
	double area;
	size_t i;
	size_t at;
	size_t next;
	int reversed;
	int error;

	/* Refuses a piece that is not a polygon or has more points than the buffer holds. */
	if (count < 3 || count > STROKE_POLY_MAX)
		return EINVAL;

	/* Maps each point by the matrix, refusing one that leaves the range of a coordinate. */
	for (i = 0;
	     i < count;
	     i++) {
		x = points[i].x;
		y = points[i].y;
		if (context->matrix != NULL) {
			x = (double)context->matrix->a * points[i].x + (double)context->matrix->c * points[i].y + (double)context->matrix->e;
			y = (double)context->matrix->b * points[i].x + (double)context->matrix->d * points[i].y + (double)context->matrix->f;
		}
		if (!(fabs(x) <= (double)VEC_COORD_MAX))
			return EINVAL;
		if (!(fabs(y) <= (double)VEC_COORD_MAX))
			return EINVAL;
		mapped[i].x = (float)x;
		mapped[i].y = (float)y;
	}

	/* Takes the signed area of the mapped points. */
	area = 0.0;
	for (i = 0;
	     i < count;
	     i++) {
		next = (i + 1) % count;
		area += (double)mapped[i].x * (double)mapped[next].y - (double)mapped[next].x * (double)mapped[i].y;
	}

	/* Drops a piece that has no area. */
	if (area == 0.0)
		return 0;

	/* Appends the vertices, in reverse when the area is negative. */
	reversed = area < 0.0;
	for (i = 0;
	     i < count;
	     i++) {
		at = i;
		if (reversed)
			at = count - 1 - i;
		if (i == 0)
			error = vec_lines_begin(context->out, mapped[at].x, mapped[at].y, 0);
		else
			error = vec_lines_add(context->out, mapped[at].x, mapped[at].y, 0);
		if (error != 0)
			return error;
	}

	/* Closes the contour. */
	vec_lines_close(context->out);

	/* Succeeded: the piece is appended. */
	return 0;
}

/*
 * Appends the rectangle of one segment piece: width wide, centred on the piece.
 */
static int
stroke_emit_segment(
	struct stroke_context *context,
	struct stroke_point from,
	struct stroke_point to,
	struct stroke_point direction)
{
	struct stroke_point points[4];
	double nx;
	double ny;
	int error;

	/* Takes the half-width offset perpendicular to the segment. */
	nx = -direction.y * context->half;
	ny = direction.x * context->half;

	/* Lays out the four corners. */
	points[0].x = from.x + nx;
	points[0].y = from.y + ny;
	points[1].x = from.x - nx;
	points[1].y = from.y - ny;
	points[2].x = to.x - nx;
	points[2].y = to.y - ny;
	points[3].x = to.x + nx;
	points[3].y = to.y + ny;

	/* Appends the rectangle. */
	error = stroke_emit(context, points, 4);
	if (error != 0)
		return error;

	/* Succeeded: the rectangle is appended. */
	return 0;
}

/*
 * Appends the join piece at a vertex between two segments, on the outer side of the turn.
 *
 * The style's join is used at a corner and a round join elsewhere.  Two
 * segments that turn by less than STROKE_COLLINEAR have no piece; a miter
 * beyond the limit is a bevel.  The offsets of the two segments' outer
 * sides are first and second; the pieces are the triangle (vertex, first,
 * second) for a bevel, the kite out to the miter point for a miter, and
 * the sector of the pen circle for a round join.
 */
static int
stroke_emit_join(
	struct stroke_context *context,
	struct stroke_point vertex,
	struct stroke_point before,
	struct stroke_point after,
	int corner)
{
	struct stroke_point points[STROKE_POLY_MAX];
	struct stroke_point first;
	struct stroke_point second;
	struct stroke_point turned;
	enum vec_join join;
	double cross;
	double dot;
	double turn;
	double side;
	double factor;
	int steps;
	int k;
	int error;

	/* Takes the turn from the direction before to the direction after. */
	cross = before.x * after.y - before.y * after.x;
	dot = before.x * after.x + before.y * after.y;
	turn = atan2(cross, dot);

	/* Draws no join where the segments are collinear. */
	if (fabs(turn) < STROKE_COLLINEAR)
		return 0;

	/* Takes the outer side: opposite to the side the path turns towards. */
	side = 1.0;
	if (turn > 0.0)
		side = -1.0;
	first.x = -before.y * context->half * side;
	first.y = before.x * context->half * side;
	second.x = -after.y * context->half * side;
	second.y = after.x * context->half * side;

	/* Chooses the join: the style's at a corner, round inside a flattened curve. */
	join = context->join;
	if (!corner)
		join = VEC_JOIN_ROUND;

	/* Cuts a miter that is longer than the limit back to a bevel. */
	factor = 1.0 + dot;
	if (join == VEC_JOIN_MITER) {
		if (!(factor > 0.0 && factor * context->miter_limit_squared >= 2.0))
			join = VEC_JOIN_BEVEL;
	}

	if (join == VEC_JOIN_ROUND) {
		/* Lays out the sector: the vertex, then points of the arc from the first offset to the second. */
		steps = stroke_arc_steps(context, fabs(turn));
		points[0] = vertex;
		for (k = 0;
		     k <= steps;
		     k++) {
			turned = first;
			if (k > 0)
				turned = stroke_rotate(first, turn * (double)k / (double)steps);
			if (k == steps)
				turned = second;
			points[k + 1].x = vertex.x + turned.x;
			points[k + 1].y = vertex.y + turned.y;
		}
		error = stroke_emit(context, points, (size_t)steps + 2);
	} else if (join == VEC_JOIN_MITER) {
		/* Lays out the kite: the vertex, the two outer corners and the miter point beyond them. */
		points[0] = vertex;
		points[1].x = vertex.x + first.x;
		points[1].y = vertex.y + first.y;
		points[2].x = vertex.x + (first.x + second.x) / factor;
		points[2].y = vertex.y + (first.y + second.y) / factor;
		points[3].x = vertex.x + second.x;
		points[3].y = vertex.y + second.y;
		error = stroke_emit(context, points, 4);
	} else {
		/* Lays out the bevel: the triangle of the vertex and the two outer corners. */
		points[0] = vertex;
		points[1].x = vertex.x + first.x;
		points[1].y = vertex.y + first.y;
		points[2].x = vertex.x + second.x;
		points[2].y = vertex.y + second.y;
		error = stroke_emit(context, points, 3);
	}
	if (error != 0)
		return error;

	/* Succeeded: the join piece is appended. */
	return 0;
}

/*
 * Appends the cap piece at an end of a dash.
 *
 * outward is the unit direction pointing away from the dash at that end.
 * Butt has no piece; square is the rectangle half the width long beyond the
 * end; round is the half disc on the outward side.
 */
static int
stroke_emit_cap(
	struct stroke_context *context,
	struct stroke_point end,
	struct stroke_point outward)
{
	struct stroke_point points[STROKE_POLY_MAX];
	struct stroke_point normal;
	struct stroke_point turned;
	double angle;
	int steps;
	int k;
	int error;

	/* Draws nothing for a butt cap. */
	if (context->cap == VEC_CAP_BUTT)
		return 0;

	/* Takes the half-width offset along the end's edge. */
	normal.x = -outward.y * context->half;
	normal.y = outward.x * context->half;

	if (context->cap == VEC_CAP_SQUARE) {
		/* Lays out the rectangle beyond the end. */
		points[0].x = end.x + normal.x;
		points[0].y = end.y + normal.y;
		points[1].x = end.x + normal.x + outward.x * context->half;
		points[1].y = end.y + normal.y + outward.y * context->half;
		points[2].x = end.x - normal.x + outward.x * context->half;
		points[2].y = end.y - normal.y + outward.y * context->half;
		points[3].x = end.x - normal.x;
		points[3].y = end.y - normal.y;
		error = stroke_emit(context, points, 4);
	} else {
		/* Lays out the half disc: the arc from one end of the edge through the outward point to the other end. */
		steps = stroke_arc_steps(context, STROKE_PI);
		if (steps < 2)
			steps = 2;
		for (k = 0;
		     k <= steps;
		     k++) {
			angle = -STROKE_PI * (double)k / (double)steps;
			turned = stroke_rotate(normal, angle);
			points[k].x = end.x + turned.x;
			points[k].y = end.y + turned.y;
		}
		error = stroke_emit(context, points, (size_t)steps + 1);
	}
	if (error != 0)
		return error;

	/* Succeeded: the cap piece is appended. */
	return 0;
}

/*
 * Appends the pieces of a dash of zero length at a distance along the contour.
 *
 * The dash is its two caps at one point, which is a disc for round and a
 * square for square, turned to the direction of the contour there (the
 * segment that starts at the position, else the last segment with a
 * length).  A contour with no segment of length at all (a zero-length
 * subpath) has the direction of the positive x axis.  Butt has nothing.
 */
static int
stroke_emit_dot(
	struct stroke_context *context,
	double position)
{
	struct stroke_point points[STROKE_POLY_MAX];
	struct stroke_point direction;
	struct stroke_point centre;
	struct stroke_point normal;
	struct stroke_point turned;
	size_t segment;
	size_t k;
	int found;
	int steps;
	int i;
	int error;

	/* Draws nothing for a butt cap. */
	if (context->cap == VEC_CAP_BUTT)
		return 0;

	/* Takes the direction and the point from the segment that holds the position. */
	found = 0;
	centre = stroke_vertex(context, 0);
	direction.x = 1.0;
	direction.y = 0.0;
	segment = stroke_find_segment(context, position);
	if (segment < context->segment_count) {
		found = stroke_segment_vector(context, segment, &direction);
		if (found) {
			centre = stroke_vertex(context, segment);
			if (position > context->cumulative[segment]) {
				centre.x += direction.x * (position - context->cumulative[segment]);
				centre.y += direction.y * (position - context->cumulative[segment]);
			}
		}
	}

	/* Falls back to the last segment that has a length, at its end. */
	if (!found) {
		for (k = context->segment_count;
		     k > 0 && !found;
		     k--) {
			found = stroke_segment_vector(context, k - 1, &direction);
			if (found)
				centre = stroke_vertex(context, k);
		}
	}

	/* Takes the half-width offset across the direction. */
	normal.x = -direction.y * context->half;
	normal.y = direction.x * context->half;

	if (context->cap == VEC_CAP_SQUARE) {
		/* Lays out the square of side width turned to the direction. */
		points[0].x = centre.x + normal.x + direction.x * context->half;
		points[0].y = centre.y + normal.y + direction.y * context->half;
		points[1].x = centre.x - normal.x + direction.x * context->half;
		points[1].y = centre.y - normal.y + direction.y * context->half;
		points[2].x = centre.x - normal.x - direction.x * context->half;
		points[2].y = centre.y - normal.y - direction.y * context->half;
		points[3].x = centre.x + normal.x - direction.x * context->half;
		points[3].y = centre.y + normal.y - direction.y * context->half;
		error = stroke_emit(context, points, 4);
	} else {
		/* Lays out the disc as a polygon with equal steps over the whole turn. */
		steps = stroke_arc_steps(context, 2.0 * STROKE_PI);
		if (steps < 3)
			steps = 3;
		for (i = 0;
		     i < steps;
		     i++) {
			turned = stroke_rotate(normal, 2.0 * STROKE_PI * (double)i / (double)steps);
			points[i].x = centre.x + turned.x;
			points[i].y = centre.y + turned.y;
		}
		error = stroke_emit(context, points, (size_t)steps);
	}
	if (error != 0)
		return error;

	/* Succeeded: the dot is appended. */
	return 0;
}

/*
 * Appends the pieces of one dash: from a distance along the contour to another.
 *
 * end may pass the length of a closed contour by up to a lap: the dash then
 * wraps from the contour's end to its start.  With loop set the dash is the
 * whole of a closed contour: it has a join where it closes on itself and no
 * caps.  A segment piece is appended for each segment of length the dash
 * covers, a join at each vertex inside the dash (between the nearest
 * segments of length on either side), and a cap at each end.
 */
static int
stroke_emit_dash(
	struct stroke_context *context,
	double start,
	double end,
	int loop)
{
	struct stroke_point direction;
	struct stroke_point previous_direction;
	struct stroke_point first_direction;
	struct stroke_point first_from;
	struct stroke_point vertex;
	struct stroke_point from;
	struct stroke_point to;
	struct stroke_point outward;
	double segment_start;
	double segment_end;
	double a;
	double b;
	size_t limit;
	size_t j;
	size_t previous_j;
	size_t first_j;
	int have_piece;
	int has_direction;
	int corner;
	int error;

	/* Draws a dash of no length as its caps only. */
	if (end <= start) {
		error = stroke_emit_dot(context, start);
		if (error != 0)
			return error;

		/* Succeeded: the dot is appended. */
		return 0;
	}

	/* Takes how many segments can be walked: one lap, or two for a closed contour whose dash may wrap. */
	limit = context->segment_count;
	if (context->closed)
		limit = context->segment_count * 2;

	/* Starts with no piece drawn. */
	have_piece = 0;
	previous_j = 0;
	first_j = 0;
	memset(&previous_direction, 0, sizeof(previous_direction));
	memset(&first_direction, 0, sizeof(first_direction));
	memset(&first_from, 0, sizeof(first_from));
	memset(&to, 0, sizeof(to));

	/* Walks the segments the dash covers, from the one that holds its start. */
	j = stroke_find_segment(context, start);
	while (j < limit) {
		segment_start = stroke_cumulative(context, j);
		if (segment_start >= end)
			break;

		/* Passes over a segment of zero length. */
		has_direction = stroke_segment_vector(context, j, &direction);
		if (!has_direction) {
			j++;
			continue;
		}
		segment_end = stroke_cumulative(context, j + 1);

		/* Takes the part of the segment inside the dash. */
		a = start;
		if (a < segment_start)
			a = segment_start;
		b = end;
		if (b > segment_end)
			b = segment_end;
		if (!(b > a)) {
			j++;
			continue;
		}

		/* Finds the two ends of the part, using the vertices themselves where the part reaches them. */
		vertex = stroke_vertex(context, j);
		from = vertex;
		if (a > segment_start) {
			from.x = vertex.x + direction.x * (a - segment_start);
			from.y = vertex.y + direction.y * (a - segment_start);
		}
		to = stroke_vertex(context, j + 1);
		if (b < segment_end) {
			to.x = vertex.x + direction.x * (b - segment_start);
			to.y = vertex.y + direction.y * (b - segment_start);
		}

		/* Appends the join between the previous piece and this one, at the vertex where this segment starts. */
		if (have_piece) {
			corner = stroke_corner(context, previous_j + 1, j);
			error = stroke_emit_join(context, vertex, previous_direction, direction, corner);
			if (error != 0)
				return error;
		}

		/* Appends the rectangle of the part. */
		error = stroke_emit_segment(context, from, to, direction);
		if (error != 0)
			return error;

		/* Remembers the first piece, for the start cap or the closing join, and this one as the previous. */
		if (!have_piece) {
			first_direction = direction;
			first_from = from;
			first_j = j;
		}
		have_piece = 1;
		previous_j = j;
		previous_direction = direction;
		j++;
	}

	/* Has nothing more to draw when no segment of length lay inside the dash. */
	if (!have_piece)
		return 0;

	/* Closes a whole-contour dash on itself with a join at its first vertex. */
	if (loop) {
		corner = stroke_corner(context, previous_j + 1, first_j + context->segment_count);
		error = stroke_emit_join(context, first_from, previous_direction, first_direction, corner);
		if (error != 0)
			return error;

		/* Succeeded: the loop has joins and no caps. */
		return 0;
	}

	/* Appends the cap at the start, pointing backwards. */
	outward.x = -first_direction.x;
	outward.y = -first_direction.y;
	error = stroke_emit_cap(context, first_from, outward);
	if (error != 0)
		return error;

	/* Appends the cap at the end, pointing forwards. */
	error = stroke_emit_cap(context, to, previous_direction);
	if (error != 0)
		return error;

	/* Succeeded: the dash is appended. */
	return 0;
}

/*
 * Strokes one contour of the center lines.
 *
 * A moveto alone draws nothing.  Otherwise the contour is measured, and
 * either drawn whole (a closed one as a loop with a join at its first
 * vertex) or cut into dashes by the pattern.  On a closed contour the
 * dash that ends at the contour's end and the dash that starts at its
 * start are one dash joined at the first vertex.
 */
static int
stroke_contour(
	struct stroke_context *context,
	const struct stroke_dashes *dashes,
	size_t index)
{
	const struct vec_contour *contour;
	struct stroke_walk walk;
	double start;
	double end;
	double first_start;
	double first_end;
	double last_end;
	size_t count;
	size_t number;
	int more;
	int merge;
	int loop;
	int error;

	/* Draws nothing for a moveto that has no drawing command after it. */
	contour = &context->center->contours[index];
	if (contour->count == 1 && !contour->closed)
		return 0;

	/* Takes the contour's vertices and counts its segments, with the closing one when it is closed. */
	context->vertices = &context->center->vertices[contour->first];
	context->vertex_count = contour->count;
	context->closed = contour->closed != 0;
	context->segment_count = contour->count - 1;
	if (context->closed)
		context->segment_count = contour->count;

	/* Measures the contour. */
	error = stroke_reserve(context, context->segment_count + 1);
	if (error != 0)
		return error;
	stroke_measure(context);

	/* Draws a contour without dashes whole: a closed one with a length as a loop. */
	if (!dashes->enabled) {
		loop = context->closed && context->length > 0.0;
		if (loop)
			error = stroke_emit_dash(context, 0.0, context->length, 1);
		else
			error = stroke_emit_dash(context, 0.0, context->length, 0);
		if (error != 0)
			return error;

		/* Succeeded: the contour is drawn whole. */
		return 0;
	}

	/* Looks over the dashes of a closed contour first, for the first and the last. */
	start = 0.0;
	end = 0.0;
	merge = 0;
	loop = 0;
	count = 0;
	first_start = 0.0;
	first_end = 0.0;
	last_end = 0.0;
	if (context->closed && context->length > 0.0) {
		stroke_dash_start(&walk);
		more = stroke_dash_next(dashes, &walk, context->length, &start, &end);
		while (more) {
			if (count == 0) {
				first_start = start;
				first_end = end;
			}
			last_end = end;
			count++;
			more = stroke_dash_next(dashes, &walk, context->length, &start, &end);
		}

		/* Joins the last dash to the first when they meet at the first vertex. */
		if (count >= 2 && first_start == 0.0 && first_end > 0.0 && last_end == context->length)
			merge = 1;

		/* Takes a single dash over the whole contour as a loop. */
		if (count == 1 && first_start == 0.0 && first_end == context->length)
			loop = 1;
	}

	/* Draws each dash. */
	number = 0;
	stroke_dash_start(&walk);
	more = stroke_dash_next(dashes, &walk, context->length, &start, &end);
	while (more) {
		if (merge && number == 0) {
			/* The first dash is drawn with the last. */
			error = 0;
		} else if (merge && number + 1 == count) {
			/* The last dash runs on past the contour's end into the first dash. */
			error = stroke_emit_dash(context, start, context->length + first_end, 0);
		} else if (loop) {
			/* The one dash is the whole contour. */
			error = stroke_emit_dash(context, 0.0, context->length, 1);
		} else {
			/* An ordinary dash. */
			error = stroke_emit_dash(context, start, end, 0);
		}
		if (error != 0)
			return error;

		number++;
		more = stroke_dash_next(dashes, &walk, context->length, &start, &end);
	}

	/* Succeeded: the contour's dashes are drawn. */
	return 0;
}
