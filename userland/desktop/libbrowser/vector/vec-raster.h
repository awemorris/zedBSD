/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The coverage rasterizer: how much of each pixel a filled shape covers,
 * as 8-bit alphas in sparse tiles.
 *
 * Definition of coverage.  The shape is the set of points whose winding
 * number about the polylines (each contour taken as closed) satisfies the
 * fill rule (SVG 2 §13.4.2): nonzero, or odd for evenodd.  Each pixel row
 * [py, py + 1) is sampled on VEC_SUBLINES horizontal lines at
 * y = py + (s + 0.5) / VEC_SUBLINES, s = 0 .. VEC_SUBLINES - 1.  On each
 * line the shape is a set of exact intervals of x (between the crossings
 * of the line with the segments, the winding number counted with the
 * direction of each crossing segment: +1 going down, -1 going up; a
 * segment holds its upper end and not its lower one, and a horizontal
 * segment crosses nothing).  The coverage of the pixel [px, px + 1) x
 * [py, py + 1) is
 *
 *   c = (1 / VEC_SUBLINES) * sum over s of length(intervals of line s
 *                                                  inside [px, px + 1))
 *
 * and its alpha is floor(255 * c + 0.5).  Computed in float, an alpha may
 * differ from that of the exact c by one step.  Overlapping or abutting
 * contours count once wherever the rule holds, so a union of pieces (a
 * stroke's outline) has no seams.
 *
 * Tiles.  The pixels are grouped in tiles of VEC_TILE_SIZE x
 * VEC_TILE_SIZE on the grid of whole multiples of VEC_TILE_SIZE (tile
 * (tx, ty) holds the pixels tx * VEC_TILE_SIZE <= px < (tx + 1) *
 * VEC_TILE_SIZE and the same in y).  A tile whose pixels all have alpha
 * 255 is full, one whose pixels all have alpha 0 is empty; the result
 * keeps the others (partial tiles) with their alphas, the full ones as
 * runs, and nothing for the empty ones.  A renderer draws a partial tile
 * as a texture and a run as a solid rectangle.
 */

#ifndef KEILAND_BROWSER_VECTOR_RASTER_H
#define KEILAND_BROWSER_VECTOR_RASTER_H

#include "vector/vec-flatten.h"

/* The side of a tile, in pixels. */
#define VEC_TILE_SIZE		16

/* The pixels of a tile. */
#define VEC_TILE_PIXELS		(VEC_TILE_SIZE * VEC_TILE_SIZE)

/* The sample lines of one pixel row. */
#define VEC_SUBLINES		16

/*
 * The largest side of the area rasterized, in pixels: the shape's bounds
 * met with the clip may be at most this wide and this tall.  A larger
 * one is refused with ENOBUFS (the caller clips to what it can show).
 */
#define VEC_RASTER_SIDE_MAX	16384

/*
 * The most segments one call takes, counted over all contours with their
 * closing segments.
 */
#define VEC_RASTER_SEGMENTS_MAX	(1UL << 21)

/*
 * The most crossings one call computes: the sum over the segments (inside
 * the clip's rows) of the sample lines each one spans, the work of the
 * scan.  2^26 is some tenths of a second on a slow core; a shape that
 * needs more (a huge number of long segments) is refused with ENOBUFS
 * before any scan, so a hostile path cannot hold the thread.
 */
#define VEC_RASTER_CROSSINGS_MAX	(1UL << 26)

/*
 * The most partial tiles one coverage holds: 65536 tiles of 256 bytes, 16
 * MiB.  A shape with more partial tiles inside the clip is refused with
 * ENOBUFS (the caller rasterizes a smaller window: the visible part).
 */
#define VEC_RASTER_TILES_MAX	65536

/* The fill rules of SVG 2 §13.4.2 (and CSS Masking 1 §6.2's clip-rule). */
enum vec_fill_rule {
	VEC_FILL_NONZERO = 0,
	VEC_FILL_EVENODD = 1
};

/*
 * One partial tile: its column and row on the tile grid, and the alphas
 * of its pixels row by row (alpha[row * VEC_TILE_SIZE + column]).
 */
struct vec_tile {
	int32_t tx;
	int32_t ty;
	unsigned char alpha[VEC_TILE_PIXELS];
};

/* A run of full tiles in one tile row: count tiles from column tx. */
struct vec_run {
	int32_t tx;
	int32_t ty;
	int32_t count;
};

/*
 * The coverage of a shape: its partial tiles and its runs of full tiles,
 * both sorted by row then column, from realloc and owned by the structure.
 * bounds is the rectangle of whole pixels that holds every pixel of
 * nonzero alpha (empty, all four 0, when there is none).  A zeroed
 * structure is a valid empty coverage.
 */
struct vec_coverage {
	struct vec_tile *tiles;
	size_t tile_count;
	size_t tile_capacity;
	struct vec_run *runs;
	size_t run_count;
	size_t run_capacity;
	struct vec_irect bounds;
};

/* Makes an empty coverage.  Input: coverage not NULL. */
void vec_coverage_init(struct vec_coverage *coverage);

/* Frees the arrays and leaves the coverage empty. */
void vec_coverage_release(struct vec_coverage *coverage);

/* Empties the coverage but keeps its arrays. */
void vec_coverage_reset(struct vec_coverage *coverage);

/*
 * Rasterizes polylines with a fill rule into out (replacing what it had).
 *
 * Every vertex is moved by (offset_x, offset_y) first: a caller keeps a
 * shape's polylines once and draws them at a pixel phase (the fractional
 * part of where it lands), so the same polylines serve every whole-pixel
 * position.  Each contour is taken as closed (a segment from its last
 * vertex to its first is implied).  Only the pixels inside clip are
 * computed and kept (NULL for no clip but the limits); pixels outside it
 * have alpha 0 whatever the shape.
 *
 * Input: lines checked here; rule one of enum vec_fill_rule; the offsets
 * finite; clip NULL or a rectangle (an empty one gives an empty result).
 * Output: on success out holds the coverage as the file comment defines.
 * Errors: EINVAL for malformed lines, an unknown rule, offsets not finite,
 * or a moved vertex beyond VEC_COORD_MAX; ENOBUFS when the shape's bounds
 * met with clip are wider or taller than VEC_RASTER_SIDE_MAX or the lines
 * have more than VEC_RASTER_SEGMENTS_MAX segments, or the crossings
 * exceed VEC_RASTER_CROSSINGS_MAX (checked before the scan), or the
 * partial tiles exceed VEC_RASTER_TILES_MAX; ENOMEM.  On error out is
 * empty.
 *
 * A clip that is not aligned to the tile grid leaves the tiles it cuts
 * partial (their pixels outside the clip at alpha 0); a caller that
 * caches tiles and later rasterizes a neighbouring window aligns its
 * clips to whole tiles so the cached tiles stay exact.
 */
int vec_rasterize(const struct vec_lines *lines, enum vec_fill_rule rule, float offset_x, float offset_y, const struct vec_irect *clip, struct vec_coverage *out);

/*
 * Reads the alpha of one pixel of a coverage (0 outside its tiles and
 * runs).  A lookup by binary search over the sorted tiles and runs.
 *
 * Returns 0 .. 255.  Never fails.
 */
unsigned vec_coverage_at(const struct vec_coverage *coverage, int32_t x, int32_t y);

/*
 * Multiplies two coverages pixel by pixel into out (replacing what it
 * had): alpha = (a * b + 127) / 255 in integers, so 255 is the identity
 * and 0 absorbs.  This is how a shape is clipped by another's coverage
 * (CSS Masking 1 §5.1, the clipping path's coverage), and how nested
 * clips meet.
 *
 * Input: a and b valid coverages; out distinct from both.
 * Errors: ENOMEM, out then empty.
 */
int vec_coverage_intersect(struct vec_coverage *out, const struct vec_coverage *a, const struct vec_coverage *b);

/*
 * Combines two coverages pixel by pixel into out by the larger alpha: the
 * coverage of the union of the parts of a clipping path whose children
 * have different clip rules (CSS Masking 1 §6.1: the clipping path is the
 * union of its children's shapes).  Where the parts overlap only partly
 * inside one pixel this underestimates, which is the accepted cost.
 *
 * Input and errors as vec_coverage_intersect.
 */
int vec_coverage_unite(struct vec_coverage *out, const struct vec_coverage *a, const struct vec_coverage *b);

#endif
