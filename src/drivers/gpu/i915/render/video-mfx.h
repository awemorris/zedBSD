/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The MFX command builder of an H.264 decode (ws083-p004).
 *
 * video.c resolves a decode -- its parameter sets, its picture, its
 * references and the GPU addresses of everything the decoder reads and
 * writes -- into a struct i915_video_mfx_decode, and the builder writes the
 * Gen12 MFX commands of that one picture into a batch.  The builder reads
 * nothing else: it checks nothing the checks before it decided (design
 * §6.6) and touches no object, so it can be run and compared on the host.
 *
 * The H.264 parameter sets are kept here as the executor holds them: the
 * fields of StdVideoH264SequenceParameterSet and StdVideoH264PictureParameter
 * Set the decoder uses, with the flags packed by name from bit 0 in the
 * order the header declares them (the wire's form).
 */

#ifndef DRIVERS_GPU_I915_RENDER_VIDEO_MFX_H
#define DRIVERS_GPU_I915_RENDER_VIDEO_MFX_H

#include "batch.h"

#include <stdint.h>

/* The most references a decode reads: the sixteen entries of the MFX DPB. */
#define I915_VIDEO_MFX_REFERENCES	16U

/* The row stores of a session, in their binding order. */
#define I915_VIDEO_MFX_ROW_STORES	4U

/*
 * The most dwords one decode writes: the commands before the slices (322
 * with four quantizer matrices), the closing flush (5) and at most eleven
 * a slice for 256 slices.
 */
#define I915_VIDEO_MFX_MAX_DWORDS	(327U + 256U * 11U)

/* StdVideoH264SpsFlags, by their bit in the packed word. */
#define I915_VIDEO_SPS_DIRECT_8X8_INFERENCE	(1U << 6)
#define I915_VIDEO_SPS_MB_ADAPTIVE		(1U << 7)
#define I915_VIDEO_SPS_FRAME_MBS_ONLY		(1U << 8)

/* The largest picture the decoder takes: 256 macroblocks a side (4096 pixels), 36864 in all (level 5.1, the 16-bit Frame Size field). */
#define I915_VIDEO_MFX_MAX_SIDE_MBS		256U
#define I915_VIDEO_MFX_MAX_FRAME_MBS		36864U
#define I915_VIDEO_SPS_DELTA_ALWAYS_ZERO	(1U << 9)
#define I915_VIDEO_SPS_SCALING_MATRIX		(1U << 14)

/* StdVideoH264PpsFlags, by their bit in the packed word. */
#define I915_VIDEO_PPS_TRANSFORM_8X8		(1U << 0)
#define I915_VIDEO_PPS_REDUNDANT_PIC_COUNT	(1U << 1)
#define I915_VIDEO_PPS_CONSTRAINED_INTRA	(1U << 2)
#define I915_VIDEO_PPS_DEBLOCKING_CONTROL	(1U << 3)
#define I915_VIDEO_PPS_WEIGHTED_PRED		(1U << 4)
#define I915_VIDEO_PPS_BOTTOM_FIELD_POC		(1U << 5)
#define I915_VIDEO_PPS_CABAC			(1U << 6)
#define I915_VIDEO_PPS_SCALING_MATRIX		(1U << 7)

/* StdVideoDecodeH264PictureInfoFlags, by their bit in the packed word. */
#define I915_VIDEO_PICTURE_IS_REFERENCE		(1U << 4)

/* StdVideoDecodeH264ReferenceInfoFlags, by their bit in the packed word. */
#define I915_VIDEO_REFERENCE_TOP_FIELD		(1U << 0)
#define I915_VIDEO_REFERENCE_BOTTOM_FIELD	(1U << 1)
#define I915_VIDEO_REFERENCE_LONG_TERM		(1U << 2)
#define I915_VIDEO_REFERENCE_NON_EXISTING	(1U << 3)

/*
 * H.264 scaling lists as the wire carries them: which lists are present and
 * which of them use the default, then the six 4x4 and the six 8x8 lists in
 * their zig-zag scan order.
 */
struct i915_video_scaling {
	uint32_t present_mask;
	uint32_t default_mask;
	uint8_t list4[96];
	uint8_t list8[384];
};

/*
 * One H.264 sequence parameter set a parameters object holds.  It is one
 * allocation (D22), made when the set is added and freed with the object or
 * when a later set of the same key replaces it.
 */
struct i915_video_sps {
	uint32_t flags;
	uint32_t profile_idc;
	uint32_t level_idc;
	uint32_t chroma_format_idc;
	uint32_t id;
	uint32_t bit_depth_luma_minus8;
	uint32_t bit_depth_chroma_minus8;
	uint32_t log2_max_frame_num_minus4;
	uint32_t pic_order_cnt_type;
	int32_t offset_for_non_ref_pic;
	int32_t offset_for_top_to_bottom_field;
	uint32_t log2_max_pic_order_cnt_lsb_minus4;
	uint32_t num_ref_frames_in_pic_order_cnt_cycle;
	uint32_t max_num_ref_frames;
	uint32_t pic_width_in_mbs_minus1;
	uint32_t pic_height_in_map_units_minus1;
	uint32_t crop[4];
	uint32_t offset_count;
	int32_t offsets[255];
	int has_scaling;
	struct i915_video_scaling scaling;
};

/*
 * One H.264 picture parameter set a parameters object holds, keyed by its
 * sequence and picture set ids; the sets of one picture set id are a list.
 */
struct i915_video_pps {
	struct i915_video_pps *next;
	uint32_t flags;
	uint32_t sps_id;
	uint32_t pps_id;
	uint32_t num_ref_idx_l0_default_active_minus1;
	uint32_t num_ref_idx_l1_default_active_minus1;
	uint32_t weighted_bipred_idc;
	int32_t pic_init_qp_minus26;
	int32_t pic_init_qs_minus26;
	int32_t chroma_qp_index_offset;
	int32_t second_chroma_qp_index_offset;
	int has_scaling;
	struct i915_video_scaling scaling;
};

/*
 * The scaling matrices a picture decodes with, derived from its sets: the
 * six 4x4 lists (intra Y, Cb, Cr, then inter Y, Cb, Cr) and the two 8x8
 * lists of 4:2:0 (intra Y, inter Y), each in zig-zag scan order.
 */
struct i915_video_matrices {
	uint8_t list4[6][16];
	uint8_t list8[2][64];
};

/*
 * One reference picture of a decode, in the order of the decode's
 * pReferenceSlots[]: its DPB slot, its StdVideoDecodeH264ReferenceInfo and
 * the GPU addresses of its picture and of its slot's motion vectors.
 */
struct i915_video_mfx_reference {
	int32_t slot_index;
	uint32_t flags;
	uint32_t frame_num;
	int32_t poc[2];
	uint64_t picture;
	uint64_t motion;
};

/*
 * One picture to decode, resolved: what the MFX commands of the picture are
 * built from.  It lives on the stack of the decode; the sets it points to
 * are the parameters object's.
 */
struct i915_video_mfx_decode {
	/* The picture's sets and its StdVideoDecodeH264PictureInfo. */
	const struct i915_video_sps *sps;
	const struct i915_video_pps *pps;
	uint32_t picture_flags;
	uint32_t frame_num;
	int32_t poc[2];

	/*
	 * The output picture: its NV12 image's address (the Y plane), extent,
	 * pitch and the rows of the Y plane (where the CbCr plane starts).
	 */
	uint64_t destination;
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint32_t chroma_rows;

	/* The session's four row stores and the motion vector buffer the picture writes. */
	uint64_t row_stores[I915_VIDEO_MFX_ROW_STORES];
	uint64_t motion_write;

	/*
	 * The bitstream: the 4 KiB page the decode's range starts in, the
	 * end of the buffer's bound range, and the bytes from the page to the
	 * range's start.
	 */
	uint64_t bitstream_base;
	uint64_t bitstream_end;
	uint32_t skew;

	/*
	 * The slices, as byte offsets from the range's start: where each one's
	 * NAL unit begins (after its start code) and where it ends.
	 */
	uint32_t slice_count;
	const uint32_t *slice_starts;
	const uint32_t *slice_ends;

	/* The references, in the decode's order. */
	uint32_t reference_count;
	struct i915_video_mfx_reference references[I915_VIDEO_MFX_REFERENCES];

	/* The MOCS value of every buffer the commands name. */
	uint32_t mocs;
};

/* The H.264 tables of video-h264-tables.c. */
extern const uint8_t drv_i915_video_zigzag4[16];
extern const uint8_t drv_i915_video_zigzag8[64];
extern const uint8_t drv_i915_video_default4_intra[16];
extern const uint8_t drv_i915_video_default4_inter[16];
extern const uint8_t drv_i915_video_default8_intra[64];
extern const uint8_t drv_i915_video_default8_inter[64];

void drv_i915_video_mfx_matrices(const struct i915_video_sps *sps, const struct i915_video_pps *pps, struct i915_video_matrices *matrices);
void drv_i915_video_mfx_build(struct i915_gfx_batch *batch, const struct i915_video_mfx_decode *decode);
const char *drv_i915_video_mfx_check_sets(const struct i915_video_sps *sps, const struct i915_video_pps *pps);

#endif
