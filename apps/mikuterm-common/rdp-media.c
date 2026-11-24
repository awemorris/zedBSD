/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * H.264 decoder backend for mikuterm's RDP client backed by libmedia
 * (Vulkan Video hardware acceleration with automatic h264sw fallback).
 */

#include "rdp-decoder.h"

#include "userland/desktop/libmedia/media-decoder.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/*
 * Per-stream libmedia decoder and scaler wrapper.
 */
struct rdp_media_ctx {
	struct media_decoder *dec;
	struct media_scaler *scaler;
};

static int media_create(void **ctx);
static void media_destroy(void *ctx);
static int media_decode(void *ctx, const uint8_t *data, size_t len, uint32_t *out_pixels, uint32_t out_w, uint32_t out_h, uint32_t *decoded_w, uint32_t *decoded_h);

/* libmedia H.264 decoder operations table. */
const struct rdp_decoder_ops rdp_media_decoder_ops = {
	media_create,
	media_destroy,
	media_decode
};

/* Creates a libmedia H.264 decoder context. */
static int
media_create(
	void **ctx)
{
	struct rdp_media_ctx *m;
	struct media_track track;
	int error;

	/* Opens an H.264 decoder via libmedia (Vulkan Video with h264sw fallback). */
	if (ctx == NULL)
		return EINVAL;
	*ctx = NULL;
	m = (struct rdp_media_ctx *)calloc(1U, sizeof(*m));
	if (m == NULL)
		return ENOMEM;
	memset(&track, 0, sizeof(track));
	track.kind = MEDIA_TRACK_VIDEO;
	track.codec = MEDIA_CODEC_H264;
	error = media_decoder_open(&track, &m->dec);
	if (error != 0) {
		free(m);
		return error;
	}

	/* Requests fast deblocking when a software backend is active. */
	(void)media_decoder_deblock(m->dec, MEDIA_DEBLOCK_FAST);
	*ctx = m;
	return 0;
}

/* Destroys a libmedia H.264 decoder context and its scaler. */
static void
media_destroy(
	void *ctx)
{
	struct rdp_media_ctx *m;

	/* Releases the scaler and closes the underlying libmedia decoder. */
	if (ctx == NULL)
		return;
	m = (struct rdp_media_ctx *)ctx;
	if (m->scaler != NULL)
		media_scaler_free(m->scaler);
	if (m->dec != NULL)
		media_decoder_close(m->dec);
	free(m);
}

/* Decodes one Annex-B access unit and scales the resulting frame to 0xAARRGGBB. */
static int
media_decode(
	void *ctx,
	const uint8_t *data,
	size_t len,
	uint32_t *out_pixels,
	uint32_t out_w,
	uint32_t out_h,
	uint32_t *decoded_w,
	uint32_t *decoded_h)
{
	struct rdp_media_ctx *m;
	struct media_packet pkt;
	struct media_frame *frame;
	struct media_frame *last;
	int64_t pts;
	int fw;
	int fh;
	int rc;

	/* Sends the Annex-B access unit into libmedia. */
	if (ctx == NULL || data == NULL || len == 0U)
		return EINVAL;
	m = (struct rdp_media_ctx *)ctx;
	memset(&pkt, 0, sizeof(pkt));
	pkt.data = data;
	pkt.size = len;
	pkt.pts_us = 0;
	pkt.keyframe = 1;
	rc = media_decoder_send(m->dec, &pkt);
	if (rc != 0)
		return rc;
	last = NULL;
	for (;;) {
		rc = media_decoder_receive(m->dec, &pts);
		if (rc < 0) {
			if (last != NULL)
				media_frame_free(&last);
			return -rc;
		}

		/* Stops draining when no more frames are ready yet. */
		if (rc == 0)
			break;
		frame = media_decoder_picture(m->dec);
		if (frame != NULL) {
			if (last != NULL)
				media_frame_free(&last);
			last = frame;
		}
	}

	/* Scales the latest decoded picture to 0xAARRGGBB if one arrived. */
	if (last == NULL)
		return EAGAIN;
	fw = 0;
	fh = 0;
	media_frame_size(last, &fw, &fh);
	if (decoded_w != NULL && fw > 0)
		*decoded_w = (uint32_t)fw;
	if (decoded_h != NULL && fh > 0)
		*decoded_h = (uint32_t)fh;
	if (out_pixels != NULL && out_w > 0U && out_h > 0U) {
		rc = media_frame_scale(last, &m->scaler, out_pixels,
				       (size_t)out_w * sizeof(uint32_t),
				       (int)out_w, (int)out_h);
		if (rc != 0) {
			media_frame_free(&last);
			return rc;
		}
	}

	/* Releases the retained frame after scaling. */
	media_frame_free(&last);
	return 0;
}
