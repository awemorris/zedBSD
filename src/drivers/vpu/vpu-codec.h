/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What a stateful video decoder core is given besides the picture's
 * slices: the H.264 and H.265 parameter sets in the form Vulkan Video's
 * StdVideo structures carry them, written back into NAL units the core
 * parses (vpu-h264-ps.c, vpu-h265-ps.c, on vpu-bits.h's writer); and the
 * bookkeeping that ties the application's decoded picture buffer slots to
 * the core's frame buffers, so a picture is never decoded into a frame
 * buffer the core still references (vpu-slots.c).  Neither part touches
 * hardware: a host test runs both.
 */

#ifndef KERN_DRIVERS_VPU_VPU_CODEC_H
#define KERN_DRIVERS_VPU_VPU_CODEC_H

#include <stddef.h>
#include <stdint.h>

/* The most bytes one written parameter set's NAL unit takes. */
#define VPU_H264_SET_MAX		3072U

/*
 * The SPS flags, one bit each, in the order StdVideoH264SpsFlags declares
 * them (the order libvulkan packs them in).
 */
#define VPU_H264_SPS_CONSTRAINT_SET0	0x0001U
#define VPU_H264_SPS_DIRECT_8X8		0x0040U
#define VPU_H264_SPS_MB_ADAPTIVE	0x0080U
#define VPU_H264_SPS_FRAME_MBS_ONLY	0x0100U
#define VPU_H264_SPS_DELTA_POC_ZERO	0x0200U
#define VPU_H264_SPS_SEPARATE_PLANES	0x0400U
#define VPU_H264_SPS_GAPS_ALLOWED	0x0800U
#define VPU_H264_SPS_QPPRIME_BYPASS	0x1000U
#define VPU_H264_SPS_CROPPING		0x2000U
#define VPU_H264_SPS_SCALING		0x4000U

/* The PPS flags, one bit each, in the order StdVideoH264PpsFlags declares them. */
#define VPU_H264_PPS_TRANSFORM_8X8	0x0001U
#define VPU_H264_PPS_REDUNDANT_PIC_CNT	0x0002U
#define VPU_H264_PPS_CONSTRAINED_INTRA	0x0004U
#define VPU_H264_PPS_DEBLOCKING_CONTROL	0x0008U
#define VPU_H264_PPS_WEIGHTED_PRED	0x0010U
#define VPU_H264_PPS_BOTTOM_FIELD_POC	0x0020U
#define VPU_H264_PPS_CABAC		0x0040U
#define VPU_H264_PPS_SCALING		0x0080U

/*
 * Scaling lists as StdVideoH264ScalingLists holds them: which lists are
 * present, which of those take the default matrix, and the lists in the
 * order the bitstream codes them (zig-zag).
 */
struct vpu_h264_scaling {
	uint32_t present_mask;
	uint32_t default_mask;
	uint8_t list4[6][16];
	uint8_t list8[6][64];
};

/*
 * A sequence parameter set as StdVideoH264SequenceParameterSet gives it:
 * level is the StdVideoH264LevelIdc enumerant, crop the four frame crop
 * offsets (left, right, top, bottom), offsets the picture order count
 * cycle's.
 */
struct vpu_h264_sps {
	uint32_t flags;
	uint32_t profile_idc;
	uint32_t level;
	uint32_t chroma_format_idc;
	uint32_t id;
	uint32_t bit_depth_luma_minus8;
	uint32_t bit_depth_chroma_minus8;
	uint32_t log2_max_frame_num_minus4;
	uint32_t pic_order_cnt_type;
	int32_t offset_for_non_ref_pic;
	int32_t offset_for_top_to_bottom_field;
	uint32_t log2_max_pic_order_cnt_lsb_minus4;
	uint32_t max_num_ref_frames;
	uint32_t pic_width_in_mbs_minus1;
	uint32_t pic_height_in_map_units_minus1;
	uint32_t crop[4];
	uint32_t offset_count;
	int32_t offsets[255];
	int has_scaling;
	struct vpu_h264_scaling scaling;
};

/* A picture parameter set as StdVideoH264PictureParameterSet gives it. */
struct vpu_h264_pps {
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
	struct vpu_h264_scaling scaling;
};

/*
 * Writes an SPS NAL unit (a four-byte start code, then the unit with
 * emulation prevention) from its fields.  With no_reorder the unit carries
 * VUI whose only content is a bitstream restriction asking for no output
 * reordering; without, no VUI.  EINVAL for a field H.264 cannot code,
 * ENOSPC when the unit does not fit.
 */
int vpu_h264_write_sps(const struct vpu_h264_sps *sps, int no_reorder, uint8_t *output, size_t capacity, size_t *written);

/*
 * Writes a PPS NAL unit from its fields; the SPS it refers to gives the
 * chroma format its scaling lists depend on (NULL takes 4:2:0).
 */
int vpu_h264_write_pps(const struct vpu_h264_pps *pps, const struct vpu_h264_sps *sps, uint8_t *output, size_t capacity, size_t *written);

/* The most bytes one written H.265 parameter set's NAL unit takes (an SPS of 64 full short-term sets and scaling lists). */
#define VPU_H265_SET_MAX		8192U

/* The H.265 counts StdVideoH265 holds: sub-layers, short-term sets, long-term pictures, entries of a set, tiles. */
#define VPU_H265_SUB_LAYERS		7U
#define VPU_H265_ST_SETS		64U
#define VPU_H265_LT_PICTURES		32U
#define VPU_H265_ST_ENTRIES		16U
#define VPU_H265_TILE_COLUMNS		19U
#define VPU_H265_TILE_ROWS		21U
#define VPU_H265_CHROMA_QP_LIST		6U

/* The VPS flags, one bit each, in the order StdVideoH265VpsFlags declares them. */
#define VPU_H265_VPS_TEMPORAL_NESTING	0x1U
#define VPU_H265_VPS_ORDERING_INFO	0x2U

/* The profile, tier and level flags, in the order StdVideoH265ProfileTierLevelFlags declares them. */
#define VPU_H265_PTL_TIER		0x01U
#define VPU_H265_PTL_PROGRESSIVE	0x02U
#define VPU_H265_PTL_INTERLACED		0x04U
#define VPU_H265_PTL_NON_PACKED		0x08U
#define VPU_H265_PTL_FRAME_ONLY		0x10U

/* The SPS flags, in the order StdVideoH265SpsFlags declares them (bit 0 sps_temporal_id_nesting_flag). */
#define VPU_H265_SPS_TEMPORAL_NESTING	(1U << 0)
#define VPU_H265_SPS_SEPARATE_PLANES	(1U << 1)
#define VPU_H265_SPS_CONFORMANCE_WINDOW	(1U << 2)
#define VPU_H265_SPS_ORDERING_INFO	(1U << 3)
#define VPU_H265_SPS_SCALING		(1U << 4)
#define VPU_H265_SPS_SCALING_DATA	(1U << 5)
#define VPU_H265_SPS_AMP		(1U << 6)
#define VPU_H265_SPS_SAO		(1U << 7)
#define VPU_H265_SPS_PCM		(1U << 8)
#define VPU_H265_SPS_PCM_LOOP_FILTER_OFF	(1U << 9)
#define VPU_H265_SPS_LONG_TERM		(1U << 10)
#define VPU_H265_SPS_TEMPORAL_MVP	(1U << 11)
#define VPU_H265_SPS_STRONG_SMOOTHING	(1U << 12)
#define VPU_H265_SPS_VUI		(1U << 13)
#define VPU_H265_SPS_EXTENSION		(1U << 14)
#define VPU_H265_SPS_RANGE_EXTENSION	(1U << 15)
#define VPU_H265_SPS_RANGE_FIRST	16U
#define VPU_H265_SPS_RANGE_FLAGS	9U
#define VPU_H265_SPS_SCC_FLAGS		(0x1fU << 25)

/* The PPS flags, in the order StdVideoH265PpsFlags declares them (bit 0 dependent_slice_segments_enabled_flag). */
#define VPU_H265_PPS_DEPENDENT_SLICES	(1U << 0)
#define VPU_H265_PPS_OUTPUT_FLAG	(1U << 1)
#define VPU_H265_PPS_SIGN_HIDING	(1U << 2)
#define VPU_H265_PPS_CABAC_INIT		(1U << 3)
#define VPU_H265_PPS_CONSTRAINED_INTRA	(1U << 4)
#define VPU_H265_PPS_TRANSFORM_SKIP	(1U << 5)
#define VPU_H265_PPS_CU_QP_DELTA	(1U << 6)
#define VPU_H265_PPS_CHROMA_QP_OFFSETS	(1U << 7)
#define VPU_H265_PPS_WEIGHTED_PRED	(1U << 8)
#define VPU_H265_PPS_WEIGHTED_BIPRED	(1U << 9)
#define VPU_H265_PPS_TRANSQUANT_BYPASS	(1U << 10)
#define VPU_H265_PPS_TILES		(1U << 11)
#define VPU_H265_PPS_ENTROPY_SYNC	(1U << 12)
#define VPU_H265_PPS_UNIFORM_SPACING	(1U << 13)
#define VPU_H265_PPS_FILTER_TILES	(1U << 14)
#define VPU_H265_PPS_FILTER_SLICES	(1U << 15)
#define VPU_H265_PPS_DEBLOCKING_CONTROL	(1U << 16)
#define VPU_H265_PPS_DEBLOCKING_OVERRIDE	(1U << 17)
#define VPU_H265_PPS_DEBLOCKING_OFF	(1U << 18)
#define VPU_H265_PPS_SCALING_DATA	(1U << 19)
#define VPU_H265_PPS_LISTS_MODIFICATION	(1U << 20)
#define VPU_H265_PPS_HEADER_EXTENSION	(1U << 21)
#define VPU_H265_PPS_EXTENSION		(1U << 22)
#define VPU_H265_PPS_CROSS_COMPONENT	(1U << 23)
#define VPU_H265_PPS_CHROMA_QP_LIST	(1U << 24)
#define VPU_H265_PPS_SCC_FLAGS		(0x1fU << 25)
#define VPU_H265_PPS_RANGE_EXTENSION	(1U << 30)

/* A profile_tier_level() as StdVideoH265ProfileTierLevel gives it: the flags, the profile and the StdVideoH265LevelIdc. */
struct vpu_h265_ptl {
	uint32_t flags;
	uint32_t profile_idc;
	uint32_t level;
};

/* The sub-layers' ordering information as StdVideoH265DecPicBufMgr holds it. */
struct vpu_h265_ordering {
	uint32_t latency_plus1[VPU_H265_SUB_LAYERS];
	uint8_t buffering_minus1[VPU_H265_SUB_LAYERS];
	uint8_t reorder[VPU_H265_SUB_LAYERS];
};

/*
 * The scaling lists as StdVideoH265ScalingLists holds them: each list's
 * coefficients in coding order (ScalingList[ sizeId ][ matrixId ][ i ]),
 * the 32x32 ones for matrixId 0 and 3, and the DC coefficients of the
 * 16x16 and 32x32 lists.
 */
struct vpu_h265_scaling {
	uint8_t list4[6][16];
	uint8_t list8[6][64];
	uint8_t list16[6][64];
	uint8_t list32[2][64];
	uint8_t dc16[6];
	uint8_t dc32[2];
};

/*
 * A short-term reference picture set in its explicit form (the form
 * StdVideoH265ShortTermRefPicSet carries for every set, predicted ones
 * too): num_negative_pics and num_positive_pics, each entry's
 * delta_poc_s0_minus1 and delta_poc_s1_minus1, and the used_by_curr_pic
 * flags as bit masks.
 */
struct vpu_h265_st_rps {
	uint32_t negative;
	uint32_t positive;
	uint16_t delta_s0_minus1[VPU_H265_ST_ENTRIES];
	uint16_t delta_s1_minus1[VPU_H265_ST_ENTRIES];
	uint32_t used_s0;
	uint32_t used_s1;
};

/* A video parameter set as StdVideoH265VideoParameterSet gives it; the timing and HRD parts are not kept. */
struct vpu_h265_vps {
	uint32_t flags;
	uint32_t id;
	uint32_t max_sub_layers_minus1;
	int has_ptl;
	struct vpu_h265_ptl ptl;
	int has_ordering;
	struct vpu_h265_ordering ordering;
};

/*
 * A sequence parameter set as StdVideoH265SequenceParameterSet gives it
 * (every field the decoding reads; the VUI is not kept): the sizes in
 * luma samples, the log2 sizes in the syntax's minus and diff forms, the
 * PCM fields, the conformance window (left, right, top, bottom), the
 * short-term sets, the long-term pictures (lt_used a bit per picture).
 */
struct vpu_h265_sps {
	uint32_t flags;
	uint32_t chroma_format_idc;
	uint32_t width;
	uint32_t height;
	uint32_t vps_id;
	uint32_t max_sub_layers_minus1;
	uint32_t id;
	uint32_t bit_depth_luma_minus8;
	uint32_t bit_depth_chroma_minus8;
	uint32_t log2_max_poc_lsb_minus4;
	uint32_t log2_min_cb_minus3;
	uint32_t log2_diff_cb;
	uint32_t log2_min_tb_minus2;
	uint32_t log2_diff_tb;
	uint32_t depth_inter;
	uint32_t depth_intra;
	uint32_t pcm_luma_minus1;
	uint32_t pcm_chroma_minus1;
	uint32_t log2_min_pcm_minus3;
	uint32_t log2_diff_pcm;
	uint32_t window[4];
	int has_ptl;
	struct vpu_h265_ptl ptl;
	int has_ordering;
	struct vpu_h265_ordering ordering;
	int has_scaling;
	struct vpu_h265_scaling scaling;
	uint32_t st_count;
	struct vpu_h265_st_rps st[VPU_H265_ST_SETS];
	uint32_t lt_count;
	uint32_t lt_used;
	uint32_t lt_lsb[VPU_H265_LT_PICTURES];
};

/*
 * A picture parameter set as StdVideoH265PictureParameterSet gives it:
 * the ids (its SPS's and that SPS's VPS's), the slice and quantizer
 * fields, the tiles (column widths and row heights when not uniform), the
 * deblocking offsets, the scaling lists, and the range extension's fields.
 */
struct vpu_h265_pps {
	uint32_t flags;
	uint32_t id;
	uint32_t sps_id;
	uint32_t vps_id;
	uint32_t extra_bits;
	uint32_t l0_minus1;
	uint32_t l1_minus1;
	int32_t init_qp_minus26;
	uint32_t qp_depth;
	int32_t cb_offset;
	int32_t cr_offset;
	int32_t beta_div2;
	int32_t tc_div2;
	uint32_t merge_level_minus2;
	uint32_t skip_size_minus2;
	uint32_t chroma_qp_depth;
	uint32_t chroma_qp_list_minus1;
	int8_t cb_list[VPU_H265_CHROMA_QP_LIST];
	int8_t cr_list[VPU_H265_CHROMA_QP_LIST];
	uint32_t sao_luma;
	uint32_t sao_chroma;
	uint32_t tile_columns_minus1;
	uint32_t tile_rows_minus1;
	uint16_t column_minus1[VPU_H265_TILE_COLUMNS];
	uint16_t row_minus1[VPU_H265_TILE_ROWS];
	int has_scaling;
	struct vpu_h265_scaling scaling;
};

/*
 * Write a VPS, an SPS or a PPS NAL unit (a four-byte start code, the
 * two-byte header of nuh_layer_id 0 and TemporalId 0, the unit with
 * emulation prevention) from its fields, in the syntax's order (ITU-T
 * H.265 7.3.2.1 to 7.3.2.3): the profile's compatibility flag set for its
 * own profile; no timing, HRD or VUI (they do not change the decoding);
 * the short-term sets in their explicit form; the scaling lists coded
 * explicitly.  EINVAL for a field H.265 cannot code (an id or a count out
 * of range, the screen content extension), ENOSPC when the unit does not
 * fit.
 */
int vpu_h265_write_vps(const struct vpu_h265_vps *vps, uint8_t *output, size_t capacity, size_t *written);
int vpu_h265_write_sps(const struct vpu_h265_sps *sps, uint8_t *output, size_t capacity, size_t *written);
int vpu_h265_write_pps(const struct vpu_h265_pps *pps, uint8_t *output, size_t capacity, size_t *written);

/* The most DPB slots and core frame buffers the bookkeeping holds. */
#define VPU_SLOTS_MAX			32U
#define VPU_BUFFERS_MAX			32U

/* No slot, no frame buffer. */
#define VPU_NONE			(-1)

/*
 * Which core frame buffer holds each DPB slot's picture, and the other
 * way round: buffer[slot] is the frame buffer the slot's picture was
 * decoded into, slot[index] the slot whose picture frame buffer index
 * holds; VPU_NONE for none.  count is how many frame buffers the core has;
 * used[index] is the number of the last picture decoded into frame buffer
 * index (0 for none yet), and serial the number of the last picture.
 */
struct vpu_slots {
	uint32_t count;
	int32_t buffer[VPU_SLOTS_MAX];
	int32_t slot[VPU_BUFFERS_MAX];
	uint32_t used[VPU_BUFFERS_MAX];
	uint32_t serial;
};

/* Forgets every binding: no slot holds a picture, and the core has count frame buffers. */
void vpu_slots_reset(struct vpu_slots *slots, uint32_t count);

/*
 * Picks the frame buffer a picture is decoded into: one no live slot
 * holds.  The live slots are those the picture references and those its
 * coding scope keeps active (a bit per slot each), less the setup slot,
 * the one the picture becomes (VPU_NONE for none), unless the picture
 * references it too: its old picture is replaced.  Of the free frame
 * buffers the one decoded into longest ago is picked, the one the core
 * most likely no longer references either.  ENOSPC when every frame
 * buffer holds a live slot's picture.
 */
int vpu_slots_pick(const struct vpu_slots *slots, uint32_t references, uint32_t active, int32_t setup, uint32_t *index);

/*
 * Reports the frame buffers (a bit each) the live slots of a picture hold,
 * the live slots as vpu_slots_pick() takes them: a picture may go into
 * any other.
 */
uint32_t vpu_slots_taken(const struct vpu_slots *slots, uint32_t references, uint32_t active, int32_t setup);

/*
 * Binds a decoded picture: the setup slot (VPU_NONE for none) now holds
 * frame buffer index, which no other slot holds any more, and the slot's
 * old frame buffer holds no slot.
 */
void vpu_slots_bind(struct vpu_slots *slots, int32_t setup, uint32_t index);

/*
 * Forgets the slots outside keep (a bit per slot): a coding scope's begin
 * names the slots it keeps; the others hold no picture from then on.
 */
void vpu_slots_keep(struct vpu_slots *slots, uint32_t keep);

#endif
