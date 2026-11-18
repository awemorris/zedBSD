/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The decoding of JPEG and GIF pictures shared by Image Viewer and the
 * file manager (ws094-p013; picture.h).  The code is Image Viewer's
 * (ws091), moved here so that both programs read a picture the same way.
 */

#include "picture.h"

#include <compat/jpeglib.h>
#include <errno.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

/* The EXIF tag of the orientation, and the TIFF type of its value (SHORT). */
#define PICTURE_EXIF_ORIENTATION	0x0112U
#define PICTURE_TIFF_SHORT		3U

/* Marks a parameter a callback has but does not use. */
#define UNUSED_PARAMETER(name) ((void)(name))

/* The marker that carries EXIF (APP1). */
#define PICTURE_JPEG_APP1		(JPEG_APP0 + 1)

/*
 * A JPEG being read: libjpeg's error manager, which jumps back here
 * instead of ending the program, and where it jumps to.
 */
struct picture_jpeg_error {
	struct jpeg_error_mgr manager;
	jmp_buf back;
};

static void picture_jpeg_exit(j_common_ptr info);
static void picture_jpeg_quiet(j_common_ptr info, int level);
static int picture_jpeg_orientation(j_decompress_ptr info);
static unsigned picture_jpeg_denominator(unsigned long width, unsigned long height, unsigned box_side, int cover);
static int picture_too_large(unsigned long width, unsigned long height, unsigned max_side, unsigned long max_pixels);
static unsigned picture_read16(const unsigned char *data, int big_endian);
static unsigned long picture_read32(const unsigned char *data, int big_endian);

/*
 * Makes a premultiplied 0xAARRGGBB word of straight components.
 */
uint32_t
kl_picture_premultiply(
	unsigned red,
	unsigned green,
	unsigned blue,
	unsigned alpha)
{
	uint32_t word;

	/* Each colour scaled by the alpha, rounded. */
	red = (red * alpha + 127U) / 255U;
	green = (green * alpha + 127U) / 255U;
	blue = (blue * alpha + 127U) / 255U;
	word = (uint32_t)alpha << 24;
	word |= (uint32_t)red << 16;
	word |= (uint32_t)green << 8;
	word |= (uint32_t)blue;

	/* Succeeded: the caller has the premultiplied pixel. */
	return word;
}

/*
 * Reads the orientation (1 to 8) from a JPEG's APP1 block (starting at
 * its "Exif" header); 1 (as stored) when it has none or cannot be read.
 */
int
kl_picture_exif_orientation(
	const unsigned char *data,
	size_t size)
{
	const unsigned char *tiff;
	unsigned long directory;
	unsigned long offset;
	size_t tiff_size;
	unsigned count;
	unsigned index;
	unsigned tag;
	unsigned type;
	unsigned field_value;
	int big_endian;
	int match;

	/* A block too short for the "Exif" header and a TIFF header has no orientation. */
	if (size < 14U)
		return 1;

	/* The block starts "Exif" and two zeros, then the TIFF header. */
	match = memcmp(data, "Exif\0\0", 6U);
	if (match != 0)
		return 1;

	/* The TIFF header and the rest of the block, which the offsets count from. */
	tiff = data + 6;
	tiff_size = size - 6U;

	/* The byte order: II (little-endian) or MM (big-endian), then 42. */
	if (tiff[0] == 'I' && tiff[1] == 'I') {
		big_endian = 0;
	} else if (tiff[0] == 'M' && tiff[1] == 'M') {
		big_endian = 1;
	} else {
		return 1;
	}

	/* The magic 42 confirms the header. */
	field_value = picture_read16(tiff + 2, big_endian);
	if (field_value != 42U)
		return 1;

	/* The first directory, which must hold its count. */
	directory = picture_read32(tiff + 4, big_endian);
	if (directory > tiff_size - 2U)
		return 1;

	/* The number of entries in the directory. */
	count = picture_read16(tiff + directory, big_endian);

	/* Looks through the directory's entries (twelve bytes each) for the orientation. */
	for (index = 0; index < count; index++) {
		/* An entry past the end of the block ends the search. */
		offset = directory + 2U + (unsigned long)index * 12U;
		if (offset > tiff_size - 12U)
			return 1;

		/* Another tag is passed over. */
		tag = picture_read16(tiff + offset, big_endian);
		if (tag != PICTURE_EXIF_ORIENTATION)
			continue;

		/* A SHORT occupies the start of the EXIF entry's value field. */
		type = picture_read16(tiff + offset + 2U, big_endian);
		if (type != PICTURE_TIFF_SHORT)
			return 1;

		/* The stored orientation must name one of the eight transforms. */
		field_value = picture_read16(tiff + offset + 8U, big_endian);
		if (field_value < 1U || field_value > 8U)
			return 1;

		/* Reports the orientation the file gives. */
		return (int)field_value;
	}

	/* Succeeded: an absent orientation leaves the picture as stored. */
	return 1;
}

/*
 * Turns a picture as an EXIF orientation says (2 to 8; 1 leaves it):
 * mirrored, turned, or both, so that it shows upright.  Returns 0 or
 * ENOMEM (the picture is then as it was).
 */
int
kl_picture_orient(
	struct kl_picture *picture,
	int orientation)
{
	uint32_t *turned;
	int width;
	int height;
	int source_x;
	int source_y;
	int x;
	int y;

	/* Upright already, or an orientation that is not one. */
	if (orientation <= 1 || orientation > 8)
		return 0;

	/* The turned picture's size: 5 to 8 swap the sides. */
	width = picture->width;
	height = picture->height;
	if (orientation >= 5) {
		width = picture->height;
		height = picture->width;
	}

	/* Room for the turned picture. */
	turned = malloc((size_t)width * (size_t)height * sizeof(uint32_t));
	if (turned == NULL)
		return ENOMEM;

	/* Each pixel of the turned picture, from where the orientation says it was. */
	for (y = 0; y < height; y++) {
		/* Maps this turned row back to the original picture. */
		for (x = 0; x < width; x++) {
			/* The source pixel of this orientation. */
			switch (orientation) {
			case 2:
				source_x = picture->width - 1 - x;
				source_y = y;
				break;
			case 3:
				source_x = picture->width - 1 - x;
				source_y = picture->height - 1 - y;
				break;
			case 4:
				source_x = x;
				source_y = picture->height - 1 - y;
				break;
			case 5:
				source_x = y;
				source_y = x;
				break;
			case 6:
				source_x = y;
				source_y = picture->height - 1 - x;
				break;
			case 7:
				source_x = picture->width - 1 - y;
				source_y = picture->height - 1 - x;
				break;
			default:
				source_x = picture->width - 1 - y;
				source_y = x;
				break;
			}

			/* The pixel into its turned place. */
			turned[(size_t)y * (size_t)width + (size_t)x] = picture->pixels[(size_t)source_y * (size_t)picture->width + (size_t)source_x];
		}
	}

	/* The turned picture replaces the stored one. */
	free(picture->pixels);
	picture->pixels = turned;
	picture->width = width;
	picture->height = height;

	/* Succeeded: the picture is upright. */
	return 0;
}

/*
 * Decodes a JPEG, from a file (file) or from memory (data and size when
 * file is NULL), into an opaque picture as stored, and gives its EXIF
 * orientation (1 to 8) for kl_picture_orient.  A side over max_side
 * or pixels over max_pixels (0: no limit) are refused before the
 * decoding.
 *
 * Returns 0, EINVAL for a damaged JPEG or one of a kind not supported,
 * E2BIG for one too large, or ENOMEM; the picture is then empty.
 */
int
kl_picture_jpeg(
	FILE *file,
	const unsigned char *data,
	size_t size,
	unsigned max_side,
	unsigned long max_pixels,
	struct kl_picture *picture,
	int *orientation)
{
	int error;

	/* The whole picture: no box to reduce it to. */
	error = kl_picture_jpeg_reduced(file, data, size, 0U, 0, max_side, max_pixels, picture, orientation);
	if (error != 0)
		return error;

	/* Succeeded: the picture as stored. */
	return 0;
}

/*
 * Decodes a JPEG as kl_picture_jpeg does, scaled down by the largest of
 * 1/8, 1/4 and 1/2 (libjpeg's scaled decoding) that still leaves the
 * picture at least as large as a box of box_side by box_side needs in
 * either orientation: for cover (the box filled, the picture cut to it)
 * the picture's shorter side, for contain (the picture within the box)
 * its longer side, stays at least box_side.  box_side 0 decodes the
 * whole picture.  The limits apply to the scaled size, so a picture too
 * large to decode whole can still be decoded smaller.
 *
 * Returns 0, EINVAL, E2BIG or ENOMEM as kl_picture_jpeg.
 */
int
kl_picture_jpeg_reduced(
	FILE *file,
	const unsigned char *data,
	size_t size,
	unsigned box_side,
	int cover,
	unsigned max_side,
	unsigned long max_pixels,
	struct kl_picture *picture,
	int *orientation)
{
	struct jpeg_decompress_struct info;
	struct picture_jpeg_error failure;
	unsigned char *volatile row;
	JSAMPROW rows[1];
	unsigned char *line;
	uint32_t *out;
	unsigned cyan;
	unsigned magenta;
	unsigned yellow;
	unsigned black;
	unsigned denominator;
	unsigned x;
	int inverted;
	int cmyk;
	int large;

	/* Nothing yet. */
	memset(picture, 0, sizeof(*picture));
	*orientation = 1;

	/* A decompressor whose errors come back here. */
	memset(&info, 0, sizeof(info));
	info.err = jpeg_std_error(&failure.manager);
	failure.manager.error_exit = picture_jpeg_exit;
	failure.manager.emit_message = picture_jpeg_quiet;
	row = NULL;

	/*
	 * An error in libjpeg comes back here (setjmp must stand in the
	 * condition itself); row is volatile so that it survives the jump.
	 */
	if (setjmp(failure.back) != 0) {
		jpeg_destroy_decompress(&info);
		free(row);
		free(picture->pixels);
		picture->pixels = NULL;
		return EINVAL;
	}

	/* The decompressor, reading the file or the memory. */
	jpeg_create_decompress(&info);
	if (file != NULL) {
		jpeg_stdio_src(&info, file);
	} else {
		jpeg_mem_src(&info, data, (unsigned long)size);
	}

	/* The header, with APP1 (EXIF) kept for the orientation. */
	jpeg_save_markers(&info, PICTURE_JPEG_APP1, 0xffffU);
	(void)jpeg_read_header(&info, TRUE);
	*orientation = picture_jpeg_orientation(&info);

	/* RGB out, except CMYK, which is turned into RGB here. */
	if (info.jpeg_color_space == JCS_CMYK || info.jpeg_color_space == JCS_YCCK) {
		info.out_color_space = JCS_CMYK;
		cmyk = 1;
	} else {
		info.out_color_space = JCS_RGB;
		cmyk = 0;
	}

	/* The scale the box needs, and the size it gives. */
	denominator = picture_jpeg_denominator(info.image_width, info.image_height, box_side, cover);
	info.scale_num = 1;
	info.scale_denom = denominator;
	jpeg_calc_output_dimensions(&info);

	/* A size the program can hold at all, known before anything is decoded. */
	large = picture_too_large(info.output_width, info.output_height, max_side, max_pixels);
	if (large) {
		jpeg_destroy_decompress(&info);
		return E2BIG;
	}

	/* The decoding; Adobe's marker says whether CMYK is stored inverted. */
	inverted = info.saw_Adobe_marker;
	(void)jpeg_start_decompress(&info);

	/* The picture's size; a JPEG is always opaque. */
	picture->width = (int)info.output_width;
	picture->height = (int)info.output_height;
	picture->has_alpha = 0;

	/* Room for the picture's pixels. */
	picture->pixels = malloc((size_t)picture->width * (size_t)picture->height * sizeof(uint32_t));
	if (picture->pixels == NULL) {
		jpeg_destroy_decompress(&info);
		return ENOMEM;
	}

	/* Room for one row of samples as libjpeg gives them. */
	row = malloc((size_t)picture->width * (size_t)info.output_components);
	if (row == NULL) {
		jpeg_destroy_decompress(&info);
		free(picture->pixels);
		picture->pixels = NULL;
		return ENOMEM;
	}

	/* Each row, into opaque words. */
	line = row;
	while (info.output_scanline < info.output_height) {
		rows[0] = line;
		(void)jpeg_read_scanlines(&info, rows, 1U);
		out = picture->pixels + (size_t)(info.output_scanline - 1U) * (size_t)picture->width;

		/* Turns the row's samples into the picture's row. */
		for (x = 0; x < info.output_width; x++) {
			/* An RGB row has three samples a pixel. */
			if (!cmyk) {
				out[x] = kl_picture_premultiply(line[x * 3U], line[x * 3U + 1U], line[x * 3U + 2U], 255U);
				continue;
			}

			/* A CMYK row has four: the pixel's inks as libjpeg gives them. */
			cyan = line[x * 4U];
			magenta = line[x * 4U + 1U];
			yellow = line[x * 4U + 2U];
			black = line[x * 4U + 3U];

			/* Adobe's CMYK is stored inverted; plain CMYK is not, and is inverted here. */
			if (!inverted) {
				cyan = 255U - cyan;
				magenta = 255U - magenta;
				yellow = 255U - yellow;
				black = 255U - black;
			}

			/* The inks, now as light, into the picture. */
			out[x] = kl_picture_premultiply(cyan * black / 255U, magenta * black / 255U, yellow * black / 255U, 255U);
		}
	}

	/* The decompressor is done with. */
	(void)jpeg_finish_decompress(&info);
	jpeg_destroy_decompress(&info);
	free(row);

	/* Succeeded: the picture as stored. */
	return 0;
}

/*
 * Draws one frame of a GIF onto its screen, leaving its transparent colour's pixels as they are.
 */
void
kl_picture_gif_draw(
	const GifFileType *gif,
	int index,
	int transparent,
	uint32_t *screen)
{
	const SavedImage *frame;
	const ColorMapObject *map;
	const GifColorType *colour;
	unsigned colour_index;
	int screen_x;
	int screen_y;
	int x;
	int y;

	/* The frame's colour map: its own, or the screen's. */
	frame = &gif->SavedImages[index];
	map = frame->ImageDesc.ColorMap;
	if (map == NULL)
		map = gif->SColorMap;

	/* A frame without colours or pixels draws nothing. */
	if (map == NULL || frame->RasterBits == NULL)
		return;

	/* Each pixel of the frame that falls on the screen. */
	for (y = 0; y < frame->ImageDesc.Height; y++) {
		screen_y = frame->ImageDesc.Top + y;
		if (screen_y < 0 || screen_y >= gif->SHeight)
			continue;

		/* The row's pixels. */
		for (x = 0; x < frame->ImageDesc.Width; x++) {
			screen_x = frame->ImageDesc.Left + x;
			if (screen_x < 0 || screen_x >= gif->SWidth)
				continue;

			/* A transparent pixel, or a colour the map lacks, leaves the screen as it is. */
			colour_index = frame->RasterBits[(size_t)y * (size_t)frame->ImageDesc.Width + (size_t)x];
			if ((int)colour_index == transparent || (int)colour_index >= map->ColorCount)
				continue;

			/* The map's colour, opaque, onto the screen. */
			colour = &map->Colors[colour_index];
			screen[(size_t)screen_y * (size_t)gif->SWidth + (size_t)screen_x] = kl_picture_premultiply(colour->Red, colour->Green, colour->Blue, 255U);
		}
	}

	/* Succeeded: the frame has been composited onto its screen. */
	return;
}

/*
 * Makes a picture of a read GIF's (DGifSlurp) first frame on its clear
 * screen.  A side over max_side or pixels over max_pixels (0: no limit)
 * are refused.  Returns 0, EINVAL for a GIF without a frame, E2BIG or
 * ENOMEM; the picture is then empty.
 */
int
kl_picture_gif_first(
	GifFileType *gif,
	unsigned max_side,
	unsigned long max_pixels,
	struct kl_picture *picture)
{
	GraphicsControlBlock control;
	int large;
	int status;

	/* Nothing yet; a GIF without a frame has no picture. */
	memset(picture, 0, sizeof(*picture));
	if (gif->ImageCount < 1)
		return EINVAL;
	if (gif->SWidth <= 0 || gif->SHeight <= 0)
		return EINVAL;

	/* A screen of a size the program can hold. */
	large = picture_too_large((unsigned long)gif->SWidth, (unsigned long)gif->SHeight, max_side, max_pixels);
	if (large)
		return E2BIG;

	/* The screen, clear. */
	picture->pixels = calloc((size_t)gif->SWidth * (size_t)gif->SHeight, sizeof(uint32_t));
	if (picture->pixels == NULL)
		return ENOMEM;
	picture->width = gif->SWidth;
	picture->height = gif->SHeight;
	picture->has_alpha = 1;

	/* The first frame, with its transparent colour if it has one. */
	memset(&control, 0, sizeof(control));
	control.TransparentColor = NO_TRANSPARENT_COLOR;
	status = DGifSavedExtensionToGCB(gif, 0, &control);
	if (status != GIF_OK)
		control.TransparentColor = NO_TRANSPARENT_COLOR;

	/* Draws the first frame using any available transparency metadata. */
	kl_picture_gif_draw(gif, 0, control.TransparentColor, picture->pixels);

	/* Succeeded: the first frame on its screen. */
	return 0;
}

/* Ends a JPEG's decoding at an error: back to kl_picture_jpeg(). */
static void
picture_jpeg_exit(
	j_common_ptr info)
{
	struct picture_jpeg_error *failure;

	/* The error manager is the first member of picture_jpeg_error. */
	failure = (struct picture_jpeg_error *)(void *)info->err;
	longjmp(failure->back, 1);
}

/* Keeps libjpeg's warnings off the standard error (the program's log). */
static void
picture_jpeg_quiet(
	j_common_ptr info,
	int level)
{
	UNUSED_PARAMETER(info);
	UNUSED_PARAMETER(level);

	/* Succeeded: the warning has been discarded. */
	return;
}

/* Reads a JPEG's orientation from its saved APP1 markers (1 when there is none). */
static int
picture_jpeg_orientation(
	j_decompress_ptr info)
{
	jpeg_saved_marker_ptr marker;
	int orientation;

	/* The first APP1 that is EXIF gives it. */
	for (marker = info->marker_list;
	     marker != NULL;
	     marker = marker->next) {
		/* Another kind of marker says nothing of the orientation. */
		if (marker->marker != PICTURE_JPEG_APP1)
			continue;

		/* A readable orientation other than the default. */
		orientation = kl_picture_exif_orientation(marker->data, marker->data_length);
		if (orientation != 1)
			return orientation;
	}

	/* Succeeded: the default orientation keeps the picture as stored. */
	return 1;
}

/* Chooses the scale (1, 2, 4 or 8 for 1/1 to 1/8) a JPEG of a size is decoded at for a box. */
static unsigned
picture_jpeg_denominator(
	unsigned long width,
	unsigned long height,
	unsigned box_side,
	int cover)
{
	unsigned long side;
	unsigned denominator;

	/* No box: the whole picture. */
	if (box_side == 0U)
		return 1U;

	/* The side that must stay at least the box's: the shorter for cover, the longer for contain. */
	side = width;
	if (cover && height < width) {
		side = height;
	} else if (!cover && height > width) {
		side = height;
	}

	/* The largest denominator whose scaled side still covers the box. */
	for (denominator = 8U; denominator > 1U; denominator /= 2U) {
		if ((unsigned long)denominator * (unsigned long)box_side <= side)
			return denominator;
	}

	/* No reduction leaves enough: the whole picture. */
	return 1U;
}

/* Tells whether a size is empty or over the limits (0: none). */
static int
picture_too_large(
	unsigned long width,
	unsigned long height,
	unsigned max_side,
	unsigned long max_pixels)
{
	/* An empty picture is refused as too large to hold (it has nothing to show). */
	if (width == 0UL || height == 0UL)
		return 1;

	/* A side over the limit. */
	if (max_side != 0U) {
		if (width > max_side || height > max_side)
			return 1;
	}

	/* Pixels over the limit. */
	if (max_pixels != 0UL && width > max_pixels / height)
		return 1;

	/* Succeeded: the dimensions fit the caller's limits. */
	return 0;
}

/* Reads a 16-bit number in a byte order. */
static unsigned
picture_read16(
	const unsigned char *data,
	int big_endian)
{
	/* The most significant byte first, or last. */
	if (big_endian)
		return ((unsigned)data[0] << 8) | (unsigned)data[1];

	/* Succeeded: reports the decoded little-endian number. */
	return ((unsigned)data[1] << 8) | (unsigned)data[0];
}

/* Reads a 32-bit number in a byte order. */
static unsigned long
picture_read32(
	const unsigned char *data,
	int big_endian)
{
	/* The most significant byte first, or last. */
	if (big_endian) {
		return ((unsigned long)data[0] << 24) | ((unsigned long)data[1] << 16) |
		    ((unsigned long)data[2] << 8) | (unsigned long)data[3];
	}

	/* Succeeded: reports the decoded little-endian number. */
	return ((unsigned long)data[3] << 24) | ((unsigned long)data[2] << 16) |
	    ((unsigned long)data[1] << 8) | (unsigned long)data[0];
}
