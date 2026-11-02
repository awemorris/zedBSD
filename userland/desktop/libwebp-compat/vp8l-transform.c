/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The inverse transforms of the lossless format.
 */

#include "vp8l-transform.h"

/* The four channels of an ARGB pixel, which the predictors treat one at a time. */
#define CHANNEL_COUNT		4U

/* The bit distance between two neighbouring channels of a pixel. */
#define CHANNEL_BITS		8U

/* The pixel every prediction at the image's top-left corner gives: opaque black. */
#define PREDICTION_BLACK	0xff000000U

/* The channels of a pixel that sit in the even bytes (blue and red) and in the odd bytes (green and alpha). */
#define EVEN_BYTES		0x00ff00ffU
#define ODD_BYTES		0xff00ff00U

/* The mode (0..15) of the predictor block that pixel (x, y) is in. */
static unsigned predictor_mode(const uint32_t *modes, unsigned modes_width, unsigned size_bits, unsigned x, unsigned y);

/* The prediction of mode for a pixel from its four neighbours. */
static uint32_t predict_pixel(unsigned mode, uint32_t left, uint32_t top, uint32_t top_right, uint32_t top_left);

/* The pixel whose channels are the sums, modulo 256, of the channels of two pixels. */
static uint32_t add_pixels(uint32_t first, uint32_t second);

/* The pixel whose channels are the floors of the means of the channels of two pixels. */
static uint32_t average2(uint32_t first, uint32_t second);

/* The predictor Select of RFC 9649 3.5.1: left or top, whichever the estimate left + top - top_left is nearer. */
static uint32_t select_pixel(uint32_t left, uint32_t top, uint32_t top_left);

/* The predictor ClampAddSubtractFull of RFC 9649 3.5.1, per channel. */
static uint32_t clamp_add_subtract_full(uint32_t left, uint32_t top, uint32_t top_left);

/* The predictor ClampAddSubtractHalf of RFC 9649 3.5.1, per channel. */
static uint32_t clamp_add_subtract_half(uint32_t first, uint32_t second);

/* The colour transform delta of RFC 9649 3.5.2 for the unsigned bytes t and c. */
static int color_delta(unsigned t, unsigned c);

/*
 * Gives DIV_ROUND_UP(size, 1 << bits) (RFC 9649 3.5.1), for size 1 or more
 * and bits 0..9.
 */
unsigned
webpc_vp8l_subsample(
	unsigned size,
	unsigned bits)
{
	unsigned whole;
	unsigned remainder;
	unsigned count;

	/* The blocks that are full, then one more block when a partial one is left over. */
	whole = size >> bits;
	remainder = size & ((1U << bits) - 1U);
	count = whole;
	if (remainder != 0U)
		count = whole + 1U;

	/* Reports how many blocks cover size. */
	return count;
}

/*
 * Undoes the predictor transform (RFC 9649 3.5.1) in place over pixels
 * (width by height, rows top to bottom), in scan-line order so that each
 * prediction uses pixels already restored.
 */
void
webpc_vp8l_inverse_predictor(
	uint32_t *pixels,
	unsigned width,
	unsigned height,
	const uint32_t *modes,
	unsigned size_bits)
{
	unsigned modes_width;
	unsigned x;
	unsigned y;
	unsigned mode;
	uint32_t *row;
	const uint32_t *above;
	uint32_t left;
	uint32_t top;
	uint32_t top_right;
	uint32_t top_left;
	uint32_t predicted;

	/* An image with no pixels has nothing to restore. */
	if (width == 0U || height == 0U)
		return;

	/* The blocks across, which strides through the predictor image. */
	modes_width = webpc_vp8l_subsample(width, size_bits);

	/*
	 * The left-topmost pixel is predicted opaque black, whatever the mode.
	 */
	pixels[0] = add_pixels(pixels[0], PREDICTION_BLACK);

	/* The rest of the top row is predicted from the pixel on its left, whatever the mode. */
	for (x = 1U;
	     x < width;
	     x++)
		pixels[x] = add_pixels(pixels[x], pixels[x - 1U]);

	/* Restores the other rows from the top, each pixel from pixels already restored. */
	for (y = 1U;
	     y < height;
	     y++) {
		row = pixels + (size_t)y * width;
		above = row - width;

		/* The leftmost pixel of a row is predicted from the pixel above it, whatever the mode. */
		row[0] = add_pixels(row[0], above[0]);

		/* The rest of the row, each pixel predicted by the mode of its block. */
		for (x = 1U;
		     x < width;
		     x++) {
			/*
			 * The neighbours already restored.  Past the last pixel of
			 * the row above, the top-right neighbour of the rightmost
			 * pixel is the first pixel of this row, which follows it in
			 * memory.
			 */
			left = row[x - 1U];
			top = above[x];
			top_left = above[x - 1U];
			top_right = above[x + 1U];

			/* The block the pixel is in names the predictor. */
			mode = predictor_mode(modes, modes_width, size_bits, x, y);

			/* Adds the residual to the prediction. */
			predicted = predict_pixel(mode, left, top, top_right, top_left);
			row[x] = add_pixels(row[x], predicted);
		}
	}
}

/*
 * Undoes the colour transform (RFC 9649 3.5.2) in place over pixels.
 */
void
webpc_vp8l_inverse_color(
	uint32_t *pixels,
	unsigned width,
	unsigned height,
	const uint32_t *elements,
	unsigned size_bits)
{
	unsigned elements_width;
	unsigned x;
	unsigned y;
	uint32_t *row;
	uint32_t element;
	uint32_t pixel;
	unsigned green_to_red;
	unsigned green_to_blue;
	unsigned red_to_blue;
	unsigned alpha;
	unsigned red;
	unsigned green;
	unsigned blue;
	int new_red;
	int new_blue;

	/* An image with no pixels has nothing to restore. */
	if (width == 0U || height == 0U)
		return;

	/* The blocks across, which strides through the colour transform image. */
	elements_width = webpc_vp8l_subsample(width, size_bits);

	/* Restores each pixel with the element of the block it is in. */
	for (y = 0U;
	     y < height;
	     y++) {
		row = pixels + (size_t)y * width;
		for (x = 0U;
		     x < width;
		     x++) {
			/*
			 * The element of the block: red_to_blue is the element's
			 * red, green_to_blue its green and green_to_red its blue.
			 */
			element = elements[(size_t)(y >> size_bits) * elements_width + (x >> size_bits)];
			red_to_blue = (unsigned)((element >> 16) & 0xffU);
			green_to_blue = (unsigned)((element >> 8) & 0xffU);
			green_to_red = (unsigned)(element & 0xffU);

			/* The channels of the pixel to restore. */
			pixel = row[x];
			alpha = (unsigned)((pixel >> 24) & 0xffU);
			red = (unsigned)((pixel >> 16) & 0xffU);
			green = (unsigned)((pixel >> 8) & 0xffU);
			blue = (unsigned)(pixel & 0xffU);

			/*
			 * Red takes the delta of green first; blue then takes the
			 * delta of green and the delta of the red just restored.  A
			 * delta can be negative, so each sum is wrapped through the
			 * unsigned conversion, which is modulo arithmetic.
			 */
			new_red = (int)red + color_delta(green_to_red, green);
			red = (unsigned)new_red & 0xffU;
			new_blue = (int)blue + color_delta(green_to_blue, green);
			new_blue = new_blue + color_delta(red_to_blue, red);
			blue = (unsigned)new_blue & 0xffU;

			/* Stores the restored pixel; alpha and green are as they were. */
			row[x] = ((uint32_t)alpha << 24) | ((uint32_t)red << 16) | ((uint32_t)green << 8) | (uint32_t)blue;
		}
	}
}

/*
 * Undoes the subtract-green transform (RFC 9649 3.5.3) over count pixels:
 * red and blue each get green added modulo 256.
 */
void
webpc_vp8l_add_green(
	uint32_t *pixels,
	size_t count)
{
	size_t index;
	uint32_t pixel;
	uint32_t green;
	uint32_t red_blue;

	/*
	 * Adds green to the red and blue of each pixel.  The two sums are
	 * made in the 16-bit lanes of one word, each lane wide enough that a
	 * sum cannot carry into its neighbour, and the lanes are masked back
	 * to 8 bits.
	 */
	for (index = 0U;
	     index < count;
	     index++) {
		pixel = pixels[index];
		green = (pixel >> 8) & 0xffU;
		red_blue = ((pixel & EVEN_BYTES) + green * 0x00010001U) & EVEN_BYTES;
		pixels[index] = (pixel & ODD_BYTES) | red_blue;
	}
}

/*
 * Undoes the colour-indexing transform (RFC 9649 3.5.4): the packed image
 * (packed_width = webpc_vp8l_subsample(width, width_bits) pixels a row,
 * height rows) becomes the image of width pixels a row in out.
 */
void
webpc_vp8l_inverse_indexing(
	const uint32_t *packed,
	unsigned width,
	unsigned height,
	unsigned width_bits,
	const uint32_t *table,
	uint32_t *out)
{
	unsigned packed_width;
	unsigned bits_per_index;
	unsigned index_mask;
	unsigned column_mask;
	unsigned x;
	unsigned y;
	unsigned shift;
	unsigned index;
	uint32_t pixel;
	size_t packed_row;
	size_t out_row;

	/* An image with no pixels has nothing to expand. */
	if (width == 0U || height == 0U)
		return;

	/*
	 * How the green of a packed pixel holds its indices: each index takes
	 * 8 >> width_bits bits, the first pixel of the group in the low bits.
	 */
	packed_width = webpc_vp8l_subsample(width, width_bits);
	bits_per_index = 8U >> width_bits;
	index_mask = (1U << bits_per_index) - 1U;
	column_mask = (1U << width_bits) - 1U;

	/*
	 * Expands from the last row and the last pixel back.  A pixel of the
	 * output is never stored before every packed pixel at or after the
	 * place it occupies has been read, so packed and out may be one buffer.
	 */
	for (y = height;
	     y > 0U;
	     y--) {
		packed_row = (size_t)(y - 1U) * packed_width;
		out_row = (size_t)(y - 1U) * width;
		for (x = width;
		     x > 0U;
		     x--) {
			/* The packed pixel that holds this pixel's index, and the index's place in its green. */
			pixel = packed[packed_row + ((x - 1U) >> width_bits)];
			shift = ((x - 1U) & column_mask) * bits_per_index;
			index = (unsigned)((pixel >> (8U + shift)) & index_mask);

			/* The colour the index names; indices past the colours the table holds find transparent black. */
			out[out_row + (x - 1U)] = table[index];
		}
	}
}

/*
 * Gives the mode of the predictor block of a pixel.
 */
static unsigned
predictor_mode(
	const uint32_t *modes,
	unsigned modes_width,
	unsigned size_bits,
	unsigned x,
	unsigned y)
{
	size_t block;
	uint32_t block_pixel;
	unsigned mode;

	/* The block holds the mode in the green of its pixel of the predictor image, modulo 16. */
	block = (size_t)(y >> size_bits) * modes_width + (x >> size_bits);
	block_pixel = modes[block];
	mode = (unsigned)((block_pixel >> 8) & 0xfU);

	/* Reports the mode of the block. */
	return mode;
}

/*
 * Gives the prediction a mode makes for a pixel from its neighbours.
 */
static uint32_t
predict_pixel(
	unsigned mode,
	uint32_t left,
	uint32_t top,
	uint32_t top_right,
	uint32_t top_left)
{
	uint32_t predicted;
	uint32_t partial;
	uint32_t other;

	/* Picks the prediction by the mode of RFC 9649 3.5.1 Table 2. */
	switch (mode) {
	case 1U:
		predicted = left;
		break;
	case 2U:
		predicted = top;
		break;
	case 3U:
		predicted = top_right;
		break;
	case 4U:
		predicted = top_left;
		break;
	case 5U:
		partial = average2(left, top_right);
		predicted = average2(partial, top);
		break;
	case 6U:
		predicted = average2(left, top_left);
		break;
	case 7U:
		predicted = average2(left, top);
		break;
	case 8U:
		predicted = average2(top_left, top);
		break;
	case 9U:
		predicted = average2(top, top_right);
		break;
	case 10U:
		partial = average2(left, top_left);
		other = average2(top, top_right);
		predicted = average2(partial, other);
		break;
	case 11U:
		predicted = select_pixel(left, top, top_left);
		break;
	case 12U:
		predicted = clamp_add_subtract_full(left, top, top_left);
		break;
	case 13U:
		partial = average2(left, top);
		predicted = clamp_add_subtract_half(partial, top_left);
		break;
	default:
		/* Mode 0, and the modes 14 and 15 that the format leaves undefined, predict opaque black. */
		predicted = PREDICTION_BLACK;
		break;
	}

	/* Reports the prediction. */
	return predicted;
}

/*
 * Adds two pixels channel by channel modulo 256.
 */
static uint32_t
add_pixels(
	uint32_t first,
	uint32_t second)
{
	uint32_t even_sum;
	uint32_t odd_sum;
	uint32_t sum;

	/*
	 * The blue and red channels are added in 16-bit lanes so that a carry
	 * stays in the lane, and the green and alpha channels are shifted
	 * down to the same lanes for the same reason.
	 */
	even_sum = ((first & EVEN_BYTES) + (second & EVEN_BYTES)) & EVEN_BYTES;
	odd_sum = (((first >> 8) & EVEN_BYTES) + ((second >> 8) & EVEN_BYTES)) & EVEN_BYTES;
	sum = even_sum | (odd_sum << 8);

	/* Reports the sum of the pixels. */
	return sum;
}

/*
 * Gives the floor of the mean of two pixels channel by channel.
 */
static uint32_t
average2(
	uint32_t first,
	uint32_t second)
{
	uint32_t half_first;
	uint32_t half_second;
	uint32_t both_odd;
	uint32_t mean;

	/*
	 * The floor of (a + b) / 2 is a / 2 + b / 2 plus one when both are
	 * odd.  The low bit of every channel is cleared before the shift so
	 * that no bit moves into the channel below.
	 */
	half_first = (first & 0xfefefefeU) >> 1;
	half_second = (second & 0xfefefefeU) >> 1;
	both_odd = first & second & 0x01010101U;
	mean = half_first + half_second + both_odd;

	/* Reports the mean of the pixels. */
	return mean;
}

/*
 * Gives the Select prediction.
 */
static uint32_t
select_pixel(
	uint32_t left,
	uint32_t top,
	uint32_t top_left)
{
	unsigned channel;
	unsigned shift;
	int left_value;
	int top_value;
	int top_left_value;
	int estimate;
	int difference;
	int left_distance;
	int top_distance;
	uint32_t selected;

	/*
	 * Adds up, over the channels, how far the estimate left + top -
	 * top_left lies from the left pixel and from the top pixel.
	 */
	left_distance = 0;
	top_distance = 0;
	for (channel = 0U;
	     channel < CHANNEL_COUNT;
	     channel++) {
		shift = channel * CHANNEL_BITS;
		left_value = (int)((left >> shift) & 0xffU);
		top_value = (int)((top >> shift) & 0xffU);
		top_left_value = (int)((top_left >> shift) & 0xffU);
		estimate = left_value + top_value - top_left_value;

		/* Distance of the estimate from the left pixel's channel. */
		difference = estimate - left_value;
		if (difference < 0)
			difference = -difference;
		left_distance = left_distance + difference;

		/* Distance of the estimate from the top pixel's channel. */
		difference = estimate - top_value;
		if (difference < 0)
			difference = -difference;
		top_distance = top_distance + difference;
	}

	/* The left pixel only when it is strictly nearer; a tie gives the top pixel. */
	if (left_distance < top_distance)
		selected = left;
	else
		selected = top;

	/* Reports the pixel selected. */
	return selected;
}

/*
 * Gives the ClampAddSubtractFull prediction.
 */
static uint32_t
clamp_add_subtract_full(
	uint32_t left,
	uint32_t top,
	uint32_t top_left)
{
	unsigned channel;
	unsigned shift;
	int sum;
	int clamped;
	uint32_t predicted;

	/* Clamps left + top - top_left to a byte in every channel. */
	predicted = 0U;
	for (channel = 0U;
	     channel < CHANNEL_COUNT;
	     channel++) {
		shift = channel * CHANNEL_BITS;
		sum = (int)((left >> shift) & 0xffU) + (int)((top >> shift) & 0xffU) - (int)((top_left >> shift) & 0xffU);
		clamped = webpc_clamp255(sum);
		predicted = predicted | ((uint32_t)clamped << shift);
	}

	/* Reports the prediction. */
	return predicted;
}

/*
 * Gives the ClampAddSubtractHalf prediction of two pixels.
 */
static uint32_t
clamp_add_subtract_half(
	uint32_t first,
	uint32_t second)
{
	unsigned channel;
	unsigned shift;
	int first_value;
	int difference;
	int half;
	int clamped;
	uint32_t predicted;

	/*
	 * Clamps first + (first - second) / 2 to a byte in every channel.  The
	 * division rounds toward zero, as the RFC's C code divides, so a
	 * negative difference is halved on its magnitude.
	 */
	predicted = 0U;
	for (channel = 0U;
	     channel < CHANNEL_COUNT;
	     channel++) {
		shift = channel * CHANNEL_BITS;
		first_value = (int)((first >> shift) & 0xffU);
		difference = first_value - (int)((second >> shift) & 0xffU);
		if (difference < 0)
			half = -((-difference) / 2);
		else
			half = difference / 2;
		clamped = webpc_clamp255(first_value + half);
		predicted = predicted | ((uint32_t)clamped << shift);
	}

	/* Reports the prediction. */
	return predicted;
}

/*
 * Gives the colour transform delta of two bytes read as signed values.
 */
static int
color_delta(
	unsigned t,
	unsigned c)
{
	int signed_t;
	int signed_c;
	int product;
	int delta;

	/* The bytes as signed 8-bit two's complement values, -128..127. */
	signed_t = (int)t;
	if (signed_t >= 128)
		signed_t = signed_t - 256;
	signed_c = (int)c;
	if (signed_c >= 128)
		signed_c = signed_c - 256;

	/* The product of 3.5-fixed-point t and c, shifted down with the floor; at most 16384 in size, so int holds it. */
	product = signed_t * signed_c;
	delta = webpc_asr(product, 5U);

	/* Reports the delta, of which the caller keeps the low 8 bits. */
	return delta;
}
