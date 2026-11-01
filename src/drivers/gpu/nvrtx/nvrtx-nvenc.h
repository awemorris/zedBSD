/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The video encoder (NVENC) of the nvrtx driver.
 *
 * The encoder runs on a channel of its own (nvenc.c) in the work channel's
 * address space, beside the video decoder's, so the memory the executor's
 * users bind is at the same GPU address for the encoder as for the
 * graphics engine.  The firmware the encoder runs is the GSP's: the driver
 * only has the resource manager make the channel, its engine context and
 * the encoder's object.  The parts here touch no hardware (a host test
 * runs them).
 */

#ifndef DRIVERS_GPU_NVRTX_NVENC_H
#define DRIVERS_GPU_NVRTX_NVENC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "drivers/gpu/nvrtx/nvrtx-private.h"

struct nvrtx_gpu;

/*
 * The encoder's classes: Turing's (TU10x and TU11x), Ampere's GA10x and
 * Ada's.
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/nvidia/generated/g_gpu_class_list.c (the NVENC classes of each
 * chip) and src/common/sdk/nvidia/inc/class/clc4b7.h
 * (NVC4B7_VIDEO_ENCODER); NVIDIA open-gpu-doc
 * classes/video/clc7b7.h and clc9b7.h (NVC7B7_VIDEO_ENCODER,
 * NVC9B7_VIDEO_ENCODER).
 */
#define NVRTX_NVENC_CLASS_TURING	0xc4b7U
#define NVRTX_NVENC_CLASS_AMPERE	0xc7b7U
#define NVRTX_NVENC_CLASS_ADA		0xc9b7U

/*
 * The first encoder's engine type as the channel's allocation, its bind
 * and the promote of its context name it (the device information table
 * numbers it otherwise, objects.c).
 *
 * Value referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/class/cl2080_notification.h
 * (NV2080_ENGINE_TYPE_NVENC0, the same as NV2080_ENGINE_TYPE_MSENC).
 */
#define NVRTX_ENGINE_NVENC0		0x1bU

/* The subchannel the encoder's class is bound to on its channel. */
#define NVRTX_NVENC_SUBCHANNEL		4U

/*
 * The bytes of the encoder object's parameters (its size, whether other
 * channels may share the encoder, and which encoder) and of the promote
 * of its engine context.
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/nvos.h (NV_MSENC_ALLOCATION_PARAMETERS) and
 * src/common/sdk/nvidia/inc/ctrl/ctrl2080/ctrl2080gpu.h
 * (NV2080_CTRL_GPU_PROMOTE_CTX_PARAMS).
 */
#define NVRTX_NVENC_ALLOC_PARAMS_BYTES	12U
#define NVRTX_NVENC_PROMOTE_BYTES	560U

/* The most bytes the push buffers of nvrtx_nvenc_push_object() and nvrtx_nvenc_push_release() take. */
#define NVRTX_NVENC_OBJECT_PUSH_BYTES	16U
#define NVRTX_NVENC_RELEASE_PUSH_BYTES	24U

/*
 * The bytes of one H.264 picture's setup: the picture setup structure
 * itself (512 bytes), then its one slice control (128), its one motion
 * search control (192), its one mode decision control (128) and its one
 * quantization control (192), each at a 256-byte boundary.
 */
#define NVRTX_NVENC_H264_SETUP_BYTES	1536U

/* Where the setup's controls start, from the setup's start. */
#define NVRTX_NVENC_H264_SLICE_AT	512U
#define NVRTX_NVENC_H264_ME_AT		768U
#define NVRTX_NVENC_H264_MD_AT		1024U
#define NVRTX_NVENC_H264_QUANT_AT	1280U

/*
 * The encoder's status of one picture, its bytes and the words the driver
 * reads: the picture's number, the error word (two bits of the engine's,
 * thirty of the firmware's), the picture's size in bits and where the
 * slice data starts in the bitstream buffer.
 *
 * Values referenced: NVIDIA open-gpu-doc classes/video/nvenc_drv.h
 * (nvenc_pic_stat_s: picture_index 0, error_status and ucode_error_status
 * 4, total_bit_count 8, bitstream_start_pos 32).
 */
#define NVRTX_NVENC_STATUS_BYTES	128U
#define NVRTX_NVENC_STATUS_INDEX_AT	0U
#define NVRTX_NVENC_STATUS_ERROR_AT	4U
#define NVRTX_NVENC_STATUS_BITS_AT	8U
#define NVRTX_NVENC_STATUS_START_AT	32U

/* The most bytes the push buffer of nvrtx_nvenc_push_encode() takes. */
#define NVRTX_NVENC_ENCODE_PUSH_BYTES	256U

/*
 * The control parameters word: H.264, the reconstructed picture written
 * even when the picture is not a reference, the firmware's errors
 * returned to the channel instead of a reset of the engine, and the
 * constant-QP rate control (0 in bits 31:24).
 *
 * Values referenced: NVIDIA open-gpu-doc classes/video/clc5b7.h
 * (NVC5B7_SET_CONTROL_PARAMS_CODEC_TYPE_H264, _FORCE_OUT_PIC,
 * _RCMODE_CONSTQP) and classes/video/nvenc_drv.h (SetControlParams'
 * DEBUG_MODE, bit 16).
 */
#define NVRTX_NVENC_CONTROL_H264	0x3U
#define NVRTX_NVENC_CONTROL_OUT_PIC	(1U << 8)
#define NVRTX_NVENC_CONTROL_DEBUG	(1U << 16)

/*
 * One H.264 picture to encode, as the setup takes it: one progressive
 * picture of one slice, NV12 in 16x16 tiles, at a constant QP, Main
 * profile with CAVLC, picture order count type 0.
 *
 * width and height are the picture's size in pixels; the planes' pitches
 * are in bytes.  chroma_units is where the chroma plane starts in 256-byte
 * units from the address its own method gives (0 when the method points
 * at the plane itself).  idr makes an IDR picture (else a P picture whose
 * one reference is in slot 0, poc_distance before it); reference marks the
 * picture as one later pictures may refer to.  history_bytes and
 * bitstream_bytes are the sizes of the buffers the encoder gets.
 * codec_field is the picture control's codec field, whose values the
 * description leaves undefined.
 */
struct nvrtx_nvenc_h264_picture {
	uint32_t width;
	uint32_t height;
	uint32_t luma_pitch;
	uint32_t chroma_pitch;
	uint32_t chroma_units;
	uint32_t profile_idc;
	uint32_t level_idc;
	uint32_t log2_max_frame_num_minus4;
	uint32_t log2_max_poc_lsb_minus4;
	uint32_t frame_num;
	uint32_t poc_lsb;
	uint32_t idr_pic_id;
	uint32_t poc_distance;
	uint32_t qp;
	uint32_t history_bytes;
	uint32_t bitstream_bytes;
	uint32_t codec_field;
	bool idr;
	bool reference;
};

/*
 * The GPU addresses (in the work channel's space, 256-byte aligned) of one
 * encode: the setup, the status, the bitstream, the history, the rate
 * control's persistent state (0 for none), the co-located data the
 * picture writes (0 for none), the input picture's planes, the
 * reconstructed picture's planes, and the reference's planes (0 for none:
 * an IDR picture).  serial is the picture's number in the session and
 * control the control parameters word.
 */
struct nvrtx_nvenc_frame {
	uint64_t setup;
	uint64_t status;
	uint64_t bitstream;
	uint64_t history;
	uint64_t rc_process;
	uint64_t coloc_out;
	uint64_t input_luma;
	uint64_t input_chroma;
	uint64_t output_luma;
	uint64_t output_chroma;
	uint64_t reference_luma;
	uint64_t reference_chroma;
	uint32_t serial;
	uint32_t control;
};

uint32_t nvrtx_nvenc_class(enum nvrtx_family family);
uint32_t nvrtx_nvenc_magic(uint32_t nvenc_class);
size_t nvrtx_nvenc_push_object(uint8_t *push, uint32_t nvenc_class);
size_t nvrtx_nvenc_push_release(uint8_t *push, uint64_t semaphore, uint32_t value);
size_t nvrtx_nvenc_push_encode(uint8_t *push, const struct nvrtx_nvenc_frame *frame);
void nvrtx_nvenc_alloc_params(uint8_t *params);
void nvrtx_nvenc_promote_params(uint8_t *params, uint32_t client, uint32_t chid, uint32_t channel, uint64_t va, uint64_t size);
void nvrtx_nvenc_h264_setup(uint8_t *setup, uint32_t magic, const struct nvrtx_nvenc_h264_picture *picture);

int nvrtx_nvenc_test(struct nvrtx_gpu *gpu);

int nvrtx_nvenc_start(struct nvrtx_gpu *gpu);
void nvrtx_nvenc_stop(struct nvrtx_gpu *gpu);
void nvrtx_nvenc_release(struct nvrtx_gpu *gpu);
bool nvrtx_nvenc_ready(const struct nvrtx_gpu *gpu);
uint32_t nvrtx_nvenc_class_of(const struct nvrtx_gpu *gpu);
int nvrtx_nvenc_run(struct nvrtx_gpu *gpu, uint64_t push_va, size_t length, uint32_t timeout_us);
uint32_t nvrtx_nvenc_notices(struct nvrtx_gpu *gpu);
void nvrtx_nvenc_report_notices(struct nvrtx_gpu *gpu, uint32_t before, const char *stage);

#endif
