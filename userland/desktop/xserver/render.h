/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * xserver's drawing layer: the pixels of the drawables (pixmaps and the
 * top-level windows) and the operations the core's requests are turned
 * into, and the regions (sets of boxes) that clip them.
 *
 * A request's handler resolves the graphics context and the drawable's
 * clip into one of these operations; the layer draws it by the X rules
 * (the graphics context's function and plane mask, the fill style) and
 * only inside the clip.  render-cpu.c draws into memory; a GPU drawing
 * layer takes the same operations.  region.c keeps the regions.
 *
 * Pure: no system header, no state outside the values passed.
 */

#ifndef KEILAND_X11SERVER_RENDER_H
#define KEILAND_X11SERVER_RENDER_H

#include <stddef.h>
#include <stdint.h>

/* The fill styles of a graphics context. */
#define X11_FILL_SOLID			0U
#define X11_FILL_TILED			1U
#define X11_FILL_STIPPLED		2U
#define X11_FILL_OPAQUE_STIPPLED	3U

/* The graphics context's functions the layer names itself (the other fourteen are their numbers). */
#define X11_FUNCTION_CLEAR		0U
#define X11_FUNCTION_COPY		3U
#define X11_FUNCTION_SET		15U

/*
 * One box: the pixels from (x0, y0) up to but not including (x1, y1).  A
 * box is empty when x0 >= x1 or y0 >= y1.
 */
struct x11_box {
	int x0;
	int y0;
	int x1;
	int y1;
};

/*
 * A region: boxes that do not overlap, in no particular order unless
 * x11_region_sort put them in one.  The boxes are the region's own
 * (malloc'd); a region made with x11_region_init holds none and ends with
 * x11_region_fini.
 */
struct x11_region {
	struct x11_box *boxes;
	size_t count;
	size_t capacity;
};

/*
 * One row of pixels to fill: y, and x0 up to but not including x1.
 */
struct x11_span {
	int y;
	int x0;
	int x1;
};

/*
 * The pixels of a drawable: rows of width pixels, stride pixels apart.
 * A pixel of depth 24 is 0x00RRGGBB, of depth 1 is 0 or 1.
 */
struct x11_image {
	uint32_t *pixels;
	size_t stride;
	int width;
	int height;
	unsigned depth;
};

/*
 * How a fill paints a pixel: the source it takes (the foreground; a tile
 * pixel; the foreground where the stipple is set and nothing or the
 * background where it is not), the tile's and stipple's origin on the
 * target image, and the function and plane mask that join the source
 * with the target's pixel.
 */
struct x11_paint {
	unsigned function;
	uint32_t plane_mask;
	uint32_t foreground;
	uint32_t background;
	unsigned fill_style;
	const struct x11_image *tile;
	const struct x11_image *stipple;
	int origin_x;
	int origin_y;
};

/* region.c: regions. */
void x11_region_init(struct x11_region *region);
void x11_region_fini(struct x11_region *region);
void x11_region_clear(struct x11_region *region);
int x11_region_set(struct x11_region *region, const struct x11_box *box);
int x11_region_copy(struct x11_region *target, const struct x11_region *source);
int x11_region_append_box(struct x11_region *region, const struct x11_box *box);
int x11_region_union_box(struct x11_region *region, const struct x11_box *box);
int x11_region_union(struct x11_region *region, const struct x11_region *other);
int x11_region_subtract_box(struct x11_region *region, const struct x11_box *box);
int x11_region_subtract(struct x11_region *region, const struct x11_region *other);
void x11_region_intersect_box(struct x11_region *region, const struct x11_box *box);
int x11_region_intersect(struct x11_region *region, const struct x11_region *other);
void x11_region_translate(struct x11_region *region, int dx, int dy);
void x11_region_sort(struct x11_region *region);
void x11_region_extents(const struct x11_region *region, struct x11_box *extents);
int x11_box_intersect(const struct x11_box *a, const struct x11_box *b, struct x11_box *result);

/* render-cpu.c: drawing into an image in memory. */
void x11_render_boxes(struct x11_image *target, const struct x11_region *clip, const struct x11_paint *paint, const struct x11_box *boxes, size_t count);
void x11_render_spans(struct x11_image *target, const struct x11_region *clip, const struct x11_paint *paint, const struct x11_span *spans, size_t count);
void x11_render_mask(struct x11_image *target, const struct x11_region *clip, const struct x11_paint *paint, int x, int y, int width, int height, const uint8_t *bits, size_t stride);
int x11_render_copy(struct x11_image *target, const struct x11_region *clip, int x, int y, const struct x11_image *source, int source_x, int source_y, unsigned function, uint32_t plane_mask);
void x11_render_put(struct x11_image *target, const struct x11_region *clip, int x, int y, int width, int height, const uint32_t *pixels, size_t stride, unsigned function, uint32_t plane_mask);
void x11_render_read(const struct x11_image *source, int x, int y, int width, int height, uint32_t *pixels, size_t stride);

#endif
