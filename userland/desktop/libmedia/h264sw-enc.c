/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Software H.264 Constrained Baseline encoder (CAVLC, I and P slices,
 * low-latency, zero B-frames) registered as a libmedia encoding back end.
 */

#include "h264sw-cavlc.h"
#include "media-private.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Bitstream scratch capacity for one encoded access unit (4 MiB). */
#define H264ENC_BUF_CAP		(4U * 1024U * 1024U)

/* Minimum and maximum quantizer values in H.264. */
#define H264ENC_QP_MIN		10U
#define H264ENC_QP_MAX		51U

/*
 * Dynamic bit writer building an RBSP before Annex-B emulation prevention.
 */
struct h264enc_bw {
	uint8_t *data;
	size_t cap;
	size_t byte_pos;
	uint8_t cur;
	unsigned bit_pos;
	int overflow;
};

/*
 * Per-instance software H.264 encoder state.
 */
struct h264sw_enc {
	uint32_t width;
	uint32_t height;
	uint32_t mb_w;
	uint32_t mb_h;
	uint32_t stride_y;
	uint32_t stride_c;
	uint32_t qp;
	uint32_t key_interval;
	uint32_t frame_num;
	uint32_t idr_id;
	uint32_t frames_since_idr;
	int force_idr;
	uint8_t *ps_buf;
	size_t ps_len;
	uint8_t *au_buf;
	size_t au_len;
	int has_packet;
	int64_t pkt_time;
	int pkt_key;
	int draining;
	uint8_t *cur_y;
	uint8_t *cur_u;
	uint8_t *cur_v;
	uint8_t *ref_y;
	uint8_t *ref_u;
	uint8_t *ref_v;
	uint8_t *nz_y;
	uint8_t *nz_u;
	uint8_t *nz_v;
	uint8_t *rbsp;
	struct media_encoder_stats stats;
};

static int enc_query(unsigned codec, struct media_encoder_info *infos, unsigned capacity, unsigned *count);
static int enc_open(const struct media_encoder_config *config, void **state);
static int enc_send(void *state, const struct media_encoder_picture *picture, int64_t time_us, unsigned flags);
static int enc_receive(void *state, unsigned flags, struct media_encoded *packet);
static int enc_set_rate(void *state, uint32_t bitrate, uint32_t max_bitrate);
static int enc_parameter_sets(const void *state, const uint8_t **data, size_t *size);
static void enc_stats(const void *state, struct media_encoder_stats *stats);
static void enc_close(void *state);

static void bw_init(struct h264enc_bw *bw, uint8_t *buf, size_t cap);
static void bw_put_bit(struct h264enc_bw *bw, unsigned bit);
static void bw_put_bits(struct h264enc_bw *bw, uint32_t val, unsigned n);
static void bw_put_ue(struct h264enc_bw *bw, uint32_t val);
static void bw_put_se(struct h264enc_bw *bw, int32_t val);
static void bw_trailing_bits(struct h264enc_bw *bw);
static size_t rbsp_to_annexb(const uint8_t *rbsp, size_t rbsp_len, uint8_t nal_hdr, uint8_t *out, size_t out_cap);
static int build_parameter_sets(struct h264sw_enc *enc);
static void copy_picture_planes(struct h264sw_enc *enc, const struct media_encoder_picture *pic, unsigned fmt);
static int encode_frame(struct h264sw_enc *enc, int is_idr);
static void encode_cavlc_block(struct h264enc_bw *bw, const int16_t *coeffs, unsigned max_coeffs, int nc, uint8_t *out_nz);
static void fwd_4x4(const int16_t *in, int16_t *out);
static void inv_4x4(const int16_t *in, int16_t *out);
static void quant_4x4(int16_t *blk, uint32_t qp, int is_intra, int skip_dc);
static void dequant_4x4(int16_t *blk, uint32_t qp, int skip_dc);
static uint8_t clip_u8(int32_t v);

/* Software H.264 encoder operations table. */
const struct media_encoder_ops media_h264sw_encoder_ops = {
	"h264sw-enc",
	MEDIA_ENCODER_SOFTWARE,
	enc_query,
	enc_open,
	enc_send,
	NULL,
	NULL,
	NULL,
	enc_receive,
	enc_set_rate,
	enc_parameter_sets,
	enc_stats,
	enc_close
};

/* Zigzag scan order for 4x4 blocks. */
static const uint8_t zigzag4[16] = {
	0U, 1U, 4U, 8U,
	5U, 2U, 3U, 6U,
	9U, 12U, 13U, 10U,
	7U, 11U, 14U, 15U
};

/* Forward quantization scaling factors (mod 6) for H.264 4x4. */
static const uint16_t quant_mf[6][3] = {
	{ 13107U, 5243U, 8066U },
	{ 11916U, 4660U, 7490U },
	{ 10082U, 4194U, 6554U },
	{  9362U, 3647U, 5825U },
	{  8192U, 3355U, 5243U },
	{  7282U, 2893U, 4559U }
};

/* Inverse quantization scaling factors (mod 6) for H.264 4x4. */
static const uint16_t dequant_v[6][3] = {
	{ 10U, 16U, 13U },
	{ 11U, 18U, 14U },
	{ 13U, 20U, 16U },
	{ 14U, 23U, 18U },
	{ 16U, 25U, 20U },
	{ 18U, 29U, 23U }
};

/* Position class (0=corner, 1=center, 2=other) in a 4x4 block. */
static const uint8_t pos_class[16] = {
	0U, 2U, 0U, 2U,
	2U, 1U, 2U, 1U,
	0U, 2U, 0U, 2U,
	2U, 1U, 2U, 1U
};

/* Reports h264sw-enc capabilities for H.264 queries. */
static int
enc_query(
	unsigned codec,
	struct media_encoder_info *infos,
	unsigned capacity,
	unsigned *count)
{
	if (count == NULL)
		return EINVAL;
	if (codec != MEDIA_CODEC_H264) {
		*count = 0U;
		return 0;
	}
	*count = 1U;
	if (infos != NULL && capacity > 0U) {
		memset(&infos[0], 0, sizeof(infos[0]));
		snprintf(infos[0].backend, sizeof(infos[0].backend), "h264sw-enc");
		infos[0].kind = MEDIA_ENCODER_SOFTWARE;
		infos[0].codec = MEDIA_CODEC_H264;
		infos[0].profiles =
			(1U << MEDIA_ENCODER_PROFILE_DEFAULT) |
			(1U << MEDIA_ENCODER_PROFILE_H264_CONSTRAINED_BASELINE);
		infos[0].min_width = 16U;
		infos[0].min_height = 16U;
		infos[0].max_width = 3840U;
		infos[0].max_height = 2160U;
		infos[0].step = 2U;
		infos[0].rate_controls =
			(1U << MEDIA_ENCODER_RATE_DEFAULT) |
			(1U << MEDIA_ENCODER_RATE_CQP) |
			(1U << MEDIA_ENCODER_RATE_CBR) |
			(1U << MEDIA_ENCODER_RATE_VBR);
		infos[0].min_qp = H264ENC_QP_MIN;
		infos[0].max_qp = H264ENC_QP_MAX;
		infos[0].max_b_frames = 0U;
		infos[0].max_references = 1U;
		infos[0].quality_levels = 1U;
		infos[0].inputs =
			MEDIA_ENCODER_INPUT_NV12 | MEDIA_ENCODER_INPUT_I420;
		infos[0].flags = MEDIA_ENCODER_RATE_CHANGE;
	}
	return 0;
}

/* Opens a software H.264 encoder for config. */
static int
enc_open(
	const struct media_encoder_config *config,
	void **state)
{
	struct h264sw_enc *enc;
	size_t y_bytes;
	size_t c_bytes;
	size_t mb_count;
	if (config == NULL || state == NULL)
		return EINVAL;
	if (config->codec != MEDIA_CODEC_H264)
		return MEDIA_PROBLEM_CONFIG;
	if (config->input != MEDIA_ENCODER_INPUT_NV12 &&
	    config->input != MEDIA_ENCODER_INPUT_I420)
		return MEDIA_PROBLEM_CONFIG;
	if (config->profile != MEDIA_ENCODER_PROFILE_DEFAULT &&
	    config->profile != MEDIA_ENCODER_PROFILE_H264_CONSTRAINED_BASELINE)
		return MEDIA_PROBLEM_CONFIG;
	enc = (struct h264sw_enc *)calloc(1U, sizeof(*enc));
	if (enc == NULL)
		return ENOMEM;
	enc->width = config->width;
	enc->height = config->height;
	enc->mb_w = (config->width + 15U) / 16U;
	enc->mb_h = (config->height + 15U) / 16U;
	enc->stride_y = enc->mb_w * 16U;
	enc->stride_c = enc->mb_w * 8U;
	enc->qp = config->qp > 0U ? config->qp : 26U;
	if (enc->qp < H264ENC_QP_MIN)
		enc->qp = H264ENC_QP_MIN;
	if (enc->qp > H264ENC_QP_MAX)
		enc->qp = H264ENC_QP_MAX;
	enc->key_interval = config->key_interval > 0U ? config->key_interval : 60U;
	enc->force_idr = 1;
	y_bytes = (size_t)enc->stride_y * (size_t)(enc->mb_h * 16U);
	c_bytes = (size_t)enc->stride_c * (size_t)(enc->mb_h * 8U);
	mb_count = (size_t)enc->mb_w * (size_t)enc->mb_h;
	enc->cur_y = (uint8_t *)calloc(1U, y_bytes);
	enc->cur_u = (uint8_t *)calloc(1U, c_bytes);
	enc->cur_v = (uint8_t *)calloc(1U, c_bytes);
	enc->ref_y = (uint8_t *)calloc(1U, y_bytes);
	enc->ref_u = (uint8_t *)calloc(1U, c_bytes);
	enc->ref_v = (uint8_t *)calloc(1U, c_bytes);
	enc->nz_y = (uint8_t *)calloc(mb_count, 16U);
	enc->nz_u = (uint8_t *)calloc(mb_count, 4U);
	enc->nz_v = (uint8_t *)calloc(mb_count, 4U);
	enc->ps_buf = (uint8_t *)malloc(256U);
	enc->au_buf = (uint8_t *)malloc(H264ENC_BUF_CAP);
	enc->rbsp = (uint8_t *)malloc(H264ENC_BUF_CAP);
	if (enc->cur_y == NULL || enc->cur_u == NULL || enc->cur_v == NULL ||
	    enc->ref_y == NULL || enc->ref_u == NULL || enc->ref_v == NULL ||
	    enc->nz_y == NULL || enc->nz_u == NULL || enc->nz_v == NULL ||
	    enc->ps_buf == NULL || enc->au_buf == NULL || enc->rbsp == NULL) {
		enc_close(enc);
		return ENOMEM;
	}
	if (build_parameter_sets(enc) != 0) {
		enc_close(enc);
		return EINVAL;
	}
	*state = enc;
	return 0;
}

/* Accepts one CPU picture (or NULL to drain) and encodes it immediately. */
static int
enc_send(
	void *state,
	const struct media_encoder_picture *picture,
	int64_t time_us,
	unsigned flags)
{
	struct h264sw_enc *enc;
	int is_idr;
	unsigned fmt;
	int error;
	enc = (struct h264sw_enc *)state;
	if (picture == NULL) {
		enc->draining = 1;
		return 0;
	}
	if (enc->has_packet != 0)
		return EAGAIN;
	fmt = picture->plane[2] != NULL ?
	      MEDIA_ENCODING_FORMAT_I420 : MEDIA_ENCODING_FORMAT_NV12;
	copy_picture_planes(enc, picture, fmt);
	is_idr = 0;
	if (enc->force_idr != 0 || (flags & MEDIA_ENCODE_FORCE_KEY) != 0U ||
	    enc->frames_since_idr >= enc->key_interval) {
		is_idr = 1;
		enc->force_idr = 0;
		enc->frames_since_idr = 0U;
		enc->frame_num = 0U;
	} else {
		enc->frames_since_idr++;
	}
	error = encode_frame(enc, is_idr);
	if (error != 0)
		return error;
	enc->pkt_time = time_us;
	enc->pkt_key = is_idr;
	enc->has_packet = 1;
	enc->frame_num = (enc->frame_num + 1U) & 15U;
	if (is_idr != 0)
		enc->idr_id = (enc->idr_id + 1U) & 7U;
	return 0;
}

/* Returns the encoded access unit in Annex B without parameter sets. */
static int
enc_receive(
	void *state,
	unsigned flags,
	struct media_encoded *packet)
{
	struct h264sw_enc *enc;
	(void)flags;
	enc = (struct h264sw_enc *)state;
	if (enc->has_packet == 0) {
		if (enc->draining != 0)
			return MEDIA_ENCODER_DRAINED;
		return EAGAIN;
	}
	packet->data = enc->au_buf;
	packet->size = enc->au_len;
	packet->time_us = enc->pkt_time;
	packet->decode_time_us = enc->pkt_time;
	packet->flags = enc->pkt_key != 0 ? MEDIA_ENCODED_KEY : 0U;
	enc->has_packet = 0;
	return 0;
}

/* Updates target bitrate by adjusting QP slightly within safe bounds. */
static int
enc_set_rate(
	void *state,
	uint32_t bitrate,
	uint32_t max_bitrate)
{
	struct h264sw_enc *enc;
	(void)max_bitrate;
	enc = (struct h264sw_enc *)state;
	if (bitrate < 500000U)
		enc->qp = 34U;
	else if (bitrate < 2000000U)
		enc->qp = 28U;
	else
		enc->qp = 24U;
	return 0;
}

/* Returns Annex-B SPS + PPS parameter sets owned by enc until close. */
static int
enc_parameter_sets(
	const void *state,
	const uint8_t **data,
	size_t *size)
{
	const struct h264sw_enc *enc;
	enc = (const struct h264sw_enc *)state;
	*data = enc->ps_buf;
	*size = enc->ps_len;
	return 0;
}

/* Reports encoder statistics. */
static void
enc_stats(
	const void *state,
	struct media_encoder_stats *stats)
{
	const struct h264sw_enc *enc;
	enc = (const struct h264sw_enc *)state;
	*stats = enc->stats;
}

/* Frees all buffers allocated by the software H.264 encoder. */
static void
enc_close(
	void *state)
{
	struct h264sw_enc *enc;
	if (state == NULL)
		return;
	enc = (struct h264sw_enc *)state;
	free(enc->cur_y);
	free(enc->cur_u);
	free(enc->cur_v);
	free(enc->ref_y);
	free(enc->ref_u);
	free(enc->ref_v);
	free(enc->nz_y);
	free(enc->nz_u);
	free(enc->nz_v);
	free(enc->ps_buf);
	free(enc->au_buf);
	free(enc->rbsp);
	free(enc);
}

/* Initializes a bit writer over buf[0..cap). */
static void
bw_init(
	struct h264enc_bw *bw,
	uint8_t *buf,
	size_t cap)
{
	bw->data = buf;
	bw->cap = cap;
	bw->byte_pos = 0U;
	bw->cur = 0U;
	bw->bit_pos = 0U;
	bw->overflow = 0;
}

/* Appends one bit to bw. */
static void
bw_put_bit(
	struct h264enc_bw *bw,
	unsigned bit)
{
	if (bw->overflow != 0)
		return;
	bw->cur = (uint8_t)((bw->cur << 1) | (bit & 1U));
	bw->bit_pos++;
	if (bw->bit_pos == 8U) {
		if (bw->byte_pos >= bw->cap) {
			bw->overflow = 1;
			return;
		}
		bw->data[bw->byte_pos++] = bw->cur;
		bw->cur = 0U;
		bw->bit_pos = 0U;
	}
}

/* Appends n bits (MSB first) to bw. */
static void
bw_put_bits(
	struct h264enc_bw *bw,
	uint32_t val,
	unsigned n)
{
	unsigned i;
	for (i = n; i > 0U; i--)
		bw_put_bit(bw, (val >> (i - 1U)) & 1U);
}

/* Appends an unsigned Exp-Golomb code ue(val). */
static void
bw_put_ue(
	struct h264enc_bw *bw,
	uint32_t val)
{
	uint32_t code;
	unsigned bits;
	uint32_t tmp;
	code = val + 1U;
	bits = 0U;
	tmp = code;
	while (tmp > 0U) {
		bits++;
		tmp >>= 1;
	}
	if (bits > 1U)
		bw_put_bits(bw, 0U, bits - 1U);
	bw_put_bits(bw, code, bits);
}

/* Appends a signed Exp-Golomb code se(val). */
static void
bw_put_se(
	struct h264enc_bw *bw,
	int32_t val)
{
	uint32_t mapped;
	if (val <= 0)
		mapped = (uint32_t)(-val) * 2U;
	else
		mapped = (uint32_t)val * 2U - 1U;
	bw_put_ue(bw, mapped);
}

/* Appends rbsp_trailing_bits (1 followed by zeros to byte boundary). */
static void
bw_trailing_bits(
	struct h264enc_bw *bw)
{
	bw_put_bit(bw, 1U);
	while (bw->bit_pos != 0U && bw->overflow == 0)
		bw_put_bit(bw, 0U);
}

/* Writes a 4-byte Annex-B start code, NAL header, and escaped RBSP. */
static size_t
rbsp_to_annexb(
	const uint8_t *rbsp,
	size_t rbsp_len,
	uint8_t nal_hdr,
	uint8_t *out,
	size_t out_cap)
{
	size_t pos;
	size_t i;
	unsigned zeros;
	if (out_cap < 5U)
		return 0U;
	pos = 0U;
	out[pos++] = 0x00U;
	out[pos++] = 0x00U;
	out[pos++] = 0x00U;
	out[pos++] = 0x01U;
	out[pos++] = nal_hdr;
	zeros = 0U;
	for (i = 0U; i < rbsp_len; i++) {
		if (zeros == 2U && rbsp[i] <= 0x03U) {
			if (pos >= out_cap)
				return 0U;
			out[pos++] = 0x03U;
			zeros = 0U;
		}
		if (pos >= out_cap)
			return 0U;
		out[pos++] = rbsp[i];
		if (rbsp[i] == 0x00U)
			zeros++;
		else
			zeros = 0U;
	}
	return pos;
}

/* Encodes Constrained Baseline SPS and PPS into enc->ps_buf. */
static int
build_parameter_sets(
	struct h264sw_enc *enc)
{
	struct h264enc_bw bw;
	uint8_t tmp[128];
	size_t sps_sz;
	size_t pps_sz;
	uint32_t crop_r;
	uint32_t crop_b;

	/* Encodes SPS RBSP. */
	bw_init(&bw, tmp, sizeof(tmp));
	bw_put_bits(&bw, 66U, 8U);
	bw_put_bits(&bw, 0x40U, 8U);
	bw_put_bits(&bw, 41U, 8U);
	bw_put_ue(&bw, 0U);
	bw_put_ue(&bw, 0U);
	bw_put_ue(&bw, 2U);
	bw_put_ue(&bw, 1U);
	bw_put_bit(&bw, 0U);
	bw_put_ue(&bw, enc->mb_w - 1U);
	bw_put_ue(&bw, enc->mb_h - 1U);
	bw_put_bit(&bw, 1U);
	bw_put_bit(&bw, 1U);
	crop_r = (enc->mb_w * 16U - enc->width) / 2U;
	crop_b = (enc->mb_h * 16U - enc->height) / 2U;
	if (crop_r > 0U || crop_b > 0U) {
		bw_put_bit(&bw, 1U);
		bw_put_ue(&bw, 0U);
		bw_put_ue(&bw, crop_r);
		bw_put_ue(&bw, 0U);
		bw_put_ue(&bw, crop_b);
	} else {
		bw_put_bit(&bw, 0U);
	}
	bw_put_bit(&bw, 0U);
	bw_trailing_bits(&bw);
	if (bw.overflow != 0)
		return EINVAL;
	sps_sz = rbsp_to_annexb(tmp, bw.byte_pos, 0x67U, enc->ps_buf, 256U);
	if (sps_sz == 0U)
		return EINVAL;

	/* Encodes PPS RBSP. */
	bw_init(&bw, tmp, sizeof(tmp));
	bw_put_ue(&bw, 0U);
	bw_put_ue(&bw, 0U);
	bw_put_bit(&bw, 0U);
	bw_put_bit(&bw, 0U);
	bw_put_ue(&bw, 0U);
	bw_put_ue(&bw, 0U);
	bw_put_ue(&bw, 0U);
	bw_put_bit(&bw, 0U);
	bw_put_bits(&bw, 0U, 2U);
	bw_put_se(&bw, 0);
	bw_put_se(&bw, 0);
	bw_put_se(&bw, 0);
	bw_put_bit(&bw, 1U);
	bw_put_bit(&bw, 0U);
	bw_put_bit(&bw, 0U);
	bw_trailing_bits(&bw);
	if (bw.overflow != 0)
		return EINVAL;
	pps_sz = rbsp_to_annexb(tmp, bw.byte_pos, 0x68U, enc->ps_buf + sps_sz,
				256U - sps_sz);
	if (pps_sz == 0U)
		return EINVAL;
	enc->ps_len = sps_sz + pps_sz;
	return 0;
}

/* Copies input NV12 or I420 picture into macroblock-padded planar buffers. */
static void
copy_picture_planes(
	struct h264sw_enc *enc,
	const struct media_encoder_picture *pic,
	unsigned fmt)
{
	uint32_t y;
	uint32_t x;
	uint32_t cw;
	uint32_t ch;
	const uint8_t *src_y;
	const uint8_t *src_u;
	const uint8_t *src_v;
	for (y = 0U; y < enc->height; y++) {
		src_y = pic->plane[0] + (size_t)y * pic->stride[0];
		memcpy(enc->cur_y + (size_t)y * enc->stride_y, src_y,
		       enc->width);
		for (x = enc->width; x < enc->stride_y; x++) {
			enc->cur_y[(size_t)y * enc->stride_y + x] =
				src_y[enc->width - 1U];
		}
	}
	for (y = enc->height; y < enc->mb_h * 16U; y++) {
		memcpy(enc->cur_y + (size_t)y * enc->stride_y,
		       enc->cur_y + (size_t)(enc->height - 1U) * enc->stride_y,
		       enc->stride_y);
	}
	cw = (enc->width + 1U) / 2U;
	ch = (enc->height + 1U) / 2U;
	for (y = 0U; y < ch; y++) {
		if (fmt == MEDIA_ENCODING_FORMAT_I420) {
			src_u = pic->plane[1] + (size_t)y * pic->stride[1];
			src_v = pic->plane[2] + (size_t)y * pic->stride[2];
			memcpy(enc->cur_u + (size_t)y * enc->stride_c, src_u,
			       cw);
			memcpy(enc->cur_v + (size_t)y * enc->stride_c, src_v,
			       cw);
		} else {
			src_u = pic->plane[1] + (size_t)y * pic->stride[1];
			for (x = 0U; x < cw; x++) {
				enc->cur_u[(size_t)y * enc->stride_c + x] =
					src_u[x * 2U];
				enc->cur_v[(size_t)y * enc->stride_c + x] =
					src_u[x * 2U + 1U];
			}
		}
		for (x = cw; x < enc->stride_c; x++) {
			enc->cur_u[(size_t)y * enc->stride_c + x] =
				enc->cur_u[(size_t)y * enc->stride_c + cw - 1U];
			enc->cur_v[(size_t)y * enc->stride_c + x] =
				enc->cur_v[(size_t)y * enc->stride_c + cw - 1U];
		}
	}
	for (y = ch; y < enc->mb_h * 8U; y++) {
		memcpy(enc->cur_u + (size_t)y * enc->stride_c,
		       enc->cur_u + (size_t)(ch - 1U) * enc->stride_c,
		       enc->stride_c);
		memcpy(enc->cur_v + (size_t)y * enc->stride_c,
		       enc->cur_v + (size_t)(ch - 1U) * enc->stride_c,
		       enc->stride_c);
	}
}

static int
compute_luma_nc(
	const struct h264sw_enc *enc,
	uint32_t mbx,
	uint32_t mby,
	uint32_t bx,
	uint32_t by)
{
	int have_a;
	int have_b;
	int na;
	int nb;
	uint32_t a_idx;
	uint32_t b_idx;
	have_a = (bx > 0U || mbx > 0U);
	have_b = (by > 0U || mby > 0U);
	na = 0;
	nb = 0;
	if (have_a) {
		uint32_t ax;
		uint32_t amb;
		ax = bx > 0U ? bx - 1U : 3U;
		amb = bx > 0U ? (mby * enc->mb_w + mbx) : (mby * enc->mb_w + mbx - 1U);
		a_idx = ((by & 2U) << 2) | ((ax & 2U) << 1) | ((by & 1U) << 1) | (ax & 1U);
		na = (int)enc->nz_y[amb * 16U + a_idx];
	}
	if (have_b) {
		uint32_t ty;
		uint32_t bmb;
		ty = by > 0U ? by - 1U : 3U;
		bmb = by > 0U ? (mby * enc->mb_w + mbx) : ((mby - 1U) * enc->mb_w + mbx);
		b_idx = ((ty & 2U) << 2) | ((bx & 2U) << 1) | ((ty & 1U) << 1) | (bx & 1U);
		nb = (int)enc->nz_y[bmb * 16U + b_idx];
	}
	if (have_a && have_b)
		return (na + nb + 1) >> 1;
	if (have_a)
		return na;
	if (have_b)
		return nb;
	return 0;
}

static int
compute_chroma_nc(
	const struct h264sw_enc *enc,
	const uint8_t *nz_c,
	uint32_t mbx,
	uint32_t mby,
	uint32_t bx,
	uint32_t by)
{
	int have_a;
	int have_b;
	int na;
	int nb;
	have_a = (bx > 0U || mbx > 0U);
	have_b = (by > 0U || mby > 0U);
	na = 0;
	nb = 0;
	if (have_a) {
		uint32_t ax;
		uint32_t amb;
		ax = bx > 0U ? bx - 1U : 1U;
		amb = bx > 0U ? (mby * enc->mb_w + mbx) : (mby * enc->mb_w + mbx - 1U);
		na = (int)nz_c[amb * 4U + by * 2U + ax];
	}
	if (have_b) {
		uint32_t ty;
		uint32_t bmb;
		ty = by > 0U ? by - 1U : 1U;
		bmb = by > 0U ? (mby * enc->mb_w + mbx) : ((mby - 1U) * enc->mb_w + mbx);
		nb = (int)nz_c[bmb * 4U + ty * 2U + bx];
	}
	if (have_a && have_b)
		return (na + nb + 1) >> 1;
	if (have_a)
		return na;
	if (have_b)
		return nb;
	return 0;
}

static void
hadamard_4x4(
	const int16_t *in,
	int16_t *out)
{
	int32_t t[16];
	int i;
	for (i = 0; i < 4; i++) {
		int32_t s0;
		int32_t s1;
		int32_t d0;
		int32_t d1;
		s0 = (int32_t)in[i * 4 + 0] + (int32_t)in[i * 4 + 3];
		s1 = (int32_t)in[i * 4 + 1] + (int32_t)in[i * 4 + 2];
		d0 = (int32_t)in[i * 4 + 0] - (int32_t)in[i * 4 + 3];
		d1 = (int32_t)in[i * 4 + 1] - (int32_t)in[i * 4 + 2];
		t[i * 4 + 0] = s0 + s1;
		t[i * 4 + 1] = d0 + d1;
		t[i * 4 + 2] = s0 - s1;
		t[i * 4 + 3] = d0 - d1;
	}
	for (i = 0; i < 4; i++) {
		int32_t s0;
		int32_t s1;
		int32_t d0;
		int32_t d1;
		s0 = t[0 * 4 + i] + t[3 * 4 + i];
		s1 = t[1 * 4 + i] + t[2 * 4 + i];
		d0 = t[0 * 4 + i] - t[3 * 4 + i];
		d1 = t[1 * 4 + i] - t[2 * 4 + i];
		out[0 * 4 + i] = (int16_t)((s0 + s1) >> 1);
		out[1 * 4 + i] = (int16_t)((d0 + d1) >> 1);
		out[2 * 4 + i] = (int16_t)((s0 - s1) >> 1);
		out[3 * 4 + i] = (int16_t)((d0 - d1) >> 1);
	}
}

static void
inv_hadamard_4x4(
	const int16_t *in,
	int16_t *out)
{
	int32_t t[16];
	int i;
	for (i = 0; i < 4; i++) {
		int32_t s0;
		int32_t s1;
		int32_t d0;
		int32_t d1;
		s0 = (int32_t)in[i * 4 + 0] + (int32_t)in[i * 4 + 2];
		s1 = (int32_t)in[i * 4 + 0] - (int32_t)in[i * 4 + 2];
		d0 = (int32_t)in[i * 4 + 1] + (int32_t)in[i * 4 + 3];
		d1 = (int32_t)in[i * 4 + 1] - (int32_t)in[i * 4 + 3];
		t[i * 4 + 0] = s0 + d0;
		t[i * 4 + 1] = s1 + d1;
		t[i * 4 + 2] = s1 - d1;
		t[i * 4 + 3] = s0 - d0;
	}
	for (i = 0; i < 4; i++) {
		int32_t s0;
		int32_t s1;
		int32_t d0;
		int32_t d1;
		s0 = t[0 * 4 + i] + t[2 * 4 + i];
		s1 = t[0 * 4 + i] - t[2 * 4 + i];
		d0 = t[1 * 4 + i] + t[3 * 4 + i];
		d1 = t[1 * 4 + i] - t[3 * 4 + i];
		out[0 * 4 + i] = (int16_t)(s0 + d0);
		out[1 * 4 + i] = (int16_t)(s1 + d1);
		out[2 * 4 + i] = (int16_t)(s1 - d1);
		out[3 * 4 + i] = (int16_t)(s0 - d0);
	}
}

static void
quant_dc_blk(
	int16_t *blk,
	unsigned count,
	uint32_t qp)
{
	uint32_t qbits;
	uint32_t qp6;
	uint32_t f;
	unsigned i;
	qbits = 16U + qp / 6U;
	qp6 = qp % 6U;
	f = (1U << qbits) / 3U;
	for (i = 0U; i < count; i++) {
		int32_t v;
		int32_t mag;
		v = (int32_t)blk[i];
		mag = v < 0 ? -v : v;
		mag = (int32_t)(((uint32_t)mag * quant_mf[qp6][0] + f) >> qbits);
		blk[i] = (int16_t)(v < 0 ? -mag : mag);
	}
}

static void
dequant_dc_blk(
	int16_t *blk,
	unsigned count,
	uint32_t qp,
	int is_chroma)
{
	uint32_t shift;
	uint32_t qp6;
	uint32_t base_shift;
	unsigned i;
	shift = qp / 6U;
	qp6 = qp % 6U;
	base_shift = is_chroma != 0 ? 1U : 2U;
	for (i = 0U; i < count; i++) {
		int32_t val;
		val = (int32_t)blk[i] * (int32_t)dequant_v[qp6][0];
		if (shift >= base_shift)
			val = val * (int32_t)(1U << (shift - base_shift));
		else
			val = (val + (int32_t)(1U << (base_shift - shift - 1U))) >> (base_shift - shift);
		blk[i] = (int16_t)val;
	}
}

static void
encode_chroma_mb(
	struct h264sw_enc *enc,
	struct h264enc_bw *bw,
	uint32_t mbx,
	uint32_t mby,
	int is_idr)
{
	uint8_t pred[2][64];
	int16_t ac[2][4][16];
	int16_t dc_inv[2][4];
	const uint8_t *cur[2];
	uint8_t *ref[2];
	uint8_t *nz_c[2];
	uint32_t mb_idx;
	uint32_t p;
	uint32_t blk;
	uint32_t y;
	uint32_t x;
	uint8_t nz;
	cur[0] = enc->cur_u;
	cur[1] = enc->cur_v;
	ref[0] = enc->ref_u;
	ref[1] = enc->ref_v;
	nz_c[0] = enc->nz_u;
	nz_c[1] = enc->nz_v;
	mb_idx = mby * enc->mb_w + mbx;
	for (p = 0U; p < 2U; p++) {
		int16_t dc[4];
		int16_t dc_h[4];
		if (is_idr != 0) {
			for (blk = 0U; blk < 4U; blk++) {
				uint32_t bx;
				uint32_t by;
				int have_l;
				int have_t;
				int32_t sum_l;
				int32_t sum_t;
				int32_t dc_val;
				bx = blk & 1U;
				by = blk >> 1;
				have_l = (mbx > 0U);
				have_t = (mby > 0U);
				sum_l = 0;
				sum_t = 0;
				if (have_l) {
					for (y = 0U; y < 4U; y++)
						sum_l += ref[p][(size_t)(mby * 8U + by * 4U + y) * enc->stride_c + mbx * 8U - 1U];
				}
				if (have_t) {
					for (x = 0U; x < 4U; x++)
						sum_t += ref[p][(size_t)(mby * 8U - 1U) * enc->stride_c + mbx * 8U + bx * 4U + x];
				}
				if (bx == by) {
					if (have_l && have_t)
						dc_val = (sum_l + sum_t + 4) >> 3;
					else if (have_l)
						dc_val = (sum_l + 2) >> 2;
					else if (have_t)
						dc_val = (sum_t + 2) >> 2;
					else
						dc_val = 128;
				} else if (bx == 1U && by == 0U) {
					if (have_t)
						dc_val = (sum_t + 2) >> 2;
					else if (have_l)
						dc_val = (sum_l + 2) >> 2;
					else
						dc_val = 128;
				} else {
					if (have_l)
						dc_val = (sum_l + 2) >> 2;
					else if (have_t)
						dc_val = (sum_t + 2) >> 2;
					else
						dc_val = 128;
				}
				for (y = 0U; y < 4U; y++) {
					for (x = 0U; x < 4U; x++)
						pred[p][(by * 4U + y) * 8U + bx * 4U + x] = (uint8_t)dc_val;
				}
			}
		} else {
			for (y = 0U; y < 8U; y++) {
				memcpy(pred[p] + y * 8U,
				       ref[p] + (size_t)(mby * 8U + y) * enc->stride_c + mbx * 8U,
				       8U);
			}
		}
		for (blk = 0U; blk < 4U; blk++) {
			uint32_t bx;
			uint32_t by;
			int16_t res[16];
			bx = blk & 1U;
			by = blk >> 1;
			for (y = 0U; y < 4U; y++) {
				size_t off;
				off = (size_t)(mby * 8U + by * 4U + y) * enc->stride_c + mbx * 8U + bx * 4U;
				for (x = 0U; x < 4U; x++) {
					res[y * 4U + x] = (int16_t)cur[p][off + x] -
							  (int16_t)pred[p][(by * 4U + y) * 8U + bx * 4U + x];
				}
			}
			fwd_4x4(res, ac[p][blk]);
			dc[blk] = ac[p][blk][0];
		}
		dc_h[0] = (int16_t)(dc[0] + dc[1] + dc[2] + dc[3]);
		dc_h[1] = (int16_t)(dc[0] - dc[1] + dc[2] - dc[3]);
		dc_h[2] = (int16_t)(dc[0] + dc[1] - dc[2] - dc[3]);
		dc_h[3] = (int16_t)(dc[0] - dc[1] - dc[2] + dc[3]);
		quant_dc_blk(dc_h, 4U, enc->qp);
		encode_cavlc_block(bw, dc_h, 4U, -1, &nz);
		dc_inv[p][0] = (int16_t)(dc_h[0] + dc_h[1] + dc_h[2] + dc_h[3]);
		dc_inv[p][1] = (int16_t)(dc_h[0] - dc_h[1] + dc_h[2] - dc_h[3]);
		dc_inv[p][2] = (int16_t)(dc_h[0] + dc_h[1] - dc_h[2] - dc_h[3]);
		dc_inv[p][3] = (int16_t)(dc_h[0] - dc_h[1] - dc_h[2] + dc_h[3]);
		dequant_dc_blk(dc_inv[p], 4U, enc->qp, 1);
	}
	for (p = 0U; p < 2U; p++) {
		for (blk = 0U; blk < 4U; blk++) {
			uint32_t bx;
			uint32_t by;
			int nc;
			int16_t recon[16];
			bx = blk & 1U;
			by = blk >> 1;
			quant_4x4(ac[p][blk], enc->qp, is_idr, 1);
			nc = compute_chroma_nc(enc, nz_c[p], mbx, mby, bx, by);
			encode_cavlc_block(bw, ac[p][blk], 15U, nc, &nz);
			nz_c[p][mb_idx * 4U + blk] = nz;
			dequant_4x4(ac[p][blk], enc->qp, 1);
			ac[p][blk][0] = dc_inv[p][blk];
			inv_4x4(ac[p][blk], recon);
			for (y = 0U; y < 4U; y++) {
				size_t off;
				off = (size_t)(mby * 8U + by * 4U + y) * enc->stride_c + mbx * 8U + bx * 4U;
				for (x = 0U; x < 4U; x++) {
					ref[p][off + x] = clip_u8(
						(int32_t)pred[p][(by * 4U + y) * 8U + bx * 4U + x] +
						recon[y * 4U + x]);
				}
			}
		}
	}
}

/* Encodes one IDR I-slice or P-slice into enc->au_buf. */
static int
encode_frame(
	struct h264sw_enc *enc,
	int is_idr)
{
	struct h264enc_bw bw;
	uint32_t mby;
	uint32_t mbx;
	uint32_t mb_idx;
	uint32_t skip_run;
	uint32_t blk;
	uint32_t y;
	uint32_t x;
	int16_t res[16];
	int16_t coeff[16];
	int16_t recon[16];
	uint8_t pred_y[256];
	uint8_t pred_u[64];
	uint8_t pred_v[64];
	int32_t dc_sum;
	uint32_t sad;
	uint8_t nz;
	uint8_t nal_hdr;
	size_t out_sz;
	bw_init(&bw, enc->rbsp, H264ENC_BUF_CAP);

	/* Writes the H.264 slice header. */
	bw_put_ue(&bw, 0U);
	bw_put_ue(&bw, is_idr != 0 ? 7U : 5U);
	bw_put_ue(&bw, 0U);
	bw_put_bits(&bw, enc->frame_num, 4U);
	if (is_idr != 0) {
		bw_put_ue(&bw, enc->idr_id);
		bw_put_bit(&bw, 0U);
		bw_put_bit(&bw, 0U);
	} else {
		bw_put_bit(&bw, 0U);
		bw_put_bit(&bw, 0U);
		bw_put_bit(&bw, 0U);
	}
	bw_put_se(&bw, (int32_t)enc->qp - 26);
	bw_put_ue(&bw, 1U);
	if (is_idr != 0) {
		memset(enc->nz_y, 0, (size_t)enc->mb_w * enc->mb_h * 16U);
		memset(enc->nz_u, 0, (size_t)enc->mb_w * enc->mb_h * 4U);
		memset(enc->nz_v, 0, (size_t)enc->mb_w * enc->mb_h * 4U);
	}
	skip_run = 0U;
	for (mby = 0U; mby < enc->mb_h; mby++) {
		for (mbx = 0U; mbx < enc->mb_w; mbx++) {
			mb_idx = mby * enc->mb_w + mbx;

			/* Checks P_Skip eligibility against reference frame. */
			if (is_idr == 0) {
				sad = 0U;
				for (y = 0U; y < 16U; y++) {
					size_t off;
					off = (size_t)(mby * 16U + y) * enc->stride_y +
					      mbx * 16U;
					for (x = 0U; x < 16U; x++) {
						int32_t d;
						d = (int32_t)enc->cur_y[off + x] -
						    (int32_t)enc->ref_y[off + x];
						sad += (uint32_t)(d < 0 ? -d : d);
					}
				}
				if (sad < 256U) {
					skip_run++;
					memset(&enc->nz_y[mb_idx * 16U], 0, 16U);
					memset(&enc->nz_u[mb_idx * 4U], 0, 4U);
					memset(&enc->nz_v[mb_idx * 4U], 0, 4U);
					continue;
				}
				bw_put_ue(&bw, skip_run);
				skip_run = 0U;
			}

			/* Computes 16x16 DC or (0,0) inter prediction. */
			if (is_idr != 0) {
				dc_sum = 0;
				if (mbx > 0U && mby > 0U) {
					for (y = 0U; y < 16U; y++) {
						dc_sum += enc->ref_y[(size_t)(mby * 16U + y) * enc->stride_y + mbx * 16U - 1U];
						dc_sum += enc->ref_y[(size_t)(mby * 16U - 1U) * enc->stride_y + mbx * 16U + y];
					}
					dc_sum = (dc_sum + 16) >> 5;
				} else if (mbx > 0U) {
					for (y = 0U; y < 16U; y++)
						dc_sum += enc->ref_y[(size_t)(mby * 16U + y) * enc->stride_y + mbx * 16U - 1U];
					dc_sum = (dc_sum + 8) >> 4;
				} else if (mby > 0U) {
					for (x = 0U; x < 16U; x++)
						dc_sum += enc->ref_y[(size_t)(mby * 16U - 1U) * enc->stride_y + mbx * 16U + x];
					dc_sum = (dc_sum + 8) >> 4;
				} else {
					dc_sum = 128;
				}
				memset(pred_y, (uint8_t)dc_sum, sizeof(pred_y));
				memset(pred_u, 128U, sizeof(pred_u));
				memset(pred_v, 128U, sizeof(pred_v));

				/* I_16x16_2_2_1 (DC, cbp_chroma=2, cbp_luma=15 -> mb_type=23). */
				bw_put_ue(&bw, 23U);
				bw_put_ue(&bw, 0U);
				bw_put_se(&bw, 0);

				{
					int16_t ac_y[16][16];
					int16_t dc_blk[16];
					int16_t dc_tr[16];
					int16_t dc_inv[16];
					int nc;
					for (blk = 0U; blk < 16U; blk++) {
						uint32_t by;
						uint32_t bx;
						by = ((blk >> 3) * 2U) + ((blk >> 1) & 1U);
						bx = (((blk >> 2) & 1U) * 2U) + (blk & 1U);
						for (y = 0U; y < 4U; y++) {
							size_t off;
							off = (size_t)(mby * 16U + by * 4U + y) * enc->stride_y +
							      mbx * 16U + bx * 4U;
							for (x = 0U; x < 4U; x++) {
								res[y * 4U + x] =
									(int16_t)enc->cur_y[off + x] -
									(int16_t)dc_sum;
							}
						}
						fwd_4x4(res, ac_y[blk]);
						dc_blk[by * 4U + bx] = ac_y[blk][0];
					}
					hadamard_4x4(dc_blk, dc_tr);
					quant_dc_blk(dc_tr, 16U, enc->qp);
					nc = compute_luma_nc(enc, mbx, mby, 0U, 0U);
					encode_cavlc_block(&bw, dc_tr, 16U, nc, &nz);
					inv_hadamard_4x4(dc_tr, dc_inv);
					dequant_dc_blk(dc_inv, 16U, enc->qp, 0);
					for (blk = 0U; blk < 16U; blk++) {
						uint32_t by;
						uint32_t bx;
						by = ((blk >> 3) * 2U) + ((blk >> 1) & 1U);
						bx = (((blk >> 2) & 1U) * 2U) + (blk & 1U);
						quant_4x4(ac_y[blk], enc->qp, 1, 1);
						nc = compute_luma_nc(enc, mbx, mby, bx, by);
						encode_cavlc_block(&bw, ac_y[blk], 15U, nc, &nz);
						enc->nz_y[mb_idx * 16U + blk] = nz;
						dequant_4x4(ac_y[blk], enc->qp, 1);
						ac_y[blk][0] = dc_inv[by * 4U + bx];
						inv_4x4(ac_y[blk], recon);
						for (y = 0U; y < 4U; y++) {
							size_t off;
							off = (size_t)(mby * 16U + by * 4U + y) * enc->stride_y +
							      mbx * 16U + bx * 4U;
							for (x = 0U; x < 4U; x++) {
								enc->ref_y[off + x] = clip_u8(
									(int32_t)dc_sum +
									recon[y * 4U + x]);
							}
						}
					}
				}
			} else {
				/* P_L0_16x16 with zero MV and CBP=47 (luma 15 + chroma 2 -> codeNum 12). */
				bw_put_ue(&bw, 0U);
				bw_put_se(&bw, 0);
				bw_put_se(&bw, 0);
				bw_put_ue(&bw, 12U);
				bw_put_se(&bw, 0);
				for (blk = 0U; blk < 16U; blk++) {
					uint32_t by;
					uint32_t bx;
					int nc;
					by = ((blk >> 3) * 2U) + ((blk >> 1) & 1U);
					bx = (((blk >> 2) & 1U) * 2U) + (blk & 1U);
					for (y = 0U; y < 4U; y++) {
						size_t off;
						off = (size_t)(mby * 16U + by * 4U + y) * enc->stride_y +
						      mbx * 16U + bx * 4U;
						for (x = 0U; x < 4U; x++) {
							res[y * 4U + x] =
								(int16_t)enc->cur_y[off + x] -
								(int16_t)enc->ref_y[off + x];
						}
					}
					fwd_4x4(res, coeff);
					quant_4x4(coeff, enc->qp, 0, 0);
					nc = compute_luma_nc(enc, mbx, mby, bx, by);
					encode_cavlc_block(&bw, coeff, 16U, nc, &nz);
					enc->nz_y[mb_idx * 16U + blk] = nz;
					dequant_4x4(coeff, enc->qp, 0);
					inv_4x4(coeff, recon);
					for (y = 0U; y < 4U; y++) {
						size_t off;
						off = (size_t)(mby * 16U + by * 4U + y) * enc->stride_y +
						      mbx * 16U + bx * 4U;
						for (x = 0U; x < 4U; x++) {
							enc->ref_y[off + x] = clip_u8(
								(int32_t)enc->ref_y[off + x] +
								recon[y * 4U + x]);
						}
					}
				}
			}

			encode_chroma_mb(enc, &bw, mbx, mby, is_idr);
		}
	}
	if (is_idr == 0 && skip_run > 0U)
		bw_put_ue(&bw, skip_run);
	bw_trailing_bits(&bw);
	if (bw.overflow != 0)
		return ENOBUFS;
	nal_hdr = is_idr != 0 ? 0x65U : 0x41U;
	out_sz = rbsp_to_annexb(enc->rbsp, bw.byte_pos, nal_hdr, enc->au_buf,
				H264ENC_BUF_CAP);
	if (out_sz == 0U)
		return ENOBUFS;
	enc->au_len = out_sz;
	return 0;
}

/* Encodes a 4x4, 15-coeff AC, or 2x2 chroma DC block using H.264 CAVLC. */
static void
encode_cavlc_block(
	struct h264enc_bw *bw,
	const int16_t *coeffs,
	unsigned max_coeffs,
	int nc,
	uint8_t *out_nz)
{
	int16_t scan[16];
	unsigned i;
	unsigned total_coeff;
	unsigned trailing_ones;
	int last_nz;
	unsigned start_idx;
	uint32_t t1_signs;
	int suffix_len;
	unsigned zeros_left;
	unsigned run_before;
	unsigned target_val;
	unsigned idx;
	unsigned rem_coeff;

	start_idx = max_coeffs == 15U ? 1U : 0U;
	total_coeff = 0U;
	last_nz = -1;
	for (i = 0U; i < max_coeffs; i++) {
		scan[i] = max_coeffs == 4U ? coeffs[i] : coeffs[zigzag4[i + start_idx]];
		if (scan[i] != 0) {
			total_coeff++;
			last_nz = (int)i;
		}
	}
	*out_nz = (uint8_t)total_coeff;
	trailing_ones = 0U;
	t1_signs = 0U;
	if (total_coeff > 0U) {
		for (i = (unsigned)last_nz; (int)i >= 0 && trailing_ones < 3U; i--) {
			if (scan[i] == 0)
				continue;
			if (scan[i] == 1 || scan[i] == -1) {
				t1_signs = (t1_signs << 1) | (scan[i] < 0 ? 1U : 0U);
				trailing_ones++;
			} else {
				break;
			}
		}
	}

	/* Emits coeff_token from ITU-T H.264 Table 9-5. */
	target_val = (total_coeff << 2) | trailing_ones;
	if (nc < 0) {
		for (idx = 0U; idx < H264SW_COEFF_TOKEN_CHROMA_DC_CODES; idx++) {
			if (h264sw_cavlc_coeff_token_chroma_dc[idx].value == target_val) {
				bw_put_bits(bw,
					    h264sw_cavlc_coeff_token_chroma_dc[idx].code,
					    h264sw_cavlc_coeff_token_chroma_dc[idx].length);
				break;
			}
		}
	} else {
		unsigned col;
		if (nc < 2)
			col = 0U;
		else if (nc < 4)
			col = 1U;
		else if (nc < 8)
			col = 2U;
		else
			col = 3U;
		for (idx = 0U; idx < H264SW_COEFF_TOKEN_CODES; idx++) {
			if (h264sw_cavlc_coeff_token[col][idx].value == target_val) {
				bw_put_bits(bw,
					    h264sw_cavlc_coeff_token[col][idx].code,
					    h264sw_cavlc_coeff_token[col][idx].length);
				break;
			}
		}
	}
	if (total_coeff == 0U)
		return;
	if (trailing_ones > 0U)
		bw_put_bits(bw, t1_signs, trailing_ones);

	/* Encodes remaining non-zero levels in reverse scan order. */
	suffix_len = (total_coeff > 10U && trailing_ones < 3U) ? 1 : 0;
	{
		unsigned skipped_t1;
		skipped_t1 = 0U;
		for (i = (unsigned)last_nz; (int)i >= 0; i--) {
			int32_t lv;
			int32_t abs_lv;
			uint32_t lv_code;
			uint32_t prefix;
			uint32_t suffix;
			if (scan[i] == 0)
				continue;
			if (skipped_t1 < trailing_ones) {
				skipped_t1++;
				continue;
			}
			lv = scan[i];
			abs_lv = lv < 0 ? -lv : lv;
			lv_code = lv > 0 ? (uint32_t)(lv * 2 - 2) :
					   (uint32_t)(-lv * 2 - 1);
			if (skipped_t1 == trailing_ones && trailing_ones < 3U)
				lv_code -= 2U;
			skipped_t1 = 4U;
			if (suffix_len == 0) {
				if (lv_code < 14U) {
					bw_put_bits(bw, 0U, lv_code);
					bw_put_bit(bw, 1U);
				} else if (lv_code < 30U) {
					bw_put_bits(bw, 0U, 14U);
					bw_put_bit(bw, 1U);
					bw_put_bits(bw, lv_code - 14U, 4U);
				} else {
					uint32_t rem;
					unsigned p;
					rem = lv_code - 30U;
					p = 15U;
					while (rem >= (1U << (p - 3U))) {
						rem -= (1U << (p - 3U));
						p++;
					}
					bw_put_bits(bw, 0U, p);
					bw_put_bit(bw, 1U);
					bw_put_bits(bw, rem, p - 3U);
				}
			} else {
				uint32_t esc_thresh;
				esc_thresh = 15U << (unsigned)suffix_len;
				if (lv_code < esc_thresh) {
					prefix = lv_code >> (unsigned)suffix_len;
					suffix = lv_code & ((1U << (unsigned)suffix_len) - 1U);
					bw_put_bits(bw, 0U, prefix);
					bw_put_bit(bw, 1U);
					bw_put_bits(bw, suffix, (unsigned)suffix_len);
				} else {
					uint32_t rem;
					unsigned p;
					rem = lv_code - esc_thresh;
					p = 15U;
					while (rem >= (1U << (p - 3U))) {
						rem -= (1U << (p - 3U));
						p++;
					}
					bw_put_bits(bw, 0U, p);
					bw_put_bit(bw, 1U);
					bw_put_bits(bw, rem, p - 3U);
				}
			}
			if (suffix_len == 0)
				suffix_len = 1;
			if (abs_lv > (3 << (suffix_len - 1)) && suffix_len < 6)
				suffix_len++;
		}
	}

	/* Encodes total_zeros and run_before when total_coeff < max_coeffs. */
	if (total_coeff < max_coeffs) {
		zeros_left = (unsigned)(last_nz + 1) - total_coeff;
		if (nc < 0) {
			const struct h264sw_vlc_code *c;
			c = &h264sw_cavlc_total_zeros_chroma_dc[total_coeff - 1U][zeros_left];
			bw_put_bits(bw, c->code, c->length);
		} else {
			const struct h264sw_vlc_code *c;
			c = &h264sw_cavlc_total_zeros[total_coeff - 1U][zeros_left];
			bw_put_bits(bw, c->code, c->length);
		}
		rem_coeff = total_coeff;
		for (i = (unsigned)last_nz; (int)i >= 0 && zeros_left > 0U && rem_coeff > 1U; i--) {
			const struct h264sw_vlc_code *c;
			unsigned row;
			if (scan[i] == 0)
				continue;
			run_before = 0U;
			while ((int)(i - 1U - run_before) >= 0 &&
			       scan[i - 1U - run_before] == 0) {
				run_before++;
			}
			row = zeros_left >= 7U ? 6U : zeros_left - 1U;
			c = &h264sw_cavlc_run_before[row][run_before];
			bw_put_bits(bw, c->code, c->length);
			zeros_left -= run_before;
			rem_coeff--;
		}
	}
}

/* Forward 4x4 integer H.264 transform. */
static void
fwd_4x4(
	const int16_t *in,
	int16_t *out)
{
	int32_t t[16];
	int i;
	for (i = 0; i < 4; i++) {
		int32_t s0;
		int32_t s1;
		int32_t d0;
		int32_t d1;
		s0 = (int32_t)in[i * 4 + 0] + (int32_t)in[i * 4 + 3];
		s1 = (int32_t)in[i * 4 + 1] + (int32_t)in[i * 4 + 2];
		d0 = (int32_t)in[i * 4 + 0] - (int32_t)in[i * 4 + 3];
		d1 = (int32_t)in[i * 4 + 1] - (int32_t)in[i * 4 + 2];
		t[i * 4 + 0] = s0 + s1;
		t[i * 4 + 1] = (d0 * 2) + d1;
		t[i * 4 + 2] = s0 - s1;
		t[i * 4 + 3] = d0 - (d1 * 2);
	}
	for (i = 0; i < 4; i++) {
		int32_t s0;
		int32_t s1;
		int32_t d0;
		int32_t d1;
		s0 = t[0 * 4 + i] + t[3 * 4 + i];
		s1 = t[1 * 4 + i] + t[2 * 4 + i];
		d0 = t[0 * 4 + i] - t[3 * 4 + i];
		d1 = t[1 * 4 + i] - t[2 * 4 + i];
		out[0 * 4 + i] = (int16_t)(s0 + s1);
		out[1 * 4 + i] = (int16_t)((d0 * 2) + d1);
		out[2 * 4 + i] = (int16_t)(s0 - s1);
		out[3 * 4 + i] = (int16_t)(d0 - (d1 * 2));
	}
}

/* Inverse 4x4 integer H.264 transform. */
static void
inv_4x4(
	const int16_t *in,
	int16_t *out)
{
	int32_t t[16];
	int i;
	for (i = 0; i < 4; i++) {
		int32_t e0;
		int32_t e1;
		int32_t e2;
		int32_t e3;
		e0 = (int32_t)in[i * 4 + 0] + (int32_t)in[i * 4 + 2];
		e1 = (int32_t)in[i * 4 + 0] - (int32_t)in[i * 4 + 2];
		e2 = ((int32_t)in[i * 4 + 1] >> 1) - (int32_t)in[i * 4 + 3];
		e3 = (int32_t)in[i * 4 + 1] + ((int32_t)in[i * 4 + 3] >> 1);
		t[i * 4 + 0] = e0 + e3;
		t[i * 4 + 1] = e1 + e2;
		t[i * 4 + 2] = e1 - e2;
		t[i * 4 + 3] = e0 - e3;
	}
	for (i = 0; i < 4; i++) {
		int32_t e0;
		int32_t e1;
		int32_t e2;
		int32_t e3;
		e0 = t[0 * 4 + i] + t[2 * 4 + i];
		e1 = t[0 * 4 + i] - t[2 * 4 + i];
		e2 = (t[1 * 4 + i] >> 1) - t[3 * 4 + i];
		e3 = t[1 * 4 + i] + (t[3 * 4 + i] >> 1);
		out[0 * 4 + i] = (int16_t)((e0 + e3 + 32) >> 6);
		out[1 * 4 + i] = (int16_t)((e1 + e2 + 32) >> 6);
		out[2 * 4 + i] = (int16_t)((e1 - e2 + 32) >> 6);
		out[3 * 4 + i] = (int16_t)((e0 - e3 + 32) >> 6);
	}
}

/* Quantizes a 4x4 transformed block. */
static void
quant_4x4(
	int16_t *blk,
	uint32_t qp,
	int is_intra,
	int skip_dc)
{
	uint32_t qbits;
	uint32_t qp6;
	uint32_t f;
	int i;
	qbits = 15U + qp / 6U;
	qp6 = qp % 6U;
	f = (1U << qbits) / (is_intra != 0 ? 3U : 6U);
	for (i = skip_dc != 0 ? 1 : 0; i < 16; i++) {
		int32_t v;
		int32_t mag;
		v = (int32_t)blk[i];
		mag = v < 0 ? -v : v;
		mag = (int32_t)(((uint32_t)mag * quant_mf[qp6][pos_class[i]] + f) >> qbits);
		blk[i] = (int16_t)(v < 0 ? -mag : mag);
	}
}

/* Dequantizes a 4x4 block. */
static void
dequant_4x4(
	int16_t *blk,
	uint32_t qp,
	int skip_dc)
{
	uint32_t shift;
	uint32_t qp6;
	int i;
	shift = qp / 6U;
	qp6 = qp % 6U;
	for (i = skip_dc != 0 ? 1 : 0; i < 16; i++)
		blk[i] = (int16_t)(((int32_t)blk[i] * (int32_t)dequant_v[qp6][pos_class[i]]) * (int32_t)(1U << shift));
}

/* Clips an integer sample to [0, 255]. */
static uint8_t
clip_u8(
	int32_t v)
{
	if (v < 0)
		return 0U;
	if (v > 255)
		return 255U;
	return (uint8_t)v;
}
