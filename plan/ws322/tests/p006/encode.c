/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The libvulkan side of ps-host-test's round trip: libvulkan's own video
 * encoders (userland/desktop/libvulkan/video.c, included so its static
 * encoders can be called) write a parameters create and a decode.  The
 * context and object calls the encoders do not reach are stubs.
 */

#include "userland/desktop/libvulkan/video.c"

#include <stdlib.h>

#include "plan/ws322/tests/p006/ps-test.h"

static size_t take(struct vulkan_writer *writer, uint8_t *out, size_t capacity);

/* A handle stands for its own identity on the wire. */
struct vulkan_object *
vulkan_nondispatchable_object(
	uint64_t handle)
{
	return (struct vulkan_object *)(uintptr_t)handle;
}

/* The wire id of an object is the handle it stands for. */
uint64_t
vulkan_object_wire_id(
	const struct vulkan_object *object)
{
	return (uint64_t)(uintptr_t)object;
}

/* The encoders' errors stay in their writers. */
void
vulkan_context_error(
	struct vulkan_context *context,
	VkResult error)
{
	(void)context;
	(void)error;
}

/* No call under test reaches an executor. */
VkResult
vulkan_context_execute(
	struct vulkan_context *context,
	const struct vulkan_writer *writer,
	size_t reply_capacity,
	struct vulkan_reader *reader)
{
	(void)context;
	(void)writer;
	(void)reply_capacity;
	(void)reader;
	abort();
}

/* Encodes a parameters create of a session (0x77) carrying the add record. */
size_t
test_encode_parameters(
	const void *add,
	uint8_t *out,
	size_t capacity)
{
	VkVideoDecodeH265SessionParametersCreateInfoKHR h265;
	VkVideoSessionParametersCreateInfoKHR create;
	struct vulkan_writer writer;

	/* The create record with room for the add's sets, then the create info. */
	memset(&h265, 0, sizeof(h265));
	h265.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_SESSION_PARAMETERS_CREATE_INFO_KHR;
	h265.maxStdVPSCount = 16U;
	h265.maxStdSPSCount = 16U;
	h265.maxStdPPSCount = 64U;
	h265.pParametersAddInfo = add;
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR;
	create.pNext = &h265;
	create.videoSession = (VkVideoSessionKHR)(uintptr_t)0x77U;

	/* The structure as the library writes it. */
	vulkan_writer_init(&writer);
	video_encode_struct(&writer, (const VkBaseInStructure *)&create);
	return take(&writer, out, capacity);
}

/* Encodes a decode with its present word. */
size_t
test_encode_decode(
	const void *decode,
	uint8_t *out,
	size_t capacity)
{
	struct vulkan_writer writer;

	/* The present word and the structure. */
	vulkan_writer_init(&writer);
	vulkan_write_u64(&writer, 1);
	video_encode_struct(&writer, (const VkBaseInStructure *)decode);
	return take(&writer, out, capacity);
}

/* Takes a writer's bytes when it succeeded and they fit, then frees it. */
static size_t
take(
	struct vulkan_writer *writer,
	uint8_t *out,
	size_t capacity)
{
	size_t bytes;

	bytes = 0U;
	if (writer->error == VK_SUCCESS && writer->bytes <= capacity) {
		memcpy(out, writer->data, writer->bytes);
		bytes = writer->bytes;
	}
	vulkan_writer_finish(writer);
	return bytes;
}
