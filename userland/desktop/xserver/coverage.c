/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Alpha masks of shapes: how much of each pixel a trapezoid covers.
 *
 * A trapezoid is sampled on a grid inside each pixel: an a8 mask takes
 * 15 rows of 17 samples a pixel (so that a whole pixel counts 255), an a1
 * mask the pixel's centre alone.  A sample is inside when it is at or
 * below the top, above the bottom, at or right of the left edge and left
 * of the right edge.  The count is added to what the mask holds
 * (kept at most 255, or 1 for a1), so that several trapezoids add up.
 */

#include "userland/desktop/xserver/render.h"

/* One in 16.16 fixed point. */
#define COVERAGE_ONE			65536

/*
 * The grid of an a8 mask: 15 rows and 17 columns of samples a pixel, a
 * row or column step apart, the first half a step in.
 *
 * Values referenced: pixman 0.44.0 pixman/pixman-private.h (N_Y_FRAC and
 * N_X_FRAC for 8 bits, STEP_Y_SMALL, STEP_X_SMALL, Y_FRAC_FIRST and
 * X_FRAC_FIRST).
 */
#define COVERAGE_ROWS_8			15
#define COVERAGE_COLUMNS_8		17
#define COVERAGE_ROW_STEP_8		(COVERAGE_ONE / COVERAGE_ROWS_8)
#define COVERAGE_COLUMN_STEP_8		(COVERAGE_ONE / COVERAGE_COLUMNS_8)

/*
 * A line of a trapezoid's side, its first point the upper one: where it
 * starts and how far it goes across and down (16.16).
 */
struct coverage_edge {
	int64_t x;
	int64_t y;
	int64_t dx;
	int64_t dy;
};

static int coverage_edge_of(const int32_t *line, struct coverage_edge *edge);
static int64_t coverage_edge_x(const struct coverage_edge *edge, int64_t y);
static int64_t coverage_floor(int64_t value, int64_t step);
static int coverage_before(int64_t place, int first, int step, int count);
static void coverage_row(struct x11_image *mask, int row, int64_t left, int64_t right, int first, int step, int count, unsigned most);

/*
 * Adds a trapezoid's coverage to a mask of depth 8 (antialiased) or 1
 * (sharp).  The trapezoid's point (0, 0) is at the mask's (x, y).
 */
void
x11_coverage_trapezoid(
	struct x11_image *mask,
	int x,
	int y,
	const struct x11_trapezoid *trapezoid)
{
	struct coverage_edge left;
	struct coverage_edge right;
	int64_t sample_y;
	int64_t origin_x;
	int64_t left_x;
	int64_t right_x;
	int64_t top;
	int64_t bottom;
	int valid;
	int rows;
	int first_row;
	int row_step;
	int columns;
	int first_column;
	int column_step;
	unsigned most;
	int row;
	int sub;
	int row_start;
	int row_end;

	/* A trapezoid with height and two sides that are not flat. */
	if (trapezoid->bottom <= trapezoid->top)
		return;
	valid = coverage_edge_of(trapezoid->left, &left);
	if (!valid)
		return;
	valid = coverage_edge_of(trapezoid->right, &right);
	if (!valid)
		return;

	/* The grid: a8's, or a1's one sample at the centre. */
	rows = COVERAGE_ROWS_8;
	first_row = COVERAGE_ROW_STEP_8 / 2;
	row_step = COVERAGE_ROW_STEP_8;
	columns = COVERAGE_COLUMNS_8;
	first_column = COVERAGE_COLUMN_STEP_8 / 2;
	column_step = COVERAGE_COLUMN_STEP_8;
	most = 255U;
	if (mask->depth == 1U) {
		rows = 1;
		first_row = COVERAGE_ONE / 2;
		row_step = COVERAGE_ONE;
		columns = 1;
		first_column = COVERAGE_ONE / 2;
		column_step = COVERAGE_ONE;
		most = 1U;
	}

	/* The mask's rows the trapezoid can reach (the trapezoid moved onto the mask). */
	top = (int64_t)trapezoid->top + (int64_t)y * COVERAGE_ONE;
	bottom = (int64_t)trapezoid->bottom + (int64_t)y * COVERAGE_ONE;
	row_start = (int)coverage_floor(top, COVERAGE_ONE);
	row_end = (int)coverage_floor(bottom, COVERAGE_ONE) + 1;
	if (row_start < 0)
		row_start = 0;
	if (row_end > mask->height)
		row_end = mask->height;
	origin_x = (int64_t)x * COVERAGE_ONE;

	/* Each sample row inside the trapezoid: the sides' places there, and the samples between them. */
	for (row = row_start; row < row_end; row++) {
		for (sub = 0; sub < rows; sub++) {
			sample_y = (int64_t)row * COVERAGE_ONE + first_row + (int64_t)sub * row_step;
			if (sample_y < top || sample_y >= bottom)
				continue;
			left_x = coverage_edge_x(&left, sample_y - (int64_t)y * COVERAGE_ONE) + origin_x;
			right_x = coverage_edge_x(&right, sample_y - (int64_t)y * COVERAGE_ONE) + origin_x;
			coverage_row(mask, row, left_x, right_x, first_column, column_step, columns, most);
		}
	}
}

/*
 * Makes an edge of a trapezoid's side from its line (x0, y0, x1, y1), the
 * upper point first.  Returns 1, or 0 for a flat line (no side).
 */
static int
coverage_edge_of(
	const int32_t *line,
	struct coverage_edge *edge)
{
	/* A flat line is no side. */
	if (line[1] == line[3])
		return 0;

	/* The upper point first. */
	if (line[1] < line[3]) {
		edge->x = line[0];
		edge->y = line[1];
		edge->dx = (int64_t)line[2] - line[0];
		edge->dy = (int64_t)line[3] - line[1];
	} else {
		edge->x = line[2];
		edge->y = line[3];
		edge->dx = (int64_t)line[0] - line[2];
		edge->dy = (int64_t)line[1] - line[3];
	}

	/* Succeeded: a side. */
	return 1;
}

/* Gives where an edge's line is across at a height (16.16), rounded down. */
static int64_t
coverage_edge_x(
	const struct coverage_edge *edge,
	int64_t y)
{
	/* Along the line from its upper point. */
	return edge->x + coverage_floor((y - edge->y) * edge->dx, edge->dy);
}

/* Divides, rounding down (step is above 0). */
static int64_t
coverage_floor(
	int64_t value,
	int64_t step)
{
	int64_t quotient;

	/* Towards zero, then down for a negative remainder. */
	quotient = value / step;
	if (value % step != 0 && value < 0)
		quotient--;

	/* The quotient. */
	return quotient;
}

/*
 * Gives how many of a pixel's sample columns are left of a place inside
 * it (16.16 from the pixel's left side, kept between 0 and one pixel).
 */
static int
coverage_before(
	int64_t place,
	int first,
	int step,
	int count)
{
	int64_t columns;

	/* None before the first sample. */
	if (place <= first)
		return 0;

	/* The samples up to the place, at most all. */
	columns = (place - first - 1) / step + 1;
	if (columns > count)
		columns = count;

	/* The count. */
	return (int)columns;
}

/*
 * Adds one sample row's coverage to a mask row: the samples at or right
 * of left and left of right (16.16 on the mask), each counting 1, kept at
 * most most.
 */
static void
coverage_row(
	struct x11_image *mask,
	int row,
	int64_t left,
	int64_t right,
	int first,
	int step,
	int count,
	unsigned most)
{
	uint32_t *pixel;
	int64_t pixel_left;
	int start;
	int end;
	int column;
	int covered;

	/* Nothing between crossed sides. */
	if (right <= left)
		return;

	/* The pixels the samples between the sides can be in, inside the mask. */
	start = (int)coverage_floor(left, COVERAGE_ONE);
	end = (int)coverage_floor(right, COVERAGE_ONE) + 1;
	if (start < 0)
		start = 0;
	if (end > mask->width)
		end = mask->width;

	/* Each pixel: the samples left of the right side less those left of the left side. */
	for (column = start; column < end; column++) {
		pixel_left = (int64_t)column * COVERAGE_ONE;
		covered = coverage_before(right - pixel_left, first, step, count) - coverage_before(left - pixel_left, first, step, count);
		if (covered <= 0)
			continue;
		pixel = &mask->pixels[(size_t)row * mask->stride + (size_t)column];
		*pixel += (uint32_t)covered;
		if (*pixel > most)
			*pixel = most;
	}
}
