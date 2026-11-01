/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What a stateful video decoder core is given besides the picture's
 * slices: the H.264 parameter sets in the form Vulkan Video's StdVideo
 * structures carry them, written back into NAL units the core parses
 * (vpu-h264-ps.c); and the bookkeeping that ties the application's
 * decoded picture buffer slots to the core's frame buffers, so a picture
 * is never decoded into a frame buffer the core still references
 * (vpu-slots.c).  Neither part touches hardware: a host test runs both.
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
