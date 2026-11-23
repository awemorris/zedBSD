/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Compositing in memory: the RENDER extension's Composite drawn pixel by
 * pixel into an image.
 *
 * A composite reads its source (and its mask, when it has one) at the
 * place of each target pixel, joins the source with the mask, and joins
 * the result with the target's pixel by an operator: the Porter-Duff
 * ones, their disjoint and conjoint forms, and the blend modes.  The
 * work is done a row at a time on pixels of 8 bits a channel, the colour
 * premultiplied by the alpha (a8r8g8b8); each picture's pixels are taken
 * to that form as they are read and the result back to the target's
 * depth as it is stored.
 */

#include "userland/desktop/xserver/render.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The places of a pixel's channels in a8r8g8b8. */
#define COMPOSITE_ALPHA_SHIFT		24U
#define COMPOSITE_RED_SHIFT		16U
#define COMPOSITE_GREEN_SHIFT		8U
#define COMPOSITE_BLUE_SHIFT		0U

/* One in 16.16 fixed point, and the smallest step of it. */
#define COMPOSITE_FIXED_ONE		65536
#define COMPOSITE_FIXED_EPSILON		1

/* Half a turn in radians (math.h's M_PI is not in every C library). */
#define COMPOSITE_PI			3.14159265358979323846

/* The bits of a bilinear sample's weights, and the weights' full scale after they are widened to 8 bits. */
#define COMPOSITE_BILINEAR_BITS		7U
#define COMPOSITE_BILINEAR_SCALE	65536U

/* The first and last operators of each family. */
#define COMPOSITE_OP_SATURATE		13U
#define COMPOSITE_OP_DISJOINT		0x10U
#define COMPOSITE_OP_DISJOINT_LAST	0x1bU
#define COMPOSITE_OP_CONJOINT		0x20U
#define COMPOSITE_OP_CONJOINT_LAST	0x2bU
#define COMPOSITE_OP_BLEND		0x30U
#define COMPOSITE_OP_BLEND_LAST		0x3eU

/* The blend modes, from the first. */
#define BLEND_MULTIPLY			0U
#define BLEND_SCREEN			1U
#define BLEND_OVERLAY			2U
#define BLEND_DARKEN			3U
#define BLEND_LIGHTEN			4U
#define BLEND_COLOR_DODGE		5U
#define BLEND_COLOR_BURN		6U
#define BLEND_HARD_LIGHT		7U
#define BLEND_SOFT_LIGHT		8U
#define BLEND_DIFFERENCE		9U
#define BLEND_EXCLUSION			10U
#define BLEND_HSL_HUE			11U
#define BLEND_HSL_SATURATION		12U
#define BLEND_HSL_COLOR			13U
#define BLEND_HSL_LUMINOSITY		14U

/* The Porter-Duff operators the joins name (the base of the disjoint and conjoint families too). */
#define PD_CLEAR			0U
#define PD_SRC				1U
#define PD_DST				2U
#define PD_OVER				3U
#define PD_OVER_REVERSE			4U
#define PD_IN				5U
#define PD_IN_REVERSE			6U
#define PD_OUT				7U
#define PD_OUT_REVERSE			8U
#define PD_ATOP				9U
#define PD_ATOP_REVERSE			10U
#define PD_XOR				11U
#define PD_ADD				12U

/*
 * The rows of one composite being worked: the source's pixels, the mask's
 * (unused without a mask) and the target's, a row's worth each.
 */
struct composite_rows {
	uint32_t *source;
	uint32_t *mask;
	uint32_t *target;
};

static void composite_box(struct x11_image *target, unsigned op, const struct x11_source *source, int source_dx, int source_dy, const struct x11_source *mask, int mask_dx, int mask_dy, const struct x11_box *box, struct composite_rows *rows);
static void composite_fetch(const struct x11_source *source, int x, int y, size_t count, uint32_t *pixels);
static void composite_fetch_image(const struct x11_source *source, int x, int y, size_t count, uint32_t *pixels);
static void composite_fetch_transformed(const struct x11_source *source, int x, int y, size_t count, uint32_t *pixels);
static void composite_fetch_gradient(const struct x11_source *source, int x, int y, size_t count, uint32_t *pixels);
static int composite_place(const struct x11_source *source, int x, int y, double *place_x, double *place_y);
static int composite_transform_fixed(const struct x11_source *source, int64_t x, int64_t y, int64_t *place_x, int64_t *place_y);
static uint32_t composite_sample_nearest(const struct x11_source *source, int64_t x, int64_t y);
static uint32_t composite_sample_bilinear(const struct x11_source *source, int64_t x, int64_t y);
static uint32_t composite_sample_convolution(const struct x11_source *source, int64_t x, int64_t y);
static uint32_t composite_pixel_at(const struct x11_source *source, int x, int y);
static int composite_repeat(unsigned repeat, int value, int size, int *place);
static uint32_t composite_read(const struct x11_image *image, int x, int y);
static void composite_store(struct x11_image *target, int x, int y, uint32_t pixel);
static uint32_t composite_solid(const struct x11_color *color);
static int composite_gradient_t(const struct x11_source *source, double x, double y, double *t);
static int composite_gradient_repeat(unsigned repeat, double t, double *place);
static uint32_t composite_gradient_color(const struct x11_source *source, double t);
static void composite_join(unsigned op, int component, const uint32_t *source, const uint32_t *mask, uint32_t *target, size_t count);
static uint32_t composite_porter_duff(unsigned op, uint32_t source, uint32_t target);
static uint32_t composite_porter_duff_ca(unsigned op, uint32_t source, uint32_t mask, uint32_t target);
static uint32_t composite_factored(unsigned op, int component, uint32_t source, uint32_t mask, uint32_t target);
static double composite_factor(unsigned family, unsigned base, int is_source, double source_alpha, double target_alpha);
static uint32_t composite_blend(unsigned mode, int component, uint32_t source, uint32_t mask, uint32_t target);
static double composite_blend_channel(unsigned mode, double source, double target);
static void composite_blend_hsl(unsigned mode, const double *source, const double *target, double *result);
static double composite_luminosity(const double *color);
static void composite_set_luminosity(const double *color, double luminosity, double *result);
static double composite_saturation(const double *color);
static void composite_set_saturation(const double *color, double saturation, double *result);
static unsigned composite_channel(uint32_t pixel, unsigned shift);
static uint32_t composite_pack(unsigned alpha, unsigned red, unsigned green, unsigned blue);
static unsigned composite_multiply(unsigned a, unsigned b);
static unsigned composite_multiply_add(unsigned x, unsigned a, unsigned y, unsigned b);
static unsigned composite_add(unsigned a, unsigned b);
static unsigned composite_divide(unsigned a, unsigned b);
static uint32_t composite_mask_pixel(uint32_t source, unsigned alpha);
static unsigned composite_round(double value);

/*
 * Composites a rectangle of a target image (x, y, width by height, on the
 * image) from a source and a mask (NULL: none) by an operator, inside a
 * clip.  The target's (x, y) reads the source at (source_x, source_y) and
 * the mask at (mask_x, mask_y), in the pictures' own coordinates.
 */
void
x11_render_composite(
	struct x11_image *target,
	const struct x11_region *clip,
	unsigned op,
	const struct x11_source *source,
	int source_x,
	int source_y,
	const struct x11_source *mask,
	int mask_x,
	int mask_y,
	int x,
	int y,
	int width,
	int height)
{
	struct composite_rows rows;
	struct x11_box rectangle;
	struct x11_box bounds;
	struct x11_box part;
	size_t columns;
	size_t index;
	int inside;

	/* The rectangle on the target, inside the image. */
	rectangle.x0 = x;
	rectangle.y0 = y;
	rectangle.x1 = x + width;
	rectangle.y1 = y + height;
	bounds.x0 = 0;
	bounds.y0 = 0;
	bounds.x1 = target->width;
	bounds.y1 = target->height;
	inside = x11_box_intersect(&rectangle, &bounds, &rectangle);
	if (!inside)
		return;

	/* The rows of the widest part (nothing is drawn without memory for them). */
	columns = (size_t)(rectangle.x1 - rectangle.x0);
	rows.source = malloc(columns * 3U * sizeof(uint32_t));
	if (rows.source == NULL)
		return;
	rows.mask = rows.source + columns;
	rows.target = rows.mask + columns;

	/* Each box of the clip, where it meets the rectangle. */
	for (index = 0U; index < clip->count; index++) {
		inside = x11_box_intersect(&clip->boxes[index], &rectangle, &part);
		if (!inside)
			continue;
		composite_box(target, op, source, source_x - x, source_y - y, mask, mask_x - x, mask_y - y, &part, &rows);
	}

	/* The rows go. */
	free(rows.source);
}

/*
 * Composites one box of the target: each row's source, mask and target
 * read, joined and stored.  The pictures are read at the target's place
 * moved by their offsets.
 */
static void
composite_box(
	struct x11_image *target,
	unsigned op,
	const struct x11_source *source,
	int source_dx,
	int source_dy,
	const struct x11_source *mask,
	int mask_dx,
	int mask_dy,
	const struct x11_box *box,
	struct composite_rows *rows)
{
	const uint32_t *mask_row;
	size_t count;
	size_t column;
	int component;
	int row;

	/* The mask's colour channels count only when it asks for them. */
	component = 0;
	if (mask != NULL && mask->component_alpha)
		component = 1;

	/* Each row: read, joined, stored. */
	count = (size_t)(box->x1 - box->x0);
	for (row = box->y0; row < box->y1; row++) {
		composite_fetch(source, box->x0 + source_dx, row + source_dy, count, rows->source);
		mask_row = NULL;
		if (mask != NULL) {
			composite_fetch(mask, box->x0 + mask_dx, row + mask_dy, count, rows->mask);
			mask_row = rows->mask;
		}
		for (column = 0U; column < count; column++)
			rows->target[column] = composite_read(target, box->x0 + (int)column, row);
		composite_join(op, component, rows->source, mask_row, rows->target, count);
		for (column = 0U; column < count; column++)
			composite_store(target, box->x0 + (int)column, row, rows->target[column]);
	}
}

/*
 * Reads a row of a picture: count pixels from (x, y) rightwards, in its
 * own coordinates, as a8r8g8b8.
 */
static void
composite_fetch(
	const struct x11_source *source,
	int x,
	int y,
	size_t count,
	uint32_t *pixels)
{
	uint32_t pixel;
	size_t index;

	/* By what the picture is. */
	switch (source->kind) {
	case X11_SOURCE_SOLID:
		/* One colour throughout. */
		pixel = composite_solid(&source->color);
		for (index = 0U; index < count; index++)
			pixels[index] = pixel;
		break;
	case X11_SOURCE_IMAGE:
		/* Pixels of an image, as they are or through a transform. */
		if (source->transformed) {
			composite_fetch_transformed(source, x, y, count, pixels);
		} else {
			composite_fetch_image(source, x, y, count, pixels);
		}
		break;
	default:
		/* A gradient. */
		composite_fetch_gradient(source, x, y, count, pixels);
		break;
	}
}

/* Reads a row of an image's pixels as they are, outside the image by its repeat. */
static void
composite_fetch_image(
	const struct x11_source *source,
	int x,
	int y,
	size_t count,
	uint32_t *pixels)
{
	size_t index;

	/* Each pixel in its place. */
	for (index = 0U; index < count; index++)
		pixels[index] = composite_pixel_at(source, x + (int)index, y);
}

/*
 * Reads a row of an image through its transform: each pixel's centre
 * taken through the matrix to the place it is read from, sampled by the
 * picture's filter.
 */
static void
composite_fetch_transformed(
	const struct x11_source *source,
	int x,
	int y,
	size_t count,
	uint32_t *pixels)
{
	int64_t centre_x;
	int64_t centre_y;
	int64_t place_x;
	int64_t place_y;
	size_t index;
	int seen;

	/* Each pixel's centre, in 16.16 fixed point. */
	centre_y = (int64_t)y * COMPOSITE_FIXED_ONE + COMPOSITE_FIXED_ONE / 2;
	for (index = 0U; index < count; index++) {
		centre_x = ((int64_t)x + (int64_t)index) * COMPOSITE_FIXED_ONE + COMPOSITE_FIXED_ONE / 2;
		seen = composite_transform_fixed(source, centre_x, centre_y, &place_x, &place_y);
		if (!seen) {
			/* A point the transform sends to infinity reads as nothing. */
			pixels[index] = 0U;
			continue;
		}

		/* Sampled by the filter. */
		if (source->filter == X11_FILTER_BILINEAR) {
			pixels[index] = composite_sample_bilinear(source, place_x, place_y);
		} else if (source->filter == X11_FILTER_CONVOLUTION) {
			pixels[index] = composite_sample_convolution(source, place_x, place_y);
		} else {
			pixels[index] = composite_sample_nearest(source, place_x, place_y);
		}
	}
}

/*
 * Takes a point (16.16 fixed point) through a picture's transform.
 * Returns 1 with the place, or 0 when the transform sends it to infinity.
 */
static int
composite_transform_fixed(
	const struct x11_source *source,
	int64_t x,
	int64_t y,
	int64_t *place_x,
	int64_t *place_y)
{
	const int32_t *matrix;
	int64_t mapped_x;
	int64_t mapped_y;
	int64_t mapped_w;

	/* The three rows of the matrix by the point (x, y, 1). */
	matrix = source->transform;
	mapped_x = ((int64_t)matrix[0] * x + (int64_t)matrix[1] * y) / COMPOSITE_FIXED_ONE + (int64_t)matrix[2];
	mapped_y = ((int64_t)matrix[3] * x + (int64_t)matrix[4] * y) / COMPOSITE_FIXED_ONE + (int64_t)matrix[5];
	mapped_w = ((int64_t)matrix[6] * x + (int64_t)matrix[7] * y) / COMPOSITE_FIXED_ONE + (int64_t)matrix[8];

	/* A projective point at infinity. */
	if (mapped_w == 0)
		return 0;

	/* Succeeded: the point divided by its weight (an affine matrix has weight one). */
	if (mapped_w == COMPOSITE_FIXED_ONE) {
		*place_x = mapped_x;
		*place_y = mapped_y;
		return 1;
	}
	*place_x = mapped_x * COMPOSITE_FIXED_ONE / mapped_w;
	*place_y = mapped_y * COMPOSITE_FIXED_ONE / mapped_w;
	return 1;
}

/*
 * Samples an image at a place (16.16) by its nearest pixel: the one whose
 * square holds the place (a place on an edge goes to the pixel before).
 */
static uint32_t
composite_sample_nearest(
	const struct x11_source *source,
	int64_t x,
	int64_t y)
{
	int64_t column;
	int64_t row;

	/* The pixel whose square holds the place. */
	column = x - COMPOSITE_FIXED_EPSILON;
	row = y - COMPOSITE_FIXED_EPSILON;
	if (column < 0)
		column -= COMPOSITE_FIXED_ONE - 1;
	if (row < 0)
		row -= COMPOSITE_FIXED_ONE - 1;

	/* Its pixel. */
	return composite_pixel_at(source, (int)(column / COMPOSITE_FIXED_ONE), (int)(row / COMPOSITE_FIXED_ONE));
}

/*
 * Samples an image at a place (16.16) between its four nearest pixel
 * centres, weighted by the distance to each in sevenths of 128 widened to
 * 8 bits, each channel's weighted sum cut to 8 bits.
 *
 * Procedure referenced: pixman 0.44.0 pixman/pixman-inlines.h
 * (bilinear_interpolation, the weights of BILINEAR_INTERPOLATION_BITS).
 */
static uint32_t
composite_sample_bilinear(
	const struct x11_source *source,
	int64_t x,
	int64_t y)
{
	uint32_t corners[4];
	uint32_t weights[4];
	uint64_t sum;
	int64_t left;
	int64_t top;
	unsigned distance_x;
	unsigned distance_y;
	unsigned shift;
	unsigned corner;
	unsigned channels;

	/* The pixel centres to the top left of the place, and how far past them it is. */
	x -= COMPOSITE_FIXED_ONE / 2;
	y -= COMPOSITE_FIXED_ONE / 2;
	left = x;
	top = y;
	if (left < 0)
		left -= COMPOSITE_FIXED_ONE - 1;
	if (top < 0)
		top -= COMPOSITE_FIXED_ONE - 1;
	left /= COMPOSITE_FIXED_ONE;
	top /= COMPOSITE_FIXED_ONE;
	distance_x = (unsigned)((x >> (16U - COMPOSITE_BILINEAR_BITS)) & ((1 << COMPOSITE_BILINEAR_BITS) - 1));
	distance_y = (unsigned)((y >> (16U - COMPOSITE_BILINEAR_BITS)) & ((1 << COMPOSITE_BILINEAR_BITS) - 1));
	distance_x <<= 8U - COMPOSITE_BILINEAR_BITS;
	distance_y <<= 8U - COMPOSITE_BILINEAR_BITS;

	/* The four pixels and their weights (which add up to the full scale). */
	corners[0] = composite_pixel_at(source, (int)left, (int)top);
	corners[1] = composite_pixel_at(source, (int)left + 1, (int)top);
	corners[2] = composite_pixel_at(source, (int)left, (int)top + 1);
	corners[3] = composite_pixel_at(source, (int)left + 1, (int)top + 1);
	weights[3] = distance_x * distance_y;
	weights[1] = (distance_x << 8U) - weights[3];
	weights[2] = (distance_y << 8U) - weights[3];
	weights[0] = COMPOSITE_BILINEAR_SCALE - (distance_y << 8U) - (distance_x << 8U) + weights[3];

	/* Each channel: the weighted sum, cut to 8 bits. */
	channels = 0U;
	for (shift = 0U; shift < 32U; shift += 8U) {
		sum = 0U;
		for (corner = 0U; corner < 4U; corner++)
			sum += (uint64_t)composite_channel(corners[corner], shift) * weights[corner];
		channels |= (unsigned)(sum / COMPOSITE_BILINEAR_SCALE) << shift;
	}

	/* The sample. */
	return channels;
}

/*
 * Samples an image at a place (16.16) by a convolution: the kernel laid
 * centred on the place over the pixels, each pixel weighted by its cell,
 * each channel's sum rounded and kept between 0 and 255.
 */
static uint32_t
composite_sample_convolution(
	const struct x11_source *source,
	int64_t x,
	int64_t y)
{
	int64_t sums[4];
	int64_t left;
	int64_t top;
	uint32_t pixel;
	uint32_t result;
	int32_t weight;
	unsigned channel;
	int64_t value;
	int column;
	int row;

	/* The pixel under the kernel's top left cell. */
	left = x - COMPOSITE_FIXED_EPSILON - (((int64_t)source->kernel_width * COMPOSITE_FIXED_ONE - COMPOSITE_FIXED_ONE) >> 1);
	top = y - COMPOSITE_FIXED_EPSILON - (((int64_t)source->kernel_height * COMPOSITE_FIXED_ONE - COMPOSITE_FIXED_ONE) >> 1);
	if (left < 0)
		left -= COMPOSITE_FIXED_ONE - 1;
	if (top < 0)
		top -= COMPOSITE_FIXED_ONE - 1;
	left /= COMPOSITE_FIXED_ONE;
	top /= COMPOSITE_FIXED_ONE;

	/* Each cell of the kernel by its pixel. */
	memset(sums, 0, sizeof(sums));
	for (row = 0; row < source->kernel_height; row++) {
		for (column = 0; column < source->kernel_width; column++) {
			weight = source->kernel[row * source->kernel_width + column];
			if (weight == 0)
				continue;
			pixel = composite_pixel_at(source, (int)left + column, (int)top + row);
			for (channel = 0U; channel < 4U; channel++)
				sums[channel] += (int64_t)composite_channel(pixel, channel * 8U) * weight;
		}
	}

	/* Each channel rounded and kept in range. */
	result = 0U;
	for (channel = 0U; channel < 4U; channel++) {
		value = (sums[channel] + COMPOSITE_FIXED_ONE / 2) / COMPOSITE_FIXED_ONE;
		if (value < 0)
			value = 0;
		if (value > 255)
			value = 255;
		result |= (uint32_t)value << (channel * 8U);
	}

	/* The sample. */
	return result;
}

/*
 * Reads a picture's pixel at a place of its own (whole pixels), outside
 * its image by its repeat (nothing when it does not repeat).
 */
static uint32_t
composite_pixel_at(
	const struct x11_source *source,
	int x,
	int y)
{
	int width;
	int height;
	int inside_x;
	int inside_y;
	int seen;

	/* The place inside the picture's box. */
	width = source->box.x1 - source->box.x0;
	height = source->box.y1 - source->box.y0;
	seen = composite_repeat(source->repeat, x, width, &inside_x);
	if (!seen)
		return 0U;
	seen = composite_repeat(source->repeat, y, height, &inside_y);
	if (!seen)
		return 0U;

	/* The pixel there. */
	return composite_read(source->image, source->box.x0 + inside_x, source->box.y0 + inside_y);
}

/*
 * Brings a place along one side of a picture inside it by a repeat.
 * Returns 1 with the place, or 0 when the place is outside a picture
 * that does not repeat (or the picture is empty).
 */
static int
composite_repeat(
	unsigned repeat,
	int value,
	int size,
	int *place)
{
	int period;

	/* An empty picture has nothing anywhere. */
	if (size <= 0)
		return 0;

	/* By the repeat. */
	switch (repeat) {
	case X11_REPEAT_NORMAL:
		/* Tiled. */
		value %= size;
		if (value < 0)
			value += size;
		break;
	case X11_REPEAT_PAD:
		/* The nearest edge pixel. */
		if (value < 0)
			value = 0;
		if (value >= size)
			value = size - 1;
		break;
	case X11_REPEAT_REFLECT:
		/* Mirrored at each edge: a period of twice the size. */
		period = size * 2;
		value %= period;
		if (value < 0)
			value += period;
		if (value >= size)
			value = period - 1 - value;
		break;
	default:
		/* Nothing outside. */
		if (value < 0 || value >= size)
			return 0;
		break;
	}

	/* Succeeded: inside. */
	*place = value;
	return 1;
}

/* Reads an image's pixel as a8r8g8b8 by its depth (nothing outside the image). */
static uint32_t
composite_read(
	const struct x11_image *image,
	int x,
	int y)
{
	uint32_t pixel;

	/* Nothing outside the image. */
	if (x < 0 || y < 0 || x >= image->width || y >= image->height)
		return 0U;
	pixel = image->pixels[(size_t)y * image->stride + (size_t)x];

	/* By the depth's format. */
	switch (image->depth) {
	case 1U:
		/* a1: no colour, the alpha all or nothing. */
		if ((pixel & 1U) != 0U)
			return 0xff000000U;
		return 0U;
	case 8U:
		/* a8: the alpha alone. */
		return (pixel & 0xffU) << COMPOSITE_ALPHA_SHIFT;
	case 32U:
		/* a8r8g8b8 as it is. */
		return pixel;
	default:
		/* x8r8g8b8: opaque. */
		return pixel | 0xff000000U;
	}
}

/* Stores an a8r8g8b8 pixel into an image by its depth. */
static void
composite_store(
	struct x11_image *target,
	int x,
	int y,
	uint32_t pixel)
{
	uint32_t *place;

	/* The pixel's place. */
	place = &target->pixels[(size_t)y * target->stride + (size_t)x];

	/* By the depth's format. */
	switch (target->depth) {
	case 1U:
		/* a1: the alpha's top bit. */
		*place = pixel >> 31U;
		break;
	case 8U:
		/* a8: the alpha. */
		*place = pixel >> COMPOSITE_ALPHA_SHIFT;
		break;
	case 32U:
		/* a8r8g8b8 as it is. */
		*place = pixel;
		break;
	default:
		/* x8r8g8b8: the colour (the alpha is not kept). */
		*place = pixel & 0x00ffffffU;
		break;
	}
}

/* Gives a premultiplied colour of 16 bits a channel as a8r8g8b8 (each channel's top 8 bits). */
static uint32_t
composite_solid(
	const struct x11_color *color)
{
	unsigned alpha;
	unsigned red;
	unsigned green;
	unsigned blue;

	/* The top byte of each. */
	alpha = (unsigned)color->alpha >> 8U;
	red = (unsigned)color->red >> 8U;
	green = (unsigned)color->green >> 8U;
	blue = (unsigned)color->blue >> 8U;
	return composite_pack(alpha, red, green, blue);
}

/*
 * Reads a row of a gradient: each pixel's centre (taken through the
 * transform when there is one) placed along the gradient, its colour
 * between the stops there.
 */
static void
composite_fetch_gradient(
	const struct x11_source *source,
	int x,
	int y,
	size_t count,
	uint32_t *pixels)
{
	double place_x;
	double place_y;
	double t;
	double inside;
	size_t index;
	int seen;

	/* Each pixel, nothing where the gradient is not defined or does not repeat. */
	for (index = 0U; index < count; index++) {
		pixels[index] = 0U;
		seen = composite_place(source, x + (int)index, y, &place_x, &place_y);
		if (!seen)
			continue;
		seen = composite_gradient_t(source, place_x, place_y, &t);
		if (!seen)
			continue;
		seen = composite_gradient_repeat(source->repeat, t, &inside);
		if (!seen)
			continue;
		pixels[index] = composite_gradient_color(source, inside);
	}
}

/*
 * Gives the place a pixel's centre reads a gradient at (through the
 * transform when there is one), in pixels.  Returns 1, or 0 when the
 * transform sends it to infinity.
 */
static int
composite_place(
	const struct x11_source *source,
	int x,
	int y,
	double *place_x,
	double *place_y)
{
	int64_t centre_x;
	int64_t centre_y;
	int64_t fixed_x;
	int64_t fixed_y;
	int seen;

	/* The centre, as it is. */
	if (!source->transformed) {
		*place_x = (double)x + 0.5;
		*place_y = (double)y + 0.5;
		return 1;
	}

	/* Through the transform. */
	centre_x = (int64_t)x * COMPOSITE_FIXED_ONE + COMPOSITE_FIXED_ONE / 2;
	centre_y = (int64_t)y * COMPOSITE_FIXED_ONE + COMPOSITE_FIXED_ONE / 2;
	seen = composite_transform_fixed(source, centre_x, centre_y, &fixed_x, &fixed_y);
	if (!seen)
		return 0;

	/* Succeeded: in pixels. */
	*place_x = (double)fixed_x / COMPOSITE_FIXED_ONE;
	*place_y = (double)fixed_y / COMPOSITE_FIXED_ONE;
	return 1;
}

/*
 * Places a point along a gradient: the parameter t that is 0 at its start
 * and 1 at its end.  Returns 1 with t, or 0 where the gradient has no
 * value (a radial gradient outside both circles' sweep, or a degenerate
 * one).
 */
static int
composite_gradient_t(
	const struct x11_source *source,
	double x,
	double y,
	double *t)
{
	double start_x;
	double start_y;
	double end_x;
	double end_y;
	double start_radius;
	double radius_change;
	double centre_x;
	double centre_y;
	double point_x;
	double point_y;
	double length;
	double a;
	double b;
	double c;
	double discriminant;
	double root;
	double candidates[2];
	double angle;
	unsigned index;

	/* The two points (or circles), in pixels. */
	start_x = (double)source->points[0] / COMPOSITE_FIXED_ONE;
	start_y = (double)source->points[1] / COMPOSITE_FIXED_ONE;
	end_x = (double)source->points[2] / COMPOSITE_FIXED_ONE;
	end_y = (double)source->points[3] / COMPOSITE_FIXED_ONE;

	/* A linear gradient: the point projected on the line from the start to the end. */
	if (source->kind == X11_SOURCE_LINEAR) {
		length = (end_x - start_x) * (end_x - start_x) + (end_y - start_y) * (end_y - start_y);
		if (length == 0.0)
			return 0;
		*t = ((x - start_x) * (end_x - start_x) + (y - start_y) * (end_y - start_y)) / length;
		return 1;
	}

	/* A conical gradient: the angle about the centre from the gradient's angle, a whole turn being 1. */
	if (source->kind == X11_SOURCE_CONICAL) {
		angle = atan2(y - start_y, x - start_x) + (double)source->angle / COMPOSITE_FIXED_ONE * COMPOSITE_PI / 180.0;
		angle = fmod(angle, 2.0 * COMPOSITE_PI);
		if (angle < 0.0)
			angle += 2.0 * COMPOSITE_PI;
		*t = 1.0 - angle / (2.0 * COMPOSITE_PI);
		return 1;
	}

	/*
	 * A radial gradient: the largest t whose circle, moving and growing
	 * from the start circle to the end circle, passes through the point
	 * with a radius of at least 0 (the circles of PDF's radial shading).
	 */
	start_radius = (double)source->radii[0] / COMPOSITE_FIXED_ONE;
	radius_change = (double)source->radii[1] / COMPOSITE_FIXED_ONE - start_radius;
	centre_x = end_x - start_x;
	centre_y = end_y - start_y;
	point_x = x - start_x;
	point_y = y - start_y;
	a = centre_x * centre_x + centre_y * centre_y - radius_change * radius_change;
	b = point_x * centre_x + point_y * centre_y + start_radius * radius_change;
	c = point_x * point_x + point_y * point_y - start_radius * start_radius;

	/* One root when the quadratic is linear. */
	if (a == 0.0) {
		if (b == 0.0)
			return 0;
		*t = c / (2.0 * b);
		if (start_radius + *t * radius_change < 0.0)
			return 0;
		return 1;
	}

	/* No circle passes through the point. */
	discriminant = b * b - a * c;
	if (discriminant < 0.0)
		return 0;

	/* The larger root first, the first whose radius is not negative. */
	root = sqrt(discriminant);
	candidates[0] = (b + root) / a;
	candidates[1] = (b - root) / a;
	if (candidates[1] > candidates[0]) {
		candidates[0] = (b - root) / a;
		candidates[1] = (b + root) / a;
	}
	for (index = 0U; index < 2U; index++) {
		if (start_radius + candidates[index] * radius_change >= 0.0) {
			*t = candidates[index];
			return 1;
		}
	}

	/* Neither circle has a radius. */
	return 0;
}

/*
 * Brings a gradient's parameter into 0 to 1 by the picture's repeat.
 * Returns 1 with it, or 0 outside a gradient that does not repeat.
 */
static int
composite_gradient_repeat(
	unsigned repeat,
	double t,
	double *place)
{
	/* By the repeat. */
	switch (repeat) {
	case X11_REPEAT_NORMAL:
		/* The fraction. */
		t -= floor(t);
		break;
	case X11_REPEAT_PAD:
		/* The nearest end. */
		if (t < 0.0)
			t = 0.0;
		if (t > 1.0)
			t = 1.0;
		break;
	case X11_REPEAT_REFLECT:
		/* Back and forth: a period of 2. */
		t = fmod(t, 2.0);
		if (t < 0.0)
			t += 2.0;
		if (t > 1.0)
			t = 2.0 - t;
		break;
	default:
		/* Nothing outside. */
		if (t < 0.0 || t > 1.0)
			return 0;
		break;
	}

	/* Succeeded: inside. */
	*place = t;
	return 1;
}

/*
 * Gives a gradient's colour at a parameter from 0 to 1: the colours of
 * the stops on either side mixed by where it is between them (the first
 * stop's colour before it, the last's after it), premultiplied, as
 * a8r8g8b8.
 */
static uint32_t
composite_gradient_color(
	const struct x11_source *source,
	double t)
{
	const struct x11_gradient_stop *before;
	const struct x11_gradient_stop *after;
	double before_offset;
	double after_offset;
	double share;
	double alpha;
	double red;
	double green;
	double blue;
	unsigned channels[4];
	size_t index;

	/* No stop: nothing. */
	if (source->stop_count == 0U)
		return 0U;

	/* The stops on either side of t. */
	before = &source->stops[0];
	after = &source->stops[0];
	for (index = 0U; index < source->stop_count; index++) {
		after = &source->stops[index];
		if ((double)after->offset / COMPOSITE_FIXED_ONE > t)
			break;
		before = after;
	}

	/* Where t is between them. */
	before_offset = (double)before->offset / COMPOSITE_FIXED_ONE;
	after_offset = (double)after->offset / COMPOSITE_FIXED_ONE;
	share = 0.0;
	if (after_offset > before_offset)
		share = (t - before_offset) / (after_offset - before_offset);
	if (share < 0.0)
		share = 0.0;
	if (share > 1.0)
		share = 1.0;

	/* The colour mixed, then premultiplied. */
	alpha = ((double)before->color.alpha * (1.0 - share) + (double)after->color.alpha * share) / 65535.0;
	red = ((double)before->color.red * (1.0 - share) + (double)after->color.red * share) / 65535.0;
	green = ((double)before->color.green * (1.0 - share) + (double)after->color.green * share) / 65535.0;
	blue = ((double)before->color.blue * (1.0 - share) + (double)after->color.blue * share) / 65535.0;

	/* As a8r8g8b8. */
	channels[0] = composite_round(alpha * 255.0);
	channels[1] = composite_round(red * alpha * 255.0);
	channels[2] = composite_round(green * alpha * 255.0);
	channels[3] = composite_round(blue * alpha * 255.0);
	return composite_pack(channels[0], channels[1], channels[2], channels[3]);
}

/*
 * Joins a row: each source pixel with its mask (the alpha alone, or each
 * channel when component is set), then with the target's pixel by the
 * operator.  The result replaces the target row.
 */
static void
composite_join(
	unsigned op,
	int component,
	const uint32_t *source,
	const uint32_t *mask,
	uint32_t *target,
	size_t count)
{
	uint32_t pixel;
	uint32_t mask_pixel;
	size_t index;

	/* Each pixel. */
	for (index = 0U; index < count; index++) {
		/* Without a mask every channel lets the source through. */
		mask_pixel = 0xffffffffU;
		if (mask != NULL)
			mask_pixel = mask[index];

		/*
		 * The families: the blend modes, the disjoint and conjoint
		 * operators, and Porter-Duff's.  The blend modes of hue,
		 * saturation and luminosity leave the target as it is under a
		 * mask of component alpha (as pixman does: what they would mean
		 * a channel at a time is not clear).
		 */
		if (op >= COMPOSITE_OP_BLEND + BLEND_HSL_HUE && component) {
			continue;
		} else if (op >= COMPOSITE_OP_BLEND) {
			target[index] = composite_blend(op - COMPOSITE_OP_BLEND, component, source[index], mask_pixel, target[index]);
		} else if (op >= COMPOSITE_OP_DISJOINT) {
			target[index] = composite_factored(op, component, source[index], mask_pixel, target[index]);
		} else if (component) {
			target[index] = composite_porter_duff_ca(op, source[index], mask_pixel, target[index]);
		} else {
			pixel = source[index];
			if (mask != NULL)
				pixel = composite_mask_pixel(pixel, composite_channel(mask_pixel, COMPOSITE_ALPHA_SHIFT));
			target[index] = composite_porter_duff(op, pixel, target[index]);
		}
	}
}

/*
 * Joins a source pixel (its mask already applied) with a target pixel by
 * a Porter-Duff operator, a channel at a time in 8 bits as pixman does
 * (each product rounded, sums kept at most 255).
 *
 * Procedure referenced: pixman 0.44.0 pixman/pixman-combine32.c (the
 * unified combiners combine_*_u).
 */
static uint32_t
composite_porter_duff(
	unsigned op,
	uint32_t source,
	uint32_t target)
{
	unsigned source_alpha;
	unsigned target_alpha;
	unsigned channels[4];
	unsigned s;
	unsigned d;
	unsigned factor;
	unsigned shift;
	unsigned index;

	/* The alphas. */
	source_alpha = composite_channel(source, COMPOSITE_ALPHA_SHIFT);
	target_alpha = composite_channel(target, COMPOSITE_ALPHA_SHIFT);

	/* Saturate: the source scaled down to fit the room the target's alpha leaves, then added. */
	if (op == COMPOSITE_OP_SATURATE) {
		if (source_alpha > 255U - target_alpha) {
			factor = composite_divide(255U - target_alpha, source_alpha);
			source = composite_mask_pixel(source, factor);
		}
		op = PD_ADD;
	}

	/* Each channel by the operator. */
	for (index = 0U; index < 4U; index++) {
		shift = index * 8U;
		s = composite_channel(source, shift);
		d = composite_channel(target, shift);

		/* The operator's form. */
		switch (op) {
		case PD_CLEAR:
			channels[index] = 0U;
			break;
		case PD_SRC:
			channels[index] = s;
			break;
		case PD_DST:
			channels[index] = d;
			break;
		case PD_OVER:
			channels[index] = composite_add(s, composite_multiply(d, 255U - source_alpha));
			break;
		case PD_OVER_REVERSE:
			channels[index] = composite_add(d, composite_multiply(s, 255U - target_alpha));
			break;
		case PD_IN:
			channels[index] = composite_multiply(s, target_alpha);
			break;
		case PD_IN_REVERSE:
			channels[index] = composite_multiply(d, source_alpha);
			break;
		case PD_OUT:
			channels[index] = composite_multiply(s, 255U - target_alpha);
			break;
		case PD_OUT_REVERSE:
			channels[index] = composite_multiply(d, 255U - source_alpha);
			break;
		case PD_ATOP:
			channels[index] = composite_multiply_add(s, target_alpha, d, 255U - source_alpha);
			break;
		case PD_ATOP_REVERSE:
			channels[index] = composite_multiply_add(s, 255U - target_alpha, d, source_alpha);
			break;
		case PD_XOR:
			channels[index] = composite_multiply_add(s, 255U - target_alpha, d, 255U - source_alpha);
			break;
		default:
			/* Add. */
			channels[index] = composite_add(s, d);
			break;
		}
	}

	/* The pixel. */
	return composite_pack(channels[3], channels[2], channels[1], channels[0]);
}

/*
 * Joins a source pixel and a mask of component alpha with a target pixel
 * by a Porter-Duff operator: each channel of the source is scaled by the
 * mask's same channel, and each channel's alpha is the mask's channel by
 * the source's alpha.
 *
 * Procedure referenced: pixman 0.44.0 pixman/pixman-combine32.c
 * (combine_mask_ca and the component-alpha combiners combine_*_ca).
 */
static uint32_t
composite_porter_duff_ca(
	unsigned op,
	uint32_t source,
	uint32_t mask,
	uint32_t target)
{
	unsigned source_alpha;
	unsigned target_alpha;
	unsigned channels[4];
	unsigned s;
	unsigned m;
	unsigned d;
	unsigned shift;
	unsigned index;
	double s_part;
	double m_part;
	double d_part;
	double room;

	/* The alphas. */
	source_alpha = composite_channel(source, COMPOSITE_ALPHA_SHIFT);
	target_alpha = composite_channel(target, COMPOSITE_ALPHA_SHIFT);

	/* Each channel: the source by the mask, the mask by the source's alpha, joined with the target. */
	for (index = 0U; index < 4U; index++) {
		shift = index * 8U;
		s = composite_multiply(composite_channel(source, shift), composite_channel(mask, shift));
		m = composite_multiply(composite_channel(mask, shift), source_alpha);
		d = composite_channel(target, shift);

		/* The operator's form, with the channel's own alpha m. */
		switch (op) {
		case PD_CLEAR:
			channels[index] = 0U;
			break;
		case PD_SRC:
			channels[index] = s;
			break;
		case PD_DST:
			channels[index] = d;
			break;
		case PD_OVER:
			channels[index] = composite_add(s, composite_multiply(d, 255U - m));
			break;
		case PD_OVER_REVERSE:
			channels[index] = composite_add(d, composite_multiply(s, 255U - target_alpha));
			break;
		case PD_IN:
			channels[index] = composite_multiply(s, target_alpha);
			break;
		case PD_IN_REVERSE:
			channels[index] = composite_multiply(d, m);
			break;
		case PD_OUT:
			channels[index] = composite_multiply(s, 255U - target_alpha);
			break;
		case PD_OUT_REVERSE:
			channels[index] = composite_multiply(d, 255U - m);
			break;
		case PD_ATOP:
			channels[index] = composite_multiply_add(d, 255U - m, s, target_alpha);
			break;
		case PD_ATOP_REVERSE:
			channels[index] = composite_multiply_add(d, m, s, 255U - target_alpha);
			break;
		case PD_XOR:
			channels[index] = composite_multiply_add(d, 255U - m, s, 255U - target_alpha);
			break;
		case COMPOSITE_OP_SATURATE:
			/* The channel scaled down to fit the room the target's alpha leaves, then added. */
			s_part = (double)s;
			m_part = (double)m;
			d_part = (double)d;
			room = (double)(255U - target_alpha);
			if (m_part > room && m_part > 0.0)
				s_part = s_part * room / m_part;
			channels[index] = composite_add(composite_round(s_part), composite_round(d_part));
			break;
		default:
			/* Add. */
			channels[index] = composite_add(s, d);
			break;
		}
	}

	/* The pixel. */
	return composite_pack(channels[3], channels[2], channels[1], channels[0]);
}

/*
 * Joins by a disjoint or conjoint operator: the source and the target
 * each weighted by their factor (which the family works out from the two
 * alphas), in floating point, rounded to 8 bits.  A mask scales the
 * source (each channel, for component alpha, with that channel's alpha).
 */
static uint32_t
composite_factored(
	unsigned op,
	int component,
	uint32_t source,
	uint32_t mask,
	uint32_t target)
{
	unsigned family;
	unsigned base;
	unsigned channels[4];
	unsigned shift;
	unsigned mask_shift;
	unsigned index;
	double source_alpha;
	double channel_alpha;
	double target_alpha;
	double s;
	double d;
	double source_factor;
	double target_factor;
	double value;

	/* The family and its Porter-Duff operator. */
	family = op & 0xf0U;
	base = op & 0x0fU;

	/*
	 * Each channel with its own alpha: the source and its alpha by the
	 * mask's alpha, or (component alpha) by the mask's channel.
	 */
	source_alpha = (double)composite_channel(source, COMPOSITE_ALPHA_SHIFT) / 255.0;
	target_alpha = (double)composite_channel(target, COMPOSITE_ALPHA_SHIFT) / 255.0;
	for (index = 0U; index < 4U; index++) {
		shift = index * 8U;
		mask_shift = COMPOSITE_ALPHA_SHIFT;
		if (component)
			mask_shift = shift;
		s = (double)composite_channel(source, shift) / 255.0 * (double)composite_channel(mask, mask_shift) / 255.0;
		channel_alpha = source_alpha * (double)composite_channel(mask, mask_shift) / 255.0;
		d = (double)composite_channel(target, shift) / 255.0;
		source_factor = composite_factor(family, base, 1, channel_alpha, target_alpha);
		target_factor = composite_factor(family, base, 0, channel_alpha, target_alpha);
		value = s * source_factor + d * target_factor;
		if (value > 1.0)
			value = 1.0;
		channels[index] = composite_round(value * 255.0);
	}

	/* The pixel. */
	return composite_pack(channels[3], channels[2], channels[1], channels[0]);
}

/*
 * Gives the factor of a disjoint or conjoint operator for the source
 * (is_source) or the target, from the two alphas (0 to 1).  The disjoint
 * family takes the shapes as far apart as they can be, the conjoint one
 * as much on top of each other.
 */
static double
composite_factor(
	unsigned family,
	unsigned base,
	int is_source,
	double source_alpha,
	double target_alpha)
{
	double own;
	double other;
	double ratio;
	unsigned part;

	/* The factor's own alpha and the other's. */
	own = target_alpha;
	other = source_alpha;
	if (is_source) {
		own = source_alpha;
		other = target_alpha;
	}

	/* Which part of the other's coverage the factor takes: none, all, the part outside it, or the part inside it. */
	part = 0U;
	switch (base) {
	case PD_SRC:
		part = is_source ? 1U : 0U;
		break;
	case PD_DST:
		part = is_source ? 0U : 1U;
		break;
	case PD_OVER:
		part = is_source ? 1U : 2U;
		break;
	case PD_OVER_REVERSE:
		part = is_source ? 2U : 1U;
		break;
	case PD_IN:
		part = is_source ? 3U : 0U;
		break;
	case PD_IN_REVERSE:
		part = is_source ? 0U : 3U;
		break;
	case PD_OUT:
		part = is_source ? 2U : 0U;
		break;
	case PD_OUT_REVERSE:
		part = is_source ? 0U : 2U;
		break;
	case PD_ATOP:
		part = is_source ? 3U : 2U;
		break;
	case PD_ATOP_REVERSE:
		part = is_source ? 2U : 3U;
		break;
	case PD_XOR:
		part = 2U;
		break;
	default:
		/* Clear, and the numbers without an operator. */
		part = 0U;
		break;
	}

	/* None, or all. */
	if (part == 0U)
		return 0.0;
	if (part == 1U)
		return 1.0;

	/*
	 * Outside the other: disjoint min(1, (1 - other) / own), conjoint
	 * max(1 - other / own, 0).  Inside it: disjoint max(1 - (1 - other) /
	 * own, 0), conjoint min(1, other / own).  An own alpha of 0 makes the
	 * ratio infinite.
	 */
	if (family == COMPOSITE_OP_DISJOINT)
		other = 1.0 - other;
	ratio = 1.0e9;
	if (own > 0.0)
		ratio = other / own;
	if ((part == 2U) == (family == COMPOSITE_OP_DISJOINT)) {
		if (ratio > 1.0)
			return 1.0;
		return ratio;
	}
	if (1.0 - ratio < 0.0)
		return 0.0;

	/* The part left. */
	return 1.0 - ratio;
}

/*
 * Joins by a blend mode (PDF's): where both are, the mode's mix of the
 * colours; where only one is, its own colour.  A mask scales the source
 * by its alpha, or (component alpha) each channel by its own channel, the
 * channel's alpha then the source's alpha by it.  In floating point,
 * rounded to 8 bits.
 */
static uint32_t
composite_blend(
	unsigned mode,
	int component,
	uint32_t source,
	uint32_t mask,
	uint32_t target)
{
	double source_colour[3];
	double target_colour[3];
	double channel_alpha[3];
	double mixed[3];
	double source_alpha;
	double target_alpha;
	double mask_alpha;
	double alpha;
	double channel;
	unsigned shifts[3];
	unsigned channels[3];
	unsigned index;

	/* Both alphas, the source's by the mask's alpha. */
	mask_alpha = (double)composite_channel(mask, COMPOSITE_ALPHA_SHIFT) / 255.0;
	source_alpha = (double)composite_channel(source, COMPOSITE_ALPHA_SHIFT) / 255.0;
	target_alpha = (double)composite_channel(target, COMPOSITE_ALPHA_SHIFT) / 255.0;

	/* The colours unpremultiplied (red, green, blue), and each channel's source alpha after the mask. */
	shifts[0] = COMPOSITE_RED_SHIFT;
	shifts[1] = COMPOSITE_GREEN_SHIFT;
	shifts[2] = COMPOSITE_BLUE_SHIFT;
	for (index = 0U; index < 3U; index++) {
		source_colour[index] = 0.0;
		target_colour[index] = 0.0;
		if (source_alpha > 0.0)
			source_colour[index] = (double)composite_channel(source, shifts[index]) / 255.0 / source_alpha;
		if (target_alpha > 0.0)
			target_colour[index] = (double)composite_channel(target, shifts[index]) / 255.0 / target_alpha;
		if (source_colour[index] > 1.0)
			source_colour[index] = 1.0;
		if (target_colour[index] > 1.0)
			target_colour[index] = 1.0;
		channel_alpha[index] = source_alpha * mask_alpha;
		if (component)
			channel_alpha[index] = source_alpha * (double)composite_channel(mask, shifts[index]) / 255.0;
	}

	/* The mode's mix: a channel at a time, or the colour as a whole (hue, saturation, colour, luminosity). */
	if (mode >= BLEND_HSL_HUE) {
		composite_blend_hsl(mode, source_colour, target_colour, mixed);
	} else {
		for (index = 0U; index < 3U; index++)
			mixed[index] = composite_blend_channel(mode, source_colour[index], target_colour[index]);
	}

	/* Each channel: the source alone, the target alone, and the mix where both are. */
	for (index = 0U; index < 3U; index++) {
		channel = (1.0 - target_alpha) * channel_alpha[index] * source_colour[index] + (1.0 - channel_alpha[index]) * target_alpha * target_colour[index] + channel_alpha[index] * target_alpha * mixed[index];
		if (channel > 1.0)
			channel = 1.0;
		channels[index] = composite_round(channel * 255.0);
	}
	source_alpha *= mask_alpha;
	alpha = source_alpha + target_alpha - source_alpha * target_alpha;

	/* The pixel. */
	return composite_pack(composite_round(alpha * 255.0), channels[0], channels[1], channels[2]);
}

/* Mixes one channel of two colours (0 to 1) by a separable blend mode. */
static double
composite_blend_channel(
	unsigned mode,
	double source,
	double target)
{
	double d;

	/* By the mode. */
	switch (mode) {
	case BLEND_MULTIPLY:
		return source * target;
	case BLEND_SCREEN:
		return source + target - source * target;
	case BLEND_OVERLAY:
		/* Hard light with the two swapped. */
		return composite_blend_channel(BLEND_HARD_LIGHT, target, source);
	case BLEND_DARKEN:
		if (source < target)
			return source;
		return target;
	case BLEND_LIGHTEN:
		if (source > target)
			return source;
		return target;
	case BLEND_COLOR_DODGE:
		if (target == 0.0)
			return 0.0;
		if (source >= 1.0)
			return 1.0;
		if (target / (1.0 - source) > 1.0)
			return 1.0;
		return target / (1.0 - source);
	case BLEND_COLOR_BURN:
		if (target >= 1.0)
			return 1.0;
		if (source <= 0.0)
			return 0.0;
		if ((1.0 - target) / source > 1.0)
			return 0.0;
		return 1.0 - (1.0 - target) / source;
	case BLEND_HARD_LIGHT:
		if (source <= 0.5)
			return source * 2.0 * target;
		return composite_blend_channel(BLEND_SCREEN, 2.0 * source - 1.0, target);
	case BLEND_SOFT_LIGHT:
		if (source <= 0.5)
			return target - (1.0 - 2.0 * source) * target * (1.0 - target);
		d = sqrt(target);
		if (target <= 0.25)
			d = ((16.0 * target - 12.0) * target + 4.0) * target;
		return target + (2.0 * source - 1.0) * (d - target);
	case BLEND_DIFFERENCE:
		if (source > target)
			return source - target;
		return target - source;
	default:
		/* Exclusion. */
		return source + target - 2.0 * source * target;
	}
}

/* Mixes two colours (red, green, blue, 0 to 1) by a blend mode of hue, saturation and luminosity. */
static void
composite_blend_hsl(
	unsigned mode,
	const double *source,
	const double *target,
	double *result)
{
	double saturated[3];

	/* By the mode. */
	switch (mode) {
	case BLEND_HSL_HUE:
		/* The source's hue, the target's saturation and luminosity. */
		composite_set_saturation(source, composite_saturation(target), saturated);
		composite_set_luminosity(saturated, composite_luminosity(target), result);
		break;
	case BLEND_HSL_SATURATION:
		/* The source's saturation, the target's hue and luminosity. */
		composite_set_saturation(target, composite_saturation(source), saturated);
		composite_set_luminosity(saturated, composite_luminosity(target), result);
		break;
	case BLEND_HSL_COLOR:
		/* The source's hue and saturation, the target's luminosity. */
		composite_set_luminosity(source, composite_luminosity(target), result);
		break;
	default:
		/* The source's luminosity, the target's hue and saturation. */
		composite_set_luminosity(target, composite_luminosity(source), result);
		break;
	}
}

/* Gives a colour's luminosity (PDF's weights of red, green and blue). */
static double
composite_luminosity(
	const double *color)
{
	/* The weighted sum. */
	return 0.3 * color[0] + 0.59 * color[1] + 0.11 * color[2];
}

/* Gives a colour moved to a luminosity, brought back inside 0 to 1 keeping its luminosity. */
static void
composite_set_luminosity(
	const double *color,
	double luminosity,
	double *result)
{
	double change;
	double lowest;
	double highest;
	double own;
	unsigned index;

	/* Moved by the difference. */
	change = luminosity - composite_luminosity(color);
	for (index = 0U; index < 3U; index++)
		result[index] = color[index] + change;

	/* Its extremes and its luminosity now. */
	own = composite_luminosity(result);
	lowest = result[0];
	highest = result[0];
	for (index = 1U; index < 3U; index++) {
		if (result[index] < lowest)
			lowest = result[index];
		if (result[index] > highest)
			highest = result[index];
	}

	/* Below 0 or above 1: drawn towards the luminosity until inside. */
	for (index = 0U; index < 3U; index++) {
		if (lowest < 0.0 && own - lowest > 0.0)
			result[index] = own + (result[index] - own) * own / (own - lowest);
	}
	for (index = 0U; index < 3U; index++) {
		if (highest > 1.0 && highest - own > 0.0)
			result[index] = own + (result[index] - own) * (1.0 - own) / (highest - own);
	}
}

/* Gives a colour's saturation: its largest channel less its smallest. */
static double
composite_saturation(
	const double *color)
{
	double lowest;
	double highest;
	unsigned index;

	/* The extremes. */
	lowest = color[0];
	highest = color[0];
	for (index = 1U; index < 3U; index++) {
		if (color[index] < lowest)
			lowest = color[index];
		if (color[index] > highest)
			highest = color[index];
	}

	/* Their difference. */
	return highest - lowest;
}

/* Gives a colour with a saturation: its middle channel scaled between its smallest (made 0) and largest (made the saturation). */
static void
composite_set_saturation(
	const double *color,
	double saturation,
	double *result)
{
	double lowest;
	double highest;
	unsigned index;

	/* The extremes. */
	lowest = color[0];
	highest = color[0];
	for (index = 1U; index < 3U; index++) {
		if (color[index] < lowest)
			lowest = color[index];
		if (color[index] > highest)
			highest = color[index];
	}

	/* A grey has no hue: black. */
	if (highest <= lowest) {
		for (index = 0U; index < 3U; index++)
			result[index] = 0.0;
		return;
	}

	/* Each channel scaled. */
	for (index = 0U; index < 3U; index++)
		result[index] = (color[index] - lowest) * saturation / (highest - lowest);
}

/* Gives a channel of a pixel (its 8 bits at a shift). */
static unsigned
composite_channel(
	uint32_t pixel,
	unsigned shift)
{
	/* The byte. */
	return (unsigned)((pixel >> shift) & 0xffU);
}

/* Packs four channels of 8 bits into a8r8g8b8. */
static uint32_t
composite_pack(
	unsigned alpha,
	unsigned red,
	unsigned green,
	unsigned blue)
{
	/* Each in its byte. */
	return ((uint32_t)alpha << COMPOSITE_ALPHA_SHIFT) | ((uint32_t)red << COMPOSITE_RED_SHIFT) | ((uint32_t)green << COMPOSITE_GREEN_SHIFT) | ((uint32_t)blue << COMPOSITE_BLUE_SHIFT);
}

/* Multiplies two 8-bit values as fractions of 255, rounded. */
static unsigned
composite_multiply(
	unsigned a,
	unsigned b)
{
	unsigned product;

	/* The product plus a half, divided by 255 exactly. */
	product = a * b + 0x80U;
	return ((product >> 8U) + product) >> 8U;
}

/* Gives x by a plus y by b (fractions of 255), each product rounded, the sum kept at most 255. */
static unsigned
composite_multiply_add(
	unsigned x,
	unsigned a,
	unsigned y,
	unsigned b)
{
	unsigned first;
	unsigned second;

	/* The two products. */
	first = composite_multiply(x, a);
	second = composite_multiply(y, b);

	/* Their sum. */
	return composite_add(first, second);
}

/* Adds two 8-bit values, kept at most 255. */
static unsigned
composite_add(
	unsigned a,
	unsigned b)
{
	/* The sum, or the most. */
	if (a + b > 255U)
		return 255U;
	return a + b;
}

/* Divides a by b as fractions of 255 (a below b), rounded. */
static unsigned
composite_divide(
	unsigned a,
	unsigned b)
{
	/* Nothing to divide by gives all. */
	if (b == 0U)
		return 255U;

	/* The quotient rounded. */
	return (a * 255U + b / 2U) / b;
}

/* Scales every channel of a pixel by an 8-bit alpha. */
static uint32_t
composite_mask_pixel(
	uint32_t source,
	unsigned alpha)
{
	uint32_t scaled;
	unsigned shift;

	/* All: the pixel as it is; none: nothing. */
	if (alpha == 255U)
		return source;
	if (alpha == 0U)
		return 0U;

	/* Each channel. */
	scaled = 0U;
	for (shift = 0U; shift < 32U; shift += 8U)
		scaled |= (uint32_t)composite_multiply(composite_channel(source, shift), alpha) << shift;
	return scaled;
}

/* Rounds a value to the nearest whole number from 0 to 255. */
static unsigned
composite_round(
	double value)
{
	/* Kept in range. */
	if (value <= 0.0)
		return 0U;
	if (value >= 255.0)
		return 255U;

	/* The nearest. */
	return (unsigned)(value + 0.5);
}
