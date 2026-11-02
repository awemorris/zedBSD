/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The lossy decoder: one VP8 key frame (RFC 6386) into its Y'CbCr 4:2:0
 * planes, reconstructed macroblock by macroblock and then loop-filtered.
 */

#ifndef LIBWEBP_COMPAT_VP8_H
#define LIBWEBP_COMPAT_VP8_H

#include "common.h"

/*
 * A decoded frame.  The planes cover whole macroblocks (mb_width * 16 by
 * mb_height * 16 luma, half of that each way for chroma), of which the
 * image is the top-left width by height (chroma (width + 1) / 2 by
 * (height + 1) / 2).  y, u and v point at the first image pixel of each
 * plane, y_stride and uv_stride bytes a row (each at least the plane's
 * macroblock width).  memory is the one allocation all three are in,
 * owned by the picture; webpc_vp8_picture_release frees it.
 */
struct webpc_vp8_picture {
	uint8_t *y;
	uint8_t *u;
	uint8_t *v;
	ptrdiff_t y_stride;
	ptrdiff_t uv_stride;
	unsigned width;
	unsigned height;
	unsigned mb_width;
	unsigned mb_height;
	void *memory;
};

/*
 * Decodes a key frame (data from its frame tag, size bytes) into a new
 * picture.
 *
 * The frame tag and header are read (webpc_vp8_read_tag,
 * webpc_vp8_read_header).  Then, for each macroblock in raster order: its
 * header from the first partition (webpc_vp8_read_modes); its residue
 * from token partition mb_y % partition_count (webpc_vp8_read_residue, or
 * webpc_vp8_skip_residue when skip is set, which codes nothing); for a
 * macroblock with Y2, the WHT into the Y blocks' DC (webpc_vp8_iwht); the
 * prediction of 16x16 luma or of each 4x4 subblock in raster order, each
 * followed by its inverse DCT (webpc_vp8_predict_block,
 * webpc_vp8_predict_subblock, webpc_vp8_idct_add), and of each chroma
 * plane with its four blocks.  The edges come from the unfiltered
 * reconstruction (RFC 6386 12, 15.1) with the out-of-frame values of
 * vp8-predict.h; contexts start as the frame's edges give them.  A block
 * whose 16 coefficients are all 0 (after the WHT has put the DC of a
 * macroblock with Y2) may skip its inverse DCT (it adds nothing); the
 * residue's coded bits are not that test.
 *
 * After every macroblock is reconstructed, the loop filter runs over all
 * of them in raster order (webpc_vp8_filter_params,
 * webpc_vp8_filter_macroblock), unless bypass_filtering is 1 or the
 * header's loop_filter_level is 0 (then nothing is filtered, whatever the
 * segments say; RFC 6386 15).
 *
 * Any partition read past its end (webpc_bool_ended, checked at least
 * after each macroblock's tokens and after each row of headers) is
 * ENOBUFS.  Other errors are those of the header.  Returns 0 and fills
 * picture; on failure nothing is left allocated and picture is cleared.
 */
int webpc_vp8_decode(const uint8_t *data, size_t size, int bypass_filtering, struct webpc_vp8_picture *picture);

/* Frees a picture's planes and clears it; a cleared picture is ignored. */
void webpc_vp8_picture_release(struct webpc_vp8_picture *picture);

#endif
