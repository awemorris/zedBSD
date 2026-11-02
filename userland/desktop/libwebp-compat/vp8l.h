/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The lossless decoder (RFC 9649 section 3): the header, the transforms,
 * the ARGB image, and the inverse transforms in reverse order; also the
 * header-less image stream an ALPH chunk holds.
 */

#ifndef LIBWEBP_COMPAT_VP8L_H
#define LIBWEBP_COMPAT_VP8L_H

#include "common.h"

/* The signature byte of a lossless stream (RFC 9649 3.4). */
#define WEBPC_VP8L_SIGNATURE		0x2fU

/* The bytes of the header before the transforms: signature, sizes, alpha, version (RFC 9649 3.4). */
#define WEBPC_VP8L_HEADER_SIZE		5U

/* What a lossless stream's header says: the size and the alpha_is_used hint. */
struct webpc_vp8l_header {
	unsigned width;
	unsigned height;
	int alpha_is_used;
};

/*
 * Reads the header of a lossless stream (data from its signature byte,
 * size bytes): signature 0x2f, width and height (14 bits each, plus 1),
 * alpha_is_used (1 bit), version_number (3 bits, which must be 0).
 * Returns 0 and fills header; ENOBUFS for fewer than 5 bytes; EINVAL for
 * another signature or version.
 */
int webpc_vp8l_read_header(const uint8_t *data, size_t size, struct webpc_vp8l_header *header);

/*
 * Decodes a whole lossless stream (data from its signature byte, size
 * bytes) into a new array of width * height ARGB pixels (as
 * webpc_vp8l_decode_image lays them out), which *pixels receives and the
 * caller frees with free().  The image-stream (RFC 9649 3.8.1) is decoded
 * as webpc_vp8l_decode_stream decodes it, with the header's size.
 * Returns 0 and sets *pixels, *width and *height; on failure *pixels is
 * NULL and nothing is left allocated.  Errors: those of
 * webpc_vp8l_read_header and webpc_vp8l_decode_stream, and ENOMEM.
 */
int webpc_vp8l_decode(const uint8_t *data, size_t size, uint32_t **pixels, unsigned *width, unsigned *height);

/*
 * Decodes an image-stream without header (RFC 9649 3.8.1: optional
 * transforms then the spatially coded image; an ALPH chunk's compressed
 * data, RFC 9649 2.7.1.2) of width by height pixels (each 1 or more, at
 * most WEBPC_VP8L_SIDE_MAX) from data (size bytes) into pixels (the
 * caller's width * height ARGB values).
 *
 * The transforms (3.5, 3.8.2) are read while their 1 bit is set: a
 * second transform of one type is EINVAL.  Predictor and colour: size_bits
 * = 2 + (3 bits) and the subresolution image of
 * webpc_vp8l_subsample(current width, size_bits) by
 * webpc_vp8l_subsample(height, size_bits) pixels.  Subtract green: no
 * data.  Colour indexing: color_table_size = 1 + (8 bits), the table as an
 * image of color_table_size by 1 pixels whose entries are then summed per
 * channel modulo 256 (each the previous plus its delta), padded with
 * 0x00000000 to WEBPC_VP8L_COLOR_TABLE_SIZE; width_bits from Table 3 (3
 * for 1..2 colours, 2 for 3..4, 1 for 5..16, 0 above); the current width
 * becomes webpc_vp8l_subsample(current width, width_bits) for what
 * follows.  The subresolution images are decoded by
 * webpc_vp8l_decode_image with argb_role 0; the ARGB image, at the current
 * width, with argb_role 1.  The transforms are then undone in the reverse
 * of the order they were read, each at the width it was read at.
 *
 * Every table limit is WEBPC_VP8L_TABLE_ENTRIES_MAX.  The data running
 * out is ENOBUFS.  Returns 0 with every pixel written; on failure the
 * pixels are unspecified.  Nothing stays allocated.
 */
int webpc_vp8l_decode_stream(const uint8_t *data, size_t size, unsigned width, unsigned height, uint32_t *pixels);

#endif
