/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The render node's view of a video engine: its registration, and what a
 * session offers of the operations it read when it opened.  Nothing here
 * touches the core, the engine or memory; the render node (render.c,
 * render-runtime.c) and the executor (vk-device.c, vk-image.c,
 * vk-query.c) call it, and the host check builds it unchanged.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libc/vulkan/vulkan_video.h>
#include <uapi/errno.h>

#include "drivers/gpu/powervr/video-state.h"

/* The decode operations of the operations word. */
#define VIDEO_DECODES			(DRV_POWERVR_VIDEO_DECODE_H264 | DRV_POWERVR_VIDEO_DECODE_H265)

/* The usages an NV12 image may add once the engine decodes: a decode's output and its reference. */
#define VIDEO_DECODE_USAGES		(VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR)

/*
 * The queue flag of a family that encodes video (the header the kernel
 * reads names it only among the beta extensions).
 *
 * Value referenced: Vulkan 1.3.269 vk.xml (VkQueueFlagBits,
 * VK_QUEUE_VIDEO_ENCODE_BIT_KHR, bit 6).
 */
#define VIDEO_QUEUE_ENCODE		0x40U

/* The format features NV12 adds once the engine decodes. */
#define VIDEO_DECODE_FEATURES		(VK_FORMAT_FEATURE_VIDEO_DECODE_OUTPUT_BIT_KHR | VK_FORMAT_FEATURE_VIDEO_DECODE_DPB_BIT_KHR)

/*
 * Whether an engine has claimed the registration (1), set once by the
 * first drv_powervr_video_register and never cleared, so a second
 * registration is refused before it writes anything.
 */
static int video_claimed;

/*
 * The registered engine's table and the engine it is called with.
 *
 * Both are written once, by the registration that claimed it: the engine
 * first, then the table with release order, so a reader that sees the
 * table (acquire) sees its engine.  A null table means no engine is
 * registered yet; they never change back, and live as long as the kernel.
 */
static const struct drv_powervr_video_ops *video_ops;
static void *video_engine;

/*
 * Registers the board's one video engine with the render node.
 */
int
drv_powervr_video_register(
	const struct drv_powervr_video_ops *ops,
	void *engine)
{
	int unclaimed;
	bool claimed;

	/* A table that can report what the engine does. */
	if (ops == NULL || ops->operations == NULL)
		return EINVAL;

	/* Claims the registration; a second engine is refused and changes nothing. */
	unclaimed = 0;
	claimed = __atomic_compare_exchange_n(&video_claimed, &unclaimed, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
	if (!claimed)
		return EBUSY;

	/* The engine, then the table that publishes it: a session reads the table and only then its engine. */
	__atomic_store_n(&video_engine, engine, __ATOMIC_RELAXED);
	__atomic_store_n(&video_ops, ops, __ATOMIC_RELEASE);

	/* Succeeded: sessions opened from now on offer the engine's operations. */
	return 0;
}

/*
 * Reports the codec operations of the registered engine
 * (DRV_POWERVR_VIDEO_*), or 0 when no engine is registered.  A session
 * calls it once, when it opens, and keeps the answer.
 */
uint32_t
powervr_video_operations(
	void)
{
	const struct drv_powervr_video_ops *ops;
	void *engine;
	uint32_t operations;

	/* No engine, no video. */
	ops = __atomic_load_n(&video_ops, __ATOMIC_ACQUIRE);
	if (ops == NULL)
		return 0;

	/* The engine's own answer, of the operations the render node knows. */
	engine = __atomic_load_n(&video_engine, __ATOMIC_RELAXED);
	operations = ops->operations(engine);
	operations &= DRV_POWERVR_VIDEO_DECODE_H264 | DRV_POWERVR_VIDEO_DECODE_H265 | DRV_POWERVR_VIDEO_ENCODE_H265;

	/* Succeeded: the engine's operations. */
	return operations;
}

/*
 * Reports the registered engine's operations table and the engine, or
 * NULL when no engine is registered.
 */
const struct drv_powervr_video_ops *
powervr_video_engine(
	void **engine)
{
	const struct drv_powervr_video_ops *ops;

	/* No engine. */
	ops = __atomic_load_n(&video_ops, __ATOMIC_ACQUIRE);
	if (ops == NULL)
		return NULL;

	/* Succeeded: the table, and the engine it publishes. */
	*engine = __atomic_load_n(&video_engine, __ATOMIC_RELAXED);
	return ops;
}

/*
 * Reports the native capset's flags for a session's operations: the
 * H.264 and H.265 decode bits libvulkan reads; 0 when the session offers
 * nothing libvulkan can use, and the node then reports the plain capset.
 *
 * Value referenced: zedBSD userland/desktop/libvulkan/context.c
 * (VULKAN_NATIVE_CAPSET_VIDEO_DECODE_H264 and _H265, bits 0 and 2 of the
 * word at 172).
 */
uint32_t
powervr_video_capset_flags(
	uint32_t operations)
{
	uint32_t flags;

	/* Each decode's bit; the encode has none yet. */
	flags = 0;
	if ((operations & DRV_POWERVR_VIDEO_DECODE_H264) != 0)
		flags |= POWERVR_VIDEO_CAPSET_DECODE_H264;
	if ((operations & DRV_POWERVR_VIDEO_DECODE_H265) != 0)
		flags |= POWERVR_VIDEO_CAPSET_DECODE_H265;

	/* Succeeded: the decode bits. */
	return flags;
}

/* Reports the queue flags the session's one family adds for its operations. */
uint32_t
powervr_video_queue_flags(
	uint32_t operations)
{
	uint32_t flags;

	/* A decode flag for any decode, an encode flag for the encode. */
	flags = 0;
	if ((operations & VIDEO_DECODES) != 0)
		flags |= VK_QUEUE_VIDEO_DECODE_BIT_KHR;
	if ((operations & DRV_POWERVR_VIDEO_ENCODE_H265) != 0)
		flags |= VIDEO_QUEUE_ENCODE;

	/* Succeeded: the family's video flags. */
	return flags;
}

/* Reports the Vulkan codec operations (VkVideoCodecOperationFlagsKHR) of the session's operations. */
uint32_t
powervr_video_codec_operations(
	uint32_t operations)
{
	uint32_t codecs;

	/* Each operation's own bit. */
	codecs = 0;
	if ((operations & DRV_POWERVR_VIDEO_DECODE_H264) != 0)
		codecs |= VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
	if ((operations & DRV_POWERVR_VIDEO_DECODE_H265) != 0)
		codecs |= VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR;
	if ((operations & DRV_POWERVR_VIDEO_ENCODE_H265) != 0)
		codecs |= VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR;

	/* Succeeded: the family's codec operations. */
	return codecs;
}

/* Reports the format features NV12 adds for the session's operations: a decode's output and reference. */
uint32_t
powervr_video_format_features(
	uint32_t operations)
{
	/* Nothing without a decode. */
	if ((operations & VIDEO_DECODES) == 0)
		return 0;

	/* Succeeded: the decode features. */
	return VIDEO_DECODE_FEATURES;
}

/* Reports the usages an NV12 image may add for the session's operations: a decode's output and reference. */
uint32_t
powervr_video_image_usages(
	uint32_t operations)
{
	/* Nothing without a decode. */
	if ((operations & VIDEO_DECODES) == 0)
		return 0;

	/* Succeeded: the decode usages. */
	return VIDEO_DECODE_USAGES;
}

/*
 * Writes one result status query's record as vkGetQueryPoolResults lays
 * it out: the status (VkQueryResultStatusKHR, sign-extended to 64 bits
 * with VK_QUERY_RESULT_64_BIT), then with
 * VK_QUERY_RESULT_WITH_AVAILABILITY_BIT whether the query is available,
 * each a word of 4 or 8 bytes.  Returns the record's bytes.
 */
uint32_t
powervr_video_query_record(
	int32_t status,
	uint32_t available,
	uint32_t flags,
	uint8_t record[POWERVR_VIDEO_QUERY_RECORD_MAX])
{
	uint64_t words[2];
	uint32_t width;
	uint32_t bytes;
	uint32_t index;

	/* Words of four bytes, or eight with the 64-bit flag. */
	width = 4;
	if ((flags & VK_QUERY_RESULT_64_BIT) != 0)
		width = 8;

	/* The status, then the availability when asked for. */
	words[0] = (uint64_t)(int64_t)status;
	words[1] = available != 0 ? 1U : 0U;
	bytes = width;
	if ((flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != 0)
		bytes += width;

	/* Each word's bytes, least significant first, as the host reads them. */
	for (index = 0; index < bytes; index++)
		record[index] = (uint8_t)(words[index / width] >> (8U * (index % width)));

	/* Succeeded: the record's bytes. */
	return bytes;
}
