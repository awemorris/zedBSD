/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The small part of H.264's bitstream syntax the VideoCore decoder's
 * driver needs: finding access units in an Annex B stream, and rewriting a
 * sequence parameter set with VUI that asks for no output reordering (so a
 * decoder that honours it outputs each picture as soon as it is decoded).
 */

#ifndef KERN_DRIVERS_RPI4_H264_H
#define KERN_DRIVERS_RPI4_H264_H

#include <stddef.h>
#include <stdint.h>

/* The NAL unit types the access unit split looks at. */
#define DRV_RPI4_H264_NAL_SLICE		1U
#define DRV_RPI4_H264_NAL_IDR		5U
#define DRV_RPI4_H264_NAL_SEI		6U
#define DRV_RPI4_H264_NAL_SPS		7U
#define DRV_RPI4_H264_NAL_PPS		8U
#define DRV_RPI4_H264_NAL_AUD		9U

/* The most bytes a rewritten parameter set may take. */
#define DRV_RPI4_H264_SPS_MAX		512U

/* chroma_format_idc of 4:2:0, the only format the push picture codes. */
#define DRV_RPI4_H264_CHROMA_420	1U

/* The SPS flags, one bit each, in the order StdVideoH264SpsFlags declares them. */
#define DRV_RPI4_H264_SPS_CONSTRAINT_SET0	0x0001U
#define DRV_RPI4_H264_SPS_CONSTRAINT_SET1	0x0002U
#define DRV_RPI4_H264_SPS_CONSTRAINT_SET2	0x0004U
#define DRV_RPI4_H264_SPS_CONSTRAINT_SET3	0x0008U
#define DRV_RPI4_H264_SPS_CONSTRAINT_SET4	0x0010U
#define DRV_RPI4_H264_SPS_CONSTRAINT_SET5	0x0020U
#define DRV_RPI4_H264_SPS_DIRECT_8X8		0x0040U
#define DRV_RPI4_H264_SPS_MB_ADAPTIVE		0x0080U
#define DRV_RPI4_H264_SPS_FRAME_MBS_ONLY	0x0100U
#define DRV_RPI4_H264_SPS_DELTA_POC_ZERO	0x0200U
#define DRV_RPI4_H264_SPS_SEPARATE_PLANES	0x0400U
#define DRV_RPI4_H264_SPS_GAPS_ALLOWED		0x0800U
#define DRV_RPI4_H264_SPS_QPPRIME_BYPASS	0x1000U
#define DRV_RPI4_H264_SPS_CROPPING		0x2000U
#define DRV_RPI4_H264_SPS_SCALING		0x4000U

/* The PPS flags, one bit each, in the order StdVideoH264PpsFlags declares them. */
#define DRV_RPI4_H264_PPS_TRANSFORM_8X8		0x0001U
#define DRV_RPI4_H264_PPS_REDUNDANT_PIC_CNT	0x0002U
#define DRV_RPI4_H264_PPS_CONSTRAINED_INTRA	0x0004U
#define DRV_RPI4_H264_PPS_DEBLOCKING_CONTROL	0x0008U
#define DRV_RPI4_H264_PPS_WEIGHTED_PRED		0x0010U
#define DRV_RPI4_H264_PPS_BOTTOM_FIELD_POC	0x0020U
#define DRV_RPI4_H264_PPS_CABAC			0x0040U
#define DRV_RPI4_H264_PPS_SCALING		0x0080U

/*
 * Scaling lists as StdVideoH264ScalingLists holds them: which lists are
 * present, which use the default matrix, and the lists in the order the
 * bitstream codes them.
 */
struct drv_rpi4_h264_scaling {
	uint32_t present_mask;
	uint32_t default_mask;
	uint8_t list4[6][16];
	uint8_t list8[6][64];
};

/*
 * A sequence parameter set, as StdVideoH264SequenceParameterSet gives it
 * (level as its enum).
 *
 * Procedure referenced: ITU-T H.264 7.3.2.1.1 (the sequence parameter set
 * syntax) and E.1.1 (the VUI syntax).
 */

struct drv_rpi4_h264_sps {
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
	struct drv_rpi4_h264_scaling scaling;
};

/* A picture parameter set, as StdVideoH264PictureParameterSet gives it. */
struct drv_rpi4_h264_pps {
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
	struct drv_rpi4_h264_scaling scaling;
};

/*
 * Writes an SPS NAL unit (a start code, then the unit with emulation
 * prevention) from its fields.  With no_reorder the VUI asks for no
 * reordering, as drv_rpi4_h264_sps_no_reorder() writes it; without, no VUI
 * is written.
 */
int drv_rpi4_h264_write_sps(const struct drv_rpi4_h264_sps *sps, int no_reorder, uint8_t *output, size_t capacity, size_t *written);

/*
 * Writes a PPS NAL unit from its fields; the SPS it refers to gives the
 * chroma format its scaling lists depend on.
 */
int drv_rpi4_h264_write_pps(const struct drv_rpi4_h264_pps *pps, const struct drv_rpi4_h264_sps *sps, uint8_t *output, size_t capacity, size_t *written);

/*
 * Writes a picture that pushes the picture before it out of a decoder: a
 * PPS of its own (CAVLC, no deblocking) with id pps_id, and one
 * non-reference I slice whose every macroblock is grey (Intra 16x16 DC
 * without residual).
 *
 * The slice follows a reference picture whose frame_num is the one before
 * frame_num.  A non-reference picture changes neither the references nor
 * the picture order count the stream's next picture derives, so the stream
 * decodes on as if the push were not there.  poc_lsb is its
 * pic_order_cnt_lsb (picture order count type 0 only).  The SPS must code
 * 4:2:0 frame pictures in one colour plane.  The slice's RBSP is written
 * into scratch first; output receives both NAL units, each after a start
 * code.
 */
int drv_rpi4_h264_write_push(const struct drv_rpi4_h264_sps *sps, uint32_t pps_id, uint32_t frame_num, uint32_t poc_lsb, uint8_t *scratch, size_t scratch_bytes, uint8_t *output, size_t capacity, size_t *written);

/*
 * Finds the next NAL unit of an Annex B stream at or after offset.
 *
 * start receives the offset of the NAL unit's first byte (after its start
 * code) and end the offset of the next start code or the stream's end.
 * Returns ENOENT when no NAL unit follows.
 */
int drv_rpi4_h264_next_nal(const uint8_t *stream, size_t bytes, size_t offset, size_t *start, size_t *end);

/*
 * Finds the end of the access unit that starts at offset.
 *
 * An access unit ends before the NAL unit that starts the next one: an
 * AUD, SPS, PPS or SEI after a slice, or a slice whose first macroblock is
 * zero after a slice.  end receives the offset of that NAL unit's start
 * code, or the stream's end.
 */
int drv_rpi4_h264_access_unit(const uint8_t *stream, size_t bytes, size_t offset, size_t *end);

/*
 * Rewrites one SPS NAL unit with VUI that asks for no reordering.
 *
 * nal is the NAL unit without its start code; output receives a start
 * code and the rewritten NAL unit, with emulation prevention.  Every field
 * before the VUI is kept bit for bit; the VUI is replaced by one whose only
 * content is the bitstream restriction: no limit on the bytes or bits of a
 * picture, motion vectors over picture boundaries allowed, 2^15 quarter
 * samples of motion, max_num_reorder_frames 0 and max_dec_frame_buffering
 * equal to max_num_ref_frames (at least 1).
 */
int drv_rpi4_h264_sps_no_reorder(const uint8_t *nal, size_t bytes, uint8_t *output, size_t capacity, size_t *written);

#endif
