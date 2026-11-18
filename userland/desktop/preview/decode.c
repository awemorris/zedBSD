/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The decoding of keiland-preview (WS168 p003; preview.h): the input's
 * kind told by its first bytes, and decoded into premultiplied pixels --
 * a PNG (libpng-compat), a JPEG turned as its EXIF orientation says and a
 * GIF's first frame (libjpeg-compat and libgif-compat through the
 * decoding Image Viewer and Files share, userland/desktop/picture), a
 * binary PPM or PGM of 8 bits, and a PDF's first page drawn on white by
 * libpdf at the scale the size asked needs.  libpdf is built for this
 * program without substitute font files (PDF_FONT_FILES 0): the sandbox
 * opens no file, so a font the document does not embed is not drawn.
 */

#include "preview.h"

#include "userland/desktop/picture/picture.h"

#include <compat/png/png.h>
#include <errno.h>
#include <pdf.h>
#include <stdlib.h>
#include <string.h>

/* The kinds told by the first bytes. */
#define DECODE_NONE		0
#define DECODE_PNG		1
#define DECODE_JPEG		2
#define DECODE_GIF		3
#define DECODE_PNM		4
#define DECODE_PDF		5

/* A GIF's bytes being read by libgif-compat: the bytes, their count and how far it has read. */
struct decode_gif_source {
	const unsigned char *data;
	size_t size;
	size_t at;
};

static int decode_kind(const unsigned char *data, size_t size);
static int decode_png(const unsigned char *data, size_t size, struct preview_image *image);
static int decode_jpeg(const unsigned char *data, size_t size, const struct preview_request *request, struct preview_image *image);
static int decode_gif(const unsigned char *data, size_t size, struct preview_image *image);
static int decode_gif_read(GifFileType *gif, GifByteType *bytes, int count);
static int decode_pnm(const unsigned char *data, size_t size, struct preview_image *image);
static int decode_pnm_number(const unsigned char *data, size_t size, size_t *at, unsigned long *number);
static int decode_pdf(const unsigned char *data, size_t size, const struct preview_request *request, struct preview_image *image);
static int decode_status(int error);

/*
 * Decodes the input into a picture (a PDF's page at the size asked, a
 * picture as it is).  Returns PREVIEW_OK with the picture (the caller's,
 * preview_image_release), or the exit status of the failure.
 */
int
preview_decode(
	const unsigned char *data,
	size_t size,
	const struct preview_request *request,
	struct preview_image *image)
{
	int kind;
	int error;

	/* The kind, and its decoder. */
	memset(image, 0, sizeof(*image));
	kind = decode_kind(data, size);
	switch (kind) {
	case DECODE_PNG:
		error = decode_png(data, size, image);
		break;
	case DECODE_JPEG:
		error = decode_jpeg(data, size, request, image);
		break;
	case DECODE_GIF:
		error = decode_gif(data, size, image);
		break;
	case DECODE_PNM:
		error = decode_pnm(data, size, image);
		break;
	case DECODE_PDF:
		error = decode_pdf(data, size, request, image);
		break;
	default:
		return PREVIEW_UNKNOWN;
	}

	/* The status of it. */
	return decode_status(error);
}

/* Tells the kind of the input by its first bytes (DECODE_*). */
static int
decode_kind(
	const unsigned char *data,
	size_t size)
{
	int same;

	/* PNG's signature. */
	if (size >= 8U) {
		same = memcmp(data, "\x89PNG\r\n\x1a\n", 8U);
		if (same == 0)
			return DECODE_PNG;
	}

	/* JPEG: FF D8 FF. */
	if (size >= 3U && data[0] == 0xffU && data[1] == 0xd8U && data[2] == 0xffU)
		return DECODE_JPEG;

	/* GIF87a or GIF89a. */
	if (size >= 6U) {
		same = memcmp(data, "GIF8", 4U);
		if (same == 0 && (data[4] == '7' || data[4] == '9') && data[5] == 'a')
			return DECODE_GIF;
	}

	/* A binary PPM or PGM. */
	if (size >= 3U && data[0] == 'P' && (data[1] == '5' || data[1] == '6'))
		return DECODE_PNM;

	/* "%PDF-". */
	if (size >= 5U) {
		same = memcmp(data, "%PDF-", 5U);
		if (same == 0)
			return DECODE_PDF;
	}

	/* Not one of them. */
	return DECODE_NONE;
}

/* Decodes a PNG into premultiplied pixels; 0 or an errno value (EFBIG over the limits). */
static int
decode_png(
	const unsigned char *data,
	size_t size,
	struct preview_image *image)
{
	png_image png;
	unsigned char *bytes;
	const unsigned char *pixel;
	size_t index;
	size_t count;
	int ok;

	/* The header, within the limits. */
	memset(&png, 0, sizeof(png));
	png.version = PNG_IMAGE_VERSION;
	ok = png_image_begin_read_from_memory(&png, data, size);
	if (!ok)
		return EINVAL;
	if (png.width == 0U || png.height == 0U || png.width > PREVIEW_DECODE_SIDE || png.height > PREVIEW_DECODE_SIDE ||
	    (unsigned long)png.width * png.height > PREVIEW_DECODE_PIXELS) {
		png_image_free(&png);
		return EFBIG;
	}

	/* The pixels, 8-bit BGRA. */
	png.format = PNG_FORMAT_BGRA;
	bytes = malloc(PNG_IMAGE_SIZE(png));
	if (bytes == NULL) {
		png_image_free(&png);
		return ENOMEM;
	}

	/* Decoded. */
	ok = png_image_finish_read(&png, NULL, bytes, 0, NULL);
	if (!ok) {
		free(bytes);
		return EINVAL;
	}

	/* The picture. */
	count = (size_t)png.width * (size_t)png.height;
	image->pixels = malloc(count * sizeof(image->pixels[0]));
	if (image->pixels == NULL) {
		free(bytes);
		return ENOMEM;
	}

	/* Its size. */
	image->width = (int)png.width;
	image->height = (int)png.height;

	/* Each pixel premultiplied. */
	for (index = 0; index < count; index++) {
		pixel = bytes + index * 4U;
		image->pixels[index] = kl_picture_premultiply(pixel[2], pixel[1], pixel[0], pixel[3]);
	}

	/* The bytes go. */
	free(bytes);
	return 0;
}

/*
 * Decodes a JPEG, turned as its EXIF orientation says; 0 or an errno
 * value.  It is decoded scaled down (1/2, 1/4 or 1/8, ws157-p017) as far
 * as the size asked allows in either orientation, so a large photo's
 * preview decodes only the samples it needs, and the limits apply to
 * that scaled size.
 */
static int
decode_jpeg(
	const unsigned char *data,
	size_t size,
	const struct preview_request *request,
	struct preview_image *image)
{
	struct kl_picture picture;
	unsigned box_side;
	int orientation;
	int error;

	/* The box's longer side, which the picture must still fill or fit. */
	box_side = (unsigned)request->width;
	if (request->height > request->width)
		box_side = (unsigned)request->height;

	/* Decoded at that scale, within the limits. */
	error = kl_picture_jpeg_reduced(NULL, data, size, box_side, request->cover, PREVIEW_DECODE_SIDE, PREVIEW_DECODE_PIXELS, &picture, &orientation);
	if (error != 0)
		return error;

	/* Turned upright. */
	error = kl_picture_orient(&picture, orientation);
	if (error != 0) {
		free(picture.pixels);
		return error;
	}

	/* The picture takes the pixels. */
	image->pixels = picture.pixels;
	image->width = picture.width;
	image->height = picture.height;
	return 0;
}

/* Decodes a GIF's first frame; 0 or an errno value. */
static int
decode_gif(
	const unsigned char *data,
	size_t size,
	struct preview_image *image)
{
	struct decode_gif_source source;
	struct kl_picture picture;
	GifFileType *gif;
	int status;
	int error;

	/* The records, read from the bytes. */
	source.data = data;
	source.size = size;
	source.at = 0;
	error = 0;
	gif = DGifOpen(&source, decode_gif_read, &error);
	if (gif == NULL)
		return EINVAL;
	status = DGifSlurp(gif);
	if (status != GIF_OK) {
		(void)DGifCloseFile(gif, &error);
		return EINVAL;
	}

	/* The first frame on its clear screen. */
	error = kl_picture_gif_first(gif, PREVIEW_DECODE_SIDE, PREVIEW_DECODE_PIXELS, &picture);
	(void)DGifCloseFile(gif, &status);
	if (error != 0)
		return error;

	/* The picture takes the pixels. */
	image->pixels = picture.pixels;
	image->width = picture.width;
	image->height = picture.height;
	return 0;
}

/* Gives libgif-compat up to a count of the GIF's bytes; returns how many were given. */
static int
decode_gif_read(
	GifFileType *gif,
	GifByteType *bytes,
	int count)
{
	struct decode_gif_source *source;
	size_t left;

	/* The bytes not read yet, no more than asked for. */
	source = gif->UserData;
	left = source->size - source->at;
	if (count < 0)
		return 0;
	if ((size_t)count < left)
		left = (size_t)count;

	/* Copied out. */
	memcpy(bytes, source->data + source->at, left);
	source->at += left;
	return (int)left;
}

/* Decodes a binary PPM (P6) or PGM (P5) of 8 bits into an opaque picture; 0 or an errno value. */
static int
decode_pnm(
	const unsigned char *data,
	size_t size,
	struct preview_image *image)
{
	unsigned long width;
	unsigned long height;
	unsigned long maximum;
	const unsigned char *pixel;
	size_t channels;
	size_t count;
	size_t index;
	size_t at;
	int error;

	/* The header: the size and the largest value (255 only). */
	at = 2;
	error = decode_pnm_number(data, size, &at, &width);
	if (error == 0)
		error = decode_pnm_number(data, size, &at, &height);
	if (error == 0)
		error = decode_pnm_number(data, size, &at, &maximum);
	if (error != 0 || maximum != 255UL || at >= size)
		return EINVAL;
	at++;
	if (width == 0UL || height == 0UL || width > PREVIEW_DECODE_SIDE || height > PREVIEW_DECODE_SIDE || width * height > PREVIEW_DECODE_PIXELS)
		return EFBIG;

	/* The pixels' bytes are all there. */
	channels = 3U;
	if (data[1] == '5')
		channels = 1U;
	count = (size_t)width * (size_t)height;
	if (size - at < count * channels)
		return EINVAL;

	/* The picture, opaque. */
	image->pixels = malloc(count * sizeof(image->pixels[0]));
	if (image->pixels == NULL)
		return ENOMEM;
	image->width = (int)width;
	image->height = (int)height;
	for (index = 0; index < count; index++) {
		pixel = data + at + index * channels;
		if (channels == 1U)
			image->pixels[index] = 0xff000000U | ((uint32_t)pixel[0] << 16) | ((uint32_t)pixel[0] << 8) | pixel[0];
		else
			image->pixels[index] = 0xff000000U | ((uint32_t)pixel[0] << 16) | ((uint32_t)pixel[1] << 8) | pixel[2];
	}

	/* Decoded. */
	return 0;
}

/* Reads a header's decimal number after white space and comments; 0, or EINVAL. */
static int
decode_pnm_number(
	const unsigned char *data,
	size_t size,
	size_t *at,
	unsigned long *number)
{
	int digits;

	/* White space and comments. */
	while (*at < size) {
		if (data[*at] == '#') {
			while (*at < size && data[*at] != '\n')
				(*at)++;
			continue;
		}

		/* Not white space: the number starts. */
		if (data[*at] != ' ' && data[*at] != '\t' && data[*at] != '\r' && data[*at] != '\n')
			break;
		(*at)++;
	}

	/* The digits (a number too long is refused). */
	*number = 0;
	digits = 0;
	while (*at < size && data[*at] >= '0' && data[*at] <= '9' && digits < 9) {
		*number = *number * 10UL + (unsigned long)(data[*at] - '0');
		(*at)++;
		digits++;
	}

	/* A number. */
	if (digits == 0)
		return EINVAL;
	return 0;
}

/* Draws a PDF's first page on white at the scale the size asked needs; 0 or an errno value. */
static int
decode_pdf(
	const unsigned char *data,
	size_t size,
	const struct preview_request *request,
	struct preview_image *image)
{
	struct pdf_display_list *list;
	struct pdf_document *document;
	struct pdf_page_box box;
	double width;
	double height;
	double scale;
	size_t index;
	size_t count;
	int fitted_width;
	int fitted_height;
	int error;

	/* The document and its first page. */
	document = NULL;
	error = pdf_document_open_memory(data, size, &document);
	if (error != 0 || document == NULL)
		return EINVAL;
	count = pdf_document_page_count(document);
	memset(&box, 0, sizeof(box));
	error = EINVAL;
	if (count > 0U)
		error = pdf_document_page_box(document, 0, &box);
	if (error != 0) {
		pdf_document_close(document);
		return EINVAL;
	}

	/* The page's size as shown (libpdf's, turned and cropped), else the crop box's, else the media box's. */
	width = box.width;
	height = box.height;
	if (width <= 0.0 || height <= 0.0) {
		width = box.crop_right - box.crop_left;
		height = box.crop_top - box.crop_bottom;
	}

	/* Else the media box. */
	if (width <= 0.0 || height <= 0.0) {
		width = box.media_right - box.media_left;
		height = box.media_top - box.media_bottom;
	}

	/* A page of no size, or of an absurd one, is not drawn. */
	if (width <= 0.0 || height <= 0.0 || width > 100000.0 || height > 100000.0) {
		pdf_document_close(document);
		return EINVAL;
	}

	/* The scale: contain fits the size (a page may be made larger), cover fills it. */
	scale = (double)request->width / width;
	if (request->cover && (double)request->height / height > scale)
		scale = (double)request->height / height;
	else if (!request->cover && (double)request->height / height < scale)
		scale = (double)request->height / height;
	fitted_width = (int)(width * scale + 0.5);
	fitted_height = (int)(height * scale + 0.5);
	if (fitted_width < 1)
		fitted_width = 1;
	if (fitted_height < 1)
		fitted_height = 1;

	/* White, like paper. */
	count = (size_t)fitted_width * (size_t)fitted_height;
	image->pixels = malloc(count * sizeof(image->pixels[0]));
	if (image->pixels == NULL) {
		pdf_document_close(document);
		return ENOMEM;
	}

	/* Its size. */
	image->width = fitted_width;
	image->height = fitted_height;
	for (index = 0; index < count; index++)
		image->pixels[index] = 0xffffffffU;

	/* The page drawn on it. */
	list = NULL;
	error = pdf_page_render(document, 0, &list);
	if (error == 0 && list != NULL) {
		error = pdf_display_list_rasterize(list, image->pixels, (size_t)fitted_width, (size_t)fitted_width, (size_t)fitted_height, scale, 0.0, 0.0);
		pdf_display_list_destroy(list);
	}

	/* Drawn, or nothing. */
	pdf_document_close(document);
	if (error != 0) {
		preview_image_release(image);
		return EINVAL;
	}

	/* Drawn. */
	return 0;
}

/* Turns a decoder's errno value into the exit status. */
static int
decode_status(
	int error)
{
	/* Each kind of failure. */
	switch (error) {
	case 0:
		return PREVIEW_OK;
	case EFBIG:
	case E2BIG:
		return PREVIEW_TOO_LARGE;
	case ENOMEM:
		return PREVIEW_NO_MEMORY;
	default:
		return PREVIEW_DAMAGED;
	}
}
