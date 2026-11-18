/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The video decoder's push buffers, picture structures and resource
 * manager parameters, which touch no hardware (a host test runs them).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/nvrtx/nvrtx-gsp.h"
#include "drivers/gpu/nvrtx/nvrtx-nvdec.h"

/*
 * A method header: incrementing, then the count, the subchannel and the
 * method's word address; and the method that binds a class to a
 * subchannel.
 *
 * Values referenced: NVIDIA open-gpu-doc classes/host/clc46f.h
 * (NVC46F_DMA_INCR_OPCODE, NVC46F_DMA_INCR_COUNT,
 * NVC46F_DMA_INCR_SUBCHANNEL, NVC46F_DMA_INCR_ADDRESS, NVC46F_SET_OBJECT).
 */
#define RECORDS_METHOD_INCREMENT	(1U << 29)
#define RECORDS_METHOD_COUNT_SHIFT	16U
#define RECORDS_METHOD_SUBCHANNEL_SHIFT	13U
#define RECORDS_SET_OBJECT		0x0000U

/*
 * The decoder's methods and the fields of its control word.
 *
 * Values referenced: NVIDIA open-gpu-doc classes/video/clc5b0.h
 * (NVC5B0_SET_APPLICATION_ID and its H264 value, NVC5B0_EXECUTE,
 * NVC5B0_SET_CONTROL_PARAMS and its CODEC_TYPE, GPTIMER_ON,
 * ERR_CONCEAL_ON, ERROR_FRM_IDX and MBTIMER_ON fields,
 * NVC5B0_SET_DRV_PIC_SETUP_OFFSET, NVC5B0_SET_IN_BUF_BASE_OFFSET,
 * NVC5B0_SET_PICTURE_INDEX, NVC5B0_SET_SLICE_OFFSETS_BUF_OFFSET,
 * NVC5B0_SET_COLOC_DATA_OFFSET, NVC5B0_SET_HISTORY_OFFSET,
 * NVC5B0_SET_NVDEC_STATUS_OFFSET, NVC5B0_SET_PICTURE_LUMA_OFFSET0,
 * NVC5B0_SET_PICTURE_CHROMA_OFFSET0, NVC5B0_H264_SET_MBHIST_BUF_OFFSET).
 */
#define RECORDS_APPLICATION_ID		0x200U
#define RECORDS_APPLICATION_H264	3U
#define RECORDS_EXECUTE			0x300U
#define RECORDS_CONTROL			0x400U
#define RECORDS_CONTROL_CODEC_H264	3U
#define RECORDS_CONTROL_GPTIMER		(1U << 4)
#define RECORDS_CONTROL_CONCEAL		(1U << 6)
#define RECORDS_CONTROL_ERROR_SHIFT	7U
#define RECORDS_CONTROL_ERROR_MASK	0x3fU
#define RECORDS_CONTROL_MBTIMER		(1U << 13)
#define RECORDS_PIC_SETUP		0x404U
#define RECORDS_IN_BUFFER		0x408U
#define RECORDS_PICTURE_INDEX		0x40cU
#define RECORDS_SLICE_OFFSETS		0x410U
#define RECORDS_COLOC			0x414U
#define RECORDS_HISTORY			0x418U
#define RECORDS_STATUS			0x424U
#define RECORDS_LUMA			0x430U
#define RECORDS_CHROMA			0x474U
#define RECORDS_MBHIST			0x500U

/*
 * Where the fields of the H.264 picture structure lie: byte offsets, and
 * the bit fields of the words at 108, 176 and 180 and 720.
 *
 * Values referenced: NVIDIA open-gpu-doc classes/video/nvdec_drv.h
 * (nvdec_h264_pic_s, laid out by a host compiler: eos at 52,
 * explicitEOSPresentFlag 68, stream_len 72 ... ssm 752).
 */
#define PIC_EOS_AT			52U
#define PIC_EOS_PRESENT_AT		68U
#define PIC_STREAM_BYTES_AT		72U
#define PIC_SLICE_COUNT_AT		76U
#define PIC_MBHIST_BYTES_AT		80U
#define PIC_TIMEOUT_AT			84U
#define PIC_LOG2_POC_LSB_AT		88U
#define PIC_DELTA_POC_ZERO_AT		92U
#define PIC_FRAME_MBS_ONLY_AT		96U
#define PIC_WIDTH_MBS_AT		100U
#define PIC_HEIGHT_MBS_AT		104U
#define PIC_SURFACE_AT			108U
#define PIC_ENTROPY_AT			112U
#define PIC_POC_PRESENT_AT		116U
#define PIC_REF_L0_AT			120U
#define PIC_REF_L1_AT			124U
#define PIC_DEBLOCKING_AT		128U
#define PIC_REDUNDANT_AT		132U
#define PIC_TRANSFORM_8X8_AT		136U
#define PIC_PITCH_LUMA_AT		140U
#define PIC_PITCH_CHROMA_AT		144U
#define PIC_LUMA_BOTTOM_AT		152U
#define PIC_CHROMA_BOTTOM_AT		164U
#define PIC_HISTORY_UNITS_AT		172U
#define PIC_FLAGS_AT			176U
#define PIC_INDICES_AT			180U
#define PIC_FIELD_ORDER_AT		184U
#define PIC_DPB_AT			192U
#define PIC_DPB_ENTRY_BYTES		16U
#define PIC_SCALING_4X4_AT		448U
#define PIC_SCALING_8X8_AT		544U
#define PIC_LOSSLESS_AT			720U

/* The bit fields of the surface word (108): the tiling, then the GOB height. */
#define PIC_TILE_FORMAT_LOW		0U
#define PIC_GOB_HEIGHT_LOW		2U

/* The bit fields of the flag word (176). */
#define PIC_FLAG_DIRECT_8X8		1U
#define PIC_FLAG_WEIGHTED_PRED		2U
#define PIC_FLAG_CONSTRAINED_INTRA	3U
#define PIC_FLAG_REFERENCE		4U
#define PIC_LOG2_FRAME_NUM_LOW		8U
#define PIC_CHROMA_FORMAT_LOW		12U
#define PIC_POC_TYPE_LOW		14U
#define PIC_QP_LOW			16U
#define PIC_CHROMA_QP_LOW		22U
#define PIC_SECOND_CHROMA_QP_LOW	27U

/* The bit fields of the index word (180). */
#define PIC_BIPRED_LOW			0U
#define PIC_CURRENT_LOW			2U
#define PIC_CURRENT_COLOC_LOW		9U
#define PIC_FRAME_NUM_LOW		14U

/* The bit field of the lossless word (720). */
#define PIC_TRANSFORM_BYPASS		1U

/* The bit fields of a reference entry's first word, and its other words. */
#define DPB_INDEX_LOW			0U
#define DPB_COLOC_LOW			7U
#define DPB_STATE_LOW			12U
#define DPB_LONG_TERM			14U
#define DPB_NON_EXISTING		15U
#define DPB_TOP_MARKING_LOW		17U
#define DPB_BOTTOM_MARKING_LOW		21U
#define DPB_FIELD_ORDER_AT		4U
#define DPB_FRAME_INDEX_AT		12U

/*
 * The values the picture structure takes for every picture: the block
 * linear layout of desktop GPUs in blocks two GOBs high, a frame's
 * reference state and markings (short and long term), the decoder's
 * timeout, and the end of stream the decoder appends to the bitstream.
 *
 * Values referenced: NVIDIA open-gpu-doc classes/video/nvdec_drv.h
 * (nvdec_h264_pic_s.tileFormat, gob_height; nvdec_dpb_entry_s.state) and
 * Mesa main 921d028a5fff2beb4ec90451913b5fcc05fb1e6a
 * src/nouveau/vulkan/rust/video/decode/h264.rs (EOS_ARRAY, the
 * gptimer_timeout_value, the markings).
 */
#define PIC_TILE_FORMAT_KBL		1U
#define PIC_GOB_HEIGHT_TWO		0U
#define PIC_TIMEOUT			81000000U
#define DPB_STATE_FRAME			3U
#define DPB_MARKING_SHORT		1U
#define DPB_MARKING_LONG		2U

/* The sizes' rounding and the history's unit. */
#define RECORDS_BUFFER_ALIGN		0x100U
#define RECORDS_HISTORY_ALIGN		0x200U
#define RECORDS_HISTORY_UNIT_SHIFT	8U

/*
 * The falcons' list: its count, then entries of five words, the engine's
 * descriptor in the first and its context's size in the third.
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/ctrl/ctrl2080/ctrl2080gpu.h
 * (NV2080_CTRL_GPU_GET_CONSTRUCTED_FALCON_INFO_PARAMS,
 * NV2080_CTRL_GPU_CONSTRUCTED_FALCON_INFO,
 * NV2080_CTRL_GPU_MAX_CONSTRUCTED_FALCONS).
 */
#define FALCON_COUNT_AT			0U
#define FALCON_ENTRIES_AT		4U
#define FALCON_ENTRY_BYTES		20U
#define FALCON_DESCRIPTOR_AT		0U
#define FALCON_CONTEXT_BYTES_AT		8U
#define FALCON_MAX			64U

/*
 * The promote of an engine context by its address alone: the engine, the
 * client, the channel's number, the channel's client and handle, the
 * address and size, and no entries.
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/ctrl/ctrl2080/ctrl2080gpu.h
 * (NV2080_CTRL_GPU_PROMOTE_CTX_PARAMS: engineType 0, hClient 4, ChID 8,
 * hChanClient 12, hObject 16, hVirtMemory 20, virtAddress 24, size 32,
 * entryCount 40).
 */
#define PROMOTE_ENGINE_AT		0U
#define PROMOTE_CLIENT_AT		4U
#define PROMOTE_CHID_AT			8U
#define PROMOTE_CHANNEL_CLIENT_AT	12U
#define PROMOTE_OBJECT_AT		16U
#define PROMOTE_ADDRESS_AT		24U
#define PROMOTE_SIZE_AT			32U
#define PROMOTE_COUNT_AT		40U

/*
 * The decoder object's parameters: their size, single instance not
 * required, the first decoder.
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/nvos.h (NV_BSP_ALLOCATION_PARAMETERS).
 */
#define ALLOC_SIZE_AT			0U
#define ALLOC_PROHIBIT_AT		4U
#define ALLOC_INSTANCE_AT		8U

/*
 * The H.265 decode's methods: its codec in the application and the
 * control word, and the scaling list, tile size and filter buffers.
 *
 * Values referenced: NVIDIA open-gpu-doc classes/video/clc5b0.h
 * (NVC5B0_SET_APPLICATION_ID_ID_HEVC, NVC5B0_SET_CONTROL_PARAMS_CODEC_TYPE_HEVC,
 * NVC5B0_HEVC_SET_SCALING_LIST_OFFSET, NVC5B0_HEVC_SET_TILE_SIZES_OFFSET,
 * NVC5B0_HEVC_SET_FILTER_BUFFER_OFFSET).
 */
#define RECORDS_APPLICATION_HEVC	7U
#define RECORDS_CONTROL_CODEC_HEVC	7U
#define RECORDS_HEVC_SCALING		0x580U
#define RECORDS_HEVC_TILES		0x584U
#define RECORDS_HEVC_FILTER		0x588U

/*
 * Where the fields of the H.265 picture structure lie: byte offsets, and
 * the bit fields of the words at 64, 92, 96, 100, 108 and 212.
 *
 * Values referenced: NVIDIA open-gpu-doc classes/video/nvdec_drv.h
 * (nvdec_hevc_pic_s, laid out by a host compiler: stream_len at 48,
 * gptimer_timeout_value 60, tileformat 64 ...
 * num_bits_short_term_ref_pics_in_slice 216,
 * v1.hevc_main10_444_ext.HevcFltAboveOffset 240 and HevcSaoAboveOffset
 * 244).
 */
#define HEVC_STREAM_BYTES_AT		48U
#define HEVC_TIMEOUT_AT			60U
#define HEVC_SURFACE_AT			64U
#define HEVC_START_CODES_AT		65U
#define HEVC_OUTPUT_MODE_AT		66U
#define HEVC_PITCH_LUMA_AT		68U
#define HEVC_PITCH_CHROMA_AT		72U
#define HEVC_COLOC_UNITS_AT		76U
#define HEVC_SAO_UNITS_AT		80U
#define HEVC_BSD_UNITS_AT		84U
#define HEVC_WIDTH_AT			88U
#define HEVC_HEIGHT_AT			90U
#define HEVC_SIZES_AT			92U
#define HEVC_TOOLS_AT			96U
#define HEVC_PPS_AT			100U
#define HEVC_CB_QP_AT			104U
#define HEVC_CR_QP_AT			105U
#define HEVC_BETA_AT			106U
#define HEVC_TC_AT			107U
#define HEVC_PPS_FLAGS_AT		108U
#define HEVC_REFERENCES_AT		112U
#define HEVC_LONG_TERM_AT		114U
#define HEVC_LIST0_AT			116U
#define HEVC_LIST1_AT			132U
#define HEVC_ORDER_AT			148U
#define HEVC_IDR_AT			180U
#define HEVC_RAP_AT			181U
#define HEVC_CURRENT_AT			182U
#define HEVC_PATTERN_AT			183U
#define HEVC_SKIP_AT			184U
#define HEVC_DXVA_AT			212U
#define HEVC_SHORT_TERM_BITS_AT		216U
#define HEVC_FILTER_ABOVE_AT		240U
#define HEVC_SAO_ABOVE_AT		244U

/*
 * The range extension word of the H.265 picture structure (224): the
 * largest transform skip block, which the decoder takes as its log2 (2,
 * a 4x4 block, the only one a Main picture skips; 0 there desynchronizes
 * the first picture that skips a transform).
 *
 * Value referenced: NVIDIA open-gpu-doc classes/video/nvdec_drv.h
 * (nvdec_hevc_main10_444_ext_s.log2MaxTransformSkipSize, at 224 in
 * nvdec_hevc_pic_s laid out by a host compiler).
 */
#define HEVC_RANGE_EXTENSION_AT		224U
#define HEVC_SKIP_SIZE_LOW		0U
#define HEVC_SKIP_SIZE_MAIN		2U

/* The bit fields of the size word (92). */
#define HEVC_CHROMA_FORMAT_LOW		0U
#define HEVC_DEPTH_LUMA_LOW		4U
#define HEVC_DEPTH_CHROMA_LOW		8U
#define HEVC_MIN_CB_LOW			12U
#define HEVC_MAX_CB_LOW			16U
#define HEVC_MIN_TB_LOW			20U
#define HEVC_MAX_TB_LOW			24U

/* The bit fields of the coding tool word (96). */
#define HEVC_DEPTH_INTER_LOW		0U
#define HEVC_DEPTH_INTRA_LOW		3U
#define HEVC_SCALING_BIT		6U
#define HEVC_AMP_BIT			7U
#define HEVC_SAO_BIT			8U
#define HEVC_PCM_BIT			9U
#define HEVC_PCM_LUMA_LOW		10U
#define HEVC_PCM_CHROMA_LOW		14U
#define HEVC_MIN_PCM_LOW		18U
#define HEVC_MAX_PCM_LOW		22U
#define HEVC_PCM_FILTER_OFF_BIT		26U
#define HEVC_TEMPORAL_MVP_BIT		27U
#define HEVC_STRONG_INTRA_BIT		28U

/* The bit fields of the first picture parameter set word (100). */
#define HEVC_DEPENDENT_BIT		0U
#define HEVC_OUTPUT_FLAG_BIT		1U
#define HEVC_EXTRA_BITS_LOW		2U
#define HEVC_SIGN_HIDING_BIT		5U
#define HEVC_CABAC_INIT_BIT		6U
#define HEVC_REF_L0_LOW			7U
#define HEVC_REF_L1_LOW			11U
#define HEVC_INIT_QP_LOW		15U
#define HEVC_CONSTRAINED_BIT		22U
#define HEVC_TRANSFORM_SKIP_BIT		23U
#define HEVC_CU_QP_DELTA_BIT		24U
#define HEVC_CU_QP_DEPTH_LOW		25U

/* The bit fields of the second picture parameter set word (108). */
#define HEVC_SLICE_QP_BIT		0U
#define HEVC_WEIGHTED_BIT		1U
#define HEVC_BIPRED_BIT			2U
#define HEVC_BYPASS_BIT			3U
#define HEVC_TILES_BIT			4U
#define HEVC_ENTROPY_SYNC_BIT		5U
#define HEVC_TILE_COLUMNS_LOW		6U
#define HEVC_TILE_ROWS_LOW		11U
#define HEVC_ACROSS_TILES_BIT		16U
#define HEVC_ACROSS_SLICES_BIT		17U
#define HEVC_DEBLOCKING_CONTROL_BIT	18U
#define HEVC_DEBLOCKING_OVERRIDE_BIT	19U
#define HEVC_DEBLOCKING_OFF_BIT		20U
#define HEVC_LISTS_MODIFICATION_BIT	21U
#define HEVC_MERGE_LEVEL_LOW		22U
#define HEVC_HEADER_EXTENSION_BIT	25U

/* The bit fields of the word of the slice header's counts (212). */
#define HEVC_SEPARATE_PLANES_BIT	0U
#define HEVC_POC_LSB_LOW		1U
#define HEVC_SHORT_TERM_SETS_LOW	5U
#define HEVC_LONG_TERM_SPS_LOW		12U
#define HEVC_DELTA_POCS_LOW		19U
#define HEVC_LONG_TERM_PRESENT_BIT	27U

/*
 * The values every H.265 picture structure takes: no dithering of an
 * 8-bit output, an 8-bit Rec. 709 output, a stream with start codes, and
 * the decoder's own timeout.
 *
 * Values referenced: NVIDIA open-gpu-doc classes/video/nvdec_drv.h
 * (nvdec_hevc_pic_s.sw_start_code_e, disp_output_mode,
 * gptimer_timeout_value: 0 for the default) and averne's FFmpeg nvtegra
 * libavcodec/nvtegra_hevc.c (nvtegra_hevc_prepare_frame_setup: pattern_id
 * 2 for an 8-bit output).
 */
#define HEVC_PATTERN_NONE		2U
#define HEVC_OUTPUT_8BIT_709		0U
#define HEVC_STREAM_START_CODES		1U
#define HEVC_TIMEOUT_DEFAULT		0U

/*
 * The scaling list buffer's parts: the eight DC values, eight bytes to a
 * 16-byte boundary, then the 4x4, 8x8, 16x16 and 32x32 lists.
 *
 * Values referenced: NVIDIA open-gpu-doc classes/video/nvdec_drv.h
 * (GIP_ASIC_SCALING_LIST_SIZE: 8 dc, 8 to the boundary, 6*16, 2*6*64,
 * 2*64) and averne's FFmpeg nvtegra libavutil/nvdec_drv.h
 * (nvdec_hevc_scaling_list_s, the order of the parts).
 */
#define SCALING_DC16_AT			0U
#define SCALING_DC32_AT			6U
#define SCALING_LIST4_AT		16U
#define SCALING_LIST8_AT		112U
#define SCALING_LIST16_AT		496U
#define SCALING_LIST32_AT		880U

/*
 * The tile buffer: each tile's width and height in CTBs from its start
 * (16-bit words, the tiles in raster order), and from word 0x380 each
 * column's and then each row's right or bottom edge in 16-pixel units.
 *
 * Procedure referenced: averne's FFmpeg nvtegra libavcodec/nvtegra_hevc.c
 * (nvtegra_hevc_set_tile_sizes, nvtegra_hevc_prepare_frame_setup).
 */
#define TILES_EDGES_WORD		0x380U
#define TILES_EDGE_LOG2			4U

/*
 * The filter buffer of an H.265 session: for each row of the picture
 * (its height on 64 rows) the vertical filter's, the SAO's and the BSD
 * control's bytes, one part after the other; after them the deblocking
 * above and the SAO above, for each 16-pixel column of the width and two
 * more.  The co-located data of a picture is a sixteenth of the bytes of
 * its area on 64 pixels.
 *
 * The decoder of the Turing GPUs writes more than these sizes say: the
 * deblocking above part grows with every row of CTBs, not only with the
 * width (about 150 bytes for each 16-pixel column of each row of CTBs,
 * whatever the CTB's size: 640 bytes a column are used up after a few
 * rows, and the SAO above part written after it then overwrites what the
 * deblocking keeps for the next row), and the SAO above part takes more
 * than 768 bytes a column too (past the buffer's end with tiles).  Each
 * of the two parts is therefore at least 320 bytes for each 16x16 block
 * of the picture (the smallest CTB's rows), and the SAO above part at
 * least four times 768 bytes a column.
 *
 * Values referenced: averne's FFmpeg nvtegra libavcodec/nvtegra_hevc.c
 * (FILTER_SIZE 480, SAO_SIZE 3840, BSD_SIZE 60, CTU_SIZE 64;
 * nvtegra_hevc_decode_init, colmv_size) and NVIDIA open-gpu-doc
 * classes/video/nvdec_drv.h (HEVC_DBLK_TOP_SIZE_IN_SB16 align(632, 128),
 * HEVC_SAO_LEFT_SIZE_IN_SB16 align(713, 128), HEVC_DBLK_TOP_BUF_SIZE).
 */
#define FILTER_ROW_BYTES		480U
#define FILTER_SAO_ROW_BYTES		3840U
#define FILTER_BSD_ROW_BYTES		60U
#define FILTER_CTB			64U
#define FILTER_ABOVE_COLUMN_BYTES	640U
#define FILTER_SAO_ABOVE_COLUMN_BYTES	(4U * 768U)
#define FILTER_ABOVE_BLOCK_BYTES	320U
#define FILTER_COLUMN			16U
#define COLOC_AREA_DIVISOR		16U

static size_t records_method(uint8_t *push, size_t at, uint32_t method, uint32_t count);
static size_t records_address(uint8_t *push, size_t at, uint32_t method, uint64_t address);
static void records_bits(uint8_t *bytes, size_t at, uint32_t low, uint32_t bits, uint32_t value);
static void records_put16(uint8_t *bytes, size_t at, uint16_t value);
static void records_reference(uint8_t *pic, uint32_t entry, const struct nvrtx_nvdec_h264_reference *reference);
static uint32_t records_align(uint32_t value, uint32_t boundary);
static uint32_t records_column(uint32_t index, uint32_t side);
static void records_clear(uint8_t *bytes, size_t length);

/*
 * The end of stream the decoder appends after the bitstream: two end of
 * stream units (start code 00 00 01, type 11), each padded to eight bytes.
 *
 * Value referenced: Mesa main 921d028a5fff2beb4ec90451913b5fcc05fb1e6a
 * src/nouveau/vulkan/rust/video/decode/h264.rs (EOS_ARRAY).
 */
static const uint8_t records_end_of_stream[16] = {
	0x00U, 0x00U, 0x01U, 0x0bU, 0x00U, 0x00U, 0x00U, 0x00U,
	0x00U, 0x00U, 0x01U, 0x0bU, 0x00U, 0x00U, 0x00U, 0x00U
};

/*
 * Reports the decoder's class of a GPU family, or 0 for a family without
 * one the driver drives.
 */
uint32_t
nvrtx_nvdec_class(
	enum nvrtx_family family)
{
	/* Each family's class. */
	switch (family) {
	case NVRTX_FAMILY_TURING:
		return NVRTX_NVDEC_CLASS_TURING;
	case NVRTX_FAMILY_AMPERE:
		return NVRTX_NVDEC_CLASS_AMPERE;
	case NVRTX_FAMILY_ADA:
		return NVRTX_NVDEC_CLASS_ADA;
	default:
		break;
	}

	/* No decoder for the others. */
	return 0U;
}

/*
 * Writes the push buffer that binds the decoder's class to its
 * subchannel.
 *
 * Returns the length in bytes.
 */
size_t
nvrtx_nvdec_push_object(
	uint8_t *push,
	uint32_t nvdec_class)
{
	size_t at;

	/* The class on the decoder's subchannel. */
	at = records_method(push, 0U, RECORDS_SET_OBJECT, 1U);
	nvrtx_put32(push, at, nvdec_class);
	at += 4U;

	/* Succeeded: the push buffer's length. */
	return at;
}

/*
 * Writes the push buffer of one H.264 picture's decode: the codec and its
 * control word, the picture structure, the bitstream, the picture's
 * number, the slice offsets, the session's buffers, the status, the
 * seventeen surfaces' luma and chroma planes, then the start.
 *
 * Returns the length in bytes (at most NVRTX_NVDEC_DECODE_PUSH_BYTES).
 *
 * Procedure referenced: Mesa main 921d028a5fff2beb4ec90451913b5fcc05fb1e6a
 * src/nouveau/vulkan/rust/video/decode/h264.rs (Decoder::decode, the order
 * of the methods and the control word's fields).
 */
size_t
nvrtx_nvdec_push_decode(
	uint8_t *push,
	const struct nvrtx_nvdec_frame *frame)
{
	uint32_t control;
	uint32_t index;
	size_t at;

	/* The codec. */
	at = records_method(push, 0U, RECORDS_APPLICATION_ID, 1U);
	nvrtx_put32(push, at, RECORDS_APPLICATION_H264);
	at += 4U;

	/* The control word: H.264, both timers, errors concealed from the error frame. */
	control = RECORDS_CONTROL_CODEC_H264;
	control |= RECORDS_CONTROL_GPTIMER;
	control |= RECORDS_CONTROL_CONCEAL;
	control |= (frame->error_frame & RECORDS_CONTROL_ERROR_MASK) << RECORDS_CONTROL_ERROR_SHIFT;
	control |= RECORDS_CONTROL_MBTIMER;
	at = records_method(push, at, RECORDS_CONTROL, 1U);
	nvrtx_put32(push, at, control);
	at += 4U;

	/* The picture structure and the bitstream. */
	at = records_address(push, at, RECORDS_PIC_SETUP, frame->pic);
	at = records_address(push, at, RECORDS_IN_BUFFER, frame->bitstream);

	/* The picture's number in the session. */
	at = records_method(push, at, RECORDS_PICTURE_INDEX, 1U);
	nvrtx_put32(push, at, frame->serial);
	at += 4U;

	/* The slice offsets, the session's co-located data and history, and the status. */
	at = records_address(push, at, RECORDS_SLICE_OFFSETS, frame->slices);
	at = records_address(push, at, RECORDS_COLOC, frame->coloc);
	at = records_address(push, at, RECORDS_HISTORY, frame->history);
	at = records_address(push, at, RECORDS_STATUS, frame->status);

	/* Every surface's luma plane. */
	at = records_method(push, at, RECORDS_LUMA, NVRTX_NVDEC_SURFACES);
	for (index = 0U; index < NVRTX_NVDEC_SURFACES; index++)
		nvrtx_put32(push, at + (size_t)index * 4U, (uint32_t)(frame->luma[index] >> 8));
	at += (size_t)NVRTX_NVDEC_SURFACES * 4U;

	/* The chroma planes, after the luma planes. */
	at = records_method(push, at, RECORDS_CHROMA, NVRTX_NVDEC_SURFACES);
	for (index = 0U; index < NVRTX_NVDEC_SURFACES; index++)
		nvrtx_put32(push, at + (size_t)index * 4U, (uint32_t)(frame->chroma[index] >> 8));
	at += (size_t)NVRTX_NVDEC_SURFACES * 4U;

	/* The session's macroblock history. */
	at = records_address(push, at, RECORDS_MBHIST, frame->mbhist);

	/* Starts the decode, without a notification. */
	at = records_method(push, at, RECORDS_EXECUTE, 1U);
	nvrtx_put32(push, at, 0U);
	at += 4U;

	/* Succeeded: the push buffer's length. */
	return at;
}

/*
 * Fills the H.264 picture structure of one progressive picture.
 *
 * pic holds NVRTX_NVDEC_PIC_BYTES; every field the description does not
 * set is zero (no field or MBAFF picture, no MVC, NV12 output).
 *
 * Procedure referenced: Mesa main 921d028a5fff2beb4ec90451913b5fcc05fb1e6a
 * src/nouveau/vulkan/rust/video/decode/h264.rs (Decoder::decode, which
 * fields come from where; the bottom fields' offsets).
 */
void
nvrtx_nvdec_h264_pic(
	uint8_t *pic,
	const struct nvrtx_nvdec_h264_picture *picture)
{
	uint32_t index;
	uint32_t row;

	/* Starts empty. */
	records_clear(pic, NVRTX_NVDEC_PIC_BYTES);

	/* The end of stream the decoder appends, and the bitstream's length with it. */
	for (index = 0U; index < sizeof(records_end_of_stream); index++)
		pic[PIC_EOS_AT + index] = records_end_of_stream[index];
	pic[PIC_EOS_PRESENT_AT] = 1U;
	nvrtx_put32(pic, PIC_STREAM_BYTES_AT, picture->stream_bytes + (uint32_t)sizeof(records_end_of_stream));
	nvrtx_put32(pic, PIC_SLICE_COUNT_AT, picture->slice_count);

	/* The macroblock history's size and the decoder's timeout. */
	nvrtx_put32(pic, PIC_MBHIST_BYTES_AT, picture->mbhist_bytes);
	nvrtx_put32(pic, PIC_TIMEOUT_AT, PIC_TIMEOUT);

	/* The sequence's fields. */
	nvrtx_put32(pic, PIC_LOG2_POC_LSB_AT, picture->log2_max_poc_lsb_minus4);
	nvrtx_put32(pic, PIC_DELTA_POC_ZERO_AT, picture->delta_poc_always_zero);
	nvrtx_put32(pic, PIC_FRAME_MBS_ONLY_AT, picture->frame_mbs_only);
	nvrtx_put32(pic, PIC_WIDTH_MBS_AT, picture->width_mbs);
	nvrtx_put32(pic, PIC_HEIGHT_MBS_AT, picture->height_mbs);

	/* The surfaces: block linear in blocks of two GOBs. */
	records_bits(pic, PIC_SURFACE_AT, PIC_TILE_FORMAT_LOW, 2U, PIC_TILE_FORMAT_KBL);
	records_bits(pic, PIC_SURFACE_AT, PIC_GOB_HEIGHT_LOW, 3U, PIC_GOB_HEIGHT_TWO);

	/* The picture parameter set's fields. */
	nvrtx_put32(pic, PIC_ENTROPY_AT, picture->entropy_coding);
	nvrtx_put32(pic, PIC_POC_PRESENT_AT, picture->bottom_field_poc_present);
	nvrtx_put32(pic, PIC_REF_L0_AT, picture->ref_l0_minus1);
	nvrtx_put32(pic, PIC_REF_L1_AT, picture->ref_l1_minus1);
	nvrtx_put32(pic, PIC_DEBLOCKING_AT, picture->deblocking_control_present);
	nvrtx_put32(pic, PIC_REDUNDANT_AT, picture->redundant_pic_cnt_present);
	nvrtx_put32(pic, PIC_TRANSFORM_8X8_AT, picture->transform_8x8);

	/*
	 * The planes' rows, and where a bottom field would start (a frame's
	 * decode does not use them; the values are those of a frame).
	 */
	nvrtx_put32(pic, PIC_PITCH_LUMA_AT, picture->pitch);
	nvrtx_put32(pic, PIC_PITCH_CHROMA_AT, picture->pitch);
	nvrtx_put32(pic, PIC_LUMA_BOTTOM_AT, picture->width_mbs * 16U);
	nvrtx_put32(pic, PIC_CHROMA_BOTTOM_AT, picture->pitch / 2U);

	/* The history's size in its 256-byte unit. */
	nvrtx_put32(pic, PIC_HISTORY_UNITS_AT, picture->history_bytes >> RECORDS_HISTORY_UNIT_SHIFT);

	/* The flag word: the inference, the predictions, the reference, the sequence's sizes and the quantizers. */
	records_bits(pic, PIC_FLAGS_AT, PIC_FLAG_DIRECT_8X8, 1U, picture->direct_8x8_inference);
	records_bits(pic, PIC_FLAGS_AT, PIC_FLAG_WEIGHTED_PRED, 1U, picture->weighted_pred);
	records_bits(pic, PIC_FLAGS_AT, PIC_FLAG_CONSTRAINED_INTRA, 1U, picture->constrained_intra_pred);
	records_bits(pic, PIC_FLAGS_AT, PIC_FLAG_REFERENCE, 1U, picture->is_reference);
	records_bits(pic, PIC_FLAGS_AT, PIC_LOG2_FRAME_NUM_LOW, 4U, picture->log2_max_frame_num_minus4);
	records_bits(pic, PIC_FLAGS_AT, PIC_CHROMA_FORMAT_LOW, 2U, picture->chroma_format_idc);
	records_bits(pic, PIC_FLAGS_AT, PIC_POC_TYPE_LOW, 2U, picture->poc_type);
	records_bits(pic, PIC_FLAGS_AT, PIC_QP_LOW, 6U, (uint32_t)picture->pic_init_qp_minus26);
	records_bits(pic, PIC_FLAGS_AT, PIC_CHROMA_QP_LOW, 5U, (uint32_t)picture->chroma_qp_offset);
	records_bits(pic, PIC_FLAGS_AT, PIC_SECOND_CHROMA_QP_LOW, 5U, (uint32_t)picture->second_chroma_qp_offset);

	/* The index word: the bi-prediction, the picture's surface (its co-located data too) and its frame number. */
	records_bits(pic, PIC_INDICES_AT, PIC_BIPRED_LOW, 2U, picture->weighted_bipred_idc);
	records_bits(pic, PIC_INDICES_AT, PIC_CURRENT_LOW, 7U, picture->surface);
	records_bits(pic, PIC_INDICES_AT, PIC_CURRENT_COLOC_LOW, 5U, picture->surface);
	records_bits(pic, PIC_INDICES_AT, PIC_FRAME_NUM_LOW, 16U, picture->frame_num);

	/* The picture's two field order counts. */
	nvrtx_put32(pic, PIC_FIELD_ORDER_AT, (uint32_t)picture->field_order[0]);
	nvrtx_put32(pic, PIC_FIELD_ORDER_AT + 4U, (uint32_t)picture->field_order[1]);

	/* Each reference in its entry. */
	for (index = 0U; index < picture->reference_count && index < NVRTX_NVDEC_REFERENCES; index++)
		records_reference(pic, picture->dpb_entry[index], &picture->references[index]);

	/* The scaling matrices, in the raster order the description has them in. */
	for (index = 0U; index < 6U; index++) {
		for (row = 0U; row < 16U; row++)
			pic[PIC_SCALING_4X4_AT + index * 16U + row] = picture->scaling_4x4[index][row];
	}

	/* The 8x8 scaling matrices, in the same raster order. */
	for (index = 0U; index < 2U; index++) {
		for (row = 0U; row < 64U; row++)
			pic[PIC_SCALING_8X8_AT + index * 64U + row] = picture->scaling_8x8[index][row];
	}

	/* The transform bypass of the lossless profiles. */
	records_bits(pic, PIC_LOSSLESS_AT, PIC_TRANSFORM_BYPASS, 1U, picture->transform_bypass);
}

/*
 * Fills the slice offset table: each slice's first byte in the bitstream,
 * then the bitstream's length after the last.  The table holds
 * NVRTX_NVDEC_SLICE_WORDS words; count is at most NVRTX_NVDEC_SLICES_MAX.
 */
void
nvrtx_nvdec_slices(
	uint32_t *table,
	const uint32_t *offsets,
	uint32_t count,
	uint32_t stream_bytes)
{
	uint32_t index;

	/* Starts empty. */
	for (index = 0U; index < NVRTX_NVDEC_SLICE_WORDS; index++)
		table[index] = 0U;

	/* Keeps to the table. */
	if (count > NVRTX_NVDEC_SLICES_MAX)
		count = NVRTX_NVDEC_SLICES_MAX;

	/* Each slice's start, then the end of the last. */
	for (index = 0U; index < count; index++)
		table[index] = offsets[index];
	table[count] = stream_bytes;
}

/*
 * Reports the sizes of a session's work buffers for pictures width_mbs
 * macroblocks wide and height_map_units map units tall that keep
 * reference_frames references.
 *
 * Values referenced: Mesa main 921d028a5fff2beb4ec90451913b5fcc05fb1e6a
 * src/nouveau/vulkan/rust/video/decode/h264.rs
 * (compute_opaque_buffer_sizes).
 */
void
nvrtx_nvdec_buffer_sizes(
	uint32_t width_mbs,
	uint32_t height_map_units,
	uint32_t reference_frames,
	struct nvrtx_nvdec_buffers *buffers)
{
	uint32_t coloc;

	/* The co-located motion data: 64 bytes a macroblock of a pair of map unit rows, for each frame kept and the one decoded. */
	coloc = records_align(height_map_units, 2U) * width_mbs * 64U - 63U;
	coloc = records_align(coloc, RECORDS_BUFFER_ALIGN);
	buffers->coloc = coloc * (reference_frames + 1U);

	/* The macroblock history: 104 bytes a macroblock of a row. */
	buffers->mbhist = records_align(width_mbs * 104U, RECORDS_BUFFER_ALIGN);

	/* The history: 0x300 bytes a macroblock of a row. */
	buffers->history = records_align(width_mbs * 0x300U, RECORDS_HISTORY_ALIGN);
}

/*
 * Writes the push buffer of one H.265 picture's decode: the codec and its
 * control word, the picture's number, the picture structure, the
 * bitstream, the status, the scaling list, tile size and filter buffers,
 * the co-located data, the sixteen surfaces' luma and chroma planes, then
 * the start.  The decoder finds the slice segments by their start codes;
 * the H.264 slice table and histories are not named.
 *
 * Returns the length in bytes (at most NVRTX_NVDEC_DECODE_PUSH_BYTES).
 *
 * Procedure referenced: averne's FFmpeg nvtegra libavcodec/nvtegra_hevc.c
 * (nvtegra_hevc_prepare_cmdbuf: the methods, the control word's codec,
 * error concealment and timer).
 */
size_t
nvrtx_nvdec_push_hevc(
	uint8_t *push,
	const struct nvrtx_nvdec_frame *frame,
	const struct nvrtx_nvdec_hevc_frame *hevc)
{
	uint32_t control;
	uint32_t index;
	size_t at;

	/* The codec. */
	at = records_method(push, 0U, RECORDS_APPLICATION_ID, 1U);
	nvrtx_put32(push, at, RECORDS_APPLICATION_HEVC);
	at += 4U;

	/* The control word: H.265, errors concealed, the decoder's timer. */
	control = RECORDS_CONTROL_CODEC_HEVC;
	control |= RECORDS_CONTROL_CONCEAL;
	control |= RECORDS_CONTROL_GPTIMER;
	at = records_method(push, at, RECORDS_CONTROL, 1U);
	nvrtx_put32(push, at, control);
	at += 4U;

	/* The picture's number in the session. */
	at = records_method(push, at, RECORDS_PICTURE_INDEX, 1U);
	nvrtx_put32(push, at, frame->serial);
	at += 4U;

	/* The picture structure, the bitstream and the status. */
	at = records_address(push, at, RECORDS_PIC_SETUP, frame->pic);
	at = records_address(push, at, RECORDS_IN_BUFFER, frame->bitstream);
	at = records_address(push, at, RECORDS_STATUS, frame->status);

	/* The scaling lists, the tile sizes, the filter buffer and the co-located data. */
	at = records_address(push, at, RECORDS_HEVC_SCALING, hevc->scaling);
	at = records_address(push, at, RECORDS_HEVC_TILES, hevc->tiles);
	at = records_address(push, at, RECORDS_HEVC_FILTER, hevc->filter);
	at = records_address(push, at, RECORDS_COLOC, frame->coloc);

	/* The sixteen surfaces' luma planes. */
	at = records_method(push, at, RECORDS_LUMA, NVRTX_NVDEC_HEVC_SURFACES);
	for (index = 0U; index < NVRTX_NVDEC_HEVC_SURFACES; index++)
		nvrtx_put32(push, at + (size_t)index * 4U, (uint32_t)(frame->luma[index] >> 8));
	at += (size_t)NVRTX_NVDEC_HEVC_SURFACES * 4U;

	/* Their chroma planes. */
	at = records_method(push, at, RECORDS_CHROMA, NVRTX_NVDEC_HEVC_SURFACES);
	for (index = 0U; index < NVRTX_NVDEC_HEVC_SURFACES; index++)
		nvrtx_put32(push, at + (size_t)index * 4U, (uint32_t)(frame->chroma[index] >> 8));
	at += (size_t)NVRTX_NVDEC_HEVC_SURFACES * 4U;

	/* Starts the decode, without a notification. */
	at = records_method(push, at, RECORDS_EXECUTE, 1U);
	nvrtx_put32(push, at, 0U);
	at += 4U;

	/* Succeeded: the push buffer's length. */
	return at;
}

/*
 * Fills the H.265 picture structure of one progressive picture.
 *
 * pic holds NVRTX_NVDEC_HEVC_PIC_BYTES; every field the description does
 * not set is zero (no encryption, no histogram, no multi-layer, no slice
 * level decode).
 *
 * Procedure referenced: averne's FFmpeg nvtegra libavcodec/nvtegra_hevc.c
 * (nvtegra_hevc_prepare_frame_setup, which fields come from where).
 */
void
nvrtx_nvdec_hevc_pic(
	uint8_t *pic,
	const struct nvrtx_nvdec_hevc_picture *picture)
{
	uint32_t index;

	/* Starts empty. */
	records_clear(pic, NVRTX_NVDEC_HEVC_PIC_BYTES);

	/* The bitstream's length and the decoder's own timeout. */
	nvrtx_put32(pic, HEVC_STREAM_BYTES_AT, picture->stream_bytes);
	nvrtx_put32(pic, HEVC_TIMEOUT_AT, HEVC_TIMEOUT_DEFAULT);

	/* The surfaces: block linear in blocks of two GOBs, an 8-bit output, a stream with start codes. */
	records_bits(pic, HEVC_SURFACE_AT, PIC_TILE_FORMAT_LOW, 2U, PIC_TILE_FORMAT_KBL);
	records_bits(pic, HEVC_SURFACE_AT, PIC_GOB_HEIGHT_LOW, 3U, PIC_GOB_HEIGHT_TWO);
	pic[HEVC_START_CODES_AT] = HEVC_STREAM_START_CODES;
	pic[HEVC_OUTPUT_MODE_AT] = HEVC_OUTPUT_8BIT_709;

	/* The planes' rows, the co-located data of one picture and where the filter buffer's parts are. */
	nvrtx_put32(pic, HEVC_PITCH_LUMA_AT, picture->pitch);
	nvrtx_put32(pic, HEVC_PITCH_CHROMA_AT, picture->pitch);
	nvrtx_put32(pic, HEVC_COLOC_UNITS_AT, picture->coloc_units);
	nvrtx_put32(pic, HEVC_SAO_UNITS_AT, picture->sao_units);
	nvrtx_put32(pic, HEVC_BSD_UNITS_AT, picture->bsd_units);
	nvrtx_put32(pic, HEVC_FILTER_ABOVE_AT, picture->filter_above_units);
	nvrtx_put32(pic, HEVC_SAO_ABOVE_AT, picture->sao_above_units);

	/* The largest transform skip block of a Main picture. */
	records_bits(pic, HEVC_RANGE_EXTENSION_AT, HEVC_SKIP_SIZE_LOW, 4U, HEVC_SKIP_SIZE_MAIN);

	/* The picture's size in luma samples, 16 bits each. */
	records_put16(pic, HEVC_WIDTH_AT, (uint16_t)picture->width);
	records_put16(pic, HEVC_HEIGHT_AT, (uint16_t)picture->height);

	/* The size word: the sampling, the depths, the coding and transform block sizes. */
	records_bits(pic, HEVC_SIZES_AT, HEVC_CHROMA_FORMAT_LOW, 4U, picture->chroma_format_idc);
	records_bits(pic, HEVC_SIZES_AT, HEVC_DEPTH_LUMA_LOW, 4U, picture->bit_depth_luma);
	records_bits(pic, HEVC_SIZES_AT, HEVC_DEPTH_CHROMA_LOW, 4U, picture->bit_depth_chroma);
	records_bits(pic, HEVC_SIZES_AT, HEVC_MIN_CB_LOW, 4U, picture->log2_min_cb);
	records_bits(pic, HEVC_SIZES_AT, HEVC_MAX_CB_LOW, 4U, picture->log2_max_cb);
	records_bits(pic, HEVC_SIZES_AT, HEVC_MIN_TB_LOW, 4U, picture->log2_min_tb);
	records_bits(pic, HEVC_SIZES_AT, HEVC_MAX_TB_LOW, 4U, picture->log2_max_tb);

	/* The coding tool word: the transform depths, the tools and the PCM sizes. */
	records_bits(pic, HEVC_TOOLS_AT, HEVC_DEPTH_INTER_LOW, 3U, picture->depth_inter);
	records_bits(pic, HEVC_TOOLS_AT, HEVC_DEPTH_INTRA_LOW, 3U, picture->depth_intra);
	records_bits(pic, HEVC_TOOLS_AT, HEVC_SCALING_BIT, 1U, picture->scaling_enabled);
	records_bits(pic, HEVC_TOOLS_AT, HEVC_AMP_BIT, 1U, picture->amp);
	records_bits(pic, HEVC_TOOLS_AT, HEVC_SAO_BIT, 1U, picture->sao);
	records_bits(pic, HEVC_TOOLS_AT, HEVC_PCM_BIT, 1U, picture->pcm);
	records_bits(pic, HEVC_TOOLS_AT, HEVC_PCM_LUMA_LOW, 4U, picture->pcm_depth_luma);
	records_bits(pic, HEVC_TOOLS_AT, HEVC_PCM_CHROMA_LOW, 4U, picture->pcm_depth_chroma);
	records_bits(pic, HEVC_TOOLS_AT, HEVC_MIN_PCM_LOW, 4U, picture->log2_min_pcm);
	records_bits(pic, HEVC_TOOLS_AT, HEVC_MAX_PCM_LOW, 4U, picture->log2_max_pcm);
	records_bits(pic, HEVC_TOOLS_AT, HEVC_PCM_FILTER_OFF_BIT, 1U, picture->pcm_loop_filter_disabled);
	records_bits(pic, HEVC_TOOLS_AT, HEVC_TEMPORAL_MVP_BIT, 1U, picture->temporal_mvp);
	records_bits(pic, HEVC_TOOLS_AT, HEVC_STRONG_INTRA_BIT, 1U, picture->strong_intra_smoothing);

	/* The first picture parameter set word: the slice header's shape, the reference counts and the quantizer. */
	records_bits(pic, HEVC_PPS_AT, HEVC_DEPENDENT_BIT, 1U, picture->dependent_slices);
	records_bits(pic, HEVC_PPS_AT, HEVC_OUTPUT_FLAG_BIT, 1U, picture->output_flag_present);
	records_bits(pic, HEVC_PPS_AT, HEVC_EXTRA_BITS_LOW, 3U, picture->extra_header_bits);
	records_bits(pic, HEVC_PPS_AT, HEVC_SIGN_HIDING_BIT, 1U, picture->sign_data_hiding);
	records_bits(pic, HEVC_PPS_AT, HEVC_CABAC_INIT_BIT, 1U, picture->cabac_init_present);
	records_bits(pic, HEVC_PPS_AT, HEVC_REF_L0_LOW, 4U, picture->ref_l0_default);
	records_bits(pic, HEVC_PPS_AT, HEVC_REF_L1_LOW, 4U, picture->ref_l1_default);
	records_bits(pic, HEVC_PPS_AT, HEVC_INIT_QP_LOW, 7U, picture->init_qp);
	records_bits(pic, HEVC_PPS_AT, HEVC_CONSTRAINED_BIT, 1U, picture->constrained_intra);
	records_bits(pic, HEVC_PPS_AT, HEVC_TRANSFORM_SKIP_BIT, 1U, picture->transform_skip);
	records_bits(pic, HEVC_PPS_AT, HEVC_CU_QP_DELTA_BIT, 1U, picture->cu_qp_delta);
	records_bits(pic, HEVC_PPS_AT, HEVC_CU_QP_DEPTH_LOW, 2U, picture->cu_qp_delta_depth);

	/* The signed offsets, one byte each. */
	pic[HEVC_CB_QP_AT] = (uint8_t)picture->cb_qp_offset;
	pic[HEVC_CR_QP_AT] = (uint8_t)picture->cr_qp_offset;
	pic[HEVC_BETA_AT] = (uint8_t)picture->beta_offset;
	pic[HEVC_TC_AT] = (uint8_t)picture->tc_offset;

	/* The second picture parameter set word: the predictions, the tiles and the filters. */
	records_bits(pic, HEVC_PPS_FLAGS_AT, HEVC_SLICE_QP_BIT, 1U, picture->slice_chroma_qp_offsets);
	records_bits(pic, HEVC_PPS_FLAGS_AT, HEVC_WEIGHTED_BIT, 1U, picture->weighted_pred);
	records_bits(pic, HEVC_PPS_FLAGS_AT, HEVC_BIPRED_BIT, 1U, picture->weighted_bipred);
	records_bits(pic, HEVC_PPS_FLAGS_AT, HEVC_BYPASS_BIT, 1U, picture->transquant_bypass);
	records_bits(pic, HEVC_PPS_FLAGS_AT, HEVC_TILES_BIT, 1U, picture->tiles);
	records_bits(pic, HEVC_PPS_FLAGS_AT, HEVC_ENTROPY_SYNC_BIT, 1U, picture->entropy_sync);
	records_bits(pic, HEVC_PPS_FLAGS_AT, HEVC_TILE_COLUMNS_LOW, 5U, picture->tile_columns);
	records_bits(pic, HEVC_PPS_FLAGS_AT, HEVC_TILE_ROWS_LOW, 5U, picture->tile_rows);
	records_bits(pic, HEVC_PPS_FLAGS_AT, HEVC_ACROSS_TILES_BIT, 1U, picture->filter_across_tiles);
	records_bits(pic, HEVC_PPS_FLAGS_AT, HEVC_ACROSS_SLICES_BIT, 1U, picture->filter_across_slices);
	records_bits(pic, HEVC_PPS_FLAGS_AT, HEVC_DEBLOCKING_CONTROL_BIT, 1U, picture->deblocking_control);
	records_bits(pic, HEVC_PPS_FLAGS_AT, HEVC_DEBLOCKING_OVERRIDE_BIT, 1U, picture->deblocking_override);
	records_bits(pic, HEVC_PPS_FLAGS_AT, HEVC_DEBLOCKING_OFF_BIT, 1U, picture->deblocking_disabled);
	records_bits(pic, HEVC_PPS_FLAGS_AT, HEVC_LISTS_MODIFICATION_BIT, 1U, picture->lists_modification);
	records_bits(pic, HEVC_PPS_FLAGS_AT, HEVC_MERGE_LEVEL_LOW, 3U, picture->log2_parallel_merge);
	records_bits(pic, HEVC_PPS_FLAGS_AT, HEVC_HEADER_EXTENSION_BIT, 1U, picture->header_extension);

	/* The references: how many the picture uses, which are long term (bit 15 for surface 0). */
	pic[HEVC_REFERENCES_AT] = (uint8_t)picture->reference_count;
	records_put16(pic, HEVC_LONG_TERM_AT, (uint16_t)picture->long_term_mask);

	/* The initial reference lists, by surface. */
	for (index = 0U; index < 16U; index++) {
		pic[HEVC_LIST0_AT + index] = picture->list0[index];
		pic[HEVC_LIST1_AT + index] = picture->list1[index];
	}

	/* Each surface's order count difference to the picture, 16 bits each. */
	for (index = 0U; index < NVRTX_NVDEC_HEVC_SURFACES; index++)
		records_put16(pic, HEVC_ORDER_AT + index * 2U, (uint16_t)(int16_t)picture->order_difference[index]);

	/* The picture's kind, its surface, no dithering. */
	pic[HEVC_IDR_AT] = (uint8_t)picture->idr;
	pic[HEVC_RAP_AT] = (uint8_t)picture->irap;
	pic[HEVC_CURRENT_AT] = (uint8_t)picture->surface;
	pic[HEVC_PATTERN_AT] = HEVC_PATTERN_NONE;

	/* The slice header's bits the decoder skips, or with dxva the counts it parses them with. */
	records_put16(pic, HEVC_SKIP_AT, (uint16_t)picture->skip_bits);
	if (picture->dxva == 0U)
		return;
	records_bits(pic, HEVC_DXVA_AT, HEVC_SEPARATE_PLANES_BIT, 1U, picture->separate_planes);
	records_bits(pic, HEVC_DXVA_AT, HEVC_POC_LSB_LOW, 4U, picture->log2_max_poc_lsb_minus4);
	records_bits(pic, HEVC_DXVA_AT, HEVC_SHORT_TERM_SETS_LOW, 7U, picture->short_term_sets);
	records_bits(pic, HEVC_DXVA_AT, HEVC_LONG_TERM_SPS_LOW, 6U, picture->long_term_sps);
	records_bits(pic, HEVC_DXVA_AT, HEVC_DELTA_POCS_LOW, 8U, picture->delta_pocs);
	records_bits(pic, HEVC_DXVA_AT, HEVC_LONG_TERM_PRESENT_BIT, 1U, picture->long_term_present);
	nvrtx_put32(pic, HEVC_SHORT_TERM_BITS_AT, picture->short_term_bits);
}

/*
 * Fills the H.265 scaling list buffer (NVRTX_NVDEC_HEVC_SCALING_BYTES)
 * from lists in raster order: the decoder reads each list column by
 * column (the first column's coefficients from the top, then the next
 * column's), so every list is written transposed.
 */
void
nvrtx_nvdec_hevc_scaling(
	uint8_t *buffer,
	const struct nvrtx_nvdec_hevc_scaling *scaling)
{
	uint32_t list;
	uint32_t index;

	/* Starts empty. */
	records_clear(buffer, NVRTX_NVDEC_HEVC_SCALING_BYTES);

	/* The DC values of the six 16x16 lists and the two 32x32 lists. */
	for (index = 0U; index < 6U; index++)
		buffer[SCALING_DC16_AT + index] = scaling->dc16[index];
	for (index = 0U; index < 2U; index++)
		buffer[SCALING_DC32_AT + index] = scaling->dc32[index];

	/* The six 4x4 lists. */
	for (list = 0U; list < 6U; list++) {
		for (index = 0U; index < 16U; index++)
			buffer[SCALING_LIST4_AT + list * 16U + index] = scaling->list4[list][records_column(index, 4U)];
	}

	/* The six 8x8 lists. */
	for (list = 0U; list < 6U; list++) {
		for (index = 0U; index < 64U; index++)
			buffer[SCALING_LIST8_AT + list * 64U + index] = scaling->list8[list][records_column(index, 8U)];
	}

	/* The six 16x16 lists, as their coded 8x8 matrices. */
	for (list = 0U; list < 6U; list++) {
		for (index = 0U; index < 64U; index++)
			buffer[SCALING_LIST16_AT + list * 64U + index] = scaling->list16[list][records_column(index, 8U)];
	}

	/* The two 32x32 lists, as their coded 8x8 matrices. */
	for (list = 0U; list < 2U; list++) {
		for (index = 0U; index < 64U; index++)
			buffer[SCALING_LIST32_AT + list * 64U + index] = scaling->list32[list][records_column(index, 8U)];
	}
}

/*
 * Fills the H.265 tile buffer (NVRTX_NVDEC_HEVC_TILE_BYTES).  Without
 * tiles (enabled false) the picture is one tile: its width and height in
 * CTBs only.
 *
 * Procedure referenced: averne's FFmpeg nvtegra libavcodec/nvtegra_hevc.c
 * (nvtegra_hevc_set_tile_sizes, nvtegra_hevc_prepare_frame_setup).
 */
void
nvrtx_nvdec_hevc_tiles(
	uint8_t *buffer,
	const struct nvrtx_nvdec_hevc_tiles *tiles,
	bool enabled)
{
	uint32_t column;
	uint32_t row;
	uint32_t edge;
	uint32_t shift;
	size_t at;

	/* Starts empty. */
	records_clear(buffer, NVRTX_NVDEC_HEVC_TILE_BYTES);

	/* Without tiles: the one tile's width and height. */
	if (!enabled) {
		records_put16(buffer, 0U, (uint16_t)tiles->width[0]);
		records_put16(buffer, 2U, (uint16_t)tiles->height[0]);
		return;
	}

	/* Each tile's width and height in CTBs, the tiles row after row. */
	at = 0U;
	for (row = 0U; row < tiles->rows && row < NVRTX_NVDEC_HEVC_TILE_ROWS; row++) {
		for (column = 0U; column < tiles->columns && column < NVRTX_NVDEC_HEVC_TILE_COLUMNS; column++) {
			records_put16(buffer, at, (uint16_t)tiles->width[column]);
			records_put16(buffer, at + 2U, (uint16_t)tiles->height[row]);
			at += 4U;
		}
	}

	/* The CTB edges in 16-pixel units: the CTB's size over 16. */
	shift = 0U;
	if (tiles->log2_ctb > TILES_EDGE_LOG2)
		shift = tiles->log2_ctb - TILES_EDGE_LOG2;

	/* Each column's right edge, from the picture's left. */
	at = (size_t)TILES_EDGES_WORD * 2U;
	edge = 0U;
	for (column = 0U; column < tiles->columns && column < NVRTX_NVDEC_HEVC_TILE_COLUMNS; column++) {
		edge += tiles->width[column];
		records_put16(buffer, at, (uint16_t)(edge << shift));
		at += 2U;
	}

	/* Then each row's bottom edge, from the picture's top. */
	edge = 0U;
	for (row = 0U; row < tiles->rows && row < NVRTX_NVDEC_HEVC_TILE_ROWS; row++) {
		edge += tiles->height[row];
		records_put16(buffer, at, (uint16_t)(edge << shift));
		at += 2U;
	}
}

/*
 * Reports the sizes of an H.265 session's work buffers for pictures up to
 * width by height luma samples decoded into surfaces surfaces: the
 * co-located data of one picture and of every surface, and the filter
 * buffer with its parts' offsets (all on 256 bytes).
 */
void
nvrtx_nvdec_hevc_buffer_sizes(
	uint32_t width,
	uint32_t height,
	uint32_t surfaces,
	struct nvrtx_nvdec_hevc_buffers *buffers)
{
	uint32_t aligned_width;
	uint32_t aligned_height;
	uint32_t columns;
	uint32_t blocks;
	uint32_t part;
	uint32_t at;

	/* The picture on whole CTBs of the largest size. */
	aligned_width = records_align(width, FILTER_CTB);
	aligned_height = records_align(height, FILTER_CTB);

	/* The co-located motion data: a sixteenth of the area for each picture, a picture for each surface. */
	buffers->coloc_picture = records_align(aligned_width * aligned_height / COLOC_AREA_DIVISOR, RECORDS_BUFFER_ALIGN);
	buffers->coloc = buffers->coloc_picture * surfaces;

	/* The rows' parts of the filter buffer: the vertical filter, then the SAO, then the BSD control. */
	buffers->sao_at = records_align(FILTER_ROW_BYTES * aligned_height, RECORDS_BUFFER_ALIGN);
	buffers->bsd_at = records_align(buffers->sao_at + FILTER_SAO_ROW_BYTES * aligned_height, RECORDS_BUFFER_ALIGN);
	at = records_align(buffers->bsd_at + FILTER_BSD_ROW_BYTES * aligned_height, RECORDS_BUFFER_ALIGN);

	/* The columns' parts after them: the deblocking above, then the SAO above. */
	columns = records_align(width, FILTER_COLUMN) / FILTER_COLUMN + 2U;
	buffers->filter_above_at = at;
	blocks = (aligned_width / FILTER_COLUMN) * (aligned_height / FILTER_COLUMN);
	part = columns * FILTER_ABOVE_COLUMN_BYTES;
	if (part < blocks * FILTER_ABOVE_BLOCK_BYTES)
		part = blocks * FILTER_ABOVE_BLOCK_BYTES;
	at = records_align(at + part, RECORDS_BUFFER_ALIGN);
	buffers->sao_above_at = at;
	part = columns * FILTER_SAO_ABOVE_COLUMN_BYTES;
	if (part < blocks * FILTER_ABOVE_BLOCK_BYTES)
		part = blocks * FILTER_ABOVE_BLOCK_BYTES;
	at = records_align(at + part, RECORDS_BUFFER_ALIGN);

	/* Succeeded: the whole filter buffer. */
	buffers->filter = at;
}

/*
 * Writes the decoder object's parameters: the first decoder, which other
 * channels may share.
 */
void
nvrtx_nvdec_alloc_params(
	uint8_t *params)
{
	/* The size, the sharing and the instance. */
	records_clear(params, NVRTX_NVDEC_ALLOC_PARAMS_BYTES);
	nvrtx_put32(params, ALLOC_SIZE_AT, NVRTX_NVDEC_ALLOC_PARAMS_BYTES);
	nvrtx_put32(params, ALLOC_PROHIBIT_AT, 0U);
	nvrtx_put32(params, ALLOC_INSTANCE_AT, 0U);
}

/*
 * Writes the promote that hands the decoder's channel its engine context:
 * the decoder's engine, the driver's client and channel, and the
 * context's address in the channel's space and its size.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/fifo.c (the promote of a
 * falcon engine's context by its address, no entries).
 */
void
nvrtx_nvdec_promote_params(
	uint8_t *params,
	uint32_t client,
	uint32_t chid,
	uint32_t channel,
	uint64_t va,
	uint64_t size)
{
	/* The engine, the client, the channel by number and by handle. */
	records_clear(params, NVRTX_NVDEC_PROMOTE_BYTES);
	nvrtx_put32(params, PROMOTE_ENGINE_AT, NVRTX_ENGINE_NVDEC0);
	nvrtx_put32(params, PROMOTE_CLIENT_AT, client);
	nvrtx_put32(params, PROMOTE_CHID_AT, chid);
	nvrtx_put32(params, PROMOTE_CHANNEL_CLIENT_AT, client);
	nvrtx_put32(params, PROMOTE_OBJECT_AT, channel);

	/* The context by its address and size, no entries. */
	nvrtx_put64(params, PROMOTE_ADDRESS_AT, va);
	nvrtx_put64(params, PROMOTE_SIZE_AT, size);
	nvrtx_put32(params, PROMOTE_COUNT_AT, 0U);
}

/*
 * Finds the context size of the falcon whose engine descriptor is given
 * in the resource manager's list of the falcons it made.
 *
 * Returns EINVAL for a list cut short, ENOENT when no falcon matches.
 */
int
nvrtx_nvdec_context_bytes(
	const uint8_t *reply,
	size_t length,
	uint32_t engine_descriptor,
	uint32_t *bytes)
{
	uint32_t count;
	uint32_t index;
	uint32_t descriptor;
	size_t at;

	/* The list must hold its count and every entry it counts. */
	if (length < FALCON_ENTRIES_AT)
		return EINVAL;

	/* Reads how many entries the list counts and refuses a count the list cannot hold. */
	count = nvrtx_get32(reply, FALCON_COUNT_AT);
	if (count > FALCON_MAX)
		return EINVAL;
	if (FALCON_ENTRIES_AT + (size_t)count * FALCON_ENTRY_BYTES > length)
		return EINVAL;

	/* Looks for the falcon by its engine descriptor. */
	for (index = 0U; index < count; index++) {
		at = FALCON_ENTRIES_AT + (size_t)index * FALCON_ENTRY_BYTES;
		descriptor = nvrtx_get32(reply, at + FALCON_DESCRIPTOR_AT);
		if (descriptor != engine_descriptor)
			continue;

		/* Succeeded: the falcon's context size. */
		*bytes = nvrtx_get32(reply, at + FALCON_CONTEXT_BYTES_AT);
		return 0;
	}

	/* No falcon of that engine. */
	return ENOENT;
}

/* Writes an incrementing method header on the decoder's subchannel and returns where its data goes. */
static size_t
records_method(
	uint8_t *push,
	size_t at,
	uint32_t method,
	uint32_t count)
{
	uint32_t header;

	/* The header's fields. */
	header = RECORDS_METHOD_INCREMENT |
	    (count << RECORDS_METHOD_COUNT_SHIFT) |
	    (NVRTX_NVDEC_SUBCHANNEL << RECORDS_METHOD_SUBCHANNEL_SHIFT) |
	    (method >> 2);
	nvrtx_put32(push, at, header);

	/* Succeeded: the data follows the header. */
	return at + 4U;
}

/* Writes one method that names a buffer by its GPU address shifted right by 8, and returns where the push buffer goes on. */
static size_t
records_address(
	uint8_t *push,
	size_t at,
	uint32_t method,
	uint64_t address)
{
	/* The method and the address in 256-byte units. */
	at = records_method(push, at, method, 1U);
	nvrtx_put32(push, at, (uint32_t)(address >> 8));

	/* Succeeded: the push buffer goes on after the word. */
	return at + 4U;
}

/* Puts a value into a bit field of a little-endian word, keeping its other bits. */
static void
records_bits(
	uint8_t *bytes,
	size_t at,
	uint32_t low,
	uint32_t bits,
	uint32_t value)
{
	uint32_t word;
	uint32_t mask;

	/* The field's mask, from its lowest bit. */
	mask = 0xffffffffU;
	if (bits < 32U)
		mask = (1U << bits) - 1U;

	/* The word with the field replaced. */
	word = nvrtx_get32(bytes, at);
	word &= ~(mask << low);
	word |= (value & mask) << low;
	nvrtx_put32(bytes, at, word);
}

/* Stores a 16-bit value little-endian. */
static void
records_put16(
	uint8_t *bytes,
	size_t at,
	uint16_t value)
{
	/* The low byte first. */
	bytes[at] = (uint8_t)(value & 0xffU);
	bytes[at + 1U] = (uint8_t)(value >> 8);
}

/*
 * Fills one reference entry: the reference's surface (its co-located data
 * too), a frame's state and markings, whether it is long term or
 * non-existing, its field order counts and its frame number.
 */
static void
records_reference(
	uint8_t *pic,
	uint32_t entry,
	const struct nvrtx_nvdec_h264_reference *reference)
{
	uint32_t marking;
	size_t at;

	/* The entry must be one of the structure's. */
	if (entry >= NVRTX_NVDEC_REFERENCES)
		return;

	/* Finds where the entry lies in the picture structure. */
	at = PIC_DPB_AT + (size_t)entry * PIC_DPB_ENTRY_BYTES;

	/* A long-term reference is marked so in both fields, a short-term one so. */
	marking = DPB_MARKING_SHORT;
	if (reference->long_term != 0U)
		marking = DPB_MARKING_LONG;

	/* The first word: the surface, the frame state, the term, the existence and the markings. */
	records_bits(pic, at, DPB_INDEX_LOW, 7U, reference->surface);
	records_bits(pic, at, DPB_COLOC_LOW, 5U, reference->surface);
	records_bits(pic, at, DPB_STATE_LOW, 2U, DPB_STATE_FRAME);
	records_bits(pic, at, DPB_LONG_TERM, 1U, reference->long_term);
	records_bits(pic, at, DPB_NON_EXISTING, 1U, reference->non_existing);
	records_bits(pic, at, DPB_TOP_MARKING_LOW, 4U, marking);
	records_bits(pic, at, DPB_BOTTOM_MARKING_LOW, 4U, marking);

	/* The field order counts and the frame number. */
	nvrtx_put32(pic, at + DPB_FIELD_ORDER_AT, (uint32_t)reference->field_order[0]);
	nvrtx_put32(pic, at + DPB_FIELD_ORDER_AT + 4U, (uint32_t)reference->field_order[1]);
	nvrtx_put32(pic, at + DPB_FRAME_INDEX_AT, reference->frame_index);
}

/* Rounds a value up to a boundary that is a power of two. */
static uint32_t
records_align(
	uint32_t value,
	uint32_t boundary)
{
	/* Succeeded: the rounded value. */
	return (value + boundary - 1U) & ~(boundary - 1U);
}

/* Clears a run of bytes. */
static void
records_clear(
	uint8_t *bytes,
	size_t length)
{
	size_t index;

	/* Every byte. */
	for (index = 0U; index < length; index++)
		bytes[index] = 0U;
}

/*
 * Reports the raster place (row times side plus column) of the index-th
 * coefficient of a side by side list read column by column.
 */
static uint32_t
records_column(
	uint32_t index,
	uint32_t side)
{
	/* The column is the index over the side, the row what is left. */
	return (index % side) * side + index / side;
}
