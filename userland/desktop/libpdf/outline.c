/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The outline of a pressure-sensitive pen stroke.
 *
 * A stroke whose width follows the pen's pressure cannot be drawn with a PDF
 * line width, which is constant along a path.  Notes therefore fills an
 * outline, and it fills the same polygon on the screen, so the page and the
 * saved PDF have the same shape (plan/ws079/design-pdf.md section 1).
 *
 * The samples are first joined by a centripetal Catmull-Rom spline, which
 * passes through every sample without the loops and cusps of the uniform
 * one, and the spline is cut into pieces short enough to stay within a
 * tolerance of the curve.  The pressure is interpolated along each piece.
 * The outline then walks the left edge forward, turns around the last point
 * with a round cap, walks the right edge back and turns around the first
 * point.  Each edge is offset from the centerline by half the width there.
 *
 * At a corner the outer edge follows an arc around the corner point (a
 * round join), and the inner edge passes through the corner point itself.
 * Filled with the nonzero rule, the polygon is then exactly the union of the
 * pieces' trapezoids, the joins' circular sectors and the two caps, all
 * turning the same way, so a stroke never narrows at a corner and never
 * shows a notch however sharply it turns.
 *
 * ws216-p002: the width may instead follow a factor the caller gives each
 * point (pdf_outline_stroke_style(), the brush, pencil and watercolour of
 * Notes), mapped by a style rather than by the pen's pressure curve, and a
 * stroke may be cut into ranges (pdf_outline_stroke_ranges(), the pencil's
 * steps of darkness).  A range ends at a gentle point of the centerline on
 * a flat cut along the point's middle normal, which the next range starts
 * from with the same two corners, so the ranges meet without a gap.
 */

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <pdf.h>

/* The largest coordinate and width the outline accepts, the writer's own limit. */
#define PDF_OUTLINE_COORDINATE_LIMIT 1000000.0

/* The farthest a spline piece or an arc's chord may stray from the true curve, in points. */
#define PDF_OUTLINE_TOLERANCE 0.05

/* The fewest chords of a half-circle cap, and the most chords of any half turn. */
#define PDF_OUTLINE_CAP_MIN 2
#define PDF_OUTLINE_CAP_MAX 64

/* The most pieces one spline interval between two samples is cut into. */
#define PDF_OUTLINE_PIECES_MAX 256

/* The most corners one outline may have, which bounds its memory to 256 MiB. */
#define PDF_OUTLINE_CORNERS_MAX 16777216

/* Points closer than this to the previous kept point add nothing to the outline. */
#define PDF_OUTLINE_MIN_STEP 0.001

/* The share of the full width that the lightest touch still draws. */
#define PDF_OUTLINE_WIDTH_FLOOR 0.15

/* The exponent of the pressure curve, which widens light strokes faster than a straight line. */
#define PDF_OUTLINE_PRESSURE_EXPONENT 0.7

/* The largest exponent and factor limit a style may give. */
#define PDF_OUTLINE_STYLE_LIMIT 16.0

/*
 * How far around a range's cut the centerline must be gentle, in the cut
 * point's half widths, and how much it may turn there in all, in radians.
 */
#define PDF_OUTLINE_SEAM_REACH 2.0
#define PDF_OUTLINE_SEAM_TURN 0.35

/* Pi, which C89's math.h does not name. */
#define PDF_OUTLINE_PI 3.14159265358979323846

/*
 * How a factor of the stroke becomes a half width.
 *
 * The pen's pressure is a factor of 0..1 mapped by the pressure curve; the
 * caller's own factors (pdf_outline_stroke_style()) are mapped by the
 * caller's style.  It lives only while one outline is being built.
 */
struct pdf_outline_shape {
	double floor;
	double exponent;
	double factor_max;
	double radius_min;
};

/*
 * One kept sample of the stroke.
 *
 * The factor (the pen's pressure) is clamped to the shape's range.  vertex
 * is the centerline point the sample became, set while the centerline is
 * built.  It lives only while one outline is being built.
 */
struct pdf_outline_sample {
	double x;
	double y;
	double factor;
	size_t vertex;
};

/*
 * One point of the smoothed centerline with the half width drawn there.
 *
 * It lives only while one outline is being built.
 */
struct pdf_outline_vertex {
	double x;
	double y;
	double radius;
};

/*
 * The smoothed centerline being built.
 *
 * error holds the first failure of an append; later appends then do
 * nothing, so the builder is checked once, when the centerline is done.
 */
struct pdf_outline_path {
	struct pdf_outline_vertex *vertices;
	size_t count;
	size_t capacity;
	int error;
};

/*
 * The outline polygon being built, which becomes the caller's.
 *
 * error works as in the centerline builder.
 */
struct pdf_outline_polygon {
	struct pdf_point *corners;
	size_t count;
	size_t capacity;
	int error;
};

/*
 * The shape of the centerline at one inner point.
 *
 * before and after are the unit left normals of the segments that arrive at
 * and leave the point, and turn is the signed angle between them.  A gentle
 * turn is one whose round join would be a single chord; its two edges then
 * meet in one corner on middle, the unit normal halfway between the two.
 */
struct pdf_outline_join {
	double before_x;
	double before_y;
	double after_x;
	double after_y;
	double middle_x;
	double middle_y;
	double turn;
	int gentle;
};

/*
 * The cubic Bezier form of one spline interval, and the four factors (the
 * pen's pressures) around it that the factor is interpolated from.
 */
struct pdf_outline_piece {
	double start_x;
	double start_y;
	double control1_x;
	double control1_y;
	double control2_x;
	double control2_y;
	double end_x;
	double end_y;
	double factor_before;
	double factor_start;
	double factor_end;
	double factor_after;
};

/*
 * The samples of one stroke, kept, and the shape that widens them.
 *
 * kept_of says, for each of the caller's points, which kept sample it
 * became (NULL when no one asks).  It lives only while one outline is
 * being built.
 */
struct pdf_outline_stroke {
	struct pdf_outline_sample *samples;
	size_t count;
	size_t *kept_of;
	struct pdf_outline_shape shape;
};

/*
 * One range of the centerline being outlined: its first and last vertex.
 * A range that starts or ends inside the centerline is cut there flat;
 * one at an end of the centerline has a round cap.
 */
struct pdf_outline_span {
	size_t first;
	size_t last;
};

static int check_style(const struct pdf_outline_style *style);
static int prepare_stroke(struct pdf_outline_stroke *stroke, size_t count, double width, int map_points);
static int keep_sample(struct pdf_outline_stroke *stroke, size_t index, double x, double y, double factor);
static int check_value(double value);
static double shape_radius(const struct pdf_outline_shape *shape, double width, double factor);
static int outline_whole(struct pdf_outline_stroke *stroke, double width, struct pdf_point **outline, size_t *outline_count);
static int build_path(struct pdf_outline_stroke *stroke, double width, struct pdf_outline_path *path);
static void build_centerline(struct pdf_outline_stroke *stroke, double width, struct pdf_outline_path *path);
static void spline_interval(const struct pdf_outline_sample *samples, size_t count, size_t index, struct pdf_outline_piece *piece);
static double knot_distance(double from_x, double from_y, double to_x, double to_y);
static size_t interval_pieces(const struct pdf_outline_piece *piece);
static double interpolate_factor(const struct pdf_outline_piece *piece, double t);
static void path_append(struct pdf_outline_path *path, double x, double y, double radius);
static int choose_cuts(const struct pdf_outline_stroke *stroke, const struct pdf_outline_path *path, const size_t *cuts, size_t cut_count, size_t *boundaries, size_t *starts, size_t *chosen);
static int measure_arcs(const struct pdf_outline_path *path, double **arcs);
static int seam_usable(const struct pdf_outline_path *path, const double *arcs, size_t index);
static void build_range(const struct pdf_outline_path *path, const struct pdf_outline_span *span, struct pdf_outline_polygon *polygon, size_t *seam_end);
static void measure_join(const struct pdf_outline_vertex *vertices, size_t index, struct pdf_outline_join *join);
static double turn_angle(const struct pdf_outline_vertex *before, const struct pdf_outline_vertex *corner, const struct pdf_outline_vertex *after);
static void segment_normal(const struct pdf_outline_vertex *from, const struct pdf_outline_vertex *to, double *normal_x, double *normal_y);
static void build_dot(const struct pdf_outline_vertex *center, struct pdf_outline_polygon *polygon);
static double arc_step(double radius);
static size_t arc_chords(double radius, double angle, size_t fewest);
static void append_arc(struct pdf_outline_polygon *polygon, const struct pdf_outline_vertex *center, double start_angle, double sweep, size_t chords);
static void polygon_append(struct pdf_outline_polygon *polygon, double x, double y);

/*
 * Builds the pressure-width outline polygon of a pen stroke.
 *
 * The stroke passes smoothly through every sample.  The width at a point is
 * width * (0.15 + 0.85 * pressure^0.7), so the lightest touch still draws 15%
 * of the full width, and the pressure between samples is interpolated.  Both
 * ends and every corner are round.  A stroke that does not move is a round
 * dot.  The polygon is to be filled with the nonzero rule.  The caller frees
 * it with pdf_outline_free().  The same points always give the same polygon.
 */
int
pdf_outline_stroke(
	const struct pdf_stroke_point *points,
	size_t count,
	double width,
	struct pdf_point **outline,
	size_t *outline_count)
{
	struct pdf_outline_stroke stroke;
	size_t index;
	int error;

	/* Refuses a stroke without points. */
	if (points == NULL)
		return EINVAL;

	/* The pen's pressure is a factor of 0..1 widened by the pressure curve. */
	memset(&stroke, 0, sizeof(stroke));
	stroke.shape.floor = PDF_OUTLINE_WIDTH_FLOOR;
	stroke.shape.exponent = PDF_OUTLINE_PRESSURE_EXPONENT;
	stroke.shape.factor_max = 1.0;
	stroke.shape.radius_min = 0.0;

	/* Checks the stroke's size and width and allocates its kept samples. */
	error = prepare_stroke(&stroke, count, width, 0);
	if (error != 0)
		return error;

	/* Keeps each point that moved away from the previous kept one. */
	for (index = 0; index < count; index++) {
		error = keep_sample(&stroke, index, points[index].x, points[index].y, points[index].pressure);
		if (error != 0) {
			free(stroke.samples);
			return error;
		}
	}

	/* Walks around the whole centerline. */
	error = outline_whole(&stroke, width, outline, outline_count);
	free(stroke.samples);
	if (error != 0)
		return error;

	/* Succeeded: the caller owns the closed polygon. */
	return 0;
}

/*
 * Builds the outline polygon of a stroke whose width follows a factor the
 * caller gives each point (ws216-p002: Notes' brush and watercolour).
 *
 * The half width at a point is width / 2 * (floor + (1 - floor) *
 * factor^exponent), the factor clamped to 0..factor_max and interpolated
 * between the points as the pen's pressure is, and never less than
 * radius_min.  The polygon is drawn as pdf_outline_stroke() draws it, and
 * the caller frees it with pdf_outline_free().
 */
int
pdf_outline_stroke_style(
	const struct pdf_factor_point *points,
	size_t count,
	double width,
	const struct pdf_outline_style *style,
	struct pdf_point **outline,
	size_t *outline_count)
{
	struct pdf_outline_stroke stroke;
	size_t index;
	int error;

	/* Refuses a stroke without points, and a style that cannot widen one. */
	if (points == NULL)
		return EINVAL;
	error = check_style(style);
	if (error != 0)
		return error;

	/* The caller's style widens the factors. */
	memset(&stroke, 0, sizeof(stroke));
	stroke.shape.floor = style->floor;
	stroke.shape.exponent = style->exponent;
	stroke.shape.factor_max = style->factor_max;
	stroke.shape.radius_min = style->radius_min;

	/* Checks the stroke's size and width and allocates its kept samples. */
	error = prepare_stroke(&stroke, count, width, 0);
	if (error != 0)
		return error;

	/* Keeps each point that moved away from the previous kept one. */
	for (index = 0; index < count; index++) {
		error = keep_sample(&stroke, index, points[index].x, points[index].y, points[index].factor);
		if (error != 0) {
			free(stroke.samples);
			return error;
		}
	}

	/* Walks around the whole centerline. */
	error = outline_whole(&stroke, width, outline, outline_count);
	free(stroke.samples);
	if (error != 0)
		return error;

	/* Succeeded: the caller owns the closed polygon. */
	return 0;
}

/*
 * Builds the outlines of a stroke cut into ranges (ws216-p002: the steps of
 * Notes' pencil).
 *
 * The stroke is widened as pdf_outline_stroke_style() widens it.  cuts are
 * indices of points, rising, each from 1 to count - 1: a range is to start
 * at each.  A cut moves on along the stroke, toward its end, to the first
 * point of the centerline where the stroke is gentle enough to be cut flat
 * (within twice the half width around it nothing turns sharply, nor much in
 * all), before the next cut's point; a cut with no such point is left out.
 * Each range's start is the point whose cut began it (0 for the first),
 * its corners its outline (nonzero), and seam_start and seam_end the edges
 * where it meets the range before and after (an edge is named by its first
 * corner and goes to the next one; PDF_OUTLINE_NO_SEAM at the stroke's
 * ends, which are round).  Two ranges that meet share the two corners of
 * their cut exactly.  The caller frees the ranges with
 * pdf_outline_ranges_free().
 */
int
pdf_outline_stroke_ranges(
	const struct pdf_factor_point *points,
	size_t count,
	double width,
	const struct pdf_outline_style *style,
	const size_t *cuts,
	size_t cut_count,
	struct pdf_outline_range **ranges,
	size_t *range_count)
{
	struct pdf_outline_stroke stroke;
	struct pdf_outline_path path;
	struct pdf_outline_polygon polygon;
	struct pdf_outline_range *made;
	struct pdf_outline_span span;
	size_t *boundaries;
	size_t *starts;
	size_t chosen;
	size_t index;
	size_t seam_end;
	int error;

	/* Refuses a stroke without points, and a style that cannot widen one. */
	if (points == NULL)
		return EINVAL;
	error = check_style(style);
	if (error != 0)
		return error;

	/* Refuses cuts that are missing, out of order or outside the inner points. */
	if (cut_count != 0 && cuts == NULL)
		return EINVAL;
	for (index = 0; index < cut_count; index++) {
		if (cuts[index] == 0 || cuts[index] >= count)
			return EINVAL;
		if (index > 0 && cuts[index] <= cuts[index - 1])
			return EINVAL;
	}

	/* The caller's style widens the factors. */
	memset(&stroke, 0, sizeof(stroke));
	stroke.shape.floor = style->floor;
	stroke.shape.exponent = style->exponent;
	stroke.shape.factor_max = style->factor_max;
	stroke.shape.radius_min = style->radius_min;

	/* Checks the stroke's size and width and allocates its kept samples and the map of points to them. */
	error = prepare_stroke(&stroke, count, width, 1);
	if (error != 0)
		return error;

	/* Keeps each point that moved away from the previous kept one. */
	for (index = 0; index < count; index++) {
		error = keep_sample(&stroke, index, points[index].x, points[index].y, points[index].factor);
		if (error != 0) {
			free(stroke.samples);
			free(stroke.kept_of);
			return error;
		}
	}

	/* Joins the samples into the centerline. */
	error = build_path(&stroke, width, &path);
	if (error != 0) {
		free(stroke.samples);
		free(stroke.kept_of);
		return error;
	}

	/* Allocates the cuts taken: one more range than cuts at most. */
	boundaries = malloc((cut_count + 1) * sizeof(*boundaries));
	if (boundaries == NULL) {
		free(path.vertices);
		free(stroke.samples);
		free(stroke.kept_of);
		return ENOMEM;
	}
	starts = malloc((cut_count + 1) * sizeof(*starts));
	if (starts == NULL) {
		free(boundaries);
		free(path.vertices);
		free(stroke.samples);
		free(stroke.kept_of);
		return ENOMEM;
	}

	/* Moves each cut to a point the stroke can be cut at; a stroke that never moved is one range. */
	chosen = 0;
	error = 0;
	if (path.count > 1)
		error = choose_cuts(&stroke, &path, cuts, cut_count, boundaries, starts, &chosen);
	free(stroke.samples);
	free(stroke.kept_of);
	if (error != 0) {
		free(starts);
		free(boundaries);
		free(path.vertices);
		return error;
	}

	/* Allocates the ranges, every one empty. */
	made = calloc(chosen + 1, sizeof(*made));
	if (made == NULL) {
		free(starts);
		free(boundaries);
		free(path.vertices);
		return ENOMEM;
	}

	/* Walks around each range, from the start of the centerline or a cut to the next cut or its end. */
	for (index = 0; index <= chosen; index++) {
		/* The range's vertices and the point that decides its step. */
		span.first = 0;
		made[index].start = 0;
		if (index > 0) {
			span.first = boundaries[index - 1];
			made[index].start = starts[index - 1];
		}
		span.last = path.count - 1;
		if (index < chosen)
			span.last = boundaries[index];

		/* A stroke that never moved is a dot without seams; the others are outlined between their cuts. */
		memset(&polygon, 0, sizeof(polygon));
		seam_end = PDF_OUTLINE_NO_SEAM;
		if (path.count == 1) {
			build_dot(&path.vertices[0], &polygon);
		} else {
			build_range(&path, &span, &polygon, &seam_end);
		}

		/* A range that could not be built fails the whole stroke. */
		if (polygon.error != 0) {
			free(polygon.corners);
			pdf_outline_ranges_free(made, index);
			free(starts);
			free(boundaries);
			free(path.vertices);
			return polygon.error;
		}

		/* The range's outline and its seams: its closing edge when it starts at a cut. */
		made[index].corners = polygon.corners;
		made[index].count = polygon.count;
		made[index].seam_start = PDF_OUTLINE_NO_SEAM;
		if (span.first > 0)
			made[index].seam_start = polygon.count - 1;
		made[index].seam_end = seam_end;
	}

	/* The centerline and the cuts are not needed any more. */
	free(starts);
	free(boundaries);
	free(path.vertices);

	/* Succeeded: the caller owns the ranges. */
	*ranges = made;
	*range_count = chosen + 1;
	return 0;
}

/*
 * Frees an outline polygon that pdf_outline_stroke() made.
 */
void
pdf_outline_free(
	struct pdf_point *outline)
{
	/* Releases the polygon's allocation. */
	free(outline);
}

/*
 * Frees the ranges that pdf_outline_stroke_ranges() made.
 */
void
pdf_outline_ranges_free(
	struct pdf_outline_range *ranges,
	size_t count)
{
	size_t index;

	/* Nothing was made. */
	if (ranges == NULL)
		return;

	/* Releases each range's polygon, then the ranges. */
	for (index = 0; index < count; index++)
		free(ranges[index].corners);
	free(ranges);
}

/* Reports whether a style can widen a stroke: every number finite and within its range. */
static int
check_style(
	const struct pdf_outline_style *style)
{
	/* Refuses a missing style. */
	if (style == NULL)
		return EINVAL;

	/* The floor is a share of the full width; the negations also refuse a NaN. */
	if (!(style->floor >= 0.0 && style->floor <= 1.0))
		return EINVAL;

	/* The exponent and the factor's limit are positive and moderate. */
	if (!(style->exponent > 0.0 && style->exponent <= PDF_OUTLINE_STYLE_LIMIT))
		return EINVAL;
	if (!(style->factor_max > 0.0 && style->factor_max <= PDF_OUTLINE_STYLE_LIMIT))
		return EINVAL;

	/* The smallest half width is not negative and not past the coordinates' limit. */
	if (!(style->radius_min >= 0.0 && style->radius_min <= PDF_OUTLINE_COORDINATE_LIMIT))
		return EINVAL;

	/* Succeeded: the style can be used. */
	return 0;
}

/*
 * Checks a stroke's size and width and allocates its kept samples, and,
 * when map_points is set, the map of the caller's points to them.
 */
static int
prepare_stroke(
	struct pdf_outline_stroke *stroke,
	size_t count,
	double width,
	int map_points)
{
	/* Refuses a stroke without points. */
	if (count == 0)
		return EINVAL;

	/* Refuses a width without size or past the limit; the negation also refuses a NaN. */
	if (!(width > 0.0 && width <= PDF_OUTLINE_COORDINATE_LIMIT))
		return EINVAL;

	/* Refuses a stroke longer than one outline may be. */
	if (count > PDF_OUTLINE_CORNERS_MAX)
		return EINVAL;

	/* Allocates the kept samples. */
	stroke->samples = malloc(count * sizeof(*stroke->samples));
	if (stroke->samples == NULL)
		return ENOMEM;
	stroke->count = 0;

	/* Allocates the map of the points to the samples, when the ranges need it. */
	stroke->kept_of = NULL;
	if (map_points) {
		stroke->kept_of = malloc(count * sizeof(*stroke->kept_of));
		if (stroke->kept_of == NULL) {
			free(stroke->samples);
			stroke->samples = NULL;
			return ENOMEM;
		}
	}

	/* Succeeded: the stroke takes its samples. */
	return 0;
}

/*
 * Checks one of the caller's points and keeps it when it moved away from
 * the previous kept one.
 *
 * A point that has not moved is merged into the previous one, which keeps
 * the larger of the two factors (the harder of two pressures).
 */
static int
keep_sample(
	struct pdf_outline_stroke *stroke,
	size_t index,
	double x,
	double y,
	double factor)
{
	struct pdf_outline_sample *last;
	double step_x;
	double step_y;
	int error;

	/* Refuses an x that cannot be drawn. */
	error = check_value(x);
	if (error != 0)
		return error;

	/* Refuses a y that cannot be drawn. */
	error = check_value(y);
	if (error != 0)
		return error;

	/* Refuses a factor that is not a number, and clamps the rest to the shape's range. */
	if (factor != factor)
		return EINVAL;
	if (factor < 0.0)
		factor = 0.0;
	if (factor > stroke->shape.factor_max)
		factor = stroke->shape.factor_max;

	/* Merges a point that has not moved away from the previous kept one into it. */
	if (stroke->count != 0) {
		last = &stroke->samples[stroke->count - 1];
		step_x = x - last->x;
		step_y = y - last->y;
		if (step_x * step_x + step_y * step_y < PDF_OUTLINE_MIN_STEP * PDF_OUTLINE_MIN_STEP) {
			if (factor > last->factor)
				last->factor = factor;
			if (stroke->kept_of != NULL)
				stroke->kept_of[index] = stroke->count - 1;
			return 0;
		}
	}

	/* Keeps the point, and notes which sample the caller's point became. */
	stroke->samples[stroke->count].x = x;
	stroke->samples[stroke->count].y = y;
	stroke->samples[stroke->count].factor = factor;
	stroke->samples[stroke->count].vertex = 0;
	if (stroke->kept_of != NULL)
		stroke->kept_of[index] = stroke->count;
	stroke->count++;

	/* Succeeded: the point is kept or merged. */
	return 0;
}

/* Reports whether a coordinate is finite and inside the limit. */
static int
check_value(
	double value)
{
	/* Refuses a NaN and anything past the limit on either side. */
	if (!(value >= -PDF_OUTLINE_COORDINATE_LIMIT && value <= PDF_OUTLINE_COORDINATE_LIMIT))
		return EINVAL;

	/* Succeeded: the coordinate can be drawn. */
	return 0;
}

/*
 * Computes the half width a shape gives a factor.
 *
 * For the pen the arithmetic is the pressure curve's, in the same order,
 * so the pen's outline does not change by a bit.
 */
static double
shape_radius(
	const struct pdf_outline_shape *shape,
	double width,
	double factor)
{
	double share;
	double radius;

	/* Maps the factor onto the share of the full width that it draws. */
	share = shape->floor + (1.0 - shape->floor) * pow(factor, shape->exponent);

	/* Half of that width, the distance of each edge from the point, but no less than the shape's least. */
	radius = width * share / 2.0;
	if (radius < shape->radius_min)
		radius = shape->radius_min;

	/* Reports the half width. */
	return radius;
}

/* Builds the outline of a whole stroke: its centerline walked around, or a dot. */
static int
outline_whole(
	struct pdf_outline_stroke *stroke,
	double width,
	struct pdf_point **outline,
	size_t *outline_count)
{
	struct pdf_outline_path path;
	struct pdf_outline_polygon polygon;
	struct pdf_outline_span span;
	size_t seam_end;
	int error;

	/* Joins the samples with the spline, cut into short pieces with their half widths. */
	error = build_path(stroke, width, &path);
	if (error != 0)
		return error;

	/* Walks around the centerline from its first point to its last; a stroke that never moved is a dot. */
	memset(&polygon, 0, sizeof(polygon));
	if (path.count == 1) {
		build_dot(&path.vertices[0], &polygon);
	} else {
		span.first = 0;
		span.last = path.count - 1;
		build_range(&path, &span, &polygon, &seam_end);
	}

	/* The centerline is not needed any more; a failed outline frees its corners. */
	free(path.vertices);
	if (polygon.error != 0) {
		free(polygon.corners);
		return polygon.error;
	}

	/* Succeeded: the caller owns the closed polygon. */
	*outline = polygon.corners;
	*outline_count = polygon.count;
	return 0;
}

/* Joins a stroke's kept samples into its centerline, which the caller frees. */
static int
build_path(
	struct pdf_outline_stroke *stroke,
	double width,
	struct pdf_outline_path *path)
{
	/* Cuts the spline through the samples into short pieces with their half widths. */
	memset(path, 0, sizeof(*path));
	build_centerline(stroke, width, path);
	if (path->error != 0) {
		free(path->vertices);
		path->vertices = NULL;
		return path->error;
	}

	/* Succeeded: the centerline has at least the first point. */
	return 0;
}

/*
 * Builds the smoothed centerline through the samples.
 *
 * Each interval between two samples is cut into the pieces its curve needs;
 * the first sample and the end of every interval become vertices, so the
 * centerline passes through every sample.  Each sample notes the vertex it
 * became.
 */
static void
build_centerline(
	struct pdf_outline_stroke *stroke,
	double width,
	struct pdf_outline_path *path)
{
	struct pdf_outline_sample *samples;
	struct pdf_outline_piece piece;
	size_t count;
	size_t index;
	size_t pieces;
	size_t step;
	double t;
	double u;
	double x;
	double y;
	double factor;
	double radius;

	/* Starts at the first sample. */
	samples = stroke->samples;
	count = stroke->count;
	radius = shape_radius(&stroke->shape, width, samples[0].factor);
	path_append(path, samples[0].x, samples[0].y, radius);
	samples[0].vertex = 0;

	/* Cuts each interval's curve into pieces and appends the end of each piece. */
	for (index = 0; index + 1 < count; index++) {
		/* Finds the interval's curve and how finely it must be cut. */
		spline_interval(samples, count, index, &piece);
		pieces = interval_pieces(&piece);

		/* Appends the end of each piece. */
		for (step = 1; step <= pieces; step++) {
			/* Evaluates the Bezier form at the end of the piece. */
			t = (double)step / (double)pieces;
			u = 1.0 - t;
			x = u * u * u * piece.start_x + 3.0 * u * u * t * piece.control1_x + 3.0 * u * t * t * piece.control2_x + t * t * t * piece.end_x;
			y = u * u * u * piece.start_y + 3.0 * u * u * t * piece.control1_y + 3.0 * u * t * t * piece.control2_y + t * t * t * piece.end_y;

			/* The interval's last piece ends exactly on the next sample. */
			if (step == pieces) {
				x = piece.end_x;
				y = piece.end_y;
			}

			/* Appends the point with the half width of its interpolated factor. */
			factor = interpolate_factor(&piece, t);
			radius = shape_radius(&stroke->shape, width, factor);
			path_append(path, x, y, radius);
		}

		/* The next sample is the vertex appended last (or the one it merged into). */
		if (path->count != 0)
			samples[index + 1].vertex = path->count - 1;
	}
}

/*
 * Finds the cubic Bezier form of the centripetal Catmull-Rom interval that
 * joins sample index to the next one.
 *
 * The spline needs a neighbor on each side of the interval.  At the ends of
 * the stroke the missing neighbor is the far sample mirrored through the
 * end, so the curve leaves the end straight toward its neighbor.
 */
static void
spline_interval(
	const struct pdf_outline_sample *samples,
	size_t count,
	size_t index,
	struct pdf_outline_piece *piece)
{
	double before_x;
	double before_y;
	double after_x;
	double after_y;
	double knot_before;
	double knot_middle;
	double knot_after;
	double tangent_x;
	double tangent_y;

	/* Takes the sample before the interval, or mirrors the next one through the first. */
	if (index > 0) {
		before_x = samples[index - 1].x;
		before_y = samples[index - 1].y;
		piece->factor_before = samples[index - 1].factor;
	} else {
		before_x = 2.0 * samples[0].x - samples[1].x;
		before_y = 2.0 * samples[0].y - samples[1].y;
		piece->factor_before = samples[0].factor;
	}

	/* Takes the sample after the interval, or mirrors the previous one through the last. */
	if (index + 2 < count) {
		after_x = samples[index + 2].x;
		after_y = samples[index + 2].y;
		piece->factor_after = samples[index + 2].factor;
	} else {
		after_x = 2.0 * samples[index + 1].x - samples[index].x;
		after_y = 2.0 * samples[index + 1].y - samples[index].y;
		piece->factor_after = samples[index + 1].factor;
	}

	/* Records the interval's ends. */
	piece->start_x = samples[index].x;
	piece->start_y = samples[index].y;
	piece->end_x = samples[index + 1].x;
	piece->end_y = samples[index + 1].y;
	piece->factor_start = samples[index].factor;
	piece->factor_end = samples[index + 1].factor;

	/*
	 * Spaces the knots by the square roots of the distances, which is what
	 * makes the spline centripetal.  Neighboring samples are at least the
	 * merge distance apart, so no knot interval is empty.
	 */
	knot_before = knot_distance(before_x, before_y, piece->start_x, piece->start_y);
	knot_middle = knot_distance(piece->start_x, piece->start_y, piece->end_x, piece->end_y);
	knot_after = knot_distance(piece->end_x, piece->end_y, after_x, after_y);

	/* Finds the tangent at the start, scaled to the interval, and the first control point a third of it along. */
	tangent_x = (piece->start_x - before_x) / knot_before - (piece->end_x - before_x) / (knot_before + knot_middle) + (piece->end_x - piece->start_x) / knot_middle;
	tangent_y = (piece->start_y - before_y) / knot_before - (piece->end_y - before_y) / (knot_before + knot_middle) + (piece->end_y - piece->start_y) / knot_middle;
	piece->control1_x = piece->start_x + tangent_x * knot_middle / 3.0;
	piece->control1_y = piece->start_y + tangent_y * knot_middle / 3.0;

	/* Finds the tangent at the end and the second control point a third of it back. */
	tangent_x = (piece->end_x - piece->start_x) / knot_middle - (after_x - piece->start_x) / (knot_middle + knot_after) + (after_x - piece->end_x) / knot_after;
	tangent_y = (piece->end_y - piece->start_y) / knot_middle - (after_y - piece->start_y) / (knot_middle + knot_after) + (after_y - piece->end_y) / knot_after;
	piece->control2_x = piece->end_x - tangent_x * knot_middle / 3.0;
	piece->control2_y = piece->end_y - tangent_y * knot_middle / 3.0;
}

/* Computes the square root of the distance between two points, the centripetal knot spacing. */
static double
knot_distance(
	double from_x,
	double from_y,
	double to_x,
	double to_y)
{
	double step_x;
	double step_y;
	double length;
	double root;

	/* Measures the step between the points. */
	step_x = to_x - from_x;
	step_y = to_y - from_y;

	/* The square root of its length (the centripetal parameterization's knot distance). */
	length = sqrt(step_x * step_x + step_y * step_y);
	root = sqrt(length);

	/* Reports the knot distance. */
	return root;
}

/*
 * Counts the pieces an interval's curve is cut into.
 *
 * Wang's bound for a cubic: n pieces stray at most 3/4 * d / n^2 from the
 * curve, where d is the larger second difference of the control points.
 */
static size_t
interval_pieces(
	const struct pdf_outline_piece *piece)
{
	double first_x;
	double first_y;
	double second_x;
	double second_y;
	double first;
	double second;
	double largest;
	double pieces;

	/* Measures the two second differences of the control points. */
	first_x = piece->start_x - 2.0 * piece->control1_x + piece->control2_x;
	first_y = piece->start_y - 2.0 * piece->control1_y + piece->control2_y;
	second_x = piece->control1_x - 2.0 * piece->control2_x + piece->end_x;
	second_y = piece->control1_y - 2.0 * piece->control2_y + piece->end_y;
	first = sqrt(first_x * first_x + first_y * first_y);
	second = sqrt(second_x * second_x + second_y * second_y);
	largest = first;
	if (second > largest)
		largest = second;

	/* Finds the fewest pieces that stay within the tolerance, within the limits. */
	pieces = ceil(sqrt(0.75 * largest / PDF_OUTLINE_TOLERANCE));
	if (pieces < 1.0)
		pieces = 1.0;
	if (pieces > PDF_OUTLINE_PIECES_MAX)
		pieces = PDF_OUTLINE_PIECES_MAX;

	/* Reports the whole number of pieces. */
	return (size_t)pieces;
}

/*
 * Interpolates the factor (the pen's pressure) at a point of an interval.
 *
 * A Catmull-Rom curve through the four factors around the interval gives
 * a smooth change; it is kept between the interval's two end factors, so
 * a stroke never draws wider or thinner than the samples on either side.
 */
static double
interpolate_factor(
	const struct pdf_outline_piece *piece,
	double t)
{
	double slope_start;
	double slope_end;
	double t2;
	double t3;
	double factor;
	double lowest;
	double highest;

	/* Finds the slopes at the interval's ends from the neighboring factors. */
	slope_start = (piece->factor_end - piece->factor_before) / 2.0;
	slope_end = (piece->factor_after - piece->factor_start) / 2.0;

	/* Evaluates the cubic Hermite curve between the end factors. */
	t2 = t * t;
	t3 = t2 * t;
	factor = (2.0 * t3 - 3.0 * t2 + 1.0) * piece->factor_start + (t3 - 2.0 * t2 + t) * slope_start + (-2.0 * t3 + 3.0 * t2) * piece->factor_end + (t3 - t2) * slope_end;

	/* Keeps the factor between the two end factors. */
	lowest = piece->factor_start;
	highest = piece->factor_end;
	if (highest < lowest) {
		lowest = piece->factor_end;
		highest = piece->factor_start;
	}

	/* The factor within them. */
	if (factor < lowest)
		factor = lowest;
	if (factor > highest)
		factor = highest;

	/* Reports the interpolated factor. */
	return factor;
}

/*
 * Appends a centerline point.
 *
 * A point that has not moved away from the previous one is merged into it,
 * keeping the wider of the two, so no segment of the centerline is empty.
 */
static void
path_append(
	struct pdf_outline_path *path,
	double x,
	double y,
	double radius)
{
	struct pdf_outline_vertex *grown;
	struct pdf_outline_vertex *last;
	size_t capacity;
	double step_x;
	double step_y;

	/* A failed centerline takes no more points. */
	if (path->error != 0)
		return;

	/* Merges a point that has not moved into the previous one. */
	if (path->count != 0) {
		last = &path->vertices[path->count - 1];
		step_x = x - last->x;
		step_y = y - last->y;
		if (step_x * step_x + step_y * step_y < PDF_OUTLINE_MIN_STEP * PDF_OUTLINE_MIN_STEP) {
			if (radius > last->radius)
				last->radius = radius;
			return;
		}
	}

	/* Refuses a centerline longer than one outline may be. */
	if (path->count == PDF_OUTLINE_CORNERS_MAX) {
		path->error = EINVAL;
		return;
	}

	/* Grows the vertex array when it is full. */
	if (path->count == path->capacity) {
		capacity = path->capacity * 2;
		if (capacity == 0)
			capacity = 64;
		grown = realloc(path->vertices, capacity * sizeof(*grown));
		if (grown == NULL) {
			path->error = ENOMEM;
			return;
		}

		/* The grown array is the path's. */
		path->vertices = grown;
		path->capacity = capacity;
	}

	/* Appends the point. */
	path->vertices[path->count].x = x;
	path->vertices[path->count].y = y;
	path->vertices[path->count].radius = radius;
	path->count++;
}

/*
 * Moves each cut to the first vertex at or after its point where the
 * centerline can be cut flat, before the next cut's point (or the last
 * vertex), and after the cut taken before it; a cut with none is left out.
 * boundaries gets the vertices of the cuts taken, starts their points, and
 * chosen their count.
 */
static int
choose_cuts(
	const struct pdf_outline_stroke *stroke,
	const struct pdf_outline_path *path,
	const size_t *cuts,
	size_t cut_count,
	size_t *boundaries,
	size_t *starts,
	size_t *chosen)
{
	double *arcs;
	size_t previous;
	size_t from;
	size_t limit;
	size_t index;
	size_t vertex;
	int usable;
	int error;

	/* Measures how far along the centerline each vertex is. */
	error = measure_arcs(path, &arcs);
	if (error != 0)
		return error;

	/* Takes each cut in order; the first range starts at the first vertex. */
	*chosen = 0;
	previous = 0;
	for (index = 0; index < cut_count; index++) {
		/* The cut's own vertex, after the cut taken before it. */
		from = stroke->samples[stroke->kept_of[cuts[index]]].vertex;
		if (from <= previous)
			from = previous + 1;

		/* The cut must come before the next cut's vertex, and before the last vertex. */
		limit = path->count - 1;
		if (index + 1 < cut_count)
			limit = stroke->samples[stroke->kept_of[cuts[index + 1]]].vertex;

		/* Walks on toward the end until a vertex the centerline can be cut at. */
		for (vertex = from; vertex < limit; vertex++) {
			usable = seam_usable(path, arcs, vertex);
			if (!usable)
				continue;

			/* The cut is taken here; the next one must come after it. */
			boundaries[*chosen] = vertex;
			starts[*chosen] = cuts[index];
			(*chosen)++;
			previous = vertex;
			break;
		}
	}

	/* The lengths are not needed any more. */
	free(arcs);

	/* Succeeded: the cuts taken are listed. */
	return 0;
}

/* Measures, for each vertex of the centerline, its distance along it from the first (a malloc'd array). */
static int
measure_arcs(
	const struct pdf_outline_path *path,
	double **arcs)
{
	double *lengths;
	double step_x;
	double step_y;
	size_t index;

	/* Allocates one length a vertex. */
	lengths = malloc(path->count * sizeof(*lengths));
	if (lengths == NULL)
		return ENOMEM;

	/* Adds up the segments' lengths from the first vertex. */
	lengths[0] = 0.0;
	for (index = 1; index < path->count; index++) {
		step_x = path->vertices[index].x - path->vertices[index - 1].x;
		step_y = path->vertices[index].y - path->vertices[index - 1].y;
		lengths[index] = lengths[index - 1] + sqrt(step_x * step_x + step_y * step_y);
	}

	/* Succeeded: the caller owns the lengths. */
	*arcs = lengths;
	return 0;
}

/*
 * Tells whether the centerline can be cut flat at an inner vertex: the turn
 * there is gentle (one corner on each edge, on its middle normal), and so
 * is every turn within twice its half width along the centerline on either
 * side, which together turn little.  The trapezoids near such a cut do not
 * reach across it, so the two ranges meet without overlapping there.
 */
static int
seam_usable(
	const struct pdf_outline_path *path,
	const double *arcs,
	size_t index)
{
	struct pdf_outline_join join;
	double reach;
	double turned;
	size_t other;

	/* Only an inner vertex can be cut. */
	if (index == 0 || index + 1 >= path->count)
		return 0;

	/* The turn at the vertex must be gentle. */
	measure_join(path->vertices, index, &join);
	if (!join.gentle)
		return 0;

	/* How far around the vertex the centerline must stay gentle, and how much it has turned. */
	reach = PDF_OUTLINE_SEAM_REACH * path->vertices[index].radius;
	turned = fabs(join.turn);

	/* Walks back over the inner vertices within the reach. */
	other = index - 1;
	while (other > 0 && arcs[index] - arcs[other] <= reach) {
		measure_join(path->vertices, other, &join);
		if (!join.gentle)
			return 0;
		turned += fabs(join.turn);
		other--;
	}

	/* Walks on over the inner vertices within the reach. */
	other = index + 1;
	while (other + 1 < path->count && arcs[other] - arcs[index] <= reach) {
		measure_join(path->vertices, other, &join);
		if (!join.gentle)
			return 0;
		turned += fabs(join.turn);
		other++;
	}

	/* Too much turning near the cut would let the pieces on either side reach across it. */
	if (turned > PDF_OUTLINE_SEAM_TURN)
		return 0;

	/* Succeeded: the centerline can be cut at the vertex. */
	return 1;
}

/*
 * Walks around one range of a centerline of two or more points.
 *
 * The left edge lies along each segment's left normal and the right edge
 * along its right one.  At a corner, the edge on the outside of the turn
 * follows an arc around the corner point from one segment's normal to the
 * next; the edge on the inside passes through the corner point.  A gentle
 * turn, whose arc would be one chord, takes one corner on each edge.  An end
 * of the centerline is a round cap; a range that ends inside it is cut
 * along the cut vertex's middle normal (its two corners are the gentle
 * turn's), and seam_end gets the index of the cut's first corner.  A range
 * that starts inside it closes along its start's middle normal.  The whole
 * centerline as one range is the stroke's outline.
 */
static void
build_range(
	const struct pdf_outline_path *path,
	const struct pdf_outline_span *span,
	struct pdf_outline_polygon *polygon,
	size_t *seam_end)
{
	const struct pdf_outline_vertex *vertices;
	const struct pdf_outline_vertex *corner;
	const struct pdf_outline_vertex *first;
	const struct pdf_outline_vertex *last;
	struct pdf_outline_join join;
	struct pdf_outline_join end_join;
	struct pdf_outline_join start_join;
	size_t count;
	size_t index;
	size_t chords;
	double normal_x;
	double normal_y;
	double start_angle;
	int capped_start;
	int capped_end;

	/* No cut is measured at the start until the range is known to start at one. */
	memset(&start_join, 0, sizeof(start_join));

	/* Names the centerline, the range's ends and whether each is an end of the centerline. */
	vertices = path->vertices;
	count = path->count;
	first = &vertices[span->first];
	last = &vertices[span->last];
	capped_start = 0;
	if (span->first == 0)
		capped_start = 1;
	capped_end = 0;
	if (span->last == count - 1)
		capped_end = 1;
	*seam_end = PDF_OUTLINE_NO_SEAM;

	/* Starts the left edge at the first point: along the first segment's normal, or the cut's middle one. */
	if (capped_start) {
		segment_normal(&vertices[0], &vertices[1], &normal_x, &normal_y);
		polygon_append(polygon, vertices[0].x + normal_x * vertices[0].radius, vertices[0].y + normal_y * vertices[0].radius);
	} else {
		measure_join(vertices, span->first, &start_join);
		polygon_append(polygon, first->x + start_join.middle_x * first->radius, first->y + start_join.middle_y * first->radius);
	}

	/* Walks the left edge forward through each inner point of the range. */
	for (index = span->first + 1; index < span->last; index++) {
		/* Measures the turn at the point. */
		corner = &vertices[index];
		measure_join(vertices, index, &join);

		/* Joins the two segments' left edges. */
		if (join.gentle) {
			/* A gentle turn needs one corner, on the middle normal. */
			polygon_append(polygon, corner->x + join.middle_x * corner->radius, corner->y + join.middle_y * corner->radius);
		} else if (join.turn < 0.0) {
			/* A turn to the right puts the left edge outside: it follows a round join. */
			start_angle = atan2(join.before_y, join.before_x);
			chords = arc_chords(corner->radius, -join.turn, 1);
			polygon_append(polygon, corner->x + join.before_x * corner->radius, corner->y + join.before_y * corner->radius);
			append_arc(polygon, corner, start_angle, join.turn, chords);
			polygon_append(polygon, corner->x + join.after_x * corner->radius, corner->y + join.after_y * corner->radius);
		} else {
			/* A turn to the left puts the left edge inside: it passes through the point. */
			polygon_append(polygon, corner->x + join.before_x * corner->radius, corner->y + join.before_y * corner->radius);
			polygon_append(polygon, corner->x, corner->y);
			polygon_append(polygon, corner->x + join.after_x * corner->radius, corner->y + join.after_y * corner->radius);
		}
	}

	/* Ends the left edge at the last point, then turns around it with a cap or crosses it along the cut. */
	if (capped_end) {
		/* The last segment's normal, then the cap from the left edge through the stroke's direction to the right edge. */
		segment_normal(&vertices[count - 2], last, &normal_x, &normal_y);
		polygon_append(polygon, last->x + normal_x * last->radius, last->y + normal_y * last->radius);
		start_angle = atan2(normal_y, normal_x);
		chords = arc_chords(last->radius, PDF_OUTLINE_PI, PDF_OUTLINE_CAP_MIN);
		append_arc(polygon, last, start_angle, -PDF_OUTLINE_PI, chords);

		/* Starts the right edge back at the last point. */
		polygon_append(polygon, last->x - normal_x * last->radius, last->y - normal_y * last->radius);
	} else {
		/* The cut's two corners on its middle normal; the edge between them is the seam with the next range. */
		measure_join(vertices, span->last, &end_join);
		*seam_end = polygon->count;
		polygon_append(polygon, last->x + end_join.middle_x * last->radius, last->y + end_join.middle_y * last->radius);
		polygon_append(polygon, last->x - end_join.middle_x * last->radius, last->y - end_join.middle_y * last->radius);
	}

	/* Walks the right edge back through each inner point of the range. */
	for (index = span->last - 1; index > span->first; index--) {
		/* Measures the turn at the point. */
		corner = &vertices[index];
		measure_join(vertices, index, &join);

		/* Joins the two segments' right edges. */
		if (join.gentle) {
			/* A gentle turn needs one corner, on the middle normal. */
			polygon_append(polygon, corner->x - join.middle_x * corner->radius, corner->y - join.middle_y * corner->radius);
		} else if (join.turn > 0.0) {
			/* A turn to the left puts the right edge outside: it follows a round join. */
			start_angle = atan2(-join.after_y, -join.after_x);
			chords = arc_chords(corner->radius, join.turn, 1);
			polygon_append(polygon, corner->x - join.after_x * corner->radius, corner->y - join.after_y * corner->radius);
			append_arc(polygon, corner, start_angle, -join.turn, chords);
			polygon_append(polygon, corner->x - join.before_x * corner->radius, corner->y - join.before_y * corner->radius);
		} else {
			/* A turn to the right puts the right edge inside: it passes through the point. */
			polygon_append(polygon, corner->x - join.after_x * corner->radius, corner->y - join.after_y * corner->radius);
			polygon_append(polygon, corner->x, corner->y);
			polygon_append(polygon, corner->x - join.before_x * corner->radius, corner->y - join.before_y * corner->radius);
		}
	}

	/* Ends the right edge at the first point: with the cap back to the left edge, or at the cut, which the polygon closes along. */
	if (capped_start) {
		/* The first segment's normal, then the cap against the stroke's direction back to the left edge. */
		segment_normal(&vertices[0], &vertices[1], &normal_x, &normal_y);
		polygon_append(polygon, vertices[0].x - normal_x * vertices[0].radius, vertices[0].y - normal_y * vertices[0].radius);
		start_angle = atan2(-normal_y, -normal_x);
		chords = arc_chords(vertices[0].radius, PDF_OUTLINE_PI, PDF_OUTLINE_CAP_MIN);
		append_arc(polygon, &vertices[0], start_angle, -PDF_OUTLINE_PI, chords);
	} else {
		/* The cut's right corner, the same as the range before's. */
		polygon_append(polygon, first->x - start_join.middle_x * first->radius, first->y - start_join.middle_y * first->radius);
	}
}

/* Measures the normals and the turn at an inner point of the centerline. */
static void
measure_join(
	const struct pdf_outline_vertex *vertices,
	size_t index,
	struct pdf_outline_join *join)
{
	double length;
	double step;
	double magnitude;

	/* Finds the normals of the segments on either side of the point and the turn between them. */
	segment_normal(&vertices[index - 1], &vertices[index], &join->before_x, &join->before_y);
	segment_normal(&vertices[index], &vertices[index + 1], &join->after_x, &join->after_y);
	join->turn = turn_angle(&vertices[index - 1], &vertices[index], &vertices[index + 1]);

	/* A turn that one chord of the join's arc covers is gentle. */
	step = arc_step(vertices[index].radius);
	magnitude = fabs(join->turn);
	join->gentle = 0;
	if (magnitude <= step)
		join->gentle = 1;

	/*
	 * Finds the normal halfway between the two.  A gentle turn is at most a
	 * quarter turn, so the two normals never cancel.
	 */
	join->middle_x = join->before_x + join->after_x;
	join->middle_y = join->before_y + join->after_y;
	length = sqrt(join->middle_x * join->middle_x + join->middle_y * join->middle_y);
	if (length > 0.0) {
		join->middle_x /= length;
		join->middle_y /= length;
	}
}

/*
 * Measures the signed angle the centerline turns at a point.
 *
 * It is positive when the stroke turns toward its left normal and runs from
 * minus to plus pi; a stroke that doubles back turns by pi either way.
 */
static double
turn_angle(
	const struct pdf_outline_vertex *before,
	const struct pdf_outline_vertex *corner,
	const struct pdf_outline_vertex *after)
{
	double in_x;
	double in_y;
	double out_x;
	double out_y;
	double cross;
	double dot;
	double angle;

	/* Takes the segments that arrive at and leave the point. */
	in_x = corner->x - before->x;
	in_y = corner->y - before->y;
	out_x = after->x - corner->x;
	out_y = after->y - corner->y;

	/* Compares their directions. */
	cross = in_x * out_y - in_y * out_x;
	dot = in_x * out_x + in_y * out_y;

	/* The angle from the arriving direction to the leaving one. */
	angle = atan2(cross, dot);

	/* Reports the turn, in radians. */
	return angle;
}

/* Finds the unit left normal of a segment, its direction turned a quarter toward positive angles. */
static void
segment_normal(
	const struct pdf_outline_vertex *from,
	const struct pdf_outline_vertex *to,
	double *normal_x,
	double *normal_y)
{
	double step_x;
	double step_y;
	double length;

	/* Measures the segment, which the centerline builder never leaves empty. */
	step_x = to->x - from->x;
	step_y = to->y - from->y;
	length = sqrt(step_x * step_x + step_y * step_y);

	/* Turns the unit direction a quarter. */
	*normal_x = -step_y / length;
	*normal_y = step_x / length;
}

/* Draws a stroke that never moved as a whole circle around its only point. */
static void
build_dot(
	const struct pdf_outline_vertex *center,
	struct pdf_outline_polygon *polygon)
{
	size_t chords;

	/* Starts on the circle and walks it once; the polygon closes itself. */
	chords = arc_chords(center->radius, 2.0 * PDF_OUTLINE_PI, 2 * PDF_OUTLINE_CAP_MIN);
	polygon_append(polygon, center->x + center->radius, center->y);
	append_arc(polygon, center, 0.0, 2.0 * PDF_OUTLINE_PI, chords);
}

/*
 * Computes the widest angle one chord of a circle may span and stay within
 * the tolerance of the circle.
 *
 * A circle smaller than the tolerance takes a quarter turn per chord.
 */
static double
arc_step(
	double radius)
{
	/* A circle smaller than the tolerance needs no finer chords. */
	if (radius <= PDF_OUTLINE_TOLERANCE)
		return PDF_OUTLINE_PI / 2.0;

	/* Reports the angle whose chord strays from the circle by the tolerance. */
	return 2.0 * acos(1.0 - PDF_OUTLINE_TOLERANCE / radius);
}

/*
 * Counts the chords an arc needs to stay within the tolerance of its circle.
 *
 * angle is the arc's size in radians.  The count is at least fewest and at
 * most the half turn's limit in proportion to the arc.
 */
static size_t
arc_chords(
	double radius,
	double angle,
	size_t fewest)
{
	double step;
	double chords;
	double most;

	/* Finds the widest angle one chord may span. */
	step = arc_step(radius);

	/* Divides the arc into chords no wider than the step, within the limits. */
	chords = ceil(angle / step);
	most = ceil(PDF_OUTLINE_CAP_MAX * angle / PDF_OUTLINE_PI);
	if (chords > most)
		chords = most;
	if (chords < (double)fewest)
		chords = (double)fewest;

	/* Reports the whole number of chords. */
	return (size_t)chords;
}

/*
 * Appends the inner corners of an arc around a point.
 *
 * The arc starts at start_angle and turns by sweep radians in chords steps.
 * Its two ends are the edges' corners, which the caller writes itself, so
 * only the corners between them are appended.
 */
static void
append_arc(
	struct pdf_outline_polygon *polygon,
	const struct pdf_outline_vertex *center,
	double start_angle,
	double sweep,
	size_t chords)
{
	size_t step;
	double angle;

	/* Writes each corner strictly between the arc's ends. */
	for (step = 1; step < chords; step++) {
		angle = start_angle + sweep * (double)step / (double)chords;
		polygon_append(polygon, center->x + center->radius * cos(angle), center->y + center->radius * sin(angle));
	}
}

/* Appends one corner to the outline polygon. */
static void
polygon_append(
	struct pdf_outline_polygon *polygon,
	double x,
	double y)
{
	struct pdf_point *grown;
	size_t capacity;

	/* A failed polygon takes no more corners. */
	if (polygon->error != 0)
		return;

	/* Refuses a polygon larger than one outline may be. */
	if (polygon->count == PDF_OUTLINE_CORNERS_MAX) {
		polygon->error = EINVAL;
		return;
	}

	/* Grows the corner array when it is full. */
	if (polygon->count == polygon->capacity) {
		capacity = polygon->capacity * 2;
		if (capacity == 0)
			capacity = 64;
		grown = realloc(polygon->corners, capacity * sizeof(*grown));
		if (grown == NULL) {
			polygon->error = ENOMEM;
			return;
		}

		/* The grown array is the polygon's. */
		polygon->corners = grown;
		polygon->capacity = capacity;
	}

	/* Appends the corner. */
	polygon->corners[polygon->count].x = x;
	polygon->corners[polygon->count].y = y;
	polygon->count++;
}
