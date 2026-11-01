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

uint32_t nvrtx_nvenc_class(enum nvrtx_family family);
size_t nvrtx_nvenc_push_object(uint8_t *push, uint32_t nvenc_class);
size_t nvrtx_nvenc_push_release(uint8_t *push, uint64_t semaphore, uint32_t value);
void nvrtx_nvenc_alloc_params(uint8_t *params);
void nvrtx_nvenc_promote_params(uint8_t *params, uint32_t client, uint32_t chid, uint32_t channel, uint64_t va, uint64_t size);

int nvrtx_nvenc_start(struct nvrtx_gpu *gpu);
void nvrtx_nvenc_stop(struct nvrtx_gpu *gpu);
void nvrtx_nvenc_release(struct nvrtx_gpu *gpu);
bool nvrtx_nvenc_ready(const struct nvrtx_gpu *gpu);
uint32_t nvrtx_nvenc_class_of(const struct nvrtx_gpu *gpu);
int nvrtx_nvenc_run(struct nvrtx_gpu *gpu, uint64_t push_va, size_t length, uint32_t timeout_us);

#endif
