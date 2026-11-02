/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The tests of vp8l.c: the header, and whole lossless streams written with
 * the lbitwriter from RFC 9649 3.4, 3.5 and 3.8.  A test starts from an
 * image of its own, applies the forward transforms (written here from the
 * RFC's descriptions of what the inverse undoes), writes the transform data
 * and the transformed image, and expects the decoder to give the starting
 * image back.  The entropy-coded images are written with flat prefix codes
 * (every literal channel value an 8-bit codeword), so that any pixel can be
 * written.
 */

#include "check.h"
#include "lbitwriter.h"
#include "vp8l.h"
#include "vp8l-transform.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The most pixels of a test image (also of its subresolution and packed forms). */
#define TEST_PIXELS_MAX		4096U

/* The kinds of the test transforms, as the type field of the stream numbers them. */
#define TX_PREDICTOR		0U
#define TX_COLOR		1U
#define TX_SUBTRACT_GREEN	2U
#define TX_INDEXING		3U

/* The stream a test writes. */
static struct lbitwriter stream;

/* The image the test starts from, the image after the transforms so far, a scratch image and the decoder's output. */
static uint32_t original[TEST_PIXELS_MAX];
static uint32_t current[TEST_PIXELS_MAX];
static uint32_t scratch[TEST_PIXELS_MAX];
static uint32_t decoded[TEST_PIXELS_MAX];

/* The subresolution image of the transform being written. */
static uint32_t sub_image[TEST_PIXELS_MAX];

/* The width of the image after the transforms written so far (colour indexing narrows it). */
static unsigned current_width;

/* The generator of the random values of the tests. */
static unsigned long test_state = 0x2545f491UL;

/* The order the code length code's lengths are written in (RFC 9649 3.7.2.1.2). */
static const uint8_t test_order[12] = { 17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8 };

static void test_header(void);
static void test_no_transform(void);
static void test_single_transforms(void);
static void test_indexing_sizes(void);
static void test_indexing_out_of_range(void);
static void test_combinations(void);
static void test_duplicates(void);
static void test_truncated(void);
static void test_random_streams(void);
static void test_decode_whole(void);
static unsigned long next_random(void);
static uint32_t random_pixel(void);
static void put_normal_code(struct lbitwriter *writer, int green);
static void put_image(struct lbitwriter *writer, const uint32_t *pixels, unsigned width, unsigned height, int role);
static void put_stream_header(struct lbitwriter *writer, unsigned width, unsigned height);
static unsigned subsample(unsigned size, unsigned bits);
static uint32_t channel(uint32_t pixel, unsigned index);
static uint32_t make_pixel(uint32_t alpha, uint32_t red, uint32_t green, uint32_t blue);
static uint32_t sub_pixels(uint32_t first, uint32_t second);
static uint32_t average2(uint32_t first, uint32_t second);
static int clamp(int value);
static uint32_t predict(unsigned mode, uint32_t left, uint32_t top, uint32_t top_right, uint32_t top_left);
static int floor_div32(int value);
static int color_delta(uint32_t t, uint32_t c);
static void forward_predictor(uint32_t *pixels, unsigned width, unsigned height, const uint32_t *modes, unsigned size_bits);
static void forward_color(uint32_t *pixels, unsigned width, unsigned height, const uint32_t *elements, unsigned size_bits);
static void forward_subtract_green(uint32_t *pixels, size_t count);
static unsigned forward_indexing(uint32_t *pixels, unsigned width, unsigned height, uint32_t *table);
static unsigned width_bits_of(unsigned colors);
static void write_transform(unsigned type, unsigned height);
static void end_transforms(unsigned height);
static void start_image(unsigned width, unsigned height, const uint32_t *pixels);
static int decode_current(unsigned width, unsigned height);
static void random_image(unsigned width, unsigned height, unsigned colors);

/*
 * Runs the tests of vp8l.c.
 */
int
main(
	void)
{
	int status;

	/* The header, then streams from simple to complete. */
	test_header();
	test_no_transform();
	test_single_transforms();
	test_indexing_sizes();
	test_indexing_out_of_range();
	test_combinations();
	test_duplicates();
	test_truncated();
	test_random_streams();
	test_decode_whole();

	/* The summary line. */
	status = check_finish("vp8l");
	return status;
}

/* Gives the next random value (32 bits). */
static unsigned long
next_random(void)
{
	/* One step of the shared generator. */
	return check_random(&test_state);
}

/* Gives a random ARGB pixel. */
static uint32_t
random_pixel(void)
{
	uint32_t pixel;

	/* Two draws of 16 bits each make the 32 bits. */
	pixel = (uint32_t)(next_random() & 0xffffUL) << 16;
	pixel |= (uint32_t)(next_random() & 0xffffUL);
	return pixel;
}

/* Gives the 8-bit channel (0 blue, 1 green, 2 red, 3 alpha) of a pixel. */
static uint32_t
channel(
	uint32_t pixel,
	unsigned index)
{
	/* The channel sits 8 * index bits up. */
	return (pixel >> (8U * index)) & 0xffU;
}

/* Builds a pixel from its channels. */
static uint32_t
make_pixel(
	uint32_t alpha,
	uint32_t red,
	uint32_t green,
	uint32_t blue)
{
	/* Alpha in the top byte, blue in the bottom one. */
	return ((alpha & 0xffU) << 24) | ((red & 0xffU) << 16) | ((green & 0xffU) << 8) | (blue & 0xffU);
}

/* Gives first - second per channel, modulo 256. */
static uint32_t
sub_pixels(
	uint32_t first,
	uint32_t second)
{
	return make_pixel(channel(first, 3U) - channel(second, 3U), channel(first, 2U) - channel(second, 2U),
		channel(first, 1U) - channel(second, 1U), channel(first, 0U) - channel(second, 0U));
}

/* Gives Average2 of two pixels per channel: floor((a + b) / 2). */
static uint32_t
average2(
	uint32_t first,
	uint32_t second)
{
	return make_pixel((channel(first, 3U) + channel(second, 3U)) / 2U, (channel(first, 2U) + channel(second, 2U)) / 2U,
		(channel(first, 1U) + channel(second, 1U)) / 2U, (channel(first, 0U) + channel(second, 0U)) / 2U);
}

/* Clamps to 0..255. */
static int
clamp(
	int value)
{
	if (value < 0)
		return 0;
	if (value > 255)
		return 255;
	return value;
}

/* Gives the prediction of Table 2 for modes 0..13, and for 14 and 15 the mode-0 prediction (the contract of the header). */
static uint32_t
predict(
	unsigned mode,
	uint32_t left,
	uint32_t top,
	uint32_t top_right,
	uint32_t top_left)
{
	unsigned index;
	int estimate[4];
	int distance_left;
	int distance_top;
	int first;
	int second;
	uint32_t half;
	uint32_t out[4];

	switch (mode) {
	case 1:
		return left;
	case 2:
		return top;
	case 3:
		return top_right;
	case 4:
		return top_left;
	case 5:
		return average2(average2(left, top_right), top);
	case 6:
		return average2(left, top_left);
	case 7:
		return average2(left, top);
	case 8:
		return average2(top_left, top);
	case 9:
		return average2(top, top_right);
	case 10:
		return average2(average2(left, top_left), average2(top, top_right));
	case 11:
		/* Select: whichever of L and T is nearer the estimate L + T - TL by the sum of absolute differences. */
		distance_left = 0;
		distance_top = 0;
		for (index = 0; index < 4U; index++) {
			estimate[index] = (int)channel(left, index) + (int)channel(top, index) - (int)channel(top_left, index);
			distance_left += abs(estimate[index] - (int)channel(left, index));
			distance_top += abs(estimate[index] - (int)channel(top, index));
		}
		if (distance_left < distance_top)
			return left;
		return top;
	case 12:
		for (index = 0; index < 4U; index++)
			out[index] = (uint32_t)clamp((int)channel(left, index) + (int)channel(top, index) - (int)channel(top_left, index));
		return make_pixel(out[3], out[2], out[1], out[0]);
	case 13:
		half = average2(left, top);
		for (index = 0; index < 4U; index++) {
			first = (int)channel(half, index);
			second = (int)channel(top_left, index);
			out[index] = (uint32_t)clamp(first + (first - second) / 2);
		}
		return make_pixel(out[3], out[2], out[1], out[0]);
	default:
		return 0xff000000U;
	}
}

/* Gives floor(value / 32). */
static int
floor_div32(
	int value)
{
	if (value >= 0)
		return value / 32;
	return -((-value + 31) / 32);
}

/* Gives the colour transform's delta (RFC 9649 3.5.2): the signed 8-bit t times the signed 8-bit c, shifted right 5. */
static int
color_delta(
	uint32_t t,
	uint32_t c)
{
	int signed_t;
	int signed_c;

	/* The bytes as signed values. */
	signed_t = (int)t;
	if (signed_t >= 128)
		signed_t -= 256;
	signed_c = (int)c;
	if (signed_c >= 128)
		signed_c -= 256;
	return floor_div32(signed_t * signed_c);
}

/* Applies the forward predictor transform: each pixel minus its prediction from the original neighbours. */
static void
forward_predictor(
	uint32_t *pixels,
	unsigned width,
	unsigned height,
	const uint32_t *modes,
	unsigned size_bits)
{
	unsigned x;
	unsigned y;
	unsigned mode;
	unsigned modes_width;
	uint32_t prediction;
	uint32_t left;
	uint32_t top;
	uint32_t top_right;
	uint32_t top_left;

	/* Every pixel in turn, predicted from the unmodified image (the decoder will have restored these). */
	modes_width = subsample(width, size_bits);
	memcpy(scratch, pixels, (size_t)width * height * sizeof(uint32_t));
	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			if (x == 0U && y == 0U) {
				prediction = 0xff000000U;
			} else if (y == 0U) {
				prediction = scratch[x - 1U];
			} else if (x == 0U) {
				prediction = scratch[(size_t)(y - 1U) * width];
			} else {
				mode = (channel(modes[(size_t)(y >> size_bits) * modes_width + (x >> size_bits)], 1U)) & 15U;
				left = scratch[(size_t)y * width + x - 1U];
				top = scratch[(size_t)(y - 1U) * width + x];
				top_left = scratch[(size_t)(y - 1U) * width + x - 1U];
				if (x == width - 1U)
					top_right = scratch[(size_t)y * width];
				else
					top_right = scratch[(size_t)(y - 1U) * width + x + 1U];
				prediction = predict(mode, left, top, top_right, top_left);
			}
			pixels[(size_t)y * width + x] = sub_pixels(scratch[(size_t)y * width + x], prediction);
		}
	}
}

/* Applies the forward colour transform (RFC 9649 3.5.2): the red and blue minus the correlated parts of green and red. */
static void
forward_color(
	uint32_t *pixels,
	unsigned width,
	unsigned height,
	const uint32_t *elements,
	unsigned size_bits)
{
	unsigned x;
	unsigned y;
	unsigned elements_width;
	uint32_t element;
	uint32_t pixel;
	uint32_t red;
	uint32_t blue;
	uint32_t green;

	/* Every pixel with the element of its block. */
	elements_width = subsample(width, size_bits);
	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			element = elements[(size_t)(y >> size_bits) * elements_width + (x >> size_bits)];
			pixel = pixels[(size_t)y * width + x];
			green = channel(pixel, 1U);
			red = channel(pixel, 2U);
			blue = channel(pixel, 0U);

			/* red_to_blue is in the element's red, green_to_blue in its green, green_to_red in its blue. */
			pixels[(size_t)y * width + x] = make_pixel(channel(pixel, 3U),
				red - (uint32_t)color_delta(channel(element, 0U), green), green,
				blue - (uint32_t)color_delta(channel(element, 1U), green) -
				(uint32_t)color_delta(channel(element, 2U), red));
		}
	}
}

/* Applies the forward subtract-green transform: green is taken off red and blue. */
static void
forward_subtract_green(
	uint32_t *pixels,
	size_t count)
{
	size_t index;

	/* Every pixel. */
	for (index = 0; index < count; index++) {
		pixels[index] = make_pixel(channel(pixels[index], 3U), channel(pixels[index], 2U) - channel(pixels[index], 1U),
			channel(pixels[index], 1U), channel(pixels[index], 0U) - channel(pixels[index], 1U));
	}
}

/* Gives the width_bits Table 3 gives for a colour count. */
static unsigned
width_bits_of(
	unsigned colors)
{
	if (colors <= 2U)
		return 3U;
	if (colors <= 4U)
		return 2U;
	if (colors <= 16U)
		return 1U;
	return 0U;
}

/*
 * Applies the forward colour indexing transform: the table is the distinct
 * colours in order of first appearance (it must be at most 256), the image
 * becomes the packed indices (alpha 255, red and blue 0, the indices of
 * 8 >> width_bits bits each in the green, the first pixel in the low bits).
 * Gives the number of colours; pixels then holds the packed image.
 */
static unsigned
forward_indexing(
	uint32_t *pixels,
	unsigned width,
	unsigned height,
	uint32_t *table)
{
	unsigned count;
	unsigned width_bits;
	unsigned packed_width;
	unsigned x;
	unsigned y;
	unsigned index;
	uint32_t green;

	/* The distinct colours. */
	count = 0;
	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			for (index = 0; index < count; index++) {
				if (table[index] == pixels[(size_t)y * width + x])
					break;
			}
			if (index == count) {
				CHECK(count < 256U);
				if (count >= 256U)
					return count;
				table[count] = pixels[(size_t)y * width + x];
				count++;
			}
		}
	}

	/* The packed indices, row by row, into the scratch image and back. */
	width_bits = width_bits_of(count);
	packed_width = subsample(width, width_bits);
	for (y = 0; y < height; y++) {
		for (x = 0; x < packed_width; x++)
			scratch[(size_t)y * packed_width + x] = 0xff000000U;
		for (x = 0; x < width; x++) {
			for (index = 0; index < count; index++) {
				if (table[index] == pixels[(size_t)y * width + x])
					break;
			}
			green = (uint32_t)index << ((x & ((1U << width_bits) - 1U)) * (8U >> width_bits));
			scratch[(size_t)y * packed_width + (x >> width_bits)] |= green << 8;
		}
	}
	memcpy(pixels, scratch, (size_t)packed_width * height * sizeof(uint32_t));
	return count;
}

/* Gives DIV_ROUND_UP(size, 1 << bits). */
static unsigned
subsample(
	unsigned size,
	unsigned bits)
{
	return (size + (1U << bits) - 1U) >> bits;
}

/*
 * Writes a normal prefix code of 256 symbols of 8 bits each, as RFC 9649
 * 3.7.2.1.2 lays it out: a code length code in which only the lengths 0 and
 * 8 exist (1 bit each), then 256 lengths of 8; the green code's alphabet is
 * longer (280), so its list is cut at max_symbol 256.
 */
static void
put_normal_code(
	struct lbitwriter *writer,
	int green)
{
	unsigned index;

	/* Not a simple code, 12 code length code lengths follow (4 + 8). */
	lbitwriter_put(writer, 0U, 1U);
	lbitwriter_put(writer, 8U, 4U);
	for (index = 0; index < 12U; index++) {
		if (test_order[index] == 0U || test_order[index] == 8U)
			lbitwriter_put(writer, 1U, 3U);
		else
			lbitwriter_put(writer, 0U, 3U);
	}

	/* max_symbol: all the alphabet for a 256-symbol code; 256 (2 + 254, in 2 + 2 * 4 = 10 bits) for green. */
	if (green) {
		lbitwriter_put(writer, 1U, 1U);
		lbitwriter_put(writer, 4U, 3U);
		lbitwriter_put(writer, 254U, 10U);
	} else {
		lbitwriter_put(writer, 0U, 1U);
	}

	/* 256 times the code length 8 (symbol 8 of the code length code is the codeword 1). */
	for (index = 0; index < 256U; index++)
		lbitwriter_put_code(writer, 1U, 1U);
}

/*
 * Writes an entropy-coded image of width by height pixels: no colour cache,
 * no meta prefix code (role 1 only has the bit), one group of flat codes,
 * then every pixel as four 8-bit codewords in the order green, red, blue,
 * alpha.
 */
static void
put_image(
	struct lbitwriter *writer,
	const uint32_t *pixels,
	unsigned width,
	unsigned height,
	int role)
{
	size_t index;
	size_t count;

	/* The colour cache bit (0) and, for the main image, the meta prefix bit (0). */
	lbitwriter_put(writer, 0U, 1U);
	if (role == 1)
		lbitwriter_put(writer, 0U, 1U);

	/* The group: green, red, blue, alpha as flat codes, the distance code as a simple code of symbol 0. */
	put_normal_code(writer, 1);
	put_normal_code(writer, 0);
	put_normal_code(writer, 0);
	put_normal_code(writer, 0);
	lbitwriter_put(writer, 1U, 1U);
	lbitwriter_put(writer, 0U, 1U);
	lbitwriter_put(writer, 0U, 1U);
	lbitwriter_put(writer, 0U, 1U);

	/* The pixels: with a flat code the codeword of a symbol is its value. */
	count = (size_t)width * height;
	for (index = 0; index < count; index++) {
		lbitwriter_put_code(writer, channel(pixels[index], 1U), 8U);
		lbitwriter_put_code(writer, channel(pixels[index], 2U), 8U);
		lbitwriter_put_code(writer, channel(pixels[index], 0U), 8U);
		lbitwriter_put_code(writer, channel(pixels[index], 3U), 8U);
	}
}

/* Writes the 5 bytes of a lossless stream's header for a width by height image (alpha bit 0, version 0). */
static void
put_stream_header(
	struct lbitwriter *writer,
	unsigned width,
	unsigned height)
{
	/* The signature, 14 bits each of width - 1 and height - 1, the alpha hint and 3 bits of version. */
	lbitwriter_put(writer, 0x2fU, 8U);
	lbitwriter_put(writer, width - 1U, 14U);
	lbitwriter_put(writer, height - 1U, 14U);
	lbitwriter_put(writer, 0U, 1U);
	lbitwriter_put(writer, 0U, 3U);
}

/* Starts a test stream (no header) for an image: the starting image is kept, and no transform has been written. */
static void
start_image(
	unsigned width,
	unsigned height,
	const uint32_t *pixels)
{
	/* The image the transforms start from, in the original and in the working copy. */
	memcpy(original, pixels, (size_t)width * height * sizeof(uint32_t));
	memcpy(current, pixels, (size_t)width * height * sizeof(uint32_t));
	current_width = width;
	lbitwriter_init(&stream);
}

/*
 * Writes a transform of the given type (with its 1 bit and type) for the
 * current image and applies it to the current image.  Random data are drawn
 * for the predictor and colour transforms; colour indexing takes the
 * colours of the image.
 */
static void
write_transform(
	unsigned type,
	unsigned height)
{
	unsigned size_bits;
	unsigned sub_width;
	unsigned sub_height;
	unsigned count;
	unsigned index;
	uint32_t table[256];
	uint32_t previous;

	/* The transform is present, and its type. */
	lbitwriter_put(&stream, 1U, 1U);
	lbitwriter_put(&stream, type, 2U);

	if (type == TX_SUBTRACT_GREEN) {
		/* No data. */
		forward_subtract_green(current, (size_t)current_width * height);
	} else if (type == TX_INDEXING) {
		/* The colour count - 1, the deltas of the table as a count by 1 image, then the packed image replaces the current one. */
		count = forward_indexing(current, current_width, height, table);
		lbitwriter_put(&stream, count - 1U, 8U);
		previous = 0;
		for (index = 0; index < count; index++) {
			sub_image[index] = sub_pixels(table[index], previous);
			previous = table[index];
		}
		put_image(&stream, sub_image, count, 1U, 0);
		current_width = subsample(current_width, width_bits_of(count));
	} else {
		/* size_bits - 2 (3 bits), and a random subresolution image of the blocks (modes in the green of the predictor's). */
		size_bits = 2U + (unsigned)(next_random() % 4U);
		lbitwriter_put(&stream, size_bits - 2U, 3U);
		sub_width = subsample(current_width, size_bits);
		sub_height = subsample(height, size_bits);
		for (index = 0; index < sub_width * sub_height; index++) {
			sub_image[index] = random_pixel();
			if (type == TX_PREDICTOR)
				sub_image[index] = (sub_image[index] & 0xffff00ffU) | ((uint32_t)(next_random() % 16U) << 8);
		}
		put_image(&stream, sub_image, sub_width, sub_height, 0);
		if (type == TX_PREDICTOR)
			forward_predictor(current, current_width, height, sub_image, size_bits);
		else
			forward_color(current, current_width, height, sub_image, size_bits);
	}
}

/* Writes the 0 bit that ends the transforms, then the current image as the main image. */
static void
end_transforms(
	unsigned height)
{
	/* No further transform, then the spatially coded image at the current width. */
	lbitwriter_put(&stream, 0U, 1U);
	put_image(&stream, current, current_width, height, 1);
}

/* Decodes the stream written so far into decoded[]; gives the error. */
static int
decode_current(
	unsigned width,
	unsigned height)
{
	/* The decoder is given exactly the bytes written. */
	memset(decoded, 0x5a, sizeof(decoded));
	return webpc_vp8l_decode_stream(stream.bytes, lbitwriter_size(&stream), width, height, decoded);
}

/* Fills original[] with a random image of width by height pixels of at most colors distinct colours (0: any). */
static void
random_image(
	unsigned width,
	unsigned height,
	unsigned colors)
{
	uint32_t palette[256];
	unsigned index;

	/* From a palette when the colour count is limited, else any pixel. */
	for (index = 0; index < colors && index < 256U; index++)
		palette[index] = random_pixel();
	for (index = 0; index < width * height; index++) {
		if (colors == 0U)
			original[index] = random_pixel();
		else
			original[index] = palette[next_random() % colors];
	}
}

/* Checks read_header on hand-made bytes. */
static void
test_header(void)
{
	struct webpc_vp8l_header header;
	uint8_t bytes[8];
	unsigned version;

	/*
	 * 0x2f then the 32 bits: width - 1 = 0x123 (bits 0..13), height - 1 =
	 * 0x2abc (bits 14..27), alpha 1 (bit 28), version 0 (bits 29..31).
	 * Bits 0..7 = 0x23, bits 8..13 = 0x01 and bits 14..15 of 0x2abc (0x0)
	 * -> byte 1 = 0x01 | (0x2abc & 3) << 6 = 0x01 | 0 = 0x01.
	 */
	bytes[0] = 0x2fU;
	bytes[1] = 0x23U;
	bytes[2] = (uint8_t)(0x01U | ((0x2abcU & 3U) << 6));
	bytes[3] = (uint8_t)((0x2abcU >> 2) & 0xffU);
	bytes[4] = (uint8_t)(((0x2abcU >> 10) & 0x0fU) | 0x10U);
	memset(&header, 0, sizeof(header));
	CHECK(webpc_vp8l_read_header(bytes, 5U, &header) == 0);
	CHECK(header.width == 0x124U);
	CHECK(header.height == 0x2abdU);
	CHECK(header.alpha_is_used == 1);

	/* The smallest and the largest sizes, no alpha. */
	lbitwriter_init(&stream);
	put_stream_header(&stream, 1U, 1U);
	CHECK(webpc_vp8l_read_header(stream.bytes, 5U, &header) == 0);
	CHECK(header.width == 1U && header.height == 1U && header.alpha_is_used == 0);
	lbitwriter_init(&stream);
	put_stream_header(&stream, 16384U, 16384U);
	CHECK(webpc_vp8l_read_header(stream.bytes, 5U, &header) == 0);
	CHECK(header.width == 16384U && header.height == 16384U);

	/* The data after the header does not matter, and more than 5 bytes is fine. */
	bytes[5] = 0xffU;
	bytes[6] = 0xffU;
	CHECK(webpc_vp8l_read_header(bytes, 7U, &header) == 0);
	CHECK(header.width == 0x124U);

	/* Versions 1 to 7 are errors (the version is bits 29..31, the top 3 bits of byte 4). */
	for (version = 1; version < 8U; version++) {
		bytes[4] = (uint8_t)(((0x2abcU >> 10) & 0x0fU) | (version << 5));
		CHECK(webpc_vp8l_read_header(bytes, 5U, &header) == EINVAL);
	}
	bytes[4] = (uint8_t)(((0x2abcU >> 10) & 0x0fU) | 0x10U);

	/* Another signature is an error, whatever else the bytes hold. */
	bytes[0] = 0x2eU;
	CHECK(webpc_vp8l_read_header(bytes, 5U, &header) == EINVAL);
	bytes[0] = 0x30U;
	CHECK(webpc_vp8l_read_header(bytes, 5U, &header) == EINVAL);
	bytes[0] = 0x2fU;

	/* Fewer than 5 bytes are ENOBUFS, even with a wrong signature. */
	CHECK(webpc_vp8l_read_header(bytes, 4U, &header) == ENOBUFS);
	CHECK(webpc_vp8l_read_header(bytes, 0U, &header) == ENOBUFS);
	CHECK(webpc_vp8l_read_header(NULL, 0U, &header) == ENOBUFS);
	bytes[0] = 0x00U;
	CHECK(webpc_vp8l_read_header(bytes, 3U, &header) == ENOBUFS);
}

/* Decodes images with no transform at all, in several sizes. */
static void
test_no_transform(void)
{
	static const unsigned sizes[6][2] = { { 1, 1 }, { 2, 1 }, { 1, 5 }, { 7, 3 }, { 16, 16 }, { 33, 9 } };
	unsigned index;

	/* Each size with random pixels. */
	for (index = 0; index < 6U; index++) {
		random_image(sizes[index][0], sizes[index][1], 0U);
		start_image(sizes[index][0], sizes[index][1], original);
		end_transforms(sizes[index][1]);
		CHECK(decode_current(sizes[index][0], sizes[index][1]) == 0);
		CHECK(memcmp(decoded, original, (size_t)sizes[index][0] * sizes[index][1] * 4U) == 0);
	}
}

/* Decodes each transform alone: subtract green, predictor, colour, and indexing with a mixed palette. */
static void
test_single_transforms(void)
{
	static const unsigned types[4] = { TX_SUBTRACT_GREEN, TX_PREDICTOR, TX_COLOR, TX_INDEXING };
	static const unsigned sizes[7][2] = { { 1, 1 }, { 3, 1 }, { 1, 4 }, { 5, 5 }, { 17, 13 }, { 32, 8 }, { 40, 25 } };
	unsigned type;
	unsigned index;
	unsigned repeat;

	/* Every type, every size, several random draws of the transform data. */
	for (type = 0; type < 4U; type++) {
		for (index = 0; index < 7U; index++) {
			for (repeat = 0; repeat < 4U; repeat++) {
				random_image(sizes[index][0], sizes[index][1], types[type] == TX_INDEXING ? 7U : 0U);
				start_image(sizes[index][0], sizes[index][1], original);
				write_transform(types[type], sizes[index][1]);
				end_transforms(sizes[index][1]);
				CHECK(decode_current(sizes[index][0], sizes[index][1]) == 0);
				CHECK(memcmp(decoded, original, (size_t)sizes[index][0] * sizes[index][1] * 4U) == 0);
			}
		}
	}
}

/*
 * Colour indexing with each colour count of the table: the bundling of
 * Table 3 (counts 2, 3, 4, 5, 16, 17, 256 give width_bits 3, 2, 2, 1, 1, 0,
 * 0) shows in the size of the packed image the stream carries; the
 * decoder must read exactly that many pixels, and widths that are not a
 * multiple of the bundling exercise the partial last pixel.
 */
static void
test_indexing_sizes(void)
{
	static const unsigned counts[8] = { 1, 2, 3, 4, 5, 16, 17, 256 };
	static const unsigned widths[4] = { 1, 9, 31, 64 };
	unsigned count_index;
	unsigned width_index;
	unsigned width;
	unsigned height;
	unsigned index;
	unsigned distinct;
	unsigned x;
	unsigned y;
	int error;

	height = 6;
	for (count_index = 0; count_index < 8U; count_index++) {
		for (width_index = 0; width_index < 4U; width_index++) {
			width = widths[width_index];

			/* An image using exactly counts[] colours when it has the pixels for them, else fewer. */
			random_image(width, height, counts[count_index]);
			for (index = 0; index < counts[count_index] && index < width * height; index++)
				original[index] = original[index];
			start_image(width, height, original);
			distinct = 0;
			for (y = 0; y < height; y++) {
				for (x = 0; x < width; x++) {
					for (index = 0; index < y * width + x; index++) {
						if (original[index] == original[y * width + x])
							break;
					}
					if (index == y * width + x)
						distinct++;
				}
			}
			write_transform(TX_INDEXING, height);

			/* The width the packed image has is the one Table 3 gives for the number of colours present. */
			CHECK(current_width == subsample(width, width_bits_of(distinct)));
			end_transforms(height);
			error = decode_current(width, height);
			CHECK(error == 0);
			CHECK(memcmp(decoded, original, (size_t)width * height * 4U) == 0);
		}
	}

	/* An image with 256 different colours exactly (a ramp), the largest table. */
	for (index = 0; index < 256U; index++)
		original[index] = make_pixel(255U - index, index, (index * 7U) & 0xffU, index ^ 0x5aU);
	start_image(256U, 1U, original);
	write_transform(TX_INDEXING, 1U);
	CHECK(current_width == 256U);
	end_transforms(1U);
	CHECK(decode_current(256U, 1U) == 0);
	CHECK(memcmp(decoded, original, 256U * 4U) == 0);
}

/*
 * A colour table of 3 entries (deltas 0x01020304, 0x80ffff01 and
 * 0x7f020203 summing, per channel and modulo 256, to 0x01020304,
 * 0x81010205 and 0x00030408) with packed indices that include 3, which is
 * past the table: the pixel is 0x00000000.  Written by hand, without the
 * forward transform.
 */
static void
test_indexing_out_of_range(void)
{
	static const uint32_t deltas[3] = { 0x01020304U, 0x80ffff01U, 0x7f020203U };
	static const uint32_t expected_table[4] = { 0x01020304U, 0x81010205U, 0x00030408U, 0x00000000U };
	static const unsigned indices[5] = { 3U, 1U, 0U, 2U, 3U };
	uint32_t packed[2];
	unsigned index;

	/* A 5 by 1 image, 3 colours: width_bits 2, packed width 2; green of pixel 0 holds indices 3, 1, 0, 2 from the low bits. */
	lbitwriter_init(&stream);
	lbitwriter_put(&stream, 1U, 1U);
	lbitwriter_put(&stream, TX_INDEXING, 2U);
	lbitwriter_put(&stream, 2U, 8U);
	put_image(&stream, deltas, 3U, 1U, 0);
	lbitwriter_put(&stream, 0U, 1U);
	packed[0] = 0xff000000U | ((3U | (1U << 2) | (0U << 4) | (2U << 6)) << 8);
	packed[1] = 0xff000000U | (3U << 8);
	put_image(&stream, packed, 2U, 1U, 1);
	CHECK(decode_current(5U, 1U) == 0);
	for (index = 0; index < 5U; index++)
		CHECK(decoded[index] == expected_table[indices[index]]);

	/* One colour (width_bits 3): packed bits 1 are past the table. */
	lbitwriter_init(&stream);
	lbitwriter_put(&stream, 1U, 1U);
	lbitwriter_put(&stream, TX_INDEXING, 2U);
	lbitwriter_put(&stream, 0U, 8U);
	put_image(&stream, deltas, 1U, 1U, 0);
	lbitwriter_put(&stream, 0U, 1U);
	packed[0] = 0xff000000U | (0xa5U << 8);
	put_image(&stream, packed, 1U, 1U, 1);
	CHECK(decode_current(8U, 1U) == 0);
	for (index = 0; index < 8U; index++) {
		if (((0xa5U >> index) & 1U) != 0U)
			CHECK(decoded[index] == 0U);
		else
			CHECK(decoded[index] == deltas[0]);
	}
}

/*
 * Several transforms together: indexing then predictor (the predictor
 * works on the packed width), all four in two orders, and transforms
 * written in the other orders on an image that stays a few colours.
 */
static void
test_combinations(void)
{
	static const unsigned order_a[4] = { TX_INDEXING, TX_PREDICTOR, TX_COLOR, TX_SUBTRACT_GREEN };
	static const unsigned order_b[4] = { TX_INDEXING, TX_SUBTRACT_GREEN, TX_COLOR, TX_PREDICTOR };
	static const unsigned order_c[4] = { TX_SUBTRACT_GREEN, TX_COLOR, TX_PREDICTOR, TX_INDEXING };
	static const unsigned order_d[3] = { TX_COLOR, TX_PREDICTOR, TX_SUBTRACT_GREEN };
	static const unsigned order_e[2] = { TX_PREDICTOR, TX_COLOR };
	static const unsigned order_f[2] = { TX_INDEXING, TX_PREDICTOR };
	static const unsigned sizes[4][2] = { { 1, 1 }, { 6, 3 }, { 21, 17 }, { 37, 11 } };
	unsigned size_index;
	unsigned repeat;
	unsigned index;
	unsigned width;
	unsigned height;

	for (size_index = 0; size_index < 4U; size_index++) {
		width = sizes[size_index][0];
		height = sizes[size_index][1];
		for (repeat = 0; repeat < 6U; repeat++) {
			/* Indexing first: any palette of up to 16 colours, then predictor on the packed image, and so on. */
			random_image(width, height, 5U + repeat * 2U);
			start_image(width, height, original);
			for (index = 0; index < 4U; index++)
				write_transform(order_a[index], height);
			end_transforms(height);
			CHECK(decode_current(width, height) == 0);
			CHECK(memcmp(decoded, original, (size_t)width * height * 4U) == 0);

			random_image(width, height, 3U + repeat);
			start_image(width, height, original);
			for (index = 0; index < 4U; index++)
				write_transform(order_b[index], height);
			end_transforms(height);
			CHECK(decode_current(width, height) == 0);
			CHECK(memcmp(decoded, original, (size_t)width * height * 4U) == 0);

			/* Indexing last: the image is one colour, which the other transforms leave a few colours. */
			random_image(width, height, 1U);
			start_image(width, height, original);
			for (index = 0; index < 4U; index++)
				write_transform(order_c[index], height);
			end_transforms(height);
			CHECK(decode_current(width, height) == 0);
			CHECK(memcmp(decoded, original, (size_t)width * height * 4U) == 0);

			/* Three, then two, then indexing and predictor. */
			random_image(width, height, 0U);
			start_image(width, height, original);
			for (index = 0; index < 3U; index++)
				write_transform(order_d[index], height);
			end_transforms(height);
			CHECK(decode_current(width, height) == 0);
			CHECK(memcmp(decoded, original, (size_t)width * height * 4U) == 0);

			random_image(width, height, 0U);
			start_image(width, height, original);
			for (index = 0; index < 2U; index++)
				write_transform(order_e[index], height);
			end_transforms(height);
			CHECK(decode_current(width, height) == 0);
			CHECK(memcmp(decoded, original, (size_t)width * height * 4U) == 0);

			random_image(width, height, 2U + repeat);
			start_image(width, height, original);
			for (index = 0; index < 2U; index++)
				write_transform(order_f[index], height);
			end_transforms(height);
			CHECK(decode_current(width, height) == 0);
			CHECK(memcmp(decoded, original, (size_t)width * height * 4U) == 0);
		}
	}
}

/* A second transform of one type is EINVAL, each type, with the first alone fine. */
static void
test_duplicates(void)
{
	unsigned type;
	unsigned other;
	unsigned width;
	unsigned height;

	width = 9;
	height = 5;
	for (type = 0; type < 4U; type++) {
		/* The same type twice (on an image of 2 colours so that indexing can come twice). */
		random_image(width, height, 2U);
		start_image(width, height, original);
		write_transform(type, height);
		write_transform(type, height);
		end_transforms(height);
		CHECK(decode_current(width, height) == EINVAL);

		/* Each other type between the two copies changes nothing. */
		for (other = 0; other < 4U; other++) {
			if (other == type)
				continue;
			random_image(width, height, 2U);
			start_image(width, height, original);
			write_transform(type, height);
			write_transform(other, height);
			write_transform(type, height);
			end_transforms(height);
			CHECK(decode_current(width, height) == EINVAL);
		}
	}
}

/*
 * Every prefix of a valid stream (the four transforms) is refused: the cut
 * stream either runs out (ENOBUFS) or, when the zeros that stand for the
 * missing bits happen to break the format first, EINVAL; never success.
 * Run under the leak checker this also shows that a failure leaves nothing
 * allocated.
 */
static void
test_truncated(void)
{
	unsigned width;
	unsigned height;
	unsigned index;
	unsigned cut;
	size_t size;
	int error;
	int enobufs;

	width = 11;
	height = 7;
	random_image(width, height, 6U);
	start_image(width, height, original);
	for (index = 0; index < 4U; index++)
		write_transform(index == 0U ? TX_INDEXING : index == 1U ? TX_PREDICTOR : index == 2U ? TX_COLOR : TX_SUBTRACT_GREEN,
			height);
	end_transforms(height);
	size = lbitwriter_size(&stream);
	CHECK(decode_current(width, height) == 0);

	/* Every shorter length, down to nothing. */
	enobufs = 0;
	for (cut = 0; cut < size; cut++) {
		error = webpc_vp8l_decode_stream(stream.bytes, cut, width, height, decoded);
		CHECK(error == ENOBUFS || error == EINVAL);
		if (error == ENOBUFS)
			enobufs++;
	}

	/* Nearly all of the cuts run out of data (the zero fill breaking the format first is rare). */
	CHECK(enobufs > (int)(size / 2U));

	/* A stream of no bytes and of one byte, on a bigger image. */
	CHECK(webpc_vp8l_decode_stream(NULL, 0U, 100U, 100U, decoded) != 0);
	CHECK(webpc_vp8l_decode_stream(stream.bytes, 1U, 100U, 100U, decoded) != 0);
}

/* Random bytes, random mutations and random sizes: only the documented errors, nothing else. */
static void
test_random_streams(void)
{
	unsigned iteration;
	unsigned width;
	unsigned height;
	unsigned count;
	unsigned index;
	unsigned position;
	size_t size;
	uint8_t bytes[512];
	int error;

	/* Pure noise. */
	for (iteration = 0; iteration < 2000U; iteration++) {
		size = (size_t)(next_random() % 512U);
		for (index = 0; index < size; index++)
			bytes[index] = (uint8_t)next_random();
		width = 1U + (unsigned)(next_random() % 40U);
		height = 1U + (unsigned)(next_random() % 40U);
		error = webpc_vp8l_decode_stream(bytes, size, width, height, decoded);
		CHECK(error == 0 || error == EINVAL || error == ENOBUFS || error == ENOMEM);
	}

	/* A valid stream with a few bytes flipped. */
	width = 13;
	height = 9;
	random_image(width, height, 5U);
	start_image(width, height, original);
	write_transform(TX_INDEXING, height);
	write_transform(TX_PREDICTOR, height);
	write_transform(TX_SUBTRACT_GREEN, height);
	end_transforms(height);
	size = lbitwriter_size(&stream);
	for (iteration = 0; iteration < 3000U; iteration++) {
		memcpy(bytes, stream.bytes, size < sizeof(bytes) ? size : sizeof(bytes));
		count = 1U + (unsigned)(next_random() % 3U);
		for (index = 0; index < count; index++) {
			position = (unsigned)(next_random() % (size < sizeof(bytes) ? size : sizeof(bytes)));
			bytes[position] = (uint8_t)next_random();
		}
		error = webpc_vp8l_decode_stream(bytes, size < sizeof(bytes) ? size : sizeof(bytes), width, height, decoded);
		CHECK(error == 0 || error == EINVAL || error == ENOBUFS || error == ENOMEM);
	}

	/* A side out of the format's range or a missing output is EINVAL, with nothing read. */
	CHECK(webpc_vp8l_decode_stream(bytes, 4U, 0U, 3U, decoded) == EINVAL);
	CHECK(webpc_vp8l_decode_stream(bytes, 4U, 3U, 0U, decoded) == EINVAL);
	CHECK(webpc_vp8l_decode_stream(bytes, 4U, 16385U, 3U, decoded) == EINVAL);
	CHECK(webpc_vp8l_decode_stream(bytes, 4U, 3U, 16385U, decoded) == EINVAL);
	CHECK(webpc_vp8l_decode_stream(bytes, 4U, 3U, 3U, NULL) == EINVAL);

	/*
	 * The largest sides are allowed (an empty stream then runs out before
	 * any pixel is written), and a stream with no bits at all is ENOBUFS,
	 * not EINVAL, whatever the size.
	 */
	CHECK(webpc_vp8l_decode_stream(NULL, 0U, 16384U, 1U, decoded) == ENOBUFS);
	CHECK(webpc_vp8l_decode_stream(NULL, 0U, 1U, 16384U, decoded) == ENOBUFS);
	CHECK(webpc_vp8l_decode_stream(NULL, 0U, 3U, 3U, decoded) == ENOBUFS);
}

/* The whole-stream call: the header and the image, the errors of both, and nothing left on failure. */
static void
test_decode_whole(void)
{
	static struct lbitwriter whole;
	uint32_t *pixels;
	unsigned width;
	unsigned height;
	unsigned cut;
	size_t size;
	int error;

	/* A 10 by 6 image with a predictor and subtract green, behind a header. */
	random_image(10U, 6U, 0U);
	start_image(10U, 6U, original);
	write_transform(TX_PREDICTOR, 6U);
	write_transform(TX_SUBTRACT_GREEN, 6U);
	end_transforms(6U);
	lbitwriter_init(&whole);
	put_stream_header(&whole, 10U, 6U);
	memcpy(whole.bytes + 5, stream.bytes, lbitwriter_size(&stream));
	size = 5U + lbitwriter_size(&stream);

	/* The decoder gives the image, its size, and an array the caller frees. */
	pixels = NULL;
	width = 0;
	height = 0;
	error = webpc_vp8l_decode(whole.bytes, size, &pixels, &width, &height);
	CHECK(error == 0);
	CHECK(pixels != NULL);
	CHECK(width == 10U && height == 6U);
	if (pixels != NULL) {
		CHECK(memcmp(pixels, original, 10U * 6U * 4U) == 0);
		free(pixels);
	}

	/* Every cut: the header's ENOBUFS below 5 bytes, then the stream's errors, never an array. */
	for (cut = 0; cut < size; cut++) {
		pixels = (uint32_t *)&whole;
		error = webpc_vp8l_decode(whole.bytes, cut, &pixels, &width, &height);
		CHECK(error != 0);
		CHECK(pixels == NULL);
		if (cut < 5U)
			CHECK(error == ENOBUFS);
	}

	/* A bad signature and a bad version fail before any decoding. */
	whole.bytes[0] = 0x2eU;
	pixels = (uint32_t *)&whole;
	CHECK(webpc_vp8l_decode(whole.bytes, size, &pixels, &width, &height) == EINVAL);
	CHECK(pixels == NULL);
	whole.bytes[0] = 0x2fU;
	whole.bytes[4] |= 0x20U;
	pixels = (uint32_t *)&whole;
	CHECK(webpc_vp8l_decode(whole.bytes, size, &pixels, &width, &height) == EINVAL);
	CHECK(pixels == NULL);

	/* A header that promises the largest image over almost no data runs out of data. */
	lbitwriter_init(&whole);
	put_stream_header(&whole, 16384U, 16384U);
	lbitwriter_put(&whole, 0U, 8U);
	pixels = (uint32_t *)&whole;
	error = webpc_vp8l_decode(whole.bytes, lbitwriter_size(&whole), &pixels, &width, &height);
	CHECK(error == ENOBUFS || error == EINVAL || error == ENOMEM);
	CHECK(pixels == NULL);

	/* Missing outputs. */
	CHECK(webpc_vp8l_decode(whole.bytes, 6U, NULL, &width, &height) == EINVAL);
}
