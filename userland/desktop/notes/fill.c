/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The area the fill tool paints (ws216-p005, plan/ws216/phase005/phase.md
 * section 2): from a point of a page, the run of pixels of the point's
 * colour around it, its small gaps closed, turned into contours of page
 * points.
 *
 * The page is drawn at a fixed scale whatever the zoom (at most 4 pixels a
 * point), a band of rows at a time, by a caller's function (fill-page.c
 * draws it with the GPU).  Each band's rows go into a table of the whole
 * page: a pixel is a border where any channel differs from the seed's by
 * more than NOTES_FILL_THRESHOLD, the borders are widened by FILL_CLOSE points
 * so that small gaps close, and the fill floods the pixels that are not
 * covered, four neighbours at a time, the page's edge its border too.  A
 * row is flooded only once every row within the widening of it was read,
 * so the area does not depend on how the page was cut into bands; a band
 * is read when the flood waits for its rows.  The area is then widened
 * back (by the closing, the border's soft edge and FILL_UNDER points) so
 * that it reaches under the lines that bound it, its edge is followed
 * pixel by pixel, and the contours are simplified within a tolerance,
 * small ones dropped, and put on the edit data's grid.
 *
 * Nothing here uses Vulkan: the pixels arrive in host memory.
 */

#include "notes.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The finest scale the page is drawn at, in pixels a point, and the most pixels a side and in all. */
#define FILL_SCALE_MAX		4.0
#define FILL_SIDE_MAX		4096.0
#define FILL_PIXELS_MAX		(16.0 * 1024.0 * 1024.0)

/* The most bytes of a band's pixels, and the fewest rows a band is halved to. */
#define FILL_BAND_BYTES		(4U * 1024U * 1024U)
#define FILL_BAND_MIN		64U

/* How far the borders are widened to close gaps, in points. */
#define FILL_CLOSE		1.5

/*
 * How far past the closing the area is widened back: the width of a
 * border's soft edge that the threshold counts as border (pixels), and how
 * far under the line the area then goes (points).
 */
#define FILL_FRINGE		0.75
#define FILL_UNDER		0.1

/* Contours smaller than this many square points are dropped (specks of grain, holes of noise). */
#define FILL_AREA_MIN		1.0

/*
 * The most corners an area may have, the most corners its contours may
 * have before they are simplified (the memory and the time of following
 * them), and the first tolerance of the simplification (pixels), doubled
 * up to twice.
 */
#define FILL_POINTS_MAX		8192U
#define FILL_RAW_MAX		(1024U * 1024U)
#define FILL_TOLERANCE		0.5
#define FILL_TOLERANCE_STEPS	3U

/* The closings tried, in points: a place narrower than one is flooded with the next (review-1 L-3), the last none. */
#define FILL_CLOSINGS		4U

/* The longest distance a row keeps to its nearest border pixel. */
#define FILL_DISTANCE_CAP	255U

/* What a pixel of the page is: a border, covered by the widened borders, in the area, in the area widened back. */
#define FILL_RAW		0x01U
#define FILL_CLOSED		0x02U
#define FILL_REGION		0x04U
#define FILL_FINAL		0x08U

/* What a row of the page is: read from a band, and ready for the flood (its covering made). */
#define FILL_ROW_LOADED		0x01U
#define FILL_ROW_READY		0x02U

/* The directions along a pixel's edge (y downwards). */
#define FILL_EAST		0U
#define FILL_SOUTH		1U
#define FILL_WEST		2U
#define FILL_NORTH		3U

/*
 * A run of pixels of one row (left to right, both included) that the
 * flood looks at next.
 */
struct fill_span {
	uint32_t left;
	uint32_t right;
	uint32_t y;
};

/*
 * A list of spans: the flood's stack, or the spans that wait for rows not
 * read yet.
 */
struct fill_list {
	struct fill_span *spans;
	size_t count;
	size_t capacity;
};

/*
 * A contour as it is followed: the corners where it turns, in pixels of
 * the page, the area inside on the right (y downwards).
 */
struct fill_contour {
	double *xy;
	size_t count;
	size_t capacity;
};

/*
 * The page as one fill sees it.
 *
 * The tables have a byte a pixel (FILL_* flags, and a distance to the
 * nearest border or area pixel along the row) and a byte a row; they
 * live for one notes_fill_find().  The band buffer holds one band's pixels
 * as the caller's function draws them.
 */
struct fill_map {
	/* The page in pixels, and the pixels a point. */
	uint32_t width;
	uint32_t height;
	double scale;

	/* The tables. */
	unsigned char *flags;
	unsigned char *distance;
	unsigned char *rows;

	/* The caller's function that draws a band, the band's buffer and rows, and the bands read. */
	notes_fill_band *band;
	void *context;
	unsigned char *pixels;
	uint32_t band_height;
	unsigned bands;

	/* The seed's pixel, and its colour once read. */
	uint32_t seed_x;
	uint32_t seed_y;
	unsigned char seed[4];
	int seeded;

	/* The closing: its rows each way and the half width of its disc at each row off. */
	uint32_t close_reach;
	uint32_t close_widths[FILL_DISTANCE_CAP + 1U];

	/* The flood's stack and the spans that wait for rows. */
	struct fill_list stack;
	struct fill_list waiting;

	/* The area: its box (pixels, both ends included) and its pixels. */
	uint32_t left;
	uint32_t top;
	uint32_t right;
	uint32_t bottom;
	size_t area;
};

static int fill_attempt(struct fill_map *map, double close_radius, int *narrow);
static void fill_widths(double radius, uint32_t *reach, uint32_t *widths);
static int fill_load(struct fill_map *map, uint32_t first, uint32_t last);
static int fill_load_row(struct fill_map *map, uint32_t row);
static int fill_band_read(struct fill_map *map, uint32_t top);
static void fill_row_borders(struct fill_map *map, uint32_t y, const unsigned char *pixels);
static void fill_row_distance(struct fill_map *map, uint32_t y, unsigned flag, uint32_t from, uint32_t to);
static int fill_ready(struct fill_map *map, uint32_t first, uint32_t last);
static void fill_row_close(struct fill_map *map, uint32_t y);
static int fill_seed_free(struct fill_map *map, int *found);
static int fill_flood(struct fill_map *map);
static int fill_flood_row(struct fill_map *map, const struct fill_span *span);
static int fill_push(struct fill_list *list, uint32_t left, uint32_t right, uint32_t y);
static void fill_back(struct fill_map *map, double radius, uint32_t *box);
static int fill_contours(const struct fill_map *map, const uint32_t *box, double tolerance, float page_width, float page_height, struct notes_fill_region *region, int *final);
static int fill_keep(const struct fill_map *map, const struct fill_contour *contour, const unsigned char *keep, float page_width, float page_height, struct notes_fill_region *region, size_t *capacity);
static void fill_seed_colour(struct fill_map *map, uint32_t top, uint32_t height);
static int fill_follow(const struct fill_map *map, const uint32_t *box, unsigned char *visited, uint32_t x0, uint32_t y0, struct fill_contour *contour);
static int fill_inside(const struct fill_map *map, const uint32_t *box, int64_t x, int64_t y);
static int fill_corner(struct fill_contour *contour, double x, double y);
static double fill_area(const struct fill_contour *contour);
static int fill_simplify(const struct fill_contour *contour, double tolerance, unsigned char *keep);
static double fill_segment_distance(const double *point, const double *a, const double *b);
static void fill_unready(struct fill_map *map);
static void fill_map_free(struct fill_map *map);

/*
 * Tells the scale a page is drawn at for the fill, in pixels a point: at
 * most FILL_SCALE_MAX, at most FILL_SIDE_MAX pixels wide (the widest image
 * a Raspberry Pi 4 makes), and at most FILL_PIXELS_MAX pixels in all.
 */
double
notes_fill_scale(
	float page_width,
	float page_height)
{
	double scale;
	double limit;

	/* The finest scale. */
	scale = FILL_SCALE_MAX;

	/* No wider than the widest image. */
	limit = FILL_SIDE_MAX / (double)page_width;
	if (limit < scale)
		scale = limit;

	/* No more pixels than the most. */
	limit = sqrt(FILL_PIXELS_MAX / ((double)page_width * (double)page_height));
	if (limit < scale)
		scale = limit;

	/* Reports the scale. */
	return scale;
}

/*
 * Tells how many pixels wide a page is drawn for the fill: its width at
 * notes_fill_scale(), at most FILL_SIDE_MAX (the one place both the area's
 * search and the bands' drawing take it from, p005 code review 1).
 */
uint32_t
notes_fill_width(
	float page_width,
	float page_height)
{
	uint32_t width;

	/* The width at the scale, kept within the widest image. */
	width = (uint32_t)ceil((double)page_width * notes_fill_scale(page_width, page_height));
	if (width > (uint32_t)FILL_SIDE_MAX)
		width = (uint32_t)FILL_SIDE_MAX;

	/* Reports the width. */
	return width;
}

/*
 * Finds the area the fill tool paints from a point of a page (points), as
 * contours of page points: the first the area's outside, the others its
 * holes, turning the other way (filled by the nonzero rule).  under tells
 * that the fill goes under the strokes that bound it (it is put below
 * them): the area then reaches FILL_UNDER points further under them.
 *
 * band draws rows of the page at notes_fill_scale() and is called as the
 * flood needs them; it returns 0, ENOMEM when the device refused memory
 * (the band is halved and drawn again, down to FILL_BAND_MIN rows), or
 * another errno value, which ends the fill.  Returns 0, EINVAL for a
 * point off the page, ENOENT for an area smaller than FILL_AREA_MIN,
 * E2BIG for an area of more than FILL_POINTS_MAX corners at the coarsest
 * tolerance (region->point_count tells how many), ENOMEM, or the band's
 * error.
 */
int
notes_fill_find(
	float page_width,
	float page_height,
	float x,
	float y,
	int under,
	notes_fill_band *band,
	void *context,
	struct notes_fill_region *region)
{
	static const double closings[FILL_CLOSINGS] = { FILL_CLOSE, FILL_CLOSE / 2.0, FILL_CLOSE / 4.0, 0.0 };
	struct fill_map map;
	size_t pixels;
	uint32_t box[4];
	double back_radius;
	double close_radius;
	double tolerance;
	unsigned closing;
	unsigned step;
	int narrow;
	int final;
	int error;

	/* Nothing is found yet. */
	memset(region, 0, sizeof(*region));
	memset(&map, 0, sizeof(map));

	/* A point off the page has nothing to fill. */
	if (!(x >= 0.0f) || !(y >= 0.0f))
		return EINVAL;
	if (!(x < page_width) || !(y < page_height))
		return EINVAL;

	/* The page in pixels at the fill's scale, no wider than the widest image. */
	map.scale = notes_fill_scale(page_width, page_height);
	map.width = notes_fill_width(page_width, page_height);
	map.height = (uint32_t)ceil((double)page_height * map.scale);
	map.band = band;
	map.context = context;

	/* The seed's pixel, on the page. */
	map.seed_x = (uint32_t)floor((double)x * map.scale);
	map.seed_y = (uint32_t)floor((double)y * map.scale);
	if (map.seed_x >= map.width)
		map.seed_x = map.width - 1U;
	if (map.seed_y >= map.height)
		map.seed_y = map.height - 1U;

	/*
	 * A band: as many rows as fit FILL_BAND_BYTES, no fewer than
	 * FILL_BAND_MIN, no more than the page or the tallest image.
	 */
	map.band_height = FILL_BAND_BYTES / (map.width * 4U);
	if (map.band_height < FILL_BAND_MIN)
		map.band_height = FILL_BAND_MIN;
	if (map.band_height > (uint32_t)FILL_SIDE_MAX)
		map.band_height = (uint32_t)FILL_SIDE_MAX;
	if (map.band_height > map.height)
		map.band_height = map.height;

	/* The tables and the band's buffer. */
	pixels = (size_t)map.width * map.height;
	map.flags = calloc(pixels, 1U);
	if (map.flags == NULL) {
		fill_map_free(&map);
		return ENOMEM;
	}

	/* The distances. */
	map.distance = malloc(pixels);
	if (map.distance == NULL) {
		fill_map_free(&map);
		return ENOMEM;
	}

	/* The rows' states. */
	map.rows = calloc(map.height, 1U);
	if (map.rows == NULL) {
		fill_map_free(&map);
		return ENOMEM;
	}

	/* The band's buffer, of the most rows a band has. */
	map.pixels = malloc((size_t)map.width * map.band_height * 4U);
	if (map.pixels == NULL) {
		fill_map_free(&map);
		return ENOMEM;
	}

	/*
	 * The flood with the gaps closed; a seed in a place narrower than the
	 * closing (every pixel near it covered) floods again with a smaller
	 * one, the rows read and their borders kept, and at last with none.
	 */
	close_radius = 0.0;
	for (closing = 0; closing < FILL_CLOSINGS; closing++) {
		close_radius = closings[closing] * map.scale;
		if (closing > 0U)
			fill_unready(&map);
		error = fill_attempt(&map, close_radius, &narrow);
		if (error != 0) {
			fill_map_free(&map);
			return error;
		}
		if (!narrow)
			break;
	}

	/*
	 * A seed narrower than every closing, or an area of no pixel (a dot too
	 * small to hold one, p005 code review M2), is too small to fill.
	 */
	if (narrow || map.area == 0U) {
		fill_map_free(&map);
		return ENOENT;
	}

	/*
	 * The area widened back to the lines that bound it, and under their
	 * soft edge when it goes under them; above them it stops at their soft
	 * edge, so that it does not cover their edges (review-1 M-2).
	 */
	back_radius = close_radius;
	if (under)
		back_radius += FILL_FRINGE + FILL_UNDER * map.scale;
	fill_back(&map, back_radius, box);

	/* The contours in points, the tolerance doubled while they have too many corners. */
	tolerance = FILL_TOLERANCE;
	error = E2BIG;
	for (step = 0; step < FILL_TOLERANCE_STEPS; step++) {
		error = fill_contours(&map, box, tolerance, page_width, page_height, region, &final);
		if (error != E2BIG || final)
			break;
		tolerance *= 2.0;
	}

	/* The measurements of the fill. */
	region->scale = map.scale;
	region->width = map.width;
	region->height = map.height;
	region->bands = map.bands;
	region->band_height = map.band_height;
	region->tolerance_step = step;
	region->pixels = map.area;

	/* The tables go. */
	fill_map_free(&map);

	/* Reports an area too small or too complex (its corner count kept), or a failure. */
	if (error != 0)
		return error;

	/* Succeeded: the area's contours. */
	return 0;
}

/*
 * Frees the contours of an area found (the measurements stay).
 */
void
notes_fill_region_free(
	struct notes_fill_region *region)
{
	/* The corners and the counts. */
	free(region->points);
	free(region->contours);
	region->points = NULL;
	region->point_count = 0;
	region->contours = NULL;
	region->contour_count = 0;
}

/*
 * Floods the area from the seed with the borders widened by a radius in
 * pixels.  *narrow tells that every pixel near the seed was covered (the
 * caller floods again without closing); the area is then empty.
 */
static int
fill_attempt(
	struct fill_map *map,
	double close_radius,
	int *narrow)
{
	uint32_t first;
	uint32_t last;
	int found;
	int error;

	/* Nothing is narrow yet, and the area is empty. */
	*narrow = 0;
	map->area = 0;
	map->left = map->width;
	map->top = map->height;
	map->right = 0;
	map->bottom = 0;

	/* The closing's disc. */
	fill_widths(close_radius, &map->close_reach, map->close_widths);

	/* The band around the seed, which gives the seed's colour (read once). */
	if (!map->seeded) {
		error = fill_band_read(map, map->seed_y);
		if (error != 0)
			return error;
	}

	/* The rows the seed's search needs: the seed's row and the closing, each way, twice. */
	first = 0;
	if (map->seed_y > 2U * map->close_reach + 1U)
		first = map->seed_y - 2U * map->close_reach - 1U;
	last = map->seed_y + 2U * map->close_reach + 1U;
	if (last >= map->height)
		last = map->height - 1U;
	error = fill_load(map, first, last);
	if (error != 0)
		return error;

	/* A seed the closing covers moves to the nearest pixel it does not. */
	error = fill_seed_free(map, &found);
	if (error != 0)
		return error;
	if (!found) {
		*narrow = 1;
		return 0;
	}

	/* The flood. */
	error = fill_flood(map);
	if (error != 0)
		return error;

	/* Succeeded: the area is marked. */
	return 0;
}

/*
 * Measures a disc of a radius in pixels: the rows it reaches each way, and
 * the half width it has at each row off its centre.
 */
static void
fill_widths(
	double radius,
	uint32_t *reach,
	uint32_t *widths)
{
	uint32_t row;
	double half;

	/* The rows each way, within what a distance can tell. */
	*reach = (uint32_t)floor(radius);
	if (*reach > FILL_DISTANCE_CAP - 1U)
		*reach = FILL_DISTANCE_CAP - 1U;

	/* The half width at each row off the centre. */
	for (row = 0; row <= *reach; row++) {
		half = floor(sqrt(radius * radius - (double)row * (double)row) + 0.5);
		if (half > (double)(FILL_DISTANCE_CAP - 1U))
			half = (double)(FILL_DISTANCE_CAP - 1U);
		widths[row] = (uint32_t)half;
	}
}

/* Reads every row from first to last (both included) that is not read yet, a band at a time. */
static int
fill_load(
	struct fill_map *map,
	uint32_t first,
	uint32_t last)
{
	uint32_t row;
	int error;

	/* Each row not read, a band at a time. */
	for (row = first; row <= last; row++) {
		while ((map->rows[row] & FILL_ROW_LOADED) == 0U) {
			error = fill_load_row(map, row);
			if (error != 0)
				return error;
		}
	}

	/* Succeeded: the rows are read. */
	return 0;
}

/*
 * Reads a band that holds a row not read: one that ends at it when the row
 * below is read (the flood goes up the page), else one that starts at it.
 * A band halved by the device may then miss the row; the caller asks
 * again.
 */
static int
fill_load_row(
	struct fill_map *map,
	uint32_t row)
{
	uint32_t top;
	int error;

	/* Where the band starts. */
	top = row;
	if (row + 1U < map->height && (map->rows[row + 1U] & FILL_ROW_LOADED) != 0U) {
		top = 0;
		if (row + 1U > map->band_height)
			top = row + 1U - map->band_height;
	}

	/* The band. */
	error = fill_band_read(map, top);
	if (error != 0)
		return error;

	/* Succeeded: a band is read. */
	return 0;
}

/*
 * Draws the band that starts at a row and reads its rows not read yet.
 * A band the device refused memory for is halved, down to FILL_BAND_MIN
 * rows.  The first band read is put round the seed's row whatever the row
 * asked for, and gives the seed's colour.
 */
static int
fill_band_read(
	struct fill_map *map,
	uint32_t top)
{
	uint32_t height;
	uint32_t row;
	int error;

	/* The band, smaller each time the device refuses it; the first is kept round the seed's row. */
	for (;;) {
		if (!map->seeded) {
			top = 0;
			if (map->seed_y > map->band_height / 2U)
				top = map->seed_y - map->band_height / 2U;
			if (top + map->band_height > map->height)
				top = map->height - map->band_height;
		}
		height = map->band_height;
		if (top + height > map->height)
			height = map->height - top;
		error = map->band(map->context, top, height, map->pixels);
		if (error == 0)
			break;

		/* Only a refusal for memory is met with a smaller band, and only down to the fewest rows. */
		if (error != ENOMEM)
			return error;
		if (map->band_height / 2U < FILL_BAND_MIN)
			return ENOMEM;
		map->band_height /= 2U;
	}
	map->bands++;

	/* The seed's colour, from the first band. */
	if (!map->seeded) {
		if (map->seed_y < top || map->seed_y >= top + height)
			return EINVAL;
		fill_seed_colour(map, top, height);
		map->seeded = 1;
	}

	/* Each row of the band not read before: its borders and their distances. */
	for (row = top; row < top + height; row++) {
		if ((map->rows[row] & FILL_ROW_LOADED) != 0U)
			continue;
		fill_row_borders(map, row, map->pixels + (size_t)(row - top) * map->width * 4U);
		fill_row_distance(map, row, FILL_RAW, 0U, map->width - 1U);
		map->rows[row] |= FILL_ROW_LOADED;
	}

	/* Succeeded: the band's rows are read. */
	return 0;
}

/*
 * Takes the seed's colour from the band that holds its row (review-1
 * L-4): of the pixels around the seed (5 x 5, in the band), the one near
 * the colour of the most others (within NOTES_FILL_THRESHOLD), among those
 * the nearest the seed's own, so that a seed on a soft edge takes the
 * colour of the paper or of the line beside it, not the grey between.
 */
static void
fill_seed_colour(
	struct fill_map *map,
	uint32_t top,
	uint32_t height)
{
	const unsigned char *pixels[25];
	const unsigned char *seed;
	const unsigned char *best;
	unsigned near[25];
	unsigned count;
	unsigned best_count;
	unsigned index;
	unsigned other;
	unsigned channel;
	int distance;
	int best_distance;
	int difference;
	int64_t x;
	int64_t y;
	int64_t dx;
	int64_t dy;

	/* The pixels around the seed that are in the band and on the page. */
	count = 0;
	for (dy = -2; dy <= 2; dy++) {
		for (dx = -2; dx <= 2; dx++) {
			x = (int64_t)map->seed_x + dx;
			y = (int64_t)map->seed_y + dy;
			if (x < 0 || x >= (int64_t)map->width)
				continue;
			if (y < (int64_t)top || y >= (int64_t)top + (int64_t)height)
				continue;
			pixels[count] = map->pixels + ((size_t)(y - (int64_t)top) * map->width + (size_t)x) * 4U;
			count++;
		}
	}

	/* How many of them each is near. */
	for (index = 0; index < count; index++) {
		near[index] = 0;
		for (other = 0; other < count; other++) {
			for (channel = 0; channel < 4U; channel++) {
				difference = (int)pixels[index][channel] - (int)pixels[other][channel];
				if (difference > NOTES_FILL_THRESHOLD || difference < -NOTES_FILL_THRESHOLD)
					break;
			}
			if (channel == 4U)
				near[index]++;
		}
	}

	/* The one near the most, the nearer the seed's own colour among equals. */
	seed = map->pixels + ((size_t)(map->seed_y - top) * map->width + map->seed_x) * 4U;
	best = seed;
	best_count = 0;
	best_distance = 0;
	for (index = 0; index < count; index++) {
		/* The largest difference of a channel from the seed's own. */
		distance = 0;
		for (channel = 0; channel < 4U; channel++) {
			difference = (int)pixels[index][channel] - (int)seed[channel];
			if (difference < 0)
				difference = -difference;
			if (difference > distance)
				distance = difference;
		}

		/* More near it, or as many and nearer the seed. */
		if (near[index] > best_count || (near[index] == best_count && distance < best_distance)) {
			best = pixels[index];
			best_count = near[index];
			best_distance = distance;
		}
	}

	/* The seed's colour. */
	memcpy(map->seed, best, 4U);
}

/* Marks the borders of a row from its pixels: any channel more than NOTES_FILL_THRESHOLD from the seed's. */
static void
fill_row_borders(
	struct fill_map *map,
	uint32_t y,
	const unsigned char *pixels)
{
	unsigned char *flags;
	unsigned channel;
	uint32_t x;
	int difference;

	/* Each pixel of the row. */
	flags = map->flags + (size_t)y * map->width;
	for (x = 0; x < map->width; x++) {
		for (channel = 0; channel < 4U; channel++) {
			/* A channel far from the seed's makes the pixel a border. */
			difference = (int)pixels[x * 4U + channel] - (int)map->seed[channel];
			if (difference > NOTES_FILL_THRESHOLD || difference < -NOTES_FILL_THRESHOLD) {
				flags[x] |= FILL_RAW;
				break;
			}
		}
	}
}

/*
 * Measures, for the pixels of a row from one column to another, the
 * distance along the row to the nearest pixel with a flag (in those
 * columns), at most FILL_DISTANCE_CAP.
 */
static void
fill_row_distance(
	struct fill_map *map,
	uint32_t y,
	unsigned flag,
	uint32_t from,
	uint32_t to)
{
	const unsigned char *flags;
	unsigned char *distance;
	uint32_t run;
	uint32_t x;

	/* From the left: the distance since the last pixel with the flag. */
	flags = map->flags + (size_t)y * map->width;
	distance = map->distance + (size_t)y * map->width;
	run = FILL_DISTANCE_CAP;
	for (x = from; x <= to; x++) {
		if ((flags[x] & flag) != 0U) {
			run = 0;
		} else if (run < FILL_DISTANCE_CAP) {
			run++;
		}
		distance[x] = (unsigned char)run;
	}

	/* From the right: the nearer of the two. */
	run = FILL_DISTANCE_CAP;
	x = to + 1U;
	while (x > from) {
		x--;
		if ((flags[x] & flag) != 0U) {
			run = 0;
		} else if (run < FILL_DISTANCE_CAP) {
			run++;
		}
		if (run < distance[x])
			distance[x] = (unsigned char)run;
	}
}

/*
 * Makes the rows from first to last ready for the flood: the rows the
 * closing reaches from them read, and their covering made.
 */
static int
fill_ready(
	struct fill_map *map,
	uint32_t first,
	uint32_t last)
{
	uint32_t from;
	uint32_t to;
	uint32_t row;
	int error;

	/* The rows the closing reaches. */
	from = 0;
	if (first > map->close_reach)
		from = first - map->close_reach;
	to = last + map->close_reach;
	if (to >= map->height)
		to = map->height - 1U;
	error = fill_load(map, from, to);
	if (error != 0)
		return error;

	/* Each row's covering, once. */
	for (row = first; row <= last; row++) {
		if ((map->rows[row] & FILL_ROW_READY) != 0U)
			continue;
		fill_row_close(map, row);
		map->rows[row] |= FILL_ROW_READY;
	}

	/* Succeeded: the rows are ready. */
	return 0;
}

/*
 * Covers the pixels of a row within the closing's disc of a border (the
 * rows around it read): a pixel is covered when a row off by dy has a
 * border within the disc's half width at dy.
 */
static void
fill_row_close(
	struct fill_map *map,
	uint32_t y)
{
	const unsigned char *distance;
	unsigned char *flags;
	uint32_t width;
	uint32_t reach;
	uint32_t other;
	uint32_t x;
	int64_t offset;

	/* Each row the disc reaches, on the page. */
	flags = map->flags + (size_t)y * map->width;
	reach = map->close_reach;
	for (offset = -(int64_t)reach; offset <= (int64_t)reach; offset++) {
		/* A row off the page has no borders. */
		if ((int64_t)y + offset < 0 || (int64_t)y + offset >= (int64_t)map->height)
			continue;

		/* Covers what that row's borders reach. */
		other = (uint32_t)((int64_t)y + offset);
		distance = map->distance + (size_t)other * map->width;
		width = map->close_widths[offset < 0 ? -offset : offset];
		for (x = 0; x < map->width; x++) {
			if (distance[x] <= width)
				flags[x] |= FILL_CLOSED;
		}
	}
}

/*
 * Moves the seed off the covering when the closing covers it: to the
 * nearest pixel within the closing (and one more) that is not covered,
 * the first in the rows' order among the nearest.  *found is 0 when there
 * is none.
 */
static int
fill_seed_free(
	struct fill_map *map,
	int *found)
{
	uint32_t first;
	uint32_t last;
	uint32_t row;
	uint32_t x;
	int64_t dx;
	int64_t dy;
	int64_t search;
	int64_t best;
	int64_t distance;
	uint32_t best_x;
	uint32_t best_y;
	int error;

	/* The rows the search looks at are made ready. */
	*found = 0;
	search = (int64_t)map->close_reach + 1;
	first = 0;
	if ((int64_t)map->seed_y > search)
		first = (uint32_t)((int64_t)map->seed_y - search);
	last = (uint32_t)((int64_t)map->seed_y + search);
	if (last >= map->height)
		last = map->height - 1U;
	error = fill_ready(map, first, last);
	if (error != 0)
		return error;

	/* A seed not covered stays. */
	if ((map->flags[(size_t)map->seed_y * map->width + map->seed_x] & FILL_CLOSED) == 0U) {
		*found = 1;
		return 0;
	}

	/* The nearest pixel not covered within the search's disc. */
	best = search * search + 1;
	best_x = map->seed_x;
	best_y = map->seed_y;
	for (row = first; row <= last; row++) {
		for (x = 0; x < map->width; x++) {
			/* Only a pixel within the disc counts. */
			dx = (int64_t)x - (int64_t)map->seed_x;
			dy = (int64_t)row - (int64_t)map->seed_y;
			if (dx < -search || dx > search)
				continue;
			distance = dx * dx + dy * dy;
			if (distance >= best)
				continue;

			/* A pixel the closing does not cover is the nearest so far. */
			if ((map->flags[(size_t)row * map->width + x] & FILL_CLOSED) == 0U) {
				best = distance;
				best_x = x;
				best_y = row;
			}
		}
	}

	/* None is near: the place is narrower than the closing. */
	if (best > search * search)
		return 0;

	/* Succeeded: the seed moves there. */
	map->seed_x = best_x;
	map->seed_y = best_y;
	*found = 1;
	return 0;
}

/*
 * Floods the pixels not covered from the seed, four neighbours at a time.
 * A span of a row that is not ready waits; when only waiting spans are
 * left, their rows are made ready (reading the bands they need) and the
 * flood goes on from them.
 */
static int
fill_flood(
	struct fill_map *map)
{
	struct fill_span span;
	size_t index;
	int error;

	/* The seed's pixel starts the flood. */
	error = fill_push(&map->stack, map->seed_x, map->seed_x, map->seed_y);
	if (error != 0)
		return error;

	/* Until nothing is left, nor waits. */
	for (;;) {
		/* Each span on the stack: a row ready is flooded, another waits. */
		while (map->stack.count > 0U) {
			map->stack.count--;
			span = map->stack.spans[map->stack.count];
			if ((map->rows[span.y] & FILL_ROW_READY) == 0U) {
				error = fill_push(&map->waiting, span.left, span.right, span.y);
				if (error != 0)
					return error;
				continue;
			}

			/* The span's row. */
			error = fill_flood_row(map, &span);
			if (error != 0)
				return error;
		}

		/* Nothing waits: the area is whole. */
		if (map->waiting.count == 0U)
			break;

		/* The waiting spans' rows are made ready, and the spans go back on the stack. */
		for (index = 0; index < map->waiting.count; index++) {
			span = map->waiting.spans[index];
			error = fill_ready(map, span.y, span.y);
			if (error != 0)
				return error;
			error = fill_push(&map->stack, span.left, span.right, span.y);
			if (error != 0)
				return error;
		}
		map->waiting.count = 0;
	}

	/* Succeeded: the area is marked. */
	return 0;
}

/*
 * Floods the runs of open pixels of a ready row that meet a span, and
 * pushes the rows above and below each run.
 */
static int
fill_flood_row(
	struct fill_map *map,
	const struct fill_span *span)
{
	unsigned char *flags;
	uint32_t x;
	uint32_t left;
	uint32_t right;
	int error;

	/* Each open pixel of the span starts a run. */
	flags = map->flags + (size_t)span->y * map->width;
	x = span->left;
	while (x <= span->right) {
		/* A covered pixel, or one in the area already, is passed. */
		if ((flags[x] & (FILL_CLOSED | FILL_REGION)) != 0U) {
			x++;
			continue;
		}

		/* The run reaches left and right to the first covered pixel or the page's edge. */
		left = x;
		while (left > 0U && (flags[left - 1U] & (FILL_CLOSED | FILL_REGION)) == 0U)
			left--;
		right = x;
		while (right + 1U < map->width && (flags[right + 1U] & (FILL_CLOSED | FILL_REGION)) == 0U)
			right++;

		/* The run is in the area. */
		for (x = left; x <= right; x++)
			flags[x] |= FILL_REGION;
		map->area += (size_t)(right - left + 1U);

		/* The area's box grows to the run. */
		if (left < map->left)
			map->left = left;
		if (right > map->right)
			map->right = right;
		if (span->y < map->top)
			map->top = span->y;
		if (span->y > map->bottom)
			map->bottom = span->y;

		/* The rows above and below the run are looked at next. */
		if (span->y > 0U) {
			error = fill_push(&map->stack, left, right, span->y - 1U);
			if (error != 0)
				return error;
		}
		if (span->y + 1U < map->height) {
			error = fill_push(&map->stack, left, right, span->y + 1U);
			if (error != 0)
				return error;
		}

		/* The scan goes on after the run. */
		x = right + 1U;
	}

	/* Succeeded: the row's runs are flooded. */
	return 0;
}

/* Adds a span to a list, growing it. */
static int
fill_push(
	struct fill_list *list,
	uint32_t left,
	uint32_t right,
	uint32_t y)
{
	struct fill_span *larger;
	size_t capacity;

	/* Grows the list when it is full. */
	if (list->count == list->capacity) {
		capacity = list->capacity * 2U;
		if (capacity < 256U)
			capacity = 256U;
		larger = realloc(list->spans, capacity * sizeof(*larger));
		if (larger == NULL)
			return ENOMEM;
		list->spans = larger;
		list->capacity = capacity;
	}

	/* Succeeded: the span is the list's last. */
	list->spans[list->count].left = left;
	list->spans[list->count].right = right;
	list->spans[list->count].y = y;
	list->count++;
	return 0;
}

/*
 * Widens the area by a disc of a radius in pixels (FILL_FINAL), and gives
 * the box of the widened area (left, top, right, bottom, both ends
 * included).  The distances along the rows are measured to the area now
 * (the borders' are not needed any more).
 */
static void
fill_back(
	struct fill_map *map,
	double radius,
	uint32_t *box)
{
	const unsigned char *distance;
	unsigned char *flags;
	uint32_t widths[FILL_DISTANCE_CAP + 1U];
	uint32_t reach;
	uint32_t width;
	uint32_t other;
	uint32_t row;
	uint32_t x;
	int64_t offset;

	/* The disc, and the box the widened area can reach. */
	fill_widths(radius, &reach, widths);
	box[0] = 0;
	if (map->left > widths[0])
		box[0] = map->left - widths[0];
	box[1] = 0;
	if (map->top > reach)
		box[1] = map->top - reach;
	box[2] = map->right + widths[0];
	if (box[2] >= map->width)
		box[2] = map->width - 1U;
	box[3] = map->bottom + reach;
	if (box[3] >= map->height)
		box[3] = map->height - 1U;

	/* The distances to the area along each row of the area's box. */
	for (row = map->top; row <= map->bottom; row++)
		fill_row_distance(map, row, FILL_REGION, box[0], box[2]);

	/* Each row of the widened box: a pixel within the disc of the area is in it. */
	map->area = 0;
	for (row = box[1]; row <= box[3]; row++) {
		flags = map->flags + (size_t)row * map->width;
		for (offset = -(int64_t)reach; offset <= (int64_t)reach; offset++) {
			/* Only the area's rows have area pixels. */
			if ((int64_t)row + offset < (int64_t)map->top || (int64_t)row + offset > (int64_t)map->bottom)
				continue;

			/* What that row's area reaches. */
			other = (uint32_t)((int64_t)row + offset);
			distance = map->distance + (size_t)other * map->width;
			width = widths[offset < 0 ? -offset : offset];
			for (x = box[0]; x <= box[2]; x++) {
				if (distance[x] <= width)
					flags[x] |= FILL_FINAL;
			}
		}

		/* The widened area's pixels of the row. */
		for (x = box[0]; x <= box[2]; x++) {
			if ((flags[x] & FILL_FINAL) != 0U)
				map->area++;
		}
	}
}

/*
 * Follows the contours of the widened area within its box, a contour at a
 * time, and keeps each in the area's corners as it goes (fill_keep()):
 * from each edge with the area below and none above that no contour went
 * along yet, the first the outside (it holds the topmost row).  A contour
 * smaller than FILL_AREA_MIN square points is dropped; when the outside
 * is, the area is too small (ENOENT).  E2BIG when the corners kept are
 * more than FILL_POINTS_MAX (*final is 0: a coarser tolerance may do), or
 * the corners followed more than FILL_RAW_MAX (*final is 1).
 */
static int
fill_contours(
	const struct fill_map *map,
	const uint32_t *box,
	double tolerance,
	float page_width,
	float page_height,
	struct notes_fill_region *region,
	int *final)
{
	struct fill_contour contour;
	unsigned char *visited;
	unsigned char *keep;
	size_t box_width;
	size_t followed;
	size_t capacity;
	size_t kept;
	double area;
	double area_min;
	uint32_t x;
	uint32_t y;
	int inside;
	int above;
	int error;

	/* Nothing is kept; a mark for each top edge of the box's pixels. */
	*final = 0;
	notes_fill_region_free(region);
	memset(&contour, 0, sizeof(contour));
	keep = NULL;
	capacity = 0;
	followed = 0;
	area_min = FILL_AREA_MIN * map->scale * map->scale;
	box_width = (size_t)(box[2] - box[0] + 1U);
	visited = calloc(box_width * (size_t)(box[3] - box[1] + 2U), 1U);
	if (visited == NULL)
		return ENOMEM;

	/* Each pixel of the area with no area above starts a contour, unless one went along its top edge. */
	error = 0;
	for (y = box[1]; y <= box[3] && error == 0; y++) {
		for (x = box[0]; x <= box[2]; x++) {
			/* Only a top edge of the area. */
			inside = fill_inside(map, box, x, y);
			above = fill_inside(map, box, x, (int64_t)y - 1);
			if (!inside || above)
				continue;
			if (visited[(size_t)(y - box[1]) * box_width + (x - box[0])] != 0U)
				continue;

			/* The contour's corners. */
			contour.count = 0;
			error = fill_follow(map, box, visited, x, y, &contour);
			if (error != 0)
				break;

			/* Too many corners to follow: the area is too complex at any tolerance. */
			followed += contour.count;
			if (followed > FILL_RAW_MAX) {
				region->point_count = followed;
				*final = 1;
				error = E2BIG;
				break;
			}

			/* A speck or a pinhole is dropped; an outside that small leaves no area. */
			area = fill_area(&contour);
			if (area < area_min && area > -area_min) {
				if (region->contour_count == 0U) {
					*final = 1;
					error = ENOENT;
					break;
				}
				continue;
			}

			/* The marks of the corners that stay. */
			free(keep);
			keep = malloc(contour.count);
			if (keep == NULL) {
				error = ENOMEM;
				break;
			}
			error = fill_simplify(&contour, tolerance, keep);
			if (error != 0)
				break;

			/* The corners that stay, in points. */
			kept = region->contour_count;
			error = fill_keep(map, &contour, keep, page_width, page_height, region, &capacity);
			if (error != 0)
				break;

			/* An outside of less than three corners leaves no area. */
			if (kept == 0U && region->contour_count == 0U) {
				*final = 1;
				error = ENOENT;
				break;
			}
		}
	}
	free(keep);
	free(contour.xy);
	free(visited);

	/* A failure, or an area too small or too complex to follow, keeps nothing. */
	if (error != 0) {
		followed = region->point_count;
		notes_fill_region_free(region);
		if (error == E2BIG)
			region->point_count = followed;
		return error;
	}

	/* Too many corners for an area: the count is kept, the corners are not. */
	if (region->point_count > FILL_POINTS_MAX) {
		followed = region->point_count;
		notes_fill_region_free(region);
		region->point_count = followed;
		return E2BIG;
	}

	/* Succeeded: the area's contours. */
	return 0;
}

/*
 * Adds the corners of a contour that stay to the area: on the page, on the
 * edit data's grid, a corner that repeats the one before left out.  A
 * contour left with less than three corners encloses nothing and is not
 * added.  *capacity is the room of the area's corners.
 */
static int
fill_keep(
	const struct fill_map *map,
	const struct fill_contour *contour,
	const unsigned char *keep,
	float page_width,
	float page_height,
	struct notes_fill_region *region,
	size_t *capacity)
{
	struct pdf_point *larger;
	size_t *counts;
	struct pdf_point corner;
	size_t start;
	size_t index;

	/* Room for every corner of the contour. */
	if (region->point_count + contour->count > *capacity) {
		*capacity = (*capacity + contour->count) * 2U;
		larger = realloc(region->points, *capacity * sizeof(*larger));
		if (larger == NULL)
			return ENOMEM;
		region->points = larger;
	}

	/* Each corner that stays, on the page and the grid, unless it repeats the one before. */
	start = region->point_count;
	for (index = 0; index < contour->count; index++) {
		if (!keep[index])
			continue;
		corner.x = contour->xy[index * 2U] / map->scale;
		corner.y = contour->xy[index * 2U + 1U] / map->scale;
		if (corner.x > (double)page_width)
			corner.x = (double)page_width;
		if (corner.y > (double)page_height)
			corner.y = (double)page_height;
		corner.x = (double)notes_quantize((float)corner.x);
		corner.y = (double)notes_quantize((float)corner.y);
		if (region->point_count > start) {
			if (region->points[region->point_count - 1U].x == corner.x && region->points[region->point_count - 1U].y == corner.y)
				continue;
		}
		region->points[region->point_count] = corner;
		region->point_count++;
	}

	/* The last corner is not the first again. */
	if (region->point_count > start + 1U) {
		if (region->points[region->point_count - 1U].x == region->points[start].x &&
		    region->points[region->point_count - 1U].y == region->points[start].y)
			region->point_count--;
	}

	/* A contour of less than three corners encloses nothing. */
	if (region->point_count - start < 3U) {
		region->point_count = start;
		return 0;
	}

	/* Room for its count. */
	counts = realloc(region->contours, (region->contour_count + 1U) * sizeof(*counts));
	if (counts == NULL)
		return ENOMEM;
	region->contours = counts;

	/* Succeeded: the contour is the area's last. */
	region->contours[region->contour_count] = region->point_count - start;
	region->contour_count++;
	return 0;
}

/*
 * Follows one contour from the top edge of a pixel, the area on the right
 * (y downwards), keeping the corners where it turns.  At a corner where
 * the area touches itself only diagonally the contour turns right, which
 * keeps such pixels apart, as the flood's four neighbours do.
 */
static int
fill_follow(
	const struct fill_map *map,
	const uint32_t *box,
	unsigned char *visited,
	uint32_t x0,
	uint32_t y0,
	struct fill_contour *contour)
{
	size_t box_width;
	int64_t x;
	int64_t y;
	int64_t right_x;
	int64_t right_y;
	int64_t left_x;
	int64_t left_y;
	unsigned direction;
	unsigned next;
	int right;
	int left;
	int error;

	/* Starts east along the top edge of the pixel. */
	box_width = (size_t)(box[2] - box[0] + 1U);
	x = x0;
	y = y0;
	direction = FILL_EAST;

	/* Each edge, until the contour comes back to its first going the same way. */
	for (;;) {
		/* An edge east is a top edge of the area: no other contour starts from it. */
		if (direction == FILL_EAST)
			visited[(size_t)(y - box[1]) * box_width + (size_t)(x - box[0])] = 1U;

		/* Along the edge to the next corner, and the pixels ahead on the right and on the left. */
		switch (direction) {
		case FILL_EAST:
			x++;
			right_x = x;
			right_y = y;
			left_x = x;
			left_y = y - 1;
			break;
		case FILL_SOUTH:
			y++;
			right_x = x - 1;
			right_y = y;
			left_x = x;
			left_y = y;
			break;
		case FILL_WEST:
			x--;
			right_x = x - 1;
			right_y = y - 1;
			left_x = x - 1;
			left_y = y;
			break;
		default:
			y--;
			right_x = x;
			right_y = y - 1;
			left_x = x - 1;
			left_y = y - 1;
			break;
		}

		/* The way on: right when the area ends ahead, left when it goes on both ahead, else straight. */
		right = fill_inside(map, box, right_x, right_y);
		left = fill_inside(map, box, left_x, left_y);
		if (!right) {
			next = (direction + 1U) % 4U;
		} else if (left) {
			next = (direction + 3U) % 4U;
		} else {
			next = direction;
		}

		/* A turn is a corner of the contour. */
		if (next != direction) {
			error = fill_corner(contour, (double)x, (double)y);
			if (error != 0)
				return error;
		}
		direction = next;

		/* Back at the first edge: the contour is closed. */
		if (x == (int64_t)x0 && y == (int64_t)y0 && direction == FILL_EAST)
			break;
	}

	/* Succeeded: the contour's corners. */
	return 0;
}

/* Tells whether a pixel is in the widened area (none is outside the box). */
static int
fill_inside(
	const struct fill_map *map,
	const uint32_t *box,
	int64_t x,
	int64_t y)
{
	/* A pixel outside the box is not. */
	if (x < (int64_t)box[0] || x > (int64_t)box[2])
		return 0;
	if (y < (int64_t)box[1] || y > (int64_t)box[3])
		return 0;

	/* A pixel in the box is when it has the flag. */
	if ((map->flags[(size_t)y * map->width + (size_t)x] & FILL_FINAL) != 0U)
		return 1;

	/* It is not. */
	return 0;
}

/* Adds a corner to a contour. */
static int
fill_corner(
	struct fill_contour *contour,
	double x,
	double y)
{
	double *larger;
	size_t capacity;

	/* Grows the corners when they are full. */
	if (contour->count == contour->capacity) {
		capacity = contour->capacity * 2U;
		if (capacity < 64U)
			capacity = 64U;
		larger = realloc(contour->xy, capacity * 2U * sizeof(*larger));
		if (larger == NULL)
			return ENOMEM;
		contour->xy = larger;
		contour->capacity = capacity;
	}

	/* Succeeded: the corner is the contour's last. */
	contour->xy[contour->count * 2U] = x;
	contour->xy[contour->count * 2U + 1U] = y;
	contour->count++;
	return 0;
}

/* Measures a contour's area in square pixels: positive for the outside (clockwise on the screen), negative for a hole. */
static double
fill_area(
	const struct fill_contour *contour)
{
	double sum;
	size_t index;
	size_t next;

	/* The shoelace over every edge. */
	sum = 0.0;
	for (index = 0; index < contour->count; index++) {
		next = (index + 1U) % contour->count;
		sum += contour->xy[index * 2U] * contour->xy[next * 2U + 1U] - contour->xy[next * 2U] * contour->xy[index * 2U + 1U];
	}

	/* Reports half the sum. */
	return sum / 2.0;
}

/*
 * Simplifies a closed contour within a tolerance in pixels (Douglas-
 * Peucker): keep[i] tells whether corner i stays.  The first corner and
 * the one farthest from it always stay.
 */
static int
fill_simplify(
	const struct fill_contour *contour,
	double tolerance,
	unsigned char *keep)
{
	size_t *stack;
	size_t count;
	size_t farthest;
	size_t index;
	size_t from;
	size_t to;
	size_t worst;
	double distance;
	double largest;
	double dx;
	double dy;
	const double *a;
	const double *b;

	/* Nothing stays but what is kept below. */
	memset(keep, 0, contour->count);

	/* The corner farthest from the first. */
	farthest = 0;
	largest = -1.0;
	for (index = 1; index < contour->count; index++) {
		dx = contour->xy[index * 2U] - contour->xy[0];
		dy = contour->xy[index * 2U + 1U] - contour->xy[1];
		distance = dx * dx + dy * dy;
		if (distance > largest) {
			largest = distance;
			farthest = index;
		}
	}
	keep[0] = 1U;
	keep[farthest] = 1U;

	/* The stack of runs to look at: a run is from a kept corner to the next (the count stands for the first again). */
	stack = malloc(2U * (contour->count + 2U) * sizeof(*stack));
	if (stack == NULL)
		return ENOMEM;
	count = 0;
	stack[count++] = 0;
	stack[count++] = farthest;
	stack[count++] = farthest;
	stack[count++] = contour->count;

	/* Each run: the corner farthest from its chord stays when it is beyond the tolerance, and splits the run. */
	while (count > 0U) {
		to = stack[--count];
		from = stack[--count];
		if (to <= from + 1U)
			continue;
		a = &contour->xy[from * 2U];
		b = &contour->xy[(to % contour->count) * 2U];
		worst = from;
		largest = -1.0;
		for (index = from + 1U; index < to; index++) {
			distance = fill_segment_distance(&contour->xy[index * 2U], a, b);
			if (distance > largest) {
				largest = distance;
				worst = index;
			}
		}

		/* A run within the tolerance keeps only its ends. */
		if (largest <= tolerance)
			continue;

		/* The farthest corner stays, and both halves are looked at. */
		keep[worst] = 1U;
		stack[count++] = from;
		stack[count++] = worst;
		stack[count++] = worst;
		stack[count++] = to;
	}

	/* Succeeded: the corners that stay are marked. */
	free(stack);
	return 0;
}

/* Measures the distance from a point to a segment (all as x, y pairs). */
static double
fill_segment_distance(
	const double *point,
	const double *a,
	const double *b)
{
	double dx;
	double dy;
	double length;
	double share;
	double px;
	double py;

	/* The segment's direction and length. */
	dx = b[0] - a[0];
	dy = b[1] - a[1];
	length = dx * dx + dy * dy;

	/* A segment of no length is its end. */
	share = 0.0;
	if (length > 0.0)
		share = ((point[0] - a[0]) * dx + (point[1] - a[1]) * dy) / length;

	/* The nearest place on the segment. */
	if (share < 0.0)
		share = 0.0;
	if (share > 1.0)
		share = 1.0;
	px = a[0] + share * dx - point[0];
	py = a[1] + share * dy - point[1];

	/* Reports the distance. */
	return sqrt(px * px + py * py);
}

/* Forgets the covering and the area, keeping the rows read and their borders. */
static void
fill_unready(
	struct fill_map *map)
{
	size_t pixels;
	size_t index;
	uint32_t row;

	/* Each pixel keeps only whether it is a border. */
	pixels = (size_t)map->width * map->height;
	for (index = 0; index < pixels; index++)
		map->flags[index] &= FILL_RAW;

	/* Each row keeps only whether it was read. */
	for (row = 0; row < map->height; row++)
		map->rows[row] &= FILL_ROW_LOADED;

	/* Nothing is on the flood's lists. */
	map->stack.count = 0;
	map->waiting.count = 0;
}

/* Frees the page's tables and lists. */
static void
fill_map_free(
	struct fill_map *map)
{
	/* Each table and list. */
	free(map->flags);
	free(map->distance);
	free(map->rows);
	free(map->pixels);
	free(map->stack.spans);
	free(map->waiting.spans);
	map->flags = NULL;
	map->distance = NULL;
	map->rows = NULL;
	map->pixels = NULL;
	map->stack.spans = NULL;
	map->waiting.spans = NULL;
}
