/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The CPU canvas of the library (Files' canvas.c, moved here unchanged by
 * ws090-p002): rectangles, rounded rectangles with
 * soft edges, shadows, gradients, circles and rings, polygons, lines,
 * coverage masks (glyphs) and scaled pictures, blended over premultiplied
 * BGRA pixels.
 *
 * Rounded shapes are drawn from their signed distance: a pixel whose centre
 * is half a pixel inside the edge is fully covered, half a pixel outside not
 * at all, and the pixels between get the fraction, which is the one pixel
 * of antialiasing a quiet interface needs.  Polygons are filled by scanlines:
 * four sub-rows a pixel row, each crossing its edges exactly, so a sloped
 * edge is smooth in both directions.
 */

#include <keiland/keiland.h>

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* How many sub-rows a polygon's pixel row is sampled at. */
#define CANVAS_SUBROWS		4

/* The most edges one sub-row of a polygon crosses. */
#define CANVAS_CROSSINGS	(KL_POLYGON_POINTS + 2)

/* The corners given to one round end of a line. */
#define CANVAS_CAP_POINTS	8

/* Pi, which the ring and the round line ends need. */
#define CANVAS_PI		3.14159265358979f

/*
 * One place where a polygon's edge crosses a sub-row, and which way the edge
 * goes (up or down), which decides whether the span after it is inside.
 */
struct canvas_crossing {
	float x;
	int direction;
};

static int canvas_bounds(const struct kl_canvas *canvas, float x, float y, float width, float height, int *left, int *top, int *right, int *bottom);
static float canvas_round_distance(float px, float py, float cx, float cy, float half_width, float half_height, float radius);
static float canvas_clamp(float value);
static void canvas_blend(uint32_t *pixel, kl_color color, float coverage);
static void canvas_blend_premultiplied(uint32_t *pixel, uint32_t source);
static void canvas_span(float *row, float from, float to, float weight, int low, int high);
static int canvas_crossings(const float *points, int count, float y, struct canvas_crossing *crossings);
static uint32_t canvas_sample(const struct kl_image *image, float u, float v);
static uint32_t canvas_lerp_pixel(uint32_t first, uint32_t second, unsigned weight);
static int canvas_inner_span(float x, float y, float width, float height, float radius, float band, int py, int *from, int *to);
static void canvas_run(uint32_t *row, int from, int to, kl_color color);

/*
 * Makes a canvas over pixels the caller owns.
 *
 * Returns 0, or ENOMEM when the polygon row cannot be allocated.
 */
int
kl_canvas_init(
	struct kl_canvas *canvas,
	uint32_t *pixels,
	size_t stride,
	int width,
	int height)
{
	/* The pixels and the whole canvas as the clip. */
	memset(canvas, 0, sizeof(*canvas));
	canvas->pixels = pixels;
	canvas->stride = stride;
	canvas->width = width;
	canvas->height = height;
	canvas->clip.width = width;
	canvas->clip.height = height;

	/* The polygon filler's row, one float a pixel and one past the end. */
	canvas->coverage = calloc((size_t)width + 2U, sizeof(float));
	if (canvas->coverage == NULL)
		return ENOMEM;

	/* Succeeded: the canvas can be drawn on. */
	return 0;
}

/*
 * Releases what the canvas allocated (not the pixels).
 */
void
kl_canvas_release(
	struct kl_canvas *canvas)
{
	/* The scratch row goes; the pixels stay with their owner. */
	free(canvas->coverage);
	memset(canvas, 0, sizeof(*canvas));
}

/*
 * Narrows the clip to its intersection with a rectangle, until the
 * matching kl_canvas_clip_pop.
 */
void
kl_canvas_clip_push(
	struct kl_canvas *canvas,
	const struct kl_rect *rect)
{
	struct kl_rect clip;
	int right;
	int bottom;

	/* The intersection of the clip in force and the rectangle. */
	clip = canvas->clip;
	right = clip.x + clip.width;
	bottom = clip.y + clip.height;
	if (rect->x > clip.x)
		clip.x = rect->x;
	if (rect->y > clip.y)
		clip.y = rect->y;
	if (rect->x + rect->width < right)
		right = rect->x + rect->width;
	if (rect->y + rect->height < bottom)
		bottom = rect->y + rect->height;
	clip.width = right - clip.x;
	clip.height = bottom - clip.y;

	/* An empty intersection draws nothing, but still nests. */
	if (clip.width < 0)
		clip.width = 0;
	if (clip.height < 0)
		clip.height = 0;

	/* Keeps the clip being replaced, when there is room for it. */
	if (canvas->clip_depth < KL_CANVAS_CLIPS) {
		canvas->clips[canvas->clip_depth] = canvas->clip;
		canvas->clip_depth++;
	}

	/* The intersection is the clip from now on. */
	canvas->clip = clip;
}

/*
 * Restores the clip that the last kl_canvas_clip_push replaced.
 */
void
kl_canvas_clip_pop(
	struct kl_canvas *canvas)
{
	/* An unmatched pop leaves the clip as it is. */
	if (canvas->clip_depth == 0)
		return;

	/* The clip before the push. */
	canvas->clip_depth--;
	canvas->clip = canvas->clips[canvas->clip_depth];
}

/*
 * Makes the canvas clear within the clip: every pixel transparent (the
 * whole canvas when no clip is pushed; a frame drawn again only in a part,
 * BUG-226, clears that part).
 */
void
kl_canvas_clear(
	struct kl_canvas *canvas)
{
	uint32_t *row;
	int left;
	int top;
	int right;
	int bottom;
	int y;

	/* The clip, kept inside the canvas. */
	left = canvas->clip.x;
	if (left < 0)
		left = 0;
	top = canvas->clip.y;
	if (top < 0)
		top = 0;
	right = canvas->clip.x + canvas->clip.width;
	if (right > canvas->width)
		right = canvas->width;
	bottom = canvas->clip.y + canvas->clip.height;
	if (bottom > canvas->height)
		bottom = canvas->height;
	if (right <= left || bottom <= top)
		return;

	/* Each row of it, its pixels zero (transparent black, premultiplied). */
	for (y = top; y < bottom; y++) {
		row = canvas->pixels + (size_t)y * canvas->stride + (size_t)left;
		memset(row, 0, sizeof(row[0]) * (size_t)(right - left));
	}
}

/*
 * Fills a rectangle with a color.
 */
void
kl_canvas_fill(
	struct kl_canvas *canvas,
	const struct kl_rect *rect,
	kl_color color)
{
	uint32_t *row;
	uint32_t source;
	unsigned alpha;
	unsigned red;
	unsigned green;
	unsigned blue;
	int inside;
	int left;
	int top;
	int right;
	int bottom;
	int x;
	int y;

	/* The part of the rectangle inside the clip. */
	inside = canvas_bounds(canvas, (float)rect->x, (float)rect->y, (float)rect->width, (float)rect->height, &left, &top, &right, &bottom);
	if (inside == 0)
		return;

	/* A transparent color leaves every pixel as it is. */
	alpha = (color >> 24) & 0xffU;
	if (alpha == 0U)
		return;

	/* An opaque color replaces every pixel (as canvas_blend does pixel by pixel). */
	if (alpha >= 255U) {
		source = 0xff000000U | (color & 0xffffffU);
		for (y = top; y < bottom; y++) {
			row = canvas->pixels + (size_t)y * canvas->stride;
			for (x = left; x < right; x++)
				row[x] = source;
		}

		/* Every pixel replaced. */
		return;
	}

	/*
	 * Otherwise the color is premultiplied once and laid over every pixel,
	 * as canvas_blend does at full coverage (BUG-226: the panels' fills were
	 * most of a frame, the color worked out again for each pixel).
	 */
	red = ((color >> 16) & 0xffU) * alpha / 255U;
	green = ((color >> 8) & 0xffU) * alpha / 255U;
	blue = (color & 0xffU) * alpha / 255U;
	source = (alpha << 24) | (red << 16) | (green << 8) | blue;
	for (y = top; y < bottom; y++) {
		row = canvas->pixels + (size_t)y * canvas->stride;
		for (x = left; x < right; x++)
			canvas_blend_premultiplied(&row[x], source);
	}
}

/*
 * Fills a rectangle with a vertical gradient from the top color to the
 * bottom one.
 */
void
kl_canvas_gradient(
	struct kl_canvas *canvas,
	const struct kl_rect *rect,
	kl_color top_color,
	kl_color bottom_color)
{
	uint32_t *row;
	kl_color color;
	float amount;
	int inside;
	int left;
	int top;
	int right;
	int bottom;
	int x;
	int y;

	/* The part of the rectangle inside the clip. */
	inside = canvas_bounds(canvas, (float)rect->x, (float)rect->y, (float)rect->width, (float)rect->height, &left, &top, &right, &bottom);
	if (inside == 0)
		return;

	/* Each row in the color of its place between the top and the bottom. */
	for (y = top; y < bottom; y++) {
		amount = 0.0f;
		if (rect->height > 1)
			amount = (float)(y - rect->y) / (float)(rect->height - 1);
		color = kl_color_mix(top_color, bottom_color, amount);
		row = canvas->pixels + (size_t)y * canvas->stride;
		for (x = left; x < right; x++)
			canvas_blend(&row[x], color, 1.0f);
	}
}

/*
 * Fills a rounded rectangle with a color, its edges antialiased.
 */
void
kl_canvas_round(
	struct kl_canvas *canvas,
	float x,
	float y,
	float width,
	float height,
	float radius,
	kl_color color)
{
	/* A single color is a gradient between the same two. */
	kl_canvas_round_gradient(canvas, x, y, width, height, radius, color, color);
}

/*
 * Fills a rounded rectangle with a vertical gradient, its edges
 * antialiased.
 */
void
kl_canvas_round_gradient(
	struct kl_canvas *canvas,
	float x,
	float y,
	float width,
	float height,
	float radius,
	kl_color top_color,
	kl_color bottom_color)
{
	uint32_t *row;
	kl_color color;
	float half_width;
	float half_height;
	float cx;
	float cy;
	float distance;
	float coverage;
	float amount;
	int inside;
	int left;
	int top;
	int right;
	int bottom;
	int from;
	int to;
	int px;
	int py;

	/* The pixels the shape may touch, inside the clip. */
	inside = canvas_bounds(canvas, x, y, width, height, &left, &top, &right, &bottom);
	if (inside == 0)
		return;

	/* The shape's centre and half sizes, and a radius no larger than half the short side. */
	half_width = width * 0.5f;
	half_height = height * 0.5f;
	cx = x + half_width;
	cy = y + half_height;
	if (radius > half_width)
		radius = half_width;
	if (radius > half_height)
		radius = half_height;

	/* Each row in its color, each pixel as much as the shape covers it. */
	for (py = top; py < bottom; py++) {
		amount = 0.0f;
		if (height > 1.0f)
			amount = ((float)py + 0.5f - y) / height;
		color = kl_color_mix(top_color, bottom_color, amount);
		row = canvas->pixels + (size_t)py * canvas->stride;

		/* The part of the row wholly inside is filled at once; only the edges are measured. */
		(void)canvas_inner_span(x, y, width, height, radius, 0.0f, py, &from, &to);
		if (from < left)
			from = left;
		if (to > right)
			to = right;
		if (to < from)
			to = from;
		canvas_run(row, from, to, color);
		for (px = left; px < right; px++) {
			/* The filled part is skipped. */
			if (px == from)
				px = to;
			if (px >= right)
				break;

			/* An edge pixel, as much as the shape covers it. */
			distance = canvas_round_distance((float)px + 0.5f, (float)py + 0.5f, cx, cy, half_width, half_height, radius);
			coverage = canvas_clamp(0.5f - distance);
			canvas_blend(&row[px], color, coverage);
		}
	}
}

/*
 * Draws the outline of a rounded rectangle, a thickness inside its edge.
 */
void
kl_canvas_round_border(
	struct kl_canvas *canvas,
	float x,
	float y,
	float width,
	float height,
	float radius,
	float thickness,
	kl_color color)
{
	uint32_t *row;
	float half_width;
	float half_height;
	float cx;
	float cy;
	float distance;
	float coverage;
	int inside;
	int left;
	int top;
	int right;
	int bottom;
	int from;
	int to;
	int px;
	int py;

	/* The pixels the outline may touch, inside the clip. */
	inside = canvas_bounds(canvas, x, y, width, height, &left, &top, &right, &bottom);
	if (inside == 0)
		return;

	/* The shape's centre and half sizes, and its radius. */
	half_width = width * 0.5f;
	half_height = height * 0.5f;
	cx = x + half_width;
	cy = y + half_height;
	if (radius > half_width)
		radius = half_width;
	if (radius > half_height)
		radius = half_height;

	/* A pixel is covered as far as it is inside the edge and not deeper than the thickness. */
	for (py = top; py < bottom; py++) {
		row = canvas->pixels + (size_t)py * canvas->stride;
		(void)canvas_inner_span(x, y, width, height, radius, thickness + 0.5f, py, &from, &to);
		for (px = left; px < right; px++) {
			/* The inside, deeper than the thickness, is not touched. */
			if (px == from && to > from)
				px = to;
			if (px >= right)
				break;

			/* A pixel near the edge. */
			distance = canvas_round_distance((float)px + 0.5f, (float)py + 0.5f, cx, cy, half_width, half_height, radius);
			coverage = canvas_clamp(0.5f - distance) - canvas_clamp(0.5f - (distance + thickness));
			canvas_blend(&row[px], color, coverage);
		}
	}
}

/*
 * Draws the soft shadow of a rounded rectangle: the color fades out over
 * the softness on both sides of the edge.
 */
void
kl_canvas_shadow(
	struct kl_canvas *canvas,
	float x,
	float y,
	float width,
	float height,
	float radius,
	float softness,
	kl_color color)
{
	uint32_t *row;
	float half_width;
	float half_height;
	float cx;
	float cy;
	float distance;
	float amount;
	int inside;
	int left;
	int top;
	int right;
	int bottom;
	int from;
	int to;
	int px;
	int py;

	/* A shadow without softness is the shape itself. */
	if (softness < 1.0f)
		softness = 1.0f;

	/* The pixels the shadow may reach, a softness around the shape. */
	inside = canvas_bounds(canvas, x - softness, y - softness, width + 2.0f * softness, height + 2.0f * softness, &left, &top, &right, &bottom);
	if (inside == 0)
		return;

	/* The shape's centre, half sizes and radius. */
	half_width = width * 0.5f;
	half_height = height * 0.5f;
	cx = x + half_width;
	cy = y + half_height;
	if (radius > half_width)
		radius = half_width;
	if (radius > half_height)
		radius = half_height;

	/* A smooth step from full inside to nothing a softness outside. */
	for (py = top; py < bottom; py++) {
		row = canvas->pixels + (size_t)py * canvas->stride;

		/* Deeper inside than the softness the shadow is the full color, filled at once. */
		(void)canvas_inner_span(x, y, width, height, radius, softness, py, &from, &to);
		if (from < left)
			from = left;
		if (to > right)
			to = right;
		if (to < from)
			to = from;
		canvas_run(row, from, to, color);
		for (px = left; px < right; px++) {
			/* The filled part is skipped. */
			if (px == from)
				px = to;
			if (px >= right)
				break;

			/* A pixel of the soft edge. */
			distance = canvas_round_distance((float)px + 0.5f, (float)py + 0.5f, cx, cy, half_width, half_height, radius);
			amount = canvas_clamp((distance + softness) / (2.0f * softness));
			amount = 1.0f - amount * amount * (3.0f - 2.0f * amount);
			canvas_blend(&row[px], color, amount);
		}
	}
}

/*
 * Fills a circle.
 */
void
kl_canvas_circle(
	struct kl_canvas *canvas,
	float cx,
	float cy,
	float radius,
	kl_color color)
{
	/* A circle is a square whose radius is half its side. */
	kl_canvas_round(canvas, cx - radius, cy - radius, 2.0f * radius, 2.0f * radius, radius, color);
}

/*
 * Draws part of a ring, clockwise from twelve o'clock: a fraction of 1
 * draws all of it (a progress indicator).
 */
void
kl_canvas_ring(
	struct kl_canvas *canvas,
	float cx,
	float cy,
	float radius,
	float thickness,
	float fraction,
	kl_color color)
{
	uint32_t *row;
	float dx;
	float dy;
	float distance;
	float angle;
	float limit;
	float coverage;
	int inside;
	int left;
	int top;
	int right;
	int bottom;
	int px;
	int py;

	/* The pixels the ring may touch, inside the clip. */
	inside = canvas_bounds(canvas, cx - radius, cy - radius, 2.0f * radius, 2.0f * radius, &left, &top, &right, &bottom);
	if (inside == 0)
		return;

	/* The angle the ring stops at. */
	limit = fraction * 2.0f * CANVAS_PI;

	/* A pixel is covered between the two circles and before the angle. */
	for (py = top; py < bottom; py++) {
		row = canvas->pixels + (size_t)py * canvas->stride;
		for (px = left; px < right; px++) {
			/* The distance from the centre decides the band. */
			dx = (float)px + 0.5f - cx;
			dy = (float)py + 0.5f - cy;
			distance = sqrtf(dx * dx + dy * dy);
			coverage = canvas_clamp(0.5f - (distance - radius)) - canvas_clamp(0.5f - (distance - (radius - thickness)));
			if (coverage <= 0.0f)
				continue;

			/* The angle from twelve o'clock, clockwise, decides the arc. */
			angle = atan2f(dx, -dy);
			if (angle < 0.0f)
				angle += 2.0f * CANVAS_PI;
			if (angle > limit)
				continue;

			/* The pixel belongs to the drawn part of the ring. */
			canvas_blend(&row[px], color, coverage);
		}
	}
}

/*
 * Fills a polygon (x, y pairs; the nonzero rule), its edges antialiased.
 */
void
kl_canvas_polygon(
	struct kl_canvas *canvas,
	const float *points,
	int count,
	kl_color color)
{
	struct canvas_crossing crossings[CANVAS_CROSSINGS];
	uint32_t *row;
	float coverage;
	float min_x;
	float min_y;
	float max_x;
	float max_y;
	float sub_y;
	int crossing_count;
	int winding;
	int inside;
	int left;
	int top;
	int right;
	int bottom;
	int index;
	int sub;
	int px;
	int py;

	/* A polygon has three corners at least and no more than the filler keeps. */
	if (count < 3 || count > KL_POLYGON_POINTS)
		return;

	/* The polygon's bounding box. */
	min_x = points[0];
	max_x = points[0];
	min_y = points[1];
	max_y = points[1];
	for (index = 1; index < count; index++) {
		if (points[2 * index] < min_x)
			min_x = points[2 * index];
		if (points[2 * index] > max_x)
			max_x = points[2 * index];
		if (points[2 * index + 1] < min_y)
			min_y = points[2 * index + 1];
		if (points[2 * index + 1] > max_y)
			max_y = points[2 * index + 1];
	}

	/* The pixels the polygon may touch, inside the clip. */
	inside = canvas_bounds(canvas, min_x, min_y, max_x - min_x, max_y - min_y, &left, &top, &right, &bottom);
	if (inside == 0)
		return;

	/* Each pixel row: the coverage of its sub-rows' inside spans, then the blend. */
	for (py = top; py < bottom; py++) {
		memset(canvas->coverage + left, 0, (size_t)(right - left + 1) * sizeof(float));
		for (sub = 0; sub < CANVAS_SUBROWS; sub++) {
			/* Where the sub-row crosses the edges, left to right. */
			sub_y = (float)py + ((float)sub + 0.5f) / (float)CANVAS_SUBROWS;
			crossing_count = canvas_crossings(points, count, sub_y, crossings);

			/* The spans after which the winding is not zero are inside. */
			winding = 0;
			for (index = 0; index + 1 < crossing_count; index++) {
				winding += crossings[index].direction;
				if (winding != 0) {
					canvas_span(canvas->coverage, crossings[index].x, crossings[index + 1].x, 1.0f / (float)CANVAS_SUBROWS, left, right);
				}
			}
		}

		/* The row's pixels, as much as the spans covered them. */
		row = canvas->pixels + (size_t)py * canvas->stride;
		for (px = left; px < right; px++) {
			coverage = canvas_clamp(canvas->coverage[px]);
			canvas_blend(&row[px], color, coverage);
		}
	}
}

/*
 * Draws a line of a thickness with round ends.
 */
void
kl_canvas_line(
	struct kl_canvas *canvas,
	float x0,
	float y0,
	float x1,
	float y1,
	float thickness,
	kl_color color)
{
	float points[2 * (2 * CANVAS_CAP_POINTS + 2)];
	float length;
	float nx;
	float ny;
	float angle;
	float base;
	float half;
	int count;
	int index;

	/* The line's direction, and the half thickness across it. */
	nx = x1 - x0;
	ny = y1 - y0;
	length = sqrtf(nx * nx + ny * ny);
	half = thickness * 0.5f;

	/* A point is a dot. */
	if (length < 0.001f) {
		kl_canvas_circle(canvas, x0, y0, half, color);
		return;
	}

	/* The angle of the line, from which the round ends are laid out. */
	base = atan2f(ny, nx);

	/* The end at (x1, y1): a half circle from one side of the line round to the other. */
	count = 0;
	for (index = 0; index <= CANVAS_CAP_POINTS; index++) {
		angle = base - CANVAS_PI * 0.5f + CANVAS_PI * (float)index / (float)CANVAS_CAP_POINTS;
		points[2 * count] = x1 + half * cosf(angle);
		points[2 * count + 1] = y1 + half * sinf(angle);
		count++;
	}

	/* The end at (x0, y0), the other half circle, which closes the outline. */
	for (index = 0; index <= CANVAS_CAP_POINTS; index++) {
		angle = base + CANVAS_PI * 0.5f + CANVAS_PI * (float)index / (float)CANVAS_CAP_POINTS;
		points[2 * count] = x0 + half * cosf(angle);
		points[2 * count + 1] = y0 + half * sinf(angle);
		count++;
	}

	/* The outline is filled as one polygon, so a translucent line is even. */
	kl_canvas_polygon(canvas, points, count, color);
}

/*
 * Blends a color through a coverage mask (a glyph): 255 is the full color.
 */
void
kl_canvas_mask(
	struct kl_canvas *canvas,
	int x,
	int y,
	const uint8_t *mask,
	int width,
	int height,
	size_t stride,
	kl_color color)
{
	const uint8_t *source;
	uint32_t *row;
	int inside;
	int left;
	int top;
	int right;
	int bottom;
	int px;
	int py;

	/* The part of the mask inside the clip. */
	inside = canvas_bounds(canvas, (float)x, (float)y, (float)width, (float)height, &left, &top, &right, &bottom);
	if (inside == 0)
		return;

	/* Each covered pixel, as much as the mask says. */
	for (py = top; py < bottom; py++) {
		row = canvas->pixels + (size_t)py * canvas->stride;
		source = mask + (size_t)(py - y) * stride;
		for (px = left; px < right; px++) {
			if (source[px - x] != 0U)
				canvas_blend(&row[px], color, (float)source[px - x] / 255.0f);
		}
	}
}

/*
 * Draws a picture scaled into a rectangle (bilinear), with rounded corners
 * and an opacity.
 */
void
kl_canvas_image(
	struct kl_canvas *canvas,
	const struct kl_image *image,
	float x,
	float y,
	float width,
	float height,
	float radius,
	float opacity)
{
	uint32_t *row;
	uint32_t sample;
	float half_width;
	float half_height;
	float cx;
	float cy;
	float distance;
	float coverage;
	float u;
	float v;
	unsigned alpha;
	unsigned red;
	unsigned green;
	unsigned blue;
	unsigned weight;
	int inside;
	int left;
	int top;
	int right;
	int bottom;
	int px;
	int py;

	/* An empty picture or rectangle draws nothing. */
	if (image->pixels == NULL ||
	    image->width <= 0 ||
	    image->height <= 0 ||
	    width <= 0.0f ||
	    height <= 0.0f)
		return;

	/* The pixels of the rectangle inside the clip. */
	inside = canvas_bounds(canvas, x, y, width, height, &left, &top, &right, &bottom);
	if (inside == 0)
		return;

	/* The rectangle's centre and half sizes, for the rounded corners. */
	half_width = width * 0.5f;
	half_height = height * 0.5f;
	cx = x + half_width;
	cy = y + half_height;
	if (radius > half_width)
		radius = half_width;
	if (radius > half_height)
		radius = half_height;

	/* Each pixel: the picture sampled at its place, faded by the corner and the opacity. */
	for (py = top; py < bottom; py++) {
		row = canvas->pixels + (size_t)py * canvas->stride;
		v = ((float)py + 0.5f - y) * (float)image->height / height - 0.5f;
		for (px = left; px < right; px++) {
			/* How much of the pixel the rounded rectangle covers. */
			distance = canvas_round_distance((float)px + 0.5f, (float)py + 0.5f, cx, cy, half_width, half_height, radius);
			coverage = canvas_clamp(0.5f - distance) * opacity;
			if (coverage <= 0.0f)
				continue;

			/* The picture's color there, weighted by that coverage (it is premultiplied). */
			u = ((float)px + 0.5f - x) * (float)image->width / width - 0.5f;
			sample = canvas_sample(image, u, v);
			weight = (unsigned)(coverage * 256.0f);
			if (weight > 256U)
				weight = 256U;
			alpha = (((sample >> 24) & 0xffU) * weight) >> 8;
			red = (((sample >> 16) & 0xffU) * weight) >> 8;
			green = (((sample >> 8) & 0xffU) * weight) >> 8;
			blue = ((sample & 0xffU) * weight) >> 8;
			canvas_blend_premultiplied(&row[px], (alpha << 24) | (red << 16) | (green << 8) | blue);
		}
	}
}

/*
 * Allocates a transparent picture of a size.
 *
 * Returns 0, EINVAL for an empty size, or ENOMEM.
 */
int
kl_image_create(
	struct kl_image *image,
	int width,
	int height)
{
	/* A picture has at least one pixel. */
	memset(image, 0, sizeof(*image));
	if (width <= 0 || height <= 0)
		return EINVAL;

	/* The pixels, cleared to transparent. */
	image->pixels = calloc((size_t)width * (size_t)height, sizeof(uint32_t));
	if (image->pixels == NULL)
		return ENOMEM;

	/* Succeeded: the picture's size and its row length. */
	image->width = width;
	image->height = height;
	image->stride = (size_t)width;
	return 0;
}

/*
 * Frees a picture's pixels.
 */
void
kl_image_release(
	struct kl_image *image)
{
	/* The pixels go and the picture is empty. */
	free(image->pixels);
	memset(image, 0, sizeof(*image));
}

/*
 * Scales a picture into another of the target's size: each target pixel is
 * the average of the source pixels under it when shrinking, and a bilinear
 * sample when growing.
 */
void
kl_image_scale(
	const struct kl_image *source,
	struct kl_image *target)
{
	const uint32_t *row;
	uint32_t pixel;
	unsigned long sums[4];
	unsigned long count;
	int x0;
	int x1;
	int y0;
	int y1;
	int tx;
	int ty;
	int sx;
	int sy;

	/* An empty picture on either side scales nothing. */
	if (source->pixels == NULL || target->pixels == NULL)
		return;

	/* Each target pixel covers a box of the source. */
	for (ty = 0; ty < target->height; ty++) {
		y0 = (int)((long)ty * source->height / target->height);
		y1 = (int)((long)(ty + 1) * source->height / target->height);
		if (y1 <= y0)
			y1 = y0 + 1;
		for (tx = 0; tx < target->width; tx++) {
			x0 = (int)((long)tx * source->width / target->width);
			x1 = (int)((long)(tx + 1) * source->width / target->width);
			if (x1 <= x0)
				x1 = x0 + 1;

			/* A box of one source pixel is growing: a bilinear sample is smoother. */
			if (x1 - x0 == 1 && y1 - y0 == 1 && (target->width > source->width || target->height > source->height)) {
				target->pixels[(size_t)ty * target->stride + (size_t)tx] = canvas_sample(source,
				    ((float)tx + 0.5f) * (float)source->width / (float)target->width - 0.5f,
				    ((float)ty + 0.5f) * (float)source->height / (float)target->height - 0.5f);
				continue;
			}

			/* The average of the box, channel by channel. */
			memset(sums, 0, sizeof(sums));
			count = 0;
			for (sy = y0; sy < y1 && sy < source->height; sy++) {
				row = source->pixels + (size_t)sy * source->stride;
				for (sx = x0; sx < x1 && sx < source->width; sx++) {
					pixel = row[sx];
					sums[0] += (pixel >> 24) & 0xffU;
					sums[1] += (pixel >> 16) & 0xffU;
					sums[2] += (pixel >> 8) & 0xffU;
					sums[3] += pixel & 0xffU;
					count++;
				}
			}

			/* The averaged pixel, when the box held any. */
			if (count == 0)
				continue;
			pixel = (uint32_t)((sums[0] / count) << 24);
			pixel |= (uint32_t)((sums[1] / count) << 16);
			pixel |= (uint32_t)((sums[2] / count) << 8);
			pixel |= (uint32_t)(sums[3] / count);
			target->pixels[(size_t)ty * target->stride + (size_t)tx] = pixel;
		}
	}
}

/*
 * Mixes two colors (alpha included): 0 is the first, 1 the second.
 */
kl_color
kl_color_mix(
	kl_color from,
	kl_color to,
	float amount)
{
	unsigned weight;
	uint32_t mixed;
	int shift;
	unsigned first;
	unsigned second;

	/* The weight of the second color, from 0 to 256. */
	if (amount <= 0.0f)
		return from;
	if (amount >= 1.0f)
		return to;
	weight = (unsigned)(amount * 256.0f);

	/* Each channel moves that far from the first to the second. */
	mixed = 0;
	for (shift = 0; shift < 32; shift += 8) {
		first = (from >> shift) & 0xffU;
		second = (to >> shift) & 0xffU;
		mixed |= (uint32_t)(((first * (256U - weight) + second * weight) >> 8) & 0xffU) << shift;
	}

	/* Reports the mixed color. */
	return mixed;
}

/*
 * Finds the pixels of a row that are inside a rounded rectangle by more
 * than a band (so wholly covered by it); returns zero, with an empty range,
 * when there are none.  The corners' rows keep out of the whole corner
 * square, which is simple and costs a few pixels measured one by one.
 */
static int
canvas_inner_span(
	float x,
	float y,
	float width,
	float height,
	float radius,
	float band,
	int py,
	int *from,
	int *to)
{
	float centre;
	float depth;
	float margin;

	/* How far the row's centre is from the nearer of the top and bottom edges. */
	centre = (float)py + 0.5f;
	depth = centre - y;
	if (y + height - centre < depth)
		depth = y + height - centre;

	/* A row near the top or the bottom has nothing that deep inside. */
	*from = 0;
	*to = 0;
	if (depth <= band + 0.5f)
		return 0;

	/* In the corners' rows the corner squares stay out; below them only the sides do. */
	margin = band + 0.5f;
	if (depth < radius + band + 0.5f)
		margin = radius + band + 0.5f;

	/* The pixels whose centres are that far from both sides. */
	*from = (int)ceilf(x + margin - 0.5f);
	*to = (int)floorf(x + width - margin - 0.5f) + 1;
	if (*to <= *from) {
		*to = *from;
		return 0;
	}

	/* Some pixels are that deep inside. */
	return 1;
}

/* Fills pixels of a row wholly with a color: stored when opaque, blended otherwise. */
static void
canvas_run(
	uint32_t *row,
	int from,
	int to,
	kl_color color)
{
	uint32_t premultiplied;
	unsigned alpha;
	int px;

	/* An opaque color is stored. */
	alpha = (color >> 24) & 0xffU;
	if (alpha == 255U) {
		for (px = from; px < to; px++)
			row[px] = color;
		return;
	}

	/* A transparent one draws nothing. */
	if (alpha == 0U)
		return;

	/* Another is premultiplied once and laid over each pixel. */
	premultiplied = alpha << 24;
	premultiplied |= (((color >> 16) & 0xffU) * alpha / 255U) << 16;
	premultiplied |= (((color >> 8) & 0xffU) * alpha / 255U) << 8;
	premultiplied |= (color & 0xffU) * alpha / 255U;
	for (px = from; px < to; px++)
		canvas_blend_premultiplied(&row[px], premultiplied);
}

/* Clips a rectangle of the canvas to the clip and to whole pixels; zero when nothing is left. */
static int
canvas_bounds(
	const struct kl_canvas *canvas,
	float x,
	float y,
	float width,
	float height,
	int *left,
	int *top,
	int *right,
	int *bottom)
{
	/* The whole pixels the rectangle touches. */
	*left = (int)floorf(x);
	*top = (int)floorf(y);
	*right = (int)ceilf(x + width);
	*bottom = (int)ceilf(y + height);

	/* Kept inside the clip. */
	if (*left < canvas->clip.x)
		*left = canvas->clip.x;
	if (*top < canvas->clip.y)
		*top = canvas->clip.y;
	if (*right > canvas->clip.x + canvas->clip.width)
		*right = canvas->clip.x + canvas->clip.width;
	if (*bottom > canvas->clip.y + canvas->clip.height)
		*bottom = canvas->clip.y + canvas->clip.height;

	/* Nothing left to draw. */
	if (*left >= *right || *top >= *bottom)
		return 0;

	/* Some pixels are left. */
	return 1;
}

/* Reports how far a point is outside a rounded rectangle (negative inside). */
static float
canvas_round_distance(
	float px,
	float py,
	float cx,
	float cy,
	float half_width,
	float half_height,
	float radius)
{
	float qx;
	float qy;
	float outside_x;
	float outside_y;
	float inside;

	/* The point folded into one quarter, relative to the corner circle's centre. */
	qx = fabsf(px - cx) - (half_width - radius);
	qy = fabsf(py - cy) - (half_height - radius);

	/* Beyond the corner circle's centre in both directions: the distance to the circle. */
	if (qx > 0.0f && qy > 0.0f) {
		inside = sqrtf(qx * qx + qy * qy);
		return inside - radius;
	}

	/* Otherwise the distance to the nearer straight edge. */
	outside_x = qx;
	outside_y = qy;
	inside = outside_x;
	if (outside_y > inside)
		inside = outside_y;

	/* Reports that distance, the corner radius taken off. */
	return inside - radius;
}

/* Keeps a coverage between 0 and 1. */
static float
canvas_clamp(
	float value)
{
	/* Below nothing and above everything. */
	if (value < 0.0f)
		return 0.0f;
	if (value > 1.0f)
		return 1.0f;

	/* Reports the value as it is. */
	return value;
}

/* Blends a color over a pixel, as much as the coverage says. */
static void
canvas_blend(
	uint32_t *pixel,
	kl_color color,
	float coverage)
{
	unsigned alpha;
	unsigned red;
	unsigned green;
	unsigned blue;

	/* The color's alpha, scaled by the coverage. */
	alpha = (unsigned)((float)((color >> 24) & 0xffU) * coverage + 0.5f);
	if (alpha == 0U)
		return;

	/* An opaque color replaces the pixel. */
	if (alpha >= 255U) {
		*pixel = 0xff000000U | (color & 0xffffffU);
		return;
	}

	/* Otherwise the premultiplied color is laid over it. */
	red = ((color >> 16) & 0xffU) * alpha / 255U;
	green = ((color >> 8) & 0xffU) * alpha / 255U;
	blue = (color & 0xffU) * alpha / 255U;
	canvas_blend_premultiplied(pixel, (alpha << 24) | (red << 16) | (green << 8) | blue);
}

/*
 * Lays a premultiplied color over a pixel.
 *
 * Each channel is the source's plus the pixel's scaled by what the source
 * leaves, (pixel * remaining + 127) / 255, at most 255.  Two channels are
 * worked out at once, each in a 16-bit lane of one word (red and blue,
 * then alpha and green), and the division by 255 is the exact
 * (t + (t >> 8) + 1) >> 8 of t = pixel * remaining + 127 (BUG-226: this is
 * most of the time of a frame's translucent panels).
 */
static void
canvas_blend_premultiplied(
	uint32_t *pixel,
	uint32_t source)
{
	uint32_t destination;
	uint32_t red_blue;
	uint32_t alpha_green;
	uint32_t overflow;
	uint32_t remaining;

	/* What the source leaves of the pixel under it. */
	remaining = 255U - ((source >> 24) & 0xffU);
	destination = *pixel;

	/* Red and blue of the pixel, scaled and rounded, in their lanes. */
	red_blue = (destination & 0x00ff00ffU) * remaining + 0x007f007fU;
	red_blue = ((red_blue + ((red_blue >> 8) & 0x00ff00ffU) + 0x00010001U) >> 8) & 0x00ff00ffU;

	/* Alpha and green, the same way. */
	alpha_green = ((destination >> 8) & 0x00ff00ffU) * remaining + 0x007f007fU;
	alpha_green = ((alpha_green + ((alpha_green >> 8) & 0x00ff00ffU) + 0x00010001U) >> 8) & 0x00ff00ffU;

	/* The source's channels added (a lane reaches at most 510, so none carries into the next). */
	red_blue += source & 0x00ff00ffU;
	alpha_green += (source >> 8) & 0x00ff00ffU;

	/* A lane past 255 is 255. */
	overflow = ((red_blue & 0x01000100U) >> 8) * 0xffU;
	red_blue = (red_blue | overflow) & 0x00ff00ffU;
	overflow = ((alpha_green & 0x01000100U) >> 8) * 0xffU;
	alpha_green = (alpha_green | overflow) & 0x00ff00ffU;

	/* The pixel takes the blended color. */
	*pixel = red_blue | (alpha_green << 8);
}

/* Adds a weight to the coverage of a span of a row, the partly covered end pixels in part. */
static void
canvas_span(
	float *row,
	float from,
	float to,
	float weight,
	int low,
	int high)
{
	int first;
	int last;
	int x;

	/* The span, kept inside the row's pixels. */
	if (from < (float)low)
		from = (float)low;
	if (to > (float)high)
		to = (float)high;
	if (to <= from)
		return;

	/* The whole pixels the span touches. */
	first = (int)floorf(from);
	last = (int)floorf(to);

	/* A span inside one pixel covers its width of it. */
	if (first == last) {
		row[first] += (to - from) * weight;
		return;
	}

	/* The first pixel in part, the ones between in full, the last in part. */
	row[first] += ((float)(first + 1) - from) * weight;
	for (x = first + 1; x < last; x++)
		row[x] += weight;
	row[last] += (to - (float)last) * weight;
}

/* Finds where a sub-row crosses a polygon's edges, sorted from left to right, and returns how many. */
static int
canvas_crossings(
	const float *points,
	int count,
	float y,
	struct canvas_crossing *crossings)
{
	struct canvas_crossing moving;
	float x0;
	float y0;
	float x1;
	float y1;
	int crossing_count;
	int index;
	int next;
	int place;

	/* Each edge that spans the sub-row (its lower end included, its upper not). */
	crossing_count = 0;
	for (index = 0; index < count; index++) {
		next = index + 1;
		if (next == count)
			next = 0;
		x0 = points[2 * index];
		y0 = points[2 * index + 1];
		x1 = points[2 * next];
		y1 = points[2 * next + 1];

		/* A level edge crosses no sub-row. */
		if (y0 == y1)
			continue;

		/* An edge going down counts +1, going up -1. */
		if (y0 < y1) {
			if (y < y0 || y >= y1)
				continue;
			crossings[crossing_count].direction = 1;
		} else {
			if (y < y1 || y >= y0)
				continue;
			crossings[crossing_count].direction = -1;
		}

		/* Where along the edge the sub-row is. */
		crossings[crossing_count].x = x0 + (y - y0) * (x1 - x0) / (y1 - y0);
		crossing_count++;
		if (crossing_count == CANVAS_CROSSINGS)
			break;
	}

	/* Sorted by x (insertion: there are few). */
	for (index = 1; index < crossing_count; index++) {
		moving = crossings[index];
		place = index;
		while (place > 0 && crossings[place - 1].x > moving.x) {
			crossings[place] = crossings[place - 1];
			place--;
		}

		/* The crossing goes into the gap it opened. */
		crossings[place] = moving;
	}

	/* Reports how many crossings there are. */
	return crossing_count;
}

/* Samples a picture between its pixels (bilinear), its edges repeated outward. */
static uint32_t
canvas_sample(
	const struct kl_image *image,
	float u,
	float v)
{
	uint32_t top;
	uint32_t bottom;
	unsigned weight_x;
	unsigned weight_y;
	int x0;
	int y0;
	int x1;
	int y1;

	/* The four pixels around the point, kept inside the picture. */
	x0 = (int)floorf(u);
	y0 = (int)floorf(v);
	weight_x = (unsigned)((u - (float)x0) * 256.0f);
	weight_y = (unsigned)((v - (float)y0) * 256.0f);
	x1 = x0 + 1;
	y1 = y0 + 1;
	if (x0 < 0)
		x0 = 0;
	if (y0 < 0)
		y0 = 0;
	if (x1 >= image->width)
		x1 = image->width - 1;
	if (y1 >= image->height)
		y1 = image->height - 1;
	if (x0 >= image->width)
		x0 = image->width - 1;
	if (y0 >= image->height)
		y0 = image->height - 1;

	/* Across each row, then down between the two rows. */
	top = canvas_lerp_pixel(image->pixels[(size_t)y0 * image->stride + (size_t)x0], image->pixels[(size_t)y0 * image->stride + (size_t)x1], weight_x);
	bottom = canvas_lerp_pixel(image->pixels[(size_t)y1 * image->stride + (size_t)x0], image->pixels[(size_t)y1 * image->stride + (size_t)x1], weight_x);

	/* Reports the blend of the two rows. */
	top = canvas_lerp_pixel(top, bottom, weight_y);
	return top;
}

/* Blends two premultiplied pixels: weight 0 is the first, 256 the second. */
static uint32_t
canvas_lerp_pixel(
	uint32_t first,
	uint32_t second,
	unsigned weight)
{
	uint32_t blended;
	int shift;

	/* The weight beyond the ends is the end. */
	if (weight > 256U)
		weight = 256U;

	/* Each channel moves that far. */
	blended = 0;
	for (shift = 0; shift < 32; shift += 8) {
		blended |= (uint32_t)(((((first >> shift) & 0xffU) * (256U - weight) +
		    ((second >> shift) & 0xffU) * weight) >> 8) & 0xffU) << shift;
	}

	/* Succeeded: the pixel between the two. */
	return blended;
}
