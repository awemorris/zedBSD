/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Raspberry Pi 5's H.265 decode arithmetic, which touches no hardware
 * (a host test runs it): the parameter sets and the picture a decode names
 * as the executor keeps them, the slice segment headers it reads from the
 * bitstream, and the command list and register values the HEVC decoder
 * runs a picture with.
 *
 * The decoder is stateless: it takes every slice segment header's values
 * from the commands, so the executor reads the headers itself.  The
 * bitstream is application memory; every read is bounded by the segment.
 */

#ifndef KERN_DRIVERS_GPU_BCM2711_VULKAN_VIDEO_HEVC_H
#define KERN_DRIVERS_GPU_BCM2711_VULKAN_VIDEO_HEVC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The DPB slots of a session (the decoder's sixteen reference entries), and the references one decode names at most. */
#define BCM2712_HEVC_SLOTS		16U
#define BCM2712_HEVC_REFERENCES		15U

/* The entries of each reference picture set a decode names, and the entry of none. */
#define BCM2712_HEVC_SET_ENTRIES	8U
#define BCM2712_HEVC_NO_SLOT		0xffU

/* The tile column widths and row heights a picture parameter set sends (the last of each is derived). */
#define BCM2712_HEVC_COLUMN_WIDTHS	19U
#define BCM2712_HEVC_ROW_HEIGHTS	21U

/* The most tile columns and rows (one more than the widths and heights sent). */
#define BCM2712_HEVC_TILE_COLUMNS	20U
#define BCM2712_HEVC_TILE_ROWS		22U

/* The slice segments one picture may have: the decoder's message list counts them in eight bits. */
#define BCM2712_HEVC_SLICES		256U

/* The picture's extent the decoder takes, in luma samples. */
#define BCM2712_HEVC_MIN_SIDE		16U
#define BCM2712_HEVC_MAX_SIDE		4096U

/*
 * The flags of the parameter sets, of a picture and of a reference as
 * libvulkan packs them (userland/desktop/libvulkan/video.c:
 * video_h265_sps_flags, video_h265_pps_flags, video_h265_picture_flags,
 * video_h265_reference_flags), each the declared order of its StdVideo
 * structure from bit 0.
 */
#define BCM2712_HEVC_SPS_SEPARATE_PLANES	(1U << 1)
#define BCM2712_HEVC_SPS_SCALING		(1U << 4)
#define BCM2712_HEVC_SPS_SCALING_DATA		(1U << 5)
#define BCM2712_HEVC_SPS_AMP			(1U << 6)
#define BCM2712_HEVC_SPS_SAO			(1U << 7)
#define BCM2712_HEVC_SPS_PCM			(1U << 8)
#define BCM2712_HEVC_SPS_PCM_FILTER_OFF		(1U << 9)
#define BCM2712_HEVC_SPS_LONG_TERM		(1U << 10)
#define BCM2712_HEVC_SPS_TEMPORAL_MVP		(1U << 11)
#define BCM2712_HEVC_SPS_STRONG_INTRA		(1U << 12)
#define BCM2712_HEVC_SPS_RANGE_EXTENSION	(1U << 15)
#define BCM2712_HEVC_SPS_SCC_EXTENSION		(1U << 25)
#define BCM2712_HEVC_PPS_DEPENDENT		(1U << 0)
#define BCM2712_HEVC_PPS_OUTPUT_FLAG		(1U << 1)
#define BCM2712_HEVC_PPS_SIGN_HIDING		(1U << 2)
#define BCM2712_HEVC_PPS_CABAC_INIT		(1U << 3)
#define BCM2712_HEVC_PPS_CONSTRAINED		(1U << 4)
#define BCM2712_HEVC_PPS_TRANSFORM_SKIP		(1U << 5)
#define BCM2712_HEVC_PPS_CU_QP_DELTA		(1U << 6)
#define BCM2712_HEVC_PPS_SLICE_QP		(1U << 7)
#define BCM2712_HEVC_PPS_WEIGHTED		(1U << 8)
#define BCM2712_HEVC_PPS_BIPRED			(1U << 9)
#define BCM2712_HEVC_PPS_BYPASS			(1U << 10)
#define BCM2712_HEVC_PPS_TILES			(1U << 11)
#define BCM2712_HEVC_PPS_ENTROPY_SYNC		(1U << 12)
#define BCM2712_HEVC_PPS_UNIFORM		(1U << 13)
#define BCM2712_HEVC_PPS_ACROSS_TILES		(1U << 14)
#define BCM2712_HEVC_PPS_ACROSS_SLICES		(1U << 15)
#define BCM2712_HEVC_PPS_DEBLOCKING_CONTROL	(1U << 16)
#define BCM2712_HEVC_PPS_DEBLOCKING_OVERRIDE	(1U << 17)
#define BCM2712_HEVC_PPS_DEBLOCKING_OFF		(1U << 18)
#define BCM2712_HEVC_PPS_SCALING_DATA		(1U << 19)
#define BCM2712_HEVC_PPS_LISTS_MODIFICATION	(1U << 20)
#define BCM2712_HEVC_PPS_HEADER_EXTENSION	(1U << 21)
#define BCM2712_HEVC_PPS_CHROMA_QP_LIST		(1U << 24)
#define BCM2712_HEVC_PPS_CURRENT_PICTURE	(1U << 25)
#define BCM2712_HEVC_PPS_ACT_OFFSETS		(1U << 27)
#define BCM2712_HEVC_PICTURE_IRAP		(1U << 0)
#define BCM2712_HEVC_PICTURE_IDR		(1U << 1)
#define BCM2712_HEVC_PICTURE_REFERENCE		(1U << 2)
#define BCM2712_HEVC_PICTURE_SPS_SET		(1U << 3)
#define BCM2712_HEVC_REFERENCE_LONG_TERM	(1U << 0)

/*
 * The scaling lists of a parameter set as sent: each list in the order the
 * syntax codes it (up-right diagonal), the 16x16 and 32x32 ones as their
 * 8x8 coded matrices (the 32x32 ones of matrixId 0 and 3), and their DC
 * values.
 */
struct bcm2712_hevc_scaling {
	uint8_t list4[6][16];
	uint8_t list8[6][64];
	uint8_t list16[6][64];
	uint8_t list32[2][64];
	uint8_t dc16[6];
	uint8_t dc32[2];
};

/* One sequence parameter set: the fields the decode reads, and its own scaling lists if any. */
struct bcm2712_hevc_sps {
	uint32_t flags;
	uint32_t vps_id;
	uint32_t id;
	uint32_t chroma_format_idc;
	uint32_t width;
	uint32_t height;
	uint32_t bit_depth_luma_minus8;
	uint32_t bit_depth_chroma_minus8;
	uint32_t log2_max_poc_lsb_minus4;
	uint32_t log2_min_cb_minus3;
	uint32_t log2_diff_cb;
	uint32_t log2_min_tb_minus2;
	uint32_t log2_diff_tb;
	uint32_t depth_inter;
	uint32_t depth_intra;
	uint32_t short_term_sets;
	uint32_t long_term_sps;
	uint32_t pcm_depth_luma_minus1;
	uint32_t pcm_depth_chroma_minus1;
	uint32_t log2_min_pcm_minus3;
	uint32_t log2_diff_pcm;
	bool has_scaling;
	struct bcm2712_hevc_scaling scaling;
};

/* One picture parameter set: the fields the decode reads, and its own scaling lists if any. */
struct bcm2712_hevc_pps {
	uint32_t flags;
	uint32_t id;
	uint32_t sps_id;
	uint32_t vps_id;
	uint32_t extra_bits;
	uint32_t l0_minus1;
	uint32_t l1_minus1;
	uint32_t cu_qp_delta_depth;
	uint32_t log2_merge_minus2;
	uint32_t tile_columns_minus1;
	uint32_t tile_rows_minus1;
	int32_t init_qp_minus26;
	int32_t cb_qp_offset;
	int32_t cr_qp_offset;
	int32_t beta_div2;
	int32_t tc_div2;
	uint32_t column_width_minus1[BCM2712_HEVC_COLUMN_WIDTHS];
	uint32_t row_height_minus1[BCM2712_HEVC_ROW_HEIGHTS];
	bool has_scaling;
	struct bcm2712_hevc_scaling scaling;
};

/*
 * The H.265 information of the picture a decode writes: its flags, the
 * sets it names, the counts of its slice header's own short-term set, its
 * order count, and the slots of its three current reference picture sets
 * (BCM2712_HEVC_NO_SLOT past their ends).
 */
struct bcm2712_hevc_picture {
	uint32_t flags;
	uint32_t vps_id;
	uint32_t sps_id;
	uint32_t pps_id;
	uint32_t delta_pocs;
	uint32_t short_term_bits;
	int32_t poc;
	uint8_t before[BCM2712_HEVC_SET_ENTRIES];
	uint8_t after[BCM2712_HEVC_SET_ENTRIES];
	uint8_t long_term[BCM2712_HEVC_SET_ENTRIES];
};

/* What a decode knows of each DPB slot: whether a reference names it, its order count and whether it is long-term. */
struct bcm2712_hevc_slot {
	bool present;
	bool long_term;
	int32_t poc;
};

/*
 * The explicit weights of one slice (7.4.7.3) as the decoder takes them:
 * the two denominators, and for each list and entry the luma weight
 * (LumaWeightLX), the luma offset, the two chroma weights (ChromaWeightLX)
 * and the two chroma offsets (ChromaOffsetLX, clipped).
 */
struct bcm2712_hevc_weights {
	uint32_t luma_log2_denom;
	uint32_t chroma_log2_denom;
	int32_t luma_weight[2][BCM2712_HEVC_SLOTS];
	int32_t luma_offset[2][BCM2712_HEVC_SLOTS];
	int32_t chroma_weight[2][BCM2712_HEVC_SLOTS][2];
	int32_t chroma_offset[2][BCM2712_HEVC_SLOTS][2];
};

/*
 * One slice segment, read from its header: the NAL unit's type and
 * temporal id, the segment's place in the picture, the independent
 * segment's fields (copied into a dependent one), the reference lists as
 * DPB slots (RefPicList0 and RefPicList1 of 8.3.4), and where its
 * slice_segment_data() starts in the segment's bytes (data_offset, after
 * the header's byte alignment and any emulation prevention byte there)
 * and how many bytes follow (data_bytes).  segment_physical is the
 * segment's first byte's bus address, which the caller sets.
 */
struct bcm2712_hevc_slice {
	uint32_t nal_type;
	uint32_t temporal_id_plus1;
	bool first_in_picture;
	bool dependent;
	uint32_t address;
	uint32_t slice_type;
	bool sao_luma;
	bool sao_chroma;
	bool temporal_mvp;
	uint32_t references[2];
	uint8_t list[2][BCM2712_HEVC_SLOTS];
	bool mvd_l1_zero;
	bool cabac_init;
	bool collocated_from_l0;
	uint32_t collocated_ref_idx;
	bool weighted;
	struct bcm2712_hevc_weights weights;
	uint32_t max_merge_candidates;
	int32_t qp;
	int32_t cb_qp_offset;
	int32_t cr_qp_offset;
	bool deblocking_disabled;
	int32_t beta_div2;
	int32_t tc_div2;
	bool loop_filter_across_slices;
	uint32_t entry_points;
	size_t data_offset;
	size_t data_bytes;
	uint64_t segment_physical;
};

/* The slice types of slice_type. */
#define BCM2712_HEVC_SLICE_B		0U
#define BCM2712_HEVC_SLICE_P		1U
#define BCM2712_HEVC_SLICE_I		2U

/*
 * The command list of phase 1: pairs of a register offset and its value,
 * which the decoder writes in order.  count grows to capacity; a list that
 * would go past it sets overflow and stops growing.
 */
struct bcm2712_hevc_commands {
	uint32_t *pairs;
	uint32_t capacity;
	uint32_t count;
	bool overflow;
};

/*
 * The register values of phase 2 that come from the picture: CONFIG2,
 * FRAMESIZE and CURRPOC, the CTB rows, whether the picture's motion
 * vectors are written for later pictures, and the slot whose motion
 * vectors are read (BCM2712_HEVC_NO_SLOT for none).
 */
struct bcm2712_hevc_phase2 {
	uint32_t config2;
	uint32_t frame_size;
	uint32_t current_poc;
	uint32_t ctb_rows;
	bool write_motion;
	uint32_t collocated_slot;
};

/*
 * Checks that a sequence and picture parameter set pair is one the decoder
 * takes: Main (8-bit 4:2:0), 16 to 4096 samples a side, no range or
 * screen content tools, and not both tiles and wavefronts.  ENOTSUP for a
 * stream outside the decoder, EINVAL for values outside H.265.
 */
int bcm2712_hevc_check(const struct bcm2712_hevc_sps *sps, const struct bcm2712_hevc_pps *pps);

/*
 * Reads one slice segment's header from its bytes (from its offset in the
 * bitstream to the next segment's, a start code prefix allowed in front),
 * with the active sets and the picture's reference sets and slots.
 * independent is the picture's last independent segment, which a
 * dependent one copies (NULL for none).  Returns 0, EINVAL for a header
 * that does not parse within the bytes or breaks H.265, ENOTSUP for one
 * the decoder does not take.
 */
int bcm2712_hevc_slice_read(const uint8_t *bytes, size_t length, const struct bcm2712_hevc_sps *sps, const struct bcm2712_hevc_pps *pps, const struct bcm2712_hevc_picture *picture, const struct bcm2712_hevc_slot *slots, const struct bcm2712_hevc_slice *independent, struct bcm2712_hevc_slice *slice);

/*
 * Builds the phase 1 command list of a picture from its segments, in
 * order, and its phase 2 values.  Returns 0, EINVAL for segments that do
 * not form a picture (the first not at address 0, an address that goes
 * back), ENOSPC when the list does not fit the commands' capacity.
 */
int bcm2712_hevc_program(const struct bcm2712_hevc_sps *sps, const struct bcm2712_hevc_pps *pps, const struct bcm2712_hevc_picture *picture, const struct bcm2712_hevc_slot *slots, const struct bcm2712_hevc_slice *slices, uint32_t count, struct bcm2712_hevc_commands *commands, struct bcm2712_hevc_phase2 *phase2);

/*
 * The geometry of the decoder's SAND output of a picture: the luma
 * column's height (rows; the chroma column has half), the columns, and
 * the bytes of each plane.
 */
struct bcm2712_hevc_sand {
	uint32_t column_rows;
	uint32_t columns;
	uint64_t luma_bytes;
	uint64_t chroma_bytes;
};

/* Lays out a picture's SAND output for a width and height in luma samples. */
void bcm2712_hevc_sand_layout(uint32_t width, uint32_t height, struct bcm2712_hevc_sand *sand);

/* Reports the bytes of a picture's motion vector store, and its stride, for a width and height. */
void bcm2712_hevc_motion_layout(uint32_t width, uint32_t height, uint32_t *stride, uint64_t *bytes);

/* Reports the starting sizes of the PU and coefficient lists of phase 1 for a width and height. */
void bcm2712_hevc_list_sizes(uint32_t width, uint32_t height, uint64_t *pu_bytes, uint64_t *coefficient_bytes);

/* Reports the next size of a phase 1 list that ran out. */
uint64_t bcm2712_hevc_list_grow(uint64_t bytes);

#endif
