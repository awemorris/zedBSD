/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * JPEG through libjpeg-compat: the whole file from memory, rows of RGB,
 * gray or CMYK packed into the bitmap.  CMYK is made RGB as browsers make
 * it: an Adobe file stores its channels inverted, so red is C * K / 255 of
 * the stored values; a file without the Adobe marker stores them plain.
 * A file cut short gives what libjpeg gives (gray past the data).
 */

#include "image/image.h"

#include <compat/jpeglib.h>

#include <errno.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

/*
 * The error manager: libjpeg's, whose exit jumps back into the decoding,
 * and which prints nothing.
 */
struct jpeg_image_error {
	struct jpeg_error_mgr pub;
	jmp_buf back;
};

static void jpeg_image_exit(j_common_ptr cinfo);
static void jpeg_image_message(j_common_ptr cinfo);
static void jpeg_image_row(const struct jpeg_decompress_struct *cinfo, const JSAMPLE *row, uint32_t *out);
static uint32_t jpeg_image_pixel(unsigned red, unsigned green, unsigned blue);

/*
 * Decodes a JPEG file into a bitmap; EINVAL when libjpeg-compat refuses
 * it.
 */
int
img_decode_jpeg(
	const unsigned char *bytes,
	size_t length,
	struct img_bitmap *bitmap)
{
	struct jpeg_decompress_struct cinfo;
	struct jpeg_image_error error;
	JSAMPLE *volatile row;
	JSAMPROW rows[1];
	size_t stride;
	int jumped;
	int status;

	/* The error manager, whose exit comes back here with everything freed. */
	row = NULL;
	memset(bitmap, 0, sizeof(*bitmap));
	cinfo.err = jpeg_std_error(&error.pub);
	error.pub.error_exit = jpeg_image_exit;
	error.pub.output_message = jpeg_image_message;
	jumped = setjmp(error.back);
	if (jumped != 0) {
		jpeg_destroy_decompress(&cinfo);
		free(row);
		img_bitmap_release(bitmap);
		return EINVAL;
	}

	/* The object, the bytes and the header. */
	jpeg_create_decompress(&cinfo);
	jpeg_mem_src(&cinfo, bytes, (unsigned long)length);
	jpeg_read_header(&cinfo, TRUE);

	/* RGB from colour files, gray from gray ones, CMYK as it is (made RGB here). */
	if (cinfo.jpeg_color_space == JCS_CMYK || cinfo.jpeg_color_space == JCS_YCCK) {
		cinfo.out_color_space = JCS_CMYK;
	} else if (cinfo.num_components == 1) {
		cinfo.out_color_space = JCS_GRAYSCALE;
	} else {
		cinfo.out_color_space = JCS_RGB;
	}

	/* The decoding, and a bitmap of the output's size. */
	jpeg_start_decompress(&cinfo);
	status = img_bitmap_create(bitmap, (int)cinfo.output_width, (int)cinfo.output_height);
	if (status != 0) {
		jpeg_destroy_decompress(&cinfo);
		return status;
	}

	/* One row of samples at a time. */
	stride = (size_t)cinfo.output_width * (size_t)cinfo.output_components;
	row = malloc(stride);
	if (row == NULL) {
		jpeg_destroy_decompress(&cinfo);
		img_bitmap_release(bitmap);
		return ENOMEM;
	}

	/* Each row, packed into the bitmap. */
	while (cinfo.output_scanline < cinfo.output_height) {
		rows[0] = row;
		jpeg_read_scanlines(&cinfo, rows, 1);
		jpeg_image_row(&cinfo, row, bitmap->pixels + (size_t)(cinfo.output_scanline - 1U) * (size_t)bitmap->width);
	}

	/* The end of the image. */
	jpeg_finish_decompress(&cinfo);
	jpeg_destroy_decompress(&cinfo);
	free(row);

	/* Succeeded: the bitmap holds the image. */
	return 0;
}

/* libjpeg's error exit: back into the decoding. */
static void
jpeg_image_exit(
	j_common_ptr cinfo)
{
	struct jpeg_image_error *error;

	/* The jump; the message is not printed. */
	error = (struct jpeg_image_error *)cinfo->err;
	longjmp(error->back, 1);
}

/* libjpeg's warnings are not printed. */
static void
jpeg_image_message(
	j_common_ptr cinfo)
{
	/* A damaged file still shows what could be decoded. */
	(void)cinfo;
}

/* Packs one row of the output's samples into opaque pixels. */
static void
jpeg_image_row(
	const struct jpeg_decompress_struct *cinfo,
	const JSAMPLE *row,
	uint32_t *out)
{
	const JSAMPLE *sample;
	unsigned cyan;
	unsigned magenta;
	unsigned yellow;
	unsigned black;
	JDIMENSION x;

	/* Each pixel by the output's colour space. */
	for (x = 0; x < cinfo->output_width; x++) {
		sample = row + (size_t)x * (size_t)cinfo->output_components;
		if (cinfo->out_color_space == JCS_GRAYSCALE) {
			/* Gray. */
			out[x] = jpeg_image_pixel(sample[0], sample[0], sample[0]);
		} else if (cinfo->out_color_space == JCS_RGB) {
			/* RGB. */
			out[x] = jpeg_image_pixel(sample[0], sample[1], sample[2]);
		} else if (cinfo->saw_Adobe_marker) {
			/* Adobe's inverted CMYK. */
			cyan = sample[0];
			magenta = sample[1];
			yellow = sample[2];
			black = sample[3];
			out[x] = jpeg_image_pixel(cyan * black / 255U, magenta * black / 255U, yellow * black / 255U);
		} else {
			/* Plain CMYK. */
			cyan = 255U - sample[0];
			magenta = 255U - sample[1];
			yellow = 255U - sample[2];
			black = 255U - sample[3];
			out[x] = jpeg_image_pixel(cyan * black / 255U, magenta * black / 255U, yellow * black / 255U);
		}
	}
}

/* Makes an opaque pixel. */
static uint32_t
jpeg_image_pixel(
	unsigned red,
	unsigned green,
	unsigned blue)
{
	uint32_t pixel;

	/* Alpha, red, green, blue from the top byte down. */
	pixel = 0xff000000U | ((uint32_t)red << 16) | ((uint32_t)green << 8) | (uint32_t)blue;
	return pixel;
}
