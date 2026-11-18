/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libgif-compat's private declarations: the state behind
 * GifFileType.Private (where the bytes come from, the extensions waiting
 * for their image, and the LZW decoder's table and bits) and the
 * functions the files share.
 */

#ifndef KERN_LIBGIF_COMPAT_INTERNAL_H
#define KERN_LIBGIF_COMPAT_INTERNAL_H

#include <compat/gif_lib.h>

#include <stdint.h>

/* The LZW codes of GIF are at most 12 bits long. */
#define GIF_LZW_BITS		12
#define GIF_LZW_CODES		(1 << GIF_LZW_BITS)

/* The largest image decoded, in pixels (the raster is one byte a pixel). */
#define GIF_PIXELS_MAX		((size_t)1 << 28)

/*
 * The state of an open file (GifFileType.Private), freed by DGifCloseFile.
 *
 * The bytes come from the program's callback, or else from a file
 * descriptor (closed at the end when the library opened it).  The
 * extensions read since the last image wait in pending until an image
 * takes them.  The LZW decoder keeps, for each code, the code its string
 * extends, the byte it adds, the string's first byte and its length; and
 * the image data's current sub-block, whether the file ended inside the
 * data, and the bits not used yet (the first of them at the bottom, as
 * GIF packs them).
 */
struct gif_compat_private {
	/* The source of the bytes. */
	InputFunc read;
	int fd;
	int owns_fd;

	/* The extensions waiting for their image. */
	ExtensionBlock *pending;
	int pending_count;

	/* The LZW table. */
	uint16_t prefix[GIF_LZW_CODES];
	uint8_t suffix[GIF_LZW_CODES];
	uint8_t first[GIF_LZW_CODES];
	uint16_t length[GIF_LZW_CODES];

	/* The image data being read. */
	GifByteType block[255];
	int block_length;
	int block_position;
	int blocks_ended;
	int read_failed;
	uint32_t bits;
	int bit_count;
};

/* Reading (decode.c). */
int gif_compat_read(GifFileType *gif, GifByteType *buffer, int count);

/* The image data (lzw.c). */
int gif_compat_decode_raster(GifFileType *gif, GifByteType *pixels, size_t count);

#endif
