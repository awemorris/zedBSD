/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Gen12 VDEnc + MFX H.264 CQP command builder and Annex-B RBSP packer
 * (see vk-video-vdenc.h).
 */

#include "vk-video-vdenc.h"
#include "vk-batch.h"

#include "../intel/genxml-video.h"

#include <stddef.h>
#include <stdint.h>

/* Bit writer for H.264 RBSP construction before emulation prevention. */
struct i915_vdenc_bw {
	uint8_t *data;
	size_t cap;
	size_t byte_pos;
	uint8_t cur;
	unsigned bit_pos;
	int overflow;
};

static void vdenc_bw_init(struct i915_vdenc_bw *bw, uint8_t *buf, size_t cap);
static void vdenc_bw_put_bit(struct i915_vdenc_bw *bw, unsigned bit);
static void vdenc_bw_put_bits(struct i915_vdenc_bw *bw, uint32_t val, unsigned n);
static void vdenc_bw_put_ue(struct i915_vdenc_bw *bw, uint32_t val);
static void vdenc_bw_put_se(struct i915_vdenc_bw *bw, int32_t val);
static void vdenc_bw_trailing(struct i915_vdenc_bw *bw);
static size_t vdenc_rbsp_to_annexb(const uint8_t *rbsp, size_t rbsp_len, uint8_t nal_hdr, uint8_t *out, size_t cap);

static void vdenc_emit_addr(struct i915_gfx_batch *batch, uint64_t addr);
static void vdenc_emit_buf(struct i915_gfx_batch *batch, uint64_t addr, uint32_t mocs);
static void vdenc_emit_zeros(struct i915_gfx_batch *batch, unsigned count);
static void vdenc_emit_head(struct i915_gfx_batch *batch);
static void vdenc_emit_mfx_surface(struct i915_gfx_batch *batch, uint32_t id, const struct i915_video_vdenc_encode *enc);
static void vdenc_emit_mfx_bufs(struct i915_gfx_batch *batch, const struct i915_video_vdenc_encode *enc);
static void vdenc_emit_mfx_ind_obj(struct i915_gfx_batch *batch, const struct i915_video_vdenc_encode *enc);
static void vdenc_emit_mfx_bsp_buf(struct i915_gfx_batch *batch, const struct i915_video_vdenc_encode *enc);
static void vdenc_emit_mfx_qm(struct i915_gfx_batch *batch);
static void vdenc_emit_mfx_fqm(struct i915_gfx_batch *batch);
static void vdenc_emit_mfx_avc_img(struct i915_gfx_batch *batch, const struct i915_video_vdenc_encode *enc);
static void vdenc_emit_mfx_ref_idx(struct i915_gfx_batch *batch, const struct i915_video_vdenc_encode *enc);
static void vdenc_emit_vdenc_pipe_mode(struct i915_gfx_batch *batch, const struct i915_video_vdenc_encode *enc);
static void vdenc_emit_vdenc_surface(struct i915_gfx_batch *batch, uint32_t opcode, const struct i915_video_vdenc_encode *enc);
static void vdenc_emit_vdenc_bufs(struct i915_gfx_batch *batch, const struct i915_video_vdenc_encode *enc);
static void vdenc_emit_vdenc_const_qpt(struct i915_gfx_batch *batch);
static void vdenc_emit_vdenc_img(struct i915_gfx_batch *batch, const struct i915_video_vdenc_encode *enc);
static void vdenc_emit_vdenc_cost(struct i915_gfx_batch *batch, int32_t qp);
static void vdenc_emit_vdenc_cmd1(struct i915_gfx_batch *batch, const struct i915_video_vdenc_encode *enc);
static void vdenc_emit_vdenc_cmd2(struct i915_gfx_batch *batch, const struct i915_video_vdenc_encode *enc);
static void vdenc_emit_pak_insert_slice(struct i915_gfx_batch *batch, const uint8_t *hdr, size_t hdr_bits);
static void vdenc_emit_mfx_slice(struct i915_gfx_batch *batch, const struct i915_video_vdenc_encode *enc);
static void vdenc_emit_vdenc_walker(struct i915_gfx_batch *batch, const struct i915_video_vdenc_encode *enc);
static void vdenc_emit_tail(struct i915_gfx_batch *batch, const struct i915_video_vdenc_encode *enc);

/*
 * Checks SPS and PPS values against what the Gen12 VDEnc + MFX CQP encoder supports.
 * Returns NULL on acceptance, or a short reason string on refusal.
 */
const char *
drv_i915_video_vdenc_check_sets(
	const struct i915_video_sps *sps,
	const struct i915_video_pps *pps)
{
	uint32_t width_mbs;
	uint32_t height_mbs;

	/* Only 8-bit 4:2:0 progressive frames are supported. */
	if (sps->chroma_format_idc != 1U)
		return "chroma format is not 4:2:0";
	if (sps->bit_depth_luma_minus8 != 0U || sps->bit_depth_chroma_minus8 != 0U)
		return "bit depth is not 8";
	if ((sps->flags & I915_VIDEO_SPS_FRAME_MBS_ONLY) == 0U)
		return "interlaced coding not supported";
	if (sps->pic_order_cnt_type != 0U && sps->pic_order_cnt_type != 2U)
		return "unsupported pic_order_cnt_type";

	/* Level 5.1 frame size ceiling (4096x4096, 36864 MBs). */
	width_mbs = sps->pic_width_in_mbs_minus1 + 1U;
	height_mbs = sps->pic_height_in_map_units_minus1 + 1U;
	if (width_mbs > I915_VIDEO_MFX_MAX_SIDE_MBS || height_mbs > I915_VIDEO_MFX_MAX_SIDE_MBS)
		return "picture dimension past 4096";
	if (width_mbs * height_mbs > I915_VIDEO_MFX_MAX_FRAME_MBS)
		return "picture past 36864 macroblocks";

	/* PPS quantizer and reference bounds. */
	if (pps->pic_init_qp_minus26 < -26 || pps->pic_init_qp_minus26 > 25)
		return "pic_init_qp_minus26 out of range";
	if (pps->chroma_qp_index_offset < -12 || pps->chroma_qp_index_offset > 12)
		return "chroma_qp_index_offset out of range";
	if (pps->num_ref_idx_l0_default_active_minus1 > 15U)
		return "num_ref_idx_l0_default_active_minus1 past 15";

	return NULL;
}

/*
 * Packs one H.264 SPS into an Annex-B NAL unit (start code + 0x67 + RBSP).
 * Returns 0 on success, -1 if the output buffer is too small.
 */
int
drv_i915_video_vdenc_pack_sps(
	const struct i915_video_sps *sps,
	uint8_t *out,
	size_t cap,
	size_t *out_len)
{
	uint8_t rbsp[192];
	struct i915_vdenc_bw bw;
	uint32_t constraint_bits;
	int has_crop;
	size_t bytes;

	vdenc_bw_init(&bw, rbsp, sizeof(rbsp));

	/* profile_idc, constraint_set0..5_flags (from sps->flags bits 0..5), level_idc. */
	vdenc_bw_put_bits(&bw, sps->profile_idc, 8U);
	constraint_bits = (sps->flags & 0x3fU) << 2;
	vdenc_bw_put_bits(&bw, constraint_bits, 8U);
	vdenc_bw_put_bits(&bw, sps->level_idc, 8U);
	vdenc_bw_put_ue(&bw, sps->id);

	/* High profile syntax elements (chroma_format_idc, bit depths, qpprime, seq_scaling_matrix). */
	if (sps->profile_idc == 100U || sps->profile_idc == 110U ||
	    sps->profile_idc == 122U || sps->profile_idc == 244U) {
		vdenc_bw_put_ue(&bw, sps->chroma_format_idc);
		vdenc_bw_put_ue(&bw, sps->bit_depth_luma_minus8);
		vdenc_bw_put_ue(&bw, sps->bit_depth_chroma_minus8);
		vdenc_bw_put_bit(&bw, (sps->flags >> 13) & 1U);
		vdenc_bw_put_bit(&bw, 0U);
	}

	/* Frame numbering and POC configuration. */
	vdenc_bw_put_ue(&bw, sps->log2_max_frame_num_minus4);
	vdenc_bw_put_ue(&bw, sps->pic_order_cnt_type);
	if (sps->pic_order_cnt_type == 0U)
		vdenc_bw_put_ue(&bw, sps->log2_max_pic_order_cnt_lsb_minus4);

	vdenc_bw_put_ue(&bw, sps->max_num_ref_frames);
	vdenc_bw_put_bit(&bw, (sps->flags >> 11) & 1U);
	vdenc_bw_put_ue(&bw, sps->pic_width_in_mbs_minus1);
	vdenc_bw_put_ue(&bw, sps->pic_height_in_map_units_minus1);
	vdenc_bw_put_bit(&bw, 1U);
	vdenc_bw_put_bit(&bw, (sps->flags & I915_VIDEO_SPS_DIRECT_8X8_INFERENCE) != 0U ? 1U : 0U);

	/* Frame cropping rectangle. */
	has_crop = (sps->crop[0] | sps->crop[1] | sps->crop[2] | sps->crop[3]) != 0U;
	vdenc_bw_put_bit(&bw, has_crop ? 1U : 0U);
	if (has_crop) {
		vdenc_bw_put_ue(&bw, sps->crop[0]);
		vdenc_bw_put_ue(&bw, sps->crop[1]);
		vdenc_bw_put_ue(&bw, sps->crop[2]);
		vdenc_bw_put_ue(&bw, sps->crop[3]);
	}

	/* vui_parameters_present_flag = 0, then rbsp_trailing_bits. */
	vdenc_bw_put_bit(&bw, 0U);
	vdenc_bw_trailing(&bw);
	if (bw.overflow)
		return -1;

	bytes = vdenc_rbsp_to_annexb(rbsp, bw.byte_pos, 0x67U, out, cap);
	if (bytes == 0U)
		return -1;

	*out_len = bytes;
	return 0;
}

/*
 * Packs one H.264 PPS into an Annex-B NAL unit (start code + 0x68 + RBSP).
 * Returns 0 on success, -1 if the output buffer is too small.
 */
int
drv_i915_video_vdenc_pack_pps(
	const struct i915_video_pps *pps,
	uint8_t *out,
	size_t cap,
	size_t *out_len)
{
	uint8_t rbsp[64];
	struct i915_vdenc_bw bw;
	size_t bytes;

	vdenc_bw_init(&bw, rbsp, sizeof(rbsp));

	vdenc_bw_put_ue(&bw, pps->pps_id);
	vdenc_bw_put_ue(&bw, pps->sps_id);
	vdenc_bw_put_bit(&bw, (pps->flags & I915_VIDEO_PPS_CABAC) != 0U ? 1U : 0U);
	vdenc_bw_put_bit(&bw, (pps->flags & I915_VIDEO_PPS_BOTTOM_FIELD_POC) != 0U ? 1U : 0U);
	vdenc_bw_put_ue(&bw, 0U);
	vdenc_bw_put_ue(&bw, pps->num_ref_idx_l0_default_active_minus1);
	vdenc_bw_put_ue(&bw, pps->num_ref_idx_l1_default_active_minus1);
	vdenc_bw_put_bit(&bw, (pps->flags & I915_VIDEO_PPS_WEIGHTED_PRED) != 0U ? 1U : 0U);
	vdenc_bw_put_bits(&bw, pps->weighted_bipred_idc, 2U);
	vdenc_bw_put_se(&bw, pps->pic_init_qp_minus26);
	vdenc_bw_put_se(&bw, pps->pic_init_qs_minus26);
	vdenc_bw_put_se(&bw, pps->chroma_qp_index_offset);
	vdenc_bw_put_bit(&bw, (pps->flags & I915_VIDEO_PPS_DEBLOCKING_CONTROL) != 0U ? 1U : 0U);
	vdenc_bw_put_bit(&bw, (pps->flags & I915_VIDEO_PPS_CONSTRAINED_INTRA) != 0U ? 1U : 0U);
	vdenc_bw_put_bit(&bw, (pps->flags & I915_VIDEO_PPS_REDUNDANT_PIC_COUNT) != 0U ? 1U : 0U);

	/* Emit transform_8x8_mode_flag and second_chroma_qp_index_offset when 8x8 is enabled. */
	if ((pps->flags & I915_VIDEO_PPS_TRANSFORM_8X8) != 0U ||
	    pps->second_chroma_qp_index_offset != pps->chroma_qp_index_offset) {
		vdenc_bw_put_bit(&bw, (pps->flags & I915_VIDEO_PPS_TRANSFORM_8X8) != 0U ? 1U : 0U);
		vdenc_bw_put_bit(&bw, 0U);
		vdenc_bw_put_se(&bw, pps->second_chroma_qp_index_offset);
	}

	vdenc_bw_trailing(&bw);
	if (bw.overflow)
		return -1;

	bytes = vdenc_rbsp_to_annexb(rbsp, bw.byte_pos, 0x68U, out, cap);
	if (bytes == 0U)
		return -1;

	*out_len = bytes;
	return 0;
}

/*
 * Packs the H.264 NAL unit start code + NAL header + slice_header() bits for
 * MFX_PAK_INSERT_OBJECT.  Returns 0 on success and stores the total bit count
 * in *out_bits.
 */
int
drv_i915_video_vdenc_pack_slice_header(
	const struct i915_video_vdenc_encode *enc,
	uint8_t *out,
	size_t cap,
	size_t *out_bits)
{
	struct i915_vdenc_bw bw;
	uint8_t nal_hdr;
	uint32_t frame_num_bits;
	uint32_t poc_lsb_bits;
	int32_t pic_init_qp;
	int32_t qp_delta;

	/* Start code (4 bytes) + NAL header (1 byte) + slice_header bits. */
	if (cap < 8U)
		return -1;

	out[0] = 0x00U;
	out[1] = 0x00U;
	out[2] = 0x00U;
	out[3] = 0x01U;
	if (enc->is_idr)
		nal_hdr = 0x65U;
	else if (enc->is_reference)
		nal_hdr = 0x41U;
	else
		nal_hdr = 0x01U;
	out[4] = nal_hdr;

	vdenc_bw_init(&bw, out + 5U, cap - 5U);

	/* first_mb_in_slice, slice_type, pic_parameter_set_id. */
	vdenc_bw_put_ue(&bw, enc->slice.first_mb_in_slice);
	vdenc_bw_put_ue(&bw, enc->slice.slice_type);
	vdenc_bw_put_ue(&bw, enc->pps->pps_id);

	/* frame_num (log2_max_frame_num_minus4 + 4 bits). */
	frame_num_bits = enc->sps->log2_max_frame_num_minus4 + 4U;
	vdenc_bw_put_bits(&bw, enc->frame_num, frame_num_bits);

	/* idr_pic_id for IDR frames. */
	if (enc->is_idr)
		vdenc_bw_put_ue(&bw, enc->idr_pic_id);

	/* pic_order_cnt_lsb for POC type 0. */
	if (enc->sps->pic_order_cnt_type == 0U) {
		poc_lsb_bits = enc->sps->log2_max_pic_order_cnt_lsb_minus4 + 4U;
		vdenc_bw_put_bits(&bw, (uint32_t)enc->poc, poc_lsb_bits);
	}

	/* num_ref_idx_active_override_flag for P slices. */
	if (enc->slice.slice_type == I915_VIDEO_SLICE_P) {
		vdenc_bw_put_bit(&bw, 1U);
		vdenc_bw_put_ue(&bw, enc->slice.num_ref_idx_l0_active_minus1);
		/* ref_pic_list_modification_flag_l0 = 0. */
		vdenc_bw_put_bit(&bw, 0U);
	}

	/* dec_ref_pic_marking for reference pictures. */
	if (enc->is_idr) {
		vdenc_bw_put_bit(&bw, 0U);
		vdenc_bw_put_bit(&bw, 0U);
	} else if (enc->is_reference) {
		vdenc_bw_put_bit(&bw, 0U);
	}

	/* cabac_init_idc for P slices when CABAC is active. */
	if ((enc->pps->flags & I915_VIDEO_PPS_CABAC) != 0U &&
	    enc->slice.slice_type != I915_VIDEO_SLICE_I)
		vdenc_bw_put_ue(&bw, enc->slice.cabac_init_idc);

	/* slice_qp_delta = slice.qp - (26 + pic_init_qp_minus26). */
	pic_init_qp = 26 + enc->pps->pic_init_qp_minus26;
	qp_delta = enc->slice.qp - pic_init_qp;
	vdenc_bw_put_se(&bw, qp_delta);

	/* Deblocking filter control if signaled in PPS. */
	if ((enc->pps->flags & I915_VIDEO_PPS_DEBLOCKING_CONTROL) != 0U) {
		vdenc_bw_put_ue(&bw, enc->slice.disable_deblocking_filter_idc);
		if (enc->slice.disable_deblocking_filter_idc != 1U) {
			vdenc_bw_put_se(&bw, enc->slice.slice_alpha_c0_offset_div2);
			vdenc_bw_put_se(&bw, enc->slice.slice_beta_offset_div2);
		}
	}

	if (bw.overflow)
		return -1;

	*out_bits = 40U + bw.byte_pos * 8U + bw.bit_pos;
	return 0;
}

/*
 * Writes the Gen12 VDEnc + MFX commands that encode one H.264 frame into a batch.
 */
void
drv_i915_video_vdenc_build(
	struct i915_gfx_batch *batch,
	const struct i915_video_vdenc_encode *enc)
{
	uint8_t slice_hdr[I915_VIDEO_VDENC_SLICE_HDR_MAX];
	size_t slice_bits;

	/* Flush, power well wakeup (MFX + VDEnc), and MFX_PIPE_MODE_SELECT. */
	vdenc_emit_head(batch);

	/* MFX surface states: ID 0 reconstructed DPB, ID 4 original source. */
	vdenc_emit_mfx_surface(batch, 0U, enc);
	vdenc_emit_mfx_surface(batch, 4U, enc);

	/* MFX pipe buffer addresses, indirect PAK bitstream, and row stores. */
	vdenc_emit_mfx_bufs(batch, enc);
	vdenc_emit_mfx_ind_obj(batch, enc);
	vdenc_emit_mfx_bsp_buf(batch, enc);

	/* Forward and inverse quantizer matrices. */
	vdenc_emit_mfx_qm(batch);
	vdenc_emit_mfx_fqm(batch);

	/* MFX AVC image state and L0 reference index state for P frames. */
	vdenc_emit_mfx_avc_img(batch, enc);
	if (enc->slice.slice_type == I915_VIDEO_SLICE_P)
		vdenc_emit_mfx_ref_idx(batch, enc);

	/* VDEnc pipeline setup. */
	vdenc_emit_vdenc_pipe_mode(batch, enc);
	vdenc_emit_vdenc_surface(batch, GEN12_VIDEO_VDENC_SRC_SURFACE_STATE, enc);
	vdenc_emit_vdenc_surface(batch, GEN12_VIDEO_VDENC_REF_SURFACE_STATE, enc);
	vdenc_emit_vdenc_surface(batch, GEN12_VIDEO_VDENC_DS_REF_SURFACE_STATE, enc);
	vdenc_emit_vdenc_bufs(batch, enc);
	vdenc_emit_vdenc_const_qpt(batch);
	vdenc_emit_vdenc_img(batch, enc);
	vdenc_emit_vdenc_cost(batch, enc->slice.qp);
	vdenc_emit_vdenc_cmd1(batch, enc);
	vdenc_emit_vdenc_cmd2(batch, enc);

	/* Pack and insert the slice header inline via MFX_PAK_INSERT_OBJECT. */
	slice_bits = 0U;
	if (drv_i915_video_vdenc_pack_slice_header(enc, slice_hdr, sizeof(slice_hdr), &slice_bits) == 0)
		vdenc_emit_pak_insert_slice(batch, slice_hdr, slice_bits);

	/* MFX slice state, VDEnc walker trigger, pipeline flush, and bytecount readback. */
	vdenc_emit_mfx_slice(batch, enc);
	vdenc_emit_vdenc_walker(batch, enc);
	vdenc_emit_tail(batch, enc);
}

static void
vdenc_bw_init(
	struct i915_vdenc_bw *bw,
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

static void
vdenc_bw_put_bit(
	struct i915_vdenc_bw *bw,
	unsigned bit)
{
	if (bw->overflow)
		return;
	if (bit != 0U)
		bw->cur |= (uint8_t)(1U << (7U - bw->bit_pos));
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

static void
vdenc_bw_put_bits(
	struct i915_vdenc_bw *bw,
	uint32_t val,
	unsigned n)
{
	unsigned i;

	for (i = 0U; i < n; i++)
		vdenc_bw_put_bit(bw, (val >> (n - 1U - i)) & 1U);
}

static void
vdenc_bw_put_ue(
	struct i915_vdenc_bw *bw,
	uint32_t val)
{
	uint32_t code;
	unsigned len;
	unsigned i;

	code = val + 1U;
	len = 0U;
	while ((code >> len) != 0U)
		len++;
	for (i = 0U; i + 1U < len; i++)
		vdenc_bw_put_bit(bw, 0U);
	vdenc_bw_put_bits(bw, code, len);
}

static void
vdenc_bw_put_se(
	struct i915_vdenc_bw *bw,
	int32_t val)
{
	uint32_t mapped;

	if (val <= 0)
		mapped = (uint32_t)(-val) * 2U;
	else
		mapped = (uint32_t)val * 2U - 1U;
	vdenc_bw_put_ue(bw, mapped);
}

static void
vdenc_bw_trailing(
	struct i915_vdenc_bw *bw)
{
	vdenc_bw_put_bit(bw, 1U);
	while (bw->bit_pos != 0U && !bw->overflow)
		vdenc_bw_put_bit(bw, 0U);
}

static size_t
vdenc_rbsp_to_annexb(
	const uint8_t *rbsp,
	size_t rbsp_len,
	uint8_t nal_hdr,
	uint8_t *out,
	size_t cap)
{
	size_t pos;
	size_t i;
	unsigned zeros;

	if (cap < 5U)
		return 0U;
	out[0] = 0x00U;
	out[1] = 0x00U;
	out[2] = 0x00U;
	out[3] = 0x01U;
	out[4] = nal_hdr;
	pos = 5U;
	zeros = 0U;

	for (i = 0U; i < rbsp_len; i++) {
		if (zeros == 2U && rbsp[i] <= 0x03U) {
			if (pos >= cap)
				return 0U;
			out[pos++] = 0x03U;
			zeros = 0U;
		}
		if (pos >= cap)
			return 0U;
		out[pos++] = rbsp[i];
		if (rbsp[i] == 0x00U)
			zeros++;
		else
			zeros = 0U;
	}

	return pos;
}

static void
vdenc_emit_addr(
	struct i915_gfx_batch *batch,
	uint64_t addr)
{
	drv_i915_batch_emit(batch, (uint32_t)addr);
	drv_i915_batch_emit(batch, (uint32_t)(addr >> 32));
}

static void
vdenc_emit_buf(
	struct i915_gfx_batch *batch,
	uint64_t addr,
	uint32_t mocs)
{
	vdenc_emit_addr(batch, addr);
	drv_i915_batch_emit(batch, mocs & GEN12_VIDEO_ATTRIBUTES_MOCS_MASK);
}

static void
vdenc_emit_zeros(
	struct i915_gfx_batch *batch,
	unsigned count)
{
	unsigned index;

	for (index = 0U; index < count; index++)
		drv_i915_batch_emit(batch, 0U);
}

static void
vdenc_emit_head(
	struct i915_gfx_batch *batch)
{
	uint32_t wake_bits;

	drv_i915_batch_emit(batch, GEN12_VIDEO_MI_FLUSH_DW | GEN12_VIDEO_MI_FLUSH_DW_INVALIDATE);
	vdenc_emit_zeros(batch, GEN12_VIDEO_MI_FLUSH_DW_DWORDS - 1U);

	/* Wake both MFX and VDEnc power wells. */
	wake_bits = GEN12_VIDEO_FORCE_WAKEUP_MFX | GEN12_VIDEO_FORCE_WAKEUP_VDENC |
		    (GEN12_VIDEO_FORCE_WAKEUP_MASK << GEN12_VIDEO_FORCE_WAKEUP_MASK_SHIFT) |
		    GEN12_VIDEO_FORCE_WAKEUP_VDENC_MASK;
	drv_i915_batch_emit(batch, GEN12_VIDEO_MI_FORCE_WAKEUP);
	drv_i915_batch_emit(batch, wake_bits);

	drv_i915_batch_emit(batch, GEN12_VIDEO_MFX_WAIT | GEN12_VIDEO_MFX_WAIT_SYNC);

	/* MFX_PIPE_MODE_SELECT: Standard AVC (2), Codec Select Encode (1), Post-Deblock + StreamOut + VDEnc. */
	drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(GEN12_VIDEO_MFX_PIPE_MODE_SELECT,
						      GEN12_VIDEO_MFX_PIPE_MODE_SELECT_DWORDS));
	drv_i915_batch_emit(batch, GEN12_VIDEO_MODE_STANDARD_AVC |
				   GEN12_VIDEO_MODE_CODEC_ENCODE |
				   GEN12_VIDEO_MODE_POST_DEBLOCKING |
				   GEN12_VIDEO_MODE_STREAM_OUT |
				   GEN12_VIDEO_MODE_VDENC);
	vdenc_emit_zeros(batch, GEN12_VIDEO_MFX_PIPE_MODE_SELECT_DWORDS - 2U);

	drv_i915_batch_emit(batch, GEN12_VIDEO_MFX_WAIT | GEN12_VIDEO_MFX_WAIT_SYNC);
}

static void
vdenc_emit_mfx_surface(
	struct i915_gfx_batch *batch,
	uint32_t id,
	const struct i915_video_vdenc_encode *enc)
{
	drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(GEN12_VIDEO_MFX_SURFACE_STATE,
						      GEN12_VIDEO_MFX_SURFACE_STATE_DWORDS));
	drv_i915_batch_emit(batch, id & 0xfU);
	drv_i915_batch_emit(batch, ((enc->width - 1U) << GEN12_VIDEO_SURFACE_WIDTH_SHIFT) |
				   ((enc->height - 1U) << GEN12_VIDEO_SURFACE_HEIGHT_SHIFT));
	drv_i915_batch_emit(batch, GEN12_VIDEO_SURFACE_WALK_YMAJOR |
				   GEN12_VIDEO_SURFACE_TILED |
				   ((enc->pitch - 1U) << GEN12_VIDEO_SURFACE_PITCH_SHIFT) |
				   GEN12_VIDEO_SURFACE_INTERLEAVE_CHROMA |
				   (GEN12_VIDEO_SURFACE_PLANAR_420_8 << GEN12_VIDEO_SURFACE_FORMAT_SHIFT));
	drv_i915_batch_emit(batch, enc->chroma_rows & 0x7fffU);
	drv_i915_batch_emit(batch, 0U);
}

static void
vdenc_emit_mfx_bufs(
	struct i915_gfx_batch *batch,
	const struct i915_video_vdenc_encode *enc)
{
	uint32_t index;
	uint64_t ref_addr;

	drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(GEN12_VIDEO_MFX_PIPE_BUF_ADDR_STATE,
						      GEN12_VIDEO_MFX_PIPE_BUF_ADDR_STATE_DWORDS));
	/* Dword 1..3: Pre-Deblocking (unused = reconstructed), 4..6: Post-Deblocking (reconstructed). */
	vdenc_emit_buf(batch, enc->reconstructed, enc->mocs);
	vdenc_emit_buf(batch, enc->reconstructed, enc->mocs);
	/* Dword 7..9: Original Source Picture. */
	vdenc_emit_buf(batch, enc->source, enc->mocs);
	/* Dword 10..12: StreamOut. */
	vdenc_emit_buf(batch, enc->streamout_buffer, enc->mocs);
	/* Dword 13..15: Intra Row Store, 16..18: Deblocking Row Store. */
	vdenc_emit_buf(batch, enc->row_stores[0], enc->mocs);
	vdenc_emit_buf(batch, enc->row_stores[1], enc->mocs);

	/* Dword 19..50: 16 reference picture addresses, then dword 51: reference attributes. */
	for (index = 0U; index < I915_VIDEO_MFX_REFERENCES; index++) {
		if (index < enc->reference_count)
			ref_addr = enc->references[index].picture;
		else
			ref_addr = enc->reconstructed;
		vdenc_emit_addr(batch, ref_addr);
	}
	drv_i915_batch_emit(batch, enc->mocs & GEN12_VIDEO_ATTRIBUTES_MOCS_MASK);

	/* Dword 52..54: MB Status buffer. */
	vdenc_emit_buf(batch, enc->mb_status_buffer, enc->mocs);
	/* Dword 55..64: ILDB, compression, scaled reference (unused). */
	vdenc_emit_zeros(batch, GEN12_VIDEO_MFX_PIPE_BUF_ADDR_STATE_DWORDS - 55U);
}

static void
vdenc_emit_mfx_ind_obj(
	struct i915_gfx_batch *batch,
	const struct i915_video_vdenc_encode *enc)
{
	drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(GEN12_VIDEO_MFX_IND_OBJ_BASE_ADDR_STATE,
						      GEN12_VIDEO_MFX_IND_OBJ_BASE_ADDR_STATE_DWORDS));
	/* Object 0..2 (indirect bitstream/MV/IT coeff unused): 15 zero dwords. */
	vdenc_emit_zeros(batch, 15U);
	/* Object 3 (dwords 16..20): Indirect PAK-BSE Output Object base, attrs, upper bound. */
	vdenc_emit_buf(batch, enc->bitstream_base, enc->mocs);
	vdenc_emit_addr(batch, enc->bitstream_end);
	/* Object 4 (dwords 21..25): unused. */
	vdenc_emit_zeros(batch, 5U);
}

static void
vdenc_emit_mfx_bsp_buf(
	struct i915_gfx_batch *batch,
	const struct i915_video_vdenc_encode *enc)
{
	drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(GEN12_VIDEO_MFX_BSP_BUF_BASE_ADDR_STATE,
						      GEN12_VIDEO_MFX_BSP_BUF_BASE_ADDR_STATE_DWORDS));
	vdenc_emit_buf(batch, enc->row_stores[2], enc->mocs);
	vdenc_emit_buf(batch, enc->row_stores[3], enc->mocs);
	vdenc_emit_zeros(batch, 3U);
}

static void
vdenc_emit_mfx_qm(
	struct i915_gfx_batch *batch)
{
	uint32_t kind;
	uint32_t index;

	/* Emit flat 16 inverse quantizer matrices for 4x4 intra/inter (and 8x8). */
	for (kind = 0U; kind < 4U; kind++) {
		drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(GEN12_VIDEO_MFX_QM_STATE,
							      GEN12_VIDEO_MFX_QM_STATE_DWORDS));
		drv_i915_batch_emit(batch, kind);
		for (index = 0U; index < 16U; index++)
			drv_i915_batch_emit(batch, 0x10101010U);
	}
}

static void
vdenc_emit_mfx_fqm(
	struct i915_gfx_batch *batch)
{
	uint32_t kind;
	uint32_t index;

	/* Emit flat forward quantizer matrices (16-bit reciprocal entries). */
	for (kind = 0U; kind < 4U; kind++) {
		drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(GEN12_VIDEO_MFX_FQM_STATE,
							      GEN12_VIDEO_MFX_FQM_STATE_DWORDS));
		drv_i915_batch_emit(batch, kind);
		for (index = 0U; index < 32U; index++)
			drv_i915_batch_emit(batch, 0x10001000U);
	}
}

static void
vdenc_emit_mfx_avc_img(
	struct i915_gfx_batch *batch,
	const struct i915_video_vdenc_encode *enc)
{
	uint32_t width_mbs;
	uint32_t height_mbs;
	uint32_t dw3;
	uint32_t dw4;
	uint32_t dw13;
	uint32_t dw14;
	uint32_t l0_active;

	width_mbs = enc->sps->pic_width_in_mbs_minus1 + 1U;
	height_mbs = enc->sps->pic_height_in_map_units_minus1 + 1U;

	dw3 = (((uint32_t)enc->pps->chroma_qp_index_offset & GEN12_VIDEO_IMG_QP_OFFSET_MASK) << GEN12_VIDEO_IMG_CHROMA_OFFSET_SHIFT) |
	      (((uint32_t)enc->pps->second_chroma_qp_index_offset & GEN12_VIDEO_IMG_QP_OFFSET_MASK) << GEN12_VIDEO_IMG_SECOND_CHROMA_SHIFT);

	dw4 = GEN12_VIDEO_IMG_FRAME_MB_ONLY |
	      (1U << GEN12_VIDEO_IMG_CHROMA_FORMAT_SHIFT);
	if ((enc->pps->flags & I915_VIDEO_PPS_TRANSFORM_8X8) != 0U)
		dw4 |= GEN12_VIDEO_IMG_TRANSFORM_8X8;
	if ((enc->sps->flags & I915_VIDEO_SPS_DIRECT_8X8_INFERENCE) != 0U)
		dw4 |= GEN12_VIDEO_IMG_DIRECT_8X8;
	if ((enc->pps->flags & I915_VIDEO_PPS_CONSTRAINED_INTRA) != 0U)
		dw4 |= GEN12_VIDEO_IMG_CONSTRAINED_INTRA;
	if (!enc->is_reference)
		dw4 |= GEN12_VIDEO_IMG_NON_REFERENCE;
	if ((enc->pps->flags & I915_VIDEO_PPS_CABAC) != 0U)
		dw4 |= GEN12_VIDEO_IMG_CABAC;

	l0_active = (enc->slice.slice_type == I915_VIDEO_SLICE_P) ? (enc->slice.num_ref_idx_l0_active_minus1 + 1U) : 0U;
	dw13 = ((uint32_t)enc->slice.qp & GEN12_VIDEO_IMG_QP_MASK) |
	       (l0_active << GEN12_VIDEO_IMG_L0_SHIFT) |
	       (enc->sps->max_num_ref_frames << GEN12_VIDEO_IMG_REFERENCES_SHIFT);

	dw14 = (enc->sps->pic_order_cnt_type << GEN12_VIDEO_IMG_POC_TYPE_SHIFT) |
	       (enc->sps->log2_max_frame_num_minus4 << GEN12_VIDEO_IMG_LOG2_FRAME_NUM_SHIFT) |
	       (enc->sps->log2_max_pic_order_cnt_lsb_minus4 << GEN12_VIDEO_IMG_LOG2_POC_LSB_SHIFT);
	if ((enc->pps->flags & I915_VIDEO_PPS_DEBLOCKING_CONTROL) != 0U)
		dw14 |= GEN12_VIDEO_IMG_DEBLOCKING_CONTROL;

	drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(GEN12_VIDEO_MFX_AVC_IMG_STATE,
						      GEN12_VIDEO_MFX_AVC_IMG_STATE_DWORDS));
	drv_i915_batch_emit(batch, width_mbs * height_mbs);
	drv_i915_batch_emit(batch, (enc->sps->pic_width_in_mbs_minus1 & 0xffU) |
				   ((enc->sps->pic_height_in_map_units_minus1 & 0xffU) << GEN12_VIDEO_IMG_HEIGHT_SHIFT));
	drv_i915_batch_emit(batch, dw3);
	drv_i915_batch_emit(batch, dw4);
	drv_i915_batch_emit(batch, GEN12_VIDEO_IMG_TRELLIS_CHROMA_DISABLE);
	vdenc_emit_zeros(batch, 7U);
	drv_i915_batch_emit(batch, dw13);
	drv_i915_batch_emit(batch, dw14);
	drv_i915_batch_emit(batch, (enc->frame_num & 0xffffU) << GEN12_VIDEO_IMG_FRAME_NUM_SHIFT);
	vdenc_emit_zeros(batch, GEN12_VIDEO_MFX_AVC_IMG_STATE_DWORDS - 16U);
}

static void
vdenc_emit_mfx_ref_idx(
	struct i915_gfx_batch *batch,
	const struct i915_video_vdenc_encode *enc)
{
	uint32_t entry0;

	/* L0 list (select 0), first entry = enc->slice.l0_slot. */
	entry0 = enc->slice.l0_slot & 0x1fU;
	drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(GEN12_VIDEO_MFX_AVC_REF_IDX_STATE,
						      GEN12_VIDEO_MFX_AVC_REF_IDX_STATE_DWORDS));
	drv_i915_batch_emit(batch, 0U);
	drv_i915_batch_emit(batch, entry0 | (0x80U << 8) | (0x80U << 16) | (0x80U << 24));
	vdenc_emit_zeros(batch, GEN12_VIDEO_MFX_AVC_REF_IDX_STATE_DWORDS - 3U);
}

static void
vdenc_emit_vdenc_pipe_mode(
	struct i915_gfx_batch *batch,
	const struct i915_video_vdenc_encode *enc)
{
	(void)enc;

	drv_i915_batch_emit(batch, GEN12_VIDEO_MFX_WAIT | GEN12_VIDEO_MFX_WAIT_SYNC);

	drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(GEN12_VIDEO_VDENC_CONTROL_STATE,
						      GEN12_VIDEO_VDENC_CONTROL_STATE_DWORDS));
	drv_i915_batch_emit(batch, 1U);

	drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(GEN12_VIDEO_VDENC_PIPE_MODE_SELECT,
						      GEN12_VIDEO_VDENC_PIPE_MODE_SELECT_DWORDS));
	/* Standard Select = AVC (2), Frame Statistics / StreamIn disabled for CQP. */
	drv_i915_batch_emit(batch, GEN12_VIDEO_MODE_STANDARD_AVC);
	vdenc_emit_zeros(batch, GEN12_VIDEO_VDENC_PIPE_MODE_SELECT_DWORDS - 2U);
}

static void
vdenc_emit_vdenc_surface(
	struct i915_gfx_batch *batch,
	uint32_t opcode,
	const struct i915_video_vdenc_encode *enc)
{
	drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(opcode,
						      GEN12_VIDEO_VDENC_SRC_SURFACE_STATE_DWORDS));
	drv_i915_batch_emit(batch, 0U);
	drv_i915_batch_emit(batch, ((enc->width - 1U) << GEN12_VIDEO_SURFACE_WIDTH_SHIFT) |
				   ((enc->height - 1U) << GEN12_VIDEO_SURFACE_HEIGHT_SHIFT));
	drv_i915_batch_emit(batch, GEN12_VIDEO_SURFACE_WALK_YMAJOR |
				   GEN12_VIDEO_SURFACE_TILED |
				   ((enc->pitch - 1U) << GEN12_VIDEO_SURFACE_PITCH_SHIFT) |
				   GEN12_VIDEO_SURFACE_INTERLEAVE_CHROMA |
				   (GEN12_VIDEO_SURFACE_PLANAR_420_8 << GEN12_VIDEO_SURFACE_FORMAT_SHIFT));
	drv_i915_batch_emit(batch, enc->chroma_rows & 0x7fffU);
	drv_i915_batch_emit(batch, 0U);
}

static void
vdenc_emit_vdenc_bufs(
	struct i915_gfx_batch *batch,
	const struct i915_video_vdenc_encode *enc)
{
	uint64_t ref_addr;

	ref_addr = (enc->reference_count > 0U) ? enc->references[0].picture : enc->reconstructed;

	drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(GEN12_VIDEO_VDENC_PIPE_BUF_ADDR_STATE,
						      GEN12_VIDEO_VDENC_PIPE_BUF_ADDR_STATE_DWORDS));
	/* Dword 1..3: Downscaled L0/L1 ref (unused -> reconstructed), 4..6: Unscaled Ref0. */
	vdenc_emit_buf(batch, ref_addr, enc->mocs);
	vdenc_emit_buf(batch, ref_addr, enc->mocs);
	/* Dword 7..9: VDEnc Row Store. */
	vdenc_emit_buf(batch, enc->vdenc_row_store, enc->mocs);
	/* Dword 10..12: StreamIn buffer. */
	vdenc_emit_buf(batch, enc->streamin_buffer, enc->mocs);
	/* Dword 13..15: Source picture. */
	vdenc_emit_buf(batch, enc->source, enc->mocs);
	/* Remaining dwords up to 37: zero. */
	vdenc_emit_zeros(batch, GEN12_VIDEO_VDENC_PIPE_BUF_ADDR_STATE_DWORDS - 16U);
}

static void
vdenc_emit_vdenc_const_qpt(
	struct i915_gfx_batch *batch)
{
	drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(GEN12_VIDEO_VDENC_CONST_QPT_STATE,
						      GEN12_VIDEO_VDENC_CONST_QPT_STATE_DWORDS));
	vdenc_emit_zeros(batch, GEN12_VIDEO_VDENC_CONST_QPT_STATE_DWORDS - 1U);
}

static void
vdenc_emit_vdenc_img(
	struct i915_gfx_batch *batch,
	const struct i915_video_vdenc_encode *enc)
{
	drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(GEN12_VIDEO_VDENC_IMG_STATE,
						      GEN12_VIDEO_VDENC_IMG_STATE_DWORDS));
	/* Dword 1: Picture Type (0=I, 1=P) and Frame Width/Height in MBs-1. */
	drv_i915_batch_emit(batch, (enc->slice.slice_type == I915_VIDEO_SLICE_I) ? 0U : 1U);
	drv_i915_batch_emit(batch, (enc->sps->pic_width_in_mbs_minus1 & 0xffU) |
				   ((enc->sps->pic_height_in_map_units_minus1 & 0xffU) << 16));
	/* Dword 3: Initial QP. */
	drv_i915_batch_emit(batch, (uint32_t)enc->slice.qp & 0x3fU);
	vdenc_emit_zeros(batch, GEN12_VIDEO_VDENC_IMG_STATE_DWORDS - 4U);
}

static void
vdenc_emit_vdenc_cost(
	struct i915_gfx_batch *batch,
	int32_t qp)
{
	drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(GEN12_VIDEO_VDENC_AVC_COST_STATE,
						      GEN12_VIDEO_VDENC_AVC_COST_STATE_DWORDS));
	drv_i915_batch_emit(batch, (uint32_t)qp & 0x3fU);
	vdenc_emit_zeros(batch, GEN12_VIDEO_VDENC_AVC_COST_STATE_DWORDS - 2U);
}

static void
vdenc_emit_vdenc_cmd1(
	struct i915_gfx_batch *batch,
	const struct i915_video_vdenc_encode *enc)
{
	(void)enc;
	drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(GEN12_VIDEO_VDENC_CMD1,
						      GEN12_VIDEO_VDENC_CMD1_DWORDS));
	vdenc_emit_zeros(batch, GEN12_VIDEO_VDENC_CMD1_DWORDS - 1U);
}

static void
vdenc_emit_vdenc_cmd2(
	struct i915_gfx_batch *batch,
	const struct i915_video_vdenc_encode *enc)
{
	(void)enc;
	drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(GEN12_VIDEO_VDENC_CMD2,
						      GEN12_VIDEO_VDENC_CMD2_DWORDS));
	vdenc_emit_zeros(batch, GEN12_VIDEO_VDENC_CMD2_DWORDS - 1U);
}

static void
vdenc_emit_pak_insert_slice(
	struct i915_gfx_batch *batch,
	const uint8_t *hdr,
	size_t hdr_bits)
{
	size_t hdr_bytes;
	size_t payload_dwords;
	size_t i;
	uint32_t word;
	uint32_t ctrl;
	size_t byte_idx;

	hdr_bytes = (hdr_bits + 7U) / 8U;
	payload_dwords = (hdr_bytes + 3U) / 4U;

	ctrl = GEN12_VIDEO_PAK_INSERT_STARTCODE |
	       GEN12_VIDEO_PAK_INSERT_END_OF_SLICE |
	       GEN12_VIDEO_PAK_INSERT_LAST_HEADER |
	       GEN12_VIDEO_PAK_INSERT_EMULATION |
	       (5U << GEN12_VIDEO_PAK_INSERT_SKIP_EMUL_SHIFT) |
	       (((uint32_t)hdr_bits & 0xffU) << GEN12_VIDEO_PAK_INSERT_DATABITS_SHIFT);

	drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(GEN12_VIDEO_MFX_PAK_INSERT_OBJECT,
						      (uint32_t)(2U + payload_dwords)));
	drv_i915_batch_emit(batch, ctrl);

	for (i = 0U; i < payload_dwords; i++) {
		word = 0U;
		for (byte_idx = 0U; byte_idx < 4U; byte_idx++) {
			size_t pos = i * 4U + byte_idx;

			if (pos < hdr_bytes)
				word |= (uint32_t)hdr[pos] << (byte_idx * 8U);
		}
		drv_i915_batch_emit(batch, word);
	}
}

static void
vdenc_emit_mfx_slice(
	struct i915_gfx_batch *batch,
	const struct i915_video_vdenc_encode *enc)
{
	uint32_t dw3;
	uint32_t dw4;

	dw3 = (enc->slice.slice_type & 0xfU) |
	      ((enc->slice.cabac_init_idc & 0x3U) << 16) |
	      (((uint32_t)enc->slice.qp & 0x3fU) << 24);
	dw4 = (enc->slice.disable_deblocking_filter_idc & 0x3U) |
	      (((uint32_t)enc->slice.slice_alpha_c0_offset_div2 & 0xfU) << 4) |
	      (((uint32_t)enc->slice.slice_beta_offset_div2 & 0xfU) << 8) |
	      (1U << 19);

	drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(GEN12_VIDEO_MFX_AVC_SLICE_STATE,
						      GEN12_VIDEO_MFX_AVC_SLICE_STATE_DWORDS));
	drv_i915_batch_emit(batch, enc->slice.slice_type & 0xfU);
	drv_i915_batch_emit(batch, enc->slice.num_ref_idx_l0_active_minus1 & 0x3fU);
	drv_i915_batch_emit(batch, dw3);
	drv_i915_batch_emit(batch, dw4);
	vdenc_emit_zeros(batch, GEN12_VIDEO_MFX_AVC_SLICE_STATE_DWORDS - 5U);
}

static void
vdenc_emit_vdenc_walker(
	struct i915_gfx_batch *batch,
	const struct i915_video_vdenc_encode *enc)
{
	drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(GEN12_VIDEO_VDENC_WALKER_STATE,
						      GEN12_VIDEO_VDENC_WALKER_STATE_DWORDS));
	drv_i915_batch_emit(batch, 0U);
	drv_i915_batch_emit(batch, (enc->sps->pic_width_in_mbs_minus1 & 0xffU) |
				   ((enc->sps->pic_height_in_map_units_minus1 & 0xffU) << 16));
	vdenc_emit_zeros(batch, GEN12_VIDEO_VDENC_WALKER_STATE_DWORDS - 3U);
}

static void
vdenc_emit_tail(
	struct i915_gfx_batch *batch,
	const struct i915_video_vdenc_encode *enc)
{
	/* Flush VDEnc + MFX pipelines. */
	drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(GEN12_VIDEO_VD_PIPELINE_FLUSH,
						      GEN12_VIDEO_VD_PIPELINE_FLUSH_DWORDS));
	drv_i915_batch_emit(batch, 0xffffU);

	drv_i915_batch_emit(batch, GEN12_VIDEO_MI_FLUSH_DW | GEN12_VIDEO_MI_FLUSH_DW_INVALIDATE);
	vdenc_emit_zeros(batch, GEN12_VIDEO_MI_FLUSH_DW_DWORDS - 1U);

	/* Store MFC_BITSTREAM_BYTECOUNT_FRAME to feedback_addr if provided. */
	if (enc->feedback_addr != 0U) {
		drv_i915_batch_emit(batch, GEN12_VIDEO_MI_STORE_REGISTER_MEM);
		drv_i915_batch_emit(batch, GEN12_VIDEO_VCS0_MFC_BYTECOUNT_FRAME);
		vdenc_emit_addr(batch, enc->feedback_addr);
	}

	drv_i915_batch_emit(batch, GEN12_VIDEO_MI_BATCH_BUFFER_END);
}
