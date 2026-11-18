/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The canvas: the pictures, the colours and the drawing, part of <keiland/keiland.h>.
 */

#ifndef KEILAND_CANVAS_H
#define KEILAND_CANVAS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* How deep the clip rectangles nest. */
#define KL_CANVAS_CLIPS		16

/* The most corners a polygon may have. */
#define KL_POLYGON_POINTS	96

/* A color as 0xAARRGGBB, not premultiplied. */
typedef uint32_t kl_color;

/* An opaque color from 0xRRGGBB. */
#define KL_RGB(value)		((kl_color)(0xff000000U | (uint32_t)(value)))

/* A color from 0xRRGGBB and an alpha from 0 to 255. */
#define KL_RGBA(value, alpha)	((kl_color)(((uint32_t)(alpha) << 24) | ((uint32_t)(value) & 0xffffffU)))

/*
 * A rectangle of whole pixels.
 *
 * It is a plain value: a layout computes one, the drawing and the hit test
 * both read it.
 */
struct kl_rect {
	int x;
	int y;
	int width;
	int height;
};

/*
 * A picture in memory, premultiplied BGRA (0xAARRGGBB words).
 *
 * The pixels belong to whoever made the image: kl_image_create allocates
 * them and kl_image_release frees them.
 */
struct kl_image {
	uint32_t *pixels;
	int width;
	int height;
	size_t stride;
};

/*
 * A surface to draw on.
 *
 * The pixels are the caller's; the canvas adds the clip rectangles and the
 * scratch row the polygon filler accumulates coverage in, which live as
 * long as the canvas.
 */
struct kl_canvas {
	/* The pixels, the words in a row and the size. */
	uint32_t *pixels;
	size_t stride;
	int width;
	int height;

	/* The clip in force, and the ones it replaced (innermost last). */
	struct kl_rect clip;
	struct kl_rect clips[KL_CANVAS_CLIPS];
	int clip_depth;

	/* One row of polygon coverage, a float per pixel and one more. */
	float *coverage;
};

/* The canvas (canvas.c). */
int kl_canvas_init(struct kl_canvas *canvas, uint32_t *pixels, size_t stride, int width, int height);
void kl_canvas_release(struct kl_canvas *canvas);
void kl_canvas_clip_push(struct kl_canvas *canvas, const struct kl_rect *rect);
void kl_canvas_clip_pop(struct kl_canvas *canvas);
void kl_canvas_clear(struct kl_canvas *canvas);
void kl_canvas_fill(struct kl_canvas *canvas, const struct kl_rect *rect, kl_color color);
void kl_canvas_gradient(struct kl_canvas *canvas, const struct kl_rect *rect, kl_color top, kl_color bottom);
void kl_canvas_round(struct kl_canvas *canvas, float x, float y, float width, float height, float radius, kl_color color);
void kl_canvas_round_gradient(struct kl_canvas *canvas, float x, float y, float width, float height, float radius, kl_color top, kl_color bottom);
void kl_canvas_round_border(struct kl_canvas *canvas, float x, float y, float width, float height, float radius, float thickness, kl_color color);
void kl_canvas_shadow(struct kl_canvas *canvas, float x, float y, float width, float height, float radius, float softness, kl_color color);
void kl_canvas_circle(struct kl_canvas *canvas, float cx, float cy, float radius, kl_color color);
void kl_canvas_ring(struct kl_canvas *canvas, float cx, float cy, float radius, float thickness, float fraction, kl_color color);
void kl_canvas_polygon(struct kl_canvas *canvas, const float *points, int count, kl_color color);
void kl_canvas_line(struct kl_canvas *canvas, float x0, float y0, float x1, float y1, float thickness, kl_color color);
void kl_canvas_mask(struct kl_canvas *canvas, int x, int y, const uint8_t *mask, int width, int height, size_t stride, kl_color color);
void kl_canvas_image(struct kl_canvas *canvas, const struct kl_image *image, float x, float y, float width, float height, float radius, float opacity);
int kl_image_create(struct kl_image *image, int width, int height);
void kl_image_release(struct kl_image *image);
void kl_image_scale(const struct kl_image *source, struct kl_image *target);
kl_color kl_color_mix(kl_color from, kl_color to, float amount);

#ifdef __cplusplus
}
#endif

#endif
