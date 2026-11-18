/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The image XObjects of libpdf's reader: an image stream decoded into RGBA
 * pixels for the display list.
 *
 * Stage 1 of design-pdf.md reads the images Notes writes and the common
 * cases around them: JPEG (DCTDecode, through libjpeg-compat), and samples
 * of 1, 2, 4, 8 or 16 bits in DeviceGray, DeviceRGB, DeviceCMYK, their
 * calibrated and ICC-based forms, and Indexed, possibly Flate-compressed;
 * a soft mask (/SMask) gives the alpha, and a stencil mask (/ImageMask) is
 * painted in the fill colour.  Anything else reports ENOTSUP and the page
 * leaves the image out.
 *
 * The stream is not trusted: the size is bounded, the sample count is
 * checked against overflow, and missing samples read as zero.
 */

#include <errno.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <compat/jpeglib.h>

#include <pdf.h>

#include "internal.h"

/* The most colour components a sample may have. */
#define PDF_IMAGE_COMPONENTS_MAX 4

/*
 * The colour space of an image's samples.
 *
 * components is how many values a sample has.  An indexed space maps one
 * value through its palette (palette_count entries of base_components
 * bytes) into the base space.
 */
struct pdf_image_space {
	int components;
	int indexed;
	int base_components;
	const unsigned char *palette;
	size_t palette_count;
};

/*
 * The error manager of a JPEG decoding: libjpeg's, whose exit jumps back to
 * the decoder instead of ending the program.
 */
struct pdf_jpeg_error {
	struct jpeg_error_mgr manager;
	jmp_buf back;
};

static int decode_image(struct pdf_document *document, const struct pdf_object *stream, const double fill[3], int with_soft_mask, unsigned char **pixels, size_t *width, size_t *height, int *interpolate, unsigned *flags);
static int read_size(struct pdf_document *document, const struct pdf_object *stream, size_t *width, size_t *height);
static int read_space(struct pdf_document *document, struct pdf_object *space, int depth, struct pdf_image_space *result, unsigned char **palette_owned);
static int read_palette(struct pdf_document *document, struct pdf_object *lookup, struct pdf_image_space *result, unsigned char **palette_owned);
static int decode_samples(struct pdf_document *document, const struct pdf_object *stream, const unsigned char *data, size_t size, size_t width, size_t height, unsigned char *pixels);
static int decode_stencil(struct pdf_document *document, const struct pdf_object *stream, const unsigned char *data, size_t size, size_t width, size_t height, const double fill[3], unsigned char *pixels);
static int decode_jpeg(const unsigned char *data, size_t size, size_t width, size_t height, unsigned char *pixels);
static void jpeg_exit(j_common_ptr cinfo);
static void jpeg_message(j_common_ptr cinfo);
static void store_color(const struct pdf_image_space *space, const double *values, unsigned char *pixel);
static unsigned read_sample(const unsigned char *data, size_t size, size_t bit_offset, int bits);
static int read_decode(struct pdf_document *document, const struct pdf_object *stream, const struct pdf_image_space *space, int bits, double *minimum, double *maximum);
static int apply_soft_mask(struct pdf_document *document, const struct pdf_object *stream, size_t width, size_t height, unsigned char *pixels);
static int read_boolean(struct pdf_document *document, const struct pdf_object *stream, const char *key, int *value);
static unsigned char to_byte(double value);

/*
 * Decodes an image XObject into RGBA pixels (malloc'd, the caller frees).
 *
 * fill is the fill colour a stencil mask paints in.  flags gets
 * PDF_DISPLAY_SKIPPED when a part of the image (a colour-key mask, a soft
 * mask that cannot be read) is left out while the image itself is drawn.
 */
int
pdf_image_decode(
	struct pdf_document *document,
	const struct pdf_object *stream,
	const double fill[3],
	unsigned char **pixels,
	size_t *width,
	size_t *height,
	int *interpolate,
	unsigned *flags)
{
	int error;

	/* Decodes the image with its soft mask. */
	error = decode_image(document, stream, fill, 1, pixels, width, height, interpolate, flags);
	if (error != 0)
		return error;

	/* Succeeded: the image's pixels. */
	return 0;
}

/*
 * Decodes an image, with its soft mask when asked to (a soft mask's own
 * soft mask is never read, so a mask that names itself ends).
 */
static int
decode_image(
	struct pdf_document *document,
	const struct pdf_object *stream,
	const double fill[3],
	int with_soft_mask,
	unsigned char **pixels,
	size_t *width,
	size_t *height,
	int *interpolate,
	unsigned *flags)
{
	const unsigned char *data;
	unsigned char *owned;
	unsigned char *created;
	struct pdf_object *mask;
	size_t size;
	int stencil;
	int dct;
	int error;

	/* Reads the size, which bounds everything else. */
	error = read_size(document, stream, width, height);
	if (error != 0)
		return error;

	/* Reads whether to smooth the image when it is scaled. */
	error = read_boolean(document, stream, "Interpolate", interpolate);
	if (error != 0)
		return error;

	/* Allocates the pixels; the size check keeps the product within the limit. */
	created = malloc(*width * *height * 4);
	if (created == NULL)
		return ENOMEM;

	/* Decodes the stream's filters, leaving a JPEG for libjpeg. */
	error = pdf_filter_decode(document, stream, 1, &data, &size, &owned, &dct);
	if (error != 0) {
		free(created);
		return error;
	}

	/* Reads whether the image is a stencil mask. */
	error = read_boolean(document, stream, "ImageMask", &stencil);
	if (error != 0) {
		free(owned);
		free(created);
		return error;
	}

	/* Decodes the samples: a JPEG, a stencil or ordinary samples. */
	if (dct) {
		error = decode_jpeg(data, size, *width, *height, created);
	} else if (stencil) {
		error = decode_stencil(document, stream, data, size, *width, *height, fill, created);
	} else {
		error = decode_samples(document, stream, data, size, *width, *height, created);
	}

	/* The decoded bytes are not needed any more; a failed decode frees its pixels. */
	free(owned);
	if (error != 0) {
		free(created);
		return error;
	}

	/* Applies the soft mask, whose failure leaves the image opaque. */
	if (with_soft_mask) {
		error = apply_soft_mask(document, stream, *width, *height, created);
		if (error != 0)
			*flags |= PDF_DISPLAY_SKIPPED;
	}

	/* A colour-key or stencil /Mask is not drawn yet. */
	mask = pdf_object_get(stream, "Mask");
	if (mask != NULL)
		*flags |= PDF_DISPLAY_SKIPPED;

	/* Succeeded: the image's pixels. */
	*pixels = created;
	return 0;
}

/* Reads an image's width and height, which must be positive and within the limits. */
static int
read_size(
	struct pdf_document *document,
	const struct pdf_object *stream,
	size_t *width,
	size_t *height)
{
	struct pdf_object *value;
	int error;

	/* Reads the width. */
	error = pdf_reader_resolve_key(document, stream, "Width", &value);
	if (error != 0)
		return error;
	if (value->type != PDF_OBJECT_INTEGER)
		return PDF_EFORMAT;
	if (value->integer < 1 || value->integer > PDF_IMAGE_SIDE_MAX)
		return PDF_EFORMAT;
	*width = (size_t)value->integer;

	/* Reads the height. */
	error = pdf_reader_resolve_key(document, stream, "Height", &value);
	if (error != 0)
		return error;
	if (value->type != PDF_OBJECT_INTEGER)
		return PDF_EFORMAT;
	if (value->integer < 1 || value->integer > PDF_IMAGE_SIDE_MAX)
		return PDF_EFORMAT;
	*height = (size_t)value->integer;

	/* Refuses an image with more pixels than one display list holds. */
	if (*width * *height > PDF_DISPLAY_PIXELS_MAX)
		return ENOMEM;

	/* Succeeded: the size is usable. */
	return 0;
}

/*
 * Reads a colour space: a device space by name, a calibrated or ICC-based
 * space by its components, or an indexed space over one of those.
 */
static int
read_space(
	struct pdf_document *document,
	struct pdf_object *space,
	int depth,
	struct pdf_image_space *result,
	unsigned char **palette_owned)
{
	struct pdf_object *family;
	struct pdf_object *parameter;
	struct pdf_object *count;
	struct pdf_image_space base;
	int is_name;
	int error;

	/* Refuses a space nested past an indexed space's base. */
	memset(result, 0, sizeof(*result));
	if (depth > 1)
		return ENOTSUP;

	/* The device spaces by name, with their inline-image abbreviations. */
	if (space->type == PDF_OBJECT_NAME) {
		is_name = pdf_object_is_name(space, "DeviceGray");
		if (is_name == 0)
			is_name = pdf_object_is_name(space, "G");
		if (is_name) {
			result->components = 1;
			return 0;
		}

		/* Three components for RGB. */
		is_name = pdf_object_is_name(space, "DeviceRGB");
		if (is_name == 0)
			is_name = pdf_object_is_name(space, "RGB");
		if (is_name) {
			result->components = 3;
			return 0;
		}

		/* Four for CMYK. */
		is_name = pdf_object_is_name(space, "DeviceCMYK");
		if (is_name == 0)
			is_name = pdf_object_is_name(space, "CMYK");
		if (is_name) {
			result->components = 4;
			return 0;
		}

		/* Any other name (a pattern, a separation) is not read yet. */
		return ENOTSUP;
	}

	/* Anything else must be an array that starts with the family's name. */
	if (space->type != PDF_OBJECT_ARRAY || space->count == 0)
		return PDF_EFORMAT;
	error = pdf_reader_resolve(document, space->values[0], &family);
	if (error != 0)
		return error;

	/* The calibrated spaces have the components of their device spaces. */
	is_name = pdf_object_is_name(family, "CalGray");
	if (is_name) {
		result->components = 1;
		return 0;
	}

	/* The calibrated RGB space has three. */
	is_name = pdf_object_is_name(family, "CalRGB");
	if (is_name) {
		result->components = 3;
		return 0;
	}

	/* An ICC-based space has the components its profile stream names. */
	is_name = pdf_object_is_name(family, "ICCBased");
	if (is_name) {
		if (space->count < 2)
			return PDF_EFORMAT;
		error = pdf_reader_resolve(document, space->values[1], &parameter);
		if (error != 0)
			return error;
		if (parameter->type != PDF_OBJECT_STREAM)
			return PDF_EFORMAT;
		error = pdf_reader_resolve_key(document, parameter, "N", &count);
		if (error != 0)
			return error;
		if (count->type != PDF_OBJECT_INTEGER)
			return PDF_EFORMAT;
		if (count->integer != 1 &&
		    count->integer != 3 &&
		    count->integer != 4)
			return ENOTSUP;
		result->components = (int)count->integer;
		return 0;
	}

	/* Anything but an indexed space is not read yet. */
	is_name = pdf_object_is_name(family, "Indexed");
	if (is_name == 0)
		is_name = pdf_object_is_name(family, "I");
	if (is_name == 0)
		return ENOTSUP;
	if (space->count < 4)
		return PDF_EFORMAT;

	/* The base space, which must not itself be indexed. */
	error = pdf_reader_resolve(document, space->values[1], &parameter);
	if (error != 0)
		return error;
	error = read_space(document, parameter, depth + 1, &base, palette_owned);
	if (error != 0)
		return error;
	if (base.indexed)
		return PDF_EFORMAT;

	/* The highest index. */
	error = pdf_reader_resolve(document, space->values[2], &count);
	if (error != 0)
		return error;
	if (count->type != PDF_OBJECT_INTEGER)
		return PDF_EFORMAT;
	if (count->integer < 0 || count->integer > 255)
		return PDF_EFORMAT;

	/* The indexed space: one value, looked up in the palette. */
	result->components = 1;
	result->indexed = 1;
	result->base_components = base.components;
	result->palette_count = (size_t)count->integer + 1;

	/* The palette itself. */
	error = pdf_reader_resolve(document, space->values[3], &parameter);
	if (error != 0)
		return error;
	error = read_palette(document, parameter, result, palette_owned);
	if (error != 0)
		return error;

	/* Succeeded: the indexed space. */
	return 0;
}

/* Reads an indexed space's palette from a string or a stream; a short palette is padded with black. */
static int
read_palette(
	struct pdf_document *document,
	struct pdf_object *lookup,
	struct pdf_image_space *result,
	unsigned char **palette_owned)
{
	const unsigned char *data;
	unsigned char *owned;
	unsigned char *palette;
	size_t needed;
	size_t size;
	int dct;
	int error;

	/* The palette's bytes: a string's, or a stream's decoded ones. */
	owned = NULL;
	if (lookup->type == PDF_OBJECT_STRING) {
		data = lookup->bytes;
		size = lookup->length;
	} else if (lookup->type == PDF_OBJECT_STREAM) {
		error = pdf_filter_decode(document, lookup, 0, &data, &size, &owned, &dct);
		if (error != 0)
			return error;
	} else {
		return PDF_EFORMAT;
	}

	/* Copies the palette, padded to its full size. */
	needed = result->palette_count * (size_t)result->base_components;
	palette = calloc(needed, 1);
	if (palette == NULL) {
		free(owned);
		return ENOMEM;
	}

	/* Copies the palette's bytes, at most its full size. */
	if (size > needed)
		size = needed;
	memcpy(palette, data, size);
	free(owned);

	/* Succeeded: the space's palette, which the caller frees. */
	result->palette = palette;
	*palette_owned = palette;
	return 0;
}

/* Decodes an image's ordinary samples into opaque RGBA pixels. */
static int
decode_samples(
	struct pdf_document *document,
	const struct pdf_object *stream,
	const unsigned char *data,
	size_t size,
	size_t width,
	size_t height,
	unsigned char *pixels)
{
	struct pdf_image_space space;
	struct pdf_object *space_object;
	struct pdf_object *bits_object;
	unsigned char *palette_owned;
	double minimum[PDF_IMAGE_COMPONENTS_MAX];
	double maximum[PDF_IMAGE_COMPONENTS_MAX];
	double values[PDF_IMAGE_COMPONENTS_MAX];
	double top;
	size_t row_bits;
	size_t x;
	size_t y;
	size_t bit;
	unsigned sample;
	int component;
	int bits;
	int error;

	/* Reads the colour space. */
	palette_owned = NULL;
	error = pdf_reader_resolve_key(document, stream, "ColorSpace", &space_object);
	if (error != 0)
		return error;
	error = read_space(document, space_object, 0, &space, &palette_owned);
	if (error != 0) {
		free(palette_owned);
		return error;
	}

	/* Reads the bits a sample takes. */
	error = pdf_reader_resolve_key(document, stream, "BitsPerComponent", &bits_object);
	if (error != 0) {
		free(palette_owned);
		return error;
	}

	/* Refuses a sample size that is not a whole bit count PDF allows. */
	bits = 0;
	if (bits_object->type == PDF_OBJECT_INTEGER && bits_object->integer <= 16)
		bits = (int)bits_object->integer;
	if (bits != 1 &&
	    bits != 2 &&
	    bits != 4 &&
	    bits != 8 &&
	    bits != 16) {
		free(palette_owned);
		return PDF_EFORMAT;
	}

	/* Reads the decode ranges. */
	error = read_decode(document, stream, &space, bits, minimum, maximum);
	if (error != 0) {
		free(palette_owned);
		return error;
	}

	/* Converts each sample; rows start on a byte, and a 16-bit sample keeps its high byte's precision. */
	top = (double)((1UL << bits) - 1);
	if (bits == 16)
		top = 255.0;
	row_bits = ((width * (size_t)space.components * (size_t)bits + 7) / 8) * 8;
	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			/* The sample's components, mapped through the decode ranges. */
			bit = y * row_bits + x * (size_t)space.components * (size_t)bits;
			for (component = 0; component < space.components; component++) {
				sample = read_sample(data, size, bit + (size_t)component * (size_t)bits, bits);
				values[component] = minimum[component] + (double)sample * (maximum[component] - minimum[component]) / top;
			}

			/* The pixel in RGB, opaque. */
			store_color(&space, values, pixels + (y * width + x) * 4);
			pixels[(y * width + x) * 4 + 3] = 255;
		}
	}

	/* Succeeded: the pixels are the image's. */
	free(palette_owned);
	return 0;
}

/* Decodes a stencil mask: painted samples in the fill colour, the others clear. */
static int
decode_stencil(
	struct pdf_document *document,
	const struct pdf_object *stream,
	const unsigned char *data,
	size_t size,
	size_t width,
	size_t height,
	const double fill[3],
	unsigned char *pixels)
{
	struct pdf_object *decode;
	struct pdf_object *first;
	unsigned painted_value;
	unsigned sample;
	size_t row_bits;
	size_t x;
	size_t y;
	unsigned char *pixel;
	int error;

	/* A sample of 0 paints, unless the decode array is [1 0]. */
	painted_value = 0;
	error = pdf_reader_resolve_key(document, stream, "Decode", &decode);
	if (error != 0)
		return error;
	if (decode->type == PDF_OBJECT_ARRAY && decode->count >= 1) {
		error = pdf_reader_resolve(document, decode->values[0], &first);
		if (error != 0)
			return error;
		if (first->type == PDF_OBJECT_INTEGER && first->integer == 1)
			painted_value = 1;
	}

	/* Paints each sample, one bit each, rows starting on a byte. */
	row_bits = ((width + 7) / 8) * 8;
	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			sample = read_sample(data, size, y * row_bits + x, 1);
			pixel = pixels + (y * width + x) * 4;
			pixel[0] = to_byte(fill[0]);
			pixel[1] = to_byte(fill[1]);
			pixel[2] = to_byte(fill[2]);
			pixel[3] = 0;
			if (sample == painted_value)
				pixel[3] = 255;
		}
	}

	/* Succeeded: the mask is drawn in the fill colour. */
	return 0;
}

/*
 * Decodes a JPEG into opaque RGBA pixels of the image's size.
 *
 * A JPEG of another size than the image's dictionary says is malformed.
 * CMYK is made RGB as Adobe's files store it (inverted) or plainly.
 */
static int
decode_jpeg(
	const unsigned char *data,
	size_t size,
	size_t width,
	size_t height,
	unsigned char *pixels)
{
	struct jpeg_decompress_struct cinfo;
	struct pdf_jpeg_error error;
	JSAMPLE *volatile row;
	JSAMPROW rows[1];
	const JSAMPLE *sample;
	unsigned char *pixel;
	unsigned cyan;
	unsigned magenta;
	unsigned yellow;
	unsigned black;
	size_t line;
	size_t x;
	int jumped;

	/* Sets up the error manager, whose exit comes back here with everything freed. */
	row = NULL;
	memset(&cinfo, 0, sizeof(cinfo));
	cinfo.err = jpeg_std_error(&error.manager);
	error.manager.error_exit = jpeg_exit;
	error.manager.output_message = jpeg_message;
	jumped = setjmp(error.back);
	if (jumped != 0) {
		jpeg_destroy_decompress(&cinfo);
		free(row);
		return PDF_EFORMAT;
	}

	/* Reads the header from the bytes. */
	jpeg_create_decompress(&cinfo);
	jpeg_mem_src(&cinfo, data, (unsigned long)size);
	jpeg_read_header(&cinfo, TRUE);

	/* Asks for gray, RGB or CMYK as the file has it. */
	if (cinfo.jpeg_color_space == JCS_CMYK || cinfo.jpeg_color_space == JCS_YCCK) {
		cinfo.out_color_space = JCS_CMYK;
	} else if (cinfo.num_components == 1) {
		cinfo.out_color_space = JCS_GRAYSCALE;
	} else {
		cinfo.out_color_space = JCS_RGB;
	}

	/* Starts decoding and refuses a size other than the image's. */
	jpeg_start_decompress(&cinfo);
	if ((size_t)cinfo.output_width != width || (size_t)cinfo.output_height != height) {
		jpeg_destroy_decompress(&cinfo);
		return PDF_EFORMAT;
	}

	/* Allocates one row of samples. */
	row = malloc(width * (size_t)cinfo.output_components);
	if (row == NULL) {
		jpeg_destroy_decompress(&cinfo);
		return ENOMEM;
	}

	/* Converts each row into pixels. */
	for (line = 0; line < height; line++) {
		rows[0] = row;
		jpeg_read_scanlines(&cinfo, rows, 1);
		for (x = 0; x < width; x++) {
			sample = row + x * (size_t)cinfo.output_components;
			pixel = pixels + (line * width + x) * 4;
			if (cinfo.out_color_space == JCS_GRAYSCALE) {
				/* Gray. */
				pixel[0] = sample[0];
				pixel[1] = sample[0];
				pixel[2] = sample[0];
			} else if (cinfo.out_color_space == JCS_RGB) {
				/* RGB. */
				pixel[0] = sample[0];
				pixel[1] = sample[1];
				pixel[2] = sample[2];
			} else {
				/* CMYK: Adobe's files store the channels inverted. */
				cyan = sample[0];
				magenta = sample[1];
				yellow = sample[2];
				black = sample[3];
				if (!cinfo.saw_Adobe_marker) {
					cyan = 255U - cyan;
					magenta = 255U - magenta;
					yellow = 255U - yellow;
					black = 255U - black;
				}

				/* The ink subtracted from white: each colour times the black's lightness. */
				pixel[0] = (unsigned char)(cyan * black / 255U);
				pixel[1] = (unsigned char)(magenta * black / 255U);
				pixel[2] = (unsigned char)(yellow * black / 255U);
			}

			/* A JPEG is opaque. */
			pixel[3] = 255;
		}
	}

	/* Ends the decoding. */
	jpeg_finish_decompress(&cinfo);
	jpeg_destroy_decompress(&cinfo);
	free(row);

	/* Succeeded: the pixels are the JPEG's. */
	return 0;
}

/* libjpeg's error exit: back into the decoder. */
static void
jpeg_exit(
	j_common_ptr cinfo)
{
	struct pdf_jpeg_error *error;

	/* Jumps back without printing the message. */
	error = (struct pdf_jpeg_error *)cinfo->err;
	longjmp(error->back, 1);
}

/* libjpeg's warnings are not printed. */
static void
jpeg_message(
	j_common_ptr cinfo)
{
	/* A damaged JPEG still shows what could be decoded. */
	(void)cinfo;
}

/* Stores a sample's colour (values in the space's ranges) as the RGB bytes of a pixel. */
static void
store_color(
	const struct pdf_image_space *space,
	const double *values,
	unsigned char *pixel)
{
	const unsigned char *entry;
	double cyan;
	double magenta;
	double yellow;
	double black;
	long index;
	int components;

	/* An indexed sample looks up its entry, whose bytes are the base space's values. */
	components = space->components;
	if (space->indexed) {
		index = (long)(values[0] + 0.5);
		if (index < 0)
			index = 0;
		if ((size_t)index >= space->palette_count)
			index = (long)space->palette_count - 1;
		entry = space->palette + (size_t)index * (size_t)space->base_components;
		components = space->base_components;
		if (components == 1) {
			pixel[0] = entry[0];
			pixel[1] = entry[0];
			pixel[2] = entry[0];
		} else if (components == 3) {
			pixel[0] = entry[0];
			pixel[1] = entry[1];
			pixel[2] = entry[2];
		} else {
			pixel[0] = (unsigned char)((255U - entry[0]) * (255U - entry[3]) / 255U);
			pixel[1] = (unsigned char)((255U - entry[1]) * (255U - entry[3]) / 255U);
			pixel[2] = (unsigned char)((255U - entry[2]) * (255U - entry[3]) / 255U);
		}

		/* The base space's colour is the pixel's. */
		return;
	}

	/* Gray and RGB as they are; CMYK by the naive subtraction. */
	if (components == 1) {
		pixel[0] = to_byte(values[0]);
		pixel[1] = pixel[0];
		pixel[2] = pixel[0];
	} else if (components == 3) {
		pixel[0] = to_byte(values[0]);
		pixel[1] = to_byte(values[1]);
		pixel[2] = to_byte(values[2]);
	} else {
		cyan = values[0];
		magenta = values[1];
		yellow = values[2];
		black = values[3];
		pixel[0] = to_byte((1.0 - cyan) * (1.0 - black));
		pixel[1] = to_byte((1.0 - magenta) * (1.0 - black));
		pixel[2] = to_byte((1.0 - yellow) * (1.0 - black));
	}
}

/* Reads a sample of some bits at a bit offset; a sample past the data is 0; 16 bits give the high byte's 8. */
static unsigned
read_sample(
	const unsigned char *data,
	size_t size,
	size_t bit_offset,
	int bits)
{
	size_t byte;
	unsigned shift;
	unsigned value;

	/* A whole byte, or the high byte of two. */
	byte = bit_offset / 8;
	if (byte >= size)
		return 0;
	if (bits == 8)
		return data[byte];
	if (bits == 16)
		return data[byte];

	/* A part of a byte, from its high bits down. */
	shift = (unsigned)(8 - (int)(bit_offset % 8) - bits);
	value = ((unsigned)data[byte] >> shift) & ((1U << bits) - 1U);

	/* Reports the sample. */
	return value;
}

/*
 * Reads an image's decode ranges, one pair a component, with the defaults
 * of its space ([0 1] a component, [0 2^bits-1] for an index).
 *
 * The maxima are in the space's range; a 16-bit sample is read as 8 bits,
 * so the top of its range is 255 as for 8 bits.
 */
static int
read_decode(
	struct pdf_document *document,
	const struct pdf_object *stream,
	const struct pdf_image_space *space,
	int bits,
	double *minimum,
	double *maximum)
{
	struct pdf_object *decode;
	double number;
	int component;
	int error;

	/* The defaults. */
	for (component = 0; component < space->components; component++) {
		minimum[component] = 0.0;
		maximum[component] = 1.0;
		if (space->indexed) {
			maximum[component] = (double)((1UL << bits) - 1);
			if (bits == 16)
				maximum[component] = 255.0;
		}
	}

	/* The stream's own ranges, when it has a complete array of numbers. */
	error = pdf_reader_resolve_key(document, stream, "Decode", &decode);
	if (error != 0)
		return error;
	if (decode->type != PDF_OBJECT_ARRAY)
		return 0;
	if (decode->count < (size_t)space->components * 2)
		return 0;

	/* Reads each pair; a pair that is not numbers keeps the default. */
	for (component = 0; component < space->components; component++) {
		error = pdf_object_number(decode->values[component * 2], &number);
		if (error != 0)
			continue;
		minimum[component] = number;
		error = pdf_object_number(decode->values[component * 2 + 1], &number);
		if (error != 0)
			continue;
		maximum[component] = number;
	}

	/* Succeeded: the ranges are known. */
	return 0;
}

/*
 * Replaces the pixels' alpha with an image's soft mask, a gray image whose
 * samples are sampled to the image's size.
 */
static int
apply_soft_mask(
	struct pdf_document *document,
	const struct pdf_object *stream,
	size_t width,
	size_t height,
	unsigned char *pixels)
{
	struct pdf_object *mask;
	unsigned char *mask_pixels;
	size_t mask_width;
	size_t mask_height;
	size_t x;
	size_t y;
	size_t mask_x;
	size_t mask_y;
	int mask_interpolate;
	unsigned mask_flags;
	double black[3];
	int error;

	/* No soft mask leaves the alpha as it is. */
	error = pdf_reader_resolve_key(document, stream, "SMask", &mask);
	if (error != 0)
		return error;
	if (mask->type != PDF_OBJECT_STREAM)
		return 0;

	/* Decodes the mask as an image of its own (its gray lands in the red channel). */
	black[0] = 0.0;
	black[1] = 0.0;
	black[2] = 0.0;
	mask_flags = 0;
	error = decode_image(document, mask, black, 0, &mask_pixels, &mask_width, &mask_height, &mask_interpolate, &mask_flags);
	if (error != 0)
		return error;

	/* Takes each pixel's alpha from the nearest mask sample. */
	for (y = 0; y < height; y++) {
		mask_y = y * mask_height / height;
		for (x = 0; x < width; x++) {
			mask_x = x * mask_width / width;
			pixels[(y * width + x) * 4 + 3] = mask_pixels[(mask_y * mask_width + mask_x) * 4];
		}
	}

	/* The mask's own pixels are not needed any more. */
	free(mask_pixels);

	/* Succeeded: the alpha is the mask's. */
	return 0;
}

/* Reads a boolean key of a stream, false when absent. */
static int
read_boolean(
	struct pdf_document *document,
	const struct pdf_object *stream,
	const char *key,
	int *value)
{
	struct pdf_object *found;
	int error;

	/* Finds the key's value. */
	*value = 0;
	error = pdf_reader_resolve_key(document, stream, key, &found);
	if (error != 0)
		return error;

	/* Only a boolean says yes. */
	if (found->type == PDF_OBJECT_BOOLEAN)
		*value = found->boolean;

	/* Succeeded: value is the key's. */
	return 0;
}

/* Converts a colour value from 0..1 to a byte, clamped and rounded. */
static unsigned char
to_byte(
	double value)
{
	/* Clamps outside the range, NaN included. */
	if (!(value > 0.0))
		return 0;
	if (value >= 1.0)
		return 255;

	/* Rounds to the nearest step. */
	return (unsigned char)(value * 255.0 + 0.5);
}
