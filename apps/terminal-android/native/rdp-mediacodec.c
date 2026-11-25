/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Android AMediaCodec H.264 hardware decoder backend for mikuterm's RDP client,
 * with automatic fallback to libmedia's software h264sw decoder.
 */

#include "rdp-decoder.h"

#include "userland/desktop/libmedia/vcodec-nal.h"

#include <android/log.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Android MediaCodecInfo.CodecCapabilities YUV420 color format constants. */
#define MC_COLOR_YUV420_PLANAR			19
#define MC_COLOR_YUV420_PACKED_PLANAR		20
#define MC_COLOR_YUV420_SEMIPLANAR		21
#define MC_COLOR_YUV420_PACKED_SEMIPLANAR	39
#define MC_COLOR_YUV420_FLEXIBLE		0x7F420888
#define MC_COLOR_QCOM_YUV420_SEMIPLANAR		0x7FA30C00
#define MC_COLOR_QCOM_YUV420_SEMIPLANAR_32M	0x7FA30C04

/* Initial default stream dimensions before output format negotiation. */
#define MC_DEFAULT_WIDTH			1280
#define MC_DEFAULT_HEIGHT			720

/* Dequeue timeouts in microseconds for low-latency RDP frames. */
#define MC_IN_TIMEOUT_US			5000
#define MC_OUT_TIMEOUT_US			100000

/*
 * Per-stream AMediaCodec H.264 decoder wrapper with h264sw fallback.
 */
struct rdp_mediacodec_ctx {
	AMediaCodec *codec;
	int configured;
	int started;
	int fallback;
	void *sw_ctx;
	int32_t out_width;
	int32_t out_height;
	int32_t out_stride;
	int32_t out_slice_height;
	int32_t out_color_format;
	int32_t crop_left;
	int32_t crop_top;
	int32_t crop_right;
	int32_t crop_bottom;
	int has_crop;
	int64_t pts_us;
};

static int mediacodec_create(void **ctx);
static void mediacodec_destroy(void *ctx);
static int mediacodec_decode(void *ctx, const uint8_t *data, size_t len, uint32_t *out_pixels, uint32_t out_w, uint32_t out_h, uint32_t *decoded_w, uint32_t *decoded_h);
static int mediacodec_activate_fallback(struct rdp_mediacodec_ctx *m);
static int mediacodec_configure_start(struct rdp_mediacodec_ctx *m, int32_t width, int32_t height);
static void mediacodec_read_output_format(struct rdp_mediacodec_ctx *m, AMediaFormat *fmt);
static int mediacodec_has_vcl_nal(const uint8_t *data, size_t len);
static uint8_t clamp_u8(int32_t v);
static int64_t mediacodec_now_us(void);
static void mediacodec_yuv_to_argb(const struct rdp_mediacodec_ctx *m, const uint8_t *buf, size_t buf_size, uint32_t *out_pixels, uint32_t out_w, uint32_t out_h);

/* AMediaCodec H.264 decoder operations table. */
const struct rdp_decoder_ops rdp_mediacodec_decoder_ops = {
	mediacodec_create,
	mediacodec_destroy,
	mediacodec_decode
};

/* Creates an AMediaCodec H.264 decoder context, falling back to h264sw if unavailable. */
static int
mediacodec_create(
	void **ctx)
{
	struct rdp_mediacodec_ctx *m;

	if (ctx == NULL)
		return EINVAL;
	*ctx = NULL;
	m = (struct rdp_mediacodec_ctx *)calloc(1U, sizeof(*m));
	if (m == NULL)
		return ENOMEM;

	m->codec = AMediaCodec_createDecoderByType("video/avc");
	if (m->codec == NULL) {
		__android_log_print(ANDROID_LOG_WARN, "mikuterm-rdp",
				    "AMediaCodec_createDecoderByType(video/avc) failed; falling back to h264sw");
		if (mediacodec_activate_fallback(m) != 0) {
			free(m);
			return ENOMEM;
		}
		*ctx = m;
		return 0;
	}

	if (mediacodec_configure_start(m, MC_DEFAULT_WIDTH, MC_DEFAULT_HEIGHT) != 0) {
		__android_log_print(ANDROID_LOG_WARN, "mikuterm-rdp",
				    "AMediaCodec configure/start failed; falling back to h264sw");
		if (mediacodec_activate_fallback(m) != 0) {
			free(m);
			return ENOMEM;
		}
	}

	*ctx = m;
	return 0;
}

/* Destroys the AMediaCodec or fallback h264sw decoder context. */
static void
mediacodec_destroy(
	void *ctx)
{
	struct rdp_mediacodec_ctx *m;

	if (ctx == NULL)
		return;
	m = (struct rdp_mediacodec_ctx *)ctx;
	if (m->codec != NULL) {
		if (m->started != 0)
			(void)AMediaCodec_stop(m->codec);
		(void)AMediaCodec_delete(m->codec);
		m->codec = NULL;
	}
	if (m->sw_ctx != NULL) {
		rdp_h264sw_decoder_ops.destroy(m->sw_ctx);
		m->sw_ctx = NULL;
	}
	free(m);
}

/* Decodes one Annex-B access unit via AMediaCodec, falling back to h264sw on error. */
static int
mediacodec_decode(
	void *ctx,
	const uint8_t *data,
	size_t len,
	uint32_t *out_pixels,
	uint32_t out_w,
	uint32_t out_h,
	uint32_t *decoded_w,
	uint32_t *decoded_h)
{
	struct rdp_mediacodec_ctx *m;
	ssize_t in_idx;
	size_t in_size;
	uint8_t *in_buf;
	uint32_t flags;
	media_status_t st;
	AMediaCodecBufferInfo info;
	ssize_t out_idx;
	ssize_t last_idx;
	AMediaCodecBufferInfo last_info;
	int64_t timeout_us;
	int64_t queued_pts;
	int64_t deadline_us;
	int got_current;
	uint8_t *out_buf;
	size_t out_size;
	uint32_t fw;
	uint32_t fh;

	if (ctx == NULL || data == NULL || len == 0U)
		return EINVAL;
	m = (struct rdp_mediacodec_ctx *)ctx;
	if (m->fallback != 0)
		return rdp_h264sw_decoder_ops.decode(m->sw_ctx, data, len,
						     out_pixels, out_w, out_h,
						     decoded_w, decoded_h);

	in_idx = AMediaCodec_dequeueInputBuffer(m->codec, MC_IN_TIMEOUT_US);
	if (in_idx < 0) {
		if (in_idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
			return EAGAIN;
		if (mediacodec_activate_fallback(m) == 0)
			return rdp_h264sw_decoder_ops.decode(m->sw_ctx, data, len,
							     out_pixels, out_w, out_h,
							     decoded_w, decoded_h);
		return EIO;
	}

	in_size = 0U;
	in_buf = AMediaCodec_getInputBuffer(m->codec, (size_t)in_idx, &in_size);
	if (in_buf == NULL || in_size < len) {
		if (mediacodec_activate_fallback(m) == 0)
			return rdp_h264sw_decoder_ops.decode(m->sw_ctx, data, len,
							     out_pixels, out_w, out_h,
							     decoded_w, decoded_h);
		return ENOBUFS;
	}

	memcpy(in_buf, data, len);
	flags = mediacodec_has_vcl_nal(data, len) ? 0U : AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG;
	queued_pts = m->pts_us;
	st = AMediaCodec_queueInputBuffer(m->codec, (size_t)in_idx, 0U, len,
					  (uint64_t)queued_pts, flags);
	m->pts_us += 16667;
	if (st != AMEDIA_OK) {
		__android_log_print(ANDROID_LOG_WARN, "mikuterm-rdp",
				    "AMediaCodec_queueInputBuffer failed (%d); falling back to h264sw",
				    (int)st);
		if (mediacodec_activate_fallback(m) == 0)
			return rdp_h264sw_decoder_ops.decode(m->sw_ctx, data, len,
							     out_pixels, out_w, out_h,
							     decoded_w, decoded_h);
		return EIO;
	}

	last_idx = -1;
	memset(&last_info, 0, sizeof(last_info));
	got_current = 0;
	deadline_us = (flags == 0U) ? (mediacodec_now_us() + MC_OUT_TIMEOUT_US) : 0;
	for (;;) {
		if (flags == 0U && got_current == 0) {
			int64_t rem = deadline_us - mediacodec_now_us();
			timeout_us = rem > 0 ? rem : 0;
		} else {
			timeout_us = 0;
		}
		memset(&info, 0, sizeof(info));
		out_idx = AMediaCodec_dequeueOutputBuffer(m->codec, &info, timeout_us);
		if (out_idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
			AMediaFormat *ofmt = AMediaCodec_getOutputFormat(m->codec);
			if (ofmt != NULL) {
				mediacodec_read_output_format(m, ofmt);
				AMediaFormat_delete(ofmt);
			}
			continue;
		}
		if (out_idx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED)
			continue;
		if (out_idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
			break;
		if (out_idx < 0) {
			if (last_idx >= 0)
				(void)AMediaCodec_releaseOutputBuffer(m->codec, (size_t)last_idx, false);
			__android_log_print(ANDROID_LOG_WARN, "mikuterm-rdp",
					    "AMediaCodec_dequeueOutputBuffer failed (%zd); falling back to h264sw",
					    out_idx);
			if (mediacodec_activate_fallback(m) == 0)
				return rdp_h264sw_decoder_ops.decode(m->sw_ctx, data, len,
								     out_pixels, out_w, out_h,
								     decoded_w, decoded_h);
			return EIO;
		}
		if (last_idx >= 0)
			(void)AMediaCodec_releaseOutputBuffer(m->codec, (size_t)last_idx, false);
		last_idx = out_idx;
		last_info = info;
		if (info.presentationTimeUs >= queued_pts)
			got_current = 1;
	}

	if (last_idx < 0)
		return EAGAIN;

	out_size = 0U;
	out_buf = AMediaCodec_getOutputBuffer(m->codec, (size_t)last_idx, &out_size);
	if (out_buf != NULL && last_info.size > 0 &&
	    (size_t)last_info.offset < out_size) {
		size_t avail = out_size - (size_t)last_info.offset;
		if ((size_t)last_info.size < avail)
			avail = (size_t)last_info.size;

		if (m->has_crop != 0) {
			fw = (uint32_t)(m->crop_right - m->crop_left + 1);
			fh = (uint32_t)(m->crop_bottom - m->crop_top + 1);
		} else {
			fw = m->out_width > 0 ? (uint32_t)m->out_width : 0U;
			fh = m->out_height > 0 ? (uint32_t)m->out_height : 0U;
		}
		if (decoded_w != NULL && fw > 0U)
			*decoded_w = fw;
		if (decoded_h != NULL && fh > 0U)
			*decoded_h = fh;
		if (out_pixels != NULL && out_w > 0U && out_h > 0U &&
		    m->out_width > 0 && m->out_height > 0) {
			mediacodec_yuv_to_argb(m, out_buf + last_info.offset,
					       avail, out_pixels, out_w, out_h);
		}
	}

	(void)AMediaCodec_releaseOutputBuffer(m->codec, (size_t)last_idx, false);
	return 0;
}

/* Tears down AMediaCodec and initializes the software h264sw fallback. */
static int
mediacodec_activate_fallback(
	struct rdp_mediacodec_ctx *m)
{
	int error;

	if (m->codec != NULL) {
		if (m->started != 0)
			(void)AMediaCodec_stop(m->codec);
		(void)AMediaCodec_delete(m->codec);
		m->codec = NULL;
	}
	m->configured = 0;
	m->started = 0;
	if (m->sw_ctx == NULL) {
		error = rdp_h264sw_decoder_ops.create(&m->sw_ctx);
		if (error != 0)
			return error;
	}
	m->fallback = 1;
	return 0;
}

/* Configures and starts AMediaCodec for Annex-B video/avc decoding. */
static int
mediacodec_configure_start(
	struct rdp_mediacodec_ctx *m,
	int32_t width,
	int32_t height)
{
	static const int32_t color_fmts[] = {
		MC_COLOR_YUV420_FLEXIBLE,
		MC_COLOR_YUV420_SEMIPLANAR,
		MC_COLOR_YUV420_PLANAR
	};
	AMediaFormat *fmt;
	media_status_t st;
	size_t i;

	st = AMEDIA_ERROR_UNKNOWN;
	for (i = 0U; i < sizeof(color_fmts) / sizeof(color_fmts[0]); i++) {
		int low_latency;
		for (low_latency = 1; low_latency >= 0; low_latency--) {
			fmt = AMediaFormat_new();
			if (fmt == NULL)
				return ENOMEM;
			AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/avc");
			AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, width);
			AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, height);
			AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_COLOR_FORMAT, color_fmts[i]);
			if (low_latency != 0)
				AMediaFormat_setInt32(fmt, "low-latency", 1);
			st = AMediaCodec_configure(m->codec, fmt, NULL, NULL, 0U);
			AMediaFormat_delete(fmt);
			if (st == AMEDIA_OK)
				break;
		}
		if (st == AMEDIA_OK)
			break;
	}
	if (st != AMEDIA_OK)
		return EIO;

	m->configured = 1;
	st = AMediaCodec_start(m->codec);
	if (st != AMEDIA_OK)
		return EIO;

	m->started = 1;
	m->out_width = width;
	m->out_height = height;
	m->out_stride = width;
	m->out_slice_height = height;
	m->out_color_format = MC_COLOR_YUV420_SEMIPLANAR;
	return 0;
}

/* Updates cached plane geometry and crop rectangle from AMediaCodec output format. */
static void
mediacodec_read_output_format(
	struct rdp_mediacodec_ctx *m,
	AMediaFormat *fmt)
{
	int32_t v;
	int32_t left;
	int32_t top;
	int32_t right;
	int32_t bottom;

	if (AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, &v) && v > 0)
		m->out_width = v;
	if (AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, &v) && v > 0)
		m->out_height = v;
	if (AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_STRIDE, &v) && v > 0)
		m->out_stride = v;
	else
		m->out_stride = m->out_width;
	if (AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_SLICE_HEIGHT, &v) && v > 0)
		m->out_slice_height = v;
	else
		m->out_slice_height = m->out_height;
	if (AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_COLOR_FORMAT, &v))
		m->out_color_format = v;

	m->has_crop = 0;
	if (AMediaFormat_getRect(fmt, "crop", &left, &top, &right, &bottom) ||
	    AMediaFormat_getRect(fmt, AMEDIAFORMAT_KEY_DISPLAY_CROP, &left, &top, &right, &bottom)) {
		if (left >= 0 && top >= 0 && right >= left && bottom >= top &&
		    right < m->out_width && bottom < m->out_height) {
			m->crop_left = left;
			m->crop_top = top;
			m->crop_right = right;
			m->crop_bottom = bottom;
			m->has_crop = 1;
		}
	}
}

/* Returns 1 if the Annex-B access unit contains at least one VCL slice NAL (1..5). */
static int
mediacodec_has_vcl_nal(
	const uint8_t *data,
	size_t len)
{
	size_t cursor;
	const uint8_t *nal;
	size_t nal_len;
	uint8_t nal_type;

	cursor = 0U;
	while (vcodec_nal_find(data, len, &cursor, &nal, &nal_len) != 0) {
		if (nal_len == 0U)
			continue;
		nal_type = nal[0] & 0x1FU;
		if (nal_type >= 1U && nal_type <= 5U)
			return 1;
	}
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

/* Returns monotonic clock time in microseconds. */
static int64_t
mediacodec_now_us(void)
{
	struct timespec now;

	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (int64_t)now.tv_sec * 1000000 + (int64_t)(now.tv_nsec / 1000);
}

/* Converts AMediaCodec YUV420 (I420 or NV12) output planes to 0xAARRGGBB scaled to out_w x out_h. */
static void
mediacodec_yuv_to_argb(
	const struct rdp_mediacodec_ctx *m,
	const uint8_t *buf,
	size_t buf_size,
	uint32_t *out_pixels,
	uint32_t out_w,
	uint32_t out_h)
{
	uint32_t stride;
	uint32_t slice_h;
	uint32_t crop_x;
	uint32_t crop_y;
	uint32_t src_w;
	uint32_t src_h;
	int is_planar;
	const uint8_t *y_plane;
	const uint8_t *u_plane;
	const uint8_t *v_plane;
	uint32_t uv_stride;
	size_t needed;
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

	stride = m->out_stride > 0 ? (uint32_t)m->out_stride : (uint32_t)m->out_width;
	slice_h = m->out_slice_height > 0 ? (uint32_t)m->out_slice_height : (uint32_t)m->out_height;
	if (stride < (uint32_t)m->out_width)
		stride = (uint32_t)m->out_width;
	if (slice_h < (uint32_t)m->out_height)
		slice_h = (uint32_t)m->out_height;

	if (m->has_crop != 0) {
		crop_x = (uint32_t)m->crop_left;
		crop_y = (uint32_t)m->crop_top;
		src_w = (uint32_t)(m->crop_right - m->crop_left + 1);
		src_h = (uint32_t)(m->crop_bottom - m->crop_top + 1);
	} else {
		crop_x = 0U;
		crop_y = 0U;
		src_w = (uint32_t)m->out_width;
		src_h = (uint32_t)m->out_height;
	}
	if (src_w == 0U || src_h == 0U)
		return;

	is_planar = (m->out_color_format == MC_COLOR_YUV420_PLANAR ||
		     m->out_color_format == MC_COLOR_YUV420_PACKED_PLANAR);

	y_plane = buf;
	if (is_planar != 0) {
		uv_stride = (stride + 1U) / 2U;
		needed = (size_t)stride * slice_h +
			 2U * (size_t)uv_stride * ((slice_h + 1U) / 2U);
		if (buf_size < needed)
			return;
		u_plane = y_plane + (size_t)stride * slice_h;
		v_plane = u_plane + (size_t)uv_stride * ((slice_h + 1U) / 2U);
	} else {
		uv_stride = stride;
		needed = (size_t)stride * slice_h +
			 (size_t)uv_stride * ((slice_h + 1U) / 2U);
		if (buf_size < needed)
			return;
		u_plane = y_plane + (size_t)stride * slice_h;
		v_plane = NULL;
	}

	for (oy = 0U; oy < out_h; oy++) {
		sy = crop_y + (oy * src_h) / out_h;
		for (ox = 0U; ox < out_w; ox++) {
			sx = crop_x + (ox * src_w) / out_w;
			y_val = (int32_t)y_plane[(size_t)sy * stride + sx];
			if (is_planar != 0) {
				u_val = (int32_t)u_plane[(size_t)(sy / 2U) * uv_stride + (sx / 2U)];
				v_val = (int32_t)v_plane[(size_t)(sy / 2U) * uv_stride + (sx / 2U)];
			} else {
				size_t uv_off = (size_t)(sy / 2U) * uv_stride + (sx & ~1U);
				u_val = (int32_t)u_plane[uv_off];
				v_val = (int32_t)u_plane[uv_off + 1U];
			}
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
