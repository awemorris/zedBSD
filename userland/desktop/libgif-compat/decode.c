/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GIF file (GIF87a and GIF89a): the header and the logical screen
 * descriptor with its global colour map at open, then (DGifSlurp) its
 * records to the trailer: each image descriptor with its local colour map
 * and its data (lzw.c), de-interlaced into rows top to bottom, and each
 * extension, kept as giflib keeps it (a block of its first sub-block with
 * the extension's code, then one block for each further sub-block) with
 * the image after it, or with the file when no image follows.
 */

#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The record introducers. */
#define GIF_IMAGE_INTRODUCER		0x2C
#define GIF_EXTENSION_INTRODUCER	0x21
#define GIF_TRAILER			0x3B

/*
 * The Windows C runtime opens a file in text mode (turning CR LF into LF
 * and stopping at a ^Z) unless told O_BINARY; a system without the flag
 * gets 0 for it.
 */
#ifndef O_BINARY
#define O_BINARY	0
#endif

static GifFileType *gif_open(InputFunc read_function, void *user, int fd, int owns_fd, int *error);
static ColorMapObject *gif_read_color_map(GifFileType *gif, int bits, int sorted);
static int gif_read_image(GifFileType *gif);
static int gif_read_extension(GifFileType *gif);
static int gif_add_block(ExtensionBlock **blocks, int *count, int function, const GifByteType *bytes, int length);
static void gif_deinterlace(const GifByteType *rows, GifByteType *out, int width, int height);
static void gif_free_color_map(ColorMapObject *map);
static void gif_free_blocks(ExtensionBlock *blocks, int count);
static int gif_fail(GifFileType *gif, int error);

/*
 * Opens a GIF file by its name; NULL with *Error set when it cannot be
 * opened or is not a GIF file.
 */
GifFileType *
DGifOpenFileName(
	const char *GifFileName,
	int *Error)
{
	GifFileType *gif;
	int fd;

	/* The file, read only. */
	fd = open(GifFileName, O_RDONLY | O_BINARY);
	if (fd < 0) {
		if (Error != NULL)
			*Error = D_GIF_ERR_OPEN_FAILED;
		return NULL;
	}

	/* The descriptor, which DGifCloseFile closes (or the open, on failure). */
	gif = DGifOpenFileHandle(fd, Error);
	if (gif == NULL)
		return NULL;

	/* Succeeded: the file is open. */
	return gif;
}

/*
 * Opens a GIF file from a descriptor the program opened; the library
 * closes it (in DGifCloseFile, or here when the file cannot be read), as
 * giflib does.
 */
GifFileType *
DGifOpenFileHandle(
	int GifFileHandle,
	int *Error)
{
	GifFileType *gif;

	/* The descriptor, closed at the end. */
	gif = gif_open(NULL, NULL, GifFileHandle, 1, Error);
	if (gif == NULL) {
		close(GifFileHandle);
		return NULL;
	}

	/* Succeeded: the file is open. */
	return gif;
}

/*
 * Opens a GIF file whose bytes come from the program's callback;
 * userPtr is GifFileType.UserData, for the callback.
 */
GifFileType *
DGifOpen(
	void *userPtr,
	InputFunc readFunc,
	int *Error)
{
	GifFileType *gif;

	/* The callback. */
	gif = gif_open(readFunc, userPtr, -1, 0, Error);
	if (gif == NULL)
		return NULL;

	/* Succeeded: the file is open. */
	return gif;
}

/*
 * Reads every record to the trailer: the images (their rasters
 * de-interlaced) and the extensions.  GIF_ERROR leaves what was read
 * (a short image keeps the pixels decoded) with GifFile->Error set.
 */
int
DGifSlurp(
	GifFileType *GifFile)
{
	struct gif_compat_private *state;
	GifByteType introducer;
	int status;

	/* Each record by its introducer. */
	state = GifFile->Private;
	for (;;) {
		status = gif_compat_read(GifFile, &introducer, 1);
		if (status != 0)
			return gif_fail(GifFile, D_GIF_ERR_READ_FAILED);

		/* Decide by the record's kind. */
		switch (introducer) {
		case GIF_IMAGE_INTRODUCER:
			status = gif_read_image(GifFile);
			if (status != GIF_OK)
				return GIF_ERROR;
			break;
		case GIF_EXTENSION_INTRODUCER:
			status = gif_read_extension(GifFile);
			if (status != GIF_OK)
				return GIF_ERROR;
			break;
		case GIF_TRAILER:
			/* The extensions after the last image belong to the file. */
			GifFile->ExtensionBlocks = state->pending;
			GifFile->ExtensionBlockCount = state->pending_count;
			state->pending = NULL;
			state->pending_count = 0;
			return GIF_OK;
		default:
			return gif_fail(GifFile, D_GIF_ERR_WRONG_RECORD);
		}
	}
}

/*
 * Tells what the graphic control extension before an image says; the
 * defaults (no disposal, no transparent colour) and GIF_ERROR when the
 * image has none.
 */
int
DGifSavedExtensionToGCB(
	GifFileType *GifFile,
	int ImageIndex,
	GraphicsControlBlock *GCB)
{
	SavedImage *image;
	ExtensionBlock *block;
	int index;

	/* An image that was read. */
	if (ImageIndex < 0 || ImageIndex >= GifFile->ImageCount)
		return GIF_ERROR;

	/* The defaults. */
	GCB->DisposalMode = DISPOSAL_UNSPECIFIED;
	GCB->UserInputFlag = false;
	GCB->DelayTime = 0;
	GCB->TransparentColor = NO_TRANSPARENT_COLOR;

	/* The first graphic control extension of the image's: its packed flags, the delay and the colour. */
	image = &GifFile->SavedImages[ImageIndex];
	for (index = 0; index < image->ExtensionBlockCount; index++) {
		block = &image->ExtensionBlocks[index];
		if (block->Function != GRAPHICS_EXT_FUNC_CODE)
			continue;

		/* A block too short for its four bytes is refused. */
		if (block->ByteCount != 4)
			return GIF_ERROR;
		GCB->DisposalMode = (block->Bytes[0] >> 2) & 7;
		GCB->UserInputFlag = (block->Bytes[0] & 2) != 0;
		GCB->DelayTime = block->Bytes[1] | (block->Bytes[2] << 8);
		if ((block->Bytes[0] & 1) != 0)
			GCB->TransparentColor = block->Bytes[3];
		return GIF_OK;
	}

	/* The image has no graphic control extension. */
	return GIF_ERROR;
}

/*
 * Closes the file: frees the images, the colour maps and the extensions,
 * and closes the descriptor the library holds.  *ErrorCode says why a
 * close failed.
 */
int
DGifCloseFile(
	GifFileType *GifFile,
	int *ErrorCode)
{
	struct gif_compat_private *state;
	SavedImage *image;
	int index;
	int closed;

	/* Nothing to close. */
	if (GifFile == NULL)
		return GIF_ERROR;

	/* Each image's colour map, raster and extensions. */
	for (index = 0; index < GifFile->ImageCount; index++) {
		image = &GifFile->SavedImages[index];
		gif_free_color_map(image->ImageDesc.ColorMap);
		free(image->RasterBits);
		gif_free_blocks(image->ExtensionBlocks, image->ExtensionBlockCount);
	}

	/* The file's own parts. */
	free(GifFile->SavedImages);
	gif_free_color_map(GifFile->SColorMap);
	gif_free_blocks(GifFile->ExtensionBlocks, GifFile->ExtensionBlockCount);

	/* The state, and the descriptor the library holds. */
	state = GifFile->Private;
	closed = 0;
	if (state != NULL) {
		gif_free_blocks(state->pending, state->pending_count);
		if (state->owns_fd)
			closed = close(state->fd);
		free(state);
	}

	/* The object itself. */
	free(GifFile);

	/* A descriptor that would not close. */
	if (closed != 0) {
		if (ErrorCode != NULL)
			*ErrorCode = D_GIF_ERR_CLOSE_FAILED;
		return GIF_ERROR;
	}

	/* Succeeded: everything is freed. */
	if (ErrorCode != NULL)
		*ErrorCode = D_GIF_SUCCEEDED;
	return GIF_OK;
}

/*
 * Describes an error code (NULL for a code that is not one).
 */
const char *
GifErrorString(
	int ErrorCode)
{
	/* Decide by the code. */
	switch (ErrorCode) {
	case D_GIF_ERR_OPEN_FAILED:
		return "Failed to open given file";
	case D_GIF_ERR_READ_FAILED:
		return "Failed to read from given file";
	case D_GIF_ERR_NOT_GIF_FILE:
		return "Data is not in GIF format";
	case D_GIF_ERR_NO_SCRN_DSCR:
		return "No screen descriptor detected";
	case D_GIF_ERR_NO_IMAG_DSCR:
		return "No image descriptor detected";
	case D_GIF_ERR_NO_COLOR_MAP:
		return "Neither global nor local color map";
	case D_GIF_ERR_WRONG_RECORD:
		return "Wrong record type detected";
	case D_GIF_ERR_DATA_TOO_BIG:
		return "Number of pixels bigger than width * height";
	case D_GIF_ERR_NOT_ENOUGH_MEM:
		return "Failed to allocate required memory";
	case D_GIF_ERR_CLOSE_FAILED:
		return "Failed to close given file";
	case D_GIF_ERR_NOT_READABLE:
		return "Given file was not opened for read";
	case D_GIF_ERR_IMAGE_DEFECT:
		return "Image is defective, decoding aborted";
	case D_GIF_ERR_EOF_TOO_SOON:
		return "Image EOF detected before image complete";
	default:
		break;
	}

	/* Not an error code of the library. */
	return NULL;
}

/*
 * Reads exactly count bytes from the file's source; nonzero (with
 * GifFile->Error set) when fewer come.
 */
int
gif_compat_read(
	GifFileType *gif,
	GifByteType *buffer,
	int count)
{
	struct gif_compat_private *state;
	ssize_t got;
	int done;

	/* The bytes, as many calls as the source needs; nothing more ends the read. */
	state = gif->Private;
	done = 0;
	while (done < count) {
		if (state->read != NULL) {
			got = state->read(gif, buffer + done, count - done);
		} else {
			got = read(state->fd, buffer + done, (size_t)(count - done));
			if (got < 0 && errno == EINTR)
				continue;
		}

		/* The file ended or failed. */
		if (got <= 0) {
			gif->Error = D_GIF_ERR_READ_FAILED;
			return 1;
		}

		/* Some bytes more. */
		done += (int)got;
	}

	/* Succeeded: every byte is read. */
	return 0;
}

/*
 * Makes the file's object and reads the header, the logical screen
 * descriptor and the global colour map.
 */
static GifFileType *
gif_open(
	InputFunc read_function,
	void *user,
	int fd,
	int owns_fd,
	int *error)
{
	GifFileType *gif;
	struct gif_compat_private *state;
	GifByteType header[6];
	GifByteType screen[7];
	int status;
	int differs;

	/* The object. */
	gif = calloc(1, sizeof(*gif));
	if (gif == NULL) {
		if (error != NULL)
			*error = D_GIF_ERR_NOT_ENOUGH_MEM;
		return NULL;
	}

	/* Its state: where the bytes come from. */
	state = calloc(1, sizeof(*state));
	if (state == NULL) {
		free(gif);
		if (error != NULL)
			*error = D_GIF_ERR_NOT_ENOUGH_MEM;
		return NULL;
	}

	/* Where the bytes come from. */
	state->read = read_function;
	state->fd = fd;
	state->owns_fd = 0;
	gif->Private = state;
	gif->UserData = user;

	/* "GIF" and a version. */
	status = gif_compat_read(gif, header, sizeof(header));
	differs = 1;
	if (status == 0)
		differs = memcmp(header, "GIF", 3);
	if (differs != 0) {
		if (error != NULL)
			*error = D_GIF_ERR_NOT_GIF_FILE;
		DGifCloseFile(gif, NULL);
		return NULL;
	}

	/* The screen: its size, its flags, the background colour and the aspect. */
	status = gif_compat_read(gif, screen, sizeof(screen));
	if (status != 0) {
		if (error != NULL)
			*error = D_GIF_ERR_NO_SCRN_DSCR;
		DGifCloseFile(gif, NULL);
		return NULL;
	}

	/* The screen's fields. */
	gif->SWidth = screen[0] | (screen[1] << 8);
	gif->SHeight = screen[2] | (screen[3] << 8);
	gif->SColorResolution = ((screen[4] >> 4) & 7) + 1;
	gif->SBackGroundColor = screen[5];
	gif->AspectByte = screen[6];

	/* The global colour map, when the flags say there is one. */
	if ((screen[4] & 0x80) != 0) {
		gif->SColorMap = gif_read_color_map(gif, (screen[4] & 7) + 1, (screen[4] & 0x08) != 0);
		if (gif->SColorMap == NULL) {
			if (error != NULL)
				*error = gif->Error;
			DGifCloseFile(gif, NULL);
			return NULL;
		}
	}

	/* Succeeded: the file is open, and its descriptor is the library's from now on. */
	state->owns_fd = owns_fd;
	if (error != NULL)
		*error = D_GIF_SUCCEEDED;
	return gif;
}

/* Reads a colour map of 2^bits colours; NULL (with the error set) on failure. */
static ColorMapObject *
gif_read_color_map(
	GifFileType *gif,
	int bits,
	int sorted)
{
	ColorMapObject *map;
	GifByteType rgb[3];
	int status;
	int index;

	/* The map. */
	map = calloc(1, sizeof(*map));
	if (map == NULL) {
		gif->Error = D_GIF_ERR_NOT_ENOUGH_MEM;
		return NULL;
	}

	/* Its size. */
	map->BitsPerPixel = bits;
	map->ColorCount = 1 << bits;
	map->SortFlag = sorted != 0;

	/* Its colours. */
	map->Colors = calloc((size_t)map->ColorCount, sizeof(*map->Colors));
	if (map->Colors == NULL) {
		free(map);
		gif->Error = D_GIF_ERR_NOT_ENOUGH_MEM;
		return NULL;
	}

	/* Each colour's red, green and blue. */
	for (index = 0; index < map->ColorCount; index++) {
		status = gif_compat_read(gif, rgb, 3);
		if (status != 0) {
			gif_free_color_map(map);
			gif->Error = D_GIF_ERR_READ_FAILED;
			return NULL;
		}

		/* The colour. */
		map->Colors[index].Red = rgb[0];
		map->Colors[index].Green = rgb[1];
		map->Colors[index].Blue = rgb[2];
	}

	/* Succeeded: the map. */
	return map;
}

/*
 * Reads an image: its descriptor and local colour map, then its data into
 * a new saved image that takes the extensions read since the last one.
 */
static int
gif_read_image(
	GifFileType *gif)
{
	struct gif_compat_private *state;
	GifByteType descriptor[9];
	GifImageDesc desc;
	SavedImage *images;
	SavedImage *image;
	GifByteType *rows;
	size_t pixels;
	int status;
	int error;

	/* The descriptor: position, size and flags. */
	state = gif->Private;
	status = gif_compat_read(gif, descriptor, sizeof(descriptor));
	if (status != 0)
		return gif_fail(gif, D_GIF_ERR_READ_FAILED);
	memset(&desc, 0, sizeof(desc));
	desc.Left = descriptor[0] | (descriptor[1] << 8);
	desc.Top = descriptor[2] | (descriptor[3] << 8);
	desc.Width = descriptor[4] | (descriptor[5] << 8);
	desc.Height = descriptor[6] | (descriptor[7] << 8);
	desc.Interlace = (descriptor[8] & 0x40) != 0;

	/* An image with no pixels, or more than the library decodes, is defective. */
	if (desc.Width <= 0 || desc.Height <= 0)
		return gif_fail(gif, D_GIF_ERR_IMAGE_DEFECT);
	pixels = (size_t)desc.Width * (size_t)desc.Height;
	if (pixels > GIF_PIXELS_MAX)
		return gif_fail(gif, D_GIF_ERR_IMAGE_DEFECT);

	/* The local colour map, when the flags say there is one. */
	if ((descriptor[8] & 0x80) != 0) {
		desc.ColorMap = gif_read_color_map(gif, (descriptor[8] & 7) + 1, (descriptor[8] & 0x20) != 0);
		if (desc.ColorMap == NULL)
			return GIF_ERROR;
	}

	/* One saved image more. */
	images = realloc(gif->SavedImages, sizeof(*images) * (size_t)(gif->ImageCount + 1));
	if (images == NULL) {
		gif_free_color_map(desc.ColorMap);
		return gif_fail(gif, D_GIF_ERR_NOT_ENOUGH_MEM);
	}

	/* The list holds one image more; the new one starts empty. */
	gif->SavedImages = images;
	image = &images[gif->ImageCount];
	memset(image, 0, sizeof(*image));
	image->ImageDesc = desc;
	gif->Image = desc;
	gif->ImageCount++;

	/* It takes the extensions read since the last image. */
	image->ExtensionBlocks = state->pending;
	image->ExtensionBlockCount = state->pending_count;
	state->pending = NULL;
	state->pending_count = 0;

	/* Its pixels, starting at index 0. */
	image->RasterBits = calloc(pixels, 1);
	if (image->RasterBits == NULL)
		return gif_fail(gif, D_GIF_ERR_NOT_ENOUGH_MEM);

	/* The data, straight into the raster, or in interlaced order and then put in place. */
	if (!desc.Interlace) {
		error = gif_compat_decode_raster(gif, image->RasterBits, pixels);
	} else {
		/* The rows as the passes give them. */
		rows = calloc(pixels, 1);
		if (rows == NULL)
			return gif_fail(gif, D_GIF_ERR_NOT_ENOUGH_MEM);
		error = gif_compat_decode_raster(gif, rows, pixels);
		gif_deinterlace(rows, image->RasterBits, desc.Width, desc.Height);
		free(rows);
	}

	/* Bad or short data: the pixels decoded stay, and the error is reported. */
	if (error != D_GIF_SUCCEEDED)
		return gif_fail(gif, error);

	/* Succeeded: the image is read. */
	return GIF_OK;
}

/*
 * Reads an extension: its code, then its sub-blocks, which wait in the
 * state for the next image.  An extension whose first sub-block is empty
 * leaves nothing (as in giflib).
 */
static int
gif_read_extension(
	GifFileType *gif)
{
	struct gif_compat_private *state;
	GifByteType bytes[255];
	GifByteType code;
	GifByteType length;
	int function;
	int status;

	/* The extension's code. */
	state = gif->Private;
	status = gif_compat_read(gif, &code, 1);
	if (status != 0)
		return gif_fail(gif, D_GIF_ERR_READ_FAILED);

	/* Each sub-block to the empty one: the first under the code, the rest as continuations. */
	function = code;
	for (;;) {
		status = gif_compat_read(gif, &length, 1);
		if (status != 0)
			return gif_fail(gif, D_GIF_ERR_READ_FAILED);
		if (length == 0)
			break;

		/* The sub-block's bytes, kept. */
		status = gif_compat_read(gif, bytes, length);
		if (status != 0)
			return gif_fail(gif, D_GIF_ERR_READ_FAILED);
		status = gif_add_block(&state->pending, &state->pending_count, function, bytes, length);
		if (status != 0)
			return gif_fail(gif, D_GIF_ERR_NOT_ENOUGH_MEM);

		/* The sub-blocks after the first continue it. */
		function = CONTINUE_EXT_FUNC_CODE;
	}

	/* Succeeded: the extension is read. */
	return GIF_OK;
}

/* Adds a copy of some bytes to a list of extension blocks; nonzero when memory runs out. */
static int
gif_add_block(
	ExtensionBlock **blocks,
	int *count,
	int function,
	const GifByteType *bytes,
	int length)
{
	ExtensionBlock *grown;
	ExtensionBlock *block;

	/* One block more. */
	grown = realloc(*blocks, sizeof(*grown) * (size_t)(*count + 1));
	if (grown == NULL)
		return 1;
	*blocks = grown;

	/* Its copy of the bytes. */
	block = &grown[*count];
	block->Bytes = malloc((size_t)length);
	if (block->Bytes == NULL)
		return 1;
	memcpy(block->Bytes, bytes, (size_t)length);
	block->ByteCount = length;
	block->Function = function;

	/* Succeeded: the list holds the block. */
	(*count)++;
	return 0;
}

/*
 * Puts the rows of an interlaced image in place: the file gives every
 * 8th row from 0, then every 8th from 4, every 4th from 2, and every
 * 2nd from 1.
 */
static void
gif_deinterlace(
	const GifByteType *rows,
	GifByteType *out,
	int width,
	int height)
{
	static const int starts[4] = { 0, 4, 2, 1 };
	static const int steps[4] = { 8, 8, 4, 2 };
	int pass;
	int row;
	int from;

	/* Each pass's rows, in the order they came. */
	from = 0;
	for (pass = 0; pass < 4; pass++) {
		for (row = starts[pass]; row < height; row += steps[pass]) {
			memcpy(out + (size_t)row * (size_t)width, rows + (size_t)from * (size_t)width, (size_t)width);
			from++;
		}
	}
}

/* Frees a colour map (NULL is none). */
static void
gif_free_color_map(
	ColorMapObject *map)
{
	/* No map. */
	if (map == NULL)
		return;

	/* The colours, then the map. */
	free(map->Colors);
	free(map);
}

/* Frees a list of extension blocks and their bytes. */
static void
gif_free_blocks(
	ExtensionBlock *blocks,
	int count)
{
	int index;

	/* Each block's bytes, then the list. */
	for (index = 0; index < count; index++)
		free(blocks[index].Bytes);
	free(blocks);
}

/* Records an error in the file and reports GIF_ERROR. */
static int
gif_fail(
	GifFileType *gif,
	int error)
{
	/* The error the program reads. */
	gif->Error = error;
	return GIF_ERROR;
}
