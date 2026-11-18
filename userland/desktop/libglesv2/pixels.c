/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The application's pixels of zedBSD's OpenGL ES (WS068 p028): where the
 * texels a texture call reads and the pixels glReadPixels writes are, by
 * OpenGL ES 3's pixel store (row length, skipped pixels, rows and images,
 * image height) and its pixel buffers.
 *
 * With a buffer bound to GL_PIXEL_UNPACK_BUFFER a texture call's pointer
 * is an offset into the buffer's bytes; with one bound to
 * GL_PIXEL_PACK_BUFFER glReadPixels writes into the buffer's bytes, and
 * the device copy becomes stale as after glBufferSubData.  Texels laid
 * out other than tightly (at the unpack alignment) are gathered into
 * tight rows here, so the conversions of format.c and texture.c read one
 * layout only.
 */

#include "gles.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * Where one image's rows of pixels lie in memory: the byte offset of the
 * first pixel read, the bytes between rows and between images, and the
 * bytes one pixel takes.
 */
struct pixels_layout {
	size_t start;
	size_t row_stride;
	size_t image_stride;
	size_t pixel;
};

static size_t pixels_type_size(GLenum type);
static size_t pixels_align(size_t bytes, GLint alignment);
static size_t pixels_extent(const struct pixels_layout *layout, GLsizei width, GLsizei height, GLsizei depth);

/*
 * Finds the texels a texture call reads (depth 0 for a 2D call, which
 * ignores the image height and the images skipped): in the pixel unpack
 * buffer when one is bound (the pointer is an offset into it), else at
 * the pointer.  *packed receives the texels as tight rows at the unpack
 * alignment (NULL when there are none: the level is zeros), gathered into
 * *owned (which the caller frees) when the pixel store lays them out
 * otherwise.  Returns GL_NO_ERROR, or GL_INVALID_OPERATION for a mapped
 * buffer or texels beyond its end, or GL_OUT_OF_MEMORY.
 */
GLenum
gles_unpack(
	struct gles_state *state,
	GLenum format,
	GLenum type,
	GLsizei width,
	GLsizei height,
	GLsizei depth,
	const void *pixels,
	const void **packed,
	unsigned char **owned)
{
	struct pixels_layout layout;
	struct gles_buffer *buffer;
	const unsigned char *base;
	unsigned char *gathered;
	size_t offset;
	size_t extent;
	size_t tight;
	size_t type_size;
	GLsizei images;
	GLsizei image;
	GLsizei row;
	GLint row_length;
	GLint image_height;
	int plain;

	/* Nothing gathered yet. */
	*packed = pixels;
	*owned = NULL;

	/* A 2D call reads one image. */
	images = depth;
	if (images < 1)
		images = 1;

	/* The bytes of one pixel; an unknown format or type is left to the conversion to refuse. */
	layout.pixel = gles_pixel_size(format, type);
	if (layout.pixel == 0U)
		return GL_NO_ERROR;

	/* The rows' length and the images' height, the pixel store's when it gives them. */
	row_length = width;
	if (state->unpack_row_length > 0)
		row_length = state->unpack_row_length;
	image_height = height;
	if (depth > 0 && state->unpack_image_height > 0)
		image_height = state->unpack_image_height;

	/* Where the first pixel is, and the strides of rows and images. */
	layout.row_stride = pixels_align((size_t)row_length * layout.pixel, state->unpack_alignment);
	layout.image_stride = (size_t)image_height * layout.row_stride;
	layout.start = (size_t)state->unpack_skip_rows * layout.row_stride + (size_t)state->unpack_skip_pixels * layout.pixel;
	if (depth > 0)
		layout.start += (size_t)state->unpack_skip_images * layout.image_stride;
	extent = pixels_extent(&layout, width, height, images);

	/* The texels in the unpack buffer: a buffer not mapped, an offset of whole components, and texels inside it. */
	base = pixels;
	buffer = state->pixel_unpack_buffer;
	if (buffer != NULL) {
		if (buffer->map_active)
			return GL_INVALID_OPERATION;
		offset = (size_t)(uintptr_t)pixels;
		type_size = pixels_type_size(type);
		if (type_size != 0U && offset % type_size != 0U)
			return GL_INVALID_OPERATION;
		if (offset > buffer->size || extent > buffer->size - offset)
			return GL_INVALID_OPERATION;
		base = buffer->data + offset;
	}

	/* No texels: the level is zeros. */
	if (base == NULL) {
		*packed = NULL;
		return GL_NO_ERROR;
	}

	/* Tight rows already: nothing skipped, rows and images of the size given. */
	plain = 1;
	if (layout.start != 0U)
		plain = 0;
	else if (row_length != width)
		plain = 0;
	else if (image_height != height)
		plain = 0;
	if (plain) {
		*packed = base;
		return GL_NO_ERROR;
	}

	/* Tight rows at the alignment, gathered row by row. */
	tight = pixels_align((size_t)width * layout.pixel, state->unpack_alignment);
	gathered = malloc(tight * (size_t)height * (size_t)images + 1U);
	if (gathered == NULL)
		return GL_OUT_OF_MEMORY;
	for (image = 0; image < images; image++) {
		for (row = 0; row < height; row++) {
			memcpy(gathered + ((size_t)image * (size_t)height + (size_t)row) * tight,
			       base + layout.start + (size_t)image * layout.image_stride + (size_t)row * layout.row_stride,
			       (size_t)width * layout.pixel);
		}
	}

	/* Succeeded: the gathered rows, which the caller frees. */
	*packed = gathered;
	*owned = gathered;
	return GL_NO_ERROR;
}

/*
 * Finds where glReadPixels writes a rectangle of pixels of a format and
 * type: in the pixel pack buffer when one is bound (the pointer is an
 * offset into it; its device copy becomes stale), else at the pointer.
 * *base receives where the first row goes and *stride the bytes between
 * rows, by the pack alignment and the pixel store.  Returns GL_NO_ERROR,
 * or GL_INVALID_OPERATION for a mapped buffer or pixels beyond its end.
 */
GLenum
gles_pack_target(
	struct gles_state *state,
	GLenum format,
	GLenum type,
	GLsizei width,
	GLsizei height,
	void *pixels,
	unsigned char **base,
	size_t *stride)
{
	struct pixels_layout layout;
	struct gles_buffer *buffer;
	size_t offset;
	size_t extent;
	size_t type_size;
	GLint row_length;
	int status;

	/* The bytes of one pixel (the caller checked the format and type). */
	layout.pixel = gles_pixel_size(format, type);

	/* The rows' length, the pixel store's when it gives one, and where the first pixel goes. */
	row_length = width;
	if (state->pack_row_length > 0)
		row_length = state->pack_row_length;
	layout.row_stride = pixels_align((size_t)row_length * layout.pixel, state->pack_alignment);
	layout.image_stride = 0U;
	layout.start = (size_t)state->pack_skip_rows * layout.row_stride + (size_t)state->pack_skip_pixels * layout.pixel;
	*stride = layout.row_stride;

	/* Into the application's memory when no pack buffer is bound. */
	buffer = state->pixel_pack_buffer;
	if (buffer == NULL) {
		*base = (unsigned char *)pixels + layout.start;
		return GL_NO_ERROR;
	}

	/* Into the pack buffer: one not mapped, an offset of whole components, and pixels inside it. */
	if (buffer->map_active)
		return GL_INVALID_OPERATION;
	offset = (size_t)(uintptr_t)pixels;
	type_size = pixels_type_size(type);
	if (type_size != 0U && offset % type_size != 0U)
		return GL_INVALID_OPERATION;
	extent = pixels_extent(&layout, width, height, 1);
	if (offset > buffer->size || extent > buffer->size - offset)
		return GL_INVALID_OPERATION;

	/* Bytes in a device copy this frame reads move to the CPU first (ws101-p017). */
	status = gles_buffer_writable(state, buffer);
	if (status != 0)
		return GL_OUT_OF_MEMORY;

	/* The buffer's bytes change: its device copy is stale. */
	buffer->dirty = 1;

	/* Succeeded: the rows go into the buffer's bytes. */
	*base = buffer->data + offset + layout.start;
	return GL_NO_ERROR;
}

/* Returns the bytes of a type's unit (a component, or a whole packed pixel), 0 for a name that is not a type. */
static size_t
pixels_type_size(
	GLenum type)
{
	/* The component types and the packed ones. */
	switch (type) {
	case GL_UNSIGNED_BYTE:
	case GL_BYTE:
		return 1U;
	case GL_UNSIGNED_SHORT:
	case GL_SHORT:
	case GL_HALF_FLOAT:
	case GL_UNSIGNED_SHORT_5_6_5:
	case GL_UNSIGNED_SHORT_4_4_4_4:
	case GL_UNSIGNED_SHORT_5_5_5_1:
		return 2U;
	case GL_UNSIGNED_INT:
	case GL_INT:
	case GL_FLOAT:
	case GL_UNSIGNED_INT_2_10_10_10_REV:
	case GL_UNSIGNED_INT_10F_11F_11F_REV:
	case GL_UNSIGNED_INT_5_9_9_9_REV:
	case GL_UNSIGNED_INT_24_8:
	case GL_FLOAT_32_UNSIGNED_INT_24_8_REV:
		return 4U;
	default:
		break;
	}

	/* Not a type. */
	return 0U;
}

/* Returns a row's bytes rounded up to an alignment (1, 2, 4 or 8). */
static size_t
pixels_align(
	size_t bytes,
	GLint alignment)
{
	size_t unit;

	/* The alignment as a size (glPixelStorei allows no other). */
	unit = (size_t)alignment;
	if (unit == 0U)
		unit = 1U;

	/* Succeeded: the next multiple of it. */
	return (bytes + unit - 1U) / unit * unit;
}

/* Returns the bytes from the start of the pixels to the end of the last one read or written (0 for an empty rectangle). */
static size_t
pixels_extent(
	const struct pixels_layout *layout,
	GLsizei width,
	GLsizei height,
	GLsizei depth)
{
	size_t extent;

	/* An empty rectangle touches nothing. */
	if (width <= 0 || height <= 0 || depth <= 0)
		return 0U;

	/* The last image's last row's last pixel. */
	extent = layout->start;
	extent += (size_t)(depth - 1) * layout->image_stride;
	extent += (size_t)(height - 1) * layout->row_stride;
	extent += (size_t)width * layout->pixel;

	/* Succeeded: the extent. */
	return extent;
}
