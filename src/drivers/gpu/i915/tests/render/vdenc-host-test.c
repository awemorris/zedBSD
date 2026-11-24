/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host unit test for the Gen12 VDEnc + MFX H.264 command builder and
 * Annex-B SPS/PPS/Slice header packer (vk-video-vdenc.c).
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "drivers/gpu/i915/intel/genxml-video.h"
#include "drivers/gpu/i915/vk/vk-video-vdenc.h"
#include "userland/desktop/libmedia/h264sw-ps.h"
#include "userland/desktop/libmedia/vcodec-bitreader.h"
#include "userland/desktop/libmedia/vcodec-nal.h"

void
drv_i915_batch_emit(
	struct i915_gfx_batch *batch,
	uint32_t dword)
{
	if (batch->count < batch->capacity)
		batch->cmds[batch->count] = dword;
	else
		batch->overflow = 1;
	batch->count++;
}

static unsigned
vdenc_command_dwords(
	uint32_t header)
{
	uint32_t type;

	type = (header >> 29) & 0x7U;
	if (type == 0U) {
		if (header == GEN12_VIDEO_MI_BATCH_BUFFER_END)
			return 1U;
		if (header == GEN12_VIDEO_MI_FORCE_WAKEUP)
			return GEN12_VIDEO_MI_FORCE_WAKEUP_DWORDS;
		if (header == GEN12_VIDEO_MI_STORE_REGISTER_MEM)
			return GEN12_VIDEO_MI_STORE_REGISTER_MEM_DWORDS;
		if ((header & 0xffff0000U) == (GEN12_VIDEO_MI_FLUSH_DW & 0xffff0000U))
			return GEN12_VIDEO_MI_FLUSH_DW_DWORDS;
		return 0U;
	}
	if (type == 3U) {
		if ((header & ~GEN12_VIDEO_MFX_WAIT_SYNC) == GEN12_VIDEO_MFX_WAIT)
			return 1U;
		return (header & 0xffffU) + 2U;
	}
	return 0U;
}

static void
verify_sps_pps_roundtrip(
	const struct i915_video_sps *sps,
	const struct i915_video_pps *pps)
{
	uint8_t sps_buf[I915_VIDEO_VDENC_PS_MAX_BYTES];
	uint8_t pps_buf[I915_VIDEO_VDENC_PS_MAX_BYTES];
	uint8_t rbsp[I915_VIDEO_VDENC_PS_MAX_BYTES];
	size_t sps_len;
	size_t pps_len;
	size_t rbsp_len;
	struct h264sw_ps_store store;
	struct vcodec_bitreader br;
	const struct h264sw_sps *dec_sps;
	const struct h264sw_pps *dec_pps;
	int err;

	memset(&store, 0, sizeof(store));

	err = drv_i915_video_vdenc_pack_sps(sps, sps_buf, sizeof(sps_buf), &sps_len);
	assert(err == 0);
	assert(sps_len > 6U);
	assert(sps_buf[0] == 0x00U && sps_buf[1] == 0x00U && sps_buf[2] == 0x00U && sps_buf[3] == 0x01U);
	assert(sps_buf[4] == 0x67U);

	err = vcodec_nal_unescape(sps_buf + 5U, sps_len - 5U, rbsp, sizeof(rbsp), &rbsp_len);
	assert(err == 0 && rbsp_len > 0U);
	vcodec_bitreader_init(&br, rbsp, rbsp_len);
	err = h264sw_ps_read_sps(&store, &br);
	assert(err == 0);

	dec_sps = &store.sps[sps->id];
	assert(store.sps_present[sps->id]);
	assert(dec_sps->profile_idc == sps->profile_idc);
	assert(dec_sps->level_idc == sps->level_idc);
	assert(dec_sps->width_mbs == sps->pic_width_in_mbs_minus1 + 1U);
	assert(dec_sps->height_mbs == sps->pic_height_in_map_units_minus1 + 1U);
	assert(dec_sps->poc_type == sps->pic_order_cnt_type);

	err = drv_i915_video_vdenc_pack_pps(pps, pps_buf, sizeof(pps_buf), &pps_len);
	assert(err == 0);
	assert(pps_len > 5U);
	assert(pps_buf[0] == 0x00U && pps_buf[1] == 0x00U && pps_buf[2] == 0x00U && pps_buf[3] == 0x01U);
	assert(pps_buf[4] == 0x68U);

	err = vcodec_nal_unescape(pps_buf + 5U, pps_len - 5U, rbsp, sizeof(rbsp), &rbsp_len);
	assert(err == 0 && rbsp_len > 0U);
	vcodec_bitreader_init(&br, rbsp, rbsp_len);
	err = h264sw_ps_read_pps(&store, &br);
	assert(err == 0);

	dec_pps = &store.pps[pps->pps_id];
	assert(store.pps_present[pps->pps_id]);
	assert(dec_pps->sps_id == pps->sps_id);
	assert(dec_pps->pic_init_qp == 26 + pps->pic_init_qp_minus26);
	assert(dec_pps->cabac == ((pps->flags & I915_VIDEO_PPS_CABAC) != 0U ? 1 : 0));
}

static void
verify_batch_walk(
	const struct i915_video_vdenc_encode *enc,
	int expect_ref_idx)
{
	uint32_t cmds[I915_VIDEO_VDENC_MAX_DWORDS];
	struct i915_gfx_batch batch;
	unsigned idx;
	unsigned count;
	int seen_vdenc_img;
	int seen_vdenc_walker;
	int seen_pak_insert;
	int seen_mfx_ref_idx;
	int seen_store_reg;
	int seen_end;

	memset(&batch, 0, sizeof(batch));
	batch.cmds = cmds;
	batch.capacity = I915_VIDEO_VDENC_MAX_DWORDS;
	batch.graphics_ver = 12U;

	drv_i915_video_vdenc_build(&batch, enc);
	assert(batch.overflow == 0);
	assert(batch.count > 0U && batch.count <= I915_VIDEO_VDENC_MAX_DWORDS);

	idx = 0U;
	count = 0U;
	seen_vdenc_img = 0;
	seen_vdenc_walker = 0;
	seen_pak_insert = 0;
	seen_mfx_ref_idx = 0;
	seen_store_reg = 0;
	seen_end = 0;

	while (idx < batch.count) {
		uint32_t hdr = cmds[idx];
		unsigned dw = vdenc_command_dwords(hdr);
		uint32_t op = hdr >> 16;

		assert(dw > 0U);
		assert(idx + dw <= batch.count);
		if (op == GEN12_VIDEO_VDENC_IMG_STATE)
			seen_vdenc_img = 1;
		if (op == GEN12_VIDEO_VDENC_WALKER_STATE)
			seen_vdenc_walker = 1;
		if (op == GEN12_VIDEO_MFX_PAK_INSERT_OBJECT)
			seen_pak_insert = 1;
		if (op == GEN12_VIDEO_MFX_AVC_REF_IDX_STATE)
			seen_mfx_ref_idx = 1;
		if (hdr == GEN12_VIDEO_MI_STORE_REGISTER_MEM)
			seen_store_reg = 1;
		if (hdr == GEN12_VIDEO_MI_BATCH_BUFFER_END) {
			seen_end = 1;
			assert(idx + dw == batch.count);
		}
		idx += dw;
		count++;
	}

	assert(count >= 20U);
	assert(seen_vdenc_img);
	assert(seen_vdenc_walker);
	assert(seen_pak_insert);
	assert(seen_mfx_ref_idx == expect_ref_idx);
	assert(seen_store_reg);
	assert(seen_end);

	/* Verify overflow detection when capacity is small. */
	memset(&batch, 0, sizeof(batch));
	batch.cmds = cmds;
	batch.capacity = 16U;
	drv_i915_video_vdenc_build(&batch, enc);
	assert(batch.overflow == 1);
	assert(batch.count > 16U);
}

int
main(void)
{
	struct i915_video_sps sps;
	struct i915_video_pps pps;
	struct i915_video_vdenc_encode enc;
	uint8_t slice_hdr[I915_VIDEO_VDENC_SLICE_HDR_MAX];
	size_t slice_bits;
	int err;

	/* 1. Baseline 320x240, POC type 2, CAVLC. */
	memset(&sps, 0, sizeof(sps));
	sps.flags = I915_VIDEO_SPS_FRAME_MBS_ONLY | I915_VIDEO_SPS_DIRECT_8X8_INFERENCE | 0x02U;
	sps.profile_idc = 66U;
	sps.level_idc = 30U;
	sps.chroma_format_idc = 1U;
	sps.id = 0U;
	sps.pic_order_cnt_type = 2U;
	sps.max_num_ref_frames = 1U;
	sps.pic_width_in_mbs_minus1 = 19U;
	sps.pic_height_in_map_units_minus1 = 14U;

	memset(&pps, 0, sizeof(pps));
	pps.flags = I915_VIDEO_PPS_DEBLOCKING_CONTROL;
	pps.sps_id = 0U;
	pps.pps_id = 0U;
	pps.pic_init_qp_minus26 = 0;

	assert(drv_i915_video_vdenc_check_sets(&sps, &pps) == NULL);
	verify_sps_pps_roundtrip(&sps, &pps);

	/* 2. High 1920x1080, POC type 0, CABAC + 8x8 transform + crop. */
	memset(&sps, 0, sizeof(sps));
	sps.flags = I915_VIDEO_SPS_FRAME_MBS_ONLY | I915_VIDEO_SPS_DIRECT_8X8_INFERENCE;
	sps.profile_idc = 100U;
	sps.level_idc = 40U;
	sps.chroma_format_idc = 1U;
	sps.id = 1U;
	sps.log2_max_frame_num_minus4 = 0U;
	sps.pic_order_cnt_type = 0U;
	sps.log2_max_pic_order_cnt_lsb_minus4 = 2U;
	sps.max_num_ref_frames = 1U;
	sps.pic_width_in_mbs_minus1 = 119U;
	sps.pic_height_in_map_units_minus1 = 67U;
	sps.crop[3] = 4U;

	memset(&pps, 0, sizeof(pps));
	pps.flags = I915_VIDEO_PPS_CABAC | I915_VIDEO_PPS_TRANSFORM_8X8 | I915_VIDEO_PPS_DEBLOCKING_CONTROL;
	pps.sps_id = 1U;
	pps.pps_id = 2U;
	pps.pic_init_qp_minus26 = -2;
	pps.chroma_qp_index_offset = -1;
	pps.second_chroma_qp_index_offset = -1;

	assert(drv_i915_video_vdenc_check_sets(&sps, &pps) == NULL);
	verify_sps_pps_roundtrip(&sps, &pps);

	/* 3. Slice header packing + batch build for IDR I-frame and P-frame. */
	memset(&enc, 0, sizeof(enc));
	enc.sps = &sps;
	enc.pps = &pps;
	enc.is_idr = 1;
	enc.is_reference = 1;
	enc.frame_num = 0U;
	enc.idr_pic_id = 0U;
	enc.poc = 0;
	enc.width = 1920U;
	enc.height = 1080U;
	enc.pitch = 1920U;
	enc.chroma_rows = 1088U;
	enc.source = 0x10000000ULL;
	enc.reconstructed = 0x20000000ULL;
	enc.row_stores[0] = 0x30000000ULL;
	enc.row_stores[1] = 0x30010000ULL;
	enc.row_stores[2] = 0x30020000ULL;
	enc.row_stores[3] = 0x30030000ULL;
	enc.vdenc_row_store = 0x30040000ULL;
	enc.streamin_buffer = 0x30050000ULL;
	enc.streamout_buffer = 0x30060000ULL;
	enc.mb_status_buffer = 0x30070000ULL;
	enc.bitstream_base = 0x40000000ULL;
	enc.bitstream_end = 0x40100000ULL;
	enc.feedback_addr = 0x50000000ULL;
	enc.slice.slice_type = I915_VIDEO_SLICE_I;
	enc.slice.qp = 24;
	enc.mocs = 2U;

	err = drv_i915_video_vdenc_pack_slice_header(&enc, slice_hdr, sizeof(slice_hdr), &slice_bits);
	assert(err == 0);
	assert(slice_hdr[0] == 0x00U && slice_hdr[1] == 0x00U && slice_hdr[2] == 0x00U && slice_hdr[3] == 0x01U);
	assert(slice_hdr[4] == 0x65U);
	assert(slice_bits > 40U && slice_bits < 256U);
	verify_batch_walk(&enc, 0);

	/* P-frame. */
	enc.is_idr = 0;
	enc.frame_num = 1U;
	enc.poc = 2;
	enc.reference_count = 1U;
	enc.references[0].slot_index = 0;
	enc.references[0].frame_num = 0U;
	enc.references[0].poc = 0;
	enc.references[0].picture = 0x20000000ULL;
	enc.slice.slice_type = I915_VIDEO_SLICE_P;
	enc.slice.qp = 26;

	err = drv_i915_video_vdenc_pack_slice_header(&enc, slice_hdr, sizeof(slice_hdr), &slice_bits);
	assert(err == 0);
	assert(slice_hdr[4] == 0x41U);
	verify_batch_walk(&enc, 1);

	printf("vdenc-host-test: PASS\n");
	return 0;
}
