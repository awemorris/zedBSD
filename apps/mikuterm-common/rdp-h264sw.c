/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Software H.264 decoder backend for mikuterm's RDP client using libmedia's
 * h264sw decoder directly without Vulkan dependencies.
 */

#include "rdp-decoder.h"

#include "userland/desktop/libmedia/h264sw-decoder.h"
#include "userland/desktop/libmedia/vcodec-nal.h"

#include <errno.h>
#include <stdlib.h>

/*
 * Per-stream h264sw decoder wrapper.
 */
struct rdp_h264sw_ctx {
	struct h264sw_decoder *dec;
};

static int h264sw_create(void **ctx);
static void h264sw_destroy(void *ctx);
static int h264sw_decode(void *ctx, const uint8_t *data, size_t len, uint32_t *out_pixels, uint32_t out_w, uint32_t out_h, uint32_t *decoded_w, uint32_t *decoded_h);
static uint8_t clamp_u8(int32_t v);
static void yuv420_to_argb(const struct h264sw_frame *f, uint32_t *out_pixels, uint32_t out_w, uint32_t out_h);

/* Software H.264 decoder operations table. */
const struct rdp_decoder_ops rdp_h264sw_decoder_ops = {
	h264sw_create,
	h264sw_destroy,
	h264sw_decode
};

/* Creates an h264sw decoder context. */
static int
h264sw_create(
	void **ctx)
{
	struct rdp_h264sw_ctx *h;
	int error;

	if (ctx == NULL)
		return EINVAL;

	h = (struct rdp_h264sw_ctx *)calloc(1U, sizeof(*h));
	if (h == NULL)
		return ENOMEM;

	error = h264sw_decoder_create(&h->dec, 1U, NULL);
	if (error != 0) {
		free(h);
		return error;
	}

	h264sw_decoder_deblock(h->dec, H264SW_DEBLOCK_FAST);
	*ctx = h;
	return 0;
}

/* Destroys an h264sw decoder context. */
static void
h264sw_destroy(
	void *ctx)
{
	struct rdp_h264sw_ctx *h;

	if (ctx == NULL)
		return;

	h = (struct rdp_h264sw_ctx *)ctx;
	if (h->dec != NULL)
		h264sw_decoder_destroy(h->dec);

	free(h);
}

/* Decodes one Annex-B access unit and converts the resulting frame to ARGB. */
static int
h264sw_decode(
	void *ctx,
	const uint8_t *data,
	size_t len,
	uint32_t *out_pixels,
	uint32_t out_w,
	uint32_t out_h,
	uint32_t *decoded_w,
	uint32_t *decoded_h)
{
	struct rdp_h264sw_ctx *h;
	size_t cursor;
	const uint8_t *nal;
	size_t nal_len;
	struct h264sw_frame *frame;
	struct h264sw_frame *last;
	int error;

	if (ctx == NULL || data == NULL || len == 0U)
		return EINVAL;

	h = (struct rdp_h264sw_ctx *)ctx;
	cursor = 0U;
	last = NULL;

	/* Feeds each Annex-B NAL unit and drains any output frames. */
	while (vcodec_nal_find(data, len, &cursor, &nal, &nal_len) != 0) {
		error = h264sw_decoder_nal(h->dec, nal, nal_len, 0);
		if (error != 0 && error != EINVAL)
			return error;

		while ((frame = h264sw_decoder_output(h->dec)) != NULL) {
			if (last != NULL)
				h264sw_frame_unref(last);

			last = frame;
		}
	}

	/* Flushes the current picture so low-latency RDP frames appear at once. */
	error = h264sw_decoder_flush(h->dec);
	if (error != 0 && error != EINVAL) {
		if (last != NULL)
			h264sw_frame_unref(last);

		return error;
	}

	while ((frame = h264sw_decoder_output(h->dec)) != NULL) {
		if (last != NULL)
			h264sw_frame_unref(last);

		last = frame;
	}

	if (last == NULL)
		return EAGAIN;

	if (decoded_w != NULL)
		*decoded_w = last->crop_width > 0U ? last->crop_width : last->width;
	if (decoded_h != NULL)
		*decoded_h = last->crop_height > 0U ? last->crop_height : last->height;

	if (out_pixels != NULL && out_w > 0U && out_h > 0U)
		yuv420_to_argb(last, out_pixels, out_w, out_h);

	h264sw_frame_unref(last);
	return 0;
}

/* Clamps an integer sample to [0, 255]. */
static uint8_t
clamp_u8(
	int32_t v)
{
	if (v < 0)
		return 0U;

	if (v > 255)
		return 255U;

	return (uint8_t)v;
}

/* Converts a 4:2:0 YCbCr h264sw_frame into 0xAARRGGBB pixels. */
static void
yuv420_to_argb(
	const struct h264sw_frame *f,
	uint32_t *out_pixels,
	uint32_t out_w,
	uint32_t out_h)
{
	uint32_t src_w;
	uint32_t src_h;
	uint32_t ox;
	uint32_t oy;
	uint32_t sx;
	uint32_t sy;
	int32_t y_val;
	int32_t u_val;
	int32_t v_val;
	int32_t c;
	int32_t d;
	int32_t e;
	uint8_t r;
	uint8_t g;
	uint8_t b;

	src_w = f->crop_width > 0U ? f->crop_width : f->width;
	src_h = f->crop_height > 0U ? f->crop_height : f->height;
	if (src_w == 0U || src_h == 0U)
		return;

	for (oy = 0U; oy < out_h; oy++) {
		sy = f->crop_y + (oy * src_h) / out_h;
		for (ox = 0U; ox < out_w; ox++) {
			sx = f->crop_x + (ox * src_w) / out_w;
			y_val = (int32_t)f->plane[0][sy * f->stride[0] + sx];
			u_val = (int32_t)f->plane[1][(sy / 2U) * f->stride[1] + (sx / 2U)];
			v_val = (int32_t)f->plane[2][(sy / 2U) * f->stride[2] + (sx / 2U)];

			c = y_val - 16;
			d = u_val - 128;
			e = v_val - 128;

			r = clamp_u8((298 * c + 409 * e + 128) >> 8);
			g = clamp_u8((298 * c - 100 * d - 208 * e + 128) >> 8);
			b = clamp_u8((298 * c + 516 * d + 128) >> 8);

			out_pixels[oy * out_w + ox] =
				0xFF000000U |
				((uint32_t)r << 16) |
				((uint32_t)g << 8) |
				(uint32_t)b;
		}
	}
}
