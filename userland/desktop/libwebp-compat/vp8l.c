/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The lossless decoder: header, transforms, image and inverse transforms.
 */

#include "vp8l.h"
#include "lbits.h"
#include "vp8l-image.h"
#include "vp8l-transform.h"

#include <errno.h>
#include <stdlib.h>

/* The number of bits of each of the width and the height in the header. */
#define HEADER_SIDE_BITS	14U

/* The number of bits of the transform type. */
#define TRANSFORM_TYPE_BITS	2U

/* The number of bits of the size field of a predictor or colour transform. */
#define TRANSFORM_SIZE_BITS	3U

/* The least size_bits of a predictor or colour transform: 2 + (3 bits). */
#define TRANSFORM_SIZE_BASE	2U

/* The number of bits of the colour count of a colour indexing transform. */
#define COLOR_COUNT_BITS	8U

/*
 * One transform read from the stream, kept until it is undone.
 *
 * It is the stream's own record: it lives from the moment the transform's
 * data is read until the image has been reconstructed, and the data it
 * points to is freed with it.
 */
struct vp8l_transform {
	/* The type, one of the WEBPC_VP8L_ transform types (RFC 9649 3.5 Table 1). */
	unsigned type;

	/*
	 * size_bits for a predictor or colour transform, width_bits for a
	 * colour indexing transform, unused for subtract green.
	 */
	unsigned bits;

	/* The image width at the moment the transform was read (before any bundling of its own). */
	unsigned width;

	/*
	 * The predictor image, colour transform image or colour table
	 * (WEBPC_VP8L_COLOR_TABLE_SIZE entries); NULL for subtract green.
	 */
	uint32_t *data;
};

/* Reads the transforms and the image, and undoes the transforms. */
static int decode_body(struct webpc_lbits *reader, unsigned width, unsigned height, uint32_t *pixels,
	struct vp8l_transform *transforms, unsigned *transform_count);

/* Reads the data of one transform of the given type from the stream. */
static int read_transform(struct webpc_lbits *reader, unsigned type, unsigned *current_width, unsigned height,
	struct vp8l_transform *transform);

/* Reads the colour table of a colour indexing transform and sums its deltas. */
static int read_color_table(struct webpc_lbits *reader, struct vp8l_transform *transform, unsigned *current_width);

/* Gives the width_bits of a colour table of the given number of colours. */
static unsigned indexing_width_bits(unsigned colors);

/* Undoes one transform over the pixels, which are in the state the transform's own input had. */
static void undo_transform(const struct vp8l_transform *transform, unsigned height, uint32_t *pixels);

/*
 * Reads the header of a lossless stream (data from its signature byte, size
 * bytes): signature 0x2f, width and height (14 bits each, plus 1),
 * alpha_is_used (1 bit), version_number (3 bits, which must be 0).
 */
int
webpc_vp8l_read_header(
	const uint8_t *data,
	size_t size,
	struct webpc_vp8l_header *header)
{
	uint32_t fields;
	uint32_t side_mask;
	unsigned version;

	/* Refuses a stream too short to hold the header. */
	if (size < WEBPC_VP8L_HEADER_SIZE)
		return ENOBUFS;

	/* Refuses a missing buffer or output that a stream of this size must have. */
	if (data == NULL || header == NULL)
		return EINVAL;

	/* Refuses a stream that does not start with the lossless signature. */
	if (data[0] != WEBPC_VP8L_SIGNATURE)
		return EINVAL;

	/*
	 * The 32 bits after the signature, the first bit read being bit 0:
	 * width - 1 (14 bits), height - 1 (14 bits), alpha_is_used (1 bit)
	 * and version_number (3 bits).
	 */
	fields = webpc_le32(data + 1);
	side_mask = (1UL << HEADER_SIDE_BITS) - 1U;
	version = (unsigned)(fields >> (2U * HEADER_SIDE_BITS + 1U));

	/* Refuses a version the format does not define. */
	if (version != 0U)
		return EINVAL;

	/* Publishes what the header says. */
	header->width = (unsigned)(fields & side_mask) + 1U;
	header->height = (unsigned)((fields >> HEADER_SIDE_BITS) & side_mask) + 1U;
	header->alpha_is_used = (int)((fields >> (2U * HEADER_SIDE_BITS)) & 1U);

	/* Succeeded: the header is read. */
	return 0;
}

/*
 * Decodes a whole lossless stream (data from its signature byte, size bytes)
 * into a new array of width * height ARGB pixels (as webpc_vp8l_decode_image
 * lays them out), which *pixels receives and the caller frees with free().
 */
int
webpc_vp8l_decode(
	const uint8_t *data,
	size_t size,
	uint32_t **pixels,
	unsigned *width,
	unsigned *height)
{
	struct webpc_vp8l_header header;
	uint32_t *array;
	size_t count;
	int error;

	/* Refuses missing outputs. */
	if (pixels == NULL || width == NULL || height == NULL)
		return EINVAL;

	/* Gives no array until one is decoded. */
	*pixels = NULL;

	/* Reads the header, which gives the size of the image. */
	error = webpc_vp8l_read_header(data, size, &header);
	if (error != 0)
		return error;

	/* Allocates the pixels (a side is at most 16384, so the count and its bytes fit). */
	count = (size_t)header.width * (size_t)header.height;
	array = (uint32_t *)malloc(count * sizeof(uint32_t));
	if (array == NULL)
		return ENOMEM;

	/* Decodes the image-stream that follows the header. */
	error = webpc_vp8l_decode_stream(data + WEBPC_VP8L_HEADER_SIZE, size - WEBPC_VP8L_HEADER_SIZE, header.width,
		header.height, array);
	if (error != 0) {
		free(array);
		return error;
	}

	/* Hands the pixels to the caller. */
	*pixels = array;
	*width = header.width;
	*height = header.height;

	/* Succeeded: the caller owns the pixels. */
	return 0;
}

/*
 * Decodes an image-stream without header (RFC 9649 3.8.1: optional
 * transforms then the spatially coded image; an ALPH chunk's compressed
 * data, RFC 9649 2.7.1.2) of width by height pixels (each 1 or more, at most
 * WEBPC_VP8L_SIDE_MAX) from data (size bytes) into pixels (the caller's
 * width * height ARGB values).
 */
int
webpc_vp8l_decode_stream(
	const uint8_t *data,
	size_t size,
	unsigned width,
	unsigned height,
	uint32_t *pixels)
{
	struct webpc_lbits reader;
	struct vp8l_transform transforms[WEBPC_VP8L_TRANSFORMS_MAX];
	unsigned transform_count;
	unsigned index;
	int error;

	/* Refuses an image with no room for pixels or a side the format cannot have. */
	if (pixels == NULL)
		return EINVAL;
	if (width < 1U || width > WEBPC_VP8L_SIDE_MAX)
		return EINVAL;
	if (height < 1U || height > WEBPC_VP8L_SIDE_MAX)
		return EINVAL;

	/* Starts reading at the first bit of the stream. */
	webpc_lbits_init(&reader, data, size);

	/* Reads the transforms and the image and undoes the transforms. */
	transform_count = 0U;
	error = decode_body(&reader, width, height, pixels, transforms, &transform_count);

	/* Frees what the transforms read, whether or not the decoding succeeded. */
	for (index = 0U; index < transform_count; index++)
		free(transforms[index].data);

	/* Reports why the decoding failed. */
	if (error != 0)
		return error;

	/* Succeeded: every pixel is written. */
	return 0;
}

/*
 * Reads the transforms while a 1 bit announces one, decodes the image at the
 * width the transforms leave, and undoes the transforms in the reverse of the
 * order they were read.  transform_count says how many records are filled in
 * (their data is the caller's to free), also when an error is returned.
 */
static int
decode_body(
	struct webpc_lbits *reader,
	unsigned width,
	unsigned height,
	uint32_t *pixels,
	struct vp8l_transform *transforms,
	unsigned *transform_count)
{
	unsigned current_width;
	unsigned used_types;
	unsigned type;
	unsigned present;
	unsigned index;
	int error;

	/* The width the next thing is read at: the full width until a colour indexing transform bundles pixels. */
	current_width = width;
	used_types = 0U;

	/* Reads each transform the stream announces, each type at most once. */
	for (;;) {
		/* Reads the bit that says whether another transform follows. */
		present = webpc_lbits_read(reader, 1U);
		if (webpc_lbits_ended(reader) != 0)
			return ENOBUFS;

		/* Stops at the 0 bit that ends the list of transforms. */
		if (present == 0U)
			break;

		/* Reads the type of this transform. */
		type = webpc_lbits_read(reader, TRANSFORM_TYPE_BITS);

		/* Refuses a second transform of one type. */
		if ((used_types & (1U << type)) != 0U)
			return EINVAL;
		used_types |= 1U << type;

		/*
		 * Takes the next record into the count before reading, so that
		 * the caller frees whatever its data holds when the read fails.
		 */
		index = *transform_count;
		transforms[index].data = NULL;
		(*transform_count)++;

		/* Reads the data of this transform into that record. */
		error = read_transform(reader, type, &current_width, height, &transforms[index]);
		if (error != 0)
			return error;
	}

	/* Decodes the image itself, at the width the transforms left, into the start of the caller's pixels. */
	error = webpc_vp8l_decode_image(reader, current_width, height, 1, WEBPC_VP8L_TABLE_ENTRIES_MAX, pixels);
	if (error != 0)
		return error;

	/* Undoes the transforms, the last one read first. */
	for (index = *transform_count; index > 0U; index--)
		undo_transform(&transforms[index - 1U], height, pixels);

	/* Succeeded: pixels hold the image. */
	return 0;
}

/*
 * Reads the data of one transform: the sizes and the subresolution image of a
 * predictor or colour transform, nothing for subtract green, the colour table
 * of colour indexing (which narrows current_width).  The record is filled in
 * and its data allocated; when an error is returned the data is NULL or
 * allocated, and the caller frees it either way.
 */
static int
read_transform(
	struct webpc_lbits *reader,
	unsigned type,
	unsigned *current_width,
	unsigned height,
	struct vp8l_transform *transform)
{
	unsigned size_bits;
	unsigned sub_width;
	unsigned sub_height;
	size_t count;
	int error;

	/* Records the type and the width the transform is read at. */
	transform->type = type;
	transform->bits = 0U;
	transform->width = *current_width;

	/* Subtract green has no data. */
	if (type == WEBPC_VP8L_SUBTRACT_GREEN)
		return 0;

	/* Colour indexing has its own data. */
	if (type == WEBPC_VP8L_COLOR_INDEXING) {
		error = read_color_table(reader, transform, current_width);
		if (error != 0)
			return error;

		/* Succeeded: the colour table is read. */
		return 0;
	}

	/* Reads size_bits, which is 2 plus a 3-bit field, so 2..9. */
	size_bits = TRANSFORM_SIZE_BASE + webpc_lbits_read(reader, TRANSFORM_SIZE_BITS);
	if (webpc_lbits_ended(reader) != 0)
		return ENOBUFS;
	transform->bits = size_bits;

	/* Works out the size of the subresolution image, which covers the image at the current width. */
	sub_width = webpc_vp8l_subsample(*current_width, size_bits);
	sub_height = webpc_vp8l_subsample(height, size_bits);
	count = (size_t)sub_width * (size_t)sub_height;

	/* Allocates the subresolution image. */
	transform->data = (uint32_t *)malloc(count * sizeof(uint32_t));
	if (transform->data == NULL)
		return ENOMEM;

	/* Reads the predictor image or colour transform image, which has no transforms and no meta prefix codes. */
	error = webpc_vp8l_decode_image(reader, sub_width, sub_height, 0, WEBPC_VP8L_TABLE_ENTRIES_MAX, transform->data);
	if (error != 0)
		return error;

	/* Succeeded: the subresolution image is read. */
	return 0;
}

/*
 * Reads the colour count and the colour table of a colour indexing transform
 * (RFC 9649 3.5.4), sums the deltas of the table, pads it, and narrows the
 * current width by the bundling the count calls for.
 */
static int
read_color_table(
	struct webpc_lbits *reader,
	struct vp8l_transform *transform,
	unsigned *current_width)
{
	unsigned color_count;
	unsigned width_bits;
	unsigned entry;
	unsigned channel;
	uint32_t previous;
	uint32_t delta;
	uint32_t sum;
	uint32_t byte_sum;
	int error;

	/* Reads color_table_size, which is 1 plus an 8-bit field. */
	color_count = webpc_lbits_read(reader, COLOR_COUNT_BITS) + 1U;
	if (webpc_lbits_ended(reader) != 0)
		return ENOBUFS;

	/* Allocates the table, padded so that any 8-bit index finds an entry. */
	transform->data = (uint32_t *)malloc(WEBPC_VP8L_COLOR_TABLE_SIZE * sizeof(uint32_t));
	if (transform->data == NULL)
		return ENOMEM;

	/* Reads the table as an image of color_count by 1 pixels, which has no transforms and no meta prefix codes. */
	error = webpc_vp8l_decode_image(reader, color_count, 1U, 0, WEBPC_VP8L_TABLE_ENTRIES_MAX, transform->data);
	if (error != 0)
		return error;

	/* Sums the deltas: each entry is the previous colour plus its own values, channel by channel, modulo 256. */
	previous = transform->data[0];
	for (entry = 1U; entry < color_count; entry++) {
		delta = transform->data[entry];
		sum = 0U;

		/* Adds the four channels one by one, each wrapping at 256. */
		for (channel = 0U; channel < 4U; channel++) {
			byte_sum = ((previous >> (8U * channel)) & 0xffU) + ((delta >> (8U * channel)) & 0xffU);
			sum |= (byte_sum & 0xffU) << (8U * channel);
		}
		transform->data[entry] = sum;
		previous = sum;
	}

	/* Pads the table with transparent black, which the indices past the colour count find. */
	for (entry = color_count; entry < WEBPC_VP8L_COLOR_TABLE_SIZE; entry++)
		transform->data[entry] = 0U;

	/* Bundles pixels as the colour count says, which narrows what is read from here on. */
	width_bits = indexing_width_bits(color_count);
	transform->bits = width_bits;
	*current_width = webpc_vp8l_subsample(*current_width, width_bits);

	/* Succeeded: the colour table is read. */
	return 0;
}

/*
 * Gives the number of bits by which a colour table of the given number of
 * colours (1..256) bundles pixels.
 *
 * Values referenced: RFC 9649 (WebP Image Format, 2024-11) 3.5.4 Table 3
 * (Color Table Size to Bundled Pixel Bit Width Mapping): 1..2 colours give
 * 3, 3..4 give 2, 5..16 give 1, 17..256 give 0.
 */
static unsigned
indexing_width_bits(
	unsigned colors)
{
	/* Eight pixels fit one pixel's green for one or two colours. */
	if (colors <= 2U)
		return 3U;

	/* Four pixels for three or four colours. */
	if (colors <= 4U)
		return 2U;

	/* Two pixels for five to sixteen colours. */
	if (colors <= 16U)
		return 1U;

	/* Larger tables are not bundled. */
	return 0U;
}

/*
 * Undoes one transform over the pixels.  The pixels are what the transform's
 * output was, at the width after the transform: for colour indexing the packed
 * rows at the start of the buffer, which the expansion widens in place.
 */
static void
undo_transform(
	const struct vp8l_transform *transform,
	unsigned height,
	uint32_t *pixels)
{
	size_t count;

	/* Undoes the transform by its type, each at the width it was read at. */
	switch (transform->type) {
	case WEBPC_VP8L_PREDICTOR:
		webpc_vp8l_inverse_predictor(pixels, transform->width, height, transform->data, transform->bits);
		break;
	case WEBPC_VP8L_COLOR:
		webpc_vp8l_inverse_color(pixels, transform->width, height, transform->data, transform->bits);
		break;
	case WEBPC_VP8L_SUBTRACT_GREEN:
		count = (size_t)transform->width * (size_t)height;
		webpc_vp8l_add_green(pixels, count);
		break;
	default:
		/* Colour indexing: the packed image becomes the full-width image in the same buffer. */
		webpc_vp8l_inverse_indexing(pixels, transform->width, height, transform->bits, transform->data, pixels);
		break;
	}
}
