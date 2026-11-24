/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Gen12 VDEnc + MFX command builder and H.264 RBSP header packer.
 *
 * vk-video.c resolves an encode operation into struct i915_video_vdenc_encode
 * and this module emits the Gen12 VDEnc + MFX batch commands and packs
 * Annex-B SPS/PPS and slice header bitstreams without touching kernel state
 * objects, so it can be built and verified on the host.
 */

#ifndef DRIVERS_GPU_I915_VK_VK_VIDEO_VDENC_H
#define DRIVERS_GPU_I915_VK_VK_VIDEO_VDENC_H

#include "vk-batch.h"
#include "vk-video-mfx.h"

#include <stddef.h>
#include <stdint.h>

/* Maximum dwords emitted for a single-slice H.264 VDEnc + MFX encode frame. */
#define I915_VIDEO_VDENC_MAX_DWORDS	1024U

/* Maximum packed Annex-B SPS or PPS byte length. */
#define I915_VIDEO_VDENC_PS_MAX_BYTES	256U

/* Maximum packed slice header byte length. */
#define I915_VIDEO_VDENC_SLICE_HDR_MAX	64U

/* Slice types as encoded in H.264 slice_header and MFX_AVC_SLICE_STATE. */
#define I915_VIDEO_SLICE_P		0U
#define I915_VIDEO_SLICE_I		2U

/*
 * One reference picture of an encode, in the order of pReferenceSlots[].
 */
struct i915_video_vdenc_reference {
	int32_t slot_index;
	uint32_t frame_num;
	int32_t poc;
	uint64_t picture;
};

/*
 * Slice parameters of one H.264 encode slice.
 */
struct i915_video_vdenc_slice {
	uint32_t slice_type;
	uint32_t first_mb_in_slice;
	int32_t qp;
	uint32_t cabac_init_idc;
	uint32_t disable_deblocking_filter_idc;
	int32_t slice_alpha_c0_offset_div2;
	int32_t slice_beta_offset_div2;
	uint32_t num_ref_idx_l0_active_minus1;
	uint32_t l0_slot;
	uint32_t l0_frame_num;
	int32_t l0_poc;
};

/*
 * Resolved parameters for one H.264 frame encode on Gen12 VDEnc + MFX.
 */
struct i915_video_vdenc_encode {
	const struct i915_video_sps *sps;
	const struct i915_video_pps *pps;
	int is_idr;
	int is_reference;
	uint32_t frame_num;
	uint32_t idr_pic_id;
	int32_t poc;

	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint32_t chroma_rows;

	uint64_t source;
	uint64_t reconstructed;
	uint64_t row_stores[I915_VIDEO_MFX_ROW_STORES];
	uint64_t vdenc_row_store;
	uint64_t streamin_buffer;
	uint64_t streamout_buffer;
	uint64_t mb_status_buffer;

	uint64_t bitstream_base;
	uint64_t bitstream_end;
	uint64_t feedback_addr;

	uint32_t reference_count;
	struct i915_video_vdenc_reference references[I915_VIDEO_MFX_REFERENCES];
	struct i915_video_vdenc_slice slice;

	uint32_t mocs;
};

const char *drv_i915_video_vdenc_check_sets(const struct i915_video_sps *sps, const struct i915_video_pps *pps);
int drv_i915_video_vdenc_pack_sps(const struct i915_video_sps *sps, uint8_t *out, size_t cap, size_t *out_len);
int drv_i915_video_vdenc_pack_pps(const struct i915_video_pps *pps, uint8_t *out, size_t cap, size_t *out_len);
int drv_i915_video_vdenc_pack_slice_header(const struct i915_video_vdenc_encode *enc, uint8_t *out, size_t cap, size_t *out_bits);
void drv_i915_video_vdenc_build(struct i915_gfx_batch *batch, const struct i915_video_vdenc_encode *enc);

#endif
