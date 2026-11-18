/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The coverage of a filled shape in sparse tiles of 8-bit alphas.
 *
 * The scan walks the sample lines of vec-raster.h from the top of the
 * area down.  The segments that cross a line are kept in an active list;
 * on each line their crossings are sorted by x and swept left to right
 * with the winding number, which gives the exact intervals where the fill
 * rule holds.  An interval is added to the pixel row's accumulators: the
 * length inside the pixels at its two ends, and a step in a difference
 * array for the whole pixels between them, so that a long interval costs
 * the same as a short one.  When the 16 sample lines of a pixel row are
 * done the accumulators become alphas in a band of one tile row; when the
 * 16 pixel rows of a tile row are done each tile of the band is sorted
 * into empty, full or partial.
 *
 * The moved vertices and the crossings are computed in double precision,
 * which keeps every sample line's choice of segments exact: the heights
 * of the lines are exact in a double, and so is the comparison with a
 * vertex moved by an offset.
 */

#include "vector/vec-raster.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The initial size of the tile and run arrays of a coverage, in elements, before they double. */
#define RASTER_CAPACITY_FIRST	16

/* The alpha of a pixel the shape covers wholly. */
#define RASTER_ALPHA_MAX	255

/*
 * The most element moves the insertion sort of the active crossings may
 * make on one sample line, as a multiple of their number, before the sort
 * turns to the heap sort.  The order of the crossings changes little from
 * one line to the next, so a few moves per crossing is the usual cost;
 * the heap sort bounds the cost of a hostile shape whose segments all
 * cross each other.
 */
#define RASTER_SORT_MOVES_PER_CROSSING	4

/*
 * The largest line number the line arithmetic works with, far beyond the
 * lines of any area (at most 2^29 for a coordinate of VEC_COORD_MAX), so
 * that a conversion of a double to an integer stays in range.
 */
#define RASTER_LINE_LIMIT	((int32_t)1 << 30)

/*
 * The two ways two coverages are combined pixel by pixel.
 */
enum raster_combine_mode {
	RASTER_COMBINE_INTERSECT = 0,
	RASTER_COMBINE_UNITE = 1
};

/*
 * One segment of the polylines that crosses at least one sample line of
 * the area, kept for the scan: the indices of its two vertices, and the
 * range of sample lines it crosses within the area (first_line included,
 * end_line not).  The vertices themselves stay in the caller's polylines.
 */
struct raster_segment {
	uint32_t from;
	uint32_t to;
	int32_t first_line;
	int32_t end_line;
};

/*
 * One crossing of the current sample line with a segment: where, which
 * way the segment goes (+1 down, -1 up) and which segment it is.  The
 * active list is an array of these, kept sorted by x on every line.
 */
struct raster_crossing {
	double x;
	int32_t direction;
	uint32_t segment;
};

/*
 * The state of one rasterization.  The arrays live from the planning to
 * the end of the call; the scan fills them in once and reads them after.
 */
struct raster_scan {
	const struct vec_lines *lines;
	enum vec_fill_rule rule;
	double offset_x;
	double offset_y;

	/* The pixels computed: the shape's bounds met with the clip.  Meaningful only when empty is 0. */
	struct vec_irect area;
	int empty;

	/* The sample lines of the area: the first and one past the last, in lines from the top of the pixel grid. */
	int32_t first_line;
	int32_t end_line;

	/* The segments that cross a line of the area, sorted by first_line, and how many the active list has taken from them. */
	struct raster_segment *segments;
	size_t segment_count;
	size_t next_segment;

	/* The crossings of the current sample line, sorted by x. */
	struct raster_crossing *active;
	size_t active_count;

	/* The accumulators of one pixel row, indexed from the area's left edge: partial lengths and steps of whole pixels. */
	double *partial;
	int32_t *step;
	int32_t width;
	int32_t dirty_left;
	int32_t dirty_right;

	/*
	 * The alphas of the tile row being made: VEC_TILE_SIZE rows of
	 * band_width bytes, whose column 0 is the pixel column band_left (a
	 * multiple of the tile size).  The columns touched since the band was
	 * last cleared are band_dirty_left .. band_dirty_right.
	 */
	unsigned char *band;
	int32_t band_width;
	int32_t band_left;
	int32_t band_dirty_left;
	int32_t band_dirty_right;
};

/*
 * One piece of a coverage as the combiner reads it: a run, or one partial
 * tile (tile not NULL, then it is one column wide), on tile row ty over
 * the columns start up to end (not included).
 */
struct raster_item {
	int32_t ty;
	int64_t start;
	int64_t end;
	const struct vec_tile *tile;
	int valid;
};

/*
 * A reader over the pieces of one coverage in the order of the tile grid
 * (row, then column), merging its tile array and its run array.
 */
struct raster_cursor {
	const struct vec_coverage *coverage;
	size_t tile_index;
	size_t run_index;
	struct raster_item item;
};

/* The forward declarations of the static functions. */
static int32_t raster_tile_of(int32_t pixel);
static int raster_reserve_tiles(struct vec_coverage *coverage, size_t needed);
static int raster_reserve_runs(struct vec_coverage *coverage, size_t needed);
static int raster_add_run(struct vec_coverage *coverage, int32_t ty, int64_t tx, int64_t count);
static int raster_add_tile(struct vec_coverage *coverage, int32_t ty, int32_t tx, const unsigned char *alpha);
static int raster_push_alphas(struct vec_coverage *coverage, int32_t ty, int32_t tx, const unsigned char *alpha);
static void raster_update_bounds(struct vec_coverage *coverage);
static int32_t raster_line_from(double y);
static int raster_segment_lines(const struct raster_scan *scan, uint32_t from, uint32_t to, struct raster_segment *segment);
static double raster_segment_x(const struct raster_scan *scan, const struct raster_segment *segment, int32_t line);
static int32_t raster_segment_direction(const struct raster_scan *scan, const struct raster_segment *segment);
static int raster_plan(struct raster_scan *scan, const struct vec_lines *lines, enum vec_fill_rule rule, float offset_x, float offset_y, const struct vec_irect *clip);
static int raster_collect(struct raster_scan *scan, uint32_t *bucket, int store);
static void raster_heap_sift(struct raster_crossing *crossings, size_t root, size_t count);
static void raster_sort_crossings(struct raster_crossing *crossings, size_t count);
static void raster_advance(struct raster_scan *scan, int32_t line);
static void raster_add_interval(struct raster_scan *scan, double left, double right);
static void raster_sweep(struct raster_scan *scan);
static void raster_finish_row(struct raster_scan *scan, int32_t band_row);
static int raster_flush_band(struct raster_scan *scan, struct vec_coverage *out, int32_t ty);
static int raster_scan_all(struct raster_scan *scan, struct vec_coverage *out);
static void raster_free(struct raster_scan *scan);
static int raster_prepare(struct raster_scan *scan, uint32_t *bucket);
static void raster_cursor_next(struct raster_cursor *cursor);
static int raster_item_before(const struct raster_item *first, const struct raster_item *second);
static int raster_keep_range(struct vec_coverage *out, const struct raster_item *item, int64_t from, int64_t to);
static int raster_combine_tiles(struct vec_coverage *out, const struct raster_item *a, const struct raster_item *b, int64_t from, int64_t to, enum raster_combine_mode mode);
static int raster_combine_walk(struct vec_coverage *out, const struct vec_coverage *a, const struct vec_coverage *b, enum raster_combine_mode mode);
static int raster_combine(struct vec_coverage *out, const struct vec_coverage *a, const struct vec_coverage *b, enum raster_combine_mode mode);

/*
 * Makes an empty coverage.
 */
void
vec_coverage_init(
	struct vec_coverage *coverage)
{
	/* Zeroes the structure, which is the empty coverage with no memory. */
	memset(coverage, 0, sizeof(*coverage));
}

/*
 * Frees the arrays and leaves the coverage empty.
 */
void
vec_coverage_release(
	struct vec_coverage *coverage)
{
	/* Gives back the tile array and the run array. */
	free(coverage->tiles);
	free(coverage->runs);

	/* Leaves the structure as vec_coverage_init does. */
	memset(coverage, 0, sizeof(*coverage));
}

/*
 * Empties the coverage but keeps its arrays.
 */
void
vec_coverage_reset(
	struct vec_coverage *coverage)
{
	/* Forgets the tiles and runs while the arrays stay allocated. */
	coverage->tile_count = 0;
	coverage->run_count = 0;

	/* The empty coverage has empty bounds. */
	memset(&coverage->bounds, 0, sizeof(coverage->bounds));
}

/*
 * Rasterizes polylines with a fill rule into out.
 *
 * The call is planned first (the input is checked, the area and the limits
 * are decided, nothing is scanned yet), then the segments that matter are
 * collected and sorted by the line they start on, then the area is
 * scanned a tile row at a time.  Every failure leaves out empty.
 */
int
vec_rasterize(
	const struct vec_lines *lines,
	enum vec_fill_rule rule,
	float offset_x,
	float offset_y,
	const struct vec_irect *clip,
	struct vec_coverage *out)
{
	struct raster_scan scan;
	uint32_t *bucket;
	size_t line_count;
	int error;

	/* Starts from an empty result, which is also what every failure leaves. */
	vec_coverage_reset(out);

	/* Checks the input and decides the area to compute. */
	error = raster_plan(&scan, lines, rule, offset_x, offset_y, clip);
	if (error != 0)
		return error;

	/* Succeeds with the empty coverage when there is nothing to compute. */
	if (scan.empty)
		return 0;

	/* Makes the table that sorts the segments by the line they start on. */
	line_count = (size_t)(scan.end_line - scan.first_line);
	bucket = calloc(line_count + 1, sizeof(*bucket));
	if (bucket == NULL)
		return ENOMEM;

	/* Collects the segments and makes the arrays the scan needs. */
	error = raster_prepare(&scan, bucket);
	free(bucket);
	if (error != 0) {
		raster_free(&scan);
		return error;
	}

	/* Scans the area into out. */
	error = raster_scan_all(&scan, out);
	raster_free(&scan);
	if (error != 0) {
		vec_coverage_reset(out);
		return error;
	}

	/* Measures the pixels of nonzero alpha the tiles hold. */
	raster_update_bounds(out);

	/* Succeeded: out holds the coverage. */
	return 0;
}

/*
 * Reads the alpha of one pixel of a coverage.
 *
 * The tile grid cell of the pixel is looked up by binary search, first
 * among the partial tiles (exact match on row and column), then among the
 * runs (the last run that starts at or before the cell, which holds it
 * when it reaches that far).
 */
unsigned
vec_coverage_at(
	const struct vec_coverage *coverage,
	int32_t x,
	int32_t y)
{
	const struct vec_tile *tile;
	const struct vec_run *run;
	size_t low;
	size_t high;
	size_t middle;
	int32_t tx;
	int32_t ty;
	int32_t column;
	int32_t row;

	/* Finds the tile grid cell of the pixel. */
	tx = raster_tile_of(x);
	ty = raster_tile_of(y);

	/* Finds the pixel's place in the cell, from the remainder (a negative one is moved up into the cell). */
	column = x % VEC_TILE_SIZE;
	if (column < 0)
		column += VEC_TILE_SIZE;
	row = y % VEC_TILE_SIZE;
	if (row < 0)
		row += VEC_TILE_SIZE;

	/* Searches the partial tiles, sorted by row then column, for the cell. */
	low = 0;
	high = coverage->tile_count;
	while (low < high) {
		middle = low + (high - low) / 2;
		tile = &coverage->tiles[middle];
		if (tile->ty < ty || (tile->ty == ty && tile->tx < tx)) {
			low = middle + 1;
		} else if (tile->ty == ty && tile->tx == tx) {
			/* The cell is a partial tile: reads the pixel from its alphas. */
			return tile->alpha[row * VEC_TILE_SIZE + column];
		} else {
			high = middle;
		}
	}

	/* Searches the runs for the first one that starts after the cell. */
	low = 0;
	high = coverage->run_count;
	while (low < high) {
		middle = low + (high - low) / 2;
		run = &coverage->runs[middle];
		if (run->ty < ty || (run->ty == ty && run->tx <= tx))
			low = middle + 1;
		else
			high = middle;
	}

	/* Reports a pixel in the run before it, when that run reaches the cell, as wholly covered. */
	if (low > 0) {
		run = &coverage->runs[low - 1];
		if (run->ty == ty && (int64_t)tx < (int64_t)run->tx + (int64_t)run->count)
			return RASTER_ALPHA_MAX;
	}

	/* Reports a pixel outside every tile and run as uncovered. */
	return 0;
}

/*
 * Multiplies two coverages pixel by pixel.
 */
int
vec_coverage_intersect(
	struct vec_coverage *out,
	const struct vec_coverage *a,
	const struct vec_coverage *b)
{
	int error;

	/* Refuses an output that is one of the inputs, which the combining would overwrite while reading. */
	if (out == a || out == b)
		return EINVAL;

	/* Combines the two, where only the pixels both cover can remain. */
	error = raster_combine(out, a, b, RASTER_COMBINE_INTERSECT);
	if (error != 0)
		return error;

	/* Succeeded: out holds the product. */
	return 0;
}

/*
 * Combines two coverages pixel by pixel by the larger alpha.
 */
int
vec_coverage_unite(
	struct vec_coverage *out,
	const struct vec_coverage *a,
	const struct vec_coverage *b)
{
	int error;

	/* Refuses an output that is one of the inputs, which the combining would overwrite while reading. */
	if (out == a || out == b)
		return EINVAL;

	/* Combines the two, where either one's pixels remain. */
	error = raster_combine(out, a, b, RASTER_COMBINE_UNITE);
	if (error != 0)
		return error;

	/* Succeeded: out holds the larger alphas. */
	return 0;
}

/*
 * Gives the tile grid index of a pixel coordinate: the pixel divided by
 * the tile size, rounded toward minus infinity, so that the pixels of
 * negative coordinates fall in the tiles left of and above the origin.
 */
static int32_t
raster_tile_of(
	int32_t pixel)
{
	int32_t tile;

	/* Divides, and corrects the rounding toward zero for a negative pixel. */
	tile = pixel / VEC_TILE_SIZE;
	if (pixel % VEC_TILE_SIZE < 0)
		tile--;

	/* Reports the tile index. */
	return tile;
}

/*
 * Makes the tile array hold at least the given number of tiles.
 */
static int
raster_reserve_tiles(
	struct vec_coverage *coverage,
	size_t needed)
{
	struct vec_tile *grown;
	size_t capacity;

	/* Keeps the array when it is already large enough. */
	if (needed <= coverage->tile_capacity)
		return 0;

	/* Doubles the capacity until it holds the needed count. */
	capacity = coverage->tile_capacity;
	if (capacity < RASTER_CAPACITY_FIRST)
		capacity = RASTER_CAPACITY_FIRST;
	while (capacity < needed) {
		if (capacity > SIZE_MAX / 2 / sizeof(*grown))
			return ENOMEM;
		capacity *= 2;
	}

	/* Grows the array; the old one stays valid when this fails. */
	grown = realloc(coverage->tiles, capacity * sizeof(*grown));
	if (grown == NULL)
		return ENOMEM;

	/* Publishes the larger array. */
	coverage->tiles = grown;
	coverage->tile_capacity = capacity;

	/* Succeeded: the array holds the needed tiles. */
	return 0;
}

/*
 * Makes the run array hold at least the given number of runs.
 */
static int
raster_reserve_runs(
	struct vec_coverage *coverage,
	size_t needed)
{
	struct vec_run *grown;
	size_t capacity;

	/* Keeps the array when it is already large enough. */
	if (needed <= coverage->run_capacity)
		return 0;

	/* Doubles the capacity until it holds the needed count. */
	capacity = coverage->run_capacity;
	if (capacity < RASTER_CAPACITY_FIRST)
		capacity = RASTER_CAPACITY_FIRST;
	while (capacity < needed) {
		if (capacity > SIZE_MAX / 2 / sizeof(*grown))
			return ENOMEM;
		capacity *= 2;
	}

	/* Grows the array; the old one stays valid when this fails. */
	grown = realloc(coverage->runs, capacity * sizeof(*grown));
	if (grown == NULL)
		return ENOMEM;

	/* Publishes the larger array. */
	coverage->runs = grown;
	coverage->run_capacity = capacity;

	/* Succeeded: the array holds the needed runs. */
	return 0;
}

/*
 * Appends full tiles to the runs.
 *
 * Tiles are added in grid order, so a run that ends where the new one
 * starts on the same row is extended instead: abutting full tiles are one
 * run.
 */
static int
raster_add_run(
	struct vec_coverage *coverage,
	int32_t ty,
	int64_t tx,
	int64_t count)
{
	struct vec_run *last;
	int error;

	/* Extends the last run when the new tiles continue it on the same row. */
	if (coverage->run_count != 0) {
		last = &coverage->runs[coverage->run_count - 1];
		if (last->ty == ty && (int64_t)last->tx + (int64_t)last->count == tx) {
			last->count = (int32_t)((int64_t)last->count + count);
			return 0;
		}
	}

	/* Makes room for one more run. */
	error = raster_reserve_runs(coverage, coverage->run_count + 1);
	if (error != 0)
		return error;

	/* Stores the run. */
	coverage->runs[coverage->run_count].tx = (int32_t)tx;
	coverage->runs[coverage->run_count].ty = ty;
	coverage->runs[coverage->run_count].count = (int32_t)count;
	coverage->run_count++;

	/* Succeeded: the tiles are in a run. */
	return 0;
}

/*
 * Appends one partial tile with a copy of the given alphas.
 *
 * The caller has already decided the alphas are neither all 0 nor all 255.
 */
static int
raster_add_tile(
	struct vec_coverage *coverage,
	int32_t ty,
	int32_t tx,
	const unsigned char *alpha)
{
	struct vec_tile *tile;
	int error;

	/* Refuses more partial tiles than one coverage holds. */
	if (coverage->tile_count >= VEC_RASTER_TILES_MAX)
		return ENOBUFS;

	/* Makes room for one more tile. */
	error = raster_reserve_tiles(coverage, coverage->tile_count + 1);
	if (error != 0)
		return error;

	/* Stores the tile's place and a copy of its alphas. */
	tile = &coverage->tiles[coverage->tile_count];
	tile->tx = tx;
	tile->ty = ty;
	memcpy(tile->alpha, alpha, VEC_TILE_PIXELS);
	coverage->tile_count++;

	/* Succeeded: the tile is kept. */
	return 0;
}

/*
 * Adds the tile with the given alphas in the form it deserves.
 *
 * A tile of all 0 is dropped, one of all 255 joins the runs, any other is
 * kept as a partial tile.  Combining coverages makes new tiles this way,
 * so a product that comes out empty or full is stored as such.
 */
static int
raster_push_alphas(
	struct vec_coverage *coverage,
	int32_t ty,
	int32_t tx,
	const unsigned char *alpha)
{
	size_t zero_count;
	size_t full_count;
	size_t index;
	int error;

	/* Counts the pixels of alpha 0 and of alpha 255. */
	zero_count = 0;
	full_count = 0;
	for (index = 0; index < VEC_TILE_PIXELS; index++) {
		if (alpha[index] == 0)
			zero_count++;
		else if (alpha[index] == RASTER_ALPHA_MAX)
			full_count++;
	}

	/* Drops an empty tile. */
	if (zero_count == VEC_TILE_PIXELS)
		return 0;

	/* Keeps a full tile as a run of one. */
	if (full_count == VEC_TILE_PIXELS) {
		error = raster_add_run(coverage, ty, tx, 1);
		if (error != 0)
			return error;

		/* Succeeded: the full tile is in the runs. */
		return 0;
	}

	/* Keeps any other tile with its alphas. */
	error = raster_add_tile(coverage, ty, tx, alpha);
	if (error != 0)
		return error;

	/* Succeeded: the partial tile is kept. */
	return 0;
}

/*
 * Measures the bounds of a coverage: the smallest rectangle of whole
 * pixels that holds every pixel of nonzero alpha.
 *
 * A run holds its tiles whole; a partial tile is read pixel by pixel, as
 * its alphas may be 0 along its edges (a clip cuts them so).
 */
static void
raster_update_bounds(
	struct vec_coverage *coverage)
{
	const struct vec_tile *tile;
	const struct vec_run *run;
	int64_t left;
	int64_t top;
	int64_t right;
	int64_t bottom;
	int64_t x;
	int64_t y;
	int64_t run_right;
	size_t index;
	int row;
	int column;
	int found;

	/* Starts with no pixel found. */
	found = 0;
	left = 0;
	top = 0;
	right = 0;
	bottom = 0;

	/* Widens the bounds by each pixel of nonzero alpha of the partial tiles. */
	for (index = 0; index < coverage->tile_count; index++) {
		tile = &coverage->tiles[index];
		for (row = 0; row < VEC_TILE_SIZE; row++) {
			for (column = 0; column < VEC_TILE_SIZE; column++) {
				if (tile->alpha[row * VEC_TILE_SIZE + column] == 0)
					continue;
				x = (int64_t)tile->tx * VEC_TILE_SIZE + column;
				y = (int64_t)tile->ty * VEC_TILE_SIZE + row;
				if (!found) {
					left = x;
					right = x + 1;
					top = y;
					bottom = y + 1;
					found = 1;
				}
				if (x < left)
					left = x;
				if (x + 1 > right)
					right = x + 1;
				if (y < top)
					top = y;
				if (y + 1 > bottom)
					bottom = y + 1;
			}
		}
	}

	/* Widens the bounds by the whole tiles of each run. */
	for (index = 0; index < coverage->run_count; index++) {
		run = &coverage->runs[index];
		if (run->count <= 0)
			continue;
		x = (int64_t)run->tx * VEC_TILE_SIZE;
		y = (int64_t)run->ty * VEC_TILE_SIZE;
		run_right = x + (int64_t)run->count * VEC_TILE_SIZE;
		if (!found) {
			left = x;
			right = run_right;
			top = y;
			bottom = y + VEC_TILE_SIZE;
			found = 1;
		}
		if (x < left)
			left = x;
		if (run_right > right)
			right = run_right;
		if (y < top)
			top = y;
		if (y + VEC_TILE_SIZE > bottom)
			bottom = y + VEC_TILE_SIZE;
	}

	/* Publishes the bounds (all 0 when no pixel has alpha). */
	coverage->bounds.left = (int32_t)left;
	coverage->bounds.top = (int32_t)top;
	coverage->bounds.right = (int32_t)right;
	coverage->bounds.bottom = (int32_t)bottom;
}

/*
 * Gives the first sample line at or below a y, that is the smallest
 * line number j whose height (j + 0.5) / VEC_SUBLINES is at least y.
 *
 * A segment holds the lines from the first one at or below its upper end
 * up to, not including, the first one at or below its lower end.  The
 * estimate from the ceiling is corrected by comparing the exact heights,
 * which are exact in a double, so the rounding of the multiplication
 * cannot move a line.
 */
static int32_t
raster_line_from(
	double y)
{
	double estimate;
	double height;
	int32_t line;

	/* Estimates the line, held inside the range the integer can take. */
	estimate = ceil(y * (double)VEC_SUBLINES - 0.5);
	if (estimate > (double)RASTER_LINE_LIMIT)
		estimate = (double)RASTER_LINE_LIMIT;
	if (estimate < -(double)RASTER_LINE_LIMIT)
		estimate = -(double)RASTER_LINE_LIMIT;
	line = (int32_t)estimate;

	/* Moves down while the line's height is above y. */
	height = ((double)line + 0.5) / (double)VEC_SUBLINES;
	while (height < y && line < RASTER_LINE_LIMIT) {
		line++;
		height = ((double)line + 0.5) / (double)VEC_SUBLINES;
	}

	/* Moves up while the line above is still at or below y. */
	height = ((double)line - 0.5) / (double)VEC_SUBLINES;
	while (height >= y && line > -RASTER_LINE_LIMIT) {
		line--;
		height = ((double)line - 0.5) / (double)VEC_SUBLINES;
	}

	/* Reports the line. */
	return line;
}

/*
 * Decides whether the segment between two vertices crosses a sample line
 * of the area and, if it does, describes it.
 *
 * Returns 1 and fills the segment, or 0 for a segment that crosses no line
 * (horizontal, too short to reach a line, or outside the area's rows).
 */
static int
raster_segment_lines(
	const struct raster_scan *scan,
	uint32_t from,
	uint32_t to,
	struct raster_segment *segment)
{
	double from_y;
	double to_y;
	double top;
	double bottom;
	int32_t first_line;
	int32_t end_line;

	/* Moves the two ends and orders them top to bottom. */
	from_y = (double)scan->lines->vertices[from].y + scan->offset_y;
	to_y = (double)scan->lines->vertices[to].y + scan->offset_y;
	top = from_y;
	bottom = to_y;
	if (to_y < from_y) {
		top = to_y;
		bottom = from_y;
	}

	/* Finds the lines the segment holds: the upper end included, the lower end not. */
	first_line = raster_line_from(top);
	end_line = raster_line_from(bottom);

	/* Keeps only the lines inside the area. */
	if (first_line < scan->first_line)
		first_line = scan->first_line;
	if (end_line > scan->end_line)
		end_line = scan->end_line;

	/* Reports a segment with no line left. */
	if (first_line >= end_line)
		return 0;

	/* Describes the segment. */
	segment->from = from;
	segment->to = to;
	segment->first_line = first_line;
	segment->end_line = end_line;

	/* Reports a segment the scan has to follow. */
	return 1;
}

/*
 * Gives the x where a segment crosses a sample line.
 *
 * The line is the one through the segment's moved end points; the segment
 * has to cross the line (not be horizontal).
 */
static double
raster_segment_x(
	const struct raster_scan *scan,
	const struct raster_segment *segment,
	int32_t line)
{
	const struct vec_vertex *from;
	const struct vec_vertex *to;
	double from_x;
	double from_y;
	double to_x;
	double to_y;
	double y;

	/* Moves the two ends. */
	from = &scan->lines->vertices[segment->from];
	to = &scan->lines->vertices[segment->to];
	from_x = (double)from->x + scan->offset_x;
	from_y = (double)from->y + scan->offset_y;
	to_x = (double)to->x + scan->offset_x;
	to_y = (double)to->y + scan->offset_y;

	/* Finds the height of the line. */
	y = ((double)line + 0.5) / (double)VEC_SUBLINES;

	/* Reports where the segment is at that height. */
	return from_x + (to_x - from_x) * (y - from_y) / (to_y - from_y);
}

/*
 * Gives the direction a segment crosses a line: +1 going down, -1 going up.
 */
static int32_t
raster_segment_direction(
	const struct raster_scan *scan,
	const struct raster_segment *segment)
{
	double from_y;
	double to_y;

	/* Moves the two ends. */
	from_y = (double)scan->lines->vertices[segment->from].y + scan->offset_y;
	to_y = (double)scan->lines->vertices[segment->to].y + scan->offset_y;

	/* Reports a segment going down. */
	if (to_y > from_y)
		return 1;

	/* Reports a segment going up. */
	return -1;
}

/*
 * Checks the input of vec_rasterize and decides the area.
 *
 * The area is the shape's bounds (the moved vertices' extremes rounded
 * outward to whole pixels) met with the clip.  An area with no pixel gives
 * a scan with nothing to compute (scan->empty).  The limits that depend
 * only on the input are checked here, before any memory is allocated.
 */
static int
raster_plan(
	struct raster_scan *scan,
	const struct vec_lines *lines,
	enum vec_fill_rule rule,
	float offset_x,
	float offset_y,
	const struct vec_irect *clip)
{
	const struct vec_contour *contour;
	const struct vec_vertex *vertex;
	double min_x;
	double min_y;
	double max_x;
	double max_y;
	double x;
	double y;
	int64_t left;
	int64_t top;
	int64_t right;
	int64_t bottom;
	int64_t width;
	int64_t height;
	int64_t tile_left;
	int64_t tile_right;
	size_t segments;
	size_t contour_index;
	size_t index;
	int finite_x;
	int finite_y;
	int found;
	int error;

	/* Starts from an empty plan. */
	memset(scan, 0, sizeof(*scan));
	scan->empty = 1;

	/* Refuses missing polylines and a fill rule that is not one of the two. */
	if (lines == NULL)
		return EINVAL;
	if (rule != VEC_FILL_NONZERO && rule != VEC_FILL_EVENODD)
		return EINVAL;

	/* Refuses offsets that are not finite numbers. */
	finite_x = vec_finite(offset_x);
	finite_y = vec_finite(offset_y);
	if (!finite_x || !finite_y)
		return EINVAL;

	/* Refuses malformed polylines, so that no contour reads beyond the vertex array. */
	error = vec_lines_check(lines);
	if (error != 0)
		return error;

	/* Refuses more segments than one call takes (each contour has as many segments as vertices). */
	segments = 0;
	for (contour_index = 0; contour_index < lines->contour_count; contour_index++) {
		contour = &lines->contours[contour_index];
		segments += contour->count;
		if (segments > VEC_RASTER_SEGMENTS_MAX)
			return ENOBUFS;
	}

	/* Finds the extremes of the moved vertices, and refuses one that lands beyond the coordinate limit. */
	found = 0;
	min_x = 0.0;
	min_y = 0.0;
	max_x = 0.0;
	max_y = 0.0;
	for (contour_index = 0; contour_index < lines->contour_count; contour_index++) {
		contour = &lines->contours[contour_index];
		for (index = 0; index < contour->count; index++) {
			vertex = &lines->vertices[contour->first + index];
			x = (double)vertex->x + (double)offset_x;
			y = (double)vertex->y + (double)offset_y;
			if (x > (double)VEC_COORD_MAX || x < -(double)VEC_COORD_MAX)
				return EINVAL;
			if (y > (double)VEC_COORD_MAX || y < -(double)VEC_COORD_MAX)
				return EINVAL;
			if (!found) {
				min_x = x;
				max_x = x;
				min_y = y;
				max_y = y;
				found = 1;
			}
			if (x < min_x)
				min_x = x;
			if (x > max_x)
				max_x = x;
			if (y < min_y)
				min_y = y;
			if (y > max_y)
				max_y = y;
		}
	}

	/* Succeeds with nothing to compute for polylines with no vertex. */
	if (!found)
		return 0;

	/* Rounds the shape's bounds outward to whole pixels. */
	left = (int64_t)floor(min_x);
	top = (int64_t)floor(min_y);
	right = (int64_t)ceil(max_x);
	bottom = (int64_t)ceil(max_y);

	/* Meets the bounds with the clip. */
	if (clip != NULL) {
		if (left < clip->left)
			left = clip->left;
		if (top < clip->top)
			top = clip->top;
		if (right > clip->right)
			right = clip->right;
		if (bottom > clip->bottom)
			bottom = clip->bottom;
	}

	/* Succeeds with nothing to compute when the area has no pixel. */
	if (right <= left || bottom <= top)
		return 0;

	/* Refuses an area larger than one call rasterizes. */
	width = right - left;
	height = bottom - top;
	if (width > VEC_RASTER_SIDE_MAX || height > VEC_RASTER_SIDE_MAX)
		return ENOBUFS;

	/* Records the plan: the polylines, the moving, the area and its sample lines. */
	scan->lines = lines;
	scan->rule = rule;
	scan->offset_x = (double)offset_x;
	scan->offset_y = (double)offset_y;
	scan->area.left = (int32_t)left;
	scan->area.top = (int32_t)top;
	scan->area.right = (int32_t)right;
	scan->area.bottom = (int32_t)bottom;
	scan->first_line = (int32_t)(top * VEC_SUBLINES);
	scan->end_line = (int32_t)(bottom * VEC_SUBLINES);
	scan->width = (int32_t)width;

	/* Records the band: whole tile columns from the one holding the area's left edge to the one holding its right edge. */
	tile_left = raster_tile_of((int32_t)left);
	tile_right = raster_tile_of((int32_t)(right - 1));
	scan->band_left = (int32_t)(tile_left * VEC_TILE_SIZE);
	scan->band_width = (int32_t)((tile_right - tile_left + 1) * VEC_TILE_SIZE);
	scan->empty = 0;

	/* Succeeded: the area is planned. */
	return 0;
}

/*
 * Walks every segment of every contour (with the closing segment of each)
 * and keeps the ones that cross a sample line of the area.
 *
 * With store 0 it only counts: in bucket[line - first line] the segments
 * that start on each line, and it refuses (ENOBUFS) when the crossings
 * they will make, summed, exceed VEC_RASTER_CROSSINGS_MAX.  With store 1,
 * bucket holds the first free slot of each line (from the first walk) and
 * the segments are stored in the order of the line they start on.
 */
static int
raster_collect(
	struct raster_scan *scan,
	uint32_t *bucket,
	int store)
{
	const struct vec_contour *contour;
	struct raster_segment candidate;
	uint64_t crossings;
	size_t contour_index;
	size_t index;
	size_t next;
	int crosses;

	/* Walks every segment of every contour. */
	crossings = 0;
	for (contour_index = 0; contour_index < scan->lines->contour_count; contour_index++) {
		contour = &scan->lines->contours[contour_index];
		for (index = 0; index < contour->count; index++) {
			/* The closing segment runs from the last vertex back to the first. */
			next = index + 1;
			if (next == contour->count)
				next = 0;

			/* Skips a segment that crosses no line of the area. */
			crosses = raster_segment_lines(scan, (uint32_t)(contour->first + index), (uint32_t)(contour->first + next), &candidate);
			if (!crosses)
				continue;

			/* Counts the segment on its first line, and the crossings it will make. */
			if (!store) {
				bucket[candidate.first_line - scan->first_line]++;
				crossings += (uint64_t)(candidate.end_line - candidate.first_line);
				if (crossings > VEC_RASTER_CROSSINGS_MAX)
					return ENOBUFS;
				continue;
			}

			/* Stores the segment in the next free slot of its first line. */
			scan->segments[bucket[candidate.first_line - scan->first_line]] = candidate;
			bucket[candidate.first_line - scan->first_line]++;
		}
	}

	/* Succeeded: the segments are counted or stored. */
	return 0;
}

/*
 * Makes the arrays of a scan: collects the segments sorted by the line
 * they start on, then the active list and the accumulators.
 *
 * bucket is zeroed memory of one more than the area's number of lines.
 */
static int
raster_prepare(
	struct raster_scan *scan,
	uint32_t *bucket)
{
	uint64_t total;
	uint32_t count_here;
	size_t line_count;
	size_t index;
	size_t band_bytes;
	int error;

	/* Counts the segments that cross the area and the crossings the scan would compute. */
	error = raster_collect(scan, bucket, 0);
	if (error != 0)
		return error;

	/* Turns the counts into the first slot of each line's segments. */
	line_count = (size_t)(scan->end_line - scan->first_line);
	total = 0;
	for (index = 0; index <= line_count; index++) {
		count_here = bucket[index];
		bucket[index] = (uint32_t)total;
		total += count_here;
	}
	scan->segment_count = (size_t)total;

	/* Makes the segment array and the active list. */
	if (scan->segment_count != 0) {
		scan->segments = malloc(scan->segment_count * sizeof(*scan->segments));
		if (scan->segments == NULL)
			return ENOMEM;
		scan->active = malloc(scan->segment_count * sizeof(*scan->active));
		if (scan->active == NULL)
			return ENOMEM;
	}

	/* Stores the segments in the order of the line they start on. */
	error = raster_collect(scan, bucket, 1);
	if (error != 0)
		return error;

	/* Makes the accumulators of a pixel row (one entry past the last pixel, and one more for the last step). */
	scan->partial = calloc((size_t)scan->width + 2, sizeof(*scan->partial));
	if (scan->partial == NULL)
		return ENOMEM;
	scan->step = calloc((size_t)scan->width + 2, sizeof(*scan->step));
	if (scan->step == NULL)
		return ENOMEM;

	/* Makes the band of a tile row. */
	band_bytes = (size_t)scan->band_width * VEC_TILE_SIZE;
	scan->band = calloc(band_bytes, 1);
	if (scan->band == NULL)
		return ENOMEM;

	/* Succeeded: the scan has what it needs. */
	return 0;
}

/*
 * Restores the heap order below a root of a binary max-heap of crossings
 * by x, among the first count of them.
 */
static void
raster_heap_sift(
	struct raster_crossing *crossings,
	size_t root,
	size_t count)
{
	struct raster_crossing held;
	size_t child;

	/* Moves the root's crossing down until both children are no larger. */
	held = crossings[root];
	while (root * 2 + 1 < count) {
		child = root * 2 + 1;
		if (child + 1 < count && crossings[child + 1].x > crossings[child].x)
			child++;
		if (crossings[child].x <= held.x)
			break;
		crossings[root] = crossings[child];
		root = child;
	}
	crossings[root] = held;
}

/*
 * Sorts crossings by x.
 *
 * The order differs little from the previous line's, so an insertion sort
 * takes a few moves per crossing; when it takes more than that budget the
 * rest is sorted by heap sort, which is O(n log n) whatever the order.
 */
static void
raster_sort_crossings(
	struct raster_crossing *crossings,
	size_t count)
{
	struct raster_crossing held;
	size_t budget;
	size_t moves;
	size_t index;
	size_t place;

	/* Inserts each crossing into the sorted ones before it, within the budget. */
	budget = count * RASTER_SORT_MOVES_PER_CROSSING + RASTER_CAPACITY_FIRST;
	moves = 0;
	for (index = 1; index < count; index++) {
		held = crossings[index];
		place = index;
		while (place > 0 && crossings[place - 1].x > held.x) {
			crossings[place] = crossings[place - 1];
			place--;
			moves++;
		}
		crossings[place] = held;
		if (moves > budget)
			break;
	}

	/* Finishes a sort that ran over its budget by heap sort. */
	if (index < count) {
		/* Builds the heap. */
		for (index = count / 2; index > 0; index--)
			raster_heap_sift(crossings, index - 1, count);

		/* Takes the largest to the end, one at a time. */
		for (index = count; index > 1; index--) {
			held = crossings[0];
			crossings[0] = crossings[index - 1];
			crossings[index - 1] = held;
			raster_heap_sift(crossings, 0, index - 1);
		}
	}
}

/*
 * Brings the active list to a sample line: drops the segments that end
 * above it, adds the ones that start on it, and puts the crossings in
 * order by x.
 */
static void
raster_advance(
	struct raster_scan *scan,
	int32_t line)
{
	const struct raster_segment *segment;
	struct raster_crossing *entry;
	size_t read;
	size_t write;

	/* Moves the segments still crossing to the front of the list, with their new x. */
	write = 0;
	for (read = 0; read < scan->active_count; read++) {
		segment = &scan->segments[scan->active[read].segment];
		if (segment->end_line <= line)
			continue;
		scan->active[write] = scan->active[read];
		scan->active[write].x = raster_segment_x(scan, segment, line);
		write++;
	}
	scan->active_count = write;

	/* Adds the segments that start on this line (or before it, for the first line of the area). */
	while (scan->next_segment < scan->segment_count &&
	    scan->segments[scan->next_segment].first_line <= line) {
		segment = &scan->segments[scan->next_segment];
		entry = &scan->active[scan->active_count];
		entry->x = raster_segment_x(scan, segment, line);
		entry->direction = raster_segment_direction(scan, segment);
		entry->segment = (uint32_t)scan->next_segment;
		scan->active_count++;
		scan->next_segment++;
	}

	/* Puts the crossings in order from left to right. */
	raster_sort_crossings(scan->active, scan->active_count);
}

/*
 * Adds the interval [left, right) of x, on one sample line, to the pixel
 * row's accumulators.
 *
 * The interval is cut to the area first.  The pixel it starts in gets the
 * length from its start to the pixel's right edge, the pixel it ends in
 * the length from its left edge to its end (one interval inside a pixel
 * gives that pixel its length), and every pixel between gets 1 by a +1
 * step where they begin and a -1 step where they end, summed later.
 */
static void
raster_add_interval(
	struct raster_scan *scan,
	double left,
	double right)
{
	double edge_left;
	double edge_right;
	int32_t first;
	int32_t last;

	/* Cuts the interval to the area; nothing is left of one outside it. */
	edge_left = (double)scan->area.left;
	edge_right = (double)scan->area.right;
	if (left < edge_left)
		left = edge_left;
	if (right > edge_right)
		right = edge_right;
	if (right <= left)
		return;

	/* Measures from the area's left edge and finds the pixels the ends fall in. */
	left -= edge_left;
	right -= edge_left;
	first = (int32_t)floor(left);
	last = (int32_t)floor(right);

	/* Adds the interval to the pixels it touches. */
	if (first == last) {
		scan->partial[first] += right - left;
	} else {
		scan->partial[first] += (double)(first + 1) - left;
		scan->partial[last] += right - (double)last;
		scan->step[first + 1] += 1;
		scan->step[last] -= 1;
	}

	/* Widens the range of pixels this row has touched. */
	if (first < scan->dirty_left)
		scan->dirty_left = first;
	if (last > scan->dirty_right)
		scan->dirty_right = last;
}

/*
 * Sweeps the sorted crossings of the current line left to right with the
 * winding number, and adds each interval where the fill rule holds.
 *
 * Intervals are made only where the rule's answer changes, so two
 * crossings that leave the rule holding on both sides (nonzero going from
 * winding 1 to 2) do not split the interval.
 */
static void
raster_sweep(
	struct raster_scan *scan)
{
	const struct raster_crossing *crossing;
	double start;
	int32_t winding;
	size_t index;
	int inside;
	int inside_after;

	/* Follows the winding number across the crossings. */
	winding = 0;
	inside = 0;
	start = 0.0;
	for (index = 0; index < scan->active_count; index++) {
		crossing = &scan->active[index];
		winding += crossing->direction;

		/* Tells whether the fill rule holds right of this crossing. */
		inside_after = 0;
		if (scan->rule == VEC_FILL_EVENODD) {
			if (winding % 2 != 0)
				inside_after = 1;
		} else {
			if (winding != 0)
				inside_after = 1;
		}

		/* Opens an interval where the rule starts to hold, closes it where it stops. */
		if (!inside && inside_after) {
			start = crossing->x;
		} else if (inside && !inside_after) {
			raster_add_interval(scan, start, crossing->x);
		}
		inside = inside_after;
	}
}

/*
 * Turns the accumulators of the pixel row just scanned into alphas in the
 * band, and clears the accumulators.
 *
 * Each pixel's coverage is its partial lengths plus the running sum of
 * the steps (the whole intervals over it), over the number of sample
 * lines; the alpha is that times 255, rounded.
 */
static void
raster_finish_row(
	struct raster_scan *scan,
	int32_t band_row)
{
	unsigned char *row;
	double sum;
	int32_t running;
	int32_t pixel;
	int32_t last;
	int32_t alpha;
	int32_t offset;

	/* Does nothing for a row no interval touched. */
	if (scan->dirty_left > scan->dirty_right)
		return;

	/* Finds where the row's pixels go in the band. */
	offset = scan->area.left - scan->band_left;
	row = scan->band + (size_t)band_row * (size_t)scan->band_width + (size_t)offset;

	/* Makes each touched pixel's alpha from its partial lengths and the running steps. */
	last = scan->dirty_right;
	if (last > scan->width - 1)
		last = scan->width - 1;
	running = 0;
	for (pixel = scan->dirty_left; pixel <= last; pixel++) {
		running += scan->step[pixel];
		sum = scan->partial[pixel] + (double)running;
		alpha = (int32_t)(sum / (double)VEC_SUBLINES * (double)RASTER_ALPHA_MAX + 0.5);
		if (alpha > RASTER_ALPHA_MAX)
			alpha = RASTER_ALPHA_MAX;
		if (alpha < 0)
			alpha = 0;
		row[pixel] = (unsigned char)alpha;
	}

	/* Widens the range of band columns the tile row has touched. */
	if (offset + scan->dirty_left < scan->band_dirty_left)
		scan->band_dirty_left = offset + scan->dirty_left;
	if (offset + last > scan->band_dirty_right)
		scan->band_dirty_right = offset + last;

	/* Clears the accumulators the row used (the last step lies one past the last pixel). */
	for (pixel = scan->dirty_left; pixel <= scan->dirty_right + 1; pixel++) {
		scan->partial[pixel] = 0.0;
		scan->step[pixel] = 0;
	}
	scan->dirty_left = scan->width;
	scan->dirty_right = -1;
}

/*
 * Sorts the tiles of the finished band into empty, full and partial, adds
 * the full ones to the runs and the partial ones to the tiles, and clears
 * the band for the next tile row.
 *
 * Only the tile columns the band's touched range spans are looked at; the
 * others are all 0.
 */
static int
raster_flush_band(
	struct raster_scan *scan,
	struct vec_coverage *out,
	int32_t ty)
{
	unsigned char alpha[VEC_TILE_PIXELS];
	const unsigned char *source;
	int32_t first_column;
	int32_t last_column;
	int32_t column;
	int32_t row;
	int32_t tx;
	size_t zero_count;
	size_t full_count;
	size_t index;
	int error;

	/* Does nothing for a tile row no pixel touched. */
	if (scan->band_dirty_left > scan->band_dirty_right)
		return 0;

	/* Finds the tile columns the touched pixels span. */
	first_column = scan->band_dirty_left / VEC_TILE_SIZE;
	last_column = scan->band_dirty_right / VEC_TILE_SIZE;

	/* Decides each tile of the span. */
	for (column = first_column; column <= last_column; column++) {
		/* Gathers the tile's 16 rows into one block of alphas. */
		for (row = 0; row < VEC_TILE_SIZE; row++) {
			source = scan->band + (size_t)row * (size_t)scan->band_width + (size_t)column * VEC_TILE_SIZE;
			memcpy(&alpha[row * VEC_TILE_SIZE], source, VEC_TILE_SIZE);
		}

		/* Counts the pixels of alpha 0 and of alpha 255. */
		zero_count = 0;
		full_count = 0;
		for (index = 0; index < VEC_TILE_PIXELS; index++) {
			if (alpha[index] == 0)
				zero_count++;
			else if (alpha[index] == RASTER_ALPHA_MAX)
				full_count++;
		}

		/* Leaves an empty tile out. */
		if (zero_count == VEC_TILE_PIXELS)
			continue;

		/* Adds a full tile to the runs, any other to the partial tiles. */
		tx = raster_tile_of(scan->band_left) + column;
		if (full_count == VEC_TILE_PIXELS)
			error = raster_add_run(out, ty, tx, 1);
		else
			error = raster_add_tile(out, ty, tx, alpha);
		if (error != 0)
			return error;
	}

	/* Clears the band's touched columns for the next tile row. */
	for (row = 0; row < VEC_TILE_SIZE; row++) {
		memset(scan->band + (size_t)row * (size_t)scan->band_width + (size_t)first_column * VEC_TILE_SIZE,
		    0, (size_t)(last_column - first_column + 1) * VEC_TILE_SIZE);
	}
	scan->band_dirty_left = scan->band_width;
	scan->band_dirty_right = -1;

	/* Succeeded: the tile row's tiles are in out. */
	return 0;
}

/*
 * Scans the whole area, a tile row at a time, into out.
 */
static int
raster_scan_all(
	struct raster_scan *scan,
	struct vec_coverage *out)
{
	int32_t first_tile_row;
	int32_t last_tile_row;
	int32_t ty;
	int32_t first_row;
	int32_t end_row;
	int32_t row;
	int32_t sub;
	int error;

	/* Starts with nothing touched in the pixel row or in the band. */
	scan->dirty_left = scan->width;
	scan->dirty_right = -1;
	scan->band_dirty_left = scan->band_width;
	scan->band_dirty_right = -1;

	/* Scans each tile row from the one holding the area's top to the one holding its bottom. */
	first_tile_row = raster_tile_of(scan->area.top);
	last_tile_row = raster_tile_of(scan->area.bottom - 1);
	for (ty = first_tile_row; ty <= last_tile_row; ty++) {
		/* The pixel rows of the area that lie in this tile row. */
		first_row = ty * VEC_TILE_SIZE;
		if (first_row < scan->area.top)
			first_row = scan->area.top;
		end_row = ty * VEC_TILE_SIZE + VEC_TILE_SIZE;
		if (end_row > scan->area.bottom)
			end_row = scan->area.bottom;

		/* Scans each pixel row: its sample lines, then its alphas. */
		for (row = first_row; row < end_row; row++) {
			for (sub = 0; sub < VEC_SUBLINES; sub++) {
				raster_advance(scan, row * VEC_SUBLINES + sub);
				raster_sweep(scan);
			}
			raster_finish_row(scan, row - ty * VEC_TILE_SIZE);
		}

		/* Sorts the tile row's tiles into out. */
		error = raster_flush_band(scan, out, ty);
		if (error != 0)
			return error;
	}

	/* Succeeded: every tile row is in out. */
	return 0;
}

/*
 * Frees the arrays of a scan.
 */
static void
raster_free(
	struct raster_scan *scan)
{
	/* Gives back every array the scan made (free accepts NULL). */
	free(scan->segments);
	free(scan->active);
	free(scan->partial);
	free(scan->step);
	free(scan->band);
	scan->segments = NULL;
	scan->active = NULL;
	scan->partial = NULL;
	scan->step = NULL;
	scan->band = NULL;
}

/*
 * Moves a cursor to the next piece of its coverage, the one that comes
 * first on the tile grid among the next partial tile and the next run.
 * Runs of no tiles are passed over.  The item is not valid at the end.
 */
static void
raster_cursor_next(
	struct raster_cursor *cursor)
{
	const struct vec_tile *tile;
	const struct vec_run *run;
	int have_tile;
	int have_run;
	int take_tile;

	/* Passes over runs of no tiles, which carry nothing. */
	while (cursor->run_index < cursor->coverage->run_count &&
	    cursor->coverage->runs[cursor->run_index].count <= 0)
		cursor->run_index++;

	/* Tells what is left in the two arrays. */
	have_tile = cursor->tile_index < cursor->coverage->tile_count;
	have_run = cursor->run_index < cursor->coverage->run_count;
	if (!have_tile && !have_run) {
		cursor->item.valid = 0;
		return;
	}

	/* Chooses the one that comes first by row, then by column. */
	take_tile = have_tile;
	if (have_tile && have_run) {
		tile = &cursor->coverage->tiles[cursor->tile_index];
		run = &cursor->coverage->runs[cursor->run_index];
		take_tile = 0;
		if (tile->ty < run->ty)
			take_tile = 1;
		else if (tile->ty == run->ty && tile->tx < run->tx)
			take_tile = 1;
	}

	/* Reads the chosen piece. */
	if (take_tile) {
		tile = &cursor->coverage->tiles[cursor->tile_index];
		cursor->item.ty = tile->ty;
		cursor->item.start = tile->tx;
		cursor->item.end = (int64_t)tile->tx + 1;
		cursor->item.tile = tile;
		cursor->tile_index++;
	} else {
		run = &cursor->coverage->runs[cursor->run_index];
		cursor->item.ty = run->ty;
		cursor->item.start = run->tx;
		cursor->item.end = (int64_t)run->tx + (int64_t)run->count;
		cursor->item.tile = NULL;
		cursor->run_index++;
	}
	cursor->item.valid = 1;
}

/*
 * Tells whether a piece lies wholly before another on the tile grid: on
 * an earlier row, or on the same row and ending at or before the other's
 * start.
 */
static int
raster_item_before(
	const struct raster_item *first,
	const struct raster_item *second)
{
	/* Tells a piece on an earlier row is before. */
	if (first->ty < second->ty)
		return 1;

	/* Tells a piece that ends at or before the other starts, on the same row, is before. */
	if (first->ty == second->ty && first->end <= second->start)
		return 1;

	/* Tells the two overlap or the first is after. */
	return 0;
}

/*
 * Adds the columns from to to (not included) of a piece to out as they
 * are: a run's columns as a run, a tile (one column) with its alphas.
 */
static int
raster_keep_range(
	struct vec_coverage *out,
	const struct raster_item *item,
	int64_t from,
	int64_t to)
{
	int error;

	/* Keeps the columns of a run. */
	if (item->tile == NULL) {
		error = raster_add_run(out, item->ty, from, to - from);
		if (error != 0)
			return error;

		/* Succeeded: the run's columns are kept. */
		return 0;
	}

	/* Keeps a tile with its alphas. */
	error = raster_push_alphas(out, item->ty, (int32_t)from, item->tile->alpha);
	if (error != 0)
		return error;

	/* Succeeded: the tile is kept. */
	return 0;
}

/*
 * Combines the columns from to to (not included) that two pieces both
 * cover, into out.
 *
 * Two runs make a run.  A run with a tile leaves the tile when
 * intersecting (255 is the identity) and the run when uniting (255 is the
 * larger).  Two tiles are combined pixel by pixel.
 */
static int
raster_combine_tiles(
	struct vec_coverage *out,
	const struct raster_item *a,
	const struct raster_item *b,
	int64_t from,
	int64_t to,
	enum raster_combine_mode mode)
{
	unsigned char alpha[VEC_TILE_PIXELS];
	size_t index;
	unsigned value;
	int error;

	/* Combines two runs. */
	if (a->tile == NULL && b->tile == NULL) {
		error = raster_add_run(out, a->ty, from, to - from);
		if (error != 0)
			return error;

		/* Succeeded: the columns are a run. */
		return 0;
	}

	/* Combines a run with a tile: uniting keeps the run, intersecting keeps the tile. */
	if (a->tile == NULL || b->tile == NULL) {
		if (mode == RASTER_COMBINE_UNITE) {
			error = raster_add_run(out, a->ty, from, to - from);
		} else if (a->tile == NULL) {
			error = raster_keep_range(out, b, from, to);
		} else {
			error = raster_keep_range(out, a, from, to);
		}
		if (error != 0)
			return error;

		/* Succeeded: the columns are combined. */
		return 0;
	}

	/* Combines two tiles pixel by pixel: the larger alpha, or the product rounded. */
	for (index = 0; index < VEC_TILE_PIXELS; index++) {
		if (mode == RASTER_COMBINE_UNITE) {
			value = a->tile->alpha[index];
			if (b->tile->alpha[index] > value)
				value = b->tile->alpha[index];
		} else {
			value = ((unsigned)a->tile->alpha[index] * (unsigned)b->tile->alpha[index] + 127U) / 255U;
		}
		alpha[index] = (unsigned char)value;
	}
	error = raster_push_alphas(out, a->ty, (int32_t)from, alpha);
	if (error != 0)
		return error;

	/* Succeeded: the tile is combined. */
	return 0;
}

/*
 * Reads the pieces of two coverages in grid order and combines them into
 * out.
 *
 * Where only one covers a range of a tile row it is kept (unite) or
 * dropped (intersect); where both do, the overlap is combined.  A tile is
 * one column wide, so a piece that overlaps a tile overlaps it as a whole.
 */
static int
raster_combine_walk(
	struct vec_coverage *out,
	const struct vec_coverage *a,
	const struct vec_coverage *b,
	enum raster_combine_mode mode)
{
	struct raster_cursor ca;
	struct raster_cursor cb;
	int64_t from;
	int64_t to;
	int a_alone;
	int b_alone;
	int error;

	/* Starts a reader on each input at its first piece. */
	memset(&ca, 0, sizeof(ca));
	memset(&cb, 0, sizeof(cb));
	ca.coverage = a;
	cb.coverage = b;
	raster_cursor_next(&ca);
	raster_cursor_next(&cb);

	/* Combines the pieces until both inputs are read. */
	while (ca.item.valid || cb.item.valid) {
		/* Tells whether the current piece of one input is clear of the other's. */
		a_alone = 0;
		b_alone = 0;
		if (!cb.item.valid) {
			a_alone = 1;
		} else if (!ca.item.valid) {
			b_alone = 1;
		} else {
			a_alone = raster_item_before(&ca.item, &cb.item);
			if (!a_alone)
				b_alone = raster_item_before(&cb.item, &ca.item);
		}

		/* Keeps (unite) or drops (intersect) a piece that nothing of the other input touches. */
		if (a_alone) {
			if (mode == RASTER_COMBINE_UNITE) {
				error = raster_keep_range(out, &ca.item, ca.item.start, ca.item.end);
				if (error != 0)
					return error;
			}
			raster_cursor_next(&ca);
			continue;
		}
		if (b_alone) {
			if (mode == RASTER_COMBINE_UNITE) {
				error = raster_keep_range(out, &cb.item, cb.item.start, cb.item.end);
				if (error != 0)
					return error;
			}
			raster_cursor_next(&cb);
			continue;
		}

		/* The pieces overlap on one tile row.  Handles the part of one that lies left of the other (only a run can). */
		if (ca.item.start < cb.item.start) {
			if (mode == RASTER_COMBINE_UNITE) {
				error = raster_keep_range(out, &ca.item, ca.item.start, cb.item.start);
				if (error != 0)
					return error;
			}
			ca.item.start = cb.item.start;
		} else if (cb.item.start < ca.item.start) {
			if (mode == RASTER_COMBINE_UNITE) {
				error = raster_keep_range(out, &cb.item, cb.item.start, ca.item.start);
				if (error != 0)
					return error;
			}
			cb.item.start = ca.item.start;
		}

		/* Combines the columns both pieces cover. */
		from = ca.item.start;
		to = ca.item.end;
		if (cb.item.end < to)
			to = cb.item.end;
		error = raster_combine_tiles(out, &ca.item, &cb.item, from, to, mode);
		if (error != 0)
			return error;

		/* Moves past the columns combined: a piece that ends there is done, the other starts after them. */
		ca.item.start = to;
		cb.item.start = to;
		if (ca.item.start >= ca.item.end)
			raster_cursor_next(&ca);
		if (cb.item.start >= cb.item.end)
			raster_cursor_next(&cb);
	}

	/* Succeeded: every piece is combined. */
	return 0;
}

/*
 * Combines two coverages pixel by pixel into out, which every failure
 * leaves empty.
 */
static int
raster_combine(
	struct vec_coverage *out,
	const struct vec_coverage *a,
	const struct vec_coverage *b,
	enum raster_combine_mode mode)
{
	int error;

	/* Starts from an empty result, which is also what a failure leaves. */
	vec_coverage_reset(out);

	/* Combines the pieces of the two inputs. */
	error = raster_combine_walk(out, a, b, mode);
	if (error != 0) {
		vec_coverage_reset(out);
		return error;
	}

	/* Measures the pixels of nonzero alpha the result holds. */
	raster_update_bounds(out);

	/* Succeeded: out holds the combination. */
	return 0;
}
