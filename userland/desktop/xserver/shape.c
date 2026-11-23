/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The shapes of the drawing requests as spans: thin lines, wide lines
 * with their caps and joins, dashes, polygons, and arcs filled or drawn
 * as lines.
 *
 * A pixel's centre is at its integer coordinates.  A filled shape (a
 * polygon, a wide line, a filled arc) takes the pixels whose centres are
 * inside it; a centre on its boundary is taken when the inside is
 * immediately to its right, or, on a horizontal edge, immediately below
 * it.  So a row's part of a shape whose boundary crosses the row at xl
 * and xr is the pixels from ceil(xl) up to but not including ceil(xr).
 * A thin line is the pixels Bresenham's steps visit, a halfway step
 * going toward the line's end.  The spans of a shape may overlap until
 * x11_spans_merge makes each pixel appear once, which is how a shape is
 * drawn without painting a pixel twice.
 *
 * Pure: no system header, no state outside the values passed.
 */

#include "userland/desktop/xserver/render.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The spans a list first makes room for, and the most it may hold (more is a failed allocation). */
#define SHAPE_FIRST		64U
#define SHAPE_MAX		((size_t)1U << 24)

/* A full circle in sixty-fourths of a degree, and the angle below which a mitre is cut off (eleven degrees). */
#define SHAPE_FULL_CIRCLE	(360 * 64)
#define SHAPE_MITER_LIMIT	11.0

/* How far (doubled pixels) a point on a ray or a chord is moved to see which side the inside is on. */
#define SHAPE_NUDGE		1e-3

/* How near half the width a distance is taken to be on the edge of a wide arc. */
#define SHAPE_ON_EDGE		1e-9

/* The largest doubled radius an ellipse is tested exactly with 64-bit integers for. */
#define SHAPE_EXACT_SIZE	8192

/* The furthest a doubled offset from an ellipse's centre is tested exactly for. */
#define SHAPE_EXACT_OFFSET	((long long)1 << 19)

/*
 * One edge of a polygon: its two ends, the rows it crosses (from first
 * up to but not including last), and whether it goes down (+1) or up
 * (-1).
 */
struct shape_edge {
	double x0;
	double y0;
	double x1;
	double y1;
	int first;
	int last;
	int direction;
};

/*
 * Where an edge crosses a row, and the edge's direction there.
 */
struct shape_crossing {
	double x;
	int direction;
};

/*
 * An ellipse and the part of it an arc keeps: its centre and size
 * doubled (so that every value is an integer), whether all of it is kept,
 * and the directions (on the ellipse's parameter, scaled by its size) of
 * the arc's two ends and its middle.
 */
struct shape_ellipse {
	long long centre_x;
	long long centre_y;
	long long width;
	long long height;
	int full;
	int over_half;
	double start_x;
	double start_y;
	double end_x;
	double end_y;
	double middle_x;
	double middle_y;
};

static int shape_reserve(struct x11_spans *list);
static int shape_compare_span(const void *a, const void *b);
static int shape_compare_edge(const void *a, const void *b);
static int shape_compare_crossing(const void *a, const void *b);
static int shape_ceil(double value);
static int shape_round(double value);
static int shape_dash_even(const struct x11_dash *dash);
static void shape_dash_step(struct x11_dash *dash, double length);
static struct x11_spans *shape_dash_list(struct x11_spans *even, struct x11_spans *odd, const struct x11_dash *dash);
static void shape_disc(struct x11_spans *list, double centre_x, double centre_y, double radius);
static void shape_quad(struct x11_spans *list, double from_x, double from_y, double to_x, double to_y, double half, double extend_from, double extend_to);
static void shape_cap(struct x11_spans *list, double x, double y, double half, unsigned cap);
static void shape_join(struct x11_spans *list, double x, double y, double in_x, double in_y, double out_x, double out_y, double half, unsigned join);
static void shape_dashed_path(struct x11_spans *even, struct x11_spans *odd, struct x11_dash *dash, const double *points, size_t count, int closed, double half, unsigned cap, unsigned join);
static void shape_ellipse_of(const struct x11_arc *arc, struct shape_ellipse *ellipse);
static int shape_inside(long long x, long long y, long long width, long long height);
static int shape_ellipse_row(long long centre_x, long long centre_y, long long width, long long height, int row, int *left, int *right);
static int shape_kept(const struct shape_ellipse *ellipse, double point_x, double point_y, int pie);
static void shape_wide_ellipse(struct x11_spans *list, const struct shape_ellipse *ellipse, const struct x11_arc *arc, unsigned width);
static double shape_foot(double x, double y, double radius_x, double radius_y, double *foot_x, double *foot_y);
static double shape_foot_root(double ratio, double along, double across, double inside);
static double shape_side(double from_x, double from_y, double to_x, double to_y, double x, double y);
static double shape_turn(const struct shape_ellipse *ellipse, double end_x, double end_y, double x, double y);
static int shape_on_line(double cross, const struct shape_ellipse *ellipse);
static void shape_add_kept(struct x11_spans *list, const struct shape_ellipse *ellipse, int row, int left, int right, int pie);
static void shape_arc_point(struct x11_spans *list, const struct shape_ellipse *ellipse, int x, int y);

/*
 * Makes an empty list of spans kept inside a box, holding no memory.
 */
void
x11_spans_init(
	struct x11_spans *list,
	const struct x11_box *bounds)
{
	/* No spans, and no room for any. */
	list->spans = NULL;
	list->count = 0U;
	list->capacity = 0U;
	list->bounds = *bounds;
	list->failed = 0;
}

/*
 * Frees a list's spans; it is empty afterwards.
 */
void
x11_spans_fini(
	struct x11_spans *list)
{
	/* The spans go. */
	free(list->spans);
	list->spans = NULL;
	list->count = 0U;
	list->capacity = 0U;
	list->failed = 0;
}

/*
 * Empties a list, keeping its room for later spans.
 */
void
x11_spans_clear(
	struct x11_spans *list)
{
	/* No spans, and memory may be found again. */
	list->count = 0U;
	list->failed = 0;
}

/*
 * Adds the pixels of a row from x0 up to but not including x1, the part
 * inside the list's box.
 */
void
x11_spans_add(
	struct x11_spans *list,
	int y,
	int x0,
	int x1)
{
	int error;

	/* A row outside the box adds nothing. */
	if (y < list->bounds.y0 || y >= list->bounds.y1)
		return;

	/* The part inside the box, if any. */
	if (x0 < list->bounds.x0)
		x0 = list->bounds.x0;
	if (x1 > list->bounds.x1)
		x1 = list->bounds.x1;
	if (x0 >= x1)
		return;

	/* Room for it. */
	error = shape_reserve(list);
	if (error != 0)
		return;

	/* The span. */
	list->spans[list->count].y = y;
	list->spans[list->count].x0 = x0;
	list->spans[list->count].x1 = x1;
	list->count++;
}

/*
 * Sorts a list's spans by row and column and joins those that overlap or
 * touch, so that each pixel appears once.
 */
void
x11_spans_merge(
	struct x11_spans *list)
{
	size_t index;
	size_t kept;

	/* Nothing to join. */
	if (list->count < 2U)
		return;

	/* In order of row, then of the first column. */
	qsort(list->spans, list->count, sizeof(*list->spans), shape_compare_span);

	/* Each span joins the one kept before it when they are on a row and meet. */
	kept = 0U;
	for (index = 1U; index < list->count; index++) {
		if (list->spans[index].y == list->spans[kept].y &&
		    list->spans[index].x0 <= list->spans[kept].x1) {
			if (list->spans[index].x1 > list->spans[kept].x1)
				list->spans[kept].x1 = list->spans[index].x1;
			continue;
		}
		kept++;
		list->spans[kept] = list->spans[index];
	}

	/* The spans kept. */
	list->count = kept + 1U;
}

/*
 * Gives the box around a list's spans (an empty box when it has none).
 */
void
x11_spans_extents(
	const struct x11_spans *list,
	struct x11_box *extents)
{
	size_t index;

	/* No spans: an empty box. */
	memset(extents, 0, sizeof(*extents));
	if (list->count == 0U)
		return;

	/* Each span widens the box. */
	extents->x0 = list->spans[0].x0;
	extents->y0 = list->spans[0].y;
	extents->x1 = list->spans[0].x1;
	extents->y1 = list->spans[0].y + 1;
	for (index = 1U; index < list->count; index++) {
		if (list->spans[index].x0 < extents->x0)
			extents->x0 = list->spans[index].x0;
		if (list->spans[index].y < extents->y0)
			extents->y0 = list->spans[index].y;
		if (list->spans[index].x1 > extents->x1)
			extents->x1 = list->spans[index].x1;
		if (list->spans[index].y + 1 > extents->y1)
			extents->y1 = list->spans[index].y + 1;
	}
}

/*
 * Starts a dashed line at its dash offset: how far into the dashes
 * (repeated) its first pixel is.
 */
void
x11_dash_start(
	struct x11_dash *dash,
	const uint8_t *lengths,
	size_t count,
	unsigned offset)
{
	size_t period;
	size_t index;
	unsigned total;

	/* The dashes, and how many make one turn of even and odd dashes. */
	dash->lengths = lengths;
	dash->count = count;
	period = count;
	if (count % 2U != 0U)
		period = count * 2U;

	/* The length of one turn, so that the offset is within one. */
	total = 0U;
	for (index = 0U; index < period; index++)
		total += lengths[index % count];
	offset %= total;

	/* The first dash, then the offset walked through. */
	dash->index = 0U;
	dash->left = (double)lengths[0];
	shape_dash_step(dash, (double)offset);
}

/*
 * Adds a thin line's pixels from one point to another: the first point
 * always, the last only when asked (a path draws each point once).  A
 * dashed line's pixels in an even dash go to the even list and those in
 * an odd dash to the odd list (dropped when it is NULL); the dash moves
 * on a pixel at a time.
 */
void
x11_shape_thin_line(
	struct x11_spans *even,
	struct x11_spans *odd,
	struct x11_dash *dash,
	int x0,
	int y0,
	int x1,
	int y1,
	int last)
{
	struct x11_spans *list;
	long long step_major;
	long long step_minor;
	long long error;
	int major_count;
	int minor_count;
	int pixels;
	int x_major;
	int step_x;
	int step_y;
	int x;
	int y;
	int index;

	/* The directions, and which axis the line goes further along. */
	step_x = 1;
	if (x1 < x0)
		step_x = -1;
	step_y = 1;
	if (y1 < y0)
		step_y = -1;
	x_major = 1;
	major_count = abs(x1 - x0);
	minor_count = abs(y1 - y0);
	if (minor_count > major_count) {
		x_major = 0;
		major_count = abs(y1 - y0);
		minor_count = abs(x1 - x0);
	}
	pixels = major_count;
	if (last)
		pixels++;

	/* A solid line wholly outside the box adds nothing; a dashed one still moves its dash. */
	if ((x0 < even->bounds.x0 && x1 < even->bounds.x0) ||
	    (x0 >= even->bounds.x1 && x1 >= even->bounds.x1) ||
	    (y0 < even->bounds.y0 && y1 < even->bounds.y0) ||
	    (y0 >= even->bounds.y1 && y1 >= even->bounds.y1)) {
		if (dash != NULL)
			shape_dash_step(dash, (double)pixels);
		return;
	}

	/*
	 * Bresenham's steps: the error says how far the minor axis lags the
	 * true line, doubled; a step that is exactly halfway goes on along the
	 * minor axis too, toward the end of the line.
	 */
	step_major = 2 * (long long)major_count;
	step_minor = 2 * (long long)minor_count;
	error = step_minor - (long long)major_count;
	x = x0;
	y = y0;
	for (index = 0; index < pixels; index++) {
		/* The pixel, in the list of its dash. */
		list = shape_dash_list(even, odd, dash);
		if (list != NULL)
			x11_spans_add(list, y, x, x + 1);
		if (dash != NULL)
			shape_dash_step(dash, 1.0);

		/* A step along the minor axis when the line has gone halfway or more past it. */
		if (error >= 0) {
			if (x_major) {
				y += step_y;
			} else {
				x += step_x;
			}
			error -= step_major;
		}
		error += step_minor;

		/* A step along the major axis. */
		if (x_major) {
			x += step_x;
		} else {
			y += step_y;
		}
	}
}

/*
 * Adds a wide line through points (x, y pairs): each segment a rectangle
 * of the width around it, the ends given the cap style and the points
 * where segments meet the join style.  A path whose first and last
 * points are the same (with more than two) is closed: its ends are
 * joined instead of capped.  A dashed path (dash not NULL) draws its even
 * dashes into the even list and its odd ones into the odd list (dropped
 * when it is NULL, an on-off dash, whose dashes take the cap style at
 * both ends).  The caller merges the spans.
 */
void
x11_shape_wide_path(
	struct x11_spans *even,
	struct x11_spans *odd,
	struct x11_dash *dash,
	const int *points,
	size_t count,
	unsigned width,
	unsigned cap,
	unsigned join)
{
	double *distinct;
	double half;
	double in_x;
	double in_y;
	double out_x;
	double out_y;
	double length;
	double extend_from;
	double extend_to;
	size_t kept;
	size_t index;
	int closed;

	/* Nothing to draw without a point. */
	if (count == 0U)
		return;

	/* The points, without those that repeat the one before (a segment of no length has no direction). */
	half = (double)width / 2.0;
	distinct = malloc(count * 2U * sizeof(*distinct));
	if (distinct == NULL) {
		even->failed = 1;
		return;
	}
	kept = 0U;
	for (index = 0U; index < count; index++) {
		if (kept > 0U &&
		    distinct[kept * 2U - 2U] == (double)points[index * 2U] &&
		    distinct[kept * 2U - 1U] == (double)points[index * 2U + 1U])
			continue;
		distinct[kept * 2U] = (double)points[index * 2U];
		distinct[kept * 2U + 1U] = (double)points[index * 2U + 1U];
		kept++;
	}

	/* A path closed on itself (its last point its first) is joined there. */
	closed = 0;
	if (count > 2U &&
	    kept > 2U &&
	    points[0] == points[count * 2U - 2U] &&
	    points[1] == points[count * 2U - 1U])
		closed = 1;

	/* A single point is its caps: a disc, a square, or nothing. */
	if (kept == 1U) {
		shape_cap(even, distinct[0], distinct[1], half, cap);
		free(distinct);
		return;
	}

	/* A dashed path is drawn dash by dash. */
	if (dash != NULL) {
		shape_dashed_path(even, odd, dash, distinct, kept, closed, half, cap, join);
		free(distinct);
		return;
	}

	/* Each segment, the ends of an open path projecting when the cap projects. */
	for (index = 0U; index + 1U < kept; index++) {
		extend_from = 0.0;
		extend_to = 0.0;
		if (!closed && index == 0U && cap == X11_CAP_PROJECTING)
			extend_from = half;
		if (!closed && index + 2U == kept && cap == X11_CAP_PROJECTING)
			extend_to = half;
		shape_quad(even, distinct[index * 2U], distinct[index * 2U + 1U], distinct[index * 2U + 2U], distinct[index * 2U + 3U], half, extend_from, extend_to);
	}

	/* The round caps of an open path's ends. */
	if (!closed && cap == X11_CAP_ROUND) {
		shape_disc(even, distinct[0], distinct[1], half);
		shape_disc(even, distinct[kept * 2U - 2U], distinct[kept * 2U - 1U], half);
	}

	/* The joins: at each point between two segments, and at the first point of a closed path. */
	for (index = 1U; index < kept; index++) {
		if (index + 1U == kept && !closed)
			break;
		in_x = distinct[index * 2U] - distinct[index * 2U - 2U];
		in_y = distinct[index * 2U + 1U] - distinct[index * 2U - 1U];
		if (index + 1U == kept) {
			out_x = distinct[2] - distinct[0];
			out_y = distinct[3] - distinct[1];
		} else {
			out_x = distinct[index * 2U + 2U] - distinct[index * 2U];
			out_y = distinct[index * 2U + 3U] - distinct[index * 2U + 1U];
		}
		length = sqrt(in_x * in_x + in_y * in_y);
		in_x /= length;
		in_y /= length;
		length = sqrt(out_x * out_x + out_y * out_y);
		out_x /= length;
		out_y /= length;
		shape_join(even, distinct[index * 2U], distinct[index * 2U + 1U], in_x, in_y, out_x, out_y, half, join);
	}

	/* Done with the points. */
	free(distinct);
}

/*
 * Adds a polygon (points as x, y pairs; its last point joins its first)
 * filled by its fill rule: a pixel is inside when a ray from it crosses
 * the boundary an odd number of times (even-odd), or crosses it more
 * times in one direction than in the other (winding).
 */
void
x11_shape_polygon(
	struct x11_spans *list,
	const double *points,
	size_t count,
	int winding)
{
	struct shape_edge *edges;
	struct shape_crossing *crossings;
	size_t *active;
	size_t edge_count;
	size_t active_count;
	size_t crossing_count;
	size_t next;
	size_t index;
	size_t kept;
	size_t following;
	double top;
	double bottom;
	int row;
	int first_row;
	int last_row;
	int wind;
	int start;

	/* Nothing to fill without three points. */
	if (count < 3U)
		return;

	/* Room for the edges, those a row crosses, and their crossings. */
	edges = malloc(count * sizeof(*edges));
	if (edges == NULL) {
		list->failed = 1;
		return;
	}
	active = malloc(count * sizeof(*active));
	if (active == NULL) {
		free(edges);
		list->failed = 1;
		return;
	}
	crossings = malloc(count * sizeof(*crossings));
	if (crossings == NULL) {
		free(active);
		free(edges);
		list->failed = 1;
		return;
	}

	/* Each edge that is not horizontal, with the rows whose centres are on or below its top and above its bottom. */
	edge_count = 0U;
	first_row = list->bounds.y1;
	last_row = list->bounds.y0;
	for (index = 0U; index < count; index++) {
		following = (index + 1U) % count;
		if (points[index * 2U + 1U] == points[following * 2U + 1U])
			continue;
		edges[edge_count].x0 = points[index * 2U];
		edges[edge_count].y0 = points[index * 2U + 1U];
		edges[edge_count].x1 = points[following * 2U];
		edges[edge_count].y1 = points[following * 2U + 1U];
		edges[edge_count].direction = 1;
		top = edges[edge_count].y0;
		bottom = edges[edge_count].y1;
		if (top > bottom) {
			edges[edge_count].direction = -1;
			top = edges[edge_count].y1;
			bottom = edges[edge_count].y0;
		}
		edges[edge_count].first = shape_ceil(top);
		edges[edge_count].last = shape_ceil(bottom);
		if (edges[edge_count].first < first_row)
			first_row = edges[edge_count].first;
		if (edges[edge_count].last > last_row)
			last_row = edges[edge_count].last;
		edge_count++;
	}

	/* The edges in order of their first row; only the rows inside the box. */
	qsort(edges, edge_count, sizeof(*edges), shape_compare_edge);
	if (first_row < list->bounds.y0)
		first_row = list->bounds.y0;
	if (last_row > list->bounds.y1)
		last_row = list->bounds.y1;

	/* Each row: the edges it crosses, where, and the spans between them. */
	next = 0U;
	active_count = 0U;
	for (row = first_row; row < last_row; row++) {
		/* The edges that start at or above the row join, those that end at or above it leave. */
		while (next < edge_count && edges[next].first <= row) {
			active[active_count] = next;
			active_count++;
			next++;
		}
		kept = 0U;
		for (index = 0U; index < active_count; index++) {
			if (edges[active[index]].last <= row)
				continue;
			active[kept] = active[index];
			kept++;
		}
		active_count = kept;

		/* Where each crosses the row's centre, left to right. */
		crossing_count = 0U;
		for (index = 0U; index < active_count; index++) {
			crossings[crossing_count].x = edges[active[index]].x0 +
			    ((double)row - edges[active[index]].y0) *
			    (edges[active[index]].x1 - edges[active[index]].x0) /
			    (edges[active[index]].y1 - edges[active[index]].y0);
			crossings[crossing_count].direction = edges[active[index]].direction;
			crossing_count++;
		}
		qsort(crossings, crossing_count, sizeof(*crossings), shape_compare_crossing);

		/* Even-odd: from each crossing to the next, every other one. */
		if (!winding) {
			for (index = 0U; index + 1U < crossing_count; index += 2U)
				x11_spans_add(list, row, shape_ceil(crossings[index].x), shape_ceil(crossings[index + 1U].x));
			continue;
		}

		/* Winding: from where the count leaves zero to where it comes back. */
		wind = 0;
		start = 0;
		for (index = 0U; index < crossing_count; index++) {
			if (wind == 0)
				start = shape_ceil(crossings[index].x);
			wind += crossings[index].direction;
			if (wind == 0)
				x11_spans_add(list, row, start, shape_ceil(crossings[index].x));
		}
	}

	/* Done with the edges. */
	free(crossings);
	free(active);
	free(edges);
}

/*
 * Adds a filled arc: the pixels inside its ellipse and, for a part of
 * one, inside the pie slice (the arc and the lines from the centre to its
 * ends) or the chord (the arc and the line between its ends).
 */
void
x11_shape_arc_fill(
	struct x11_spans *list,
	const struct x11_arc *arc,
	int pie)
{
	struct shape_ellipse ellipse;
	int first_row;
	int last_row;
	int row;
	int left;
	int right;
	int inside;

	/* An ellipse of no size, or an arc of no angle, has no inside. */
	if (arc->width <= 0 || arc->height <= 0)
		return;
	if (arc->angle2 == 0)
		return;

	/* The ellipse, and its rows inside the box. */
	shape_ellipse_of(arc, &ellipse);
	first_row = arc->y;
	last_row = arc->y + arc->height + 1;
	if (first_row < list->bounds.y0)
		first_row = list->bounds.y0;
	if (last_row > list->bounds.y1)
		last_row = list->bounds.y1;

	/* Each row's part of the ellipse, and of it the part the arc keeps. */
	for (row = first_row; row < last_row; row++) {
		inside = shape_ellipse_row(ellipse.centre_x, ellipse.centre_y, ellipse.width, ellipse.height, row, &left, &right);
		if (!inside)
			continue;
		shape_add_kept(list, &ellipse, row, left, right, pie);
	}
}

/*
 * Adds an arc drawn as a line: a thin one (width 0) is the pixels nearest
 * the ellipse, one in each column where it is flatter than a diagonal and
 * one in each row where it is steeper; a wide one is the pixels no
 * further than half the width from the ellipse whose nearest point on it
 * is inside the angles of its ends (for a circle, the ring between the
 * circles half the width outside and inside it).  The caller merges the
 * spans.
 */
void
x11_shape_arc_line(
	struct x11_spans *list,
	const struct x11_arc *arc,
	unsigned width)
{
	struct shape_ellipse ellipse;
	double centre_x;
	double centre_y;
	double radius_x;
	double radius_y;
	double offset;
	double reach;
	double square;
	double distance;
	long long outer_width;
	long long outer_height;
	long long inner_width;
	long long inner_height;
	int first_row;
	int last_row;
	int row;
	int column;
	int left;
	int right;
	int inner_left;
	int inner_right;
	int inside;
	int hollow;

	/* An arc of no angle draws nothing. */
	if (arc->angle2 == 0)
		return;

	/* A flat ellipse is a thin line along it. */
	if (arc->width <= 0 || arc->height <= 0) {
		x11_shape_thin_line(list, NULL, NULL, arc->x, arc->y, arc->x + arc->width, arc->y + arc->height, 1);
		return;
	}

	/* The ellipse; an arc wholly outside the box draws nothing. */
	shape_ellipse_of(arc, &ellipse);
	if (arc->x + arc->width + (int)width < list->bounds.x0 ||
	    arc->x - (int)width >= list->bounds.x1 ||
	    arc->y + arc->height + (int)width < list->bounds.y0 ||
	    arc->y - (int)width >= list->bounds.y1)
		return;

	/* A wide arc of an ellipse that is not a circle: the pixels near enough to it. */
	if (width > 0U && arc->width != arc->height) {
		shape_wide_ellipse(list, &ellipse, arc, width);
		return;
	}

	/* A wide arc of a circle: the ring between the outer and inner circles, row by row. */
	if (width > 0U) {
		outer_width = ellipse.width + (long long)width;
		outer_height = ellipse.height + (long long)width;
		inner_width = ellipse.width - (long long)width;
		inner_height = ellipse.height - (long long)width;
		hollow = 0;
		if (inner_width > 0 && inner_height > 0)
			hollow = 1;
		first_row = arc->y - (int)width;
		last_row = arc->y + arc->height + (int)width + 1;
		if (first_row < list->bounds.y0)
			first_row = list->bounds.y0;
		if (last_row > list->bounds.y1)
			last_row = list->bounds.y1;
		for (row = first_row; row < last_row; row++) {
			inside = shape_ellipse_row(ellipse.centre_x, ellipse.centre_y, outer_width, outer_height, row, &left, &right);
			if (!inside)
				continue;
			inside = 0;
			if (hollow)
				inside = shape_ellipse_row(ellipse.centre_x, ellipse.centre_y, inner_width, inner_height, row, &inner_left, &inner_right);
			if (!inside) {
				shape_add_kept(list, &ellipse, row, left, right, 1);
				continue;
			}
			shape_add_kept(list, &ellipse, row, left, inner_left, 1);
			shape_add_kept(list, &ellipse, row, inner_right, right, 1);
		}
		return;
	}

	/* A thin arc: the centre and radii in pixels. */
	centre_x = (double)ellipse.centre_x / 2.0;
	centre_y = (double)ellipse.centre_y / 2.0;
	radius_x = (double)arc->width / 2.0;
	radius_y = (double)arc->height / 2.0;

	/* In each column, the nearest pixel above and below the centre where the ellipse is flatter than a diagonal. */
	for (column = shape_ceil(centre_x - radius_x); (double)column <= centre_x + radius_x; column++) {
		if (column < list->bounds.x0 || column >= list->bounds.x1)
			continue;
		offset = (double)column - centre_x;
		square = 1.0 - (offset / radius_x) * (offset / radius_x);
		if (square < 0.0)
			square = 0.0;
		reach = radius_y * sqrt(square);
		distance = fabs(offset);
		if (radius_y * radius_y * distance > radius_x * radius_x * reach)
			continue;
		shape_arc_point(list, &ellipse, column, shape_round(centre_y - reach));
		shape_arc_point(list, &ellipse, column, shape_round(centre_y + reach));
	}

	/* In each row, the nearest pixel left and right of the centre where the ellipse is steeper than a diagonal. */
	for (row = shape_ceil(centre_y - radius_y); (double)row <= centre_y + radius_y; row++) {
		if (row < list->bounds.y0 || row >= list->bounds.y1)
			continue;
		offset = (double)row - centre_y;
		square = 1.0 - (offset / radius_y) * (offset / radius_y);
		if (square < 0.0)
			square = 0.0;
		reach = radius_x * sqrt(square);
		distance = fabs(offset);
		if (radius_x * radius_x * distance > radius_y * radius_y * reach)
			continue;
		shape_arc_point(list, &ellipse, shape_round(centre_x - reach), row);
		shape_arc_point(list, &ellipse, shape_round(centre_x + reach), row);
	}
}

/* Makes room for one more span.  Returns 0, or nonzero (the list is marked failed) when there is no memory. */
static int
shape_reserve(
	struct x11_spans *list)
{
	struct x11_span *spans;
	size_t capacity;

	/* Room enough already. */
	if (list->count < list->capacity)
		return 0;

	/* Doubled, within the most a list holds. */
	capacity = list->capacity * 2U;
	if (capacity < SHAPE_FIRST)
		capacity = SHAPE_FIRST;
	if (capacity > SHAPE_MAX) {
		list->failed = 1;
		return 1;
	}

	/* The memory. */
	spans = realloc(list->spans, capacity * sizeof(*spans));
	if (spans == NULL) {
		list->failed = 1;
		return 1;
	}

	/* Succeeded: the room. */
	list->spans = spans;
	list->capacity = capacity;
	return 0;
}

/* Orders spans by row, then by first column. */
static int
shape_compare_span(
	const void *a,
	const void *b)
{
	const struct x11_span *first;
	const struct x11_span *second;

	/* The rows, then the columns. */
	first = a;
	second = b;
	if (first->y != second->y) {
		if (first->y < second->y)
			return -1;
		return 1;
	}
	if (first->x0 < second->x0)
		return -1;
	if (first->x0 > second->x0)
		return 1;

	/* The same place. */
	return 0;
}

/* Orders edges by their first row. */
static int
shape_compare_edge(
	const void *a,
	const void *b)
{
	const struct shape_edge *first;
	const struct shape_edge *second;

	/* The first rows. */
	first = a;
	second = b;
	if (first->first < second->first)
		return -1;
	if (first->first > second->first)
		return 1;

	/* The same row. */
	return 0;
}

/* Orders crossings from left to right. */
static int
shape_compare_crossing(
	const void *a,
	const void *b)
{
	const struct shape_crossing *first;
	const struct shape_crossing *second;

	/* The columns. */
	first = a;
	second = b;
	if (first->x < second->x)
		return -1;
	if (first->x > second->x)
		return 1;

	/* The same column. */
	return 0;
}

/* Returns the smallest integer not below a value, kept within what an int holds. */
static int
shape_ceil(
	double value)
{
	/* Far values are as far as the coordinates go. */
	if (value > 1073741824.0)
		return 1073741824;
	if (value < -1073741824.0)
		return -1073741824;

	/* The ceiling. */
	return (int)ceil(value);
}

/* Returns the nearest integer to a value, halfway rounding up. */
static int
shape_round(
	double value)
{
	/* The floor of the value moved half a pixel. */
	return shape_ceil(floor(value + 0.5));
}

/* Tells whether a dash is an even one (drawn with the line's paint). */
static int
shape_dash_even(
	const struct x11_dash *dash)
{
	/* The dashes take turns from the first, which is even. */
	if (dash->index % 2U == 0U)
		return 1;

	return 0;
}

/* Moves a dash on by a length along its line. */
static void
shape_dash_step(
	struct x11_dash *dash,
	double length)
{
	size_t period;

	/* How many dashes make one turn. */
	period = dash->count;
	if (dash->count % 2U != 0U)
		period = dash->count * 2U;

	/* Each dash the length passes is left for the next. */
	dash->left -= length;
	while (dash->left <= 0.0) {
		dash->index = (dash->index + 1U) % period;
		dash->left += (double)dash->lengths[dash->index % dash->count];
	}
}

/* Returns the list a dash's pixels go to: the even list without dashes, else the dash's (NULL: dropped). */
static struct x11_spans *
shape_dash_list(
	struct x11_spans *even,
	struct x11_spans *odd,
	const struct x11_dash *dash)
{
	int drawn;

	/* A solid line. */
	if (dash == NULL)
		return even;

	/* An even dash, or an odd one. */
	drawn = shape_dash_even(dash);
	if (drawn)
		return even;

	return odd;
}

/* Adds a disc: the pixels whose centres are inside a circle. */
static void
shape_disc(
	struct x11_spans *list,
	double centre_x,
	double centre_y,
	double radius)
{
	double offset;
	double reach;
	int row;

	/* Each row within the radius, between the circle's two crossings. */
	for (row = shape_ceil(centre_y - radius); (double)row < centre_y + radius; row++) {
		if (row < list->bounds.y0)
			continue;
		if (row >= list->bounds.y1)
			break;
		offset = (double)row - centre_y;
		reach = sqrt(radius * radius - offset * offset);
		x11_spans_add(list, row, shape_ceil(centre_x - reach), shape_ceil(centre_x + reach));
	}
}

/*
 * Adds the rectangle of a wide segment: half the width on each side of
 * it, its ends moved out along it by their extensions.
 */
static void
shape_quad(
	struct x11_spans *list,
	double from_x,
	double from_y,
	double to_x,
	double to_y,
	double half,
	double extend_from,
	double extend_to)
{
	double corners[8];
	double length;
	double along_x;
	double along_y;
	double normal_x;
	double normal_y;

	/* The direction along the segment and across it. */
	length = sqrt((to_x - from_x) * (to_x - from_x) + (to_y - from_y) * (to_y - from_y));
	if (length == 0.0)
		return;
	along_x = (to_x - from_x) / length;
	along_y = (to_y - from_y) / length;
	normal_x = -along_y;
	normal_y = along_x;

	/* The ends moved out. */
	from_x -= along_x * extend_from;
	from_y -= along_y * extend_from;
	to_x += along_x * extend_to;
	to_y += along_y * extend_to;

	/* The four corners, around. */
	corners[0] = from_x + normal_x * half;
	corners[1] = from_y + normal_y * half;
	corners[2] = to_x + normal_x * half;
	corners[3] = to_y + normal_y * half;
	corners[4] = to_x - normal_x * half;
	corners[5] = to_y - normal_y * half;
	corners[6] = from_x - normal_x * half;
	corners[7] = from_y - normal_y * half;
	x11_shape_polygon(list, corners, 4U, 0);
}

/* Adds the cap of a lone point: a disc (Round), a square (Projecting), or nothing. */
static void
shape_cap(
	struct x11_spans *list,
	double x,
	double y,
	double half,
	unsigned cap)
{
	double corners[8];

	/* A round cap is a disc of the width. */
	if (cap == X11_CAP_ROUND) {
		shape_disc(list, x, y, half);
		return;
	}

	/* Butt and NotLast caps cover nothing beyond the point. */
	if (cap != X11_CAP_PROJECTING)
		return;

	/* A projecting cap is a square of the width. */
	corners[0] = x - half;
	corners[1] = y - half;
	corners[2] = x + half;
	corners[3] = y - half;
	corners[4] = x + half;
	corners[5] = y + half;
	corners[6] = x - half;
	corners[7] = y + half;
	x11_shape_polygon(list, corners, 4U, 0);
}

/*
 * Adds the join where a segment coming in (a direction of length 1)
 * meets one going out: a disc (Round), or the triangle between the point
 * and the outer corners of the two segments (Bevel), with the tip where
 * their outer edges meet (Miter, unless the angle between the segments
 * is under eleven degrees).
 */
static void
shape_join(
	struct x11_spans *list,
	double x,
	double y,
	double in_x,
	double in_y,
	double out_x,
	double out_y,
	double half,
	unsigned join)
{
	double corners[8];
	double in_normal_x;
	double in_normal_y;
	double out_normal_x;
	double out_normal_y;
	double side;
	double turn;
	double normals;
	double angle;

	/* A round join is a disc of the width. */
	if (join == X11_JOIN_ROUND) {
		shape_disc(list, x, y, half);
		return;
	}

	/* The normals of the two segments, and the outer side (away from where the path turns). */
	in_normal_x = -in_y;
	in_normal_y = in_x;
	out_normal_x = -out_y;
	out_normal_y = out_x;
	turn = in_normal_x * out_x + in_normal_y * out_y;
	side = 1.0;
	if (turn > 0.0)
		side = -1.0;

	/* A path that goes straight on needs no join. */
	if (turn == 0.0 && in_x * out_x + in_y * out_y > 0.0)
		return;

	/* The point and the outer corners. */
	corners[0] = x;
	corners[1] = y;
	corners[2] = x + side * half * in_normal_x;
	corners[3] = y + side * half * in_normal_y;
	corners[6] = x + side * half * out_normal_x;
	corners[7] = y + side * half * out_normal_y;

	/* The angle between the segments where they meet, in degrees. */
	angle = acos(-(in_x * out_x + in_y * out_y)) * 180.0 / 3.14159265358979323846;
	normals = in_normal_x * out_normal_x + in_normal_y * out_normal_y;

	/* A bevel, or a mitre too sharp to keep. */
	if (join == X11_JOIN_BEVEL || angle < SHAPE_MITER_LIMIT || normals <= -1.0) {
		corners[4] = corners[6];
		corners[5] = corners[7];
		x11_shape_polygon(list, corners, 3U, 0);
		return;
	}

	/* A mitre: the tip where the outer edges meet. */
	corners[4] = x + side * half * (in_normal_x + out_normal_x) / (1.0 + normals);
	corners[5] = y + side * half * (in_normal_y + out_normal_y) / (1.0 + normals);
	x11_shape_polygon(list, corners, 4U, 0);
}

/*
 * Adds a dashed wide path (distinct points), dash by dash: each piece of
 * a dash on a segment is a rectangle; the path's ends take the cap
 * style, so do both ends of each dash of an on-off dash (a double dash
 * has butt ends where its dashes meet); a point where segments meet
 * inside a dash is joined in that dash's list.
 */
static void
shape_dashed_path(
	struct x11_spans *even,
	struct x11_spans *odd,
	struct x11_dash *dash,
	const double *points,
	size_t count,
	int closed,
	double half,
	unsigned cap,
	unsigned join)
{
	struct x11_spans *list;
	double length;
	double along_x;
	double along_y;
	double done;
	double piece;
	double from_x;
	double from_y;
	double to_x;
	double to_y;
	double extend_from;
	double extend_to;
	double next_x;
	double next_y;
	double next_length;
	size_t segment;
	size_t following;
	unsigned dash_cap;
	int path_start;
	int path_end;
	int dash_start;
	int dash_end;

	/* The ends of each on-off dash take the cap style; a double dash's dashes meet butt to butt. */
	dash_cap = X11_CAP_BUTT;
	if (odd == NULL)
		dash_cap = cap;

	/* Each segment, walked through its dashes. */
	for (segment = 0U; segment + 1U < count; segment++) {
		length = sqrt((points[segment * 2U + 2U] - points[segment * 2U]) * (points[segment * 2U + 2U] - points[segment * 2U]) +
		    (points[segment * 2U + 3U] - points[segment * 2U + 1U]) * (points[segment * 2U + 3U] - points[segment * 2U + 1U]));
		along_x = (points[segment * 2U + 2U] - points[segment * 2U]) / length;
		along_y = (points[segment * 2U + 3U] - points[segment * 2U + 1U]) / length;
		done = 0.0;
		while (done < length) {
			/* The piece of the dash on this segment. */
			piece = dash->left;
			if (piece > length - done)
				piece = length - done;
			from_x = points[segment * 2U] + along_x * done;
			from_y = points[segment * 2U + 1U] + along_y * done;
			to_x = from_x + along_x * piece;
			to_y = from_y + along_y * piece;

			/* Where its ends are: the path's ends, a dash's ends, or a point where segments meet. */
			path_start = 0;
			if (!closed && segment == 0U && done == 0.0)
				path_start = 1;
			path_end = 0;
			if (!closed && segment + 2U == count && done + piece >= length)
				path_end = 1;
			dash_start = 0;
			if (done > 0.0)
				dash_start = 1;
			dash_end = 0;
			if (done + piece < length)
				dash_end = 1;

			/* Drawn in its dash's list, its ends capped where it is capped. */
			list = shape_dash_list(even, odd, dash);
			if (list != NULL) {
				extend_from = 0.0;
				extend_to = 0.0;
				if ((path_start && cap == X11_CAP_PROJECTING) || (dash_start && dash_cap == X11_CAP_PROJECTING))
					extend_from = half;
				if ((path_end && cap == X11_CAP_PROJECTING) || (dash_end && dash_cap == X11_CAP_PROJECTING))
					extend_to = half;
				shape_quad(list, from_x, from_y, to_x, to_y, half, extend_from, extend_to);
				if ((path_start && cap == X11_CAP_ROUND) || (dash_start && dash_cap == X11_CAP_ROUND))
					shape_disc(list, from_x, from_y, half);
				if ((path_end && cap == X11_CAP_ROUND) || (dash_end && dash_cap == X11_CAP_ROUND))
					shape_disc(list, to_x, to_y, half);
			}

			/* The dash moves on by the piece. */
			done += piece;
			shape_dash_step(dash, piece);
		}

		/* The join at the segment's end, in the list of the dash there. */
		following = segment + 2U;
		if (following >= count) {
			if (!closed)
				continue;
			following = 1U;
		}
		next_x = points[following * 2U] - points[segment * 2U + 2U];
		next_y = points[following * 2U + 1U] - points[segment * 2U + 3U];
		next_length = sqrt(next_x * next_x + next_y * next_y);
		list = shape_dash_list(even, odd, dash);
		if (list != NULL)
			shape_join(list, points[segment * 2U + 2U], points[segment * 2U + 3U], along_x, along_y, next_x / next_length, next_y / next_length, half, join);
	}
}

/*
 * Gives an arc's ellipse: its doubled centre and size, and the
 * directions of the arc's ends and middle (an extent going clockwise is
 * turned around to start at its other end).
 */
static void
shape_ellipse_of(
	const struct x11_arc *arc,
	struct shape_ellipse *ellipse)
{
	double start;
	double extent;
	double radians;

	/* The centre and size, doubled. */
	ellipse->centre_x = 2 * (long long)arc->x + (long long)arc->width;
	ellipse->centre_y = 2 * (long long)arc->y + (long long)arc->height;
	ellipse->width = (long long)arc->width;
	ellipse->height = (long long)arc->height;

	/* All of it, or the part from the start going counterclockwise through the extent. */
	start = (double)arc->angle1;
	extent = (double)arc->angle2;
	if (extent < 0.0) {
		start += extent;
		extent = -extent;
	}
	ellipse->full = 0;
	if (extent >= (double)SHAPE_FULL_CIRCLE)
		ellipse->full = 1;
	ellipse->over_half = 0;
	if (extent > (double)SHAPE_FULL_CIRCLE / 2.0)
		ellipse->over_half = 1;

	/*
	 * The directions of the ends and middle on the ellipse's parameter:
	 * the point at angle t is (width cos t, -height sin t) from the
	 * centre, doubled (y grows downward).
	 */
	radians = 3.14159265358979323846 / (180.0 * 64.0);
	ellipse->start_x = (double)arc->width * cos(start * radians);
	ellipse->start_y = -(double)arc->height * sin(start * radians);
	ellipse->end_x = (double)arc->width * cos((start + extent) * radians);
	ellipse->end_y = -(double)arc->height * sin((start + extent) * radians);
	ellipse->middle_x = (double)arc->width * cos((start + extent / 2.0) * radians);
	ellipse->middle_y = -(double)arc->height * sin((start + extent / 2.0) * radians);
}

/*
 * Tells whether a pixel is inside an ellipse: its doubled offset (x, y)
 * from the doubled centre, against the ellipse's doubled size.  A pixel
 * on the boundary is inside on the left half, and at the top.
 */
static int
shape_inside(
	long long x,
	long long y,
	long long width,
	long long height)
{
	long long distance;
	long long boundary;
	double far;
	double edge;

	/* A small ellipse near its pixel is tested exactly. */
	if (width <= SHAPE_EXACT_SIZE &&
	    height <= SHAPE_EXACT_SIZE &&
	    llabs(x) <= SHAPE_EXACT_OFFSET &&
	    llabs(y) <= SHAPE_EXACT_OFFSET) {
		distance = x * x * height * height + y * y * width * width;
		boundary = width * width * height * height;
		if (distance < boundary)
			return 1;
		if (distance > boundary)
			return 0;
	} else {
		far = (double)x * (double)x * (double)height * (double)height + (double)y * (double)y * (double)width * (double)width;
		edge = (double)width * (double)width * (double)height * (double)height;
		if (far < edge)
			return 1;
		if (far > edge)
			return 0;
	}

	/* On the boundary: the inside is to the right of the left half, and below the top. */
	if (x < 0)
		return 1;
	if (x == 0 && y < 0)
		return 1;

	return 0;
}

/*
 * Gives the pixels of a row inside an ellipse (doubled centre and size),
 * from left up to but not including right.  Returns nonzero when there
 * are any.
 */
static int
shape_ellipse_row(
	long long centre_x,
	long long centre_y,
	long long width,
	long long height,
	int row,
	int *left,
	int *right)
{
	double reach;
	double ratio;
	long long offset_y;
	int low;
	int high;
	int inside;

	/* A row beyond the top or bottom has none. */
	offset_y = 2 * (long long)row - centre_y;
	if (offset_y > height || offset_y < -height)
		return 0;

	/* Near where the ellipse crosses the row, doubled. */
	ratio = (double)offset_y / (double)height;
	reach = (double)width * sqrt(1.0 - ratio * ratio);
	low = shape_ceil(((double)centre_x - reach) / 2.0) - 2;
	high = shape_ceil(((double)centre_x + reach) / 2.0) + 2;

	/* The first pixel inside from the left. */
	inside = 0;
	while (low <= high) {
		inside = shape_inside(2 * (long long)low - centre_x, offset_y, width, height);
		if (inside)
			break;
		low++;
	}
	if (!inside)
		return 0;

	/* The last pixel inside from the right. */
	for (;;) {
		inside = shape_inside(2 * (long long)high - centre_x, offset_y, width, height);
		if (inside)
			break;
		high--;
	}

	/* Succeeded: the pixels between. */
	*left = low;
	*right = high + 1;
	return 1;
}

/*
 * Tells whether a pixel (its doubled offset from the centre) is in the
 * part of an ellipse an arc keeps: inside the angles of its ends (pie),
 * or on the side of the line between its ends that its middle is on
 * (chord).  A pixel on a ray or on the chord is kept when the part kept
 * is immediately to its right, or, on a horizontal one, immediately
 * below it: it is tested again moved a little right and a very little
 * down.
 */
static int
shape_kept(
	const struct shape_ellipse *ellipse,
	double point_x,
	double point_y,
	int pie)
{
	double side;
	double middle_side;
	double after_start;
	double before_end;
	int on_line;

	/* All of a full ellipse. */
	if (ellipse->full)
		return 1;

	/* A chord: the same side of the line between the ends as the middle. */
	if (!pie) {
		middle_side = shape_side(ellipse->start_x, ellipse->start_y, ellipse->end_x, ellipse->end_y, ellipse->middle_x, ellipse->middle_y);
		side = shape_side(ellipse->start_x, ellipse->start_y, ellipse->end_x, ellipse->end_y, point_x, point_y);
		on_line = shape_on_line(side, ellipse);
		if (on_line) {
			point_x += SHAPE_NUDGE;
			point_y += SHAPE_NUDGE * SHAPE_NUDGE;
			side = shape_side(ellipse->start_x, ellipse->start_y, ellipse->end_x, ellipse->end_y, point_x, point_y);
		}
		if ((side < 0.0) == (middle_side < 0.0))
			return 1;
		return 0;
	}

	/* A pie: counterclockwise from the start and clockwise from the end. */
	after_start = shape_turn(ellipse, ellipse->start_x, ellipse->start_y, point_x, point_y);
	on_line = shape_on_line(after_start, ellipse);
	if (on_line)
		after_start = shape_turn(ellipse, ellipse->start_x, ellipse->start_y, point_x + SHAPE_NUDGE, point_y + SHAPE_NUDGE * SHAPE_NUDGE);
	before_end = -shape_turn(ellipse, ellipse->end_x, ellipse->end_y, point_x, point_y);
	on_line = shape_on_line(before_end, ellipse);
	if (on_line)
		before_end = -shape_turn(ellipse, ellipse->end_x, ellipse->end_y, point_x + SHAPE_NUDGE, point_y + SHAPE_NUDGE * SHAPE_NUDGE);

	/* Within an extent of half a turn or less, both. */
	if (!ellipse->over_half) {
		if (after_start <= 0.0 && before_end <= 0.0)
			return 1;
		return 0;
	}

	/* Within more, not both of the other way. */
	if (after_start > 0.0 && before_end > 0.0)
		return 0;

	return 1;
}

/*
 * Returns which side of the line from one point to another a third point
 * is on (the cross product; 0 on the line).
 */
static double
shape_side(
	double from_x,
	double from_y,
	double to_x,
	double to_y,
	double x,
	double y)
{
	/* The cross product of the line's direction and the way to the point. */
	return (to_x - from_x) * (y - from_y) - (to_y - from_y) * (x - from_x);
}

/*
 * Returns how a point (its doubled offset from the centre) turns from the
 * direction of an end of an arc, on the ellipse's parameter (each offset
 * divided by the ellipse's size, here all multiplied by width times
 * height): below zero counterclockwise, above zero clockwise (y grows
 * downward), 0 on the end's ray or its opposite.
 */
static double
shape_turn(
	const struct shape_ellipse *ellipse,
	double end_x,
	double end_y,
	double x,
	double y)
{
	double direction_x;
	double direction_y;
	double toward_x;
	double toward_y;

	/* Both directions scaled alike. */
	direction_x = x * (double)ellipse->height;
	direction_y = y * (double)ellipse->width;
	toward_x = end_x * (double)ellipse->height;
	toward_y = end_y * (double)ellipse->width;

	/* The cross product. */
	return toward_x * direction_y - toward_y * direction_x;
}

/*
 * Tells whether a cross product is so small against the ellipse's size
 * that its point is on the line (the ends' directions are rounded).
 */
static int
shape_on_line(
	double cross,
	const struct shape_ellipse *ellipse)
{
	double scale;
	double size;

	/* The size the products are of: width times height, twice over. */
	scale = (double)ellipse->width * (double)ellipse->height;
	scale *= scale;
	size = fabs(cross);
	if (size <= scale * 1e-12)
		return 1;

	return 0;
}

/* Adds the pixels of a row from left up to right that an arc keeps, as runs. */
static void
shape_add_kept(
	struct x11_spans *list,
	const struct shape_ellipse *ellipse,
	int row,
	int left,
	int right,
	int pie)
{
	long long offset_y;
	int column;
	int start;
	int kept;

	/* Only the part inside the box. */
	if (left < list->bounds.x0)
		left = list->bounds.x0;
	if (right > list->bounds.x1)
		right = list->bounds.x1;
	if (left >= right)
		return;

	/* All of them, for a full ellipse. */
	if (ellipse->full) {
		x11_spans_add(list, row, left, right);
		return;
	}

	/* Each run of kept pixels. */
	offset_y = 2 * (long long)row - ellipse->centre_y;
	start = -1;
	for (column = left; column <= right; column++) {
		kept = 0;
		if (column < right)
			kept = shape_kept(ellipse, (double)(2 * (long long)column - ellipse->centre_x), (double)offset_y, pie);
		if (kept) {
			if (start < 0)
				start = column;
			continue;
		}
		if (start >= 0)
			x11_spans_add(list, row, start, column);
		start = -1;
	}
}

/* Adds one pixel of a thin arc when the arc keeps it. */
static void
shape_arc_point(
	struct x11_spans *list,
	const struct shape_ellipse *ellipse,
	int x,
	int y)
{
	int kept;

	/* Inside the angles of its ends. */
	kept = shape_kept(ellipse, (double)(2 * (long long)x - ellipse->centre_x), (double)(2 * (long long)y - ellipse->centre_y), 1);
	if (!kept)
		return;

	/* The pixel. */
	x11_spans_add(list, y, x, x + 1);
}

/*
 * Adds a wide arc of an ellipse that is not a circle, pixel by pixel in
 * the box around it: a pixel is drawn when its distance from the ellipse
 * is under half the width and its nearest point on the ellipse is inside
 * the angles of the arc's ends (the ends are cut square to the ellipse).
 * A pixel at just half the width is drawn when the line is immediately to
 * its right, or below it.
 */
static void
shape_wide_ellipse(
	struct x11_spans *list,
	const struct shape_ellipse *ellipse,
	const struct x11_arc *arc,
	unsigned width)
{
	double centre_x;
	double centre_y;
	double radius_x;
	double radius_y;
	double half;
	double distance;
	double edge;
	double foot_x;
	double foot_y;
	int first_row;
	int last_row;
	int first_column;
	int last_column;
	int row;
	int column;
	int start;
	int drawn;

	/* The ellipse in pixels, and the box half the width around it, inside the list's box. */
	centre_x = (double)ellipse->centre_x / 2.0;
	centre_y = (double)ellipse->centre_y / 2.0;
	radius_x = (double)arc->width / 2.0;
	radius_y = (double)arc->height / 2.0;
	half = (double)width / 2.0;
	first_row = shape_ceil(centre_y - radius_y - half) - 1;
	last_row = shape_ceil(centre_y + radius_y + half) + 1;
	first_column = shape_ceil(centre_x - radius_x - half) - 1;
	last_column = shape_ceil(centre_x + radius_x + half) + 1;
	if (first_row < list->bounds.y0)
		first_row = list->bounds.y0;
	if (last_row > list->bounds.y1)
		last_row = list->bounds.y1;
	if (first_column < list->bounds.x0)
		first_column = list->bounds.x0;
	if (last_column > list->bounds.x1)
		last_column = list->bounds.x1;

	/* Each row's runs of pixels near enough. */
	for (row = first_row; row < last_row; row++) {
		start = -1;
		for (column = first_column; column <= last_column; column++) {
			drawn = 0;
			if (column < last_column) {
				distance = shape_foot((double)column - centre_x, (double)row - centre_y, radius_x, radius_y, &foot_x, &foot_y);
				edge = fabs(distance - half);
				if (edge <= SHAPE_ON_EDGE)
					distance = shape_foot((double)column - centre_x + SHAPE_NUDGE, (double)row - centre_y + SHAPE_NUDGE * SHAPE_NUDGE, radius_x, radius_y, &foot_x, &foot_y);
				if (distance < half)
					drawn = shape_kept(ellipse, 2.0 * foot_x, 2.0 * foot_y, 1);
			}
			if (drawn) {
				if (start < 0)
					start = column;
				continue;
			}
			if (start >= 0)
				x11_spans_add(list, row, start, column);
			start = -1;
		}
	}
}

/*
 * Returns the distance from a point (its offset from an ellipse's centre)
 * to the ellipse, and gives the nearest point on it.
 *
 * Procedure referenced: David Eberly, "Distance from a Point to an
 * Ellipse, an Ellipsoid, or a Hyperellipsoid" (Geometric Tools, 2013):
 * in the first quadrant, with the longer axis first, the nearest point is
 * found by bisection on the root of the function of the Lagrange
 * multiplier; a point on the shorter axis is handled apart.
 */
static double
shape_foot(
	double x,
	double y,
	double radius_x,
	double radius_y,
	double *foot_x,
	double *foot_y)
{
	double longer;
	double shorter;
	double along;
	double across;
	double near_along;
	double near_across;
	double ratio;
	double root;
	double inside;
	double numerator;
	double denominator;
	double share;
	double distance;
	int swapped;

	/* The first quadrant, the longer axis along x. */
	swapped = 0;
	longer = radius_x;
	shorter = radius_y;
	along = fabs(x);
	across = fabs(y);
	if (radius_y > radius_x) {
		swapped = 1;
		longer = radius_y;
		shorter = radius_x;
		along = fabs(y);
		across = fabs(x);
	}

	/* Off the longer axis: the root of the multiplier's function, unless the point is on the ellipse. */
	if (across > 0.0) {
		if (along > 0.0) {
			inside = (along / longer) * (along / longer) + (across / shorter) * (across / shorter) - 1.0;
			if (inside != 0.0) {
				ratio = (longer / shorter) * (longer / shorter);
				root = shape_foot_root(ratio, along / longer, across / shorter, inside);
				near_along = ratio * along / (root + ratio);
				near_across = across / (root + 1.0);
			} else {
				near_along = along;
				near_across = across;
			}
		} else {
			near_along = 0.0;
			near_across = shorter;
		}
	} else {
		/* On the longer axis: inside the evolute's reach the nearest point is off the axis. */
		numerator = longer * along;
		denominator = longer * longer - shorter * shorter;
		if (numerator < denominator) {
			share = numerator / denominator;
			near_along = longer * share;
			near_across = shorter * sqrt(1.0 - share * share);
		} else {
			near_along = longer;
			near_across = 0.0;
		}
	}
	distance = sqrt((near_along - along) * (near_along - along) + (near_across - across) * (near_across - across));

	/* The nearest point back in the point's quadrant and axes. */
	if (x < 0.0 && !swapped)
		near_along = -near_along;
	if (y < 0.0 && !swapped)
		near_across = -near_across;
	if (y < 0.0 && swapped)
		near_along = -near_along;
	if (x < 0.0 && swapped)
		near_across = -near_across;
	*foot_x = near_along;
	*foot_y = near_across;
	if (swapped) {
		*foot_x = near_across;
		*foot_y = near_along;
	}

	/* The distance. */
	return distance;
}

/*
 * Finds by bisection the root of the multiplier's function for a point
 * scaled by the ellipse's axes (along, across), the axes' squared ratio,
 * and where the point is (inside: below zero inside the ellipse).
 */
static double
shape_foot_root(
	double ratio,
	double along,
	double across,
	double inside)
{
	double scaled;
	double low;
	double high;
	double middle;
	double first;
	double second;
	int step;

	/* The root is between these. */
	scaled = ratio * along;
	low = across - 1.0;
	high = 0.0;
	if (inside > 0.0)
		high = sqrt(scaled * scaled + across * across) - 1.0;

	/* Halved until the doubles cannot be halved further. */
	middle = low;
	for (step = 0; step < 200; step++) {
		middle = (low + high) / 2.0;
		if (middle == low || middle == high)
			break;
		first = scaled / (middle + ratio);
		second = across / (middle + 1.0);
		inside = first * first + second * second - 1.0;
		if (inside > 0.0) {
			low = middle;
		} else if (inside < 0.0) {
			high = middle;
		} else {
			break;
		}
	}

	/* The root. */
	return middle;
}
