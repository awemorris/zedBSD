/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The unit test of the icon writer (tools/ico.c): the exact bytes of a
 * 16 x 16 bitmap icon written out by hand from the file format, the mask
 * padding at sizes that are not multiples of 32, PNG entries, a directory of
 * eight sizes, every refusal, and a round trip through a small reader of
 * the format that this file carries.
 */

#include "check.h"

#include "ico.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * What the reader found in one directory entry and its image: the entry's
 * fields as they stand in the file, and, for a bitmap, the pixels and the
 * mask bits it decoded.
 */
struct read_image {
	unsigned entry_width;
	unsigned entry_height;
	unsigned long length;
	unsigned long offset;
	int is_png;
	uint32_t width;
	uint32_t height;
	uint32_t *pixels;
	unsigned char *mask_bits;
};

static void test_exact_16(void);
static void test_mask_padding(void);
static void test_stride(void);
static void test_png_entry(void);
static void test_eight_sizes(void);
static void test_count_limits(void);
static void test_errors(void);
static void test_large_sizes(void);
static void test_alpha_zero(void);
static void make_pixels(uint32_t *pixels, uint32_t width, uint32_t height, size_t stride, uint32_t seed);
static uint32_t pixel_at(uint32_t x, uint32_t y, uint32_t seed);
static void make_png(unsigned char *bytes, size_t length, unsigned char fill);
static unsigned get_u16(const unsigned char *bytes);
static unsigned long get_u32(const unsigned char *bytes);
static int read_image(const unsigned char *file, size_t length, unsigned index, struct read_image *found);
static void free_read(struct read_image *found);
static void check_round_trip(uint32_t width, uint32_t height, size_t stride, unsigned long bitmap_bytes);
static void check_refused(const struct ico_image *images, unsigned count);

/*
 * Runs every group of checks.
 */
int
main(
	int argc,
	char **argv)
{
	int differs;

	/* The harness passes the system's name. */
	CHECK(argc == 2);
	differs = 1;
	if (argc == 2)
		differs = strcmp(argv[1], "posix") != 0 && strcmp(argv[1], "win32") != 0;
	CHECK(differs == 0);

	/* Each group of checks. */
	test_exact_16();
	test_mask_padding();
	test_stride();
	test_png_entry();
	test_eight_sizes();
	test_count_limits();
	test_errors();
	test_large_sizes();
	test_alpha_zero();

	/* Reports the result. */
	return check_finish("ico");
}

/*
 * Checks the whole file of one 16 x 16 icon against bytes written out by
 * hand from the format: every header field, the pixels from the bottom
 * row, and the mask.
 */
static void
test_exact_16(void)
{
	static const unsigned char head[22 + 40] = {
		/* ICONDIR: reserved 0, type 1, one image. */
		0x00, 0x00, 0x01, 0x00, 0x01, 0x00,
		/* ICONDIRENTRY: 16 x 16, colours 0, reserved 0, planes 1, 32 bits, 1128 bytes at offset 22. */
		0x10, 0x10, 0x00, 0x00, 0x01, 0x00, 0x20, 0x00,
		0x68, 0x04, 0x00, 0x00, 0x16, 0x00, 0x00, 0x00,
		/* BITMAPINFOHEADER: size 40, width 16, height 32, planes 1, 32 bits, no compression, image 1088 bytes. */
		0x28, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00,
		0x20, 0x00, 0x00, 0x00, 0x01, 0x00, 0x20, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x40, 0x04, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};
	uint32_t pixels[16 * 16];
	struct ico_image image;
	unsigned char *out;
	size_t length;
	size_t at;
	unsigned x;
	unsigned y;
	unsigned bottom;
	uint32_t pixel;
	unsigned bit_mask;
	unsigned expected_byte;
	int error;

	/* The picture has transparent and opaque pixels of different colours. */
	make_pixels(pixels, 16, 16, 16, 1U);
	memset(&image, 0, sizeof(image));
	image.width = 16;
	image.height = 16;
	image.pixels = pixels;
	image.stride = 16;
	out = (unsigned char *)1;
	length = 99;

	error = ico_write(&image, 1, &out, &length);
	CHECK_EQ(error, 0);
	CHECK(out != NULL);
	CHECK_EQ(length, 22 + 1128);
	if (out == NULL || length != 1150U) {
		free(out);
		return;
	}

	/* The directory and the bitmap header are the bytes written out above. */
	CHECK(memcmp(out, head, sizeof(head)) == 0);

	/* The first colour row is the picture's last row, each pixel as blue, green, red, alpha. */
	for (bottom = 0; bottom < 16; bottom++) {
		y = 15 - bottom;
		for (x = 0; x < 16; x++) {
			pixel = pixel_at(x, y, 1U);
			at = 62 + (size_t)bottom * 64 + (size_t)x * 4;
			CHECK_EQ(out[at], pixel & 0xFFU);
			CHECK_EQ(out[at + 1], (pixel >> 8) & 0xFFU);
			CHECK_EQ(out[at + 2], (pixel >> 16) & 0xFFU);
			CHECK_EQ(out[at + 3], (pixel >> 24) & 0xFFU);
		}
	}

	/* One mask row is 4 bytes: 16 bits and 16 bits of padding. */
	for (bottom = 0; bottom < 16; bottom++) {
		y = 15 - bottom;
		at = 62 + 1024 + (size_t)bottom * 4;
		for (x = 0; x < 16; x += 8) {
			expected_byte = 0;
			for (bit_mask = 0; bit_mask < 8; bit_mask++) {
				pixel = pixel_at(x + bit_mask, y, 1U);
				if ((pixel >> 24) == 0U)
					expected_byte |= 0x80U >> bit_mask;
			}
			CHECK_EQ(out[at + x / 8], expected_byte);
		}
		CHECK_EQ(out[at + 2], 0);
		CHECK_EQ(out[at + 3], 0);
	}

	/* The hand-chosen pattern has both kinds of pixel, so the mask checks mean something. */
	CHECK((pixel_at(2, 0, 1U) >> 24) == 0U);
	CHECK((pixel_at(1, 0, 1U) >> 24) != 0U);

	free(out);
}

/*
 * Checks sizes whose mask rows need padding to 32 bits, by the sizes the
 * format gives (a mask row is 4 bytes for 1 to 32 pixels, 8 for 33 to 64,
 * and so on) and by a round trip.
 */
static void
test_mask_padding(void)
{
	/* 20 x 20: colours 1600 + masks 4 x 20 = 80. */
	check_round_trip(20, 20, 20, 40 + 1600 + 80);

	/* 24 x 24: colours 2304 + masks 4 x 24 = 96. */
	check_round_trip(24, 24, 24, 40 + 2304 + 96);

	/* 40 x 40: colours 6400 + masks 8 x 40 = 320. */
	check_round_trip(40, 40, 40, 40 + 6400 + 320);

	/* 48 x 48: colours 9216 + masks 8 x 48 = 384. */
	check_round_trip(48, 48, 48, 40 + 9216 + 384);

	/* 32 x 32 fills a mask word exactly: colours 4096 + masks 4 x 32 = 128. */
	check_round_trip(32, 32, 32, 40 + 4096 + 128);

	/* 33 wide needs a second word: colours 33 x 2 x 4 = 264 + masks 8 x 2 = 16. */
	check_round_trip(33, 2, 33, 40 + 264 + 16);

	/* 1 x 1: colours 4 + mask 4. */
	check_round_trip(1, 1, 1, 40 + 4 + 4);

	/* Not square: 5 wide and 3 high, colours 60 + masks 4 x 3 = 12. */
	check_round_trip(5, 3, 5, 40 + 60 + 12);
	check_round_trip(3, 5, 3, 40 + 60 + 20);

	/* 65 wide: three words per mask row = 12 bytes; colours 65 x 4 = 260. */
	check_round_trip(65, 1, 65, 40 + 260 + 12);
}

/*
 * Checks that a stride wider than the image skips the extra pixels: the
 * output equals that of the same picture packed tightly, and the extra
 * pixels never reach the file.
 */
static void
test_stride(void)
{
	uint32_t *wide;
	uint32_t packed[7 * 5];
	struct ico_image image;
	unsigned char *tight_out;
	unsigned char *wide_out;
	size_t tight_length;
	size_t wide_length;
	size_t index;
	uint32_t x;
	uint32_t y;
	int error;

	/* The wide array has a recognizable value in the columns past the image. */
	wide = (uint32_t *)malloc(5U * 12U * sizeof(uint32_t));
	CHECK(wide != NULL);
	if (wide == NULL)
		return;

	for (index = 0; index < 5U * 12U; index++)
		wide[index] = 0xFEDCBA98U;

	for (y = 0; y < 5; y++) {
		for (x = 0; x < 7; x++) {
			wide[y * 12U + x] = pixel_at(x, y, 4U);
			packed[y * 7U + x] = pixel_at(x, y, 4U);
		}
	}

	memset(&image, 0, sizeof(image));
	image.width = 7;
	image.height = 5;
	image.pixels = packed;
	image.stride = 7;
	error = ico_write(&image, 1, &tight_out, &tight_length);
	CHECK_EQ(error, 0);

	image.pixels = wide;
	image.stride = 12;
	error = ico_write(&image, 1, &wide_out, &wide_length);
	CHECK_EQ(error, 0);

	CHECK(tight_out != NULL);
	CHECK(wide_out != NULL);
	if (tight_out != NULL && wide_out != NULL) {
		CHECK_EQ(tight_length, 22 + 40 + 7 * 5 * 4 + 4 * 5);
		CHECK_EQ(wide_length, tight_length);
		CHECK(memcmp(tight_out, wide_out, tight_length) == 0);
	}

	free(tight_out);
	free(wide_out);

	/* The last row of a wide array ends right after the image's last pixel: nothing past it is read. */
	wide_out = NULL;
	free(wide);
	wide = (uint32_t *)malloc((4U * 12U + 7U) * sizeof(uint32_t));
	CHECK(wide != NULL);
	if (wide == NULL)
		return;

	for (y = 0; y < 5; y++) {
		for (x = 0; x < 7; x++) {
			if (y * 12U + x < 4U * 12U + 7U)
				wide[y * 12U + x] = pixel_at(x, y, 4U);
		}
	}

	image.pixels = wide;
	image.stride = 12;
	error = ico_write(&image, 1, &wide_out, &wide_length);
	CHECK_EQ(error, 0);
	free(wide_out);
	free(wide);

	/* A round trip with a stride larger than the width. */
	check_round_trip(20, 6, 31, 40 + 480 + 24);
}

/*
 * Checks PNG entries: a 256 x 256 one has width and height 0 in the
 * directory and its bytes are stored unchanged.
 */
static void
test_png_entry(void)
{
	unsigned char png[300];
	unsigned char small_png[50];
	struct ico_image image;
	struct ico_image pair[2];
	unsigned char *out;
	size_t length;
	int error;

	make_png(png, sizeof(png), 0x5A);
	memset(&image, 0, sizeof(image));
	image.width = 256;
	image.height = 256;
	image.png = png;
	image.png_length = sizeof(png);

	error = ico_write(&image, 1, &out, &length);
	CHECK_EQ(error, 0);
	CHECK(out != NULL);
	if (out != NULL) {
		CHECK_EQ(length, 6 + 16 + 300);
		CHECK_EQ(out[6], 0);
		CHECK_EQ(out[7], 0);
		CHECK_EQ(out[8], 0);
		CHECK_EQ(out[9], 0);
		CHECK_EQ(get_u16(out + 10), 1);
		CHECK_EQ(get_u16(out + 12), 32);
		CHECK_EQ(get_u32(out + 14), 300);
		CHECK_EQ(get_u32(out + 18), 22);
		CHECK(memcmp(out + 22, png, sizeof(png)) == 0);
		free(out);
	}

	/* A PNG of another size keeps its size in the directory. */
	make_png(small_png, sizeof(small_png), 0x11);
	pair[0] = image;
	memset(&pair[1], 0, sizeof(pair[1]));
	pair[1].width = 48;
	pair[1].height = 40;
	pair[1].png = small_png;
	pair[1].png_length = sizeof(small_png);

	error = ico_write(pair, 2, &out, &length);
	CHECK_EQ(error, 0);
	CHECK(out != NULL);
	if (out != NULL) {
		CHECK_EQ(length, 6 + 32 + 300 + 50);
		CHECK_EQ(out[6], 0);
		CHECK_EQ(out[7], 0);
		CHECK_EQ(get_u32(out + 14), 300);
		CHECK_EQ(get_u32(out + 18), 38);
		CHECK_EQ(out[22], 48);
		CHECK_EQ(out[23], 40);
		CHECK_EQ(get_u32(out + 30), 50);
		CHECK_EQ(get_u32(out + 34), 338);
		CHECK(memcmp(out + 38, png, sizeof(png)) == 0);
		CHECK(memcmp(out + 338, small_png, sizeof(small_png)) == 0);
		free(out);
	}

	/* A PNG that is just its signature is still stored. */
	make_png(small_png, 8, 0);
	image.width = 16;
	image.height = 16;
	image.png = small_png;
	image.png_length = 8;
	error = ico_write(&image, 1, &out, &length);
	CHECK_EQ(error, 0);
	CHECK(out != NULL);
	if (out != NULL) {
		CHECK_EQ(length, 30);
		CHECK_EQ(out[6], 16);
		CHECK_EQ(out[7], 16);
		free(out);
	}
}

/*
 * Checks a directory of the eight sizes of the browser's icon, the largest
 * a PNG: the offsets follow one another with no gap, and each image reads
 * back.
 */
static void
test_eight_sizes(void)
{
	static const uint32_t sizes[8] = { 16, 20, 24, 32, 40, 48, 64, 256 };
	static const unsigned long bitmap_bytes[7] = {
		/* header + colours + mask, from the format: 16, 20, 24, 32, 40, 48, 64. */
		40 + 1024 + 64, 40 + 1600 + 80, 40 + 2304 + 96, 40 + 4096 + 128,
		40 + 6400 + 320, 40 + 9216 + 384, 40 + 16384 + 512
	};
	uint32_t *pixels[7];
	unsigned char png[1000];
	struct ico_image images[8];
	struct read_image found;
	unsigned char *out;
	size_t length;
	unsigned long expected_offset;
	unsigned index;
	uint32_t x;
	uint32_t y;
	int error;
	int read_error;

	make_png(png, sizeof(png), 0x33);
	memset(images, 0, sizeof(images));

	/* The seven bitmap sizes each get a picture of its own. */
	for (index = 0; index < 7; index++) {
		pixels[index] = (uint32_t *)malloc((size_t)sizes[index] * sizes[index] * sizeof(uint32_t));
		CHECK(pixels[index] != NULL);
		if (pixels[index] == NULL)
			return;

		make_pixels(pixels[index], sizes[index], sizes[index], sizes[index], index + 10U);
		images[index].width = sizes[index];
		images[index].height = sizes[index];
		images[index].pixels = pixels[index];
		images[index].stride = sizes[index];
	}

	images[7].width = 256;
	images[7].height = 256;
	images[7].png = png;
	images[7].png_length = sizeof(png);

	error = ico_write(images, 8, &out, &length);
	CHECK_EQ(error, 0);
	CHECK(out != NULL);
	if (out == NULL) {
		for (index = 0; index < 7; index++)
			free(pixels[index]);
		return;
	}

	/* The directory header and total length. */
	CHECK_EQ(get_u16(out), 0);
	CHECK_EQ(get_u16(out + 2), 1);
	CHECK_EQ(get_u16(out + 4), 8);
	expected_offset = 6 + 16 * 8;
	/* The total: the directory, the seven bitmaps and the PNG. */
	CHECK_EQ(length, 6 + 16 * 8 + 42888 + 1000);

	for (index = 0; index < 8; index++) {
		read_error = read_image(out, length, index, &found);
		CHECK_EQ(read_error, 0);
		if (read_error != 0)
			continue;

		/* The entry's offset is the end of the previous image. */
		CHECK_EQ(found.offset, expected_offset);
		if (index < 7) {
			CHECK_EQ(found.length, bitmap_bytes[index]);
			CHECK_EQ(found.entry_width, sizes[index]);
			CHECK_EQ(found.entry_height, sizes[index]);
			CHECK_EQ(found.is_png, 0);
			CHECK_EQ(found.width, sizes[index]);
			CHECK_EQ(found.height, sizes[index]);
			if (found.pixels != NULL) {
				for (y = 0; y < sizes[index]; y++) {
					for (x = 0; x < sizes[index]; x++)
						CHECK(found.pixels[y * sizes[index] + x] == pixel_at(x, y, index + 10U));
				}
			}
		} else {
			CHECK_EQ(found.length, 1000);
			CHECK_EQ(found.entry_width, 0);
			CHECK_EQ(found.entry_height, 0);
			CHECK_EQ(found.is_png, 1);
			CHECK(memcmp(out + found.offset, png, sizeof(png)) == 0);
		}

		expected_offset += found.length;
		free_read(&found);
	}

	/* The last image ends exactly at the end of the file. */
	CHECK_EQ(expected_offset, length);

	free(out);
	for (index = 0; index < 7; index++)
		free(pixels[index]);
}

/*
 * Checks the count limits: 255 images are written (two of the same size
 * included), 0 and 256 are refused.
 */
static void
test_count_limits(void)
{
	struct ico_image *images;
	uint32_t pixel;
	unsigned char *out;
	size_t length;
	unsigned index;
	int error;

	pixel = 0x80102030U;
	images = (struct ico_image *)calloc(256U, sizeof(*images));
	CHECK(images != NULL);
	if (images == NULL)
		return;

	for (index = 0; index < 256; index++) {
		images[index].width = 1;
		images[index].height = 1;
		images[index].pixels = &pixel;
		images[index].stride = 1;
	}

	/* The greatest count; every image is the same size. */
	error = ico_write(images, 255, &out, &length);
	CHECK_EQ(error, 0);
	CHECK(out != NULL);
	if (out != NULL) {
		CHECK_EQ(length, 6 + 255 * 16 + 255 * 48);
		CHECK_EQ(get_u16(out + 4), 255);
		CHECK_EQ(get_u32(out + 6 + 254 * 16 + 12), 6 + 255 * 16 + 254 * 48);
		free(out);
	}

	/* Two images of one size are allowed. */
	error = ico_write(images, 2, &out, &length);
	CHECK_EQ(error, 0);
	CHECK_EQ(length, 6 + 32 + 96);
	free(out);

	/* Counts that the directory refuses. */
	check_refused(images, 0);
	check_refused(images, 256);
	check_refused(images, 1000);

	free(images);
}

/*
 * Checks every refusal: missing arguments, sizes out of range, pixels and
 * PNG both or neither, a short stride, a wrong signature, and a total
 * above 32 bits.  Each leaves *out NULL and *length 0.
 */
static void
test_errors(void)
{
	uint32_t pixels[16 * 16];
	unsigned char png[64];
	struct ico_image image;
	struct ico_image pair[2];
	unsigned char *out;
	size_t length;
	unsigned index;
	int error;

	make_pixels(pixels, 16, 16, 16, 2U);
	make_png(png, sizeof(png), 1);

	/* Missing arguments. */
	memset(&image, 0, sizeof(image));
	image.width = 16;
	image.height = 16;
	image.pixels = pixels;
	image.stride = 16;
	out = (unsigned char *)pixels;
	length = 5;
	error = ico_write(NULL, 1, &out, &length);
	CHECK_EQ(error, EINVAL);
	CHECK(out == NULL);
	CHECK_EQ(length, 0);

	length = 5;
	error = ico_write(&image, 1, NULL, &length);
	CHECK_EQ(error, EINVAL);
	CHECK_EQ(length, 0);

	out = (unsigned char *)pixels;
	error = ico_write(&image, 1, &out, NULL);
	CHECK_EQ(error, EINVAL);
	CHECK(out == NULL);

	/* The sizes: 0 and 257 on either side, for pixels and for a PNG. */
	image.width = 0;
	check_refused(&image, 1);
	image.width = 257;
	check_refused(&image, 1);
	image.width = 16;
	image.height = 0;
	check_refused(&image, 1);
	image.height = 257;
	check_refused(&image, 1);
	image.height = 16;
	image.width = 4294967295U;
	check_refused(&image, 1);
	image.width = 16;

	memset(&pair[0], 0, sizeof(pair[0]));
	pair[0].width = 0;
	pair[0].height = 16;
	pair[0].png = png;
	pair[0].png_length = sizeof(png);
	check_refused(&pair[0], 1);
	pair[0].width = 257;
	check_refused(&pair[0], 1);
	pair[0].width = 16;
	pair[0].height = 257;
	check_refused(&pair[0], 1);

	/* Both pixels and PNG, and neither. */
	pair[0] = image;
	pair[0].png = png;
	pair[0].png_length = sizeof(png);
	check_refused(&pair[0], 1);
	memset(&pair[0], 0, sizeof(pair[0]));
	pair[0].width = 16;
	pair[0].height = 16;
	pair[0].stride = 16;
	check_refused(&pair[0], 1);

	/* A PNG pointer with no length, and a length with no pointer. */
	pair[0].png = png;
	pair[0].png_length = 0;
	check_refused(&pair[0], 1);
	pair[0].png_length = 7;
	check_refused(&pair[0], 1);
	pair[0].png = NULL;
	pair[0].png_length = 64;
	check_refused(&pair[0], 1);

	/* A stride below the width. */
	image.stride = 15;
	check_refused(&image, 1);
	image.stride = 0;
	check_refused(&image, 1);
	image.stride = 16;

	/* A stride too large to count the last row's place in memory. */
	image.stride = SIZE_MAX;
	check_refused(&image, 1);
	image.stride = SIZE_MAX / 15U;
	check_refused(&image, 1);
	image.stride = 16;

	/* Each byte of the PNG signature changed in turn, and the second image bad after a good first. */
	for (index = 0; index < 8; index++) {
		png[index] = (unsigned char)(png[index] ^ 0x01U);
		memset(&pair[0], 0, sizeof(pair[0]));
		pair[0].width = 16;
		pair[0].height = 16;
		pair[0].png = png;
		pair[0].png_length = sizeof(png);
		check_refused(&pair[0], 1);

		pair[1] = pair[0];
		pair[0] = image;
		check_refused(pair, 2);
		png[index] = (unsigned char)(png[index] ^ 0x01U);
	}

	/* A good PNG is accepted again with the signature restored. */
	memset(&pair[0], 0, sizeof(pair[0]));
	pair[0].width = 16;
	pair[0].height = 16;
	pair[0].png = png;
	pair[0].png_length = sizeof(png);
	error = ico_write(&pair[0], 1, &out, &length);
	CHECK_EQ(error, 0);
	free(out);
}

/*
 * Checks the refusal of a file longer than 32 bits can describe.  The PNG
 * lengths here are stated, not backed by memory: the writer must refuse
 * before it reads past the signature, so only 8 bytes are real.
 */
static void
test_large_sizes(void)
{
	unsigned char png[8];
	struct ico_image pair[2];
	uint64_t big;

	/* Only a platform whose size_t holds more than 32 bits can state these lengths. */
	if (sizeof(size_t) <= 4U)
		return;

	make_png(png, sizeof(png), 0);
	memset(pair, 0, sizeof(pair));
	pair[0].width = 256;
	pair[0].height = 256;
	pair[0].png = png;
	pair[1] = pair[0];

	/* One PNG of 2^32 bytes cannot be given an offset and a length in the directory. */
	big = UINT64_C(1) << 32;
	pair[0].png_length = (size_t)big;
	check_refused(pair, 1);

	/* Two PNGs that each fit in 32 bits but together, with the directory, do not. */
	big = UINT64_C(0xFFFFFFF0);
	pair[0].png_length = (size_t)big;
	pair[1].png_length = (size_t)big;
	check_refused(pair, 2);

	/* A single PNG whose length plus the directory is just above 32 bits. */
	big = UINT64_C(0xFFFFFFFF) - 22U + 1U;
	pair[0].png_length = (size_t)big;
	check_refused(pair, 1);
}

/*
 * Checks that a transparent pixel keeps its colour: no premultiplication
 * is applied and the mask still marks it.
 */
static void
test_alpha_zero(void)
{
	static const uint32_t pixels[4] = {
		0x00FF8040U, 0x01102030U, 0xFF0A0B0CU, 0x00000000U
	};
	struct ico_image image;
	struct read_image found;
	unsigned char *out;
	size_t length;
	int error;
	int read_error;

	memset(&image, 0, sizeof(image));
	image.width = 2;
	image.height = 2;
	image.pixels = pixels;
	image.stride = 2;

	error = ico_write(&image, 1, &out, &length);
	CHECK_EQ(error, 0);
	CHECK(out != NULL);
	if (out == NULL)
		return;

	/* The bottom row comes first: pixel 2 as blue, green, red, alpha, then pixel 3, all zero. */
	CHECK_EQ(length, 22 + 40 + 16 + 8);
	CHECK_EQ(out[62], 0x0C);
	CHECK_EQ(out[63], 0x0B);
	CHECK_EQ(out[64], 0x0A);
	CHECK_EQ(out[65], 0xFF);
	CHECK_EQ(out[66], 0);
	CHECK_EQ(out[69], 0);

	/* The top row: pixel 0 is transparent but keeps its colour, pixel 1 is nearly so. */
	CHECK_EQ(out[70], 0x40);
	CHECK_EQ(out[71], 0x80);
	CHECK_EQ(out[72], 0xFF);
	CHECK_EQ(out[73], 0x00);
	CHECK_EQ(out[74], 0x30);
	CHECK_EQ(out[75], 0x20);
	CHECK_EQ(out[76], 0x10);
	CHECK_EQ(out[77], 0x01);

	/* The mask, bottom row first: only pixel 3 is transparent (bit 1 from the left), then only pixel 0. */
	CHECK_EQ(out[78], 0x40);
	CHECK_EQ(out[79], 0);
	CHECK_EQ(out[80], 0);
	CHECK_EQ(out[81], 0);
	CHECK_EQ(out[82], 0x80);
	CHECK_EQ(out[83], 0);
	CHECK_EQ(out[84], 0);
	CHECK_EQ(out[85], 0);

	read_error = read_image(out, length, 0, &found);
	CHECK_EQ(read_error, 0);
	if (read_error == 0) {
		CHECK(found.pixels != NULL);
		if (found.pixels != NULL) {
			CHECK(found.pixels[0] == 0x00FF8040U);
			CHECK(found.pixels[1] == 0x01102030U);
			CHECK(found.pixels[2] == 0xFF0A0B0CU);
			CHECK(found.pixels[3] == 0x00000000U);
		}
		free_read(&found);
	}

	free(out);
}

/*
 * Fills width x height pixels, rows stride apart, from a pattern of the
 * position and the seed.  Columns past the width keep a marker value.
 */
static void
make_pixels(
	uint32_t *pixels,
	uint32_t width,
	uint32_t height,
	size_t stride,
	uint32_t seed)
{
	size_t x;
	size_t y;

	/* Marks the columns beyond the picture so reading them would show. */
	for (y = 0; y < height; y++) {
		for (x = 0; x < stride; x++)
			pixels[y * stride + x] = 0xDEADBEEFU;
	}

	/* Fills the picture itself. */
	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++)
			pixels[y * stride + x] = pixel_at((uint32_t)x, (uint32_t)y, seed);
	}
}

/*
 * Gives the pixel at a position: a different colour at each position, and
 * alpha 0 at about a third of them, so the mask has both bits.
 */
static uint32_t
pixel_at(
	uint32_t x,
	uint32_t y,
	uint32_t seed)
{
	uint32_t alpha;
	uint32_t red;
	uint32_t green;
	uint32_t blue;

	/* Every third position along a diagonal is transparent. */
	alpha = 200U + ((x + seed) & 0x3FU);
	if ((x + y + seed) % 3U == 0U)
		alpha = 0;

	red = (x * 16U + seed) & 0xFFU;
	green = (y * 13U + 7U) & 0xFFU;
	blue = (x + 3U * y + seed * 5U) & 0xFFU;

	/* Reports the pixel as alpha, red, green, blue from the top byte. */
	return (alpha << 24) | (red << 16) | (green << 8) | blue;
}

/*
 * Makes the bytes of a stand-in PNG: the 8-byte signature and then bytes
 * that the writer must not look at.
 */
static void
make_png(
	unsigned char *bytes,
	size_t length,
	unsigned char fill)
{
	static const unsigned char signature[8] = {
		0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A
	};
	size_t index;

	/* The body is the fill byte, changed along the way so a shifted copy would differ. */
	for (index = 0; index < length; index++)
		bytes[index] = (unsigned char)(fill + index);

	/* The signature. */
	for (index = 0; index < 8 && index < length; index++)
		bytes[index] = signature[index];
}

/*
 * Reads a 16-bit little-endian value.
 */
static unsigned
get_u16(
	const unsigned char *bytes)
{
	/* The first byte is the low one. */
	return (unsigned)bytes[0] | ((unsigned)bytes[1] << 8);
}

/*
 * Reads a 32-bit little-endian value.
 */
static unsigned long
get_u32(
	const unsigned char *bytes)
{
	/* The first byte is the low one. */
	return (unsigned long)bytes[0] | ((unsigned long)bytes[1] << 8) |
	    ((unsigned long)bytes[2] << 16) | ((unsigned long)bytes[3] << 24);
}

/*
 * Reads one image of an icon file: the directory entry, and for a bitmap the
 * header, the pixels (alpha taken from the colour bytes) and the mask.
 * Checks the fixed fields on the way, and that the mask is 1 exactly
 * where the alpha is 0.  Returns 0 or EINVAL when the file is malformed.
 */
static int
read_image(
	const unsigned char *file,
	size_t length,
	unsigned index,
	struct read_image *found)
{
	const unsigned char *entry;
	const unsigned char *image;
	const unsigned char *colours;
	const unsigned char *mask;
	unsigned long mask_row;
	unsigned long header_height;
	uint32_t row;
	uint32_t column;
	uint32_t from_top;
	unsigned bit;
	size_t at;

	memset(found, 0, sizeof(*found));

	/* The directory header. */
	if (length < 6U + 16U * (size_t)(index + 1U))
		return EINVAL;
	if (get_u16(file) != 0U || get_u16(file + 2) != 1U)
		return EINVAL;
	if (index >= get_u16(file + 4))
		return EINVAL;

	/* The entry. */
	entry = file + 6 + 16 * (size_t)index;
	found->entry_width = entry[0];
	found->entry_height = entry[1];
	CHECK_EQ(entry[2], 0);
	CHECK_EQ(entry[3], 0);
	CHECK_EQ(get_u16(entry + 4), 1);
	CHECK_EQ(get_u16(entry + 6), 32);
	found->length = get_u32(entry + 8);
	found->offset = get_u32(entry + 12);
	if (found->offset > length || found->length > length - found->offset)
		return EINVAL;

	image = file + found->offset;

	/* A PNG begins with the signature. */
	if (found->length >= 8U && image[0] == 0x89 && image[1] == 0x50 && image[2] == 0x4E && image[3] == 0x47) {
		found->is_png = 1;
		return 0;
	}

	/* A bitmap's header: size 40, width, doubled height, planes 1, 32 bits, no compression. */
	if (found->length < 40U)
		return EINVAL;
	if (get_u32(image) != 40U)
		return EINVAL;
	found->width = (uint32_t)get_u32(image + 4);
	header_height = get_u32(image + 8);
	if (header_height % 2U != 0U)
		return EINVAL;
	found->height = (uint32_t)(header_height / 2U);
	CHECK_EQ(get_u16(image + 12), 1);
	CHECK_EQ(get_u16(image + 14), 32);
	CHECK_EQ(get_u32(image + 16), 0);

	/* The directory's size byte is the side, or 0 for 256. */
	if (found->width == 256U)
		CHECK_EQ(found->entry_width, 0);
	else
		CHECK_EQ(found->entry_width, found->width);
	if (found->height == 256U)
		CHECK_EQ(found->entry_height, 0);
	else
		CHECK_EQ(found->entry_height, found->height);

	/* The sizes the header gives must account for the image's bytes. */
	mask_row = ((found->width + 31UL) / 32UL) * 4UL;
	CHECK_EQ(get_u32(image + 20), found->width * found->height * 4UL + mask_row * found->height);
	CHECK_EQ(found->length, 40UL + found->width * found->height * 4UL + mask_row * found->height);
	CHECK_EQ(get_u32(image + 24), 0);
	CHECK_EQ(get_u32(image + 28), 0);
	CHECK_EQ(get_u32(image + 32), 0);
	CHECK_EQ(get_u32(image + 36), 0);
	if (found->length != 40UL + found->width * found->height * 4UL + mask_row * found->height)
		return EINVAL;

	found->pixels = (uint32_t *)malloc((size_t)found->width * found->height * sizeof(uint32_t));
	if (found->pixels == NULL)
		return EINVAL;

	/* The pixels, the bottom row first. */
	colours = image + 40;
	mask = colours + (size_t)found->width * found->height * 4U;
	for (row = 0; row < found->height; row++) {
		from_top = found->height - 1U - row;
		for (column = 0; column < found->width; column++) {
			at = ((size_t)row * found->width + column) * 4U;
			found->pixels[(size_t)from_top * found->width + column] =
			    ((uint32_t)colours[at + 3] << 24) |
			    ((uint32_t)colours[at + 2] << 16) |
			    ((uint32_t)colours[at + 1] << 8) |
			    (uint32_t)colours[at];

			/* The mask bit of the pixel: the leftmost pixel is the top bit. */
			bit = (mask[(size_t)row * mask_row + column / 8U] >> (7U - column % 8U)) & 1U;
			if (colours[at + 3] == 0U)
				CHECK_EQ(bit, 1);
			else
				CHECK_EQ(bit, 0);
		}

		/* The padding bits and bytes of the mask row are zero. */
		for (column = found->width; column < mask_row * 8U; column++) {
			bit = (mask[(size_t)row * mask_row + column / 8U] >> (7U - column % 8U)) & 1U;
			CHECK_EQ(bit, 0);
		}
	}

	return 0;
}

/*
 * Frees what read_image allocated.
 */
static void
free_read(
	struct read_image *found)
{
	/* The pixels are the only allocation. */
	free(found->pixels);
	found->pixels = NULL;
}

/*
 * Writes one picture of the given size and stride, checks its length against
 * the number of bytes the format gives, and reads it back.
 */
static void
check_round_trip(
	uint32_t width,
	uint32_t height,
	size_t stride,
	unsigned long bitmap_bytes)
{
	uint32_t *pixels;
	struct ico_image image;
	struct read_image found;
	unsigned char *out;
	size_t length;
	uint32_t x;
	uint32_t y;
	int error;
	int read_error;
	unsigned long size_field;

	pixels = (uint32_t *)malloc(stride * height * sizeof(uint32_t));
	CHECK(pixels != NULL);
	if (pixels == NULL)
		return;

	make_pixels(pixels, width, height, stride, width + height);
	memset(&image, 0, sizeof(image));
	image.width = width;
	image.height = height;
	image.pixels = pixels;
	image.stride = stride;

	error = ico_write(&image, 1, &out, &length);
	CHECK_EQ(error, 0);
	CHECK(out != NULL);
	if (out == NULL) {
		free(pixels);
		return;
	}

	CHECK_EQ(length, 22 + bitmap_bytes);
	read_error = read_image(out, length, 0, &found);
	CHECK_EQ(read_error, 0);
	if (read_error == 0) {
		CHECK_EQ(found.offset, 22);
		CHECK_EQ(found.length, bitmap_bytes);
		CHECK_EQ(found.width, width);
		CHECK_EQ(found.height, height);

		/* The header's image size excludes the 40 bytes of the header itself. */
		size_field = get_u32(out + 22 + 20);
		CHECK_EQ(size_field, bitmap_bytes - 40);
		if (found.pixels != NULL) {
			for (y = 0; y < height; y++) {
				for (x = 0; x < width; x++)
					CHECK(found.pixels[(size_t)y * width + x] == pixel_at(x, y, width + height));
			}
		}

		free_read(&found);
	}

	free(out);
	free(pixels);
}

/*
 * Checks that a call is refused with EINVAL and leaves nothing in the results.
 */
static void
check_refused(
	const struct ico_image *images,
	unsigned count)
{
	unsigned char *out;
	size_t length;
	int error;

	/* The results start as something else so that a refusal must clear them. */
	out = (unsigned char *)&out;
	length = 77;
	error = ico_write(images, count, &out, &length);
	CHECK_EQ(error, EINVAL);
	CHECK(out == NULL);
	CHECK_EQ(length, 0);
	if (out != NULL && out != (unsigned char *)&out)
		free(out);
}
