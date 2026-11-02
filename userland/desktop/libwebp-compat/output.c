/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The conversion of a decoded image into the output layouts.
 */

#include "output.h"

/*
 * The constants of the YUV to RGB conversion, in 14-bit fixed point: the
 * multiplier of Y', the multipliers of Cr for red and green, of Cb for
 * green and blue, and the offsets that fold in the 16 and 128 biases.
 *
 * Values referenced: libwebp 1.5.0 (BSD-3-Clause) src/dsp/yuv.h, the
 * default YUV to RGB constants, as output.h states them.
 */
#define OUTPUT_Y_SCALE		19077
#define OUTPUT_V_TO_RED		26149
#define OUTPUT_U_TO_GREEN	6419
#define OUTPUT_V_TO_GREEN	13320
#define OUTPUT_U_TO_BLUE	33050
#define OUTPUT_RED_OFFSET	14234
#define OUTPUT_GREEN_OFFSET	8708
#define OUTPUT_BLUE_OFFSET	17685

/* The fixed point has 6 fractional bits left after the 8-bit products. */
#define OUTPUT_FIXED_SHIFT	6

static int output_multiply(unsigned value, unsigned factor);
static unsigned output_clip(int total);
static unsigned output_chroma(const uint8_t *plane, ptrdiff_t stride, unsigned plane_width, unsigned plane_height,
	unsigned x, unsigned y, int fancy);
static void output_store(const struct webpc_layout *layout, uint8_t *pixel, unsigned red, unsigned green, unsigned blue,
	unsigned alpha);

/*
 * Converts one pixel's Y', Cb (u) and Cr (v), each 0..255, to rgb[0..2]
 * (red, green, blue).
 */
void
webpc_output_yuv_pixel(
	unsigned y,
	unsigned u,
	unsigned v,
	uint8_t rgb[3])
{
	int luma;
	int red;
	int green;
	int blue;

	/* The luma term is shared by the three components. */
	luma = output_multiply(y, OUTPUT_Y_SCALE);

	/* Each component adds its chroma terms and offset before the fraction is dropped. */
	red = luma + output_multiply(v, OUTPUT_V_TO_RED) - OUTPUT_RED_OFFSET;
	green = luma - output_multiply(u, OUTPUT_U_TO_GREEN) - output_multiply(v, OUTPUT_V_TO_GREEN) + OUTPUT_GREEN_OFFSET;
	blue = luma + output_multiply(u, OUTPUT_U_TO_BLUE) - OUTPUT_BLUE_OFFSET;

	/* The clipped components are stored in the order red, green, blue. */
	rgb[0] = (uint8_t)output_clip(red);
	rgb[1] = (uint8_t)output_clip(green);
	rgb[2] = (uint8_t)output_clip(blue);
}

/*
 * Premultiplies a component x (0..255) by alpha a (0..255) as libwebp does:
 * (x * a) / 255, rounded down (x itself when a is 255).
 *
 * Procedure referenced: libwebp 1.5.0 (BSD-3-Clause)
 * src/dsp/alpha_processing.c, the premultiplication of its rgbA modes, as
 * output.h states it: the floor of x * a / 255.
 */
unsigned
webpc_output_premultiply(
	unsigned x,
	unsigned a)
{
	unsigned product;

	/* An opaque pixel keeps its colour exactly. */
	if (a == 255U)
		return x;

	/* The product is at most 255 * 255, so the division is the floor of x * a / 255. */
	product = x * a;

	/* Reports the premultiplied component. */
	return product / 255U;
}

/*
 * Writes a lossy image as RGB rows: the row of image row r starts at
 * first_row + r * row_step (row_step negative to flip; the caller has
 * checked that every row fits its buffer).
 *
 * Procedure referenced: libwebp 1.5.0 (BSD-3-Clause) src/dsp/upsampling.c,
 * its default (fancy) chroma upsampling, which output.h states: the 9-3-3-1
 * weights of the four nearest chroma samples.
 */
void
webpc_output_from_yuv(
	const struct webpc_yuv_view *image,
	const uint8_t *alpha,
	ptrdiff_t alpha_stride,
	int fancy,
	const struct webpc_layout *layout,
	uint8_t *first_row,
	ptrdiff_t row_step)
{
	unsigned plane_width;
	unsigned plane_height;
	unsigned x;
	unsigned y;
	unsigned u;
	unsigned v;
	unsigned alpha_value;
	uint8_t rgb[3];
	const uint8_t *luma_row;
	const uint8_t *alpha_row;
	uint8_t *out_row;

	/* The chroma planes are half the size, rounded up. */
	plane_width = (image->width + 1U) / 2U;
	plane_height = (image->height + 1U) / 2U;

	/* Converts every image row. */
	for (y = 0; y < image->height; y++) {
		/* The row's sources and its destination. */
		luma_row = image->y + (ptrdiff_t)y * image->y_stride;
		alpha_row = NULL;
		if (alpha != NULL)
			alpha_row = alpha + (ptrdiff_t)y * alpha_stride;
		out_row = first_row + (ptrdiff_t)y * row_step;

		/* Converts every pixel of the row. */
		for (x = 0; x < image->width; x++) {
			/* The pixel's chroma, interpolated or copied. */
			u = output_chroma(image->u, image->uv_stride, plane_width, plane_height, x, y, fancy);
			v = output_chroma(image->v, image->uv_stride, plane_width, plane_height, x, y, fancy);

			/* The colour from the YUV triple. */
			webpc_output_yuv_pixel(luma_row[x], u, v, rgb);

			/* Without an alpha plane the pixel is opaque. */
			alpha_value = 255U;
			if (alpha_row != NULL)
				alpha_value = alpha_row[x];

			/* Stores the pixel in the layout. */
			output_store(layout, out_row + (size_t)x * layout->bytes, rgb[0], rgb[1], rgb[2], alpha_value);
		}
	}
}

/*
 * Writes a lossless image (width * height ARGB pixels, rows top to bottom)
 * as rows of layout, the row of image row r at first_row + r * row_step.
 */
void
webpc_output_from_argb(
	const uint32_t *pixels,
	unsigned width,
	unsigned height,
	const struct webpc_layout *layout,
	uint8_t *first_row,
	ptrdiff_t row_step)
{
	unsigned x;
	unsigned y;
	uint32_t argb;
	const uint32_t *in_row;
	uint8_t *out_row;

	/* Converts every image row. */
	for (y = 0; y < height; y++) {
		/* The row's source and its destination. */
		in_row = pixels + (size_t)y * width;
		out_row = first_row + (ptrdiff_t)y * row_step;

		/* Converts every pixel of the row. */
		for (x = 0; x < width; x++) {
			/* The pixel's four 8-bit components are in alpha, red, green, blue order. */
			argb = in_row[x];

			/* Stores the pixel in the layout. */
			output_store(layout, out_row + (size_t)x * layout->bytes, (argb >> 16) & 0xffU, (argb >> 8) & 0xffU,
				argb & 0xffU, argb >> 24);
		}
	}
}

/*
 * Multiplies an 8-bit value by a constant and drops 8 bits.
 */
static int
output_multiply(
	unsigned value,
	unsigned factor)
{
	unsigned product;

	/* The product of an 8-bit value and a 16-bit constant fits an unsigned int. */
	product = (value * factor) >> 8;

	/* Reports the product as a signed value for the sums with the offsets. */
	return (int)product;
}

/*
 * Drops the fraction of a sum and clips it to 0..255: 0 below zero, 255
 * when the shifted value is above 255.
 */
static unsigned
output_clip(
	int total)
{
	int shifted;

	/* A negative sum is black. */
	if (total < 0)
		return 0;

	/* A non-negative sum shifts right without a sign. */
	shifted = total >> OUTPUT_FIXED_SHIFT;

	/* A value above 255 saturates. */
	if (shifted > 255)
		return 255U;

	/* Reports the in-range component. */
	return (unsigned)shifted;
}

/*
 * Gives the chroma of pixel (x, y) from one chroma plane: the 9-3-3-1
 * interpolation of the four nearest samples with fancy, else the sample of
 * the 2x2 block the pixel is in.
 */
static unsigned
output_chroma(
	const uint8_t *plane,
	ptrdiff_t stride,
	unsigned plane_width,
	unsigned plane_height,
	unsigned x,
	unsigned y,
	int fancy)
{
	unsigned column;
	unsigned row;
	unsigned side_column;
	unsigned side_row;
	unsigned near_sample;
	unsigned horizontal;
	unsigned vertical;
	unsigned diagonal;
	const uint8_t *near_row;
	const uint8_t *side_row_pointer;

	/* The sample of the 2x2 block the pixel is in. */
	column = x / 2U;
	row = y / 2U;
	near_row = plane + (ptrdiff_t)row * stride;

	/* Without fancy upsampling the block's sample is copied. */
	if (!fancy)
		return near_row[column];

	/* The neighbouring sample is on the left for an even coordinate and on the right for an odd one. */
	side_column = column + 1U;
	if ((x & 1U) == 0U)
		side_column = column - 1U;
	side_row = row + 1U;
	if ((y & 1U) == 0U)
		side_row = row - 1U;

	/* A neighbour beyond the plane takes the edge sample (0 - 1 wraps to a value above the size). */
	if (side_column >= plane_width) {
		if (column == 0U)
			side_column = 0U;
		else
			side_column = plane_width - 1U;
	}
	if (side_row >= plane_height) {
		if (row == 0U)
			side_row = 0U;
		else
			side_row = plane_height - 1U;
	}

	/* The four samples: the nearest, the horizontal and vertical neighbours, and the diagonal one. */
	side_row_pointer = plane + (ptrdiff_t)side_row * stride;
	near_sample = near_row[column];
	horizontal = near_row[side_column];
	vertical = side_row_pointer[column];
	diagonal = side_row_pointer[side_column];

	/* Reports the 9-3-3-1 weighted mean, rounded. */
	return (9U * near_sample + 3U * horizontal + 3U * vertical + diagonal + 8U) >> 4;
}

/*
 * Writes one pixel's bytes in a layout: the colour at the layout's offsets
 * (multiplied by alpha with premultiply) and alpha where the layout has it.
 */
static void
output_store(
	const struct webpc_layout *layout,
	uint8_t *pixel,
	unsigned red,
	unsigned green,
	unsigned blue,
	unsigned alpha)
{
	/* A premultiplied layout multiplies the colour by the alpha. */
	if (layout->premultiply) {
		red = webpc_output_premultiply(red, alpha);
		green = webpc_output_premultiply(green, alpha);
		blue = webpc_output_premultiply(blue, alpha);
	}

	/* The colour goes to the layout's offsets. */
	pixel[layout->red] = (uint8_t)red;
	pixel[layout->green] = (uint8_t)green;
	pixel[layout->blue] = (uint8_t)blue;

	/* A layout without alpha leaves it out. */
	if (layout->alpha >= 0)
		pixel[layout->alpha] = (uint8_t)alpha;
}
