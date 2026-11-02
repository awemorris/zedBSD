/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The alpha chunk of a lossy image (RFC 9649 2.7.1.2): its header byte,
 * the raw or lossless-compressed alpha values, and the inverse of its
 * prediction filter.
 */

#ifndef LIBWEBP_COMPAT_ALPHA_H
#define LIBWEBP_COMPAT_ALPHA_H

#include "common.h"

/* The filtering methods (RFC 9649 2.7.1.2, F). */
#define WEBPC_ALPHA_FILTER_NONE		0U
#define WEBPC_ALPHA_FILTER_HORIZONTAL	1U
#define WEBPC_ALPHA_FILTER_VERTICAL	2U
#define WEBPC_ALPHA_FILTER_GRADIENT	3U

/*
 * Decodes an ALPH chunk's payload (payload, size bytes: the header byte,
 * then the alpha bitstream; size 0 or 1 is EINVAL, as libwebp has it) into alpha (the caller's width *
 * height bytes, rows top to bottom, width a row; width and height are the
 * image's, each 1..WEBPC_VP8_SIDE_MAX).
 *
 * The header byte (most significant bits first): Rsv (2 bits, ignored),
 * pre-processing P (2 bits, informative, ignored, whatever its value: RFC
 * 9649 lets decoders ignore it; libwebp refuses Rsv not 0 and P above 1,
 * which no encoder writes), filtering method F (2
 * bits), compression method C (2 bits).  C 0: the bitstream is width *
 * height raw values (fewer bytes is EINVAL; more are ignored).  C 1: it
 * is a lossless image-stream of width by height pixels
 * (webpc_vp8l_decode_stream), whose green channel is the alpha.  C 2 or 3
 * is EINVAL.  Then the filter F is undone (webpc_alpha_unfilter).
 *
 * Returns 0 with every value written; errors of webpc_vp8l_decode_stream
 * pass through.  On failure alpha is unspecified.
 */
int webpc_alpha_decode(const uint8_t *payload, size_t size, unsigned width, unsigned height, uint8_t *alpha);

/*
 * Undoes an alpha prediction filter in place (alpha as webpc_alpha_decode
 * lays it out; method WEBPC_ALPHA_FILTER_*), in scan-line order so each
 * prediction uses values already restored (RFC 9649 2.7.1.2, Figure 11):
 * value = (predictor + stored) modulo 256 with the predictor 0 for method
 * 0, A (left) for 1, B (above) for 2, clip(A + B - C) for 3 (C above-left,
 * clip to 0..255).  Pixel (0, 0) is predicted 0.  For methods 1 and 3 the
 * rest of the left column is predicted from the value above; for methods
 * 2 and 3 the rest of the top row from the value on the left.
 */
void webpc_alpha_unfilter(uint8_t *alpha, unsigned width, unsigned height, unsigned method);

#endif
