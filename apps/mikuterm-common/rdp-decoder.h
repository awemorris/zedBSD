/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Pluggable H.264 decoder boundary for mikuterm's RDP client.
 */

#ifndef MIKUTERM_RDP_DECODER_H
#define MIKUTERM_RDP_DECODER_H

#include <stddef.h>
#include <stdint.h>

/*
 * Operations implemented by an H.264 Annex-B decoder backend.
 *
 * decode takes one Annex-B access unit and writes 0xAARRGGBB pixels into
 * out_pixels (out_w by out_h), reporting the stream's native dimensions
 * in decoded_w and decoded_h when a picture is produced.
 */
struct rdp_decoder_ops {
	int (*create)(void **ctx);
	void (*destroy)(void *ctx);
	int (*decode)(void *ctx, const uint8_t *data, size_t len,
		      uint32_t *out_pixels, uint32_t out_w, uint32_t out_h,
		      uint32_t *decoded_w, uint32_t *decoded_h);
};

/* Software H.264 decoder backend backed by libmedia's h264sw. */
extern const struct rdp_decoder_ops rdp_h264sw_decoder_ops;

/* H.264 decoder backend backed by libmedia (Vulkan Video with h264sw fallback). */
extern const struct rdp_decoder_ops rdp_media_decoder_ops;

#ifdef __ANDROID__
/* H.264 decoder backend backed by Android AMediaCodec (with h264sw fallback). */
extern const struct rdp_decoder_ops rdp_mediacodec_decoder_ops;
#endif

#endif
