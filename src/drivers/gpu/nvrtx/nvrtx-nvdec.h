/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The video decoder (NVDEC) of the nvrtx driver.
 *
 * The decoder runs on a channel of its own (nvdec.c) in the work channel's
 * address space, so the memory the executor's users bind is at the same
 * GPU address for the decoder as for the graphics engine.  The firmware the
 * decoder runs is the GSP's: the driver only has the resource manager make
 * the channel, its engine context and the decoder's object.
 *
 * The decoder is stateless.  Each picture is one push buffer that names the
 * picture's parameters (a structure in memory, filled by
 * nvrtx_nvdec_h264_pic() or nvrtx_nvdec_hevc_pic()), the bitstream (and
 * for H.264 its slice offsets), the session's work buffers, and the luma
 * and chroma planes of up to seventeen surfaces (sixteen for H.265): the
 * picture being decoded and the pictures it refers to.  The parts here
 * touch no hardware (a host test runs them).
 */

#ifndef DRIVERS_GPU_NVRTX_NVDEC_H
#define DRIVERS_GPU_NVRTX_NVDEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "drivers/gpu/nvrtx/nvrtx-private.h"

struct nvrtx_gpu;

/*
 * The decoder's classes: Turing's (TU10x and TU11x), Ampere's GA10x and
 * Ada's.  Their methods and the picture structure are the same.
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/nvidia/generated/g_gpu_class_list.c (the NVDEC classes of each
 * chip) and src/common/sdk/nvidia/inc/class/clc4b0.h
 * (NVC4B0_VIDEO_DECODER).
 */
#define NVRTX_NVDEC_CLASS_TURING	0xc4b0U
#define NVRTX_NVDEC_CLASS_AMPERE	0xc7b0U
#define NVRTX_NVDEC_CLASS_ADA		0xc9b0U

/*
 * The first decoder's engine type (the "BSP") as the channel's allocation,
 * its bind and the promote of its context name it (the device information
 * table numbers it otherwise, objects.c).
 *
 * Value referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/class/cl2080_notification.h
 * (NV2080_ENGINE_TYPE_NVDEC0).
 */
#define NVRTX_ENGINE_NVDEC0		0x13U

/* The subchannel the decoder's class is bound to on its channel. */
#define NVRTX_NVDEC_SUBCHANNEL		4U

/*
 * The surfaces one picture names at most (sixteen references and the
 * picture itself), and the reference entries of the picture structure.
 *
 * Values referenced: NVIDIA open-gpu-doc classes/video/clc5b0.h
 * (NVC5B0_SET_PICTURE_LUMA_OFFSET0 to 16) and classes/video/nvdec_drv.h
 * (nvdec_h264_pic_s.dpb).
 */
#define NVRTX_NVDEC_SURFACES		17U
#define NVRTX_NVDEC_REFERENCES		16U

/*
 * The bytes of the H.264 picture structure, of the decoder's status, and
 * the words of the slice offset table (one for each slice and one after
 * the last).
 *
 * Values referenced: NVIDIA open-gpu-doc classes/video/nvdec_drv.h
 * (nvdec_h264_pic_s, 764 bytes; nvdec_status_s, 56 bytes), and Mesa
 * src/nouveau/vulkan/rust/video/decode/h264.rs (the 256-word slice
 * offset table).
 */
#define NVRTX_NVDEC_PIC_BYTES		764U
#define NVRTX_NVDEC_STATUS_BYTES	56U
#define NVRTX_NVDEC_SLICE_WORDS		256U

/* The most slices one picture has (one word of the table is the end). */
#define NVRTX_NVDEC_SLICES_MAX		(NVRTX_NVDEC_SLICE_WORDS - 1U)

/*
 * The decoder reads every address it is given as the GPU address shifted
 * right by 8: the buffers and planes it names lie on 256-byte boundaries.
 */
#define NVRTX_NVDEC_ADDRESS_ALIGN	256U

/* The most bytes the push buffer of one picture takes (nvrtx_nvdec_push_decode). */
#define NVRTX_NVDEC_DECODE_PUSH_BYTES	256U

/* The status word of one decode's status: the macroblocks it found in error. */
#define NVRTX_NVDEC_STATUS_ERRORS_AT	4U

/*
 * The resource manager's parameters the decoder's channel needs: the
 * decoder's object, the promote of its engine context and the list of the
 * falcons' context sizes.
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/nvos.h (NV_BSP_ALLOCATION_PARAMETERS) and
 * src/common/sdk/nvidia/inc/ctrl/ctrl2080/ctrl2080gpu.h
 * (NV2080_CTRL_CMD_GPU_PROMOTE_CTX, NV2080_CTRL_GPU_PROMOTE_CTX_PARAMS,
 * NV2080_CTRL_CMD_GPU_GET_CONSTRUCTED_FALCON_INFO,
 * NV2080_CTRL_GPU_GET_CONSTRUCTED_FALCON_INFO_PARAMS).
 */
#define NVRTX_NVDEC_ALLOC_PARAMS_BYTES	12U
#define NVRTX_NVDEC_PROMOTE_BYTES	560U
#define NVRTX_CTL_FALCON_INFO		0x208001b0U
#define NVRTX_FALCON_INFO_PARAMS_BYTES	1284U

/*
 * Where the decoder finds one picture's inputs and outputs: GPU addresses
 * in the decoder's space, each on a 256-byte boundary (0 for a surface the
 * picture does not name).  serial numbers the picture in the session;
 * error_frame is the surface the decoder conceals errors from.
 */
struct nvrtx_nvdec_frame {
	uint64_t pic;
	uint64_t bitstream;
	uint64_t slices;
	uint64_t coloc;
	uint64_t history;
	uint64_t status;
	uint64_t mbhist;
	uint64_t luma[NVRTX_NVDEC_SURFACES];
	uint64_t chroma[NVRTX_NVDEC_SURFACES];
	uint32_t serial;
	uint32_t error_frame;
};

/*
 * One picture a decode refers to, as the picture structure lists it: its
 * surface, whether it is a long-term reference, whether it is a frame the
 * bitstream skipped (non-existing), its two field order counts and its
 * frame number (or long-term frame index).
 */
struct nvrtx_nvdec_h264_reference {
	uint32_t surface;
	uint32_t long_term;
	uint32_t non_existing;
	int32_t field_order[2];
	uint32_t frame_index;
};

/*
 * One progressive H.264 picture as the decoder takes it: the fields of its
 * sequence and picture parameter sets, of the picture itself, its
 * references (each in the entry dpb_entry[n] names) and its scaling
 * matrices in raster order.  The sizes are those of the session's buffers
 * and of the planes' rows.
 */
struct nvrtx_nvdec_h264_picture {
	/* From the sequence parameter set. */
	uint32_t log2_max_poc_lsb_minus4;
	uint32_t delta_poc_always_zero;
	uint32_t frame_mbs_only;
	uint32_t width_mbs;
	uint32_t height_mbs;
	uint32_t direct_8x8_inference;
	uint32_t log2_max_frame_num_minus4;
	uint32_t chroma_format_idc;
	uint32_t poc_type;
	uint32_t transform_bypass;
	/* From the picture parameter set. */
	uint32_t entropy_coding;
	uint32_t bottom_field_poc_present;
	uint32_t ref_l0_minus1;
	uint32_t ref_l1_minus1;
	uint32_t deblocking_control_present;
	uint32_t redundant_pic_cnt_present;
	uint32_t transform_8x8;
	uint32_t weighted_pred;
	uint32_t weighted_bipred_idc;
	uint32_t constrained_intra_pred;
	int32_t pic_init_qp_minus26;
	int32_t chroma_qp_offset;
	int32_t second_chroma_qp_offset;
	/* From the picture. */
	uint32_t is_reference;
	uint32_t frame_num;
	int32_t field_order[2];
	uint32_t surface;
	/* The references, each placed in the picture structure's entry dpb_entry[n]. */
	uint32_t reference_count;
	struct nvrtx_nvdec_h264_reference references[NVRTX_NVDEC_REFERENCES];
	uint32_t dpb_entry[NVRTX_NVDEC_REFERENCES];
	/* The scaling matrices in raster order. */
	uint8_t scaling_4x4[6][16];
	uint8_t scaling_8x8[2][64];
	/* The bitstream's bytes and slices, the work buffers' and the planes' row bytes. */
	uint32_t stream_bytes;
	uint32_t slice_count;
	uint32_t mbhist_bytes;
	uint32_t history_bytes;
	uint32_t pitch;
};

/* The sizes of a session's three work buffers: the co-located motion data, the macroblock history and the history. */
struct nvrtx_nvdec_buffers {
	uint32_t coloc;
	uint32_t mbhist;
	uint32_t history;
};

/*
 * The bytes of the H.265 picture structure, the surfaces one H.265
 * picture names (the picture and its references share the sixteen), the
 * bytes of the scaling list buffer and of the tile buffer.
 *
 * Values referenced: NVIDIA open-gpu-doc classes/video/nvdec_drv.h
 * (nvdec_hevc_pic_s, 276 bytes laid out by a host compiler;
 * GIP_ASIC_SCALING_LIST_SIZE, 1024 bytes) and averne's FFmpeg nvtegra
 * libavcodec/nvtegra_hevc.c (nvtegra_hevc_decode_init: 0x900 bytes of
 * tile sizes; the sixteen pictures of ctx->refs).
 */
#define NVRTX_NVDEC_HEVC_PIC_BYTES	276U
#define NVRTX_NVDEC_HEVC_SURFACES	16U
#define NVRTX_NVDEC_HEVC_SCALING_BYTES	1024U
#define NVRTX_NVDEC_HEVC_TILE_BYTES	0x900U

/* The most tile columns and rows an H.265 picture has. */
#define NVRTX_NVDEC_HEVC_TILE_COLUMNS	20U
#define NVRTX_NVDEC_HEVC_TILE_ROWS	22U

/*
 * The status words an H.265 decode reports, besides the blocks in error
 * (NVRTX_NVDEC_STATUS_ERRORS_AT): the blocks decoded, the decode's error,
 * the first block and slice in error (the row in bits 15:0, the column in
 * bits 31:16) and its slice header's error.
 *
 * Values referenced: NVIDIA open-gpu-doc classes/video/nvdec_drv.h
 * (nvdec_status_s: mbs_correctly_decoded 0, error_status 12,
 * hevc.error_ctb_pos 44, hevc.error_slice_pos 48, slice_header_error_code
 * 52).
 */
#define NVRTX_NVDEC_STATUS_DECODED_AT	0U
#define NVRTX_NVDEC_STATUS_ERROR_AT	12U
#define NVRTX_NVDEC_STATUS_CTB_AT	44U
#define NVRTX_NVDEC_STATUS_SLICE_AT	48U
#define NVRTX_NVDEC_STATUS_HEADER_AT	52U

/*
 * The addresses one H.265 picture adds to a frame's: the scaling lists,
 * the tile sizes and the filter buffer (each on 256 bytes).
 */
struct nvrtx_nvdec_hevc_frame {
	uint64_t scaling;
	uint64_t tiles;
	uint64_t filter;
};

/*
 * One progressive 8-bit 4:2:0 H.265 picture as the decoder takes it: the
 * fields of its sequence and picture parameter sets (sizes as the
 * picture structure counts them, not as the syntax codes them), of the
 * picture and its references (indexed by surface), the bits of its slice
 * header the decoder skips (or, with dxva, the counts it parses the
 * header with), and the session's buffers' units.
 */
struct nvrtx_nvdec_hevc_picture {
	/* From the sequence parameter set. */
	uint32_t width;
	uint32_t height;
	uint32_t chroma_format_idc;
	uint32_t bit_depth_luma;
	uint32_t bit_depth_chroma;
	uint32_t log2_min_cb;
	uint32_t log2_max_cb;
	uint32_t log2_min_tb;
	uint32_t log2_max_tb;
	uint32_t depth_inter;
	uint32_t depth_intra;
	uint32_t scaling_enabled;
	uint32_t amp;
	uint32_t sao;
	uint32_t pcm;
	uint32_t pcm_depth_luma;
	uint32_t pcm_depth_chroma;
	uint32_t log2_min_pcm;
	uint32_t log2_max_pcm;
	uint32_t pcm_loop_filter_disabled;
	uint32_t temporal_mvp;
	uint32_t strong_intra_smoothing;
	/* From the picture parameter set. */
	uint32_t dependent_slices;
	uint32_t output_flag_present;
	uint32_t extra_header_bits;
	uint32_t sign_data_hiding;
	uint32_t cabac_init_present;
	uint32_t ref_l0_default;
	uint32_t ref_l1_default;
	uint32_t init_qp;
	uint32_t constrained_intra;
	uint32_t transform_skip;
	uint32_t cu_qp_delta;
	uint32_t cu_qp_delta_depth;
	int32_t cb_qp_offset;
	int32_t cr_qp_offset;
	int32_t beta_offset;
	int32_t tc_offset;
	uint32_t slice_chroma_qp_offsets;
	uint32_t weighted_pred;
	uint32_t weighted_bipred;
	uint32_t transquant_bypass;
	uint32_t tiles;
	uint32_t entropy_sync;
	uint32_t tile_columns;
	uint32_t tile_rows;
	uint32_t filter_across_tiles;
	uint32_t filter_across_slices;
	uint32_t deblocking_control;
	uint32_t deblocking_override;
	uint32_t deblocking_disabled;
	uint32_t lists_modification;
	uint32_t log2_parallel_merge;
	uint32_t header_extension;
	/* From the picture and its references, each reference by its surface. */
	uint32_t idr;
	uint32_t irap;
	uint32_t surface;
	uint32_t reference_count;
	uint32_t long_term_mask;
	uint8_t list0[16];
	uint8_t list1[16];
	int32_t order_difference[NVRTX_NVDEC_HEVC_SURFACES];
	/* How the decoder gets past the slice header's reference picture sets. */
	uint32_t skip_bits;
	uint32_t dxva;
	uint32_t separate_planes;
	uint32_t log2_max_poc_lsb_minus4;
	uint32_t short_term_sets;
	uint32_t long_term_sps;
	uint32_t long_term_present;
	uint32_t delta_pocs;
	uint32_t short_term_bits;
	/* The bitstream's bytes, the planes' row bytes, the session's buffers in 256-byte units. */
	uint32_t stream_bytes;
	uint32_t pitch;
	uint32_t coloc_units;
	uint32_t sao_units;
	uint32_t bsd_units;
	uint32_t filter_above_units;
	uint32_t sao_above_units;
};

/*
 * An H.265 picture's scaling lists in the raster order the decoder reads
 * them: the six 4x4, six 8x8, six 16x16 and two 32x32 lists (the larger
 * two as their 8x8 coded matrices), and the DC values of the 16x16 and
 * 32x32 lists.
 */
struct nvrtx_nvdec_hevc_scaling {
	uint8_t dc16[6];
	uint8_t dc32[2];
	uint8_t list4[6][16];
	uint8_t list8[6][64];
	uint8_t list16[6][64];
	uint8_t list32[2][64];
};

/*
 * An H.265 picture's tiles: how many columns and rows, each column's
 * width and each row's height in CTBs, and the CTB's size (log2).
 */
struct nvrtx_nvdec_hevc_tiles {
	uint32_t columns;
	uint32_t rows;
	uint32_t width[NVRTX_NVDEC_HEVC_TILE_COLUMNS];
	uint32_t height[NVRTX_NVDEC_HEVC_TILE_ROWS];
	uint32_t log2_ctb;
};

/*
 * The sizes of an H.265 session's two work buffers and where the filter
 * buffer's parts lie: one picture's co-located motion data and all of
 * it, the filter buffer, and the offsets in it of the SAO, the BSD
 * control, the deblocking above and the SAO above.
 */
struct nvrtx_nvdec_hevc_buffers {
	uint32_t coloc_picture;
	uint32_t coloc;
	uint32_t filter;
	uint32_t sao_at;
	uint32_t bsd_at;
	uint32_t filter_above_at;
	uint32_t sao_above_at;
};

uint32_t nvrtx_nvdec_class(enum nvrtx_family family);
size_t nvrtx_nvdec_push_object(uint8_t *push, uint32_t nvdec_class);
size_t nvrtx_nvdec_push_decode(uint8_t *push, const struct nvrtx_nvdec_frame *frame);
void nvrtx_nvdec_h264_pic(uint8_t *pic, const struct nvrtx_nvdec_h264_picture *picture);
void nvrtx_nvdec_slices(uint32_t *table, const uint32_t *offsets, uint32_t count, uint32_t stream_bytes);
void nvrtx_nvdec_buffer_sizes(uint32_t width_mbs, uint32_t height_map_units, uint32_t reference_frames, struct nvrtx_nvdec_buffers *buffers);
size_t nvrtx_nvdec_push_hevc(uint8_t *push, const struct nvrtx_nvdec_frame *frame, const struct nvrtx_nvdec_hevc_frame *hevc);
void nvrtx_nvdec_hevc_pic(uint8_t *pic, const struct nvrtx_nvdec_hevc_picture *picture);
void nvrtx_nvdec_hevc_scaling(uint8_t *buffer, const struct nvrtx_nvdec_hevc_scaling *scaling);
void nvrtx_nvdec_hevc_tiles(uint8_t *buffer, const struct nvrtx_nvdec_hevc_tiles *tiles, bool enabled);
void nvrtx_nvdec_hevc_buffer_sizes(uint32_t width, uint32_t height, uint32_t surfaces, struct nvrtx_nvdec_hevc_buffers *buffers);
void nvrtx_nvdec_alloc_params(uint8_t *params);
void nvrtx_nvdec_promote_params(uint8_t *params, uint32_t client, uint32_t chid, uint32_t channel, uint64_t va, uint64_t size);
int nvrtx_nvdec_context_bytes(const uint8_t *reply, size_t length, uint32_t engine_descriptor, uint32_t *bytes);

int nvrtx_nvdec_start(struct nvrtx_gpu *gpu);
void nvrtx_nvdec_stop(struct nvrtx_gpu *gpu);
void nvrtx_nvdec_release(struct nvrtx_gpu *gpu);
bool nvrtx_nvdec_ready(const struct nvrtx_gpu *gpu);
uint32_t nvrtx_nvdec_class_of(const struct nvrtx_gpu *gpu);
int nvrtx_nvdec_run(struct nvrtx_gpu *gpu, uint64_t push_va, size_t length, uint32_t timeout_us);

#endif
