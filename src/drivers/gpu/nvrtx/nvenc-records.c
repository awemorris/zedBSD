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

static size_t records_method(uint8_t *push, size_t at, uint32_t method, uint32_t count);
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
