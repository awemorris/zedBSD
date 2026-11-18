/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libpng-compat: zedBSD's own implementation of libpng's simplified API
 * for its base programs (plan/ws035/ws.md, D1 to D6).  It holds no libpng
 * code; the names and values are libpng's.  Programs of the packages use
 * the real libpng (/usr/include/png.h); base programs include
 * <compat/png/png.h>.
 *
 * This first part reads (ws071-p010): PNG files of every colour type and
 * bit depth that are not interlaced, into 8-bit gray, gray with alpha, RGB
 * or RGBA in any channel order.  Writing (png_image_write_to_file), the
 * linear 16-bit formats, colour-mapped output and Adam7 come later
 * (ws035-p041).
 */

#ifndef KERN_COMPAT_PNG_H
#define KERN_COMPAT_PNG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The version libpng's interface is named by (D1). */
#define PNG_LIBPNG_VER_STRING	"1.6.58"
#define PNG_LIBPNG_VER		10658

/* libpng's integer and pointer types. */
typedef uint32_t png_uint_32;
typedef int32_t png_int_32;
typedef uint16_t png_uint_16;
typedef uint8_t png_byte;
typedef const void *png_const_voidp;

/* A colour of three 8-bit components (a background). */
typedef struct png_color_struct {
	png_byte red;
	png_byte green;
	png_byte blue;
} png_color;
typedef const png_color *png_const_colorp;

/* The implementation's state of an image being read. */
typedef struct png_control *png_controlp;

/*
 * An image of the simplified API: the implementation's state, the version
 * of this structure, the size and format, flags, the colour map's size,
 * whether a warning or an error happened, and its message.
 */
typedef struct {
	png_controlp opaque;
	png_uint_32 version;
	png_uint_32 width;
	png_uint_32 height;
	png_uint_32 format;
	png_uint_32 flags;
	png_uint_32 colormap_entries;
	png_uint_32 warning_or_error;
	char message[64];
} png_image;
typedef png_image *png_imagep;

/* The version of png_image. */
#define PNG_IMAGE_VERSION		1

/* warning_or_error's values. */
#define PNG_IMAGE_WARNING		1
#define PNG_IMAGE_ERROR			2
#define PNG_IMAGE_FAILED(image)		((((image).warning_or_error) & 0x03) > 1)

/* The parts a format is made of. */
#define PNG_FORMAT_FLAG_ALPHA		0x01U
#define PNG_FORMAT_FLAG_COLOR		0x02U
#define PNG_FORMAT_FLAG_LINEAR		0x04U
#define PNG_FORMAT_FLAG_COLORMAP	0x08U
#define PNG_FORMAT_FLAG_BGR		0x10U
#define PNG_FORMAT_FLAG_AFIRST		0x20U

/* The formats. */
#define PNG_FORMAT_GRAY			0
#define PNG_FORMAT_GA			PNG_FORMAT_FLAG_ALPHA
#define PNG_FORMAT_AG			(PNG_FORMAT_GA | PNG_FORMAT_FLAG_AFIRST)
#define PNG_FORMAT_RGB			PNG_FORMAT_FLAG_COLOR
#define PNG_FORMAT_BGR			(PNG_FORMAT_FLAG_COLOR | PNG_FORMAT_FLAG_BGR)
#define PNG_FORMAT_RGBA			(PNG_FORMAT_RGB | PNG_FORMAT_FLAG_ALPHA)
#define PNG_FORMAT_ARGB			(PNG_FORMAT_RGBA | PNG_FORMAT_FLAG_AFIRST)
#define PNG_FORMAT_BGRA			(PNG_FORMAT_BGR | PNG_FORMAT_FLAG_ALPHA)
#define PNG_FORMAT_ABGR			(PNG_FORMAT_BGRA | PNG_FORMAT_FLAG_AFIRST)

/* The sizes of a format's pixels and of an image's buffer. */
#define PNG_IMAGE_SAMPLE_CHANNELS(fmt)		((((fmt) & (PNG_FORMAT_FLAG_COLOR | PNG_FORMAT_FLAG_ALPHA))) + 1)
#define PNG_IMAGE_SAMPLE_COMPONENT_SIZE(fmt)	((((fmt) & PNG_FORMAT_FLAG_LINEAR) >> 2) + 1)
#define PNG_IMAGE_PIXEL_CHANNELS(fmt)		PNG_IMAGE_SAMPLE_CHANNELS(fmt)
#define PNG_IMAGE_PIXEL_COMPONENT_SIZE(fmt)	PNG_IMAGE_SAMPLE_COMPONENT_SIZE(fmt)
#define PNG_IMAGE_ROW_STRIDE(image)		(PNG_IMAGE_PIXEL_CHANNELS((image).format) * (image).width)
#define PNG_IMAGE_BUFFER_SIZE(image, row_stride) \
	(PNG_IMAGE_PIXEL_COMPONENT_SIZE((image).format) * (image).height * (row_stride))
#define PNG_IMAGE_SIZE(image)			PNG_IMAGE_BUFFER_SIZE(image, PNG_IMAGE_ROW_STRIDE(image))

/*
 * Starts reading a PNG from a file or from memory: its size and its own
 * format are filled in.  Returns non-zero on success; on failure the
 * image's message says why and nothing needs freeing.
 */
int png_image_begin_read_from_file(png_imagep image, const char *file_name);
int png_image_begin_read_from_memory(png_imagep image, png_const_voidp memory, size_t size);

/*
 * Reads the pixels into a buffer in the format set in image->format (8
 * bits a component), row_stride components a row (0: the image's width's
 * worth).  An image read into a format without alpha is laid on the
 * background (NULL: the alpha is dropped).  The image is freed.  Returns
 * non-zero on success.
 */
int png_image_finish_read(png_imagep image, png_const_colorp background, void *buffer, png_int_32 row_stride, void *colormap);

/*
 * Frees what an image read so far holds (after a failure, or instead of
 * finishing).
 */
void png_image_free(png_imagep image);

#ifdef __cplusplus
}
#endif

#endif
