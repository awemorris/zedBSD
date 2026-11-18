/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The stroker of libpdf's reader: the area a stroked path covers, made into
 * a path to fill, so that a program only has to fill (design-pdf.md
 * section 4.1).
 *
 * The path is flattened into polylines, dashed when the style has a dash
 * pattern, and each polyline becomes a set of small closed polygons: a
 * rectangle for each segment, a wedge for each join (round, miter or
 * bevel) and a shape for each cap (round or square).  Every polygon is
 * turned to the same orientation, so that the nonzero fill of all of them
 * is their union: overlaps are covered once, and a stroke with alpha does
 * not darken where its pieces meet.
 *
 * The work is in the path's own (user) space; the caller transforms the
 * result, so a stroke under a non-uniform scale is shaped as PDF defines.
 */

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <pdf.h>

#include "internal.h"

/* The line caps and joins of PDF's J and j operators. */
#define PDF_CAP_BUTT 0
#define PDF_CAP_ROUND 1
#define PDF_CAP_SQUARE 2
#define PDF_JOIN_MITER 0
#define PDF_JOIN_ROUND 1
#define PDF_JOIN_BEVEL 2

/* The most pieces one curve is flattened into, and the most chords of a half circle. */
#define PDF_STROKE_CURVE_PIECES 256
#define PDF_STROKE_ARC_CHORDS 128

/* The most corners one piece of the outline has: a half circle's chords, the centre and the ends. */
#define PDF_STROKE_PIECE_MAX (2 * PDF_STROKE_ARC_CHORDS + 8)

/* The most dash pattern entries one stroke walks through (a pattern of tiny entries on a long line stops there). */
#define PDF_STROKE_DASHES_MAX ((size_t)1 << 20)

/* The first capacity of the growing point arrays. */
#define PDF_STROKE_INITIAL 256

/* The value of pi, which C89's math.h does not name. */
#define PDF_STROKE_PI 3.14159265358979323846

/*
 * A growing polyline: the flattened points of one subpath.
 */
struct stroke_line {
	struct pdf_point *points;
	size_t count;
	size_t capacity;
	int closed;
	int painted;
};

/*
 * The growing outline the pieces are appended to.
 */
struct stroke_output {
	unsigned char *verbs;
	size_t verb_count;
	size_t verb_capacity;
	struct pdf_point *points;
	size_t point_count;
	size_t point_capacity;
};

/*
 * A dash pattern while a polyline is walked: the pattern, the entry in
 * force, how much of it is left, whether it is a dash (on) or a gap, and
 * how many entries the stroke has walked through (bounded).
 */
struct stroke_dash {
	const double *pattern;
	size_t count;
	size_t index;
	double left;
	int on;
	size_t entries;
};

static int flatten_subpath(const unsigned char *verbs, size_t verb_count, const struct pdf_point *points, size_t point_count, size_t *verb_index, size_t *point_index, double tolerance, struct stroke_line *line);
static int line_append(struct stroke_line *line, double x, double y);
static int flatten_cubic(struct stroke_line *line, const struct pdf_point *start, const struct pdf_point *control, double tolerance);
static int stroke_line(const struct stroke_line *line, const struct pdf_stroke_style *style, struct stroke_output *output);
static int stroke_dashed(const struct stroke_line *line, const struct pdf_stroke_style *style, struct stroke_output *output);
static int dash_segment(const struct pdf_point *from, const struct pdf_point *to, const struct pdf_stroke_style *style, struct stroke_dash *dash, struct stroke_line *dash_line, struct stroke_output *output);
static int end_dash(struct stroke_line *dash_line, const struct pdf_stroke_style *style, struct stroke_output *output);
static int same_point(const struct pdf_point *point, double x, double y);
static int stroke_open(const struct pdf_point *points, size_t count, int closed, const struct pdf_stroke_style *style, struct stroke_output *output);
static int stroke_dot(const struct pdf_point *point, const struct pdf_stroke_style *style, struct stroke_output *output);
static int emit_segment(const struct pdf_point *from, const struct pdf_point *to, double half, struct stroke_output *output);
static int emit_join(const struct pdf_point *before, const struct pdf_point *at, const struct pdf_point *after, const struct pdf_stroke_style *style, struct stroke_output *output);
static int emit_cap(const struct pdf_point *end, const struct pdf_point *inner, const struct pdf_stroke_style *style, struct stroke_output *output);
static int emit_arc(const struct pdf_point *centre, double half, double start_angle, double sweep, double tolerance, struct stroke_output *output);
static int emit_polygon(struct pdf_point *corners, size_t count, struct stroke_output *output);
static int output_reserve(struct stroke_output *output, size_t verbs, size_t points);
static void dash_start(struct stroke_dash *dash, const struct pdf_stroke_style *style);
static size_t arc_chords(double half, double sweep, double tolerance);

/*
 * Makes the outline a path covers when it is stroked with a style.
 *
 * The outline is a path of closed polygons (malloc'd verbs and points, the
 * caller frees both) to be filled by the nonzero rule.  An empty outline
 * is not an error.  ENOMEM reports memory or the point limit running out.
 */
int
pdf_stroke_path(
	const unsigned char *verbs,
	size_t verb_count,
	const struct pdf_point *points,
	size_t point_count,
	const struct pdf_stroke_style *style,
	unsigned char **out_verbs,
	size_t *out_verb_count,
	struct pdf_point **out_points,
	size_t *out_point_count)
{
	struct stroke_output output;
	struct stroke_line line;
	size_t verb_index;
	size_t point_index;
	int error;

	/* Starts with an empty outline and an empty polyline. */
	memset(&output, 0, sizeof(output));
	memset(&line, 0, sizeof(line));
	verb_index = 0;
	point_index = 0;

	/* Strokes each subpath in turn. */
	error = 0;
	while (verb_index < verb_count) {
		/* Flattens the next subpath. */
		error = flatten_subpath(verbs, verb_count, points, point_count, &verb_index, &point_index, style->tolerance, &line);
		if (error != 0)
			break;

		/* Adds its outline. */
		error = stroke_line(&line, style, &output);
		if (error != 0)
			break;
	}

	/* The subpath's points are not needed any more. */
	free(line.points);

	/* Reports a failure with nothing kept. */
	if (error != 0) {
		free(output.verbs);
		free(output.points);
		return error;
	}

	/* Succeeded: the outline, which the caller frees. */
	*out_verbs = output.verbs;
	*out_verb_count = output.verb_count;
	*out_points = output.points;
	*out_point_count = output.point_count;
	return 0;
}

/*
 * Flattens one subpath, from a move to the next move or the end, into a
 * polyline without repeated points.
 *
 * painted says whether the subpath has a segment at all (a lone move draws
 * nothing, a zero-length segment draws a dot for round and square caps).
 */
static int
flatten_subpath(
	const unsigned char *verbs,
	size_t verb_count,
	const struct pdf_point *points,
	size_t point_count,
	size_t *verb_index,
	size_t *point_index,
	double tolerance,
	struct stroke_line *line)
{
	struct pdf_point start;
	unsigned char verb;
	int error;

	/* Starts an empty polyline at the subpath's move. */
	line->count = 0;
	line->closed = 0;
	line->painted = 0;
	if (*point_index >= point_count)
		return PDF_EFORMAT;
	verb = verbs[*verb_index];
	if (verb != PDF_PATH_MOVE)
		return PDF_EFORMAT;
	error = line_append(line, points[*point_index].x, points[*point_index].y);
	if (error != 0)
		return error;
	(*verb_index)++;
	(*point_index)++;

	/* Reads the subpath's segments until the next move. */
	while (*verb_index < verb_count) {
		verb = verbs[*verb_index];
		if (verb == PDF_PATH_MOVE)
			break;
		(*verb_index)++;

		/* Adds a segment's points by the kind of segment. */
		switch (verb) {
		case PDF_PATH_LINE:
			if (*point_index + 1 > point_count)
				return PDF_EFORMAT;
			error = line_append(line, points[*point_index].x, points[*point_index].y);
			if (error != 0)
				return error;
			(*point_index)++;
			line->painted = 1;
			break;
		case PDF_PATH_CUBIC:
			if (*point_index + 3 > point_count)
				return PDF_EFORMAT;
			start = line->points[line->count - 1];
			error = flatten_cubic(line, &start, points + *point_index, tolerance);
			if (error != 0)
				return error;
			*point_index += 3;
			line->painted = 1;
			break;
		case PDF_PATH_CLOSE:
			/* A close joins the end back to the start; the subpath ends here for a stroke. */
			line->closed = 1;
			line->painted = 1;
			return 0;
		default:
			return PDF_EFORMAT;
		}
	}

	/* Succeeded: the subpath is flattened. */
	return 0;
}

/* Appends a point to a polyline, unless it repeats the last one. */
static int
line_append(
	struct stroke_line *line,
	double x,
	double y)
{
	struct pdf_point *grown;
	size_t capacity;
	int repeated;

	/* Skips a point on top of the last one. */
	if (line->count > 0) {
		repeated = same_point(&line->points[line->count - 1], x, y);
		if (repeated)
			return 0;
	}

	/* Grows the array when full, within the point limit. */
	if (line->count == line->capacity) {
		if (line->capacity >= PDF_DISPLAY_POINTS_MAX)
			return ENOMEM;
		capacity = line->capacity * 2;
		if (capacity == 0)
			capacity = PDF_STROKE_INITIAL;
		grown = realloc(line->points, capacity * sizeof(*grown));
		if (grown == NULL)
			return ENOMEM;
		line->points = grown;
		line->capacity = capacity;
	}

	/* Appends the point. */
	line->points[line->count].x = x;
	line->points[line->count].y = y;
	line->count++;

	/* Succeeded: the point ends the polyline. */
	return 0;
}

/*
 * Flattens a cubic Bezier curve into line segments within a tolerance,
 * by the number of pieces Wang's formula bounds.
 */
static int
flatten_cubic(
	struct stroke_line *line,
	const struct pdf_point *start,
	const struct pdf_point *control,
	double tolerance)
{
	double first_x;
	double first_y;
	double second_x;
	double second_y;
	double second;
	double largest;
	double t;
	double u;
	double x;
	double y;
	long pieces;
	long piece;
	int error;

	/* The larger second difference of the control points bounds the curve's bend. */
	first_x = start->x - 2.0 * control[0].x + control[1].x;
	first_y = start->y - 2.0 * control[0].y + control[1].y;
	second_x = control[0].x - 2.0 * control[1].x + control[2].x;
	second_y = control[0].y - 2.0 * control[1].y + control[2].y;
	largest = sqrt(first_x * first_x + first_y * first_y);
	second = sqrt(second_x * second_x + second_y * second_y);
	if (second > largest)
		largest = second;

	/* The pieces that keep the chords within the tolerance, between one and the limit. */
	pieces = 1;
	if (largest > 0.0 && tolerance > 0.0) {
		t = ceil(sqrt(0.75 * largest / tolerance));
		if (t > (double)PDF_STROKE_CURVE_PIECES)
			t = (double)PDF_STROKE_CURVE_PIECES;
		if (t > 1.0)
			pieces = (long)t;
	}

	/* Appends the end of each piece. */
	for (piece = 1; piece <= pieces; piece++) {
		t = (double)piece / (double)pieces;
		u = 1.0 - t;
		x = u * u * u * start->x + 3.0 * u * u * t * control[0].x + 3.0 * u * t * t * control[1].x + t * t * t * control[2].x;
		y = u * u * u * start->y + 3.0 * u * u * t * control[0].y + 3.0 * u * t * t * control[1].y + t * t * t * control[2].y;
		error = line_append(line, x, y);
		if (error != 0)
			return error;
	}

	/* Succeeded: the curve is in the polyline. */
	return 0;
}

/* Adds the outline of one flattened subpath, dashed or whole. */
static int
stroke_line(
	const struct stroke_line *line,
	const struct pdf_stroke_style *style,
	struct stroke_output *output)
{
	size_t count;
	int repeated;
	int error;

	/* A subpath without a segment draws nothing. */
	if (!line->painted)
		return 0;

	/* A subpath that went nowhere is a dot, drawn only by round and square caps. */
	if (line->count < 2) {
		error = stroke_dot(&line->points[0], style, output);
		if (error != 0)
			return error;
		return 0;
	}

	/* A dashed stroke is cut into its dashes first. */
	if (style->dash_count > 0) {
		error = stroke_dashed(line, style, output);
		if (error != 0)
			return error;
		return 0;
	}

	/* A closed subpath's last point may repeat its first, which the join then covers. */
	count = line->count;
	if (line->closed && count > 2) {
		repeated = same_point(&line->points[0], line->points[count - 1].x, line->points[count - 1].y);
		if (repeated)
			count--;
	}

	/* Strokes the whole polyline. */
	error = stroke_open(line->points, count, line->closed, style, output);
	if (error != 0)
		return error;

	/* Succeeded: the subpath's outline is added. */
	return 0;
}

/*
 * Cuts a polyline into the dashes of the style's pattern and strokes each
 * dash as an open polyline with its caps.
 */
static int
stroke_dashed(
	const struct stroke_line *line,
	const struct pdf_stroke_style *style,
	struct stroke_output *output)
{
	struct stroke_line dash_line;
	struct stroke_dash dash;
	size_t segments;
	size_t segment;
	int error;

	/* Starts the pattern at the phase, with an empty dash. */
	memset(&dash_line, 0, sizeof(dash_line));
	dash_start(&dash, style);
	segments = line->count - 1;
	if (line->closed)
		segments = line->count;

	/* Walks each segment, the closing one of a closed subpath too. */
	error = 0;
	for (segment = 0; segment < segments; segment++) {
		error = dash_segment(&line->points[segment], &line->points[(segment + 1) % line->count], style, &dash, &dash_line, output);
		if (error != 0)
			break;
	}

	/* The dash still on at the end is stroked too. */
	if (error == 0) {
		if (dash.on && dash_line.count >= 2)
			error = stroke_open(dash_line.points, dash_line.count, 0, style, output);
	}

	/* The dash's points are not needed any more; a failure is reported after them. */
	free(dash_line.points);
	if (error != 0)
		return error;

	/* Succeeded: every dash is added. */
	return 0;
}

/*
 * Walks one segment through the dash pattern: the parts under dashes join
 * the dash being gathered, and each dash that ends is stroked.
 */
static int
dash_segment(
	const struct pdf_point *from,
	const struct pdf_point *to,
	const struct pdf_stroke_style *style,
	struct stroke_dash *dash,
	struct stroke_line *dash_line,
	struct stroke_output *output)
{
	double length;
	double done;
	double step;
	double x;
	double y;
	int error;

	/* The segment's length. */
	length = sqrt((to->x - from->x) * (to->x - from->x) + (to->y - from->y) * (to->y - from->y));

	/* A dash that is on at the segment's start starts there. */
	if (dash->on && dash_line->count == 0) {
		error = line_append(dash_line, from->x, from->y);
		if (error != 0)
			return error;
	}

	/* Walks the segment through the pattern's entries. */
	done = 0.0;
	while (done < length) {
		/* Moves to the end of the entry or of the segment, whichever is first. */
		step = length - done;
		if (dash->left < step)
			step = dash->left;
		done += step;
		dash->left -= step;
		x = from->x + (to->x - from->x) * done / length;
		y = from->y + (to->y - from->y) * done / length;

		/* Extends a dash that is on. */
		if (dash->on) {
			error = line_append(dash_line, x, y);
			if (error != 0)
				return error;
		}

		/* An entry not used up goes on into the next segment. */
		if (dash->left > 1e-12)
			continue;

		/* An entry used up: a dash ends and is stroked, or a gap ends and a dash starts here. */
		if (dash->on) {
			error = end_dash(dash_line, style, output);
		} else {
			error = line_append(dash_line, x, y);
		}

		/* Reports a dash that could not be stroked or grown. */
		if (error != 0)
			return error;

		/* The next entry, which turns the dash on or off; too many entries stop the stroke. */
		dash->entries++;
		if (dash->entries > PDF_STROKE_DASHES_MAX)
			return ENOMEM;
		dash->index = (dash->index + 1) % dash->count;
		dash->left = dash->pattern[dash->index];
		dash->on = !dash->on;
	}

	/* Succeeded: the segment is walked. */
	return 0;
}

/* Strokes a dash that ended (a dot when it has no length) and empties it. */
static int
end_dash(
	struct stroke_line *dash_line,
	const struct pdf_stroke_style *style,
	struct stroke_output *output)
{
	int error;

	/* A dash with a length is an open polyline with its caps. */
	dash_line->painted = 1;
	if (dash_line->count >= 2) {
		error = stroke_open(dash_line->points, dash_line->count, 0, style, output);
		if (error != 0)
			return error;
	}

	/* A dash of no length is a dot, drawn by round and square caps. */
	if (dash_line->count == 1) {
		error = stroke_dot(&dash_line->points[0], style, output);
		if (error != 0)
			return error;
	}

	/* Succeeded: the next dash starts empty. */
	dash_line->count = 0;
	return 0;
}

/* Tells whether a point is at a place, within a rounding error. */
static int
same_point(
	const struct pdf_point *point,
	double x,
	double y)
{
	double distance_x;
	double distance_y;

	/* Apart across. */
	distance_x = fabs(point->x - x);
	if (distance_x >= 1e-9)
		return 0;

	/* Apart along. */
	distance_y = fabs(point->y - y);
	if (distance_y >= 1e-9)
		return 0;

	/* The same place. */
	return 1;
}

/*
 * Strokes a polyline of two or more points: a rectangle for each segment,
 * a join at each inner corner (every corner of a closed one), and caps at
 * the ends of an open one.
 */
static int
stroke_open(
	const struct pdf_point *points,
	size_t count,
	int closed,
	const struct pdf_stroke_style *style,
	struct stroke_output *output)
{
	size_t index;
	size_t segments;
	double half;
	int error;

	/* Nothing to stroke without a segment. */
	if (count < 2)
		return 0;

	/* The rectangle of each segment, the closing one too. */
	half = style->width / 2.0;
	segments = count - 1;
	if (closed)
		segments = count;
	for (index = 0; index < segments; index++) {
		error = emit_segment(&points[index], &points[(index + 1) % count], half, output);
		if (error != 0)
			return error;
	}

	/* The join at each inner corner. */
	for (index = 1; index + 1 < count; index++) {
		error = emit_join(&points[index - 1], &points[index], &points[index + 1], style, output);
		if (error != 0)
			return error;
	}

	/* A closed polyline also joins at its last and first corners. */
	if (closed && count > 2) {
		error = emit_join(&points[count - 2], &points[count - 1], &points[0], style, output);
		if (error != 0)
			return error;
		error = emit_join(&points[count - 1], &points[0], &points[1], style, output);
		if (error != 0)
			return error;
		return 0;
	}

	/* A closed polyline of two points has no corner but no caps either. */
	if (closed)
		return 0;

	/* The caps of an open polyline. */
	error = emit_cap(&points[0], &points[1], style, output);
	if (error != 0)
		return error;
	error = emit_cap(&points[count - 1], &points[count - 2], style, output);
	if (error != 0)
		return error;

	/* Succeeded: the polyline's outline is added. */
	return 0;
}

/* Strokes a zero-length subpath: a round cap makes a disc, a square cap a square, a butt cap nothing. */
static int
stroke_dot(
	const struct pdf_point *point,
	const struct pdf_stroke_style *style,
	struct stroke_output *output)
{
	struct pdf_point corners[4];
	double half;
	int error;

	/* The disc of a round cap. */
	half = style->width / 2.0;
	if (style->cap == PDF_CAP_ROUND) {
		error = emit_arc(point, half, 0.0, 2.0 * PDF_STROKE_PI, style->tolerance, output);
		if (error != 0)
			return error;
		return 0;
	}

	/* A butt cap draws nothing. */
	if (style->cap != PDF_CAP_SQUARE)
		return 0;

	/* The square of a square cap, upright. */
	corners[0].x = point->x - half;
	corners[0].y = point->y - half;
	corners[1].x = point->x + half;
	corners[1].y = point->y - half;
	corners[2].x = point->x + half;
	corners[2].y = point->y + half;
	corners[3].x = point->x - half;
	corners[3].y = point->y + half;
	error = emit_polygon(corners, 4, output);
	if (error != 0)
		return error;

	/* Succeeded: the dot is added. */
	return 0;
}

/* Adds the rectangle a segment covers. */
static int
emit_segment(
	const struct pdf_point *from,
	const struct pdf_point *to,
	double half,
	struct stroke_output *output)
{
	struct pdf_point corners[4];
	double length;
	double normal_x;
	double normal_y;
	int error;

	/* The segment's normal, scaled to half the width. */
	length = sqrt((to->x - from->x) * (to->x - from->x) + (to->y - from->y) * (to->y - from->y));
	if (length <= 0.0)
		return 0;
	normal_x = -(to->y - from->y) / length * half;
	normal_y = (to->x - from->x) / length * half;

	/* The four corners on both sides of the segment. */
	corners[0].x = from->x + normal_x;
	corners[0].y = from->y + normal_y;
	corners[1].x = to->x + normal_x;
	corners[1].y = to->y + normal_y;
	corners[2].x = to->x - normal_x;
	corners[2].y = to->y - normal_y;
	corners[3].x = from->x - normal_x;
	corners[3].y = from->y - normal_y;
	error = emit_polygon(corners, 4, output);
	if (error != 0)
		return error;

	/* Succeeded: the rectangle is added. */
	return 0;
}

/*
 * Adds the join at a corner, on the outer side of the turn: a wedge of a
 * circle for a round join, a triangle for a bevel, and the triangle with
 * its miter point for a miter within the miter limit.
 */
static int
emit_join(
	const struct pdf_point *before,
	const struct pdf_point *at,
	const struct pdf_point *after,
	const struct pdf_stroke_style *style,
	struct stroke_output *output)
{
	struct pdf_point corners[4];
	double in_x;
	double in_y;
	double out_x;
	double out_y;
	double length;
	double cross;
	double dot;
	double side;
	double half;
	double start_angle;
	double end_angle;
	double sweep;
	double bisector_x;
	double bisector_y;
	double bisector;
	double ratio;
	int error;

	/* The unit directions into and out of the corner. */
	length = sqrt((at->x - before->x) * (at->x - before->x) + (at->y - before->y) * (at->y - before->y));
	if (length <= 0.0)
		return 0;
	in_x = (at->x - before->x) / length;
	in_y = (at->y - before->y) / length;
	length = sqrt((after->x - at->x) * (after->x - at->x) + (after->y - at->y) * (after->y - at->y));
	if (length <= 0.0)
		return 0;
	out_x = (after->x - at->x) / length;
	out_y = (after->y - at->y) / length;

	/* A straight corner needs no join. */
	cross = in_x * out_y - in_y * out_x;
	dot = in_x * out_x + in_y * out_y;
	if (cross < 1e-12 &&
	    cross > -1e-12 &&
	    dot > 0.0)
		return 0;

	/* The outer side: the normals' side opposite the turn. */
	half = style->width / 2.0;
	side = 1.0;
	if (cross > 0.0)
		side = -1.0;

	/* A round join: the wedge of the circle between the two segments' outer corners. */
	if (style->join == PDF_JOIN_ROUND) {
		start_angle = atan2(side * in_x, -side * in_y);
		end_angle = atan2(side * out_x, -side * out_y);
		sweep = end_angle - start_angle;
		while (sweep > PDF_STROKE_PI)
			sweep -= 2.0 * PDF_STROKE_PI;
		while (sweep < -PDF_STROKE_PI)
			sweep += 2.0 * PDF_STROKE_PI;
		error = emit_arc(at, half, start_angle, sweep, style->tolerance, output);
		if (error != 0)
			return error;
		return 0;
	}

	/* The bevel's triangle: the corner and the two outer corners of the segments. */
	corners[0] = *at;
	corners[1].x = at->x - side * in_y * half;
	corners[1].y = at->y + side * in_x * half;
	corners[3].x = at->x - side * out_y * half;
	corners[3].y = at->y + side * out_x * half;

	/* A miter adds the point where the outer edges meet, when it is within the limit. */
	bisector_x = -side * in_y - side * out_y;
	bisector_y = side * in_x + side * out_x;
	bisector = sqrt(bisector_x * bisector_x + bisector_y * bisector_y);
	ratio = 0.0;
	if (bisector > 1e-12)
		ratio = 2.0 / bisector;
	if (style->join == PDF_JOIN_MITER &&
	    bisector > 1e-12 &&
	    ratio <= style->miter_limit) {
		corners[2].x = at->x + bisector_x / bisector * half * ratio;
		corners[2].y = at->y + bisector_y / bisector * half * ratio;
		error = emit_polygon(corners, 4, output);
		if (error != 0)
			return error;
		return 0;
	}

	/* The bevel. */
	corners[2] = corners[3];
	error = emit_polygon(corners, 3, output);
	if (error != 0)
		return error;

	/* Succeeded: the join is added. */
	return 0;
}

/* Adds the cap at an end of an open polyline, whose neighbour gives its direction. */
static int
emit_cap(
	const struct pdf_point *end,
	const struct pdf_point *inner,
	const struct pdf_stroke_style *style,
	struct stroke_output *output)
{
	struct pdf_point corners[4];
	double length;
	double out_x;
	double out_y;
	double half;
	int error;

	/* A round cap is the disc at the end. */
	half = style->width / 2.0;
	if (style->cap == PDF_CAP_ROUND) {
		error = emit_arc(end, half, 0.0, 2.0 * PDF_STROKE_PI, style->tolerance, output);
		if (error != 0)
			return error;
		return 0;
	}

	/* A butt cap adds nothing. */
	if (style->cap != PDF_CAP_SQUARE)
		return 0;

	/* The direction out of the line at the end. */
	length = sqrt((end->x - inner->x) * (end->x - inner->x) + (end->y - inner->y) * (end->y - inner->y));
	if (length <= 0.0)
		return 0;
	out_x = (end->x - inner->x) / length * half;
	out_y = (end->y - inner->y) / length * half;

	/* A square cap extends the line by half its width. */
	corners[0].x = end->x - out_y;
	corners[0].y = end->y + out_x;
	corners[1].x = end->x - out_y + out_x;
	corners[1].y = end->y + out_x + out_y;
	corners[2].x = end->x + out_y + out_x;
	corners[2].y = end->y - out_x + out_y;
	corners[3].x = end->x + out_y;
	corners[3].y = end->y - out_x;
	error = emit_polygon(corners, 4, output);
	if (error != 0)
		return error;

	/* Succeeded: the cap is added. */
	return 0;
}

/*
 * Adds a sector of a circle: a whole disc for a sweep of a full turn, or
 * the wedge from the centre for a smaller one.
 */
static int
emit_arc(
	const struct pdf_point *centre,
	double half,
	double start_angle,
	double sweep,
	double tolerance,
	struct stroke_output *output)
{
	struct pdf_point corners[PDF_STROKE_PIECE_MAX];
	size_t chords;
	size_t count;
	size_t index;
	double angle;
	int whole;
	int error;

	/* A disc has no centre corner; a wedge starts at the centre. */
	count = 0;
	whole = 0;
	if (sweep >= 2.0 * PDF_STROKE_PI - 1e-9 || sweep <= -2.0 * PDF_STROKE_PI + 1e-9)
		whole = 1;
	if (!whole) {
		corners[count] = *centre;
		count++;
	}

	/* The corners along the arc. */
	chords = arc_chords(half, sweep, tolerance);
	for (index = 0; index <= chords; index++) {
		/* A disc does not repeat its first corner. */
		if (whole && index == chords)
			break;
		angle = start_angle + sweep * (double)index / (double)chords;
		corners[count].x = centre->x + cos(angle) * half;
		corners[count].y = centre->y + sin(angle) * half;
		count++;
	}

	/* Adds the sector. */
	error = emit_polygon(corners, count, output);
	if (error != 0)
		return error;

	/* Succeeded: the sector is added. */
	return 0;
}

/*
 * Adds a closed polygon in the common orientation (a positive signed
 * area), reversing it when it runs the other way.
 */
static int
emit_polygon(
	struct pdf_point *corners,
	size_t count,
	struct stroke_output *output)
{
	struct pdf_point swap;
	double area;
	size_t index;
	size_t next;
	int error;

	/* A polygon needs three corners. */
	if (count < 3)
		return 0;

	/* The signed area by the shoelace formula. */
	area = 0.0;
	for (index = 0; index < count; index++) {
		next = (index + 1) % count;
		area += corners[index].x * corners[next].y - corners[next].x * corners[index].y;
	}

	/* A flat polygon covers nothing. */
	if (area == 0.0)
		return 0;

	/* Reverses a polygon that runs the other way. */
	if (area < 0.0) {
		for (index = 0; index < count / 2; index++) {
			swap = corners[index];
			corners[index] = corners[count - 1 - index];
			corners[count - 1 - index] = swap;
		}
	}

	/* Makes room for its move, lines and close. */
	error = output_reserve(output, count + 1, count);
	if (error != 0)
		return error;

	/* Appends the polygon. */
	for (index = 0; index < count; index++) {
		output->verbs[output->verb_count] = PDF_PATH_LINE;
		if (index == 0)
			output->verbs[output->verb_count] = PDF_PATH_MOVE;
		output->verb_count++;
		output->points[output->point_count] = corners[index];
		output->point_count++;
	}

	/* The polygon is closed. */
	output->verbs[output->verb_count] = PDF_PATH_CLOSE;
	output->verb_count++;

	/* Succeeded: the polygon is part of the outline. */
	return 0;
}

/* Makes room for more verbs and points in the outline, within the point limit. */
static int
output_reserve(
	struct stroke_output *output,
	size_t verbs,
	size_t points)
{
	unsigned char *grown_verbs;
	struct pdf_point *grown_points;
	size_t capacity;

	/* Refuses an outline past the limit. */
	if (verbs > PDF_DISPLAY_POINTS_MAX - output->verb_count)
		return ENOMEM;
	if (points > PDF_DISPLAY_POINTS_MAX - output->point_count)
		return ENOMEM;

	/* Grows the verbs when they do not fit. */
	if (output->verb_count + verbs > output->verb_capacity) {
		capacity = output->verb_capacity * 2;
		if (capacity < PDF_STROKE_INITIAL)
			capacity = PDF_STROKE_INITIAL;
		while (capacity < output->verb_count + verbs)
			capacity *= 2;
		grown_verbs = realloc(output->verbs, capacity);
		if (grown_verbs == NULL)
			return ENOMEM;
		output->verbs = grown_verbs;
		output->verb_capacity = capacity;
	}

	/* Grows the points when they do not fit. */
	if (output->point_count + points > output->point_capacity) {
		capacity = output->point_capacity * 2;
		if (capacity < PDF_STROKE_INITIAL)
			capacity = PDF_STROKE_INITIAL;
		while (capacity < output->point_count + points)
			capacity *= 2;
		grown_points = realloc(output->points, capacity * sizeof(*grown_points));
		if (grown_points == NULL)
			return ENOMEM;
		output->points = grown_points;
		output->point_capacity = capacity;
	}

	/* Succeeded: the room is there. */
	return 0;
}

/*
 * Starts a dash pattern at its phase: the entry the phase falls in, what
 * is left of it, and whether it is a dash.
 */
static void
dash_start(
	struct stroke_dash *dash,
	const struct pdf_stroke_style *style)
{
	double phase;

	/* The pattern starts with a dash. */
	dash->pattern = style->dash;
	dash->count = style->dash_count;
	dash->index = 0;
	dash->left = dash->pattern[0];
	dash->on = 1;
	dash->entries = 0;

	/* Walks the phase through the pattern (the caller made the pattern's total positive and the phase within it). */
	phase = style->dash_phase;
	while (phase > 0.0) {
		if (phase < dash->left) {
			dash->left -= phase;
			break;
		}

		/* The phase passes the whole entry: the next entry starts. */
		phase -= dash->left;
		dash->index = (dash->index + 1) % dash->count;
		dash->left = dash->pattern[dash->index];
		dash->on = !dash->on;
	}
}

/* Chooses the chords of an arc that keep it within the tolerance, between two and the limit per half turn. */
static size_t
arc_chords(
	double half,
	double sweep,
	double tolerance)
{
	double step;
	double chords;

	/* The largest angle whose chord stays within the tolerance of the circle. */
	step = PDF_STROKE_PI / 2.0;
	if (tolerance < half)
		step = 2.0 * acos(1.0 - tolerance / half);
	if (step < PDF_STROKE_PI / (double)PDF_STROKE_ARC_CHORDS)
		step = PDF_STROKE_PI / (double)PDF_STROKE_ARC_CHORDS;

	/* The chords the sweep needs, at least two. */
	chords = ceil(fabs(sweep) / step);
	if (chords < 2.0)
		chords = 2.0;
	if (chords > (double)(2 * PDF_STROKE_ARC_CHORDS))
		chords = (double)(2 * PDF_STROKE_ARC_CHORDS);

	/* Reports the count. */
	return (size_t)chords;
}
