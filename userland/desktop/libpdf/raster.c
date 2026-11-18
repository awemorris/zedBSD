/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The CPU rasterizer of libpdf: a display list drawn into memory.
 *
 * A program that draws on the GPU renders the display list itself; this
 * one serves the viewers that draw on the CPU (PDF Viewer's first version,
 * a thumbnail, Notes' background of a page it did not write) and the host
 * tests, which compare it with other renderers.
 *
 * Paths are flattened into edges and filled scanline by scanline with
 * PDF_RASTER_SUBSAMPLES sub-scanlines per pixel row: each sub-scanline's
 * spans follow the fill rule exactly and are added to the row's coverage
 * with their exact horizontal extent, so edges are smoothed in both
 * directions.  Clips are coverage masks of the whole target, intersected
 * as they nest.  Images are sampled bilinearly, or averaged over the
 * pixel when they are shrunk; an image enlarged four times or more without
 * /Interpolate keeps its pixels square (the nearest one), as poppler and
 * pdf.js draw it.  Colours are blended in premultiplied form
 * by the Normal or Multiply blend mode.  A pattern's fill (ws216-p002) is
 * covered as a fill and coloured, pixel by pixel, by its repeated image,
 * sampled as an image with its edges wrapped around.
 */

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <pdf.h>

#include "internal.h"

/* The sub-scanlines of a pixel row. */
#define PDF_RASTER_SUBSAMPLES 5

/* How far a flattened curve may stray from the true one, in pixels. */
#define PDF_RASTER_TOLERANCE 0.2

/* The most pieces one curve is flattened into. */
#define PDF_RASTER_CURVE_PIECES 256

/* How deep clips may nest in the rasterizer; deeper ones are not applied. */
#define PDF_RASTER_CLIPS_MAX 16

/* The most samples across one pixel when an image is shrunk. */
#define PDF_RASTER_IMAGE_SAMPLES 4

/* The enlargement from which an image without /Interpolate is sampled at the nearest pixel. */
#define PDF_RASTER_IMAGE_BLOCKY 4.0

/* The largest target the rasterizer draws into, in pixels a side. */
#define PDF_RASTER_SIDE_MAX 32768

/*
 * One edge of a flattened path, top to bottom in pixels.
 *
 * direction is +1 for an edge that went down in the path and -1 for one
 * that went up; slope is how far x moves per pixel of y.
 */
struct raster_edge {
	double top;
	double bottom;
	double x;
	double slope;
	int direction;
};

/*
 * One crossing of a sub-scanline with an edge.
 */
struct raster_crossing {
	double x;
	int direction;
};

/*
 * A rasterization in progress: the target, the transform from the page,
 * the edges of the path being filled, the rows of coverage, the clip masks
 * and the scratch arrays.
 *
 * masks[mask_depth - 1] is the clip in force (NULL entries do not exist:
 * mask_depth 0 is no clip); ignored_clips counts clip pushes past the
 * limit, which their pops only uncount.  row_left and row_right bound the
 * coverage of the row last computed.  The spare arrays are the merge
 * sorts' room (the C library's qsort is an insertion sort, too slow for the
 * thousands of edges of a page's strokes).
 */
struct raster {
	uint32_t *pixels;
	size_t stride;
	int width;
	int height;
	double scale;
	double offset_x;
	double offset_y;
	struct raster_edge *edges;
	size_t edge_count;
	size_t edge_capacity;
	size_t *active;
	size_t active_capacity;
	struct raster_crossing *crossings;
	struct raster_crossing *crossings_spare;
	size_t crossing_capacity;
	struct raster_edge *edges_spare;
	size_t edges_spare_capacity;
	float *coverage;
	float *steps;
	int row_left;
	int row_right;
	unsigned char *masks[PDF_RASTER_CLIPS_MAX];
	int mask_depth;
	size_t ignored_clips;
};

static int build_edges(struct raster *raster, const struct pdf_display_item *item);
static int add_edge(struct raster *raster, double x0, double y0, double x1, double y1);
static int flatten_curve(struct raster *raster, const struct pdf_point *start, const struct pdf_point *controls, struct pdf_point *end);
static void to_pixel(const struct raster *raster, const struct pdf_point *point, struct pdf_point *pixel);
static int sort_edges(struct raster *raster);
static int fill_item(struct raster *raster, const struct pdf_display_item *item);
static int push_clip(struct raster *raster, const struct pdf_display_item *item);
static void pop_clip(struct raster *raster);
static int scan_path(struct raster *raster, enum pdf_fill_rule rule, int *top, int *bottom);
static int row_coverage(struct raster *raster, enum pdf_fill_rule rule, int row, size_t *next_edge, size_t *active_count);
static void add_span(struct raster *raster, double left, double right, float weight);
static void blend_pixel(uint32_t *pixel, const unsigned color[3], unsigned alpha, enum pdf_blend_mode blend);
static unsigned to_level(double value);
static void sort_crossings(struct raster *raster, size_t count);
static int draw_image(struct raster *raster, const struct pdf_display_item *item);
static void sample_image(const struct pdf_display_item *item, double u, double v, double sample[4]);
static void sample_nearest(const struct pdf_display_item *item, double u, double v, double sample[4]);
static void texel(const struct pdf_display_item *item, long x, long y, double sample[4]);
static int fill_pattern(struct raster *raster, const struct pdf_display_item *item);
static void sample_repeated(const struct pdf_display_item *item, double u, double v, double sample[4]);
static long wrap_index(long index, size_t size);

/*
 * Draws a display list into 32-bit pixels.
 *
 * The pixels are premultiplied 0xAARRGGBB words, stride words a row, and
 * are drawn over as they are (the caller clears them, usually to the
 * page's white).  A page point p lands on the pixel p * scale + offset.
 */
int
pdf_display_list_rasterize(
	const struct pdf_display_list *list,
	uint32_t *pixels,
	size_t stride,
	size_t width,
	size_t height,
	double scale,
	double offset_x,
	double offset_y)
{
	struct raster raster;
	const struct pdf_display_item *item;
	size_t index;
	int level;
	int error;

	/* Refuses a missing list or target, or one of an impossible size. */
	if (list == NULL || pixels == NULL)
		return EINVAL;
	if (width == 0 || height == 0)
		return EINVAL;
	if (width > PDF_RASTER_SIDE_MAX || height > PDF_RASTER_SIDE_MAX)
		return EINVAL;
	if (stride < width)
		return EINVAL;
	if (!(scale > 0.0 && scale < 1e6))
		return EINVAL;

	/* Starts with no edges and no clip. */
	memset(&raster, 0, sizeof(raster));
	raster.pixels = pixels;
	raster.stride = stride;
	raster.width = (int)width;
	raster.height = (int)height;
	raster.scale = scale;
	raster.offset_x = offset_x;
	raster.offset_y = offset_y;

	/* Allocates the row of coverage and the row of its steps, one more than the width. */
	raster.coverage = calloc(width + 2, sizeof(float));
	if (raster.coverage == NULL)
		return ENOMEM;
	raster.steps = calloc(width + 2, sizeof(float));
	if (raster.steps == NULL) {
		free(raster.coverage);
		return ENOMEM;
	}

	/* Draws each item in order, until one fails. */
	error = 0;
	for (index = 0; index < list->count; index++) {
		item = &list->items[index];
		switch (item->type) {
		case PDF_ITEM_FILL:
			error = fill_item(&raster, item);
			break;
		case PDF_ITEM_IMAGE:
			error = draw_image(&raster, item);
			break;
		case PDF_ITEM_CLIP_PUSH:
			error = push_clip(&raster, item);
			break;
		case PDF_ITEM_CLIP_POP:
			pop_clip(&raster);
			break;
		case PDF_ITEM_PATTERN:
			error = fill_pattern(&raster, item);
			break;
		}

		/* A failure ends the drawing. */
		if (error != 0)
			break;
	}

	/* Frees the masks and the scratch arrays. */
	for (level = 0; level < raster.mask_depth; level++)
		free(raster.masks[level]);
	free(raster.edges);
	free(raster.edges_spare);
	free(raster.active);
	free(raster.crossings);
	free(raster.crossings_spare);
	free(raster.coverage);
	free(raster.steps);
	if (error != 0)
		return error;

	/* Succeeded: the list is drawn. */
	return 0;
}

/* Flattens an item's path into the raster's edges, in pixels, each subpath closed. */
static int
build_edges(
	struct raster *raster,
	const struct pdf_display_item *item)
{
	struct pdf_point start;
	struct pdf_point current;
	struct pdf_point next;
	size_t verb;
	size_t point;
	int error;

	/* Starts with no edges and no subpath. */
	raster->edge_count = 0;
	point = 0;
	start.x = 0.0;
	start.y = 0.0;
	current = start;

	/* Walks the verbs. */
	for (verb = 0; verb < item->verb_count; verb++) {
		switch (item->verbs[verb]) {
		case PDF_PATH_MOVE:
			/* Closes the subpath before, then starts one. */
			if (point + 1 > item->point_count)
				return PDF_EFORMAT;
			error = add_edge(raster, current.x, current.y, start.x, start.y);
			if (error != 0)
				return error;
			to_pixel(raster, &item->points[point], &start);
			current = start;
			point++;
			break;
		case PDF_PATH_LINE:
			if (point + 1 > item->point_count)
				return PDF_EFORMAT;
			to_pixel(raster, &item->points[point], &next);
			error = add_edge(raster, current.x, current.y, next.x, next.y);
			if (error != 0)
				return error;
			current = next;
			point++;
			break;
		case PDF_PATH_CUBIC:
			if (point + 3 > item->point_count)
				return PDF_EFORMAT;
			error = flatten_curve(raster, &current, &item->points[point], &next);
			if (error != 0)
				return error;
			current = next;
			point += 3;
			break;
		case PDF_PATH_CLOSE:
			error = add_edge(raster, current.x, current.y, start.x, start.y);
			if (error != 0)
				return error;
			current = start;
			break;
		default:
			return PDF_EFORMAT;
		}
	}

	/* Closes the last subpath. */
	error = add_edge(raster, current.x, current.y, start.x, start.y);
	if (error != 0)
		return error;

	/* Succeeded: the path is edges. */
	return 0;
}

/* Adds an edge between two pixel positions; a horizontal edge crosses no scanline and is dropped. */
static int
add_edge(
	struct raster *raster,
	double x0,
	double y0,
	double x1,
	double y1)
{
	struct raster_edge *edge;
	struct raster_edge *grown;
	size_t capacity;

	/* Drops a horizontal edge and one that is not a number. */
	if (!(y0 != y1))
		return 0;
	if (!(x0 == x0 && x1 == x1))
		return 0;

	/* Grows the edges when full, within the point limit. */
	if (raster->edge_count == raster->edge_capacity) {
		if (raster->edge_capacity >= PDF_DISPLAY_POINTS_MAX * 4)
			return ENOMEM;
		capacity = raster->edge_capacity * 2;
		if (capacity == 0)
			capacity = 256;
		grown = realloc(raster->edges, capacity * sizeof(*grown));
		if (grown == NULL)
			return ENOMEM;
		raster->edges = grown;
		raster->edge_capacity = capacity;
	}

	/* Stores the edge from its top, with the direction it went. */
	edge = &raster->edges[raster->edge_count];
	if (y0 < y1) {
		edge->top = y0;
		edge->bottom = y1;
		edge->x = x0;
		edge->direction = 1;
	} else {
		edge->top = y1;
		edge->bottom = y0;
		edge->x = x1;
		edge->direction = -1;
	}

	/* Its slope, and one edge more. */
	edge->slope = (x1 - x0) / (y1 - y0);
	raster->edge_count++;

	/* Succeeded: the edge is added. */
	return 0;
}

/* Flattens a cubic curve from the current pixel position into edges, reporting its end in pixels. */
static int
flatten_curve(
	struct raster *raster,
	const struct pdf_point *start,
	const struct pdf_point *controls,
	struct pdf_point *end)
{
	struct pdf_point first;
	struct pdf_point second;
	struct pdf_point previous;
	struct pdf_point next;
	double bend_x;
	double bend_y;
	double bend;
	double other_bend;
	double pieces;
	double t;
	double u;
	long count;
	long piece;
	int error;

	/* The control points in pixels. */
	to_pixel(raster, &controls[0], &first);
	to_pixel(raster, &controls[1], &second);
	to_pixel(raster, &controls[2], end);

	/* The pieces Wang's formula needs for the tolerance. */
	bend_x = fabs(start->x - 2.0 * first.x + second.x);
	bend_y = fabs(start->y - 2.0 * first.y + second.y);
	bend = sqrt(bend_x * bend_x + bend_y * bend_y);
	bend_x = fabs(first.x - 2.0 * second.x + end->x);
	bend_y = fabs(first.y - 2.0 * second.y + end->y);
	other_bend = sqrt(bend_x * bend_x + bend_y * bend_y);
	if (other_bend > bend)
		bend = other_bend;
	pieces = ceil(sqrt(0.75 * bend / PDF_RASTER_TOLERANCE));
	count = 1;
	if (pieces > 1.0)
		count = (long)pieces;
	if (!(pieces <= (double)PDF_RASTER_CURVE_PIECES))
		count = PDF_RASTER_CURVE_PIECES;

	/* Adds an edge for each piece. */
	previous = *start;
	for (piece = 1; piece <= count; piece++) {
		t = (double)piece / (double)count;
		u = 1.0 - t;
		next.x = u * u * u * start->x + 3.0 * u * u * t * first.x + 3.0 * u * t * t * second.x + t * t * t * end->x;
		next.y = u * u * u * start->y + 3.0 * u * u * t * first.y + 3.0 * u * t * t * second.y + t * t * t * end->y;
		error = add_edge(raster, previous.x, previous.y, next.x, next.y);
		if (error != 0)
			return error;
		previous = next;
	}

	/* Succeeded: the curve is edges. */
	return 0;
}

/* Maps a page point to the target's pixels. */
static void
to_pixel(
	const struct raster *raster,
	const struct pdf_point *point,
	struct pdf_point *pixel)
{
	/* Scales and moves. */
	pixel->x = point->x * raster->scale + raster->offset_x;
	pixel->y = point->y * raster->scale + raster->offset_y;
}

/*
 * Sorts the edges by their tops with a bottom-up merge sort, stable and
 * in n log n, through the spare array.
 */
static int
sort_edges(
	struct raster *raster)
{
	struct raster_edge *source;
	struct raster_edge *target;
	struct raster_edge *swap;
	size_t count;
	size_t width;
	size_t start;
	size_t middle;
	size_t end;
	size_t left;
	size_t right;
	size_t place;

	/* Makes the spare array as large as the edges. */
	count = raster->edge_count;
	if (count > raster->edges_spare_capacity) {
		swap = realloc(raster->edges_spare, count * sizeof(*swap));
		if (swap == NULL)
			return ENOMEM;
		raster->edges_spare = swap;
		raster->edges_spare_capacity = count;
	}

	/* Merges runs of doubling width from one array into the other. */
	source = raster->edges;
	target = raster->edges_spare;
	for (width = 1; width < count; width *= 2) {
		for (start = 0; start < count; start += 2 * width) {
			middle = start + width;
			if (middle > count)
				middle = count;
			end = start + 2 * width;
			if (end > count)
				end = count;
			left = start;
			right = middle;
			for (place = start; place < end; place++) {
				if (left < middle &&
				    (right >= end ||
				     source[left].top <= source[right].top)) {
					target[place] = source[left];
					left++;
				} else {
					target[place] = source[right];
					right++;
				}
			}
		}

		/* The merged runs are the next pass's source. */
		swap = source;
		source = target;
		target = swap;
	}

	/* The sorted edges end in the edges' own array. */
	if (source != raster->edges) {
		raster->edges_spare = raster->edges;
		raster->edges = source;
		width = raster->edge_capacity;
		raster->edge_capacity = raster->edges_spare_capacity;
		raster->edges_spare_capacity = width;
	}

	/* Succeeded: the edges are in order from the top. */
	return 0;
}

/* Fills a fill item's path, row by row, blending its colour by coverage, alpha and clip. */
static int
fill_item(
	struct raster *raster,
	const struct pdf_display_item *item)
{
	const unsigned char *mask;
	unsigned color[3];
	unsigned level;
	size_t next_edge;
	size_t active_count;
	float coverage;
	float scale;
	int top;
	int bottom;
	int row;
	int x;
	int error;

	/* Nothing to draw at no alpha. */
	if (!(item->alpha > 0.0))
		return 0;

	/* The colour as levels of 0 to 255, and the alpha that scales the coverage to a level. */
	color[0] = to_level(item->red);
	color[1] = to_level(item->green);
	color[2] = to_level(item->blue);
	scale = (float)(item->alpha * 255.0);
	if (item->alpha > 1.0)
		scale = 255.0f;

	/* Flattens the path and finds the rows it touches. */
	error = build_edges(raster, item);
	if (error != 0)
		return error;
	error = scan_path(raster, item->rule, &top, &bottom);
	if (error != 0)
		return error;

	/* Blends each row's covered pixels. */
	mask = NULL;
	if (raster->mask_depth > 0)
		mask = raster->masks[raster->mask_depth - 1];
	next_edge = 0;
	active_count = 0;
	for (row = top; row < bottom; row++) {
		error = row_coverage(raster, item->rule, row, &next_edge, &active_count);
		if (error != 0)
			return error;
		for (x = raster->row_left; x <= raster->row_right; x++) {
			/* The pixel's coverage, within the clip, as a level of the alpha. */
			coverage = raster->coverage[x];
			if (coverage <= 0.0f)
				continue;
			if (coverage > 1.0f)
				coverage = 1.0f;
			if (mask != NULL)
				coverage *= (float)mask[(size_t)row * (size_t)raster->width + (size_t)x] / 255.0f;
			level = (unsigned)(coverage * scale + 0.5f);
			if (level == 0U)
				continue;
			blend_pixel(raster->pixels + (size_t)row * raster->stride + (size_t)x, color, level, item->blend);
		}
	}

	/* Succeeded: the fill is drawn. */
	return 0;
}

/* Pushes a clip: the path's coverage intersected with the clip in force, as a mask of the whole target. */
static int
push_clip(
	struct raster *raster,
	const struct pdf_display_item *item)
{
	unsigned char *mask;
	const unsigned char *parent;
	size_t next_edge;
	size_t active_count;
	size_t offset;
	double coverage;
	int top;
	int bottom;
	int row;
	int x;
	int error;

	/* Past the limit, a clip is not applied but counted, so that its pop matches. */
	if (raster->mask_depth == PDF_RASTER_CLIPS_MAX) {
		raster->ignored_clips++;
		return 0;
	}

	/* Allocates the mask, clear: nothing outside the path is drawn. */
	mask = calloc((size_t)raster->width * (size_t)raster->height, 1);
	if (mask == NULL)
		return ENOMEM;
	raster->masks[raster->mask_depth] = mask;
	raster->mask_depth++;
	parent = NULL;
	if (raster->mask_depth > 1)
		parent = raster->masks[raster->mask_depth - 2];

	/* Flattens the path and finds the rows it touches. */
	error = build_edges(raster, item);
	if (error != 0)
		return error;
	error = scan_path(raster, item->rule, &top, &bottom);
	if (error != 0)
		return error;

	/* Writes each covered pixel's coverage, times the clip it is inside. */
	next_edge = 0;
	active_count = 0;
	for (row = top; row < bottom; row++) {
		error = row_coverage(raster, item->rule, row, &next_edge, &active_count);
		if (error != 0)
			return error;
		for (x = raster->row_left; x <= raster->row_right; x++) {
			coverage = raster->coverage[x];
			if (coverage <= 0.0)
				continue;
			if (coverage > 1.0)
				coverage = 1.0;
			offset = (size_t)row * (size_t)raster->width + (size_t)x;
			if (parent != NULL)
				coverage *= (double)parent[offset] / 255.0;
			mask[offset] = (unsigned char)(coverage * 255.0 + 0.5);
		}
	}

	/* Succeeded: the clip is in force. */
	return 0;
}

/* Pops the innermost clip. */
static void
pop_clip(
	struct raster *raster)
{
	/* A pop of a clip past the limit only uncounts it. */
	if (raster->ignored_clips > 0) {
		raster->ignored_clips--;
		return;
	}

	/* A pop without a push does nothing. */
	if (raster->mask_depth == 0)
		return;

	/* Frees the mask; the one under it is in force again. */
	raster->mask_depth--;
	free(raster->masks[raster->mask_depth]);
	raster->masks[raster->mask_depth] = NULL;
}

/*
 * Prepares the scan of the edges: sorts them by their tops and finds the
 * rows of the target they touch (bottom is past the last one).
 */
static int
scan_path(
	struct raster *raster,
	enum pdf_fill_rule rule,
	int *top,
	int *bottom)
{
	double lowest;
	double highest;
	size_t index;
	size_t *active;
	struct raster_crossing *crossings;
	int error;

	/* No edges touch no rows. */
	(void)rule;
	*top = 0;
	*bottom = 0;
	if (raster->edge_count == 0)
		return 0;

	/* Sorts the edges from the top. */
	error = sort_edges(raster);
	if (error != 0)
		return error;

	/* The rows from the highest top to the lowest bottom, within the target. */
	highest = raster->edges[0].top;
	lowest = raster->edges[0].bottom;
	for (index = 1; index < raster->edge_count; index++) {
		if (raster->edges[index].bottom > lowest)
			lowest = raster->edges[index].bottom;
	}

	/* Within the target. */
	if (highest < 0.0)
		highest = 0.0;
	if (lowest > (double)raster->height)
		lowest = (double)raster->height;
	if (!(highest < lowest))
		return 0;
	*top = (int)floor(highest);
	*bottom = (int)ceil(lowest);

	/* Makes room for every edge to be active and to cross a sub-scanline. */
	if (raster->edge_count > raster->active_capacity) {
		active = realloc(raster->active, raster->edge_count * sizeof(*active));
		if (active == NULL)
			return ENOMEM;
		raster->active = active;
		crossings = realloc(raster->crossings, raster->edge_count * sizeof(*crossings));
		if (crossings == NULL)
			return ENOMEM;
		raster->crossings = crossings;
		crossings = realloc(raster->crossings_spare, raster->edge_count * sizeof(*crossings));
		if (crossings == NULL)
			return ENOMEM;
		raster->crossings_spare = crossings;
		raster->active_capacity = raster->edge_count;
		raster->crossing_capacity = raster->edge_count;
	}

	/* Succeeded: the edges are ready to scan. */
	return 0;
}

/*
 * Computes one row's coverage from its sub-scanlines' spans, keeping the
 * active edges between calls (the rows are scanned from the top).
 *
 * row_left and row_right bound the pixels with coverage; row_left >
 * row_right when there are none.
 */
static int
row_coverage(
	struct raster *raster,
	enum pdf_fill_rule rule,
	int row,
	size_t *next_edge,
	size_t *active_count)
{
	struct raster_edge *edge;
	double scanline;
	double left;
	float weight;
	float running;
	size_t index;
	size_t kept;
	size_t crossing_count;
	int sub;
	int winding;
	int inside;
	int was_inside;
	int x;

	/* Clears the row's coverage from the last row's bounds, and starts with none. */
	if (raster->row_left <= raster->row_right) {
		for (x = raster->row_left; x <= raster->row_right + 1; x++) {
			raster->coverage[x] = 0.0f;
			raster->steps[x] = 0.0f;
		}
	}

	/* No coverage yet in this row. */
	raster->row_left = raster->width;
	raster->row_right = -1;
	weight = 1.0f / (float)PDF_RASTER_SUBSAMPLES;

	/* Scans each sub-scanline of the row. */
	for (sub = 0; sub < PDF_RASTER_SUBSAMPLES; sub++) {
		scanline = (double)row + ((double)sub + 0.5) / (double)PDF_RASTER_SUBSAMPLES;

		/* Activates the edges that start above the sub-scanline. */
		while (*next_edge < raster->edge_count && raster->edges[*next_edge].top <= scanline) {
			raster->active[*active_count] = *next_edge;
			(*active_count)++;
			(*next_edge)++;
		}

		/* Drops the edges that end above it and collects the others' crossings. */
		kept = 0;
		crossing_count = 0;
		for (index = 0; index < *active_count; index++) {
			edge = &raster->edges[raster->active[index]];
			if (edge->bottom <= scanline)
				continue;
			raster->active[kept] = raster->active[index];
			kept++;
			if (edge->top > scanline)
				continue;
			raster->crossings[crossing_count].x = edge->x + (scanline - edge->top) * edge->slope;
			raster->crossings[crossing_count].direction = edge->direction;
			crossing_count++;
		}

		/* The edges still active. */
		*active_count = kept;

		/* Orders the crossings from the left. */
		if (crossing_count > 1)
			sort_crossings(raster, crossing_count);

		/* Adds the spans that are inside by the rule. */
		winding = 0;
		was_inside = 0;
		left = 0.0;
		for (index = 0; index < crossing_count; index++) {
			/* Inside by the rule: any winding for nonzero, an odd one for even-odd. */
			winding += raster->crossings[index].direction;
			inside = 0;
			if (rule == PDF_FILL_NONZERO && winding != 0)
				inside = 1;
			if (rule == PDF_FILL_EVEN_ODD && (winding & 1) != 0)
				inside = 1;
			if (inside && !was_inside)
				left = raster->crossings[index].x;
			if (!inside && was_inside)
				add_span(raster, left, raster->crossings[index].x, weight);
			was_inside = inside;
		}
	}

	/* Turns the steps of the whole pixels into coverage. */
	if (raster->row_left <= raster->row_right) {
		running = 0.0f;
		for (x = raster->row_left; x <= raster->row_right; x++) {
			running += raster->steps[x];
			raster->coverage[x] += running;
		}
	}

	/* Succeeded: the row's coverage. */
	return 0;
}

/*
 * Adds a span of one sub-scanline, from left to right in pixels, to the
 * row's coverage: the partial pixels at its ends directly, the whole ones
 * between as a step up and a step down.
 */
static void
add_span(
	struct raster *raster,
	double left,
	double right,
	float weight)
{
	int first;
	int last;

	/* Keeps the span within the target. */
	if (left < 0.0)
		left = 0.0;
	if (right > (double)raster->width)
		right = (double)raster->width;
	if (!(left < right))
		return;

	/* The pixels the span starts and ends in. */
	first = (int)left;
	last = (int)right;
	if (last >= raster->width)
		last = raster->width - 1;

	/* Widens the row's bounds. */
	if (first < raster->row_left)
		raster->row_left = first;
	if (last > raster->row_right)
		raster->row_right = last;

	/* A span within one pixel covers its length of it. */
	if (first == last) {
		raster->coverage[first] += (float)(right - left) * weight;
		return;
	}

	/* The partial pixels at both ends, and the whole ones between. */
	raster->coverage[first] += (float)((double)(first + 1) - left) * weight;
	raster->coverage[last] += (float)(right - (double)last) * weight;
	raster->steps[first + 1] += weight;
	raster->steps[last] -= weight;
}

/*
 * Blends a colour (levels of 0 to 255, not premultiplied) at an alpha level
 * over a premultiplied pixel, by the Normal or Multiply blend mode.
 *
 * Normal is the source over the backdrop.  Multiply adds, where both are,
 * the product of the two colours: with premultiplied source s and
 * backdrop d of alphas as and ab, the result is s (1 - ab) + d (1 - as) + s d.
 */
static void
blend_pixel(
	uint32_t *pixel,
	const unsigned color[3],
	unsigned alpha,
	enum pdf_blend_mode blend)
{
	unsigned backdrop[4];
	unsigned source[4];
	unsigned result[4];
	unsigned keep;
	int channel;

	/* The pixel's premultiplied channels, alpha first. */
	backdrop[0] = (*pixel >> 24) & 0xffU;
	backdrop[1] = (*pixel >> 16) & 0xffU;
	backdrop[2] = (*pixel >> 8) & 0xffU;
	backdrop[3] = *pixel & 0xffU;

	/* The source premultiplied, and what of the backdrop stays. */
	source[0] = alpha;
	source[1] = (color[0] * alpha + 127U) / 255U;
	source[2] = (color[1] * alpha + 127U) / 255U;
	source[3] = (color[2] * alpha + 127U) / 255U;
	keep = 255U - alpha;

	/* The alpha: the source over the backdrop in either mode. */
	result[0] = source[0] + (backdrop[0] * keep + 127U) / 255U;

	/* Each colour by the mode. */
	for (channel = 1; channel < 4; channel++) {
		result[channel] = source[channel] + (backdrop[channel] * keep + 127U) / 255U;
		if (blend == PDF_BLEND_MULTIPLY) {
			result[channel] = (source[channel] * (255U - backdrop[0]) + 127U) / 255U +
			    (backdrop[channel] * keep + 127U) / 255U +
			    (source[channel] * backdrop[channel] + 127U) / 255U;
		}

		/* A colour stays within a byte. */
		if (result[channel] > 255U)
			result[channel] = 255U;
	}

	/* So does the alpha. */
	if (result[0] > 255U)
		result[0] = 255U;

	/* Stores the result. */
	*pixel = (result[0] << 24) | (result[1] << 16) | (result[2] << 8) | result[3];
}

/* Converts a colour or alpha value from 0..1 to a level of 0 to 255, clamped and rounded. */
static unsigned
to_level(
	double value)
{
	/* Below and above the range, NaN below. */
	if (!(value > 0.0))
		return 0U;
	if (value >= 1.0)
		return 255U;

	/* Rounds to the nearest level. */
	return (unsigned)(value * 255.0 + 0.5);
}

/* Orders a sub-scanline's crossings from the left: few by insertion, many by a merge sort through the spare array. */
static void
sort_crossings(
	struct raster *raster,
	size_t count)
{
	struct raster_crossing *crossings;
	struct raster_crossing *source;
	struct raster_crossing *target;
	struct raster_crossing *swap;
	struct raster_crossing moving;
	size_t index;
	size_t place;
	size_t width;
	size_t start;
	size_t middle;
	size_t end;
	size_t left;
	size_t right;

	/* Few crossings: each moves left past the larger ones before it. */
	crossings = raster->crossings;
	if (count <= 24) {
		for (index = 1; index < count; index++) {
			moving = crossings[index];
			place = index;
			while (place > 0 && crossings[place - 1].x > moving.x) {
				crossings[place] = crossings[place - 1];
				place--;
			}

			/* The crossing in its place. */
			crossings[place] = moving;
		}

		/* A short list is in order. */
		return;
	}

	/* Many: runs of doubling width merged from one array into the other. */
	source = raster->crossings;
	target = raster->crossings_spare;
	for (width = 1; width < count; width *= 2) {
		for (start = 0; start < count; start += 2 * width) {
			middle = start + width;
			if (middle > count)
				middle = count;
			end = start + 2 * width;
			if (end > count)
				end = count;
			left = start;
			right = middle;
			for (place = start; place < end; place++) {
				if (left < middle &&
				    (right >= end ||
				     source[left].x <= source[right].x)) {
					target[place] = source[left];
					left++;
				} else {
					target[place] = source[right];
					right++;
				}
			}
		}

		/* The merged runs are the next pass's source. */
		swap = source;
		source = target;
		target = swap;
	}

	/* The sorted crossings end in the crossings' own array (the spare has the same size). */
	if (source != raster->crossings) {
		raster->crossings_spare = raster->crossings;
		raster->crossings = source;
	}
}

/*
 * Draws an image item: each target pixel whose centre falls on the image
 * takes the image's colour there.
 */
static int
draw_image(
	struct raster *raster,
	const struct pdf_display_item *item)
{
	const unsigned char *mask;
	struct pdf_point corners[4];
	struct pdf_point pixel;
	unsigned color[3];
	unsigned level;
	double determinant;
	double page_x;
	double page_y;
	double u;
	double v;
	double sample[4];
	double sum[4];
	double shrink;
	double alpha;
	double area;
	double left;
	double right;
	double top;
	double bottom;
	int samples;
	int nearest;
	int sub_x;
	int sub_y;
	int channel;
	int row;
	int x;
	int first_row;
	int last_row;
	int first_x;
	int last_x;
	size_t index;

	/* Nothing to draw at no alpha, or for an image squashed flat. */
	if (!(item->alpha > 0.0))
		return 0;
	determinant = item->matrix[0] * item->matrix[3] - item->matrix[1] * item->matrix[2];
	if (!(determinant > 1e-12 || determinant < -1e-12))
		return 0;

	/* The image's corners in pixels, and the rows and columns they span within the target. */
	for (index = 0; index < 4; index++) {
		u = (double)(index & 1U);
		v = (double)((index >> 1) & 1U);
		page_x = item->matrix[0] * u + item->matrix[2] * v + item->matrix[4];
		page_y = item->matrix[1] * u + item->matrix[3] * v + item->matrix[5];
		corners[index].x = page_x * raster->scale + raster->offset_x;
		corners[index].y = page_y * raster->scale + raster->offset_y;
	}

	/* The corners' bounds. */
	left = corners[0].x;
	right = corners[0].x;
	top = corners[0].y;
	bottom = corners[0].y;
	for (index = 1; index < 4; index++) {
		if (corners[index].x < left)
			left = corners[index].x;
		if (corners[index].x > right)
			right = corners[index].x;
		if (corners[index].y < top)
			top = corners[index].y;
		if (corners[index].y > bottom)
			bottom = corners[index].y;
	}

	/* Within the target. */
	if (left < 0.0)
		left = 0.0;
	if (top < 0.0)
		top = 0.0;
	if (right > (double)raster->width)
		right = (double)raster->width;
	if (bottom > (double)raster->height)
		bottom = (double)raster->height;
	if (!(left < right && top < bottom))
		return 0;
	first_x = (int)floor(left);
	last_x = (int)ceil(right) - 1;
	first_row = (int)floor(top);
	last_row = (int)ceil(bottom) - 1;

	/* How many image pixels fall on one target pixel: more than one and a half are averaged. */
	area = fabs(determinant) * raster->scale * raster->scale;
	shrink = sqrt((double)item->image_width * (double)item->image_height / area);
	samples = 1;
	if (shrink > 1.5)
		samples = (int)ceil(shrink);
	if (samples > PDF_RASTER_IMAGE_SAMPLES)
		samples = PDF_RASTER_IMAGE_SAMPLES;

	/* A large enlargement keeps the image's pixels square unless the image asks to be smoothed. */
	nearest = 0;
	if (!item->interpolate) {
		if (shrink * PDF_RASTER_IMAGE_BLOCKY <= 1.0)
			nearest = 1;
	}

	/* Draws each pixel of the span whose samples fall on the image. */
	mask = NULL;
	if (raster->mask_depth > 0)
		mask = raster->masks[raster->mask_depth - 1];
	for (row = first_row; row <= last_row; row++) {
		for (x = first_x; x <= last_x; x++) {
			/* Averages the samples of the pixel that fall on the image. */
			sum[0] = 0.0;
			sum[1] = 0.0;
			sum[2] = 0.0;
			sum[3] = 0.0;
			for (sub_y = 0; sub_y < samples; sub_y++) {
				for (sub_x = 0; sub_x < samples; sub_x++) {
					pixel.x = (double)x + ((double)sub_x + 0.5) / (double)samples;
					pixel.y = (double)row + ((double)sub_y + 0.5) / (double)samples;
					page_x = (pixel.x - raster->offset_x) / raster->scale - item->matrix[4];
					page_y = (pixel.y - raster->offset_y) / raster->scale - item->matrix[5];
					u = (item->matrix[3] * page_x - item->matrix[2] * page_y) / determinant;
					v = (-item->matrix[1] * page_x + item->matrix[0] * page_y) / determinant;
					if (!(u >= 0.0 &&
					      u < 1.0 &&
					      v >= 0.0 &&
					      v < 1.0))
						continue;
					if (nearest) {
						sample_nearest(item, u, v, sample);
					} else {
						sample_image(item, u, v, sample);
					}

					/* Adds the sample to the pixel's sums. */
					for (channel = 0; channel < 4; channel++)
						sum[channel] += sample[channel];
				}
			}

			/* A pixel no sample covered stays as it is. */
			if (sum[0] <= 0.0)
				continue;

			/* The sample's alpha, times the item's and the clip's. */
			alpha = sum[0] / (double)(samples * samples);
			alpha *= item->alpha;
			if (mask != NULL)
				alpha *= (double)mask[(size_t)row * (size_t)raster->width + (size_t)x] / 255.0;
			if (alpha <= 0.0)
				continue;

			/* Blends the colour, unpremultiplied from the sums. */
			color[0] = to_level(sum[1] / sum[0]);
			color[1] = to_level(sum[2] / sum[0]);
			color[2] = to_level(sum[3] / sum[0]);
			level = to_level(alpha);
			if (level == 0U)
				continue;
			blend_pixel(raster->pixels + (size_t)row * raster->stride + (size_t)x, color, level, item->blend);
		}
	}

	/* Succeeded: the image is drawn. */
	return 0;
}

/*
 * Samples an image at (u, v) of its unit square, bilinearly between the
 * four nearest pixels, as premultiplied alpha, red, green, blue from 0 to 1.
 */
static void
sample_image(
	const struct pdf_display_item *item,
	double u,
	double v,
	double sample[4])
{
	double x;
	double y;
	double fraction_x;
	double fraction_y;
	double corner[4][4];
	long left;
	long top;
	int channel;

	/* The position among the pixels' centres. */
	x = u * (double)item->image_width - 0.5;
	y = v * (double)item->image_height - 0.5;
	left = (long)floor(x);
	top = (long)floor(y);
	fraction_x = x - (double)left;
	fraction_y = y - (double)top;

	/* The four pixels around it (clamped at the image's edges). */
	texel(item, left, top, corner[0]);
	texel(item, left + 1, top, corner[1]);
	texel(item, left, top + 1, corner[2]);
	texel(item, left + 1, top + 1, corner[3]);

	/* Mixes them by the distances. */
	for (channel = 0; channel < 4; channel++) {
		sample[channel] = corner[0][channel] * (1.0 - fraction_x) * (1.0 - fraction_y) +
		    corner[1][channel] * fraction_x * (1.0 - fraction_y) +
		    corner[2][channel] * (1.0 - fraction_x) * fraction_y +
		    corner[3][channel] * fraction_x * fraction_y;
	}
}

/* Samples an image at (u, v) of its unit square at the pixel it falls in. */
static void
sample_nearest(
	const struct pdf_display_item *item,
	double u,
	double v,
	double sample[4])
{
	long x;
	long y;

	/* The pixel under the position (clamped at the image's edges by texel). */
	x = (long)floor(u * (double)item->image_width);
	y = (long)floor(v * (double)item->image_height);
	texel(item, x, y, sample);
}

/* Reads one image pixel, clamped to the image, as premultiplied alpha, red, green, blue from 0 to 1. */
static void
texel(
	const struct pdf_display_item *item,
	long x,
	long y,
	double sample[4])
{
	const unsigned char *pixel;
	double alpha;

	/* Clamps the position to the image. */
	if (x < 0)
		x = 0;
	if (y < 0)
		y = 0;
	if ((size_t)x >= item->image_width)
		x = (long)item->image_width - 1;
	if ((size_t)y >= item->image_height)
		y = (long)item->image_height - 1;

	/* Reads the pixel and premultiplies it. */
	pixel = item->pixels + ((size_t)y * item->image_width + (size_t)x) * 4;
	alpha = (double)pixel[3] / 255.0;
	sample[0] = alpha;
	sample[1] = (double)pixel[0] / 255.0 * alpha;
	sample[2] = (double)pixel[1] / 255.0 * alpha;
	sample[3] = (double)pixel[2] / 255.0 * alpha;
}

/*
 * Fills a pattern item's path, row by row: each covered pixel takes the
 * colour of the repeated image there (averaged over the pixel when the
 * image is shrunk), by coverage, alpha and clip.
 */
static int
fill_pattern(
	struct raster *raster,
	const struct pdf_display_item *item)
{
	const unsigned char *mask;
	unsigned color[3];
	unsigned level;
	size_t next_edge;
	size_t active_count;
	double determinant;
	double area;
	double shrink;
	double page_x;
	double page_y;
	double u;
	double v;
	double sample[4];
	double sum[4];
	double alpha;
	float coverage;
	int samples;
	int sub_x;
	int sub_y;
	int channel;
	int top;
	int bottom;
	int row;
	int x;
	int error;

	/* Nothing to draw at no alpha, or with a repeat squashed flat or without pixels. */
	if (!(item->alpha > 0.0))
		return 0;
	if (item->pixels == NULL ||
	    item->image_width == 0 ||
	    item->image_height == 0)
		return 0;
	determinant = item->matrix[0] * item->matrix[3] - item->matrix[1] * item->matrix[2];
	if (!(determinant > 1e-12 || determinant < -1e-12))
		return 0;

	/* How many of the image's pixels fall on one target pixel: more than one and a half are averaged. */
	area = fabs(determinant) * raster->scale * raster->scale;
	shrink = sqrt((double)item->image_width * (double)item->image_height / area);
	samples = 1;
	if (shrink > 1.5)
		samples = (int)ceil(shrink);
	if (samples > PDF_RASTER_IMAGE_SAMPLES)
		samples = PDF_RASTER_IMAGE_SAMPLES;

	/* Flattens the path and finds the rows it touches. */
	error = build_edges(raster, item);
	if (error != 0)
		return error;
	error = scan_path(raster, item->rule, &top, &bottom);
	if (error != 0)
		return error;

	/* Blends each row's covered pixels in the image's colour there. */
	mask = NULL;
	if (raster->mask_depth > 0)
		mask = raster->masks[raster->mask_depth - 1];
	next_edge = 0;
	active_count = 0;
	for (row = top; row < bottom; row++) {
		error = row_coverage(raster, item->rule, row, &next_edge, &active_count);
		if (error != 0)
			return error;
		for (x = raster->row_left; x <= raster->row_right; x++) {
			/* The pixel's coverage, within the clip. */
			coverage = raster->coverage[x];
			if (coverage <= 0.0f)
				continue;
			if (coverage > 1.0f)
				coverage = 1.0f;
			if (mask != NULL)
				coverage *= (float)mask[(size_t)row * (size_t)raster->width + (size_t)x] / 255.0f;

			/* Averages the repeated image over the pixel's samples. */
			sum[0] = 0.0;
			sum[1] = 0.0;
			sum[2] = 0.0;
			sum[3] = 0.0;
			for (sub_y = 0; sub_y < samples; sub_y++) {
				for (sub_x = 0; sub_x < samples; sub_x++) {
					page_x = ((double)x + ((double)sub_x + 0.5) / (double)samples - raster->offset_x) / raster->scale - item->matrix[4];
					page_y = ((double)row + ((double)sub_y + 0.5) / (double)samples - raster->offset_y) / raster->scale - item->matrix[5];
					u = (item->matrix[3] * page_x - item->matrix[2] * page_y) / determinant;
					v = (-item->matrix[1] * page_x + item->matrix[0] * page_y) / determinant;
					sample_repeated(item, u - floor(u), v - floor(v), sample);
					for (channel = 0; channel < 4; channel++)
						sum[channel] += sample[channel];
				}
			}

			/* A transparent part of the image paints nothing. */
			if (sum[0] <= 0.0)
				continue;

			/* The image's alpha, times the item's and the coverage. */
			alpha = sum[0] / (double)(samples * samples);
			alpha *= item->alpha;
			alpha *= (double)coverage;
			level = to_level(alpha);
			if (level == 0U)
				continue;

			/* Blends the colour, unpremultiplied from the sums. */
			color[0] = to_level(sum[1] / sum[0]);
			color[1] = to_level(sum[2] / sum[0]);
			color[2] = to_level(sum[3] / sum[0]);
			blend_pixel(raster->pixels + (size_t)row * raster->stride + (size_t)x, color, level, item->blend);
		}
	}

	/* Succeeded: the pattern's fill is drawn. */
	return 0;
}

/*
 * Samples a repeated image at (u, v) of one repeat (0 to 1 each),
 * bilinearly between the four nearest pixels, the pixels past an edge
 * being those of the next repeat, as premultiplied alpha, red, green, blue
 * from 0 to 1.
 */
static void
sample_repeated(
	const struct pdf_display_item *item,
	double u,
	double v,
	double sample[4])
{
	double x;
	double y;
	double fraction_x;
	double fraction_y;
	double corner[4][4];
	long left;
	long top;
	long right;
	long bottom;
	int channel;

	/* The position among the pixels' centres. */
	x = u * (double)item->image_width - 0.5;
	y = v * (double)item->image_height - 0.5;
	left = (long)floor(x);
	top = (long)floor(y);
	fraction_x = x - (double)left;
	fraction_y = y - (double)top;

	/* The four pixels around it, wrapped around the repeat's edges. */
	right = wrap_index(left + 1, item->image_width);
	bottom = wrap_index(top + 1, item->image_height);
	left = wrap_index(left, item->image_width);
	top = wrap_index(top, item->image_height);
	texel(item, left, top, corner[0]);
	texel(item, right, top, corner[1]);
	texel(item, left, bottom, corner[2]);
	texel(item, right, bottom, corner[3]);

	/* Mixes them by the distances. */
	for (channel = 0; channel < 4; channel++) {
		sample[channel] = corner[0][channel] * (1.0 - fraction_x) * (1.0 - fraction_y) +
		    corner[1][channel] * fraction_x * (1.0 - fraction_y) +
		    corner[2][channel] * (1.0 - fraction_x) * fraction_y +
		    corner[3][channel] * fraction_x * fraction_y;
	}
}

/* Wraps a pixel index into 0..size - 1, as the repeats of an image tile the plane. */
static long
wrap_index(
	long index,
	size_t size)
{
	long wrapped;

	/* The remainder, made positive. */
	wrapped = index % (long)size;
	if (wrapped < 0)
		wrapped += (long)size;

	/* Reports the index within the image. */
	return wrapped;
}
