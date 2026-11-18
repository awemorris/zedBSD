/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The H.265 part of the executor's video decode (vk-video.c), which
 * touches no hardware and calls nothing of the kernel but the wire reader
 * (a host test runs it): the parameter sets and the picture of a decode
 * as the executor keeps them, read from libvulkan's wire; the checks of
 * what the HEVC decoder (HCP) takes; the slice segment headers read from
 * the bitstream, with their reference lists and weights; the tiles and
 * the scaling matrices of a picture.
 *
 * The decoder runs in the long format: it takes every slice segment
 * header's values from the commands, so the executor reads the headers
 * itself.  The bitstream is application memory; every read is bounded by
 * the segment.
 */

#ifndef DRIVERS_GPU_I915_VK_VK_VIDEO_H265_H
#define DRIVERS_GPU_I915_VK_VK_VIDEO_H265_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct i915_wire_reader;

/* The DPB slots of a session, and the entries one reference list has at most. */
#define I915_VIDEO_H265_SLOTS		17U
#define I915_VIDEO_H265_LIST		15U

/* The entries of each reference picture set a decode names, and the entry of none. */
#define I915_VIDEO_H265_SET_ENTRIES	8U
#define I915_VIDEO_H265_NO_SLOT		0xffU

/* The parameter set id ranges (VPS 0..15, SPS 0..15, PPS 0..63). */
#define I915_VIDEO_H265_VPS_IDS		16U
#define I915_VIDEO_H265_SPS_IDS		16U
#define I915_VIDEO_H265_PPS_IDS		64U

/* The tile column widths and row heights a picture parameter set sends, and the most tiles the decoder takes. */
#define I915_VIDEO_H265_COLUMN_WIDTHS	19U
#define I915_VIDEO_H265_ROW_HEIGHTS	21U
#define I915_VIDEO_H265_TILE_COLUMNS	20U
#define I915_VIDEO_H265_TILE_ROWS	22U

/* The picture's extent the decoder takes, in luma samples. */
#define I915_VIDEO_H265_MIN_SIDE	16U
#define I915_VIDEO_H265_MAX_SIDE	4096U

/*
 * The flags of the parameter sets, of a picture and of a reference as
 * libvulkan packs them (userland/desktop/libvulkan/video.c), each the
 * declared order of its StdVideo structure from bit 0.
 */
#define I915_VIDEO_H265_SPS_SEPARATE_PLANES	(1U << 1)
#define I915_VIDEO_H265_SPS_SCALING		(1U << 4)
#define I915_VIDEO_H265_SPS_SCALING_DATA	(1U << 5)
#define I915_VIDEO_H265_SPS_AMP			(1U << 6)
#define I915_VIDEO_H265_SPS_SAO			(1U << 7)
#define I915_VIDEO_H265_SPS_PCM			(1U << 8)
#define I915_VIDEO_H265_SPS_PCM_FILTER_OFF	(1U << 9)
#define I915_VIDEO_H265_SPS_LONG_TERM		(1U << 10)
#define I915_VIDEO_H265_SPS_TEMPORAL_MVP	(1U << 11)
#define I915_VIDEO_H265_SPS_STRONG_INTRA	(1U << 12)
#define I915_VIDEO_H265_SPS_RANGE_EXTENSION	(1U << 15)
#define I915_VIDEO_H265_SPS_RANGE_TOOLS		(0x1ffU << 16)
#define I915_VIDEO_H265_SPS_SCC_EXTENSION	(1U << 25)
#define I915_VIDEO_H265_PPS_DEPENDENT		(1U << 0)
#define I915_VIDEO_H265_PPS_OUTPUT_FLAG		(1U << 1)
#define I915_VIDEO_H265_PPS_SIGN_HIDING		(1U << 2)
#define I915_VIDEO_H265_PPS_CABAC_INIT		(1U << 3)
#define I915_VIDEO_H265_PPS_CONSTRAINED		(1U << 4)
#define I915_VIDEO_H265_PPS_TRANSFORM_SKIP	(1U << 5)
#define I915_VIDEO_H265_PPS_CU_QP_DELTA		(1U << 6)
#define I915_VIDEO_H265_PPS_SLICE_QP		(1U << 7)
#define I915_VIDEO_H265_PPS_WEIGHTED		(1U << 8)
#define I915_VIDEO_H265_PPS_BIPRED		(1U << 9)
#define I915_VIDEO_H265_PPS_BYPASS		(1U << 10)
#define I915_VIDEO_H265_PPS_TILES		(1U << 11)
#define I915_VIDEO_H265_PPS_ENTROPY_SYNC	(1U << 12)
#define I915_VIDEO_H265_PPS_UNIFORM		(1U << 13)
#define I915_VIDEO_H265_PPS_ACROSS_TILES	(1U << 14)
#define I915_VIDEO_H265_PPS_ACROSS_SLICES	(1U << 15)
#define I915_VIDEO_H265_PPS_DEBLOCKING_CONTROL	(1U << 16)
#define I915_VIDEO_H265_PPS_DEBLOCKING_OVERRIDE	(1U << 17)
#define I915_VIDEO_H265_PPS_DEBLOCKING_OFF	(1U << 18)
#define I915_VIDEO_H265_PPS_SCALING_DATA	(1U << 19)
#define I915_VIDEO_H265_PPS_LISTS_MODIFICATION	(1U << 20)
#define I915_VIDEO_H265_PPS_HEADER_EXTENSION	(1U << 21)
#define I915_VIDEO_H265_PPS_CROSS_COMPONENT	(1U << 23)
#define I915_VIDEO_H265_PPS_CHROMA_QP_LIST	(1U << 24)
#define I915_VIDEO_H265_PPS_SCC_TOOLS		(0x1fU << 25)
#define I915_VIDEO_H265_PICTURE_IRAP		(1U << 0)
#define I915_VIDEO_H265_PICTURE_IDR		(1U << 1)
#define I915_VIDEO_H265_PICTURE_REFERENCE	(1U << 2)
#define I915_VIDEO_H265_REFERENCE_LONG_TERM	(1U << 0)

/*
 * The scaling lists of a parameter set as sent: each list in the order the
 * syntax codes it (up-right diagonal), the 16x16 and 32x32 ones as their
 * 8x8 coded matrices (the 32x32 ones of matrixId 0 and 3), and their DC
 * values.
 */
struct i915_video_h265_scaling {
	uint8_t list4[6][16];
	uint8_t list8[6][64];
	uint8_t list16[6][64];
	uint8_t list32[2][64];
	uint8_t dc16[6];
	uint8_t dc32[2];
};

/* One sequence parameter set: the fields the decode reads, and its own scaling lists if any. */
struct i915_video_h265_sps {
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
	uint32_t profile_idc;
	uint32_t level_idc;
	bool has_scaling;
	struct i915_video_h265_scaling scaling;
};

/* One picture parameter set: the fields the decode reads, and its own scaling lists if any. */
struct i915_video_h265_pps {
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
	uint32_t column_width_minus1[I915_VIDEO_H265_COLUMN_WIDTHS];
	uint32_t row_height_minus1[I915_VIDEO_H265_ROW_HEIGHTS];
	bool has_scaling;
	struct i915_video_h265_scaling scaling;
};

/*
 * The H.265 information of the picture a decode writes: its flags, the
 * sets it names, the bits of its slice header's own short-term set, its
 * order count, and the slots of its three current reference picture sets
 * (I915_VIDEO_H265_NO_SLOT past their ends).
 */
struct i915_video_h265_picture {
	uint32_t flags;
	uint32_t vps_id;
	uint32_t sps_id;
	uint32_t pps_id;
	uint32_t delta_pocs;
	uint32_t short_term_bits;
	int32_t poc;
	uint8_t before[I915_VIDEO_H265_SET_ENTRIES];
	uint8_t after[I915_VIDEO_H265_SET_ENTRIES];
	uint8_t long_term[I915_VIDEO_H265_SET_ENTRIES];
};

/* What a decode knows of each DPB slot: whether a reference names it, its order count and whether it is long-term. */
struct i915_video_h265_slot {
	bool present;
	bool long_term;
	int32_t poc;
};

/*
 * The explicit weights of one slice (7.4.7.3): the two denominators, and
 * for each list and entry whether its luma and chroma weights were sent,
 * the luma weight (LumaWeightLX), the luma offset, the two chroma weights
 * (ChromaWeightLX) and the two chroma offsets (ChromaOffsetLX, clipped).
 */
struct i915_video_h265_weights {
	uint32_t luma_log2_denom;
	uint32_t chroma_log2_denom;
	bool luma_sent[2][I915_VIDEO_H265_LIST];
	bool chroma_sent[2][I915_VIDEO_H265_LIST];
	int32_t luma_weight[2][I915_VIDEO_H265_LIST];
	int32_t luma_offset[2][I915_VIDEO_H265_LIST];
	int32_t chroma_weight[2][I915_VIDEO_H265_LIST][2];
	int32_t chroma_offset[2][I915_VIDEO_H265_LIST][2];
};

/*
 * One slice segment, read from its header: the NAL unit's type, the
 * segment's place in the picture, the independent segment's fields
 * (copied into a dependent one, with the independent one's address as
 * slice_address), the reference lists as DPB slots (RefPicList0 and
 * RefPicList1 of 8.3.4), and where the NAL unit and its data start in the
 * segment's bytes: nal_offset after the start code, data_offset the data's
 * first byte (past any emulation prevention byte there), and header_bytes
 * the bytes of the NAL unit before its data with the emulation prevention
 * bytes taken out.
 */
struct i915_video_h265_slice {
	uint32_t nal_type;
	bool first_in_picture;
	bool dependent;
	uint32_t address;
	uint32_t slice_address;
	uint32_t slice_type;
	bool sao_luma;
	bool sao_chroma;
	bool temporal_mvp;
	uint32_t references[2];
	uint8_t list[2][I915_VIDEO_H265_LIST];
	bool mvd_l1_zero;
	bool cabac_init;
	bool collocated_from_l0;
	uint32_t collocated_ref_idx;
	bool weighted;
	struct i915_video_h265_weights weights;
	uint32_t max_merge_candidates;
	int32_t qp;
	int32_t cb_qp_offset;
	int32_t cr_qp_offset;
	bool deblocking_disabled;
	int32_t beta_div2;
	int32_t tc_div2;
	bool loop_filter_across_slices;
	uint32_t entry_points;
	size_t nal_offset;
	size_t data_offset;
	size_t header_bytes;
};

/* The slice types of slice_type. */
#define I915_VIDEO_H265_SLICE_B		0U
#define I915_VIDEO_H265_SLICE_P		1U
#define I915_VIDEO_H265_SLICE_I		2U

/*
 * The CTB geometry of a picture: the CTB's size, the picture in CTBs, and
 * the tiles' first column and row in CTBs (columns + 1 and rows + 1 of
 * them, the last the picture's edge).
 */
struct i915_video_h265_geometry {
	uint32_t log2_ctb;
	uint32_t width_ctbs;
	uint32_t height_ctbs;
	uint32_t columns;
	uint32_t rows;
	uint32_t column_bounds[I915_VIDEO_H265_TILE_COLUMNS + 1U];
	uint32_t row_bounds[I915_VIDEO_H265_TILE_ROWS + 1U];
};

/*
 * The scaling matrices a picture decodes with, in raster order: the six
 * 4x4, 8x8 and 16x16 lists (intra Y, Cb, Cr, then inter Y, Cb, Cr) and
 * the two 32x32 ones (intra, inter), the larger ones as their 8x8 coded
 * matrices, and the DC values of the 16x16 and the 32x32 lists.
 */
struct i915_video_h265_matrices {
	uint8_t list4[6][16];
	uint8_t list8[6][64];
	uint8_t list16[6][64];
	uint8_t list32[2][64];
	uint8_t dc16[6];
	uint8_t dc32[2];
};

/*
 * Reads one video parameter set from the wire (libvulkan's
 * video_encode_h265_vps) and reports its id; nothing else of it is kept.
 */
uint32_t drv_i915_video_h265_read_vps(struct i915_wire_reader *reader);

/* Reads one sequence parameter set from the wire (libvulkan's video_encode_h265_sps). */
void drv_i915_video_h265_read_sps(struct i915_wire_reader *reader, struct i915_video_h265_sps *sps);

/* Reads one picture parameter set from the wire (libvulkan's video_encode_h265_pps). */
void drv_i915_video_h265_read_pps(struct i915_wire_reader *reader, struct i915_video_h265_pps *pps);

/* Reads the H.265 information of a decode's picture from the wire (libvulkan's video_encode_h265_std_picture). */
void drv_i915_video_h265_read_picture(struct i915_wire_reader *reader, struct i915_video_h265_picture *picture);

/*
 * Checks that a sequence and picture parameter set pair is one the decoder
 * takes: Main's tools (8-bit 4:2:0, no range or screen content tools), 16
 * to 4096 samples a side, at most 20 tile columns.
 * Returns NULL, or why the pair is not taken.
 */
const char *drv_i915_video_h265_check(const struct i915_video_h265_sps *sps, const struct i915_video_h265_pps *pps);

/*
 * Reads one slice segment's header from its bytes (from its offset in the
 * bitstream to the next segment's, a start code prefix in front), with
 * the active sets and the picture's reference sets and slots.
 * independent is the picture's last independent segment, which a
 * dependent one copies (NULL for none).  Returns NULL, or why the segment
 * is not decoded.
 */
const char *drv_i915_video_h265_slice_read(const uint8_t *bytes, size_t length, const struct i915_video_h265_sps *sps,
					   const struct i915_video_h265_pps *pps, const struct i915_video_h265_picture *picture,
					   const struct i915_video_h265_slot *slots, const struct i915_video_h265_slice *independent,
					   struct i915_video_h265_slice *slice);

/* Makes a picture's CTB geometry and tile boundaries. */
void drv_i915_video_h265_geometry(const struct i915_video_h265_sps *sps, const struct i915_video_h265_pps *pps,
				  struct i915_video_h265_geometry *geometry);

/* Reports a CTB's place in the tile scan (6.5.1, CtbAddrRsToTs) from its raster address. */
uint32_t drv_i915_video_h265_tile_scan(const struct i915_video_h265_geometry *geometry, uint32_t address);

/*
 * Makes the scaling matrices a picture decodes with: the picture set's
 * lists, else the sequence's, else the default ones when scaling lists are
 * on, and flat ones (16) when they are off.
 */
void drv_i915_video_h265_matrices(const struct i915_video_h265_sps *sps, const struct i915_video_h265_pps *pps,
				  struct i915_video_h265_matrices *matrices);

#endif
