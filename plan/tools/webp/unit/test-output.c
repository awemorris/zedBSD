/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The tests of output.c: the YUV to RGB conversion against values worked
 * out by hand from output.h's formula (and against a division-based
 * transcription of the formula over every input), the premultiplication
 * over every component and alpha, the lossy output with the chroma of
 * small images worked out by hand (fancy and not, every layout, with and
 * without an alpha plane, flipped, the padding of rows left alone), and the
 * lossless output into every layout, with random images checked against
 * transcriptions of the rules kept in the test.
 */

#include "check.h"
#include "output.h"

#include <stdlib.h>
#include <string.h>

/* The number of layouts the tests cover. */
#define LAYOUT_COUNT	8U

/* The fill of output bytes the conversion must not write (row padding). */
#define UNTOUCHED	0xA5U

/* The byte layouts: RGB, RGBA, BGR, BGRA, ARGB, rgbA, bgrA, Argb. */
static const struct webpc_layout layouts[LAYOUT_COUNT] = {
	{ 3, 0, 1, 2, -1, 0 },
	{ 4, 0, 1, 2, 3, 0 },
	{ 3, 2, 1, 0, -1, 0 },
	{ 4, 2, 1, 0, 3, 0 },
	{ 4, 1, 2, 3, 0, 0 },
	{ 4, 0, 1, 2, 3, 1 },
	{ 4, 2, 1, 0, 3, 1 },
	{ 4, 1, 2, 3, 0, 1 }
};

/* The random generator's state. */
static unsigned long random_state = 0x2468ace1UL;

static void reference_yuv(unsigned y, unsigned u, unsigned v, unsigned rgb[3]);
static unsigned reference_clip(long total);
static unsigned reference_premultiply(unsigned x, unsigned a);
static long reference_clamp_index(long index, long size);
static unsigned reference_chroma(const uint8_t *plane, unsigned plane_width, unsigned plane_height, unsigned x,
	unsigned y, int fancy);
static uint8_t *make_plane(const uint8_t *tight, unsigned width, unsigned height, ptrdiff_t stride);
static void expected_bytes(const struct webpc_layout *layout, unsigned red, unsigned green, unsigned blue,
	unsigned alpha, unsigned char out[4]);
static void check_output(const struct webpc_layout *layout, const uint8_t *buffer, ptrdiff_t stride, unsigned width,
	unsigned height, int flip, const unsigned *colour, const unsigned *alpha);
static void run_yuv_case(unsigned width, unsigned height, const uint8_t *y_tight, const uint8_t *u_tight,
	const uint8_t *v_tight, const uint8_t *alpha_tight, int fancy, const struct webpc_layout *layout, int flip,
	const uint8_t *expect_u, const uint8_t *expect_v);
static void run_argb_case(unsigned width, unsigned height, const uint32_t *pixels, const struct webpc_layout *layout,
	int flip);
static void test_yuv_pixel_hand(void);
static void test_yuv_pixel_all(void);
static void test_premultiply(void);
static void test_from_yuv_hand(void);
static void test_from_yuv_random(void);
static void test_from_argb_hand(void);
static void test_from_argb_random(void);

/*
 * Runs the tests of output.c.
 */
int
main(
	void)
{
	int status;

	/* Each helper in turn. */
	test_yuv_pixel_hand();
	test_yuv_pixel_all();
	test_premultiply();
	test_from_yuv_hand();
	test_from_yuv_random();
	test_from_argb_hand();
	test_from_argb_random();

	/* The summary line. */
	status = check_finish("output");
	return status;
}

/*
 * Computes the conversion of output.h with wide signed integers and
 * division, in place of the shifts of the implementation.
 */
static void
reference_yuv(
	unsigned y,
	unsigned u,
	unsigned v,
	unsigned rgb[3])
{
	long luma;

	/* mul(x, k) is x * k divided by 256, rounded down (the values are not negative). */
	luma = (long)y * 19077L / 256L;
	rgb[0] = reference_clip(luma + (long)v * 26149L / 256L - 14234L);
	rgb[1] = reference_clip(luma - (long)u * 6419L / 256L - (long)v * 13320L / 256L + 8708L);
	rgb[2] = reference_clip(luma + (long)u * 33050L / 256L - 17685L);
}

/*
 * Clips a sum as output.h says: 0 below zero, 255 above, else the sum
 * divided by 64.
 */
static unsigned
reference_clip(
	long total)
{
	/* Below zero is 0. */
	if (total < 0L)
		return 0U;

	/* The quotient above 255 is 255. */
	if (total / 64L > 255L)
		return 255U;

	/* Reports the quotient. */
	return (unsigned)(total / 64L);
}

/*
 * Multiplies a component by alpha as the floor of the product over 255.
 */
static unsigned
reference_premultiply(
	unsigned x,
	unsigned a)
{
	long product;

	/* The product is exact in a long. */
	product = (long)x * (long)a;

	/* Reports the floor of the quotient. */
	return (unsigned)(product / 255L);
}

/*
 * Clamps a signed index into 0..size - 1.
 */
static long
reference_clamp_index(
	long index,
	long size)
{
	/* Below the plane is the first sample. */
	if (index < 0L)
		return 0L;

	/* Past the plane is the last sample. */
	if (index > size - 1L)
		return size - 1L;

	/* Reports the index. */
	return index;
}

/*
 * Gives the chroma of a pixel from a tightly packed chroma plane by the
 * rule of output.h, with signed indexes.
 */
static unsigned
reference_chroma(
	const uint8_t *plane,
	unsigned plane_width,
	unsigned plane_height,
	unsigned x,
	unsigned y,
	int fancy)
{
	long i;
	long j;
	long i_side;
	long j_side;
	long width;
	long height;
	long near_sample;
	long horizontal;
	long vertical;
	long diagonal;

	/* The pixel's own block. */
	i = (long)(x / 2U);
	j = (long)(y / 2U);
	width = (long)plane_width;
	height = (long)plane_height;

	/* Without fancy upsampling the block's sample is the chroma. */
	if (!fancy)
		return plane[j * width + i];

	/* The neighbours on the side the pixel is nearer to. */
	i_side = i + 1L;
	if (x % 2U == 0U)
		i_side = i - 1L;
	j_side = j + 1L;
	if (y % 2U == 0U)
		j_side = j - 1L;
	i_side = reference_clamp_index(i_side, width);
	j_side = reference_clamp_index(j_side, height);

	/* The four samples with their weights 9, 3, 3 and 1. */
	near_sample = plane[j * width + i];
	horizontal = plane[j * width + i_side];
	vertical = plane[j_side * width + i];
	diagonal = plane[j_side * width + i_side];

	/* Reports the weighted mean with the rounding of 8 and the division by 16. */
	return (unsigned)((9L * near_sample + 3L * horizontal + 3L * vertical + diagonal + 8L) / 16L);
}

/*
 * Copies a tight plane into a new allocation of exactly the size the rows
 * and the stride need, the padding between rows filled with 0xEE.
 */
static uint8_t *
make_plane(
	const uint8_t *tight,
	unsigned width,
	unsigned height,
	ptrdiff_t stride)
{
	uint8_t *plane;
	unsigned row;
	size_t size;

	/* The last row has no padding after it, so ASan sees a read beyond it. */
	size = (size_t)stride * (size_t)(height - 1U) + (size_t)width;
	plane = (uint8_t *)malloc(size);
	CHECK(plane != NULL);
	if (plane == NULL)
		exit(1);
	memset(plane, 0xEE, size);

	/* Copies each row to its place. */
	for (row = 0; row < height; row++)
		memcpy(plane + (size_t)stride * (size_t)row, tight + (size_t)width * (size_t)row, width);

	return plane;
}

/*
 * Places a pixel's four components in the bytes of a layout (the colour
 * already premultiplied by the caller when the layout asks for it).
 */
static void
expected_bytes(
	const struct webpc_layout *layout,
	unsigned red,
	unsigned green,
	unsigned blue,
	unsigned alpha,
	unsigned char out[4])
{
	/* The colour at the layout's offsets. */
	out[layout->red] = (unsigned char)red;
	out[layout->green] = (unsigned char)green;
	out[layout->blue] = (unsigned char)blue;

	/* Alpha where the layout has it. */
	if (layout->alpha >= 0)
		out[layout->alpha] = (unsigned char)alpha;
}

/*
 * Checks an output buffer of rows stride bytes apart: each pixel's bytes
 * (colour holds red, green and blue of every pixel in image order, alpha
 * its alpha, both as the conversion should receive them before the
 * layout's premultiplication) and the untouched padding after each row.
 */
static void
check_output(
	const struct webpc_layout *layout,
	const uint8_t *buffer,
	ptrdiff_t stride,
	unsigned width,
	unsigned height,
	int flip,
	const unsigned *colour,
	const unsigned *alpha)
{
	unsigned x;
	unsigned y;
	unsigned k;
	unsigned buffer_row;
	unsigned red;
	unsigned green;
	unsigned blue;
	unsigned pixel_alpha;
	unsigned char expected[4];
	const uint8_t *row_start;

	/* Checks every row, image row y being at the buffer's row y, or the mirrored one when flipped. */
	for (y = 0; y < height; y++) {
		buffer_row = y;
		if (flip)
			buffer_row = height - 1U - y;
		row_start = buffer + (size_t)stride * (size_t)buffer_row;

		/* Checks every pixel of the row. */
		for (x = 0; x < width; x++) {
			red = colour[((size_t)y * width + x) * 3U];
			green = colour[((size_t)y * width + x) * 3U + 1U];
			blue = colour[((size_t)y * width + x) * 3U + 2U];
			pixel_alpha = alpha[(size_t)y * width + x];
			if (layout->premultiply) {
				red = reference_premultiply(red, pixel_alpha);
				green = reference_premultiply(green, pixel_alpha);
				blue = reference_premultiply(blue, pixel_alpha);
			}
			expected_bytes(layout, red, green, blue, pixel_alpha, expected);
			for (k = 0; k < layout->bytes; k++)
				CHECK(row_start[(size_t)x * layout->bytes + k] == expected[k]);
		}

		/* The padding after the pixels is not written. */
		for (k = width * layout->bytes; k < (unsigned)stride; k++)
			CHECK(row_start[k] == UNTOUCHED);
	}
}

/*
 * Converts one lossy image (tight planes; expect_u and expect_v are the
 * chroma each pixel should get, worked out by the caller) into a layout
 * and checks every byte.  The planes get strides larger than their widths.
 */
static void
run_yuv_case(
	unsigned width,
	unsigned height,
	const uint8_t *y_tight,
	const uint8_t *u_tight,
	const uint8_t *v_tight,
	const uint8_t *alpha_tight,
	int fancy,
	const struct webpc_layout *layout,
	int flip,
	const uint8_t *expect_u,
	const uint8_t *expect_v)
{
	struct webpc_yuv_view view;
	unsigned plane_width;
	unsigned plane_height;
	unsigned x;
	unsigned y;
	unsigned rgb[3];
	unsigned *colour;
	unsigned *alpha;
	ptrdiff_t out_stride;
	ptrdiff_t alpha_stride;
	size_t out_size;
	uint8_t *y_plane;
	uint8_t *u_plane;
	uint8_t *v_plane;
	uint8_t *alpha_plane;
	uint8_t *buffer;
	uint8_t *first_row;
	ptrdiff_t row_step;

	/* The chroma plane size and the padded planes. */
	plane_width = (width + 1U) / 2U;
	plane_height = (height + 1U) / 2U;
	view.width = width;
	view.height = height;
	view.y_stride = (ptrdiff_t)width + 3;
	view.uv_stride = (ptrdiff_t)plane_width + 2;
	y_plane = make_plane(y_tight, width, height, view.y_stride);
	u_plane = make_plane(u_tight, plane_width, plane_height, view.uv_stride);
	v_plane = make_plane(v_tight, plane_width, plane_height, view.uv_stride);
	view.y = y_plane;
	view.u = u_plane;
	view.v = v_plane;
	alpha_stride = (ptrdiff_t)width + 2;
	alpha_plane = NULL;
	if (alpha_tight != NULL)
		alpha_plane = make_plane(alpha_tight, width, height, alpha_stride);

	/* The expected colour of each pixel and its alpha. */
	colour = (unsigned *)malloc((size_t)width * height * 3U * sizeof(unsigned));
	alpha = (unsigned *)malloc((size_t)width * height * sizeof(unsigned));
	CHECK(colour != NULL && alpha != NULL);
	if (colour == NULL || alpha == NULL)
		exit(1);
	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			reference_yuv(y_tight[(size_t)y * width + x], expect_u[(size_t)y * width + x],
				expect_v[(size_t)y * width + x], rgb);
			colour[((size_t)y * width + x) * 3U] = rgb[0];
			colour[((size_t)y * width + x) * 3U + 1U] = rgb[1];
			colour[((size_t)y * width + x) * 3U + 2U] = rgb[2];
			alpha[(size_t)y * width + x] = 255U;
			if (alpha_tight != NULL)
				alpha[(size_t)y * width + x] = alpha_tight[(size_t)y * width + x];
		}
	}

	/* The output buffer is exactly the rows with their padding of 5 bytes. */
	out_stride = (ptrdiff_t)(width * layout->bytes + 5U);
	out_size = (size_t)out_stride * (size_t)height;
	buffer = (uint8_t *)malloc(out_size);
	CHECK(buffer != NULL);
	if (buffer == NULL)
		exit(1);
	memset(buffer, (int)UNTOUCHED, out_size);

	/* A flipped output starts at the last row and steps up. */
	first_row = buffer;
	row_step = out_stride;
	if (flip) {
		first_row = buffer + out_stride * (ptrdiff_t)(height - 1U);
		row_step = -out_stride;
	}

	/* Converts and checks. */
	webpc_output_from_yuv(&view, alpha_plane, alpha_stride, fancy, layout, first_row, row_step);
	check_output(layout, buffer, out_stride, width, height, flip, colour, alpha);

	/* Releases the case's memory. */
	free(buffer);
	free(alpha);
	free(colour);
	free(alpha_plane);
	free(v_plane);
	free(u_plane);
	free(y_plane);
}

/*
 * Converts one lossless image into a layout and checks every byte.
 */
static void
run_argb_case(
	unsigned width,
	unsigned height,
	const uint32_t *pixels,
	const struct webpc_layout *layout,
	int flip)
{
	unsigned x;
	unsigned y;
	unsigned *colour;
	unsigned *alpha;
	ptrdiff_t out_stride;
	size_t out_size;
	uint8_t *buffer;
	uint8_t *first_row;
	ptrdiff_t row_step;
	uint32_t *copy;

	/* The expected components of each pixel. */
	colour = (unsigned *)malloc((size_t)width * height * 3U * sizeof(unsigned));
	alpha = (unsigned *)malloc((size_t)width * height * sizeof(unsigned));
	copy = (uint32_t *)malloc((size_t)width * height * sizeof(uint32_t));
	CHECK(colour != NULL && alpha != NULL && copy != NULL);
	if (colour == NULL || alpha == NULL || copy == NULL)
		exit(1);
	memcpy(copy, pixels, (size_t)width * height * sizeof(uint32_t));
	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			colour[((size_t)y * width + x) * 3U] = (unsigned)((pixels[(size_t)y * width + x] >> 16) & 0xffUL);
			colour[((size_t)y * width + x) * 3U + 1U] = (unsigned)((pixels[(size_t)y * width + x] >> 8) & 0xffUL);
			colour[((size_t)y * width + x) * 3U + 2U] = (unsigned)(pixels[(size_t)y * width + x] & 0xffUL);
			alpha[(size_t)y * width + x] = (unsigned)((pixels[(size_t)y * width + x] >> 24) & 0xffUL);
		}
	}

	/* The output buffer is exactly the rows with their padding of 5 bytes. */
	out_stride = (ptrdiff_t)(width * layout->bytes + 5U);
	out_size = (size_t)out_stride * (size_t)height;
	buffer = (uint8_t *)malloc(out_size);
	CHECK(buffer != NULL);
	if (buffer == NULL)
		exit(1);
	memset(buffer, (int)UNTOUCHED, out_size);
	first_row = buffer;
	row_step = out_stride;
	if (flip) {
		first_row = buffer + out_stride * (ptrdiff_t)(height - 1U);
		row_step = -out_stride;
	}

	/* Converts, checks, and checks that the source is unchanged. */
	webpc_output_from_argb(copy, width, height, layout, first_row, row_step);
	check_output(layout, buffer, out_stride, width, height, flip, colour, alpha);
	CHECK(memcmp(copy, pixels, (size_t)width * height * sizeof(uint32_t)) == 0);

	/* Releases the case's memory. */
	free(buffer);
	free(copy);
	free(alpha);
	free(colour);
}

/*
 * Checks yuv_pixel against values worked out by hand from the formula,
 * both ends of each clip included.
 *
 * (16,128,128): mul(16,19077) = 1192, mul(128,26149) = 13074,
 * mul(128,6419) = 3209, mul(128,13320) = 6660, mul(128,33050) = 16525;
 * red 1192 + 13074 - 14234 = 32 and green 1192 - 3209 - 6660 + 8708 = 31
 * and blue 1192 + 16525 - 17685 = 32, each >> 6 = 0.
 * (235,128,128): mul = 17512, so 16352 (red, blue) and 16351 (green),
 * each >> 6 = 255.  (0,0,0): green 8708 >> 6 = 136, red and blue below 0.
 * (255,255,255): mul(y) = 19002, 26046, 6393, 13267, 32920; red 30814 and
 * blue 34237 clip to 255, green 8050 >> 6 = 125.  (81,90,240): 6036, 24514,
 * 2256, 12487, 11619; red 16316 >> 6 = 254, green 1 >> 6 = 0, blue -30
 * clips to 0.  (128,128,128): 9538; 8378, 8377, 8378 >> 6 = 130.
 */
static void
test_yuv_pixel_hand(void)
{
	uint8_t rgb[3];

	/* Black at the bottom of the limited range. */
	webpc_output_yuv_pixel(16U, 128U, 128U, rgb);
	CHECK(rgb[0] == 0 && rgb[1] == 0 && rgb[2] == 0);

	/* White at the top of the limited range. */
	webpc_output_yuv_pixel(235U, 128U, 128U, rgb);
	CHECK(rgb[0] == 255 && rgb[1] == 255 && rgb[2] == 255);

	/* A saturated red. */
	webpc_output_yuv_pixel(81U, 90U, 240U, rgb);
	CHECK(rgb[0] == 254 && rgb[1] == 0 && rgb[2] == 0);

	/* The zero input: red and blue clip low, green keeps its offset. */
	webpc_output_yuv_pixel(0U, 0U, 0U, rgb);
	CHECK(rgb[0] == 0 && rgb[1] == 136 && rgb[2] == 0);

	/* The largest input: red and blue clip high. */
	webpc_output_yuv_pixel(255U, 255U, 255U, rgb);
	CHECK(rgb[0] == 255 && rgb[1] == 125 && rgb[2] == 255);

	/* A mid grey. */
	webpc_output_yuv_pixel(128U, 128U, 128U, rgb);
	CHECK(rgb[0] == 130 && rgb[1] == 130 && rgb[2] == 130);
}

/*
 * Checks yuv_pixel over every Y', Cb and Cr against the division-based
 * transcription.
 */
static void
test_yuv_pixel_all(void)
{
	unsigned y;
	unsigned u;
	unsigned v;
	unsigned expected[3];
	uint8_t rgb[3];
	unsigned long wrong;

	/* Every input; wrong counts the mismatches so one check reports the sweep. */
	wrong = 0UL;
	for (y = 0; y < 256U; y++) {
		for (u = 0; u < 256U; u++) {
			for (v = 0; v < 256U; v++) {
				webpc_output_yuv_pixel(y, u, v, rgb);
				reference_yuv(y, u, v, expected);
				if (rgb[0] != expected[0] || rgb[1] != expected[1] || rgb[2] != expected[2])
					wrong++;
			}
		}
	}
	CHECK(wrong == 0UL);
}

/*
 * Checks premultiply over every component and alpha: the floor of x * a
 * over 255, x for alpha 255, 0 for alpha 0 or x 0.
 */
static void
test_premultiply(void)
{
	unsigned x;
	unsigned a;
	unsigned long wrong;

	/* Every pair against the floor of x * a / 255. */
	wrong = 0UL;
	for (x = 0; x < 256U; x++) {
		for (a = 0; a < 256U; a++) {
			if (webpc_output_premultiply(x, a) != reference_premultiply(x, a))
				wrong++;
		}
	}
	CHECK(wrong == 0UL);

	/* Opaque keeps the colour, transparent and black give 0. */
	wrong = 0UL;
	for (x = 0; x < 256U; x++) {
		if (webpc_output_premultiply(x, 255U) != x)
			wrong++;
		if (webpc_output_premultiply(x, 0U) != 0U)
			wrong++;
		if (webpc_output_premultiply(0U, x) != 0U)
			wrong++;
	}
	CHECK(wrong == 0UL);

	/* Values worked out by hand: 255 * 128 / 255 = 128, 200 * 128 / 255 = 100, 1 * 254 / 255 = 0, 254 * 254 / 255 = 253. */
	CHECK(webpc_output_premultiply(255U, 128U) == 128U);
	CHECK(webpc_output_premultiply(200U, 128U) == 100U);
	CHECK(webpc_output_premultiply(1U, 254U) == 0U);
	CHECK(webpc_output_premultiply(254U, 254U) == 253U);
}

/*
 * Checks the lossy output on small images whose chroma was worked out by
 * hand with (9 * near + 3 * horizontal + 3 * vertical + diagonal + 8) >> 4.
 *
 * 3x3 with chroma 2x2, u = {0,160 / 80,240}: pixel (1,0) has near 0,
 * horizontal 160, vertical 0 (clamped), diagonal 160: (0 + 480 + 0 + 160
 * + 8) >> 4 = 40; (2,0) has near 160, horizontal 0, vertical 160, diagonal
 * 0: (1440 + 0 + 480 + 0 + 8) >> 4 = 120; (1,1) has 0, 160, 80, 240: (0 +
 * 480 + 240 + 240 + 8) >> 4 = 60; and so on, the whole tables below.
 * 4x2 with chroma 2x1 has the vertical neighbours clamped to the row
 * itself.  1x1 has the one sample for every weight.
 */
static void
test_from_yuv_hand(void)
{
	static const uint8_t y3[9] = { 16, 50, 90, 130, 170, 210, 235, 200, 100 };
	static const uint8_t u3[4] = { 0, 160, 80, 240 };
	static const uint8_t v3[4] = { 255, 16, 128, 64 };
	static const uint8_t alpha3[9] = { 0, 255, 128, 1, 254, 77, 200, 10, 255 };
	static const uint8_t fancy_u3[9] = { 0, 40, 120, 20, 60, 140, 60, 100, 180 };
	static const uint8_t fancy_v3[9] = { 255, 195, 76, 223, 174, 77, 160, 133, 79 };
	static const uint8_t plain_u3[9] = { 0, 0, 160, 0, 0, 160, 80, 80, 240 };
	static const uint8_t plain_v3[9] = { 255, 255, 16, 255, 255, 16, 128, 128, 64 };
	static const uint8_t y4[8] = { 20, 60, 100, 140, 180, 220, 255, 0 };
	static const uint8_t u4[2] = { 100, 200 };
	static const uint8_t v4[2] = { 40, 250 };
	static const uint8_t alpha4[8] = { 255, 0, 100, 200, 17, 255, 128, 64 };
	static const uint8_t fancy_u4[8] = { 100, 125, 175, 200, 100, 125, 175, 200 };
	static const uint8_t fancy_v4[8] = { 40, 93, 198, 250, 40, 93, 198, 250 };
	static const uint8_t plain_u4[8] = { 100, 100, 200, 200, 100, 100, 200, 200 };
	static const uint8_t plain_v4[8] = { 40, 40, 250, 250, 40, 40, 250, 250 };
	static const uint8_t y1[1] = { 100 };
	static const uint8_t u1[1] = { 77 };
	static const uint8_t v1[1] = { 200 };
	static const uint8_t alpha1[1] = { 99 };
	unsigned layout_index;
	unsigned flip;
	unsigned with_alpha;
	unsigned fancy;

	/* Every layout, flipped or not, with an alpha plane or without, fancy or not. */
	for (layout_index = 0; layout_index < LAYOUT_COUNT; layout_index++) {
		for (flip = 0; flip < 2U; flip++) {
			for (with_alpha = 0; with_alpha < 2U; with_alpha++) {
				for (fancy = 0; fancy < 2U; fancy++) {
					run_yuv_case(3U, 3U, y3, u3, v3, with_alpha ? alpha3 : NULL, (int)fancy,
						&layouts[layout_index], (int)flip, fancy ? fancy_u3 : plain_u3,
						fancy ? fancy_v3 : plain_v3);
					run_yuv_case(4U, 2U, y4, u4, v4, with_alpha ? alpha4 : NULL, (int)fancy,
						&layouts[layout_index], (int)flip, fancy ? fancy_u4 : plain_u4,
						fancy ? fancy_v4 : plain_v4);
					run_yuv_case(1U, 1U, y1, u1, v1, with_alpha ? alpha1 : NULL, (int)fancy,
						&layouts[layout_index], (int)flip, u1, v1);
				}
			}
		}
	}
}

/*
 * Checks the lossy output on random images (sizes 1..19, random planes and
 * alpha) against the transcription of the chroma rule, in every layout,
 * flipped or not, fancy or not.
 */
static void
test_from_yuv_random(void)
{
	unsigned round;
	unsigned width;
	unsigned height;
	unsigned plane_width;
	unsigned plane_height;
	unsigned x;
	unsigned y;
	unsigned fancy;
	unsigned flip;
	unsigned layout_index;
	size_t index;
	uint8_t *y_tight;
	uint8_t *u_tight;
	uint8_t *v_tight;
	uint8_t *alpha_tight;
	uint8_t *expect_u;
	uint8_t *expect_v;

	/* Many random images. */
	for (round = 0; round < 400U; round++) {
		/* The size and the random planes. */
		width = 1U + (unsigned)(check_random(&random_state) % 19UL);
		height = 1U + (unsigned)(check_random(&random_state) % 19UL);
		plane_width = (width + 1U) / 2U;
		plane_height = (height + 1U) / 2U;
		y_tight = (uint8_t *)malloc((size_t)width * height);
		alpha_tight = (uint8_t *)malloc((size_t)width * height);
		expect_u = (uint8_t *)malloc((size_t)width * height);
		expect_v = (uint8_t *)malloc((size_t)width * height);
		u_tight = (uint8_t *)malloc((size_t)plane_width * plane_height);
		v_tight = (uint8_t *)malloc((size_t)plane_width * plane_height);
		CHECK(y_tight != NULL && alpha_tight != NULL && expect_u != NULL && expect_v != NULL && u_tight != NULL &&
			v_tight != NULL);
		if (y_tight == NULL || alpha_tight == NULL || expect_u == NULL || expect_v == NULL || u_tight == NULL ||
		    v_tight == NULL)
			exit(1);
		for (index = 0; index < (size_t)width * height; index++) {
			y_tight[index] = (uint8_t)check_random(&random_state);
			alpha_tight[index] = (uint8_t)check_random(&random_state);
		}
		for (index = 0; index < (size_t)plane_width * plane_height; index++) {
			u_tight[index] = (uint8_t)check_random(&random_state);
			v_tight[index] = (uint8_t)check_random(&random_state);
		}

		/* The chroma each pixel should get, for fancy and not. */
		for (fancy = 0; fancy < 2U; fancy++) {
			for (y = 0; y < height; y++) {
				for (x = 0; x < width; x++) {
					expect_u[(size_t)y * width + x] =
						(uint8_t)reference_chroma(u_tight, plane_width, plane_height, x, y, (int)fancy);
					expect_v[(size_t)y * width + x] =
						(uint8_t)reference_chroma(v_tight, plane_width, plane_height, x, y, (int)fancy);
				}
			}

			/* A random layout, flip and alpha choice for this chroma mode. */
			layout_index = (unsigned)(check_random(&random_state) % LAYOUT_COUNT);
			flip = (unsigned)(check_random(&random_state) % 2UL);
			run_yuv_case(width, height, y_tight, u_tight, v_tight,
				(check_random(&random_state) % 2UL) ? alpha_tight : NULL, (int)fancy,
				&layouts[layout_index], (int)flip, expect_u, expect_v);
		}

		/* Releases the round's memory. */
		free(v_tight);
		free(u_tight);
		free(expect_v);
		free(expect_u);
		free(alpha_tight);
		free(y_tight);
	}
}

/*
 * Checks the lossless output: the bytes of one pixel worked out by hand in
 * each layout, then small images in every layout, flipped or not.
 */
static void
test_from_argb_hand(void)
{
	/* One pixel, alpha 0x80, red 0xC8, green 0x40, blue 0x10; premultiplied: 100, 32, 8 (x * 128 / 255). */
	static const uint32_t one[1] = { 0x80C84010UL };
	static const unsigned char want[LAYOUT_COUNT][4] = {
		{ 0xC8, 0x40, 0x10, 0x00 },
		{ 0xC8, 0x40, 0x10, 0x80 },
		{ 0x10, 0x40, 0xC8, 0x00 },
		{ 0x10, 0x40, 0xC8, 0x80 },
		{ 0x80, 0xC8, 0x40, 0x10 },
		{ 0x64, 0x20, 0x08, 0x80 },
		{ 0x08, 0x20, 0x64, 0x80 },
		{ 0x80, 0x64, 0x20, 0x08 }
	};
	static const uint32_t three_by_two[6] = {
		0xFF000000UL, 0x00FFFFFFUL, 0x80808080UL, 0x01FF0001UL, 0xFEFEFEFEUL, 0x7F123456UL
	};
	unsigned layout_index;
	unsigned flip;
	unsigned k;
	uint8_t buffer[4];
	uint32_t copy[1];

	/* The one pixel's literal bytes in each layout. */
	for (layout_index = 0; layout_index < LAYOUT_COUNT; layout_index++) {
		memset(buffer, 0, sizeof(buffer));
		copy[0] = one[0];
		webpc_output_from_argb(copy, 1U, 1U, &layouts[layout_index], buffer, 4);
		for (k = 0; k < layouts[layout_index].bytes; k++)
			CHECK(buffer[k] == want[layout_index][k]);
		if (layouts[layout_index].bytes == 3U)
			CHECK(buffer[3] == 0);
	}

	/* A 3x2 image and the one pixel in every layout, flipped or not. */
	for (layout_index = 0; layout_index < LAYOUT_COUNT; layout_index++) {
		for (flip = 0; flip < 2U; flip++) {
			run_argb_case(3U, 2U, three_by_two, &layouts[layout_index], (int)flip);
			run_argb_case(1U, 1U, one, &layouts[layout_index], (int)flip);
			run_argb_case(6U, 1U, three_by_two, &layouts[layout_index], (int)flip);
			run_argb_case(1U, 6U, three_by_two, &layouts[layout_index], (int)flip);
		}
	}
}

/*
 * Checks the lossless output on random images in every layout.
 */
static void
test_from_argb_random(void)
{
	unsigned round;
	unsigned width;
	unsigned height;
	unsigned layout_index;
	unsigned flip;
	size_t index;
	uint32_t *pixels;

	/* Many random images. */
	for (round = 0; round < 400U; round++) {
		/* The size and the random pixels. */
		width = 1U + (unsigned)(check_random(&random_state) % 19UL);
		height = 1U + (unsigned)(check_random(&random_state) % 19UL);
		pixels = (uint32_t *)malloc((size_t)width * height * sizeof(uint32_t));
		CHECK(pixels != NULL);
		if (pixels == NULL)
			exit(1);
		for (index = 0; index < (size_t)width * height; index++)
			pixels[index] = (uint32_t)(check_random(&random_state) & 0xffffffffUL);

		/* A random layout and flip. */
		layout_index = (unsigned)(check_random(&random_state) % LAYOUT_COUNT);
		flip = (unsigned)(check_random(&random_state) % 2UL);
		run_argb_case(width, height, pixels, &layouts[layout_index], (int)flip);

		/* Releases the round's memory. */
		free(pixels);
	}
}
