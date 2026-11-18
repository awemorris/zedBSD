/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The decoding of a wallpaper (see wallpaper.h).
 *
 * The format is told by the file's first bytes, never by its name.  A PNG
 * is decoded whole into RGB with libpng-compat's simplified reading, its
 * transparency composited over black (ws138 decision U8).  A JPEG is read
 * a row at a time with libjpeg-compat, grey turned into RGB; CMYK, rare
 * for a picture of a desktop, is refused, and the EXIF orientation is not
 * applied.  Every picture must lie within KL_WALLPAPER_SIDE_MAX on a side
 * and KL_WALLPAPER_PIXELS_MAX in all, since the decoding keeps the whole
 * picture (and libpng-compat a second copy of the same size meanwhile).
 */

#include "wallpaper.h"

#include <compat/jpeglib.h>
#include <compat/png/png.h>
#include <errno.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Marks a parameter a callback has but does not use. */
#define UNUSED_PARAMETER(name) ((void)(name))

/* The first bytes of a PNG and of a JPEG (its SOI and the next marker's FF). */
#define WALLPAPER_PNG_MAGIC_SIZE	8U
#define WALLPAPER_JPEG_MAGIC_SIZE	3U

/*
 * The error manager of one JPEG decoding: libjpeg's own, and where its
 * fatal errors jump back to.  It lives on the decoder's stack.
 */
struct wallpaper_jpeg_error {
	struct jpeg_error_mgr manager;
	jmp_buf back;
};

/* The PNG signature. */
static const unsigned char wallpaper_png_magic[WALLPAPER_PNG_MAGIC_SIZE] = {
	0x89U, 0x50U, 0x4eU, 0x47U, 0x0dU, 0x0aU, 0x1aU, 0x0aU
};

/* A JPEG's start of image and the next marker's first byte. */
static const unsigned char wallpaper_jpeg_magic[WALLPAPER_JPEG_MAGIC_SIZE] = {
	0xffU, 0xd8U, 0xffU
};

static int wallpaper_png(const unsigned char *data, size_t size, struct kl_wallpaper_image *image);
static int wallpaper_jpeg(const unsigned char *data, size_t size, struct kl_wallpaper_image *image);
static int wallpaper_size_allowed(unsigned long width, unsigned long height);
static void wallpaper_jpeg_exit(j_common_ptr info);
static void wallpaper_jpeg_quiet(j_common_ptr info, int level);

/*
 * Decodes a wallpaper file's bytes into RGB pixels.
 *
 * It reports 0 with the image filled (the caller frees image->rgb),
 * EINVAL for bytes that are no picture this decodes or a damaged one,
 * EFBIG for a picture beyond the sizes, and ENOMEM.  The bytes stay the
 * caller's.
 */
int
kl_wallpaper_decode(
	const unsigned char *data,
	size_t size,
	struct kl_wallpaper_image *image)
{
	int compared;
	int error;

	/* Nothing decoded yet. */
	memset(image, 0, sizeof(*image));

	/* A PNG. */
	compared = 1;
	if (size >= WALLPAPER_PNG_MAGIC_SIZE)
		compared = memcmp(data, wallpaper_png_magic, WALLPAPER_PNG_MAGIC_SIZE);
	if (compared == 0) {
		/* Decodes the PNG. */
		error = wallpaper_png(data, size, image);
		if (error != 0)
			return error;

		/* Succeeded: the PNG is decoded. */
		return 0;
	}

	/* A JPEG. */
	compared = 1;
	if (size >= WALLPAPER_JPEG_MAGIC_SIZE)
		compared = memcmp(data, wallpaper_jpeg_magic, WALLPAPER_JPEG_MAGIC_SIZE);
	if (compared != 0)
		return EINVAL;

	/* Decodes the JPEG. */
	error = wallpaper_jpeg(data, size, image);
	if (error != 0)
		return error;

	/* Succeeded: the JPEG is decoded. */
	return 0;
}

/* Decodes a PNG whole into RGB, its transparency over black. */
static int
wallpaper_png(
	const unsigned char *data,
	size_t size,
	struct kl_wallpaper_image *image)
{
	png_image png;
	png_color black;
	unsigned char *rgb;
	int allowed;
	int ok;

	/* Reads the header. */
	memset(&png, 0, sizeof(png));
	png.version = PNG_IMAGE_VERSION;
	ok = png_image_begin_read_from_memory(&png, data, size);
	if (!ok)
		return EINVAL;

	/* Refuses a picture beyond the sizes. */
	allowed = wallpaper_size_allowed(png.width, png.height);
	if (!allowed) {
		png_image_free(&png);
		return EFBIG;
	}

	/* Room for the RGB pixels. */
	png.format = PNG_FORMAT_RGB;
	rgb = malloc(PNG_IMAGE_SIZE(png));
	if (rgb == NULL) {
		png_image_free(&png);
		return ENOMEM;
	}

	/* Decodes it over black; the finish frees the reader whether or not it succeeds. */
	memset(&black, 0, sizeof(black));
	ok = png_image_finish_read(&png, &black, rgb, 0, NULL);
	if (!ok) {
		free(rgb);
		return EINVAL;
	}

	/* Succeeded: the image holds the pixels. */
	image->rgb = rgb;
	image->width = png.width;
	image->height = png.height;
	return 0;
}

/* Decodes a JPEG into RGB a row at a time. */
static int
wallpaper_jpeg(
	const unsigned char *data,
	size_t size,
	struct kl_wallpaper_image *image)
{
	struct jpeg_decompress_struct info;
	struct wallpaper_jpeg_error failure;
	unsigned char *volatile rgb;
	JSAMPROW rows[1];
	size_t stride;
	int allowed;

	/* A decompressor whose errors come back here. */
	memset(&info, 0, sizeof(info));
	info.err = jpeg_std_error(&failure.manager);
	failure.manager.error_exit = wallpaper_jpeg_exit;
	failure.manager.emit_message = wallpaper_jpeg_quiet;
	rgb = NULL;

	/*
	 * An error in libjpeg comes back here (setjmp must stand in the
	 * condition itself); rgb is volatile so that it survives the jump.
	 */
	if (setjmp(failure.back) != 0) {
		jpeg_destroy_decompress(&info);
		free(rgb);
		return EINVAL;
	}

	/* The decompressor, reading the memory, and the header. */
	jpeg_create_decompress(&info);
	jpeg_mem_src(&info, data, (unsigned long)size);
	(void)jpeg_read_header(&info, TRUE);

	/* Refuses CMYK, which RGB cannot be asked for. */
	if (info.jpeg_color_space == JCS_CMYK || info.jpeg_color_space == JCS_YCCK) {
		jpeg_destroy_decompress(&info);
		return EINVAL;
	}

	/* RGB out, grey turned into it, and the decoding starts. */
	info.out_color_space = JCS_RGB;
	(void)jpeg_start_decompress(&info);

	/* Refuses a picture beyond the sizes, or one that is not three samples a pixel. */
	allowed = wallpaper_size_allowed(info.output_width, info.output_height);
	if (!allowed) {
		jpeg_destroy_decompress(&info);
		return EFBIG;
	}

	/* RGB gives three samples a pixel; anything else is not read. */
	if (info.output_components != 3) {
		jpeg_destroy_decompress(&info);
		return EINVAL;
	}

	/* Room for the RGB pixels. */
	stride = (size_t)info.output_width * 3U;
	rgb = malloc(stride * (size_t)info.output_height);
	if (rgb == NULL) {
		jpeg_destroy_decompress(&info);
		return ENOMEM;
	}

	/* Each row straight into its place. */
	while (info.output_scanline < info.output_height) {
		rows[0] = rgb + (size_t)info.output_scanline * stride;
		(void)jpeg_read_scanlines(&info, rows, 1U);
	}

	/* The decoding ends. */
	(void)jpeg_finish_decompress(&info);
	jpeg_destroy_decompress(&info);

	/* Succeeded: the image holds the pixels. */
	image->rgb = rgb;
	image->width = info.output_width;
	image->height = info.output_height;
	return 0;
}

/* Tells whether a picture's size lies within the side and the pixels allowed. */
static int
wallpaper_size_allowed(
	unsigned long width,
	unsigned long height)
{
	/* An empty picture is none. */
	if (width == 0UL || height == 0UL)
		return 0;

	/* A side too long. */
	if (width > KL_WALLPAPER_SIDE_MAX || height > KL_WALLPAPER_SIDE_MAX)
		return 0;

	/* Too many pixels (both sides are at most KL_WALLPAPER_SIDE_MAX here, so the product fits an unsigned long). */
	if (width * height > KL_WALLPAPER_PIXELS_MAX)
		return 0;

	/* The size is allowed. */
	return 1;
}

/* Jumps back to the decoder on a fatal libjpeg error. */
static void
wallpaper_jpeg_exit(
	j_common_ptr info)
{
	struct wallpaper_jpeg_error *failure;

	/* The manager is the first member of the error record. */
	failure = (struct wallpaper_jpeg_error *)info->err;

	/* Returns to the setjmp in wallpaper_jpeg, which frees what was made and reports EINVAL. */
	longjmp(failure->back, 1);
}

/* Keeps libjpeg's warnings and traces off the terminal. */
static void
wallpaper_jpeg_quiet(
	j_common_ptr info,
	int level)
{
	UNUSED_PARAMETER(info);
	UNUSED_PARAMETER(level);
}
