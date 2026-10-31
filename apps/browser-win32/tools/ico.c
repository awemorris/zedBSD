/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The writer of Windows icon files.
 *
 * The layout follows the icon file format: a 6-byte directory header, one
 * 16-byte entry per image, then the images.  A bitmap image is a 40-byte
 * BITMAPINFOHEADER, the colour pixels (bottom row first) and a 1-bit mask,
 * and every integer is little-endian.
 */

#include "ico.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The bytes of the directory header: reserved, type and count, 2 bytes each. */
#define ICO_HEADER_BYTES	6U

/* The bytes of one directory entry. */
#define ICO_ENTRY_BYTES		16U

/* The bytes of the BITMAPINFOHEADER that starts a bitmap image. */
#define ICO_DIB_HEADER_BYTES	40U

/* The bytes of the signature every PNG file starts with. */
#define ICO_PNG_SIGNATURE_BYTES	8U

/* The most images a directory holds: its count is a 16-bit field, and this writer stops at 255. */
#define ICO_COUNT_MAX		255U

/* The size of a file, an offset and an image length: they are 32-bit fields. */
#define ICO_FILE_MAX		UINT32_MAX

/*
 * The 8 bytes that begin every PNG file.
 *
 * A PNG entry is stored as it is, so this is all the writer checks of it:
 * it refuses bytes that are plainly not a PNG file.
 */
static const unsigned char ico_png_signature[ICO_PNG_SIGNATURE_BYTES] = {
	0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A
};

static int ico_check_image(const struct ico_image *image);
static uint64_t ico_image_bytes(const struct ico_image *image);
static uint64_t ico_mask_row_bytes(uint32_t width);
static unsigned char *ico_put_u16(unsigned char *destination, uint32_t value);
static unsigned char *ico_put_u32(unsigned char *destination, uint32_t value);
static void ico_put_entry(unsigned char *destination, const struct ico_image *image, uint32_t length, uint32_t offset);
static void ico_put_bitmap(unsigned char *destination, const struct ico_image *image);

/*
 * Writes an icon file from images given as pixels or as PNG files.
 *
 * Every image is checked before anything is allocated, so a refused call
 * leaves nothing behind.
 */
int
ico_write(
	const struct ico_image *images,
	unsigned count,
	unsigned char **out,
	size_t *length)
{
	uint64_t total;
	uint64_t image_bytes;
	uint32_t offset;
	unsigned char *file;
	unsigned index;
	int error;

	/* Clears the results first, so every refusal below reports nothing. */
	if (out != NULL)
		*out = NULL;
	if (length != NULL)
		*length = 0;

	/* Refuses calls that cannot name an input or hold an output. */
	if (images == NULL || out == NULL || length == NULL)
		return EINVAL;

	/* Refuses a count the directory cannot hold. */
	if (count == 0U || count > ICO_COUNT_MAX)
		return EINVAL;

	/* Checks every image and adds up the size of the whole file. */
	total = ICO_HEADER_BYTES + (uint64_t)count * ICO_ENTRY_BYTES;
	for (index = 0; index < count; index++) {
		error = ico_check_image(&images[index]);
		if (error != 0)
			return error;

		image_bytes = ico_image_bytes(&images[index]);
		total += image_bytes;

		/* The directory's offsets and lengths are 32 bits, so a bigger file cannot be described. */
		if (total > ICO_FILE_MAX)
			return EINVAL;
	}

	/* Allocates the file zeroed, so the mask bits and the padding start clear. */
	file = (unsigned char *)calloc((size_t)total, 1);
	if (file == NULL)
		return ENOMEM;

	/* Writes the directory header: reserved 0, type 1 for icons, and the count. */
	ico_put_u16(file, 0U);
	ico_put_u16(file + 2, 1U);
	ico_put_u16(file + 4, count);

	/* Writes each directory entry and its image, one after another with no gap. */
	offset = (uint32_t)(ICO_HEADER_BYTES + (uint64_t)count * ICO_ENTRY_BYTES);
	for (index = 0; index < count; index++) {
		image_bytes = ico_image_bytes(&images[index]);
		ico_put_entry(file + ICO_HEADER_BYTES + (size_t)index * ICO_ENTRY_BYTES, &images[index], (uint32_t)image_bytes, offset);

		/* A PNG is copied as it is; pixels are converted to a bitmap. */
		if (images[index].png != NULL) {
			memcpy(file + offset, images[index].png, images[index].png_length);
		} else {
			ico_put_bitmap(file + offset, &images[index]);
		}

		offset += (uint32_t)image_bytes;
	}

	/* Hands the finished file to the caller. */
	*out = file;
	*length = (size_t)total;

	/* Succeeded: the caller owns the file. */
	return 0;
}

/*
 * Checks one image against the rules of the header: a size in range, exactly
 * one of pixels and a PNG, a stride that covers a row, and a PNG signature.
 */
static int
ico_check_image(
	const struct ico_image *image)
{
	size_t room;
	unsigned index;

	/* Refuses a side of 0 or above the largest an icon holds. */
	if (image->width == 0U || image->width > ICO_SIZE_MAX)
		return EINVAL;
	if (image->height == 0U || image->height > ICO_SIZE_MAX)
		return EINVAL;

	/* Refuses an image with both of pixels and a PNG. */
	if (image->pixels != NULL && image->png != NULL)
		return EINVAL;

	/* Refuses an image with neither. */
	if (image->pixels == NULL && image->png == NULL)
		return EINVAL;

	/* Checks a PNG: a length that holds the signature and fits the file, then the signature. */
	if (image->png != NULL) {
		if (image->png_length < ICO_PNG_SIGNATURE_BYTES)
			return EINVAL;
		if ((uint64_t)image->png_length > ICO_FILE_MAX)
			return EINVAL;

		/* Compares the first 8 bytes with the PNG signature. */
		for (index = 0; index < ICO_PNG_SIGNATURE_BYTES; index++) {
			if (image->png[index] != ico_png_signature[index])
				return EINVAL;
		}

		/* Succeeded: a PNG entry. */
		return 0;
	}

	/* Refuses a stride shorter than a row. */
	if (image->stride < image->width)
		return EINVAL;

	/*
	 * Refuses a stride so large that the last row's position cannot be
	 * counted in a size_t, which no real pixel array could reach.
	 */
	if (image->height > 1U) {
		room = (SIZE_MAX - image->width) / (image->height - 1U);
		if (image->stride > room)
			return EINVAL;
	}

	/* Succeeded: a bitmap entry. */
	return 0;
}

/*
 * Reports how many bytes the image takes in the file: the PNG's length, or
 * the header, the colour pixels and the mask of a bitmap.
 *
 * The image must have passed ico_check_image.
 */
static uint64_t
ico_image_bytes(
	const struct ico_image *image)
{
	uint64_t colour_bytes;
	uint64_t mask_bytes;

	/* A PNG is stored as it is. */
	if (image->png != NULL)
		return image->png_length;

	/* A bitmap holds 4 bytes per pixel and a mask of one bit per pixel padded to 32 bits per row. */
	colour_bytes = (uint64_t)image->width * image->height * 4U;
	mask_bytes = ico_mask_row_bytes(image->width) * image->height;

	/* Reports the whole bitmap image. */
	return ICO_DIB_HEADER_BYTES + colour_bytes + mask_bytes;
}

/*
 * Reports the bytes of one row of the 1-bit mask: the bits rounded up to a
 * whole number of 32-bit words.
 */
static uint64_t
ico_mask_row_bytes(
	uint32_t width)
{
	/* Reports 4 bytes for each 32 bits or part of them. */
	return (uint64_t)((width + 31U) / 32U) * 4U;
}

/*
 * Stores a 16-bit value least significant byte first and reports where the
 * next field starts.
 */
static unsigned char *
ico_put_u16(
	unsigned char *destination,
	uint32_t value)
{
	/* The low byte comes first. */
	destination[0] = (unsigned char)(value & 0xFFU);
	destination[1] = (unsigned char)((value >> 8) & 0xFFU);

	/* Reports the byte after the field. */
	return destination + 2;
}

/*
 * Stores a 32-bit value least significant byte first and reports where the
 * next field starts.
 */
static unsigned char *
ico_put_u32(
	unsigned char *destination,
	uint32_t value)
{
	/* The low byte comes first. */
	destination[0] = (unsigned char)(value & 0xFFU);
	destination[1] = (unsigned char)((value >> 8) & 0xFFU);
	destination[2] = (unsigned char)((value >> 16) & 0xFFU);
	destination[3] = (unsigned char)((value >> 24) & 0xFFU);

	/* Reports the byte after the field. */
	return destination + 4;
}

/*
 * Writes one 16-byte directory entry for an image that takes length bytes
 * at offset.
 */
static void
ico_put_entry(
	unsigned char *destination,
	const struct ico_image *image,
	uint32_t length,
	uint32_t offset)
{
	unsigned char *at;
	unsigned char width_byte;
	unsigned char height_byte;

	/* A side of 256 does not fit a byte and is written as 0. */
	width_byte = (unsigned char)image->width;
	if (image->width == ICO_SIZE_MAX)
		width_byte = 0;

	height_byte = (unsigned char)image->height;
	if (image->height == ICO_SIZE_MAX)
		height_byte = 0;

	/* Writes the size, the colour count 0 (for 8 bits or more per pixel) and the reserved byte. */
	destination[0] = width_byte;
	destination[1] = height_byte;
	destination[2] = 0;
	destination[3] = 0;

	/* Writes the planes (1), the bits per pixel (32), the image's length and its offset. */
	at = destination + 4;
	at = ico_put_u16(at, 1U);
	at = ico_put_u16(at, 32U);
	at = ico_put_u32(at, length);
	ico_put_u32(at, offset);
}

/*
 * Writes a bitmap image: the header, the colour pixels bottom row first as
 * blue, green, red and alpha, then the mask rows bottom row first.
 *
 * The destination holds the image's bytes, already zero.  A mask bit is set
 * where the pixel's alpha is 0, and the leftmost pixel is the top bit of a
 * mask byte.
 */
static void
ico_put_bitmap(
	unsigned char *destination,
	const struct ico_image *image)
{
	const uint32_t *source_row;
	unsigned char *at;
	unsigned char *mask_row;
	uint64_t colour_bytes;
	uint64_t mask_row_bytes;
	uint32_t pixel;
	uint32_t row;
	uint32_t column;
	uint32_t source_index;

	colour_bytes = (uint64_t)image->width * image->height * 4U;
	mask_row_bytes = ico_mask_row_bytes(image->width);

	/*
	 * Writes the BITMAPINFOHEADER: its size, the width, the height counted
	 * twice for the mask, planes 1, 32 bits, no compression, and the size of the
	 * colour pixels and the mask.  The remaining fields stay 0.
	 */
	at = destination;
	at = ico_put_u32(at, ICO_DIB_HEADER_BYTES);
	at = ico_put_u32(at, image->width);
	at = ico_put_u32(at, image->height * 2U);
	at = ico_put_u16(at, 1U);
	at = ico_put_u16(at, 32U);
	at = ico_put_u32(at, 0U);
	ico_put_u32(at, (uint32_t)(colour_bytes + mask_row_bytes * image->height));

	/* Writes the colour pixels and the mask of each row, the bottom row of the picture first. */
	at = destination + ICO_DIB_HEADER_BYTES;
	mask_row = destination + ICO_DIB_HEADER_BYTES + colour_bytes;
	for (row = 0; row < image->height; row++) {
		/* The first row written is the picture's last. */
		source_index = image->height - 1U - row;
		source_row = image->pixels + (size_t)source_index * image->stride;

		/* Writes each pixel in blue, green, red, alpha order and marks a transparent one in the mask. */
		for (column = 0; column < image->width; column++) {
			pixel = source_row[column];
			*at = (unsigned char)(pixel & 0xFFU);
			at++;
			*at = (unsigned char)((pixel >> 8) & 0xFFU);
			at++;
			*at = (unsigned char)((pixel >> 16) & 0xFFU);
			at++;
			*at = (unsigned char)((pixel >> 24) & 0xFFU);
			at++;

			/* A fully transparent pixel is marked in the mask, counting from the top bit of each byte. */
			if ((pixel >> 24) == 0U)
				mask_row[column / 8U] |= (unsigned char)(0x80U >> (column % 8U));
		}

		mask_row += mask_row_bytes;
	}
}
