/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The shapes of Notes' drawing tools (ws216-p003, plan/ws216/phase001/
 * phase.md sections 2 and 3): the pen, the highlighter, the brush, the
 * pencil, the watercolour and the fill.
 *
 * A stroke's ink is a list of shapes, each a polygon the screen and the PDF
 * fill alike.  The pen's and the highlighter's come from the pressure as
 * they always did.  The brush's, the pencil's and the watercolour's widths
 * follow factors that formulas give each sample (the speed, the taper, a
 * wobble of the watercolour's edges), and the pencil's darkness follows
 * steps of the pressure; when a stroke is put on a page those values are
 * baked into it (notes_stroke_bake()), so that its shapes depend on its
 * samples, its baked values and the tool's constants only.  Until then the
 * formulas give them afresh, with the stroke's end where it is now.
 *
 * The pencil's grain is a tile of 8-bit coverage made from integers only,
 * so that every machine makes the same bytes; the PDF repeats it as a
 * pattern and the screen as a texture.
 */

#include "notes.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/*
 * The brush (section 3.1): the pressure's least share, the speed's window
 * on either side of a sample (milliseconds), the speed that thins it most
 * gently (points a millisecond), the speed term's top, slope and least,
 * the taper's least share, and the entry's and the exit's lengths (in
 * widths, and as a share of the stroke's length at most).
 */
#define SHAPE_BRUSH_PRESSURE_MIN	0.05
#define SHAPE_BRUSH_WINDOW_MS		8U
#define SHAPE_BRUSH_SPEED_REFERENCE	0.6
#define SHAPE_BRUSH_SPEED_TOP		1.15
#define SHAPE_BRUSH_SPEED_SLOPE		0.35
#define SHAPE_BRUSH_SPEED_LEAST		0.55
#define SHAPE_BRUSH_TAPER_LEAST		0.08
#define SHAPE_BRUSH_ENTRY_WIDTHS	3.0
#define SHAPE_BRUSH_ENTRY_SHARE		0.25
#define SHAPE_BRUSH_EXIT_WIDTHS		4.0
#define SHAPE_BRUSH_EXIT_SHARE		0.3

/*
 * The pencil (section 3.2): its width factor's base, span and exponent of
 * the pressure, the darkness step's exponent of the pressure, the length
 * a new step must last to be taken (points), and the opacity of step 0 and
 * the span up to the last step.
 */
#define SHAPE_PENCIL_BASE		0.6
#define SHAPE_PENCIL_SPAN		0.4
#define SHAPE_PENCIL_EXPONENT		0.7
#define SHAPE_PENCIL_STEP_EXPONENT	0.8
#define SHAPE_PENCIL_STEP_HOLD		2.0
#define SHAPE_PENCIL_ALPHA_BASE		0.30f
#define SHAPE_PENCIL_ALPHA_SPAN		0.70f

/*
 * The watercolour (section 3.3): the core's base and span of the pressure,
 * the two edges' sizes over the core, the edges' wobble, and the opacity
 * of the core and of each edge.
 */
#define SHAPE_WATER_CORE_BASE		0.85
#define SHAPE_WATER_CORE_SPAN		0.15
#define SHAPE_WATER_EDGE1		1.18
#define SHAPE_WATER_EDGE2		1.36
#define SHAPE_WATER_WOBBLE		0.08
#define SHAPE_WATER_ALPHA_CORE		0.22f
#define SHAPE_WATER_ALPHA_EDGE1		0.10f
#define SHAPE_WATER_ALPHA_EDGE2		0.06f

/* The watercolour's factor columns: the core, the first edge and the second. */
#define SHAPE_WATER_COLUMNS		3U

/* libpdf's style of a stroke of factors: no floor, the factor as it is, up to NOTES_FACTOR_MAX, a least radius. */
#define SHAPE_STYLE_FLOOR		0.0
#define SHAPE_STYLE_EXPONENT		1.0
#define SHAPE_STYLE_RADIUS_MIN		0.05

/*
 * The grain (section 3.2): the texels between the lattice points of the
 * coarse and the fine noise, and the share of the fine one in 1/3.
 */
#define SHAPE_GRAIN_COARSE_STEP		4U
#define SHAPE_GRAIN_FINE_STEP		2U

/*
 * The smoothstep of the quarter steps of a lattice of four texels and of
 * the halves of one of two, in 1/256: the weights the grain's integer
 * interpolation uses.
 */
static const unsigned shape_grain_quarters[4] = { 0U, 40U, 128U, 216U };
static const unsigned shape_grain_halves[2] = { 0U, 128U };

static int make_pen(struct notes_stroke *stroke);
static int make_brush(struct notes_stroke *stroke, const uint16_t *factors);
static int make_pencil(struct notes_stroke *stroke, const uint16_t *factors, const unsigned char *steps);
static int make_watercolor(struct notes_stroke *stroke, const uint16_t *factors);
static int make_fill(struct notes_stroke *stroke);
static int make_outline(const struct notes_stroke *stroke, const uint16_t *factors, unsigned column, unsigned columns, struct pdf_point **outline, size_t *count);
static int shapes_alloc(struct notes_stroke *stroke, size_t count);
static void shape_init(struct notes_shape *shape, struct pdf_point *polygon, size_t corner_count, float alpha, unsigned paint, unsigned blend);
static int compute_values(const struct notes_stroke *stroke, uint16_t **factors, unsigned char **steps);
static void compute_brush(const struct notes_stroke *stroke, const double *lengths, uint16_t *factors);
static void compute_pencil(const struct notes_stroke *stroke, const double *lengths, uint16_t *factors, unsigned char *steps);
static void compute_watercolor(const struct notes_stroke *stroke, const double *lengths, uint16_t *factors);
static uint16_t factor_units(double factor);
static double factor_value(uint16_t units);
static double sample_pressure(const struct notes_point *point);
static double smooth(double share);
static double wobble(double length, double spacing, uint32_t seed);
static uint32_t mix(uint32_t value);
static unsigned grain_lattice(unsigned x, unsigned y, unsigned side, uint32_t seed);
static unsigned grain_noise(unsigned x, unsigned y, unsigned step, const unsigned *weights, uint32_t seed);

/*
 * Tells the version of a tool's constants a new stroke of it carries.
 */
unsigned
notes_tool_constants(
	unsigned tool)
{
	/* The tools of ws216 have constants; the pen and the highlighter have none. */
	switch (tool) {
	case NOTES_TOOL_BRUSH:
	case NOTES_TOOL_PENCIL:
	case NOTES_TOOL_WATERCOLOR:
	case NOTES_TOOL_FILL:
		/* The version this Notes draws. */
		return NOTES_TOOL_CONSTANTS;
	default:
		/* No constants. */
		return 0U;
	}
}

/*
 * Tells how many factor columns a stroke of a tool bakes (0: none).
 */
unsigned
notes_tool_columns(
	unsigned tool)
{
	/* The brush and the pencil bake a width factor, the watercolour its core and two edges. */
	switch (tool) {
	case NOTES_TOOL_BRUSH:
	case NOTES_TOOL_PENCIL:
		/* One width factor a sample. */
		return 1U;
	case NOTES_TOOL_WATERCOLOR:
		/* The core and the two edges. */
		return SHAPE_WATER_COLUMNS;
	default:
		/* The pen's width follows the pressure, a fill has no width. */
		return 0U;
	}
}

/*
 * Bakes a stroke's factors and steps: the values the formulas give its
 * samples now are kept with it, and its shapes follow them from now on.
 *
 * A stroke that bakes nothing (the pen, the highlighter, the fill) or is
 * baked already stays as it is.  Returns 0, EINVAL for a stroke without
 * samples, or ENOMEM.
 */
int
notes_stroke_bake(
	struct notes_stroke *stroke)
{
	uint16_t *factors;
	unsigned char *steps;
	unsigned columns;
	int baked;
	int error;

	/* A tool without factors, or a stroke baked already, has nothing to bake. */
	columns = notes_tool_columns(stroke->tool);
	if (columns == 0U)
		return 0;
	baked = notes_stroke_baked(stroke);
	if (baked)
		return 0;

	/* A stroke without samples has no values. */
	if (stroke->point_count == 0U)
		return EINVAL;

	/* The values of the formulas, now. */
	error = compute_values(stroke, &factors, &steps);
	if (error != 0)
		return error;

	/* The stroke keeps them; its shapes are made from them from now on. */
	free(stroke->factors);
	free(stroke->steps);
	stroke->factors = factors;
	stroke->factor_columns = columns;
	stroke->steps = steps;
	notes_shape_drop(stroke);

	/* Succeeded: the stroke is baked. */
	return 0;
}

/*
 * Tells whether a stroke holds every value its tool bakes (a tool that
 * bakes nothing always does).
 */
int
notes_stroke_baked(
	const struct notes_stroke *stroke)
{
	unsigned columns;

	/* A tool that bakes nothing. */
	columns = notes_tool_columns(stroke->tool);
	if (columns == 0U)
		return 1;

	/* The factors, in the tool's columns. */
	if (stroke->factors == NULL)
		return 0;
	if (stroke->factor_columns != columns)
		return 0;

	/* The pencil's steps. */
	if (stroke->tool == NOTES_TOOL_PENCIL && stroke->steps == NULL)
		return 0;

	/* Every value is there. */
	return 1;
}

/*
 * Tells how far from its path a stroke's ink reaches, in points: half its
 * width for the pen and the highlighter, half its width times its largest
 * factor for the brush and the pencil, times its outer edge's largest for
 * the watercolour (plan/ws216/phase003/phase.md section 6).
 *
 * The eraser touches and cuts at this reach beyond its circle, so the
 * round end of a piece never reaches into the circle.  A stroke not baked
 * yet is given the largest reach its factors could have.
 */
float
notes_stroke_reach(
	const struct notes_stroke *stroke)
{
	unsigned columns;
	unsigned column;
	unsigned largest;
	size_t index;
	double reach;
	int baked;

	/* The pen, the highlighter and the fill reach half their width. */
	columns = notes_tool_columns(stroke->tool);
	if (columns == 0U)
		return stroke->width / 2.0f;

	/* A stroke not baked yet may reach as far as its factors could. */
	baked = notes_stroke_baked(stroke);
	if (!baked)
		return stroke->width / 2.0f * NOTES_FACTOR_MAX;

	/* The column of the outermost ink: the watercolour's second edge, the only column of the others. */
	column = columns - 1U;

	/* The largest factor of that column. */
	largest = 0;
	for (index = 0; index < stroke->point_count; index++) {
		if (stroke->factors[index * columns + column] > largest)
			largest = stroke->factors[index * columns + column];
	}

	/* Half the width at that factor, never less than libpdf's least radius. */
	reach = (double)stroke->width / 2.0 * factor_value((uint16_t)largest);
	if (reach < SHAPE_STYLE_RADIUS_MIN)
		reach = SHAPE_STYLE_RADIUS_MIN;

	/* Reports the reach. */
	return (float)reach;
}

/*
 * Makes a stroke's shapes and its outline (one of the polygons, which
 * marks the shapes made and starts their box).
 *
 * The stroke must have samples and no shapes.  Returns 0, EINVAL for a
 * stroke whose values do not fit its tool, or the errno value of libpdf.
 */
int
notes_shape_make(
	struct notes_stroke *stroke)
{
	uint16_t *factors;
	unsigned char *steps;
	unsigned columns;
	int baked;
	int error;

	/* The pen, the highlighter and the fill follow their samples only. */
	if (stroke->tool == NOTES_TOOL_FILL) {
		error = make_fill(stroke);
		if (error != 0)
			return error;

		/* Succeeded: the fill's shape. */
		return 0;
	}

	/* Any tool not of ws216 is drawn as a pen (as Notes always did). */
	columns = notes_tool_columns(stroke->tool);
	if (columns == 0U) {
		error = make_pen(stroke);
		if (error != 0)
			return error;

		/* Succeeded: the pen's shape. */
		return 0;
	}

	/* The baked values, or those of the formulas now while the stroke is drawn. */
	factors = stroke->factors;
	steps = stroke->steps;
	baked = notes_stroke_baked(stroke);
	if (!baked) {
		error = compute_values(stroke, &factors, &steps);
		if (error != 0)
			return error;
	}

	/* The tool's shapes. */
	switch (stroke->tool) {
	case NOTES_TOOL_BRUSH:
		/* One shape of the width factors. */
		error = make_brush(stroke, factors);
		break;
	case NOTES_TOOL_PENCIL:
		/* One shape a range of one darkness. */
		error = make_pencil(stroke, factors, steps);
		break;
	default:
		/* The core and the two edges. */
		error = make_watercolor(stroke, factors);
		break;
	}

	/* Values of the formulas are not kept. */
	if (!baked) {
		free(factors);
		free(steps);
	}

	/* A failure leaves no half-made shapes. */
	if (error != 0) {
		notes_shape_drop(stroke);
		return error;
	}

	/* Succeeded: the stroke's shapes and outline. */
	return 0;
}

/*
 * Frees a stroke's shapes and its outline; they are made again when next
 * asked for.
 */
void
notes_shape_drop(
	struct notes_stroke *stroke)
{
	size_t index;

	/* Each shape's polygon, except the one the outline is. */
	for (index = 0; index < stroke->shape_count; index++) {
		if (stroke->shapes[index].polygon != stroke->outline)
			pdf_outline_free(stroke->shapes[index].polygon);
	}
	free(stroke->shapes);
	stroke->shapes = NULL;
	stroke->shape_count = 0;

	/* The outline. */
	pdf_outline_free(stroke->outline);
	stroke->outline = NULL;
	stroke->outline_count = 0;
}

/*
 * Makes the pencil's grain: NOTES_GRAIN_SIDE texels a side of 8-bit
 * coverage, rows from the top, repeating at its edges.
 *
 * Only integers are used, so every machine makes the same bytes (the PDF's
 * pattern and the screen's texture come from them alike).
 */
void
notes_grain_tile(
	unsigned char *tile)
{
	unsigned x;
	unsigned y;
	unsigned coarse;
	unsigned fine;
	int value;

	/* Each texel: two octaves of value noise, then a contrast that spreads them. */
	for (y = 0; y < NOTES_GRAIN_SIDE; y++) {
		for (x = 0; x < NOTES_GRAIN_SIDE; x++) {
			/* The coarse noise (64 lattice points a side) and the fine one (128). */
			coarse = grain_noise(x, y, SHAPE_GRAIN_COARSE_STEP, shape_grain_quarters, 0x5eedU);
			fine = grain_noise(x, y, SHAPE_GRAIN_FINE_STEP, shape_grain_halves, 0x7a11U);

			/* Two thirds coarse and one third fine, then twice the contrast around the middle. */
			value = (int)((coarse * 2U + fine) / 3U);
			value = (value - 128) * 2 + 128;
			if (value < 0)
				value = 0;
			if (value > 255)
				value = 255;
			tile[y * NOTES_GRAIN_SIDE + x] = (unsigned char)value;
		}
	}
}

/*
 * Makes the next coarser level of the grain: each texel the rounded mean
 * of a square of four of the finer level (of finer_side texels a side).
 */
void
notes_grain_level(
	const unsigned char *finer,
	size_t finer_side,
	unsigned char *coarser)
{
	size_t side;
	size_t x;
	size_t y;
	unsigned sum;

	/* Each coarser texel. */
	side = finer_side / 2U;
	for (y = 0; y < side; y++) {
		for (x = 0; x < side; x++) {
			/* The four finer texels it covers. */
			sum = finer[(y * 2U) * finer_side + x * 2U];
			sum += finer[(y * 2U) * finer_side + x * 2U + 1U];
			sum += finer[(y * 2U + 1U) * finer_side + x * 2U];
			sum += finer[(y * 2U + 1U) * finer_side + x * 2U + 1U];
			coarser[y * side + x] = (unsigned char)((sum + 2U) / 4U);
		}
	}
}

/*
 * Makes the pen's and the highlighter's shape: the outline of
 * pdf_outline_stroke(), as Notes always drew it (the highlighter at its
 * full width).
 */
static int
make_pen(
	struct notes_stroke *stroke)
{
	struct pdf_stroke_point *samples;
	struct pdf_point *outline;
	size_t outline_count;
	size_t index;
	int error;

	/* Allocates the samples in libpdf's form. */
	samples = malloc(stroke->point_count * sizeof(*samples));
	if (samples == NULL)
		return ENOMEM;

	/* Converts each sample: its position and its pressure from 0 to 1. */
	for (index = 0; index < stroke->point_count; index++) {
		samples[index].x = stroke->points[index].x;
		samples[index].y = stroke->points[index].y;
		samples[index].pressure = (double)stroke->points[index].pressure / (double)NOTES_PRESSURE_MAX;

		/* A highlighter's width does not follow the pressure. */
		if (stroke->tool == NOTES_TOOL_HIGHLIGHTER)
			samples[index].pressure = 1.0;
	}

	/* Builds the polygon. */
	error = pdf_outline_stroke(samples, stroke->point_count, stroke->width, &outline, &outline_count);
	free(samples);
	if (error != 0)
		return error;

	/* The one shape. */
	error = shapes_alloc(stroke, 1U);
	if (error != 0) {
		pdf_outline_free(outline);
		return error;
	}

	/* Succeeded: the outline is the stroke's one shape. */
	stroke->outline = outline;
	stroke->outline_count = outline_count;
	shape_init(&stroke->shapes[0], outline, outline_count, 1.0f, NOTES_PAINT_SOLID, NOTES_BLEND_NORMAL);
	return 0;
}

/* Makes the brush's shape: the outline of its width factors. */
static int
make_brush(
	struct notes_stroke *stroke,
	const uint16_t *factors)
{
	struct pdf_point *outline;
	size_t outline_count;
	int error;

	/* The outline of the factors. */
	error = make_outline(stroke, factors, 0U, 1U, &outline, &outline_count);
	if (error != 0)
		return error;

	/* The one shape. */
	error = shapes_alloc(stroke, 1U);
	if (error != 0) {
		pdf_outline_free(outline);
		return error;
	}

	/* Succeeded: the outline is the stroke's one shape. */
	stroke->outline = outline;
	stroke->outline_count = outline_count;
	shape_init(&stroke->shapes[0], outline, outline_count, 1.0f, NOTES_PAINT_SOLID, NOTES_BLEND_NORMAL);
	return 0;
}

/*
 * Makes the pencil's shapes: the stroke cut into ranges where its darkness
 * steps change (libpdf moves each cut to a place the ranges can meet
 * without overlap, or drops it), each range a shape of its step's
 * opacity through the grain, the first range also the outline.
 */
static int
make_pencil(
	struct notes_stroke *stroke,
	const uint16_t *factors,
	const unsigned char *steps)
{
	struct pdf_outline_style style;
	struct pdf_factor_point *points;
	struct pdf_outline_range *ranges;
	size_t range_count;
	size_t *cuts;
	size_t cut_count;
	size_t index;
	float alpha;
	int error;

	/* Allocates the cuts, one at most a sample. */
	cuts = malloc(stroke->point_count * sizeof(*cuts));
	if (cuts == NULL)
		return ENOMEM;

	/* A cut where a sample's step differs from the one before. */
	cut_count = 0;
	for (index = 1; index < stroke->point_count; index++) {
		if (steps[index] == steps[index - 1U])
			continue;
		cuts[cut_count] = index;
		cut_count++;
	}

	/* Allocates the samples in libpdf's form. */
	points = malloc(stroke->point_count * sizeof(*points));
	if (points == NULL) {
		free(cuts);
		return ENOMEM;
	}

	/* Each sample: its position and its width factor. */
	for (index = 0; index < stroke->point_count; index++) {
		points[index].x = stroke->points[index].x;
		points[index].y = stroke->points[index].y;
		points[index].factor = factor_value(factors[index]);
	}

	/* The ranges. */
	style.floor = SHAPE_STYLE_FLOOR;
	style.exponent = SHAPE_STYLE_EXPONENT;
	style.factor_max = NOTES_FACTOR_MAX;
	style.radius_min = SHAPE_STYLE_RADIUS_MIN;
	error = pdf_outline_stroke_ranges(points, stroke->point_count, stroke->width, &style, cuts, cut_count, &ranges, &range_count);
	free(points);
	free(cuts);
	if (error != 0)
		return error;

	/* A stroke has one range at least. */
	if (range_count == 0U) {
		pdf_outline_ranges_free(ranges, range_count);
		return EINVAL;
	}

	/* One shape a range. */
	error = shapes_alloc(stroke, range_count);
	if (error != 0) {
		pdf_outline_ranges_free(ranges, range_count);
		return error;
	}

	/* Each range's shape takes its polygon over, at the opacity of the step that began it. */
	for (index = 0; index < range_count; index++) {
		alpha = SHAPE_PENCIL_ALPHA_BASE + SHAPE_PENCIL_ALPHA_SPAN * (float)steps[ranges[index].start] / (float)NOTES_PENCIL_STEPS;
		shape_init(&stroke->shapes[index], ranges[index].corners, ranges[index].count, alpha, NOTES_PAINT_GRAIN, NOTES_BLEND_NORMAL);
		stroke->shapes[index].seam_start = ranges[index].seam_start;
		stroke->shapes[index].seam_end = ranges[index].seam_end;
		ranges[index].corners = NULL;
	}

	/* The ranges' list goes, their polygons are the shapes'. */
	pdf_outline_ranges_free(ranges, range_count);

	/* Succeeded: the first range is the outline (ws216-p004: the screen draws the shapes). */
	stroke->outline = stroke->shapes[0].polygon;
	stroke->outline_count = stroke->shapes[0].corner_count;
	return 0;
}

/*
 * Makes the watercolour's shapes: its core and its two edges, each laid
 * over what is under it by multiplying, the outer edge also the outline.
 */
static int
make_watercolor(
	struct notes_stroke *stroke,
	const uint16_t *factors)
{
	static const float alphas[SHAPE_WATER_COLUMNS] = {
		SHAPE_WATER_ALPHA_CORE,
		SHAPE_WATER_ALPHA_EDGE1,
		SHAPE_WATER_ALPHA_EDGE2
	};
	struct pdf_point *outline;
	size_t outline_count;
	unsigned column;
	int error;

	/* The three shapes. */
	error = shapes_alloc(stroke, SHAPE_WATER_COLUMNS);
	if (error != 0)
		return error;

	/* Each column's outline, at its opacity. */
	for (column = 0; column < SHAPE_WATER_COLUMNS; column++) {
		error = make_outline(stroke, factors, column, SHAPE_WATER_COLUMNS, &outline, &outline_count);
		if (error != 0)
			return error;
		shape_init(&stroke->shapes[column], outline, outline_count, alphas[column], NOTES_PAINT_SOLID, NOTES_BLEND_MULTIPLY);
	}

	/* Succeeded: the outer edge is the outline. */
	stroke->outline = stroke->shapes[SHAPE_WATER_COLUMNS - 1U].polygon;
	stroke->outline_count = stroke->shapes[SHAPE_WATER_COLUMNS - 1U].corner_count;
	return 0;
}

/*
 * Makes a fill's shape: its samples are the corners of its contours, the
 * first contour also the outline.
 */
static int
make_fill(
	struct notes_stroke *stroke)
{
	struct pdf_point *polygon;
	size_t index;
	size_t total;
	int error;

	/* Refuses contours that are missing, too small or do not add up to the samples. */
	if (stroke->contours == NULL || stroke->contour_count == 0U)
		return EINVAL;
	total = 0;
	for (index = 0; index < stroke->contour_count; index++) {
		if (stroke->contours[index] < 3U)
			return EINVAL;
		total += stroke->contours[index];
	}
	if (total != stroke->point_count)
		return EINVAL;

	/* Allocates the corners. */
	polygon = malloc(stroke->point_count * sizeof(*polygon));
	if (polygon == NULL)
		return ENOMEM;

	/* Each corner is a sample. */
	for (index = 0; index < stroke->point_count; index++) {
		polygon[index].x = stroke->points[index].x;
		polygon[index].y = stroke->points[index].y;
	}

	/* The one shape. */
	error = shapes_alloc(stroke, 1U);
	if (error != 0) {
		free(polygon);
		return error;
	}

	/* Succeeded: one opaque shape of every contour, its first contour the outline. */
	shape_init(&stroke->shapes[0], polygon, stroke->point_count, 1.0f, NOTES_PAINT_SOLID, NOTES_BLEND_NORMAL);
	stroke->shapes[0].contours = stroke->contours;
	stroke->shapes[0].contour_count = stroke->contour_count;
	stroke->outline = polygon;
	stroke->outline_count = stroke->contours[0];
	return 0;
}

/* Makes the outline of one column of a stroke's factors with libpdf's style. */
static int
make_outline(
	const struct notes_stroke *stroke,
	const uint16_t *factors,
	unsigned column,
	unsigned columns,
	struct pdf_point **outline,
	size_t *count)
{
	struct pdf_outline_style style;
	struct pdf_factor_point *points;
	size_t index;
	int error;

	/* Allocates the samples in libpdf's form. */
	points = malloc(stroke->point_count * sizeof(*points));
	if (points == NULL)
		return ENOMEM;

	/* Each sample: its position and its factor of the column. */
	for (index = 0; index < stroke->point_count; index++) {
		points[index].x = stroke->points[index].x;
		points[index].y = stroke->points[index].y;
		points[index].factor = factor_value(factors[index * columns + column]);
	}

	/* The outline: half the width times the factor, as it is. */
	style.floor = SHAPE_STYLE_FLOOR;
	style.exponent = SHAPE_STYLE_EXPONENT;
	style.factor_max = NOTES_FACTOR_MAX;
	style.radius_min = SHAPE_STYLE_RADIUS_MIN;
	error = pdf_outline_stroke_style(points, stroke->point_count, stroke->width, &style, outline, count);
	free(points);
	if (error != 0)
		return error;

	/* Succeeded: the caller owns the outline. */
	return 0;
}

/* Allocates a stroke's shapes, all empty. */
static int
shapes_alloc(
	struct notes_stroke *stroke,
	size_t count)
{
	/* Allocates them cleared, so that a failure part way frees only what was made. */
	stroke->shapes = calloc(count, sizeof(stroke->shapes[0]));
	if (stroke->shapes == NULL)
		return ENOMEM;

	/* Succeeded: the stroke has room for its shapes. */
	stroke->shape_count = count;
	return 0;
}

/* Fills in a shape of one contour without seams. */
static void
shape_init(
	struct notes_shape *shape,
	struct pdf_point *polygon,
	size_t corner_count,
	float alpha,
	unsigned paint,
	unsigned blend)
{
	/* The polygon and how it is painted. */
	memset(shape, 0, sizeof(*shape));
	shape->polygon = polygon;
	shape->corner_count = corner_count;
	shape->contour_count = 1U;
	shape->alpha = alpha;
	shape->paint = paint;
	shape->blend = blend;

	/* No range meets it. */
	shape->seam_start = PDF_OUTLINE_NO_SEAM;
	shape->seam_end = PDF_OUTLINE_NO_SEAM;
}

/*
 * Computes the values the formulas give a stroke's samples now: its factor
 * columns and, for the pencil, its steps (NULL for the other tools).  The
 * caller owns both.
 */
static int
compute_values(
	const struct notes_stroke *stroke,
	uint16_t **factors,
	unsigned char **steps)
{
	double *lengths;
	double dx;
	double dy;
	unsigned columns;
	size_t index;

	/* No values yet. */
	*factors = NULL;
	*steps = NULL;
	columns = notes_tool_columns(stroke->tool);

	/* Allocates the arc lengths of the samples. */
	lengths = malloc(stroke->point_count * sizeof(*lengths));
	if (lengths == NULL)
		return ENOMEM;

	/* The length of the path from the first sample to each. */
	lengths[0] = 0.0;
	for (index = 1; index < stroke->point_count; index++) {
		dx = (double)stroke->points[index].x - (double)stroke->points[index - 1U].x;
		dy = (double)stroke->points[index].y - (double)stroke->points[index - 1U].y;
		lengths[index] = lengths[index - 1U] + sqrt(dx * dx + dy * dy);
	}

	/* Allocates the factors. */
	*factors = malloc(stroke->point_count * columns * sizeof(**factors));
	if (*factors == NULL) {
		free(lengths);
		return ENOMEM;
	}

	/* Allocates the pencil's steps. */
	if (stroke->tool == NOTES_TOOL_PENCIL) {
		*steps = malloc(stroke->point_count);
		if (*steps == NULL) {
			free(*factors);
			*factors = NULL;
			free(lengths);
			return ENOMEM;
		}
	}

	/* The tool's formulas. */
	switch (stroke->tool) {
	case NOTES_TOOL_BRUSH:
		/* The pressure, the speed and the taper. */
		compute_brush(stroke, lengths, *factors);
		break;
	case NOTES_TOOL_PENCIL:
		/* The pressure's width and darkness. */
		compute_pencil(stroke, lengths, *factors, *steps);
		break;
	default:
		/* The core and the wobbling edges. */
		compute_watercolor(stroke, lengths, *factors);
		break;
	}

	/* Succeeded: the arc lengths are not needed any more. */
	free(lengths);
	return 0;
}

/*
 * Computes the brush's width factors (section 3.1): the pressure, times a
 * speed term that thins a fast stroke, times a taper at the entry and the
 * exit.
 */
static void
compute_brush(
	const struct notes_stroke *stroke,
	const double *lengths,
	uint16_t *factors)
{
	const struct notes_point *points;
	double total;
	double entry;
	double exit;
	double pressure;
	double speed;
	double speed_term;
	double taper;
	double rest;
	double factor;
	size_t count;
	size_t before;
	size_t after;
	size_t index;

	/* The stroke's length and its taper's lengths. */
	points = stroke->points;
	count = stroke->point_count;
	total = lengths[count - 1U];
	entry = SHAPE_BRUSH_ENTRY_WIDTHS * (double)stroke->width;
	if (entry > SHAPE_BRUSH_ENTRY_SHARE * total)
		entry = SHAPE_BRUSH_ENTRY_SHARE * total;
	exit = SHAPE_BRUSH_EXIT_WIDTHS * (double)stroke->width;
	if (exit > SHAPE_BRUSH_EXIT_SHARE * total)
		exit = SHAPE_BRUSH_EXIT_SHARE * total;

	/* Each sample, with the window of its speed moving along. */
	before = 0;
	after = 0;
	for (index = 0; index < count; index++) {
		/* The pressure, never quite nothing. */
		pressure = sample_pressure(&points[index]);
		if (pressure < SHAPE_BRUSH_PRESSURE_MIN)
			pressure = SHAPE_BRUSH_PRESSURE_MIN;

		/* The nearest sample the window's time before this one (the first when none). */
		while (before + 1U <= index &&
		       points[before + 1U].time_ms + SHAPE_BRUSH_WINDOW_MS <= points[index].time_ms)
			before++;

		/* The nearest sample the window's time after this one (the last when none). */
		if (after < index)
			after = index;
		while (after + 1U < count &&
		       points[after].time_ms < points[index].time_ms + SHAPE_BRUSH_WINDOW_MS)
			after++;

		/* The speed over the window; samples of one time only are slow. */
		speed = 0.0;
		if (points[after].time_ms > points[before].time_ms)
			speed = (lengths[after] - lengths[before]) / (double)(points[after].time_ms - points[before].time_ms);

		/* The speed's term, within its least and its top. */
		speed_term = SHAPE_BRUSH_SPEED_TOP - SHAPE_BRUSH_SPEED_SLOPE * speed / SHAPE_BRUSH_SPEED_REFERENCE;
		if (speed_term < SHAPE_BRUSH_SPEED_LEAST)
			speed_term = SHAPE_BRUSH_SPEED_LEAST;
		if (speed_term > SHAPE_BRUSH_SPEED_TOP)
			speed_term = SHAPE_BRUSH_SPEED_TOP;

		/* The entry's taper. */
		taper = 1.0;
		if (entry > 0.0)
			taper = SHAPE_BRUSH_TAPER_LEAST + (1.0 - SHAPE_BRUSH_TAPER_LEAST) * smooth(lengths[index] / entry);

		/* The exit's, where it is the thinner. */
		if (exit > 0.0) {
			rest = SHAPE_BRUSH_TAPER_LEAST + (1.0 - SHAPE_BRUSH_TAPER_LEAST) * smooth((total - lengths[index]) / exit);
			if (rest < taper)
				taper = rest;
		}

		/* The factor. */
		factor = pressure * speed_term * taper;
		factors[index] = factor_units(factor);
	}
}

/*
 * Computes the pencil's width factors and darkness steps (section 3.2):
 * a step of the pressure is taken only when it lasts the hold's length,
 * so that a wavering pressure does not cut the stroke into crumbs.
 */
static void
compute_pencil(
	const struct notes_stroke *stroke,
	const double *lengths,
	uint16_t *factors,
	unsigned char *steps)
{
	unsigned char *raw;
	double pressure;
	double factor;
	size_t count;
	size_t index;
	size_t last;
	size_t run;
	unsigned current;

	/* The steps of each sample's own pressure are first kept in the result. */
	raw = steps;
	count = stroke->point_count;

	/* Each sample's width factor and step of its own pressure. */
	for (index = 0; index < count; index++) {
		pressure = sample_pressure(&stroke->points[index]);
		factor = SHAPE_PENCIL_BASE + SHAPE_PENCIL_SPAN * pow(pressure, SHAPE_PENCIL_EXPONENT);
		factors[index] = factor_units(factor);
		raw[index] = (unsigned char)floor((double)NOTES_PENCIL_STEPS * pow(pressure, SHAPE_PENCIL_STEP_EXPONENT) + 0.5);
	}

	/* Each run of one step: a new step is taken when the run lasts the hold, else the step before goes on. */
	current = raw[0];
	index = 1;
	while (index < count) {
		/* A sample of the step that stands. */
		if (raw[index] == current) {
			index++;
			continue;
		}

		/* The run of the new step. */
		last = index;
		while (last + 1U < count && raw[last + 1U] == raw[index])
			last++;

		/* A run long enough changes the step. */
		if (lengths[last] - lengths[index] >= SHAPE_PENCIL_STEP_HOLD)
			current = raw[index];

		/* The run's samples have the step that stands. */
		for (run = index; run <= last; run++)
			steps[run] = (unsigned char)current;
		index = last + 1U;
	}
}

/*
 * Computes the watercolour's factors (section 3.3): its core follows the
 * pressure gently, and its two edges are wider by a wobble along the path.
 */
static void
compute_watercolor(
	const struct notes_stroke *stroke,
	const double *lengths,
	uint16_t *factors)
{
	double pressure;
	double core;
	double edge;
	double spacing;
	size_t index;

	/* The wobble's lattice spacing: the stroke's width. */
	spacing = (double)stroke->width;
	if (!(spacing > 0.0))
		spacing = 1.0;

	/* Each sample's core and edges. */
	for (index = 0; index < stroke->point_count; index++) {
		/* The core. */
		pressure = sample_pressure(&stroke->points[index]);
		core = SHAPE_WATER_CORE_BASE + SHAPE_WATER_CORE_SPAN * pressure;
		factors[index * SHAPE_WATER_COLUMNS] = factor_units(core);

		/* The first edge, wobbling with a seed of the stroke's number. */
		edge = SHAPE_WATER_EDGE1 * core * (1.0 + SHAPE_WATER_WOBBLE * wobble(lengths[index], spacing, stroke->id * 2U));
		factors[index * SHAPE_WATER_COLUMNS + 1U] = factor_units(edge);

		/* The second edge, with a seed of its own. */
		edge = SHAPE_WATER_EDGE2 * core * (1.0 + SHAPE_WATER_WOBBLE * wobble(lengths[index], spacing, stroke->id * 2U + 1U));
		factors[index * SHAPE_WATER_COLUMNS + 2U] = factor_units(edge);
	}
}

/* Turns a factor into its baked units, rounded and kept within 0 to NOTES_FACTOR_MAX. */
static uint16_t
factor_units(
	double factor)
{
	/* A factor below nothing (or not a number) is nothing. */
	if (!(factor > 0.0))
		return 0U;

	/* A factor past the largest is the largest. */
	if (factor >= (double)NOTES_FACTOR_MAX)
		return (uint16_t)NOTES_FACTOR_UNITS;

	/* Reports the nearest unit. */
	return (uint16_t)floor(factor / (double)NOTES_FACTOR_MAX * (double)NOTES_FACTOR_UNITS + 0.5);
}

/* Turns baked units into the factor they stand for. */
static double
factor_value(
	uint16_t units)
{
	/* Reports the share of the largest factor. */
	return (double)units / (double)NOTES_FACTOR_UNITS * (double)NOTES_FACTOR_MAX;
}

/* Reports a sample's pressure from 0 to 1. */
static double
sample_pressure(
	const struct notes_point *point)
{
	/* The share of the pressure's range. */
	return (double)point->pressure / (double)NOTES_PRESSURE_MAX;
}

/* Reports the smoothstep of a share, kept within 0 to 1. */
static double
smooth(
	double share)
{
	/* Below the start and past the end it is flat. */
	if (!(share > 0.0))
		return 0.0;
	if (share >= 1.0)
		return 1.0;

	/* Reports the cubic that leaves and arrives flat. */
	return share * share * (3.0 - 2.0 * share);
}

/*
 * Reports a one-dimensional value noise of a length along a path, from -1
 * to 1: values of a hash at lattice points a spacing apart, joined by the
 * smoothstep.
 */
static double
wobble(
	double length,
	double spacing,
	uint32_t seed)
{
	double position;
	double share;
	double first;
	double second;
	uint32_t point;

	/* The lattice point before the length, and the share of the way to the next. */
	position = length / spacing;
	if (!(position > 0.0))
		position = 0.0;
	point = (uint32_t)floor(position);
	share = position - floor(position);

	/* The values at the two lattice points, from -1 to 1. */
	first = (double)(mix(point ^ mix(seed)) & 0xffffU) / 32767.5 - 1.0;
	second = (double)(mix((point + 1U) ^ mix(seed)) & 0xffffU) / 32767.5 - 1.0;

	/* Reports the value between them. */
	return first + (second - first) * smooth(share);
}

/* Mixes the bits of a number (a 32-bit integer hash). */
static uint32_t
mix(
	uint32_t value)
{
	/* Folds and multiplies until every bit depends on every other. */
	value ^= value >> 16;
	value *= 0x7feb352dU;
	value ^= value >> 15;
	value *= 0x846ca68bU;
	value ^= value >> 16;

	/* Reports the mixed number. */
	return value;
}

/* Reports the grain's value at a lattice point (0 to 255), repeating every side points. */
static unsigned
grain_lattice(
	unsigned x,
	unsigned y,
	unsigned side,
	uint32_t seed)
{
	uint32_t hash;

	/* The point's hash, the lattice wrapped so that the tile repeats. */
	hash = mix((x % side) + mix((y % side) + mix(seed)));

	/* Reports its top byte. */
	return (unsigned)(hash >> 24);
}

/*
 * Reports the value noise of the grain at a texel (0 to 255): the lattice
 * values a step of texels apart, joined by the integer weights of the
 * smoothstep (in 1/256).
 */
static unsigned
grain_noise(
	unsigned x,
	unsigned y,
	unsigned step,
	const unsigned *weights,
	uint32_t seed)
{
	unsigned side;
	unsigned left;
	unsigned top;
	unsigned across;
	unsigned down;
	unsigned upper;
	unsigned lower;

	/* The lattice cell of the texel, and its weights across and down. */
	side = NOTES_GRAIN_SIDE / step;
	left = x / step;
	top = y / step;
	across = weights[x % step];
	down = weights[y % step];

	/* The upper and the lower pair of lattice values, each joined across. */
	upper = grain_lattice(left, top, side, seed) * (256U - across) + grain_lattice(left + 1U, top, side, seed) * across;
	lower = grain_lattice(left, top + 1U, side, seed) * (256U - across) + grain_lattice(left + 1U, top + 1U, side, seed) * across;

	/* Reports them joined down, back to 0 to 255. */
	return (upper * (256U - down) + lower * down + 32768U) >> 16;
}
