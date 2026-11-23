/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Regions: sets of pixels kept as boxes that do not overlap.
 *
 * A window's clip (what of it can be drawn), the parts that become
 * visible and are exposed, and the parts of a copy's source that cannot
 * be read are regions.  The operations keep the boxes apart from one
 * another; they do not merge neighbouring boxes, so a region made by many
 * subtractions may hold more boxes than it needs.
 */

#include "userland/desktop/xserver/render.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The boxes a region first makes room for, and the most it may hold (more is a failed allocation). */
#define REGION_FIRST		8U
#define REGION_MAX		((size_t)1U << 20)

static int region_reserve(struct x11_region *region, size_t count);
static size_t region_cut(const struct x11_box *box, const struct x11_box *hole, struct x11_box *pieces);
static int region_box_before(const struct x11_box *a, const struct x11_box *b);
static int region_compare_int(const void *a, const void *b);
static int region_compare_x(const void *a, const void *b);

/*
 * Makes an empty region that holds no memory.
 */
void
x11_region_init(
	struct x11_region *region)
{
	/* No boxes, and no room for any. */
	region->boxes = NULL;
	region->count = 0U;
	region->capacity = 0U;
}

/*
 * Frees a region's boxes; it is empty afterwards.
 */
void
x11_region_fini(
	struct x11_region *region)
{
	/* The boxes go, and the region holds nothing. */
	free(region->boxes);
	x11_region_init(region);
}

/*
 * Empties a region, keeping its room for later boxes.
 */
void
x11_region_clear(
	struct x11_region *region)
{
	/* No box. */
	region->count = 0U;
}

/*
 * Makes a region one box (empty when the box is).  Returns 0, or ENOMEM.
 */
int
x11_region_set(
	struct x11_region *region,
	const struct x11_box *box)
{
	int error;

	/* An empty box is an empty region. */
	region->count = 0U;
	if (box->x0 >= box->x1 || box->y0 >= box->y1)
		return 0;

	/* Room for the box. */
	error = region_reserve(region, 1U);
	if (error != 0)
		return error;

	/* Succeeded: the one box. */
	region->boxes[0] = *box;
	region->count = 1U;
	return 0;
}

/*
 * Makes a region the same as another.  Returns 0, or ENOMEM (the target
 * is then empty).
 */
int
x11_region_copy(
	struct x11_region *target,
	const struct x11_region *source)
{
	int error;

	/* Room for every box of the source. */
	target->count = 0U;
	error = region_reserve(target, source->count);
	if (error != 0)
		return error;

	/* Succeeded: the source's boxes. */
	if (source->count != 0U)
		memcpy(target->boxes, source->boxes, source->count * sizeof(*source->boxes));
	target->count = source->count;
	return 0;
}

/*
 * Adds a box that the caller knows overlaps none of the region's boxes
 * (as the rows of a bitmap's runs do).  Returns 0, or ENOMEM.
 */
int
x11_region_append_box(
	struct x11_region *region,
	const struct x11_box *box)
{
	int error;

	/* An empty box adds nothing. */
	if (box->x0 >= box->x1 || box->y0 >= box->y1)
		return 0;

	/* Room for the box. */
	error = region_reserve(region, region->count + 1U);
	if (error != 0)
		return error;

	/* Succeeded: the box is in the region. */
	region->boxes[region->count] = *box;
	region->count++;
	return 0;
}

/*
 * Adds a box to a region.  Returns 0, or ENOMEM (the region may then
 * lack the part of the box it did not have).
 */
int
x11_region_union_box(
	struct x11_region *region,
	const struct x11_box *box)
{
	int error;

	/* An empty box adds nothing. */
	if (box->x0 >= box->x1 || box->y0 >= box->y1)
		return 0;

	/* What the region has of the box goes, so that the box can be added whole without overlapping. */
	error = x11_region_subtract_box(region, box);
	if (error != 0)
		return error;

	/* Room for the box. */
	error = region_reserve(region, region->count + 1U);
	if (error != 0)
		return error;

	/* Succeeded: the box is in the region. */
	region->boxes[region->count] = *box;
	region->count++;
	return 0;
}

/*
 * Adds a region to another.  Returns 0, or ENOMEM.
 */
int
x11_region_union(
	struct x11_region *region,
	const struct x11_region *other)
{
	size_t index;
	int error;

	/* Each box of the other region in turn (they do not overlap one another). */
	for (index = 0U; index < other->count; index++) {
		error = x11_region_union_box(region, &other->boxes[index]);
		if (error != 0)
			return error;
	}

	/* Succeeded: the region has both. */
	return 0;
}

/*
 * Takes a box out of a region.  Returns 0, or ENOMEM (the region is then
 * unchanged).
 */
int
x11_region_subtract_box(
	struct x11_region *region,
	const struct x11_box *box)
{
	struct x11_box pieces[4];
	struct x11_box *boxes;
	struct x11_box overlap;
	size_t index;
	size_t count;
	size_t made;
	size_t piece;
	size_t touched;
	int overlaps;

	/* The boxes that touch the hole each become up to four; count what the region will hold. */
	count = 0U;
	touched = 0U;
	for (index = 0U; index < region->count; index++) {
		overlaps = x11_box_intersect(&region->boxes[index], box, &overlap);
		if (!overlaps) {
			count++;
			continue;
		}
		touched++;
		count += region_cut(&region->boxes[index], box, pieces);
	}

	/* No box touches the hole. */
	if (touched == 0U)
		return 0;

	/* The new boxes are made apart from the old, so a failure leaves the region as it was. */
	if (count > REGION_MAX)
		return ENOMEM;
	boxes = NULL;
	if (count != 0U) {
		boxes = malloc(count * sizeof(*boxes));
		if (boxes == NULL)
			return ENOMEM;
	}

	/* Each box, whole when it does not touch the hole, else the pieces around the hole. */
	made = 0U;
	for (index = 0U; index < region->count; index++) {
		overlaps = x11_box_intersect(&region->boxes[index], box, &overlap);
		if (!overlaps) {
			boxes[made] = region->boxes[index];
			made++;
			continue;
		}
		count = region_cut(&region->boxes[index], box, pieces);
		for (piece = 0U; piece < count; piece++) {
			boxes[made] = pieces[piece];
			made++;
		}
	}

	/* Succeeded: the new boxes replace the old. */
	free(region->boxes);
	region->boxes = boxes;
	region->count = made;
	region->capacity = made;
	return 0;
}

/*
 * Takes another region out of a region.  Returns 0, or ENOMEM.
 */
int
x11_region_subtract(
	struct x11_region *region,
	const struct x11_region *other)
{
	size_t index;
	int error;

	/* Each box of the other region in turn. */
	for (index = 0U; index < other->count; index++) {
		error = x11_region_subtract_box(region, &other->boxes[index]);
		if (error != 0)
			return error;
	}

	/* Succeeded: what the other region covered is gone. */
	return 0;
}

/*
 * Keeps only the part of a region inside a box.
 */
void
x11_region_intersect_box(
	struct x11_region *region,
	const struct x11_box *box)
{
	struct x11_box overlap;
	size_t index;
	size_t kept;
	int overlaps;

	/* Each box shrinks to its part inside, or goes. */
	kept = 0U;
	for (index = 0U; index < region->count; index++) {
		overlaps = x11_box_intersect(&region->boxes[index], box, &overlap);
		if (!overlaps)
			continue;
		region->boxes[kept] = overlap;
		kept++;
	}

	/* The boxes kept. */
	region->count = kept;
}

/*
 * Keeps only the part of a region inside another.  Returns 0, or ENOMEM
 * (the region is then unchanged).
 */
int
x11_region_intersect(
	struct x11_region *region,
	const struct x11_region *other)
{
	struct x11_region result;
	struct x11_box overlap;
	size_t index;
	size_t other_index;
	int overlaps;
	int error;

	/* Every pair of boxes that meet gives their meeting (the pairs do not overlap one another). */
	x11_region_init(&result);
	for (index = 0U; index < region->count; index++) {
		for (other_index = 0U; other_index < other->count; other_index++) {
			overlaps = x11_box_intersect(&region->boxes[index], &other->boxes[other_index], &overlap);
			if (!overlaps)
				continue;
			error = region_reserve(&result, result.count + 1U);
			if (error != 0) {
				x11_region_fini(&result);
				return error;
			}
			result.boxes[result.count] = overlap;
			result.count++;
		}
	}

	/* Succeeded: the meetings replace the region. */
	x11_region_fini(region);
	*region = result;
	return 0;
}

/*
 * Moves a region by a distance.
 */
void
x11_region_translate(
	struct x11_region *region,
	int dx,
	int dy)
{
	size_t index;

	/* Each box. */
	for (index = 0U; index < region->count; index++) {
		region->boxes[index].x0 += dx;
		region->boxes[index].y0 += dy;
		region->boxes[index].x1 += dx;
		region->boxes[index].y1 += dy;
	}
}

/*
 * Puts a region's boxes in order, top to bottom and left to right (the
 * order Expose and GraphicsExpose events give them in).
 */
void
x11_region_sort(
	struct x11_region *region)
{
	struct x11_box moved;
	size_t index;
	size_t place;
	int before;

	/* Each box goes down past the boxes that come after it (an insertion sort: regions are small). */
	for (index = 1U; index < region->count; index++) {
		moved = region->boxes[index];
		place = index;
		while (place > 0U) {
			before = region_box_before(&moved, &region->boxes[place - 1U]);
			if (!before)
				break;
			region->boxes[place] = region->boxes[place - 1U];
			place--;
		}
		region->boxes[place] = moved;
	}
}

/*
 * Puts a region in the form X's regions take: bands from top to bottom,
 * each band's boxes from left to right with no two touching, and two
 * bands that touch merged when their boxes span the same columns.  Two
 * regions of the same pixels are then the same boxes in the same order
 * (what Expose and GraphicsExpose events give).  Returns 0, or ENOMEM
 * (the region is then only sorted).
 */
int
x11_region_canonical(
	struct x11_region *region)
{
	struct x11_box *result;
	struct x11_box *band;
	int *edges;
	size_t edge_count;
	size_t count;
	size_t index;
	size_t edge;
	size_t band_start;
	size_t previous_start;
	size_t previous_count;
	size_t made;
	int same;
	int top;
	int bottom;

	/* Nothing to do for fewer than two boxes. */
	if (region->count < 2U)
		return 0;

	/* Every box's top and bottom, in order, each once. */
	edges = malloc(region->count * 2U * sizeof(*edges));
	if (edges == NULL) {
		x11_region_sort(region);
		return ENOMEM;
	}
	for (index = 0U; index < region->count; index++) {
		edges[index * 2U] = region->boxes[index].y0;
		edges[index * 2U + 1U] = region->boxes[index].y1;
	}
	qsort(edges, region->count * 2U, sizeof(*edges), region_compare_int);
	edge_count = 0U;
	for (index = 0U; index < region->count * 2U; index++) {
		if (edge_count != 0U && edges[edge_count - 1U] == edges[index])
			continue;
		edges[edge_count] = edges[index];
		edge_count++;
	}

	/* Room for the result: a band holds at most every box, and there are fewer bands than edges (too many is only sorted). */
	if (edge_count > REGION_MAX / region->count) {
		free(edges);
		x11_region_sort(region);
		return ENOMEM;
	}
	result = malloc(region->count * edge_count * sizeof(*result));
	if (result == NULL) {
		free(edges);
		x11_region_sort(region);
		return ENOMEM;
	}

	/* Each band between two edges: the boxes across it, left to right, touching ones merged. */
	made = 0U;
	previous_start = 0U;
	previous_count = 0U;
	for (edge = 0U; edge + 1U < edge_count; edge++) {
		top = edges[edge];
		bottom = edges[edge + 1U];
		band_start = made;
		for (index = 0U; index < region->count; index++) {
			if (region->boxes[index].y0 > top || region->boxes[index].y1 < bottom)
				continue;
			result[made].x0 = region->boxes[index].x0;
			result[made].x1 = region->boxes[index].x1;
			result[made].y0 = top;
			result[made].y1 = bottom;
			made++;
		}
		band = result + band_start;
		count = made - band_start;
		qsort(band, count, sizeof(*band), region_compare_x);
		made = band_start;
		for (index = 0U; index < count; index++) {
			if (made > band_start && result[made - 1U].x1 >= band[index].x0) {
				if (band[index].x1 > result[made - 1U].x1)
					result[made - 1U].x1 = band[index].x1;
				continue;
			}
			result[made] = band[index];
			made++;
		}
		count = made - band_start;
		if (count == 0U)
			continue;

		/* A band that touches the one before and spans the same columns lengthens it. */
		same = 0;
		if (previous_count == count && result[previous_start].y1 == top) {
			same = 1;
			for (index = 0U; index < count; index++) {
				if (result[previous_start + index].x0 != result[band_start + index].x0 ||
				    result[previous_start + index].x1 != result[band_start + index].x1)
					same = 0;
			}
		}
		if (same) {
			for (index = 0U; index < count; index++)
				result[previous_start + index].y1 = bottom;
			made = band_start;
			continue;
		}
		previous_start = band_start;
		previous_count = count;
	}

	/* Succeeded: the region in its form. */
	free(edges);
	free(region->boxes);
	region->boxes = result;
	region->count = made;
	region->capacity = region->count * edge_count;
	return 0;
}

/*
 * Gives the smallest box around a region (an empty box for an empty
 * region).
 */
void
x11_region_extents(
	const struct x11_region *region,
	struct x11_box *extents)
{
	size_t index;

	/* Nothing for an empty region. */
	memset(extents, 0, sizeof(*extents));
	if (region->count == 0U)
		return;

	/* The first box, widened by each other. */
	*extents = region->boxes[0];
	for (index = 1U; index < region->count; index++) {
		if (region->boxes[index].x0 < extents->x0)
			extents->x0 = region->boxes[index].x0;
		if (region->boxes[index].y0 < extents->y0)
			extents->y0 = region->boxes[index].y0;
		if (region->boxes[index].x1 > extents->x1)
			extents->x1 = region->boxes[index].x1;
		if (region->boxes[index].y1 > extents->y1)
			extents->y1 = region->boxes[index].y1;
	}
}

/*
 * Gives where two boxes meet.  Returns nonzero when they meet (*result is
 * then a box that is not empty), 0 when they do not.
 */
int
x11_box_intersect(
	const struct x11_box *a,
	const struct x11_box *b,
	struct x11_box *result)
{
	/* The inner edges. */
	result->x0 = a->x0;
	if (b->x0 > result->x0)
		result->x0 = b->x0;
	result->y0 = a->y0;
	if (b->y0 > result->y0)
		result->y0 = b->y0;
	result->x1 = a->x1;
	if (b->x1 < result->x1)
		result->x1 = b->x1;
	result->y1 = a->y1;
	if (b->y1 < result->y1)
		result->y1 = b->y1;

	/* They do not meet when the edges cross. */
	if (result->x0 >= result->x1)
		return 0;
	if (result->y0 >= result->y1)
		return 0;

	/* Succeeded: they meet. */
	return 1;
}

/* Makes room for a count of boxes.  Returns 0, or ENOMEM. */
static int
region_reserve(
	struct x11_region *region,
	size_t count)
{
	struct x11_box *boxes;
	size_t capacity;

	/* Room enough already. */
	if (count <= region->capacity)
		return 0;

	/* Doubled until the boxes fit, within the most a region holds. */
	if (count > REGION_MAX)
		return ENOMEM;
	capacity = region->capacity;
	if (capacity < REGION_FIRST)
		capacity = REGION_FIRST;
	while (capacity < count)
		capacity *= 2U;

	/* The memory. */
	boxes = realloc(region->boxes, capacity * sizeof(*boxes));
	if (boxes == NULL)
		return ENOMEM;

	/* Succeeded: the room. */
	region->boxes = boxes;
	region->capacity = capacity;
	return 0;
}

/*
 * Cuts a hole out of a box that it overlaps: up to four pieces, the band
 * above the hole, the band below it, and the parts left and right of it
 * between them.  Returns how many pieces there are.
 */
static size_t
region_cut(
	const struct x11_box *box,
	const struct x11_box *hole,
	struct x11_box *pieces)
{
	size_t count;
	int top;
	int bottom;

	/* The band above the hole, the width of the box. */
	count = 0U;
	top = box->y0;
	if (hole->y0 > box->y0) {
		pieces[count].x0 = box->x0;
		pieces[count].y0 = box->y0;
		pieces[count].x1 = box->x1;
		pieces[count].y1 = hole->y0;
		count++;
		top = hole->y0;
	}

	/* The band below the hole, the width of the box. */
	bottom = box->y1;
	if (hole->y1 < box->y1) {
		pieces[count].x0 = box->x0;
		pieces[count].y0 = hole->y1;
		pieces[count].x1 = box->x1;
		pieces[count].y1 = box->y1;
		count++;
		bottom = hole->y1;
	}

	/* The part left of the hole, between the bands. */
	if (hole->x0 > box->x0) {
		pieces[count].x0 = box->x0;
		pieces[count].y0 = top;
		pieces[count].x1 = hole->x0;
		pieces[count].y1 = bottom;
		count++;
	}

	/* The part right of the hole, between the bands. */
	if (hole->x1 < box->x1) {
		pieces[count].x0 = hole->x1;
		pieces[count].y0 = top;
		pieces[count].x1 = box->x1;
		pieces[count].y1 = bottom;
		count++;
	}

	/* The pieces. */
	return count;
}

/* Tells whether a box comes before another: higher up, or as high and further left. */
static int
region_box_before(
	const struct x11_box *a,
	const struct x11_box *b)
{
	/* The higher box first. */
	if (a->y0 < b->y0)
		return 1;
	if (a->y0 > b->y0)
		return 0;

	/* As high: the box further left. */
	if (a->x0 < b->x0)
		return 1;

	return 0;
}

/* Orders two ints for qsort. */
static int
region_compare_int(
	const void *a,
	const void *b)
{
	int left;
	int right;

	/* The values. */
	left = *(const int *)a;
	right = *(const int *)b;
	if (left < right)
		return -1;
	if (left > right)
		return 1;

	return 0;
}

/* Orders two boxes of one band by their left edges for qsort. */
static int
region_compare_x(
	const void *a,
	const void *b)
{
	int left;
	int right;

	/* The left edges. */
	left = ((const struct x11_box *)a)->x0;
	right = ((const struct x11_box *)b)->x0;
	if (left < right)
		return -1;
	if (left > right)
		return 1;

	return 0;
}
