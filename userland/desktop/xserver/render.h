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
 * only inside the clip.  render-cpu.c draws into memory; an image with a
 * copy on the GPU (render-vulkan.c) is drawn there by the same operations,
 * which render-cpu.c hands to it.  region.c keeps the regions.
 * render-composite.c composites (the RENDER extension's operations:
 * pictures joined by the Porter-Duff operators and the blend modes) and
 * coverage.c turns trapezoids into alpha masks for it.
 * shape.c turns the shapes of the requests (lines, arcs, polygons) into
 * spans, the same for every drawing layer.
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

/* The cap styles of a line's ends. */
#define X11_CAP_NOT_LAST		0U
#define X11_CAP_BUTT			1U
#define X11_CAP_ROUND			2U
#define X11_CAP_PROJECTING		3U

/* The join styles where two lines of a path meet. */
#define X11_JOIN_MITER			0U
#define X11_JOIN_ROUND			1U
#define X11_JOIN_BEVEL			2U

/*
 * The compositing operators the layer names itself (the others are
 * RENDER's numbers: Porter-Duff 0 to 13, the disjoint ones 0x10 to 0x1b,
 * the conjoint ones 0x20 to 0x2b, the blend modes 0x30 to 0x3e).
 */
#define X11_OP_CLEAR			0U
#define X11_OP_SRC			1U
#define X11_OP_OVER			3U
#define X11_OP_ADD			12U

/* How a picture's image goes on outside it: nothing, tiled, its edge, mirrored. */
#define X11_REPEAT_NONE			0U
#define X11_REPEAT_NORMAL		1U
#define X11_REPEAT_PAD			2U
#define X11_REPEAT_REFLECT		3U

/* How a transformed image is sampled. */
#define X11_FILTER_NEAREST		0U
#define X11_FILTER_BILINEAR		1U
#define X11_FILTER_CONVOLUTION		2U

/* What a picture read in a composite is: an image, one colour, or a gradient. */
#define X11_SOURCE_IMAGE		0U
#define X11_SOURCE_SOLID		1U
#define X11_SOURCE_LINEAR		2U
#define X11_SOURCE_RADIAL		3U
#define X11_SOURCE_CONICAL		4U

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

/* The server's Vulkan (render-vulkan.h), and an image's copy on its device. */
struct x11_vulkan;
struct x11_gpu_image;

/*
 * The pixels of a drawable: rows of width pixels, stride pixels apart.
 * A pixel of depth 24 is 0x00RRGGBB, of depth 32 is 0xAARRGGBB with the
 * colour premultiplied by the alpha, of depth 8 an alpha from 0 to 255,
 * of depth 1 is 0 or 1.  Composited, the depth names the picture format:
 * depth 1 is a1, 8 is a8, 24 is x8r8g8b8 (the alpha is always 1) and 32
 * is a8r8g8b8.
 *
 * gpu is the image's copy on the GPU (NULL: the pixels are all there is).
 * With one, the drawing is done there and the pixels in memory may be
 * older: whoever reads them other than through these operations brings
 * them up to date first (x11_render_image_memory).
 */
struct x11_image {
	uint32_t *pixels;
	size_t stride;
	int width;
	int height;
	unsigned depth;
	struct x11_gpu_image *gpu;
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

/*
 * Spans being made: the spans (malloc'd), the box they are kept inside
 * (a span or the part of one outside it is dropped), and whether memory
 * ran out (the spans made before are kept).
 */
struct x11_spans {
	struct x11_span *spans;
	size_t count;
	size_t capacity;
	struct x11_box bounds;
	int failed;
};

/*
 * Where a dashed line is in its dashes: the lengths (the list repeated
 * twice when it has an odd count, so that even and odd dashes take
 * turns), which dash it is in (an even one is drawn, an odd one is the
 * gap or, for a double dash, drawn otherwise), and how much of that dash
 * is left (pixels along the major axis for a thin line, length for a wide
 * one).
 */
struct x11_dash {
	const uint8_t *lengths;
	size_t count;
	size_t index;
	double left;
};

/*
 * An arc: the box of its ellipse (from (x, y), width by height pixels),
 * the angle it starts at and the angle it goes through (sixty-fourths of
 * a degree, counterclockwise from three o'clock, the angles of the
 * ellipse's parameter; a negative extent goes clockwise).
 */
struct x11_arc {
	int x;
	int y;
	int width;
	int height;
	int angle1;
	int angle2;
};

/*
 * A colour of 16 bits a channel.  A solid picture's colour is
 * premultiplied by its alpha; a gradient's stops are not.
 */
struct x11_color {
	uint16_t red;
	uint16_t green;
	uint16_t blue;
	uint16_t alpha;
};

/* One stop of a gradient: where it is (16.16 fixed point, 0 to 1) and its colour. */
struct x11_gradient_stop {
	int32_t offset;
	struct x11_color color;
};

/*
 * A picture as a composite reads it (its source or its mask).
 *
 * An image is read from box, the drawable's pixels on the image, with
 * the picture's (0, 0) at the box's top left; outside the box the
 * picture repeats as repeat says.  A transform (transformed nonzero)
 * maps the place of a pixel to the place it is read from (a 3 by 3
 * matrix of 16.16 fixed point numbers, row by row), and filter says how
 * the image is sampled there (a convolution by a kernel of kernel_width
 * by kernel_height 16.16 numbers, row by row).  component_alpha makes a
 * mask's colour channels the alphas of the source's channels.
 *
 * A solid picture is color everywhere.  A linear gradient goes from
 * points[0..1] to points[2..3]; a radial one from the circle at
 * points[0..1] of radius radii[0] to the one at points[2..3] of radius
 * radii[1]; a conical one turns about points[0..1] from angle (degrees);
 * all in 16.16 fixed point, with their stops in order of offset.
 */
struct x11_source {
	unsigned kind;
	const struct x11_image *image;
	struct x11_box box;
	unsigned repeat;
	unsigned filter;
	const int32_t *kernel;
	int kernel_width;
	int kernel_height;
	int transformed;
	int32_t transform[9];
	int component_alpha;
	struct x11_color color;
	int32_t points[4];
	int32_t radii[2];
	int32_t angle;
	const struct x11_gradient_stop *stops;
	size_t stop_count;
};

/*
 * A trapezoid of RENDER: between the horizontal lines top and bottom,
 * and between the lines through left's two points and right's two
 * points (x0, y0, x1, y1), all 16.16 fixed point.
 */
struct x11_trapezoid {
	int32_t top;
	int32_t bottom;
	int32_t left[4];
	int32_t right[4];
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
int x11_region_canonical(struct x11_region *region);
void x11_region_extents(const struct x11_region *region, struct x11_box *extents);
int x11_box_intersect(const struct x11_box *a, const struct x11_box *b, struct x11_box *result);

/* render-cpu.c: drawing into an image in memory. */
void x11_render_boxes(struct x11_image *target, const struct x11_region *clip, const struct x11_paint *paint, const struct x11_box *boxes, size_t count);
void x11_render_spans(struct x11_image *target, const struct x11_region *clip, const struct x11_paint *paint, const struct x11_span *spans, size_t count);
void x11_render_mask(struct x11_image *target, const struct x11_region *clip, const struct x11_paint *paint, int x, int y, int width, int height, const uint8_t *bits, size_t stride);
int x11_render_copy(struct x11_image *target, const struct x11_region *clip, int x, int y, const struct x11_image *source, int source_x, int source_y, unsigned function, uint32_t plane_mask);
int x11_render_copy_plane(struct x11_image *target, const struct x11_region *clip, int x, int y, const struct x11_image *source, int source_x, int source_y, uint32_t plane, const struct x11_paint *paint);
void x11_render_put(struct x11_image *target, const struct x11_region *clip, int x, int y, int width, int height, const uint32_t *pixels, size_t stride, unsigned function, uint32_t plane_mask);
void x11_render_read(const struct x11_image *source, int x, int y, int width, int height, uint32_t *pixels, size_t stride);

/* render-composite.c: compositing. */
void x11_render_composite(struct x11_image *target, const struct x11_region *clip, unsigned op, const struct x11_source *source, int source_x, int source_y, const struct x11_source *mask, int mask_x, int mask_y, int x, int y, int width, int height);

/* coverage.c: alpha masks of shapes. */
void x11_coverage_trapezoid(struct x11_image *mask, int x, int y, const struct x11_trapezoid *trapezoid);

/* shape.c: shapes as spans. */
void x11_spans_init(struct x11_spans *list, const struct x11_box *bounds);
void x11_spans_fini(struct x11_spans *list);
void x11_spans_clear(struct x11_spans *list);
void x11_spans_add(struct x11_spans *list, int y, int x0, int x1);
void x11_spans_merge(struct x11_spans *list);
void x11_spans_extents(const struct x11_spans *list, struct x11_box *extents);
void x11_dash_start(struct x11_dash *dash, const uint8_t *lengths, size_t count, unsigned offset);
void x11_shape_thin_line(struct x11_spans *even, struct x11_spans *odd, struct x11_dash *dash, int x0, int y0, int x1, int y1, int last);
void x11_shape_wide_path(struct x11_spans *even, struct x11_spans *odd, struct x11_dash *dash, const int *points, size_t count, unsigned width, unsigned cap, unsigned join);
void x11_shape_polygon(struct x11_spans *list, const double *points, size_t count, int winding);
void x11_shape_arc_fill(struct x11_spans *list, const struct x11_arc *arc, int pie);
void x11_shape_arc_line(struct x11_spans *list, const struct x11_arc *arc, unsigned width);

/* render-vulkan.c: an image's copy on the GPU, and the operations drawn there. */
void x11_render_image_attach(struct x11_vulkan *vulkan, struct x11_image *image, int rows, uint32_t fill);
void x11_render_image_detach(struct x11_image *image);
void x11_render_image_memory(const struct x11_image *image);
void x11_render_image_memory_box(const struct x11_image *image, const struct x11_box *box);
void x11_render_vulkan_boxes(struct x11_image *target, const struct x11_region *clip, const struct x11_paint *paint, const struct x11_box *boxes, size_t count);
void x11_render_vulkan_spans(struct x11_image *target, const struct x11_region *clip, const struct x11_paint *paint, const struct x11_span *spans, size_t count);
void x11_render_vulkan_mask(struct x11_image *target, const struct x11_region *clip, const struct x11_paint *paint, int x, int y, int width, int height, const uint8_t *bits, size_t stride);
int x11_render_vulkan_copy(struct x11_image *target, const struct x11_region *clip, int x, int y, const struct x11_image *source, int source_x, int source_y, unsigned function, uint32_t plane_mask);
int x11_render_vulkan_copy_plane(struct x11_image *target, const struct x11_region *clip, int x, int y, const struct x11_image *source, int source_x, int source_y, uint32_t plane, const struct x11_paint *paint);
void x11_render_vulkan_put(struct x11_image *target, const struct x11_region *clip, int x, int y, int width, int height, const uint32_t *pixels, size_t stride, unsigned function, uint32_t plane_mask);
void x11_render_vulkan_read(const struct x11_image *source, int x, int y, int width, int height, uint32_t *pixels, size_t stride);
int x11_render_vulkan_composite(struct x11_image *target, const struct x11_region *clip, unsigned op, const struct x11_source *source, int source_x, int source_y, const struct x11_source *mask, int mask_x, int mask_y, int x, int y, int width, int height);

#endif
