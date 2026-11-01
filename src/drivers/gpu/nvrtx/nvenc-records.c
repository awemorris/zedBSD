/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The video encoder's push buffers and resource manager parameters, which
 * touch no hardware (a host test runs them).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "drivers/gpu/nvrtx/nvrtx-gsp.h"
#include "drivers/gpu/nvrtx/nvrtx-nvenc.h"

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
 * The encoder's methods the channel's bring-up uses: the no-operation,
 * and the release of a semaphore by the encoder itself (the upper eight
 * bits of its address, the lower thirty-two, the payload, then the
 * operation word: a release of one word, no awakening, flushed first,
 * which is the word 0).
 *
 * Values referenced: NVIDIA open-gpu-doc classes/video/clc5b7.h
 * (NVC5B7_NOP, NVC5B7_SEMAPHORE_A, NVC5B7_SEMAPHORE_A_UPPER,
 * NVC5B7_SEMAPHORE_B, NVC5B7_SEMAPHORE_C, NVC5B7_SEMAPHORE_D and its
 * STRUCTURE_SIZE_ONE, AWAKEN_ENABLE_FALSE, OPERATION_RELEASE and
 * FLUSH_DISABLE_FALSE values).
 */
#define RECORDS_NOP			0x0100U
#define RECORDS_SEMAPHORE_A		0x0240U
#define RECORDS_SEMAPHORE_UPPER_MASK	0xffU
#define RECORDS_SEMAPHORE_D		0x0304U
#define RECORDS_SEMAPHORE_D_RELEASE	0U

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
 * The encoder object's parameters: their size, single instance not
 * required, the first encoder.
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/nvos.h (NV_MSENC_ALLOCATION_PARAMETERS: size
 * 0, prohibitMultipleInstances 4, engineInstance 8).
 */
#define ALLOC_SIZE_AT			0U
#define ALLOC_PROHIBIT_AT		4U
#define ALLOC_INSTANCE_AT		8U

/*
 * The encoder's methods of one encode: the codec, the control parameters,
 * the picture's number, the buffers' addresses (each the upper 32 bits of
 * a 40-bit address, so the address shifted right by 8), the reference
 * pictures' planes, and the start.
 *
 * Values referenced: NVIDIA open-gpu-doc classes/video/clc5b7.h
 * (NVC5B7_SET_APPLICATION_ID and its _ID_NVENC_H264, NVC5B7_EXECUTE,
 * NVC5B7_SET_IN_REF_PIC0_LUMA, NVC5B7_SET_IN_REF_PIC0_CHROMA,
 * NVC5B7_SET_CONTROL_PARAMS, NVC5B7_SET_PICTURE_INDEX,
 * NVC5B7_SET_IN_DRV_PIC_SETUP, NVC5B7_SET_OUT_ENC_STATUS,
 * NVC5B7_SET_OUT_BITSTREAM, NVC5B7_SET_IOHISTORY,
 * NVC5B7_SET_IO_RC_PROCESS, NVC5B7_SET_OUT_COLOC_DATA,
 * NVC5B7_SET_OUT_REF_PIC_LUMA, NVC5B7_SET_IN_CUR_PIC,
 * NVC5B7_SET_IN_CUR_PIC_CHROMA_U, NVC5B7_SET_OUT_REF_PIC_CHROMA) and
 * classes/video/nvenc_drv.h (section 3.1: each offset method takes the
 * upper 32 bits of the 40-bit offset).
 */
#define RECORDS_APPLICATION_ID		0x0200U
#define RECORDS_APPLICATION_H264	1U
#define RECORDS_EXECUTE			0x0300U
#define RECORDS_IN_REF_LUMA		0x0400U
#define RECORDS_IN_REF_CHROMA		0x0440U
#define RECORDS_CONTROL			0x0700U
#define RECORDS_PICTURE_INDEX		0x0704U
#define RECORDS_PIC_SETUP		0x0710U
#define RECORDS_OUT_STATUS		0x0718U
#define RECORDS_OUT_BITSTREAM		0x071cU
#define RECORDS_HISTORY			0x0720U
#define RECORDS_RC_PROCESS		0x0724U
#define RECORDS_OUT_COLOC		0x072cU
#define RECORDS_OUT_REF_LUMA		0x0730U
#define RECORDS_IN_CUR_LUMA		0x0734U
#define RECORDS_IN_CUR_CHROMA		0x0740U
#define RECORDS_OUT_REF_CHROMA		0x074cU
#define RECORDS_ADDRESS_SHIFT		8U

/*
 * The magic word's low half: version 0, revision 6 of the class's driver
 * interface (its high half is the class).
 *
 * Value referenced: NVIDIA open-gpu-doc classes/video/nvenc_drv.h
 * (NV_NVENC_DRV_MAGIC_VALUE: 0xC4B70006 for NV_NVENC_7_2, 0xC7B70006 and
 * 0xC9B70006 for the later classes).
 */
#define RECORDS_MAGIC_REVISION		0x0006U

/*
 * The H.264 picture setup structure: where its parts start (the magic
 * word, the reference, input and reconstructed pictures' surfaces, the
 * sequence's and the picture's parameter words, the rate control and the
 * picture control).  Both published layouts of the structure agree on
 * every byte before 472; the driver writes none at or after it, nor the
 * four rate-control bytes the layouts order differently (73 to 76 of the
 * rate control), so the zeroes there mean the same in either.
 *
 * Values referenced: NVIDIA open-gpu-doc classes/video/nvenc_drv.h, in
 * commits 97c510e and 9fdf5c4 (nvenc_h264_drv_pic_setup_s: magic 0,
 * refpic_cfg 4, input_cfg 36, outputpic_cfg 68, sps_data 100, pps_data
 * 104, rate_control 112, pic_control 200).
 */
#define SETUP_MAGIC_AT			0U
#define SETUP_REFERENCE_AT		4U
#define SETUP_INPUT_AT			36U
#define SETUP_OUTPUT_AT			68U
#define SETUP_SPS_AT			100U
#define SETUP_PPS_AT			104U
#define SETUP_RC_AT			112U
#define SETUP_PIC_AT			200U

/*
 * A surface of the setup (32 bytes): the width and the height less one,
 * the planes' pitches, the planes' offsets in 256-byte units, and the
 * layout word (bit 7: 16x16 tiles instead of block linear; bits 9:8 the
 * plane mode, 0 for semi-planar).
 *
 * Values referenced: NVIDIA open-gpu-doc classes/video/nvenc_drv.h
 * (nvenc_h264_surface_cfg_s: frame_width_minus1 0, frame_height_minus1 2,
 * sfc_pitch 4, sfc_pitch_chroma 6, luma_top_frm_offset 12,
 * chroma_top_frm_offset 20, then the bit fields at 28 with tiled_16x16
 * at bit 7).
 */
#define SURFACE_WIDTH_AT		0U
#define SURFACE_HEIGHT_AT		2U
#define SURFACE_PITCH_AT		4U
#define SURFACE_CHROMA_PITCH_AT		6U
#define SURFACE_LUMA_AT			12U
#define SURFACE_CHROMA_AT		20U
#define SURFACE_LAYOUT_AT		28U
#define SURFACE_TILED			(1U << 7)

/*
 * The sequence parameters word: the profile, the level, the chroma format
 * (1: 4:2:0), the picture order count type (0), the two log2 sizes less 4,
 * and frames only.
 *
 * Values referenced: NVIDIA open-gpu-doc classes/video/nvenc_drv.h
 * (nvenc_h264_sps_data_s: profile_idc 7:0, level_idc 15:8,
 * chroma_format_idc 17:16, pic_order_cnt_type 19:18,
 * log2_max_frame_num_minus4 23:20, log2_max_pic_order_cnt_lsb_minus4
 * 27:24, frame_mbs_only 28).
 */
#define SPS_LEVEL_SHIFT			8U
#define SPS_CHROMA_420			(1U << 16)
#define SPS_FRAME_NUM_SHIFT		20U
#define SPS_POC_LSB_SHIFT		24U
#define SPS_LOG2_MASK			0xfU
#define SPS_FRAMES_ONLY			(1U << 28)
#define SPS_BYTE_MASK			0xffU

/*
 * The picture parameters' first word: the set's number (0), CAVLC, one
 * active reference in each list (the counts less one, 0) and the initial
 * QP less 26 in six signed bits at 26:21; the second word is all zero (no
 * chroma QP offsets, no deblocking controls in the slice header, no 8x8
 * transform, no weighted prediction).
 *
 * Values referenced: NVIDIA open-gpu-doc classes/video/nvenc_drv.h
 * (nvenc_h264_pps_data_s: pic_init_qp_minus26 26:21 of its first word).
 */
#define PPS_INIT_QP_SHIFT		21U
#define PPS_INIT_QP_MASK		0x3fU
#define PPS_INIT_QP_BASE		26U

/*
 * The rate control's constant QPs (P, B, I, a byte each), their minimum
 * and maximum, and the period of intra pictures (all ones: never forced).
 *
 * Values referenced: NVIDIA open-gpu-doc classes/video/nvenc_drv.h
 * (nvenc_h264_rc_s: QP 1, minQP 4, maxQP 7, gop_length 48, and
 * gop_length 0xffffffff for an infinite period).
 */
#define RC_QP_AT			1U
#define RC_MIN_QP_AT			4U
#define RC_MAX_QP_AT			7U
#define RC_GOP_AT			48U
#define RC_QP_COUNT			3U
#define RC_QP_MAX			51U
#define RC_GOP_INFINITE			0xffffffffU

/*
 * The picture control (from its start in the setup): the reference lists
 * (eight signed bytes each, -1 for no entry), list 0's temporal distances,
 * the cap on the bytes before the residuals are forced to zero, frame_num,
 * the picture order count's low bits, idr_pic_id, the controls' offsets
 * from the setup's start (their counts less one stay 0: one of each), the
 * history's and the bitstream's sizes, where the slice data starts (0),
 * the picture word (pic_type 3:2, ref_pic_flag 4, slice_mode 5: static,
 * from the slice control) and the codec field (18:16 of the next word).
 *
 * Values referenced: NVIDIA open-gpu-doc classes/video/nvenc_drv.h
 * (nvenc_h264_pic_control_s: l0 0, l1 8, temp_dist_l0 16,
 * max_byte_count_before_resid_zero 172, frame_num 180, pic_order_cnt_lsb
 * 182, idr_pic_id 184, slice_control_offset 196, me_control_offset 200,
 * md_control_offset 204, q_control_offset 208, hist_buf_size 212,
 * bitstream_buf_size 216, the picture word at 224 with pic_type 0 = P and
 * 3 = IDR, codec 18:16 of the word at 228).
 */
#define PIC_L0_AT			0U
#define PIC_L1_AT			8U
#define PIC_DISTANCE_L0_AT		16U
#define PIC_RESIDUAL_CAP_AT		172U
#define PIC_FRAME_NUM_AT		180U
#define PIC_POC_LSB_AT			182U
#define PIC_IDR_ID_AT			184U
#define PIC_SLICE_OFFSET_AT		196U
#define PIC_ME_OFFSET_AT		200U
#define PIC_MD_OFFSET_AT		204U
#define PIC_QUANT_OFFSET_AT		208U
#define PIC_HISTORY_AT			212U
#define PIC_BITSTREAM_AT		216U
#define PIC_WORD_AT			224U
#define PIC_TYPE_SHIFT			2U
#define PIC_REFERENCE			(1U << 4)
#define PIC_STATIC_SLICES		(1U << 5)
#define PIC_CODEC_WORD_AT		228U
#define PIC_CODEC_SHIFT			16U
#define PIC_CODEC_MASK			0x7U
#define PIC_LIST_ENTRIES		8U
#define PIC_LIST_NONE			0xffU
#define PIC_TYPE_P			0U
#define PIC_TYPE_IDR			3U

/*
 * The slice control (one slice of the whole picture): the macroblocks and
 * the slice's QP (word 0), the slice QP's range and whether every
 * macroblock is intra (word 2); the controls' indices stay 0.
 *
 * Values referenced: NVIDIA open-gpu-doc classes/video/nvenc_drv.h
 * (nvenc_h264_slice_control_s: num_mb 18:0 and qp_avr 26:19 of word 0;
 * qp_slice_min 15:8, qp_slice_max 23:16, force_intra 24 of word 2).
 */
#define SLICE_MB_AT			0U
#define SLICE_QP_SHIFT			19U
#define SLICE_RANGE_AT			8U
#define SLICE_QP_MAX_SHIFT		16U
#define SLICE_INTRA			(1U << 24)
#define SLICE_QP_MASK			0xffU

/*
 * The motion search control: quarter-pel refinement (word 0 bit 1), the
 * predictor sources (the spatial neighbours' and the constant vector's,
 * each searched and refined, in the words at 12 and 20), the first
 * stamping pattern (two words of an 8x8 bitmask, at 44, all points) and
 * the order of the five hint types (a 3-bit type each at 30:16 of the
 * word at 152, together every type once: the constant vector, the
 * spatial, the temporal, the co-located and the external hint).
 *
 * Values referenced: NVIDIA open-gpu-doc classes/video/nvenc_drv.h
 * (nvenc_h264_me_control_s: refinement_mode 1 of word 0, predsrc 12,
 * shape0 44, hint_type0 18:16 to hint_type4 30:28 of the word at 152 and
 * the rule that the five types are each named once;
 * nvenc_h264_me_hint_cfg_s: self_spatial_search 27, _refine 28, _enable
 * 29 of its word 0; const_mv_search 29, _refine 30, _enable 31 of its
 * word 2).
 */
#define ME_WORD_AT			0U
#define ME_QUARTER_PEL			(1U << 1)
#define ME_SOURCES_AT			12U
#define ME_SPATIAL			((1U << 27) | (1U << 28) | (1U << 29))
#define ME_CONSTANT_AT			20U
#define ME_CONSTANT			((1U << 29) | (1U << 30) | (1U << 31))
#define ME_SHAPE_AT			44U
#define ME_SHAPE_ALL			0xffffffffU
#define ME_HINTS_AT			152U
#define ME_HINT_SHIFT			16U
#define ME_HINT_WIDTH			3U
#define ME_HINT_TYPES			5U

/*
 * The mode decision control: the intra modes searched (the nine 4x4 luma
 * modes, no 8x8 one in the Main profile, the four 16x16 luma modes, the
 * four chroma modes), the inter partitions (L0's 16x16, 16x8, 8x16 and
 * 8x8) and P_Skip, and the intra sizes searched before the intra or inter
 * decision (4x4 and 16x16) with no early intra decision (3).
 *
 * Values referenced: NVIDIA open-gpu-doc classes/video/nvenc_drv.h
 * (nvenc_h264_md_control_s: intra_luma4x4_mode_enable 8:0,
 * intra_luma8x8_mode_enable 17:9, intra_luma16x16_mode_enable 21:18,
 * intra_chroma_mode_enable 25:22 of the word at 4; l0_part_16x16_enable 0
 * to l0_part_8x8_enable 3 and pskip_enable 27 of the word at 8;
 * early_intra_mode_control 20:19 and ip_search_mode 28:26 (bit 0 4x4,
 * bit 2 16x16) of the word at 52).
 */
#define MD_INTRA_AT			4U
#define MD_INTRA			(0x1ffU | (0xfU << 18) | (0xfU << 22))
#define MD_INTER_AT			8U
#define MD_INTER			(0xfU | (1U << 27))
#define MD_DECISION_AT			52U
#define MD_DECISION			((3U << 19) | (5U << 26))

static size_t records_method(uint8_t *push, size_t at, uint32_t method, uint32_t count);
static size_t records_address(uint8_t *push, size_t at, uint32_t method, uint64_t address);
static void records_surface(uint8_t *surface, const struct nvrtx_nvenc_h264_picture *picture, uint32_t chroma_units);
static void records_h264_controls(uint8_t *setup, const struct nvrtx_nvenc_h264_picture *picture);
static void records_put16(uint8_t *bytes, size_t at, uint32_t value);
static void records_clear(uint8_t *bytes, size_t length);

/*
 * Reports the encoder's class of a GPU family, or 0 for a family without
 * one the driver drives.
 */
uint32_t
nvrtx_nvenc_class(
	enum nvrtx_family family)
{
	/* Each family's class. */
	switch (family) {
	case NVRTX_FAMILY_TURING:
		return NVRTX_NVENC_CLASS_TURING;
	case NVRTX_FAMILY_AMPERE:
		return NVRTX_NVENC_CLASS_AMPERE;
	case NVRTX_FAMILY_ADA:
		return NVRTX_NVENC_CLASS_ADA;
	default:
		break;
	}

	/* No encoder for the others. */
	return 0U;
}

/* Reports the magic word a picture setup starts with for the encoder's class. */
uint32_t
nvrtx_nvenc_magic(
	uint32_t nvenc_class)
{
	/* Succeeded: the class in the high half, the interface's revision in the low. */
	return (nvenc_class << 16) | RECORDS_MAGIC_REVISION;
}

/*
 * Writes the push buffer that binds the encoder's class to its subchannel
 * and gives the encoder one no-operation, so the first method the encoder
 * itself takes is one that does nothing.
 *
 * Returns the length in bytes (at most NVRTX_NVENC_OBJECT_PUSH_BYTES).
 */
size_t
nvrtx_nvenc_push_object(
	uint8_t *push,
	uint32_t nvenc_class)
{
	size_t at;

	/* The class on the encoder's subchannel. */
	at = records_method(push, 0U, RECORDS_SET_OBJECT, 1U);
	nvrtx_put32(push, at, nvenc_class);
	at += 4U;

	/* The no-operation. */
	at = records_method(push, at, RECORDS_NOP, 1U);
	nvrtx_put32(push, at, 0U);
	at += 4U;

	/* Succeeded: the push buffer's length. */
	return at;
}

/*
 * Writes the push buffer of a semaphore release the encoder performs
 * itself: value written at the semaphore's GPU address (on a 4-byte
 * boundary, below 1 TiB) once the encoder reaches it.
 *
 * Returns the length in bytes (at most NVRTX_NVENC_RELEASE_PUSH_BYTES).
 */
size_t
nvrtx_nvenc_push_release(
	uint8_t *push,
	uint64_t semaphore,
	uint32_t value)
{
	size_t at;

	/* The semaphore's address, its upper bits then its lower, and the payload. */
	at = records_method(push, 0U, RECORDS_SEMAPHORE_A, 3U);
	nvrtx_put32(push, at, (uint32_t)(semaphore >> 32) & RECORDS_SEMAPHORE_UPPER_MASK);
	nvrtx_put32(push, at + 4U, (uint32_t)semaphore);
	nvrtx_put32(push, at + 8U, value);
	at += 12U;

	/* The release of one word. */
	at = records_method(push, at, RECORDS_SEMAPHORE_D, 1U);
	nvrtx_put32(push, at, RECORDS_SEMAPHORE_D_RELEASE);
	at += 4U;

	/* Succeeded: the push buffer's length. */
	return at;
}

/*
 * Writes the push buffer of one H.264 picture's encode: the codec, the
 * control parameters, the picture's number, the setup, the status, the
 * bitstream, the history, the rate control's state, the input picture,
 * the reconstructed picture and its co-located data, the reference, then
 * the start.  Buffers the frame leaves at 0 get no method.
 *
 * Returns the length in bytes (at most NVRTX_NVENC_ENCODE_PUSH_BYTES).
 *
 * Procedure referenced: NVIDIA open-gpu-doc classes/video/nvenc_drv.h
 * (section 3.1: the methods an execute needs, the co-located output only
 * for a reference picture, the reference pictures' offsets only for those
 * the execute uses, the reconstructed picture for a reference picture or
 * FORCE_OUTPIC).
 */
size_t
nvrtx_nvenc_push_encode(
	uint8_t *push,
	const struct nvrtx_nvenc_frame *frame)
{
	size_t at;

	/* The codec. */
	at = records_method(push, 0U, RECORDS_APPLICATION_ID, 1U);
	nvrtx_put32(push, at, RECORDS_APPLICATION_H264);
	at += 4U;

	/* The control parameters. */
	at = records_method(push, at, RECORDS_CONTROL, 1U);
	nvrtx_put32(push, at, frame->control);
	at += 4U;

	/* The picture's number in the session. */
	at = records_method(push, at, RECORDS_PICTURE_INDEX, 1U);
	nvrtx_put32(push, at, frame->serial);
	at += 4U;

	/* The setup, the status, the bitstream and the history. */
	at = records_address(push, at, RECORDS_PIC_SETUP, frame->setup);
	at = records_address(push, at, RECORDS_OUT_STATUS, frame->status);
	at = records_address(push, at, RECORDS_OUT_BITSTREAM, frame->bitstream);
	at = records_address(push, at, RECORDS_HISTORY, frame->history);

	/* The rate control's persistent state, when the session keeps one. */
	if (frame->rc_process != 0U)
		at = records_address(push, at, RECORDS_RC_PROCESS, frame->rc_process);

	/* The input picture's planes. */
	at = records_address(push, at, RECORDS_IN_CUR_LUMA, frame->input_luma);
	at = records_address(push, at, RECORDS_IN_CUR_CHROMA, frame->input_chroma);

	/* The reconstructed picture's planes. */
	at = records_address(push, at, RECORDS_OUT_REF_LUMA, frame->output_luma);
	at = records_address(push, at, RECORDS_OUT_REF_CHROMA, frame->output_chroma);

	/* The co-located data a reference picture leaves for the pictures after it. */
	if (frame->coloc_out != 0U)
		at = records_address(push, at, RECORDS_OUT_COLOC, frame->coloc_out);

	/* The one reference of a P picture, in slot 0. */
	if (frame->reference_luma != 0U) {
		at = records_address(push, at, RECORDS_IN_REF_LUMA, frame->reference_luma);
		at = records_address(push, at, RECORDS_IN_REF_CHROMA, frame->reference_chroma);
	}

	/* Starts the encode, without a notification. */
	at = records_method(push, at, RECORDS_EXECUTE, 1U);
	nvrtx_put32(push, at, 0U);
	at += 4U;

	/* Succeeded: the push buffer's length. */
	return at;
}

/*
 * Writes the encoder object's parameters: the first encoder, which other
 * channels may share.
 */
void
nvrtx_nvenc_alloc_params(
	uint8_t *params)
{
	/* The size, the sharing and the instance. */
	records_clear(params, NVRTX_NVENC_ALLOC_PARAMS_BYTES);
	nvrtx_put32(params, ALLOC_SIZE_AT, NVRTX_NVENC_ALLOC_PARAMS_BYTES);
	nvrtx_put32(params, ALLOC_PROHIBIT_AT, 0U);
	nvrtx_put32(params, ALLOC_INSTANCE_AT, 0U);
}

/*
 * Writes the promote that hands the encoder's channel its engine context:
 * the encoder's engine, the driver's client and channel, and the
 * context's address in the channel's space and its size.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/fifo.c (the promote of a
 * falcon engine's context by its address, no entries).
 */
void
nvrtx_nvenc_promote_params(
	uint8_t *params,
	uint32_t client,
	uint32_t chid,
	uint32_t channel,
	uint64_t va,
	uint64_t size)
{
	/* The engine, the client, the channel by number and by handle. */
	records_clear(params, NVRTX_NVENC_PROMOTE_BYTES);
	nvrtx_put32(params, PROMOTE_ENGINE_AT, NVRTX_ENGINE_NVENC0);
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
 * Writes one H.264 picture's setup (NVRTX_NVENC_H264_SETUP_BYTES at a
 * 256-byte boundary): the picture setup structure that starts with magic,
 * and after it the one slice, motion search, mode decision and
 * quantization control the picture control points at.  Every byte the
 * picture does not decide is zero.
 */
void
nvrtx_nvenc_h264_setup(
	uint8_t *setup,
	uint32_t magic,
	const struct nvrtx_nvenc_h264_picture *picture)
{
	uint8_t *pic;
	uint8_t *rc;
	uint32_t word;
	uint32_t index;

	/* Starts from zeroes and the magic word the firmware checks first. */
	records_clear(setup, NVRTX_NVENC_H264_SETUP_BYTES);
	nvrtx_put32(setup, SETUP_MAGIC_AT, magic);

	/* The reference, input and reconstructed pictures share one layout: NV12 in 16x16 tiles. */
	records_surface(setup + SETUP_REFERENCE_AT, picture, picture->chroma_units);
	records_surface(setup + SETUP_INPUT_AT, picture, picture->chroma_units);
	records_surface(setup + SETUP_OUTPUT_AT, picture, picture->chroma_units);

	/* The sequence: the profile and level, 4:2:0, picture order count type 0, frames only. */
	word = picture->profile_idc & SPS_BYTE_MASK;
	word |= (picture->level_idc & SPS_BYTE_MASK) << SPS_LEVEL_SHIFT;
	word |= SPS_CHROMA_420;
	word |= (picture->log2_max_frame_num_minus4 & SPS_LOG2_MASK) << SPS_FRAME_NUM_SHIFT;
	word |= (picture->log2_max_poc_lsb_minus4 & SPS_LOG2_MASK) << SPS_POC_LSB_SHIFT;
	word |= SPS_FRAMES_ONLY;
	nvrtx_put32(setup, SETUP_SPS_AT, word);

	/* The picture parameters: the initial QP is the picture's, so the slice header's QP delta is 0. */
	word = ((picture->qp - PPS_INIT_QP_BASE) & PPS_INIT_QP_MASK) << PPS_INIT_QP_SHIFT;
	nvrtx_put32(setup, SETUP_PPS_AT, word);

	/* The rate control: one constant QP for every picture type, the full range, no forced intra period. */
	rc = setup + SETUP_RC_AT;

	/* Every picture type's QP, minimum and maximum. */
	for (index = 0U; index < RC_QP_COUNT; index++) {
		rc[RC_QP_AT + index] = (uint8_t)picture->qp;
		rc[RC_MIN_QP_AT + index] = 0U;
		rc[RC_MAX_QP_AT + index] = RC_QP_MAX;
	}

	nvrtx_put32(rc, RC_GOP_AT, RC_GOP_INFINITE);

	/* The picture control's reference lists: none, or slot 0 in list 0 for a P picture. */
	pic = setup + SETUP_PIC_AT;

	/* No entry in either list. */
	for (index = 0U; index < PIC_LIST_ENTRIES; index++) {
		pic[PIC_L0_AT + index] = PIC_LIST_NONE;
		pic[PIC_L1_AT + index] = PIC_LIST_NONE;
	}

	/* A P picture's one reference and its distance. */
	if (!picture->idr) {
		pic[PIC_L0_AT] = 0U;
		pic[PIC_DISTANCE_L0_AT] = (uint8_t)picture->poc_distance;
	}

	/* The slice header's numbers. */
	nvrtx_put32(pic, PIC_RESIDUAL_CAP_AT, picture->bitstream_bytes);
	records_put16(pic, PIC_FRAME_NUM_AT, picture->frame_num);
	records_put16(pic, PIC_POC_LSB_AT, picture->poc_lsb);
	records_put16(pic, PIC_IDR_ID_AT, picture->idr_pic_id);

	/* The controls after the structure, one of each, and the buffers' sizes. */
	nvrtx_put32(pic, PIC_SLICE_OFFSET_AT, NVRTX_NVENC_H264_SLICE_AT);
	nvrtx_put32(pic, PIC_ME_OFFSET_AT, NVRTX_NVENC_H264_ME_AT);
	nvrtx_put32(pic, PIC_MD_OFFSET_AT, NVRTX_NVENC_H264_MD_AT);
	nvrtx_put32(pic, PIC_QUANT_OFFSET_AT, NVRTX_NVENC_H264_QUANT_AT);
	nvrtx_put32(pic, PIC_HISTORY_AT, picture->history_bytes);
	nvrtx_put32(pic, PIC_BITSTREAM_AT, picture->bitstream_bytes);

	/* The picture word: IDR or P, a reference or not, slices from the slice control. */
	word = PIC_STATIC_SLICES;
	if (picture->idr) {
		word |= PIC_TYPE_IDR << PIC_TYPE_SHIFT;
	} else {
		word |= PIC_TYPE_P << PIC_TYPE_SHIFT;
	}
	if (picture->reference)
		word |= PIC_REFERENCE;
	nvrtx_put32(pic, PIC_WORD_AT, word);

	/* The codec field. */
	nvrtx_put32(pic, PIC_CODEC_WORD_AT, (picture->codec_field & PIC_CODEC_MASK) << PIC_CODEC_SHIFT);

	/* The controls the picture control points at. */
	records_h264_controls(setup, picture);
}

/* Writes an incrementing method header on the encoder's subchannel and returns where its data goes. */
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
	    (NVRTX_NVENC_SUBCHANNEL << RECORDS_METHOD_SUBCHANNEL_SHIFT) |
	    (method >> 2);
	nvrtx_put32(push, at, header);

	/* Succeeded: the data follows the header. */
	return at + 4U;
}

/* Writes one of the encoder's address methods (the address's upper 32 of 40 bits) and returns where the next method goes. */
static size_t
records_address(
	uint8_t *push,
	size_t at,
	uint32_t method,
	uint64_t address)
{
	/* The method and the address in 256-byte units. */
	at = records_method(push, at, method, 1U);
	nvrtx_put32(push, at, (uint32_t)(address >> RECORDS_ADDRESS_SHIFT));

	/* Succeeded: the next method follows the address. */
	return at + 4U;
}

/*
 * Writes one surface of a setup: the picture's size less one, the planes'
 * pitches, the luma plane at its method's address and the chroma plane
 * chroma_units 256-byte units after its own, semi-planar NV12 in 16x16
 * tiles.
 */
static void
records_surface(
	uint8_t *surface,
	const struct nvrtx_nvenc_h264_picture *picture,
	uint32_t chroma_units)
{
	/* The size and the pitches. */
	records_put16(surface, SURFACE_WIDTH_AT, picture->width - 1U);
	records_put16(surface, SURFACE_HEIGHT_AT, picture->height - 1U);
	records_put16(surface, SURFACE_PITCH_AT, picture->luma_pitch);
	records_put16(surface, SURFACE_CHROMA_PITCH_AT, picture->chroma_pitch);

	/* The planes' offsets and the tiled layout. */
	nvrtx_put32(surface, SURFACE_LUMA_AT, 0U);
	nvrtx_put32(surface, SURFACE_CHROMA_AT, chroma_units);
	nvrtx_put32(surface, SURFACE_LAYOUT_AT, SURFACE_TILED);
}

/*
 * Writes the controls after an H.264 setup's structure: the slice of the
 * whole picture at the picture's QP, the motion search from the spatial
 * neighbours and the zero vector with every point of the first pattern
 * and quarter-pel refinement, the mode decision over the Main profile's
 * intra modes and L0 partitions with P_Skip, and the quantization control
 * left all zero (no coefficient thresholds, no dead zones).
 */
static void
records_h264_controls(
	uint8_t *setup,
	const struct nvrtx_nvenc_h264_picture *picture)
{
	uint8_t *slice;
	uint8_t *me;
	uint8_t *md;
	uint32_t macroblocks;
	uint32_t word;
	uint32_t index;

	/* The slice: every macroblock, at the picture's QP. */
	slice = setup + NVRTX_NVENC_H264_SLICE_AT;
	macroblocks = ((picture->width + 15U) / 16U) * ((picture->height + 15U) / 16U);
	word = macroblocks | ((picture->qp & SLICE_QP_MASK) << SLICE_QP_SHIFT);
	nvrtx_put32(slice, SLICE_MB_AT, word);

	/* The slice QP's range, and an IDR picture all intra. */
	word = RC_QP_MAX << SLICE_QP_MAX_SHIFT;
	if (picture->idr)
		word |= SLICE_INTRA;
	nvrtx_put32(slice, SLICE_RANGE_AT, word);

	/* The motion search: quarter-pel, from the spatial neighbours and the zero vector. */
	me = setup + NVRTX_NVENC_H264_ME_AT;
	nvrtx_put32(me, ME_WORD_AT, ME_QUARTER_PEL);
	nvrtx_put32(me, ME_SOURCES_AT, ME_SPATIAL);
	nvrtx_put32(me, ME_CONSTANT_AT, ME_CONSTANT);
	nvrtx_put32(me, ME_SHAPE_AT, ME_SHAPE_ALL);
	nvrtx_put32(me, ME_SHAPE_AT + 4U, ME_SHAPE_ALL);

	/* The hint types in their order, each type once. */
	word = 0U;
	for (index = 0U; index < ME_HINT_TYPES; index++)
		word |= index << (ME_HINT_SHIFT + index * ME_HINT_WIDTH);
	nvrtx_put32(me, ME_HINTS_AT, word);

	/* The mode decision. */
	md = setup + NVRTX_NVENC_H264_MD_AT;
	nvrtx_put32(md, MD_INTRA_AT, MD_INTRA);
	nvrtx_put32(md, MD_INTER_AT, MD_INTER);
	nvrtx_put32(md, MD_DECISION_AT, MD_DECISION);
}

/* Writes a 16-bit field, lowest byte first. */
static void
records_put16(
	uint8_t *bytes,
	size_t at,
	uint32_t value)
{
	/* The field's two bytes. */
	bytes[at] = (uint8_t)value;
	bytes[at + 1U] = (uint8_t)(value >> 8);
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
