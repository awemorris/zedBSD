/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's H.264 and H.265 decode: the parameter sets as the
 * executor keeps them, and the arithmetic that turns a decode into the
 * video decoder's picture description (vk-video-h264.c and
 * vk-video-h265.c, which touch no hardware: a host test runs them).  The
 * wire, the objects and the run are vk-video.c's.
 */

#ifndef DRIVERS_GPU_NVRTX_VK_VIDEO_H
#define DRIVERS_GPU_NVRTX_VK_VIDEO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "drivers/gpu/nvrtx/nvrtx-nvdec.h"

/* The DPB slots a session has at most, and the references one decode names at most. */
#define NVRTX_VIDEO_SLOTS		17U
#define NVRTX_VIDEO_REFERENCES		16U

/*
 * The flags of the parameter sets and of a picture as libvulkan packs
 * them (userland/desktop/libvulkan/video.c: video_sps_flags,
 * video_pps_flags, video_picture_flags, video_reference_flags), each the
 * declared order of its StdVideo structure from bit 0.
 */
#define NVRTX_VIDEO_SPS_DIRECT_8X8	(1U << 6)
#define NVRTX_VIDEO_SPS_MBAFF		(1U << 7)
#define NVRTX_VIDEO_SPS_FRAME_MBS_ONLY	(1U << 8)
#define NVRTX_VIDEO_SPS_DELTA_POC_ZERO	(1U << 9)
#define NVRTX_VIDEO_SPS_SEPARATE_PLANES	(1U << 10)
#define NVRTX_VIDEO_SPS_BYPASS		(1U << 12)
#define NVRTX_VIDEO_SPS_SCALING		(1U << 14)
#define NVRTX_VIDEO_PPS_TRANSFORM_8X8	(1U << 0)
#define NVRTX_VIDEO_PPS_REDUNDANT	(1U << 1)
#define NVRTX_VIDEO_PPS_CONSTRAINED	(1U << 2)
#define NVRTX_VIDEO_PPS_DEBLOCKING	(1U << 3)
#define NVRTX_VIDEO_PPS_WEIGHTED	(1U << 4)
#define NVRTX_VIDEO_PPS_BOTTOM_POC	(1U << 5)
#define NVRTX_VIDEO_PPS_ENTROPY		(1U << 6)
#define NVRTX_VIDEO_PPS_SCALING		(1U << 7)
#define NVRTX_VIDEO_PICTURE_FIELD	(1U << 0)
#define NVRTX_VIDEO_PICTURE_REFERENCE	(1U << 4)
#define NVRTX_VIDEO_PICTURE_PAIR	(1U << 5)
#define NVRTX_VIDEO_REFERENCE_LONG_TERM	(1U << 2)
#define NVRTX_VIDEO_REFERENCE_MISSING	(1U << 3)

/*
 * The scaling lists of a parameter set as sent: which lists are present
 * and which of those take the default matrix, and the lists in their scan
 * (zig-zag) order, six 4x4 lists and six 8x8 lists.
 */
struct nvrtx_video_scaling {
	uint32_t present_mask;
	uint32_t default_mask;
	uint8_t list4[6][16];
	uint8_t list8[6][64];
};

/* One H.264 sequence parameter set: the fields the decode reads, and its own scaling lists if any. */
struct nvrtx_video_sps {
	uint32_t flags;
	uint32_t profile_idc;
	uint32_t chroma_format_idc;
	uint32_t id;
	uint32_t bit_depth_luma_minus8;
	uint32_t bit_depth_chroma_minus8;
	uint32_t log2_max_frame_num_minus4;
	uint32_t pic_order_cnt_type;
	uint32_t log2_max_pic_order_cnt_lsb_minus4;
	uint32_t max_num_ref_frames;
	uint32_t pic_width_in_mbs_minus1;
	uint32_t pic_height_in_map_units_minus1;
	bool has_scaling;
	struct nvrtx_video_scaling scaling;
};

/* One H.264 picture parameter set: the fields the decode reads, and its own scaling lists if any. */
struct nvrtx_video_pps {
	uint32_t flags;
	uint32_t sps_id;
	uint32_t pps_id;
	uint32_t num_ref_idx_l0_default_active_minus1;
	uint32_t num_ref_idx_l1_default_active_minus1;
	uint32_t weighted_bipred_idc;
	int32_t pic_init_qp_minus26;
	int32_t chroma_qp_index_offset;
	int32_t second_chroma_qp_index_offset;
	bool has_scaling;
	struct nvrtx_video_scaling scaling;
};

/*
 * The H.264 information of the picture a decode writes: its flags, the
 * sets it names, its frame number and its two picture order counts.
 */
struct nvrtx_video_picture {
	uint32_t flags;
	uint32_t sps_id;
	uint32_t pps_id;
	uint32_t frame_num;
	int32_t poc[2];
};

/*
 * The H.264 information of one reference picture: its flags, its frame
 * number (or long-term frame index) and its picture order counts.
 */
struct nvrtx_video_reference {
	uint32_t flags;
	uint32_t frame_num;
	int32_t poc[2];
};

/*
 * Which of the decoder's reference entries each DPB slot's picture has:
 * entry[n] for slot n, or -1 for none.  An entry stays with its picture
 * for as long as the picture stays a reference, as the decoder expects.
 */
struct nvrtx_video_dpb {
	int32_t entry[NVRTX_VIDEO_SLOTS];
};

void nvrtx_video_scaling_resolve(const struct nvrtx_video_sps *sps, const struct nvrtx_video_pps *pps, uint8_t scaling_4x4[6][16], uint8_t scaling_8x8[2][64]);
void nvrtx_video_dpb_reset(struct nvrtx_video_dpb *dpb);
void nvrtx_video_dpb_keep(struct nvrtx_video_dpb *dpb, const bool *active);
void nvrtx_video_dpb_forget(struct nvrtx_video_dpb *dpb, uint32_t slot);
int nvrtx_video_dpb_entry(struct nvrtx_video_dpb *dpb, uint32_t slot, uint32_t *entry);
uint32_t nvrtx_video_free_surface(const struct nvrtx_video_dpb *dpb, const uint32_t *referenced, uint32_t count);
void nvrtx_video_describe(const struct nvrtx_video_sps *sps, const struct nvrtx_video_pps *pps, const struct nvrtx_video_picture *picture, struct nvrtx_nvdec_h264_picture *description);
int32_t nvrtx_video_order(int32_t own, int32_t other);

/* The H.265 DPB slots a session has at most, and the references one decode names at most. */
#define NVRTX_VIDEO_H265_SLOTS		16U
#define NVRTX_VIDEO_H265_REFERENCES	15U

/* The entries of each reference picture set an H.265 decode names, and the entry of none. */
#define NVRTX_VIDEO_H265_SET_ENTRIES	8U
#define NVRTX_VIDEO_H265_NO_SLOT	0xffU

/* The tile column widths and row heights an H.265 picture parameter set sends (the last of each is derived). */
#define NVRTX_VIDEO_H265_COLUMN_WIDTHS	19U
#define NVRTX_VIDEO_H265_ROW_HEIGHTS	21U

/*
 * The H.265 flags of the parameter sets, of a picture and of a reference
 * as libvulkan packs them (userland/desktop/libvulkan/video.c:
 * video_h265_sps_flags, video_h265_pps_flags, video_h265_picture_flags,
 * video_h265_reference_flags), each the declared order of its StdVideo
 * structure from bit 0.
 */
#define NVRTX_VIDEO_H265_SPS_SEPARATE_PLANES	(1U << 1)
#define NVRTX_VIDEO_H265_SPS_SCALING		(1U << 4)
#define NVRTX_VIDEO_H265_SPS_SCALING_DATA	(1U << 5)
#define NVRTX_VIDEO_H265_SPS_AMP		(1U << 6)
#define NVRTX_VIDEO_H265_SPS_SAO		(1U << 7)
#define NVRTX_VIDEO_H265_SPS_PCM		(1U << 8)
#define NVRTX_VIDEO_H265_SPS_PCM_FILTER_OFF	(1U << 9)
#define NVRTX_VIDEO_H265_SPS_LONG_TERM		(1U << 10)
#define NVRTX_VIDEO_H265_SPS_TEMPORAL_MVP	(1U << 11)
#define NVRTX_VIDEO_H265_SPS_STRONG_INTRA	(1U << 12)
#define NVRTX_VIDEO_H265_PPS_DEPENDENT		(1U << 0)
#define NVRTX_VIDEO_H265_PPS_OUTPUT_FLAG	(1U << 1)
#define NVRTX_VIDEO_H265_PPS_SIGN_HIDING	(1U << 2)
#define NVRTX_VIDEO_H265_PPS_CABAC_INIT		(1U << 3)
#define NVRTX_VIDEO_H265_PPS_CONSTRAINED	(1U << 4)
#define NVRTX_VIDEO_H265_PPS_TRANSFORM_SKIP	(1U << 5)
#define NVRTX_VIDEO_H265_PPS_CU_QP_DELTA	(1U << 6)
#define NVRTX_VIDEO_H265_PPS_SLICE_QP		(1U << 7)
#define NVRTX_VIDEO_H265_PPS_WEIGHTED		(1U << 8)
#define NVRTX_VIDEO_H265_PPS_BIPRED		(1U << 9)
#define NVRTX_VIDEO_H265_PPS_BYPASS		(1U << 10)
#define NVRTX_VIDEO_H265_PPS_TILES		(1U << 11)
#define NVRTX_VIDEO_H265_PPS_ENTROPY_SYNC	(1U << 12)
#define NVRTX_VIDEO_H265_PPS_UNIFORM		(1U << 13)
#define NVRTX_VIDEO_H265_PPS_ACROSS_TILES	(1U << 14)
#define NVRTX_VIDEO_H265_PPS_ACROSS_SLICES	(1U << 15)
#define NVRTX_VIDEO_H265_PPS_DEBLOCKING_CONTROL	(1U << 16)
#define NVRTX_VIDEO_H265_PPS_DEBLOCKING_OVERRIDE	(1U << 17)
#define NVRTX_VIDEO_H265_PPS_DEBLOCKING_OFF	(1U << 18)
#define NVRTX_VIDEO_H265_PPS_SCALING_DATA	(1U << 19)
#define NVRTX_VIDEO_H265_PPS_LISTS_MODIFICATION	(1U << 20)
#define NVRTX_VIDEO_H265_PPS_HEADER_EXTENSION	(1U << 21)
#define NVRTX_VIDEO_H265_PICTURE_IRAP		(1U << 0)
#define NVRTX_VIDEO_H265_PICTURE_IDR		(1U << 1)
#define NVRTX_VIDEO_H265_PICTURE_REFERENCE	(1U << 2)
#define NVRTX_VIDEO_H265_PICTURE_SPS_SET	(1U << 3)
#define NVRTX_VIDEO_H265_REFERENCE_LONG_TERM	(1U << 0)
#define NVRTX_VIDEO_H265_REFERENCE_UNUSED	(1U << 1)

/*
 * The scaling lists of an H.265 parameter set as sent: each list in the
 * order the syntax codes it (up-right diagonal), the 16x16 and 32x32 ones
 * as their 8x8 coded matrices, and their DC values.
 */
struct nvrtx_video_h265_scaling {
	uint8_t list4[6][16];
	uint8_t list8[6][64];
	uint8_t list16[6][64];
	uint8_t list32[2][64];
	uint8_t dc16[6];
	uint8_t dc32[2];
};

/* One H.265 sequence parameter set: the fields the decode reads, and its own scaling lists if any. */
struct nvrtx_video_h265_sps {
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
	struct nvrtx_video_h265_scaling scaling;
};

/* One H.265 picture parameter set: the fields the decode reads, and its own scaling lists if any. */
struct nvrtx_video_h265_pps {
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
	uint32_t column_width_minus1[NVRTX_VIDEO_H265_COLUMN_WIDTHS];
	uint32_t row_height_minus1[NVRTX_VIDEO_H265_ROW_HEIGHTS];
	bool has_scaling;
	struct nvrtx_video_h265_scaling scaling;
};

/*
 * The H.265 information of the picture a decode writes: its flags, the
 * sets it names, the counts of its slice header's own short-term set,
 * its order count, and the slots of its three current reference picture
 * sets (NVRTX_VIDEO_H265_NO_SLOT past their ends).
 */
struct nvrtx_video_h265_picture {
	uint32_t flags;
	uint32_t vps_id;
	uint32_t sps_id;
	uint32_t pps_id;
	uint32_t delta_pocs;
	uint32_t short_term_bits;
	int32_t poc;
	uint8_t before[NVRTX_VIDEO_H265_SET_ENTRIES];
	uint8_t after[NVRTX_VIDEO_H265_SET_ENTRIES];
	uint8_t long_term[NVRTX_VIDEO_H265_SET_ENTRIES];
};

/* One H.265 reference a decode names: its slot, its flags and its order count. */
struct nvrtx_video_h265_reference {
	uint32_t slot;
	uint32_t flags;
	int32_t poc;
};

void nvrtx_video_h265_describe(const struct nvrtx_video_h265_sps *sps, const struct nvrtx_video_h265_pps *pps, const struct nvrtx_video_h265_picture *picture, struct nvrtx_nvdec_hevc_picture *description);
void nvrtx_video_h265_scaling(const struct nvrtx_video_h265_sps *sps, const struct nvrtx_video_h265_pps *pps, struct nvrtx_nvdec_hevc_scaling *scaling);
void nvrtx_video_h265_tiles(const struct nvrtx_video_h265_sps *sps, const struct nvrtx_video_h265_pps *pps, struct nvrtx_nvdec_hevc_tiles *tiles);
int nvrtx_video_h265_references(const struct nvrtx_video_h265_picture *picture, const struct nvrtx_video_h265_reference *references, uint32_t count, struct nvrtx_nvdec_hevc_picture *description, uint32_t *substitute);
uint32_t nvrtx_video_h265_free_surface(const struct nvrtx_video_h265_reference *references, uint32_t count);
int nvrtx_video_h265_skip_bits(const uint8_t *bytes, size_t length, const struct nvrtx_video_h265_sps *sps, const struct nvrtx_video_h265_pps *pps, const struct nvrtx_video_h265_picture *picture, uint32_t *bits);

/* The NV12 layout of a video image: its rows' bytes, and each plane's offset and rows. */
struct nvrtx_video_layout {
	uint32_t pitch;
	uint64_t plane_offset[2];
	uint32_t plane_rows[2];
	uint64_t bytes;
};

void nvrtx_video_layout(uint32_t width, uint32_t height, struct nvrtx_video_layout *layout);

#endif
