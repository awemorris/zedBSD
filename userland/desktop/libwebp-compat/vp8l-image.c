/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The entropy-coded images of the lossless format: colour cache, meta prefix
 * codes and the pixels.
 */

#include "vp8l-image.h"

#include <errno.h>
#include <stdlib.h>

/* The first green symbol that is a backward reference, and the first that is a colour cache index. */
#define IMAGE_SYMBOL_COPY_FIRST		WEBPC_VP8L_LITERALS
#define IMAGE_SYMBOL_CACHE_FIRST	(WEBPC_VP8L_LITERALS + WEBPC_VP8L_LENGTH_CODES)

/* The widest prefix code of a length or distance value that has extra bits to read: codes 0 to 3 have none. */
#define IMAGE_PREFIX_CODE_LITERAL_MAX	3U

/* The bits of the entropy image's prefix_bits field, and the least value it adds to (RFC 9649 3.7.2.2.1). */
#define IMAGE_PREFIX_BITS_FIELD		3U
#define IMAGE_PREFIX_BITS_MIN		2U

/* The bits of the colour cache's size field (RFC 9649 3.6.2.3). */
#define IMAGE_CACHE_BITS_FIELD		4U

/* The bits a group number occupies in an entropy image pixel, and where (RFC 9649 3.7.2.2.2). */
#define IMAGE_GROUP_MASK		0xffffU
#define IMAGE_GROUP_SHIFT		8U

/* The channel shifts of an ARGB pixel. */
#define IMAGE_SHIFT_ALPHA		24U
#define IMAGE_SHIFT_RED			16U
#define IMAGE_SHIFT_GREEN		8U

/*
 * The state of decoding one entropy-coded image.
 *
 * It owns everything allocated for the image (colour cache, entropy image,
 * groups and the pool their tables live in) and is released as a whole
 * before the decoding returns.
 */
struct image_context {
	struct webpc_lbits *reader;
	unsigned width;
	unsigned height;
	size_t limit;
	unsigned cache_bits;
	uint32_t *cache;
	unsigned prefix_bits;
	size_t entropy_width;
	size_t entropy_count;
	uint32_t *entropy;
	size_t group_count;
	struct webpc_vp8l_group *groups;
	struct webpc_prefix_pool pool;
	struct webpc_prefix_pool scratch;
};

/*
 * The neighbouring pixel offsets (xi, yi) of distance codes 1 to 120, the
 * offset of code n being entry n - 1: xi is the number of pixels to the left
 * of the current one and yi the number of rows above it.
 *
 * Values referenced: RFC 9649 (WebP Image Format, 2024-11) 3.6.2.2.1 Figure
 * 20 (Distance Code to Neighboring Pixel Offset Mapping).
 */
static const int8_t image_distance_map[WEBPC_VP8L_NEAR_CODES][2] = {
	{ 0, 1 }, { 1, 0 }, { 1, 1 }, { -1, 1 }, { 0, 2 }, { 2, 0 },
	{ 1, 2 }, { -1, 2 }, { 2, 1 }, { -2, 1 }, { 2, 2 }, { -2, 2 },
	{ 0, 3 }, { 3, 0 }, { 1, 3 }, { -1, 3 }, { 3, 1 }, { -3, 1 },
	{ 2, 3 }, { -2, 3 }, { 3, 2 }, { -3, 2 }, { 0, 4 }, { 4, 0 },
	{ 1, 4 }, { -1, 4 }, { 4, 1 }, { -4, 1 }, { 3, 3 }, { -3, 3 },
	{ 2, 4 }, { -2, 4 }, { 4, 2 }, { -4, 2 }, { 0, 5 }, { 3, 4 },
	{ -3, 4 }, { 4, 3 }, { -4, 3 }, { 5, 0 }, { 1, 5 }, { -1, 5 },
	{ 5, 1 }, { -5, 1 }, { 2, 5 }, { -2, 5 }, { 5, 2 }, { -5, 2 },
	{ 4, 4 }, { -4, 4 }, { 3, 5 }, { -3, 5 }, { 5, 3 }, { -5, 3 },
	{ 0, 6 }, { 6, 0 }, { 1, 6 }, { -1, 6 }, { 6, 1 }, { -6, 1 },
	{ 2, 6 }, { -2, 6 }, { 6, 2 }, { -6, 2 }, { 4, 5 }, { -4, 5 },
	{ 5, 4 }, { -5, 4 }, { 3, 6 }, { -3, 6 }, { 6, 3 }, { -6, 3 },
	{ 0, 7 }, { 7, 0 }, { 1, 7 }, { -1, 7 }, { 5, 5 }, { -5, 5 },
	{ 7, 1 }, { -7, 1 }, { 4, 6 }, { -4, 6 }, { 6, 4 }, { -6, 4 },
	{ 2, 7 }, { -2, 7 }, { 7, 2 }, { -7, 2 }, { 3, 7 }, { -3, 7 },
	{ 7, 3 }, { -7, 3 }, { 5, 6 }, { -5, 6 }, { 6, 5 }, { -6, 5 },
	{ 8, 0 }, { 4, 7 }, { -4, 7 }, { 7, 4 }, { -7, 4 }, { 8, 1 },
	{ 8, 2 }, { 6, 6 }, { -6, 6 }, { 8, 3 }, { 5, 7 }, { -5, 7 },
	{ 7, 5 }, { -7, 5 }, { 8, 4 }, { 6, 7 }, { -6, 7 }, { 7, 6 },
	{ -7, 6 }, { 8, 5 }, { 7, 7 }, { -7, 7 }, { 8, 6 }, { 8, 7 },
};

static int image_read_cache_info(struct image_context *context);
static int image_read_entropy(struct image_context *context);
static int image_read_groups(struct image_context *context);
static int image_read_pixels(struct image_context *context, uint32_t *pixels);
static int image_check_reader(const struct image_context *context);
static void image_release(struct image_context *context);
static void image_cache_insert(struct image_context *context, uint32_t argb);
static size_t image_group_at(const struct image_context *context, size_t x, size_t y);

/*
 * Decodes one entropy-coded image into pixels.
 *
 * The image is decoded from reader in the order RFC 9649 3.8.3 gives: the
 * colour cache, the meta prefix codes for the main image, the groups of
 * prefix codes and then the pixels.  Everything allocated is freed before
 * returning, and on failure the pixels are unspecified.
 */
int
webpc_vp8l_decode_image(
	struct webpc_lbits *reader,
	unsigned width,
	unsigned height,
	int argb_role,
	size_t limit,
	uint32_t *pixels)
{
	struct image_context context;
	int error;

	/* Refuses a size outside the format's range. */
	if (width < 1U || width > WEBPC_VP8L_SIDE_MAX)
		return EINVAL;
	if (height < 1U || height > WEBPC_VP8L_SIDE_MAX)
		return EINVAL;

	/* Starts a context that owns nothing yet. */
	context.reader = reader;
	context.width = width;
	context.height = height;
	context.limit = limit;
	context.cache_bits = 0;
	context.cache = NULL;
	context.prefix_bits = 0;
	context.entropy_width = 0;
	context.entropy_count = 0;
	context.entropy = NULL;
	context.group_count = 1;
	context.groups = NULL;

	/* Starts the pool of the groups in use and the scratch pool for the groups no block names. */
	webpc_prefix_pool_init(&context.pool, limit);
	webpc_prefix_pool_init(&context.scratch, limit);

	/* Reads the colour cache information. */
	error = image_read_cache_info(&context);

	/* Reads the meta prefix codes, which only the main image has. */
	if (error == 0 && argb_role == 1)
		error = image_read_entropy(&context);

	/* Reads the groups of prefix codes. */
	if (error == 0)
		error = image_read_groups(&context);

	/* Reads the pixels with them. */
	if (error == 0)
		error = image_read_pixels(&context, pixels);

	/* Gives back everything the decoding allocated, whether it succeeded or not. */
	image_release(&context);

	/* Reports why the image could not be decoded. */
	if (error != 0)
		return error;

	/* Succeeded: every pixel is written. */
	return 0;
}

/*
 * Reads the value a length or distance prefix code stands for.
 *
 * The codes 0 to 3 are the values 1 to 4; above them the code gives the
 * number of extra bits and the start of the range the extra bits select in.
 * A code above 39 stands for no value and reads nothing.
 */
uint32_t
webpc_vp8l_lz77_value(
	struct webpc_lbits *reader,
	unsigned prefix_code)
{
	unsigned extra_bits;
	uint32_t offset;
	uint32_t extra;

	/* Refuses a prefix code beyond the 40 the format has. */
	if (prefix_code >= WEBPC_VP8L_DISTANCE_CODES)
		return 0;

	/* The first four codes are the values 1 to 4 with no extra bits. */
	if (prefix_code <= IMAGE_PREFIX_CODE_LITERAL_MAX)
		return prefix_code + 1U;

	/*
	 * Works out where the code's range starts and how many extra bits
	 * select inside it.
	 *
	 * Procedure referenced: RFC 9649 (WebP Image Format, 2024-11) 3.6.2.2
	 * (the pseudocode that obtains a value from the prefix code).
	 */
	extra_bits = (prefix_code - 2U) >> 1;
	offset = (2U + (prefix_code & 1U)) << extra_bits;

	/* Reads the extra bits that pick the value inside the range. */
	extra = webpc_lbits_read(reader, extra_bits);

	/* The value is the range's start, the extra bits and one. */
	return offset + extra + 1U;
}

/*
 * Turns a distance code into a distance in pixels.
 *
 * The codes 1 to 120 name a neighbouring pixel that is turned into a
 * distance in scan-line order for this width, never less than 1; a larger
 * code is the distance plus 120.  Code 0 is no code and gives 1.
 */
size_t
webpc_vp8l_plane_distance(
	uint32_t distance_code,
	unsigned width)
{
	long distance;
	int x_offset;
	int y_offset;

	/* Gives a code beyond the neighbourhood the distance it carries, offset by 120. */
	if (distance_code > WEBPC_VP8L_NEAR_CODES)
		return (size_t)(distance_code - WEBPC_VP8L_NEAR_CODES);

	/* Gives the invalid code 0 the nearest distance, so no table entry before the first is read. */
	if (distance_code == 0U)
		return 1U;

	/*
	 * Looks up the neighbour and turns it into a distance: the columns to
	 * the left plus the rows above times the width.
	 *
	 * Procedure referenced: RFC 9649 (WebP Image Format, 2024-11) 3.6.2.2.1
	 * (dist = xi + yi * image_width, at least 1).
	 */
	x_offset = image_distance_map[distance_code - 1U][0];
	y_offset = image_distance_map[distance_code - 1U][1];
	distance = (long)x_offset + (long)y_offset * (long)width;

	/* The nearest pixel is one back: a neighbour that is not before the current pixel is taken as it. */
	if (distance < 1L)
		return 1U;

	/* The neighbour's distance in pixels. */
	return (size_t)distance;
}

/*
 * Gives the colour cache index of a colour.
 *
 * The index is the top cache_bits bits of the colour times the cache's
 * multiplier, the product taken modulo 2^32.  A size outside 1 to 11 bits
 * gives index 0.
 */
unsigned
webpc_vp8l_cache_index(
	uint32_t argb,
	unsigned cache_bits)
{
	uint32_t product;

	/* Refuses a cache size the format does not have, so the shift below stays inside 32 bits. */
	if (cache_bits < 1U || cache_bits > WEBPC_VP8L_CACHE_BITS_MAX)
		return 0;

	/* Multiplies, wrapping modulo 2^32 as the format's hash does. */
	product = (uint32_t)(WEBPC_VP8L_CACHE_MULTIPLIER * argb);

	/* The index is the product's top cache_bits bits. */
	return (unsigned)(product >> (32U - cache_bits));
}

/*
 * Reads the colour cache information and makes the cache.
 */
static int
image_read_cache_info(
	struct image_context *context)
{
	uint32_t has_cache;
	uint32_t cache_bits;
	int error;

	/* Reads whether a colour cache is used, and its size when it is. */
	has_cache = webpc_lbits_read(context->reader, 1U);
	cache_bits = 0;
	if (has_cache != 0U)
		cache_bits = webpc_lbits_read(context->reader, IMAGE_CACHE_BITS_FIELD);

	/* Reports a stream that ended before the information was whole. */
	error = image_check_reader(context);
	if (error != 0)
		return error;

	/* Without a cache there is nothing to make. */
	if (has_cache == 0U)
		return 0;

	/* Refuses a cache of 0 bits (written but empty) or one beyond 11 bits. */
	if (cache_bits < 1U || cache_bits > WEBPC_VP8L_CACHE_BITS_MAX)
		return EINVAL;

	/* Makes the cache, every entry 0 at the start. */
	context->cache_bits = (unsigned)cache_bits;
	context->cache = (uint32_t *)calloc((size_t)1U << cache_bits, sizeof(uint32_t));
	if (context->cache == NULL)
		return ENOMEM;

	/* Succeeded: the cache is ready. */
	return 0;
}

/*
 * Reads the meta prefix codes: the entropy image and the number of groups.
 */
static int
image_read_entropy(
	struct image_context *context)
{
	uint32_t has_meta;
	size_t block_size;
	size_t entropy_height;
	size_t index;
	uint32_t largest;
	uint32_t group;
	int error;

	/* Reads whether the image has meta prefix codes. */
	has_meta = webpc_lbits_read(context->reader, 1U);
	error = image_check_reader(context);
	if (error != 0)
		return error;

	/* One group serves the whole image when there are none. */
	if (has_meta == 0U)
		return 0;

	/* Reads the size of the blocks, which gives the size of the entropy image. */
	context->prefix_bits = IMAGE_PREFIX_BITS_MIN + webpc_lbits_read(context->reader, IMAGE_PREFIX_BITS_FIELD);
	error = image_check_reader(context);
	if (error != 0)
		return error;

	/* Works out the entropy image's size: one pixel per block, the blocks at the edges partial. */
	block_size = (size_t)1U << context->prefix_bits;
	context->entropy_width = ((size_t)context->width + block_size - 1U) >> context->prefix_bits;
	entropy_height = ((size_t)context->height + block_size - 1U) >> context->prefix_bits;
	context->entropy_count = context->entropy_width * entropy_height;

	/* Allocates the entropy image. */
	context->entropy = (uint32_t *)malloc(context->entropy_count * sizeof(uint32_t));
	if (context->entropy == NULL)
		return ENOMEM;

	/* Decodes the entropy image, an image of its own with no meta prefix codes. */
	error = webpc_vp8l_decode_image(context->reader, (unsigned)context->entropy_width, (unsigned)entropy_height, 0,
		context->limit, context->entropy);
	if (error != 0)
		return error;

	/* Finds the largest group number any block names; there is one group up to it. */
	largest = 0;
	for (index = 0; index < context->entropy_count; index++) {
		group = (context->entropy[index] >> IMAGE_GROUP_SHIFT) & IMAGE_GROUP_MASK;
		if (group > largest)
			largest = group;
	}
	context->group_count = (size_t)largest + 1U;

	/* Succeeded: the entropy image and the group count are known. */
	return 0;
}

/*
 * Reads the groups of prefix codes, keeping those some block names and
 * dropping the rest.
 */
static int
image_read_groups(
	struct image_context *context)
{
	struct webpc_vp8l_group unused_group;
	unsigned char *used;
	size_t index;
	uint32_t group;
	int error;

	/* Allocates the groups that are kept. */
	context->groups = (struct webpc_vp8l_group *)malloc(context->group_count * sizeof(struct webpc_vp8l_group));
	if (context->groups == NULL)
		return ENOMEM;

	/* Notes which groups the blocks name; without an entropy image the one group is in use. */
	used = (unsigned char *)calloc(context->group_count, 1U);
	if (used == NULL)
		return ENOMEM;
	if (context->entropy == NULL) {
		used[0] = 1;
	} else {
		/* Marks the group of every block. */
		for (index = 0; index < context->entropy_count; index++) {
			group = (context->entropy[index] >> IMAGE_GROUP_SHIFT) & IMAGE_GROUP_MASK;
			used[group] = 1;
		}
	}

	/* Reads every group in order: a group in use into the kept pool, any other into the scratch pool. */
	error = 0;
	for (index = 0; index < context->group_count && error == 0; index++) {
		if (used[index] != 0) {
			error = webpc_vp8l_read_group(context->reader, &context->pool, context->cache_bits,
				&context->groups[index]);
		} else {
			error = webpc_vp8l_read_group(context->reader, &context->scratch, context->cache_bits,
				&unused_group);

			/* A group nothing uses leaves no table behind: the scratch pool starts empty again. */
			context->scratch.count = 0;
		}
	}

	/* Gives back the marks. */
	free(used);

	/* Reports a group that could not be read. */
	if (error != 0)
		return error;

	/* Reports a stream that ended inside the codes. */
	error = image_check_reader(context);
	if (error != 0)
		return error;

	/* Succeeded: the groups in use are built. */
	return 0;
}

/*
 * Reads the pixels in scan-line order.
 */
static int
image_read_pixels(
	struct image_context *context,
	uint32_t *pixels)
{
	const struct webpc_vp8l_group *group;
	size_t total;
	size_t position;
	size_t x;
	size_t y;
	size_t distance;
	size_t copied;
	size_t cache_size;
	unsigned symbol;
	unsigned cache_index;
	uint32_t length;
	uint32_t distance_code;
	uint32_t red;
	uint32_t blue;
	uint32_t alpha;
	uint32_t argb;
	int error;

	/* Starts at the first pixel of the image. */
	total = (size_t)context->width * (size_t)context->height;
	position = 0;
	x = 0;
	y = 0;
	cache_size = 0;
	if (context->cache != NULL)
		cache_size = (size_t)1U << context->cache_bits;

	/* Reads one literal, one backward reference or one cache index at a time until the image is full. */
	while (position < total) {
		/* The group of the block the symbol starts in (groups are as many as the largest number plus one). */
		group = &context->groups[image_group_at(context, x, y)];

		/* Reads the green symbol, which says which of the three methods this is. */
		symbol = webpc_prefix_read(&context->pool, &group->codes[WEBPC_VP8L_CODE_GREEN], context->reader);

		/* Decodes the pixel or the run of pixels by the method the symbol selects. */
		if (symbol < IMAGE_SYMBOL_COPY_FIRST) {
			/* A literal: the symbol is green, and red, blue and alpha follow with their own codes. */
			red = webpc_prefix_read(&context->pool, &group->codes[WEBPC_VP8L_CODE_RED], context->reader);
			blue = webpc_prefix_read(&context->pool, &group->codes[WEBPC_VP8L_CODE_BLUE], context->reader);
			alpha = webpc_prefix_read(&context->pool, &group->codes[WEBPC_VP8L_CODE_ALPHA], context->reader);

			/* Reports a stream that ended inside the pixel. */
			error = image_check_reader(context);
			if (error != 0)
				return error;

			/* Puts the pixel in the image and the cache. */
			argb = ((uint32_t)alpha << IMAGE_SHIFT_ALPHA) | (red << IMAGE_SHIFT_RED) |
			    ((uint32_t)symbol << IMAGE_SHIFT_GREEN) | blue;
			pixels[position] = argb;
			image_cache_insert(context, argb);
			copied = 1;
		} else if (symbol < IMAGE_SYMBOL_CACHE_FIRST) {
			/* A backward reference: its length from the green symbol, then a distance symbol. */
			length = webpc_vp8l_lz77_value(context->reader, symbol - IMAGE_SYMBOL_COPY_FIRST);
			symbol = webpc_prefix_read(&context->pool, &group->codes[WEBPC_VP8L_CODE_DISTANCE], context->reader);
			distance_code = webpc_vp8l_lz77_value(context->reader, symbol);

			/* Reports a stream that ended inside the reference, before its values are judged. */
			error = image_check_reader(context);
			if (error != 0)
				return error;

			/* Turns the distance code into pixels for this image's width. */
			distance = webpc_vp8l_plane_distance(distance_code, context->width);

			/* Refuses a reference that reaches before the first pixel. */
			if (distance > position)
				return EINVAL;

			/* Refuses a reference that runs past the last pixel. */
			if ((size_t)length > total - position)
				return EINVAL;

			/* Copies the pixels one at a time, so a copy may overlap what it makes, and caches each. */
			for (copied = 0; copied < (size_t)length; copied++) {
				argb = pixels[position + copied - distance];
				pixels[position + copied] = argb;
				image_cache_insert(context, argb);
			}
		} else {
			/* A colour cache index: the symbol past the lengths names the entry. */
			cache_index = symbol - IMAGE_SYMBOL_CACHE_FIRST;

			/* Reports a stream that ended inside the symbol. */
			error = image_check_reader(context);
			if (error != 0)
				return error;

			/* Refuses an index beyond the cache (the alphabet keeps it inside, this keeps the read safe). */
			if (cache_index >= cache_size)
				return EINVAL;

			/* Takes the colour from the cache and puts it in the image; it enters the cache again. */
			argb = context->cache[cache_index];
			pixels[position] = argb;
			image_cache_insert(context, argb);
			copied = 1;
		}

		/* Moves the position past what was written, to the next row when it passes the end of this one. */
		position += copied;
		x += copied;
		y += x / (size_t)context->width;
		x = x % (size_t)context->width;
	}

	/* Reports a stream that ended inside the last pixel. */
	error = image_check_reader(context);
	if (error != 0)
		return error;

	/* Succeeded: the image is full. */
	return 0;
}

/*
 * Tells that the reader ran past the end of its data as the error ENOBUFS.
 */
static int
image_check_reader(
	const struct image_context *context)
{
	int ended;

	/* Asks the reader whether it consumed a bit that is not there. */
	ended = webpc_lbits_ended(context->reader);

	/* Reports a stream that is cut short. */
	if (ended != 0)
		return ENOBUFS;

	/* Succeeded: the data has not run out. */
	return 0;
}

/*
 * Frees everything the context owns.
 */
static void
image_release(
	struct image_context *context)
{
	/* Frees the cache, the entropy image and the groups. */
	free(context->cache);
	free(context->entropy);
	free(context->groups);

	/* Frees the tables of the groups. */
	webpc_prefix_pool_release(&context->pool);
	webpc_prefix_pool_release(&context->scratch);

	/* Leaves nothing pointing at what was freed. */
	context->cache = NULL;
	context->entropy = NULL;
	context->groups = NULL;
}

/*
 * Puts a pixel into the colour cache, when there is one.
 */
static void
image_cache_insert(
	struct image_context *context,
	uint32_t argb)
{
	unsigned index;

	/* An image without a cache has nothing to update. */
	if (context->cache == NULL)
		return;

	/* The entry the colour hashes to takes the colour, whatever it held. */
	index = webpc_vp8l_cache_index(argb, context->cache_bits);
	context->cache[index] = argb;
}

/*
 * Gives the number of the group that decodes the pixel at column x, row y.
 */
static size_t
image_group_at(
	const struct image_context *context,
	size_t x,
	size_t y)
{
	size_t position;
	uint32_t group;

	/* One group serves the whole image when there is no entropy image. */
	if (context->entropy == NULL)
		return 0;

	/*
	 * Finds the block the pixel is in and takes the group its entropy image
	 * pixel names.
	 *
	 * Procedure referenced: RFC 9649 (WebP Image Format, 2024-11) 3.7.2.2.2
	 * (position, meta_prefix_code).
	 */
	position = (y >> context->prefix_bits) * context->entropy_width + (x >> context->prefix_bits);
	group = (context->entropy[position] >> IMAGE_GROUP_SHIFT) & IMAGE_GROUP_MASK;

	/* The group number of the block. */
	return (size_t)group;
}
